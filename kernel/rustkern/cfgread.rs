// rustkern/cfgread.rs - #192: THE decision of whether a config-file read
// outcome deserves a log line, and how loud that line should be.
//
// WHY THIS EXISTS.
//
// `/CONFIG/TZ.CFG` does not exist on a fresh image. That is the NORMAL state
// before the user picks a timezone. On golden build 2011 the serial console
// carried this, forever, in 3-attempt bursts, restarting every 2 seconds:
//
//   [FAT] fat_read_file: opening /CONFIG/TZ.CFG
//   [FAT] fat_read_file: open failed
//   [BOOTLOG] [USERS] /CONFIG/TZ.CFG: read FAILED/empty on attempt 1/3 - retrying
//   [BOOTLOG] [USERS] /CONFIG/TZ.CFG: read FAILED/empty on attempt 2/3 - retrying
//   [BOOTLOG] [USERS] /CONFIG/TZ.CFG: read FAILED/empty on attempt 3/3 - giving up
//
// 125 lines of a 3634-line capture, and "giving up" was a lie: the caller
// (gui/clock.c's 2-second timezone refresh) simply asked again. Three things
// were wrong and only one of them was the wording:
//
//   1. ABSENT WAS TREATED AS AN I/O FAULT. A directory lookup that misses is
//      DETERMINISTIC. Retrying it, with a spin backoff, cannot change the
//      answer. The retry helper (proc/users.c fat_read_file_retry, added for
//      #307 because real USB-MSC hardware returns transient short reads) was
//      built for a file that IS THERE and did not read this time. Absence is a
//      different outcome and needs a different policy.
//   2. THE NORMAL CASE SHOUTED. "read FAILED/empty ... giving up" for a
//      timezone nobody has chosen yet trains every reader to skip boot-log
//      errors, and it buried the real lines: a `tail` of that serial showed
//      nothing but this flood and a boot failure was misdiagnosed from it.
//   3. THE SAME SHAPE HIT /CONFIG/PASSWD, /CONFIG/SHADOW and /CONFIG/GROUP,
//      whose absence is likewise correct on a virgin image now that default
//      credentials are gone (#568). One helper serves all of them, so the fix
//      belongs at the chokepoint, not per file.
//
// WHAT THIS MODULE IS, AND IS NOT.
//
// It is a pure decision function over a small per-path state table: given a
// path and what just happened to it, return whether to emit a line and at what
// level. It PRINTS NOTHING. The C caller owns the wording, because the C caller
// is the one that knows what the file is for; this owns the RATE and the LEVEL,
// because that is the part that was wrong in three places at once.
//
// It deliberately does NOT mute a genuine fault. A file that is PRESENT and
// will not read is still loud: the first CFG_ERR_BURST occurrences each get a
// warning line, then one "suppressed" line so the reader knows more happened,
// and the burst RE-ARMS the moment the file reads successfully again. A
// silenced error is the failure mode this whole ticket is about, one level
// down; the cure for a flood is not a gag.
//
// STATE TRANSITIONS, not counts, drive the normal-case lines: a line is emitted
// when the outcome for a path CHANGES. So an absent file logs once per boot, a
// file that appears logs once when it appears, and a file that starts failing
// logs immediately rather than after some timer.
//
// RUST, per the standing directive (CLAUDE.md): new kernel code is Rust unless
// there is a stated performance reason. There is none here - this runs at most
// a handful of times per second and does no I/O.
//
// CONCURRENCY. The table is plain relaxed atomics, no lock. Deliberate: this is
// a LOGGING policy, and the worst outcome of a race between two callers on the
// same path is one duplicated or one dropped line. Taking a lock here would put
// a spin (or a wait-queue turnstile) on a path that boot-time, pre-scheduler
// code calls, which is the #426 shape and a far worse trade than a duplicate
// line. No `static mut`, so there is no aliasing UB either.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// ---------------------------------------------------------------------------
// The ABI. THESE VALUES ARE MIRRORED IN kernel/fs/cfgread.h AND ARE CHECKED AT
// BOOT by cfgread_abi_check_rs(), which is handed the C header's constants and
// compares them with these. A silently diverging code would turn a warning into
// a note, which is exactly the class of defect this module removes.
// ---------------------------------------------------------------------------

/// The read produced usable bytes.
pub const CFG_OUTCOME_OK: i32 = 0;
/// The path is not present on any volume the read would have consulted. NORMAL
/// for an unconfigured setting; NOT an I/O fault.
pub const CFG_OUTCOME_ABSENT: i32 = 1;
/// The path IS present and the read still failed (short read, media error,
/// allocation failure). A genuine fault.
pub const CFG_OUTCOME_IOERR: i32 = 2;

