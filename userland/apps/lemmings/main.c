// main.c - Lemmings-style game for MayteraOS (user-space version)
// A simplified Lemmings clone with terrain modification, skill assignment,
// and procedurally generated levels.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"

// ============================================================================
// Constants
// ============================================================================

#define WIN_W           640
#define WIN_H           440
#define TERRAIN_W       640
#define TERRAIN_H       380
#define SKILL_PANEL_H   50
#define MAX_LEMMINGS    50
#define DOOR_W          20
#define DOOR_H          25
#define LEM_W           8
#define LEM_H           12
#define SPAWN_INTERVAL  45
#define WALK_SPEED      1
#define FALL_SPEED      2
#define CLIMB_SPEED     1
#define BUILD_STEPS     12
#define MAX_TERRAIN     80

// Colors
#define COL_BG          0x00000040
#define COL_TERRAIN     0x00804020
#define COL_TERRAIN_TOP 0x00906030
#define COL_LEMMING     0x0000FF00
#define COL_HAIR        0x000080FF
#define COL_SKIN        0x00FFCC99
#define COL_ENTRY       0x00808080
#define COL_EXIT        0x0080FF80
#define COL_BLOCKER     0x00FF0000
#define COL_DIGGER      0x00FFFF00
#define COL_BUILDER     0x00FF8000
#define COL_CLIMBER     0x00FF00FF
#define COL_PANEL_BG    0x00202020
#define COL_PANEL_SEL   0x00404080
#define COL_TEXT         0x00FFFFFF
#define COL_HIGHLIGHT   0x00FFFF00
#define COL_DIM         0x00808080

// ============================================================================
// Types
// ============================================================================

typedef enum {
    STATE_MENU,
    STATE_PLAYING,
    STATE_PAUSED,
    STATE_WON,
    STATE_LOST
} game_state_t;

typedef enum {
    LEM_WALKING,
    LEM_FALLING,
    LEM_DIGGING,
    LEM_BLOCKING,
    LEM_BUILDING,
    LEM_CLIMBING,
    LEM_DEAD,
    LEM_SAVED
} lem_state_t;

typedef enum {
    SKILL_NONE = 0,
    SKILL_DIGGER,
    SKILL_BLOCKER,
    SKILL_BUILDER,
    SKILL_CLIMBER,
    SKILL_COUNT
} skill_t;

// Terrain rectangle (compressed terrain representation)
typedef struct {
    int x, y, w, h;
} rect_t;

// Lemming structure
typedef struct {
    int x, y;
    int dir;            // 1 = right, -1 = left
    lem_state_t state;
    int fall_dist;
    int action_timer;
    int build_count;
    int is_climber;     // permanent climber skill
    int active;
} lemming_t;

// ============================================================================
// Game State (all static)
// ============================================================================

static int win_handle = -1;
static int win_x = 50, win_y = 30;

// Game state
static game_state_t game_state = STATE_MENU;
static int current_level = 1;

// Terrain (compressed as rectangles)
static rect_t terrain[MAX_TERRAIN];
static int terrain_count = 0;

// Lemmings
static lemming_t lemmings[MAX_LEMMINGS];
static int lem_count = 0;        // currently active
static int lems_spawned = 0;     // total spawned so far
static int lems_saved = 0;       // successfully exited
static int lems_dead = 0;        // dead lemmings

// Level parameters
static int lems_to_spawn = 20;
static int lems_to_save = 12;
static int spawn_x = 60, spawn_y = 30;
static int exit_x = 560, exit_y = 300;

// Skills
static int skill_counts[SKILL_COUNT];
static skill_t selected_skill = SKILL_NONE;

// Timing
static int spawn_counter = 0;
static int highlight_lem = -1;

// Random seed
static uint32_t rand_seed = 12345;

// ============================================================================
// Utility Functions
// ============================================================================

static int game_rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

// Integer to string (simple, for status display)
static void int_to_str(int val, char *buf, int buflen) {
    if (buflen < 2) { buf[0] = '\0'; return; }

    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }

    char tmp[16];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0 && i < 15) {
            tmp[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }

    int pos = 0;
    if (neg && pos < buflen - 1) buf[pos++] = '-';
    for (int j = i - 1; j >= 0 && pos < buflen - 1; j--) {
        buf[pos++] = tmp[j];
    }
    buf[pos] = '\0';
}

