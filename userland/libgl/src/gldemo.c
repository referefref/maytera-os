// gldemo.c - shared TinyGL demo render cores: spinning textured cube + 3D
// "matrix code rain". See gldemo.h. (#319)
#include "../include/gldemo.h"
#include "../include/GL/gl.h"
#include "../include/zbuffer.h"
#include <math.h>
#include "font8x8_basic.h"

#define MAXW 1600
#define MAXH 1000

static ZBuffer *g_zb = 0;
static int g_mode = GLDEMO_CUBE;
static int g_w = 0, g_h = 0;
static int g_inited = 0;
static float g_ang = 0.0f;
static unsigned int g_seed = 2463534242u;

static unsigned int rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}
static float rndf(void) { return (float)(rnd() & 0xFFFFFF) / (float)0x1000000; }

// ---------------------------------------------------------------------------
// Cube texture (colored checkerboard)
// ---------------------------------------------------------------------------
static unsigned char g_cubetex[256*256*3];
static void build_cube_texture(void) {
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int c = ((x >> 5) ^ (y >> 5)) & 1;
            int r, g, b;
            if (c) { r = 235; g = 200; b = 70; } else { r = 30; g = 90; b = 175; }
            int grad = (x + y) >> 3;
            r = r - 16 + (grad & 31); if (r < 0) r = 0; if (r > 255) r = 255;
            g = g - 16 + (grad & 31); if (g < 0) g = 0; if (g > 255) g = 255;
            int i = (y * 256 + x) * 3;
            g_cubetex[i] = r; g_cubetex[i+1] = g; g_cubetex[i+2] = b;
        }
}

// ---------------------------------------------------------------------------
// Matrix glyph atlas: 256x256, 8x8 grid of 32x32 cells (64 glyphs). White on
// black, scaled 8x8 font glyphs. Tinted green at draw time, blended additively.
// ---------------------------------------------------------------------------
static unsigned char g_glyphtex[256*256*3];
static const char *GLYPHSET =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.=*+-<>|/\\!?#$%&@^~()[]{}";
static void build_glyph_atlas(void) {
    for (int i = 0; i < 256*256*3; i++) g_glyphtex[i] = 0;
    int n = 0; while (GLYPHSET[n]) n++;
    for (int cell = 0; cell < 64; cell++) {
        int cc = cell & 7, cr = cell >> 3;
        unsigned char ch = (cell < n) ? (unsigned char)GLYPHSET[cell] : 32;
        GLbyte *gl = font8x8_basic[ch];
        // place an 8x8 glyph scaled x3 (24x24) centered in the 32x32 cell
        int ox = cc * 32 + 4, oy = cr * 32 + 4;
        for (int gy = 0; gy < 8; gy++) {
            int bits = (unsigned char)gl[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (bits & (1 << gx)) {
                    for (int sy = 0; sy < 3; sy++)
                        for (int sx = 0; sx < 3; sx++) {
                            int px = ox + gx*3 + sx;
                            int py = oy + gy*3 + sy;
                            int idx = (py * 256 + px) * 3;
                            g_glyphtex[idx] = 255;
                            g_glyphtex[idx+1] = 255;
                            g_glyphtex[idx+2] = 255;
                        }
                }
            }
        }
    }
}

// matrix stream state
#define MAXSTREAMS 120
#define MAXTRAIL   16
typedef struct { float x, y, z, speed; int len; unsigned char cell[MAXTRAIL]; } stream_t;
static stream_t g_streams[MAXSTREAMS];
static int g_nstreams = 0;

static void respawn_stream(stream_t *s, int top) {
    s->z = -3.5f - rndf() * 16.0f;       // depth
    float spread = -s->z * 1.3f;
    s->x = (rndf() * 2.0f - 1.0f) * spread;
    float ytop = -s->z * 0.95f;
    s->y = top ? (ytop + rndf() * ytop) : ((rndf() * 2.0f - 1.0f) * ytop);
    s->speed = 0.04f + rndf() * 0.10f + (-s->z) * 0.004f;
    s->len = 6 + (int)(rndf() * (MAXTRAIL - 6));
    for (int k = 0; k < MAXTRAIL; k++) s->cell[k] = (unsigned char)(rnd() % 64);
}
static void init_streams(void) {
    g_nstreams = MAXSTREAMS;
    for (int i = 0; i < g_nstreams; i++) respawn_stream(&g_streams[i], 0);
}

