// rustkern/dospath.rs - CANONICALISING "." AND ".." OUT OF A RESOLVED DOS PATH.
//
// WHAT WAS BROKEN, AND HOW IT HID
// -------------------------------
// dos/dospath.c's dos_resolve_path_ex() turns a DOS spec into a native path.
// It maps drive letters, applies the per-drive current directory, flips
// backslashes to slashes, collapses runs of slashes and uppercases the result.
// It did NOT understand the two component names every DOS directory actually
// contains, "." and "..", so it emitted them verbatim:
//
//     cwd E:\DIG , spec ".\*.BUN"  ->  /WINDIR/DRIVE_E/DIG/./*.BUN
//
// int21svc.c's 4Eh findfirst then splits at the last slash and opens the
// directory part, which is "/WINDIR/DRIVE_E/DIG/.". No filesystem in this tree
// resolves a trailing "." component, so the open failed and the search reported
// the DOCUMENTED, CORRECT-LOOKING answer for a wildcard that matches nothing:
// CF=1, AX=0x12 "no more files". A guest cannot tell that apart from an empty
// directory, and neither could the log line, which is why this survived every
// DOS title in the tree until one searched with a "." prefix.
//
// MEASURED, The Dig (2026-08-28). Against a DOSBox-X reference run of the same
// binary the guest issues, in both, `4Eh findfirst ".\*.BUN"`. The reference
// finds DIGMUSIC.BUN and DIGVOICE.BUN, opens both and allocates SIX conventional
// blocks for their caches (0003 006a 002b 0003 13f8 07fe) before the twelve
// allocations that the two runs share. Here the search returned "no more files",
// those six allocations never happened, and the engine's next request was a
// 0x57e5-paragraph block the reference never asks for. See blame.md.
//
// WHY THIS IS A SEPARATE FUNCTION AND NOT MORE C IN dospath.c
// ----------------------------------------------------------
// It is new code, so the tree's rule makes it Rust, and there is no performance
// argument: it runs once per INT 21h path operation on a <=256 byte buffer. It
// is also exactly the shape that goes wrong in C - an in-place backwards scan
// over a caller's buffer with a pop that must not run off the front - so the
// bounds checks are worth having. It does NOT fork the resolver: dospath.c
// still owns every policy decision about drives, CWDs and fallbacks, and calls
// this for the one mechanical step of collapsing dot components.
//
// THE ".." CLAMP IS SECURITY, NOT TIDINESS
// ----------------------------------------
// Before this change ".." simply did not resolve, so no guest could use it to
// leave its drive. Making ".." work restores that lever, and a DOS guest whose
// files live under /WINDIR/DRIVE_X must not be able to spell
// "E:\DIG\..\..\..\CONFIG\KIMI.KEY" and reach the host's /CONFIG. So the pop
// STOPS at the drive root: for a path under /WINDIR/DRIVE_<letter> the floor is
// those two components, otherwise it is the native root.
//
// That is not a bolted-on restriction, it is what DOS does. ".." in the root
// directory of a drive is the root directory of that drive; there is nothing
// above it to name. A guest that walks up too far lands on its own root on real
// DOS and lands on its own root here.

const PATH_MAX: usize = 256;
const SLASH: u8 = b'/';

/// Is component buf[a..b) exactly "."?
fn is_dot(buf: &[u8; PATH_MAX], a: usize, b: usize) -> bool {
    b == a + 1 && buf[a] == b'.'
}

/// Is component buf[a..b) exactly ".."?
fn is_dotdot(buf: &[u8; PATH_MAX], a: usize, b: usize) -> bool {
    b == a + 2 && buf[a] == b'.' && buf[a + 1] == b'.'
}

/// How many leading components of an ALREADY-UPPERCASED native path may never
/// be popped by "..". Two for /WINDIR/DRIVE_X (so a DOS guest cannot climb out
/// of its own drive), zero otherwise.
///
/// Matched on the literal prefix rather than by re-splitting, because this must
/// agree with dospath.c's WINDIR_ROOT "/DRIVE_" composition byte for byte; if
/// the two ever disagree the clamp silently stops clamping.
fn floor_components(buf: &[u8; PATH_MAX], n: usize) -> usize {
    const PFX: &[u8] = b"/WINDIR/DRIVE_";
    if n < PFX.len() + 1 {
        return 0;
    }
    let mut i = 0usize;
    while i < PFX.len() {
        if buf[i] != PFX[i] {
            return 0;
        }
        i += 1;
    }
    let letter = buf[PFX.len()];
    if !(b'A'..=b'Z').contains(&letter) {
        return 0;
    }
    // The letter must be the whole component: /WINDIR/DRIVE_EX is not a drive.
    if n > PFX.len() + 1 && buf[PFX.len() + 1] != SLASH {
        return 0;
    }
    2
}

