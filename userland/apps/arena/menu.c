/* Maytera Arena - menu.c
 * Front-end screens layered over a live (frozen) arena:
 *   GS_MENU      - main menu (Play / Arena / Bots / Settings / Controls / Quit)
 *   GS_PAUSED    - in-match pause menu (ESC while playing)
 *   GS_SETTINGS  - display (windowed/fullscreen), sensitivity, FOV, volume
 *   GS_CONTROLS  - key/mouse reference
 *   GS_SCOREBOARD- frag table (also the TAB overlay while playing)
 *   GS_GAMEOVER  - match-over frag table
 *
 * All 2D primitives are the shared arena_* ones from hud.c (one implementation,
 * no per-file forks). Settings edits apply live via the main.c hooks
 * (arena_apply_fullscreen / arena_cfg_save / r_set_fov / set_volume).
 */
#include "game.h"
#include "polish.h"   /* pol_load_bmp + Image for the AI art pack (LOGO/MENUBG/EMBLEM) */
#include "bsp_load.h" /* #491 Stage 1/2: bsp_available()/arena_start_bsp() for the
                       * de_dust2 menu entry (was press-B-only; now menu-selectable) */

/* shared 2D primitives, defined in hud.c */
extern void arena_fill_rect(uint32_t *blit, int W, int H, int x, int y,
                            int rw, int rh, uint32_t rgb);
extern void arena_blend_rect(uint32_t *blit, int W, int H, int x, int y,
                             int rw, int rh, uint32_t rgb, int alpha);
extern void arena_text(uint32_t *blit, int W, int H, int x, int y,
                       const char *s, uint32_t rgb, int scale);
extern void arena_text_sh(uint32_t *blit, int W, int H, int x, int y,
                          const char *s, uint32_t rgb, int scale);
extern int  arena_text_width(const char *s, int scale);
extern void arena_itoa(int v, char *out);

/* ------------------------------------------------------------ menu state */
enum { MM_PLAY = 0, MM_ARENA, MM_BOTS, MM_SETTINGS, MM_CONTROLS, MM_QUIT, MM_COUNT };
enum { PM_RESUME = 0, PM_SETTINGS, PM_CONTROLS, PM_RESTART, PM_MENU, PM_QUIT, PM_COUNT };
enum { ST_DISPLAY = 0, ST_SENS, ST_FOV, ST_VOLUME, ST_MINIMAP, ST_BACK, ST_COUNT };

static int g_main_sel = MM_PLAY;
static int g_pause_sel = PM_RESUME;
static int g_set_sel  = ST_DISPLAY;
static int g_set_from = GS_MENU;      /* screen to return to from Settings/Controls */

static void mcat(char *dst, int cap, const char *src) {
    int l = (int)strlen(dst);
    while (src && *src && l < cap - 1) dst[l++] = *src++;
    dst[l] = 0;
}

/* #491 Stage 2 UX: the imported GoldSrc map (/ARENA/MAP.BSP) is offered as one
 * extra "Arena: " entry PAST the built-in NUM_LEVELS box levels, index
 * NUM_LEVELS, but ONLY when bsp_load_file() actually parsed a map at startup
 * (bsp_available()). If the file is absent/invalid the entry simply does not
 * exist in the cycle, so the menu never lets the user select something that
 * would fail - graceful, no crash, no dead selection. */
static int level_menu_count(void) {
    return bsp_available() ? (NUM_LEVELS + 1) : NUM_LEVELS;
}

