// rustkern/selftestreg.rs - #PERMSKIP: the register of self-test groups that
// DID NOT RUN.
//
// WHY THIS EXISTS
// ---------------
// fs/perms.c perms_selftest() printed this on every boot, of every machine,
// forever:
//
//     [PERMS-SELFTEST] SKIP traversal vectors (/HOME/ADMIN not 1000:0750)
//
// It reads like ordinary boot noise about an unprovisioned image. It is not.
// The wizard names the first account, so a machine whose owner is called
// anything other than "admin" has a home at /HOME/<THAT NAME> and NEVER has
// /HOME/ADMIN. MEASURED on golden build 2234: the shipped /CONFIG/PERMS.DB
// holds three entries (/CONFIG/SHADOW, /HOME, /CONFIG) and /CONFIG/PASSWD is
// EMPTY, so the vectors cannot run before provisioning either. The
// directory-traversal half of the permission model - the half #674 was written
// to add, the half that stops a 0750 home from leaking to another account -
// has therefore probably never been exercised on any real user's machine, and
// the kernel said so once per boot in a line indistinguishable from a pass.
//
// This is the project's most-repeated shape: a harness armed by a marker no
// image carries (blame.md, the concurrency lint that could not fail, the
// invariant gate that never ran, the counter structurally incapable of seeing
// its own fault). The fix for the instance is to stop pinning a constant. The
// fix for the MECHANISM is this module: a self-test that declines to run has
// to say so THROUGH A REGISTER, the register is summarised once per boot on a
// DURABLE sink, and a build gate (kernel/tools/skipgate) refuses to compile a
// *_selftest that prints a SKIP without going through it.
//
// WHY RUST
// --------
// New kernel code is Rust unless there is a stated performance reason
// (CLAUDE.md). There is none: this runs a handful of times per boot. The work
// is bounded copying of caller-supplied C strings into fixed buffers, which is
// the exact shape that keeps producing truncation and overflow bugs in C, and
// every copy below is a bounds-checked slice write that NUL-terminates by
// construction.
//
// CONCURRENCY: every caller is on the boot path or the login path, both
// single-threaded with respect to each other, and nothing here blocks, waits
// or allocates. There is no wait/poll of any kind in this file.

#![allow(dead_code)]

use core::ptr;

/// Most not-run groups recorded. A boot that manages to decline 24 distinct
/// self-tests has a problem the 25th line was not going to explain.
const MAX_GROUPS: usize = 24;
const NAME_CAP: usize = 48;
const REASON_CAP: usize = 160;
/// How far a caller's C string is scanned for its NUL before it is refused.
/// Callers pass string literals from kernel .rodata; this bound exists so a
/// corrupt pointer cannot walk memory, not because a real caller is long.
const SCAN_MAX: usize = 1024;

#[derive(Copy, Clone)]
struct Entry {
    name: [u8; NAME_CAP],
    reason: [u8; REASON_CAP],
}

const ENTRY_INIT: Entry = Entry {
    name: [0u8; NAME_CAP],
    reason: [0u8; REASON_CAP],
};

static mut NOTRUN: [Entry; MAX_GROUPS] = [ENTRY_INIT; MAX_GROUPS];
static mut NOTRUN_N: usize = 0;
/// Groups that declined after the table filled. Counted rather than dropped
/// silently, because "we ran out of room to tell you" must not read as zero.
static mut NOTRUN_OVERFLOW: u32 = 0;
static mut RAN_N: u32 = 0;

/// Copy a bounded C string into `dst`, always NUL-terminating. Returns the
/// number of bytes copied (excluding the NUL). A NULL source yields "?".
///
/// SAFETY: `src` is a kernel pointer supplied by the caller. It is read one
/// byte at a time for at most SCAN_MAX bytes, and the write side is a
/// bounds-checked slice, so neither side can run past its buffer.
unsafe fn copy_cstr(src: *const u8, dst: &mut [u8]) -> usize {
    let cap = dst.len();
    if cap == 0 {
        return 0;
    }
    if src.is_null() {
        dst[0] = b'?';
        if cap > 1 {
            dst[1] = 0;
        }
        return 1;
    }
    let mut i = 0usize;
    while i < SCAN_MAX && i + 1 < cap {
        let b = ptr::read_volatile(src.add(i));
        if b == 0 {
            break;
        }
        dst[i] = b;
        i += 1;
    }
    dst[i] = 0;
    i
}

