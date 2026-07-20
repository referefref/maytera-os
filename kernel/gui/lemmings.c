// lemmings.c - Lemmings-style Game for MayteraOS
#include "lemmings.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "syslog.h"

// External timer
extern volatile uint64_t timer_ticks;

// Colors
#define COLOR_BG            0x00000040  // Dark blue background
#define COLOR_TERRAIN       0x00804020  // Brown terrain
#define COLOR_TERRAIN_EDGE  0x00603010  // Darker terrain edge
#define COLOR_LEMMING       0x0000FF00  // Green lemming body
#define COLOR_LEMMING_HAIR  0x000080FF  // Blue hair
#define COLOR_LEMMING_SKIN  0x00FFCC99  // Skin tone
#define COLOR_ENTRY_DOOR    0x00808080  // Grey entry door
#define COLOR_EXIT_DOOR     0x0080FF80  // Green exit door
#define COLOR_BLOCKER       0x00FF0000  // Red for blockers
#define COLOR_DIGGER        0x00FFFF00  // Yellow for diggers
#define COLOR_BUILDER       0x00FF8000  // Orange for builders
#define COLOR_CLIMBER       0x00FF00FF  // Magenta for climbers
#define COLOR_SKILL_BG      0x00202020  // Skill panel background
#define COLOR_SKILL_SEL     0x00404080  // Selected skill highlight
#define COLOR_TEXT          0x00FFFFFF  // White text
#define COLOR_HIGHLIGHT     0x00FFFF00  // Yellow highlight

// UI dimensions
#define SKILL_PANEL_HEIGHT  50
#define SKILL_BUTTON_WIDTH  50
#define SKILL_BUTTON_HEIGHT 40
#define DOOR_WIDTH          20
#define DOOR_HEIGHT         25

// Simple random number generator
static int lemmings_rand(lemmings_t *game) {
    game->rand_seed = game->rand_seed * 1103515245 + 12345;
    return (game->rand_seed >> 16) & 0x7FFF;
}

// ============================================================================
// Level Generation
// ============================================================================

// Generate a simple level with terrain
static void generate_level(lemmings_t *game, int level_num) {
    // Clear terrain
    memset(game->terrain, 0, sizeof(game->terrain));

    // Level parameters
    game->level.spawn_rate = SPAWN_INTERVAL;
    game->level.lemmings_to_spawn = 20 + level_num * 5;
    if (game->level.lemmings_to_spawn > MAX_LEMMINGS) {
        game->level.lemmings_to_spawn = MAX_LEMMINGS;
    }
    game->level.lemmings_to_save = (game->level.lemmings_to_spawn * 60) / 100; // 60% to win

    // Set skill counts based on level
    game->level.skill_counts[SKILL_NONE] = 0;
    game->level.skill_counts[SKILL_DIGGER] = 5 + level_num;
    game->level.skill_counts[SKILL_BLOCKER] = 3 + level_num / 2;
    game->level.skill_counts[SKILL_BUILDER] = 5 + level_num;
    game->level.skill_counts[SKILL_CLIMBER] = 3 + level_num / 2;

    // Copy to active skill counts
    for (int i = 0; i < SKILL_COUNT; i++) {
        game->skills.count[i] = game->level.skill_counts[i];
    }

    // Entry door at top left area
    game->level.spawn_x = 50 + (lemmings_rand(game) % 50);
    game->level.spawn_y = 30;

    // Exit door at bottom right area
    game->level.exit_x = TERRAIN_WIDTH - 80 + (lemmings_rand(game) % 30);
    game->level.exit_y = TERRAIN_HEIGHT - 40;

    // Generate terrain platforms
    // Main ground
    for (int y = TERRAIN_HEIGHT - 20; y < TERRAIN_HEIGHT; y++) {
        for (int x = 0; x < TERRAIN_WIDTH; x++) {
            game->terrain[y][x] = 1;
        }
    }

    // Platform under spawn
    for (int y = game->level.spawn_y + DOOR_HEIGHT; y < game->level.spawn_y + DOOR_HEIGHT + 15; y++) {
        for (int x = game->level.spawn_x - 30; x < game->level.spawn_x + 50; x++) {
            if (x >= 0 && x < TERRAIN_WIDTH && y >= 0 && y < TERRAIN_HEIGHT) {
                game->terrain[y][x] = 1;
            }
        }
    }

    // Middle platforms (varied based on level)
    int num_platforms = 3 + level_num % 3;
    for (int p = 0; p < num_platforms; p++) {
        int px = 50 + (lemmings_rand(game) % (TERRAIN_WIDTH - 150));
        int py = 80 + (lemmings_rand(game) % 120);
        int pw = 60 + (lemmings_rand(game) % 80);
        int ph = 8 + (lemmings_rand(game) % 8);

        for (int y = py; y < py + ph && y < TERRAIN_HEIGHT; y++) {
            for (int x = px; x < px + pw && x < TERRAIN_WIDTH; x++) {
                if (x >= 0 && y >= 0) {
                    game->terrain[y][x] = 1;
                }
            }
        }
    }

    // Add some vertical obstacles/pillars
    int num_pillars = 2 + level_num % 2;
    for (int p = 0; p < num_pillars; p++) {
        int px = 100 + (lemmings_rand(game) % (TERRAIN_WIDTH - 200));
        int py = 100 + (lemmings_rand(game) % 80);
        int ph = 60 + (lemmings_rand(game) % 60);
        int pw = 15 + (lemmings_rand(game) % 15);

        for (int y = py; y < py + ph && y < TERRAIN_HEIGHT; y++) {
            for (int x = px; x < px + pw && x < TERRAIN_WIDTH; x++) {
                if (x >= 0 && y >= 0) {
                    game->terrain[y][x] = 1;
                }
            }
        }
    }

    // Clear area around exit door
    for (int y = game->level.exit_y - 5; y < game->level.exit_y + DOOR_HEIGHT; y++) {
        for (int x = game->level.exit_x - 5; x < game->level.exit_x + DOOR_WIDTH + 5; x++) {
            if (x >= 0 && x < TERRAIN_WIDTH && y >= 0 && y < TERRAIN_HEIGHT) {
                game->terrain[y][x] = 0;
            }
        }
    }

    // Make sure there's ground under exit
    for (int x = game->level.exit_x - 10; x < game->level.exit_x + DOOR_WIDTH + 10; x++) {
        if (x >= 0 && x < TERRAIN_WIDTH) {
            game->terrain[game->level.exit_y + DOOR_HEIGHT][x] = 1;
            game->terrain[game->level.exit_y + DOOR_HEIGHT + 1][x] = 1;
        }
    }
}

