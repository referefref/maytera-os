// rustkern/tls12.rs - #502 TLS 1.2 (RFC 5246) client core - NEW code written in Rust
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #502: TLS 1.2 (RFC 5246) client core - NEW code, written in Rust.
//
// User rule 2026-07-16: new kernel code is Rust unless there is a MEASURED
// performance reason. This is new code, so it is Rust. What lives here is
// exactly the part where bounds-in-the-types pays and where a subtle constant
// is a silent security hole:
//   - tls12_ske_parse_rs      : ServerKeyExchange framing (nested attacker
//                               lengths; the message that carries the ECDHE
//                               params AND the signature binding them to the
//                               certificate).
//   - emsa_pss_verify_rs      : EMSA-PSS-VERIFY (RFC 8017 9.1.2) decode of an
//                               attacker-supplied signature block. Every step
//                               is a bounds/format check; getting any one wrong
//                               is a signature forgery.
//   - tls12_master_secret_rs  : PRF master secret, incl. RFC 7627 extended
//   - tls12_key_block_rs        master secret, key block split, and Finished
//   - tls12_finished_rs         verify_data. The TLS 1.2 key schedule.
//   - tls12_downgrade_check_rs: RFC 8446 4.1.3 ServerHello.random sentinel.
//
// What stays in C (justified, see CHANGELOG): the bignum modexp (crypto/rsa.c)
// and the EC point math (crypto/ecdsa.c) that PSS and ECDH build on, and the
// record/message sequencing in net/tls/tls.c that is entangled with the
// existing C record layer, kmalloc'd record buffers and tls_context_t. Those
// are REUSED shared primitives, not new code; reimplementing them in Rust would
// fork a shared primitive (CLAUDE.md forbids that) and rewrite audited crypto
// for no benefit.
//
// SOFT-FLOAT: no float anywhere here (kernel target is +soft-float / -mno-sse).
// ============================================================================

extern "C" {
    // crypto/crypto.h. C prototypes take `const void*`; pointer type is not
    // part of the C ABI, so *const u8 matches byte-for-byte.
    fn hmac_sha256(key: *const u8, key_len: usize, data: *const u8, data_len: usize, mac: *mut u8);
    fn hmac_sha384(key: *const u8, key_len: usize, data: *const u8, data_len: usize, mac: *mut u8);
    fn sha256(data: *const u8, length: usize, digest: *mut u8);
    fn sha384(data: *const u8, length: usize, digest: *mut u8);
}

// Return codes shared by the #502 helpers (mirror the C side).
const TLS12_OK: i32 = 0;
const TLS12_BAD: i32 = -1;

// Largest label+seed a TLS 1.2 PRF call here ever needs:
//   "extended master secret"(22) + session_hash(<=48) = 70
//   "key expansion"(13) + server_random(32) + client_random(32) = 77
//   "client finished"(15) + transcript hash(<=48) = 63
// 160 is comfortable headroom; anything larger is rejected rather than
// truncated (a truncating PRF would silently derive the WRONG keys).
const TLS12_PRF_MAX_SEED: usize = 160;
const TLS12_MAX_HASH: usize = 48; // SHA-384 is the largest PRF hash we support

// One-shot HMAC with the negotiated PRF hash. `out` must hold >= hash_len.
// Only 32 (SHA-256) and 48 (SHA-384) are valid: those are the PRF hashes of the
// two suites we negotiate (0xc02f -> SHA-256, 0xc030 -> SHA-384). Any other
// length is a caller bug and fails closed rather than picking a default.
fn tls12_hmac(hash_len: usize, key: &[u8], data: &[u8], out: &mut [u8]) -> bool {
    if out.len() < hash_len {
        return false;
    }
    match hash_len {
        32 => {
            // SAFETY: key/data are real slices; out has >= 32 writable bytes
            // (checked above). hmac_sha256 writes exactly 32.
            unsafe { hmac_sha256(key.as_ptr(), key.len(), data.as_ptr(), data.len(), out.as_mut_ptr()) };
            true
        }
        48 => {
            // SAFETY: as above; hmac_sha384 writes exactly 48.
            unsafe { hmac_sha384(key.as_ptr(), key.len(), data.as_ptr(), data.len(), out.as_mut_ptr()) };
            true
        }
        _ => false,
    }
}

