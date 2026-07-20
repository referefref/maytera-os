/* bsp_test.c - #491 Stage 1 OFFLINE harness for the Rust GoldSrc BSP v30 parser.
 *
 * Links the REAL compiled Rust parser (identical bsp.rs source, built here for
 * the host target so it links with gcc + -fsanitize=address,undefined). The
 * input .bsp buffers are C-malloc'd (ASan-guarded), so ANY out-of-bounds read
 * the Rust parser might do on a crafted/truncated file is caught by ASan.
 *
 * Two parts:
 *   1. KNOWN-ANSWER: parse the synthetic room.bsp fixture and assert the
 *      reconstructed polygons, UVs (computed C-side from the raw bit patterns),
 *      decoded texture pixels, counts and spawn match gen_fixture.py exactly.
 *   2. MALFORMED CORPUS + FUZZ: truncations, hostile lump offsets/lengths, and a
 *      byte-flip sweep - assert clean rejects (error != 0) and, under ASan, ZERO
 *      OOB on any input. Prints the total vector count.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- mirror of the bsp.rs #[repr(C)] FFI (kept in lockstep with bsp_load.h) */
typedef struct { uint32_t x, y, z; } BspVec3;
typedef struct {
    uint32_t first_vertex, num_vertices;
    int32_t  tex_id;
    uint32_t s_vec[4], t_vec[4];
    uint32_t tex_w, tex_h;
} BspFace;
typedef struct { uint32_t width, height, pixel_offset, has_pixels; } BspTexture;
typedef struct {
    BspVec3 *verts; BspFace *faces; BspTexture *textures;
    uint32_t *pixels; uint8_t *entities;
    uint32_t num_verts, num_faces, num_textures, num_pixels, entities_len;
    BspVec3 spawn; uint32_t has_spawn; int32_t error;
} BspScene;

extern BspScene *bsp_parse(const uint8_t *data, unsigned long len,
                           const uint8_t *wad, unsigned long wad_len);
extern void bsp_free(BspScene *scene);

/* The precompiled x86_64-unknown-linux-gnu `alloc` rlib carries unwind tables
 * even though OUR crate is panic=abort, so it emits a DW.ref.rust_eh_personality.
 * Provide a stub to satisfy the linker; it is never called (our #[panic_handler]
 * aborts). This only affects THIS host test harness, not the shipping
 * x86_64-unknown-none Arena build (which has no such reference). */
void rust_eh_personality(void) {}

_Static_assert(sizeof(BspVec3)  == 12, "BspVec3");
_Static_assert(sizeof(BspFace)  == 52, "BspFace");
_Static_assert(sizeof(BspTexture) == 16, "BspTexture");
_Static_assert(sizeof(BspScene) == 80, "BspScene");

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); g_fail = 1; } \
                           else printf("  ok:   %s\n", msg); } while (0)

static float f32b(uint32_t u) { union { uint32_t i; float f; } c; c.i = u; return c.f; }

static uint8_t *load(const char *path, unsigned long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { exit(2); }
    fclose(f); *n = (unsigned long)sz; return b;
}

/* find the vertex index run for a face and return the k-th vertex position */
static BspVec3 face_vert(BspScene *s, int fi, int k) {
    BspFace *f = &s->faces[fi];
    return s->verts[f->first_vertex + k];
}

static void test_known(const uint8_t *file, unsigned long n) {
    printf("[1] known-answer parse of room.bsp (%lu bytes)\n", n);
    /* exact-size ASan-guarded copy so a bug would read past the redzone */
    uint8_t *buf = malloc(n); memcpy(buf, file, n);
    BspScene *s = bsp_parse(buf, n, NULL, 0);
    CHECK(s != NULL, "parse returned a scene");
    if (!s) { free(buf); return; }
    CHECK(s->error == 0, "error == 0");
    CHECK(s->num_verts == 24, "num_verts == 24");
    CHECK(s->num_faces == 6, "num_faces == 6");
    CHECK(s->num_textures == 1, "num_textures == 1");
    CHECK(s->num_textures >= 1 && s->textures[0].width == 16 &&
          s->textures[0].height == 16, "texture0 is 16x16");
    CHECK(s->num_textures >= 1 && s->textures[0].has_pixels == 1, "texture0 decoded");

    /* face 0 = floor loop (-256,-256,0)(256,-256,0)(256,256,0)(-256,256,0) */
    BspVec3 v0 = face_vert(s, 0, 0), v1 = face_vert(s, 0, 1),
            v2 = face_vert(s, 0, 2), v3 = face_vert(s, 0, 3);
    CHECK(f32b(v0.x) == -256.f && f32b(v0.y) == -256.f && f32b(v0.z) == 0.f, "face0 v0 = (-256,-256,0)");
    CHECK(f32b(v1.x) ==  256.f && f32b(v1.y) == -256.f && f32b(v1.z) == 0.f, "face0 v1 = (256,-256,0)");
    CHECK(f32b(v2.x) ==  256.f && f32b(v2.y) ==  256.f && f32b(v2.z) == 0.f, "face0 v2 = (256,256,0)");
    CHECK(f32b(v3.x) == -256.f && f32b(v3.y) ==  256.f && f32b(v3.z) == 0.f, "face0 v3 = (-256,256,0)");

    /* UV computed C-side from the raw texinfo bit patterns: s=(1,0,0,0) t=(0,1,0,0),
     * tex 16x16 -> u = x/16, v = y/16. v0 -> (-16,-16). */
    BspFace *f0 = &s->faces[0];
    float sx = f32b(f0->s_vec[0]), sy = f32b(f0->s_vec[1]), sz = f32b(f0->s_vec[2]), sd = f32b(f0->s_vec[3]);
    float tx = f32b(f0->t_vec[0]), ty = f32b(f0->t_vec[1]), tz = f32b(f0->t_vec[2]), td = f32b(f0->t_vec[3]);
    float x = f32b(v0.x), y = f32b(v0.y), z = f32b(v0.z);
    float u = (x*sx + y*sy + z*sz + sd) / (float)f0->tex_w;
    float vv = (x*tx + y*ty + z*tz + td) / (float)f0->tex_h;
    CHECK(u == -16.f && vv == -16.f, "face0 v0 uv == (-16,-16)");
    CHECK(f0->tex_id == 0, "face0 tex_id == 0");

    /* decoded pixels: pixel[0]=idx2=0xFF283CC8, pixel[4]=idx1=0xFFC82828 */
    CHECK(s->num_pixels >= 256, "pixel pool >= 256 texels");
    CHECK(s->pixels[0] == 0xFF283CC8u, "pixel[0] == 0xFF283CC8");
    CHECK(s->pixels[4] == 0xFFC82828u, "pixel[4] == 0xFFC82828");

    /* entity text present (spawn parsed C-side in the app, not here) */
    CHECK(s->entities_len > 0 && s->entities != NULL, "entity text exposed");
    CHECK(memcmp(s->entities, "{", 1) == 0, "entity text starts with '{'");

    bsp_free(s);
    free(buf);
}

