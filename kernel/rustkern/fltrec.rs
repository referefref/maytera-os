// rustkern/fltrec.rs - RAW-BLOCK BOOT FLIGHT RECORDER: record layout, ring
// bookkeeping, CRC32 and the append/dirty-sector arithmetic. NO I/O.
//
// WHY THIS EXISTS
// ---------------
// The ASUS i7 laptop target has NO SERIAL PORT. Every persistent log this
// kernel writes (/BOOTLOG.TXT, /USBLOG.TXT, /DEVLOG.TXT, /boot/STAGE.TXT)
// reaches the medium only through a MOUNTED filesystem, so a failure before or
// during the mount leaves nothing anywhere. This recorder puts breadcrumbs at a
// raw LBA inside the GPT alignment gap (34..2046), which no filesystem owns, so
// it survives a filesystem that is itself broken and needs no mount at all.
//
// WHY THIS HALF IS RUST AND THE I/O HALF IS C
// -------------------------------------------
// Everything here is byte-offset arithmetic over disk-controlled bytes and
// sector-index arithmetic across a boundary. That is exactly where the
// off-by-one and the out-of-bounds read live, and it is the half that must not
// be able to fault: the whole point of a flight recorder is that it still works
// on a machine that is already going wrong. A Rust panic in this crate routes
// to kpanic and halts, so NOTHING below may panic: every index goes through a
// bounds-checked slice, every arithmetic step that could wrap is checked, and
// there is no unwrap, no unchecked indexing and no unbounded loop.
//
// The C side (fs/fltrec.c) owns the block layer, the .bss text buffer and the
// arm/flush/seal sequencing, and hands this module plain buffers.
//
// ON-DISK LAYOUT (fixed here as code; the C header repeats the constants and
// the host reader tools/fltrec/fltrec-read.sh parses exactly this):
//
//   LBA 34             superblock          (512 bytes)
//   LBA 35             slot 0 header       (512 bytes)
//   LBA 36..537        slot 0 text         (502 sectors = 257,024 bytes)
//   LBA 538            slot 1 header
//   LBA 539..1040      slot 1 text
//   LBA 1041           slot 2 header
//   LBA 1042..1543     slot 2 text
//   LBA 1544           slot 3 header
//   LBA 1545..2046     slot 3 text
//   LBA 2047           reserved, left zero
//
// 35 + 4*503 = 2047, so the four slots end exactly one sector short of the
// first partition's 2048 alignment boundary. Four slots means the last four
// boot attempts survive: the user can try three more times and still bring back
// the first failure.
//
// All multi-byte fields are LITTLE-ENDIAN. The CRC32 is the IEEE/zlib one
// (reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF),
// which is the same CRC PNG and GPT use, so the host reader can check it with
// any standard tool.

// ---------------------------------------------------------------------------
// GEOMETRY. Duplicated as #defines in fs/fltrec.h and re-derived in the host
// reader; fltrec_selftest_rs() asserts the relationships between them so a
// hand-edit that breaks the arithmetic goes red at boot rather than silently
// writing over the first partition.
// ---------------------------------------------------------------------------
pub(crate) const FLT_SECTOR: u32 = 512;
pub(crate) const FLT_SLOTS: u32 = 4;
pub(crate) const FLT_SB_LBA: u64 = 34;
pub(crate) const FLT_SLOT_BASE_LBA: u64 = 35;
pub(crate) const FLT_SLOT_SECTORS: u32 = 503;
pub(crate) const FLT_TEXT_SECTORS: u32 = FLT_SLOT_SECTORS - 1; // 502
pub(crate) const FLT_TEXT_CAP: u32 = FLT_TEXT_SECTORS * FLT_SECTOR; // 257,024
pub(crate) const FLT_REGION_LO: u64 = 34;
pub(crate) const FLT_REGION_HI: u64 = 2047;

const FLT_VERSION: u32 = 1;
const SB_MAGIC: [u8; 8] = *b"MAYTFLT1";
const HDR_MAGIC: [u8; 8] = *b"MAYTFSL1";
// Both records are 512 bytes with the CRC32 in the last 4, computed over the
// preceding 508. Keeping the CRC last means a future field can be added inside
// the reserved area without moving it.
const REC_LEN: usize = 512;
const CRC_OFF: usize = 508;

/// Completion verdicts, written into the slot header.
pub(crate) const FLT_VERDICT_OPEN: u32 = 0; // armed, never sealed: THIS BOOT DID NOT FINISH
pub(crate) const FLT_VERDICT_OK: u32 = 1; // sealed, caller said the boot succeeded
pub(crate) const FLT_VERDICT_FAIL: u32 = 2; // sealed, caller said the boot failed

// ---------------------------------------------------------------------------
// CRC32 (IEEE / zlib polynomial).
//
// REUSE CHECK, per the project rule, done before writing this: there are three
// CRC32 implementations in kernel/ and NOT ONE of them is callable.
//   * kernel/media/png.c:69 `png_crc32` is global, but media/png.c is NOT in
//     the Makefile's ALL_C_SOURCES, so that translation unit is never compiled
//     and the symbol is not in the image. It is dead source.
//   * kernel/gui/png.c:71 `crc32_png` is `static`.
//   * kernel/gui/installer.c:869 `inst_crc32` is `static`.
// So this is the first linkable CRC32 in the kernel and it is exported for
// anyone else who needs one, rather than being a fourth private copy.
//
// Table built at COMPILE time: no lazily-initialised `crc_table_computed` flag
// (the shape all three C copies use), which cannot be raced and costs no first
// -call branch on a path that runs before the scheduler exists.
const fn crc32_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut n = 0usize;
    while n < 256 {
        let mut c = n as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[n] = c;
        n += 1;
    }
    t
}
static CRC32_TAB: [u32; 256] = crc32_table();

/// CRC32 over a slice. Pure, panic-free: indexes CRC32_TAB with a byte, which
/// is 0..=255 and therefore always in range for a [u32; 256].
pub(crate) fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        let idx = ((crc ^ (b as u32)) & 0xFF) as usize;
        crc = CRC32_TAB[idx] ^ (crc >> 8);
    }
    crc ^ 0xFFFF_FFFF
}

/// C entry point for the CRC32 above.
///
/// # Safety
/// `data` must point to at least `len` readable bytes, or be null (in which
/// case 0 is returned).
#[no_mangle]
pub extern "C" fn fltrec_crc32_rs(data: *const u8, len: u32) -> u32 {
    if data.is_null() || len == 0 {
        // The CRC of the empty message is 0 with this init/xor pair, and a null
        // pointer has no message at all, so both answer 0. Neither is an error
        // worth a separate channel here.
        return 0;
    }
    // SAFETY: the caller guarantees `data` spans `len` readable bytes. We build
    // a slice of exactly that extent and index ONLY through it, so every read
    // below is bounds-checked by Rust.
    let s = unsafe { core::slice::from_raw_parts(data, len as usize) };
    crc32(s)
}

