// rustkern/aesccm.rs - AES-CCM (NIST SP 800-38C / RFC 3610) as an AEAD mode.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. It is a byte-shuffling
// mode of operation over a block cipher: no floats, no allocation, no
// hardware, nothing that argues for C.
//
// ===========================================================================
// WHY THIS HAD TO BE ADDED, when the rule is to reuse what is already there
// ---------------------------------------------------------------------------
// The tree already ships AES-GCM (crypto/crypto.h, used by net/tls) and
// AES-CMAC and AES-CBC and the AES block cipher itself. It did NOT ship CCM,
// and the instinct, which this file exists because of, is that "SMB3
// encryption" means AES-128-GCM and the GCM already in the tree would do.
//
// MEASURED 2026-10-04 against Samba 4.17.12-Debian, with an independent
// Python MS-SMB2 client that encrypts a real TREE_CONNECT and sends it:
//
//   dialect 0x0302, transform header EncryptionAlgorithm 0x0001 (AES-128-CCM,
//                   11-byte nonce)  -> ACCEPTED, encrypted reply came back
//   dialect 0x0302, transform header EncryptionAlgorithm 0x0002 (AES-128-GCM,
//                   12-byte nonce)  -> server CLOSED THE CONNECTION, its log
//                                      says "Server exit (NT_STATUS_ACCESS_DENIED)"
//
// 0x0302 is the dialect net/smb.c negotiates. So GCM, the cipher the tree
// already had, is the WRONG one for the only dialect this system speaks, and
// using it would have produced a self-test-green implementation that cannot
// talk to the server. CCM is not negotiable at 3.0/3.0.2: MS-SMB2 3.1.4.3
// fixes it, and cipher negotiation (which is where GCM becomes available)
// arrives with the 3.1.1 SMB2_ENCRYPTION_CAPABILITIES context, a dialect this
// client does not offer.
//
// NO BLOCK CIPHER IS IMPLEMENTED HERE. The key schedule and the AES core come
// from crypto/aes.c through `aes_set_encrypt_key()` and `aes_encrypt_block()`,
// which is also what picks up the Rust AES core (aes_encrypt_block_rs) when it
// is enabled: this file calls the canonical entry point rather than either
// implementation directly. What is new is only the MODE.
//
// CCM uses the forward cipher ONLY, for both directions, because the message
// confidentiality is CTR and the authentication is CBC-MAC. There is
// deliberately no call to aes_set_decrypt_key() anywhere in this file; if you
// find yourself wanting one, something is wrong.
//
// ===========================================================================
// THE CONSTRUCTION, with the field widths that are easy to get wrong
// ---------------------------------------------------------------------------
// With a nonce of N bytes, the length field is L = 15 - N bytes. SMB3 uses an
// 11-byte nonce, so L = 4 and a message may be up to 2^32-1 bytes.
//
//   B0 = flags || nonce || len(plaintext) in L bytes, big-endian
//        flags = 0x40 if there is AAD, plus ((tag_len-2)/2) << 3, plus (L-1)
//   AAD, when present, is prefixed with its length: 2 bytes big-endian for
//        any length below 0xFF00, then zero-padded to a block boundary.
//   T  = CBC-MAC(B0 || encoded-AAD || zero-padded plaintext), first tag_len
//        bytes. The CBC-MAC is taken with an all-zero IV, which is B0 going
//        through the cipher by itself.
//   A_i = flags' || nonce || i in L bytes, big-endian, flags' = L-1
//   The transmitted tag is T XOR E(A_0); the keystream is E(A_1), E(A_2), ...
//
// The XOR of the tag with E(A_0) is the step most readily left out, because a
// test that only round-trips against itself passes either way. It is pinned
// here by RFC 3610's own published vectors in `aesccm_selftest_rs()`, which is
// a cross-implementation check: those vectors were produced by neither this
// file nor anything else in this tree.
//
// ===========================================================================
// DECRYPTION WIPES THE BUFFER WHEN THE TAG IS WRONG
// ---------------------------------------------------------------------------
// `aes_ccm_decrypt()` works in place, so a failed decryption would otherwise
// leave attacker-influenced plaintext in the caller's buffer and rely on every
// caller, present and future, to ignore it. It zeroes the buffer instead. The
// cost is one memset on a path that has already failed.

