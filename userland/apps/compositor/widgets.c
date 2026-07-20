// widgets.c - Desktop widget layer for the MayteraOS userland compositor (#77).
// Draws always-present desktop gadgets behind app windows: an analog clock
// (#79), a month calendar (#78) and a walking "sheep" desktop pet (#80).
// Network-backed widgets (weather/crypto/ticker) are added separately once the
// VM is back on the LAN.

#include "compositor.h"
extern int g_draw_blend;   // draw.c blend factor (255=opaque)
extern int g_win_opacity;  // main.c global window opacity
#include "../../libc/syscall.h"

// #102/#379 dirty-rect: when set, widgets_render() only DRAWS (no state advance:
// no sheep/dog update, no sysmon/netinfo sampling). The idle compositor advances
// state + collects damage once per frame via widgets_collect_damage(), then
// draws each dirty rect with this flag set so per-rect redraws never double-tick.
int g_widgets_draw_only = 0;
// Taskbar shares its sampled CPU% (and per-core array) so the sysmon widget and
// the taskbar gauge read the identical source (#102 meter reconciliation).
extern int taskbar_cpu_snapshot(unsigned int *cores, int *ncores);

// Master enable. Defaults on; a Settings toggle can flip g_widgets_enabled.
int g_widgets_enabled = 1;

// sin(i*6deg) * 1024 for clock positions i = 0..59.
static const int SIN60[60] = {
        0,   107,   213,   316,   416,   512,   602,   685,   761,   828,
      887,   935,   974,  1002,  1018,  1024,  1018,  1002,   974,   935,
      887,   828,   761,   685,   602,   512,   416,   316,   213,   107,
        0,  -107,  -213,  -316,  -416,  -512,  -602,  -685,  -761,  -828,
     -887,  -935,  -974, -1002, -1018, -1024, -1018, -1002,  -974,  -935,
     -887,  -828,  -761,  -685,  -602,  -512,  -416,  -316,  -213,  -107,
};
static int sin60(int i) { i %= 60; if (i < 0) i += 60; return SIN60[i]; }
static int cos60(int i) { return sin60(i + 15); }   // cos(t) = sin(t + 90deg)

// Simple integer (Bresenham) line for clock hands.
extern int g_draw_blend;   // draw.c: global blend op (255 = opaque)
// Per-pixel alpha blend (coverage 0..255), honouring g_draw_blend.
static void wdg_aa_px(int x, int y, uint32_t color, int cov) {
    if (x < 0 || y < 0 || x >= g_fb_width || y >= g_fb_height || cov <= 0) return;
    if (cov > 255) cov = 255;
    int op = (g_draw_blend < 255) ? (cov * g_draw_blend / 255) : cov;
    uint32_t *d = &g_fb[y * g_fb_pitch + x];
    if (op >= 255) { *d = color; return; }
    uint32_t bg = *d;
    int r = ((((color >> 16) & 0xFF) * op) + (((bg >> 16) & 0xFF) * (255 - op))) / 255;
    int g = ((((color >> 8)  & 0xFF) * op) + (((bg >> 8)  & 0xFF) * (255 - op))) / 255;
    int b = (((color & 0xFF) * op) + ((bg & 0xFF) * (255 - op))) / 255;
    *d = (uint32_t)((r << 16) | (g << 8) | b);
}
static int wdg_isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
// Anti-aliased ring (outline circle) of radius R, ~2px stroke, centered (cx,cy).
static void wdg_ring(int cx, int cy, int R, uint32_t color) {
    for (int dy = -R - 2; dy <= R + 2; dy++)
        for (int dx = -R - 2; dx <= R + 2; dx++) {
            int d = wdg_isqrt(dx * dx + dy * dy);
            int diff = d - R; if (diff < 0) diff = -diff;
            int a = (diff == 0) ? 255 : (diff == 1) ? 150 : 0;   // 2px AA stroke
            if (a) wdg_aa_px(cx + dx, cy + dy, color, a);
        }
}
// Anti-aliased line (Xiaolin Wu, fixed-point) for crisp clock hands + ticks.
static void wdg_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == 0 && ady == 0) { wdg_aa_px(x0, y0, color, 255); return; }
    int steep = ady > adx, t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    dx = x1 - x0; dy = y1 - y0;
    int grad = (dx == 0) ? (dy << 8) : ((dy << 8) / dx);
    int inter = (y0 << 8);
    for (int x = x0; x <= x1; x++) {
        int yi = inter >> 8, fr = inter & 0xFF;
        if (steep) { wdg_aa_px(yi, x, color, 255 - fr); wdg_aa_px(yi + 1, x, color, fr); }
        else       { wdg_aa_px(x, yi, color, 255 - fr); wdg_aa_px(x, yi + 1, color, fr); }
        inter += grad;
    }
}

// A hand drawn as a short cluster of parallel lines for visible thickness.
static void wdg_hand(int cx, int cy, int pos, int len, int thick, uint32_t color) {
    int ex = cx + sin60(pos) * len / 1024;
    int ey = cy - cos60(pos) * len / 1024;
    wdg_line(cx, cy, ex, ey, color);
    for (int t = 1; t < thick; t++) {
        wdg_line(cx + t, cy, ex + t, ey, color);
        wdg_line(cx, cy + t, ex, ey + t, color);
    }
}

// --- Analog clock (#79) ---------------------------------------------------
static void widget_analog_clock(int cx, int cy, int r) {
    draw_circle_filled(cx, cy, r, CLR_MENU_BG);
    draw_circle_outline(cx, cy, r, CLR_MENU_TEXT);
    draw_circle_outline(cx, cy, r - 1, CLR_MENU_BORDER);

    // Hour ticks (every 5 positions) + minute ticks.
    for (int i = 0; i < 60; i++) {
        int outer = r - 2;
        int inner = (i % 5 == 0) ? r - 8 : r - 4;
        uint32_t tc = (i % 5 == 0) ? CLR_MENU_TEXT : CLR_MENU_BORDER;
        int ox = cx + sin60(i) * outer / 1024;
        int oy = cy - cos60(i) * outer / 1024;
        int ix = cx + sin60(i) * inner / 1024;
        int iy = cy - cos60(i) * inner / 1024;
        wdg_line(ix, iy, ox, oy, tc);
    }

    int h, m, s;
    get_rtc_time(&h, &m, &s);
    int hour_pos = ((h % 12) * 5 + m / 12) % 60;

    wdg_hand(cx, cy, hour_pos, r - 22, 2, CLR_MENU_TEXT);  // hour
    wdg_hand(cx, cy, m,        r - 12, 2, CLR_MENU_TEXT);  // minute
    wdg_hand(cx, cy, s,        r - 8,  1, 0x00FF5050);  // second
    draw_circle_filled(cx, cy, 3, 0x00FFD040);          // hub
}

// --- Month calendar (#78) -------------------------------------------------
static const char *MON_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int days_in_month(int m, int y) {
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    if (m < 1 || m > 12) return 30;
    return d[m - 1];
}

// Zeller's congruence -> weekday of (d/m/y), returned as 0=Sunday..6=Saturday.
static int day_of_week(int d, int m, int y) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (d + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7; // 0=Sat
    return (h + 6) % 7;   // shift so 0=Sunday
}

static void itoa2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; b[2] = '\0'; }

static void widget_calendar(int x, int y, int w) {
    int dd, mm, yy;
    get_rtc_date(&dd, &mm, &yy);
    if (mm < 1 || mm > 12) mm = 1;
    if (yy < 1970 || yy > 3000) yy = 2026;

    int cell = w / 7;
    int header_h = FONT_CHAR_H + 6;
    int dow_h = FONT_CHAR_H + 2;
    int rows = 6;
    int h = header_h + dow_h + rows * (FONT_CHAR_H + 2) + 6;

    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);

    // Title: "Month Year"
    char title[32];
    const char *mn = MON_NAMES[mm - 1];
    int ti = 0;
    while (mn[ti]) { title[ti] = mn[ti]; ti++; }
    title[ti++] = ' ';
    title[ti++] = '0' + (yy / 1000) % 10;
    title[ti++] = '0' + (yy / 100) % 10;
    title[ti++] = '0' + (yy / 10) % 10;
    title[ti++] = '0' + yy % 10;
    title[ti] = '\0';
    draw_text_centered(x + w / 2, y + 4, title, readable_accent(0x00FFD040, CLR_MENU_BG));

    // Weekday header
    static const char *wd[7] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    int gy = y + header_h;
    for (int c = 0; c < 7; c++) {
        uint32_t wc = (c == 0 || c == 6) ? readable_accent(0x00FF8080, CLR_MENU_BG) : CLR_MENU_TEXT;
        draw_text(x + c * cell + (cell - text_width(wd[c])) / 2, gy, wd[c], wc);
    }

    int first = day_of_week(1, mm, yy);
    int ndays = days_in_month(mm, yy);
    int gy0 = gy + dow_h;
    int row_h = FONT_CHAR_H + 2;
    for (int day = 1; day <= ndays; day++) {
        int idx = first + day - 1;
        int col = idx % 7;
        int row = idx / 7;
        int cx = x + col * cell;
        int cyy = gy0 + row * row_h;
        char num[3]; int n = day; char nb[4];
        if (n < 10) { nb[0] = '0' + n; nb[1] = '\0'; }
        else itoa2(nb, n);
        (void)num;
        uint32_t col_text = (col == 0 || col == 6) ? readable_accent(0x00FF9090, CLR_MENU_BG) : CLR_MENU_TEXT;
        if (day == dd) {
            // Circle the current day with a high-contrast AA ring (2px). The old
            // readable_accent(gold) rendered near-invisible on dark cards; use a
            // luminance-contrast ink so it stands out on ANY theme.
            int rr = (row_h < cell ? row_h : cell) / 2 - 1; if (rr < 6) rr = 6;
            uint32_t ring = readable_ink(CLR_MENU_BG);
            wdg_ring(cx + cell / 2, cyy + FONT_CHAR_H / 2, rr,     ring);
            wdg_ring(cx + cell / 2, cyy + FONT_CHAR_H / 2, rr - 1, ring);
        }
        draw_text(cx + (cell - text_width(nb)) / 2, cyy, nb, col_text);
    }
}

// --- Sheep desktop pets (#80, multi): drag + gravity + speed/size/style ---
#define MAX_SHEEP 50
int g_sheep_enabled = 0;
int g_show_clock     = 0;
int g_aichat_enabled = 1;   // #185 AI Chat docked panel app (default ON; #453 busy-spin fixed in b740)
int g_show_calendar  = 0;
int g_sheep_speed    = 3;   // 1..5
int g_sheep_size     = 2;   // 1..3 (small/normal/large)
int g_sheep_style    = 0;   // 0 classic, 1 spotted
int g_sheep_count    = 1;   // 1..50

// Sheepdog state (declared early so sheep flee logic can see it).
int g_dog_enabled = 0;   // sheepdog: enable via the sheep tray menu
static int dog_x = -1, dog_y, dog_dir = 1, dog_dragging = 0, dog_gdx, dog_gdy;
static unsigned dog_frame = 0;
#define DOG_W 56
#define DOG_H 30

typedef struct {
    int x, y, vy, dir, state, land_t, blink_t, inited;
    int behavior, btimer;   // 0 walk,1 sleep,2 run,3 fart
    int climb;              // climbing the screen edge
    unsigned frame, rng;
    int anim, aidx, ahold;   // one-shot eSheep animation (#80): -1 = none
} sheep_t;
static sheep_t g_sheep[MAX_SHEEP];
static int g_grabbed = -1;
static int g_grab_dx = 0, g_grab_dy = 0;

static int sheep_sc(void){ return (g_sheep_size<=1)?75 : (g_sheep_size>=3)?135 : 100; }
static int sheep_w(void){ return 50 * sheep_sc() / 100; }
static int sheep_h(void){ return 34 * sheep_sc() / 100; }
static int sheep_ground(void){ return g_fb_height - 36 - sheep_h(); }

// Window cache (refreshed once per frame) so sheep can walk on window tops.
static wm_window_info_t g_wlist[16];
static int g_wn = 0;
static int sheep_floor(int x, int w, int cur_y) {
    int g = g_fb_height - 36 - sheep_h();      // taskbar surface (lowest = largest y)
    int best = g;
    int cx = x + w / 2;
    for (int i = 0; i < g_wn; i++) {
        if (!g_wlist[i].visible || g_wlist[i].minimized) continue;
        if (cx >= g_wlist[i].x && cx < g_wlist[i].x + g_wlist[i].width) {
            int top = g_wlist[i].y - sheep_h();
            // Only a valid floor if it is at/below the sheep's current feet, so a
            // sheep on the taskbar does NOT teleport up onto a window above it.
            // A sheep only rests on a window it fell onto, then walks off it.
            if (top > 24 && top >= cur_y && top < best) best = top;
        }
    }
    return best;
}

static void sheep_spawn(int i) {
    unsigned seed = (unsigned)(i * 2654435761u) ^ 0x9e3779b9u;
    int span = g_fb_width - 120; if (span < 1) span = 1;
    g_sheep[i].x = 40 + (int)((seed >> 13) % (unsigned)span);
    g_sheep[i].y = sheep_ground();
    g_sheep[i].vy = 0;
    g_sheep[i].dir = (i & 1) ? 1 : -1;
    g_sheep[i].state = 0;
    g_sheep[i].frame = seed & 0x3F;
    g_sheep[i].land_t = 0;
    g_sheep[i].blink_t = 0;
    g_sheep[i].behavior = 0;
    g_sheep[i].climb = 0;
    g_sheep[i].anim = -1; g_sheep[i].aidx = 0; g_sheep[i].ahold = 0;
    g_sheep[i].btimer = (int)(seed % 120u);
    g_sheep[i].rng = seed | 1u;
    g_sheep[i].inited = 1;
}

// Public hit / drag interface (operates on the sheep under the cursor).
int sheep_hit(int x, int y) {
    if (!g_sheep_enabled) return 0;
    int w = sheep_w(), h = sheep_h();
    for (int i = 0; i < g_sheep_count && i < MAX_SHEEP; i++) {
        if (!g_sheep[i].inited) continue;
        if (x >= g_sheep[i].x && x < g_sheep[i].x + w &&
            y >= g_sheep[i].y && y < g_sheep[i].y + h) return 1;
    }
    return 0;
}
void sheep_grab(int x, int y) {
    int w = sheep_w(), h = sheep_h();
    for (int i = g_sheep_count - 1; i >= 0; i--) {     // topmost first
        if (i >= MAX_SHEEP || !g_sheep[i].inited) continue;
        if (x >= g_sheep[i].x && x < g_sheep[i].x + w &&
            y >= g_sheep[i].y && y < g_sheep[i].y + h) {
            g_grabbed = i; g_sheep[i].state = 3;
            g_grab_dx = x - g_sheep[i].x; g_grab_dy = y - g_sheep[i].y;
            g_sheep[i].vy = 0;
            return;
        }
    }
}
void sheep_drag_to(int x, int y) {
    if (g_grabbed < 0) return;
    sheep_t *sp = &g_sheep[g_grabbed];
    sp->x = x - g_grab_dx; sp->y = y - g_grab_dy;
    if (sp->x < 0) sp->x = 0;
    if (sp->x > g_fb_width - sheep_w()) sp->x = g_fb_width - sheep_w();
    if (sp->y < 0) sp->y = 0;
}
void sheep_release(void) {
    if (g_grabbed < 0) return;
    g_sheep[g_grabbed].state = 1; g_sheep[g_grabbed].vy = 0;
    g_grabbed = -1;
}
int sheep_is_dragging(void) { return g_grabbed >= 0; }


// One-shot eSheep behaviours (frame indices into the 16x11 sheet, from
// src/Actions.js) the idle RNG can randomly trigger (#80).
static const unsigned char SHEEP_ANIM_ROLL[]   = {9,10,126,125,124,123,122,121,120,119,118,117,116,115,114,113,112,10,9};
static const unsigned char SHEEP_ANIM_METEOR[] = {134,135,136,137,138,139,140,141,142,143,144,145};
static const unsigned char SHEEP_ANIM_EAT[]    = {58,59,60,61,60,61,60,61,58,59,60,61};
static const unsigned char SHEEP_ANIM_YAWN[]   = {31,107,108,110,111,110,111,109,31};
static const unsigned char SHEEP_ANIM_HANDS[]  = {78,86,87,86,87,86,87,86,87,78};
static const struct { const unsigned char *f; int count; int hold; } g_sheep_anims[] = {
    { SHEEP_ANIM_ROLL,   19, 3 },   // tumble
    { SHEEP_ANIM_METEOR, 12, 4 },   // fall on fire
    { SHEEP_ANIM_EAT,    12, 6 },   // graze
    { SHEEP_ANIM_YAWN,    9, 8 },   // yawn
    { SHEEP_ANIM_HANDS,  10, 5 },   // walk on hands
};
#define SHEEP_NANIM ((int)(sizeof(g_sheep_anims)/sizeof(g_sheep_anims[0])))

