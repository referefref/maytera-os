/* r_polish.c - Maytera Arena graphics polish: shared render helpers that turn
 * the asset packs (textures.c / characters.c / weapons_art.c / world_art.c)
 * into pixels. Implements the agent-5 half of polish.h:
 *
 *   img_bind_gl()    - upload an Image once into a TinyGL texture (cached by
 *                      pixel-pointer identity) and bind it. Transparent texels
 *                      (alpha < 128) are written as TinyGL's NO_DRAW color
 *                      (0xFF00FF magenta, see libgl zfeatures.h), which the
 *                      rasterizer skips per texel: that IS the alpha test.
 *   billboard_draw() - camera-facing textured quad in the 3D scene. Uses the
 *                      camera basis exposed by render.c (r_camera_basis), no
 *                      duplicated camera math.
 *   screen_sprite()  - software alpha blit into the ARGB frame buffer,
 *                      integer scaled + clipped (weapon viewmodel, HUD art).
 *   pol_load_bmp()   - lazy-decode an image file from disk (/ARENA/...) and
 *                      cache it. Native path for uncompressed 24/32-bit BMP
 *                      (keeps a real alpha channel); everything else goes
 *                      through the kernel codec (SYS_DECODE_IMAGE), which
 *                      returns flattened opaque RGB, so those files use a
 *                      magenta (0xFF00FF) color key for transparency.
 *
 * Also defines g_polish_ready (set by pol_init(), called from r_init) and
 * WEAK fallback stubs for every asset-pack getter, so ARENA links and runs
 * (with the original boxy art) even while some packs are missing; a pack's
 * strong definition simply replaces the weak stub at link time.
 *
 * Freestanding: game.h pulls the shared libc (malloc/free/memcpy/sys_open).
 * No busy waits, no blocking beyond the one-shot lazy file read.
 */
#include "game.h"
#include "polish.h"
#include <GL/gl.h>

int g_polish_ready = 0;

/* diagnostics: count of GL texture uploads that failed (white-box symptom) */
int g_pol_texfail = 0;
int g_pol_texok   = 0;

/* camera basis accessor implemented in render.c (single source of truth) */
void r_camera_basis(vec3 *eye, vec3 *right, vec3 *up, vec3 *fwd);

void pol_init(void) {
    g_polish_ready = 1;      /* getters are lazy; NULL results fall back      */
}

/* ====================================================================== */
/* GL texture cache: Image.px pointer -> TinyGL texture handle.           */
/* TinyGL stores every texture as 256x256 (it point-resizes on upload),   */
/* i.e. 256 KB each; cap the cache and recycle round-robin so a big       */
/* sprite set cannot exhaust the user heap.                               */
/* ====================================================================== */
#define POL_MAX_TEX 48

typedef struct { const unsigned int *key; GLuint tex; } TexSlot;
static TexSlot g_tex[POL_MAX_TEX];
static int g_tex_n = 0, g_tex_rr = 0;

static GLuint tex_upload(const Image *im) {
    int w = im->w, h = im->h;
    long n = (long)w * h;
    if (n <= 0 || n > 1024 * 1024) return 0;
    unsigned char *rgb = (unsigned char *)malloc((size_t)(n * 3));
    if (!rgb) return 0;
    const unsigned int *src = im->px;
    for (long i = 0; i < n; i++) {
        unsigned int p = src[i];
        unsigned char r, g, b;
        if ((p >> 24) < 128) {           /* transparent -> NO_DRAW magenta   */
            r = 255; g = 0; b = 255;
        } else {
            r = (unsigned char)(p >> 16);
            g = (unsigned char)(p >> 8);
            b = (unsigned char)p;
            if (r == 255 && g == 0 && b == 255) b = 250;  /* keep opaque     */
        }
        rgb[i * 3 + 0] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, (GLint)t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    free(rgb);
    return t;
}

void img_bind_gl(const Image *im) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) {
        glDisable(GL_TEXTURE_2D);        /* NULL: bind nothing (flat color)  */
        return;
    }
    for (int i = 0; i < g_tex_n; i++) {
        if (g_tex[i].key == im->px) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, (GLint)g_tex[i].tex);
            return;
        }
    }
    TexSlot *s;
    if (g_tex_n < POL_MAX_TEX) {
        s = &g_tex[g_tex_n++];
    } else {                             /* recycle the oldest slot          */
        s = &g_tex[g_tex_rr];
        g_tex_rr = (g_tex_rr + 1) % POL_MAX_TEX;
        if (s->tex) glDeleteTextures(1, &s->tex);
    }
    s->key = im->px;
    s->tex = tex_upload(im);
    if (!s->tex) { g_pol_texfail++; s->key = 0; glDisable(GL_TEXTURE_2D); return; }
    g_pol_texok++;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLint)s->tex);
}

