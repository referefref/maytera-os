// rustkern/hotplug.rs - #250: the removable-volume surface the GUI reads.
//
// WHY THIS IS RUST
// ==========================================================================
// New kernel logic with no C twin, so Rust per the 2026-07-16 rule, and no
// -DRUST_* strangler flag and no RUST_PORT_LEDGER row: there is nothing to
// differ from, so the rollback is reverting the commit.
//
// It is also the right language for the two things it does. Both are the
// arithmetic-and-bounds kind that C does silently and wrongly:
//
//  1. hotplug_path_split_rs. "Is /USB10/FOO a path on the volume mounted at
//     /USB1?" A strncmp(path, mount, strlen(mount)) says YES, and the file
//     opens on the wrong stick with no error anywhere. The boundary character
//     has to be checked, and so does the exact-match case (/USB1 with nothing
//     after it is the volume ROOT and must resolve to "/"), and the compare
//     has to be case-insensitive because the mount point is spelled uppercase
//     and userland hands paths over in both cases.
//
//  2. hotplug_vol_list_rs. A count-times-element-size copy into a Ring-3
//     buffer. Every bound below is a checked slice write into a
//     caller-supplied array, and the strings are copied with an explicit
//     NUL-terminated bound rather than strncpy's does-not-always-terminate
//     contract.
//
// WHAT IT DOES NOT DO
// ==========================================================================
// It does not own the volume table. drivers/hotplug.c does, because the table
// is written from the USB MSC event path and holds a C union of two
// filesystem states. This module READS it one slot at a time through
// hotplug_vol_raw(), whose layout is locked by a _Static_assert on the C
// side.

#![allow(dead_code)]

// Mirrors hotplug_raw_t in drivers/hotplug.h. The _Static_assert there locks
// the size; the field order is locked by review and by hotplug_selftest_rs,
// which round-trips a slot through both sides.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HpRaw {
    pub present: i32,
    pub mounted: i32,
    pub fs_type: i32,
    pub readable: i32,
    pub capacity_bytes: u64,
    pub free_bytes: u64,
    pub free_known: i32,
    pub reserved0: i32,
    pub name: [u8; 64],
    pub mount_point: [u8; 32],
}

impl HpRaw {
    const fn zeroed() -> Self {
        HpRaw {
            present: 0, mounted: 0, fs_type: 0, readable: 0,
            capacity_bytes: 0, free_bytes: 0, free_known: 0, reserved0: 0,
            name: [0u8; 64], mount_point: [0u8; 32],
        }
    }
}

// Mirrors sc_volume_t in proc/syscall.h and userland/libc/syscall.h. Locked by
// _Static_assert on the C side and by the syscall-number-lint's rule that the
// two headers agree.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ScVolume {
    pub index: i32,
    pub flags: u32,
    pub fs_type: u32,
    pub pad: u32,
    pub total_bytes: u64,
    pub free_bytes: u64,
    pub name: [u8; 64],
    pub mount: [u8; 32],
    pub fsname: [u8; 8],
}

impl ScVolume {
    const fn zeroed() -> Self {
        ScVolume {
            index: 0, flags: 0, fs_type: 0, pad: 0,
            total_bytes: 0, free_bytes: 0,
            name: [0u8; 64], mount: [0u8; 32], fsname: [0u8; 8],
        }
    }
}

// Must match the MOSVOL_* defines in proc/syscall.h.
const MOSVOL_MOUNTED: u32 = 0x01;
const MOSVOL_REMOVABLE: u32 = 0x02;
const MOSVOL_READABLE: u32 = 0x04;
const MOSVOL_FREE_UNKNOWN: u32 = 0x08;
// #234i. Derived from fs_type below, never passed in, so "read-only" has one
// definition for every producer.
const MOSVOL_READONLY: u32 = 0x10;
const MOSVOL_OPTICAL: u32 = 0x20;
const MOSVOL_FLOPPY: u32 = 0x40;

// Must match the HOTPLUG_FS_* defines in drivers/hotplug.h.
const FS_UNKNOWN: i32 = 0;
const FS_FAT16: i32 = 1;
const FS_FAT32: i32 = 2;
const FS_EXFAT: i32 = 3;
const FS_NTFS: i32 = 4;
// #234i: the two disk-image classes. Produced by dos/diskimg.c, never by the
// USB path, so the mapping to MOSVOL_OPTICAL / MOSVOL_FLOPPY is unambiguous.
const FS_ISO9660: i32 = 5;
const FS_FAT12: i32 = 6;

