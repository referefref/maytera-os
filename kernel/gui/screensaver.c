// screensaver.c - Screensaver System for MayteraOS
#include "screensaver.h"
#include "../serial.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "syslog.h"

// External timer
extern volatile uint64_t timer_ticks;

// Screen dimensions (from framebuffer)
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);

// Window manager redraw
extern void wm_invalidate_all(void);

// Global state
static screensaver_config_t g_ss_config;
static screensaver_state_t g_ss_state;
static bool g_ss_initialized = false;

// Simple pseudo-random number generator
static uint32_t g_ss_rand_seed = 12345;

static uint32_t ss_rand(void) {
    g_ss_rand_seed = g_ss_rand_seed * 1103515245 + 12345;
    return (g_ss_rand_seed >> 16) & 0x7FFF;
}

static int ss_rand_range(int min, int max) {
    return min + (ss_rand() % (max - min + 1));
}

// Initialize screensaver
void screensaver_init(void) {
    if (g_ss_initialized) return;
    LOG_INFO("[Screensaver] Initializing");

    memset(&g_ss_config, 0, sizeof(g_ss_config));
    memset(&g_ss_state, 0, sizeof(g_ss_state));

    // Default config
    g_ss_config.enabled = true;
    g_ss_config.type = SCREENSAVER_STARFIELD;
    g_ss_config.timeout_seconds = 120;  // 2 minutes

    g_ss_state.active = false;
    g_ss_state.last_input_time = timer_ticks;

    g_ss_initialized = true;
    kprintf("[Screensaver] Initialized (timeout: %d sec)\n", g_ss_config.timeout_seconds);
}

// Get config
screensaver_config_t *screensaver_get_config(void) {
    return &g_ss_config;
}

// Set type
void screensaver_set_type(screensaver_type_t type) {
    if (type < SCREENSAVER_COUNT) {
        g_ss_config.type = type;
    }
}

// Set timeout
void screensaver_set_timeout(uint32_t seconds) {
    g_ss_config.timeout_seconds = seconds;
}

// Set enabled
void screensaver_set_enabled(bool enabled) {
    g_ss_config.enabled = enabled;
    if (!enabled && g_ss_state.active) {
        g_ss_state.active = false;
    }
}

// Initialize starfield
static void init_starfield(void) {
    for (int i = 0; i < SS_MAX_STARS; i++) {
        // Random 3D position - x,y in range [-1000, 1000], z in range [1, 1000]
        g_ss_state.stars[i].x = ss_rand_range(-1000, 1000);
        g_ss_state.stars[i].y = ss_rand_range(-1000, 1000);
        g_ss_state.stars[i].z = ss_rand_range(1, 1000);
        g_ss_state.stars[i].prev_sx = -1;
        g_ss_state.stars[i].prev_sy = -1;
        g_ss_state.stars[i].active = true;
    }
}

// Reset a single star to far away
static void reset_star(int i) {
    g_ss_state.stars[i].x = ss_rand_range(-1000, 1000);
    g_ss_state.stars[i].y = ss_rand_range(-1000, 1000);
    g_ss_state.stars[i].z = 1000;
    g_ss_state.stars[i].prev_sx = -1;
    g_ss_state.stars[i].prev_sy = -1;
}
// Initialize lines
static void init_lines(void) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    for (int i = 0; i < SS_MAX_LINES; i++) {
        g_ss_state.lines[i].x1 = ss_rand_range(0, sw);
        g_ss_state.lines[i].y1 = ss_rand_range(0, sh);
        g_ss_state.lines[i].x2 = ss_rand_range(0, sw);
        g_ss_state.lines[i].y2 = ss_rand_range(0, sh);
        g_ss_state.lines[i].dx1 = ss_rand_range(-5, 5);
        g_ss_state.lines[i].dy1 = ss_rand_range(-5, 5);
        g_ss_state.lines[i].dx2 = ss_rand_range(-5, 5);
        g_ss_state.lines[i].dy2 = ss_rand_range(-5, 5);
        // Random color
        g_ss_state.lines[i].color = (ss_rand() << 16) | (ss_rand() << 1) | ss_rand();
        g_ss_state.lines[i].active = true;
    }
}

// Initialize bubbles
static void init_bubbles(void) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    for (int i = 0; i < SS_MAX_BUBBLES; i++) {
        g_ss_state.bubbles[i].x = ss_rand_range(50, sw - 50);
        g_ss_state.bubbles[i].y = ss_rand_range(50, sh - 50);
        g_ss_state.bubbles[i].radius = ss_rand_range(10, 50);
        g_ss_state.bubbles[i].dx = ss_rand_range(-3, 3);
        g_ss_state.bubbles[i].dy = ss_rand_range(-3, 3);
        g_ss_state.bubbles[i].dr = ss_rand_range(-1, 1);
        // Pastel colors
        uint8_t r = 128 + ss_rand_range(0, 127);
        uint8_t g = 128 + ss_rand_range(0, 127);
        uint8_t b = 128 + ss_rand_range(0, 127);
        g_ss_state.bubbles[i].color = (r << 16) | (g << 8) | b;
        g_ss_state.bubbles[i].active = true;
    }
}

