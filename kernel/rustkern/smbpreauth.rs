// rustkern/smbpreauth.rs - SMB 3.1.1 PRE-AUTHENTICATION INTEGRITY
//
// THE GAP THIS CLOSES, in one sentence: before this, nothing protected the
// OPENING HANDSHAKE of an SMB file-sharing connection. The NEGOTIATE exchange
// decides the protocol version, whether messages will be signed, whether they
// will be encrypted and with which cipher, and it happens in the clear, before
// any key exists, with nothing authenticating it. An attacker able to modify
// packets in flight edited that exchange and NEITHER SIDE COULD TELL
// AFTERWARDS.
//
// Two defences already shipped and both sit ON TOP of this gap rather than
// closing it:
//
//   - MESSAGE SIGNING (rustkern/smbsign.rs, advisory 0047) proves who sent each
//     message AFTER the handshake. By construction it cannot protect the
//     handshake: the key it would need does not exist yet.
//
//   - ENCRYPTION (rustkern/smbcrypt.rs, advisory 0049) was, until this module,
//     the only thing blocking a version downgrade, AND ONLY AS A SIDE EFFECT.
//     The client required encryption, so a server that negotiated a dialect
//     without it failed. That is load-bearing protection coming from the wrong
//     place: relaxing `encrypt=` in /CONFIG/SMB.CFG silently removed downgrade
//     protection, which nobody would guess from the setting's name.
//
// The mechanism that actually closes it is SMB 3.1.1's pre-authentication
// integrity hash: a running SHA-512 over the NEGOTIATE and SESSION_SETUP
// messages, mixed into the key schedule, so a tampered handshake produces keys
// that DO NOT MATCH and the session fails at the first signature check.
//
// ============================================================================
// WHAT IS MEASURED AND WHAT IS INFERRED
// ============================================================================
//
// Every constant and every byte layout below was MEASURED on 2026-10-07 against
// Samba 4.17.12, by reconstructing a real smbclient SMB 3.1.1 session from a
// packet capture plus the account password and checking the result against the
// signature SAMBA ITSELF put on the final SESSION_SETUP response. A wrong label,
// a wrong seeding, or a wrong message set makes that signature not match, so the
// match is the proof. The recovered values are pinned as known-answer vectors in
// the self-test at the bottom of this file (the REAL_* constants).
//
// MEASURED, in that session:
//
//   1. The running hash starts at SIXTY-FOUR ZERO BYTES, and each fold is an
//      INDEPENDENT SHA-512 over the previous 64-byte value followed by the
//      message:  H = SHA512(H_prev || message).  No streaming context has to
//      survive between messages, which is why this struct carries 64 bytes of
//      hash and not a 200-byte hash context.
//
//   2. The messages folded in, in order, are exactly FIVE:
//        NEGOTIATE request, NEGOTIATE response,
//        SESSION_SETUP request #1, SESSION_SETUP response #1
//                                  (STATUS_MORE_PROCESSING_REQUIRED),
//        FINAL SESSION_SETUP request.
//      The FINAL SESSION_SETUP RESPONSE IS NOT FOLDED. It cannot be: it is the
//      message whose signature the derived key has to verify.
//      What is folded is the SMB2 message only, never the 4-byte NetBIOS
//      length prefix.
//
//   3. MEASURED, AND NOT WHAT I EXPECTED: the real client does NOT sign the
//      final SESSION_SETUP request. Its Flags field was 0x00000010
//      (SMB2_FLAGS_PRIORITY_MASK bits), with SMB2_FLAGS_SIGNED (0x08) CLEAR and
//      the Signature field all zero. An earlier attempt here DID sign it, which
//      changed the bytes folded into the hash by one bit in Flags, which changed
//      every derived key, and Samba dropped the connection with
//      NT_STATUS_INVALID_HANDLE and "No signing key for SMB2 signing" -- an
//      error message that points nowhere near the actual cause. This client
//      therefore also does not sign it, and smbsign.rs already declines to sign
//      SESSION_SETUP (R_NOT_ARMED for pre-session commands), so the two agree
//      by construction rather than by coincidence.
//
//   4. The four KDF labels, with the 64-byte running hash as the SP800-108
//      CONTEXT and the 16-byte session key as Ki:
//        SigningKey        = KDF(SessionKey, "SMBSigningKey\0",   PreauthHash)
//        ClientToServerKey = KDF(SessionKey, "SMBC2SCipherKey\0", PreauthHash)
//        ServerToClientKey = KDF(SessionKey, "SMBS2CCipherKey\0", PreauthHash)
//        ApplicationKey    = KDF(SessionKey, "SMBAppKey\0",       PreauthHash)
//      The KDF itself is the SAME one 3.0.2 uses, smbsign.rs's SP800-108
//      counter-mode HMAC-SHA256 construction; only the label and the context
//      change. It is NOT reimplemented here. What DID have to change is that
//      smbsign's scratch buffer was 48 bytes and a 64-byte context does not
//      fit; see smbsign_kdf128_ctx().
//
//   5. The SMB2_TRANSFORM_HEADER field at offset 42, which is
//      EncryptionAlgorithm at 3.0.2 and Flags at 3.1.1, carried 0x0001 in both
//      roles. So smbcrypt.rs's existing AES-128-CCM seal/unseal is wire-correct
//      at 3.1.1 WITHOUT CHANGE, as long as CCM is the cipher that was
//      negotiated. The CCM nonce is 11 bytes at both dialects; a server
//      TRANSFORM frame from the captured session decrypted with the
//      3.1.1-derived ServerToClientKey and an 11-byte nonce.
//
//   6. Negotiate-context framing. NegotiateContextOffset is counted FROM THE
//      START OF THE SMB2 HEADER (offset 0 of the message, not from the end of
//      the body), and is 8-byte aligned. Each context is an 8-byte preamble
//      (ContextType u16, DataLength u16, Reserved u32) followed by DataLength
//      bytes. Contexts are padded to an 8-byte boundary BETWEEN one another;
//      the LAST context is NOT padded, so the message can and does end
//      unaligned. In the server's NEGOTIATE response the contexts sit AFTER the
//      SPNEGO security buffer, so they must be found through
//      NegotiateContextOffset and never by walking on from the body.
//
//   7. What Samba chooses, which is why the offer below is narrow. Offered the
//      full cipher list a real smbclient sends, Samba 4.17.12 selected
//      AES-128-GCM for encryption and AES-GMAC for signing. THIS KERNEL HAS
//      NEITHER. Offered a cipher list of AES-128-CCM alone it selected
//      AES-128-CCM, and offered a signing list of AES-CMAC alone it selected
//      AES-CMAC; omitting the signing context entirely also produced AES-CMAC,
//      which is the specified default. So this client offers exactly what it
//      can actually do, and a server that will not accept that is refused by
//      name rather than silently mis-keyed.
//
// INFERRED, NOT MEASURED, and said plainly because it matters:
//
//   - Only Samba 4.17.12 was exercised. No Windows server, no macOS SMB server,
//     and no Azure Files endpoint was tested. A Windows server that requires
//     AES-GMAC signing or AES-256-GCM encryption will be REFUSED by this client,
//     not silently downgraded, which is the safe failure but is still a failure.
//   - The SMB2_COMPRESSION_CAPABILITIES, SMB2_NETNAME_NEGOTIATE_CONTEXT_ID,
//     SMB2_TRANSPORT_CAPABILITIES and SMB2_RDMA_TRANSFORM_CAPABILITIES contexts
//     are neither sent nor interpreted. They are folded into the hash like every
//     other byte of the handshake, so they cannot be edited undetected; they are
//     simply not acted on.
//   - Multi-channel and session binding are not implemented, so the separate
//     Session.PreauthIntegrityHashValue-per-channel rules do not arise.
//
// ============================================================================
// WHAT NOW PROTECTS WHAT. READ THIS BEFORE CHANGING A POLICY DEFAULT.
// ============================================================================
//
// There are TWO distinct downgrade attacks and they need DIFFERENT defences.
// Conflating them is how the old code ended up with encryption accidentally
// holding up version security.
//
//   ATTACK A, VERSION DOWNGRADE: the attacker deletes 0x0311 from the dialect
//   list in our NEGOTIATE request. The server then legitimately selects 3.0.2,
//   where no pre-auth hash exists on either side, so THE HASH CANNOT DETECT
//   THIS. Nothing inside 3.1.1 can: the attack prevents 3.1.1 from happening.
//   The only defence is a CLIENT-SIDE FLOOR: if we offered 3.1.1 we require
//   3.1.1, and we refuse anything lower. That is `smbpreauth_floor_check()`,
//   and `min_dialect=3.1.1` is the DEFAULT.
//
//   ATTACK B, FEATURE DOWNGRADE WITHIN 3.1.1: the attacker edits any other part
//   of the handshake, for example clearing SMB2_GLOBAL_CAP_ENCRYPTION, swapping
//   the cipher the server chose, flipping a bit of either salt, or altering the
//   NTLM exchange. Every one of those bytes is folded into the running hash, so
//   the two sides' hashes diverge, every derived key diverges, and the signature
//   on the final SESSION_SETUP response FAILS TO VERIFY. This is the mechanism
//   that genuinely authenticates the handshake, and it works regardless of what
//   the encryption policy is set to.
//
// Therefore, AND THIS IS THE POINT OF THE WHOLE MODULE: after this change a
// user may set `encrypt=enabled` or even `encrypt=off` in /CONFIG/SMB.CFG and
// STILL have handshake integrity. Encryption protects confidentiality, which is
// what its name suggests and all it should ever have been responsible for. The
// floor plus the pre-auth hash protect the negotiation. Relaxing one no longer
// quietly removes the other.
//
// ONE DEPENDENCY REMAINS AND IT IS DELIBERATE: the pre-auth hash is only ever
// CHECKED by a signature, so at 3.1.1 the final SESSION_SETUP response MUST be
// signed and MUST verify, whatever the signing policy says. An unsigned final
// response under `signing=enabled` would leave the hash computed and never
// compared, which is the shape of a control that looks present and does
// nothing. net/smb.c refuses that case explicitly; see the call site.
//
// HONEST LIMITS OF THIS CONTROL, stated so nobody has to discover them:
//   - It authenticates the handshake only as far as the SESSION KEY is
//     authentic. With NTLM that key comes from the password, so an attacker who
//     knows the password defeats this as they defeat everything else.
//   - It detects tampering; it does not prevent it. The session fails, loudly.
//     That is the correct outcome and it is also a denial of service an attacker
//     can always cause anyway by dropping packets.
//   - A server that does not offer 3.1.1 AT ALL is refused under the default
//     floor. That is a real compatibility cost of a real security gain, and it
//     is a one-line config change to take the old behaviour back, with the
//     console saying exactly what was given up.

