// launcher.c - MayteraOS command launcher (macOS-Spotlight style AI prompt)
//
// A centered, rounded, floating prompt panel opened from the Maytera-logo button
// next to the taskbar Start button. The user types a natural-language request
// (or picks a seeded example with the arrow keys) and presses Enter; the prompt
// is handed as a ONE-SHOT request to the existing AI backend by reusing the AI
// Chat dock (/APPS/aichat) hand-off path: we write the prompt to
// /CONFIG/AIASK.TXT and (re)enable the dock app, which consumes the file, pops
// its panel open, and runs the prompt through the SAME ReAct tool loop
// (aiclient_run_turn, #292/#327) that the chat input box uses. We do NOT add a
// second AI backend here; the launcher is purely the entry UI + the hand-off.
//
// The overlay is fully event-driven (opened/closed by a click or a key; while
// open the compositor is already in its interactive full-frame path, so the
// caret blinks without any busy-wait, per CLAUDE.md #426). Styling reuses the
// shared start-menu palette so it tracks the active theme (Midnight, etc.).

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/notify.h"

// From main.c: (re)launch / enable the AI Chat dock app that fulfils the prompt.
void aichat_set_enabled(int on);

bool g_launcher_open = false;

#define LAUNCH_MAX_INPUT  240
static char s_input[LAUNCH_MAX_INPUT + 1];
static int  s_len = 0;

// Seeded example prompts (autocomplete suggestions). Arrow keys move the
// highlight; Enter runs the highlighted example (or the typed text). These are
// exactly the examples requested for the first release.
static const char *s_examples[] = {
    "Build me a new widget that shows a feed from thehackernews.com",
    "Make a screensaver of an aquarium with African cichlids",
    "Open my photos in the studio",
    "Let's get my game controller working",
};
#define N_EXAMPLES ((int)(sizeof(s_examples) / sizeof(s_examples[0])))

// Highlighted suggestion: -1 = none (editing the field), 0..N-1 = an example.
static int s_sel = -1;

// Cached panel geometry (recomputed each render) for hit-testing the rows.
static int s_panel_x, s_panel_y, s_panel_w, s_panel_h;
static int s_field_x, s_field_y, s_field_w, s_field_h;
static int s_row_y0, s_row_h;

// ---------------------------------------------------------------------------
// Open / close / toggle
// ---------------------------------------------------------------------------
void launcher_open(void) {
    g_launcher_open = true;
    s_len = 0; s_input[0] = 0;
    s_sel = -1;
    g_needs_redraw = true;
}

void launcher_close(void) {
    if (!g_launcher_open) return;
    g_launcher_open = false;
    g_needs_redraw = true;
}

void launcher_toggle(void) { if (g_launcher_open) launcher_close(); else launcher_open(); }

int launcher_is_open(void) { return g_launcher_open ? 1 : 0; }

// ---------------------------------------------------------------------------
// Submit: hand the prompt to the AI Chat dock (one-shot), then close.
// ---------------------------------------------------------------------------
static void launcher_submit(const char *prompt) {
    if (!prompt || !prompt[0]) return;

    // Write the prompt to the hand-off file the AI Chat dock polls (#185/#292).
    int fd = sys_open("/CONFIG/AIASK.TXT", 0x41 | 0x200 /* O_WRONLY|O_CREAT|O_TRUNC */);
    if (fd >= 0) {
        int n = 0; while (prompt[n]) n++;
        sys_write(fd, prompt, (unsigned long)n);
        sys_write(fd, "\n", 1);
        sys_close(fd);
    }

    // Make sure the dock app is running/enabled so it consumes the file, opens
    // its panel, and runs the ReAct tool loop on the prompt.
    aichat_set_enabled(1);

    // Brief acknowledgement so the user sees the request was accepted while the
    // AI Chat dock is opening with the answer.
    notify_post("AI Assistant", "Working on your request...", NOTIFY_INFO);

    launcher_close();
}

// ---------------------------------------------------------------------------
// Keyboard: typing, backspace, arrow selection, Enter submit, Esc cancel.
// Returns 1 if the key was consumed (the launcher captures ALL keys while open).
// ---------------------------------------------------------------------------
int launcher_handle_key(int key) {
    if (!g_launcher_open) return 0;

    if (key == 27 /* ESC */) { launcher_close(); return 1; }

    if (key == '\n' || key == '\r') {
        if (s_sel >= 0 && s_sel < N_EXAMPLES) launcher_submit(s_examples[s_sel]);
        else if (s_len > 0)                   launcher_submit(s_input);
        return 1;
    }

    if (key == 0x80 /* Up */) {
        if (s_sel < 0) s_sel = N_EXAMPLES - 1; else s_sel--;
        g_needs_redraw = true; return 1;
    }
    if (key == 0x81 /* Down */) {
        if (s_sel >= N_EXAMPLES - 1) s_sel = -1; else s_sel++;
        g_needs_redraw = true; return 1;
    }

    if (key == '\b' || key == 8 || key == 127) {
        if (s_len > 0) s_input[--s_len] = 0;
        s_sel = -1;                       // typing cancels the example highlight
        g_needs_redraw = true; return 1;
    }

    if (key >= 0x20 && key <= 0x7E) {
        if (s_len < LAUNCH_MAX_INPUT) { s_input[s_len++] = (char)key; s_input[s_len] = 0; }
        s_sel = -1;
        g_needs_redraw = true; return 1;
    }

    return 1;   // swallow every other key while open (modal)
}

