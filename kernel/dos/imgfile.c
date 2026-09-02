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
#include "../fs/blockdev.h"   // #740: raw block-device backing

extern fat_fs_t g_fat_fs;
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

// #740: parse exactly `n` hex digits into *out. Returns the address just past
// them, or NULL if any of the n characters is not a hex digit. Fixed-width on
// purpose: a variable-width parse would have to decide what terminates a field,
// and the producer (dos/usbvol.c) is the only writer of this string, so there
// is no reason to accept anything it does not emit.
static const char *hex_n(const char *s, int n, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9')      d = (uint64_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return s + n;
}

// #740: recognise and decode IMGF_BLKDEV_PREFIX. Returns 1 and fills the handle
// (kind/base/size/ch/dr) if `path` is a well-formed synthetic device path, 0 if
// it is an ordinary path that the file backings should handle.
//
// A malformed synthetic path returns 0, NOT an error, so it falls through to
// the filesystem lookup and fails there as a missing file. There is no caller
// that can produce one except a bug in dos/usbvol.c, and a confusing "no such
// file" is a better failure than a special error code nobody handles.
static int blkdev_parse(imgfile_t *f, const char *path) {
    const char *p = path;
    for (const char *q = IMGF_BLKDEV_PREFIX; *q; q++, p++)
        if (*p != *q) return 0;

    uint64_t ch, dr, base, len;
    if (!(p = hex_n(p, 2, &ch)))  return 0;
    if (*p++ != ':')              return 0;
    if (!(p = hex_n(p, 2, &dr)))  return 0;
    if (*p++ != ':')              return 0;
    if (!(p = hex_n(p, 16, &base))) return 0;
    if (*p++ != ':')              return 0;
    if (!(p = hex_n(p, 16, &len)))  return 0;
    if (*p != '\0')               return 0;

    // The cache reads whole IMGF_CACHE_BLK blocks and turns each into a whole
    // number of 512-byte sectors, so a base that is not sector-aligned would
    // silently shear every read. rustkern/usbvol.rs enforces 1 MiB alignment
    // on the producing side; this is the consuming side refusing to trust it.
    if (len == 0 || (base & 511u) != 0) return 0;

    f->kind = IMGF_KIND_BLKDEV;
    f->base = base;
    f->size = len;
    f->ch   = (uint8_t)ch;
    f->dr   = (uint8_t)dr;
    return 1;
}

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
    if (f->cache_raw) kfree(f->cache_raw);
    memset(f, 0, sizeof(*f));
    f->kind = IMGF_KIND_NONE;
}

// [no-ticket] rustkern/imgra.rs owns the readahead policy and the victim-run
// choice. The struct is shared by value, so its width is locked here the same
// way every other rustkern FFI struct in this tree is.
_Static_assert(sizeof(imgra_stream_t) == 16, "[no-ticket] FFI: ImgRaStream is u64 + two u32");
_Static_assert(sizeof(imgra_t) == 104,
               "[no-ticket] FFI: ImgRa is four streams, two u32 and four u64");
extern void imgra_reset_rs(imgra_t *ra);
extern uint32_t imgra_plan_rs(imgra_t *ra, uint64_t blk, uint32_t max_win, uint64_t avail);
extern void imgra_abort_rs(imgra_t *ra);
extern uint32_t imgra_victim_rs(const uint32_t *ages, uint32_t nslots, uint32_t run,
                                uint32_t align);

// The device command size fs/blockdev.c can reach is bounded by the 64 KB xHCI
// TRB boundary, so a readahead span that starts on a 64 KB boundary costs
// strictly fewer round trips than the same span starting anywhere else. Align
// the cache base so slot 0 is on one; the aligned victim search then keeps
// every full-window span aligned too.
#define IMGF_CACHE_ALIGN 65536ull

