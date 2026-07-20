// ttf.c - TrueType font renderer for MayteraOS
// Adapted for freestanding kernel environment.
//
// Multi-face font registry (OS-wide fonts): face 0 is the default UI font
// (/FONT.TTF, kept identical to the historical single-font behaviour). Any
// additional .ttf/.otf files found in /FONTS are registered as extra faces so
// apps (the Font Browser, Maytera Studio's text tool, ...) can select among
// installed families. All the public "active face" functions keep working on
// face 0 unless ttf_set_active_face() moves them, and every function has a
// _f(face,...) variant that names the face explicitly (used by the syscalls).

#include "ttf.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"
#include "../fs/fat.h"
#include "../video/framebuffer.h"
#include "../cpu/mono.h"

extern void bootlog_write(const char *fmt, ...);

// ============================================================================
// Configure stb_truetype for freestanding kernel environment
// ============================================================================
#define STBTT_STATIC
// Use V1 rasterizer (V2 has issues with narrow vertical glyphs like lowercase l)
#define STBTT_RASTERIZER_VERSION 1
#define STB_TRUETYPE_IMPLEMENTATION

// Map to kernel memory allocator
#define STBTT_malloc(x,u)  ((void)(u), kmalloc(x))
#define STBTT_free(x,u)    ((void)(u), kfree(x))

// Map to kernel string functions
#define STBTT_memcpy  memcpy
#define STBTT_memset  memset
#define STBTT_strlen  strlen

// Disable assert
#define STBTT_assert(x)  ((void)0)

// Math function declarations (implemented in gui/math.c)
extern double floor(double x);
extern double ceil(double x);
extern double sqrt(double x);
extern double pow(double x, double y);
extern double fmod(double x, double y);
extern double cos(double x);
extern double acos(double x);
extern double fabs(double x);

// Define stb_truetype math macros to avoid #include <math.h>
#define STBTT_ifloor(x)   ((int) floor(x))
#define STBTT_iceil(x)    ((int) ceil(x))
#define STBTT_sqrt(x)     sqrt(x)
#define STBTT_pow(x,y)    pow(x,y)
#define STBTT_fmod(x,y)   fmod(x,y)
#define STBTT_cos(x)      cos(x)
#define STBTT_acos(x)     acos(x)
#define STBTT_fabs(x)     fabs(x)

// Also prevent stdlib.h include (we already defined STBTT_malloc/free above)
// and string.h include (we defined STBTT_memcpy/memset/strlen above)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

// ============================================================================
// Font file paths (8.3 FAT compatible)
// ============================================================================
#define TTF_FONT_PATH "/FONT.TTF"
#define TTF_FONTS_DIR "/FONTS"

// ============================================================================
// Glyph cache (per size, per face)
// ============================================================================
#define MAX_CACHED_GLYPHS 128
#define NUM_SIZE_CACHES   8

typedef struct {
    int codepoint;
    int style;
    ttf_glyph_t glyph;
} glyph_cache_entry_t;

typedef struct {
    int size;
    float scale;
    glyph_cache_entry_t entries[MAX_CACHED_GLYPHS];
    int count;
} size_cache_t;

// One installed font face.
#define TTF_MAX_FACES 64
#define TTF_NAME_MAX  48
#define TTF_STYLE_MAX 32
#define TTF_PATH_MAX  64
typedef struct {
    int             in_use;
    char            name[TTF_NAME_MAX];   // typographic family (name ID 16, else ID 1)
    char            style[TTF_STYLE_MAX]; // typographic subfamily (ID 17, else ID 2)
    char            path[TTF_PATH_MAX];   // source file, so a rescan can skip it
    uint8_t        *data;                  // font file bytes (owned)
    stbtt_fontinfo  info;
    size_cache_t    caches[NUM_SIZE_CACHES];
} font_face_t;

static font_face_t g_faces[TTF_MAX_FACES];
static int g_nfaces = 0;
static int g_active = 0;   // active face for the legacy (non-_f) API
static int ttf_ready = 0;

static const int size_cache_sizes[NUM_SIZE_CACHES] = {
    TTF_SIZE_SMALL,  // 12
    14,              // browser small
    TTF_SIZE_NORMAL, // 16
    18,              // medium
    20,              // browser h3
    TTF_SIZE_LARGE,  // 24
    28,              // browser h1
    TTF_SIZE_XLARGE  // 32
};

