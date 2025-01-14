// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::diagnostics::{
        constants::{COMPONENT_CPU_MAX_SAMPLES, CPU_SAMPLE_PERIOD},
        measurement::{Measurement, MeasurementsQueue},
        runtime_stats_source::RuntimeStatsSource,
    },
    fuchsia_inspect::{self as inspect, HistogramProperty, UintLinearHistogramProperty},
    fuchsia_zircon as zx,
    fuchsia_zircon_sys::{self as zx_sys, zx_system_get_num_cpus},
    futures::{future::BoxFuture, lock::Mutex, FutureExt},
    injectable_time::{MonotonicTime, TimeSource},
    lazy_static::lazy_static,
    moniker::ExtendedMoniker,
    std::sync::Weak,
};

lazy_static! {
    static ref SAMPLES: inspect::StringReference<'static> = "@samples".into();
    static ref SAMPLE_INDEXES: Vec<inspect::StringReference<'static>> =
        (0..COMPONENT_CPU_MAX_SAMPLES).map(|x| x.to_string().into()).collect();
}

pub(crate) fn create_cpu_histogram(
    node: &inspect::Node,
    moniker: &ExtendedMoniker,
) -> inspect::UintLinearHistogramProperty {
    node.create_uint_linear_histogram(
        moniker.to_string_without_instances(),
        inspect::LinearHistogramParams { floor: 1, step_size: 1, buckets: 99 },
    )
}

fn num_cpus() -> i64 {
    // zx_system_get_num_cpus() is FFI to C++. It simply returns a value from a static struct
    // so it should always be safe to call.
    (unsafe { zx_system_get_num_cpus() }) as i64
}

#[derive(Debug)]
pub struct TaskInfo<T: RuntimeStatsSource, U: TimeSource> {
    koid: zx_sys::zx_koid_t,
    task: T,
    time_source: U,
    pub has_parent_task: bool,
    measurements: MeasurementsQueue,
    histogram: Option<UintLinearHistogramProperty>,
    previous_cpu: zx::Duration,
    previous_histogram_timestamp: i64,
    cpu_cores: i64,
    sample_period: std::time::Duration,
    children: Vec<Weak<Mutex<TaskInfo<T, MonotonicTime>>>>,
    should_drop_old_measurements: bool,
    post_invalidation_measurements: usize,
}

impl<T: RuntimeStatsSource + Send + Sync> TaskInfo<T, MonotonicTime> {
    /// Creates a new `TaskInfo` from the given cpu stats provider.
    // Due to https://github.com/rust-lang/rust/issues/50133 we cannot just derive TryFrom on a
    // generic type given a collision with the blanket implementation.
    pub fn try_from(
        task: T,
        histogram: Option<UintLinearHistogramProperty>,
    ) -> Result<Self, zx::Status> {
        Self::try_from_internal(
            task,
            histogram,
            MonotonicTime::new(),
            CPU_SAMPLE_PERIOD,
            num_cpus(),
        )
    }
}

impl<T: RuntimeStatsSource + Send + Sync, U: TimeSource + std::marker::Send> TaskInfo<T, U> {
    // Injects a couple of test dependencies
    fn try_from_internal(
        task: T,
        histogram: Option<UintLinearHistogramProperty>,
        time_source: U,
        sample_period: std::time::Duration,
        cpu_cores: i64,
    ) -> Result<Self, zx::Status> {
        Ok(Self {
            koid: task.koid()?,
            task,
            has_parent_task: false,
            measurements: MeasurementsQueue::new(),
            children: vec![],
            should_drop_old_measurements: false,
            cpu_cores,
            sample_period,
            post_invalidation_measurements: 0,
            histogram,
            previous_cpu: zx::Duration::from_nanos(0),
            previous_histogram_timestamp: time_source.now(),
            time_source,
        })
    }

    /// Take a new measurement. If the handle of this task is invalid, then it keeps track of how
    /// many measurements would have been done. When the maximum amount allowed is hit, then it
    /// drops the oldest measurement.
    pub async fn measure_if_no_parent(&mut self) -> Option<&Measurement> {
        // Tasks with a parent are measured by the parent as done right below in the internal
        // `measure_subtree`.
        if self.has_parent_task {
            return None;
        }
        self.measure_subtree().await
    }