/* ------------------------------------------------------------ scoreboard */
static void draw_scoreboard(World *w, uint32_t *blit, int W, int H,
                            int gameover) {
    arena_blend_rect(blit, W, H, 0, 0, W, H, 0x00000000, gameover ? 170 : 140);

    char buf[96], num[16];

    int y = H / 8;
    if (gameover) {
        const char *t = "MATCH OVER";
        arena_text_sh(blit, W, H, (W - arena_text_width(t, 3)) / 2, y, t,
                      0x00F0D030, 3);
        y += 34;
        buf[0] = 0;
        if (w->winner >= 0 && w->winner < w->nplayers) {
            mcat(buf, 96, w->players[w->winner].name);
            mcat(buf, 96, " wins!");
        } else {
            mcat(buf, 96, "Nobody wins");
        }
        arena_text_sh(blit, W, H, (W - arena_text_width(buf, 2)) / 2, y, buf,
                      (w->winner == w->local_player) ? 0x0060FF60
                                                     : 0x00FF8040, 2);
        y += 30;
    }

    buf[0] = 0;
    mcat(buf, 96, "DEATHMATCH - first to ");
    arena_itoa(w->frag_limit > 0 ? w->frag_limit : FRAG_LIMIT, num);
    mcat(buf, 96, num);
    mcat(buf, 96, " frags");
    arena_text_sh(blit, W, H, (W - arena_text_width(buf, 2)) / 2, y, buf,
                  0x00F0F0F0, 2);
    y += 30;

    /* sort player indices by frags desc, deaths asc (n <= 16) */
    int idx[MAX_PLAYERS], n = w->nplayers;
    if (n > MAX_PLAYERS) n = MAX_PLAYERS;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0) {
            PlayerSlot *a = &w->players[idx[j]], *b = &w->players[v];
            int worse = (a->frags < b->frags) ||
                        (a->frags == b->frags && a->deaths > b->deaths);
            if (!worse) break;
            idx[j + 1] = idx[j]; j--;
        }
        idx[j + 1] = v;
    }

    int sc     = (W >= 640) ? 2 : 1;
    int colw   = 8 * sc;                      /* one character cell        */
    int rowh   = 10 * sc + 6;
    int tblw   = 34 * colw;                   /* 34-cell wide table        */
    if (tblw > W - 24) tblw = W - 24;
    int tx     = (W - tblw) / 2;
    int cfrag  = tx + tblw - 17 * colw;       /* FRAGS  (5 cells + gap)    */
    int cdeath = tx + tblw - 10 * colw;       /* DEATHS (6 cells + gap)    */
    int ctag   = tx + tblw -  3 * colw;       /* BOT    (3 cells)          */

    arena_text_sh(blit, W, H, tx,     y, "NAME",   0x00A0A0A0, sc);
    arena_text_sh(blit, W, H, cfrag,  y, "FRAGS",  0x00A0A0A0, sc);
    arena_text_sh(blit, W, H, cdeath, y, "DEATHS", 0x00A0A0A0, sc);
    y += rowh;
    arena_fill_rect(blit, W, H, tx, y - 3, tblw, 1, 0x00808080);

    for (int i = 0; i < n && y + rowh < H - 30; i++) {
        PlayerSlot *p = &w->players[idx[i]];
        int local = (idx[i] == w->local_player);
        if (local)
            arena_blend_rect(blit, W, H, tx - 6, y - 2, tblw + 12, rowh,
                             0x004060A0, 120);
        uint32_t col = local ? 0x00FFFFFF
                             : (p->color ? p->color : 0x00D0D0D0);
        char nm[16];                          /* clip to the name column */
        {
            int k = 0;
            while (k < 15 && p->name[k]) { nm[k] = p->name[k]; k++; }
            nm[k] = 0;
        }
        arena_text_sh(blit, W, H, tx, y, nm, col, sc);
        arena_itoa(p->frags, num);
        arena_text_sh(blit, W, H, cfrag, y, num, 0x0060FF60, sc);
        arena_itoa(p->deaths, num);
        arena_text_sh(blit, W, H, cdeath, y, num, 0x00FF8060, sc);
        if (p->is_bot)
            arena_text_sh(blit, W, H, ctag, y, "BOT", 0x00909090, sc);
        y += rowh;
    }

    const char *hint = gameover ? "Press Enter for menu" : "Release TAB to resume";
    arena_text_sh(blit, W, H, (W - arena_text_width(hint, 2)) / 2,
                  H - 40, hint, 0x00F0D030, 2);
}

/* ---- AI art pack (loaded once from /ARENA; NULL = graceful text fallback) -- */
static const Image *art_logo(void)  { return pol_load_bmp("/ARENA/LOGO.BMP"); }
static const Image *art_menubg(void){ return pol_load_bmp("/ARENA/MENUBG.BMP"); }
static const Image *art_emblem(void){ return pol_load_bmp("/ARENA/EMBLEM.BMP"); }