// ============================================================================
// Terrain Functions
// ============================================================================

// Check if a point is solid terrain
static bool is_solid(lemmings_t *game, int x, int y) {
    if (x < 0 || x >= TERRAIN_WIDTH || y < 0 || y >= TERRAIN_HEIGHT) {
        return (y >= TERRAIN_HEIGHT); // Below terrain is solid (prevents falling forever)
    }
    return game->terrain[y][x] != 0;
}

// Check if there's ground under position
static bool has_ground(lemmings_t *game, int x, int y) {
    return is_solid(game, x, y + 1);
}

// Remove terrain (for digging)
static void remove_terrain(lemmings_t *game, int x, int y, int w, int h) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int tx = x + dx;
            int ty = y + dy;
            if (tx >= 0 && tx < TERRAIN_WIDTH && ty >= 0 && ty < TERRAIN_HEIGHT) {
                game->terrain[ty][tx] = 0;
            }
        }
    }
}

// Add terrain (for building)
static void add_terrain(lemmings_t *game, int x, int y, int w, int h) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int tx = x + dx;
            int ty = y + dy;
            if (tx >= 0 && tx < TERRAIN_WIDTH && ty >= 0 && ty < TERRAIN_HEIGHT) {
                game->terrain[ty][tx] = 1;
            }
        }
    }
}

// ============================================================================
// Lemming Functions
// ============================================================================

// Spawn a new lemming
static void spawn_lemming(lemmings_t *game) {
    if (game->lemmings_spawned >= game->level.lemmings_to_spawn) return;

    // Find empty slot
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        if (!game->lemmings[i].active) {
            lemming_t *lem = &game->lemmings[i];
            lem->active = true;
            lem->x = game->level.spawn_x + DOOR_WIDTH / 2;
            lem->y = game->level.spawn_y + DOOR_HEIGHT - 5;
            lem->state = LEMMING_FALLING;
            lem->dir = DIR_RIGHT;
            lem->skill = SKILL_NONE;
            lem->fall_distance = 0;
            lem->action_timer = 0;
            lem->build_count = 0;
            lem->is_climber = false;
            game->lemming_count++;
            game->lemmings_spawned++;
            break;
        }
    }
}

// Check if lemming reached exit
static bool check_exit(lemmings_t *game, lemming_t *lem) {
    int ex = game->level.exit_x;
    int ey = game->level.exit_y;
    return (lem->x >= ex && lem->x < ex + DOOR_WIDTH &&
            lem->y >= ey && lem->y < ey + DOOR_HEIGHT);
}

