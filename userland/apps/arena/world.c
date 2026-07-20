/* Maytera Arena - world.c
 * Map geometry, collision, and tracing against axis-aligned box brushes and
 * (optionally) entity bounding boxes. Coordinate system: z is UP. Player AABB
 * is [pos-(r,r,0) .. pos+(r,r,height)] with pos at the FEET center.
 *
 * All routines are cheap and allocation-free: brushes are a flat array copied
 * into the World, and every trace/move is a linear scan over that array. That
 * is plenty for MAX_BRUSHES(256) at 64 entities * 60Hz on a software renderer.
 */
#include "game.h"
/* #491 Stage 2: GoldSrc BSP hull-trace collision. bsp_is_active() gates a
 * SEPARATE collider (bsp_hull_sweep/bsp_hull_point_solid) into the SAME
 * movement path below - map_move_entity/map_point_solid keep exactly one
 * control-flow shape; only the "is this segment/point solid" primitive is
 * swapped, never a second parallel physics path. The six built-in box levels
 * are untouched: bsp_is_active() is 0 for them (nbrush>0, no BSP loaded/
 * selected), so every branch added here is a no-op on the original code path. */
#include "bsp_load.h"

/* ------------------------------------------------------------------ tunables */
#define TRACE_EPS      0.03125f   /* nudge back off a surface (1/32 unit)       */
#define FLOOR_NORMAL_Z 0.7f       /* normal.z above this counts as "floor"      */
#define MOVE_ITERS     4          /* slide/clip passes per move                 */

/* ------------------------------------------------------------------ helpers  */
static inline float fminf_(float a, float b){ return a < b ? a : b; }
static inline float fmaxf_(float a, float b){ return a > b ? a : b; }

/* Ray/segment vs a single AABB using the slab method. `start`->`end` is the
 * segment; the box is [bmins,bmaxs]. On hit within [0,1], writes the hit
 * fraction to *out_frac and the surface normal to *out_n, returns 1.
 * A start point already inside the box is reported as frac 0 with a normal
 * derived from the nearest exit slab (so callers still get a usable plane).
 *
 * `solid_when_embedded` controls what happens in that "already inside" case:
 *   1 - always report a hit at frac 0 (legacy/hitscan behaviour: a ray that
 *       starts inside a solid is blocked immediately, regardless of `dir`).
 *   0 - only report a hit if `dir` continues to drive further INTO the box
 *       along the chosen exit axis (dot(dir, exit_normal) < 0); if `dir` is
 *       heading OUT (or purely tangential), report no hit for this brush.
 * The move/slide sweep (sweep_aabb_vs_brush) needs mode 0: see the "player
 * gets stuck" bug this fixes, below map_move_entity.                        */
static int ray_vs_aabb(vec3 start, vec3 dir, vec3 bmins, vec3 bmaxs,
                       float *out_frac, vec3 *out_n, int solid_when_embedded)
{
    float tmin = 0.0f, tmax = 1.0f;
    int   axis_min = -1;       /* which axis produced tmin                     */
    float sign_min = 0.0f;
    const float *s = &start.x, *d = &dir.x, *mn = &bmins.x, *mx = &bmaxs.x;

    for (int a = 0; a < 3; a++) {
        if (mx_absf(d[a]) < 1e-8f) {
            /* segment parallel to this slab: must already be inside it        */
            if (s[a] < mn[a] || s[a] > mx[a]) return 0;
            continue;
        }
        float inv = 1.0f / d[a];
        float t1 = (mn[a] - s[a]) * inv;
        float t2 = (mx[a] - s[a]) * inv;
        float sgn = -1.0f;             /* entering through the min face        */
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; sgn = 1.0f; }
        if (t1 > tmin) { tmin = t1; axis_min = a; sign_min = sgn; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;     /* slabs disjoint -> miss               */
    }

    vec3 n = v3(0,0,0);
    if (axis_min >= 0) {
        (&n.x)[axis_min] = sign_min;
    } else {
        /* started inside all slabs: pick the axis of nearest exit as normal   */
        float best = 1e30f; int ba = 0; float bs = 1.0f;
        for (int a = 0; a < 3; a++) {
            float dlo = s[a] - mn[a], dhi = mx[a] - s[a];
            if (dlo < best) { best = dlo; ba = a; bs = -1.0f; }
            if (dhi < best) { best = dhi; ba = a; bs =  1.0f; }
        }
        (&n.x)[ba] = bs;
        tmin = 0.0f;

        /* THE STUCK-PLAYER FIX: a start point already embedded in the box used
         * to ALWAYS report a hit here, no matter which way `dir` pointed. For
         * the move/slide sweep that means: once a player is (even marginally)
         * embedded in a brush's Minkowski-expanded collision box - which the
         * levels' own "walls overlap at the corners" construction (levels.c)
         * makes easy near any inside corner - EVERY subsequent move attempt,
         * in ANY direction including straight away from the wall, hit this
         * branch, got frac=0 (no position advance) and had its velocity
         * clipped on an arbitrary nearest-exit axis. The player was frozen:
         * not just blocked from walking further in, but unable to back away
         * or slide either, since the check never looked at `dir` at all.
         * Fix: when the caller opts out of the legacy always-solid behaviour
         * (solid_when_embedded==0), only treat this as a blocking hit if `dir`
         * is still driving deeper along the chosen exit axis; movement that
         * heads out (or tangentially) is let through so the player can escape
         * the embedding, and the MTV pushout pass below cleans up the rest.  */
        if (!solid_when_embedded) {
            float ddotn = d[0]*n.x + d[1]*n.y + d[2]*n.z;
            if (ddotn >= -1e-5f) return 0;   /* exiting or tangential: no block */
        }
    }
    *out_frac = tmin;
    *out_n = n;
    return 1;
}