static size_cache_t *get_size_cache(font_face_t *f, int size) {
    // Exact match first
    for (int i = 0; i < NUM_SIZE_CACHES; i++)
        if (f->caches[i].size == size) return &f->caches[i];
    // Closest match
    int best = 0, best_diff = 1000;
    for (int i = 0; i < NUM_SIZE_CACHES; i++) {
        int diff = f->caches[i].size - size;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return &f->caches[best];
}

static font_face_t *face_at(int idx) {
    if (idx < 0 || idx >= g_nfaces || !g_faces[idx].in_use) return &g_faces[0];
    return &g_faces[idx];
}

// ============================================================================
// Name-table extraction: pull a human family name out of the TTF so the Font
// Browser and Studio show "DejaVu Sans" / "JetBrainsMono Nerd Font" etc.
// stbtt returns UTF-16BE; we keep the ASCII subset (good enough for the Latin
// family names these fonts use). Falls back to the supplied default.
// ============================================================================
// Pull one name-table record (by name ID) into `out` as ASCII. Returns length.
// stbtt hands back UTF-16BE for the Microsoft platform and 1 byte/char for Mac;
// we keep the printable ASCII subset, which covers the Latin family names.
static int extract_name_id(stbtt_fontinfo *info, int name_id, char *out, int cap) {
    int len = 0;
    const char *s = stbtt_GetFontNameString(
        info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
        STBTT_MS_LANG_ENGLISH, name_id);
    if (s && len > 0) {
        int j = 0;
        for (int i = 1; i < len && j < cap - 1; i += 2) {   // UTF-16BE low byte
            char c = s[i];
            if (c >= 32 && c < 127) out[j++] = c;
        }
        out[j] = 0;
        if (j > 0) return j;
    }
    // Some fonts only fill the Mac platform records.
    len = 0;
    s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, 0, 0, name_id);
    if (s && len > 0) {
        int j = 0;
        for (int i = 0; i < len && j < cap - 1; i++) {
            char c = s[i];
            if (c >= 32 && c < 127) out[j++] = c;
        }
        out[j] = 0;
        if (j > 0) return j;
    }
    out[0] = 0;
    return 0;
}

// Extract the TYPOGRAPHIC family and subfamily.
//
// WHY ID 16/17 AND NOT 1/2 (#351): name ID 1 is the *legacy* family, which is
// capped at four styles per family (Regular/Bold/Italic/Bold Italic), so any
// family with more weights than that splits itself across several ID-1 families
// to stay inside the limit. Measured on the shipped open-fonts set:
//   SourceCodePro-SemiBoldItalic.ttf  ID1='Source Code Pro Semibold' ID2='Italic'
//                                     ID16='Source Code Pro'  ID17='Semibold Italic'
//   Lato-Heavy.ttf                    ID1='Lato Heavy'  ID2='Regular'
//                                     ID16='Lato'       ID17='Heavy'
// Keying off ID 1 would therefore invent bogus families ('Lato Heavy') and file
// a weight under the wrong one. ID 16/17 (typographic family/subfamily) carry
// the real grouping, but they are OPTIONAL and are omitted exactly when ID 1/2
// are already correct (SourceCodePro-Regular.ttf has no ID16/17 at all), so the
// rule is: prefer 16/17, fall back to 1/2. That is what fontconfig and the
// Windows font mapper do. Deriving any of this from the FILENAME is also wrong:
// the same set ships DejaVuSans-Bold.ttf whose real family is 'DejaVu Sans'.
static void extract_names(stbtt_fontinfo *info, char *fam, int famcap,
                          char *sty, int stycap, const char *fallback) {
    if (extract_name_id(info, 16, fam, famcap) == 0)      // typographic family
        if (extract_name_id(info, 1, fam, famcap) == 0)   // legacy family
            extract_name_id(info, 4, fam, famcap);        // full name
    if (fam[0] == 0) {
        strncpy(fam, fallback, famcap - 1);
        fam[famcap - 1] = 0;
    }

    if (extract_name_id(info, 17, sty, stycap) == 0)      // typographic subfamily
        extract_name_id(info, 2, sty, stycap);            // legacy subfamily
    if (sty[0] == 0) {
        strncpy(sty, "Regular", stycap - 1);
        sty[stycap - 1] = 0;
    }
    // DejaVu calls its upright weight 'Book'; normalise so the style list reads
    // the same across families and 'Regular' is always the one apps default to.
    if (strcmp(sty, "Book") == 0) {
        strncpy(sty, "Regular", stycap - 1);
        sty[stycap - 1] = 0;
    }
}

// ============================================================================
// Face loading (LAZY, #536)
// ============================================================================
// A face is ENROLLED cheaply at scan time: we read only its sfnt directory and
// name table (a few KB), pull the family/style out, and leave `data == NULL`.
// The full font file (up to 3 MB for Symbola) is slurped and stbtt-initialised
// ONLY on the first glyph/metric/advance/kern call for that face
// (ensure_face_loaded), then cached for the life of the boot. Enumeration
// (Font Book, the font pickers) touches only name/style, so listing all 53
// installed faces no longer pays to read 21 MB up front. Before this the whole
// library was read eagerly in ttf_init(), which measured ~25 s at boot on the
// USB-MSC golden (face0=0.9s, scan=24.8s), stalling the desktop bring-up.
//
// data == NULL  => metadata-only (enrolled, enumerable, not yet renderable)
// data != NULL  => fully loaded (info valid, size caches primed, renderable)