// One-shot hash with the given digest length (32 = SHA-256, 48 = SHA-384).
fn tls12_hash(hash_len: usize, data: &[u8], out: &mut [u8]) -> bool {
    if out.len() < hash_len {
        return false;
    }
    match hash_len {
        // SAFETY: data is a real slice; out has >= hash_len writable bytes.
        32 => {
            unsafe { sha256(data.as_ptr(), data.len(), out.as_mut_ptr()) };
            true
        }
        48 => {
            unsafe { sha384(data.as_ptr(), data.len(), out.as_mut_ptr()) };
            true
        }
        _ => false,
    }
}

// RFC 5246 5: P_hash(secret, seed) = HMAC(secret, A(1)+seed) |
//                                    HMAC(secret, A(2)+seed) | ...
// with A(0) = seed, A(i) = HMAC(secret, A(i-1)). Fills `out` completely,
// truncating only the final block (which the RFC allows).
fn tls12_p_hash(hash_len: usize, secret: &[u8], seed: &[u8], out: &mut [u8]) -> bool {
    if hash_len == 0 || hash_len > TLS12_MAX_HASH || seed.len() > TLS12_PRF_MAX_SEED {
        return false;
    }
    let mut a = [0u8; TLS12_MAX_HASH];
    // A(1) = HMAC(secret, A(0)) where A(0) = seed
    if !tls12_hmac(hash_len, secret, seed, &mut a) {
        return false;
    }
    let mut buf = [0u8; TLS12_MAX_HASH + TLS12_PRF_MAX_SEED];
    let mut block = [0u8; TLS12_MAX_HASH];
    let mut off: usize = 0;
    while off < out.len() {
        // buf = A(i) || seed
        buf[..hash_len].copy_from_slice(&a[..hash_len]);
        buf[hash_len..hash_len + seed.len()].copy_from_slice(seed);
        if !tls12_hmac(hash_len, secret, &buf[..hash_len + seed.len()], &mut block) {
            return false;
        }
        let n = core::cmp::min(hash_len, out.len() - off);
        out[off..off + n].copy_from_slice(&block[..n]);
        off += n;
        // A(i+1) = HMAC(secret, A(i))
        let mut next = [0u8; TLS12_MAX_HASH];
        if !tls12_hmac(hash_len, secret, &a[..hash_len], &mut next) {
            return false;
        }
        a[..hash_len].copy_from_slice(&next[..hash_len]);
    }
    true
}

// RFC 5246 5: PRF(secret, label, seed) = P_hash(secret, label + seed).
fn tls12_prf(hash_len: usize, secret: &[u8], label: &[u8], seed: &[u8], out: &mut [u8]) -> bool {
    let total = label.len() + seed.len();
    if total > TLS12_PRF_MAX_SEED {
        return false;
    }
    let mut ls = [0u8; TLS12_PRF_MAX_SEED];
    ls[..label.len()].copy_from_slice(label);
    ls[label.len()..total].copy_from_slice(seed);
    tls12_p_hash(hash_len, secret, &ls[..total], out)
}

