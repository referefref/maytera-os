// pong.h - Pong Game for MayteraOS
#ifndef PONG_H
#define PONG_H

#include "window.h"

// Game constants
#define PONG_PADDLE_WIDTH   10
#define PONG_PADDLE_HEIGHT  60
#define PONG_BALL_SIZE      10
#define PONG_BALL_SPEED     4
#define PONG_PADDLE_SPEED   8
#define PONG_WIN_SCORE      11

// Game states
typedef enum {
    PONG_STATE_MENU,
    PONG_STATE_PLAYING,
    PONG_STATE_PAUSED,
    PONG_STATE_GAME_OVER
} pong_state_t;

// Game mode
typedef enum {
    PONG_MODE_1P,       // Single player vs AI
    PONG_MODE_2P        // Two players
} pong_mode_t;

// Pong game state
typedef struct {
    window_t *window;

    // Game state
    pong_state_t state;
    pong_mode_t mode;

    // Scores
    int score_left;
    int score_right;

    // Left paddle (player 1)
    int paddle_left_y;

    // Right paddle (player 2 or AI)
    int paddle_right_y;

    // Ball position and velocity (fixed-point: actual = value * 1)
    int ball_x;
    int ball_y;
    int ball_dx;        // X velocity
    int ball_dy;        // Y velocity

    // Game area
    int area_x, area_y;
    int area_w, area_h;

    // AI difficulty (1-10, affects reaction time)
    int ai_difficulty;

    // Update counter (for timing)
    uint64_t last_update;

} pong_t;

// Create pong game
pong_t *pong_create(void);

// Destroy pong game
void pong_destroy(pong_t *g);

// Game control
void pong_start(pong_t *g, pong_mode_t mode);
void pong_pause(pong_t *g);
void pong_resume(pong_t *g);
void pong_reset(pong_t *g);

// Event handling
void pong_handle_event(pong_t *g, gui_event_t *event);

// Update game state (call from draw or timer)
void pong_update(pong_t *g);

// Drawing
void pong_draw(pong_t *g);

// Launch pong
void pong_launch(void);

#endif // PONG_H