use core::sync::atomic::{AtomicU32, Ordering};

// ============================================================================
// FFI into the existing kernel crypto. NOTHING IS REIMPLEMENTED HERE.
// ============================================================================
//
// SHA-512 comes from crypto/sha512.c, whose block compression already routes
// into rustkern/sha512.rs under -DRUST_SHA512. Reaching it through the C
// init/update/final triple rather than calling the Rust transform directly is
// deliberate: it is the SAME path every other SHA-512 user in the kernel takes,
// including the boot self-test that proves it against the NIST vectors, so this
// module cannot be right about a hash the rest of the kernel is wrong about.
extern "C" {
    fn sha512_init(ctx: *mut u8);
    fn sha512_update(ctx: *mut u8, data: *const u8, len: usize);
    fn sha512_final(ctx: *mut u8, digest: *mut u8);

    // smbsign.rs's SP800-108 KDF, widened to take a 64-byte context for this
    // module and given a RETURN CODE, which the older void-returning
    // smbsign_kdf128_shared() never had. Returns 0 on success.
    fn smbsign_kdf128_ctx(ki: *const u8,
                          label: *const u8, label_len: usize,
                          context: *const u8, context_len: usize,
                          out: *mut u8) -> i32;
}

/// Opaque room for one `sha512_ctx_t`. The C side carries a
/// `_Static_assert(sizeof(sha512_ctx_t) <= SMB_PREAUTH_SHA512_CTX_ROOM)` in
/// net/smb.h, so this cannot drift into a stack overwrite without the build
/// stopping. The same pattern is already used by rustkern/hmac.rs.
const SHA512_CTX_ROOM: usize = 256;

// ============================================================================
// Wire constants. EVERY ONE WAS READ OFF A REAL CAPTURE, not recalled.
// ============================================================================

pub const DIALECT_202: u16 = 0x0202;
pub const DIALECT_210: u16 = 0x0210;
pub const DIALECT_300: u16 = 0x0300;
pub const DIALECT_302: u16 = 0x0302;
pub const DIALECT_311: u16 = 0x0311;

const SMB2_HDR_LEN: usize = 64;
const OFF_MAGIC: usize = 0;
const OFF_COMMAND: usize = 12;
const OFF_FLAGS: usize = 16;
const SMB2_MAGIC: u32 = 0x424D_53FE;
const SMB2_FLAGS_SERVER_TO_REDIR: u32 = 0x0000_0001;

// NEGOTIATE response field offsets, relative to the start of the SMB2 header.
// 64 is the header; the response body begins there.
const OFF_R_STRUCT: usize = 64;          // StructureSize, must be 65
const OFF_R_DIALECT: usize = 68;         // DialectRevision
const OFF_R_NCTX_COUNT: usize = 70;      // NegotiateContextCount
const OFF_R_NCTX_OFFSET: usize = 64 + 60; // NegotiateContextOffset
const NEGOTIATE_RESP_MIN: usize = 64 + 64;

// Negotiate context types (MS-SMB2 2.2.3.1). Only the first two and the eighth
// are acted on; the rest are listed so an unexpected one can be NAMED.
const CTX_PREAUTH_INTEGRITY: u16 = 0x0001;
const CTX_ENCRYPTION: u16 = 0x0002;
const CTX_COMPRESSION: u16 = 0x0003;
const CTX_NETNAME: u16 = 0x0005;
const CTX_TRANSPORT: u16 = 0x0006;
const CTX_RDMA_TRANSFORM: u16 = 0x0007;
const CTX_SIGNING: u16 = 0x0008;

/// The only hash algorithm MS-SMB2 defines for pre-auth integrity.
const HASH_SHA512: u16 = 0x0001;
const PREAUTH_HASH_LEN: usize = 64;
const SALT_LEN: usize = 32;

// Cipher ids. This client offers CCM alone because CCM is what it implements.
const CIPHER_AES128_CCM: u16 = 0x0001;
const CIPHER_AES128_GCM: u16 = 0x0002;
const CIPHER_AES256_CCM: u16 = 0x0003;
const CIPHER_AES256_GCM: u16 = 0x0004;

// Signing algorithm ids. AES-CMAC alone, for the same reason.
const SIGNALG_HMAC_SHA256: u16 = 0x0000;
const SIGNALG_AES_CMAC: u16 = 0x0001;
const SIGNALG_AES_GMAC: u16 = 0x0002;

/// The KDF labels, MEASURED. Each INCLUDES its terminating NUL, because the
/// SP800-108 input does.
const LBL_SIGNING: &[u8] = b"SMBSigningKey\0";
const LBL_C2S: &[u8] = b"SMBC2SCipherKey\0";
const LBL_S2C: &[u8] = b"SMBS2CCipherKey\0";
const LBL_APP: &[u8] = b"SMBAppKey\0";

// ============================================================================
// Refusal codes. Mirrored in net/smb.h only as prose; C reads the text.
// ============================================================================

pub const R_OK: u16 = 0;
pub const R_NULL: u16 = 1;
pub const R_SHORT: u16 = 2;
pub const R_NOT_ACTIVE: u16 = 3;
pub const R_DIALECT_FLOOR: u16 = 4;
pub const R_NO_PREAUTH_CONTEXT: u16 = 5;
pub const R_HASH_ALGO: u16 = 6;
pub const R_CIPHER_NOT_OFFERED: u16 = 7;
pub const R_SIGNALG_NOT_OFFERED: u16 = 8;
pub const R_CONTEXT_MALFORMED: u16 = 9;
pub const R_FROZEN: u16 = 10;
pub const R_NO_SESSION_KEY: u16 = 11;
pub const R_KDF: u16 = 12;
pub const R_SALT: u16 = 13;
pub const R_CONTEXT_DUP: u16 = 14;
pub const R_NOT_SMB2: u16 = 15;
pub const R_TOO_MANY_FOLDS: u16 = 16;
pub const R_NO_ROOM: u16 = 17;

// ============================================================================
// Policy: the DIALECT FLOOR. This is Attack A's only defence.
// ============================================================================

pub const FLOOR_NONE: u32 = 0;
pub const FLOOR_311: u32 = 1;

/// Default is the STRONG one. A kernel with no /CONFIG/SMB.CFG requires 3.1.1,
/// and therefore requires an authenticated handshake.
static FLOOR: AtomicU32 = AtomicU32::new(FLOOR_311);

