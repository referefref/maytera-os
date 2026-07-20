/* Maytera Arena - skyboxes + environmental decals + per-theme colour grading.
 * (agent 4 of the graphics polish split, see polish.h)
 *
 * ============================== ASSET_CREDITS ==============================
 * Sky panoramas are STAGED FILES (not baked): 1024x512 equirectangular 24-bit
 * BMPs shipped on the boot disk under /ARENA/SKY/ and lazily decoded through
 * the kernel image codec (SYS_DECODE_IMAGE). Staging copies for the boot
 * image live in the repo at userland/apps/arena/assets/sky/.
 *
 *   SKY0.BMP  theme 0 "space-metal", blue starfield nebula.
 *             Source: "Space Skyboxes (0)" by Rawdanitsu, OpenGameArt,
 *             https://opengameart.org/content/space-skyboxes-0
 *             Licence: CC0 1.0 (public domain).
 *             The 6 cube faces of the "blue" set were reprojected to a single
 *             equirectangular panorama at build time.
 *   SKY1.BMP  theme 1 "industrial", smoggy amber overcast.
 *             Source: EveningSkyHDRI032A, ambientCG.com,
 *             https://ambientcg.com/view?id=EveningSkyHDRI032A
 *             Licence: CC0 1.0. Tone-mapped 1K JPG, graded toward rust-brown
 *             smog, lower half faded into the theme fog colour.
 *   SKY2.BMP  theme 2 "stone keep", blue sky with heavy cumulus.
 *             Source: DaySkyHDRI007B, ambientCG.com,
 *             https://ambientcg.com/view?id=DaySkyHDRI007B
 *             Licence: CC0 1.0. Tone-mapped 1K JPG, horizon fog fade.
 *   SKY3.BMP  theme 3 "desert colosseum", hazy warm sunrise.
 *             Source: MorningSkyHDRI007A, ambientCG.com,
 *             https://ambientcg.com/view?id=MorningSkyHDRI007A
 *             Licence: CC0 1.0. Tone-mapped 1K JPG, warm haze boost, fog fade.
 *   SKY4.BMP  theme 4 "violet tower", violet aurora over a star void.
 *             Source: NightSkyHDRI007, ambientCG.com,
 *             https://ambientcg.com/view?id=NightSkyHDRI007
 *             Licence: CC0 1.0. Tone-mapped 1K JPG, violet grade, fog fade.
 *
 * All decal/smoke/spark sprites below are procedurally generated in this file
 * at runtime (no external assets). If a sky file is missing or the decoder
 * fails, sky_face() falls back to a procedural sky of the same mood, so the
 * game never renders without a sky.
 * ===========================================================================
 */

#include "game.h"       /* pulls maytera.h: syscalls, malloc/free, memset    */
#include "polish.h"
#include "world_art.h"

/* ------------------------------------------------------------------ tuning */
#define SKY_W       1024
#define SKY_H        512
#define DECAL_SZ      64

#define SMOKE_FRAMES   6
#define SPARK_FRAMES   4
#define BLOOD_FRAMES   4
#define SCORCH_FRAMES  2

/* --------------------------------------------------------- colour grading */
/* Fog colours match the horizon band baked into the staged panoramas. */
static const unsigned int g_fog_rgb[NUM_LEVELS] = {
    0x141B33,   /* 0 space-metal : deep blue nebula haze     */
    0x4A3A28,   /* 1 industrial  : smoggy brown-gray          */
    0x9FA8B4,   /* 2 stone keep  : pale blue-gray             */
    0xCBA98B,   /* 3 desert      : warm sand haze             */
    0x2A1440,   /* 4 violet tower: dark violet                */
};
static const unsigned int g_ambient_rgb[NUM_LEVELS] = {
    0xAAB8E6,   /* cool starlight blue     */
    0xE0C49C,   /* rusty sodium warmth     */
    0xF2F0E8,   /* neutral daylight        */
    0xFFE2B8,   /* warm sand bounce        */
    0xC0A0E0,   /* violet glow             */
};

