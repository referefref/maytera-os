/* Maytera Arena - levels.c
 * The 5 deathmatch arenas. Geometry is a set of axis-aligned box brushes
 * (Brush{mins,maxs,rgb,is_floor}). Coordinate system: z is UP. Player AABB is
 * radius 16, height 56, eye at +40; feet rest at pos.z. Movement code can climb
 * steps up to ~56-60 tall, so tiers/stairs use <=48 risers; taller structures
 * are reached by stacked steps or short jumps.
 *
 * Every arena is fully SEALED: a floor slab that runs under the walls, four
 * wall brushes that overlap at the corners, and a ceiling slab. No gap a player
 * can fall out of. Spawns are authored to sit on solid floor (feet at the
 * surface height) and face inward; items are scattered to create circulation
 * and map flow.
 *
 * #501: "spawns sit on solid floor" is an INTENT, not a guarantee, and this
 * comment used to assert it as fact. It was false: a later level-design pass
 * built The Longest Yard's low cover blocks and north sniper platform straight
 * on top of 4 of its 8 spawns, and nobody re-checked the ring against the new
 * geometry. Adding or moving a brush near a spawn CAN bury it, and nothing in
 * this file will stop you. The check now lives where it can actually be
 * enforced instead of asserted: map_spawn_valid() (world.c) rejects an
 * un-standable spawn at selection time, and spawn_test/ asserts every spawn of
 * every level offline on every build. If you bury one, spawn_test fails.     */
#include "game.h"

#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|((uint32_t)(b)))

/* ------------------------------------------------------------------ builders */
static void add_brush_m(Level *l, float x0,float y0,float z0,
                                  float x1,float y1,float z1,
                                  uint32_t rgb, int fl, int mat)
{
    if (l->nbrush >= MAX_BRUSHES) return;
    Brush *b = &l->brushes[l->nbrush++];
    b->mins = v3(x0,y0,z0);
    b->maxs = v3(x1,y1,z1);
    b->rgb = rgb;
    b->is_floor = fl;
    b->mat = mat;
}
static void add_brush(Level *l, float x0,float y0,float z0,
                                float x1,float y1,float z1, uint32_t rgb, int fl)
{
    add_brush_m(l, x0,y0,z0, x1,y1,z1, rgb, fl, MAT_THEME);
}

/* A solid, walk-on-top wooden crate (de_dust cover). Textured with CRATE.BMP.  */
static void add_crate(Level *l, float cx, float cy, float z0, float s, float h)
{
    add_brush_m(l, cx-s, cy-s, z0, cx+s, cy+s, z0+h, RGB(150,110,60), 1, MAT_CRATE);
}

/* A static world prop (tree / lamp) or a barrel spawn marker.                  */
static void add_prop(Level *l, float x, float y, float z,
                     int kind, float size, uint32_t rgb)
{
    if (l->nprop >= MAX_PROPS) return;
    PropDef *p = &l->props[l->nprop++];
    p->pos = v3(x,y,z);
    p->kind = kind;
    p->size = size;
    p->rgb = rgb;
}

/* A spawn at (x,y) with feet on height z, automatically facing the origin.    */
static void add_spawn(Level *l, float x, float y, float z)
{
    if (l->nspawn >= MAX_SPAWNS) return;
    Spawn *s = &l->spawns[l->nspawn++];
    s->pos = v3(x,y,z);
    s->yaw = mx_atan2f(-y, -x);   /* face toward map center (0,0)              */
}

static void add_item(Level *l, float x,float y,float z, int kind, int value)
{
    if (l->nitem >= MAX_ITEMS) return;
    ItemDef *it = &l->items[l->nitem++];
    it->pos = v3(x,y,z);
    it->kind = kind;
    it->value = value;
}

/* Build a sealed rectangular shell: floor + 4 walls + ceiling. The play area
 * is [x0,x1] x [y0,y1]; walls sit just outside it and the floor/ceiling run
 * under/over the walls so there is never a seam.                              */
static void build_shell(Level *l, float x0,float y0,float x1,float y1,
                        float floorz, float ceilz, float thk,
                        uint32_t floorc, uint32_t wallc, uint32_t ceilc)
{
    float ox0=x0-thk, oy0=y0-thk, ox1=x1+thk, oy1=y1+thk;
    add_brush(l, ox0,oy0,floorz-24, ox1,oy1,floorz, floorc, 1);      /* floor  */
    add_brush(l, ox0,oy0,floorz, x0, oy1,ceilz, wallc, 0);           /* west   */
    add_brush(l, x1, oy0,floorz, ox1,oy1,ceilz, wallc, 0);           /* east   */
    add_brush(l, ox0,oy0,floorz, ox1,y0, ceilz, wallc, 0);           /* south  */
    add_brush(l, ox0,y1, floorz, ox1,oy1,ceilz, wallc, 0);           /* north  */
    add_brush(l, ox0,oy0,ceilz,  ox1,oy1,ceilz+24, ceilc, 0);        /* ceiling*/
    l->world_mins = v3(ox0-32, oy0-32, floorz-64);
    l->world_maxs = v3(ox1+32, oy1+32, ceilz+64);
}

/* Iron Works only (Tier-1 #1): same walls/ceiling as build_shell, but the
 * floor is a rectangular frame around a rectangular hole [px0,px1]x[py0,py1],
 * plus a separate SUNKEN floor brush `pit_depth` units below, so there is a
 * real fall-through pit under the bridge instead of one solid slab. Brushes
 * are a boolean-free box model (game.h), so a "hole" has to be built as a
 * frame of pieces rather than cut out of one - this is a private helper
 * local to levels.c (not build_shell itself) specifically so the other five
 * maps' floors are untouched by this change.                                  */
