/* Maytera Arena - main loop, window, GL bring-up, input, entity pool.
 * Owns g_world. Renders with TinyGL (render.c) into an ARGB buffer pushed to
 * the compositor via SYS_WIN_BLIT (same path as glcube). (#320-style FPS)     */
#include "game.h"
#include "stdlib.h"   /* O_RDONLY/O_WRONLY/O_CREAT/O_TRUNC, exit               */
#include "stdio.h"    /* printf (fd 1 -> serial) for the #491 Rust selftest    */
#include "arena_rs.h" /* #491 Stage 0: userland Rust FFI (arena_rs_selftest)   */
#include "bsp_load.h" /* #491 Stage 1: GoldSrc BSP v30 import                  */

#define MAXW 1280
#define MAXH 800
static uint32_t g_blit[MAXW * MAXH];

World g_world;

/* #491 Stage 1: set while a BSP map is active so phys_step free-flies (noclip). */
int g_arena_noclip = 0;
/* Test-only: when /ARENA/BSPAUTO exists, auto-enter the BSP map at startup and
 * auto-fly a slow orbit so a headless VM screendump can prove the render path
 * WITHOUT keyboard injection. Absent this sentinel, behaviour is normal (menu +
 * box levels default; press B to enter the BSP map and WASD to noclip-walk).  */
int g_bsp_autodemo = 0;
/* Path of the optional imported map + its external texture WAD (user-supplied). */
#define BSP_MAP_PATH "/ARENA/MAP.BSP"
#define BSP_WAD_PATH "/ARENA/MAP.WAD"

/* #491 Stage 3 diagnostics, defined with the rest of the KEYLOG block far below
 * but called from arena_start_bsp() / the main loop above it.                  */
static void klog_spawntab(World *w);
static void klog_respawn_watch(World *w);

/* Enter a loaded BSP map as a noclip free-fly world: zero box geometry, one live
 * local player at the map spawn, render the BSP faces directly. Existing box
 * levels are untouched (this only runs when the user presses B with a map
 * loaded); leaving BSP mode is a relaunch this stage.                          */
void arena_start_bsp(void) {
    if (!bsp_available()) return;
    /* Reuse mode_init to build a valid World + a live local player, 0 bots. */
    mode_init(&g_world, 0, 0);
    g_world.level.nbrush = 0;      /* no box brushes: BSP faces render instead   */
    g_world.level.nprop  = 0;
    g_world.level.nitem  = 0;
    /* #491 Stage 3: mode_init() above ran level_load(), which ALSO filled
     * spawns[]/nspawn, and those coordinates belong to THAT box level. Zeroing
     * nbrush/nprop/nitem but not nspawn left The Longest Yard's spawn table live
     * inside an imported map: the first death (this function deliberately spawns
     * a bot, so a death is a matter of a minute) sent mode_respawn_player ->
     * spawn_player_entity -> pick_spawn to a box-level coordinate embedded in
     * dust2's geometry, which is indistinguishable from the Stage 2 collision
     * fix having failed. Replace the table with the map's OWN info_player_*
     * entities so every respawn lands where CS itself stands a player.         */
    g_world.level.nspawn = 0;
    int nsp = bsp_get_spawn_count();
    for (int i = 0; i < nsp && g_world.level.nspawn < MAX_SPAWNS; i++) {
        vec3 sp; float yaw;
        if (!bsp_get_spawn_n(i, &sp, &yaw)) continue;
        Spawn *s = &g_world.level.spawns[g_world.level.nspawn++];
        s->pos = sp;
        s->yaw = yaw;
    }
    /* A map carrying no info_player_* at all: fall back to the single entry
     * spawn (the map centre, per bsp_load_file) rather than leaving nspawn 0 and
     * dropping pick_spawn onto its (0,0,48) last-resort constant, a coordinate
     * with no relationship to this map whatsoever.                             */
    if (g_world.level.nspawn == 0) {
        vec3 sp; bsp_get_spawn(&sp);
        g_world.level.spawns[0].pos = sp;
        g_world.level.spawns[0].yaw = 0.0f;
        g_world.level.nspawn = 1;
    }
    bsp_get_bounds(&g_world.level.world_mins, &g_world.level.world_maxs);
    bsp_set_active(1);
    /* #491 Stage 2: real hull-trace collision (world.c map_move_entity/
     * map_point_solid) is only usable when the loaded map actually parsed
     * usable CLIPNODES+PLANES+MODELS data (bsp_hull_available()). Gravity
     * with NO collision would just drop the player through the floor
     * forever, which is worse than the Stage-1 noclip fallback - so a map
     * missing/degenerate clip data (a non-standard compile, or a bare
     * render-only test fixture) stays noclip rather than silently falling
     * through an undefined world. de_dust2's own CS 1.6 compile carries
     * hull 1 clipnodes, so this is 0 (real collision) for it in practice.    */
    g_arena_noclip = bsp_hull_available() ? 0 : 1;
    /* place the local player entity at the BSP spawn */
    if (g_world.local_player >= 0) {
        int ei = g_world.players[g_world.local_player].entity;
        Entity *p = ent_get(&g_world, ei);
        if (p) {
            vec3 sp; bsp_get_spawn(&sp);
            p->pos = sp; p->vel = v3(0, 0, 0);
            p->yaw = 0.0f; p->pitch = 0.0f;
            /* #481-style nudge: lift a hair so the FIRST real-collision frame
             * does not start exactly coplanar with a floor surface (see
             * modes.c spawn_player_entity's identical nudge for box levels;
             * the BSP spawn origin comes from the map's own entity text and
             * has no such guarantee). Irrelevant in the noclip fallback.     */
            p->pos.z += 2.0f;
            p->on_ground = g_arena_noclip ? 1 : 0;
        }
    }
    g_world.state = GS_PLAYING;

    /* #491 Stage 2 verification aid: prove the SAME shared movement path
     * (map_move_entity/phys_step) also blocks/holds a SECOND entity, not
     * just the local player ("bots share the movement path"). Proper BSP
     * entity placement (nav around the imported geometry) remains Stage 3+
     * future work (ARENA_BSP_PLAN.md). The bot is still placed directly rather
     * than through mode_init's bot count, but it now stands on one of the map's
     * OWN spawns: spawn index 1 when the map has one (dust2's index 1 is 96
     * units from index 0, so the bot is a real threat AND the coordinate is one
     * CS itself stands a player on), falling back to the old fixed +80/+80
     * offset from the entry spawn for a map with a single spawn. NOTE this is
     * only the bot's PLACEMENT; once it dies it respawns through
     * pick_spawn()/level.spawns[], which is now the map's real spawn set (see
     * the nspawn rebuild above) rather than the stale box-level table. Skipped
     * entirely under the noclip fallback (nothing to collide with) or if the
     * entity pool has no free slot.                                         */
    if (!g_arena_noclip) {
        int bi = ent_alloc(&g_world);
        if (bi >= 0 && g_world.nplayers < MAX_PLAYERS) {
            Entity *b = ent_get(&g_world, bi);
            vec3 sp; bsp_get_spawn(&sp);
            vec3 bsp_pos; float bsp_yaw;
            if (bsp_get_spawn_n(1, &bsp_pos, &bsp_yaw))
                sp = bsp_pos;
            else { sp.x += 80.0f; sp.y += 80.0f; }
            b->type   = ET_BOT;
            b->pos    = v3(sp.x, sp.y, sp.z + 2.0f);
            b->vel    = v3(0, 0, 0);
            b->yaw    = 2.3f; b->pitch = 0.0f;
            b->health = 100;  b->armor = 0; b->on_ground = 0;
            b->have_weapon[W_GAUNTLET]   = 1;
            b->have_weapon[W_MACHINEGUN] = 1;
            b->ammo[W_MACHINEGUN]        = 100;
            b->weapon = W_MACHINEGUN;

            int slot = g_world.nplayers;
            PlayerSlot *ps = &g_world.players[slot];
            const char *bn = "Bot 1"; int k = 0;
            while (bn[k] && k < 23) { ps->name[k] = bn[k]; k++; } ps->name[k] = 0;
            ps->frags = 0; ps->deaths = 0; ps->is_bot = 1;
            ps->entity = bi; ps->respawn_ms = 0; ps->color = 0x00FF4040;
            b->player_slot = slot;
            g_world.nplayers = slot + 1;
            bot_spawn(&g_world, slot);
        }
    }
    klog_spawntab(&g_world);
}

