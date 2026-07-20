// rustkern/hmac.rs - #404 Phase K / #493 HMAC construction (RFC 2104 / RFC 4231)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase K port (#404 / #493): HMAC construction (RFC 2104 / RFC 4231).
//
// Unlike the pure leaf cores above (each a fixed-size block transform), HMAC is
// a CONSTRUCTION that WRAPS a hash. So this Rust port legitimately calls BACK
// into the C hash API via extern "C": the already-Rust compression cores
// (sha256/sha512/md5 transforms, live under -DRUST_SHA256/512/MD5) are reached
// through the C init/update/final wrappers, so the actual compression still runs
// in Rust while the padding/streaming bookkeeping stays in the audited C hash
// glue. The ONE Rust thing here is the HMAC outer construction:
//     K'    = (key_len > B) ? H(key) : key            (then zero-padded to B)
//     ipad  = K' XOR 0x36 (B bytes) ; opad = K' XOR 0x5c (B bytes)
//     MAC   = H( opad || H( ipad || msg ) )
// parameterized by block size B and digest size D of the chosen hash. Three
// public one-shot entry points route here under -DRUST_HMAC:
//     hmac_sha256_rs (B=64,  D=32) - TLS 1.3 Finished key schedule + CSPRNG
//                                     HMAC-DRBG (crypto/csprng.c) one-shot
//     hmac_sha384_rs (B=128, D=48) - TLS 1.3 SHA-384 cipher-suite Finished
//     hmac_md5_rs    (B=64,  D=16) - HMAC-MD5 (NTLM/SMB auth in net/smb.c)
// The kernel's INCREMENTAL streaming HMAC (hmac_*_init/update/final, used by the
// CSPRNG's per-field DRBG update) stays C for now; only the one-shot public
// symbols cross to Rust this phase. The original one-shots are kept as
// hmac_*_c for the boot-time differential (hmac_rust_selftest in crypto/hmac.c)
// which proves rs == c AND replays the RFC 4231 / RFC 2202 known-answer tests
// through the live hmac_* API before any TLS/CSPRNG consumer runs.
//
// Proven locally (standalone C, blame.md pre-flight discipline) before the
// kernel build: RFC 4231 HMAC-SHA256 TC1/TC2/TC6 + HMAC-SHA384 TC1/TC2, RFC
// 2202 HMAC-MD5 TC1/TC2 all PASS, plus 200,000 random (key,msg) vectors of the
// direct construction vs an independent incremental-HMAC reference, 0 mismatch.
//
// The hash context never has its C struct definition duplicated in Rust: it is
// handled as an OPAQUE, 16-byte-aligned, bounded byte buffer larger than any
// kernel hash ctx (the largest is sha512_ctx_t at 200 bytes; 256 is a safe
// upper bound, asserted in C via _Static_assert). init/update/final treat the
// buffer as their own ctx type; Rust only passes the pointer through.

// C hash glue (defined in crypto/sha256.c, crypto/sha512.c, crypto/md5.c). All
// arguments are plain pointers, so declaring the ctx as *mut u8 (rather than the
// C struct) is ABI-identical. `_update` takes (ctx, data, len); `_final` writes
// exactly D digest bytes to `out`. sha384 shares sha512_update.
extern "C" {
    fn sha256_init(ctx: *mut u8);
    fn sha256_update(ctx: *mut u8, data: *const u8, len: usize);
    fn sha256_final(ctx: *mut u8, out: *mut u8);
    fn sha384_init(ctx: *mut u8);
    fn sha512_update(ctx: *mut u8, data: *const u8, len: usize);
    fn sha384_final(ctx: *mut u8, out: *mut u8);
    fn md5_init(ctx: *mut u8);
    fn md5_update(ctx: *mut u8, data: *const u8, len: usize);
    fn md5_final(ctx: *mut u8, out: *mut u8);
}

type HmacInit = unsafe extern "C" fn(*mut u8);
type HmacUpdate = unsafe extern "C" fn(*mut u8, *const u8, usize);
type HmacFinal = unsafe extern "C" fn(*mut u8, *mut u8);

// Opaque hash-context storage: 16-aligned and larger than any kernel hash ctx.
#[repr(C, align(16))]
struct HmacHashCtx([u8; 256]);

/// Overwrite a byte slice with zeros using volatile writes so the compiler
/// cannot elide the wipe of sensitive key-derived material (ipad/opad/K').
#[inline]
fn hmac_zeroize(b: &mut [u8]) {
    for x in b.iter_mut() {
        // SAFETY: `x` is a valid, uniquely-borrowed, aligned u8 within `b`.
        unsafe { core::ptr::write_volatile(x, 0u8); }
    }
}