static void build_shell_pit(Level *l, float x0,float y0,float x1,float y1,
                        float floorz, float ceilz, float thk,
                        uint32_t floorc, uint32_t wallc, uint32_t ceilc,
                        float px0, float px1, float py0, float py1,
                        float pit_depth)
{
    float ox0=x0-thk, oy0=y0-thk, ox1=x1+thk, oy1=y1+thk;
    /* floor frame around the pit hole (west / east / north-cap / south-cap) */
    add_brush(l, ox0,oy0,floorz-24, px0,oy1,floorz, floorc, 1);      /* west   */
    add_brush(l, px1,oy0,floorz-24, ox1,oy1,floorz, floorc, 1);      /* east   */
    add_brush(l, px0,py1,floorz-24, px1,oy1,floorz, floorc, 1);      /* n-cap  */
    add_brush(l, px0,oy0,floorz-24, px1,py0,floorz, floorc, 1);      /* s-cap  */
    /* sunken pit floor, pit_depth below the main floor */
    add_brush(l, px0,py0,floorz-pit_depth-24, px1,py1,floorz-pit_depth, floorc, 1);
    add_brush(l, ox0,oy0,floorz, x0, oy1,ceilz, wallc, 0);           /* west wall */
    add_brush(l, x1, oy0,floorz, ox1,oy1,ceilz, wallc, 0);           /* east wall */
    add_brush(l, ox0,oy0,floorz, ox1,y0, ceilz, wallc, 0);           /* south wall*/
    add_brush(l, ox0,y1, floorz, ox1,oy1,ceilz, wallc, 0);           /* north wall*/
    add_brush(l, ox0,oy0,ceilz,  ox1,oy1,ceilz+24, ceilc, 0);        /* ceiling   */
    l->world_mins = v3(ox0-32, oy0-32, floorz-pit_depth-64);
    l->world_maxs = v3(ox1+32, oy1+32, ceilz+64);
}

/* =============================================================== LEVEL NAMES */
static const char *g_level_names[NUM_LEVELS] = {
    "The Longest Yard",
    "Iron Works",
    "Dueling Keep",
    "Grand Colosseum",
    "The Spire",
    "Dust Storm",
};
const char *level_name(int index)
{
    if (index < 0 || index >= NUM_LEVELS) return "Unknown";
    return g_level_names[index];
}

/* ============================================================ LEVEL 0 ======
 * "The Longest Yard" - a wide open space-metal arena. A central stepped
 * pyramid holds the mega health; four corner pillars give cover and break up
 * the long sightlines; low blocks along the walls hide health pickups.
 *
 * Tier-1 #1 extension: was perfectly 4-fold symmetric ("big flat square with
 * a bump"), so every fight collapsed onto the pyramid. Broken into a 2-fold
 * "high side / low side" identity - north = raised sniper platform (new
 * railgun lane), south = elevated catwalk skirmish, pyramid = neutral ground
 * everyone still has to cross.
 */
static void load_longest_yard(Level *l)
{
    uint32_t floorc = RGB(60,68,80), wallc = RGB(44,52,66), ceilc = RGB(24,28,38);
    uint32_t accent = RGB(90,150,160);
    build_shell(l, -512,-512, 512,512, 0.0f, 336.0f, 24.0f, floorc, wallc, ceilc);
    l->sky_rgb_top = v3(0.05f,0.06f,0.12f);
    l->sky_rgb_bot = v3(0.18f,0.10f,0.22f);

    /* central stepped pyramid (two climbable tiers)                          */
    add_brush(l, -192,-192,0, 192,192,48, accent, 1);   /* tier 1 (top 48)    */
    add_brush(l, -112,-112,48, 112,112,96, RGB(120,180,190), 1); /* tier 2 top96 */

    /* four corner cover pillars                                              */
    float px = 336;
    add_brush(l, -px-28,-px-28,0, -px+28,-px+28,208, RGB(70,80,96), 0);
    add_brush(l,  px-28,-px-28,0,  px+28,-px+28,208, RGB(70,80,96), 0);
    add_brush(l, -px-28, px-28,0, -px+28, px+28,208, RGB(70,80,96), 0);
    add_brush(l,  px-28, px-28,0,  px+28, px+28,208, RGB(70,80,96), 0);

    /* low cover blocks near the middle of the south/east/west walls (north's
     * was replaced by the new platform below, see the "north side" block).   */
    add_brush(l, -56,-460,0, 56,-404,56, RGB(80,90,104), 1);           /* south */
    /* east/west blocks: keep the original base, add a stacked 2nd tier
     * (top 104) so they double as sniper-denial cover, not just crouch cover.*/
    add_brush(l, -460,-56,0, -404,56,56, RGB(80,90,104), 1);           /* west base */
    add_brush(l, -444,-32,56, -420,32,104, RGB(96,106,120), 1);        /* west top2 */
    add_brush(l,  404,-56,0,  460,56,56, RGB(80,90,104), 1);           /* east base */
    add_brush(l,  420,-32,56,  444,32,104, RGB(96,106,120), 1);        /* east top2 */

    /* --- north side: raised sniper platform (new railgun lane) -------------
     * two steps up from the pyramid's tier-1 (top48 -> top88 -> top112),
     * each delta <=48 so it climbs the same way the existing staircases do.  */
    add_brush(l, -48,192,0, 48,260,88,  accent, 1);                    /* riser */
    add_brush(l, -48,260,0, 48,508,112, RGB(120,180,190), 1);          /* platform top112 */

    /* --- south side: two floating catwalk fragments (z160-180), open
     * underside, bridging each south pillar toward the middle with a gap
     * between them so crossing is a deliberate risk, not a free walk.        */
    add_brush(l, -308,-372,160, -40,-356,180, RGB(90,150,160), 1);     /* SW fragment */
    add_brush(l,   40,-372,160, 308,-356,180, RGB(90,150,160), 1);     /* SE fragment */

    /* spawns (8) around the ring, facing center                             */
    add_spawn(l, -400,-400,0); add_spawn(l, 400,-400,0);
    add_spawn(l, -400, 400,0); add_spawn(l, 400, 400,0);
    add_spawn(l,    0,-420,0); add_spawn(l,   0, 420,0);
    add_spawn(l, -420,   0,0); add_spawn(l, 420,   0,0);

    /* items: mega on the pyramid top, armor on tier1, weapons + ammo spread  */
    add_item(l,   0,  0, 96, IT_MEGA,   100);
    add_item(l, -150,-150,48, IT_ARMOR,  50);
    add_item(l,  150, 150,48, IT_ARMOR,  50);
    add_item(l,   0,  0, 48, IT_WEAPON, W_ROCKET);
    add_item(l,    0,390,112, IT_WEAPON, W_RAILGUN);   /* moved: new north platform */
    add_item(l,  336, 336,208, IT_WEAPON, W_LIGHTNING);
    add_item(l,  336,-336,208, IT_WEAPON, W_SHOTGUN);
    add_item(l,    0,-432,56, IT_HEALTH, 25);
    add_item(l,    0, 432,56, IT_HEALTH, 25);
    add_item(l, -432,   0,56, IT_HEALTH, 25);
    add_item(l,  432,   0,56, IT_AMMO,   60);
    add_item(l, -336, 336,208, IT_AMMO,  60);
    /* Tier-1 #3 pickups retune: small tiers near spawns + south catwalk risk/reward */
    add_item(l, -170,-364,180, IT_ARMOR,  50);         /* 2nd armor, south catwalk */
    add_item(l, -170,-364,  0, IT_AMMO,   60);          /* under the SW catwalk    */
    add_item(l,  170,-364,  0, IT_AMMO,   60);          /* under the SE catwalk    */
    add_item(l, -336,-336,208, IT_ARMOR,   5);          /* small: pillar vacated by railgun move */
    add_item(l, -400,-370,  0, IT_HEALTH,  5);
    add_item(l,  400,-370,  0, IT_HEALTH,  5);
    add_item(l, -400, 370,  0, IT_ARMOR,   5);
    add_item(l,  400, 370,  0, IT_ARMOR,   5);
}

