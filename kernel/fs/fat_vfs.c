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

extern fat_fs_t g_fat_fs;

// --------------------------------------------------------------------------
// Write-buffer singleton
// --------------------------------------------------------------------------

#define FAT_WBUF_MAX (1 * 1024 * 1024)      // 1 MB, sized for NetHack save files

static uint8_t g_wbuf[FAT_WBUF_MAX];        // BSS
static file_t *g_wbuf_owner = NULL;          // owning struct file, or NULL
static uint32_t g_wbuf_len = 0;              // bytes currently buffered
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
            for (uint32_t i = 0; i < n; i++) g_wbuf[g_wbuf_len + i] = src[i];
            g_wbuf_len += n;
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

static void fat_file_release(file_t *f) {
    fat_file_t *fp = (fat_file_t *)f->priv;

    // Flush the write buffer if this file owned it.
    if (f == g_wbuf_owner) {
        if (g_wbuf_len > 0) {
            fat_write_file(&g_fat_fs, g_wbuf_path, g_wbuf, g_wbuf_len);
        }
        g_wbuf_owner = NULL;
        g_wbuf_len = 0;
        g_wbuf_path[0] = '\0';
    }

    if (fp) {
        fat_close(fp);
        kfree(fp);
    }
    f->priv = NULL;
}

static const file_ops_t fat_file_ops = {
    .read    = fat_file_read,
    .write   = fat_file_write,
    .seek    = fat_file_seek,
    .ioctl   = NULL,
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
        int n = 0;
        while (path[n] && n < (int)sizeof(g_wbuf_path) - 1) {
            g_wbuf_path[n] = path[n];
            n++;
        }
        g_wbuf_path[n] = '\0';
    }

    return f;
}
