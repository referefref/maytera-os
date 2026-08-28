// syscall_path.h - path plumbing shared by the syscall dispatcher and the
// legacy fd layer (#746b).
//
// WHY THIS HEADER EXISTS. These helpers were file-static in proc/syscall.c and
// are used on BOTH sides of the fd-layer extraction: 17 sites outside the fd
// layer (open_redir_file, sc_meta_permit, sys_stat_path, sys_mkdir, sys_rmdir,
// sys_unlink, sys_rename, sc_path_is_dir, sys_chdir, sys_fs_perm_info) and the
// open/read/write/readdir family that moved to fdlayer.c.
//
// WHY static inline AND NOT extern. Turning them into extern functions would
// take away gcc's ability to inline them into the callers that STAYED, which
// would change the machine code of six functions this extraction is not
// supposed to touch. A `static inline` definition keeps the definition visible
// at every call site exactly as it was, so the move stays a move.
//
// Every line below the marker is byte-for-byte the code that was in
// proc/syscall.c; only this preamble, the guard and the includes are new.
#ifndef PROC_SYSCALL_PATH_H
#define PROC_SYSCALL_PATH_H

#include "../types.h"
#include "../string.h"
#include "../fs/ext2.h"
#include "../net/smb.h"
#include "../net/nfs.h"
#include "../security/validate.h"   // strncpy_from_user (#58)

// ---- moved verbatim from proc/syscall.c ------------------------------------
#define SC_PATH_MAX 1024
// #676: the parent directory of a path. There were three byte-identical inline
// copies of this (sys_mkdir, sys_rmdir, sys_unlink) and #676 needed a fourth, so
// it is a function now rather than a fourth fork of the same eight lines.
// A path with no slash after the root, and the root itself, both yield "/".
static inline void sc_parent_of(const char *path, char *out, unsigned long cap) {
    unsigned long i = 0;
    while (path[i] && i < cap - 1) { out[i] = path[i]; i++; }
    out[i] = '\0';
    char *last = strrchr(out, '/');
    if (last && last != out) {
        *last = '\0';
    } else {
        out[0] = '/'; out[1] = '\0';
    }
}

static inline int path_is_ext2(const char *p) {
    return p && p[0]=='/' && p[1]=='e' && p[2]=='x' && p[3]=='t' && p[4]=='2' &&
           (p[5]=='\0' || p[5]=='/');
}
static inline int path_is_smb(const char *p) {
    return smb_vfs_is_smb_path(p);
}
// #317 pass 4: NFS exports use the same smbfd[] table (read/write/seek are
// fs-agnostic, operating on the cached rbuf/wbuf); only mount/stat/opendir/
// readdir/closedir/upload differ and branch on s->is_nfs.
static inline int path_is_nfs(const char *p) {
    return nfs_vfs_is_nfs_path(p) ? 1 : 0;
}
// "/ext2" -> "/", "/ext2/a/b" -> "/a/b"
static inline const char *ext2_relpath(const char *p) {
    const char *r = p + 5;
    return (*r == '\0') ? "/" : r;
}
// #193: is `p` inside a drive letter that has a disk IMAGE mounted on it right
// now? Defined ONCE, in fs/fat.c, over the Rust path split in
// rustkern/drvmap.rs. Non-blocking: a bounded string compare plus one
// spinlock-protected read of the mount table. It performs no I/O and cannot
// sleep, which is why it is safe to put on a predicate every path syscall
// evaluates.
int path_img_shadows(const char *p);

