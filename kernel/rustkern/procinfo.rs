// rustkern/procinfo.rs - #487/#349 Ring-3 introspection builders (procinfo)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #487/#349 Ring-3 introspection builders (procinfo).
//
// WHY: the Task Manager the user actually opens is the USERLAND app
// (/apps/taskmgr). It had no way to ask the kernel about open handles or
// services, so it could not render them at any level of effort. These builders
// back SYS_PROC_HANDLES and SYS_SVC_LIST.
//
// SPLIT OF RESPONSIBILITY: the C side walks the fd table / service registry
// (it needs process_t and service_t, which stay C) and hands over a flat array
// of raw source rows. EVERY copy out of those raw pointers happens HERE, in
// Rust, bounded. The source strings are explicitly NOT trusted to be
// NUL-terminated: each is scanned through an exactly-sized slice, so a corrupt
// registry entry truncates instead of reading off the end of the heap.
//
// Routed live under -DRUST_PROCINFO (proc/procinfo.c keeps handles_build_c /
// svc_build_c as reference twins + rollback). Boot [RUST-DIFF] procinfo.
// ===========================================================================

const PI_PATH_MAX: usize = 96;
pub(crate) const PI_NAME_MAX: usize = 32;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HandleInfo {
    pub fd: i32,
    pub flags: i32,
    pub kind: u32,
    pub _pad: u32,
    pub path: [u8; PI_PATH_MAX],
}
const _: () = assert!(core::mem::size_of::<HandleInfo>() == 112);

#[repr(C)]
pub struct HandleSrc {
    pub fd: i32,
    pub flags: i32,
    pub kind: u32,
    pub _pad: u32,
    pub path: *const u8,
}
const _: () = assert!(core::mem::size_of::<HandleSrc>() == 24);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SvcInfo {
    pub running: u32,
    pub autostart: u32,
    pub perms: u32,
    pub pid: u32,
    pub name: [u8; PI_NAME_MAX],
    pub account: [u8; PI_NAME_MAX],
}
const _: () = assert!(core::mem::size_of::<SvcInfo>() == 80);

#[repr(C)]
pub struct SvcSrc {
    pub running: u32,
    pub autostart: u32,
    pub perms: u32,
    pub pid: u32,
    pub name: *const u8,
    pub account: *const u8,
}
const _: () = assert!(core::mem::size_of::<SvcSrc>() == 32);

/// Copy a NUL-terminated C string from `src` into the fixed array `dst`,
/// bounded on BOTH ends and always terminated. `src` is untrusted: the scan
/// stops at dst.len()-1 regardless of whether a NUL was found, so an
/// unterminated source cannot drive a read past that bound. A NULL `src`
/// yields an empty string, never a fault.
///
/// # Safety
/// `src`, if non-null, must be readable for at least `dst.len()` bytes OR be
/// NUL-terminated before then. Both callers below pass kernel strings from
/// file_t.path / service_t (fixed-size fields), which satisfy this.
unsafe fn cstr_into(dst: &mut [u8], src: *const u8) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if src.is_null() || dst.is_empty() {
        return;
    }
    let room = dst.len() - 1; // reserve the terminator
    let mut i = 0usize;
    while i < room {
        let c = *src.add(i);
        if c == 0 {
            break;
        }
        dst[i] = c;
        i += 1;
    }
    dst[i] = 0; // i <= room, always in range
}

#[no_mangle]
pub extern "C" fn handles_build_rs(src: *const HandleSrc, n: u32,
                                   out: *mut HandleInfo, cap: u32) -> i32 {
    if src.is_null() || out.is_null() {
        return -1;
    }
    if cap == 0 || n == 0 {
        return 0;
    }
    // SAFETY: the caller (proc/procinfo.c) guarantees `src` spans >= n rows and
    // `out` spans >= cap rows. Both are reached ONLY through these exactly-sized
    // slices, so the row index cannot leave either allocation.
    let s: &[HandleSrc] = unsafe { core::slice::from_raw_parts(src, n as usize) };
    let d: &mut [HandleInfo] = unsafe { core::slice::from_raw_parts_mut(out, cap as usize) };
    let mut w = 0usize;
    for row in s.iter() {
        if w >= d.len() {
            break; // caller's array full: stop, never over-write
        }
        d[w].fd = row.fd;
        d[w].flags = row.flags;
        d[w].kind = row.kind;
        d[w]._pad = 0;
        // SAFETY: row.path is a file_t.path field (fixed 128-byte array) or
        // NULL; cstr_into bounds the scan to PI_PATH_MAX-1 either way.
        unsafe { cstr_into(&mut d[w].path, row.path) };
        w += 1;
    }
    w as i32
}

#[no_mangle]
pub extern "C" fn svc_build_rs(src: *const SvcSrc, n: u32,
                               out: *mut SvcInfo, cap: u32) -> i32 {
    if src.is_null() || out.is_null() {
        return -1;
    }
    if cap == 0 || n == 0 {
        return 0;
    }
    // SAFETY: as handles_build_rs above.
    let s: &[SvcSrc] = unsafe { core::slice::from_raw_parts(src, n as usize) };
    let d: &mut [SvcInfo] = unsafe { core::slice::from_raw_parts_mut(out, cap as usize) };
    let mut w = 0usize;
    for row in s.iter() {
        if w >= d.len() {
            break;
        }
        d[w].running = row.running;
        d[w].autostart = row.autostart;
        d[w].perms = row.perms;
        d[w].pid = row.pid;
        // SAFETY: both are service_t fixed-size char fields or NULL.
        unsafe {
            cstr_into(&mut d[w].name, row.name);
            cstr_into(&mut d[w].account, row.account);
        }
        w += 1;
    }
    w as i32
}
