// diskimg.c - removable disk-image mount/eject (#196). See diskimg.h.
//
// ISO images are STREAMED through an imgfile_t (256 KiB cache) rather than
// loaded whole into RAM, because the discs this exists for are 653 MB and
// 677 MB. FAT12 floppies are still RAM-resident (a floppy is <= 2.88 MB).
//
// The ISO 9660 parsing itself lives in Rust (rustkern/iso9660.rs) behind the
// iso_vd_parse / iso_dirrec_at / iso_name_decode seam below: those bytes come
// off a downloaded image and are fully attacker-controlled, and this tree has
// shipped three heap over-reads (#476, #490, #597) in C parsers of exactly this
// shape. The C references are kept as *_c for rollback and for the boot
// differential.
#include "diskimg.h"
#include "imgfile.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../sync/spinlock.h"
#include "../sync/waitq.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "../drivers/hotplug.h"   // #234i: hotplug_raw_t, the ONE volume-record shape

extern fat_fs_t g_fat_fs;
extern void *fat_read_file(fat_fs_t *fs, const char *path, unsigned int *size_out);
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

// rustkern/drvmap.rs - the drive-letter policy. See that file for the model.
extern uint32_t drvmap_class_rs(uint32_t idx);
extern uint32_t drvmap_class_for_fmt_rs(uint32_t fmt);
extern int32_t  drvmap_place_rs(int32_t want, uint32_t fmt, uint32_t occupied, uint32_t mounted);
extern int32_t  drvmap_mscdex_rs(uint32_t cd_mask, mscdex_info_t *out);
extern int32_t  drvmap_path_ok_rs(const uint8_t *path, uint32_t maxlen);
extern int32_t  drvmap_selftest_rs(uint32_t *out_checks);

// DRVMAP_E_PATH in rustkern/drvmap.rs. Mirrored here rather than included
// because it is the one drvmap code this file RETURNS rather than passes
// through, and a silent drift would turn "bad path" into "bad format".
#define DRVMAP_PATH_REFUSED (-6)

// FFI width locks: the two structs are shared with rustkern/iso9660.rs by
// layout. A field added on one side without the other would silently
// mis-decode a disc, so make it a compile error instead.
_Static_assert(sizeof(iso_vol_t)    == 56,
               "#196 FFI: IsoVol is six u32 plus the 32-byte volume identifier");
_Static_assert(sizeof(iso_dirrec_t) == 32, "#196 FFI: IsoDirRec is eight u32");
_Static_assert(sizeof(uint32_t)     == 4,  "#196 FFI: u32 width");
_Static_assert(sizeof(uint8_t)      == 1,  "#196 FFI: u8 width");
// #739 FFI width lock: mscdex_info_t is shared with MscdexInfo in
// rustkern/drvmap.rs by layout, and drvmap_mscdex_rs() writes every byte of it.
_Static_assert(sizeof(mscdex_info_t) == 40, "#739 FFI: MscdexInfo is 2xu32 + 32 bytes");

#define ISO_SECT 2048u

// [no-ticket] rustkern/isomemo.rs. The struct is shared by value; the width
// lock is below the definition, where sizeof is available.
#define ISOMEMO_PATH_MAX 128
#define ISOMEMO_WAYS     4
typedef struct isomemo_ent {
    uint8_t  path[ISOMEMO_PATH_MAX];
    uint32_t lba, ext, isdir, multi, valid, lru;
} isomemo_ent_t;
typedef struct isomemo {
    isomemo_ent_t e[ISOMEMO_WAYS];
    uint32_t clock, _pad;
    uint64_t n_hit, n_miss;
} isomemo_t;
extern void isomemo_reset_rs(isomemo_t *m);
extern int  isomemo_lookup_rs(isomemo_t *m, const char *path, uint32_t *lba,
                              uint32_t *ext, int *isdir, int *multi);
extern void isomemo_store_rs(isomemo_t *m, const char *path, uint32_t lba,
                             uint32_t ext, int isdir, int multi);
extern void isomemo_stats_rs(const isomemo_t *m, uint64_t *hit, uint64_t *miss);

// [no-ticket] The memo's own off switch, for the same two reasons as
// imgfile_readahead_set_disabled(): it is the control arm of the A/B in
// dos/cdbench.c, and it is the escape hatch if a disc ever needs the full walk.
static int g_memo_off = 0;
void isomemo_set_disabled(int off) { g_memo_off = off ? 1 : 0; }
int  isomemo_disabled(void) { return g_memo_off; }

typedef struct {
    int       fmt;              // DISKIMG_FMT_*
    char      name[64];         // basename, for the UI
    char      path[192];        // full image path, for the UI
    uint64_t  size;             // image size in bytes

    // ISO9660 (streamed)
    imgfile_t img;
    iso_vol_t pvd;              // primary descriptor (kind 1)
    iso_vol_t jol;              // Joliet descriptor (kind 2), root_len 0 if none
    // #184: set by iso_probe() when a PRIMARY descriptor parsed but pointed its
    // root directory outside the file. Only meaningful during img_build(), which
    // turns it into DISKIMG_E_TRUNC and frees the image; no mounted image ever
    // carries it set.
    int       truncated;

    // FAT12 (RAM-resident)
    unsigned char *buf;
    unsigned int   bufsize;
    unsigned bps, spc, rsvd, nfat, rootent, spf;
    unsigned fat_off, root_off, data_off;

    // #739 LIFETIME. `refs` is the drive slot's own reference (1 while
    // attached) plus one per in-flight read. `retired` is set when the slot
    // lets go, so the last reader out knows to free rather than leaving a
    // detached image on the heap forever. Both are touched only under g_lock.
    int       refs;
    int       retired;

    // #739 TURNSTILE. One 256 KiB imgfile cache per image, so exactly one read
    // may be inside imgfile_read() at a time or two readers hand each other the
    // wrong block. wait_event on the shared wait queue (sync/waitq.h), never a
    // poll: the release always wakes, so the wake source is armed by
    // construction and cannot be lost.
    volatile uint32_t busy;
    wait_queue_head_t wq;

    // [no-ticket] RESOLVED-EXTENT MEMO. read_range_inner() used to call
    // iso_resolve() on every read, and a DOS guest reads 4 KB at a time, so a
    // 105 MB streamed file re-walked the directory tree from the volume root
    // ~26,000 times. Touched only by a reader holding the turnstile above, so
    // it needs no lock of its own. rustkern/isomemo.rs owns the policy and the
    // bounded path handling.
    isomemo_t memo;
} diskimg_t;
_Static_assert(sizeof(isomemo_ent_t) == 152, "[no-ticket] FFI: IsoMemoEnt is 128 bytes of path plus six u32");
_Static_assert(sizeof(isomemo_t) == 152 * 4 + 8 + 16,
               "[no-ticket] FFI: IsoMemo is four ways, two u32 and two u64");

// ---------------------------------------------------------------------------
// THE TABLE. 26 letters, A: at index 0. A slot holds a POINTER, not a struct by
// value, because an image must be able to outlive its slot: an eject detaches
// it while a reader still holds a reference, and the reader is what frees it.
// ---------------------------------------------------------------------------
static diskimg_t *g_drive[26];
// Mount generation per letter. Bumped on every attach AND every detach, never
// reset, so a generation identifies one disc-in-one-drive for the whole boot.
// Starts at 1 on the first mount, so 0 can safely mean "no handle stamp".
static uint32_t   g_gen[26];
static spinlock_t g_lock;
static int        g_lock_ready;

// The table lock covers ONLY pointer/refcount/generation bookkeeping. Nothing
// that can block (kmalloc, imgfile I/O, kfree) is ever done while holding it:
// those all happen either before it is taken or after it is dropped.
static void tbl_lock_init_once(void) {
    if (!g_lock_ready) {
        spinlock_init_named(&g_lock, "diskimg");
        g_lock_ready = 1;
    }
}

static int idx_for(char letter) {
    char d = (letter >= 'a' && letter <= 'z') ? (char)(letter - 32) : letter;
    return (d >= 'A' && d <= 'Z') ? (d - 'A') : -1;
}
static char letter_of(int idx) {
    return (idx >= 0 && idx < 26) ? (char)('A' + idx) : '?';
}