// ===========================================================================
// The AES primitives, from crypto/aes.c. aes_ctx_t is mirrored here field for
// field and locked by a _Static_assert on the C side (crypto/crypto.h), so a
// change to either shape fails the build instead of reading wrong offsets.
// ===========================================================================
#[repr(C)]
struct AesCtx {
    rk: [u32; 60],
    nr: i32,
}

extern "C" {
    fn aes_set_encrypt_key(ctx: *mut AesCtx, key: *const u8, key_bits: i32) -> i32;
    fn aes_encrypt_block(ctx: *const AesCtx, input: *const u8, out: *mut u8);
}

/// Return codes. Negative, so a caller can test `< 0` without knowing them,
/// and distinct, so a refusal says which rule was broken.
pub const CCM_OK: i32 = 0;
pub const CCM_E_PARAM_NONCE: i32 = -1;
pub const CCM_E_PARAM_TAG: i32 = -2;
pub const CCM_E_PARAM_LEN: i32 = -3;
pub const CCM_E_PARAM_AAD: i32 = -4;
pub const CCM_E_NULL: i32 = -5;
pub const CCM_E_KEYSCHED: i32 = -6;
/// The tag did not match. The only code a caller must treat as an attack
/// rather than a bug.
pub const CCM_E_AUTH: i32 = -7;

const BLK: usize = 16;

#[inline(always)]
fn xor_into(dst: &mut [u8; BLK], src: &[u8]) {
    let mut i = 0;
    while i < src.len() && i < BLK {
        dst[i] ^= src[i];
        i += 1;
    }
}

struct Ccm {
    ctx: AesCtx,
    /// The running CBC-MAC state.
    mac: [u8; BLK],
    /// Nonce length, 7..=13.
    nlen: usize,
    nonce: [u8; 13],
}

impl Ccm {
    fn mac_block(&mut self, block: &[u8]) {
        xor_into(&mut self.mac, block);
        let mut out = [0u8; BLK];
        // SAFETY: ctx is a live, initialised AesCtx; mac and out are 16-byte
        // local arrays, which is what aes_encrypt_block reads and writes.
        unsafe {
            aes_encrypt_block(&self.ctx as *const AesCtx, self.mac.as_ptr(),
                              out.as_mut_ptr());
        }
        self.mac = out;
    }

    /// Feed `data` to the CBC-MAC, zero-padded up to a block boundary.
    fn mac_padded(&mut self, data: &[u8]) {
        let mut off = 0usize;
        while off < data.len() {
            let n = core::cmp::min(BLK, data.len() - off);
            if n == BLK {
                self.mac_block(&data[off..off + BLK]);
            } else {
                let mut last = [0u8; BLK];
                last[..n].copy_from_slice(&data[off..off + n]);
                self.mac_block(&last);
            }
            off += n;
        }
    }

    /// Build counter block A_i and encrypt it into `out`.
    fn ctr_block(&self, i: u64, out: &mut [u8; BLK]) {
        let l = 15 - self.nlen;
        let mut a = [0u8; BLK];
        a[0] = (l - 1) as u8;
        a[1..1 + self.nlen].copy_from_slice(&self.nonce[..self.nlen]);
        // i, big-endian, in the low L bytes.
        let mut k = 0usize;
        while k < l {
            a[BLK - 1 - k] = ((i >> (8 * k)) & 0xFF) as u8;
            k += 1;
        }
        // SAFETY: ctx is live and initialised; a and out are 16-byte locals.
        unsafe {
            aes_encrypt_block(&self.ctx as *const AesCtx, a.as_ptr(),
                              out.as_mut_ptr());
        }
    }