/* ============================================================ LEVEL 1 ======
 * "Iron Works" - an industrial box with an upper catwalk RING at z=144 and a
 * central bridge crossing an open pit. You reach the ring by a corner
 * staircase. Crates on the floor give ground-level cover. Dark metal + rust.
 *
 * Tier-1 #1 extension: the ring only had one way up (SW stairs), so it was
 * trivially lockdown-camped, and the "pit" under the bridge was flat z=0
 * floor - no actual hazard. Added: a mirrored NE staircase (second access
 * point) and a real sunken pit (z=-64) under the bridge with chained barrels,
 * so falling/being knocked in is now a punishing risk/reward pull.
 */
static void load_iron_works(Level *l)
{
    uint32_t floorc = RGB(48,46,44), wallc = RGB(58,52,46), ceilc = RGB(28,26,24);
    uint32_t metal  = RGB(88,78,60), rust = RGB(120,74,40);
    /* Sunken pit directly under the bridge (x -64..64), 64 units deep, so
     * there is a real fall-through instead of decorative open floor.         */
    build_shell_pit(l, -448,-448, 448,448, 0.0f, 352.0f, 24.0f, floorc, wallc, ceilc,
                     -64.0f, 64.0f, -400.0f, 400.0f, 64.0f);
    l->sky_rgb_top = v3(0.10f,0.09f,0.08f);
    l->sky_rgb_bot = v3(0.22f,0.14f,0.07f);

    /* upper catwalk ring (thin slabs, top 144, bottom 120 -> walk underneath) */
    uint32_t ck = RGB(96,90,78);
    add_brush(l, -400, 300,120, 400,400,144, ck, 1);   /* north catwalk       */
    add_brush(l, -400,-400,120, 400,-300,144, ck, 1);  /* south catwalk       */
    add_brush(l, -400,-300,120,-300,300,144, ck, 1);   /* west catwalk        */
    add_brush(l,  300,-300,120, 400,300,144, ck, 1);   /* east catwalk        */
    /* central bridge across the pit                                          */
    add_brush(l,  -40,-400,120,  40,400,144, rust, 1);

    /* SW corner staircase up to the ring (48-unit risers, existing)          */
    add_brush(l, -400,-300,0,-300,-220,48,  metal, 1);
    add_brush(l, -400,-220,0,-300,-140,96,  metal, 1);
    add_brush(l, -400,-140,0,-300, -60,144, metal, 1);  /* meets west catwalk */

    /* NE corner staircase (mirrored) - second way up, ends the single-
     * chokepoint lockdown. Meets the east catwalk.                           */
    add_brush(l,  300,220,0, 400,300, 48, metal, 1);
    add_brush(l,  300,140,0, 400,220, 96, metal, 1);
    add_brush(l,  300, 60,0, 400,140,144, metal, 1);   /* meets east catwalk  */

    /* pit edge "DANGER" warning-stripe markers (rust tint; uses the T1W1.BMP
     * hazard-stripe wall variant already baked for this theme when the
     * brush's texture-variant slot lands on it - see CREDITS.TXT). A low
     * curb, not a wall: auto-steppable, just a readable hazard cue.          */
    add_brush(l, -68,-400,0, -64,400,12, rust, 1);      /* west pit edge      */
    add_brush(l,  64,-400,0,  68,400,12, rust, 1);      /* east pit edge      */

    /* ground crates for cover                                                */
    add_brush(l,  120,120,0, 200,200,48, rust, 1);
    add_brush(l, -200,-200,0,-120,-120,72, rust, 1);
    add_brush(l,  120,-200,0,200,-120,48, metal, 1);
    add_brush(l, -200,120,0,-120,200,48, metal, 1);

    /* spawns (8) on the floor                                               */
    add_spawn(l, -360,-360,0); add_spawn(l, 360,-360,0);
    add_spawn(l, -360, 360,0); add_spawn(l, 360, 360,0);
    add_spawn(l,    0,-360,0); add_spawn(l,   0, 360,0);
    add_spawn(l, -360,   0,0); add_spawn(l, 360,   0,0);

    /* items */
    add_item(l,   0,  0,144, IT_MEGA,  100);           /* on the bridge       */
    add_item(l, -350, 350,144, IT_WEAPON, W_RAILGUN);  /* NW catwalk          */
    add_item(l,  350, 350,144, IT_WEAPON, W_ROCKET);   /* NE catwalk          */
    add_item(l, -350,-350,144, IT_AMMO,   60);
    add_item(l,  160,160,48, IT_WEAPON, W_SHOTGUN);
    add_item(l, -160,-160,72, IT_WEAPON, W_PLASMA);
    add_item(l,  160,-160,48, IT_HEALTH, 25);
    add_item(l, -160,160,48, IT_HEALTH, 25);
    add_item(l,    0,-360,0, IT_HEALTH, 25);
    add_item(l,    0, 360,0, IT_AMMO,   60);
    add_item(l, -360,   0,0, IT_ARMOR,  25);
    add_item(l,  350,100,144, IT_WEAPON, W_LIGHTNING); /* new: NE stair landing, makes the route worth taking */
    /* Tier-1 #2/#3: armor moved DOWN into the pit as a risk pull, plus an
     * ammo cache; 2 barrels close enough to chain (see props below).         */
    add_item(l,   40,  0,-64, IT_ARMOR,  50);          /* moved from (350,-350,144) */
    add_item(l,  -40,  0,-64, IT_AMMO,   60);
    add_item(l,  360,-360,  0, IT_HEALTH,  5);
    add_item(l, -360, 360,  0, IT_HEALTH,  5);

    /* 2 explosive barrels down in the pit, close enough (60 units, well under
     * the 160-unit splash radius) to chain - punishes camping the bottom.    */
    add_prop(l,   0,-40,-64, PROP_BARREL, 44, RGB(200,60,40));
    add_prop(l,   0, 20,-64, PROP_BARREL, 44, RGB(200,60,40));
}