/* Nearest-neighbour stretch of an Image into the ARGB blit buffer. keyed=1
 * honours the magenta color key (transparent pixels skipped).                 */
static void art_blit(const Image *im, uint32_t *blit, int W, int H,
                     int dx, int dy, int dw, int dh, int keyed) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int py = dy + y; if (py < 0 || py >= H) continue;
        int sy = y * im->h / dh;
        const unsigned int *row = &im->px[sy * im->w];
        for (int x = 0; x < dw; x++) {
            int px = dx + x; if (px < 0 || px >= W) continue;
            unsigned int s = row[x * im->w / dw];
            if (keyed && (s >> 24) < 128) continue;   /* transparent cut-out    */
            blit[py * W + px] = s & 0x00FFFFFF;
        }
    }
}

/* Shared list renderer: background (art or gradient) + title (LOGO on the main
 * menu, else text) + subtitle. use_logo selects the LOGO art for the title.    */
static void draw_backdrop_ex(uint32_t *blit, int W, int H, const char *title,
                             const char *sub, int use_logo) {
    const Image *bg = art_menubg();
    if (bg) {
        art_blit(bg, blit, W, H, 0, 0, W, H, 0);      /* opaque menu background */
        arena_blend_rect(blit, W, H, 0, 0, W, H, 0x00060A12, 90); /* legibility scrim */
    } else {
        arena_blend_rect(blit, W, H, 0, 0, W, H, 0x00081018, 215);
        arena_blend_rect(blit, W, H, 0, 0, W, H / 3, 0x00102030, 90);
    }
    arena_fill_rect(blit, W, H, 0, H / 5 + 44, W, 2, 0x00305070);

    const Image *logo = use_logo ? art_logo() : 0;
    if (logo && logo->w > 0 && logo->h > 0) {
        int dw = (W * 55) / 100;                       /* 55% of screen width    */
        int dh = dw * logo->h / logo->w;
        if (dh > H / 3) { dh = H / 3; dw = dh * logo->w / logo->h; }
        art_blit(logo, blit, W, H, (W - dw) / 2, H / 5 - dh / 2, dw, dh, 1);
    } else {
        int ts = (W >= 780) ? 4 : 3;
        arena_text_sh(blit, W, H, (W - arena_text_width(title, ts)) / 2,
                      H / 5 - 8 * ts, title, 0x00F0D030, ts);
    }
    if (sub)
        arena_text_sh(blit, W, H, (W - arena_text_width(sub, 1)) / 2,
                      H / 5 + 40, sub, 0x008098A8, 1);

    /* optional faction emblem accent, top-left corner */
    const Image *em = art_emblem();
    if (em && em->w > 0 && em->h > 0) {
        int es = 64; art_blit(em, blit, W, H, 16, 16, es, es * em->h / em->w, 1);
    }
}
static void draw_backdrop(uint32_t *blit, int W, int H, const char *title,
                          const char *sub) {
    draw_backdrop_ex(blit, W, H, title, sub, 0);
}

static void draw_row(uint32_t *blit, int W, int H, int y, const char *s,
                     int selected) {
    int sc = 2;
    int tw = arena_text_width(s, sc);
    int x  = (W - tw) / 2;
    if (selected) {
        arena_blend_rect(blit, W, H, x - 34, y - 5, tw + 68, 26, 0x00305070, 150);
        arena_text_sh(blit, W, H, x - 26, y, ">", 0x00F0D030, sc);
        arena_text_sh(blit, W, H, x + tw + 10, y, "<", 0x00F0D030, sc);
    }
    arena_text_sh(blit, W, H, x, y, s, selected ? 0x00FFFFFF : 0x00A8B8C8, sc);
}