// Start screensaver
static void start_screensaver(void) {
    g_ss_state.active = true;
    g_ss_state.start_time = timer_ticks;
    g_ss_state.frame_count = 0;

    // Initialize animation based on type
    switch (g_ss_config.type) {
        case SCREENSAVER_STARFIELD:
            init_starfield();
            break;
        case SCREENSAVER_LINES:
            init_lines();
            break;
        case SCREENSAVER_BUBBLES:
            init_bubbles();
            break;
        default:
            break;
    }

    kprintf("[Screensaver] Started (%s)\n", screensaver_get_type_name(g_ss_config.type));
}

// Called on input
void screensaver_on_input(void) {
    g_ss_state.last_input_time = timer_ticks;

    if (g_ss_state.active) {
        g_ss_state.active = false;
        kprintf("[Screensaver] Deactivated\n");
        wm_invalidate_all();  // Force full redraw
    }
}

// Check if active
bool screensaver_is_active(void) {
    return g_ss_state.active;
}

// Update (called every frame)
void screensaver_update(void) {
    if (!g_ss_initialized) {
        screensaver_init();
    }

    if (!g_ss_config.enabled) return;

    // Check idle time
    uint64_t idle_time = timer_ticks - g_ss_state.last_input_time;
    uint64_t timeout_ticks = g_ss_config.timeout_seconds * 250;   // 250 Hz timer (pit_init(250))

    if (!g_ss_state.active && idle_time >= timeout_ticks) {
        start_screensaver();
    }
}

// Draw starfield
static void draw_starfield(void) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    int cx = sw / 2;
    int cy = sh / 2;
    int speed = 15;
    int perspective = 300;

    fb_clear(0x000000);

    for (int i = 0; i < SS_MAX_STARS; i++) {
        ss_star_t *star = &g_ss_state.stars[i];
        if (!star->active) continue;

        star->z -= speed;
        if (star->z <= 0) {
            reset_star(i);
            continue;
        }

        int sx = cx + (star->x * perspective) / star->z;
        int sy = cy + (star->y * perspective) / star->z;

        if (sx < 0 || sx >= (int)sw || sy < 0 || sy >= (int)sh) {
            reset_star(i);
            continue;
        }

        int brightness = 255 - (star->z * 205 / 1000);
        if (brightness < 50) brightness = 50;
        if (brightness > 255) brightness = 255;
        uint32_t color = (brightness << 16) | (brightness << 8) | brightness;

        if (star->prev_sx >= 0 && star->prev_sy >= 0) {
            int x0 = star->prev_sx;
            int y0 = star->prev_sy;
            int x1 = sx;
            int y1 = sy;
            int dx = x1 - x0;
            int dy = y1 - y0;
            int adx = dx > 0 ? dx : -dx;
            int ady = dy > 0 ? dy : -dy;
            int steps = adx > ady ? adx : ady;
            
            if (steps > 0) {
                int xi = (dx * 1000) / steps;
                int yi = (dy * 1000) / steps;
                int px = x0 * 1000;
                int py = y0 * 1000;
                
                for (int j = 0; j <= steps; j++) {
                    int ppx = px / 1000;
                    int ppy = py / 1000;
                    if (ppx >= 0 && ppx < (int)sw && ppy >= 0 && ppy < (int)sh) {
                        int trail_bright = brightness * (j + 1) / (steps + 1);
                        if (trail_bright < 30) trail_bright = 30;
                        uint32_t trail_color = (trail_bright << 16) | (trail_bright << 8) | trail_bright;
                        fb_put_pixel(ppx, ppy, trail_color);
                    }
                    px += xi;
                    py += yi;
                }
            }
        }

        int size = 1;
        if (star->z < 200) size = 3;
        else if (star->z < 400) size = 2;
        
        for (int py = -size/2; py <= size/2; py++) {
            for (int px = -size/2; px <= size/2; px++) {
                int drawx = sx + px;
                int drawy = sy + py;
                if (drawx >= 0 && drawx < (int)sw && drawy >= 0 && drawy < (int)sh) {
                    fb_put_pixel(drawx, drawy, color);
                }
            }
        }

        star->prev_sx = sx;
        star->prev_sy = sy;
    }
}

