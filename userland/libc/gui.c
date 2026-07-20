// gui.c - GUI library implementation for MayteraOS user-space applications
// Provides high-level GUI wrappers around window syscalls
#include "gui.h"
#include "syscall.h"
#include "string.h"

// ============================================================================
// Window Protocol Client Implementation
// ============================================================================

// Create a window with specified title and dimensions
// Returns: window handle (>=0) on success, -1 on failure
int gui_window_create(const char *title, int x, int y, int width, int height) {
    return win_create(title, x, y, width, height);
}

// Destroy a window
// Returns: 0 on success, -1 on failure
int gui_window_destroy(int handle) {
    return win_destroy(handle);
}

// ============================================================================
// Drawing Functions
// ============================================================================

// Fill a rectangle with solid color
void gui_fill_rect(int handle, int x, int y, int width, int height, uint32_t color) {
    win_draw_rect(handle, x, y, width, height, color);
}

// Draw a single pixel
void gui_draw_pixel(int handle, int x, int y, uint32_t color) {
    win_draw_pixel(handle, x, y, color);
}

// Draw text at position
void gui_draw_text(int handle, int x, int y, const char *text, uint32_t color) {
    win_draw_text(handle, x, y, text, color);
}

// Draw text with background color
void gui_draw_text_bg(int handle, int x, int y, const char *text,
                      uint32_t fg_color, uint32_t bg_color) {
    // First draw background
    int len = 0;
    const char *p = text;
    while (*p++) len++;

    win_draw_rect(handle, x, y, len * FONT_WIDTH, FONT_HEIGHT, bg_color);
    // Then draw text
    win_draw_text(handle, x, y, text, fg_color);
}

// Draw a character at position
void gui_draw_char(int handle, int x, int y, char c, uint32_t color) {
    char str[2] = { c, '\0' };
    win_draw_text(handle, x, y, str, color);
}

// Draw a character with background
void gui_draw_char_bg(int handle, int x, int y, char c,
                      uint32_t fg_color, uint32_t bg_color) {
    // Draw background cell
    win_draw_rect(handle, x, y, FONT_WIDTH, FONT_HEIGHT, bg_color);
    // Draw character
    char str[2] = { c, '\0' };
    win_draw_text(handle, x, y, str, fg_color);
}

// Draw rectangle outline (not filled)
void gui_draw_rect(int handle, int x, int y, int width, int height, uint32_t color) {
    // Top
    win_draw_rect(handle, x, y, width, 1, color);
    // Bottom
    win_draw_rect(handle, x, y + height - 1, width, 1, color);
    // Left
    win_draw_rect(handle, x, y, 1, height, color);
    // Right
    win_draw_rect(handle, x + width - 1, y, 1, height, color);
}

// Draw a 3D-style button
void gui_draw_button_3d(int handle, int x, int y, int width, int height,
                        uint32_t bg_color, bool pressed) {
    // Background
    win_draw_rect(handle, x, y, width, height, bg_color);

    uint32_t light = pressed ? 0x00202020 : 0x00808080;
    uint32_t dark = pressed ? 0x00808080 : 0x00202020;

    // Top and left edges (light when not pressed)
    win_draw_rect(handle, x, y, width, 2, light);
    win_draw_rect(handle, x, y, 2, height, light);

    // Bottom and right edges (dark when not pressed)
    win_draw_rect(handle, x, y + height - 2, width, 2, dark);
    win_draw_rect(handle, x + width - 2, y, 2, height, dark);
}

// Draw centered text within a rectangle
void gui_draw_text_centered(int handle, int x, int y, int width, int height,
                            const char *text, uint32_t color) {
    int text_w = gui_string_width(text);
    int text_x = x + (width - text_w) / 2;
    int text_y = y + (height - FONT_HEIGHT) / 2;
    win_draw_text(handle, text_x, text_y, text, color);
}

// ============================================================================
// Event Handling
// ============================================================================

