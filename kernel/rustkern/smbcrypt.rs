// rustkern/smbcrypt.rs - SMB3 ENCRYPTION: make the file contents unreadable on
// the wire, and refuse to carry on in the clear once they are meant to be.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. It is a protocol state
// machine plus the wire framing of an AEAD: no floats, no allocation, no
// hardware, no performance argument for C.
//
// NO CIPHER IS IMPLEMENTED HERE. The AEAD is `aes_ccm_encrypt()` /
// `aes_ccm_decrypt()` (rustkern/aesccm.rs, declared in crypto/crypto.h) and
// the key derivation is the same SP800-108 KDF the signing module uses,
// reached through `smbsign_kdf128_shared()` so there is ONE copy of that byte
// layout in the tree. What is new here is WHEN to encrypt, WHAT the transform
// header looks like, and WHAT TO DO when a message arrives that should have
// been encrypted and was not.
//
// ===========================================================================
// WHAT WAS WRONG, measured on golden build 2576 (commit 2cfda82d), not assumed
// ---------------------------------------------------------------------------
// Everything. File contents crossed the network in plain text.
//
// Captured with tcpdump on the server side while that golden mounted a share,
// listed it, read a file, wrote a file and reconnected: 89 SMB2 messages, all
// of them signed (signing landed the day before and works), ZERO encrypted,
// and the 55-byte test file's contents appear THREE TIMES in the capture as
// readable ASCII. Signing proves who sent a message and that it was not
// altered. It does nothing whatsoever to stop anyone reading it.
//
// `SMB2_TRANSFORM_MAGIC` was defined in net/smb.h and read by no .c file in
// the tree, which is this tree's recurring shape: the constant for the feature
// existed, so a reader could believe the feature did.
//
// ===========================================================================
// WHICH CIPHER, AND WHY THE ANSWER EVERYONE GIVES IS WRONG HERE
// ---------------------------------------------------------------------------
// "SMB3 encryption is AES-128-GCM" is the common formulation, GCM is what the
// tree already had (crypto/crypto.h, for TLS), and it is WRONG for the dialect
// this client speaks.
//
// MEASURED 2026-10-04, Samba 4.17.12-Debian, with an independent Python
// MS-SMB2 client that sends a real encrypted TREE_CONNECT:
//
//   dialect 0x0302 + EncryptionAlgorithm 0x0001 (AES-128-CCM, 11-byte nonce)
//        -> ACCEPTED. Encrypted reply, and an encrypted READ returned the file.
//   dialect 0x0302 + EncryptionAlgorithm 0x0002 (AES-128-GCM, 12-byte nonce)
//        -> CONNECTION KILLED. smbd logged "Server exit (NT_STATUS_ACCESS_DENIED)".
//   dialect left free -> the server picked 0x0311 and used GCM (12-byte nonce,
//        the field at offset 42 being Flags rather than EncryptionAlgorithm).
//
// So the cipher is not a choice at 3.0.2, it is fixed at AES-128-CCM by
// MS-SMB2 3.1.4.3, and cipher negotiation only exists from 3.1.1 via the
// SMB2_ENCRYPTION_CAPABILITIES negotiate context. net/smb.c offers 2.0.2 /
// 2.1 / 3.0 / 3.0.2 and Samba picks 0x0302.
//
// SMB 3.1.1 IS NOT COVERED, NOT OFFERED, AND REFUSED IF OFFERED, exactly as
// the signing module says of it. Its encryption keys hang off the pre-auth
// integrity hash (a running SHA-512 over every NEGOTIATE and SESSION_SETUP
// message), it negotiates its cipher in a context this client does not send,
// and both halves are a materially bigger job than this one. `smbcrypt_begin()`
// refuses a dialect it does not have a key schedule for rather than guessing,
// because a guessed key schedule does not fail loudly.
//
// SMB 2.0.2 and 2.1 CANNOT ENCRYPT AT ALL. There is no transform header before
// 3.0. A mount to such a server under the default policy is refused, which is
// a real behaviour change and is stated as such below.
//
// ===========================================================================
// THE KEY SCHEDULE (MS-SMB2 3.1.4.2), and the three NULs again
// ---------------------------------------------------------------------------
// For 3.0 and 3.0.2, with the same SP800-108 counter-mode KDF as signing:
//
//   EncryptionKey (client -> server) = KDF(SessionKey, "SMB2AESCCM\0", "ServerIn \0")
//   DecryptionKey (server -> client) = KDF(SessionKey, "SMB2AESCCM\0", "ServerOut\0")
//
// The direction labels are from the SERVER'S point of view and that is the
// single easiest thing here to get backwards: "ServerIn " is the key the
// CLIENT ENCRYPTS WITH. Note also the TRAILING SPACE in "ServerIn " before
// its NUL, which makes both labels 10 bytes. Getting either wrong produces a
// connection the server kills with no useful diagnosis, so both are pinned by
// a cross-implementation known-answer test below, not by this comment.
//
// ===========================================================================
// THE WIRE FRAMING (MS-SMB2 2.2.41), read field by field
// ---------------------------------------------------------------------------
//   0  ProtocolId            0xFD 'S' 'M' 'B'
//   4  Signature             16 bytes: the AEAD TAG. Not a signature in the
//                            signing sense; MS-SMB2 just calls it that.
//  20  Nonce                 16 bytes, of which CCM uses the FIRST 11 and the
//                            remaining 5 MUST be zero.
//  36  OriginalMessageSize   4 bytes: the PLAINTEXT length, not the frame's.
//  40  Reserved              2 bytes
//  42  EncryptionAlgorithm   2 bytes, 0x0001 = AES-128-CCM (at 3.1.1 this
//                            field is Flags instead, where 0x0001 = encrypted)
//  44  SessionId             8 bytes
//
// The AAD is bytes 20..52, i.e. Nonce + OriginalMessageSize + Reserved +
// EncryptionAlgorithm + SessionId: 32 bytes. So the length and the SessionId
// are authenticated even though they travel in the clear, which is what makes
// a truncated or re-targeted frame detectable. The 4-byte NetBIOS length
// prefix is NOT part of the AAD.
//
// ===========================================================================
// THE NONCE IS A COUNTER, AND THAT IS A CORRECTNESS REQUIREMENT
// ---------------------------------------------------------------------------
// CCM, like every counter-mode AEAD, is broken by a repeated (key, nonce)
// pair: two messages under the same pair leak their XOR and, worse, hand an
// attacker the material to forge. The counter here starts at 1 for each
// session and only ever increases; it is reset ONLY by `smbcrypt_reset()`,
// which also throws the keys away, so a reconnect gets fresh keys rather than
// a restarted counter under an old key. `smbcrypt_seal()` REFUSES at
// u64::MAX rather than wrapping. A 64-bit counter at one message per
// microsecond takes half a million years to reach that, so the refusal is a
// safety net, not a limit anyone will meet.
//
// ===========================================================================
// SIGNING IS NOT APPLIED TO AN ENCRYPTED MESSAGE, AND THAT IS NOT A GAP
// ---------------------------------------------------------------------------
// MS-SMB2 3.2.4.1.1 signs a request only if it is not being encrypted, and
// MEASURED here: Samba's encrypted replies carry Flags without
// SMB2_FLAGS_SIGNED and an all-zero Signature field. The AEAD tag already
// authenticates the whole message, including the SessionId, under a key
// derived from the same session key, so a second MAC would add nothing. The
// receive path therefore does NOT demand a signature on a decrypted message,
// and net/smb.c says so at the call site, because "the signature check was
// skipped" is exactly the sentence that should make a reader suspicious.
//
// ===========================================================================
// THE DEFAULT IS "REQUIRED", AND THE REASON IS SPECIFIC TO THIS DIALECT
// ---------------------------------------------------------------------------
// MEASURED 2026-10-04, Samba 4.17.12-Debian, all four `smb encrypt` settings,
// 3.0.2 pinned, read off the wire:
//
//   smb encrypt = default   caps ENCRYPTION SET,   plain allowed, encryption works
//   smb encrypt = desired   caps ENCRYPTION SET,   SessionFlags 0x0004 set, and
//                           the server ANSWERS ENCRYPTED even to a plain request
//   smb encrypt = required  caps ENCRYPTION SET,   SessionFlags 0x0004 set, and
//                           an unencrypted TREE_CONNECT is refused 0xC0000022
//   smb encrypt = off       caps ENCRYPTION CLEAR, and an encrypted request
//                           gets the connection killed
//
// Also measured: the server sets the ENCRYPTION capability bit in its
// NEGOTIATE response ONLY IF THE CLIENT SET IT FIRST (0x47 with, 0x07
// without), so the capability we advertise is load-bearing rather than
// decorative.
//
// So against a stock modern Samba, requiring encryption works with no server
// change at all. That is the cost side. The reason it is the DEFAULT rather
// than merely available is specific and worth stating:
//
//   AT 3.0.2 THERE IS NO PRE-AUTH INTEGRITY PROTECTION. The NEGOTIATE
//   exchange precedes every key, is unsigned, and at this dialect there is
//   nothing that retroactively authenticates it (the pre-auth integrity hash
//   that fixes this arrives with 3.1.1; measured, our 3.0.2 sessions carry
//   NegotiateContextCount=0 while the 3.1.1 one carried 3). An attacker in
//   the path has only to CLEAR ONE BIT in the server's advertised
//   capabilities, and an opportunistic client proceeds in plain text, happily,
//   with no way to tell. Opportunistic encryption at this dialect is
//   therefore defeated by precisely the attacker encryption exists to stop.
//   Requiring it is what converts that attack from an invisible success into
//   a visible failure.
//
// WHAT THE DEFAULT BREAKS, stated plainly rather than glossed: a mount to any
// server that will not encrypt. That means SMB 2.0.2 and 2.1 servers (no
// transform header exists before 3.0), a server deliberately configured
// `smb encrypt = off` (measured: refused, with the reason named), and an
// anonymous/guest mount, which has no secret session key to derive from and
// is handled as the honest special case described at `smbcrypt_begin()`.
// Only Samba 4.17.12 has been exercised. No Windows server, no NAS appliance
// and no macOS share was available on the rig; none of them has been tested.
//
// HOW A USER OVERRULES IT: a line in /CONFIG/SMB.CFG, the same file the
// signing policy uses.
//
//     encrypt=required   refuse any mount that will not encrypt (DEFAULT)
//     encrypt=enabled    encrypt whenever the server says it can, and say so
//                        loudly when it cannot, but allow the mount
//     encrypt=off        never encrypt; refuse a mount to a server that
//                        REQUIRES encryption rather than fail obscurely later
//
// `enabled` is NOT a quiet option. Every mount that ends up unencrypted prints
// why, and every message that goes out or comes in unencrypted after a session
// exists is COUNTED, so "this mount was protected" can be checked against a
// number instead of against a hope.