/* Persisted settings (see game.h). Defaults are used until ARENA.CFG loads.
 * Mouse sensitivity default raised 8 -> 12 (out of a widened 1..30 range, was
 * 1..20) - user-requested (2026-07-16): "increase the mouse sensitivity in
 * arena default...to help with testing", since Proxmox's noVNC viewer does
 * not pointer-grab, so the cursor hits the window edge and stops producing
 * motion deltas, making the OLD default (~0.00345 rad/px) borderline
 * unusable for driving a full turn. JUDGEMENT CALL, made explicitly rather
 * than silently: a default tuned to compensate for noVNC's broken pointer
 * grab would be WRONG on the real iMac's real mouse, so this is NOT the
 * absolute max of the new range - it is ~1.9x the old default (~0.0065
 * rad/px, see arena_look_scale()), a genuinely snappier but still
 * conventional FPS feel on real hardware. The range widened to 1..30 (max
 * ~0.0155 rad/px, ~2x the old max) so a noVNC session can crank it further
 * for testing via Settings -> Mouse Sensitivity without moving the default
 * that far on its own.                                                       */
ArenaCfg g_arena_cfg = { 1, 12, 90, 7, 3, 1 };  /* fullscreen, sens 12, fov 90, vol 7, 3 bots, minimap on */

static int g_win = -1;
static int g_nochrome = 0;     /* borderless flag applied (one-way in kernel)  */

/* mouse-look state, updated from EVENT_MOUSE_MOVE */
static int g_have_mouse, g_last_mx, g_last_my;

/* ------------------------------------------------------------ entity pool -- */
int ent_alloc(World *w) {
    for (int i = 0; i < MAX_ENTITIES; i++) if (!w->ents[i].alive) {
        Entity *e = &w->ents[i];
        for (unsigned z = 0; z < sizeof(*e); z++) ((unsigned char*)e)[z] = 0;
        e->alive = 1; e->player_slot = -1; e->proj_owner = -1;
        return i;
    }
    return -1;
}
void ent_free(World *w, int idx) {
    if (idx < 0 || idx >= MAX_ENTITIES) return;
    if (w->ents[idx].brain) bot_free(&w->ents[idx]);
    w->ents[idx].alive = 0; w->ents[idx].type = ET_FREE;
}
Entity *ent_get(World *w, int idx) {
    if (idx < 0 || idx >= MAX_ENTITIES || !w->ents[idx].alive) return 0;
    return &w->ents[idx];
}

/* ============================================================ settings I/O = */
#define CFG_PATH "/CONFIG/ARENA.CFG"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* apply clamps + push volume to the master mixer + FOV to the renderer */
static void cfg_clamp_apply(void) {
    g_arena_cfg.fullscreen  = g_arena_cfg.fullscreen ? 1 : 0;
    g_arena_cfg.sensitivity = clampi(g_arena_cfg.sensitivity, 1, 30);  /* widened, see g_arena_cfg init comment */
    g_arena_cfg.fov         = clampi(g_arena_cfg.fov, 70, 110);
    g_arena_cfg.volume      = clampi(g_arena_cfg.volume, 0, 10);
    g_arena_cfg.bots        = clampi(g_arena_cfg.bots, 1, MAX_PLAYERS - 1);
    g_arena_cfg.minimap     = g_arena_cfg.minimap ? 1 : 0;
    set_volume(g_arena_cfg.volume * 10);      /* 0..10 -> 0..100 master volume  */
    r_set_fov(g_arena_cfg.fov);
}

/* tiny int parser: reads an unsigned decimal, ignores leading spaces          */
static int cfg_atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* match "key=" at the start of a line; returns pointer past '=' or 0          */
static const char *cfg_match(const char *line, const char *key) {
    while (*key) { if (*line != *key) return 0; line++; key++; }
    if (*line != '=') return 0;
    return line + 1;
}

