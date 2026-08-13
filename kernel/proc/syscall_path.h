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
// #99 cutover: true when ext2 is the root fs and `p` is a normal "/" path that
// should be served from ext2 (the UEFI ESP paths /boot, /EFI are never routed).
static inline int path_root_ext2(const char *p) {
    if (!g_root_ext2 || !p || p[0] != '/') return 0;
    if (path_is_ext2(p)) return 0;   // explicit /ext2 handled separately
    if (p[1]=='b'&&p[2]=='o'&&p[3]=='o'&&p[4]=='t'&&(p[5]=='/'||p[5]==0)) return 0;
    if (p[1]=='E'&&p[2]=='F'&&p[3]=='I'&&(p[4]=='/'||p[4]==0)) return 0;
    return 1;
}

#endif /* PROC_SYSCALL_PATH_H */
