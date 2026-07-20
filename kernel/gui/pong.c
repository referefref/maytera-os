// pong.c - Pong Game for MayteraOS
#include "pong.h"
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
#define PONG_BG_COLOR       0x000000
#define PONG_FG_COLOR       0xFFFFFF
#define PONG_PADDLE_COLOR   0xFFFFFF
#define PONG_BALL_COLOR     0xFFFFFF
#define PONG_NET_COLOR      0x808080

// Key states (tracked internally)
static bool key_w_down = false;
static bool key_s_down = false;
static bool key_up_down = false;
static bool key_down_down = false;

// Simple random number generator
static uint32_t pong_rand_seed = 12345;
static int pong_rand(void) {
    pong_rand_seed = pong_rand_seed * 1103515245 + 12345;
    return (pong_rand_seed >> 16) & 0x7FFF;
}

// Create pong game
pong_t *pong_create(void) {
    pong_t *g = (pong_t *)kmalloc(sizeof(pong_t));
    if (!g) return NULL;

    memset(g, 0, sizeof(pong_t));

    // Create window
    g->window = window_create("Pong", 100, 50, 640, 400);
    if (!g->window) {
        kfree(g);
        return NULL;
    }

    // Initialize state
    g->state = PONG_STATE_MENU;
    g->mode = PONG_MODE_1P;
    g->ai_difficulty = 5;

    // Initialize random seed from timer
    pong_rand_seed = timer_ticks;

    return g;
}

// Destroy pong game
void pong_destroy(pong_t *g) {
    if (!g) return;
    if (g->window) {
        window_destroy(g->window);
    }
    kfree(g);
}

// Reset ball to center with random direction
static void pong_reset_ball(pong_t *g) {
    g->ball_x = g->area_w / 2 - PONG_BALL_SIZE / 2;
    g->ball_y = g->area_h / 2 - PONG_BALL_SIZE / 2;

    // Random direction
    g->ball_dx = (pong_rand() % 2) ? PONG_BALL_SPEED : -PONG_BALL_SPEED;
    g->ball_dy = (pong_rand() % 3) - 1;  // -1, 0, or 1
    if (g->ball_dy == 0) g->ball_dy = (pong_rand() % 2) ? 2 : -2;
}

// Start game
void pong_start(pong_t *g, pong_mode_t mode) {
    if (!g) return;

    g->mode = mode;
    g->state = PONG_STATE_PLAYING;
    g->score_left = 0;
    g->score_right = 0;

    // Center paddles
    g->paddle_left_y = g->area_h / 2 - PONG_PADDLE_HEIGHT / 2;
    g->paddle_right_y = g->area_h / 2 - PONG_PADDLE_HEIGHT / 2;

    pong_reset_ball(g);
    g->last_update = timer_ticks;
}

// Pause/resume
void pong_pause(pong_t *g) {
    if (g && g->state == PONG_STATE_PLAYING) {
        g->state = PONG_STATE_PAUSED;
    }
}

void pong_resume(pong_t *g) {
    if (g && g->state == PONG_STATE_PAUSED) {
        g->state = PONG_STATE_PLAYING;
        g->last_update = timer_ticks;
    }
}

// Reset to menu
void pong_reset(pong_t *g) {
    if (g) {
        g->state = PONG_STATE_MENU;
    }
}

// AI paddle movement
static void pong_update_ai(pong_t *g) {
    // Simple AI: track ball with some reaction delay based on difficulty
    int target_y = g->ball_y + PONG_BALL_SIZE / 2 - PONG_PADDLE_HEIGHT / 2;

    // Add some error based on difficulty (lower difficulty = more error)
    int error = (10 - g->ai_difficulty) * 5;
    target_y += (pong_rand() % (error * 2 + 1)) - error;

    // Move towards target
    int paddle_center = g->paddle_right_y + PONG_PADDLE_HEIGHT / 2;
    int speed = PONG_PADDLE_SPEED * g->ai_difficulty / 10;
    if (speed < 2) speed = 2;

    if (g->ball_dx > 0) {  // Ball coming towards AI
        if (paddle_center < target_y - 5) {
            g->paddle_right_y += speed;
        } else if (paddle_center > target_y + 5) {
            g->paddle_right_y -= speed;
        }
    }

    // Clamp paddle position
    if (g->paddle_right_y < 0) g->paddle_right_y = 0;
    if (g->paddle_right_y > g->area_h - PONG_PADDLE_HEIGHT)
        g->paddle_right_y = g->area_h - PONG_PADDLE_HEIGHT;
}

