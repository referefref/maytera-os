// rustkern/dosstate.rs - #dosperm: A DOS GAME MUST BE ABLE TO WRITE THE FILES
// IT SHIPPED WITH. (#word6blank, 2026-09-12: a Win16 app has the identical
// need; see win16_state_writable_rs() near the bottom of this file, which
// shares writable_under_prefix() with the /DOS rule below rather than forking
// a copy of it.)
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
//
// ===========================================================================
// THE DEFECT
// ---------------------------------------------------------------------------
// Nothing on the shipped image carries a /CONFIG/PERMS.DB row unless someone
// wrote one, and perms_check_leaf()'s no-entry default is root:root 0755. So
// EVERY file under /DOS/<title>/ is unwritable by every non-root account. A
// DOS-era game has one user and keeps its state beside its executable, so that
// default breaks a shipped game for the default desktop identity.
//
// MEASURED. Red Alert, launched by uid 1000 from the Start menu on golden 2339:
// it opens its own /DOS/RA/REDALERT.INI (93 bytes, present on the image), reads
// it, then reissues INT 21h AH=3Ch to rewrite it and is refused. On the Ring-3
// route it retried that create 112,423 times in 150 seconds and never got
// anywhere. The retry loop is a separate defect, fixed separately; THIS is why
// there was anything to retry.
//
// The two titles that were noticed before this (NetHack, SimCity) were each
// fixed by adding their own rows to perms_shared_state_seed[] in fs/perms.c.
// That is fixing the instance. There are thirteen titles under /DOS on the
// shipped asset base and the same wall stands in front of every one of them:
// Keen's CONFIG.CK5, The Incredible Machine's TIM.CFG, Aladdin's SOUND.CFG and
// REC1/REC2.DAT, Stunts' .HIG lap records and .RPL replays, Red Alert's
// REDALERT.INI. Adding a fourteenth row when the fourteenth game is reported is
// not a fix, it is a queue.
//
// ===========================================================================
// THE RULE
// ---------------------------------------------------------------------------
// A path of the shape /DOS/<title>/<name>... with NO explicit permission entry
// defaults to WRITABLE (root:root 0666) instead of root:root 0755, UNLESS its
// last component is program-shaped.
//
// WHAT THIS DELIBERATELY DOES NOT DO, and why each limit is where it is:
//
//  * THE DIRECTORY /DOS/<title> IS NOT TOUCHED. It keeps root:root 0755, so no
//    non-root account can create, delete or rename a name inside it. That is
//    the property that stops one user handing the next user a different
//    program, and it is worth more than the convenience of new files. The
//    HONEST COST: a game that saves by CREATING a file that is not already on
//    the image still cannot save. int21svc.c's dos_svc_allow_create() asks for
//    W_OK on the PARENT when the leaf does not exist, and the parent is not
//    covered by this rule. Monkey Island's save slots are the known example.
//    That is a real remaining gap, stated rather than papered over.
//
//  * PROGRAM-SHAPED NAMES KEEP 0755. Overwriting the .EXE in place would hand
//    the next user a different program without needing to create a name, which
//    would give back most of what keeping the directory at 0755 bought. The
//    extension list is the DOS loader's own notion of an executable.
//
//  * AN EXTENSION IS NOT PROOF, AND THIS IS THE HOLE THE LIST CANNOT CLOSE.
//    Red Alert's own launch target is /DOS/RA/GAME.DAT: a DOS/4GW LE executable
//    behind a data-file extension. A name-based test cannot see that. So the
//    disguised ones are named explicitly in perms_shared_state_seed[] (they get
//    a real 0755 row, which takes priority over this default), and
//    build/build-golden.sh REFUSES TO BUILD if any file under /DOS/<title>/ on
//    the image starts with an executable magic, lacks an executable extension,
//    and is not in that table. The list is small because the build computes the
//    requirement; it cannot go stale because a stale list fails the build.
//
//  * IT IS SCOPED TO /DOS. /GAMES has its own writable-directory arrangement
//    (perms_shared_state_seed[]) and nothing else on the image is a
//    single-user-era program that writes beside its own binary.

// The DOS loader's executables, plus the overlay and driver kinds a DOS program
// loads and executes as code. Upper case; the comparison folds case.
const EXEC_EXT: [&[u8]; 12] = [
    b".EXE", b".COM", b".BAT", b".SYS", b".OVL", b".OVR", b".DRV", b".BIN",
    b".PIF", b".DLL", b".386", b".CMD",
];

fn to_upper(c: u8) -> u8 {
    if c >= b'a' && c <= b'z' {
        c - 32
    } else {
        c
    }
}