static void sheep_one_update(int idx) {
    sheep_t *sp = &g_sheep[idx];
    sp->frame++;
    int ground = sheep_floor(sp->x, sheep_w(), sp->y);
    if (sp->y > ground && idx != g_grabbed) sp->y = ground;

    if (idx == g_grabbed) { sp->state = 3; return; }

    if (sp->climb) {                       // climbing the screen edge
        sp->state = 1;                     // cling/splayed pose
        sp->y -= 3;
        int top_target = ground - g_fb_height / 3;
        if (top_target < 4) top_target = 4;
        if (sp->y <= top_target) { sp->climb = 0; sp->vy = 0; sp->dir = -sp->dir; }
        sp->frame++;
        return;
    }

    if (sp->y < ground) {
        sp->state = 1;
        if ((sp->frame & 1) == 0 && sp->vy < 9) sp->vy++;
        sp->y += sp->vy;
        if (sp->y >= ground) {
            sp->y = ground;
            if (sp->vy > 4) sp->vy = -(sp->vy / 3);
            else { sp->vy = 0; sp->state = 2; sp->land_t = 10; }
        }
    } else {
        sp->y = ground;
        if (sp->state == 2) { if (--sp->land_t <= 0) sp->state = 0; }
        else {
            sp->state = 0;
            // A one-shot special animation is playing: advance it, hold position.
            if (sp->anim >= 0) {
                if (++sp->ahold >= g_sheep_anims[sp->anim].hold) {
                    sp->ahold = 0;
                    if (++sp->aidx >= g_sheep_anims[sp->anim].count) { sp->anim = -1; sp->btimer = 20; }
                }
                if (sp->blink_t > 0) sp->blink_t--;
                return;
            }
            // Idle-behavior machine: walk / sleep / run / fart + random special anims.
            if (sp->btimer > 0) sp->btimer--;
            if (sp->btimer == 0) {
                sp->rng = sp->rng * 1103515245u + 12345u;
                unsigned roll = (sp->rng >> 16) % 100u;
                if      (roll < 45) sp->behavior = 0;          // walk
                else if (roll < 78) sp->behavior = 1;          // sleep
                else if (roll < 83) sp->behavior = 2;          // run
                else if (roll < 87) sp->behavior = 3;          // fart
                else {                                          // ~13%: a special eSheep animation
                    sp->rng = sp->rng * 1103515245u + 12345u;
                    sp->anim = (int)((sp->rng >> 16) % (unsigned)SHEEP_NANIM);
                    sp->aidx = 0; sp->ahold = 0;
                    sp->behavior = 0;
                }
                sp->rng = sp->rng * 1103515245u + 12345u;
                sp->btimer = 90 + (int)((sp->rng >> 16) % 210u);   // ~1.5-5s
            }
            // Flee from the sheepdog if it is close.
            if (g_dog_enabled) {
                int ddx = (sp->x + sheep_w()/2) - (dog_x + DOG_W/2);
                int ady = (sp->y - dog_y); if (ady < 0) ady = -ady;
                int adx = ddx < 0 ? -ddx : ddx;
                if (adx < 150 && ady < 120) {
                    sp->dir = (ddx >= 0) ? 1 : -1;     // run away from the dog
                    sp->behavior = 2;                  // run
                    sp->btimer = 30;
                }
            }
            if (sp->behavior == 0 || sp->behavior == 2) {          // walk / run
                int run = (sp->behavior == 2);
                int iv = run ? 2 : (8 - g_sheep_speed); if (iv < 2) iv = 2;
                int step = (2 + g_sheep_speed) * (run ? 2 : 1);
                if ((sp->frame % (unsigned)iv) == 0) {
                    sp->x += sp->dir * step;
                    if (sp->x > g_fb_width - sheep_w()) {
                        sp->x = g_fb_width - sheep_w();
                        sp->rng = sp->rng * 1103515245u + 12345u;
                        if (((sp->rng >> 16) % 100u) < 30) sp->climb = 1; else sp->dir = -1;
                    } else if (sp->x < 0) {
                        sp->x = 0;
                        sp->rng = sp->rng * 1103515245u + 12345u;
                        if (((sp->rng >> 16) % 100u) < 30) sp->climb = 1; else sp->dir = 1;
                    }
                }
            }
            if (sp->blink_t > 0) sp->blink_t--;
            else if ((sp->frame % 180) == 0) sp->blink_t = 8;
        }
    }
}


// ===== eSheep sprite-sheet rendering (#80/#93). Loads /SHEEP.SPR once (raw
// ARGB grid, 16x11 cells of 40x40 from github.com/kuindji/sheep-js) and blits
// the frame for the current state, replacing the procedural draw. Falls back to
// the procedural sheep if the asset is missing. =====
#define SHEEP_SHEET_W   640
#define SHEEP_SHEET_H   440
#define SHEEP_CELL      40
#define SHEEP_GRID_COLS 16
static uint32_t g_sheep_sheet[SHEEP_SHEET_W * SHEEP_SHEET_H];
static int g_sheep_sheet_ok = 0;
static int g_sheep_sheet_tried = 0;

static void sheep_sheet_load(void) {
    if (g_sheep_sheet_tried) return;
    g_sheep_sheet_tried = 1;
    int fd = sys_open("/SHEEP.SPR", 0);
    if (fd < 0) return;
    unsigned char hdr[12];
    if (sys_read(fd, hdr, 12) != 12 ||
        hdr[0] != 'S' || hdr[1] != 'H' || hdr[2] != 'P' || hdr[3] != '1') {
        sys_close(fd); return;
    }
    unsigned int w = hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((unsigned)hdr[7]<<24);
    unsigned int h = hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | ((unsigned)hdr[11]<<24);
    if (w != SHEEP_SHEET_W || h != SHEEP_SHEET_H) { sys_close(fd); return; }
    int total = (int)(w * h * 4), got = 0;
    unsigned char *dst = (unsigned char *)g_sheep_sheet;
    while (got < total) {
        long n = sys_read(fd, dst + got, total - got);
        if (n <= 0) break;
        got += (int)n;
    }
    sys_close(fd);
    if (got == total) g_sheep_sheet_ok = 1;
}

static int sheep_sprites_ready(void) {
    sheep_sheet_load();
    return g_sheep_sheet_ok;
}

// Blit grid frame `idx` (row-major, 0..175) to (dx,dy) scaled to dw x dh, with
// alpha compositing and optional horizontal flip.
static void sheep_blit_frame(int idx, int dx, int dy, int dw, int dh, int hflip) {
    if (!g_sheep_sheet_ok || dw <= 0 || dh <= 0) return;
    int sx0 = (idx % SHEEP_GRID_COLS) * SHEEP_CELL;
    int sy0 = (idx / SHEEP_GRID_COLS) * SHEEP_CELL;
    int stepx = (SHEEP_CELL << 8) / dw, stepy = (SHEEP_CELL << 8) / dh, syf = 0;
    for (int r = 0; r < dh; r++, syf += stepy) {
        int sy = syf >> 8; if (sy > SHEEP_CELL-1) sy = SHEEP_CELL-1;
        int py = dy + r; if (py < 0 || py >= g_fb_height) continue;
        int sxf = 0;
        for (int c = 0; c < dw; c++, sxf += stepx) {
            int sx = sxf >> 8; if (sx > SHEEP_CELL-1) sx = SHEEP_CELL-1;
            int col = hflip ? (SHEEP_CELL-1 - sx) : sx;
            int px = dx + c; if (px < 0 || px >= g_fb_width) continue;
            uint32_t spx = g_sheep_sheet[(sy0 + sy) * SHEEP_SHEET_W + (sx0 + col)];
            uint32_t a = (spx >> 24) & 0xFF;
            if (a == 0) continue;
            uint32_t sr = (spx >> 16) & 0xFF, sg = (spx >> 8) & 0xFF, sb = spx & 0xFF;
            uint32_t *d = &g_fb[py * g_fb_pitch + px];
            if (a == 255) { *d = 0xFF000000u | (sr<<16) | (sg<<8) | sb; continue; }
            uint32_t dv = *d, dR = (dv>>16)&0xFF, dG = (dv>>8)&0xFF, dB = dv & 0xFF;
            uint32_t rr = (sr*a + dR*(255-a))/255, rg = (sg*a + dG*(255-a))/255, rb = (sb*a + dB*(255-a))/255;
            *d = 0xFF000000u | (rr<<16) | (rg<<8) | rb;
        }
    }
}

// Map the widget's sheep state to an eSheep frame index (left-facing base art).
static int sheep_frame_idx(const sheep_t *sp) {
    if (sp->anim >= 0 && sp->aidx < g_sheep_anims[sp->anim].count)
        return g_sheep_anims[sp->anim].f[sp->aidx];
    unsigned fr = sp->frame;
    if (sp->climb)                              { int q[2] = {40, 41}; return q[(fr >> 2) & 1]; } // climb
    if (sp->state == 3)                         return 71;                                       // held/grabbed (arms up)
    if (sp->state == 0 && sp->behavior == 1)    { int q[2] = {0, 1};   return q[(fr >> 4) & 1]; } // sleep
    if (sp->state == 0 && sp->behavior == 3)    return 6;                                        // fart -> stand (no fart art)
    if (sp->state == 2)                         { int q[2] = {4, 5};   return q[(fr >> 1) & 1]; } // run
    if (sp->state == 1)                         return 6;                                        // stand / fall / cling
    int q[2] = {2, 3}; return q[(fr >> 2) & 1];                                                   // walk
}

static void sheep_draw_sprite(const sheep_t *sp) {
    int sc = sheep_sc();
    int dw = SHEEP_CELL * sc / 100, dh = dw;        // square frame, scaled to sheep size
    int dx = sp->x + (sheep_w() - dw) / 2;          // centre over the logical box
    int dy = sp->y + sheep_h() - dh;                // feet on the box bottom (ground)
    int hflip = (sp->dir >= 0);                     // sheet faces LEFT; flip for right
    sheep_blit_frame(sheep_frame_idx(sp), dx, dy, dw, dh, hflip);
}

static void sheep_one_draw(const sheep_t *sp) {
    if (sheep_sprites_ready()) { sheep_draw_sprite(sp); return; }
    int sc = sheep_sc();
    int bx = sp->x, by = sp->y;
    uint32_t wool = 0x00F0F0F0, woolsh = 0x00C8C8C8, face = 0x00303030;
    #define PX(v) (bx + (v) * sc / 100)
    #define PY(v) (by + (v) * sc / 100)
    #define PR(v) (((v) * sc / 100) < 1 ? 1 : ((v) * sc / 100))
    int squash = (sp->state == 2) ? PR(4) : 0;
    int lie = (sp->state == 0 && sp->behavior == 1) ? PR(8) : 0;  // sleeping: lie down

    if (sp->climb) {
        // Rotated 90deg against the wall (feet on the edge), head up. Left wall
        // = clockwise, right wall = counter-clockwise.
        int left = (bx < g_fb_width / 2);
        int wallx = left ? 0 : g_fb_width;                       // the edge the feet grip
        // ly = distance perpendicular to the wall (feet at ly~34 grip the edge),
        // lx = position along the body (maps to vertical; head lx=40 is up).
        #define TX(ly) (left ? (wallx + (34-(ly))*sc/100) : (wallx - (34-(ly))*sc/100))
        #define TY(lx) (by + (50-(lx))*sc/100)
        draw_circle_filled(TX(14), TY(18), PR(11), woolsh);
        draw_circle_filled(TX(13), TY(30), PR(11), woolsh);
        draw_circle_filled(TX(10), TY(24), PR(12), wool);
        draw_circle_filled(TX(12), TY(16), PR(9),  wool);
        draw_circle_filled(TX(12), TY(32), PR(9),  wool);
        if (g_sheep_style == 1) {
            draw_circle_filled(TX(11), TY(20), PR(3), woolsh);
            draw_circle_filled(TX(15), TY(29), PR(3), woolsh);
        }
        draw_circle_filled(TX(14), TY(40), PR(7), face);   // head (up)
        draw_circle_filled(TX(9),  TY(40), PR(6), wool);
        draw_circle_filled(TX(13), TY(42), PR(1), 0x00FFFFFF);
        // 4 legs gripping the wall, animated so they "step" while climbing
        int ph = sp->frame / 4;
        for (int i = 0; i < 4; i++) {
            int lxp = 8 + i * 9;                  // position along the body
            int step = ((ph + i) & 1);            // alternate legs reach/pull
            int footly = step ? 34 : 29;          // 34 = flush to the wall
            int kX = TX(22), kY = TY(lxp);
            int fX = TX(footly), fY = TY(lxp + (step ? 1 : -1));
            int rx = fX < kX ? fX : kX, ry = fY < kY ? fY : kY;
            int rw = (fX > kX ? fX-kX : kX-fX) + PR(3);
            int rh = (fY > kY ? fY-kY : kY-fY) + PR(3);
            draw_fill_rect(rx, ry, rw, rh, face);
        }
        #undef TX
        #undef TY
        return;
    }

    if (lie) {                                   // lying: tucked legs (low stubs)
        draw_fill_rect(PX(11), PY(24) + lie, PR(8), PR(3), face);
        draw_fill_rect(PX(27), PY(24) + lie, PR(8), PR(3), face);
    } else if (sp->state == 1) {
        draw_fill_rect(PX(6),  PY(20), PR(4), PR(8), face);
        draw_fill_rect(PX(16), PY(22), PR(4), PR(6), face);
        draw_fill_rect(PX(30), PY(22), PR(4), PR(6), face);
        draw_fill_rect(PX(40), PY(20), PR(4), PR(8), face);
    } else if (sp->state == 3) {
        draw_fill_rect(PX(12), PY(22), PR(4), PR(11), face);
        draw_fill_rect(PX(22), PY(22), PR(4), PR(12), face);
        draw_fill_rect(PX(34), PY(22), PR(4), PR(11), face);
    } else {
        int walk = (sp->state == 0) ? ((sp->frame >> 2) & 1) : 0;
        draw_fill_rect(PX(10), PY(22) + squash, PR(4), PR(9 - walk * 2), face);
        draw_fill_rect(PX(22), PY(22) + squash, PR(4), PR(7 + walk * 2), face);
        draw_fill_rect(PX(34), PY(22) + squash, PR(4), PR(9 - walk * 2), face);
    }
    int byo = squash + lie;
    draw_circle_filled(PX(18), PY(14) + byo, PR(11), woolsh);
    draw_circle_filled(PX(30), PY(13) + byo, PR(11), woolsh);
    draw_circle_filled(PX(24), PY(10) + byo, PR(12), wool);
    draw_circle_filled(PX(16), PY(12) + byo, PR(9),  wool);
    draw_circle_filled(PX(32), PY(12) + byo, PR(9),  wool);
    if (g_sheep_style == 1) {
        draw_circle_filled(PX(20), PY(11) + byo, PR(3), woolsh);
        draw_circle_filled(PX(29), PY(15) + byo, PR(3), woolsh);
        draw_circle_filled(PX(24), PY(8)  + byo, PR(2), woolsh);
    }
    int hx = (sp->dir >= 0) ? PX(40) : PX(8);
    draw_circle_filled(hx, PY(14) + byo, PR(7), face);
    draw_circle_filled(hx, PY(9)  + byo, PR(6), wool);
    int eye = (sp->dir >= 0) ? hx + PR(2) : hx - PR(2);
    if (sp->blink_t > 0) draw_fill_rect(eye - 1, PY(13) + byo, 3, 1, 0x00FFFFFF);
    else draw_circle_filled(eye, PY(13) + byo, PR(1), 0x00FFFFFF);
    draw_fill_rect((sp->dir >= 0) ? hx - PR(4) : hx + PR(2), PY(12) + byo, PR(3), PR(4), woolsh);
    if (sp->state == 0 && sp->behavior == 1) {          // sleeping: closed eye + Zzz
        draw_fill_rect(eye - 1, PY(13) + byo, 3, 1, face);
        int zx = (sp->dir >= 0) ? PX(44) : PX(0);
        draw_text(zx, by - 7, "z", CLR_MENU_TEXT);
        draw_text(zx + 5, by - 14, "z", CLR_MENU_TEXT);
    } else if (sp->state == 0 && sp->behavior == 3) {   // fart puff behind
        int px = (sp->dir >= 0) ? PX(2) : PX(46);
        draw_circle_filled(px, PY(21) + byo, PR(3), 0x0098C878);
        draw_circle_filled(px + (sp->dir >= 0 ? -4 : 4), PY(18) + byo, PR(2), 0x00B8E0A0);
    }
    #undef PX
    #undef PY
    #undef PR
}