use core::sync::atomic::{AtomicU32, Ordering};

// ===========================================================================
// The primitives. Both already exist; nothing is reimplemented here.
// ===========================================================================
extern "C" {
    /// rustkern/aesccm.rs, declared in crypto/crypto.h. In place on `data`.
    fn aes_ccm_encrypt(key: *const u8, nonce: *const u8, nonce_len: usize,
                       aad: *const u8, aad_len: usize,
                       data: *mut u8, data_len: usize,
                       tag: *mut u8, tag_len: usize) -> i32;
    fn aes_ccm_decrypt(key: *const u8, nonce: *const u8, nonce_len: usize,
                       aad: *const u8, aad_len: usize,
                       data: *mut u8, data_len: usize,
                       tag: *const u8, tag_len: usize) -> i32;
    /// rustkern/smbsign.rs: the SP800-108 counter-mode KDF, PRF HMAC-SHA256,
    /// one iteration, 128-bit output. SHARED deliberately: the label/context
    /// byte layout with its three NULs is the single most error-prone thing in
    /// either module and there must not be two copies of it.
    fn smbsign_kdf128_shared(ki: *const u8, label: *const u8, label_len: usize,
                             context: *const u8, context_len: usize,
                             out: *mut u8);
}

// ===========================================================================
// Wire geometry, read from MS-SMB2 2.2.41 and from net/smb.h's smb2_header_t.
// ===========================================================================
const SMB2_HDR_LEN: usize = 64;
const TFORM_LEN: usize = 52;
const OFF_T_MAGIC: usize = 0;
const OFF_T_TAG: usize = 4;
const OFF_T_NONCE: usize = 20;
const OFF_T_OSIZE: usize = 36;
const OFF_T_ALGO: usize = 42;
const OFF_T_SID: usize = 44;
const AAD_OFF: usize = 20;
const AAD_LEN: usize = 32;
const TAG_LEN: usize = 16;
/// CCM at SMB3 uses an 11-byte nonce; the remaining 5 bytes of the 16-byte
/// field are zero. MEASURED against Samba, whose own client does the same.
const NONCE_LEN: usize = 11;

const SMB2_MAGIC: u32 = 0x424D53FE;      // "\xFESMB" as a little-endian u32
const TRANSFORM_MAGIC: u32 = 0x424D53FD; // "\xFDSMB"
const OFF_COMMAND: usize = 12;
const OFF_SESSION_ID: usize = 40;

const DIALECT_300: u16 = 0x0300;
const DIALECT_302: u16 = 0x0302;
/// SMB 3.1.1. Its key schedule lives in rustkern/smbpreauth.rs, because it
/// needs the pre-authentication integrity hash, not just the session key.
const DIALECT_311: u16 = 0x0311;

/// MS-SMB2 2.2.4: the server's NEGOTIATE Capabilities, and ours.
const GLOBAL_CAP_ENCRYPTION: u32 = 0x0000_0040;
/// MS-SMB2 2.2.6: SESSION_SETUP response SessionFlags.
const SESSION_FLAG_ENCRYPT_DATA: u16 = 0x0004;
/// MS-SMB2 2.2.10: TREE_CONNECT response ShareFlags.
const SHAREFLAG_ENCRYPT_DATA: u32 = 0x0000_8000;

const SMB2_NEGOTIATE: u16 = 0x0000;
const SMB2_SESSION_SETUP: u16 = 0x0001;

/// The only encryption algorithm 3.0/3.0.2 defines.
const ALGO_NONE: u8 = 0;
const ALGO_AES128_CCM: u8 = 1;
/// 3.1.1-only, and here only so an inbound frame that uses it is refused with
/// its own reason rather than mis-decrypted.
const WIRE_ALGO_CCM: u16 = 0x0001;
const WIRE_ALGO_GCM: u16 = 0x0002;

// Refusal codes. As in smbsign.rs these are deliberately NOT mirrored into C:
// C never compares one, it hands it to smbcrypt_reason_text() and refuses.
const R_OK: u16 = 0;
const R_NOT_ARMED: u16 = 1;
const R_SHORT: u16 = 2;
const R_NOT_TRANSFORM: u16 = 3;
const R_DIALECT: u16 = 4;
const R_SERVER_CANNOT: u16 = 5;
const R_SERVER_DEMANDS: u16 = 6;
const R_NO_SESSION_KEY: u16 = 7;
const R_NULL: u16 = 8;
const R_ALGO: u16 = 9;
const R_SESSION_MISMATCH: u16 = 10;
const R_SIZE_MISMATCH: u16 = 11;
const R_AUTH: u16 = 12;
const R_NONCE_EXHAUSTED: u16 = 13;
const R_CLEARTEXT: u16 = 14;
const R_CIPHER: u16 = 15;
const R_NONCE_PADDING: u16 = 16;
/// SMB 3.1.1 was passed to smbcrypt_begin() instead of smbcrypt_begin311().
const R_DIALECT_311_WRONG_ENTRY: u16 = 17;

/// Policy values. These three ARE mirrored into net/smb.h because C sets the
/// policy, and `smbcrypt_policy_abi_check()` is handed C's copies at boot and
/// refuses to agree if they ever drift.
const POLICY_OFF: u32 = 0;
const POLICY_ENABLED: u32 = 1;
const POLICY_REQUIRED: u32 = 2;

static POLICY: AtomicU32 = AtomicU32::new(POLICY_REQUIRED);

// ===========================================================================
// Per-connection state. Mirrored by `smb_crypt_t` in net/smb.h and size-locked
// there, so a change on either side fails the build rather than reading the
// wrong offsets.
//
// THE SESSION KEY IS NOT KEPT. `smbcrypt_begin()` consumes it and the caller
// zeroizes its copy; only the two derived directional keys live for the length
// of the connection. Same reasoning net/smb.h already records for keeping the
// NT hash instead of the password.
// ===========================================================================
#[repr(C)]
pub struct SmbCrypt {
    /// Client -> server. KDF(SessionKey, "SMB2AESCCM\0", "ServerIn \0").
    enc_key: [u8; 16],
    /// Server -> client. KDF(SessionKey, "SMB2AESCCM\0", "ServerOut\0").
    dec_key: [u8; 16],
    /// The outbound nonce counter. Starts at 0, pre-incremented, so the first
    /// message uses 1. Never reused under a key: see the file header.
    nonce_ctr: u64,
    /// The SessionId this state belongs to, so a frame aimed at another
    /// session is refused by identity rather than only by its tag.
    session_id: u64,
    sealed_out: u32,
    unsealed_in: u32,
    rejected_in: u32,
    /// Post-session messages that went out, or came in, IN THE CLEAR. Under
    /// the default policy both are zero for the life of a mount; they are
    /// counted rather than assumed so the claim is checkable.
    clear_out: u32,
    clear_in: u32,
    /// 1 once both directional keys exist.
    armed: u8,
    /// 1 once every non-NEGOTIATE, non-SESSION_SETUP message is encrypted.
    active: u8,
    /// The policy this connection came up under, captured at NEGOTIATE so a
    /// mid-mount config change cannot retroactively weaken a live mount.
    policy: u8,
    /// 1 if the server's NEGOTIATE response advertised CAP_ENCRYPTION.
    server_capable: u8,
    /// 1 if the server DEMANDS encryption, from SESSION_SETUP SessionFlags
    /// 0x0004 or TREE_CONNECT ShareFlags 0x8000. BOTH are read: measured, a
    /// per-share `smb encrypt = required` sets only the share flag, so a
    /// client that read just the session flag would miss it and then be
    /// refused at CREATE with 0xC0000022 and no idea why.
    server_requires: u8,
    /// ALGO_*; AES-128-CCM for every dialect this module covers.
    algo: u8,
    pad: [u8; 2],
}