/* ------------------------------------------------------------- main menu */
static void draw_main_menu(World *w, uint32_t *blit, int W, int H) {
    draw_backdrop_ex(blit, W, H, "MAYTERA ARENA", 0, 1);

    char items[MM_COUNT][48], num[16];
    items[MM_PLAY][0] = 0;     mcat(items[MM_PLAY], 48, "Play Deathmatch");
    items[MM_ARENA][0] = 0;    mcat(items[MM_ARENA], 48, "Arena: ");
    {
        int cnt = level_menu_count();
        int li = w->level_index;
        if (li < 0 || li >= cnt) li = 0;
        if (li == NUM_LEVELS)    /* the imported BSP map slot (only when available) */
            mcat(items[MM_ARENA], 48, "de_dust2 (CS 1.6 BSP)");
        else {
            const char *ln = level_name(li);
            mcat(items[MM_ARENA], 48, ln ? ln : "?");
        }
    }
    items[MM_BOTS][0] = 0;     mcat(items[MM_BOTS], 48, "Bots: ");
    arena_itoa(g_arena_cfg.bots, num); mcat(items[MM_BOTS], 48, num);
    items[MM_SETTINGS][0] = 0; mcat(items[MM_SETTINGS], 48, "Settings");
    items[MM_CONTROLS][0] = 0; mcat(items[MM_CONTROLS], 48, "Controls");
    items[MM_QUIT][0] = 0;     mcat(items[MM_QUIT], 48, "Quit");

    int rowh = 30;
    int my = H / 2 - (MM_COUNT * rowh) / 2 + 10;
    for (int i = 0; i < MM_COUNT; i++) {
        draw_row(blit, W, H, my, items[i], i == g_main_sel);
        my += rowh;
    }
    const char *help = "W/S select   A/D change   Enter confirm";
    arena_text_sh(blit, W, H, (W - arena_text_width(help, 1)) / 2,
                  H - 28, help, 0x00708090, 1);
}

/* ------------------------------------------------------------- pause menu */
static void draw_pause(World *w, uint32_t *blit, int W, int H) {
    (void)w;
    draw_backdrop(blit, W, H, "PAUSED", 0);
    const char *items[PM_COUNT] = {
        "Resume", "Settings", "Controls", "Restart Match", "Main Menu", "Quit"
    };
    int rowh = 30;
    int my = H / 2 - (PM_COUNT * rowh) / 2 + 10;
    for (int i = 0; i < PM_COUNT; i++) {
        draw_row(blit, W, H, my, items[i], i == g_pause_sel);
        my += rowh;
    }
    const char *help = "ESC resumes   W/S select   Enter confirm";
    arena_text_sh(blit, W, H, (W - arena_text_width(help, 1)) / 2,
                  H - 28, help, 0x00708090, 1);
}

/* ------------------------------------------------------------- settings */
static void draw_settings(uint32_t *blit, int W, int H) {
    draw_backdrop(blit, W, H, "SETTINGS", 0);
    char rows[ST_COUNT][48], num[16];

    rows[ST_DISPLAY][0] = 0;
    mcat(rows[ST_DISPLAY], 48, "Display: ");
    mcat(rows[ST_DISPLAY], 48, g_arena_cfg.fullscreen ? "Fullscreen" : "Windowed");

    rows[ST_SENS][0] = 0;
    mcat(rows[ST_SENS], 48, "Mouse Sensitivity: ");
    arena_itoa(g_arena_cfg.sensitivity, num); mcat(rows[ST_SENS], 48, num);

    rows[ST_FOV][0] = 0;
    mcat(rows[ST_FOV], 48, "Field of View: ");
    arena_itoa(g_arena_cfg.fov, num); mcat(rows[ST_FOV], 48, num);

    rows[ST_VOLUME][0] = 0;
    mcat(rows[ST_VOLUME], 48, "Volume: ");
    if (g_arena_cfg.volume == 0) mcat(rows[ST_VOLUME], 48, "Mute");
    else { arena_itoa(g_arena_cfg.volume, num); mcat(rows[ST_VOLUME], 48, num); }

    rows[ST_MINIMAP][0] = 0;
    mcat(rows[ST_MINIMAP], 48, "Minimap: ");
    mcat(rows[ST_MINIMAP], 48, g_arena_cfg.minimap ? "On" : "Off");

    rows[ST_BACK][0] = 0; mcat(rows[ST_BACK], 48, "Back");

    int rowh = 30;
    int my = H / 2 - (ST_COUNT * rowh) / 2 + 10;
    for (int i = 0; i < ST_COUNT; i++) {
        draw_row(blit, W, H, my, rows[i], i == g_set_sel);
        my += rowh;
    }
    /* Honest note: the kernel cannot restore window chrome at runtime, so
     * switching back to Windowed only takes full effect on next launch.        */
    const char *note = "A/D change   Windowed <-> Fullscreen applies; return to Windowed needs relaunch";
    arena_text_sh(blit, W, H, (W - arena_text_width(note, 1)) / 2,
                  H - 28, note, 0x00708090, 1);
}

