// usbvol.rs - #740: geometry and validation for DATA VOLUMES that live in the
//             unpartitioned TAIL of the boot device, outside the OS image.
//
// WHY THIS EXISTS
// ---------------
// The golden image's ext2 root is 1.62 GB with ~1.13 GB free. Discworld 2's
// data is 1.54 GB COMPRESSED. It does not fit and no amount of tidying will
// make it fit. The boot stick, by contrast, is 124 GB with ~1.8 GB used: about
// 122 GB of the stick is unpartitioned space that the OS image never touches.
// A large game's data belongs there, read from the stick on demand, NOT copied
// into the image and NOT pulled into the TO-RAM window.
//
// THE ON-DISK CONTRACT, AND WHY IT INVENTS NO NEW FORMAT
// ------------------------------------------------------
// A data volume is a RAW ISO 9660 IMAGE written directly into the device at a
// 1 MiB-aligned byte offset at or above USBVOL_BASE. That is the whole format.
// There is no superblock of our own, no directory of our own, no magic of our
// own, because the tree already has every piece needed to consume it:
//   - ISO 9660 is already parsed here, in Rust (rustkern/iso9660.rs, #196).
//   - dos/imgfile.c already streams an arbitrarily large image through a 256 KiB
//     cache, so a 100 GB volume costs 256 KiB of RAM, not 100 GB.
//   - dos/diskimg.c already mounts such an image on a CD drive letter with a
//     mount generation, a refcount and a read turnstile, and fat_open() already
//     redirects the guest to it.
// The only thing missing was where the BYTES come from, and that is one new
// backing kind in imgfile.c. ISO 9660 also self-describes its own length in its
// primary volume descriptor, which is what lets several volumes be CHAINED one
// after another with no table of contents anywhere.
//
// WHY THE TAIL AND NOT A THIRD GPT PARTITION
// ------------------------------------------
// Two independent reasons, both measured, not preferred:
//  1. build/invariant-gate.sh:320-332 FAILS an image that does not have EXACTLY
//     two GPT partitions with type codes "EF00 8300". A p3 in the golden image
//     is therefore not a design choice, it is a build failure.
//  2. A p3 added to the STICK after the image is written would be erased the
//     next time a golden is written, because that write replaces the GPT at LBA
//     0 with the image's own two-entry table. The partition table is the one
//     structure a golden refresh is GUARANTEED to overwrite. Data in the tail
//     past the image's length is the one region it is guaranteed NOT to touch.
// So the tail placement is not a workaround for the gate. It is the option that
// survives the operation (writing a new golden) that the other option does not.
//
// WHY USBVOL_BASE IS 4 GiB
// ------------------------
// It must clear two things, and 4 GiB clears both with room:
//  - the OS image itself (the two-partition golden is ~1.9 GB today), and
//  - the TO-RAM window, which fs/blockdev.c caps at BLK_MAX_CHUNKS *
//    BLK_CHUNK_BYTES = 1280 * 2 MiB = 2560 MiB. An offset inside that window
//    would be served out of the RAM copy, which is exactly the "off the
//    ramdisk" behaviour this feature exists to avoid, AND would mean the volume
//    had to fit in RAM.
// Every byte at or above USBVOL_BASE is read by blk_read() straight from the
// device (fs/blockdev.c:489 takes the RAM path only when lba + count <=
// g_ram_sectors), which is the requirement stated as "read off the usb not off
// the ramdisk".
//
// LANGUAGE NOTE (standing Rust-first rule): this module is Rust because it is
// pure integer policy over UNTRUSTED ON-DISK BYTES. Every number in here comes
// out of an image found by scanning a raw device, and each one is used to
// compute a byte offset and a length that a later read will trust. That is
// precisely the arithmetic that should not be in C. The I/O glue that calls it
// (dos/usbvol.c) is C for the same reason dos/imgfile.c is: it is entangled
// with blk_read() and kmalloc.
#![allow(dead_code)]

// ---------------------------------------------------------------------------
// The contract's constants. dos/usbvol.c does NOT mirror them; it fetches them
// at runtime through usbvol_consts_rs(). A mirrored constant is a constant that
// can drift, and the C side has no need to know these at compile time.
// ---------------------------------------------------------------------------

/// Lowest byte offset a data volume may start at. See the header comment: it
/// clears both the OS image and the TO-RAM window.
pub const USBVOL_BASE: u64 = 4u64 << 30; // 4 GiB