/// TLS 1.2 master secret (RFC 5246 8.1, RFC 7627 4).
///
/// `ems` != 0 selects the RFC 7627 extended master secret:
///     master = PRF(pms, "extended master secret", session_hash)
/// where session_hash = Hash(ClientHello..ClientKeyExchange). Otherwise the
/// classic
///     master = PRF(pms, "master secret", client_random + server_random)
///
/// EMS is NOT cosmetic here: we advertise extended_master_secret (ext 23) in
/// the ClientHello, and both real target hosts (xkcd.com, hnrss.org) echo it,
/// so the server derives its master secret the EMS way. Deriving it the classic
/// way against an EMS server yields a different master secret and the ONLY
/// symptom is a Finished mismatch at the very end of the handshake, with no
/// hint as to why. Verified against the servers, not assumed.
///
/// `out` receives exactly 48 bytes. Returns 0 on success, -1 on bad input.
/// # Safety: pointers must be valid for the lengths implied by the arguments;
/// client_random/server_random are 32 bytes each, `out` 48 writable bytes.
#[no_mangle]
pub extern "C" fn tls12_master_secret_rs(
    pms: *const u8,
    pms_len: usize,
    ems: i32,
    client_random: *const u8,
    server_random: *const u8,
    session_hash: *const u8,
    session_hash_len: usize,
    hash_len: usize,
    out: *mut u8,
) -> i32 {
    if pms.is_null() || out.is_null() || pms_len == 0 || pms_len > 256 {
        return TLS12_BAD;
    }
    // SAFETY: caller guarantees `pms` spans pms_len readable bytes.
    let pms_s: &[u8] = unsafe { core::slice::from_raw_parts(pms, pms_len) };
    let mut ms = [0u8; 48];

    let ok = if ems != 0 {
        if session_hash.is_null() || session_hash_len == 0 || session_hash_len > TLS12_MAX_HASH {
            return TLS12_BAD;
        }
        // SAFETY: caller guarantees session_hash spans session_hash_len bytes.
        let sh: &[u8] = unsafe { core::slice::from_raw_parts(session_hash, session_hash_len) };
        tls12_prf(hash_len, pms_s, b"extended master secret", sh, &mut ms)
    } else {
        if client_random.is_null() || server_random.is_null() {
            return TLS12_BAD;
        }
        // SAFETY: both randoms are 32 bytes by the TLS wire format.
        let cr: &[u8] = unsafe { core::slice::from_raw_parts(client_random, 32) };
        let sr: &[u8] = unsafe { core::slice::from_raw_parts(server_random, 32) };
        let mut seed = [0u8; 64];
        seed[..32].copy_from_slice(cr);
        seed[32..].copy_from_slice(sr);
        tls12_prf(hash_len, pms_s, b"master secret", &seed, &mut ms)
    };
    if !ok {
        return TLS12_BAD;
    }
    // SAFETY: caller guarantees `out` has 48 writable bytes.
    unsafe { core::ptr::copy_nonoverlapping(ms.as_ptr(), out, 48) };
    TLS12_OK
}

/// TLS 1.2 key block (RFC 5246 6.3) for an AEAD (GCM) suite: no MAC keys, so
///   key_block = PRF(master, "key expansion", server_random + client_random)
///   -> client_write_key[key_size] | server_write_key[key_size]
///      | client_write_IV[4] | server_write_IV[4]
/// The 4-byte IVs are the GCM *implicit* (salt) halves; the explicit half is
/// per-record and is built by the record layer, NOT here.
///
/// NOTE the seed order: "key expansion" uses server_random + client_random,
/// while the master secret uses client_random + server_random. Swapping them is
/// the classic TLS 1.2 key-schedule bug and produces keys that decrypt nothing.
///
/// Returns 0 on success, -1 on bad input.
/// # Safety: master is 48 bytes; randoms 32 each; ck/sk have key_size writable
/// bytes; civ/siv 4 each.
#[no_mangle]
pub extern "C" fn tls12_key_block_rs(
    master: *const u8,
    client_random: *const u8,
    server_random: *const u8,
    key_size: usize,
    hash_len: usize,
    ck: *mut u8,
    sk: *mut u8,
    civ: *mut u8,
    siv: *mut u8,
) -> i32 {
    if master.is_null() || client_random.is_null() || server_random.is_null()
        || ck.is_null() || sk.is_null() || civ.is_null() || siv.is_null()
    {
        return TLS12_BAD;
    }
    // Only the two AEAD suites we negotiate: AES-128 (16) and AES-256 (32).
    if key_size != 16 && key_size != 32 {
        return TLS12_BAD;
    }
    // SAFETY: caller contract (48-byte master, 32-byte randoms).
    let ms: &[u8] = unsafe { core::slice::from_raw_parts(master, 48) };
    let cr: &[u8] = unsafe { core::slice::from_raw_parts(client_random, 32) };
    let sr: &[u8] = unsafe { core::slice::from_raw_parts(server_random, 32) };

    let mut seed = [0u8; 64];
    seed[..32].copy_from_slice(sr); // server_random FIRST for key expansion
    seed[32..].copy_from_slice(cr);

    let need = key_size * 2 + 8;
    let mut kb = [0u8; 32 * 2 + 8];
    if !tls12_prf(hash_len, ms, b"key expansion", &seed, &mut kb[..need]) {
        return TLS12_BAD;
    }
    // SAFETY: caller-provided output buffers of exactly these sizes.
    unsafe {
        core::ptr::copy_nonoverlapping(kb.as_ptr(), ck, key_size);
        core::ptr::copy_nonoverlapping(kb.as_ptr().add(key_size), sk, key_size);
        core::ptr::copy_nonoverlapping(kb.as_ptr().add(key_size * 2), civ, 4);
        core::ptr::copy_nonoverlapping(kb.as_ptr().add(key_size * 2 + 4), siv, 4);
    }
    TLS12_OK
}

