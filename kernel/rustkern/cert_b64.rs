// rustkern/cert_b64.rs - #404 batch-2 PEM base64 -> DER decode (net/tls/cert_store.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 batch-2 seam 2/3: net/tls/cert_store.c PEM base64 -> DER decode
// (cert_base64_decode_rs), ported from base64_decode(). Untrusted input: the
// base64 body of a PEM CERTIFICATE served by a remote/store. Byte-for-byte
// identical output to base64_decode_c over well-formed AND malformed input, but
// every output write is Rust-bounds-checked against out_cap (the C form is
// ALREADY output-bounded by its `written < out_len` guard; the Rust keeps that
// AND makes the bound structural), and the 6-bit accumulator is a wrapping u32,
// removing the C form's signed-int left-shift UB (minor CWE-190 hardening). Only
// the low <=15 bits are ever observed, so wrapping u32 is byte-identical.
// ===========================================================================
#[no_mangle]
pub extern "C" fn cert_base64_decode_rs(
    in_ptr: *const u8,
    in_len: u32,
    out_ptr: *mut u8,
    out_cap: u32,
    out_len: *mut u32,
) -> i32 {
    // Null-guard the buffers. The C reference dereferences unconditionally; the
    // real caller (cert_parse_pem) always passes non-null in/out, so this guard
    // cannot change live behavior, it only hardens the FFI edge.
    if in_ptr.is_null() || out_ptr.is_null() {
        return -1;
    }
    let ilen = in_len as usize;
    let ocap = out_cap as usize;

    // SAFETY: the caller guarantees `in_ptr` covers at least `in_len` readable
    // bytes (cert_parse_pem passes the body span `finish - start`) and `out_ptr`
    // at least `out_cap` writable bytes (a kmalloc(der_max) buffer). We build
    // slices spanning EXACTLY those extents and index ONLY through them, so every
    // read of `input` and every write to `output` is bounds-checked.
    let input: &[u8] = unsafe { core::slice::from_raw_parts(in_ptr, ilen) };
    let output: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out_ptr, ocap) };

    let mut written: usize = 0;
    let mut acc: u32 = 0;
    let mut bits: i32 = 0;

    let mut i: usize = 0;
    // Loop guard mirrors C `for (i=0; i<in_len && written<out_len; i++)`.
    while i < ilen && written < ocap {
        let c = input[i];
        i += 1;

        if c == b'\n' || c == b'\r' || c == b' ' {
            continue; // C: skip CR/LF/space
        }
        if c == b'=' {
            break; // C: padding terminates the stream
        }

        let val: u32 = match c {
            b'A'..=b'Z' => (c - b'A') as u32,
            b'a'..=b'z' => (c - b'a') as u32 + 26,
            b'0'..=b'9' => (c - b'0') as u32 + 52,
            b'+' => 62,
            b'/' => 63,
            _ => continue, // val < 0 in C -> skip this char
        };

        // Wrapping accumulate: `acc << 6` on u32 drops high bits (defined, no UB);
        // low bits equal the C int accumulator's wrapped value.
        acc = (acc << 6) | val;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            output[written] = ((acc >> bits) & 0xFF) as u8;
            written += 1;
        }
    }

    // SAFETY: out_len is either null or a valid writable u32 (C passes &der_len).
    if !out_len.is_null() {
        unsafe { *out_len = written as u32; }
    }
    0
}