// [no-ticket] RUNTIME OFF SWITCH, and it is not decoration.
//
// It is what makes the before/after numbers in this ticket a CONTROLLED
// experiment rather than two builds compared: dos/cdbench.c runs the same file,
// on the same medium, in the same boot, over two cold regions, with only this
// flag differing. Two kernels compared across two boots would have carried
// every difference between those boots into the result, and this project has
// already published one number that turned out to be the host's load (#122).
//
// It is also the escape hatch. Readahead changes what the device is asked for
// on the machine the OS BOOTS FROM. If it ever misbehaves on a particular
// stick, /CONFIG/CDRAOFF.CFG restores the old one-block-per-miss behaviour
// exactly, with no rebuild.
static int g_ra_off = 0;
void imgfile_readahead_set_disabled(int off) { g_ra_off = off ? 1 : 0; }
int  imgfile_readahead_disabled(void) { return g_ra_off; }

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

    // #740: the raw block-device backing is decided FIRST, because it is the
    // one form that must never be looked up as a filename. On success it sets
    // f->kind, and the two filesystem lookups below are both already guarded on
    // f->kind still being NONE, so they simply do not run.
    blkdev_parse(f, path);

    // ext2 root first (the two-partition golden, #365). ext2_resolve_path
    // returns 0 when the path is absent, in which case we fall through to FAT
    // so an image on the FAT ESP still works.
    if (f->kind == IMGF_KIND_NONE && ext2_is_mounted()) {
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

    // Over-allocate by one alignment unit and align the usable base up. The raw
    // pointer is kept because it, not the aligned one, is what kfree() wants.
    unsigned long need = (unsigned long)IMGF_CACHE_SLOTS * IMGF_CACHE_BLK
                       + (unsigned long)IMGF_CACHE_ALIGN;
    f->cache_raw = (uint8_t *)kmalloc(need);
    if (!f->cache_raw) { imgfile_close(f); return -4; }
    uint64_t a = (uint64_t)f->cache_raw;
    uint64_t aligned = (a + (IMGF_CACHE_ALIGN - 1)) & ~(IMGF_CACHE_ALIGN - 1);
    f->cache = (uint8_t *)aligned;
    imgra_reset_rs(&f->ra);
    imgfile_cache_reset(f);
    return 0;
}

