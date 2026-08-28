// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui.c - GUI library implementation for MayteraOS user-space applications
// Provides high-level GUI wrappers around window syscalls
#include "gui.h"
#include "syscall.h"
#include "string.h"
#include "gui_scroll.h"   // gui_scroll_thumb_ink: ONE scrollbar contrast rule

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

    // Thumb. These were two hardcoded greys, which is theme-blind by
    // construction: 0x808080 on a dark theme's dark gutter is the same class
    // of invisible thumb that #745 item 77 fixed in the shared widget. This
    // predates gui_scroll.c and the installer still calls it, so it is routed
    // through the SAME rule rather than given its own second-best fix.
    uint32_t thumb_c = gui_scroll_thumb_ink(theme_color(THEME_COLOR_SCROLLBAR_THUMB),
                                            bg_color, bg_color);
    win_draw_rect(handle, x + 2, y + thumb_pos, scroll_width - 4, thumb_size, thumb_c);
    gui_draw_rect(handle, x + 2, y + thumb_pos, scroll_width - 4, thumb_size,
                  gui_scroll_thumb_ink(thumb_c, thumb_c, bg_color));
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
//
// REMOVED at #222. This block held a SECOND set of SYS_FB_* numbers:
//
//     #define SYS_FB_INFO 90 / SYS_FB_MAP 91 / SYS_FB_FLIP 92 / SYS_FB_EXCLUSIVE 93
//
// written BELOW the #include "syscall.h" above, which defines the real ones as
// SYS_FB_MAP 200, SYS_FB_INFO 201, SYS_FB_FLIP 202. The later definition wins,
// so the two functions that lived here did not call the framebuffer at all:
// 90 is SYS_DUP and 93 is SYS_FCNTL. fb_get_info() duplicated a file descriptor
// and reported the returned fd as width | height<<16 | pitch<<32;
// fb_set_exclusive() performed an fcntl. SYS_FB_EXCLUSIVE never existed in the
// kernel at any number.
//
// Nothing called either one, which is why it survived: userland/libc/gui.h does
// not declare them, and DOOM (i_video.c) carries its own static inline copies
// that use the correct 201 and SYS_GRAB_INPUT 213. So this was a latent trap,
// not a live fault - the first caller would have got SYS_DUP.
//
// They are DELETED rather than renumbered because the shared primitive already
// exists and is correct: syscall.h provides fb_map(), fb_info(fb_info_t *),
// fb_flip() and fb_damage() as static inlines against the real numbers. A
// third hand-rolled copy is the fault this ticket is about.

// ============================================================================
// Widget-Style Engine (Phase 0-2) - see gui_style.h
// ============================================================================
#include "gui_style.h"

// #711: GUI_RADIUS is a theme read now, not an integer constant expression,
// so the static initialiser carries the fallback and gui_set_style() /
// gui_style_sync_from_theme() replace it with the file value.
static ui_style_t   g_style = { GUI_STYLE_MODERN, GUI_RADIUS_FALLBACK, true, true };
// (#745) The FALLBACK palette, used by every GUI app that never calls
// gui_set_palette(). Its old focus/boundary story measured, on its own
// surface #252525: accent-as-focus-ring 2.89:1, border 1.90:1, track 1.48:1,
// all below the 3:1 non-text floor. focus #5C9BD6 (5.19:1) and edge_strong
// #7A7A7A (3.57:1) are stated here rather than left to the repair pass so the
// static initialiser is correct on its own terms and readable as a spec.
static gui_palette_t g_pal = {
    0x00252525, 0x002A2A2A, 0x00FFFFFF, 0x00AAAAAA,
    0x003A6EA5, 0x004A7EB5, 0x00505050, 0x00333333, 0x00505050, 0x00404040,
    0x005C9BD6, 0x007A7A7A
};

void gui_set_style(gui_base_style_t base) {
    g_style.base = base;
    if (base == GUI_STYLE_MODERN) { g_style.radius = GUI_RADIUS; g_style.gradients = true; g_style.shadows = true; }
    else { g_style.radius = 0; g_style.gradients = false; g_style.shadows = false; } // classic + flat
}

// #711: the family used to be a compiled choice with a compiled radius/gradient
// /shadow triple baked into gui_set_style() above. This reads the same three
// decisions out of the ACTIVE .mtheme file instead (decor.style, radius.card),
// so a theme can ask for beveled/flat/gradient chrome without an app rebuild.
// Shadows stay OFF unconditionally: window shadows were removed by explicit
// user decision (#189) and the renderer has no soft-shadow primitive, so the
// elevation model is (surface token, border token) pairs, never a shadow.
void gui_style_sync_from_theme(void) {
    int d = theme_metric(THEME_METRIC_DECOR_STYLE);
    g_style.base      = (d == THEME_DECOR_BEVELED) ? GUI_STYLE_CLASSIC
                      : (d == THEME_DECOR_FLAT)    ? GUI_STYLE_FLAT
                                                   : GUI_STYLE_MODERN;
    g_style.radius    = GUI_RADIUS;
    g_style.gradients = (d == THEME_DECOR_GRADIENT);
    g_style.shadows   = false;
}
ui_style_t gui_active_style(void) { return g_style; }
// (#745) A palette is DATA, most of it read out of a .mtheme file, and a theme
// file cannot be trusted to clear an accessibility floor: measured across the
// 14 shipped themes, color.focus_ring was below 3:1 against its own surface in
// 3 of them. So the engine REPAIRS rather than trusts, the same belt-and-braces
// shape the kernel already uses (theme_ensure_contrast() at load time, checked
// again at commit time by build/assets/theme-scale-lint.sh).
//
// focus and edge_strong are derived here and NEVER read from *p: see the
// comment on those fields in gui_style.h. The theme's authored ring is picked
// up by libc itself via theme_color(), not passed in by the caller, so no app
// has to be edited for this to take effect and no app can half-adopt it. An
// older kernel that does not know the id falls through to window_bg; that is
// detected below and replaced by accent before the repair runs, so the ring
// degrades to the app's own accent raised to floor, never to invisible.
void gui_set_palette(const gui_palette_t *p) {
    if (!p) return;
    g_pal = *p;
    uint32_t ring = theme_color(THEME_COLOR_FOCUS_RING);
    // An older kernel that does not know this id falls through to window_bg.
    // Detect that by the property that makes the answer useless rather than by
    // the floor: an echoed background is INDISTINGUISHABLE from the surface.
    // Testing the 3:1 floor here would have been wrong, because a theme is
    // allowed to author a weak ring (3 of the 14 shipped ones did) and the
    // repair below is what fixes those; throwing the hue away as well would
    // lose the theme's identity for no reason. Every one of the 14 shipped
    // rings clears 1.2:1 against its own surface, the weakest being Dark's
    // pre-#745 1.76:1.
    if (gui_contrast_x100(ring, g_pal.surface) < 120) ring = g_pal.accent;
    g_pal.focus = gui_ensure_contrast2(ring, g_pal.surface, g_pal.surface_raised,
                                       GUI_FLOOR_NONTEXT);
    g_pal.edge_strong = gui_ensure_contrast(g_pal.border, g_pal.surface,
                                            GUI_FLOOR_NONTEXT);
    // field_border is repaired IN PLACE rather than replaced: an input's fill
    // sits within ~1.3:1 of the surface on every dark theme, so the outline is
    // the whole boundary. p->border is deliberately left alone, because it also
    // draws dividers and hairlines where a quiet line is the correct design.
    g_pal.field_border = gui_ensure_contrast(g_pal.field_border, g_pal.surface,
                                             GUI_FLOOR_NONTEXT);
}
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

