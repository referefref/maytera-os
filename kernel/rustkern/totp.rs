// rustkern/totp.rs - #secretuse: RFC 6238 TOTP, RFC 4648 base32, and the RECORD
// TABLE inside an encrypted secret store.
//
// WHAT THIS IS FOR, and it is the whole point of the change.
//
// Before this, userland/apps/mfa held every enrolled TOTP shared secret in
// Ring-3 memory and on disk under a single fixed XOR byte, and computed codes
// with its own private copy of SHA-1, HMAC, base32 and the truncation. Any
// Ring-3 read of that file, or of that process, WAS the second factor.
//
// After this, the secret lives only inside a secstore.rs container that only
// Ring 0 can open, and the app asks the kernel for A CODE. This module is the
// part of that path that has to be exactly right: it parses the decrypted store
// bytes into fixed-size records, decodes attacker-influenceable base32, and does
// the RFC 4226 dynamic truncation. Pure arithmetic and bounds over untrusted
// input, no kernel services, so it is Rust per the standing directive, and it is
// NEW code: there is no C twin to strangle. The C in proc/secretuse.c is the
// uaccess/VFS/lock glue and holds no algorithm.
//
// ===========================================================================
// THE ONE DESIGN RULE THAT MATTERS HERE: THE COUNTER IS NEVER A CALLER'S CHOICE.
// ===========================================================================
// totp_code_rs() takes a UNIX SECOND, and proc/secretuse.c passes what
// realtime_sec_rs() says it is. Ring 3 cannot supply it, and there is no
// "compute the code for counter N" entry point anywhere in this file.
//
// This is not a convenience. docs/OPEN_BACKLOG.md A4 records the measured
// reasoning for refusing secret_use(SMB, ntlmv2_response, challenge): a
// primitive that computes a valid authenticator over an ATTACKER-CHOSEN input
// turns a closed credential into an oracle, and the caller can then authenticate
// to anything as the user. A TOTP responder with a caller-supplied counter is
// the SAME construction wearing different clothes: an attacker who can ask for
// counter T+1, T+2, ... harvests a month of future codes in one call and needs
// the secret never again. A KERNEL-CLOCK counter is what makes this a proxy
// rather than an oracle, because the only thing it will ever compute is the code
// the human is currently looking at on the screen.
//
// ===========================================================================
// WHAT THIS DOES NOT PROTECT, stated so nobody reads more into it.
// ===========================================================================
//   * The secret enters through Ring 3 ONCE, at enrolment, because the user is
//     typing or pasting it. There is no way around that and no claim otherwise.
//     The property is that it never comes back out.
//   * A code is a bearer token for 30 seconds. Any process that can call the
//     syscall as the same uid can read the current code, exactly as it could
//     read it off the screen. The right further control is a capability class
//     for secret.use; it is NOT built here and is recorded in the backlog rather
//     than claimed.
//   * SHA-1 is collision-broken. HMAC-SHA1 is not affected (HMAC rests on the
//     keyed compression function as a PRF), and RFC 6238 plus every authenticator
//     in the field speaks it, which is the only reason it is here. See
//     rustkern/sha1.rs's header.

#![allow(dead_code)]

extern "C" {
    // rustkern/hmac.rs, the one shared RFC 2104 construction. Declared through
    // the C ABI rather than as `crate::hmac::...` so this file also compiles as
    // a standalone crate root for crypto/secretuse_kat.sh, which is how the KAT
    // gets to test the shipping bytes without a sed.
    fn hmac_sha1_rs(key: *const u8, key_len: usize, data: *const u8, data_len: usize, mac: *mut u8);
    fn hmac_sha256_rs(key: *const u8, key_len: usize, data: *const u8, data_len: usize, mac: *mut u8);
}

// ---------------------------------------------------------------------------
// Geometry. Every one of these bounds a loop or a buffer below; none is decoration.
// ---------------------------------------------------------------------------
/// Accounts in one store. Matches what the app has always allowed.
pub const TOTP_MAX_RECS: usize = 32;
/// Decoded shared-secret bytes. RFC 4226 wants >= 16; 64 covers every issuer
/// seen in the field and is the app's historic cap.
pub const TOTP_SECRET_MAX: usize = 64;
/// Issuer / label text. NOT truncated on overflow: a truncated label is a
/// DIFFERENT account, and silently showing the user the wrong account name next
/// to a code is how someone types a code into the wrong site.
pub const TOTP_NAME_MAX: usize = 48;
/// Base32 text accepted at enrolment.
pub const TOTP_B32_MAX: usize = 128;
/// Digits buffer: 8 digits + NUL, rounded up.
pub const TOTP_CODE_MAX: usize = 12;

pub const TOTP_ALG_SHA1: u16 = 1;
pub const TOTP_ALG_SHA256: u16 = 256;

// Negative returns. Distinguished for the same reason secstore.rs distinguishes
// its own: the app this replaces treated every failure as "no accounts", so a
// failed decrypt was indistinguishable from an empty store, which is how a user
// loses a second factor without being told.
pub const TOTP_E_ARG: i32 = -1;
/// Store header missing or not MFADB1.
pub const TOTP_E_MAGIC: i32 = -2;
/// A row is malformed, over-long, or carries a value outside policy. The WHOLE
/// store is refused, never partially loaded: see parse_store().
pub const TOTP_E_FORMAT: i32 = -3;
/// More records than the table holds, or the formatted store exceeds the buffer.
/// REFUSED, never truncated.
pub const TOTP_E_NOSPACE: i32 = -4;
/// Base32 that is not base32.
pub const TOTP_E_B32: i32 = -5;
/// The kernel clock has never presented a plausible time. A TOTP code computed
/// against a counter of zero is not a code, it is a wrong answer that looks like
/// one, so this is a refusal rather than a guess.
pub const TOTP_E_NOCLOCK: i32 = -6;

// ---------------------------------------------------------------------------
// THE SECRET TIER TABLE (docs/SECRET_STORE.md, backlog A1).
//
// The store needs MORE THAN ONE KEY SOURCE, because two of the secrets the owner
// wants moved into it cannot be opened by a login-derived key:
//   * SSHHOST.KEY is a HOST key, read by load_host_key() at boot before any user
//     exists. A per-user copy would be wrong in principle and would break sshd
//     on a machine nobody has logged into.
//   * AISVC.CFG is read by proc/cron.c, which is headless by definition.
//
// So each secret CLASS declares its key source, and a class that has not declared
// one is REFUSED. That is the fail-closed shape: adding a new secret to the store
// without deciding how it is keyed does not quietly inherit the login key, it
// does not work at all until somebody writes the decision down here.
//
// BE HONEST ABOUT THE HEADLESS TIER. It is declared and NOT implemented, and
// when it is implemented it will be BLAST-RADIUS MANAGEMENT, NOT CONFIDENTIALITY.
// If the machine can boot unattended and use the secret, the key material is on
// the disk, so anyone holding the disk can use it too. The iMac14,4 target has
// no TPM, so there is no hardware root to escape that with. A headless-tier
// secret is protected from a Ring-3 process reading the file; it is NOT protected
// from someone who takes the machine. Do not describe it as encrypted-at-rest
// and leave it there.
// ---------------------------------------------------------------------------
/// Not a declared class. Always maps to KEYSRC_NONE.
pub const SECRET_CLASS_NONE: u32 = 0;
/// TOTP shared secrets (the /APPS/MFA store). The one implemented class.
pub const SECRET_CLASS_TOTP: u32 = 1;

