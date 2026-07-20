// rustkern/parttbl.rs - #404 driver/block tier: MBR / GPT partition-table parse
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 driver/block tier: MBR / GPT partition-table parse.
// Rust drop-ins for parttbl_gpt_hdr_c / parttbl_gpt_sec_scan_c /
// parttbl_mbr_find_c (fs/ext2.c), the pure byte-walking of
// ext2_find_partition() (#365 root discovery, live from main.c).
//
// UNTRUSTED input: LBA 0/1 and the GPT entry array come off whatever disk or
// USB stick is inserted, and the iMac target BOOTS off USB.
//
// PASTE INTO rustkern.rs AS-IS. Do NOT copy the standalone file's `#![no_std]`,
// `use core::panic::PanicInfo` or `#[panic_handler]`: rustkern.rs already has
// them and duplicates will not compile.
// ===========================================================================

// Mirrors parttbl_gpt_hdr_t in parttbl.h. sizeof-locked by the harness.
#[repr(C)]
pub struct GptHdr {
    pub ent_lba: u64,
    pub num: u32,
    pub esz: u32,
    pub per_sec: u32,
    pub _pad: u32,
}

// Mirrors parttbl_scan_t in parttbl.h. sizeof-locked by the harness.
#[repr(C)]
pub struct Scan {
    pub consumed: u32,
    pub found_lin: u32,
    pub lin_lba: u64,
}

const GUID_ESP: [u8; 16] = [
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
];
const GUID_LINUX: [u8; 16] = [
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4,
];

/// GPT header parse + validation. Returns 0 and fills `*out` on accept, -1 on
/// reject. Mirrors parttbl_gpt_hdr_c exactly.
#[no_mangle]
pub extern "C" fn parttbl_gpt_hdr_rs(sec: *const u8, len: u32, out: *mut GptHdr) -> i32 {
    if sec.is_null() || out.is_null() {
        return -1;
    }
    let n = len as usize;

    // SAFETY: the caller guarantees `sec` points to at least `len` contiguous
    // readable bytes (the live contract: a freshly kmalloc(512) buffer filled
    // by blk_read, len = 512). We build a slice spanning exactly that extent
    // and index ONLY through it, so every disk-controlled read below is
    // bounds-checked by Rust. `out` is a caller-owned, properly aligned
    // GptHdr (a stack local in fs/ext2.c) written only on the accept path.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(sec, n) };

    // The C reads sec[0..7], sec[72..79], sec[80..87] with no length check,
    // relying on the 512-byte buffer. Rust rejects a short sector instead of
    // reading past it. At the live len = 512 this is never taken, so the
    // accept/reject decision is identical to the C on every live input.
    if n < 88 {
        return -1;
    }

    if !(s[0] == b'E'
        && s[1] == b'F'
        && s[2] == b'I'
        && s[3] == b' '
        && s[4] == b'P'
        && s[5] == b'A'
        && s[6] == b'R'
        && s[7] == b'T')
    {
        return -1;
    }

    let mut ent_lba: u64 = 0;
    let mut i = 0usize;
    while i < 8 {
        ent_lba |= (s[72 + i] as u64) << (8 * i);
        i += 1;
    }
    let num = u32::from_le_bytes([s[80], s[81], s[82], s[83]]);
    let esz = u32::from_le_bytes([s[84], s[85], s[86], s[87]]);

    // Verbatim acceptance guard: esz >= 128 && esz <= len && num > 0 && num <= 256.
    // esz >= 128 is what bounds the 40-byte entry read below; see parttbl_gpt_sec_scan_rs.
    if !(esz >= 128 && esz <= len && num > 0 && num <= 256) {
        return -1;
    }

    // len/esz cannot divide by zero (esz >= 128) and cannot overflow.
    let mut per_sec = len / esz;
    if per_sec == 0 {
        per_sec = 1;
    }

    // SAFETY: `out` is non-null and caller-owned (checked above).
    unsafe {
        (*out).ent_lba = ent_lba;
        (*out).num = num;
        (*out).esz = esz;
        (*out).per_sec = per_sec;
        (*out)._pad = 0;
    }
    0
}

