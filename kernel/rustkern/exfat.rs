// rustkern/exfat.rs - #404 Phase T / #501 exFAT directory entry-set decode
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 Phase T port (#501): exFAT directory entry-set decode (Tier-2).
//
// Faithful, memory-safe Rust drop-in for parse_entry_set / exfat_dir_step_c in
// fs/exfat.c. exfat_readdir reads a directory cluster off a hot-plugged USB
// exFAT volume and decodes each 32-byte on-disk entry SET here: a File entry
// (0x85) carrying SecondaryCount, a Stream Extension (0xC0) carrying NameLength /
// DataLength / ValidDataLength, and ceil(NameLength/15) File Name entries (0xC1)
// each holding 15 UTF-16 code units, reassembled into the filename.
// SecondaryCount, NameLength and every entry byte are attacker-supplyable (an
// exFAT volume arrives on a USB stick), so this is a Tier-2 untrusted-input
// parser. Pure: no disk I/O, no FS mutation (the C `fs` handle went unused).
//
// ExfatDirInfo is a #[repr(C)] mirror of exfat_dir_info_t (fs/exfat.h); its size
// (296) is asserted in fs/exfat.c so this FFI can never silently drift.
//
// CONFINEMENT (HONEST): unlike the FAT b810 LFN case this C does NOT overflow.
// It (a) bounds SecondaryCount via `offset + bytes_used > buf_size -> -1` so the
// whole entry set must fit the cluster buffer, and (b) caps the name reassembly
// with `name_offset + j < 255` into name_buf[256]. The residual defect the Rust
// closes is that the C's name_buf is UNINITIALIZED: a crafted NameLength larger
// than the File Name entries actually present makes the UTF-16 decode read stale
// stack (a non-deterministic stack info-leak into the filename), never an OOB.
// The Rust (a) reads every disk-controlled field through a slice spanning exactly
// the block (bounds-checked index, never OOB) and (b) zero-inits the name buffer,
// so an over-long NameLength decodes exactly the chars actually present -
// deterministic, no leak. On real (well-formed) entries it is byte-identical.
// ============================================================================

use crate::common::{elf_rd_u16, elf_rd_u32, elf_rd_u64};

const EXFAT_ENTRY_EOD_RS: u8 = 0x00;
const EXFAT_ENTRY_FILE_RS: u8 = 0x85;
const EXFAT_ENTRY_STREAM_RS: u8 = 0xC0;
const EXFAT_ENTRY_NAME_RS: u8 = 0xC1;
const EXFAT_FLAG_CONTIGUOUS_RS: u8 = 0x02;

#[repr(C)]
pub struct ExfatDirInfo {
    pub name: [u8; 256],
    pub attr: u16,
    pub first_cluster: u32,
    pub file_size: u64,
    pub valid_size: u64,
    pub is_contiguous: i32,
    pub create_time: u32,
    pub modify_time: u32,
    pub access_time: u32,
}

// Faithful port of fs/exfat.c utf16le_to_utf8: encode `src_len` UTF-16LE code
// units into `dst` (NUL-terminated), stopping at a 0x0000 unit or when dst is
// full. Byte-identical to the C encoder (BMP only, matching the C which has no
// surrogate handling). Every dst write is a checked slice index.
fn exfat_utf16le_to_utf8_rs(src: &[u16; 256], src_len: i32, dst: &mut [u8; 256], dst_size: i32) -> i32 {
    let mut di: i32 = 0;
    let mut si: i32 = 0;
    while si < src_len && (si as usize) < src.len() && src[si as usize] != 0 && di < dst_size - 1 {
        let c = src[si as usize];
        if c < 0x80 {
            dst[di as usize] = c as u8;
            di += 1;
        } else if c < 0x800 {
            if di + 2 > dst_size - 1 {
                break;
            }
            dst[di as usize] = 0xC0 | ((c >> 6) as u8);
            di += 1;
            dst[di as usize] = 0x80 | ((c & 0x3F) as u8);
            di += 1;
        } else {
            if di + 3 > dst_size - 1 {
                break;
            }
            dst[di as usize] = 0xE0 | ((c >> 12) as u8);
            di += 1;
            dst[di as usize] = 0x80 | (((c >> 6) & 0x3F) as u8);
            di += 1;
            dst[di as usize] = 0x80 | ((c & 0x3F) as u8);
            di += 1;
        }
        si += 1;
    }
    dst[di as usize] = 0;
    di
}

/// Rust port of fs/exfat.c parse_entry_set (exfat_dir_step_c). Pure: no disk
/// I/O, no FS mutation. Same return contract: 0 = end-of-directory (0x00), -1 =
/// error/reject, 32 = deleted/non-file entry skipped, else the number of bytes
/// (32 * total_entries) the decoded File entry set consumed. Byte-identical to
/// the C reference on real entries; confines the uninitialized-stack info-leak
/// described above.
#[no_mangle]
pub extern "C" fn exfat_dir_step_rs(
    buf: *const u8,
    buf_size: u32,
    offset: u32,
    info: *mut ExfatDirInfo,
) -> i32 {
    if buf.is_null() {
        return -1;
    }
    let bs = buf_size as usize;
    // SAFETY: the caller (exfat_readdir) guarantees `buf` points to at least
    // buf_size (= cluster_size) contiguous readable bytes - a freshly
    // kmalloc(cluster_size) buffer filled by exfat_read_cluster. We span exactly
    // that extent and index ONLY through the slice, so every disk-controlled
    // field access is bounds-checked by Rust: a crafted SecondaryCount /
    // NameLength / entry byte can only hit a guard or return, never an OOB read.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, bs) };

    let off = offset as usize;
    // Matches C `if (offset >= buf_size) return -1;`
    if off >= bs {
        return -1;
    }

    let etype = s[off]; // in bounds: off < bs
    if etype == EXFAT_ENTRY_EOD_RS {
        return 0; // end of directory
    }
    if (etype & 0x80) == 0 {
        return 32; // EXFAT_ENTRY_IS_DELETED: skip deleted
    }
    if etype != EXFAT_ENTRY_FILE_RS {
        return 32; // skip non-file entries
    }

    if info.is_null() {
        return -1;
    }
    // SAFETY: `info` is non-null and points to a writable ExfatDirInfo (296
    // bytes, asserted == sizeof(exfat_dir_info_t) in fs/exfat.c). Zero it first,
    // exactly like the C `memset(info, 0, sizeof(exfat_dir_info_t))`.
    unsafe {
        core::ptr::write_bytes(info as *mut u8, 0, core::mem::size_of::<ExfatDirInfo>());
    }
    let out: &mut ExfatDirInfo = unsafe { &mut *info };

    // exfat_file_entry_t (packed): secondary_count@+1, file_attributes@+4,
    // create_timestamp@+8, modify_timestamp@+12, access_timestamp@+16. All
    // bounds-checked; on the real (32-aligned block) path they always succeed.
    out.attr = elf_rd_u16(s, (off + 4) as u64).unwrap_or(0);
    out.create_time = elf_rd_u32(s, (off + 8) as u64).unwrap_or(0);
    out.modify_time = elf_rd_u32(s, (off + 12) as u64).unwrap_or(0);
    out.access_time = elf_rd_u32(s, (off + 16) as u64).unwrap_or(0);

    let secondary_count = match s.get(off + 1) {
        Some(&v) => v as i32,
        None => return -1,
    };
    let total_entries = 1 + secondary_count;
    let bytes_used = total_entries * 32;
    // Matches C `if (offset + bytes_used > buf_size) return -1;` - this is the
    // SecondaryCount bound: the whole entry set must fit the cluster buffer.
    if off + (bytes_used as usize) > bs {
        return -1;
    }

    // Zero-initialised (the CONFINEMENT of the C's uninitialised name_buf).
    let mut name_buf = [0u16; 256];
    let mut name_len: i32 = 0;

    let mut i: i32 = 1;
    // Matches C `for (i=1; i <= secondary_count && offset + i*32 < buf_size; i++)`
    while i <= secondary_count && off + (i as usize) * 32 < bs {
        let soff = off + (i as usize) * 32;
        let stype = s[soff]; // soff < bs by the loop condition
        if stype == EXFAT_ENTRY_STREAM_RS {
            // exfat_stream_entry_t (packed): general_flags@+1, name_length@+3,
            // valid_data_length@+8 (u64), first_cluster@+20 (u32),
            // data_length@+24 (u64).
            out.first_cluster = elf_rd_u32(s, (soff + 20) as u64).unwrap_or(0);
            out.file_size = elf_rd_u64(s, (soff + 24) as u64).unwrap_or(0);
            out.valid_size = elf_rd_u64(s, (soff + 8) as u64).unwrap_or(0);
            let gflags = s.get(soff + 1).copied().unwrap_or(0);
            out.is_contiguous = if (gflags & EXFAT_FLAG_CONTIGUOUS_RS) != 0 { 1 } else { 0 };
            name_len = s.get(soff + 3).copied().unwrap_or(0) as i32;
        } else if stype == EXFAT_ENTRY_NAME_RS {
            // exfat_name_entry_t (packed): name[15] UTF-16LE @+2 (2 bytes each).
            // First File Name entry is at secondary index 2. Matches C exactly,
            // including the `name_offset + j < 255` cap into name_buf[256].
            if i >= 2 {
                let name_offset = (i - 2) * 15;
                let mut j: i32 = 0;
                while j < 15 && name_offset + j < 255 {
                    let ch = elf_rd_u16(s, (soff + 2 + (j as usize) * 2) as u64).unwrap_or(0);
                    let idx = (name_offset + j) as usize;
                    if idx < name_buf.len() {
                        name_buf[idx] = ch;
                    }
                    j += 1;
                }
            }
        }
        i += 1;
    }

    // Matches C `if (name_len > 0) utf16le_to_utf8(name_buf, name_len, info->name, sizeof(info->name));`
    if name_len > 0 {
        exfat_utf16le_to_utf8_rs(&name_buf, name_len, &mut out.name, 256);
    }

    bytes_used
}