/// THE ENTRY POINT. Collapse "." and ".." components of the NUL-terminated
/// native path in `path`, in place. Returns 1 if the path was changed, 0 if it
/// was already canonical or could not be read (null, unterminated, too long).
///
/// Expects the input dospath.c produces: '/'-separated, no backslashes, no
/// doubled slashes. It does not require them: the path is rebuilt one component
/// at a time, so a doubled or trailing slash simply does not survive.
///
/// A trailing slash is not preserved. int21svc.c's 4Eh findfirst DID depend on
/// one, and this line used to say nothing did: a search spelled "E:\\" arrived
/// as "/WINDIR/DRIVE_E/", and the handler's split-at-the-last-slash turned the
/// trailing slash into (dir=/WINDIR/DRIVE_E, pat=""). Without it the same search
/// split into (dir=/WINDIR, pat="DRIVE_E") and every volume-label lookup on
/// every drive answered "no such disc" (Red Alert, 2026-08-29). Dropping the
/// slash is still right; the handler now asks whether the path IS a drive root
/// instead of reading punctuation. Do not restore the slash to fix a caller.
/// The general rule stands, and
/// "/DIR/." must become "/DIR", not "/DIR/".
#[no_mangle]
pub unsafe extern "C" fn dospath_canon_rs(path: *mut u8, cap: i32) -> i32 {
    if path.is_null() || cap <= 1 {
        return 0;
    }
    let cap = if (cap as usize) < PATH_MAX { cap as usize } else { PATH_MAX };

    // Read in, bounded. An unterminated or over-long buffer is left alone:
    // failing closed here means "no canonicalisation", i.e. exactly the
    // behaviour this function replaced.
    let mut buf = [0u8; PATH_MAX];
    let mut n = 0usize;
    loop {
        if n >= cap {
            return 0;
        }
        let c = *path.add(n);
        if c == 0 {
            break;
        }
        buf[n] = c;
        n += 1;
    }

    let absolute = n > 0 && buf[0] == SLASH;
    let floor = if absolute { floor_components(&buf, n) } else { 0 };

    // Component start offsets into `out`, so ".." can pop one.
    let mut out = [0u8; PATH_MAX];
    let mut on = 0usize;
    let mut starts = [0u16; PATH_MAX / 2 + 1];
    let mut nc = 0usize;

    let mut i = if absolute { 1 } else { 0 };
    while i < n {
        let a = i;
        while i < n && buf[i] != SLASH {
            i += 1;
        }
        let b = i;
        while i < n && buf[i] == SLASH {
            i += 1;
        }
        if b == a {
            continue; // an empty component (a leading doubled slash)
        }
        if is_dot(&buf, a, b) {
            continue;
        }
        if is_dotdot(&buf, a, b) {
            if nc > floor {
                nc -= 1;
                on = starts[nc] as usize;
            }
            // At or below the floor a ".." is DROPPED, which is what DOS does
            // at a drive root. It is never turned into a literal "..".
            continue;
        }
        if nc >= starts.len() - 1 {
            return 0; // absurd component count: leave the caller's path alone
        }
        starts[nc] = on as u16;
        nc += 1;
        if on > 0 || absolute {
            if on + 1 >= cap {
                return 0;
            }
            out[on] = SLASH;
            on += 1;
        }
        let mut k = a;
        while k < b {
            if on + 1 >= cap {
                return 0;
            }
            out[on] = buf[k];
            on += 1;
            k += 1;
        }
    }

    // Everything collapsed away: an absolute path becomes the root it started
    // from, a relative one becomes empty.
    if on == 0 && absolute {
        if cap < 2 {
            return 0;
        }
        out[0] = SLASH;
        on = 1;
    }

    // DID ANYTHING ACTUALLY CHANGE? Answer by comparing the REBUILT path to the
    // input, never by a flag set at the places that looked like they changed
    // something. The flag version shipped for one build and the boot self-test
    // caught it: a doubled slash is consumed by the scanner's "skip a run of
    // separators" step, so it never reaches the empty-component arm that set
    // the flag, and "/WINDIR/DRIVE_C//DOS" was returned unchanged. Rebuilding
    // and comparing cannot have that class of blind spot.
    if on == n {
        let mut i = 0usize;
        while i < n && out[i] == buf[i] {
            i += 1;
        }
        if i == n {
            return 0;
        }
    }
    let mut w = 0usize;
    while w < on {
        *path.add(w) = out[w];
        w += 1;
    }
    *path.add(on) = 0;
    1
}