#[no_mangle]
pub extern "C" fn smbpreauth_floor_set(floor: u32) {
    // Anything unrecognised resolves to the STRONGER setting, never the weaker
    // one. A typo in a config file must not quietly disable a control.
    FLOOR.store(if floor == FLOOR_NONE { FLOOR_NONE } else { FLOOR_311 },
                Ordering::Release);
}

#[no_mangle]
pub extern "C" fn smbpreauth_floor() -> u32 {
    FLOOR.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn smbpreauth_floor_text(floor: u32) -> *const u8 {
    if floor == FLOOR_NONE {
        b"none: 3.0.2 and below, as the signing policy allows (handshake NOT authenticated)\0".as_ptr()
    } else {
        b"3.1.1 (handshake authenticated by pre-auth integrity)\0".as_ptr()
    }
}

/// Lock the C-side `SMB_PREAUTH_FLOOR_*` macros against these values, the same
/// way smbsign and smbcrypt lock theirs. A silent renumbering would turn
/// "require 3.1.1" into "allow 3.0.2" with nothing to show for it.
#[no_mangle]
pub extern "C" fn smbpreauth_floor_abi_check(c_none: u32, c_311: u32) -> i32 {
    if c_none != FLOOR_NONE { return 1; }
    if c_311 != FLOOR_311 { return 2; }
    0
}

fn fold_ascii(b: u8) -> u8 {
    if b >= b'A' && b <= b'Z' { b | 0x20 } else { b }
}

/// Parse `min_dialect=` out of /CONFIG/SMB.CFG. Same file, same comment rules
/// and same case folding as the `signing=` and `encrypt=` parsers, because three
/// different parsers over one file is three chances to disagree about what a
/// line means.
///
/// Returns 1 for `3.1.1`, 0 for `3.0.2`, -1 when the key is absent, -2 when the
/// key is present with a value this kernel does not recognise.
#[no_mangle]
pub extern "C" fn smbpreauth_floor_parse(buf: *const u8, len: i32) -> i32 {
    if buf.is_null() || len <= 0 {
        return -1;
    }
    // SAFETY: the caller guarantees `len` readable bytes at `buf`; the slice is
    // not retained past this call.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };
    let n = s.len();
    let mut i = 0usize;
    while i < n {
        // Skip leading blanks on the line.
        while i < n && (s[i] == b' ' || s[i] == b'\t') { i += 1; }
        if i < n && (s[i] == b'#' || s[i] == b';') {
            while i < n && s[i] != b'\n' { i += 1; }
            i += 1;
            continue;
        }
        let key = b"min_dialect";
        let mut k = 0usize;
        let mut j = i;
        while k < key.len() && j < n && fold_ascii(s[j]) == key[k] { k += 1; j += 1; }
        if k == key.len() {
            while j < n && (s[j] == b' ' || s[j] == b'\t') { j += 1; }
            if j < n && s[j] == b'=' {
                j += 1;
                while j < n && (s[j] == b' ' || s[j] == b'\t') { j += 1; }
                let start = j;
                while j < n && s[j] != b'\n' && s[j] != b'\r'
                    && s[j] != b' ' && s[j] != b'\t'
                    && s[j] != b'#' && s[j] != b';' { j += 1; }
                let val = &s[start..j];
                if tok_eq(val, b"3.1.1") || tok_eq(val, b"311") {
                    return 1;
                }
                if tok_eq(val, b"3.0.2") || tok_eq(val, b"302") {
                    return 0;
                }
                return -2;
            }
        }
        while i < n && s[i] != b'\n' { i += 1; }
        i += 1;
    }
    -1
}

fn tok_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() { return false; }
    let mut i = 0;
    while i < a.len() {
        if fold_ascii(a[i]) != fold_ascii(b[i]) { return false; }
        i += 1;
    }
    true
}

// ============================================================================
// The per-connection state.
// ============================================================================

/// Mirrored by `smb_preauth_t` in net/smb.h with a sizeof and alignment lock.
#[repr(C)]
pub struct SmbPreauth {
    /// The running SHA-512. Starts at 64 zero bytes, which is also the value a
    /// freshly reset struct holds, so "never folded" and "correctly seeded" are
    /// the same state and there is no third one to get wrong.
    hash: [u8; PREAUTH_HASH_LEN],
    /// The 32 random bytes we put in our own PREAUTH_INTEGRITY context. Kept so
    /// the console can show it and so the self-test can prove it reached the
    /// wire unchanged, not because the protocol needs us to remember it.
    salt: [u8; SALT_LEN],
    /// How many messages went into the hash. Evidence, not decoration: a claim
    /// that the handshake was authenticated can be checked against a NUMBER,
    /// and the expected number is 5.
    folded: u32,
    /// 1 once we actually put 0x0311 on the wire.
    offered: u8,
    /// 1 once the server selected 0x0311 and the hash is live.
    active: u8,
    /// 1 once keys have been derived from the hash. A fold after this point is
    /// refused rather than silently changing a hash nothing will re-read.
    frozen: u8,
    /// 1 once the server's PREAUTH_INTEGRITY context was seen and said SHA-512.
    hash_alg_ok: u8,
    /// The cipher and signing algorithm the server selected, as wire values, or
    /// 0xFFFF for "the server sent no such context", which is legal and means
    /// the specified default.
    cipher: u16,
    signalg: u16,
    /// 1 once the final SESSION_SETUP response was verified against a key
    /// derived from this hash. THIS is the flag that means the handshake was
    /// actually authenticated, as opposed to merely hashed.
    verified: u8,
    pad: [u8; 3],
}

const NO_CONTEXT: u16 = 0xFFFF;

const SMBPREAUTH_ZERO: SmbPreauth = SmbPreauth {
    hash: [0u8; PREAUTH_HASH_LEN],
    salt: [0u8; SALT_LEN],
    folded: 0,
    offered: 0,
    active: 0,
    frozen: 0,
    hash_alg_ok: 0,
    cipher: NO_CONTEXT,
    signalg: NO_CONTEXT,
    verified: 0,
    pad: [0u8; 3],
};

fn zeroize(b: &mut [u8]) {
    let mut i = 0;
    while i < b.len() {
        // read_volatile/write_volatile so the compiler cannot elide the wipe of
        // a buffer that is never read again.
        unsafe { core::ptr::write_volatile(&mut b[i] as *mut u8, 0) };
        i += 1;
    }
}

fn rd_u16(b: &[u8], off: usize) -> u16 {
    (b[off] as u16) | ((b[off + 1] as u16) << 8)
}

fn rd_u32(b: &[u8], off: usize) -> u32 {
    (b[off] as u32) | ((b[off + 1] as u32) << 8)
        | ((b[off + 2] as u32) << 16) | ((b[off + 3] as u32) << 24)
}

fn wr_u16(b: &mut [u8], off: usize, v: u16) {
    b[off] = (v & 0xFF) as u8;
    b[off + 1] = (v >> 8) as u8;
}

fn wr_u32(b: &mut [u8], off: usize, v: u32) {
    b[off] = (v & 0xFF) as u8;
    b[off + 1] = ((v >> 8) & 0xFF) as u8;
    b[off + 2] = ((v >> 16) & 0xFF) as u8;
    b[off + 3] = ((v >> 24) & 0xFF) as u8;
}

#[no_mangle]
pub extern "C" fn smbpreauth_reset(st: *mut SmbPreauth) {
    if st.is_null() { return; }
    // SAFETY: the caller guarantees a live, aligned, uniquely-owned SmbPreauth.
    let s = unsafe { &mut *st };
    zeroize(&mut s.hash);
    zeroize(&mut s.salt);
    *s = SMBPREAUTH_ZERO;
}

#[no_mangle]
pub extern "C" fn smbpreauth_is_active(st: *const SmbPreauth) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbPreauth.
    let s = unsafe { &*st };
    (s.active != 0) as i32
}

#[no_mangle]
pub extern "C" fn smbpreauth_is_verified(st: *const SmbPreauth) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbPreauth.
    let s = unsafe { &*st };
    (s.verified != 0) as i32
}

#[no_mangle]
pub extern "C" fn smbpreauth_folded(st: *const SmbPreauth) -> u32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbPreauth.
    unsafe { (*st).folded }
}

#[no_mangle]
pub extern "C" fn smbpreauth_cipher(st: *const SmbPreauth) -> u16 {
    if st.is_null() { return NO_CONTEXT; }
    // SAFETY: caller guarantees a live, aligned SmbPreauth.
    unsafe { (*st).cipher }
}

