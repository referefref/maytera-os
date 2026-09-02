// rustkern/dospolicy.rs - #DOSROUTE: WHICH DOS interpreter a launch goes to.
//
// WHY THIS EXISTS
// ---------------
// MayteraOS has TWO DOS interpreters running the SAME sources: the in-kernel
// Ring-0 one (dos/dosexec.c on a kernel thread) and the Ring-3 host
// (/APPS/DOSUSER, which compiles those same sources into a user process).
// Until now nothing chose between them at a normal launch: the Start menu, the
// shell, the Files association and the AI launcher all reach SYS_DOS_RUN, and
// SYS_DOS_RUN went straight to the in-kernel path. The only way to reach Ring 3
// was /CONFIG/DOSRING3.CFG, a boot-time harness for ONE named guest that exists
// to run a differential, not to route anything.
//
// That gap blocks SMP. MEASURED (#67/#168): with a DOS guest in-kernel the Big
// Kernel Lock is held 93.2% of wall clock and the Amdahl ceiling is 1.05x;
// turning SMP on with the guest still in-kernel makes every axis WORSE (97.3%
// BKL, 3.2x host CPU, compositor flips 22.9/s -> 3.1/s, 9.2e7 pause-spins/s).
// With the guest in Ring 3 the BKL falls to 3.7% and the ceiling rises to 3.59x.
// So routing has to exist before SMP can be turned on.
//
// WHAT THIS MODULE IS
// -------------------
// The PURE part of that decision, and only that: parse /CONFIG/DOSROUTE.CFG,
// and answer "kernel or Ring 3?" for one program path. It performs no I/O, logs
// nothing, spawns nothing and holds no lock. The C glue (proc/dosroute.c) reads
// the file, prints the decision and does the spawn, because that half is three
// calls to existing C APIs and has no logic worth isolating.
//
// WHY RUST: the standing rule (CLAUDE.md) is that new kernel code is Rust
// unless there is a stated performance reason. There is none - this runs once
// per DOS launch, a human-scale event. And the work is exactly the shape that
// keeps producing C bugs in this tree: bounded copies of untrusted file bytes
// into fixed buffers, case folding, and a substring/suffix match. Every copy
// below is a bounds-checked slice write that NUL-terminates by construction.
//
// CONCURRENCY: no statics, no interior mutability, no allocation, no waiting.
// The caller owns the DosPolicy and the caller owns its lifetime. There is
// nothing here for #426 to be about.
//
// THE FILE FORMAT
// ---------------
//     # comments and blank lines are ignored
//     default=kernel          # or ring3. ABSENT FILE == kernel.
//     kernel=BATS.EXE         # force this guest to the in-kernel path
//     ring3=/DOS/ROGUE/ROGUE.EXE
//
// A pattern CONTAINING a '/' is matched against the whole program path; one
// WITHOUT is matched against the basename. Matching is case-insensitive and
// '\\' folds to '/', because DOS paths reach us in both cases and from both
// FAT (upper 8.3) and ext2 (whatever was written).
//
// LAST MATCHING RULE WINS. Deliberate, and the opposite of first-match: a
// config file is edited by APPENDING, and a user who adds a line at the bottom
// to change their mind should not have to find and delete the earlier one.
//
// AN OVER-LONG PATTERN IS REFUSED, NOT TRUNCATED. A truncated pattern matches
// MORE guests than the author wrote, which for a rule whose whole job is "send
// exactly this one title elsewhere" is a silent widening. Refusing is counted
// in `n_bad` so the C side can say the file has a bad line in it.

#![allow(dead_code)]

/// Route to the in-kernel Ring-0 interpreter. THE SHIPPING DEFAULT.
pub const DOSROUTE_KERNEL: i32 = 0;
/// Route to the Ring-3 host, /APPS/DOSUSER.
pub const DOSROUTE_RING3: i32 = 1;

/// Most per-guest override rules honoured. A tree with more than 32 DOS titles
/// needing individual routing has a systemic problem the 33rd line would not
/// fix.
pub const DOSPOL_MAX_RULES: usize = 32;
/// Longest override pattern. The longest path in the shipped corpus is
/// /WINDIR/DRIVE_E/DWB.EXE at 23 bytes.
pub const DOSPOL_PAT_CAP: usize = 64;
/// Longest program path this matcher will consider. Matches the kernel's own
/// g_dos_path.
pub const DOSPOL_PROG_CAP: usize = 256;

