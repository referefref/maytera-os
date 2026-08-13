// sshdcfg.rs - sshd listen/auth POLICY and its parsers (#697). Rust per the
// Rust-first rule: this is entirely new kernel code and it is a parser over
// attacker-influenceable-ish on-disk bytes, which is exactly where the bounds
// checking pays.
//
// WHY THIS EXISTS
// ---------------
// The C sshd used to carry a compiled-in credential pair:
//     static char g_user[64]  = "root";
//     static char g_pass[128] = "maytera";
// and kernel/main.c started the listener whenever /CONFIG/SSHHOST.KEY existed.
// Every shipping image had that key, so every shipping image answered on port
// 22 and handed out a uid 0 shell to root/maytera over the network. The
// password WAS checked (a wrong one was refused) and SHADOW was NEVER
// consulted (a real `admin` account with a real hash was refused), which is
// the signature of a second, private authenticator.
//
// The fix is ordered so the CLASS dies, not the instance:
//   1. there is no credential here at all, of any value;
//   2. ABSENT CONFIG MEANS NO LISTENER. The presence of a host key is not
//      consent to listen. Nothing to exploit if nothing is listening, even if
//      a future credential path regresses;
//   3. password auth, when explicitly enabled, goes through the SAME
//      users_authenticate() the local login gate uses (PBKDF2 + lockout);
//   4. an account must be named in `allowusers=` before ANY method can
//      authenticate it, so a global AUTHKEYS file cannot be used to log in as
//      an arbitrary account.
//
// CONFIG FORMAT (/CONFIG/SSHD.CFG), one key=value per line, '#' comments:
//     enable=0|1          REQUIRED for the listener to start. Default 0.
//     password_auth=0|1   Default 0 (pubkey only).
//     port=<1..65535>     Default 22.
//     allowusers=a,b,c    Accounts permitted to log in. Default EMPTY = none.
// The legacy `user=` / `pass=` keys are not merely ignored: their presence
// REJECTS the whole config, so an old-format file left on an upgraded image
// can never grant access.
//
// AUTHKEYS FORMAT (/CONFIG/AUTHKEYS), one entry per line:
//     <64-hex-sha256-of-pubkey-blob>              authorizes any allowed user
//     <username> <64-hex-sha256-of-pubkey-blob>   authorizes ONLY that user
// The second form is new here. The bare form is kept for the keys already
// deployed, and is bounded by allowusers= rather than by nothing.

use core::slice;

pub const SSHD_ALLOWUSERS_MAX: usize = 256;

/// Parse/policy result codes shared with C (see net/ssh/ssh2_server.c).
pub const SSHD_OK: u8 = 0;
/// A legacy `user=`/`pass=`/`password=` key was present: refuse the config.
pub const SSHD_REJECT_LEGACY_CRED: u8 = 1;
/// A value could not be parsed (e.g. `enable=maybe`): refuse the config.
pub const SSHD_REJECT_MALFORMED: u8 = 2;

#[repr(C)]
pub struct SshdCfg {
    pub enable: u8,
    pub password_auth: u8,
    pub reject: u8,
    pub _pad0: u8,
    pub port: u16,
    pub _pad1: u16,
    /// NUL-terminated comma list. EMPTY means no account may authenticate.
    pub allowusers: [u8; SSHD_ALLOWUSERS_MAX],
}

fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0usize;
    let mut b = s.len();
    while a < b && (s[a] == b' ' || s[a] == b'\t' || s[a] == b'\r') {
        a += 1;
    }
    while b > a && (s[b - 1] == b' ' || s[b - 1] == b'\t' || s[b - 1] == b'\r') {
        b -= 1;
    }
    &s[a..b]
}

fn eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for i in 0..a.len() {
        if a[i].to_ascii_lowercase() != b[i].to_ascii_lowercase() {
            return false;
        }
    }
    true
}

/// 1 = true, 0 = false, -1 = not a boolean.
fn parse_bool(v: &[u8]) -> i32 {
    if eq_ci(v, b"1") || eq_ci(v, b"yes") || eq_ci(v, b"on") || eq_ci(v, b"true") {
        1
    } else if eq_ci(v, b"0") || eq_ci(v, b"no") || eq_ci(v, b"off") || eq_ci(v, b"false") {
        0
    } else {
        -1
    }
}