#[no_mangle]
pub extern "C" fn smbpreauth_signalg(st: *const SmbPreauth) -> u16 {
    if st.is_null() { return NO_CONTEXT; }
    // SAFETY: caller guarantees a live, aligned SmbPreauth.
    unsafe { (*st).signalg }
}

/// Copy the 64-byte running hash out, for a console line or a test. Returns 0
/// on success. Deliberately NOT how keys are derived: the key schedule stays
/// inside this module so the hash never has to be handed to a caller who might
/// keep it.
#[no_mangle]
pub extern "C" fn smbpreauth_hash_copy(st: *const SmbPreauth, out: *mut u8,
                                       out_len: usize) -> i32 {
    if st.is_null() || out.is_null() || out_len < PREAUTH_HASH_LEN { return -1; }
    // SAFETY: caller guarantees a live SmbPreauth and out_len writable bytes.
    unsafe {
        core::ptr::copy_nonoverlapping((*st).hash.as_ptr(), out, PREAUTH_HASH_LEN);
    }
    0
}

#[no_mangle]
pub extern "C" fn smbpreauth_reason_text(code: u16) -> *const u8 {
    match code {
        R_OK => b"ok\0".as_ptr(),
        R_NULL => b"internal error: null pre-auth integrity state\0".as_ptr(),
        R_SHORT => b"the message is too short to be an SMB2 message\0".as_ptr(),
        R_NOT_ACTIVE =>
            b"pre-auth integrity is not active on this connection\0".as_ptr(),
        R_DIALECT_FLOOR =>
            b"the server selected an SMB dialect BELOW the configured floor, so \
              the opening handshake would not be authenticated and a version \
              downgrade could not be detected\0".as_ptr(),
        R_NO_PREAUTH_CONTEXT =>
            b"the server selected SMB 3.1.1 but sent no PREAUTH_INTEGRITY \
              negotiate context, which 3.1.1 requires\0".as_ptr(),
        R_HASH_ALGO =>
            b"the server chose a pre-auth integrity hash this kernel does not \
              implement (only SHA-512 is defined)\0".as_ptr(),
        R_CIPHER_NOT_OFFERED =>
            b"the server chose an encryption cipher this client did not offer \
              (this kernel implements AES-128-CCM only)\0".as_ptr(),
        R_SIGNALG_NOT_OFFERED =>
            b"the server chose a signing algorithm this client did not offer \
              (this kernel implements AES-128-CMAC only)\0".as_ptr(),
        R_CONTEXT_MALFORMED =>
            b"a negotiate context has an offset or length that does not fit \
              inside the message\0".as_ptr(),
        R_FROZEN =>
            b"internal error: a message was folded into the pre-auth hash after \
              the session keys were already derived from it\0".as_ptr(),
        R_NO_SESSION_KEY =>
            b"no session key: an anonymous or guest session has nothing to \
              derive handshake-integrity keys from\0".as_ptr(),
        R_KDF => b"internal error: the key derivation refused its inputs\0".as_ptr(),
        R_SALT =>
            b"the server's PREAUTH_INTEGRITY context has an implausible salt \
              length\0".as_ptr(),
        R_CONTEXT_DUP =>
            b"the server sent the same negotiate context type twice\0".as_ptr(),
        R_NOT_SMB2 => b"the message does not begin with the SMB2 magic\0".as_ptr(),
        R_TOO_MANY_FOLDS =>
            b"more handshake messages were folded into the pre-auth hash than \
              the handshake has\0".as_ptr(),
        R_NO_ROOM =>
            b"not enough room in the send buffer for the negotiate contexts\0".as_ptr(),
        _ => b"unknown pre-auth integrity refusal\0".as_ptr(),
    }
}

// ============================================================================
// Offering 3.1.1, and building the request contexts.
// ============================================================================

/// Should net/smb.c put 0x0311 in the dialect array? Always yes: offering it
/// costs nothing and a server that does not support it simply picks something
/// else, which the floor then judges. The question of whether that lower
/// dialect is ACCEPTABLE is a separate decision made by the floor, and keeping
/// the two apart is what stops "we did not ask" from masquerading as "it was
/// not available".
#[no_mangle]
pub extern "C" fn smbpreauth_offer_311() -> i32 {
    1
}

/// Build the three negotiate contexts for our NEGOTIATE request into `out`.
///
/// `base_len` is the length of the message so far (SMB2 header + 36-byte body +
/// the dialect array), because the padding to the 8-byte boundary that precedes
/// the first context depends on it, and `*out_pad` reports how many pad bytes
/// the caller must insert before copying this blob. Returns the number of
/// context bytes written, or a negative refusal code.
///
/// The salt is supplied by the caller rather than drawn here, so this function
/// is a pure function of its inputs and the self-test can pin it to a
/// known-answer vector. A key-schedule input that comes from a global RNG is a
/// key-schedule input you cannot write a test for.
#[no_mangle]
pub extern "C" fn smbpreauth_build_contexts(st: *mut SmbPreauth,
                                            salt: *const u8, salt_len: usize,
                                            base_len: usize,
                                            out: *mut u8, out_cap: usize,
                                            out_pad: *mut u32,
                                            out_count: *mut u16) -> i32 {
    if st.is_null() || salt.is_null() || out.is_null()
        || out_pad.is_null() || out_count.is_null() {
        return -(R_NULL as i32);
    }
    if salt_len != SALT_LEN {
        return -(R_SALT as i32);
    }
    // SAFETY: the caller guarantees a live SmbPreauth, salt_len readable bytes
    // at salt, out_cap writable bytes at out, and writable u32/u16 at the two
    // out parameters. No pointer is retained.
    let s = unsafe { &mut *st };
    let sl: &[u8] = unsafe { core::slice::from_raw_parts(salt, salt_len) };
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };

    let pad = (8 - (base_len % 8)) % 8;

    // PREAUTH_INTEGRITY_CAPABILITIES: HashAlgorithmCount(2) SaltLength(2)
    // HashAlgorithms[1](2) Salt(32) = 38 bytes of data.
    // ENCRYPTION_CAPABILITIES: CipherCount(2) Ciphers[1](2) = 4.
    // SIGNING_CAPABILITIES: SigningAlgorithmCount(2) Algorithms[1](2) = 4.
    // Each needs an 8-byte preamble, and each but the LAST is padded up to 8.
    const D_PREAUTH: usize = 4 + 2 + SALT_LEN;      // 38
    const D_ENC: usize = 2 + 2;                      // 4
    const D_SIGN: usize = 2 + 2;                     // 4
    let c1 = 8 + D_PREAUTH;                          // 46, padded to 48
    let c1p = (c1 + 7) & !7usize;
    let c2 = 8 + D_ENC;                              // 12, padded to 16
    let c2p = (c2 + 7) & !7usize;
    let c3 = 8 + D_SIGN;                             // 12, NOT padded: last
    let total = c1p + c2p + c3;
    if out_cap < total {
        return -(R_NO_ROOM as i32);
    }

    let mut i = 0usize;
    while i < total { o[i] = 0; i += 1; }

    // --- context 1: PREAUTH_INTEGRITY_CAPABILITIES
    let mut p = 0usize;
    wr_u16(o, p, CTX_PREAUTH_INTEGRITY);
    wr_u16(o, p + 2, D_PREAUTH as u16);
    wr_u32(o, p + 4, 0);
    wr_u16(o, p + 8, 1);                 // HashAlgorithmCount
    wr_u16(o, p + 10, SALT_LEN as u16);  // SaltLength
    wr_u16(o, p + 12, HASH_SHA512);      // HashAlgorithms[0]
    let mut k = 0usize;
    while k < SALT_LEN { o[p + 14 + k] = sl[k]; k += 1; }
    p += c1p;

    // --- context 2: ENCRYPTION_CAPABILITIES, AES-128-CCM alone
    wr_u16(o, p, CTX_ENCRYPTION);
    wr_u16(o, p + 2, D_ENC as u16);
    wr_u32(o, p + 4, 0);
    wr_u16(o, p + 8, 1);                      // CipherCount
    wr_u16(o, p + 10, CIPHER_AES128_CCM);
    p += c2p;

    // --- context 3: SIGNING_CAPABILITIES, AES-CMAC alone
    //
    // Sent EXPLICITLY rather than omitted. Omitting it also yields AES-CMAC,
    // because that is the specified default and Samba was measured doing
    // exactly that, but then our capability statement is an absence, and an
    // absence cannot be folded into a hash as a positive claim about what we
    // can do. With the context present, a server cannot later be understood to
    // have been offered AES-GMAC, which this kernel does not implement.
    wr_u16(o, p, CTX_SIGNING);
    wr_u16(o, p + 2, D_SIGN as u16);
    wr_u32(o, p + 4, 0);
    wr_u16(o, p + 8, 1);                      // SigningAlgorithmCount
    wr_u16(o, p + 10, SIGNALG_AES_CMAC);

    s.salt.copy_from_slice(sl);
    s.offered = 1;
    // SAFETY: both out pointers were null-checked above.
    unsafe {
        *out_pad = pad as u32;
        *out_count = 3;
    }
    total as i32
}

