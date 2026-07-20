/* Maytera Arena - first-person weapon viewmodel sprites (wv_*), agent 3.
 *
 * Implements the polish.h contract:
 *   const Image *wv_sprite(int weapon, int frame);
 *   int          wv_fire_frames(int weapon);
 *
 * Frames are real Doom-style viewmodel sprites shipped as colour-keyed 24-bit
 * BMP files on the boot disk under /ARENA/WPN/WxFy.BMP (x = weapon 0..9,
 * y = frame 0..3). They are lazily decoded through the kernel image codec
 * (SYS_DECODE_IMAGE) on first use and cached for the lifetime of the app.
 *
 * The kernel decoder returns composited OPAQUE pixels (its alpha byte is not
 * a coverage value, see apps/paint/imgio.c), so transparency is carried by a
 * magenta colour key: every decoded pixel equal to RGB ff00ff becomes fully
 * transparent (0x00000000), everything else is forced opaque (alpha 0xFF).
 * The asset build step guarantees no real sprite pixel is exactly ff00ff.
 *
 * Frame layout per weapon: frame 0 = idle held pose, frames 1..N-1 = firing
 * animation including the muzzle-flash frames (flash sprites are already
 * composited onto the gun frame at authentic Doom psprite offsets). All
 * frames of one weapon share one canvas whose bottom edge is the Doom screen
 * bottom, so the renderer can anchor the sprite bottom to the window bottom
 * and the recoil/pump motion baked into the frames lines up correctly.
 *
 * ---------------------------------------------------------------------------
 * ASSET_CREDITS
 *   Source: Freedoom v0.13.0 (freedoom2.wad), https://freedoom.github.io/
 *           https://github.com/freedoom/freedoom
 *           (release: github.com/freedoom/freedoom/releases/tag/v0.13.0)
 *   Licence: BSD 3-clause (COPYING.txt in the Freedoom distribution),
 *            GPL-compatible. Copyright (c) 2001-2024 Contributors to the
 *            Freedoom project. All rights reserved.
 *   The weapon frames were extracted from the WAD sprite lumps (gun + muzzle
 *   flash composited at their original psprite offsets) and, for four
 *   weapons, hue-tinted to give MayteraOS Arena distinct weapon identities:
 *     W0 gauntlet    = SAWG C/A/B                    (chainsaw)
 *     W1 machinegun  = CHGG A/B + CHGF A/B           (minigun)
 *     W2 shotgun     = SHTG A/B + SHTF A/B           (pump shotgun)
 *     W3 grenade     = MISG A/B + MISF A/B/C, tinted green
 *     W4 rocket      = MISG A/B + MISF A/B/C         (missile launcher)
 *     W5 lightning   = PLSG A + PLSF A/B, tinted yellow-white
 *     W6 railgun     = SHT2 A/B + SHT2 I/J, tinted cyan
 *     W7 plasma      = PLSG A/B + PLSF A/B           (polaric energy weapon)
 *     W8 bfg         = BFGG A/B/C + BFGF A/B         (SKAG 1337)
 *     W9 nailgun     = PISG A/B/C + PISF A, tinted purple
 * ---------------------------------------------------------------------------
 */
#include "polish.h"

/* frames per weapon, INCLUDING the idle frame (index 0) */
#define WV_MAX_FRAMES 4
static const int g_wv_nframes[NUM_WEAPONS] = {
    3,  /* W_GAUNTLET   idle + 2 sawing frames                    */
    3,  /* W_MACHINEGUN idle + 2 flash frames (barrels alternate) */
    4,  /* W_SHOTGUN    idle + 2 flash + pump                     */
    4,  /* W_GRENADE    idle + 3 flash/recoil                     */
    4,  /* W_ROCKET     idle + 3 flash/recoil                     */
    3,  /* W_LIGHTNING  idle + 2 flash                            */
    4,  /* W_RAILGUN    idle + 2 flash + recoil                   */
    3,  /* W_PLASMA     idle + 2 flash                            */
    4,  /* W_BFG        idle + charge + 2 flash                   */
    4,  /* W_NAILGUN    idle + flash + 2 recoil                   */
};

/* compile-time guard: the table above must cover exactly the 10 weapons */
typedef char wv_assert_ten_weapons[(NUM_WEAPONS == 10) ? 1 : -1];

#define WV_KEY_RGB   0x00FF00FFu   /* magenta transparency key            */
#define WV_MAX_DIM   512           /* sanity cap on decoded sprite sides  */

/* cache state per weapon/frame. A transient boot-time read failure (#444-class
 * ATA-DMA contention) must NOT be cached as WV_FAILED forever, or the weapon
 * viewmodel stays broken for the session; retry a bounded number of times so
 * it loads once the disk settles, then give up for a truly-missing frame.     */
#define WV_MAX_TRIES 12
enum { WV_UNTRIED = 0, WV_LOADED = 1, WV_FAILED = -1 };
typedef struct {
    Image img;
    int   state;
    int   tries;
} WvSlot;
static WvSlot g_wv[NUM_WEAPONS][WV_MAX_FRAMES];

/* ------------------------------------------------------------------ file IO */
static unsigned char *wv_read_whole(const char *path, long *out_len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 65536, len = 0;
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
    if (len <= 0) { free(buf); return 0; }
    *out_len = len;
    return buf;
}