// --- (#745) WCAG contrast, integer-only. See gui_style.h for the contract. ---
// sRGB -> linear, scaled to 0..65535. Generated, not typed: see the generator
// in the #745 patch script. Table (512 bytes) not powf() because the freestanding
// libc has no float runtime it can rely on.
static const uint16_t gs_srgb_lin[256] = {
        0,    20,    40,    60,    80,    99,   119,   139,
      159,   179,   199,   219,   241,   264,   288,   313,
      340,   367,   396,   427,   458,   491,   526,   562,
      599,   637,   677,   718,   761,   805,   851,   898,
      947,   997,  1048,  1101,  1156,  1212,  1270,  1330,
     1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,
     1937,  2013,  2090,  2170,  2250,  2333,  2418,  2504,
     2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,
     3360,  3464,  3570,  3678,  3788,  3900,  4014,  4129,
     4247,  4366,  4488,  4611,  4736,  4864,  4993,  5124,
     5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,
     6395,  6547,  6700,  6856,  7014,  7174,  7335,  7500,
     7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,
     9072,  9258,  9445,  9635,  9828, 10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878,
    16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281,
    20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033,
    31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429,
    37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369,
    52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955,
    61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

uint32_t gui_luma_wcag(uint32_t rgb) {
    uint32_t r = gs_srgb_lin[(rgb >> 16) & 0xFF];
    uint32_t g = gs_srgb_lin[(rgb >> 8)  & 0xFF];
    uint32_t b = gs_srgb_lin[ rgb        & 0xFF];
    // 0.2126/0.7152/0.0722 in ten-thousandths. Max value 10000*65535 fits u32.
    return (2126u * r + 7152u * g + 722u * b) / 10000u;
}

// (L_hi + 0.05) / (L_lo + 0.05), x100. 0.05 * 65535 = 3277.
int gui_contrast_x100(uint32_t a, uint32_t b) {
    uint32_t la = gui_luma_wcag(a) + 3277u;
    uint32_t lb = gui_luma_wcag(b) + 3277u;
    if (la < lb) { uint32_t t = la; la = lb; lb = t; }
    return (int)((la * 100u) / lb);
}

// Below this background luminance, mixing toward white beats mixing toward
// black. Solving (L+0.05)^2 == 1.05*0.05 gives L = 0.1791 -> 11738/65535.
#define GS_WHITE_WINS 11738u

uint32_t gui_ensure_contrast(uint32_t fg, uint32_t bg, int min_x100) {
    if (gui_contrast_x100(fg, bg) >= min_x100) return fg;
    uint32_t target = (gui_luma_wcag(bg) < GS_WHITE_WINS) ? 0x00FFFFFF : 0x00000000;
    for (int t = 8; t < 256; t += 8) {
        uint32_t c = gui_mix(fg, target, t);
        if (gui_contrast_x100(c, bg) >= min_x100) return c;
    }
    return target;
}

uint32_t gui_ensure_contrast2(uint32_t fg, uint32_t bg1, uint32_t bg2, int min_x100) {
    int c1 = gui_contrast_x100(fg, bg1), c2 = gui_contrast_x100(fg, bg2);
    int worst = (c1 < c2) ? c1 : c2;
    if (worst >= min_x100) return fg;
    // Try both directions and keep the SMALLER mix that clears, so the theme's
    // hue is disturbed as little as the floor allows. If neither direction can
    // satisfy both backgrounds (they would have to straddle the whole range),
    // keep whichever candidate maximised the worse of the two.
    uint32_t best = fg; int best_worst = worst, best_t = 256; uint32_t best_clear = 0;
    for (int d = 0; d < 2; d++) {
        uint32_t target = d ? 0x00000000 : 0x00FFFFFF;
        for (int t = 8; t < 256; t += 8) {
            uint32_t k = gui_mix(fg, target, t);
            int k1 = gui_contrast_x100(k, bg1), k2 = gui_contrast_x100(k, bg2);
            int kw = (k1 < k2) ? k1 : k2;
            if (kw > best_worst) { best_worst = kw; best = k; }
            if (kw >= min_x100) { if (t < best_t) { best_t = t; best_clear = k; } break; }
        }
    }
    return best_t < 256 ? best_clear : best;
}

// (#117) Generic two-tone bevel pair. Walk `base` independently toward pure
// black (shadow) and pure white (highlight) until each side clears
// GUI_AIM_NONTEXT against `base` ITSELF (the colour the bevel sits on: the
// surrounding surface for a sunken well like a checkbox/textfield, or the
// element's own raised fill like a card/button), forcing the DIRECTION per
// side rather than letting gui_ensure_contrast() pick whichever is easier -
// a bevel needs one side pinned dark and the other pinned light regardless
// of which an automatic chooser would prefer.
//
// WHY THIS EXISTS (#117, following on from #96). gui_scroll_trough_bevel()
// was the first widget to get this treatment: #96 found the shared
// scrollbar trough at 1.00-1.42:1 against its surface on every shipped
// theme and fixed it by walking each side to a floor instead of a fixed
// offset. #96's own comment on that function already flagged that
// gui_checkbox() and gui_textfield2() used the SAME fixed darken(70)/
// lighten(80) magnitude the trough used to, measured BELOW the floor on
// every sampled theme - that finding is ticket #117. Auditing the rest of
// the shared widget library (userland/libc/gui.c) found the identical
// defect in gui_card() (lighten(70)/darken(60) of surface_raised) and
// gui_button()'s CLASSIC bevel (darken(40..55)/lighten(70) of the button's
// own fill). All four now call this ONE walk instead of each carrying its
// own fixed-offset copy, and gui_scroll_trough_bevel() itself is now a
// one-line wrapper around it so the scrollbar trough and every other
// bevelled widget share one repair instead of five near-identical copies.
//
// *cleared (returned via the two _ok flags at the call site) is 0 only when
// even full saturation (pure black/white) cannot clear the floor, which is
// only possible when `base` is already at that extreme (gui_style.h's own
// guarantee: at least one of pure white or pure black always reaches
// 4.58:1 against any background, so at most one direction can ever fail).
static uint32_t gs_bevel_walk(uint32_t from, uint32_t bg, int min_x100, int to_white, int *cleared) {
    uint32_t target = to_white ? 0x00FFFFFF : 0x00000000;
    if (gui_contrast_x100(from, bg) >= min_x100) { *cleared = 1; return from; }
    for (int t = 8; t < 256; t += 8) {
        uint32_t c = gui_mix(from, target, t);
        if (gui_contrast_x100(c, bg) >= min_x100) { *cleared = 1; return c; }
    }
    *cleared = (gui_contrast_x100(target, bg) >= min_x100);
    return target;
}

void gui_bevel_pair(uint32_t base, uint32_t *shadow_out, uint32_t *highlight_out) {
    int dark_ok, light_ok;
    uint32_t dark  = gs_bevel_walk(base, base, GUI_AIM_NONTEXT, 0, &dark_ok);
    uint32_t light = gs_bevel_walk(base, base, GUI_AIM_NONTEXT, 1, &light_ok);
    // Both directions saturating without clearing cannot happen (see the
    // comment above), so at least one of the two `_ok` flags is set and the
    // fallback below always has something to fall back TO.
    if (shadow_out)    *shadow_out    = dark_ok  ? dark  : light;
    if (highlight_out) *highlight_out = light_ok ? light : dark;
}

static int gs_isqrt(int n) { if (n<=0) return 0; int x=n, y=(x+1)/2; while (y<x){ x=y; y=(x+n/x)/2; } return x; }
static int gs_corner_inset(int r, int d) {
    if (r <= 0) return 0;
    int dy = r - 1 - d;
    int dx = r - gs_isqrt(r*r - dy*dy);
    return dx < 0 ? 0 : dx;
}
// #306: exported (was gs_line, file-static). Any arbitrary-angle stroke is a
// primitive every app needs (gui_checkbox's tick was the only caller and had
// no way to hand it out), so this is promoted into the shared API in
// gui_style.h rather than forking a private Bresenham copy into a new app.
// gs_line kept as a macro alias so the two pre-existing in-file callers below
// do not need touching.
void gui_line(int handle, int x0, int y0, int x1, int y1, uint32_t col) {
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
#define gs_line gui_line

// Draw an arbitrary-angle stroke with approximate thickness (odd px, small).
// Bundles gui_line() copies offset by -(t/2)..+(t/2) in BOTH axes (a small
// "plus" of parallel copies) so the visual thickness holds up at any angle,
// unlike offsetting a single axis (which thins out near 45 degrees). Used by
// the installer's done-screen check/X result glyph (#306), which is drawn as
// real strokes rather than a font character so it is guaranteed present at
// size regardless of font coverage (see docs/INSTALLER_UI_DESIGN.html 4/9).
void gui_thick_line(int handle, int x0, int y0, int x1, int y1, int thickness, uint32_t col) {
    int half = thickness / 2;
    for (int oy = -half; oy <= half; oy++)
        gui_line(handle, x0, y0 + oy, x1, y1 + oy, col);
    for (int ox = -half; ox <= half; ox++)
        gui_line(handle, x0 + ox, y0, x1 + ox, y1, col);
}

int gui_ttf_width(const char *s, int size) {
    if (!s || !*s) return 0;
    int w = ttf_measure(s, size);
    if (w <= 0) { int n=0; while (s[n]) n++; w = n * (size*6/10); }
    return w;
}

// #B3 (UI/UX): width EXACTLY as win_draw_text_ttf() will actually draw it -
// each glyph's own advance, default face/style, with NO inter-glyph kerning.
//
// gui_ttf_width() above (and the raw ttf_measure() syscall it wraps) goes
// through the kernel's ttf_measure_string(), which DOES sum kerning between
// every adjacent pair. win_draw_text_ttf()'s render loop (sys_win_draw_text_ttf
// in kernel/gui/ttf.c / proc/syscall.c) does NOT apply kerning at all - it
// just walks cx += glyph->advance. That is a genuine mismatch between the
// kernel's two TTF code paths (measure vs. draw), and since most kerning
// pairs are negative (tightening), the measured width UNDERSHOOTS the actual
// rendered spread: any label centered with gui_ttf_width() as the reference
// width ends up drawn wider than predicted, drifting right of true center
// (this is what made the App Store's type-filter pills look badly
// off-center - see #B3 report). The correct long-term fix is in the kernel
// (either make the renderer apply kerning too, or drop kerning from
// ttf_measure_string to match the renderer); until then this is the
// userland-only workaround, matching the renderer glyph-for-glyph via the
// font_glyph() syscall (returns the same stbtt advance win_draw_text_ttf
// uses internally).
//
// COST: one syscall per glyph, vs. gui_ttf_width()'s single whole-string
// syscall - fine for the short button/pill/tab labels this is meant for
// (a handful of glyphs), but NOT a drop-in replacement for gui_ttf_width()
// in a hot wrap/truncate loop over long strings (trunc_fit-style code
// should keep using gui_ttf_width()).
int gui_ttf_render_width(const char *s, int size) {
    // #575: the kernel renderer (sys_win_draw_text_ttf, behind win_draw_text_ttf)
    // now applies inter-glyph kerning exactly like ttf_measure_string(), so the
    // kerning-inclusive gui_ttf_width() once again matches the renderer
    // glyph-for-glyph. The old per-glyph, kerning-free #B3 workaround is obsolete;
    // defer to the single-syscall gui_ttf_width() (one syscall, not one-per-glyph).
    return gui_ttf_width(s, size);
}

// #204: word-wrap `body` into up to `max_lines` lines that each fit `max_w`
// pixels at TTF size `size`, measured with the REAL glyph metrics
// (gui_ttf_width) - a proportional TTF face has no fixed char width, so
// guessing "N chars per line" is wrong for any string with wide glyphs.
// Breaks at the last space that still fits; a single word wider than max_w
// is hard-broken so it can never leave the box. Any text left over once
// max_lines is reached is dropped and the last line ellipsized (real-
// measured, not a fixed suffix count) so it still fits. Verbatim port of
// the compositor's notif.c wrap_text_ttf() (#762) - same algorithm, a
// second copy only because gui_confirm_open() (this file's caller) runs in
// an app process, not the compositor, and has no text_width_ttf() to call.
int gui_wrap_text_ttf(const char *body, int size, int max_w, int max_lines,
                      char out[][GUI_WRAP_COL]) {
    int nlines = 0;
    const char *p = body;
    while (*p && nlines < max_lines) {
        int len = 0, last_space = -1;
        char probe[GUI_WRAP_COL];
        while (p[len]) {
            if (p[len] == ' ' && len > 0) last_space = len;
            int pl = len + 1; if (pl > GUI_WRAP_COL - 1) pl = GUI_WRAP_COL - 1;
            memcpy(probe, p, pl); probe[pl] = 0;
            if (gui_ttf_width(probe, size) > max_w) break;
            if (pl >= GUI_WRAP_COL - 1) { len = pl; break; }
            len++;
        }
        int cut;
        if (!p[len]) {
            cut = len;                 // rest of the string fits on this line
        } else if (last_space > 0) {
            cut = last_space;          // break at the last word boundary that fit
        } else {
            cut = (len > 0) ? len : 1; // single word wider than max_w: hard-break it
        }
        int cl = cut; if (cl > GUI_WRAP_COL - 1) cl = GUI_WRAP_COL - 1;
        memcpy(out[nlines], p, cl); out[nlines][cl] = 0;
        nlines++;
        p += cut;
        while (*p == ' ') p++;
    }
    if (*p && nlines > 0) {
        // Text remains beyond max_lines: ellipsize the last line, measuring
        // the actual "..." glyph width rather than assuming a fixed suffix.
        char *last = out[nlines - 1];
        int ellw = gui_ttf_width("...", size);
        int l = (int)strlen(last);
        while (l > 0 && gui_ttf_width(last, size) + ellw > max_w) last[--l] = 0;
        if (l + 3 < GUI_WRAP_COL) { last[l]='.'; last[l+1]='.'; last[l+2]='.'; last[l+3]=0; }
    }
    if (nlines < 1) { nlines = 1; out[0][0] = 0; }
    return nlines;
}

void gui_text_ttf_centered(int handle, int x, int y, int w, int h,
                           const char *s, uint32_t color, int size) {
    // #B3: center using the RENDERED width (see gui_ttf_render_width above),
    // not the kerning-inclusive gui_ttf_width(), so the glyphs actually land
    // centered instead of drifting toward one edge.
    int tw = gui_ttf_render_width(s, size);
    int tx = x + (w - tw)/2; if (tx < x) tx = x;
    // Vertical centering: win_draw_text_ttf's y is the TOP of the text line
    // (baseline = y + ascent); the true line height is ascent - descent
    // (descent negative), which stbtt's pixel-height scale keeps close to
    // `size` but not exact. Ask the real metrics instead of assuming the
    // line is exactly `size` px tall, so short numeral/label strings land in
    // the visual middle of the box instead of a few px high or low.
    int asc = size, desc = 0, gap = 0;
    int mret[3] = { size, 0, 0 };
    if (font_metrics(0, size, mret) == 0) { asc = mret[0]; desc = mret[1]; gap = mret[2]; }
    (void)gap;
    int line_h = asc - desc;
    if (line_h <= 0) line_h = size;
    int ty = y + (h - line_h) / 2; if (ty < y) ty = y;
    win_draw_text_ttf(handle, tx, ty, s, size, color);
}

// (#307 follow-up) See gui_style.h for why this lives here and not as a
// private static in one app. Rows/columns of width 7,5,3,1 from base to apex,
// centred on (cx, cy). 7 is odd and a chevron is usually centred in an
// even-sized box, so one side keeps a 1px larger margin; that is inherent to
// an odd-width mark, not a centring error.
void gui_chevron(int handle, int cx, int cy, int dir, uint32_t col) {
    for (int r = 0; r < 4; r++) {
        int w = 7 - r * 2;                 // 7, 5, 3, 1 - base first, apex last
        if (w < 1) w = 1;
        switch (dir) {
        case GUI_CHEV_DOWN:                // base at top, apex at bottom
            win_draw_rect(handle, cx - w / 2, cy - 2 + r, w, 1, col); break;
        case GUI_CHEV_UP:                  // base at bottom, apex at top
            win_draw_rect(handle, cx - w / 2, cy + 1 - r, w, 1, col); break;
        case GUI_CHEV_RIGHT:               // base at left, apex at right
            win_draw_rect(handle, cx - 2 + r, cy - w / 2, 1, w, col); break;
        default:                           // GUI_CHEV_LEFT
            win_draw_rect(handle, cx + 1 - r, cy - w / 2, 1, w, col); break;
        }
    }
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

// --- Star rating icon (#B2 app store: shared primitive so no app hand-rolls
// its own star shape) -------------------------------------------------------
// A regular 5-point star polygon, described once as 10 unit-circle direction
// vectors (outer point / inner notch, alternating), scaled to the caller's
// radius. Point-in-polygon uses the standard even-odd ray-casting test; edges
// get a cheap 2x2 supersample so small icon sizes (10-24px, as used for
// ratings) don't look jagged without the cost of the 4x4 grid the rounded-rect
// helpers above use for much larger shapes.
static const int GS_STAR_OX[5] = { 0, 951, 588, -588, -951 };   // outer, angles -90,-18,54,126,198
static const int GS_STAR_OY[5] = { -1000, -309, 809, 809, -309 };
static const int GS_STAR_IX[5] = { 588, 951, 0, -951, -588 };   // inner, angles -54,18,90,162,234
static const int GS_STAR_IY[5] = { -809, 309, 1000, 309, -809 };

static int gs_star_inside(int fx, int fy, int cx, int cy, int outerR, int innerR) {
    int vx[10], vy[10];
    for (int k = 0; k < 5; k++) {
        vx[2*k]     = cx + GS_STAR_OX[k] * outerR / 1000;
        vy[2*k]     = cy + GS_STAR_OY[k] * outerR / 1000;
        vx[2*k + 1] = cx + GS_STAR_IX[k] * innerR / 1000;
        vy[2*k + 1] = cy + GS_STAR_IY[k] * innerR / 1000;
    }
    int inside = 0;
    for (int i = 0, j = 9; i < 10; j = i++) {
        if (((vy[i] > fy) != (vy[j] > fy)) &&
            (fx < (int)((long)(vx[j] - vx[i]) * (fy - vy[i]) / (vy[j] - vy[i]) + vx[i])))
            inside = !inside;
    }
    return inside;
}

// Draw one star icon in a d x d box. fill_pct (0..100) is the fraction of the
// star's WIDTH (left to right) painted with fill_color; the rest gets
// empty_color. Callers building a 1..5 rating row pass 100 for a fully-earned
// star, 0 for an unearned one, and an in-between value for the star that
// straddles a fractional average (e.g. a 4.3 average: stars 1-4 at 100, star
// 5 at 30) - so partial stars read correctly instead of only whole/empty.
void gui_fill_star_aa(int handle, int x, int y, int d, int fill_pct,
                      uint32_t fill_color, uint32_t empty_color, uint32_t bg) {
    if (d <= 0) return;
    if (fill_pct < 0) fill_pct = 0;
    if (fill_pct > 100) fill_pct = 100;
    int cx = x + d / 2, cy = y + d / 2;
    int outerR = d / 2;
    int innerR = outerR * 45 / 100;
    int fillx = x + d * fill_pct / 100;
    for (int j = 0; j < d; j++) {
        int py = y + j;
        for (int i = 0; i < d; i++) {
            int px = x + i;
            int cnt = 0;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++)
                    cnt += gs_star_inside(px * 2 + sx, py * 2 + sy, cx * 2, cy * 2, outerR * 2, innerR * 2);
            if (cnt == 0) continue;
            uint32_t col = (px < fillx) ? fill_color : empty_color;
            if (cnt >= 4) win_draw_pixel(handle, px, py, col);
            else win_draw_pixel(handle, px, py, gui_mix(bg, col, cnt * 255 / 4));
        }
    }
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
    // (#117) bord used to be a FIXED gui_darken(x,40) regardless of what
    // p->surface actually was, which on a dark theme (base already close to
    // black) barely moves and can land below GUI_FLOOR_NONTEXT against the
    // surface the button sits on. gui_ensure_contrast() keeps the darkened
    // hue as its starting point and only walks further if that starting
    // point does not already clear the floor, so most themes are unchanged.
    if (variant == GUI_BTN_PRIMARY)      { base = p->accent; ink = gui_ink_on(p->accent); bord = gui_ensure_contrast(gui_darken(p->accent, 40), p->surface, GUI_FLOOR_NONTEXT); }
    else if (variant == GUI_BTN_GHOST)   { base = p->surface; ink = p->accent; bord = p->edge_strong; }
    else if (variant == GUI_BTN_SUCCESS) { base = 0x003FA34D; ink = gui_ink_on(0x003FA34D); bord = gui_ensure_contrast(gui_darken(0x003FA34D, 40), p->surface, GUI_FLOOR_NONTEXT); }
    // (#appstyle) DANGER reads the theme's own error token rather than pinning
    // a literal the way SUCCESS above has to: every shipped .mtheme defines
    // color_error and they differ on purpose. theme_color() answering 0 (an
    // older kernel, or a theme with the key missing) would render an invisible
    // black button, so a red fallback is applied on exactly that condition -
    // this is a "0 means unset" test on a colour that no theme would ever
    // legitimately author as pure black for an error state.
    else if (variant == GUI_BTN_DANGER)  { base = theme_color(THEME_COLOR_ERROR); if (!base) base = 0x00CC0000;
                                           ink = gui_ink_on(base); bord = gui_ensure_contrast(gui_darken(base, 40), p->surface, GUI_FLOOR_NONTEXT); }
    else                                 { base = gui_mix(p->surface_raised, p->ink, 8); ink = p->ink; bord = p->edge_strong; }
    if (!disabled) {
        if (st == GUI_ST_HOVER)        base = (variant==GUI_BTN_PRIMARY) ? p->accent_hover : gui_lighten(base, 18);
        else if (st == GUI_ST_PRESSED) base = gui_darken(base, 18);
    } else {
        base = gui_mix(base, p->surface, 150);
        ink  = gui_mix(ink,  p->surface, 110);
        // (#appstyle) ...AND THEN GUARANTEE THE DISABLED LABEL IS STILL
        // READABLE. Both lines above walk toward p->surface by a FIXED amount,
        // which works for a SECONDARY button (whose ink was already the surface
        // ink) and fails for every saturated variant, because their ink is
        // gui_ink_on(base) - white on a red or teal fill. Mixing white 110/255
        // toward a light surface leaves white-on-pale: MEASURED on the
        // installer's disabled "Erase and install" (GUI_BTN_DANGER on
        // retro_unix) the label was invisible, and disabled PRIMARY had the
        // same fault everywhere it appears.
        //
        // The floor is the 3:1 NON-TEXT one, not the 4.5:1 text one, and that
        // is deliberate: WCAG 1.4.3 exempts disabled controls, and lifting them
        // to full text contrast would delete the visual difference between
        // "you can press this" and "you cannot", which is the whole point of
        // the state. 3:1 keeps the label clearly de-emphasised AND readable,
        // which matters most on exactly the screens where a disabled button is
        // telling the user what they have not done yet.
        ink = gui_ensure_contrast(ink, base, GUI_FLOOR_NONTEXT);
    }

    if (g_style.base == GUI_STYLE_CLASSIC) {
        bool pressed = (st == GUI_ST_PRESSED);
        win_draw_rect(handle, x, y, w, h, base);
        // (#117) was a fixed darken(40/55)/lighten(70) of `base`, the same
        // defect class as the checkbox/textfield/card bevels below: see
        // gui_bevel_pair()'s comment. shadow/highlight are `base` walked to
        // GUI_AIM_NONTEXT against `base` itself; not-pressed shows the raised
        // look (highlight top/left), pressed shows the sunken press feedback
        // (shadow top/left), same swap the fixed-magnitude code did.
        uint32_t shadow, highlight;
        gui_bevel_pair(base, &shadow, &highlight);
        uint32_t lt = pressed ? shadow : highlight;
        uint32_t dk = pressed ? highlight : shadow;
        win_draw_rect(handle, x, y, w, 2, lt);
        win_draw_rect(handle, x, y, 2, h, lt);
        win_draw_rect(handle, x, y+h-2, w, 2, dk);
        win_draw_rect(handle, x+w-2, y, 2, h, dk);
    } else if (g_style.base == GUI_STYLE_FLAT) {
        win_draw_rect(handle, x, y, w, h, base);
        gui_draw_rect_outline(handle, x, y, w, h, bord);
    } else { // modern
        // #612: square-edge buttons (GUI_BTN_RADIUS == 0), not g_style.radius.
        // The old h/2 capsule blended its AA edge toward p->surface regardless
        // of what the button actually sat on (e.g. a coloured card or a hero
        // banner), which is exactly the "stray white pixels around the
        // curves" fringe reported against the App Store's Get button - a
        // shared-engine bug, not an app-local one, since every app using
        // gui_button() inherited it. r=0 takes gui_fill_rounded_aa's exact
        // win_draw_rect fallback, so there is no blend left to get wrong.
        int r = GUI_BTN_RADIUS;
        if (g_style.shadows && !disabled) gui_soft_shadow(handle, x, y, w, h, r, p->surface);
        gui_fill_rounded_aa(handle, x, y, w, h, r, bord, p->surface);
        uint32_t top = gui_lighten(base, g_style.gradients ? 16 : 0);
        uint32_t bot = gui_darken(base,  g_style.gradients ? 10 : 0);
        gui_fill_rounded_grad(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, top, bot);
    }
    if (st == GUI_ST_FOCUS) {
        // (#745) p->focus, not p->accent. accent stays the SELECTION/VALUE
        // colour (primary fill, checked box, toggle-on, slider and progress
        // fill); focus is the KEYBOARD POSITION colour and is the one that has
        // to clear 3:1, because on this OS pointer input is unreliable (#334)
        // and the keyboard is the primary path. Sharing one colour for both
        // meanings is what made the ring 2.89:1 on the default surface.
        if (g_style.base == GUI_STYLE_MODERN) gui_rounded_border(handle, x, y, w, h, GUI_BTN_RADIUS, p->focus);
        else gui_draw_rect_outline(handle, x, y, w, h, p->focus);
    }
    if (label && *label) gui_text_ttf_centered(handle, x, y, w, h, label, ink, GUI_TTF_SIZE);
}

void gui_checkbox(int handle, int x, int y, int sz, bool checked,
                  const char *label, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    bool disabled = (st == GUI_ST_DISABLED);
    uint32_t boxbg = checked ? p->accent : p->field_bg;
    // (#117) checked-ring border used to be a fixed gui_darken(accent,30);
    // gui_ensure_contrast() keeps that as its starting point and only walks
    // further where it does not already clear the floor against p->surface
    // (the ring is drawn by gui_fill_rounded_aa below, blended toward
    // p->surface at its AA edge, so that is the background that matters).
    uint32_t bord  = checked ? gui_ensure_contrast(gui_darken(p->accent,30), p->surface, GUI_FLOOR_NONTEXT) : p->field_border;
    if (disabled) boxbg = gui_mix(boxbg, p->surface, 130);

    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, sz, sz, checked ? p->accent : 0x00FFFFFF);
        // (#117) was a fixed darken(70)/lighten(80) of p->surface - the
        // exact defect this ticket is about (measured 1.82:1 on retro_unix).
        // gui_bevel_pair() walks each side to GUI_AIM_NONTEXT instead.
        uint32_t shadow, hi;
        gui_bevel_pair(p->surface, &shadow, &hi);
        win_draw_rect(handle, x, y, sz, 1, shadow);
        win_draw_rect(handle, x, y, 1, sz, shadow);
        win_draw_rect(handle, x, y+sz-1, sz, 1, hi);
        win_draw_rect(handle, x+sz-1, y, 1, sz, hi);
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
    // (#237) st used to be entirely ignored ((void)st;) - gui_toggle() was the
    // one shared control with no GUI_ST_DISABLED treatment at all, discovered
    // while giving the Wi-Fi/Bluetooth panels an inert power switch (a real
    // adapter detected with no driver to back it, #237). Faded the same way
    // gui_button()/gui_checkbox() already fade toward p->surface, so a
    // disabled toggle now actually reads as disabled instead of looking live.
    bool disabled = (st == GUI_ST_DISABLED);
    uint32_t tr = on ? p->accent : p->field_bg;
    if (disabled) tr = gui_mix(tr, p->surface, 140);
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, tr);
        // (#117) was a fixed gui_darken(accent,30); repaired against
        // p->surface (the panel the toggle sits on) the same way as the
        // checkbox's checked-ring border above.
        uint32_t bord = on ? gui_ensure_contrast(gui_darken(p->accent,30), p->surface, GUI_FLOOR_NONTEXT) : p->field_border;
        if (disabled) bord = gui_mix(bord, p->surface, 140);
        gui_draw_rect_outline(handle, x, y, w, h, bord);
        int kx = on ? (x + w - (h-4) - 2) : (x + 2);
        uint32_t knob = disabled ? gui_mix(p->surface_raised, p->surface, 140) : p->surface_raised;
        uint32_t kbord = disabled ? gui_mix(p->border, p->surface, 140) : p->border;
        win_draw_rect(handle, kx, y+2, h-4, h-4, knob);
        gui_draw_rect_outline(handle, kx, y+2, h-4, h-4, kbord);
    } else {
        int r = h/2;
        gui_fill_rounded_aa(handle, x, y, w, h, r, tr, p->surface);
        if (!on) gui_rounded_border(handle, x, y, w, h, r, disabled ? gui_mix(p->field_border, p->surface, 140) : p->field_border);
        int kd = h - 6, kx = on ? (x + w - kd - 3) : (x + 3), ky = y + 3;
        if (g_style.shadows && !disabled) gui_fill_circle_aa(handle, kx+1, ky+1, kd, gui_mix(tr, 0x00000000, 45), tr);
        gui_fill_circle_aa(handle, kx, ky, kd, 0x00FFFFFF, tr);
    }
}

