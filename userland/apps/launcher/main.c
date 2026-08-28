// launcher - MayteraOS Command Palette / app launcher.
// A borderless, centered, keyboard-first palette: type a few letters, fuzzy
// search across the app catalog (built-in registry + everything in /APPS),
// Enter launches via sys_spawn. Fills the gap where the start menu is fully
// mouse-driven: this is the fast "type to launch" path.
//
//   Up/Down or Tab   move selection        Enter   launch selected app
//   printable keys   filter (fuzzy)        Esc     dismiss
//   mouse hover/click/scroll also work; losing focus dismisses the palette.
//
// Self-contained companion app: uses only existing syscalls (win_create,
// win_set_nochrome, sys_readdir, sys_spawn, fb_info) and the shared theme
// palette + TTF text, per docs/UI_STYLE_GUIDE.md.

#include "syscall.h"
#include "gui.h"
#include "gui_style.h"
#include "string.h"
#include "stdio.h"
#include "userconf.h"   // #745: <home>/APPS, the per-user application directory

#define WIN_W       560
#define MAX_APPS    192
#define MAX_QUERY   48
#define ROW_H       34
#define VISIBLE_MAX 9
#define FIELD_H     40
#define FOOT_H      24
#define PAD         10

typedef struct {
    char name[40];      // display name
    // #745: widened from 64. A per-user spawn path carries the home prefix
    // ("/HOME/ADMIN/APPS/counter"), and a TRUNCATED spawn path is not a shorter
    // path, it is a DIFFERENT one that does not exist, so the app would appear
    // in the palette and refuse to start. scan_apps_in() also skips anything
    // that still would not fit rather than storing a truncated entry.
    char path[160];     // spawn path
    char hint[24];      // category / keyword shown dimmed on the right
    int  builtin;       // 1 = curated registry entry, 0 = /APPS scan
} app_entry_t;

static app_entry_t g_apps[MAX_APPS];
static int g_app_count = 0;

// Filter results: indexes into g_apps, sorted by score (desc).
static int g_match[MAX_APPS];
static int g_score[MAX_APPS];
static int g_match_count = 0;

static char g_query[MAX_QUERY + 1];
static int  g_qlen = 0;
static int  g_sel = 0;          // index into g_match
static int  g_scroll = 0;       // first visible row
static int  g_win = -1;
static int  g_win_h = 0;
static int  g_had_focus = 0;

// ---------------------------------------------------------------------------
// Theme palette (follows the kernel theme id, same scheme as Notes/Settings)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_CARD, COL_BORDER, COL_TEXT, COL_TEXT2, COL_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEL, COL_SEL_TEXT;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00F4F4F4; COL_CARD=0x00FFFFFF; COL_BORDER=0x00B8B8B8;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00C4C4C4;
            COL_SEL=0x00D6E4FB; COL_SEL_TEXT=0x00202020; break;
        case 4:  // Classic (CDE/Motif)
            COL_BG=0x00C0C0C0; COL_CARD=0x00C0C0C0; COL_BORDER=0x00404040;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_DIM=0x00707070;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_SEL=0x00000080; COL_SEL_TEXT=0x00FFFFFF; break;
        case 5:  // Ocean
            COL_BG=0x001E4050; COL_CARD=0x00224455; COL_BORDER=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_SEL=0x00305060; COL_SEL_TEXT=0x00E0F0FF; break;
        case 9:  // Nord
            COL_BG=0x002E3440; COL_CARD=0x00343B49; COL_BORDER=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_SEL=0x00434C5E; COL_SEL_TEXT=0x00ECEFF4; break;
        default: // Dark
            COL_BG=0x00202020; COL_CARD=0x002A2A2A; COL_BORDER=0x00484848;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_SEL=0x003A5A80; COL_SEL_TEXT=0x00FFFFFF; break;
    }
}

