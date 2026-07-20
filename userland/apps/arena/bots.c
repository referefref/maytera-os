/* Maytera Arena - bots.c
 * Bot AI. Each bot has a "brain" (a slot in a static pool, pointed to by
 * Entity.brain) holding its target, memory, timers and difficulty. bot_think()
 * runs once per bot per frame BEFORE phys_step: it only sets the bot entity's
 * DESIRED horizontal velocity (vel.x/y), aim (yaw/pitch), an occasional jump
 * (vel.z), current weapon, and calls weap_fire(). It never touches pos - the
 * physics module moves the bot using vel + collision.
 *
 * Cost budget: a bot does a handful of ray traces per frame (one line-of-sight
 * to its current enemy, up to three short obstacle probes while roaming) plus a
 * throttled nearest-enemy rescan a few times a second. Cheap enough for 15
 * bots at 60Hz on the software renderer.
 */
#include "game.h"

/* ------------------------------------------------------------------ tunables */
#define BOT_SPEED        300.0f   /* desired ground speed (units/sec)          */
#define BOT_JUMP_VEL     300.0f   /* vertical kick when hopping a ledge/gap     */
#define EYE_Z            40.0f    /* eye height above feet                      */
#define CHEST_Z          30.0f    /* aim target height on enemies              */
#define RETARGET_MS      250      /* nearest-enemy rescan period (skill scaled) */
#define PROBE_LEN        96.0f    /* obstacle probe distance                    */
#define REACH_ITEM       56.0f    /* "arrived at roam target" radius           */
#define STUCK_TIME_MS    600      /* movement below threshold for this = stuck  */
#define STUCK_DIST       24.0f    /* min travel over STUCK_TIME_MS to be un-stuck*/

/* difficulty tiers: 0 = easy, 1 = medium, 2 = hard                            */
static const float SKILL_AIM_ERR[3]  = { 0.22f, 0.10f, 0.035f }; /* radians    */
static const float SKILL_TURN[3]     = { 5.0f,  9.0f,  15.0f  }; /* rad/sec     */
static const int   SKILL_REACT_MS[3] = { 420,   240,   110    }; /* fire delay  */
static const float SKILL_AGGRO[3]    = { 0.55f, 0.80f, 1.0f   }; /* push factor */

/* ------------------------------------------------------------------ brain     */
typedef struct {
    int   used;
    int   slot;
    int   skill;

    int   enemy;            /* entity index of current target, -1 if none      */
    int   enemy_visible;    /* had LOS to enemy this frame                      */
    vec3  last_seen;        /* last known enemy position                       */
    int   have_last_seen;

    int   retarget_ms;      /* countdown to next nearest-enemy rescan          */
    int   react_ms;         /* reaction delay before firing at a fresh target  */

    vec3  roam_target;      /* where we wander to when there is no enemy        */
    int   has_roam;

    int   strafe_dir;       /* +1 / -1 circle-strafe direction                 */
    int   strafe_ms;        /* countdown to flip strafe direction              */

    vec3  last_pos;         /* for stuck detection                             */
    int   stuck_accum_ms;
    float wander_yaw;       /* current heading used while roaming/steering     */

    float aim_off_yaw;      /* slowly-wandering aim error (skill scaled)       */
    float aim_off_pitch;
    int   aim_off_ms;

    unsigned rng;
} Brain;

static Brain g_brains[MAX_PLAYERS];

/* ------------------------------------------------------------------ rng ----- */
static unsigned xorshift(unsigned *s)
{
    unsigned x = *s ? *s : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static float frand(Brain *b) { return (xorshift(&b->rng) & 0xFFFFFF) / (float)0x1000000; }
static float frand_s(Brain *b) { return frand(b) * 2.0f - 1.0f; }  /* [-1,1)    */

/* ------------------------------------------------------------------ angles --- */
static float ang_norm(float a)
{
    while (a >  M_PI_F) a -= 2.0f*M_PI_F;
    while (a < -M_PI_F) a += 2.0f*M_PI_F;
    return a;
}
static float turn_toward(float cur, float want, float max_step)
{
    float d = ang_norm(want - cur);
    if (d >  max_step) d =  max_step;
    if (d < -max_step) d = -max_step;
    return ang_norm(cur + d);
}

/* ------------------------------------------------------------------ helpers -- */
static int ent_is_fighter(const Entity *e)
{
    return e && e->alive && e->health > 0 &&
           (e->type == ET_PLAYER || e->type == ET_BOT);
}

static vec3 eye_of(const Entity *e) { return v3(e->pos.x, e->pos.y, e->pos.z + EYE_Z); }

/* clear line of sight from `from` to entity `target_idx`? one trace.          */
static int has_los(World *w, vec3 from, int self_idx, int target_idx)
{
    Entity *t = ent_get(w, target_idx);
    if (!t) return 0;
    vec3 aim = v3(t->pos.x, t->pos.y, t->pos.z + CHEST_Z);
    Trace tr = map_trace_ray(w, from, aim, self_idx, 1);
    /* visible when the trace reaches the target's own bbox first              */
    return (tr.hit_entity == target_idx) || (!tr.hit) || (tr.frac > 0.985f);
}

/* pick the nearest visible fighter (excluding self). Returns index or -1.     */
static int find_target(World *w, int self_idx, vec3 eye)
{
    int best = -1;
    float bestd2 = 1e30f;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (i == self_idx) continue;
        Entity *e = ent_get(w, i);
        if (!ent_is_fighter(e)) continue;
        vec3 d = v3sub(e->pos, eye);
        float d2 = v3dot(d, d);
        if (d2 >= bestd2) continue;
        if (!has_los(w, eye, self_idx, i)) continue;
        bestd2 = d2; best = i;
    }
    return best;
}

