// rustkern/vfs_path.rs - #487/#349 Task Manager accessor: open-handle paths
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #487/#349 Task Manager kernel accessor tier 2: OPEN-HANDLE PATHS
// (vfs_path_store).
//
// WHY THIS EXISTS: file_t (fs/vfs.h) carried no name, so the Task Manager's
// Details tab could only render "fd 5 flags 0x1" for every open handle. Naming
// the object behind a handle is a Process Explorer signature feature. file_t
// now carries a bounded path, recorded at open time by the two real open choke
// points (fat_vfs_open / ext2_vfs_open) plus the device openers.
//
// SAFETY VALUE (why Rust): this is the classic C string-copy footgun, in Ring 0,
// on a path that comes from a Ring-3 caller's open(). A strcpy into a fixed
// field is CWE-120; a strncpy that forgets the terminator is CWE-170. This
// stores through an exactly-cap slice, bounds the SOURCE scan too (the source
// is NOT trusted to be NUL-terminated within any particular distance), and
// ALWAYS terminates. Truncation is reported, never silent corruption.
//
// Routed live under -DRUST_VFS_PATH (fs/vfs.c keeps vfs_path_store_c as the
// reference twin + rollback). Boot [RUST-DIFF] vfs_path proves rs == c.
// ===========================================================================

/// Store `src` into the `cap`-byte buffer `dst`, always NUL-terminating.
/// `src_max` bounds how far `src` is scanned for its terminator, so a source
/// that is not NUL-terminated (a corrupt or hostile buffer) can never drive an
/// unbounded read. Returns the number of bytes stored, excluding the NUL.
/// A return of cap-1 means the value was truncated to fit.
#[no_mangle]
pub extern "C" fn vfs_path_store_rs(dst: *mut u8, cap: u32, src: *const u8, src_max: u32) -> u32 {
    if dst.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: caller guarantees `dst` spans at least `cap` writable bytes. Every
    // write below goes through this exactly-cap slice, so no index can leave it.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(dst, cap as usize) };
    if src.is_null() || src_max == 0 {
        d[0] = 0;
        return 0;
    }
    // SAFETY: caller guarantees `src` spans at least `src_max` readable bytes.
    // The scan below never leaves this exactly-src_max slice, so an unterminated
    // source is truncated rather than over-read.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(src, src_max as usize) };
    // Reserve one byte for the terminator: this is the whole bug class.
    let room = (cap - 1) as usize;
    let mut n = 0usize;
    while n < room && n < s.len() && s[n] != 0 {
        d[n] = s[n];
        n += 1;
    }
    d[n] = 0; // n <= cap-1, always in range
    n as u32
}