static void arena_cfg_load(void) {
    int fd = sys_open(CFG_PATH, O_RDONLY);
    if (fd < 0) { cfg_clamp_apply(); return; }
    char buf[512];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) { cfg_clamp_apply(); return; }
    buf[n] = 0;
    /* walk lines split on '\n' */
    int i = 0;
    while (i < n) {
        char *line = &buf[i];
        while (i < n && buf[i] != '\n') i++;
        if (i < n) buf[i++] = 0;
        const char *v;
        if      ((v = cfg_match(line, "fullscreen")))  g_arena_cfg.fullscreen  = cfg_atoi(v);
        else if ((v = cfg_match(line, "sensitivity"))) g_arena_cfg.sensitivity = cfg_atoi(v);
        else if ((v = cfg_match(line, "fov")))         g_arena_cfg.fov         = cfg_atoi(v);
        else if ((v = cfg_match(line, "volume")))      g_arena_cfg.volume      = cfg_atoi(v);
        else if ((v = cfg_match(line, "bots")))        g_arena_cfg.bots        = cfg_atoi(v);
        else if ((v = cfg_match(line, "minimap")))     g_arena_cfg.minimap     = cfg_atoi(v);
    }
    cfg_clamp_apply();
}

static void putint(char *dst, int *pos, int v) {
    if (v < 0) { dst[(*pos)++] = '-'; v = -v; }
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 12) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) dst[(*pos)++] = tmp[--t];
}
static void putkv(char *dst, int *pos, const char *key, int v) {
    while (*key) dst[(*pos)++] = *key++;
    dst[(*pos)++] = '=';
    putint(dst, pos, v);
    dst[(*pos)++] = '\n';
}

void arena_cfg_save(void) {
    cfg_clamp_apply();
    char buf[256]; int p = 0;
    putkv(buf, &p, "fullscreen",  g_arena_cfg.fullscreen);
    putkv(buf, &p, "sensitivity", g_arena_cfg.sensitivity);
    putkv(buf, &p, "fov",         g_arena_cfg.fov);
    putkv(buf, &p, "volume",      g_arena_cfg.volume);
    putkv(buf, &p, "bots",        g_arena_cfg.bots);
    putkv(buf, &p, "minimap",     g_arena_cfg.minimap);
    int fd = sys_open(CFG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)p);
    sys_close(fd);
}

/* Mouse-look radians per pixel from the sensitivity setting (1..30, widened
 * from 1..20 - see g_arena_cfg's init comment). Low end unchanged (sens=1
 * still gives the original 0.0010 rad/px for a precise real mouse); the
 * per-step increment was raised so the new default (12) lands at ~0.0065
 * rad/px (~1.9x the old default) and the new max (30) reaches ~0.0155 rad/px
 * (~2x the old max), giving a noVNC test session real cranking headroom.     */
float arena_look_scale(void) {
    return 0.0010f + (float)(g_arena_cfg.sensitivity - 1) * 0.00050f; /* ~.001..~.0155 */
}

/* Enter borderless fullscreen live. Kernel win_set_nochrome() is one-way (no
 * un-set), so the windowed direction only takes effect on the next launch;
 * that is surfaced to the user in the Settings screen.                        */
void arena_apply_fullscreen(void) {
    if (g_win < 0) return;
    if (g_arena_cfg.fullscreen && !g_nochrome) {
        win_set_nochrome(g_win);
        g_nochrome = 1;
    }
}

/* (Re)start a match with the current settings (bots + level_index).           */
void arena_start_match(void) {
    int li = g_world.level_index;
    if (li < 0 || li >= NUM_LEVELS) li = 0;
    /* #491 Stage 2: a normal box-level match must leave BSP noclip mode behind.
     * arena_start_bsp() sets g_arena_noclip=1 + bsp_set_active(1) and nothing
     * previously cleared them, so entering a BSP map (menu or B) then going
     * back to the menu and picking a box level left the world noclip-flying
     * with render.c still drawing the (stale) BSP faces instead of brushes.
     * bsp_set_active(0) only flips the active flag - bsp_available() (g_loaded)
     * is untouched, so the de_dust2 menu entry is still there next time.       */
    g_arena_noclip = 0;
    bsp_set_active(0);
    mode_init(&g_world, li, g_arena_cfg.bots);
    g_world.state = GS_PLAYING;
    /* #501: dump the spawn table + per-spawn validity for BOX levels too. This
     * call previously existed ONLY in arena_start_bsp(), because it was written
     * for the #491 de_dust2 stale-spawn-table bug, so starting a normal level
     * logged NOTHING and the oracle silently did not fire. Found the honest way:
     * the first #501 verification run came back with 7/7 good spawns but no
     * SPAWNTAB line at all, which is what prompted looking rather than
     * declaring victory on the 7/7. An instrument that does not run is not
     * evidence of anything.                                                   */
    klog_spawntab(&g_world);
}

/* ------------------------------------------------------------ input state -- */
/* Real-hardware robustness (round 3, iMac USB-HID vs VM PS/2):
 *
 * The kernel delivers keys identically for PS/2 and USB-HID (usb_hid.c
 * translates HID usage -> PS/2 set-1 scancodes into the same path), so keycode
 * and key_char carry the SAME ASCII value on both. The real difference is HOLD
 * semantics: PS/2 auto-repeats make-codes while a key is held (so a dropped or
 * spurious edge self-heals within ~33 ms), while USB-HID boot protocol is pure
 * edge - ONE down, silence, ONE up, no repeat. A single spurious UP from the
 * iMac's flaky hub (#373/#433) therefore latches a movement key OFF with nothing
 * to re-assert it, and the player stops moving mid-hold.
 *
 * Defence: a latch with a short RELEASE-DEBOUNCE. A key counts as held while
 * latched OR within KEY_GRACE_MS of its last release; a real release applies
 * after the grace (imperceptible), but a glitch UP immediately followed by the
 * recovery DOWN never drops the hold. We set/read by BOTH keycode and key_char,
 * and additionally accept the raw set-1 scancodes for WASD/space in case a build
 * ever delivers scancodes instead of ASCII. Every event is also logged to
 * /ARENA/KEYLOG.TXT for real-hardware ground truth (see keylog()).            */
#define KEY_GRACE_MS 90
static unsigned char g_keys[256];       /* instantaneous latch                  */
static unsigned      g_key_rel[256];     /* uptime_ms at release (0 = held/idle) */
static unsigned      g_now_ms;           /* refreshed once per frame             */
static unsigned      g_prev_ms;          /* uptime_ms of the previous frame      */
static unsigned      g_junk_keys;        /* count of ignored mouse/unknown keys  */