/// Record that a self-test group RAN.
///
/// Two jobs. The first is arithmetic: the boot summary should say "12 ran, 1
/// did not" rather than "1 did not", which is the difference between a number
/// a reader can calibrate and a number they cannot.
///
/// The second matters more. It RETRACTS any earlier not-run entry with the
/// same name. A group can legitimately decline at one point in the boot and
/// run later: perms/traversal declines during the OOBE bootstrap session
/// (which has no home yet, by design) and then runs a few seconds later at the
/// first real login of the account the wizard just created. Reporting that
/// decline at the end of the boot would be true about an instant and false
/// about the boot, and a summary that cries wolf on every first boot is a
/// summary people learn to skip - which is the exact failure this register was
/// built to end.
#[no_mangle]
pub extern "C" fn selftest_ran_rs(name: *const u8) {
    unsafe {
        RAN_N = RAN_N.saturating_add(1);
        if name.is_null() || NOTRUN_N == 0 {
            return;
        }
        let mut key = [0u8; NAME_CAP];
        copy_cstr(name, &mut key);
        let tbl = ptr::addr_of_mut!(NOTRUN) as *mut Entry;
        let mut i = 0usize;
        while i < NOTRUN_N {
            if (*tbl.add(i)).name == key {
                // Shift the tail down by one. Bounded by NOTRUN_N <= MAX_GROUPS.
                let mut j = i;
                while j + 1 < NOTRUN_N {
                    let next = *tbl.add(j + 1);
                    *tbl.add(j) = next;
                    j += 1;
                }
                NOTRUN_N -= 1;
                // Do not advance i: the entry now at i has not been examined.
                continue;
            }
            i += 1;
        }
    }
}

/// Record that a self-test group DID NOT RUN, with the reason. Returns 1 if it
/// was stored, 0 if the table was full (still counted; see NOTRUN_OVERFLOW).
///
/// This does NOT print. The C wrapper `selftest_notrun()`
/// (security/selftest_registry.c) prints the loud durable line, so that every
/// caller gets the same wording and the same sink whether or not it remembered
/// to ask for one.
#[no_mangle]
pub extern "C" fn selftest_notrun_rs(name: *const u8, reason: *const u8) -> i32 {
    unsafe {
        let n = NOTRUN_N;
        if n >= MAX_GROUPS {
            NOTRUN_OVERFLOW = NOTRUN_OVERFLOW.saturating_add(1);
            return 0;
        }
        let tbl = ptr::addr_of_mut!(NOTRUN) as *mut Entry;
        let e = &mut *tbl.add(n);
        copy_cstr(name, &mut e.name);
        copy_cstr(reason, &mut e.reason);
        NOTRUN_N = n + 1;
        1
    }
}

/// How many groups declined to run this boot (stored entries only).
#[no_mangle]
pub extern "C" fn selftest_notrun_count_rs() -> u32 {
    unsafe { NOTRUN_N as u32 }
}

/// How many declined but could not be stored.
#[no_mangle]
pub extern "C" fn selftest_notrun_overflow_rs() -> u32 {
    unsafe { NOTRUN_OVERFLOW }
}

/// How many groups ran.
#[no_mangle]
pub extern "C" fn selftest_ran_count_rs() -> u32 {
    unsafe { RAN_N }
}

