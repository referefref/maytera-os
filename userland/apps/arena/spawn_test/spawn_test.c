/* spawn_test.c - #501: assert that EVERY spawn of EVERY built-in box level is a
 * place a player can actually stand.
 *
 * THE BUG THIS PINS DOWN
 * ----------------------
 * The Longest Yard (levels.c load_longest_yard, level 0 = THE DEFAULT LEVEL)
 * authored 8 spawns in a ring. A later level-design pass built the low cover
 * blocks and the north sniper platform directly on top of 4 of them. A player
 * drawing one of those 4 spawned embedded in solid, fell THROUGH the map, and
 * came to rest at pz=-64 - which is not a surface, it is the world-floor clamp
 * (physics.c clamps to level.world_mins.z, and build_shell sets that to
 * floorz-64). pick_spawn() is RANDOM, so it was a coin flip per life. THAT is
 * why it survived review: a single green run is not a pass when the outcome is
 * random. This harness removes the randomness by ENUMERATING all of them.
 *
 * METHOD: this compiles the REAL ../levels.c and the REAL ../world.c for the
 * host and calls the REAL map_spawn_valid(). It does NOT copy or re-implement
 * either (the sibling collision_test/ keeps snapshot copies because it needs an
 * old-vs-new A/B; this test needs no A/B, and a copy would rot). So a level
 * edit or a collision edit is felt here immediately, which is the entire point:
 * if you bury a spawn, this test fails.
 *
 * HONEST SCOPE: bsp_is_active() is stubbed to 0. That is FAITHFUL, not a dodge:
 * the six built-in levels are box-brush worlds and bsp_is_active() really is 0
 * for them at runtime (a BSP world has nbrush==0). The BSP/hull side of
 * map_spawn_valid is covered by the existing hull_test/dust2_test.c, which
 * asserts the same two properties (0/40 solid, 0/40 bottomless) against
 * de_dust2's real hull. Between the two, both collider branches are asserted.
 *
 * Build/run: ./run_spawn_test.sh
 */
/* NOTE: deliberately NO host <stdio.h>/<string.h>. game.h pulls in the MayteraOS
 * libc headers, which collide with glibc's (off_t, FILE, BUFSIZ...). The sibling
 * collision_test/ dodged that by keeping private COPIES of game.h + mathx.h;
 * copies rot, and a spawn test that validates a stale copy of the level contract
 * is worthless. So: include the REAL game.h only, take printf/memset from its
 * libc declarations, and let the linker bind them to glibc. The signatures are
 * compatible, which is why the real ../world.c and ../levels.c also build here
 * unmodified.                                                                 */
#include "game.h"
#include "bsp_load.h"

/* ------------------------------------------------------------------ stubs -- */
/* Box levels only: the BSP collider is genuinely inactive for them (see the
 * HONEST SCOPE note above). If this ever returns 1, the test is lying.        */
int bsp_is_active(void) { return 0; }
int bsp_hull_point_solid(vec3 feet) { (void)feet; return 0; }
Trace bsp_hull_sweep(vec3 a, vec3 b) {
    Trace t; t.frac = 1.0f; t.hit = 0; t.end = b; t.normal = v3(0,0,0);
    t.hit_entity = -1; (void)a; return t;
}
Entity *ent_get(World *w, int idx) {
    if (!w || idx < 0 || idx >= MAX_ENTITIES) return 0;
    if (!w->ents[idx].alive) return 0;
    return &w->ents[idx];
}

/* ------------------------------------------------------------------ harness */
static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); g_fail = 1; } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

static World g_w;

static void load_level(int idx) {
    Level lv;
    memset(&lv, 0, sizeof(lv));
    level_load(idx, &lv);
    memset(&g_w, 0, sizeof(g_w));
    map_set_level(&g_w, &lv);
}

/* Why a spawn failed, for the report. */
static const char *why(vec3 sp) {
    vec3 feet = sp; feet.z += SPAWN_FEET_NUDGE;
    if (map_box_solid(&g_w, feet, PLAYER_RADIUS, PLAYER_HEIGHT)) return "SOLID (buried in a brush)";
    float d = map_box_drop_dist(&g_w, feet, PLAYER_RADIUS, PLAYER_HEIGHT, SPAWN_MAX_DROP);
    if (d >= SPAWN_MAX_DROP) return "BOTTOMLESS (no floor within 256u)";
    return "ok";
}