pub const SECRET_KEYSRC_NONE: u32 = 0;
/// PBKDF2-HMAC-SHA256 over the user's login password, derived at
/// users_authenticate() and held only in kernel memory. IMPLEMENTED.
pub const SECRET_KEYSRC_LOGIN: u32 = 1;
/// A machine-local key usable with no human present. DECLARED, NOT IMPLEMENTED.
pub const SECRET_KEYSRC_HEADLESS: u32 = 2;
/// A key sealed to a TPM. DECLARED, NOT IMPLEMENTED, and NOT AVAILABLE on the
/// iMac14,4 target at all.
pub const SECRET_KEYSRC_TPM: u32 = 3;

/// Which key source opens a store of this class. An undeclared class gets
/// SECRET_KEYSRC_NONE, and proc/secretuse.c refuses on NONE.
#[no_mangle]
pub extern "C" fn secret_keysrc_for_class_rs(class: u32) -> u32 {
    match class {
        SECRET_CLASS_TOTP => SECRET_KEYSRC_LOGIN,
        _ => SECRET_KEYSRC_NONE,
    }
}

/// Is this key source actually built? 1 yes, 0 no. Separate from the mapping on
/// purpose: a class may be declared against a tier that does not exist yet, and
/// the honest answer is then "refused because unimplemented", not "refused
/// because undeclared".
#[no_mangle]
pub extern "C" fn secret_keysrc_is_implemented_rs(src: u32) -> i32 {
    if src == SECRET_KEYSRC_LOGIN { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
// The record. #[repr(C)] because proc/secretuse.c owns the array; the field
// order below has no padding at all:
//   issuer 48 @0, label 48 @48, secret 64 @96, secret_len 2 @160,
//   digits 2 @162, period 2 @164, alg 2 @166  => 168 bytes, align 2.
// crypto/secretuse.h locks that with a _Static_assert.
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TotpRec {
    pub issuer: [u8; TOTP_NAME_MAX],
    pub label: [u8; TOTP_NAME_MAX],
    pub secret: [u8; TOTP_SECRET_MAX],
    pub secret_len: u16,
    pub digits: u16,
    pub period: u16,
    pub alg: u16,
}

impl TotpRec {
    const fn zeroed() -> TotpRec {
        TotpRec {
            issuer: [0u8; TOTP_NAME_MAX],
            label: [0u8; TOTP_NAME_MAX],
            secret: [0u8; TOTP_SECRET_MAX],
            secret_len: 0,
            digits: 0,
            period: 0,
            alg: 0,
        }
    }
}

/// Volatile zero so the compiler may not elide wiping a secret.
fn wipe(b: &mut [u8]) {
    for p in b.iter_mut() {
        unsafe { core::ptr::write_volatile(p, 0u8) };
    }
}

/// Wipe one record in place. Called on delete and on table teardown, so a freed
/// slot never leaves a shared secret in kernel memory for the next enrolment to
/// sit on top of.
#[no_mangle]
pub unsafe extern "C" fn totp_wipe_rec_rs(rec: *mut TotpRec) {
    if rec.is_null() {
        return;
    }
    let r = &mut *rec;
    wipe(&mut r.secret);
    wipe(&mut r.issuer);
    wipe(&mut r.label);
    r.secret_len = 0;
    r.digits = 0;
    r.period = 0;
    r.alg = 0;
}

// ---------------------------------------------------------------------------
// RFC 4648 base32 decode.
// ---------------------------------------------------------------------------
// Case-insensitive. '=' padding and ASCII space/tab/dash are IGNORED, because
// every issuer in the field prints the secret in spaced groups and users paste
// it that way. Anything else is a REFUSAL, not a skipped character: silently
// dropping an unexpected byte turns a typo into a different, wrong secret that
// generates wrong codes forever with no error anywhere.
//
// THE PARTIAL-GROUP RULE. base32 packs 5 bits at a time; a final group of fewer
// than 5 leftover bits cannot contribute a byte and MUST be zero. A non-zero
// remainder means the input was truncated mid-character, so it is refused. The
// app's old decoder accepted it.
fn b32_val(c: u8) -> i32 {
    match c {
        b'A'..=b'Z' => (c - b'A') as i32,
        b'a'..=b'z' => (c - b'a') as i32,
        b'2'..=b'7' => (c - b'2') as i32 + 26,
        _ => -1,
    }
}

fn base32_decode(input: &[u8], out: &mut [u8]) -> Result<usize, i32> {
    let mut acc: u32 = 0;
    let mut bits: u32 = 0;
    let mut n: usize = 0;
    for &c in input {
        if c == b'=' || c == b' ' || c == b'\t' || c == b'-' {
            continue;
        }
        let v = b32_val(c);
        if v < 0 {
            return Err(TOTP_E_B32);
        }
        acc = (acc << 5) | (v as u32);
        bits += 5;
        if bits >= 8 {
            bits -= 8;
            if n >= out.len() {
                return Err(TOTP_E_NOSPACE);
            }
            out[n] = ((acc >> bits) & 0xFF) as u8;
            n += 1;
        }
    }
    // Leftover bits must be zero padding, never data.
    if bits > 0 && (acc & ((1u32 << bits) - 1)) != 0 {
        return Err(TOTP_E_B32);
    }
    if n == 0 {
        return Err(TOTP_E_B32);
    }
    Ok(n)
}

/// C entry for the decoder, so the KAT and the enrolment path share one
/// implementation rather than each having a copy of the alphabet.
/// # Safety: `input` has `in_len` readable bytes, `out` `out_cap` writable.
#[no_mangle]
pub unsafe extern "C" fn totp_base32_decode_rs(
    input: *const u8,
    in_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    if input.is_null() || out.is_null() || in_len > TOTP_B32_MAX {
        return TOTP_E_ARG;
    }
    let i = core::slice::from_raw_parts(input, in_len);
    let o = core::slice::from_raw_parts_mut(out, out_cap);
    match base32_decode(i, o) {
        Ok(n) => n as i32,
        Err(e) => {
            wipe(o);
            e
        }
    }
}

// ---------------------------------------------------------------------------
// RFC 4226 / 6238 code generation.
// ---------------------------------------------------------------------------
// THE RED-TEAM HOOK. The kernel build NEVER sets `totp_kat_fault` (it appears in
// no Makefile; grep the tree). crypto/secretuse_kat.sh compiles a second copy
// WITH it, pinning the dynamic-truncation offset to 0.
//
// That fault is chosen because it is the one a careless implementation actually
// ships. With a fixed offset the code is still the right number of digits, still
// changes every period, still matches itself on both ends of a round trip, and
// is WRONG against every real issuer. Only a known-answer vector notices. It is
// the concrete reason "it produced six digits" is not evidence of a TOTP
// implementation.
#[cfg(totp_kat_fault)]
fn dt_offset(_digest: &[u8]) -> usize {
    0
}
#[cfg(not(totp_kat_fault))]
fn dt_offset(digest: &[u8]) -> usize {
    (digest[digest.len() - 1] & 0x0f) as usize
}

fn pow10(digits: u16) -> u32 {
    let mut p: u32 = 1;
    let mut i = 0u16;
    while i < digits {
        p *= 10;
        i += 1;
    }
    p
}

/// The truncated TOTP value for an explicit counter. PRIVATE, and it stays
/// private: see the file header on why there is no caller-supplied-counter entry
/// point. The only public code entry takes a unix second that the KERNEL read.
fn totp_value(rec: &TotpRec, counter: u64) -> Result<u32, i32> {
    if rec.secret_len == 0 || rec.secret_len as usize > TOTP_SECRET_MAX {
        return Err(TOTP_E_FORMAT);
    }
    if rec.digits != 6 && rec.digits != 8 {
        return Err(TOTP_E_FORMAT);
    }
    let msg = counter.to_be_bytes(); // RFC 4226: 8-byte big-endian counter
    let klen = rec.secret_len as usize;
    let mut dg = [0u8; 32];
    let dlen: usize = match rec.alg {
        TOTP_ALG_SHA1 => {
            // SAFETY: key/data are in-bounds slices of this record and the
            // stack; hmac_sha1_rs writes exactly 20 bytes.
            unsafe {
                hmac_sha1_rs(rec.secret.as_ptr(), klen, msg.as_ptr(), 8, dg.as_mut_ptr());
            }
            20
        }
        TOTP_ALG_SHA256 => {
            // SAFETY: as above; hmac_sha256_rs writes exactly 32 bytes.
            unsafe {
                hmac_sha256_rs(rec.secret.as_ptr(), klen, msg.as_ptr(), 8, dg.as_mut_ptr());
            }
            32
        }
        _ => return Err(TOTP_E_FORMAT),
    };
    let d = &dg[..dlen];
    let off = dt_offset(d);
    // RFC 4226 guarantees off <= 15 and the digest is at least 20 bytes, so
    // off+3 is in bounds. Checked anyway: an assumption that holds is free to
    // state, and this one stops a future digest length from becoming an overrun.
    if off + 3 >= dlen {
        return Err(TOTP_E_FORMAT);
    }
    let bin = (((d[off] & 0x7f) as u32) << 24)
        | ((d[off + 1] as u32) << 16)
        | ((d[off + 2] as u32) << 8)
        | (d[off + 3] as u32);
    let v = bin % pow10(rec.digits);
    wipe(&mut dg);
    Ok(v)
}

/// Write `v` zero-padded to `digits` characters plus a NUL.
fn write_digits(v: u32, digits: u16, out: &mut [u8]) -> Result<(), i32> {
    let n = digits as usize;
    if out.len() < n + 1 {
        return Err(TOTP_E_NOSPACE);
    }
    let mut x = v;
    let mut i = n;
    while i > 0 {
        i -= 1;
        out[i] = b'0' + (x % 10) as u8;
        x /= 10;
    }
    out[n] = 0;
    Ok(())
}

/// The ONE public code entry.
///
/// `unix_sec` is what realtime_sec_rs() returned; 0 means the kernel clock has
/// never been plausible and is REFUSED (TOTP_E_NOCLOCK) rather than treated as
/// 1970. Writes a NUL-terminated decimal code to `out`.
///
/// Returns the SECONDS REMAINING in the current window (1..=period), so the app
/// can draw its countdown without a clock of its own and, more to the point,
/// without any reason to ever ask for a different window.
///
/// # Safety: `rec` is a readable TotpRec, `out` has `out_cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn totp_code_rs(
    rec: *const TotpRec,
    unix_sec: i64,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    if rec.is_null() || out.is_null() {
        return TOTP_E_ARG;
    }
    let o = core::slice::from_raw_parts_mut(out, out_cap);
    if unix_sec <= 0 {
        wipe(o);
        return TOTP_E_NOCLOCK;
    }
    let r = &*rec;
    if r.period == 0 || r.period > 3600 {
        wipe(o);
        return TOTP_E_FORMAT;
    }
    let period = r.period as u64;
    let now = unix_sec as u64;
    let counter = now / period;
    let v = match totp_value(r, counter) {
        Ok(v) => v,
        Err(e) => {
            wipe(o);
            return e;
        }
    };
    if let Err(e) = write_digits(v, r.digits, o) {
        wipe(o);
        return e;
    }
    // Seconds left in this window: 1..=period, never 0, so "0 seconds left" can
    // never be confused with a failure return.
    (period - (now % period)) as i32
}

// ---------------------------------------------------------------------------
// The store plaintext.
//
// FORMAT, UNCHANGED FROM THE APP'S DE-OBFUSCATED BYTES:
//     MFADB1\n
//     <issuer>\t<label>\t<base32secret>\t<digits>\t<period>\t<alg>\n   (repeated)
//
// Keeping it byte-identical is deliberate: MIGRATION from the legacy XOR file is
// then literally "XOR every byte with 0x5A and seal the result", with no
// reformatting step that could lose a field. A migration that has to transform
// the data is a migration that can transform it wrongly.
//
// PARSE FAILURE REFUSES THE WHOLE STORE. Not "skip the bad row", which is what
// the app did. The store is AEAD-authenticated, so bytes that reach this parser
// have already proven they were written by this kernel under this user's key: a
// malformed row is therefore OUR BUG, not tampering, and the loud failure is the
// only way anyone finds out. Skipping the row would silently drop one of the
// user's second factors.
// ---------------------------------------------------------------------------
const STORE_MAGIC: &[u8; 7] = b"MFADB1\n";

/// What ended a field. THREE outcomes, not two, and the distinction is the whole
/// reason this is an enum and not a bool.
///
/// The first draft of this returned `hit_eol: bool` that was true BOTH for "this
/// field ended at a newline" AND for "this field ran off the end of the buffer".
/// Those are opposite facts: the first is a complete row, the second is a
/// TRUNCATED store. A last row with its newline chopped off therefore parsed as
/// valid, and the self-test case that asserts otherwise (case 38) is what caught
/// it. A two-valued return for a three-valued question is how a parser accepts
/// something it was written to reject.
#[derive(PartialEq, Eq, Clone, Copy)]
enum Term {
    /// Ended at the field separator: more fields follow on this row.
    Sep,
    /// Ended at '\n': this was the last field of the row.
    Eol,
    /// Ran out of buffer with no terminator: the store is truncated.
    End,
}

fn next_field(buf: &[u8], sep: u8) -> (&[u8], &[u8], Term) {
    let mut i = 0usize;
    while i < buf.len() && buf[i] != sep && buf[i] != b'\n' {
        i += 1;
    }
    let f = &buf[..i];
    if i >= buf.len() {
        return (f, &buf[i..], Term::End);
    }
    let t = if buf[i] == b'\n' { Term::Eol } else { Term::Sep };
    (f, &buf[i + 1..], t)
}

fn parse_u32(f: &[u8]) -> Result<u32, i32> {
    if f.is_empty() || f.len() > 10 {
        return Err(TOTP_E_FORMAT);
    }
    let mut v: u32 = 0;
    for &c in f {
        if !c.is_ascii_digit() {
            return Err(TOTP_E_FORMAT);
        }
        v = match v.checked_mul(10).and_then(|x| x.checked_add((c - b'0') as u32)) {
            Some(x) => x,
            None => return Err(TOTP_E_FORMAT),
        };
    }
    Ok(v)
}

/// Copy a name field. Refuses rather than truncating, and refuses a control
/// character or a tab-like byte that would re-split the row on the way back out.
fn copy_name(src: &[u8], dst: &mut [u8; TOTP_NAME_MAX]) -> Result<(), i32> {
    if src.len() >= TOTP_NAME_MAX {
        return Err(TOTP_E_FORMAT);
    }
    for &c in src {
        if c < 0x20 || c == 0x7F {
            return Err(TOTP_E_FORMAT);
        }
    }
    for (i, &c) in src.iter().enumerate() {
        dst[i] = c;
    }
    dst[src.len()] = 0;
    Ok(())
}

fn parse_store(buf: &[u8], recs: &mut [TotpRec]) -> Result<usize, i32> {
    if buf.len() < STORE_MAGIC.len() || &buf[..STORE_MAGIC.len()] != &STORE_MAGIC[..] {
        return Err(TOTP_E_MAGIC);
    }
    let mut p = &buf[STORE_MAGIC.len()..];
    let mut n = 0usize;
    while !p.is_empty() {
        // A trailing newline, or trailing NUL padding, ends the store cleanly.
        if p[0] == b'\n' || p[0] == 0 {
            break;
        }
        if n >= recs.len() {
            return Err(TOTP_E_NOSPACE);
        }
        // Fields 1..5 MUST end at a separator. A newline there means the row has
        // too few fields; end-of-buffer means the store is truncated. Both are
        // refusals, and neither is a partially usable record.
        let (f_issuer, r1, t1) = next_field(p, b'\t');
        if t1 != Term::Sep {
            return Err(TOTP_E_FORMAT);
        }
        let (f_label, r2, t2) = next_field(r1, b'\t');
        if t2 != Term::Sep {
            return Err(TOTP_E_FORMAT);
        }
        let (f_b32, r3, t3) = next_field(r2, b'\t');
        if t3 != Term::Sep {
            return Err(TOTP_E_FORMAT);
        }
        let (f_dig, r4, t4) = next_field(r3, b'\t');
        if t4 != Term::Sep {
            return Err(TOTP_E_FORMAT);
        }
        let (f_per, r5, t5) = next_field(r4, b'\t');
        if t5 != Term::Sep {
            return Err(TOTP_E_FORMAT);
        }
        // Field 6 MUST end at the newline. Term::Sep is a seventh field (a row
        // that means something we do not understand); Term::End is a truncated
        // store whose last row lost its newline.
        let (f_alg, r6, t6) = next_field(r5, b'\t');
        if t6 != Term::Eol {
            return Err(TOTP_E_FORMAT);
        }

        let mut rec = TotpRec::zeroed();
        copy_name(f_issuer, &mut rec.issuer)?;
        copy_name(f_label, &mut rec.label)?;
        if f_b32.len() > TOTP_B32_MAX {
            return Err(TOTP_E_FORMAT);
        }
        let sl = base32_decode(f_b32, &mut rec.secret)?;
        rec.secret_len = sl as u16;

        let dig = parse_u32(f_dig)?;
        if dig != 6 && dig != 8 {
            return Err(TOTP_E_FORMAT);
        }
        rec.digits = dig as u16;

        let per = parse_u32(f_per)?;
        if per == 0 || per > 3600 {
            return Err(TOTP_E_FORMAT);
        }
        rec.period = per as u16;

        let alg = parse_u32(f_alg)?;
        if alg != TOTP_ALG_SHA1 as u32 && alg != TOTP_ALG_SHA256 as u32 {
            return Err(TOTP_E_FORMAT);
        }
        rec.alg = alg as u16;

        recs[n] = rec;
        n += 1;
        p = r6;
    }
    Ok(n)
}

/// # Safety: `buf` has `len` readable bytes; `recs` holds `max_recs` TotpRec.
#[no_mangle]
pub unsafe extern "C" fn totp_parse_store_rs(
    buf: *const u8,
    len: usize,
    recs: *mut TotpRec,
    max_recs: usize,
) -> i32 {
    if buf.is_null() || recs.is_null() || max_recs == 0 || max_recs > TOTP_MAX_RECS {
        return TOTP_E_ARG;
    }
    let b = core::slice::from_raw_parts(buf, len);
    let r = core::slice::from_raw_parts_mut(recs, max_recs);
    match parse_store(b, r) {
        Ok(n) => n as i32,
        Err(e) => {
            // A refused store must leave no half-loaded table behind, and no
            // secret bytes from a partially parsed row.
            for rec in r.iter_mut() {
                wipe(&mut rec.secret);
                wipe(&mut rec.issuer);
                wipe(&mut rec.label);
                rec.secret_len = 0;
                rec.digits = 0;
                rec.period = 0;
                rec.alg = 0;
            }
            e
        }
    }
}

// Re-encode raw secret bytes to base32 for storage. No padding is emitted; the
// decoder ignores '=' anyway and a shorter file is one fewer thing to get wrong.
const B32A: &[u8; 32] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

fn base32_encode(input: &[u8], out: &mut [u8]) -> Result<usize, i32> {
    let mut acc: u32 = 0;
    let mut bits: u32 = 0;
    let mut n = 0usize;
    for &b in input {
        acc = (acc << 8) | (b as u32);
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            if n >= out.len() {
                return Err(TOTP_E_NOSPACE);
            }
            out[n] = B32A[((acc >> bits) & 0x1F) as usize];
            n += 1;
        }
    }
    if bits > 0 {
        if n >= out.len() {
            return Err(TOTP_E_NOSPACE);
        }
        out[n] = B32A[((acc << (5 - bits)) & 0x1F) as usize];
        n += 1;
    }
    Ok(n)
}

