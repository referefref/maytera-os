/* Maytera Arena - modes.c
 * Deathmatch ruleset: spawn players + bots + items, score frags, respawn the
 * dead, hand out pickups, and end the match at the frag limit.
 *
 * Calls into: level_load/map_set_level (levels.c/world.c), ent_alloc/ent_free/
 * ent_get (main.c), bot_spawn (bots.c), snd_play (sound.c). Weapon defaults
 * come from weapons.c.
 */
#include "game.h"

/* Default ammo granted with a weapon pickup, defined in weapons.c.            */
extern const int g_weap_pickup_ammo[NUM_WEAPONS];

/* ------------------------------------------------------------------ tuning */
#define RESPAWN_DELAY_MS   1500     /* time dead before you respawn             */
#define PICKUP_RADIUS      40.0f    /* horizontal reach to grab an item         */
#define PICKUP_ZSPAN       72.0f    /* vertical overlap window                  */
#define HEALTH_CAP         100
#define HEALTH_CAP_MEGA    200
#define ARMOR_CAP          200

#define ITEM_RESPAWN_HEALTH  15000
#define ITEM_RESPAWN_ARMOR   20000
#define ITEM_RESPAWN_AMMO    15000
#define ITEM_RESPAWN_WEAPON  15000
#define ITEM_RESPAWN_MEGA    30000

/* Distinct scoreboard colours (0x00RRGGBB). Slot 0 (You) gets the first.      */
static const uint32_t g_slot_colors[] = {
    0x0040C0FF, /* You  - sky blue   */
    0x00FF4040, /* red               */
    0x0040FF40, /* green             */
    0x00FFD040, /* yellow            */
    0x00FF60FF, /* magenta           */
    0x0040FFFF, /* cyan              */
    0x00FF8020, /* orange            */
    0x00A060FF, /* purple            */
    0x00FFFFFF, /* white             */
    0x0080FF80, /* mint              */
    0x00FF80A0, /* pink              */
    0x00A0A0A0, /* grey              */
    0x0060A0FF, /* steel             */
    0x00C0FF40, /* lime              */
    0x00FFA060, /* peach             */
    0x0080D0D0  /* teal              */
};

/* ------------------------------------------------------------------ RNG --- */
static uint32_t g_mrng = 0xB5297A4Du;
static uint32_t mrng(void) {
    uint32_t x = g_mrng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_mrng = x;
    return x;
}

/* ---------------------------------------------------------- small helpers - */
static void zero_bytes(void *p, unsigned n) {
    unsigned char *b = (unsigned char *)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}
static void str_copy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
/* Write "Bot <n>" into dst (n up to 999).                                     */
static void bot_name(char *dst, int cap, int n) {
    const char *pfx = "Bot ";
    int i = 0;
    for (; pfx[i] && i < cap - 1; i++) dst[i] = pfx[i];
    char digits[4]; int d = 0;
    if (n <= 0) { digits[d++] = '0'; }
    else { int t = n; while (t > 0 && d < 4) { digits[d++] = (char)('0' + t % 10); t /= 10; } }
    while (d > 0 && i < cap - 1) dst[i++] = digits[--d];
    dst[i] = 0;
}

/* ---------------------------------------------------------- spawn picking - */
/* #501 LAST RESORT: not one spawn in the table is standable. Rescue the point by
 * walking UP its own column and taking the first height where the player box
 * genuinely fits: for a spawn buried under cover, that is the cover's roof.
 * Returns 1 and fills *out on success.
 *
 * Bounded by construction ((world height / step) iterations, no while-true), and
 * a FALLBACK only: it never runs while any authored spawn is usable, so it
 * cannot quietly relocate a level's spawns out from under the designer.
 *
 * Asks map_spawn_valid() about every candidate rather than computing a "should
 * be fine" height, so the rescue is held to the exact standard the filter is.
 *
 * THE FIRST VERSION OF THIS FUNCTION WAS SILENTLY BROKEN AND spawn_test/ CAUGHT
 * IT: it dropped the box from `world_maxs.z - PLAYER_HEIGHT - 8`, on the
 * assumption that world_maxs.z is the ceiling. It is not. build_shell sets
 * world_maxs.z = ceilz + 64 (the world BOUND), while the ceiling BRUSH occupies
 * ceilz..ceilz+24, so that start height put the box inside the ceiling brush,
 * map_box_solid said "sealed in", and the rescue returned 0 every single time on
 * every box level. It would never have been noticed in play, because this path
 * cannot fire on any shipping level. A fallback nobody can trigger is a fallback
 * nobody will test: test it offline or do not claim it works.                */