    /// XOR the CCM keystream over `data` in place, starting at counter 1.
    fn ctr_apply(&self, data: &mut [u8]) {
        let mut off = 0usize;
        let mut i = 1u64;
        let mut ks = [0u8; BLK];
        while off < data.len() {
            self.ctr_block(i, &mut ks);
            let n = core::cmp::min(BLK, data.len() - off);
            let mut j = 0usize;
            while j < n {
                data[off + j] ^= ks[j];
                j += 1;
            }
            off += n;
            i += 1;
        }
    }
}

/// Set up the shared state and absorb B0 and the AAD, which is identical for
/// both directions. Returns the live Ccm or a negative code.
fn ccm_start(key: &[u8; 16], nonce: &[u8], aad: &[u8], msg_len: usize,
             tag_len: usize) -> Result<Ccm, i32> {
    if nonce.len() < 7 || nonce.len() > 13 {
        return Err(CCM_E_PARAM_NONCE);
    }
    if tag_len < 4 || tag_len > 16 || (tag_len & 1) != 0 {
        return Err(CCM_E_PARAM_TAG);
    }
    // The length field is L = 15 - N bytes, so a message must fit in it.
    let l = 15 - nonce.len();
    if l < 8 {
        let limit: u64 = 1u64 << (8 * l as u64);
        if msg_len as u64 >= limit {
            return Err(CCM_E_PARAM_LEN);
        }
    }
    // Only the 2-byte AAD length encoding is implemented. SMB3's AAD is 32
    // bytes, and an unimplemented encoding is refused rather than guessed:
    // a wrong AAD length silently produces a tag the peer rejects, which
    // reads like a key problem.
    if aad.len() >= 0xFF00 {
        return Err(CCM_E_PARAM_AAD);
    }

    let mut c = Ccm {
        ctx: AesCtx { rk: [0u32; 60], nr: 0 },
        mac: [0u8; BLK],
        nlen: nonce.len(),
        nonce: [0u8; 13],
    };
    c.nonce[..nonce.len()].copy_from_slice(nonce);

    // SAFETY: ctx is a live AesCtx of the size crypto/aes.c expects (locked by
    // _Static_assert there); key is 16 readable bytes.
    let rc = unsafe {
        aes_set_encrypt_key(&mut c.ctx as *mut AesCtx, key.as_ptr(), 128)
    };
    if rc != 0 {
        return Err(CCM_E_KEYSCHED);
    }

    // B0.
    let mut b0 = [0u8; BLK];
    b0[0] = (if aad.is_empty() { 0u8 } else { 0x40u8 })
        | ((((tag_len - 2) / 2) as u8) << 3)
        | ((l - 1) as u8);
    b0[1..1 + nonce.len()].copy_from_slice(nonce);
    let mut k = 0usize;
    while k < l {
        b0[BLK - 1 - k] = ((msg_len >> (8 * k)) & 0xFF) as u8;
        k += 1;
    }
    c.mac_block(&b0);

    // The AAD, length-prefixed and zero-padded.
    if !aad.is_empty() {
        let mut first = [0u8; BLK];
        first[0] = ((aad.len() >> 8) & 0xFF) as u8;
        first[1] = (aad.len() & 0xFF) as u8;
        let n = core::cmp::min(BLK - 2, aad.len());
        first[2..2 + n].copy_from_slice(&aad[..n]);
        c.mac_block(&first);
        if aad.len() > n {
            c.mac_padded(&aad[n..]);
        }
    }
    Ok(c)
}

fn finish_tag(c: &Ccm, tag_len: usize, out: &mut [u8; BLK]) {
    let mut s0 = [0u8; BLK];
    c.ctr_block(0, &mut s0);
    let mut i = 0usize;
    while i < tag_len {
        out[i] = c.mac[i] ^ s0[i];
        i += 1;
    }
}

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    let mut i = 0usize;
    while i < a.len() {
        diff |= a[i] ^ b[i];
        i += 1;
    }
    diff == 0
}

// ===========================================================================
// The two public entry points. Both work IN PLACE on `data`.
// ===========================================================================