/* nearest live item entity (a pickup to roam toward). Returns idx or -1.      */
static int find_item(World *w, vec3 from)
{
    int best = -1; float bestd2 = 1e30f;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = ent_get(w, i);
        if (!e || e->type != ET_ITEM) continue;
        vec3 d = v3sub(e->pos, from);
        float d2 = d.x*d.x + d.y*d.y;          /* horizontal distance          */
        if (d2 < bestd2) { bestd2 = d2; best = i; }
    }
    return best;
}

/* choose the best weapon the bot actually owns for a given range.            */
static void pick_weapon(Entity *e, float dist)
{
    /* preference lists by band, first owned one wins                          */
    static const int close_pref[] = { W_LIGHTNING, W_SHOTGUN, W_PLASMA, W_MACHINEGUN, W_GAUNTLET };
    static const int mid_pref[]   = { W_ROCKET, W_PLASMA, W_LIGHTNING, W_MACHINEGUN, W_SHOTGUN };
    static const int far_pref[]   = { W_RAILGUN, W_MACHINEGUN, W_ROCKET, W_PLASMA, W_LIGHTNING };
    const int *pref; int n;

    if (dist < 96.0f && e->have_weapon[W_GAUNTLET]) { e->weapon = W_GAUNTLET; return; }
    if (dist < 380.0f)      { pref = close_pref; n = 5; }
    else if (dist < 800.0f) { pref = mid_pref;   n = 5; }
    else                    { pref = far_pref;   n = 5; }

    for (int i = 0; i < n; i++)
        if (e->have_weapon[pref[i]]) { e->weapon = pref[i]; return; }

    /* fallback: keep current if owned, else first owned weapon               */
    if (e->have_weapon[e->weapon]) return;
    for (int wi = 0; wi < NUM_WEAPONS; wi++)
        if (e->have_weapon[wi]) { e->weapon = wi; return; }
}

/* short wall probe from eye along a heading; 1 if blocked within PROBE_LEN.   */
static int blocked_dir(World *w, vec3 eye, int self_idx, float yaw, float len)
{
    vec3 end = v3(eye.x + mx_cosf(yaw)*len, eye.y + mx_sinf(yaw)*len, eye.z);
    Trace tr = map_trace_ray(w, eye, end, self_idx, 0);   /* brushes only      */
    return tr.hit && tr.frac < 0.99f;
}

/* set the horizontal desired velocity from a heading angle + speed scale.     */
static void drive(Entity *e, float yaw, float speed)
{
    e->vel.x = mx_cosf(yaw) * speed;
    e->vel.y = mx_sinf(yaw) * speed;
}

