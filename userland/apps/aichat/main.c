// main.c - MayteraOS AI Chat widget (#185)
//
// A dockable GUI chat client that talks to the Kimi (Moonshot) chat-completions
// API over the kernel's synchronous HTTPS POST syscall (SYS_HTTP_POST = 239).
//
//   - Scrollable conversation transcript (user + assistant turns, word-wrapped,
//     visually distinct bubbles).
//   - Single-line text input at the bottom + a Send button (Enter also sends).
//   - Full conversation history is sent on every request so the model keeps
//     context across turns.
//   - The API key is read at runtime from /CONFIG/KIMI.KEY (never hardcoded).
//   - Theme-aware (switch(get_theme()) per the theme-app-pattern memory).
//
// Docking: the window snaps flush to the RIGHT screen edge on launch (a tall,
// narrow chat strip, the classic "sidebar assistant" placement). A normal,
// movable/resizable window otherwise; reflows on EVENT_RESIZE. (Full
// hover-to-edge auto-hide needs compositor support and is documented as
// deferred; userland apps cannot move/resize their own window.)
//
// Blocking-call UX: sys_http_post is synchronous and blocks THIS app's event
// loop for the duration of the HTTPS round-trip (the kernel yields during net
// waits so the rest of the desktop keeps running). Before the call we set a
// "Thinking..." state and force a redraw so the user sees feedback, then make
// the call.

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "aiclient.h"
#include "aicap.h"      // #293 capability tokens + consent gate
#include "notify.h"     // #168 toast notifications (consent prompt surfacing)

#undef win_draw_text
#define win_draw_text(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

// ---------------------------------------------------------------------------
// Layout (window content size; reflows on EVENT_RESIZE)
// ---------------------------------------------------------------------------
static int g_win_w = 380, g_win_h = 600;   // current window content size
#define WIN_W g_win_w
#define WIN_H g_win_h

#define SLIVER_W   12          // fully-collapsed sliver (1-char wide) on the edge
#define HOVER_BAND 28          // rightmost band that counts as hovering the dock edge (#185)
#define PEEK_W     28          // (legacy) partial peek strip; no longer used: hover now opens to full width (#185)
#define TAB_W      22          // dock toggle tab width inside the open panel
#define PANEL_W    380         // full (expanded) panel width

// Dock state machine (#185):
//   COLLAPSED (12px sliver)
//     --(hover edge 200ms dwell)--> PEEK   (transient full-width open, auto-retracts 0.5s after the cursor leaves)
//     --(click in panel)----------> OPEN   (pinned full-width, never auto-retracts)
// PEEK and OPEN render identically at g_panel_w; the only difference is auto-retract.
enum { DOCK_COLLAPSED = 0, DOCK_PEEK = 1, DOCK_OPEN = 2 };
#define HEADER_H   30          // in-content header bar
#define INPUT_H    46          // bottom input row height
#define SEND_W     60          // Send button width
#define PAD        10
#define MSG_FONT   13
#define LINE_H     17
#define BUBBLE_PAD 8
#define BUBBLE_GAP 10

// Real framebuffer size is read at runtime via fb_info(); these are fallbacks.
#define DEFAULT_SCREEN_W 1280
#define DEFAULT_SCREEN_H 768
#define TASKBAR_H        36    // compositor.h TASKBAR_HEIGHT (keep in sync)
#define PANEL_TOP        0     // panel spans the very top ...
// ... down to exactly (screen height - taskbar) so it is flush with the taskbar.

static int g_screen_w = DEFAULT_SCREEN_W;
static int g_screen_h = DEFAULT_SCREEN_H;

// Dock position: which screen edge the panel is glued to (#185). The handle (tab)
// always faces the screen interior; content is inset away from the handle.
enum { POS_RIGHT = 0, POS_LEFT = 1, POS_TOP = 2 };
static int g_pos = POS_RIGHT;

// Runtime content insets (set by layout_insets() from g_pos + dock state). The
// handle occupies TAB_W on the inner edge; content lives in the remaining box.
static int g_ins_l = TAB_W;   // left inset (handle on the left  = right-dock)
static int g_ins_r = 0;       // right inset (handle on the right = left-dock)
static int g_ins_b = 0;       // bottom inset (handle on the bottom = top-dock)
// CONTENT_X kept as the left content origin for the existing draw code.
#define CONTENT_X  g_ins_l

#define KEY_PATH   "/CONFIG/KIMI.KEY"
#define CFG_PATH   "/CONFIG/AICHAT.CFG"   // persisted panel width + enabled flag (#185)
#define API_URL    "https://api.moonshot.ai/v1/chat/completions"
#define API_MODEL  "kimi-k2.6"

#define MAX_MSGS   64
#define MAX_INPUT  1024
#define RESP_MAX   65536        // response buffer
#define BODY_MAX   65536        // request body buffer

// ---------------------------------------------------------------------------
// AI tool-contract layer (#292): the ReAct ACTION/OBSERVATION loop.
//   - /AITOOLS/INDEX.yaml lists the callable tools (id + summary).
//   - A system message tells Kimi to emit exactly one line
//       ACTION <tool-id> <json-args>
//     when it needs data or an action, and nothing else on that line.
//   - After each reply we scan for a leading ACTION line, dispatch to a real
//     executor, enforce the contract permissions + deadline, append
//       OBSERVATION <json>
//     as a tool/user message, and re-POST. Hard cap of 4 actions per turn.
// ---------------------------------------------------------------------------
#define AITOOLS_INDEX "/AITOOLS/INDEX.yaml"
#define STARTURL_PATH "/STARTURL.TXT"
#define MAX_ACTIONS   4           // hard cap of tool calls per user turn
#define TOOLLIST_MAX  4096        // compact tool list injected into the system msg
#define OBS_MAX        8192       // OBSERVATION payload buffer


// ---------------------------------------------------------------------------
// Theme palette
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_HEADER, COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEP;
static uint32_t COL_USER_BUBBLE, COL_USER_TEXT;
static uint32_t COL_AI_BUBBLE, COL_AI_TEXT;
static uint32_t COL_ERR;
static uint32_t COL_DOTS = 0x00B0B0B0;   // dock-glyph dots: soft light grey

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_HEADER=0x00F0F0F0; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00BBBBBB;
            COL_USER_BUBBLE=0x002D6CDF; COL_USER_TEXT=0x00FFFFFF;
            COL_AI_BUBBLE=0x00EDEDED;   COL_AI_TEXT=0x00202020;
            COL_ERR=0x00C03030; break;
        case 4:  // Classic gray
            COL_BG=0x00C0C0C0; COL_HEADER=0x00C0C0C0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00606060;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_USER_BUBBLE=0x00000080; COL_USER_TEXT=0x00FFFFFF;
            COL_AI_BUBBLE=0x00D8D8D8;   COL_AI_TEXT=0x00000000;
            COL_ERR=0x00800000; break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_HEADER=0x001A3A4A; COL_SEP=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_TEXT_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_USER_BUBBLE=0x0040C0E0; COL_USER_TEXT=0x00102030;
            COL_AI_BUBBLE=0x001E4050;   COL_AI_TEXT=0x00E0F0FF;
            COL_ERR=0x00FF8080; break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_HEADER=0x002E3440; COL_SEP=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_TEXT_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_USER_BUBBLE=0x0088C0D0; COL_USER_TEXT=0x002E3440;
            COL_AI_BUBBLE=0x00434C5E;   COL_AI_TEXT=0x00ECEFF4;
            COL_ERR=0x00BF616A; break;
        default: // Dark
            COL_BG=0x00252525; COL_HEADER=0x001E1E1E; COL_SEP=0x00404040;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_TEXT_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_USER_BUBBLE=0x004A90D9; COL_USER_TEXT=0x00FFFFFF;
            COL_AI_BUBBLE=0x00333333;   COL_AI_TEXT=0x00EDEDED;
            COL_ERR=0x00E08080; break;
    }
    // dock-glyph dots: a soft light grey on dark themes, a soft mid grey on
    // light themes (visible but not harsh on either).
    COL_DOTS = (kt == 2 || kt == 4) ? 0x00808080 : 0x00B0B0B0;
    gui_set_style(kt == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
}

