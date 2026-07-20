/* Maytera Arena - physics.c
 * Movement integration for players and bots. Quake-like ground/air accel with
 * friction, gravity, jumping. Projectiles and items are NOT stepped here
 * (weapons.c owns projectile motion). z is UP.
 */
#include "game.h"

/* ------------------------------------------------------------------ tunables */
#define PHYS_GRAVITY     800.0f   /* units/s^2                                  */
#define PHYS_MAXSPEED    320.0f   /* ground walk speed                          */
#define PHYS_JUMPVEL     270.0f
#define PHYS_STOPSPEED   100.0f
#define PHYS_FRICTION      6.0f    /* player ground friction                    */
#define PHYS_BOT_FRICTION  4.0f    /* bots: lighter, vel is authoritative       */
#define PHYS_ACCEL_GROUND 10.0f
#define PHYS_ACCEL_AIR     1.0f
#define PHYS_BOT_ACCEL    12.0f    /* bots track their desired vel quickly      */
/* PLAYER_RADIUS / PLAYER_HEIGHT moved to game.h (#501): world.c's spawn probe
 * must test the exact box this file integrates, so they are shared, not copied. */
#define PHYS_MAXVEL      2000.0f   /* hard clamp on any velocity component      */

/* subtract the horizontal component along -wishdir up to friction, in place   */
static void apply_friction(vec3 *vel, float friction, float dt)
{
    float sx = vel->x, sy = vel->y;
    float speed = mx_sqrtf(sx*sx + sy*sy);
    if (speed < 1.0f) { vel->x = 0.0f; vel->y = 0.0f; return; }
    float control = speed < PHYS_STOPSPEED ? PHYS_STOPSPEED : speed;
    float drop = control * friction * dt;
    float newspeed = speed - drop;
    if (newspeed < 0.0f) newspeed = 0.0f;
    float scale = newspeed / speed;
    vel->x = sx * scale;
    vel->y = sy * scale;
}

