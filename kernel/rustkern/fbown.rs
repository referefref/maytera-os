// rustkern/fbown.rs - #745 (task #59): OWNERSHIP AND LIFETIME of the single
// framebuffer claim that gui/fb_syscall.c calls "the compositor latch".
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, and the same shape as fetchown.rs/elevate.rs: the INPUTS
// come from existing, unchanged C (proc_current(), proc_get() liveness) and
// the DECISION lives here.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED ON dev BEFORE THE CHANGE
// ---------------------------------------------------------------------------
// gui/fb_syscall.c held `uint32_t compositor_pid` and latched it to the pid of
// the FIRST process ever to call sys_fb_map(). The ONLY place that ever wrote
// it back to 0 was fb_syscall_init(), which runs ONCE, at kernel boot.
//
// Switch User and Log Out both work by exiting /APPS/COMPOSIT. So the moment
// the first compositor of a boot exited, the latch still held its DEAD pid,
// and every later relaunch was refused with
//     "[FB] ERROR: Non-compositor process tried to map framebuffer"
// exit(1), straight back to the login gate. With the shipped autologin=root
// the gate re-authenticates instantly and relaunches, so the failure is not a
// dead feature but an INFINITE CRASH-RESPAWN LOOP: 59 respawns and 57 FB-map
// failures in ~40 s with idle at 0%, measured on golden build 1851.
//
// ===========================================================================
// WHY A STATE MACHINE AND NOT "SET IT BACK TO 0 ON EXIT"
// ---------------------------------------------------------------------------
// Clearing the latch on exit is necessary but NOT sufficient, because the
// claim rule underneath it is "first caller wins". At boot that is nearly
// harmless: the compositor is the first Ring 3 process there is. At the LOGIN
// GATE after a Log Out it is not: the autostarted background services and any
// app the departing session left running are still alive Ring 3 processes, and
// the framebuffer claim is not merely a drawing permission. proc/elevate.c
// admits SYS_ELEV_VIEW / SYS_ELEV_RESOLVE - reading a pending elevation
// request and SUBMITTING THE PASSWORD FOR IT - to whoever holds this latch.
// So a naive re-arm would trade a liveness bug for a privilege one.
//
// The rule here is therefore an explicitly ARMED, single-owner claim:
//
//   ARM     gui/desktop.c opens the window immediately BEFORE it launches
//           /APPS/COMPOSIT, and narrows it to the returned pid immediately
//           after. Arming BEFORE the launch is what makes it race-free: the
//           child cannot run (and cannot claim) before the window exists.
//           The narrowing call is a no-op if the compositor already claimed
//           inside that microscopic window, so the two calls cannot fight.
//   CLAIM   succeeds only while ARMED, and only for the expected pid once one
//           is known. It closes the window behind itself.
//   RELEASE happens at the process-exit chokepoint (proc/process.c's
//           proc_exit(), beside cleanup_user_windows_for_process(),
//           audio_pcm_proc_exit() and async_http_proc_exit()) and DISARMS.
//
// The important consequence: between the compositor dying and the kernel
// deciding to launch the next one, NOBODY can claim the framebuffer. The
// window is not merely narrow, it is shut. That is strictly stronger than the
// code this replaces, which left "first caller wins" open for the whole boot
// until the compositor happened to be the one to use it.
//
// A stale-owner sweep exists as a BACKSTOP only (fbown_note_stale_rs, driven
// by a liveness check C makes with proc_get): if some teardown path ever fails
// to reach proc_exit(), a dead pid must not be able to hold the screen hostage
// until reboot. It clears the owner; it does NOT arm. So the backstop can
// never hand the framebuffer to a process the kernel did not launch as the
// compositor.
//
// pid REUSE: proc/process.c allocates pids with a monotonic `next_pid++`
// (uint32_t, reset only by proc_init at boot), so a pid is not reused until it
// wraps 2^32 process creations. Reuse is therefore not the near-term risk that
// staleness is - but "not reachable today" is not a property to lean on, and
// both the exit release and the liveness backstop remove the question rather
// than argue about it.
// ===========================================================================

use core::sync::atomic::{AtomicU32, Ordering};

// The live claim. 0 = unclaimed.
static OWNER: AtomicU32 = AtomicU32::new(0);
// The pid permitted to claim while ARMED. 0 = "the next claimant" (the
// pre-launch half of the two-step arm, open for microseconds only).
static EXPECT: AtomicU32 = AtomicU32::new(0);
// Is the claim window open at all? Nothing can claim while this is 0.
static ARMED: AtomicU32 = AtomicU32::new(0);

