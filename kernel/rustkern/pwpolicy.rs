// rustkern/pwpolicy.rs - password strength policy and breached-password
// rejection. New kernel logic with no C twin to strangle, so Rust per the
// 2026-07-16 rule, in the same shape as sessionid.rs: this module holds the
// DECISION and calls no C at all. The C glue in proc/users.c owns the tables
// and passes bytes in.
//
// ===========================================================================
// WHAT WAS ACTUALLY WRONG
// ---------------------------------------------------------------------------
// The public hardening page stated it plainly, and it was true:
// user_set_password() rejected only an EMPTY password, and the sole length rule
// was a 6-character minimum applied at first-boot account creation and nowhere
// else. So `passwd` could set "a", Settings' Add User could set "1", and the
// only path with any rule at all was the one screen a user sees once.
//
// Storage was never the weak part (PBKDF2-HMAC-SHA256, 50,000 iterations,
// 16-byte DRBG salt). PBKDF2 raises the cost of guessing an UNKNOWN password;
// it does nothing at all when the password is "123456789", because the attacker
// does not have to search, they have to try the top of a list. The rule that
// matters against that is the one this file implements.
//
// ===========================================================================
// WHY THESE RULES AND NOT COMPOSITION RULES
// ---------------------------------------------------------------------------
// NIST SP 800-63B is explicit that composition rules ("must contain an upper,
// a digit and a symbol") make passwords WORSE: users satisfy them with
// predictable transforms (password -> Password1!), which is exactly the shape
// the breach lists are full of. What it recommends instead, and what this
// implements, is a length floor plus a check against known-compromised values.
//
// So there is deliberately NO character-class requirement here. That is a
// decision, not an omission, and the hardening page says so in as many words.
// The rules are:
//
//   1. Not empty.                                     PW_ERR_EMPTY
//   2. No control characters.                         PW_ERR_BADCHAR
//      Bytes >= 0x80 are ALLOWED: a UTF-8 passphrase is a good password and
//      PBKDF2 hashes bytes, so there is no reason to refuse them.
//   3. At least PW_MIN_LEN bytes.                     PW_ERR_TOO_SHORT
//   4. At most PW_MAX_LEN bytes.                      PW_ERR_TOO_LONG
//      A bound, not a strength rule: it keeps the syscall bounce buffers and
//      the PBKDF2 input honest. Deliberately far above any real password.
//   5. Does not contain the username (either way).    PW_ERR_CONTAINS_USERNAME
//   6. Uses at least PW_MIN_DISTINCT distinct bytes.  PW_ERR_LOW_VARIETY
//      Catches "aaaaaaaa", "abababab", "abcabcabc": long by the length rule,
//      trivially guessable in reality.
//   7. Is not a keyboard or counting run.             PW_ERR_SEQUENCE
//   8. Is not on the breached list.                   PW_ERR_BREACHED
//
// ===========================================================================
// THE BREACHED-PASSWORD TABLE
// ---------------------------------------------------------------------------
// Source: rockyou top 50,000 (see kernel/tools/pwbreach-gen/pwbreach-gen.py for
// the URL and the exact conversion). The kernel does NOT ship the 403 KB of
// plaintext: pwbreach.bin holds a SORTED, DEDUPLICATED array of 32-bit
// truncated FNV-1a hashes, binary searched here.
//
//   size          66,524 bytes (16-byte header + 16,627 x 4)
//   false NEG     structurally impossible for a list entry. Every entry's own
//                 hash is in the table, so a listed password is always found.
//                 That is the direction that matters: a false negative would
//                 ACCEPT a breached password. The generator re-verifies it by
//                 binary searching all 49,999 source words back out of the
//                 table it just wrote, and the host harness re-verifies it
//                 through this exact module.
//   false POS     16,627 / 2^32 per lookup = 1 in 258,314. Two lookups are done
//                 (the candidate and its lowercased form), so about 1 in
//                 129,000 non-breached passwords are refused. Measured: 1 in
//                 200,000 random 12-character passwords. The cost of a false
//                 positive is one message asking the user to pick another
//                 password, which is why the rate is set where it is.
//
// Entries shorter than PW_MIN_LEN BYTES are not in the table because rule 3
// rejects them before the lookup is reached; the two rules together still
// reject all 49,999. BYTES, not characters: a 7-character word that is 8 bytes
// of UTF-8 passes the length rule and must be in the table, and filtering the
// list on character count let exactly two such words through until the host
// harness caught it. The header records the length the list was filtered at and
// pwpolicy_selftest_rs() FAILS if it disagrees with PW_MIN_LEN, so lowering the
// minimum without regenerating the table is loud rather than silent.
//
// HONEST LIMITS, stated because overstating a control is worse than not having
// one:
//   - This is a MEMBERSHIP test over one 50k list, not a strength estimator. It
//     knows nothing about "Passw0rd!2026", which is not on the list and is a
//     bad password.
//   - No leetspeak or suffix normalisation: "password1234" is caught only if it
//     is itself on the list.
//   - Case folding is ASCII only, and only for the whole-string lookup.
// ===========================================================================