// --- Sheepdog (border collie): drag to herd; sheep flee from it (#93) -------
static int dog_ground(void) { return g_fb_height - 36 - DOG_H; }

int dog_hit(int x, int y) {
    if (!g_dog_enabled) return 0;
    return x >= dog_x && x < dog_x + DOG_W && y >= dog_y && y < dog_y + DOG_H;
}
void dog_grab(int x, int y) { dog_dragging = 1; dog_gdx = x - dog_x; dog_gdy = y - dog_y; }
void dog_drag_to(int x, int y) {
    if (!dog_dragging) return;
    int new_x = x - dog_gdx;
    if (new_x < dog_x) dog_dir = -1;        // dragged left: face left
    else if (new_x > dog_x) dog_dir = 1;    // dragged right: face right
    dog_x = new_x; dog_y = y - dog_gdy;
    if (dog_x < 0) dog_x = 0;
    if (dog_x > g_fb_width - DOG_W) dog_x = g_fb_width - DOG_W;
    if (dog_y < 0) dog_y = 0;
}
void dog_release(void) { dog_dragging = 0; }
int dog_is_dragging(void) { return dog_dragging; }

static void dog_update(void) {
    if (dog_x < 0) { dog_x = g_fb_width / 2; dog_y = dog_ground(); }
    dog_frame++;
    if (dog_dragging) return;
    if (dog_y < dog_ground()) { dog_y += 5; if (dog_y > dog_ground()) dog_y = dog_ground(); }
    else if ((dog_frame & 7) == 0) {
        dog_x += dog_dir * 2;
        if (dog_x > g_fb_width - DOG_W) dog_dir = -1;
        if (dog_x < 0) dog_dir = 1;
    }
}

static void dog_draw(void) {
    int bx = dog_x, by = dog_y;
    uint32_t blk = 0x00282828, wht = 0x00F0F0F0;
    int fwd = (dog_dir >= 0);
    int walk = (dog_frame >> 2) & 1;
    // four thin legs
    draw_fill_rect(bx + 13, by + 19, 3, 9 - walk * 2, blk);
    draw_fill_rect(bx + 20, by + 19, 3, 7 + walk * 2, blk);
    draw_fill_rect(bx + 33, by + 19, 3, 7 + walk * 2, blk);
    draw_fill_rect(bx + 40, by + 19, 3, 9 - walk * 2, blk);
    // elongated body with rounded ends
    draw_fill_rect(bx + 13, by + 9, 30, 11, blk);
    draw_circle_filled(bx + 15, by + 14, 6, blk);
    draw_circle_filled(bx + 41, by + 14, 6, blk);
    draw_fill_rect(bx + 17, by + 16, 22, 4, wht);          // white underbelly
    // raised tail at the rear
    if (fwd) { draw_fill_rect(bx + 8, by + 5, 3, 9, blk); draw_fill_rect(bx + 6, by + 4, 4, 3, blk); }
    else     { draw_fill_rect(bx + 45, by + 5, 3, 9, blk); draw_fill_rect(bx + 46, by + 4, 4, 3, blk); }
    // neck + head at the front
    int hx = fwd ? bx + 46 : bx + 10;
    draw_fill_rect(fwd ? bx + 40 : bx + 14, by + 7, 6, 9, blk);   // neck
    draw_circle_filled(hx, by + 8, 6, blk);                       // head
    // snout poking forward + nose
    if (fwd) { draw_fill_rect(hx + 3, by + 9, 8, 4, blk); draw_fill_rect(hx + 10, by + 9, 2, 3, blk); }
    else     { draw_fill_rect(hx - 11, by + 9, 8, 4, blk); draw_fill_rect(hx - 12, by + 9, 2, 3, blk); }
    // pointy ears
    draw_fill_rect(hx - 4, by + 0, 3, 5, blk);
    draw_fill_rect(hx + 1, by + 0, 3, 5, blk);
    // white face blaze + eye
    draw_circle_filled(hx, by + 6, 2, wht);
    draw_circle_filled(fwd ? hx + 2 : hx - 2, by + 6, 1, 0x00101010);
}

// ===========================================================================
// #274: three new desktop widgets (System Monitor, Timer/Stopwatch, World
// Time). Each is a normal draggable/lockable/theme-aware widget that the
// widget framework owns: position + drag + lock + right-click + visibility +
// persistence are all handled exactly like the clock/calendar via widget_box(),
// widget_lock_ptr(), widget_vis_ptr() and the widget_registry().
//   widget id 6 = System Monitor, 7 = Timer/Stopwatch, 8 = World Time.
// ===========================================================================

// Small helper: int -> decimal string (no libc itoa in this freestanding unit).
static char *w_itoa(char *b, int v) {
    int i = 0, neg = 0; char t[12];
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    int j = 0; if (neg) b[j++] = '-';
    while (i) b[j++] = t[--i];
    b[j] = '\0';
    return b;
}

// --- System Monitor mini (id 6): live CPU / RAM / Net bars + CPU sparkline ---
int g_show_sysmon = 0;
int g_sysmon_x = -1, g_sysmon_y = -1, g_sysmon_locked = 0;
#define SYSMON_W   188
#define SYSMON_H   162
#define SPARK_N    48
static unsigned char s_spark_cpu[SPARK_N];   // ring buffer of recent CPU %
static int s_spark_head = 0, s_spark_filled = 0;
static int s_sysmon_tick = 0;
static int s_cpu_pct = 0, s_ram_pct = 0, s_net_pct = 0;
static unsigned int s_cpu_cores[65];          // [0]=count, [1..]=per-core % (#279)
static int s_cpu_ncores = 1;

static void sysmon_sample(void) {
    // #102: read the SAME CPU sample the taskbar gauge uses so the widget CPU%
    // and the taskbar CPU% never disagree (single accurate source).
    s_cpu_pct = taskbar_cpu_snapshot(s_cpu_cores, &s_cpu_ncores);
    if (s_cpu_pct < 0) s_cpu_pct = 0;
    if (s_cpu_pct > 100) s_cpu_pct = 100;
    if (s_cpu_ncores < 1) s_cpu_ncores = 1;
    if (s_cpu_ncores > 64) s_cpu_ncores = 64;
    unsigned long total = 0, used = 0;
    sys_get_mem_info(&total, &used);
    s_ram_pct = (total > 0) ? (int)(used * 100UL / total) : 0;
    if (s_ram_pct > 100) s_ram_pct = 100;
    static unsigned long s_last_bytes = 0; static int primed = 0;
    unsigned long now_bytes = get_net_bytes();
    if (!primed) { primed = 1; s_net_pct = 0; }
    else {
        unsigned long d = now_bytes - s_last_bytes;     // bytes since last sample
        s_net_pct = (int)((d * 100UL) / 6250000UL);     // ~half-Gbit window full-scale
        if (s_net_pct > 100) s_net_pct = 100;
    }
    s_last_bytes = now_bytes;
    s_spark_cpu[s_spark_head] = (unsigned char)s_cpu_pct;
    s_spark_head = (s_spark_head + 1) % SPARK_N;
    if (s_spark_filled < SPARK_N) s_spark_filled++;
}

// One labeled meter row: label, colored fill bar, value %.
static void sysmon_bar(int x, int y, int w, const char *label, int pct, uint32_t col) {
    draw_text(x, y, label, CLR_MENU_TEXT);
    char vb[8]; w_itoa(vb, pct);
    int vl = text_width(vb) + text_width("%");
    char vv[10]; int vi = 0; for (int k = 0; vb[k]; k++) vv[vi++] = vb[k]; vv[vi++]='%'; vv[vi]=0;
    draw_text(x + w - vl, y, vv, CLR_MENU_TEXT);
    int by = y + FONT_CHAR_H + 1, bh = 7;
    draw_fill_rect(x, by, w, bh, 0x00202830);
    draw_rect_outline(x, by, w, bh, CLR_MENU_BORDER);
    int fw = pct * (w - 2) / 100; if (fw < 0) fw = 0; if (fw > w - 2) fw = w - 2;
    draw_fill_rect(x + 1, by + 1, fw, bh - 2, col);
}

static void widget_sysmon(int x, int y) {
    // Sampling side-effect is suppressed in draw-only (idle per-rect) mode; the
    // idle path advances the sample once via sysmon_tick_sample().
    if (!g_widgets_draw_only && (s_sysmon_tick++ % 15 == 0)) sysmon_sample();
    int w = SYSMON_W, h = SYSMON_H;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + 10, y + 6, "System Monitor", readable_accent(0x0066C0FF, CLR_MENU_BG));
    int ix = x + 10, iw = w - 20;
    sysmon_bar(ix, y + 26, iw, "CPU", s_cpu_pct, 0x0050C050);
    // Per-core strip (#279): one vertical bar per core, height = that core's %.
    {
        int cy = y + 60, ch = 16;
        draw_text(ix, cy, "Cores", CLR_MENU_TEXT);
        int bx0 = ix + text_width("Cores") + 6;
        int bw_avail = iw - (bx0 - ix);
        int nc = s_cpu_ncores; if (nc < 1) nc = 1;
        int gap = (nc > 1) ? 1 : 0;
        int cbw = (bw_avail - gap * (nc - 1)) / nc; if (cbw < 1) cbw = 1;
        for (int i = 0; i < nc; i++) {
            int pct = (int)s_cpu_cores[1 + i];
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            int bx = bx0 + i * (cbw + gap);
            draw_fill_rect(bx, cy - 2, cbw, ch, 0x00202830);
            int fh = pct * (ch - 2) / 100; if (fh < 0) fh = 0;
            // green<60, amber<85, red otherwise
            uint32_t col = pct < 60 ? 0x0050C050 : pct < 85 ? 0x00E0C040 : 0x00E05050;
            if (fh > 0) draw_fill_rect(bx, cy - 2 + (ch - 1 - fh), cbw, fh, col);
        }
    }
    sysmon_bar(ix, y + 82, iw, "RAM", s_ram_pct, 0x000088CC);
    // CPU sparkline strip across the bottom.
    int gx = ix, gy = y + 116, gw = iw, gh = 30;
    draw_fill_rect(gx, gy, gw, gh, 0x00161C22);
    draw_rect_outline(gx, gy, gw, gh, CLR_MENU_BORDER);
    int n = s_spark_filled;
    for (int i = 0; i < n; i++) {
        int idx = (s_spark_head - n + i + SPARK_N * 2) % SPARK_N;
        int v = s_spark_cpu[idx];
        int bx = gx + 1 + i * (gw - 2) / SPARK_N;
        int bh = v * (gh - 2) / 100; if (bh < 1) bh = 1;
        draw_fill_rect(bx, gy + gh - 1 - bh, (gw - 2) / SPARK_N + 1, bh, 0x0040A0E0);
    }
    // Net indicator dot (top-right) reflecting current throughput.
    uint32_t nd = s_net_pct > 50 ? 0x00FF8040 : s_net_pct > 5 ? 0x00FFD040 : 0x00608060;
    draw_circle_filled(x + w - 14, y + 12, 4, nd);
}

// --- Timer / Stopwatch (id 7): stopwatch + countdown, uses uptime_ms() ------
int g_show_timer = 0;
int g_timer_x = -1, g_timer_y = -1, g_timer_locked = 0;
#define TIMER_W  176
#define TIMER_H  96
// mode 0 = stopwatch (counts up), 1 = countdown timer.
static int s_tmr_mode = 0;
static int s_tmr_running = 0;
static unsigned long s_tmr_base = 0;       // uptime_ms at last (re)start
static unsigned long s_tmr_acc = 0;        // accumulated ms while paused
static unsigned long s_tmr_target = 60000; // countdown preset (ms), default 1:00

static unsigned long tmr_elapsed(void) {
    unsigned long e = s_tmr_acc;
    if (s_tmr_running) e += uptime_ms() - s_tmr_base;
    return e;
}
// remaining ms for countdown (clamped at 0)
static unsigned long tmr_remaining(void) {
    unsigned long e = tmr_elapsed();
    return (e >= s_tmr_target) ? 0 : (s_tmr_target - e);
}
static void tmr_start(void) { if (!s_tmr_running) { s_tmr_base = uptime_ms(); s_tmr_running = 1; } }
static void tmr_pause(void) { if (s_tmr_running) { s_tmr_acc += uptime_ms() - s_tmr_base; s_tmr_running = 0; } }
static void tmr_reset(void) { s_tmr_running = 0; s_tmr_acc = 0; s_tmr_base = uptime_ms(); }

// Format ms as M:SS.t (tenths) into buf.
static void tmr_fmt(char *buf, unsigned long ms) {
    unsigned long tenths = (ms / 100) % 10;
    unsigned long secs = (ms / 1000) % 60;
    unsigned long mins = (ms / 60000);
    if (mins > 999) mins = 999;
    int i = 0; char t[6];
    w_itoa(t, (int)mins); for (int k = 0; t[k]; k++) buf[i++] = t[k];
    buf[i++] = ':';
    buf[i++] = '0' + (secs / 10); buf[i++] = '0' + (secs % 10);
    buf[i++] = '.';
    buf[i++] = '0' + (char)tenths;
    buf[i] = '\0';
}

// Button rects within the widget (computed from top-left x,y).
// 3 buttons in a row near the bottom: [Start/Pause] [Reset] [Mode].
static void tmr_btn_rect(int x, int y, int i, int *bx, int *by, int *bw, int *bh) {
    int n = 3, pad = 8, gap = 6;
    int tw = (TIMER_W - pad * 2 - gap * (n - 1)) / n;
    *bx = x + pad + i * (tw + gap);
    *by = y + TIMER_H - 30;
    *bw = tw; *bh = 22;
}
static void widget_timer(int x, int y) {
    int w = TIMER_W, h = TIMER_H;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    const char *title = s_tmr_mode ? "Countdown" : "Stopwatch";
    draw_text(x + 10, y + 6, title, readable_accent(0x00FFC850, CLR_MENU_BG));
    unsigned long ms = s_tmr_mode ? tmr_remaining() : tmr_elapsed();
    char tb[16]; tmr_fmt(tb, ms);
    uint32_t tc = (s_tmr_mode && ms == 0) ? 0x00FF6060 : readable_ink(CLR_MENU_BG);
    draw_text_centered(x + w / 2, y + 26, tb, tc);
    static const char *lbl[3];
    lbl[0] = s_tmr_running ? "Pause" : "Start";
    lbl[1] = "Reset";
    lbl[2] = s_tmr_mode ? "Watch" : "Timer";
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; tmr_btn_rect(x, y, i, &bx, &by, &bw, &bh);
        draw_fill_rect(bx, by, bw, bh, CLR_MENU_ITEM_HOVER);
        draw_rect_outline(bx, by, bw, bh, CLR_MENU_BORDER);
        draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2, lbl[i], CLR_MENU_TEXT);
    }
}
// Handle a click inside the timer widget (returns 1 if a button was hit).
static int timer_click(int x, int y) {
    int wx = g_timer_x, wy = g_timer_y;
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; tmr_btn_rect(wx, wy, i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            if (i == 0) { if (s_tmr_running) tmr_pause(); else tmr_start(); }
            else if (i == 1) tmr_reset();
            else { s_tmr_mode ^= 1; tmr_reset(); }
            return 1;
        }
    }
    return 0;
}

// --- World Time (id 8): three configurable timezone clocks -------------------
int g_show_worldtime = 0;
int g_worldtime_x = -1, g_worldtime_y = -1, g_worldtime_locked = 0;
#define WT_W   188
#define WT_H   96
// WT_ZONES is defined in compositor.h (shared with profile.c).
// Persisted: per-zone UTC offset in minutes and a 3-char label. The compositor
// clock is treated as UTC reference; offsets shift it per zone. (No DST.)
int g_wt_off[WT_ZONES] = { 0, -300, 540 };   // UTC, New York(-5), Tokyo(+9)
static const char *s_wt_lbl[WT_ZONES] = { "UTC", "NYC", "TYO" };

// Apply a signed minute offset to the local RTC h/m and render HH:MM.
static void wt_fmt(char *buf, int off_min) {
    long rtc = sys_get_rtc_time();
    int h = (int)((rtc >> 16) & 0xFF), m = (int)((rtc >> 8) & 0xFF);
    int total = h * 60 + m + off_min;
    total %= (24 * 60); if (total < 0) total += 24 * 60;
    int oh = total / 60, om = total % 60;
    buf[0] = '0' + (oh / 10); buf[1] = '0' + (oh % 10);
    buf[2] = ':';
    buf[3] = '0' + (om / 10); buf[4] = '0' + (om % 10);
    buf[5] = '\0';
}
static void widget_worldtime(int x, int y) {
    int w = WT_W, h = WT_H;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + 10, y + 6, "World Time", readable_accent(0x0066FF99, CLR_MENU_BG));
    int row0 = y + 26, rh = (h - 32) / WT_ZONES;
    for (int z = 0; z < WT_ZONES; z++) {
        int ry = row0 + z * rh;
        draw_text(x + 12, ry, s_wt_lbl[z], CLR_MENU_TEXT);
        char tb[8]; wt_fmt(tb, g_wt_off[z]);
        draw_text(x + w - text_width(tb) - 12, ry, tb, readable_ink(CLR_MENU_BG));
    }
}

