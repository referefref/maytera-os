// rustkern/xattr.rs - #404 batch-2 / MAYTERA-SEC-2026-0011 on-disk xattr parse (fs/xattr.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 batch-2 seam 3/3 / MAYTERA-SEC-2026-0011: fs/xattr.c on-disk xattr
// entry-walk (xattr_entry_next_rs). Bounded iterator over the packed
// [entry_header | name | value] records in /.xattr/XXXXXXXX.xat. UNTRUSTED: a
// crafted/corrupt FAT image controls every name_len/value_len. The live C walk
// advances by these lengths with NO bounds check -> attr_name/attr_value point
// past the kmalloc'd buffer -> heap over-read (CWE-125) via getxattr/listxattr.
// This port removes that class BY CONSTRUCTION: the block is a slice of exactly
// `len`, and header/name/value extents are each checked_add'd and gated against
// `len` before the descriptor is returned. Returns 1=one entry, 0=clean end,
// -1=malformed/would-be-OOB (the C reference returns 1 with OOB offsets).
// ===========================================================================
#[repr(C)]
pub struct XattrEntry {
    pub name_off: u32,
    pub value_off: u32,
    pub value_len: u32,
    pub name_len: u16,
    pub namespace_id: u8,
    pub _pad: u8,
}
// Compile-time sizeof lock (== 16). Mirrors the C _Static_assert.
const _: () = assert!(core::mem::size_of::<XattrEntry>() == 16);

// sizeof(xattr_entry_header_t) on disk (packed): name_len(2)+value_len(4)+ns(1)+resv(1).
const XATTR_ENTRY_HDR_RS: usize = 8;

#[no_mangle]
pub extern "C" fn xattr_entry_next_rs(
    block: *const u8,
    len: u32,
    pos: *mut u32,
    out: *mut XattrEntry,
) -> i32 {
    if block.is_null() || pos.is_null() || out.is_null() {
        return -1;
    }
    let l = len as usize;
    // SAFETY: the caller (xattr_get/list) guarantees `block` points to at least
    // `len` contiguous readable bytes: it is (file_data +
    // sizeof(xattr_file_header_t)) into a kmalloc(file_size) buffer that
    // xattr_read_file filled from disk, and len = file_size - header, so the
    // slice spans exactly the entries region. We index ONLY through this slice.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(block, l) };

    // SAFETY: pos is non-null (checked) and points to a writable u32.
    let p = unsafe { *pos } as usize;

    // Mirrors the C `ptr < end` loop guard: nothing left to decode.
    if p >= l {
        return 0;
    }

    // Confine OOB #1: the 8-byte entry header must fit entirely.
    let hdr_end = match p.checked_add(XATTR_ENTRY_HDR_RS) {
        Some(v) => v,
        None => return -1,
    };
    if hdr_end > l {
        return -1;
    }

    // Read the packed header fields little-endian, exactly as the x86-64 C struct
    // read does. All indices are < hdr_end <= l, so bounds-checked and in range.
    let name_len = u16::from_le_bytes([s[p], s[p + 1]]);
    let value_len = u32::from_le_bytes([s[p + 2], s[p + 3], s[p + 4], s[p + 5]]);
    let namespace_id = s[p + 6];
    // s[p + 7] is the reserved/padding byte (unused, matching the C).

    // name_off == p + 8 (== hdr_end). Confine OOB #2: the name must fit.
    let name_off = hdr_end;
    let value_off = match name_off.checked_add(name_len as usize) {
        Some(v) => v,
        None => return -1,
    };
    if value_off > l {
        return -1;
    }

    // Confine OOB #3: the value must fit.
    let next = match value_off.checked_add(value_len as usize) {
        Some(v) => v,
        None => return -1,
    };
    if next > l {
        return -1;
    }

    // SAFETY: out is non-null (checked) and points to a writable XattrEntry (16
    // bytes, asserted). All derived offsets are <= l <= u32::MAX (len is u32).
    let o = unsafe { &mut *out };
    o.name_off = name_off as u32;
    o.value_off = value_off as u32;
    o.value_len = value_len;
    o.name_len = name_len;
    o.namespace_id = namespace_id;
    o._pad = 0;

    // SAFETY: pos is a writable u32; next <= l <= u32::MAX.
    unsafe {
        *pos = next as u32;
    }
    1
}
