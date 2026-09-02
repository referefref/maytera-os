// rustkern/dosdisp.rs - DO NOT COMPUTE A FRAME NOBODY WILL EVER SEE (no-ticket).
//
// New kernel logic, so Rust per the 2026-07-16 rule. It is one comparison per
// DOS frame, not per pixel and not per instruction, so there is no performance
// argument for C and none is claimed.
//
// ===========================================================================
// THE DEFECT, MEASURED BEFORE IT WAS FIXED
// ---------------------------------------------------------------------------
// A DOS guest presents every DOS_PRESENT_MS (14 ms, ~70 Hz). The SCREEN is
// presented far less often than that, and on one core it is not close.
// MEASURED on golden 2259 at a real 2560x1600 framebuffer, Aladdin maximised,
// with the new [DOSFRAME] profile and the pre-existing [COMPIDLE] line taken
// over the same interval:
//
//     DOS presents            ~65 per second
//     framebuffer presents     13-16 per second  ([FLIPPROF] flips)
//     compositor ticks         407 in 30 s = 13/s, busy=27%, maxtick 32.5 ms
//
// So roughly four out of every five frames the guest painted, this kernel
// scaled, published and marked dirty were REPLACED by the next one before the
// compositor ever composited them. In the same interval that waste came to
// 11-12% of one core in the publish (uw_commit_content's full-window memcpy)
// plus 4.4-4.8% in the scaler, against an interpreter that had 79-81%.
//
// The cost is not the guest's and it does not shrink when the guest's
// resolution does: publish is content_width * content_height * 4 bytes per
// frame no matter whether the guest is 320x200 or 640x480, which is exactly
// why three different titles in three different video modes were all reported
// as slow to the same degree.
//
// ===========================================================================
// WHY A FRAME COUNTER AND NOT A TIMER
// ---------------------------------------------------------------------------
// The obvious fix is to slow the present cadence to a constant. That is the
// mistake #232's own pacing notes already record: two constants multiplied
// together are wrong at every load they were not measured at, and the right
// cadence here is a property of the HOST's current composite cost, the
// resolution, and what else is running - i.e. exactly the thing a constant
// cannot know. At 1280x800 with an idle desktop the compositor manages 30 Hz;
// at 3840x2160 with a DOS guest on one core the owner's machine manages
// single figures.
//
// So the signal is the compositor's OWN progress: g_fb_flip_count
// (gui/fb_syscall.c), the monotonic count of framebuffer presents that already
// exists and is already read by [FLIPPROF]. If it has not moved since the last
// frame we published, that frame has not reached the screen and a new one can
// only overwrite it. Publishing again buys the user nothing and costs a
// full-window memcpy.
//
// THIS IS A SKIP, NOT A WAIT (#426). Nothing blocks, nothing polls, nothing
// sleeps and no condition is retried: the run loop asks a question, gets a
// yes or a no, and carries straight on interpreting either way. The time
// saved goes back to the same core the compositor is starving on, which is
// the point - the aim is a HIGHER visible frame rate, not merely a lower CPU
// figure.
//
// THE STALENESS FLOOR IS NOT A TIMEOUT DRESSED UP. The flip counter can
// legitimately stop moving for reasons that have nothing to do with us: the
// window is fully occluded, the screensaver owns the framebuffer, the
// compositor is only serving a remote viewer. In those states the guest must
// still refresh eventually, or a window that becomes visible again shows a
// stale picture until something else happens to flip. DOSDISP_STALE_MS is the
// longest a frame may be withheld on that account, and it is deliberately far
// longer than a frame: it is a correctness backstop for the not-being-
// displayed case, not a cadence.

/// Longest a frame may be withheld when the framebuffer is not flipping at
/// all. 200 ms = 5 Hz, slow enough that it can never be the thing setting the
/// visible rate on a working machine, fast enough that a picture uncovered by
/// a window move is never visibly stale.
pub const DOSDISP_STALE_MS: u64 = 200;

// ===========================================================================
// WHERE `flips` COMES FROM, AND WHY A RESET CANNOT WEDGE THIS (#flipfix)
// ---------------------------------------------------------------------------
// The caller reads it through dos/dosexec.c's win16_host_flip_count(), which is
// the same gui/fb_syscall.c counter in Ring 0 and the same counter fetched with
// SYS_FB_FLIP_COUNT in the Ring-3 DOS host. It used to be read as a variable,
// which the Ring-3 host could only satisfy with a stub that nothing ever wrote:
// a constant 0, so `flips != s.last_flips` was false on every frame for the life
// of the process and DOSDISP_STALE_MS became the cadence at 5.005 flips/s
// against 24.98 in-kernel. The backstop below says in its own words that it must
// never be the thing setting the visible rate; for three months on that path it
// was the ONLY thing setting it.
//
// THE TEST IS INEQUALITY, NOT GROWTH, and that is load-bearing rather than
// incidental. A counter that resets to 0, jumps, or goes backwards - a
// compositor that exits and relaunches, a kernel that ever restarts its
// framebuffer accounting - reads as DIFFERENT, so it presents. The only reading
// that can withhold a frame is one that does not CHANGE, which is exactly the
// "nothing is reaching the screen" case the staleness floor exists for. So there
// is no failure mode where a bad counter wedges the gate shut; the worst a
// broken source can do is make this present every frame, i.e. the behaviour from
// before the gate existed. That is the right direction for an instrument to fail
// in, and the Ring-3 shim leans on it deliberately: if the running kernel has no
// SYS_FB_FLIP_COUNT, it returns a value that differs on every call.
//
// It also means the counter needs no monotonicity guarantee from its source and
// none is asserted here. Do not "fix" this into a `flips > s.last_flips`
// comparison: that WOULD wedge on a reset, and it would buy nothing.
// ===========================================================================

