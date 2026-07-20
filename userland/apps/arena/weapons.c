/* Maytera Arena - weapons.c
 * The 10 weapons, projectile simulation, hitscan tracing, damage + knockback.
 * Quake-flavoured. z is UP. Player radius 16, height 56, eye height 40.
 *
 * Calls into: map_trace_ray/map_move_entity (world.c), ent_alloc/ent_free/
 * ent_get (main.c), r_add_tracer/r_spawn_particles/r_explosion (render.c),
 * snd_play (sound.c), mode_on_kill (modes.c).
 */
#include "game.h"

/* ------------------------------------------------------------------ tuning */
#define EYE_HEIGHT      40.0f    /* eye offset above feet (pos.z)             */
#define BODY_CENTER     28.0f    /* mid-body offset for splash/aim targeting  */
#define KNOCK_SCALE     5.0f     /* velocity kick per point of raw damage     */
#define GRENADE_GRAVITY 800.0f   /* units/sec^2 pulling grenades down         */
#define GRENADE_RESTITUTION 0.55f/* velocity kept after a bounce              */
#define NAIL_HOMING     0.16f    /* steer strength toward nearest enemy       */
#define NAIL_HOMING_RANGE 1400.0f

/* Tier-1 #5 retune: barrels get physically shoved by splash before they die,
 * instead of sitting dead-still until they pop. weapons.c-only (reuses the
 * existing map_move_entity() move/collide primitive from world.c and the
 * splash-damage direction already computed at every call site) - never
 * touches physics.c's ground/air integration, so it can't step on the
 * physics agent's work. XY-only (no gravity): keeps this a "small" feel
 * addition, not a new physics object class.                                  */
#define BARREL_SHOVE_SCALE 3.0f  /* velocity impulse per point of splash dmg  */
#define BARREL_FRICTION    6.0f  /* per-second decay so a shoved barrel settles */
#define BARREL_RADIUS      20.0f /* matches world.c's ET_PROP collision size  */
#define BARREL_HEIGHT      44.0f

/* Per-weapon maximum hitscan range (WeaponDef has no range field).           */
static const float g_weap_range[NUM_WEAPONS] = {
    72.0f,    /* W_GAUNTLET  - melee reach                                    */
    8192.0f,  /* W_MACHINEGUN                                                 */
    8192.0f,  /* W_SHOTGUN                                                    */
    0.0f,     /* W_GRENADE   - projectile                                     */
    0.0f,     /* W_ROCKET    - projectile                                     */
    768.0f,   /* W_LIGHTNING - continuous beam                                */
    16384.0f, /* W_RAILGUN   - effectively unlimited                          */
    0.0f,     /* W_PLASMA    - projectile                                     */
    0.0f,     /* W_BFG       - projectile                                     */
    0.0f      /* W_NAILGUN   - projectile                                     */
};

/* ---------------------------------------------------------- weapon table -- */
const WeaponDef g_weapons[NUM_WEAPONS] = {
    /*                    name         hs  dmg  pel  spread   fireMs  splash  ammo  spd   tracer      */
    /* W_GAUNTLET  */ { "Gauntlet",     1,  50,  1,  0.0f,      300,     0,     0,    0, 0x00FFE0A0 },
    /* W_MACHINEGUN*/ { "Machinegun",   1,   7,  1,  0.020f,    100,     0,     1,    0, 0x00FFFFC0 },
    /* W_SHOTGUN   */ { "Shotgun",      1,  10, 11,  0.085f,   1000,     0,     1,    0, 0x00FFF0B0 },
    /* W_GRENADE   */ { "Grenade Launcher",0,100,0,  0.0f,      800,   150,     1,  700, 0x0060FF60 },
    /* W_ROCKET    */ { "Rocket Launcher",0,100,0,   0.0f,      800,   120,     1,  900, 0x00FF8020 },
    /* W_LIGHTNING */ { "Lightning Gun", 1,  8,  1,  0.0f,       50,     0,     1,    0, 0x0090C0FF },
    /* W_RAILGUN   */ { "Railgun",       1,100,  1,  0.0f,     1500,     0,     1,    0, 0x0040FFA0 },
    /* W_PLASMA    */ { "Plasma Gun",    0, 20,  0,  0.0f,      100,    20,     1, 1600, 0x0060A0FF },
    /* W_BFG       */ { "BFG10K",        0,100,  0,  0.0f,      200,   300,     1,  820, 0x00A0FF60 },
    /* W_NAILGUN   */ { "Nailgun",       0, 14,  0,  0.0f,       90,     0,     1, 1000, 0x00E0E0E0 }
};