// ============================================================================
// THE RUNNING HASH.
// ============================================================================

/// Fold one handshake message into the running hash: H = SHA512(H || message).
///
/// `pdu`/`len` is the SMB2 message WITHOUT the 4-byte NetBIOS length prefix.
/// Passing the prefix in would produce a hash that differs from the server's by
/// four bytes per message and would fail only at the very last check, with
/// nothing to say why, so this function refuses anything that does not start
/// with the SMB2 magic.
#[no_mangle]
pub extern "C" fn smbpreauth_fold(st: *mut SmbPreauth, pdu: *const u8,
                                  len: usize) -> u16 {
    if st.is_null() || pdu.is_null() { return R_NULL; }
    // SAFETY: the caller guarantees a live SmbPreauth and `len` readable bytes
    // at `pdu`. Neither pointer is retained past this call.
    let s = unsafe { &mut *st };
    if len < SMB2_HDR_LEN { return R_SHORT; }
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    if rd_u32(p, OFF_MAGIC) != SMB2_MAGIC { return R_NOT_SMB2; }
    if s.frozen != 0 { return R_FROZEN; }
    // The handshake this client performs has exactly five foldable messages.
    // A sixth means a caller is folding something that does not belong, which
    // would corrupt the hash in a way only the far end could notice.
    if s.folded >= 5 { return R_TOO_MANY_FOLDS; }

    let mut ctx = [0u8; SHA512_CTX_ROOM];
    let mut digest = [0u8; PREAUTH_HASH_LEN];
    // SAFETY: sha512_init/update/final are the matching triple from
    // crypto/sha512.c. `ctx` is at least sizeof(sha512_ctx_t) (locked by a
    // _Static_assert in net/smb.h), the two update calls read 64 and `len`
    // bytes from live slices, and final writes exactly 64 bytes to `digest`.
    unsafe {
        sha512_init(ctx.as_mut_ptr());
        sha512_update(ctx.as_mut_ptr(), s.hash.as_ptr(), PREAUTH_HASH_LEN);
        sha512_update(ctx.as_mut_ptr(), p.as_ptr(), len);
        sha512_final(ctx.as_mut_ptr(), digest.as_mut_ptr());
    }
    s.hash.copy_from_slice(&digest);
    s.folded = s.folded.wrapping_add(1);
    zeroize(&mut digest);
    zeroize(&mut ctx);
    R_OK
}

// ============================================================================
// THE DIALECT FLOOR: Attack A's only defence.
// ============================================================================

/// Judge the dialect the server selected against the configured floor.
///
/// This fires in net/smb.c immediately after the NEGOTIATE response is parsed
/// and BEFORE any credential leaves the machine, for the same reason the
/// signing and encryption gates do: finding out that a server cannot be talked
/// to safely after sending it an NTLM authentication is finding out too late.
#[no_mangle]
pub extern "C" fn smbpreauth_floor_check(dialect: u16) -> u16 {
    if smbpreauth_floor() == FLOOR_311 && dialect != DIALECT_311 {
        return R_DIALECT_FLOOR;
    }
    R_OK
}

// ============================================================================
// Validating what the server actually selected.
// ============================================================================

/// Walk the server's NEGOTIATE response contexts and check every choice it made
/// against what this client can do.
///
/// Refusing here is not redundant with the pre-auth hash. The hash catches a
/// MITM; this catches an HONEST server whose choices we cannot implement, which
/// would otherwise show up as an unexplained decryption failure several messages
/// later. The two failures look identical on the console if you do not separate
/// them, and one is an attack while the other is a compatibility problem.
#[no_mangle]
pub extern "C" fn smbpreauth_note_negotiate(st: *mut SmbPreauth, dialect: u16,
                                            pdu: *const u8, len: usize) -> u16 {
    if st.is_null() || pdu.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live SmbPreauth and `len` readable bytes.
    let s = unsafe { &mut *st };
    if dialect != DIALECT_311 {
        // Not an error here: the floor already decided whether a lower dialect
        // is allowed. If it is allowed, there is simply no pre-auth state.
        s.active = 0;
        return R_OK;
    }
    if len < NEGOTIATE_RESP_MIN { return R_SHORT; }
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    if rd_u32(p, OFF_MAGIC) != SMB2_MAGIC { return R_NOT_SMB2; }

    let count = rd_u16(p, OFF_R_NCTX_COUNT) as usize;
    let off = rd_u32(p, OFF_R_NCTX_OFFSET) as usize;
    if count == 0 { return R_NO_PREAUTH_CONTEXT; }
    // The offset is from the START OF THE SMB2 HEADER, measured, and must land
    // inside the message with room for at least one preamble.
    if off < NEGOTIATE_RESP_MIN || off > len || len - off < 8 {
        return R_CONTEXT_MALFORMED;
    }

    let mut seen_preauth = false;
    let mut seen_enc = false;
    let mut seen_sign = false;
    let mut o = off;
    let mut n = 0usize;
    while n < count {
        if o + 8 > len { return R_CONTEXT_MALFORMED; }
        let ctype = rd_u16(p, o);
        let dlen = rd_u16(p, o + 2) as usize;
        if o + 8 + dlen > len { return R_CONTEXT_MALFORMED; }
        let d = &p[o + 8..o + 8 + dlen];
        match ctype {
            CTX_PREAUTH_INTEGRITY => {
                if seen_preauth { return R_CONTEXT_DUP; }
                seen_preauth = true;
                if dlen < 4 { return R_CONTEXT_MALFORMED; }
                let hcount = rd_u16(d, 0) as usize;
                let slen = rd_u16(d, 2) as usize;
                // The server echoes exactly ONE hash algorithm: its choice.
                if hcount != 1 { return R_HASH_ALGO; }
                if 4 + 2 + slen > dlen { return R_CONTEXT_MALFORMED; }
                if rd_u16(d, 4) != HASH_SHA512 { return R_HASH_ALGO; }
                // A zero-length or absurd salt is not something to shrug at:
                // the salt is an input to the hash on the server side.
                if slen == 0 || slen > 512 { return R_SALT; }
                s.hash_alg_ok = 1;
            }
            CTX_ENCRYPTION => {
                if seen_enc { return R_CONTEXT_DUP; }
                seen_enc = true;
                if dlen < 4 { return R_CONTEXT_MALFORMED; }
                if rd_u16(d, 0) != 1 { return R_CIPHER_NOT_OFFERED; }
                let c = rd_u16(d, 2);
                if c != CIPHER_AES128_CCM { return R_CIPHER_NOT_OFFERED; }
                s.cipher = c;
            }
            CTX_SIGNING => {
                if seen_sign { return R_CONTEXT_DUP; }
                seen_sign = true;
                if dlen < 4 { return R_CONTEXT_MALFORMED; }
                if rd_u16(d, 0) != 1 { return R_SIGNALG_NOT_OFFERED; }
                let a = rd_u16(d, 2);
                if a != SIGNALG_AES_CMAC { return R_SIGNALG_NOT_OFFERED; }
                s.signalg = a;
            }
            // Not acted on, and that is a deliberate, stated choice rather
            // than an oversight: these contexts are still folded into the
            // running hash like every other handshake byte, so none of them can
            // be edited by a MITM undetected. We simply negotiate none of the
            // features they describe.
            CTX_COMPRESSION | CTX_NETNAME | CTX_TRANSPORT | CTX_RDMA_TRANSFORM => {}
            _ => {}
        }
        // Contexts are 8-byte aligned BETWEEN one another; the last is not
        // padded, which is why this advance may run past `len` on the final
        // iteration and must not be bounds-checked here.
        o = (o + 8 + dlen + 7) & !7usize;
        n += 1;
    }
    if !seen_preauth { return R_NO_PREAUTH_CONTEXT; }
    s.active = 1;
    R_OK
}

// ============================================================================
// THE 3.1.1 KEY SCHEDULE.
// ============================================================================

