/* dust2_test.c - #491 Stage 2 REAL-MAP known-answer boundary test.
 *
 * hull_test.c proves the clipnode-walk ALGORITHM against a synthetic room whose
 * geometry we authored. That is necessary but NOT sufficient: it cannot catch a
 * wrong lump offset, a bad headnode pick, or a plane/clipnode layout mistake
 * that only real compiler output exercises. This test runs the SAME Rust trace
 * against the REAL de_dust2 (CS 1.6 GoldSrc v30 compile, md5
 * 74d6d81b4b818b691fafd2e56a5ae362, 2057288 bytes) and asserts against GROUND
 * TRUTH THAT COMES FROM THE MAP ITSELF, not from anything we invented:
 *
 *   the map's own info_player_* entity origin. Counter-Strike places real
 *   players at that exact origin with the exact hull-1 box we trace, so a
 *   correct hull MUST report it as open space standing on a floor. If our
 *   clipnode walk, plane parse, or headnode pick were wrong, a legitimate CS
 *   spawn would almost certainly come back solid or bottomless.
 *
 * NON-VACUITY: the "user cannot fly" assertions (floor/wall/sealed) all require
 * frac<1, while the open-space assertion requires frac==1. A trace stubbed to
 * "never hit" (the exact Stage-1 bug the user reported) fails the former; one
 * stubbed to "always hit" fails the latter. Neither degenerate implementation
 * can pass this file. See run_dust2_test.sh's NEGATIVE CONTROL, which proves
 * that claim by actually building and running the never-hit stub.
 *
 * All coordinates are hull-ORIGIN space (what bsp_hull_trace consumes), which
 * is the space the map's entity origins are already expressed in.
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
#define CHECK(cond, ...) do { \
    if (cond) { printf("  PASS: "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } \
} while (0)

static BspScene *g_sc;
static HullTrace tr(float ax, float ay, float az, float bx, float by, float bz) {
    HullTrace ht; memset(&ht, 0, sizeof(ht));
    int32_t rc = bsp_hull_trace(g_sc, 1, bits(ax), bits(ay), bits(az),
                                bits(bx), bits(by), bits(bz), &ht);
    if (rc != 0) { printf("  FAIL: bsp_hull_trace rc=%d\n", rc); g_fail = 1; }
    return ht;
}

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f); *out_len = n; return buf;
}

/* ---- the map's OWN spawn entities = our ground truth -------------------------
 * NOTE: BspScene.spawn/has_spawn are VESTIGIAL Rust-side fields that bsp.rs
 * never fills (see its "the C side parses info_player_* spawn origin" comment);
 * the shipping engine parses the spawn in C (bsp_load.c parse_spawn -> g_spawn).
 * So this test does what the engine does: read the verbatim entity text. In
 * GoldSrc an info_player_* "origin" IS the player's hull origin, which is
 * exactly the space bsp_hull_trace consumes.                                  */
typedef struct { float x, y, z; } Spawn;
#define MAX_SPAWNS 64
static Spawn g_spawns[MAX_SPAWNS];
static int   g_nspawns;