/// Mirrored by `dosdisp_state_t` in dos/dosexec.c with a _Static_assert on the
/// size, the same contract as vbe_present_t and dos_view_policy_t. C owns the
/// one instance; the logic lives here.
#[repr(C)]
pub struct DosDispState {
    /// g_fb_flip_count as it stood when we last published a frame.
    /// u64::MAX means "nothing published yet", which is a value the real
    /// counter cannot reach in any plausible uptime and so needs no separate
    /// flag to distinguish from a genuine reading.
    pub last_flips: u64,
    pub last_present_ms: u64,
    pub presented: u64,
    pub skipped: u64,
}

#[no_mangle]
pub extern "C" fn dosdisp_reset_rs(st: *mut DosDispState) {
    if st.is_null() {
        return;
    }
    // SAFETY: non-null, checked; the C caller passes the address of its single
    // static dosdisp_state_t.
    let s = unsafe { &mut *st };
    s.last_flips = u64::MAX;
    s.last_present_ms = 0;
    s.presented = 0;
    s.skipped = 0;
}

/// 1 = present this frame, 0 = skip it.
///
/// `force` is the caller's "this frame is not optional" flag: a pending buffer
/// handover after a resize, or a halted guest whose last picture is the only
/// one there will ever be. Those must never be withheld, and the decision is
/// made HERE rather than by the caller skipping the call, so that a forced
/// frame still updates the state and cannot leave the counter pointing at a
/// frame that was superseded.
///
/// A null state PRESENTS. An instrument must never be the reason a picture
/// stops appearing.
#[no_mangle]
pub extern "C" fn dosdisp_should_present_rs(st: *mut DosDispState, enabled: i32,
                                            force: i32, flips: u64, now_ms: u64) -> i32 {
    if st.is_null() {
        return 1;
    }
    // SAFETY: non-null, checked.
    let s = unsafe { &mut *st };
    let go = if enabled == 0 || force != 0 {
        true
    } else if s.last_flips == u64::MAX {
        true                                  // nothing published yet
    } else if flips != s.last_flips {
        true                                  // the screen moved on: ours was shown
    } else {
        // wrapping_sub, not a plain subtract: sched_now_ms() is monotonic today
        // but a caller that ever hands this a going-backwards clock must get a
        // present, not a 584-million-year withhold.
        now_ms.wrapping_sub(s.last_present_ms) >= DOSDISP_STALE_MS
    };
    if go {
        s.last_flips = flips;
        s.last_present_ms = now_ms;
        s.presented = s.presented.wrapping_add(1);
        1
    } else {
        s.skipped = s.skipped.wrapping_add(1);
        0
    }
}

/// Self-test: every arm of the decision, including the two that must never
/// withhold a frame. Returns failed checks, 0 = pass. Wired into the same
/// /CONFIG/DOSSPEED.CFG arming as the [DOSFRAME] profile, so this is a test
/// that has been watched go green rather than one that merely compiles.
#[no_mangle]
pub extern "C" fn dosdisp_selftest_rs() -> i32 {
    let mut bad = 0i32;
    let mut s = DosDispState { last_flips: 0, last_present_ms: 0, presented: 0, skipped: 0 };
    dosdisp_reset_rs(&mut s);
    if s.last_flips != u64::MAX { bad += 1; }

    // First frame always goes out, whatever the flip counter says.
    if dosdisp_should_present_rs(&mut s, 1, 0, 100, 1000) != 1 { bad += 1; }
    if s.last_flips != 100 || s.presented != 1 { bad += 1; }

    // Screen has not flipped and it is not stale yet: SKIP, and the recorded
    // flip count must NOT move (or the next comparison is against a frame that
    // was never published).
    if dosdisp_should_present_rs(&mut s, 1, 0, 100, 1010) != 0 { bad += 1; }
    if s.skipped != 1 || s.last_flips != 100 || s.last_present_ms != 1000 { bad += 1; }

    // Screen flipped: present.
    if dosdisp_should_present_rs(&mut s, 1, 0, 101, 1020) != 1 { bad += 1; }
    if s.last_flips != 101 || s.presented != 2 { bad += 1; }

    // Not flipping at all, but now past the staleness floor: present anyway.
    if dosdisp_should_present_rs(&mut s, 1, 0, 101, 1020 + DOSDISP_STALE_MS - 1) != 0 { bad += 1; }
    if dosdisp_should_present_rs(&mut s, 1, 0, 101, 1020 + DOSDISP_STALE_MS) != 1 { bad += 1; }

    // force: present even when the screen has not flipped and nothing is stale.
    let before = s.presented;
    if dosdisp_should_present_rs(&mut s, 1, 1, s.last_flips, s.last_present_ms) != 1 { bad += 1; }
    if s.presented != before + 1 { bad += 1; }

    // disabled: every call presents, which is the byte-for-byte old behaviour.
    dosdisp_reset_rs(&mut s);
    for i in 0..8u64 {
        if dosdisp_should_present_rs(&mut s, 0, 0, 7, 5000 + i) != 1 { bad += 1; }
    }
    if s.skipped != 0 || s.presented != 8 { bad += 1; }

    // A clock that goes backwards must present, not withhold for an age.
    dosdisp_reset_rs(&mut s);
    dosdisp_should_present_rs(&mut s, 1, 0, 5, 10_000);
    if dosdisp_should_present_rs(&mut s, 1, 0, 5, 9_000) != 1 { bad += 1; }

    // A null state presents rather than stopping the picture.
    if dosdisp_should_present_rs(core::ptr::null_mut(), 1, 0, 0, 0) != 1 { bad += 1; }
    dosdisp_reset_rs(core::ptr::null_mut());   // must not fault
    bad
}