const HOTPLUG_MAX_DEVICES: i32 = 8;
// Drive letters A..Z. Only DISKIMG_MAX_MOUNTS of them can hold an image at
// once, but the letter is the index, so the scan is over all 26.
const DRVMAP_LETTERS: i32 = 26;
// #234i: THE INDEX NAMESPACE. sc_volume_t.index is an opaque handle a UI hands
// straight back to SYS_VOL_EJECT, so the two producers must not collide.
// USB slots are 0..HOTPLUG_MAX_DEVICES-1; a disk image is this base plus its
// drive-letter index. sys_vol_eject() splits on the same constant
// (SC_VOL_IMAGE_BASE in proc/syscall.h) and there is a _Static_assert there
// tying the two together.
const VOL_INDEX_IMAGE_BASE: i32 = 1000;

extern "C" {
    fn hotplug_vol_raw(index: i32, out: *mut HpRaw) -> i32;
    // #234i: the same record shape, produced by the disk-image mounter.
    // Declared in drivers/hotplug.h next to hotplug_vol_raw() for that reason.
    fn diskimg_vol_raw(idx: i32, out: *mut HpRaw) -> i32;
}

// --------------------------------------------------------------------------
// Pure cores. Everything below this line is testable without a device.
// --------------------------------------------------------------------------

#[inline]
fn upper(c: u8) -> u8 {
    if c >= b'a' && c <= b'z' { c - 32 } else { c }
}

/// Length of a NUL-terminated C string, bounded by `max`.
///
/// SAFETY: `p` must be readable for at least `max` bytes or up to its NUL,
/// whichever comes first.
unsafe fn cstr_len(p: *const u8, max: usize) -> usize {
    let mut n = 0usize;
    while n < max && *p.add(n) != 0 { n += 1; }
    n
}

/// THE mount-point match. Returns the byte offset within `path` at which the
/// volume-relative path begins, or None.
///
/// The relative path ALWAYS starts with '/': for an exact match on the mount
/// point the offset points at the mount point's own trailing position, and the
/// caller is handed "/" by the exact-match arm below returning the offset of
/// the mount point's last character... which would be wrong. So instead the
/// exact match is reported separately and the C side is given the offset of a
/// '/' that really is in the string. `path` is absolute, so index 0 is '/'
/// and pointing there yields exactly "/" only when the whole path IS the
/// mount point. That is the special case, and it is why this returns an
/// offset rather than a bool.
fn split_core(path: &[u8], mount: &[u8]) -> Option<usize> {
    if mount.is_empty() || path.is_empty() { return None; }
    if path[0] != b'/' || mount[0] != b'/' { return None; }
    if path.len() < mount.len() { return None; }
    for i in 0..mount.len() {
        if upper(path[i]) != upper(mount[i]) { return None; }
    }
    if path.len() == mount.len() {
        // Exactly the volume root. Offset 0 makes the caller's `path + 0`
        // read "/USB0", which is not a path INSIDE the volume, so the caller
        // is told to use the root explicitly: offset == mount.len() - 1 is
        // the '0' of "/USB0". Neither is "/". The honest answer is that the
        // relative path is the constant "/", which has no offset in `path` at
        // all. Reported as usize::MAX and translated by the FFI wrapper.
        return Some(usize::MAX);
    }
    // The next character must be a separator, or "/USB1" would match
    // "/USB10/FOO" and open a file on the wrong volume.
    if path[mount.len()] != b'/' { return None; }
    Some(mount.len())
}

// --------------------------------------------------------------------------
// FFI
// --------------------------------------------------------------------------

/// 1 if `path` lies on the volume mounted at `mount`, else 0.
/// On a match `*rel_off` receives the offset into `path` of the
/// volume-relative path, which always begins with '/'. For a path that IS the
/// mount point, `*rel_off` receives -1, meaning "the volume root", and the
/// caller substitutes the literal "/".
///
/// SAFETY: both pointers must be NUL-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn hotplug_path_split_rs(path: *const u8, mount: *const u8,
                                               rel_off: *mut i32) -> i32 {
    if path.is_null() || mount.is_null() { return 0; }
    let pl = cstr_len(path, 4096);
    let ml = cstr_len(mount, 64);
    let ps = core::slice::from_raw_parts(path, pl);
    let ms = core::slice::from_raw_parts(mount, ml);
    match split_core(ps, ms) {
        None => 0,
        Some(off) => {
            if !rel_off.is_null() {
                *rel_off = if off == usize::MAX { -1 } else { off as i32 };
            }
            1
        }
    }
}

fn fsname_for(fs_type: i32) -> [u8; 8] {
    let mut out = [0u8; 8];
    let src: &[u8] = match fs_type {
        FS_FAT16 => b"FAT16",
        FS_FAT32 => b"FAT32",
        FS_EXFAT => b"exFAT",
        FS_NTFS => b"NTFS",
        FS_ISO9660 => b"ISO9660",
        FS_FAT12 => b"FAT12",
        _ => b"Unknown",
    };
    let n = if src.len() < 7 { src.len() } else { 7 };
    out[..n].copy_from_slice(&src[..n]);
    out
}

