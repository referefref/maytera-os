// rustkern/secstore.rs - #secretuse: the ON-DISK form of an encrypted secret store.
//
// WHY THIS EXISTS.
//
// userland/apps/mfa persists TOTP shared secrets to MFA.DB under a SINGLE FIXED
// XOR BYTE (`#define OBF_KEY 0x5A`, applied in two open-coded loops). The app's
// own UI string says "Secrets stored obfuscated (not encrypted)" and
// fs/perms.c:284 records the consequence plainly: the file IS the second factor,
// so anyone who can read it can generate the codes. A fixed single-byte XOR is
// not a key; it is a formatting convention that anyone who has seen the file
// format can undo by hand.
//
// This module is the replacement FRAMING: a versioned, authenticated,
// password-derived-key container. It deliberately owns only the part that is
// pure and that parses attacker-influenceable bytes, which is the part that has
// to be exactly right and the part Rust is for.
//
// WHAT THIS MODULE DOES NOT DO, so nobody reads more into it than is here:
//   * It does NOT derive the key. The caller supplies 32 bytes. Key derivation
//     from a login password (PBKDF2-HMAC-SHA256, the same construction the
//     SHADOW verifier already uses) is the CALLER's job and is deliberately not
//     duplicated here.
//   * It does NOT read or write files, and it does NOT know what the plaintext
//     means. It frames an opaque byte string.
//   * It is NOT yet the store any shipping app uses. This change lands the
//     format and its proof; the syscall proxy and the app rewrite that consume
//     it are the next stage. Said explicitly because a module with no consumer
//     is the "zero callers" trap, which is why secstore_selftest_rs() runs on
//     EVERY boot from main.c rather than waiting for its first real caller.
//
// THE LAYOUT. All integers little-endian. Total file = 64 + pt_len + 16.
//
//   off  len  field
//     0    8  magic "MSECSTR1"
//     8    4  version (must be exactly 1)
//    12    4  kdf_iters (recorded so a future raise is detectable, not silent)
//    16   16  kdf_salt   (per-store, random, PUBLIC; never the SHADOW salt)
//    32   12  nonce      (per-WRITE, random)
//    44    4  pt_len     (plaintext length, <= SECSTORE_MAX_PLAINTEXT)
//    48   16  reserved, MUST be all zero
//    64  ..   ciphertext (pt_len bytes)
//    ..   16  Poly1305 tag
//
// THE HEADER IS THE AAD. All 64 bytes of it are fed to the AEAD as additional
// authenticated data, which is the property that makes the container
// TAMPER-EVIDENT rather than merely encrypted. Consequences, each deliberate:
//   * Flipping any ciphertext or tag byte fails the tag. Obvious.
//   * Flipping kdf_iters fails the tag. This matters: without the header in the
//     AAD, an attacker could rewrite iters to 1, and a naive reader would then
//     derive the key cheaply enough to brute force offline. An "encrypted" file
//     whose KDF cost is attacker-editable is not protected by its KDF.
//   * Swapping kdf_salt or the nonce fails the tag, so records cannot be
//     transplanted between stores or replayed across writes.
//   * pt_len is authenticated, so a truncation attack cannot be dressed up as a
//     shorter legitimate record.
//
// WHY A RANDOM PER-STORE SALT AND NOT THE SHADOW SALT. If this store were keyed
// by PBKDF2(password, <the SHADOW salt>, <the SHADOW iters>), the derived key
// would be byte-identical to the verifier output that /CONFIG/SHADOW already
// stores in cleartext hex. Reading SHADOW would then hand over the store key,
// and the encryption would protect nothing from anyone who could read the file
// it is stored next to. The salt here is independent and random, so the two
// derivations are unrelated even though both start from the same password.
//
// THE HONEST LIMIT, and it is the important one. At-rest encryption keyed by a
// login password protects a store only against someone who has the DISK and not
// the password. It does not protect against:
//   * root, or the owning uid, on a running machine with the key unlocked;
//   * an offline dictionary attack on a weak password, which PBKDF2 slows by a
//     constant factor and does not prevent;
//   * a machine that unlocks the store WITHOUT a human, which is the bootstrap
//     problem: if a headless boot can derive the key, so can anyone holding the
//     disk, and the encryption is then bookkeeping rather than protection. The
//     iMac target has no TPM, so there is no hardware root to escape this with.
// This container is therefore the right protection for a PER-USER secret
// unlocked by that user's own login password, and it is NOT a solution for
// autologin or service secrets. Do not wire it to one and call it protected.
//
// WHY RUST, per the standing directive: this is new code, there is no
// performance reason for C, and it is the right shape for Rust. It parses
// untrusted on-disk bytes into fixed-size fields, it is pure, and it needs no
// kernel services beyond the AEAD it calls out to. The AEAD itself stays C
// (crypto/chacha20.c) because it is existing, KAT-covered, and already the TLS
// 1.3 data plane; forking it would fork the thing that is already proven.