// Read `len` bytes starting at byte `off` of a font file into `buf`. The /FONTS
// store lives on the FAT ESP (verified on golden 859: p1/FONTS, not the ext2
// root), so we open it straight through the FAT layer and fat_seek() to the
// name table, which sits at the END of the file (after glyf) in every shipped
// face. Returns bytes read, or -1. NOTE: this cheap-enumeration path is
// FAT-only; the full-load path below uses the g_root_ext2-aware fat_read_file(),
// so if /FONTS is ever moved onto ext2 the faces still LOAD and RENDER, they
// just fall back to the eager full read for their name (see enroll_face_meta).
static int font_read_range(const char *path, uint32_t off, uint32_t len, void *buf) {
    extern fat_fs_t g_fat_fs;
    fat_file_t file;
    if (fat_open(&g_fat_fs, path, &file) != 0) return -1;
    if (file.is_dir) { fat_close(&file); return -1; }
    if (off && fat_seek(&file, off) != 0) { fat_close(&file); return -1; }
    int n = fat_read(&file, buf, len);
    fat_close(&file);
    return n;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wbe32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

// Full load: read the whole font file (routed FAT/ext2), stbtt-init it, prime
// the per-size scale caches. Used for face 0 (needed immediately for all UI
// text) and lazily by ensure_face_loaded(). `fill_names` also extracts the
// family/style (used by the enroll fallback, which has no cheap name yet).
static int face_full_load(font_face_t *f, const char *fallback_name, int fill_names) {
    extern fat_fs_t g_fat_fs;
    uint32_t fsize = 0;
    void *data = fat_read_file(&g_fat_fs, f->path, &fsize);
    if (!data || fsize == 0) { if (data) kfree(data); return -1; }

    int offset = stbtt_GetFontOffsetForIndex((uint8_t *)data, 0);
    stbtt_fontinfo info;
    if (offset < 0 || !stbtt_InitFont(&info, (uint8_t *)data, offset)) {
        kfree(data);
        return -1;
    }
    f->info = info;
    for (int i = 0; i < NUM_SIZE_CACHES; i++) {
        f->caches[i].size  = size_cache_sizes[i];
        f->caches[i].scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_cache_sizes[i]);
        f->caches[i].count = 0;
    }
    if (fill_names)
        extract_names(&f->info, f->name, TTF_NAME_MAX, f->style, TTF_STYLE_MAX, fallback_name);
    // Publish `data` LAST: a concurrent renderer keys off data != NULL, so it
    // must only see the pointer once info + caches are fully built.
    f->data = (uint8_t *)data;
    return 0;
}

// Lazily bring a face's outline data resident on first render. Cheap and
// idempotent once loaded. MUST run with interrupts ENABLED (it does block-layer
// I/O), so callers invoke it BEFORE the cli-guarded glyph-cache section.
//
// Loads IN PLACE via face_full_load(), which writes f->info and the size caches
// first and publishes f->data LAST, so a concurrent renderer that observes
// data == NULL never touches a half-built face. font_face_t is ~64 KB (the
// glyph caches), so we deliberately do NOT copy it onto the (small) kernel
// stack. On the vanishingly rare true race two callers both read the file and
// one font buffer leaks once; TTF drawing is in practice serialised by the
// single compositor draw path, so the sequential case (no race, no leak) is
// what actually runs.
static int ensure_face_loaded(font_face_t *f) {
    if (!f || !f->in_use) return 0;
    if (f->data) return 1;                 // already resident
    return face_full_load(f, f->name, 0) == 0;
}

