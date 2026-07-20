// help - the MayteraOS Help viewer (task #267).
//
// A two-pane help browser styled with the libc style engine to match Settings
// and Files: a left TOC pane listing topics, a right pane that renders the
// document's blocks via TTF (headings larger/bold, paragraphs, bullet lists,
// links in the accent color and clickable to navigate), a top toolbar with
// Back / Forward / Home and a search box, and a status line.
//
// Usage: help [<file.mhlp|file.hlp> [<topic-id>]]
//   With no argument it opens the system help /HELP/SYSTEM.MHLP.
//
// External (http) links are NOT followed; clicking one shows a note in the
// status line. Internal topic links navigate within the document.
//
// Robustness: a malformed help file never crashes the app. If the file cannot
// be opened or parsed, the viewer shows an error topic.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libhelp/help.h"

// Route in-window text through the antialiased TTF path, like Settings.
#define TXT(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

#define WIN_W      820
#define WIN_H      600
#define TOOLBAR_H  44
#define STATUS_H   24
#define TOC_W      220
#define CONTENT_X  (TOC_W + 1)
#define CONTENT_W  (WIN_W - TOC_W - 1)
#define CONTENT_Y0 (TOOLBAR_H + 1)
#define CONTENT_H  (WIN_H - TOOLBAR_H - STATUS_H - 2)

// Palette (filled from the kernel theme, like Settings does).
static gui_palette_t PAL;

// Document state
static help_doc_t *g_doc = NULL;
static char        g_path[128];
static int         g_cur = 0;       // current topic index
static int         g_scroll = 0;    // pixels scrolled in content pane

// Navigation history (simple back/forward stacks of topic indices).
#define HIST_MAX 64
static int g_hist[HIST_MAX];
static int g_hist_len = 0;          // number of entries
static int g_hist_pos = -1;         // index of current within history

// Search
static char g_search[64];
static int  g_search_len = 0;
static int  g_search_focus = 0;
static int *g_results = NULL;
static int  g_results_n = 0;

static char g_status[160] = "Ready.";

// Clickable-link hit map built during render of the content pane.
typedef struct { int x, y, w, h; int topic; int external; } link_hit_t;
#define LINK_MAX 256
static link_hit_t g_links[LINK_MAX];
static int g_link_n = 0;

// TOC row hit map.
typedef struct { int y, h, topic; } toc_hit_t;
#define TOC_MAX 256
static toc_hit_t g_toc[TOC_MAX];
static int g_toc_n = 0;

static int g_win = -1;

// ---------------------------------------------------------------------------
static void set_status(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(g_status) - 1) { g_status[i] = s[i]; i++; }
    g_status[i] = 0;
}