/// Mirrored in kernel/proc/dosroute.h and locked there with _Static_assert on
/// sizeof and on the two array bounds.
#[repr(C)]
pub struct DosPolicy {
    /// 1 once dospolicy_parse_rs() has run over a buffer, 0 for the
    /// no-config-file state. A zeroed struct is a VALID "everything in-kernel"
    /// policy, which is what makes the absent-file case need no special case.
    pub valid: i32,
    /// The `default=` line. 0 = kernel, 1 = ring3.
    pub default_ring3: i32,
    /// Number of override rules parsed into `mode`/`pat`.
    pub n: i32,
    /// Lines that were neither blank, a comment, nor a rule this version
    /// understands, plus rules refused for being too long. Reported once by the
    /// C caller: a config file with a typo in it must not fail silently, which
    /// is how a deny-list stops denying.
    pub n_bad: i32,
    /// Per-rule route, DOSROUTE_KERNEL or DOSROUTE_RING3.
    pub mode: [i32; DOSPOL_MAX_RULES],
    /// Per-rule pattern, upper-cased, '/'-normalised, NUL-terminated.
    pub pat: [[u8; DOSPOL_PAT_CAP]; DOSPOL_MAX_RULES],
}

#[inline]
fn fold(c: u8) -> u8 {
    let c = if c == b'\\' { b'/' } else { c };
    if c >= b'a' && c <= b'z' { c - 32 } else { c }
}

/// Copy `src` into `dst` folded, refusing rather than truncating. Returns the
/// length written, or None if it would not fit with its NUL.
fn fold_into(dst: &mut [u8], src: &[u8]) -> Option<usize> {
    if src.len() + 1 > dst.len() {
        return None;
    }
    for (i, &b) in src.iter().enumerate() {
        dst[i] = fold(b);
    }
    dst[src.len()] = 0;
    Some(src.len())
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
        if fold(a[i]) != fold(b[i]) {
            return false;
        }
    }
    true
}

/// Parse one `key=value` line. Returns (key, value) trimmed, or None.
fn split_kv(line: &[u8]) -> Option<(&[u8], &[u8])> {
    let mut eq = None;
    for i in 0..line.len() {
        if line[i] == b'=' {
            eq = Some(i);
            break;
        }
    }
    let e = eq?;
    Some((trim(&line[..e]), trim(&line[e + 1..])))
}

/// Length of a NUL-terminated C string, scanned no further than `max`.
/// Returns `max` if no NUL is found, so a corrupt pointer cannot walk memory.
unsafe fn cstr_len(p: *const u8, max: usize) -> usize {
    let mut n = 0usize;
    while n < max {
        if unsafe { *p.add(n) } == 0 {
            break;
        }
        n += 1;
    }
    n
}

// ---------------------------------------------------------------------------
// Pure core, testable without any pointer at all. Kept separate from the FFI
// wrappers so the self-test exercises THE SAME CODE the kernel runs, not a
// re-implementation of it.
// ---------------------------------------------------------------------------

fn parse_core(buf: &[u8], out: &mut DosPolicy) {
    out.valid = 1;
    out.default_ring3 = 0;
    out.n = 0;
    out.n_bad = 0;

    let mut i = 0usize;
    while i < buf.len() {
        // One line, LF- or CRLF-terminated; the trailing CR is eaten by trim().
        let start = i;
        while i < buf.len() && buf[i] != b'\n' {
            i += 1;
        }
        let line = trim(&buf[start..i]);
        if i < buf.len() {
            i += 1;
        }

        if line.is_empty() || line[0] == b'#' || line[0] == b';' {
            continue;
        }
        let (k, v) = match split_kv(line) {
            Some(kv) => kv,
            None => {
                out.n_bad += 1;
                continue;
            }
        };
        if v.is_empty() {
            out.n_bad += 1;
            continue;
        }

        if eq_ci(k, b"default") {
            if eq_ci(v, b"ring3") {
                out.default_ring3 = 1;
            } else if eq_ci(v, b"kernel") {
                out.default_ring3 = 0;
            } else {
                // An unrecognised value FAILS SAFE to the in-kernel path rather
                // than to the newer one. A typo must never silently move every
                // DOS guest in the machine onto the path that is still opt-in.
                out.default_ring3 = 0;
                out.n_bad += 1;
            }
            continue;
        }

        let mode = if eq_ci(k, b"kernel") {
            DOSROUTE_KERNEL
        } else if eq_ci(k, b"ring3") {
            DOSROUTE_RING3
        } else {
            out.n_bad += 1;
            continue;
        };

        if (out.n as usize) >= DOSPOL_MAX_RULES {
            out.n_bad += 1;
            continue;
        }
        let idx = out.n as usize;
        match fold_into(&mut out.pat[idx], v) {
            Some(_) => {
                out.mode[idx] = mode;
                out.n += 1;
            }
            // Too long: refused, never truncated. See the header.
            None => out.n_bad += 1,
        }
    }
}

