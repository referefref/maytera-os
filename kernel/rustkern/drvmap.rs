// rustkern/drvmap.rs - #196 / #739: THE DRIVE-LETTER POLICY FOR MOUNTED DISK
// IMAGES. Which letter a mounted image gets, when a letter is free again, and
// what MSCDEX tells a DOS guest about all of it.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// There is no `_c` reference and no [RUST-DIFF] differential here because there
// is nothing to differ FROM; what proves it instead is drvmap_selftest_rs(),
// a property test over the allocator's own invariants that runs on every boot
// and prints one line. Say which kind of evidence you have: a differential
// proves "same as the C", a property test proves "obeys its own rules". This
// is the second kind.
//
// ===========================================================================
// WHY THE POLICY IS A SEPARATE, PURE MODULE
// ---------------------------------------------------------------------------
// Before this change the answer to "which drive letter" was three constants
// spread across three files: dos/diskimg.c hardcoded slot 0 = A: and slot 1 =
// E:, dos/dospath.c hardcoded the set {A, C, E}, and dos/dosexec.c hardcoded
// `c->cx = 4` and `ndrives = 1` into the MSCDEX installation check. Those three
// have to agree or a guest is told about a drive that does not exist, and
// nothing made them agree except that they were all wrong in the same way.
//
// So the policy lives in ONE pure function, drvmap_place_rs(), and the MSCDEX
// answer is DERIVED from the same table by drvmap_mscdex_rs() rather than
// written down a second time. The C side owns the images (imgfile handles,
// heap, the 256 KiB cache); this side owns only integers, which is what makes
// it testable without a disc.
//
// ===========================================================================
// THE MODEL
// ---------------------------------------------------------------------------
// Letters are typed by CLASS, and an image may only land on a letter whose
// class matches the format that was probed off the image itself:
//
//   A:, B:      FLOPPY.  Removable, folder-backed when empty, FAT12 image when
//                        mounted. Always "known" to DOS: a floppy DRIVE exists
//                        whether or not a disk is in it, which is how real DOS
//                        behaves and what lets a program see "drive not ready"
//                        rather than "no such drive".
//   C:          FIXED.   The hard disk, always folder-backed at
//                        /WINDIR/DRIVE_C. Never mountable, never ejectable.
//   D:          RESERVED and deliberately left out of every pool. Real MSCDEX
//                        assigns CD letters after the last hard disk, and a
//                        great deal of DOS-era software assumes C: is the
//                        system disk and D: is a second one. One unused letter
//                        is a cheap price for not surprising it. It also keeps
//                        E: as the FIRST CD letter, which is what the layers
//                        below were already verified against.
//   E: .. Z:    CD-ROM.  Allocated dynamically, LOWEST FREE FIRST. Read-only.
//                        A CD drive exists exactly while a disc is mounted on
//                        it, so "no disc" and "no drive" are the same
//                        observable state, which is what an eject should look
//                        like to a guest.
//
// ALLOCATION is lowest-free-in-class. RELEASE is immediate on eject: the letter
// returns to the pool and the next mount may reuse it. Lowest-free rather than
// next-highest is deliberate, see the MSCDEX contiguity note below.
//
// SWAP IS LETTER-PRESERVING. Mounting onto a letter that is already occupied is
// not an error, it is the disc-swap case, and it keeps the letter. That matters
// concretely: Red Alert ships one disc per faction and asks for the other one
// mid-game, and a swap that moved the disc from E: to F: would break the very
// program the swap exists for. So an explicit-letter mount over an occupied
// letter is allowed and does NOT count against the concurrent-mount limit.
//
// A CONCURRENT-MOUNT LIMIT EXISTS AND IS NOT A STYLE CHOICE. Each mounted image
// holds a 256 KiB imgfile cache (IMGF_CACHE_SLOTS * IMGF_CACHE_BLK), so N mounts
// cost N * 256 KiB of a 256 MB heap forever. 22 CD letters times 256 KiB is
// 5.5 MB pinned by nothing but someone clicking Mount 22 times. The cap makes
// the worst case a number rather than a discovery.

/// The MSCDEX view of the world, derived from the live mount table rather than
/// written down a second time. Mirrors mscdex_info_t in dos/diskimg.h;
/// sizeof-locked there with _Static_assert.
#[repr(C)]
pub struct MscdexInfo {
    /// Number of CD-ROM drives currently mounted. This is what INT 2Fh AX=1500h
    /// returns in BX, and it used to be the constant 1.
    pub count: u32,
    /// DOS drive number (A=0) of the lowest CD letter, or 0 when count == 0.
    /// This is AX=1500h's CX, and it used to be the constant 4.
    pub first: u32,
    /// The CD drive NUMBERS, ascending, one per mounted CD, the rest zero.
    /// This is the buffer AX=150Dh fills. 32 rather than 26 so the struct is a
    /// round 40 bytes and the FFI width lock is easy to read.
    pub letters: [u8; 32],
}