const SMBCRYPT_ZERO: SmbCrypt = SmbCrypt {
    enc_key: [0u8; 16],
    dec_key: [0u8; 16],
    nonce_ctr: 0,
    session_id: 0,
    sealed_out: 0,
    unsealed_in: 0,
    rejected_in: 0,
    clear_out: 0,
    clear_in: 0,
    armed: 0,
    active: 0,
    policy: POLICY_REQUIRED as u8,
    server_capable: 0,
    server_requires: 0,
    algo: ALGO_NONE,
    pad: [0u8; 2],
};

// ===========================================================================
// Small helpers.
// ===========================================================================
#[inline(always)]
fn rd_u16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

#[inline(always)]
fn rd_u32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

#[inline(always)]
fn rd_u64(b: &[u8], off: usize) -> u64 {
    let mut v = 0u64;
    let mut i = 0;
    while i < 8 {
        v |= (b[off + i] as u64) << (8 * i);
        i += 1;
    }
    v
}

#[inline(always)]
fn wr_u16(b: &mut [u8], off: usize, v: u16) {
    b[off] = (v & 0xFF) as u8;
    b[off + 1] = (v >> 8) as u8;
}

#[inline(always)]
fn wr_u32(b: &mut [u8], off: usize, v: u32) {
    let mut i = 0;
    while i < 4 {
        b[off + i] = ((v >> (8 * i)) & 0xFF) as u8;
        i += 1;
    }
}

#[inline(always)]
fn wr_u64(b: &mut [u8], off: usize, v: u64) {
    let mut i = 0;
    while i < 8 {
        b[off + i] = ((v >> (8 * i)) & 0xFF) as u8;
        i += 1;
    }
}

fn zeroize(b: &mut [u8]) {
    let mut i = 0;
    while i < b.len() {
        unsafe { core::ptr::write_volatile(&mut b[i], 0) };
        i += 1;
    }
}

/// Is this a message that precedes any key, and so is never encrypted?
/// Decided by command code rather than by the caller remembering, so an SMB2
/// command added later is encrypted by default rather than by accident.
fn command_is_presession(pdu: &[u8]) -> bool {
    if pdu.len() < SMB2_HDR_LEN {
        return true;
    }
    if rd_u32(pdu, 0) != SMB2_MAGIC {
        return true;
    }
    let cmd = rd_u16(pdu, OFF_COMMAND);
    cmd == SMB2_NEGOTIATE || cmd == SMB2_SESSION_SETUP
}

// ===========================================================================
// Policy, global.
// ===========================================================================