#![allow(dead_code)]

// The AEAD lives in crypto/chacha20.c. `ciphertext_len` on open INCLUDES the
// 16-byte tag (verified by reading chacha20.c:480-482), and on tag failure the
// primitive crypto_zero()s the plaintext buffer before returning -1, which is
// the property this module relies on to never hand back unverified plaintext.
extern "C" {
    fn chacha20_poly1305_seal(
        key: *const u8,
        nonce: *const u8,
        aad: *const u8,
        aad_len: usize,
        plaintext: *const u8,
        plaintext_len: usize,
        ciphertext: *mut u8,
    ) -> i32;
    fn chacha20_poly1305_open(
        key: *const u8,
        nonce: *const u8,
        aad: *const u8,
        aad_len: usize,
        ciphertext: *const u8,
        ciphertext_len: usize,
        plaintext: *mut u8,
    ) -> i32;
}

/// Fixed header size, and the AAD length.
pub const SECSTORE_HDR_LEN: usize = 64;
/// Poly1305 tag.
pub const SECSTORE_TAG_LEN: usize = 16;
/// Key the caller must supply.
pub const SECSTORE_KEY_LEN: usize = 32;
pub const SECSTORE_SALT_LEN: usize = 16;
pub const SECSTORE_NONCE_LEN: usize = 12;
/// Hard cap on the plaintext. Bounds every length check in this file and keeps a
/// corrupt pt_len from becoming an enormous read. 8 KiB is well clear of the
/// 32-account TOTP store it is sized for.
pub const SECSTORE_MAX_PLAINTEXT: usize = 8192;
/// Smallest possible well-formed file: header + empty ciphertext + tag.
pub const SECSTORE_MIN_FILE: usize = SECSTORE_HDR_LEN + SECSTORE_TAG_LEN;

const MAGIC: [u8; 8] = *b"MSECSTR1";
const VERSION: u32 = 1;

// How many header bytes are fed to the AEAD as additional authenticated data.
// Normally the WHOLE header, which is what makes the container tamper-evident.
//
// HOST KAT RED-TEAM ONLY: the kernel build NEVER sets `secstore_kat_fault` (it
// appears in no Makefile; grep the tree). crypto/secstore_kat.sh compiles a
// second copy WITH it to prove the header-tamper cases genuinely FIRE.
//
// This particular fault is chosen because it is the one a careless
// implementation actually ships: with the header left out of the AAD the
// container still seals, still opens, and still round-trips its plaintext
// perfectly. Only the tamper cases notice. It is the concrete reason a
// round-trip test is not a proof of an AEAD, and a KAT that could not catch it
// would be decoration.
#[cfg(secstore_kat_fault)]
const AAD_LEN: usize = 0;
#[cfg(not(secstore_kat_fault))]
const AAD_LEN: usize = SECSTORE_HDR_LEN;

// Negative returns. Distinct values on purpose: "wrong password" and "corrupt
// file" and "not a store at all" must be distinguishable, because the app they
// replace treated ALL of them as "no accounts" and returned silently, which
// makes a failed decrypt indistinguishable from an empty store. That is how a
// user loses secrets without being told.
pub const SECSTORE_E_ARG: i32 = -1;
/// Magic or version wrong: this is not a v1 store.
pub const SECSTORE_E_MAGIC: i32 = -2;
/// Header self-inconsistent, reserved bytes non-zero, or length disagrees.
pub const SECSTORE_E_FORMAT: i32 = -3;
/// The output buffer is too small. REFUSED, never truncated.
pub const SECSTORE_E_NOSPACE: i32 = -4;
/// Tag mismatch: wrong key, or the bytes were altered. Cannot tell which, by
/// design; an AEAD that could tell you would be leaking.
pub const SECSTORE_E_AUTH: i32 = -5;

#[inline]
fn rd_u32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

#[inline]
fn wr_u32(b: &mut [u8], off: usize, v: u32) {
    let e = v.to_le_bytes();
    b[off] = e[0];
    b[off + 1] = e[1];
    b[off + 2] = e[2];
    b[off + 3] = e[3];
}

/// Zero a slice through a volatile write so the compiler may not elide it. The
/// kernel's secure_zero() is C and takes a raw pointer; this is the in-module
/// equivalent for Rust slices and exists for the same reason.
#[inline]
fn wipe(b: &mut [u8]) {
    for p in b.iter_mut() {
        unsafe { core::ptr::write_volatile(p, 0u8) };
    }
}

