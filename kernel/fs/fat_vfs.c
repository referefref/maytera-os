// fat_vfs.c - VFS adapter for FAT files (Phase A1)
//
// Wraps the existing fat_file_t / fat_read / fat_write / fat_seek / fat_close
// API behind the struct file_ops vtable declared in vfs.h.
//
// Preserves the write-buffer kludge from the old sys_open path: a single
// 1 MB scratch buffer coalesces small appends on O_CREAT/O_TRUNC files and
// commits them via fat_write_file on release. Only one file may own the
// buffer at a time; additional O_CREAT opens fall back to direct fat_write
// (same as before the refactor).

#include "vfs.h"
#include "fat.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket on the caller-buffer copy

extern fat_fs_t g_fat_fs;

// --------------------------------------------------------------------------
// Write-buffer singleton
// --------------------------------------------------------------------------

#define FAT_WBUF_MAX (1 * 1024 * 1024)      // 1 MB, sized for NetHack save files

static uint8_t g_wbuf[FAT_WBUF_MAX];        // BSS
static file_t *g_wbuf_owner = NULL;          // owning struct file, or NULL
static uint32_t g_wbuf_len = 0;              // bytes currently buffered
static int g_wbuf_dirty = 0;                 // #695: buffered bytes not yet on
                                             // the medium. Distinct from
                                             // g_wbuf_len, which stays non-zero
                                             // after a flush because SEEK_END
                                             // reports it as the file size.
static char g_wbuf_path[256];                // path to flush to on release

// --------------------------------------------------------------------------
// fat-backed file_ops
// --------------------------------------------------------------------------

static int64_t fat_file_read(file_t *f, void *buf, size_t count) {
    fat_file_t *fp = (fat_file_t *)f->priv;
    if (!fp) return -1;
    return fat_read(fp, buf, count);
}

static int64_t fat_file_write(file_t *f, const void *buf, size_t count) {
    fat_file_t *fp = (fat_file_t *)f->priv;
    if (!fp) return -1;
    // If this file owns the write buffer, coalesce into the buffer.
    if (f == g_wbuf_owner) {
        uint32_t avail = FAT_WBUF_MAX - g_wbuf_len;
        uint32_t n = (count < avail) ? (uint32_t)count : avail;
        if (n > 0) {
            const uint8_t *src = (const uint8_t *)buf;
            // #19/#645: `src` is the caller's buffer (Ring-3 via sys_write).
            {   uaccess_ac_t __ac = uaccess_begin();
                for (uint32_t i = 0; i < n; i++) g_wbuf[g_wbuf_len + i] = src[i];
                uaccess_end(__ac); }
            g_wbuf_len += n;
            g_wbuf_dirty = 1;      // #695: new bytes are not on the medium yet
        }
        return (int64_t)n;
    }
    return fat_write(fp, buf, count);
}

static int64_t fat_file_seek(file_t *f, int64_t offset, int whence) {
    fat_file_t *fp = (fat_file_t *)f->priv;
    if (!fp) return -1;
    // Buffered-write files: SEEK_END returns the buffer length so userland
    // "append" logic sees the correct virtual size. Other seeks are no-ops
    // (matches pre-refactor behavior).
    if (f == g_wbuf_owner) {
        if (whence == SEEK_END) return (int64_t)g_wbuf_len;
        return 0;
    }
    uint32_t pos;
    switch (whence) {
        case SEEK_SET: pos = (uint32_t)offset; break;
        case SEEK_CUR: pos = fp->position + (uint32_t)offset; break;
        case SEEK_END: pos = fp->file_size + (uint32_t)offset; break;
        default: return -1;
    }
    return fat_seek(fp, pos);
}

