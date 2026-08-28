// rustkern/dosovl.rs - #rawrite: THE PER-USER WRITE OVERLAY FOR A DOS GAME
// DIRECTORY. Pure path policy: given a program directory (the read-only,
// shared install) and a per-user overlay directory, decide which of the two a
// native path belongs to.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A PERMISSION ROW
// ---------------------------------------------------
// dos/dospath.c has carried this exact unsolved problem in a comment since
// #736 Stage 2:
//
//     WHAT THIS DOES NOT SOLVE, stated because it is the interesting half:
//     Keen 5 writes SAVEGAM0.CK5 next to KEEN5E.EXE, in /DOS/KEEN5, and it
//     always will, because that is what the game does. Under a non-root
//     desktop that save is still denied. Fixing THAT needs guest writes into a
//     program directory to be redirected into a per-user overlay, which is a
//     design (where does the redirect live, how does a read find the overlaid
//     file, what happens on uninstall) and not a permission tweak. It is not
//     built, and this comment is the place the next person will look.
//
// This is that design. Red Alert is the case that forced it, because unlike
// the two games that were worked around instead, it has no way to be pointed
// somewhere else:
//
//   NetHack (#221b)  has DEFAULTS.NH, a config file naming savedir/scoredir,
//                    so its state could be aimed at %HOME%/GAMES/NETHACK.
//   SimCity (#234f)  has a load/save requester that takes a TYPED path, so a
//                    player can name C:\GAMES\SIMCITY once per session.
//   Red Alert        has NEITHER. Every mutable file is a bare relative name.
//
// AND IT IGNORES THE LAUNCH CWD, WHICH IS THE MEASUREMENT THAT KILLED THE
// CHEAPER DESIGN. The obvious fix is to start the guest with its current
// directory set to a writable per-user directory and let reads fall through to
// the install. Measured under a DOSBox-X reference run (2026-08-27): with the
// program at C:\RA\GAME.EXE and the shell current directory set to C:\SAVE
// before launch, Red Alert wrote its DOS/4G swap file (NWLRBBMQ.SWP) and
// rewrote REDALERT.INI into C:\RA, the EXECUTABLE'S directory, and left
// C:\SAVE completely empty. A launch CWD is therefore not a lever on this
// program at all, and the redirect has to happen where the path is resolved.
//
// THE RULE, and it is deliberately ACCESS-INDEPENDENT
// ---------------------------------------------------
// This module answers only "what is the overlay spelling of this path". The C
// caller (dos/int21svc.c overlay_apply) then picks between the two by
// EXISTENCE, not by whether the operation is a read or a write:
//
//   exists in overlay          -> use the overlay      (the mutable copy wins)
//   else exists in base        -> use the base         (read-through)
//   else                       -> use the overlay      (a create lands here)
//
// One rule covers open, create, findfirst, getattr and chdir with no access
// bit plumbed through eleven call sites, and it is correct because of the
// SEEDING step that goes with it: when the overlay directory is created, every
// SMALL file in the base is copied into it (dos/dosexec.c). So REDALERT.INI
// exists in the overlay from first run and a rewrite of it lands there, while
// REDALERT.MIX (25 MB) exists only in the base and is read through.
//
// HONEST LIMITS, stated because they are the interesting half of this one too:
//   - There is no WHITEOUT. Deleting or renaming a file that exists only in
//     the base resolves to a missing overlay path and fails. No shipped title
//     in this tree does that to its own installed data.
//   - There is no COPY-UP. A program that opens a LARGE base file for
//     read/write and patches it in place resolves to the base and is denied,
//     which is the correct answer for shipped read-only data but is a denial
//     rather than a redirect.
//   - Directory ENUMERATION is merged by the caller (find_step), not here.
//
// WHY RUST: CLAUDE.md's 2026-07-16 rule, and there is no performance reason to
// stay in C. This is bounded string surgery on a guest-supplied path in Ring 0,
// the same CWE-120/CWE-170 territory permpath.rs was moved here for. Every
// buffer is a fixed-size slice indexed through bounds-checked slice operations,
// the source scan is explicitly bounded (a caller's string is not trusted to
// terminate), and an overflow FAILS CLOSED (returns 0, "not overlaid") rather
// than truncating into a shorter path that names a different directory.

// Mirrors DOS_SVC_PATH_MAX in dos/int21svc.h. A path longer than this cannot
// reach this module through dos_svc_resolve() anyway.
const PATH_MAX: usize = 256;
const SLASH: u8 = b'/';

/// Read a NUL-terminated C string into a fixed buffer. Returns the length, or
/// None if it does not terminate within PATH_MAX or is a null pointer.
unsafe fn cstr_in(p: *const u8, buf: &mut [u8; PATH_MAX]) -> Option<usize> {
    if p.is_null() {
        return None;
    }
    let mut n = 0usize;
    while n < PATH_MAX {
        let c = *p.add(n);
        if c == 0 {
            return Some(n);
        }
        buf[n] = c;
        n += 1;
    }
    None // no NUL within bounds: fail closed
}