// Update a single lemming
static void update_lemming(lemmings_t *game, lemming_t *lem) {
    if (!lem->active) return;
    if (lem->state == LEMMING_DEAD || lem->state == LEMMING_SAVED) return;

    // Check for exit
    if (check_exit(game, lem)) {
        lem->state = LEMMING_SAVED;
        game->lemmings_saved++;
        game->lemming_count--;
        return;
    }

    // Check for falling off map
    if (lem->y >= TERRAIN_HEIGHT + 50) {
        lem->state = LEMMING_DEAD;
        game->lemmings_dead++;
        game->lemming_count--;
        return;
    }

    switch (lem->state) {
        case LEMMING_FALLING: {
            // Check if hit ground
            if (has_ground(game, lem->x, lem->y + LEMMING_HEIGHT)) {
                // Check fall damage
                if (lem->fall_distance > 80) {
                    lem->state = LEMMING_DEAD;
                    game->lemmings_dead++;
                    game->lemming_count--;
                } else {
                    lem->state = LEMMING_WALKING;
                    lem->fall_distance = 0;
                }
            } else {
                lem->y += LEMMING_FALL_SPEED;
                lem->fall_distance += LEMMING_FALL_SPEED;
            }
            break;
        }

        case LEMMING_WALKING: {
            // Check if still on ground
            if (!has_ground(game, lem->x, lem->y + LEMMING_HEIGHT)) {
                lem->state = LEMMING_FALLING;
                break;
            }

            // Try to move forward
            int new_x = lem->x + lem->dir * LEMMING_WALK_SPEED;

            // Check for wall ahead
            if (is_solid(game, new_x + (lem->dir > 0 ? LEMMING_WIDTH : 0), lem->y + LEMMING_HEIGHT - 1)) {
                // Try to step up (max 6 pixels)
                bool stepped = false;
                for (int step = 1; step <= 6; step++) {
                    if (!is_solid(game, new_x + (lem->dir > 0 ? LEMMING_WIDTH : 0), lem->y + LEMMING_HEIGHT - 1 - step) &&
                        !is_solid(game, new_x, lem->y - step)) {
                        lem->x = new_x;
                        lem->y -= step;
                        stepped = true;
                        break;
                    }
                }

                if (!stepped) {
                    // Can't step up - if climber, start climbing
                    if (lem->is_climber) {
                        lem->state = LEMMING_CLIMBING;
                    } else {
                        // Turn around
                        lem->dir = -lem->dir;
                    }
                }
            } else {
                lem->x = new_x;
            }

            // Check for blockers
            for (int i = 0; i < MAX_LEMMINGS; i++) {
                lemming_t *other = &game->lemmings[i];
                if (other != lem && other->active && other->state == LEMMING_BLOCKING) {
                    int dx = lem->x - other->x;
                    if (dx < 0) dx = -dx;
                    if (dx < LEMMING_WIDTH + 4) {
                        int dy = lem->y - other->y;
                        if (dy < 0) dy = -dy;
                        if (dy < LEMMING_HEIGHT) {
                            // Blocked! Turn around
                            lem->dir = -lem->dir;
                            lem->x += lem->dir * 4; // Push away
                        }
                    }
                }
            }
            break;
        }

        case LEMMING_CLIMBING: {
            // Climb up
            if (is_solid(game, lem->x + (lem->dir > 0 ? LEMMING_WIDTH + 1 : -1), lem->y)) {
                // Still wall to climb
                lem->y -= LEMMING_CLIMB_SPEED;

                // Check if reached top
                if (!is_solid(game, lem->x + lem->dir * (LEMMING_WIDTH / 2), lem->y - 1)) {
                    // Pull up onto ledge
                    lem->x += lem->dir * 4;
                    lem->state = LEMMING_WALKING;
                }

                // Check if hit ceiling
                if (is_solid(game, lem->x, lem->y - 1)) {
                    lem->state = LEMMING_FALLING;
                    lem->dir = -lem->dir;
                }
            } else {
                // Wall ended, fall
                lem->state = LEMMING_FALLING;
            }
            break;
        }

        case LEMMING_DIGGING: {
            lem->action_timer++;
            if (lem->action_timer >= 8) {
                lem->action_timer = 0;
                // Dig down
                remove_terrain(game, lem->x - 2, lem->y + LEMMING_HEIGHT, LEMMING_WIDTH + 4, 3);
                lem->y += 2;

                // Check if broke through
                if (!has_ground(game, lem->x, lem->y + LEMMING_HEIGHT + 2)) {
                    lem->state = LEMMING_WALKING;
                }
            }
            break;
        }

        case LEMMING_BUILDING: {
            lem->action_timer++;
            if (lem->action_timer >= 12) {
                lem->action_timer = 0;

                // Build a step
                int build_x = lem->x + lem->dir * (LEMMING_WIDTH + lem->build_count * 4);
                int build_y = lem->y + LEMMING_HEIGHT - lem->build_count * 2 - 2;
                add_terrain(game, build_x, build_y, 6, 2);

                lem->build_count++;
                lem->y -= 2;
                lem->x += lem->dir * 4;

                if (lem->build_count >= BUILDER_STEP_COUNT) {
                    lem->state = LEMMING_WALKING;
                    lem->build_count = 0;
                }

                // Check if hit ceiling
                if (is_solid(game, lem->x, lem->y - 2)) {
                    lem->state = LEMMING_WALKING;
                    lem->build_count = 0;
                }
            }
            break;
        }

        case LEMMING_BLOCKING:
            // Blockers just stand still
            break;

        default:
            break;
    }
}