void gui_slider(int handle, int x, int y, int w, int value, int max_val, gui_state_t st) {
    gui_palette_t *p = gui_pal();
    (void)st;
    if (max_val <= 0) max_val = 1;
    int fillw = value * w / max_val; if (fillw < 0) fillw = 0; if (fillw > w) fillw = w;
    if (g_style.base == GUI_STYLE_CLASSIC) {
        gui_draw_rect_outline(handle, x, y+5, w, 6, p->edge_strong);
        win_draw_rect(handle, x+1, y+6, w-2, 4, p->track);
        if (fillw > 2) win_draw_rect(handle, x+1, y+6, fillw-2, 4, p->accent);
        int tx = x + fillw - 7; if (tx < x) tx = x;
        win_draw_rect(handle, tx, y, 14, 16, p->surface_raised);
        gui_draw_rect_outline(handle, tx, y, 14, 16, p->border);
    } else {
        // 4px -> 6px so a 1px boundary ring still leaves a readable 4px well.
        // Same vertical centre (y+8) as before, so the thumb still lines up.
        int th = 6, ty = y + 5;
        gui_fill_rounded_aa(handle, x, ty, w, th, th/2, p->edge_strong, p->surface);
        gui_fill_rounded(handle, x+1, ty+1, w-2, th-2, (th-2)/2, p->track);
        if (fillw > 2) gui_fill_rounded(handle, x+1, ty+1, fillw-2, th-2, (th-2)/2, p->accent);
        int td = 14, tx = x + fillw - td/2;
        if (tx < x) tx = x; if (tx > x + w - td) tx = x + w - td;
        if (g_style.shadows) gui_fill_circle_aa(handle, tx+1, y+1, td, gui_mix(p->surface, 0x00000000, 45), p->surface);
        gui_fill_circle_aa(handle, tx, y, td, 0x00FFFFFF, p->surface);
        // (#117) was a fixed gui_darken(accent,20). The ring sits between the
        // thumb's own pure-white fill (inside) and p->surface (outside, via
        // the AA blend above), so it has to clear the floor against BOTH -
        // gui_ensure_contrast2() is the same two-background repair
        // gui_set_palette() already uses for the focus ring.
        gui_rounded_border(handle, tx, y, td, td, td/2, gui_ensure_contrast2(gui_darken(p->accent, 20), 0x00FFFFFF, p->surface, GUI_FLOOR_NONTEXT));
    }
}

