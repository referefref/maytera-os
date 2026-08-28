// rustkern/sessend.rs - #126: WHAT A LOGIN BOUNDARY MEANS, i.e. which
// processes belong to the session that just ended and must not outlive it.
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, in the same shape as fbown.rs / pgrp.rs / sessionid.rs: the
// INPUTS come from existing, unchanged C (the process table walk in
// gui/desktop.c, which owns proc_get()/MAX_PROCESSES) and the DECISION lives
// here, so it is testable at boot with no processes and no scheduler, which
// sessend_selftest_rs() does.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED ON dev BEFORE THE CHANGE (build 1901)
// ---------------------------------------------------------------------------
// A login was not a session boundary. Log Out and Switch User both work by
// exiting /APPS/COMPOSIT (startmenu.c sm_power_confirm_yes case 3,
// lockscreen.c "Switch User"); gui/desktop.c notices the compositor process is
// gone and returns, and main.c's login-gate loop re-runs login_run(). NOTHING
// anywhere in that path touched the OTHER processes of the departing session.
//
// They are ordinary Ring 3 processes with their own kernel windows, so they
// survive the gate, the new compositor composites them, and the incoming user
// inherits a live, focusable, INTERACTIVE window belonging to a process
// running as the OUTGOING user. On the shipped autologin=root golden the
// outgoing user is uid 0, so what the second user inherits is a root process.
// perms_check() returning 0 for uid 0 masks the consequence today; it stops
// masking it the moment permissions are real (#20).
//
// ===========================================================================
// ATTRIBUTION: WHY THE session FIELD, AND WHY IT NEEDED ONE LINE TO BE USEFUL
// ---------------------------------------------------------------------------
// There was no usable session id to kill by. process_t carries a session field
// (Phase D4, proc/process.h) and proc_create seeds it as
//     proc->session = creator->session ? creator->session : proc->pid
// so it is INHERITED, correctly, all the way down a spawn tree. But the
// compositor is spawned by the kernel desktop thread, so it inherited THAT
// thread's session, and so did every service and every kernel worker: one
// session id for the entire machine, which distinguishes nothing.
//
// The fix is not a new field. gui/desktop.c makes the compositor a session
// LEADER at the moment it launches it (session = pgrp = its own pid), which is
// exactly what a login manager does and exactly what setsid() would do if the
// compositor could call it for itself before its first child. Everything the
// compositor spawns then inherits that id through the existing, unchanged
// inheritance rule, including grandchildren (terminal -> msh -> app), and
// everything the KERNEL spawns (background services, workers) keeps the kernel
// thread's id and is therefore not in the session.
//
// PPID WAS CONSIDERED AND IS WRONG. The teardown necessarily runs AFTER the
// compositor has exited, and proc_exit() reparents orphans, so by the time
// there is anything to decide the tree that would have identified them is
// already gone. The session id is stable across the death of the leader; a
// parent pointer is not.
//
// ===========================================================================
// WHAT IS DELIBERATELY NOT KILLED
// ---------------------------------------------------------------------------
//   * Kernel threads (privilege != 3). The idle task, the autorun worker, the
//     xHCI rescan worker and the network pump are not anybody's session.
//   * The session LEADER itself. It is the compositor, it is already dead, and
//     it is what the caller detected in order to get here. Signalling a
//     zombie is a no-op at best and confuses the log at worst.
//   * Anything in a DIFFERENT session: background services (svc_autostart runs
//     from the kernel desktop thread, once per boot), and the next session's
//     own processes if this ever runs late.
//   * Already-dead slots (UNUSED / ZOMBIE).
// The compositor of the NEXT session cannot be caught by this even in
// principle: it does not exist yet when the teardown runs, and when it does it
// is a new session leader with its own id.
// ===========================================================================

/// Process states, mirrored from proc/process.h process_state_t. Mirrored
/// rather than shared because the glue passes the value as a plain u32 and a
/// silent renumbering on the C side must not silently change a kill decision:
/// the two constants that matter are named here so a mismatch is greppable.
pub const ST_UNUSED: u32 = 0;
pub const ST_ZOMBIE: u32 = 5;

/// PRIV_USER from proc/process.h. Ring 3 is the only privilege a session
/// member can have.
pub const PRIV_USER: u32 = 3;

/// Should this process be terminated because the leader's session has ended?
///
/// Pure: every input is supplied by the caller's table walk. Returns 1 for
/// kill, 0 for keep, so the C glue reads as a filter and there is exactly one
/// place where the rule lives.
///
/// `leader` is the session id of the session that ended, which is also the pid
/// of its leader (the compositor). A zero leader is REFUSED rather than
/// treated as a wildcard: 0 is what an unset session id looks like, and a
/// wildcard here would kill every Ring 3 process on the machine.
#[no_mangle]
pub extern "C" fn sessend_should_kill_rs(leader: u32, pid: u32, session: u32,
                                         privilege: u32, state: u32) -> i32 {
    if leader == 0 { return 0; }          // never a wildcard
    if pid == 0 { return 0; }             // pid 0 is the idle task
    if pid == leader { return 0; }        // the leader is the corpse we came from
    if session != leader { return 0; }    // a different session, or none
    if privilege != PRIV_USER { return 0; }
    if state == ST_UNUSED || state == ST_ZOMBIE { return 0; }
    1
}

/// Boot self-test. Proves BOTH DIRECTIONS of every clause: a guard that never
/// fires and a guard that is absent produce identical evidence on the happy
/// path, so each rule is exercised with an input it must accept and an input
/// it must refuse. Returns 0 on success, or the 1-based index of the first
/// failing case.
#[no_mangle]
pub extern "C" fn sessend_selftest_rs() -> i32 {
    // (leader, pid, session, privilege, state, expected)
    const CASES: [(u32, u32, u32, u32, u32, i32); 12] = [
        // The case the feature exists for: a Ring 3 app of the dead session.
        (19, 42, 19, PRIV_USER, 1, 1),
        (19, 42, 19, PRIV_USER, 2, 1),
        // The leader itself is never killed, in any state.
        (19, 19, 19, PRIV_USER, 5, 0),
        (19, 19, 19, PRIV_USER, 1, 0),
        // A different session is never touched (a background service).
        (19, 55, 3, PRIV_USER, 1, 0),
        // No session id at all.
        (19, 55, 0, PRIV_USER, 1, 0),
        // Kernel threads are not session members.
        (19, 55, 19, 0, 1, 0),
        // Dead slots.
        (19, 55, 19, PRIV_USER, ST_UNUSED, 0),
        (19, 55, 19, PRIV_USER, ST_ZOMBIE, 0),
        // A zero leader is a refusal, not a wildcard: with the wildcard bug
        // this line would kill an innocent Ring 3 process in session 0.
        (0, 55, 0, PRIV_USER, 1, 0),
        (0, 55, 19, PRIV_USER, 1, 0),
        // The idle task, even if it somehow carried the session id.
        (19, 0, 19, PRIV_USER, 1, 0),
    ];
    let mut i = 0usize;
    while i < CASES.len() {
        let (l, p, s, pv, st, want) = CASES[i];
        if sessend_should_kill_rs(l, p, s, pv, st) != want { return (i + 1) as i32; }
        i += 1;
    }
    0
}
