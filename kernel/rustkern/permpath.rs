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
fn canonicalize(src: &[u8], dst: &mut [u8]) -> Option<usize> {
    // #58: `dst` became a SLICE rather than a fixed [u8; PATH_MAX] so that ONE
    // canonicalizer can serve buffers of different widths (perms_check()'s
    // 256-byte perm_entry_t key, and #58's relative-path resolution) without a
    // second copy of this logic existing anywhere in the tree. Behaviour at
    // width PATH_MAX is unchanged: every bound below now reads `cap`, which IS
    // PATH_MAX for both pre-existing callers.
    let cap = dst.len();
    if cap < 2 {
        return None; // no room for "/" and its NUL
    }
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
            if out + 1 >= cap {
                return None;
            }
            dst[out] = b'/';
            out += 1;
        }
        if out + comp.len() >= cap {
            return None; // no room for the component plus its NUL
        }
        let mut k = 0usize;
        while k < comp.len() {
            dst[out] = comp[k];
            out += 1;
            k += 1;
        }
    }
    dst[out] = 0; // out < cap is guaranteed by every bound above
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


// ===========================================================================
// #58: RESOLVING A RELATIVE PATH AGAINST THE PROCESS WORKING DIRECTORY.
//
// THE DEFECT. process_t::cwd EXISTS, sys_chdir() maintains it correctly (four
// separate faults were fixed in it by #745 Stage 3) and sys_getcwd() reads it
// back. NOTHING ELSE IN THE KERNEL EVER READ IT. Every path-taking syscall
// handed the raw Ring-3 string straight to the FAT/ext2 resolvers, and both of
// those treat a name with no leading '/' as ROOT-relative: fs/fat.c
// fat_open_inner() does `if (*p == '/') p++` and then walks from the root
// cluster, and proc/fdlayer.c sys_open_k() explicitly PREPENDS '/' to a bare
// name so the ext2-root redirect can resolve it.
//
// So after chdir("/GAMES/FOO"), open("data/x") opened /data/x and
// mkdir("texpacks") created /texpacks. Measured on golden 1811 and recorded in
// blame.md ("MayteraOS has no working directory"): chdir returned 0, mkdir
// returned 0, the kernel printed "[FS] Created directory: texpacks", and
// debugfs then found no such directory on EITHER partition. THE FAILURE IS
// SILENT AND POSITIVE - a confident wrong answer, not a crash - which is the
// same shape as the fake-tool family, and it is why every ported POSIX program
// has had to rewrite paths at its own boundary to work here at all.
//
// It was also NOT UNIFORM, which is worse than being uniformly wrong: only
// sys_open() had the prepend-'/' branch, so open("X") and stat("X") could
// disagree about which file they meant.
//
// WHY THE JOIN LIVES HERE, NEXT TO canonicalize(). sys_chdir() already builds
// an absolute path from cwd and canonicalizes it through this file, so that the
// string it AUTHORIZES and the string it STORES are canonical in the same sense
// as the one perms_check() walks. A relative path handed to open() must answer
// the identical question, and answering it with a SECOND, separate
// implementation is precisely how the string that is CHECKED stops being the
// object that is OPENED - faults (2) and (3) at the top of this file. One
// canonicalizer, one join, both used by everyone.
//
// WHAT IS DELIBERATELY NOT CHANGED: AN ABSOLUTE PATH IS LEFT EXACTLY ALONE,
// byte for byte, and the function does not even copy it. Two reasons, both
// load-bearing:
//   (1) BLAST RADIUS. Every existing caller in the tree passes absolute paths,
//       and a process that has never called chdir() has cwd "/" (set in
//       proc create, process.c). So this change cannot alter any path that
//       resolves correctly today. In particular the compositor's bare-name
//       asset opens ("MAYTERA.BMP") resolve to "/MAYTERA.BMP" exactly as the
//       fdlayer prepend gave them, because "/" joined with a bare name IS the
//       prepend.
//   (2) THE 256-BYTE CAP IS NOT OURS TO IMPOSE ON ABSOLUTE PATHS.
//       canonicalize() caps at PATH_MAX (256) because that is the width of
//       perm_entry_t::path, while the syscall path buffers are SC_PATH_MAX
//       (1024). Pushing every absolute path through the 256-byte canonicalizer
//       would start REFUSING long absolute paths that open fine today. Relative
//       paths carry no such exposure: they are joined onto a cwd that
//       sys_chdir() already refuses to set beyond PROC_CWD_MAX (256), and a
//       resolved path longer than PATH_MAX could not be permission-checked
//       anyway (perms_path_check_rs would return PERMS_ETOOLONG and deny), so
//       refusing it here is consistent with what the policy layer can express
//       rather than a new limit invented at this layer.
// ===========================================================================

