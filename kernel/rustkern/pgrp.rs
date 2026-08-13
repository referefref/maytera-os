// rustkern/pgrp.rs - POSIX process-group and session POLICY (#745, local 82).
//
// New kernel logic, so Rust per the 2026-07-16 rule, in the same shape as
// sessionid.rs: this module holds the DECISIONS and calls no C at all. Every
// input is supplied by the thin C glue in proc/syscall.c, which owns the
// process table walk (proc_get / MAX_PROCESSES). That split is what makes the
// rules testable at boot with no processes and no scheduler, which
// pgrp_selftest_rs() does.
//
// ===========================================================================
// WHY THESE FOUR SYSCALLS ARE WORTH IMPLEMENTING AND NOT JUST DECLARING
// ---------------------------------------------------------------------------
// setsid/setpgid/getpgid/getsid were declared in userland/libc/syscall.h with
// working wrappers in unistd.c, and defined nowhere in the kernel: every call
// hit the dispatcher default, logged "[SYSCALL] Unknown syscall 95" and
// returned -1, so setpgid() reported EPERM and getpgid() reported failure.
//
// The reason to fix that rather than delete the numbers is that THE DATA MODEL
// ALREADY EXISTS AND IS ALREADY ENFORCED. process_t carries `pgrp` and
// `session` (proc/process.h), proc_create seeds them and fork inherits them
// (proc/process.c), sig_raise_pgrp() walks the table matching p->pgrp
// (proc/signal.c), and the TTY line discipline uses it for real: Ctrl-C, Ctrl-\
// and Ctrl-Z raise SIGINT/SIGQUIT/SIGTSTP at t->fg_pgrp only, and a hangup
// raises SIGHUP at it (drivers/tty.c). TIOCSPGRP already sets fg_pgrp.
//
// So the machinery to deliver a signal to a job exists, and the machinery to
// choose which job is foreground exists, and the ONLY missing piece was any way
// for a process to put itself or a child into a group in the first place. Every
// process inherits pgrp from its parent forever, so the whole tree is one group
// and Ctrl-C reaches every process descended from the shell. That is not a
// number nobody acts on; it is a half-built mechanism, and these four calls are
// the half that was missing.
//
// ===========================================================================
// THE RULES, AND WHY EACH ONE IS A REFUSAL RATHER THAN A CONVENIENCE
// ---------------------------------------------------------------------------
// POSIX's setpgid restrictions are not bureaucracy: each one prevents a process
// from reaching into a job it does not own. Without them, "set my group" is an
// arbitrary-signal-target primitive, because putting yourself into another
// session's foreground group makes that terminal's Ctrl-C signal YOU, and
// moving a victim into a group you control makes YOUR terminal signal IT.
// They are enforced here, not in the glue, so there is one place to read them.
// ===========================================================================

// Negative Linux errnos, the convention every other syscall in this dispatcher
// returns (net/socket.c E_* block, userland/libc/errno.h).
pub const E_PERM: i32 = -1;
pub const E_SRCH: i32 = -3;
pub const E_INVAL: i32 = -22;

/// Decision codes for setsid(). Kept as an enum-shaped set of consts rather
/// than raw errnos at the boundary so the glue logs a REASON.
pub const SETSID_ALLOW: i32 = 0;
pub const SETSID_DENY_ALREADY_LEADER: i32 = 1;

/// setsid(): may the caller create a new session?
///
/// The single POSIX rule: a process that is ALREADY a process group leader
/// (pid == pgrp) may not call setsid(). The reason is not arbitrary. setsid()
/// makes the caller the leader of a NEW group whose pgid equals its pid. If the
/// caller already leads a group with that same pgid, the new session's group id
/// would collide with the existing group's, and the two groups would be
/// indistinguishable to every pgid-keyed operation in the kernel, including
/// sig_raise_pgrp(). Refusing is what keeps pgid unique per group.
///
/// The standard idiom is therefore fork-then-setsid in the child, which is
/// guaranteed not to be a group leader.
///
/// Returns SETSID_ALLOW or SETSID_DENY_ALREADY_LEADER.
#[no_mangle]
pub extern "C" fn pgrp_setsid_decide_rs(caller_pid: u32, caller_pgrp: u32) -> i32 {
    if caller_pid == caller_pgrp {
        return SETSID_DENY_ALREADY_LEADER;
    }
    SETSID_ALLOW
}

