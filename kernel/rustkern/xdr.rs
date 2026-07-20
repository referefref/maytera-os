// rustkern/xdr.rs - #404 batch-3 XDR decode primitives (net/rpc.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 batch-3 (LAST parser-tier seam): net/rpc.c XDR DECODE primitives. The
// bounded XDR decode cursor + typed reads that pull u32/u64/opaque/string/fh3/
// skip out of untrusted XDR-encoded RPC/NFS server replies (#317). Every
// length/cursor advance uses checked_add; the 4-byte-align round-up is overflow-
// checked; every opaque/string source read is a slice bounded to exactly `size`.
// Defense-in-depth (source-bounded on 64-bit; NO reachable OOB). The ENCODE
// branch stays C. Routed live under -DRUST_XDR.
// NOTE: the source-bounded XDR seam does NOT confine the SEPARATE net/nfs.c
// destination over-write (MAYTERA-SEC-2026-0012); that fix lives in nfs.c.
// ===========================================================================
#[repr(C)]
pub struct XdrT {
    data: *mut u8,  // off 0
    size: usize,    // off 8
    pos: usize,     // off 16
    encoding: bool, // off 24
    error: bool,    // off 25  (struct padded to 32, align 8)
}

const _: () = {
    assert!(core::mem::size_of::<XdrT>() == 32);
    assert!(core::mem::align_of::<XdrT>() == 8);
    assert!(core::mem::offset_of!(XdrT, data) == 0);
    assert!(core::mem::offset_of!(XdrT, size) == 8);
    assert!(core::mem::offset_of!(XdrT, pos) == 16);
    assert!(core::mem::offset_of!(XdrT, encoding) == 24);
    assert!(core::mem::offset_of!(XdrT, error) == 25);
};

/// XDR aligns opaque/string bodies up to a 4-byte boundary: (len + 3) & ~3.
/// The `+3` is checked so a near-SIZE_MAX length cannot wrap the round-up down.
#[inline]
fn xdr_aligned_len(len: usize) -> Option<usize> {
    Some(len.checked_add(3)? & !3usize)
}