/* ====================================================================== */
/* Camera-facing billboard inside the current 3D scene.                   */
/* ====================================================================== */
void billboard_draw(const Image *im, vec3 worldpos, float size, unsigned int tint) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0 || size <= 0.0f) return;
    vec3 eye, R, U, F;
    r_camera_basis(&eye, &R, &U, &F);
    /* fully behind the camera: skip early (TinyGL would clip it anyway)   */
    if (v3dot(v3sub(worldpos, eye), F) < -size) return;

    float hh = size * 0.5f;
    float hw = hh * ((float)im->w / (float)im->h);
    vec3 Rv = v3scale(R, hw), Uv = v3scale(U, hh);

    img_bind_gl(im);
    glColor3f((float)((tint >> 16) & 0xFF) * (1.0f / 255.0f),
              (float)((tint >>  8) & 0xFF) * (1.0f / 255.0f),
              (float)( tint        & 0xFF) * (1.0f / 255.0f));
    glBegin(GL_QUADS);
    /* image row 0 is the sprite's top: map it to the +up vertices         */
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(worldpos.x - Rv.x + Uv.x, worldpos.y - Rv.y + Uv.y, worldpos.z - Rv.z + Uv.z);
    glTexCoord2f(1.0f, 0.0f);
    glVertex3f(worldpos.x + Rv.x + Uv.x, worldpos.y + Rv.y + Uv.y, worldpos.z + Rv.z + Uv.z);
    glTexCoord2f(1.0f, 1.0f);
    glVertex3f(worldpos.x + Rv.x - Uv.x, worldpos.y + Rv.y - Uv.y, worldpos.z + Rv.z - Uv.z);
    glTexCoord2f(0.0f, 1.0f);
    glVertex3f(worldpos.x - Rv.x - Uv.x, worldpos.y - Rv.y - Uv.y, worldpos.z - Rv.z - Uv.z);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* ====================================================================== */