/* ============================================================ LEVEL 2 ======
 * "Dueling Keep" - a tight 1v1 stone room. A single tall central pillar forces
 * constant circling; four knee-high blocks give micro-cover; two corner
 * pedestals hold the power weapons. Small, fast, brutal.
 *
 * Tier-1 #1 extension: this map is already the tightest/most-focused in the
 * set, so it deliberately stays small - ONE vertical wrinkle (a jump-only
 * ledge on the pillar holding the mega) rather than more floor space, plus a
 * small pedestal tweak so standing on one isn't a free target from every
 * angle. No new floor footprint, no new spawns - that is the point.
 *
 * SPEC DISCREPANCY (reported, not silently fixed): the spec's map-1 table
 * says "Dueling Keep: railgun + rocket only (already true)", but the
 * pre-existing item list here also has a W_LIGHTNING pickup at (236,-236,0).
 * Left as-is per "implement its design, don't redesign" - removing the
 * lightning gun was never actually instructed, the spec's claim was just
 * inaccurate. Flagged in the task report.
 */
static void load_dueling_keep(Level *l)
{
    uint32_t floorc = RGB(78,74,66), wallc = RGB(66,62,56), ceilc = RGB(40,38,34);
    uint32_t stone  = RGB(96,92,84);
    build_shell(l, -288,-288, 288,288, 0.0f, 288.0f, 24.0f, floorc, wallc, ceilc);
    l->sky_rgb_top = v3(0.12f,0.12f,0.14f);
    l->sky_rgb_bot = v3(0.20f,0.20f,0.22f);

    /* central pillar */
    add_brush(l, -64,-64,0, 64,64,220, stone, 0);

    /* Jump-only ledge wrapping half the pillar (north face + partial
     * east/west wrap), top z=96, reached by a jump off a knee-block toward
     * the pillar - no stairs, deliberately the one skill-testing spot on the
     * map. Holds the megahealth (moved off the floor). Bots cannot reach
     * this (no strafe/skill-jump AI, per spec's own bot-navigability rule) -
     * that is accepted/intended here, same as the spec calls out.           */
    add_brush(l, -64, 64,64,  64, 96,96, RGB(112,104,150), 1);  /* north ledge */
    add_brush(l,  64, 32,64,  96, 64,96, RGB(112,104,150), 1);  /* NE wrap     */
    add_brush(l, -96, 32,64, -64, 64,96, RGB(112,104,150), 1);  /* NW wrap     */

    /* knee-high cover blocks */
    add_brush(l,  150,-40,0, 210,40,56, stone, 1);
    add_brush(l, -210,-40,0,-150,40,56, stone, 1);
    add_brush(l,  -40,150,0, 40,210,56, stone, 1);
    add_brush(l,  -40,-210,0,40,-150,56, stone, 1);

    /* corner pedestals: top 56 -> top 72, plus a knee-block beside each so
     * standing on one isn't a free target from every angle in the room.      */
    add_brush(l,  200,200,0, 272,272,72, RGB(110,104,92), 1);
    add_brush(l, -272,-272,0,-200,-200,72, RGB(110,104,92), 1);
    add_brush(l,  272,180,0, 320,220,40, stone, 1);             /* NE knee-block */
    add_brush(l, -320,-220,0,-272,-180,40, stone, 1);            /* SW knee-block */

    /* spawns (6) */
    add_spawn(l, -220,   0,0); add_spawn(l, 220,   0,0);
    add_spawn(l,    0,-220,0); add_spawn(l,   0, 220,0);
    add_spawn(l, -160, 160,0); add_spawn(l, 160,-160,0);

    /* items */
    add_item(l,  236,236,72, IT_WEAPON, W_RAILGUN);    /* pedestal raised to top72 */
    add_item(l, -236,-236,72, IT_WEAPON, W_ROCKET);
    add_item(l,    0, 80,96, IT_MEGA,   100);          /* moved: north ledge, jump-only */
    add_item(l,  180,   0,56, IT_HEALTH, 25);
    add_item(l, -180,   0,56, IT_HEALTH, 25);
    add_item(l,    0, 180,56, IT_ARMOR,  50);
    add_item(l,    0,-180,56, IT_AMMO,   60);
    add_item(l,  236,-236,0, IT_WEAPON, W_LIGHTNING);
    /* Tier-1 #3: small pickups near spawns */
    add_item(l, -160, 160,0, IT_HEALTH,  5);
    add_item(l,  160,-160,0, IT_ARMOR,   5);
}

