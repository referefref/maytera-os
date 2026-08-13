/* side1_test.c - #568 regression test: recursive_hull_check() must negate the
 * plane normal when the impact was approached from the plane's NEGATIVE side
 * (side==1), matching the reference SV_RecursiveHullCheck. See
 * gen_side1_fixture.py for why hull_test.c/dust2_test.c's existing fixtures
 * cannot catch this (both only ever exercise side==0 impacts by construction).
 *
 * The fixture is a single clipnode describing an ORDINARY floor at z=0 (solid
 * below, open air above), but with the split plane stored FACING DOWN
 * (normal=(0,0,-1)) and its children reversed to match - physically the same
 * floor a "normal" up-facing plane would describe, just algebraically encoded
 * the other way, which is a choice a real qbsp/hlbsp compile is free to make
 * for any given internal split. A correct trace reports the SAME real-world
 * outward normal, (0,0,1), regardless of which way the underlying plane is
 * stored.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void rust_eh_personality(void) {}

typedef struct { uint32_t x, y, z; } BspVec3;
typedef struct {
    uint32_t first_vertex, num_vertices; int32_t tex_id;
    uint32_t s_vec[4], t_vec[4]; uint32_t tex_w, tex_h;
} BspFace;
typedef struct { uint32_t width, height, pixel_offset, has_pixels; } BspTexture;
typedef struct { BspVec3 normal; uint32_t dist; int32_t ptype; } BspPlane;
typedef struct { int32_t planenum; int32_t children[2]; } BspClipnode;
typedef struct {
    BspVec3 *verts; BspFace *faces; BspTexture *textures; uint32_t *pixels; uint8_t *entities;
    uint32_t num_verts, num_faces, num_textures, num_pixels, entities_len;
    BspVec3 spawn; uint32_t has_spawn; int32_t error;
    BspPlane *planes; BspClipnode *clipnodes;
    uint32_t num_planes, num_clipnodes;
    int32_t hull_headnode[4];
    uint32_t hull_ok;
} BspScene;
typedef struct {
    uint32_t frac; BspVec3 end; BspVec3 normal; uint32_t start_solid, all_solid;
} HullTrace;

extern BspScene *bsp_parse(const uint8_t *data, unsigned long len, const uint8_t *wad, unsigned long wad_len);
extern void bsp_free(BspScene *scene);
extern int32_t bsp_hull_trace(const BspScene *scene, int32_t hull,
                              uint32_t p1x, uint32_t p1y, uint32_t p1z,
                              uint32_t p2x, uint32_t p2y, uint32_t p2z,
                              HullTrace *out);

static uint32_t bits(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static float unbits(uint32_t u) { union { float f; uint32_t u; } c; c.u = u; return c.f; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS: %s\n", msg); \
    else { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f);
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "side1_floor.bsp";
    long len; uint8_t *buf = read_file(path, &len);
    if (!buf) return 1;

    BspScene *sc = bsp_parse(buf, (unsigned long)len, NULL, 0);
    CHECK(sc != NULL, "bsp_parse returned non-null");
    CHECK(sc && sc->error == 0, "bsp_parse error == 0");
    CHECK(sc && sc->hull_ok == 1, "hull_ok == 1");
    CHECK(sc && sc->num_clipnodes == 1, "num_clipnodes == 1 (the single reversed node)");
    if (!sc || sc->error != 0 || !sc->hull_ok) { printf("\nSOME TESTS FAILED\n"); return 1; }

    printf("\n-- fall from open air (z=50) through the floor (z=0), side==1 case --\n");
    {
        HullTrace ht; memset(&ht, 0, sizeof(ht));
        int32_t rc = bsp_hull_trace(sc, 1, bits(0),bits(0),bits(50), bits(0),bits(0),bits(-50), &ht);
        CHECK(rc == 0, "rc==0");
        CHECK(!ht.start_solid, "start not solid (z=50 is open air)");
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "floor blocks the fall (0<frac<1)");
        float ez = unbits(ht.end.z);
        CHECK(ez > 0.0f, "stop point end.z > 0 (did not sink through the floor)");
        float nz = unbits(ht.normal.z);
        /* THE ACTUAL #568 REGRESSION CHECK: the pre-fix code returned -1.0
         * here (the raw, un-negated stored plane normal); a real map's floor
         * encoded this way would never pass world.c's
         * `best_n.z > FLOOR_NORMAL_Z(0.7) && vel.z<0` ground check, so the
         * player's on_ground would never be set standing on it. */
        CHECK(nz > 0.7f, "impact normal.z > 0.7: reported as a walkable FLOOR facing up");
        CHECK(fabsf(nz - 1.0f) < 0.01f, "impact normal.z ~= 1.0 exactly (0,0,1), not the raw stored (0,0,-1)");
    }

    printf("\n-- standing just above the floor must be open (non-vacuity) --\n");
    {
        HullTrace ht; memset(&ht, 0, sizeof(ht));
        int32_t rc = bsp_hull_trace(sc, 1, bits(0),bits(0),bits(10), bits(0),bits(0),bits(18), &ht);
        CHECK(rc == 0, "rc==0");
        CHECK(fabsf(unbits(ht.frac) - 1.0f) < 0.001f, "frac==1.0 (open air is not solid)");
    }

    bsp_free(sc);
    free(buf);

    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail;
}