// ---------------------------------------------------------------------------
// Result codes. MUST match the PW_* defines in proc/pwpolicy.h (kernel) and
// libc/pwpolicy.h (userland). The order is the order the checks run in, so the
// code a caller receives is the FIRST rule the password broke.
// ---------------------------------------------------------------------------
pub const PW_OK: u32 = 0;
pub const PW_ERR_EMPTY: u32 = 1;
pub const PW_ERR_BADCHAR: u32 = 2;
pub const PW_ERR_TOO_SHORT: u32 = 3;
pub const PW_ERR_TOO_LONG: u32 = 4;
pub const PW_ERR_CONTAINS_USERNAME: u32 = 5;
pub const PW_ERR_LOW_VARIETY: u32 = 6;
pub const PW_ERR_SEQUENCE: u32 = 7;
pub const PW_ERR_BREACHED: u32 = 8;
/// #745. NOT produced by pw_check(): that judges one password in isolation and
/// has no second password to compare against. It is produced by the first-boot
/// PAIR check in proc/users.c, and it lives here so the three copies of this
/// enumeration stay identical and nobody reuses 9 for a real strength rule.
pub const PW_ERR_SAME_AS_OTHER: u32 = 9;

/// Minimum accepted length. 8 is the NIST SP 800-63B floor for a
/// user-chosen secret that is also checked against a breach list. The old
/// first-boot rule was 6 and applied on one screen only.
pub const PW_MIN_LEN: usize = 8;
/// Upper bound. Not a strength rule; see the header comment.
pub const PW_MAX_LEN: usize = 127;
/// Distinct byte values a password must use.
pub const PW_MIN_DISTINCT: usize = 4;
/// Usernames shorter than this are not substring-matched: a 2-character name
/// would refuse far too many legitimate passwords for no real gain.
const USERNAME_MIN_MATCH: usize = 3;

// Scratch sizes. PW_MAX_LEN + 1 and USERNAME_MAX from proc/users.h.
const PW_BUF: usize = 128;
const USER_BUF: usize = 64;

// ---------------------------------------------------------------------------
// The breached-password table (see kernel/tools/pwbreach-gen/pwbreach-gen.py).
// ---------------------------------------------------------------------------
static BREACH: &[u8] = include_bytes!("pwbreach.bin");
/// b"MPWB" read as a little-endian u32.
const BLOB_MAGIC: u32 = 0x4257_504D;
const BLOB_HEADER_LEN: usize = 16;

fn rd32(b: &[u8], off: usize) -> u32 {
    if off + 4 > b.len() {
        return 0;
    }
    (b[off] as u32)
        | ((b[off + 1] as u32) << 8)
        | ((b[off + 2] as u32) << 16)
        | ((b[off + 3] as u32) << 24)
}

fn blob_ok() -> bool {
    BREACH.len() >= BLOB_HEADER_LEN && rd32(BREACH, 0) == BLOB_MAGIC
}

/// Number of hashes in the table. 0 when the blob is missing or malformed,
/// which would silently disable the breach check, so pwpolicy_selftest_rs()
/// asserts a plausible count at boot.
fn breach_count() -> usize {
    if !blob_ok() {
        return 0;
    }
    let declared = rd32(BREACH, 12) as usize;
    let available = (BREACH.len() - BLOB_HEADER_LEN) / 4;
    if declared > available {
        available
    } else {
        declared
    }
}

