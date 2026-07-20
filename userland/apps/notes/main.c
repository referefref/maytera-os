// main.c - MayteraOS Notes app (#269)
// Multi-note notebook: left sidebar list, right editor pane. Create / rename /
// delete notes, edit a plain-text multi-line body with scrolling, autosave,
// search filter, word + char counts. Persists one file per note under /NOTES/.
// Styled to match Settings (theme-following palette + TTF text).

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#undef win_draw_text
#define win_draw_text(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

static int g_win_w = 720, g_win_h = 480;  // live content size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h
#define SIDEBAR_W 220
#define NOTES_DIR "/NOTES"
#define MAX_NOTES 128
#define MAX_BODY 16384
#define MAX_TITLE 48
#define LINE_H 18
#define EDIT_PAD 12

// ---------------------------------------------------------------------------
// Theme palette
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_SIDEBAR, COL_CARD, COL_SEP;
static uint32_t COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEL, COL_HOVER;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_SIDEBAR=0x00F0F0F0; COL_CARD=0x00F8F8F8; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00CCCCCC;
            COL_SEL=0x00D6E4FB; COL_HOVER=0x00E8E8E8; break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_SIDEBAR=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00808080;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_SEL=0x00000080; COL_HOVER=0x00D0D0D0; break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_SIDEBAR=0x001A3A4A; COL_CARD=0x001E4050; COL_SEP=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_TEXT_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_SEL=0x00305060; COL_HOVER=0x00254555; break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_SIDEBAR=0x002E3440; COL_CARD=0x00343B49; COL_SEP=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_TEXT_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_SEL=0x00434C5E; COL_HOVER=0x00343B49; break;
        default: // Dark
            COL_BG=0x00252525; COL_SIDEBAR=0x001E1E1E; COL_CARD=0x002A2A2A; COL_SEP=0x00404040;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_TEXT_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_SEL=0x0037527A; COL_HOVER=0x002D2D2D; break;
    }
    gui_set_style(kt == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface=COL_BG; p.surface_raised=COL_CARD; p.ink=COL_TEXT; p.ink_dim=COL_TEXT2;
    p.accent=COL_ACCENT; p.accent_hover=COL_ACCENT; p.border=COL_SEP;
    p.field_bg=COL_FIELD; p.field_border=COL_FIELD_BORDER; p.track=COL_SEP;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Note model
// ---------------------------------------------------------------------------
typedef struct {
    char file[64];          // on-disk filename, e.g. NOTE0001.TXT
    char title[MAX_TITLE];  // first line / display title
    char *body;             // heap-allocated body text
    int  body_len;
    int  body_cap;
    int  dirty;
} note_t;

static note_t g_notes[MAX_NOTES];
static int g_count = 0;
static int g_sel = -1;          // selected note index
static int g_window;

// editor state
static int g_cursor = 0;        // byte offset into body
static int g_scroll = 0;        // first visible line
static int g_focus = 1;         // 0 = sidebar/search, 1 = editor

// search
static char g_search[40] = "";
static int g_search_active = 0;

// rename modal
static int g_renaming = 0;
static char g_rename_buf[MAX_TITLE];

// status flash
static char g_status[80] = "";
static int g_status_flash = 0;
static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); g_status_flash = 20; }

// ---------------------------------------------------------------------------
// Body buffer helpers
// ---------------------------------------------------------------------------
static int body_ensure(note_t *n, int need) {
    if (n->body_cap >= need) return 1;
    int cap = n->body_cap ? n->body_cap : 256;
    while (cap < need) cap *= 2;
    if (cap > MAX_BODY) cap = MAX_BODY;
    if (cap < need) return 0;
    char *nb = (char *)realloc(n->body, cap);
    if (!nb) return 0;
    n->body = nb; n->body_cap = cap;
    return 1;
}

// First non-empty line becomes the title; fallback "Untitled".
static void derive_title(note_t *n) {
    int i = 0;
    while (i < n->body_len && (n->body[i] == '\n' || n->body[i] == '\r' || n->body[i] == ' ')) i++;
    int j = i;
    while (j < n->body_len && n->body[j] != '\n' && (j - i) < MAX_TITLE - 1) j++;
    int len = j - i;
    if (len <= 0) { strlcpy(n->title, "Untitled", sizeof(n->title)); return; }
    memcpy(n->title, n->body + i, len);
    n->title[len] = 0;
}