// ---------------------------------------------------------------------------
// Bounds-checked little-endian helpers over a fixed-size record.
//
// The crate already has elf_rd_u32/elf_rd_u64 in rustkern/common.rs and they
// are used here rather than a private fork, per the shared-primitive rule.
// Only the WRITE side is new (common.rs has readers only).
// ---------------------------------------------------------------------------
use crate::common::{elf_rd_u32, elf_rd_u64};

#[inline]
fn wr_u32(s: &mut [u8], off: usize, v: u32) -> bool {
    match off.checked_add(4) {
        Some(end) if end <= s.len() => {
            s[off] = v as u8;
            s[off + 1] = (v >> 8) as u8;
            s[off + 2] = (v >> 16) as u8;
            s[off + 3] = (v >> 24) as u8;
            true
        }
        _ => false,
    }
}

#[inline]
fn wr_u64(s: &mut [u8], off: usize, v: u64) -> bool {
    match off.checked_add(8) {
        Some(end) if end <= s.len() => {
            let mut i = 0usize;
            while i < 8 {
                s[off + i] = (v >> (8 * i)) as u8;
                i += 1;
            }
            true
        }
        _ => false,
    }
}

// ---------------------------------------------------------------------------
// SUPERBLOCK (LBA 34)
//
//    0  8  magic "MAYTFLT1"
//    8  4  format version
//   12  4  slot count
//   16  8  slot base LBA
//   24  4  sectors per slot (header + text)
//   28  4  head slot index (the slot THIS boot is writing)
//   32  8  boot sequence number, monotonically increasing from 1
//   40  4  superblock LBA (self-describing, so a reader that found this record
//          by scanning can confirm it is where it thinks it is)
//   44  4  text sectors per slot
//   48 460 reserved, zero
//  508  4  CRC32 over bytes [0, 508)
// ---------------------------------------------------------------------------

/// Decoded superblock. `#[repr(C)]`; sizeof-locked from fs/fltrec.h.
#[repr(C)]
pub struct FltSb {
    pub slot_base_lba: u64, //  0
    pub boot_seq: u64,      //  8
    pub version: u32,       // 16
    pub slot_count: u32,    // 20
    pub slot_sectors: u32,  // 24
    pub text_sectors: u32,  // 28
    pub head_slot: u32,     // 32
    pub sb_lba: u32,        // 36
}

/// Encode a superblock into a 512-byte buffer. The buffer is fully overwritten
/// (including the reserved area), so a caller may hand us a sector it just read
/// off the disk without zeroing it first and no stale byte survives.
///
/// Returns 0 on success, -1 on bad arguments.
///
/// # Safety
/// `buf` must point to at least `buf_len` writable bytes.
#[no_mangle]
pub extern "C" fn fltrec_sb_encode_rs(buf: *mut u8, buf_len: u32, head_slot: u32, boot_seq: u64) -> i32 {
    if buf.is_null() || (buf_len as usize) < REC_LEN || head_slot >= FLT_SLOTS {
        return -1;
    }
    // SAFETY: null-checked and length-checked above; the caller owns `buf` and
    // guarantees `buf_len` writable bytes. We narrow to exactly REC_LEN and
    // write only through that slice.
    let s = unsafe { core::slice::from_raw_parts_mut(buf, REC_LEN) };
    sb_encode_in(s, head_slot, boot_seq)
}

fn sb_encode_in(s: &mut [u8], head_slot: u32, boot_seq: u64) -> i32 {
    if s.len() < REC_LEN {
        return -1;
    }
    for b in s.iter_mut() {
        *b = 0;
    }
    s[0..8].copy_from_slice(&SB_MAGIC);
    let ok = wr_u32(s, 8, FLT_VERSION)
        && wr_u32(s, 12, FLT_SLOTS)
        && wr_u64(s, 16, FLT_SLOT_BASE_LBA)
        && wr_u32(s, 24, FLT_SLOT_SECTORS)
        && wr_u32(s, 28, head_slot)
        && wr_u64(s, 32, boot_seq)
        && wr_u32(s, 40, FLT_SB_LBA as u32)
        && wr_u32(s, 44, FLT_TEXT_SECTORS);
    if !ok {
        return -1;
    }
    let c = crc32(&s[0..CRC_OFF]);
    if !wr_u32(s, CRC_OFF, c) {
        return -1;
    }
    0
}

/// Decode and VALIDATE a superblock.
///
/// REJECTION IS THE POINT. This region may hold someone else's bytes on a stick
/// that was not written from our image, and it may hold OUR bytes half-written
/// by a machine that lost power mid-flush. Trusting either would make us write
/// a slot header at an LBA derived from a foreign u32. Every field that is used
/// to compute an LBA is therefore checked against the compiled-in geometry, not
/// merely read.
///
/// Returns 0 on accept (and fills `*out`), -1 on reject.
///
/// # Safety
/// `buf` must point to at least `buf_len` readable bytes; `out` must be a
/// caller-owned, aligned FltSb.
#[no_mangle]
pub extern "C" fn fltrec_sb_decode_rs(buf: *const u8, buf_len: u32, out: *mut FltSb) -> i32 {
    if buf.is_null() || out.is_null() || (buf_len as usize) < REC_LEN {
        return -1;
    }
    // SAFETY: both pointers null-checked, length checked. `buf` spans at least
    // REC_LEN readable bytes and we narrow to exactly that; `out` is a
    // caller-owned aligned FltSb (a stack local in fs/fltrec.c), written only
    // on the accept path.
    let s = unsafe { core::slice::from_raw_parts(buf, REC_LEN) };
    let mut tmp = FltSb {
        slot_base_lba: 0,
        boot_seq: 0,
        version: 0,
        slot_count: 0,
        slot_sectors: 0,
        text_sectors: 0,
        head_slot: 0,
        sb_lba: 0,
    };
    if sb_decode_in(s, &mut tmp) != 0 {
        return -1;
    }
    unsafe {
        *out = tmp;
    }
    0
}