// ---------------------------------------------------------------------------
// Conversation model

static int  g_window;
static char g_input[MAX_INPUT];
static int  g_input_len = 0;
static int  g_scroll = 0;            // pixel scroll offset of transcript
static int  g_thinking = 0;          // 1 while a request is in flight
static int  g_total_height = 0;      // last computed transcript content height
static int  g_dock = DOCK_COLLAPSED; // dock state machine (see enum). Boot HIDDEN
                                     // as a thin edge sliver (#185); the panel is
                                     // not popped open until the user hovers the
                                     // screen edge, clicks the tab, or the command
                                     // launcher hands a prompt to it.
static int  g_panel_h = 600;         // full panel height (screen - taskbar)
static int  g_panel_w = PANEL_W;     // OPEN-state width (runtime, persisted) (#185)
static unsigned long g_peek_out_ms = 0;  // when the cursor first left the open panel (0=inside) (#185)
static unsigned long g_dwell_ms = 0;     // when the cursor first touched the collapsed edge (0=not dwelling) (#185)
static int  g_resizing = 0;          // dragging the dock handle to resize (#185)
static int  g_resize_start_w = 0;    // panel width when the drag began
static int  g_resize_start_cx = 0;   // global cursor x when the drag began
static int  g_resize_moved = 0;      // 1 once the drag moved enough to count as a resize