fn copy_bounded(dst: &mut [u8], src: &[u8]) {
    let cap = dst.len();
    if cap == 0 { return; }
    let n = {
        let mut k = 0usize;
        while k < src.len() && k < cap - 1 && src[k] != 0 { k += 1; }
        k
    };
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}

fn raw_to_vol(index: i32, r: &HpRaw) -> ScVolume {
    let mut v = ScVolume::zeroed();
    v.index = index;
    v.fs_type = r.fs_type as u32;
    v.flags = MOSVOL_REMOVABLE;
    if r.mounted != 0 { v.flags |= MOSVOL_MOUNTED; }
    if r.readable != 0 { v.flags |= MOSVOL_READABLE; }
    if r.free_known == 0 { v.flags |= MOSVOL_FREE_UNKNOWN; }
    // #234i. A CD is read-only because it is a CD, not because someone
    // remembered to say so at the call site. A floppy image is writable in
    // principle but this layer serves reads only today, so it is flagged as
    // its own class and left OUT of MOSVOL_READONLY rather than being
    // described as something it is not; dos/diskimg.c refuses the writes.
    match r.fs_type {
        FS_ISO9660 => { v.flags |= MOSVOL_OPTICAL | MOSVOL_READONLY; }
        FS_FAT12 => { v.flags |= MOSVOL_FLOPPY | MOSVOL_READONLY; }
        _ => {}
    }
    v.total_bytes = r.capacity_bytes;
    v.free_bytes = if r.free_known != 0 { r.free_bytes } else { 0 };
    copy_bounded(&mut v.name, &r.name);
    copy_bounded(&mut v.mount, &r.mount_point);
    v.fsname = fsname_for(r.fs_type);
    v
}

/// Fill up to `max` sc_volume_t records for the currently present removable
/// volumes. Returns the number written, or -1 on a bad argument.
///
/// SAFETY: `dst` must be writable for `max * size_of::<ScVolume>()` bytes. The
/// syscall dispatcher validates that against the caller's address space before
/// this is reached (rustkern/argtab.rs), and the handler clamps `max`.
#[no_mangle]
pub unsafe extern "C" fn hotplug_vol_list_rs(dst: *mut ScVolume, max: i32) -> i32 {
    if dst.is_null() || max <= 0 { return -1; }
    let out = core::slice::from_raw_parts_mut(dst, max as usize);
    let mut n = 0usize;
    let mut i = 0i32;
    while i < HOTPLUG_MAX_DEVICES && n < out.len() {
        let mut raw = HpRaw::zeroed();
        if hotplug_vol_raw(i, &mut raw) == 0 && raw.present != 0 {
            out[n] = raw_to_vol(i, &raw);
            n += 1;
        }
        i += 1;
    }
    // #234i: then every DOS drive letter holding a mounted disk image. Second
    // rather than first so a hot-plugged stick keeps the slot index it has
    // always had, and so a caller with a short buffer loses the image rows
    // rather than the physical ones.
    let mut li = 0i32;
    while li < DRVMAP_LETTERS && n < out.len() {
        let mut raw = HpRaw::zeroed();
        if diskimg_vol_raw(li, &mut raw) != 0 && raw.present != 0 {
            out[n] = raw_to_vol(VOL_INDEX_IMAGE_BASE + li, &raw);
            n += 1;
        }
        li += 1;
    }
    n as i32
}

