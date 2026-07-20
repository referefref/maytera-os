// main.c - User-mode Pong game for MayteraOS
// Ported from the kernel-side pong implementation to use window syscalls.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"

// Window and game area dimensions
#define WIN_W       640
#define WIN_H       400

// Game constants
#define PADDLE_W    10
#define PADDLE_H    60
#define BALL_SIZE   10
#define BALL_SPEED  4
#define PADDLE_SPEED 8
#define WIN_SCORE   11
#define PADDLE_MARGIN 10

// Colors
#define BG_COLOR    0x00000000
#define FG_COLOR    0x00FFFFFF
#define NET_COLOR   0x00808080

// Game states
enum { STATE_MENU, STATE_PLAYING, STATE_PAUSED, STATE_GAME_OVER };

// Game modes
enum { MODE_1P, MODE_2P };

// Game state variables
static int win = -1;
static int state = STATE_MENU;
static int mode = MODE_1P;
static int score_left = 0, score_right = 0;
static int paddle_left_y, paddle_right_y;
static int ball_x, ball_y, ball_dx, ball_dy;
static int ai_difficulty = 5;
static uint32_t rand_seed = 12345;

// Key tracking
static int key_w = 0, key_s = 0, key_up = 0, key_down = 0;

// Timing
static uint64_t last_update = 0;

// ============================================================================
// Simple PRNG
// ============================================================================

static int pong_rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

// ============================================================================
// Integer to string helper
// ============================================================================

