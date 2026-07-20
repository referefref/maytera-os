// help_ui.c - implementation of the GUI help primitives.
//
// Built on libc gui.h / gui_style.h. Self-contained: static state, no malloc,
// no kernel changes. Timing via uptime_ms(). Robust against unset/odd inputs.

#include "help_ui.h"

#include "../libc/gui.h"
#include "../libc/syscall.h"
#include "../libc/string.h"

typedef struct {
    int  used;
    int  win;
    int  x, y, w, h;
    char text[128];
} tip_t;

static tip_t   g_tips[HELP_UI_MAX_TIPS];
static int     g_tip_count = 0;

// Hover tracking
static int            g_hover_idx = -1;      // tip slot under the cursor
static unsigned long  g_hover_since = 0;     // when hover began (ms)
static int            g_show_idx = -1;       // tip currently shown
static int            g_mx = -1, g_my = -1;  // last mouse pos (window-relative)

// ---------------------------------------------------------------------------
static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    if (!src) src = "";
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int help_ui_register(int win, int x, int y, int w, int h, const char *text) {
    // Replace if the same rect already exists.
    for (int i = 0; i < g_tip_count; i++) {
        if (g_tips[i].used && g_tips[i].win == win &&
            g_tips[i].x == x && g_tips[i].y == y &&
            g_tips[i].w == w && g_tips[i].h == h) {
            copy_text(g_tips[i].text, text, (int)sizeof(g_tips[i].text));
            return i;
        }
    }
    // Find a free slot.
    int slot = -1;
    for (int i = 0; i < HELP_UI_MAX_TIPS; i++) {
        if (!g_tips[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;
    g_tips[slot].used = 1;
    g_tips[slot].win = win;
    g_tips[slot].x = x; g_tips[slot].y = y;
    g_tips[slot].w = w; g_tips[slot].h = h;
    copy_text(g_tips[slot].text, text, (int)sizeof(g_tips[slot].text));
    if (slot >= g_tip_count) g_tip_count = slot + 1;
    return slot;
}

void help_ui_clear(void) {
    for (int i = 0; i < HELP_UI_MAX_TIPS; i++) g_tips[i].used = 0;
    g_tip_count = 0;
    g_hover_idx = -1;
    g_show_idx = -1;
    g_hover_since = 0;
}

void help_ui_tick(int mouse_x, int mouse_y, unsigned long now_ms) {
    g_mx = mouse_x; g_my = mouse_y;

    int hit = -1;
    for (int i = 0; i < g_tip_count; i++) {
        if (!g_tips[i].used) continue;
        if (in_rect(mouse_x, mouse_y, g_tips[i].x, g_tips[i].y,
                    g_tips[i].w, g_tips[i].h)) { hit = i; break; }
    }

    if (hit != g_hover_idx) {
        g_hover_idx = hit;
        g_hover_since = now_ms;
        g_show_idx = -1;        // moving to a new widget hides the old tip
        return;
    }
    if (hit >= 0 && g_show_idx < 0 &&
        (now_ms - g_hover_since) >= HELP_UI_HOVER_MS) {
        g_show_idx = hit;       // dwell satisfied: show it
    }
    if (hit < 0) g_show_idx = -1;
}

void help_ui_draw(int win) {
    if (g_show_idx < 0 || g_show_idx >= g_tip_count) return;
    tip_t *t = &g_tips[g_show_idx];
    if (!t->used || t->win != win || !t->text[0]) return;

    // Position the tooltip just below-right of the cursor, clamped to a sane
    // window-relative area. We do not know the window size here, so we keep the
    // box near the widget if the cursor is unknown.
    int tw = gui_ttf_width(t->text, 12) + 16;
    if (tw < 40) tw = 40;
    int th = 22;
    int tx = (g_mx >= 0 ? g_mx + 14 : t->x);
    int ty = (g_my >= 0 ? g_my + 18 : t->y + t->h + 4);
    if (tx < 2) tx = 2;
    if (ty < 2) ty = 2;

    // Classic tooltip palette: pale yellow surface, dark ink, 1px border.
    uint32_t bg     = 0x00FFFFE1;   // info yellow
    uint32_t border = 0x00808080;
    uint32_t ink    = 0x00000000;

    gui_fill_rect(win, tx, ty, tw, th, bg);
    gui_draw_rect_outline(win, tx, ty, tw, th, border);
    win_draw_text_ttf(win, tx + 8, ty + 4, t->text, 12, ink);
}

// ---------------------------------------------------------------------------
// "?" question icon.
// ---------------------------------------------------------------------------
void help_ui_question_icon(int win, int x, int y, int diameter) {
    if (diameter < 12) diameter = 12;
    gui_palette_t *pal = gui_pal();
    uint32_t fill   = pal ? pal->accent : 0x00569CD6;
    uint32_t bg     = pal ? pal->surface : 0x001E1E1E;
    uint32_t ink    = 0x00FFFFFF;
    // Round accent disc with a centered "?".
    gui_fill_circle_aa(win, x, y, diameter, fill, bg);
    int cx = x + diameter / 2;
    int cy = y + diameter / 2 - 8;
    // center the glyph
    int gw = gui_ttf_width("?", 13);
    win_draw_text_ttf(win, cx - gw / 2, cy, "?", 13, ink);
}

bool help_ui_question_hit(int x, int y, int diameter, int mx, int my) {
    if (diameter < 12) diameter = 12;
    int cx = x + diameter / 2;
    int cy = y + diameter / 2;
    int r  = diameter / 2;
    int dx = mx - cx, dy = my - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

// ---------------------------------------------------------------------------
// F1 / launch helpers.
// ---------------------------------------------------------------------------
bool help_ui_is_f1(unsigned int keycode) {
    return keycode == HELP_UI_KEY_F1;
}

int help_ui_open_topic(const char *help_file, const char *topic_id) {
    if (!help_file || !*help_file) help_file = "/HELP/SYSTEM.MHLP";

    // Build argv: [ "/APPS/HELP", help_file, topic_id? ]
    static char a0[] = "/APPS/HELP";
    char *argv[3];
    int argc = 0;
    argv[argc++] = a0;
    argv[argc++] = (char *)help_file;
    if (topic_id && *topic_id) argv[argc++] = (char *)topic_id;

    int r = sys_spawn_args("/APPS/HELP", argv, argc);
    if (r < 0) r = sys_spawn_args("/APPS/help", argv, argc); // ext2 lowercase
    return r;
}

int help_ui_open_system(void) {
    return help_ui_open_topic("/HELP/SYSTEM.MHLP", "desktop");
}