/// Resolve setpgid/getpgid/getsid's pid argument. 0 means "me" in all three.
/// Separated out because getting this wrong silently operates on pid 0, which
/// is the idle process, and would look like a no-op rather than an error.
#[no_mangle]
pub extern "C" fn pgrp_resolve_pid_rs(arg_pid: i64, caller_pid: u32) -> u32 {
    if arg_pid == 0 {
        return caller_pid;
    }
    arg_pid as u32
}

/// Resolve setpgid's pgid argument. 0 means "the target's own pid", i.e. make
/// the target a group leader. A negative pgid is EINVAL.
///
/// Returns the resolved pgid, or E_INVAL (negative) on a bad argument. The
/// caller distinguishes them by sign, which is unambiguous because a pgid is a
/// pid and pids here start at 1.
#[no_mangle]
pub extern "C" fn pgrp_resolve_pgid_rs(arg_pgid: i64, target_pid: u32) -> i64 {
    if arg_pgid < 0 {
        return E_INVAL as i64;
    }
    if arg_pgid == 0 {
        return target_pid as i64;
    }
    arg_pgid
}

/// setpgid(pid, pgid): may the caller move `target` into group `pgid`?
///
/// All arguments are already resolved (0 substituted) by the two functions
/// above. The glue supplies the facts; this decides.
///
/// `target_exists`        0 when proc_get(target_pid) found nothing.
/// `target_ppid`          the target's parent pid.
/// `target_session`       the target's session id.
/// `pgid_in_caller_session`
///                        1 when at least one EXISTING process is already in
///                        group `pgid` AND is in the caller's session, or when
///                        pgid == target_pid (creating a new group is always
///                        allowed within your own session). The glue computes
///                        this because it requires a table walk.
///
/// Returns 0 to allow, or a negative errno.
#[no_mangle]
pub extern "C" fn pgrp_setpgid_decide_rs(
    caller_pid: u32,
    caller_session: u32,
    target_pid: u32,
    target_exists: u32,
    target_ppid: u32,
    target_session: u32,
    pgid: u32,
    pgid_in_caller_session: u32,
) -> i32 {
    // 1. No such process. Checked first: every rule below reads target state.
    if target_exists == 0 {
        return E_SRCH;
    }

    // 2. The target must be the caller or a CHILD of the caller. POSIX says
    //    ESRCH here, not EPERM, and the distinction is load-bearing: a shell
    //    racing a child that already exited must be able to tell "gone" from
    //    "refused", because the first is normal and the second is a bug.
    if target_pid != caller_pid && target_ppid != caller_pid {
        return E_SRCH;
    }

    // 3. The target must be in the CALLER'S session. Without this, a process
    //    could move a child it forked into another session's process group, and
    //    that session's terminal would then signal a process that is not on it.
    if target_session != caller_session {
        return E_PERM;
    }

    // 4. A SESSION LEADER may not change its process group. Its pgid is the
    //    session id; changing it would leave the session named after a group
    //    that no longer exists, so every session lookup would be wrong.
    if target_pid == target_session {
        return E_PERM;
    }

    // 5. The destination group must already exist in the caller's session, or
    //    be the target's own new group. Joining a group in another session is
    //    the same reach-across as rule 3, in the other direction.
    if pgid != target_pid && pgid_in_caller_session == 0 {
        return E_PERM;
    }

    0
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Runs at boot so the rules are proven LIVE on this build rather than merely
// compiled in. Returns a bit mask of failures; 0 is a pass.
//
// Every case below is a REFUSAL this tree would otherwise have had to discover
// from a misbehaving shell. The permissive cases are here too, because a policy
// that refuses everything also passes every refusal test.
// ===========================================================================
#[no_mangle]
pub extern "C" fn pgrp_selftest_rs() -> u32 {
    let mut fails: u32 = 0;

    // --- setsid ------------------------------------------------------------
    // 1. A group leader may not setsid(). This is the case that makes the
    //    fork-then-setsid idiom mandatory; if it ever starts returning ALLOW,
    //    two distinct groups can share a pgid and sig_raise_pgrp() silently
    //    signals both.
    if pgrp_setsid_decide_rs(10, 10) != SETSID_DENY_ALREADY_LEADER {
        fails |= 1 << 0;
    }
    // 2. A non-leader may.
    if pgrp_setsid_decide_rs(11, 10) != SETSID_ALLOW {
        fails |= 1 << 1;
    }

    // --- argument resolution -----------------------------------------------
    // 3. pid 0 means "me", not "the idle process".
    if pgrp_resolve_pid_rs(0, 42) != 42 {
        fails |= 1 << 2;
    }
    if pgrp_resolve_pid_rs(7, 42) != 7 {
        fails |= 1 << 3;
    }
    // 4. pgid 0 means "make the target a group leader".
    if pgrp_resolve_pgid_rs(0, 7) != 7 {
        fails |= 1 << 4;
    }
    if pgrp_resolve_pgid_rs(9, 7) != 9 {
        fails |= 1 << 5;
    }
    // 5. A negative pgid is EINVAL, not a huge unsigned group id. Getting this
    //    wrong would let a caller name group 4294967295 and quietly succeed.
    if pgrp_resolve_pgid_rs(-1, 7) != E_INVAL as i64 {
        fails |= 1 << 6;
    }

    // --- setpgid -----------------------------------------------------------
    // 6. The ordinary shell case: shell(pid 10, session 10) puts its child
    //    (pid 11, ppid 10, session 10) into a NEW group led by the child.
    if pgrp_setpgid_decide_rs(10, 10, 11, 1, 10, 10, 11, 0) != 0 {
        fails |= 1 << 7;
    }
    // 7. The pipeline case: a second child joins the FIRST child's group, which
    //    already exists in this session.
    if pgrp_setpgid_decide_rs(10, 10, 12, 1, 10, 10, 11, 1) != 0 {
        fails |= 1 << 8;
    }
    // 8. A process setting its own group is allowed (the child half of the
    //    race-free fork/setpgid idiom, where BOTH parent and child call it).
    if pgrp_setpgid_decide_rs(11, 10, 11, 1, 10, 10, 11, 0) != 0 {
        fails |= 1 << 9;
    }

    // 9. A process that no longer exists is ESRCH, and specifically NOT EPERM.
    if pgrp_setpgid_decide_rs(10, 10, 99, 0, 0, 0, 99, 0) != E_SRCH {
        fails |= 1 << 10;
    }
    // 10. A process that is neither the caller nor its child is ESRCH.
    //     Target 20's parent is 19, not the caller 10.
    if pgrp_setpgid_decide_rs(10, 10, 20, 1, 19, 10, 20, 0) != E_SRCH {
        fails |= 1 << 11;
    }
    // 11. THE REACH-ACROSS THIS EXISTS TO STOP: a child that has already
    //     setsid()'d into its own session may not be dragged back. Target 11 is
    //     a real child of 10, but its session is 11, not 10.
    if pgrp_setpgid_decide_rs(10, 10, 11, 1, 10, 11, 11, 0) != E_PERM {
        fails |= 1 << 12;
    }
    // 12. A session leader may not change its own group.
    if pgrp_setpgid_decide_rs(10, 10, 10, 1, 1, 10, 5, 1) != E_PERM {
        fails |= 1 << 13;
    }
    // 13. Joining a group that does not exist in this session is EPERM. Same
    //     shape as case 7 but with the group absent, so this asserts the glue's
    //     table walk is actually consulted and not ignored.
    if pgrp_setpgid_decide_rs(10, 10, 12, 1, 10, 10, 11, 0) != E_PERM {
        fails |= 1 << 14;
    }

    // 14. Rule ORDER: a non-existent process must report ESRCH even when it
    //     would ALSO have failed the session check. If the session rule ran
    //     first, a caller could distinguish "exists in another session" from
    //     "does not exist", which leaks the existence of processes it may not
    //     touch.
    if pgrp_setpgid_decide_rs(10, 10, 99, 0, 10, 77, 99, 0) != E_SRCH {
        fails |= 1 << 15;
    }

    fails
}
