// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::convert::TryFrom;
use std::sync::Arc;

use super::waiting::*;
use crate::errno;
use crate::error;
use crate::not_implemented;
use crate::signals::signal_handling::*;
use crate::signals::*;
use crate::syscalls::*;
use crate::task::*;
use crate::types::*;

pub fn sys_rt_sigaction(
    ctx: &SyscallContext<'_>,
    signum: UncheckedSignal,
    user_action: UserRef<sigaction_t>,
    user_old_action: UserRef<sigaction_t>,
    sigset_size: usize,
) -> Result<SyscallResult, Errno> {
    if sigset_size != std::mem::size_of::<sigset_t>() {
        return error!(EINVAL);
    }

    let signal = Signal::try_from(signum)?;

    let new_signal_action = if !user_action.is_null() {
        // Actions can't be set for SIGKILL and SIGSTOP, but the actions for these signals can
        // still be returned in `user_old_action`, so only return early if the intention is to
        // set an action (i.e., the user_action is non-null).
        if signal.is_unblockable() {
            return error!(EINVAL);
        }

        let mut signal_action = sigaction_t::default();
        ctx.task.mm.read_object(user_action, &mut signal_action)?;
        Some(signal_action)
    } else {
        None
    };

    let signal_actions = &ctx.task.signal_actions;
    let old_action = if let Some(new_signal_action) = new_signal_action {
        signal_actions.set(signal, new_signal_action)
    } else {
        signal_actions.get(signal)
    };

    if !user_old_action.is_null() {
        ctx.task.mm.write_object(user_old_action, &old_action)?;
    }

    Ok(SUCCESS)
}

pub fn sys_rt_sigprocmask(
    ctx: &SyscallContext<'_>,
    how: u32,
    user_set: UserRef<sigset_t>,
    user_old_set: UserRef<sigset_t>,
    sigset_size: usize,
) -> Result<SyscallResult, Errno> {
    if sigset_size != std::mem::size_of::<sigset_t>() {
        return error!(EINVAL);
    }
    match how {
        SIG_BLOCK | SIG_UNBLOCK | SIG_SETMASK => (),
        _ => return error!(EINVAL),
    };

    // Read the new mask. This must be done before the old maks is written to `user_old_set`
    // since it might point to the same location as `user_set`.
    let mut new_mask = sigset_t::default();
    if !user_set.is_null() {
        ctx.task.mm.read_object(user_set, &mut new_mask)?;
    }

    let mut signal_state = ctx.task.signals.write();
    let signal_mask = signal_state.mask;
    // If old_set is not null, store the previous value in old_set.
    if !user_old_set.is_null() {
        ctx.task.mm.write_object(user_old_set, &signal_mask)?;
    }

    // If set is null, how is ignored and the mask is not updated.
    if user_set.is_null() {
        return Ok(SUCCESS);
    }

    let signal_mask = match how {
        SIG_BLOCK => (signal_mask | new_mask),
        SIG_UNBLOCK => signal_mask & !new_mask,
        SIG_SETMASK => new_mask,
        // Arguments have already been verified, this should never match.
        _ => signal_mask,
    };

    // Can't block SIGKILL, or SIGSTOP.
    let signal_mask = signal_mask & !(Signal::SIGSTOP.mask() | Signal::SIGKILL.mask());
    signal_state.mask = signal_mask;

    Ok(SUCCESS)
}

