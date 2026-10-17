// rustkern/smbsign.rs - SMB2 message signing: produce it outbound, VERIFY it
// inbound, and make `signing_required` mean something.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle and no -DRUST_* differential, because before this module the kernel
// had no SMB signing at all. It is a protocol state machine plus a MAC check
// over a byte buffer: no floats, no allocation, no hardware, no performance
// argument for C.
//
// NO CIPHER AND NO MAC IS IMPLEMENTED HERE. Both primitives already ship:
// `hmac_sha256()` (crypto/crypto.h, Rust-backed via rustkern/hmac.rs under
// -DRUST_HMAC) and `aes_cmac()` (crypto/crypto.h, RFC 4493 over AES-128,
// already in use by bt/pair.c for the Bluetooth SMP LESC functions). This file
// calls them. What is new here is the KEY SCHEDULE, the WIRE FRAMING of a
// signature, and the POLICY about what to do when one is missing or wrong.
//
// ===========================================================================
// WHAT WAS WRONG, measured on the tree at dev 71da4de2, not assumed
// ---------------------------------------------------------------------------
// `smb_connection_t::signing_required` was WRITTEN once, at net/smb.c:668, out
// of the server's NEGOTIATE response, and READ BY NOTHING IN ANY FILE. Grep
// over kernel/ for the identifier returned exactly two hits: the declaration
// in net/smb.h and that one assignment.
//
// `smb_connection_t::session_key[16]` was DECLARED and never written and never
// read, anywhere in the tree. So there was not merely no signature check,
// there was no key with which one could have been made: the NTLMv2
// SessionBaseKey was discarded at the point it was computed.
//
// The consequence, stated plainly: nothing authenticated the file server to
// MayteraOS. An attacker able to modify packets in flight could change the
// contents of any file read over SMB, the result of any directory listing, or
// the status of any write, and no layer below the application could tell.
//
// This also undercut the identity check that landed earlier the same day
// (advisory 0044, rustkern/smbident.rs). That check reads `session_flags` out
// of the final SESSION_SETUP response and refuses a session that came back
// anonymous when a named one was asked for. It is a real fix for a real
// misconfiguration, but it trusts the server's own unauthenticated answer
// about who you are: an attacker in the path could simply clear the
// SMB2_SESSION_FLAG_IS_GUEST bit. Verifying the signature on that very
// response, which is what `smbsign_check_response()` is used for at that point in
// net/smb.c, is what turns it from a configuration check into an
// authenticated one.
//
// ===========================================================================
// WHICH ALGORITHM, AND WHY THE OBVIOUS ANSWER IS WRONG
// ---------------------------------------------------------------------------
// It is widely repeated that "SMB 2.0 through 3.0.2 signs with HMAC-SHA256".
// That is FALSE for the 3.x half and it matters here, because the dialect this
// client actually negotiates against Samba is 3.0.2. MS-SMB2 3.1.4.1:
//
//   dialect 2.0.2, 2.1       -> HMAC-SHA256, SigningKey = SessionKey
//   dialect 3.0, 3.0.2       -> AES-128-CMAC, SigningKey = KDF(SessionKey, ...)
//   dialect 3.1.1            -> AES-128-CMAC keyed off a pre-auth integrity
//                               hash, or AES-128-GMAC if negotiated
//
// MEASURED 2026-10-04 against Samba 4.17.12-Debian with `server max protocol`
// at its default: this client offers 2.0.2 / 2.1 / 3.0 / 3.0.2 and the server
// picks 0x0302. So the LIVE path is AES-CMAC. Implementing only HMAC-SHA256
// and calling SMB signing done would have produced a module that signs nothing
// on the only dialect the system actually speaks. Both are implemented here.
//
// SMB 3.1.1 IS NOT COVERED AND IS NOT OFFERED. It needs the pre-auth integrity
// hash (a running SHA-512 over every NEGOTIATE and SESSION_SETUP message) fed
// into the key derivation, which is a materially larger job than this one.
// net/smb.c does not offer 0x0311 in its dialect list, so the server cannot
// select it; and if one ever answers with it anyway, `smbsign_begin()` refuses
// the dialect rather than guessing at a key schedule. A guessed key schedule
// does not fail loudly: it produces a signature the server rejects, or worse,
// a verification that passes something it should not.
//
// ===========================================================================
// THE KEY SCHEDULE
// ---------------------------------------------------------------------------
// For 3.0 and 3.0.2 the signing key is derived with the SP800-108 counter-mode
// KDF, PRF = HMAC-SHA256, one iteration (MS-SMB2 3.1.4.2):
//
//   SigningKey = HMAC-SHA256(SessionKey,
//                   0x00000001            (counter i, 4 bytes big-endian)
//                || "SMB2AESCMAC\0"       (Label, INCLUDING its NUL)
//                || 0x00                  (the SP800-108 separator byte)
//                || "SmbSign\0"           (Context, INCLUDING its NUL)
//                || 0x00000080)[0..16]    (L = 128 bits, 4 bytes big-endian)
//
// The label and context each carry their own terminating NUL AND there is a
// separate 0x00 separator between them. That is three NULs in a 29-byte PRF
// input and it is the single easiest thing in this file to get wrong by one
// byte, which is why the layout is pinned by a cross-implementation
// known-answer test in `smbsign_selftest_rs()` rather than by a comment.
//
// ===========================================================================
// WHAT A SIGNATURE COVERS
// ---------------------------------------------------------------------------
// The MAC is taken over the WHOLE SMB2 message, header and body, with the
// 16-byte Signature field at offset 48 ZEROED and SMB2_FLAGS_SIGNED set in the
// Flags field at offset 16. The 4-byte NetBIOS length prefix is NOT part of
// it. The leftmost 16 bytes of the MAC are the signature, which for AES-CMAC
// is the whole output and for HMAC-SHA256 is a truncation from 32.
//
// Every offset in this file was read out of `smb2_header_t` in net/smb.h field
// by field and not assumed: protocol_id 0, structure_size 4, credit_charge 6,
// status 8, command 12, credit_req_resp 14, flags 16, next_command 20,
// message_id 24, process_id 32, tree_id 36, session_id 40, signature 48..64.

use core::sync::atomic::{AtomicU32, Ordering};

// ===========================================================================
// The two primitives, both already in the tree. Nothing is reimplemented.
// ===========================================================================
extern "C" {
    /// crypto/crypto.h: one-shot HMAC-SHA256. `mac` takes 32 bytes.
    fn hmac_sha256(key: *const u8, key_len: usize,
                   data: *const u8, data_len: usize, mac: *mut u8);
    /// crypto/crypto.h: RFC 4493 AES-CMAC over AES-128. `mac` takes 16 bytes.
    fn aes_cmac(key: *const u8, msg: *const u8, msg_len: usize, mac: *mut u8);
}

// ===========================================================================
// Wire geometry. Read from net/smb.h's smb2_header_t, not assumed.
// ===========================================================================
const SMB2_HDR_LEN: usize = 64;
const OFF_MAGIC: usize = 0;
const OFF_STATUS: usize = 8;
const OFF_COMMAND: usize = 12;
const OFF_FLAGS: usize = 16;
const OFF_SESSION_ID: usize = 40;
const OFF_SIGNATURE: usize = 48;
const SIG_LEN: usize = 16;

const SMB2_MAGIC: u32 = 0x424D53FE; // "\xFESMB" read as a little-endian u32
const SMB2_FLAGS_SIGNED: u32 = 0x0000_0008;
const SMB2_FLAGS_ASYNC_COMMAND: u32 = 0x0000_0002;
const STATUS_PENDING: u32 = 0x0000_0103;
const SMB2_NEGOTIATE: u16 = 0x0000;
const SMB2_SESSION_SETUP: u16 = 0x0001;

// Dialect revisions, from net/smb.h.
const DIALECT_202: u16 = 0x0202;
const DIALECT_210: u16 = 0x0210;
const DIALECT_300: u16 = 0x0300;
const DIALECT_302: u16 = 0x0302;
/// SMB 3.1.1. Its key schedule lives in rustkern/smbpreauth.rs, because it
/// needs the pre-authentication integrity hash, not just the session key.
const DIALECT_311: u16 = 0x0311;

// Server NEGOTIATE SecurityMode bits, from net/smb.h.
const SIGNING_ENABLED: u16 = 0x0001;
const SIGNING_REQUIRED: u16 = 0x0002;

// ===========================================================================
// Algorithms and refusal codes.
//
// MIRRORED IN net/smb.h ONLY FOR THE POLICY VALUES, which the C side has to be
// able to name. The refusal codes are deliberately NOT mirrored: C never
// compares one, it only hands it back to `smbsign_reason_text()`. A constant
// mirrored on both sides is a thing that can drift, and no _Static_assert can
// catch a mirrored constant holding the WRONG VALUE (the lesson smbident.rs
// records).
// ===========================================================================
const ALGO_NONE: u8 = 0;
const ALGO_HMAC_SHA256: u8 = 1;
const ALGO_AES_CMAC: u8 = 2;

const R_OK: u16 = 0;
const R_NOT_ARMED: u16 = 1;
const R_SHORT: u16 = 2;
const R_NOT_SMB2: u16 = 3;
const R_UNSIGNED: u16 = 4;
const R_BAD_SIGNATURE: u16 = 5;
const R_DIALECT: u16 = 6;
const R_SERVER_REFUSES: u16 = 7;
const R_NO_SESSION_KEY: u16 = 8;
const R_NULL: u16 = 9;
/// The key derivation refused its inputs. Previously impossible to express:
/// kdf_128 had no failure path and overran its scratch instead.
const R_KDF: u16 = 10;
/// SMB 3.1.1 was passed to smbsign_begin() instead of smbsign_begin311().
const R_DIALECT_311_WRONG_ENTRY: u16 = 11;