/// Derive the SMB 3.1.1 session keys from the session key and the running
/// pre-auth hash, and FREEZE the hash so nothing can fold into it afterwards.
///
/// Writes 16 bytes to each non-null out pointer. The application key is derived
/// and returned for completeness even though nothing in this kernel consumes it
/// yet; deriving three of four and leaving a gap would invite someone to guess
/// the fourth label later.
#[no_mangle]
pub extern "C" fn smbpreauth_derive(st: *mut SmbPreauth,
                                    session_key: *const u8,
                                    out_sign: *mut u8,
                                    out_enc: *mut u8,
                                    out_dec: *mut u8,
                                    out_app: *mut u8) -> u16 {
    if st.is_null() || session_key.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live SmbPreauth, 16 readable bytes at
    // session_key, and 16 writable bytes at each non-null out pointer.
    let s = unsafe { &mut *st };
    if s.active == 0 { return R_NOT_ACTIVE; }
    let sk: &[u8] = unsafe { core::slice::from_raw_parts(session_key, 16) };

    // An all-zero session key is an anonymous or guest session. There is
    // nothing to key a handshake-integrity check with, so say so by name
    // rather than deriving a key everybody can compute.
    let mut any = 0u8;
    let mut i = 0;
    while i < 16 { any |= sk[i]; i += 1; }
    if any == 0 { return R_NO_SESSION_KEY; }

    let mut rc = R_OK;
    for (label, outp) in [(LBL_SIGNING, out_sign), (LBL_C2S, out_enc),
                          (LBL_S2C, out_dec), (LBL_APP, out_app)] {
        if outp.is_null() { continue; }
        let mut k = [0u8; 16];
        // SAFETY: smbsign_kdf128_ctx reads 16 bytes at sk, label.len() bytes at
        // the label and 64 at the hash, and writes exactly 16 bytes to k.
        let r = unsafe {
            smbsign_kdf128_ctx(sk.as_ptr(), label.as_ptr(), label.len(),
                               s.hash.as_ptr(), PREAUTH_HASH_LEN,
                               k.as_mut_ptr())
        };
        if r != 0 {
            zeroize(&mut k);
            rc = R_KDF;
            break;
        }
        // SAFETY: outp was null-checked and the caller guarantees 16 bytes.
        unsafe { core::ptr::copy_nonoverlapping(k.as_ptr(), outp, 16) };
        zeroize(&mut k);
    }
    if rc == R_OK {
        s.frozen = 1;
    }
    rc
}

/// Record that the final SESSION_SETUP response verified under a key derived
/// from this hash. Until this is called, the hash has been COMPUTED but never
/// COMPARED, and a computed-but-never-compared integrity value is exactly the
/// shape of a control that looks present and does nothing.
#[no_mangle]
pub extern "C" fn smbpreauth_note_verified(st: *mut SmbPreauth) -> u16 {
    if st.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbPreauth.
    let s = unsafe { &mut *st };
    if s.active == 0 { return R_NOT_ACTIVE; }
    s.verified = 1;
    R_OK
}