fn sb_decode_in(s: &[u8], out: &mut FltSb) -> i32 {
    if s.len() < REC_LEN {
        return -1;
    }
    if s[0..8] != SB_MAGIC {
        return -1;
    }
    // CRC before any field is believed.
    let stored = match elf_rd_u32(s, CRC_OFF as u64) {
        Some(v) => v,
        None => return -1,
    };
    if stored != crc32(&s[0..CRC_OFF]) {
        return -1;
    }
    let version = match elf_rd_u32(s, 8) {
        Some(v) => v,
        None => return -1,
    };
    let slot_count = match elf_rd_u32(s, 12) {
        Some(v) => v,
        None => return -1,
    };
    let slot_base_lba = match elf_rd_u64(s, 16) {
        Some(v) => v,
        None => return -1,
    };
    let slot_sectors = match elf_rd_u32(s, 24) {
        Some(v) => v,
        None => return -1,
    };
    let head_slot = match elf_rd_u32(s, 28) {
        Some(v) => v,
        None => return -1,
    };
    let boot_seq = match elf_rd_u64(s, 32) {
        Some(v) => v,
        None => return -1,
    };
    let sb_lba = match elf_rd_u32(s, 40) {
        Some(v) => v,
        None => return -1,
    };
    let text_sectors = match elf_rd_u32(s, 44) {
        Some(v) => v,
        None => return -1,
    };

    // GEOMETRY MUST MATCH THIS BUILD EXACTLY. A superblock that says something
    // else is either a different format version or corruption that happened to
    // keep its CRC consistent (a whole-record overwrite by another writer).
    // Either way we must not derive an LBA from it: re-arming from scratch
    // costs one boot's history, using a foreign geometry costs the partition
    // table.
    if version != FLT_VERSION
        || slot_count != FLT_SLOTS
        || slot_base_lba != FLT_SLOT_BASE_LBA
        || slot_sectors != FLT_SLOT_SECTORS
        || text_sectors != FLT_TEXT_SECTORS
        || sb_lba as u64 != FLT_SB_LBA
        || head_slot >= FLT_SLOTS
    {
        return -1;
    }

    out.slot_base_lba = slot_base_lba;
    out.boot_seq = boot_seq;
    out.version = version;
    out.slot_count = slot_count;
    out.slot_sectors = slot_sectors;
    out.text_sectors = text_sectors;
    out.head_slot = head_slot;
    out.sb_lba = sb_lba;
    0
}

// ---------------------------------------------------------------------------
// SLOT HEADER (first sector of each slot)
//
//    0  8  magic "MAYTFSL1"
//    8  4  format version
//   12  4  slot index
//   16  8  boot sequence number (matches the superblock's at arm time)
//   24  4  build number (MAYTERA_BUILD_NUMBER)
//   28  4  completion verdict: 0 OPEN, 1 OK, 2 FAIL
//   32  4  text length in bytes
//   36  4  text capacity in bytes
//   40  8  milliseconds since boot at seal time (0 while OPEN)
//   48 32  identity string, NUL-padded (see fs/fltrec.c: the kernel has no
//          commit macro, so this carries version + build + build date; a
//          setter exists for when a real commit string appears)
//   80  4  CRC32 of the FIRST text_len bytes of the slot's text area. Lets a
//          reader distinguish "the text on the medium is intact" from "the last
//          sectors were lost", which is the difference between a clean
//          power-off and a machine that died mid-write.
//   84 424 reserved, zero
//  508  4  CRC32 over bytes [0, 508)
// ---------------------------------------------------------------------------

/// Decoded slot header. `#[repr(C)]`; sizeof-locked from fs/fltrec.h. The
/// explicit `_pad` keeps `ident` at offset 48 on both sides rather than relying
/// on two compilers inserting the same implicit padding.
#[repr(C)]
pub struct FltHdr {
    pub boot_seq: u64,   //  0
    pub seal_ms: u64,    //  8
    pub version: u32,    // 16
    pub slot_index: u32, // 20
    pub build: u32,      // 24
    pub verdict: u32,    // 28
    pub text_len: u32,   // 32
    pub text_cap: u32,   // 36
    pub text_crc: u32,   // 40
    pub _pad: u32,       // 44
    pub ident: [u8; 32], // 48
}

/// Encode a slot header into a 512-byte buffer. Fully overwrites the buffer.
/// Returns 0 on success, -1 on bad arguments (including an out-of-range slot
/// index, verdict or text length, none of which may reach the disk).
///
/// # Safety
/// `buf` must point to at least `buf_len` writable bytes; `h` must be a
/// caller-owned, aligned, initialised FltHdr.
#[no_mangle]
pub extern "C" fn fltrec_hdr_encode_rs(buf: *mut u8, buf_len: u32, h: *const FltHdr) -> i32 {
    if buf.is_null() || h.is_null() || (buf_len as usize) < REC_LEN {
        return -1;
    }
    // SAFETY: both pointers null-checked and the length checked. `h` is a
    // caller-owned aligned FltHdr; we only read from it.
    let s = unsafe { core::slice::from_raw_parts_mut(buf, REC_LEN) };
    let hr = unsafe { &*h };
    hdr_encode_in(s, hr)
}

fn hdr_encode_in(s: &mut [u8], h: &FltHdr) -> i32 {
    if s.len() < REC_LEN {
        return -1;
    }
    if h.slot_index >= FLT_SLOTS
        || h.verdict > FLT_VERDICT_FAIL
        || h.text_cap != FLT_TEXT_CAP
        || h.text_len > FLT_TEXT_CAP
    {
        return -1;
    }
    for b in s.iter_mut() {
        *b = 0;
    }
    s[0..8].copy_from_slice(&HDR_MAGIC);
    let ok = wr_u32(s, 8, FLT_VERSION)
        && wr_u32(s, 12, h.slot_index)
        && wr_u64(s, 16, h.boot_seq)
        && wr_u32(s, 24, h.build)
        && wr_u32(s, 28, h.verdict)
        && wr_u32(s, 32, h.text_len)
        && wr_u32(s, 36, h.text_cap)
        && wr_u64(s, 40, h.seal_ms)
        && wr_u32(s, 80, h.text_crc);
    if !ok {
        return -1;
    }
    // 32 bytes at offset 48; both extents are compile-time constants inside
    // REC_LEN, but slice the destination rather than indexing so a future
    // layout edit cannot silently write past the record.
    let dst = match s.get_mut(48..80) {
        Some(d) => d,
        None => return -1,
    };
    dst.copy_from_slice(&h.ident);
    // An identity string is printed by the host reader; a stray NUL is fine
    // (it terminates) but a control byte would corrupt the output, so scrub.
    for b in dst.iter_mut() {
        if *b != 0 && (*b < 0x20 || *b > 0x7E) {
            *b = b'?';
        }
    }
    let c = crc32(&s[0..CRC_OFF]);
    if !wr_u32(s, CRC_OFF, c) {
        return -1;
    }
    0
}

/// Decode and validate a slot header. Returns 0 on accept, -1 on reject.
///
/// # Safety
/// `buf` must point to at least `buf_len` readable bytes; `out` must be a
/// caller-owned, aligned FltHdr.
#[no_mangle]
pub extern "C" fn fltrec_hdr_decode_rs(buf: *const u8, buf_len: u32, out: *mut FltHdr) -> i32 {
    if buf.is_null() || out.is_null() || (buf_len as usize) < REC_LEN {
        return -1;
    }
    // SAFETY: as fltrec_sb_decode_rs above.
    let s = unsafe { core::slice::from_raw_parts(buf, REC_LEN) };
    let mut tmp = FltHdr {
        boot_seq: 0,
        seal_ms: 0,
        version: 0,
        slot_index: 0,
        build: 0,
        verdict: 0,
        text_len: 0,
        text_cap: 0,
        text_crc: 0,
        _pad: 0,
        ident: [0u8; 32],
    };
    if hdr_decode_in(s, &mut tmp) != 0 {
        return -1;
    }
    unsafe {
        *out = tmp;
    }
    0
}