// ============================================================================
// Game Control
// ============================================================================

// Create game
lemmings_t *lemmings_create(void) {
    lemmings_t *game = (lemmings_t *)kmalloc(sizeof(lemmings_t));
    if (!game) return NULL;

    memset(game, 0, sizeof(lemmings_t));

    // Create window
    game->window = window_create("Lemmings", 80, 40, 450, 380);
    if (!game->window) {
        kfree(game);
        return NULL;
    }

    // Initialize random seed
    game->rand_seed = timer_ticks;

    // Start at menu
    game->state = LEMMINGS_STATE_MENU;
    game->selected_skill = SKILL_NONE;
    game->highlight_lemming = -1;
    game->current_level = 1;

    return game;
}

// Destroy game
void lemmings_destroy(lemmings_t *game) {
    if (!game) return;
    if (game->window) {
        window_destroy(game->window);
    }
    kfree(game);
}

// Start a level
void lemmings_start_level(lemmings_t *game, int level) {
    if (!game) return;

    game->current_level = level;
    game->state = LEMMINGS_STATE_PLAYING;

    // Clear lemmings
    memset(game->lemmings, 0, sizeof(game->lemmings));
    game->lemming_count = 0;
    game->lemmings_spawned = 0;
    game->lemmings_saved = 0;
    game->lemmings_dead = 0;

    // Generate level
    generate_level(game, level);

    // Reset timing
    game->last_update = timer_ticks;
    game->last_spawn = timer_ticks;
    game->spawn_counter = 0;

    // Default selected skill
    game->selected_skill = SKILL_DIGGER;
    game->highlight_lemming = -1;
}

// Pause/resume
void lemmings_pause(lemmings_t *game) {
    if (game && game->state == LEMMINGS_STATE_PLAYING) {
        game->state = LEMMINGS_STATE_PAUSED;
    }
}

void lemmings_resume(lemmings_t *game) {
    if (game && game->state == LEMMINGS_STATE_PAUSED) {
        game->state = LEMMINGS_STATE_PLAYING;
        game->last_update = timer_ticks;
    }
}

// ============================================================================
// Update
// ============================================================================

void lemmings_update(lemmings_t *game) {
    if (!game || game->state != LEMMINGS_STATE_PLAYING) return;

    // Rate limit updates
    if (timer_ticks - game->last_update < 55) return;
    game->last_update = timer_ticks;

    // Spawn lemmings
    game->spawn_counter++;
    if (game->spawn_counter >= game->level.spawn_rate) {
        game->spawn_counter = 0;
        spawn_lemming(game);
    }

    // Update all lemmings
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        update_lemming(game, &game->lemmings[i]);
    }

    // Check win/lose conditions
    if (game->lemmings_spawned >= game->level.lemmings_to_spawn &&
        game->lemming_count == 0) {
        // All lemmings processed
        if (game->lemmings_saved >= game->level.lemmings_to_save) {
            game->state = LEMMINGS_STATE_WON;
        } else {
            game->state = LEMMINGS_STATE_LOST;
        }
    }
}

// ============================================================================
// Drawing
// ============================================================================