// Audit counters. Read back by fbown_stats_rs() and printed on serial, which
// is how a guard is OBSERVED FIRING rather than assumed to (blame.md: "a guard
// that has never been watched firing is indistinguishable from one that is
// switched off").
static N_CLAIMS: AtomicU32 = AtomicU32::new(0);
static N_RELEASES: AtomicU32 = AtomicU32::new(0);
static N_STALE: AtomicU32 = AtomicU32::new(0);
static N_REF_UNARMED: AtomicU32 = AtomicU32::new(0);
static N_REF_NOTEXPECTED: AtomicU32 = AtomicU32::new(0);
static N_REF_NOTOWNER: AtomicU32 = AtomicU32::new(0);

/// Mirrors fbown_stats_t in gui/fbown.h. sizeof-locked on both sides.
#[repr(C)]
pub struct FbownStats {
    pub owner: u32,
    pub expect: u32,
    pub armed: u32,
    pub claims: u32,
    pub releases: u32,
    pub stale_cleared: u32,
    pub refused_unarmed: u32,
    pub refused_not_expected: u32,
    pub refused_not_owner: u32,
}

const _: () = assert!(core::mem::size_of::<FbownStats>() == 36);

/// Open (or narrow) the claim window.
///
/// `pid == 0` means "the next claimant", which is only ever used for the
/// instant between opening the window and learning the launched pid.
/// Returns 1 if the window is now open for `pid`, 0 if there is already an
/// owner (in which case NOTHING is changed: a second arm must never be able to
/// evict or reopen a live claim).
#[no_mangle]
pub extern "C" fn fbown_arm_rs(pid: u32) -> i32 {
    if OWNER.load(Ordering::SeqCst) != 0 {
        return 0;
    }
    EXPECT.store(pid, Ordering::SeqCst);
    ARMED.store(1, Ordering::SeqCst);
    1
}

/// Shut the claim window without granting it. Used when the compositor launch
/// itself failed: leaving the window open would be the old first-caller-wins
/// rule with extra steps.
#[no_mangle]
pub extern "C" fn fbown_disarm_rs() -> i32 {
    if OWNER.load(Ordering::SeqCst) != 0 {
        return 0;
    }
    ARMED.store(0, Ordering::SeqCst);
    EXPECT.store(0, Ordering::SeqCst);
    1
}

