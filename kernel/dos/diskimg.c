// diskimg.c - removable disk-image mount/eject (#196). See diskimg.h.
//
// The image is loaded whole into RAM at mount time (capped). ISO9660 and FAT12
// readers operate on that buffer. Reads are served via diskimg_try_read(), called
// from fat_read_file, so the DOS + Win16 file APIs transparently see the image.
#include "diskimg.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"

extern fat_fs_t g_fat_fs;
extern void *fat_read_file(fat_fs_t *fs, const char *path, unsigned int *size_out);
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

#define DISKIMG_MAX (16u * 1024u * 1024u)   // cap an in-RAM image at 16 MiB

typedef struct {
    int      fmt;            // DISKIMG_FMT_*
    unsigned char *buf;      // whole image in RAM
    unsigned int   size;     // image size
    char     name[64];       // basename, for the UI
    // FAT12 geometry (computed at mount)
    unsigned bps, spc, rsvd, nfat, rootent, spf;
    unsigned fat_off, root_off, data_off;   // byte offsets
} diskimg_t;

// Slot 0 = A:, slot 1 = E:. (Only removable drives can hold an image.)
static diskimg_t g_img[2];

static int slot_for(char letter) {
    char d = (letter >= 'a' && letter <= 'z') ? (char)(letter - 32) : letter;
    if (d == 'A') return 0;
    if (d == 'E') return 1;
    return -1;
}