/* ------------------------------------------------------------- controls */
static void draw_controls(uint32_t *blit, int W, int H) {
    draw_backdrop(blit, W, H, "CONTROLS", 0);
    static const char *lines[] = {
        "W / A / S / D    move + strafe",
        "Mouse            look",
        "Left Mouse       fire",
        "Space            jump",
        "1 - 0            select weapon",
        "M                toggle minimap",
        "TAB (hold)       scoreboard",
        "ESC              pause / back",
        0
    };
    int sc = 2, rowh = 26;
    int my = H / 2 - 4 * rowh;
    for (int i = 0; lines[i]; i++) {
        int tw = arena_text_width(lines[i], sc);
        arena_text_sh(blit, W, H, (W - tw) / 2, my, lines[i], 0x00C8D4E0, sc);
        my += rowh;
    }
    const char *help = "Enter or ESC to go back";
    arena_text_sh(blit, W, H, (W - arena_text_width(help, 1)) / 2,
                  H - 28, help, 0x00F0D030, 1);
}

/* Instant loading backdrop: the menu background art (or gradient fallback) with
 * the logo + "Loading...", so the window is never a white flash while r_init +
 * the world spin up. Safe to call BEFORE r_init (uses only pol_load_bmp).      */
void menu_draw_loading(uint32_t *blit, int width, int height) {
    if (!blit || width <= 0 || height <= 0) return;
    draw_backdrop_ex(blit, width, height, "MAYTERA ARENA", "Loading...", 1);
}

/* ==================================================================== */
void menu_draw(World *w, uint32_t *blit, int width, int height) {
    if (!w || !blit || width <= 0 || height <= 0) return;
    switch (w->state) {
    case GS_MENU:       draw_main_menu(w, blit, width, height); break;
    case GS_PAUSED:     draw_pause(w, blit, width, height); break;
    case GS_SETTINGS:   draw_settings(blit, width, height); break;
    case GS_CONTROLS:   draw_controls(blit, width, height); break;
    case GS_SCOREBOARD: draw_scoreboard(w, blit, width, height, 0); break;
    case GS_GAMEOVER:   draw_scoreboard(w, blit, width, height, 1); break;
    default: break;
    }
}

/* TAB overlay while playing: the frag table without the full menu backdrop.   */
void menu_draw_scores(World *w, uint32_t *blit, int width, int height) {
    if (!w || !blit || width <= 0 || height <= 0) return;
    draw_scoreboard(w, blit, width, height, 0);
}

/* ---------------------------------------------------------- key handling */
static void main_adjust(World *w, int dir) {
    switch (g_main_sel) {
    case MM_ARENA: {
        int cnt = level_menu_count();       /* NUM_LEVELS, +1 if de_dust2 loaded */
        int li = w->level_index + dir;
        if (li < 0) li = cnt - 1;
        if (li >= cnt) li = 0;
        w->level_index = li;
    } break;
    case MM_BOTS:
        g_arena_cfg.bots += dir;
        if (g_arena_cfg.bots < 1) g_arena_cfg.bots = MAX_PLAYERS - 1;
        if (g_arena_cfg.bots > MAX_PLAYERS - 1) g_arena_cfg.bots = 1;
        arena_cfg_save();
        break;
    default: break;
    }
}

static void settings_adjust(int dir) {
    switch (g_set_sel) {
    case ST_DISPLAY:
        g_arena_cfg.fullscreen = !g_arena_cfg.fullscreen;
        arena_cfg_save();
        arena_apply_fullscreen();        /* windowed->fullscreen applies live   */
        break;
    case ST_SENS:
        g_arena_cfg.sensitivity += dir;
        arena_cfg_save();
        break;
    case ST_FOV:
        g_arena_cfg.fov += dir * 5;
        arena_cfg_save();                /* clamps + r_set_fov inside save       */
        break;
    case ST_VOLUME:
        g_arena_cfg.volume += dir;
        arena_cfg_save();                /* clamps + set_volume inside save       */
        break;
    case ST_MINIMAP:
        g_arena_cfg.minimap = !g_arena_cfg.minimap;
        arena_cfg_save();
        break;
    default: break;
    }
}