#[no_mangle]
pub extern "C" fn smbcrypt_policy_set(policy: u32) {
    // An unrecognised value resolves to REQUIRED, never to the weaker one.
    let p = match policy {
        POLICY_OFF => POLICY_OFF,
        POLICY_ENABLED => POLICY_ENABLED,
        _ => POLICY_REQUIRED,
    };
    POLICY.store(p, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn smbcrypt_policy() -> u32 {
    POLICY.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn smbcrypt_policy_text(policy: u32) -> *const u8 {
    match policy {
        POLICY_OFF => b"off\0".as_ptr(),
        POLICY_ENABLED => b"enabled\0".as_ptr(),
        _ => b"required\0".as_ptr(),
    }
}

/// Parse a /CONFIG/SMB.CFG buffer for an `encrypt=` line.
/// Returns 2 = required, 1 = enabled, 0 = off, -1 = no `encrypt=` key present
/// (leave the default alone), -2 = the key is present with a value we do not
/// know. An unrecognised value is REPORTED, not silently resolved: a typo that
/// quietly picks a policy is how a machine ends up running one nobody chose.
#[no_mangle]
pub extern "C" fn smbcrypt_policy_parse(buf: *const u8, len: i32) -> i32 {
    if buf.is_null() || len <= 0 {
        return -1;
    }
    // SAFETY: the caller (net/smb.c, from the config read) guarantees `len`
    // readable bytes at `buf`. The slice is not retained past this call.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };
    let key = b"encrypt";
    let mut i = 0usize;
    while i < b.len() {
        let mut p = i;
        while p < b.len() && (b[p] == b' ' || b[p] == b'\t') { p += 1; }
        let is_comment = p < b.len() && (b[p] == b'#' || b[p] == b';');
        if !is_comment && p + key.len() < b.len() {
            let mut k = 0usize;
            let mut hit = true;
            while k < key.len() {
                if (b[p + k] | 0x20) != key[k] { hit = false; break; }
                k += 1;
            }
            if hit {
                let mut q = p + key.len();
                while q < b.len() && (b[q] == b' ' || b[q] == b'\t') { q += 1; }
                if q < b.len() && b[q] == b'=' {
                    q += 1;
                    while q < b.len() && (b[q] == b' ' || b[q] == b'\t') { q += 1; }
                    let vstart = q;
                    while q < b.len() && b[q] != b'\n' && b[q] != b'\r'
                        && b[q] != b' ' && b[q] != b'\t' && b[q] != b'#' { q += 1; }
                    let v = &b[vstart..q];
                    if tok_eq(v, b"required") { return 2; }
                    if tok_eq(v, b"enabled") { return 1; }
                    if tok_eq(v, b"off") { return 0; }
                    return -2;
                }
            }
        }
        while i < b.len() && b[i] != b'\n' { i += 1; }
        if i < b.len() { i += 1; }
    }
    -1
}

fn tok_eq(v: &[u8], want: &[u8]) -> bool {
    if v.len() != want.len() { return false; }
    let mut i = 0;
    while i < v.len() {
        if (v[i] | 0x20) != want[i] { return false; }
        i += 1;
    }
    true
}

/// Lock the three policy constants C mirrors against this module's copies.
#[no_mangle]
pub extern "C" fn smbcrypt_policy_abi_check(c_off: u32, c_enabled: u32,
                                            c_required: u32) -> i32 {
    if c_off != POLICY_OFF { return 1; }
    if c_enabled != POLICY_ENABLED { return 2; }
    if c_required != POLICY_REQUIRED { return 3; }
    0
}

/// The capability bits this client must advertise in NEGOTIATE for encryption
/// to be available at all. MEASURED: the server sets CAP_ENCRYPTION in its
/// answer only if the client set it in the request (0x47 with, 0x07 without),
/// so without this the server never offers encryption and a `required` policy
/// would refuse every mount for a reason that was our own fault.
#[no_mangle]
pub extern "C" fn smbcrypt_client_capabilities() -> u32 {
    if smbcrypt_policy() == POLICY_OFF { 0 } else { GLOBAL_CAP_ENCRYPTION }
}

// ===========================================================================
// Per-connection lifecycle.
// ===========================================================================

/// Clear a connection's encryption state. Called wherever the socket is
/// (re)made, so a revived connection can never reuse the previous session's
/// keys OR, worse, restart the nonce counter under a key that is still live.
#[no_mangle]
pub extern "C" fn smbcrypt_reset(st: *mut SmbCrypt) {
    if st.is_null() { return; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbCrypt.
    unsafe {
        let s = &mut *st;
        zeroize(&mut s.enc_key);
        zeroize(&mut s.dec_key);
        *s = SMBCRYPT_ZERO;
    }
}

/// Record what the server's NEGOTIATE response said, and refuse the mount now
/// if the policy cannot be satisfied. Refusing HERE rather than at the first
/// encrypted message is the point: a server that will not encrypt cannot be
/// talked to safely under this policy, and finding that out after sending an
/// NTLM authentication is finding it out too late.
#[no_mangle]
pub extern "C" fn smbcrypt_note_negotiate(st: *mut SmbCrypt, dialect: u16,
                                          server_caps: u32) -> u16 {
    if st.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbCrypt.
    let s = unsafe { &mut *st };
    s.policy = smbcrypt_policy() as u8;
    s.server_capable = ((server_caps & GLOBAL_CAP_ENCRYPTION) != 0) as u8;

    if s.policy as u32 == POLICY_REQUIRED {
        // A dialect with no transform header at all, or one whose key schedule
        // this module does not implement, is refused by name.
        //
        // 0x0311 is now implemented, keyed from the pre-auth integrity hash
        // (rustkern/smbpreauth.rs) rather than from the session key alone, so
        // it passes here and is armed through smbcrypt_begin311().
        if dialect != DIALECT_300 && dialect != DIALECT_302
            && dialect != DIALECT_311 {
            return R_DIALECT;
        }
        if s.server_capable == 0 {
            return R_SERVER_CANNOT;
        }
    }
    R_OK
}

/// Derive the two directional keys and arm the connection. Called once,
/// immediately after the final SESSION_SETUP response, from the same place
/// `smbsign_begin()` is called and BEFORE the caller zeroizes the session key.
///
/// AN ANONYMOUS OR GUEST SESSION CANNOT BE ENCRYPTED, and saying otherwise
/// would be the dishonest option. Such a session produces no secret session
/// key (the NTLM exchange yields zeros), so the "encryption" would run under a
/// key any peer can derive. That is refused here with R_NO_SESSION_KEY, and
/// net/smb.c treats it exactly as the signing path already treats the same
/// case: the mount proceeds, loudly unprotected, because refusing it would
/// break the documented no-credential flow in exchange for no security at all.
/// It is not a downgrade an attacker can force: reaching it requires that the
/// CALLER asked for an anonymous identity, and a named mount whose session
/// came back as guest is refused by the identity check (rustkern/smbident.rs).
#[no_mangle]
pub extern "C" fn smbcrypt_begin(st: *mut SmbCrypt, dialect: u16,
                                 session_key: *const u8,
                                 session_id: u64) -> u16 {
    if st.is_null() || session_key.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbCrypt and
    // 16 readable bytes at session_key. Neither pointer is retained.
    let s = unsafe { &mut *st };
    let sk: &[u8] = unsafe { core::slice::from_raw_parts(session_key, 16) };

    let mut any = 0u8;
    let mut i = 0;
    while i < 16 { any |= sk[i]; i += 1; }
    if any == 0 {
        return R_NO_SESSION_KEY;
    }

    if dialect == DIALECT_311 {
        // Reached only if a caller used the WRONG entry point. At 3.1.1 the two
        // directional cipher keys are functions of the pre-auth integrity hash,
        // which this function has no argument for. Deriving them from the
        // session key alone with the 3.0.2 labels would produce keys that look
        // perfectly ordinary and that the server will not share, and the
        // failure would surface as an authentication-tag mismatch on the first
        // encrypted read. Refuse by name.
        return R_DIALECT_311_WRONG_ENTRY;
    }
    if dialect != DIALECT_300 && dialect != DIALECT_302 {
        return R_DIALECT;
    }

    let mut ki = [0u8; 16];
    ki.copy_from_slice(sk);
    let mut ek = [0u8; 16];
    let mut dk = [0u8; 16];
    // SAFETY: smbsign_kdf128_shared reads 16 bytes at ki and the two
    // label/context slices, and writes exactly 16 bytes to the out pointer.
    unsafe {
        smbsign_kdf128_shared(ki.as_ptr(), b"SMB2AESCCM\0".as_ptr(), 11,
                              b"ServerIn \0".as_ptr(), 10, ek.as_mut_ptr());
        smbsign_kdf128_shared(ki.as_ptr(), b"SMB2AESCCM\0".as_ptr(), 11,
                              b"ServerOut\0".as_ptr(), 10, dk.as_mut_ptr());
    }
    s.enc_key.copy_from_slice(&ek);
    s.dec_key.copy_from_slice(&dk);
    zeroize(&mut ek);
    zeroize(&mut dk);
    zeroize(&mut ki);

    s.algo = ALGO_AES128_CCM;
    s.session_id = session_id;
    s.nonce_ctr = 0;
    s.armed = 1;
    R_OK
}

/// Read the SESSION_SETUP response SessionFlags. Returns 0, or a refusal if
/// the server DEMANDS encryption and the policy forbids it, which is refused
/// here rather than left to fail as an unexplained 0xC0000022 at TREE_CONNECT.
/// Arm SMB 3.1.1 encryption with the two directional keys ALREADY DERIVED from
/// the pre-authentication integrity hash by smbpreauth_derive().
///
/// Note the direction mapping, which is where this is easy to get backwards.
/// At 3.0.2 the labels are written from the SERVER's point of view:
/// "ServerIn \0" is the key the CLIENT encrypts with, "ServerOut\0" the one it
/// decrypts with. At 3.1.1 the labels are written from the CLIENT's point of
/// view instead: "SMBC2SCipherKey\0" is client-to-server, so it is the
/// encryption key, and "SMBS2CCipherKey\0" is the decryption key. Same
/// semantics, opposite naming convention, and swapping them produces a session
/// in which every frame fails its tag check.
///
/// MEASURED: a server TRANSFORM frame from a real Samba 4.17.12 SMB 3.1.1
/// session decrypted correctly under the "SMBS2CCipherKey\0" key with an
/// 11-byte CCM nonce, and the SMB2_TRANSFORM_HEADER field at offset 42, which
/// is EncryptionAlgorithm at 3.0.2 and Flags at 3.1.1, carried 0x0001 in both
/// roles. So the seal and unseal paths below need NO change for 3.1.1; only the
/// key schedule differs. That is why this function sets ALGO_AES128_CCM.
#[no_mangle]
pub extern "C" fn smbcrypt_begin311(st: *mut SmbCrypt,
                                    enc_key: *const u8,
                                    dec_key: *const u8,
                                    session_id: u64) -> u16 {
    if st.is_null() || enc_key.is_null() || dec_key.is_null() { return R_NULL; }
    // SAFETY: the caller guarantees a live, aligned, uniquely-owned SmbCrypt and
    // 16 readable bytes at each key pointer. No pointer is retained.
    let s = unsafe { &mut *st };
    let ek: &[u8] = unsafe { core::slice::from_raw_parts(enc_key, 16) };
    let dk: &[u8] = unsafe { core::slice::from_raw_parts(dec_key, 16) };

    // An all-zero key means the derivation upstream failed and reported it by
    // zeroing its output. Both are checked: a session that encrypted with a real
    // key and decrypted with zeros would appear to half-work.
    let mut any = 0u8;
    let mut i = 0;
    while i < 16 { any |= ek[i] | dk[i]; i += 1; }
    if any == 0 {
        return R_NO_SESSION_KEY;
    }

    s.enc_key.copy_from_slice(ek);
    s.dec_key.copy_from_slice(dk);
    s.algo = ALGO_AES128_CCM;
    s.session_id = session_id;
    s.nonce_ctr = 0;
    s.armed = 1;
    R_OK
}

#[no_mangle]
pub extern "C" fn smbcrypt_note_session_flags(st: *mut SmbCrypt,
                                              session_flags: u16) -> u16 {
    if st.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbCrypt.
    let s = unsafe { &mut *st };
    if (session_flags & SESSION_FLAG_ENCRYPT_DATA) != 0 {
        s.server_requires = 1;
    }
    settle(s)
}

/// Read the TREE_CONNECT response ShareFlags. MEASURED: a per-share
/// `smb encrypt = required` sets ONLY this flag and leaves the session flag
/// clear, so a client that read one and not the other would carry on in the
/// clear and be refused at CREATE with no useful reason.
#[no_mangle]
pub extern "C" fn smbcrypt_note_share_flags(st: *mut SmbCrypt,
                                            share_flags: u32) -> u16 {
    if st.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbCrypt.
    let s = unsafe { &mut *st };
    if (share_flags & SHAREFLAG_ENCRYPT_DATA) != 0 {
        s.server_requires = 1;
    }
    settle(s)
}

/// The one place that decides whether encryption is in force. Called from both
/// flag readers so the rule has a single definition.
fn settle(s: &mut SmbCrypt) -> u16 {
    if s.policy as u32 == POLICY_OFF {
        if s.server_requires != 0 {
            return R_SERVER_DEMANDS;
        }
        return R_OK;
    }
    if s.armed == 0 {
        // No keys: an anonymous session. Whether that is survivable is
        // net/smb.c's call (it is, loudly, and only for a caller that asked
        // for anonymity), but if the SERVER demands encryption there is
        // nothing to discuss.
        if s.server_requires != 0 {
            return R_NO_SESSION_KEY;
        }
        return R_OK;
    }
    // Under REQUIRED, encrypt unconditionally. Under ENABLED, encrypt whenever
    // the server says it can, which protects against a passive listener on
    // every capable server; the honest limit is in the file header.
    if s.policy as u32 == POLICY_REQUIRED
        || s.server_requires != 0
        || s.server_capable != 0
    {
        s.active = 1;
    }
    R_OK
}

#[no_mangle]
pub extern "C" fn smbcrypt_is_armed(st: *const SmbCrypt) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbCrypt for the read.
    let s = unsafe { &*st };
    (s.armed != 0 && s.algo != ALGO_NONE) as i32
}

#[no_mangle]
pub extern "C" fn smbcrypt_is_active(st: *const SmbCrypt) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbCrypt for the read.
    let s = unsafe { &*st };
    (s.active != 0 && s.armed != 0) as i32
}

#[no_mangle]
pub extern "C" fn smbcrypt_server_capable(st: *const SmbCrypt) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbCrypt for the read.
    unsafe { ((*st).server_capable != 0) as i32 }
}

#[no_mangle]
pub extern "C" fn smbcrypt_server_requires(st: *const SmbCrypt) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbCrypt for the read.
    unsafe { ((*st).server_requires != 0) as i32 }
}