/// Width of the syscall path buffers (SC_PATH_MAX, proc/syscall_path.h). The
/// joined "cwd + '/' + path" is assembled at this width so that a long relative
/// path is REFUSED by canonicalize()'s own cap rather than being clipped on the
/// way in. Clipping would silently resolve a DIFFERENT path, which is the exact
/// class of fault #745 Stage 3 removed from sys_chdir (its "FAULT 3").
const SC_PATH_MAX: usize = 1024;

/// Resolve, IN PLACE, a path that has already been bounced out of user memory.
///
/// `buf` holds a NUL-terminated path on entry and receives the resolved path on
/// exit; `cap` is its full width. `cwd` is the calling process's working
/// directory (process_t::cwd).
///
/// IN PLACE, AND ON A BUFFER THE CALLER ALREADY HAS, on purpose: the alternative
/// signature (separate src and dst) forces every call site to carry a SECOND
/// SC_PATH_MAX buffer in the same stack frame purely to hold a string it is
/// about to overwrite. sys_rename already carries two 1 KB path buffers, so
/// doubling them is real Ring-0 stack, spent to no end.
///
/// Behaviour:
///   * ABSOLUTE `buf` (leading '/'): returns its length and touches nothing.
///     See the block comment above for why this is deliberate.
///   * RELATIVE `buf`: joined onto `cwd` and canonicalized with the SAME
///     canonicalize() that perms_check() and sys_chdir() use, so "." is dropped,
///     ".." is popped, and ".." can never climb above the root.
///   * `cwd` NULL or empty is treated as "/", which reproduces the pre-#58
///     root-relative behaviour EXACTLY. That is the correct fallback for the
///     contexts that have no current process (boot-time and kernel-thread
///     callers), and it is what makes this change a no-op for them.
///   * An EMPTY `buf` is REFUSED. POSIX open("") is ENOENT; silently promoting
///     it to the cwd would make open("") hand back a directory.
///
/// Returns the resolved length (excluding the NUL), or -1 on refusal: a null
/// pointer, a source with no terminator inside the buffer, or a result that does
/// not fit. Callers MUST treat -1 as a hard failure. There is deliberately no
/// truncating path out of this function, because acting on a truncated path acts
/// on a different file.
#[no_mangle]
pub extern "C" fn path_resolve_cwd_rs(cwd: *const u8, buf: *mut u8, cap: u32) -> i32 {
    if buf.is_null() || cap == 0 {
        return -1;
    }
    let capu = cap as usize;

    // Read the incoming path WITHOUT forming a shared reference to `buf`: the
    // same allocation is written through a &mut slice at the end of this
    // function, and holding a & and a &mut to it simultaneously would be UB
    // even though no read and write overlap in time. Raw reads sidestep that
    // entirely. The scan is bounded by `cap`, so a caller-supplied buffer with
    // no terminator can never drive an unbounded read.
    let mut slen = 0usize;
    loop {
        if slen >= capu {
            return -1; // not terminated inside its own buffer: fail closed
        }
        // SAFETY: slen < capu and the caller guarantees `buf` spans `cap` bytes.
        if unsafe { core::ptr::read(buf.add(slen)) } == 0 {
            break;
        }
        slen += 1;
    }

    if slen == 0 {
        return -1; // POSIX: "" is ENOENT, never "the current directory"
    }
    // SAFETY: 0 < capu, so index 0 is inside the buffer.
    if unsafe { core::ptr::read(buf) } == b'/' {
        return slen as i32; // already absolute: leave it exactly as it is
    }

    // ---- relative: join onto the cwd, then canonicalize ----
    let base: &[u8] = if cwd.is_null() {
        b"/"
    } else {
        // SAFETY: `cwd` is a kernel-side NUL-terminated string (process_t::cwd,
        // which sys_chdir always terminates). cstr_bounded bounds the scan.
        match unsafe { cstr_bounded(cwd) } {
            Some(c) if !c.is_empty() => c,
            _ => b"/",
        }
    };

    let mut joined = [0u8; SC_PATH_MAX];
    let mut n = 0usize;
    let mut k = 0usize;
    while k < base.len() {
        if n >= SC_PATH_MAX {
            return -1;
        }
        joined[n] = base[k];
        n += 1;
        k += 1;
    }
    if n == 0 || joined[n - 1] != b'/' {
        if n >= SC_PATH_MAX {
            return -1;
        }
        joined[n] = b'/';
        n += 1;
    }
    k = 0;
    while k < slen {
        if n >= SC_PATH_MAX {
            return -1;
        }
        // SAFETY: k < slen < capu, inside the caller's buffer.
        joined[n] = unsafe { core::ptr::read(buf.add(k)) };
        n += 1;
        k += 1;
    }

    // ONE canonicalizer for the whole kernel. Caps at PATH_MAX and FAILS CLOSED.
    let mut canon = [0u8; PATH_MAX];
    let cn = match canonicalize(&joined[..n], &mut canon) {
        Some(v) => v,
        None => return -1,
    };
    if cn + 1 > capu {
        return -1; // will not fit in the caller's buffer: refuse, never clip
    }
    // SAFETY: the caller guarantees `buf` spans `cap` bytes, and cn + 1 <= cap.
    // No shared reference to this allocation is alive here: every read above
    // went through core::ptr::read.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, capu) };
    let mut z = 0usize;
    while z <= cn {
        d[z] = canon[z];
        z += 1;
    }
    cn as i32
}

