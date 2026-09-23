// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// Compound File Binary (OLE2) reader: the container of legacy .doc/.xls/.ppt.
// Read-only, freestanding-pure C (byte parsing only, no MayteraOS syscalls).
// Owner: agent 8 (fmt_ole2/cfb.c). Contract frozen in include/cfb.h.
//
// Parses: the 512-byte header, the DIFAT + FAT sector chains, the directory
// (128-byte entries), and reassembles a named stream from either the regular
// FAT (size >= mini-stream cutoff) or the mini-FAT + root mini-stream (smaller).
// Reference: [MS-CFB] Compound File Binary File Format.
#include "cfb.h"
#include <stdlib.h>
#include <string.h>

// ---- special FAT sector values ([MS-CFB] 2.2) ----
#define CFB_MAXREGSECT 0xFFFFFFFAu
#define CFB_DIFSECT    0xFFFFFFFCu
#define CFB_FATSECT    0xFFFFFFFDu
#define CFB_ENDOFCHAIN 0xFFFFFFFEu
#define CFB_FREESECT   0xFFFFFFFFu
#define CFB_NOSTREAM   0xFFFFFFFFu

// directory object types
#define CFB_T_UNKNOWN 0
#define CFB_T_STORAGE 1
#define CFB_T_STREAM  2
#define CFB_T_ROOT    5

typedef struct {
    char          name[64];   // ASCII (low byte of each UTF-16 unit), NUL-terminated
    unsigned char type;
    unsigned int  start;      // starting sector
    unsigned long size;       // stream size in bytes
} cfb_dirent;

struct cfb {
    const unsigned char *data;
    unsigned long        len;
    unsigned int         sector_size;   // 1<<sector_shift (512 or 4096)
    unsigned int         mini_size;      // 1<<mini_sector_shift (usually 64)
    unsigned int         mini_cutoff;    // usually 4096

    unsigned int        *fat;            // fat[s] = next sector after s
    unsigned int         fat_n;          // entries in fat
    unsigned int        *minifat;
    unsigned int         minifat_n;

    cfb_dirent          *dir;
    int                  ndir;

    unsigned char       *ministream;     // root entry's mini-stream, reassembled
    unsigned long        ministream_len;

    // lazily-reassembled per-directory-entry stream cache
    unsigned char      **cache_buf;      // ndir entries
    unsigned long       *cache_len;
};

// ---- little-endian readers (bounds-checked by caller) ----
static unsigned int rd16(const unsigned char *p){ return (unsigned int)p[0] | ((unsigned int)p[1]<<8); }
static unsigned int rd32(const unsigned char *p){
    return (unsigned int)p[0] | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}
static unsigned long long rd64(const unsigned char *p){
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p+4)<<32);
}

// byte offset of sector s in the file body. Header occupies the first
// sector_size bytes (512 padded to 4096 in v4), so sector s starts at
// (s+1)*sector_size. Returns 0 and *ok=0 if s is out of the file.
static unsigned long sect_off(cfb *c, unsigned int s, int *ok){
    unsigned long off = (unsigned long)(s + 1) * (unsigned long)c->sector_size;
    if (off + c->sector_size > c->len) { *ok = 0; return 0; }
    *ok = 1; return off;
}

static void cfb_free_internal(cfb *c){
    if (!c) return;
    if (c->cache_buf){ for (int i=0;i<c->ndir;i++) free(c->cache_buf[i]); free(c->cache_buf); }
    free(c->cache_len);
    free(c->ministream);
    free(c->dir);
    free(c->minifat);
    free(c->fat);
    free(c);
}