// #695 Phase 1: THE FAT flush, and the only one. An fd that does NOT own the
// write buffer went straight to fat_write() -> blk_write(), which is
// write-through, so there is genuinely nothing pending for it and 0 is the
// truth rather than a stub.
//
// Idempotent: a successful commit clears g_wbuf_dirty, so a second fsync and
// the eventual release do nothing. g_wbuf_len is deliberately NOT cleared,
// because fat_file_seek() reports it as the file's virtual size to userland.
static int fat_file_flush(file_t *f) {
    if (f != g_wbuf_owner) return 0;
    if (!g_wbuf_dirty || g_wbuf_len == 0) return 0;
    // fat_write_file() DELETES the existing file before rewriting it, so on
    // failure the destination may be ABSENT or short and is never the previous
    // contents. See the sys_fsync() contract in proc/syscall.c.
    if (fat_write_file(&g_fat_fs, g_wbuf_path, g_wbuf, g_wbuf_len) != 0) return -1;
    g_wbuf_dirty = 0;
    return 0;
}

// #695 Phase 2: release() IS flush() + teardown, so FAT has one flush, not two.
static int fat_file_release(file_t *f) {
    fat_file_t *fp = (fat_file_t *)f->priv;

    int rc = fat_file_flush(f);

    // Give the singleton back unconditionally: a failed flush must not strand
    // the 1 MB buffer on a dead description and lock out every later writer.
    if (f == g_wbuf_owner) {
        g_wbuf_owner = NULL;
        g_wbuf_len = 0;
        g_wbuf_dirty = 0;
        g_wbuf_path[0] = '\0';
    }

    if (fp) {
        fat_close(fp);
        kfree(fp);
    }
    f->priv = NULL;
    return rc;
}

static const file_ops_t fat_file_ops = {
    .read    = fat_file_read,
    .write   = fat_file_write,
    .seek    = fat_file_seek,
    .ioctl   = NULL,
    .flush   = fat_file_flush,
    .release = fat_file_release,
    .poll    = NULL,
};

// --------------------------------------------------------------------------
// Public factory
// --------------------------------------------------------------------------

// Open a FAT path and return a struct file* wrapping it, or NULL on error.
// `flags` honors O_CREAT (0x40) and O_TRUNC (0x200); O_EXCL is not yet
// enforced (matches pre-refactor behavior).
//
// Caller receives one reference; drop via file_put() or install in an fd.
file_t *fat_vfs_open(const char *path, int flags) {
    if (!path) return NULL;

    fat_file_t *fp = (fat_file_t *)kmalloc(sizeof(fat_file_t));
    if (!fp) return NULL;

    int needs_wbuf = 0;

    if (fat_open(&g_fat_fs, path, fp) != 0) {
        if (flags & O_CREAT) {
            if (fat_create(&g_fat_fs, path) != 0) { kfree(fp); return NULL; }
            if (fat_open(&g_fat_fs, path, fp) != 0) { kfree(fp); return NULL; }
            needs_wbuf = 1;
        } else {
            kfree(fp);
            return NULL;
        }
    } else if (flags & O_TRUNC) {
        fat_close(fp);
        fat_delete(&g_fat_fs, path);
        if (fat_create(&g_fat_fs, path) != 0) { kfree(fp); return NULL; }
        if (fat_open(&g_fat_fs, path, fp) != 0) { kfree(fp); return NULL; }
        needs_wbuf = 1;
    }

    file_t *f = file_alloc(&fat_file_ops, fp, flags);
    if (!f) {
        fat_close(fp);
        kfree(fp);
        return NULL;
    }

    // #487/#349: record the path so Task Manager / Process Explorer can name
    // this handle. Bounded + always terminated (Rust seam under -DRUST_VFS_PATH).
    file_set_path(f, path);

    // Claim the write buffer if we created/truncated and nobody else has it.
    if (needs_wbuf && g_wbuf_owner == NULL) {
        g_wbuf_owner = f;
        g_wbuf_len = 0;
        g_wbuf_dirty = 0;   // #695: fat_create() already made the (empty) file
        int n = 0;
        while (path[n] && n < (int)sizeof(g_wbuf_path) - 1) {
            g_wbuf_path[n] = path[n];
            n++;
        }
        g_wbuf_path[n] = '\0';
    }

    return f;
}