// Enroll a face using ONLY its name table (cheap). Reads the sfnt header +
// directory (<=8 KB from the start), locates the `name` table, reads just that
// table (a few KB) from the tail of the file, and reuses extract_names() by
// splicing the header+directory and the relocated name table into one small
// buffer. Leaves data == NULL (metadata-only). Falls back to a full eager load
// on anything unexpected (TTC, name table absent, short read), so no shipped
// face is ever lost, it just is not lazy.
#define FONT_HDR_MAX 8192
static int enroll_face_meta(const char *path, const char *fallback_name) {
    if (g_nfaces >= TTF_MAX_FACES) return -1;
    font_face_t *f = &g_faces[g_nfaces];
    memset(f, 0, sizeof(*f));
    strncpy(f->path, path, TTF_PATH_MAX - 1);
    f->path[TTF_PATH_MAX - 1] = 0;

    uint8_t *hdr = kmalloc(FONT_HDR_MAX);
    int cheap_ok = 0;
    if (hdr) {
        int hn = font_read_range(path, 0, FONT_HDR_MAX, hdr);
        if (hn >= 12) {
            uint32_t ver = be32(hdr);
            // Accept only single-font sfnt containers (TrueType 0x00010000,
            // 'OTTO', 'true', 'typ1'); a 'ttcf' collection or anything else
            // routes to the full loader, which handles the offset table.
            int known = (ver == 0x00010000 || ver == 0x4F54544F /*OTTO*/ ||
                         ver == 0x74727565 /*true*/ || ver == 0x74797031 /*typ1*/);
            uint32_t numTables = be16(hdr + 4);
            uint32_t dir_end = 12 + numTables * 16;
            if (known && numTables > 0 && dir_end <= (uint32_t)hn) {
                uint32_t name_pos = 0, name_off = 0, name_len = 0;
                for (uint32_t i = 0; i < numTables; i++) {
                    const uint8_t *e = hdr + 12 + i * 16;
                    if (e[0]=='n'&&e[1]=='a'&&e[2]=='m'&&e[3]=='e') {
                        name_pos = 12 + i * 16;
                        name_off = be32(e + 8);
                        name_len = be32(e + 12);
                        break;
                    }
                }
                // name_len is bounded (real name tables are a few KB); reject a
                // corrupt/huge value rather than allocate on attacker input.
                if (name_pos && name_len > 0 && name_len <= 131072) {
                    uint8_t *buf = kmalloc(dir_end + name_len);
                    if (buf) {
                        memcpy(buf, hdr, dir_end);
                        int rn = font_read_range(path, name_off, name_len, buf + dir_end);
                        if (rn == (int)name_len) {
                            // Relocate the name table to sit right after the
                            // directory so stbtt's find_table resolves it.
                            wbe32(buf + name_pos + 8, dir_end);
                            stbtt_fontinfo mini;
                            memset(&mini, 0, sizeof(mini));
                            mini.data = buf;
                            mini.fontstart = 0;
                            extract_names(&mini, f->name, TTF_NAME_MAX,
                                          f->style, TTF_STYLE_MAX, fallback_name);
                            cheap_ok = (f->name[0] != 0);
                        }
                        kfree(buf);
                    }
                }
            }
        }
        kfree(hdr);
    }

    if (!cheap_ok) {
        // Fallback: full eager load (correct, just not lazy for this one face).
        if (face_full_load(f, fallback_name, 1) != 0) {
            memset(f, 0, sizeof(*f));
            return -1;
        }
        f->in_use = 1;
        int idx = g_nfaces++;
        kprintf("[TTF] Face %d: '%s' %s (%s, eager fallback)\n",
                idx, f->name, f->style, path);
        return idx;
    }

    f->data = NULL;                 // metadata-only: outlines loaded on demand
    f->in_use = 1;
    int idx = g_nfaces++;
    kprintf("[TTF] Face %d: '%s' %s (%s, lazy)\n", idx, f->name, f->style, path);
    return idx;
}

// Case-insensitive ".ttf"/".otf" suffix test.
static int is_font_name(const char *n) {
    int len = 0; while (n[len]) len++;
    if (len < 4) return 0;
    const char *e = n + len - 4;
    char a = e[1], b = e[2], c = e[3];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    return e[0] == '.' && ((a=='t'&&b=='t'&&c=='f') || (a=='o'&&b=='t'&&c=='f'));
}

// Is `path` already registered? Keeps a rescan from double-loading a face.
static int face_loaded_from(const char *path) {
    for (int i = 0; i < g_nfaces; i++)
        if (g_faces[i].in_use && strcmp(g_faces[i].path, path) == 0) return 1;
    return 0;
}

// Walk /FONTS and register every .ttf/.otf not already loaded. Returns how many
// NEW faces were added. Shared by ttf_init() (boot) and ttf_rescan() (install).
static int scan_fonts_dir(void) {
    extern fat_fs_t g_fat_fs;
    fat_file_t dir;
    int added = 0;
    if (fat_open(&g_fat_fs, TTF_FONTS_DIR, &dir) != 0) return 0;
    if (!fat_is_dir(&dir)) { fat_close(&dir); return 0; }

    fat_dir_entry_t ent;
    char nm[256];
    while (g_nfaces < TTF_MAX_FACES && fat_readdir(&dir, &ent, nm) == 0) {
        if (ent.attr & 0x10) continue;         // subdirectory
        if (nm[0] == '.') continue;            // . / ..
        if (!is_font_name(nm)) continue;
        char full[TTF_PATH_MAX];
        int p = 0;
        const char *d = TTF_FONTS_DIR;
        while (d[p]) { full[p] = d[p]; p++; }
        full[p++] = '/';
        for (int i = 0; nm[i] && p < (int)sizeof(full) - 1; i++) full[p++] = nm[i];
        full[p] = 0;
        if (face_loaded_from(full)) continue;
        if (enroll_face_meta(full, nm) >= 0) added++;
    }
    fat_close(&dir);
    return added;
}

