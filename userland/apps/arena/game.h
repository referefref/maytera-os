/* Maytera Arena - a Quake-style 3D arena FPS for MayteraOS, rendered with the
 * userland TinyGL (software) engine into a compositor window.
 *
 * THIS HEADER IS THE SHARED CONTRACT between all modules. Do NOT change existing
 * struct layouts or function signatures without coordinating - other modules
 * depend on them. Add new fields at the END of structs, and new functions freely.
 *
 * Module ownership (one .c each, all built by the Makefile via $(wildcard *.c)):
 *   render.c  - TinyGL 3D scene + viewmodel + effects  (r_*)
 *   world.c   - map geometry + collision/tracing        (map_*)
 *   physics.c - movement integration + entity stepping  (phys_*)
 *   weapons.c - 10 weapons: fire, projectiles, damage   (weap_*)
 *   bots.c    - bot AI: navigate, target, shoot          (bot_*)
 *   levels.c  - the 5 arena maps (geometry + spawns)     (level_*)
 *   modes.c   - deathmatch rules: score, respawn, frags  (mode_*)
 *   hud.c     - HUD, crosshair, scoreboard, menu         (hud_*, menu_*)
 *   sound.c   - sound effects                            (snd_*)
 *   main.c    - window, GL context, input, game loop     (owns g_world)
 */
#ifndef ARENA_GAME_H
#define ARENA_GAME_H

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "mathx.h"

/* ------------------------------------------------------------------ limits */
#define MAX_ENTITIES     64      /* players + bots + projectiles + items      */
#define MAX_BRUSHES     256      /* convex axis-aligned boxes per level        */
#define MAX_SPAWNS       32
#define MAX_ITEMS        48
#define MAX_PARTICLES   512
#define MAX_PLAYERS      16      /* scoreboard slots (1 human + up to 15 bots)  */
#define NUM_WEAPONS      10
#define NUM_LEVELS        6      /* 5 arenas + the outdoor "Dust Storm" level    */
#define MAX_PROPS        64      /* trees/lamps/barrel-spawns per level          */
#define FRAG_LIMIT       20      /* deathmatch win condition                    */

/* ----------------------------------------------- player collision volume */
/* The player/bot AABB, FEET-centred: [pos-(r,r,0) .. pos+(r,r,height)].
 * SHARED ON PURPOSE (#501): physics.c integrates movement with these, and
 * world.c's spawn-validity probe tests the SAME box. They were private
 * #defines inside physics.c until #501 needed them too; a spawn checked
 * against a box that is not the box physics uses is a checker that agrees
 * with nothing, so there is exactly ONE definition of the player's size.    */
#define PLAYER_RADIUS    16.0f
#define PLAYER_HEIGHT    56.0f

/* #481: levels place spawn feet at the EXACT floor-top Z, which leaves the
 * entity coplanar with the floor brush and makes the swept trace report
 * "started inside" (pinning every horizontal move). spawn_player_entity()
 * therefore lifts feet by this much. map_spawn_valid() MUST probe at the same
 * offset: validating a position nobody ever occupies proves nothing.        */
#define SPAWN_FEET_NUDGE  2.0f

/* A spawn needs floor under it within this drop, or it is "bottomless" and the
 * player would fall (on a box level, all the way to the world_mins.z clamp).
 * Same 256u criterion the de_dust2 hull harness asserts against (#491).      */
#define SPAWN_MAX_DROP  256.0f

/* ------------------------------------------------------------------ weapons */
enum {
    W_GAUNTLET = 0,  /* melee                */
    W_MACHINEGUN,    /* rapid hitscan        */
    W_SHOTGUN,       /* spread hitscan       */
    W_GRENADE,       /* bouncing projectile  */
    W_ROCKET,        /* splash projectile    */
    W_LIGHTNING,     /* continuous hitscan beam */
    W_RAILGUN,       /* instant slug + trail */
    W_PLASMA,        /* fast projectile      */
    W_BFG,           /* heavy splash         */
    W_NAILGUN        /* homing-ish nails     */
};
typedef struct {
    const char *name;
    int   is_hitscan;     /* 1 = instant trace, 0 = spawns a projectile ent    */
    int   damage;         /* per hit / per pellet                              */
    int   pellets;        /* hitscan spread count (1 for single)               */
    float spread;         /* radians                                           */
    int   fire_ms;        /* cooldown between shots                             */
    int   splash_radius;  /* 0 = no splash                                     */
    int   ammo_per_shot;
    int   proj_speed;     /* units/sec for projectile weapons                  */
    uint32_t tracer_rgb;  /* colour for beams/trails                           */
} WeaponDef;
extern const WeaponDef g_weapons[NUM_WEAPONS];   /* defined in weapons.c */

