// rustkern/tls_parse.rs - #404 Phase Y / #502 TLS record / handshake / certificate walkers
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// Phase Y port (#404 / #502): TLS record + handshake + certificate LENGTH-PARSE
// framing seam (net/tls/).
//
// TIER 1 (REMOTE-reachable, untrusted wire input). Every HTTPS connection
// (browser, the LLM/Kimi API, update checks, netinfo widget fetches, music
// metadata) parses attacker-influenced length fields straight off the wire in
// this layer. The classic TLS-parser OOB is trusting a length field. The seams
// ported here are the byte-FRAMING/length-BOUNDING of the record header, the
// handshake-message header walk, and the certificate-list walk. NONE of the
// crypto, AEAD, key schedule, transcript hashing, or X.509 DER parse crosses
// into Rust: those stay in C. Only the pure length arithmetic that gates a
// subsequent read/copy/index is confined here, with CHECKED arithmetic on every
// add/compare and a slice over exactly `len` bytes so every derived offset is
// bounds-checked by construction.
//
// SECURITY [RUST-SEC]: unlike the ARP/DNS/DHCP wire ports (whose C references
// were already length-gated), the ORIGINAL inline TLS 1.2 plaintext handshake
// loop in net/tls/tls.c (the ServerHello-flight parser, run for EVERY handshake
// including TLS 1.3 up to ServerHello) had a REMOTE-REACHABLE heap over-read:
//   `while (pos < length) { hs_type = data[pos];
//      hs_len = data[pos+1]<<16 | data[pos+2]<<8 | data[pos+3]; pos += 4;
//      ... process(data+pos, hs_len) ...; pos += hs_len; }`
// The loop guard `pos < length` only covers `data[pos]`, yet it then reads
// `data[pos+1..pos+3]` (up to 3 bytes past the kmalloc(length) record buffer
// when the body is 1..3 bytes) AND passes an UNBOUNDED 3-byte `hs_len` (up to
// 0x00FFFFFF) into tls_process_server_hello() as its `length`, whose internal
// bounds then compare against that attacker-declared length rather than the real
// remaining buffer, letting the ServerHello extension walk and the 32-byte
// key_share memcpy read far past the record allocation. A malicious or MITM
// server triggers this on its FIRST flight, before any certificate or crypto
// check. Routing the live loop through `tls_hs_next_rs` REMOVES the class by
// construction (both the header read and the body extent are checked-bounded to
// the slice). See task #503 (C-fallback hardening of the same two bounds so a
// -DRUST_TLS_PARSE rollback is still safe). A fold is warranted.

// tls_parse_record_header return codes (mirror the tls.c dispatcher).
const TLS_REC_OK: i32 = 0;
const TLS_REC_EINVAL: i32 = -2; // == TLS_ERR_INVALID_PARAM
// #497 fault 2: this bounds the CIPHERTEXT record body, not the plaintext, so
// it must be 2^14 + 2048 (RFC 5246 6.2.3), not 2^14. At 2^14 every full-size
// record was rejected (2^14 plaintext + 1 content-type + 16 GCM tag = 16401),
// which is what made every HTTPS response over ~16 KB fail. MUST stay equal to
// tls.h TLS_MAX_RECORD_SIZE or the boot [RUST-DIFF] tls_parse differential
// diverges (that differential is what would have caught a drift here).
const TLS_MAX_RECORD_SIZE_RS: u32 = 18432; // == tls.h TLS_MAX_RECORD_SIZE

// tls_hs_next / tls_cert_next / tls13_cert_next walk return codes.
pub(crate) const TLS_WALK_MSG: i32 = 1;  // a complete element was parsed; *out filled, *pos advanced
const TLS_WALK_END: i32 = 0;  // clean end: no complete element header remains
pub(crate) const TLS_WALK_BAD: i32 = -1; // malformed: a declared length overruns the buffer

// #[repr(C)] mirror of tls_record_hdr_t (net/tls/tls.h). Layout asserted == 6
// bytes on the C side (_Static_assert in tls.c). content_type at 0, reserved at
// 1, version (host order) at 2, length (record body length) at 4.
#[repr(C)]
pub struct TlsRecordHdr {
    pub content_type: u8, // 0
    pub reserved: u8,     // 1
    pub version: u16,     // 2: host order
    pub length: u16,      // 4: record body length
}