// ---------------------------------------------------------------------------
// SELF-TEST
// ---------------------------------------------------------------------------
// Run at DOS-subsystem init so the serial log carries the proof rather than the
// claim (#514's rule: a gate you only read about is not a gate). Every vector
// is a path this tree can actually produce.
//
// Returns the number of FAILURES, and writes the count of vectors run through
// `total`. Zero failures is the only acceptable answer; dos/dosexec.c prints
// the line either way.
#[no_mangle]
pub unsafe extern "C" fn dospath_canon_selftest_rs(total: *mut i32) -> i32 {
    // (input, expected). Expected == input means "must be left alone".
    const V: &[(&str, &str)] = &[
        // The Dig: cwd E:\DIG, spec ".\*.BUN". This is the one that mattered.
        ("/WINDIR/DRIVE_E/DIG/./*.BUN", "/WINDIR/DRIVE_E/DIG/*.BUN"),
        // The directory half of the same search, after 4Eh splits at the slash.
        ("/WINDIR/DRIVE_E/DIG/.", "/WINDIR/DRIVE_E/DIG"),
        (".", ""),
        ("/WINDIR/DRIVE_E/DIG/./DIGMUSIC.BUN", "/WINDIR/DRIVE_E/DIG/DIGMUSIC.BUN"),
        // Ordinary parent walk.
        ("/WINDIR/DRIVE_C/DOS/KEEN5/../KEEN4/K4.EXE", "/WINDIR/DRIVE_C/DOS/KEEN4/K4.EXE"),
        ("/WINDIR/DRIVE_C/DOS/..", "/WINDIR/DRIVE_C"),
        // THE CLAMP. A guest may not climb out of its own drive, however many
        // ".." it spells, and the answer is its own drive root.
        ("/WINDIR/DRIVE_E/DIG/../..", "/WINDIR/DRIVE_E"),
        ("/WINDIR/DRIVE_E/DIG/../../..", "/WINDIR/DRIVE_E"),
        ("/WINDIR/DRIVE_E/../../../CONFIG/KIMI.KEY", "/WINDIR/DRIVE_E/CONFIG/KIMI.KEY"),
        ("/WINDIR/DRIVE_E/..", "/WINDIR/DRIVE_E"),
        // Outside a drive directory the floor is the native root.
        ("/DOS/KEEN5/../..", "/"),
        ("/..", "/"),
        // Left alone: no dot components at all.
        ("/WINDIR/DRIVE_E/DIG/DIG.EXE", "/WINDIR/DRIVE_E/DIG/DIG.EXE"),
        ("/", "/"),
        // A trailing dot in a NAME is not a "." component. DOS spells "no
        // extension" this way and it must survive untouched.
        ("/WINDIR/DRIVE_C/FILE.", "/WINDIR/DRIVE_C/FILE."),
        ("/WINDIR/DRIVE_C/..FOO", "/WINDIR/DRIVE_C/..FOO"),
        // /WINDIR/DRIVE_EX is not drive E: with a suffix, so no clamp applies.
        ("/WINDIR/DRIVE_EX/..", "/WINDIR"),
        // Doubled slashes are empty components.
        ("/WINDIR/DRIVE_C//DOS", "/WINDIR/DRIVE_C/DOS"),
    ];
    let mut fails = 0i32;
    let mut ran = 0i32;
    for (input, want) in V {
        let mut b = [0u8; PATH_MAX];
        let src = input.as_bytes();
        if src.len() + 1 > PATH_MAX {
            continue;
        }
        b[..src.len()].copy_from_slice(src);
        b[src.len()] = 0;
        dospath_canon_rs(b.as_mut_ptr(), PATH_MAX as i32);
        let mut got = 0usize;
        while got < PATH_MAX && b[got] != 0 {
            got += 1;
        }
        ran += 1;
        if &b[..got] != want.as_bytes() {
            fails += 1;
        }
    }
    if !total.is_null() {
        *total = ran;
    }
    fails
}
