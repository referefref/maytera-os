// imgfile.c - seekable, bounded-RAM reader over a disk-image file (#196).
// See imgfile.h for why whole-image RAM loading had to go.
//
// LANGUAGE NOTE (standing Rust-first rule): this file is C because it is pure
// I/O plumbing entangled with C-only kernel APIs (ext2_read_file_range, the
// fat_file_t streaming handle, kmalloc, and the sleeping locks both take). The
// PARSING that sits on top of it, which is where untrusted image bytes are
// actually interpreted, is Rust (rustkern/iso9660.rs). That is the same split
// the tree already uses for #653 (pure formatting in Rust, filesystem worker in
// C) and it puts the memory-safety win exactly where the attacker-controlled
// input is.
#include "imgfile.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/ext2.h"

extern fat_fs_t g_fat_fs;
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

int imgfile_is_open(const imgfile_t *f) {
    return (f && f->kind != IMGF_KIND_NONE) ? 1 : 0;
}

uint64_t imgfile_size(const imgfile_t *f) {
    return (f && f->kind != IMGF_KIND_NONE) ? f->size : 0;
}

void imgfile_close(imgfile_t *f) {
    if (!f) return;
    if (f->kind == IMGF_KIND_FAT && f->fat) {
        fat_close((fat_file_t *)f->fat);
        kfree(f->fat);
    }
    if (f->cache) kfree(f->cache);
    memset(f, 0, sizeof(*f));
    f->kind = IMGF_KIND_NONE;
}

static void imgfile_cache_reset(imgfile_t *f) {
    for (int i = 0; i < IMGF_CACHE_SLOTS; i++) {
        f->tag[i] = IMGF_TAG_EMPTY;
        f->age[i] = 0;
    }
    f->clock = 0;
}

int imgfile_open(imgfile_t *f, const char *path) {
    if (!f || !path || !path[0]) return -1;
    memset(f, 0, sizeof(*f));
    f->kind = IMGF_KIND_NONE;

    // ext2 root first (the two-partition golden, #365). ext2_resolve_path
    // returns 0 when the path is absent, in which case we fall through to FAT
    // so an image on the FAT ESP still works.
    if (ext2_is_mounted()) {
        uint32_t ino = ext2_resolve_path(path);
        if (ino) {
            int is_dir = 1;
            if (ext2_get_is_dir(ino, &is_dir) == 0 && !is_dir) {
                // Size via a probe read at the end is unreliable; ext2_read_inode
                // gives it directly.
                ext2_inode_t in;
                if (ext2_read_inode(ino, &in) == 0) {
                    f->kind = IMGF_KIND_EXT2;
                    f->ino  = ino;
                    // i_size_lo + i_size_high (dir_acl doubles as the high half
                    // for regular files on ext2 with the large_file feature).
                    f->size = (uint64_t)in.i_size | ((uint64_t)in.i_size_high << 32);
                }
            }
        }
    }

    if (f->kind == IMGF_KIND_NONE && g_fat_fs.mounted) {
        fat_file_t *ff = (fat_file_t *)kmalloc(sizeof(fat_file_t));
        if (!ff) return -2;
        if (fat_open(&g_fat_fs, path, ff) == 0) {
            if (!ff->is_dir) {
                f->kind = IMGF_KIND_FAT;
                f->fat  = ff;
                f->size = (uint64_t)fat_size(ff);
            } else {
                fat_close(ff);
                kfree(ff);
            }
        } else {
            kfree(ff);
        }
    }

    if (f->kind == IMGF_KIND_NONE) return -3;

    f->cache = (uint8_t *)kmalloc((unsigned long)IMGF_CACHE_SLOTS * IMGF_CACHE_BLK);
    if (!f->cache) { imgfile_close(f); return -4; }
    imgfile_cache_reset(f);
    return 0;
}