// Get window event with timeout
// timeout: 0 = non-blocking, >0 = wait up to timeout ms, -1 = wait forever
// Returns: event type, fills event structure
int gui_get_event(int handle, gui_event_t *event, int timeout) {
    return win_get_event(handle, event, timeout);
}

// Request window redraw
void gui_invalidate(int handle) {
    win_invalidate(handle);
}

// ============================================================================
// Simple Widget Drawing Helpers
// ============================================================================

// Draw a labeled button
void gui_draw_button(int handle, int x, int y, int width, int height,
                     const char *label, uint32_t bg_color, uint32_t text_color,
                     bool hovered, bool pressed) {
    uint32_t actual_bg = hovered ? 0x00606060 : bg_color;

    gui_draw_button_3d(handle, x, y, width, height, actual_bg, pressed);
    gui_draw_text_centered(handle, x, y, width, height, label, text_color);
}

// Draw a text input field
void gui_draw_textfield(int handle, int x, int y, int width, int height,
                        const char *text, uint32_t bg_color, uint32_t text_color,
                        uint32_t border_color) {
    // Background
    win_draw_rect(handle, x, y, width, height, bg_color);
    // Border
    gui_draw_rect(handle, x, y, width, height, border_color);
    // Text (left-aligned with padding)
    if (text && *text) {
        win_draw_text(handle, x + 4, y + (height - FONT_HEIGHT) / 2, text, text_color);
    }
}

// Draw a checkbox
void gui_draw_checkbox(int handle, int x, int y, bool checked,
                       const char *label, uint32_t color) {
    int box_size = 16;

    // Box background
    win_draw_rect(handle, x, y, box_size, box_size, 0x00FFFFFF);
    // Box border
    gui_draw_rect(handle, x, y, box_size, box_size, 0x00404040);

    // Checkmark
    if (checked) {
        // Draw a simple X as checkmark
        for (int i = 3; i < box_size - 3; i++) {
            win_draw_pixel(handle, x + i, y + i, color);
            win_draw_pixel(handle, x + box_size - 1 - i, y + i, color);
        }
    }

    // Label
    if (label && *label) {
        win_draw_text(handle, x + box_size + 4, y + (box_size - FONT_HEIGHT) / 2,
                      label, color);
    }
}

// Draw a progress bar
void gui_draw_progressbar(int handle, int x, int y, int width, int height,
                          int percent, uint32_t bg_color, uint32_t fg_color) {
    // Background
    win_draw_rect(handle, x, y, width, height, bg_color);
    // Border
    gui_draw_rect(handle, x, y, width, height, 0x00404040);

    // Fill
    if (percent > 0) {
        int fill_width = (width - 4) * percent / 100;
        if (fill_width > 0) {
            win_draw_rect(handle, x + 2, y + 2, fill_width, height - 4, fg_color);
        }
    }
}

// Draw a vertical scrollbar
void gui_draw_scrollbar_v(int handle, int x, int y, int height,
                          int thumb_pos, int thumb_size, uint32_t bg_color) {
    int scroll_width = 16;

    // Background
    win_draw_rect(handle, x, y, scroll_width, height, bg_color);

    // Thumb
    win_draw_rect(handle, x + 2, y + thumb_pos, scroll_width - 4, thumb_size, 0x00808080);
    gui_draw_rect(handle, x + 2, y + thumb_pos, scroll_width - 4, thumb_size, 0x00404040);
}

// ============================================================================
// Integer to String Conversion
// ============================================================================