static unsigned rd16le(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Case-insensitive compare of a component [a,alen) against NUL-terminated b.
static int comp_eq(const char *a, int alen, const char *b) {
    int i = 0;
    for (; i < alen && b[i]; i++)
        if (upc(a[i]) != upc(b[i])) return 0;
    return (i == alen && b[i] == 0);
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

// ---------------------------------------------------------------------------
// ISO9660 (2048-byte logical sectors), read-only
// ---------------------------------------------------------------------------
#define ISO_SECT 2048

// Find an entry named `want` inside the directory at byte offset dir_off of
// length dir_len. On success fills *out_off (extent byte offset) + *out_len and
// *out_isdir; returns 1. `want` NULL matches nothing (use for listing).
static int iso_find(diskimg_t *im, unsigned dir_off, unsigned dir_len,
                    const char *want, int wantlen,
                    unsigned *out_off, unsigned *out_len, int *out_isdir) {
    unsigned pos = 0;
    while (pos < dir_len) {
        if (dir_off + pos + 33 > im->size) break;
        const unsigned char *r = im->buf + dir_off + pos;
        unsigned reclen = r[0];
        if (reclen == 0) {
            // pad to next logical sector
            unsigned nextsec = ((pos / ISO_SECT) + 1) * ISO_SECT;
            if (nextsec <= pos) break;
            pos = nextsec;
            continue;
        }
        if (dir_off + pos + reclen > im->size) break;
        unsigned ext_lba = rd32le(r + 2);
        unsigned ext_len = rd32le(r + 10);
        unsigned char flags = r[25];
        unsigned fil = r[32];
        const char *fid = (const char *)(r + 33);
        // strip ";1" version suffix on files
        int namelen = (int)fil;
        for (int i = 0; i < namelen; i++) if (fid[i] == ';') { namelen = i; break; }
        // skip self(0x00) / parent(0x01) special single-byte ids
        int special = (fil == 1 && (fid[0] == 0 || fid[0] == 1));
        if (!special && want && comp_eq(fid, namelen, want) && namelen == wantlen) {
            *out_off = ext_lba * ISO_SECT;
            *out_len = ext_len;
            *out_isdir = (flags & 0x02) ? 1 : 0;
            return 1;
        }
        pos += reclen;
    }
    return 0;
}

static int iso_root(diskimg_t *im, unsigned *root_off, unsigned *root_len) {
    if (im->size < 0x8000 + 190) return 0;
    const unsigned char *pvd = im->buf + 16 * ISO_SECT;
    if (pvd[0] != 1) return 0;
    if (!(pvd[1] == 'C' && pvd[2] == 'D' && pvd[3] == '0' && pvd[4] == '0' && pvd[5] == '1'))
        return 0;
    const unsigned char *rootrec = pvd + 156;
    *root_off = rd32le(rootrec + 2) * ISO_SECT;
    *root_len = rd32le(rootrec + 10);
    return 1;
}

// Resolve relpath to a directory's parent + final component for read/list.
// Walks all but the last component as directories. Returns 1 with the final
// container dir in *dir_off/*dir_len (for listing the whole path as a dir, pass
// want_dir=1 and it descends fully). For file read, want_dir=0 returns the file.
static int iso_resolve(diskimg_t *im, const char *relpath, int want_dir,
                       unsigned *res_off, unsigned *res_len, int *res_isdir) {
    unsigned cur_off, cur_len;
    if (!iso_root(im, &cur_off, &cur_len)) return 0;
    const char *p = relpath ? relpath : "";
    char comp[64];
    int isdir = 1;
    unsigned f_off = cur_off, f_len = cur_len;
    int any = 0;
    while (next_comp(&p, comp, sizeof comp)) {
        any = 1;
        unsigned no, nl; int nd;
        if (!iso_find(im, cur_off, cur_len, comp, (int)strlen(comp), &no, &nl, &nd))
            return 0;
        f_off = no; f_len = nl; isdir = nd;
        if (nd) { cur_off = no; cur_len = nl; }
        else { cur_off = no; cur_len = nl; }   // last may be a file
    }
    if (!any) { *res_off = cur_off; *res_len = cur_len; *res_isdir = 1; return 1; }
    if (want_dir && !isdir) return 0;
    *res_off = f_off; *res_len = f_len; *res_isdir = isdir;
    return 1;
}

// ---------------------------------------------------------------------------
// FAT12 (512-byte sectors)
// ---------------------------------------------------------------------------
#define F12_SECT 512

static int fat12_parse(diskimg_t *im) {
    if (im->size < 512) return 0;
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
    if (im->data_off > im->size) return 0;
    return 1;
}

static unsigned fat12_next(diskimg_t *im, unsigned cl) {
    unsigned off = im->fat_off + (cl * 3) / 2;
    if (off + 1 >= im->size) return 0xFFF;
    unsigned v = rd16le(im->buf + off);
    return (cl & 1) ? (v >> 4) : (v & 0xFFF);
}

// Build an 8.3 name string from a dir entry (trims spaces, adds dot+ext).
static void fat12_name(const unsigned char *e, char *out) {
    int n = 0;
    int base = 8; while (base > 0 && e[base - 1] == ' ') base--;
    for (int i = 0; i < base; i++) out[n++] = (char)e[i];
    int ext = 3; while (ext > 0 && e[8 + ext - 1] == ' ') ext--;
    if (ext > 0) { out[n++] = '.'; for (int i = 0; i < ext; i++) out[n++] = (char)e[8 + i]; }
    out[n] = '\0';
}

// Iterate the directory whose first cluster is `clus` (clus==0 => the fixed root
// dir region). For each real entry call cb(entry32, ud); cb returns 1 to stop.
typedef int (*f12_iter_cb)(const unsigned char *e, void *ud);
static void fat12_iterdir(diskimg_t *im, unsigned clus, f12_iter_cb cb, void *ud) {
    if (clus == 0) {
        unsigned n = im->rootent;
        for (unsigned i = 0; i < n; i++) {
            const unsigned char *e = im->buf + im->root_off + i * 32;
            if (im->root_off + i * 32 + 32 > im->size) return;
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
            const unsigned char *e = im->buf + base + i * 32;
            if (base + i * 32 + 32 > im->size) return;
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
    if (comp_eq(f->want, (int)strlen(f->want), nm)) { f->hit = e; return 1; }
    return 0;
}

// Resolve relpath in the FAT12 image. Returns 1 with first cluster + size +
// isdir of the final component (or root if empty).
static int fat12_resolve(diskimg_t *im, const char *relpath,
                         unsigned *clus, unsigned *size, int *isdir) {
    unsigned cur = 0;          // root
    const char *p = relpath ? relpath : "";
    char comp[64];
    int any = 0;
    *clus = 0; *size = 0; *isdir = 1;
    while (next_comp(&p, comp, sizeof comp)) {
        any = 1;
        struct f12_find f = { comp, 0 };
        fat12_iterdir(im, cur, f12_find_cb, &f);
        if (!f.hit) return 0;
        unsigned fc = rd16le(f.hit + 26);
        unsigned sz = rd32le(f.hit + 28);
        int d = (f.hit[11] & 0x10) ? 1 : 0;
        *clus = fc; *size = sz; *isdir = d;
        cur = fc;
    }
    if (!any) { *clus = 0; *size = 0; *isdir = 1; }
    return 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void basename_of(const char *path, char *out, int outsz) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    int n = 0; while (b[n] && n < outsz - 1) { out[n] = b[n]; n++; }
    out[n] = '\0';
}

int diskimg_mount(char letter, const char *imgpath) {
    int s = slot_for(letter);
    if (s < 0) return -1;
    if (!imgpath || !imgpath[0]) return -1;
    diskimg_eject(letter);

    unsigned int sz = 0;
    void *d = fat_read_file(&g_fat_fs, imgpath, &sz);
    if (!d || sz == 0) { if (d) kfree(d); return -2; }
    if (sz > DISKIMG_MAX) { kfree(d); kprintf("[diskimg] %s too large (%u)\n", imgpath, sz); return -3; }

    diskimg_t *im = &g_img[s];
    im->buf = (unsigned char *)d;
    im->size = sz;
    basename_of(imgpath, im->name, sizeof im->name);

    // Detect format. ISO9660: "CD001" at sector 16. FAT12: valid BPB.
    unsigned ro, rl;
    if (iso_root(im, &ro, &rl)) {
        im->fmt = DISKIMG_FMT_ISO9660;
    } else if (fat12_parse(im)) {
        im->fmt = DISKIMG_FMT_FAT12;
    } else {
        kprintf("[diskimg] %s: unrecognized image format\n", imgpath);
        kfree(im->buf); im->buf = 0; im->size = 0; im->fmt = DISKIMG_FMT_NONE;
        return -4;
    }
    kprintf("[diskimg] mounted %s on %c: (%s, %u bytes)\n", im->name, upc(letter),
            im->fmt == DISKIMG_FMT_ISO9660 ? "ISO9660" : "FAT12", im->size);
    return 0;
}

void diskimg_eject(char letter) {
    int s = slot_for(letter);
    if (s < 0) return;
    if (g_img[s].buf) kfree(g_img[s].buf);
    diskimg_t z; memset(&z, 0, sizeof z);
    g_img[s] = z;
}

int diskimg_is_mounted(char letter) {
    int s = slot_for(letter);
    return (s >= 0 && g_img[s].fmt != DISKIMG_FMT_NONE && g_img[s].buf) ? 1 : 0;
}

int diskimg_format(char letter) {
    int s = slot_for(letter);
    return (s >= 0) ? g_img[s].fmt : DISKIMG_FMT_NONE;
}

const char *diskimg_mounted_name(char letter) {
    int s = slot_for(letter);
    return (s >= 0 && diskimg_is_mounted(letter)) ? g_img[s].name : "";
}

void *diskimg_read_file(char letter, const char *relpath, unsigned int *size_out) {
    int s = slot_for(letter);
    if (s < 0 || !diskimg_is_mounted(letter)) return 0;
    diskimg_t *im = &g_img[s];
    if (size_out) *size_out = 0;

    if (im->fmt == DISKIMG_FMT_ISO9660) {
        unsigned off, len; int isdir;
        if (!iso_resolve(im, relpath, 0, &off, &len, &isdir) || isdir) return 0;
        if (off + len > im->size) { if (len > im->size - off) len = im->size - off; }
        unsigned char *out = (unsigned char *)kmalloc(len ? len : 1);
        if (!out) return 0;
        for (unsigned i = 0; i < len; i++) out[i] = im->buf[off + i];
        if (size_out) *size_out = len;
        return out;
    }
    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, size; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &size, &isdir) || isdir) return 0;
        unsigned char *out = (unsigned char *)kmalloc(size ? size : 1);
        if (!out) return 0;
        unsigned got = 0, cl = clus, guard = 0;
        unsigned cbytes = im->spc * F12_SECT;
        while (cl >= 2 && cl < 0xFF8 && got < size && guard++ < 65536) {
            unsigned base = im->data_off + (cl - 2) * cbytes;
            unsigned n = cbytes; if (got + n > size) n = size - got;
            if (base + n > im->size) n = (base < im->size) ? im->size - base : 0;
            for (unsigned i = 0; i < n; i++) out[got + i] = im->buf[base + i];
            got += n;
            cl = fat12_next(im, cl);
        }
        if (size_out) *size_out = got;
        return out;
    }
    return 0;
}

// listdir
struct list_ctx { diskimg_dir_cb cb; void *ud; int count; };
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

int diskimg_listdir(char letter, const char *relpath, diskimg_dir_cb cb, void *ud) {
    int s = slot_for(letter);
    if (s < 0 || !diskimg_is_mounted(letter) || !cb) return -1;
    diskimg_t *im = &g_img[s];

    if (im->fmt == DISKIMG_FMT_FAT12) {
        unsigned clus, size; int isdir;
        if (!fat12_resolve(im, relpath, &clus, &size, &isdir) || !isdir) return -1;
        struct list_ctx c = { cb, ud, 0 };
        fat12_iterdir(im, clus, f12_list_cb, &c);
        return c.count;
    }
    if (im->fmt == DISKIMG_FMT_ISO9660) {
        unsigned off, len; int isdir;
        if (!iso_resolve(im, relpath, 1, &off, &len, &isdir) || !isdir) return -1;
        int count = 0;
        unsigned pos = 0;
        while (pos < len) {
            if (off + pos + 33 > im->size) break;
            const unsigned char *r = im->buf + off + pos;
            unsigned reclen = r[0];
            if (reclen == 0) { unsigned ns = ((pos / ISO_SECT) + 1) * ISO_SECT; if (ns <= pos) break; pos = ns; continue; }
            unsigned ext_len = rd32le(r + 10);
            unsigned char flags = r[25];
            unsigned fil = r[32];
            const char *fid = (const char *)(r + 33);
            int namelen = (int)fil;
            for (int i = 0; i < namelen; i++) if (fid[i] == ';') { namelen = i; break; }
            int special = (fil == 1 && (fid[0] == 0 || fid[0] == 1));
            if (!special && namelen > 0) {
                char nm[64]; int k = 0;
                for (int i = 0; i < namelen && k < 63; i++) nm[k++] = fid[i];
                nm[k] = '\0';
                cb(nm, (flags & 0x02) ? 1 : 0, ext_len, ud);
                count++;
            }
            pos += reclen;
        }
        return count;
    }
    return -1;
}

// fat_read_file hook: only "/WINDIR/DRIVE_E/.." or "/WINDIR/DRIVE_A/.." when an
// image is mounted on that drive; everything else returns NULL (normal read).
void *diskimg_try_read(const char *path, unsigned int *size_out) {
    if (!path) return 0;
    const char *pfx = "/WINDIR/DRIVE_";
    int i = 0; while (pfx[i]) { if (path[i] != pfx[i]) return 0; i++; }
    char letter = path[i];           // drive letter
    if (letter != 'A' && letter != 'E') return 0;
    if (!diskimg_is_mounted(letter)) return 0;
    const char *rel = path + i + 1;  // skip the letter
    while (*rel == '/' || *rel == '\\') rel++;
    return diskimg_read_file(letter, rel, size_out);
}