/// Decimal port, or -1 if not a valid 1..=65535.
fn parse_port(v: &[u8]) -> i32 {
    if v.is_empty() || v.len() > 5 {
        return -1;
    }
    let mut n: u32 = 0;
    for &c in v {
        if !c.is_ascii_digit() {
            return -1;
        }
        n = n * 10 + (c - b'0') as u32;
    }
    if n == 0 || n > 65535 {
        -1
    } else {
        n as i32
    }
}

fn cstr_len(p: *const u8, max: usize) -> usize {
    let mut n = 0usize;
    while n < max {
        // SAFETY: caller guarantees `p` points at a NUL-terminated buffer.
        if unsafe { *p.add(n) } == 0 {
            break;
        }
        n += 1;
    }
    n
}

/// Is `name` one of the comma-separated tokens in `list`? Exact, case
/// sensitive (account names are). An empty list matches NOTHING, on purpose.
fn list_contains(list: &[u8], name: &[u8]) -> bool {
    if name.is_empty() {
        return false;
    }
    let mut start = 0usize;
    let mut i = 0usize;
    while i <= list.len() {
        let at_end = i == list.len();
        if at_end || list[i] == b',' {
            let tok = trim(&list[start..i]);
            if !tok.is_empty() && tok == name {
                return true;
            }
            start = i + 1;
        }
        if at_end {
            break;
        }
        i += 1;
    }
    false
}

/// Parse /CONFIG/SSHD.CFG into `out`. `buf` may be NULL with `len` 0, which
/// yields the SAFE DEFAULTS (enable=0: no listener). Returns 0 when the
/// resulting config is usable, or a negative value when the config must be
/// refused; `out.reject` carries the reason either way.
///
/// # Safety
/// `out` must be a valid writable SshdCfg. `buf` must be readable for `len`
/// bytes, or NULL when `len` is 0.
#[no_mangle]
pub unsafe extern "C" fn sshd_cfg_parse_rs(buf: *const u8, len: u32, out: *mut SshdCfg) -> i32 {
    if out.is_null() {
        return -1;
    }
    // Defaults first, so every field is defined even on an early refusal.
    // enable=0 is the whole point: absent config means OFF.
    let cfg = unsafe { &mut *out };
    cfg.enable = 0;
    cfg.password_auth = 0;
    cfg.reject = SSHD_OK;
    cfg._pad0 = 0;
    cfg.port = 22;
    cfg._pad1 = 0;
    cfg.allowusers = [0u8; SSHD_ALLOWUSERS_MAX];

    if buf.is_null() || len == 0 {
        return 0;
    }
    // SAFETY: caller guarantees buf is readable for len bytes.
    let data = unsafe { slice::from_raw_parts(buf, len as usize) };

    let mut start = 0usize;
    let mut i = 0usize;
    while i <= data.len() {
        let at_end = i == data.len();
        if at_end || data[i] == b'\n' {
            let line = trim(&data[start..i]);
            start = i + 1;
            if !line.is_empty() && line[0] != b'#' {
                // split on the FIRST '='
                let mut eq = None;
                for j in 0..line.len() {
                    if line[j] == b'=' {
                        eq = Some(j);
                        break;
                    }
                }
                if let Some(j) = eq {
                    let k = trim(&line[..j]);
                    let v = trim(&line[j + 1..]);
                    if eq_ci(k, b"user") || eq_ci(k, b"pass") || eq_ci(k, b"password") {
                        // A credential in a config file is the same defect as a
                        // credential in the binary. Refuse the whole file.
                        cfg.enable = 0;
                        cfg.password_auth = 0;
                        cfg.reject = SSHD_REJECT_LEGACY_CRED;
                        return -1;
                    } else if eq_ci(k, b"enable") || eq_ci(k, b"enabled") {
                        let b = parse_bool(v);
                        if b < 0 {
                            cfg.enable = 0;
                            cfg.reject = SSHD_REJECT_MALFORMED;
                            return -1;
                        }
                        cfg.enable = b as u8;
                    } else if eq_ci(k, b"password_auth") || eq_ci(k, b"passwordauth") {
                        let b = parse_bool(v);
                        if b < 0 {
                            cfg.enable = 0;
                            cfg.reject = SSHD_REJECT_MALFORMED;
                            return -1;
                        }
                        cfg.password_auth = b as u8;
                    } else if eq_ci(k, b"port") {
                        let p = parse_port(v);
                        if p < 0 {
                            cfg.enable = 0;
                            cfg.reject = SSHD_REJECT_MALFORMED;
                            return -1;
                        }
                        cfg.port = p as u16;
                    } else if eq_ci(k, b"allowusers") {
                        let n = if v.len() < SSHD_ALLOWUSERS_MAX - 1 {
                            v.len()
                        } else {
                            SSHD_ALLOWUSERS_MAX - 1
                        };
                        cfg.allowusers[..n].copy_from_slice(&v[..n]);
                        cfg.allowusers[n] = 0;
                    }
                    // unknown keys are ignored: forward compatibility
                }
            }
        }
        if at_end {
            break;
        }
        i += 1;
    }
    0
}

