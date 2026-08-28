// rustkern/fstatkind.rs - #120: what KIND of object an open file description
// refers to, decided from the name the description carries.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle and no -DRUST_* flag: this classification did not exist before #120
// in any form. The thing it replaces is not a C function, it is a Ring-3 LIE -
// userland/libc/sys/stat.c hardcoded `S_IFREG | 0644` for every descriptor.
//
// ===========================================================================
// WHY THIS IS A SEPARATE, PURE FUNCTION
// ---------------------------------------------------------------------------
// sys_fstat() is entangled with the C fd table, the C file_ops vtable and the
// four C filesystem backends, so it stays C (stated in the CHANGELOG). But the
// question "given the recorded name, what type bits and device identity should
// this fd report" is pure logic over a byte string, with no kernel structure in
// it at all. That is exactly the seam worth having in Rust, and it is where the
// bug class lives: a wrong TYPE is the defect #120 exists to fix, and a bounded
// slice compare cannot walk off the end of a 128-byte path the way the ad-hoc C
// string handling elsewhere in this tree has repeatedly done.
//
// THE NAMES THIS READS are not parsed out of user input. They are written by
// the kernel itself at the moment each fd is created, at exactly four sites:
//
//     fs/fat_vfs.c   file_set_path(f, path)          -> "/BOOT/KERNEL.ELF"
//     fs/ext2_vfs.c  file_set_path(f, path)          -> "/APPS/FILES"
//     fs/pipe.c      file_set_path(rf, "pipe:[read]")
//     drivers/dev.c  file_set_path(f, "/dev/<name>") -> the ONE device opener
//     net/socket.c   file_set_path(f, "socket:[...]") -> added by #120
//
// so the prefixes below are a closed set, not a heuristic over arbitrary text.
//
// THE SOCKET CASE WAS DEAD CODE UNTIL #120. proc/procinfo.h has defined
// PI_KIND_SOCKET since #487 and proc/procinfo.c's classifier could never return
// it, because sockets were the one fd family that called no file_set_path at
// all, so every socket handle in Task Manager was UNKNOWN. #120 names them, so
// the value that was declared for years now actually occurs.
//
// ===========================================================================
// THE HONESTY RULE, and it is the whole point of the ticket
// ---------------------------------------------------------------------------
// An UNRECOGNISED name reports KIND_UNKNOWN with st_mode = 0: NO type bits at
// all. It does NOT default to S_IFREG.
//
// That matters more than it looks. S_ISREG/S_ISDIR/S_ISFIFO/S_ISSOCK are all
// FALSE against a zero mode, so a caller that asks "is this a regular file"
// gets NO, which is the truthful answer for something we cannot identify. The
// implementation being deleted answered YES to that question for a directory,
// for a pipe, for a socket and for a closed descriptor. Defaulting to S_IFREG
// here would have carried the original defect into its own fix.
//
// Zero-means-unknown is the convention this kernel already uses for st_ino and
// for the three timestamps (see the per-backend table in proc/syscall.c), so
// this is the existing rule applied to st_mode, not a new one.
// ===========================================================================

/// Kind values. THESE ARE NOT NEW NUMBERS. proc/procinfo.h has defined
/// PI_KIND_FILE/DEV/PIPE/SOCKET/UNKNOWN since #487 for Task Manager's handle
/// list, and that is an ABI Ring 3 already reads, so this module ADOPTS them
/// rather than inventing a parallel set.
///
/// #120 nearly shipped a second one: this file first used FILE=0 PIPE=1 DEV=2,
/// which has DEV and PIPE SWAPPED against procinfo.h. Two classifiers with
/// silently transposed constants is precisely the defect class this codebase
/// keeps paying for, and it was caught by a compiler error about an unrelated
/// name collision, not by review. Locked by _Static_assert in
/// proc/syscall_argtab_lock.c against the C defines.
///
/// A real filesystem object: the caller must go to the per-backend fill
/// (sc_stat_fill) for it. This module deliberately says NOTHING about a
/// KIND_FILE's mode, because only the filesystem knows whether it is a
/// directory.
pub const KIND_FILE: u32 = 0;
pub const KIND_DEV: u32 = 1;
pub const KIND_PIPE: u32 = 2;
pub const KIND_SOCK: u32 = 3;
pub const KIND_UNKNOWN: u32 = 4;

// POSIX type bits, same values as userland/libc/sys/stat.h and as the ext2
// on-disk i_mode this kernel already reports verbatim.
const S_IFIFO: u32 = 0o010000;
const S_IFCHR: u32 = 0o020000;
const S_IFSOCK: u32 = 0o140000;

// st_dev identities. 1..4 are OWNED BY sc_stat_fill in proc/syscall.c
// (FAT=1, EXT2=2, SMB=3, NFS=4) and must not be reused here; 5 and 6 continue
// that one numbering. Kept as a single ascending list in a comment rather than
// duplicated as constants in two files, which is how two numbering schemes for
// the same field come to disagree.
const DEV_PIPE: u64 = 5;
const DEV_DEV: u64 = 6;

/// The answer, shared with C. `#[repr(C)]` + a `_Static_assert` on the C side
/// (proc/syscall_argtab_lock.c) locks the layout, per this codebase's existing
/// FFI convention.
#[repr(C)]
pub struct KStatKind {
    pub kind: u32,
    pub mode: u32,
    pub dev: u64,
    pub rdev: u64,
}

/// Bounded view of a C string. `cap` is the BUFFER size (VFS_FPATH_MAX = 128),
/// not a length the caller promises is right: the scan stops at the first NUL
/// or at `cap`, whichever comes first, so an unterminated buffer is truncated
/// rather than over-read.
unsafe fn as_slice<'a>(p: *const u8, cap: u32) -> &'a [u8] {
    if p.is_null() || cap == 0 {
        return &[];
    }
    let cap = cap as usize;
    let mut n = 0usize;
    while n < cap && *p.add(n) != 0 {
        n += 1;
    }
    core::slice::from_raw_parts(p, n)
}