fn hdr_decode_in(s: &[u8], out: &mut FltHdr) -> i32 {
    if s.len() < REC_LEN {
        return -1;
    }
    if s[0..8] != HDR_MAGIC {
        return -1;
    }
    let stored = match elf_rd_u32(s, CRC_OFF as u64) {
        Some(v) => v,
        None => return -1,
    };
    if stored != crc32(&s[0..CRC_OFF]) {
        return -1;
    }
    let version = match elf_rd_u32(s, 8) {
        Some(v) => v,
        None => return -1,
    };
    let slot_index = match elf_rd_u32(s, 12) {
        Some(v) => v,
        None => return -1,
    };
    let boot_seq = match elf_rd_u64(s, 16) {
        Some(v) => v,
        None => return -1,
    };
    let build = match elf_rd_u32(s, 24) {
        Some(v) => v,
        None => return -1,
    };
    let verdict = match elf_rd_u32(s, 28) {
        Some(v) => v,
        None => return -1,
    };
    let text_len = match elf_rd_u32(s, 32) {
        Some(v) => v,
        None => return -1,
    };
    let text_cap = match elf_rd_u32(s, 36) {
        Some(v) => v,
        None => return -1,
    };
    let seal_ms = match elf_rd_u64(s, 40) {
        Some(v) => v,
        None => return -1,
    };
    let text_crc = match elf_rd_u32(s, 80) {
        Some(v) => v,
        None => return -1,
    };
    if version != FLT_VERSION
        || slot_index >= FLT_SLOTS
        || verdict > FLT_VERDICT_FAIL
        || text_cap != FLT_TEXT_CAP
        || text_len > FLT_TEXT_CAP
    {
        return -1;
    }
    let src = match s.get(48..80) {
        Some(d) => d,
        None => return -1,
    };
    out.ident.copy_from_slice(src);
    out.boot_seq = boot_seq;
    out.seal_ms = seal_ms;
    out.version = version;
    out.slot_index = slot_index;
    out.build = build;
    out.verdict = verdict;
    out.text_len = text_len;
    out.text_cap = text_cap;
    out.text_crc = text_crc;
    out._pad = 0;
    0
}

// ---------------------------------------------------------------------------
// APPEND
// ---------------------------------------------------------------------------

/// Append one line into the text buffer, adding a trailing newline if the line
/// does not already end in one. Returns the NEW length in bytes.
///
/// NEVER PARTIAL. If the whole line plus its newline does not fit, nothing is
/// copied and `len` comes back unchanged, so the caller can count the drop. A
/// half line written because the buffer filled would be indistinguishable, to
/// the person reading the stick afterwards, from a half line written because
/// the machine died mid-write, and that second case is the one they came for.
///
/// Embedded NULs and control bytes are replaced: the host reader treats a run
/// of NULs as end-of-text (the slot's text area is zeroed at arm time, which is
/// what makes that valid), so a NUL inside a breadcrumb would truncate the log.
///
/// # Safety
/// `buf` must point to at least `cap` writable bytes; `src` to at least
/// `src_len` readable bytes.
#[no_mangle]
pub extern "C" fn fltrec_append_rs(
    buf: *mut u8,
    cap: u32,
    len: u32,
    src: *const u8,
    src_len: u32,
    add_nl: i32,
) -> u32 {
    if buf.is_null() || cap == 0 || len > cap {
        // Nothing sane to return but the caller's own length; a bad call must
        // not be able to move the length forward.
        return len;
    }
    if src.is_null() || src_len == 0 {
        return len;
    }
    // SAFETY: both pointers null-checked and their extents are exactly what the
    // caller declared. We index only through the slices below.
    let b = unsafe { core::slice::from_raw_parts_mut(buf, cap as usize) };
    let s = unsafe { core::slice::from_raw_parts(src, src_len as usize) };
    append_in(b, len, s, add_nl != 0)
}

fn append_in(b: &mut [u8], len: u32, s: &[u8], add_nl: bool) -> u32 {
    let cap = b.len();
    let mut pos = len as usize;
    if pos > cap {
        return len;
    }
    let need_nl = add_nl && !(s.last() == Some(&b'\n'));
    let extra = if need_nl { 1usize } else { 0usize };
    let total = match s.len().checked_add(extra) {
        Some(v) => v,
        None => return len,
    };
    // The only place the buffer's remaining room is computed. `pos <= cap` was
    // established above, so this subtraction cannot wrap.
    if total > cap - pos {
        return len; // does not fit: drop the WHOLE line, see the doc comment
    }
    for &raw in s {
        let c = if raw == b'\n' || raw == b'\r' {
            raw
        } else if raw < 0x20 || raw > 0x7E {
            b'?'
        } else {
            raw
        };
        b[pos] = c;
        pos += 1;
    }
    if need_nl {
        b[pos] = b'\n';
        pos += 1;
    }
    pos as u32
}

// ---------------------------------------------------------------------------
// DIRTY-SECTOR ARITHMETIC
//
// This is the off-by-one, and it is why this half is Rust.
//
// `flushed` bytes are already on the medium, `len` bytes are in RAM. The
// sectors that must be rewritten are those covering byte offsets
// [flushed, len). The subtlety is the PARTIAL tail sector: if `flushed` is not
// a multiple of 512 then the sector holding byte flushed-1 is on disk with only
// part of its bytes valid and MUST be rewritten. `flushed / 512` gives exactly
// that sector when flushed is not a multiple of 512, and gives the next
// (untouched) sector when it is, which is also correct. One expression, both
// cases, and the self-test pins both.
// ---------------------------------------------------------------------------

/// Compute the inclusive dirty text-sector range.
///
/// Returns 1 and fills `*out_first`/`*out_last` when there is something to
/// write, 0 when nothing changed, -1 on impossible arguments (which the C side
/// treats as a bug in itself and reports, rather than writing anything).
///
/// # Safety
/// `out_first` and `out_last` must be caller-owned, aligned u32s.
#[no_mangle]
pub extern "C" fn fltrec_dirty_rs(flushed: u32, len: u32, out_first: *mut u32, out_last: *mut u32) -> i32 {
    if out_first.is_null() || out_last.is_null() {
        return -1;
    }
    let mut f: u32 = 0;
    let mut l: u32 = 0;
    let r = dirty_in(flushed, len, &mut f, &mut l);
    if r == 1 {
        // SAFETY: both pointers null-checked above and owned by the caller
        // (stack locals in fs/fltrec.c).
        unsafe {
            *out_first = f;
            *out_last = l;
        }
    }
    r
}

fn dirty_in(flushed: u32, len: u32, out_first: &mut u32, out_last: &mut u32) -> i32 {
    if len > FLT_TEXT_CAP || flushed > len {
        return -1;
    }
    if len == flushed {
        return 0;
    }
    // len > flushed >= 0, so len >= 1 and (len - 1) cannot wrap.
    let first = flushed / FLT_SECTOR;
    let last = (len - 1) / FLT_SECTOR;
    if last >= FLT_TEXT_SECTORS || first > last {
        // Unreachable given len <= FLT_TEXT_CAP, but a flight recorder that
        // computes an LBA must not rely on "unreachable".
        return -1;
    }
    *out_first = first;
    *out_last = last;
    1
}

