/* characters.c - Maytera Arena sprite pass (agent 2 of the polish contract).
 *
 * Doom-style billboard sprites for enemies, item pickups, projectiles and
 * effects. Implements ch_frame / ch_state_frames / it_sprite / pj_sprite /
 * fx_sprite from polish.h.
 *
 * =========================== ASSET_CREDITS =================================
 * All sprite artwork is from the FREEDOOM project, version 0.13.0:
 *     https://github.com/freedoom/freedoom
 *     https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
 * Licence: BSD 3-Clause ("Copyright 2001-2024 Contributors to the Freedoom
 * project"), GPL-compatible. Full text + contributor list ship in the
 * Freedoom release (COPYING.txt, CREDITS.txt).
 *
 * The sprites were extracted from freedoom2.wad (Doom picture format ->
 * 24-bit BMP against the WAD's PLAYPAL palette) and staged as
 * /ARENA/SPR/<NAME>.BMP on the boot image (FAT 8.3 names). Transparency is
 * encoded as a pure-magenta colour key (255,0,255): the kernel image decoder
 * (SYS_DECODE_IMAGE) flattens alpha to opaque, so the key is the only channel
 * that survives the round trip. The loader below converts key pixels back to
 * alpha 0 (the palette was nudged during extraction so no real pixel is ever
 * exactly 255,0,255).
 *
 * Sprite sets used (Freedoom lump prefixes):
 *   POSS / SPOS / TROO  - enemy skins 0/1/2 (zombie, shotgun zombie,
 *                         serpentipede), 8 rotations for idle/run/fire/pain,
 *                         5-frame deaths
 *   MEDI STIM ARM1 ARM2 AMMO CLIP MEGA          - pickups
 *   CSAW MGUN SHOT SGN2 LAUN PLAS BFUG          - weapon pickups
 *   MISL PLSS BFS1                              - projectiles
 *   MISL(B-D) BLUD PUFF PLSE BFE1               - explosion/blood/smoke/spark
 * Weapons Freedoom has no direct art for (grenade launcher, lightning gun,
 * railgun, nailgun) reuse the closest sprite with a colour tint applied at
 * load time.
 *
 * If a sprite file is missing at runtime, every getter falls back to a
 * procedurally generated sprite (shaded humanoid silhouette / glowing pickup
 * icon / glow ball), so the game never renders nothing.
 * ============================================================================
 */
#include "polish.h"

/* ------------------------------------------------------------------ config */
#define SPR_DIR      "/ARENA/SPR/"     /* where the BMPs live on the image   */
#define SPR_MAXDIM   256               /* decode box; sprites are far smaller */
#define CACHE_SLOTS  512               /* open-addressing cache (223 files)   */
#define KEY_RGB      0x00FF00FFu       /* magenta colour key                  */
#define NO_TINT      0x00FFFFFFu

/* --------------------------------------------------------------- the cache */
/* Keyed by sprite name + tint. st: 0 empty, 1 loaded ok, 2 failed-retryable,
 * 3 failed-permanent. A transient boot-time read failure (#444-class ATA-DMA
 * contention) must NOT be cached forever, or the sprite stays broken for the
 * whole session; retry a bounded number of times so it loads once the disk
 * settles, then give up permanently for a genuinely-missing file.            */
#define SPR_MAX_TRIES 12
typedef struct {
    char     name[10];        /* "POSSA1" style, <= 8 chars + NUL            */
    unsigned tint;
    int      st;
    int      tries;
    Image    img;
} CacheEnt;

static CacheEnt g_cache[CACHE_SLOTS];

static unsigned cache_hash(const char *n, unsigned tint) {
    unsigned h = 2166136261u;
    for (const char *p = n; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ tint) * 16777619u;
    return h;
}

/* ------------------------------------------------------------- file loader */
static unsigned char *read_whole(const char *path, long *out_len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 16384, len = 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) { sys_close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            long ncap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, (size_t)ncap);
            if (!nb) { free(buf); sys_close(fd); return 0; }
            buf = nb; cap = ncap;
        }
        long n = sys_read(fd, buf + len, (unsigned long)(cap - len));
        if (n < 0) { free(buf); sys_close(fd); return 0; }
        if (n == 0) break;
        len += n;
    }
    sys_close(fd);
    *out_len = len;
    return buf;
}