// Free a DETACHED image. Caller must own the last reference and must NOT hold
// g_lock: imgfile_close() can reach the filesystem and kfree() can block.
static void img_destroy(diskimg_t *im) {
    if (!im) return;
    if (im->buf) kfree(im->buf);
    imgfile_close(&im->img);
    kfree(im);
}

// Take a reference on the image mounted at `idx`. When `gen` is non-zero the
// caller is stating which mount it believes it is reading, and a mismatch is a
// refusal rather than a read of whatever is there now. Returns NULL if the
// drive is empty or the generation is stale.
static diskimg_t *img_acquire(int idx, uint32_t gen) {
    if (idx < 0 || idx >= 26) return 0;
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    diskimg_t *im = g_drive[idx];
    if (im && (gen == 0 || gen == g_gen[idx])) im->refs++;
    else im = 0;
    spinlock_release_irqrestore(&g_lock, fl);
    return im;
}

// Drop a reference. Frees only when this was the last one AND the slot has
// already let go, which is the eject-during-read case.
static void img_release(diskimg_t *im) {
    if (!im) return;
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    int last = (--im->refs <= 0) && im->retired;
    spinlock_release_irqrestore(&g_lock, fl);
    if (last) img_destroy(im);
}

// Serialise access to this image's single imgfile cache. Both halves are called
// only from a context that may block (imgfile.h states the same requirement for
// the reads underneath), never from an ISR.
// The CLAIM IS THE TEST. `wait_event(wq, busy == 0); busy = 1;` would be wrong:
// wait_event re-checks on every wake, but between its last check and a separate
// assignment two waiters can both see zero and both proceed. atomic_xchg32 makes
// observing "it was free" and taking it one indivisible step, so the macro's
// re-evaluation (it may evaluate the condition twice per iteration) is harmless:
// an attempt that loses writes 1 over 1 and changes nothing.
//
// At boot, before the scheduler is live, this is never contended, so the xchg
// returns 0 on the first evaluation and __wait_prepare() is never reached. That
// matters: wq_assert_may_block() would (correctly) object to sleeping there.
static void img_enter(diskimg_t *im) {
    wait_event(&im->wq, atomic_xchg32(&im->busy, 1) == 0);
}
static void img_leave(diskimg_t *im) {
    (void)atomic_xchg32(&im->busy, 0);
    wake_up_all(&im->wq);
}

static unsigned rd16le(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Case-insensitive compare of NUL-terminated a against NUL-terminated b.
static int name_eq(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && b[i]; i++)
        if (upc(a[i]) != upc(b[i])) return 0;
    return a[i] == 0 && b[i] == 0;
}