// ============================================================================
// Terrain Functions
// ============================================================================

static void add_terrain_rect(int x, int y, int w, int h) {
    if (terrain_count >= MAX_TERRAIN) return;
    terrain[terrain_count].x = x;
    terrain[terrain_count].y = y;
    terrain[terrain_count].w = w;
    terrain[terrain_count].h = h;
    terrain_count++;
}

static int is_solid(int x, int y) {
    if (y >= TERRAIN_H) return 1;  // below terrain is solid
    if (x < 0 || x >= TERRAIN_W || y < 0) return 0;

    for (int i = 0; i < terrain_count; i++) {
        if (x >= terrain[i].x && x < terrain[i].x + terrain[i].w &&
            y >= terrain[i].y && y < terrain[i].y + terrain[i].h) {
            return 1;
        }
    }
    return 0;
}

static int has_ground(int x, int y) {
    return is_solid(x, y + 1);
}

// Remove terrain in a rectangular area (for digging).
// This splits existing rects that overlap the removal zone.
static void remove_terrain_area(int rx, int ry, int rw, int rh) {
    int rx2 = rx + rw;
    int ry2 = ry + rh;

    for (int i = 0; i < terrain_count; i++) {
        rect_t *t = &terrain[i];
        int tx2 = t->x + t->w;
        int ty2 = t->y + t->h;

        // Check overlap
        if (t->x >= rx2 || tx2 <= rx || t->y >= ry2 || ty2 <= ry)
            continue;

        // This rect overlaps the removal area; split it into up to 4 pieces
        rect_t orig = *t;

        // Remove this rect (replace with last)
        terrain[i] = terrain[terrain_count - 1];
        terrain_count--;
        i--;

        // Top piece (above removal zone)
        if (orig.y < ry) {
            add_terrain_rect(orig.x, orig.y, orig.w, ry - orig.y);
        }

        // Bottom piece (below removal zone)
        if (ty2 > ry2) {
            add_terrain_rect(orig.x, ry2, orig.w, ty2 - ry2);
        }

        // Left piece (within the vertical overlap)
        int clip_top = (orig.y > ry) ? orig.y : ry;
        int clip_bot = (ty2 < ry2) ? ty2 : ry2;
        if (clip_top < clip_bot) {
            if (orig.x < rx) {
                add_terrain_rect(orig.x, clip_top, rx - orig.x, clip_bot - clip_top);
            }
            // Right piece
            if (tx2 > rx2) {
                add_terrain_rect(rx2, clip_top, tx2 - rx2, clip_bot - clip_top);
            }
        }
    }
}

// Add terrain for building
static void add_build_terrain(int x, int y, int w, int h) {
    add_terrain_rect(x, y, w, h);
}

// ============================================================================
// Level Generation
// ============================================================================