// ---------------------------------------------------------------------------
// API key
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Config (panel width + enabled flag) persisted to /CONFIG/AICHAT.CFG (#185)
// ---------------------------------------------------------------------------
// Clamp the OPEN-state panel thickness (width for left/right docks, height for the
// top dock) to a sane range for the current screen. (#185)
static int clamp_panel_w(int w) {
    int span = (g_pos == POS_TOP) ? g_screen_h : g_screen_w;
    int hi = 900;
    if (hi > span) hi = span;
    if (w < 240) w = 240;
    if (w > hi)  w = hi;
    return w;
}

// Compositor writes "enabled=0" here to ask a running instance to quit (#185).
// We re-read this each idle tick; 1 (or missing key) = stay running.
static int g_cfg_enabled = 1;

// Parse the small "key=value" config file. Sets g_panel_w and g_cfg_enabled.
static void load_cfg(void) {
    int fd = sys_open(CFG_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[256];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    int i = 0;
    while (buf[i]) {
        char key[24]; int ks = 0;
        while (buf[i] && buf[i] != '=' && buf[i] != '\n') { if (ks < 23) key[ks++] = buf[i]; i++; }
        key[ks] = 0;
        int val = 0; int have = 0;
        if (buf[i] == '=') {
            i++;
            while (buf[i] >= '0' && buf[i] <= '9') { val = val * 10 + (buf[i] - '0'); i++; have = 1; }
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
        if (have) {
            if (!strcmp(key, "width"))   g_panel_w = clamp_panel_w(val);
            else if (!strcmp(key, "enabled")) g_cfg_enabled = val ? 1 : 0;
            else if (!strcmp(key, "position")) { if (val>=0 && val<=2) g_pos = val; }
        }
    }
}

static void save_cfg(void) {
    int fd = sys_open(CFG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    char buf[96];
    int len = snprintf(buf, sizeof(buf), "width=%d\nenabled=%d\nposition=%d\n",
                       g_panel_w, g_cfg_enabled, g_pos);
    if (len > 0) sys_write(fd, buf, (unsigned long)len);
    sys_close(fd);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Word-wrap rendering
// ---------------------------------------------------------------------------
// Render one message bubble. If draw==0 only measures height (returns height).
// y is top of the bubble. Returns height consumed (including BUBBLE_GAP).
static int render_msg(const ai_msg_t *m, int y, int draw) {
    int cont_l  = g_ins_l;
    int cont_w  = WIN_W - g_ins_l - g_ins_r;
    int avail   = cont_w - 2 * PAD;
    int bub_max = avail * 4 / 5;          // bubble max width ~80% of content
    int inner   = bub_max - 2 * BUBBLE_PAD;
    if (inner < 40) inner = 40;

    uint32_t bub_col, txt_col;
    int right_align;
    if (m->role == 0)      { bub_col = COL_USER_BUBBLE; txt_col = COL_USER_TEXT; right_align = 1; }
    else if (m->role == 2) { bub_col = COL_BG;          txt_col = COL_ERR;       right_align = 0; }
    else                   { bub_col = COL_AI_BUBBLE;   txt_col = COL_AI_TEXT;   right_align = 0; }

    // word-wrap into lines, tracking widest line
    static char line[512];
    int lines[256]; int line_start[256]; int nlines = 0;
    int widest = 0;
    int lstart = 0, llen = 0, lwidth_idx = 0;
    int i = 0;
    int last_space = -1;          // index in line[] of last space
    (void)lwidth_idx;
    while (1) {
        char c = m->text[i];
        if (c == '\n' || c == 0) {
            line[llen] = 0;
            int w = gui_ttf_width(line, MSG_FONT);
            if (w > widest) widest = w;
            if (nlines < 256) { lines[nlines] = lstart; line_start[nlines] = llen; nlines++; }
            if (c == 0) break;
            i++; lstart = i; llen = 0; last_space = -1;
            continue;
        }
        // tentatively add char
        if (llen < (int)sizeof(line) - 1) {
            line[llen] = c;
            line[llen + 1] = 0;
            if (c == ' ') last_space = llen;
            int w = gui_ttf_width(line, MSG_FONT);
            if (w > inner && llen > 0) {
                // need to wrap; break at last space if possible
                int brk;
                if (last_space > 0) brk = last_space; else brk = llen;
                char saved = line[brk]; line[brk] = 0;
                int ww = gui_ttf_width(line, MSG_FONT);
                if (ww > widest) widest = ww;
                if (nlines < 256) { lines[nlines] = lstart; line_start[nlines] = brk; nlines++; }
                line[brk] = saved;
                // continue from after break
                int consumed = (last_space > 0) ? (brk + 1) : brk;
                lstart += consumed;
                i = lstart;
                llen = 0; last_space = -1;
                continue;
            }
            llen++;
        }
        i++;
    }
    if (nlines == 0) { lines[0] = 0; line_start[0] = 0; nlines = 1; }

    int text_h = nlines * LINE_H;
    int bub_w  = widest + 2 * BUBBLE_PAD;
    if (bub_w > bub_max) bub_w = bub_max;
    if (bub_w < 30) bub_w = 30;
    int bub_h = text_h + 2 * BUBBLE_PAD;
    int bub_x = right_align ? (cont_l + cont_w - PAD - bub_w) : (cont_l + PAD);

    if (draw) {
        if (m->role != 2) {
            win_draw_rect(g_window, bub_x, y, bub_w, bub_h, bub_col);
        }
        // draw each wrapped line
        for (int k = 0; k < nlines; k++) {
            int s = lines[k], len = line_start[k];
            if (len > (int)sizeof(line) - 1) len = sizeof(line) - 1;
            memcpy(line, m->text + s, len);
            line[len] = 0;
            // strip a trailing wrap-space for cleanliness
            draw_text_sz(g_window, bub_x + BUBBLE_PAD, y + BUBBLE_PAD + k * LINE_H,
                         line, MSG_FONT, txt_col);
        }
    }
    return bub_h + BUBBLE_GAP;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// Draw a 3x3 grid of 9 dots centered at (cx,cy), like the Start-menu glyph.
static void draw_dot_grid(int cx, int cy, uint32_t col) {
    int gap = 4;                 // spacing between dot centers
    int d = 2;                   // dot size (2x2)
    for (int row = -1; row <= 1; row++) {
        for (int coli = -1; coli <= 1; coli++) {
            int x = cx + coli * gap - d / 2;
            int y = cy + row  * gap - d / 2;
            win_draw_rect(g_window, x, y, d, d, col);
        }
    }
}

// The dock handle: a clean themed strip with a centered 9-dot grid glyph in a
// soft light-grey (not pure white). Width = TAB_W when the panel is OPEN, or the
// full (sliver/peek) window width when collapsed/peeking.
// Draw the dock handle. For left/right docks it is a vertical strip of thickness
// `thick` on the inner edge; for the top dock it is a horizontal strip of height
// `thick` along the bottom edge. `collapsed` means the strip fills the window. (#185)
static void draw_tab(int thick) {
    if (g_pos == POS_TOP) {
        // horizontal handle along the BOTTOM of the (collapsed=whole / open=bottom strip)
        int hy = (thick >= WIN_H) ? 0 : (WIN_H - thick);   // collapsed fills, else bottom band
        int hh = (thick >= WIN_H) ? WIN_H : thick;
        win_draw_rect(g_window, 0, hy, WIN_W, hh, COL_HEADER);
        win_draw_rect(g_window, 0, hy, WIN_W, 1, COL_SEP);             // top edge
        win_draw_rect(g_window, 0, hy + hh - 1, WIN_W, 1, COL_FIELD);  // bottom highlight
        win_draw_rect(g_window, WIN_W / 2 - 26, hy + hh / 2 - 1, 52, 2, COL_ACCENT);
        draw_dot_grid(WIN_W / 2, hy + hh / 2, COL_DOTS);
        return;
    }
    // vertical handle: right-dock -> left edge (x=0); left-dock -> right edge.
    int hx = (g_pos == POS_LEFT && thick < WIN_W) ? (WIN_W - thick) : 0;
    int hw = thick;
    win_draw_rect(g_window, hx, 0, hw, WIN_H, COL_HEADER);
    win_draw_rect(g_window, hx, 0, hw, 1, COL_FIELD);                  // top highlight
    win_draw_rect(g_window, hx, 0, 1, WIN_H, COL_FIELD);              // left edge
    win_draw_rect(g_window, hx + hw - 1, 0, 1, WIN_H, COL_SEP);        // right edge
    win_draw_rect(g_window, hx + 2, WIN_H / 2 - 26, 2, 52, COL_ACCENT);
    draw_dot_grid(hx + hw / 2 + 1, WIN_H / 2, COL_DOTS);
}

static void draw_all(void) {
    // Clear whole window
    win_draw_rect(g_window, 0, 0, WIN_W, WIN_H, COL_BG);

    // Collapsed: the whole (12px) window IS the dock strip. PEEK and OPEN both
    // render the full panel below (#185).
    if (g_dock == DOCK_COLLAPSED) {
        draw_tab(WIN_W);
        win_invalidate(g_window);
        return;
    }

    // ---- Expanded panel layout (content box inset away from the handle) ----
    int cx0 = g_ins_l;                           // left edge of content
    int cw  = WIN_W - g_ins_l - g_ins_r;         // content width
    int CH  = WIN_H - g_ins_b;                    // content height (above bottom handle)
    if (cw < 40) cw = 40;
    if (CH < 80) CH = 80;

    // Header bar
    win_draw_rect(g_window, cx0, 0, cw, HEADER_H, COL_HEADER);
    draw_text_sz(g_window, cx0 + PAD, 8, "AI Chat - Kimi", 14, COL_TEXT);
    win_draw_rect(g_window, cx0, HEADER_H, cw, 1, COL_SEP);

    // Transcript area bounds. Input row is PINNED at the bottom of the content
    // box (above any bottom handle): transcript = (header)..(CH - INPUT_H).
    int trans_top = HEADER_H + 1;
    int trans_bot = CH - INPUT_H;
    int trans_h   = trans_bot - trans_top;
    if (trans_h < 0) trans_h = 0;

    // Measure total transcript height (system + OBSERVATION/ACTION rows are not shown)
    int total = 0;
    for (int i = 0; i < aiclient_count(); i++) {
        const ai_msg_t *m = aiclient_get(i);
        if (m->role >= 3) continue;   // system + internal tool turns hidden
        total += render_msg(m, 0, 0);
    }
    if (g_thinking) total += LINE_H + BUBBLE_GAP + 2 * BUBBLE_PAD;
    g_total_height = total;

    // Clamp scroll
    int max_scroll = total - trans_h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    // bottom-anchored chat layout
    int y;
    if (trans_h > total) y = trans_top + (trans_h - total);
    else y = trans_top - g_scroll;

    for (int i = 0; i < aiclient_count(); i++) {
        const ai_msg_t *m = aiclient_get(i);
        if (m->role >= 3) continue;   // hide system + internal tool turns
        int h = render_msg(m, y, 0);
        if (y + h > trans_top && y < trans_bot) render_msg(m, y, 1);
        y += h;
    }
    if (g_thinking && y < trans_bot) {
        int bw = gui_ttf_width("Thinking...", MSG_FONT) + 2 * BUBBLE_PAD;
        win_draw_rect(g_window, cx0 + PAD, y, bw, LINE_H + 2 * BUBBLE_PAD, COL_AI_BUBBLE);
        draw_text_sz(g_window, cx0 + PAD + BUBBLE_PAD, y + BUBBLE_PAD, "Thinking...", MSG_FONT, COL_TEXT2);
    }

    // Cover overflow above the transcript area, then redraw the header on top.
    win_draw_rect(g_window, cx0, 0, cw, trans_top, COL_BG);
    win_draw_rect(g_window, cx0, 0, cw, HEADER_H, COL_HEADER);
    draw_text_sz(g_window, cx0 + PAD, 8, "AI Chat - Kimi", 14, COL_TEXT);
    win_draw_rect(g_window, cx0, HEADER_H, cw, 1, COL_SEP);

    // Scrollbar hint
    if (total > trans_h) {
        int bar_h = trans_h * trans_h / total;
        if (bar_h < 20) bar_h = 20;
        int range = trans_h - bar_h;
        int max_scroll2 = total - trans_h;
        int bar_y = trans_top + (max_scroll2 > 0 ? (g_scroll * range / max_scroll2) : 0);
        win_draw_rect(g_window, cx0 + cw - 4, bar_y, 3, bar_h, COL_SEP);
    }

    // ---- Input row, pinned at the bottom of the content box ----
    int iy = CH - INPUT_H;
    win_draw_rect(g_window, cx0, iy, cw, INPUT_H, COL_HEADER);
    win_draw_rect(g_window, cx0, iy, cw, 1, COL_SEP);

    int field_x = cx0 + PAD, field_y = iy + 9, field_h = INPUT_H - 18;
    int field_w = cw - 2 * PAD - SEND_W - 6;
    if (field_w < 40) field_w = 40;
    win_draw_rect(g_window, field_x, field_y, field_w, field_h, COL_FIELD);
    gui_draw_rect_outline(g_window, field_x, field_y, field_w, field_h, COL_FIELD_BORDER);

    static char vis[MAX_INPUT];
    strlcpy(vis, g_input, sizeof(vis));
    while (gui_ttf_width(vis, MSG_FONT) > field_w - 14 && vis[0])
        memmove(vis, vis + 1, strlen(vis));
    int ftext_y = field_y + (field_h - MSG_FONT) / 2;
    if (g_input_len == 0 && !g_thinking) {
        draw_text_sz(g_window, field_x + 7, ftext_y, "Type a message...", MSG_FONT, COL_TEXT_DIM);
    } else {
        draw_text_sz(g_window, field_x + 7, ftext_y, vis, MSG_FONT, COL_TEXT);
        int curx = field_x + 7 + gui_ttf_width(vis, MSG_FONT);
        win_draw_rect(g_window, curx + 1, field_y + 4, 1, field_h - 8, COL_ACCENT);
    }

    int sx = cx0 + cw - PAD - SEND_W;
    uint32_t sb = g_thinking ? COL_SEP : COL_ACCENT;
    win_draw_rect(g_window, sx, field_y, SEND_W, field_h, sb);
    int tw = gui_ttf_width("Send", MSG_FONT);
    // center the label both horizontally and vertically in the button rect
    int tlabel_y = field_y + (field_h - MSG_FONT) / 2;
    draw_text_sz(g_window, sx + (SEND_W - tw) / 2, tlabel_y, "Send", MSG_FONT, 0x00FFFFFF);

    // The dock tab sits on top of everything at the very left.
    draw_tab(TAB_W);

    win_invalidate(g_window);
}

// ---------------------------------------------------------------------------
// Send / network
// ---------------------------------------------------------------------------
static void do_send(void) {
    if (g_thinking) return;
    if (g_input_len == 0) return;

    if (!aiclient_have_key()) {
        aiclient_add(2, "Error: no API key. Place your key in /CONFIG/KIMI.KEY");
        g_input[0] = 0; g_input_len = 0;
        g_scroll = 0;
        draw_all();
        return;
    }

    // append user message
    aiclient_add(0, g_input);
    g_input[0] = 0; g_input_len = 0;

    // show thinking state and force redraw BEFORE the blocking call
    g_thinking = 1;
    g_scroll = 0;
    draw_all();

    // Run the ReAct tool loop (#292): Kimi may emit ACTION lines that we execute
    // and feed back as OBSERVATION turns before it produces the final answer.
    static char content[RESP_MAX];
    int rc = aiclient_run_turn(content, sizeof(content), 0);

    g_thinking = 0;

    if (rc != 0) {
        aiclient_add(2, content);   // content holds the error message on failure
    } else if (content[0]) {
        aiclient_add(1, content);
    } else {
        aiclient_add(2, "Could not parse assistant reply from response.");
    }

    g_scroll = 0;   // anchor to bottom
    draw_all();
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void set_dock(int state);   // fwd decl

static void handle_key(gui_event_t *ev) {
    // F2 toggles open <-> collapsed (keyboard equivalent of the dock handle).
    if (ev->keycode == 0x89) { set_dock(g_dock == DOCK_COLLAPSED ? DOCK_OPEN : DOCK_COLLAPSED); return; }
    if (g_dock == DOCK_COLLAPSED) return;
    if (g_thinking) return;
    char c = ev->key_char;
    if (c == '\r' || c == '\n') { do_send(); return; }
    if (c == '\b') {
        if (g_input_len > 0) { g_input_len--; g_input[g_input_len] = 0; }
        return;
    }
    if (c == 27) return;   // Esc
    if (c >= 32 && c < 127) {
        if (g_input_len < MAX_INPUT - 1) {
            g_input[g_input_len++] = c;
            g_input[g_input_len] = 0;
        }
    }
}

// Width of the window for a given dock state.
static int dock_width(int state) {
    if (state == DOCK_OPEN) return g_panel_w;   // pinned, full width (drag-resizable) (#185)
    if (state == DOCK_PEEK) return g_panel_w;   // transient hover-open, also full width (#185)
    return SLIVER_W;                            // collapsed sliver
}

// Compute content insets for the current dock position + dock state. The handle
// (TAB_W) sits on the inner edge when OPEN/PEEK; when COLLAPSED there is no inset
// (the whole sliver is the handle). (#185)
static void layout_insets(int collapsed) {
    g_ins_l = g_ins_r = g_ins_b = 0;
    if (collapsed) return;
    if      (g_pos == POS_RIGHT) g_ins_l = TAB_W;   // handle on left edge
    else if (g_pos == POS_LEFT)  g_ins_r = TAB_W;   // handle on right edge
    else /* POS_TOP */           g_ins_b = TAB_W;   // handle on bottom edge
}

// Recreate the borderless panel window glued to the chosen screen edge. `thick`
// is the adjustable thickness (width for left/right docks; height for the top
// dock). Full span along the docked edge. (#185)
static void create_panel(int thick) {
    if (g_window >= 0) win_destroy(g_window);
    int x, y;
    if (g_pos == POS_TOP) {
        g_win_w = g_screen_w;               // full screen width
        g_win_h = thick;                    // adjustable height
        x = 0; y = PANEL_TOP;               // glued to the top
    } else if (g_pos == POS_LEFT) {
        g_win_w = thick;                    // adjustable width
        g_win_h = g_panel_h;                // full height
        x = 0; y = PANEL_TOP;               // glued to the left
    } else { /* POS_RIGHT */
        g_win_w = thick;
        g_win_h = g_panel_h;
        x = g_screen_w - g_win_w; if (x < 0) x = 0;
        y = PANEL_TOP;                      // glued to the right
    }
    g_window = win_create("AI Chat", x, y, g_win_w, g_win_h);
    if (g_window >= 0)
        win_set_nochrome(g_window);   // borderless panel (kernel also focuses it)
}

// Transition to a dock state, recreating the window at the matching width.
static void set_dock(int state) {
    if (state == g_dock) return;
    g_peek_out_ms = 0;   // clean retract timer on every transition (#185)
    g_dwell_ms = 0;      // clean dwell timer too
    g_dock = state;
    layout_insets(state == DOCK_COLLAPSED);
    create_panel(dock_width(state));
    if (state == DOCK_OPEN) g_scroll = 0;
    draw_all();
}

// True if window-local (mx,my) lands on the dock handle strip (inner edge). (#185)
static int on_handle(int mx, int my) {
    if (g_pos == POS_TOP)   return (my >= WIN_H - TAB_W);              // bottom strip
    if (g_pos == POS_LEFT)  return (mx >= WIN_W - TAB_W);             // right strip
    return (mx >= 0 && mx < TAB_W);                                    // left strip (right-dock)
}

// Cycle the dock position (RIGHT -> LEFT -> TOP -> RIGHT), persist, and recreate
// the panel at the new edge keeping the current dock state. (#185)
static void cycle_position(void) {
    g_pos = (g_pos + 1) % 3;
    g_panel_w = clamp_panel_w(g_panel_w);   // re-clamp against the new edge span
    save_cfg();
    layout_insets(g_dock == DOCK_COLLAPSED);
    create_panel(dock_width(g_dock));
    draw_all();
}

static void handle_click(int mx, int my) {
    (void)my;
    // Collapsed should pop open via the hover dwell, but a stray click here
    // still opens (pinned) so the panel is never stuck closed.
    if (g_dock == DOCK_COLLAPSED) { set_dock(DOCK_OPEN); return; }

    // A press on the dock handle begins a potential drag-resize (both PEEK and
    // OPEN). MOUSE_UP decides click (collapse) vs drag (resize). Along the active
    // axis: x for left/right docks, y for the top dock.
    if (on_handle(mx, my)) {
        g_resizing = 1;
        g_resize_moved = 0;
        g_resize_start_w = g_panel_w;
        int cx = 0, cy = 0; unsigned int mb = 0; get_global_mouse(&cx, &cy, &mb);
        g_resize_start_cx = (g_pos == POS_TOP) ? cy : cx;
        return;
    }

    // A click anywhere else PINS a transient (hover-opened) panel so it stops
    // auto-retracting. Then fall through to normal hit testing.
    if (g_dock == DOCK_PEEK) { g_dock = DOCK_OPEN; g_peek_out_ms = 0; }

    if (g_thinking) return;

    int cx0 = g_ins_l;
    int cw  = WIN_W - g_ins_l - g_ins_r;
    int CH  = WIN_H - g_ins_b;
    int iy = CH - INPUT_H;
    int field_y = iy + 9, field_h = INPUT_H - 18;
    int sx = cx0 + cw - PAD - SEND_W;
    if (mx >= sx && mx <= sx + SEND_W && my >= field_y && my <= field_y + field_h) {
        do_send();
        return;
    }
}

// While dragging the handle, recompute the OPEN width from the global cursor x.
// The window is pinned to the right edge, so width = screen_w - cursor_x.
// Returns 1 if the width changed (and the panel was recreated).
static int resize_drag_poll(void) {
    if (!g_resizing) return 0;
    int cx = 0, cy = 0; unsigned int mb = 0; get_global_mouse(&cx, &cy, &mb);
    int axis = (g_pos == POS_TOP) ? cy : cx;
    if (axis - g_resize_start_cx > 4 || g_resize_start_cx - axis > 4) g_resize_moved = 1;
    // Thickness = distance from the docked edge to the cursor along the active axis.
    int nw;
    if      (g_pos == POS_TOP)  nw = clamp_panel_w(cy);                  // top: height = cursor y
    else if (g_pos == POS_LEFT) nw = clamp_panel_w(cx);                  // left: width = cursor x
    else                        nw = clamp_panel_w(g_screen_w - cx);     // right: width = w - x
    int diff = nw - g_panel_w; if (diff < 0) diff = -diff;
    if (diff >= 2) {
        g_panel_w = nw;
        create_panel(g_panel_w);
        draw_all();
        return 1;
    }
    return 0;
}

// End a drag. Tiny movement = treat as a plain click on the handle (collapse);
// otherwise commit the new width to the config file.
static void resize_drag_end(void) {
    if (!g_resizing) return;
    g_resizing = 0;
    if (!g_resize_moved) {
        set_dock(DOCK_COLLAPSED);   // it was a click, not a drag
    } else {
        save_cfg();                 // persist the new width
    }
    g_resize_moved = 0;
}

static void handle_scroll(int delta) {
    if (g_dock == DOCK_COLLAPSED) return;
    int trans_top = HEADER_H + 1;
    int trans_bot = (WIN_H - g_ins_b) - INPUT_H;
    int trans_h   = trans_bot - trans_top;
    int max_scroll = g_total_height - trans_h;
    if (max_scroll < 0) max_scroll = 0;
    // Each wheel notch jumps ~4 text lines for a responsive feel. delta>0 =
    // wheel up -> reveal older messages (increase g_scroll toward the top).
    g_scroll += delta * (LINE_H * 4);
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;
}

// ---------------------------------------------------------------------------
// Consent dialog (#293)
// ---------------------------------------------------------------------------
// When the AI requests a HIGH-risk tool with no valid capability token, the
// aicap gate calls back into this app to ask the user. We render a modal dialog
// over the panel describing exactly what the AI wants to do (tool, capability,
// risk, target) with four choices, and return one of AICAP_CONSENT_*. For
// headless tests a decision can be pre-seeded in /CONFIG/AICONSENT.CFG; in that
// case we still draw the dialog (so a screendump captures it) for a few seconds
// before auto-resolving.
static char g_cs_tool[64], g_cs_cap[48], g_cs_target[200];
static int  g_cs_btn_x = 0, g_cs_btn_w = 0, g_cs_btn_h = 0, g_cs_btn_y[4];
static const char *CS_BTN[4] = { "Allow Once", "Allow This Session", "Always Allow", "Deny" };

static void draw_consent_dialog(void) {
    int cx0 = g_ins_l;
    int cw  = WIN_W - g_ins_l - g_ins_r;
    if (cw < 60) cw = WIN_W;
    int line_h = 18, btn_h = 30, btn_gap = 8;
    int n_info = 5;
    int bw = cw - 24; if (bw < 180) bw = (cw > 188) ? 180 : cw - 8;
    int bx = cx0 + (cw - bw) / 2;
    int by = 56;
    int bh = 14 + line_h + 6 + n_info * line_h + 10 + 4 * (btn_h + btn_gap) + 8;

    // dim the panel behind the dialog
    win_draw_rect(g_window, cx0, 0, cw, WIN_H, COL_BG);
    // dialog box
    win_draw_rect(g_window, bx, by, bw, bh, COL_HEADER);
    gui_draw_rect_outline(g_window, bx, by, bw, bh, COL_ACCENT);

    int tx = bx + 12, ty = by + 12;
    draw_text_sz(g_window, tx, ty, "AI Permission Request", 15, COL_TEXT); ty += line_h + 6;
    draw_text_sz(g_window, tx, ty, "Kimi wants to perform an action:", 12, COL_TEXT2); ty += line_h;
    char l[224];
    snprintf(l, sizeof(l), "Tool: %s", g_cs_tool);        draw_text_sz(g_window, tx, ty, l, 13, COL_TEXT);  ty += line_h;
    snprintf(l, sizeof(l), "Capability: %s", g_cs_cap);   draw_text_sz(g_window, tx, ty, l, 13, COL_TEXT2); ty += line_h;
    draw_text_sz(g_window, tx, ty, "Risk: HIGH", 13, COL_ERR); ty += line_h;
    snprintf(l, sizeof(l), "Target: %s", g_cs_target[0] ? g_cs_target : "(none)");
    // truncate target to box width
    while (gui_ttf_width(l, 12) > bw - 24 && strlen(l) > 12) l[strlen(l) - 1] = 0;
    draw_text_sz(g_window, tx, ty, l, 12, COL_TEXT); ty += line_h + 8;

    g_cs_btn_x = bx + 12; g_cs_btn_w = bw - 24; g_cs_btn_h = btn_h;
    for (int i = 0; i < 4; i++) {
        int yy = ty + i * (btn_h + btn_gap);
        g_cs_btn_y[i] = yy;
        uint32_t bc = (i == 3) ? COL_ERR : COL_ACCENT;
        win_draw_rect(g_window, g_cs_btn_x, yy, g_cs_btn_w, btn_h, bc);
        int tw = gui_ttf_width(CS_BTN[i], 13);
        draw_text_sz(g_window, g_cs_btn_x + (g_cs_btn_w - tw) / 2, yy + (btn_h - 13) / 2,
                     CS_BTN[i], 13, 0x00FFFFFF);
    }
    win_invalidate(g_window);
}

static int consent_hit(int mx, int my) {
    if (mx < g_cs_btn_x || mx > g_cs_btn_x + g_cs_btn_w) return -1;
    for (int i = 0; i < 4; i++)
        if (my >= g_cs_btn_y[i] && my <= g_cs_btn_y[i] + g_cs_btn_h) return i;
    return -1;
}

// aicap consent callback. Returns AICAP_CONSENT_* (0=deny..3=persist).
static int chat_consent(const char *tool_id, const char *cap, int risk,
                        const char *args, const char *target) {
    (void)args; (void)risk;
    strlcpy(g_cs_tool, tool_id, sizeof(g_cs_tool));
    strlcpy(g_cs_cap, cap, sizeof(g_cs_cap));
    strlcpy(g_cs_target, target ? target : "", sizeof(g_cs_target));
    if (g_dock != DOCK_OPEN) set_dock(DOCK_OPEN);
    draw_consent_dialog();
    notify_post("AI Permission Request", tool_id, NOTIFY_WARNING);

    // Headless determinism: if a decision was pre-seeded, keep the dialog up for
    // a few seconds (screendump window) then auto-resolve.
    int pre = aicap_preseed_consent(tool_id, cap);
    if (pre >= 0) {
        unsigned long t0 = uptime_ms();
        while (uptime_ms() - t0 < 6000) {
            draw_consent_dialog();
            gui_event_t ev; win_get_event(g_window, &ev, 250);
        }
        draw_all();
        return pre;
    }

    // Interactive: block on the dialog until a button is pressed (or closed).
    unsigned long t0 = uptime_ms();
    for (;;) {
        gui_event_t ev;
        int et = win_get_event(g_window, &ev, 150);
        if (et != 0) {
            if (ev.type == EVENT_REDRAW) {
                draw_consent_dialog();
            } else if (ev.type == EVENT_WINDOW_CLOSE) {
                draw_all(); return AICAP_CONSENT_DENY;
            } else if (ev.type == EVENT_MOUSE_DOWN &&
                       (ev.mouse_buttons & MOUSE_BUTTON_LEFT)) {
                int h = consent_hit(ev.mouse_x, ev.mouse_y);
                if (h >= 0) { draw_all(); return h; }
            }
        }
        if (uptime_ms() - t0 > 180000) { draw_all(); return AICAP_CONSENT_DENY; }
    }
}

// ---------------------------------------------------------------------------
// Per-iteration dock polling: hover-to-open dwell, auto-retract, live drag, and
// the compositor enable flag. Runs every loop turn (the collapsed sliver emits a
// steady REDRAW stream, so we must not gate this on idle ticks only). (#185)
// ---------------------------------------------------------------------------
static int  g_running = 1;
static void poll_dock(void) {
    if (g_resizing) { resize_drag_poll(); return; }

    int amx = 0, amy = 0; unsigned int mb = 0; get_global_mouse(&amx, &amy, &mb);

    if (g_dock == DOCK_COLLAPSED) {
        int on_edge;
        if (g_pos == POS_TOP)
            on_edge = (amy < HOVER_BAND);                                  // top screen edge
        else if (g_pos == POS_LEFT)
            on_edge = (amx < HOVER_BAND &&
                       amy >= PANEL_TOP && amy < PANEL_TOP + g_panel_h);   // left edge
        else
            on_edge = (amx >= g_screen_w - HOVER_BAND &&
                       amy >= PANEL_TOP && amy < PANEL_TOP + g_panel_h);   // right edge
        if (on_edge) {
            if (g_dwell_ms == 0) g_dwell_ms = uptime_ms();
            else if (uptime_ms() - g_dwell_ms >= 200) {
                g_dwell_ms = 0;
                set_dock(DOCK_PEEK);          // 200ms dwell -> full-width transient open
            }
        } else {
            g_dwell_ms = 0;                    // left the edge before 200ms: cancel
        }
    } else if (g_dock == DOCK_PEEK) {
        int wx = 0, wy = 0; win_get_pos(g_window, &wx, &wy);
        int outside = (amx < wx || amx >= wx + g_win_w ||
                       amy < wy || amy >= wy + g_win_h);
        if (outside) {
            if (g_peek_out_ms == 0) g_peek_out_ms = uptime_ms();
            else if (uptime_ms() - g_peek_out_ms >= 500) {
                g_peek_out_ms = 0;
                set_dock(DOCK_COLLAPSED);      // 0.5s after leaving -> retract
            }
        } else {
            g_peek_out_ms = 0;                 // cursor returned: cancel retract
        }
    }
    // Live enable flag: the compositor writes enabled=0 to ask us to quit.
    { static int poll = 0; if (++poll >= 8) { poll = 0; load_cfg();
        if (!g_cfg_enabled) g_running = 0; } }
}

// ---------------------------------------------------------------------------
// One-shot prompt hand-off from the taskbar command launcher (Spotlight).
// The compositor writes the typed prompt to /CONFIG/AIASK.TXT and (re)enables
// this app. We consume that file, pop the dock OPEN, and run the prompt through
// the SAME ReAct tool loop (aiclient_run_turn) the chat input and #292 seed path
// use. Consumed once so a stale file never re-fires. Called on a throttled tick
// from the event loop (NOT a busy-wait): a plain sys_open miss is cheap and
// returns immediately.
// ---------------------------------------------------------------------------
#define ASK_PATH   "/CONFIG/AIASK.TXT"
static void process_ask_file(void) {
    int fd = sys_open(ASK_PATH, O_RDONLY);
    if (fd < 0) return;                       // nothing pending: return at once
    static char ask[MAX_INPUT];
    long n = sys_read(fd, ask, sizeof(ask) - 1);
    sys_close(fd);
    sys_unlink(ASK_PATH);                     // consume-once
    if (n <= 0) return;
    ask[n] = 0;
    // Trim a single trailing newline / stray whitespace.
    while (n > 0 && (ask[n-1] == '\n' || ask[n-1] == '\r' ||
                     ask[n-1] == ' '  || ask[n-1] == '\t')) ask[--n] = 0;
    if (!ask[0]) return;

    if (g_dock != DOCK_OPEN) set_dock(DOCK_OPEN);   // reveal the panel for the answer
    if (!aiclient_have_key()) {
        aiclient_add(0, ask);
        aiclient_add(2, "No API key found at /CONFIG/KIMI.KEY. Add your Moonshot key to chat.");
        g_scroll = 0; draw_all();
        return;
    }
    aiclient_add(0, ask);                     // show the user's turn
    g_thinking = 1; g_scroll = 0; draw_all();
    static char sc[RESP_MAX];
    int rc = aiclient_run_turn(sc, sizeof(sc), 1 /* verbose */);
    g_thinking = 0;
    if (rc != 0)      aiclient_add(2, sc);
    else if (sc[0])   aiclient_add(1, sc);
    else              aiclient_add(2, "Could not parse assistant reply.");
    g_scroll = 0; draw_all();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// Headless harness (#292): `aichat <prompt...>` runs the SAME ReAct tool loop
// without a window and prints the full transcript (ACTION/OBSERVATION + final
// answer) to the serial console. Drive it over serial via msh to prove the loop
// end to end without the flaky GUI input path. Returns a process exit code.
static int run_headless(int argc, char **argv) {
    if (!aiclient_init()) { printf("aichat: no API key at /CONFIG/KIMI.KEY\n"); return 1; }

    // join argv[1..] into one prompt string
    static char prompt[MAX_INPUT];
    int pl = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i > 1 && pl < (int)sizeof(prompt) - 1) prompt[pl++] = ' ';
        while (*a && pl < (int)sizeof(prompt) - 1) prompt[pl++] = *a++;
    }
    prompt[pl] = 0;

    printf("=== aichat headless (#292 tool loop) ===\n");
    printf("PROMPT: %s\n", prompt);
    printf("TOOLS LOADED: %d\n", aiclient_have_tools());

    static char final[RESP_MAX];
    int rc = aiclient_ask(prompt, final, sizeof(final), 1 /* verbose: print steps */);
    if (rc != 0) printf("ERROR: %s\n", final);
    else         printf("FINAL: %s\n", final);
    printf("=== end ===\n");
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2) return run_headless(argc, argv);

    apply_theme(get_theme());
    if (!aiclient_init()) { /* no key: still run; UI shows the no-key notice */ }

    // Read the real framebuffer size so the panel is exactly full height and
    // flush against the right edge + the taskbar (no gaps), on any resolution.
    fb_info_t fi;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) {
        g_screen_w = (int)fi.width;
        g_screen_h = (int)fi.height;
    }

    // Full-height panel: top at y=0, bottom flush with the taskbar top.
    g_panel_h = g_screen_h - TASKBAR_H - PANEL_TOP;
    if (g_panel_h < 200) g_panel_h = 200;

    // Persisted OPEN width (+ live enabled flag). Default 380 if missing/garbage.
    g_panel_w = clamp_panel_w(g_panel_w);
    load_cfg();
    g_cfg_enabled = 1;   // we were launched, so treat ourselves as enabled now

    g_window = -1;
    layout_insets(g_dock == DOCK_COLLAPSED);
    create_panel(dock_width(g_dock));
    if (g_window < 0) return 1;

    // Inject the tool-contract system message first (#292) so it rides on every
    // request but is never shown in the transcript.
    aiclient_reset();   // seed the conversation with the system prompt (tools + ACTION protocol)

    // #293: route HIGH-risk tool consent through our modal dialog.
    aicap_set_consent_cb(chat_consent);

    if (aiclient_have_key())
        aiclient_add(1, "Hi! I'm Kimi, your AI assistant. Ask me anything.");
    else
        aiclient_add(2, "No API key found at /CONFIG/KIMI.KEY. Add your Moonshot key to chat.");

    draw_all();

    // --- Seed prompt (#292 live-test / autorun) -------------------------------
    // If /CONFIG/AISEED.TXT exists, each non-empty line is sent as a user turn on
    // launch and the ReAct tool loop runs (verbose). The compositor wires a
    // spawned app's stdout to a PTY (not the raw serial console), so the loop also
    // appends the literal ACTION/OBSERVATION transcript to /HOME/AILOG.TXT for
    // off-disk inspection. The seed file is consumed (deleted) so it fires exactly
    // once. This drives the tool loop deterministically for a live test (file log +
    // screendump) without depending on the flaky GUI keyboard input path.
    {
        int sfd = sys_open("/CONFIG/AISEED.TXT", O_RDONLY);
        if (sfd >= 0) {
            static char seed[MAX_INPUT];
            long sn = sys_read(sfd, seed, sizeof(seed) - 1);
            sys_close(sfd);
            sys_unlink("/CONFIG/AISEED.TXT");          // consume-once
            if (sn > 0 && aiclient_have_key()) {
                seed[sn] = 0;
                if (g_dock != DOCK_OPEN) set_dock(DOCK_OPEN);
                // Wait for the network (DHCP) so a boot-time POST is reliable even
                // when aichat is auto-launched before the DHCP lease arrives.
                for (int w = 0; w < 60; w++) {
                    net_info_t ni;
                    if (get_net_info(&ni, sizeof(ni)) >= 0 && ni.connected &&
                        ni.ip[0] && strcmp(ni.ip, "0.0.0.0")) break;
                    sys_sleep(500);
                }
                // Run each non-empty line of the seed as its own user turn.
                int li = 0;
                while (seed[li]) {
                    char line[MAX_INPUT]; int ll = 0;
                    while (seed[li] && seed[li] != '\n' && seed[li] != '\r') {
                        if (ll < (int)sizeof(line) - 1) line[ll++] = seed[li];
                        li++;
                    }
                    while (seed[li] == '\n' || seed[li] == '\r') li++;
                    line[ll] = 0;
                    while (ll > 0 && (line[ll-1] == ' ' || line[ll-1] == '\t')) line[--ll] = 0;
                    if (!line[0]) continue;
                    printf("[aichat] SEED PROMPT: %s\n", line);
                    aiclient_add(0, line);
                    g_thinking = 1; g_scroll = 0; draw_all();
                    static char sc[RESP_MAX];
                    int src = aiclient_run_turn(sc, sizeof(sc), 1 /* verbose */);
                    g_thinking = 0;
                    if (src != 0)   aiclient_add(2, sc);
                    else if (sc[0]) aiclient_add(1, sc);
                    else            aiclient_add(2, "Could not parse assistant reply.");
                    g_scroll = 0; draw_all();
                    printf("[aichat] SEED FINAL: %s\n", sc);
                }
                // Dump the full transcript (incl. internal ACTION/OBSERVATION turns)
                // to /HOME/AILOG.TXT for off-disk inspection of the tool cycle.
                int lfd = sys_open("/HOME/AILOG.TXT", O_WRONLY | O_CREAT | O_TRUNC);
                if (lfd >= 0) {
                    const char *hdr = "=== MayteraOS AI tool-loop transcript (#292) ===\n";
                    sys_write(lfd, hdr, strlen(hdr));
                    int nm = aiclient_count();
                    for (int i = 0; i < nm; i++) {
                        const ai_msg_t *m = aiclient_get(i);
                        if (!m || !m->text) continue;
                        const char *tag;
                        switch (m->role) {
                            case 0: tag = "USER: ";      break;
                            case 1: tag = "ASSISTANT: "; break;
                            case 4: tag = "[internal] "; break;  // raw ACTION reply
                            case 5: tag = "[internal] "; break;  // OBSERVATION turn
                            default: tag = 0;            break;  // 2=note, 3=system: skip
                        }
                        if (!tag) continue;
                        sys_write(lfd, tag, strlen(tag));
                        sys_write(lfd, m->text, strlen(m->text));
                        sys_write(lfd, "\n", 1);
                    }
                    sys_close(lfd);
                }
            }
        }
    }

    // --- #294 build self-test driver (deterministic plumbing proof) -----------
    // If /CONFIG/BUILDTEST.CFG exists (line1 = app_id, line2 = scenario:
    // "compile_deploy" | "compile_only"), read the new app source from
    // /CONFIG/BUILDSRC.TXT, then drive build.compile_app (and, on success,
    // build.deploy_app) through the REAL #293 consent + audit gate via
    // aiclient_run_action(). This proves the compile->deploy->relaunch plumbing
    // (and the consent/error-feedback paths) WITHOUT depending on the LLM. Each
    // OBSERVATION is appended to /HOME/AILOG.TXT. Consumed once.
    {
        int cfd = sys_open("/CONFIG/BUILDTEST.CFG", O_RDONLY);
        if (cfd >= 0) {
            static char cfg[256];
            long cn = sys_read(cfd, cfg, sizeof(cfg) - 1);
            sys_close(cfd);
            sys_unlink("/CONFIG/BUILDTEST.CFG");           // consume-once
            if (cn > 0) {
                cfg[cn] = 0;
                char app_id[64]; int ai = 0;
                int ci = 0;
                while (cfg[ci] && cfg[ci] != '\n' && cfg[ci] != '\r') {
                    if (ai < (int)sizeof(app_id) - 1) app_id[ai++] = cfg[ci];
                    ci++;
                }
                app_id[ai] = 0;
                while (cfg[ci] == '\n' || cfg[ci] == '\r') ci++;
                int deploy = (strstr(cfg + ci, "compile_deploy") != 0);
                if (g_dock != DOCK_OPEN) set_dock(DOCK_OPEN);
                // wait for the network so the HTTPS POST to the build service works
                for (int w = 0; w < 60; w++) {
                    net_info_t ni;
                    if (get_net_info(&ni, sizeof(ni)) >= 0 && ni.connected &&
                        ni.ip[0] && strcmp(ni.ip, "0.0.0.0")) break;
                    sys_sleep(500);
                }
                // read the new source
                static char src[16384];
                long srn = 0;
                int srcfd = sys_open("/CONFIG/BUILDSRC.TXT", O_RDONLY);
                if (srcfd >= 0) { srn = sys_read(srcfd, src, sizeof(src) - 1); sys_close(srcfd); }
                if (srn < 0) srn = 0;
                src[srn] = 0;
                // build {"app_id":"..","source":"<json-escaped>"}
                static char bargs[24576];
                int bo = 0;
                const char *pfx = "{\"app_id\":\"";
                for (const char *p = pfx; *p; p++) bargs[bo++] = *p;
                for (const char *p = app_id; *p; p++) bargs[bo++] = *p;
                const char *mid = "\",\"source\":\"";
                for (const char *p = mid; *p; p++) bargs[bo++] = *p;
                for (long i = 0; i < srn && bo < (int)sizeof(bargs) - 8; i++) {
                    unsigned char c = (unsigned char)src[i];
                    if      (c == '\\') { bargs[bo++] = '\\'; bargs[bo++] = '\\'; }
                    else if (c == '"')  { bargs[bo++] = '\\'; bargs[bo++] = '"';  }
                    else if (c == '\n') { bargs[bo++] = '\\'; bargs[bo++] = 'n';  }
                    else if (c == '\r') { bargs[bo++] = '\\'; bargs[bo++] = 'r';  }
                    else if (c == '\t') { bargs[bo++] = '\\'; bargs[bo++] = 't';  }
                    else if (c < 0x20)  { bo += snprintf(bargs + bo, 8, "\\u%04x", c); }
                    else                { bargs[bo++] = (char)c; }
                }
                bargs[bo++] = '"'; bargs[bo++] = '}'; bargs[bo] = 0;

                // append to /HOME/AILOG.TXT
                int lfd = sys_open("/HOME/AILOG.TXT", O_WRONLY | O_CREAT);
                if (lfd >= 0) sys_seek(lfd, 0, 2 /* SEEK_END */);
                #define BLOG(s) do { printf("%s", s); if (lfd >= 0) sys_write(lfd, s, strlen(s)); } while (0)
                BLOG("\n=== #294 build self-test ===\n");
                BLOG("ACTION build.compile_app app_id=");
                BLOG(app_id); BLOG("\n");

                static char obs[8192];
                int az = aiclient_run_action("build.compile_app", bargs, obs, sizeof(obs));
                BLOG("OBSERVATION "); BLOG(obs); BLOG("\n");

                int compiled = (az == 0 /*AICAP_ALLOW*/ && strstr(obs, "\"status\":\"compiled\"") != 0);
                if (deploy && compiled) {
                    char dargs[96];
                    snprintf(dargs, sizeof(dargs), "{\"app_id\":\"%s\"}", app_id);
                    BLOG("ACTION build.deploy_app "); BLOG(dargs); BLOG("\n");
                    static char obs2[2048];
                    aiclient_run_action("build.deploy_app", dargs, obs2, sizeof(obs2));
                    BLOG("OBSERVATION "); BLOG(obs2); BLOG("\n");
                } else if (deploy) {
                    BLOG("deploy skipped (compile did not succeed)\n");
                }
                if (lfd >= 0) sys_close(lfd);
                #undef BLOG
                draw_all();
            }
        }
    }

    gui_event_t ev;
    int running = 1; g_running = 1;
    while (running && g_running) {
        // live theme follow
        { static int lt = -1; int th = get_theme(); if (th != lt) { lt = th; apply_theme(th); draw_all(); } }

        int et = win_get_event(g_window, &ev, 120);

        // Run dock polling (hover-dwell, auto-retract, drag, enable flag) on
        // EVERY iteration so the steady sliver REDRAW stream cannot starve it.
        poll_dock();
        // Pick up a one-shot prompt handed over by the taskbar command launcher
        // (throttled: ~once/second, so the miss-case sys_open is negligible).
        { static int ask_poll = 0; if (++ask_poll >= 8) { ask_poll = 0; process_ask_file(); } }
        if (!g_running) break;
        if (et == 0) continue;   // pure idle tick: nothing else to dispatch

        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                // win_set_nochrome queues this with the true (full-window)
                // content size. Adopt it so the layout uses the exact dims.
                if (ev.mouse_x > 4 && ev.mouse_y > 60) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; }
                draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; g_running = 0; break;
            case EVENT_MOUSE_MOVE:
                if (g_resizing) { resize_drag_poll(); break; }
                // Hover-to-open is handled by the 200ms dwell in the idle tick;
                // a bare move over the sliver no longer pops the panel.
                break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_RIGHT) {
                    // Right-click the 9-dot handle to cycle dock position (R->L->T). (#185)
                    if (g_dock != DOCK_COLLAPSED && on_handle(ev.mouse_x, ev.mouse_y)) cycle_position();
                } else if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    handle_click(ev.mouse_x, ev.mouse_y); draw_all();
                }
                break;
            case EVENT_MOUSE_UP:
                if (g_resizing) { resize_drag_end(); draw_all(); }
                break;
            case EVENT_MOUSE_SCROLL:
                handle_scroll(ev.scroll_delta);
                draw_all();
                break;
            case EVENT_KEY_DOWN:
                handle_key(&ev);
                if (running && g_running) draw_all();
                break;
            default: break;
        }
    }

    win_destroy(g_window);
    return 0;
}