static unsigned rd16le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned rd32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Native fallback parser for the exact BMPs we stage (24-bit BI_RGB,
 * bottom-up), used only if SYS_DECODE_IMAGE is unavailable or fails.         */
static unsigned *bmp_parse(const unsigned char *f, long len, int *ow, int *oh) {
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return 0;
    unsigned dataoff = rd32le(f + 10);
    int w = (int)rd32le(f + 18);
    int hraw = (int)rd32le(f + 22);
    int bpp = (int)rd16le(f + 28);
    unsigned comp = rd32le(f + 30);
    int topdown = 0, h = hraw;
    if (hraw < 0) { topdown = 1; h = -hraw; }
    if (w < 1 || h < 1 || w > 2048 || h > 2048) return 0;
    if (comp != 0 || (bpp != 24 && bpp != 32)) return 0;
    int stride = ((w * (bpp / 8)) + 3) & ~3;
    if ((long)dataoff + (long)stride * h > len) return 0;
    unsigned *px = (unsigned *)malloc((size_t)w * h * 4);
    if (!px) return 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = f + dataoff +
            (long)stride * (topdown ? y : (h - 1 - y));
        unsigned *dst = px + (long)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *s = row + x * (bpp / 8);
            dst[x] = 0xFF000000u | ((unsigned)s[2] << 16) |
                     ((unsigned)s[1] << 8) | (unsigned)s[0];
        }
    }
    *ow = w; *oh = h;
    return px;
}

/* Load SPR_DIR/name.BMP, run the magenta key back into alpha 0, apply an
 * optional multiplicative tint (0xRRGGBB, NO_TINT = identity). Returns a
 * malloc'd pixel buffer via the cache entry; NULL on failure.                */
static const Image *spr_load(const char *name, unsigned tint) {
    unsigned h = cache_hash(name, tint);
    CacheEnt *ce = 0;
    for (unsigned probe = 0; probe < CACHE_SLOTS; probe++) {
        CacheEnt *c = &g_cache[(h + probe) & (CACHE_SLOTS - 1)];
        if (c->st == 0) { ce = c; break; }     /* empty: claim for a new load */
        if (c->tint == tint && !strcmp(c->name, name)) {
            if (c->st == 1) return &c->img;    /* already loaded              */
            if (c->st == 3) return 0;          /* gave up (truly missing)     */
            ce = c; break;                     /* st==2: retry below          */
        }
    }
    if (!ce) return 0;                         /* cache full: should not happen */

    /* build path */
    char path[48];
    int n = 0;
    for (const char *p = SPR_DIR; *p; p++) path[n++] = *p;
    for (const char *p = name; *p && n < 40; p++) path[n++] = *p;
    path[n++] = '.'; path[n++] = 'B'; path[n++] = 'M'; path[n++] = 'P';
    path[n] = 0;

    /* record the attempt (retryable until SPR_MAX_TRIES is exhausted) */
    int i = 0;
    while (name[i] && i < 9) { ce->name[i] = name[i]; i++; }
    ce->name[i] = 0;
    ce->tint = tint;
    ce->st = 2;

    long flen = 0;
    unsigned char *f = read_whole(path, &flen);
    if (!f) { if (++ce->tries >= SPR_MAX_TRIES) ce->st = 3; return 0; }

    int w = 0, hh = 0;
    unsigned *px = 0;

    /* Native BMP parse FIRST. The kernel SYS_DECODE_IMAGE BMP path returns a
     * white/blank buffer for these 24-bit BMPs (it is only exercised by paint
     * for PNG/JPEG, which loads BMPs with its own native reader), so decoding
     * our sprites through it produced solid-white billboards. Our staged
     * sprites are always uncompressed BMP, so the native parser is authoritative
     * and correct; the kernel decoder is kept only as a non-BMP fallback. */
    px = bmp_parse(f, flen, &w, &hh);
    if (!px) {
        unsigned cap = (unsigned)SPR_MAXDIM * SPR_MAXDIM * 4u;
        unsigned *tmp = (unsigned *)malloc(cap);
        if (tmp) {
            int dims[2] = { 0, 0 };
            int r = decode_image(f, (unsigned)flen, SPR_MAXDIM, SPR_MAXDIM,
                                 tmp, cap, dims);
            if (r > 0 && dims[0] > 0 && dims[1] > 0) {
                w = dims[0]; hh = dims[1];
                px = (unsigned *)malloc((size_t)w * hh * 4);
                if (px) memcpy(px, tmp, (size_t)w * hh * 4);
            }
            free(tmp);
        }
    }
    free(f);
    if (!px) { if (++ce->tries >= SPR_MAX_TRIES) ce->st = 3; return 0; }

    /* key -> alpha 0, everything else opaque (+ optional tint) */
    unsigned tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    long npx = (long)w * hh;
    for (long k = 0; k < npx; k++) {
        unsigned c = px[k] & 0x00FFFFFFu;
        if (c == KEY_RGB) { px[k] = 0; continue; }
        if (tint != NO_TINT) {
            unsigned r = ((c >> 16) & 0xFF) * tr / 255u;
            unsigned g = ((c >> 8) & 0xFF) * tg / 255u;
            unsigned b = (c & 0xFF) * tb / 255u;
            c = (r << 16) | (g << 8) | b;
        }
        px[k] = 0xFF000000u | c;
    }
    ce->img.w = w;
    ce->img.h = hh;
    ce->img.px = px;
    ce->st = 1;
    return &ce->img;
}