/// Strip trailing slashes from a length, never below 1 (so "/" stays "/").
fn trimmed(buf: &[u8; PATH_MAX], mut n: usize) -> usize {
    while n > 1 && buf[n - 1] == SLASH {
        n -= 1;
    }
    n
}

/// Is the path equal to the base, or does it lie strictly under it? Returns the
/// byte offset in the path where the tail starts (or the whole length when the
/// path IS the base), or None when it is neither.
///
/// The separator test is what makes this a PATH prefix rather than a STRING
/// prefix: without it /DOS/RANGER would be treated as living under /DOS/RA and
/// one game's writes would be redirected into another game's overlay.
fn under(base: &[u8; PATH_MAX], bn: usize, path: &[u8; PATH_MAX], pn: usize) -> Option<usize> {
    if bn == 0 || pn < bn {
        return None;
    }
    let mut i = 0usize;
    while i < bn {
        if base[i] != path[i] {
            return None;
        }
        i += 1;
    }
    if pn == bn {
        return Some(pn);
    }
    if path[bn] != SLASH {
        return None;
    }
    let mut t = bn;
    while t < pn && path[t] == SLASH {
        t += 1;
    }
    Some(t)
}

/// THE ENTRY POINT. If the path is the base or lies under it, write the overlay
/// spelling (overlay root + the same tail) into out and return 1. Otherwise
/// leave out untouched and return 0.
///
/// Returns 0 on ANY failure (null pointer, unterminated string, result too
/// long). Fail-closed here means "not overlaid", i.e. the caller keeps the
/// original path and the pre-existing permission gate decides, which is exactly
/// the behaviour this subsystem had before the overlay existed.
#[no_mangle]
pub extern "C" fn dosovl_map_rs(
    base: *const u8,
    ovl: *const u8,
    path: *const u8,
    out: *mut u8,
    outsz: i32,
) -> i32 {
    if out.is_null() || outsz <= 1 {
        return 0;
    }
    let mut b = [0u8; PATH_MAX];
    let mut o = [0u8; PATH_MAX];
    let mut p = [0u8; PATH_MAX];
    let (bn, on, pn) = unsafe {
        let bn = match cstr_in(base, &mut b) {
            Some(n) => n,
            None => return 0,
        };
        let on = match cstr_in(ovl, &mut o) {
            Some(n) => n,
            None => return 0,
        };
        let pn = match cstr_in(path, &mut p) {
            Some(n) => n,
            None => return 0,
        };
        (bn, on, pn)
    };
    // An empty base or overlay means "no overlay is configured for this guest".
    if bn == 0 || on == 0 {
        return 0;
    }
    let bn = trimmed(&b, bn);
    let on = trimmed(&o, on);
    let pn = trimmed(&p, pn);
    let tail = match under(&b, bn, &p, pn) {
        Some(t) => t,
        None => return 0,
    };

    // Compose overlay + "/" + tail, bounded by the caller's buffer AND by
    // PATH_MAX, refusing rather than truncating.
    let mut r = [0u8; PATH_MAX];
    let mut n = 0usize;
    while n < on {
        r[n] = o[n];
        n += 1;
    }
    if tail < pn {
        if n + 1 >= PATH_MAX {
            return 0;
        }
        r[n] = SLASH;
        n += 1;
        let mut i = tail;
        while i < pn {
            if n + 1 >= PATH_MAX {
                return 0;
            }
            r[n] = p[i];
            n += 1;
            i += 1;
        }
    }
    if n + 1 > outsz as usize {
        return 0; // caller buffer too small: fail closed
    }
    let mut i = 0usize;
    unsafe {
        while i < n {
            *out.add(i) = r[i];
            i += 1;
        }
        *out.add(n) = 0;
    }
    1
}

/// THE REGISTRY: which installed program directories get a per-user overlay,
/// and what that overlay is called under the user's home.
///
/// PER-TITLE ON PURPOSE, AND THIS IS THE INTERESTING DECISION. The mechanism
/// is general and would work for every DOS title in the tree; the ROLLOUT is
/// not, because two of them already have VERIFIED workarounds that this would
/// silently change underneath them. NetHack's DEFAULTS.NH aims savedir and
/// scoredir at %HOME%/GAMES/NETHACK (#221b) and SimCity is driven by typed
/// C:\GAMES\SIMCITY paths (#234f); turning the overlay on for those two would
/// re-route state that is already landing correctly, on a build nobody had
/// re-verified them on. Adding a title here is one line plus a run of that
/// game, which is the right price. Keen 5, named as the unsolved case in
/// dospath.c's #736 comment, is the obvious next entry.
///
/// The base is matched WHOLE, not as a prefix, because it is compared against
/// the executable's own directory, which is exactly one string.
const TITLES: [(&[u8], &[u8]); 1] = [
    // Command & Conquer: Red Alert. GAME.DAT and REDALERT.MIX stay read-only
    // and shared; REDALERT.INI, the DOS/4G swap file and SAVEGAME.NNN become
    // per-user.
    (b"/DOS/RA", b"GAMES/RA"),
];