// Update game state
void pong_update(pong_t *g) {
    if (!g || g->state != PONG_STATE_PLAYING) return;

    // Rate limit updates (~60 FPS)
    if (timer_ticks - g->last_update < 2) return;
    g->last_update = timer_ticks;

    // Update paddle positions from key states
    if (key_w_down) {
        g->paddle_left_y -= PONG_PADDLE_SPEED;
        if (g->paddle_left_y < 0) g->paddle_left_y = 0;
    }
    if (key_s_down) {
        g->paddle_left_y += PONG_PADDLE_SPEED;
        if (g->paddle_left_y > g->area_h - PONG_PADDLE_HEIGHT)
            g->paddle_left_y = g->area_h - PONG_PADDLE_HEIGHT;
    }

    // Player 2 or AI
    if (g->mode == PONG_MODE_2P) {
        if (key_up_down) {
            g->paddle_right_y -= PONG_PADDLE_SPEED;
            if (g->paddle_right_y < 0) g->paddle_right_y = 0;
        }
        if (key_down_down) {
            g->paddle_right_y += PONG_PADDLE_SPEED;
            if (g->paddle_right_y > g->area_h - PONG_PADDLE_HEIGHT)
                g->paddle_right_y = g->area_h - PONG_PADDLE_HEIGHT;
        }
    } else {
        pong_update_ai(g);
    }

    // Update ball position
    g->ball_x += g->ball_dx;
    g->ball_y += g->ball_dy;

    // Ball collision with top/bottom
    if (g->ball_y <= 0) {
        g->ball_y = 0;
        g->ball_dy = -g->ball_dy;
    }
    if (g->ball_y >= g->area_h - PONG_BALL_SIZE) {
        g->ball_y = g->area_h - PONG_BALL_SIZE;
        g->ball_dy = -g->ball_dy;
    }

    // Ball collision with left paddle
    if (g->ball_x <= PONG_PADDLE_WIDTH + 10) {
        if (g->ball_y + PONG_BALL_SIZE >= g->paddle_left_y &&
            g->ball_y <= g->paddle_left_y + PONG_PADDLE_HEIGHT) {
            g->ball_x = PONG_PADDLE_WIDTH + 10;
            g->ball_dx = -g->ball_dx;
            // Add spin based on where ball hit paddle
            int hit_pos = (g->ball_y + PONG_BALL_SIZE / 2) - (g->paddle_left_y + PONG_PADDLE_HEIGHT / 2);
            g->ball_dy = hit_pos / 8;
            // Speed up slightly
            if (g->ball_dx > 0 && g->ball_dx < 8) g->ball_dx++;
            else if (g->ball_dx < 0 && g->ball_dx > -8) g->ball_dx--;
        }
    }

    // Ball collision with right paddle
    if (g->ball_x >= g->area_w - PONG_PADDLE_WIDTH - 10 - PONG_BALL_SIZE) {
        if (g->ball_y + PONG_BALL_SIZE >= g->paddle_right_y &&
            g->ball_y <= g->paddle_right_y + PONG_PADDLE_HEIGHT) {
            g->ball_x = g->area_w - PONG_PADDLE_WIDTH - 10 - PONG_BALL_SIZE;
            g->ball_dx = -g->ball_dx;
            int hit_pos = (g->ball_y + PONG_BALL_SIZE / 2) - (g->paddle_right_y + PONG_PADDLE_HEIGHT / 2);
            g->ball_dy = hit_pos / 8;
            if (g->ball_dx > 0 && g->ball_dx < 8) g->ball_dx++;
            else if (g->ball_dx < 0 && g->ball_dx > -8) g->ball_dx--;
        }
    }

    // Score when ball goes off screen
    if (g->ball_x < 0) {
        // Right player scores
        g->score_right++;
        if (g->score_right >= PONG_WIN_SCORE) {
            g->state = PONG_STATE_GAME_OVER;
        } else {
            pong_reset_ball(g);
        }
    }
    if (g->ball_x > g->area_w) {
        // Left player scores
        g->score_left++;
        if (g->score_left >= PONG_WIN_SCORE) {
            g->state = PONG_STATE_GAME_OVER;
        } else {
            pong_reset_ball(g);
        }
    }
}