/* ------------------------------------------------------------------ entities */
enum { ET_FREE=0, ET_PLAYER, ET_BOT, ET_PROJECTILE, ET_ITEM, ET_PROP };
enum { PROJ_NONE=0, PROJ_ROCKET, PROJ_GRENADE, PROJ_PLASMA, PROJ_BFG, PROJ_NAIL };
enum { IT_HEALTH=0, IT_ARMOR, IT_AMMO, IT_WEAPON, IT_MEGA };

/* World props. PROP_BARREL is spawned as a shootable ET_PROP entity (explodes);
 * PROP_TREE/PROP_LAMP are static render-only level decoration (billboard/glow).
 * PROP_CRATE is realised as a solid textured brush, not a prop entry.          */
enum { PROP_NONE=0, PROP_BARREL, PROP_TREE, PROP_LAMP, PROP_CRATE };

/* Brush surface material. MAT_THEME uses the per-level texture set; the others
 * pull a dedicated /ARENA art texture (crate/wood/sandstone/window/sand) so a
 * level can mix de_dust-style props + terrain into the normal brush geometry.  */
enum { MAT_THEME=0, MAT_CRATE, MAT_WOOD, MAT_SANDST, MAT_WINDOW, MAT_SAND };

typedef struct Entity {
    int   type;           /* ET_*                                              */
    int   alive;          /* 0 = free slot                                     */
    vec3  pos;            /* world position (feet at pos.z bottom of bbox)     */
    vec3  vel;
    float yaw, pitch;     /* view angles (radians)                             */
    int   health, armor;
    int   weapon;         /* current W_*                                       */
    int   ammo[NUM_WEAPONS];
    int   have_weapon[NUM_WEAPONS];
    int   on_ground;
    int   fire_cooldown;  /* ms remaining                                      */
    int   player_slot;    /* index into g_world.players[] (-1 if none)         */
    /* projectile fields */
    int   proj_kind;      /* PROJ_*                                            */
    int   proj_owner;     /* entity index of shooter                          */
    int   proj_life_ms;
    int   proj_weapon;    /* which W_* spawned it (for damage/splash)         */
    /* item fields */
    int   item_kind;      /* IT_*                                             */
    int   item_value;     /* amount / weapon id                              */
    int   respawn_ms;     /* item respawn timer                              */
    /* bot brain (opaque to others; bots.c owns it) */
    void *brain;
    int   prop_kind;      /* PROP_* when type==ET_PROP (barrel, etc.)          */
} Entity;

/* ------------------------------------------------------------------ map ---- */
/* Level geometry = a set of axis-aligned boxes (brushes). Solid walls/floors.
 * Simple to render (6 quads) and collide (swept AABB). mins<maxs.             */
typedef struct { vec3 mins, maxs; uint32_t rgb; int is_floor; int mat; } Brush;
typedef struct { vec3 pos; float yaw; } Spawn;
typedef struct { vec3 pos; int kind; int value; } ItemDef;
/* Static world prop (tree/lamp) or barrel-spawn marker (see PROP_*).           */
typedef struct { vec3 pos; int kind; float size; uint32_t rgb; } PropDef;

typedef struct {
    const char *name;
    Brush  brushes[MAX_BRUSHES]; int nbrush;
    Spawn  spawns[MAX_SPAWNS];   int nspawn;
    ItemDef items[MAX_ITEMS];    int nitem;
    vec3   sky_rgb_top, sky_rgb_bot;
    vec3   world_mins, world_maxs;   /* bounds for clamping / fog              */
    PropDef props[MAX_PROPS];    int nprop;   /* trees/lamps + barrel spawns    */
} Level;