fn put(out: &mut [u8], at: &mut usize, bytes: &[u8]) -> Result<(), i32> {
    if *at + bytes.len() > out.len() {
        return Err(TOTP_E_NOSPACE);
    }
    for &b in bytes {
        out[*at] = b;
        *at += 1;
    }
    Ok(())
}

fn put_u32(out: &mut [u8], at: &mut usize, v: u32) -> Result<(), i32> {
    let mut tmp = [0u8; 10];
    let mut i = tmp.len();
    let mut x = v;
    loop {
        i -= 1;
        tmp[i] = b'0' + (x % 10) as u8;
        x /= 10;
        if x == 0 {
            break;
        }
    }
    put(out, at, &tmp[i..])
}

fn cstr_len(b: &[u8]) -> usize {
    let mut i = 0usize;
    while i < b.len() && b[i] != 0 {
        i += 1;
    }
    i
}

fn format_store(recs: &[TotpRec], out: &mut [u8]) -> Result<usize, i32> {
    let mut at = 0usize;
    put(out, &mut at, &STORE_MAGIC[..])?;
    for r in recs {
        let mut b32 = [0u8; TOTP_B32_MAX];
        let sl = r.secret_len as usize;
        if sl == 0 || sl > TOTP_SECRET_MAX {
            return Err(TOTP_E_FORMAT);
        }
        let bn = base32_encode(&r.secret[..sl], &mut b32)?;
        put(out, &mut at, &r.issuer[..cstr_len(&r.issuer)])?;
        put(out, &mut at, b"\t")?;
        put(out, &mut at, &r.label[..cstr_len(&r.label)])?;
        put(out, &mut at, b"\t")?;
        put(out, &mut at, &b32[..bn])?;
        put(out, &mut at, b"\t")?;
        put_u32(out, &mut at, r.digits as u32)?;
        put(out, &mut at, b"\t")?;
        put_u32(out, &mut at, r.period as u32)?;
        put(out, &mut at, b"\t")?;
        put_u32(out, &mut at, r.alg as u32)?;
        put(out, &mut at, b"\n")?;
        wipe(&mut b32);
    }
    Ok(at)
}