/// FNV-1a over the device name, folded to 16 bits, forced non-zero.
///
/// HONEST LABEL: this is NOT a Linux dev_t and there is no major/minor here,
/// because this kernel has no device numbers to report - drivers/dev.c keys
/// everything on the NAME. What st_rdev is actually good for in a caller is
/// telling two devices apart, and a stable per-name value does exactly that and
/// nothing more. It is the same discipline as the SYNTHESISED st_ino that #115
/// gives FAT: derived, stable, documented as derived, and never presented as a
/// number the medium supplied.
fn synth_rdev(name: &[u8]) -> u64 {
    let mut h: u32 = 0x811c9dc5;
    for &b in name {
        h ^= b as u32;
        h = h.wrapping_mul(0x01000193);
    }
    let v = ((h ^ (h >> 16)) & 0xffff) as u64;
    if v == 0 {
        1
    } else {
        v
    }
}

/// Classify the recorded name of an open file description.
///
/// Always writes `out`. Never fails: an unusable input is KIND_UNKNOWN with a
/// zero mode, which is the honest answer and is safe for every caller.
#[no_mangle]
pub extern "C" fn fstat_kind_rs(path: *const u8, cap: u32, out: *mut KStatKind) {
    if out.is_null() {
        return;
    }
    let name = unsafe { as_slice(path, cap) };
    let (kind, mode, dev, rdev) = if name.starts_with(b"pipe:[") {
        // 0600: a pipe is readable and writable by its creator and by nobody
        // else. There is no other principal that can name it.
        (KIND_PIPE, S_IFIFO | 0o600, DEV_PIPE, 0)
    } else if name.starts_with(b"socket:[") {
        (KIND_SOCK, S_IFSOCK | 0o600, 0, 0)
    } else if name.starts_with(b"/dev/") {
        // 0666: every registered device in drivers/dev.c is openable by any
        // caller today. Reporting a tighter mode than the kernel enforces would
        // be the same class of fiction this ticket is removing.
        (KIND_DEV, S_IFCHR | 0o666, DEV_DEV, synth_rdev(&name[5..]))
    } else if name.starts_with(b"/") {
        (KIND_FILE, 0, 0, 0)
    } else {
        // Empty (an anonymous description) or anything unrecognised.
        (KIND_UNKNOWN, 0, 0, 0)
    };
    unsafe {
        (*out).kind = kind;
        (*out).mode = mode;
        (*out).dev = dev;
        (*out).rdev = rdev;
    }
}

/// Boot self-test. Returns the number of MISMATCHES, so 0 is the pass value and
/// a caller that forgets to check still sees a non-zero count in the log.
///
/// This is deliberately NOT a [RUST-DIFF] equivalence differential against a C
/// twin: there is no C twin, and #433's lesson in blame.md is that a
/// differential where both arms share the same wrong assumption scores a
/// meaningless pass. These are ABSOLUTE assertions about what each name must
/// classify as, written from the four file_set_path call sites.
#[no_mangle]
pub extern "C" fn fstat_kind_selftest_rs() -> u32 {
    let mut bad: u32 = 0;
    let mut k = KStatKind { kind: 0, mode: 0, dev: 0, rdev: 0 };

    let check = |k: &KStatKind, want_kind: u32, want_type: u32| -> u32 {
        let type_ok = if want_type == 0 {
            k.mode == 0
        } else {
            (k.mode & 0o170000) == want_type
        };
        if k.kind == want_kind && type_ok { 0 } else { 1 }
    };

    fstat_kind_rs(b"pipe:[read]\0".as_ptr(), 12, &mut k);
    bad += check(&k, KIND_PIPE, S_IFIFO);
    fstat_kind_rs(b"pipe:[write]\0".as_ptr(), 13, &mut k);
    bad += check(&k, KIND_PIPE, S_IFIFO);
    fstat_kind_rs(b"/dev/null\0".as_ptr(), 10, &mut k);
    bad += check(&k, KIND_DEV, S_IFCHR);
    fstat_kind_rs(b"socket:[tcp]\0".as_ptr(), 13, &mut k);
    bad += check(&k, KIND_SOCK, S_IFSOCK);
    fstat_kind_rs(b"/APPS/FILES\0".as_ptr(), 12, &mut k);
    bad += check(&k, KIND_FILE, 0);
    // THE case the old implementation got wrong in the other direction: an
    // unrecorded name must NOT come back as a regular file.
    fstat_kind_rs(b"\0".as_ptr(), 1, &mut k);
    bad += check(&k, KIND_UNKNOWN, 0);
    // Two different devices must be distinguishable.
    let mut a = KStatKind { kind: 0, mode: 0, dev: 0, rdev: 0 };
    let mut b = KStatKind { kind: 0, mode: 0, dev: 0, rdev: 0 };
    fstat_kind_rs(b"/dev/null\0".as_ptr(), 10, &mut a);
    fstat_kind_rs(b"/dev/zero\0".as_ptr(), 10, &mut b);
    if a.rdev == b.rdev || a.rdev == 0 || b.rdev == 0 {
        bad += 1;
    }
    // An UNTERMINATED buffer must be bounded by cap, not walked past it.
    let raw = b"/dev/nullXXXXXXXX";
    fstat_kind_rs(raw.as_ptr(), 9, &mut k);
    bad += check(&k, KIND_DEV, S_IFCHR);
    // NULL out must not fault.
    fstat_kind_rs(b"/dev/null\0".as_ptr(), 10, core::ptr::null_mut());

    bad
}