/* Real-hardware event hygiene: the iMac Magic Mouse (and a flaky USB hub, #373/
 * #433) leaks spurious key events into the window stream, observed as a flood of
 * kc=0x80 / kc=0x81 DN/UP pairs with key_char=0. Every movement/menu key we act
 * on is an ASCII value (< 0x80) or a PS/2 set-1 scancode that is ALSO < 0x80
 * (W=0x11, A=0x1E, S=0x1F, D=0x20, space=0x39, TAB=0x09, ESC=0x1B). So any event
 * whose keycode is >= 0x80 and carries no ASCII char is mouse-origin noise: drop
 * it so it can neither churn g_keys[] nor starve the WASD latch, and never let it
 * consume the bounded KEYLOG budget. */
static int key_usable(const gui_event_t *ev) {
    int kc = (int)ev->keycode & 0xFF;
    int ch = (int)(unsigned char)ev->key_char;
    if (kc >= 0x80 && ch == 0) return 0;   /* Magic Mouse / unknown HID leak      */
    if (kc == 0 && ch == 0)   return 0;    /* empty event                         */
    return 1;
}

static int key_is_down(int k) {
    if (k < 0 || k > 255) return 0;
    if (g_keys[k]) return 1;
    if (g_key_rel[k] && (g_now_ms - g_key_rel[k]) < KEY_GRACE_MS) return 1;  /* grace */
    return 0;
}

static void key_apply(int k, int down) {
    if (k <= 0 || k > 255) return;
    if (down) { g_keys[k] = 1; g_key_rel[k] = 0; }
    else if (g_keys[k]) { g_keys[k] = 0; g_key_rel[k] = g_now_ms ? g_now_ms : 1; }
}

static void key_set(const gui_event_t *ev, int down) {
    key_apply((int)(unsigned)ev->keycode, down);
    key_apply((int)(unsigned char)ev->key_char, down);
}

/* Held test for a movement key: ASCII lower/upper + an OPTIONAL PS/2 set-1
 * scancode fallback (pass scan1 < 0 to skip the scancode check entirely).     */
static int held_mv(int ascii_lo, int scan1) {
    int up = (ascii_lo >= 'a' && ascii_lo <= 'z') ? ascii_lo - 32 : ascii_lo;
    if (key_is_down(ascii_lo) || key_is_down(up)) return 1;
    if (scan1 < 0) return 0;
    return key_is_down(scan1);
}

static void build_cmd(World *w) {
    Entity *p = (w->local_player >= 0 && w->players[w->local_player].entity >= 0)
                    ? ent_get(w, w->players[w->local_player].entity) : 0;
    w->cmd.fwd = w->cmd.side = w->cmd.up = 0;
    w->cmd.fire = w->cmd.jump = 0;
    if (!p) { w->cmd.dyaw = w->cmd.dpitch = 0; return; }
    if (held_mv('w', 0x11)) w->cmd.fwd  += 1;      /* set-1: W=0x11             */
    if (held_mv('s', 0x1F)) w->cmd.fwd  -= 1;      /*        S=0x1F             */
    if (held_mv('a', 0x1E)) w->cmd.side -= 1;      /*        A=0x1E             */
    /* #484 ROOT CAUSE (found by live instrumentation, VM2491, 2026-07-16):
     * D's PS/2 set-1 fallback scancode is 0x20 - which is ALSO the ASCII code
     * for space (jump). key_set()/key_is_down() store both the ASCII code and
     * any scancode fallback in the SAME g_keys[256]/g_key_rel[256] arrays by
     * raw byte value (see key_apply above), so holding jump alone lit
     * g_keys[0x20], and held_mv('d', 0x20) misread that as "D is held" ->
     * cmd.side spuriously became +1 on every jump. Combined with a genuine A
     * press (cmd.side -= 1) this cancelled strafe to exactly 0 for as long as
     * jump was held (jump+A = no lateral movement at all - the "strafing does
     * not move the player" symptom of #484); jump alone caused an unwanted
     * rightward drift. Proven live: pressing ONLY space (no A, no D) set
     * w->cmd.side=1 and moved the player sideways with dx growing to 400+ over
     * a few seconds of held jump. No other WASD scancode collides with an
     * in-use ASCII key (W=0x11/S=0x1F/A=0x1E are all non-printable control
     * codes with nothing else mapped to them), so D is the only offender.
     * This platform always delivers ASCII in both ev->keycode and
     * ev->key_char (see the block comment above key_usable()), so the
     * scancode fallback was never actually needed for D; drop it rather than
     * pick a different, larger scancode namespace to dodge future collisions. */
    if (held_mv('d', -1))   w->cmd.side += 1;      /*        D: ASCII only      */
    if (key_is_down(' ') || key_is_down(0x39)) w->cmd.jump = 1;  /* space/0x39   */
    /* weapon select 1..0 */
    for (int k = 0; k < 10; k++) {
        char c = (char)('1' + k);
        if (k == 9) c = '0';
        if (key_is_down((int)c)) w->cmd.wantweap = (k) % NUM_WEAPONS;
    }
}

/* apply the assembled command to the local player's entity */
static void apply_local_input(World *w) {
    if (w->local_player < 0) return;
    int ei = w->players[w->local_player].entity;
    Entity *p = ent_get(w, ei);
    if (!p) return;
    p->yaw   += w->cmd.dyaw;
    p->pitch += w->cmd.dpitch;
    if (p->pitch >  1.4f) p->pitch = 1.4f;
    if (p->pitch < -1.4f) p->pitch = -1.4f;
    w->cmd.dyaw = w->cmd.dpitch = 0;
    if (w->cmd.wantweap >= 0 && w->cmd.wantweap < NUM_WEAPONS &&
        p->have_weapon[w->cmd.wantweap]) p->weapon = w->cmd.wantweap;
    w->cmd.wantweap = -1;
    /* movement handled in phys_step using cmd.fwd/side + yaw; fire here */
    if (w->cmd.fire) weap_fire(w, ei);
}

/* TAB (0x09) or the raw special code shows the scoreboard while playing.       */
#define KEY_TAB   0x09
#define KEY_ESC   0x1B

/* ------------------------------------------------------------- KEYLOG (diag) */
/* Real-hardware ground truth: every key event is appended (type + keycode +
 * key_char + window + frame ms) and the whole bounded buffer is flushed to
 * /ARENA/KEYLOG.TXT so, if WASD STILL fails on the iMac, one boot + a few W/A/S/D
 * presses + reading this file tells us EXACTLY what the USB-HID keyboard sends
 * (are DOWN/UP arriving at all? what codes? clean edges or bouncing?).         */