/* ===================================================================== */
/*  Procedural fallbacks (only used when a sprite file is missing)       */
/* ===================================================================== */

/* fetch-or-build a generated sprite through the same cache; `gen` fills a
 * fresh w*h buffer. tag must be unique per generated variant.               */
typedef void (*GenFn)(unsigned *px, int w, int h, int a, int b, int c);

static const Image *proc_get(const char *tag, int w, int h,
                             GenFn gen, int a, int b, int c) {
    unsigned hsh = cache_hash(tag, 0x50524Fu /* 'PRO' */);
    CacheEnt *ce = 0;
    for (unsigned probe = 0; probe < CACHE_SLOTS; probe++) {
        CacheEnt *cc = &g_cache[(hsh + probe) & (CACHE_SLOTS - 1)];
        if (cc->st == 0) { ce = cc; break; }
        if (cc->tint == 0x50524Fu && !strcmp(cc->name, tag))
            return cc->st == 1 ? &cc->img : 0;
    }
    if (!ce) return 0;
    int i = 0;
    while (tag[i] && i < 9) { ce->name[i] = tag[i]; i++; }
    ce->name[i] = 0;
    ce->tint = 0x50524Fu;
    ce->st = 2;
    unsigned *px = (unsigned *)malloc((size_t)w * h * 4);
    if (!px) { ce->st = 0; return 0; }         /* release slot: retry later   */
    memset(px, 0, (size_t)w * h * 4);
    gen(px, w, h, a, b, c);
    ce->img.w = w; ce->img.h = h; ce->img.px = px;
    ce->st = 1;
    return &ce->img;
}

static unsigned shade(unsigned rgb, int num, int den) {
    unsigned r = ((rgb >> 16) & 0xFF) * (unsigned)num / (unsigned)den;
    unsigned g = ((rgb >> 8) & 0xFF) * (unsigned)num / (unsigned)den;
    unsigned b = (rgb & 0xFF) * (unsigned)num / (unsigned)den;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void px_rect(unsigned *px, int w, int h, int x0, int y0, int x1, int y1,
                    unsigned argb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) px[y * w + x] = argb;
}

/* shaded humanoid silhouette; team colour per skin, small pose changes per
 * state/frame so idle/run/fire/pain/die still read at a glance.             */
