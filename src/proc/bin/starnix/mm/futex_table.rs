// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::Arc;

use crate::task::*;
use crate::types::*;

/// A table of futexes.
///
/// Each 32-bit aligned address in an address space can potentially have an associated futex that
/// userspace can wait upon. This table is a sparse representation that has an actual WaitQueue
/// only for those addresses that have ever actually had a futex operation performed on them.
#[derive(Default)]
pub struct FutexTable {
    /// The futexes associated with each address in the address space.
    ///
    /// This HashMap is populated on-demand when futexes are used.
    state: Mutex<HashMap<UserAddress, Arc<Mutex<WaitQueue>>>>,
}

impl FutexTable {
    /// Wait on the futex at the given address.
    ///
    /// See FUTEX_WAIT.
    pub fn wait(
        &self,
        task: &Task,
        addr: UserAddress,
        value: u32,
        mask: u32,
        deadline: zx::Time,
    ) -> Result<(), Errno> {
        let user_current = UserRef::<u32>::new(addr);
        let mut current = 0;
        let waiter = Waiter::for_task(task);
        {
            let waiters = self.get_waiters(addr);
            let mut waiters = waiters.lock();
            // TODO: This read should be atomic.
            task.mm.read_object(user_current, &mut current)?;
            if current != value {
                return Ok(());
            }

            waiters.wait_async_mask(&waiter, mask, WaitCallback::none());
        }
        waiter.wait_until(task, deadline)
    }

    /// Wake the given number of waiters on futex at the given address.
    ///
    /// See FUTEX_WAKE.
    pub fn wake(&self, addr: UserAddress, count: usize, mask: u32) {
        self.get_waiters(addr).lock().notify_mask_count(mask, count);
    }

    /// Returns the WaitQueue for a given address.
    fn get_waiters(&self, addr: UserAddress) -> Arc<Mutex<WaitQueue>> {
        let mut state = self.state.lock();
        let waiters = state.entry(addr).or_insert_with(|| Arc::new(Default::default()));
        Arc::clone(waiters)
    }
}