/// TLS 1.2 Finished verify_data (RFC 5246 7.4.9):
///   verify_data = PRF(master_secret, finished_label, Hash(handshake_messages))
///                 [0..12]
/// finished_label is "client finished" or "server finished". `transcript_hash`
/// is the hash of every handshake message so far EXCLUDING the Finished being
/// computed. Always 12 bytes for these suites.
/// Returns 0 on success, -1 on bad input.
/// # Safety: master 48 bytes; transcript_hash hash_len bytes; out 12 writable.
#[no_mangle]
pub extern "C" fn tls12_finished_rs(
    master: *const u8,
    is_client: i32,
    transcript_hash: *const u8,
    hash_len: usize,
    out: *mut u8,
) -> i32 {
    if master.is_null() || transcript_hash.is_null() || out.is_null() {
        return TLS12_BAD;
    }
    if hash_len == 0 || hash_len > TLS12_MAX_HASH {
        return TLS12_BAD;
    }
    // SAFETY: caller contract.
    let ms: &[u8] = unsafe { core::slice::from_raw_parts(master, 48) };
    let th: &[u8] = unsafe { core::slice::from_raw_parts(transcript_hash, hash_len) };
    let label: &[u8] = if is_client != 0 { b"client finished" } else { b"server finished" };
    let mut vd = [0u8; 12];
    if !tls12_prf(hash_len, ms, label, th, &mut vd) {
        return TLS12_BAD;
    }
    // SAFETY: caller guarantees 12 writable bytes.
    unsafe { core::ptr::copy_nonoverlapping(vd.as_ptr(), out, 12) };
    TLS12_OK
}