// ---------------------------------------------------------------------------
// #58 boot self-test. "A rule that has never been watched being right is a
// comment" (dos/dosexec.c). This drives the ACTUAL exported entry point, on the
// exact cases the ticket and blame.md name, rather than a re-implementation of
// it, which is the failure mode where the test and its subject share a bug.
//
// Returns the number of FAILING cases; 0 is a pass.
// ---------------------------------------------------------------------------

/// One case: run `path` from `cwd` and require exactly `want`.
/// `want` empty means "the call must be REFUSED" (return < 0).
fn pr_case(cwd: &[u8], path: &[u8], want: &[u8]) -> bool {
    let mut c = [0u8; PATH_MAX];
    if cwd.len() + 1 > c.len() {
        return false;
    }
    c[..cwd.len()].copy_from_slice(cwd);
    c[cwd.len()] = 0;

    let mut b = [0u8; SC_PATH_MAX];
    if path.len() + 1 > b.len() {
        return false;
    }
    b[..path.len()].copy_from_slice(path);
    b[path.len()] = 0;

    // A zero-length `cwd` is passed as a NULL pointer, which is how the C side
    // reports "no current process": that fallback needs testing too.
    let cp: *const u8 = if cwd.is_empty() { core::ptr::null() } else { c.as_ptr() };
    let r = path_resolve_cwd_rs(cp, b.as_mut_ptr(), SC_PATH_MAX as u32);

    if want.is_empty() {
        return r < 0;
    }
    if r < 0 || r as usize != want.len() {
        return false;
    }
    let mut i = 0usize;
    while i < want.len() {
        if b[i] != want[i] {
            return false;
        }
        i += 1;
    }
    b[want.len()] == 0
}

#[no_mangle]
pub extern "C" fn path_resolve_selftest_rs() -> i32 {
    let cases: [(&[u8], &[u8], &[u8]); 18] = [
        // THE EXACT FAILING CASE from blame.md, golden 1811: chdir into a game
        // directory, then touch a name inside it. Before #58 this resolved to
        // "/texpacks" and reported success.
        (b"/GAMES/CLASSICUBE", b"texpacks", b"/GAMES/CLASSICUBE/texpacks"),
        (b"/GAMES/FOO", b"data/x", b"/GAMES/FOO/data/x"),
        (b"/HOME/ADMIN", b"notes.txt", b"/HOME/ADMIN/notes.txt"),

        // An absolute path is returned UNCHANGED, from any cwd. This is the
        // half of the contract that guarantees nothing in the tree regresses.
        (b"/HOME/ADMIN", b"/APPS/TERMINAL", b"/APPS/TERMINAL"),
        (b"/GAMES/FOO", b"/", b"/"),
        // Unchanged means unchanged: an absolute path is NOT canonicalized, so
        // its ".." survives for the filesystem to resolve, exactly as today.
        (b"/HOME/ADMIN", b"/A/../B", b"/A/../B"),

        // "." and ".." behave.
        (b"/HOME/ADMIN", b".", b"/HOME/ADMIN"),
        (b"/HOME/ADMIN", b"..", b"/HOME"),
        (b"/HOME/ADMIN", b"../REF/x", b"/HOME/REF/x"),
        (b"/A", b"./b//c/", b"/A/b/c"),

        // ".." CANNOT ESCAPE THE ROOT, however hard it tries. This is the case
        // that makes cwd resolution safe to expose to Ring 3 at all.
        (b"/", b"..", b"/"),
        (b"/", b"../../../etc", b"/etc"),
        (b"/HOME/ADMIN", b"../../../../..", b"/"),

        // cwd "/" reproduces the PRE-#58 root-relative behaviour exactly, which
        // is why processes that never chdir (i.e. nearly all of them, including
        // the compositor) see no change at all.
        (b"/", b"MAYTERA.BMP", b"/MAYTERA.BMP"),
        (b"/", b"APPS/FILES", b"/APPS/FILES"),
        // ... and so does a NULL cwd (no current process: boot / kernel thread).
        (b"", b"MAYTERA.BMP", b"/MAYTERA.BMP"),

        // Refusals. An empty path is ENOENT, not "the cwd".
        (b"/HOME/ADMIN", b"", b""),
        // A relative path that cannot fit the canonical buffer is refused, not
        // clipped: 300 'a's under a cwd cannot land inside PATH_MAX (256).
        (b"/HOME/ADMIN", &[b'a'; 300], b""),
    ];

    let mut bad = 0i32;
    let mut i = 0usize;
    while i < cases.len() {
        let (cwd, path, want) = cases[i];
        if !pr_case(cwd, path, want) {
            bad += 1;
        }
        i += 1;
    }
    bad
}