// Pull the next path component from *pp (skipping separators). Writes up to
// outsz-1 chars to out. Returns 1 if a component was produced, 0 at end.
static int next_comp(const char **pp, char *out, int outsz) {
    const char *p = *pp;
    while (*p == '/' || *p == '\\') p++;
    if (!*p) { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != '/' && *p != '\\') {
        if (n < outsz - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return 1;
}

// ===========================================================================
// ISO 9660 parse seam: C references (verbatim behavioral mirrors of the Rust
// in rustkern/iso9660.rs) plus the live routing wrappers.
// ===========================================================================

int iso_vd_parse_c(const uint8_t *sec, uint32_t len, iso_vol_t *out) {
    if (!sec || !out) return -1;
    if (len < 190) return -1;
    if (!(sec[1] == 'C' && sec[2] == 'D' && sec[3] == '0' && sec[4] == '0' && sec[5] == '1'))
        return -1;

    uint8_t ty = sec[0];
    uint32_t joliet_ucs = 0;
    if (ty == 1) {
        // primary
    } else if (ty == 2) {
        if (sec[88] == '%' && sec[89] == '/') {
            if (sec[90] == '@')      joliet_ucs = 1;
            else if (sec[90] == 'C') joliet_ucs = 2;
            else if (sec[90] == 'E') joliet_ucs = 3;
        }
        if (joliet_ucs == 0) return 0;
    } else {
        return 0;
    }

    uint32_t bs = rd16le(sec + 128);
    if (bs != ISO_SECT) return 0;

    uint32_t root_lba = rd32le(sec + 156 + 2);
    uint32_t root_len = rd32le(sec + 156 + 10);
    if (root_len == 0) return 0;

    out->root_lba   = root_lba;
    out->root_len   = root_len;
    out->block_size = bs;
    out->kind       = (ty == 1) ? 1u : 2u;
    out->joliet_ucs = joliet_ucs;
    out->_pad       = 0;
    // Volume identifier: 32 raw bytes at offset 40. The len < 190 guard at the
    // top of this function already proves 72 <= len.
    memcpy(out->volid, sec + 40, sizeof out->volid);
    return 1;
}

int iso_dirrec_at_c(const uint8_t *buf, uint32_t buflen, uint32_t pos, iso_dirrec_t *out) {
    if (!buf || !out) return -1;
    memset(out, 0, sizeof(*out));

    if (pos >= buflen) return 0;

    unsigned reclen = buf[pos];
    if (reclen == 0) {
        uint32_t next = ((pos / ISO_SECT) + 1) * ISO_SECT;
        if (next <= pos || next >= buflen) return 0;
        out->next = next;
        return 1;
    }
    if (reclen < 33 || (uint64_t)pos + reclen > (uint64_t)buflen) return 0;
    unsigned namelen = buf[pos + 32];
    if (namelen == 0 || 33u + namelen > reclen) return 0;

    out->next     = pos + reclen;
    out->lba      = rd32le(buf + pos + 2);
    out->len      = rd32le(buf + pos + 10);
    uint8_t flags = buf[pos + 25];
    out->is_dir   = (flags & 0x02) ? 1u : 0u;
    out->multi    = (flags & 0x80) ? 1u : 0u;
    out->name_off = pos + 33;
    out->name_len = namelen;
    return 1;
}

int iso_name_decode_c(const uint8_t *src, uint32_t srclen, int joliet,
                      uint8_t *out, uint32_t outcap) {
    if (!src || !out || outcap == 0) return -1;
    out[0] = 0;
    if (srclen == 0) return 0;
    if (srclen == 1 && (src[0] == 0 || src[0] == 1)) return 0;

    uint32_t n = 0;
    if (joliet) {
        uint32_t pairs = srclen / 2;
        for (uint32_t i = 0; i < pairs; i++) {
            uint32_t cp = ((uint32_t)src[i * 2] << 8) | (uint32_t)src[i * 2 + 1];
            if (cp == 0) break;
            uint8_t c = (cp >= 0x20 && cp < 0x7F) ? (uint8_t)cp : (uint8_t)'_';
            if (n + 1 >= outcap) break;
            out[n++] = c;
        }
    } else {
        for (uint32_t i = 0; i < srclen; i++) {
            uint8_t c = src[i];
            if (c == 0) break;
            if (n + 1 >= outcap) break;
            out[n++] = (c >= 0x20 && c < 0x7F) ? c : (uint8_t)'_';
        }
    }

    for (uint32_t k = 0; k < n; k++) {
        if (out[k] == ';') { n = k; break; }
    }
    if (n > 0 && out[n - 1] == '.') n--;
    out[n] = 0;
    return (int)n;
}

// Live routing. -DRUST_ISO9660 (set in the Makefile CFLAGS) selects the Rust
// ports; removing that one flag reverts every ISO parse to the C above.
int iso_vd_parse(const uint8_t *sec, uint32_t len, iso_vol_t *out) {
#ifdef RUST_ISO9660
    return iso_vd_parse_rs(sec, len, out);
#else
    return iso_vd_parse_c(sec, len, out);
#endif
}
int iso_dirrec_at(const uint8_t *buf, uint32_t buflen, uint32_t pos, iso_dirrec_t *out) {
#ifdef RUST_ISO9660
    return iso_dirrec_at_rs(buf, buflen, pos, out);
#else
    return iso_dirrec_at_c(buf, buflen, pos, out);
#endif
}
int iso_name_decode(const uint8_t *src, uint32_t srclen, int joliet,
                    uint8_t *out, uint32_t outcap) {
#ifdef RUST_ISO9660
    return iso_name_decode_rs(src, srclen, joliet, out, outcap);
#else
    return iso_name_decode_c(src, srclen, joliet, out, outcap);
#endif
}

// ===========================================================================
// ISO 9660 directory walking over the streamed image.
//
// Directory records never straddle a 2048-byte logical sector (the tail of a
// sector is zero padding), so the walk reads ONE sector at a time into a 2 KiB
// stack-free buffer. RAM cost is constant regardless of directory or image
// size, and a record that claims to run past the sector is rejected by the
// parser rather than trusted.
// ===========================================================================

// Callback for each named record in a directory. Return 1 to stop the walk.
typedef int (*iso_walk_cb)(const char *name, const iso_dirrec_t *rec, void *ud);

static int iso_walk_dir(diskimg_t *im, int joliet, uint32_t dir_lba, uint32_t dir_len,
                        iso_walk_cb cb, void *ud) {
    uint8_t *sect = (uint8_t *)kmalloc(ISO_SECT);
    if (!sect) return -1;

    int stopped = 0;
    uint32_t nsect = (dir_len + ISO_SECT - 1) / ISO_SECT;
    for (uint32_t s = 0; s < nsect && !stopped; s++) {
        uint64_t off = (uint64_t)dir_lba * ISO_SECT + (uint64_t)s * ISO_SECT;
        int64_t got = imgfile_read(&im->img, off, ISO_SECT, sect);
        if (got <= 0) break;
        if ((uint64_t)got < ISO_SECT) memset(sect + got, 0, ISO_SECT - (uint32_t)got);

        uint32_t pos = 0;
        for (;;) {
            iso_dirrec_t rec;
            int r = iso_dirrec_at(sect, ISO_SECT, pos, &rec);
            if (r != 1) break;
            if (rec.name_len) {
                char nm[128];
                int nl = iso_name_decode(sect + rec.name_off, rec.name_len,
                                         joliet, (uint8_t *)nm, sizeof nm);
                if (nl > 0) {
                    if (cb(nm, &rec, ud)) { stopped = 1; break; }
                }
            }
            // The parser always reports strictly forward progress (a record
            // consumes >= 33 bytes, padding jumps to the next sector), so this
            // check is a belt-and-braces guard against a future parser bug, not
            // a poll: it exits the loop, it does not retry.
            if (rec.next <= pos) break;
            pos = rec.next;
        }
    }
    kfree(sect);
    return stopped ? 1 : 0;
}

struct iso_find_ctx { const char *want; iso_dirrec_t hit; int found; };
static int iso_find_cb(const char *name, const iso_dirrec_t *rec, void *ud) {
    struct iso_find_ctx *c = (struct iso_find_ctx *)ud;
    if (name_eq(name, c->want)) { c->hit = *rec; c->found = 1; return 1; }
    return 0;
}

// Resolve relpath against one namespace (primary or Joliet). Returns 1 on hit.
static int iso_resolve_ns(diskimg_t *im, const iso_vol_t *vol, int joliet,
                          const char *relpath,
                          uint32_t *out_lba, uint32_t *out_len, int *out_isdir,
                          int *out_multi) {
    if (!vol->root_len) return 0;
    uint32_t cur_lba = vol->root_lba, cur_len = vol->root_len;
    int isdir = 1, multi = 0;
    const char *p = relpath ? relpath : "";
    char comp[128];
    int any = 0;

    while (next_comp(&p, comp, sizeof comp)) {
        any = 1;
        if (!isdir) return 0;              // tried to descend into a file
        struct iso_find_ctx c; c.want = comp; c.found = 0;
        memset(&c.hit, 0, sizeof c.hit);
        iso_walk_dir(im, joliet, cur_lba, cur_len, iso_find_cb, &c);
        if (!c.found) return 0;
        cur_lba = c.hit.lba;
        cur_len = c.hit.len;
        isdir   = c.hit.is_dir ? 1 : 0;
        multi   = c.hit.multi ? 1 : 0;
    }
    (void)any;
    *out_lba = cur_lba; *out_len = cur_len; *out_isdir = isdir; *out_multi = multi;
    return 1;
}

// Resolve against the primary namespace, then Joliet. The primary tree is the
// authoritative one because it carries the 8.3 names a DOS guest expects; the
// Joliet fallback only matters for a name that exists solely in the long-name
// tree, which the target discs do not have but other discs do.
static int iso_resolve(diskimg_t *im, const char *relpath,
                       uint32_t *out_lba, uint32_t *out_len, int *out_isdir,
                       int *out_multi) {
    if (iso_resolve_ns(im, &im->pvd, 0, relpath, out_lba, out_len, out_isdir, out_multi))
        return 1;
    if (im->jol.root_len &&
        iso_resolve_ns(im, &im->jol, 1, relpath, out_lba, out_len, out_isdir, out_multi))
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// FAT12 (512-byte sectors), RAM-resident: a floppy image is at most 2.88 MB.
// Carried across from the first #196 implementation unchanged except that the
// image now arrives via imgfile_read instead of fat_read_file.
// ---------------------------------------------------------------------------
#define F12_SECT 512

static int fat12_parse(diskimg_t *im) {
    if (im->bufsize < 512) return 0;
    const unsigned char *b = im->buf;
    im->bps = rd16le(b + 11);
    im->spc = b[13];
    im->rsvd = rd16le(b + 14);
    im->nfat = b[16];
    im->rootent = rd16le(b + 17);
    im->spf = rd16le(b + 22);
    if (im->bps != 512 || im->spc == 0 || im->nfat == 0 || im->spf == 0) return 0;
    if (im->rootent == 0 || im->rootent > 1024) return 0;
    im->fat_off = im->rsvd * F12_SECT;
    im->root_off = (im->rsvd + im->nfat * im->spf) * F12_SECT;
    unsigned rootbytes = im->rootent * 32;
    unsigned rootsecs = (rootbytes + F12_SECT - 1) / F12_SECT;
    im->data_off = im->root_off + rootsecs * F12_SECT;
    if (im->data_off > im->bufsize) return 0;
    return 1;
}

static unsigned fat12_next(diskimg_t *im, unsigned cl) {
    unsigned off = im->fat_off + (cl * 3) / 2;
    if (off + 1 >= im->bufsize) return 0xFFF;
    unsigned v = rd16le(im->buf + off);
    return (cl & 1) ? (v >> 4) : (v & 0xFFF);
}

static void fat12_name(const unsigned char *e, char *out) {
    int n = 0;
    int base = 8; while (base > 0 && e[base - 1] == ' ') base--;
    for (int i = 0; i < base; i++) out[n++] = (char)e[i];
    int ext = 3; while (ext > 0 && e[8 + ext - 1] == ' ') ext--;
    if (ext > 0) { out[n++] = '.'; for (int i = 0; i < ext; i++) out[n++] = (char)e[8 + i]; }
    out[n] = '\0';
}

typedef int (*f12_iter_cb)(const unsigned char *e, void *ud);
static void fat12_iterdir(diskimg_t *im, unsigned clus, f12_iter_cb cb, void *ud) {
    if (clus == 0) {
        unsigned n = im->rootent;
        for (unsigned i = 0; i < n; i++) {
            if (im->root_off + i * 32 + 32 > im->bufsize) return;
            const unsigned char *e = im->buf + im->root_off + i * 32;
            if (e[0] == 0x00) return;
            if (e[0] == 0xE5) continue;
            if ((e[11] & 0x0F) == 0x0F) continue;   // LFN
            if (e[11] & 0x08) continue;             // volume label
            if (cb(e, ud)) return;
        }
        return;
    }
    unsigned cl = clus, guard = 0;
    while (cl >= 2 && cl < 0xFF8 && guard++ < 4096) {
        unsigned base = im->data_off + (cl - 2) * im->spc * F12_SECT;
        unsigned perclus = im->spc * F12_SECT / 32;
        for (unsigned i = 0; i < perclus; i++) {
            if (base + i * 32 + 32 > im->bufsize) return;
            const unsigned char *e = im->buf + base + i * 32;
            if (e[0] == 0x00) return;
            if (e[0] == 0xE5) continue;
            if ((e[11] & 0x0F) == 0x0F) continue;
            if (e[11] & 0x08) continue;
            if (cb(e, ud)) return;
        }
        cl = fat12_next(im, cl);
    }
}

struct f12_find { const char *want; const unsigned char *hit; };
static int f12_find_cb(const unsigned char *e, void *ud) {
    struct f12_find *f = (struct f12_find *)ud;
    char nm[16]; fat12_name(e, nm);
    if (name_eq(f->want, nm)) { f->hit = e; return 1; }
    return 0;
}

static int fat12_resolve(diskimg_t *im, const char *relpath,
                         unsigned *clus, unsigned *size, int *isdir) {
    unsigned cur = 0;
    const char *p = relpath ? relpath : "";
    char comp[64];
    *clus = 0; *size = 0; *isdir = 1;
    while (next_comp(&p, comp, sizeof comp)) {
        struct f12_find f = { comp, 0 };
        fat12_iterdir(im, cur, f12_find_cb, &f);
        if (!f.hit) return 0;
        unsigned fc = rd16le(f.hit + 26);
        unsigned sz = rd32le(f.hit + 28);
        int d = (f.hit[11] & 0x10) ? 1 : 0;
        *clus = fc; *size = sz; *isdir = d;
        cur = fc;
    }
    return 1;
}

// Read a byte range out of a FAT12 file identified by its first cluster.
static int64_t fat12_read_range(diskimg_t *im, unsigned clus, unsigned fsize,
                                uint64_t off, uint64_t len, uint8_t *dst) {
    if (off >= fsize) return 0;
    if (off + len > fsize) len = fsize - off;
    unsigned cbytes = im->spc * F12_SECT;
    if (cbytes == 0) return -1;

    // Walk to the cluster containing `off`. Bounded by the FAT chain guard.
    unsigned cl = clus, guard = 0;
    uint64_t skip = off / cbytes;
    while (skip-- > 0 && cl >= 2 && cl < 0xFF8 && guard++ < 65536)
        cl = fat12_next(im, cl);

    uint64_t done = 0;
    unsigned within = (unsigned)(off % cbytes);
    while (done < len && cl >= 2 && cl < 0xFF8 && guard++ < 65536) {
        unsigned base = im->data_off + (cl - 2) * cbytes;
        unsigned avail = cbytes - within;
        if ((uint64_t)avail > len - done) avail = (unsigned)(len - done);
        if (base + within + avail > im->bufsize) {
            avail = (base + within < im->bufsize) ? (im->bufsize - base - within) : 0;
            if (avail == 0) break;
        }
        memcpy(dst + done, im->buf + base + within, avail);
        done += avail;
        within = 0;
        cl = fat12_next(im, cl);
    }
    return (int64_t)done;
}

// ===========================================================================
// Public API
// ===========================================================================
static void basename_of(const char *path, char *out, int outsz) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    int n = 0; while (b[n] && n < outsz - 1) { out[n] = b[n]; n++; }
    out[n] = '\0';
}

// Scan the volume-descriptor set (sectors 16..) for a primary and a Joliet
// descriptor. Stops at the terminator or after a bounded number of sectors, so
// a malformed image cannot make this run away.
static int iso_probe(diskimg_t *im) {
    uint8_t *sect = (uint8_t *)kmalloc(ISO_SECT);
    if (!sect) return 0;
    memset(&im->pvd, 0, sizeof im->pvd);
    memset(&im->jol, 0, sizeof im->jol);

    // #184: the bounds check lives in Rust (rustkern/iso9660.rs). Read the doc
    // comment on iso_root_within_rs() before changing anything here: a valid
    // descriptor is NOT the same fact as a present disc, and this function used
    // to conflate them.
    extern int iso_root_within_rs(uint32_t root_lba, uint32_t root_len,
                                  uint32_t block_size, uint64_t image_size);
    extern uint64_t iso_vd_declared_bytes_rs(const uint8_t *sec, uint32_t len);

    int got_pvd = 0;
    im->truncated = 0;
    for (unsigned s = 16; s < 16 + 32; s++) {
        int64_t got = imgfile_read(&im->img, (uint64_t)s * ISO_SECT, ISO_SECT, sect);
        if (got < (int64_t)ISO_SECT) break;
        if (!(sect[1] == 'C' && sect[2] == 'D' && sect[3] == '0' &&
              sect[4] == '0' && sect[5] == '1')) break;
        if (sect[0] == 0xFF) break;                  // terminator
        iso_vol_t v;
        int r = iso_vd_parse(sect, ISO_SECT, &v);
        if (r == 1) {
            // #184: REFUSE a descriptor whose root directory is not in the file.
            // Nothing below this point can read a directory that is not there,
            // so accepting it produces a drive that mounts, reports its size,
            // and answers every lookup with "not found" - the failure mode this
            // tree deletes tools for (#98, #103). Recorded on the image rather
            // than returned directly so a Joliet SVD pointing outside a
            // otherwise-good disc degrades to "no Joliet" instead of failing
            // the whole mount.
            if (!iso_root_within_rs(v.root_lba, v.root_len, v.block_size,
                                    im->size)) {
                kprintf("[diskimg] %s: %s descriptor at sector %u has its root "
                        "directory at lba=%u len=%u (byte %llu), past the end of "
                        "a %llu-byte image: TRUNCATED or corrupt, refusing\n",
                        im->path, (v.kind == 1) ? "primary" : "Joliet", s,
                        (unsigned)v.root_lba, (unsigned)v.root_len,
                        (unsigned long long)v.root_lba * v.block_size,
                        (unsigned long long)im->size);
                if (v.kind == 1) im->truncated = 1;
                continue;
            }
            if (v.kind == 1 && !got_pvd) {
                im->pvd = v; got_pvd = 1;
                // Not a refusal, on purpose: real rips drop trailing padding and
                // still work. Say it once so a later "file not found" on a big
                // file at the end of the disc has an explanation on the record.
                uint64_t decl = iso_vd_declared_bytes_rs(sect, ISO_SECT);
                if (decl && decl > im->size) {
                    kprintf("[diskimg] %s: descriptor declares %llu bytes but the "
                            "file is %llu; the tail is missing, files near the end "
                            "of the disc will not read\n",
                            im->path, (unsigned long long)decl,
                            (unsigned long long)im->size);
                }
            }
            else if (v.kind == 2 && !im->jol.root_len) { im->jol = v; }
        }
    }
    kfree(sect);
    return got_pvd;
}

// Build a fully-formed, DETACHED image from a path. Everything that can block
// (imgfile_open, the probe reads, kmalloc of a floppy buffer) happens here, with
// no lock held and nothing yet visible to any other context. Returns the image,
// or NULL with *err set to a DISKIMG_E_* code.
static diskimg_t *img_build(const char *imgpath, int *err) {
    *err = DISKIMG_E_NOMEM;
    diskimg_t *im = (diskimg_t *)kmalloc(sizeof(diskimg_t));
    if (!im) return 0;
    memset(im, 0, sizeof(*im));
    wait_queue_head_init(&im->wq);

    if (imgfile_open(&im->img, imgpath) != 0) {
        kprintf("[diskimg] %s: cannot open\n", imgpath);
        kfree(im);
        *err = DISKIMG_E_OPEN;
        return 0;
    }
    im->size = imgfile_size(&im->img);
    basename_of(imgpath, im->name, sizeof im->name);
    { size_t k = 0;
      while (k + 1 < sizeof(im->path) && imgpath[k]) { im->path[k] = imgpath[k]; k++; }
      im->path[k] = 0; }

    if (iso_probe(im)) {
        im->fmt = DISKIMG_FMT_ISO9660;
        isomemo_reset_rs(&im->memo);   // the memset above already did this; say so
        return im;
    }
    // #184: it WAS an ISO, and its root directory is not in the file. Say that,
    // rather than falling through to the FAT12 attempt and reporting whatever
    // that fails with - "unrecognised image format" for a file that is plainly a
    // recognised format sends the user looking in the wrong place.
    if (im->truncated) {
        imgfile_close(&im->img); kfree(im);
        *err = DISKIMG_E_TRUNC;
        return 0;
    }

    // Not an ISO: try FAT12, which needs the image in RAM.
    if (im->size > DISKIMG_FAT12_MAX) {
        kprintf("[diskimg] %s: not ISO9660 and too large for a floppy (%u bytes)\n",
                imgpath, (unsigned)im->size);
        imgfile_close(&im->img); kfree(im);
        *err = DISKIMG_E_TOOBIG;
        return 0;
    }
    im->bufsize = (unsigned)im->size;
    im->buf = (unsigned char *)kmalloc(im->bufsize ? im->bufsize : 1);
    if (!im->buf) {
        imgfile_close(&im->img); kfree(im);
        *err = DISKIMG_E_NOMEM;
        return 0;
    }
    if (imgfile_read(&im->img, 0, im->bufsize, im->buf) != (int64_t)im->bufsize) {
        kfree(im->buf); imgfile_close(&im->img); kfree(im);
        *err = DISKIMG_E_OPEN;
        return 0;
    }
    if (fat12_parse(im)) {
        im->fmt = DISKIMG_FMT_FAT12;
        return im;
    }

    kprintf("[diskimg] %s: unrecognized image format\n", imgpath);
    kfree(im->buf); imgfile_close(&im->img); kfree(im);
    *err = DISKIMG_E_UNKNOWN;
    return 0;
}

// Detach whatever is on `idx`, bumping the generation. Returns the outgoing
// image IF this call owned its last reference (caller must then img_destroy it
// OUTSIDE the lock), or NULL. Must be called with g_lock held.
static diskimg_t *slot_detach_locked(int idx) {
    diskimg_t *old = g_drive[idx];
    if (!old) return 0;
    g_drive[idx] = 0;
    g_gen[idx]++;               // every handle stamped with the old value is now stale
    old->retired = 1;
    return (--old->refs <= 0) ? old : 0;
}

int diskimg_mount_idx(int want, const char *imgpath) {
    if (!imgpath || !imgpath[0]) return DRVMAP_PATH_REFUSED;
    // Reject the path shape BEFORE opening anything. drvmap_path_ok_rs() runs on
    // a kernel copy (the syscall bounced it already) and rejects relative paths,
    // "..", backslashes and control characters, so what perms_check() approved
    // and what gets opened are provably the same string.
    if (drvmap_path_ok_rs((const uint8_t *)imgpath, 4096) < 0) return DRVMAP_PATH_REFUSED;
    if (want != DISKIMG_LETTER_AUTO && (want < 0 || want > 25)) return -1;

    int err = 0;
    diskimg_t *im = img_build(imgpath, &err);
    if (!im) return err;

    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);

    // Placement is decided from the LIVE table under the lock, so two concurrent
    // mounts cannot both be told the same letter is free.
    uint32_t mask = 0; uint32_t n = 0;
    for (int i = 0; i < 26; i++) if (g_drive[i]) { mask |= (1u << i); n++; }
    int32_t place = drvmap_place_rs((int32_t)want, (uint32_t)im->fmt, mask, n);
    if (place < 0) {
        spinlock_release_irqrestore(&g_lock, fl);
        img_destroy(im);
        return place;                       // a DRVMAP_E_* code, passed through
    }

    diskimg_t *dead = slot_detach_locked(place);   // the swap case
    im->refs = 1;                                  // the slot's own reference
    g_drive[place] = im;
    g_gen[place]++;                                // a NEW disc, a new generation
    uint32_t gen = g_gen[place];
    spinlock_release_irqrestore(&g_lock, fl);

    if (dead) img_destroy(dead);

    if (im->fmt == DISKIMG_FMT_ISO9660)
        kprintf("[diskimg] mounted %s on %c: gen=%u (ISO9660, %u MiB, root lba=%u len=%u%s)\n",
                im->name, letter_of(place), gen, (unsigned)(im->size >> 20),
                im->pvd.root_lba, im->pvd.root_len,
                im->jol.root_len ? ", +Joliet" : "");
    else
        kprintf("[diskimg] mounted %s on %c: gen=%u (FAT12, %u bytes)\n",
                im->name, letter_of(place), gen, im->bufsize);

    // A drive's CWD belonged to the disc that just left. Leaving it would point
    // a guest at a directory on a disc that is no longer there.
    extern void dos_set_drive_cwd(char letter, const char *path);
    dos_set_drive_cwd(letter_of(place), "");
    return place;
}

int diskimg_mount(char letter, const char *imgpath) {
    int idx = idx_for(letter);
    if (idx < 0) return -1;
    int r = diskimg_mount_idx(idx, imgpath);
    return (r >= 0) ? 0 : r;
}

void diskimg_eject_idx(int idx) {
    if (idx < 0 || idx >= 26) return;
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    diskimg_t *cur = g_drive[idx];
    char nm[64]; nm[0] = 0;
    int inflight = 0;
    if (cur) {
        for (size_t k = 0; k < sizeof nm && cur->name[k]; k++) { nm[k] = cur->name[k]; nm[k+1] = 0; }
        inflight = cur->refs - 1;           // minus the slot's own reference
    }
    diskimg_t *dead = slot_detach_locked(idx);
    spinlock_release_irqrestore(&g_lock, fl);

    if (!cur) return;
    // Say when a reader was still inside the image. This is not an error and
    // nothing is refused, but it is the difference between "nothing was using
    // it" and "a guest just had its handle invalidated", and a silent eject
    // would make the second indistinguishable from the first in a bug report.
    if (inflight > 0)
        kprintf("[diskimg] ejected %s from %c: (%d read(s) in flight; handles invalidated)\n",
                nm, letter_of(idx), inflight);
    else
        kprintf("[diskimg] ejected %s from %c:\n", nm, letter_of(idx));

    extern void dos_set_drive_cwd(char letter, const char *path);
    dos_set_drive_cwd(letter_of(idx), "");
    if (dead) img_destroy(dead);
}

void diskimg_eject(char letter) { diskimg_eject_idx(idx_for(letter)); }

uint32_t diskimg_mounted_mask(void) {
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    uint32_t mask = 0;
    for (int i = 0; i < 26; i++) if (g_drive[i]) mask |= (1u << i);
    spinlock_release_irqrestore(&g_lock, fl);
    return mask;
}

int diskimg_mount_count(void) {
    uint32_t m = diskimg_mounted_mask(); int n = 0;
    while (m) { n += (int)(m & 1u); m >>= 1; }
    return n;
}

int diskimg_letter_class(int idx) {
    return (idx < 0 || idx > 25) ? DISKIMG_CLASS_NONE : (int)drvmap_class_rs((uint32_t)idx);
}

uint32_t diskimg_generation(char letter) {
    int idx = idx_for(letter);
    if (idx < 0) return 0;
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    uint32_t g = g_gen[idx];
    spinlock_release_irqrestore(&g_lock, fl);
    return g;
}

void diskimg_mscdex(mscdex_info_t *out) {
    if (!out) return;
    // Only CD-CLASS letters that actually hold a disc. drvmap_mscdex_rs()
    // filters by class itself, so handing it the whole mask is safe and keeps
    // ONE definition of which letters are CDs.
    drvmap_mscdex_rs(diskimg_mounted_mask(), out);
}

// ---------------------------------------------------------------------------
// VOLUME LABEL
// ---------------------------------------------------------------------------
// The name written ON the disc, which is how a DOS program tells one disc from
// another. It is NOT the image filename: two copies of the same disc under
// different filenames must identify as the same volume, and a renamed image
// must not change what a guest thinks it is holding.
//
// WHY 11 CHARACTERS. This answers INT 21h 4Eh with a volume-label mask, and
// that call returns a FAT directory entry, whose name field is 11 bytes. Real
// MSCDEX truncates an ISO volume identifier (up to 32 bytes) to 11 for exactly
// that reason, so a guest that compares the answer against a hardcoded name is
// comparing against what real hardware would have given it. Trailing spaces go
// too: the on-disc field is space-padded, and a caller doing strcmp against
// "CD1" must not be handed "CD1        ".
//
// Sources, both of which are already in RAM by the time a disc is mounted, so
// this reads no sectors and cannot block:
//   ISO 9660  the PRIMARY descriptor's volume identifier (offset 40, 32 bytes).
//             The Joliet descriptor's copy is UCS-2 and deliberately ignored:
//             DOS never saw Joliet, so answering a DOS call from it would be
//             inventing history.
//   FAT12     the root-directory entry carrying attribute 0x08, which is where
//             a real floppy keeps its label. fat12_iterdir() filters that entry
//             out of normal listings (correctly: it is not a file), so this
//             walks the root itself rather than loosening that filter for
//             everybody.
#define DISKIMG_LABEL_MAX 12          // 11 characters plus the terminator

// Copy `n` raw label bytes into `out`, dropping trailing spaces and NULs and
// clamping to 11 characters.
static void label_trim(const uint8_t *raw, int n, char *out, int cap) {
    if (!out || cap < 1) return;
    out[0] = '\0';
    if (!raw || n <= 0) return;
    while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\0')) n--;
    if (n > 11) n = 11;                       // what MSCDEX reports to DOS
    int k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++) {
        uint8_t ch = raw[i];
        // A label reaches a 16-bit guest's DTA. Anything below 0x20 would be a
        // control character in the middle of a filename field, so refuse the
        // whole thing rather than pass a string that prints as garbage.
        if (ch < 0x20) { out[0] = '\0'; return; }
        out[k++] = (char)ch;
    }
    out[k] = '\0';
}