// ---------------------------------------------------------------------------
// Persistence: one file per note under /NOTES/
// ---------------------------------------------------------------------------
static void notes_dir_ensure(void) {
    // create dir; ignore error if it already exists
    sys_mkdir(NOTES_DIR, 0755);
}

static void note_path(const char *file, char *out, int cap) {
    snprintf(out, cap, "%s/%s", NOTES_DIR, file);
}

static void note_save(note_t *n) {
    if (!n->dirty) return;
    char path[128]; note_path(n->file, path, sizeof(path));
    sys_unlink(path);
    int fd = sys_open(path, O_WRONLY | O_CREAT);
    if (fd < 0) return;
    if (n->body && n->body_len > 0) sys_write(fd, n->body, (unsigned long)n->body_len);
    sys_close(fd);
    n->dirty = 0;
}

static void note_save_all(void) {
    for (int i = 0; i < g_count; i++) note_save(&g_notes[i]);
}

static void note_load_body(note_t *n) {
    if (n->body) return;            // already loaded
    char path[128]; note_path(n->file, path, sizeof(path));
    int fd = sys_open(path, O_RDONLY);
    n->body = NULL; n->body_len = 0; n->body_cap = 0; n->dirty = 0;
    if (fd < 0) { body_ensure(n, 256); if (n->body) n->body[0] = 0; return; }
    static char tmp[MAX_BODY];
    long total = 0;
    long r;
    while (total < MAX_BODY - 1 && (r = sys_read(fd, tmp + total, MAX_BODY - 1 - total)) > 0)
        total += r;
    sys_close(fd);
    if (total < 0) total = 0;
    body_ensure(n, (int)total + 1);
    if (n->body) { memcpy(n->body, tmp, total); n->body_len = (int)total; n->body[total] = 0; }
    derive_title(n);
}

// Scan /NOTES for NOTE*.TXT files.
static void notes_scan(void) {
    g_count = 0;
    dirent_t e;
    int idx = 0;
    while (g_count < MAX_NOTES && sys_readdir(NOTES_DIR, idx, &e) == 0) {
        idx++;
        if (DIRENT_IS_DIR(e)) continue;
        // accept any regular file (the dir is ours); store name + lazily title
        note_t *n = &g_notes[g_count];
        memset(n, 0, sizeof(*n));
        strlcpy(n->file, e.name, sizeof(n->file));
        strlcpy(n->title, e.name, sizeof(n->title));   // placeholder until loaded
        g_count++;
    }
    // Load titles for all (cheap: read first line). Keep bodies for selected.
    for (int i = 0; i < g_count; i++) {
        note_load_body(&g_notes[i]);
    }
}

static void make_note_filename(char *out, int cap) {
    // find an unused NOTExxxx.TXT
    for (int k = 1; k < 9999; k++) {
        char cand[64]; snprintf(cand, sizeof(cand), "NOTE%04d.TXT", k);
        int used = 0;
        for (int i = 0; i < g_count; i++)
            if (strcasecmp(g_notes[i].file, cand) == 0) { used = 1; break; }
        if (!used) { strlcpy(out, cand, cap); return; }
    }
    strlcpy(out, "NOTE9999.TXT", cap);
}

static void note_new(void) {
    if (g_count >= MAX_NOTES) { set_status("Note limit reached"); return; }
    note_t *n = &g_notes[g_count];
    memset(n, 0, sizeof(*n));
    make_note_filename(n->file, sizeof(n->file));
    body_ensure(n, 256);
    if (n->body) { n->body[0] = 0; n->body_len = 0; }
    strlcpy(n->title, "Untitled", sizeof(n->title));
    n->dirty = 1;
    g_sel = g_count;
    g_count++;
    g_cursor = 0; g_scroll = 0; g_focus = 1;
    note_save(n);
    set_status("New note");
}

