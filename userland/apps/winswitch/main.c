// winswitch - MayteraOS Task Switcher overlay (Alt-Tab style).
// A borderless, centered overlay listing every open window with a live mini
// screen map. Fills the gap where switching windows requires hunting the
// taskbar with the mouse: this is the keyboard-first switcher.
//
//   Tab / Down       next window          Enter / Space   focus + dismiss
//   Shift+Tab / Up   previous window      M               minimize selected
//   R                refresh list         Esc             dismiss (restores
//   mouse hover/click also work; list auto-refreshes while open.  original)
//
// #alttab: launched by the compositor (userland/apps/compositor/main.c) the
// moment Tab is pressed while Alt is held, and normally driven ENTIRELY by
// held-Alt semantics from there: further Tab/Shift+Tab presses in the same
// hold reach this window through the ordinary focus-routed key path (this
// window takes focus on win_create(), so nothing needs to address it
// specially), and releasing Alt commits the highlighted window (see the
// EVENT_KEY_UP case below) exactly like pressing Enter does. The Enter/
// Space/click/1-9/R/M/Esc bindings above all still work too, so this is a
// perfectly usable app on its own (e.g. launched from the Accessories start
// menu) for anyone who would rather click than hold a chord.
//
// Self-contained companion app: uses only existing syscalls
// (wm_get_windows / wm_focus / wm_minimize, win_set_nochrome, fb_info)
// plus the shared theme palette + TTF text, per docs/UI_STYLE_GUIDE.md, and
// the shared modifier tracker (gui_mods.h) for Shift+Tab and Alt-release -
// nobody needs, or should write, a second one of those.

#include "syscall.h"
#include "gui.h"
#include "gui_style.h"
#include "gui_mods.h"
#include "string.h"
#include "stdio.h"

#define MAX_WINS    32
#define WIN_W       620
#define ROW_H       36
#define VISIBLE_MAX 10
#define HEAD_H      34
#define FOOT_H      24
#define PAD         10
#define MAP_W       200      // mini screen map panel on the right
#define OWN_TITLE   "Task Switcher"

static wm_window_info_t g_wins[MAX_WINS];
static int g_count = 0;      // entries after filtering out ourselves
static int g_sel = 0;
static int g_scroll = 0;
static int g_win = -1;
static int g_win_h = 0;
static int g_sw = 1024, g_sh = 768;   // screen size for the mini map
static int g_had_focus = 0;

// ---------------------------------------------------------------------------
// Theme palette (same scheme as Notes/Settings/launcher)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_CARD, COL_BORDER, COL_TEXT, COL_TEXT2, COL_DIM;
static uint32_t COL_ACCENT, COL_SEL, COL_SEL_TEXT, COL_WARN;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00F4F4F4; COL_CARD=0x00FFFFFF; COL_BORDER=0x00B8B8B8;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_SEL=0x00D6E4FB; COL_SEL_TEXT=0x00202020;
            COL_WARN=0x00B06000; break;
        case 4:  // Classic (CDE/Motif)
            COL_BG=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_BORDER=0x00404040;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_DIM=0x00707070;
            COL_ACCENT=0x00000080; COL_SEL=0x00000080; COL_SEL_TEXT=0x00FFFFFF;
            COL_WARN=0x00804000; break;
        case 5:  // Ocean
            COL_BG=0x001E4050; COL_CARD=0x00224455; COL_BORDER=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_SEL=0x00305060; COL_SEL_TEXT=0x00E0F0FF;
            COL_WARN=0x00E0A040; break;
        case 9:  // Nord
            COL_BG=0x002E3440; COL_CARD=0x00343B49; COL_BORDER=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_SEL=0x00434C5E; COL_SEL_TEXT=0x00ECEFF4;
            COL_WARN=0x00EBCB8B; break;
        default: // Dark
            COL_BG=0x00202020; COL_CARD=0x002A2A2A; COL_BORDER=0x00484848;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_SEL=0x003A5A80; COL_SEL_TEXT=0x00FFFFFF;
            COL_WARN=0x00E0A040; break;
    }
}

// ---------------------------------------------------------------------------
// Window list
// ---------------------------------------------------------------------------
static int is_self(const wm_window_info_t *w) {
    return strcmp(w->title, OWN_TITLE) == 0;
}