/// The shared HMAC construction (RFC 2104), parameterized by the hash's block
/// size `block_size` (<= 128) and digest size `digest_size` (<= 64) plus the C
/// hash init/update/final. Byte-for-byte equivalent to the kernel's C HMAC.
///
/// # Safety
/// The three fn pointers must be the matching init/update/final of ONE hash
/// whose block size == `block_size` (<= 128) and digest size == `digest_size`
/// (<= 64). `key` must point to `key_len` readable bytes (or be unused when
/// key_len == 0), `data` to `data_len` readable bytes (the C hash reads nothing
/// when data_len == 0), and `mac` to `digest_size` writable bytes. All hash
/// scratch stays inside the bounded, 16-aligned `HmacHashCtx` and the bounded
/// stack arrays; no pointer is retained past this call. block_size and
/// digest_size are compile-time constants at every call site (64/32, 128/48,
/// 64/16), so the <=128 / <=64 array bounds always hold.
#[inline]
unsafe fn hmac_construct(
    init: HmacInit,
    update: HmacUpdate,
    finalize: HmacFinal,
    block_size: usize,
    digest_size: usize,
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
    mac: *mut u8,
) {
    let mut ctx = HmacHashCtx([0u8; 256]);
    let cp = ctx.0.as_mut_ptr();

    // K' normalized to the block size (zero-padded). Bounded [u8; 128].
    let mut kb = [0u8; 128];
    if key_len > block_size {
        // K' = H(key); writes digest_size bytes, the rest stays zero-padded.
        init(cp);
        update(cp, key, key_len);
        finalize(cp, kb.as_mut_ptr());
    } else if key_len > 0 {
        // key_len <= block_size <= 128: verbatim copy, rest stays zero.
        core::ptr::copy_nonoverlapping(key, kb.as_mut_ptr(), key_len);
    }

    // ipad / opad = K' XOR 0x36 / 0x5c over the block. Bounded [u8; 128].
    let mut ipad = [0u8; 128];
    let mut opad = [0u8; 128];
    let mut i = 0usize;
    while i < block_size {
        ipad[i] = kb[i] ^ 0x36;
        opad[i] = kb[i] ^ 0x5c;
        i += 1;
    }

    // inner = H(ipad || msg). Digest lands in a bounded [u8; 64].
    let mut inner = [0u8; 64];
    init(cp);
    update(cp, ipad.as_ptr(), block_size);
    update(cp, data, data_len);
    finalize(cp, inner.as_mut_ptr());

    // MAC = H(opad || inner[..digest_size]).
    init(cp);
    update(cp, opad.as_ptr(), block_size);
    update(cp, inner.as_ptr(), digest_size);
    finalize(cp, mac);

    // Wipe key-derived + hash-state material.
    hmac_zeroize(&mut kb);
    hmac_zeroize(&mut ipad);
    hmac_zeroize(&mut opad);
    hmac_zeroize(&mut inner);
    hmac_zeroize(&mut ctx.0);
}

/// HMAC-SHA256 one-shot (RFC 2104), B=64 D=32. crypto/sha256.c routes the live
/// hmac_sha256() symbol here under -DRUST_HMAC. Primary consumer: TLS 1.3
/// Finished key schedule (net/tls/tls.c) + CSPRNG HMAC-DRBG (crypto/csprng.c).
/// # Safety: `key`/`data`/`mac` obey the hmac_construct contract; `mac` has 32
/// writable bytes.
#[no_mangle]
pub extern "C" fn hmac_sha256_rs(
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
    mac: *mut u8,
) {
    // SAFETY: sha256_init/update/final are the matching hash triple for B=64,
    // D=32; the caller guarantees mac points to 32 writable bytes and key/data
    // to their stated lengths. All other scratch is bounded inside hmac_construct.
    unsafe {
        hmac_construct(
            sha256_init, sha256_update, sha256_final,
            64, 32, key, key_len, data, data_len, mac,
        );
    }
}

/// HMAC-SHA384 one-shot (RFC 2104), B=128 D=48. crypto/sha512.c routes the live
/// hmac_sha384() symbol here under -DRUST_HMAC. Consumer: TLS 1.3 SHA-384
/// cipher-suite Finished key schedule (net/tls/tls.c).
/// # Safety: `mac` has 48 writable bytes; key/data obey the hmac_construct
/// contract.
#[no_mangle]
pub extern "C" fn hmac_sha384_rs(
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
    mac: *mut u8,
) {
    // SAFETY: sha384_init/sha512_update/sha384_final are the matching triple for
    // B=128, D=48; caller guarantees mac points to 48 writable bytes. Bounded
    // scratch inside hmac_construct (block_size 128 == the kb/ipad/opad bound).
    unsafe {
        hmac_construct(
            sha384_init, sha512_update, sha384_final,
            128, 48, key, key_len, data, data_len, mac,
        );
    }
}

/// HMAC-MD5 one-shot (RFC 2104), B=64 D=16. crypto/hmac.c routes the live
/// hmac_md5() symbol here under -DRUST_HMAC. Consumer: NTLM/SMB auth
/// (net/smb.c). MD5 is cryptographically weak; this only preserves the existing
/// NTLM interop path (no new use).
/// # Safety: `mac` has 16 writable bytes; key/data obey the hmac_construct
/// contract.
#[no_mangle]
pub extern "C" fn hmac_md5_rs(
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
    mac: *mut u8,
) {
    // SAFETY: md5_init/update/final are the matching triple for B=64, D=16;
    // caller guarantees mac points to 16 writable bytes. Bounded scratch inside
    // hmac_construct.
    unsafe {
        hmac_construct(
            md5_init, md5_update, md5_final,
            64, 16, key, key_len, data, data_len, mac,
        );
    }
}