static int spawn_rescue(World *w, vec3 base, Spawn *out) {
    const float step = 8.0f;
    float top = w->level.world_maxs.z;
    for (float z = base.z; z < top; z += step) {
        if (!map_spawn_valid(w, v3(base.x, base.y, z))) continue;
        out->pos = v3(base.x, base.y, z);
        out->yaw = 0.0f;
        return 1;
    }
    return 0;
}

/* Choose a spawn point. If far_from_players, maximise distance to live players.
 *
 * #501: candidates are filtered through map_spawn_valid() FIRST, so a spawn that
 * a level-design pass has buried inside a cover brush can never be drawn. See
 * world.c map_spawn_valid() for the bug this fixes.
 *
 * VALIDATION HAPPENS HERE, AT SELECTION TIME, NOT AT LEVEL LOAD. Deliberate:
 *  - pick_spawn is the ONE chokepoint every spawn flows through, for both world
 *    types. The spawn TABLE, by contrast, is filled by two different paths
 *    (level_load for the box levels, arena_start_bsp for an imported BSP), and
 *    blame.md records a live bug from exactly that duplication: arena_start_bsp
 *    rebuilt the world and zeroed nbrush/nprop/nitem but forgot nspawn, so
 *    de_dust2 ran a whole match on The Longest Yard's stale spawn table. A
 *    load-time hook would have to be installed on every such path, and a future
 *    third path would silently miss it. That IS the bug class, not the fix.
 *  - it cannot go stale: it reads the geometry that is live right now.
 *  - the cost is trivial and nowhere near a hot path: a respawn is a
 *    once-per-life event, and one check is O(nspawn * nbrush) AABB compares
 *    (8 * <=256 here), versus 60Hz of software rasterisation.
 * The level-wide proof a load-time count would have given us lives in
 * spawn_test/ instead: offline, every spawn of every level, on every build.  */
static Spawn pick_spawn(World *w, int far_from_players) {
    int n = w->level.nspawn;
    if (n > MAX_SPAWNS) n = MAX_SPAWNS;

    /* Filter to spawns a player can actually stand in (#501).                 */
    int valid[MAX_SPAWNS];
    int nv = 0;
    for (int i = 0; i < n; i++)
        if (map_spawn_valid(w, w->level.spawns[i].pos)) valid[nv++] = i;

    if (nv <= 0) {
        /* Either the table is empty or every entry is buried. Never loop, never
         * silently entomb the player, and never let it pass unremarked: a level
         * in this state is a level-design bug and must be visible in the log. */
        arena_log("[ARENA] #501 NO VALID SPAWN in this level, using fallback\n");
        Spawn s;
        for (int i = 0; i < n; i++)
            if (spawn_rescue(w, w->level.spawns[i].pos, &s)) return s;
        /* Nothing stands anywhere. Do the least-worst thing and preserve the
         * historical behaviour rather than inventing a new failure: a random
         * authored spawn if we have one, else the legacy constant. Both may be
         * inside rock, which is why the line above is logged unconditionally.
         * (Noted for whoever gets here: the legacy (0,0,48) constant is itself
         * inside The Longest Yard's tier-2 pyramid, so it was never a safe
         * default; it only ever survived because nspawn > 0 on every level.)  */
        if (n > 0) return w->level.spawns[mrng() % (uint32_t)n];
        s.pos = v3(0, 0, 48); s.yaw = 0;
        return s;
    }

    if (!far_from_players)
        return w->level.spawns[valid[mrng() % (uint32_t)nv]];

    int best = -1;
    float best_score = -1.0f;
    /* Look at a few random candidates and keep the most isolated one.         */
    int tries = nv < 6 ? nv : 6;
    for (int c = 0; c < tries; c++) {
        int idx = valid[mrng() % (uint32_t)nv];
        Spawn *sp = &w->level.spawns[idx];
        float nearest = 1.0e9f;
        for (int s = 0; s < w->nplayers; s++) {
            Entity *pe = ent_get(w, w->players[s].entity);
            if (!pe) continue;
            float d = v3dist(sp->pos, pe->pos);
            if (d < nearest) nearest = d;
        }
        if (nearest > best_score) { best_score = nearest; best = idx; }
    }
    if (best < 0) best = valid[mrng() % (uint32_t)nv];
    return w->level.spawns[best];
}

/* Give a freshly-spawned player entity the deathmatch starting loadout.        */
static void give_start_loadout(Entity *e) {
    for (int i = 0; i < NUM_WEAPONS; i++) { e->have_weapon[i] = 0; e->ammo[i] = 0; }
    e->have_weapon[W_GAUNTLET]   = 1;
    e->have_weapon[W_MACHINEGUN] = 1;
    e->ammo[W_MACHINEGUN]        = 100;
    e->weapon = W_MACHINEGUN;
}