// Draw text helper
static void pong_draw_text(const char *text, int x, int y, uint32_t color) {
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

// Draw large digit (2x scale)
static void pong_draw_digit(int digit, int x, int y, uint32_t color) {
    char c = '0' + digit;
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_fill_rect(x + col * 2, y + row * 2, 2, 2, color);
            }
        }
    }
}

// Draw score
static void pong_draw_score(pong_t *g, int x, int y) {
    // Left score
    if (g->score_left >= 10) {
        pong_draw_digit(g->score_left / 10, x - 40, y, PONG_FG_COLOR);
        pong_draw_digit(g->score_left % 10, x - 20, y, PONG_FG_COLOR);
    } else {
        pong_draw_digit(g->score_left, x - 20, y, PONG_FG_COLOR);
    }

    // Right score
    if (g->score_right >= 10) {
        pong_draw_digit(g->score_right / 10, x + 20, y, PONG_FG_COLOR);
        pong_draw_digit(g->score_right % 10, x + 40, y, PONG_FG_COLOR);
    } else {
        pong_draw_digit(g->score_right, x + 20, y, PONG_FG_COLOR);
    }
}

// Draw menu
static void pong_draw_menu(pong_t *g, int x, int y, int w, int h) {
    (void)g;  // Unused
    // Title
    pong_draw_text("P O N G", x + w / 2 - 28, y + 40, PONG_FG_COLOR);

    // Menu options
    pong_draw_text("Press 1 for Single Player", x + w / 2 - 100, y + h / 2 - 20, PONG_FG_COLOR);
    pong_draw_text("Press 2 for Two Players", x + w / 2 - 92, y + h / 2 + 10, PONG_FG_COLOR);

    // Controls
    pong_draw_text("Controls:", x + w / 2 - 36, y + h - 100, 0x808080);
    pong_draw_text("P1: W/S keys    P2: Up/Down arrows", x + w / 2 - 140, y + h - 80, 0x808080);
    pong_draw_text("P to pause, ESC for menu", x + w / 2 - 96, y + h - 60, 0x808080);
}

// Draw game over
static void pong_draw_game_over(pong_t *g, int x, int y, int w, int h) {
    const char *winner = (g->score_left > g->score_right) ? "Player 1 Wins!" :
                         (g->mode == PONG_MODE_1P) ? "Computer Wins!" : "Player 2 Wins!";
    int text_len = strlen(winner) * 8;
    pong_draw_text(winner, x + w / 2 - text_len / 2, y + h / 2 - 20, PONG_FG_COLOR);
    pong_draw_text("Press SPACE to play again", x + w / 2 - 100, y + h / 2 + 20, 0x808080);
    pong_draw_text("Press ESC for menu", x + w / 2 - 72, y + h / 2 + 40, 0x808080);
}