/// RFC 8446 4.1.3 downgrade protection.
///
/// A TLS 1.3-capable server that ends up negotiating 1.2 (or below) MUST set
/// the last 8 bytes of ServerHello.random to a fixed sentinel. A real 1.2-only
/// server (xkcd.com, hnrss.org) never sets it. So: if we offered 1.3 and the
/// sentinel IS present, the peer is telling us it could have done 1.3 and
/// something forced 1.2 -> that is an active downgrade attack, abort.
///
/// This is the ONLY thing standing between "we support 1.2" and "an attacker
/// can strip every connection down to 1.2". It cannot protect against a MITM
/// downgrading a 1.3 server that is unaware of the sentinel, but every modern
/// 1.3 stack sets it.
///
///   negotiated == 0x0303 (1.2)  -> sentinel 44 4F 57 4E 47 52 44 01
///   negotiated <= 0x0302 (1.1-) -> sentinel 44 4F 57 4E 47 52 44 00
///
/// Returns 0 = no downgrade signalled, -1 = DOWNGRADE DETECTED (caller must
/// abort with illegal_parameter).
/// # Safety: `server_random` must span 32 readable bytes.
#[no_mangle]
pub extern "C" fn tls12_downgrade_check_rs(
    server_random: *const u8,
    negotiated_version: u16,
    offered_13: i32,
) -> i32 {
    if server_random.is_null() {
        return TLS12_BAD;
    }
    if offered_13 == 0 {
        return TLS12_OK; // we never offered 1.3: the sentinel carries no meaning
    }
    // SAFETY: ServerHello.random is 32 bytes by the wire format.
    let sr: &[u8] = unsafe { core::slice::from_raw_parts(server_random, 32) };
    let tail = &sr[24..32];
    const DOWNGRD_12: [u8; 8] = [0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01];
    const DOWNGRD_11: [u8; 8] = [0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x00];
    if negotiated_version == 0x0303 && tail == DOWNGRD_12 {
        return TLS12_BAD;
    }
    if negotiated_version <= 0x0302 && tail == DOWNGRD_11 {
        return TLS12_BAD;
    }
    TLS12_OK
}

// #[repr(C)] mirror of tls12_ske_t (net/tls/tls.h). Layout locked to 32 bytes
// by a _Static_assert on the C side so this can never silently drift.
#[repr(C)]
pub struct Tls12Ske {
    pub curve_type: u8,   // 0
    pub _pad0: u8,        // 1
    pub named_curve: u16, // 2
    pub pub_off: u32,     // 4
    pub pub_len: u32,     // 8
    pub sig_alg: u16,     // 12
    pub _pad1: u16,       // 14
    pub sig_off: u32,     // 16
    pub sig_len: u32,     // 20
    pub params_len: u32,  // 24: signed ECParams+pubkey extent (starts at off 0)
    pub _pad2: u32,       // 28
}

/// Parse a TLS 1.2 ECDHE ServerKeyExchange body (RFC 4492 5.4 + RFC 5246 7.4.3):
///
///   ECParametersServerKeyExchange:
///     curve_type   (1)   MUST be 3 (named_curve); anything else (explicit
///                        prime/char2 curves) is legacy and rejected outright.
///     named_curve  (2)
///     public_len   (1)
///     public       (public_len)
///   digitally-signed:
///     sig_alg      (2)   TLS 1.2 only (1.0/1.1 have no algorithm here)
///     sig_len      (2)
///     signature    (sig_len)
///
/// `params_len` is handed back because the signature covers
/// client_random + server_random + the ECParams bytes EXACTLY as they appeared
/// on the wire (offset 0 .. params_len). Recomputing those bytes instead of
/// re-using the received ones is how signature checks get silently wrong.
///
/// Every length is checked against the real buffer with checked arithmetic, and
/// trailing garbage after the signature is rejected rather than ignored.
/// Returns 1 = parsed, -1 = malformed.
/// # Safety: `buf` must span >= `len` readable bytes; `out` non-null writable.
#[no_mangle]
pub extern "C" fn tls12_ske_parse_rs(buf: *const u8, len: u32, out: *mut Tls12Ske) -> i32 {
    if buf.is_null() || out.is_null() {
        return TLS12_BAD;
    }
    // SAFETY: caller guarantees `buf` spans >= `len` readable bytes; the slice
    // covers exactly `len` so every index below is bounds-checked.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // curve_type(1) + named_curve(2) + public_len(1)
    if len < 4 {
        return TLS12_BAD;
    }
    let curve_type = b[0];
    if curve_type != 3 {
        return TLS12_BAD; // only named_curve
    }
    let named_curve = ((b[1] as u16) << 8) | (b[2] as u16);
    let pub_len = b[3] as u32;
    if pub_len == 0 {
        return TLS12_BAD;
    }
    let pub_off: u32 = 4;
    let params_end = match pub_off.checked_add(pub_len) {
        Some(v) => v,
        None => return TLS12_BAD,
    };
    if params_end > len {
        return TLS12_BAD;
    }

    // digitally-signed: sig_alg(2) + sig_len(2)
    let sig_hdr_end = match params_end.checked_add(4) {
        Some(v) => v,
        None => return TLS12_BAD,
    };
    if sig_hdr_end > len {
        return TLS12_BAD;
    }
    let pe = params_end as usize;
    let sig_alg = ((b[pe] as u16) << 8) | (b[pe + 1] as u16);
    let sig_len = ((b[pe + 2] as u32) << 8) | (b[pe + 3] as u32);
    if sig_len == 0 {
        return TLS12_BAD;
    }
    let sig_end = match sig_hdr_end.checked_add(sig_len) {
        Some(v) => v,
        None => return TLS12_BAD,
    };
    // Exact fit: the ServerKeyExchange body ends with the signature. Trailing
    // bytes mean the message is not what we think it is -> reject.
    if sig_end != len {
        return TLS12_BAD;
    }

    // SAFETY: `out` non-null (checked), C-provided writable Tls12Ske.
    unsafe {
        let o = &mut *out;
        o.curve_type = curve_type;
        o._pad0 = 0;
        o.named_curve = named_curve;
        o.pub_off = pub_off;
        o.pub_len = pub_len;
        o.sig_alg = sig_alg;
        o._pad1 = 0;
        o.sig_off = sig_hdr_end;
        o.sig_len = sig_len;
        o.params_len = params_end; // ECParams span from offset 0
        o._pad2 = 0;
    }
    1
}