/// Say nothing.
pub const CFG_LOG_NONE: i32 = 0;
/// One informational line. A normal, expected condition worth stating once.
pub const CFG_LOG_NOTE: i32 = 1;
/// One loud line. Something that should have worked did not.
pub const CFG_LOG_WARN: i32 = 2;
/// One line saying that further warnings for this path are being withheld.
pub const CFG_LOG_SUPPRESSED: i32 = 3;

/// How many consecutive genuine failures on one path are logged in full before
/// the suppression notice. Small on purpose: four lines is enough to see a
/// pattern, few enough that a hard-failing device cannot bury the boot log.
const CFG_ERR_BURST: u32 = 4;

/// Paths tracked. Config files in this kernel number well under a dozen; 32
/// leaves room for the wallpaper/backdrop paths that also go through the retry
/// helper. Overflow evicts least-recently-used rather than going silent, so a
/// pathological caller degrades to "may repeat a line", never to "hides one".
const CFG_SLOTS: usize = 32;

/// Longest path byte-compared. Longer paths are hashed over their first
/// CFG_PATH_MAX bytes, which can only ever merge two very long sibling paths
/// into one rate-limit bucket. Bounded by construction: this parses no length
/// out of the data it is given.
const CFG_PATH_MAX: usize = 256;

const LAST_UNKNOWN: u32 = 0xFF;

struct Tab {
    key: [AtomicU64; CFG_SLOTS], // FNV-1a of the path; 0 = empty slot
    st: [AtomicU32; CFG_SLOTS],  // low 8 bits = last outcome, high bits = reps
    seq: [AtomicU64; CFG_SLOTS], // LRU stamp
    tick: AtomicU64,
}

impl Tab {
    const fn new() -> Tab {
        Tab {
            key: [const { AtomicU64::new(0) }; CFG_SLOTS],
            st: [const { AtomicU32::new(LAST_UNKNOWN) }; CFG_SLOTS],
            seq: [const { AtomicU64::new(0) }; CFG_SLOTS],
            tick: AtomicU64::new(0),
        }
    }
}

/// The live table the kernel uses.
static LIVE: Tab = Tab::new();
/// A second, identical table used ONLY by the self-test, so running the
/// self-test cannot perturb the real boot's logging decisions (and so the
/// self-test's expectations cannot be broken by whatever the boot happened to
/// read first).
static TEST: Tab = Tab::new();