/// Length of a NUL-terminated path, capped so a corrupt pointer cannot spin.
///
/// # Safety
/// `p` must be a valid NUL-terminated pointer.
unsafe fn plen(p: *const u8) -> usize {
    let mut n = 0usize;
    while n < 4096 && *p.add(n) != 0 {
        n += 1;
    }
    n
}

fn ends_with_exec_ext(name: &[u8]) -> bool {
    for ext in EXEC_EXT.iter() {
        if name.len() < ext.len() {
            continue;
        }
        let tail = &name[name.len() - ext.len()..];
        let mut same = true;
        for i in 0..ext.len() {
            if to_upper(tail[i]) != ext[i] {
                same = false;
                break;
            }
        }
        if same {
            return true;
        }
    }
    false
}

/// The rule, over an already-canonical path (perms_check() canonicalizes before
/// it looks anything up, so this only ever sees "/A/B/C" form).
///
/// Returns true when the no-entry default for `path` should carry the write bit.
fn writable_by_rule(path: &[u8]) -> bool {
    writable_under_prefix(path, b"/DOS/")
}

/// Same rule, parameterized on the state-tree prefix (must end in "/"), so a
/// second single-user-era tree (#word6blank: /WIN16) can share the identical,
/// already-reviewed decision instead of forking a copy of it. See
/// win16_state_writable_rs() below for why /WIN16 needs this too.
fn writable_under_prefix(path: &[u8], prefix: &[u8]) -> bool {
    // Must be exactly the tree named by `prefix`. "/DOSGAMES/..." must NOT
    // match "/DOS/", so the component boundary is checked rather than the
    // prefix alone.
    if path.len() < prefix.len() || &path[..prefix.len()] != prefix {
        return false;
    }
    // At least <prefix><title>/<name>: one more separator after the title,
    // with a non-empty title before it and a non-empty name after it.
    let rest = &path[prefix.len()..];
    let mut sep = None;
    for (i, c) in rest.iter().enumerate() {
        if *c == b'/' {
            sep = Some(i);
            break;
        }
    }
    let sep = match sep {
        Some(0) => return false, // "/DOS//..." : no title
        Some(i) => i,
        None => return false, // "/DOS/RA" : the title directory itself
    };
    let tail = &rest[sep + 1..];
    if tail.is_empty() {
        return false; // "/DOS/RA/" : still the directory
    }
    // The LAST component decides, so a file in a subdirectory
    // (/DOS/SIMCITY/DATA/X.CTY) is judged on its own name.
    let mut last = tail;
    for (i, c) in tail.iter().enumerate() {
        if *c == b'/' && i + 1 < tail.len() {
            last = &tail[i + 1..];
        }
    }
    if last.is_empty() {
        return false;
    }
    !ends_with_exec_ext(last)
}

/// C entry point, called from perms_check_leaf()'s NO-ENTRY branch in
/// fs/perms.c and from nowhere else. 1 = the write bit is granted by default,
/// 0 = keep the root-owned 0755 default.
///
/// # Safety
/// `path` must be a valid NUL-terminated kernel pointer or null.
#[no_mangle]
pub extern "C" fn dos_state_writable_rs(path: *const u8) -> i32 {
    if path.is_null() {
        return 0;
    }
    let n = unsafe { plen(path) };
    let s = unsafe { core::slice::from_raw_parts(path, n) };
    if writable_by_rule(s) {
        1
    } else {
        0
    }
}

/// Self-test. Returns the number of FAILING vectors, 0 for a clean run. Run at
/// boot from perms_selftest() so a rule that stopped matching is visible on the
/// machine rather than in a review.
#[no_mangle]
pub extern "C" fn dosstate_selftest_rs() -> u32 {
    // (path, expected)
    const V: [(&[u8], bool); 18] = [
        // the case this exists for
        (b"/DOS/RA/REDALERT.INI", true),
        (b"/DOS/KEEN5/CONFIG.CK5", true),
        (b"/DOS/TIM/TIM.CFG", true),
        (b"/DOS/ALADDIN/SOUND.CFG", true),
        (b"/DOS/ALADDIN/REC1.DAT", true),
        (b"/DOS/STUNTS/DEFAULT.HIG", true),
        (b"/DOS/STUNTS/BAM!.RPL", true),
        // a file one level deeper is judged on its own name
        (b"/DOS/SIMCITY/DATA/CITY.CTY", true),
        // program-shaped names keep the read-only default
        (b"/DOS/RA/REDALERT.EXE", false),
        (b"/DOS/STUNTS/ST.COM", false),
        (b"/DOS/JOUST/START.bat", false), // lower case, folded
        (b"/DOS/STUNTS/MT15.DRV", false),
        (b"/DOS/SIMCITY/SIMCITY.EXE", false),
        // the title directory itself is NEVER made writable by this rule
        (b"/DOS/RA", false),
        (b"/DOS/RA/", false),
        (b"/DOS", false),
        // and the rule is scoped: a look-alike prefix must not match
        (b"/DOSGAMES/RA/REDALERT.INI", false),
        (b"/CONFIG/SHADOW", false),
    ];
    let mut bad = 0u32;
    for (p, want) in V.iter() {
        if writable_by_rule(p) != *want {
            bad += 1;
        }
    }
    bad
}

