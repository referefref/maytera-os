// rustkern/permpath.rs - #674: real POSIX path resolution for perms_check().
//
// WHY THIS EXISTS
// ---------------
// perms_check() was an EXACT-PATH hash lookup. It looked up the one string it
// was handed and nothing else. That is not POSIX, and it broke the model in
// three separate ways, each of which alone defeats the permission database:
//
//   1. NO TRAVERSAL. /CONFIG being mode 0700 did not protect /CONFIG/KIMI.KEY,
//      because that path has no entry of its own and fell into perms_check()'s
//      "no entry" default (root-owned, read/execute for everyone). In POSIX,
//      resolving a path requires search (x) permission on EVERY directory
//      component, so the mode on the DIRECTORY is what protects its contents.
//
//   2. "." AND ".." WERE NEVER RESOLVED, but the filesystem resolves them.
//      fs/ext2.c ext2_resolve_path() walks the path one component at a time
//      through real on-disk directory entries, and ext2_mkdir() writes real
//      "." and ".." dirents (fs/ext2.c ~3015). So "/CONFIG/../CONFIG/SHADOW"
//      opens the very same inode as "/CONFIG/SHADOW", while the string the
//      permission check saw was a THIRD string that matched no entry and hit
//      the permissive default. That is a bypass of the 0600 on SHADOW that
//      exists in the shipping tree today, independent of #674.
//
//   3. RELATIVE PATHS WERE NEVER MADE ABSOLUTE, but the filesystem makes them
//      absolute. proc/syscall.c sys_open_k() prepends '/' to a bare name so
//      the ext2-root redirect can resolve it (userland opens assets by bare
//      name). perms_check() was called BEFORE that, on the bare string, so
//      open("CONFIG/SHADOW") checked the key "CONFIG/SHADOW", found nothing,
//      and allowed the read of /CONFIG/SHADOW. Same bypass, simpler to reach.
//
// (2) and (3) are the same underlying fault: THE STRING THAT WAS CHECKED WAS
// NOT THE OBJECT THAT WAS OPENED. Adding traversal without fixing them would
// have shipped a control that a single "/./" defeats, which is worse than no
// control because it reads as protection. So this module canonicalizes FIRST,
// exactly the way the filesystem will resolve the name, and only then walks.
//
// WHY RUST
// --------
// New kernel code is Rust unless there is a stated performance reason (CLAUDE.md).
// There is none here: this runs once per open(), not per byte. The work is
// bounded string surgery on a Ring-3-supplied path in Ring 0, which is the
// exact CWE-120/CWE-170 territory that keeps producing kernel bugs in C. Every
// buffer below is a fixed-size slice indexed only through bounds-checked slice
// operations, the source scan is explicitly bounded (the caller's string is NOT
// trusted to terminate), and overflow FAILS CLOSED rather than truncating into
// a shorter, more permissive key.
//
// The single-object decision stays in C as perms_check_leaf() (fs/perms.c),
// because it is the pre-existing hash-table lookup and its pool/chain layout is
// C-internal. This module owns the NEW logic: canonicalization and the walk.

// Access bits, matching fs/perms.h. Duplicated as constants rather than
// #included because there is no bindgen in this build; the values are POSIX and
// fixed, and perms_selftest() (fs/perms.c) would fail loudly if they drifted.
const X_OK: i32 = 1;

const PATH_MAX: usize = 256; // matches perm_entry_t.path[256] in fs/perms.h
// Bound on how far a caller-supplied string is scanned for its NUL. A path
// longer than this cannot correspond to any perm_entry_t key anyway (they cap
// at 255 chars), so refusing it loses no reachable policy.
const SRC_SCAN_MAX: usize = 1024;

extern "C" {
    // fs/perms.c: the single-object check. Applies the entry's rwx bits (or the
    // no-entry default) to ONE already-canonical path. Does NOT recurse and
    // does NOT apply the uid-0 / !perms_initialized early-outs: perms_check()
    // owns those, so this module can never be a way around them.
    fn perms_check_leaf(path: *const u8, uid: u32, gid: u32, access: i32) -> i32;
}

/// Return code for "the path could not be canonicalized into PATH_MAX bytes".
/// Distinct from a plain deny so the C caller can log it: a legitimate deep
/// path being refused is a policy problem worth seeing on the console, not a
/// silent EACCES to chase from userland. The C caller treats it as a DENY.
/// (kprintf is variadic; keeping it out of the Rust FFI surface entirely is
/// cheaper than carrying a variadic extern for one diagnostic.)
pub const PERMS_ETOOLONG: i32 = -2;