/* Returns 1 if the key was consumed by a menu screen. main.c routes all
 * non-gameplay keys here (and ESC contextually).                              */
int menu_handle_key(World *w, int key) {
    if (!w) return 0;
    int enter = (key == '\r' || key == '\n');
    int up    = (key == 'w' || key == 'W' || key == 0x80 || key == 0x48);
    int down  = (key == 's' || key == 'S' || key == 0x81 || key == 0x50);
    int left  = (key == 'a' || key == 'A' || key == 0x82 || key == 0x4B);
    int right = (key == 'd' || key == 'D' || key == 0x83 || key == 0x4D);
    int esc   = (key == 0x1B);

    switch (w->state) {

    case GS_GAMEOVER:
        if (enter) { w->state = GS_MENU; g_main_sel = MM_PLAY; return 1; }
        return 0;

    case GS_MENU:
        if (up)    { g_main_sel = (g_main_sel + MM_COUNT - 1) % MM_COUNT; return 1; }
        if (down)  { g_main_sel = (g_main_sel + 1) % MM_COUNT; return 1; }
        if (left)  { main_adjust(w, -1); return 1; }
        if (right) { main_adjust(w,  1); return 1; }
        if (enter) {
            switch (g_main_sel) {
            case MM_PLAY:
                /* #491 Stage 2: the extra "Arena: de_dust2 (CS 1.6 BSP)" slot
                 * (index NUM_LEVELS, only offered when bsp_available()) loads
                 * the imported map through the SAME path the B shortcut uses.
                 * Any stale/out-of-range selection (map removed after being
                 * picked) falls back to arena_start_match(), which clamps
                 * level_index itself - never a crash, never a dead selection. */
                if (w->level_index == NUM_LEVELS && bsp_available())
                    arena_start_bsp();
                else
                    arena_start_match();
                break;
            case MM_ARENA:
            case MM_BOTS:     main_adjust(w, 1); break;
            case MM_SETTINGS: g_set_from = GS_MENU; g_set_sel = ST_DISPLAY;
                              w->state = GS_SETTINGS; break;
            case MM_CONTROLS: g_set_from = GS_MENU; w->state = GS_CONTROLS; break;
            case MM_QUIT:     exit(0); break;
            }
            return 1;
        }
        return 0;

    case GS_PAUSED:
        if (esc)   { w->state = GS_PLAYING; return 1; }
        if (up)    { g_pause_sel = (g_pause_sel + PM_COUNT - 1) % PM_COUNT; return 1; }
        if (down)  { g_pause_sel = (g_pause_sel + 1) % PM_COUNT; return 1; }
        if (enter) {
            switch (g_pause_sel) {
            case PM_RESUME:   w->state = GS_PLAYING; break;
            case PM_SETTINGS: g_set_from = GS_PAUSED; g_set_sel = ST_DISPLAY;
                              w->state = GS_SETTINGS; break;
            case PM_CONTROLS: g_set_from = GS_PAUSED; w->state = GS_CONTROLS; break;
            case PM_RESTART:  arena_start_match(); break;
            case PM_MENU:     w->state = GS_MENU; g_main_sel = MM_PLAY; break;
            case PM_QUIT:     exit(0); break;
            }
            return 1;
        }
        return 0;

    case GS_SETTINGS:
        if (esc)   { w->state = g_set_from; return 1; }
        if (up)    { g_set_sel = (g_set_sel + ST_COUNT - 1) % ST_COUNT; return 1; }
        if (down)  { g_set_sel = (g_set_sel + 1) % ST_COUNT; return 1; }
        if (left)  { settings_adjust(-1); return 1; }
        if (right) { settings_adjust( 1); return 1; }
        if (enter) {
            if (g_set_sel == ST_BACK)         w->state = g_set_from;
            else if (g_set_sel == ST_DISPLAY) settings_adjust(1);
            return 1;
        }
        return 0;

    case GS_CONTROLS:
        if (esc || enter) { w->state = g_set_from; return 1; }
        return 0;

    default:
        return 0;
    }
}