// #99 cutover: true when ext2 is the root fs and `p` is a normal "/" path that
// should be served from ext2 (the UEFI ESP paths /boot, /EFI are never routed).
static inline int path_root_ext2(const char *p) {
    if (!g_root_ext2 || !p || p[0] != '/') return 0;
    if (path_is_ext2(p)) return 0;   // explicit /ext2 handled separately
    if (p[1]=='b'&&p[2]=='o'&&p[3]=='o'&&p[4]=='t'&&(p[5]=='/'||p[5]==0)) return 0;
    if (p[1]=='E'&&p[2]=='F'&&p[3]=='I'&&(p[4]=='/'||p[4]==0)) return 0;
    // #193 THE ORDERING FIX, AT THE PREDICATE RATHER THAN AT ONE CALLER.
    //
    // A mounted disk image OWNS its drive's subtree. fat_open() has known that
    // since #196 and is explicit about it ("a mounted disc is authoritative for
    // its own drive letter"), but every syscall-layer caller reached its own
    // ext2-root branch FIRST and never got as far as fat_open(): sys_open_k(),
    // sys_stat_path(), sys_utimens(), sc_path_is_dir() (chdir) and the VFS
    // open all ask THIS predicate. So a path present both in the image and in
    // the folder underneath resolved to the FOLDER through every one of them,
    // while the DOS guest calling fat_open() directly got the IMAGE. Two
    // answers for one path, and the wrong one arrives as data, not as an error.
    //
    // Fixing sys_open() alone would have recreated #58 in a new shape: open()
    // seeing the disc while stat() still saw the folder. The five sites share
    // exactly one predicate, so the decision belongs in the predicate.
    //
    // Returning 0 here does NOT mean "not found". It means "ext2-root is not
    // the answer for this path", and the caller then falls through to the
    // fat_* family, whose fat_open() does image-first with a hard miss. When
    // nothing is mounted path_img_shadows() is false and every branch below
    // and above is byte-for-byte the behaviour that shipped.
    if (path_img_shadows(p)) return 0;
    return 1;
}

// ===========================================================================
// #58: THE PATH CHOKEPOINT. Every Ring-3 path enters the kernel HERE.
//
// WHY A CHOKEPOINT AND NOT FIFTEEN PATCHES. Before this, each path syscall
// bounced its own path with its own strncpy_from_user and handed the result
// straight to the FAT/ext2 resolvers, which treat a name with no leading '/'
// as ROOT-relative. process_t::cwd was maintained correctly by sys_chdir and
// read by nobody. The result was not merely wrong, it was NON-UNIFORMLY wrong:
// sys_open() had a prepend-'/' branch for the ext2-root redirect and the other
// fourteen handlers did not, so open("X") and stat("X") could mean different
// files. A per-syscall fix would have preserved exactly that hazard, one
// handler at a time, which is why the rule is one funnel that they all use.
//
// THE ORDER MATTERS: BOUNCE FIRST, RESOLVE SECOND. Resolution reads the path
// repeatedly (scan for the terminator, join, canonicalize). Doing that against
// Ring-3 memory would be a TOCTOU window of exactly the shape #509 closed: a
// sibling thread can rewrite the string between the scan and the use, so the
// path that is CHECKED stops being the path that is OPENED. strncpy_from_user
// takes one atomic snapshot; everything after it works on kernel memory.
// ===========================================================================

// Resolve a path ALREADY in kernel memory, in place, against the caller's cwd.
// See rustkern/permpath.rs path_resolve_cwd_rs() for the semantics: absolute
// paths are untouched, relative ones are joined onto the cwd and canonicalized
// by the same canonicalizer perms_check() and sys_chdir() use.
// Returns 0, or -1 when the path cannot be resolved (never truncates).
static inline int sc_path_resolve(char *kbuf, unsigned long cap) {
    extern const char *proc_cwd(void);
    extern int path_resolve_cwd_rs(const char *cwd, char *buf, uint32_t cap);
    return (path_resolve_cwd_rs(proc_cwd(), kbuf, (uint32_t)cap) < 0) ? -1 : 0;
}

// Bounce a Ring-3 path into `dst` (cap bytes) AND resolve it against the
// calling process's working directory. This is the ONLY correct way for a
// syscall handler to obtain a path from userland.
//
// Returns 0 on success, -14 (EFAULT) on a NULL/bad user pointer or a mid-copy
// fault, and -1 on a path that will not resolve. Callers should PROPAGATE the
// value rather than collapsing it, so a fault stays distinguishable from a bad
// path.
static inline int sc_path_from_user(const char *usrc, char *dst, unsigned long cap) {
    if (!usrc || cap == 0) return -14;
    if (strncpy_from_user(dst, usrc, cap) < 0) return -14;
    return sc_path_resolve(dst, cap);
}

#endif /* PROC_SYSCALL_PATH_H */