/// Volumes start on a 1 MiB boundary. Large enough to be obviously deliberate
/// when read in a hex dump, and a multiple of every plausible sector size.
pub const USBVOL_ALIGN: u64 = 1u64 << 20; // 1 MiB

/// How many volumes the chain walk will follow before giving up. Matches
/// DISKIMG_MAX_MOUNTS: following a ninth volume when only eight can be mounted
/// would just be a slower way to fail.
pub const USBVOL_MAX: u32 = 8;

/// ISO 9660 logical sector size, and the sector the primary volume descriptor
/// lives at. Fixed by ECMA-119, not tunable.
pub const ISO_SECT: u64 = 2048;
pub const ISO_PVD_SECT: u64 = 16;

/// Byte offset from the start of a volume to its primary volume descriptor.
pub const USBVOL_PVD_OFF: u64 = ISO_PVD_SECT * ISO_SECT; // 32768

/// Smallest span a probe must be able to read to decide anything at all.
pub const USBVOL_PROBE_SPAN: u64 = USBVOL_PVD_OFF + ISO_SECT; // 34816

// Error codes. Kept distinct so a log line can say WHICH check refused.
pub const USBVOL_E_ARG: i64 = -1; // null pointer or short buffer
pub const USBVOL_E_MAGIC: i64 = -2; // not "CD001" / not a primary descriptor
pub const USBVOL_E_GEOM: i64 = -3; // implausible block size or block count
pub const USBVOL_E_RANGE: i64 = -4; // does not fit the device, or overflows
pub const USBVOL_E_END: i64 = -5; // no further candidate in the chain

#[inline]
fn rd_u16_le(b: &[u8], o: usize) -> u16 {
    (b[o] as u16) | ((b[o + 1] as u16) << 8)
}

#[inline]
fn rd_u32_le(b: &[u8], o: usize) -> u32 {
    (b[o] as u32) | ((b[o + 1] as u32) << 8) | ((b[o + 2] as u32) << 16) | ((b[o + 3] as u32) << 24)
}

#[inline]
fn rd_u32_be(b: &[u8], o: usize) -> u32 {
    ((b[o] as u32) << 24) | ((b[o + 1] as u32) << 16) | ((b[o + 2] as u32) << 8) | (b[o + 3] as u32)
}

#[inline]
fn rd_u16_be(b: &[u8], o: usize) -> u16 {
    ((b[o] as u16) << 8) | (b[o + 1] as u16)
}

/// Round `v` up to the next multiple of USBVOL_ALIGN, or None on overflow.
fn align_up(v: u64) -> Option<u64> {
    let m = USBVOL_ALIGN - 1;
    v.checked_add(m).map(|x| x & !m)
}