/// Parsed header. Pure data, no borrow of the input.
#[derive(Clone, Copy)]
pub struct SecHdr {
    pub iters: u32,
    pub pt_len: usize,
    pub salt: [u8; SECSTORE_SALT_LEN],
    pub nonce: [u8; SECSTORE_NONCE_LEN],
}

/// Strict header parse. Every rule here rejects a specific way a file can lie
/// about itself.
fn parse_hdr(buf: &[u8]) -> Result<SecHdr, i32> {
    // A file shorter than the minimum cannot be a store, and checking this
    // FIRST is what makes every later fixed-offset read in-bounds.
    if buf.len() < SECSTORE_MIN_FILE {
        return Err(SECSTORE_E_FORMAT);
    }
    if buf[0..8] != MAGIC {
        return Err(SECSTORE_E_MAGIC);
    }
    // Version is an EXACT match, not a >= test. A newer store must not be read
    // by older code guessing that the layout is still compatible.
    if rd_u32(buf, 8) != VERSION {
        return Err(SECSTORE_E_MAGIC);
    }
    let iters = rd_u32(buf, 12);
    // iters is advisory to this module (the caller derives the key), but zero is
    // never a legitimate value and is the shape an attacker would write.
    if iters == 0 {
        return Err(SECSTORE_E_FORMAT);
    }
    let pt_len = rd_u32(buf, 44) as usize;
    if pt_len > SECSTORE_MAX_PLAINTEXT {
        return Err(SECSTORE_E_FORMAT);
    }
    // The declared plaintext length must account for EXACTLY the bytes present.
    // Not "at least": a trailing byte nobody accounts for is a file that means
    // something other than what it says.
    if buf.len() != SECSTORE_HDR_LEN + pt_len + SECSTORE_TAG_LEN {
        return Err(SECSTORE_E_FORMAT);
    }
    // Reserved MUST be zero. Enforcing it now is what keeps the field usable
    // later: a field that was never checked cannot be given a meaning without
    // breaking whatever was writing garbage into it.
    for i in 48..64 {
        if buf[i] != 0 {
            return Err(SECSTORE_E_FORMAT);
        }
    }
    let mut salt = [0u8; SECSTORE_SALT_LEN];
    salt.copy_from_slice(&buf[16..32]);
    let mut nonce = [0u8; SECSTORE_NONCE_LEN];
    nonce.copy_from_slice(&buf[32..44]);
    Ok(SecHdr { iters, pt_len, salt, nonce })
}

/// Seal `plaintext` into a complete store file.
///
/// Returns the total number of bytes written, or a negative SECSTORE_E_*.
/// The caller owns salt and nonce generation; the nonce MUST be fresh for every
/// write under the same key, which is why it is an argument rather than
/// something this function invents.
///
/// # Safety
/// `key` has 32 readable bytes, `salt` 16, `nonce` 12, `plaintext` `pt_len`, and
/// `out` `out_cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn secstore_seal_rs(
    key: *const u8,
    salt: *const u8,
    nonce: *const u8,
    iters: u32,
    plaintext: *const u8,
    pt_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    if key.is_null() || salt.is_null() || nonce.is_null() || out.is_null() {
        return SECSTORE_E_ARG;
    }
    if pt_len > 0 && plaintext.is_null() {
        return SECSTORE_E_ARG;
    }
    if pt_len > SECSTORE_MAX_PLAINTEXT || iters == 0 {
        return SECSTORE_E_ARG;
    }
    let total = SECSTORE_HDR_LEN + pt_len + SECSTORE_TAG_LEN;
    // Refuse rather than truncate. A partially written store is a store that
    // will fail to open later, reported as corruption, with no way to tell it
    // from an attack.
    if out_cap < total {
        return SECSTORE_E_NOSPACE;
    }

    let obuf = core::slice::from_raw_parts_mut(out, total);
    // Build the header first: it is the AAD, so it must be final before sealing.
    for b in obuf[0..SECSTORE_HDR_LEN].iter_mut() {
        *b = 0;
    }
    obuf[0..8].copy_from_slice(&MAGIC);
    wr_u32(obuf, 8, VERSION);
    wr_u32(obuf, 12, iters);
    obuf[16..32].copy_from_slice(core::slice::from_raw_parts(salt, SECSTORE_SALT_LEN));
    obuf[32..44].copy_from_slice(core::slice::from_raw_parts(nonce, SECSTORE_NONCE_LEN));
    wr_u32(obuf, 44, pt_len as u32);
    // 48..64 stay zero, and parse_hdr enforces that on the way back in.

    // Split so the AAD borrow and the ciphertext borrow do not overlap.
    let (hdr, ct) = obuf.split_at_mut(SECSTORE_HDR_LEN);
    let rc = chacha20_poly1305_seal(
        key,
        nonce,
        hdr.as_ptr(),
        AAD_LEN,
        plaintext,
        pt_len,
        ct.as_mut_ptr(),
    );
    if rc != 0 {
        // Leave nothing half-sealed behind for a caller that ignores the code.
        wipe(obuf);
        return SECSTORE_E_FORMAT;
    }
    total as i32
}