/// # Safety: `recs` holds `count` TotpRec; `out` has `out_cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn totp_format_store_rs(
    recs: *const TotpRec,
    count: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    if out.is_null() || count > TOTP_MAX_RECS {
        return TOTP_E_ARG;
    }
    if count > 0 && recs.is_null() {
        return TOTP_E_ARG;
    }
    let r: &[TotpRec] = if count == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(recs, count)
    };
    let o = core::slice::from_raw_parts_mut(out, out_cap);
    match format_store(r, o) {
        Ok(n) => n as i32,
        Err(e) => {
            wipe(o);
            e
        }
    }
}

/// Bounded scan for a NUL terminator. Returns the bytes before it, or None if
/// there is no NUL within `max + 1` bytes. An unterminated buffer is a REFUSAL,
/// never a walk off the end: this is the one place enrolment text arrives from
/// Ring 3, and the historic shape of this bug is a string the caller promised was
/// terminated and was not.
///
/// # Safety: `p` has at least `max + 1` readable bytes, or a NUL before that.
unsafe fn cstr_bounded<'a>(p: *const u8, max: usize) -> Option<&'a [u8]> {
    let mut i = 0usize;
    while i <= max {
        if *p.add(i) == 0 {
            return Some(core::slice::from_raw_parts(p, i));
        }
        i += 1;
    }
    None
}

