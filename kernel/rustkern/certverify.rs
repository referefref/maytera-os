// rustkern/certverify.rs - #510 / MAYTERA-SEC-2026-0017 TLS 1.3 CertificateVerify (RFC 8446 4.4.3)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #510 / MAYTERA-SEC-2026-0017: TLS 1.3 CertificateVerify (RFC 8446 4.4.3).
//
// [RUST-SEC] THE HOLE THIS CLOSES: tls.c parsed CertificateVerify and threw it
// away. In TLS 1.3 that message is the server's ONLY proof that it holds the
// private key for the certificate it presented. The chain check (#232) proves
// the certificate is genuine and trusted; it does NOT prove the peer OWNS it.
// Certificates are PUBLIC, so an on-path attacker could replay the real chain,
// run its OWN ECDHE, and complete the handshake: Finished still matched because
// Finished is keyed off the attacker's ECDHE, not the server's key. Silent, full
// MITM of every TLS 1.3 connection while the kernel printed "chain verified OK".
//
// This is NEW code, not a port, so there is no `_c` twin and no -D fallback:
// per the 2026-07-16 Rust rule new kernel code is Rust, and a fallback here
// would only be a second, weaker way to authenticate the peer.
//
// The split follows #502: Rust owns the framing and the exact signed-content
// bytes (where bounds and byte-layout errors bite); C keeps the message
// sequencing and the cert key-union dispatch (cert_verify_tls_signature), which
// reuses the proven bignum/EC primitives rather than forking them.

// #[repr(C)] mirror of tls13_cv_t (net/tls/tls.h). Layout asserted == 12 bytes
// on the C side (_Static_assert in tls.c).
use crate::tls_parse::{TLS_WALK_BAD, TLS_WALK_MSG};

#[repr(C)]
pub struct Tls13Cv {
    pub sig_scheme: u16, // 0: SignatureScheme from the wire
    pub _pad: u16,       // 2
    pub sig_off: u32,    // 4: offset of the signature within the CV body
    pub sig_len: u32,    // 8: signature length
}

/// RFC 8446 4.4.3 context string for a SERVER CertificateVerify. Exactly 33
/// bytes. "server" is load-bearing: the client-side string differs, and signing
/// the wrong one is the classic mirror/reflection confusion. Proven by oracle
/// negative control (wrong-ctx rejected on all 5 live hosts).
const TLS13_CV_CTX: &[u8; 33] = b"TLS 1.3, server CertificateVerify";

/// Parse a TLS 1.3 CertificateVerify body: `SignatureScheme algorithm` (2 bytes)
/// then `opaque signature<0..2^16-1>` (2-byte length + that many bytes).
/// Returns TLS_WALK_MSG (1) with *out filled, or TLS_WALK_BAD (-1) if the body
/// is short, the declared signature length overruns, the signature is empty, or
/// trailing bytes remain. STRICT (`sig_end != len` is rejected): RFC 8446 gives
/// this body an exact length, so trailing bytes are a framing violation, and a
/// verifier that tolerates them lets an attacker vary the encoding of a message
/// that is supposed to be rigid. Fail closed.
/// # Safety: `buf` must point to >= `len` readable bytes; `out` non-null writable.
#[no_mangle]
pub extern "C" fn tls13_cv_parse_rs(buf: *const u8, len: u32, out: *mut Tls13Cv) -> i32 {
    if buf.is_null() || out.is_null() {
        return TLS_WALK_BAD;
    }
    if len < 4 {
        return TLS_WALK_BAD;
    }
    // SAFETY: `buf` spans >= `len` readable bytes (caller contract); slice covers
    // exactly `len`; all indexing bounds-checked.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };
    let scheme = ((b[0] as u16) << 8) | (b[1] as u16);
    let sig_len = ((b[2] as u32) << 8) | (b[3] as u32);
    if sig_len == 0 {
        return TLS_WALK_BAD;
    }
    let sig_end = match 4u32.checked_add(sig_len) {
        Some(v) => v,
        None => return TLS_WALK_BAD,
    };
    if sig_end != len {
        return TLS_WALK_BAD;
    }
    // SAFETY: `out` non-null (checked), C-provided writable pointer.
    unsafe {
        let o = &mut *out;
        o.sig_scheme = scheme;
        o._pad = 0;
        o.sig_off = 4;
        o.sig_len = sig_len;
    }
    TLS_WALK_MSG
}

/// Build the RFC 8446 4.4.3 signed content that the server's CertificateVerify
/// signature covers:
///   64 bytes of 0x20, then "TLS 1.3, server CertificateVerify", then 0x00,
///   then Transcript-Hash(ClientHello..Certificate).
/// Writes 98 + th_len bytes into `out`; returns that length, or -1.
///
/// `th` MUST be the hash of the NEGOTIATED CIPHER SUITE (32 for *_SHA256, 48 for
/// *_SHA384), NOT the signature scheme's hash. Those differ on real hosts:
/// feeds.bbci.co.uk / lwn.net / api.moonshot.ai all negotiate a SHA-384 suite
/// yet sign with a SHA-256 scheme (0x0403 / 0x0804). Using the scheme's hash
/// here would still verify against a SHA-256-suite server (lobste.rs,
/// reddit.com) and fail only against the others: exactly the "green test, open
/// hole" shape. th_len is therefore whitelisted to 32/48 rather than trusted.
/// # Safety: `th` >= `th_len` readable bytes; `out` >= `out_cap` writable bytes.
#[no_mangle]
pub extern "C" fn tls13_cv_content_rs(th: *const u8, th_len: u32, out: *mut u8, out_cap: u32) -> i32 {
    if th.is_null() || out.is_null() {
        return -1;
    }
    if th_len != 32 && th_len != 48 {
        return -1;
    }
    let need: u32 = 64 + 33 + 1 + th_len; // 130 (SHA-256) or 146 (SHA-384)
    if out_cap < need {
        return -1;
    }
    // SAFETY: caller contract gives >= th_len readable / >= out_cap writable
    // bytes; `need` <= out_cap was just checked, so the write slice is in range.
    let h: &[u8] = unsafe { core::slice::from_raw_parts(th, th_len as usize) };
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, need as usize) };
    for i in 0..64 {
        o[i] = 0x20;
    }
    o[64..97].copy_from_slice(TLS13_CV_CTX);
    o[97] = 0x00;
    o[98..(98 + th_len as usize)].copy_from_slice(h);
    need as i32
}