/// The min length the source list was filtered at, from the blob header.
fn breach_min_len() -> usize {
    if !blob_ok() {
        return 0;
    }
    rd32(BREACH, 8) as usize
}

fn breach_at(i: usize) -> u32 {
    rd32(BREACH, BLOB_HEADER_LEN + i * 4)
}

fn breach_contains(h: u32) -> bool {
    let mut lo: usize = 0;
    let mut hi: usize = breach_count();
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let v = breach_at(mid);
        if v == h {
            return true;
        }
        if v < h {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    false
}

/// FNV-1a 64, truncated to its low 32 bits. Must stay byte-identical to
/// pwbreach-gen.py's fnv1a32() or the table means nothing.
fn fnv1a32(s: &[u8]) -> u32 {
    let mut h: u64 = 0xCBF2_9CE4_8422_2325;
    let mut i = 0usize;
    while i < s.len() {
        h ^= s[i] as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01B3);
        i += 1;
    }
    h as u32
}

// ---------------------------------------------------------------------------
// Keyboard and counting runs. A password that is wholly a contiguous slice of
// one of these is a run. Written out in both directions rather than reversing
// at runtime, because the reversed forms are what people actually type.
// ---------------------------------------------------------------------------
const RUNS: [&[u8]; 10] = [
    b"12345678901234567890",
    b"09876543210987654321",
    b"abcdefghijklmnopqrstuvwxyz",
    b"zyxwvutsrqponmlkjihgfedcba",
    b"qwertyuiop",
    b"poiuytrewq",
    b"asdfghjkl",
    b"lkjhgfdsa",
    b"zxcvbnm",
    b"mnbvcxz",
];

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || needle.len() > hay.len() {
        return false;
    }
    let last = hay.len() - needle.len();
    let mut i = 0usize;
    while i <= last {
        let mut j = 0usize;
        while j < needle.len() && hay[i + j] == needle[j] {
            j += 1;
        }
        if j == needle.len() {
            return true;
        }
        i += 1;
    }
    false
}

fn ascii_lower_into(src: &[u8], dst: &mut [u8]) -> usize {
    let n = if src.len() < dst.len() { src.len() } else { dst.len() };
    let mut i = 0usize;
    while i < n {
        let c = src[i];
        dst[i] = if c >= b'A' && c <= b'Z' { c + 32 } else { c };
        i += 1;
    }
    n
}

fn distinct_bytes(s: &[u8]) -> usize {
    let mut seen: [u64; 4] = [0; 4];
    let mut n = 0usize;
    let mut i = 0usize;
    while i < s.len() {
        let c = s[i] as usize;
        let w = c >> 6;
        let b = 1u64 << (c & 63);
        if seen[w] & b == 0 {
            seen[w] |= b;
            n += 1;
        }
        i += 1;
    }
    n
}

// ---------------------------------------------------------------------------
// THE POLICY. Pure over its inputs; every caller reaches it.
// ---------------------------------------------------------------------------
pub fn pw_check(pw: &[u8], user: &[u8]) -> u32 {
    if pw.is_empty() {
        return PW_ERR_EMPTY;
    }
    if pw.len() > PW_MAX_LEN {
        return PW_ERR_TOO_LONG;
    }

    // Control characters, including DEL. High bytes are allowed on purpose.
    {
        let mut i = 0usize;
        while i < pw.len() {
            let c = pw[i];
            if c < 0x20 || c == 0x7F {
                return PW_ERR_BADCHAR;
            }
            i += 1;
        }
    }

    if pw.len() < PW_MIN_LEN {
        return PW_ERR_TOO_SHORT;
    }

    let mut lowbuf = [0u8; PW_BUF];
    let ln = ascii_lower_into(pw, &mut lowbuf);
    let low = &lowbuf[..ln];

    // The username, either way round. "alice" must not be able to pick
    // "alice1234", and "al" (below USERNAME_MIN_MATCH) does not trigger it.
    if user.len() >= USERNAME_MIN_MATCH {
        let mut ubuf = [0u8; USER_BUF];
        let un = ascii_lower_into(user, &mut ubuf);
        let ulow = &ubuf[..un];
        if contains(low, ulow) || contains(ulow, low) {
            return PW_ERR_CONTAINS_USERNAME;
        }
    }

    if distinct_bytes(pw) < PW_MIN_DISTINCT {
        return PW_ERR_LOW_VARIETY;
    }

    {
        let mut i = 0usize;
        while i < RUNS.len() {
            if contains(RUNS[i], low) {
                return PW_ERR_SEQUENCE;
            }
            i += 1;
        }
    }

    // Breach membership: the candidate as typed, and its ASCII-lowercased form
    // so any case variant of a lowercase list entry is caught.
    if breach_contains(fnv1a32(pw)) {
        return PW_ERR_BREACHED;
    }
    if ln != pw.len() || low != pw {
        if breach_contains(fnv1a32(low)) {
            return PW_ERR_BREACHED;
        }
    }

    PW_OK
}

