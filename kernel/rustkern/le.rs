// rustkern/le.rs - #740: LE (Linear Executable) parser and loader, in-kernel.
//
// This is NEW kernel code, so per the standing rule it is Rust, not C. There is
// no `<fn>_c` twin and no -DRUST_LE strangler flag: nothing is being replaced.
// The reference implementation it is written against is the host-side C parser
// tools/le-harness/le.c, which parses, loads and relocates four real LE
// binaries and survives 12,000 mutation-fuzz iterations under ASan. That C
// stays where it is as the HOST harness; this file is the kernel's copy of the
// same algorithm, in a language where the bounds checks cannot be forgotten.
//
// Every measured fact this implements is recorded in docs/DOS4GW_LE_FORMAT.md:
//
//  * The MZ at file offset 0 of a wbind'ed DOS/4GW executable is the WRONG MZ.
//    Page data is anchored on the LAST MZ whose e_lfanew lands on "LE", which
//    in DOOM.EXE is at 0x25214, not 0. A wrong page base is SILENT: it produces
//    a module whose strings and interrupt histogram look perfectly healthy and
//    whose cross-references all return zero hits (blame.md, 2026-08-07).
//  * The LE header MIXES TWO ORIGINS. obj_tab_off / page_map_off /
//    fixup_page_tab_off / fixup_rec_tab_off are LE-header-relative;
//    data_pages_off (+0x80) is anchored on the MZ. Open Watcom's linker does
//    this deliberately (bld/wl/c/loadflat.c).
//  * Page numbers are 24-bit BIG-ENDIAN, 1-based, 0 meaning "no file data".
//    Settled on Shadow Warrior (317 pages) and against the producer's source.
//  * src_off in a fixup record is SIGNED: a fixup whose 4-byte value crosses
//    the end of its page is listed a SECOND time on the following page with a
//    negative offset. Reading it unsigned writes ~64 KiB away, silently.
//  * A fixup record has NO fixed length. Both 7- and 9-byte records occur in
//    bulk in the same file; the length is a function of the flag bits.
//
// MEMORY SAFETY. The input is a downloaded game: it is hostile. #476 shipped a
// heap over-read in exactly this kind of parser by trusting an on-disk length
// field. Two rules hold here, and they are stronger than "it is Rust":
//
//   1. EVERY read of the file goes through rd8/rd16/rd32, which return
//      Option and are indexed with `get()`. There is no `s[i]` on a
//      file-derived index anywhere in this file. That matters because a
//      panicking index in a kernel parser is not a safe failure: the
//      #[panic_handler] in rustkern.rs HALTS THE CPU. A malformed header must
//      return an error code, not take the machine down.
//   2. All offset arithmetic is done in u64 and range-checked before it is
//      narrowed, so nothing can wrap into a small value that then passes a
//      check.
//
// Nothing here allocates. The only writes are into the caller's explicitly
// sized buffer, through mem_range(), which does the same overflow-safe check.

// ---------------------------------------------------------------------------
// error codes - numerically identical to le_err_t in tools/le-harness/le.h and
// to the mirror in kernel/exec/le.h (sizeof/value locked by _Static_assert).
// ---------------------------------------------------------------------------
pub const LE_OK: i32 = 0;
pub const LE_E_TRUNCATED: i32 = 1;
pub const LE_E_NO_MZ: i32 = 2;
#[allow(dead_code)] // part of the numbering: le_err_t values must stay aligned with the C mirror
pub const LE_E_NO_LE: i32 = 3;
pub const LE_E_LX_UNSUPPORTED: i32 = 4;
pub const LE_E_BYTE_ORDER: i32 = 5;
pub const LE_E_PAGE_SIZE: i32 = 6;
pub const LE_E_PAGE_COUNT: i32 = 7;
pub const LE_E_OBJECT_COUNT: i32 = 8;
pub const LE_E_OBJECT_RANGE: i32 = 9;
pub const LE_E_OBJECT_OVERLAP: i32 = 10;
pub const LE_E_PAGE_NUM: i32 = 11;
pub const LE_E_PAGE_FLAGS: i32 = 12;
pub const LE_E_ITERATED: i32 = 13;
pub const LE_E_PAGE_DATA: i32 = 14;
pub const LE_E_FIXUP_TABLE: i32 = 15;
pub const LE_E_FIXUP_RECORD: i32 = 16;
pub const LE_E_FIXUP_TARGET: i32 = 17;
pub const LE_E_SRC_TYPE: i32 = 18;
pub const LE_E_IMPORTS: i32 = 19;
pub const LE_E_ENTRY: i32 = 20;
pub const LE_E_OVERFLOW: i32 = 21;
pub const LE_E_MEM: i32 = 22;
pub const LE_E_COUNT: i32 = 23;

const ERRSTR: [&str; LE_E_COUNT as usize] = [
    "ok\0",
    "truncated\0",
    "no MZ header\0",
    "no LE signature\0",
    "LX not supported\0",
    "not little-endian\0",
    "bad page size\0",
    "bad page count\0",
    "bad object count\0",
    "object page range escapes module\0",
    "objects overlap in linear space\0",
    "page number out of range\0",
    "unknown page flags\0",
    "iterated page not supported\0",
    "page data outside file\0",
    "bad fixup page table\0",
    "bad fixup record\0",
    "fixup target object does not exist\0",
    "unsupported fixup source type\0",
    "module has imports\0",
    "bad entry point\0",
    "arithmetic overflow\0",
    "image buffer too small\0",
];

/// NUL-terminated static description of an le error code. One implementation,
/// shared by every C printer.
#[no_mangle]
pub extern "C" fn le_strerror_rs(e: i32) -> *const u8 {
    if e < 0 || e >= LE_E_COUNT {
        return "unknown\0".as_ptr();
    }
    ERRSTR[e as usize].as_ptr()
}

pub const LE_MAX_OBJECTS: usize = 64;
pub const LE_MAX_PAGES: u32 = 0x0010_0000; // 1M pages * 4 KiB = 4 GiB

// object flags
#[allow(dead_code)] // decoded and printed on the C side; kept here so the flag map has one home
pub const LE_OBJ_ALIAS_16_16: u32 = 0x1000;
#[allow(dead_code)]
pub const LE_OBJ_BIG: u32 = 0x2000;

// page map flags (4th byte of each entry)
pub const LE_PAGE_VALID: u8 = 0;
pub const LE_PAGE_ITERATED: u8 = 1;
pub const LE_PAGE_INVALID: u8 = 2;
pub const LE_PAGE_ZEROED: u8 = 3;
pub const LE_PAGE_RANGE: u8 = 4;

// fixup source types (low nibble of the source byte)
pub const LE_SRC_BYTE: u8 = 0;
pub const LE_SRC_SEL16: u8 = 2;
pub const LE_SRC_PTR16_16: u8 = 3;
pub const LE_SRC_OFF16: u8 = 5;
pub const LE_SRC_PTR16_32: u8 = 6;
pub const LE_SRC_OFF32: u8 = 7;
pub const LE_SRC_SELFREL32: u8 = 8;
pub const LE_SRC_LIST: u8 = 0x20;

// fixup target flags (the second byte)
pub const LE_TGT_TYPE_MASK: u8 = 0x03;
pub const LE_TGT_INTERNAL: u8 = 0;
pub const LE_TGT_ADDITIVE: u8 = 0x04;
pub const LE_TGT_OFF32: u8 = 0x10;
pub const LE_TGT_ADD32: u8 = 0x20;
pub const LE_TGT_ORD16: u8 = 0x40;

// ---------------------------------------------------------------------------
// FFI structs. All-u32 on purpose: no pointers and no mixed widths, so the
// layout is trivially reproducible on the C side and locked by _Static_assert.
// The file buffer is passed as (ptr, len) to every call rather than being
// stashed in the struct, so a stale pointer cannot outlive its buffer here.
// ---------------------------------------------------------------------------

/// Mirrors le_object_t in kernel/exec/le.h. 24 bytes.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct LeObject {
    pub virt_size: u32,
    pub reloc_base: u32,
    pub flags: u32,
    pub page_index: u32, // 1-based index into the page map
    pub page_count: u32,
    pub reserved: u32,
}