/// Try to become the framebuffer owner. Returns 1 if `pid` owns it after this
/// call (including the "already owned it" case), 0 otherwise.
#[no_mangle]
pub extern "C" fn fbown_claim_rs(pid: u32) -> i32 {
    if pid == 0 {
        return 0;
    }
    let owner = OWNER.load(Ordering::SeqCst);
    if owner == pid {
        return 1;
    }
    if owner != 0 {
        N_REF_NOTOWNER.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    if ARMED.load(Ordering::SeqCst) == 0 {
        N_REF_UNARMED.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    let expect = EXPECT.load(Ordering::SeqCst);
    if expect != 0 && expect != pid {
        N_REF_NOTEXPECTED.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    match OWNER.compare_exchange(0, pid, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            // Close the window behind us: the claim is single-owner, so a
            // second claimant must be refused even if it was also expected.
            ARMED.store(0, Ordering::SeqCst);
            EXPECT.store(0, Ordering::SeqCst);
            N_CLAIMS.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(now) => {
            // Lost the race to another core. Whoever won owns it.
            if now == pid {
                1
            } else {
                N_REF_NOTOWNER.fetch_add(1, Ordering::SeqCst);
                0
            }
        }
    }
}

/// Does `pid` own the framebuffer right now? 1/0. This is the whole of the
/// "is this the compositor" question, and proc/elevate.c asks it too.
#[no_mangle]
pub extern "C" fn fbown_is_owner_rs(pid: u32) -> i32 {
    if pid != 0 && OWNER.load(Ordering::SeqCst) == pid {
        1
    } else {
        0
    }
}

/// The owning pid, or 0 if unclaimed.
#[no_mangle]
pub extern "C" fn fbown_owner_rs() -> u32 {
    OWNER.load(Ordering::SeqCst)
}

/// Release the claim held by `pid` and DISARM. Returns 1 if this call is what
/// released it, 0 if `pid` did not hold it (the overwhelmingly common case:
/// every other process in the system also runs this on exit).
#[no_mangle]
pub extern "C" fn fbown_release_rs(pid: u32) -> i32 {
    if pid == 0 {
        return 0;
    }
    match OWNER.compare_exchange(pid, 0, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            ARMED.store(0, Ordering::SeqCst);
            EXPECT.store(0, Ordering::SeqCst);
            N_RELEASES.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(_) => 0,
    }
}

/// Count a backstop release: the owner was found dead by a liveness check
/// rather than by its own proc_exit(). Kept separate from N_RELEASES on
/// purpose - a non-zero value here means a teardown path is bypassing the
/// exit hook, which is a bug report, not routine.
#[no_mangle]
pub extern "C" fn fbown_note_stale_rs() {
    N_STALE.fetch_add(1, Ordering::SeqCst);
}

/// Boot-time only: return to the cold state. Called from fb_syscall_init().
#[no_mangle]
pub extern "C" fn fbown_reset_rs() {
    OWNER.store(0, Ordering::SeqCst);
    EXPECT.store(0, Ordering::SeqCst);
    ARMED.store(0, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn fbown_stats_rs(out: *mut FbownStats) -> i32 {
    if out.is_null() {
        return -1;
    }
    let s = FbownStats {
        owner: OWNER.load(Ordering::SeqCst),
        expect: EXPECT.load(Ordering::SeqCst),
        armed: ARMED.load(Ordering::SeqCst),
        claims: N_CLAIMS.load(Ordering::SeqCst),
        releases: N_RELEASES.load(Ordering::SeqCst),
        stale_cleared: N_STALE.load(Ordering::SeqCst),
        refused_unarmed: N_REF_UNARMED.load(Ordering::SeqCst),
        refused_not_expected: N_REF_NOTEXPECTED.load(Ordering::SeqCst),
        refused_not_owner: N_REF_NOTOWNER.load(Ordering::SeqCst),
    };
    unsafe { core::ptr::write(out, s) };
    0
}

/// Boot self-test. Returns 0 on success, or the number of the first failing
/// case (negative), so a failure names WHICH rule broke rather than "false".
///
/// Case 5 is the whole point of this change and the one that would have caught
/// the shipped bug: after a release, a claim must be REFUSED until the kernel
/// arms again. The old code's equivalent state (compositor_pid still holding a
/// dead pid) refused too, but for the wrong reason and forever.
#[no_mangle]
pub extern "C" fn fbown_selftest_rs() -> i32 {
    // Refuse to run against live state rather than corrupt it.
    if OWNER.load(Ordering::SeqCst) != 0 || ARMED.load(Ordering::SeqCst) != 0 {
        return -100;
    }

    let mut rc: i32 = 0;

    // 1. Cold: nothing is armed, so nothing may claim.
    if fbown_claim_rs(7) != 0 {
        rc = -1;
    }

    // 2. Armed for pid 7: 9 is refused, 7 is granted.
    if rc == 0 {
        fbown_arm_rs(0);
        fbown_arm_rs(7);
        if fbown_claim_rs(9) != 0 {
            rc = -2;
        } else if fbown_claim_rs(7) != 1 {
            rc = -2;
        } else if fbown_owner_rs() != 7 {
            rc = -2;
        }
    }

    // 3. A live claim cannot be re-armed away, and a non-owner is refused.
    if rc == 0 {
        if fbown_arm_rs(9) != 0 {
            rc = -3;
        } else if fbown_claim_rs(9) != 0 {
            rc = -3;
        } else if fbown_is_owner_rs(7) != 1 || fbown_is_owner_rs(9) != 0 {
            rc = -3;
        }
    }

    // 4. Only the owner's own release releases it.
    if rc == 0 {
        if fbown_release_rs(9) != 0 {
            rc = -4;
        } else if fbown_release_rs(7) != 1 {
            rc = -4;
        } else if fbown_owner_rs() != 0 {
            rc = -4;
        }
    }

    // 5. THE FIX: released also means DISARMED. The gate window is shut.
    if rc == 0 && fbown_claim_rs(7) != 0 {
        rc = -5;
    }

    // 6. Re-arming after a release works, which is the other half of the fix:
    //    the next compositor of the boot can claim.
    if rc == 0 {
        fbown_arm_rs(0);
        fbown_arm_rs(5);
        if fbown_claim_rs(5) != 1 {
            rc = -6;
        } else if fbown_release_rs(5) != 1 {
            rc = -6;
        }
    }

    // 7. disarm() shuts a window that was never used (failed launch).
    if rc == 0 {
        fbown_arm_rs(0);
        if fbown_disarm_rs() != 1 {
            rc = -7;
        } else if fbown_claim_rs(3) != 0 {
            rc = -7;
        }
    }

    // Leave the machine cold and the counters at zero: the self-test's own
    // refusals must not be mistaken for a real guard firing later.
    fbown_reset_rs();
    N_CLAIMS.store(0, Ordering::SeqCst);
    N_RELEASES.store(0, Ordering::SeqCst);
    N_STALE.store(0, Ordering::SeqCst);
    N_REF_UNARMED.store(0, Ordering::SeqCst);
    N_REF_NOTEXPECTED.store(0, Ordering::SeqCst);
    N_REF_NOTOWNER.store(0, Ordering::SeqCst);

    rc
}