    /// Adds a weak pointer to a task for which this task is the parent.
    pub fn add_child(&mut self, task: Weak<Mutex<TaskInfo<T, MonotonicTime>>>) {
        self.children.push(task);
    }

    fn measure_subtree<'a>(&'a mut self) -> BoxFuture<'a, Option<&'a Measurement>> {
        async move {
            if self.task.handle_is_invalid() {
                if self.should_drop_old_measurements {
                    self.measurements.pop_front();
                } else {
                    self.post_invalidation_measurements += 1;
                    self.should_drop_old_measurements = self.post_invalidation_measurements
                        + self.measurements.len()
                        >= COMPONENT_CPU_MAX_SAMPLES;
                }
                return None;
            }
            if let Ok(runtime_info) = self.task.get_runtime_info().await {
                let mut measurement = Measurement::from_runtime_info(
                    runtime_info,
                    zx::Time::from_nanos(self.time_source.now()),
                );
                // Subtract all child measurements.
                let mut alive_children = vec![];
                while let Some(weak_child) = self.children.pop() {
                    if let Some(child) = weak_child.upgrade() {
                        let mut child_guard = child.lock().await;
                        if let Some(child_measurement) = child_guard.measure_subtree().await {
                            measurement -= child_measurement;
                        }
                        if child_guard.is_alive() {
                            alive_children.push(weak_child);
                        }
                    }
                }
                self.children = alive_children;

                let current_cpu = *measurement.cpu_time();
                self.add_to_histogram(current_cpu - self.previous_cpu, *measurement.timestamp());
                self.previous_cpu = current_cpu;
                self.measurements.insert(measurement);
                return self.measurements.back();
            }
            None
        }
        .boxed()
    }

    // Add a measurement to this task's histogram.
    fn add_to_histogram(&mut self, cpu_time_delta: zx::Duration, timestamp: zx::Time) {
        if let Some(histogram) = &self.histogram {
            let time_value: i64 = timestamp.into_nanos();
            let elapsed_time = time_value - self.previous_histogram_timestamp;
            self.previous_histogram_timestamp = time_value;
            if elapsed_time < ((self.sample_period.as_nanos() as i64) * 9 / 10) {
                return;
            }
            let available_core_time = elapsed_time * self.cpu_cores;
            if available_core_time != 0 {
                // Multiply by 100 to get percent. Add available_core_time-1 to compute ceil().
                let cpu_numerator =
                    (cpu_time_delta.into_nanos() as i64) * 100 + available_core_time - 1;
                histogram.insert((cpu_numerator / available_core_time) as u64);
            }
        }
    }

    /// A task is alive when:
    /// - Its handle is valid, or
    /// - There's at least one measurement saved.
    pub fn is_alive(&self) -> bool {
        return !self.task.handle_is_invalid() || !self.measurements.is_empty();
    }

    /// Writes the task measurements under the given inspect node `parent`.
    pub fn record_to_node(&self, parent: &inspect::Node) {
        let node = parent.create_child(self.koid.to_string());
        let samples = node.create_child(&*SAMPLES);
        for (i, measurement) in self.measurements.iter().enumerate() {
            let child = samples.create_child(&SAMPLE_INDEXES[i]);
            measurement.record_to_node(&child);
            samples.record(child);
        }
        node.record(samples);
        parent.record(node);
    }

    pub fn koid(&self) -> zx_sys::zx_koid_t {
        self.koid
    }

    #[cfg(test)]
    pub fn total_measurements(&self) -> usize {
        self.measurements.len()
    }

    #[cfg(test)]
    pub fn task_mut(&mut self) -> &mut T {
        &mut self.task
    }
}

#[cfg(test)]
mod tests {
    use inspect::testing::DiagnosticsHierarchyGetter;