// The FAT12 root-directory volume-label entry, if the image has one.
static int fat12_volume_label(diskimg_t *im, char *out, int cap) {
    if (!im->rootent) return 0;
    for (unsigned i = 0; i < im->rootent; i++) {
        if (im->root_off + i * 32 + 32 > im->bufsize) return 0;
        const unsigned char *e = im->buf + im->root_off + i * 32;
        if (e[0] == 0x00) return 0;                 // end of directory
        if (e[0] == 0xE5) continue;                 // deleted
        if ((e[11] & 0x0F) == 0x0F) continue;       // long-filename fragment
        if (!(e[11] & 0x08)) continue;              // not the label
        label_trim(e, 11, out, cap);
        return out[0] ? 1 : 0;
    }
    return 0;
}

int diskimg_volume_label(char letter, char *out, int cap) {
    if (!out || cap < 1) return 0;
    out[0] = '\0';
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return 0;
    int got = 0;
    if (im->fmt == DISKIMG_FMT_ISO9660)
        { label_trim(im->pvd.volid, (int)sizeof im->pvd.volid, out, cap); got = out[0] ? 1 : 0; }
    else if (im->fmt == DISKIMG_FMT_FAT12)
        got = fat12_volume_label(im, out, cap);
    img_release(im);
    return got;
}