static void int_to_str(int num, char *buf, int max_len) {
    int negative = 0;
    char tmp[24];
    int i = 0;

    if (num < 0) {
        negative = 1;
        num = -num;
    }
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (num > 0 && i < 20) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    int j = 0;
    if (negative && j < max_len - 1) {
        buf[j++] = '-';
    }
    while (i > 0 && j < max_len - 1) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// ============================================================================
// Ball reset
// ============================================================================

static void reset_ball(void) {
    ball_x = WIN_W / 2 - BALL_SIZE / 2;
    ball_y = WIN_H / 2 - BALL_SIZE / 2;

    // Random direction
    ball_dx = (pong_rand() % 2) ? BALL_SPEED : -BALL_SPEED;
    ball_dy = (pong_rand() % 3) - 1;  // -1, 0, or 1
    if (ball_dy == 0) {
        ball_dy = (pong_rand() % 2) ? 2 : -2;
    }
}

// ============================================================================
// Start a new game
// ============================================================================

static void start_game(int game_mode) {
    mode = game_mode;
    state = STATE_PLAYING;
    score_left = 0;
    score_right = 0;

    // Center paddles
    paddle_left_y = WIN_H / 2 - PADDLE_H / 2;
    paddle_right_y = WIN_H / 2 - PADDLE_H / 2;

    reset_ball();
    last_update = sys_clock();
}

// ============================================================================
// AI paddle movement
// ============================================================================

static void update_ai(void) {
    int target_y = ball_y + BALL_SIZE / 2 - PADDLE_H / 2;

    // Add tracking error based on difficulty (lower = more error)
    int error = (10 - ai_difficulty) * 5;
    target_y += (pong_rand() % (error * 2 + 1)) - error;

    int paddle_center = paddle_right_y + PADDLE_H / 2;
    int speed = PADDLE_SPEED * ai_difficulty / 10;
    if (speed < 2) speed = 2;

    // Only track when ball is heading toward the AI paddle
    if (ball_dx > 0) {
        if (paddle_center < target_y - 5) {
            paddle_right_y += speed;
        } else if (paddle_center > target_y + 5) {
            paddle_right_y -= speed;
        }
    }

    // Clamp
    if (paddle_right_y < 0) paddle_right_y = 0;
    if (paddle_right_y > WIN_H - PADDLE_H) paddle_right_y = WIN_H - PADDLE_H;
}

// ============================================================================
// Update game logic
// ============================================================================

static void update_game(void) {
    if (state != STATE_PLAYING) return;

    // Rate-limit to roughly 60 FPS using sys_clock() (milliseconds)
    uint64_t now = sys_clock();
    if (now - last_update < 16) return;
    last_update = now;

    // Move left paddle (Player 1: W/S)
    if (key_w) {
        paddle_left_y -= PADDLE_SPEED;
        if (paddle_left_y < 0) paddle_left_y = 0;
    }
    if (key_s) {
        paddle_left_y += PADDLE_SPEED;
        if (paddle_left_y > WIN_H - PADDLE_H)
            paddle_left_y = WIN_H - PADDLE_H;
    }

    // Move right paddle (Player 2: Up/Down, or AI)
    if (mode == MODE_2P) {
        if (key_up) {
            paddle_right_y -= PADDLE_SPEED;
            if (paddle_right_y < 0) paddle_right_y = 0;
        }
        if (key_down) {
            paddle_right_y += PADDLE_SPEED;
            if (paddle_right_y > WIN_H - PADDLE_H)
                paddle_right_y = WIN_H - PADDLE_H;
        }
    } else {
        update_ai();
    }

    // Move ball
    ball_x += ball_dx;
    ball_y += ball_dy;

    // Top/bottom wall collision
    if (ball_y <= 0) {
        ball_y = 0;
        ball_dy = -ball_dy;
    }
    if (ball_y >= WIN_H - BALL_SIZE) {
        ball_y = WIN_H - BALL_SIZE;
        ball_dy = -ball_dy;
    }

    // Left paddle collision
    if (ball_x <= PADDLE_W + PADDLE_MARGIN) {
        if (ball_y + BALL_SIZE >= paddle_left_y &&
            ball_y <= paddle_left_y + PADDLE_H) {
            ball_x = PADDLE_W + PADDLE_MARGIN;
            ball_dx = -ball_dx;
            // Add spin based on where ball hit the paddle
            int hit_pos = (ball_y + BALL_SIZE / 2) -
                          (paddle_left_y + PADDLE_H / 2);
            ball_dy = hit_pos / 8;
            // Speed up slightly, capped at 8
            if (ball_dx > 0 && ball_dx < 8) ball_dx++;
            else if (ball_dx < 0 && ball_dx > -8) ball_dx--;
        }
    }

    // Right paddle collision
    if (ball_x >= WIN_W - PADDLE_W - PADDLE_MARGIN - BALL_SIZE) {
        if (ball_y + BALL_SIZE >= paddle_right_y &&
            ball_y <= paddle_right_y + PADDLE_H) {
            ball_x = WIN_W - PADDLE_W - PADDLE_MARGIN - BALL_SIZE;
            ball_dx = -ball_dx;
            int hit_pos = (ball_y + BALL_SIZE / 2) -
                          (paddle_right_y + PADDLE_H / 2);
            ball_dy = hit_pos / 8;
            if (ball_dx > 0 && ball_dx < 8) ball_dx++;
            else if (ball_dx < 0 && ball_dx > -8) ball_dx--;
        }
    }

    // Scoring: ball exits left side
    if (ball_x < 0) {
        score_right++;
        if (score_right >= WIN_SCORE) {
            state = STATE_GAME_OVER;
        } else {
            reset_ball();
        }
    }

    // Scoring: ball exits right side
    if (ball_x > WIN_W) {
        score_left++;
        if (score_left >= WIN_SCORE) {
            state = STATE_GAME_OVER;
        } else {
            reset_ball();
        }
    }
}

// ============================================================================
// Drawing helpers
// ============================================================================

// Draw centered text at a given Y position within the window
static void draw_centered_text(const char *text, int y, uint32_t color) {
    int len = (int)strlen(text);
    int x = (WIN_W - len * FONT_WIDTH) / 2;
    win_draw_text(win, x, y, text, color);
}

// Draw the score display near the top center
static void draw_score(void) {
    char buf[8];

    // Left score (left of center)
    int_to_str(score_left, buf, sizeof(buf));
    int lx = WIN_W / 2 - 40;
    win_draw_text(win, lx, 20, buf, FG_COLOR);

    // Right score (right of center)
    int_to_str(score_right, buf, sizeof(buf));
    int rx = WIN_W / 2 + 30;
    win_draw_text(win, rx, 20, buf, FG_COLOR);
}

// Draw the dashed center net
static void draw_net(void) {
    int x = WIN_W / 2 - 2;
    for (int y = 0; y < WIN_H; y += 20) {
        win_draw_rect(win, x, y, 4, 10, NET_COLOR);
    }
}

// ============================================================================
// Draw routines for each game state
// ============================================================================

static void draw_menu(void) {
    // Clear background
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, BG_COLOR);

    // Title
    draw_centered_text("P O N G", 40, FG_COLOR);

    // Menu options
    draw_centered_text("Press 1 for Single Player", WIN_H / 2 - 20, FG_COLOR);
    draw_centered_text("Press 2 for Two Players", WIN_H / 2 + 10, FG_COLOR);

    // Controls info
    draw_centered_text("Controls:", WIN_H - 100, NET_COLOR);
    draw_centered_text("P1: W/S keys    P2: Up/Down arrows", WIN_H - 80, NET_COLOR);
    draw_centered_text("P to pause, ESC for menu", WIN_H - 60, NET_COLOR);
}