/// Decode a 4-byte big-endian unsigned int and advance 4.
#[no_mangle]
pub extern "C" fn xdr_decode_uint32_rs(xdr: *mut XdrT, val: *mut u32) -> bool {
    // SAFETY: `xdr` is a valid writable XdrT* from the C caller. POD reads.
    let (data, size, pos, err) = unsafe { ((*xdr).data, (*xdr).size, (*xdr).pos, (*xdr).error) };
    if err {
        return false;
    }
    let end = match pos.checked_add(4) {
        Some(e) => e,
        None => {
            unsafe {
                (*xdr).error = true;
            }
            return false;
        }
    };
    if end > size {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    // SAFETY: `data` points to at least `size` readable bytes; [pos, end) is
    // proven in-bounds (end <= size), so every index below is bounds-checked.
    let src = unsafe { core::slice::from_raw_parts(data as *const u8, size) };
    let b = &src[pos..end];
    let v = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
    // SAFETY: `val` is a valid writable u32* per the caller contract.
    unsafe {
        *val = v;
        (*xdr).pos = end;
    }
    true
}

/// Decode a 64-bit unsigned int as two big-endian 32-bit halves (high then low).
#[no_mangle]
pub extern "C" fn xdr_decode_uint64_rs(xdr: *mut XdrT, val: *mut u64) -> bool {
    let mut high: u32 = 0;
    let mut low: u32 = 0;
    if !xdr_decode_uint32_rs(xdr, &mut high) {
        return false;
    }
    if !xdr_decode_uint32_rs(xdr, &mut low) {
        return false;
    }
    // SAFETY: `val` valid writable u64* per caller contract.
    unsafe {
        *val = ((high as u64) << 32) | (low as u64);
    }
    true
}

/// Fixed-length opaque: copy exactly `len` bytes, advancing by the aligned len.
/// SOURCE reads are bounds-checked against `size`; the align round-up + cursor
/// advance are overflow-checked. DESTINATION size is the caller's contract.
#[no_mangle]
pub extern "C" fn xdr_decode_opaque_rs(xdr: *mut XdrT, dst: *mut u8, len: usize) -> bool {
    let (data, size, pos, err) = unsafe { ((*xdr).data, (*xdr).size, (*xdr).pos, (*xdr).error) };
    if err {
        return false;
    }
    let aligned = match xdr_aligned_len(len) {
        Some(a) => a,
        None => {
            unsafe {
                (*xdr).error = true;
            }
            return false;
        }
    };
    let end = match pos.checked_add(aligned) {
        Some(e) => e,
        None => {
            unsafe {
                (*xdr).error = true;
            }
            return false;
        }
    };
    if end > size {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    if len > 0 {
        // SAFETY: source slice spans exactly `size` readable bytes; [pos, pos+len)
        // is proven in-bounds (pos+len <= end <= size). `dst` is a caller buffer
        // of >= `len` bytes (same contract the C memcpy relies on).
        let src = unsafe { core::slice::from_raw_parts(data as *const u8, size) };
        let s = &src[pos..pos + len];
        let d = unsafe { core::slice::from_raw_parts_mut(dst, len) };
        d.copy_from_slice(s);
    }
    unsafe {
        (*xdr).pos = end;
    }
    true
}

/// Variable-length opaque with a 4-byte length prefix, bounded by `maxlen`.
#[no_mangle]
pub extern "C" fn xdr_decode_bytes_rs(
    xdr: *mut XdrT,
    data: *mut *mut u8,
    len: *mut u32,
    maxlen: u32,
) -> bool {
    if unsafe { (*xdr).error } {
        return false;
    }
    let mut l: u32 = 0;
    if !xdr_decode_uint32_rs(xdr, &mut l) {
        return false;
    }
    unsafe {
        *len = l;
    }
    if l > maxlen {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    let dptr = unsafe { *data };
    if dptr.is_null() {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    xdr_decode_opaque_rs(xdr, dptr, l as usize)
}

/// NUL-terminated string with a 4-byte length prefix; wire length must be < maxlen.
#[no_mangle]
pub extern "C" fn xdr_decode_string_rs(xdr: *mut XdrT, str_: *mut u8, maxlen: u32) -> bool {
    if unsafe { (*xdr).error } {
        return false;
    }
    let mut len: u32 = 0;
    if !xdr_decode_uint32_rs(xdr, &mut len) {
        return false;
    }
    if len >= maxlen {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    if !xdr_decode_opaque_rs(xdr, str_, len as usize) {
        return false;
    }
    // SAFETY: `str_` is a caller buffer of >= maxlen bytes; index len < maxlen.
    unsafe {
        *str_.add(len as usize) = 0u8;
    }
    true
}

/// Like xdr_string but the wire length is returned via `len`.
#[no_mangle]
pub extern "C" fn xdr_decode_string_len_rs(
    xdr: *mut XdrT,
    str_: *mut u8,
    len: *mut u32,
    maxlen: u32,
) -> bool {
    if unsafe { (*xdr).error } {
        return false;
    }
    let mut l: u32 = 0;
    if !xdr_decode_uint32_rs(xdr, &mut l) {
        return false;
    }
    unsafe {
        *len = l;
    }
    if l >= maxlen {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    if !xdr_decode_opaque_rs(xdr, str_, l as usize) {
        return false;
    }
    // SAFETY: `str_` is a caller buffer of >= maxlen bytes; index l < maxlen.
    unsafe {
        *str_.add(l as usize) = 0u8;
    }
    true
}

/// Skip a 4-byte-aligned opaque field without copying.
#[no_mangle]
pub extern "C" fn xdr_decode_skip_rs(xdr: *mut XdrT, len: usize) -> bool {
    let (size, pos, err) = unsafe { ((*xdr).size, (*xdr).pos, (*xdr).error) };
    if err {
        return false;
    }
    let aligned = match xdr_aligned_len(len) {
        Some(a) => a,
        None => {
            unsafe {
                (*xdr).error = true;
            }
            return false;
        }
    };
    let end = match pos.checked_add(aligned) {
        Some(e) => e,
        None => {
            unsafe {
                (*xdr).error = true;
            }
            return false;
        }
    };
    if end > size {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    unsafe {
        (*xdr).pos = end;
    }
    true
}

/// NFS3 file handle: 4-byte length prefix (rejected if > 64 = NFS3_FHSIZE) then
/// the opaque handle bytes.
#[no_mangle]
pub extern "C" fn xdr_decode_nfs_fh3_rs(xdr: *mut XdrT, fh_data: *mut u8, fh_len: *mut u32) -> bool {
    let mut l: u32 = 0;
    if !xdr_decode_uint32_rs(xdr, &mut l) {
        return false;
    }
    unsafe {
        *fh_len = l;
    }
    if l > 64 {
        unsafe {
            (*xdr).error = true;
        }
        return false;
    }
    xdr_decode_opaque_rs(xdr, fh_data, l as usize)
}