// #234e. See the header for why this exists. The ONE place that turns a
// mounted medium into the (bytes/sector, sectors/cluster, clusters) triple
// DOS asks for, so no caller has to invent one again.
//
// ISO 9660: 2048-byte sectors is not a convention here, it is a hard refusal
// point. rustkern/iso9660.rs rejects any volume whose logical block size at
// descriptor offset 128 is not 2048, so a mounted ISO cannot have any other
// sector size. One sector per cluster keeps the cluster count inside the 16
// bits DX has to hold it in, up to a 128 GB medium.
//
// FAT12: bps and spc are read straight out of the BPB by fat12_parse(), and
// the data area is what is left after the reserved sectors, the FATs and the
// root directory, which is exactly what data_off already measures. A 720 KB
// disk gives 512/2/713; a 1.44 MB disk gives 512/1/2847. Neither is a
// constant in this file, which is the point: the geometry comes off the disk.
int diskimg_geometry(char letter, uint32_t *bps_out, uint32_t *spc_out,
                     uint32_t *clusters_out) {
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return 0;
    uint32_t bps = 0, spc = 0;
    uint64_t clusters = 0;
    if (im->fmt == DISKIMG_FMT_ISO9660) {
        bps = ISO_SECT;
        spc = 1;
        clusters = (im->img.size + (ISO_SECT - 1)) / ISO_SECT;
    } else if (im->fmt == DISKIMG_FMT_FAT12) {
        bps = im->bps;
        spc = im->spc;
        uint32_t per = im->bps * im->spc;
        // fat12_parse() has already refused bps == 0 and spc == 0, and has
        // already refused data_off > bufsize, so neither guard below can be
        // the thing that saves us. They are here because this function is
        // reachable from a syscall and a divide by zero in Ring 0 is a panic,
        // not a bad answer.
        if (per && im->bufsize > im->data_off)
            clusters = (uint64_t)(im->bufsize - im->data_off) / per;
    }
    img_release(im);
    if (!bps || !spc) return 0;
    if (clusters > 0xFFFFu) clusters = 0xFFFFu;
    if (bps_out)      *bps_out = bps;
    if (spc_out)      *spc_out = spc;
    if (clusters_out) *clusters_out = (uint32_t)clusters;
    return 1;
}