// Reassemble a chain from the regular FAT into a fresh buffer of `want` bytes
// (want may be less than the chain's sector-rounded length; we copy min).
// Returns malloc'd buffer or NULL. On short/broken chain, fills what it can and
// zero-pads the rest so callers never read uninitialised memory.
static unsigned char *read_fat_chain(cfb *c, unsigned int start, unsigned long want, unsigned long *got){
    if (got) *got = 0;
    if (want > c->len) want = c->len;    // a chain can never exceed the file
    unsigned char *out = (unsigned char*)malloc(want ? want : 1);
    if (!out) return 0;
    if (want) memset(out, 0, want);
    unsigned long filled = 0;
    unsigned int s = start;
    unsigned int guard = 0;              // cap iterations to defeat cycles
    while (s <= CFB_MAXREGSECT && filled < want){
        if (guard++ > c->fat_n) break;   // cyclic/over-long chain
        int ok; unsigned long off = sect_off(c, s, &ok);
        if (!ok) break;
        unsigned long chunk = c->sector_size;
        if (chunk > want - filled) chunk = want - filled;
        memcpy(out + filled, c->data + off, chunk);
        filled += chunk;
        if (s >= c->fat_n) break;        // no next link available
        s = c->fat[s];
    }
    if (got) *got = filled;
    return out;
}

// Reassemble a chain from the mini-FAT out of the (already reassembled)
// mini-stream. Returns malloc'd buffer or NULL.
static unsigned char *read_mini_chain(cfb *c, unsigned int start, unsigned long want, unsigned long *got){
    if (got) *got = 0;
    if (want > c->len) want = c->len;    // bounded by the file (mini-stream <= file)
    unsigned char *out = (unsigned char*)malloc(want ? want : 1);
    if (!out) return 0;
    if (want) memset(out, 0, want);
    unsigned long filled = 0;
    unsigned int s = start;
    unsigned int guard = 0;
    while (s <= CFB_MAXREGSECT && filled < want){
        if (guard++ > c->minifat_n) break;
        unsigned long off = (unsigned long)s * (unsigned long)c->mini_size;
        if (off + c->mini_size > c->ministream_len){
            // partial last mini-sector at the tail of the mini-stream
            if (off >= c->ministream_len) break;
        }
        unsigned long avail = (off < c->ministream_len) ? (c->ministream_len - off) : 0;
        unsigned long chunk = c->mini_size;
        if (chunk > avail) chunk = avail;
        if (chunk > want - filled) chunk = want - filled;
        if (chunk == 0) break;
        memcpy(out + filled, c->ministream + off, chunk);
        filled += chunk;
        if (s >= c->minifat_n) break;
        s = c->minifat[s];
    }
    if (got) *got = filled;
    return out;
}