void gui_textfield2(int handle, int x, int y, int w, int h, const char *text, bool focused) {
    gui_palette_t *p = gui_pal();
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, p->field_bg);
        // (#117) NAMED IN THE TICKET: was a fixed darken(70)/lighten(80) of
        // p->surface, measured 1.27:1 on retro_unix. See gui_bevel_pair().
        uint32_t shadow, hi;
        gui_bevel_pair(p->surface, &shadow, &hi);
        win_draw_rect(handle, x, y, w, 1, shadow);
        win_draw_rect(handle, x, y, 1, h, shadow);
        win_draw_rect(handle, x, y+h-1, w, 1, hi);
        win_draw_rect(handle, x+w-1, y, 1, h, hi);
    } else {
        int r = (g_style.base == GUI_STYLE_MODERN) ? GUI_RADIUS : 0;
        gui_fill_rounded(handle, x, y, w, h, r, focused ? p->focus : p->field_border);
        gui_fill_rounded(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, p->field_bg);
    }
    if (text && *text) win_draw_text_ttf(handle, x+8, y + (h-GUI_TTF_SIZE)/2, text, GUI_TTF_SIZE, p->ink);
}

// (#appstyle) See gui_style.h for why this exists and why it takes primitives.
// The chrome is gui_textfield2()'s, verbatim, so a plain field and a
// caret-aware field are the same control in every theme; only the contents
// differ.
void gui_textfield_tf(int handle, int x, int y, int w, int h,
                      const char *text, int len, int cursor, int sel_anchor,
                      bool focused, const char *placeholder) {
    gui_palette_t *p = gui_pal();
    int size = GUI_TTF_SIZE;
    gui_textfield2(handle, x, y, w, h, 0, focused);   // chrome only, no text

    int tx = x + 8;
    int ty = y + (h - size) / 2;
    if (len < 0) len = 0;
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;

    if (!text || len == 0) {
        if (placeholder && *placeholder) {
            // The placeholder must clear the TEXT floor against the field, not
            // merely look lighter than the real ink: a "hint" nobody can read
            // is a blank box with extra steps.
            uint32_t ph = gui_ensure_contrast(p->ink_dim, p->field_bg, GUI_FLOOR_TEXT);
            win_draw_text_ttf(handle, tx, ty, placeholder, size, ph);
        }
    } else {
        // Selection highlight FIRST, so the ink lands on top of it. Both edges
        // are measured with gui_ttf_render_width() against a NUL-terminated
        // prefix copy, because that is the only width function documented to
        // agree with win_draw_text_ttf() below.
        if (sel_anchor >= 0 && sel_anchor != cursor) {
            int lo = sel_anchor < cursor ? sel_anchor : cursor;
            int hi = sel_anchor < cursor ? cursor : sel_anchor;
            if (lo < 0) lo = 0;
            if (hi > len) hi = len;
            char pre[512];
            int n = lo < (int)sizeof(pre) - 1 ? lo : (int)sizeof(pre) - 1;
            for (int i = 0; i < n; i++) pre[i] = text[i];
            pre[n] = 0;
            int sx = gui_ttf_render_width(pre, size);
            n = hi < (int)sizeof(pre) - 1 ? hi : (int)sizeof(pre) - 1;
            for (int i = 0; i < n; i++) pre[i] = text[i];
            pre[n] = 0;
            int ex = gui_ttf_render_width(pre, size);
            if (ex > sx) win_draw_rect(handle, tx + sx, ty, ex - sx, size, p->accent);
        }
        win_draw_text_ttf(handle, tx, ty, text, size, p->ink);
    }

    if (focused) {
        char pre[512];
        int n = cursor < (int)sizeof(pre) - 1 ? cursor : (int)sizeof(pre) - 1;
        for (int i = 0; i < n; i++) pre[i] = text ? text[i] : 0;
        pre[n] = 0;
        int cx = tx + (text ? gui_ttf_render_width(pre, size) : 0);
        // Caret is a full line-box bar, not a size-tall one: a 12px bar beside
        // a 14-unit glyph run reads as a stray tick.
        win_draw_rect(handle, cx, ty - 1, 1, size + 2, p->ink);
    }
}

