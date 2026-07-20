// ext2_vfs.c - minimal VFS file backing for ext2 files.
//
// Used by shell I/O redirection: sys_spawn_redir installs one of these as a
// child's stdin/stdout so `cmd > file` / `cmd < file` work. The whole file
// lives in a kernel buffer - reads serve from it, writes grow it, and on the
// final release a dirty buffer is committed to the ext2 volume via
// ext2_write_file() (which is DMA-fast and rollback-safe). This keeps the
// fd uniform (a struct file_t in proc->fds[]) so dup/inherit/close all work,
// unlike the legacy global e2fd table which is keyed by fd number.

#include "vfs.h"
#include "ext2.h"
#include "../string.h"

extern void *kmalloc(unsigned long size);
extern void  kfree(void *ptr);

typedef struct {
    char     path[256];
    uint8_t *buf;
    uint32_t size;
    uint32_t cap;
    uint32_t pos;
    int      dirty;
    int      writable;
} ext2_vfile_t;

static int64_t e2v_read(file_t *f, void *buf, size_t count) {
    ext2_vfile_t *v = (ext2_vfile_t *)f->priv;
    if (!v) return -1;
    if (v->pos >= v->size) return 0;
    uint32_t avail = v->size - v->pos;
    uint32_t n = (count < (size_t)avail) ? (uint32_t)count : avail;
    memcpy(buf, v->buf + v->pos, n);
    v->pos += n;
    return (int64_t)n;
}

static int e2v_grow(ext2_vfile_t *v, uint32_t need) {
    if (need <= v->cap) return 0;
    uint32_t nc = v->cap ? v->cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t *nb = (uint8_t *)kmalloc(nc);
    if (!nb) return -1;
    if (v->buf) { memcpy(nb, v->buf, v->size); kfree(v->buf); }
    v->buf = nb;
    v->cap = nc;
    return 0;
}

static int64_t e2v_write(file_t *f, const void *buf, size_t count) {
    ext2_vfile_t *v = (ext2_vfile_t *)f->priv;
    if (!v || !v->writable) return -1;
    if (count == 0) return 0;
    if (e2v_grow(v, v->pos + (uint32_t)count) != 0) return -1;
    memcpy(v->buf + v->pos, buf, count);
    v->pos += (uint32_t)count;
    if (v->pos > v->size) v->size = v->pos;
    v->dirty = 1;
    return (int64_t)count;
}

static int64_t e2v_seek(file_t *f, int64_t offset, int whence) {
    ext2_vfile_t *v = (ext2_vfile_t *)f->priv;
    if (!v) return -1;
    int64_t np;
    if (whence == SEEK_SET)      np = offset;
    else if (whence == SEEK_CUR) np = (int64_t)v->pos + offset;
    else if (whence == SEEK_END) np = (int64_t)v->size + offset;
    else return -1;
    if (np < 0) np = 0;
    v->pos = (uint32_t)np;
    return np;
}

static void e2v_release(file_t *f) {
    ext2_vfile_t *v = (ext2_vfile_t *)f->priv;
    if (!v) return;
    if (v->dirty && v->writable) {
        ext2_write_file(v->path, v->buf ? (const void *)v->buf : (const void *)"", v->size);
    }
    if (v->buf) kfree(v->buf);
    kfree(v);
    f->priv = NULL;
}

static const file_ops_t ext2_vfs_ops = {
    .read    = e2v_read,
    .write   = e2v_write,
    .seek    = e2v_seek,
    .ioctl   = NULL,
    .release = e2v_release,
    .poll    = NULL,
};

// Open `path` on the ext2 volume as a struct file_t. Honors O_TRUNC (start
// empty), O_CREAT (create on flush if absent), O_APPEND (seek to end), and
// loads existing content otherwise. Returns NULL on error.
file_t *ext2_vfs_open(const char *path, int flags) {
    if (!path) return NULL;
    ext2_vfile_t *v = (ext2_vfile_t *)kmalloc(sizeof(ext2_vfile_t));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    { int i = 0; while (path[i] && i < 255) { v->path[i] = path[i]; i++; } v->path[i] = 0; }
    v->writable = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) ? 1 : 0;

    if (!(flags & O_TRUNC)) {
        uint32_t sz = 0;
        void *data = ext2_read_whole(path, &sz);
        if (data) {
            v->buf = (uint8_t *)data;
            v->size = sz;
            v->cap = sz;
        } else if (!(flags & O_CREAT)) {
            kfree(v);
            return NULL;   // not found and not creating
        }
    }
    if (flags & O_APPEND) v->pos = v->size;

    file_t *f = file_alloc(&ext2_vfs_ops, v, flags);
    if (!f) { if (v->buf) kfree(v->buf); kfree(v); return NULL; }

    // #487/#349: record the path so Task Manager / Process Explorer can name
    // this handle. Bounded + always terminated (Rust seam under -DRUST_VFS_PATH).
    file_set_path(f, path);

    // A freshly created/truncated file is marked dirty so an (even empty) file
    // is committed to disk on release.
    if ((flags & O_TRUNC) || ((flags & O_CREAT) && v->size == 0)) v->dirty = 1;
    return f;
}