// MGF1 (RFC 8017 B.2.1) with the given hash. `seed` is at most one digest.
fn mgf1(hash_len: usize, seed: &[u8], out: &mut [u8]) -> bool {
    if seed.len() > TLS12_MAX_HASH {
        return false;
    }
    let mut buf = [0u8; TLS12_MAX_HASH + 4];
    let mut digest = [0u8; TLS12_MAX_HASH];
    let mut counter: u32 = 0;
    let mut off: usize = 0;
    while off < out.len() {
        buf[..seed.len()].copy_from_slice(seed);
        buf[seed.len()] = (counter >> 24) as u8;
        buf[seed.len() + 1] = (counter >> 16) as u8;
        buf[seed.len() + 2] = (counter >> 8) as u8;
        buf[seed.len() + 3] = counter as u8;
        if !tls12_hash(hash_len, &buf[..seed.len() + 4], &mut digest) {
            return false;
        }
        let n = core::cmp::min(hash_len, out.len() - off);
        out[off..off + n].copy_from_slice(&digest[..n]);
        off += n;
        counter += 1;
    }
    true
}

// Largest RSA modulus we accept for PSS (matches RSA_MAX_KEY_BYTES = 512).
const PSS_MAX_EM: usize = 512;

/// EMSA-PSS-VERIFY (RFC 8017 9.1.2) over an already-modexp'd encoded message.
///
/// The caller (crypto/rsa.c rsa_verify_pss_*) does the bignum s^e mod n with the
/// existing audited C modexp and hands the raw EM here; this function does the
/// entire *decode*, which is where PSS forgeries live. Every one of these checks
/// is load-bearing: skipping the 0xbc trailer, the top-bit mask, the PS-zero /
/// 0x01 separator check, or comparing H' to H is individually enough to let a
/// forged signature through.
///
///   em_bits = modBits - 1; em_len = ceil(em_bits / 8)
///   1. em_len >= hash_len + salt_len + 2
///   2. EM[em_len-1] == 0xbc
///   3. maskedDB = EM[0..em_len-hash_len-1], H = EM[em_len-hash_len-1 ..][..hash_len]
///   4. top (8*em_len - em_bits) bits of maskedDB[0] must be zero
///   5. DB = maskedDB XOR MGF1(H, db_len); clear those same top bits of DB[0]
///   6. DB[0..db_len-salt_len-1] all zero AND DB[db_len-salt_len-1] == 0x01
///   7. H' = Hash(0x00 x8 || mHash || salt); consistent iff H' == H
///
/// Returns 0 = consistent (signature valid), -1 = inconsistent/unsupported.
/// Fails closed on every unexpected input.
/// # Safety: `em` spans em_len readable bytes; `mhash` spans hash_len.
#[no_mangle]
pub extern "C" fn emsa_pss_verify_rs(
    em: *const u8,
    em_len: usize,
    em_bits: u32,
    mhash: *const u8,
    hash_len: usize,
    salt_len: usize,
) -> i32 {
    if em.is_null() || mhash.is_null() {
        return TLS12_BAD;
    }
    if hash_len != 32 && hash_len != 48 {
        return TLS12_BAD; // only SHA-256 / SHA-384 MGF+hash supported
    }
    if em_len == 0 || em_len > PSS_MAX_EM || salt_len > TLS12_MAX_HASH {
        return TLS12_BAD;
    }
    // em_bits must be consistent with em_len: em_len == ceil(em_bits/8).
    let expect_em_len = ((em_bits as usize) + 7) / 8;
    if expect_em_len != em_len {
        return TLS12_BAD;
    }
    // Step 3: emLen < hLen + sLen + 2 -> inconsistent
    if em_len < hash_len + salt_len + 2 {
        return TLS12_BAD;
    }
    // SAFETY: caller guarantees the spans.
    let e: &[u8] = unsafe { core::slice::from_raw_parts(em, em_len) };
    let mh: &[u8] = unsafe { core::slice::from_raw_parts(mhash, hash_len) };

    // Step 4: rightmost octet must be 0xbc
    if e[em_len - 1] != 0xbc {
        return TLS12_BAD;
    }
    // Step 5: split maskedDB || H || 0xbc
    let db_len = em_len - hash_len - 1;
    let h = &e[db_len..db_len + hash_len];

    // Step 6: the leftmost (8*em_len - em_bits) bits of maskedDB[0] must be 0.
    let unused_bits = (8 * em_len) as u32 - em_bits; // 0..=7 by construction
    if unused_bits > 7 {
        return TLS12_BAD;
    }
    if unused_bits > 0 && (e[0] >> (8 - unused_bits)) != 0 {
        return TLS12_BAD;
    }

    // Step 7/8: DB = maskedDB XOR MGF1(H, db_len)
    let mut db = [0u8; PSS_MAX_EM];
    if !mgf1(hash_len, h, &mut db[..db_len]) {
        return TLS12_BAD;
    }
    for i in 0..db_len {
        db[i] ^= e[i];
    }
    // Step 9: clear the same leftmost bits in DB[0]
    if unused_bits > 0 {
        db[0] &= 0xFFu8 >> unused_bits;
    }
    // Step 10: PS must be all-zero and followed by a single 0x01
    let ps_len = db_len - salt_len - 1;
    for i in 0..ps_len {
        if db[i] != 0 {
            return TLS12_BAD;
        }
    }
    if db[ps_len] != 0x01 {
        return TLS12_BAD;
    }
    // Step 11: salt = last salt_len bytes of DB
    let salt = &db[db_len - salt_len..db_len];

    // Step 12/13: M' = (0x)00 00 00 00 00 00 00 00 || mHash || salt ; H' = Hash(M')
    let mut mprime = [0u8; 8 + TLS12_MAX_HASH + TLS12_MAX_HASH];
    let mlen = 8 + hash_len + salt_len;
    for i in 0..8 {
        mprime[i] = 0;
    }
    mprime[8..8 + hash_len].copy_from_slice(mh);
    mprime[8 + hash_len..mlen].copy_from_slice(salt);
    let mut hprime = [0u8; TLS12_MAX_HASH];
    if !tls12_hash(hash_len, &mprime[..mlen], &mut hprime) {
        return TLS12_BAD;
    }
    // Step 14: constant-time compare H' == H
    let mut diff: u8 = 0;
    for i in 0..hash_len {
        diff |= hprime[i] ^ h[i];
    }
    if diff != 0 {
        return TLS12_BAD;
    }
    TLS12_OK
}