static unsigned int wv_rd32le(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int wv_rd16le(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Native parser for the exact BMPs we stage (24/32-bit BI_RGB, bottom-up or
 * top-down). Used FIRST: the kernel SYS_DECODE_IMAGE BMP path returns a
 * white/blank buffer for these files (it is only exercised by paint for
 * PNG/JPEG; paint loads BMPs with its own native reader), so decoding our BMPs
 * through it yields solid-white viewmodels. Returns a malloc'd 0x00RRGGBB
 * buffer (the caller's colour-key pass rebuilds alpha), NULL on any mismatch. */
static unsigned int *wv_bmp_parse(const unsigned char *f, long len, int *ow, int *oh) {
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return 0;
    unsigned int dataoff = wv_rd32le(f + 10);
    int w = (int)wv_rd32le(f + 18);
    int hraw = (int)wv_rd32le(f + 22);
    int bpp = (int)wv_rd16le(f + 28);
    unsigned int comp = wv_rd32le(f + 30);
    int topdown = 0, h = hraw;
    if (hraw < 0) { topdown = 1; h = -hraw; }
    if (w < 1 || h < 1 || w > WV_MAX_DIM || h > WV_MAX_DIM) return 0;
    if (comp != 0 || (bpp != 24 && bpp != 32)) return 0;
    int bypp = bpp / 8;
    long stride = ((long)w * bypp + 3) & ~3L;
    if ((long)dataoff + stride * h > len) return 0;
    unsigned int *px = (unsigned int *)malloc((size_t)w * h * 4);
    if (!px) return 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = f + dataoff + stride * (topdown ? y : (h - 1 - y));
        unsigned int *dst = px + (long)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *s = row + (long)x * bypp;
            dst[x] = ((unsigned int)s[2] << 16) | ((unsigned int)s[1] << 8) | s[0];
        }
    }
    *ow = w; *oh = h;
    return px;
}

/* ------------------------------------------------------------------ loader */
static int wv_load_slot(int weapon, int frame) {
    /* "/ARENA/WPN/WxFy.BMP" with x/y patched in (both are single digits) */
    char path[24];
    static const char tmpl[] = "/ARENA/WPN/W0F0.BMP";
    memcpy(path, tmpl, sizeof(tmpl));
    path[12] = (char)('0' + weapon);
    path[14] = (char)('0' + frame);

    long flen = 0;
    unsigned char *fbuf = wv_read_whole(path, &flen);
    if (!fbuf) return -1;

    /* Parse the BMP header for the true pixel size so the kernel decoder is
     * asked for a 1:1 box (it point-samples DOWN to fit the target box; with
     * the exact size it never resamples). */
    if (flen < 54 || fbuf[0] != 'B' || fbuf[1] != 'M') { free(fbuf); return -1; }
    int bw = (int)wv_rd32le(fbuf + 18);
    int bh = (int)wv_rd32le(fbuf + 22);
    if (bh < 0) bh = -bh;                       /* top-down BMPs store -height */
    if (bw < 1 || bh < 1 || bw > WV_MAX_DIM || bh > WV_MAX_DIM) {
        free(fbuf);
        return -1;
    }

    /* Native BMP parse FIRST (kernel decode returns white for these BMPs). */
    int dims[2] = { 0, 0 };
    unsigned int *out = wv_bmp_parse(fbuf, flen, &dims[0], &dims[1]);
    if (!out) {
        /* fallback for any non-BMP asset: the kernel image codec */
        unsigned long cap = (unsigned long)bw * (unsigned long)bh * 4u;
        out = (unsigned int *)malloc((size_t)cap);
        if (!out) { free(fbuf); return -1; }
        int n = decode_image(fbuf, (unsigned int)flen, bw, bh, out,
                             (unsigned int)cap, dims);
        if (n <= 0 || dims[0] < 1 || dims[1] < 1 ||
            dims[0] > bw || dims[1] > bh) {
            free(out); free(fbuf);
            return -1;
        }
    }
    free(fbuf);

    /* Colour-key pass: rebuild alpha from the magenta key. */
    long npx = (long)dims[0] * (long)dims[1];
    for (long i = 0; i < npx; i++) {
        unsigned int rgb = out[i] & 0x00FFFFFFu;
        out[i] = (rgb == WV_KEY_RGB) ? 0x00000000u : (0xFF000000u | rgb);
    }

    WvSlot *slot = &g_wv[weapon][frame];
    slot->img.w  = dims[0];
    slot->img.h  = dims[1];
    slot->img.px = out;
    slot->state  = WV_LOADED;
    return 0;
}

/* ------------------------------------------------------------- public API */
int wv_fire_frames(int weapon) {
    if (weapon < 0 || weapon >= NUM_WEAPONS) return 1;
    return g_wv_nframes[weapon];
}

const Image *wv_sprite(int weapon, int frame) {
    if (weapon < 0 || weapon >= NUM_WEAPONS) return 0;
    int nf = g_wv_nframes[weapon];
    if (frame < 0) frame = 0;
    if (frame >= nf) frame = nf - 1;            /* clamp overshoot to last   */

    WvSlot *slot = &g_wv[weapon][frame];
    if (slot->state == WV_UNTRIED) {            /* UNTRIED also means "retry"  */
        if (wv_load_slot(weapon, frame) == 0)
            slot->state = WV_LOADED;
        else if (++slot->tries >= WV_MAX_TRIES)
            slot->state = WV_FAILED;            /* give up after bounded retry */
        /* else stay WV_UNTRIED so the next frame retries the load */
    }
    if (slot->state == WV_LOADED) return &slot->img;

    /* a missing fire frame degrades to the idle pose instead of vanishing */
    if (frame != 0) return wv_sprite(weapon, 0);
    return 0;
}