int diskimg_query(int idx, diskimg_info_t *out) {
    if (idx < 0 || idx > 25 || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->letter = (uint32_t)idx;
    out->cls    = drvmap_class_rs((uint32_t)idx);
    if (out->cls == DISKIMG_CLASS_FLOPPY || out->cls == DISKIMG_CLASS_CDROM)
        out->flags |= DISKIMG_F_MOUNTABLE;
    if (out->cls == DISKIMG_CLASS_CDROM)
        out->flags |= DISKIMG_F_READONLY;      // a CD is read-only with or without a disc

    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    diskimg_t *im = g_drive[idx];
    out->gen = g_gen[idx];
    if (im) {
        out->fmt     = (uint32_t)im->fmt;
        out->size    = im->size;
        out->readers = (uint32_t)(im->refs > 0 ? im->refs - 1 : 0);
        out->flags  |= DISKIMG_F_MOUNTED;
        // No write-back to a mounted image is implemented, on any class. Saying
        // so up front is better than a write that fails at fat_write().
        out->flags  |= DISKIMG_F_READONLY;
        if (im->jol.root_len) out->flags |= DISKIMG_F_JOLIET;
        if (out->readers)     out->flags |= DISKIMG_F_INUSE;
        for (size_t k = 0; k + 1 < sizeof(out->name) && im->name[k]; k++) out->name[k] = im->name[k];
        for (size_t k = 0; k + 1 < sizeof(out->path) && im->path[k]; k++) out->path[k] = im->path[k];
    }
    spinlock_release_irqrestore(&g_lock, fl);
    return 0;
}

void diskimg_drvmap_selftest(void) {
    uint32_t checks = 0;
    int fails = drvmap_selftest_rs(&checks);
    char line[128];
    snprintf(line, sizeof line,
              "[DRVMAP] drvmap.rs property test: %u checks, %d failures -> %s\n",
              checks, fails, fails == 0 ? "PASS" : "FAIL");
    kprintf("%s", line);
    bootlog_write(line);
}

int diskimg_is_mounted(char letter) {
    int idx = idx_for(letter);
    if (idx < 0) return 0;
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    int r = g_drive[idx] ? 1 : 0;
    spinlock_release_irqrestore(&g_lock, fl);
    return r;
}

int diskimg_format(char letter) {
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return DISKIMG_FMT_NONE;
    int f = im->fmt;
    img_release(im);
    return f;
}

// NOTE: this returns a pointer INTO the image, so it is only sound while the
// image stays mounted. Every caller is a log line or a UI string built
// immediately; anything that needs to hold the name must use diskimg_query(),
// which copies. Kept for the existing callers rather than changed under them.
const char *diskimg_mounted_name(char letter) {
    int idx = idx_for(letter);
    if (idx < 0) return "";
    tbl_lock_init_once();
    uint64_t fl = spinlock_acquire_irqsave(&g_lock);
    const char *n = g_drive[idx] ? g_drive[idx]->name : "";
    spinlock_release_irqrestore(&g_lock, fl);
    return n;
}

uint64_t diskimg_image_size(char letter) {
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return 0;
    uint64_t s = im->size;
    img_release(im);
    return s;
}

int diskimg_has_joliet(char letter) {
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return 0;
    int j = im->jol.root_len ? 1 : 0;
    img_release(im);
    return j;
}

// ===========================================================================
// #234i: A MOUNTED IMAGE IS A VOLUME, AND THERE IS ONLY ONE VOLUME LIST.
// ===========================================================================
// The image mounter and the USB hot-plug layer had nothing in common except
// the thing a USER cares about: a drive that appeared and can be browsed and
// ejected. #250 built exactly that surface (SYS_VOL_LIST -> the Files
// sidebar and the desktop icons) and wired only USB into it, so mounting a
// floppy or a CD put a browsable filesystem on the machine that nothing in
// the GUI ever mentioned. You had to already know the path.
//
// The fix is NOT a second list in Files. It is this function: the mounter,
// which is the thing that knows what is mounted, publishes into the list that
// already exists, in the record shape that already exists. Files, the desktop
// and anything added later ask ONE question and get one answer. That is the
// same reasoning that put the drive-letter policy in rustkern/drvmap.rs after
// #739 found the same decision open-coded in three places that had already
// disagreed.
//
// WHAT IT DOES NOT DO. It does not decide how a volume is DRAWN, which flag
// bits it gets or what the filesystem is called on screen: rustkern/hotplug.rs
// derives all of that from fs_type, so there is one marshaller for both
// producers rather than two that can disagree about what "read-only" means.
//
// NON-BLOCKING: img_acquire()/img_release() around RAM fields only, plus the
// label, which dos/diskimg.h states reads no sector. No turnstile is taken,
// so this cannot wait behind an in-flight read of a 653 MB disc.
int diskimg_vol_raw(int idx, hotplug_raw_t *out) {
    if (!out || idx < 0 || idx >= 26) return 0;
    char letter = (char)('A' + idx);

    memset(out, 0, sizeof(*out));
    if (!diskimg_is_mounted(letter)) return 0;

    int fmt = diskimg_format(letter);
    if (fmt != DISKIMG_FMT_ISO9660 && fmt != DISKIMG_FMT_FAT12) return 0;

    out->present   = 1;
    out->mounted   = 1;
    // Both formats have real file operations behind them (fat_open() redirects
    // into the image), which is what `readable` means. It is NOT "writable":
    // read-only-ness travels as a separate flag derived from fs_type.
    out->readable  = 1;
    out->fs_type   = (fmt == DISKIMG_FMT_ISO9660) ? HOTPLUG_FS_ISO9660
                                                  : HOTPLUG_FS_FAT12;
    out->capacity_bytes = diskimg_image_size(letter);
    // A disc has no free space worth reporting and a floppy's would cost a
    // whole-FAT scan for a number nobody asked for. Say "unknown" rather than
    // print a confident zero.
    out->free_bytes = 0;
    out->free_known = 0;

    // NAME. The label written ON the medium if it has one, because that is
    // what the disc calls itself and what the user recognises; otherwise the
    // image's own basename, which always exists. Never empty: an unlabelled
    // row is indistinguishable from any other row.
    {
        char label[16];
        int n = 0;
        if (diskimg_volume_label(letter, label, sizeof(label)) && label[0]) {
            while (label[n] && n < (int)sizeof(out->name) - 1) { out->name[n] = label[n]; n++; }
        } else {
            const char *bn = diskimg_mounted_name(letter);
            while (bn[n] && n < (int)sizeof(out->name) - 1) { out->name[n] = bn[n]; n++; }
        }
        out->name[n] = '\0';
    }

    // MOUNT POINT. The native spelling of a DOS drive, which is what
    // rustkern/drvmap.rs's drvmap_windir_split_rs() parses and what
    // fat_open()'s image redirect keys on. Built here rather than passed in,
    // so the browsable path a UI is handed is the same string the kernel
    // resolves.
    {
        const char *pfx = "/WINDIR/DRIVE_";
        int n = 0;
        while (pfx[n]) { out->mount_point[n] = pfx[n]; n++; }
        out->mount_point[n++] = letter;
        out->mount_point[n] = '\0';
    }
    return 1;
}

// ---------------------------------------------------------------------------
// DATA PATH. Every public entry point below follows the SAME four steps, and
// they are not optional:
//   img_acquire()  take a reference (and, where a generation is supplied,
//                  prove the caller is still talking about the disc it opened)
//   img_enter()    take the per-image turnstile, because there is ONE cache
//   ...            do the work; it may block, and that is fine
//   img_leave() / img_release()
// Skipping the acquire is a use-after-free the moment a UI eject lands mid-read;
// skipping the turnstile is two readers corrupting each other's cache blocks.
// ---------------------------------------------------------------------------
static int stat_inner(diskimg_t *im, const char *relpath, uint64_t *size_out, int *isdir_out) {
    if (im->fmt == DISKIMG_FMT_ISO9660) {
        uint32_t lba, len; int isdir, multi;
        if (!iso_resolve(im, relpath, &lba, &len, &isdir, &multi)) return 0;
        if (size_out)  *size_out  = isdir ? 0 : (uint64_t)len;
        if (isdir_out) *isdir_out = isdir;
        return 1;
    }
    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, sz; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &sz, &isdir)) return 0;
        if (size_out)  *size_out  = isdir ? 0 : (uint64_t)sz;
        if (isdir_out) *isdir_out = isdir;
        return 1;
    }
    return 0;
}