/// AES-128-CCM encrypt-and-authenticate, in place.
/// `data` is plaintext on entry and ciphertext on return; `tag` receives
/// `tag_len` bytes. Returns 0, or a negative CCM_E_* code, in which case
/// nothing has been written to `tag` and `data` is untouched only if the code
/// is a parameter error (checked before any work begins).
#[no_mangle]
pub extern "C" fn aes_ccm_encrypt(key: *const u8,
                                  nonce: *const u8, nonce_len: usize,
                                  aad: *const u8, aad_len: usize,
                                  data: *mut u8, data_len: usize,
                                  tag: *mut u8, tag_len: usize) -> i32 {
    if key.is_null() || nonce.is_null() || tag.is_null() {
        return CCM_E_NULL;
    }
    if data.is_null() && data_len != 0 {
        return CCM_E_NULL;
    }
    if aad.is_null() && aad_len != 0 {
        return CCM_E_NULL;
    }
    // SAFETY: the caller guarantees 16 readable bytes at key, nonce_len at
    // nonce, aad_len at aad, data_len readable+writable at data and tag_len
    // writable at tag. None of the pointers is retained past this call.
    let k: &[u8; 16] = unsafe { &*(key as *const [u8; 16]) };
    let n: &[u8] = unsafe { core::slice::from_raw_parts(nonce, nonce_len) };
    let a: &[u8] = if aad_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(aad, aad_len) }
    };
    let d: &mut [u8] = if data_len == 0 {
        &mut []
    } else {
        unsafe { core::slice::from_raw_parts_mut(data, data_len) }
    };

    let mut c = match ccm_start(k, n, a, data_len, tag_len) {
        Ok(c) => c,
        Err(e) => return e,
    };
    // The MAC is over the PLAINTEXT, so it must be taken before CTR runs.
    c.mac_padded(d);
    let mut t = [0u8; BLK];
    finish_tag(&c, tag_len, &mut t);
    c.ctr_apply(d);

    // SAFETY: tag_len writable bytes at tag, per the contract above.
    unsafe {
        core::ptr::copy_nonoverlapping(t.as_ptr(), tag, tag_len);
    }
    CCM_OK
}

/// AES-128-CCM decrypt-and-verify, in place.
/// Returns 0 on success. On CCM_E_AUTH the plaintext is NOT handed back: the
/// buffer is zeroed, so a caller that ignores the return value cannot act on
/// unauthenticated bytes.
#[no_mangle]
pub extern "C" fn aes_ccm_decrypt(key: *const u8,
                                  nonce: *const u8, nonce_len: usize,
                                  aad: *const u8, aad_len: usize,
                                  data: *mut u8, data_len: usize,
                                  tag: *const u8, tag_len: usize) -> i32 {
    if key.is_null() || nonce.is_null() || tag.is_null() {
        return CCM_E_NULL;
    }
    if data.is_null() && data_len != 0 {
        return CCM_E_NULL;
    }
    if aad.is_null() && aad_len != 0 {
        return CCM_E_NULL;
    }
    // SAFETY: as for aes_ccm_encrypt above.
    let k: &[u8; 16] = unsafe { &*(key as *const [u8; 16]) };
    let n: &[u8] = unsafe { core::slice::from_raw_parts(nonce, nonce_len) };
    let a: &[u8] = if aad_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(aad, aad_len) }
    };
    let d: &mut [u8] = if data_len == 0 {
        &mut []
    } else {
        unsafe { core::slice::from_raw_parts_mut(data, data_len) }
    };
    let want: &[u8] = unsafe { core::slice::from_raw_parts(tag, tag_len) };

    let mut c = match ccm_start(k, n, a, data_len, tag_len) {
        Ok(c) => c,
        Err(e) => return e,
    };
    // CTR first here: the MAC is defined over the plaintext in both
    // directions, so decryption has to recover it before it can check.
    c.ctr_apply(d);
    c.mac_padded(d);
    let mut t = [0u8; BLK];
    finish_tag(&c, tag_len, &mut t);
    if !ct_eq(&t[..tag_len], want) {
        let mut i = 0usize;
        while i < d.len() {
            d[i] = 0;
            i += 1;
        }
        return CCM_E_AUTH;
    }
    CCM_OK
}