// ---------------------------------------------------------------------------
// GPT SAFETY SCAN
//
// We are about to WRITE to LBA 34..2046 of whatever the kernel selected as its
// root block device. On our own image that is the standard GPT alignment gap
// and nothing owns it. On a stick that was not written from our image it could
// be anything, and the kernel selects a USB root before it has proved the root
// is ours.
//
// So arming is gated on a structural fact rather than on a filename: every
// non-empty GPT partition entry must start at or after LBA 2048. The GPT header
// parse itself is NOT re-implemented here; fs/fltrec.c calls the existing
// parttbl_gpt_hdr_rs (rustkern/parttbl.rs) for the magic check and the
// entry-array geometry, and passes the resulting esz/per_sec in here.
//
// A disk with no valid GPT header fails that first step and arming is refused
// outright, which is deliberate: an MBR disk's post-MBR gap is exactly where
// GRUB embeds core.img, and we will not gamble on it.
// ---------------------------------------------------------------------------

/// Validate the GPT header sector (LBA 1) and hand back the entry-array
/// geometry the scan below needs.
///
/// THE PARSE IS NOT RE-IMPLEMENTED HERE. This delegates to the crate's existing
/// `parttbl_gpt_hdr_rs` (rustkern/parttbl.rs), which already does the
/// "EFI PART" magic check and the esz/num acceptance guard and is the tree's
/// one GPT header parser. It exists as a wrapper only because
/// `parttbl_gpt_hdr_t` is declared inside fs/ext2.c rather than in a header, so
/// there is no way for fs/fltrec.c to name that struct without duplicating its
/// definition; four scalars out-params cost nothing and cannot drift.
///
/// Returns 0 on accept, -1 on reject.
///
/// # Safety
/// `sec` must point to at least `len` readable bytes; each out pointer must be
/// a caller-owned, aligned scalar.
#[no_mangle]
pub extern "C" fn fltrec_gpt_geom_rs(
    sec: *const u8,
    len: u32,
    out_ent_lba: *mut u64,
    out_num: *mut u32,
    out_esz: *mut u32,
    out_per_sec: *mut u32,
) -> i32 {
    if sec.is_null()
        || out_ent_lba.is_null()
        || out_num.is_null()
        || out_esz.is_null()
        || out_per_sec.is_null()
    {
        return -1;
    }
    let mut h = crate::parttbl::GptHdr {
        ent_lba: 0,
        num: 0,
        esz: 0,
        per_sec: 0,
        _pad: 0,
    };
    // SAFETY: `sec`/`len` are passed straight through with the same contract
    // parttbl_gpt_hdr_rs already documents (at least `len` readable bytes), and
    // `h` is our own stack local, so the out pointer is valid and aligned.
    if crate::parttbl::parttbl_gpt_hdr_rs(sec, len, &mut h as *mut crate::parttbl::GptHdr) != 0 {
        return -1;
    }
    // SAFETY: all four out pointers null-checked above; caller-owned scalars.
    unsafe {
        *out_ent_lba = h.ent_lba;
        *out_num = h.num;
        *out_esz = h.esz;
        *out_per_sec = h.per_sec;
    }
    0
}

/// Scan ONE sector of the GPT partition-entry array and report whether any
/// entry in it overlaps the inclusive LBA range [lo, hi].
///
/// Returns 1 if this sector is CLEAR, 0 if an entry OVERLAPS (arming must be
/// refused), -1 on malformed arguments (also treated as refuse). `*out_consumed`
/// receives the number of entries examined, so the caller can walk the array.
///
/// # Safety
/// `sec` must point to at least `sec_len` readable bytes; `out_consumed` must
/// be a caller-owned, aligned u32.
#[no_mangle]
pub extern "C" fn fltrec_parr_scan_rs(
    sec: *const u8,
    sec_len: u32,
    esz: u32,
    per_sec: u32,
    remaining: u32,
    lo: u64,
    hi: u64,
    out_consumed: *mut u32,
) -> i32 {
    if sec.is_null() || out_consumed.is_null() {
        return -1;
    }
    // Same acceptance guard parttbl_gpt_hdr_rs applies to esz, restated because
    // this function can be called independently and must not trust its caller.
    if esz < 128 || esz > sec_len || per_sec == 0 || lo > hi {
        return -1;
    }
    // SAFETY: null-checked; the caller guarantees `sec` spans `sec_len`
    // readable bytes (a 512-byte blk_read buffer). Every access below goes
    // through this slice.
    let s = unsafe { core::slice::from_raw_parts(sec, sec_len as usize) };
    let mut consumed: u32 = 0;
    let mut e: u32 = 0;
    let mut verdict = 1i32;
    while e < per_sec && consumed < remaining {
        let off = match e.checked_mul(esz) {
            Some(v) => v as usize,
            None => break,
        };
        // An entry's fields of interest span [0,16) type GUID, [32,40) first
        // LBA and [40,48) last LBA, so 48 bytes must be present.
        let end = match off.checked_add(48) {
            Some(v) => v,
            None => break,
        };
        let ent = match s.get(off..end) {
            Some(v) => v,
            None => break, // confine: never read past the sector
        };
        let mut empty = true;
        for &b in &ent[0..16] {
            if b != 0 {
                empty = false;
                break;
            }
        }
        if !empty {
            let mut first: u64 = 0;
            let mut last: u64 = 0;
            let mut i = 0usize;
            while i < 8 {
                first |= (ent[32 + i] as u64) << (8 * i);
                last |= (ent[40 + i] as u64) << (8 * i);
                i += 1;
            }
            // A partition occupies [first, last] inclusive. Treat a reversed or
            // degenerate extent as an overlap: we would rather decline to arm
            // on a weird disk than write into one.
            if last < first || (first <= hi && last >= lo) {
                verdict = 0;
            }
        }
        e += 1;
        consumed += 1;
    }
    // SAFETY: null-checked above; caller-owned u32.
    unsafe {
        *out_consumed = consumed;
    }
    verdict
}

// ---------------------------------------------------------------------------
// SELF-TEST
//
// HOW TO MAKE IT GO RED, because a self-test nobody has watched fail is
// indistinguishable from one that is not wired up (#514/#665):
//
//     make FLTTESTFAIL=1
//
// which appends `--cfg fltrec_test_fail` to RUSTFLAGS and arms the deliberately
// wrong assertion at the bottom of this function. The [FLTREC] boot line then
// says FAIL on an otherwise healthy machine. The Makefile block MUST sit below
// the RUSTFLAGS definition, exactly like RTCLKTESTFAIL/KTZTESTFAIL/CFGTESTFAIL:
// the same option was once written 1100 lines earlier in that file and its `+=`
// was silently discarded by the plain `=`, so the flag existed, the build
// accepted it, and the self-test stayed green.
// ---------------------------------------------------------------------------