static void note_delete(int idx) {
    if (idx < 0 || idx >= g_count) return;
    char path[128]; note_path(g_notes[idx].file, path, sizeof(path));
    sys_unlink(path);
    if (g_notes[idx].body) free(g_notes[idx].body);
    for (int i = idx; i < g_count - 1; i++) g_notes[i] = g_notes[i + 1];
    g_count--;
    if (g_sel >= g_count) g_sel = g_count - 1;
    g_cursor = 0; g_scroll = 0;
    set_status("Note deleted");
}

// ---------------------------------------------------------------------------
// Search filter: does a note match the query (title or body, case-insensitive)?
// ---------------------------------------------------------------------------
static int istrstr(const char *h, const char *needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = h; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return 1;
    return 0;
}
static int note_matches(note_t *n, const char *q) {
    if (!q[0]) return 1;
    if (istrstr(n->title, q)) return 1;
    if (n->body && istrstr(n->body, q)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Editor text operations
// ---------------------------------------------------------------------------
static void editor_insert(note_t *n, char c) {
    if (n->body_len >= MAX_BODY - 1) return;
    if (!body_ensure(n, n->body_len + 2)) return;
    for (int i = n->body_len; i > g_cursor; i--) n->body[i] = n->body[i - 1];
    n->body[g_cursor] = c;
    n->body_len++;
    n->body[n->body_len] = 0;
    g_cursor++;
    n->dirty = 1;
    derive_title(n);
}

static void editor_backspace(note_t *n) {
    if (g_cursor <= 0) return;
    for (int i = g_cursor - 1; i < n->body_len - 1; i++) n->body[i] = n->body[i + 1];
    n->body_len--;
    n->body[n->body_len] = 0;
    g_cursor--;
    n->dirty = 1;
    derive_title(n);
}

// word + char counts
static void count_words(note_t *n, int *words, int *chars) {
    int w = 0, inword = 0;
    for (int i = 0; i < n->body_len; i++) {
        char c = n->body[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') inword = 0;
        else { if (!inword) w++; inword = 1; }
    }
    *words = w; *chars = n->body_len;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// Sidebar list rows; account for search box at top.
#define SEARCH_Y 44
#define SEARCH_H 26
#define LIST_Y (SEARCH_Y + SEARCH_H + 8)
#define ROW_H 40

// map a visible list position to the actual note index (after filter)
static int g_visible[MAX_NOTES];
static int g_visible_n = 0;

static void rebuild_visible(void) {
    g_visible_n = 0;
    for (int i = 0; i < g_count; i++)
        if (note_matches(&g_notes[i], g_search)) g_visible[g_visible_n++] = i;
}

static void draw_sidebar(void) {
    win_draw_rect(g_window, 0, 0, SIDEBAR_W, WIN_H, COL_SIDEBAR);
    win_draw_rect(g_window, SIDEBAR_W - 1, 0, 1, WIN_H, COL_SEP);

    draw_text_sz(g_window, 12, 10, "Notes", 18, COL_TEXT);
    // new-note button
    gui_button(g_window, SIDEBAR_W - 40, 12, 28, 24, "+", GUI_BTN_PRIMARY, GUI_ST_NORMAL);

    // search field
    gui_fill_rounded_aa(g_window, 12, SEARCH_Y, SIDEBAR_W - 24, SEARCH_H, 4, COL_FIELD, COL_SIDEBAR);
    gui_rounded_border(g_window, 12, SEARCH_Y, SIDEBAR_W - 24, SEARCH_H, 4,
                       g_search_active ? COL_ACCENT : COL_FIELD_BORDER);
    if (g_search[0])
        draw_text_sz(g_window, 18, SEARCH_Y + 5, g_search, 12, COL_TEXT);
    else
        draw_text_sz(g_window, 18, SEARCH_Y + 5, "Search...", 12, COL_TEXT_DIM);
    if (g_search_active) {
        int cx = 18 + gui_ttf_width(g_search, 12);
        win_draw_rect(g_window, cx, SEARCH_Y + 4, 1, 18, COL_TEXT);
    }

    rebuild_visible();
    int y = LIST_Y;
    for (int v = 0; v < g_visible_n && y < WIN_H - ROW_H; v++) {
        int idx = g_visible[v];
        note_t *n = &g_notes[idx];
        if (idx == g_sel) {
            gui_fill_rounded_aa(g_window, 6, y, SIDEBAR_W - 12, ROW_H - 4, 5, COL_SEL, COL_SIDEBAR);
        }
        uint32_t tc = (idx == g_sel && COL_SEL == 0x00000080) ? 0x00FFFFFF : COL_TEXT;
        char tline[40];
        strlcpy(tline, n->title[0] ? n->title : "Untitled", sizeof(tline));
        draw_text_sz(g_window, 14, y + 4, tline, 13, tc);
        // preview (second line or body snippet)
        char prev[40] = "";
        if (n->body) {
            const char *p = n->body;
            // skip first line
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            int k = 0; while (*p && *p != '\n' && k < 36) { prev[k++] = *p++; }
            prev[k] = 0;
        }
        draw_text_sz(g_window, 14, y + 22, prev[0] ? prev : "(empty)", 10,
                     (idx == g_sel && COL_SEL == 0x00000080) ? 0x00DDDDDD : COL_TEXT_DIM);
        y += ROW_H;
    }
    if (g_visible_n == 0) {
        draw_text_sz(g_window, 14, LIST_Y + 6, g_search[0] ? "No matches" : "No notes yet", 12, COL_TEXT_DIM);
    }
}

static void draw_editor(void) {
    int ex = SIDEBAR_W;
    int ew = WIN_W - SIDEBAR_W;
    win_draw_rect(g_window, ex, 0, ew, WIN_H, COL_BG);

    if (g_sel < 0 || g_sel >= g_count) {
        draw_text_sz(g_window, ex + 40, 60, "Select a note, or click + to create one.", 14, COL_TEXT2);
        return;
    }
    note_t *n = &g_notes[g_sel];

    // header: title + rename + delete
    if (g_renaming) {
        gui_fill_rounded_aa(g_window, ex + 12, 10, ew - 200, 28, 4, COL_FIELD, COL_BG);
        gui_rounded_border(g_window, ex + 12, 10, ew - 200, 28, 4, COL_ACCENT);
        draw_text_sz(g_window, ex + 18, 16, g_rename_buf, 14, COL_TEXT);
        int cx = ex + 18 + gui_ttf_width(g_rename_buf, 14);
        win_draw_rect(g_window, cx, 14, 1, 20, COL_TEXT);
    } else {
        draw_text_sz(g_window, ex + 14, 12, n->title[0] ? n->title : "Untitled", 16, COL_TEXT);
    }
    gui_button(g_window, WIN_W - 178, 10, 80, 26, "Rename", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(g_window, WIN_W - 90,  10, 78, 26, "Delete", GUI_BTN_SECONDARY, GUI_ST_NORMAL);

    win_draw_rect(g_window, ex + 12, 44, ew - 24, 1, COL_SEP);

    // editor body region
    int by = 54;
    int bh = WIN_H - by - 28;
    int tx = ex + EDIT_PAD;
    int ty = by + 4;
    int max_lines = bh / LINE_H;

    // walk body, render lines (with cursor)
    int line = 0;
    int col = 0;
    int draw_line = 0;
    int cursor_drawn = 0;
    char linebuf[256]; int lb = 0;

    // Determine cursor line/col by scanning
    int cl = 0;
    for (int i = 0; i < g_cursor && i < n->body_len; i++) if (n->body[i] == '\n') cl++;
    // keep cursor visible
    if (cl < g_scroll) g_scroll = cl;
    if (cl >= g_scroll + max_lines) g_scroll = cl - max_lines + 1;
    if (g_scroll < 0) g_scroll = 0;

    for (int i = 0; i <= n->body_len; i++) {
        char c = (i < n->body_len) ? n->body[i] : '\n';
        int at_cursor = (i == g_cursor);
        if (c == '\n' || lb >= (int)sizeof(linebuf) - 1) {
            if (line >= g_scroll && draw_line < max_lines) {
                linebuf[lb] = 0;
                draw_text_sz(g_window, tx, ty + draw_line * LINE_H, linebuf, 13, COL_TEXT);
            }
            if (c == '\n') {
                if (i == g_cursor && line >= g_scroll && draw_line < max_lines && !cursor_drawn) {
                    int cxp = tx + gui_ttf_width(linebuf, 13);
                    win_draw_rect(g_window, cxp, ty + draw_line * LINE_H, 1, 15, COL_ACCENT);
                    cursor_drawn = 1;
                }
                if (line >= g_scroll) draw_line++;
                line++; lb = 0; col = 0;
                if (line >= g_scroll + max_lines) break;
                continue;
            }
        }
        if (at_cursor && line >= g_scroll && draw_line < max_lines && !cursor_drawn) {
            linebuf[lb] = 0;
            int cxp = tx + gui_ttf_width(linebuf, 13);
            win_draw_rect(g_window, cxp, ty + draw_line * LINE_H, 1, 15, COL_ACCENT);
            cursor_drawn = 1;
        }
        if (i < n->body_len) { linebuf[lb++] = c; col++; }
    }

    // footer: word + char count + autosave hint
    int words, chars; count_words(n, &words, &chars);
    char foot[80];
    snprintf(foot, sizeof(foot), "%d words   %d chars%s", words, chars, n->dirty ? "   *" : "");
    draw_text_sz(g_window, ex + 14, WIN_H - 22, foot, 11, COL_TEXT2);
}

static void draw_all(void) {
    win_draw_rect(g_window, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_sidebar();
    draw_editor();
    if (g_status_flash > 0 && g_status[0])
        draw_text_sz(g_window, WIN_W - gui_ttf_width(g_status, 11) - 14, WIN_H - 22, g_status, 11, COL_ACCENT);
    win_invalidate(g_window);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void select_note(int idx) {
    if (idx == g_sel) return;
    if (g_sel >= 0 && g_sel < g_count) note_save(&g_notes[g_sel]);
    g_sel = idx;
    g_cursor = 0; g_scroll = 0; g_focus = 1; g_renaming = 0;
    if (g_sel >= 0) { note_load_body(&g_notes[g_sel]); g_cursor = g_notes[g_sel].body_len; }
}

static void handle_click(int mx, int my) {
    // new-note button
    if (mx >= SIDEBAR_W - 40 && mx <= SIDEBAR_W - 12 && my >= 12 && my <= 36) { note_new(); return; }

    // search field
    if (mx >= 12 && mx <= SIDEBAR_W - 12 && my >= SEARCH_Y && my <= SEARCH_Y + SEARCH_H) {
        g_search_active = 1; g_focus = 0; return;
    }

    if (mx < SIDEBAR_W) {
        // sidebar list
        g_search_active = 0;
        if (my >= LIST_Y) {
            int v = (my - LIST_Y) / ROW_H;
            if (v >= 0 && v < g_visible_n) { select_note(g_visible[v]); g_focus = 1; }
        }
        return;
    }

    // editor pane
    g_search_active = 0;
    // rename / delete buttons
    if (my >= 10 && my <= 36) {
        if (mx >= WIN_W - 178 && mx <= WIN_W - 98 && g_sel >= 0) {
            g_renaming = 1; strlcpy(g_rename_buf, g_notes[g_sel].title, sizeof(g_rename_buf)); return;
        }
        if (mx >= WIN_W - 90 && mx <= WIN_W - 12 && g_sel >= 0) { note_delete(g_sel); return; }
    }
    g_focus = 1;
}

// Insert a new title line at the very front of the body (used by rename).
static void apply_rename(void) {
    if (g_sel < 0) { g_renaming = 0; return; }
    note_t *n = &g_notes[g_sel];
    // Replace the first line of the body with g_rename_buf.
    // Find end of first line.
    int eol = 0;
    while (eol < n->body_len && n->body[eol] != '\n') eol++;
    int newlen = (int)strlen(g_rename_buf);
    int tail = n->body_len - eol;            // includes the '\n' if present
    int total = newlen + tail;
    if (total >= MAX_BODY) { g_renaming = 0; return; }
    if (!body_ensure(n, total + 1)) { g_renaming = 0; return; }
    // move tail
    memmove(n->body + newlen, n->body + eol, tail);
    memcpy(n->body, g_rename_buf, newlen);
    n->body_len = total;
    n->body[n->body_len] = 0;
    g_cursor = newlen;
    n->dirty = 1;
    derive_title(n);
    note_save(n);
    g_renaming = 0;
    set_status("Renamed");
}

static void handle_key(gui_event_t *ev) {
    char c = ev->key_char;

    if (g_renaming) {
        if (c == 27) { g_renaming = 0; return; }
        if (c == '\r' || c == '\n') { apply_rename(); return; }
        if (c == '\b') { int l = (int)strlen(g_rename_buf); if (l > 0) g_rename_buf[l-1] = 0; return; }
        if (c >= 32 && c < 127) { int l = (int)strlen(g_rename_buf); if (l < MAX_TITLE - 1) { g_rename_buf[l] = c; g_rename_buf[l+1] = 0; } }
        return;
    }

    if (g_search_active) {
        if (c == 27) { g_search_active = 0; return; }
        if (c == '\r' || c == '\n') { g_search_active = 0; return; }
        if (c == '\b') { int l = (int)strlen(g_search); if (l > 0) g_search[l-1] = 0; return; }
        if (c >= 32 && c < 127) { int l = (int)strlen(g_search); if (l < (int)sizeof(g_search) - 1) { g_search[l] = c; g_search[l+1] = 0; } }
        return;
    }

    // editor focus
    if (g_sel < 0) return;
    note_t *n = &g_notes[g_sel];

    // arrow keys via keycode (best effort: many builds deliver as control chars)
    // Use key_char for printable + the common control chars.
    if (c == '\b') { editor_backspace(n); return; }
    if (c == '\r' || c == '\n') { editor_insert(n, '\n'); return; }
    if (c == '\t') { editor_insert(n, ' '); editor_insert(n, ' '); return; }
    if (c == 27) return;   // Esc ignored in editor
    if (c >= 32 && c < 127) { editor_insert(n, c); return; }

    // keycode-based navigation (left/right/up/down/home/end if provided)
    switch (ev->keycode) {
        case 0x4B: if (g_cursor > 0) g_cursor--; break;             // Left
        case 0x4D: if (g_cursor < n->body_len) g_cursor++; break;   // Right
        case 0x48: {                                                // Up
            int col = 0, i = g_cursor;
            while (i > 0 && n->body[i-1] != '\n') { i--; col++; }
            if (i > 0) { int j = i - 1; while (j > 0 && n->body[j-1] != '\n') j--; int k = j; int cc = 0; while (k < i - 1 && cc < col) { k++; cc++; } g_cursor = k; }
            break;
        }
        case 0x50: {                                                // Down
            int col = 0, i = g_cursor;
            while (i > 0 && n->body[i-1] != '\n') { i--; col++; }
            int e = g_cursor; while (e < n->body_len && n->body[e] != '\n') e++;
            if (e < n->body_len) { int k = e + 1; int cc = 0; while (k < n->body_len && n->body[k] != '\n' && cc < col) { k++; cc++; } g_cursor = k; }
            break;
        }
        default: break;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    apply_theme(get_theme());
    g_window = win_create("Notes", 140, 100, WIN_W, WIN_H);
    if (g_window < 0) return 1;

    notes_dir_ensure();
    notes_scan();
    if (g_count > 0) { g_sel = 0; note_load_body(&g_notes[0]); g_cursor = g_notes[0].body_len; }

    draw_all();

    gui_event_t ev;
    int running = 1;
    int autosave_tick = 0;
    while (running) {
        int et = win_get_event(g_window, &ev, 300);
        if (et == 0) {
            if (++autosave_tick >= 3) {     // ~ every 0.9s, persist dirty notes
                autosave_tick = 0;
                note_save_all();
            }
            if (g_status_flash > 0) { g_status_flash--; draw_all(); }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; }
                draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) { handle_click(ev.mouse_x, ev.mouse_y); draw_all(); }
                break;
            case EVENT_KEY_DOWN:
                handle_key(&ev);
                draw_all();
                break;
            default: break;
        }
    }
    note_save_all();
    win_destroy(g_window);
    return 0;
}