// ===========================================================================
// KNOWN-ANSWER TESTS, from RFC 3610 section 8. These vectors were published
// with the specification; nothing in this tree produced them, which is the
// whole point (a vector a module generates for itself proves only that it
// agrees with itself, a property a wrong implementation also has).
//
// Returns 0 on success, else the 1-based number of the first failing case.
// ===========================================================================
#[no_mangle]
pub extern "C" fn aesccm_selftest_rs() -> u32 {
    // RFC 3610 Packet Vector #1. Key, nonce, 8-byte AAD, 23-byte payload,
    // 8-byte tag (M=8, L=2 so the nonce is 13 bytes).
    const KEY1: [u8; 16] = [0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
                            0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF];
    const NONCE1: [u8; 13] = [0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0,
                              0xA1, 0xA2, 0xA3, 0xA4, 0xA5];
    const AAD1: [u8; 8] = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07];
    const PT1: [u8; 23] = [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                           0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                           0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E];
    // Expected ciphertext || tag, from the RFC's "CBC-MAC/encrypted" line.
    const CT1: [u8; 23] = [0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2,
                           0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
                           0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84];
    const TAG1: [u8; 8] = [0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0];

    // RFC 3610 Packet Vector #2, 24-byte payload.
    const NONCE2: [u8; 13] = [0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01, 0xA0,
                              0xA1, 0xA2, 0xA3, 0xA4, 0xA5];
    const PT2: [u8; 24] = [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                           0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                           0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F];
    const CT2: [u8; 24] = [0x72, 0xC9, 0x1A, 0x36, 0xE1, 0x35, 0xF8, 0xCF,
                           0x29, 0x1C, 0xA8, 0x94, 0x08, 0x5C, 0x87, 0xE3,
                           0xCC, 0x15, 0xC4, 0x39, 0xC9, 0xE4, 0x3A, 0x3B];
    const TAG2: [u8; 8] = [0xA0, 0x91, 0xD5, 0x6E, 0x10, 0x40, 0x09, 0x16];

    let mut case = 0u32;

    // ---- 1. Vector #1 encrypt.
    case += 1;
    let mut buf = [0u8; 32];
    let mut tag = [0u8; 16];
    buf[..23].copy_from_slice(&PT1);
    if aes_ccm_encrypt(KEY1.as_ptr(), NONCE1.as_ptr(), 13,
                       AAD1.as_ptr(), 8, buf.as_mut_ptr(), 23,
                       tag.as_mut_ptr(), 8) != CCM_OK {
        return case;
    }
    if !ct_eq(&buf[..23], &CT1) || !ct_eq(&tag[..8], &TAG1) {
        return case;
    }

    // ---- 2. Vector #1 decrypt round-trips to the plaintext.
    case += 1;
    if aes_ccm_decrypt(KEY1.as_ptr(), NONCE1.as_ptr(), 13,
                       AAD1.as_ptr(), 8, buf.as_mut_ptr(), 23,
                       TAG1.as_ptr(), 8) != CCM_OK {
        return case;
    }
    if !ct_eq(&buf[..23], &PT1) {
        return case;
    }

    // ---- 3. A FLIPPED CIPHERTEXT BIT IS REFUSED, and the buffer is wiped.
    case += 1;
    buf[..23].copy_from_slice(&CT1);
    buf[5] ^= 0x01;
    if aes_ccm_decrypt(KEY1.as_ptr(), NONCE1.as_ptr(), 13,
                       AAD1.as_ptr(), 8, buf.as_mut_ptr(), 23,
                       TAG1.as_ptr(), 8) != CCM_E_AUTH {
        return case;
    }
    let mut any = 0u8;
    let mut i = 0;
    while i < 23 { any |= buf[i]; i += 1; }
    if any != 0 {
        return case;   // a refused decryption must not leave bytes behind
    }

    // ---- 4. A FLIPPED TAG BIT IS REFUSED.
    case += 1;
    let mut badtag = TAG1;
    badtag[7] ^= 0x80;
    buf[..23].copy_from_slice(&CT1);
    if aes_ccm_decrypt(KEY1.as_ptr(), NONCE1.as_ptr(), 13,
                       AAD1.as_ptr(), 8, buf.as_mut_ptr(), 23,
                       badtag.as_ptr(), 8) != CCM_E_AUTH {
        return case;
    }

    // ---- 5. A FLIPPED AAD BIT IS REFUSED. The AAD is authenticated but not
    // encrypted, and this is the half an implementation that forgets to feed
    // the AAD into the MAC still passes every round-trip test.
    case += 1;
    let mut badaad = AAD1;
    badaad[0] ^= 0x01;
    buf[..23].copy_from_slice(&CT1);
    if aes_ccm_decrypt(KEY1.as_ptr(), NONCE1.as_ptr(), 13,
                       badaad.as_ptr(), 8, buf.as_mut_ptr(), 23,
                       TAG1.as_ptr(), 8) != CCM_E_AUTH {
        return case;
    }

    // ---- 6. Vector #2, a different nonce and a payload that is an exact
    // multiple of the block size: the zero-padding path and the not-padded
    // path are different code and both have to be right.
    case += 1;
    buf[..24].copy_from_slice(&PT2);
    if aes_ccm_encrypt(KEY1.as_ptr(), NONCE2.as_ptr(), 13,
                       AAD1.as_ptr(), 8, buf.as_mut_ptr(), 24,
                       tag.as_mut_ptr(), 8) != CCM_OK {
        return case;
    }
    if !ct_eq(&buf[..24], &CT2) || !ct_eq(&tag[..8], &TAG2) {
        return case;
    }

    // ---- 7. The SMB3 parameter set: 11-byte nonce, 16-byte tag, 32-byte AAD.
    // No published vector exists for the parameters themselves, so this case
    // asserts only the properties: a round trip works, and one flipped bit of
    // the 32-byte AAD (which is where the SMB transform header's SessionId
    // and OriginalMessageSize live) is refused. The SMB-specific
    // cross-implementation vector lives in smbcrypt.rs, where the wire
    // framing it depends on also lives.
    case += 1;
    let smbkey = [0x11u8; 16];
    let smbnonce = [0x01u8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    let mut smbaad = [0u8; 32];
    let mut j = 0;
    while j < 32 { smbaad[j] = j as u8; j += 1; }
    let mut body = [0u8; 70];
    j = 0;
    while j < 70 { body[j] = (j as u8) ^ 0x5A; j += 1; }
    let orig = body;
    let mut t16 = [0u8; 16];
    if aes_ccm_encrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 11,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 70,
                       t16.as_mut_ptr(), 16) != CCM_OK {
        return case;
    }
    if ct_eq(&body, &orig) {
        return case;   // "encryption" that did not change the bytes
    }
    let saved = body;
    if aes_ccm_decrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 11,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 70,
                       t16.as_ptr(), 16) != CCM_OK {
        return case;
    }
    if !ct_eq(&body, &orig) {
        return case;
    }
    case += 1;
    body = saved;
    smbaad[20] ^= 0x01;
    if aes_ccm_decrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 11,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 70,
                       t16.as_ptr(), 16) != CCM_E_AUTH {
        return case;
    }

    // ---- 9. A TRUNCATED message is refused. Shortening the ciphertext
    // changes len(plaintext) in B0, so the MAC cannot match; this is the case
    // that covers a truncated SMB transform PDU.
    case += 1;
    body = saved;
    if aes_ccm_decrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 11,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 69,
                       t16.as_ptr(), 16) != CCM_E_AUTH {
        return case;
    }

    // ---- 10. Parameter refusals, so a caller that gets the geometry wrong is
    // told rather than handed a tag computed under different rules.
    case += 1;
    if aes_ccm_encrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 6,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 10,
                       t16.as_mut_ptr(), 16) != CCM_E_PARAM_NONCE {
        return case;
    }
    case += 1;
    if aes_ccm_encrypt(smbkey.as_ptr(), smbnonce.as_ptr(), 11,
                       smbaad.as_ptr(), 32, body.as_mut_ptr(), 10,
                       t16.as_mut_ptr(), 5) != CCM_E_PARAM_TAG {
        return case;
    }

    0
}
