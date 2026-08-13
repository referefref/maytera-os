// rustkern/fsperm.rs - #554 filesystem-aware permission/attribute support.
//
// New kernel code (new syscall + new routing logic), so Rust per the
// 2026-07-16 rule. Not a strangler port: there is no C twin, because this
// logic never existed before task #554. It calls into EXISTING, unchanged C
// (perms.c's perms_get/perms_chmod/perms_set, fs/ext2.c's ext2_get_is_dir,
// fs/fat.c's fat_get_attr_info/fat_set_readonly) rather than reimplementing
// any of it - those own their respective on-disk layouts and stay C.
//
// ===========================================================================
// THE DESIGN CRUX (see docs/UI_STYLE_GUIDE.md / CHANGELOG #554 for the fuller
// writeup). ext2 and FAT genuinely differ:
//   - ext2/POSIX paths: uid, gid, mode (rwxrwxrwx). This kernel's actual
//     enforcement of that model is perms.c, a path-keyed ACL overlay synced
//     to /CONFIG/PERMS.DB (NOT ext2's own on-disk inode, which has no
//     i_uid/i_gid field at all - see fs/ext2.h). sys_open() already calls
//     perms_check() against this table for every local path; this module
//     is the first thing to also let userland READ and WRITE it in a
//     filesystem-aware way instead of just being silently enforced.
//   - FAT (ESP: /boot, /EFI) paths: read-only, hidden, system, archive,
//     directory. No owner, no mode. This module NEVER fabricates ext2-style
//     uid/gid/mode for a FAT path (fs_type reports which model applies, and
//     the ext2 fields are simply left at 0/unset when fs_type==1); chown on
//     a FAT path is refused outright (no ownership concept to change), and
//     chmod on a FAT path honors ONLY the one genuine equivalent that
//     exists - the owner-write bit maps to the real on-disk
//     FAT_ATTR_READ_ONLY bit. Every other requested mode bit is silently
//     ignored at this layer (the terminal's chmod/chown commands are the
//     ones responsible for telling the user that, not this routing layer).
//
// ROUTING RULE DUPLICATION (known, accepted): "is this path ext2/POSIX or
// genuine FAT" is decided by path_is_ext2()/path_root_ext2() in
// proc/syscall.c (~line 1774/1810) and, independently, by fat_path_on_ext2()
// in fs/fat.c (~line 98). Both are `static` C functions with no external
// linkage, so they cannot be called from here; path_is_posix_perms() below is
// a THIRD copy of the identical rule (an explicit "/ext2" mount is always
// POSIX; otherwise, when g_root_ext2 is set, everything except "/boot" and
// "/EFI" is POSIX; when g_root_ext2 is clear, everything is FAT). If that
// routing rule ever changes, all three copies must move together.
// ===========================================================================

extern "C" {
    fn perms_get(path: *const u8, uid: *mut u32, gid: *mut u32, mode: *mut u16) -> i32;
    fn perms_chmod(path: *const u8, caller_uid: u32, mode: u16) -> i32;
    fn perms_set(path: *const u8, uid: u32, gid: u32, mode: u16);
    fn ext2_resolve_path(path: *const u8) -> u32;
    fn ext2_get_is_dir(ino: u32, is_dir_out: *mut i32) -> i32;
    fn fat_get_attr_info(path: *const u8, attr_out: *mut u8, is_dir_out: *mut i32) -> i32;
    fn fat_set_readonly(path: *const u8, readonly: i32) -> i32;
    static g_root_ext2: i32;
}

// Bounded NUL scan: by the time any of these #[no_mangle] entry points run,
// the syscall dispatcher's argtab (see argtab.rs) has already validated that
// `path` is readable, user-owned, and NUL-terminated within PATH_MAX (4096)
// bytes for every syscall that declares an `s(PATH_MAX)` argument, which is
// true of all three syscalls that reach this module (SYS_CHMOD, SYS_CHOWN,
// SYS_FS_PERM_INFO). This is a defensive backstop, not the primary check.
unsafe fn cstr_slice<'a>(p: *const u8) -> &'a [u8] {
    const MAX: usize = 4096;
    let mut n = 0usize;
    while n < MAX && unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    unsafe { core::slice::from_raw_parts(p, n) }
}