/* classic PM_Accelerate on the horizontal plane                               */
static void apply_accel(vec3 *vel, vec3 wishdir, float wishspeed,
                        float accel, float dt)
{
    float cur = vel->x*wishdir.x + vel->y*wishdir.y;
    float add = wishspeed - cur;
    if (add <= 0.0f) return;
    float accelspeed = accel * dt * wishspeed;
    if (accelspeed > add) accelspeed = add;
    vel->x += accelspeed * wishdir.x;
    vel->y += accelspeed * wishdir.y;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void phys_step(World *w, int dt_ms)
{
    if (!w || dt_ms <= 0) return;
    float dt = (float)dt_ms / 1000.0f;

    int local_ent = -1;
    if (w->local_player >= 0 && w->local_player < MAX_PLAYERS)
        local_ent = w->players[w->local_player].entity;

    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = ent_get(w, i);
        if (!e) continue;
        if (e->type != ET_PLAYER && e->type != ET_BOT) continue;   /* skip proj/item */

        int is_local = (i == local_ent);
        int is_bot   = (e->type == ET_BOT);

        /* #491 Stage 1: BSP noclip free-fly for the local player. No gravity, no
         * collision (Stage 2 adds hull-trace); move the eye along the full 3D
         * look direction from WASD, and up with jump. Bots are left alone (they
         * simply clamp to the world box below in the normal path).             */
        if (g_arena_noclip && is_local) {
            vec3 look = v3fromangles(e->yaw, e->pitch);     /* full 3D forward   */
            vec3 flat = v3norm(v3(look.x, look.y, 0.0f));
            vec3 right = v3(flat.y, -flat.x, 0.0f);
            vec3 move = v3add(v3scale(look,  (float)w->cmd.fwd),
                              v3scale(right, (float)w->cmd.side));
            if (w->cmd.jump) move.z += 1.0f;                /* space rises       */
            float ml = v3len(move);
            if (ml > 0.0001f) {
                move = v3scale(move, 1.0f / ml);
                e->pos = v3add(e->pos, v3scale(move, 360.0f * dt));  /* fly speed */
            }
            /* keep inside a padded world box so we never fly to infinity */
            vec3 lo = w->level.world_mins, hi = w->level.world_maxs;
            if (hi.x > lo.x) {
                e->pos.x = clampf(e->pos.x, lo.x, hi.x);
                e->pos.y = clampf(e->pos.y, lo.y, hi.y);
                e->pos.z = clampf(e->pos.z, lo.z, hi.z);
            }
            e->vel = v3(0, 0, 0);
            e->on_ground = 1;              /* keeps the camera off the death-orbit */
            continue;
        }

        /* ---- build the desired horizontal move (wishdir / wishspeed) ------- */
        vec3  wishdir = v3(0,0,0);
        float wishspeed = 0.0f;

        if (is_local) {
            vec3 fwd = v3fromangles(e->yaw, 0.0f);   /* (cos,sin,0), already flat */
            fwd.z = 0.0f;
            fwd = v3norm(fwd);
            vec3 right = v3(fwd.y, -fwd.x, 0.0f);
            vec3 wish = v3add(v3scale(fwd,  (float)w->cmd.fwd),
                              v3scale(right, (float)w->cmd.side));
            wish.z = 0.0f;
            float wl = mx_sqrtf(wish.x*wish.x + wish.y*wish.y);
            if (wl > 0.0001f) {
                wishdir = v3scale(wish, 1.0f / wl);
                /* diagonal input has length sqrt2; cap magnitude at 1 */
                wishspeed = PHYS_MAXSPEED * (wl > 1.0f ? 1.0f : wl);
            }
        } else {
            /* bot: bot_think already wrote ent->vel.xy as the DESIRED velocity */
            vec3 wish = v3(e->vel.x, e->vel.y, 0.0f);
            float wl = mx_sqrtf(wish.x*wish.x + wish.y*wish.y);
            if (wl > 0.0001f) {
                wishdir = v3scale(wish, 1.0f / wl);
                wishspeed = wl > PHYS_MAXSPEED ? PHYS_MAXSPEED : wl;
            }
        }

        int was_on_ground = e->on_ground;

        /* ---- jump (before friction/accel, before gravity) ----------------- */
        if (is_local) {
            if (w->cmd.jump && was_on_ground) {
                e->vel.z = PHYS_JUMPVEL;
                was_on_ground = 0;
                e->on_ground = 0;
#ifdef SND_JUMP
                snd_play(SND_JUMP);
#endif
            }
        }
        /* bots: bot_think may already have set e->vel.z for a jump; leave it. */

        /* ---- friction + acceleration on the horizontal plane -------------- */
        if (was_on_ground) {
            apply_friction(&e->vel, is_bot ? PHYS_BOT_FRICTION : PHYS_FRICTION, dt);
            apply_accel(&e->vel, wishdir, wishspeed,
                        is_bot ? PHYS_BOT_ACCEL : PHYS_ACCEL_GROUND, dt);
        } else {
            apply_accel(&e->vel, wishdir, wishspeed,
                        is_bot ? (PHYS_BOT_ACCEL * 0.15f) : PHYS_ACCEL_AIR, dt);
        }

        /* ---- gravity ------------------------------------------------------ */
        if (!was_on_ground)
            e->vel.z -= PHYS_GRAVITY * dt;

        /* hard clamp so a bad input can never launch an entity to infinity   */
        e->vel.x = clampf(e->vel.x, -PHYS_MAXVEL, PHYS_MAXVEL);
        e->vel.y = clampf(e->vel.y, -PHYS_MAXVEL, PHYS_MAXVEL);
        e->vel.z = clampf(e->vel.z, -PHYS_MAXVEL, PHYS_MAXVEL);

        /* ---- integrate + collide ------------------------------------------ */
        e->on_ground = 0;   /* map_move_entity re-establishes ground contact   */
        vec3 delta = v3scale(e->vel, dt);
        map_move_entity(w, e, delta, PLAYER_RADIUS, PLAYER_HEIGHT);

        /* landing: if we came to rest on a floor while descending, kill any
         * residual downward velocity (map_move_entity already clips it on the
         * plane, this is the belt-and-suspenders guarantee).                  */
        if (e->on_ground && e->vel.z < 0.0f)
            e->vel.z = 0.0f;

        /* ---- keep inside the world box ------------------------------------ */
        vec3 lo = w->level.world_mins, hi = w->level.world_maxs;
        if (hi.x > lo.x) {   /* only if bounds look valid                      */
            e->pos.x = clampf(e->pos.x, lo.x + PLAYER_RADIUS, hi.x - PLAYER_RADIUS);
            e->pos.y = clampf(e->pos.y, lo.y + PLAYER_RADIUS, hi.y - PLAYER_RADIUS);
            /* z: allow standing on the floor bound, cap the ceiling           */
            if (e->pos.z < lo.z) { e->pos.z = lo.z; if (e->vel.z < 0) e->vel.z = 0; }
            if (e->pos.z + PLAYER_HEIGHT > hi.z) {
                e->pos.z = hi.z - PLAYER_HEIGHT;
                if (e->vel.z > 0) e->vel.z = 0;
            }
        }
    }
}