// Refresh the list. keep_id >= 0 tries to keep that window id selected so the
// periodic auto-refresh does not yank the user's selection around.
static void refresh_windows(int keep_id) {
    wm_window_info_t raw[MAX_WINS];
    int n = wm_get_windows(raw, MAX_WINS);
    if (n < 0) n = 0;
    g_count = 0;
    int focused_idx = -1;
    for (int i = 0; i < n && g_count < MAX_WINS; i++) {
        if (is_self(&raw[i])) continue;
        if (!raw[i].visible && !raw[i].minimized) continue;
        g_wins[g_count] = raw[i];
        if (raw[i].focused) focused_idx = g_count;
        g_count++;
    }
    int new_sel = -1;
    if (keep_id >= 0) {
        for (int i = 0; i < g_count; i++)
            if (g_wins[i].id == keep_id) { new_sel = i; break; }
    }
    if (new_sel < 0) {
        // Alt-Tab semantics: preselect the entry after the focused window.
        new_sel = (focused_idx >= 0 && g_count > 0)
                  ? (focused_idx + 1) % g_count : 0;
    }
    g_sel = new_sel;
    if (g_sel >= g_count) g_sel = g_count ? g_count - 1 : 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + VISIBLE_MAX) g_scroll = g_sel - VISIBLE_MAX + 1;
    if (g_scroll > 0 && g_scroll + VISIBLE_MAX > g_count)
        g_scroll = g_count - VISIBLE_MAX < 0 ? 0 : g_count - VISIBLE_MAX;
}

static int list_rows(void) {
    int n = g_count;
    return (n > VISIBLE_MAX) ? VISIBLE_MAX : (n < 1 ? 1 : n);
}

static int calc_height(void) {
    int list_h = list_rows() * ROW_H;
    int map_h = MAP_W * g_sh / (g_sw > 0 ? g_sw : 1) + 20;
    int body = list_h > map_h ? list_h : map_h;
    return PAD + HEAD_H + body + 4 + FOOT_H;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void draw_mini_map(int mx, int my) {
    // Scaled outline of the whole screen with every window's rect; the
    // selected one is filled with the accent color. Gives instant spatial
    // "where is that window" context.
    int mw = MAP_W - 16;
    int mh = mw * g_sh / (g_sw > 0 ? g_sw : 1);
    win_draw_rect(g_win, mx, my, mw, mh, COL_CARD);
    gui_draw_rect_outline(g_win, mx, my, mw, mh, COL_BORDER);
    // Draw unselected windows first, selected last so it sits on top.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_count; i++) {
            int selected = (i == g_sel);
            if ((pass == 0 && selected) || (pass == 1 && !selected)) continue;
            if (g_wins[i].minimized) continue;
            int rx = mx + g_wins[i].x * mw / g_sw;
            int ry = my + g_wins[i].y * mh / g_sh;
            int rw = g_wins[i].width  * mw / g_sw;
            int rh = g_wins[i].height * mh / g_sh;
            if (rw < 4) rw = 4;
            if (rh < 3) rh = 3;
            if (rx < mx) rx = mx;
            if (ry < my) ry = my;
            if (rx + rw > mx + mw) rw = mx + mw - rx;
            if (ry + rh > my + mh) rh = my + mh - ry;
            if (rw <= 0 || rh <= 0) continue;
            if (selected) {
                win_draw_rect(g_win, rx, ry, rw, rh, COL_ACCENT);
                gui_draw_rect_outline(g_win, rx, ry, rw, rh, COL_TEXT);
            } else {
                win_draw_rect(g_win, rx, ry, rw, rh, COL_SEL);
                gui_draw_rect_outline(g_win, rx, ry, rw, rh, COL_DIM);
            }
        }
    }
    win_draw_text_ttf(g_win, mx, my + mh + 4, "Screen map", 11, COL_DIM);
}