// ---------------------------------------------------------------------------
// App catalog
// ---------------------------------------------------------------------------
static void add_app(const char *name, const char *path, const char *hint, int builtin) {
    if (g_app_count >= MAX_APPS) return;
    // De-dupe by spawn path (case-insensitive: FAT is case-preserving-ish).
    for (int i = 0; i < g_app_count; i++)
        if (strcasecmp(g_apps[i].path, path) == 0) return;
    app_entry_t *a = &g_apps[g_app_count++];
    strncpy(a->name, name, sizeof(a->name) - 1); a->name[sizeof(a->name)-1] = 0;
    strncpy(a->path, path, sizeof(a->path) - 1); a->path[sizeof(a->path)-1] = 0;
    strncpy(a->hint, hint, sizeof(a->hint) - 1); a->hint[sizeof(a->hint)-1] = 0;
    a->builtin = builtin;
}

// Curated registry: mirrors the compositor start menu entries so the palette
// shows friendly names and categories for the first-class apps.
static void load_registry(void) {
    add_app("Terminal",      "/APPS/TERMINAL",  "System",     1);
    add_app("Files",         "/APPS/FILES",     "System",     1);
    add_app("Browser",       "/APPS/BROWSER",   "Internet",   1);
    add_app("Editor",        "/APPS/EDITOR",    "Productivity",1);
    add_app("Notes",         "/APPS/NOTES",     "Productivity",1);
    add_app("Calculator",    "/APPS/CALC",      "Productivity",1);
    add_app("World Clock",   "/APPS/clock",     "Productivity",1);
    add_app("Paint",         "/APPS/PAINT",     "Graphics",   1);
    add_app("Image Viewer",  "/APPS/IMAGEVIEWER",   "Graphics",   1);
    add_app("Media Player",  "/APPS/MEDIAPLAYER",   "Media",      1);
    add_app("Music Player",  "/APPS/MUSICPLR",  "Media",      1);
    add_app("IRC",           "/APPS/IRC",       "Internet",   1);
    add_app("AI Chat",       "/APPS/AICHAT",    "Internet",   1);
    add_app("Authenticator", "/APPS/mfa",       "Security",   1);
    add_app("Settings",      "/APPS/SETTINGS",  "System",     1);
    add_app("Task Manager",  "/APPS/taskmgr",   "System",     1);
    add_app("Task Switcher", "/APPS/WINSWTCH",  "System",     1);
    add_app("App Repo",      "/APPS/APPSTORE",  "System",     1);
    add_app("Help",          "/APPS/help",      "System",     1);
    add_app("Python",        "/APPS/PYTHON.ELF","Development",1);
    add_app("DOOM",          "/GAMES/DOOM/DOOM.ELF",  "Games",      1);
    add_app("Solitaire",     "/APPS/SOLITAIRE",    "Games",      1);
    add_app("Lemmings",      "/APPS/lemmings",  "Games",      1);
    add_app("Pong",          "/APPS/pong",      "Games",      1);
    add_app("GL Cube",       "/APPS/GLCUBE",    "Games",      1);
    add_app("GL Matrix",     "/APPS/GLMATRIX",  "Games",      1);
}

// Everything else on disk: scan an applications directory and append entries
// the registry did not already claim, so newly installed apps are launchable
// with zero config. add_app() de-dupes by spawn path, so a directory scanned
// twice costs nothing.
static void scan_apps_in(const char *dir, const char *hint) {
    dirent_t ent;
    for (int idx = 0; idx < 256 && g_app_count < MAX_APPS; idx++) {
        if (sys_readdir(dir, idx, &ent) != 0) break;
        if (DIRENT_IS_DIR(ent)) continue;
        if (ent.name[0] == '.' || ent.name[0] == 0) continue;
        // Display name: file name without a trailing ".ELF"/".elf".
        char disp[40];
        strncpy(disp, ent.name, sizeof(disp) - 1); disp[sizeof(disp)-1] = 0;
        int dl = (int)strlen(disp);
        if (dl > 4 && strcasecmp(disp + dl - 4, ".ELF") == 0) disp[dl-4] = 0;
        // Refuse rather than truncate: see the app_entry_t.path comment.
        if (strlen(dir) + 1 + strlen(ent.name) + 1 > sizeof(g_apps[0].path)) continue;
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", dir, ent.name);
        add_app(disp, path, hint, 0);
    }
}