/* ============================================================ LEVEL 3 ======
 * "Grand Colosseum" - a huge sunlit sand arena ringed by two tiers of stepped
 * "seating" that rise toward the walls (great for high-ground duels). The
 * center is an open pit with a low dais holding the mega. Long sightlines for
 * railguns, cover pillars to break them.
 *
 * Tier-1 #1 extension: the seating ring was purely cosmetic (no reason to
 * prefer tier1 vs tier2) and had zero connection to the dais except walking
 * all the way around. Cut two "vomitory" ramps (east/west) through tier-2 -
 * a 24-unit gap filled with 3 stacked steps (32/64/96, each <=48 so it's
 * bot-navigable) - and bridged those quadrants' cover pillars at a new
 * top-96 landing, adding a third combat layer between "ring" and "pit
 * floor". North/south stay pure sniper perches (untouched full rings).
 */
static void load_colosseum(Level *l)
{
    uint32_t floorc = RGB(150,132,96), wallc = RGB(120,104,72), ceilc = RGB(90,120,150);
    uint32_t seat1  = RGB(166,148,110), seat2 = RGB(182,164,124);
    build_shell(l, -576,-576, 576,576, 0.0f, 384.0f, 24.0f, floorc, wallc, ceilc);
    l->sky_rgb_top = v3(0.35f,0.55f,0.85f);
    l->sky_rgb_bot = v3(0.75f,0.72f,0.60f);

    /* tier 1 seating ring (top 48): frame between 448..576 - untouched       */
    add_brush(l, -576, 448,0, 576,576,48, seat1, 1);
    add_brush(l, -576,-576,0, 576,-448,48, seat1, 1);
    add_brush(l, -576,-448,0,-448,448,48, seat1, 1);
    add_brush(l,  448,-448,0, 576,448,48, seat1, 1);

    /* tier 2 seating ring (top 96): north/south untouched sniper perches     */
    add_brush(l, -448, 320,0, 448,448,96, seat2, 1);   /* north */
    add_brush(l, -448,-448,0, 448,-320,96, seat2, 1);  /* south */

    /* tier 2 east: 24-wide vomitory gap at y-12..12, 3 stacked risers
     * (32/64/96) climbing from the pit up to tier-2 top, flanked by the
     * remaining ring on either side.                                        */
    add_brush(l,  320, 12,0, 448,320,96, seat2, 1);    /* east, north remainder */
    add_brush(l,  320,-320,0, 448,-12,96, seat2, 1);   /* east, south remainder */
    add_brush(l,  320,-12,0, 368, 12, 32, seat2, 1);    /* east vomitory step 1 */
    add_brush(l,  368,-12,0, 408, 12, 64, seat2, 1);    /* east vomitory step 2 */
    add_brush(l,  408,-12,0, 448, 12, 96, seat2, 1);    /* east vomitory step 3 */

    /* tier 2 west: mirrored vomitory gap                                    */
    add_brush(l, -448, 12,0,-320,320,96, seat2, 1);    /* west, north remainder */
    add_brush(l, -448,-320,0,-320,-12,96, seat2, 1);   /* west, south remainder */
    add_brush(l, -368,-12,0,-320, 12, 32, seat2, 1);    /* west vomitory step 1 */
    add_brush(l, -408,-12,0,-368, 12, 64, seat2, 1);    /* west vomitory step 2 */
    add_brush(l, -448,-12,0,-408, 12, 96, seat2, 1);    /* west vomitory step 3 */

    /* central dais + four cover pillars around it                           */
    add_brush(l, -96,-96,0, 96,96,32, RGB(196,178,140), 1);
    add_brush(l,  200-20,-20,0, 200+20,20,180, wallc, 0);   /* east pillar */
    add_brush(l, -200-20,-20,0,-200+20,20,180, wallc, 0);   /* west pillar */
    add_brush(l,  -20,200-20,0, 20,200+20,180, wallc, 0);   /* north pillar (unchanged) */
    add_brush(l,  -20,-200-20,0,20,-200+20,180, wallc, 0);  /* south pillar (unchanged) */

    /* East/west pillar-top landings (cap at 96) + short bridges from the new
     * tier-2 vomitory top out to them - a mid-height combat layer between
     * "ring" and "pit floor" without dropping to the sand.                   */
    add_brush(l,  160,-40,80,  240, 40, 96, wallc, 1);      /* east pillar landing */
    add_brush(l, -240,-40,80, -160, 40, 96, wallc, 1);      /* west pillar landing */
    add_brush(l,  240,-12,80,  320, 12, 96, RGB(196,178,140), 1); /* east bridge */
    add_brush(l, -320,-12,80, -240, 12, 96, RGB(196,178,140), 1); /* west bridge */

    /* spawns (8) - mix of pit floor and upper seating                       */
    add_spawn(l, -300,-300,0); add_spawn(l, 300,-300,0);
    add_spawn(l, -300, 300,0); add_spawn(l, 300, 300,0);
    add_spawn(l, -500,   0,96); add_spawn(l, 500,   0,96);
    add_spawn(l,    0,-500,96); add_spawn(l,   0, 500,96);

    /* items */
    add_item(l,   0,  0,32, IT_MEGA,   100);
    add_item(l,  200,  0,96, IT_WEAPON, W_RAILGUN);    /* moved: east pillar landing, rewards the new bridge */
    add_item(l, -512,-512,48, IT_WEAPON, W_BFG);
    add_item(l,  512,-512,48, IT_WEAPON, W_ROCKET);
    add_item(l, -512, 512,48, IT_WEAPON, W_LIGHTNING);
    add_item(l,  512, 512,48, IT_AMMO,   60);          /* corner spot vacated by the railgun move */
    add_item(l,    0, 384,96, IT_ARMOR,  50);
    add_item(l,    0,-384,96, IT_ARMOR,  50);
    add_item(l,  384,   0,96, IT_HEALTH, 25);
    add_item(l, -384,   0,96, IT_HEALTH, 25);
    add_item(l,  200,   0,0, IT_HEALTH, 25);
    add_item(l, -200,   0,0, IT_AMMO,   60);
    add_item(l,    0, 200,0, IT_AMMO,   60);
    add_item(l,    0,-200,0, IT_WEAPON, W_SHOTGUN);
    /* Tier-1 #3: health under each new vomitory ramp, small pickups          */
    add_item(l,  392,   0,0, IT_HEALTH, 25);
    add_item(l, -392,   0,0, IT_HEALTH, 25);
    add_item(l,  300,-300,0, IT_HEALTH,  5);
    add_item(l, -300, 300,0, IT_ARMOR,   5);
}

