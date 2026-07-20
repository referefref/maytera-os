// screensaver.h - Screensaver System for MayteraOS
#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include "../types.h"

// Screensaver types
typedef enum {
    SCREENSAVER_NONE = 0,
    SCREENSAVER_BLANK,
    SCREENSAVER_STARFIELD,
    SCREENSAVER_LINES,
    SCREENSAVER_BUBBLES,
    SCREENSAVER_MATRIX,
    SCREENSAVER_COUNT
} screensaver_type_t;

// Maximum objects for animations
#define SS_MAX_STARS    300
#define SS_MAX_LINES    20
#define SS_MAX_BUBBLES  30

// Star for starfield
typedef struct { // 3D starfield
    int x, y;           // 3D position (scaled, relative to center)
    int z;              // Depth (larger = farther)
    int prev_sx, prev_sy; // Previous screen position for trails
    bool active;
} ss_star_t;

// Line for line screensaver
typedef struct { // 3D starfield
    int x1, y1, x2, y2;     // Start and end points
    int dx1, dy1, dx2, dy2; // Velocities
    uint32_t color;
    bool active;
} ss_line_t;

// Bubble for bubble screensaver
typedef struct { // 3D starfield
    int x, y;           // Center position
    int radius;         // Current radius
    int dx, dy;         // Velocity
    int dr;             // Radius change rate
    uint32_t color;
    bool active;
} ss_bubble_t;

// Screensaver configuration
typedef struct { // 3D starfield
    bool enabled;               // Is screensaver enabled?
    screensaver_type_t type;    // Current screensaver type
    uint32_t timeout_seconds;   // Idle time before activation (seconds)
} screensaver_config_t;

// Screensaver state
typedef struct { // 3D starfield
    bool active;                // Is screensaver currently running?
    uint64_t last_input_time;   // Last input timestamp
    uint64_t start_time;        // When screensaver started

    // Animation state
    ss_star_t stars[SS_MAX_STARS];
    ss_line_t lines[SS_MAX_LINES];
    ss_bubble_t bubbles[SS_MAX_BUBBLES];

    int frame_count;            // Animation frame counter
} screensaver_state_t;

// Initialize screensaver system
void screensaver_init(void);

// Get/set configuration
screensaver_config_t *screensaver_get_config(void);
void screensaver_set_type(screensaver_type_t type);
void screensaver_set_timeout(uint32_t seconds);
void screensaver_set_enabled(bool enabled);

// Called every frame from desktop
void screensaver_update(void);

// Called on any input event
void screensaver_on_input(void);

// Check if screensaver is active
bool screensaver_is_active(void);

// Draw screensaver (if active)
void screensaver_draw(void);

// Get screensaver type name
const char *screensaver_get_type_name(screensaver_type_t type);

#endif // SCREENSAVER_H