// Convert integer to string
void gui_itoa(long num, char *buf, int max_len) {
    bool negative = false;
    char tmp[24];
    int i = 0;

    if (num < 0) {
        negative = true;
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

// Convert unsigned integer to string
void gui_utoa(unsigned long num, char *buf, int max_len) {
    char tmp[24];
    int i = 0;

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
    while (i > 0 && j < max_len - 1) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// Convert integer to hex string
void gui_itoa_hex(unsigned long num, char *buf, int digits) {
    static const char hex_chars[] = "0123456789ABCDEF";

    buf[0] = '0';
    buf[1] = 'x';

    for (int i = digits - 1; i >= 0; i--) {
        buf[2 + i] = hex_chars[num & 0xF];
        num >>= 4;
    }
    buf[2 + digits] = '\0';
}

// ============== Fullscreen/Direct Framebuffer Syscalls ==============

#define SYS_FB_INFO         90
#define SYS_FB_MAP          91
#define SYS_FB_FLIP         92
#define SYS_FB_EXCLUSIVE    93

extern long syscall1(long num, long arg1);
extern long syscall0(long num);

// Get framebuffer info - returns width | (height << 16) | (pitch << 32)
long fb_get_info(int *width, int *height, int *pitch) {
    long result = syscall0(SYS_FB_INFO);
    if (width) *width = result & 0xFFFF;
    if (height) *height = (result >> 16) & 0xFFFF;
    if (pitch) *pitch = (result >> 32) & 0xFFFFFFFF;
    return result;
}

// fb_map() and fb_flip() are provided as static inline in syscall.h

// Enter or exit exclusive fullscreen mode
void fb_set_exclusive(int enable) {
    syscall1(SYS_FB_EXCLUSIVE, enable);
}

// ============================================================================
// Widget-Style Engine (Phase 0-2) - see gui_style.h
// ============================================================================
#include "gui_style.h"

static ui_style_t   g_style = { GUI_STYLE_MODERN, GUI_RADIUS, true, true };
static gui_palette_t g_pal = {
    0x00252525, 0x002A2A2A, 0x00FFFFFF, 0x00AAAAAA,
    0x003A6EA5, 0x004A7EB5, 0x00505050, 0x00333333, 0x00505050, 0x00404040
};

void gui_set_style(gui_base_style_t base) {
    g_style.base = base;
    if (base == GUI_STYLE_MODERN) { g_style.radius = GUI_RADIUS; g_style.gradients = true; g_style.shadows = true; }
    else { g_style.radius = 0; g_style.gradients = false; g_style.shadows = false; } // classic + flat
}
ui_style_t gui_active_style(void) { return g_style; }
void gui_set_palette(const gui_palette_t *p) { if (p) g_pal = *p; }
gui_palette_t *gui_pal(void) { return &g_pal; }

uint32_t gui_mix(uint32_t a, uint32_t b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int ra=(a>>16)&0xFF, ga=(a>>8)&0xFF, ba=a&0xFF;
    int rb=(b>>16)&0xFF, gb=(b>>8)&0xFF, bb=b&0xFF;
    int r  = ra + (rb-ra)*t/255;
    int g  = ga + (gb-ga)*t/255;
    int bl = ba + (bb-ba)*t/255;
    return (uint32_t)((r<<16)|(g<<8)|bl);
}
uint32_t gui_lighten(uint32_t c, int amt) { return gui_mix(c, 0x00FFFFFF, amt); }
uint32_t gui_darken(uint32_t c, int amt)  { return gui_mix(c, 0x00000000, amt); }
uint32_t gui_ink_on(uint32_t bg) {
    int r=(bg>>16)&0xFF, g=(bg>>8)&0xFF, b=bg&0xFF;
    int lum = (r*54 + g*183 + b*19) >> 8;
    return lum > 140 ? 0x00141414 : 0x00F4F4F4;
}

static int gs_isqrt(int n) { if (n<=0) return 0; int x=n, y=(x+1)/2; while (y<x){ x=y; y=(x+n/x)/2; } return x; }
static int gs_corner_inset(int r, int d) {
    if (r <= 0) return 0;
    int dy = r - 1 - d;
    int dx = r - gs_isqrt(r*r - dy*dy);
    return dx < 0 ? 0 : dx;
}
static void gs_line(int handle, int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = x1>x0?x1-x0:x0-x1, sx = x0<x1?1:-1;
    int dy = y1>y0?y1-y0:y0-y1, sy = y0<y1?1:-1;
    int err = (dx>dy?dx:-dy)/2, e2;
    for (;;) {
        win_draw_pixel(handle, x0, y0, col);
        if (x0==x1 && y0==y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

int gui_ttf_width(const char *s, int size) {
    if (!s || !*s) return 0;
    int w = ttf_measure(s, size);
    if (w <= 0) { int n=0; while (s[n]) n++; w = n * (size*6/10); }
    return w;
}
void gui_text_ttf_centered(int handle, int x, int y, int w, int h,
                           const char *s, uint32_t color, int size) {
    int tw = gui_ttf_width(s, size);
    int tx = x + (w - tw)/2; if (tx < x) tx = x;
    int ty = y + (h - size)/2; if (ty < y) ty = y;
    win_draw_text_ttf(handle, tx, ty, s, size, color);
}

void gui_fill_rounded_grad(int handle, int x, int y, int w, int h, int r,
                           uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    if (r*2 > w) r = w/2;
    if (r*2 > h) r = h/2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) inset = gs_corner_inset(r, j);
        else if (j >= h - r) inset = gs_corner_inset(r, h - 1 - j);
        int rw = w - 2*inset;
        if (rw <= 0) continue;
        uint32_t col = (h > 1) ? gui_mix(top, bottom, j*255/(h-1)) : top;
        win_draw_rect(handle, x + inset, y + j, rw, 1, col);
    }
}
void gui_fill_rounded(int handle, int x, int y, int w, int h, int r, uint32_t color) {
    gui_fill_rounded_grad(handle, x, y, w, h, r, color, color);
}

// --- Antialiased rounded fill -------------------------------------------------
// No framebuffer read-back is available, so edge pixels blend the fill color
// toward a caller-supplied background (the surface the shape sits on). Corner
// coverage is computed by 4x4 supersampling against the rounded-rect outline.
static inline void gs_aa_px(int handle, int px, int py, uint32_t color, uint32_t bg, int cov) {
    if (cov <= 0) return;
    if (cov >= 255) { win_draw_pixel(handle, px, py, color); return; }
    win_draw_pixel(handle, px, py, gui_mix(bg, color, cov));
}
// inside test in 1/4-pixel fixed units
static inline int gs_rr_inside4(int fx, int fy, int x4, int y4, int w4, int h4, int r4) {
    if (fx < x4 || fy < y4 || fx >= x4 + w4 || fy >= y4 + h4) return 0;
    int left = fx < x4 + r4, right = fx >= x4 + w4 - r4;
    int top  = fy < y4 + r4, bot   = fy >= y4 + h4 - r4;
    if ((left || right) && (top || bot)) {
        int cx = left ? x4 + r4 : x4 + w4 - r4;
        int cy = top  ? y4 + r4 : y4 + h4 - r4;
        int dx = fx - cx, dy = fy - cy;
        return (dx*dx + dy*dy) <= r4*r4;
    }
    return 1;
}
void gui_fill_rounded_aa(int handle, int x, int y, int w, int h, int r, uint32_t color, uint32_t bg) {
    if (w <= 0 || h <= 0) return;
    if (r*2 > w) r = w/2;
    if (r*2 > h) r = h/2;
    if (r <= 0) { win_draw_rect(handle, x, y, w, h, color); return; }
    int x4 = x*4, y4 = y*4, w4 = w*4, h4 = h*4, r4 = r*4;
    for (int j = 0; j < h; j++) {
        int corner = (j < r) || (j >= h - r);
        if (!corner) { win_draw_rect(handle, x, y+j, w, 1, color); continue; }
        // solid middle span (between the two corner columns)
        if (w - 2*r > 0) win_draw_rect(handle, x+r, y+j, w-2*r, 1, color);
        // AA the two corner column bands
        for (int side = 0; side < 2; side++) {
            int cx0 = side ? (x + w - r) : x;
            for (int i = 0; i < r; i++) {
                int px = cx0 + i, cnt = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        cnt += gs_rr_inside4(px*4+sx, (y+j)*4+sy, x4, y4, w4, h4, r4);
                gs_aa_px(handle, px, y+j, color, bg, cnt*255/16);
            }
        }
    }
}
// Antialiased filled circle (diameter d) blending toward bg. d==w==h, r=d/2.
void gui_fill_circle_aa(int handle, int x, int y, int d, uint32_t color, uint32_t bg) {
    gui_fill_rounded_aa(handle, x, y, d, d, d/2, color, bg);
}
void gui_rounded_border(int handle, int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (r*2 > w) r = w/2;
    if (r*2 > h) r = h/2;
    int prev = -1;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) inset = gs_corner_inset(r, j);
        else if (j >= h - r) inset = gs_corner_inset(r, h - 1 - j);
        win_draw_pixel(handle, x+inset, y+j, color);
        win_draw_pixel(handle, x+w-1-inset, y+j, color);
        if (j == 0 || j == h-1) win_draw_rect(handle, x+inset, y+j, w-2*inset, 1, color);
        else if (prev >= 0 && inset != prev) {
            int a = inset<prev?inset:prev, b = inset<prev?prev:inset;
            win_draw_rect(handle, x+a, y+j, b-a, 1, color);
            win_draw_rect(handle, x+w-1-b, y+j, b-a, 1, color);
        }
        prev = inset;
    }
}
void gui_soft_shadow(int handle, int x, int y, int w, int h, int r, uint32_t bg) {
    gui_fill_rounded(handle, x+2, y+4, w, h, r, gui_mix(bg, 0x00000000, 16));
    gui_fill_rounded(handle, x+1, y+3, w, h, r, gui_mix(bg, 0x00000000, 28));
}

void gui_button(int handle, int x, int y, int w, int h, const char *label,
                gui_btn_variant_t variant, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    bool disabled = (st == GUI_ST_DISABLED);
    uint32_t base, ink, bord;
    if (variant == GUI_BTN_PRIMARY)      { base = p->accent; ink = gui_ink_on(p->accent); bord = gui_darken(p->accent, 40); }
    else if (variant == GUI_BTN_GHOST)   { base = p->surface; ink = p->accent; bord = p->border; }
    else                                 { base = gui_mix(p->surface_raised, p->ink, 8); ink = p->ink; bord = p->border; }
    if (!disabled) {
        if (st == GUI_ST_HOVER)        base = (variant==GUI_BTN_PRIMARY) ? p->accent_hover : gui_lighten(base, 18);
        else if (st == GUI_ST_PRESSED) base = gui_darken(base, 18);
    } else { base = gui_mix(base, p->surface, 150); ink = gui_mix(ink, p->surface, 110); }

    if (g_style.base == GUI_STYLE_CLASSIC) {
        bool pressed = (st == GUI_ST_PRESSED);
        win_draw_rect(handle, x, y, w, h, base);
        uint32_t lt = pressed ? gui_darken(base,40) : gui_lighten(base,70);
        uint32_t dk = pressed ? gui_lighten(base,70) : gui_darken(base,55);
        win_draw_rect(handle, x, y, w, 2, lt);
        win_draw_rect(handle, x, y, 2, h, lt);
        win_draw_rect(handle, x, y+h-2, w, 2, dk);
        win_draw_rect(handle, x+w-2, y, 2, h, dk);
    } else if (g_style.base == GUI_STYLE_FLAT) {
        win_draw_rect(handle, x, y, w, h, base);
        gui_draw_rect_outline(handle, x, y, w, h, bord);
    } else { // modern
        int r = g_style.radius;
        if (g_style.shadows && !disabled) gui_soft_shadow(handle, x, y, w, h, r, p->surface);
        gui_fill_rounded_aa(handle, x, y, w, h, r, bord, p->surface);
        uint32_t top = gui_lighten(base, g_style.gradients ? 16 : 0);
        uint32_t bot = gui_darken(base,  g_style.gradients ? 10 : 0);
        gui_fill_rounded_grad(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, top, bot);
    }
    if (st == GUI_ST_FOCUS) {
        if (g_style.base == GUI_STYLE_MODERN) gui_rounded_border(handle, x, y, w, h, g_style.radius, p->accent);
        else gui_draw_rect_outline(handle, x, y, w, h, p->accent);
    }
    if (label && *label) gui_text_ttf_centered(handle, x, y, w, h, label, ink, GUI_TTF_SIZE);
}

void gui_checkbox(int handle, int x, int y, int sz, bool checked,
                  const char *label, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    bool disabled = (st == GUI_ST_DISABLED);
    uint32_t boxbg = checked ? p->accent : p->field_bg;
    uint32_t bord  = checked ? gui_darken(p->accent,30) : p->field_border;
    if (disabled) boxbg = gui_mix(boxbg, p->surface, 130);

    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, sz, sz, checked ? p->accent : 0x00FFFFFF);
        win_draw_rect(handle, x, y, sz, 1, gui_darken(p->surface,70));
        win_draw_rect(handle, x, y, 1, sz, gui_darken(p->surface,70));
        win_draw_rect(handle, x, y+sz-1, sz, 1, gui_lighten(p->surface,80));
        win_draw_rect(handle, x+sz-1, y, 1, sz, gui_lighten(p->surface,80));
    } else {
        int r = (g_style.base == GUI_STYLE_MODERN) ? 4 : 0;
        gui_fill_rounded_aa(handle, x, y, sz, sz, r, bord, p->surface);
        gui_fill_rounded(handle, x+1, y+1, sz-2, sz-2, r>0?r-1:0, boxbg);
    }
    if (checked) {
        uint32_t tick = (g_style.base == GUI_STYLE_CLASSIC) ? p->ink : gui_ink_on(p->accent);
        int x0 = x + sz*27/100, y0 = y + sz*52/100;
        int x1 = x + sz*43/100, y1 = y + sz*70/100;
        int x2 = x + sz*76/100, y2 = y + sz*30/100;
        gs_line(handle, x0, y0, x1, y1, tick);
        gs_line(handle, x0, y0+1, x1, y1+1, tick);
        gs_line(handle, x1, y1, x2, y2, tick);
        gs_line(handle, x1, y1+1, x2, y2+1, tick);
    }
    if (label && *label) {
        uint32_t ink = disabled ? gui_mix(p->ink, p->surface, 120) : p->ink;
        win_draw_text_ttf(handle, x+sz+8, y + (sz-GUI_TTF_SIZE)/2, label, GUI_TTF_SIZE, ink);
    }
}