/// Scan ONE sector of the GPT partition-entry array. Mirrors
/// parttbl_gpt_sec_scan_c exactly on every in-contract input.
///
/// Where the C twin performs an UNCHECKED `sec + e * esz` read of 40 bytes and
/// relies purely on the caller's header guard (esz >= 128 && esz <= len) to
/// keep it inside the buffer, this port bounds every access by construction:
/// the index multiply is checked and the 40-byte extent must fit in `len`,
/// otherwise the entry is skipped. On header-validated input the two agree.
#[no_mangle]
pub extern "C" fn parttbl_gpt_sec_scan_rs(
    sec: *const u8,
    len: u32,
    esz: u32,
    per_sec: u32,
    remaining: u32,
    io_fallback: *mut u32,
    out: *mut Scan,
) -> i32 {
    if sec.is_null() || out.is_null() || io_fallback.is_null() {
        return -1;
    }
    let n = len as usize;

    // SAFETY: as above, `sec` spans exactly `len` readable bytes (a blk_read
    // sector buffer); `out` is a caller-owned, aligned Scan and `io_fallback`
    // a caller-owned, aligned u32 (both stack locals in fs/ext2.c).
    let s: &[u8] = unsafe { core::slice::from_raw_parts(sec, n) };
    let o: &mut Scan = unsafe { &mut *out };
    let fb: &mut u32 = unsafe { &mut *io_fallback };

    o.consumed = 0;
    o.found_lin = 0;
    o.lin_lba = 0;

    let mut e: u32 = 0;
    while e < per_sec && o.consumed < remaining {
        // CHECKED multiply: the classic `entries * entry_size` index. The C
        // computes `e * esz` in u32 and dereferences unconditionally.
        let off = match e.checked_mul(esz) {
            Some(v) => v as usize,
            None => break,
        };
        // The entry read spans ent[0..16] (GUID) and ent[32..40] (FirstLBA),
        // so 40 bytes must fit. checked_add prevents the bound itself wrapping.
        let end = match off.checked_add(40) {
            Some(v) => v,
            None => break,
        };
        if end > n {
            break; // confine: never read past the sector
        }

        let ent = &s[off..end];
        let mut empty = true;
        let mut is_esp = true;
        let mut is_lin = true;
        let mut i = 0usize;
        while i < 16 {
            let b = ent[i];
            if b != 0 {
                empty = false;
            }
            if b != GUID_ESP[i] {
                is_esp = false;
            }
            if b != GUID_LINUX[i] {
                is_lin = false;
            }
            i += 1;
        }
        if empty || is_esp {
            e += 1;
            o.consumed += 1;
            continue; // skip empty + the FAT ESP
        }

        let mut first: u64 = 0;
        let mut k = 0usize;
        while k < 8 {
            first |= (ent[32 + k] as u64) << (8 * k);
            k += 1;
        }
        if is_lin {
            // Live code returns immediately; consumed is not bumped for this
            // entry (mirrors the C loop's increment never running).
            o.found_lin = 1;
            o.lin_lba = first;
            return 0;
        }
        // Mirrors the C exactly: test the u64 `first`, store the TRUNCATED u32,
        // re-test the stored value on the next entry.
        if *fb == 0 && first != 0 {
            *fb = first as u32;
        }
        e += 1;
        o.consumed += 1;
    }
    0
}

/// MBR fallback: primary table at LBA 0, first type-0x83 (Linux) entry.
/// Mirrors parttbl_mbr_find_c exactly. The C reads sec[510]/sec[511] and
/// sec[446..510] unchecked; this port rejects a short sector instead.
#[no_mangle]
pub extern "C" fn parttbl_mbr_find_rs(sec: *const u8, len: u32, out_start: *mut u32) -> i32 {
    if sec.is_null() || out_start.is_null() {
        return -1;
    }
    let n = len as usize;

    // SAFETY: `sec` spans exactly `len` readable bytes; `out_start` is a
    // caller-owned aligned u32 written only on the accept path.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(sec, n) };

    // A whole MBR must be present. At the live len = 512 this is never taken.
    if n < 512 {
        return -1;
    }
    if !(s[510] == 0x55 && s[511] == 0xAA) {
        return -1;
    }
    let mut i = 0usize;
    while i < 4 {
        let p = 446 + i * 16; // max 494; p+12 = 506 <= 512, bounds-checked anyway
        let start = u32::from_le_bytes([s[p + 8], s[p + 9], s[p + 10], s[p + 11]]);
        if s[p + 4] == 0x83 && start != 0 {
            // SAFETY: `out_start` is non-null and caller-owned (checked above).
            unsafe {
                *out_start = start;
            }
            return 0;
        }
        i += 1;
    }
    -1
}