/// Canonicalize `src` into `dst` the way the filesystem will resolve it:
/// force absolute, collapse repeated '/', drop "." components, and pop ".."
/// components (never above the root, matching POSIX where "/.." is "/").
///
/// Returns the canonical length (excluding the NUL that is always written), or
/// None if the result would not fit. A caller that gets None must FAIL CLOSED:
/// truncating would produce a SHORTER key, and a shorter key is a
/// less-specific, more-permissive one.
fn canonicalize(src: &[u8], dst: &mut [u8; PATH_MAX]) -> Option<usize> {
    dst[0] = b'/';
    let mut out = 1usize; // out is always >= 1: dst[0..out] is the prefix so far

    let mut i = 0usize;
    while i < src.len() {
        // Skip any run of separators. This collapses "//" and handles the
        // leading '/' of an absolute path uniformly with the implicit root of
        // a relative one (sys_open_k prepends '/' to bare names, so a relative
        // path is root-relative here, not cwd-relative).
        if src[i] == b'/' {
            i += 1;
            continue;
        }
        // Take one component.
        let start = i;
        while i < src.len() && src[i] != b'/' {
            i += 1;
        }
        let comp = &src[start..i];

        if comp == b"." {
            continue; // no-op component
        }
        if comp == b".." {
            // Pop the last component. Walk back over the component bytes to the
            // '/' that introduced it, then drop that '/' too (unless it is the
            // root's own, which we never remove).
            while out > 1 && dst[out - 1] != b'/' {
                out -= 1;
            }
            if out > 1 {
                out -= 1; // drop the separator
            }
            continue;
        }
        // Append "/comp", except at the root where the '/' is already there.
        if out > 1 {
            if out + 1 >= PATH_MAX {
                return None;
            }
            dst[out] = b'/';
            out += 1;
        }
        if out + comp.len() >= PATH_MAX {
            return None; // no room for the component plus its NUL
        }
        let mut k = 0usize;
        while k < comp.len() {
            dst[out] = comp[k];
            out += 1;
            k += 1;
        }
    }
    dst[out] = 0; // out < PATH_MAX is guaranteed by every bound above
    Some(out)
}

/// Bounded scan for the NUL terminator of a caller-supplied C string.
/// Returns None if no terminator is found within SRC_SCAN_MAX bytes, so an
/// unterminated or hostile buffer can never drive an unbounded read.
unsafe fn cstr_bounded<'a>(p: *const u8) -> Option<&'a [u8]> {
    let mut n = 0usize;
    while n < SRC_SCAN_MAX {
        if *p.add(n) == 0 {
            return Some(core::slice::from_raw_parts(p, n));
        }
        n += 1;
    }
    None
}

/// POSIX path-resolution permission check.
///
/// Canonicalizes `path`, then requires search (x) on the root and on EVERY
/// intermediate directory component, then applies `access` to the object
/// itself. Returns 0 on allow, -1 on deny, PERMS_ETOOLONG (-2) on a path that
/// will not canonicalize into PATH_MAX (also a deny; the C caller logs it).
///
/// The uid-0 bypass and the !perms_initialized bypass are NOT here: perms_check()
/// applies them before calling, so this function's behavior is identical for
/// root and non-root and cannot be used to sidestep either.
#[no_mangle]
pub extern "C" fn perms_path_check_rs(path: *const u8, uid: u32, gid: u32, access: i32) -> i32 {
    if path.is_null() {
        return -1;
    }
    // SAFETY: caller passes a kernel-side NUL-terminated path (sys_open_k has
    // already bounced it out of user memory with strncpy_from_user). The scan
    // is bounded regardless, so a missing terminator yields a reject.
    let src = match unsafe { cstr_bounded(path) } {
        Some(s) => s,
        None => return -1, // fail closed
    };

    let mut canon = [0u8; PATH_MAX];
    let n = match canonicalize(src, &mut canon) {
        Some(n) => n,
        None => return PERMS_ETOOLONG, // fail closed; C logs and denies
    };

    let mut prefix = [0u8; PATH_MAX];

    // The root itself must be searchable.
    prefix[0] = b'/';
    prefix[1] = 0;
    if unsafe { perms_check_leaf(prefix.as_ptr(), uid, gid, X_OK) } != 0 {
        return -1;
    }

    // Every intermediate directory component: canon[0..i] for each separator at
    // index i > 0. The final component is deliberately NOT included here; it is
    // the object, and gets `access` rather than X_OK below.
    let mut i = 1usize;
    while i < n {
        if canon[i] == b'/' {
            let mut k = 0usize;
            while k < i {
                prefix[k] = canon[k];
                k += 1;
            }
            prefix[i] = 0;
            if unsafe { perms_check_leaf(prefix.as_ptr(), uid, gid, X_OK) } != 0 {
                return -1;
            }
        }
        i += 1;
    }

    // The object itself.
    unsafe { perms_check_leaf(canon.as_ptr(), uid, gid, access) }
}

/// Canonicalization self-test, callable from C (fs/perms.c perms_selftest()).
/// Writes the canonical form of `src` into a caller-provided `cap`-byte buffer
/// and returns its length, or -1 on overflow. This exists so the boot self-test
/// can assert on the ACTUAL canonicalizer rather than on a re-implementation of
/// it, which is the failure mode where a test and its subject share a bug.
#[no_mangle]
pub extern "C" fn perms_canon_rs(src: *const u8, out: *mut u8, cap: u32) -> i32 {
    if src.is_null() || out.is_null() || (cap as usize) < PATH_MAX {
        return -1;
    }
    let s = match unsafe { cstr_bounded(src) } {
        Some(s) => s,
        None => return -1,
    };
    let mut canon = [0u8; PATH_MAX];
    let n = match canonicalize(s, &mut canon) {
        Some(n) => n,
        None => return -1,
    };
    // SAFETY: caller guarantees `out` spans at least `cap` >= PATH_MAX bytes.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, PATH_MAX) };
    let mut k = 0usize;
    while k <= n {
        d[k] = canon[k];
        k += 1;
    }
    n as i32
}