// (#282) Uptime widget (id 9): shows time since boot via uptime_ms().
int g_show_uptime = 0;
int g_uptime_x = -1, g_uptime_y = -1, g_uptime_locked = 0;
#define UPT_W   188
#define UPT_H   72
static int upt_num(char *o, unsigned long v) {
    char t[12]; int n = 0;
    if (v == 0) { o[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int k = 0; k < n; k++) o[k] = t[n - 1 - k];
    return n;
}
static int upt_2d(char *o, unsigned long v) {
    o[0] = (char)('0' + (int)((v / 10) % 10));
    o[1] = (char)('0' + (int)(v % 10));
    return 2;
}
static void widget_uptime(int x, int y) {
    int w = UPT_W, h = UPT_H;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + 10, y + 6, "Uptime", readable_accent(0x00FFB060, CLR_MENU_BG));
    unsigned long sec = (unsigned long)(uptime_ms() / 1000UL);
    unsigned long d = sec / 86400UL; sec %= 86400UL;
    unsigned long hh = sec / 3600UL;  sec %= 3600UL;
    unsigned long mm = sec / 60UL;    unsigned long ss = sec % 60UL;
    char buf[40]; int i = 0;
    if (d > 0) { i += upt_num(buf + i, d); buf[i++] = 'd'; buf[i++] = ' '; }
    i += upt_2d(buf + i, hh); buf[i++] = ':';
    i += upt_2d(buf + i, mm); buf[i++] = ':';
    i += upt_2d(buf + i, ss); buf[i] = 0;
    draw_text(x + 14, y + 34, buf, readable_ink(CLR_MENU_BG));
}

// --- Public entry ---------------------------------------------------------
// Persisted widget positions (-1 = use default on first render). ALL widgets
// are draggable, lockable and have a right-click menu. Widget ids:
//   0 clock, 1 calendar, 2 weather, 3 crypto, 4 stocks, 5 digital clock,
//   6 system monitor, 7 timer/stopwatch, 8 world time.
int g_clock_cx = -1, g_clock_cy = -1, g_cal_x = -1, g_cal_y = -1;
int g_clock_locked = 0, g_cal_locked = 0;   // per-widget lock (persisted)
// Digital clock widget (id 5) - state in clock.c, drawn by digclk_draw().
extern int g_show_digclock, g_digclk_x, g_digclk_y, g_digclk_locked, g_digclk_12h, g_digclk_secs, g_digclk_style;
void digclk_geom(int *w, int *h);
void digclk_draw(int x, int y);
static int s_digclk_w = 0, s_digclk_h = 0;
// #81-83 info cards: top-left positions (-1 = default top-center row) + locks.
int g_weather_x = -1, g_weather_y = -1, g_crypto_x = -1, g_crypto_y = -1, g_stocks_x = -1, g_stocks_y = -1;
int g_weather_locked = 0, g_crypto_locked = 0, g_stocks_locked = 0;
static int s_clk_r = 44, s_cal_w = 196, s_cal_h = 0;
#define CARD_W 248
static int s_card_h[3] = { 64, 64, 64 };   // dynamic per-card height (auto-resize)
// Verbosity per card: 1 = detailed (extra lines), 0 = compact. Persisted.
int g_weather_verbose = 1, g_crypto_verbose = 1, g_stocks_verbose = 1;
static int g_wdrag = -1, g_wdx = 0, g_wdy = 0;   // -1 none; 0..4 widget id

// Card descriptor arrays (index 0..2 = weather, crypto, stocks; widget id = 2+i)
static int *s_card_x[3]    = { &g_weather_x, &g_crypto_x, &g_stocks_x };
static int *s_card_y[3]    = { &g_weather_y, &g_crypto_y, &g_stocks_y };
static int *s_card_lock[3] = { &g_weather_locked, &g_crypto_locked, &g_stocks_locked };
static int *s_card_vis[3]  = { &g_show_weather, &g_show_crypto, &g_show_stocks };
static int *s_card_verbose[3] = { &g_weather_verbose, &g_crypto_verbose, &g_stocks_verbose };

// Per-widget right-click menu.
static int g_wmenu = -1, g_wmenu_x = 0, g_wmenu_y = 0;
#define WMENU_W 124
#define WMENU_IH 22

void widget_settings_open(int id);   // forward decl (settings dialog, below)

// ===========================================================================
// #414 Home Assistant desktop widget (id 10). A normal draggable / lockable /
// hideable card that shows the live state of one HA entity. The blocking HTTP
// is done by the background haservice (writes /HA0.TXT = "entity|friendly|state|
// unit|domain"); this widget only reads that cache - it never touches the
// network on the draw thread (#211/#381 rule). Its Settings dialog is an entity
// picker (search + domain filter) over the /HALIST.TXT catalog.
// ===========================================================================
#define HA_ID 10
#define HA_W  224
int g_show_ha = 1;                 // flagship widget: shown by default
int g_ha_x = -1, g_ha_y = -1, g_ha_locked = 0;
static char s_ha_entity[96]="", s_ha_friendly[96]="", s_ha_state[64]="", s_ha_unit[24]="", s_ha_domain[24]="";
static int  s_ha_h = 74;
static char s_ha_cat[200000];      // /HALIST.TXT catalog for the picker
static int  s_ha_cat_len = 0;

// #419 per-widget display config (mode + rename + gauge range + sparkline).
//   mode 0 = value + unit (default)   1 = large number
//        2 = on/off/state badge       3 = gauge/bar (numeric, auto min..max)
int g_ha_mode = 0;                     // persisted (profile: hamode)
int g_ha_min  = 0, g_ha_max = 100;     // gauge bounds, auto-grown + persisted
static char g_ha_label[64] = "";       // custom display name (overrides HA name)
static int  s_ha_spark[64];            // normalized 0..1000 sparkline series
static int  s_ha_spark_n = 0;
static int  s_ha_io_tick  = 0;

static void ha_field(const char *line,int f,char *out,int cap){
    int idx=0,oi=0; out[0]=0;
    for(int i=0; line[i] && line[i]!='\n' && line[i]!='\r'; i++){
        if(line[i]=='|'){ if(idx==f){ out[oi]=0; return; } idx++; oi=0; continue; }
        if(idx==f && oi<cap-1) out[oi++]=line[i];
    }
    if(idx==f) out[oi]=0;
}
static void ha_refresh_cache(void){
    int fd=sys_open("/HA0.TXT",0); if(fd<0) return;
    char b[420]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0) return;
    b[n]=0;
    ha_field(b,0,s_ha_entity,sizeof(s_ha_entity));
    ha_field(b,1,s_ha_friendly,sizeof(s_ha_friendly));
    ha_field(b,2,s_ha_state,sizeof(s_ha_state));
    ha_field(b,3,s_ha_unit,sizeof(s_ha_unit));
    ha_field(b,4,s_ha_domain,sizeof(s_ha_domain));
}
static char ha_lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
// case-insensitive: does haystack contain needle?
static int ha_ci_has(const char *hay,const char *nd){
    if(!nd[0]) return 1;
    for(int i=0; hay[i]; i++){ int j=0; while(nd[j]&&ha_lc(hay[i+j])==ha_lc(nd[j])) j++; if(!nd[j]) return 1; }
    return 0;
}
// Return the nth catalog line (as raw "id|friendly|state") matching filter `flt`
// (substring over the whole line). Returns 1 and fills out, else 0.
static int ha_cat_nth(const char *flt,int n,char *out,int cap){
    int count=0,ls=0;
    for(int i=0;;i++){
        char c=s_ha_cat[i];
        if(c=='\n'||c==0){
            int len=i-ls; if(len>0 && len<cap){
                char line[256]; int k=0; for(int j=ls;j<i&&k<255;j++) line[k++]=s_ha_cat[j]; line[k]=0;
                if(ha_ci_has(line,flt)){ if(count==n){ int m=0; for(;line[m]&&m<cap-1;m++) out[m]=line[m]; out[m]=0; return 1; } count++; }
            }
            ls=i+1; if(c==0) break;
        }
    }
    return 0;
}
static void ha_load_catalog(void){
    s_ha_cat_len=0; s_ha_cat[0]=0;
    int fd=sys_open("/HALIST.TXT",0); if(fd<0) return;
    int got=0;
    while(got<(int)sizeof(s_ha_cat)-1){ long n=sys_read(fd,s_ha_cat+got,sizeof(s_ha_cat)-1-got); if(n<=0) break; got+=(int)n; }
    s_ha_cat[got]=0; s_ha_cat_len=got; sys_close(fd);
}
// Ask the haservice to (re)generate the catalog.
static void ha_request_catalog(void){ int fd=sys_open("/HALIST.REQ",0x0001|0x0040); if(fd>=0){ sys_write(fd,"1\n",2); sys_close(fd);} }
// Persist the chosen entity for slot 0 so the service caches it. O_TRUNC (not
// just O_CREAT) so a PRIOR longer /HAENT.TXT (e.g. leftover multi-entity
// lines past this write's padding) can never survive underneath the new
// single-entity selection - haservice reads up to 8 lines from this file and
// refreshes a slot per line, so stale trailing entities would otherwise keep
// being refreshed even after the user picked a single entity here.
static void ha_write_entity(const char *eid){
    int fd=sys_open("/HAENT.TXT",0x0001|0x0040|0x0200); if(fd<0) return;
    char out[128]; int i=0; for(;eid[i]&&i<95;i++) out[i]=eid[i]; out[i++]='\n';
    sys_write(fd,out,i); sys_close(fd);
    int n=0; for(;eid[n]&&n<95;n++) s_ha_entity[n]=eid[n]; s_ha_entity[n]=0;
}
// #419 Load the custom display label from /HALABEL.TXT (blank => use HA name).
static void ha_load_label(void){
    int fd=sys_open("/HALABEL.TXT",0); if(fd<0){ g_ha_label[0]=0; return; }
    char b[80]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0){ g_ha_label[0]=0; return; }
    b[n]=0; int i=0; for(;b[i]&&b[i]!='\n'&&b[i]!='\r'&&i<63;i++) g_ha_label[i]=b[i]; g_ha_label[i]=0;
    if(g_ha_label[0]==' ') g_ha_label[0]=0;                // padded/blank => none
}
static void ha_write_label(const char *lbl){
    int fd=sys_open("/HALABEL.TXT",0x0001|0x0040); if(fd<0) return;
    char out[80]; int i=0; for(;lbl[i]&&i<63;i++) out[i]=lbl[i]; out[i++]='\n';
    while(i<72) out[i++]=' '; out[i++]='\n';               // pad over any old longer label
    sys_write(fd,out,i); sys_close(fd);
    int n=0; for(;lbl[n]&&n<63;n++) g_ha_label[n]=lbl[n]; g_ha_label[n]=0;
}
// #419 Load the sparkline series (/HAHIST0.TXT: line1 entity_id, line2 ints 0..1000).
static void ha_load_spark(void){
    s_ha_spark_n=0;
    int fd=sys_open("/HAHIST0.TXT",0); if(fd<0) return;
    char b[640]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0) return; b[n]=0;
    int i=0; char hid[96]; int k=0; for(;b[i]&&b[i]!='\n'&&k<95;i++) hid[k++]=b[i]; hid[k]=0;
    if(b[i]=='\n') i++;
    // Ignore a series cached for a different entity than the one we display.
    if(s_ha_entity[0]){ int j=0; for(;hid[j]&&s_ha_entity[j];j++){ if(hid[j]!=s_ha_entity[j]) return; }
                        if(hid[j]!=s_ha_entity[j]) return; }
    while(b[i] && s_ha_spark_n<64){
        while(b[i]==' '||b[i]=='\n'||b[i]=='\r') i++;
        if(b[i]<'0'||b[i]>'9') break;
        int v=0; while(b[i]>='0'&&b[i]<='9'){ v=v*10+(b[i]-'0'); i++; }
        if(v>1000)v=1000; s_ha_spark[s_ha_spark_n++]=v;
    }
}
// #419 Parse the leading numeric part of the state into value*10 (one decimal).
// Returns 1 if the state is numeric.
static int ha_state_num(long *out){
    const char *s=s_ha_state; int i=0,neg=0; if(s[0]=='-'){neg=1;i=1;}
    if(!(s[i]>='0'&&s[i]<='9')) return 0;
    long v=0; for(;s[i]>='0'&&s[i]<='9';i++) v=v*10+(s[i]-'0');
    long d=0; if(s[i]=='.'&&s[i+1]>='0'&&s[i+1]<='9') d=s[i+1]-'0';
    long m=v*10+d; *out = neg?-m:m; return 1;
}
// #419 classify a non-numeric state as "active" (green) vs inactive (gray).
static int ha_state_active(void){
    const char *s=s_ha_state;
    if(ha_ci_has(s,"on")||ha_ci_has(s,"open")||ha_ci_has(s,"home")||ha_ci_has(s,"playing")
       ||ha_ci_has(s,"active")||ha_ci_has(s,"heat")||ha_ci_has(s,"cool")||ha_ci_has(s,"locked")) return 1;
    return 0;
}
// #419 mini sparkline (line chart) of the cached series in box (x,y,w,h).
static void ha_draw_spark(int x,int y,int w,int h){
    if(s_ha_spark_n<2) return;
    draw_fill_rect(x,y,w,h,0x00161E26);
    draw_rect_outline(x,y,w,h,0x00304556);
    int n=s_ha_spark_n, px=-1,py=-1;
    for(int i=0;i<n;i++){
        int vx = x+1 + i*(w-3)/(n-1);
        int vy = y+1 + (h-3) - s_ha_spark[i]*(h-3)/1000;
        if(px>=0) wdg_line(px,py,vx,vy,0x0041B0E0);
        px=vx; py=vy;
    }
}
static void ha_card_draw(int x,int y){
    ha_refresh_cache();
    if((s_ha_io_tick++ % 24)==0){ ha_load_label(); ha_load_spark(); }
    int W=HA_W;
    int have_spark = (s_ha_spark_n>=2 && g_ha_mode!=3);
    int top = 10 + FONT_CHAR_H + 8;                       // below the title
    int valh = (g_ha_mode==1)? (FONT_CHAR_H*3+6) : (g_ha_mode==2)? 30 : (g_ha_mode==3)? 30 : (FONT_CHAR_H*2+6);
    int sparkh = have_spark ? 26 : 0;
    int H = top + valh + (sparkh?sparkh+6:0) + 8 + FONT_CHAR_H + 8;  s_ha_h=H;
    draw_fill_rect(x,y,W,H,0x00202832);
    draw_rect_outline(x,y,W,H,0x00415A6E);
    draw_fill_rect(x,y,W,4,0x0041B0E0);                   // HA accent
    // Title: custom label overrides HA friendly name (#419 rename).
    const char *title = g_ha_label[0]?g_ha_label:(s_ha_friendly[0]?s_ha_friendly:(s_ha_entity[0]?s_ha_entity:"Home Assistant"));
    char tt[34]; int i=0; for(;title[i]&&i<32;i++) tt[i]=title[i]; tt[i]=0;
    draw_text(x+12,y+10,tt,0x00CFE8F5);
    int vy = y+top;
    long num; int is_num = ha_state_num(&num);
    if(g_ha_mode==2){                                     // ---- state badge ----
        int act = is_num ? -1 : ha_state_active();
        uint32_t bg = (act==1)?0x001E7A3C : (act==0)?0x00444C55 : 0x00274A63;
        char bt[40]; int bi=0; const char *bs=s_ha_state[0]?s_ha_state:"...";
        for(;bs[bi]&&bi<24;bi++) bt[bi]=bs[bi]; bt[bi]=0;
        int bw = text_width(bt)+24; if(bw<64)bw=64; if(bw>W-24)bw=W-24;
        draw_fill_rect(x+12,vy,bw,24,bg);
        draw_rect_outline(x+12,vy,bw,24,0x0080A0B8);
        draw_text_centered(x+12+bw/2,vy+4,bt,0x00FFFFFF);
    } else if(g_ha_mode==3){                              // ---- gauge / bar ----
        long v = is_num? num : 0;
        if(is_num && v/10 > g_ha_max){ g_ha_max = (int)(v/10); }   // auto-grow range
        long lo=(long)g_ha_min*10, hi=(long)g_ha_max*10; if(hi<=lo)hi=lo+10;
        long frac = (v-lo)*1000/(hi-lo); if(frac<0)frac=0; if(frac>1000)frac=1000;
        int gw=W-24, gh=16;
        draw_fill_rect(x+12,vy,gw,gh,0x00161E26);
        draw_fill_rect(x+12,vy,(int)((long)gw*frac/1000),gh,0x0041B0E0);
        draw_rect_outline(x+12,vy,gw,gh,0x00304556);
        char val[48]; int vi=0; if(s_ha_state[0]) for(int j=0;s_ha_state[j]&&vi<40;j++) val[vi++]=s_ha_state[j];
        if(s_ha_unit[0]){ val[vi++]=' '; for(int j=0;s_ha_unit[j]&&vi<46;j++) val[vi++]=s_ha_unit[j]; } val[vi]=0;
        draw_text(x+12,vy+gh+2,val,0x00CFE8F5);
    } else {                                              // ---- value / big ----
        char val[90]; int vi=0;
        const char *body = s_ha_state[0]?s_ha_state:"...";
        for(int j=0;body[j]&&vi<80;j++) val[vi++]=body[j];
        if(g_ha_mode!=1 && s_ha_unit[0]){ val[vi++]=' '; for(int j=0;s_ha_unit[j]&&vi<86;j++) val[vi++]=s_ha_unit[j]; }
        val[vi]=0;
        int scale = (g_ha_mode==1)?3:2;
        draw_text_large(x+12,vy,val,0x00FFFFFF,scale);
        if(g_ha_mode==1 && s_ha_unit[0]) draw_text(x+12,vy+FONT_CHAR_H*3-6,s_ha_unit,0x008FA9BD);
    }
    if(have_spark) ha_draw_spark(x+12,vy+valh+6,W-24,20);
    char eb[36]; i=0; for(;s_ha_entity[i]&&i<34;i++) eb[i]=s_ha_entity[i]; eb[i]=0;
    draw_text(x+12,y+H-FONT_CHAR_H-8,eb,0x007E93A6);
}