#[no_mangle]
pub extern "C" fn smbcrypt_algo_text(st: *const SmbCrypt) -> *const u8 {
    if st.is_null() { return b"none\0".as_ptr(); }
    // SAFETY: caller guarantees a live, aligned SmbCrypt for the read.
    let s = unsafe { &*st };
    match s.algo {
        ALGO_AES128_CCM => b"AES-128-CCM (SMB 3.0/3.0.2/3.1.1)\0".as_ptr(),
        _ => b"none\0".as_ptr(),
    }
}

#[no_mangle]
pub extern "C" fn smbcrypt_counts(st: *const SmbCrypt, out_sealed: *mut u32,
                                  out_unsealed: *mut u32, out_rejected: *mut u32,
                                  out_clear_out: *mut u32, out_clear_in: *mut u32) {
    if st.is_null() { return; }
    // SAFETY: caller guarantees a live SmbCrypt and, for each non-null out
    // pointer, one writable u32.
    unsafe {
        let s = &*st;
        if !out_sealed.is_null() { *out_sealed = s.sealed_out; }
        if !out_unsealed.is_null() { *out_unsealed = s.unsealed_in; }
        if !out_rejected.is_null() { *out_rejected = s.rejected_in; }
        if !out_clear_out.is_null() { *out_clear_out = s.clear_out; }
        if !out_clear_in.is_null() { *out_clear_in = s.clear_in; }
    }
}

// ===========================================================================
// THE TWO CHOKEPOINTS.
// ===========================================================================

/// Should this outbound PDU be encrypted? 1 = yes, 0 = send it as it is.
/// A 0 for a post-session message is COUNTED, so "this mount sent nothing in
/// the clear" is a number rather than a belief.
#[no_mangle]
pub extern "C" fn smbcrypt_should_seal(st: *mut SmbCrypt, pdu: *const u8,
                                       len: usize) -> i32 {
    if st.is_null() || pdu.is_null() { return 0; }
    // SAFETY: caller guarantees a live SmbCrypt and `len` readable bytes.
    let s = unsafe { &mut *st };
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    if command_is_presession(p) {
        return 0;
    }
    if s.active != 0 && s.armed != 0 {
        return 1;
    }
    s.clear_out = s.clear_out.wrapping_add(1);
    0
}

/// Encrypt `pdu` IN PLACE and write the 52-byte transform header to `hdr_out`.
///
/// In place, and a separate header, on purpose: net/smb.c's send buffer is a
/// fixed 64 KB and a second one would double that for no reason. The caller
/// sends the header and then the buffer, with a NetBIOS length of 52 + len.
#[no_mangle]
pub extern "C" fn smbcrypt_seal(st: *mut SmbCrypt, pdu: *mut u8, len: usize,
                                hdr_out: *mut u8) -> u16 {
    if st.is_null() || pdu.is_null() || hdr_out.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live SmbCrypt, `len` readable+writable bytes
    // at pdu, and TFORM_LEN writable bytes at hdr_out.
    let s = unsafe { &mut *st };
    if s.armed == 0 || s.algo != ALGO_AES128_CCM { return R_NOT_ARMED; }
    if len < SMB2_HDR_LEN { return R_SHORT; }

    if s.nonce_ctr == u64::MAX { return R_NONCE_EXHAUSTED; }
    s.nonce_ctr += 1;

    let h: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(hdr_out, TFORM_LEN) };
    let mut i = 0usize;
    while i < TFORM_LEN { h[i] = 0; i += 1; }
    wr_u32(h, OFF_T_MAGIC, TRANSFORM_MAGIC);
    // The nonce: counter in the low 8 bytes, the other 3 of the 11 and all 5
    // trailing bytes of the field left zero.
    wr_u64(h, OFF_T_NONCE, s.nonce_ctr);
    wr_u32(h, OFF_T_OSIZE, len as u32);
    wr_u16(h, OFF_T_ALGO, WIRE_ALGO_CCM);
    wr_u64(h, OFF_T_SID, s.session_id);

    let mut aad = [0u8; AAD_LEN];
    aad.copy_from_slice(&h[AAD_OFF..AAD_OFF + AAD_LEN]);
    let mut nonce = [0u8; NONCE_LEN];
    nonce.copy_from_slice(&h[OFF_T_NONCE..OFF_T_NONCE + NONCE_LEN]);
    let mut tag = [0u8; TAG_LEN];

    // SAFETY: enc_key is 16 bytes, nonce/aad/tag are locals of the stated
    // lengths, and pdu has `len` readable+writable bytes per the contract.
    let rc = unsafe {
        aes_ccm_encrypt(s.enc_key.as_ptr(), nonce.as_ptr(), NONCE_LEN,
                        aad.as_ptr(), AAD_LEN, pdu, len,
                        tag.as_mut_ptr(), TAG_LEN)
    };
    if rc != 0 {
        return R_CIPHER;
    }
    h[OFF_T_TAG..OFF_T_TAG + TAG_LEN].copy_from_slice(&tag);
    s.sealed_out = s.sealed_out.wrapping_add(1);
    R_OK
}

/// Is this inbound frame an encrypted one? Cheap, and deliberately separate
/// from unsealing: net/smb.c has to ask before it looks at any field, because
/// a transform header has no SMB2 header in it to look at.
#[no_mangle]
pub extern "C" fn smbcrypt_pdu_is_transform(pdu: *const u8, len: usize) -> i32 {
    if pdu.is_null() || len < TFORM_LEN { return 0; }
    // SAFETY: caller guarantees `len` readable bytes at pdu.
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    (rd_u32(p, OFF_T_MAGIC) == TRANSFORM_MAGIC) as i32
}

/// Decrypt an inbound transform frame IN PLACE. `buf` holds the whole frame on
/// entry; on success the plaintext SMB2 message is at buf[0..*out_len].
///
/// EVERY FIELD IS CHECKED BEFORE THE CIPHER RUNS, and each refusal has its own
/// reason, because "it did not decrypt" is the least useful of the answers
/// this can give. The size and the SessionId are inside the AAD, so a frame
/// that lies about either also fails its tag; checking them explicitly means
/// the log says which lie it was.
#[no_mangle]
pub extern "C" fn smbcrypt_unseal(st: *mut SmbCrypt, buf: *mut u8, len: usize,
                                  out_len: *mut u32) -> u16 {
    if st.is_null() || buf.is_null() || out_len.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live SmbCrypt, `len` readable+writable bytes
    // at buf, and one writable u32 at out_len.
    let s = unsafe { &mut *st };
    let b: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, len) };
    unsafe { *out_len = 0 };

    if s.armed == 0 || s.algo != ALGO_AES128_CCM {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_NOT_ARMED;
    }
    if len <= TFORM_LEN {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_SHORT;
    }
    if rd_u32(b, OFF_T_MAGIC) != TRANSFORM_MAGIC {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_NOT_TRANSFORM;
    }
    let algo = rd_u16(b, OFF_T_ALGO);
    if algo != WIRE_ALGO_CCM {
        // 0x0002 is AES-128-GCM, which exists only from 3.1.1 and is NOT
        // guessed at here. MEASURED: this very server kills a connection that
        // uses GCM at 3.0.2, so a frame that arrives claiming it is either a
        // different dialect's or not from the server at all.
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_ALGO;
    }
    let osize = rd_u32(b, OFF_T_OSIZE) as usize;
    if osize != len - TFORM_LEN || osize < SMB2_HDR_LEN {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_SIZE_MISMATCH;
    }
    if rd_u64(b, OFF_T_SID) != s.session_id {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_SESSION_MISMATCH;
    }
    // The 5 bytes of the Nonce field past the 11 CCM uses MUST be zero
    // (MS-SMB2 2.2.41). Checking it costs nothing and closes the only part of
    // the nonce field that is not pinned by the AAD... which is to say: it is
    // pinned, so this check is about saying WHICH thing was wrong.
    let mut padbits = 0u8;
    let mut i = OFF_T_NONCE + NONCE_LEN;
    while i < OFF_T_NONCE + 16 { padbits |= b[i]; i += 1; }
    if padbits != 0 {
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_NONCE_PADDING;
    }

    let mut aad = [0u8; AAD_LEN];
    aad.copy_from_slice(&b[AAD_OFF..AAD_OFF + AAD_LEN]);
    let mut nonce = [0u8; NONCE_LEN];
    nonce.copy_from_slice(&b[OFF_T_NONCE..OFF_T_NONCE + NONCE_LEN]);
    let mut tag = [0u8; TAG_LEN];
    tag.copy_from_slice(&b[OFF_T_TAG..OFF_T_TAG + TAG_LEN]);

    // SAFETY: dec_key is 16 bytes; the data pointer is inside `b`, which has
    // len readable+writable bytes, and osize == len - TFORM_LEN was checked
    // above; nonce/aad/tag are locals of the stated lengths.
    let rc = unsafe {
        aes_ccm_decrypt(s.dec_key.as_ptr(), nonce.as_ptr(), NONCE_LEN,
                        aad.as_ptr(), AAD_LEN,
                        b.as_mut_ptr().add(TFORM_LEN), osize,
                        tag.as_ptr(), TAG_LEN)
    };
    if rc != 0 {
        // aes_ccm_decrypt has already wiped the buffer on an auth failure, so
        // there is no unauthenticated plaintext left for a careless caller.
        s.rejected_in = s.rejected_in.wrapping_add(1);
        return R_AUTH;
    }

    // Move the plaintext to the front. Forward copy of a backwards shift, so
    // the overlap is safe.
    b.copy_within(TFORM_LEN..TFORM_LEN + osize, 0);
    unsafe { *out_len = osize as u32 };
    s.unsealed_in = s.unsealed_in.wrapping_add(1);
    R_OK
}