// Draw text helper
static void draw_text(int x, int y, const char *text, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw centered text
static void draw_text_centered(int x, int y, int w, const char *text, uint32_t color) {
    int len = strlen(text);
    draw_text(x + (w - len * 8) / 2, y, text, color);
}

// Draw a lemming
static void draw_lemming(lemmings_t *game, lemming_t *lem, int ox, int oy, bool highlighted) {
    (void)game;  // Unused - may be used for future features
    if (!lem->active || lem->state == LEMMING_DEAD || lem->state == LEMMING_SAVED) return;

    int x = ox + lem->x;
    int y = oy + lem->y;

    uint32_t body_color = COLOR_LEMMING;

    // Color based on state/skill
    switch (lem->state) {
        case LEMMING_BLOCKING: body_color = COLOR_BLOCKER; break;
        case LEMMING_DIGGING:  body_color = COLOR_DIGGER; break;
        case LEMMING_BUILDING: body_color = COLOR_BUILDER; break;
        case LEMMING_CLIMBING: body_color = COLOR_CLIMBER; break;
        default:
            if (lem->is_climber) body_color = COLOR_CLIMBER;
            break;
    }

    // Highlight if selected
    if (highlighted) {
        fb_draw_rect(x - 2, y - 2, LEMMING_WIDTH + 4, LEMMING_HEIGHT + 4, COLOR_HIGHLIGHT);
    }

    // Draw body (simple rectangle for now)
    fb_fill_rect(x, y + 2, LEMMING_WIDTH, LEMMING_HEIGHT - 4, body_color);

    // Draw head/hair
    fb_fill_rect(x + 1, y, LEMMING_WIDTH - 2, 3, COLOR_LEMMING_HAIR);

    // Draw face
    fb_fill_rect(x + 2, y + 1, 2, 2, COLOR_LEMMING_SKIN);

    // Draw feet (walking animation)
    if (lem->state == LEMMING_WALKING) {
        int anim = (timer_ticks / 275) % 2;
        if (anim) {
            fb_put_pixel(x + 1, y + LEMMING_HEIGHT - 1, COLOR_LEMMING_SKIN);
            fb_put_pixel(x + LEMMING_WIDTH - 2, y + LEMMING_HEIGHT - 2, COLOR_LEMMING_SKIN);
        } else {
            fb_put_pixel(x + 1, y + LEMMING_HEIGHT - 2, COLOR_LEMMING_SKIN);
            fb_put_pixel(x + LEMMING_WIDTH - 2, y + LEMMING_HEIGHT - 1, COLOR_LEMMING_SKIN);
        }
    }

    // Direction indicator
    if (lem->dir == DIR_LEFT) {
        fb_put_pixel(x - 1, y + 3, body_color);
    } else {
        fb_put_pixel(x + LEMMING_WIDTH, y + 3, body_color);
    }
}

// Draw terrain
static void draw_terrain(lemmings_t *game, int ox, int oy) {
    for (int y = 0; y < TERRAIN_HEIGHT; y++) {
        for (int x = 0; x < TERRAIN_WIDTH; x++) {
            if (game->terrain[y][x]) {
                // Check if edge (for shading)
                bool is_top = (y == 0 || !game->terrain[y-1][x]);
                uint32_t color = is_top ? COLOR_TERRAIN : COLOR_TERRAIN_EDGE;
                fb_put_pixel(ox + x, oy + y, color);
            }
        }
    }
}

// Draw doors
static void draw_doors(lemmings_t *game, int ox, int oy) {
    // Entry door
    int ex = ox + game->level.spawn_x;
    int ey = oy + game->level.spawn_y;
    fb_fill_rect(ex, ey, DOOR_WIDTH, DOOR_HEIGHT, COLOR_ENTRY_DOOR);
    fb_draw_rect(ex, ey, DOOR_WIDTH, DOOR_HEIGHT, 0x00A0A0A0);
    // Arrow down
    fb_draw_line(ex + DOOR_WIDTH/2, ey + 5, ex + DOOR_WIDTH/2, ey + DOOR_HEIGHT - 5, 0x0040FF40);
    fb_draw_line(ex + DOOR_WIDTH/2 - 5, ey + DOOR_HEIGHT - 10, ex + DOOR_WIDTH/2, ey + DOOR_HEIGHT - 5, 0x0040FF40);
    fb_draw_line(ex + DOOR_WIDTH/2 + 5, ey + DOOR_HEIGHT - 10, ex + DOOR_WIDTH/2, ey + DOOR_HEIGHT - 5, 0x0040FF40);

    // Exit door
    ex = ox + game->level.exit_x;
    ey = oy + game->level.exit_y;
    fb_fill_rect(ex, ey, DOOR_WIDTH, DOOR_HEIGHT, COLOR_EXIT_DOOR);
    fb_draw_rect(ex, ey, DOOR_WIDTH, DOOR_HEIGHT, 0x00FFFFFF);
    // Home icon (simple house shape)
    fb_draw_line(ex + DOOR_WIDTH/2, ey + 3, ex + 3, ey + 10, 0x00FFFFFF);
    fb_draw_line(ex + DOOR_WIDTH/2, ey + 3, ex + DOOR_WIDTH - 3, ey + 10, 0x00FFFFFF);
    fb_fill_rect(ex + 5, ey + 10, DOOR_WIDTH - 10, DOOR_HEIGHT - 13, 0x00FFFFFF);
}

// Draw skill panel
static void draw_skill_panel(lemmings_t *game, int x, int y, int w) {
    // Background
    fb_fill_rect(x, y, w, SKILL_PANEL_HEIGHT, COLOR_SKILL_BG);
    fb_draw_line(x, y, x + w, y, 0x00606060);

    // Skill buttons
    const char *skill_names[] = {"None", "Dig", "Block", "Build", "Climb"};
    int bx = x + 10;

    for (int i = 0; i < SKILL_COUNT; i++) {
        uint32_t bg = ((int)game->selected_skill == i) ? COLOR_SKILL_SEL : COLOR_SKILL_BG;
        fb_fill_rect(bx, y + 5, SKILL_BUTTON_WIDTH, SKILL_BUTTON_HEIGHT, bg);
        fb_draw_rect(bx, y + 5, SKILL_BUTTON_WIDTH, SKILL_BUTTON_HEIGHT, 0x00808080);

        // Skill name
        draw_text(bx + 5, y + 10, skill_names[i], COLOR_TEXT);

        // Count
        if (i > 0) {
            char count[8];
            itoa(game->skills.count[i], count, 10);
            draw_text(bx + 20, y + 28, count, 0x00FFFF00);
        }

        bx += SKILL_BUTTON_WIDTH + 5;
    }

    // Status info
    char info[64];
    strcpy(info, "Save:");
    char num[8];
    itoa(game->lemmings_saved, num, 10);
    strcat(info, num);
    strcat(info, "/");
    itoa(game->level.lemmings_to_save, num, 10);
    strcat(info, num);
    draw_text(x + w - 100, y + 10, info, COLOR_TEXT);

    strcpy(info, "Out:");
    itoa(game->lemmings_spawned, num, 10);
    strcat(info, num);
    strcat(info, "/");
    itoa(game->level.lemmings_to_spawn, num, 10);
    strcat(info, num);
    draw_text(x + w - 100, y + 28, info, COLOR_TEXT);
}

// Draw menu
static void draw_menu(lemmings_t *game, int x, int y, int w, int h) {
    // Title
    draw_text_centered(x, y + 40, w, "L E M M I N G S", COLOR_TEXT);

    // Instructions
    draw_text_centered(x, y + h/2 - 40, w, "Press SPACE to start", 0x00808080);
    draw_text_centered(x, y + h/2 - 10, w, "Level:", COLOR_TEXT);
    char lvl[8];
    itoa(game->current_level, lvl, 10);
    draw_text_centered(x, y + h/2 + 10, w, lvl, 0x0080FF80);

    draw_text_centered(x, y + h/2 + 50, w, "+/- to change level", 0x00808080);

    // Controls
    draw_text_centered(x, y + h - 80, w, "Controls:", 0x00606060);
    draw_text_centered(x, y + h - 60, w, "1-4: Select skill  Click: Assign", 0x00606060);
    draw_text_centered(x, y + h - 40, w, "P: Pause  ESC: Menu", 0x00606060);
}

// Draw game over screen
static void draw_end_screen(lemmings_t *game, int x, int y, int w, int h) {
    const char *title = (game->state == LEMMINGS_STATE_WON) ? "LEVEL COMPLETE!" : "LEVEL FAILED";
    uint32_t title_color = (game->state == LEMMINGS_STATE_WON) ? 0x0080FF80 : 0x00FF8080;
    draw_text_centered(x, y + 60, w, title, title_color);

    char msg[64];
    strcpy(msg, "Saved: ");
    char num[8];
    itoa(game->lemmings_saved, num, 10);
    strcat(msg, num);
    strcat(msg, " / ");
    itoa(game->level.lemmings_to_save, num, 10);
    strcat(msg, num);
    strcat(msg, " needed");
    draw_text_centered(x, y + h/2, w, msg, COLOR_TEXT);

    if (game->state == LEMMINGS_STATE_WON) {
        draw_text_centered(x, y + h/2 + 40, w, "Press SPACE for next level", 0x00808080);
    } else {
        draw_text_centered(x, y + h/2 + 40, w, "Press SPACE to retry", 0x00808080);
    }
    draw_text_centered(x, y + h/2 + 60, w, "Press ESC for menu", 0x00808080);
}

// Main draw function
void lemmings_draw(lemmings_t *game) {
    if (!game || !game->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(game->window, &wx, &wy, &ww, &wh);

    game->area_x = wx;
    game->area_y = wy;
    game->area_w = ww;
    game->area_h = wh;

    // Update if playing
    lemmings_update(game);

    // Clear background
    fb_fill_rect(wx, wy, ww, wh, COLOR_BG);

    if (game->state == LEMMINGS_STATE_MENU) {
        draw_menu(game, wx, wy, ww, wh);
        return;
    }

    if (game->state == LEMMINGS_STATE_WON || game->state == LEMMINGS_STATE_LOST) {
        draw_end_screen(game, wx, wy, ww, wh);
        return;
    }

    // Calculate terrain offset (center in window)
    int terrain_x = wx + (ww - TERRAIN_WIDTH) / 2;
    int terrain_y = wy + 5;

    // Draw terrain
    draw_terrain(game, terrain_x, terrain_y);

    // Draw doors
    draw_doors(game, terrain_x, terrain_y);

    // Draw lemmings
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        bool highlighted = (i == game->highlight_lemming);
        draw_lemming(game, &game->lemmings[i], terrain_x, terrain_y, highlighted);
    }

    // Draw skill panel
    draw_skill_panel(game, wx, wy + wh - SKILL_PANEL_HEIGHT, ww);

    // Pause overlay
    if (game->state == LEMMINGS_STATE_PAUSED) {
        fb_fill_rect(wx + ww/4, wy + wh/3, ww/2, 60, 0x00202040);
        fb_draw_rect(wx + ww/4, wy + wh/3, ww/2, 60, COLOR_TEXT);
        draw_text_centered(wx + ww/4, wy + wh/3 + 15, ww/2, "PAUSED", COLOR_TEXT);
        draw_text_centered(wx + ww/4, wy + wh/3 + 35, ww/2, "Press P to resume", 0x00808080);
    }
}

// ============================================================================
// Event Handling
// ============================================================================

// Find lemming at position
static int find_lemming_at(lemmings_t *game, int mx, int my) {
    int terrain_x = game->area_x + (game->area_w - TERRAIN_WIDTH) / 2;
    int terrain_y = game->area_y + 5;

    // Convert to terrain coords
    int tx = mx - terrain_x;
    int ty = my - terrain_y;

    // Find lemming near this position
    for (int i = 0; i < MAX_LEMMINGS; i++) {
        lemming_t *lem = &game->lemmings[i];
        if (!lem->active) continue;
        if (lem->state == LEMMING_DEAD || lem->state == LEMMING_SAVED) continue;

        int dx = tx - lem->x;
        int dy = ty - lem->y;
        if (dx >= -4 && dx < LEMMING_WIDTH + 4 &&
            dy >= -4 && dy < LEMMING_HEIGHT + 4) {
            return i;
        }
    }

    return -1;
}

// Assign skill to lemming
static void assign_skill(lemmings_t *game, int lemming_idx) {
    if (lemming_idx < 0 || lemming_idx >= MAX_LEMMINGS) return;
    lemming_t *lem = &game->lemmings[lemming_idx];
    if (!lem->active) return;

    lemming_skill_t skill = game->selected_skill;
    if (skill == SKILL_NONE) return;
    if (game->skills.count[skill] <= 0) return;

    // Can't assign to blocker or if already has permanent skill
    if (lem->state == LEMMING_BLOCKING) return;

    // Can only assign skill when walking
    if (lem->state != LEMMING_WALKING && lem->state != LEMMING_FALLING) return;

    // Assign the skill
    switch (skill) {
        case SKILL_DIGGER:
            if (lem->state == LEMMING_WALKING && has_ground(game, lem->x, lem->y + LEMMING_HEIGHT)) {
                lem->state = LEMMING_DIGGING;
                lem->action_timer = 0;
                game->skills.count[skill]--;
            }
            break;

        case SKILL_BLOCKER:
            if (lem->state == LEMMING_WALKING) {
                lem->state = LEMMING_BLOCKING;
                game->skills.count[skill]--;
            }
            break;

        case SKILL_BUILDER:
            if (lem->state == LEMMING_WALKING) {
                lem->state = LEMMING_BUILDING;
                lem->action_timer = 0;
                lem->build_count = 0;
                game->skills.count[skill]--;
            }
            break;

        case SKILL_CLIMBER:
            if (!lem->is_climber) {
                lem->is_climber = true;
                game->skills.count[skill]--;
            }
            break;

        default:
            break;
    }
}

// Check if click is on skill button
static int check_skill_button(lemmings_t *game, int mx, int my) {
    int panel_y = game->area_y + game->area_h - SKILL_PANEL_HEIGHT;
    if (my < panel_y + 5 || my > panel_y + 5 + SKILL_BUTTON_HEIGHT) return -1;

    int bx = game->area_x + 10;
    for (int i = 0; i < SKILL_COUNT; i++) {
        if (mx >= bx && mx < bx + SKILL_BUTTON_WIDTH) {
            return i;
        }
        bx += SKILL_BUTTON_WIDTH + 5;
    }
    return -1;
}

void lemmings_handle_event(lemmings_t *game, gui_event_t *event) {
    if (!game || !event) return;

    int mx = event->mouse_x;
    int my = event->mouse_y;

    if (event->type == EVENT_MOUSE_MOVE) {
        game->mouse_x = mx;
        game->mouse_y = my;

        // Highlight lemming under cursor
        if (game->state == LEMMINGS_STATE_PLAYING) {
            game->highlight_lemming = find_lemming_at(game, mx, my);
        }
    }
    else if (event->type == EVENT_MOUSE_DOWN) {
        if (game->state == LEMMINGS_STATE_PLAYING) {
            // Check skill buttons first
            int skill_idx = check_skill_button(game, mx, my);
            if (skill_idx >= 0) {
                game->selected_skill = skill_idx;
            } else {
                // Try to assign skill to lemming
                int lem_idx = find_lemming_at(game, mx, my);
                if (lem_idx >= 0) {
                    assign_skill(game, lem_idx);
                }
            }
        }
    }
    else if (event->type == EVENT_KEY_DOWN) {
        switch (event->keycode) {
            case ' ':
                if (game->state == LEMMINGS_STATE_MENU) {
                    lemmings_start_level(game, game->current_level);
                } else if (game->state == LEMMINGS_STATE_WON) {
                    game->current_level++;
                    lemmings_start_level(game, game->current_level);
                } else if (game->state == LEMMINGS_STATE_LOST) {
                    lemmings_start_level(game, game->current_level);
                }
                break;

            case '+': case '=':
                if (game->state == LEMMINGS_STATE_MENU) {
                    game->current_level++;
                    if (game->current_level > 10) game->current_level = 10;
                }
                break;

            case '-': case '_':
                if (game->state == LEMMINGS_STATE_MENU) {
                    game->current_level--;
                    if (game->current_level < 1) game->current_level = 1;
                }
                break;

            case 'p': case 'P':
                if (game->state == LEMMINGS_STATE_PLAYING) {
                    lemmings_pause(game);
                } else if (game->state == LEMMINGS_STATE_PAUSED) {
                    lemmings_resume(game);
                }
                break;

            case '1':
                if (game->state == LEMMINGS_STATE_PLAYING || game->state == LEMMINGS_STATE_PAUSED) {
                    game->selected_skill = SKILL_DIGGER;
                }
                break;
            case '2':
                if (game->state == LEMMINGS_STATE_PLAYING || game->state == LEMMINGS_STATE_PAUSED) {
                    game->selected_skill = SKILL_BLOCKER;
                }
                break;
            case '3':
                if (game->state == LEMMINGS_STATE_PLAYING || game->state == LEMMINGS_STATE_PAUSED) {
                    game->selected_skill = SKILL_BUILDER;
                }
                break;
            case '4':
                if (game->state == LEMMINGS_STATE_PLAYING || game->state == LEMMINGS_STATE_PAUSED) {
                    game->selected_skill = SKILL_CLIMBER;
                }
                break;

            case 0x1B:  // Escape
                if (game->state == LEMMINGS_STATE_PLAYING || game->state == LEMMINGS_STATE_PAUSED) {
                    game->state = LEMMINGS_STATE_MENU;
                } else if (game->state == LEMMINGS_STATE_WON || game->state == LEMMINGS_STATE_LOST) {
                    game->state = LEMMINGS_STATE_MENU;
                }
                break;
        }
    }
}

// ============================================================================
// Launch
// ============================================================================

void lemmings_launch(void) {
    LOG_INFO("[Lemmings] Game launched");
    lemmings_t *game = lemmings_create();
    if (!game) {
        LOG_ERROR("[Lemmings] Failed to create game");
        kprintf("[Lemmings] Failed to create game\n");
        return;
    }

    // Register with window manager
    wm_register_app(game->window, game,
                    (app_event_handler_t)lemmings_handle_event,
                    (app_draw_handler_t)lemmings_draw,
                    (app_destroy_handler_t)lemmings_destroy);

    kprintf("[Lemmings] Game launched\n");
}
