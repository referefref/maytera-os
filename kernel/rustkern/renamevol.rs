// rustkern/renamevol.rs - [renamevol] #246: WHICH FILESYSTEM MAY SERVE A
// TWO-PATH FILESYSTEM OPERATION, when either end can be on a hot-plugged
// removable volume.
//
// WHY THIS EXISTS, MEASURED BEFORE IT WAS WRITTEN
// ==========================================================================
// `sys_rename()` performed every rename with `fat_rename(&g_fat_fs, old, new)`.
// `g_fat_fs` routes the ext2 root, the FAT ESP, /SMB, /NFS and a mounted disk
// image; it does NOT route a hot-plugged removable volume. So a rename of a
// file on a USB stick asked the ROOT filesystem about a path the root has never
// heard of, and simply failed. Three sibling syscalls (mkdir, rmdir, unlink)
// had the identical single-path form of the same defect.
//
// The repair is not "call the volume's fat_fs_t instead". It is "decide, for a
// PAIR of paths, whether this operation is legal at all", and that decision has
// five distinguishable inputs and three outcomes. In C it is a nest of
// conditionals over two pointers and two flags where every wrong answer is
// silent and destructive in a different way:
//
//   * one end on a volume and the other on the root, performed on the ROOT
//     filesystem, writes a file into the root directory under the stick's name
//     (or, with the volume's fat_fs_t, writes it onto the stick) and reports
//     success. A cross-device rename that SUCCEEDS at the wrong end is worse
//     than one that fails: the user believes the file moved.
//   * two DIFFERENT volumes, performed on either one, is the same fault with
//     two sticks instead of one.
//   * a volume that is MOUNTED BUT NOT READABLE (exFAT today: the hotplug
//     layer knows its size but has no file operations for it) must be REFUSED,
//     never allowed to fall through to the root filesystem, because the root
//     may well have a file at that same spelling and would quietly destroy it.
//     That exact fall-through is what `hotplug_volume_for_path`'s `matched_out`
//     parameter exists to prevent, and it is the reason "the volume pointer is
//     NULL" is not the same question as "this path is not on a volume".
//
// So: the ROUTING POLICY is here, in Rust, with an exhaustive self-test over
// all 32 input combinations that runs on every boot. The fat_fs_t pointers, the
// descriptors and the filesystem calls stay in C, which is the established
// split in this tree (fs/recycle_fs.c's own header comment argues it at
// length, and drivers/hotplug.c keeps the pointer-valued resolver).
//
// WHAT THIS DELIBERATELY DOES NOT DECIDE
// ==========================================================================
// It does not know what a mount point is spelled like, how many volumes exist,
// or which slot a path lands in. `hotplug_path_split_rs` already owns the
// "/USB10 is not /USB1" boundary question and `hotplug_volume_for_path` already
// owns the slot lookup. This takes their ANSWERS. A second copy of the matching
// rule is precisely the fault that produced three bugs in one week, so there
// is none here.
//
// It also does not decide PERMISSION. `perms_may_remove()` is the one place
// that answers "may this principal displace this name", and the caller asks it
// with an existence answer that comes from `recycle_victim_stat()`, the
// volume-aware oracle the capture path already uses. Routing and authorization
// are separate questions and conflating them is how the destination-removal
// check came to be skipped on one class of volume in the first place.
//
// CONCURRENCY: pure arithmetic over five integers. No state, no allocation, no
// waiting, no polling. Nothing here can block, so wq_assert_may_block() has
// nothing to say about it and concurrency-lint has nothing to see.

#![allow(dead_code)]

/// Both ends belong to the ROOT FAMILY (ext2 root, FAT ESP, /SMB, /NFS, a
/// mounted disk image). The caller performs the operation exactly as it did
/// before this module existed: `g_fat_fs` with the full absolute paths.
pub const RNV_ROOT: i32 = 0;

/// Both ends are on the SAME mounted, readable, removable volume. The caller
/// performs the operation against THAT volume's `fat_fs_t` with the
/// VOLUME-RELATIVE paths.
pub const RNV_SAMEVOL: i32 = 1;