/// Read back entry `idx`. Copies the stored name into `name_out` (capacity
/// `name_cap`) and reason into `reason_out` (capacity `reason_cap`), both
/// NUL-terminated. Returns 0 on success, -1 if `idx` is out of range or a
/// buffer is NULL/zero-length.
///
/// SAFETY: the caller guarantees the two output buffers span the capacities it
/// declares. Writes are bounds-checked slice writes against those capacities.
#[no_mangle]
pub extern "C" fn selftest_notrun_entry_rs(
    idx: u32,
    name_out: *mut u8,
    name_cap: u32,
    reason_out: *mut u8,
    reason_cap: u32,
) -> i32 {
    unsafe {
        let i = idx as usize;
        if i >= NOTRUN_N {
            return -1;
        }
        if name_out.is_null() || reason_out.is_null() || name_cap == 0 || reason_cap == 0 {
            return -1;
        }
        let tbl = ptr::addr_of!(NOTRUN) as *const Entry;
        let e = &*tbl.add(i);
        let nd: &mut [u8] = core::slice::from_raw_parts_mut(name_out, name_cap as usize);
        copy_cstr(e.name.as_ptr(), nd);
        let rd: &mut [u8] = core::slice::from_raw_parts_mut(reason_out, reason_cap as usize);
        copy_cstr(e.reason.as_ptr(), rd);
        0
    }
}

/// Self-test of the register itself, so the thing that reports not-run groups
/// is not itself a control nobody has watched work. Returns the number of
/// failing assertions (0 = good). Pure: it uses a private scratch table and
/// does not disturb the live one.
#[no_mangle]
pub extern "C" fn selftestreg_selftest_rs() -> i32 {
    let mut bad = 0i32;

    // A bounded copy of a string that does not fit must truncate and still
    // NUL-terminate, never run past the buffer.
    let src = b"abcdefghij\0";
    let mut small = [0xAAu8; 5];
    let n = unsafe { copy_cstr(src.as_ptr(), &mut small) };
    if n != 4 || small[4] != 0 || &small[0..4] != b"abcd" {
        bad += 1;
    }

    // An exact fit.
    let mut exact = [0xAAu8; 11];
    let n2 = unsafe { copy_cstr(src.as_ptr(), &mut exact) };
    if n2 != 10 || exact[10] != 0 || &exact[0..10] != b"abcdefghij" {
        bad += 1;
    }

    // A NULL source must yield a terminated placeholder, not a wild read.
    let mut nul = [0xAAu8; 8];
    let n3 = unsafe { copy_cstr(core::ptr::null(), &mut nul) };
    if n3 != 1 || nul[0] != b'?' || nul[1] != 0 {
        bad += 1;
    }

    // THE RETRACTION, on the live table, because it is the half most likely to
    // be wrong and the half nothing else would notice. Two entries are added,
    // the FIRST is retracted (so the shift-down is exercised, not just a
    // trailing removal), and the table is left exactly as it was found.
    unsafe {
        let n_before = NOTRUN_N;
        let ran_before = RAN_N;
        if n_before + 2 <= MAX_GROUPS {
            selftest_notrun_rs(b"selftestreg/probe-a\0".as_ptr(),
                               b"self-test probe, not a real decline\0".as_ptr());
            selftest_notrun_rs(b"selftestreg/probe-b\0".as_ptr(),
                               b"self-test probe, not a real decline\0".as_ptr());
            if NOTRUN_N != n_before + 2 {
                bad += 1;
            }
            selftest_ran_rs(b"selftestreg/probe-a\0".as_ptr());
            if NOTRUN_N != n_before + 1 {
                bad += 1;
            }
            // probe-b must have SHIFTED DOWN into probe-a's slot, not been
            // left stranded past the new length.
            let tbl = ptr::addr_of!(NOTRUN) as *const Entry;
            let e = &*tbl.add(n_before);
            if &e.name[0..19] != b"selftestreg/probe-b" {
                bad += 1;
            }
            selftest_ran_rs(b"selftestreg/probe-b\0".as_ptr());
            if NOTRUN_N != n_before {
                bad += 1;
            }
            // Retracting something that was never registered must be a no-op.
            selftest_ran_rs(b"selftestreg/never\0".as_ptr());
            if NOTRUN_N != n_before {
                bad += 1;
            }
            // Leave the RAN counter as we found it: this probe is not four
            // groups' worth of coverage and must not inflate the summary.
            RAN_N = ran_before;
        }
    }

    bad
}