static void generate_level(int level_num) {
    terrain_count = 0;

    // Seed the RNG with the level number for reproducibility
    rand_seed = (uint32_t)(level_num * 7919 + 42);

    // Level parameters
    lems_to_spawn = 20 + level_num * 5;
    if (lems_to_spawn > MAX_LEMMINGS) lems_to_spawn = MAX_LEMMINGS;
    lems_to_save = (lems_to_spawn * 60) / 100;  // 60% required

    // Skill counts scale with level
    skill_counts[SKILL_NONE] = 0;
    skill_counts[SKILL_DIGGER] = 5 + level_num;
    skill_counts[SKILL_BLOCKER] = 3 + level_num / 2;
    skill_counts[SKILL_BUILDER] = 5 + level_num;
    skill_counts[SKILL_CLIMBER] = 3 + level_num / 2;

    // Entry door position
    spawn_x = 40 + (game_rand() % 60);
    spawn_y = 25;

    // Exit door position (bottom right area)
    exit_x = TERRAIN_W - 80 + (game_rand() % 30);
    exit_y = TERRAIN_H - 60;

    // Main ground (thick floor)
    add_terrain_rect(0, TERRAIN_H - 20, TERRAIN_W, 20);

    // Platform under spawn door so lemmings land
    add_terrain_rect(spawn_x - 30, spawn_y + DOOR_H, 80, 12);

    // Middle platforms (varied by level)
    int num_platforms = 3 + level_num % 4;
    for (int p = 0; p < num_platforms; p++) {
        int px = 50 + (game_rand() % (TERRAIN_W - 200));
        int py = 80 + (game_rand() % 160);
        int pw = 60 + (game_rand() % 100);
        int ph = 8 + (game_rand() % 8);
        add_terrain_rect(px, py, pw, ph);
    }

    // Vertical pillars / walls
    int num_pillars = 2 + level_num % 3;
    for (int p = 0; p < num_pillars; p++) {
        int px = 100 + (game_rand() % (TERRAIN_W - 250));
        int py = 90 + (game_rand() % 100);
        int pw = 12 + (game_rand() % 15);
        int ph = 60 + (game_rand() % 80);
        add_terrain_rect(px, py, pw, ph);
    }

    // Ramp from mid-level to ground (aids path to exit)
    int ramp_x = exit_x - 120 - (game_rand() % 60);
    int ramp_y = TERRAIN_H - 80;
    add_terrain_rect(ramp_x, ramp_y, 100, 10);
    add_terrain_rect(ramp_x + 40, ramp_y + 25, 80, 10);

    // Platform under exit door
    add_terrain_rect(exit_x - 15, exit_y + DOOR_H, DOOR_W + 30, 10);
}

// ============================================================================
// Game Reset / Start Level
// ============================================================================

static void start_level(int level) {
    current_level = level;
    game_state = STATE_PLAYING;

    memset(lemmings, 0, sizeof(lemmings));
    lem_count = 0;
    lems_spawned = 0;
    lems_saved = 0;
    lems_dead = 0;
    spawn_counter = 0;
    highlight_lem = -1;
    selected_skill = SKILL_DIGGER;

    generate_level(level);
}

// ============================================================================
// Lemming Spawning
// ============================================================================

static void spawn_lemming(void) {
    if (lems_spawned >= lems_to_spawn) return;

    for (int i = 0; i < MAX_LEMMINGS; i++) {
        if (!lemmings[i].active) {
            lemming_t *lem = &lemmings[i];
            lem->active = 1;
            lem->x = spawn_x + DOOR_W / 2;
            lem->y = spawn_y + DOOR_H - 4;
            lem->state = LEM_FALLING;
            lem->dir = 1;
            lem->fall_dist = 0;
            lem->action_timer = 0;
            lem->build_count = 0;
            lem->is_climber = 0;
            lem_count++;
            lems_spawned++;
            break;
        }
    }
}

// ============================================================================
// Lemming Update Logic
// ============================================================================

static int check_exit_zone(lemming_t *lem) {
    return (lem->x >= exit_x && lem->x < exit_x + DOOR_W &&
            lem->y >= exit_y && lem->y < exit_y + DOOR_H);
}