/// Policy values. These two ARE mirrored into net/smb.h because C sets the
/// policy, so `smbsign_policy_abi_check_rs()` below is handed C's copies and
/// refuses to agree if they ever differ.
const POLICY_ENABLED: u32 = 0;
const POLICY_REQUIRED: u32 = 1;

// ===========================================================================
// THE DEFAULT IS "REQUIRED", AND THE REASONING IS MEASURED, NOT ASSUMED
// ---------------------------------------------------------------------------
// The usual argument against requiring signing by default is that it breaks
// mounts against servers that do not offer it. For SMB2 and later that
// argument does not hold up, and it was tested rather than reasoned about.
//
// MEASURED 2026-10-04, Samba 4.17.12-Debian, one server, all four settings of
// `server signing`, read off the wire with a 72-byte NEGOTIATE probe:
//
//   server signing = auto       -> SecurityMode 0x0001 (ENABLED, not required)
//   server signing = mandatory  -> SecurityMode 0x0003 (ENABLED + REQUIRED)
//   server signing = no         -> SecurityMode 0x0001 (ENABLED, not required)
//   server signing = default    -> SecurityMode 0x0001 (ENABLED, not required)
//
// SIGNING_ENABLED was set in ALL FOUR, including `no`. That is not a Samba
// quirk: in SMB2 and later, signing support is mandatory to implement, and
// `server signing = no` means "I will not INSIST on it", not "I cannot do it".
// Confirmed behaviourally on the same rig: with `server signing = no`, an
// `smbclient` run with `client signing = required` completed a full directory
// listing, so the server does sign when the client insists.
//
// So requiring signing costs approximately nothing against an SMB2+ server,
// and it closes a hole that is otherwise open on every mount. REQUIRED is
// therefore the default.
//
// WHAT THIS DEFAULT WOULD BREAK, stated honestly rather than glossed: a server
// that genuinely cannot sign. In practice that means SMB1/CIFS, which this
// module does not cover at all and which net/smb.c only retains as a
// negotiation fallback, and some embedded NAS firmware and consumer appliances
// that implement SMB2 incompletely. Those have NOT been tested here, because
// no such device was available on the rig. If one turns up, the mount will
// fail with the reason named on the serial console, and the knob below is how
// it is overruled.
//
// THE POLICY IS THE OWNER'S TO OVERRULE. This picks the safer of two defensible
// defaults and makes the other one reachable; it does not claim the choice is
// not a judgement call.
//
// HOW A USER CHANGES IT: a line in /CONFIG/SMB.CFG
//
//     signing=required     refuse any mount that cannot be signed (DEFAULT)
//     signing=enabled      sign when the server supports it, and still VERIFY
//                          every signature that arrives, but allow a mount to
//                          an SMB2 server that will not sign at all
//
// Note what `enabled` does NOT do: it does not stop verifying. A signature
// that is present and wrong is rejected under both policies. The only thing
// the policy changes is whether a server that offers no signing at all is
// allowed to be mounted.
static POLICY: AtomicU32 = AtomicU32::new(POLICY_REQUIRED);

// ===========================================================================
// Per-connection state. Mirrored by `smb_sign_t` in net/smb.h, size-locked
// there by _Static_assert so a change on either side fails the build instead
// of reading the wrong offsets.
//
// THE SESSION KEY IS NOT KEPT HERE. It is consumed by `smbsign_begin()` and
// the caller zeroizes its copy. Only the derived signing key lives for the
// length of the connection, which is one fewer password-equivalent secret in
// a struct that survives into any crash dump (the reasoning net/smb.h already
// records for storing the NT hash instead of the password).
// ===========================================================================
#[repr(C)]
pub struct SmbSign {
    /// The derived signing key. 16 bytes for both algorithms.
    key: [u8; 16],
    /// ALGO_*: which MAC this connection's dialect uses.
    algo: u8,
    /// 1 once a key is derived. Until then nothing is signed and nothing is
    /// required, which is correct: NEGOTIATE precedes any key material.
    armed: u8,
    /// The policy this connection was brought up under, captured at NEGOTIATE
    /// so a mid-mount config change cannot retroactively weaken a live mount.
    policy: u8,
    /// 1 if the server said SIGNING_REQUIRED in its NEGOTIATE response.
    server_requires: u8,
    /// Evidence counters. These exist so a claim about signing can be checked
    /// against a number rather than against a log line that may not have been
    /// reached.
    signed_out: u32,
    verified_in: u32,
    rejected_in: u32,
    /// Responses ACCEPTED WITHOUT A SIGNATURE under POLICY_ENABLED. Counted
    /// separately from `verified_in` on purpose: lumping them together would
    /// let a mount that was never protected report the same number as one that
    /// was, which is the kind of reassuring statistic this tree has been
    /// bitten by before.
    unsigned_in: u32,
}