static void scan_apps_dir(void) {
    scan_apps_in("/APPS", "app");

    // #745: the SESSION USER'S own application directory, <home>/APPS. An app
    // the user installed for themselves lives here and lives nowhere else, so
    // without this feed the launcher palette cannot see it at all. Scanned
    // SECOND so a same-named system app keeps the curated registry entry it
    // already has (add_app de-dupes by spawn path, and the two paths differ, so
    // both appear; they are genuinely different binaries).
    //
    // Root's home is "/", so this resolves to "/APPS" and is deduped away
    // entry by entry: a root session's palette is unchanged.
    char hdir[192];
    if (userhome_path(0, "APPS", hdir, sizeof(hdir)) == 0)
        scan_apps_in(hdir, "installed for you");
}

// ---------------------------------------------------------------------------
// Fuzzy matching: case-insensitive subsequence with scoring. Higher = better.
// Bonus for match at word start, consecutive runs, and full-prefix matches.
// Returns score > 0 on match, 0 on no match.
// ---------------------------------------------------------------------------
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int fuzzy_score(const char *hay, const char *needle) {
    if (!needle[0]) return 1;               // empty query matches everything
    int score = 0, run = 0, hi = 0, prev_hi = -2;
    for (int ni = 0; needle[ni]; ni++) {
        int nc = lower((unsigned char)needle[ni]);
        int found = -1;
        for (int i = hi; hay[i]; i++) {
            if (lower((unsigned char)hay[i]) == nc) { found = i; break; }
        }
        if (found < 0) return 0;
        // Base point per matched char.
        int pts = 4;
        if (found == prev_hi + 1) { run++; pts += 6 + run * 2; }  // consecutive
        else run = 0;
        if (found == 0) pts += 12;                                // string start
        else if (hay[found-1] == ' ' || hay[found-1] == '/' ||
                 hay[found-1] == '_' || hay[found-1] == '-') pts += 8; // word start
        pts -= (found - hi);                                      // gap penalty
        if (pts < 1) pts = 1;
        score += pts;
        prev_hi = found;
        hi = found + 1;
    }
    return score;
}