/* Ammo granted when a weapon pickup is collected (used by modes.c too).       */
const int g_weap_pickup_ammo[NUM_WEAPONS] = {
    0,   /* gauntlet - no ammo */
    50,  /* machinegun */
    10,  /* shotgun */
    5,   /* grenades */
    5,   /* rockets */
    60,  /* lightning cells */
    10,  /* rail slugs */
    50,  /* plasma cells */
    20,  /* bfg cells */
    50   /* nails */
};

/* ------------------------------------------------------------------ RNG --- */
/* xorshift32, seeded from a monotonically increasing counter (never rand()).  */
static uint32_t g_rng = 0x9E3779B9u;
static uint32_t g_rng_seed_ctr = 1u;

static uint32_t rng_u32(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rng = x + (g_rng_seed_ctr += 0x6D2B79F5u);
    return x;
}
/* uniform in [0,1) */
static float rng_unit(void) { return (float)(rng_u32() >> 8) * (1.0f / 16777216.0f); }
/* uniform in [-1,1) */
static float rng_sym(void) { return rng_unit() * 2.0f - 1.0f; }

/* ------------------------------------------------------------- small utils */
static vec3 body_center(const Entity *e) {
    vec3 c = e->pos; c.z += BODY_CENTER; return c;
}

/* Build a perturbed direction inside a cone of half-angle `spread` around fwd. */
static vec3 spread_dir(vec3 fwd, float spread) {
    if (spread <= 0.0f) return v3norm(fwd);
    vec3 up = v3(0, 0, 1);
    vec3 right = v3cross(fwd, up);
    if (v3len(right) < 0.001f) right = v3(1, 0, 0);
    right = v3norm(right);
    vec3 ru = v3norm(v3cross(right, fwd));
    float rx = rng_sym() * spread;
    float ry = rng_sym() * spread;
    vec3 d = v3add(fwd, v3add(v3scale(right, rx), v3scale(ru, ry)));
    return v3norm(d);
}

/* Which PROJ_* kind a projectile weapon spawns (PROJ_NONE for hitscan).       */
static int weapon_proj_kind(int wp) {
    switch (wp) {
    case W_GRENADE: return PROJ_GRENADE;
    case W_ROCKET:  return PROJ_ROCKET;
    case W_PLASMA:  return PROJ_PLASMA;
    case W_BFG:     return PROJ_BFG;
    case W_NAILGUN: return PROJ_NAIL;
    default:        return PROJ_NONE;
    }
}

static int weapon_sound(int wp) {
    if (wp == W_ROCKET || wp == W_GRENADE || wp == W_BFG) return SND_ROCKET;
    if (wp == W_RAILGUN) return SND_RAIL;
    return SND_SHOOT;
}

/* First owned weapon that has enough ammo, preferring the given start index.   */
static int next_weapon_with_ammo(const Entity *e, int start) {
    for (int i = 0; i < NUM_WEAPONS; i++) {
        int wp = (start + i) % NUM_WEAPONS;
        if (!e->have_weapon[wp]) continue;
        if (e->ammo[wp] >= g_weapons[wp].ammo_per_shot) return wp;
    }
    return -1;
}

/* ------------------------------------------------------------- ent_damage - */
/* Explosive barrel: chain-reacting area burst. Defined after ent_damage.       */
static void barrel_explode(World *w, vec3 at, int attacker);