/// Rust port of the TLS record-header parse extracted from net/tls/tls.c
/// tls_recv_record(): given the 5-byte record header, decode content_type +
/// version + the 2-byte body length and bound that length <= TLS_MAX_RECORD_SIZE
/// (RFC 8446 caps a TLSCiphertext body at 2^14 + 256; this client, like the C it
/// replaces, uses the 2^14 plaintext cap). Returns TLS_REC_OK (0) with *out
/// filled, or TLS_REC_EINVAL (-2) if the header is short or the length exceeds
/// the cap. Byte-identical to the C reference on well-formed AND malformed input
/// (the C already bounded this one). Never reads the record body (which the C
/// then reads into a fresh kmalloc(length) after this returns).
/// # Safety: `hdr` must point to >= `len` readable bytes; `out` null or writable.
#[no_mangle]
pub extern "C" fn tls_parse_record_header_rs(hdr: *const u8, len: u32, out: *mut TlsRecordHdr) -> i32 {
    if hdr.is_null() || len < 5 {
        return TLS_REC_EINVAL;
    }
    // SAFETY: caller guarantees `hdr` spans >= `len` (>= 5) readable bytes; the
    // slice covers exactly `len` and every index below is in [0,5) < len, so all
    // reads are bounds-checked and cannot leave the slice. Read-only.
    let h: &[u8] = unsafe { core::slice::from_raw_parts(hdr, len as usize) };
    let content_type = h[0];
    let version = u16::from_be_bytes([h[1], h[2]]);
    let length = u16::from_be_bytes([h[3], h[4]]);
    if !out.is_null() {
        // SAFETY: `out` non-null (checked) and a valid writable TlsRecordHdr*
        // from the C caller. POD scalar writes.
        unsafe {
            let o = &mut *out;
            o.content_type = content_type;
            o.reserved = 0;
            o.version = version;
            o.length = length;
        }
    }
    if length as u32 > TLS_MAX_RECORD_SIZE_RS {
        return TLS_REC_EINVAL;
    }
    TLS_REC_OK
}

// #[repr(C)] mirror of tls_hs_msg_t (net/tls/tls.h). Layout asserted == 12 bytes
// on the C side. hs_type at 0, 3 bytes pad, hs_len (body length) at 4, body_off
// (offset of the body within the walked buffer) at 8.
#[repr(C)]
pub struct TlsHsMsg {
    pub hs_type: u8,    // 0
    pub _pad: [u8; 3],  // 1..3
    pub hs_len: u32,    // 4: 3-byte body length, decoded
    pub body_off: u32,  // 8: offset of the message body ( == old pos + 4 )
}

/// Rust port of the TLS handshake-message header walk. One step: at buffer
/// offset *pos, read the 1-byte msg_type + 3-byte length, bound the body to the
/// buffer, and (on success) fill *out and advance *pos past the whole message.
/// Returns TLS_WALK_MSG (1) with *out filled + *pos advanced, TLS_WALK_END (0)
/// when fewer than 4 header bytes remain (clean end of the coalesced messages in
/// this record), or TLS_WALK_BAD (-1) when the declared 3-byte length overruns
/// the buffer. Every add/compare is checked. This is the SAFE form the TLS 1.3
/// encrypted-handshake loop already used; routing the TLS 1.2 plaintext loop
/// through it too removes that loop's remote-reachable over-read (see header
/// note above). The caller still owns the transcript hashing, the per-type
/// dispatch, and reading the body at buf+body_off.
/// # Safety: `buf` >= `len` readable bytes; `pos`/`out` non-null writable.
#[no_mangle]
pub extern "C" fn tls_hs_next_rs(buf: *const u8, len: u32, pos: *mut u32, out: *mut TlsHsMsg) -> i32 {
    if buf.is_null() || pos.is_null() || out.is_null() {
        return TLS_WALK_BAD;
    }
    // SAFETY: `pos` is a valid writable u32 supplied by the C caller.
    let p: u32 = unsafe { *pos };
    // SAFETY: caller guarantees `buf` spans >= `len` readable bytes; slice covers
    // exactly `len`. All indexing below is bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // Need a complete 4-byte header: p + 4 <= len, using checked add so a near-
    // u32::MAX `p` can never wrap to appear in-bounds.
    let hdr_end = match p.checked_add(4) {
        Some(v) => v,
        None => return TLS_WALK_END,
    };
    if hdr_end > len {
        return TLS_WALK_END;
    }
    let pi = p as usize;
    let hs_type = b[pi];
    let hs_len = ((b[pi + 1] as u32) << 16) | ((b[pi + 2] as u32) << 8) | (b[pi + 3] as u32);

    // Body must fit: (p + 4) + hs_len <= len, checked.
    let body_end = match hdr_end.checked_add(hs_len) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if body_end > len {
        return TLS_WALK_BAD;
    }

    // SAFETY: `out`/`pos` non-null (checked), valid writable C-provided pointers.
    unsafe {
        let o = &mut *out;
        o.hs_type = hs_type;
        o._pad = [0, 0, 0];
        o.hs_len = hs_len;
        o.body_off = hdr_end; // == p + 4
        *pos = body_end;
    }
    TLS_WALK_MSG
}

// #[repr(C)] mirror of tls_cert_ent_t (net/tls/cert_store.h). Layout asserted ==
// 8 bytes. cert_off (offset of the DER cert within the walked buffer) at 0,
// cert_len at 4.
#[repr(C)]
pub struct TlsCertEnt {
    pub cert_off: u32, // 0
    pub cert_len: u32, // 4
}