/// Build one record from enrolment fields. The base32 secret is decoded HERE, so
/// a bad secret is refused before anything is written to disk, and the same
/// policy checks that guard a parsed row guard a new one. `issuer`/`label`/`b32`
/// are NUL-terminated kernel buffers (proc/secretuse.c has already copied them
/// in from user space).
///
/// # Safety: the three pointers are NUL-terminated within `TOTP_B32_MAX + 1`
/// bytes; `rec` is writable.
#[no_mangle]
pub unsafe extern "C" fn totp_make_rec_rs(
    rec: *mut TotpRec,
    issuer: *const u8,
    label: *const u8,
    b32: *const u8,
    digits: u32,
    period: u32,
    alg: u32,
) -> i32 {
    if rec.is_null() || issuer.is_null() || label.is_null() || b32.is_null() {
        return TOTP_E_ARG;
    }
    let s_issuer = match cstr_bounded(issuer, TOTP_NAME_MAX) {
        Some(s) => s,
        None => return TOTP_E_FORMAT,
    };
    let s_label = match cstr_bounded(label, TOTP_NAME_MAX) {
        Some(s) => s,
        None => return TOTP_E_FORMAT,
    };
    let s_b32 = match cstr_bounded(b32, TOTP_B32_MAX) {
        Some(s) => s,
        None => return TOTP_E_FORMAT,
    };

    let mut r = TotpRec::zeroed();
    if copy_name(s_issuer, &mut r.issuer).is_err() {
        return TOTP_E_FORMAT;
    }
    if copy_name(s_label, &mut r.label).is_err() {
        return TOTP_E_FORMAT;
    }
    let sl = match base32_decode(s_b32, &mut r.secret) {
        Ok(n) => n,
        Err(e) => {
            wipe(&mut r.secret);
            return e;
        }
    };
    r.secret_len = sl as u16;
    if digits != 6 && digits != 8 {
        wipe(&mut r.secret);
        return TOTP_E_FORMAT;
    }
    r.digits = digits as u16;
    if period == 0 || period > 3600 {
        wipe(&mut r.secret);
        return TOTP_E_FORMAT;
    }
    r.period = period as u16;
    if alg != TOTP_ALG_SHA1 as u32 && alg != TOTP_ALG_SHA256 as u32 {
        wipe(&mut r.secret);
        return TOTP_E_FORMAT;
    }
    r.alg = alg as u16;
    *rec = r;
    0
}