void ent_damage(World *w, int victim, int attacker, int dmg, vec3 dir) {
    Entity *v = ent_get(w, victim);
    if (!v) return;
    if (dmg <= 0) return;

    /* Explosive barrels: absorb hits, then pop with an area burst. Freed BEFORE
     * the burst so a chained barrel can't re-trigger this one.                 */
    if (v->type == ET_PROP) {
        if (v->prop_kind != PROP_BARREL || v->health <= 0) return;
        v->health -= dmg;
        if (v->health > 0) {
            r_spawn_particles(v->pos, 0x00FFAA30, 4, 90.0f);   /* struck sparks   */
            /* Tier-1 #5: shove the barrel along the hit/splash direction; the
             * velocity is integrated (with friction) in weap_update() below. */
            vec3 kd = v3(dir.x, dir.y, 0.0f);
            if (v3len(kd) > 0.001f) {
                kd = v3norm(kd);
                float kb = (float)dmg * BARREL_SHOVE_SCALE;
                v->vel.x += kd.x * kb;
                v->vel.y += kd.y * kb;
            }
            return;
        }
        vec3 at = v->pos; at.z += 22.0f;
        ent_free(w, victim);
        barrel_explode(w, at, attacker);
        return;
    }
    if (v->type != ET_PLAYER && v->type != ET_BOT) return;
    if (v->health <= 0) return;              /* already dead: double-death guard */

    /* Armor absorbs ~2/3 of incoming damage, up to what armor remains.         */
    int absorbed = 0;
    if (v->armor > 0) {
        absorbed = (dmg * 2) / 3;
        if (absorbed > v->armor) absorbed = v->armor;
        v->armor -= absorbed;
    }
    int taken = dmg - absorbed;
    if (taken < 0) taken = 0;
    v->health -= taken;

    /* Knockback (rocket-jump friendly): kick velocity along the hit direction. */
    vec3 kd = v3norm(dir);
    float kb = (float)dmg * KNOCK_SCALE;
    v->vel = v3add(v->vel, v3scale(kd, kb));
    v->vel.z += kb * 0.30f;                   /* a little extra lift            */

    snd_play(SND_PAIN);

    if (v->health <= 0) {
        vec3 c = body_center(v);
        r_spawn_particles(c, 0x00CC2020, 28, 320.0f);   /* gib / blood burst    */
        snd_play(SND_DIE);
        int slot = v->player_slot;
        /* mode_on_kill needs the victim entity intact (reads player_slot).     */
        mode_on_kill(w, victim, attacker);
        if (slot >= 0 && slot < MAX_PLAYERS) w->players[slot].entity = -1;
        ent_free(w, victim);
    }
}

/* Barrel area burst: damages nearby actors AND other barrels (chain reaction).
 * The exploding barrel is already freed by the caller.                        */
static void barrel_explode(World *w, vec3 at, int attacker) {
    const float R = 160.0f;
    const int   BASE = 95;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *t = ent_get(w, i);
        if (!t || t->health <= 0) continue;
        int actor  = (t->type == ET_PLAYER || t->type == ET_BOT);
        int barrel = (t->type == ET_PROP && t->prop_kind == PROP_BARREL);
        if (!actor && !barrel) continue;
        vec3 c = barrel ? v3(t->pos.x, t->pos.y, t->pos.z + 22.0f) : body_center(t);
        float dist = v3dist(at, c);
        if (dist >= R) continue;
        int dmg = (int)((float)BASE * (1.0f - dist / R) + 0.5f);
        if (dmg <= 0) continue;
        vec3 kdir = v3sub(c, at);
        if (v3len(kdir) < 0.001f) kdir = v3(0, 0, 1);
        ent_damage(w, i, attacker, dmg, kdir);        /* chains into other barrels */
    }
    r_explosion(at, R);
    r_spawn_particles(at, 0x00FF8020, 26, 320.0f);
    snd_play(SND_EXPLODE);
}

/* ------------------------------------------------------------ hitscan fire */
static void fire_hitscan(World *w, int shooter, Entity *e,
                         const WeaponDef *def, int wp, vec3 eye, vec3 fwd) {
    float range = g_weap_range[wp];
    int pellets = def->pellets > 0 ? def->pellets : 1;
    int draw_beam = (wp == W_RAILGUN || wp == W_LIGHTNING);

    for (int p = 0; p < pellets; p++) {
        vec3 dir = (def->spread > 0.0f) ? spread_dir(fwd, def->spread) : v3norm(fwd);
        vec3 end = v3add(eye, v3scale(dir, range));
        Trace tr = map_trace_ray(w, eye, end, shooter, 1);
        vec3 hitpt = tr.hit ? tr.end : end;

        if (tr.hit) {
            if (tr.hit_entity >= 0) {
                ent_damage(w, tr.hit_entity, shooter, def->damage, dir);
                r_spawn_particles(hitpt, 0x00CC3030, 6, 160.0f);  /* impact blood */
            } else {
                r_spawn_particles(hitpt, 0x00C0C0C0, 5, 140.0f);  /* wall sparks  */
            }
        }
        if (draw_beam) r_add_tracer(eye, hitpt, def->tracer_rgb);
    }
    /* Railgun leaves a bright slug trail even into empty space.                */
    if (wp == W_RAILGUN) {
        vec3 far = v3add(eye, v3scale(v3norm(fwd), range));
        r_add_tracer(eye, far, def->tracer_rgb);
    }
}