pub fn sys_sigaltstack(
    ctx: &SyscallContext<'_>,
    user_ss: UserRef<sigaltstack_t>,
    user_old_ss: UserRef<sigaltstack_t>,
) -> Result<SyscallResult, Errno> {
    let mut signal_state = ctx.task.signals.write();
    let on_signal_stack = signal_state
        .alt_stack
        .map(|signal_stack| signal_stack.contains_pointer(ctx.registers.rsp))
        .unwrap_or(false);

    let mut ss = sigaltstack_t::default();
    if !user_ss.is_null() {
        if on_signal_stack {
            return error!(EPERM);
        }
        ctx.task.mm.read_object(user_ss, &mut ss)?;
        if (ss.ss_flags & !(SS_AUTODISARM | SS_DISABLE)) != 0 {
            return error!(EINVAL);
        }
    }

    if !user_old_ss.is_null() {
        let mut old_ss = match signal_state.alt_stack {
            Some(old_ss) => old_ss,
            None => sigaltstack_t { ss_flags: SS_DISABLE, ..sigaltstack_t::default() },
        };
        if on_signal_stack {
            old_ss.ss_flags = SS_ONSTACK;
        }
        ctx.task.mm.write_object(user_old_ss, &old_ss)?;
    }

    if !user_ss.is_null() {
        if ss.ss_flags & SS_DISABLE != 0 {
            signal_state.alt_stack = None;
        } else {
            signal_state.alt_stack = Some(ss);
        }
    }

    Ok(SUCCESS)
}

pub fn sys_rt_sigsuspend(
    ctx: &SyscallContext<'_>,
    user_mask: UserRef<sigset_t>,
    sigset_size: usize,
) -> Result<SyscallResult, Errno> {
    if sigset_size != std::mem::size_of::<sigset_t>() {
        return error!(EINVAL);
    }

    let mut mask = sigset_t::default();
    ctx.task.mm.read_object(user_mask, &mut mask)?;

    let waiter = Waiter::for_task(&ctx.task);
    // This block is important to release the signal state lock while waiting.
    let old_mask = {
        let mut signal_state = ctx.task.signals.write();
        let old_mask = signal_state.mask;
        signal_state.mask = mask & !(Signal::SIGSTOP.mask() | Signal::SIGKILL.mask());
        old_mask
    };
    let result = waiter.wait(&ctx.task);
    // TODO(tbodt): There's a window right here where another thread can see an empty
    // signals.waiter and the wrong mask. Unsure if this is actually a problem.
    ctx.task.signals.write().mask = old_mask;
    result?;
    Ok(SUCCESS)
}

pub fn sys_kill(
    ctx: &SyscallContext<'_>,
    pid: pid_t,
    unchecked_signal: UncheckedSignal,
) -> Result<SyscallResult, Errno> {
    let task = ctx.task;
    match pid {
        pid if pid > 0 => {
            // "If pid is positive, then signal sig is sent to the process with
            // the ID specified by pid."
            let target = task.get_task(pid).ok_or(errno!(ESRCH))?;
            if !task.can_signal(&target, &unchecked_signal) {
                return error!(EPERM);
            }
            send_signal(&target, &unchecked_signal)?;
        }
        pid if pid == -1 => {
            // "If pid equals -1, then sig is sent to every process for which
            // the calling process has permission to send signals, except for
            // process 1 (init), but ... POSIX.1-2001 requires that kill(-1,sig)
            // send sig to all processes that the calling process may send
            // signals to, except possibly for some implementation-defined
            // system processes. Linux allows a process to signal itself, but on
            // Linux the call kill(-1,sig) does not signal the calling process."

            let thread_groups = task.thread_group.kernel.pids.read().get_thread_groups();
            signal_thread_groups(
                &task,
                &unchecked_signal,
                thread_groups.into_iter().filter(|thread_group| {
                    if task.thread_group == *thread_group {
                        return false;
                    }
                    // TODO(lindkvist): This should be compared to the init pid.
                    if thread_group.leader == 0 {
                        return false;
                    }
                    true
                }),
            )?;
        }
        _ => {
            // "If pid equals 0, then sig is sent to every process in the
            // process group of the calling process."
            //
            // "If pid is less than -1, then sig is sent to every process in the
            // process group whose ID is -pid."
            let process_group_id = match pid {
                0 => task.get_pgrp(),
                _ => -pid,
            };

            let thread_groups = task.thread_group.kernel.pids.read().get_thread_groups();
            signal_thread_groups(
                &task,
                &unchecked_signal,
                thread_groups.into_iter().filter(|thread_group| {
                    task.get_task(thread_group.leader).unwrap().get_pgrp() == process_group_id
                }),
            )?;
        }
    };

    Ok(SUCCESS)
}

