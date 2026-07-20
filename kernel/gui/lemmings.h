// lemmings.h - Lemmings-style Game for MayteraOS
#ifndef LEMMINGS_H
#define LEMMINGS_H

#include "window.h"

// Game constants
#define LEMMING_WIDTH       8
#define LEMMING_HEIGHT      12
#define MAX_LEMMINGS        50
#define SPAWN_INTERVAL      60      // Ticks between spawns (about 0.6 sec at 100Hz)
#define LEMMING_WALK_SPEED  1
#define LEMMING_FALL_SPEED  2
#define LEMMING_CLIMB_SPEED 1
#define LEMMING_DIG_SPEED   1
#define BUILDER_STEP_COUNT  12      // Steps builder can place

// Terrain dimensions
#define TERRAIN_WIDTH       400
#define TERRAIN_HEIGHT      280

// Fixed-point math (8.8 format) - Prefixed to avoid conflict with doom.h
#define LEM_FP_SHIFT    8
#define LEM_FP_ONE      (1 << LEM_FP_SHIFT)
#define LEM_FP_HALF     (LEM_FP_ONE / 2)
#define LEM_INT_TO_FP(x)    ((x) << LEM_FP_SHIFT)
#define LEM_FP_TO_INT(x)    ((x) >> LEM_FP_SHIFT)

// Lemming states
typedef enum {
    LEMMING_WALKING,
    LEMMING_FALLING,
    LEMMING_DIGGING,
    LEMMING_BLOCKING,
    LEMMING_BUILDING,
    LEMMING_CLIMBING,
    LEMMING_DEAD,
    LEMMING_SAVED
} lemming_state_t;

// Lemming skills (assignable)
typedef enum {
    SKILL_NONE = 0,
    SKILL_DIGGER,
    SKILL_BLOCKER,
    SKILL_BUILDER,
    SKILL_CLIMBER,
    SKILL_COUNT
} lemming_skill_t;

// Direction
typedef enum {
    DIR_RIGHT = 1,
    DIR_LEFT = -1
} lemming_dir_t;

// Lemming structure
typedef struct {
    int x, y;               // Position (fixed-point)
    lemming_state_t state;
    lemming_dir_t dir;      // Walking direction
    lemming_skill_t skill;  // Permanent skill (climber)
    int fall_distance;      // Frames falling (for death check)
    int action_timer;       // Timer for building/digging
    int build_count;        // Steps built
    bool is_climber;        // Has climber skill permanently
    bool active;            // Is this lemming active?
} lemming_t;

// Game states
typedef enum {
    LEMMINGS_STATE_MENU,
    LEMMINGS_STATE_PLAYING,
    LEMMINGS_STATE_PAUSED,
    LEMMINGS_STATE_WON,
    LEMMINGS_STATE_LOST
} lemmings_state_t;

// Currently selected skill
typedef struct {
    lemming_skill_t skill;
    int count[SKILL_COUNT]; // Available count for each skill
} skill_panel_t;

// Level structure
typedef struct {
    int spawn_x, spawn_y;       // Entry door position
    int exit_x, exit_y;         // Exit door position
    int lemmings_to_spawn;      // Total lemmings in level
    int lemmings_to_save;       // Required to win (percentage)
    int spawn_rate;             // Ticks between spawns
    int skill_counts[SKILL_COUNT]; // Available skills
} level_t;

// Main game state
typedef struct {
    window_t *window;

    // Game state
    lemmings_state_t state;

    // Lemmings
    lemming_t lemmings[MAX_LEMMINGS];
    int lemming_count;          // Active lemmings
    int lemmings_spawned;       // Total spawned
    int lemmings_saved;         // Successfully exited
    int lemmings_dead;          // Dead lemmings

    // Level data
    level_t level;

    // Terrain (1 = solid, 0 = air)
    uint8_t terrain[TERRAIN_HEIGHT][TERRAIN_WIDTH];

    // Skill panel
    skill_panel_t skills;
    lemming_skill_t selected_skill;

    // Game area
    int area_x, area_y;
    int area_w, area_h;

    // Timing
    uint64_t last_update;
    uint64_t last_spawn;
    int spawn_counter;

    // Current level
    int current_level;

    // Random seed
    uint32_t rand_seed;

    // Selected lemming highlight
    int highlight_lemming;      // Index of lemming under cursor, or -1

    // Mouse position (for selection)
    int mouse_x, mouse_y;

} lemmings_t;

// Create/destroy game
lemmings_t *lemmings_create(void);
void lemmings_destroy(lemmings_t *game);

// Game control
void lemmings_start_level(lemmings_t *game, int level);
void lemmings_pause(lemmings_t *game);
void lemmings_resume(lemmings_t *game);

// Update and draw
void lemmings_update(lemmings_t *game);
void lemmings_draw(lemmings_t *game);

// Event handling
void lemmings_handle_event(lemmings_t *game, gui_event_t *event);

// Launch the game
void lemmings_launch(void);

#endif // LEMMINGS_H