// Re-walk /FONTS so a font installed at runtime is usable WITHOUT a reboot.
//
// DELIBERATELY ADDITIVE: it only ever appends faces, never frees or reorders
// them. Two invariants depend on that. (1) ttf_get_glyph_f() hands back a
// pointer INTO a face's glyph cache, and the compositor may still be holding one
// while this runs, so freeing a face here would dangle it. (2) A face index is a
// persistent handle: apps and /CONFIG/UIFONT.CFG store one, so re-packing the
// array would silently repoint every saved selection at a different typeface.
int ttf_rescan(void) {
    if (!ttf_ready) return 0;
    int added = scan_fonts_dir();
    if (added) kprintf("[TTF] Rescan: +%d face(s), %d total\n", added, g_nfaces);
    return added;
}

// Uninstall: hide a face from enumeration. The slot and its bytes are kept for
// the same aliasing reason rescan is additive (a cached glyph pointer may be
// live), so the memory is only reclaimed at the next boot. Face 0 is the default
// UI font and cannot be removed: nothing would render without it.
int ttf_face_remove(int idx) {
    if (idx <= 0 || idx >= g_nfaces || !g_faces[idx].in_use) return -1;
    g_faces[idx].in_use = 0;
    if (g_active == idx) g_active = 0;
    kprintf("[TTF] Face %d uninstalled\n", idx);
    return 0;
}

int ttf_init(void) {
    if (ttf_ready) return 0;

    g_nfaces = 0;
    g_active = 0;

    // Face 0: the default UI font. FULLY loaded (eager) because it is used for
    // essentially all desktop/app text within milliseconds of boot; lazy-loading
    // it would just move a guaranteed cost to the first character drawn.
    uint64_t t0 = mono_ms();
    font_face_t *f0 = &g_faces[0];
    memset(f0, 0, sizeof(*f0));
    strncpy(f0->path, TTF_FONT_PATH, TTF_PATH_MAX - 1);
    f0->path[TTF_PATH_MAX - 1] = 0;
    if (face_full_load(f0, "Default", 1) != 0) {
        kprintf("[TTF] Default font %s not found on disk\n", TTF_FONT_PATH);
        memset(f0, 0, sizeof(*f0));
        return -1;
    }
    f0->in_use = 1;
    g_nfaces = 1;
    uint64_t t1 = mono_ms();

    scan_fonts_dir();
    uint64_t t2 = mono_ms();

    ttf_ready = 1;
    // #536 breadcrumb: face0 = eager full load of the UI font; enroll = cheap
    // name-table-only enrollment of the rest (outlines load lazily on 1st use).
    bootlog_write("[TTF-TIMING] init face0=%llu ms enroll=%llu ms total=%llu ms faces=%d",
                  (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
                  (unsigned long long)(t2 - t0), g_nfaces);
    kprintf("[TTF] Ready: %d face(s) registered (lazy)\n", g_nfaces);
    return 0;
}

int ttf_is_ready(void) { return ttf_ready; }

int ttf_face_count(void) { return g_nfaces; }

// Face registered from `path`, or -1. Lets an installer name the face it just
// registered even when the font was ALREADY installed (rescan is additive and
// skips a known path, so the face count alone cannot identify it).
int ttf_face_by_path(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < g_nfaces; i++)
        if (g_faces[i].in_use && strcmp(g_faces[i].path, path) == 0) return i;
    return -1;
}

// Copy face `idx`'s family name into buf. Returns 0 for an empty/removed slot:
// callers enumerate 0..ttf_face_count()-1 and SKIP zero-length entries. Do not
// route this through face_at(), which substitutes face 0 for a dead index and
// would make an uninstalled font reappear under the default font's name.
int ttf_face_name(int idx, char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (idx < 0 || idx >= g_nfaces || !g_faces[idx].in_use) return 0;
    font_face_t *f = &g_faces[idx];
    int i = 0;
    while (f->name[i] && i < cap - 1) { buf[i] = f->name[i]; i++; }
    buf[i] = 0;
    return i;
}

// Copy face `idx`'s subfamily ("Regular", "Bold", "Semibold Italic", ...).
int ttf_face_style(int idx, char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (idx < 0 || idx >= g_nfaces || !g_faces[idx].in_use) return 0;
    font_face_t *f = &g_faces[idx];
    int i = 0;
    while (f->style[i] && i < cap - 1) { buf[i] = f->style[i]; i++; }
    buf[i] = 0;
    return i;
}

int ttf_get_active_face(void) { return g_active; }