/* 2D screen sprite: alpha-composite into the ARGB blit buffer.           */
/* ====================================================================== */
void screen_sprite(const Image *im, unsigned int *blit, int bw, int bh,
                   int x, int y, int scale) {
    if (!im || !im->px || !blit || bw <= 0 || bh <= 0) return;
    if (scale < 1) scale = 1;
    int dw = im->w * scale, dh = im->h * scale;
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = (x + dw > bw) ? bw - x : dw;
    int y1 = (y + dh > bh) ? bh - y : dh;
    for (int dy = y0; dy < y1; dy++) {
        const unsigned int *srow = im->px + (long)(dy / scale) * im->w;
        unsigned int *drow = blit + (long)(y + dy) * bw + x;
        for (int dx = x0; dx < x1; dx++) {
            unsigned int p = srow[dx / scale];
            unsigned int a = p >> 24;
            if (a == 0) continue;
            if (a >= 255) { drow[dx] = p & 0x00FFFFFFu; continue; }
            unsigned int d  = drow[dx];
            unsigned int ia = 255u - a;
            unsigned int r = (((p >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) >> 8;
            unsigned int g = (((p >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * ia) >> 8;
            unsigned int b = (( p        & 0xFF) * a + ( d        & 0xFF) * ia) >> 8;
            drow[dx] = (r << 16) | (g << 8) | b;
        }
    }
}

/* ====================================================================== */
/* Disk image loader with cache: /ARENA/... -> Image (0xAARRGGBB).        */
/* ====================================================================== */
#define POL_MAX_IMG   64   /* menu art + 4 MAT_* + up to 25 /ARENA/TEX theme BMPs */
#define POL_PATH_MAX  96

/* state: 0 empty, 1 ok, 2 failed-but-retryable, 3 failed-permanent. A boot-time
 * transient read failure (#444-class ATA-DMA contention) must NOT be cached
 * forever, or the asset stays broken for the whole session; retry a bounded
 * number of times so the load succeeds once the disk settles. */
/* Retries are THROTTLED (every POL_RETRY_MS) so a large asset that comes back
 * corrupt does not trigger a 3 MB re-read every single frame; the budget then
 * spans the noisy first ~16 s of boot rather than 12 back-to-back frames. */
#define POL_MAX_TRIES 40
#define POL_RETRY_MS  400
typedef struct { char path[POL_PATH_MAX]; Image im; int state; int tries; unsigned last_ms; } ImgSlot;
static ImgSlot g_img[POL_MAX_IMG];
static int g_img_n = 0;

/* Detect an ATA-DMA partial-read (#444: correct length, NUL holes). A real
 * photo/texture has almost no EXACTLY-opaque-black (0xFF000000) pixels, but a
 * zero-filled region decodes to exactly that. On a bottom-up BMP the file tail
 * (= the TOP image rows) is what a truncated DMA zero-fills, which is why the
 * big MENUBG.BMP came back black on top. >30% opaque-black = reject + re-read. */
static int bmp_looks_corrupt(const Image *im) {
    if (!im || !im->px || im->w < 8 || im->h < 8) return 0;
    long n = (long)im->w * im->h, black = 0;
    for (long i = 0; i < n; i++)
        if (im->px[i] == 0xFF000000u) black++;
    return black * 100 > n * 30;
}

static unsigned char *pol_read_whole(const char *path, long *out_len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 65536, len = 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) { sys_close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            unsigned char *nb = (unsigned char *)realloc(buf, (size_t)(cap * 2));
            if (!nb) { free(buf); sys_close(fd); return 0; }
            buf = nb; cap *= 2;
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

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Native uncompressed 24/32-bit BMP: the only on-disk format whose alpha
 * channel survives (the kernel codec flattens transparency). 0 on success. */
static int bmp_native(const unsigned char *f, long len, Image *out) {
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return -1;
    unsigned off = rd32(f + 10);
    int w   = (int)rd32(f + 18);
    int hh  = (int)rd32(f + 22);
    int bpp = (int)rd16(f + 28);
    unsigned comp = rd32(f + 30);
    int topdown = hh < 0;
    int h = topdown ? -hh : hh;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return -1;
    if (bpp != 24 && bpp != 32) return -1;
    if (comp != 0 && comp != 3) return -1;   /* BI_RGB / BI_BITFIELDS       */
    long stride = ((long)w * (bpp / 8) + 3) & ~3L;
    if ((long)off + stride * h > len) return -1;

    unsigned int *px = (unsigned int *)malloc((size_t)w * h * 4);
    if (!px) return -1;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = f + off + stride * (topdown ? y : (h - 1 - y));
        unsigned int *drow = px + (long)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *s = row + (long)x * (bpp / 8);
            unsigned int b = s[0], g = s[1], r = s[2];
            unsigned int a = (bpp == 32) ? s[3] : 255u;
            if (bpp == 32 && a) any_alpha = 1;
            drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    long n = (long)w * h;
    if (bpp == 32 && !any_alpha)             /* all-zero alpha = opaque BMP */
        for (long i = 0; i < n; i++) px[i] |= 0xFF000000u;
    for (long i = 0; i < n; i++)             /* magenta color key           */
        if ((px[i] & 0x00FFFFFFu) == 0x00FF00FFu) px[i] &= 0x00FFFFFFu;
    out->w = w; out->h = h; out->px = px;
    return 0;
}

/* Kernel codec path (PNG/JPEG/GIF/compressed BMP). Decoded pixels come back
 * opaque and flattened, so transparency uses the magenta color key.        */
static int decode_kernel(const unsigned char *f, long len, Image *out) {
    const int TW = 1024, TH = 1024;          /* codec only ever downscales   */
    unsigned cap = (unsigned)TW * TH * 4u;
    unsigned int *tmp = (unsigned int *)malloc(cap);
    if (!tmp) return -1;
    int dims[2] = {0, 0};
    int n = decode_image(f, (unsigned)len, TW, TH, tmp, cap, dims);
    int w = dims[0], h = dims[1];
    if (n <= 0 || w < 1 || h < 1) { free(tmp); return -1; }
    unsigned int *px = (unsigned int *)malloc((size_t)w * h * 4);
    if (!px) { free(tmp); return -1; }
    long cnt = (long)w * h;
    for (long i = 0; i < cnt; i++) {
        unsigned int p = tmp[i] & 0x00FFFFFFu;
        px[i] = (p == 0x00FF00FFu) ? p : (0xFF000000u | p);
    }
    free(tmp);
    out->w = w; out->h = h; out->px = px;
    return 0;
}

const Image *pol_load_bmp(const char *path) {
    if (!path || !path[0]) return 0;
    ImgSlot *s = 0;
    for (int i = 0; i < g_img_n; i++)
        if (!strcmp(g_img[i].path, path)) {
            if (g_img[i].state == 1) return &g_img[i].im;   /* loaded         */
            if (g_img[i].state == 3) return 0;              /* gave up        */
            s = &g_img[i];                                  /* retry below    */
            break;
        }
    if (!s) {
        if (g_img_n >= POL_MAX_IMG) return 0;
        s = &g_img[g_img_n++];
        strncpy(s->path, path, POL_PATH_MAX - 1);
        s->path[POL_PATH_MAX - 1] = 0;
        s->tries = 0;
        s->last_ms = 0;
    }
    /* Throttle re-reads: the first attempt runs now; retries wait POL_RETRY_MS
     * so a corrupt large file is not re-read every frame (which would itself
     * cause the DMA contention that corrupted it). Until it loads clean the
     * caller gets NULL and shows its graceful fallback.                       */
    unsigned now = (unsigned)uptime_ms();
    if (s->tries > 0 && (now - s->last_ms) < POL_RETRY_MS) return 0;
    s->last_ms = now;
    s->state = 2;

    long len = 0;
    unsigned char *f = pol_read_whole(path, &len);
    int ok = 0;
    if (f) {
        Image im = {0, 0, 0};
        ok = (bmp_native(f, len, &im) == 0) || (decode_kernel(f, len, &im) == 0);
        free(f);
        if (ok && bmp_looks_corrupt(&im)) {   /* partial read: retry, don't cache */
            free((void *)im.px);
            ok = 0;
        }
        if (ok) { s->im = im; s->state = 1; return &s->im; }
    }
    if (++s->tries >= POL_MAX_TRIES) s->state = 3;   /* stop retrying         */
    return 0;
}

/* ====================================================================== */
/* WEAK fallback stubs for the asset packs. Each pack's real (strong)     */
/* definition overrides its stub at link time; a missing pack simply      */
/* yields NULL and render.c keeps the original boxy art on screen.        */
/* ====================================================================== */
#define POL_WEAK __attribute__((weak))

POL_WEAK const Image *tx_wall (int theme, int id) { (void)theme; (void)id; return 0; }
POL_WEAK const Image *tx_floor(int theme, int id) { (void)theme; (void)id; return 0; }
POL_WEAK const Image *tx_ceil (int theme, int id) { (void)theme; (void)id; return 0; }
POL_WEAK const Image *tx_sky  (int theme)         { (void)theme; return 0; }

POL_WEAK const Image *ch_frame(int skin, int state, int angle, int frame)
{ (void)skin; (void)state; (void)angle; (void)frame; return 0; }
POL_WEAK int ch_state_frames(int state) { (void)state; return 1; }

POL_WEAK const Image *it_sprite(int item_kind, int value) { (void)item_kind; (void)value; return 0; }
POL_WEAK const Image *pj_sprite(int proj_kind, int frame) { (void)proj_kind; (void)frame; return 0; }
POL_WEAK const Image *fx_sprite(int fx_id, int frame)     { (void)fx_id; (void)frame; return 0; }

POL_WEAK const Image *wv_sprite(int weapon, int frame) { (void)weapon; (void)frame; return 0; }
POL_WEAK int wv_fire_frames(int weapon) { (void)weapon; return 1; }

POL_WEAK const Image *sky_face(int theme, int face) { (void)theme; (void)face; return 0; }

/* optional world_art.h grading extras (0 = keep the level's own colours)   */
POL_WEAK unsigned int world_fog_rgb(int theme)     { (void)theme; return 0; }
POL_WEAK unsigned int world_ambient_rgb(int theme) { (void)theme; return 0; }