// ---------------------------------------------------------------------------
// FFI
// ---------------------------------------------------------------------------

/// Policy constants and table facts, for the boot log and for UI that wants to
/// state the rule rather than hardcode it. Mirrored by pw_policy_info_t in
/// proc/pwpolicy.h with a _Static_assert on its size.
#[repr(C)]
pub struct PwPolicyInfo {
    pub min_len: u32,
    pub max_len: u32,
    pub min_distinct: u32,
    pub table_entries: u32,
    pub table_min_len: u32,
    pub table_bytes: u32,
}

#[no_mangle]
pub unsafe extern "C" fn pw_policy_info_rs(out: *mut PwPolicyInfo) {
    if out.is_null() {
        return;
    }
    // SAFETY: caller passes a writable PwPolicyInfo. Written once, no aliasing.
    unsafe {
        (*out).min_len = PW_MIN_LEN as u32;
        (*out).max_len = PW_MAX_LEN as u32;
        (*out).min_distinct = PW_MIN_DISTINCT as u32;
        (*out).table_entries = breach_count() as u32;
        (*out).table_min_len = breach_min_len() as u32;
        (*out).table_bytes = BREACH.len() as u32;
    }
}

/// Check a candidate password. Returns PW_OK or the first PW_ERR_* it broke.
///
/// `pw`/`pw_len`      the candidate, as bytes, NOT NUL terminated by contract.
/// `user`/`user_len`  the account name it is being set for; may be NULL/0, in
///                    which case the contains-username rule is skipped.
#[no_mangle]
pub unsafe extern "C" fn pw_policy_check_rs(
    pw: *const u8,
    pw_len: u32,
    user: *const u8,
    user_len: u32,
) -> u32 {
    if pw.is_null() || pw_len == 0 {
        return PW_ERR_EMPTY;
    }
    // Cap BEFORE forming the slice: an over-long password is refused on its
    // length, and no slice longer than the bound is ever constructed.
    if pw_len as usize > PW_MAX_LEN {
        return PW_ERR_TOO_LONG;
    }
    // SAFETY: caller guarantees pw points at pw_len readable bytes (proc/users.c
    // passes a kernel buffer whose length it measured with strlen).
    let pws = unsafe { core::slice::from_raw_parts(pw, pw_len as usize) };
    let us: &[u8] = if user.is_null() || user_len == 0 {
        &[]
    } else {
        let ul = if user_len as usize > USER_BUF {
            USER_BUF
        } else {
            user_len as usize
        };
        // SAFETY: same contract; length clamped to USER_BUF.
        unsafe { core::slice::from_raw_parts(user, ul) }
    };
    pw_check(pws, us)
}