static int stat_gen(char letter, uint32_t gen, const char *relpath,
                    uint64_t *size_out, int *isdir_out) {
    diskimg_t *im = img_acquire(idx_for(letter), gen);
    if (!im) return 0;
    img_enter(im);
    int r = stat_inner(im, relpath, size_out, isdir_out);
    img_leave(im);
    img_release(im);
    return r;
}

int diskimg_stat(char letter, const char *relpath, uint64_t *size_out, int *isdir_out) {
    return stat_gen(letter, 0, relpath, size_out, isdir_out);
}

static int64_t read_range_inner(diskimg_t *im, const char *relpath,
                                uint64_t off, uint64_t len, void *dst) {
    if (!dst) return -1;
    if (im->fmt == DISKIMG_FMT_ISO9660) {
        uint32_t lba, elen; int isdir, multi;
        // [no-ticket] The same relpath arrives thousands of times in a row when
        // a guest streams a file, and an ISO volume is read-only within one
        // mount, so the walk is done once. A miss falls through to the full
        // resolve and is stored; nothing here can answer for a disc that is no
        // longer in the drive, because diskimg_read_range_gen has already
        // checked the mount generation before this is reached.
        if (g_memo_off ||
            !isomemo_lookup_rs(&im->memo, relpath, &lba, &elen, &isdir, &multi)) {
            if (!iso_resolve(im, relpath, &lba, &elen, &isdir, &multi)) return -1;
            if (!g_memo_off)
                isomemo_store_rs(&im->memo, relpath, lba, elen, isdir, multi);
        }
        if (isdir) return -1;
        // A multi-extent file would need its continuation records followed; we
        // do not, so refuse rather than hand back a silently truncated file.
        if (multi) {
            kprintf("[diskimg] %s: multi-extent file not supported\n", relpath);
            return -1;
        }
        if (off >= elen) return 0;
        if (off + len > elen) len = elen - off;
        return imgfile_read(&im->img, (uint64_t)lba * ISO_SECT + off, len, dst);
    }
    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, sz; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &sz, &isdir)) return -1;
        if (isdir) return -1;
        return fat12_read_range(im, clus, sz, off, len, (uint8_t *)dst);
    }
    return -1;
}

int64_t diskimg_read_range_gen(char letter, uint32_t gen, const char *relpath,
                               uint64_t off, uint64_t len, void *dst) {
    // A non-zero generation that no longer matches means the disc this caller
    // opened has been ejected or swapped. Returning DISKIMG_E_STALE rather than
    // reading whatever is mounted NOW is the whole point: both Red Alert discs
    // carry a file called \MAIN.MIX, so "read it off the other disc" would be
    // wrong bytes with no error anywhere.
    diskimg_t *im = img_acquire(idx_for(letter), gen);
    if (!im) return (gen != 0) ? DISKIMG_E_STALE : -1;
    img_enter(im);
    int64_t r = read_range_inner(im, relpath, off, len, dst);
    img_leave(im);
    img_release(im);
    return r;
}

int64_t diskimg_read_range(char letter, const char *relpath,
                           uint64_t off, uint64_t len, void *dst) {
    return diskimg_read_range_gen(letter, 0, relpath, off, len, dst);
}

void *diskimg_read_file(char letter, const char *relpath, unsigned int *size_out) {
    if (size_out) *size_out = 0;
    uint64_t fsz = 0; int isdir = 0;
    if (!diskimg_stat(letter, relpath, &fsz, &isdir) || isdir) return 0;
    // Whole-file reads allocate the whole file. Above the cap that allocation
    // cannot succeed on a 256 MB heap, so refuse here where the caller can see
    // a NULL rather than in kmalloc where it looks like memory pressure.
    if (fsz > DISKIMG_WHOLE_MAX) {
        kprintf("[diskimg] %s is %u MiB: use diskimg_read_range (whole-file cap %u MiB)\n",
                relpath, (unsigned)(fsz >> 20), (unsigned)(DISKIMG_WHOLE_MAX >> 20));
        return 0;
    }
    unsigned char *out = (unsigned char *)kmalloc((unsigned long)(fsz ? fsz : 1));
    if (!out) return 0;
    int64_t got = diskimg_read_range(letter, relpath, 0, fsz, out);
    if (got < 0) { kfree(out); return 0; }
    if (size_out) *size_out = (unsigned int)got;
    return out;
}