    use {
        super::*,
        crate::diagnostics::testing::FakeTask,
        diagnostics_hierarchy::ArrayContent,
        fuchsia_inspect::testing::{assert_data_tree, AnyProperty},
        injectable_time::FakeTime,
        std::sync::Arc,
    };

    #[fuchsia::test]
    async fn rotates_measurements_per_task() {
        // Set up test
        let mut task: TaskInfo<FakeTask, _> =
            TaskInfo::try_from(FakeTask::default(), None /* histogram */).unwrap();
        assert!(task.is_alive());

        // Take three measurements.
        task.measure_if_no_parent().await;
        assert_eq!(task.measurements.len(), 1);
        task.measure_if_no_parent().await;
        assert_eq!(task.measurements.len(), 2);
        task.measure_if_no_parent().await;
        assert!(task.is_alive());
        assert_eq!(task.measurements.len(), 3);

        // Invalidate the handle
        task.task.invalid_handle = true;

        // Allow MAX-N (N=3 here) measurements to be taken until we start dropping.
        for i in 3..COMPONENT_CPU_MAX_SAMPLES {
            task.measure_if_no_parent().await;
            assert_eq!(task.measurements.len(), 3);
            assert_eq!(task.post_invalidation_measurements, i - 2);
        }

        task.measure_if_no_parent().await; // 1 dropped, 2 left
        assert!(task.is_alive());
        assert_eq!(task.measurements.len(), 2);
        task.measure_if_no_parent().await; // 2 dropped, 1 left
        assert!(task.is_alive());
        assert_eq!(task.measurements.len(), 1);

        // Take one last measure.
        task.measure_if_no_parent().await; // 3 dropped, 0 left
        assert!(!task.is_alive());
        assert_eq!(task.measurements.len(), 0);
    }

    #[fuchsia::test]
    async fn write_inspect() {
        let mut task = TaskInfo::try_from(
            FakeTask::new(
                1,
                vec![
                    zx::TaskRuntimeInfo {
                        cpu_time: 2,
                        queue_time: 4,
                        ..zx::TaskRuntimeInfo::default()
                    },
                    zx::TaskRuntimeInfo {
                        cpu_time: 6,
                        queue_time: 8,
                        ..zx::TaskRuntimeInfo::default()
                    },
                ],
            ),
            None, /* histogram */
        )
        .unwrap();

        task.measure_if_no_parent().await;
        task.measure_if_no_parent().await;

        let inspector = inspect::Inspector::new();
        task.record_to_node(inspector.root());
        assert_data_tree!(inspector, root: {
            "1": {
                "@samples": {
                    "0": {
                        cpu_time: 2i64,
                        queue_time: 4i64,
                        timestamp: AnyProperty,
                    },
                    "1": {
                        cpu_time: 6i64,
                        queue_time: 8i64,
                        timestamp: AnyProperty,
                    }
                }
            }
        });
    }

    #[fuchsia::test]
    async fn write_more_than_max_samples() {
        let inspector = inspect::Inspector::new();
        let mut task = TaskInfo::try_from(
            FakeTask::new(
                1,
                vec![
                    zx::TaskRuntimeInfo {
                        cpu_time: 2,
                        queue_time: 4,
                        ..zx::TaskRuntimeInfo::default()
                    },
                    zx::TaskRuntimeInfo {
                        cpu_time: 6,
                        queue_time: 8,
                        ..zx::TaskRuntimeInfo::default()
                    },
                ],
            ),
            None, /* histogram */
        )
        .unwrap();

        for _ in 0..(COMPONENT_CPU_MAX_SAMPLES + 10) {
            assert!(task.measure_if_no_parent().await.is_some());
        }

        assert_eq!(task.measurements.len(), COMPONENT_CPU_MAX_SAMPLES);
        task.record_to_node(inspector.root());

        let hierarchy = inspector.get_diagnostics_hierarchy();
        for top_level in &hierarchy.children {
            for i in 0..COMPONENT_CPU_MAX_SAMPLES {
                let index = i.to_string();
                let child =
                    hierarchy.get_child_by_path(&[&top_level.name, "@samples", &index]).unwrap();
                assert_eq!(child.name, index);
            }
        }
    }