void gui_progress(int handle, int x, int y, int w, int h, int pct) {
    gui_palette_t *p = gui_pal();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r = (g_style.base == GUI_STYLE_MODERN) ? h/2 : 0;
    // (#745) track #404040 on surface #252525 measured 1.48:1, so at 0% the
    // control had no visible extent at all. The well keeps its quiet fill and
    // gains a 1px edge_strong boundary, which is the thing the 3:1 floor is
    // actually about.
    gui_fill_rounded_aa(handle, x, y, w, h, r, p->edge_strong, p->surface);
    gui_fill_rounded(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, p->track);
    int fw = (w-2) * pct / 100;
    if (fw > 0) gui_fill_rounded(handle, x+1, y+1, fw, h-2, r>0?r-1:0, p->accent);
}

void gui_card(int handle, int x, int y, int w, int h) {
    gui_palette_t *p = gui_pal();
    if (g_style.base == GUI_STYLE_CLASSIC) {
        win_draw_rect(handle, x, y, w, h, p->surface_raised);
        // (#117) was a fixed lighten(70)/darken(60) of p->surface_raised,
        // the same defect class as gui_checkbox/gui_textfield2 above (a
        // raised bevel rather than a sunken one: highlight top/left, shadow
        // bottom/right, derived from the card's OWN fill rather than the
        // surface it sits on, same as gui_button's CLASSIC bevel).
        uint32_t shadow, hi;
        gui_bevel_pair(p->surface_raised, &shadow, &hi);
        win_draw_rect(handle, x, y, w, 1, hi);
        win_draw_rect(handle, x, y, 1, h, hi);
        win_draw_rect(handle, x, y+h-1, w, 1, shadow);
        win_draw_rect(handle, x+w-1, y, 1, h, shadow);
    } else {
        int r = (g_style.base == GUI_STYLE_MODERN) ? GUI_RADIUS : 0;
        if (g_style.base == GUI_STYLE_MODERN && g_style.shadows) gui_soft_shadow(handle, x, y, w, h, r, p->surface);
        gui_fill_rounded_aa(handle, x, y, w, h, r, p->edge_strong, p->surface);
        gui_fill_rounded(handle, x+1, y+1, w-2, h-2, r>0?r-1:0, p->surface_raised);
    }
}