// Bounding box of widget `id` (top-left). Returns 0 if hidden/unplaced.
static int widget_box(int id, int *bx, int *by, int *bw, int *bh) {
    if (!g_widgets_enabled) return 0;
    if (id == 0) {
        if (!g_show_clock || g_clock_cx < 0) return 0;
        *bx = g_clock_cx - s_clk_r; *by = g_clock_cy - s_clk_r;
        *bw = s_clk_r * 2; *bh = s_clk_r * 2; return 1;
    }
    if (id == 1) {
        if (!g_show_calendar || g_cal_x < 0) return 0;
        *bx = g_cal_x; *by = g_cal_y; *bw = s_cal_w; *bh = s_cal_h; return 1;
    }
    if (id >= 2 && id <= 4) {
        int c = id - 2;
        if (!*s_card_vis[c] || *s_card_x[c] < 0) return 0;
        *bx = *s_card_x[c]; *by = *s_card_y[c]; *bw = CARD_W; *bh = s_card_h[c]; return 1;
    }
    if (id == 5) {
        if (!g_show_digclock || g_digclk_x < 0) return 0;
        *bx = g_digclk_x; *by = g_digclk_y; *bw = s_digclk_w; *bh = s_digclk_h; return 1;
    }
    if (id == 6) {
        if (!g_show_sysmon || g_sysmon_x < 0) return 0;
        *bx = g_sysmon_x; *by = g_sysmon_y; *bw = SYSMON_W; *bh = SYSMON_H; return 1;
    }
    if (id == 7) {
        if (!g_show_timer || g_timer_x < 0) return 0;
        *bx = g_timer_x; *by = g_timer_y; *bw = TIMER_W; *bh = TIMER_H; return 1;
    }
    if (id == 8) {
        if (!g_show_worldtime || g_worldtime_x < 0) return 0;
        *bx = g_worldtime_x; *by = g_worldtime_y; *bw = WT_W; *bh = WT_H; return 1;
    }
    if (id == 9) {
        if (!g_show_uptime || g_uptime_x < 0) return 0;
        *bx = g_uptime_x; *by = g_uptime_y; *bw = UPT_W; *bh = UPT_H; return 1;
    }
    if (id == 10) {
        if (!g_show_ha || g_ha_x < 0) return 0;
        *bx = g_ha_x; *by = g_ha_y; *bw = HA_W; *bh = s_ha_h; return 1;
    }
    return 0;
}
static int *widget_lock_ptr(int id) {
    if (id == 0) return &g_clock_locked;
    if (id == 1) return &g_cal_locked;
    if (id >= 2 && id <= 4) return s_card_lock[id - 2];
    if (id == 5) return &g_digclk_locked;
    if (id == 6) return &g_sysmon_locked;
    if (id == 7) return &g_timer_locked;
    if (id == 8) return &g_worldtime_locked;
    if (id == 9) return &g_uptime_locked;
    if (id == 10) return &g_ha_locked;
    return 0;
}
static int *widget_vis_ptr(int id) {
    if (id == 0) return &g_show_clock;
    if (id == 1) return &g_show_calendar;
    if (id >= 2 && id <= 4) return s_card_vis[id - 2];
    if (id == 5) return &g_show_digclock;
    if (id == 6) return &g_show_sysmon;
    if (id == 7) return &g_show_timer;
    if (id == 8) return &g_show_worldtime;
    if (id == 9) return &g_show_uptime;
    if (id == 10) return &g_show_ha;
    return 0;
}

int widget_hit(int x, int y) {
    // Topmost first (cards over calendar over clock on overlap).
    for (int id = 10; id >= 0; id--) {
        int bx, by, bw, bh;
        if (!widget_box(id, &bx, &by, &bw, &bh)) continue;
        if (id == 0) {
            int dx = x - g_clock_cx, dy = y - g_clock_cy;
            if (dx*dx + dy*dy <= s_clk_r * s_clk_r) return 0;
        } else if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            return id;
        }
    }
    return -1;
}
void widget_grab(int x, int y) {
    g_wdrag = widget_hit(x, y);
    if (g_wdrag < 0) return;
    // Timer/Stopwatch: a click on one of its buttons acts on the timer and does
    // NOT begin a drag (so the user can press Start/Reset/Mode without moving it).
    if (g_wdrag == 7 && timer_click(x, y)) { g_wdrag = -1; return; }
    int *lk = widget_lock_ptr(g_wdrag);
    if (lk && *lk) { g_wdrag = -1; return; }            // locked: no drag
    int bx, by, bw, bh; widget_box(g_wdrag, &bx, &by, &bw, &bh);
    g_wdx = x - bx; g_wdy = y - by;
}
void widget_drag_to(int x, int y) {
    if (g_wdrag < 0) return;
    int bx, by, bw, bh; if (!widget_box(g_wdrag, &bx, &by, &bw, &bh)) return;
    int nx = x - g_wdx, ny = y - g_wdy;                 // new top-left
    if (nx < 0) nx = 0;
    if (ny < 24) ny = 24;
    if (nx > g_fb_width - bw) nx = g_fb_width - bw;
    if (ny > g_fb_height - bh - 30) ny = g_fb_height - bh - 30;
    if (g_wdrag == 0)      { g_clock_cx = nx + s_clk_r; g_clock_cy = ny + s_clk_r; }
    else if (g_wdrag == 1) { g_cal_x = nx; g_cal_y = ny; }
    else if (g_wdrag == 5) { g_digclk_x = nx; g_digclk_y = ny; }
    else if (g_wdrag == 6) { g_sysmon_x = nx; g_sysmon_y = ny; }
    else if (g_wdrag == 7) { g_timer_x = nx; g_timer_y = ny; }
    else if (g_wdrag == 8) { g_worldtime_x = nx; g_worldtime_y = ny; }
    else if (g_wdrag == 9) { g_uptime_x = nx; g_uptime_y = ny; }
    else if (g_wdrag == 10) { g_ha_x = nx; g_ha_y = ny; }
    else                   { int c = g_wdrag - 2; *s_card_x[c] = nx; *s_card_y[c] = ny; }
}
void widget_release(void) { g_wdrag = -1; }

// --- Per-widget right-click menu (Hide / Lock|Unlock / [Settings]) ---------
void widget_menu_open(int which, int x, int y) { g_wmenu = which; g_wmenu_x = x; g_wmenu_y = y; }
int  widget_menu_is_open(void) { return g_wmenu >= 0; }
static int widget_menu_nitems(void) {
    if (g_wmenu == 5) return 5;          // digital clock: Hide, Lock, 12/24h, Seconds, Design
    if (g_wmenu >= 2 && g_wmenu <= 4) return 3;  // info cards: Hide, Lock, Settings
    if (g_wmenu == 10) return 4;                 // #414/#419 HA: Hide, Lock, Display, Settings
    return 2;                            // all other widgets: Hide, Lock
}

static void widget_menu_geom(int *mx, int *my, int *h) {
    int hh = widget_menu_nitems() * WMENU_IH + 4;
    int x = g_wmenu_x, y = g_wmenu_y;
    if (x + WMENU_W > g_fb_width) x = g_fb_width - WMENU_W;
    if (y + hh > g_fb_height) y = g_fb_height - hh;
    *mx = x; *my = y; *h = hh;
}
void widget_menu_render(void) {
    if (g_wmenu < 0) return;
    int x, y, h; widget_menu_geom(&x, &y, &h);
    int n = widget_menu_nitems();
    draw_fill_rect(x, y, WMENU_W, h, CLR_MENU_BG);
    draw_rect_outline(x, y, WMENU_W, h, CLR_MENU_BORDER);
    int *lk = widget_lock_ptr(g_wmenu);
    static const char *ha_mode_names[4] = { "Value", "Big", "Badge", "Gauge" };
    const char *items[5];
    items[0] = "Hide";
    items[1] = (lk && *lk) ? "Unlock" : "Lock";
    if (g_wmenu == 5) {
        items[2] = g_digclk_12h ? "24-hour" : "12-hour";
        items[3] = g_digclk_secs ? "Hide seconds" : "Show seconds";
        items[4] = "Next design";
    } else if (g_wmenu == 10) {
        static char mbuf[24];
        const char *mn = ha_mode_names[g_ha_mode & 3];
        int q = 0; const char *pfx = "Mode: ";
        for (int j = 0; pfx[j] && q < 20; j++) mbuf[q++] = pfx[j];
        for (int j = 0; mn[j] && q < 23; j++) mbuf[q++] = mn[j];
        mbuf[q] = 0;
        items[2] = mbuf;
        items[3] = "Settings...";
    } else {
        items[2] = "Settings";
    }
    for (int i = 0; i < n; i++)
        draw_text(x + 12, y + 4 + i * WMENU_IH + 5, items[i], CLR_MENU_TEXT);
}
int widget_menu_handle(int x, int y, int click) {
    if (g_wmenu < 0) return 0;
    if (!click) return 1;
    int mx, my, h; widget_menu_geom(&mx, &my, &h);
    int n = widget_menu_nitems();
    if (x >= mx && x < mx + WMENU_W && y >= my + 4 && y < my + 4 + n * WMENU_IH) {
        int idx = (y - (my + 4)) / WMENU_IH;
        int *vis = widget_vis_ptr(g_wmenu);
        int *lk  = widget_lock_ptr(g_wmenu);
        int which = g_wmenu;
        if (idx == 0)      { if (vis) *vis = 0; }                 // Hide
        else if (idx == 1) { if (lk) *lk = !*lk; }               // Lock/Unlock
        else if (which == 5 && idx == 2) { g_digclk_12h = !g_digclk_12h; }    // 12/24h
        else if (which == 5 && idx == 3) { g_digclk_secs = !g_digclk_secs; }  // seconds
        else if (which == 5 && idx == 4) { g_digclk_style = (g_digclk_style + 1) % 5; } // design
        else if (which == 10 && idx == 2) { g_ha_mode = (g_ha_mode + 1) & 3; }   // #419 cycle display mode
        else if (which == 10 && idx == 3) { g_wmenu = -1; widget_settings_open(10); return 1; }
        else if (idx == 2 && which >= 2 && which <= 4) { g_wmenu = -1; widget_settings_open(which); return 1; }
    }
    g_wmenu = -1;
    return 1;
}
int  widget_is_dragging(void) { return g_wdrag >= 0; }

// ===========================================================================
// #81-83: internet info widgets (weather / crypto / stock ticker)
//
// The slow network fetch is done by the background `netinfo` service, which
// writes short result files. These widgets just read the cached files every
// few seconds and draw a row of info cards along the top of the desktop.
// ===========================================================================
int g_show_weather = 0, g_show_crypto = 0, g_show_stocks = 0;
static char s_weather[256], s_crypto[256], s_stocks[256];
static int  s_netinfo_tick = 0;

// Validate that a quote/weather payload looks like real card data (CODE,price..
// or loc|cond|...) and NOT some other file's content (e.g. the heartbeat log
// /SVCLOG.TXT, which a stale/short FAT read could surface). A real payload has
// no spaces inside its first token and uses ',' or '|' as field separators.
// #303: this stops "MayteraOS heartbeat service tick=N" from rendering as a
// bogus stock/crypto ticker when the data file is briefly unreadable.
static int netinfo_payload_ok(const char *s) {
    if (!s || !s[0]) return 0;
    // First token (up to first separator) must be short and space-free.
    int i = 0;
    for (; s[i] && s[i] != ',' && s[i] != '|'; i++) {
        if (s[i] == ' ') return 0;            // ticker codes never contain spaces
        if (i >= 16) return 0;                // codes/locations are short
    }
    return 1;
}

static void netinfo_read(const char *path, char *dst, int cap) {
    // Always start from a known-clean buffer so a failed read can never leave
    // stale or uninitialized bytes on screen (#303).
    dst[0] = '\0';
    int fd = sys_open(path, 0);
    if (fd < 0) return;                 // file missing -> leave "" (shows "...")
    char buf[256];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int i = 0;
    char tmp[256];
    for (; i < (int)sizeof(tmp) - 1 && buf[i] && buf[i] != '\n' && buf[i] != '\r'; i++) tmp[i] = buf[i];
    while (i > 0 && tmp[i - 1] == ' ') i--;   // trim trailing pad
    tmp[i] = '\0';
    if (!netinfo_payload_ok(tmp)) return;     // reject garbage -> leave ""
    // Commit validated payload to dst.
    int j = 0;
    for (; j < cap - 1 && tmp[j]; j++) dst[j] = tmp[j];
    dst[j] = '\0';
}
static void netinfo_refresh(void) {
    netinfo_read("/WEATHER.TXT", s_weather, sizeof(s_weather));
    netinfo_read("/CRYPTO.TXT",  s_crypto,  sizeof(s_crypto));
    netinfo_read("/STOCKS.TXT",  s_stocks,  sizeof(s_stocks));
}

// --- small drawing + parsing helpers --------------------------------------
static void sb(char *d, int *di, int cap, const char *s) { for (int i = 0; s[i] && *di < cap - 1; i++) d[(*di)++] = s[i]; d[*di] = '\0'; }
// Split s on delim into out[maxf][fcap]; returns field count.
static int wsplit(const char *s, char delim, char out[][48], int maxf, int fcap) {
    int fi = 0, ci = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == delim || c == '\0') {
            if (fi < maxf) { out[fi][ci] = '\0'; fi++; }
            ci = 0;
            if (c == '\0' || fi >= maxf) break;
        } else if (ci < fcap - 1) out[fi][ci++] = c;
    }
    return fi;
}
// 9px-wide up/down arrow centered at cx, top at y (5 rows).
static void warrow(int cx, int y, int up, uint32_t col) {
    for (int r = 0; r < 5; r++) {
        int wdt = up ? (2 * r + 1) : (2 * (4 - r) + 1);
        draw_fill_rect(cx - wdt / 2, y + r, wdt, 1, col);
    }
}
// All offsets/radii are multiplied by scale s so the icon can be drawn larger.
static void wx_cloud(int x, int y, uint32_t c, int s) {
    draw_circle_filled(x + 7*s,  y + 12*s, 6*s, c);
    draw_circle_filled(x + 14*s, y + 10*s, 7*s, c);
    draw_circle_filled(x + 20*s, y + 13*s, 5*s, c);
    draw_fill_rect(x + 7*s, y + 13*s, 14*s, 5*s, c);
}
// One sun ray from radius ri to ro along unit dir (dx,dy in /1024 fixed point),
// drawn `thick` parallel pixels wide (perpendicular to the ray).
static int wx_sgn(int v) { return v > 512 ? 1 : (v < -512 ? -1 : 0); }
static void wx_ray(int cx, int cy, int dx, int dy, int ri, int ro, int thick, uint32_t c) {
    int ax = cx + dx * ri / 1024, ay = cy + dy * ri / 1024;
    int bx = cx + dx * ro / 1024, by = cy + dy * ro / 1024;
    int ox = wx_sgn(-dy), oy = wx_sgn(dx);          // perpendicular step (1px)
    for (int t = 0; t < thick; t++)
        wdg_line(ax + ox * t, ay + oy * t, bx + ox * t, by + oy * t, c);
}