/// Length in BYTES of the ISO 9660 volume whose primary volume descriptor is in
/// `sec`, or a negative USBVOL_E_* code.
///
/// `sec` must point at the 2048 bytes at volume offset USBVOL_PVD_OFF. This is
/// the ONLY function that turns bytes found by scanning a raw device into a
/// length, so it is deliberately strict:
///
///  - the type byte must be 1 (primary) and the identifier must be "CD001" with
///    version 1. At a 1 MiB-aligned offset that alone is a ~2^48 filter against
///    finding a "volume" in unrelated data.
///  - the block size must be a power of two in 512..=4096 and the block count
///    must be at least ISO_PVD_SECT + 1, because a volume that does not contain
///    its own descriptor is not a volume.
///  - the multiply is checked, so a hostile 0xFFFFFFFF block count cannot wrap
///    into a small positive length that would later pass a range check.
///
/// ECMA-119 stores volume_space_size and logical_block_size BOTH-ENDIAN. The
/// little-endian half is the one used; the big-endian half is consulted only
/// when the LE half is zero, which is the single case where LE is unusable
/// rather than merely disagreeing. Requiring the two to agree was considered
/// and rejected: a few real mastering tools get the BE half wrong, and the
/// strictness would buy nothing that the sanity checks above do not already.
#[no_mangle]
pub extern "C" fn usbvol_iso_extent_rs(sec: *const u8, len: u32) -> i64 {
    if sec.is_null() || (len as u64) < ISO_SECT {
        return USBVOL_E_ARG;
    }
    // SAFETY: the caller guarantees `len` readable bytes at `sec`, and `len` is
    // at least ISO_SECT, so the slice below spans only bytes the caller owns and
    // every fixed index used (max 131) is inside it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(sec, ISO_SECT as usize) };

    if b[0] != 1 {
        return USBVOL_E_MAGIC; // not a PRIMARY volume descriptor
    }
    if !(b[1] == b'C' && b[2] == b'D' && b[3] == b'0' && b[4] == b'0' && b[5] == b'1') {
        return USBVOL_E_MAGIC;
    }
    if b[6] != 1 {
        return USBVOL_E_MAGIC; // descriptor version
    }

    // volume_space_size: LE at 80, BE at 84. logical_block_size: LE at 128, BE at 130.
    let blocks_le = rd_u32_le(b, 80);
    let blocks_be = rd_u32_be(b, 84);
    let bs_le = rd_u16_le(b, 128);
    let bs_be = rd_u16_be(b, 130);

    let blocks: u64 = if blocks_le != 0 {
        blocks_le as u64
    } else {
        blocks_be as u64
    };
    let bs: u64 = if bs_le != 0 { bs_le as u64 } else { bs_be as u64 };

    if bs < 512 || bs > 4096 || (bs & (bs - 1)) != 0 {
        return USBVOL_E_GEOM;
    }
    if blocks < ISO_PVD_SECT + 1 {
        return USBVOL_E_GEOM;
    }

    match blocks.checked_mul(bs) {
        // i64 guard: the value is handed back as i64 and must stay positive, or
        // a caller comparing "< 0" for failure would read a length as an error.
        Some(n) if n <= (i64::MAX as u64) => n as i64,
        _ => USBVOL_E_RANGE,
    }
}

/// 1 if [off, off+len) is a legal data-volume span on a device of `dev_bytes`,
/// 0 otherwise.
///
/// `dev_bytes == 0` means THE DEVICE SIZE IS NOT KNOWN. That is a real case,
/// not a convenience: fs/blockdev.c can report a capacity for a USB MSC root
/// (usb_msc_device_t.num_blocks) but drivers/ata.h exposes no capacity accessor
/// at all, so an ATA-disk VM genuinely cannot answer. In that case every check
/// except the device-fit one still applies and the read itself is the final
/// arbiter: a blk_read past the end of the medium fails, which is a refusal,
/// not a wrong answer. Silently assuming an unknown device is big enough would
/// be the bug.
#[no_mangle]
pub extern "C" fn usbvol_range_ok_rs(off: u64, len: u64, dev_bytes: u64) -> i32 {
    if len == 0 {
        return 0;
    }
    if off < USBVOL_BASE {
        return 0; // inside the OS image / the TO-RAM window
    }
    if off % USBVOL_ALIGN != 0 {
        // Also guarantees 512-alignment, which imgfile's block-device backing
        // needs so that a whole cache block maps to whole sectors.
        return 0;
    }
    let end = match off.checked_add(len) {
        Some(e) => e,
        None => return 0,
    };
    if dev_bytes != 0 && end > dev_bytes {
        return 0;
    }
    1
}

/// The offset the chain walk should probe next, or a negative USBVOL_E_* code
/// when the chain ends here.
///
/// The chain has no table of contents: each volume states its own length, so
/// the next candidate is simply the aligned offset just past this one. The
/// guard that matters is PROGRESS. A volume whose declared length rounds to
/// zero advance would make the walk sit on the same offset forever, and that
/// length came off the disc. Refusing to go backwards or stand still is what
/// turns a hostile length field into a terminated scan instead of a hang, which
/// is the #426 rule applied to a scan rather than to a wait.
#[no_mangle]
pub extern "C" fn usbvol_next_off_rs(cur: u64, vol_bytes: u64, dev_bytes: u64) -> i64 {
    if vol_bytes == 0 {
        return USBVOL_E_END;
    }
    let raw = match cur.checked_add(vol_bytes) {
        Some(v) => v,
        None => return USBVOL_E_END,
    };
    let next = match align_up(raw) {
        Some(v) => v,
        None => return USBVOL_E_END,
    };
    if next <= cur {
        return USBVOL_E_END; // no progress: refuse rather than spin
    }
    if usbvol_range_ok_rs(next, USBVOL_PROBE_SPAN, dev_bytes) != 1 {
        return USBVOL_E_END; // no room left for even a descriptor
    }
    if next > (i64::MAX as u64) {
        return USBVOL_E_END;
    }
    next as i64
}