// ---------------------------------------------------------------------------
// GL context setup
// ---------------------------------------------------------------------------
static void setup_projection(void) {
    glViewport(0, 0, g_w, g_h);
    double aspect = (double)g_w / (double)g_h;
    double nh = 0.55;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-nh * aspect, nh * aspect, -nh, nh, 1.5, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

int gldemo_init(int mode, int w, int h) {
    if (w < 64) w = 64; if (h < 64) h = 64;
    if (w > MAXW) w = MAXW; if (h > MAXH) h = MAXH;
    if (g_inited) gldemo_shutdown();
    g_mode = mode; g_w = w; g_h = h;
    g_zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
    if (!g_zb) return 0;
    glInit(g_zb);
    g_inited = 1;
    GLuint tid;
    glShadeModel(GL_SMOOTH);
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    if (mode == GLDEMO_CUBE) {
        build_cube_texture();
        glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, g_cubetex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glClearColor(0.04f, 0.04f, 0.09f, 0.0f);
        glEnable(GL_DEPTH_TEST);
    } else {
        build_glyph_atlas();
        glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, g_glyphtex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glClearColor(0.0f, 0.02f, 0.0f, 0.0f);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        init_streams();
    }
    setup_projection();
    g_ang = 0.0f;
    return 1;
}

void gldemo_resize(int w, int h) {
    int m = g_mode;
    gldemo_init(m, w, h);
}

void gldemo_shutdown(void) {
    if (!g_inited) return;
    glClose();
    ZB_close(g_zb);
    g_zb = 0;
    g_inited = 0;
}

int gldemo_width(void)  { return g_w; }
int gldemo_height(void) { return g_h; }

// ---------------------------------------------------------------------------
// Cube frame
// ---------------------------------------------------------------------------
static void face(float sh,
                 float ax,float ay,float az, float bx,float by,float bz,
                 float cx,float cy,float cz, float dx,float dy,float dz) {
    glColor3f(sh, sh, sh);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex3f(ax,ay,az);
    glTexCoord2f(1,0); glVertex3f(bx,by,bz);
    glTexCoord2f(1,1); glVertex3f(cx,cy,cz);
    glTexCoord2f(0,1); glVertex3f(dx,dy,dz);
    glEnd();
}
static void draw_cube(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.0f);
    glRotatef(g_ang * 0.7f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_ang,        0.0f, 1.0f, 0.0f);
    glRotatef(g_ang * 0.3f, 0.0f, 0.0f, 1.0f);
    float s = 1.3f;
    face(1.00f,-s,-s, s,  s,-s, s,  s, s, s, -s, s, s);
    face(0.55f, s,-s,-s, -s,-s,-s, -s, s,-s,  s, s,-s);
    face(0.80f, s,-s, s,  s,-s,-s,  s, s,-s,  s, s, s);
    face(0.65f,-s,-s,-s, -s,-s, s, -s, s, s, -s, s,-s);
    face(0.90f,-s, s, s,  s, s, s,  s, s,-s, -s, s,-s);
    face(0.45f,-s,-s,-s,  s,-s,-s,  s,-s, s, -s,-s, s);
    g_ang += 1.6f;
    if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// ---------------------------------------------------------------------------
// Matrix frame
// ---------------------------------------------------------------------------
static void glyph_quad(float x, float y, float z, float sz, unsigned char cell,
                       float r, float g, float b) {
    float u0 = (float)(cell & 7) / 8.0f;
    float v0 = (float)(cell >> 3) / 8.0f;
    float u1 = u0 + 1.0f/8.0f;
    float v1 = v0 + 1.0f/8.0f;
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v1); glVertex3f(x - sz, y - sz, z);
    glTexCoord2f(u1, v1); glVertex3f(x + sz, y - sz, z);
    glTexCoord2f(u1, v0); glVertex3f(x + sz, y + sz, z);
    glTexCoord2f(u0, v0); glVertex3f(x - sz, y + sz, z);
    glEnd();
}
static void draw_matrix(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    float gap = 0.62f;     // vertical spacing between glyphs (world units)
    float sz  = 0.30f;     // glyph half-size
    for (int i = 0; i < g_nstreams; i++) {
        stream_t *s = &g_streams[i];
        s->y -= s->speed;
        float ybot = s->z * 0.95f; // negative; bottom limit
        if (s->y < ybot) respawn_stream(s, 1);
        // occasionally mutate a glyph for shimmer
        if ((rnd() & 31) == 0) s->cell[rnd() % s->len] = (unsigned char)(rnd() % 64);
        for (int k = 0; k < s->len; k++) {
            float gy = s->y + (float)k * gap;   // trail extends upward
            float t = 1.0f - (float)k / (float)s->len;  // 1 at head .. ->0 tail
            float r, g, b;
            if (k == 0) { r = 0.75f; g = 1.0f; b = 0.80f; }   // bright head
            else { r = 0.0f; g = 0.30f + 0.70f * t; b = 0.10f * t; }
            glyph_quad(s->x, gy, s->z, sz, s->cell[k], r, g, b);
        }
    }
}

// ---------------------------------------------------------------------------
void gldemo_frame(uint32_t *dst, int dst_pitch) {
    if (!g_inited) return;
    if (g_mode == GLDEMO_CUBE) draw_cube();
    else draw_matrix();
    ZB_copyFrameBuffer(g_zb, dst, dst_pitch * (int)sizeof(uint32_t));
    for (int y = 0; y < g_h; y++) {
        uint32_t *row = dst + (long)y * dst_pitch;
        for (int x = 0; x < g_w; x++) row[x] |= 0xFF000000u;
    }
}