// Drive classes. DRV_CLASS_NONE is 0 so that a zeroed or out-of-range letter
// classifies as unusable rather than as a floppy, the same by-construction
// fail-closed property PROC_AS_INVALID and GUESTFS "unarmed = 0" rely on.
pub const DRV_CLASS_NONE: u32 = 0;
pub const DRV_CLASS_FLOPPY: u32 = 1;
pub const DRV_CLASS_FIXED: u32 = 2;
pub const DRV_CLASS_CDROM: u32 = 3;

// Image formats. Mirror DISKIMG_FMT_* in dos/diskimg.h.
const FMT_NONE: u32 = 0;
const FMT_ISO9660: u32 = 1;
const FMT_FAT12: u32 = 2;

/// Maximum images mounted at once, across all classes. 8 * 256 KiB = 2 MiB of
/// pinned cache, which is affordable against a 256 MB heap in a way that 24
/// mounts is not. Mirrored as DISKIMG_MAX_MOUNTS in dos/diskimg.h.
pub const DRVMAP_MAX_MOUNTS: u32 = 8;

// Placement errors. Negative, distinct, and each one maps to a different
// sentence in the UI, because "mount failed" with no reason is the thing that
// makes a user try the same click again.
pub const DRVMAP_E_RANGE: i32 = -1; // letter index outside A..Z
pub const DRVMAP_E_FMT: i32 = -2; // image is neither ISO 9660 nor FAT12
pub const DRVMAP_E_CLASS: i32 = -3; // that letter cannot hold that kind of image
pub const DRVMAP_E_FULL: i32 = -4; // no free letter left in the class
pub const DRVMAP_E_LIMIT: i32 = -5; // DRVMAP_MAX_MOUNTS already mounted
pub const DRVMAP_E_PATH: i32 = -6; // image path rejected before anything opened it

/// Class of a drive-letter index (0 = A .. 25 = Z). Out of range is NONE.
#[no_mangle]
pub extern "C" fn drvmap_class_rs(idx: u32) -> u32 {
    match idx {
        0 | 1 => DRV_CLASS_FLOPPY, // A:, B:
        2 => DRV_CLASS_FIXED,      // C:
        3 => DRV_CLASS_NONE,       // D: reserved, see the header comment
        4..=25 => DRV_CLASS_CDROM, // E: .. Z:
        _ => DRV_CLASS_NONE,
    }
}

/// The class an image of this format belongs on. An ISO is a CD, a FAT12 image
/// is a floppy. This is the function that makes "mount this ISO on A:" a typed
/// error instead of a drive that lies about what it is to GetDriveType and to
/// MSCDEX simultaneously.
#[no_mangle]
pub extern "C" fn drvmap_class_for_fmt_rs(fmt: u32) -> u32 {
    match fmt {
        FMT_ISO9660 => DRV_CLASS_CDROM,
        FMT_FAT12 => DRV_CLASS_FLOPPY,
        _ => DRV_CLASS_NONE,
    }
}

/// Lowest free letter index in `class`, given the occupancy bitmask
/// (bit N set = letter N currently has an image). Returns the index, or
/// DRVMAP_E_FULL when the class pool is exhausted.
#[no_mangle]
pub extern "C" fn drvmap_alloc_rs(class: u32, occupied: u32) -> i32 {
    if class == DRV_CLASS_NONE || class == DRV_CLASS_FIXED {
        return DRVMAP_E_CLASS;
    }
    let mut i: u32 = 0;
    while i < 26 {
        if drvmap_class_rs(i) == class && (occupied & (1u32 << i)) == 0 {
            return i as i32;
        }
        i += 1;
    }
    DRVMAP_E_FULL
}