/* --------------------------------------------------------- projectile fire */
static void fire_projectile(World *w, int shooter, Entity *e,
                            const WeaponDef *def, int wp, vec3 eye, vec3 fwd) {
    int pi = ent_alloc(w);
    if (pi < 0) return;                       /* entity pool full: silently drop */
    Entity *pe = ent_get(w, pi);
    pe->type       = ET_PROJECTILE;
    pe->pos        = eye;
    pe->vel        = v3scale(v3norm(fwd), (float)def->proj_speed);
    pe->proj_kind  = weapon_proj_kind(wp);
    pe->proj_owner = shooter;
    pe->proj_weapon= wp;
    pe->player_slot= -1;
    /* grenades run a short fuse; everything else flies until it hits or expires */
    pe->proj_life_ms = (wp == W_GRENADE) ? 2500 : 4000;
    (void)e;
}

/* ---------------------------------------------------------------- weap_fire */
void weap_fire(World *w, int shooter_idx) {
    Entity *e = ent_get(w, shooter_idx);
    if (!e) return;
    if (e->type != ET_PLAYER && e->type != ET_BOT) return;
    if (e->fire_cooldown > 0) return;

    int wp = e->weapon;
    if (wp < 0 || wp >= NUM_WEAPONS) return;
    if (!e->have_weapon[wp]) {
        int ns = next_weapon_with_ammo(e, 0);
        if (ns >= 0) e->weapon = ns;
        return;
    }
    const WeaponDef *def = &g_weapons[wp];

    if (e->ammo[wp] < def->ammo_per_shot) {
        /* Out of ammo: auto-switch to something usable (falls back to gauntlet).*/
        int ns = next_weapon_with_ammo(e, wp);
        if (ns >= 0) e->weapon = ns;
        snd_play(SND_HITBEEP);                /* dry "click"                     */
        return;
    }

    e->ammo[wp]      -= def->ammo_per_shot;
    e->fire_cooldown  = def->fire_ms;

    /* seed the RNG from a per-shot counter so spread patterns vary each shot.  */
    g_rng ^= (g_rng_seed_ctr += 0x9E3779B9u) ^ (uint32_t)(w->time_ms * 2654435761u);

    vec3 eye = e->pos; eye.z += EYE_HEIGHT;
    vec3 fwd = v3fromangles(e->yaw, e->pitch);

    if (def->is_hitscan) fire_hitscan(w, shooter_idx, e, def, wp, eye, fwd);
    else                 fire_projectile(w, shooter_idx, e, def, wp, eye, fwd);

    snd_play(weapon_sound(wp));
}

/* ---------------------------------------------------------- detonation ---- */
static void proj_detonate(World *w, int pi, Entity *pe, vec3 at, int direct_victim) {
    const WeaponDef *def = &g_weapons[pe->proj_weapon];
    vec3 flight = v3norm(pe->vel);

    /* Direct hit damage on the entity we struck.                              */
    if (direct_victim >= 0)
        ent_damage(w, direct_victim, pe->proj_owner, def->damage, flight);

    if (def->splash_radius > 0) {
        float R = (float)def->splash_radius;
        for (int i = 0; i < MAX_ENTITIES; i++) {
            if (i == direct_victim) continue;         /* no double-dipping      */
            Entity *t = ent_get(w, i);
            if (!t) continue;
            if (t->type != ET_PLAYER && t->type != ET_BOT) continue;
            if (t->health <= 0) continue;
            vec3 c = body_center(t);
            float dist = v3dist(at, c);
            if (dist >= R) continue;
            float falloff = 1.0f - dist / R;          /* linear falloff         */
            int dmg = (int)((float)def->damage * falloff + 0.5f);
            if (dmg <= 0) continue;
            vec3 kdir = v3sub(c, at);
            if (v3len(kdir) < 0.001f) kdir = v3(0, 0, 1);
            ent_damage(w, i, pe->proj_owner, dmg, kdir);
        }
        r_explosion(at, R);
        snd_play(SND_EXPLODE);
    } else {
        /* Non-splash projectile (nail): just a small impact spray.            */
        r_spawn_particles(at, def->tracer_rgb, 8, 150.0f);
    }
    ent_free(w, pi);
}