/// Open a store file.
///
/// Returns the plaintext length written to `out`, or a negative SECSTORE_E_*.
/// On ANY failure `out` is zeroed for `out_cap` bytes, so a caller that ignores
/// the return value cannot read unverified plaintext. That is not a courtesy: it
/// is the property that makes a wrong password safe to get wrong.
///
/// # Safety
/// `key` has 32 readable bytes, `buf` `len` readable, `out` `out_cap` writable.
#[no_mangle]
pub unsafe extern "C" fn secstore_open_rs(
    key: *const u8,
    buf: *const u8,
    len: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    if key.is_null() || buf.is_null() || out.is_null() {
        return SECSTORE_E_ARG;
    }
    if len < SECSTORE_MIN_FILE || len > SECSTORE_HDR_LEN + SECSTORE_MAX_PLAINTEXT + SECSTORE_TAG_LEN
    {
        // Still scrub: the caller may already hold stale plaintext here.
        wipe(core::slice::from_raw_parts_mut(out, out_cap));
        return SECSTORE_E_FORMAT;
    }
    let ibuf = core::slice::from_raw_parts(buf, len);
    let obuf = core::slice::from_raw_parts_mut(out, out_cap);

    let hdr = match parse_hdr(ibuf) {
        Ok(h) => h,
        Err(e) => {
            wipe(obuf);
            return e;
        }
    };
    if out_cap < hdr.pt_len {
        wipe(obuf);
        return SECSTORE_E_NOSPACE;
    }

    let rc = chacha20_poly1305_open(
        key,
        hdr.nonce.as_ptr(),
        ibuf.as_ptr(),
        AAD_LEN,
        ibuf[SECSTORE_HDR_LEN..].as_ptr(),
        hdr.pt_len + SECSTORE_TAG_LEN,
        obuf.as_mut_ptr(),
    );
    if rc != 0 {
        // chacha20_poly1305_open already zeroes pt_len bytes on tag failure;
        // this zeroes the WHOLE output buffer, which also covers out_cap >
        // pt_len and does not depend on that primitive's internals staying so.
        wipe(obuf);
        return SECSTORE_E_AUTH;
    }
    hdr.pt_len as i32
}

/// Report the framing overhead so a C caller can size a buffer without
/// duplicating the constants (and drifting from them).
#[no_mangle]
pub extern "C" fn secstore_overhead_rs() -> u32 {
    (SECSTORE_HDR_LEN + SECSTORE_TAG_LEN) as u32
}

#[no_mangle]
pub extern "C" fn secstore_max_plaintext_rs() -> u32 {
    SECSTORE_MAX_PLAINTEXT as u32
}