// listdir
struct list_ctx { diskimg_dir_cb cb; void *ud; int count; };

static int iso_list_cb(const char *name, const iso_dirrec_t *rec, void *ud) {
    struct list_ctx *c = (struct list_ctx *)ud;
    c->cb(name, rec->is_dir ? 1 : 0, rec->len, c->ud);
    c->count++;
    return 0;
}

static int f12_list_cb(const unsigned char *e, void *ud) {
    struct list_ctx *c = (struct list_ctx *)ud;
    char nm[16]; fat12_name(e, nm);
    if (nm[0] == '.') return 0;
    int isdir = (e[11] & 0x10) ? 1 : 0;
    unsigned sz = rd32le(e + 28);
    c->cb(nm, isdir, sz, c->ud);
    c->count++;
    return 0;
}

static int listdir_inner(diskimg_t *im, const char *relpath, diskimg_dir_cb cb, void *ud) {
    if (!cb) return -1;
    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, size; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &size, &isdir) || !isdir) return -1;
        struct list_ctx c = { cb, ud, 0 };
        fat12_iterdir(im, clus, f12_list_cb, &c);
        return c.count;
    }
    if (im->fmt == DISKIMG_FMT_ISO9660) {
        uint32_t lba, len; int isdir, multi;
        if (!iso_resolve(im, relpath, &lba, &len, &isdir, &multi) || !isdir) return -1;
        struct list_ctx c = { cb, ud, 0 };
        iso_walk_dir(im, 0, lba, len, iso_list_cb, &c);
        return c.count;
    }
    return -1;
}

int diskimg_listdir(char letter, const char *relpath, diskimg_dir_cb cb, void *ud) {
    diskimg_t *im = img_acquire(idx_for(letter), 0);
    if (!im) return -1;
    img_enter(im);
    int r = listdir_inner(im, relpath, cb, ud);
    img_leave(im);
    img_release(im);
    return r;
}

// Positional directory step. The walkers are callback-driven, so the cursor is
// implemented by counting entries and stopping at the requested index. A
// directory listing is therefore O(n^2) in the entry count; the largest
// directory on either target disc has 31 entries, and the alternative (handing
// callers a raw on-disc byte offset) would leak the record layout into every
// caller and re-create the class of bug the Rust parser exists to prevent.
struct nth_ctx {
    unsigned want, seen;
    char *name; int cap;
    int *isdir; unsigned *size;
    int hit;
};

static int iso_nth_cb(const char *name, const iso_dirrec_t *rec, void *ud) {
    struct nth_ctx *c = (struct nth_ctx *)ud;
    if (c->seen++ != c->want) return 0;
    int k = 0;
    while (name[k] && k < c->cap - 1) { c->name[k] = name[k]; k++; }
    c->name[k] = 0;
    if (c->isdir) *c->isdir = rec->is_dir ? 1 : 0;
    if (c->size)  *c->size  = rec->len;
    c->hit = 1;
    return 1;
}

static int f12_nth_cb(const unsigned char *e, void *ud) {
    struct nth_ctx *c = (struct nth_ctx *)ud;
    char nm[16]; fat12_name(e, nm);
    if (nm[0] == '.') return 0;
    if (c->seen++ != c->want) return 0;
    int k = 0;
    while (nm[k] && k < c->cap - 1) { c->name[k] = nm[k]; k++; }
    c->name[k] = 0;
    if (c->isdir) *c->isdir = (e[11] & 0x10) ? 1 : 0;
    if (c->size)  *c->size  = rd32le(e + 28);
    c->hit = 1;
    return 1;
}

static int readdir_n_inner(diskimg_t *im, const char *relpath, unsigned index,
                           char *name_out, int name_cap,
                           int *isdir_out, unsigned *size_out) {
    if (!name_out || name_cap < 2) return -1;
    struct nth_ctx c;
    c.want = index; c.seen = 0; c.name = name_out; c.cap = name_cap;
    c.isdir = isdir_out; c.size = size_out; c.hit = 0;
    name_out[0] = 0;

    if (im->fmt == DISKIMG_FMT_ISO9660) {
        uint32_t lba, len; int isdir, multi;
        if (!iso_resolve(im, relpath, &lba, &len, &isdir, &multi) || !isdir) return -1;
        iso_walk_dir(im, 0, lba, len, iso_nth_cb, &c);
        return c.hit ? 1 : 0;
    }
    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, size; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &size, &isdir) || !isdir) return -1;
        fat12_iterdir(im, clus, f12_nth_cb, &c);
        return c.hit ? 1 : 0;
    }
    return -1;
}

int diskimg_readdir_n_gen(char letter, uint32_t gen, const char *relpath, unsigned index,
                          char *name_out, int name_cap,
                          int *isdir_out, unsigned *size_out) {
    diskimg_t *im = img_acquire(idx_for(letter), gen);
    if (!im) return (gen != 0) ? DISKIMG_E_STALE : -1;
    img_enter(im);
    int r = readdir_n_inner(im, relpath, index, name_out, name_cap, isdir_out, size_out);
    img_leave(im);
    img_release(im);
    return r;
}

int diskimg_readdir_n(char letter, const char *relpath, unsigned index,
                      char *name_out, int name_cap,
                      int *isdir_out, unsigned *size_out) {
    return diskimg_readdir_n_gen(letter, 0, relpath, index, name_out, name_cap,
                                 isdir_out, size_out);
}

// ===========================================================================
// #VOLAPI: the mediated view. See the long contract note on dimg_vol_t in
// diskimg.h; this is only its implementation.
//
// BUILT ON THE EXISTING ACCESSORS ON PURPOSE. diskimg_query(),
// diskimg_volume_label() and diskimg_geometry() each already take the table
// lock and the image refcount correctly, and each is already the ONE answer to
// its question. Composing them here means the mediated view cannot disagree
// with the view the mount UI and the DOS layer see, which is exactly the
// failure mode #739 fixed when three files each held their own drive-letter
// constants. The cost is three short lock acquisitions instead of one; this is
// called once per volume at app start, not on a hot path.
// ===========================================================================
int diskimg_vol_root(int idx, char *out, int cap) {
    // The spelling lives here and NOWHERE else. rustkern/drvmap.rs's
    // drvmap_windir_split_rs() is the authority on PARSING this shape back into
    // a letter; this is the authority on WRITING it. Sixteen bytes plus the NUL.
    static const char PFX[] = "/WINDIR/DRIVE_";
    if (!out || cap < (int)sizeof(PFX) + 1) return -1;
    if (idx < 0 || idx > 25) return -1;
    int i = 0;
    for (; PFX[i]; i++) out[i] = PFX[i];
    out[i++] = (char)('A' + idx);
    out[i]   = '\0';
    return 0;
}

int diskimg_volinfo(int idx, dimg_vol_t *out) {
    if (idx < 0 || idx > 25 || !out) return -1;
    memset(out, 0, sizeof(*out));

    diskimg_info_t q;
    if (diskimg_query(idx, &q) != 0) return -1;
    out->letter = q.letter;
    out->cls    = q.cls;
    out->fmt    = q.fmt;
    out->gen    = q.gen;
    out->size   = q.size;
    // Deliberately NOT copied: q.name and q.path. Those name the HOST-side
    // backing file, which is not the app's business and is not part of the
    // contract. The app is given a VFS root, not a provenance.
    out->flags  = q.flags;

    // The root folder exists whether or not a disc is in the drive, so it is
    // written unconditionally: an app that is told "no disc on E:" can still be
    // told where E: will appear when one is inserted.
    if (diskimg_vol_root(idx, out->root, (int)sizeof(out->root)) != 0)
        out->root[0] = '\0';

    if (q.flags & DISKIMG_F_MOUNTED) {
        char letter = (char)('A' + idx);
        // The disc's OWN name. This is the field the whole feature turns on:
        // a DOS guest of the Red Alert generation finds its disc by matching a
        // volume LABEL through INT 21h 4Eh attr 8, not by drive letter, so an
        // API that exposed files but not labels would not answer the question
        // the guest actually asks.
        (void)diskimg_volume_label(letter, out->label, (int)sizeof(out->label));
        (void)diskimg_geometry(letter, &out->bytes_per_sector,
                               &out->sectors_per_cluster, &out->total_clusters);
    }
    return 0;
}