/* ============================================================ LEVEL 4 ======
 * "The Spire" - a vertical tower. A staircase climbs one wall to the top, two
 * mid-height tower platforms branch off it, and a top bridge crosses to the
 * central column that carries the mega. Fall damage / rocket-jumps make the
 * vertical fights spicy. Deep blue / violet.
 *
 * Tier-1 #1 extension: was a single continuous critical path (floor->west
 * stairs->top), so holding the top of the stairs denied 100% of vertical
 * access. Added a second, shorter east staircase (4x48 risers) landing on
 * the east tower, plus a connector step from the NE tower up to the top
 * landing, closing the vertical route into a loop: floor -> east stairs ->
 * east tower -> NE tower -> connector -> top landing -> down the original
 * west staircase, without passing through the same chokepoint twice.
 * Every new riser is <=48 (bot-navigable via straight-line probes); the
 * loop is walkable by a plain player, strafe-jumping (once #484 lands) is
 * an optional speed edge on the west-vs-east route choice, never required.
 */
static void load_spire(Level *l)
{
    uint32_t floorc = RGB(40,42,64), wallc = RGB(34,36,56), ceilc = RGB(20,20,34);
    uint32_t plat   = RGB(72,64,110), accent = RGB(120,90,170);
    build_shell(l, -384,-384, 384,384, 0.0f, 480.0f, 24.0f, floorc, wallc, ceilc);
    l->sky_rgb_top = v3(0.06f,0.05f,0.14f);
    l->sky_rgb_bot = v3(0.20f,0.10f,0.30f);

    /* central column carrying the mega up top */
    add_brush(l, -44,-44,0, 44,44,392, accent, 0);

    /* staircase up the west wall, rising along +y in 48-unit risers         */
    for (int k = 0; k < 8; k++) {
        float y0 = -360 + k*90.0f;
        add_brush(l, -360, y0, 0, -240, y0+90, 48.0f*(k+1), plat, 1);
    }
    /* top landing (z 384) spanning the north end, meets the top stair       */
    add_brush(l, -360, 240,0, 240,360,384, plat, 1);
    /* top bridge from the landing across to the central column (top 384)     */
    add_brush(l,  -40, 40,360, 40,300,384, accent, 1);

    /* two mid tower platforms branching off the staircase (solid, reachable
     * by a step across from the adjacent stair riser)                        */
    add_brush(l,  120,-120,0, 300,120,192, plat, 1);   /* east tower top 192  */
    add_brush(l,  120, 160,0, 300,340,288, plat, 1);   /* NE tower top 288    */

    /* NEW: second, shorter east-wall staircase - 4 stacked 48-risers from
     * z=0 to z=192, landing directly on the east tower. Independent ground-
     * up route (was previously a dead-end branch off the west stairs only). */
    add_brush(l, 300,-120,0, 376, -60, 48,  plat, 1);
    add_brush(l, 300, -60,0, 376,   0, 96,  plat, 1);
    add_brush(l, 300,   0,0, 376,  60,144,  plat, 1);
    add_brush(l, 300,  60,0, 376, 120,192,  plat, 1);  /* meets east tower    */

    /* NEW: NE-tower -> top-landing connector. NE tower's top (288) and the
     * top landing (384) are 96 apart; a single intermediate step (top 336,
     * delta <=48 on both sides) closes the gap via two auto-step climbs
     * instead of one un-navigable sheer jump, so bots can complete the loop
     * the same way they climb the existing staircases.                      */
    add_brush(l, 160,200,0, 260,240,336, accent, 1);

    /* spawns (7): floor + a couple on platforms                             */
    add_spawn(l, -300,-300,0); add_spawn(l, 300,-300,0);
    add_spawn(l,  300, 300,0); add_spawn(l,   0,-300,0);
    add_spawn(l, -300,   0,0);
    add_spawn(l,  210,   0,192);   /* on east tower                          */
    add_spawn(l,    0, 300,384);   /* on top landing                         */

    /* items: mega on the top bridge, weapons climbing the tower             */
    add_item(l,   0,  0,392, IT_MEGA,   100);          /* top of column       */
    add_item(l,   0, 200,384, IT_WEAPON, W_RAILGUN);   /* top landing         */
    add_item(l, 210, 250,288, IT_WEAPON, W_ROCKET);    /* NE tower            */
    add_item(l, 210,   0,192, IT_ARMOR,  50);          /* east tower          */
    add_item(l, -300,-180,144, IT_WEAPON, W_LIGHTNING);/* on the staircase    */
    add_item(l, -300, 0,240, IT_AMMO,   60);
    add_item(l,  300,-300,0, IT_WEAPON, W_PLASMA);
    add_item(l, -300, 300,0, IT_WEAPON, W_SHOTGUN);
    add_item(l,    0,-300,0, IT_HEALTH, 25);
    add_item(l,  300, 300,0, IT_HEALTH, 25);
    add_item(l, -300,   0,0, IT_ARMOR,  25);
    add_item(l,  210, 300,288, IT_HEALTH, 25);
    /* Tier-1 #3: small pickup + ammo at the base of the new east stair       */
    add_item(l,  340,-150,0, IT_HEALTH,  5);
    add_item(l,  340,-150,0, IT_AMMO,   30);
}