/// THE ONE PLACEMENT DECISION. Everything the C side needs to know about where
/// a newly probed image goes.
///
/// `want`      requested letter index, or -1 for "pick one".
/// `fmt`       the format PROBED OFF THE IMAGE (DISKIMG_FMT_*), never a guess
///             from the filename: a .img can be either and a .iso can be
///             neither.
/// `occupied`  bitmask of letters that currently hold an image.
/// `mounted`   how many images are mounted right now (popcount of `occupied`,
///             passed separately so the caller's own count is what is checked).
///
/// Returns the letter index to use, or one of the negative DRVMAP_E_* codes.
///
/// Replacing an occupied letter (`want` is set in `occupied`) is the SWAP case:
/// allowed, letter-preserving, and not counted against DRVMAP_MAX_MOUNTS,
/// because the outgoing image's cache is freed before the incoming one's is
/// allocated.
#[no_mangle]
pub extern "C" fn drvmap_place_rs(want: i32, fmt: u32, occupied: u32, mounted: u32) -> i32 {
    let class = drvmap_class_for_fmt_rs(fmt);
    if class == DRV_CLASS_NONE {
        return DRVMAP_E_FMT;
    }

    if want >= 0 {
        if want > 25 {
            return DRVMAP_E_RANGE;
        }
        let w = want as u32;
        if drvmap_class_rs(w) != class {
            return DRVMAP_E_CLASS;
        }
        // Occupied means swap: allowed, and it consumes no new budget.
        if (occupied & (1u32 << w)) != 0 {
            return want;
        }
        if mounted >= DRVMAP_MAX_MOUNTS {
            return DRVMAP_E_LIMIT;
        }
        return want;
    }

    // Auto-placement always consumes budget, so the limit is checked first.
    if mounted >= DRVMAP_MAX_MOUNTS {
        return DRVMAP_E_LIMIT;
    }
    drvmap_alloc_rs(class, occupied)
}

/// Derive the MSCDEX answer from the live CD occupancy mask.
///
/// `cd_mask` has bit N set for every letter N that currently holds a CD image;
/// bits for non-CD letters are ignored rather than trusted, so a caller that
/// passes the whole occupancy mask (floppies included) still gets the right
/// answer. Returns the drive count, and fills `*out` when `out` is non-null.
///
/// CONTIGUITY, HONESTLY. INT 2Fh AX=1500h reports a COUNT and a FIRST DRIVE,
/// a pair that implies one contiguous block, and real MSCDEX assigned one.
/// Lowest-free allocation keeps our letters contiguous in the ordinary case and
/// refills a hole on the next mount, but ejecting the middle of three discs
/// does leave E: and G: with F: empty. A program that walks first..first+count-1
/// then probes F: with AX=150Bh is told "not a CD-ROM", which is true, and sees
/// two drives instead of three. AX=150Dh lists the real letters and is the
/// authoritative answer. The alternative, renumbering the surviving discs to
/// close the hole, would move a mounted disc's letter out from under a running
/// program, which is worse than a hole.
///
/// # Safety
/// `out` must be null or a valid, writable, correctly aligned MscdexInfo. The
/// C side only ever passes the address of a stack MscdexInfo, and the struct's
/// size is locked against the C mirror by _Static_assert in dos/diskimg.c.
#[no_mangle]
pub extern "C" fn drvmap_mscdex_rs(cd_mask: u32, out: *mut MscdexInfo) -> i32 {
    let mut count: u32 = 0;
    let mut first: u32 = 0;
    let mut letters = [0u8; 32];

    let mut i: u32 = 0;
    while i < 26 {
        if drvmap_class_rs(i) == DRV_CLASS_CDROM && (cd_mask & (1u32 << i)) != 0 {
            if count == 0 {
                first = i;
            }
            if (count as usize) < letters.len() {
                letters[count as usize] = i as u8;
            }
            count += 1;
        }
        i += 1;
    }

    if !out.is_null() {
        // SAFETY: `out` is non-null here and the C caller's contract (above) is
        // that it points at a whole MscdexInfo. Every field is written, so no
        // uninitialised byte of the struct is left for the caller to read.
        unsafe {
            (*out).count = count;
            (*out).first = first;
            (*out).letters = letters;
        }
    }
    count as i32
}