/* Steer a nail slightly toward the nearest live enemy of its owner.           */
static void nail_home(World *w, Entity *pe) {
    int best = -1;
    float bestd = NAIL_HOMING_RANGE;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (i == pe->proj_owner) continue;
        Entity *t = ent_get(w, i);
        if (!t) continue;
        if (t->type != ET_PLAYER && t->type != ET_BOT) continue;
        if (t->health <= 0) continue;
        float d = v3dist(pe->pos, body_center(t));
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return;
    Entity *t = ent_get(w, best);
    vec3 to = v3norm(v3sub(body_center(t), pe->pos));
    float speed = v3len(pe->vel);
    vec3 nd = v3norm(v3add(v3norm(pe->vel), v3scale(to, NAIL_HOMING)));
    pe->vel = v3scale(nd, speed);
}

/* ------------------------------------------------------------ weap_update - */
void weap_update(World *w, int dt_ms) {
    /* 1) tick every entity's fire cooldown down toward zero.                  */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = ent_get(w, i);
        if (!e) continue;
        if (e->fire_cooldown > 0) {
            e->fire_cooldown -= dt_ms;
            if (e->fire_cooldown < 0) e->fire_cooldown = 0;
        }
    }

    float dt = (float)dt_ms * 0.001f;

    /* 2) integrate + collide every projectile.                               */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *pe = ent_get(w, i);
        if (!pe || pe->type != ET_PROJECTILE) continue;

        pe->proj_life_ms -= dt_ms;

        if (pe->proj_kind == PROJ_GRENADE)
            pe->vel.z -= GRENADE_GRAVITY * dt;
        else if (pe->proj_kind == PROJ_NAIL)
            nail_home(w, pe);

        vec3 start = pe->pos;
        vec3 end   = v3add(start, v3scale(pe->vel, dt));
        Trace tr = map_trace_ray(w, start, end, pe->proj_owner, 1);

        if (tr.hit) {
            if (pe->proj_kind == PROJ_GRENADE && tr.hit_entity < 0) {
                /* Bounce off world geometry, lose some energy, keep flying.   */
                float vn = v3dot(pe->vel, tr.normal);
                pe->vel = v3sub(pe->vel, v3scale(tr.normal, 2.0f * vn));
                pe->vel = v3scale(pe->vel, GRENADE_RESTITUTION);
                pe->pos = v3add(tr.end, v3scale(tr.normal, 1.0f)); /* nudge off  */
                snd_play(SND_HITBEEP);
                if (pe->proj_life_ms <= 0) proj_detonate(w, i, pe, pe->pos, -1);
                continue;
            }
            /* Hit a wall (non-grenade) or an entity: detonate here.           */
            pe->pos = tr.end;
            proj_detonate(w, i, pe, tr.end, tr.hit_entity);
            continue;
        }

        pe->pos = end;

        /* Fuse / lifetime expiry: grenades explode, others simply detonate.   */
        if (pe->proj_life_ms <= 0)
            proj_detonate(w, i, pe, pe->pos, -1);
    }

    /* 3) Tier-1 #5: integrate + decay any barrel currently shoved by splash.
     * XY-only, reuses map_move_entity() (world.c's exported move/collide
     * primitive) so this never touches physics.c's own entity stepping.      */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *be = ent_get(w, i);
        if (!be || be->type != ET_PROP || be->prop_kind != PROP_BARREL) continue;
        float sp2 = be->vel.x * be->vel.x + be->vel.y * be->vel.y;
        if (sp2 < 4.0f) { be->vel.x = 0.0f; be->vel.y = 0.0f; continue; }
        vec3 delta = v3(be->vel.x * dt, be->vel.y * dt, 0.0f);
        map_move_entity(w, be, delta, BARREL_RADIUS, BARREL_HEIGHT);
        float decay = 1.0f - BARREL_FRICTION * dt;
        if (decay < 0.0f) decay = 0.0f;
        be->vel.x *= decay;
        be->vel.y *= decay;
    }
}

/* --------------------------------------------------------- weap_give_all -- */
void weap_give_all(Entity *e) {
    if (!e) return;
    for (int i = 0; i < NUM_WEAPONS; i++) {
        e->have_weapon[i] = 1;
        e->ammo[i] = 200;
    }
    e->ammo[W_GAUNTLET] = 0;   /* gauntlet consumes no ammo */
}
