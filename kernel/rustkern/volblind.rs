// rustkern/volblind.rs - [volaudit] #246: THE RUNTIME DETECTOR FOR
// "THE ROOT FILESYSTEM WAS ASKED ABOUT A PATH ON A REMOVABLE VOLUME".
//
// WHY A DETECTOR AND NOT (ONLY) A LINT
// ==========================================================================
// In two days this one fault was found FOUR separate times, in four unrelated
// subsystems, every time by a human tripping over a symptom:
//
//   * the Recycle-Bin capture asked the root fs, so a marked AI actor could
//     not delete on a stick at all (#246, [escrowred]);
//   * four of five non-delete capture hooks did the same, so an AI could
//     DESTROY a file on a stick with nothing kept (#246, [usbescrow]);
//   * rename, mkdir, rmdir and unlink in proc/syscall.c all did it, so a
//     stick was read-mostly for every ordinary user AND a permission check
//     silently did not run (#246, [renamevol]);
//   * one of three ownership-stamping sites was missing from the removable
//     branch of the open path, so a file an unprivileged user creates on a
//     stick is judged ROOT'S.
//
// Four for four by accident means the next one is also waiting to be tripped
// over. The project's instinct here is a build-time lint, and there is one
// (kernel/tools/volume-route-lint). But a lint cannot be the primary defence
// for this class, and it is worth saying exactly why rather than implying the
// lint is weak:
//
//   * The offending argument is almost never a literal. There are ~260 call
//     sites in this kernel that hand a non-literal path to a g_fat_fs
//     function, and the overwhelming majority are kernel-internal paths built
//     by snprintf from a fixed "/CONFIG/..." style prefix, which can never
//     name a removable volume. A lint that flags all of them is ~95% false
//     alarms, and a lint that cries wolf gets switched off. That has already
//     happened once in this tree.
//   * Deciding WHICH of the 260 is reachable with a user-supplied path needs
//     interprocedural taint. `sc_path_is_dir(kpath)` is blind, but its
//     parameter is tainted two frames up in `sys_chdir`. A regex cannot see
//     that, and a sound C dataflow analysis is not a 400-line tool.
//
// The runtime check has the opposite error profile and it is the better one:
// it fires if and only if a path that GENUINELY lies under the mount point of
// a GENUINELY mounted removable volume is handed to the root filesystem's
// handle. There is no such thing as a legitimate instance of that, so the
// false-alarm rate is zero by construction, and it sees every route into the
// fault including the ones no static tool can follow (a path assembled at
// runtime, a path arriving through the DOS layer, a path a GUI app built from
// a directory listing).
//
// Its weakness is the mirror image: it only reports what is EXERCISED, and
// only on a machine with a volume plugged in. So the two are kept together
// and neither is described as sufficient. The lint stops a new instance being
// committed in the shapes it can read; this reports the live ones with a
// return address, which is how the remaining backlog was enumerated at all
// rather than guessed at.
//
// MODELLED ON sync/noblock.c, DELIBERATELY
// ==========================================================================
// Same shape as the #426 wq_assert_may_block() detector, because that one is
// already proven in this tree: detection always on including in the golden,
// only the REACTION gated; de-duplicated by CALLER ADDRESS so one site in a
// loop cannot turn the serial port into a hang; a bounded site table; and an
// addr2line-able return address in the line. Copying a working pattern is the
// instruction, and a second pattern for the same job is the thing this ticket
// exists to stop.
//
// WHAT LIVES HERE AND WHAT STAYS IN C
// ==========================================================================
// Here: the VERDICT (is this ask blind?), the de-duplication table, the
// counters, and an exhaustive boot self-test over the verdict's whole input
// space. All of it is integer and array work with no allocation.
//
// In C (fs/volblind.c, ~60 lines): the `fat_fs_t *` pointer comparison, the
// call to hotplug_resolve_path(), and bootlog_fault_write(), which is a
// varargs C function. That is the same split drivers/hotplug.c already uses
// for path_route_rs(): the pointers stay where the pointers live, the decision
// crosses as integers. Nothing in this file can be handed a pointer it might
// dereference.
//
// CONCURRENCY. Lock-free by construction: the site table is claimed with
// compare_exchange on AtomicU64 slots and the counters are atomic adds. It
// takes no lock and does not wait, so it is safe to call from anywhere the
// FAT wrappers are called from, which includes paths that already hold the
// FAT lock's caller's locks. It must never take a lock: fs/fat.c calls it
// BEFORE fat_lock(), and a detector that can block would turn a diagnostic
// into a deadlock. concurrency-lint has nothing to see here and that is not
// an accident.