static void draw_all(void) {
    int rows = list_rows();
    int list_h = rows * ROW_H;

    win_draw_rect(g_win, 0, 0, WIN_W, g_win_h, COL_BG);
    gui_draw_rect_outline(g_win, 0, 0, WIN_W, g_win_h, COL_BORDER);
    gui_draw_rect_outline(g_win, 1, 1, WIN_W - 2, g_win_h - 2, COL_CARD);

    // Header.
    win_draw_text_ttf(g_win, PAD + 4, PAD + 2, "Task Switcher", 17, COL_TEXT);
    char cnt[40];
    snprintf(cnt, sizeof(cnt), "%d window%s", g_count, g_count == 1 ? "" : "s");
    int cw = gui_ttf_width(cnt, 12);
    win_draw_text_ttf(g_win, WIN_W - PAD - cw - 4, PAD + 6, cnt, 12, COL_DIM);
    gui_draw_hline(g_win, PAD, PAD + HEAD_H - 6, WIN_W - PAD * 2, COL_BORDER);

    int ly = PAD + HEAD_H;
    int lw = WIN_W - PAD * 2 - MAP_W;

    if (g_count == 0) {
        win_draw_text_ttf(g_win, PAD + 8, ly + 8, "No open windows", 15, COL_DIM);
    }
    for (int r = 0; r < rows && (g_scroll + r) < g_count; r++) {
        int i = g_scroll + r;
        wm_window_info_t *w = &g_wins[i];
        int ry = ly + r * ROW_H;
        int selected = (i == g_sel);
        if (selected) {
            win_draw_rect(g_win, PAD, ry, lw, ROW_H, COL_SEL);
            win_draw_rect(g_win, PAD, ry, 3, ROW_H, COL_ACCENT);
        }
        uint32_t tc = selected ? COL_SEL_TEXT : COL_TEXT;
        // Number badge 1..9 for quick pick.
        if (i < 9) {
            char nb[3] = { (char)('1' + i), 0, 0 };
            win_draw_text_ttf(g_win, PAD + 10, ry + (ROW_H - 13) / 2, nb, 12,
                              selected ? COL_SEL_TEXT : COL_DIM);
        }
        // Title, truncated to fit.
        char title[64];
        strncpy(title, w->title[0] ? w->title : "(untitled)", sizeof(title) - 1);
        title[sizeof(title) - 1] = 0;
        int avail = lw - 44 - 90;
        while (title[0] && gui_ttf_width(title, 15) > avail) {
            int tl = (int)strlen(title);
            title[tl - 1] = 0;
            if (tl >= 3) { title[tl-2] = '.'; title[tl-3] = '.'; }
        }
        win_draw_text_ttf(g_win, PAD + 28, ry + (ROW_H - 16) / 2, title, 15, tc);
        // Status column: focused / minimized / geometry.
        const char *st = w->focused ? "current" : (w->minimized ? "minimized" : "");
        if (st[0]) {
            uint32_t sc = w->minimized ? COL_WARN
                                       : (selected ? COL_SEL_TEXT : COL_ACCENT);
            int stw = gui_ttf_width(st, 11);
            win_draw_text_ttf(g_win, PAD + lw - stw - 10, ry + (ROW_H - 12) / 2,
                              st, 11, sc);
        }
    }

    // Scroll marker.
    if (g_count > VISIBLE_MAX) {
        int th = list_h * VISIBLE_MAX / g_count;
        if (th < 12) th = 12;
        int denom = g_count - VISIBLE_MAX;
        int ty = ly + (list_h - th) * g_scroll / (denom > 0 ? denom : 1);
        win_draw_rect(g_win, PAD + lw + 2, ly, 3, list_h, COL_CARD);
        win_draw_rect(g_win, PAD + lw + 2, ty, 3, th, COL_ACCENT);
    }

    // Mini screen map on the right.
    draw_mini_map(PAD + lw + 16, ly);

    // Footer hints.
    int map_h = (MAP_W - 16) * g_sh / (g_sw > 0 ? g_sw : 1) + 20;
    int body = list_h > map_h ? list_h : map_h;
    int fy = ly + body + 4;
    gui_draw_hline(g_win, PAD, fy, WIN_W - PAD * 2, COL_BORDER);
    win_draw_text_ttf(g_win, PAD + 4, fy + 5,
        "Tab/Shift+Tab select   Enter or release Alt focus   M min   Esc close",
        12, COL_DIM);

    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void focus_selected(void) {
    // #alttab: this is now also the Alt-release commit path (see the
    // EVENT_KEY_UP case in main()), not just Enter/Space/click/1-9. With
    // ZERO other windows open g_count is 0 and g_sel is clamped to 0 by
    // refresh_windows(), so `g_sel >= g_count` is true and this function
    // used to return here WITHOUT closing the overlay - Enter, and now
    // releasing Alt, would leave the switcher stuck open forever with
    // nothing to select. There is nothing to focus in that case, but the
    // overlay must still dismiss: skip wm_focus(), never skip the close.
    if (g_sel >= 0 && g_sel < g_count) {
        wm_focus(g_wins[g_sel].id);
    }
    win_destroy(g_win);
    sys_exit(0);
}

static void minimize_selected(void) {
    if (g_sel < 0 || g_sel >= g_count) return;
    int id = g_wins[g_sel].id;
    wm_minimize(id);
    refresh_windows(id);
}

static void move_sel(int delta) {
    if (g_count == 0) return;
    g_sel += delta;
    if (g_sel < 0) g_sel = g_count - 1;    // wrap
    if (g_sel >= g_count) g_sel = 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + VISIBLE_MAX) g_scroll = g_sel - VISIBLE_MAX + 1;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    apply_theme(get_theme());

    fb_info_t fbi;
    if (fb_info(&fbi) == 0 && fbi.width > 0 && fbi.height > 0) {
        g_sw = (int)fbi.width; g_sh = (int)fbi.height;
    }

    refresh_windows(-1);

    g_win_h = calc_height();
    int wx = (g_sw - WIN_W) / 2;
    int wy = (g_sh - g_win_h) / 3;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;

    g_win = win_create(OWN_TITLE, wx, wy, WIN_W, g_win_h);
    if (g_win < 0) return 1;
    win_set_nochrome(g_win);
    // Creating our own window changed the list (and stole focus); re-query so
    // the "current" tag still points at the window the user came from.
    refresh_windows(g_count ? g_wins[g_sel].id : -1);

    // #alttab: seed the shared modifier tracker from LIVE physical state
    // (gui_mods.h) rather than waiting for an event, and handle the race a
    // held-Alt launch has by construction: the compositor spawns this app on
    // the FIRST Tab of an Alt+Tab hold, but spawning is async (fork+exec),
    // so a very fast tap-and-release can have Alt already back up by the
    // time this window exists and takes focus - no EVENT_KEY_UP for Alt will
    // ever arrive here in that case, because the kernel already delivered it
    // (to whatever had focus a moment ago, before this window existed).
    // Rather than leave the overlay open with no way to commit, treat "Alt
    // already up at startup" the same as "Alt released": commit immediately
    // to the MRU target (index 1, preselected by refresh_windows() above).
    // A normal held Alt+Tab never takes this branch (gui_mods_is is false).
    gui_mods_resync();
    // #alttab BUGFIX: gui_mods_is() is an EXACT chord match (by design, see
    // gui_mods.h - it is what tells Ctrl+C apart from Ctrl+Shift+C), so it
    // requires the held set to be Alt and ONLY Alt. That is wrong here: Alt
    // is by definition always the base of this whole gesture, and a user is
    // free to also be holding Shift (Alt+Shift+Tab, straight into reverse)
    // the instant this window opens. A plain bitwise test of gui_mods_get()
    // is what "is Alt currently held, regardless of what else is" means.
    if (!(gui_mods_get() & GUI_MOD_ALT)) {
        focus_selected();   // never returns: wm_focus + win_destroy + exit
    }
    draw_all();

    gui_event_t ev;
    int running = 1;
    int ticks = 0;
    while (running) {
        int et = gui_mods_next_event(g_win, &ev, 250);
        if (et == 0) {
            // Periodic refresh keeps the overlay honest while it is open.
            if (++ticks >= 2) {
                ticks = 0;
                int keep = (g_sel < g_count) ? g_wins[g_sel].id : -1;
                refresh_windows(keep);
                draw_all();
            }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW:
                draw_all();
                break;
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
            case EVENT_WINDOW_FOCUS:
                g_had_focus = 1;
                break;
            case EVENT_WINDOW_BLUR:
                if (g_had_focus) running = 0;   // overlay: click-away dismisses
                break;
            case EVENT_MOUSE_MOVE: {
                int ly = PAD + HEAD_H;
                int lw = WIN_W - PAD * 2 - MAP_W;
                if (ev.mouse_x >= PAD && ev.mouse_x < PAD + lw &&
                    ev.mouse_y >= ly && ev.mouse_y < ly + list_rows() * ROW_H) {
                    int i = g_scroll + (ev.mouse_y - ly) / ROW_H;
                    if (i < g_count && i != g_sel) { g_sel = i; draw_all(); }
                }
                break;
            }
            case EVENT_MOUSE_DOWN: {
                if (!(ev.mouse_buttons & MOUSE_BUTTON_LEFT)) break;
                int ly = PAD + HEAD_H;
                int lw = WIN_W - PAD * 2 - MAP_W;
                if (ev.mouse_x >= PAD && ev.mouse_x < PAD + lw &&
                    ev.mouse_y >= ly && ev.mouse_y < ly + list_rows() * ROW_H) {
                    int i = g_scroll + (ev.mouse_y - ly) / ROW_H;
                    if (i < g_count) { g_sel = i; focus_selected(); }
                }
                break;
            }
            case EVENT_MOUSE_SCROLL:
                if (ev.scroll_delta > 0) move_sel(-1); else move_sel(1);
                draw_all();
                break;
            case EVENT_KEY_UP: {
                // #alttab: releasing Alt COMMITS, same as Enter, so "hold
                // Alt, tap Tab (any number of times), release" behaves the
                // way every other Alt-Tab implementation does. gui_mods_feed()
                // (called by gui_mods_next_event() above) already folded this
                // into the tracker's state; GUI_KEY_ALT_DELIVERED_REL is the
                // value SYS_INJECT_KEY actually delivers for an Alt release
                // (equal to the press code, #232 - see keys.h), not the raw
                // GUI_KEY_ALT_UP the kernel pushes into the cooked ring.
                if (ev.keycode == GUI_KEY_ALT_DELIVERED_REL) {
                    focus_selected();   // never returns
                }
                break;
            }
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode;
                char c = ev.key_char;
                g_had_focus = 1;
                if (c == 27 || kc == 0x01) { running = 0; break; }        // Esc
                if (kc == 0x1C || c == '\n' || c == '\r' || c == ' ') {
                    focus_selected(); break;                              // Enter/Space
                }
                if (kc == 0x80) { move_sel(-1); draw_all(); break; }      // Up
                if (kc == 0x81) { move_sel(1); draw_all(); break; }       // Down
                if (kc == GUI_KEY_TAB || c == '\t') {                     // Tab
                    // #alttab: Shift+Tab walks backwards, via the shared
                    // modifier tracker fed by gui_mods_next_event() above -
                    // Tab itself carries no shift information in key_char
                    // (Tab has no shifted ASCII form), so this is the only
                    // reliable way to tell the two apart.
                    // #alttab BUGFIX (MEASURED on a live VM): gui_mods_is()
                    // is an EXACT chord match, and Alt is ALWAYS also held
                    // for every Tab this app ever sees (that is the whole
                    // gesture), so gui_mods_is(GUI_MOD_SHIFT) can never be
                    // true here - it requires Shift and ONLY Shift. Reverse
                    // cycling silently never fired until this was changed to
                    // a plain bitwise "is the Shift bit set, regardless of
                    // Alt" test.
                    if (gui_mods_get() & GUI_MOD_SHIFT) move_sel(-1);
                    else move_sel(1);
                    draw_all(); break;
                }
                if (c == 'm' || c == 'M') { minimize_selected(); draw_all(); break; }
                if (c == 'r' || c == 'R') {
                    refresh_windows(g_sel < g_count ? g_wins[g_sel].id : -1);
                    draw_all(); break;
                }
                if (c >= '1' && c <= '9') {                               // quick pick
                    int i = c - '1';
                    if (i < g_count) { g_sel = i; focus_selected(); }
                    break;
                }
                break;
            }
            default: break;
        }
    }
    win_destroy(g_win);
    return 0;
}
