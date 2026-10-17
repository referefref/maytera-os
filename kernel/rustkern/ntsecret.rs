// rustkern/ntsecret.rs - #smbdisk: the ON-DISK form of a saved SMB credential.
//
// WHY THIS EXISTS.
//
// /CONFIG/NETMOUNTS.CFG stores one saved network location per line as
// `label|server|share|user|secret`. Until this change the last field was the
// SHARE PASSWORD IN CLEARTEXT. #peruser2 found it had no permission entry at
// all, so it sat on the world-readable no-entry default and any Ring-3 process
// of any user could read every saved share password; the mode was fixed to
// 0600 and #smbnt then removed cleartext from kernel MEMORY (the connection
// struct and the VFS defaults hold the 16-byte NT hash now). The file itself
// was still cleartext, which is what this closes.
//
// NTLM needs only nt_hash = MD4(UTF-16LE(password)). So the persisted field
// becomes the hash, in a SELF-DESCRIBING form:
//
//     nt$<32 lowercase hex>
//
// Self-describing on purpose. A bare 32-hex blob would be distinguishable from
// a password only by length and character class, which is a guess; `nt$` makes
// an old record and a new record tellable apart BY INSPECTION, by a human
// reading the file and by the migration code.
//
// HONEST LIMIT, stated here so nobody has to rediscover it: the NT hash is
// PASSWORD-EQUIVALENT FOR SMB (pass-the-hash). Stealing this file still lets
// you mount the share. What the change removes is PASSWORD-REUSE blast radius,
// which is the realistic harm, because a share password is very often also the
// user's password somewhere that matters more. /CONFIG/NETMOUNTS.CFG therefore
// STAYS 0600. If you find yourself arguing that a hash makes the file safe to
// world-read, that is the wrong conclusion.
//
// WHY RUST, per the standing directive (CLAUDE.md): this is new kernel code and
// there is no performance reason for C. It is also the right SHAPE for Rust: it
// is the only part of this change that parses ATTACKER-INFLUENCEABLE data (a
// file on disk) into a fixed-size buffer, it is pure, and it needs no kernel
// services at all. The surrounding work (the mount chokepoint, the boot-time
// migration rewrite, the syscall case) stays C because it is entangled with
// existing C: kprintf, fat_read_file/fat_write_file, the file-local
// smb_mount_auth_nt, and the syscall dispatcher switch.
//
// STRICTNESS IS THE POINT. Three rules that each exist to stop a specific
// failure:
//
//   1. THE TAG IS CASE-SENSITIVE and exactly `nt$`. A literal password that
//      happens to begin `NT$` is then not mistaken for a hash.
//   2. THE LENGTH IS EXACT: `nt$` plus exactly 32 hex digits, then end of
//      string. 31 or 33 digits, trailing whitespace, or a trailing `|` all
//      REJECT. A short field silently zero-padded would authenticate with a
//      hash nobody chose.
//   3. TAGGED-BUT-MALFORMED IS NOT CLEARTEXT. nt_secret_is_tagged() exists so
//      the caller can tell "this record means to be a hash and is corrupt"
//      from "this record is a legacy password", and refuse the first LOUDLY
//      instead of trying to authenticate with the literal string `nt$zzz...`.
//      Silently falling back would turn a corrupt file into a mystery auth
//      failure, and it is exactly the kind of fallback that hides a bug.
//
// Reading is bounded: the scan never touches more than NT_SECRET_SCAN_MAX
// bytes even if the caller hands over an unterminated buffer.

#![allow(dead_code)]

/// `nt$` + 32 hex digits.
pub const NT_SECRET_TEXT_LEN: usize = 3 + 32;
/// Bytes a formatted secret needs, including the NUL.
pub const NT_SECRET_BUF_MIN: usize = NT_SECRET_TEXT_LEN + 1;
/// Hard cap on how far any scan here will read.
const NT_SECRET_SCAN_MAX: usize = 64;