static int clamp_theme(int theme) {
    if (theme < 0) return 0;
    if (theme >= NUM_LEVELS) return NUM_LEVELS - 1;
    return theme;
}

unsigned int world_fog_rgb(int theme)     { return g_fog_rgb[clamp_theme(theme)]; }
unsigned int world_ambient_rgb(int theme) { return g_ambient_rgb[clamp_theme(theme)]; }

/* ------------------------------------------------------------- tiny PRNG */
static unsigned int g_rng;
static unsigned int wa_rand(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static float wa_frand(void) { return (float)(wa_rand() & 0xFFFF) * (1.0f / 65535.0f); }

/* integer coordinate hash to [0,1), stable per (x,y,seed) */
static float wa_hash(int x, int y, unsigned int seed) {
    unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) * (1.0f / 65535.0f);
}

/* smooth value noise (bilinear-interpolated lattice hash), period `cell` px */
static float wa_noise(float x, float y, float cell, unsigned int seed) {
    float fx = x / cell, fy = y / cell;
    int ix = (int)fx, iy = (int)fy;
    float tx = fx - (float)ix, ty = fy - (float)iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = wa_hash(ix,     iy,     seed);
    float b = wa_hash(ix + 1, iy,     seed);
    float c = wa_hash(ix,     iy + 1, seed);
    float d = wa_hash(ix + 1, iy + 1, seed);
    return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

static float wa_sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static unsigned int pack_argb(float a, float r, float g, float b) {
    a = wa_sat(a); r = wa_sat(r); g = wa_sat(g); b = wa_sat(b);
    return ((unsigned int)(a * 255.0f) << 24) | ((unsigned int)(r * 255.0f) << 16)
         | ((unsigned int)(g * 255.0f) << 8)  |  (unsigned int)(b * 255.0f);
}

/* ------------------------------------------------------------ file loading */
/* Read a whole file into a malloc'd buffer (caller frees). NULL on failure. */
static unsigned char *wa_read_whole(const char *path, long *out_len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 256 * 1024, len = 0;
    unsigned char *buf = (unsigned char *)malloc((unsigned long)cap);
    if (!buf) { sys_close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            long ncap = cap * 2;
            unsigned char *nb = (unsigned char *)malloc((unsigned long)ncap);
            if (!nb) { free(buf); sys_close(fd); return 0; }
            memcpy(nb, buf, (unsigned long)len);
            free(buf); buf = nb; cap = ncap;
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

static unsigned int wa_rd16le(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int wa_rd32le(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Native BMP -> dst (exactly w*h ARGB, alpha forced opaque). Used FIRST: the
 * kernel SYS_DECODE_IMAGE BMP path returns a white/blank buffer for these BMPs
 * (paint only ever routes PNG/JPEG through it), which showed as a blown-out
 * white sky. The staged panoramas are always uncompressed 24-bit BMP, so the
 * native parser is authoritative. Returns 1 on success. */
static int wa_bmp_to(const unsigned char *f, long len, unsigned int *dst, int w, int h) {
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return 0;
    unsigned int dataoff = wa_rd32le(f + 10);
    int bw = (int)wa_rd32le(f + 18);
    int hraw = (int)wa_rd32le(f + 22);
    int bpp = (int)wa_rd16le(f + 28);
    unsigned int comp = wa_rd32le(f + 30);
    int topdown = 0, bh = hraw;
    if (hraw < 0) { topdown = 1; bh = -hraw; }
    if (bw != w || bh != h) return 0;
    if (comp != 0 || (bpp != 24 && bpp != 32)) return 0;
    int bypp = bpp / 8;
    long stride = ((long)w * bypp + 3) & ~3L;
    if ((long)dataoff + stride * h > len) return 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = f + dataoff + stride * (topdown ? y : (h - 1 - y));
        unsigned int *drow = dst + (long)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *s = row + (long)x * bypp;
            drow[x] = 0xFF000000u | ((unsigned int)s[2] << 16) |
                      ((unsigned int)s[1] << 8) | s[0];
        }
    }
    return 1;
}

/* ------------------------------------------------------ procedural fallback */
/* If the staged BMP is missing or the kernel decoder fails, synthesise a sky
 * of the same mood so the arena never loses its sky. px is SKY_W*SKY_H. */
static void proc_sky(int theme, unsigned int *px) {
    unsigned int fog = g_fog_rgb[theme];
    float fr = (float)((fog >> 16) & 0xFF) / 255.0f;
    float fg = (float)((fog >> 8) & 0xFF) / 255.0f;
    float fb = (float)(fog & 0xFF) / 255.0f;

    for (int y = 0; y < SKY_H; y++) {
        float v = (float)y / (float)(SKY_H - 1);      /* 0 top .. 1 bottom   */
        for (int x = 0; x < SKY_W; x++) {
            float r, g, b;
            switch (theme) {
            case 0: {   /* deep space: near-black blue + nebula + stars      */
                float neb = wa_noise((float)x, (float)y, 96.0f, 11u) * 0.6f
                          + wa_noise((float)x, (float)y, 34.0f, 12u) * 0.4f;
                neb = neb * neb;
                r = 0.02f + neb * 0.10f;
                g = 0.03f + neb * 0.16f;
                b = 0.07f + neb * 0.45f;
                float s = wa_hash(x, y, 77u);
                if (s > 0.9975f) { float m = (s - 0.9975f) * 400.0f; r += m; g += m; b += m; }
                break; }
            case 1: {   /* industrial smog: amber overcast bands             */
                float bank = wa_noise((float)x, (float)y, 120.0f, 21u) * 0.7f
                           + wa_noise((float)x, (float)y, 40.0f, 22u) * 0.3f;
                float base = 0.30f + 0.25f * bank - 0.15f * (1.0f - v);
                r = base * 1.25f + 0.10f;
                g = base * 0.95f + 0.05f;
                b = base * 0.60f;
                break; }
            case 2: {   /* keep: blue sky, white clouds                      */
                float cl = wa_noise((float)x, (float)y, 90.0f, 31u) * 0.6f
                         + wa_noise((float)x, (float)y, 30.0f, 32u) * 0.4f;
                cl = cl > 0.52f ? (cl - 0.52f) * 2.2f : 0.0f;
                if (cl > 1) cl = 1;
                r = 0.30f + 0.15f * v + cl * 0.55f;
                g = 0.48f + 0.14f * v + cl * 0.42f;
                b = 0.80f + 0.05f * v + cl * 0.18f;
                break; }
            case 3: {   /* desert: warm haze + low sun glow                  */
                float cx = (float)x - (float)SKY_W * 0.5f;
                float cy = (float)y - (float)SKY_H * 0.52f;
                float d = mx_sqrtf(cx * cx * 0.25f + cy * cy) / 90.0f;
                float sun = d < 1.0f ? (1.0f - d) : 0.0f;
                r = 0.62f + 0.20f * v + sun * 0.40f;
                g = 0.52f + 0.16f * v + sun * 0.30f;
                b = 0.42f + 0.12f * v + sun * 0.10f;
                break; }
            default: {  /* violet void: aurora curtains + stars              */
                float cur = wa_noise((float)x * 0.35f, (float)y * 1.6f, 70.0f, 41u);
                float band = cur > 0.55f ? (cur - 0.55f) * 2.5f : 0.0f;
                float fade = 1.0f - v;                 /* aurora lives up top */
                r = 0.10f + band * 0.45f * fade;
                g = 0.03f + band * 0.18f * fade;
                b = 0.18f + band * 0.60f * fade;
                float s = wa_hash(x, y, 88u);
                if (s > 0.998f) { float m = (s - 0.998f) * 350.0f; r += m; g += m * 0.8f; b += m; }
                break; }
            }
            /* blend the lower part into the fog colour, like the baked BMPs */
            if (theme != 0 && v > 0.55f) {
                float t = (v - 0.55f) / 0.30f;
                if (t > 1) t = 1;
                t = t * t * (3.0f - 2.0f * t);
                r = r * (1 - t) + fr * t;
                g = g * (1 - t) + fg * t;
                b = b * (1 - t) + fb * t;
            }
            px[y * SKY_W + x] = pack_argb(1.0f, r, g, b);
        }
    }
}

/* ------------------------------------------------------------- sky loading */
static const char *const g_sky_paths[NUM_LEVELS] = {
    "/ARENA/SKY/SKY0.BMP",
    "/ARENA/SKY/SKY1.BMP",
    "/ARENA/SKY/SKY2.BMP",
    "/ARENA/SKY/SKY3.BMP",
    "/ARENA/SKY/SKY4.BMP",
    "/ARENA/SKY/SKY5.BMP",   /* outdoor Dust Storm: planets + starfield */
};

/* One decoded panorama cached at a time: the game only renders one level's
 * sky, and a 1024x512 ARGB panorama is 2 MB, so caching all five would waste
 * 8 MB for nothing. Switching levels swaps the cache (one decode, ~ms). */
static int           g_sky_theme = -1;
static Image         g_sky_img;
static unsigned int *g_sky_px;
/* Was the cached panorama the REAL decoded file, or a procedural stand-in?
 * A boot-time read failure (#444-class ATA-DMA contention) must not lock the
 * procedural sky in for the whole session: keep retrying the file (bounded)
 * until the real one decodes, then stop. The 1.5 MB sky BMP is the largest
 * asset and thus the most exposed to a transient partial read. */
#define SKY_MAX_TRIES 16
static int           g_sky_real  = 0;
static int           g_sky_tries = 0;

/* returns 1 if it decoded the REAL file, 0 if it fell back to procedural */
static int sky_load(int theme) {
    unsigned long cap = (unsigned long)SKY_W * SKY_H * 4u;
    if (!g_sky_px) {
        g_sky_px = (unsigned int *)malloc(cap);
        if (!g_sky_px) return 0;
    }

    long len = 0;
    unsigned char *f = wa_read_whole(g_sky_paths[theme], &len);
    int ok = 0;
    if (f) {
        /* Native BMP first (kernel decode returns white for these BMPs). */
        if (wa_bmp_to(f, len, g_sky_px, SKY_W, SKY_H)) {
            ok = 1;
        } else {
            int dims[2] = {0, 0};
            int n = decode_image(f, (unsigned int)len, SKY_W, SKY_H,
                                 g_sky_px, (unsigned int)cap, dims);
            if (n > 0 && dims[0] == SKY_W && dims[1] == SKY_H) {
                unsigned long npx = (unsigned long)SKY_W * SKY_H;
                for (unsigned long i = 0; i < npx; i++)
                    g_sky_px[i] = 0xFF000000u | (g_sky_px[i] & 0x00FFFFFFu);
                ok = 1;
            }
        }
        free(f);
    }
    if (!ok)
        proc_sky(theme, g_sky_px);       /* never ship a blank sky            */

    g_sky_img.w  = SKY_W;
    g_sky_img.h  = SKY_H;
    g_sky_img.px = g_sky_px;
    g_sky_theme  = theme;
    g_sky_real   = ok;
    return ok;
}

/* Contract (polish.h): single equirect panorama delivered at face 0, NULL for
 * faces 1..5, the integrator handles either form. */
const Image *sky_face(int theme, int face) {
    if (face != 0) return 0;
    theme = clamp_theme(theme);
    if (g_sky_theme != theme) {              /* switched level: reset + load  */
        g_sky_real = 0; g_sky_tries = 0;
        sky_load(theme);
    } else if (!g_sky_real && g_sky_tries < SKY_MAX_TRIES) {
        g_sky_tries++;                       /* still procedural: retry file  */
        sky_load(theme);
    }
    return g_sky_px ? &g_sky_img : 0;
}

/* ------------------------------------------------------- decal generation */
/* All decals are DECAL_SZ x DECAL_SZ, baked once on first request. */
typedef struct { Image img; unsigned int *px; } DecalSlot;
static DecalSlot g_blood [BLOOD_FRAMES];
static DecalSlot g_scorch[SCORCH_FRAMES];
static DecalSlot g_smoke [SMOKE_FRAMES];
static DecalSlot g_spark [SPARK_FRAMES];

static unsigned int *decal_alloc(DecalSlot *s) {
    if (s->px) return s->px;
    unsigned long cap = (unsigned long)DECAL_SZ * DECAL_SZ * 4u;
    s->px = (unsigned int *)malloc(cap);
    if (s->px) {
        memset(s->px, 0, cap);
        s->img.w = DECAL_SZ; s->img.h = DECAL_SZ; s->img.px = s->px;
    }
    return s->px;
}

/* blood splat: central pool + satellite droplets, dark arterial red */
static void bake_blood(int variant) {
    unsigned int *px = decal_alloc(&g_blood[variant]);
    if (!px) return;
    g_rng = 0xB100D + (unsigned int)variant * 7919u;

    float cx[10], cy[10], cr[10];
    cx[0] = 32.0f; cy[0] = 32.0f; cr[0] = 11.0f + wa_frand() * 4.0f;
    int nblob = 6 + (int)(wa_frand() * 4.0f);
    for (int i = 1; i < nblob; i++) {
        float ang = wa_frand() * 6.2831853f;
        float dst = 8.0f + wa_frand() * 18.0f;
        cx[i] = 32.0f + mx_cosf(ang) * dst;
        cy[i] = 32.0f + mx_sinf(ang) * dst;
        cr[i] = 1.5f + wa_frand() * 5.0f;
    }
    for (int y = 0; y < DECAL_SZ; y++)
        for (int x = 0; x < DECAL_SZ; x++) {
            float cov = 0.0f;
            for (int i = 0; i < nblob; i++) {
                float dx = (float)x - cx[i], dy = (float)y - cy[i];
                float d = mx_sqrtf(dx * dx + dy * dy);
                float edge = cr[i] * (0.75f + 0.5f * wa_hash(x, y, 900u + (unsigned int)i));
                if (d < edge) { float c = 1.0f - d / edge; if (c > cov) cov = c; }
            }
            if (cov <= 0.02f) continue;
            float a = cov > 0.25f ? 1.0f : cov * 4.0f;
            float shade = 0.55f + 0.45f * wa_hash(x, y, 901u);   /* clotting   */
            px[y * DECAL_SZ + x] = pack_argb(a, 0.45f * shade + 0.10f,
                                                0.02f * shade, 0.02f * shade);
        }
}

/* scorch mark: charred disc with a noisy edge; variant 1 keeps ember glow */
static void bake_scorch(int variant) {
    unsigned int *px = decal_alloc(&g_scorch[variant]);
    if (!px) return;
    for (int y = 0; y < DECAL_SZ; y++)
        for (int x = 0; x < DECAL_SZ; x++) {
            float dx = (float)x - 32.0f, dy = (float)y - 32.0f;
            float d = mx_sqrtf(dx * dx + dy * dy) / 28.0f;
            float n = wa_noise((float)x, (float)y, 9.0f, 500u + (unsigned int)variant);
            d += (n - 0.5f) * 0.45f;                    /* ragged edge        */
            if (d >= 1.0f) continue;
            float a = (1.0f - d) * 1.6f; if (a > 0.92f) a = 0.92f;
            float k = 0.05f + 0.10f * n;                /* charcoal           */
            float r = k, g = k, b = k;
            if (variant == 1 && d > 0.45f && d < 0.8f) {  /* ember ring       */
                float e = (0.8f - d) * (d - 0.45f) * 11.0f * n;
                r += e * 0.9f; g += e * 0.35f;
            }
            px[y * DECAL_SZ + x] = pack_argb(a, r, g, b);
        }
}

/* smoke puff: grows and thins over SMOKE_FRAMES */
static void bake_smoke(int frame) {
    unsigned int *px = decal_alloc(&g_smoke[frame]);
    if (!px) return;
    float t = (float)frame / (float)(SMOKE_FRAMES - 1);          /* 0..1      */
    float rad  = 9.0f + t * 19.0f;
    float amax = 0.80f - t * 0.62f;
    float lum  = 0.35f + t * 0.32f;                    /* soot brightens out  */
    for (int y = 0; y < DECAL_SZ; y++)
        for (int x = 0; x < DECAL_SZ; x++) {
            float dx = (float)x - 32.0f, dy = (float)y - 32.0f;
            float d = mx_sqrtf(dx * dx + dy * dy) / rad;
            float n = wa_noise((float)x + t * 40.0f, (float)y, 12.0f, 600u)
                    * 0.65f + 0.35f;
            d /= n;                                     /* curly silhouette   */
            if (d >= 1.0f) continue;
            float fall = 1.0f - d;
            float a = amax * fall * fall * (3.0f - 2.0f * fall);
            float l = lum * (0.8f + 0.4f * n);
            px[y * DECAL_SZ + x] = pack_argb(a, l, l, l * 1.04f);
        }
}

/* spark: 4-point star flash, white-hot core decaying to orange */
static void bake_spark(int frame) {
    unsigned int *px = decal_alloc(&g_spark[frame]);
    if (!px) return;
    float t = (float)frame / (float)(SPARK_FRAMES - 1);
    float len = 26.0f * (1.0f - t * 0.55f);
    float heat = 1.0f - t * 0.75f;
    for (int y = 0; y < DECAL_SZ; y++)
        for (int x = 0; x < DECAL_SZ; x++) {
            float dx = (float)x - 32.0f, dy = (float)y - 32.0f;
            float ax = mx_absf(dx), ay = mx_absf(dy);
            float d = mx_sqrtf(dx * dx + dy * dy);
            /* axis rays + shorter diagonals + hot core */
            float ray = 0.0f;
            float w1 = 1.6f + d * 0.08f;
            if (ax < w1 && d < len)             { float v = (1.0f - d / len);              if (v > ray) ray = v; }
            if (ay < w1 && d < len)             { float v = (1.0f - d / len);              if (v > ray) ray = v; }
            float dd = mx_absf(ax - ay) * 0.7071f;
            if (dd < w1 * 0.8f && d < len*0.6f) { float v = (1.0f - d / (len * 0.6f)) * 0.8f; if (v > ray) ray = v; }
            float core = d < 5.0f ? (1.0f - d / 5.0f) : 0.0f;
            float i = ray * ray * heat + core * core;
            if (i <= 0.03f) continue;
            if (i > 1) i = 1;
            px[y * DECAL_SZ + x] = pack_argb(i, 1.0f,
                                             0.55f + 0.45f * i * heat,
                                             0.15f + 0.75f * i * i * heat);
        }
}

int decal_frames(int kind) {
    switch (kind) {
    case WA_DECAL_BLOOD:  return BLOOD_FRAMES;
    case WA_DECAL_SCORCH: return SCORCH_FRAMES;
    case WA_DECAL_SMOKE:  return SMOKE_FRAMES;
    case WA_DECAL_SPARK:  return SPARK_FRAMES;
    default:              return 0;
    }
}

const Image *decal_sprite(int kind, int frame) {
    DecalSlot *slot; int n;
    switch (kind) {
    case WA_DECAL_BLOOD:  slot = g_blood;  n = BLOOD_FRAMES;  break;
    case WA_DECAL_SCORCH: slot = g_scorch; n = SCORCH_FRAMES; break;
    case WA_DECAL_SMOKE:  slot = g_smoke;  n = SMOKE_FRAMES;  break;
    case WA_DECAL_SPARK:  slot = g_spark;  n = SPARK_FRAMES;  break;
    default: return 0;
    }
    if (frame < 0) frame = 0;
    if (frame >= n) frame = n - 1;
    if (!slot[frame].px) {
        switch (kind) {
        case WA_DECAL_BLOOD:  bake_blood(frame);  break;
        case WA_DECAL_SCORCH: bake_scorch(frame); break;
        case WA_DECAL_SMOKE:  bake_smoke(frame);  break;
        case WA_DECAL_SPARK:  bake_spark(frame);  break;
        }
        if (!slot[frame].px) return 0;               /* allocation failed     */
    }
    return &slot[frame].img;
}