/* Swept player-AABB (half-extents hx,hy in xy; 0..height in z, feet at pos)
 * against a solid brush. We reduce the moving box vs static box to a ray vs
 * the Minkowski-expanded box: expand the brush by the player's half extents
 * and trace the feet point (with the z offsets folded in) through it.         */
static int sweep_aabb_vs_brush(vec3 feet, vec3 dir, float hx, float hy,
                               float height, const Brush *b,
                               float *out_frac, vec3 *out_n)
{
    vec3 emins = v3(b->mins.x - hx, b->mins.y - hy, b->mins.z - height);
    vec3 emaxs = v3(b->maxs.x + hx, b->maxs.y + hy, b->maxs.z);
    /* solid_when_embedded=0: see the stuck-player fix in ray_vs_aabb - a move
     * sweep must let an embedded entity move OUT, not just further in.        */
    return ray_vs_aabb(feet, dir, emins, emaxs, out_frac, out_n, 0);
}

/* True if the player AABB at feet-position `p` overlaps brush `b`.            */
static int aabb_overlaps_brush(vec3 p, float hx, float hy, float height,
                               const Brush *b)
{
    if (p.x + hx <= b->mins.x || p.x - hx >= b->maxs.x) return 0;
    if (p.y + hy <= b->mins.y || p.y - hy >= b->maxs.y) return 0;
    if (p.z + height <= b->mins.z || p.z >= b->maxs.z)  return 0;
    return 1;
}

/* ------------------------------------------------------------- public: level */
void map_set_level(World *w, const Level *lvl)
{
    if (!w || !lvl) return;
    /* Level is a plain-old-data value; a straight copy resets all accel state
     * (there is no separate spatial index to rebuild - the brush array IS it). */
    memcpy(&w->level, lvl, sizeof(Level));
    if (w->level.nbrush < 0) w->level.nbrush = 0;
    if (w->level.nbrush > MAX_BRUSHES) w->level.nbrush = MAX_BRUSHES;
}