/* ============================================================ LEVEL 5 ======
 * "Dust Storm" - an outdoor Counter-Strike de_dust-style desert map. Sand
 * ground, sandstone perimeter + buildings with window insets, stacked wooden
 * crates for cover, explosive barrels, palm trees around the rim and lamp posts
 * for lighting. Terrain + props use the /ARENA art textures (MAT_*), so the
 * whole thing reads as a real map, not the flat-shaded programmer boxes.
 *
 * Tier-1 #1 extension: the two buildings were solid masses you could only
 * climb onto (the MAT_WINDOW insets were dark-tinted decoration, not real
 * openings), the divider wall had no gap so the two halves only connected
 * around the far edges, and nothing broke the long NW-bunker<->SE-tower
 * diagonal railgun lane. Fixed all three: a real doorway hollows the NW
 * bunker into an enterable room, the divider is now two shorter (56) wall
 * sections with a 24-unit vaulting gap between them, and 2 more trees + a
 * center crate cluster break the diagonal. This is the one map that leans
 * into "real interiors" rather than open desert - see spec.
 */
static void load_dust(Level *l)
{
    uint32_t sand = RGB(196,170,120), stone = RGB(170,146,104);
    uint32_t skyc = RGB(150,180,210);
    float X = 640, ceil = 512, thk = 32;

    /* sand floor slab (runs under the walls) - MAT_SAND                       */
    add_brush_m(l, -X-thk,-X-thk,-24, X+thk,X+thk,0, sand, 1, MAT_SAND);
    /* four sandstone perimeter walls - MAT_SANDST                             */
    add_brush_m(l, -X-thk,-X-thk,0, -X, X+thk,ceil, stone, 0, MAT_SANDST);   /* W */
    add_brush_m(l,  X, -X-thk,0,  X+thk,X+thk,ceil, stone, 0, MAT_SANDST);   /* E */
    add_brush_m(l, -X-thk,-X-thk,0,  X+thk,-X, ceil, stone, 0, MAT_SANDST);  /* S */
    add_brush_m(l, -X-thk, X, 0,  X+thk,X+thk,ceil, stone, 0, MAT_SANDST);   /* N */
    /* high sky ceiling (keeps the arena sealed; sky panorama dominates view)  */
    add_brush_m(l, -X-thk,-X-thk,ceil, X+thk,X+thk,ceil+24, skyc, 0, MAT_THEME);
    l->sky_rgb_top = v3(0.30f,0.45f,0.72f);
    l->sky_rgb_bot = v3(0.80f,0.72f,0.52f);
    l->world_mins = v3(-X-thk-32, -X-thk-32, -64);
    l->world_maxs = v3( X+thk+32,  X+thk+32, ceil+64);

    /* --- NW bunker: was one solid mass, now a hollow room with a real
     * doorway (64 wide x 96 tall) cut into the south face, so it is
     * enterable instead of only climbable-on-top-of. Walls 20 thick, roof
     * at the same 176 top the solid block used to have.                      */
    add_brush_m(l, -420,240,0, -400,440,176, stone, 0, MAT_SANDST);   /* west wall  */
    add_brush_m(l, -240,240,0, -220,440,176, stone, 0, MAT_SANDST);   /* east wall  */
    add_brush_m(l, -420,420,0, -220,440,176, stone, 0, MAT_SANDST);   /* north wall */
    add_brush_m(l, -420,240,0, -352,260,176, stone, 0, MAT_SANDST);   /* south wall, west of door */
    add_brush_m(l, -288,240,0, -220,260,176, stone, 0, MAT_SANDST);   /* south wall, east of door */
    add_brush_m(l, -352,240,96, -288,260,176, stone, 0, MAT_SANDST);  /* lintel above the door     */
    add_brush_m(l, -420,240,156, -220,440,176, stone, 0, MAT_SANDST); /* roof                       */
    add_brush_m(l, -420-1,320,60, -420+1,380,120, RGB(60,70,90), 0, MAT_WINDOW); /* west window (moved off the new door) */
    add_brush_m(l, -300,240-1,60, -240,240+1,120, RGB(60,70,90), 0, MAT_WINDOW); /* east-of-door window, kept */
    /* SE tower: taller, holds the mega up top                                 */
    add_brush_m(l, 240,-440,0, 440,-240,224, stone, 0, MAT_SANDST);
    add_brush_m(l, 240-1,-400,80, 240+1,-320,150, RGB(60,70,90), 0, MAT_WINDOW); /* W window */
    /* Divider wall: was one 120-tall block (single-lane bottleneck). Now two
     * 56-tall sections with a 24-unit gap between - low enough to hop over
     * from either side, plus a real notch to walk straight through.          */
    add_brush_m(l, -40,-260,0, 40, -12,56, stone, 0, MAT_SANDST);     /* south section */
    add_brush_m(l, -40, 12,0, 40, 260,56, stone, 0, MAT_SANDST);      /* north section */

    /* --- crate stacks (walk-on-top cover) ---------------------------------- */
    add_crate(l, -260,180,0, 40, 60);                 /* stair up to NW bunker  */
    add_crate(l, -300,120,0, 40, 120);
    add_crate(l,  150,150,0, 44, 64);
    add_crate(l,  150,150,64, 40, 56);                /* stacked                */
    add_crate(l, -160,-200,0, 44, 64);
    add_crate(l,  340,300,0, 44, 64);
    add_crate(l,  360,-140,0, 40, 60);                /* stair up to SE tower   */
    add_crate(l,  320,-200,0, 40, 120);
    /* NEW: third small crate cluster near center, breaks the NW-bunker <->
     * SE-tower diagonal railgun lane along with the 2 new trees below.       */
    add_crate(l, -60,-60,0, 40, 60);
    add_crate(l, -60,-60,60, 36, 50);                 /* stacked                */

    /* --- explosive barrels: re-spaced into 3 chaining pairs (each pair
     * well under the 160-unit splash radius), instead of the original
     * spread where only one pair happened to be close enough to chain.       */
    add_prop(l, -300,180,0,  PROP_BARREL, 44, RGB(200,60,40));  /* NW bunker area */
    add_prop(l, -260,205,0,  PROP_BARREL, 44, RGB(200,60,40));  /* pairs with above (dist ~48) */
    add_prop(l,  150,206,0,  PROP_BARREL, 44, RGB(200,60,40));  /* center crates  */
    add_prop(l,  190,150,0,  PROP_BARREL, 44, RGB(200,60,40));  /* pairs with above (dist ~69) */
    add_prop(l,  330,250,0,  PROP_BARREL, 44, RGB(200,60,40));  /* SE tower area  */
    add_prop(l,  300,210,0,  PROP_BARREL, 44, RGB(200,60,40));  /* pairs with above (dist ~50) */

    /* --- palm trees around the rim (billboards), + 2 new along the
     * NW-bunker<->SE-tower diagonal to break that sightline.                 */
    add_prop(l, -560,-560,0, PROP_TREE, 220, RGB(90,140,70));
    add_prop(l,  560,-560,0, PROP_TREE, 220, RGB(90,140,70));
    add_prop(l, -560, 560,0, PROP_TREE, 220, RGB(90,140,70));
    add_prop(l,  560, 560,0, PROP_TREE, 220, RGB(90,140,70));
    add_prop(l,    0,-580,0, PROP_TREE, 200, RGB(90,140,70));
    add_prop(l,  520,   0,0, PROP_TREE, 200, RGB(90,140,70));
    add_prop(l, -140, 120,0, PROP_TREE, 180, RGB(90,140,70));   /* NEW: diagonal break */
    add_prop(l,  140,-120,0, PROP_TREE, 180, RGB(90,140,70));   /* NEW: diagonal break */

    /* --- lamp posts (warm glow) -------------------------------------------- */
    add_prop(l, -200,-200,0, PROP_LAMP, 120, RGB(255,210,140));
    add_prop(l,  200, 200,0, PROP_LAMP, 120, RGB(255,210,140));
    add_prop(l, -420, 120,0, PROP_LAMP, 120, RGB(255,210,140));
    add_prop(l,  420,-120,0, PROP_LAMP, 120, RGB(255,210,140));

    /* spawns (8) around the sand                                             */
    add_spawn(l, -520,-520,0); add_spawn(l, 520,-520,0);
    add_spawn(l, -520, 520,0); add_spawn(l, 520, 520,0);
    add_spawn(l,    0,-520,0); add_spawn(l,   0, 520,0);
    add_spawn(l, -520,   0,0); add_spawn(l, 520,   0,0);

    /* items */
    add_item(l,  340,-340,224, IT_MEGA,   100);        /* SE tower top          */
    add_item(l, -320,340,176, IT_WEAPON, W_ROCKET);    /* NW bunker top         */
    add_item(l,  340,-340,224, IT_WEAPON, W_RAILGUN);  /* reserved for the long diagonal */
    add_item(l,    0, 100, 56, IT_WEAPON, W_MACHINEGUN);/* moved off the divider (now shorter); mid-range bias */
    add_item(l,  150,150,120, IT_ARMOR,  50);          /* atop the crate stack  */
    add_item(l, -300,120,120, IT_HEALTH, 25);
    add_item(l,  520,   0,0, IT_WEAPON, W_SHOTGUN);
    add_item(l, -520,   0,0, IT_WEAPON, W_PLASMA);
    add_item(l,    0,-520,0, IT_HEALTH, 25);
    add_item(l,    0, 520,0, IT_AMMO,   60);
    add_item(l,  520, 520,0, IT_ARMOR,  25);
    add_item(l, -520,-520,0, IT_AMMO,   60);
    /* Tier-1 #3: NW bunker interior cache (a reason to go in), + small pickups */
    add_item(l, -320,320,  0, IT_HEALTH, 25);
    add_item(l, -320,360,  0, IT_AMMO,   60);
    add_item(l,    0,-100, 56, IT_HEALTH,  5);
    add_item(l,    0, 100,  0, IT_ARMOR,   5);
}

/* ------------------------------------------------------------------ dispatch */
void level_load(int index, Level *out)
{
    if (!out) return;
    /* zero the level so counts / arrays start clean                          */
    for (unsigned i = 0; i < sizeof(*out); i++) ((unsigned char*)out)[i] = 0;
    out->nbrush = out->nspawn = out->nitem = 0;

    switch (index) {
    case 0:  load_longest_yard(out); break;
    case 1:  load_iron_works(out);   break;
    case 2:  load_dueling_keep(out); break;
    case 3:  load_colosseum(out);    break;
    case 4:  load_spire(out);        break;
    case 5:  load_dust(out);         break;
    default: load_longest_yard(out); index = 0; break;
    }
    out->name = level_name(index);
}