static void update_lemming(lemming_t *lem) {
    if (!lem->active) return;
    if (lem->state == LEM_DEAD || lem->state == LEM_SAVED) return;

    // Check exit
    if (check_exit_zone(lem)) {
        lem->state = LEM_SAVED;
        lems_saved++;
        lem_count--;
        return;
    }

    // Off the bottom of the map
    if (lem->y >= TERRAIN_H + 50) {
        lem->state = LEM_DEAD;
        lems_dead++;
        lem_count--;
        return;
    }

    switch (lem->state) {
        case LEM_FALLING: {
            if (has_ground(lem->x, lem->y + LEM_H)) {
                if (lem->fall_dist > 80) {
                    // Fatal fall
                    lem->state = LEM_DEAD;
                    lems_dead++;
                    lem_count--;
                } else {
                    lem->state = LEM_WALKING;
                    lem->fall_dist = 0;
                }
            } else {
                lem->y += FALL_SPEED;
                lem->fall_dist += FALL_SPEED;
            }
            break;
        }

        case LEM_WALKING: {
            // Check ground
            if (!has_ground(lem->x, lem->y + LEM_H)) {
                lem->state = LEM_FALLING;
                break;
            }

            int new_x = lem->x + lem->dir * WALK_SPEED;

            // Check for wall ahead
            int wall_check_x = new_x + (lem->dir > 0 ? LEM_W : 0);
            if (is_solid(wall_check_x, lem->y + LEM_H - 1)) {
                // Try stepping up (max 6 pixels)
                int stepped = 0;
                for (int step = 1; step <= 6; step++) {
                    if (!is_solid(wall_check_x, lem->y + LEM_H - 1 - step) &&
                        !is_solid(new_x, lem->y - step)) {
                        lem->x = new_x;
                        lem->y -= step;
                        stepped = 1;
                        break;
                    }
                }
                if (!stepped) {
                    if (lem->is_climber) {
                        lem->state = LEM_CLIMBING;
                    } else {
                        lem->dir = -lem->dir;
                    }
                }
            } else {
                lem->x = new_x;
            }

            // Check for blockers
            for (int i = 0; i < MAX_LEMMINGS; i++) {
                lemming_t *other = &lemmings[i];
                if (other == lem || !other->active || other->state != LEM_BLOCKING)
                    continue;

                int dx = lem->x - other->x;
                if (dx < 0) dx = -dx;
                if (dx < LEM_W + 4) {
                    int dy = lem->y - other->y;
                    if (dy < 0) dy = -dy;
                    if (dy < LEM_H) {
                        lem->dir = -lem->dir;
                        lem->x += lem->dir * 4;
                    }
                }
            }
            break;
        }

        case LEM_CLIMBING: {
            int wall_x = lem->x + (lem->dir > 0 ? LEM_W + 1 : -1);
            if (is_solid(wall_x, lem->y)) {
                lem->y -= CLIMB_SPEED;

                // Reached top?
                if (!is_solid(lem->x + lem->dir * (LEM_W / 2), lem->y - 1)) {
                    lem->x += lem->dir * 4;
                    lem->state = LEM_WALKING;
                }

                // Hit ceiling?
                if (is_solid(lem->x, lem->y - 1)) {
                    lem->state = LEM_FALLING;
                    lem->dir = -lem->dir;
                }
            } else {
                lem->state = LEM_FALLING;
            }
            break;
        }

        case LEM_DIGGING: {
            lem->action_timer++;
            if (lem->action_timer >= 8) {
                lem->action_timer = 0;
                remove_terrain_area(lem->x - 2, lem->y + LEM_H, LEM_W + 4, 3);
                lem->y += 2;

                if (!has_ground(lem->x, lem->y + LEM_H + 2)) {
                    lem->state = LEM_WALKING;
                }
            }
            break;
        }

        case LEM_BUILDING: {
            lem->action_timer++;
            if (lem->action_timer >= 12) {
                lem->action_timer = 0;

                int build_x = lem->x + lem->dir * (LEM_W + lem->build_count * 4);
                int build_y = lem->y + LEM_H - lem->build_count * 2 - 2;
                add_build_terrain(build_x, build_y, 6, 2);

                lem->build_count++;
                lem->y -= 2;
                lem->x += lem->dir * 4;

                if (lem->build_count >= BUILD_STEPS) {
                    lem->state = LEM_WALKING;
                    lem->build_count = 0;
                }

                if (is_solid(lem->x, lem->y - 2)) {
                    lem->state = LEM_WALKING;
                    lem->build_count = 0;
                }
            }
            break;
        }

        case LEM_BLOCKING:
            // Stand still
            break;

        default:
            break;
    }
}

// ============================================================================
// Skill Assignment
// ============================================================================