// Draw game
void pong_draw(pong_t *g) {
    if (!g || !g->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(g->window, &wx, &wy, &ww, &wh);

    // Store game area dimensions
    g->area_x = wx;
    g->area_y = wy;
    g->area_w = ww;
    g->area_h = wh;

    // Update game state
    pong_update(g);

    // Clear background
    fb_fill_rect(wx, wy, ww, wh, PONG_BG_COLOR);

    if (g->state == PONG_STATE_MENU) {
        pong_draw_menu(g, wx, wy, ww, wh);
        return;
    }

    if (g->state == PONG_STATE_GAME_OVER) {
        // Draw final score
        pong_draw_score(g, wx + ww / 2, wy + 20);
        pong_draw_game_over(g, wx, wy, ww, wh);
        return;
    }

    // Draw center net (dashed line)
    for (int y = 0; y < wh; y += 20) {
        fb_fill_rect(wx + ww / 2 - 2, wy + y, 4, 10, PONG_NET_COLOR);
    }

    // Draw score
    pong_draw_score(g, wx + ww / 2, wy + 20);

    // Draw paddles
    fb_fill_rect(wx + 10, wy + g->paddle_left_y, PONG_PADDLE_WIDTH, PONG_PADDLE_HEIGHT, PONG_PADDLE_COLOR);
    fb_fill_rect(wx + ww - 10 - PONG_PADDLE_WIDTH, wy + g->paddle_right_y, PONG_PADDLE_WIDTH, PONG_PADDLE_HEIGHT, PONG_PADDLE_COLOR);

    // Draw ball
    fb_fill_rect(wx + g->ball_x, wy + g->ball_y, PONG_BALL_SIZE, PONG_BALL_SIZE, PONG_BALL_COLOR);

    // Pause overlay
    if (g->state == PONG_STATE_PAUSED) {
        pong_draw_text("PAUSED", wx + ww / 2 - 24, wy + wh / 2 - 8, PONG_FG_COLOR);
        pong_draw_text("Press P to resume", wx + ww / 2 - 68, wy + wh / 2 + 20, 0x808080);
    }
}

// Event handling
void pong_handle_event(pong_t *g, gui_event_t *event) {
    if (!g || !event) return;

    if (event->type == EVENT_KEY_DOWN) {
        switch (event->keycode) {
            case 'w': case 'W': key_w_down = true; break;
            case 's': case 'S': key_s_down = true; break;
            case 0x48:  // Up arrow
                key_up_down = true; break;
            case 0x50:  // Down arrow
                key_down_down = true; break;

            case '1':
                if (g->state == PONG_STATE_MENU) {
                    pong_start(g, PONG_MODE_1P);
                }
                break;
            case '2':
                if (g->state == PONG_STATE_MENU) {
                    pong_start(g, PONG_MODE_2P);
                }
                break;
            case 'p':  // Pause (lowercase only to avoid conflict with 0x50 down arrow)
                if (g->state == PONG_STATE_PLAYING) pong_pause(g);
                else if (g->state == PONG_STATE_PAUSED) pong_resume(g);
                break;
            case ' ':
                if (g->state == PONG_STATE_GAME_OVER) {
                    pong_start(g, g->mode);
                }
                break;
            case 0x1B:  // Escape
                if (g->state == PONG_STATE_PAUSED || g->state == PONG_STATE_GAME_OVER) {
                    pong_reset(g);
                }
                break;
        }
    } else if (event->type == EVENT_KEY_UP) {
        switch (event->keycode) {
            case 'w': case 'W': key_w_down = false; break;
            case 's': case 'S': key_s_down = false; break;
            case 0x48: key_up_down = false; break;
            case 0x50: key_down_down = false; break;
        }
    }
}

// Launch pong
void pong_launch(void) {
    LOG_INFO("[Pong] Game launched");
    pong_t *g = pong_create();
    if (!g) {
        LOG_ERROR("[Pong] Failed to create game");
        kprintf("[Pong] Failed to create game\n");
        return;
    }

    // Register with window manager
    wm_register_app(g->window, g,
                    (app_event_handler_t)pong_handle_event,
                    (app_draw_handler_t)pong_draw,
                    (app_destroy_handler_t)pong_destroy);

    kprintf("[Pong] Game launched\n");
}