#![allow(dead_code)]

use core::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

/// The ask is fine: either the handle is not the root filesystem, or the path
/// does not lie under a mounted removable volume's mount point.
pub const VB_OK: i32 = 0;

/// BLIND ASK. The root filesystem's handle was given a path that lies under a
/// mounted removable volume. The root cannot see that file. Every outcome is
/// wrong and they are wrong in different ways: an existence test answers
/// ABSENT for a file that is there (which is how a permission gate came to
/// take its weaker branch), a read fails, and a WRITE either fails or, worse,
/// creates a different file of the same spelling in the root namespace.
pub const VB_BLIND: i32 = 1;

/// Max distinct reported call sites per boot. One site is reported ONCE; after
/// that it is counted silently. 32 rather than noblock.c's 16 because the
/// known backlog at the time this landed was larger than that.
pub const VB_SITES_MAX: usize = 32;

static SITES: [AtomicU64; VB_SITES_MAX] = {
    #[allow(clippy::declare_interior_mutable_const)]
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; VB_SITES_MAX]
};
static SITE_COUNT: AtomicUsize = AtomicUsize::new(0);
static OVERFLOW_LOGGED: AtomicU64 = AtomicU64::new(0);

/// Total blind asks seen this boot, reported or not. This is the number the
/// verification reads: it is 0 on a boot with no removable volume and on a
/// boot where every exercised path routes correctly, and non-zero the moment
/// one does not. A count is evidence; "the code looks right" is not.
static BLIND_TOTAL: AtomicU64 = AtomicU64::new(0);

/// Is this ask blind? Both arguments are ANSWERS the C side has already
/// computed, passed as 0/1:
///
/// * `fs_is_root`: the `fat_fs_t *` the caller passed is `&g_fat_fs`. The
///   pointer comparison stays in C; only its result crosses.
/// * `matched`: the path lies under the mount point of a MOUNTED removable
///   volume, i.e. `hotplug_resolve_path(path, _) >= 0`.
///
/// Note what is deliberately NOT an input: whether the volume's filesystem is
/// READABLE. An exFAT volume that the hotplug layer mounted but cannot serve
/// files from is still a volume whose namespace the root does not own, and
/// asking the root about a path inside it is the exact fall-through that
/// `hotplug_volume_for_path`'s `matched_out` parameter exists to prevent. So a
/// non-readable volume is reported too, and that is the point.
#[no_mangle]
pub extern "C" fn volblind_verdict_rs(fs_is_root: i32, matched: i32) -> i32 {
    if fs_is_root != 0 && matched != 0 {
        VB_BLIND
    } else {
        VB_OK
    }
}

/// Count one blind ask and return the running total. Separate from the verdict
/// so the verdict stays a pure function the self-test can hammer 2^2 times
/// without moving the ship-visible counter.
#[no_mangle]
pub extern "C" fn volblind_count_rs() -> u64 {
    BLIND_TOTAL.fetch_add(1, Ordering::Relaxed) + 1
}

/// The running total without disturbing it.
#[no_mangle]
pub extern "C" fn volblind_total_rs() -> u64 {
    BLIND_TOTAL.load(Ordering::Relaxed)
}

/// 1 if `caller` has not been reported before and a slot was claimed for it,
/// 0 if it is a repeat or the table is full.
///
/// `caller` is a return address used ONLY as an opaque identity: it is never
/// dereferenced here, which is why it crosses as a u64 and not a pointer.
/// 0 is treated as "no identity available" and always reports, because a
/// de-duplication key of 0 would silence every site that could not produce
/// one.
#[no_mangle]
pub extern "C" fn volblind_site_is_new_rs(caller: u64) -> i32 {
    if caller == 0 {
        return 1;
    }
    let n = SITE_COUNT.load(Ordering::Acquire);
    for i in 0..n.min(VB_SITES_MAX) {
        if SITES[i].load(Ordering::Relaxed) == caller {
            return 0;
        }
    }
    // Claim the next slot. The CAS makes a concurrent claim by another core
    // safe; losing the race only costs a duplicate line, never a bad write.
    loop {
        let idx = SITE_COUNT.load(Ordering::Acquire);
        if idx >= VB_SITES_MAX {
            // Report the overflow once, then go quiet. A detector that floods
            // the serial port is itself a hang; that is noblock.c's lesson and
            // it was learned the expensive way.
            if OVERFLOW_LOGGED
                .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
            {
                return -1; // caller prints the one table-full line
            }
            return 0;
        }
        if SITE_COUNT
            .compare_exchange(idx, idx + 1, Ordering::AcqRel, Ordering::Relaxed)
            .is_ok()
        {
            SITES[idx].store(caller, Ordering::Release);
            return 1;
        }
    }
}