// A teardrop-shaped raindrop: round bulb at the bottom, tapered point on top.
static void wx_drop(int cx, int cy, int s, uint32_t c) {
    int r = (3 * s) / 2; if (r < 1) r = 1;
    draw_circle_filled(cx, cy, r, c);                 // round bottom
    for (int i = 0; i < 3 * s; i++) {                 // pointed top
        int w = 3 * s - i; if (w < 1) w = 1;
        draw_fill_rect(cx - w / 2, cy - r - i, w, 1, c);
    }
}
// One thick segment of a lightning bolt.
static void wx_bolt_seg(int x0, int y0, int x1, int y1, int s, uint32_t c) {
    for (int t = 0; t < s; t++) wdg_line(x0 + t, y0, x1 + t, y1, c);
}
// A small jagged Z lightning bolt with its top at (bx,by).
static void wx_bolt(int bx, int by, int s, uint32_t c) {
    wx_bolt_seg(bx + s, by,         bx - s, by + 2*s, s, c);
    wx_bolt_seg(bx - s, by + 2*s,   bx + s, by + 2*s, s, c);
    wx_bolt_seg(bx + s, by + 2*s,   bx - s, by + 5*s, s, c);
}

static void wx_icon(int x, int y, int idx, int s) {
    // One consistent light grey for every cloud. Draw the whole icon opaque so
    // overlapping cloud circles don't double-blend (which produced patchy greys
    // when the widget honors global window transparency).
    int saved_blend = g_draw_blend; g_draw_blend = 255;
    uint32_t sun = 0x00FFD040, cl = 0x00CAD2DE, rn = 0x0064B0FF, sw = 0x00FFFFFF, bo = 0x00FFE000;
    int cx = x + 12*s, cy = y + 10*s;
    if (idx == 0) {                                   // clear / sun
        // Core disc + 8 evenly-spaced thin rays at a uniform gap from the disc.
        int cr = 5 * s;                               // core radius
        int ri = cr + 2 * s;                          // ray start (even gap all round)
        int ro = ri + 4 * s;                          // ray end
        static const int rd[8][2] = {
            {0,-1024}, {724,-724}, {1024,0}, {724,724},
            {0,1024}, {-724,724}, {-1024,0}, {-724,-724}
        };
        draw_circle_filled(cx, cy, cr, sun);
        for (int k = 0; k < 8; k++)
            wx_ray(cx, cy, rd[k][0], rd[k][1], ri, ro, s, sun);
    } else if (idx == 1) { draw_circle_filled(x + 8*s, y + 6*s, 5*s, sun); wx_cloud(x, y + 3*s, cl, s); }
    else if (idx == 3)   {                            // fog: light grey cloud + fog lines
        wx_cloud(x, y, cl, s);
        for (int i = 0; i < 3; i++) draw_fill_rect(x + 6*s, y + (21 + i*2)*s, 17*s, s, 0x00A8B0BC);
    }
    else if (idx == 4)   {                            // rain: teardrops only, no cloud
        wx_drop(x + 8*s,  y + 8*s,  s, rn);
        wx_drop(x + 16*s, y + 6*s,  s, rn);
        wx_drop(x + 22*s, y + 10*s, s, rn);
        wx_drop(x + 11*s, y + 18*s, s, rn);
        wx_drop(x + 19*s, y + 18*s, s, rn);
    }
    else if (idx == 5)   {                            // snow: cloud + 3 staggered rows of dots
        wx_cloud(x, y, cl, s);
        for (int r = 0; r < 3; r++)
            for (int i = 0; i < 4; i++)
                draw_circle_filled(x + (7 + i*5 + (r & 1) * 2) * s, y + (20 + r*3) * s, s, sw);
    }
    else if (idx == 6)   {                            // thunder: cloud + 3 bolts (big centre)
        wx_cloud(x, y, cl, s);
        wx_bolt(x + 7*s,  y + 19*s, s, bo);           // left (small)
        wx_bolt(cx,       y + 16*s, s + 1, bo);       // centre: larger, from cloud base
        wx_bolt(x + 17*s, y + 19*s, s, bo);           // right (small)
    }
    else                 { wx_cloud(x, y, cl, s); }   // cloudy (2) / default
    g_draw_blend = saved_blend;                        // restore caller's blend
}
// Currency symbol: dollar-family -> "$", otherwise "" (currency shown in title).
static const char *cursym(const char *cur) {
    static const char *d[] = { "USD","AUD","CAD","NZD","SGD","HKD","MXN","BRL" };
    for (int i = 0; i < 8; i++) { int j = 0; while (d[i][j] && cur[j] && d[i][j] == cur[j]) j++; if (!d[i][j] && !cur[j]) return "$"; }
    return "";
}

static int draw_weather_card(int x, int y, int w) {
    char f[9][48]; int nf = wsplit(s_weather, '|', f, 9, 48);
    int verbose = g_weather_verbose;
    int h = (verbose && nf >= 9) ? 118 : 64;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, 4, h, 2, 0x0066C0FF);
    if (nf < 3) { draw_text(x + 12, y + 8, "WEATHER", readable_accent(0x0066C0FF, CLR_MENU_BG)); draw_text(x + 12, y + 32, "...", CLR_MENU_TEXT); return h; }
    int icon = f[2][0] ? (f[2][0] - '0') : 2;
    int isc = 2;                                                // 2x-size icon
    // Drop the icon down so it lines up with the Min/Max row (verbose) instead
    // of colliding with the location text on the top line.
    int icy = (verbose && nf >= 9) ? y + 44 : y + 24;
    wx_icon(x + w - 24*isc - 12, icy, icon, isc);
    draw_text(x + 12, y + 8, f[0], readable_accent(0x0066C0FF, CLR_MENU_BG)); // location
    char l[64]; int li = 0;
    sb(l, &li, sizeof(l), f[1]); sb(l, &li, sizeof(l), "  ");
    sb(l, &li, sizeof(l), f[3]); sb(l, &li, sizeof(l), "\xB0" "C");    // condition + now temp
    draw_text(x + 12, y + 30, l, CLR_MENU_TEXT);
    if (verbose && nf >= 9) {
        li = 0; sb(l, &li, sizeof(l), "Min "); sb(l, &li, sizeof(l), f[4]); sb(l, &li, sizeof(l), "\xB0" "C   Max ");
        sb(l, &li, sizeof(l), f[5]); sb(l, &li, sizeof(l), "\xB0" "C");
        draw_text(x + 12, y + 52, l, readable_ink_dim(CLR_MENU_BG));
        li = 0; sb(l, &li, sizeof(l), "Rain "); sb(l, &li, sizeof(l), f[7]); sb(l, &li, sizeof(l), "%  ");
        sb(l, &li, sizeof(l), f[8]); sb(l, &li, sizeof(l), "mm");
        draw_text(x + 12, y + 72, l, readable_ink_dim(CLR_MENU_BG));
        li = 0; sb(l, &li, sizeof(l), "Humidity "); sb(l, &li, sizeof(l), f[6]); sb(l, &li, sizeof(l), "%");
        draw_text(x + 12, y + 92, l, readable_ink_dim(CLR_MENU_BG));
    }
    return h;
}

// Shared renderer for crypto/stocks (per-item line: CODE  $price  ^chg%).
static int draw_quote_card(int x, int y, int w, const char *raw, unsigned int accent,
                           const char *title_base, const char *cur, int verbose) {
    char f[12][48]; int nf = wsplit(raw, '|', f, 12, 48);
    int first = cur ? 1 : 0;                 // crypto: f[0] is the currency
    int nq = nf - first; if (nq < 0) nq = 0;
    int rows = nq > 0 ? nq : 1;
    int h = 30 + rows * 18 + 6;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, 4, h, 2, accent);
    char title[40]; int ti = 0; sb(title, &ti, sizeof(title), title_base);
    if (cur && f[0][0]) { sb(title, &ti, sizeof(title), "  "); sb(title, &ti, sizeof(title), f[0]); }
    draw_text(x + 12, y + 8, title, readable_accent(accent, CLR_MENU_BG));
    if (nq == 0) { draw_text(x + 12, y + 30, "unavailable", readable_ink_dim(CLR_MENU_BG)); return h; }
    const char *sym = cur ? cursym(f[0]) : "$";       // crypto: symbol from the file's currency
    for (int i = 0; i < nq; i++) {
        char q[3][48]; int qn = wsplit(f[first + i], ',', q, 3, 48);   // code, price, chg
        int yy = y + 30 + i * 18;
        // #303: only render a row that parsed into the expected code,price[,chg]
        // shape with a clean (space-free, short) ticker code; otherwise show a
        // placeholder rather than echoing whatever bytes ended up in the buffer.
        if (qn < 2 || !netinfo_payload_ok(q[0])) {
            draw_text(x + 12, yy, "unavailable", readable_ink_dim(CLR_MENU_BG));
            continue;
        }
        draw_text(x + 12, yy, q[0], CLR_MENU_TEXT);            // code
        char pr[40]; int pi = 0; sb(pr, &pi, sizeof(pr), sym); sb(pr, &pi, sizeof(pr), q[1]);
        draw_text(x + 64, yy, pr, CLR_MENU_TEXT);              // $price
        if (verbose && q[2][0]) {                             // change arrow + %
            int up = (q[2][0] != '-');
            uint32_t col = up ? 0x0050E070 : 0x00FF6060;
            warrow(x + w - 70, yy + 4, up, col);
            char cg[24]; int ci = 0;
            sb(cg, &ci, sizeof(cg), (q[2][0] == '-') ? q[2] + 1 : q[2]); sb(cg, &ci, sizeof(cg), "%");
            draw_text(x + w - 60, yy, cg, col);
        }
    }
    return h;
}

// Test-only: draw a labeled gallery of all weather icons (gated by /WXTEST.TXT)
// so the icon set can be eyeballed without waiting on real conditions.
static int g_wxtest = -1;
static void draw_icon_gallery(void) {
    static const char *names[7] = { "Clear", "Partly Cloudy", "Cloudy", "Fog", "Rain", "Snow", "Thunder" };
    int x = 60, y = 80, w = 210, rowh = 56;
    int h = 7 * rowh + 36;
    draw_rounded_rect(x, y, w, h, 8, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + 12, y + 8, "Weather Icons (test)", 0x0066C0FF);
    for (int i = 0; i < 7; i++) {
        int ry = y + 30 + i * rowh;
        wx_icon(x + 18, ry, i, 2);
        char lbl[24]; int li = 0;
        lbl[li++] = '0' + i; lbl[li++] = ':'; lbl[li++] = ' ';
        for (int j = 0; names[i][j] && li < 22; j++) lbl[li++] = names[i][j];
        lbl[li] = 0;
        draw_text(x + 78, ry + 16, lbl, CLR_MENU_TEXT);
    }
}

static void netinfo_render(void) {
    // Draw-only (idle per-rect) mode never advances the refresh tick; the idle
    // path refreshes + damages the cards once via netinfo_collect_damage().
    if (!g_widgets_draw_only && (s_netinfo_tick++ % 150 == 0)) netinfo_refresh();
    int w = CARD_W, gap = 12;
    int startx = (g_fb_width - (3 * w + 2 * gap)) / 2, dy = 14;
    for (int i = 0; i < 3; i++) {
        if (!*s_card_vis[i]) continue;
        if (*s_card_x[i] < 0) { *s_card_x[i] = startx + i * (w + gap); *s_card_y[i] = dy; }
        int hx = *s_card_x[i], hy = *s_card_y[i];
        if (i == 0)      s_card_h[0] = draw_weather_card(hx, hy, w);
        else if (i == 1) s_card_h[1] = draw_quote_card(hx, hy, w, s_crypto, 0x00FFC850, "CRYPTO", "USD", g_crypto_verbose);
        else             s_card_h[2] = draw_quote_card(hx, hy, w, s_stocks, 0x0066FF99, "STOCKS", 0, g_stocks_verbose);
    }
}

// ===========================================================================
// #81-83: per-widget Settings dialog (configure weather location / symbols).
// Editable text field (keyboard input) that saves to a small config file the
// netinfo service reads. Opened from the widget right-click menu -> Settings.
// ===========================================================================
static int  g_wsettings = -1;          // widget id being configured, -1 = closed
static char g_wset_buf[2][64];         // field 0 + (crypto) field 1
static int  g_wset_len[2] = { 0, 0 };
static int  g_wset_focus = 0;
#define WSET_W 400
#define WXCUR_PATH "/CRYPTOCUR.TXT"

// #419b: draggable settings modal. The modal opens centred; g_wset_off_x/y is a
// live offset from that centre applied by every geom(). Dragging the 26px title
// bar moves it (same title-bar-drag pattern as the desktop widgets/windows).
// g_wset_drag: 0 = none, 1 = moving the modal, 2 = dragging the HA list scrollbar.
static int  g_wset_off_x = 0, g_wset_off_y = 0;
static int  g_wset_drag = 0, g_wset_grab_dx = 0, g_wset_grab_dy = 0;
// Keep the title bar reachable: clamp so a margin of the modal + its full 26px
// header stay on-screen no matter how far the user drags.
static void modal_clamp(int *x, int *y, int w, int h) {
    int margin = 72;
    if (*x > g_fb_width - margin)  *x = g_fb_width - margin;
    if (*x < margin - w)           *x = margin - w;
    if (*y < 0)                    *y = 0;
    if (*y > g_fb_height - 30)      *y = g_fb_height - 30;
    (void)h;
}

static int  wset_nfields(int id) { return id == 3 ? 2 : 1; }       // crypto has currency too
static int *wset_verbose_ptr(int id) { return s_card_verbose[id - 2]; }   // 2/3/4 -> 0/1/2
static int  wset_h(void)         { return (wset_nfields(g_wsettings) == 2 ? 214 : 156) + 34; }
static void wset_geom(int *x, int *y) {
    *x = (g_fb_width - WSET_W) / 2 + g_wset_off_x;
    *y = (g_fb_height - wset_h()) / 2 + g_wset_off_y;
    modal_clamp(x, y, WSET_W, wset_h());
}
// Verbose checkbox box, just above the buttons.
static void wset_chk_box(int *cx, int *cy) {
    int x, y; wset_geom(&x, &y);
    *cx = x + 14; *cy = y + wset_h() - 36 - 30;
}

static const char *wcfg_path0(int id)  { return id==2 ? "/WXLOC.TXT" : id==3 ? "/CRYPTOID.TXT" : "/STOCKID.TXT"; }
static const char *wcfg_def0(int id)   { return id==2 ? "London" : id==3 ? "BTC,ETH" : "AAPL,MSFT"; }
static const char *wcfg_title(int id)  { return id==2 ? "Weather Settings" : id==3 ? "Crypto Settings" : "Stock Settings"; }
static const char *wcfg_lbl0(int id)   { return id==2 ? "Location" : id==3 ? "Coins (short codes)" : "Tickers"; }
static const char *wcfg_hint0(int id)  { return id==2 ? "City,Country  e.g. Perth,AU  or  London"
                                              : id==3 ? "e.g. BTC,ETH,USDT,SOL,XRP"
                                                      : "e.g. AAPL,MSFT,GOOG"; }

static void wset_load(int f, const char *path, const char *def) {
    g_wset_buf[f][0] = '\0'; g_wset_len[f] = 0;
    int fd = sys_open(path, 0);
    if (fd >= 0) {
        char b[128]; long n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
        if (n > 0) {
            int i = 0;
            for (; i < (int)sizeof(g_wset_buf[f]) - 1 && i < n && b[i] && b[i] != '\n' && b[i] != '\r'; i++)
                g_wset_buf[f][i] = b[i];
            while (i > 0 && g_wset_buf[f][i-1] == ' ') i--;
            g_wset_buf[f][i] = '\0'; g_wset_len[f] = i;
        }
    }
    if (g_wset_len[f] == 0) {
        int i = 0; for (; def[i] && i < (int)sizeof(g_wset_buf[f]) - 1; i++) g_wset_buf[f][i] = def[i];
        g_wset_buf[f][i] = '\0'; g_wset_len[f] = i;
    }
}