/// Mirrors le_image_t in kernel/exec/le.h.
#[repr(C)]
pub struct LeImage {
    pub mz_off: u32, // the ANCHOR MZ, origin for page_data_abs
    pub le_off: u32, // absolute file offset of the LE header
    pub cpu_type: u32,
    pub os_type: u32,
    pub mod_flags: u32,
    pub num_pages: u32,
    pub page_size: u32,
    pub last_page_size: u32,
    pub eip_obj: u32, // 1-based
    pub eip: u32,
    pub esp_obj: u32,
    pub esp: u32,
    pub num_objects: u32,
    // LE-header-relative, already bounds-checked
    pub obj_tab_off: u32,
    pub page_map_off: u32,
    pub fixup_page_tab_off: u32,
    pub fixup_rec_tab_off: u32,
    pub import_mod_tab_off: u32,
    pub import_proc_tab_off: u32,
    pub num_import_mod: u32,
    pub fixup_sect_size: u32,
    pub loader_sect_size: u32,
    // ABSOLUTE file offset of page data, already anchored on mz_off
    pub page_data_abs: u32,
    pub page_data_len: u32,
    pub lin_lo: u32,
    pub lin_hi: u32,
    pub reloc_delta: u32, // total delta applied by le_relocate_rs so far
    pub _pad: u32,
    pub obj: [LeObject; LE_MAX_OBJECTS],
}

/// One decoded fixup source. A source list of N yields N of these.
/// Mirrors le_fixup_t in kernel/exec/le.h.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct LeFixup {
    pub src: u32,      // raw source byte
    pub flags: u32,    // raw target flags byte
    pub src_type: u32, // src & 0x0f
    pub tgt_type: u32, // flags & 0x03
    pub src_off: i32,  // SIGNED; may be negative (page-straddling continuation)
    pub tgt_obj: u32,  // 1-based
    pub tgt_off: u32,  // additive already folded in
    pub rec_off: u32,  // file offset of the record, for diagnostics
    pub rec_len: u32,  // decoded byte length of the record
    pub page: u32,     // 0-based page the record belongs to
}

/// Fixup census. This is what sizes the fixup engine: see
/// docs/DOS4GW_LE_FORMAT.md section 4.1. Mirrors le_hist_t.
#[repr(C)]
pub struct LeHist {
    pub records: u32, // fixup RECORDS decoded
    pub sources: u32, // expanded SOURCE entries (a source list expands)
    pub pages_with_fixups: u32,
    pub max_page_sources: u32,
    pub by_src: [u32; 16],     // by source type (low nibble)
    pub by_tgt: [u32; 4],      // by target type
    pub by_rec_len: [u32; 16], // record byte length, index 15 = 15 or more
    pub src_list_recs: u32,
    pub additive: u32,
    pub tgt_off32: u32, // records carrying a 32-bit target offset
    pub tgt_off16: u32,
    pub ord16: u32,        // records with a 16-bit object ordinal
    pub neg_src_off: u32,  // src_off < 0
    pub straddling: u32,   // the write crosses the end of its page
    pub by_page_flag: [u32; 8], // page map flag histogram
    pub pages_no_data: u32,     // page number 0 = no file data
    pub last_page_num: u32,     // raw 24-bit page number of the last entry
}

/// Mirrors le_stats_t. Filled by le_load_rs.
#[repr(C)]
pub struct LeStats {
    pub pages_copied: u32,
    pub pages_zeroed: u32,
    pub pages_short: u32,
    pub fixups_applied: u32,
    pub fixups_by_src: [u32; 16],
    pub fixups_negative_off: u32,
    pub fixups_straddling: u32,
    pub fixups_off_object: u32,
    pub bytes_copied: u32,
    pub bytes_zeroed: u32,
}

/// Mirrors le_valid_t. Filled by le_validate_rs, the post-load invariant.
#[repr(C)]
pub struct LeValid {
    pub checked: u32,      // 32-bit-offset fixups read back
    pub inside_object: u32, // ... whose written value points inside an object
    pub outside: u32,
    pub unreadable: u32, // source address not inside the caller's buffer
    pub first_bad_lin: u32,
    pub first_bad_val: u32,
}

// ---------------------------------------------------------------------------
// bounds-checked primitive reads. Option-returning by construction: there is
// no path from a file-derived offset to a panicking index.
// ---------------------------------------------------------------------------

#[inline]
fn rd8(f: &[u8], o: u64) -> Option<u8> {
    if o > usize::MAX as u64 {
        return None;
    }
    f.get(o as usize).copied()
}

#[inline]
fn rd16(f: &[u8], o: u64) -> Option<u16> {
    let a = rd8(f, o)? as u16;
    let b = rd8(f, o.checked_add(1)?)? as u16;
    Some(a | (b << 8))
}

#[inline]
fn rd32(f: &[u8], o: u64) -> Option<u32> {
    let a = rd16(f, o)? as u32;
    let b = rd16(f, o.checked_add(2)?)? as u32;
    Some(a | (b << 16))
}

/// Is [off, off+len) entirely inside the file? Overflow-safe.
#[inline]
fn rd_ok(flen: u32, off: u64, len: u64) -> bool {
    let flen = flen as u64;
    if len > flen {
        return false;
    }
    off <= flen - len
}

// ---------------------------------------------------------------------------
// locating the LE
// ---------------------------------------------------------------------------

/// Scan for the LAST MZ whose e_lfanew lands on an "LE"/"LX" signature, and
/// record BOTH offsets. Recording the MZ offset is not bookkeeping: it is the
/// origin for the page data, and getting it wrong is silent.
fn find(f: &[u8]) -> Result<(u32, u32), i32> {
    let len = f.len() as u32;
    if f.len() < 0x40 {
        return Err(LE_E_TRUNCATED);
    }
    let mut found: Option<(u32, u32)> = None;
    let mut i: u64 = 0;
    while i + 0x40 <= len as u64 {
        if rd8(f, i) == Some(b'M') && rd8(f, i + 1) == Some(b'Z') {
            if let Some(lfa) = rd32(f, i + 0x3c) {
                if lfa >= 0x40 && rd_ok(len, i + lfa as u64, 2) {
                    let s0 = rd8(f, i + lfa as u64);
                    let s1 = rd8(f, i + lfa as u64 + 1);
                    if s0 == Some(b'L') && (s1 == Some(b'E') || s1 == Some(b'X')) {
                        found = Some((i as u32, (i + lfa as u64) as u32));
                    }
                }
            }
        }
        i += 1;
    }
    if let Some((mz, le)) = found {
        if rd8(f, le as u64 + 1) == Some(b'X') {
            return Err(LE_E_LX_UNSUPPORTED);
        }
        return Ok((mz, le));
    }
    // A bare LE with no stub is legal (mz_off = 0).
    if rd8(f, 0) == Some(b'L') && rd8(f, 1) == Some(b'E') {
        return Ok((0, 0));
    }
    if rd8(f, 0) == Some(b'L') && rd8(f, 1) == Some(b'X') {
        return Err(LE_E_LX_UNSUPPORTED);
    }
    Err(LE_E_NO_MZ)
}