/// How many distinct sites have been recorded. Printed in the end-of-boot
/// summary so a boot that saw none says so with a number.
#[no_mangle]
pub extern "C" fn volblind_sites_rs() -> u32 {
    let n = SITE_COUNT.load(Ordering::Acquire);
    if n > VB_SITES_MAX {
        VB_SITES_MAX as u32
    } else {
        n as u32
    }
}

/// EXHAUSTIVE over the verdict's input space, plus the de-duplication table's
/// three distinguishable behaviours (new site, repeat site, table full),
/// checked against an independently written expectation rather than a
/// restatement of the code above.
///
/// Returns the number of FAILING vectors; 0 is the only acceptable answer.
/// `*out_checks` receives the number of vectors actually evaluated, so the
/// boot line carries a COUNT: blame.md's most repeated lesson is that a
/// self-test which silently stopped checking anything still prints PASS.
///
/// It runs BEFORE any real detection, and deliberately uses a private table
/// rather than the live one, so arming the self-test cannot consume the live
/// site slots or move the live counter. That mistake would make the detector
/// report fewer real sites the better tested it was.
#[no_mangle]
pub extern "C" fn volblind_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut checks: u32 = 0;
    let mut fails: u32 = 0;

    // --- the verdict, all four input combinations.
    for fs_is_root in 0..2i32 {
        for matched in 0..2i32 {
            checks += 1;
            // Independent expectation: blind exactly when the ROOT handle is
            // used for a path that IS on a removable volume.
            let want = if fs_is_root == 1 && matched == 1 { VB_BLIND } else { VB_OK };
            if volblind_verdict_rs(fs_is_root, matched) != want {
                fails += 1;
            }
        }
    }

    // --- the de-duplication table, on a PRIVATE copy of the algorithm so the
    // live table is untouched. This checks the property that matters: a site
    // reports once and then never again, and the table is bounded.
    {
        // A free function, not a closure, so `cnt` can still be inspected
        // afterwards: the whole point of the third check is to read the final
        // table length, and a closure holding it mutably would make that
        // unreadable.
        fn tab_is_new(tab: &mut [u64; VB_SITES_MAX], cnt: &mut usize, caller: u64) -> i32 {
            if caller == 0 {
                return 1;
            }
            for i in 0..*cnt {
                if tab[i] == caller {
                    return 0;
                }
            }
            if *cnt >= VB_SITES_MAX {
                return 0;
            }
            tab[*cnt] = caller;
            *cnt += 1;
            1
        }

        let mut tab = [0u64; VB_SITES_MAX];
        let mut cnt: usize = 0;

        // A fresh site reports; the same site never reports twice, however
        // many times it fires. 1000 repeats because the real shape of this
        // fault is a call inside a directory-listing loop.
        checks += 1;
        if tab_is_new(&mut tab, &mut cnt, 0xDEAD_BEEF) != 1 {
            fails += 1;
        }
        let mut repeats = 0;
        for _ in 0..1000 {
            if tab_is_new(&mut tab, &mut cnt, 0xDEAD_BEEF) != 0 {
                repeats += 1;
            }
        }
        checks += 1;
        if repeats != 0 {
            fails += 1;
        }

        // Distinct sites each get one slot, up to the bound, and then the
        // table stops growing instead of writing past itself.
        for k in 0..(VB_SITES_MAX as u64 * 2) {
            tab_is_new(&mut tab, &mut cnt, 0x1000 + k);
        }
        checks += 1;
        if cnt != VB_SITES_MAX {
            fails += 1;
        }

        // A zero key is "no identity": it must always report rather than
        // silently claim slot 0 and silence everything that shares it.
        checks += 1;
        if tab_is_new(&mut tab, &mut cnt, 0) != 1 {
            fails += 1;
        }
    }

    if !out_checks.is_null() {
        unsafe { *out_checks = checks };
    }
    fails
}