static void load_palette(void) {
    int kt = get_theme();
    // Dark default; map a couple of known themes to light surfaces.
    if (kt == 2) {  // Light
        PAL.surface = 0x00F0F0F0; PAL.surface_raised = 0x00FFFFFF;
        PAL.ink = 0x00202020; PAL.ink_dim = 0x00606060;
        PAL.accent = 0x00569CD6; PAL.accent_hover = 0x006FB0E0;
        PAL.border = 0x00C0C0C0; PAL.field_bg = 0x00FFFFFF;
        PAL.field_border = 0x00A0A0A0; PAL.track = 0x00D0D0D0;
        gui_set_style(GUI_STYLE_MODERN);
    } else if (kt == 4) {  // Classic
        PAL.surface = 0x00C0C0C0; PAL.surface_raised = 0x00D4D0C8;
        PAL.ink = 0x00000000; PAL.ink_dim = 0x00404040;
        PAL.accent = 0x00000080; PAL.accent_hover = 0x000000A0;
        PAL.border = 0x00808080; PAL.field_bg = 0x00FFFFFF;
        PAL.field_border = 0x00808080; PAL.track = 0x00A0A0A0;
        gui_set_style(GUI_STYLE_CLASSIC);
    } else {  // Dark / others
        PAL.surface = 0x001E1E1E; PAL.surface_raised = 0x00252526;
        PAL.ink = 0x00E0E0E0; PAL.ink_dim = 0x00A0A0A0;
        PAL.accent = 0x00569CD6; PAL.accent_hover = 0x006FB0E0;
        PAL.border = 0x003C3C3C; PAL.field_bg = 0x00333333;
        PAL.field_border = 0x00505050; PAL.track = 0x00404040;
        gui_set_style(GUI_STYLE_MODERN);
    }
    gui_set_palette(&PAL);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
static void history_push(int topic) {
    if (g_hist_pos >= 0 && g_hist_pos < g_hist_len && g_hist[g_hist_pos] == topic)
        return;  // already here
    // truncate forward history
    g_hist_len = g_hist_pos + 1;
    if (g_hist_len >= HIST_MAX) {
        // shift down by one
        for (int i = 1; i < HIST_MAX; i++) g_hist[i-1] = g_hist[i];
        g_hist_len = HIST_MAX - 1;
        g_hist_pos--;
    }
    g_hist[g_hist_len++] = topic;
    g_hist_pos = g_hist_len - 1;
}

static void go_topic(int topic, int push) {
    int n = help_topic_count(g_doc);
    if (topic < 0 || topic >= n) return;
    g_cur = topic;
    g_scroll = 0;
    if (push) history_push(topic);
    const help_topic_t *t = help_topic_at(g_doc, topic);
    if (t) set_status(t->title);
}

static void go_back(void) {
    if (g_hist_pos > 0) { g_hist_pos--; go_topic(g_hist[g_hist_pos], 0); }
}
static void go_forward(void) {
    if (g_hist_pos >= 0 && g_hist_pos < g_hist_len - 1) {
        g_hist_pos++; go_topic(g_hist[g_hist_pos], 0);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void draw_toolbar(void) {
    gui_fill_rect(g_win, 0, 0, WIN_W, TOOLBAR_H, PAL.surface_raised);
    gui_draw_hline(g_win, 0, TOOLBAR_H, WIN_W, PAL.border);

    // Back / Forward / Home buttons.
    int bx = 8, by = 8, bw = 64, bh = 28;
    int can_back = (g_hist_pos > 0);
    int can_fwd  = (g_hist_pos >= 0 && g_hist_pos < g_hist_len - 1);
    gui_button(g_win, bx, by, bw, bh, "< Back", GUI_BTN_SECONDARY,
               can_back ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    bx += bw + 6;
    gui_button(g_win, bx, by, bw, bh, "Fwd >", GUI_BTN_SECONDARY,
               can_fwd ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    bx += bw + 6;
    gui_button(g_win, bx, by, bw, bh, "Home", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    bx += bw + 14;

    // Search box on the right.
    int sw = 220;
    int sx = WIN_W - sw - 10;
    gui_textfield2(g_win, sx, by, sw, bh,
                   g_search_len ? g_search : "Search...", g_search_focus);
}

static int toolbar_hit(int mx, int my) {
    // returns: 1=back 2=fwd 3=home 4=search, else 0
    if (my < 8 || my > 36) return 0;
    int bx = 8, bw = 64;
    if (mx >= bx && mx < bx + bw) return 1;
    bx += bw + 6;
    if (mx >= bx && mx < bx + bw) return 2;
    bx += bw + 6;
    if (mx >= bx && mx < bx + bw) return 3;
    int sw = 220, sx = WIN_W - sw - 10;
    if (mx >= sx && mx < sx + sw) return 4;
    return 0;
}

static void draw_toc(void) {
    gui_fill_rect(g_win, 0, CONTENT_Y0, TOC_W, CONTENT_H, PAL.surface_raised);
    gui_draw_vline(g_win, TOC_W, CONTENT_Y0, CONTENT_H, PAL.border);

    g_toc_n = 0;
    int y = CONTENT_Y0 + 6;
    int n = help_topic_count(g_doc);
    int *list = NULL, ln = 0;

    // If a search is active and has results, show only the results.
    if (g_results && g_results_n > 0) {
        TXT(g_win, 10, y, "Search results", 11, PAL.ink_dim);
        y += 20;
        list = g_results; ln = g_results_n;
    }

    for (int i = 0; i < (list ? ln : n); i++) {
        int ti = list ? list[i] : i;
        const help_topic_t *t = help_topic_at(g_doc, ti);
        if (!t) continue;
        int rh = 24;
        int selected = (ti == g_cur);
        if (selected) gui_fill_rect(g_win, 4, y - 2, TOC_W - 8, rh, PAL.accent);
        uint32_t col = selected ? 0x00FFFFFF : PAL.ink;
        // truncate long titles to fit
        char buf[40];
        int k = 0;
        while (t->title[k] && k < 36) { buf[k] = t->title[k]; k++; }
        buf[k] = 0;
        TXT(g_win, 12, y + 2, buf, 13, col);
        if (g_toc_n < TOC_MAX) {
            g_toc[g_toc_n].y = y - 2; g_toc[g_toc_n].h = rh;
            g_toc[g_toc_n].topic = ti; g_toc_n++;
        }
        y += rh;
        if (y > CONTENT_Y0 + CONTENT_H - rh) break;
    }
}

// Render one block; advances *py. Records link hit rectangles.
static void draw_runs_line(int x, int *py, int avail_w, const help_block_t *b,
                           int size, int bold_all) {
    // Simple word-wrap across runs at the given font size.
    int cx = x;
    int line_h = size + 6;
    for (int r = 0; r < b->run_count; r++) {
        const help_run_t *run = &b->runs[r];
        const char *txt = run->text ? run->text : "";
        uint32_t col = PAL.ink;
        int is_link = 0, ext = 0;
        if (run->kind == HELP_RUN_LINK_TOPIC) { col = PAL.accent; is_link = 1; }
        else if (run->kind == HELP_RUN_LINK_EXTERN) { col = PAL.accent; is_link = 1; ext = 1; }
        else if (run->kind == HELP_RUN_BOLD || bold_all) col = PAL.ink;
        else if (run->kind == HELP_RUN_ITALIC) col = PAL.ink_dim;

        // word wrap on spaces
        const char *p = txt;
        while (*p) {
            // grab a word (including trailing space)
            char word[128];
            int wl = 0;
            while (*p && *p != ' ' && wl < 126) word[wl++] = *p++;
            if (*p == ' ' && wl < 127) word[wl++] = *p++;
            word[wl] = 0;
            int ww = gui_ttf_width(word, size);
            if (cx + ww > x + avail_w && cx > x) {
                cx = x; *py += line_h;
            }
            int draw_y = *py;
            TXT(g_win, cx, draw_y, word, size, col);
            if (is_link && g_link_n < LINK_MAX && wl > 0) {
                // underline links
                gui_draw_hline(g_win, cx, draw_y + size, ww > 2 ? ww - 4 : ww, col);
                g_links[g_link_n].x = cx; g_links[g_link_n].y = draw_y;
                g_links[g_link_n].w = ww; g_links[g_link_n].h = line_h;
                g_links[g_link_n].external = ext;
                // store topic index resolved now (or -1 if external/unknown)
                g_links[g_link_n].topic = (run->kind == HELP_RUN_LINK_TOPIC)
                    ? help_find_topic_index(g_doc, run->target) : -1;
                g_link_n++;
            }
            cx += ww;
        }
    }
    *py += line_h;
}

static void draw_content(void) {
    gui_fill_rect(g_win, CONTENT_X, CONTENT_Y0, CONTENT_W, CONTENT_H, PAL.surface);
    g_link_n = 0;

    const help_topic_t *t = help_topic_at(g_doc, g_cur);
    if (!t) { TXT(g_win, CONTENT_X + 16, CONTENT_Y0 + 16, "No topic.", 14, PAL.ink); return; }

    int x = CONTENT_X + 18;
    int avail = CONTENT_W - 36;
    int py = CONTENT_Y0 + 14 - g_scroll;

    // Title.
    TXT(g_win, x, py, t->title, 20, PAL.accent);
    py += 32;
    gui_draw_hline(g_win, x, py, avail, PAL.border);
    py += 12;

    for (int i = 0; i < t->block_count; i++) {
        const help_block_t *b = &t->blocks[i];
        // skip blocks entirely above the viewport for speed (still cheap here)
        switch (b->kind) {
            case HELP_BLK_HEADING: {
                int sz = (b->heading_level == 1) ? 17 : 15;
                py += 6;
                draw_runs_line(x, &py, avail, b, sz, 1);
                py += 2;
                break;
            }
            case HELP_BLK_LIST_ITEM: {
                TXT(g_win, x, py, "\xE2\x80\xA2", 13, PAL.ink); // bullet (UTF-8)
                int save = py;
                draw_runs_line(x + 16, &py, avail - 16, b, 13, 0);
                if (py < save) py = save + 19;
                break;
            }
            case HELP_BLK_CODE: {
                const char *code = (b->run_count > 0 && b->runs[0].text)
                                   ? b->runs[0].text : "";
                // count lines for box height
                int lines = 1; for (const char *q = code; *q; q++) if (*q == '\n') lines++;
                int box_h = lines * 18 + 12;
                gui_fill_rect(g_win, x, py, avail, box_h, PAL.field_bg);
                gui_draw_rect_outline(g_win, x, py, avail, box_h, PAL.border);
                int ly = py + 6;
                char line[160]; int li = 0;
                for (const char *q = code; ; q++) {
                    if (*q == '\n' || *q == 0) {
                        line[li] = 0;
                        TXT(g_win, x + 8, ly, line, 12, PAL.ink);
                        ly += 18; li = 0;
                        if (*q == 0) break;
                    } else if (li < 158) line[li++] = *q;
                }
                py += box_h + 8;
                break;
            }
            case HELP_BLK_IMAGE: {
                char note[160];
                snprintf(note, sizeof(note), "[image: %s]",
                         (b->run_count > 0 && b->runs[0].target) ? b->runs[0].target : "?");
                TXT(g_win, x, py, note, 12, PAL.ink_dim);
                py += 20;
                break;
            }
            default: { // paragraph
                draw_runs_line(x, &py, avail, b, 14, 0);
                py += 6;
                break;
            }
        }
        if (py > CONTENT_Y0 + CONTENT_H + 400) break; // sanity bound
    }
}

static void draw_status(void) {
    int y = WIN_H - STATUS_H;
    gui_fill_rect(g_win, 0, y, WIN_W, STATUS_H, PAL.surface_raised);
    gui_draw_hline(g_win, 0, y, WIN_W, PAL.border);
    char line[200];
    snprintf(line, sizeof(line), "%s   |   %s   (%d topics)",
             g_status, g_doc && g_doc->title ? g_doc->title : "Help",
             help_topic_count(g_doc));
    TXT(g_win, 8, y + 4, line, 11, PAL.ink_dim);
}

static void draw_all(void) {
    gui_fill_rect(g_win, 0, 0, WIN_W, WIN_H, PAL.surface);
    draw_toc();
    draw_content();
    draw_toolbar();
    draw_status();
    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void do_search(void) {
    free(g_results); g_results = NULL; g_results_n = 0;
    if (g_search_len > 0) {
        g_results = help_search(g_doc, g_search, &g_results_n);
        char s[80];
        snprintf(s, sizeof(s), "Search: %d result(s) for '%s'", g_results_n, g_search);
        set_status(s);
        if (g_results_n > 0) go_topic(g_results[0], 1);
    } else {
        set_status("Ready.");
    }
}

static void handle_click(int mx, int my) {
    int tb = toolbar_hit(mx, my);
    if (tb == 1) { go_back(); g_search_focus = 0; return; }
    if (tb == 2) { go_forward(); g_search_focus = 0; return; }
    if (tb == 3) { g_search_focus = 0; go_topic(0, 1); return; }
    if (tb == 4) { g_search_focus = 1; return; }
    g_search_focus = 0;

    // TOC rows
    if (mx < TOC_W && my > CONTENT_Y0) {
        for (int i = 0; i < g_toc_n; i++) {
            if (my >= g_toc[i].y && my < g_toc[i].y + g_toc[i].h) {
                go_topic(g_toc[i].topic, 1);
                return;
            }
        }
        return;
    }

    // Content links
    if (mx >= CONTENT_X) {
        for (int i = 0; i < g_link_n; i++) {
            if (mx >= g_links[i].x && mx < g_links[i].x + g_links[i].w &&
                my >= g_links[i].y && my < g_links[i].y + g_links[i].h) {
                if (g_links[i].external) {
                    set_status("External link ignored (no browser hand-off).");
                } else if (g_links[i].topic >= 0) {
                    go_topic(g_links[i].topic, 1);
                } else {
                    set_status("Link target not found.");
                }
                return;
            }
        }
    }
}

static void handle_key(gui_event_t *e) {
    if (g_search_focus) {
        unsigned int kc = e->keycode;
        char ch = e->key_char;
        if (kc == 0x1C || ch == '\n' || ch == '\r') {   // Enter
            do_search();
            g_search_focus = 0;
            return;
        }
        if (kc == 0x0E || ch == '\b') {                  // Backspace
            if (g_search_len > 0) g_search[--g_search_len] = 0;
            return;
        }
        if (kc == 0x01) { g_search_focus = 0; return; }  // Esc
        if (ch >= 32 && ch < 127 && g_search_len < (int)sizeof(g_search) - 1) {
            g_search[g_search_len++] = ch;
            g_search[g_search_len] = 0;
        }
        return;
    }

    // Navigation when not typing.
    switch (e->keycode) {
        case 0x81: g_scroll += 40; break;            // Down arrow scroll
        case 0x80: g_scroll -= 40; if (g_scroll < 0) g_scroll = 0; break; // Up
        case 0x49: g_scroll -= 200; if (g_scroll < 0) g_scroll = 0; break; // PgUp
        case 0x51: g_scroll += 200; break;           // PgDn
        case 0x47: g_scroll = 0; break;              // Home
        default: break;
    }
}

// ---------------------------------------------------------------------------
static void open_doc(const char *path, const char *topic_id) {
    if (g_doc) { help_close(g_doc); g_doc = NULL; }
    int i = 0;
    while (path[i] && i < (int)sizeof(g_path) - 1) { g_path[i] = path[i]; i++; }
    g_path[i] = 0;

    g_doc = help_open(path);
    if (!g_doc) {
        // Build a tiny in-memory error document so the viewer still works.
        const char *msg =
            "@title Help\n@topic error Cannot open help\n"
            "# Cannot open help file\n\nThe requested help file could not be "
            "opened or parsed. Check that the file exists on the disk.\n";
        g_doc = help_parse_mhlp(msg, strlen(msg));
        set_status("Could not open help file.");
    }
    g_cur = 0;
    g_hist_len = 0; g_hist_pos = -1;
    if (topic_id && *topic_id) {
        int ti = help_find_topic_index(g_doc, topic_id);
        if (ti >= 0) g_cur = ti;
    }
    history_push(g_cur);
    const help_topic_t *t = help_topic_at(g_doc, g_cur);
    if (t) set_status(t->title);
}

int main(int argc, char **argv) {
    const char *path = "/HELP/SYSTEM.MHLP";
    const char *topic = NULL;
    if (argc >= 2 && argv[1] && argv[1][0]) path = argv[1];
    if (argc >= 3 && argv[2] && argv[2][0]) topic = argv[2];

    g_win = win_create("Help", 120, 80, WIN_W, WIN_H);
    if (g_win < 0) return 1;

    load_palette();
    open_doc(path, topic);
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int et = win_get_event(g_win, &ev, 100);
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_REDRAW:
                draw_all();
                break;
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
            case EVENT_MOUSE_DOWN:
                handle_click(ev.mouse_x, ev.mouse_y);
                draw_all();
                break;
            case EVENT_MOUSE_SCROLL:
                g_scroll -= ev.scroll_delta * 40;
                if (g_scroll < 0) g_scroll = 0;
                draw_all();
                break;
            case EVENT_KEY_DOWN:
                handle_key(&ev);
                draw_all();
                break;
            default:
                break;
        }
    }

    free(g_results);
    if (g_doc) help_close(g_doc);
    win_destroy(g_win);
    return 0;
}