static void gen_humanoid(unsigned *px, int w, int h, int skin, int state, int frame) {
    static const unsigned team[3] = { 0xC8A878u, 0x9098A8u, 0xA06840u };
    unsigned body = team[skin % 3];
    unsigned dark = shade(body, 5, 10);
    unsigned lite = shade(body, 14, 10);
    int cx = w / 2;
    int lean = 0, squat = 0;
    if (state == 3) lean = 2;                       /* pain: knocked back    */
    if (state == 4) squat = frame * (h / 6);        /* die: sink into floor  */
    int top = squat;
    if (top > h - 8) top = h - 8;
    int head_r = w / 6;
    /* legs (run state swings them via frame parity) */
    int swing = (state == 1) ? ((frame & 1) ? 3 : -3) : 0;
    px_rect(px, w, h, cx - 5 + swing, h * 2 / 3, cx - 1 + swing, h, dark);
    px_rect(px, w, h, cx + 1 - swing, h * 2 / 3, cx + 5 - swing, h, dark);
    /* torso */
    px_rect(px, w, h, cx - 7 + lean, top + head_r * 2 + 2, cx + 7 + lean,
            h * 2 / 3 + 1, body);
    /* highlight strip (fake top-left light) */
    px_rect(px, w, h, cx - 7 + lean, top + head_r * 2 + 2, cx - 4 + lean,
            h * 2 / 3, lite);
    /* arms; fire state extends one arm with a muzzle blob */
    px_rect(px, w, h, cx - 10 + lean, top + head_r * 2 + 4, cx - 7 + lean,
            h / 2 + 4, dark);
    if (state == 2) {
        px_rect(px, w, h, cx + 7, top + head_r * 2 + 5, cx + 14, top + head_r * 2 + 9, dark);
        px_rect(px, w, h, cx + 14, top + head_r * 2 + 4, cx + 17, top + head_r * 2 + 10,
                0xFFFFD040u);                        /* muzzle flash          */
    } else {
        px_rect(px, w, h, cx + 7 + lean, top + head_r * 2 + 4, cx + 10 + lean,
                h / 2 + 4, dark);
    }
    /* head */
    for (int y = -head_r; y <= head_r; y++)
        for (int x = -head_r; x <= head_r; x++)
            if (x * x + y * y <= head_r * head_r) {
                int yy = top + head_r + y, xx = cx + lean + x;
                if (yy >= 0 && yy < h && xx >= 0 && xx < w)
                    px[yy * w + xx] = (x < 0) ? lite : body;
            }
    if (state == 4 && frame >= 4)                    /* final death frame:   */
        px_rect(px, w, h, cx - 10, h - 5, cx + 10, h, dark);  /* just a heap */
}

/* glowing pickup icon: radial glow + a simple symbol per item kind          */
static void gen_icon(unsigned *px, int w, int h, int kind, int unused1, int unused2) {
    (void)unused1; (void)unused2;
    static const unsigned col[5] = { 0x40FF58u, 0xFFD830u, 0xFF8C26u,
                                     0x4CBFFFu, 0xFF4CE6u };
    unsigned c = col[kind % 5];
    int cx = w / 2, cy = h / 2, rr = (w < h ? w : h) / 2 - 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= rr * rr) {
                int fall = 255 - (d2 * 220) / (rr * rr);
                px[y * w + x] = 0xFF000000u |
                    ((((c >> 16) & 0xFF) * fall / 255) << 16) |
                    ((((c >> 8) & 0xFF) * fall / 255) << 8) |
                    ((c & 0xFF) * fall / 255);
            }
        }
    unsigned wht = 0xFFFFFFFFu;
    if (kind == IT_HEALTH || kind == IT_MEGA) {      /* cross                */
        px_rect(px, w, h, cx - 1, cy - 5, cx + 2, cy + 6, wht);
        px_rect(px, w, h, cx - 5, cy - 1, cx + 6, cy + 2, wht);
    } else if (kind == IT_ARMOR) {                   /* shield block          */
        px_rect(px, w, h, cx - 4, cy - 4, cx + 5, cy + 3, wht);
        px_rect(px, w, h, cx - 2, cy + 3, cx + 3, cy + 5, wht);
    } else if (kind == IT_AMMO) {                    /* twin slugs            */
        px_rect(px, w, h, cx - 4, cy - 4, cx - 1, cy + 5, wht);
        px_rect(px, w, h, cx + 1, cy - 4, cx + 4, cy + 5, wht);
    } else {                                         /* weapon: gun bar       */
        px_rect(px, w, h, cx - 6, cy - 1, cx + 6, cy + 2, wht);
        px_rect(px, w, h, cx - 6, cy + 2, cx - 3, cy + 5, wht);
    }
}