    #[fuchsia::test]
    async fn measure_with_children() {
        let mut task = TaskInfo::try_from(
            FakeTask::new(
                1,
                vec![
                    zx::TaskRuntimeInfo {
                        cpu_time: 100,
                        queue_time: 200,
                        ..zx::TaskRuntimeInfo::default()
                    },
                    zx::TaskRuntimeInfo {
                        cpu_time: 300,
                        queue_time: 400,
                        ..zx::TaskRuntimeInfo::default()
                    },
                ],
            ),
            None, /* histogram */
        )
        .unwrap();

        let child_1 = Arc::new(Mutex::new(
            TaskInfo::try_from(
                FakeTask::new(
                    2,
                    vec![
                        zx::TaskRuntimeInfo {
                            cpu_time: 10,
                            queue_time: 20,
                            ..zx::TaskRuntimeInfo::default()
                        },
                        zx::TaskRuntimeInfo {
                            cpu_time: 30,
                            queue_time: 40,
                            ..zx::TaskRuntimeInfo::default()
                        },
                    ],
                ),
                None, /* histogram */
            )
            .unwrap(),
        ));

        let child_2 = Arc::new(Mutex::new(
            TaskInfo::try_from(
                FakeTask::new(
                    3,
                    vec![
                        zx::TaskRuntimeInfo {
                            cpu_time: 5,
                            queue_time: 2,
                            ..zx::TaskRuntimeInfo::default()
                        },
                        zx::TaskRuntimeInfo {
                            cpu_time: 15,
                            queue_time: 4,
                            ..zx::TaskRuntimeInfo::default()
                        },
                    ],
                ),
                None, /* histogram */
            )
            .unwrap(),
        ));

        task.add_child(Arc::downgrade(&child_1));
        task.add_child(Arc::downgrade(&child_2));

        {
            let measurement = task.measure_if_no_parent().await.unwrap();
            assert_eq!(measurement.cpu_time().into_nanos(), 100 - 10 - 5);
            assert_eq!(measurement.queue_time().into_nanos(), 200 - 20 - 2);
        }
        assert_eq!(child_1.lock().await.total_measurements(), 1);
        assert_eq!(child_2.lock().await.total_measurements(), 1);

        // Fake child 2 not being alive anymore. It should be removed.
        {
            let mut child_2_guard = child_2.lock().await;
            child_2_guard.task.invalid_handle = true;
            child_2_guard.measurements = MeasurementsQueue::new();
        }

        assert_eq!(task.children.len(), 2);
        {
            let measurement = task.measure_if_no_parent().await.unwrap();
            assert_eq!(measurement.cpu_time().into_nanos(), 300 - 30);
            assert_eq!(measurement.queue_time().into_nanos(), 400 - 40);
        }

        assert_eq!(task.children.len(), 1); // after measuring dead children are cleaned.
        assert_eq!(child_1.lock().await.total_measurements(), 2);
    }

    type BucketPairs = Vec<(i64, i64)>;

    use diagnostics_hierarchy::Property;

    // Returns a list of <bucket index, count> for buckets where count > 0.
    fn histogram_non_zero_values(inspector: &inspect::Inspector) -> BucketPairs {
        let mut output = vec![];
        let hierarchy = inspector.get_diagnostics_hierarchy();
        let histogram = hierarchy.get_property_by_path(&["/foo"]).unwrap();
        if let Property::UintArray(_, data) = histogram {
            if let ArrayContent::Buckets(buckets) = data {
                for bucket in buckets {
                    if bucket.count > 0 {
                        output.push((bucket.floor as i64, bucket.count as i64));
                    }
                }
            }
        }
        output
    }

    fn fake_readings(id: u64, cpu_deltas: Vec<u64>) -> FakeTask {
        let mut cpu_time = 0i64;
        let mut readings = vec![];
        for delta in cpu_deltas.iter() {
            cpu_time += *delta as i64;
            readings.push(zx::TaskRuntimeInfo { cpu_time, ..zx::TaskRuntimeInfo::default() })
        }
        FakeTask::new(id, readings)
    }