static int collect_spawns(const char *ents, size_t len) {
    g_nspawns = 0;
    for (size_t i = 0; i + 1 < len && g_nspawns < MAX_SPAWNS; i++) {
        if (ents[i] != '{') continue;
        size_t j = i;
        while (j < len && ents[j] != '}') j++;
        if (j >= len) break;
        size_t blen = j - i;
        char blk[4096];
        size_t n = blen < sizeof(blk) - 1 ? blen : sizeof(blk) - 1;
        memcpy(blk, ents + i, n); blk[n] = 0;
        if (strstr(blk, "\"info_player_start\"") || strstr(blk, "\"info_player_deathmatch\"")) {
            char *o = strstr(blk, "\"origin\"");
            if (o) {
                o = strchr(o + 8, '"');
                if (o) {
                    float x, y, z;
                    if (sscanf(o + 1, "%f %f %f", &x, &y, &z) == 3) {
                        g_spawns[g_nspawns].x = x; g_spawns[g_nspawns].y = y;
                        g_spawns[g_nspawns].z = z; g_nspawns++;
                    }
                }
            }
        }
        i = j;
    }
    return g_nspawns;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/root/MAP.BSP";
    long len; uint8_t *buf = read_file(path, &len);
    if (!buf) return 1;
    printf("== REAL de_dust2: %s (%ld bytes) ==\n", path, len);

    g_sc = bsp_parse(buf, (unsigned long)len, NULL, 0);
    if (!g_sc) { printf("  FAIL: bsp_parse returned NULL\n"); return 1; }
    CHECK(g_sc->error == 0, "bsp_parse error == 0");
    CHECK(g_sc->hull_ok == 1, "hull_ok == 1: real PLANES+CLIPNODES+MODELS parsed");
    CHECK(g_sc->num_clipnodes > 1000, "num_clipnodes = %u (a real map, not a stub)", g_sc->num_clipnodes);
    CHECK(g_sc->num_planes > 1000, "num_planes = %u", g_sc->num_planes);
    CHECK(g_sc->hull_headnode[1] >= 0 && (uint32_t)g_sc->hull_headnode[1] < g_sc->num_clipnodes,
          "hull_headnode[1] = %d is a valid clipnode index", g_sc->hull_headnode[1]);
    collect_spawns((const char *)g_sc->entities, g_sc->entities_len);
    CHECK(g_nspawns >= 40, "collected %d real info_player_* spawns from the map's entity text", g_nspawns);
    if (g_nspawns == 0) { printf("no spawns, cannot continue\n"); return 1; }

    /* Use spawn 0 as the representative point for the directional tests below. */
    float sx = g_spawns[0].x, sy = g_spawns[0].y, sz = g_spawns[0].z;
    printf("\n-- GROUND TRUTH: de_dust2 spawn[0] origin = (%.1f, %.1f, %.1f) --\n", sx, sy, sz);

    printf("\n-- 1. ALL %d of the map's real spawns must be OPEN SPACE standing on a FLOOR --\n", g_nspawns);
    {
        /* Counter-Strike itself stands a player, with the exact hull-1 box we
         * trace, at every one of these origins. So a correct hull MUST report
         * each as non-solid with ground under it. This is the single strongest
         * real-map check available: 40 independent known-good points, spread
         * across the whole map, that we did not choose or invent. */
        int solid = 0, bottomless = 0;
        for (int i = 0; i < g_nspawns; i++) {
            float x = g_spawns[i].x, y = g_spawns[i].y, z = g_spawns[i].z;
            HullTrace pt = tr(x, y, z, x, y, z);
            if (pt.start_solid) { solid++; printf("     spawn[%d] (%.0f,%.0f,%.0f) SOLID\n", i, x, y, z); continue; }
            HullTrace dn = tr(x, y, z, x, y, z - 4096.0f);
            float drop = 4096.0f * unbits(dn.frac);
            if (unbits(dn.frac) >= 1.0f || drop > 256.0f) {
                bottomless++;
                printf("     spawn[%d] (%.0f,%.0f,%.0f) no floor within 256u (drop=%.1f)\n", i, x, y, z, drop);
            }
        }
        CHECK(solid == 0, "0/%d real CS spawns report SOLID (%d did)", g_nspawns, solid);
        CHECK(bottomless == 0, "0/%d real CS spawns are bottomless (%d were)", g_nspawns, bottomless);
    }

    printf("\n-- 1b. INTEGRATION: the ENGINE's own spawn conversion chain must also land safely --\n");
    {
        /* Model exactly what the shipping engine does to a map spawn origin:
         *   bsp_load.c parse_spawn : g_spawn.z = ent_z + SPAWN_Z_ADJ
         *   main.c arena_start_bsp : p->pos = g_spawn (Arena pos = FEET); pos.z += 2
         *   bsp_load.c feet_to_origin: traced hull origin = feet.z + 36
         * so traced origin z = ent_z + SPAWN_Z_ADJ + 2 + 36.
         * GoldSrc ground truth: feet = origin - 36, i.e. SPAWN_Z_ADJ must be -36
         * for the traced origin to come back to the map's real origin (+2 nudge).
         * Pass -DSPAWN_Z_ADJ=24 to reproduce the old Stage-1 free-fly value. */
#ifndef SPAWN_Z_ADJ
#define SPAWN_Z_ADJ (-36.0f)
#endif
        int bad = 0;
        for (int i = 0; i < g_nspawns; i++) {
            float x = g_spawns[i].x, y = g_spawns[i].y;
            float traced_z = g_spawns[i].z + (SPAWN_Z_ADJ) + 2.0f + 36.0f;
            HullTrace pt = tr(x, y, traced_z, x, y, traced_z);
            if (pt.start_solid) {
                bad++;
                printf("     spawn[%d] engine-placed origin z=%.1f is SOLID (map origin z=%.1f)\n",
                       i, traced_z, g_spawns[i].z);
            }
        }
        CHECK(bad == 0, "engine spawn conversion (SPAWN_Z_ADJ=%.0f) puts 0/%d players inside geometry (%d bad)",
              (double)(SPAWN_Z_ADJ), g_nspawns, bad);
    }

    printf("\n-- 2. NON-VACUITY: a short move in open space at spawn must NOT be blocked --\n");
    {
        HullTrace ht = tr(sx, sy, sz, sx, sy, sz + 8.0f);
        CHECK(fabsf(unbits(ht.frac) - 1.0f) < 0.001f,
              "frac == 1.0 stepping 8u up in open air (collider is not 'everything is solid')");
    }

    printf("\n-- 3. THE FLOOR (the user's bug: 'i can fly through the floor') --\n");
    {
        HullTrace ht = tr(sx, sy, sz, sx, sy, sz - 4096.0f);
        float frac = unbits(ht.frac);
        CHECK(frac > 0.0f && frac < 1.0f, "falling 4096u from spawn is BLOCKED (frac=%.4f)", frac);
        float drop = 4096.0f * frac;
        CHECK(drop < 200.0f, "floor is %.1fu below the spawn (a real spawn sits just above its floor)", drop);
        float nz = unbits(ht.normal.z);
        CHECK(nz > 0.7f, "impact normal.z = %.3f: a walkable floor pointing UP", nz);
        CHECK(unbits(ht.end.z) > sz - 200.0f, "stop point did not sink through the world");
    }

    printf("\n-- 4. THE WALLS (the user's bug: 'i can fly through walls'): de_dust2 is SEALED --\n");
    {
        /* 8 compass directions, 4096 units. de_dust2 is a sealed competitive map:
         * you cannot travel 4096u in ANY horizontal direction from a spawn. If any
         * of these runs to frac==1 we are flying through the map, which is the bug. */
        const float dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{.707f,.707f},{-.707f,.707f},{.707f,-.707f},{-.707f,-.707f}};
        const char *nm[8] = {"+X","-X","+Y","-Y","+X+Y","-X+Y","+X-Y","-X-Y"};
        int blocked = 0;
        for (int i = 0; i < 8; i++) {
            HullTrace ht = tr(sx, sy, sz, sx + dirs[i][0]*4096.0f, sy + dirs[i][1]*4096.0f, sz);
            float frac = unbits(ht.frac);
            int b = (frac < 1.0f);
            blocked += b;
            printf("     %-5s frac=%.4f -> %s\n", nm[i], frac, b ? "BLOCKED by geometry" : "FLEW THROUGH (bug!)");
        }
        CHECK(blocked == 8, "all 8/8 horizontal 4096u runs blocked (%d/8): the map is solid to the player", blocked);
    }

    printf("\n-- 5. leaving the sealed world entirely must be blocked --\n");
    {
        HullTrace ht = tr(sx, sy, sz, sx + 100000.0f, sy + 100000.0f, sz + 100000.0f);
        CHECK(unbits(ht.frac) < 1.0f, "cannot escape the map to (+100000)^3");
    }

    printf("\n-- 6. a point far outside the sealed hull reads SOLID (GoldSrc convention) --\n");
    {
        HullTrace ht = tr(99999.0f, 99999.0f, 99999.0f, 99999.0f, 99999.0f, 99999.0f);
        CHECK(ht.start_solid == 1, "outside the world is solid, so it can never be entered");
    }

    bsp_free(g_sc); free(buf);
    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail;
}