#define KEYLOG_PATH   "/ARENA/KEYLOG.TXT"
#define KEYLOG_CAP    32000
#define KEYLOG_MAXEV  600      /* per-key-event budget                            */
#define KEYLOG_MAXST  400      /* separate STAT-line budget (not gated by keys)   */
static char g_klog[KEYLOG_CAP];
static int  g_klog_len = 0;
static int  g_klog_ev  = 0;
static int  g_klog_st  = 0;

static void klog_s(const char *s) { while (*s && g_klog_len < KEYLOG_CAP - 1) g_klog[g_klog_len++] = *s++; }
static void klog_h(unsigned v) {
    const char *h = "0123456789abcdef";
    if (g_klog_len < KEYLOG_CAP - 2) { g_klog[g_klog_len++] = h[(v >> 4) & 0xF]; g_klog[g_klog_len++] = h[v & 0xF]; }
}
static void klog_d(unsigned v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v && n < 12) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && g_klog_len < KEYLOG_CAP - 1) g_klog[g_klog_len++] = t[--n];
}
static void klog_di(int v) {   /* signed decimal                                  */
    if (v < 0) { if (g_klog_len < KEYLOG_CAP - 1) g_klog[g_klog_len++] = '-'; v = -v; }
    klog_d((unsigned)v);
}
static void klog_flush(void) {
    int fd = sys_open(KEYLOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, g_klog, (unsigned long)g_klog_len);
    /* O_TRUNC does NOT shrink an existing file on this FAT volume: a run that
     * writes FEWER bytes than the last one leaves the previous run's bytes
     * stranded in the tail, and a `tail` of this file then serves stale
     * coordinates from a DIFFERENT level that read exactly like a real result
     * (this nearly caused a false PASS during the Stage 2 collision work).
     * Terminate every flush with a sentinel: read only up to the first #EOF#
     * and everything after it is, by construction, the older run.             */
    static const char eof_mark[] = "\n#EOF#\n";
    sys_write(fd, eof_mark, sizeof(eof_mark) - 1);
    sys_close(fd);
}

/* Loud, rare, cross-module log line (declared in game.h). modes.c uses it for
 * the #501 "no valid spawn anywhere" fallback; klog_* are static to this file. */
void arena_log(const char *s) { klog_s(s); klog_flush(); }

/* #491 Stage 3 oracle: dump the spawn table the map actually produced, so a
 * post-death position can be checked AGAINST THE MAP'S OWN entity list rather
 * than believed because it looks plausible. n= is what pick_spawn will draw
 * from; bsp= is what the entity text yielded before the MAX_SPAWNS clamp.
 *
 * #501: each spawn now also reports ok=, i.e. what map_spawn_valid() says about
 * it, and the header carries nok=<valid>/<total>. This makes the fix's premise
 * checkable AT RUNTIME over EVERY spawn from a single launch, instead of having
 * to re-roll a random pick_spawn draw until the buried half happens to come up.
 * Expected on The Longest Yard (level 0, the default): nok=4/8, with SP4..SP7
 * (the four edge-centre spawns) ok=0 and the four corners ok=1. If this ever
 * reads 8/8 or 0/8 on that level, the checker is broken, not the level.      */
static void klog_spawntab(World *w) {
    int nok = 0;
    for (int i = 0; i < w->level.nspawn && i < MAX_SPAWNS; i++)
        if (map_spawn_valid(w, w->level.spawns[i].pos)) nok++;
    klog_s("SPAWNTAB n=");  klog_di(w->level.nspawn);
    klog_s(" nok=");        klog_di(nok);
    klog_s(" bsp=");        klog_di(bsp_get_spawn_count());
    klog_s(" nc=");         klog_di(g_arena_noclip);
    klog_s("\n");
    for (int i = 0; i < w->level.nspawn && i < MAX_SPAWNS; i++) {
        klog_s(" SP");   klog_di(i);
        klog_s(" x=");   klog_di((int)w->level.spawns[i].pos.x);
        klog_s(" y=");   klog_di((int)w->level.spawns[i].pos.y);
        klog_s(" z=");   klog_di((int)w->level.spawns[i].pos.z);
        klog_s(" ok=");  klog_di(map_spawn_valid(w, w->level.spawns[i].pos));
        klog_s("\n");
    }
    klog_flush();
}

/* #491 Stage 3 oracle: the local player's entity id changes on death (-> -1) and
 * on respawn (-> a fresh slot). Watched EVERY frame (not on the 1Hz STAT tick)
 * so the logged position is the one pick_spawn actually produced, before any
 * movement or gravity can drift it. Only emits on a change, so it costs nothing
 * in steady state. */
static void klog_respawn_watch(World *w) {
    static int prev_ent = -2;
    int cur = (w->local_player >= 0 && w->local_player < MAX_PLAYERS)
                  ? w->players[w->local_player].entity : -1;
    if (cur == prev_ent) return;
    prev_ent = cur;
    if (g_klog_st >= KEYLOG_MAXST) return;
    g_klog_st++;
    klog_d(g_now_ms);
    if (cur < 0) { klog_s(" DIED\n"); klog_flush(); return; }
    Entity *p = ent_get(w, cur);
    klog_s(" RESPAWN ent=");  klog_di(cur);
    klog_s(" px=");           klog_di(p ? (int)p->pos.x : 0);
    klog_s(" py=");           klog_di(p ? (int)p->pos.y : 0);
    klog_s(" pz=");           klog_di(p ? (int)p->pos.z : 0);
    klog_s(" og=");           klog_di(p ? p->on_ground : 0);
    klog_s("\n");
    klog_flush();
}
static void keylog(const char *type, const gui_event_t *ev) {
    if (g_klog_ev >= KEYLOG_MAXEV) return;
    g_klog_ev++;
    klog_d(g_now_ms);          klog_s(" ");
    klog_s(type);
    klog_s(" kc=0x");          klog_h((unsigned)ev->keycode & 0xFF);
    klog_s(" ch=0x");          klog_h((unsigned)(unsigned char)ev->key_char);
    klog_s(" win=");           klog_d((unsigned)(g_win < 0 ? 0 : g_win));
    klog_s("\n");
    klog_flush();
}
static void keylog_mark(const char *msg) {
    if (g_klog_ev >= KEYLOG_MAXEV) return;
    klog_s(msg); klog_s("\n");
    klog_flush();
}