void gui_toggle(int handle, int x, int y, int w, int h, bool on, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    (void)st;
    uint32_t tr = on ? p->accent : p->field_bg;
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, tr);
        gui_draw_rect_outline(handle, x, y, w, h, on ? gui_darken(p->accent,30) : p->field_border);
        int kx = on ? (x + w - (h-4) - 2) : (x + 2);
        win_draw_rect(handle, kx, y+2, h-4, h-4, p->surface_raised);
        gui_draw_rect_outline(handle, kx, y+2, h-4, h-4, p->border);
    } else {
        int r = h/2;
        gui_fill_rounded_aa(handle, x, y, w, h, r, tr, p->surface);
        if (!on) gui_rounded_border(handle, x, y, w, h, r, p->field_border);
        int kd = h - 6, kx = on ? (x + w - kd - 3) : (x + 3), ky = y + 3;
        if (g_style.shadows) gui_fill_circle_aa(handle, kx+1, ky+1, kd, gui_mix(tr, 0x00000000, 45), tr);
        gui_fill_circle_aa(handle, kx, ky, kd, 0x00FFFFFF, tr);
    }
}

void gui_slider(int handle, int x, int y, int w, int value, int max_val, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    (void)st;
    if (max_val <= 0) max_val = 1;
    int fillw = value * w / max_val; if (fillw < 0) fillw = 0; if (fillw > w) fillw = w;
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y+5, w, 6, p->track);
        if (fillw > 0) win_draw_rect(handle, x, y+5, fillw, 6, p->accent);
        int tx = x + fillw - 7; if (tx < x) tx = x;
        win_draw_rect(handle, tx, y, 14, 16, p->surface_raised);
        gui_draw_rect_outline(handle, tx, y, 14, 16, p->border);
    } else {
        int th = 4, ty = y + 6;
        gui_fill_rounded_aa(handle, x, ty, w, th, th/2, p->track, p->surface);
        if (fillw > 0) gui_fill_rounded_aa(handle, x, ty, fillw, th, th/2, p->accent, p->surface);
        int td = 14, tx = x + fillw - td/2;
        if (tx < x) tx = x; if (tx > x + w - td) tx = x + w - td;
        if (g_style.shadows) gui_fill_circle_aa(handle, tx+1, y+1, td, gui_mix(p->surface, 0x00000000, 45), p->surface);
        gui_fill_circle_aa(handle, tx, y, td, 0x00FFFFFF, p->surface);
        gui_rounded_border(handle, tx, y, td, td, td/2, gui_darken(p->accent, 20));
    }
}