// ===========================================================================
// Boot self-test. Returns 0, or the CASE NUMBER that failed, the same convention
// as secstore_selftest_rs(), because "it failed" is not useful on a serial
// console at 3am. `out_checks` receives how many checks actually ran: a
// self-test that runs ZERO checks and returns 0 is vacuous, which is the exact
// failure the contract work found this week (promise=fulfilled, paths=0/0), so
// the count is reported and main.c refuses a zero.
//
// The vectors are RFC 6238 Appendix B (SHA1 and SHA256 arms, all six times each)
// and RFC 4648 section 10. They are not self-generated: a round trip through our
// own encoder and decoder would pass with a wrong alphabet, and a TOTP that
// checks only "did I get six digits" passes with the truncation offset nailed to
// zero. Every case below can be FALSE for a plausible bug.
// ===========================================================================
#[no_mangle]
pub extern "C" fn totp_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    macro_rules! ck {
        ($case:expr, $cond:expr) => {{
            n += 1;
            if !($cond) {
                if !out_checks.is_null() {
                    unsafe { *out_checks = n };
                }
                return $case;
            }
        }};
    }

    // ---- RFC 4648 section 10 base32 vectors -------------------------------
    {
        let cases: [(&[u8], &[u8]); 6] = [
            (b"MY======", b"f"),
            (b"MZXQ====", b"fo"),
            (b"MZXW6===", b"foo"),
            (b"MZXW6YQ=", b"foob"),
            (b"MZXW6YTB", b"fooba"),
            (b"MZXW6YTBOI======", b"foobar"),
        ];
        let mut case_no: i32 = 1;
        for &(enc, dec) in cases.iter() {
            let mut buf = [0u8; 16];
            let r = base32_decode(enc, &mut buf);
            ck!(case_no, r.is_ok());
            let got = r.unwrap_or(0);
            ck!(case_no, got == dec.len() && &buf[..got] == dec);
            case_no += 1;
        }
    }

    // ---- base32 REFUSALS. Each names a specific way input can lie. --------
    {
        let mut buf = [0u8; 16];
        // 7: '1' and '8' are not in the RFC 4648 alphabet (the classic
        // confusable-with-I/B pair). The app's decoder returned -1 here too;
        // this proves we did not lose that.
        ck!(7, base32_decode(b"MZXW1YTB", &mut buf) == Err(TOTP_E_B32));
        ck!(8, base32_decode(b"MZXW8YTB", &mut buf) == Err(TOTP_E_B32));
        // 9: lower case and separators are ACCEPTED (users paste spaced groups).
        let r = base32_decode(b"mzxw 6ytb-oi", &mut buf);
        ck!(9, r == Ok(6) && &buf[..6] == b"foobar");
        // 10: empty, and padding-only, decode to nothing and are refused rather
        // than becoming a zero-length secret that would generate a code.
        ck!(10, base32_decode(b"", &mut buf) == Err(TOTP_E_B32));
        ck!(11, base32_decode(b"========", &mut buf) == Err(TOTP_E_B32));
        // 12: NON-CANONICAL padding bits are refused. "MY" and "MZ" both carry
        // one byte of data (0x66, 'f') and differ only in the two leftover bits
        // that base32 requires to be zero. A lax decoder returns "f" for BOTH, so
        // two different strings would mean the same secret, and a user who
        // mistyped one character would get working codes from the wrong text and
        // never learn that their backup copy does not match. The refusal is what
        // makes the encoding injective.
        //
        // Note the trap this replaced: the first draft used "MZXW6YTC", which is
        // 8 characters, exactly 40 bits, and therefore has NO leftover bits at
        // all. It is perfectly valid base32 for a different 5 bytes, and asserting
        // a refusal on it failed GREEN. A refusal case has to be built from the
        // arithmetic, not from a string that merely looks corrupt.
        ck!(12, base32_decode(b"MZ", &mut buf) == Err(TOTP_E_B32));
        ck!(12, base32_decode(b"MZ======", &mut buf) == Err(TOTP_E_B32));
        // and the canonical neighbour still decodes, so case 12 is a statement
        // about the padding bits and not about the length.
        ck!(12, base32_decode(b"MY", &mut buf) == Ok(1) && buf[0] == b'f');
        // 13: output too small is NOSPACE, distinct from bad-alphabet, and the
        // caller can tell "your buffer" from "your input".
        let mut tiny = [0u8; 2];
        ck!(13, base32_decode(b"MZXW6YTBOI======", &mut tiny) == Err(TOTP_E_NOSPACE));
    }

    // ---- RFC 6238 Appendix B, SHA1 arm, seed "12345678901234567890" -------
    // 8 digits, period 30. All six published times.
    {
        let seed = b"12345678901234567890";
        let mut rec = TotpRec::zeroed();
        rec.secret[..seed.len()].copy_from_slice(seed);
        rec.secret_len = seed.len() as u16;
        rec.digits = 8;
        rec.period = 30;
        rec.alg = TOTP_ALG_SHA1;
        let vecs: [(i64, &[u8]); 6] = [
            (59, b"94287082"),
            (1111111109, b"07081804"),
            (1111111111, b"14050471"),
            (1234567890, b"89005924"),
            (2000000000, b"69279037"),
            (20000000000, b"65353130"),
        ];
        let mut case_no: i32 = 14;
        for &(t, want) in vecs.iter() {
            let mut out = [0u8; TOTP_CODE_MAX];
            let rem = unsafe {
                totp_code_rs(&rec as *const TotpRec, t, out.as_mut_ptr(), out.len())
            };
            ck!(case_no, rem >= 1 && rem <= 30);
            ck!(case_no, &out[..8] == want && out[8] == 0);
            case_no += 1;
        }
    }

    // ---- RFC 6238 Appendix B, SHA256 arm, 32-byte seed --------------------
    {
        let seed = b"12345678901234567890123456789012";
        let mut rec = TotpRec::zeroed();
        rec.secret[..seed.len()].copy_from_slice(seed);
        rec.secret_len = seed.len() as u16;
        rec.digits = 8;
        rec.period = 30;
        rec.alg = TOTP_ALG_SHA256;
        let vecs: [(i64, &[u8]); 6] = [
            (59, b"46119246"),
            (1111111109, b"68084774"),
            (1111111111, b"67062674"),
            (1234567890, b"91819424"),
            (2000000000, b"90698825"),
            (20000000000, b"77737706"),
        ];
        let mut case_no: i32 = 20;
        for &(t, want) in vecs.iter() {
            let mut out = [0u8; TOTP_CODE_MAX];
            let rem = unsafe {
                totp_code_rs(&rec as *const TotpRec, t, out.as_mut_ptr(), out.len())
            };
            ck!(case_no, rem >= 1 && rem <= 30);
            ck!(case_no, &out[..8] == want && out[8] == 0);
            case_no += 1;
        }
    }

    // ---- code-path REFUSALS ----------------------------------------------
    {
        let seed = b"12345678901234567890";
        let mut rec = TotpRec::zeroed();
        rec.secret[..seed.len()].copy_from_slice(seed);
        rec.secret_len = seed.len() as u16;
        rec.digits = 6;
        rec.period = 30;
        rec.alg = TOTP_ALG_SHA1;
        let mut out = [0u8; TOTP_CODE_MAX];
        // 26: NO CLOCK is a refusal, not a code at counter 0. realtime_sec_rs()
        // returns 0 when the RTC has never been plausible, and a wrong code that
        // looks right is worse than an error the user can act on.
        ck!(26, unsafe {
            totp_code_rs(&rec as *const TotpRec, 0, out.as_mut_ptr(), out.len())
        } == TOTP_E_NOCLOCK);
        ck!(26, unsafe {
            totp_code_rs(&rec as *const TotpRec, -1, out.as_mut_ptr(), out.len())
        } == TOTP_E_NOCLOCK);
        // 27: a 6-digit code is the low 6 digits of the 8-digit vector, which
        // ties the digits policy to the SAME published vector rather than to a
        // second hand-typed constant.
        let rem = unsafe {
            totp_code_rs(&rec as *const TotpRec, 59, out.as_mut_ptr(), out.len())
        };
        ck!(27, rem == 1 && &out[..6] == b"287082" && out[6] == 0);
        // 28: a 7-digit request is outside RFC 6238 and is refused, not rounded.
        let mut bad = rec;
        bad.digits = 7;
        ck!(28, unsafe {
            totp_code_rs(&bad as *const TotpRec, 59, out.as_mut_ptr(), out.len())
        } == TOTP_E_FORMAT);
        // 29: period 0 would divide by zero. Refused before the division.
        let mut bad2 = rec;
        bad2.period = 0;
        ck!(29, unsafe {
            totp_code_rs(&bad2 as *const TotpRec, 59, out.as_mut_ptr(), out.len())
        } == TOTP_E_FORMAT);
        // 30: an unknown algorithm is refused, not silently treated as SHA-1.
        let mut bad3 = rec;
        bad3.alg = 512;
        ck!(30, unsafe {
            totp_code_rs(&bad3 as *const TotpRec, 59, out.as_mut_ptr(), out.len())
        } == TOTP_E_FORMAT);
        // 31: an output buffer too small to hold digits + NUL is refused, never
        // written short. A code missing its last digit is a code that fails to
        // authenticate and looks like a wrong secret.
        let mut small = [0u8; 6];
        ck!(31, unsafe {
            totp_code_rs(&rec as *const TotpRec, 59, small.as_mut_ptr(), small.len())
        } == TOTP_E_NOSPACE);
        // 32: the countdown really counts down. t=59 is 1s from the boundary and
        // t=30 is 30s from it, so a constant would fail.
        let r30 = unsafe {
            totp_code_rs(&rec as *const TotpRec, 30, out.as_mut_ptr(), out.len())
        };
        ck!(32, r30 == 30);
    }

    // ---- store round trip and parse REFUSALS ------------------------------
    {
        let mut recs = [TotpRec::zeroed(); TOTP_MAX_RECS];
        // UNPADDED base32 here on purpose: format_store() emits no '=' padding
        // (the decoder ignores it, and a shorter file is one fewer thing to get
        // wrong), so this is the canonical form that case 34 round-trips. That a
        // PADDED store also parses is case 36b below, not an assumption.
        let good: &[u8] = b"MFADB1\nACME\talice@acme\tMZXW6YTBOI\t6\t30\t1\n\
                            Bank\tbob\tJBSWY3DPEHPK3PXP\t8\t60\t256\n";
        let k = parse_store(good, &mut recs);
        // 33: two rows parse, with the fields landing where they belong.
        ck!(33, k == Ok(2));
        ck!(33, &recs[0].issuer[..4] == b"ACME" && recs[0].issuer[4] == 0);
        ck!(33, recs[0].digits == 6 && recs[0].period == 30 && recs[0].alg == TOTP_ALG_SHA1);
        ck!(33, recs[0].secret_len == 6 && &recs[0].secret[..6] == b"foobar");
        ck!(33, recs[1].digits == 8 && recs[1].period == 60 && recs[1].alg == TOTP_ALG_SHA256);

        // 34: format(parse(x)) == x. This is the migration-safety property: the
        // sealed plaintext is byte-identical to the legacy de-obfuscated file, so
        // migration cannot reshape the data.
        let mut back = [0u8; 512];
        let m = format_store(&recs[..2], &mut back);
        ck!(34, m.is_ok());
        let mlen = m.unwrap_or(0);
        ck!(34, mlen == good.len() && &back[..mlen] == good);

        // 35: no header is a refusal. The app read a bad header as "no accounts",
        // which is how a failed load became indistinguishable from an empty store.
        ck!(35, parse_store(b"XXXXXX\n", &mut recs) == Err(TOTP_E_MAGIC));
        ck!(35, parse_store(b"", &mut recs) == Err(TOTP_E_MAGIC));
        // 36: header only is a VALID EMPTY store, and must not be an error: a
        // user who deleted their last account has an empty store, not a broken one.
        ck!(36, parse_store(b"MFADB1\n", &mut recs) == Ok(0));
        // 36b: a PADDED secret in a store row parses to the same bytes, which is
        // what makes the legacy MFA.DB migration safe: the app's db_save() wrote
        // padded base32, and those exact bytes become the sealed plaintext.
        let padded: &[u8] = b"MFADB1\nA\tb\tMZXW6YTBOI======\t6\t30\t1\n";
        ck!(36, parse_store(padded, &mut recs) == Ok(1));
        ck!(36, recs[0].secret_len == 6 && &recs[0].secret[..6] == b"foobar");
        // 37: five fields instead of six.
        ck!(37, parse_store(b"MFADB1\nA\tb\tMY======\t6\t30\n", &mut recs) == Err(TOTP_E_FORMAT));
        // 38: a row with no terminating newline is a TRUNCATED store, refused
        // rather than accepted as a last record.
        ck!(38, parse_store(b"MFADB1\nA\tb\tMY======\t6\t30\t1", &mut recs) == Err(TOTP_E_FORMAT));
        // 38b: SEVEN fields is also a refusal. A row with an extra field means
        // something this parser does not understand, and guessing that the extra
        // one is ignorable is how a format gains an unversioned second meaning.
        ck!(38, parse_store(b"MFADB1\nA\tb\tMY======\t6\t30\t1\tx\n", &mut recs)
                == Err(TOTP_E_FORMAT));
        // 39: digits outside {6,8}.
        ck!(39, parse_store(b"MFADB1\nA\tb\tMY======\t7\t30\t1\n", &mut recs) == Err(TOTP_E_FORMAT));
        // 40: period 0, and a period past the sane bound.
        ck!(40, parse_store(b"MFADB1\nA\tb\tMY======\t6\t0\t1\n", &mut recs) == Err(TOTP_E_FORMAT));
        ck!(40, parse_store(b"MFADB1\nA\tb\tMY======\t6\t99999\t1\n", &mut recs) == Err(TOTP_E_FORMAT));
        // 41: an algorithm nobody implements.
        ck!(41, parse_store(b"MFADB1\nA\tb\tMY======\t6\t30\t512\n", &mut recs) == Err(TOTP_E_FORMAT));
        // 42: a non-numeric numeric field.
        ck!(42, parse_store(b"MFADB1\nA\tb\tMY======\tsix\t30\t1\n", &mut recs) == Err(TOTP_E_FORMAT));
        // 43: bad base32 in a row refuses the WHOLE store, and reports why.
        ck!(43, parse_store(b"MFADB1\nA\tb\tMZXW1YTB\t6\t30\t1\n", &mut recs) == Err(TOTP_E_B32));
        // 44: a name at exactly the buffer size has no room for its NUL and is
        // refused rather than truncated to a different account name.
        let mut long: [u8; 7 + TOTP_NAME_MAX + 32] = [b'x'; 7 + TOTP_NAME_MAX + 32];
        long[..7].copy_from_slice(b"MFADB1\n");
        let tail = b"\tb\tMY======\t6\t30\t1\n";
        long[7 + TOTP_NAME_MAX..7 + TOTP_NAME_MAX + tail.len()].copy_from_slice(tail);
        ck!(44, parse_store(&long[..7 + TOTP_NAME_MAX + tail.len()], &mut recs)
                == Err(TOTP_E_FORMAT));
        // 45: a control byte in a name is refused, because writing it back out
        // could re-split the row into different fields, which is a stored record
        // that means something other than what was enrolled.
        ck!(45, parse_store(b"MFADB1\nA\x01x\tb\tMY======\t6\t30\t1\n", &mut recs)
                == Err(TOTP_E_FORMAT));
        // 46: more rows than the table holds is NOSPACE, not a silent drop of
        // the accounts that did not fit.
        let mut only_one = [TotpRec::zeroed(); 1];
        ck!(46, parse_store(good, &mut only_one) == Err(TOTP_E_NOSPACE));
        // 47: a refused parse leaves NO secret bytes in the caller's table.
        let mut dirty = [TotpRec::zeroed(); 2];
        let _ = parse_store(good, &mut dirty);
        let bad: &[u8] = b"MFADB1\nA\tb\tMZXW1YTB\t6\t30\t1\n";
        let rc = unsafe {
            totp_parse_store_rs(bad.as_ptr(), bad.len(), dirty.as_mut_ptr(), 2)
        };
        ck!(47, rc == TOTP_E_B32);
        let mut any: u16 = 0;
        for r in dirty.iter() {
            for &b in r.secret.iter() {
                any |= b as u16;
            }
            any |= r.secret_len;
            any |= r.digits;
        }
        ck!(47, any == 0);
    }

    // ---- enrolment field checks ------------------------------------------
    {
        let mut rec = TotpRec::zeroed();
        // 48: a well-formed enrolment succeeds and decodes the secret.
        let rc = unsafe {
            totp_make_rec_rs(&mut rec, b"ACME\0".as_ptr(), b"alice\0".as_ptr(),
                             b"MZXW6YTBOI======\0".as_ptr(), 6, 30, 1)
        };
        ck!(48, rc == 0 && rec.secret_len == 6 && &rec.secret[..6] == b"foobar");
        // 49: a bad secret is refused BEFORE anything reaches the disk.
        ck!(49, unsafe {
            totp_make_rec_rs(&mut rec, b"A\0".as_ptr(), b"b\0".as_ptr(),
                             b"MZXW1YTB\0".as_ptr(), 6, 30, 1)
        } == TOTP_E_B32);
        // 50: digits, period and algorithm outside policy are refused here too,
        // so an enrolment cannot create a record the parser would later reject.
        ck!(50, unsafe {
            totp_make_rec_rs(&mut rec, b"A\0".as_ptr(), b"b\0".as_ptr(),
                             b"MY======\0".as_ptr(), 7, 30, 1)
        } == TOTP_E_FORMAT);
        ck!(50, unsafe {
            totp_make_rec_rs(&mut rec, b"A\0".as_ptr(), b"b\0".as_ptr(),
                             b"MY======\0".as_ptr(), 6, 0, 1)
        } == TOTP_E_FORMAT);
        ck!(50, unsafe {
            totp_make_rec_rs(&mut rec, b"A\0".as_ptr(), b"b\0".as_ptr(),
                             b"MY======\0".as_ptr(), 6, 30, 512)
        } == TOTP_E_FORMAT);
        // 51: an UNTERMINATED buffer is refused rather than scanned off the end.
        // This is the one place enrolment text arrives from Ring 3.
        let unterm = [b'A'; TOTP_NAME_MAX + 8];
        ck!(51, unsafe {
            totp_make_rec_rs(&mut rec, unterm.as_ptr(), b"b\0".as_ptr(),
                             b"MY======\0".as_ptr(), 6, 30, 1)
        } == TOTP_E_FORMAT);
        // 52: wiping a record really clears the secret. A delete that leaves the
        // bytes behind is a delete that did not happen.
        let mut w = rec;
        w.secret[0] = 0xAB;
        w.secret_len = 1;
        unsafe { totp_wipe_rec_rs(&mut w) };
        let mut any = 0u8;
        for &b in w.secret.iter() {
            any |= b;
        }
        ck!(52, any == 0 && w.secret_len == 0 && w.digits == 0);
    }

    // ---- the tier table --------------------------------------------------
    {
        // 53: the ONE declared class maps to the ONE implemented key source.
        ck!(53, secret_keysrc_for_class_rs(SECRET_CLASS_TOTP) == SECRET_KEYSRC_LOGIN);
        ck!(53, secret_keysrc_is_implemented_rs(SECRET_KEYSRC_LOGIN) == 1);
        // 54: an UNDECLARED class gets NONE, which proc/secretuse.c refuses. This
        // is the fail-closed property: adding a secret to the store without
        // deciding how it is keyed does not inherit the login key, it does not
        // work. If somebody later declares class 2 and this case starts failing,
        // that is the gate doing its job, not a broken test.
        ck!(54, secret_keysrc_for_class_rs(SECRET_CLASS_NONE) == SECRET_KEYSRC_NONE);
        for c in 2u32..16u32 {
            ck!(54, secret_keysrc_for_class_rs(c) == SECRET_KEYSRC_NONE);
        }
        // 55: the headless and TPM tiers are DECLARED and NOT IMPLEMENTED, and
        // the code says so rather than a comment saying so. When one is built,
        // this case must be updated in the same change, which is the point.
        ck!(55, secret_keysrc_is_implemented_rs(SECRET_KEYSRC_HEADLESS) == 0);
        ck!(55, secret_keysrc_is_implemented_rs(SECRET_KEYSRC_TPM) == 0);
        ck!(55, secret_keysrc_is_implemented_rs(SECRET_KEYSRC_NONE) == 0);
    }

    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    0
}