/// This pair must not be performed by anyone. The caller returns failure
/// without touching either end. Covers a cross-device pair, two different
/// volumes, and a volume that is mounted but whose filesystem cannot serve
/// files.
pub const RNV_REFUSE: i32 = -1;

/// Route a TWO-PATH operation (rename today; any future link/exchange would ask
/// the identical question).
///
/// Every argument is an ANSWER already produced by the C resolver, passed as
/// 0/1 so that this function cannot be handed a pointer it might dereference:
///
/// * `old_matched` / `new_matched`: that end's path lies under the mount point
///   of a MOUNTED removable volume. This is `hotplug_volume_for_path`'s
///   `*matched_out`, and it is set even when the volume pointer came back NULL.
/// * `old_readable` / `new_readable`: that end's volume returned a usable
///   `fat_fs_t` (1), or did not (0). Only meaningful when the matching
///   `*_matched` is 1.
/// * `same_volume`: both ends resolved to the SAME `fat_fs_t`. The pointer
///   comparison stays in C, where the pointer lives; only its answer crosses.
///
/// Returns `RNV_ROOT`, `RNV_SAMEVOL` or `RNV_REFUSE`. There is deliberately no
/// fourth outcome and no "best effort": every input combination that is not
/// unambiguously one of the first two is a refusal, because the alternative to
/// refusing is performing the operation on a filesystem that was not asked
/// about.
#[no_mangle]
pub extern "C" fn rename_route_rs(old_matched: i32, new_matched: i32,
                                  old_readable: i32, new_readable: i32,
                                  same_volume: i32) -> i32 {
    let om = old_matched != 0;
    let nm = new_matched != 0;

    // Neither end is on a removable volume: the pre-existing root path,
    // bit-for-bit. This is the overwhelmingly common case and it must be
    // impossible for this module to change its behaviour.
    if !om && !nm {
        return RNV_ROOT;
    }

    // A path under a mounted volume whose filesystem cannot serve files
    // (exFAT). Checked BEFORE the cross-device test so that the refusal names
    // the real reason rather than being lumped in with EXDEV.
    if om && old_readable == 0 {
        return RNV_REFUSE;
    }
    if nm && new_readable == 0 {
        return RNV_REFUSE;
    }

    // Exactly one end on a volume: a cross-device rename. POSIX calls this
    // EXDEV and the caller must copy-then-delete if it wants the effect. This
    // is also EXACTLY what happened before this module existed (the root
    // filesystem could not find the volume-side path, so the rename failed), so
    // refusing introduces no regression; it just stops being an accident.
    if om != nm {
        return RNV_REFUSE;
    }

    // Both ends on a volume. The same one, or two different ones.
    if same_volume != 0 {
        RNV_SAMEVOL
    } else {
        RNV_REFUSE
    }
}

/// Route a SINGLE-PATH operation (mkdir, rmdir, unlink). Separate from the pair
/// form on purpose: it has no cross-device case at all, and folding it into the
/// five-argument function by passing the same end twice would make every call
/// site read as if it were asking a question it is not.
///
/// Returns `RNV_ROOT`, `RNV_SAMEVOL` or `RNV_REFUSE`.
#[no_mangle]
pub extern "C" fn path_route_rs(matched: i32, readable: i32) -> i32 {
    if matched == 0 {
        return RNV_ROOT;
    }
    if readable == 0 {
        return RNV_REFUSE;      // mounted, cannot serve files: never the root
    }
    RNV_SAMEVOL
}