#[inline]
fn hex_val(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

#[inline]
fn hex_digit(v: u8) -> u8 {
    const H: &[u8; 16] = b"0123456789abcdef";
    H[(v & 0x0F) as usize]
}

/// Length of a NUL-terminated C string, capped at NT_SECRET_SCAN_MAX.
/// Returns None if no NUL appears within the cap (a caller bug or a corrupt
/// field; either way we refuse to read further).
unsafe fn cstr_len_bounded(s: *const u8) -> Option<usize> {
    let mut i = 0usize;
    while i < NT_SECRET_SCAN_MAX {
        if *s.add(i) == 0 {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// 1 if `s` starts with the exact tag `nt$`, else 0. Says nothing about whether
/// the rest is well formed: that is nt_secret_parse's job. The caller uses the
/// two together so that a corrupt tagged record is refused rather than treated
/// as a password.
#[no_mangle]
pub unsafe extern "C" fn nt_secret_is_tagged(s: *const u8) -> i32 {
    if s.is_null() {
        return 0;
    }
    if *s.add(0) == b'n' && *s.add(1) == b't' && *s.add(2) == b'$' {
        1
    } else {
        0
    }
}

/// Parse `nt$<32 hex>` into 16 bytes. Returns 1 on success, 0 on any deviation.
/// `out16` is written ONLY on success, so a rejected parse cannot leave a
/// half-filled hash behind for a caller that ignored the return value.
#[no_mangle]
pub unsafe extern "C" fn nt_secret_parse(s: *const u8, out16: *mut u8) -> i32 {
    if s.is_null() || out16.is_null() {
        return 0;
    }
    if nt_secret_is_tagged(s) == 0 {
        return 0;
    }
    let len = match cstr_len_bounded(s) {
        Some(n) => n,
        None => return 0,
    };
    // Rule 2: exact length. No trailing anything.
    if len != NT_SECRET_TEXT_LEN {
        return 0;
    }
    let mut tmp = [0u8; 16];
    let mut i = 0usize;
    while i < 16 {
        let hi = match hex_val(*s.add(3 + i * 2)) {
            Some(v) => v,
            None => return 0,
        };
        let lo = match hex_val(*s.add(3 + i * 2 + 1)) {
            Some(v) => v,
            None => return 0,
        };
        tmp[i] = (hi << 4) | lo;
        i += 1;
    }
    let mut i = 0usize;
    while i < 16 {
        *out16.add(i) = tmp[i];
        i += 1;
    }
    1
}

/// Format 16 bytes as `nt$<32 lowercase hex>` plus a NUL. Returns 1 on success,
/// 0 if the buffer is too small (nothing is written in that case, so a caller
/// that ignores the return value cannot persist a truncated secret).
#[no_mangle]
pub unsafe extern "C" fn nt_secret_format(h: *const u8, out: *mut u8, cap: usize) -> i32 {
    if h.is_null() || out.is_null() || cap < NT_SECRET_BUF_MIN {
        return 0;
    }
    *out.add(0) = b'n';
    *out.add(1) = b't';
    *out.add(2) = b'$';
    let mut i = 0usize;
    while i < 16 {
        let b = *h.add(i);
        *out.add(3 + i * 2) = hex_digit(b >> 4);
        *out.add(3 + i * 2 + 1) = hex_digit(b & 0x0F);
        i += 1;
    }
    *out.add(NT_SECRET_TEXT_LEN) = 0;
    1
}

// ---------------------------------------------------------------------------
// Boot self-test.
//
// It runs on EVERY boot including the golden, for the reason blame.md keeps
// recording: a check that only runs when someone remembers to run it is
// indistinguishable from no check. crypto/run-all-kats.sh cannot host this one
// (its oracles compile crypto/*.c with the host cc; this is Rust in the kernel
// crate), so the wiring that makes it un-rottable is the boot path instead.
//
// The round-trip vector is the MS-NLMP 4.2.4.1 NTOWFv1 of "Password",
// a4f49c406510bdcab6824ee7c30fd852, the same published value
// crypto/ntlm_nthash_kat.sh pins the derivation against. Using the same vector
// on purpose: if the derivation and the on-disk encoding ever disagree about
// byte order, these two tests disagree with each other.
//
// Returns 0 for PASS, or a small non-zero case number so a failure says WHICH
// rule broke rather than just "failed".
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn nt_secret_selftest_rs() -> i32 {
    const KAT_HASH: [u8; 16] = [
        0xa4, 0xf4, 0x9c, 0x40, 0x65, 0x10, 0xbd, 0xca,
        0xb6, 0x82, 0x4e, 0xe7, 0xc3, 0x0f, 0xd8, 0x52,
    ];
    const KAT_TEXT: &[u8; 36] = b"nt$a4f49c406510bdcab6824ee7c30fd852\0";

    // 1. format produces exactly the published hex, lowercase, NUL-terminated.
    let mut buf = [0u8; 48];
    if nt_secret_format(KAT_HASH.as_ptr(), buf.as_mut_ptr(), buf.len()) != 1 {
        return 1;
    }
    let mut i = 0usize;
    while i < 36 {
        if buf[i] != KAT_TEXT[i] {
            return 2;
        }
        i += 1;
    }

    // 2. parse of that text reproduces the hash byte for byte.
    let mut got = [0xEEu8; 16];
    if nt_secret_parse(KAT_TEXT.as_ptr(), got.as_mut_ptr()) != 1 {
        return 3;
    }
    let mut i = 0usize;
    while i < 16 {
        if got[i] != KAT_HASH[i] {
            return 4;
        }
        i += 1;
    }

    // 3. UPPERCASE hex parses (a hand-edited file must still work) ...
    if nt_secret_parse(b"nt$A4F49C406510BDCAB6824EE7C30FD852\0".as_ptr(),
                       got.as_mut_ptr()) != 1 {
        return 5;
    }
    if got[0] != 0xa4 || got[15] != 0x52 {
        return 6;
    }

    // 4. ... but the TAG is case-sensitive, so a password cannot be mistaken
    //    for a hash by capitalisation.
    if nt_secret_parse(b"NT$a4f49c406510bdcab6824ee7c30fd852\0".as_ptr(),
                       got.as_mut_ptr()) != 0 {
        return 7;
    }

    // 5. REJECTIONS. Each of these would be an authentication with a hash
    //    nobody chose if it were accepted.
    let bad: [&[u8]; 7] = [
        b"nt$a4f49c406510bdcab6824ee7c30fd85\0",     // 31 digits
        b"nt$a4f49c406510bdcab6824ee7c30fd8522\0",   // 33 digits
        b"nt$a4f49c406510bdcab6824ee7c30fd852 \0",   // trailing space
        b"nt$a4f49c406510bdcab6824ee7c30fd852|\0",   // trailing separator
        b"nt$a4f49c406510bdcab6824ee7c30fd85z\0",    // non-hex digit
        b"nt$\0",                                    // tag only
        b"hunter2\0",                                // a legacy cleartext password
    ];
    let mut k = 0usize;
    while k < bad.len() {
        if nt_secret_parse(bad[k].as_ptr(), got.as_mut_ptr()) != 0 {
            return 8 + k as i32;
        }
        k += 1;
    }

    // 6. A rejected parse must not have touched the output buffer.
    let mut untouched = [0x5Au8; 16];
    if nt_secret_parse(b"nt$zz\0".as_ptr(), untouched.as_mut_ptr()) != 0 {
        return 20;
    }
    let mut i = 0usize;
    while i < 16 {
        if untouched[i] != 0x5A {
            return 21;
        }
        i += 1;
    }

    // 7. is_tagged separates "means to be a hash" from "is a password", which
    //    is what lets the caller refuse a corrupt record loudly.
    if nt_secret_is_tagged(b"nt$garbage\0".as_ptr()) != 1 {
        return 22;
    }
    if nt_secret_is_tagged(b"hunter2\0".as_ptr()) != 0 {
        return 23;
    }

    // 8. A too-small output buffer must refuse rather than truncate.
    let mut small = [0u8; NT_SECRET_TEXT_LEN]; // one short of the NUL
    if nt_secret_format(KAT_HASH.as_ptr(), small.as_mut_ptr(), small.len()) != 0 {
        return 24;
    }

    0
}