/// Locate the LE. `mz_off` and `le_off` are written only on success.
///
/// # Safety
/// `file` must point to at least `len` contiguous readable bytes; `mz_off` and
/// `le_off` must be writable u32s owned by the caller.
#[no_mangle]
pub unsafe extern "C" fn le_find_rs(
    file: *const u8,
    len: u32,
    mz_off: *mut u32,
    le_off: *mut u32,
) -> i32 {
    if file.is_null() || mz_off.is_null() || le_off.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above. We only read through the slice, which is
    // exactly `len` bytes, so every file-derived index is bounds-checked.
    let f = core::slice::from_raw_parts(file, len as usize);
    match find(f) {
        Ok((m, l)) => {
            *mz_off = m;
            *le_off = l;
            LE_OK
        }
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// header + tables
// ---------------------------------------------------------------------------

fn parse(f: &[u8], img: &mut LeImage) -> Result<(), i32> {
    let len = f.len() as u32;
    let (mz, b) = find(f)?;
    if !rd_ok(len, b as u64, 0xac) {
        return Err(LE_E_TRUNCATED);
    }
    let bb = b as u64;
    let h32 = |o: u64| -> Result<u32, i32> { rd32(f, bb + o).ok_or(LE_E_TRUNCATED) };
    let h16 = |o: u64| -> Result<u16, i32> { rd16(f, bb + o).ok_or(LE_E_TRUNCATED) };
    let h8 = |o: u64| -> Result<u8, i32> { rd8(f, bb + o).ok_or(LE_E_TRUNCATED) };

    img.mz_off = mz;
    img.le_off = b;

    if h8(0x02)? != 0 || h8(0x03)? != 0 {
        return Err(LE_E_BYTE_ORDER);
    }

    img.cpu_type = h16(0x08)? as u32;
    img.os_type = h16(0x0a)? as u32;
    img.mod_flags = h32(0x10)?;
    img.num_pages = h32(0x14)?;
    img.eip_obj = h32(0x18)?;
    img.eip = h32(0x1c)?;
    img.esp_obj = h32(0x20)?;
    img.esp = h32(0x24)?;
    img.page_size = h32(0x28)?;
    img.last_page_size = h32(0x2c)?;
    img.fixup_sect_size = h32(0x30)?;
    img.loader_sect_size = h32(0x38)?;
    img.obj_tab_off = h32(0x40)?;
    img.num_objects = h32(0x44)?;
    img.page_map_off = h32(0x48)?;
    img.fixup_page_tab_off = h32(0x68)?;
    img.fixup_rec_tab_off = h32(0x6c)?;
    img.import_mod_tab_off = h32(0x70)?;
    img.num_import_mod = h32(0x74)?;
    img.import_proc_tab_off = h32(0x78)?;

    if img.page_size != 4096 {
        return Err(LE_E_PAGE_SIZE);
    }
    if img.num_pages == 0 || img.num_pages > LE_MAX_PAGES {
        return Err(LE_E_PAGE_COUNT);
    }
    if img.last_page_size == 0 || img.last_page_size > img.page_size {
        return Err(LE_E_PAGE_COUNT);
    }
    if img.num_objects == 0 || img.num_objects as usize > LE_MAX_OBJECTS {
        return Err(LE_E_OBJECT_COUNT);
    }

    // THE TWO ORIGINS. data_pages_off (+0x80) is anchored on the ANCHOR MZ, not
    // on the LE header and not on file offset 0. Everything above is
    // LE-header-relative. Getting this wrong loads a plausible-looking module
    // whose every cross-reference misses.
    let need = mz as u64 + h32(0x80)? as u64;
    if need > 0xffff_ffff {
        return Err(LE_E_OVERFLOW);
    }
    img.page_data_abs = need as u32;

    let plen = (img.num_pages as u64 - 1) * img.page_size as u64 + img.last_page_size as u64;
    if plen > 0xffff_ffff {
        return Err(LE_E_OVERFLOW);
    }
    img.page_data_len = plen as u32;
    if !rd_ok(len, img.page_data_abs as u64, img.page_data_len as u64) {
        return Err(LE_E_PAGE_DATA);
    }

    // object table (24 bytes each), page map (4 each), fixup page table (num+1 u32)
    if !rd_ok(len, bb + img.obj_tab_off as u64, img.num_objects as u64 * 24) {
        return Err(LE_E_TRUNCATED);
    }
    if !rd_ok(len, bb + img.page_map_off as u64, img.num_pages as u64 * 4) {
        return Err(LE_E_TRUNCATED);
    }
    if !rd_ok(
        len,
        bb + img.fixup_page_tab_off as u64,
        (img.num_pages as u64 + 1) * 4,
    ) {
        return Err(LE_E_TRUNCATED);
    }

    let mut i = 0usize;
    while i < img.num_objects as usize {
        let o = bb + img.obj_tab_off as u64 + i as u64 * 24;
        let ob = LeObject {
            virt_size: rd32(f, o).ok_or(LE_E_TRUNCATED)?,
            reloc_base: rd32(f, o + 4).ok_or(LE_E_TRUNCATED)?,
            flags: rd32(f, o + 8).ok_or(LE_E_TRUNCATED)?,
            page_index: rd32(f, o + 12).ok_or(LE_E_TRUNCATED)?,
            page_count: rd32(f, o + 16).ok_or(LE_E_TRUNCATED)?,
            reserved: rd32(f, o + 20).ok_or(LE_E_TRUNCATED)?,
        };
        if ob.page_count > img.num_pages {
            return Err(LE_E_OBJECT_RANGE);
        }
        if ob.page_count != 0 {
            if ob.page_index == 0 || ob.page_index > img.num_pages {
                return Err(LE_E_OBJECT_RANGE);
            }
            if ob.page_index as u64 - 1 + ob.page_count as u64 > img.num_pages as u64 {
                return Err(LE_E_OBJECT_RANGE);
            }
        }
        // virt_size must cover the pages the object claims
        if ob.page_count as u64 * img.page_size as u64
            > ob.virt_size as u64 + img.page_size as u64
        {
            return Err(LE_E_OBJECT_RANGE);
        }
        if ob.reloc_base as u64 + ob.virt_size as u64 > 0xffff_ffff {
            return Err(LE_E_OVERFLOW);
        }
        if i == 0 || ob.reloc_base < img.lin_lo {
            img.lin_lo = ob.reloc_base;
        }
        if ob.reloc_base + ob.virt_size > img.lin_hi {
            img.lin_hi = ob.reloc_base + ob.virt_size;
        }
        img.obj[i] = ob;
        i += 1;
    }

    // objects must not overlap in linear space
    let n = img.num_objects as usize;
    let mut i = 0usize;
    while i < n {
        let mut j = i + 1;
        while j < n {
            let a0 = img.obj[i].reloc_base;
            let a1 = a0 + img.obj[i].virt_size;
            let c0 = img.obj[j].reloc_base;
            let c1 = c0 + img.obj[j].virt_size;
            if a0 < c1 && c0 < a1 {
                return Err(LE_E_OBJECT_OVERLAP);
            }
            j += 1;
        }
        i += 1;
    }

    // fixup page table must be monotonic and inside the fixup record table
    let mut prev = 0u32;
    let mut i = 0u64;
    while i <= img.num_pages as u64 {
        let v = rd32(f, bb + img.fixup_page_tab_off as u64 + i * 4).ok_or(LE_E_TRUNCATED)?;
        if v < prev {
            return Err(LE_E_FIXUP_TABLE);
        }
        if !rd_ok(len, bb + img.fixup_rec_tab_off as u64, v as u64) {
            return Err(LE_E_FIXUP_TABLE);
        }
        prev = v;
        i += 1;
    }

    // entry point
    if img.eip_obj == 0 || img.eip_obj > img.num_objects {
        return Err(LE_E_ENTRY);
    }
    if img.eip >= img.obj[img.eip_obj as usize - 1].virt_size {
        return Err(LE_E_ENTRY);
    }
    if img.esp_obj != 0 {
        if img.esp_obj > img.num_objects {
            return Err(LE_E_ENTRY);
        }
        if img.esp > img.obj[img.esp_obj as usize - 1].virt_size {
            return Err(LE_E_ENTRY);
        }
    }

    if img.num_import_mod != 0 {
        return Err(LE_E_IMPORTS);
    }
    Ok(())
}

/// Parse and fully validate an LE image. Touches no guest memory.
///
/// # Safety
/// `file` must point to at least `len` contiguous readable bytes and `img` must
/// be a writable, properly aligned LeImage owned by the caller. `img` is fully
/// overwritten (zeroed first), so an error return leaves no stale fields.
#[no_mangle]
pub unsafe extern "C" fn le_parse_rs(file: *const u8, len: u32, img: *mut LeImage) -> i32 {
    if file.is_null() || img.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract. Zero first so a partially-filled struct can
    // never be mistaken for a parsed one after an error return.
    core::ptr::write_bytes(img as *mut u8, 0, core::mem::size_of::<LeImage>());
    let f = core::slice::from_raw_parts(file, len as usize);
    match parse(f, &mut *img) {
        Ok(()) => LE_OK,
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// page map
// ---------------------------------------------------------------------------

fn page_entry(f: &[u8], img: &LeImage, page: u32) -> Result<(u32, u8), i32> {
    if page >= img.num_pages {
        return Err(LE_E_PAGE_NUM);
    }
    let o = img.le_off as u64 + img.page_map_off as u64 + page as u64 * 4;
    if !rd_ok(f.len() as u32, o, 4) {
        return Err(LE_E_TRUNCATED);
    }
    // 24-bit BIG-ENDIAN page number, then a flags byte. SETTLED two ways:
    // Shadow Warrior (317 pages) has entry 256 = "00 01 00 00", and Open Watcom
    // writes page_num[2]=n, [1]=n>>8, [0]=n>>16. A module of 255 pages or fewer
    // cannot tell the readings apart, which is why three earlier samples all
    // "passed" under the wrong one.
    let b0 = rd8(f, o).ok_or(LE_E_TRUNCATED)? as u32;
    let b1 = rd8(f, o + 1).ok_or(LE_E_TRUNCATED)? as u32;
    let b2 = rd8(f, o + 2).ok_or(LE_E_TRUNCATED)? as u32;
    let fl = rd8(f, o + 3).ok_or(LE_E_TRUNCATED)?;
    Ok(((b0 << 16) | (b1 << 8) | b2, fl))
}

fn page_file_off(f: &[u8], img: &LeImage, page: u32) -> Result<(u32, u32), i32> {
    let (num, fl) = page_entry(f, img, page)?;
    if fl == LE_PAGE_ITERATED {
        return Err(LE_E_ITERATED);
    }
    if fl == LE_PAGE_INVALID || fl == LE_PAGE_ZEROED || num == 0 {
        return Ok((0, 0)); // no file data: page is zero-fill
    }
    if fl != LE_PAGE_VALID && fl != LE_PAGE_RANGE {
        return Err(LE_E_PAGE_FLAGS);
    }
    if num > img.num_pages {
        return Err(LE_E_PAGE_NUM);
    }
    // file_ofs = ((page_num - 1) * page_size) + page_off  (1-based, os2exe.c)
    let o = img.page_data_abs as u64 + (num as u64 - 1) * img.page_size as u64;
    let n = if num == img.num_pages {
        img.last_page_size as u64
    } else {
        img.page_size as u64
    };
    if !rd_ok(f.len() as u32, o, n) {
        return Err(LE_E_PAGE_DATA);
    }
    Ok((o as u32, n as u32))
}

fn page_object(img: &LeImage, page: u32) -> i32 {
    let mut i = 0usize;
    while i < img.num_objects as usize {
        let o = &img.obj[i];
        if o.page_count != 0
            && page as u64 + 1 >= o.page_index as u64
            && page as u64 + 1 < o.page_index as u64 + o.page_count as u64
        {
            return i as i32;
        }
        i += 1;
    }
    -1
}

fn page_linear(img: &LeImage, page: u32) -> Result<u32, i32> {
    let oi = page_object(img, page);
    if oi < 0 {
        return Err(LE_E_PAGE_NUM);
    }
    let o = &img.obj[oi as usize];
    let v = o.reloc_base as u64 + (page as u64 + 1 - o.page_index as u64) * img.page_size as u64;
    if v > 0xffff_ffff {
        return Err(LE_E_OVERFLOW);
    }
    Ok(v as u32)
}

/// Raw page map entry: 24-bit big-endian page number + flag byte.
///
/// # Safety
/// `file`/`len` as in le_parse_rs; `img` must be a parsed image; `num`/`flags`
/// must be writable u32s.
#[no_mangle]
pub unsafe extern "C" fn le_page_entry_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    page: u32,
    num: *mut u32,
    flags: *mut u32,
) -> i32 {
    if file.is_null() || img.is_null() || num.is_null() || flags.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    let f = core::slice::from_raw_parts(file, len as usize);
    match page_entry(f, &*img, page) {
        Ok((n, fl)) => {
            *num = n;
            *flags = fl as u32;
            LE_OK
        }
        Err(e) => e,
    }
}

/// File offset and byte length of a page's data. (0,0) means "no file data,
/// this page is zero-fill", which is not an error.
///
/// # Safety
/// As le_page_entry_rs.
#[no_mangle]
pub unsafe extern "C" fn le_page_file_off_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    page: u32,
    off: *mut u32,
    plen: *mut u32,
) -> i32 {
    if file.is_null() || img.is_null() || off.is_null() || plen.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    let f = core::slice::from_raw_parts(file, len as usize);
    match page_file_off(f, &*img, page) {
        Ok((o, n)) => {
            *off = o;
            *plen = n;
            LE_OK
        }
        Err(e) => e,
    }
}

/// 0-based page index -> owning object index, or -1.
///
/// # Safety
/// `img` must be a parsed LeImage.
#[no_mangle]
pub unsafe extern "C" fn le_page_object_rs(img: *const LeImage, page: u32) -> i32 {
    if img.is_null() {
        return -1;
    }
    // SAFETY: caller contract above.
    page_object(&*img, page)
}

/// Guest linear address of the start of 0-based `page`.
///
/// # Safety
/// `img` must be a parsed LeImage; `lin` a writable u32.
#[no_mangle]
pub unsafe extern "C" fn le_page_linear_rs(img: *const LeImage, page: u32, lin: *mut u32) -> i32 {
    if img.is_null() || lin.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    match page_linear(&*img, page) {
        Ok(v) => {
            *lin = v;
            LE_OK
        }
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// fixups
// ---------------------------------------------------------------------------

fn fixup_page_range(f: &[u8], img: &LeImage, page: u32) -> Result<(u32, u32), i32> {
    if page >= img.num_pages {
        return Err(LE_E_PAGE_NUM);
    }
    let t = img.le_off as u64 + img.fixup_page_tab_off as u64;
    let s0 = rd32(f, t + page as u64 * 4).ok_or(LE_E_FIXUP_TABLE)?;
    let s1 = rd32(f, t + (page as u64 + 1) * 4).ok_or(LE_E_FIXUP_TABLE)?;
    if s1 < s0 {
        return Err(LE_E_FIXUP_TABLE);
    }
    Ok((s0, s1))
}

/// Walk every fixup source of one page. `cb` returns Ok(()) to continue.
///
/// The record layout is decoded from the flag bits, NEVER from a fixed size:
/// 7- and 9-byte records occur in bulk in the same file (DOOM: 1,191 and
/// 10,912), and which one you get depends on flag bit 0x10.
fn walk_page_fixups<F>(f: &[u8], img: &LeImage, page: u32, cb: &mut F) -> Result<(), i32>
where
    F: FnMut(&LeFixup) -> Result<(), i32>,
{
    let flen = f.len() as u32;
    let (s0, s1) = fixup_page_range(f, img, page)?;
    let base = img.le_off as u64 + img.fixup_rec_tab_off as u64;
    if !rd_ok(flen, base, s1 as u64) {
        return Err(LE_E_FIXUP_TABLE);
    }
    let mut o = base + s0 as u64;
    let end = base + s1 as u64;

    while o < end {
        let rec_start = o;
        if o + 2 > end {
            return Err(LE_E_FIXUP_RECORD);
        }
        let src = rd8(f, o).ok_or(LE_E_FIXUP_RECORD)?;
        let flags = rd8(f, o + 1).ok_or(LE_E_FIXUP_RECORD)?;
        o += 2;

        let have_list = (src & LE_SRC_LIST) != 0;
        let mut cnt: u32 = 1;
        let mut list_off: u64 = 0;
        if have_list {
            if o + 1 > end {
                return Err(LE_E_FIXUP_RECORD);
            }
            cnt = rd8(f, o).ok_or(LE_E_FIXUP_RECORD)? as u32;
            o += 1;
            if cnt == 0 {
                return Err(LE_E_FIXUP_RECORD);
            }
        } else {
            if o + 2 > end {
                return Err(LE_E_FIXUP_RECORD);
            }
            list_off = o; // the single source offset; read after the target
            o += 2;
        }

        let tgt_type = flags & LE_TGT_TYPE_MASK;
        if tgt_type != LE_TGT_INTERNAL {
            // Imports are rejected, not implemented. All four measured samples
            // have exactly zero, and a module with imports is not a DOS/4GW
            // game (see docs/DOS4GW_LE_FORMAT.md section 8).
            return Err(LE_E_IMPORTS);
        }

        // object ordinal
        let tgt_obj: u32;
        if (flags & LE_TGT_ORD16) != 0 {
            if o + 2 > end {
                return Err(LE_E_FIXUP_RECORD);
            }
            tgt_obj = rd16(f, o).ok_or(LE_E_FIXUP_RECORD)? as u32;
            o += 2;
        } else {
            if o + 1 > end {
                return Err(LE_E_FIXUP_RECORD);
            }
            tgt_obj = rd8(f, o).ok_or(LE_E_FIXUP_RECORD)? as u32;
            o += 1;
        }
        if tgt_obj == 0 || tgt_obj > img.num_objects {
            return Err(LE_E_FIXUP_TARGET);
        }

        // target offset, absent for a pure 16-bit selector fixup
        let mut tgt_off: u32 = 0;
        if (src & 0x0f) != LE_SRC_SEL16 {
            if (flags & LE_TGT_OFF32) != 0 {
                if o + 4 > end {
                    return Err(LE_E_FIXUP_RECORD);
                }
                tgt_off = rd32(f, o).ok_or(LE_E_FIXUP_RECORD)?;
                o += 4;
            } else {
                if o + 2 > end {
                    return Err(LE_E_FIXUP_RECORD);
                }
                tgt_off = rd16(f, o).ok_or(LE_E_FIXUP_RECORD)? as u32;
                o += 2;
            }
        }

        if (flags & LE_TGT_ADDITIVE) != 0 {
            let add: u32;
            if (flags & LE_TGT_ADD32) != 0 {
                if o + 4 > end {
                    return Err(LE_E_FIXUP_RECORD);
                }
                add = rd32(f, o).ok_or(LE_E_FIXUP_RECORD)?;
                o += 4;
            } else {
                if o + 2 > end {
                    return Err(LE_E_FIXUP_RECORD);
                }
                add = rd16(f, o).ok_or(LE_E_FIXUP_RECORD)? as u32;
                o += 2;
            }
            // Wraps in 32 bits, which is the guest's own arithmetic.
            tgt_off = tgt_off.wrapping_add(add);
        }

        if have_list {
            list_off = o;
            if o + cnt as u64 * 2 > end {
                return Err(LE_E_FIXUP_RECORD);
            }
            o += cnt as u64 * 2;
        }

        let rec_len = (o - rec_start) as u32;
        let mut k = 0u32;
        while k < cnt {
            // src_off is SIGNED. A negative one is the continuation entry of a
            // fixup whose value straddles the end of the previous page; it
            // names the SAME linear address, so applying both is idempotent.
            let raw = rd16(f, list_off + k as u64 * 2).ok_or(LE_E_FIXUP_RECORD)?;
            let fx = LeFixup {
                src: src as u32,
                flags: flags as u32,
                src_type: (src & 0x0f) as u32,
                tgt_type: tgt_type as u32,
                src_off: raw as i16 as i32,
                tgt_obj,
                tgt_off,
                rec_off: rec_start as u32,
                rec_len,
                page,
            };
            cb(&fx)?;
            k += 1;
        }
    }
    if o != end {
        return Err(LE_E_FIXUP_RECORD);
    }
    Ok(())
}

fn src_width(src_type: u32) -> Option<u32> {
    match src_type as u8 {
        LE_SRC_OFF32 => Some(4),
        LE_SRC_SELFREL32 => Some(4),
        LE_SRC_OFF16 => Some(2),
        LE_SRC_SEL16 => Some(2),
        LE_SRC_BYTE => Some(1),
        LE_SRC_PTR16_16 => Some(4),
        LE_SRC_PTR16_32 => Some(6),
        _ => None,
    }
}

/// Full fixup census over every page: this is what sizes the fixup engine.
///
/// # Safety
/// `file`/`len` as in le_parse_rs; `img` parsed; `hist` a writable LeHist.
#[no_mangle]
pub unsafe extern "C" fn le_fixup_hist_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    hist: *mut LeHist,
) -> i32 {
    if file.is_null() || img.is_null() || hist.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    core::ptr::write_bytes(hist as *mut u8, 0, core::mem::size_of::<LeHist>());
    let f = core::slice::from_raw_parts(file, len as usize);
    let im = &*img;
    let h = &mut *hist;

    let mut page = 0u32;
    while page < im.num_pages {
        // page flag census, from the same reader the loader uses
        if let Ok((num, fl)) = page_entry(f, im, page) {
            h.by_page_flag[(fl & 7) as usize] += 1;
            if num == 0 {
                h.pages_no_data += 1;
            }
            h.last_page_num = num;
        }

        let before_recs = h.records;
        let before_srcs = h.sources;
        let mut last_rec: u32 = u32::MAX;
        {
            let mut cb = |fx: &LeFixup| -> Result<(), i32> {
                if fx.rec_off != last_rec {
                    last_rec = fx.rec_off;
                    h.records += 1;
                    h.by_rec_len[if fx.rec_len >= 15 { 15 } else { fx.rec_len as usize }] += 1;
                    if (fx.src as u8 & LE_SRC_LIST) != 0 {
                        h.src_list_recs += 1;
                    }
                    if (fx.flags as u8 & LE_TGT_ADDITIVE) != 0 {
                        h.additive += 1;
                    }
                    if (fx.flags as u8 & LE_TGT_OFF32) != 0 {
                        h.tgt_off32 += 1;
                    } else {
                        h.tgt_off16 += 1;
                    }
                    if (fx.flags as u8 & LE_TGT_ORD16) != 0 {
                        h.ord16 += 1;
                    }
                }
                h.sources += 1;
                h.by_src[(fx.src_type & 0x0f) as usize] += 1;
                h.by_tgt[(fx.tgt_type & 0x03) as usize] += 1;
                if fx.src_off < 0 {
                    h.neg_src_off += 1;
                } else if let Some(w) = src_width(fx.src_type) {
                    if fx.src_off as u32 + w > im.page_size {
                        h.straddling += 1;
                    }
                }
                Ok(())
            };
            let e = walk_page_fixups(f, im, page, &mut cb);
            if let Err(e) = e {
                return e;
            }
        }
        if h.records > before_recs {
            h.pages_with_fixups += 1;
        }
        let n = h.sources - before_srcs;
        if n > h.max_page_sources {
            h.max_page_sources = n;
        }
        page += 1;
    }
    LE_OK
}

/// Decode the `index`'th expanded fixup source of `page`. O(index) per call:
/// this is a diagnostic accessor for printing a handful of records, not the
/// engine. Returns LE_E_PAGE_NUM when `index` is past the end.
///
/// # Safety
/// `file`/`len` as in le_parse_rs; `img` parsed; `out` a writable LeFixup.
#[no_mangle]
pub unsafe extern "C" fn le_fixup_at_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    page: u32,
    index: u32,
    out: *mut LeFixup,
) -> i32 {
    if file.is_null() || img.is_null() || out.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    let f = core::slice::from_raw_parts(file, len as usize);
    let im = &*img;
    let mut seen = 0u32;
    let mut got = false;
    {
        // Sentinel error used only to stop the walk early; never returned.
        const STOP: i32 = -1;
        let mut cb = |fx: &LeFixup| -> Result<(), i32> {
            if seen == index {
                // SAFETY: `out` was null-checked above and is a caller-owned,
                // properly aligned LeFixup. Explicit block because the implicit
                // unsafe of an `unsafe fn` body is not something to rely on
                // inside a closure.
                unsafe {
                    *out = *fx;
                }
                got = true;
                return Err(STOP);
            }
            seen += 1;
            Ok(())
        };
        match walk_page_fixups(f, im, page, &mut cb) {
            Ok(()) => {}
            Err(e) if e == STOP => {}
            Err(e) => return e,
        }
    }
    if got {
        LE_OK
    } else {
        LE_E_PAGE_NUM
    }
}

/// Number of fixup RECORD BYTES on a page, and the raw table offsets, so the C
/// side can print the fixup page table exactly as it appears on disk.
///
/// # Safety
/// `file`/`len` as in le_parse_rs; `img` parsed; `s0`/`s1` writable u32s.
#[no_mangle]
pub unsafe extern "C" fn le_fixup_page_range_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    page: u32,
    s0: *mut u32,
    s1: *mut u32,
) -> i32 {
    if file.is_null() || img.is_null() || s0.is_null() || s1.is_null() {
        return LE_E_TRUNCATED;
    }
    // SAFETY: caller contract above.
    let f = core::slice::from_raw_parts(file, len as usize);
    match fixup_page_range(f, &*img, page) {
        Ok((a, b)) => {
            *s0 = a;
            *s1 = b;
            LE_OK
        }
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// relocation
// ---------------------------------------------------------------------------

/// Slide every object by `delta` before loading.
///
/// NOT optional for a real game, and this is a measurement rather than a
/// preference. Every one of DOOM / DUKE3D / SW has an object whose range at the
/// linker's base covers the VGA aperture at 0xA0000-0xC0000, AND contains
/// unrelocated literal 0x000A0000 immediates in code (DOOM 6, DUKE3D 28, SW 18)
/// that are NOT fixup sources, i.e. absolute addresses the game expects to be
/// the framebuffer. Both cannot be true at the linker's base, so the extender
/// slides the module above the low megabyte and so must we.
///
/// Fixups need no special handling: internal targets are computed from
/// obj[].reloc_base, so sliding the objects slides every target with them.
///
/// # Safety
/// `img` must be a parsed, writable LeImage.
#[no_mangle]
pub unsafe extern "C" fn le_relocate_rs(img: *mut LeImage, delta: u32) -> i32 {
    if img.is_null() {
        return LE_E_MEM;
    }
    // SAFETY: caller contract above.
    let im = &mut *img;
    if im.page_size == 0 || (delta & (im.page_size - 1)) != 0 {
        return LE_E_MEM;
    }
    let mut i = 0usize;
    while i < im.num_objects as usize {
        if im.obj[i].reloc_base as u64 + delta as u64 + im.obj[i].virt_size as u64 > 0xffff_ffff {
            return LE_E_OVERFLOW;
        }
        i += 1;
    }
    let mut i = 0usize;
    while i < im.num_objects as usize {
        im.obj[i].reloc_base += delta;
        i += 1;
    }
    im.lin_lo += delta;
    im.lin_hi += delta;
    im.reloc_delta = im.reloc_delta.wrapping_add(delta);
    LE_OK
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

/// Sub-slice of the caller's buffer covering guest linear [lin, lin+n).
fn mem_range(mem: &mut [u8], mem_base: u32, lin: u32, n: u32) -> Option<&mut [u8]> {
    if lin < mem_base {
        return None;
    }
    let off = lin as u64 - mem_base as u64;
    let sz = mem.len() as u64;
    if n as u64 > sz || off > sz - n as u64 {
        return None;
    }
    mem.get_mut(off as usize..(off + n as u64) as usize)
}

fn apply_one(
    mem: &mut [u8],
    mem_base: u32,
    img: &LeImage,
    fx: &LeFixup,
    st: &mut LeStats,
) -> Result<(), i32> {
    let page_lin = page_linear(img, fx.page)?;
    // SIGNED addition: the continuation entry of a straddling fixup is negative
    // and names the same linear address as its partner on the previous page.
    let src_lin = (page_lin as i64 + fx.src_off as i64) as u32;
    if fx.src_off < 0 {
        st.fixups_negative_off += 1;
    }

    if fx.tgt_obj == 0 || fx.tgt_obj > img.num_objects {
        return Err(LE_E_FIXUP_TARGET);
    }
    let ob = &img.obj[fx.tgt_obj as usize - 1];
    let tgt_lin = ob.reloc_base.wrapping_add(fx.tgt_off);
    if fx.tgt_off > ob.virt_size {
        st.fixups_off_object += 1;
    }

    let width = src_width(fx.src_type).ok_or(LE_E_SRC_TYPE)?;
    if fx.src_off >= 0 && fx.src_off as u32 + width > img.page_size {
        st.fixups_straddling += 1;
    }

    let p = mem_range(mem, mem_base, src_lin, width).ok_or(LE_E_MEM)?;
    match fx.src_type as u8 {
        LE_SRC_OFF32 => p[0..4].copy_from_slice(&tgt_lin.to_le_bytes()),
        LE_SRC_SELFREL32 => {
            // value = target - (source + 4). MEASURED: 12 of 12 self-relative
            // fixups across DUKE3D and SW sit immediately after an E8 (CALL
            // rel32) or E9 (JMP rel32) opcode byte, so src_off names the rel32
            // displacement and the reference point is the end of the
            // instruction.
            let v = tgt_lin.wrapping_sub(src_lin.wrapping_add(4));
            p[0..4].copy_from_slice(&v.to_le_bytes());
        }
        LE_SRC_OFF16 => p[0..2].copy_from_slice(&(fx.tgt_off as u16).to_le_bytes()),
        LE_SRC_SEL16 => {
            // Flat model: one code and one data selector, both base 0. The
            // emulator picks the values; 0 here means "caller patches".
            p[0] = 0;
            p[1] = 0;
        }
        LE_SRC_BYTE => p[0] = fx.tgt_off as u8,
        LE_SRC_PTR16_16 => {
            p[0..2].copy_from_slice(&(fx.tgt_off as u16).to_le_bytes());
            p[2] = 0;
            p[3] = 0;
        }
        LE_SRC_PTR16_32 => {
            p[0..4].copy_from_slice(&tgt_lin.to_le_bytes());
            p[4] = 0;
            p[5] = 0;
        }
        _ => return Err(LE_E_SRC_TYPE),
    }
    st.fixups_applied += 1;
    st.fixups_by_src[(fx.src_type & 0x0f) as usize] += 1;
    Ok(())
}

fn load(
    f: &[u8],
    img: &LeImage,
    mem: &mut [u8],
    mem_base: u32,
    st: &mut LeStats,
) -> Result<(), i32> {
    if mem_base > img.lin_lo {
        return Err(LE_E_MEM);
    }
    if mem_base as u64 + (mem.len() as u64) < img.lin_hi as u64 {
        return Err(LE_E_MEM);
    }

    // Zero the WHOLE linear span first. virt_size is much larger than the file
    // pages: DOOM's object 3 has 38 file pages (155,648 bytes) and a virt_size
    // of 548,368, so 392,720 bytes of it are .bss that no page ever covers.
    {
        let n = img.lin_hi - img.lin_lo;
        let z = mem_range(mem, mem_base, img.lin_lo, n).ok_or(LE_E_MEM)?;
        for b in z.iter_mut() {
            *b = 0;
        }
        st.bytes_zeroed = n;
    }

    let mut i = 0u32;
    while i < img.num_pages {
        let (foff, flen) = page_file_off(f, img, i)?;
        let lin = page_linear(img, i)?;
        if flen == 0 {
            st.pages_zeroed += 1;
            i += 1;
            continue;
        }
        let src = f
            .get(foff as usize..(foff as u64 + flen as u64) as usize)
            .ok_or(LE_E_PAGE_DATA)?;
        let dst = mem_range(mem, mem_base, lin, flen).ok_or(LE_E_MEM)?;
        dst.copy_from_slice(src);
        st.pages_copied += 1;
        st.bytes_copied += flen;
        if flen != img.page_size {
            st.pages_short += 1;
        }
        i += 1;
    }

    let mut i = 0u32;
    while i < img.num_pages {
        let mut err: i32 = LE_OK;
        {
            let mut cb = |fx: &LeFixup| -> Result<(), i32> { apply_one(mem, mem_base, img, fx, st) };
            if let Err(e) = walk_page_fixups(f, img, i, &mut cb) {
                err = e;
            }
        }
        if err != LE_OK {
            return Err(err);
        }
        i += 1;
    }
    Ok(())
}

/// Materialise the module into a flat buffer representing guest linear
/// [mem_base_lin, mem_base_lin + mem_size), then apply every fixup.
///
/// # Safety
/// `file`/`len` as in le_parse_rs. `mem` must point to `mem_size` contiguous
/// writable bytes owned by the caller; nothing outside that extent is written.
/// `img` must be a parsed image and `st` a writable LeStats.
#[no_mangle]
pub unsafe extern "C" fn le_load_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    mem: *mut u8,
    mem_size: u32,
    mem_base_lin: u32,
    st: *mut LeStats,
) -> i32 {
    if file.is_null() || img.is_null() || mem.is_null() || st.is_null() {
        return LE_E_MEM;
    }
    // SAFETY: caller contract above. Both slices span exactly the extents the
    // caller declared, so every read and write below is bounds-checked.
    core::ptr::write_bytes(st as *mut u8, 0, core::mem::size_of::<LeStats>());
    let f = core::slice::from_raw_parts(file, len as usize);
    let m = core::slice::from_raw_parts_mut(mem, mem_size as usize);
    match load(f, &*img, m, mem_base_lin, &mut *st) {
        Ok(()) => LE_OK,
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// the post-load invariant (docs/DOS4GW_LE_FORMAT.md section 4.5)
// ---------------------------------------------------------------------------

/// Read back every applied 32-bit-offset fixup and check that the value it
/// wrote points inside a declared object.
///
/// This is ONE cheap check that fails if the page base, the page numbering, the
/// record decode or the signed src_off is wrong, because each of those scatters
/// the targets. Measured on the reference implementation: DOOM 12,103 of
/// 12,103 inside, DUKE3D 27,302 of 27,302, SW 46,680 of 46,680.
///
/// The boundary is INCLUSIVE at the top: DOOM's last fixup targets object 2 at
/// offset 0x19, exactly one past the end of an object that is 0x19 bytes long.
/// That is a legal end-of-segment symbol, not a bug.
///
/// # Safety
/// As le_load_rs, but `mem` is only read.
#[no_mangle]
pub unsafe extern "C" fn le_validate_rs(
    file: *const u8,
    len: u32,
    img: *const LeImage,
    mem: *const u8,
    mem_size: u32,
    mem_base_lin: u32,
    out: *mut LeValid,
) -> i32 {
    if file.is_null() || img.is_null() || mem.is_null() || out.is_null() {
        return LE_E_MEM;
    }
    // SAFETY: caller contract above.
    core::ptr::write_bytes(out as *mut u8, 0, core::mem::size_of::<LeValid>());
    let f = core::slice::from_raw_parts(file, len as usize);
    let m = core::slice::from_raw_parts(mem, mem_size as usize);
    let im = &*img;
    let v = &mut *out;

    let mut page = 0u32;
    while page < im.num_pages {
        let page_lin = match page_linear(im, page) {
            Ok(x) => x,
            Err(e) => return e,
        };
        let mut err: i32 = LE_OK;
        {
            let mut cb = |fx: &LeFixup| -> Result<(), i32> {
                if fx.src_type as u8 != LE_SRC_OFF32 {
                    return Ok(());
                }
                let src_lin = (page_lin as i64 + fx.src_off as i64) as u32;
                if src_lin < mem_base_lin {
                    v.unreadable += 1;
                    return Ok(());
                }
                let off = (src_lin - mem_base_lin) as u64;
                if off + 4 > m.len() as u64 {
                    v.unreadable += 1;
                    return Ok(());
                }
                let b = match m.get(off as usize..off as usize + 4) {
                    Some(x) => x,
                    None => {
                        v.unreadable += 1;
                        return Ok(());
                    }
                };
                let val = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
                v.checked += 1;
                let mut inside = false;
                let mut i = 0usize;
                while i < im.num_objects as usize {
                    let o = &im.obj[i];
                    // Inclusive top boundary: see the doc comment.
                    if val >= o.reloc_base && val <= o.reloc_base.wrapping_add(o.virt_size) {
                        inside = true;
                        break;
                    }
                    i += 1;
                }
                if inside {
                    v.inside_object += 1;
                } else {
                    if v.outside == 0 {
                        v.first_bad_lin = src_lin;
                        v.first_bad_val = val;
                    }
                    v.outside += 1;
                }
                Ok(())
            };
            if let Err(e) = walk_page_fixups(f, im, page, &mut cb) {
                err = e;
            }
        }
        if err != LE_OK {
            return err;
        }
        page += 1;
    }
    LE_OK
}

// ---------------------------------------------------------------------------
// struct size reporters. The C side _Static_asserts its own sizeof against
// these constants at COMPILE time; these functions let a boot-time check prove
// the two languages agree at RUN time on the build that actually shipped.
// ---------------------------------------------------------------------------

/// sizeof(LeImage) as Rust lays it out.
#[no_mangle]
pub extern "C" fn le_sizeof_image_rs() -> u32 {
    core::mem::size_of::<LeImage>() as u32
}

/// sizeof(LeFixup) as Rust lays it out.
#[no_mangle]
pub extern "C" fn le_sizeof_fixup_rs() -> u32 {
    core::mem::size_of::<LeFixup>() as u32
}

/// sizeof(LeHist) as Rust lays it out.
#[no_mangle]
pub extern "C" fn le_sizeof_hist_rs() -> u32 {
    core::mem::size_of::<LeHist>() as u32
}

/// sizeof(LeStats) as Rust lays it out.
#[no_mangle]
pub extern "C" fn le_sizeof_stats_rs() -> u32 {
    core::mem::size_of::<LeStats>() as u32
}

/// sizeof(LeValid) as Rust lays it out.
#[no_mangle]
pub extern "C" fn le_sizeof_valid_rs() -> u32 {
    core::mem::size_of::<LeValid>() as u32
}

// ---------------------------------------------------------------------------
// self-test fixtures
//
// The three facts below were each settled the hard way, and two of them are
// INVISIBLE on a small or well-formed corpus:
//
//   * a module of 255 pages or fewer cannot tell big- from little-endian page
//     numbers apart, so the fixture asserts index 256, not index 5;
//   * a loader that reads src_off unsigned writes ~64 KiB away and corrupts
//     the image with no error anywhere;
//   * a loader that anchors on the FIRST MZ produces a plausible module whose
//     every cross-reference misses (blame.md, 2026-08-07).
//
// docs/DOS4GW_LE_FORMAT.md section 3 says the synthetic fixture belongs in the
// self-test, not the corpus, because "the corpus will be small again next time
// somebody adds a format". This is that fixture, in the kernel, running on
// every boot that runs the LE self-test, so it cannot regress silently.
// ---------------------------------------------------------------------------

fn wr(b: &mut [u8], o: usize, v: &[u8]) -> bool {
    match b.get_mut(o..o + v.len()) {
        Some(d) => {
            d.copy_from_slice(v);
            true
        }
        None => false,
    }
}

/// Returns 0 on pass, or a negative code naming the check that failed.
/// Needs at least 16 KiB of scratch.
///
/// # Safety
/// `buf` must point to `buflen` contiguous writable bytes owned by the caller.
/// The buffer is fully overwritten and is not read after return.
#[no_mangle]
pub unsafe extern "C" fn le_selftest_rs(buf: *mut u8, buflen: u32) -> i32 {
    if buf.is_null() || buflen < 0x4000 {
        return -1;
    }
    // SAFETY: caller contract above.
    let b = core::slice::from_raw_parts_mut(buf, buflen as usize);

    // --- check 1: le_find takes the LAST MZ, not the first -----------------
    for x in b.iter_mut() {
        *x = 0;
    }
    // Outer stub MZ at 0, with the DOOM.EXE-shaped garbage e_lfanew that is
    // not an e_lfanew at all.
    if !wr(b, 0, b"MZ") {
        return -2;
    }
    if !wr(b, 0x3c, &0x09b4_0000u32.to_le_bytes()) {
        return -2;
    }
    // Inner (anchor) MZ at 0x100 whose e_lfanew lands on a real "LE".
    if !wr(b, 0x100, b"MZ") {
        return -2;
    }
    if !wr(b, 0x13c, &0x200u32.to_le_bytes()) {
        return -2;
    }
    if !wr(b, 0x300, b"LE") {
        return -2;
    }
    match find(b) {
        Ok((mz, le)) => {
            if mz != 0x100 || le != 0x300 {
                return -3; // took the wrong MZ: the trap that eats loaders
            }
        }
        Err(_) => return -4,
    }

    // --- check 2: page numbers are 24-bit BIG-ENDIAN, 1-based --------------
    for x in b.iter_mut() {
        *x = 0;
    }
    let mut img = LeImage {
        mz_off: 0,
        le_off: 0,
        cpu_type: 0,
        os_type: 0,
        mod_flags: 0,
        num_pages: 300,
        page_size: 4096,
        last_page_size: 4096,
        eip_obj: 1,
        eip: 0,
        esp_obj: 0,
        esp: 0,
        num_objects: 1,
        obj_tab_off: 0,
        page_map_off: 0x1000,
        fixup_page_tab_off: 0x2000,
        fixup_rec_tab_off: 0x3000,
        import_mod_tab_off: 0,
        import_proc_tab_off: 0,
        num_import_mod: 0,
        fixup_sect_size: 0,
        loader_sect_size: 0,
        page_data_abs: 0,
        page_data_len: 0,
        lin_lo: 0x10000,
        lin_hi: 0x20000,
        reloc_delta: 0,
        _pad: 0,
        obj: [LeObject {
            virt_size: 0x10000,
            reloc_base: 0x10000,
            flags: 0,
            page_index: 1,
            page_count: 300,
            reserved: 0,
        }; LE_MAX_OBJECTS],
    };
    let mut i = 0usize;
    while i < 300 {
        let n = (i + 1) as u32; // 1-based page numbers
        if !wr(
            b,
            0x1000 + i * 4,
            &[(n >> 16) as u8, (n >> 8) as u8, n as u8, 0u8],
        ) {
            return -5;
        }
        i += 1;
    }
    // Index 255 is the last entry the two readings agree on; index 256 is the
    // byte that settles it. Under a little-endian high half it decodes 65536.
    match page_entry(b, &img, 254) {
        Ok((n, f)) => {
            if n != 255 || f != 0 {
                return -6;
            }
        }
        Err(_) => return -6,
    }
    match page_entry(b, &img, 255) {
        Ok((n, _)) => {
            if n != 256 {
                return -7; // THE endianness check
            }
        }
        Err(_) => return -7,
    }
    match page_entry(b, &img, 299) {
        Ok((n, _)) => {
            if n != 300 {
                return -8;
            }
        }
        Err(_) => return -8,
    }
    // Out-of-range page is an ERROR, not a read past the table.
    if page_entry(b, &img, 300).is_ok() {
        return -9;
    }

    // --- check 3: src_off is SIGNED, and the record length comes from the
    //     flag bits, not from a constant ----------------------------------
    img.num_pages = 2;
    img.obj[0].page_count = 2;
    // fixup page table: page 0 owns record bytes [0,7), page 1 owns none.
    if !wr(b, 0x2000, &0u32.to_le_bytes()) {
        return -10;
    }
    if !wr(b, 0x2004, &7u32.to_le_bytes()) {
        return -10;
    }
    if !wr(b, 0x2008, &7u32.to_le_bytes()) {
        return -10;
    }
    // one 7-byte record: src=0x07 (32-bit offset), flags=0x00 (internal,
    // 16-bit target offset), src_off = 0xFFFC = -4, obj = 1, tgt_off = 0x1234
    if !wr(b, 0x3000, &[0x07, 0x00, 0xFC, 0xFF, 0x01, 0x34, 0x12]) {
        return -10;
    }
    let mut seen = 0u32;
    let mut ok = true;
    {
        let mut cb = |fx: &LeFixup| -> Result<(), i32> {
            seen += 1;
            if fx.src_off != -4 {
                ok = false; // read unsigned this would be 65532
            }
            if fx.tgt_obj != 1 || fx.tgt_off != 0x1234 || fx.rec_len != 7 {
                ok = false;
            }
            Ok(())
        };
        if walk_page_fixups(b, &img, 0, &mut cb).is_err() {
            return -11;
        }
    }
    if seen != 1 || !ok {
        return -12;
    }

    // A 9-byte record (flag 0x10 = 32-bit target offset) must decode as 9, in
    // the same file. Keying off a fixed record size is the documented way to
    // get this silently wrong.
    if !wr(b, 0x2004, &9u32.to_le_bytes()) {
        return -13;
    }
    if !wr(b, 0x2008, &9u32.to_le_bytes()) {
        return -13;
    }
    if !wr(
        b,
        0x3000,
        &[0x07, 0x10, 0x08, 0x00, 0x01, 0x78, 0x56, 0x34, 0x12],
    ) {
        return -13;
    }
    let mut ok9 = false;
    {
        let mut cb = |fx: &LeFixup| -> Result<(), i32> {
            if fx.rec_len == 9 && fx.src_off == 8 && fx.tgt_off == 0x1234_5678 {
                ok9 = true;
            }
            Ok(())
        };
        if walk_page_fixups(b, &img, 0, &mut cb).is_err() {
            return -14;
        }
    }
    if !ok9 {
        return -15;
    }

    // --- check 4: a truncated record is REJECTED, not read past ------------
    if !wr(b, 0x2004, &6u32.to_le_bytes()) {
        return -16;
    }
    if !wr(b, 0x2008, &6u32.to_le_bytes()) {
        return -16;
    }
    {
        let mut cb = |_fx: &LeFixup| -> Result<(), i32> { Ok(()) };
        if walk_page_fixups(b, &img, 0, &mut cb).is_ok() {
            return -17; // a 6-byte window cannot hold that 9-byte record
        }
    }

    // --- check 5: a wholly malformed buffer parses to an ERROR, and does so
    //     without touching the panic handler (which would halt the CPU) -----
    for x in b.iter_mut() {
        *x = 0xff;
    }
    let mut img2 = LeImage {
        mz_off: 0,
        le_off: 0,
        cpu_type: 0,
        os_type: 0,
        mod_flags: 0,
        num_pages: 0,
        page_size: 0,
        last_page_size: 0,
        eip_obj: 0,
        eip: 0,
        esp_obj: 0,
        esp: 0,
        num_objects: 0,
        obj_tab_off: 0,
        page_map_off: 0,
        fixup_page_tab_off: 0,
        fixup_rec_tab_off: 0,
        import_mod_tab_off: 0,
        import_proc_tab_off: 0,
        num_import_mod: 0,
        fixup_sect_size: 0,
        loader_sect_size: 0,
        page_data_abs: 0,
        page_data_len: 0,
        lin_lo: 0,
        lin_hi: 0,
        reloc_delta: 0,
        _pad: 0,
        obj: [LeObject {
            virt_size: 0,
            reloc_base: 0,
            flags: 0,
            page_index: 0,
            page_count: 0,
            reserved: 0,
        }; LE_MAX_OBJECTS],
    };
    if parse(b, &mut img2).is_ok() {
        return -18;
    }
    // ... and so does every 16-byte-stepped prefix of it. This is a WEAK
    // sweep and is labelled as one: the buffer is all-0xFF, so it exercises
    // the reject path and the absence of an out-of-bounds read, NOT the
    // accept path. The real truncation sweep (33,322 prefixes of a valid
    // file, plus 12,000 mutation-fuzz iterations under ASan) lives on the
    // host in tools/le-harness, where a sanitiser can actually watch.
    let mut n = 0usize;
    while n < 0x400 {
        let sub = match b.get(0..n) {
            Some(s) => s,
            None => return -19,
        };
        if parse(sub, &mut img2).is_ok() {
            return -20;
        }
        n += 16;
    }

    0
}