static int spawn_eq(vec3 a, float x, float y, float z) {
    return mx_absf(a.x - x) < 0.5f && mx_absf(a.y - y) < 0.5f && mx_absf(a.z - z) < 0.5f;
}

int main(void) {
    printf("=== Arena #501: spawn validity over every built-in level ===\n");

    /* ------------------------------------------------------------------- 1 */
    printf("\n-- 1. NON-VACUITY: the checker must DISCRIMINATE, not answer a constant --\n");
    printf("   (a checker that says 'all valid' or 'all invalid' proves nothing;\n");
    printf("    an earlier agent nearly shipped a false PASS on exactly this.)\n");
    load_level(0);
    {
        /* Known-GOOD: an authored corner spawn, on open floor. Must PASS.     */
        CHECK(map_spawn_valid(&g_w, v3(-400,-400,0)) == 1,
              "corner spawn (-400,-400,0) on open floor is VALID  [checker can say yes]");
        /* Known-BAD: dead centre of the tier-2 pyramid (levels.c builds
         * -112,-112,48 .. 112,112,96). Must FAIL as solid.                    */
        CHECK(map_spawn_valid(&g_w, v3(0,0,60)) == 0,
              "a point inside the tier-2 pyramid is INVALID       [checker can say no]");
        /* Known-BAD (the OTHER criterion): mid-air over open floor, >256u up.
         * Must FAIL as bottomless, NOT as solid, or the two arms are not
         * independently proven. (-300,-100) is a clear column: the pyramid
         * stops at x=-192, the west pillar at y=-308, and feet at 270 keep the
         * box's head at 326, below the z=336 ceiling. Getting this point wrong
         * is easy - the first attempt used z=300, where 300+PLAYER_HEIGHT
         * punches into the ceiling brush and the point reads SOLID, which would
         * have "passed" the bottomless check for entirely the wrong reason.   */
        CHECK(map_spawn_valid(&g_w, v3(-300,-100,268)) == 0,
              "a point 268u up over open floor is INVALID         [bottomless arm fires]");
        CHECK(map_box_solid(&g_w, v3(-300,-100,270), PLAYER_RADIUS, PLAYER_HEIGHT) == 0,
              "  ...and it is rejected for NO FLOOR, not for being solid (the two arms are distinct)");
        CHECK(map_box_drop_dist(&g_w, v3(-300,-100,270), PLAYER_RADIUS, PLAYER_HEIGHT,
                                SPAWN_MAX_DROP) >= SPAWN_MAX_DROP,
              "  ...map_box_drop_dist finds no floor within %.0fu of it", (double)SPAWN_MAX_DROP);
        /* ...and the SAME column low down IS valid, proving the rejection above
         * is about the height, not about that (x,y) being a bad place.        */
        CHECK(map_spawn_valid(&g_w, v3(-300,-100,0)) == 1,
              "  ...while the same column at floor level IS valid (height, not location)");
        /* The floor a spawn rests on must NOT count as burying it.            */
        CHECK(map_box_solid(&g_w, v3(-400,-400,SPAWN_FEET_NUDGE), PLAYER_RADIUS, PLAYER_HEIGHT) == 0,
              "the floor brush under a spawn does NOT read as solid (touching faces excluded)");
    }

    /* ------------------------------------------------------------------- 2 */
    printf("\n-- 2. WHY map_point_solid() WAS NOT THE PRIMITIVE TO USE --\n");
    printf("   The obvious one-liner, map_point_solid(spawn.pos), is a VACUOUS\n");
    printf("   checker: spawns sit at the floor brush's top-face Z and that test\n");
    printf("   is inclusive of the face. Demonstrated, not asserted:\n");
    {
        int pt_solid = 0;
        for (int i = 0; i < g_w.level.nspawn; i++)
            if (map_point_solid(&g_w, g_w.level.spawns[i].pos)) pt_solid++;
        printf("     map_point_solid() says %d of %d Longest Yard spawns are solid\n",
               pt_solid, g_w.level.nspawn);
        CHECK(pt_solid == g_w.level.nspawn,
              "map_point_solid() reports ALL %d spawns solid = discriminates nothing "
              "(this is the trap map_box_solid exists to avoid)", g_w.level.nspawn);
    }

    /* ------------------------------------------------------------------- 3 */
    printf("\n-- 3. THE BUG: The Longest Yard (level 0, THE DEFAULT), all 8 spawns --\n");
    load_level(0);
    {
        CHECK(g_w.level.nspawn == 8, "level 0 has 8 spawns (%d)", g_w.level.nspawn);
        int nbad = 0, ngood = 0;
        for (int i = 0; i < g_w.level.nspawn; i++) {
            vec3 p = g_w.level.spawns[i].pos;
            int ok = map_spawn_valid(&g_w, p);
            printf("     SP%d (%6.0f,%6.0f,%4.0f)  %s   %s\n",
                   i, p.x, p.y, p.z, ok ? "VALID  " : "INVALID", ok ? "" : why(p));
            if (ok) ngood++; else nbad++;
        }
        CHECK(nbad == 4, "exactly 4 of 8 spawns are INVALID (got %d) - matches the runtime-confirmed report", nbad);
        CHECK(ngood == 4, "exactly 4 of 8 spawns remain VALID (got %d) - the fix does not empty the table", ngood);

        /* Name them. A count alone could be right for the wrong reason.       */
        struct { float x, y, z; const char *what; } bad[] = {
            {   0,-420, 0, "south cover block  x-56..56  y-460..-404 z0..56"  },
            {   0, 420, 0, "north sniper platform x-48..48 y260..508 z0..112" },
            {-420,   0, 0, "west cover base    x-460..-404 y-56..56 z0..56"   },
            { 420,   0, 0, "east cover base    x404..460   y-56..56 z0..56"   },
        };
        for (unsigned b = 0; b < sizeof(bad)/sizeof(bad[0]); b++) {
            int found = -1;
            for (int i = 0; i < g_w.level.nspawn; i++)
                if (spawn_eq(g_w.level.spawns[i].pos, bad[b].x, bad[b].y, bad[b].z)) found = i;
            CHECK(found >= 0 && map_spawn_valid(&g_w, v3(bad[b].x,bad[b].y,bad[b].z)) == 0,
                  "(%.0f,%.0f,%.0f) is rejected - buried in the %s", bad[b].x, bad[b].y, bad[b].z, bad[b].what);
        }
        /* The four corners must survive: this is the control that stops a
         * "reject everything" checker from passing this section.              */
        float cx[4] = {-400, 400, -400, 400}, cy[4] = {-400, -400, 400, 400};
        for (int c = 0; c < 4; c++)
            CHECK(map_spawn_valid(&g_w, v3(cx[c], cy[c], 0)) == 1,
                  "corner spawn (%.0f,%.0f,0) is still selectable", cx[c], cy[c]);

        /* THE USER-FACING SYMPTOM: the buried spawns are exactly the ones that
         * would drop the player to the world-floor clamp. Show the clamp value
         * so the -64 in the runtime log is traceable to this level's geometry. */
        printf("     world_mins.z = %.0f  <- physics.c clamps a falling player here;\n",
               g_w.level.world_mins.z);
        printf("     that is the pz=-64 seen at runtime, i.e. NOT a surface.\n");
        CHECK(g_w.level.world_mins.z == -64.0f,
              "world_mins.z == -64 (the clamp the buried spawns fell to)");
    }

    /* ------------------------------------------------------------------- 4 */
    printf("\n-- 4. EVERY built-in level: settling the dueling_keep / spire lead --\n");
    printf("   (reported as 'also look affected' by a REGEX over levels.c that\n");
    printf("    mis-parses variable-built brushes. This is the real collider, so\n");
    printf("    it settles the question properly - lead, not result.)\n");
    {
        for (int L = 0; L < NUM_LEVELS; L++) {
            load_level(L);
            int nv = 0;
            for (int i = 0; i < g_w.level.nspawn; i++)
                if (map_spawn_valid(&g_w, g_w.level.spawns[i].pos)) nv++;
            printf("\n     level %d '%s': %d/%d spawns valid, %d brushes\n",
                   L, level_name(L), nv, g_w.level.nspawn, g_w.level.nbrush);
            for (int i = 0; i < g_w.level.nspawn; i++) {
                vec3 p = g_w.level.spawns[i].pos;
                if (!map_spawn_valid(&g_w, p))
                    printf("        SP%d (%6.0f,%6.0f,%4.0f) INVALID  %s\n", i, p.x, p.y, p.z, why(p));
            }
            /* The brush count guards against the regex tool's failure mode:
             * "0 brushes parsed" read as "level is clean". Here 0 brushes would
             * mean the level did not build, and we would say so.              */
            CHECK(g_w.level.nbrush > 0, "level %d actually built geometry (%d brushes) - not a silent no-parse",
                  L, g_w.level.nbrush);
            CHECK(g_w.level.nspawn > 0, "level %d has spawns (%d)", L, g_w.level.nspawn);
            /* THE SHIPPING GUARANTEE: at least one spawn must survive, or
             * pick_spawn's loud fallback would fire in the user's face.       */
            CHECK(nv > 0, "level %d has at least one VALID spawn (%d) - pick_spawn's fallback never fires",
                  L, nv);
        }
    }

    /* ------------------------------------------------------------------- 5 */
    printf("\n-- 5. THE ALL-SPAWNS-BURIED FALLBACK (modes.c spawn_rescue) --\n");
    printf("   HONEST SCOPE: spawn_rescue() itself is static in modes.c and is not\n");
    printf("   linked here (it would drag in ent_alloc/bot_spawn/weapons/sound).\n");
    printf("   It also CANNOT fire on any shipping level - section 4 proves every\n");
    printf("   level keeps >=1 valid spawn - so it has never executed at runtime.\n");
    printf("   What IS proven here is the MECHANISM it is built from, on the real\n");
    printf("   buried spawns: walk up the buried spawn's own column and the first\n");
    printf("   height the player box fits must be standable (the cover's roof).\n");
    printf("   THIS SECTION ALREADY EARNED ITS KEEP: it caught the FIRST version of\n");
    printf("   spawn_rescue() returning 0 every time (it started the probe inside\n");
    printf("   the ceiling brush, because world_maxs.z is the world BOUND at\n");
    printf("   ceilz+64, not the ceiling). That bug was untriggerable in play.\n");
    load_level(0);
    {
        float bx[4] = {   0,    0, -420,  420 };
        float by[4] = {-420,  420,    0,    0 };
        /* The exact top face the rescue must land on, read off levels.c:
         *   (0,-420) south cover      z0..56                       -> 56
         *   (0, 420) north platform   z0..112                      -> 112
         *   (-420,0) west base z0..56 PLUS stacked "top2" z56..104 -> 104
         *   ( 420,0) east base z0..56 PLUS stacked "top2" z56..104 -> 104
         * The west/east 104s are not a typo: those two covers are two tiers, and
         * the player box (radius 16) at x=-420 still overlaps the upper tier
         * (x-444..-420), so the first height it fits is that tier's roof. My
         * first expectation here said 56 and the harness rejected it - the test
         * was right and I was wrong, which is the point of writing the expected
         * value down instead of printing whatever came out.                   */
        float expect_roof[4] = { 56, 112, 104, 104 };
        for (int i = 0; i < 4; i++) {
            /* Mirrors spawn_rescue()'s loop using the SAME primitive it calls.  */
            float landed = -1.0f;
            for (float z = 0.0f; z < g_w.level.world_maxs.z; z += 8.0f) {
                if (!map_spawn_valid(&g_w, v3(bx[i], by[i], z))) continue;
                landed = z; break;
            }
            CHECK(landed >= 0.0f && mx_absf(landed - expect_roof[i]) < 0.01f,
                  "buried spawn (%.0f,%.0f,0) rescues to z=%.0f, standing on the "
                  "cover roof at %.0f (not inside it, not the -64 clamp)",
                  bx[i], by[i], landed, expect_roof[i]);
        }
    }

    printf("\n=== %s (%d checks) ===\n", g_fail ? "FAIL" : "ALL PASS", g_checks);
    return g_fail;
}