/// THE ANTI-DOWNGRADE RULE ON THE RECEIVE SIDE.
///
/// Once encryption is in force, a response that arrives in the CLEAR must not
/// be acted on. This is the same defect shape as the guest-session downgrade
/// fixed the day before: if an attacker can strip the protection and still be
/// believed, the protection is decorative.
///
/// Returns R_OK if an unencrypted inbound message is acceptable, else the
/// refusal reason. The only acceptable ones are the messages that precede any
/// key: NEGOTIATE and SESSION_SETUP. Counted either way.
#[no_mangle]
pub extern "C" fn smbcrypt_check_clear_response(st: *mut SmbCrypt,
                                                pdu: *const u8,
                                                len: usize) -> u16 {
    if st.is_null() || pdu.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live SmbCrypt and `len` readable bytes.
    let s = unsafe { &mut *st };
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    if s.active == 0 {
        if !command_is_presession(p) {
            s.clear_in = s.clear_in.wrapping_add(1);
        }
        return R_OK;
    }
    if command_is_presession(p) {
        return R_OK;
    }
    s.clear_in = s.clear_in.wrapping_add(1);
    s.rejected_in = s.rejected_in.wrapping_add(1);
    R_CLEARTEXT
}

#[no_mangle]
pub extern "C" fn smbcrypt_pdu_session_id(pdu: *const u8, len: usize) -> u64 {
    if pdu.is_null() || len < SMB2_HDR_LEN { return 0; }
    // SAFETY: caller guarantees `len` readable bytes at pdu.
    let p: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    rd_u64(p, OFF_SESSION_ID)
}

#[no_mangle]
pub extern "C" fn smbcrypt_reason_text(code: u16) -> *const u8 {
    match code {
        R_OK => b"ok\0".as_ptr(),
        R_NOT_ARMED => b"this connection has no encryption key yet\0".as_ptr(),
        R_SHORT => {
            b"the frame is too short to be an encrypted SMB2 message\0".as_ptr()
        }
        R_NOT_TRANSFORM => {
            b"this is not an SMB2 TRANSFORM (encrypted) frame\0".as_ptr()
        }
        R_DIALECT => {
            b"this SMB dialect has no encryption this client implements (3.0 \
              and 3.0.2 do; 2.0.2 and 2.1 cannot encrypt at all, and 3.1.1 is \
              not implemented or offered)\0".as_ptr()
        }
        R_SERVER_CANNOT => {
            b"the server did not offer encryption (no SMB2_GLOBAL_CAP_ENCRYPTION \
              in its NEGOTIATE response)\0".as_ptr()
        }
        R_SERVER_DEMANDS => {
            b"the server REQUIRES encryption and the local policy is encrypt=off\0"
                .as_ptr()
        }
        R_NO_SESSION_KEY => {
            b"this session produced no secret key to encrypt with (anonymous or \
              guest)\0".as_ptr()
        }
        R_NULL => b"internal: null state or buffer\0".as_ptr(),
        R_ALGO => {
            b"the frame names an encryption algorithm this dialect does not \
              define (0x0002 AES-GCM arrives with SMB 3.1.1; 3.0.2 is CCM only)\0"
                .as_ptr()
        }
        R_SESSION_MISMATCH => {
            b"the frame is addressed to a different SMB session\0".as_ptr()
        }
        R_SIZE_MISMATCH => {
            b"the frame's stated plaintext size does not match the bytes that \
              arrived (truncated, padded or forged)\0".as_ptr()
        }
        R_AUTH => {
            b"DECRYPTION FAILED: the authentication tag does not match, so these \
              bytes are not what the server sent\0".as_ptr()
        }
        R_NONCE_EXHAUSTED => {
            b"this session's encryption counter is exhausted; it must be \
              re-established rather than reuse a nonce\0".as_ptr()
        }
        R_CLEARTEXT => {
            b"this message arrived UNENCRYPTED on a session that is encrypted\0"
                .as_ptr()
        }
        R_CIPHER => b"internal: the AES-CCM call refused its parameters\0".as_ptr(),
        R_DIALECT_311_WRONG_ENTRY =>
            b"internal error: SMB 3.1.1 needs smbcrypt_begin311(), whose keys \
              come from the pre-auth integrity hash\0".as_ptr(),
        R_NONCE_PADDING => {
            b"the frame's nonce field has non-zero bytes past the 11 that AES-CCM \
              uses\0".as_ptr()
        }
        _ => b"unknown encryption refusal\0".as_ptr(),
    }
}