/// Does `pat` (already folded) select `prog` (already folded)?
fn rule_matches(pat: &[u8], prog: &[u8]) -> bool {
    let has_slash = pat.iter().any(|&c| c == b'/');
    if has_slash {
        return pat == prog;
    }
    // Basename form.
    let mut base = 0usize;
    for i in 0..prog.len() {
        if prog[i] == b'/' {
            base = i + 1;
        }
    }
    &prog[base..] == pat
}

/// Returns (route, rule_index). rule_index is -1 when the default decided.
fn route_core(pol: &DosPolicy, prog: &[u8]) -> (i32, i32) {
    let dflt = if pol.default_ring3 != 0 {
        DOSROUTE_RING3
    } else {
        DOSROUTE_KERNEL
    };
    if pol.valid == 0 || prog.is_empty() {
        return (dflt, -1);
    }
    let mut folded = [0u8; DOSPOL_PROG_CAP];
    let n = core::cmp::min(prog.len(), DOSPOL_PROG_CAP);
    for i in 0..n {
        folded[i] = fold(prog[i]);
    }
    let fp = &folded[..n];

    // LAST match wins: walk forwards and keep overwriting.
    let mut hit = -1i32;
    let cnt = core::cmp::min(pol.n as usize, DOSPOL_MAX_RULES);
    for r in 0..cnt {
        let pl = {
            let mut l = 0usize;
            while l < DOSPOL_PAT_CAP && pol.pat[r][l] != 0 {
                l += 1;
            }
            l
        };
        if pl == 0 {
            continue;
        }
        if rule_matches(&pol.pat[r][..pl], fp) {
            hit = r as i32;
        }
    }
    if hit >= 0 {
        (pol.mode[hit as usize], hit)
    } else {
        (dflt, -1)
    }
}

// ---------------------------------------------------------------------------
// FFI
// ---------------------------------------------------------------------------

/// Parse a whole /CONFIG/DOSROUTE.CFG image into `out`. Returns 0, or -1 on a
/// null argument. There is no "bad file" return: a file with unusable lines
/// still yields a usable policy (they are counted in n_bad), because refusing
/// the whole file over one typo would silently revert every override in it.
#[no_mangle]
pub unsafe extern "C" fn dospolicy_parse_rs(buf: *const u8, len: u32, out: *mut DosPolicy) -> i32 {
    if out.is_null() {
        return -1;
    }
    let o = unsafe { &mut *out };
    // Defaults first, so every field is defined even on an early refusal.
    o.valid = 0;
    o.default_ring3 = 0;
    o.n = 0;
    o.n_bad = 0;
    if buf.is_null() || len == 0 {
        return -1;
    }
    // SAFETY: caller guarantees `buf` is readable for `len` bytes.
    let s = unsafe { core::slice::from_raw_parts(buf, len as usize) };
    parse_core(s, o);
    0
}

/// Decide where the guest at `prog` (a NUL-terminated program path, no command
/// tail) should run. Returns DOSROUTE_KERNEL or DOSROUTE_RING3. If `out_rule`
/// is non-null it receives the index of the override rule that decided, or -1
/// when the default did - so the caller can name the exact rule on serial
/// rather than printing a verdict with no provenance.
#[no_mangle]
pub unsafe extern "C" fn dospolicy_route_rs(
    pol: *const DosPolicy,
    prog: *const u8,
    out_rule: *mut i32,
) -> i32 {
    if !out_rule.is_null() {
        unsafe { *out_rule = -1 };
    }
    if pol.is_null() {
        return DOSROUTE_KERNEL;
    }
    let p = unsafe { &*pol };
    if prog.is_null() {
        return if p.default_ring3 != 0 { DOSROUTE_RING3 } else { DOSROUTE_KERNEL };
    }
    let n = unsafe { cstr_len(prog, DOSPOL_PROG_CAP) };
    let s = unsafe { core::slice::from_raw_parts(prog, n) };
    let (route, rule) = route_core(p, s);
    if !out_rule.is_null() {
        unsafe { *out_rule = rule };
    }
    route
}