pub fn sys_tgkill(
    ctx: &SyscallContext<'_>,
    tgid: pid_t,
    tid: pid_t,
    unchecked_signal: UncheckedSignal,
) -> Result<SyscallResult, Errno> {
    // Linux returns EINVAL when the tgid or tid <= 0.
    if tgid <= 0 || tid <= 0 {
        return error!(EINVAL);
    }

    let target = ctx.task.get_task(tid).ok_or(errno!(ESRCH))?;
    if target.get_pid() != tgid {
        return error!(EINVAL);
    }

    if !ctx.task.can_signal(&target, &unchecked_signal) {
        return error!(EPERM);
    }

    send_signal(&target, &unchecked_signal)?;
    Ok(SUCCESS)
}

pub fn sys_rt_sigreturn(ctx: &mut SyscallContext<'_>) -> Result<SyscallResult, Errno> {
    restore_from_signal_handler(ctx);
    Ok(SyscallResult::SigReturn)
}

/// Sends a signal to all thread groups in `thread_groups`.
///
/// # Parameters
/// - `task`: The task that is sending the signal.
/// - `unchecked_signal`: The signal that is to be sent. Unchecked, since `0` is a sentinel value
/// where rights are to be checked but no signal is actually sent.
/// - `thread_groups`: The thread groups to signal.
///
/// # Returns
/// Returns Ok(()) if at least one signal was sent, otherwise the last error that was encountered.
fn signal_thread_groups<F>(
    task: &Task,
    unchecked_signal: &UncheckedSignal,
    thread_groups: F,
) -> Result<(), Errno>
where
    F: Iterator<Item = Arc<ThreadGroup>>,
{
    let mut last_error = errno!(ESRCH);
    let mut sent_signal = false;

    // This loop keeps track of whether a signal was sent, so that "on
    // success (at least one signal was sent), zero is returned."
    for thread_group in thread_groups {
        let leader = task.get_task(thread_group.leader).ok_or(errno!(ESRCH))?;
        if !task.can_signal(&leader, &unchecked_signal) {
            last_error = errno!(EPERM);
        }

        match send_signal(&leader, unchecked_signal) {
            Ok(_) => sent_signal = true,
            Err(errno) => last_error = errno,
        }
    }

    if sent_signal {
        return Ok(());
    } else {
        return Err(last_error);
    }
}

pub fn sys_waitid(
    ctx: &SyscallContext<'_>,
    id_type: u32,
    id: i32,
    user_info: UserRef<siginfo_t>,
    options: u32,
) -> Result<SyscallResult, Errno> {
    // waitid requires at least one option to be provided.
    if options == 0 {
        return error!(EINVAL);
    }
    if options & !(WEXITED | WNOHANG) != 0 {
        not_implemented!("Waitid does support options: {:?}", options);
        return error!(EINVAL);
    }

    match id_type {
        P_PID => {
            // wait_on_pid returns None if the task was not waited on. In that case, no siginfo is
            // returned.
            if let Some(zombie_task) =
                wait_on_pid(ctx.task, TaskSelector::Pid(id), (options & WNOHANG) == 0)?
            {
                let status = exit_code_to_status(zombie_task.exit_code);

                let mut siginfo = siginfo_t::default();
                siginfo.si_signo = SIGCHLD as i32;
                siginfo.si_code = CLD_EXITED;
                siginfo.si_status = status;
                ctx.task.mm.write_object(user_info, &siginfo)?;
            }

            Ok(SUCCESS)
        }
        P_ALL | P_PIDFD | P_PGID => {
            not_implemented!("waitid currently only supports P_ID");
            error!(ENOSYS)
        }
        _ => error!(EINVAL),
    }
}