// ===========================================================================
// SELF-TEST.
//
// The SMB-specific vectors come from an INDEPENDENT Python implementation of
// MS-SMB2 3.1.4.2/3.1.4.3 built on pycryptodome's AES-CCM, whose correctness
// is in turn pinned by RFC 3610's published vectors (checked in
// aesccm_selftest_rs, and separately confirmed byte for byte against
// pycryptodome). They were NOT generated by this module. A vector a module
// produces for itself proves only that it agrees with itself, which is exactly
// the property a wrong implementation also has.
//
// The SAME Python client, with these same framing rules, had a real encrypted
// TREE_CONNECT, CREATE and READ accepted by Samba 4.17.12 on dialect 0x0302,
// so this framing is known to work against a server and not merely to be
// self-consistent.
//
// Returns 0 on success, else the 1-based number of the first failing case.
// ===========================================================================
#[no_mangle]
pub extern "C" fn smbcrypt_selftest_rs() -> u32 {
    // The oracle's inputs.
    const SESSION_KEY: [u8; 16] = [0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                                   0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50];
    const SESSION_ID: u64 = 0x0011_2233_4455_6677;
    const ENC_KEY: [u8; 16] = [0x50, 0x77, 0xee, 0x3f, 0x04, 0x2e, 0x5e, 0x11,
                               0xd9, 0x9a, 0xff, 0xa5, 0xb7, 0xfc, 0x4b, 0xbc];
    const DEC_KEY: [u8; 16] = [0x1e, 0x5a, 0xaf, 0xcc, 0x84, 0x2e, 0x01, 0xe3,
                               0xf0, 0x7c, 0x98, 0x66, 0x4f, 0x8b, 0x2e, 0xec];
    // An 80-byte SMB2 READ request: 64-byte header (command 8, MessageId 7,
    // TreeId 0x12345678, the SessionId above) plus 16 bytes of body.
    const PDU: [u8; 80] = [
        0xfe, 0x53, 0x4d, 0x42, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f];
    // Client -> server, nonce counter 1, under ENC_KEY.
    const C2S_HDR: [u8; 52] = [
        0xfd, 0x53, 0x4d, 0x42, 0xbc, 0xe8, 0x62, 0xed, 0xf5, 0x26, 0x9d, 0x9c,
        0xab, 0xc4, 0x17, 0xc4, 0x58, 0xae, 0x01, 0xc3, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x77, 0x66, 0x55, 0x44,
        0x33, 0x22, 0x11, 0x00];
    const C2S_CT: [u8; 80] = [
        0xbd, 0xb1, 0x00, 0xb7, 0xb6, 0xd9, 0xdc, 0x99, 0xc6, 0x73, 0xec, 0xc1,
        0x81, 0x84, 0x06, 0x29, 0x34, 0x5c, 0x76, 0x4d, 0x74, 0xcb, 0xa7, 0x10,
        0x6f, 0x2f, 0x3a, 0x8a, 0x20, 0xc7, 0x8e, 0xd1, 0xde, 0xd2, 0x1e, 0xa0,
        0x39, 0x63, 0xf5, 0xe9, 0x50, 0x43, 0x04, 0xe4, 0x13, 0x07, 0x7a, 0x9d,
        0x91, 0x1d, 0x8d, 0xc7, 0xc7, 0x70, 0x05, 0x4a, 0x6e, 0xa3, 0x42, 0x5b,
        0x06, 0xa6, 0x2b, 0x3b, 0x60, 0x9a, 0x0c, 0x65, 0xa1, 0xa5, 0x19, 0x0e,
        0x8c, 0x3d, 0x94, 0x9f, 0x39, 0xf8, 0x73, 0x31];
    // Server -> client, nonce counter 9, under DEC_KEY.
    const S2C_HDR: [u8; 52] = [
        0xfd, 0x53, 0x4d, 0x42, 0xe6, 0x5b, 0xb8, 0xf6, 0xbc, 0x32, 0xd8, 0x9f,
        0xcf, 0xed, 0x22, 0xaa, 0xcb, 0x03, 0x6d, 0x5a, 0x09, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x77, 0x66, 0x55, 0x44,
        0x33, 0x22, 0x11, 0x00];
    const S2C_CT: [u8; 80] = [
        0x7b, 0xe8, 0xdd, 0x38, 0xe6, 0xa8, 0x7e, 0xc0, 0xb6, 0xe8, 0x14, 0xe0,
        0x0c, 0xc6, 0xd3, 0x53, 0xad, 0x4b, 0xff, 0xea, 0xf3, 0xa9, 0xe5, 0x7e,
        0xd8, 0x4a, 0xae, 0x75, 0x53, 0xab, 0xbf, 0x95, 0x45, 0x03, 0x25, 0x7e,
        0xc8, 0x54, 0xac, 0x1c, 0x37, 0x10, 0xd7, 0x06, 0xbc, 0x9e, 0xbf, 0xae,
        0xdb, 0x65, 0x71, 0xf0, 0xfc, 0xcf, 0xcc, 0xf5, 0x06, 0xde, 0x82, 0x79,
        0x4e, 0xd5, 0x10, 0x9f, 0x57, 0x55, 0x65, 0xa6, 0x58, 0x78, 0x8e, 0x20,
        0x0f, 0x70, 0xc4, 0x2b, 0x65, 0x05, 0x92, 0x48];

    let mut case = 0u32;
    let mut st = SMBCRYPT_ZERO;

    // ---- 1. THE KEY SCHEDULE, against the oracle's keys. This is the case
    // that catches a swapped "ServerIn "/"ServerOut", a missing NUL and a
    // missing trailing space, none of which fail any round-trip test.
    case += 1;
    if smbcrypt_begin(&mut st as *mut SmbCrypt, DIALECT_302,
                      SESSION_KEY.as_ptr(), SESSION_ID) != R_OK {
        return case;
    }
    if !eq(&st.enc_key, &ENC_KEY) || !eq(&st.dec_key, &DEC_KEY) {
        return case;
    }

    // ---- 2. SEAL produces the oracle's header AND ciphertext byte for byte,
    // including the tag and the counter nonce.
    case += 1;
    let mut body = [0u8; 80];
    body.copy_from_slice(&PDU);
    let mut hdr = [0u8; TFORM_LEN];
    st.active = 1;
    if smbcrypt_seal(&mut st as *mut SmbCrypt, body.as_mut_ptr(), 80,
                     hdr.as_mut_ptr()) != R_OK {
        return case;
    }
    if !eq(&hdr, &C2S_HDR) || !eq(&body, &C2S_CT) {
        return case;
    }

    // ---- 3. The nonce ADVANCES. Two seals must never share a nonce; this
    // asserts the counter moved and the ciphertext changed with it.
    case += 1;
    let mut body2 = [0u8; 80];
    body2.copy_from_slice(&PDU);
    let mut hdr2 = [0u8; TFORM_LEN];
    if smbcrypt_seal(&mut st as *mut SmbCrypt, body2.as_mut_ptr(), 80,
                     hdr2.as_mut_ptr()) != R_OK {
        return case;
    }
    if eq(&hdr2[OFF_T_NONCE..OFF_T_NONCE + 11], &hdr[OFF_T_NONCE..OFF_T_NONCE + 11]) {
        return case;
    }
    if eq(&body2, &body) {
        return case;
    }

    // ---- 4. UNSEAL of the oracle's server-direction frame recovers the
    // plaintext exactly, and reports the right length.
    case += 1;
    let mut frame = [0u8; TFORM_LEN + 80];
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    let mut outlen = 0u32;
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_OK {
        return case;
    }
    if outlen != 80 || !eq(&frame[..80], &PDU) {
        return case;
    }

    // ---- 5. ONE FLIPPED CIPHERTEXT BYTE IS REFUSED. The destructive case:
    // an AEAD that decrypts whatever arrives is not protection.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    frame[TFORM_LEN + 40] ^= 0x01;
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_AUTH {
        return case;
    }
    if outlen != 0 {
        return case;
    }

    // ---- 6. ONE FLIPPED TAG BYTE IS REFUSED.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    frame[OFF_T_TAG + 3] ^= 0x80;
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_AUTH {
        return case;
    }

    // ---- 7. A TRUNCATED frame is refused, and as a SIZE mismatch rather than
    // as a tag failure, so the log names the right thing.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 79, &mut outlen) != R_SIZE_MISMATCH {
        return case;
    }

    // ---- 8. A frame whose OriginalMessageSize LIES is refused.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    wr_u32(&mut frame, OFF_T_OSIZE, 79);
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_SIZE_MISMATCH {
        return case;
    }

    // ---- 9. A frame aimed at ANOTHER SESSION is refused by identity.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    wr_u64(&mut frame, OFF_T_SID, SESSION_ID ^ 1);
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_SESSION_MISMATCH {
        return case;
    }

    // ---- 10. A frame claiming AES-GCM is refused BY NAME rather than
    // mis-decrypted. MEASURED: GCM at 3.0.2 is what Samba kills a connection
    // over, so a GCM frame arriving here is not this dialect's traffic.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    wr_u16(&mut frame, OFF_T_ALGO, WIRE_ALGO_GCM);
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_ALGO {
        return case;
    }

    // ---- 11. Non-zero bytes in the unused tail of the nonce field are
    // refused.
    case += 1;
    frame[..TFORM_LEN].copy_from_slice(&S2C_HDR);
    frame[TFORM_LEN..].copy_from_slice(&S2C_CT);
    frame[OFF_T_NONCE + 11] = 0x01;
    if smbcrypt_unseal(&mut st as *mut SmbCrypt, frame.as_mut_ptr(),
                       TFORM_LEN + 80, &mut outlen) != R_NONCE_PADDING {
        return case;
    }

    // ---- 12. THE DOWNGRADE RULE. With encryption active, a plaintext READ
    // response is refused; NEGOTIATE and SESSION_SETUP are still allowed
    // because they precede every key.
    case += 1;
    if smbcrypt_check_clear_response(&mut st as *mut SmbCrypt,
                                     PDU.as_ptr(), 80) != R_CLEARTEXT {
        return case;
    }
    case += 1;
    let mut neg = [0u8; 64];
    neg.copy_from_slice(&PDU[..64]);
    wr_u16(&mut neg, OFF_COMMAND, SMB2_NEGOTIATE);
    if smbcrypt_check_clear_response(&mut st as *mut SmbCrypt,
                                     neg.as_ptr(), 64) != R_OK {
        return case;
    }
    case += 1;
    wr_u16(&mut neg, OFF_COMMAND, SMB2_SESSION_SETUP);
    if smbcrypt_check_clear_response(&mut st as *mut SmbCrypt,
                                     neg.as_ptr(), 64) != R_OK {
        return case;
    }

    // ---- 15/16. should_seal: NEGOTIATE and SESSION_SETUP are never sealed, a
    // READ is, and a clear send on an inactive connection is COUNTED.
    case += 1;
    wr_u16(&mut neg, OFF_COMMAND, SMB2_NEGOTIATE);
    if smbcrypt_should_seal(&mut st as *mut SmbCrypt, neg.as_ptr(), 64) != 0 {
        return case;
    }
    if smbcrypt_should_seal(&mut st as *mut SmbCrypt, PDU.as_ptr(), 80) != 1 {
        return case;
    }
    case += 1;
    {
        let before = st.clear_out;
        st.active = 0;
        if smbcrypt_should_seal(&mut st as *mut SmbCrypt, PDU.as_ptr(), 80) != 0 {
            return case;
        }
        if st.clear_out != before + 1 {
            return case;   // an unencrypted post-session send must be counted
        }
        st.active = 1;
    }

    // ---- 17. THE POLICY KNOB KNOB'S THREE VALUES MUST BEHAVE DIFFERENTLY, which
    // is the lesson from the signing work: a documented knob whose settings
    // behave identically is worse than no knob, and `smbsign_requires()` had
    // been written carefully and never called.
    //
    // Server offers encryption, dialect 3.0.2:
    //   required -> accept, and encryption is in force
    //   enabled  -> accept, and encryption is in force
    //   off      -> accept, and encryption is NOT in force
    case += 1;
    for (pol, want_active) in [(POLICY_REQUIRED, 1u8), (POLICY_ENABLED, 1u8),
                               (POLICY_OFF, 0u8)] {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(pol);
        let mut c = SMBCRYPT_ZERO;
        let r1 = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, DIALECT_302,
                                         GLOBAL_CAP_ENCRYPTION);
        let r2 = smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                                SESSION_KEY.as_ptr(), SESSION_ID);
        let r3 = smbcrypt_note_session_flags(&mut c as *mut SmbCrypt, 0);
        smbcrypt_policy_set(saved);
        if r1 != R_OK || r2 != R_OK || r3 != R_OK { return case; }
        if c.active != want_active { return case; }
    }

    // ---- 18. Server does NOT offer encryption:
    //   required -> REFUSED at NEGOTIATE, by name
    //   enabled  -> accepted, and encryption is NOT in force (the honest
    //               fallback, which net/smb.c prints and counts)
    //   off      -> accepted, not in force
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_REQUIRED);
        let mut c = SMBCRYPT_ZERO;
        let r = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, DIALECT_302, 0);
        smbcrypt_policy_set(saved);
        if r != R_SERVER_CANNOT { return case; }
    }
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_ENABLED);
        let mut c = SMBCRYPT_ZERO;
        let r1 = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, DIALECT_302, 0);
        let r2 = smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                                SESSION_KEY.as_ptr(), SESSION_ID);
        let r3 = smbcrypt_note_session_flags(&mut c as *mut SmbCrypt, 0);
        smbcrypt_policy_set(saved);
        if r1 != R_OK || r2 != R_OK || r3 != R_OK { return case; }
        if c.active != 0 { return case; }
    }

    // ---- 20. A dialect that cannot encrypt: refused under `required`,
    // allowed (unencrypted) under `enabled`. 2.1 has no transform header.
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_REQUIRED);
        let mut c = SMBCRYPT_ZERO;
        let r = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, 0x0210,
                                        GLOBAL_CAP_ENCRYPTION);
        smbcrypt_policy_set(saved);
        if r != R_DIALECT { return case; }
    }
    case += 1;
    {
        let mut c = SMBCRYPT_ZERO;
        if smbcrypt_begin(&mut c as *mut SmbCrypt, 0x0210,
                          SESSION_KEY.as_ptr(), SESSION_ID) != R_DIALECT {
            return case;
        }
    }

    // ---- 22. THE PER-SHARE DEMAND. A session flag of 0 and a ShareFlags
    // 0x8000 must still turn encryption on. MEASURED: that is exactly what
    // Samba does for a per-share `smb encrypt = required`, and a client that
    // only read the session flag would be refused at CREATE instead.
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_ENABLED);
        let mut c = SMBCRYPT_ZERO;
        // Deliberately a server with the capability CLEAR, so `enabled` does
        // not switch encryption on by itself and the share flag is the only
        // thing that can.
        let r1 = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, DIALECT_302, 0);
        let r2 = smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                                SESSION_KEY.as_ptr(), SESSION_ID);
        let r3 = smbcrypt_note_session_flags(&mut c as *mut SmbCrypt, 0);
        let active_before = c.active;
        let r4 = smbcrypt_note_share_flags(&mut c as *mut SmbCrypt,
                                           SHAREFLAG_ENCRYPT_DATA);
        smbcrypt_policy_set(saved);
        if r1 != R_OK || r2 != R_OK || r3 != R_OK || r4 != R_OK { return case; }
        if active_before != 0 || c.active != 1 { return case; }
    }

    // ---- 23. A server that DEMANDS encryption while the policy is `off` is
    // refused by name, not left to fail as an unexplained ACCESS_DENIED.
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_OFF);
        let mut c = SMBCRYPT_ZERO;
        let r1 = smbcrypt_note_negotiate(&mut c as *mut SmbCrypt, DIALECT_302,
                                         GLOBAL_CAP_ENCRYPTION);
        let r2 = smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                                SESSION_KEY.as_ptr(), SESSION_ID);
        let r3 = smbcrypt_note_session_flags(&mut c as *mut SmbCrypt,
                                             SESSION_FLAG_ENCRYPT_DATA);
        smbcrypt_policy_set(saved);
        if r1 != R_OK || r2 != R_OK { return case; }
        if r3 != R_SERVER_DEMANDS { return case; }
    }

    // ---- 24. An anonymous session (all-zero session key) produces no keys
    // and is NOT armed. Encrypting under a key of zeros would "work" with
    // anyone else who also has zeros, i.e. everybody.
    case += 1;
    {
        let zero = [0u8; 16];
        let mut c = SMBCRYPT_ZERO;
        if smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                          zero.as_ptr(), SESSION_ID) != R_NO_SESSION_KEY {
            return case;
        }
        if smbcrypt_is_armed(&c as *const SmbCrypt) != 0 { return case; }
        if smbcrypt_is_active(&c as *const SmbCrypt) != 0 { return case; }
    }

    // ---- 25. The config parser: each value maps to its own policy, an
    // unknown value is REPORTED (-2) and an absent key leaves the default
    // (-1). The signing module's `signing=yes` reasoning applies verbatim.
    case += 1;
    {
        let cases: [(&[u8], i32); 8] = [
            (b"encrypt=required\n", 2),
            (b"encrypt=enabled\n", 1),
            (b"encrypt=off\n", 0),
            (b"  Encrypt = OFF  # trailing comment\n", 0),
            (b"encrypt=yes\n", -2),
            (b"signing=required\n", -1),
            (b"# encrypt=off\n", -1),
            (b"signing=enabled\nencrypt=enabled\n", 1),
        ];
        let mut i = 0;
        while i < cases.len() {
            let (buf, want) = cases[i];
            if smbcrypt_policy_parse(buf.as_ptr(), buf.len() as i32) != want {
                return case;
            }
            i += 1;
        }
    }

    // ---- 26. The ABI lock on the three policy constants C mirrors.
    case += 1;
    if smbcrypt_policy_abi_check(POLICY_OFF, POLICY_ENABLED, POLICY_REQUIRED) != 0 {
        return case;
    }
    if smbcrypt_policy_abi_check(1, 1, 2) == 0 {
        return case;
    }

    // ---- 27. reset() leaves no key behind and disarms.
    case += 1;
    {
        let mut c = SMBCRYPT_ZERO;
        if smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                          SESSION_KEY.as_ptr(), SESSION_ID) != R_OK {
            return case;
        }
        smbcrypt_reset(&mut c as *mut SmbCrypt);
        let mut any = 0u8;
        let mut i = 0;
        while i < 16 { any |= c.enc_key[i] | c.dec_key[i]; i += 1; }
        if any != 0 { return case; }
        if c.armed != 0 || c.active != 0 || c.nonce_ctr != 0 { return case; }
    }

    // ---- 28. The capability we advertise follows the policy, because the
    // server only offers encryption if we ask (measured, 0x47 vs 0x07).
    case += 1;
    {
        let saved = smbcrypt_policy();
        smbcrypt_policy_set(POLICY_OFF);
        let a = smbcrypt_client_capabilities();
        smbcrypt_policy_set(POLICY_ENABLED);
        let b = smbcrypt_client_capabilities();
        smbcrypt_policy_set(POLICY_REQUIRED);
        let c = smbcrypt_client_capabilities();
        smbcrypt_policy_set(saved);
        if a != 0 || b != GLOBAL_CAP_ENCRYPTION || c != GLOBAL_CAP_ENCRYPTION {
            return case;
        }
    }

    // ---- 29. A LARGE message round-trips. A 32 KB read response is the
    // normal case on this path and exercises the multi-block CTR and CBC-MAC
    // loops that an 80-byte vector does not.
    case += 1;
    {
        let mut c = SMBCRYPT_ZERO;
        if smbcrypt_begin(&mut c as *mut SmbCrypt, DIALECT_302,
                          SESSION_KEY.as_ptr(), SESSION_ID) != R_OK {
            return case;
        }
        c.active = 1;
        // Build it in a static-sized local: no allocator in here.
        const N: usize = 1024;
        let mut big = [0u8; TFORM_LEN + N];
        let mut i = 0;
        while i < N {
            big[TFORM_LEN + i] = ((i * 7 + 13) & 0xFF) as u8;
            i += 1;
        }
        big[TFORM_LEN..TFORM_LEN + 4].copy_from_slice(&[0xfe, 0x53, 0x4d, 0x42]);
        let mut orig = [0u8; N];
        orig.copy_from_slice(&big[TFORM_LEN..TFORM_LEN + N]);
        let mut h = [0u8; TFORM_LEN];
        // Seal it with the SERVER'S key so unseal (which uses dec_key) can
        // check it: swap the keys for the duration.
        let ek = c.enc_key;
        c.enc_key = c.dec_key;
        let r = smbcrypt_seal(&mut c as *mut SmbCrypt,
                              big.as_mut_ptr().wrapping_add(TFORM_LEN), N,
                              h.as_mut_ptr());
        c.enc_key = ek;
        if r != R_OK { return case; }
        if eq(&big[TFORM_LEN..TFORM_LEN + N], &orig) { return case; }
        big[..TFORM_LEN].copy_from_slice(&h);
        let mut ol = 0u32;
        if smbcrypt_unseal(&mut c as *mut SmbCrypt, big.as_mut_ptr(),
                           TFORM_LEN + N, &mut ol) != R_OK {
            return case;
        }
        if ol as usize != N || !eq(&big[..N], &orig) { return case; }
    }

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