/// Hand the constants to C. This is why there is no second copy of them.
#[no_mangle]
pub extern "C" fn usbvol_consts_rs(
    base: *mut u64,
    align: *mut u64,
    probe_span: *mut u64,
    max: *mut u32,
) {
    // SAFETY: the caller passes four writable locals (or nulls); each is
    // written at most once and nothing is read.
    unsafe {
        if !base.is_null() {
            *base = USBVOL_BASE;
        }
        if !align.is_null() {
            *align = USBVOL_ALIGN;
        }
        if !probe_span.is_null() {
            *probe_span = USBVOL_PROBE_SPAN;
        }
        if !max.is_null() {
            *max = USBVOL_MAX;
        }
    }
}

// ---------------------------------------------------------------------------
// Property self-test. Proves the rules above hold ON THIS BUILD, which is the
// only claim a boot-time test can honestly make. Returns the FAILURE count and
// writes the number of checks executed to *out_checks.
// ---------------------------------------------------------------------------

/// Build a minimal but valid primary volume descriptor in `b`.
fn mk_pvd(b: &mut [u8; 2048], blocks: u32, bs: u16) {
    for x in b.iter_mut() {
        *x = 0;
    }
    b[0] = 1;
    b[1] = b'C';
    b[2] = b'D';
    b[3] = b'0';
    b[4] = b'0';
    b[5] = b'1';
    b[6] = 1;
    b[80] = (blocks & 0xFF) as u8;
    b[81] = ((blocks >> 8) & 0xFF) as u8;
    b[82] = ((blocks >> 16) & 0xFF) as u8;
    b[83] = ((blocks >> 24) & 0xFF) as u8;
    b[84] = ((blocks >> 24) & 0xFF) as u8;
    b[85] = ((blocks >> 16) & 0xFF) as u8;
    b[86] = ((blocks >> 8) & 0xFF) as u8;
    b[87] = (blocks & 0xFF) as u8;
    b[128] = (bs & 0xFF) as u8;
    b[129] = ((bs >> 8) & 0xFF) as u8;
    b[130] = ((bs >> 8) & 0xFF) as u8;
    b[131] = (bs & 0xFF) as u8;
}