static void refilter(void) {
    g_match_count = 0;
    for (int i = 0; i < g_app_count; i++) {
        int s = fuzzy_score(g_apps[i].name, g_query);
        if (s == 0) s = fuzzy_score(g_apps[i].path, g_query) / 2;
        if (s <= 0) continue;
        if (g_apps[i].builtin) s += 2;      // curated entries win ties
        // Insertion sort by score descending; stable for equal scores.
        int j = g_match_count;
        while (j > 0 && g_score[j-1] < s) {
            g_match[j] = g_match[j-1]; g_score[j] = g_score[j-1]; j--;
        }
        g_match[j] = i; g_score[j] = s;
        g_match_count++;
    }
    if (g_sel >= g_match_count) g_sel = g_match_count ? g_match_count - 1 : 0;
    if (g_sel < 0) g_sel = 0;
    g_scroll = 0;
    if (g_sel >= VISIBLE_MAX) g_scroll = g_sel - VISIBLE_MAX + 1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static int list_rows(void) {
    int n = g_match_count;
    return (n > VISIBLE_MAX) ? VISIBLE_MAX : (n < 1 ? 1 : n);
}

static void draw_all(void) {
    int rows = list_rows();
    int list_h = rows * ROW_H;

    // Panel background + border (borderless window: we draw our own frame).
    win_draw_rect(g_win, 0, 0, WIN_W, g_win_h, COL_BG);
    gui_draw_rect_outline(g_win, 0, 0, WIN_W, g_win_h, COL_BORDER);
    gui_draw_rect_outline(g_win, 1, 1, WIN_W - 2, g_win_h - 2, COL_CARD);

    // Search field.
    int fx = PAD, fy = PAD, fw = WIN_W - PAD * 2;
    win_draw_rect(g_win, fx, fy, fw, FIELD_H, COL_FIELD);
    gui_draw_rect_outline(g_win, fx, fy, fw, FIELD_H, COL_FIELD_BORDER);
    gui_draw_rect_outline(g_win, fx + 1, fy + 1, fw - 2, FIELD_H - 2, COL_ACCENT);
    if (g_qlen > 0) {
        win_draw_text_ttf(g_win, fx + 12, fy + (FIELD_H - 18) / 2, g_query, 17, COL_TEXT);
    } else {
        win_draw_text_ttf(g_win, fx + 12, fy + (FIELD_H - 18) / 2,
                          "Type to search apps...", 17, COL_DIM);
    }
    // Caret.
    int cw = g_qlen ? gui_ttf_width(g_query, 17) : 0;
    win_draw_rect(g_win, fx + 12 + cw + 2, fy + 8, 2, FIELD_H - 16, COL_ACCENT);

    // Results.
    int ly = fy + FIELD_H + 6;
    if (g_match_count == 0) {
        win_draw_text_ttf(g_win, fx + 12, ly + 8, "No matching apps", 15, COL_DIM);
    }
    for (int r = 0; r < rows && (g_scroll + r) < g_match_count; r++) {
        int mi = g_scroll + r;
        app_entry_t *a = &g_apps[g_match[mi]];
        int ry = ly + r * ROW_H;
        int selected = (mi == g_sel);
        if (selected) {
            win_draw_rect(g_win, fx, ry, fw, ROW_H, COL_SEL);
            win_draw_rect(g_win, fx, ry, 3, ROW_H, COL_ACCENT);
        }
        uint32_t tc = selected ? COL_SEL_TEXT : COL_TEXT;
        win_draw_text_ttf(g_win, fx + 14, ry + (ROW_H - 16) / 2, a->name, 15, tc);
        // Right-aligned dimmed hint (category or "app").
        int hw = gui_ttf_width(a->hint, 12);
        win_draw_text_ttf(g_win, fx + fw - hw - 12, ry + (ROW_H - 13) / 2,
                          a->hint, 12, selected ? COL_SEL_TEXT : COL_DIM);
        // Path in small text under the name for scanned entries only when selected.
        if (selected) {
            int nw = gui_ttf_width(a->name, 15);
            win_draw_text_ttf(g_win, fx + 14 + nw + 10, ry + (ROW_H - 12) / 2,
                              a->path, 11, COL_TEXT2);
        }
    }

    // Scroll marker.
    if (g_match_count > VISIBLE_MAX) {
        int th = list_h * VISIBLE_MAX / g_match_count;
        if (th < 12) th = 12;
        int ty = ly + (list_h - th) * g_scroll /
                 (g_match_count - VISIBLE_MAX > 0 ? g_match_count - VISIBLE_MAX : 1);
        win_draw_rect(g_win, WIN_W - 6, ly, 3, list_h, COL_CARD);
        win_draw_rect(g_win, WIN_W - 6, ty, 3, th, COL_ACCENT);
    }

    // Footer hints.
    int fy2 = ly + list_h + 4;
    gui_draw_hline(g_win, PAD, fy2, WIN_W - PAD * 2, COL_BORDER);
    char foot[80];
    snprintf(foot, sizeof(foot), "%d app%s   Up/Down select   Enter launch   Esc close",
             g_match_count, g_match_count == 1 ? "" : "s");
    win_draw_text_ttf(g_win, PAD + 4, fy2 + 5, foot, 12, COL_DIM);

    win_invalidate(g_win);
}

// Window height tracks how many rows are visible so the palette hugs content.
static int calc_height(void) {
    return PAD + FIELD_H + 6 + list_rows() * ROW_H + 4 + FOOT_H;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void launch_selected(void) {
    if (g_sel < 0 || g_sel >= g_match_count) return;
    app_entry_t *a = &g_apps[g_match[g_sel]];
    sys_spawn(a->path);
    // Palette semantics: launch and get out of the way.
    win_destroy(g_win);
    sys_exit(0);
}

static void move_sel(int delta) {
    if (g_match_count == 0) return;
    g_sel += delta;
    if (g_sel < 0) g_sel = g_match_count - 1;          // wrap
    if (g_sel >= g_match_count) g_sel = 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + VISIBLE_MAX) g_scroll = g_sel - VISIBLE_MAX + 1;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    apply_theme(get_theme());
    load_registry();
    scan_apps_dir();
    g_query[0] = 0;
    refilter();

    // Center on screen, upper third (command-palette placement).
    fb_info_t fbi; int sw = 1024, sh = 768;
    if (fb_info(&fbi) == 0 && fbi.width > 0 && fbi.height > 0) {
        sw = (int)fbi.width; sh = (int)fbi.height;
    }
    g_win_h = calc_height();
    int wx = (sw - WIN_W) / 2;
    int wy = sh / 6;
    if (wx < 0) wx = 0;
    if (wy + g_win_h > sh) wy = (sh - g_win_h) / 2;

    g_win = win_create("Launcher", wx, wy, WIN_W, g_win_h);
    if (g_win < 0) return 1;
    win_set_nochrome(g_win);
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int et = win_get_event(g_win, &ev, 250);
        if (et == 0) continue;              // idle tick, nothing to animate
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
                // Palette behavior: clicking elsewhere dismisses it.
                if (g_had_focus) running = 0;
                break;
            case EVENT_MOUSE_MOVE: {
                int ly = PAD + FIELD_H + 6;
                if (ev.mouse_y >= ly && ev.mouse_y < ly + list_rows() * ROW_H) {
                    int row = (ev.mouse_y - ly) / ROW_H;
                    int mi = g_scroll + row;
                    if (mi < g_match_count && mi != g_sel) { g_sel = mi; draw_all(); }
                }
                break;
            }
            case EVENT_MOUSE_DOWN: {
                if (!(ev.mouse_buttons & MOUSE_BUTTON_LEFT)) break;
                int ly = PAD + FIELD_H + 6;
                if (ev.mouse_y >= ly && ev.mouse_y < ly + list_rows() * ROW_H) {
                    int row = (ev.mouse_y - ly) / ROW_H;
                    int mi = g_scroll + row;
                    if (mi < g_match_count) { g_sel = mi; launch_selected(); }
                }
                break;
            }
            case EVENT_MOUSE_SCROLL:
                if (ev.scroll_delta > 0) move_sel(-1); else move_sel(1);
                draw_all();
                break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode;
                char c = ev.key_char;
                g_had_focus = 1;
                if (c == 27 || kc == 0x01) { running = 0; break; }        // Esc
                if (kc == 0x1C || c == '\n' || c == '\r') { launch_selected(); break; }
                if (kc == 0x80) { move_sel(-1); draw_all(); break; }      // Up
                if (kc == 0x81 || kc == 0x0F || c == '\t') {              // Down/Tab
                    move_sel(1); draw_all(); break;
                }
                if (c == '\b' || kc == 0x0E) {                            // Backspace
                    if (g_qlen > 0) {
                        g_query[--g_qlen] = 0;
                        refilter(); g_sel = 0; g_scroll = 0;
                        int nh = calc_height();
                        if (nh != g_win_h) g_win_h = nh;
                        draw_all();
                    }
                    break;
                }
                if (c >= 32 && c < 127 && g_qlen < MAX_QUERY) {           // printable
                    g_query[g_qlen++] = c; g_query[g_qlen] = 0;
                    refilter(); g_sel = 0; g_scroll = 0;
                    int nh = calc_height();
                    if (nh != g_win_h) g_win_h = nh;
                    draw_all();
                }
                break;
            }
            default: break;
        }
    }
    win_destroy(g_win);
    return 0;
}