int ttf_set_active_face(int idx) {
    int prev = g_active;
    if (idx >= 0 && idx < g_nfaces && g_faces[idx].in_use) g_active = idx;
    return prev;
}

// ============================================================================
// Glyph rendering
// ============================================================================

// Apply faux bold by shifting bitmap right by 1px and blending
static void apply_bold(uint8_t *bitmap, int stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = w; x > 0; x--) {
            int idx = y * stride + x;
            int src = y * stride + x - 1;
            int val = bitmap[idx] + bitmap[src];
            bitmap[idx] = (uint8_t)((val > 255) ? 255 : val);
        }
    }
}

// Faux italic: shear each row horizontally by an amount proportional to its
// height above the baseline area. Done on the already-widened glyph buffer.
// (skew ~ 0.2). We render into a widened buffer allocated by the caller.

/* #302 ROOT CAUSE (v1.69.0): the digit '7' was NOT a rasterizer bug. See the
   long note preserved below the historical location; '7' uses the real font
   outline and the xmm/glyph-cache race is contained by the cli/RFLAGS section. */

ttf_glyph_t *ttf_get_glyph_f(int face_idx, int codepoint, int size, int style) {
    if (!ttf_ready) return NULL;
    font_face_t *f = face_at(face_idx);
    // #536: bring the face's outlines resident on first use. MUST be before the
    // cli block below, as it does block-layer I/O (needs interrupts enabled).
    if (!ensure_face_loaded(f)) return NULL;
    size_cache_t *cache = get_size_cache(f, size);

    /* CRITICAL (#302): the glyph cache is SHARED global state and stb_truetype
       uses SSE float math (context_switch.asm does NOT save/restore xmm). Hold
       the whole operation in one interrupt-disabled critical section and
       SAVE/RESTORE RFLAGS so nested calls do not prematurely re-enable IRQs. */
    uint64_t saved_flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
    #define TTF_GLYPH_RETURN(v) do { \
        __asm__ volatile("push %0; popfq" :: "r"(saved_flags) : "memory", "cc"); \
        return (v); } while (0)

    // Check cache
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].codepoint == codepoint &&
            cache->entries[i].style == style) {
            TTF_GLYPH_RETURN(&cache->entries[i].glyph);
        }
    }

    // Cache full: evict all
    if (cache->count >= MAX_CACHED_GLYPHS) {
        for (int i = 0; i < MAX_CACHED_GLYPHS; i++) {
            if (cache->entries[i].glyph.bitmap) {
                kfree(cache->entries[i].glyph.bitmap);
                cache->entries[i].glyph.bitmap = NULL;
            }
        }
        cache->count = 0;
    }

    glyph_cache_entry_t *entry = &cache->entries[cache->count];

    // Render glyph (interrupts already disabled -> xmm safe across the rasterizer)
    int w, h, xoff, yoff;
    uint8_t *bmp = stbtt_GetCodepointBitmap(
        &f->info, cache->scale, cache->scale,
        codepoint, &w, &h, &xoff, &yoff);

    if (!bmp) {
        // Empty glyph (e.g. space): use the font's REAL advance
        int adv0 = 0, lsb0 = 0;
        stbtt_GetCodepointHMetrics(&f->info, codepoint, &adv0, &lsb0);
        int aw = (int)(adv0 * cache->scale);
        if (aw <= 0) aw = size / 4;
        entry->codepoint = codepoint;
        entry->style = style;
        entry->glyph.bitmap = NULL;
        entry->glyph.width = 0;
        entry->glyph.height = 0;
        entry->glyph.xoff = 0;
        entry->glyph.yoff = 0;
        entry->glyph.advance = aw;
        cache->count++;
        TTF_GLYPH_RETURN(&entry->glyph);
    }

    int advance, lsb;
    stbtt_GetCodepointHMetrics(&f->info, codepoint, &advance, &lsb);

    int extra_w = (style & TTF_STYLE_BOLD) ? 1 : 0;
    // Italic shear widens the glyph by up to skew*height columns.
    int italic = (style & TTF_STYLE_ITALIC) ? 1 : 0;
    int shear_max = italic ? ((h * 2) / 10 + 1) : 0;   // ~0.2 skew
    int alloc_w = w + extra_w + shear_max;
    if (alloc_w <= 0) alloc_w = 1;
    if (h <= 0) h = 1;

    uint8_t *gbmp = kmalloc(alloc_w * h);
    if (!gbmp) {
        stbtt_FreeBitmap(bmp, NULL);
        TTF_GLYPH_RETURN(NULL);
    }
    memset(gbmp, 0, alloc_w * h);

    // Copy bitmap into the (possibly wider) buffer, applying italic shear per row.
    for (int row = 0; row < h; row++) {
        int shift = 0;
        if (italic) {
            // top rows shift right most; baseline area least
            shift = ((h - 1 - row) * shear_max) / (h > 1 ? (h - 1) : 1);
        }
        memcpy(gbmp + row * alloc_w + shift, bmp + row * w, w);
    }
    stbtt_FreeBitmap(bmp, NULL);

    int g_w = alloc_w;
    int g_h = h;
    int g_yoff = yoff;
    int g_advance = (int)(advance * cache->scale);

    if (style & TTF_STYLE_BOLD) {
        apply_bold(gbmp, alloc_w, w + shear_max, h);
        g_advance += 1;
    }

    // Lowercase l: collapse to a crisp 1px stem (only for the default UI face,
    // and only when not styled - keep other faces/styled text faithful).
    if (face_idx == 0 && style == TTF_STYLE_NORMAL &&
        codepoint == (int)"l"[0] && alloc_w > 0 && h > 0) {
        int col = alloc_w / 2;
        for (int row = 0; row < h; row++) {
            uint8_t *r = gbmp + row * alloc_w;
            uint8_t mx = 0;
            for (int c = 0; c < alloc_w; c++) { if (r[c] > mx) mx = r[c]; r[c] = 0; }
            r[col] = mx;
        }
    }

    entry->codepoint = codepoint;
    entry->style = style;
    entry->glyph.bitmap = gbmp;
    entry->glyph.width = g_w;
    entry->glyph.height = g_h;
    entry->glyph.xoff = xoff;
    entry->glyph.yoff = g_yoff;
    entry->glyph.advance = g_advance;

    cache->count++;
    TTF_GLYPH_RETURN(&entry->glyph);
    #undef TTF_GLYPH_RETURN
}