/// Is `user` permitted to authenticate at all? Requires an explicit
/// `allowusers=` entry: an empty list authorizes NOBODY.
///
/// # Safety
/// `cfg` must be a valid SshdCfg; `user` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn sshd_user_allowed_rs(cfg: *const SshdCfg, user: *const u8) -> i32 {
    if cfg.is_null() || user.is_null() {
        return 0;
    }
    let c = unsafe { &*cfg };
    let ul = cstr_len(user, 64);
    if ul == 0 {
        return 0;
    }
    // SAFETY: bounded by cstr_len above.
    let uname = unsafe { slice::from_raw_parts(user, ul) };
    let al = cstr_len(c.allowusers.as_ptr(), SSHD_ALLOWUSERS_MAX);
    if list_contains(&c.allowusers[..al], uname) {
        1
    } else {
        0
    }
}

fn hex_nib(c: u8) -> i32 {
    match c {
        b'0'..=b'9' => (c - b'0') as i32,
        b'a'..=b'f' => (c - b'a' + 10) as i32,
        b'A'..=b'F' => (c - b'A' + 10) as i32,
        _ => -1,
    }
}

/// Does `tok` (64 hex chars) equal the hex of `fp` (32 raw bytes)?
fn hex_eq_fp(tok: &[u8], fp: &[u8]) -> bool {
    if tok.len() != 64 || fp.len() != 32 {
        return false;
    }
    for i in 0..32 {
        let hi = hex_nib(tok[i * 2]);
        let lo = hex_nib(tok[i * 2 + 1]);
        if hi < 0 || lo < 0 {
            return false;
        }
        if ((hi << 4) | lo) as u8 != fp[i] {
            return false;
        }
    }
    true
}

/// Split a line into (optional user, fingerprint token) on ASCII whitespace.
fn split_authkey_line(line: &[u8]) -> (&[u8], &[u8]) {
    let mut i = 0usize;
    while i < line.len() && line[i] != b' ' && line[i] != b'\t' {
        i += 1;
    }
    if i == line.len() {
        return (&line[..0], line); // bare fingerprint
    }
    let first = &line[..i];
    let rest = trim(&line[i..]);
    // second field ends at the next whitespace (allow trailing comments)
    let mut j = 0usize;
    while j < rest.len() && rest[j] != b' ' && rest[j] != b'\t' {
        j += 1;
    }
    (first, &rest[..j])
}