/* A trace result from a ray/box sweep through the map + entities.            */
typedef struct {
    float frac;           /* 0..1 fraction of the move completed              */
    int   hit;            /* 1 if something was hit                           */
    vec3  end;            /* final position                                   */
    vec3  normal;         /* surface normal at hit                            */
    int   hit_entity;     /* entity index if an entity was hit, else -1       */
} Trace;

/* ------------------------------------------------------------------ effects */
typedef struct { vec3 pos, vel; uint32_t rgb; int life_ms; int max_life; float size; int active; } Particle;

/* ------------------------------------------------------------------ players  */
typedef struct {
    char name[24];
    int  frags;
    int  deaths;
    int  is_bot;
    int  entity;          /* current entity index (-1 if dead/respawning)     */
    int  respawn_ms;      /* >0 while waiting to respawn                       */
    uint32_t color;
} PlayerSlot;

/* ------------------------------------------------------------------ world -- */
/* GS_PAUSED/SETTINGS/CONTROLS are additive states layered over a live match:
 * the simulation is frozen (main.c only steps physics while GS_PLAYING) and the
 * menu module owns the overlay + key handling for them.                       */
enum { GS_MENU=0, GS_PLAYING, GS_SCOREBOARD, GS_GAMEOVER,
       GS_PAUSED, GS_SETTINGS, GS_CONTROLS };
typedef struct World {
    int        state;           /* GS_*                                        */
    int        level_index;
    Level      level;
    Entity     ents[MAX_ENTITIES];
    Particle   particles[MAX_PARTICLES];
    PlayerSlot players[MAX_PLAYERS]; int nplayers;
    int        local_player;    /* players[] index of the human               */
    int        frag_limit;
    int        time_ms;         /* elapsed match time                         */
    int        winner;          /* players[] index or -1                      */
    /* input snapshot for the local player, filled by main.c each frame       */
    struct { int fwd, side, up; int fire, jump; float dyaw, dpitch; int wantweap; } cmd;
} World;

extern World g_world;

/* ------------------------------------------------------------ user settings */
/* Persisted to /CONFIG/ARENA.CFG. Kept OUTSIDE World on purpose: mode_init()
 * zeroes the whole World every match, so settings must not live there or they
 * would reset on every Play. main.c owns g_arena_cfg (load/save/apply).       */
typedef struct {
    int fullscreen;   /* 0 = windowed, 1 = borderless fullscreen (maximized)   */
    int sensitivity;  /* mouse look, 1..30 (maps to ~0.0010..0.0155 rad/px);
                       * widened from 1..20, default raised 8->12 - see the
                       * g_arena_cfg init comment in main.c for why           */
    int fov;          /* horizontal field of view in degrees, 70..110          */
    int volume;       /* 0..10 (0 = mute)                                      */
    int bots;         /* default bot count for a new match, 1..15               */
    int minimap;      /* 1 = show the radar minimap overlay (toggle with M)     */
} ArenaCfg;
extern ArenaCfg g_arena_cfg;

/* Defined in main.c. Called by menu.c when a setting changes so it applies
 * live (no restart) and persists.                                             */
void arena_cfg_save(void);
void arena_apply_fullscreen(void);   /* enter/leave borderless-maximized mode  */
float arena_look_scale(void);        /* radians-per-pixel from g_arena_cfg.sens */
void  arena_start_match(void);       /* (re)start a match with current settings */

/* #491 Stage 1: GoldSrc BSP v30 import. arena_start_bsp() enters a loaded BSP
 * map as a noclip free-fly world (no box brushes, no collision this stage);
 * g_arena_noclip makes phys_step free-fly the local player. Defined in main.c. */
void arena_start_bsp(void);
extern int g_arena_noclip;

/* ------------------------------------------------------------ entity helpers */
int     ent_alloc(World *w);                 /* returns index or -1           */
void    ent_free(World *w, int idx);
Entity *ent_get(World *w, int idx);          /* NULL if free                  */