// Read `nblk` CONSECUTIVE cache blocks starting at index `blk` from the backing
// store into `dst`, which must be nblk * IMGF_CACHE_BLK bytes of contiguous
// buffer. Returns bytes read (short only at EOF), or negative on error.
//
// [no-ticket] `nblk` is the whole point: ONE backing read of 64 KiB instead of
// eight of 8 KiB is eight fewer trips through blk_read() and, on a USB root,
// six fewer SCSI commands. The bytes are identical; the round trips are not.
static int64_t imgfile_backing_read(imgfile_t *f, uint64_t blk, uint32_t nblk,
                                    uint8_t *dst) {
    if (nblk == 0) return 0;
    uint64_t off = blk * (uint64_t)IMGF_CACHE_BLK;
    if (off >= f->size) return 0;
    uint64_t want = (uint64_t)nblk * IMGF_CACHE_BLK;
    if (off + want > f->size) want = f->size - off;

    if (f->kind == IMGF_KIND_EXT2)
        return ext2_read_file_range(f->ino, off, want, dst);

    // #740: raw device range. f->base is 512-aligned (checked at open) and `off`
    // is always a multiple of IMGF_CACHE_BLK, so base + off is sector-aligned
    // and the read needs no bounce buffer. `want` is nblk * IMGF_CACHE_BLK
    // except in the final partial block, so nsec is at most
    // nblk * IMGF_CACHE_BLK / 512 and nsec * 512 never exceeds the
    // nblk-slot span `dst` points into, which the caller guarantees is
    // contiguous (the slots are one allocation and the run is consecutive).
    //
    // Reading only the sectors `want` needs, rather than a full block every
    // time, is what keeps the LAST block of a volume that ends at the end of
    // the medium from running off the end of it.
    if (f->kind == IMGF_KIND_BLKDEV) {
        uint64_t dev_off = f->base + off;
        uint32_t nsec    = (uint32_t)((want + 511u) / 512u);
        int got = blk_read(f->ch, f->dr, dev_off / 512u, nsec, dst);
        if (got != (int)nsec) return -1;
        return (int64_t)want;
    }

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
    for (int i = 0; i < IMGF_CACHE_SLOTS; i++) {
        if (f->tag[i] == blk) {
            f->age[i] = ++f->clock;
            f->hits++;
            uint64_t off = blk * (uint64_t)IMGF_CACHE_BLK;
            uint64_t rem = (off < f->size) ? (f->size - off) : 0;
            *vlen = (rem > IMGF_CACHE_BLK) ? IMGF_CACHE_BLK : (uint32_t)rem;
            return f->cache + (uint64_t)i * IMGF_CACHE_BLK;
        }
    }

    // [no-ticket] MISS. How many CONSECUTIVE blocks is it worth fetching?
    // rustkern/imgra.rs answers, from whether this caller is streaming. A cold
    // or random caller gets 1 and pays exactly what it paid before.
    uint64_t nblocks_total = (f->size + IMGF_CACHE_BLK - 1) / IMGF_CACHE_BLK;
    uint64_t avail = (blk < nblocks_total) ? (nblocks_total - blk) : 0;
    uint32_t run = imgra_plan_rs(&f->ra, blk, g_ra_off ? 1u : IMGF_RA_MAX, avail);
    if (run == 0) run = 1;
    if (run > IMGF_CACHE_SLOTS) run = IMGF_CACHE_SLOTS;

    // A run of CONSECUTIVE slots is one contiguous buffer, which is what makes
    // the whole readahead a single backing read. Aligning the start to the run
    // length keeps the span on a 64 KB boundary (see imgra_victim_rs).
    uint32_t victim = imgra_victim_rs(f->age, (uint32_t)IMGF_CACHE_SLOTS, run, run);
    if (victim + run > IMGF_CACHE_SLOTS) victim = 0;   // defensive; Rust guarantees it

    // Slots about to be overwritten stop describing anything BEFORE the read,
    // so a failure part-way cannot leave a tag pointing at a half-written slot.
    for (uint32_t k = 0; k < run; k++) {
        f->tag[victim + k] = IMGF_TAG_EMPTY;
        f->age[victim + k] = 0;
    }
    // Any OTHER slot already holding one of the blocks we are about to install
    // is dropped, so no block index is ever tagged in two places.
    for (int i = 0; i < IMGF_CACHE_SLOTS; i++) {
        if ((uint32_t)i >= victim && (uint32_t)i < victim + run) continue;
        if (f->tag[i] != IMGF_TAG_EMPTY && f->tag[i] >= blk && f->tag[i] < blk + run) {
            f->tag[i] = IMGF_TAG_EMPTY;
            f->age[i] = 0;
        }
    }

    uint8_t *buf = f->cache + (uint64_t)victim * IMGF_CACHE_BLK;
    int64_t got = imgfile_backing_read(f, blk, run, buf);
    if (got <= 0) {
        // Nothing was installed, so the prediction this plan made has to be
        // withdrawn or the next miss is judged sequential against absent blocks.
        imgra_abort_rs(&f->ra);
        return 0;
    }

    // Tag only the blocks the read actually covered. A short read at EOF covers
    // fewer; a short read that is NOT at EOF means the backing store failed
    // mid-run, and the same rule applies, so the untouched slots stay EMPTY
    // rather than advertising bytes nobody wrote.
    uint32_t full = (uint32_t)((uint64_t)got / IMGF_CACHE_BLK);
    uint32_t tail = (uint32_t)((uint64_t)got % IMGF_CACHE_BLK);
    if (full > run) { full = run; tail = 0; }
    for (uint32_t k = 0; k < full; k++) {
        f->tag[victim + k] = blk + k;
        f->age[victim + k] = ++f->clock;
    }
    if (tail && full < run) {
        // The last block is partial: zero its tail so a caller can never observe
        // stale bytes from this slot's previous tenant.
        memset(buf + (uint64_t)full * IMGF_CACHE_BLK + tail, 0,
               IMGF_CACHE_BLK - tail);
        f->tag[victim + full] = blk + full;
        f->age[victim + full] = ++f->clock;
        full++;
    }
    if (full == 0) { imgra_abort_rs(&f->ra); return 0; }
    f->misses++;

    // `blk` itself is always the first block of the run, so the slot to return
    // is the one we just filled at `victim`.
    uint64_t off0 = blk * (uint64_t)IMGF_CACHE_BLK;
    uint64_t rem0 = (off0 < f->size) ? (f->size - off0) : 0;
    *vlen = (rem0 > IMGF_CACHE_BLK) ? IMGF_CACHE_BLK : (uint32_t)rem0;
    if ((uint64_t)*vlen > (uint64_t)got) *vlen = (uint32_t)got;
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