// ---- #414/#419 HA entity picker + rename + display mode (widget_settings id 10)
#define HAP_W 480
#define HAP_H 468
#define HAP_SB_W 12                 // #419b entity-list scrollbar width
static int ha_pick_scroll = 0;
static const char *HA_MODE_LBL[4] = { "Value", "Big", "Badge", "Gauge" };
static void hap_geom(int *x,int *y){
    *x=(g_fb_width-HAP_W)/2 + g_wset_off_x;
    *y=(g_fb_height-HAP_H)/2 + g_wset_off_y;
    modal_clamp(x,y,HAP_W,HAP_H);
}
// #419b count catalog lines matching the current filter (for scroll bounds).
static int ha_cat_count(const char *flt){
    int count=0,ls=0;
    for(int i=0;;i++){
        char c=s_ha_cat[i];
        if(c=='\n'||c==0){
            int len=i-ls;
            if(len>0){
                char line[256]; int k=0; for(int j=ls;j<i&&k<255;j++) line[k++]=s_ha_cat[j]; line[k]=0;
                if(ha_ci_has(line,flt)) count++;
            }
            ls=i+1; if(c==0) break;
        }
    }
    return count;
}
// shared layout offsets (render + click must agree)
#define HAP_RENAME_DY 48
#define HAP_MODE_DY   100
#define HAP_SEARCH_DY 152
#define HAP_LIST_DY   184
static void ha_chip_box(int x,int y,int i,int *cx,int *cy,int *cw,int *ch){
    int fw=HAP_W-28, gap=6, w=(fw-3*gap)/4;
    *cx=x+14+i*(w+gap); *cy=y+HAP_MODE_DY; *cw=w; *ch=26;
}
static int hap_rows(void){ int rowh=20; return (HAP_H-HAP_LIST_DY-46)/rowh; }
// #414: true while haservice is actively fetching the catalog. The service drops
// /HALIST.PRG during a fetch and removes it once /HALIST.TXT has been written.
static int ha_prg_active(void){ int fd=sys_open("/HALIST.PRG",0); if(fd<0) return 0;
    char c; long n=sys_read(fd,&c,1); sys_close(fd); return n>0; }  // 0-byte (truncate-delete) = not fetching