/* --------------------------------------------------------------- module API */
/* render.c (fable5) */
void r_init(int w, int h);
void r_resize(int w, int h);
void r_frame(World *world, uint32_t *blit, int stride);   /* draw full 3D scene */
void r_shutdown(void);
void r_add_tracer(vec3 a, vec3 b, uint32_t rgb);          /* beam/rail effect  */
void r_set_fov(int fov_deg);                              /* horizontal FOV     */
void r_spawn_particles(vec3 pos, uint32_t rgb, int count, float speed);
void r_explosion(vec3 pos, float radius);

/* world.c (opus) - collision + tracing against brushes + optionally entities */
void  map_set_level(World *w, const Level *lvl);
Trace map_trace_ray(World *w, vec3 start, vec3 end, int ignore_ent, int hit_ents);
/* Swept-AABB move: slides `ent` from pos by delta, resolves collisions,
 * updates ent->pos/vel/on_ground. Returns 1 if it hit something.            */
int   map_move_entity(World *w, Entity *ent, vec3 delta, float radius, float height);
int   map_point_solid(World *w, vec3 p);
/* #501: the AABB-shaped siblings of map_point_solid / map_trace_ray. They share
 * the same single-collider dispatch (box brushes, or the GoldSrc hull when
 * bsp_is_active()), so BOTH world types are answered by the SAME primitive
 * rather than by a second, parallel opinion about what is solid.
 *   map_box_solid     - does the player box, feet at `feet`, overlap solid?
 *   map_box_drop_dist - how far does that box fall before it lands? Returns
 *                       `maxdist` when nothing is under it within the cap.
 *   map_spawn_valid   - can a player actually STAND at this spawn? (both of
 *                       the above, at the position spawn_player_entity uses)  */
int   map_box_solid(World *w, vec3 feet, float radius, float height);
float map_box_drop_dist(World *w, vec3 feet, float radius, float height, float maxdist);
int   map_spawn_valid(World *w, vec3 spawn_pos);

/* main.c - append one line to the ARENA diagnostic log (/ARENA/KEYLOG.TXT) and
 * flush it. For rare, loud, must-not-be-missed events only (#501's "no valid
 * spawn"), NOT for per-frame telemetry: it does a real file write per call.  */
void  arena_log(const char *s);

/* physics.c (opus) */
void  phys_step(World *w, int dt_ms);        /* integrate all entities        */

/* weapons.c (opus) */
void  weap_fire(World *w, int shooter_idx);  /* fire shooter's current weapon */
void  weap_update(World *w, int dt_ms);      /* projectiles, cooldowns        */
void  weap_give_all(Entity *e);              /* debug/testing: full loadout   */
void  ent_damage(World *w, int victim, int attacker, int dmg, vec3 dir);

/* bots.c (opus) */
void  bot_spawn(World *w, int player_slot);
void  bot_think(World *w, int ent_idx, int dt_ms);
void  bot_free(Entity *e);

/* levels.c (opus) - fills `out` with level `index` (0..NUM_LEVELS-1)         */
void  level_load(int index, Level *out);
const char *level_name(int index);

/* modes.c (opus) - deathmatch                                                */
void  mode_init(World *w, int level_index, int num_bots);
void  mode_update(World *w, int dt_ms);      /* respawns, frag limit, winner  */
void  mode_on_kill(World *w, int victim, int attacker);
void  mode_respawn_player(World *w, int player_slot);

/* hud.c (fable5 coord) - draws over the 3D frame into the ARGB blit buffer    */
void  hud_draw(World *w, uint32_t *blit, int width, int height);
void  menu_draw(World *w, uint32_t *blit, int width, int height);
void  menu_draw_loading(uint32_t *blit, int width, int height); /* instant no-flash backdrop */
void  menu_draw_scores(World *w, uint32_t *blit, int width, int height); /* TAB overlay */
int   menu_handle_key(World *w, int key);    /* returns 1 if consumed         */
void  hud_minimap(World *w, uint32_t *blit, int width, int height); /* radar overlay */

/* sound.c (fable5 coord) */
enum { SND_SHOOT=0, SND_ROCKET, SND_EXPLODE, SND_RAIL, SND_JUMP, SND_PAIN,
       SND_DIE, SND_PICKUP, SND_HITBEEP, SND_NUM };
void  snd_init(void);
void  snd_play(int snd_id);

#endif /* ARENA_GAME_H */