pub fn sys_wait4(
    ctx: &SyscallContext<'_>,
    pid: pid_t,
    user_wstatus: UserRef<i32>,
    options: u32,
    user_rusage: UserRef<rusage>,
) -> Result<SyscallResult, Errno> {
    let selector = if pid == -1 {
        TaskSelector::Any
    } else if pid > 0 {
        TaskSelector::Pid(pid)
    } else {
        not_implemented!("unimplemented wait4 pid selector {}", pid);
        return error!(ENOSYS);
    };

    if let Some(zombie_task) = wait_on_pid(ctx.task, selector, (options & WNOHANG) == 0)? {
        let status = exit_code_to_status(zombie_task.exit_code);

        if !user_rusage.is_null() {
            let usage = rusage::default();
            // TODO(fxb/76976): Return proper usage information.
            ctx.task.mm.write_object(user_rusage, &usage)?;
            not_implemented!("wait4 does not set rusage info");
        }

        if !user_wstatus.is_null() {
            // TODO(fxb/76976): Return proper status.
            not_implemented!("wait4 does not set signal info in wstatus");
            ctx.task.mm.write_object(user_wstatus, &status)?;
        }

        Ok(zombie_task.id.into())
    } else {
        Ok(SUCCESS)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mm::PAGE_SIZE;
    use fuchsia_async as fasync;

    use crate::testing::*;

    #[fasync::run_singlethreaded(test)]
    async fn test_sigaltstack() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);

        let user_ss = UserRef::<sigaltstack_t>::new(addr);
        let nullptr = UserRef::<sigaltstack_t>::default();

        // Check that the initial state is disabled.
        sys_sigaltstack(&ctx, nullptr, user_ss).expect("failed to call sigaltstack");
        let mut ss = sigaltstack_t::default();
        ctx.task.mm.read_object(user_ss, &mut ss).expect("failed to read struct");
        assert!(ss.ss_flags & SS_DISABLE != 0);

        // Install a sigaltstack and read it back out.
        ss.ss_sp = UserAddress::from(0x7FFFF);
        ss.ss_size = 0x1000;
        ss.ss_flags = SS_AUTODISARM;
        ctx.task.mm.write_object(user_ss, &ss).expect("failed to write struct");
        sys_sigaltstack(&ctx, user_ss, nullptr).expect("failed to call sigaltstack");
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigaltstack_t>()])
            .expect("failed to clear struct");
        sys_sigaltstack(&ctx, nullptr, user_ss).expect("failed to call sigaltstack");
        let mut another_ss = sigaltstack_t::default();
        ctx.task.mm.read_object(user_ss, &mut another_ss).expect("failed to read struct");
        assert_eq!(ss, another_ss);

        // Disable the sigaltstack and read it back out.
        ss.ss_flags = SS_DISABLE;
        ctx.task.mm.write_object(user_ss, &ss).expect("failed to write struct");
        sys_sigaltstack(&ctx, user_ss, nullptr).expect("failed to call sigaltstack");
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigaltstack_t>()])
            .expect("failed to clear struct");
        sys_sigaltstack(&ctx, nullptr, user_ss).expect("failed to call sigaltstack");
        ctx.task.mm.read_object(user_ss, &mut ss).expect("failed to read struct");
        assert!(ss.ss_flags & SS_DISABLE != 0);
    }

    /// It is invalid to call rt_sigprocmask with a sigsetsize that does not match the size of
    /// sigset_t.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_invalid_size() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);

        let set = UserRef::<sigset_t>::default();
        let old_set = UserRef::<sigset_t>::default();
        let how = 0;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>() * 2),
            error!(EINVAL)
        );
        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>() / 2),
            error!(EINVAL)
        );
    }

    /// It is invalid to call rt_sigprocmask with a bad `how`.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_invalid_how() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);

        let set = UserRef::<sigset_t>::new(addr);
        let old_set = UserRef::<sigset_t>::default();
        let how = SIG_SETMASK | SIG_UNBLOCK | SIG_BLOCK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            error!(EINVAL)
        );
    }

    /// It is valid to call rt_sigprocmask with a null value for set. In that case, old_set should
    /// contain the current signal mask.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_null_set() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        let original_mask = Signal::SIGTRAP.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let set = UserRef::<sigset_t>::default();
        let old_set = UserRef::<sigset_t>::new(addr);
        let how = SIG_SETMASK;

        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>()])
            .expect("failed to clear struct");

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
    }

    /// It is valid to call rt_sigprocmask with null values for both set and old_set.
    /// In this case, how should be ignored and the set remains the same.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_null_set_and_old_set() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let original_mask = Signal::SIGTRAP.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let set = UserRef::<sigset_t>::default();
        let old_set = UserRef::<sigset_t>::default();
        let how = SIG_SETMASK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );
        assert_eq!(ctx.task.signals.read().mask, original_mask);
    }

    /// Calling rt_sigprocmask with SIG_SETMASK should set the mask to the provided set.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_setmask() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let original_mask = Signal::SIGTRAP.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let new_mask: sigset_t = Signal::SIGIO.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        let old_set = UserRef::<sigset_t>::new(addr + std::mem::size_of::<sigset_t>());
        let how = SIG_SETMASK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
        assert_eq!(ctx.task.signals.read().mask, new_mask);
    }

    /// Calling st_sigprocmask with a how of SIG_BLOCK should add to the existing set.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_block() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let original_mask = Signal::SIGTRAP.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let new_mask: sigset_t = Signal::SIGIO.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        let old_set = UserRef::<sigset_t>::new(addr + std::mem::size_of::<sigset_t>());
        let how = SIG_BLOCK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
        assert_eq!(ctx.task.signals.read().mask, new_mask | original_mask);
    }

    /// Calling st_sigprocmask with a how of SIG_UNBLOCK should remove from the existing set.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_unblock() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let original_mask = Signal::SIGTRAP.mask() | Signal::SIGIO.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let new_mask: sigset_t = Signal::SIGTRAP.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        let old_set = UserRef::<sigset_t>::new(addr + std::mem::size_of::<sigset_t>());
        let how = SIG_UNBLOCK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
        assert_eq!(ctx.task.signals.read().mask, Signal::SIGIO.mask());
    }

    /// It's ok to call sigprocmask to unblock a signal that is not set.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_unblock_not_set() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let original_mask = Signal::SIGIO.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let new_mask: sigset_t = Signal::SIGTRAP.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        let old_set = UserRef::<sigset_t>::new(addr + std::mem::size_of::<sigset_t>());
        let how = SIG_UNBLOCK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
        assert_eq!(ctx.task.signals.read().mask, original_mask);
    }

    /// It's not possible to block SIGKILL or SIGSTOP.
    #[fasync::run_singlethreaded(test)]
    async fn test_sigprocmask_kill_stop() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let original_mask = Signal::SIGIO.mask();
        {
            ctx.task.signals.write().mask = original_mask;
        }

        let new_mask: sigset_t = Signal::SIGSTOP.mask() | Signal::SIGKILL.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        let old_set = UserRef::<sigset_t>::new(addr + std::mem::size_of::<sigset_t>());
        let how = SIG_BLOCK;

        assert_eq!(
            sys_rt_sigprocmask(&ctx, how, set, old_set, std::mem::size_of::<sigset_t>()),
            Ok(SUCCESS)
        );

        let mut old_mask = sigset_t::default();
        ctx.task.mm.read_object(old_set, &mut old_mask).expect("failed to read mask");
        assert_eq!(old_mask, original_mask);
        assert_eq!(ctx.task.signals.read().mask, original_mask);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sigaction_invalid_signal() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        assert_eq!(
            sys_rt_sigaction(
                &ctx,
                UncheckedSignal::from(SIGKILL),
                // The signal is only checked when the action is set (i.e., action is non-null).
                UserRef::<sigaction_t>::new(UserAddress::from(10)),
                UserRef::<sigaction_t>::default(),
                std::mem::size_of::<sigset_t>(),
            ),
            error!(EINVAL)
        );
        assert_eq!(
            sys_rt_sigaction(
                &ctx,
                UncheckedSignal::from(SIGSTOP),
                // The signal is only checked when the action is set (i.e., action is non-null).
                UserRef::<sigaction_t>::new(UserAddress::from(10)),
                UserRef::<sigaction_t>::default(),
                std::mem::size_of::<sigset_t>(),
            ),
            error!(EINVAL)
        );
        assert_eq!(
            sys_rt_sigaction(
                &ctx,
                UncheckedSignal::from(Signal::NUM_SIGNALS + 1),
                // The signal is only checked when the action is set (i.e., action is non-null).
                UserRef::<sigaction_t>::new(UserAddress::from(10)),
                UserRef::<sigaction_t>::default(),
                std::mem::size_of::<sigset_t>(),
            ),
            error!(EINVAL)
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sigaction_old_value_set() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigaction_t>()])
            .expect("failed to clear struct");

        let mut original_action = sigaction_t::default();
        original_action.sa_mask = 3;

        {
            ctx.task.signal_actions.set(Signal::SIGHUP, original_action.clone());
        }

        let old_action_ref = UserRef::<sigaction_t>::new(addr);
        assert_eq!(
            sys_rt_sigaction(
                &ctx,
                UncheckedSignal::from(SIGHUP),
                UserRef::<sigaction_t>::default(),
                old_action_ref,
                std::mem::size_of::<sigset_t>()
            ),
            Ok(SUCCESS)
        );

        let mut old_action = sigaction_t::default();
        ctx.task.mm.read_object(old_action_ref, &mut old_action).expect("failed to read action");
        assert_eq!(old_action, original_action);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sigaction_new_value_set() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigaction_t>()])
            .expect("failed to clear struct");

        let mut original_action = sigaction_t::default();
        original_action.sa_mask = 3;
        let set_action_ref = UserRef::<sigaction_t>::new(addr);
        ctx.task.mm.write_object(set_action_ref, &original_action).expect("failed to set action");

        assert_eq!(
            sys_rt_sigaction(
                &ctx,
                UncheckedSignal::from(SIGINT),
                set_action_ref,
                UserRef::<sigaction_t>::default(),
                std::mem::size_of::<sigset_t>(),
            ),
            Ok(SUCCESS)
        );

        assert_eq!(ctx.task.signal_actions.get(Signal::SIGINT), original_action,);
    }

    /// A task should be able to signal itself.
    #[fasync::run_singlethreaded(test)]
    async fn test_kill_same_task() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);

        assert_eq!(sys_kill(&ctx, task_owner.task.id, SIGINT.into()), Ok(SUCCESS));
    }

    /// A task should not be able to signal a nonexistent task.
    #[fasync::run_singlethreaded(test)]
    async fn test_kill_invalid_task() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);

        assert_eq!(sys_kill(&ctx, 9, SIGINT.into()), error!(ESRCH));
    }

    /// A task should not be able to send an invalid signal.
    #[fasync::run_singlethreaded(test)]
    async fn test_kill_invalid_signal() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);

        assert_eq!(sys_kill(&ctx, task_owner.task.id, UncheckedSignal::from(75)), error!(EINVAL));
    }

    /// Sending a blocked signal should result in a pending signal.
    #[fasync::run_singlethreaded(test)]
    async fn test_blocked_signal_pending() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let new_mask: sigset_t = Signal::SIGIO.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        assert_eq!(
            sys_rt_sigprocmask(
                &ctx,
                SIG_BLOCK,
                set,
                UserRef::default(),
                std::mem::size_of::<sigset_t>()
            ),
            Ok(SUCCESS)
        );
        assert_eq!(sys_kill(&ctx, task_owner.task.id, SIGIO.into()), Ok(SUCCESS));

        {
            let pending_signals = &task_owner.task.signals.read().pending;
            assert_eq!(pending_signals[&Signal::SIGIO], 1);
        }

        // A second signal should not increment the number of pending signals.
        assert_eq!(sys_kill(&ctx, task_owner.task.id, SIGIO.into()), Ok(SUCCESS));

        {
            let pending_signals = &task_owner.task.signals.read().pending;
            assert_eq!(pending_signals[&Signal::SIGIO], 1);
        }
    }

    /// More than one instance of a real-time signal can be blocked.
    #[fasync::run_singlethreaded(test)]
    async fn test_blocked_real_time_signal_pending() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
        ctx.task
            .mm
            .write_memory(addr, &[0u8; std::mem::size_of::<sigset_t>() * 2])
            .expect("failed to clear struct");

        let new_mask: sigset_t = Signal::SIGRTMIN.mask();
        let set = UserRef::<sigset_t>::new(addr);
        ctx.task.mm.write_object(set, &new_mask).expect("failed to set mask");

        assert_eq!(
            sys_rt_sigprocmask(
                &ctx,
                SIG_BLOCK,
                set,
                UserRef::default(),
                std::mem::size_of::<sigset_t>()
            ),
            Ok(SUCCESS)
        );
        assert_eq!(sys_kill(&ctx, task_owner.task.id, SIGRTMIN.into()), Ok(SUCCESS));

        {
            let pending_signals = &task_owner.task.signals.read().pending;
            assert_eq!(pending_signals[&Signal::SIGRTMIN], 1);
        }

        // A second signal should increment the number of pending signals.
        assert_eq!(sys_kill(&ctx, task_owner.task.id, SIGRTMIN.into()), Ok(SUCCESS));

        {
            let pending_signals = &task_owner.task.signals.read().pending;
            assert_eq!(pending_signals[&Signal::SIGRTMIN], 2);
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_suspend() {
        let (kernel, task_owner) = create_kernel_and_task();
        let first_task_clone = Arc::clone(&task_owner.task);
        let first_task_id = first_task_clone.id;

        let thread = std::thread::spawn(move || {
            let ctx = SyscallContext::new(&task_owner.task);
            let addr = map_memory(&ctx, UserAddress::default(), *PAGE_SIZE);
            let user_ref = UserRef::<sigset_t>::new(addr);

            let sigset: sigset_t = !Signal::SIGCONT.mask();
            ctx.task.mm.write_object(user_ref, &sigset).expect("failed to set action");

            assert_eq!(
                sys_rt_sigsuspend(&ctx, user_ref, std::mem::size_of::<sigset_t>()),
                error!(EINTR)
            );
        });

        let second_task_owner = create_task(&kernel, "test-task-2");
        let ctx = SyscallContext::new(&second_task_owner.task);

        // Wait for the first task to be suspended.
        let mut suspended = false;
        while !suspended {
            suspended = first_task_clone.signals.read().waiter.is_some();
            std::thread::sleep(std::time::Duration::from_millis(10));
        }

        // Signal the suspended task with a signal that is not blocked (only SIGIO in this test).
        let _ = sys_kill(&ctx, first_task_id, UncheckedSignal::from(SIGCONT));

        // Wait for the sigsuspend to complete.
        let _ = thread.join();

        assert!(first_task_clone.signals.read().waiter.is_none());
    }

    /// Waitid does not support all options.
    #[fasync::run_singlethreaded(test)]
    async fn test_waitid_options() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let ctx = SyscallContext::new(&task_owner.task);
        let id = 1;
        assert_eq!(sys_waitid(&ctx, P_PID, id, UserRef::default(), 0), Err(EINVAL));
        assert_eq!(sys_waitid(&ctx, P_PID, id, UserRef::default(), WSTOPPED), Err(EINVAL));
        assert_eq!(sys_waitid(&ctx, P_PID, id, UserRef::default(), WCONTINUED), Err(EINVAL));
        assert_eq!(sys_waitid(&ctx, P_PID, id, UserRef::default(), WNOWAIT), Err(EINVAL));
        assert_eq!(
            sys_waitid(&ctx, P_PID, id, UserRef::default(), WEXITED | WSTOPPED),
            Err(EINVAL)
        );
        assert_eq!(
            sys_waitid(&ctx, P_PID, id, UserRef::default(), WEXITED | WCONTINUED),
            Err(EINVAL)
        );
        assert_eq!(sys_waitid(&ctx, P_PID, id, UserRef::default(), WEXITED | WNOWAIT), Err(EINVAL));
    }
}