/* ------------------------------------------------------------------ spawn ---- */
void bot_spawn(World *w, int player_slot)
{
    if (!w || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    int ei = w->players[player_slot].entity;
    Entity *e = ent_get(w, ei);
    if (!e) return;

    Brain *b = &g_brains[player_slot];
    for (unsigned i = 0; i < sizeof(*b); i++) ((unsigned char*)b)[i] = 0;
    b->used = 1;
    b->slot = player_slot;
    /* spread three difficulty tiers across the bots so a match has variety    */
    b->skill = (player_slot * 5 + 1) % 3;
    b->enemy = -1;
    b->strafe_dir = (player_slot & 1) ? 1 : -1;
    b->strafe_ms = 900;
    b->retarget_ms = 0;
    b->rng = 0x9E3779B1u ^ (unsigned)(player_slot * 2654435761u + 12345u);
    b->last_pos = e->pos;
    b->wander_yaw = e->yaw;

    e->brain = b;
}

/* ------------------------------------------------------------------ free ----- */
void bot_free(Entity *e)
{
    if (!e || !e->brain) return;
    Brain *b = (Brain*)e->brain;
    b->used = 0;
    b->enemy = -1;
    e->brain = 0;
}

/* ------------------------------------------------------------------ think ---- */
void bot_think(World *w, int ent_idx, int dt_ms)
{
    Entity *e = ent_get(w, ent_idx);
    if (!e || !e->brain) return;
    Brain *b = (Brain*)e->brain;
    if (e->health <= 0) return;

    float dt = dt_ms * 0.001f;
    if (dt <= 0.0f) dt = 0.016f;
    vec3 eye = eye_of(e);

    /* --- timers ---------------------------------------------------------- */
    b->retarget_ms -= dt_ms;
    if (b->react_ms  > 0) b->react_ms  -= dt_ms;
    if (b->strafe_ms > 0) b->strafe_ms -= dt_ms;
    b->aim_off_ms -= dt_ms;

    /* --- target selection ------------------------------------------------ */
    int prev_enemy = b->enemy;

    /* verify current enemy is still alive + visible (one cheap LOS trace)    */
    b->enemy_visible = 0;
    if (b->enemy >= 0) {
        Entity *t = ent_get(w, b->enemy);
        if (!ent_is_fighter(t)) { b->enemy = -1; }
        else if (has_los(w, eye, ent_idx, b->enemy)) {
            b->enemy_visible = 1;
            b->last_seen = t->pos;
            b->have_last_seen = 1;
        }
    }

    /* periodically rescan for the nearest visible enemy (skill scaled rate)  */
    if (b->retarget_ms <= 0 || b->enemy < 0) {
        int scale = (b->skill == 2) ? 1 : (b->skill == 1 ? 2 : 3);
        b->retarget_ms = RETARGET_MS * scale;
        int cand = find_target(w, ent_idx, eye);
        if (cand >= 0) {
            b->enemy = cand;
            b->enemy_visible = 1;
            Entity *t = ent_get(w, cand);
            b->last_seen = t->pos; b->have_last_seen = 1;
        }
    }
    /* fresh sighting -> arm the reaction delay before we may shoot           */
    if (b->enemy >= 0 && b->enemy != prev_enemy)
        b->react_ms = SKILL_REACT_MS[b->skill];

    /* refresh the slowly-wandering aim error a few times a second           */
    if (b->aim_off_ms <= 0) {
        b->aim_off_ms = 180;
        float mag = SKILL_AIM_ERR[b->skill];
        b->aim_off_yaw   = frand_s(b) * mag;
        b->aim_off_pitch = frand_s(b) * mag * 0.6f;
    }

    /* --- decide aim + movement ------------------------------------------- */
    int want_fire = 0;
    Entity *tgt = (b->enemy >= 0) ? ent_get(w, b->enemy) : 0;

    if (tgt && b->enemy_visible) {
        /* ---- combat: aim, choose weapon, circle-strafe -------------------*/
        vec3 aim_at = v3(tgt->pos.x, tgt->pos.y, tgt->pos.z + CHEST_Z);
        vec3 d = v3sub(aim_at, eye);
        float horiz = mx_sqrtf(d.x*d.x + d.y*d.y);
        float want_yaw   = mx_atan2f(d.y, d.x) + b->aim_off_yaw;
        float want_pitch = mx_atan2f(d.z, horiz > 1.0f ? horiz : 1.0f) + b->aim_off_pitch;

        float maxturn = SKILL_TURN[b->skill] * dt;
        e->yaw   = turn_toward(e->yaw, want_yaw, maxturn);
        if (want_pitch >  1.4f) want_pitch =  1.4f;
        if (want_pitch < -1.4f) want_pitch = -1.4f;
        e->pitch = turn_toward(e->pitch, want_pitch, maxturn);

        float dist = v3len(d);
        pick_weapon(e, dist);

        /* fire when roughly on target, reaction elapsed, gauntlet only close */
        float aim_err = mx_absf(ang_norm(want_yaw - e->yaw));
        float fire_cone = 0.20f - 0.05f * b->skill;   /* tighter for pros      */
        int in_range = (e->weapon != W_GAUNTLET) || (dist < 100.0f);
        if (b->react_ms <= 0 && aim_err < fire_cone && in_range)
            want_fire = 1;

        /* movement: hold a comfortable band, strafe around the target        */
        float to_yaw = mx_atan2f(d.y, d.x);
        float approach = 0.0f;
        float pref_min = 220.0f, pref_max = 620.0f;
        if (e->weapon == W_RAILGUN)  { pref_min = 500.0f; pref_max = 1100.0f; }
        if (e->weapon == W_GAUNTLET) { pref_min = 0.0f;   pref_max = 90.0f;   }
        if (dist > pref_max)      approach =  1.0f;
        else if (dist < pref_min) approach = -1.0f;

        if (b->strafe_ms <= 0) { b->strafe_dir = -b->strafe_dir; b->strafe_ms = 700 + (int)(frand(b)*900); }
        float strafe_yaw = to_yaw + (b->strafe_dir > 0 ? 1.5708f : -1.5708f);

        vec3 mv = v3(0,0,0);
        mv = v3add(mv, v3(mx_cosf(to_yaw)*approach, mx_sinf(to_yaw)*approach, 0));
        float strafe_amt = 0.9f;
        mv = v3add(mv, v3(mx_cosf(strafe_yaw)*strafe_amt, mx_sinf(strafe_yaw)*strafe_amt, 0));
        vec3 mvd = v3norm(mv);
        float move_yaw = mx_atan2f(mvd.y, mvd.x);

        /* if the chosen move runs into a wall, bias to the open side         */
        if (blocked_dir(w, eye, ent_idx, move_yaw, PROBE_LEN)) {
            b->strafe_dir = -b->strafe_dir;
            move_yaw = to_yaw + (b->strafe_dir > 0 ? 1.5708f : -1.5708f);
            if (e->on_ground) e->vel.z = BOT_JUMP_VEL;   /* try hopping a ledge */
        }
        drive(e, move_yaw, BOT_SPEED * SKILL_AGGRO[b->skill]);
        b->has_roam = 0;   /* forget any roam goal while fighting             */
    }
    else {
        /* ---- no visible enemy: pursue last-seen, else roam to items -------*/
        vec3 goal; int have_goal = 0;

        if (b->have_last_seen) {
            goal = b->last_seen; have_goal = 1;
            if (v3dist(v3(goal.x,goal.y,e->pos.z), e->pos) < REACH_ITEM) {
                b->have_last_seen = 0; b->enemy = -1; have_goal = 0;
            }
        }
        if (!have_goal) {
            if (!b->has_roam) {
                int it = find_item(w, e->pos);
                Entity *ie = (it >= 0) ? ent_get(w, it) : 0;
                if (ie) { b->roam_target = ie->pos; }
                else {
                    /* wander to a random point using the map bounds          */
                    vec3 lo = w->level.world_mins, hi = w->level.world_maxs;
                    b->roam_target = v3(lo.x + frand(b)*(hi.x-lo.x),
                                        lo.y + frand(b)*(hi.y-lo.y),
                                        e->pos.z);
                }
                b->has_roam = 1;
            }
            goal = b->roam_target; have_goal = 1;
            if (v3dist(v3(goal.x,goal.y,e->pos.z), e->pos) < REACH_ITEM)
                b->has_roam = 0;
        }

        float goal_yaw = mx_atan2f(goal.y - e->pos.y, goal.x - e->pos.x);

        /* steer around walls with three short probes                        */
        float steer = goal_yaw;
        if (blocked_dir(w, eye, ent_idx, goal_yaw, PROBE_LEN)) {
            int left_ok  = !blocked_dir(w, eye, ent_idx, goal_yaw + 0.9f, PROBE_LEN);
            int right_ok = !blocked_dir(w, eye, ent_idx, goal_yaw - 0.9f, PROBE_LEN);
            if (left_ok)       steer = goal_yaw + 0.9f;
            else if (right_ok) steer = goal_yaw - 0.9f;
            else { steer = goal_yaw + M_PI_F; b->has_roam = 0; }  /* dead end  */
            if (e->on_ground) e->vel.z = BOT_JUMP_VEL;            /* hop ledge */
        }
        /* face where we move (slow turn) so the model looks natural          */
        e->yaw = turn_toward(e->yaw, steer, SKILL_TURN[b->skill] * dt);
        e->pitch = turn_toward(e->pitch, 0.0f, 2.0f * dt);
        drive(e, steer, BOT_SPEED * 0.85f);
    }

    /* --- stuck detection: barely moved while trying to -> shake loose ---- */
    {
        float moved = v3dist(e->pos, b->last_pos);
        b->stuck_accum_ms += dt_ms;
        if (b->stuck_accum_ms >= STUCK_TIME_MS) {
            if (moved < STUCK_DIST) {
                /* pick a new heading, drop the roam goal, and hop            */
                b->wander_yaw = frand_s(b) * M_PI_F;
                b->has_roam = 0;
                b->strafe_dir = -b->strafe_dir;
                if (e->on_ground) e->vel.z = BOT_JUMP_VEL;
                drive(e, b->wander_yaw, BOT_SPEED * 0.85f);
            }
            b->stuck_accum_ms = 0;
            b->last_pos = e->pos;
        }
    }

    /* --- fire ------------------------------------------------------------ */
    if (want_fire) weap_fire(w, ent_idx);
}