fn path_hash(p: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in p {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    // 0 is the "empty slot" marker, so it can never be a live key.
    if h == 0 {
        1
    } else {
        h
    }
}

/// Find (or claim) the slot for `k`. Never fails: a full table evicts the
/// least-recently-touched entry.
fn slot_for(t: &Tab, k: u64) -> usize {
    let mut free = usize::MAX;
    let mut oldest = 0usize;
    let mut oldest_seq = u64::MAX;
    for i in 0..CFG_SLOTS {
        let kk = t.key[i].load(Ordering::Relaxed);
        if kk == k {
            return i;
        }
        if kk == 0 && free == usize::MAX {
            free = i;
        }
        let s = t.seq[i].load(Ordering::Relaxed);
        if s < oldest_seq {
            oldest_seq = s;
            oldest = i;
        }
    }
    let i = if free != usize::MAX { free } else { oldest };
    t.key[i].store(k, Ordering::Relaxed);
    t.st[i].store(LAST_UNKNOWN, Ordering::Relaxed);
    i
}

fn decide(t: &Tab, k: u64, outcome: i32) -> i32 {
    let i = slot_for(t, k);
    let tk = t.tick.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    t.seq[i].store(tk, Ordering::Relaxed);

    let cur = t.st[i].load(Ordering::Relaxed);
    let last = cur & 0xFF;
    let reps = cur >> 8;

    match outcome {
        CFG_OUTCOME_OK => {
            t.st[i].store(CFG_OUTCOME_OK as u32, Ordering::Relaxed);
            // First successful read of this file, or a recovery from absent or
            // failing. Both are worth exactly one line; a steady state of
            // "readable" is worth none.
            if last == CFG_OUTCOME_OK as u32 {
                CFG_LOG_NONE
            } else {
                CFG_LOG_NOTE
            }
        }
        CFG_OUTCOME_ABSENT => {
            t.st[i].store(CFG_OUTCOME_ABSENT as u32, Ordering::Relaxed);
            if last == CFG_OUTCOME_ABSENT as u32 {
                CFG_LOG_NONE
            } else {
                CFG_LOG_NOTE
            }
        }
        CFG_OUTCOME_IOERR => {
            let n = if last == CFG_OUTCOME_IOERR as u32 {
                let m = reps.wrapping_add(1);
                if m > CFG_ERR_BURST + 2 {
                    CFG_ERR_BURST + 2
                } else {
                    m
                }
            } else {
                1
            };
            t.st[i].store((n << 8) | CFG_OUTCOME_IOERR as u32, Ordering::Relaxed);
            if n <= CFG_ERR_BURST {
                CFG_LOG_WARN
            } else if n == CFG_ERR_BURST + 1 {
                CFG_LOG_SUPPRESSED
            } else {
                CFG_LOG_NONE
            }
        }
        _ => CFG_LOG_NONE,
    }
}

fn report_in(t: &Tab, path: *const u8, plen: i32, outcome: i32) -> i32 {
    if path.is_null() {
        // A caller bug. Never silence a fault because of it.
        return if outcome == CFG_OUTCOME_IOERR {
            CFG_LOG_WARN
        } else {
            CFG_LOG_NONE
        };
    }
    // Determine the byte length. plen < 0 means "NUL-terminated", which is what
    // every C caller passes; an explicit length is accepted so a non-terminated
    // buffer is expressible.
    let n: usize = if plen < 0 {
        let mut i = 0usize;
        // SAFETY: caller guarantees `path` is a NUL-terminated C string when it
        // passes a negative length. The scan is bounded by CFG_PATH_MAX
        // regardless, so a missing terminator cannot run away.
        while i < CFG_PATH_MAX && unsafe { *path.add(i) } != 0 {
            i += 1;
        }
        i
    } else if (plen as usize) > CFG_PATH_MAX {
        CFG_PATH_MAX
    } else {
        plen as usize
    };
    if n == 0 {
        return if outcome == CFG_OUTCOME_IOERR {
            CFG_LOG_WARN
        } else {
            CFG_LOG_NONE
        };
    }
    // SAFETY: `n` is either the measured distance to the NUL (both bounded by
    // CFG_PATH_MAX) or the caller's own declared length clamped to the same
    // bound, so all `n` bytes are within the caller's buffer.
    let s = unsafe { core::slice::from_raw_parts(path, n) };
    decide(t, path_hash(s), outcome)
}

/// Report what just happened to a config-file read; get back whether to log.
///
/// `path`   - the path that was read. NUL-terminated when `plen` < 0.
/// `plen`   - byte length, or negative for "NUL-terminated".
/// `outcome`- one of CFG_OUTCOME_*.
///
/// Returns one of CFG_LOG_*. The caller emits at most the one line it is told
/// to; the wording is the caller's, the rate is this function's.
#[no_mangle]
pub extern "C" fn cfgread_report_rs(path: *const u8, plen: i32, outcome: i32) -> i32 {
    report_in(&LIVE, path, plen, outcome)
}

/// Boot-time ABI lock: C hands in the constants from kernel/fs/cfgread.h and
/// this compares them with the Rust ones. Returns 0 when every value matches.
/// A mismatch is not cosmetic - swapping WARN and NOTE would silently downgrade
/// every genuine fault.
#[no_mangle]
pub extern "C" fn cfgread_abi_check_rs(
    ok: i32,
    absent: i32,
    ioerr: i32,
    none: i32,
    note: i32,
    warn: i32,
    supp: i32,
) -> i32 {
    let good = ok == CFG_OUTCOME_OK
        && absent == CFG_OUTCOME_ABSENT
        && ioerr == CFG_OUTCOME_IOERR
        && none == CFG_LOG_NONE
        && note == CFG_LOG_NOTE
        && warn == CFG_LOG_WARN
        && supp == CFG_LOG_SUPPRESSED;
    if good {
        0
    } else {
        -1
    }
}

/// Self-test over the policy. 0 = pass, and `*out_checks` gets the number of
/// assertions made. Provably RED via `make CFGTESTFAIL=1`.
///
/// It runs against TEST, never LIVE, so it neither disturbs nor is disturbed by
/// the boot's own reads.
#[no_mangle]
pub extern "C" fn cfgread_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    let t = &TEST;
    let rep = |p: &[u8], o: i32| -> i32 { report_in(t, p.as_ptr(), p.len() as i32, o) };

    // --- THE DEFECT ITSELF -------------------------------------------------
    // An absent file logs ONCE, however many times it is asked about. This is
    // the assertion that the 125-line TZ.CFG flood cannot come back.
    let tz = b"/CONFIG/TZ.CFG";
    chk(rep(tz, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE);
    for _ in 0..200 {
        chk(rep(tz, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);
    }

    // ...and it is a NOTE, not a WARN. The normal case must not be reported at
    // fault level; that is the half of #192 that is about honesty rather than
    // volume.
    chk(CFG_LOG_NOTE != CFG_LOG_WARN);

    // Configuring the file afterwards says so, once.
    chk(rep(tz, CFG_OUTCOME_OK) == CFG_LOG_NOTE);
    chk(rep(tz, CFG_OUTCOME_OK) == CFG_LOG_NONE);
    chk(rep(tz, CFG_OUTCOME_OK) == CFG_LOG_NONE);
    // Deleting it again is a state change, so it is stated again, once.
    chk(rep(tz, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE);
    chk(rep(tz, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);

    // --- A GENUINE FAULT IS STILL LOUD -------------------------------------
    // The whole risk of this change is muting a real error. A file that is
    // PRESENT and will not read gets a full burst of warnings, then ONE line
    // saying the rest are withheld, then silence - and it re-arms on recovery.
    let bad = b"/CONFIG/PASSWD";
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_WARN); // == CFG_ERR_BURST
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_SUPPRESSED);
    for _ in 0..50 {
        chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_NONE);
    }
    chk(rep(bad, CFG_OUTCOME_OK) == CFG_LOG_NOTE); // recovered
    chk(rep(bad, CFG_OUTCOME_IOERR) == CFG_LOG_WARN); // re-armed, loud again

    // A first failure is loud IMMEDIATELY. No warm-up, no timer.
    let bad2 = b"/CONFIG/SHADOW";
    chk(rep(bad2, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);

    // --- PATHS DO NOT SHARE STATE ------------------------------------------
    // A quiet, absent TZ.CFG must not silence a failing PASSWD, and vice versa.
    let g = b"/CONFIG/GROUP";
    chk(rep(g, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE);
    chk(rep(g, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);
    chk(rep(bad2, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    chk(rep(tz, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);

    // --- LENGTH HANDLING ---------------------------------------------------
    // A NUL-terminated call and an explicit-length call must land on the SAME
    // slot, or every C caller would get a fresh "first sighting" every time.
    let nul = b"/CONFIG/LOGIN.CFG\0";
    chk(report_in(t, nul.as_ptr(), -1, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE);
    chk(report_in(t, nul.as_ptr(), 17, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);
    // A different path is a different slot.
    let other = b"/CONFIG/LOGIN.CFX";
    chk(report_in(t, other.as_ptr(), 17, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE);
    // Degenerate inputs never silence a fault.
    chk(report_in(t, core::ptr::null(), -1, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    chk(report_in(t, core::ptr::null(), -1, CFG_OUTCOME_ABSENT) == CFG_LOG_NONE);
    chk(report_in(t, nul.as_ptr(), 0, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);

    // --- TABLE OVERFLOW ----------------------------------------------------
    // More distinct paths than slots must not wedge, and must not start
    // answering NONE for a first sighting. Fill well past CFG_SLOTS.
    let mut buf = *b"/CONFIG/OVFL0000";
    for i in 0..(CFG_SLOTS * 3) {
        buf[12] = b'0' + ((i / 1000) % 10) as u8;
        buf[13] = b'0' + ((i / 100) % 10) as u8;
        buf[14] = b'0' + ((i / 10) % 10) as u8;
        buf[15] = b'0' + (i % 10) as u8;
        chk(report_in(t, buf.as_ptr(), buf.len() as i32, CFG_OUTCOME_IOERR) == CFG_LOG_WARN);
    }

    // An unknown outcome code is not a licence to invent a line.
    chk(rep(tz, 99) == CFG_LOG_NONE);

    // --- THE ABI LOCK ITSELF -----------------------------------------------
    chk(cfgread_abi_check_rs(0, 1, 2, 0, 1, 2, 3) == 0);
    chk(cfgread_abi_check_rs(0, 1, 2, 0, 2, 1, 3) != 0); // NOTE/WARN swapped

    // The DELIBERATE failure, so this line has been WATCHED to go red.
    #[cfg(cfg_test_fail)]
    chk(rep(b"/CONFIG/NEVER", CFG_OUTCOME_ABSENT) == CFG_LOG_WARN);

    if !out_checks.is_null() {
        // SAFETY: null-checked; caller passes the address of a u32 local.
        unsafe { *out_checks = n };
    }
    if ok {
        0
    } else {
        -1
    }
}