ttf_glyph_t *ttf_get_glyph(int codepoint, int size, int style) {
    return ttf_get_glyph_f(g_active, codepoint, size, style);
}

/* #302 regression guard (unchanged): render "0123456789" at several sizes and
   sanity-check '7' keeps the real outline shape. Operates on face 0. */
void ttf_selfcheck_digits(void) {
    if (!ttf_ready) { kprintf("[TTF] selfcheck: font not ready\n"); return; }
    static const int sizes[] = { 12, 14, 16, 24, 32 };
    int fail = 0;
    for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
        int sz = sizes[si];
        for (const char *p = "0123456789"; *p; p++) {
            int d = *p;
            ttf_glyph_t *g = ttf_get_glyph_f(0, d, sz, TTF_STYLE_NORMAL);
            if (!g || !g->bitmap || g->width <= 0 || g->height <= 0) {
                kprintf("[TTF] selfcheck FAIL: digit '%c' sz=%d missing bitmap\n", d, sz);
                fail++;
                continue;
            }
            if (d == '7') {
                int W = g->width, H = g->height;
                int bar_ok = 0;
                for (int y = 0; y < H && y < 4 && !bar_ok; y++) {
                    int set = 0;
                    for (int x = 0; x < W; x++)
                        if (g->bitmap[y * W + x] >= 120) set++;
                    if (W >= 3 && set * 100 / W >= 60) bar_ok = 1;
                }
                int ink = 0;
                for (int i = 0; i < W * H; i++)
                    if (g->bitmap[i] >= 128) ink++;
                int area = W * H;
                int solid = (area > 0 && ink * 100 / area > 50);
                if (!bar_ok || solid) {
                    kprintf("[TTF] selfcheck FAIL: '7' sz=%d bad shape "
                            "(bar=%d solid=%d cover=%d%%)\n",
                            sz, bar_ok, solid, area ? ink * 100 / area : 0);
                    fail++;
                }
            }
        }
    }
    if (fail == 0)
        kprintf("[TTF] selfcheck PASS: digits 0-9 render correctly at all sizes\n");
    else
        kprintf("[TTF] selfcheck: %d failure(s) detected\n", fail);
}

void ttf_get_metrics_f(int face_idx, int size, int *ascent, int *descent, int *line_gap) {
    if (!ttf_ready) { *ascent = size; *descent = 0; *line_gap = 0; return; }
    font_face_t *f = face_at(face_idx);
    if (!ensure_face_loaded(f)) { *ascent = size; *descent = 0; *line_gap = 0; return; }
    size_cache_t *cache = get_size_cache(f, size);
    int a, d, lg;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &lg);
    *ascent = (int)(a * cache->scale);
    *descent = (int)(d * cache->scale);
    *line_gap = (int)(lg * cache->scale);
}
void ttf_get_metrics(int size, int *ascent, int *descent, int *line_gap) {
    ttf_get_metrics_f(g_active, size, ascent, descent, line_gap);
}

int ttf_get_advance_f(int face_idx, int codepoint, int size) {
    if (!ttf_ready) return size / 2;
    font_face_t *f = face_at(face_idx);
    if (!ensure_face_loaded(f)) return size / 2;
    size_cache_t *cache = get_size_cache(f, size);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&f->info, codepoint, &advance, &lsb);
    return (int)(advance * cache->scale);
}
int ttf_get_advance(int codepoint, int size) {
    return ttf_get_advance_f(g_active, codepoint, size);
}