/* glow ball / ring / splat for projectiles and fx                           */
static void gen_ball(unsigned *px, int w, int h, int rgb, int style, int frame) {
    int cx = w / 2, cy = h / 2, rr = (w < h ? w : h) / 2 - 1;
    unsigned c = (unsigned)rgb;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            int keep = 0, fall = 0;
            if (style == 0) {                        /* solid glow ball       */
                if (d2 <= rr * rr) { keep = 1; fall = 255 - (d2 * 230) / (rr * rr); }
            } else if (style == 1) {                 /* expanding ring        */
                int ring = rr * (frame + 1) / 4;
                int band = rr / 3 + 1;
                int d = 0; while (d * d < d2) d++;   /* integer sqrt          */
                if (d >= ring - band && d <= ring) { keep = 1; fall = 255 - frame * 50; }
            } else {                                 /* splat: noisy blob     */
                unsigned n = (unsigned)(x * 73856093 ^ y * 19349663);
                if (d2 <= rr * rr && ((n >> 3) & 7) > 2)
                    { keep = 1; fall = 230 - (d2 * 180) / (rr * rr); }
            }
            if (keep && fall > 0)
                px[y * w + x] = 0xFF000000u |
                    ((((c >> 16) & 0xFF) * (unsigned)fall / 255u) << 16) |
                    ((((c >> 8) & 0xFF) * (unsigned)fall / 255u) << 8) |
                    ((c & 0xFF) * (unsigned)fall / 255u);
        }
}

/* ===================================================================== */
/*  Public API (polish.h contract)                                       */
/* ===================================================================== */

/* enemy skins: Freedoom zombie / shotgun zombie / serpentipede              */
#define NSKINS 3
static const char *k_skin_pref[NSKINS] = { "POSS", "SPOS", "TROO" };
static const char *k_die_frames[NSKINS] = { "HIJKL", "HIJKL", "IJKLM" };
static const char  k_pain_frame[NSKINS] = { 'G', 'G', 'H' };

/* frames per state: idle, run, fire, pain, die                              */
static const int k_state_frames[5] = { 1, 4, 2, 1, 5 };

int ch_state_frames(int state) {
    if (state < 0 || state > 4) return 1;
    return k_state_frames[state];
}

const Image *ch_frame(int skin, int state, int angle, int frame) {
    if (skin < 0) skin = -skin;
    skin %= NSKINS;
    if (state < 0 || state > 4) state = 0;
    angle &= 7;
    int nf = k_state_frames[state];
    if (frame < 0) frame = 0;
    if (frame >= nf) frame = nf - 1;

    char letter;
    char rot;
    switch (state) {
    case 0:  letter = 'A'; rot = (char)('1' + angle); break;          /* idle */
    case 1:  letter = (char)('A' + frame); rot = (char)('1' + angle); break;
    case 2:  letter = (char)('E' + frame); rot = (char)('1' + angle); break;
    case 3:  letter = k_pain_frame[skin]; rot = (char)('1' + angle); break;
    default: letter = k_die_frames[skin][frame]; rot = '0'; break;    /* die  */
    }

    char name[10];
    const char *pre = k_skin_pref[skin];
    name[0] = pre[0]; name[1] = pre[1]; name[2] = pre[2]; name[3] = pre[3];
    name[4] = letter; name[5] = rot; name[6] = 0;

    const Image *im = spr_load(name, NO_TINT);
    if (im) return im;
    if (rot != '1' && rot != '0') {              /* missing rotation: reuse front */
        name[5] = '1';
        im = spr_load(name, NO_TINT);
        if (im) return im;
    }
    /* procedural fallback */
    char tag[10] = { 'H', (char)('0' + skin), (char)('0' + state),
                     (char)('0' + frame), 0 };
    return proc_get(tag, 32, 56, gen_humanoid, skin, state, frame);
}