/// EXHAUSTIVE over the pair form's whole input space (2^5 = 32 combinations)
/// plus the four of the single-path form, checked against an independently
/// written expectation rather than against a restatement of the code above.
///
/// Returns the number of FAILING vectors; 0 is the only acceptable answer. It
/// runs on every boot and the result is printed either way, because blame.md's
/// most-repeated lesson is that in-tree prose lies and a self-test that only
/// speaks up on failure is indistinguishable from one that never runs.
///
/// `*out_checks` receives the number of vectors actually evaluated, so the boot
/// line carries a COUNT and not just a verdict: a self-test that silently
/// stopped checking anything would otherwise still print PASS.
#[no_mangle]
pub extern "C" fn renamevol_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut bad: u32 = 0;
    let mut n: u32 = 0;

    // THE CONSTANTS THEMSELVES. drivers/hotplug.h carries a C copy of these
    // three values (RNV_ROOT / RNV_SAMEVOL / RNV_REFUSE) because the C callers
    // compare against them, and a _Static_assert cannot reach across the FFI
    // to check a Rust const. So they are pinned HERE instead: if anyone
    // renumbers them on this side, this self-test goes red on the next boot
    // rather than the C side silently reading a refusal as a root route.
    n += 1; if RNV_ROOT != 0 { bad += 1; }
    n += 1; if RNV_SAMEVOL != 1 { bad += 1; }
    n += 1; if RNV_REFUSE != -1 { bad += 1; }

    // ---- the pair form, all 32 combinations ----
    for bits in 0..32u32 {
        let om = (bits & 1) as i32;
        let nm = ((bits >> 1) & 1) as i32;
        let or_ = ((bits >> 2) & 1) as i32;
        let nr = ((bits >> 3) & 1) as i32;
        let sv = ((bits >> 4) & 1) as i32;

        // The expectation, written as the English rule and not as the code:
        //  - no volume involved            -> ROOT
        //  - a volume involved that cannot serve files -> REFUSE
        //  - one side only                 -> REFUSE (EXDEV)
        //  - both sides, same volume       -> SAMEVOL
        //  - both sides, different volumes -> REFUSE
        let want = if om == 0 && nm == 0 {
            RNV_ROOT
        } else if (om == 1 && or_ == 0) || (nm == 1 && nr == 0) {
            RNV_REFUSE
        } else if om != nm {
            RNV_REFUSE
        } else if sv != 0 {
            RNV_SAMEVOL
        } else {
            RNV_REFUSE
        };

        n += 1;
        if rename_route_rs(om, nm, or_, nr, sv) != want {
            bad += 1;
        }
    }

    // ---- the four properties that matter most, named individually, so a
    // ---- failure says WHICH rule broke instead of "one of 32 vectors".
    // A rename entirely on the root family must be routed to the root.
    n += 1; if rename_route_rs(0, 0, 0, 0, 0) != RNV_ROOT { bad += 1; }
    // Both ends on one readable volume is the case this whole change enables.
    n += 1; if rename_route_rs(1, 1, 1, 1, 1) != RNV_SAMEVOL { bad += 1; }
    // Two readable volumes is NOT a rename.
    n += 1; if rename_route_rs(1, 1, 1, 1, 0) != RNV_REFUSE { bad += 1; }
    // Stick -> root, and root -> stick, are both EXDEV and not half-done.
    n += 1; if rename_route_rs(1, 0, 1, 0, 0) != RNV_REFUSE { bad += 1; }
    n += 1; if rename_route_rs(0, 1, 0, 1, 0) != RNV_REFUSE { bad += 1; }
    // THE ONE THAT MUST NEVER FALL THROUGH: matched, but not readable. If this
    // ever returns RNV_ROOT, a rename aimed at an exFAT stick lands on the root
    // filesystem and can destroy a file there.
    n += 1; if rename_route_rs(1, 1, 0, 0, 1) != RNV_REFUSE { bad += 1; }
    n += 1; if rename_route_rs(1, 1, 1, 0, 1) != RNV_REFUSE { bad += 1; }
    n += 1; if rename_route_rs(1, 1, 0, 1, 1) != RNV_REFUSE { bad += 1; }

    // ---- the single-path form, all four combinations ----
    n += 1; if path_route_rs(0, 0) != RNV_ROOT { bad += 1; }
    n += 1; if path_route_rs(0, 1) != RNV_ROOT { bad += 1; }
    n += 1; if path_route_rs(1, 1) != RNV_SAMEVOL { bad += 1; }
    n += 1; if path_route_rs(1, 0) != RNV_REFUSE { bad += 1; }

    if !out_checks.is_null() {
        // SAFETY: a caller-supplied pointer to one u32 in kernel memory, written
        // once. Every caller in the tree passes the address of a local.
        unsafe { core::ptr::write(out_checks, n) };
    }
    bad
}