static void ha_picker_render(void){
    int x,y; hap_geom(&x,&y);
    // Self-refresh while the catalog is still empty: re-read /HALIST.TXT (the
    // haservice writes it when the fetch finishes) and periodically re-issue the
    // request, so the panel populates itself when data arrives - no "reopen" needed.
    if(s_ha_cat_len==0){
        static unsigned long s_ha_rl=0, s_ha_rq=0; unsigned long now=uptime_ms();
        if(now-s_ha_rl>700){ s_ha_rl=now; ha_load_catalog(); }
        if(now-s_ha_rq>6000){ s_ha_rq=now; ha_request_catalog(); }
    }
    draw_fill_rect(x,y,HAP_W,HAP_H,CLR_MENU_BG);
    draw_rect_outline(x,y,HAP_W,HAP_H,CLR_MENU_BORDER);
    draw_fill_rect(x,y,HAP_W,26,CLR_MENU_ITEM_HOVER);
    draw_text(x+12,y+7,"Home Assistant widget - settings",CLR_MENU_TEXT);
    // Close (X) button, top-right of the title bar (excluded from the header drag).
    { int cxx=x+HAP_W-24; draw_fill_rect(cxx,y+5,18,16,0x00A83232);
      draw_rect_outline(cxx,y+5,18,16,0x00D06060); draw_text_centered(cxx+9,y+6,"X",0x00FFFFFF); }
    int fw=HAP_W-28;
    // --- rename field (#419), focus 1 ---
    int rx=x+14, ry=y+HAP_RENAME_DY;
    draw_text(rx,ry-14,"Display name (blank = use HA name)",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(rx,ry,fw,24,0x00202020);
    draw_rect_outline(rx,ry,fw,24,(g_wset_focus==1)?0x0066B3FF:CLR_MENU_BORDER);
    draw_text(rx+6,ry+5,g_wset_buf[1],0x00FFFFFF);
    if(g_wset_focus==1){ int cw=text_width(g_wset_buf[1]); draw_fill_rect(rx+6+cw,ry+4,2,16,0x00FFFFFF); }
    // --- display mode chips (#419) ---
    draw_text(x+14,y+HAP_MODE_DY-14,"Display mode",readable_ink_dim(CLR_MENU_BG));
    for(int i=0;i<4;i++){
        int cx,cy,cw,ch; ha_chip_box(x,y,i,&cx,&cy,&cw,&ch);
        int sel=(g_ha_mode==i);
        draw_fill_rect(cx,cy,cw,ch,sel?0x00005FB8:0x00303840);
        draw_rect_outline(cx,cy,cw,ch,sel?0x0066B3FF:CLR_MENU_BORDER);
        draw_text_centered(cx+cw/2,cy+5,HA_MODE_LBL[i],0x00FFFFFF);
    }
    // --- search field (#419 task 2), focus 0 ---
    int sx=x+14, sy=y+HAP_SEARCH_DY;
    draw_text(sx,sy-14,"Search entity id / name / domain",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(sx,sy,fw,24,0x00202020);
    draw_rect_outline(sx,sy,fw,24,(g_wset_focus==0)?0x0066B3FF:CLR_MENU_BORDER);
    draw_text(sx+6,sy+5,g_wset_buf[0],0x00FFFFFF);
    if(g_wset_focus==0){ int cw=text_width(g_wset_buf[0]); draw_fill_rect(sx+6+cw,sy+4,2,16,0x00FFFFFF); }
    // --- filtered entity list (#419b scrollable: wheel + draggable thumb) ---
    int ly=y+HAP_LIST_DY, rowh=20, rows=hap_rows();
    int total=ha_cat_count(g_wset_buf[0]);
    int maxsc=(total>rows)?(total-rows):0;
    if(ha_pick_scroll>maxsc) ha_pick_scroll=maxsc;   // keep scroll in range (also after filtering)
    if(ha_pick_scroll<0) ha_pick_scroll=0;
    char row[256];
    int shown=0;
    for(int r=0;r<rows;r++){
        if(!ha_cat_nth(g_wset_buf[0], ha_pick_scroll+r, row, sizeof(row))) break;
        char eid[96],fn[96]; ha_field(row,0,eid,sizeof(eid)); ha_field(row,1,fn,sizeof(fn));
        int ry2=ly+r*rowh;
        if(fn[0]){ char lbl[60]; int k=0; for(;fn[k]&&k<40;k++) lbl[k]=fn[k]; lbl[k]=0; draw_text(sx+6,ry2+3,lbl,0x00E0E0E0); }
        char es[40]; int k=0; for(;eid[k]&&k<28;k++) es[k]=eid[k]; es[k]=0;   // narrowed for the scrollbar gutter
        draw_text(sx+6+232,ry2+3,es,0x008FA9BD);
        shown++;
    }
    if(shown==0){
        if(s_ha_cat_len){
            draw_text(sx+6,ly+3,"No entities match - clear the search.",readable_ink_dim(CLR_MENU_BG));
        } else {
            // Animated spinner + live status while the background fetch runs; the
            // auto-reload above fills the list when /HALIST.TXT arrives.
            static const int SDX[8]={0,7,10,7,0,-7,-10,-7};
            static const int SDY[8]={-10,-7,0,7,10,7,0,-7};
            unsigned long t=uptime_ms(); int head=(int)((t/90)%8);
            int scx=sx+18, scy=ly+22;
            for(int d=0;d<8;d++){ int ph=(head-d+8)%8;
                unsigned c=(ph<4)?(0x00FFFFFFu-(unsigned)ph*0x00303030u):0x00384048u;
                draw_fill_rect(scx+SDX[d]-2,scy+SDY[d]-2,4,4,c); }
            int active=ha_prg_active();
            draw_text(scx+26,scy-6, active? "Fetching entities from Home Assistant..."
                                          : "Contacting Home Assistant...", 0x00E6E6E6);
            draw_text(scx+26,scy+8, "The list updates automatically - no need to reopen.",
                      readable_ink_dim(CLR_MENU_BG));
        }
    }
    // Scrollbar track + thumb on the right of the list (only when it overflows).
    if(maxsc>0){
        int track_h=rows*rowh, sbx=x+HAP_W-14-HAP_SB_W;
        draw_fill_rect(sbx,ly,HAP_SB_W,track_h,0x00202830);
        draw_rect_outline(sbx,ly,HAP_SB_W,track_h,CLR_MENU_BORDER);
        int th=track_h*rows/total; if(th<24)th=24; if(th>track_h)th=track_h;
        int ty=ly+(track_h-th)*ha_pick_scroll/maxsc;
        draw_fill_rect(sbx+1,ty,HAP_SB_W-2,th,0x005A6A7A);
        draw_rect_outline(sbx+1,ty,HAP_SB_W-2,th,0x0066B3FF);
    }
    int by=y+HAP_H-34;
    draw_text(x+14,by+7,"Click an entity to select.",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(x+HAP_W-134,by,124,26,0x00005FB8);
    draw_text_centered(x+HAP_W-134+62,by+6,"Save & Close",0x00FFFFFF);
}
static int ha_picker_click(int mx,int my){
    int x,y; hap_geom(&x,&y);
    // Modal: clicks outside the panel do nothing (close only via X / Save & Close / ESC).
    if(mx<x||mx>=x+HAP_W||my<y||my>=y+HAP_H){ return 1; }
    // Close (X) button in the title bar.
    if(mx>=x+HAP_W-24&&mx<x+HAP_W-6&&my>=y+5&&my<y+21){ ha_write_label(g_wset_buf[1]); g_wsettings=-1; return 1; }
    int fw=HAP_W-28;
    // rename field focus
    int rx=x+14, ry=y+HAP_RENAME_DY;
    if(mx>=rx&&mx<rx+fw&&my>=ry&&my<ry+24){ g_wset_focus=1; return 1; }
    // search field focus
    int sy=y+HAP_SEARCH_DY;
    if(mx>=rx&&mx<rx+fw&&my>=sy&&my<sy+24){ g_wset_focus=0; return 1; }
    // mode chips
    for(int i=0;i<4;i++){ int cx,cy,cw,ch; ha_chip_box(x,y,i,&cx,&cy,&cw,&ch);
        if(mx>=cx&&mx<cx+cw&&my>=cy&&my<cy+ch){ g_ha_mode=i; return 1; } }
    // Save & Close
    int by=y+HAP_H-34;
    if(my>=by&&my<by+26&&mx>=x+HAP_W-134&&mx<x+HAP_W-10){ ha_write_label(g_wset_buf[1]); g_wsettings=-1; return 1; }
    // entity rows
    int ly=y+HAP_LIST_DY, rowh=20, rows=hap_rows();
    if(my>=ly&&my<ly+rows*rowh){
        int r=(my-ly)/rowh; char row[256];
        if(ha_cat_nth(g_wset_buf[0], ha_pick_scroll+r, row, sizeof(row))){
            char eid[96]; ha_field(row,0,eid,sizeof(eid));
            ha_write_entity(eid); g_show_ha=1; ha_write_label(g_wset_buf[1]); g_wsettings=-1;
        }
    }
    return 1;
}

// ---- #419b modal drag (title bar) + HA entity-list scroll -----------------
int widget_settings_handle_mouse(int x, int y, int click);   // fwd (defined below)
// Active modal rect: HA picker (id 10) or the generic weather/crypto/stock modal.
static void modal_rect(int *x,int *y,int *w,int *h){
    if(g_wsettings==10){ hap_geom(x,y); *w=HAP_W; *h=HAP_H; }
    else { wset_geom(x,y); *w=WSET_W; *h=wset_h(); }
}
int widget_settings_header_hit(int mx,int my){
    if(g_wsettings<0) return 0;
    int x,y,w,h; modal_rect(&x,&y,&w,&h);
    if(!(mx>=x && mx<x+w && my>=y && my<y+26)) return 0;   // the 26px title bar only
    // Exclude the HA picker's top-right close (X) button so a click there closes
    // the dialog instead of starting a drag.
    if(g_wsettings==10 && mx>=x+w-24 && mx<x+w-6 && my>=y+5 && my<y+21) return 0;
    return 1;
}
// HA list scrollbar track geometry; returns 0 when the list does not overflow.
static int ha_scroll_track(int *tx,int *ty,int *tw,int *th,int *maxsc,int *total,int *rows){
    if(g_wsettings!=10) return 0;
    int x,y; hap_geom(&x,&y);
    *rows=hap_rows(); *total=ha_cat_count(g_wset_buf[0]);
    *maxsc=(*total>*rows)?(*total-*rows):0;
    if(*maxsc<=0) return 0;
    *tw=HAP_SB_W; *tx=x+HAP_W-14-HAP_SB_W; *ty=y+HAP_LIST_DY; *th=(*rows)*20;
    return 1;
}
static void ha_scroll_to_y(int my){
    int tx,ty,tw,th,maxsc,total,rows;
    if(!ha_scroll_track(&tx,&ty,&tw,&th,&maxsc,&total,&rows)) return;
    int thmb=th*rows/total; if(thmb<24)thmb=24; if(thmb>th)thmb=th;
    int span=th-thmb; if(span<1) span=1;
    int rel=my-ty-thmb/2; if(rel<0)rel=0; if(rel>span)rel=span;   // centre thumb on cursor
    ha_pick_scroll=rel*maxsc/span;
    if(ha_pick_scroll>maxsc)ha_pick_scroll=maxsc; if(ha_pick_scroll<0)ha_pick_scroll=0;
    (void)tw;
}
static int ha_scrollbar_press(int mx,int my){
    int tx,ty,tw,th,maxsc,total,rows;
    if(!ha_scroll_track(&tx,&ty,&tw,&th,&maxsc,&total,&rows)) return 0;
    if(mx<tx||mx>=tx+tw||my<ty||my>=ty+th) return 0;
    g_wset_drag=2; ha_scroll_to_y(my); return 1;    // begin thumb drag + jump
}
// A fresh left-press on the open modal. Returns 1 if it began an ongoing drag
// (header move or scrollbar), else 0 (handled as a normal click).
int widget_settings_press(int mx,int my){
    if(g_wsettings<0) return 0;
    if(widget_settings_header_hit(mx,my)){                       // drag the modal
        int x,y,w,h; modal_rect(&x,&y,&w,&h);
        g_wset_drag=1; g_wset_grab_dx=mx-x; g_wset_grab_dy=my-y; return 1;
    }
    if(ha_scrollbar_press(mx,my)) return 1;                      // drag the list scrollbar
    widget_settings_handle_mouse(mx,my,1);                       // normal control click
    return 0;
}
void widget_settings_drag_to(int mx,int my){
    if(g_wset_drag==2){ ha_scroll_to_y(my); return; }           // scrollbar thumb
    if(g_wset_drag!=1) return;                                  // header move
    int w=(g_wsettings==10)?HAP_W:WSET_W;
    int h=(g_wsettings==10)?HAP_H:wset_h();
    int nx=mx-g_wset_grab_dx, ny=my-g_wset_grab_dy;
    modal_clamp(&nx,&ny,w,h);
    g_wset_off_x=nx-(g_fb_width-w)/2;
    g_wset_off_y=ny-(g_fb_height-h)/2;
}
void widget_settings_drag_end(void){ g_wset_drag=0; }
int  widget_settings_is_dragging(void){ return g_wset_drag!=0; }
// Mouse wheel scrolls the HA entity list whenever its (modal) picker is open.
// Bounds are not gated on the exact cursor position: the picker is modal, so a
// wheel notch anywhere scrolls its list (also robust to cursor-tracking lag).
int widget_settings_handle_scroll(int mx,int my,int delta){
    if(g_wsettings!=10) return 0;
    (void)mx; (void)my;
    int rows=hap_rows(), total=ha_cat_count(g_wset_buf[0]);
    int maxsc=(total>rows)?(total-rows):0;
    ha_pick_scroll-=delta*3;                                    // wheel up = list up
    if(ha_pick_scroll>maxsc)ha_pick_scroll=maxsc;
    if(ha_pick_scroll<0)ha_pick_scroll=0;
    return 1;
}

void widget_settings_open(int id) {
    g_wsettings = id; g_wset_focus = 0;
    g_wset_off_x = 0; g_wset_off_y = 0; g_wset_drag = 0;    // #419b open centred, no drag
    if (id == 10) {                          // #414/#419 HA settings (picker+rename+mode)
        g_wset_buf[0][0] = '\0'; g_wset_len[0] = 0; ha_pick_scroll = 0;
        ha_load_label();                     // seed rename field with the current label
        int n=0; for(; g_ha_label[n] && n<(int)sizeof(g_wset_buf[1])-1; n++) g_wset_buf[1][n]=g_ha_label[n];
        g_wset_buf[1][n]=0; g_wset_len[1]=n;
        g_wset_focus = 0;                    // start on the search field
        ha_request_catalog();                // ask the service to (re)build /HALIST.TXT
        ha_load_catalog();                   // load whatever exists now
        return;
    }
    wset_load(0, wcfg_path0(id), wcfg_def0(id));
    if (id == 3) wset_load(1, WXCUR_PATH, "USD");
}
int widget_settings_is_open(void) { return g_wsettings >= 0; }

static void wset_write(const char *path, const char *buf, int len) {
    int fd = sys_open(path, 0x0001 | 0x0040);  // O_WRONLY|O_CREAT
    if (fd < 0) return;
    char out[130]; int i = 0;
    for (; i < len && i < 120; i++) out[i] = buf[i];
    out[i++] = '\n';
    while (i < 125) out[i++] = ' ';        // pad so a shorter value leaves no stale tail
    out[i++] = '\n';
    sys_write(fd, out, i); sys_close(fd);
}
static void wset_save(void) {
    if (g_wsettings < 0) return;
    if (g_wsettings == 10) { return; }       // #414 HA saves on entity click
    wset_write(wcfg_path0(g_wsettings), g_wset_buf[0], g_wset_len[0]);
    if (g_wsettings == 3) wset_write(WXCUR_PATH, g_wset_buf[1], g_wset_len[1]);
}

static void wset_field_box(int f, int *fx, int *fy, int *fw, int *fh) {
    int x, y; wset_geom(&x, &y);
    *fx = x + 14; *fw = WSET_W - 28; *fh = 26; *fy = y + 56 + f * 54;
}

void widget_settings_render(void) {
    if (g_wsettings < 0) return;
    if (g_wsettings == 10) { ha_picker_render(); return; }
    int id = g_wsettings, nf = wset_nfields(id);
    int x, y; wset_geom(&x, &y); int H = wset_h();
    draw_fill_rect(x, y, WSET_W, H, CLR_MENU_BG);
    draw_rect_outline(x, y, WSET_W, H, CLR_MENU_BORDER);
    draw_fill_rect(x, y, WSET_W, 26, CLR_MENU_ITEM_HOVER);
    draw_text(x + 12, y + 7, wcfg_title(id), CLR_MENU_TEXT);
    for (int f = 0; f < nf; f++) {
        int fx, fy, fw, fh; wset_field_box(f, &fx, &fy, &fw, &fh);
        const char *lbl  = (f == 0) ? wcfg_lbl0(id) : "Currency";
        const char *hint = (f == 0) ? wcfg_hint0(id) : "e.g. USD, EUR, GBP, AUD, JPY";
        draw_text(fx, fy - 14, lbl, readable_ink_dim(CLR_MENU_BG));
        draw_fill_rect(fx, fy, fw, fh, 0x00202020);
        draw_rect_outline(fx, fy, fw, fh, (f == g_wset_focus) ? 0x0066B3FF : CLR_MENU_BORDER);
        draw_text(fx + 6, fy + 6, g_wset_buf[f], 0x00FFFFFF);
        if (f == g_wset_focus) {
            int cw = text_width(g_wset_buf[f]);
            draw_fill_rect(fx + 6 + cw, fy + 5, 2, fh - 10, 0x00FFFFFF);
        }
        draw_text(fx, fy + fh + 2, hint, readable_ink_dim(CLR_MENU_BG));
    }
    int cbx, cby; wset_chk_box(&cbx, &cby);
    int on = *wset_verbose_ptr(id);
    draw_fill_rect(cbx, cby, 18, 18, 0x00202020);
    draw_rect_outline(cbx, cby, 18, 18, CLR_MENU_BORDER);
    if (on) {                                       // checkmark
        draw_fill_rect(cbx + 4, cby + 8, 3, 5, 0x0066B3FF);
        draw_fill_rect(cbx + 6, cby + 10, 8, 3, 0x0066B3FF);
        draw_fill_rect(cbx + 11, cby + 4, 3, 9, 0x0066B3FF);
    }
    draw_text(cbx + 26, cby + 2, "Verbose (show details / per-line)", CLR_MENU_TEXT);
    int by = y + H - 36;
    draw_fill_rect(x + WSET_W - 184, by, 84, 28, 0x00005FB8);
    draw_text_centered(x + WSET_W - 184 + 42, by + 8, "Save", 0x00FFFFFF);
    draw_fill_rect(x + WSET_W - 94, by, 84, 28, 0x00444444);
    draw_text_centered(x + WSET_W - 94 + 42, by + 8, "Cancel", 0x00FFFFFF);
}

int widget_settings_handle_key(int key) {
    if (g_wsettings < 0) return 0;
    if (key == 27) { g_wsettings = -1; return 1; }                       // ESC
    if (g_wsettings == 10) {                          // #414/#419 edit search / rename
        if (key == '\t') { g_wset_focus ^= 1; return 1; }           // Tab: switch fields
        int f = (g_wset_focus == 1) ? 1 : 0;
        if (f == 0) ha_pick_scroll = 0;                             // search edit resets scroll
        if (key == '\b' || key == 8 || key == 127) { if (g_wset_len[f] > 0) g_wset_buf[f][--g_wset_len[f]] = '\0'; return 1; }
        if (key == '\n' || key == '\r') { ha_load_catalog(); return 1; }
        if (key >= 0x20 && key <= 0x7E && g_wset_len[f] < (int)sizeof(g_wset_buf[f]) - 1) {
            g_wset_buf[f][g_wset_len[f]++] = (char)key; g_wset_buf[f][g_wset_len[f]] = '\0';
        }
        return 1;
    }
    if (key == '\n' || key == '\r') {
        if (wset_nfields(g_wsettings) == 2 && g_wset_focus == 0) { g_wset_focus = 1; return 1; }
        wset_save(); g_wsettings = -1; return 1;
    }
    if (key == '\t') { if (wset_nfields(g_wsettings) == 2) g_wset_focus ^= 1; return 1; }
    int f = g_wset_focus;
    if (key == '\b' || key == 8 || key == 127) { if (g_wset_len[f] > 0) g_wset_buf[f][--g_wset_len[f]] = '\0'; return 1; }
    if (key >= 0x20 && key <= 0x7E && g_wset_len[f] < (int)sizeof(g_wset_buf[f]) - 1) {
        g_wset_buf[f][g_wset_len[f]++] = (char)key; g_wset_buf[f][g_wset_len[f]] = '\0';
    }
    return 1;
}

int widget_settings_handle_mouse(int x, int y, int click) {
    if (g_wsettings < 0) return 0;
    if (!click) return 1;
    if (g_wsettings == 10) return ha_picker_click(x, y);
    int dx, dy; wset_geom(&dx, &dy); int H = wset_h();
    for (int f = 0; f < wset_nfields(g_wsettings); f++) {
        int fx, fy, fw, fh; wset_field_box(f, &fx, &fy, &fw, &fh);
        if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) { g_wset_focus = f; return 1; }
    }
    int cbx, cby; wset_chk_box(&cbx, &cby);
    if (x >= cbx && x < cbx + 260 && y >= cby && y < cby + 18) {     // toggle verbose
        int *v = wset_verbose_ptr(g_wsettings); *v = !*v; return 1;
    }
    int by = dy + H - 36;
    if (y >= by && y < by + 28) {
        if (x >= dx + WSET_W - 184 && x < dx + WSET_W - 100) { wset_save(); g_wsettings = -1; return 1; }
        if (x >= dx + WSET_W - 94  && x < dx + WSET_W - 10)  { g_wsettings = -1; return 1; }
    }
    if (x < dx || x >= dx + WSET_W || y < dy || y >= dy + H) g_wsettings = -1;   // click-away cancels
    return 1;
}

void widgets_render(void) {
    if (g_widgets_enabled) {
        g_draw_blend = g_win_opacity;   // desktop widgets honor the global transparency
        int r = 44; s_clk_r = r;
        if (g_clock_cx < 0) { g_clock_cx = g_fb_width - r - 18; g_clock_cy = 48 + r; }
        if (g_show_clock) widget_analog_clock(g_clock_cx, g_clock_cy, r);
        int calw = 196; s_cal_w = calw;
        s_cal_h = (FONT_CHAR_H + 6) + (FONT_CHAR_H + 2) + 6 * (FONT_CHAR_H + 2) + 6;
        if (g_cal_x < 0) { g_cal_x = g_fb_width - calw - 16; g_cal_y = g_clock_cy + r + 14; }
        if (g_show_calendar) widget_calendar(g_cal_x, g_cal_y, calw);
        // Digital clock (id 5): a normal draggable/lockable widget, not an overlay.
        digclk_geom(&s_digclk_w, &s_digclk_h);
        if (g_digclk_x < 0) { g_digclk_x = g_fb_width - s_digclk_w - 16; g_digclk_y = 8; }
        if (g_show_digclock) digclk_draw(g_digclk_x, g_digclk_y);
        // #274 new widgets (system monitor / timer / world time). Default to a
        // left-edge stack so they don't collide with the right-side clock column.
        if (g_sysmon_x < 0)    { g_sysmon_x = 16;    g_sysmon_y = 70; }
        if (g_timer_x < 0)     { g_timer_x = 16;     g_timer_y = 70 + SYSMON_H + 12; }
        if (g_worldtime_x < 0) { g_worldtime_x = 16; g_worldtime_y = 70 + SYSMON_H + 12 + TIMER_H + 12; }
        if (g_show_sysmon)    widget_sysmon(g_sysmon_x, g_sysmon_y);
        if (g_show_timer)     widget_timer(g_timer_x, g_timer_y);
        if (g_uptime_x < 0)    { g_uptime_x = 16;    g_uptime_y = 70 + SYSMON_H + 12 + TIMER_H + 12 + WT_H + 12; }
        if (g_show_worldtime) widget_worldtime(g_worldtime_x, g_worldtime_y);
        if (g_show_uptime)    widget_uptime(g_uptime_x, g_uptime_y);
        if (g_ha_x < 0)       { g_ha_x = 16; g_ha_y = 70 + SYSMON_H + 12 + TIMER_H + 12 + WT_H + 12 + UPT_H + 12; }
        if (g_show_ha)        ha_card_draw(g_ha_x, g_ha_y);
        netinfo_render();               // #81-83 weather/crypto/stock cards
        if (g_wxtest < 0) { int fd = sys_open("/WXTEST.TXT", 0); g_wxtest = (fd >= 0); if (fd >= 0) sys_close(fd); }
        if (g_wxtest) draw_icon_gallery();
        g_draw_blend = 255;             // sheep + dog are never transparent
    }
    if (g_sheep_enabled) {
        if (g_sheep_count < 1) g_sheep_count = 1;
        if (g_sheep_count > MAX_SHEEP) g_sheep_count = MAX_SHEEP;
        if (!g_widgets_draw_only) {
            // Advance positions only in the full/busy path; the idle path has
            // already stepped the sheep in widgets_collect_damage().
            g_wn = wm_get_windows(g_wlist, 16); if (g_wn < 0) g_wn = 0;
            for (int i = 0; i < g_sheep_count; i++) {
                if (!g_sheep[i].inited) sheep_spawn(i);
                sheep_one_update(i);
            }
        }
        for (int i = 0; i < g_sheep_count; i++) sheep_one_draw(&g_sheep[i]);
    }
    if (g_dog_enabled) { if (!g_widgets_draw_only) dog_update(); dog_draw(); }
}

// ===========================================================================
// #102/#379 dirty-rect: advance per-frame widget state ONCE and record which
// regions changed. Called by the idle compositor before the clipped redraw.
// After this, widgets_render() is invoked in draw-only mode per damage rect.
// Only elements whose displayed content changed contribute damage, so a static
// desktop yields no damage (present nothing = 0 CPU); an animated pet/clock
// yields only its own small rect.
// ===========================================================================
static unsigned strsum(const char *s) {
    unsigned h = 2166136261u;
    for (int i = 0; s && s[i]; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

void widgets_collect_damage(void) {
    int bx, by, bw, bh;

    if (g_widgets_enabled) {
        long rtc = sys_get_rtc_time();
        int sec = (int)(rtc & 0xFF);
        int minu = (int)((rtc >> 8) & 0xFF);
        int day = 19, mon = 6, yr = 2026; get_rtc_date(&day, &mon, &yr);

        static int last_sec = -1, last_min = -1, last_day = -1;
        int sec_ch = (sec != last_sec);
        int min_ch = (minu != last_min);
        int day_ch = (day != last_day);
        last_sec = sec; last_min = minu; last_day = day;

        // Analog clock (0): second hand moves each second.
        if (sec_ch && g_show_clock && widget_box(0, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Calendar (1): only at day rollover.
        if (day_ch && g_show_calendar && widget_box(1, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Digital clock (5): seconds (when shown) else minute.
        if (((g_digclk_secs && sec_ch) || min_ch) && g_show_digclock &&
            widget_box(5, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // World time (8): minute.
        if (min_ch && g_show_worldtime && widget_box(8, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Uptime (9): second.
        if (sec_ch && g_show_uptime && widget_box(9, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Timer (7): while running the display changes continuously.
        if (g_show_timer && s_tmr_running && widget_box(7, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);

        // System monitor (6): sample on a fixed ~0.5s wall-clock cadence (so the
        // rate is independent of the adaptive idle poll interval); damage on a
        // new sample.
        if (g_show_sysmon) {
            static unsigned long last_sm_ms = 0;
            unsigned long nowms = uptime_ms();
            if (nowms - last_sm_ms >= 500) {
                last_sm_ms = nowms;
                sysmon_sample();
                if (widget_box(6, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
            }
        }

        // Network info cards (2-4): refresh cached files on cadence; damage when
        // the payload actually changed.
        {
            static unsigned last_card_sig = 0;
            if ((s_netinfo_tick++ % 150) == 0) netinfo_refresh();
            unsigned sig = strsum(s_weather) * 131u + strsum(s_crypto) * 17u + strsum(s_stocks);
            if (sig != last_card_sig) {
                last_card_sig = sig;
                for (int c = 0; c < 3; c++)
                    if (widget_box(2 + c, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
            }
        }
    }

    // Desktop pets: step them on a fixed ~11 FPS wall-clock cadence (so their
    // walk speed stays the same regardless of the adaptive idle poll interval)
    // and damage OLD+NEW rects. The generous margin covers sprite overflow above
    // the logical box and the wall-climb pose. Drawing happens later in
    // widgets_render() (draw-only).
    const int M = 20;   // safety margin around the pet sprite
    if (g_sheep_enabled || g_dog_enabled) {
        static unsigned long last_pet_ms = 0;
        unsigned long nowms = uptime_ms();
        if (nowms - last_pet_ms >= 200) {   // ~5 FPS pets (the old idle path drew
            last_pet_ms = nowms;
            if (g_sheep_enabled) {
                if (g_sheep_count < 1) g_sheep_count = 1;
                if (g_sheep_count > MAX_SHEEP) g_sheep_count = MAX_SHEEP;
                g_wn = wm_get_windows(g_wlist, 16); if (g_wn < 0) g_wn = 0;
                int sw = sheep_w(), sh = sheep_h();
                for (int i = 0; i < g_sheep_count; i++) {
                    if (!g_sheep[i].inited) sheep_spawn(i);
                    int ox = g_sheep[i].x, oy = g_sheep[i].y;
                    sheep_one_update(i);
                    damage_add(ox - M, oy - M, sw + 2 * M, sh + 2 * M);
                    damage_add(g_sheep[i].x - M, g_sheep[i].y - M, sw + 2 * M, sh + 2 * M);
                }
            }
            if (g_dog_enabled) {
                int ox = dog_x, oy = dog_y;
                dog_update();
                if (ox >= 0) damage_add(ox - M, oy - M, DOG_W + 2 * M, DOG_H + 2 * M);
                damage_add(dog_x - M, dog_y - M, DOG_W + 2 * M, DOG_H + 2 * M);
            }
        }
    }
}

// ===========================================================================
// Widget registry: SINGLE source of truth for the available desktop widgets.
// The dynamic widgets tray menu (traymenu.c) enumerates this instead of a
// hardcoded list, so adding a widget here makes it appear in the tray menu
// automatically. The `flag` pointers are the same live globals the desktop
// widget layer toggles, so menu state and on-screen state never diverge.
// ===========================================================================
extern int g_show_digclock;   // defined in clock.c (top-right digital clock)
static const widget_desc_t g_widget_registry[] = {
    { "Digital Clock", "show_digclock", &g_show_digclock  },
    { "Clock",         "show_clock",    &g_show_clock     },
    { "Calendar",      "show_calendar", &g_show_calendar  },
    { "Weather",       "show_weather",  &g_show_weather   },
    { "Crypto",        "show_crypto",   &g_show_crypto    },
    { "Stocks",        "show_stocks",   &g_show_stocks    },
    { "System Monitor","show_sysmon",   &g_show_sysmon    },   // #274
    { "Timer",         "show_timer",    &g_show_timer     },   // #274
    { "World Time",    "show_worldtime",&g_show_worldtime },   // #274
    { "Uptime",        "show_uptime",   &g_show_uptime    },   // #282
    { "Home Assistant","show_ha",       &g_show_ha        },   // #414
    { "Sticky Notes",  "show_stickies", &g_show_stickies  },   // #270
    { "Sheep",         "sheep_show",    &g_sheep_enabled  },
    { "AI Chat",       "show_aichat",   &g_aichat_enabled },   // #185 external app
};
const widget_desc_t *widget_registry(int *count) {
    if (count) *count = (int)(sizeof(g_widget_registry) / sizeof(g_widget_registry[0]));
    return g_widget_registry;
}