fn starts_with(hay: &[u8], needle: &[u8]) -> bool {
    hay.len() >= needle.len() && &hay[..needle.len()] == needle
}

/// true => ext2/POSIX-perms path (perms.c is authoritative); false => genuine
/// FAT (ESP). See the module header for why this rule is duplicated here.
fn path_is_posix_perms(path: &[u8]) -> bool {
    if path.is_empty() || path[0] != b'/' {
        return false;
    }
    // Explicit "/ext2" or "/ext2/..." mount: always POSIX, regardless of
    // g_root_ext2 (matches path_is_ext2() in proc/syscall.c).
    if starts_with(path, b"/ext2") && (path.len() == 5 || path[5] == b'/') {
        return true;
    }
    let root_ext2 = unsafe { g_root_ext2 } != 0;
    if !root_ext2 {
        return false; // legacy FAT-root state: everything is FAT.
    }
    if starts_with(path, b"/boot") && (path.len() == 5 || path[5] == b'/') {
        return false;
    }
    if starts_with(path, b"/EFI") && (path.len() == 4 || path[4] == b'/') {
        return false;
    }
    true
}

// #[repr(C)] mirror of k_fsperm_info_t in proc/syscall.c (sizeof-locked there
// via _Static_assert, per the codebase's existing k_stat_t convention).
#[repr(C)]
pub struct FsPermInfo {
    pub fs_type: u8,        // 0 = ext2/POSIX (perms.c), 1 = FAT (ESP), 2 = other (SMB/NFS)
    pub is_dir: u8,
    pub has_perm_entry: u8, // fs_type==0 only: 1 = explicit perms.c entry, 0 = reporting the perms_check() default
    pub fat_attr: u8,       // fs_type==1 only: raw on-disk FAT_ATTR_* byte
    pub mode: u16,          // fs_type==0 only: rwxrwxrwx bits
    pub _reserved: u16,
    pub uid: u32,           // fs_type==0 only
    pub gid: u32,           // fs_type==0 only
}
const _: () = assert!(core::mem::size_of::<FsPermInfo>() == 16);

/// SYS_FS_PERM_INFO handler body. `path` and `out` are pre-validated user
/// pointers (see cstr_slice's doc comment); `smb_or_nfs` is computed by the
/// thin C wrapper sys_fs_perm_info() in proc/syscall.c, which already has
/// path_is_smb()/path_is_nfs() in scope (those, unlike the ext2/FAT routing
/// pair, are non-static and could be called from here too, but keeping the
/// one C-only classification in C avoids a fourth mirrored predicate for no
/// benefit - SMB/NFS enforce access server-side and were never going to gain
/// a local permission model here).
#[no_mangle]
pub extern "C" fn rk_fs_perm_info(path: *const u8, smb_or_nfs: i32, out: *mut FsPermInfo) -> i64 {
    if path.is_null() || out.is_null() {
        return -1;
    }
    let o = unsafe { &mut *out };
    o.fs_type = 0;
    o.is_dir = 0;
    o.has_perm_entry = 0;
    o.fat_attr = 0;
    o.mode = 0;
    o._reserved = 0;
    o.uid = 0;
    o.gid = 0;

    if smb_or_nfs != 0 {
        o.fs_type = 2;
        return 0;
    }

    let pslice = unsafe { cstr_slice(path) };
    if path_is_posix_perms(pslice) {
        o.fs_type = 0;
        let ino = unsafe { ext2_resolve_path(path) };
        if ino != 0 {
            let mut isdir: i32 = 0;
            if unsafe { ext2_get_is_dir(ino, &mut isdir) } == 0 {
                o.is_dir = if isdir != 0 { 1 } else { 0 };
            }
        }
        let mut uid: u32 = 0;
        let mut gid: u32 = 0;
        let mut mode: u16 = 0;
        if unsafe { perms_get(path, &mut uid, &mut gid, &mut mode) } == 0 {
            o.has_perm_entry = 1;
            o.uid = uid;
            o.gid = gid;
            o.mode = mode & 0o777;
        } else {
            // Mirrors perms_check()'s own default (root:root, 0755/0644) so a
            // Properties dialog shows the SAME permissions that would actually
            // be enforced on open(), not an unrelated guess.
            o.has_perm_entry = 0;
            o.uid = 0;
            o.gid = 0;
            o.mode = if o.is_dir != 0 { 0o755 } else { 0o644 };
        }
        return 0;
    }

    // Genuine FAT (ESP) path.
    o.fs_type = 1;
    let mut attr: u8 = 0;
    let mut isdir: i32 = 0;
    if unsafe { fat_get_attr_info(path, &mut attr, &mut isdir) } != 0 {
        return -1;
    }
    o.fat_attr = attr;
    o.is_dir = if isdir != 0 { 1 } else { 0 };
    0
}