const Image *it_sprite(int item_kind, int value) {
    const char *name = 0;
    unsigned tint = NO_TINT;
    switch (item_kind) {
    case IT_HEALTH: name = (value > 0 && value <= 10) ? "STIMA0" : "MEDIA0"; break;
    case IT_ARMOR:  name = (value >= 100) ? "ARM2A0" : "ARM1A0"; break;
    case IT_AMMO:   name = "AMMOA0"; break;
    case IT_MEGA:   name = "MEGAA0"; break;
    case IT_WEAPON:
        switch (value) {
        case W_GAUNTLET:   name = "CSAWA0"; break;
        case W_MACHINEGUN: name = "MGUNA0"; break;
        case W_SHOTGUN:    name = "SHOTA0"; break;
        case W_GRENADE:    name = "LAUNA0"; tint = 0x70FF70u; break;
        case W_ROCKET:     name = "LAUNA0"; break;
        case W_LIGHTNING:  name = "PLASA0"; tint = 0xA8D0FFu; break;
        case W_RAILGUN:    name = "SGN2A0"; tint = 0x90FFE0u; break;
        case W_PLASMA:     name = "PLASA0"; break;
        case W_BFG:        name = "BFUGA0"; break;
        case W_NAILGUN:    name = "MGUNA0"; tint = 0xFFC080u; break;
        default:           name = "SHOTA0"; break;
        }
        break;
    default: break;
    }
    if (name) {
        const Image *im = spr_load(name, tint);
        if (im) return im;
        if (item_kind == IT_AMMO) {                  /* alt ammo sprite      */
            im = spr_load("CLIPA0", NO_TINT);
            if (im) return im;
        }
    }
    char tag[8] = { 'I', (char)('0' + (item_kind & 7)),
                    (char)('A' + (value & 15)), 0 };
    int kind = (item_kind >= 0 && item_kind <= 4) ? item_kind : 0;
    return proc_get(tag, 24, 24, gen_icon, kind, 0, 0);
}

const Image *pj_sprite(int proj_kind, int frame) {
    if (frame < 0) frame = 0;
    const char *name = 0;
    unsigned tint = NO_TINT;
    switch (proj_kind) {
    case PROJ_ROCKET:  name = "MISLA1"; break;
    case PROJ_GRENADE: name = "MISLA1"; tint = 0x60FF60u; break;
    case PROJ_PLASMA:  name = (frame & 1) ? "PLSSB0" : "PLSSA0"; break;
    case PROJ_BFG:     name = (frame & 1) ? "BFS1B0" : "BFS1A0"; break;
    case PROJ_NAIL:    name = (frame & 1) ? "PLSSB0" : "PLSSA0";
                       tint = 0xD8D8E8u; break;
    default: break;
    }
    if (name) {
        const Image *im = spr_load(name, tint);
        if (im) return im;
    }
    static const int fallcol[6] = { 0xFFFFFF, 0xFF8C33, 0x59E64D, 0x4D99FF,
                                    0x66FF4D, 0xCCCCE6 };
    int k = (proj_kind >= 0 && proj_kind <= 5) ? proj_kind : 0;
    char tag[8] = { 'P', (char)('0' + k), 0 };
    return proc_get(tag, 16, 16, gen_ball, fallcol[k], 0, 0);
}

/* fx_id: 0 explosion, 1 blood, 2 smoke, 3 spark, 4 BFG blast (bonus).
 * Returns NULL once `frame` runs past the end of the animation so the caller
 * can retire the effect.                                                    */
const Image *fx_sprite(int fx_id, int frame) {
    static const char *fx_expl[3]  = { "MISLB0", "MISLC0", "MISLD0" };
    static const char *fx_blood[3] = { "BLUDC0", "BLUDB0", "BLUDA0" };
    static const char *fx_smoke[4] = { "PUFFA0", "PUFFB0", "PUFFC0", "PUFFD0" };
    static const char *fx_spark[5] = { "PLSEA0", "PLSEB0", "PLSEC0",
                                       "PLSED0", "PLSEE0" };
    static const char *fx_bfg[6]   = { "BFE1A0", "BFE1B0", "BFE1C0",
                                       "BFE1D0", "BFE1E0", "BFE1F0" };
    const char *const *tab = 0;
    int n = 0;
    switch (fx_id) {
    case 0: tab = fx_expl;  n = 3; break;
    case 1: tab = fx_blood; n = 3; break;
    case 2: tab = fx_smoke; n = 4; break;
    case 3: tab = fx_spark; n = 5; break;
    case 4: tab = fx_bfg;   n = 6; break;
    default: return 0;
    }
    if (frame < 0) frame = 0;
    if (frame >= n) return 0;                        /* animation over        */
    const Image *im = spr_load(tab[frame], NO_TINT);
    if (im) return im;
    /* procedural fallback per fx type */
    static const int fxcol[5] = { 0xFF9933, 0xCC1A1A, 0x999999, 0x80CCFF,
                                  0x66FF4D };
    static const int fxsty[5] = { 1, 2, 0, 0, 1 };
    char tag[8] = { 'F', (char)('0' + fx_id), (char)('0' + frame), 0 };
    return proc_get(tag, 32, 32, gen_ball, fxcol[fx_id], fxsty[fx_id], frame);
}