int ttf_get_kerning_f(int face_idx, int cp1, int cp2, int size) {
    if (!ttf_ready) return 0;
    font_face_t *f = face_at(face_idx);
    if (!ensure_face_loaded(f)) return 0;
    size_cache_t *cache = get_size_cache(f, size);
    int kern = stbtt_GetCodepointKernAdvance(&f->info, cp1, cp2);
    return (int)(kern * cache->scale);
}
int ttf_get_kerning(int cp1, int cp2, int size) {
    return ttf_get_kerning_f(g_active, cp1, cp2, size);
}

// ============================================================================
// Convenience framebuffer drawing (default face, used by the kernel desktop)
// ============================================================================

void ttf_draw_string(int x, int y, const char *str, int size, uint32_t color) {
    if (!ttf_ready || !str) return;

    int ascent, descent, line_gap;
    ttf_get_metrics(size, &ascent, &descent, &line_gap);

    int baseline = y + ascent;
    int cursor_x = x;

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();

    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') {
            cursor_x = x;
            baseline += ascent - descent + line_gap;
            continue;
        }
        ttf_glyph_t *glyph = ttf_get_glyph((unsigned char)str[i], size, TTF_STYLE_NORMAL);
        if (!glyph || !glyph->bitmap) {
            cursor_x += glyph ? glyph->advance : (size / 2);
            continue;
        }
        int gx = cursor_x + glyph->xoff;
        int gy = baseline + glyph->yoff;
        for (int row = 0; row < glyph->height; row++) {
            int py = gy + row;
            if (py < 0 || py >= (int)fb_h) continue;
            for (int col = 0; col < glyph->width; col++) {
                int px = gx + col;
                if (px < 0 || px >= (int)fb_w) continue;
                uint8_t alpha = glyph->bitmap[row * glyph->width + col];
                if (alpha == 0) continue;
                if (alpha >= 250) { fb_put_pixel(px, py, color); }
                else {
                    uint32_t bg = fb_get_pixel(px, py);
                    uint8_t bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;
                    uint8_t inv = 255 - alpha;
                    uint8_t pr = (cr * alpha + bg_r * inv) / 255;
                    uint8_t pg = (cg * alpha + bg_g * inv) / 255;
                    uint8_t pb = (cb * alpha + bg_b * inv) / 255;
                    fb_put_pixel(px, py, (pr << 16) | (pg << 8) | pb);
                }
            }
        }
        cursor_x += glyph->advance;
        if (str[i + 1])
            cursor_x += ttf_get_kerning((unsigned char)str[i], (unsigned char)str[i + 1], size);
    }
}

int ttf_measure_string(const char *str, int size) {
    if (!ttf_ready || !str) return 0;
    int width = 0;
    for (int i = 0; str[i]; i++) {
        width += ttf_get_advance((unsigned char)str[i], size);
        if (str[i + 1])
            width += ttf_get_kerning((unsigned char)str[i], (unsigned char)str[i + 1], size);
    }
    return width;
}

int ttf_draw_char(int x, int y, int codepoint, int size, int style, uint32_t color) {
    if (!ttf_ready) return 0;
    ttf_glyph_t *glyph = ttf_get_glyph(codepoint, size, style);
    if (!glyph) return size / 2;
    if (!glyph->bitmap) return glyph->advance;

    int ascent, descent, line_gap;
    ttf_get_metrics(size, &ascent, &descent, &line_gap);
    int baseline = y + ascent;

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    int fb_w = (int)fb_get_width();
    int fb_h = (int)fb_get_height();

    int gx = x + glyph->xoff;
    int gy = baseline + glyph->yoff;

    for (int row = 0; row < glyph->height; row++) {
        int py = gy + row;
        if (py < 0 || py >= fb_h) continue;
        for (int col = 0; col < glyph->width; col++) {
            int px = gx + col;
            if (px < 0 || px >= fb_w) continue;
            uint8_t alpha = glyph->bitmap[row * glyph->width + col];
            if (alpha == 0) continue;
            if (alpha >= 250) { fb_put_pixel(px, py, color); }
            else {
                uint32_t bg = fb_get_pixel(px, py);
                uint8_t bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;
                uint8_t inv = 255 - alpha;
                uint8_t pr = (cr * alpha + bg_r * inv) / 255;
                uint8_t pg = (cg * alpha + bg_g * inv) / 255;
                uint8_t pb = (cb * alpha + bg_b * inv) / 255;
                fb_put_pixel(px, py, (pr << 16) | (pg << 8) | pb);
            }
        }
    }
    return glyph->advance;
}