/// SYS_CHMOD routing. `euid` is the caller's effective uid (proc_current()->euid,
/// read by the C wrapper before crossing into Rust).
#[no_mangle]
pub extern "C" fn rk_chmod_route(path: *const u8, mode: u16, euid: u32) -> i64 {
    if path.is_null() {
        return -1;
    }
    let pslice = unsafe { cstr_slice(path) };
    if path_is_posix_perms(pslice) {
        return unsafe { perms_chmod(path, euid, mode) } as i64;
    }
    // Genuine FAT: the only real equivalent is the write bit -> the on-disk
    // read-only attribute. Every other bit (exec, group/other, setuid/gid) has
    // no FAT meaning and is intentionally dropped here rather than faked.
    //
    // #745: ROOT ONLY, and this branch used to consult `euid` NOWHERE.
    //
    // `euid` was read by the C wrapper, passed in, and then used only by the
    // POSIX branch above. So any Ring-3 process at any uid could toggle the
    // on-disk read-only attribute of any path on the FAT ESP: /BOOT/*, /EFI/*,
    // /KERNEL.ELF. That directly contradicts pkg_path_is_boot() in
    // proc/syscall.c, which refuses Ring-3 writes to exactly those paths for
    // EVERY caller including root, on the grounds that there is no legitimate
    // Ring-3 write to the boot medium. Metadata is a write.
    //
    // It is also strictly worse than the POSIX branch it sits next to, which
    // requires an existing entry AND ownership. FAT carries no owner, so there
    // is no ownership test available to make; root-only is the only rule that
    // can be stated honestly here, and it matches rk_chown_route's rule on the
    // same filesystem for the same reason.
    if euid != 0 {
        return -1; // EPERM
    }
    let want_writable = (mode & 0o200) != 0;
    let r = unsafe { fat_set_readonly(path, if want_writable { 0 } else { 1 }) };
    if r == 0 { 0 } else { -1 }
}

/// SYS_CHOWN routing. FAT has no ownership concept; refuse rather than accept
/// and silently drop it (task #554: "do not silently pretend to succeed").
#[no_mangle]
pub extern "C" fn rk_chown_route(path: *const u8, uid: u32, gid: u32, euid: u32) -> i64 {
    if path.is_null() {
        return -1;
    }
    if euid != 0 {
        return -1; // EPERM: matches the pre-existing sys_chown rule (root only)
    }
    let pslice = unsafe { cstr_slice(path) };
    if !path_is_posix_perms(pslice) {
        return -1; // FAT: no owner to change
    }
    // Preserve the existing mode bits (chown must not reset permissions);
    // uid/gid are about to be overwritten so their prior values are unused.
    let mut old_uid: u32 = 0;
    let mut old_gid: u32 = 0;
    let mut cur_mode: u16 = 0;
    let has = unsafe { perms_get(path, &mut old_uid, &mut old_gid, &mut cur_mode) } == 0;
    let mode = if has { cur_mode } else { 0o755 };
    unsafe { perms_set(path, uid, gid, mode) };
    0
}