/// Copy rule `idx`'s pattern out as a C string, for the log line. Returns the
/// length written, or -1.
#[no_mangle]
pub unsafe extern "C" fn dospolicy_rule_text_rs(
    pol: *const DosPolicy,
    idx: i32,
    out: *mut u8,
    outlen: u32,
) -> i32 {
    if pol.is_null() || out.is_null() || outlen == 0 || idx < 0 {
        return -1;
    }
    let p = unsafe { &*pol };
    if idx >= p.n || (idx as usize) >= DOSPOL_MAX_RULES {
        return -1;
    }
    let src = &p.pat[idx as usize];
    let mut l = 0usize;
    while l < DOSPOL_PAT_CAP && src[l] != 0 {
        l += 1;
    }
    if l + 1 > outlen as usize {
        l = outlen as usize - 1;
    }
    for i in 0..l {
        unsafe { *out.add(i) = src[i] };
    }
    unsafe { *out.add(l) = 0 };
    l as i32
}

// ---------------------------------------------------------------------------
// SELF-TEST
//
// It has to be able to go RED. Half of these vectors assert the WRONG answer is
// not produced (a `kernel=` rule must not send a guest to Ring 3, an unknown
// `default=` value must not open the gate), because a self-test that only ever
// checks the happy path is the shape this tree keeps getting burned by. Each
// failure increments the return, so a non-zero result names how many.
// ---------------------------------------------------------------------------

fn blank() -> DosPolicy {
    DosPolicy {
        valid: 0,
        default_ring3: 0,
        n: 0,
        n_bad: 0,
        mode: [0; DOSPOL_MAX_RULES],
        pat: [[0u8; DOSPOL_PAT_CAP]; DOSPOL_MAX_RULES],
    }
}

fn chk(cond: bool, fails: &mut u32) {
    if !cond {
        *fails += 1;
    }
}