const SMBSIGN_ZERO: SmbSign = SmbSign {
    key: [0u8; 16],
    algo: ALGO_NONE,
    armed: 0,
    policy: POLICY_REQUIRED as u8,
    server_requires: 0,
    signed_out: 0,
    verified_in: 0,
    rejected_in: 0,
    unsigned_in: 0,
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
fn wr_u32(b: &mut [u8], off: usize, v: u32) {
    let x = v.to_le_bytes();
    b[off] = x[0];
    b[off + 1] = x[1];
    b[off + 2] = x[2];
    b[off + 3] = x[3];
}

/// Constant-time 16-byte comparison. Returns true on equal.
///
/// A `memcmp` here would leak, through its early exit, how many leading bytes
/// of a guessed signature were right, which is the standard way a MAC check is
/// turned into a byte-at-a-time forgery oracle. The accumulate-then-test shape
/// below reads every byte on every call regardless of the outcome.
#[inline(never)]
fn ct_eq16(a: &[u8], b: &[u8]) -> bool {
    let mut diff = 0u8;
    let mut i = 0;
    while i < SIG_LEN {
        diff |= a[i] ^ b[i];
        i += 1;
    }
    diff == 0
}

fn zeroize(b: &mut [u8]) {
    let mut i = 0;
    while i < b.len() {
        // write_volatile so the compiler may not elide the wipe of a buffer
        // that is never read again.
        unsafe { core::ptr::write_volatile(&mut b[i], 0u8) };
        i += 1;
    }
}

/// SP800-108 counter-mode KDF with PRF = HMAC-SHA256, single iteration,
/// 128-bit output (MS-SMB2 3.1.4.2). See the file header for the byte layout.
fn kdf_128(ki: &[u8; 16], label: &[u8], context: &[u8],
           out: &mut [u8; 16]) -> bool {
    // 4 (counter) + label + 1 (separator) + context + 4 (L).
    //
    // WIDENED from 48 to 128 for SMB 3.1.1 (rustkern/smbpreauth.rs), whose
    // context is a 64-byte SHA-512 pre-auth integrity hash and whose longest
    // label is "SMBC2SCipherKey\0" at 16 bytes: 4 + 16 + 1 + 64 + 4 = 89. The
    // 3.0.2 callers need 29 and 30. 128 leaves room without being a guess.
    //
    // AND IT IS NOW BOUNDS-CHECKED HERE, not only in the exported wrapper.
    // Before this, kdf_128 indexed a fixed 48-byte array with no check of its
    // own and relied entirely on the caller for safety; a new in-module caller
    // with a longer label would have written past the end. A key derivation
    // that overruns its scratch is not a bug you find in testing, because the
    // wrong key it produces looks exactly like the right one until something
    // on the far end disagrees.
    const SCRATCH: usize = 128;
    if 4 + label.len() + 1 + context.len() + 4 > SCRATCH {
        return false;
    }
    let mut buf = [0u8; SCRATCH];
    let mut n = 0usize;

    // i = 1, big-endian.
    buf[n] = 0x00; buf[n + 1] = 0x00; buf[n + 2] = 0x00; buf[n + 3] = 0x01;
    n += 4;

    let mut i = 0;
    while i < label.len() { buf[n] = label[i]; n += 1; i += 1; }

    // The SP800-108 separator. This is IN ADDITION to the NUL that the label
    // itself carries; see the file header.
    buf[n] = 0x00;
    n += 1;

    i = 0;
    while i < context.len() { buf[n] = context[i]; n += 1; i += 1; }

    // L = 128 bits, big-endian.
    buf[n] = 0x00; buf[n + 1] = 0x00; buf[n + 2] = 0x00; buf[n + 3] = 0x80;
    n += 4;

    let mut full = [0u8; 32];
    // SAFETY: hmac_sha256 reads ki (16 bytes) and buf (n <= 48 bytes), and
    // writes exactly 32 bytes to `full`. All three are local arrays of at
    // least those sizes and none of the pointers is retained.
    unsafe {
        hmac_sha256(ki.as_ptr(), 16, buf.as_ptr(), n, full.as_mut_ptr());
    }
    out.copy_from_slice(&full[0..16]);
    zeroize(&mut full);
    zeroize(&mut buf);
    true
}

/// The SAME KDF, exported for rustkern/smbcrypt.rs to derive the SMB3
/// ENCRYPTION keys with.
///
/// It is shared rather than copied because the byte layout above, with its
/// three NULs in a 29-byte PRF input, is the single most error-prone thing in
/// either module, and two copies of an error-prone layout is two things to get
/// wrong. The encryption module passes different labels ("SMB2AESCCM\0" with
/// "ServerIn \0" / "ServerOut\0", MS-SMB2 3.1.4.2) to the same bytes.
///
/// `label_len` and `context_len` INCLUDE each string's terminating NUL,
/// because the KDF input does.
#[no_mangle]
pub extern "C" fn smbsign_kdf128_shared(ki: *const u8,
                                        label: *const u8, label_len: usize,
                                        context: *const u8, context_len: usize,
                                        out: *mut u8) {
    if ki.is_null() || label.is_null() || context.is_null() || out.is_null() {
        return;
    }
    // SAFETY: the caller guarantees 16 readable bytes at ki, label_len at
    // label, context_len at context and 16 writable bytes at out. No pointer
    // is retained past this call.
    unsafe {
        let k: &[u8; 16] = &*(ki as *const [u8; 16]);
        let l: &[u8] = core::slice::from_raw_parts(label, label_len);
        let c: &[u8] = core::slice::from_raw_parts(context, context_len);
        let mut o = [0u8; 16];
        if !kdf_128(k, l, c, &mut o) {
            // This function's signature returns nothing, so an oversize input
            // used to leave `out` UNTOUCHED, which is indistinguishable from
            // success to the caller and would have it proceed on whatever was
            // already in its buffer. Zeroing is still a silent failure, but an
            // all-zero key is at least DETECTABLE, and every caller in the tree
            // already refuses an all-zero key by name. New code should call
            // smbsign_kdf128_ctx() below, which returns a code.
            zeroize(&mut o);
            core::ptr::copy_nonoverlapping(o.as_ptr(), out, 16);
            return;
        }
        core::ptr::copy_nonoverlapping(o.as_ptr(), out, 16);
        zeroize(&mut o);
    }
}

/// The same KDF, with a LONG context and a RETURN CODE.
///
/// SMB 3.1.1 derives every session key with the 64-byte SHA-512
/// pre-authentication integrity hash as the SP800-108 context
/// (rustkern/smbpreauth.rs). The older `smbsign_kdf128_shared()` cannot be used
/// for that for two separate reasons, either of which alone is disqualifying:
/// its scratch could not hold a 64-byte context, and it returns `void`, so its
/// refusal was invisible. A key schedule whose failure is invisible is worse
/// than one that does not exist, because the session comes up and the keys are
/// wrong.
///
/// `label_len` and `context_len` INCLUDE the label's terminating NUL, because
/// the KDF input does. Returns 0 on success, -1 on a null pointer, -2 if the
/// inputs do not fit the scratch. `out` is left all-zero on any failure.
#[no_mangle]
pub extern "C" fn smbsign_kdf128_ctx(ki: *const u8,
                                     label: *const u8, label_len: usize,
                                     context: *const u8, context_len: usize,
                                     out: *mut u8) -> i32 {
    if ki.is_null() || label.is_null() || context.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: the caller guarantees 16 readable bytes at ki, label_len at
    // label, context_len at context and 16 writable bytes at out. No pointer
    // is retained past this call.
    unsafe {
        let k: &[u8; 16] = &*(ki as *const [u8; 16]);
        let l: &[u8] = core::slice::from_raw_parts(label, label_len);
        let c: &[u8] = core::slice::from_raw_parts(context, context_len);
        let mut o = [0u8; 16];
        let ok = kdf_128(k, l, c, &mut o);
        if !ok {
            zeroize(&mut o);
            core::ptr::copy_nonoverlapping(o.as_ptr(), out, 16);
            return -2;
        }
        core::ptr::copy_nonoverlapping(o.as_ptr(), out, 16);
        zeroize(&mut o);
    }
    0
}

/// Compute the 16-byte signature of `msg` under this connection's key and
/// algorithm. `msg` must already be in its canonical signing form: Signature
/// field zeroed and SMB2_FLAGS_SIGNED set.
fn mac16(st: &SmbSign, msg: &[u8], out: &mut [u8; SIG_LEN]) {
    match st.algo {
        ALGO_HMAC_SHA256 => {
            let mut full = [0u8; 32];
            // SAFETY: key is 16 bytes, msg is a live slice of msg.len()
            // readable bytes, and `full` has the 32 bytes hmac_sha256 writes.
            unsafe {
                hmac_sha256(st.key.as_ptr(), 16, msg.as_ptr(), msg.len(),
                            full.as_mut_ptr());
            }
            out.copy_from_slice(&full[0..16]);
            zeroize(&mut full);
        }
        ALGO_AES_CMAC => {
            // SAFETY: key is 16 bytes (AES-128), msg is a live slice of
            // msg.len() readable bytes, `out` has the 16 bytes aes_cmac writes.
            unsafe {
                aes_cmac(st.key.as_ptr(), msg.as_ptr(), msg.len(),
                         out.as_mut_ptr());
            }
        }
        _ => {
            // Unreachable while armed; an all-zero signature is never accepted
            // by ct_eq16 against a real MAC, so the failure is closed.
            let mut i = 0;
            while i < SIG_LEN { out[i] = 0; i += 1; }
        }
    }
}

// ===========================================================================
// Policy, global.
// ===========================================================================

/// Set the signing policy. 0 = enabled/opportunistic, 1 = required.
/// Anything else is treated as REQUIRED, because an unparseable policy must
/// not be the weaker one.
#[no_mangle]
pub extern "C" fn smbsign_policy_set(policy: u32) {
    let p = if policy == POLICY_ENABLED { POLICY_ENABLED } else { POLICY_REQUIRED };
    POLICY.store(p, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn smbsign_policy() -> u32 {
    POLICY.load(Ordering::Acquire)
}

/// Parse a /CONFIG/SMB.CFG buffer for a `signing=` line.
/// Returns 1 = required, 0 = enabled, -1 = no `signing=` key present (leave
/// the default alone), -2 = the key is present with a value we do not know.
///
/// An UNRECOGNISED value is NOT silently treated as the default. A config file
/// that says `signing=yes` (a plausible thing for someone to type) must be
/// reported, because a typo that silently resolves to a policy is how a
/// machine ends up running one the operator did not choose.
#[no_mangle]
pub extern "C" fn smbsign_policy_parse(buf: *const u8, len: i32) -> i32 {
    if buf.is_null() || len <= 0 {
        return -1;
    }
    // SAFETY: the caller (net/smb.c, from fat_read_file) guarantees `len`
    // readable bytes at `buf`. The slice is not retained past this call.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    let key = b"signing";
    let mut i = 0usize;
    while i < b.len() {
        // Start of a line: skip leading blanks.
        let mut p = i;
        while p < b.len() && (b[p] == b' ' || b[p] == b'\t') { p += 1; }

        // A comment line is not a setting.
        let is_comment = p < b.len() && (b[p] == b'#' || b[p] == b';');

        if !is_comment && p + key.len() < b.len() {
            let mut k = 0usize;
            let mut hit = true;
            while k < key.len() {
                // SMB config keys are matched case-insensitively; a file that
                // says `Signing=` must not be ignored.
                let c = b[p + k] | 0x20;
                if c != key[k] { hit = false; break; }
                k += 1;
            }
            if hit {
                let mut q = p + key.len();
                while q < b.len() && (b[q] == b' ' || b[q] == b'\t') { q += 1; }
                if q < b.len() && b[q] == b'=' {
                    q += 1;
                    while q < b.len() && (b[q] == b' ' || b[q] == b'\t') { q += 1; }
                    // Collect the value token.
                    let vstart = q;
                    while q < b.len() && b[q] != b'\n' && b[q] != b'\r'
                        && b[q] != b' ' && b[q] != b'\t' && b[q] != b'#' { q += 1; }
                    let v = &b[vstart..q];
                    if tok_eq(v, b"required") { return 1; }
                    if tok_eq(v, b"enabled") { return 0; }
                    return -2;
                }
            }
        }

        // Advance to the start of the next line.
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

/// Lock the two policy constants C mirrors against this module's copies.
/// Returns 0 on agreement. main.c prints the verdict at boot, so a drift is
/// visible rather than latent.
#[no_mangle]
pub extern "C" fn smbsign_policy_abi_check(c_enabled: u32, c_required: u32) -> i32 {
    if c_enabled != POLICY_ENABLED { return 1; }
    if c_required != POLICY_REQUIRED { return 2; }
    0
}

// ===========================================================================
// Per-connection lifecycle.
// ===========================================================================

/// Clear a connection's signing state. Called where the socket is (re)made, so
/// a revived connection can never carry the previous session's signing key:
/// the key is bound to a session, and a new session has a new one.
#[no_mangle]
pub extern "C" fn smbsign_reset(st: *mut SmbSign) {
    if st.is_null() { return; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbSign.
    let s = unsafe { &mut *st };
    zeroize(&mut s.key);
    *s = SMBSIGN_ZERO;
    s.policy = smbsign_policy() as u8;
}

/// What this client puts in the SecurityMode field of its own NEGOTIATE
/// request: ENABLED always, plus REQUIRED when our policy demands it.
///
/// Announcing REQUIRED is what makes a server that merely SUPPORTS signing
/// actually sign its replies. Without it, a server set to `server signing =
/// no` answers unsigned and we would be left refusing a mount that could have
/// worked (measured: see the policy comment above).
#[no_mangle]
pub extern "C" fn smbsign_client_security_mode() -> u16 {
    if smbsign_policy() == POLICY_REQUIRED {
        SIGNING_ENABLED | SIGNING_REQUIRED
    } else {
        SIGNING_ENABLED
    }
}

/// Record what the server said in its NEGOTIATE response, and decide whether
/// the connection may proceed. Returns 0 to continue, else a refusal code.
///
/// THE DOWNGRADE GATE. A server that does not advertise signing at all cannot
/// sign, so under POLICY_REQUIRED the mount must fail HERE, before any
/// credential is sent, rather than coming up unsigned. That is the same defect
/// class as the silent downgrade to guest that advisory 0044 closed: a mount
/// that works but is not the mount that was asked for.
#[no_mangle]
pub extern "C" fn smbsign_note_negotiate(st: *mut SmbSign, dialect: u16,
                                         server_security_mode: u16) -> u16 {
    if st.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbSign.
    let s = unsafe { &mut *st };
    s.server_requires = ((server_security_mode & SIGNING_REQUIRED) != 0) as u8;
    s.policy = smbsign_policy() as u8;

    let server_can_sign = (server_security_mode & SIGNING_ENABLED) != 0
        || (server_security_mode & SIGNING_REQUIRED) != 0;

    if s.policy as u32 == POLICY_REQUIRED {
        if !server_can_sign {
            return R_SERVER_REFUSES;
        }
        // A dialect whose key schedule we do not implement is refused here,
        // not at the first signed message. 0x0311 is not offered by
        // net/smb.c, so reaching this means the server answered with a
        // dialect nobody asked for.
        if !dialect_known(dialect) {
            return R_DIALECT;
        }
    }
    R_OK
}

fn dialect_known(d: u16) -> bool {
    // 0x0311 joins the list now that rustkern/smbpreauth.rs implements its key
    // schedule. It is keyed through smbsign_begin311() rather than
    // smbsign_begin(), because its signing key needs the pre-auth integrity
    // hash and smbsign_begin()'s signature has nowhere to put one.
    d == DIALECT_202 || d == DIALECT_210 || d == DIALECT_300
        || d == DIALECT_302 || d == DIALECT_311
}

/// Arm signing for SMB 3.1.1 with a signing key DERIVED FROM THE
/// PRE-AUTHENTICATION INTEGRITY HASH.
///
/// This is the whole mechanism in one function. At 3.0.2 the signing key is a
/// function of the session key alone, so a tampered handshake still produces a
/// working session. At 3.1.1 it is a function of the session key AND a SHA-512
/// over every handshake message, so if an attacker edited any of those messages
/// our key and the server's key differ, and the very next signature check
/// fails. The handshake is authenticated RETROACTIVELY, by the keys it produced.
///
/// `preauth_hash` is 64 bytes, supplied by smbpreauth_derive(); the algorithm is
/// AES-128-CMAC, which is what Samba 4.17.12 was MEASURED selecting when offered
/// a SIGNING_CAPABILITIES context of AES-CMAC alone, and is also the specified
/// default when no such context is sent. AES-128-GMAC is NOT implemented in this
/// kernel, which is why the client never offers it.
#[no_mangle]
pub extern "C" fn smbsign_begin311(st: *mut SmbSign,
                                   signing_key: *const u8) -> u16 {
    if st.is_null() || signing_key.is_null() { return R_NULL; }
    // SAFETY: the caller guarantees a live, aligned, uniquely-owned SmbSign and
    // 16 readable bytes at signing_key. Neither pointer is retained.
    let s = unsafe { &mut *st };
    let sk: &[u8] = unsafe { core::slice::from_raw_parts(signing_key, 16) };

    // An all-zero key here means the derivation upstream failed and said so by
    // zeroing its output. Arming with it would sign every message with a key
    // every other party could compute.
    let mut any = 0u8;
    let mut i = 0;
    while i < 16 { any |= sk[i]; i += 1; }
    if any == 0 {
        return R_NO_SESSION_KEY;
    }
    s.key.copy_from_slice(sk);
    s.algo = ALGO_AES_CMAC;
    s.armed = 1;
    R_OK
}

/// Derive the signing key and arm the connection. Called once, immediately
/// after the final SESSION_SETUP response arrives and the NTLMv2 session key
/// has been computed. Returns 0 on success, else a refusal code.
///
/// `session_key` is the 16-byte NTLMv2 ExportedSessionKey. With
/// NTLMSSP_NEGOTIATE_KEY_EXCH not negotiated (net/smb.c does not set it) that
/// is equal to the SessionBaseKey = HMAC-MD5(NTOWFv2, NTProofStr), per
/// MS-NLMP 3.1.5.1.2.
#[no_mangle]
pub extern "C" fn smbsign_begin(st: *mut SmbSign, dialect: u16,
                                session_key: *const u8) -> u16 {
    if st.is_null() || session_key.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbSign and 16
    // readable bytes at session_key. Neither pointer is retained.
    let s = unsafe { &mut *st };
    let sk: &[u8] = unsafe { core::slice::from_raw_parts(session_key, 16) };

    // An all-zero session key means the NTLM exchange did not produce one,
    // which for an anonymous or guest session is exactly what happens. Signing
    // with a key of zeros would "work" in the sense of producing matching
    // signatures with anyone else who also has zeros, i.e. everybody. Refuse.
    let mut any = 0u8;
    let mut i = 0;
    while i < 16 { any |= sk[i]; i += 1; }
    if any == 0 {
        return R_NO_SESSION_KEY;
    }

    let mut ki = [0u8; 16];
    ki.copy_from_slice(sk);

    match dialect {
        DIALECT_202 | DIALECT_210 => {
            // MS-SMB2 3.1.4.1: SigningKey IS the SessionKey, MAC is
            // HMAC-SHA256 truncated to 16 bytes.
            s.key.copy_from_slice(&ki);
            s.algo = ALGO_HMAC_SHA256;
        }
        DIALECT_300 | DIALECT_302 => {
            let mut derived = [0u8; 16];
            if !kdf_128(&ki, b"SMB2AESCMAC\0", b"SmbSign\0", &mut derived) {
                zeroize(&mut derived);
                zeroize(&mut ki);
                return R_KDF;
            }
            s.key.copy_from_slice(&derived);
            s.algo = ALGO_AES_CMAC;
            zeroize(&mut derived);
        }
        DIALECT_311 => {
            // Reached only if a caller used the WRONG entry point. 3.1.1's
            // signing key is a function of the pre-auth integrity hash, which
            // this function has no argument for, so guessing one here would
            // produce a plausible-looking key that signs nothing the server
            // will accept. Refuse by name and let smbsign_begin311() do it.
            zeroize(&mut ki);
            return R_DIALECT_311_WRONG_ENTRY;
        }
        _ => {
            zeroize(&mut ki);
            return R_DIALECT;
        }
    }

    zeroize(&mut ki);
    s.armed = 1;
    R_OK
}

#[no_mangle]
pub extern "C" fn smbsign_is_armed(st: *const SmbSign) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbSign for the read.
    let s = unsafe { &*st };
    (s.armed != 0 && s.algo != ALGO_NONE) as i32
}

/// Does this connection's policy mean an unsigned response must be refused?
#[no_mangle]
pub extern "C" fn smbsign_requires(st: *const SmbSign) -> i32 {
    if st.is_null() { return 0; }
    // SAFETY: caller guarantees a live, aligned SmbSign for the read.
    let s = unsafe { &*st };
    if s.armed == 0 { return 0; }
    (s.policy as u32 == POLICY_REQUIRED || s.server_requires != 0) as i32
}

#[no_mangle]
pub extern "C" fn smbsign_algo(st: *const SmbSign) -> u16 {
    if st.is_null() { return ALGO_NONE as u16; }
    // SAFETY: caller guarantees a live, aligned SmbSign for the read.
    unsafe { (*st).algo as u16 }
}

#[no_mangle]
pub extern "C" fn smbsign_algo_text(algo: u16) -> *const u8 {
    match algo as u8 {
        ALGO_HMAC_SHA256 => b"HMAC-SHA256 (SMB 2.0.2/2.1)\0".as_ptr(),
        ALGO_AES_CMAC => b"AES-128-CMAC (SMB 3.0/3.0.2/3.1.1)\0".as_ptr(),
        _ => b"none\0".as_ptr(),
    }
}

#[no_mangle]
pub extern "C" fn smbsign_counts(st: *const SmbSign, out_signed: *mut u32,
                                 out_verified: *mut u32, out_rejected: *mut u32,
                                 out_unsigned: *mut u32) {
    if st.is_null() { return; }
    // SAFETY: caller guarantees a live SmbSign and, for each non-null out
    // pointer, one writable u32.
    unsafe {
        let s = &*st;
        if !out_signed.is_null() { *out_signed = s.signed_out; }
        if !out_verified.is_null() { *out_verified = s.verified_in; }
        if !out_rejected.is_null() { *out_rejected = s.rejected_in; }
        if !out_unsigned.is_null() { *out_unsigned = s.unsigned_in; }
    }
}

// ===========================================================================
// Sign and verify. These two are the point of the file.
// ===========================================================================

/// Is this message one that is signed at all? NEGOTIATE never is (no key
/// exists yet) and neither is a SESSION_SETUP request (the session is still
/// being built). Both are decided by the command code rather than by the
/// caller remembering, so a new SMB2 command added later is signed by default.
fn command_is_presession(cmd: u16) -> bool {
    cmd == SMB2_NEGOTIATE || cmd == SMB2_SESSION_SETUP
}

/// Sign an outbound request in place.
///
/// Returns 0 if the message was signed, R_NOT_ARMED if this connection has no
/// key yet (which is the normal, correct answer for NEGOTIATE and
/// SESSION_SETUP), else a refusal code. The caller must NOT send a message
/// that came back with any code other than 0 or R_NOT_ARMED.
#[no_mangle]
pub extern "C" fn smbsign_sign(st: *mut SmbSign, pdu: *mut u8, len: usize) -> u16 {
    if st.is_null() || pdu.is_null() { return R_NULL; }
    if len < SMB2_HDR_LEN { return R_SHORT; }
    // SAFETY: the caller (net/smb.c smb_send_packet) guarantees `len` readable
    // AND writable bytes at `pdu`: it is always smb_send_buffer, a static
    // array of SMB_BUFFER_SIZE bytes, and `len` is the length it is about to
    // transmit from it. The slice is not retained past this call.
    let s = unsafe { &mut *st };
    let m: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(pdu, len) };

    if rd_u32(m, OFF_MAGIC) != SMB2_MAGIC { return R_NOT_SMB2; }
    if s.armed == 0 || s.algo == ALGO_NONE { return R_NOT_ARMED; }
    if command_is_presession(rd_u16(m, OFF_COMMAND)) { return R_NOT_ARMED; }

    // MS-SMB2 3.1.4.1: set SMB2_FLAGS_SIGNED, zero the Signature field, MAC
    // the whole message, then write the MAC into the Signature field.
    let flags = rd_u32(m, OFF_FLAGS);
    wr_u32(m, OFF_FLAGS, flags | SMB2_FLAGS_SIGNED);
    let mut i = 0;
    while i < SIG_LEN { m[OFF_SIGNATURE + i] = 0; i += 1; }

    let mut mac = [0u8; SIG_LEN];
    mac16(s, m, &mut mac);

    i = 0;
    while i < SIG_LEN { m[OFF_SIGNATURE + i] = mac[i]; i += 1; }

    s.signed_out = s.signed_out.wrapping_add(1);
    R_OK
}

/// Verify an inbound response in place.
///
/// Returns 0 if the signature is present and correct, else a refusal code. The
/// caller must treat ANY non-zero return as "this response is not evidence of
/// anything" and must not act on its contents.
///
/// R_UNSIGNED is returned when the response carries no signature at all. That
/// is reported separately from R_BAD_SIGNATURE because the two mean different
/// things to whoever reads the serial log: one is a server (or an attacker)
/// declining to sign, the other is a signature that does not match, which
/// means the bytes were altered or the key is not shared.
///
/// THE BUFFER IS RESTORED ON EVERY PATH. Verification has to put the message
/// back into the canonical form the server signed, which means zeroing the
/// Signature field in place. The saved bytes are written back before this
/// function returns on every outcome, so the caller's buffer still holds what
/// arrived on the wire and a later reader of it (the dump in net/smb.c's
/// refusal path, for one) sees the real packet.
///
/// THIS IS NOT THE C-FACING ENTRY POINT. `smbsign_check_response()` below is.
/// This one is private so that the decision about which refusals are tolerable
/// lives here and not in net/smb.c: a C caller comparing against a specific
/// refusal code would be a mirrored constant, which is the thing this module
/// and smbident.rs both refuse to have.
fn verify_one(s: &mut SmbSign, pdu: *mut u8, len: usize) -> u16 {
    if pdu.is_null() { return R_NULL; }
    if len < SMB2_HDR_LEN { return R_SHORT; }
    // SAFETY: the caller (net/smb.c smb_recv_packet) guarantees `len` readable
    // AND writable bytes at `pdu`: it is always smb_recv_buffer, a static
    // array of SMB_BUFFER_SIZE bytes, and `len` is the byte count just read
    // into it. The slice is not retained past this call.
    let m: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(pdu, len) };

    if rd_u32(m, OFF_MAGIC) != SMB2_MAGIC { return R_NOT_SMB2; }
    if s.armed == 0 || s.algo == ALGO_NONE { return R_NOT_ARMED; }

    let flags = rd_u32(m, OFF_FLAGS);
    if (flags & SMB2_FLAGS_SIGNED) == 0 {
        return R_UNSIGNED;
    }

    // Save the signature, then zero it to rebuild the canonical signing form.
    let mut wire = [0u8; SIG_LEN];
    let mut i = 0;
    while i < SIG_LEN {
        wire[i] = m[OFF_SIGNATURE + i];
        m[OFF_SIGNATURE + i] = 0;
        i += 1;
    }

    let mut mac = [0u8; SIG_LEN];
    mac16(s, m, &mut mac);

    let ok = ct_eq16(&mac, &wire);

    // Restore. ONE exit point after this, so there is no path that leaves the
    // caller's buffer holding a zeroed signature field.
    i = 0;
    while i < SIG_LEN { m[OFF_SIGNATURE + i] = wire[i]; i += 1; }
    zeroize(&mut mac);

    if ok { R_OK } else { R_BAD_SIGNATURE }
}

/// May an UNSIGNED response be let through? Only an async interim response,
/// which MS-SMB2 2.2.1.2 defines as one carrying STATUS_PENDING with
/// SMB2_FLAGS_ASYNC_COMMAND set.
///
/// WHY THIS EXEMPTION EXISTS AND WHAT IT COSTS, stated rather than buried: an
/// interim response carries no payload, only "still working on it", so letting
/// an unsigned one through cannot deliver forged file contents or a forged
/// directory listing. The real answer that follows it must still be signed.
/// What an attacker CAN do with it is keep us waiting, and net/smb.c already
/// bounds that at SMB_MAX_INTERIM_RESPONSES (16) per request, after which the
/// request fails. So the exemption costs a bounded delay, not an integrity
/// hole. It is not a general "unsigned is fine sometimes" escape hatch.
fn unsigned_is_permitted(pdu: *const u8, len: usize) -> bool {
    if pdu.is_null() || len < SMB2_HDR_LEN { return false; }
    // SAFETY: caller guarantees `len` readable bytes at `pdu`; not retained.
    let m: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    if rd_u32(m, OFF_MAGIC) != SMB2_MAGIC { return false; }
    let flags = rd_u32(m, OFF_FLAGS);
    let status = rd_u32(m, OFF_STATUS);
    (status == STATUS_PENDING) && ((flags & SMB2_FLAGS_ASYNC_COMMAND) != 0)
}

/// THE INBOUND CHOKEPOINT, and the only verify entry point C uses.
///
/// Returns 0 if this response may be acted on, else a refusal code that the
/// caller passes straight to `smbsign_reason_text()`. C never compares the
/// code against anything, which is deliberate: every judgement about which
/// refusals are survivable is made here, in one place, rather than being
/// duplicated as a mirrored constant in net/smb.c.
///
/// The three outcomes:
///
///   * Not armed (no session key yet, i.e. NEGOTIATE and SESSION_SETUP) -> 0.
///     There is nothing to verify and nothing to be suspicious of.
///   * An unsigned async interim response -> 0, by the narrow exemption
///     documented on `unsigned_is_permitted` above.
///   * Anything else that is not a correct signature -> the refusal code. That
///     includes a response with NO signature, which is the cheapest downgrade
///     an attacker has and is the one a "sign outbound only" implementation
///     would wave through.
#[no_mangle]
pub extern "C" fn smbsign_check_response(st: *mut SmbSign, pdu: *mut u8,
                                         len: usize) -> u16 {
    if st.is_null() || pdu.is_null() { return R_NULL; }
    // SAFETY: caller guarantees a live, aligned, uniquely-owned SmbSign.
    let s = unsafe { &mut *st };
    if s.armed == 0 || s.algo == ALGO_NONE {
        return R_OK;
    }

    let r = verify_one(s, pdu, len);
    if r == R_OK {
        s.verified_in = s.verified_in.wrapping_add(1);
        return R_OK;
    }
    if r == R_UNSIGNED {
        // An async interim response carries no payload, so an unsigned one is
        // permitted under either policy (see unsigned_is_permitted above).
        if unsigned_is_permitted(pdu as *const u8, len) {
            return R_OK;
        }
        // A MISSING signature on a real answer is where the two policies
        // genuinely differ, and they have to, or the knob documented in this
        // file is a knob whose two settings behave identically.
        //
        // Under POLICY_ENABLED against a server that did not say
        // SIGNING_REQUIRED, an unsigned answer is let through. BE CLEAR ABOUT
        // WHAT THAT COSTS: an attacker in the network path can then simply
        // remove the signature from every response and we will accept it, so
        // POLICY_ENABLED buys almost nothing against an active attacker. It
        // exists for one case only, a server that genuinely cannot sign, and
        // it is NOT the default for exactly this reason. A signature that is
        // present and WRONG is still refused under both policies, which is the
        // one thing POLICY_ENABLED does keep.
        if (s.policy as u32) == POLICY_ENABLED && s.server_requires == 0 {
            s.unsigned_in = s.unsigned_in.wrapping_add(1);
            return R_OK;
        }
    }
    s.rejected_in = s.rejected_in.wrapping_add(1);
    r
}

/// Read the SessionId out of a response header, so net/smb.c can log it
/// without duplicating the offset.
#[no_mangle]
pub extern "C" fn smbsign_pdu_session_id(pdu: *const u8, len: usize) -> u64 {
    if pdu.is_null() || len < SMB2_HDR_LEN { return 0; }
    // SAFETY: caller guarantees `len` readable bytes at `pdu`; not retained.
    let m: &[u8] = unsafe { core::slice::from_raw_parts(pdu, len) };
    rd_u64(m, OFF_SESSION_ID)
}

#[no_mangle]
pub extern "C" fn smbsign_reason_text(code: u16) -> *const u8 {
    match code {
        R_OK => b"signature verified\0".as_ptr(),
        R_NOT_ARMED => b"this connection has no signing key yet\0".as_ptr(),
        R_SHORT => b"the message is shorter than an SMB2 header\0".as_ptr(),
        R_NOT_SMB2 => b"the message is not SMB2 (bad magic)\0".as_ptr(),
        R_UNSIGNED => b"the response carries NO SIGNATURE, and this mount requires one\0".as_ptr(),
        R_BAD_SIGNATURE =>
            b"the signature DOES NOT MATCH: the bytes were altered in flight, or this is not the server that holds the credential\0".as_ptr(),
        R_DIALECT => b"the negotiated SMB dialect has no signing key schedule in this kernel (SMB 3.1.1 is not implemented)\0".as_ptr(),
        R_SERVER_REFUSES => b"the server does not offer message signing at all\0".as_ptr(),
        R_DIALECT_311_WRONG_ENTRY =>
            b"internal error: SMB 3.1.1 needs smbsign_begin311(), whose key \
              comes from the pre-auth integrity hash\0".as_ptr(),
        R_KDF => b"internal error: the signing-key derivation refused its \
                   inputs\0".as_ptr(),
        R_NO_SESSION_KEY => b"the authentication produced no session key, so nothing can be signed\0".as_ptr(),
        _ => b"unknown signing refusal\0".as_ptr(),
    }
}

// ===========================================================================
// Boot self-test.
//
// It runs on EVERY boot including the golden, because blame.md's standing
// lesson is that an inert check and no check are indistinguishable from
// outside. Returns 0 on pass, else the number of the case that failed.
//
// THE RED ARM IS THE POINT. Cases 8 to 13 feed the verifier messages that have
// been tampered with and require it to REFUSE. A signing module that has only
// ever been shown correct signatures is untested: the failure mode this whole
// file exists to prevent is a check that returns success while doing nothing.
//
// THE KNOWN-ANSWER VECTORS ARE CROSS-IMPLEMENTATION, NOT SELF-GENERATED. The
// AES-CMAC and HMAC-SHA256 expectations come from RFC 4493 and RFC 4231. The
// SMB-specific ones (the 3.0.2 signing key and both dialects' signatures over
// a fixed 72-byte PDU) were produced by an independent Python implementation
// written from MS-SMB2 3.1.4.1/3.1.4.2 and cross-checked against Samba's
// smb2_key_derivation() byte layout, whose own AES-CMAC passes all three RFC
// 4493 vectors. A vector this module generated itself would only prove it
// agrees with itself.
// ===========================================================================
/// HOW MANY CASES THIS SELF-TEST IS EXPECTED TO RUN.
///
/// This is not a comment that can rot. `smbsign_selftest_rs()` COUNTS the
/// cases it actually ran and refuses to report a pass unless the count equals
/// this number, so adding or deleting a case without updating it fails the
/// self-test loudly instead of quietly changing the coverage.
pub const SMBSIGN_SELFTEST_CASES: u32 = 80;

/// Return-code base for "the cases are misnumbered": the Nth `case!()` to run
/// did not claim to be case N, so the case number a failure report quotes
/// would not be the position the test actually stopped at.
const RC_MISNUMBERED: u32 = 9000;
/// Return-code base for "every case passed, but the wrong NUMBER of them ran".
const RC_WRONG_TOTAL: u32 = 8000;

/// The number of cases this self-test declares, for C to print alongside the
/// number that ran. Exposed as its own symbol so the boot line can say
/// "80/80" rather than only "PASS", because a test that has silently shrunk to
/// three cases also says PASS.
#[no_mangle]
pub extern "C" fn smbsign_selftest_cases_rs() -> u32 { SMBSIGN_SELFTEST_CASES }

/// Run the self-test. Returns 0 on pass, else the number of the FIRST FAILING
/// CASE, and writes the number of cases that actually ran to `ran_out`.
///
/// IT STOPS AT THE FIRST FAILURE, and that is deliberate: a module whose key
/// schedule is wrong fails dozens of cases and the first one is the diagnosis.
/// But it means the pass/fail word ALONE hides how much of the test ran. When
/// case 25's expectation went stale (commit 1a96ca71), cases 26 to 80, 55 of
/// them, did not execute on golden 2581 and the boot line said only "FAIL
/// (first failing case 25)", which reads like one broken check rather than 55
/// unexecuted ones. Hence `ran_out`, and hence the count in the boot line.
#[no_mangle]
pub extern "C" fn smbsign_selftest_rs(ran_out: *mut u32) -> u32 {
    let mut ran: u32 = 0;
    let rc = smbsign_selftest_body(&mut ran);
    if !ran_out.is_null() {
        // SAFETY: the caller guarantees 4 writable, aligned bytes at ran_out.
        // The pointer is not retained.
        unsafe { *ran_out = ran; }
    }
    if rc != 0 { return rc; }
    // A pass is only a pass if every declared case actually ran.
    if ran != SMBSIGN_SELFTEST_CASES { return RC_WRONG_TOTAL + ran; }
    0
}

fn smbsign_selftest_body(ran: &mut u32) -> u32 {
    // Every case below is preceded by case!(N), which counts the case AND
    // asserts that it is the Nth to run. Both halves earn their keep: the
    // count is what lets the boot line tell "ran 80 of 80" from "ran 25 of
    // 80", and the assert is what stops the count and the case numbers a
    // failure quotes from drifting apart when a case is inserted in the
    // middle and its successors are not renumbered.
    macro_rules! case {
        ($n:expr) => {{
            *ran += 1;
            if *ran != $n { return RC_MISNUMBERED + *ran; }
        }};
    }

    // The fixed inputs the oracle used.
    const SESSION_KEY: [u8; 16] = [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                   0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f];
    // Oracle: SMB 3.0.2 SigningKey = KDF(SessionKey, "SMB2AESCMAC\0", "SmbSign\0").
    const SK_302: [u8; 16] = [0x24, 0xf1, 0xf0, 0xfd, 0xb2, 0x69, 0xdb, 0x98,
                              0x36, 0xd7, 0x0e, 0xfb, 0xbb, 0x97, 0x41, 0x3f];
    // Oracle: AES-CMAC signature of the fixed PDU under SK_302.
    const SIG_CMAC: [u8; 16] = [0x5a, 0x75, 0xcd, 0xac, 0xfa, 0x2b, 0x2b, 0x0c,
                                0x8e, 0xa7, 0xdc, 0x15, 0x1f, 0xa3, 0x7c, 0x0d];
    // Oracle: HMAC-SHA256 signature of the same PDU under SESSION_KEY.
    const SIG_HMAC: [u8; 16] = [0x01, 0x21, 0x01, 0x16, 0x3c, 0x67, 0x70, 0xa3,
                                0x2c, 0x43, 0x3f, 0xd2, 0xdd, 0xf8, 0xf2, 0x37];

    // Build the fixed 72-byte PDU: a TREE_CONNECT request, MessageId 7,
    // SessionId 0x1122334455667788, SMB2_FLAGS_SIGNED already set, Signature
    // zeroed, and an 8-byte body of 0xA0..0xA7.
    fn build_pdu() -> [u8; 72] {
        let mut p = [0u8; 72];
        wr_u32(&mut p, OFF_MAGIC, SMB2_MAGIC);
        p[4] = 64; p[5] = 0;                       // structure_size
        p[6] = 1;  p[7] = 0;                       // credit_charge
        p[OFF_COMMAND] = 0x03;                     // TREE_CONNECT
        p[14] = 31;                                // credit_req_resp
        wr_u32(&mut p, OFF_FLAGS, SMB2_FLAGS_SIGNED);
        p[24] = 7;                                 // message_id = 7
        let sid: [u8; 8] = [0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11];
        let mut i = 0;
        while i < 8 { p[OFF_SESSION_ID + i] = sid[i]; i += 1; }
        i = 0;
        while i < 8 { p[64 + i] = 0xA0 + i as u8; i += 1; }
        p
    }

    let mut st = SMBSIGN_ZERO;

    // ---- 1. SP800-108 KDF byte layout, against the oracle's key.
    // One wrong NUL in the 29-byte PRF input changes every byte of this.
    {
        let mut k = [0u8; 16];
        kdf_128(&SESSION_KEY, b"SMB2AESCMAC\0", b"SmbSign\0", &mut k);
        case!(1);
        if !ct_eq16(&k, &SK_302) { return 1; }
    }

    // ---- 2. The 3.0.2 key schedule, through the real entry point, and the
    // derived key must NOT be the session key (which is what a 2.x mount uses).
    case!(2);
    if smbsign_begin(&mut st, DIALECT_302, SESSION_KEY.as_ptr()) != R_OK { return 2; }
    case!(3);
    if st.algo != ALGO_AES_CMAC { return 3; }
    case!(4);
    if !ct_eq16(&st.key, &SK_302) { return 4; }
    case!(5);
    if ct_eq16(&st.key, &SESSION_KEY) { return 5; }

    // ---- 3. Sign the fixed PDU with AES-CMAC and match the oracle byte for
    // byte. This is the cross-implementation check on the whole 3.0.2 path:
    // KDF, then CMAC, then the wire framing.
    {
        let mut p = build_pdu();
        case!(6);
        if smbsign_sign(&mut st, p.as_mut_ptr(), 72) != R_OK { return 6; }
        case!(7);
        if !ct_eq16(&p[OFF_SIGNATURE..OFF_SIGNATURE + 16], &SIG_CMAC) { return 7; }

        // ---- 4. GREEN: that same message verifies.
        case!(8);
        if verify_one(&mut st, p.as_mut_ptr(), 72) != R_OK { return 8; }

        // ---- 5. RED: one flipped bit in the BODY must be refused. This is the
        // tampering case: a server's reply whose payload was altered in flight.
        p[70] ^= 0x01;
        case!(9);
        if verify_one(&mut st, p.as_mut_ptr(), 72) != R_BAD_SIGNATURE { return 9; }
        p[70] ^= 0x01;
        case!(10);
        if verify_one(&mut st, p.as_mut_ptr(), 72) != R_OK { return 10; }

        // ---- 6. RED: one flipped bit in the HEADER must be refused too. The
        // signature covers the header, so altering the status code (the field a
        // man-in-the-middle would change to turn a refusal into a success) is
        // caught.
        p[OFF_STATUS] ^= 0x01;
        case!(11);
        if verify_one(&mut st, p.as_mut_ptr(), 72) != R_BAD_SIGNATURE { return 11; }
        p[OFF_STATUS] ^= 0x01;

        // ---- 7. RED: stripping the signature is not a way past the check.
        // Clearing SMB2_FLAGS_SIGNED and zeroing the field is the cheapest
        // downgrade an attacker has, and it must land as R_UNSIGNED.
        let saved_flags = rd_u32(&p, OFF_FLAGS);
        wr_u32(&mut p, OFF_FLAGS, saved_flags & !SMB2_FLAGS_SIGNED);
        case!(12);
        if verify_one(&mut st, p.as_mut_ptr(), 72) != R_UNSIGNED { return 12; }
        wr_u32(&mut p, OFF_FLAGS, saved_flags);

        // ---- 8. RED: a truncated message is refused rather than read past.
        case!(13);
        if verify_one(&mut st, p.as_mut_ptr(), 32) != R_SHORT { return 13; }

        // ---- 9. The verify path must leave the caller's buffer exactly as it
        // arrived. It zeroes the Signature field to rebuild the signing form,
        // and a path that forgot to restore it would silently corrupt the
        // packet every later reader sees.
        case!(14);
        if !ct_eq16(&p[OFF_SIGNATURE..OFF_SIGNATURE + 16], &SIG_CMAC) { return 14; }
    }

    // ---- 10. The 2.x path: SigningKey IS the SessionKey, MAC is
    // HMAC-SHA256 truncated to 16, and the signature differs from the 3.0.2
    // one over the same bytes (so the two schedules cannot be confused).
    {
        let mut st2 = SMBSIGN_ZERO;
        case!(15);
        if smbsign_begin(&mut st2, DIALECT_210, SESSION_KEY.as_ptr()) != R_OK { return 15; }
        case!(16);
        if st2.algo != ALGO_HMAC_SHA256 { return 16; }
        case!(17);
        if !ct_eq16(&st2.key, &SESSION_KEY) { return 17; }

        let mut p = build_pdu();
        case!(18);
        if smbsign_sign(&mut st2, p.as_mut_ptr(), 72) != R_OK { return 18; }
        case!(19);
        if !ct_eq16(&p[OFF_SIGNATURE..OFF_SIGNATURE + 16], &SIG_HMAC) { return 19; }
        case!(20);
        if ct_eq16(&p[OFF_SIGNATURE..OFF_SIGNATURE + 16], &SIG_CMAC) { return 20; }
        case!(21);
        if verify_one(&mut st2, p.as_mut_ptr(), 72) != R_OK { return 21; }

        // RED: a 3.0.2 signature must not verify on a 2.1 connection, and vice
        // versa. Cross-dialect acceptance would mean the algorithm selection is
        // not actually being honoured.
        let mut i = 0;
        while i < 16 { p[OFF_SIGNATURE + i] = SIG_CMAC[i]; i += 1; }
        case!(22);
        if verify_one(&mut st2, p.as_mut_ptr(), 72) != R_BAD_SIGNATURE { return 22; }
    }

    // ---- 11. An all-zero session key is refused. An anonymous/guest NTLM
    // exchange produces one, and signing with it would match anyone else who
    // also has zeros.
    {
        let mut st3 = SMBSIGN_ZERO;
        let zeros = [0u8; 16];
        case!(23);
        if smbsign_begin(&mut st3, DIALECT_302, zeros.as_ptr()) != R_NO_SESSION_KEY {
            return 23;
        }
        case!(24);
        if smbsign_is_armed(&st3) != 0 { return 24; }
    }

    // ---- 12. SMB 3.1.1 through the WRONG ENTRY POINT is refused BY NAME,
    // not guessed at. A 3.1.1 signing key is a function of the pre-auth
    // integrity hash, which smbsign_begin() has no argument for, so a guess
    // here would arm the connection with a plausible-looking key that signs
    // nothing the server will accept. smbsign_begin311() is the right door.
    //
    // THE EXPECTATION HERE WAS STALE, and it is why this file now counts its
    // cases. When rustkern/smbpreauth.rs implemented the 3.1.1 key schedule,
    // smbsign_begin() stopped answering R_DIALECT for 0x0311 and started
    // answering the more specific R_DIALECT_311_WRONG_ENTRY. This one
    // comparison was not updated with it, so case 25 failed on every boot and
    // the early return took cases 26 to 80 with it.
    //
    // Write DIALECT_311, never the bare literal 0x0311: a named constant is
    // the thing a reader can grep when its meaning moves, and a literal is
    // exactly how this comparison came to be testing a value nobody returns.
    {
        let mut st4 = SMBSIGN_ZERO;
        case!(25);
        if smbsign_begin(&mut st4, DIALECT_311, SESSION_KEY.as_ptr())
            != R_DIALECT_311_WRONG_ENTRY { return 25; }
        case!(26);
        if smbsign_is_armed(&st4) != 0 { return 26; }
    }

    // ---- 13. An unarmed connection signs nothing and verifies nothing. This
    // is the NEGOTIATE state and it must not be mistakable for success.
    {
        let mut st5 = SMBSIGN_ZERO;
        let mut p = build_pdu();
        case!(27);
        if smbsign_sign(&mut st5, p.as_mut_ptr(), 72) != R_NOT_ARMED { return 27; }
        case!(28);
        if verify_one(&mut st5, p.as_mut_ptr(), 72) != R_NOT_ARMED { return 28; }
    }

    // ---- 14. An armed connection still refuses to sign the two pre-session
    // commands, so a NEGOTIATE on a revived connection cannot pick up a stale
    // key.
    {
        let mut p = build_pdu();
        p[OFF_COMMAND] = 0x00;     // NEGOTIATE
        case!(29);
        if smbsign_sign(&mut st, p.as_mut_ptr(), 72) != R_NOT_ARMED { return 29; }
        p[OFF_COMMAND] = 0x01;     // SESSION_SETUP
        case!(30);
        if smbsign_sign(&mut st, p.as_mut_ptr(), 72) != R_NOT_ARMED { return 30; }
    }

    // ---- 15. The downgrade gate at NEGOTIATE.
    {
        smbsign_policy_set(POLICY_REQUIRED);
        let mut st6 = SMBSIGN_ZERO;
        smbsign_reset(&mut st6);
        // A server that offers nothing is refused under REQUIRED.
        case!(31);
        if smbsign_note_negotiate(&mut st6, DIALECT_302, 0x0000) != R_SERVER_REFUSES {
            return 31;
        }
        // A server that offers signing is accepted.
        case!(32);
        if smbsign_note_negotiate(&mut st6, DIALECT_302, SIGNING_ENABLED) != R_OK {
            return 32;
        }
        // And we announce REQUIRED, which is what makes a merely-capable
        // server actually sign.
        case!(33);
        if smbsign_client_security_mode() != (SIGNING_ENABLED | SIGNING_REQUIRED) {
            return 33;
        }
        // Under ENABLED the same no-signing server is allowed through.
        smbsign_policy_set(POLICY_ENABLED);
        let mut st7 = SMBSIGN_ZERO;
        smbsign_reset(&mut st7);
        case!(34);
        if smbsign_note_negotiate(&mut st7, DIALECT_302, 0x0000) != R_OK { return 34; }
        case!(35);
        if smbsign_client_security_mode() != SIGNING_ENABLED { return 35; }
        // ... but a server that REQUIRES signing still forces us to sign, even
        // under the weaker local policy.
        let mut st8 = SMBSIGN_ZERO;
        smbsign_reset(&mut st8);
        case!(36);
        if smbsign_note_negotiate(&mut st8, DIALECT_302,
                                  SIGNING_ENABLED | SIGNING_REQUIRED) != R_OK {
            return 36;
        }
        case!(37);
        if smbsign_begin(&mut st8, DIALECT_302, SESSION_KEY.as_ptr()) != R_OK { return 37; }
        case!(38);
        if smbsign_requires(&st8) == 0 { return 38; }
        // Restore the shipping default; a self-test must not leave policy changed.
        smbsign_policy_set(POLICY_REQUIRED);
    }

    // ---- 16. The interim-response exemption is narrow: STATUS_PENDING AND
    // the async flag, both. Neither alone opens it.
    {
        let mut p = build_pdu();
        case!(39);
        if unsigned_is_permitted(p.as_ptr(), 72) { return 39; }
        wr_u32(&mut p, OFF_STATUS, STATUS_PENDING);
        case!(40);
        if unsigned_is_permitted(p.as_ptr(), 72) { return 40; }
        wr_u32(&mut p, OFF_FLAGS, SMB2_FLAGS_SIGNED | SMB2_FLAGS_ASYNC_COMMAND);
        case!(41);
        if !unsigned_is_permitted(p.as_ptr(), 72) { return 41; }
        // A real answer (STATUS_SUCCESS) with the async flag is NOT exempt.
        wr_u32(&mut p, OFF_STATUS, 0);
        case!(42);
        if unsigned_is_permitted(p.as_ptr(), 72) { return 42; }
    }

    // ---- 17. The config parser, including the cases that must NOT silently
    // resolve to a policy.
    {
        let a = b"signing=required\n";
        case!(43);
        if smbsign_policy_parse(a.as_ptr(), a.len() as i32) != 1 { return 43; }
        let b = b"signing=enabled\n";
        case!(44);
        if smbsign_policy_parse(b.as_ptr(), b.len() as i32) != 0 { return 44; }
        let c = b"# signing=enabled\nsigning=required\n";
        case!(45);
        if smbsign_policy_parse(c.as_ptr(), c.len() as i32) != 1 { return 45; }
        let d = b"user=bob\nsigning = enabled   # trailing\n";
        case!(46);
        if smbsign_policy_parse(d.as_ptr(), d.len() as i32) != 0 { return 46; }
        let e = b"SIGNING=REQUIRED\n";
        case!(47);
        if smbsign_policy_parse(e.as_ptr(), e.len() as i32) != 1 { return 47; }
        let f = b"user=bob\n";
        case!(48);
        if smbsign_policy_parse(f.as_ptr(), f.len() as i32) != -1 { return 48; }
        // A plausible typo must be REPORTED, not resolved.
        let g = b"signing=yes\n";
        case!(49);
        if smbsign_policy_parse(g.as_ptr(), g.len() as i32) != -2 { return 49; }
        let h = b"signing=off\n";
        case!(50);
        if smbsign_policy_parse(h.as_ptr(), h.len() as i32) != -2 { return 50; }
        // An empty / absent file leaves the default alone.
        case!(51);
        if smbsign_policy_parse(core::ptr::null(), 0) != -1 { return 51; }
    }

    // ---- 18. THE CHOKEPOINT ITSELF, which is the function net/smb.c calls.
    // Cases 1 to 17 exercise the pieces; this exercises the thing that is
    // actually wired in. A module whose internals are tested and whose entry
    // point is not is the shape of several past defects in this tree.
    {
        let mut st9 = SMBSIGN_ZERO;
        let mut p = build_pdu();

        // Unarmed: nothing to check, so the response is let through. This is
        // the NEGOTIATE / SESSION_SETUP state.
        case!(52);
        if smbsign_check_response(&mut st9, p.as_mut_ptr(), 72) != R_OK { return 52; }

        case!(53);
        if smbsign_begin(&mut st9, DIALECT_302, SESSION_KEY.as_ptr()) != R_OK { return 53; }
        case!(54);
        if smbsign_sign(&mut st9, p.as_mut_ptr(), 72) != R_OK { return 54; }

        // GREEN through the chokepoint, and the counter moves.
        case!(55);
        if smbsign_check_response(&mut st9, p.as_mut_ptr(), 72) != R_OK { return 55; }
        case!(56);
        if st9.verified_in != 1 { return 56; }

        // RED through the chokepoint: a tampered body is refused and counted.
        p[68] ^= 0xFF;
        case!(57);
        if smbsign_check_response(&mut st9, p.as_mut_ptr(), 72) != R_BAD_SIGNATURE {
            return 57;
        }
        case!(58);
        if st9.rejected_in != 1 { return 58; }
        p[68] ^= 0xFF;

        // RED through the chokepoint: an unsigned NON-interim response is
        // refused. This is the case a sign-only implementation waves through.
        let f = rd_u32(&p, OFF_FLAGS);
        wr_u32(&mut p, OFF_FLAGS, f & !SMB2_FLAGS_SIGNED);
        case!(59);
        if smbsign_check_response(&mut st9, p.as_mut_ptr(), 72) != R_UNSIGNED {
            return 59;
        }
        case!(60);
        if st9.rejected_in != 2 { return 60; }

        // GREEN through the chokepoint: the same unsigned message, but now an
        // async interim, is permitted by the narrow exemption and is NOT
        // counted as a rejection.
        wr_u32(&mut p, OFF_STATUS, STATUS_PENDING);
        wr_u32(&mut p, OFF_FLAGS, SMB2_FLAGS_ASYNC_COMMAND);
        case!(61);
        if smbsign_check_response(&mut st9, p.as_mut_ptr(), 72) != R_OK { return 61; }
        case!(62);
        if st9.rejected_in != 2 { return 62; }
        case!(63);
        if st9.verified_in != 1 { return 63; }
    }

    // ---- 19. THE TWO POLICIES MUST ACTUALLY DIFFER on a missing signature.
    // Measured 2026-10-04: before this case existed they did not. `enabled`
    // and `required` both refused an unsigned response, so the documented knob
    // had two settings with one behaviour, which is worse than no knob because
    // it reads as a choice.
    {
        const SK: [u8; 16] = SESSION_KEY;
        // (a) REQUIRED refuses an unsigned real answer.
        smbsign_policy_set(POLICY_REQUIRED);
        let mut r1 = SMBSIGN_ZERO;
        smbsign_reset(&mut r1);
        case!(64);
        if smbsign_note_negotiate(&mut r1, DIALECT_302, SIGNING_ENABLED) != R_OK { return 64; }
        case!(65);
        if smbsign_begin(&mut r1, DIALECT_302, SK.as_ptr()) != R_OK { return 65; }
        let mut p = build_pdu();
        wr_u32(&mut p, OFF_FLAGS, 0);          // no signature at all
        case!(66);
        if smbsign_check_response(&mut r1, p.as_mut_ptr(), 72) != R_UNSIGNED { return 66; }

        // (b) ENABLED, against a server that did not REQUIRE signing, lets the
        // same unsigned answer through, and counts it as unsigned rather than
        // as verified.
        smbsign_policy_set(POLICY_ENABLED);
        let mut r2 = SMBSIGN_ZERO;
        smbsign_reset(&mut r2);
        case!(67);
        if smbsign_note_negotiate(&mut r2, DIALECT_302, SIGNING_ENABLED) != R_OK { return 67; }
        case!(68);
        if smbsign_begin(&mut r2, DIALECT_302, SK.as_ptr()) != R_OK { return 68; }
        case!(69);
        if smbsign_check_response(&mut r2, p.as_mut_ptr(), 72) != R_OK { return 69; }
        case!(70);
        if r2.unsigned_in != 1 { return 70; }
        case!(71);
        if r2.verified_in != 0 { return 71; }

        // (c) ENABLED still refuses a signature that is PRESENT and WRONG.
        // That is the part the weaker policy keeps.
        wr_u32(&mut p, OFF_FLAGS, SMB2_FLAGS_SIGNED);
        let mut i = 0;
        while i < SIG_LEN { p[OFF_SIGNATURE + i] = 0xEE; i += 1; }
        case!(72);
        if smbsign_check_response(&mut r2, p.as_mut_ptr(), 72) != R_BAD_SIGNATURE { return 72; }

        // (d) ENABLED does NOT relax anything when the SERVER required signing.
        // The server gets to insist even where we would not have.
        let mut r3 = SMBSIGN_ZERO;
        smbsign_reset(&mut r3);
        case!(73);
        if smbsign_note_negotiate(&mut r3, DIALECT_302,
                                  SIGNING_ENABLED | SIGNING_REQUIRED) != R_OK { return 73; }
        case!(74);
        if smbsign_begin(&mut r3, DIALECT_302, SK.as_ptr()) != R_OK { return 74; }
        wr_u32(&mut p, OFF_FLAGS, 0);
        case!(75);
        if smbsign_check_response(&mut r3, p.as_mut_ptr(), 72) != R_UNSIGNED { return 75; }

        smbsign_policy_set(POLICY_REQUIRED);   // restore the shipping default
    }

    // ---- 20. Null and short arguments are refused rather than dereferenced.
    {
        case!(76);
        if smbsign_sign(core::ptr::null_mut(), core::ptr::null_mut(), 72) != R_NULL { return 76; }
        case!(77);
        if smbsign_check_response(core::ptr::null_mut(), core::ptr::null_mut(), 72) != R_NULL { return 77; }
        case!(78);
        if smbsign_begin(core::ptr::null_mut(), DIALECT_302, core::ptr::null()) != R_NULL { return 78; }
        case!(79);
        if smbsign_is_armed(core::ptr::null()) != 0 { return 79; }
        case!(80);
        if smbsign_requires(core::ptr::null()) != 0 { return 80; }
    }

    0
}