// Draw lines
static void draw_lines(void) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    // Fade effect - darken entire screen slightly
    // For simplicity, just clear with dark color every 10 frames
    if (g_ss_state.frame_count % 10 == 0) {
        fb_clear(0x050505);
    }

    for (int i = 0; i < SS_MAX_LINES; i++) {
        ss_line_t *line = &g_ss_state.lines[i];
        if (!line->active) continue;

        // Move endpoints
        line->x1 += line->dx1;
        line->y1 += line->dy1;
        line->x2 += line->dx2;
        line->y2 += line->dy2;

        // Bounce off edges
        if (line->x1 < 0 || line->x1 >= (int)sw) line->dx1 = -line->dx1;
        if (line->y1 < 0 || line->y1 >= (int)sh) line->dy1 = -line->dy1;
        if (line->x2 < 0 || line->x2 >= (int)sw) line->dx2 = -line->dx2;
        if (line->y2 < 0 || line->y2 >= (int)sh) line->dy2 = -line->dy2;

        // Clamp to screen
        if (line->x1 < 0) line->x1 = 0;
        if (line->x1 >= (int)sw) line->x1 = sw - 1;
        if (line->y1 < 0) line->y1 = 0;
        if (line->y1 >= (int)sh) line->y1 = sh - 1;
        if (line->x2 < 0) line->x2 = 0;
        if (line->x2 >= (int)sw) line->x2 = sw - 1;
        if (line->y2 < 0) line->y2 = 0;
        if (line->y2 >= (int)sh) line->y2 = sh - 1;

        // Draw line using Bresenham's algorithm
        int dx = line->x2 - line->x1;
        int dy = line->y2 - line->y1;
        int steps = (dx > 0 ? dx : -dx);
        int dys = (dy > 0 ? dy : -dy);
        if (dys > steps) steps = dys;
        if (steps == 0) steps = 1;

        int xi = (dx * 1000) / steps;
        int yi = (dy * 1000) / steps;
        int x = line->x1 * 1000;
        int y = line->y1 * 1000;

        for (int j = 0; j <= steps; j++) {
            int px = x / 1000;
            int py = y / 1000;
            if (px >= 0 && px < (int)sw && py >= 0 && py < (int)sh) {
                fb_put_pixel(px, py, line->color);
            }
            x += xi;
            y += yi;
        }
    }
}

// Draw circle outline
static void draw_circle(int cx, int cy, int r, uint32_t color) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    // Simple circle drawing using midpoint algorithm
    int x = 0;
    int y = r;
    int d = 1 - r;

    while (x <= y) {
        // Draw 8 octants
        int points[8][2] = {
            {cx + x, cy + y}, {cx - x, cy + y},
            {cx + x, cy - y}, {cx - x, cy - y},
            {cx + y, cy + x}, {cx - y, cy + x},
            {cx + y, cy - x}, {cx - y, cy - x}
        };

        for (int i = 0; i < 8; i++) {
            int px = points[i][0];
            int py = points[i][1];
            if (px >= 0 && px < (int)sw && py >= 0 && py < (int)sh) {
                fb_put_pixel(px, py, color);
            }
        }

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// Draw bubbles
static void draw_bubbles(void) {
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    // Clear to dark blue
    fb_clear(0x000020);

    for (int i = 0; i < SS_MAX_BUBBLES; i++) {
        ss_bubble_t *bubble = &g_ss_state.bubbles[i];
        if (!bubble->active) continue;

        // Move bubble
        bubble->x += bubble->dx;
        bubble->y += bubble->dy;
        bubble->radius += bubble->dr;

        // Bounce off edges
        if (bubble->x - bubble->radius < 0 || bubble->x + bubble->radius >= (int)sw) {
            bubble->dx = -bubble->dx;
        }
        if (bubble->y - bubble->radius < 0 || bubble->y + bubble->radius >= (int)sh) {
            bubble->dy = -bubble->dy;
        }

        // Radius limits
        if (bubble->radius < 10 || bubble->radius > 80) {
            bubble->dr = -bubble->dr;
        }
        if (bubble->radius < 5) bubble->radius = 5;
        if (bubble->radius > 100) bubble->radius = 100;

        // Draw bubble (multiple circles for thickness)
        draw_circle(bubble->x, bubble->y, bubble->radius, bubble->color);
        draw_circle(bubble->x, bubble->y, bubble->radius - 1, bubble->color);
        if (bubble->radius > 20) {
            // Highlight
            uint32_t highlight = 0xFFFFFF;
            draw_circle(bubble->x - bubble->radius / 3, bubble->y - bubble->radius / 3,
                       bubble->radius / 4, highlight);
        }
    }
}

// Draw blank screen
static void draw_blank(void) {
    fb_clear(0x000000);
}

// Main draw function
void screensaver_draw(void) {
    if (!g_ss_state.active) return;

    g_ss_state.frame_count++;

    switch (g_ss_config.type) {
        case SCREENSAVER_BLANK:
            draw_blank();
            break;
        case SCREENSAVER_STARFIELD:
            draw_starfield();
            break;
        case SCREENSAVER_LINES:
            draw_lines();
            break;
        case SCREENSAVER_BUBBLES:
            draw_bubbles();
            break;
        case SCREENSAVER_MATRIX:
            // TODO: Matrix rain effect
            draw_blank();
            break;
        default:
            draw_blank();
            break;
    }
}

// Get type name
const char *screensaver_get_type_name(screensaver_type_t type) {
    switch (type) {
        case SCREENSAVER_NONE:      return "None";
        case SCREENSAVER_BLANK:     return "Blank";
        case SCREENSAVER_STARFIELD: return "Starfield";
        case SCREENSAVER_LINES:     return "Lines";
        case SCREENSAVER_BUBBLES:   return "Bubbles";
        case SCREENSAVER_MATRIX:    return "Matrix";
        default:                    return "Unknown";
    }
}