void gui_textfield2(int handle, int x, int y, int w, int h, const char *text, bool focused) {
    gui_palette_t *p = gui_pal();
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, p->field_bg);
        win_draw_rect(handle, x, y, w, 1, gui_darken(p->surface,70));
        win_draw_rect(handle, x, y, 1, h, gui_darken(p->surface,70));
        win_draw_rect(handle, x, y+h-1, w, 1, gui_lighten(p->surface,80));
        win_draw_rect(handle, x+w-1, y, 1, h, gui_lighten(p->surface,80));
    } else {
        int r = (g_style.base == GUI_STYLE_MODERN) ? GUI_RADIUS : 0;
        gui_fill_rounded(handle, x, y, w, h, r, focused ? p->accent : p->field_border);
        gui_fill_rounded(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, p->field_bg);
    }
    if (text && *text) win_draw_text_ttf(handle, x+8, y + (h-GUI_TTF_SIZE)/2, text, GUI_TTF_SIZE, p->ink);
}

void gui_progress(int handle, int x, int y, int w, int h, int pct) {
    gui_palette_t *p = gui_pal();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r = (g_style.base == GUI_STYLE_MODERN) ? h/2 : 0;
    gui_fill_rounded_aa(handle, x, y, w, h, r, p->track, p->surface);
    int fw = w * pct / 100;
    if (fw > 0) gui_fill_rounded_aa(handle, x, y, fw, h, r, p->accent, p->surface);
}

void gui_card(int handle, int x, int y, int w, int h) {
    gui_palette_t *p = gui_pal();
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, p->surface_raised);
        win_draw_rect(handle, x, y, w, 1, gui_lighten(p->surface_raised,70));
        win_draw_rect(handle, x, y, 1, h, gui_lighten(p->surface_raised,70));
        win_draw_rect(handle, x, y+h-1, w, 1, gui_darken(p->surface_raised,60));
        win_draw_rect(handle, x+w-1, y, 1, h, gui_darken(p->surface_raised,60));
    } else {
        int r = (g_style.base == GUI_STYLE_MODERN) ? GUI_RADIUS : 0;
        if (g_style.base == GUI_STYLE_MODERN && g_style.shadows) gui_soft_shadow(handle, x, y, w, h, r, p->surface);
        gui_fill_rounded_aa(handle, x, y, w, h, r, p->border, p->surface);
        gui_fill_rounded(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, p->surface_raised);
    }
}