/// Validate the image path a Ring-3 caller handed us, BEFORE anything opens it.
///
/// This runs on an attacker-controlled string, which is the reason it is here
/// and not in the C. It is deliberately strict:
///
///  * absolute only. A relative path would be resolved against whatever the
///    kernel's notion of "current" happens to be, which is not a thing a
///    permission check can be written against.
///  * no ".." component. perms_check() CANONICALIZES its argument (permpath.rs
///    pops "..") and the image opener does not, so a path containing ".." is
///    one the two halves could interpret differently. Rejecting it makes them
///    provably the same string rather than arguably the same path.
///  * no backslashes. Native MayteraOS paths use '/', and accepting '\' would
///    mean two spellings of one file, which is how a denylist gets bypassed.
///  * no control characters, no empty path, bounded length.
///
/// Returns the string length on success, or DRVMAP_E_PATH.
///
/// # Safety
/// `path` must point to at least `maxlen` readable bytes, or to a NUL-terminated
/// string shorter than that. The C caller passes a KERNEL buffer that has
/// already been bounced out of user space with sc_bounce_str(), never a raw
/// user pointer: validating the user copy and then opening the user copy again
/// would be a time-of-check/time-of-use gap (argtab.rs is explicit that its
/// pointer validation is not TOCTOU-safe either).
#[no_mangle]
pub extern "C" fn drvmap_path_ok_rs(path: *const u8, maxlen: u32) -> i32 {
    if path.is_null() || maxlen == 0 || maxlen > 4096 {
        return DRVMAP_E_PATH;
    }
    // SAFETY: the caller guarantees `maxlen` readable bytes at `path` (see the
    // contract above). The slice spans exactly that, so every index below is
    // inside the caller's buffer and a missing NUL is a rejection, not a walk
    // off the end.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(path, maxlen as usize) };

    let mut len: usize = 0;
    while len < s.len() && s[len] != 0 {
        len += 1;
    }
    if len == 0 || len >= s.len() {
        return DRVMAP_E_PATH; // empty, or no NUL inside the buffer
    }
    if s[0] != b'/' {
        return DRVMAP_E_PATH; // must be absolute
    }

    let mut i: usize = 0;
    while i < len {
        let c = s[i];
        if c < 0x20 || c == 0x7F || c == b'\\' {
            return DRVMAP_E_PATH;
        }
        i += 1;
    }

    // Reject any ".." COMPONENT (not a substring: "/A..B" is a legal filename).
    let mut start: usize = 0;
    let mut j: usize = 0;
    while j <= len {
        if j == len || s[j] == b'/' {
            if j - start == 2 && s[start] == b'.' && s[start + 1] == b'.' {
                return DRVMAP_E_PATH;
            }
            start = j + 1;
        }
        j += 1;
    }

    len as i32
}