#[no_mangle]
pub extern "C" fn usbvol_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut fails: u32 = 0;
    macro_rules! want {
        ($c:expr) => {{
            checks += 1;
            if !($c) {
                fails += 1;
            }
        }};
    }

    let mut pvd = [0u8; 2048];

    // ---- extent parsing -----------------------------------------------------
    mk_pvd(&mut pvd, 1000, 2048);
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == 1000 * 2048);

    // A short buffer is refused, not read.
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 100) == USBVOL_E_ARG);
    want!(usbvol_iso_extent_rs(core::ptr::null(), 2048) == USBVOL_E_ARG);

    // Wrong magic / wrong descriptor type / wrong version.
    mk_pvd(&mut pvd, 1000, 2048);
    pvd[3] = b'X';
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_MAGIC);
    mk_pvd(&mut pvd, 1000, 2048);
    pvd[0] = 2; // a supplementary (Joliet) descriptor is NOT the volume length
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_MAGIC);
    mk_pvd(&mut pvd, 1000, 2048);
    pvd[6] = 2;
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_MAGIC);

    // All-zero bytes (an unwritten region of the stick) must NOT look like a
    // volume. This is the case that ends every chain walk in practice.
    for x in pvd.iter_mut() {
        *x = 0;
    }
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) < 0);

    // Implausible geometry.
    mk_pvd(&mut pvd, 1000, 777); // not a power of two
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_GEOM);
    mk_pvd(&mut pvd, 1000, 8192); // out of range
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_GEOM);
    mk_pvd(&mut pvd, 3, 2048); // cannot even contain its own descriptor
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_GEOM);
    mk_pvd(&mut pvd, 0, 2048);
    want!(usbvol_iso_extent_rs(pvd.as_ptr(), 2048) == USBVOL_E_GEOM);

    // A hostile maximal block count must not overflow into a small positive.
    mk_pvd(&mut pvd, 0xFFFF_FFFF, 4096);
    let big = usbvol_iso_extent_rs(pvd.as_ptr(), 2048);
    want!(big > 0 && (big as u64) == 0xFFFF_FFFFu64 * 4096);

    // ---- range checking -----------------------------------------------------
    let dev: u64 = 8u64 << 30; // an 8 GiB device
    want!(usbvol_range_ok_rs(USBVOL_BASE, 1 << 20, dev) == 1);
    want!(usbvol_range_ok_rs(USBVOL_BASE - USBVOL_ALIGN, 1 << 20, dev) == 0); // below base
    want!(usbvol_range_ok_rs(USBVOL_BASE + 512, 1 << 20, dev) == 0); // misaligned
    want!(usbvol_range_ok_rs(USBVOL_BASE, 0, dev) == 0); // empty
    want!(usbvol_range_ok_rs(USBVOL_BASE, dev, dev) == 0); // runs off the end
    want!(usbvol_range_ok_rs(USBVOL_BASE, u64::MAX, dev) == 0); // overflow
    // An unknown device size still enforces base + alignment, and still refuses
    // an overflowing span.
    want!(usbvol_range_ok_rs(USBVOL_BASE, 1 << 30, 0) == 1);
    want!(usbvol_range_ok_rs(USBVOL_BASE + 1, 1 << 30, 0) == 0);
    want!(usbvol_range_ok_rs(u64::MAX & !(USBVOL_ALIGN - 1), 1 << 30, 0) == 0);

    // ---- chain walk ---------------------------------------------------------
    // A volume of exactly 1 MiB advances by exactly 1 MiB.
    want!(usbvol_next_off_rs(USBVOL_BASE, 1 << 20, dev) == (USBVOL_BASE + (1 << 20)) as i64);
    // A volume of 1 MiB + 1 byte still advances to the NEXT 1 MiB boundary.
    want!(usbvol_next_off_rs(USBVOL_BASE, (1 << 20) + 1, dev) == (USBVOL_BASE + (2 << 20)) as i64);
    // Zero length must terminate, never stand still.
    want!(usbvol_next_off_rs(USBVOL_BASE, 0, dev) == USBVOL_E_END);
    // A length that reaches the end of the device leaves no room to probe.
    want!(usbvol_next_off_rs(USBVOL_BASE, dev - USBVOL_BASE, dev) == USBVOL_E_END);
    // Overflow terminates.
    want!(usbvol_next_off_rs(USBVOL_BASE, u64::MAX, dev) == USBVOL_E_END);
    want!(usbvol_next_off_rs(u64::MAX - 4096, 8192, 0) == USBVOL_E_END);

    // THE PROPERTY THAT MATTERS: the walk always makes strictly positive
    // progress and stays aligned and in range, for every length it is given.
    // Anything else is a hang, or an out-of-bounds read driven by disc bytes.
    //
    // The device here is deliberately SMALL and the lengths deliberately
    // BOUNDED, so the walk is guaranteed to run out of room long before the
    // step cap. The first version of this test used the 8 GiB device with
    // lengths up to 64 MiB, which needs more than 64 steps to exhaust: the walk
    // hit the cap, the final assertion failed, and the boot line read "220
    // checks, 1 failure" for a fault that was entirely in the test. A
    // termination test whose bound the correct behaviour cannot reach is not a
    // termination test. Here the minimum advance is 1 MiB (USBVOL_ALIGN) and
    // there are only 32 MiB of tail, so at most 32 steps are possible against a
    // cap of 512.
    let dev_small: u64 = USBVOL_BASE + 32 * (1 << 20);
    let mut off = USBVOL_BASE;
    let mut steps = 0u32;
    let mut len: u64 = 1;
    while steps < 512 {
        let n = usbvol_next_off_rs(off, len, dev_small);
        if n < 0 {
            break;
        }
        let n = n as u64;
        want!(n > off);
        want!(n % USBVOL_ALIGN == 0);
        want!(usbvol_range_ok_rs(n, USBVOL_PROBE_SPAN, dev_small) == 1);
        off = n;
        len = len.wrapping_mul(3).wrapping_add(4096) & 0x3F_FFFF; // <= 4 MiB
        steps += 1;
    }
    // It must have terminated by running out of device, not by hitting the cap.
    want!(steps < 512);

    if !out_checks.is_null() {
        // SAFETY: non-null, and the caller's contract is a writable u32.
        unsafe {
            *out_checks = checks;
        }
    }
    fails as i32
}
