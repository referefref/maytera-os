// screensaver - Fullscreen animated screensaver for MayteraOS
// Supports three animation modes: starfield, bouncing lines, and bubbles.
// Press any key or click to exit; spacebar cycles through modes.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"

#define WIN_W       800
#define WIN_H       600
#define MAX_STARS   200
#define MAX_LINES   8
#define MAX_BUBBLES 15

// Screensaver animation types
enum { SS_STARFIELD, SS_LINES, SS_BUBBLES, SS_COUNT };

// Star structure for the 3D starfield effect
typedef struct {
    int x, y, z;            // 3D coordinates (z is depth, range 1..1000)
    int prev_sx, prev_sy;   // Previous projected screen position
} star_t;

// Bouncing line segment
typedef struct {
    int x1, y1, x2, y2;
    int dx1, dy1, dx2, dy2;
    uint32_t color;
} line_t;

// Floating bubble
typedef struct {
    int x, y, radius;
    int dx, dy;
    uint32_t color;
} bubble_t;

static int win = -1;
static int ss_type = SS_STARFIELD;
static star_t stars[MAX_STARS];
static line_t lines[MAX_LINES];
static bubble_t bubbles[MAX_BUBBLES];
static uint32_t frame_count = 0;
static uint32_t rand_seed = 54321;

// ----------------------------------------------------------------------------
// Simple PRNG (xorshift32)
// ----------------------------------------------------------------------------

static uint32_t prng(void) {
    rand_seed ^= rand_seed << 13;
    rand_seed ^= rand_seed >> 17;
    rand_seed ^= rand_seed << 5;
    return rand_seed;
}

static int rand_range(int lo, int hi) {
    if (lo >= hi) return lo;
    return lo + (int)(prng() % (uint32_t)(hi - lo));
}

// ----------------------------------------------------------------------------
// Absolute value helper
// ----------------------------------------------------------------------------

static int iabs(int v) {
    return v < 0 ? -v : v;
}

// ----------------------------------------------------------------------------
// Starfield helpers
// ----------------------------------------------------------------------------

static void star_init(star_t *s, int randomize_z) {
    s->x = rand_range(-WIN_W, WIN_W);
    s->y = rand_range(-WIN_H, WIN_H);
    s->z = randomize_z ? rand_range(1, 1000) : 1000;
    s->prev_sx = -1;
    s->prev_sy = -1;
}

static void starfield_init(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        star_init(&stars[i], 1);
    }
}

static void starfield_draw(void) {
    // Clear the screen to black every frame for a clean starfield
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, COLOR_BLACK);

    int cx = WIN_W / 2;
    int cy = WIN_H / 2;

    for (int i = 0; i < MAX_STARS; i++) {
        star_t *s = &stars[i];

        // Move star closer to the viewer
        s->z -= 8;
        if (s->z <= 0) {
            star_init(s, 0);
            continue;
        }

        // Project 3D to 2D
        int sx = cx + (s->x * 256) / s->z;
        int sy = cy + (s->y * 256) / s->z;

        // Skip stars outside window bounds
        if (sx < 0 || sx >= WIN_W - 1 || sy < 0 || sy >= WIN_H - 1) {
            star_init(s, 0);
            continue;
        }

        // Brightness scales with closeness (lower z = brighter)
        int brightness = 255 - (s->z * 255 / 1000);
        if (brightness < 40) brightness = 40;
        if (brightness > 255) brightness = 255;
        uint32_t gray = (uint32_t)brightness;
        uint32_t color = (gray << 16) | (gray << 8) | gray;

        // Draw star as a small rectangle; closer stars are larger
        int size = 1;
        if (s->z < 200) size = 3;
        else if (s->z < 500) size = 2;

        win_draw_rect(win, sx, sy, size, size, color);

        s->prev_sx = sx;
        s->prev_sy = sy;
    }

    // Title overlay
    win_draw_text(win, 10, WIN_H - 20, "STARFIELD  [SPACE] cycle  [ESC/click] exit", 0x00888888);
}

// ----------------------------------------------------------------------------
// Bouncing lines helpers
// ----------------------------------------------------------------------------

static uint32_t random_bright_color(void) {
    // Generate a vibrant color by ensuring at least one channel is high
    uint32_t r = rand_range(80, 256);
    uint32_t g = rand_range(80, 256);
    uint32_t b = rand_range(80, 256);
    // Boost a random channel to full brightness
    int ch = rand_range(0, 3);
    if (ch == 0) r = 255;
    else if (ch == 1) g = 255;
    else b = 255;
    return (r << 16) | (g << 8) | b;
}

