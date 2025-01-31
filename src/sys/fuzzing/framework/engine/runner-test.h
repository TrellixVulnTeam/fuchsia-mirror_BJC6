// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_FRAMEWORK_ENGINE_RUNNER_TEST_H_
#define SRC_SYS_FUZZING_FRAMEWORK_ENGINE_RUNNER_TEST_H_

#include <fuchsia/fuzzer/cpp/fidl.h>
#include <lib/fidl/cpp/interface_request.h>

#include <memory>

#include "src/sys/fuzzing/common/dispatcher.h"
#include "src/sys/fuzzing/common/options.h"
#include "src/sys/fuzzing/common/runner-unittest.h"
#include "src/sys/fuzzing/common/testing/coverage.h"
#include "src/sys/fuzzing/framework/engine/runner.h"
#include "src/sys/fuzzing/framework/testing/adapter.h"
#include "src/sys/fuzzing/framework/testing/process.h"

namespace fuzzing {

using fuchsia::fuzzer::Result;

// Specializes the generic |RunnerTest| for |RunnerImpl|.
class RunnerImplTest : public RunnerTest {
 protected:
  // RunnerTest methods.
  void SetUp() override;
  void Configure(Runner* runner, const std::shared_ptr<Options>& options) override;
  Input GetTestInput() override;
  void SetFeedback(const Coverage& coverage, Result result, bool leak) override;

  // Unit test implementations and overrides.
  void RunAllForFuzzUntilTime() override;
  void MergeSeedError(Runner* runner) override;
  void RunAllForMerge() override;

 private:
  std::shared_ptr<Dispatcher> dispatcher_;
  FakeTargetAdapter target_adapter_;
  FakeProcess process_;
  fidl::InterfaceRequestHandler<ProcessProxy> process_proxy_handler_;
  bool stopped_ = true;
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_FRAMEWORK_ENGINE_RUNNER_TEST_H_