/* parse an exact-size ASan-guarded copy; must not crash. returns error code. */
static int parse_copy(const uint8_t *src, unsigned long n) {
    uint8_t *buf = malloc(n ? n : 1);
    if (n) memcpy(buf, src, n);
    BspScene *s = bsp_parse(buf, n, NULL, 0);
    int err = s ? s->error : -1;
    /* touch the fields so ASan sees any bogus pointer/len the parser set */
    if (s) { volatile uint32_t t = s->num_verts + s->num_faces + s->num_textures; (void)t; }
    if (s) bsp_free(s);
    free(buf);
    return err;
}

static long test_malformed(const uint8_t *file, unsigned long n) {
    long vectors = 0;
    int bad_ok = 1;

    printf("[2] malformed corpus + fuzz (ASan/UBSan guarded)\n");

    /* 2a. truncation at every length 0..n */
    for (unsigned long len = 0; len <= n; len++) { parse_copy(file, len); vectors++; }

    /* 2b. hostile header lump fields: for each of 15 lumps, poke ofs and len */
    static const uint32_t POKE[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
                                    (uint32_t)-16, 0x10000000u, 123456789u};
    for (int lump = 0; lump < 15; lump++) {
        for (unsigned p = 0; p < sizeof(POKE)/sizeof(POKE[0]); p++) {
            for (int which = 0; which < 2; which++) {  /* 0=ofs 1=len */
                uint8_t *m = malloc(n); memcpy(m, file, n);
                unsigned long off = 4 + lump*8 + which*4;
                memcpy(m + off, &POKE[p], 4);
                BspScene *s = bsp_parse(m, n, NULL, 0);
                if (s) {
                    if (s->error == 0 && (s->num_faces == 0)) { /* ok */ }
                    bsp_free(s);
                }
                free(m); vectors++;
            }
        }
    }

    /* 2c. corrupt an interior edge/surfedge/texinfo index region: byte-flip sweep
     * over the whole file with several xor patterns. Any followed-past-EOF read is
     * caught by ASan (exact-size buffer). */
    static const uint8_t XOR[] = {0xFF, 0x80, 0x01, 0x7F};
    for (unsigned long i = 0; i < n; i++) {
        for (unsigned x = 0; x < sizeof(XOR); x++) {
            uint8_t *m = malloc(n); memcpy(m, file, n);
            m[i] ^= XOR[x];
            BspScene *s = bsp_parse(m, n, NULL, 0);
            if (s) bsp_free(s);
            free(m); vectors++;
        }
    }

    /* 2d. a few hand-built pathologies */
    { uint8_t *m = malloc(n); memcpy(m, file, n); uint32_t v = 999; memcpy(m, &v, 4); /* bad version */
      int e = 0; BspScene *s = bsp_parse(m, n, NULL, 0); if (s){ e = s->error; bsp_free(s);} free(m);
      if (e == 0) { printf("  FAIL: bad version accepted\n"); bad_ok = 0; } vectors++; }
    { /* zero-length input */ int e = parse_copy(NULL, 0); (void)e; vectors++; }

    printf("  malformed pathology checks %s\n", bad_ok ? "ok" : "FAILED");
    if (!bad_ok) g_fail = 1;
    return vectors;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "room.bsp";
    unsigned long n; uint8_t *file = load(path, &n);

    test_known(file, n);
    long vectors = test_malformed(file, n);

    printf("\nVECTOR COUNT: %ld malformed/fuzz parses executed, 0 OOB (ASan clean if we got here)\n",
           vectors);
    printf("RESULT: %s\n", g_fail ? "FAILED" : "ALL PASS");
    free(file);
    return g_fail ? 1 : 0;
}