static void lines_init(void) {
    for (int i = 0; i < MAX_LINES; i++) {
        line_t *l = &lines[i];
        l->x1 = rand_range(0, WIN_W);
        l->y1 = rand_range(0, WIN_H);
        l->x2 = rand_range(0, WIN_W);
        l->y2 = rand_range(0, WIN_H);
        l->dx1 = rand_range(1, 5) * (prng() & 1 ? 1 : -1);
        l->dy1 = rand_range(1, 5) * (prng() & 1 ? 1 : -1);
        l->dx2 = rand_range(1, 5) * (prng() & 1 ? 1 : -1);
        l->dy2 = rand_range(1, 5) * (prng() & 1 ? 1 : -1);
        l->color = random_bright_color();
    }
}

static void bounce_val(int *pos, int *vel, int lo, int hi) {
    *pos += *vel;
    if (*pos < lo) { *pos = lo; *vel = -(*vel); }
    if (*pos >= hi) { *pos = hi - 1; *vel = -(*vel); }
}

// Draw a line using Bresenham, rendered with small rectangles
static void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = iabs(x1 - x0);
    int dy = -iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (int steps = 0; steps < 2000; steps++) {
        // Draw a 2x2 pixel for visibility
        if (x0 >= 0 && x0 < WIN_W - 1 && y0 >= 0 && y0 < WIN_H - 1) {
            win_draw_rect(win, x0, y0, 2, 2, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void lines_draw(void) {
    // Periodically clear the screen to prevent total fill
    if (frame_count % 90 == 0) {
        win_draw_rect(win, 0, 0, WIN_W, WIN_H, COLOR_BLACK);
    }

    for (int i = 0; i < MAX_LINES; i++) {
        line_t *l = &lines[i];

        // Move endpoints
        bounce_val(&l->x1, &l->dx1, 0, WIN_W);
        bounce_val(&l->y1, &l->dy1, 0, WIN_H);
        bounce_val(&l->x2, &l->dx2, 0, WIN_W);
        bounce_val(&l->y2, &l->dy2, 0, WIN_H);

        // Occasionally change color
        if (prng() % 200 == 0) {
            l->color = random_bright_color();
        }

        draw_line(l->x1, l->y1, l->x2, l->y2, l->color);
    }

    win_draw_text(win, 10, WIN_H - 20, "LINES  [SPACE] cycle  [ESC/click] exit", 0x00888888);
}

// ----------------------------------------------------------------------------
// Bubbles helpers
// ----------------------------------------------------------------------------

static void bubbles_init(void) {
    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubble_t *b = &bubbles[i];
        b->radius = rand_range(15, 60);
        b->x = rand_range(b->radius, WIN_W - b->radius);
        b->y = rand_range(b->radius, WIN_H - b->radius);
        b->dx = rand_range(1, 4) * (prng() & 1 ? 1 : -1);
        b->dy = rand_range(1, 4) * (prng() & 1 ? 1 : -1);
        b->color = random_bright_color();
    }
}

// Draw a circle outline approximated by rectangles placed along the perimeter.
// Uses the midpoint circle algorithm.
static void draw_circle_outline(int cx, int cy, int r, uint32_t color) {
    int x = r;
    int y = 0;
    int err = 1 - r;

    while (x >= y) {
        // Draw small 2x2 blocks at each of the 8 octant points
        win_draw_rect(win, cx + x, cy + y, 2, 2, color);
        win_draw_rect(win, cx - x, cy + y, 2, 2, color);
        win_draw_rect(win, cx + x, cy - y, 2, 2, color);
        win_draw_rect(win, cx - x, cy - y, 2, 2, color);
        win_draw_rect(win, cx + y, cy + x, 2, 2, color);
        win_draw_rect(win, cx - y, cy + x, 2, 2, color);
        win_draw_rect(win, cx + y, cy - x, 2, 2, color);
        win_draw_rect(win, cx - y, cy - x, 2, 2, color);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

// Draw a filled circle by stacking horizontal rectangles (scanline fill).
static void draw_circle_filled(int cx, int cy, int r, uint32_t color) {
    for (int row = -r; row <= r; row++) {
        // Compute half-width at this row using integer square root approximation
        int rr = r * r - row * row;
        if (rr < 0) continue;

        // Integer square root via Newton's method (a few iterations suffice)
        int hw = r;
        if (hw <= 0) hw = 1;
        for (int iter = 0; iter < 8; iter++) {
            hw = (hw + rr / hw) / 2;
            if (hw <= 0) { hw = 0; break; }
        }

        int x0 = cx - hw;
        int y0 = cy + row;
        int w = hw * 2;
        if (w <= 0) continue;

        // Clamp to window bounds
        if (y0 < 0 || y0 >= WIN_H) continue;
        if (x0 < 0) { w += x0; x0 = 0; }
        if (x0 + w > WIN_W) w = WIN_W - x0;
        if (w <= 0) continue;

        win_draw_rect(win, x0, y0, w, 1, color);
    }
}

static void bubbles_draw(void) {
    // Clear screen each frame for clean animation
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, 0x00101020);

    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubble_t *b = &bubbles[i];

        // Move and bounce
        b->x += b->dx;
        b->y += b->dy;

        if (b->x - b->radius < 0) { b->x = b->radius; b->dx = -b->dx; }
        if (b->x + b->radius >= WIN_W) { b->x = WIN_W - b->radius - 1; b->dx = -b->dx; }
        if (b->y - b->radius < 0) { b->y = b->radius; b->dy = -b->dy; }
        if (b->y + b->radius >= WIN_H) { b->y = WIN_H - b->radius - 1; b->dy = -b->dy; }

        // Draw a semi-transparent look by using a dimmer fill and bright outline
        // Dim the fill color: shift each channel right by 2
        uint32_t r = (b->color >> 16) & 0xFF;
        uint32_t g = (b->color >> 8) & 0xFF;
        uint32_t bl = b->color & 0xFF;
        uint32_t fill = ((r / 4) << 16) | ((g / 4) << 8) | (bl / 4);

        draw_circle_filled(b->x, b->y, b->radius, fill);
        draw_circle_outline(b->x, b->y, b->radius, b->color);

        // Draw a small highlight near the top-left to simulate a specular reflection
        if (b->radius > 20) {
            int hx = b->x - b->radius / 3;
            int hy = b->y - b->radius / 3;
            if (hx >= 0 && hy >= 0 && hx < WIN_W - 4 && hy < WIN_H - 4) {
                win_draw_rect(win, hx, hy, 4, 4, 0x00FFFFFF);
            }
        }
    }

    win_draw_text(win, 10, WIN_H - 20, "BUBBLES  [SPACE] cycle  [ESC/click] exit", 0x00888888);
}

// ----------------------------------------------------------------------------
// Mode switching
// ----------------------------------------------------------------------------

static void init_mode(int mode) {
    ss_type = mode;
    frame_count = 0;
    // Clear window when switching modes
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, COLOR_BLACK);

    switch (mode) {
        case SS_STARFIELD: starfield_init(); break;
        case SS_LINES:     lines_init();     break;
        case SS_BUBBLES:   bubbles_init();   break;
    }
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(void) {
    // Seed the PRNG with the system clock for variety
    rand_seed = (uint32_t)sys_clock();
    if (rand_seed == 0) rand_seed = 54321;

    // Create a window (the window manager provides the title bar)
    win = win_create("Screensaver", 50, 30, WIN_W, WIN_H);
    if (win < 0) {
        sys_exit(1);
    }

    // Initialize the first screensaver mode
    init_mode(SS_STARFIELD);

    // Main loop
    int running = 1;
    gui_event_t evt;

    while (running) {
        // Draw the current animation frame
        switch (ss_type) {
            case SS_STARFIELD: starfield_draw(); break;
            case SS_LINES:     lines_draw();     break;
            case SS_BUBBLES:   bubbles_draw();   break;
        }

        frame_count++;

        // Poll for events with a ~33ms timeout (roughly 30 fps)
        int got = win_get_event(win, &evt, 33);
        if (got > 0) {
            switch (evt.type) {
                case EVENT_KEY_DOWN:
                    if (evt.key_char == ' ') {
                        // Cycle to next screensaver mode
                        init_mode((ss_type + 1) % SS_COUNT);
                    } else {
                        // Any other key exits
                        running = 0;
                    }
                    break;

                case EVENT_MOUSE_DOWN:
                    running = 0;
                    break;

                case EVENT_WINDOW_CLOSE:
                    running = 0;
                    break;

                case EVENT_REDRAW:
                    // Redraw requested by the window manager
                    break;

                default:
                    break;
            }
        }
    }

    win_destroy(win);
    sys_exit(0);
    return 0;
}
