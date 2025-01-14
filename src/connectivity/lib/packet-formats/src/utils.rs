// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Utilities useful when parsing and serializing wire formats.

use core::num::{NonZeroU32, NonZeroU64};
use core::time::Duration;

/// A zero-valued `Duration`.
const ZERO_DURATION: Duration = Duration::from_secs(0);

/// A thin wrapper over a [`Duration`] that guarantees that the underlying
/// `Duration` is non-zero.
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Copy, Hash)]
pub struct NonZeroDuration(Duration);

impl NonZeroDuration {
    /// Creates a non-zero without checking the value.
    ///
    /// # Safety
    ///
    /// If `d` is zero, unsafe code which relies on the invariant that
    /// `NonZeroDuration` values are always zero may cause undefined behavior.
    pub const unsafe fn new_unchecked(d: Duration) -> NonZeroDuration {
        NonZeroDuration(d)
    }

    /// Creates a new `NonZeroDuration` from the specified non-zero number of
    /// whole seconds.
    pub const fn from_nonzero_secs(secs: NonZeroU64) -> NonZeroDuration {
        NonZeroDuration(Duration::from_secs(secs.get()))
    }

    /// Creates a new `NonZeroDuration` from the specified non-zero number of
    /// whole seconds and additional nanoseconds.
    ///
    /// If the number of nanoseconds is greater than 1 billion (the number of
    /// nanoseconds in a second), then it will carry over into the seconds
    /// provided.
    ///
    /// # Panics
    ///
    /// This constructor will panic if the carry from the nanoseconds overflows
    /// the seconds counter.
    // TODO(https://github.com/rust-lang/rust/issues/72440): Make this const
    pub fn from_nonzero_secs_nanos(secs: NonZeroU64, nanos: NonZeroU32) -> NonZeroDuration {
        NonZeroDuration(Duration::new(secs.get(), nanos.get()))
    }

    /// Creates a new `NonZeroDuration` from the specified non-zero number of
    /// milliseconds.
    pub const fn from_nonzero_millis(millis: NonZeroU64) -> NonZeroDuration {
        NonZeroDuration(Duration::from_millis(millis.get()))
    }

    /// Creates a new `NonZeroDuration` from the specified non-zero number of
    /// microseconds.
    pub const fn from_nonzero_micros(micros: NonZeroU64) -> NonZeroDuration {
        NonZeroDuration(Duration::from_micros(micros.get()))
    }

    /// Creates a new `NonZeroDuration` from the specified non-zero number of
    /// nanoseconds.
    pub const fn from_nonzero_nanos(nanos: NonZeroU64) -> NonZeroDuration {
        NonZeroDuration(Duration::from_nanos(nanos.get()))
    }

    /// Creates a non-zero if the given value is not zero.
    pub fn new(d: Duration) -> Option<NonZeroDuration> {
        if d == ZERO_DURATION {
            return None;
        }

        Some(NonZeroDuration(d))
    }

    /// Returns the value as a [`Duration`].
    pub const fn get(self) -> Duration {
        self.0
    }
}

impl From<NonZeroDuration> for Duration {
    fn from(NonZeroDuration(d): NonZeroDuration) -> Duration {
        d
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn non_zero_duration() {
        // `NonZeroDuration` should not hold a zero duration.
        assert_eq!(NonZeroDuration::new(Duration::from_secs(0)), None);

        // `new_unchecked` should hold the duration as is.
        let d = Duration::from_secs(1);
        assert_eq!(unsafe { NonZeroDuration::new_unchecked(d) }, NonZeroDuration(d));

        // `NonZeroDuration` should hold a non-zero duration.
        let non_zero = NonZeroDuration::new(d);
        assert_eq!(non_zero, Some(NonZeroDuration(d)));

        let one_u64 = NonZeroU64::new(1).unwrap();
        assert_eq!(
            NonZeroDuration::from_nonzero_secs(one_u64),
            NonZeroDuration(Duration::from_secs(1))
        );
        assert_eq!(
            NonZeroDuration::from_nonzero_secs_nanos(one_u64, NonZeroU32::new(1).unwrap()),
            NonZeroDuration(Duration::new(1, 1))
        );
        assert_eq!(
            NonZeroDuration::from_nonzero_millis(one_u64),
            NonZeroDuration(Duration::from_millis(1))
        );
        assert_eq!(
            NonZeroDuration::from_nonzero_micros(one_u64),
            NonZeroDuration(Duration::from_micros(1))
        );
        assert_eq!(
            NonZeroDuration::from_nonzero_nanos(one_u64),
            NonZeroDuration(Duration::from_nanos(1))
        );

        // `get` and `Into::into` should return the underlying duration.
        let non_zero = non_zero.unwrap();
        assert_eq!(d, non_zero.get());
        assert_eq!(d, non_zero.into());
    }
}