/// Boot-time PROPERTY TEST of the placement rules. Not a differential: there is
/// no C twin to compare against, so what is proven here is that the allocator
/// obeys its own stated invariants on this exact build.
///
/// Returns the number of FAILURES (0 = pass) and writes the per-group counts
/// through `out_checks` so the one serial line can report how much was actually
/// exercised, rather than a bare PASS that would look identical if the test had
/// silently run zero cases (the zero-callers trap).
///
/// # Safety
/// `out_checks` must be null or point at a writable u32.
#[no_mangle]
pub extern "C" fn drvmap_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut fails: u32 = 0;
    let mut checks: u32 = 0;

    macro_rules! want {
        ($cond:expr) => {{
            checks += 1;
            if !($cond) {
                fails += 1;
            }
        }};
    }

    // Classes are exactly as documented, for every letter, with no gaps.
    want!(drvmap_class_rs(0) == DRV_CLASS_FLOPPY);
    want!(drvmap_class_rs(1) == DRV_CLASS_FLOPPY);
    want!(drvmap_class_rs(2) == DRV_CLASS_FIXED);
    want!(drvmap_class_rs(3) == DRV_CLASS_NONE);
    want!(drvmap_class_rs(4) == DRV_CLASS_CDROM);
    want!(drvmap_class_rs(25) == DRV_CLASS_CDROM);
    want!(drvmap_class_rs(26) == DRV_CLASS_NONE);
    want!(drvmap_class_rs(0xFFFF_FFFF) == DRV_CLASS_NONE);

    // Auto-placement walks the class pool from the bottom and never leaves it.
    want!(drvmap_place_rs(-1, FMT_ISO9660, 0, 0) == 4); // first CD is E:
    want!(drvmap_place_rs(-1, FMT_ISO9660, 1 << 4, 1) == 5); // then F:
    want!(drvmap_place_rs(-1, FMT_ISO9660, (1 << 4) | (1 << 5), 2) == 6); // then G:
    want!(drvmap_place_rs(-1, FMT_FAT12, 0, 0) == 0); // first floppy is A:
    want!(drvmap_place_rs(-1, FMT_FAT12, 1 << 0, 1) == 1); // then B:
    want!(drvmap_place_rs(-1, FMT_FAT12, 0b11, 2) == DRVMAP_E_FULL); // only two

    // A hole is refilled by the next mount, which is why allocation is
    // lowest-free rather than next-highest.
    want!(drvmap_place_rs(-1, FMT_ISO9660, (1 << 4) | (1 << 6), 2) == 5);

    // Class typing: an ISO may not land on a floppy letter, a floppy image may
    // not land on a CD letter, and nothing at all may land on C: or D:.
    want!(drvmap_place_rs(0, FMT_ISO9660, 0, 0) == DRVMAP_E_CLASS);
    want!(drvmap_place_rs(4, FMT_FAT12, 0, 0) == DRVMAP_E_CLASS);
    want!(drvmap_place_rs(2, FMT_ISO9660, 0, 0) == DRVMAP_E_CLASS);
    want!(drvmap_place_rs(2, FMT_FAT12, 0, 0) == DRVMAP_E_CLASS);
    want!(drvmap_place_rs(3, FMT_ISO9660, 0, 0) == DRVMAP_E_CLASS);
    want!(drvmap_place_rs(26, FMT_ISO9660, 0, 0) == DRVMAP_E_RANGE);
    want!(drvmap_place_rs(-1, FMT_NONE, 0, 0) == DRVMAP_E_FMT);

    // Swap keeps the letter and costs no budget, even at the mount limit.
    want!(drvmap_place_rs(4, FMT_ISO9660, 1 << 4, DRVMAP_MAX_MOUNTS) == 4);
    // A NEW mount at the limit is refused, explicit letter or not.
    want!(drvmap_place_rs(5, FMT_ISO9660, 1 << 4, DRVMAP_MAX_MOUNTS) == DRVMAP_E_LIMIT);
    want!(drvmap_place_rs(-1, FMT_ISO9660, 1 << 4, DRVMAP_MAX_MOUNTS) == DRVMAP_E_LIMIT);

    // MSCDEX is derived, and counts only CD letters even when handed floppies.
    let mut mi = MscdexInfo { count: 0, first: 0, letters: [0u8; 32] };
    want!(drvmap_mscdex_rs(0, &mut mi) == 0);
    want!(mi.count == 0 && mi.first == 0);
    want!(drvmap_mscdex_rs(1 << 4, &mut mi) == 1);
    want!(mi.count == 1 && mi.first == 4 && mi.letters[0] == 4);
    want!(drvmap_mscdex_rs((1 << 4) | (1 << 5), &mut mi) == 2);
    want!(mi.count == 2 && mi.first == 4 && mi.letters[0] == 4 && mi.letters[1] == 5);
    // A hole: E: and G: mounted, F: empty. Count 2, first E:, list is the truth.
    want!(drvmap_mscdex_rs((1 << 4) | (1 << 6), &mut mi) == 2);
    want!(mi.count == 2 && mi.first == 4 && mi.letters[0] == 4 && mi.letters[1] == 6);
    // Floppy bits must not be counted as CD drives.
    want!(drvmap_mscdex_rs(0b11, &mut mi) == 0);
    want!(mi.count == 0);
    // Null out is legal and still returns the count.
    want!(drvmap_mscdex_rs(1 << 4, core::ptr::null_mut()) == 1);

    // Path validation, including the two spellings that would let a denylist be
    // walked around.
    want!(drvmap_path_ok_rs(b"/WINDIR/RA1.ISO\0".as_ptr(), 16) == 15);
    want!(drvmap_path_ok_rs(b"WINDIR/RA1.ISO\0".as_ptr(), 15) == DRVMAP_E_PATH);
    want!(drvmap_path_ok_rs(b"/A/../CONFIG/KIMI.KEY\0".as_ptr(), 22) == DRVMAP_E_PATH);
    want!(drvmap_path_ok_rs(b"/..\0".as_ptr(), 4) == DRVMAP_E_PATH);
    want!(drvmap_path_ok_rs(b"/WINDIR\\RA1.ISO\0".as_ptr(), 16) == DRVMAP_E_PATH);
    want!(drvmap_path_ok_rs(b"\0".as_ptr(), 1) == DRVMAP_E_PATH);
    want!(drvmap_path_ok_rs(core::ptr::null(), 16) == DRVMAP_E_PATH);
    // No NUL inside the buffer is a rejection, not a read past the end.
    want!(drvmap_path_ok_rs(b"/AAAA".as_ptr(), 5) == DRVMAP_E_PATH);
    // ".." as a SUBSTRING of a real name is fine; only a whole component is not.
    want!(drvmap_path_ok_rs(b"/WINDIR/A..B.ISO\0".as_ptr(), 17) == 16);

    if !out_checks.is_null() {
        // SAFETY: non-null and the caller's contract is a writable u32.
        unsafe {
            *out_checks = checks;
        }
    }
    fails as i32
}