/* Create a fresh live entity for a player slot and attach it. Returns idx.     */
static int spawn_player_entity(World *w, int slot, int far_from_players) {
    Spawn sp = pick_spawn(w, far_from_players);
    int ei = ent_alloc(w);
    if (ei < 0) return -1;
    Entity *e = ent_get(w, ei);
    e->type        = w->players[slot].is_bot ? ET_BOT : ET_PLAYER;
    e->pos         = sp.pos;
    /* #481: lift feet a hair off the spawn surface. Spawns place feet at the
     * exact floor-top Z (levels.c), so the entity rests EXACTLY coplanar with
     * the floor brush's top face. The swept-AABB trace (world.c ray_vs_aabb)
     * treats a feet point sitting on that face as "started inside" and returns
     * frac 0 for every horizontal move, pinning pos so nothing translates
     * (players via input AND bots via AI). Nudging up means the first frame's
     * gravity lands the entity with the standard TRACE_EPS ground gap, after
     * which feet are always slightly above the floor and horizontal slides work.
     * Landings self-heal the gap thereafter, so this one nudge is sufficient.
     * #501: the literal 2.0f became SPAWN_FEET_NUDGE (game.h) so that
     * map_spawn_valid() probes the box at the position this line ACTUALLY
     * places it. If these two ever drift apart, the validity check starts
     * vouching for a position no player is ever put in.                      */
    e->pos.z      += SPAWN_FEET_NUDGE;
    e->vel         = v3(0, 0, 0);
    e->yaw         = sp.yaw;
    e->pitch       = 0.0f;
    e->health      = 100;
    e->armor       = 0;
    e->on_ground   = 0;
    e->fire_cooldown = 0;
    e->player_slot = slot;
    give_start_loadout(e);
    w->players[slot].entity     = ei;
    w->players[slot].respawn_ms = 0;
    return ei;
}

/* -------------------------------------------------------------- item spawn */
static void spawn_level_items(World *w) {
    for (int i = 0; i < w->level.nitem; i++) {
        ItemDef *id = &w->level.items[i];
        int ei = ent_alloc(w);
        if (ei < 0) break;
        Entity *e = ent_get(w, ei);
        e->type       = ET_ITEM;
        e->pos        = id->pos;
        e->item_kind  = id->kind;
        e->item_value = id->value;
        e->respawn_ms = 0;          /* 0 = available; >0 = respawning           */
        e->player_slot = -1;
    }
}

/* Spawn shootable props (explosive barrels) as ET_PROP entities. Trees/lamps
 * stay in the level prop list and are drawn directly by render.c (no entity).  */
static void spawn_level_props(World *w) {
    for (int i = 0; i < w->level.nprop; i++) {
        PropDef *pd = &w->level.props[i];
        if (pd->kind != PROP_BARREL) continue;      /* only barrels are entities */
        int ei = ent_alloc(w);
        if (ei < 0) break;
        Entity *e = ent_get(w, ei);
        e->type        = ET_PROP;
        e->prop_kind   = PROP_BARREL;
        e->pos         = pd->pos;
        e->health      = 30;                          /* pops after a few hits    */
        e->player_slot = -1;
        e->on_ground   = 1;
    }
}

static int item_respawn_time(int kind) {
    switch (kind) {
    case IT_HEALTH: return ITEM_RESPAWN_HEALTH;
    case IT_ARMOR:  return ITEM_RESPAWN_ARMOR;
    case IT_AMMO:   return ITEM_RESPAWN_AMMO;
    case IT_WEAPON: return ITEM_RESPAWN_WEAPON;
    case IT_MEGA:   return ITEM_RESPAWN_MEGA;
    default:        return ITEM_RESPAWN_HEALTH;
    }
}

/* Apply a pickup to a live player entity.                                     */
static void apply_item(Entity *p, Entity *it) {
    switch (it->item_kind) {
    case IT_HEALTH: {
        int amt = it->item_value > 0 ? it->item_value : 25;
        p->health += amt;
        if (p->health > HEALTH_CAP) p->health = HEALTH_CAP;
    } break;
    case IT_ARMOR: {
        int amt = it->item_value > 0 ? it->item_value : 50;
        p->armor += amt;
        if (p->armor > ARMOR_CAP) p->armor = ARMOR_CAP;
    } break;
    case IT_AMMO: {
        int wp = it->item_value;
        if (wp > 0 && wp < NUM_WEAPONS) {
            p->ammo[wp] += g_weap_pickup_ammo[wp] > 0 ? g_weap_pickup_ammo[wp] : 20;
        } else {
            for (int i = 0; i < NUM_WEAPONS; i++) p->ammo[i] += 10;
        }
    } break;
    case IT_WEAPON: {
        int wp = it->item_value;
        if (wp >= 0 && wp < NUM_WEAPONS) {
            p->have_weapon[wp] = 1;
            p->ammo[wp] += g_weap_pickup_ammo[wp];
            p->weapon = wp;              /* auto-switch to the shiny new gun     */
        }
    } break;
    case IT_MEGA: {
        p->health += 100;               /* megahealth: +100, capped at 200      */
        if (p->health > HEALTH_CAP_MEGA) p->health = HEALTH_CAP_MEGA;
    } break;
    default: break;
    }
}