/// Is the public key with SHA-256 fingerprint `fp` authorized for `user` by
/// the AUTHKEYS file in `buf`? Returns 1 or 0.
///
/// # Safety
/// `buf` readable for `len` bytes (or NULL with len 0); `fp` readable for 32
/// bytes; `user` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn sshd_authkey_match_rs(
    buf: *const u8,
    len: u32,
    fp: *const u8,
    user: *const u8,
) -> i32 {
    if buf.is_null() || len == 0 || fp.is_null() || user.is_null() {
        return 0;
    }
    // SAFETY: contracts documented above.
    let data = unsafe { slice::from_raw_parts(buf, len as usize) };
    let fpb = unsafe { slice::from_raw_parts(fp, 32) };
    let ul = cstr_len(user, 64);
    if ul == 0 {
        return 0;
    }
    let uname = unsafe { slice::from_raw_parts(user, ul) };

    let mut start = 0usize;
    let mut i = 0usize;
    while i <= data.len() {
        let at_end = i == data.len();
        if at_end || data[i] == b'\n' {
            let line = trim(&data[start..i]);
            start = i + 1;
            if !line.is_empty() && line[0] != b'#' {
                let (who, tok) = split_authkey_line(line);
                if hex_eq_fp(tok, fpb) && (who.is_empty() || who == uname) {
                    return 1;
                }
            }
        }
        if at_end {
            break;
        }
        i += 1;
    }
    0
}

/// Append `b` to `out[..len]`, returning the new length, or `usize::MAX` if it
/// would not fit. One place decides "does not fit", so no caller can forget.
fn put(out: &mut [u8], len: usize, b: &[u8]) -> usize {
    if len == usize::MAX || len + b.len() > out.len() {
        return usize::MAX;
    }
    out[len..len + b.len()].copy_from_slice(b);
    len + b.len()
}

fn put_u16(out: &mut [u8], len: usize, mut v: u16) -> usize {
    let mut d = [0u8; 5];
    let mut n = 0usize;
    if v == 0 {
        d[0] = b'0';
        n = 1;
    } else {
        while v > 0 {
            d[n] = b'0' + (v % 10) as u8;
            v /= 10;
            n += 1;
        }
        // written least-significant first, so reverse in place
        let mut a = 0usize;
        let mut b = n - 1;
        while a < b {
            d.swap(a, b);
            a += 1;
            b -= 1;
        }
    }
    put(out, len, &d[..n])
}

/// Render `cfg` back to the canonical /CONFIG/SSHD.CFG text.
///
/// WHY THIS EXISTS (#785). Enabling or disabling the service at runtime has to
/// persist, and the only honest way to persist it is to rewrite the file the
/// parser reads, so there is exactly ONE source of truth for "is sshd on".
/// Writing that text from C would have meant a second, hand-rolled
/// understanding of the format sitting next to the Rust parser, and two
/// implementations of one format is how #697 happened in the first place. The
/// round-trip is self-tested below: render(parse(x)) must parse back equal.
///
/// A rendered file NEVER contains a credential. There is no key in `SshdCfg`
/// that could carry one, and the legacy `user=`/`pass=` keys are rejected by
/// the parser rather than stored, so a render can not reintroduce them.
///
/// Returns the number of bytes written, or -1 if `out` is too small / null.
///
/// # Safety
/// `cfg` must be a valid SshdCfg. `out` must be writable for `outlen` bytes.
#[no_mangle]
pub unsafe extern "C" fn sshd_cfg_render_rs(cfg: *const SshdCfg, out: *mut u8, outlen: u32) -> i32 {
    if cfg.is_null() || out.is_null() || outlen == 0 {
        return -1;
    }
    let c = unsafe { &*cfg };
    // SAFETY: caller guarantees `out` is writable for `outlen` bytes.
    let o = unsafe { slice::from_raw_parts_mut(out, outlen as usize) };

    let mut n = 0usize;
    n = put(o, n, b"# MayteraOS sshd configuration (/CONFIG/SSHD.CFG)\n");
    n = put(o, n, b"# Written by the OS. There is NO credential in this file:\n");
    n = put(o, n, b"# authentication goes through the same account database and\n");
    n = put(o, n, b"# the same authenticator as the desktop login.\n");
    n = put(o, n, b"enable=");
    n = put(o, n, if c.enable != 0 { b"1\n" } else { b"0\n" });
    n = put(o, n, b"port=");
    n = put_u16(o, n, if c.port == 0 { 22 } else { c.port });
    n = put(o, n, b"\n");
    n = put(o, n, b"password_auth=");
    n = put(o, n, if c.password_auth != 0 { b"1\n" } else { b"0\n" });
    n = put(o, n, b"allowusers=");
    let al = cstr_len(c.allowusers.as_ptr(), SSHD_ALLOWUSERS_MAX);
    n = put(o, n, &c.allowusers[..al]);
    n = put(o, n, b"\n");

    if n == usize::MAX {
        return -1;
    }
    n as i32
}