// ---------------------------------------------------------------------------
// Mouse: click a suggestion row to run it; click outside the panel to cancel.
// Returns 1 if consumed.
// ---------------------------------------------------------------------------
int launcher_handle_mouse(int x, int y, int clicked) {
    if (!g_launcher_open) return 0;
    if (!clicked) return 1;               // block clicks reaching the desktop

    // Click inside a suggestion row -> submit it.
    for (int i = 0; i < N_EXAMPLES; i++) {
        int ry = s_row_y0 + i * s_row_h;
        if (x >= s_panel_x && x < s_panel_x + s_panel_w &&
            y >= ry && y < ry + s_row_h) {
            launcher_submit(s_examples[i]);
            return 1;
        }
    }

    // Click outside the panel -> cancel. A click on the field itself is ignored.
    if (x < s_panel_x || x >= s_panel_x + s_panel_w ||
        y < s_panel_y || y >= s_panel_y + s_panel_h) {
        launcher_close();
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Render: dim the desktop, then draw the centered rounded panel + field + rows.
// ---------------------------------------------------------------------------
void launcher_render(void) {
    if (!g_launcher_open) return;

    // 1. Dim (approx blur) the whole desktop behind the panel.
    g_draw_blend = 150;
    draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF0B0D12);
    g_draw_blend = 255;

    // 2. Panel geometry: centered, ~28% down the screen.
    int pw = g_fb_width - 120;
    if (pw > 620) pw = 620;
    if (pw < 320) pw = 320;
    int field_h = 52;
    int ph = field_h + 14 + N_EXAMPLES * 34 + 20;
    int px = (g_fb_width - pw) / 2;
    int py = g_fb_height / 4;
    if (py < 40) py = 40;

    s_panel_x = px; s_panel_y = py; s_panel_w = pw; s_panel_h = ph;

    // 3. Soft drop shadow (a few translucent offset rounded rects).
    g_draw_blend = 40;
    draw_rounded_rect(px - 2, py + 6, pw + 4, ph + 4, 20, 0xFF000000);
    draw_rounded_rect(px,     py + 3, pw,     ph + 6, 18, 0xFF000000);
    g_draw_blend = 255;

    // 4. Panel body (rounded, themed) + hairline border.
    draw_rounded_rect(px, py, pw, ph, 18, CLR_MENU_BG);
    // A subtle inner top highlight for depth (1px).
    g_draw_blend = 30;
    draw_hline(px + 18, py + 1, pw - 36, 0xFFFFFFFF);
    g_draw_blend = 255;

    // 5. Text field (rounded pill) inside the panel.
    int fx = px + 16, fy = py + 14, fw = pw - 32, fh = field_h - 10;
    s_field_x = fx; s_field_y = fy; s_field_w = fw; s_field_h = fh;
    // Rounded pill with a hairline border: a border-coloured pill, then a 1px
    // smaller field-coloured pill on top (no rounded-outline primitive exists).
    draw_rounded_rect(fx, fy, fw, fh, fh / 2, CLR_MENU_BORDER);
    draw_rounded_rect(fx + 1, fy + 1, fw - 2, fh - 2, (fh - 2) / 2, CLR_MENU_CAT_BG);

    // Search glyph (a small magnifier: ring + a short diagonal handle).
    uint32_t dim = readable_ink_dim(CLR_MENU_BG);
    int gx = fx + 18, gy = fy + fh / 2;
    draw_circle_outline(gx, gy - 1, 6, dim);
    draw_circle_outline(gx, gy - 1, 5, dim);
    for (int d = 0; d < 5; d++) {
        draw_putpixel(gx + 4 + d, gy + 3 + d, dim);
        draw_putpixel(gx + 5 + d, gy + 3 + d, dim);
    }

    // Field text: the typed input (large) or a ghost placeholder.
    int tx = fx + 36, ty = fy + (fh - 16) / 2;
    draw_set_clip(fx + 32, fy, fw - 44, fh);
    if (s_len > 0) {
        // Right-align the tail if the text overflows the field.
        int tw = text_width_large(s_input, 2);
        int avail = fw - 52;
        int drawx = tx;
        if (tw > avail) drawx = tx - (tw - avail);
        draw_text_large(drawx, ty, s_input, CLR_MENU_TEXT, 2);
        // Blinking caret after the text.
        if ((uptime_ms() / 500) & 1) {
            int cx = drawx + tw + 2;
            draw_fill_rect(cx, ty - 1, 2, 18, CLR_MENU_TEXT);
        }
    } else {
        draw_text_large(tx, ty, "Ask Maytera AI...", dim, 2);
    }
    draw_clear_clip();

    // 6. Suggestion rows (seeded examples). Arrow keys / click select + run.
    int ry = fy + fh + 12;
    s_row_y0 = ry; s_row_h = 34;
    for (int i = 0; i < N_EXAMPLES; i++) {
        int row_y = ry + i * 34;
        if (i == s_sel) {
            draw_rounded_rect(px + 10, row_y + 2, pw - 20, 30, 8, CLR_MENU_ITEM_HOVER);
        }
        // Small accent dot bullet.
        uint32_t rowink = (i == s_sel) ? CLR_MENU_TEXT : dim;
        draw_circle_filled(px + 24, row_y + 17, 2, rowink);
        // Ellipsize the example to the row width.
        char label[128];
        int n = 0; const char *e = s_examples[i];
        while (e[n] && n < 127) { label[n] = e[n]; n++; }
        label[n] = 0;
        int avail = pw - 62;
        if (text_width(label) > avail) {
            int ew = text_width("...");
            while (n > 0 && text_width(label) + ew > avail) label[--n] = 0;
            label[n] = '.'; label[n+1] = '.'; label[n+2] = '.'; label[n+3] = 0;
        }
        draw_text(px + 36, row_y + 11, label, rowink);
    }
}