/// Exercise the encode/decode round trips, the CRC, rejection of corrupt and
/// foreign records, the append bounds and the dirty-sector arithmetic across a
/// sector boundary and at the exact end of the slot.
///
/// Returns 0 on PASS, -1 on FAIL. `*out_checks` (optional) receives the number
/// of assertions executed, so "0 checks, PASS" is distinguishable from a real
/// pass, which is the anti-vacuity property this tree keeps having to relearn.
///
/// # Safety
/// `out_checks`, if non-null, must be a caller-owned, aligned u32.
#[no_mangle]
pub extern "C" fn fltrec_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    // Closure captures both by mutable reference; declared as one `chk` so a
    // failing assertion can never be silently skipped by an early return.
    macro_rules! chk {
        ($c:expr) => {{
            n += 1;
            if !($c) {
                ok = false;
            }
        }};
    }

    // --- geometry invariants ------------------------------------------------
    // If these ever stop holding, the last slot runs past LBA 2046 and into the
    // first partition. This is the assertion that guards the partition table
    // against a careless constant edit.
    chk!(FLT_SLOT_BASE_LBA == FLT_SB_LBA + 1);
    chk!(FLT_SLOT_BASE_LBA + (FLT_SLOTS as u64) * (FLT_SLOT_SECTORS as u64) == FLT_REGION_HI);
    chk!(FLT_TEXT_SECTORS == FLT_SLOT_SECTORS - 1);
    chk!(FLT_TEXT_CAP == 257_024);
    chk!(FLT_REGION_LO == FLT_SB_LBA);

    // --- CRC32 against published vectors ------------------------------------
    chk!(crc32(b"") == 0x0000_0000);
    chk!(crc32(b"a") == 0xE8B7_BE43);
    chk!(crc32(b"123456789") == 0xCBF4_3926);
    chk!(crc32(b"The quick brown fox jumps over the lazy dog") == 0x414F_A339);
    // Every byte value reachable, and the table indexed at both ends.
    {
        let mut all = [0u8; 256];
        let mut i = 0usize;
        while i < 256 {
            all[i] = i as u8;
            i += 1;
        }
        chk!(crc32(&all) == 0x29058C73);
    }

    // --- superblock round trip ----------------------------------------------
    let mut sb = [0u8; REC_LEN];
    // Pre-dirty the buffer to prove the encoder overwrites the reserved area.
    for b in sb.iter_mut() {
        *b = 0xA5;
    }
    chk!(sb_encode_in(&mut sb, 2, 0x0102_0304_0506_0708) == 0);
    let mut d = FltSb {
        slot_base_lba: 0,
        boot_seq: 0,
        version: 0,
        slot_count: 0,
        slot_sectors: 0,
        text_sectors: 0,
        head_slot: 0,
        sb_lba: 0,
    };
    chk!(sb_decode_in(&sb, &mut d) == 0);
    chk!(d.head_slot == 2);
    chk!(d.boot_seq == 0x0102_0304_0506_0708);
    chk!(d.slot_base_lba == FLT_SLOT_BASE_LBA);
    chk!(d.slot_sectors == FLT_SLOT_SECTORS);
    chk!(d.text_sectors == FLT_TEXT_SECTORS);
    chk!(d.slot_count == FLT_SLOTS);
    chk!(d.sb_lba as u64 == FLT_SB_LBA);
    chk!(sb[100] == 0 && sb[500] == 0); // reserved area really was cleared

    // An out-of-range head slot must not be encodable at all.
    chk!(fltrec_sb_encode_rs(sb.as_mut_ptr(), REC_LEN as u32, FLT_SLOTS, 1) == -1);
    // ...and the failed call must not have damaged the record.
    chk!(sb_decode_in(&sb, &mut d) == 0 && d.head_slot == 2);

    // --- superblock REJECTION ----------------------------------------------
    // A corrupt record. This is the case that matters: the region may hold half
    // a record written by a machine that lost power, and believing it would put
    // a slot header at an LBA derived from a foreign u32.
    {
        let mut bad = sb;
        bad[200] ^= 0x01; // one bit, inside the CRC-covered area
        chk!(sb_decode_in(&bad, &mut d) == -1);
    }
    {
        let mut bad = sb;
        bad[CRC_OFF] ^= 0xFF; // the CRC field itself
        chk!(sb_decode_in(&bad, &mut d) == -1);
    }
    {
        let mut bad = sb;
        bad[0] = b'X'; // foreign magic
        chk!(sb_decode_in(&bad, &mut d) == -1);
    }
    {
        // A FOREIGN record whose CRC is internally consistent: someone else's
        // structure that happens to sit here, re-CRC'd. Magic must reject it
        // before the CRC ever agrees.
        let mut bad = [0x5Au8; REC_LEN];
        let c = crc32(&bad[0..CRC_OFF]);
        bad[CRC_OFF] = c as u8;
        bad[CRC_OFF + 1] = (c >> 8) as u8;
        bad[CRC_OFF + 2] = (c >> 16) as u8;
        bad[CRC_OFF + 3] = (c >> 24) as u8;
        chk!(sb_decode_in(&bad, &mut d) == -1);
    }
    {
        // A record with OUR magic and a VALID CRC but a different geometry (a
        // future format, or a deliberate forgery). Must reject: we would
        // otherwise derive LBAs from it.
        let mut bad = sb;
        bad[24] = 0xFF; // slot_sectors low byte
        let c = crc32(&bad[0..CRC_OFF]);
        bad[CRC_OFF] = c as u8;
        bad[CRC_OFF + 1] = (c >> 8) as u8;
        bad[CRC_OFF + 2] = (c >> 16) as u8;
        bad[CRC_OFF + 3] = (c >> 24) as u8;
        chk!(sb_decode_in(&bad, &mut d) == -1);
    }
    // An all-zero region (a virgin stick) is not a superblock.
    chk!(sb_decode_in(&[0u8; REC_LEN], &mut d) == -1);
    // A short buffer is a reject, not a read past the end.
    chk!(fltrec_sb_decode_rs(sb.as_ptr(), 511, &mut d as *mut FltSb) == -1);
    chk!(fltrec_sb_decode_rs(core::ptr::null(), 512, &mut d as *mut FltSb) == -1);

    // --- slot header round trip ---------------------------------------------
    let mut hb = [0xC3u8; REC_LEN];
    let mut ident = [0u8; 32];
    ident[0..11].copy_from_slice(b"v2.0.2-b999");
    let h = FltHdr {
        boot_seq: 42,
        seal_ms: 123_456,
        version: FLT_VERSION,
        slot_index: 3,
        build: 2019,
        verdict: FLT_VERDICT_FAIL,
        text_len: 1234,
        text_cap: FLT_TEXT_CAP,
        text_crc: 0xCBF4_3926,
        _pad: 0,
        ident,
    };
    chk!(hdr_encode_in(&mut hb, &h) == 0);
    let mut dh = FltHdr {
        boot_seq: 0,
        seal_ms: 0,
        version: 0,
        slot_index: 0,
        build: 0,
        verdict: 0,
        text_len: 0,
        text_cap: 0,
        text_crc: 0,
        _pad: 0,
        ident: [0u8; 32],
    };
    chk!(hdr_decode_in(&hb, &mut dh) == 0);
    chk!(dh.boot_seq == 42 && dh.seal_ms == 123_456);
    chk!(dh.slot_index == 3 && dh.build == 2019);
    chk!(dh.verdict == FLT_VERDICT_FAIL);
    chk!(dh.text_len == 1234 && dh.text_cap == FLT_TEXT_CAP);
    chk!(dh.text_crc == 0xCBF4_3926);
    chk!(&dh.ident[0..11] == b"v2.0.2-b999");
    chk!(dh.ident[11] == 0);
    chk!(hb[400] == 0); // reserved cleared

    // Header rejection.
    {
        let mut bad = hb;
        bad[64] ^= 0x20;
        chk!(hdr_decode_in(&bad, &mut dh) == -1);
    }
    {
        let mut bad = hb;
        bad[3] = b'x';
        chk!(hdr_decode_in(&bad, &mut dh) == -1);
    }
    // Out-of-range fields must be refused at ENCODE time, so they can never
    // reach the disk in the first place.
    {
        let mut b2 = FltHdr {
            boot_seq: 1,
            seal_ms: 0,
            version: FLT_VERSION,
            slot_index: FLT_SLOTS,
            build: 1,
            verdict: FLT_VERDICT_OPEN,
            text_len: 0,
            text_cap: FLT_TEXT_CAP,
            text_crc: 0,
            _pad: 0,
            ident: [0u8; 32],
        };
        chk!(hdr_encode_in(&mut hb.clone(), &b2) == -1);
        b2.slot_index = 0;
        b2.text_len = FLT_TEXT_CAP + 1;
        chk!(hdr_encode_in(&mut hb.clone(), &b2) == -1);
        b2.text_len = 0;
        b2.verdict = 99;
        chk!(hdr_encode_in(&mut hb.clone(), &b2) == -1);
        b2.verdict = FLT_VERDICT_OK;
        b2.text_cap = 4096;
        chk!(hdr_encode_in(&mut hb.clone(), &b2) == -1);
    }
    // A control byte in the identity string is scrubbed, not passed through.
    {
        let mut i2 = [0u8; 32];
        i2[0] = b'A';
        i2[1] = 0x07;
        i2[2] = b'B';
        let h2 = FltHdr {
            boot_seq: 1,
            seal_ms: 0,
            version: FLT_VERSION,
            slot_index: 0,
            build: 1,
            verdict: FLT_VERDICT_OPEN,
            text_len: 0,
            text_cap: FLT_TEXT_CAP,
            text_crc: 0,
            _pad: 0,
            ident: i2,
        };
        let mut b3 = [0u8; REC_LEN];
        chk!(hdr_encode_in(&mut b3, &h2) == 0);
        chk!(hdr_decode_in(&b3, &mut dh) == 0);
        chk!(dh.ident[0] == b'A' && dh.ident[1] == b'?' && dh.ident[2] == b'B');
    }

    // --- dirty-sector arithmetic --------------------------------------------
    let mut f: u32 = 0xDEAD;
    let mut l: u32 = 0xBEEF;
    // Nothing appended yet.
    chk!(dirty_in(0, 0, &mut f, &mut l) == 0);
    // One byte lands in sector 0 alone.
    chk!(dirty_in(0, 1, &mut f, &mut l) == 1 && f == 0 && l == 0);
    // Exactly one full sector: still sector 0 only. The classic off-by-one is
    // reporting sector 1 here.
    chk!(dirty_in(0, 512, &mut f, &mut l) == 1 && f == 0 && l == 0);
    // One byte past the boundary now touches sector 1 as well.
    chk!(dirty_in(0, 513, &mut f, &mut l) == 1 && f == 0 && l == 1);
    // Resuming from a boundary must NOT rewrite the completed sector.
    chk!(dirty_in(512, 513, &mut f, &mut l) == 1 && f == 1 && l == 1);
    // Resuming from mid-sector MUST rewrite that partial sector.
    chk!(dirty_in(1000, 1100, &mut f, &mut l) == 1 && f == 1 && l == 2);
    chk!(dirty_in(1000, 1001, &mut f, &mut l) == 1 && f == 1 && l == 1);
    // Nothing new since the last flush, mid-sector.
    chk!(dirty_in(1000, 1000, &mut f, &mut l) == 0);
    // THE EXACT END OF THE SLOT. Last valid text sector index is 501, not 502.
    chk!(dirty_in(FLT_TEXT_CAP - 512, FLT_TEXT_CAP, &mut f, &mut l) == 1 && f == 501 && l == 501);
    chk!(dirty_in(0, FLT_TEXT_CAP, &mut f, &mut l) == 1 && f == 0 && l == FLT_TEXT_SECTORS - 1);
    chk!(dirty_in(FLT_TEXT_CAP, FLT_TEXT_CAP, &mut f, &mut l) == 0);
    chk!(dirty_in(FLT_TEXT_CAP - 1, FLT_TEXT_CAP, &mut f, &mut l) == 1 && f == 501 && l == 501);
    // Impossible arguments are a reject, never an LBA.
    chk!(dirty_in(0, FLT_TEXT_CAP + 1, &mut f, &mut l) == -1);
    chk!(dirty_in(100, 50, &mut f, &mut l) == -1);
    chk!(dirty_in(0, 0xFFFF_FFFF, &mut f, &mut l) == -1);
    chk!(fltrec_dirty_rs(0, 1, core::ptr::null_mut(), &mut l as *mut u32) == -1);
    // A "no change" answer must not scribble on the outputs.
    f = 7;
    l = 9;
    chk!(fltrec_dirty_rs(10, 10, &mut f as *mut u32, &mut l as *mut u32) == 0);
    chk!(f == 7 && l == 9);

    // --- append -------------------------------------------------------------
    {
        let mut buf = [0u8; 32];
        let mut len = 0u32;
        len = append_in(&mut buf, len, b"hi", true);
        chk!(len == 3 && &buf[0..3] == b"hi\n");
        // A line that already ends in a newline must not get a second one.
        len = append_in(&mut buf, len, b"yo\n", true);
        chk!(len == 6 && &buf[0..6] == b"hi\nyo\n");
        // add_nl = false appends verbatim.
        len = append_in(&mut buf, len, b"ab", false);
        chk!(len == 8 && &buf[0..8] == b"hi\nyo\nab");
        // Control bytes are scrubbed, newlines survive.
        len = append_in(&mut buf, len, b"\x01\x7f", false);
        chk!(len == 10 && &buf[8..10] == b"??");
        // EXACT FIT: 22 remaining, a 21-byte line plus its newline.
        let before = len;
        len = append_in(&mut buf, len, b"012345678901234567890", true);
        chk!(len == 32 && before == 10);
        // Now full: any further append is dropped WHOLE, length unchanged.
        let full = len;
        chk!(append_in(&mut buf, len, b"x", true) == full);
        chk!(append_in(&mut buf, len, b"x", false) == full);
        chk!(buf[31] == b'\n'); // the last byte really is the newline we wrote
    }
    {
        // One byte short of fitting: still dropped whole, nothing partial.
        let mut buf = [0u8; 8];
        let len = append_in(&mut buf, 0, b"12345678", true); // 8 + newline = 9
        chk!(len == 0);
        chk!(buf[0] == 0); // NOTHING was written
        // ...and without the newline it fits exactly.
        chk!(append_in(&mut buf, 0, b"12345678", false) == 8);
    }
    // Degenerate calls cannot move the length.
    chk!(fltrec_append_rs(core::ptr::null_mut(), 32, 5, b"x".as_ptr(), 1, 1) == 5);
    {
        let mut buf = [0u8; 8];
        chk!(fltrec_append_rs(buf.as_mut_ptr(), 8, 3, core::ptr::null(), 1, 1) == 3);
        chk!(fltrec_append_rs(buf.as_mut_ptr(), 8, 3, b"x".as_ptr(), 0, 1) == 3);
        // A length claimed larger than the capacity is a bug in the caller and
        // must not be able to write anywhere.
        chk!(fltrec_append_rs(buf.as_mut_ptr(), 8, 9, b"x".as_ptr(), 1, 1) == 9);
        chk!(buf[0] == 0);
    }

    // --- GPT safety scan ----------------------------------------------------
    {
        // Build one 512-byte entry-array sector: 4 entries of 128 bytes.
        let mut sec = [0u8; 512];
        let put = |s: &mut [u8; 512], e: usize, first: u64, last: u64| {
            let o = e * 128;
            s[o] = 0x28; // any non-zero type GUID byte: entry is in use
            let mut i = 0usize;
            while i < 8 {
                s[o + 32 + i] = (first >> (8 * i)) as u8;
                s[o + 40 + i] = (last >> (8 * i)) as u8;
                i += 1;
            }
        };
        let mut consumed: u32 = 0;
        // Our real image: ESP at 2048, ext2 root at 526336. Both clear.
        put(&mut sec, 0, 2048, 526335);
        put(&mut sec, 1, 526336, 3686366);
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        chk!(consumed == 4); // the two empty entries are examined, not skipped over
        // A partition starting at 34 is exactly the disk we must refuse.
        put(&mut sec, 2, 34, 2047);
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 0);
        // A partition that merely ENDS inside the region also overlaps.
        let mut sec2 = [0u8; 512];
        put(&mut sec2, 0, 1, 40);
        chk!(fltrec_parr_scan_rs(sec2.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 0);
        // A partition that SPANS the region overlaps.
        let mut sec3 = [0u8; 512];
        put(&mut sec3, 0, 1, 4096);
        chk!(fltrec_parr_scan_rs(sec3.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 0);
        // One sector past the region is clear; one sector before the region is
        // clear. These two pin the inclusive bounds.
        let mut sec4 = [0u8; 512];
        put(&mut sec4, 0, 2048, 9999);
        chk!(fltrec_parr_scan_rs(sec4.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        let mut sec5 = [0u8; 512];
        put(&mut sec5, 0, 2, 33);
        chk!(fltrec_parr_scan_rs(sec5.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        // A reversed extent is refused rather than reasoned about.
        let mut sec6 = [0u8; 512];
        put(&mut sec6, 0, 9999, 2048);
        chk!(fltrec_parr_scan_rs(sec6.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 0);
        // An all-zero sector (no entries in use) is clear.
        let zero = [0u8; 512];
        chk!(fltrec_parr_scan_rs(zero.as_ptr(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        // `remaining` bounds the walk: with 1 entry left we stop after 1.
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 128, 4, 1, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        chk!(consumed == 1);
        // Malformed geometry is refused, never read past.
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 127, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == -1);
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 1024, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == -1);
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 512, 128, 0, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == -1);
        chk!(fltrec_parr_scan_rs(core::ptr::null(), 512, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == -1);
        // A SHORT sector with a large per_sec must confine, not over-read: 128
        // bytes of buffer, 4 entries claimed. Only entry 0 fits.
        chk!(fltrec_parr_scan_rs(sec.as_ptr(), 128, 128, 4, 128, FLT_REGION_LO, FLT_REGION_HI, &mut consumed) == 1);
        chk!(consumed == 1);
    }

    // --- GPT header geometry (delegates to parttbl_gpt_hdr_rs) --------------
    {
        let mut hdr = [0u8; 512];
        hdr[0..8].copy_from_slice(b"EFI PART");
        hdr[72] = 2; // PartitionEntryLBA = 2
        hdr[80] = 128; // NumberOfPartitionEntries = 128
        hdr[84] = 128; // SizeOfPartitionEntry = 128
        let mut el: u64 = 0;
        let mut nm: u32 = 0;
        let mut es: u32 = 0;
        let mut ps: u32 = 0;
        chk!(fltrec_gpt_geom_rs(hdr.as_ptr(), 512, &mut el, &mut nm, &mut es, &mut ps) == 0);
        chk!(el == 2 && nm == 128 && es == 128 && ps == 4);
        // A sector with no GPT magic is refused, which is what stops us writing
        // into an MBR disk's post-MBR gap.
        let mut bad = hdr;
        bad[1] = b'x';
        chk!(fltrec_gpt_geom_rs(bad.as_ptr(), 512, &mut el, &mut nm, &mut es, &mut ps) == -1);
        chk!(fltrec_gpt_geom_rs([0u8; 512].as_ptr(), 512, &mut el, &mut nm, &mut es, &mut ps) == -1);
        chk!(fltrec_gpt_geom_rs(core::ptr::null(), 512, &mut el, &mut nm, &mut es, &mut ps) == -1);
        chk!(
            fltrec_gpt_geom_rs(hdr.as_ptr(), 512, core::ptr::null_mut(), &mut nm, &mut es, &mut ps)
                == -1
        );
        // A short sector is a reject, not a read past the end.
        chk!(fltrec_gpt_geom_rs(hdr.as_ptr(), 64, &mut el, &mut nm, &mut es, &mut ps) == -1);
    }

    // --- the deliberate failure, armed by `make FLTTESTFAIL=1` --------------
    // A CRC32 that has been proven correct three assertions above cannot also
    // be zero, so this assertion is false by construction whenever it compiles.
    #[cfg(fltrec_test_fail)]
    {
        chk!(crc32(b"123456789") == 0);
    }

    if !out_checks.is_null() {
        // SAFETY: null-checked; the caller passes the address of a u32 local.
        unsafe {
            *out_checks = n;
        }
    }
    if ok {
        0
    } else {
        -1
    }
}
