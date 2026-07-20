/* hull_test.c - #491 Stage 2 offline known-answer test for the Rust hull
 * trace (bsp.rs bsp_hull_trace / bsp_hull_box), same technique as
 * bsp_test/bsp_test.c: link the IDENTICAL Rust source built for the HOST
 * target so it runs as a normal Linux process, and assert against KNOWN
 * geometry (a synthetic room fixture built by gen_hull_fixture.py, NOT real
 * de_dust2 - this isolates and proves the ALGORITHM against ground truth
 * before spending real VM time on the integration).
 *
 * Room fixture: a hollow AABB [-200,200] x [-200,200] x [0,200], hull 1's
 * headnode = clipnode 0 (a 6-plane chain: "outside any plane -> SOLID").
 *
 * Build: see run_hull_test.sh in this dir.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The Rust crate is built -C panic=abort, but libcore/liballoc in the shipped
 * rlibs still emit a DW.ref.rust_eh_personality reference. Same one-line shim
 * the sibling bsp_test.c / bsp_guard.c already use; it is never called. */
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
extern int32_t bsp_hull_box(int32_t hull, BspVec3 *out_mins, BspVec3 *out_maxs);

static uint32_t bits(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static float unbits(uint32_t u) { union { float f; uint32_t u; } c; c.u = u; return c.f; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS: %s\n", msg); \
    else { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    *out_len = n;
    return buf;
}

static HullTrace trace(const BspScene *sc, float ax, float ay, float az,
                       float bx, float by, float bz, int32_t *rc_out) {
    HullTrace ht; memset(&ht, 0, sizeof(ht));
    int32_t rc = bsp_hull_trace(sc, 1,
                                bits(ax), bits(ay), bits(az),
                                bits(bx), bits(by), bits(bz), &ht);
    if (rc_out) *rc_out = rc;
    return ht;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "hull_room.bsp";
    long len; uint8_t *buf = read_file(path, &len);
    if (!buf) return 1;

    BspScene *sc = bsp_parse(buf, (unsigned long)len, NULL, 0);
    CHECK(sc != NULL, "bsp_parse returned non-null");
    CHECK(sc->error == 0, "bsp_parse error == 0");
    CHECK(sc->hull_ok == 1, "hull_ok == 1 (PLANES+CLIPNODES+MODELS all present)");
    CHECK(sc->num_planes == 6, "num_planes == 6");
    CHECK(sc->num_clipnodes == 6, "num_clipnodes == 6");
    CHECK(sc->hull_headnode[1] == 0, "hull_headnode[1] == 0 (our chain root)");

    printf("\n-- bsp_hull_box: fixed engine hull constants --\n");
    BspVec3 mn, mx;
    CHECK(bsp_hull_box(1, &mn, &mx) == 0, "bsp_hull_box(1) rc==0");
    CHECK(NEAR(unbits(mn.x), -16.0f, 0.01f) && NEAR(unbits(mn.y), -16.0f, 0.01f) && NEAR(unbits(mn.z), -36.0f, 0.01f),
          "hull1 mins == (-16,-16,-36)");
    CHECK(NEAR(unbits(mx.x), 16.0f, 0.01f) && NEAR(unbits(mx.y), 16.0f, 0.01f) && NEAR(unbits(mx.z), 36.0f, 0.01f),
          "hull1 maxs == (16,16,36)");
    CHECK(bsp_hull_box(4, &mn, &mx) == -1, "bsp_hull_box(4) [out of range] rc==-1");

    printf("\n-- 1. clear path fully inside the room: must NOT hit --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 0,0,100, 0,0,150, &rc);
        CHECK(rc == 0, "rc==0");
        CHECK(ht.all_solid == 0 && ht.start_solid == 0, "no solid flags");
        CHECK(NEAR(unbits(ht.frac), 1.0f, 0.001f), "frac == 1.0 (never blocked)");
    }

    printf("\n-- 2. walk into the +X wall: must STOP, normal points -X (into the room) --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 150,0,100, 250,0,100, &rc);
        CHECK(rc == 0, "rc==0");
        CHECK(ht.all_solid == 0 && ht.start_solid == 0, "not embedded (started in open air)");
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "0 < frac < 1 (blocked partway)");
        /* exact crossing at x=200 is frac=0.5 of a 150->250 segment; the
         * DIST_EPSILON back-off must land STRICTLY before that, never past it -
         * this is what stops sinking/jitter into the surface. */
        CHECK(frac < 0.5f, "frac backed off BEFORE the exact plane crossing (DIST_EPSILON)");
        CHECK(frac > 0.45f, "but not backed off excessively (still close to the true crossing)");
        float ex = unbits(ht.end.x);
        CHECK(ex < 200.0f, "stop point end.x < 200 (did not cross into the wall)");
        CHECK(NEAR(unbits(ht.normal.x), -1.0f, 0.01f) && NEAR(unbits(ht.normal.y), 0.0f, 0.01f),
              "impact normal ~= (-1,0,0): points back into open space");
    }

    printf("\n-- 3. stand on the floor: walk straight down through z=0, must STOP, normal +Z --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 0,0,50, 0,0,-50, &rc);
        CHECK(rc == 0, "rc==0");
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "0 < frac < 1 (floor blocks the fall)");
        CHECK(frac < 0.5f, "backed off before the exact z=0 crossing");
        float ez = unbits(ht.end.z);
        CHECK(ez > 0.0f, "stop point end.z > 0 (did not sink through the floor)");
        CHECK(NEAR(unbits(ht.normal.z), 1.0f, 0.01f), "impact normal ~= (0,0,1): floor normal points up");
    }

    printf("\n-- 4. ceiling: walk straight up through z=200, must STOP, normal -Z --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 0,0,150, 0,0,250, &rc);
        CHECK(rc == 0, "rc==0");
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "0 < frac < 1 (ceiling blocks upward motion)");
        float ez = unbits(ht.end.z);
        CHECK(ez < 200.0f, "stop point end.z < 200 (did not poke through the ceiling)");
        CHECK(NEAR(unbits(ht.normal.z), -1.0f, 0.01f), "impact normal ~= (0,0,-1): ceiling normal points down");
    }

    printf("\n-- 5. start already embedded (both endpoints outside the room): all_solid+start_solid --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 0,0,-50, 0,0,-100, &rc);
        CHECK(rc == 0, "rc==0");
        CHECK(ht.start_solid == 1, "start_solid == 1");
        CHECK(ht.all_solid == 1, "all_solid == 1 (never found empty space)");
        CHECK(NEAR(unbits(ht.frac), 0.0f, 0.001f), "frac forced to 0.0 (never an undefined result)");
    }

    printf("\n-- 6. diagonal corner approach (X and Y walls near-simultaneously) --\n");
    {
        int32_t rc; HullTrace ht = trace(sc, 150,150,100, 250,250,100, &rc);
        CHECK(rc == 0, "rc==0");
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "0 < frac < 1 (blocked by one of the two walls)");
        CHECK(!isnan(frac) && !isinf(frac), "frac is a finite number (no NaN/Inf from the corner case)");
    }

    printf("\n-- 7. bad hull index / no scene: must fail closed, not crash --\n");
    {
        HullTrace ht; memset(&ht, 0, sizeof(ht));
        int32_t rc = bsp_hull_trace(sc, 9, bits(0),bits(0),bits(0), bits(1),bits(1),bits(1), &ht);
        CHECK(rc < 0, "out-of-range hull index rejected (rc<0), no crash");
        rc = bsp_hull_trace(NULL, 1, bits(0),bits(0),bits(0), bits(1),bits(1),bits(1), &ht);
        CHECK(rc < 0, "null scene rejected (rc<0), no crash");
    }

    bsp_free(sc);
    free(buf);

    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail;
}