// ===========================================================================
// #word6blank (2026-09-12): the IDENTICAL defect, one level up the Win16
// stack, found chasing the "Word 6 renders a blank chrome-only window for a
// non-root account" report.
//
// MEASURED, uid 1000, golden dev build: with the (separate, since-fixed)
// WinMain "not enough memory" false-positive out of the way, Word 6 gets as
// far as creating its startup recovery/lock file beside its own binary --
// INT 21h AH=3Ch on /WIN16/WORD6/~WRF0000.TMP -- and is refused:
// `[GUESTFS-DENY] ... op=_lcreat/OF_CREATE write reason=PERMS
// path=/WIN16/WORD6`. It retries under ~WRF0001.TMP, is refused again, then
// gives up and calls INT 21h AH=4Ch (exit code 0). The compositor-side host
// window (title bar + frame) is deliberately kept visible after the guest
// exits (W6PERSIST), so the user sees exactly the reported symptom: a
// permanently blank "Microsoft Word" window with no menu, no toolbar, no
// document. THIS is the real #708 GUESTFS differential the task set out to
// confirm or refute -- root's uid-0 perms_check() bypass makes this
// invisible for root, and every verified-working Word6 reference (2160,
// 2200) has always run as root.
//
// Root cause is identical in shape to #dosperm above: a Win16-era app has one
// user and keeps its scratch/state beside its own binary (Word 6's install
// directory here plays the same role /DOS/<title> plays for a DOS game), and
// the root-owned-0755 no-entry default makes every such file unwritable for
// any non-root account. So: the exact same rule, over /WIN16/<app>/<name>
// instead of /DOS/<title>/<name>, sharing writable_under_prefix() rather than
// forking a second copy of the decision. The app DIRECTORY and every
// program-shaped name in it (EXEC_EXT already covers .EXE/.DLL/.DRV/.OVL/...)
// stay root-owned 0755, so a non-root account still cannot add, remove, or
// overwrite the shipped program -- only the state it keeps beside itself
// (WINWORD6.INI, NORMAL.DOT, ~WRF*.TMP and the like).
#[no_mangle]
pub extern "C" fn win16_state_writable_rs(path: *const u8) -> i32 {
    if path.is_null() {
        return 0;
    }
    let n = unsafe { plen(path) };
    let s = unsafe { core::slice::from_raw_parts(path, n) };
    if writable_under_prefix(s, b"/WIN16/") {
        1
    } else {
        0
    }
}

/// Self-test for the /WIN16 rule, same shape and same reasons as
/// dosstate_selftest_rs() above (run from perms_selftest()).
#[no_mangle]
pub extern "C" fn win16state_selftest_rs() -> u32 {
    const V: [(&[u8], bool); 13] = [
        // the case this exists for: Word 6's own startup recovery file
        (b"/WIN16/WORD6/~WRF0000.TMP", true),
        (b"/WIN16/WORD6/WINWORD6.INI", true),
        (b"/WIN16/WORD6/NORMAL.DOT", true),
        // a file one level deeper is judged on its own name
        (b"/WIN16/WORD6/TEMPLATE/CUSTOM.DOT", true),
        // program-shaped names keep the read-only default: no non-root
        // account may overwrite the shipped app
        (b"/WIN16/WORD6/WINWORD.EXE", false),
        (b"/WIN16/WORD6/COMPOBJ.DLL", false),
        (b"/WIN16/WORD6/OLE2.DLL", false),
        // the app directory itself is NEVER made writable by this rule
        (b"/WIN16/WORD6", false),
        (b"/WIN16/WORD6/", false),
        (b"/WIN16", false),
        // scoped: a look-alike prefix must not match, and /DOS is unaffected
        (b"/WIN16GAMES/WORD6/FOO.INI", false),
        (b"/DOS/RA/REDALERT.INI", false),
        (b"/CONFIG/SHADOW", false),
    ];
    let mut bad = 0u32;
    for (p, want) in V.iter() {
        if writable_under_prefix(p, b"/WIN16/") != *want {
            bad += 1;
        }
    }
    bad
}