/* --------------------------------------------------------- public: point solid */
int map_point_solid(World *w, vec3 p)
{
    if (!w) return 0;
    /* #491 Stage 2: a BSP map has nbrush==0 (arena_start_bsp), so the brush
     * loop below is naturally a no-op for it; dispatch to the hull instead. */
    if (bsp_is_active())
        return bsp_hull_point_solid(p);
    const Level *lv = &w->level;
    int n = lv->nbrush;
    for (int i = 0; i < n; i++) {
        const Brush *b = &lv->brushes[i];
        if (p.x >= b->mins.x && p.x <= b->maxs.x &&
            p.y >= b->mins.y && p.y <= b->maxs.y &&
            p.z >= b->mins.z && p.z <= b->maxs.z)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- public: ray */
Trace map_trace_ray(World *w, vec3 start, vec3 end, int ignore_ent, int hit_ents)
{
    Trace tr;
    tr.frac = 1.0f;
    tr.hit  = 0;
    tr.end  = end;
    tr.normal = v3(0,0,0);
    tr.hit_entity = -1;
    if (!w) return tr;

    vec3 dir = v3sub(end, start);
    const Level *lv = &w->level;

    /* #491 Stage 2: hitscan/LOS against the active BSP map's world geometry.
     * This is an APPROXIMATION, disclosed rather than silently wrong: it
     * reuses the hull-1 (player-box) sweep rather than a true point/hull-0
     * trace against the render nodes (which this task did not implement -
     * see ARENA_BSP_PLAN.md Stage 2 scope), so a shot can occasionally clip
     * a corner a true point trace would have grazed past. It is still far
     * better than the Stage-1 status quo (bullets and bot line-of-sight
     * passed through ALL BSP geometry, since map_trace_ray never looked at
     * it). No effect on the six box levels (bsp_is_active()==0 there).       */
    if (bsp_is_active()) {
        Trace bt = bsp_hull_sweep(start, end);
        if (bt.hit && bt.frac < tr.frac) {
            tr.frac = bt.frac; tr.hit = 1; tr.normal = bt.normal; tr.hit_entity = -1;
        }
    }

    /* brushes */
    int nb = lv->nbrush;
    for (int i = 0; i < nb; i++) {
        float f; vec3 n;
        /* solid_when_embedded=1: unchanged legacy hitscan/trace semantics - a
         * ray starting inside solid geometry is blocked immediately.          */
        if (ray_vs_aabb(start, dir, lv->brushes[i].mins, lv->brushes[i].maxs, &f, &n, 1)) {
            if (f < tr.frac) {
                tr.frac = f; tr.hit = 1; tr.normal = n; tr.hit_entity = -1;
            }
        }
    }

    /* entities (players / bots / items) as bounding boxes */
    if (hit_ents) {
        for (int i = 0; i < MAX_ENTITIES; i++) {
            if (i == ignore_ent) continue;
            Entity *e = ent_get(w, i);
            if (!e) continue;
            if (e->type != ET_PLAYER && e->type != ET_BOT &&
                e->type != ET_ITEM   && e->type != ET_PROP)
                continue;
            /* player-ish bbox: feet at pos, radius 16, height 56.
             * items use a small cube around pos; barrels a stout can.         */
            float hx, hy, zlo, zhi;
            if (e->type == ET_ITEM) {
                hx = hy = 16.0f; zlo = e->pos.z - 16.0f; zhi = e->pos.z + 16.0f;
            } else if (e->type == ET_PROP) {
                hx = hy = 20.0f; zlo = e->pos.z;         zhi = e->pos.z + 44.0f;
            } else {
                hx = hy = 16.0f; zlo = e->pos.z;         zhi = e->pos.z + 56.0f;
            }
            vec3 bmins = v3(e->pos.x - hx, e->pos.y - hy, zlo);
            vec3 bmaxs = v3(e->pos.x + hx, e->pos.y + hy, zhi);
            float f; vec3 n;
            if (ray_vs_aabb(start, dir, bmins, bmaxs, &f, &n, 1)) {
                if (f < tr.frac) {
                    tr.frac = f; tr.hit = 1; tr.normal = n; tr.hit_entity = i;
                }
            }
        }
    }

    tr.end = v3add(start, v3scale(dir, tr.frac));
    return tr;
}

/* #491 Stage 2: the ONE swept-collider primitive the move loop below uses,
 * regardless of world type ("reuse primitives, never reinvent" - this is a
 * swappable COLLIDER behind one movement path, not a second parallel physics
 * path). BSP maps have lv->nbrush==0 (arena_start_bsp), so the box-brush
 * branch is naturally inert for them and vice versa: exactly one of the two
 * branches ever does real work for a given World. Returns 1 and fills
 * *out_frac/*out_n if `vel` (swept from `pos`) hits something before
 * completing; 0 if the segment is fully clear. */
static int sweep_vs_world(World *w, vec3 pos, vec3 vel, float hx, float hy,
                          float height, float *out_frac, vec3 *out_n)
{
    if (bsp_is_active()) {
        Trace tr = bsp_hull_sweep(pos, v3add(pos, vel));
        if (!tr.hit) return 0;
        *out_frac = tr.frac;
        *out_n = tr.normal;
        return 1;
    }
    const Level *lv = &w->level;
    int nb = lv->nbrush;
    float best_frac = 1.0f; vec3 best_n = v3(0, 0, 0); int hit = 0;
    for (int i = 0; i < nb; i++) {
        float f; vec3 n;
        if (sweep_aabb_vs_brush(pos, vel, hx, hy, height, &lv->brushes[i], &f, &n)) {
            if (f < best_frac) { best_frac = f; best_n = n; hit = 1; }
        }
    }
    if (!hit) return 0;
    *out_frac = best_frac; *out_n = best_n;
    return 1;
}

/* ---------------------------------------------------------- public: box solid */
/* True if the player-sized AABB standing with its FEET at `feet` overlaps world
 * solid. This is map_point_solid's box-shaped sibling and shares its dispatch:
 * exactly one collider ever answers (see sweep_vs_world above).
 *
 * WHY A POINT TEST IS NOT GOOD ENOUGH - spelled out because getting this wrong
 * silently produces a checker that discriminates NOTHING, which is worse than
 * no checker at all (#501): map_point_solid(spawn.pos) probes a single point at
 * FEET height. Every box level's spawn sits at exactly the floor brush's top-
 * face Z (levels.c build_shell puts the floor at floorz-24..floorz, spawns at
 * floorz). map_point_solid's test is INCLUSIVE of that face (p.z <= b->maxs.z),
 * so a point test at feet height reports EVERY spawn on EVERY box level as
 * solid: "all invalid", a vacuous pass. aabb_overlaps_brush is EXCLUSIVE of
 * touching faces (p.z >= b->maxs.z returns 0), so the floor a spawn rests on
 * correctly does not count as burying it, while a cover block that genuinely
 * encloses the box does. The player is a box; ask about the box.
 *
 * BSP: hull 1 is already dilated by the player's box, so a point query at the
 * hull origin IS the box query, and radius/height are legitimately unused here.
 * That is the GoldSrc convention bsp_hull_point_solid() implements, not an
 * approximation being papered over.                                          */
int map_box_solid(World *w, vec3 feet, float radius, float height)
{
    if (!w) return 0;
    if (bsp_is_active()) return bsp_hull_point_solid(feet);
    const Level *lv = &w->level;
    int n = lv->nbrush;
    for (int i = 0; i < n; i++)
        if (aabb_overlaps_brush(feet, radius, radius, height, &lv->brushes[i]))
            return 1;
    return 0;
}

/* How far the player-sized box at `feet` falls before it lands on something,
 * capped at `maxdist`. Returns `maxdist` exactly when nothing is under it
 * within the cap, i.e. "no floor here".
 *
 * Reuses sweep_vs_world - the SAME swept collider map_move_entity uses to stand
 * the player up - so "is there floor here" is answered by the code that actually
 * does the standing, not by a second and subtly different opinion.           */
float map_box_drop_dist(World *w, vec3 feet, float radius, float height, float maxdist)
{
    if (!w || maxdist <= 0.0f) return 0.0f;
    float frac = 1.0f;
    vec3  n = v3(0, 0, 0);
    if (!sweep_vs_world(w, feet, v3(0, 0, -maxdist), radius, radius, height, &frac, &n))
        return maxdist;                       /* clear all the way down       */
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return maxdist * frac;
}

/* ------------------------------------------------------- public: spawn valid */
/* #501: can a player actually STAND at this spawn point? Valid iff the player
 * box, AT THE POSITION spawn_player_entity() will really place it (feet nudged
 * up by SPAWN_FEET_NUDGE), is clear of solid AND has floor within SPAWN_MAX_DROP.
 *
 * THE BUG THIS EXISTS FOR: The Longest Yard (levels.c level 0 = the DEFAULT
 * level) shipped 8 spawns in a ring, and a LATER level-design pass built the low
 * cover blocks and the north sniper platform directly on top of 4 of them. A
 * player drawing one of those 4 started embedded in solid, fell straight through
 * the map, and came to rest at the world-floor clamp (physics.c clamps to
 * level.world_mins.z, which build_shell sets to floorz-64: pz=-64 is NOT a
 * surface, it is the clamp). pick_spawn is RANDOM, so this was a coin flip per
 * life, which is exactly why it survived review: a single green run is not a
 * pass when the outcome is random.
 *
 * THE EXACT MECHANISM, since "spawned in solid" does not by itself explain
 * falling THROUGH a sealed floor (measured, see spawn_test/): map_move_entity's
 * safety-net pushout below ejects an embedded box along its SMALLEST-penetration
 * axis. For spawn (0,420,0) inside the north platform (z0..112), the box at feet
 * z=2 needs 104u to leave northward and 64u sideways, but only 58u DOWNWARD, so
 * "cheapest" is straight down: feet land at z=-56, which is UNDER the floor slab
 * (-24..0). The floor then pushes it down again (24u down beats 56u up), and
 * gravity finishes the job at the world_mins.z clamp. That is the runtime-
 * observed pz=-64 exactly. The other three buried spawns have a horizontal
 * cheapest-axis (32u) and get ejected sideways onto open floor instead, which is
 * why (0,420,0) is the one that was caught falling: the geometry decides whether
 * being buried is survivable, so "it looked fine when I tried it" proves nothing.
 *
 * We VALIDATE rather than hand-move the coordinates: relocating spawns is a
 * subjective level-design call that fixes only the four instances we happen to
 * know about, whereas rejecting un-standable spawns fixes the CLASS and catches
 * the next level-design pass that builds over the spawn ring.
 *
 * BOTH criteria are load-bearing and neither is redundant: "not solid" alone
 * would pass a spawn hanging in the air over a pit; "has floor" alone would pass
 * a spawn entombed in rock that happens to have floor beneath the rock.       */
int map_spawn_valid(World *w, vec3 spawn_pos)
{
    if (!w) return 0;
    vec3 feet = spawn_pos;
    feet.z += SPAWN_FEET_NUDGE;
    if (map_box_solid(w, feet, PLAYER_RADIUS, PLAYER_HEIGHT))
        return 0;
    return map_box_drop_dist(w, feet, PLAYER_RADIUS, PLAYER_HEIGHT,
                             SPAWN_MAX_DROP) < SPAWN_MAX_DROP;
}

/* --------------------------------------------------------- public: move+slide */
int map_move_entity(World *w, Entity *ent, vec3 delta, float radius, float height)
{
    if (!w || !ent) return 0;
    const Level *lv = &w->level;
    float hx = radius, hy = radius;
    int   bumped = 0;
    int   grounded = 0;

    vec3  pos = ent->pos;
    vec3  vel = delta;   /* remaining motion this frame                         */

    for (int iter = 0; iter < MOVE_ITERS; iter++) {
        float len2 = v3dot(vel, vel);
        if (len2 < 1e-10f) break;

        /* find the earliest thing (box brush OR the active BSP hull) the
         * swept box hits along `vel` - see sweep_vs_world() above.           */
        float best_frac = 1.0f;
        vec3  best_n = v3(0,0,0);
        int   hit = sweep_vs_world(w, pos, vel, hx, hy, height, &best_frac, &best_n);

        if (!hit) {
            pos = v3add(pos, vel);
            break;
        }

        bumped = 1;

        /* advance to just before the surface (leave a small epsilon gap)      */
        float adv = best_frac;
        float mlen = mx_sqrtf(len2);
        float back = (mlen > 1e-6f) ? (TRACE_EPS / mlen) : 0.0f;
        adv -= back;
        if (adv < 0.0f) adv = 0.0f;
        pos = v3add(pos, v3scale(vel, adv));

        if (best_n.z > FLOOR_NORMAL_Z && vel.z < 0.0f) grounded = 1;

        /* clip remaining velocity: subtract the component into the plane      */
        float into = v3dot(vel, best_n);
        vec3 remain = v3scale(vel, 1.0f - adv);
        vel = v3sub(remain, v3scale(best_n, v3dot(remain, best_n)));

        /* clip the entity's stored velocity on the same plane so we don't keep
         * driving into the wall next frame                                    */
        if (into < 0.0f) {
            ent->vel = v3sub(ent->vel, v3scale(best_n, v3dot(ent->vel, best_n)));
        }
    }

    /* Push out of any brush we ended up embedded in (numerical safety net).
     * 4 passes (was 2): the built-in levels intentionally overlap wall brushes
     * at every corner (levels.c build_shell), so a diagonal approach into a
     * corner routinely overlaps 2 brushes at once; resolving brush A can push
     * the box straight into brush B. 2 passes could leave a residual overlap
     * for next frame, which combined with the sweep's old always-solid
     * "embedded" fallback was the other half of the stuck-player bug (see
     * ray_vs_aabb above). 4 passes matches MOVE_ITERS and reliably fully
     * resolves a 2-brush corner within this single call.                     */
    for (int pass = 0; pass < 4; pass++) {
        int nb = lv->nbrush, moved = 0;
        for (int i = 0; i < nb; i++) {
            const Brush *b = &lv->brushes[i];
            if (!aabb_overlaps_brush(pos, hx, hy, height, b)) continue;
            /* smallest-penetration axis pushout */
            float px1 = (b->maxs.x) - (pos.x - hx);   /* push +x               */
            float px2 = (pos.x + hx) - (b->mins.x);   /* push -x               */
            float py1 = (b->maxs.y) - (pos.y - hy);
            float py2 = (pos.y + hy) - (b->mins.y);
            float pz1 = (b->maxs.z) - (pos.z);        /* push up (feet up)     */
            float pz2 = (pos.z + height) - (b->mins.z);/* push down            */
            float best = 1e30f; int ax = 0; float dv = 0.0f;
            if (px1 < best){ best=px1; ax=0; dv= px1; }
            if (px2 < best){ best=px2; ax=0; dv=-px2; }
            if (py1 < best){ best=py1; ax=1; dv= py1; }
            if (py2 < best){ best=py2; ax=1; dv=-py2; }
            if (pz1 < best){ best=pz1; ax=2; dv= pz1; }
            if (pz2 < best){ best=pz2; ax=2; dv=-pz2; }
            (&pos.x)[ax] += dv + (dv >= 0 ? TRACE_EPS : -TRACE_EPS);
            if (ax == 2 && dv > 0.0f) grounded = 1;   /* pushed up = on floor  */
            moved = 1; bumped = 1;
        }
        if (!moved) break;
    }

    /* #491 Stage 2: BSP embedded-recovery. The box-brush pushout above uses
     * per-axis penetration depth, which the BSP hull trace does not expose
     * (bsp_hull_point_solid only answers solid/not-solid, not "by how much").
     * Approximate the same "never leave this call embedded if a nearby exit
     * exists" guarantee by nudging vertically in TRACE_EPS steps - up first
     * (the common case: an entity resting exactly coplanar with a floor or
     * step, same #481 coplanar-spawn issue the box-brush levels also hit),
     * then down. Bounded at 8 point-solid queries: never a spin (#426), and a
     * failure to fully clear after 8 tries just leaves the position as-is
     * (a rare, cosmetic residual overlap) rather than hanging or crashing.    */
    if (bsp_is_active()) {
        float dir = 1.0f;
        for (int pass = 0; pass < 8 && bsp_hull_point_solid(pos); pass++) {
            pos.z += dir * TRACE_EPS * 2.0f;
            bumped = 1;
            if (pass == 3) dir = -1.0f;
        }
    }

    /* Entity-vs-entity: cheap XY push so two players can't share a cell.      */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *o = ent_get(w, i);
        if (!o || o == ent) continue;
        if (o->type != ET_PLAYER && o->type != ET_BOT) continue;
        /* vertical overlap test (use nominal 56 height) */
        if (pos.z + height <= o->pos.z || pos.z >= o->pos.z + height) continue;
        float dx = pos.x - o->pos.x;
        float dy = pos.y - o->pos.y;
        float minsep = hx + radius;   /* both use same radius here             */
        float d2 = dx*dx + dy*dy;
        if (d2 >= minsep * minsep) continue;
        float d = mx_sqrtf(d2);
        if (d < 1e-4f) { dx = 1.0f; dy = 0.0f; d = 1.0f; } /* exactly coincident */
        float push = (minsep - d) * 0.5f;
        float nx = dx / d, ny = dy / d;
        pos.x += nx * push; pos.y += ny * push;
        bumped = 1;
    }

    ent->pos = pos;
    if (grounded) ent->on_ground = 1;
    return bumped;
}