/// Boot self-test. Proves the policy is LIVE on this exact build rather than
/// merely compiled in (the trap blame.md keeps recording). Returns a bitmask
/// of FAILED checks; 0 means all passed. net/ssh/ssh2_server.c prints it.
///
/// # Safety
/// Operates only on stack buffers it owns.
#[no_mangle]
pub unsafe extern "C" fn sshd_cfg_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut c = SshdCfg {
        enable: 9,
        password_auth: 9,
        reject: 9,
        _pad0: 0,
        port: 0,
        _pad1: 0,
        allowusers: [0u8; SSHD_ALLOWUSERS_MAX],
    };

    // 1. NO CONFIG AT ALL => NOT ENABLED. This is the invariant that makes the
    //    whole bug class unexploitable, so it is check number one.
    if unsafe { sshd_cfg_parse_rs(core::ptr::null(), 0, &mut c) } != 0
        || c.enable != 0
        || c.password_auth != 0
        || c.port != 22
    {
        fails |= 1;
    }

    // 2. An explicit enable=1 turns it on, and password auth stays OFF unless
    //    separately asked for.
    let s2 = b"# comment\nenable=1\nport=2222\nallowusers=admin, bob\n";
    if unsafe { sshd_cfg_parse_rs(s2.as_ptr(), s2.len() as u32, &mut c) } != 0
        || c.enable != 1
        || c.password_auth != 0
        || c.port != 2222
    {
        fails |= 2;
    }
    // 3. allowusers gates accounts: listed yes, unlisted no.
    if unsafe { sshd_user_allowed_rs(&c, b"admin\0".as_ptr()) } != 1
        || unsafe { sshd_user_allowed_rs(&c, b"bob\0".as_ptr()) } != 1
        || unsafe { sshd_user_allowed_rs(&c, b"root\0".as_ptr()) } != 0
    {
        fails |= 4;
    }
    // 4. enable=1 with NO allowusers authorizes nobody.
    let s4 = b"enable=1\n";
    if unsafe { sshd_cfg_parse_rs(s4.as_ptr(), s4.len() as u32, &mut c) } != 0
        || unsafe { sshd_user_allowed_rs(&c, b"root\0".as_ptr()) } != 0
    {
        fails |= 8;
    }
    // 5. A legacy credential config is REFUSED outright, not partially honoured.
    let s5 = b"enable=1\nuser=root\npass=hunter2\n";
    if unsafe { sshd_cfg_parse_rs(s5.as_ptr(), s5.len() as u32, &mut c) } >= 0
        || c.enable != 0
        || c.reject != SSHD_REJECT_LEGACY_CRED
    {
        fails |= 16;
    }
    // 6. A malformed value fails CLOSED (disabled), never open.
    let s6 = b"enable=maybe\n";
    if unsafe { sshd_cfg_parse_rs(s6.as_ptr(), s6.len() as u32, &mut c) } >= 0 || c.enable != 0 {
        fails |= 32;
    }

    // 7. AUTHKEYS: a bare fingerprint matches any user; a user-scoped one
    //    matches only its own user.
    let fp: [u8; 32] = [
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
        0xff, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54,
        0x32, 0x10,
    ];
    let bare = b"00112233445566778899aabbccddeeff0123456789abcdeffedcba9876543210\n";
    if unsafe {
        sshd_authkey_match_rs(bare.as_ptr(), bare.len() as u32, fp.as_ptr(), b"anyone\0".as_ptr())
    } != 1
    {
        fails |= 64;
    }
    let scoped =
        b"# key\nadmin 00112233445566778899aabbccddeeff0123456789abcdeffedcba9876543210\n";
    if unsafe {
        sshd_authkey_match_rs(
            scoped.as_ptr(),
            scoped.len() as u32,
            fp.as_ptr(),
            b"admin\0".as_ptr(),
        )
    } != 1
    {
        fails |= 128;
    }
    if unsafe {
        sshd_authkey_match_rs(
            scoped.as_ptr(),
            scoped.len() as u32,
            fp.as_ptr(),
            b"root\0".as_ptr(),
        )
    } != 0
    {
        fails |= 256;
    }
    // 8. A non-matching fingerprint never authorizes.
    let mut fp2 = fp;
    fp2[31] ^= 0x01;
    if unsafe {
        sshd_authkey_match_rs(bare.as_ptr(), bare.len() as u32, fp2.as_ptr(), b"anyone\0".as_ptr())
    } != 0
    {
        fails |= 512;
    }
    // 9. RENDER ROUND TRIP (#785). Persisting enable=0/1 rewrites this file, so
    //    a render that loses `port` or `allowusers` would silently widen or
    //    narrow access on the next boot. render(parse(x)) must parse back to
    //    the same policy, and the rendered bytes must still be accepted by the
    //    parser (a file we write that we would then REFUSE would fail closed
    //    and take the service down at the next boot).
    let s9 = b"enable=1\nport=2222\npassword_auth=1\nallowusers=admin,bob\n";
    let mut c9 = SshdCfg {
        enable: 0, password_auth: 0, reject: 0, _pad0: 0, port: 0, _pad1: 0,
        allowusers: [0u8; SSHD_ALLOWUSERS_MAX],
    };
    let mut buf = [0u8; 512];
    if unsafe { sshd_cfg_parse_rs(s9.as_ptr(), s9.len() as u32, &mut c9) } != 0 {
        fails |= 1024;
    } else {
        let n = unsafe { sshd_cfg_render_rs(&c9, buf.as_mut_ptr(), buf.len() as u32) };
        if n <= 0 {
            fails |= 1024;
        } else {
            let mut c10 = SshdCfg {
                enable: 9, password_auth: 9, reject: 9, _pad0: 0, port: 0, _pad1: 0,
                allowusers: [0u8; SSHD_ALLOWUSERS_MAX],
            };
            if unsafe { sshd_cfg_parse_rs(buf.as_ptr(), n as u32, &mut c10) } != 0
                || c10.enable != 1
                || c10.password_auth != 1
                || c10.port != 2222
                || unsafe { sshd_user_allowed_rs(&c10, b"admin\0".as_ptr()) } != 1
                || unsafe { sshd_user_allowed_rs(&c10, b"bob\0".as_ptr()) } != 1
                || unsafe { sshd_user_allowed_rs(&c10, b"root\0".as_ptr()) } != 0
            {
                fails |= 1024;
            }
        }
    }

    // 10. A DISABLED render must round trip to disabled. This is the direction
    //     that matters for failing closed: if rendering enable=0 produced a
    //     file that parsed back as enabled, "disable the service" would be a
    //     no-op that reported success.
    let mut c11 = SshdCfg {
        enable: 0, password_auth: 0, reject: 0, _pad0: 0, port: 22, _pad1: 0,
        allowusers: [0u8; SSHD_ALLOWUSERS_MAX],
    };
    let n11 = unsafe { sshd_cfg_render_rs(&c11, buf.as_mut_ptr(), buf.len() as u32) };
    if n11 <= 0 {
        fails |= 2048;
    } else if unsafe { sshd_cfg_parse_rs(buf.as_ptr(), n11 as u32, &mut c11) } != 0
        || c11.enable != 0
    {
        fails |= 2048;
    }

    // 11. A too-small output buffer must FAIL, not truncate. A truncated config
    //     is a different config, and a half-written "enable=1" line that lost
    //     its allowusers= would authorize nobody while claiming to be on.
    let mut tiny = [0u8; 8];
    if unsafe { sshd_cfg_render_rs(&c11, tiny.as_mut_ptr(), tiny.len() as u32) } != -1 {
        fails |= 4096;
    }

    fails
}