static void assign_skill_to(int lem_idx) {
    if (lem_idx < 0 || lem_idx >= MAX_LEMMINGS) return;
    lemming_t *lem = &lemmings[lem_idx];
    if (!lem->active) return;

    skill_t skill = selected_skill;
    if (skill == SKILL_NONE || skill_counts[skill] <= 0) return;
    if (lem->state == LEM_BLOCKING) return;
    if (lem->state != LEM_WALKING && lem->state != LEM_FALLING) return;

    switch (skill) {
        case SKILL_DIGGER:
            if (lem->state == LEM_WALKING && has_ground(lem->x, lem->y + LEM_H)) {
                lem->state = LEM_DIGGING;
                lem->action_timer = 0;
                skill_counts[skill]--;
            }
            break;
        case SKILL_BLOCKER:
            if (lem->state == LEM_WALKING) {
                lem->state = LEM_BLOCKING;
                skill_counts[skill]--;
            }
            break;
        case SKILL_BUILDER:
            if (lem->state == LEM_WALKING) {
                lem->state = LEM_BUILDING;
                lem->action_timer = 0;
                lem->build_count = 0;
                skill_counts[skill]--;
            }
            break;
        case SKILL_CLIMBER:
            if (!lem->is_climber) {
                lem->is_climber = 1;
                skill_counts[skill]--;
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// Find lemming under cursor
// ============================================================================

static int find_lemming_at(int mx, int my) {
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        lemming_t *lem = &lemmings[i];
        if (!lem->active) continue;
        if (lem->state == LEM_DEAD || lem->state == LEM_SAVED) continue;

        int dx = mx - lem->x;
        int dy = my - lem->y;
        if (dx >= -4 && dx < LEM_W + 4 &&
            dy >= -4 && dy < LEM_H + 4) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Drawing
// ============================================================================

// Draw the terrain
static void draw_terrain(void) {
    for (int i = 0; i < terrain_count; i++) {
        rect_t *r = &terrain[i];
        // Main body
        gui_fill_rect(win_handle, r->x, r->y, r->w, r->h, COL_TERRAIN);
        // Top edge highlight (1 pixel)
        if (r->h > 1) {
            gui_fill_rect(win_handle, r->x, r->y, r->w, 1, COL_TERRAIN_TOP);
        }
    }
}

// Draw the entry and exit doors
static void draw_doors(void) {
    // Entry door
    gui_fill_rect(win_handle, spawn_x, spawn_y, DOOR_W, DOOR_H, COL_ENTRY);
    gui_draw_rect(win_handle, spawn_x, spawn_y, DOOR_W, DOOR_H, 0x00A0A0A0);
    // Down arrow indicator
    gui_fill_rect(win_handle, spawn_x + DOOR_W / 2, spawn_y + 5,
                  1, DOOR_H - 12, 0x0040FF40);
    gui_fill_rect(win_handle, spawn_x + DOOR_W / 2 - 3,
                  spawn_y + DOOR_H - 10, 7, 1, 0x0040FF40);

    // Exit door
    gui_fill_rect(win_handle, exit_x, exit_y, DOOR_W, DOOR_H, COL_EXIT);
    gui_draw_rect(win_handle, exit_x, exit_y, DOOR_W, DOOR_H, COL_TEXT);
    // "E" letter for exit
    gui_draw_text(win_handle, exit_x + 5, exit_y + 5, "E", COL_TEXT);
}

// Draw a single lemming
static void draw_lemming(lemming_t *lem, int highlighted) {
    if (!lem->active || lem->state == LEM_DEAD || lem->state == LEM_SAVED)
        return;

    int x = lem->x;
    int y = lem->y;

    uint32_t body_color = COL_LEMMING;

    switch (lem->state) {
        case LEM_BLOCKING:  body_color = COL_BLOCKER; break;
        case LEM_DIGGING:   body_color = COL_DIGGER;  break;
        case LEM_BUILDING:  body_color = COL_BUILDER;  break;
        case LEM_CLIMBING:  body_color = COL_CLIMBER;  break;
        default:
            if (lem->is_climber) body_color = COL_CLIMBER;
            break;
    }

    // Highlight ring
    if (highlighted) {
        gui_draw_rect(win_handle, x - 2, y - 2,
                      LEM_W + 4, LEM_H + 4, COL_HIGHLIGHT);
    }

    // Body
    gui_fill_rect(win_handle, x, y + 2, LEM_W, LEM_H - 4, body_color);

    // Hair / head
    gui_fill_rect(win_handle, x + 1, y, LEM_W - 2, 3, COL_HAIR);

    // Face
    gui_fill_rect(win_handle, x + 2, y + 1, 2, 2, COL_SKIN);

    // Direction indicator (small dot)
    if (lem->dir < 0) {
        gui_fill_rect(win_handle, x - 1, y + 3, 1, 1, body_color);
    } else {
        gui_fill_rect(win_handle, x + LEM_W, y + 3, 1, 1, body_color);
    }
}

// Draw the skill panel at the bottom
static void draw_skill_panel(void) {
    int panel_y = TERRAIN_H;

    // Background
    gui_fill_rect(win_handle, 0, panel_y, WIN_W, SKILL_PANEL_H, COL_PANEL_BG);
    gui_fill_rect(win_handle, 0, panel_y, WIN_W, 1, 0x00606060);

    // Skill buttons
    static const char *skill_labels[] = {
        "None", "Dig", "Block", "Build", "Climb"
    };
    int btn_w = 55;
    int btn_h = 38;
    int bx = 10;

    for (int i = 0; i < SKILL_COUNT; i++) {
        uint32_t bg = ((int)selected_skill == i)
                      ? COL_PANEL_SEL : COL_PANEL_BG;
        gui_fill_rect(win_handle, bx, panel_y + 6, btn_w, btn_h, bg);
        gui_draw_rect(win_handle, bx, panel_y + 6, btn_w, btn_h, COL_DIM);

        // Label
        gui_draw_text(win_handle, bx + 4, panel_y + 10,
                      skill_labels[i], COL_TEXT);

        // Count (for skills with counts)
        if (i > 0) {
            char cnt[8];
            int_to_str(skill_counts[i], cnt, sizeof(cnt));
            gui_draw_text(win_handle, bx + 20, panel_y + 28,
                          cnt, COL_HIGHLIGHT);
        }

        bx += btn_w + 5;
    }

    // Status: Saved / Needed
    char info[32];
    char num1[8], num2[8];

    int_to_str(lems_saved, num1, sizeof(num1));
    int_to_str(lems_to_save, num2, sizeof(num2));
    strcpy(info, "Save:");
    strcat(info, num1);
    strcat(info, "/");
    strcat(info, num2);
    gui_draw_text(win_handle, WIN_W - 130, panel_y + 10, info, COL_TEXT);

    int_to_str(lems_spawned, num1, sizeof(num1));
    int_to_str(lems_to_spawn, num2, sizeof(num2));
    strcpy(info, "Out:");
    strcat(info, num1);
    strcat(info, "/");
    strcat(info, num2);
    gui_draw_text(win_handle, WIN_W - 130, panel_y + 28, info, COL_TEXT);

    // Level indicator
    int_to_str(current_level, num1, sizeof(num1));
    strcpy(info, "Lv:");
    strcat(info, num1);
    gui_draw_text(win_handle, WIN_W - 48, panel_y + 19, info, COL_DIM);
}

// Draw centered text on the play area
static void draw_centered_text(int y, const char *text, uint32_t color) {
    int len = (int)strlen(text);
    int x = (WIN_W - len * 8) / 2;
    gui_draw_text(win_handle, x, y, text, color);
}

// Draw the menu screen
static void draw_menu(void) {
    gui_fill_rect(win_handle, 0, 0, WIN_W, WIN_H, COL_BG);

    draw_centered_text(60, "L E M M I N G S", COL_TEXT);
    draw_centered_text(100, "MayteraOS Edition", 0x0060A060);

    draw_centered_text(170, "Press SPACE to start", COL_DIM);

    char lvl_str[32];
    int_to_str(current_level, lvl_str, sizeof(lvl_str));
    draw_centered_text(200, "Level:", COL_TEXT);
    draw_centered_text(220, lvl_str, 0x0080FF80);

    draw_centered_text(260, "+/- to change level", COL_DIM);

    draw_centered_text(320, "Controls:", 0x00606060);
    draw_centered_text(340, "1-4: Select skill   Click: Assign",
                       0x00606060);
    draw_centered_text(360, "P: Pause   R: Restart   ESC: Menu",
                       0x00606060);
}

// Draw the win/lose screen
static void draw_end_screen(void) {
    gui_fill_rect(win_handle, 0, 0, WIN_W, WIN_H, COL_BG);

    const char *title;
    uint32_t tcolor;
    if (game_state == STATE_WON) {
        title = "LEVEL COMPLETE!";
        tcolor = 0x0080FF80;
    } else {
        title = "LEVEL FAILED";
        tcolor = 0x00FF8080;
    }
    draw_centered_text(80, title, tcolor);

    char msg[64];
    char n1[8], n2[8];
    int_to_str(lems_saved, n1, sizeof(n1));
    int_to_str(lems_to_save, n2, sizeof(n2));
    strcpy(msg, "Saved: ");
    strcat(msg, n1);
    strcat(msg, " / ");
    strcat(msg, n2);
    strcat(msg, " needed");
    draw_centered_text(WIN_H / 2, msg, COL_TEXT);

    if (game_state == STATE_WON) {
        draw_centered_text(WIN_H / 2 + 40,
                           "Press SPACE for next level", COL_DIM);
    } else {
        draw_centered_text(WIN_H / 2 + 40,
                           "Press SPACE to retry", COL_DIM);
    }
    draw_centered_text(WIN_H / 2 + 60, "Press ESC for menu", COL_DIM);
}

// Full redraw of the game
static void redraw(void) {
    if (game_state == STATE_MENU) {
        draw_menu();
        return;
    }

    if (game_state == STATE_WON || game_state == STATE_LOST) {
        draw_end_screen();
        return;
    }

    // Clear background
    gui_fill_rect(win_handle, 0, 0, WIN_W, TERRAIN_H, COL_BG);

    // Draw terrain
    draw_terrain();

    // Draw doors
    draw_doors();

    // Draw lemmings
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        draw_lemming(&lemmings[i], (i == highlight_lem));
    }

    // Skill panel
    draw_skill_panel();

    // Pause overlay
    if (game_state == STATE_PAUSED) {
        gui_fill_rect(win_handle, WIN_W / 4, TERRAIN_H / 3,
                      WIN_W / 2, 60, 0x00202040);
        gui_draw_rect(win_handle, WIN_W / 4, TERRAIN_H / 3,
                      WIN_W / 2, 60, COL_TEXT);
        draw_centered_text(TERRAIN_H / 3 + 15, "PAUSED", COL_TEXT);
        draw_centered_text(TERRAIN_H / 3 + 35,
                           "Press P to resume", COL_DIM);
    }
}

// ============================================================================
// Game Update
// ============================================================================

static void game_update(void) {
    if (game_state != STATE_PLAYING) return;

    // Spawn lemmings at intervals
    spawn_counter++;
    if (spawn_counter >= SPAWN_INTERVAL) {
        spawn_counter = 0;
        spawn_lemming();
    }

    // Update all lemmings
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        update_lemming(&lemmings[i]);
    }

    // Win/lose check: all lemmings are done
    if (lems_spawned >= lems_to_spawn && lem_count == 0) {
        if (lems_saved >= lems_to_save) {
            game_state = STATE_WON;
        } else {
            game_state = STATE_LOST;
        }
    }
}

// ============================================================================
// Skill Button Hit Test
// ============================================================================

static int check_skill_click(int mx, int my) {
    int panel_y = TERRAIN_H;
    int btn_w = 55;
    int btn_h = 38;

    if (my < panel_y + 6 || my > panel_y + 6 + btn_h) return -1;

    int bx = 10;
    for (int i = 0; i < SKILL_COUNT; i++) {
        if (mx >= bx && mx < bx + btn_w) return i;
        bx += btn_w + 5;
    }
    return -1;
}

// ============================================================================
// Event Handling
// ============================================================================

static void handle_mouse_down(int mx, int my) {
    if (game_state != STATE_PLAYING) return;

    // Check skill panel first
    int skill_idx = check_skill_click(mx, my);
    if (skill_idx >= 0) {
        selected_skill = (skill_t)skill_idx;
        return;
    }

    // Try to assign skill to a lemming
    int lem_idx = find_lemming_at(mx, my);
    if (lem_idx >= 0) {
        assign_skill_to(lem_idx);
    }
}

static void handle_mouse_move(int mx, int my) {
    if (game_state == STATE_PLAYING) {
        highlight_lem = find_lemming_at(mx, my);
    }
}

static void handle_key(char key, uint32_t keycode) {
    (void)keycode;

    switch (key) {
        case 0x20:  // SPACE
            if (game_state == STATE_MENU) {
                start_level(current_level);
            } else if (game_state == STATE_WON) {
                current_level++;
                if (current_level > 10) current_level = 10;
                start_level(current_level);
            } else if (game_state == STATE_LOST) {
                start_level(current_level);
            }
            break;

        case 0x2B:  // '+'
        case 0x3D:  // '='
            if (game_state == STATE_MENU) {
                current_level++;
                if (current_level > 10) current_level = 10;
            }
            break;

        case 0x2D:  // '-'
        case 0x5F:  // '_'
            if (game_state == STATE_MENU) {
                current_level--;
                if (current_level < 1) current_level = 1;
            }
            break;

        case 0x70:  // 'p'
        case 0x50:  // 'P'
            if (game_state == STATE_PLAYING) {
                game_state = STATE_PAUSED;
            } else if (game_state == STATE_PAUSED) {
                game_state = STATE_PLAYING;
            }
            break;

        case 0x72:  // 'r'
        case 0x52:  // 'R'
            if (game_state == STATE_PLAYING || game_state == STATE_PAUSED) {
                start_level(current_level);
            }
            break;

        case 0x31:  // '1'
            if (game_state == STATE_PLAYING || game_state == STATE_PAUSED)
                selected_skill = SKILL_DIGGER;
            break;
        case 0x32:  // '2'
            if (game_state == STATE_PLAYING || game_state == STATE_PAUSED)
                selected_skill = SKILL_BLOCKER;
            break;
        case 0x33:  // '3'
            if (game_state == STATE_PLAYING || game_state == STATE_PAUSED)
                selected_skill = SKILL_BUILDER;
            break;
        case 0x34:  // '4'
            if (game_state == STATE_PLAYING || game_state == STATE_PAUSED)
                selected_skill = SKILL_CLIMBER;
            break;

        default:
            break;
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Seed RNG from system clock
    rand_seed = (uint32_t)sys_clock();

    // Create the window
    win_handle = win_create("Lemmings", win_x, win_y, WIN_W, WIN_H);
    if (win_handle < 0) {
        return 1;
    }

    // Initial draw
    redraw();

    // Main event loop
    gui_event_t event;
    int running = 1;
    uint64_t last_tick = sys_clock();

    while (running) {
        // Poll events with a short timeout for animation
        int ev = win_get_event(win_handle, &event, 50);

        if (ev != 0) {
            switch (event.type) {
                case EVENT_WINDOW_CLOSE:
                    running = 0;
                    break;

                case EVENT_REDRAW:
                    redraw();
                    break;

                case EVENT_KEY_DOWN:
                    if (event.key_char == 0x1B &&
                        game_state == STATE_MENU) {
                        // ESC on menu exits the app
                        running = 0;
                    } else if (event.key_char == 0x1B) {
                        // ESC during game returns to menu
                        game_state = STATE_MENU;
                        redraw();
                    } else {
                        handle_key(event.key_char, event.keycode);
                    }
                    break;

                case EVENT_MOUSE_DOWN: {
                    // Convert screen coords to local window coords
                    int local_x = event.mouse_x;
                    int local_y = event.mouse_y;
                    handle_mouse_down(local_x, local_y);
                    break;
                }

                case EVENT_MOUSE_MOVE: {
                    int local_x = event.mouse_x;
                    int local_y = event.mouse_y;
                    handle_mouse_move(local_x, local_y);
                    break;
                }

                default:
                    break;
            }
        }

        // Game update at roughly 20 fps (50ms per tick)
        uint64_t now = sys_clock();
        if (now - last_tick >= 50) {
            last_tick = now;
            game_update();
            redraw();
        }
    }

    win_destroy(win_handle);
    sys_exit(0);
    return 0;
}