// ---------------------------------------------------------------------------
// SELF-TEST. Run once at boot so the policy is proven LIVE on this build rather
// than merely compiled in (same discipline as sessionid_selftest_rs). Returns a
// bit mask of failures; 0 is a pass and anything else is loud.
//
// The RED/GREEN pair the whole change turns on is bits 8 and 12: a known
// breached password is REFUSED, and a strong one is ACCEPTED.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn pwpolicy_selftest_rs() -> u32 {
    let mut fails: u32 = 0;

    // 1. The table is actually linked in and sane. A zero count would disable
    //    the breach check with no other symptom, which is the silent-skip
    //    failure this project keeps relearning.
    if !blob_ok() {
        fails |= 1 << 0;
    }
    if breach_count() < 10_000 {
        fails |= 1 << 1;
    }
    // 2. The table was filtered at the length this policy enforces. If someone
    //    lowers PW_MIN_LEN without regenerating pwbreach.bin, the shorter
    //    breached passwords become reachable AND absent from the table.
    if breach_min_len() != PW_MIN_LEN {
        fails |= 1 << 2;
    }
    // 3. The table is sorted ascending, which is what makes the binary search a
    //    membership test rather than a coin flip. Checked over every entry;
    //    16,625 comparisons is nothing at boot.
    {
        let n = breach_count();
        let mut i = 1usize;
        let mut prev = if n > 0 { breach_at(0) } else { 0 };
        while i < n {
            let v = breach_at(i);
            if v < prev {
                fails |= 1 << 3;
                break;
            }
            prev = v;
            i += 1;
        }
    }

    // 4. Empty and control characters.
    if pw_check(b"", b"") != PW_ERR_EMPTY {
        fails |= 1 << 4;
    }
    if pw_check(b"abc\x01defgh", b"") != PW_ERR_BADCHAR {
        fails |= 1 << 5;
    }
    // 5. Length floor. 7 characters, varied, not on the list.
    if pw_check(b"Kx7#vqL", b"") != PW_ERR_TOO_SHORT {
        fails |= 1 << 6;
    }
    // 6. Length ceiling.
    {
        let mut longpw = [b'a'; 200];
        let mut i = 0usize;
        while i < longpw.len() {
            longpw[i] = b'a' + ((i % 23) as u8);
            i += 1;
        }
        if pw_check(&longpw, b"") != PW_ERR_TOO_LONG {
            fails |= 1 << 7;
        }
    }

    // 7. THE RED HALF: known-breached passwords are REFUSED. All three are in
    //    the rockyou top 50k. The third is mixed case and is caught only by the
    //    lowercase lookup, which is why it is here.
    if pw_check(b"password1", b"") != PW_ERR_BREACHED {
        fails |= 1 << 8;
    }
    if pw_check(b"trustno1", b"") != PW_ERR_BREACHED {
        fails |= 1 << 9;
    }
    if pw_check(b"ILoveYou", b"") != PW_ERR_BREACHED {
        fails |= 1 << 10;
    }
    // 8. A password NOT on the list but adjacent to it must NOT be reported as
    //    breached, or the check would be indistinguishable from "reject
    //    everything".
    if pw_check(b"Kx7#vqLm2Zt", b"") == PW_ERR_BREACHED {
        fails |= 1 << 11;
    }

    // 9. THE GREEN HALF: strong passwords are ACCEPTED, including a long
    //    passphrase with no digits or symbols, which a composition rule would
    //    have refused.
    if pw_check(b"Kx7#vqLm2Zt", b"") != PW_OK {
        fails |= 1 << 12;
    }
    if pw_check(b"correct horse battery staple", b"") != PW_OK {
        fails |= 1 << 13;
    }

    // 10. Variety, runs, and the username rule.
    if pw_check(b"aaaaaaaa", b"") != PW_ERR_LOW_VARIETY {
        fails |= 1 << 14;
    }
    if pw_check(b"abababababab", b"") != PW_ERR_LOW_VARIETY {
        fails |= 1 << 15;
    }
    if pw_check(b"12345678", b"") != PW_ERR_SEQUENCE {
        fails |= 1 << 16;
    }
    if pw_check(b"qwertyuiop", b"") != PW_ERR_SEQUENCE {
        fails |= 1 << 17;
    }
    if pw_check(b"lkjhgfdsa", b"") != PW_ERR_SEQUENCE {
        fails |= 1 << 18;
    }
    if pw_check(b"Kx7#alice2Zt", b"alice") != PW_ERR_CONTAINS_USERNAME {
        fails |= 1 << 19;
    }
    // Case-insensitive both ways, and the short-username exemption.
    if pw_check(b"Kx7#ALICE2Zt", b"Alice") != PW_ERR_CONTAINS_USERNAME {
        fails |= 1 << 20;
    }
    if pw_check(b"Kx7#vqLm2Zt", b"al") != PW_OK {
        fails |= 1 << 21;
    }
    // The same strong password IS accepted for an unrelated username, proving
    // rule 5 tests the username and does not just always fire.
    if pw_check(b"Kx7#vqLm2Zt", b"alice") != PW_OK {
        fails |= 1 << 22;
    }

    fails
}