// ============================================================================
// SELF-TEST. Runs on EVERY boot, including the golden.
// ============================================================================
//
// Returns 0 on pass, or the number of the first failing case. The numbers are
// stable so a failure on a console says which property broke.
//
// TWO INDEPENDENT ORACLES, because one is not enough for a key schedule:
//
//   The SYN_* vectors are a SYNTHETIC three-message chain. They pin the hash
//   SEEDING (sixty-four zero bytes) and the FOLD FORM (H = SHA512(H || msg)),
//   computed by an independent Python implementation. If the seeding were, say,
//   an empty hash instead of 64 zeros, these fail.
//
//   The REAL_* vectors come from a session SAMBA 4.17.12 ITSELF SIGNED on
//   2026-10-07. The pre-auth hash and session key were recovered from a packet
//   capture of a real smbclient mount plus the account password, and the four
//   derived keys were confirmed by checking the AES-CMAC signature Samba put on
//   the final SESSION_SETUP response. If any KDF LABEL here is wrong, these
//   fail. A synthetic vector cannot catch a wrong label, because a synthetic
//   vector is computed with the same label it is checking.
//
//   Recovering those values needed one step that is easy to miss and worth
//   recording: a real smbclient sets NTLMSSP_NEGOTIATE_KEY_EXCH, so its SMB3
//   session key is NOT the NTLMv2 SessionBaseKey but a random key RC4-wrapped
//   under it inside the AUTHENTICATE message. Deriving from the base key
//   reproduces NTProofStr correctly, looks entirely successful, and then every
//   later check fails for no visible reason. MayteraOS's own client does not
//   set that flag, so for this kernel the two keys are the same.
//
// Case 23 is the self-test form of the destructive proof: it folds the same
// chain with ONE BIT flipped in the first message and requires the derived
// signing key to COME OUT DIFFERENT. A pre-auth hash that did not change the
// keys would pass every other case here and protect nothing.
#[no_mangle]
pub extern "C" fn smbpreauth_selftest_rs() -> u32 {
    const SYN_M1: [u8; 72] = [
        0xfe, 0x53, 0x4d, 0x42, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
        0x4c, 0x4d, 0x4e, 0x4f, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    ];
    const SYN_M2: [u8; 80] = [
        0xfe, 0x53, 0x4d, 0x42, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
        0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
        0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b,
        0x7c, 0x7d, 0x7e, 0x7f, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
        0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
    ];
    const SYN_M3: [u8; 64] = [
        0xfe, 0x53, 0x4d, 0x42, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
        0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3,
        0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
        0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb,
        0xbc, 0xbd, 0xbe, 0xbf,
    ];
    const SYN_HASH: [u8; 64] = [
        0xc7, 0x62, 0x77, 0xd0, 0xfd, 0xe7, 0xe8, 0xd2, 0xc0, 0xb5, 0x44, 0x12,
        0x83, 0x22, 0x90, 0x8c, 0x0b, 0xcc, 0x65, 0xd5, 0x8f, 0x7b, 0x00, 0xa4,
        0x6a, 0xf8, 0xee, 0x89, 0x27, 0xfa, 0x01, 0xd9, 0xd2, 0x7e, 0x3d, 0x5b,
        0x56, 0xea, 0x3d, 0xeb, 0x19, 0x1f, 0x1e, 0x0d, 0x10, 0x2a, 0x19, 0x5b,
        0xd3, 0x31, 0xbb, 0x1d, 0x73, 0xc4, 0xb8, 0x1b, 0x5a, 0x8a, 0x37, 0x4a,
        0x40, 0x3a, 0xe0, 0xa9,
    ];
    const SYN_SESSION_KEY: [u8; 16] = [
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f,
    ];
    const SYN_SIGN: [u8; 16] = [
        0xc7, 0xec, 0x1a, 0xfa, 0x6a, 0x13, 0x34, 0x10, 0x8f, 0x66, 0x9f, 0x98,
        0x79, 0xcf, 0x2c, 0x25,
    ];
    const SYN_C2S: [u8; 16] = [
        0x4a, 0xc9, 0xcf, 0xc9, 0x94, 0x7c, 0x00, 0x23, 0x88, 0x37, 0x3e, 0xb9,
        0x0e, 0x40, 0xe9, 0xba,
    ];
    const SYN_S2C: [u8; 16] = [
        0x52, 0xf3, 0x9e, 0xae, 0x50, 0xb8, 0x7b, 0x74, 0xb3, 0x37, 0xe5, 0x7e,
        0xa0, 0x2a, 0xc1, 0xbf,
    ];
    const SYN_APP: [u8; 16] = [
        0x85, 0x37, 0x3a, 0x22, 0x44, 0x3f, 0x93, 0x39, 0xa3, 0x35, 0x64, 0x44,
        0xe3, 0xaf, 0x0c, 0x52,
    ];
    const REAL_SESSION_KEY: [u8; 16] = [
        0x02, 0x7c, 0x4d, 0xcf, 0xee, 0x11, 0x23, 0x57, 0x69, 0xa3, 0x58, 0x9b,
        0xd2, 0xc3, 0xc0, 0xe6,
    ];
    const REAL_PREAUTH: [u8; 64] = [
        0x5c, 0x2d, 0x44, 0x13, 0x31, 0xef, 0x37, 0x0b, 0x72, 0x8b, 0x9c, 0xc1,
        0x1d, 0x06, 0x0d, 0x56, 0x58, 0x05, 0x1e, 0x0e, 0x19, 0x76, 0xfd, 0xb1,
        0xe0, 0x7e, 0x00, 0x2c, 0xe7, 0x1a, 0x6c, 0xee, 0xae, 0x25, 0x10, 0x83,
        0xe4, 0xd8, 0xc4, 0xad, 0xf3, 0xa1, 0xf9, 0xed, 0x9b, 0xf6, 0x0e, 0x38,
        0xec, 0xcb, 0xb4, 0x68, 0xb5, 0xe3, 0x3f, 0x67, 0xe4, 0xbf, 0x50, 0x00,
        0xf3, 0xf2, 0x96, 0xef,
    ];
    const REAL_SIGN: [u8; 16] = [
        0xdb, 0x81, 0x53, 0xde, 0xba, 0x02, 0xc3, 0x20, 0x48, 0x8b, 0x3c, 0x40,
        0x0a, 0x41, 0xe8, 0x8a,
    ];
    const REAL_C2S: [u8; 16] = [
        0x41, 0x43, 0x61, 0x92, 0x31, 0xd7, 0xd2, 0xa6, 0x2d, 0x1c, 0x5c, 0xbb,
        0x6f, 0xdf, 0x08, 0x9b,
    ];
    const REAL_S2C: [u8; 16] = [
        0x19, 0xce, 0xe6, 0x07, 0x37, 0xa7, 0x79, 0xbd, 0xd6, 0xbc, 0x51, 0x80,
        0xfa, 0xfd, 0xeb, 0x5f,
    ];
    const REAL_APP: [u8; 16] = [
        0x47, 0x28, 0x8b, 0xbc, 0x5e, 0xe1, 0x68, 0x7d, 0xa7, 0xc2, 0x57, 0xa0,
        0xde, 0xfb, 0xa7, 0x27,
    ];
    const CTX_SALT: [u8; 32] = [
        0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb,
        0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
        0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    ];
    const CTX_EXPECT: [u8; 76] = [
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
        0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5,
        0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_GOOD: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_NO_PREAUTH: [u8; 172] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_GCM: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_GMAC: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x02, 0x00,
    ];
    const NR_BADHASH: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_BADOFF: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0xff, 0xff, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_DUP: [u8; 236] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    ];
    const NR_302: [u8; 220] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x02, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
    ];
    const NR_ZEROSALT: [u8; 188] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x11, 0x03, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x10, 0x00, 0x90, 0x00, 0x00, 0x00, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    ];

    let saved_floor = smbpreauth_floor();
    let mut st = SMBPREAUTH_ZERO;

    // --- 1..4: the dialect floor, Attack A's only defence -------------------
    smbpreauth_floor_set(FLOOR_311);
    if smbpreauth_floor() != FLOOR_311 { return 1; }
    if smbpreauth_floor_check(DIALECT_311) != R_OK { return 2; }
    if smbpreauth_floor_check(DIALECT_302) != R_DIALECT_FLOOR { return 3; }
    if smbpreauth_floor_check(DIALECT_202) != R_DIALECT_FLOOR { return 4; }

    // 5..6: an unrecognised value must resolve to the STRONGER setting.
    smbpreauth_floor_set(0xDEAD);
    if smbpreauth_floor() != FLOOR_311 { return 5; }
    smbpreauth_floor_set(FLOOR_NONE);
    if smbpreauth_floor_check(DIALECT_302) != R_OK { return 6; }
    smbpreauth_floor_set(FLOOR_311);

    // 7: the ABI lock against net/smb.h's macros.
    if smbpreauth_floor_abi_check(FLOOR_NONE, FLOOR_311) != 0 { return 7; }
    if smbpreauth_floor_abi_check(9, FLOOR_311) == 0 { return 7; }

    // --- 8: the config parser ---------------------------------------------
    {
        let cases: [(&[u8], i32); 9] = [
            (b"min_dialect=3.1.1\n", 1),
            (b"MIN_DIALECT = 3.1.1\n", 1),
            (b"min_dialect=311\n", 1),
            (b"min_dialect=3.0.2\n", 0),
            (b"min_dialect=302  # allow the old behaviour\n", 0),
            (b"# min_dialect=3.0.2\nsigning=required\n", -1),
            (b"signing=required\nencrypt=enabled\n", -1),
            (b"min_dialect=banana\n", -2),
            (b"", -1),
        ];
        let mut i = 0;
        while i < cases.len() {
            let (buf, want) = cases[i];
            let got = smbpreauth_floor_parse(buf.as_ptr(), buf.len() as i32);
            if got != want { return 8; }
            i += 1;
        }
        if smbpreauth_floor_parse(core::ptr::null(), 4) != -1 { return 8; }
    }

    // --- 9..10: building the request contexts ------------------------------
    {
        smbpreauth_reset(&mut st);
        let mut out = [0u8; 128];
        let mut pad: u32 = 0xFF;
        let mut cnt: u16 = 0;
        // base_len 110 is the real shape: 64 header + 36 body + 10 dialects
        // (five dialects at 3.1.1), which needs 2 pad bytes to reach 112.
        let n = smbpreauth_build_contexts(&mut st, CTX_SALT.as_ptr(), 32, 110,
                                          out.as_mut_ptr(), out.len(),
                                          &mut pad, &mut cnt);
        if n != CTX_EXPECT.len() as i32 { return 9; }
        if pad != 2 || cnt != 3 { return 9; }
        if !eq(&out[0..CTX_EXPECT.len()], &CTX_EXPECT) { return 9; }
        if st.offered != 1 { return 9; }
        if !eq(&st.salt, &CTX_SALT) { return 9; }
        // A buffer too small must be REFUSED, not truncated: a half-written
        // context is a handshake the server will reject for the wrong reason.
        let mut tiny = [0u8; 40];
        if smbpreauth_build_contexts(&mut st, CTX_SALT.as_ptr(), 32, 110,
                                     tiny.as_mut_ptr(), tiny.len(),
                                     &mut pad, &mut cnt)
            != -(R_NO_ROOM as i32) { return 10; }
        // A wrong salt length must be refused rather than zero-padded.
        if smbpreauth_build_contexts(&mut st, CTX_SALT.as_ptr(), 16, 110,
                                     out.as_mut_ptr(), out.len(),
                                     &mut pad, &mut cnt)
            != -(R_SALT as i32) { return 10; }
    }

    // --- 11..13: the running hash -----------------------------------------
    {
        smbpreauth_reset(&mut st);
        st.active = 1;
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), SYN_M1.len()) != R_OK { return 11; }
        if smbpreauth_fold(&mut st, SYN_M2.as_ptr(), SYN_M2.len()) != R_OK { return 11; }
        if smbpreauth_fold(&mut st, SYN_M3.as_ptr(), SYN_M3.len()) != R_OK { return 11; }
        if !eq(&st.hash, &SYN_HASH) { return 11; }
        if st.folded != 3 { return 11; }

        // 12: refusals. A short message, a non-SMB2 message, a null pointer.
        let short = [0xFEu8, 0x53, 0x4D, 0x42, 0x00];
        if smbpreauth_fold(&mut st, short.as_ptr(), short.len()) != R_SHORT { return 12; }
        let mut notsmb = [0u8; 64];
        notsmb[0] = 0xAA;
        if smbpreauth_fold(&mut st, notsmb.as_ptr(), 64) != R_NOT_SMB2 { return 12; }
        if smbpreauth_fold(core::ptr::null_mut(), SYN_M1.as_ptr(), 72) != R_NULL { return 12; }
        // A refused fold must not have moved the hash or the counter.
        if !eq(&st.hash, &SYN_HASH) || st.folded != 3 { return 12; }

        // 13: a sixth fold, and a fold after the keys are frozen, are both
        // refused. Either would silently change a hash nothing re-reads.
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), 72) != R_OK { return 13; }
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), 72) != R_OK { return 13; }
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), 72) != R_TOO_MANY_FOLDS { return 13; }
        st.folded = 1;
        st.frozen = 1;
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), 72) != R_FROZEN { return 13; }
    }

    // --- 14..16: the 3.1.1 key schedule, SYNTHETIC oracle ------------------
    {
        smbpreauth_reset(&mut st);
        st.active = 1;
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), SYN_M1.len()) != R_OK { return 14; }
        if smbpreauth_fold(&mut st, SYN_M2.as_ptr(), SYN_M2.len()) != R_OK { return 14; }
        if smbpreauth_fold(&mut st, SYN_M3.as_ptr(), SYN_M3.len()) != R_OK { return 14; }
        let mut sgn = [0u8; 16];
        let mut enc = [0u8; 16];
        let mut dec = [0u8; 16];
        let mut app = [0u8; 16];
        if smbpreauth_derive(&mut st, SYN_SESSION_KEY.as_ptr(), sgn.as_mut_ptr(),
                             enc.as_mut_ptr(), dec.as_mut_ptr(), app.as_mut_ptr())
            != R_OK { return 14; }
        if !eq(&sgn, &SYN_SIGN) { return 14; }
        if !eq(&enc, &SYN_C2S) { return 15; }
        if !eq(&dec, &SYN_S2C) { return 15; }
        if !eq(&app, &SYN_APP) { return 15; }
        // Deriving FREEZES the hash.
        if st.frozen != 1 { return 16; }
        if smbpreauth_fold(&mut st, SYN_M1.as_ptr(), 72) != R_FROZEN { return 16; }
    }

    // --- 17..18: the key schedule, REAL SAMBA-SIGNED oracle ----------------
    //
    // This is the case that pins the four KDF LABELS to a real server. The
    // synthetic case above cannot: it computes its expected answer with the
    // same label string it is checking, so a wrong label passes it.
    {
        smbpreauth_reset(&mut st);
        st.active = 1;
        st.hash.copy_from_slice(&REAL_PREAUTH);
        let mut sgn = [0u8; 16];
        let mut enc = [0u8; 16];
        let mut dec = [0u8; 16];
        let mut app = [0u8; 16];
        if smbpreauth_derive(&mut st, REAL_SESSION_KEY.as_ptr(), sgn.as_mut_ptr(),
                             enc.as_mut_ptr(), dec.as_mut_ptr(), app.as_mut_ptr())
            != R_OK { return 17; }
        if !eq(&sgn, &REAL_SIGN) { return 17; }
        if !eq(&enc, &REAL_C2S) { return 18; }
        if !eq(&dec, &REAL_S2C) { return 18; }
        if !eq(&app, &REAL_APP) { return 18; }
    }

    // --- 19: derive refuses an anonymous session and an inactive state -----
    {
        smbpreauth_reset(&mut st);
        st.active = 1;
        st.hash.copy_from_slice(&REAL_PREAUTH);
        let zero = [0u8; 16];
        let mut sgn = [0u8; 16];
        if smbpreauth_derive(&mut st, zero.as_ptr(), sgn.as_mut_ptr(),
                             core::ptr::null_mut(), core::ptr::null_mut(),
                             core::ptr::null_mut()) != R_NO_SESSION_KEY { return 19; }
        if st.frozen != 0 { return 19; }
        smbpreauth_reset(&mut st);
        if smbpreauth_derive(&mut st, REAL_SESSION_KEY.as_ptr(), sgn.as_mut_ptr(),
                             core::ptr::null_mut(), core::ptr::null_mut(),
                             core::ptr::null_mut()) != R_NOT_ACTIVE { return 19; }
    }

    // --- 20..22: validating what the server selected -----------------------
    {
        smbpreauth_reset(&mut st);
        if smbpreauth_note_negotiate(&mut st, DIALECT_311, NR_GOOD.as_ptr(),
                                     NR_GOOD.len()) != R_OK { return 20; }
        if st.active != 1 || st.hash_alg_ok != 1 { return 20; }
        if st.cipher != CIPHER_AES128_CCM { return 20; }
        if st.signalg != SIGNALG_AES_CMAC { return 20; }

        // 21: every refusal arm, each with its own code so a console line
        // names the actual problem instead of saying "negotiate failed".
        let arms: [(&[u8], u16); 6] = [
            (&NR_NO_PREAUTH, R_NO_PREAUTH_CONTEXT),
            (&NR_GCM,        R_CIPHER_NOT_OFFERED),
            (&NR_GMAC,       R_SIGNALG_NOT_OFFERED),
            (&NR_BADHASH,    R_HASH_ALGO),
            (&NR_BADOFF,     R_CONTEXT_MALFORMED),
            (&NR_DUP,        R_CONTEXT_DUP),
        ];
        let mut i = 0;
        while i < arms.len() {
            let (buf, want) = arms[i];
            smbpreauth_reset(&mut st);
            let got = smbpreauth_note_negotiate(&mut st, DIALECT_311,
                                                buf.as_ptr(), buf.len());
            if got != want { return 21; }
            // A refused negotiate must NEVER leave the state active, or the
            // mount would carry on with a hash the server does not share.
            if st.active != 0 { return 21; }
            i += 1;
        }
        smbpreauth_reset(&mut st);
        if smbpreauth_note_negotiate(&mut st, DIALECT_311, NR_ZEROSALT.as_ptr(),
                                     NR_ZEROSALT.len()) != R_SALT { return 21; }

        // 22: a lower dialect leaves the state INACTIVE and does not error.
        // Whether that lower dialect is acceptable is the floor's decision,
        // kept deliberately separate so "we did not ask" can never pass
        // itself off as "it was not available".
        smbpreauth_reset(&mut st);
        if smbpreauth_note_negotiate(&mut st, DIALECT_302, NR_302.as_ptr(),
                                     NR_302.len()) != R_OK { return 22; }
        if st.active != 0 { return 22; }
        if smbpreauth_note_verified(&mut st) != R_NOT_ACTIVE { return 22; }
        // A truncated response must be refused, not read past.
        smbpreauth_reset(&mut st);
        if smbpreauth_note_negotiate(&mut st, DIALECT_311, NR_GOOD.as_ptr(), 100)
            != R_SHORT { return 22; }
    }

    // --- 23: THE DESTRUCTIVE CASE -----------------------------------------
    //
    // Fold the same handshake with ONE BIT flipped in the first message and
    // require EVERY derived key to come out different. This is the whole point
    // of the module: a pre-auth hash that did not change the keys would pass
    // all 22 cases above and protect nothing at all.
    {
        let mut tampered = [0u8; 72];
        tampered.copy_from_slice(&SYN_M1);
        tampered[40] ^= 0x01;
        smbpreauth_reset(&mut st);
        st.active = 1;
        if smbpreauth_fold(&mut st, tampered.as_ptr(), tampered.len()) != R_OK { return 23; }
        if smbpreauth_fold(&mut st, SYN_M2.as_ptr(), SYN_M2.len()) != R_OK { return 23; }
        if smbpreauth_fold(&mut st, SYN_M3.as_ptr(), SYN_M3.len()) != R_OK { return 23; }
        // The hash must differ from the honest one.
        if eq(&st.hash, &SYN_HASH) { return 23; }
        let mut sgn = [0u8; 16];
        let mut enc = [0u8; 16];
        let mut dec = [0u8; 16];
        if smbpreauth_derive(&mut st, SYN_SESSION_KEY.as_ptr(), sgn.as_mut_ptr(),
                             enc.as_mut_ptr(), dec.as_mut_ptr(),
                             core::ptr::null_mut()) != R_OK { return 23; }
        // ALL THREE keys must differ from the honest ones. If the signing key
        // differed but a cipher key did not, a tampered handshake would still
        // be able to read traffic.
        if eq(&sgn, &SYN_SIGN) { return 23; }
        if eq(&enc, &SYN_C2S) { return 23; }
        if eq(&dec, &SYN_S2C) { return 23; }
    }

    // --- 24: reset leaves nothing behind -----------------------------------
    {
        st.hash.copy_from_slice(&REAL_PREAUTH);
        st.salt.copy_from_slice(&CTX_SALT);
        st.active = 1; st.frozen = 1; st.verified = 1; st.folded = 5;
        st.cipher = CIPHER_AES128_CCM; st.signalg = SIGNALG_AES_CMAC;
        smbpreauth_reset(&mut st);
        let mut i = 0;
        while i < PREAUTH_HASH_LEN { if st.hash[i] != 0 { return 24; } i += 1; }
        i = 0;
        while i < SALT_LEN { if st.salt[i] != 0 { return 24; } i += 1; }
        if st.active != 0 || st.frozen != 0 || st.verified != 0 { return 24; }
        if st.folded != 0 { return 24; }
        if st.cipher != NO_CONTEXT || st.signalg != NO_CONTEXT { return 24; }
        if smbpreauth_is_active(&st) != 0 { return 24; }
        if smbpreauth_is_verified(&st) != 0 { return 24; }
    }

    // --- 25: the cipher ids this kernel knows are DISTINCT ------------------
    // A copy-paste that made CCM and GCM the same number would let a GCM
    // server through the cipher gate and then fail at decryption time.
    if CIPHER_AES128_CCM == CIPHER_AES128_GCM { return 25; }
    if CIPHER_AES256_CCM == CIPHER_AES128_CCM { return 25; }
    if CIPHER_AES256_GCM == CIPHER_AES128_GCM { return 25; }
    if SIGNALG_AES_CMAC == SIGNALG_AES_GMAC { return 25; }
    if SIGNALG_HMAC_SHA256 == SIGNALG_AES_CMAC { return 25; }

    smbpreauth_floor_set(saved_floor);
    0
}

fn eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() { return false; }
    let mut i = 0;
    while i < a.len() {
        if a[i] != b[i] { return false; }
        i += 1;
    }
    true
}