static void draw_playing(void) {
    // Clear background
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, BG_COLOR);

    // Draw net
    draw_net();

    // Draw score
    draw_score();

    // Draw left paddle
    win_draw_rect(win, PADDLE_MARGIN, paddle_left_y,
                  PADDLE_W, PADDLE_H, FG_COLOR);

    // Draw right paddle
    win_draw_rect(win, WIN_W - PADDLE_MARGIN - PADDLE_W, paddle_right_y,
                  PADDLE_W, PADDLE_H, FG_COLOR);

    // Draw ball
    win_draw_rect(win, ball_x, ball_y, BALL_SIZE, BALL_SIZE, FG_COLOR);

    // Pause overlay
    if (state == STATE_PAUSED) {
        draw_centered_text("PAUSED", WIN_H / 2 - 8, FG_COLOR);
        draw_centered_text("Press P to resume", WIN_H / 2 + 20, NET_COLOR);
    }
}

static void draw_game_over(void) {
    // Clear background
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, BG_COLOR);

    // Draw final score
    draw_score();

    // Winner text
    const char *winner;
    if (score_left > score_right) {
        winner = "Player 1 Wins!";
    } else if (mode == MODE_1P) {
        winner = "Computer Wins!";
    } else {
        winner = "Player 2 Wins!";
    }
    draw_centered_text(winner, WIN_H / 2 - 20, FG_COLOR);

    draw_centered_text("Press SPACE to play again", WIN_H / 2 + 20, NET_COLOR);
    draw_centered_text("Press ESC for menu", WIN_H / 2 + 40, NET_COLOR);
}

static void draw_game(void) {
    switch (state) {
    case STATE_MENU:
        draw_menu();
        break;
    case STATE_PLAYING:
    case STATE_PAUSED:
        draw_playing();
        break;
    case STATE_GAME_OVER:
        draw_game_over();
        break;
    }
}

// ============================================================================
// Event handling
// ============================================================================

static int handle_event(gui_event_t *ev) {
    if (ev->type == EVENT_WINDOW_CLOSE) {
        return 0;  // Signal exit
    }

    if (ev->type == EVENT_KEY_DOWN) {
        switch (ev->keycode) {
        case 'w': case 'W':
            key_w = 1;
            break;
        case 's': case 'S':
            key_s = 1;
            break;
        case 0x80:  // Up arrow
            key_up = 1;
            break;
        case 0x81:  // Down arrow
            key_down = 1;
            break;
        case '1':
            if (state == STATE_MENU) start_game(MODE_1P);
            break;
        case '2':
            if (state == STATE_MENU) start_game(MODE_2P);
            break;
        case 'p':  // Pause toggle (lowercase only; 0x50 is the down arrow)
            if (state == STATE_PLAYING) {
                state = STATE_PAUSED;
            } else if (state == STATE_PAUSED) {
                state = STATE_PLAYING;
                last_update = sys_clock();
            }
            break;
        case ' ':
            if (state == STATE_GAME_OVER) {
                start_game(mode);
            }
            break;
        case 0x1B:  // Escape
            if (state == STATE_PAUSED || state == STATE_GAME_OVER) {
                state = STATE_MENU;
            }
            break;
        }
    } else if (ev->type == EVENT_KEY_UP) {
        switch (ev->keycode) {
        case 'w': case 'W':
            key_w = 0;
            break;
        case 's': case 'S':
            key_s = 0;
            break;
        case 0x80:
            key_up = 0;
            break;
        case 0x81:
            key_down = 0;
            break;
        }
    }

    return 1;  // Continue running
}

// ============================================================================
// Entry point
// ============================================================================

int main(void) {
    // Seed PRNG from system clock
    rand_seed = (uint32_t)sys_clock();

    // Create window
    win = win_create("Pong", 100, 50, WIN_W, WIN_H);
    if (win < 0) {
        sys_exit(1);
    }

    // Initialize paddle positions for menu screen
    paddle_left_y = WIN_H / 2 - PADDLE_H / 2;
    paddle_right_y = WIN_H / 2 - PADDLE_H / 2;

    // Main loop
    int running = 1;
    while (running) {
        // Poll events with a 16 ms timeout (roughly 60 FPS)
        gui_event_t ev;
        int ret = win_get_event(win, &ev, 16);
        if (ret > 0) {
            running = handle_event(&ev);
        }

        // Update and draw
        update_game();
        draw_game();
        win_invalidate(win);
    }

    win_destroy(win);
    sys_exit(0);
    return 0;
}