/// Rust port of the bare TLS 1.2 certificate_list walk (cert_store.c
/// cert_parse_chain): each entry is a 3-byte length followed by that many DER
/// bytes, back to back. One step at *pos: bound the 3-byte cert length to the
/// buffer, fill *out with the cert byte-range, advance *pos past it. Returns
/// TLS_WALK_MSG (1), TLS_WALK_END (0) when < 3 header bytes remain, or
/// TLS_WALK_BAD (-1) when the cert length overruns. cert_parse_der (the X.509
/// parse) stays in C and receives buf+cert_off/cert_len from the caller.
/// # Safety: `buf` >= `len` readable bytes; `pos`/`out` non-null writable.
#[no_mangle]
pub extern "C" fn tls_cert_next_rs(buf: *const u8, len: u32, pos: *mut u32, out: *mut TlsCertEnt) -> i32 {
    if buf.is_null() || pos.is_null() || out.is_null() {
        return TLS_WALK_BAD;
    }
    // SAFETY: `pos` valid writable u32 from the C caller.
    let p: u32 = unsafe { *pos };
    // SAFETY: `buf` spans >= `len` readable bytes (caller contract); slice covers
    // exactly `len`; all indexing bounds-checked.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    let hdr_end = match p.checked_add(3) {
        Some(v) => v,
        None => return TLS_WALK_END,
    };
    if hdr_end > len {
        return TLS_WALK_END;
    }
    let pi = p as usize;
    let cert_len = ((b[pi] as u32) << 16) | ((b[pi + 1] as u32) << 8) | (b[pi + 2] as u32);
    let cert_end = match hdr_end.checked_add(cert_len) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if cert_end > len {
        return TLS_WALK_BAD;
    }
    // SAFETY: `out`/`pos` non-null (checked), C-provided writable pointers.
    unsafe {
        let o = &mut *out;
        o.cert_off = hdr_end; // == p + 3
        o.cert_len = cert_len;
        *pos = cert_end;
    }
    TLS_WALK_MSG
}

/// Rust port of the TLS 1.3 CertificateEntry walk (tls.c
/// tls13_parse_certificate_msg): each entry is a 3-byte cert length + that many
/// DER bytes + a 2-byte extensions length + that many extension bytes. One step
/// at *pos over the certificate_list region: bound the cert length, then the
/// 2-byte extensions length, fill *out with the cert byte-range, advance *pos
/// past the extensions. Returns TLS_WALK_MSG (1), TLS_WALK_END (0) when < 3
/// header bytes remain, or TLS_WALK_BAD (-1) when the cert length, the
/// extensions-length field, or the extensions body overruns. The caller passes
/// buf+cert_off/cert_len to cert_parse_der (still C) only for a full entry;
/// matches the C, which on a malformed trailing extensions field adds no cert
/// for that entry and stops.
/// # Safety: `buf` >= `len` readable bytes; `pos`/`out` non-null writable.
#[no_mangle]
pub extern "C" fn tls13_cert_next_rs(buf: *const u8, len: u32, pos: *mut u32, out: *mut TlsCertEnt) -> i32 {
    if buf.is_null() || pos.is_null() || out.is_null() {
        return TLS_WALK_BAD;
    }
    // SAFETY: `pos` valid writable u32 from the C caller.
    let p: u32 = unsafe { *pos };
    // SAFETY: `buf` spans >= `len` readable bytes (caller contract); slice covers
    // exactly `len`; all indexing bounds-checked.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // 3-byte cert length.
    let chdr_end = match p.checked_add(3) {
        Some(v) => v,
        None => return TLS_WALK_END,
    };
    if chdr_end > len {
        return TLS_WALK_END;
    }
    let pi = p as usize;
    let cert_len = ((b[pi] as u32) << 16) | ((b[pi + 1] as u32) << 8) | (b[pi + 2] as u32);
    let cert_end = match chdr_end.checked_add(cert_len) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if cert_end > len {
        return TLS_WALK_BAD;
    }
    // 2-byte extensions length must fit after the cert.
    let ext_hdr_end = match cert_end.checked_add(2) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if ext_hdr_end > len {
        return TLS_WALK_BAD;
    }
    let ci = cert_end as usize;
    let ext_len = ((b[ci] as u32) << 8) | (b[ci + 1] as u32);
    let ext_end = match ext_hdr_end.checked_add(ext_len) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if ext_end > len {
        return TLS_WALK_BAD;
    }
    // SAFETY: `out`/`pos` non-null (checked), C-provided writable pointers.
    unsafe {
        let o = &mut *out;
        o.cert_off = chdr_end; // == p + 3
        o.cert_len = cert_len;
        *pos = ext_end;
    }
    TLS_WALK_MSG
}