    // Test that the ceil function works: 0 cpu goes in bucket 0, 0.1..1 in bucket 1, etc.
    #[fuchsia::test]
    async fn bucket_cutoffs() {
        let readings = fake_readings(1, vec![1, 0, 500, 989, 990, 991, 999, 0]);
        let inspector = inspect::Inspector::new();
        let clock = FakeTime::new();
        let histogram = create_cpu_histogram(
            &inspector.root(),
            &ExtendedMoniker::parse_string_without_instances("/foo").unwrap(),
        );
        //assert_data_tree!(            inspector,            root: {});
        let mut task = TaskInfo::try_from_internal(
            readings,
            Some(histogram),
            clock.clone(),
            std::time::Duration::from_nanos(1000),
            1, /* cores */
        )
        .unwrap();

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 1
        let answer = vec![(1, 1)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 0
        let answer = vec![(0, 1), (1, 1)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 500
        let answer = vec![(0, 1), (1, 1), (50, 1)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 989
        let answer = vec![(0, 1), (1, 1), (50, 1), (99, 1)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 990
        let answer = vec![(0, 1), (1, 1), (50, 1), (99, 2)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 991
        let answer = vec![(0, 1), (1, 1), (50, 1), (99, 2), (100, 1)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 999
        let answer = vec![(0, 1), (1, 1), (50, 1), (99, 2), (100, 2)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await; // 0...
        let answer = vec![(0, 2), (1, 1), (50, 1), (99, 2), (100, 2)];
        assert_eq!(histogram_non_zero_values(&inspector), answer);
    }

    // Test that short time intervals (less than 90% of sample_period) are discarded.
    // Extra-long intervals should be recorded. In all cases, CPU % should be calculated over the
    // actual interval, not the sample_period.
    #[fuchsia::test]
    async fn discard_short_intervals() {
        let readings = fake_readings(1, vec![100, 100, 100, 100]);
        let inspector = inspect::Inspector::new();
        let clock = FakeTime::new();
        let histogram = create_cpu_histogram(
            &inspector.root(),
            &ExtendedMoniker::parse_string_without_instances("/foo").unwrap(),
        );
        let mut task = TaskInfo::try_from_internal(
            readings,
            Some(histogram),
            clock.clone(),
            std::time::Duration::from_nanos(1000),
            1, /* cores */
        )
        .unwrap();

        assert_eq!(histogram_non_zero_values(&inspector), vec![]);

        clock.add_ticks(900);
        task.measure_if_no_parent().await;
        assert_eq!(histogram_non_zero_values(&inspector), vec![(12, 1)]);

        clock.add_ticks(899);
        task.measure_if_no_parent().await;
        assert_eq!(histogram_non_zero_values(&inspector), vec![(12, 1)]); // No change

        clock.add_ticks(2000);
        task.measure_if_no_parent().await;
        assert_eq!(histogram_non_zero_values(&inspector), (vec![(5, 1), (12, 1)]));

        clock.add_ticks(1000);
        task.measure_if_no_parent().await;
        assert_eq!(histogram_non_zero_values(&inspector), (vec![(5, 1), (10, 1), (12, 1)]));
    }

    // Test that the CPU% takes the number of cores into account - that is, with N cores
    // the CPU% should be 1/N the amount it would be for 1 core.
    #[fuchsia::test]
    async fn divide_by_cores() {
        let readings = fake_readings(1, vec![400]);
        let inspector = inspect::Inspector::new();
        let clock = FakeTime::new();
        let histogram = create_cpu_histogram(
            &inspector.root(),
            &ExtendedMoniker::parse_string_without_instances("/foo").unwrap(),
        );
        let mut task = TaskInfo::try_from_internal(
            readings,
            Some(histogram),
            clock.clone(),
            std::time::Duration::from_nanos(1000),
            4, /* cores */
        )
        .unwrap();

        assert_eq!(histogram_non_zero_values(&inspector), vec![]);

        clock.add_ticks(1000);
        task.measure_if_no_parent().await;
        assert_eq!(histogram_non_zero_values(&inspector), vec![(10, 1)]);
    }
}