/* ================================================================ mode API */

void mode_init(World *w, int level_index, int num_bots) {
    zero_bytes(w, sizeof(*w));

    w->level_index = level_index;
    level_load(level_index, &w->level);
    map_set_level(w, &w->level);

    w->frag_limit   = FRAG_LIMIT;
    w->winner       = -1;
    w->local_player = 0;
    w->cmd.wantweap = -1;

    int total = num_bots + 1;
    if (total < 1) total = 1;
    if (total > MAX_PLAYERS) total = MAX_PLAYERS;
    w->nplayers = total;

    int ncol = (int)(sizeof(g_slot_colors) / sizeof(g_slot_colors[0]));
    for (int s = 0; s < total; s++) {
        PlayerSlot *ps = &w->players[s];
        ps->frags   = 0;
        ps->deaths  = 0;
        ps->entity  = -1;
        ps->respawn_ms = 0;
        ps->is_bot  = (s == 0) ? 0 : 1;
        ps->color   = g_slot_colors[s % ncol];
        if (s == 0) str_copy(ps->name, "You", (int)sizeof(ps->name));
        else        bot_name(ps->name, (int)sizeof(ps->name), s);

        spawn_player_entity(w, s, 0);
        if (ps->is_bot) bot_spawn(w, s);
    }

    spawn_level_items(w);
    spawn_level_props(w);
}

void mode_update(World *w, int dt_ms) {
    /* 1) respawn dead players once their timer runs out.                      */
    for (int s = 0; s < w->nplayers; s++) {
        PlayerSlot *ps = &w->players[s];
        if (ps->entity >= 0) continue;
        ps->respawn_ms -= dt_ms;
        if (ps->respawn_ms <= 0) mode_respawn_player(w, s);
    }

    /* 2) items: respawn timers + pickups.                                     */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *it = ent_get(w, i);
        if (!it || it->type != ET_ITEM) continue;

        if (it->respawn_ms > 0) {           /* still cooling down, not grabbable */
            it->respawn_ms -= dt_ms;
            if (it->respawn_ms < 0) it->respawn_ms = 0;
            continue;
        }
        for (int s = 0; s < w->nplayers; s++) {
            Entity *pe = ent_get(w, w->players[s].entity);
            if (!pe || pe->health <= 0) continue;
            vec3 d = v3sub(pe->pos, it->pos);
            float horiz = mx_sqrtf(d.x * d.x + d.y * d.y);
            if (horiz > PICKUP_RADIUS) continue;
            if (d.z < -PICKUP_ZSPAN || d.z > PICKUP_ZSPAN) continue;
            apply_item(pe, it);
            it->respawn_ms = item_respawn_time(it->item_kind);
            snd_play(SND_PICKUP);
            break;
        }
    }

    /* 3) win condition: first to the frag limit ends the match.               */
    if (w->state != GS_GAMEOVER) {
        for (int s = 0; s < w->nplayers; s++) {
            if (w->players[s].frags >= w->frag_limit) {
                w->winner = s;
                w->state  = GS_GAMEOVER;
                break;
            }
        }
    }
}

void mode_on_kill(World *w, int victim, int attacker) {
    /* victim/attacker are ENTITY indices; map to player slots. The victim
     * entity is still alive at this point (ent_damage frees it afterwards).   */
    Entity *v = ent_get(w, victim);
    int vslot = v ? v->player_slot : -1;
    Entity *a = ent_get(w, attacker);
    int aslot = a ? a->player_slot : -1;

    if (attacker >= 0 && attacker != victim && aslot >= 0 && aslot != vslot) {
        w->players[aslot].frags++;                  /* clean frag              */
    } else if (vslot >= 0) {
        w->players[vslot].frags--;                  /* suicide / world / self  */
    }

    if (vslot >= 0) {
        w->players[vslot].deaths++;
        w->players[vslot].respawn_ms = RESPAWN_DELAY_MS;
    }
}

void mode_respawn_player(World *w, int player_slot) {
    if (player_slot < 0 || player_slot >= w->nplayers) return;
    if (w->players[player_slot].entity >= 0) return;   /* already alive         */

    int ei = spawn_player_entity(w, player_slot, 1);
    if (ei < 0) {                                       /* pool full: try soon   */
        w->players[player_slot].respawn_ms = 500;
        return;
    }
    if (w->players[player_slot].is_bot) bot_spawn(w, player_slot);
}