/// Look up `appdir` in the registry. Returns 1 and writes the home-relative
/// overlay tail (e.g. "GAMES/RA") into `out`, or 0 when this directory has no
/// overlay, which is every directory not listed above.
#[no_mangle]
pub extern "C" fn dosovl_title_rs(appdir: *const u8, out: *mut u8, outsz: i32) -> i32 {
    if out.is_null() || outsz <= 1 {
        return 0;
    }
    let mut a = [0u8; PATH_MAX];
    let an = unsafe {
        match cstr_in(appdir, &mut a) {
            Some(n) => n,
            None => return 0,
        }
    };
    if an == 0 {
        return 0;
    }
    let an = trimmed(&a, an);
    let mut ti = 0usize;
    while ti < TITLES.len() {
        let (base, tail) = TITLES[ti];
        if base.len() == an {
            let mut i = 0usize;
            let mut same = true;
            while i < an {
                if base[i] != a[i] {
                    same = false;
                    break;
                }
                i += 1;
            }
            if same {
                if tail.len() + 1 > outsz as usize {
                    return 0; // fail closed rather than truncate
                }
                let mut j = 0usize;
                unsafe {
                    while j < tail.len() {
                        *out.add(j) = tail[j];
                        j += 1;
                    }
                    *out.add(j) = 0;
                }
                return 1;
            }
        }
        ti += 1;
    }
    0
}

/// Property test over the rules above, run once at boot. Returns a bitmask of
/// failures; 0 is a pass. A property test rather than a differential because
/// there is no C twin to differ from (drvmap.rs makes the same call).
#[no_mangle]
pub extern "C" fn dosovl_selftest_rs() -> u32 {
    let mut fails = 0u32;
    let mut out = [0u8; PATH_MAX];
    let base = b"/DOS/RA\0";
    let ovl = b"/HOME/ADMIN/GAMES/RA\0";

    fn eq(out: &[u8; PATH_MAX], want: &[u8]) -> bool {
        // want includes its NUL terminator
        let mut i = 0usize;
        while i + 1 < want.len() {
            if out[i] != want[i] {
                return false;
            }
            i += 1;
        }
        out[i] == 0
    }

    // 1. a file under the base maps, tail preserved
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RA/REDALERT.INI\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 1
        || !eq(&out, b"/HOME/ADMIN/GAMES/RA/REDALERT.INI\0") {
        fails |= 1 << 0;
    }
    // 2. the base directory itself maps to the overlay directory
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RA\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 1
        || !eq(&out, b"/HOME/ADMIN/GAMES/RA\0") {
        fails |= 1 << 1;
    }
    // 3. a SIBLING sharing a string prefix must NOT map (the separator test)
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RANGER/X.DAT\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 2;
    }
    // 4. an unrelated path must not map
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/CONFIG/SHADOW\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 3;
    }
    // 5. a deep tail is preserved whole
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RA/SAVE/SAVEGAME.001\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 1
        || !eq(&out, b"/HOME/ADMIN/GAMES/RA/SAVE/SAVEGAME.001\0") {
        fails |= 1 << 4;
    }
    // 6. a trailing slash on the input is tolerated and does not double up
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RA/\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 1
        || !eq(&out, b"/HOME/ADMIN/GAMES/RA\0") {
        fails |= 1 << 5;
    }
    // 7. an undersized caller buffer FAILS CLOSED rather than truncating
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS/RA/REDALERT.INI\0".as_ptr(),
                     out.as_mut_ptr(), 8) != 0 {
        fails |= 1 << 6;
    }
    // 8. an empty overlay means "not configured" and never maps
    if dosovl_map_rs(base.as_ptr(), b"\0".as_ptr(), b"/DOS/RA/X\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 7;
    }
    // 9. a null path is refused rather than dereferenced
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), core::ptr::null(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 8;
    }
    // 10. a path SHORTER than the base cannot be under it
    if dosovl_map_rs(base.as_ptr(), ovl.as_ptr(), b"/DOS\0".as_ptr(),
                     out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 9;
    }
    // 11. the registry resolves the one title it lists
    if dosovl_title_rs(b"/DOS/RA\0".as_ptr(), out.as_mut_ptr(), PATH_MAX as i32) != 1
        || !eq(&out, b"GAMES/RA\0") {
        fails |= 1 << 10;
    }
    // 12. an unlisted directory has no overlay, which is the default for
    //     every DOS title in the tree
    if dosovl_title_rs(b"/DOS/KEEN5\0".as_ptr(), out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 11;
    }
    // 13. the registry match is WHOLE, not a prefix
    if dosovl_title_rs(b"/DOS/RANGER\0".as_ptr(), out.as_mut_ptr(), PATH_MAX as i32) != 0 {
        fails |= 1 << 12;
    }
    fails
}