// ===========================================================================
// Boot self-test. Returns 0 on success or the CASE NUMBER that failed, the same
// convention as nt_secret_selftest_rs(), because "it failed" is not a useful
// thing to read on a serial console at 3am.
//
// EVERY case here is a REFUSAL or an exact match. A container that has only
// ever been seen to round-trip successfully is not evidence of anything: the
// whole value of an AEAD is what it does with bytes that are WRONG. So the
// round trip is one case out of twelve, and the other eleven are the ways a
// file can be wrong.
// ===========================================================================
#[no_mangle]
pub extern "C" fn secstore_selftest_rs() -> i32 {
    const PT: &[u8; 40] = b"totp:ACME:alice:JBSWY3DPEHPK3PXP:6:30:1\n";
    let key = [0x11u8; SECSTORE_KEY_LEN];
    let salt = [0x22u8; SECSTORE_SALT_LEN];
    let nonce = [0x33u8; SECSTORE_NONCE_LEN];
    let iters: u32 = 50000;

    // 64 + 40 + 16 = 120; a little headroom for the truncation cases.
    let mut file = [0u8; 160];
    let mut plain = [0u8; 64];

    let n = unsafe {
        secstore_seal_rs(
            key.as_ptr(), salt.as_ptr(), nonce.as_ptr(), iters,
            PT.as_ptr(), PT.len(), file.as_mut_ptr(), file.len(),
        )
    };
    // 1: seal succeeds and reports exactly the framed length.
    if n != (SECSTORE_HDR_LEN + PT.len() + SECSTORE_TAG_LEN) as i32 {
        return 1;
    }
    let flen = n as usize;

    // 2: the ciphertext must not equal the plaintext. Catches a seal that
    //    silently degraded to a copy, which a round-trip test alone would pass.
    if file[SECSTORE_HDR_LEN..SECSTORE_HDR_LEN + PT.len()] == PT[..] {
        return 2;
    }

    // 3: round trip returns the exact plaintext.
    let r = unsafe {
        secstore_open_rs(key.as_ptr(), file.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
    };
    if r != PT.len() as i32 || plain[0..PT.len()] != PT[..] {
        return 3;
    }

    // 4: WRONG KEY is refused. This is the "wrong password fails" proof.
    let wrong = {
        let mut k = key;
        k[31] ^= 0x01;
        k
    };
    let r = unsafe {
        secstore_open_rs(wrong.as_ptr(), file.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
    };
    if r != SECSTORE_E_AUTH {
        return 4;
    }
    // 5: and it left NO plaintext behind for a caller that ignored the code.
    for i in 0..plain.len() {
        if plain[i] != 0 {
            return 5;
        }
    }

    // 6: a flipped CIPHERTEXT byte is refused.
    {
        let mut t = file;
        t[SECSTORE_HDR_LEN] ^= 0x80;
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_AUTH {
            return 6;
        }
    }

    // 7: a flipped TAG byte is refused.
    {
        let mut t = file;
        t[flen - 1] ^= 0x01;
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_AUTH {
            return 7;
        }
    }

    // 8: a rewritten kdf_iters is refused. The header is the AAD, so an
    //    attacker cannot downgrade the KDF cost and leave the file openable.
    {
        let mut t = file;
        wr_u32(&mut t, 12, 1);
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_AUTH {
            return 8;
        }
    }

    // 9: a swapped salt is refused (no transplanting between stores).
    {
        let mut t = file;
        t[16] ^= 0xFF;
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_AUTH {
            return 9;
        }
    }

    // 10: a bad magic is refused, and distinguishably so.
    {
        let mut t = file;
        t[0] = b'X';
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_MAGIC {
            return 10;
        }
    }

    // 11: a truncated file is refused as FORMAT, not read short. pt_len is
    //     authenticated, so a short read cannot masquerade as a small store.
    {
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), file.as_ptr(), flen - 1, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_FORMAT {
            return 11;
        }
    }

    // 12: a non-zero reserved field is refused.
    {
        let mut t = file;
        t[48] = 1;
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), t.as_ptr(), flen, plain.as_mut_ptr(), plain.len())
        };
        if r != SECSTORE_E_FORMAT {
            return 12;
        }
    }

    // 13: an output buffer one byte short REFUSES rather than truncating.
    {
        let mut small = [0u8; 39]; // PT.len() is 40
        let r = unsafe {
            secstore_open_rs(key.as_ptr(), file.as_ptr(), flen, small.as_mut_ptr(), small.len())
        };
        if r != SECSTORE_E_NOSPACE {
            return 13;
        }
    }

    // 14: seal into a buffer one byte short REFUSES rather than truncating.
    {
        let mut small = [0u8; SECSTORE_HDR_LEN + 40 + SECSTORE_TAG_LEN - 1];
        let r = unsafe {
            secstore_seal_rs(
                key.as_ptr(), salt.as_ptr(), nonce.as_ptr(), iters,
                PT.as_ptr(), PT.len(), small.as_mut_ptr(), small.len(),
            )
        };
        if r != SECSTORE_E_NOSPACE {
            return 14;
        }
    }

    // 15: a DIFFERENT nonce must produce a different ciphertext for identical
    //     plaintext. Catches a nonce that is accepted and then ignored, which
    //     would silently reuse a keystream across every write.
    {
        let mut n2 = nonce;
        n2[0] ^= 0x01;
        let mut f2 = [0u8; 160];
        let r = unsafe {
            secstore_seal_rs(
                key.as_ptr(), salt.as_ptr(), n2.as_ptr(), iters,
                PT.as_ptr(), PT.len(), f2.as_mut_ptr(), f2.len(),
            )
        };
        if r != flen as i32 {
            return 15;
        }
        if f2[SECSTORE_HDR_LEN..flen - SECSTORE_TAG_LEN]
            == file[SECSTORE_HDR_LEN..flen - SECSTORE_TAG_LEN]
        {
            return 15;
        }
    }

    0
}