cfb *cfb_open(const unsigned char *bytes, unsigned long len){
    if (!bytes || len < 512) return 0;
    static const unsigned char SIG[8] = {0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1};
    if (memcmp(bytes, SIG, 8) != 0) return 0;

    cfb *c = (cfb*)malloc(sizeof(*c));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->data = bytes; c->len = len;

    unsigned int sector_shift = rd16(bytes + 0x1E);
    unsigned int mini_shift   = rd16(bytes + 0x20);
    if (sector_shift < 7 || sector_shift > 20){ free(c); return 0; }   // 128..1MiB
    if (mini_shift < 2 || mini_shift > sector_shift){ free(c); return 0; }
    c->sector_size = 1u << sector_shift;
    c->mini_size   = 1u << mini_shift;
    c->mini_cutoff = rd32(bytes + 0x38);
    if (c->mini_cutoff == 0) c->mini_cutoff = 4096;
    if (c->sector_size > len){ free(c); return 0; }

    unsigned int num_fat_sects   = rd32(bytes + 0x2C);
    unsigned int first_dir_sect  = rd32(bytes + 0x30);
    unsigned int first_mfat_sect = rd32(bytes + 0x3C);
    unsigned int num_mfat_sects  = rd32(bytes + 0x40);
    unsigned int first_difat     = rd32(bytes + 0x44);
    unsigned int num_difat_sects = rd32(bytes + 0x48);

    // total number of body sectors that could exist in the file
    unsigned long total_sects = (len / c->sector_size);
    if (total_sects) total_sects -= 1;    // minus the header sector
    if (total_sects == 0){ free(c); return 0; }

    // ---- collect the list of FAT sector numbers from the DIFAT ----
    // DIFAT[0..108] live in the header at 0x4C; the rest chain through DIFAT
    // sectors. Guard sizes against absurd headers.
    if (num_fat_sects > total_sects + 1){ free(c); return 0; }
    unsigned int *fatlist = (unsigned int*)malloc((num_fat_sects ? num_fat_sects : 1) * sizeof(unsigned int));
    if (!fatlist){ free(c); return 0; }
    unsigned int fatlist_n = 0;

    for (unsigned int i = 0; i < 109 && fatlist_n < num_fat_sects; i++){
        unsigned int fs = rd32(bytes + 0x4C + i*4);
        if (fs <= CFB_MAXREGSECT) fatlist[fatlist_n++] = fs;
    }
    // walk DIFAT sector chain for any remaining FAT-sector pointers
    {
        unsigned int ds = first_difat;
        unsigned int per = c->sector_size / 4;    // last slot is the next-DIFAT link
        unsigned int guard = 0;
        while (ds <= CFB_MAXREGSECT && fatlist_n < num_fat_sects && guard++ <= num_difat_sects + 1){
            int ok; unsigned long off = sect_off(c, ds, &ok);
            if (!ok) break;
            for (unsigned int i = 0; i + 1 < per && fatlist_n < num_fat_sects; i++){
                unsigned int fs = rd32(c->data + off + i*4);
                if (fs <= CFB_MAXREGSECT) fatlist[fatlist_n++] = fs;
            }
            ds = rd32(c->data + off + (per - 1)*4);
        }
    }

    // ---- build the FAT: concatenate every FAT sector as a uint32 array ----
    unsigned int ents_per_sect = c->sector_size / 4;
    c->fat_n = fatlist_n * ents_per_sect;
    if (c->fat_n == 0){ free(fatlist); free(c); return 0; }
    c->fat = (unsigned int*)malloc((unsigned long)c->fat_n * sizeof(unsigned int));
    if (!c->fat){ free(fatlist); free(c); return 0; }
    for (unsigned int i = 0; i < fatlist_n; i++){
        int ok; unsigned long off = sect_off(c, fatlist[i], &ok);
        for (unsigned int j = 0; j < ents_per_sect; j++)
            c->fat[i*ents_per_sect + j] = ok ? rd32(c->data + off + j*4) : CFB_FREESECT;
    }
    free(fatlist);

    // ---- build the mini-FAT (chain of sectors from the regular FAT) ----
    if (num_mfat_sects){
        unsigned long mfat_bytes = (unsigned long)num_mfat_sects * c->sector_size;
        unsigned long got = 0;
        unsigned char *mf = read_fat_chain(c, first_mfat_sect, mfat_bytes, &got);
        if (mf){
            c->minifat_n = (unsigned int)(got / 4);
            c->minifat = (unsigned int*)malloc((c->minifat_n ? c->minifat_n : 1) * sizeof(unsigned int));
            if (c->minifat)
                for (unsigned int i = 0; i < c->minifat_n; i++) c->minifat[i] = rd32(mf + i*4);
            else c->minifat_n = 0;
            free(mf);
        }
    }

    // ---- parse the directory (chain from the regular FAT) ----
    unsigned long dir_bytes = 0;
    {
        // measure the directory chain length first (sectors), capped
        unsigned int s = first_dir_sect, guard = 0, cnt = 0;
        while (s <= CFB_MAXREGSECT && guard++ <= c->fat_n){
            int ok; (void)sect_off(c, s, &ok); if (!ok) break;
            cnt++;
            if (s >= c->fat_n) break;
            s = c->fat[s];
        }
        dir_bytes = (unsigned long)cnt * c->sector_size;
    }
    if (dir_bytes){
        unsigned long got = 0;
        unsigned char *db = read_fat_chain(c, first_dir_sect, dir_bytes, &got);
        if (db){
            int maxent = (int)(got / 128);
            if (maxent > 0){
                c->dir = (cfb_dirent*)malloc((unsigned long)maxent * sizeof(cfb_dirent));
                if (c->dir){
                    for (int i = 0; i < maxent; i++){
                        const unsigned char *e = db + (unsigned long)i*128;
                        cfb_dirent *d = &c->dir[i];
                        memset(d, 0, sizeof(*d));
                        unsigned int nlen = rd16(e + 0x40);   // bytes incl. terminator
                        d->type  = e[0x42];
                        d->start = rd32(e + 0x74);
                        d->size  = (unsigned long)rd64(e + 0x78);
                        // Version-3 (512-byte sector) quirk: some old writers
                        // (Word 6/95) leave garbage in the high 32 bits of the
                        // 64-bit size. Per [MS-CFB] those bits MUST be zero on
                        // v3; mask them so we never malloc a bogus multi-PB
                        // buffer. On v4 (>512) sizes can legitimately exceed 4GB.
                        if (c->sector_size == 512) d->size &= 0xFFFFFFFFu;
                        // A stream can never be larger than the whole file; clamp
                        // so a corrupt size can never drive a giant allocation.
                        if (d->size > c->len) d->size = c->len;
                        // convert UTF-16LE name -> ASCII low byte
                        unsigned int nchars = (nlen >= 2) ? (nlen/2 - 1) : 0;
                        if (nchars > 31) nchars = 31;
                        unsigned int k = 0;
                        for (unsigned int j = 0; j < nchars; j++){
                            unsigned int u = rd16(e + j*2);
                            d->name[k++] = (u < 0x80) ? (char)u : '?';
                        }
                        d->name[k] = 0;
                        c->ndir = i + 1;
                    }
                }
            }
            free(db);
        }
    }
    if (!c->dir || c->ndir <= 0){ cfb_free_internal(c); return 0; }

    // ---- reassemble the root entry's mini-stream from the regular FAT ----
    // The root entry (type 5) is conventionally dir[0]; scan to be safe.
    {
        int root = -1;
        for (int i = 0; i < c->ndir; i++) if (c->dir[i].type == CFB_T_ROOT){ root = i; break; }
        if (root >= 0 && c->dir[root].size){
            c->ministream_len = c->dir[root].size;
            unsigned long got = 0;
            c->ministream = read_fat_chain(c, c->dir[root].start, c->ministream_len, &got);
            if (!c->ministream) c->ministream_len = 0;
            else if (got < c->ministream_len) c->ministream_len = got;
        }
    }

    // per-entry stream cache
    c->cache_buf = (unsigned char**)malloc((unsigned long)c->ndir * sizeof(unsigned char*));
    c->cache_len = (unsigned long*)malloc((unsigned long)c->ndir * sizeof(unsigned long));
    if (!c->cache_buf || !c->cache_len){ cfb_free_internal(c); return 0; }
    for (int i = 0; i < c->ndir; i++){ c->cache_buf[i] = 0; c->cache_len[i] = 0; }

    return c;
}