/* Movement ground-truth for the NEXT iMac boot. Emitted once/second or whenever
 * the movement command changes, so a single boot + a few W/A/S/D presses tells us
 * EXACTLY where motion dies. Fields:
 *   dt   = real per-frame elapsed ms after clamp (reveals slow-motion / framerate)
 *   st   = GS_* state (is the match actually PLAYING? or stuck in a menu?)
 *   lp   = local-player entity valid (build_cmd early-returns if 0)
 *   fwd/side = the movement command build_cmd derived from the held keys
 *   vx/vy    = local player velocity from phys_step
 *   dx/dy    = integer world-position delta since the previous STAT line
 *   junk     = cumulative mouse/unknown key events ignored (the 0x80/0x81 flood)
 * If fwd flips to +/-1 on W/S but dx/dy stay 0, motion dies in physics/blit; if
 * fwd never leaves 0 while keys arrive, the latch/state is wrong; lp=0 means the
 * local entity is invalid on real HW. */
static void klog_status(World *w, int dt) {
    if (g_klog_st >= KEYLOG_MAXST) return;
    g_klog_st++;
    Entity *p = (w->local_player >= 0 && w->local_player < MAX_PLAYERS &&
                 w->players[w->local_player].entity >= 0)
                    ? ent_get(w, w->players[w->local_player].entity) : 0;
    int px = p ? (int)p->pos.x : 0, py = p ? (int)p->pos.y : 0;
    int pz = p ? (int)p->pos.z : 0;
    int vx = p ? (int)p->vel.x : 0, vy = p ? (int)p->vel.y : 0;
    int vz = p ? (int)p->vel.z : 0;
    int og = p ? p->on_ground : 0;
    static int have_prev = 0, prev_px = 0, prev_py = 0, prev_pz = 0;
    int dx = have_prev ? px - prev_px : 0;
    int dy = have_prev ? py - prev_py : 0;
    int dz = have_prev ? pz - prev_pz : 0;
    prev_px = px; prev_py = py; prev_pz = pz; have_prev = 1;

    klog_d(g_now_ms);
    klog_s(" STAT dt=");   klog_di(dt);
    klog_s(" st=");        klog_di(w->state);
    klog_s(" lp=");        klog_di(p ? 1 : 0);
    klog_s(" nc=");        klog_di(g_arena_noclip);
    klog_s(" fwd=");       klog_di(w->cmd.fwd);
    klog_s(" side=");      klog_di(w->cmd.side);
    /* #491 Stage 2 collision oracle: absolute position + ground contact.
     * pz/og/dz are the FLOOR truth (a fall-through reads og=0 with dz very
     * negative every sample); px/py/dx/dy are the WALL truth (fwd=1 with
     * dx==0 and dy==0 across samples means the hull actually blocked us).   */
    klog_s(" px=");        klog_di(px);
    klog_s(" py=");        klog_di(py);
    klog_s(" pz=");        klog_di(pz);
    klog_s(" og=");        klog_di(og);
    klog_s(" vx=");        klog_di(vx);
    klog_s(" vy=");        klog_di(vy);
    klog_s(" vz=");        klog_di(vz);
    klog_s(" dx=");        klog_di(dx);
    klog_s(" dy=");        klog_di(dy);
    klog_s(" dz=");        klog_di(dz);
    klog_s(" junk=");      klog_d(g_junk_keys);
    klog_s("\n");
    klog_flush();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* #491 Stage 0: prove the userland Rust object (no_std + alloc + FFI, built
     * for the Ring-3 ABI and linked into this ELF) actually executes. This does
     * NOT change any game behavior; it only logs a deterministic magic value so
     * a boot can confirm Rust ran in Ring-3. Kept as the FIRST thing main() does
     * so the line lands on serial before the window/renderer spin up. */
    {
        uint32_t rs = arena_rs_selftest();
        printf("[ARENA-RS] selftest=0x%08X %s\n", rs,
               rs == ARENA_RS_SELFTEST_MAGIC ? "ok" : "MISMATCH");
    }

    /* Load prefs FIRST: need the fullscreen flag before creating the window.
     * True fullscreen must cover the ENTIRE framebuffer, including the bottom
     * dock strip. window_maximize() only fills to fb_h-80, which is why the
     * old maximize-based "fullscreen" left the taskbar/start menu visible.    */
    arena_cfg_load();
    fb_info_t _fi; int scr_w = 1280, scr_h = 800;
    if (fb_info(&_fi) == 0 && _fi.width > 0 && _fi.height > 0) {
        scr_w = (int)_fi.width; scr_h = (int)_fi.height;
    }
    /* #491 Stage 1: a boot AUTORUN.CFG can launch Arena BEFORE the compositor is
     * ready, so win_create races and fails; Arena would then exit and never show.
     * Retry a bounded number of times, yielding via sys_sleep (NOT a busy-wait;
     * #426-compliant), until the compositor accepts the window (~8s max).       */
    for (int attempt = 0; attempt < 40 && g_win < 0; attempt++) {
        if (g_arena_cfg.fullscreen)
            g_win = win_create("Maytera Arena", 0, 0, scr_w, scr_h);  /* true fullscreen */
        else
            g_win = win_create("Maytera Arena", 60, 40, 900, 620);
        if (g_win < 0) sys_sleep(200);
    }
    if (g_win < 0) return 1;
    arena_apply_fullscreen();            /* set nochrome if fullscreen pref     */
    if (!g_arena_cfg.fullscreen)
        sys_wm_maximize_focused();       /* windowed: maximize (keeps chrome)   */
    wm_focus(g_win);                     /* explicitly grab kbd focus (IRC #242) */
    { gui_event_t pe; win_get_event(g_win, &pe, 60); (void)pe; }
    keylog_mark("BOOT arena keylog ready");

    int cw = 900, ch = 620;
    if (win_get_size(g_win, &cw, &ch) != 0 || cw <= 0) { cw = 900; ch = 620; }
    if (cw > MAXW) cw = MAXW;
    if (ch > MAXH) ch = MAXH;

    /* Paint the menu backdrop (art, or gradient fallback) instantly so the
     * window never shows a white flash while r_init + the world spin up.      */
    menu_draw_loading(g_blit, cw, ch);
    syscall5(SYS_WIN_BLIT, g_win, 0, 0, (cw & 0xFFFF) | ((ch & 0xFFFF) << 16), (long)g_blit);
    win_invalidate(g_win);

    r_init(cw, ch);
    r_set_fov(g_arena_cfg.fov);
    snd_init();

    /* Spin up a live match so there is an arena rendering behind the menu, but
     * present the Main Menu first (Play/Settings/Controls/Quit).               */
    g_world.local_player = -1;
    g_world.cmd.wantweap = -1;
    mode_init(&g_world, 0, g_arena_cfg.bots);
    g_world.state = GS_MENU;

    /* #491 Stage 1: if the user dropped a GoldSrc map at /ARENA/MAP.BSP, parse +
     * decode it now (all in Rust, crash-safe on hostile input). It is NOT entered
     * automatically: press B from the menu to noclip-walk it, so the built-in box
     * levels stay the default (no regression). */
    if (bsp_load_file(BSP_MAP_PATH, BSP_WAD_PATH)) {
        printf("[ARENA-BSP] loaded %s ok\n", BSP_MAP_PATH);
        /* headless auto-demo (test sentinel): enter BSP + auto-fly, no keyboard */
        int af = sys_open("/ARENA/BSPAUTO", O_RDONLY);
        if (af >= 0) { sys_close(af); g_bsp_autodemo = 1; arena_start_bsp(); }
    }

    gui_event_t ev;
    int running = 1;
    g_prev_ms = (unsigned)uptime_ms();
    while (running) {
        /* Frame-rate-independent timestep. The renderer is a full-screen software
         * rasteriser; on the real iMac (2013 dual-core, no GPU accel) a frame can
         * take 100 ms+ where QEMU runs near 60 fps. With the OLD fixed 16 ms dt the
         * world advanced only 16 ms of motion per RENDERED frame, so at ~5-10 fps
         * the game ran in slow-motion and a held W barely moved the player in real
         * time: the "WASD does not move on the iMac" report. Deriving dt from real
         * elapsed uptime (clamped 1..100 ms to stay collision-safe) makes movement
         * wall-clock correct at ANY framerate. */
        /* Fullscreen games must NEVER lose keyboard focus. Keys route only to the
         * kernel's focused window (SYS_INJECT_KEY -> wm_dispatch_event -> focused
         * window), while the mouse routes to whatever window is under the pointer.
         * A fullscreen window always sits under the pointer, so the MOUSE keeps
         * working even after focus is lost - which is exactly the iMac symptom:
         * mouse turns the view, but WASD/ESC do nothing. Focus gets stolen when
         * ANOTHER window is created while we boot (the kernel gives a new window
         * keyboard focus immediately, syscall.c), and on the slow iMac r_init /
         * snd_init leave a wide gap for a notification / dock / service window to
         * appear. wm_focus_window() early-returns when we already hold focus, so
         * re-asserting every frame is a cheap no-op on the VM (where it already
         * worked) and self-heals a stolen focus on real hardware. */
        wm_focus(g_win);

        g_now_ms = (unsigned)uptime_ms();     /* drives key release-debounce + keylog */
        int frame_dt = (int)(g_now_ms - g_prev_ms);
        if (frame_dt < 1)   frame_dt = 1;
        /* Collision-safety clamp ONLY: a dt bigger than this lets a fast entity
         * tunnel through a wall in one step. It is deliberately NOT the frame
         * limiter, and it must never be read as one: before #550 this clamp was
         * silently saturating at 100 every single frame, which is precisely what
         * hid the runaway loop (every STAT line on the iMac read dt=100, the
         * ceiling). With the limiter at the bottom of the loop in place, a
         * healthy frame lands at 33-50 ms and this clamp should NOT fire; if the
         * logs show dt pinned at 100 again, the renderer has regressed. */
        if (frame_dt > 100) frame_dt = 100;
        g_prev_ms = g_now_ms;

        /* Drain ALL events queued this frame so the iMac mouse/junk flood cannot
         * starve input: the first read blocks up to 16 ms to pace the loop, the
         * rest are non-blocking. Physics + render run ONCE per frame below. */
        int et = win_get_event(g_win, &ev, 16);
        int drain_guard = 0;
        while (et != EVENT_NONE && drain_guard++ < 512) {
        switch (et) {
        case EVENT_WINDOW_CLOSE: running = 0; break;
        case EVENT_RESIZE: {
            int nw, nh;
            if (win_get_size(g_win, &nw, &nh) == 0 && nw > 0 && nh > 0) {
                if (nw > MAXW) nw = MAXW; if (nh > MAXH) nh = MAXH;
                cw = nw; ch = nh; r_resize(nw, nh);
            }
        } break;
        case EVENT_KEY_DOWN: {
            if (!key_usable(&ev)) { g_junk_keys++; break; }  /* drop mouse/HID leak */
            int k = (int)ev.keycode ? (int)ev.keycode
                                    : (int)(unsigned char)ev.key_char;
            keylog("DN", &ev);       /* real-HW ground truth (see keylog())      */
            /* ESC: context-sensitive. Playing -> pause; pause/settings/controls
             * are handled by menu_handle_key; scoreboard -> back to play.       */
            if (k == KEY_ESC) {
                if (g_world.state == GS_PLAYING)      g_world.state = GS_PAUSED;
                else if (g_world.state == GS_PAUSED)  g_world.state = GS_PLAYING;  /* ESC toggles pause: resume the match (never get stuck paused) */
                else if (g_world.state == GS_SCOREBOARD) g_world.state = GS_PLAYING;
                else menu_handle_key(&g_world, KEY_ESC);   /* SETTINGS/CONTROLS/MENU back out via the menu handler */
                break;
            }
            /* #491 Stage 1: B loads/enters the imported GoldSrc BSP map (noclip
             * free-fly) when one is available and we are not already playing.   */
            if ((k == 'b' || k == 'B') && bsp_available() &&
                g_world.state != GS_PLAYING) {
                arena_start_bsp();
                break;
            }
            if (g_world.state == GS_PLAYING) {
                key_set(&ev, 1);
                if (k == KEY_TAB) g_world.state = GS_SCOREBOARD;
                if (k == 'm' || k == 'M') {          /* toggle radar minimap    */
                    g_arena_cfg.minimap = !g_arena_cfg.minimap;
                    arena_cfg_save();
                }
            } else {
                /* menus consume navigation/confirm keys */
                menu_handle_key(&g_world, k);
            }
        } break;
        case EVENT_KEY_UP:
            if (!key_usable(&ev)) { g_junk_keys++; break; }  /* drop mouse/HID leak */
            keylog("UP", &ev);
            key_set(&ev, 0);
            /* release TAB -> leave the scoreboard overlay */
            if (g_world.state == GS_SCOREBOARD) {
                int k = (int)ev.keycode ? (int)ev.keycode
                                        : (int)(unsigned char)ev.key_char;
                if (k == KEY_TAB) g_world.state = GS_PLAYING;
            }
            break;
        case EVENT_MOUSE_MOVE: {
            if (g_have_mouse && g_world.state == GS_PLAYING) {
                float s = arena_look_scale();
                /* moving the mouse right must turn the view right (yaw grows
                 * CCW with our v3fromangles convention, so subtract dx).       */
                g_world.cmd.dyaw   -= (float)(ev.mouse_x - g_last_mx) * s;
                g_world.cmd.dpitch -= (float)(ev.mouse_y - g_last_my) * s;
            }
            g_last_mx = ev.mouse_x; g_last_my = ev.mouse_y; g_have_mouse = 1;
        } break;
        case EVENT_MOUSE_DOWN: if (g_world.state == GS_PLAYING) g_keys[1] = 1; break;
        case EVENT_MOUSE_UP:   g_keys[1] = 0; break;
        default: break;
        }
        et = win_get_event(g_win, &ev, 0);   /* next queued event, non-blocking */
        }   /* end per-frame event drain */

        if (g_world.state == GS_PLAYING) {
            build_cmd(&g_world);
            g_world.cmd.fire = g_keys[1];
            apply_local_input(&g_world);
            /* #491 headless auto-demo: slowly orbit + drive the camera so a
             * screendump proves the BSP render path with no keyboard injection.
             * Stage 2: this was gated on g_arena_noclip, which was ALWAYS 1
             * while BSP maps were free-fly. Now that a map with real clip data
             * runs with collision (g_arena_noclip==0), that gate would silently
             * stop the demo from ever moving. cmd.fwd/yaw are consumed by
             * phys_step on BOTH paths, so the gate is simply wrong now: drive
             * the demo either way. Under real collision the demo walks the map
             * and stops at walls instead of flying through them, which exercises
             * strictly more of the engine than the noclip version did.        */
            if (g_bsp_autodemo) {
                int ei = g_world.players[g_world.local_player].entity;
                Entity *p = ent_get(&g_world, ei);
                if (p) {
                    p->yaw += 0.010f;
                    if (p->yaw > 6.2831853f) p->yaw -= 6.2831853f;
                    p->pitch = -0.12f;
                }
                g_world.cmd.fwd = 1;   /* noclip path in phys_step consumes this */
            }
            /* AI + simulation */
            for (int i = 0; i < MAX_ENTITIES; i++) {
                Entity *e = ent_get(&g_world, i);
                if (e && e->type == ET_BOT) bot_think(&g_world, i, frame_dt);
            }
            phys_step(&g_world, frame_dt);
            weap_update(&g_world, frame_dt);
            mode_update(&g_world, frame_dt);
            g_world.time_ms += frame_dt;
        }

        /* #491 Stage 3: catch the death/respawn edge on the frame it happens,
         * ahead of the 1Hz STAT tick below (which would let gravity move the
         * player off the spawn coordinate before it was ever logged).          */
        klog_respawn_watch(&g_world);

        /* Bounded movement diagnostics for the next iMac boot: once/second or
         * whenever the movement command changes. Runs in every state so a log
         * captured off real HW shows whether the match ever reached GS_PLAYING. */
        {
            static unsigned last_stat_ms = 0;
            static int last_fwd = 0x7fffffff, last_side = 0x7fffffff;
            if ((unsigned)(g_now_ms - last_stat_ms) >= 1000u ||
                g_world.cmd.fwd != last_fwd || g_world.cmd.side != last_side) {
                klog_status(&g_world, frame_dt);
                last_stat_ms = g_now_ms;
                last_fwd  = g_world.cmd.fwd;
                last_side = g_world.cmd.side;
            }
        }

        r_frame(&g_world, g_blit, cw);
        /* HUD only over a live match; menus draw their own full overlay.        */
        if (g_world.state == GS_PLAYING || g_world.state == GS_SCOREBOARD)
            hud_draw(&g_world, g_blit, cw, ch);
        if (g_world.state == GS_PLAYING && g_arena_cfg.minimap)
            hud_minimap(&g_world, g_blit, cw, ch);
        if (g_world.state == GS_SCOREBOARD)
            menu_draw_scores(&g_world, g_blit, cw, ch);
        else if (g_world.state != GS_PLAYING)
            menu_draw(&g_world, g_blit, cw, ch);

        syscall5(SYS_WIN_BLIT, g_win, 0, 0, (cw & 0xFFFF) | ((ch & 0xFFFF) << 16), (long)g_blit);
        win_invalidate(g_win);

        /* #550 FRAME LIMITER. This loop had NO cap: the only yield was the 16 ms
         * timeout on win_get_event above, and the software rasteriser plus the
         * full-screen SYS_WIN_BLIT overran that by ~6x, so Arena free-ran and ate
         * the whole CPU. MEASURED on the user's iMac14,4 (golden 862, 12 samples
         * over 68 s): Arena accumulated ~104 ticks/s while the idle process got
         * 0.07 ticks/s, i.e. the machine had essentially zero idle. Every other
         * process, including the network path servicing ICMP, then had to queue
         * behind a ~100 ms compute frame, which is exactly the observed 3-86 ms
         * random ping spread. Arena was doing this while sitting in a MENU.
         *
         * Sleep off the remainder of the frame budget with sys_sleep, the kernel
         * timed sleep that blocks in the scheduler. This is NOT a busy-wait and
         * NOT a proc_yield spin (#426): the process leaves the ready queue for
         * the duration, so the CPU actually goes to somebody else.
         *
         * A live match gets 30 fps; a static menu/scoreboard does not need that,
         * so it gets 20 fps and gives back correspondingly more. */
        {
            unsigned target_ms = (g_world.state == GS_PLAYING) ? 33u : 50u;
            unsigned spent_ms  = (unsigned)uptime_ms() - g_now_ms;
            if (spent_ms < target_ms)
                sys_sleep((int)(target_ms - spent_ms));
        }
    }
    r_shutdown();
    win_destroy(g_win);
    return 0;
}