#[no_mangle]
pub extern "C" fn dospolicy_selftest_rs() -> u32 {
    let mut f: u32 = 0;

    // 1. A ZEROED policy is "everything in-kernel". This is the absent-file
    //    state and it is the shipping default, so it is checked first.
    {
        let p = blank();
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_KERNEL, &mut f);
        chk(route_core(&p, b"").0 == DOSROUTE_KERNEL, &mut f);
    }

    // 2. default=kernel with a per-guest ring3 override. This is the shape the
    //    switch ships in: OFF globally, one title opted IN for testing.
    {
        let mut p = blank();
        parse_core(b"default=kernel\nring3=/DOS/ROGUE/ROGUE.EXE\n", &mut p);
        chk(p.n == 1 && p.n_bad == 0 && p.default_ring3 == 0, &mut f);
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_RING3, &mut f);
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").1 == 0, &mut f);
        // NEGATIVE: a different guest must NOT be dragged along.
        chk(route_core(&p, b"/DOS/TIM/TIM.EXE").0 == DOSROUTE_KERNEL, &mut f);
        chk(route_core(&p, b"/DOS/TIM/TIM.EXE").1 == -1, &mut f);
    }

    // 3. default=ring3 with a per-guest kernel override - the deny-list shape
    //    the switch would ship in AFTER the owner flips it. BATS is the real
    //    case: its music is OPL2 FM, and FM is not wired in Ring 3, so it must
    //    be sent back or it plays silence.
    {
        let mut p = blank();
        parse_core(b"default=ring3\nkernel=BATS.EXE\n", &mut p);
        chk(p.default_ring3 == 1 && p.n == 1, &mut f);
        chk(route_core(&p, b"/GAMES/BATS/BATS.EXE").0 == DOSROUTE_KERNEL, &mut f);
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_RING3, &mut f);
    }

    // 4. Case folding and backslashes, both directions.
    {
        let mut p = blank();
        parse_core(b"default=kernel\nring3=\\dos\\rogue\\rogue.exe\n", &mut p);
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_RING3, &mut f);
    }

    // 5. Basename form matches any directory; full-path form matches only one.
    {
        let mut p = blank();
        parse_core(b"default=kernel\nring3=ROGUE.EXE\n", &mut p);
        chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_RING3, &mut f);
        chk(route_core(&p, b"/OTHER/ROGUE.EXE").0 == DOSROUTE_RING3, &mut f);

        let mut q = blank();
        parse_core(b"default=kernel\nring3=/DOS/ROGUE/ROGUE.EXE\n", &mut q);
        chk(route_core(&q, b"/OTHER/ROGUE.EXE").0 == DOSROUTE_KERNEL, &mut f);
        // NEGATIVE: a full-path pattern must not match a mere suffix.
        chk(route_core(&q, b"/X/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_KERNEL, &mut f);
        // NEGATIVE: nor a prefix.
        chk(route_core(&q, b"/DOS/ROGUE/ROGUE.EXEX").0 == DOSROUTE_KERNEL, &mut f);
    }

    // 6. LAST match wins.
    {
        let mut p = blank();
        parse_core(b"default=kernel\nring3=TIM.EXE\nkernel=TIM.EXE\n", &mut p);
        chk(p.n == 2, &mut f);
        chk(route_core(&p, b"/DOS/TIM/TIM.EXE").0 == DOSROUTE_KERNEL, &mut f);
        chk(route_core(&p, b"/DOS/TIM/TIM.EXE").1 == 1, &mut f);
    }

    // 7. FAIL-SAFE: an unrecognised default= value must land on KERNEL, and be
    //    counted as bad. This is the arm that stops a typo moving the whole
    //    machine onto the opt-in path.
    {
        let mut p = blank();
        parse_core(b"default=RING-3\n", &mut p);
        chk(p.default_ring3 == 0, &mut f);
        chk(p.n_bad == 1, &mut f);
        chk(route_core(&p, b"/DOS/TIM/TIM.EXE").0 == DOSROUTE_KERNEL, &mut f);
    }

    // 8. Comments, blanks, CRLF, and junk counted rather than swallowed.
    {
        let mut p = blank();
        parse_core(b"# c\r\n\r\n; c2\r\ndefault=ring3\r\nnonsense\r\nkernel=\r\n", &mut p);
        chk(p.default_ring3 == 1, &mut f);
        chk(p.n == 0, &mut f);
        chk(p.n_bad == 2, &mut f);   // "nonsense" (no '='), "kernel=" (empty)
    }

    // 9. An over-long pattern is REFUSED, not truncated. The truncation bug
    //    this guards against would turn a one-title rule into a prefix rule.
    {
        let mut p = blank();
        let mut cfg = [0u8; 8 + DOSPOL_PAT_CAP + 8];
        let head = b"ring3=/";
        let mut w = 0usize;
        for &b in head { cfg[w] = b; w += 1; }
        for _ in 0..DOSPOL_PAT_CAP { cfg[w] = b'A'; w += 1; }
        cfg[w] = b'\n'; w += 1;
        parse_core(&cfg[..w], &mut p);
        chk(p.n == 0, &mut f);
        chk(p.n_bad == 1, &mut f);
    }

    // 10. The rule table cannot be overrun by a hostile config.
    {
        let mut p = blank();
        let mut cfg = [0u8; 16 * 64];
        let mut w = 0usize;
        for _ in 0..(DOSPOL_MAX_RULES + 5) {
            for &b in b"ring3=A.EXE\n" { cfg[w] = b; w += 1; }
        }
        parse_core(&cfg[..w], &mut p);
        chk(p.n == DOSPOL_MAX_RULES as i32, &mut f);
        chk(p.n_bad == 5, &mut f);
    }

    f
}

/// Deliberately-broken arm, so the self-test can be PROVEN able to go red.
/// Built only under DOSPOLICY_REDTEST; it asserts the OPPOSITE of vector 3 and
/// must report a non-zero failure count. Without this, "selftest fails=0" is
/// indistinguishable from a self-test whose checks cannot fail.
#[no_mangle]
pub extern "C" fn dospolicy_selftest_red_rs() -> u32 {
    let mut f: u32 = 0;
    let mut p = blank();
    parse_core(b"default=ring3\nkernel=BATS.EXE\n", &mut p);
    // WRONG ON PURPOSE: a kernel= rule sends BATS to the KERNEL, so asserting
    // RING3 here must fail.
    chk(route_core(&p, b"/GAMES/BATS/BATS.EXE").0 == DOSROUTE_RING3, &mut f);
    chk(route_core(&p, b"/DOS/ROGUE/ROGUE.EXE").0 == DOSROUTE_KERNEL, &mut f);
    f
}