// find a stream directory entry by ASCII name (case-sensitive exact match)
static int cfb_find(cfb *c, const char *name){
    if (!c || !name) return -1;
    for (int i = 0; i < c->ndir; i++)
        if (c->dir[i].type == CFB_T_STREAM && strcmp(c->dir[i].name, name) == 0) return i;
    return -1;
}

const unsigned char *cfb_stream(cfb *c, const char *name, unsigned long *out_len){
    if (out_len) *out_len = 0;
    int i = cfb_find(c, name);
    if (i < 0) return 0;
    if (c->cache_buf[i]){ if (out_len) *out_len = c->cache_len[i]; return c->cache_buf[i]; }

    unsigned long size = c->dir[i].size;
    unsigned char *buf; unsigned long got = 0;
    if (size >= c->mini_cutoff)
        buf = read_fat_chain(c, c->dir[i].start, size, &got);
    else
        buf = read_mini_chain(c, c->dir[i].start, size, &got);
    if (!buf) return 0;

    c->cache_buf[i] = buf;
    c->cache_len[i] = size;      // logical stream size (chain may over-provide)
    if (out_len) *out_len = size;
    return buf;
}

int cfb_has(cfb *c, const char *name){ return cfb_find(c, name) >= 0; }

void cfb_close(cfb *c){ cfb_free_internal(c); }