// Read exactly one cache block (index `blk`) from the backing store into `dst`.
// Returns bytes read (may be short only at EOF), or negative on error.
static int64_t imgfile_backing_read(imgfile_t *f, uint64_t blk, uint8_t *dst) {
    uint64_t off = blk * (uint64_t)IMGF_CACHE_BLK;
    if (off >= f->size) return 0;
    uint64_t want = IMGF_CACHE_BLK;
    if (off + want > f->size) want = f->size - off;

    if (f->kind == IMGF_KIND_EXT2)
        return ext2_read_file_range(f->ino, off, want, dst);

    if (f->kind == IMGF_KIND_FAT) {
        fat_file_t *ff = (fat_file_t *)f->fat;
        // fat_seek takes a uint32_t position, so the FAT backing path is
        // limited to images below 4 GiB. Every CD/floppy image is, and the FAT
        // ESP itself is 256 MB, so this is not a live limit; reject rather than
        // silently truncate if it ever becomes one.
        if (off > 0xFFFFFFFFULL) return -1;
        if (fat_seek(ff, (uint32_t)off) != 0) return -1;
        int r = fat_read(ff, dst, (uint32_t)want);
        return (int64_t)r;
    }
    return -1;
}

// Find or fill the cache slot holding block `blk`. Returns the slot's buffer
// and its valid length in *vlen, or NULL on error.
static uint8_t *imgfile_block(imgfile_t *f, uint64_t blk, uint32_t *vlen) {
    int victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;

    for (int i = 0; i < IMGF_CACHE_SLOTS; i++) {
        if (f->tag[i] == blk) {
            f->age[i] = ++f->clock;
            f->hits++;
            uint64_t off = blk * (uint64_t)IMGF_CACHE_BLK;
            uint64_t rem = (off < f->size) ? (f->size - off) : 0;
            *vlen = (rem > IMGF_CACHE_BLK) ? IMGF_CACHE_BLK : (uint32_t)rem;
            return f->cache + (uint64_t)i * IMGF_CACHE_BLK;
        }
        if (f->tag[i] == IMGF_TAG_EMPTY) { victim = i; oldest = 0; }
        else if (oldest != 0 && f->age[i] < oldest) { oldest = f->age[i]; victim = i; }
    }

    uint8_t *buf = f->cache + (uint64_t)victim * IMGF_CACHE_BLK;
    int64_t got = imgfile_backing_read(f, blk, buf);
    if (got < 0) { f->tag[victim] = IMGF_TAG_EMPTY; return 0; }
    // A short read that is not at EOF means the backing store failed mid-block;
    // zero the tail so a caller can never observe stale bytes from the previous
    // tenant of this slot.
    if ((uint64_t)got < IMGF_CACHE_BLK)
        memset(buf + got, 0, IMGF_CACHE_BLK - (uint32_t)got);
    f->tag[victim] = blk;
    f->age[victim] = ++f->clock;
    f->misses++;
    *vlen = (uint32_t)got;
    return buf;
}

int64_t imgfile_read(imgfile_t *f, uint64_t off, uint64_t len, void *dst) {
    if (!f || f->kind == IMGF_KIND_NONE || !dst) return -1;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;

    uint8_t *out = (uint8_t *)dst;
    uint64_t done = 0;
    while (done < len) {
        uint64_t p    = off + done;
        uint64_t blk  = p / IMGF_CACHE_BLK;
        uint32_t bo   = (uint32_t)(p % IMGF_CACHE_BLK);
        uint32_t vlen = 0;
        uint8_t *b = imgfile_block(f, blk, &vlen);
        if (!b) return (done > 0) ? (int64_t)done : -1;
        if (bo >= vlen) break;                      // EOF inside this block
        uint32_t n = vlen - bo;
        if ((uint64_t)n > len - done) n = (uint32_t)(len - done);
        memcpy(out + done, b + bo, n);
        done += n;
    }
    return (int64_t)done;
}