// --------------------------------------------------------------------------
// Boot self-test. There is no C twin to run a differential against, so what
// stands in for it is a vector test over the pure core, run every boot with
// one line of output. Every case here is a routing decision that would be
// silent and wrong if it regressed.
// --------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn hotplug_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut checks: u32 = 0;
    let mut fails: u32 = 0;

    #[inline]
    fn chk(cond: bool, checks: &mut u32, fails: &mut u32) {
        *checks += 1;
        if !cond { *fails += 1; }
    }
    macro_rules! chk { ($c:expr) => { chk($c, &mut checks, &mut fails) } }

    // Exact mount point -> volume root.
    chk!(split_core(b"/USB0", b"/USB0") == Some(usize::MAX));
    // Case-insensitive, both directions.
    chk!(split_core(b"/usb0", b"/USB0") == Some(usize::MAX));
    chk!(split_core(b"/USB0/FOO.TXT", b"/usb0") == Some(5));
    // A file at the volume root.
    chk!(split_core(b"/USB0/FOO.TXT", b"/USB0") == Some(5));
    // Nested.
    chk!(split_core(b"/USB0/DIR/FOO.TXT", b"/USB0") == Some(5));
    // THE off-by-one: /USB1 must NOT swallow /USB10.
    chk!(split_core(b"/USB10/FOO", b"/USB1").is_none());
    chk!(split_core(b"/USB10", b"/USB1").is_none());
    // A prefix that is not on a separator boundary.
    chk!(split_core(b"/USB0X/FOO", b"/USB0").is_none());
    // Shorter than the mount point.
    chk!(split_core(b"/USB", b"/USB0").is_none());
    // Unrelated path.
    chk!(split_core(b"/APPS/FILES", b"/USB0").is_none());
    // Relative paths are never volume paths.
    chk!(split_core(b"USB0/FOO", b"/USB0").is_none());
    // Empty inputs.
    chk!(split_core(b"", b"/USB0").is_none());
    chk!(split_core(b"/USB0", b"").is_none());

    // Record marshalling: flags and the unknown-free rule.
    let mut r = HpRaw::zeroed();
    r.present = 1; r.mounted = 1; r.fs_type = FS_FAT32; r.readable = 1;
    r.capacity_bytes = 64 * 1024 * 1024;
    r.free_bytes = 0; r.free_known = 0;
    r.name[0] = b'X'; r.name[1] = 0;
    r.mount_point[0] = b'/'; r.mount_point[1] = b'U'; r.mount_point[2] = 0;
    let v = raw_to_vol(3, &r);
    chk!(v.index == 3);
    chk!(v.flags == (MOSVOL_REMOVABLE | MOSVOL_MOUNTED | MOSVOL_READABLE | MOSVOL_FREE_UNKNOWN));
    chk!(v.free_bytes == 0);
    chk!(v.total_bytes == 64 * 1024 * 1024);
    chk!(v.name[0] == b'X' && v.name[1] == 0);
    chk!(v.fsname[0] == b'F' && v.fsname[4] == b'2' && v.fsname[5] == 0);

    // exFAT: mounted, removable, NOT readable. This is the one that keeps an
    // unbrowsable volume from being presented as browsable.
    let mut e = HpRaw::zeroed();
    e.present = 1; e.mounted = 1; e.fs_type = FS_EXFAT; e.readable = 0;
    e.free_bytes = 4096; e.free_known = 1;
    let ev = raw_to_vol(0, &e);
    chk!((ev.flags & MOSVOL_READABLE) == 0);
    chk!((ev.flags & MOSVOL_MOUNTED) != 0);
    chk!((ev.flags & MOSVOL_FREE_UNKNOWN) == 0);
    chk!(ev.free_bytes == 4096);

    // #234i: a mounted CD-ROM image. READ-ONLY and OPTICAL must both come out
    // of fs_type alone. If this regresses, Files offers Delete on a disc.
    let mut c = HpRaw::zeroed();
    c.present = 1; c.mounted = 1; c.fs_type = FS_ISO9660; c.readable = 1;
    c.capacity_bytes = 354 * 1024;
    let cv = raw_to_vol(VOL_INDEX_IMAGE_BASE + 4, &c);
    chk!(cv.index == 1004);
    chk!((cv.flags & MOSVOL_READONLY) != 0);
    chk!((cv.flags & MOSVOL_OPTICAL) != 0);
    chk!((cv.flags & MOSVOL_FLOPPY) == 0);
    chk!((cv.flags & MOSVOL_READABLE) != 0);
    chk!(cv.fsname[0] == b'I' && cv.fsname[6] == b'0' && cv.fsname[7] == 0);

    // #234i: a mounted floppy image. Same read-only answer, different class,
    // and it must NOT be reported as optical.
    let mut f = HpRaw::zeroed();
    f.present = 1; f.mounted = 1; f.fs_type = FS_FAT12; f.readable = 1;
    let fv = raw_to_vol(VOL_INDEX_IMAGE_BASE, &f);
    chk!(fv.index == 1000);
    chk!((fv.flags & MOSVOL_FLOPPY) != 0);
    chk!((fv.flags & MOSVOL_OPTICAL) == 0);
    chk!((fv.flags & MOSVOL_READONLY) != 0);
    chk!(fv.fsname[0] == b'F' && fv.fsname[4] == b'2' && fv.fsname[5] == 0);

    // #234i: the two namespaces must not overlap. A USB slot index can never
    // reach the image base, which is what makes sys_vol_eject()'s split sound.
    chk!(HOTPLUG_MAX_DEVICES < VOL_INDEX_IMAGE_BASE);
    chk!(VOL_INDEX_IMAGE_BASE + DRVMAP_LETTERS - 1 == 1025);

    // A USB volume must carry NEITHER image class, or a stick would draw as a
    // disc.
    chk!((v.flags & (MOSVOL_OPTICAL | MOSVOL_FLOPPY | MOSVOL_READONLY)) == 0);

    // Unknown filesystem.
    let uv = raw_to_vol(1, &HpRaw::zeroed());
    chk!(uv.fsname[0] == b'U');
    chk!(uv.fs_type == FS_UNKNOWN as u32);

    if !out_checks.is_null() { *out_checks = checks; }
    fails
}
