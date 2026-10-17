// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_filedlg.c - the shared file chooser. See gui_filedlg.h for where it came
// from and why it is here rather than inside officelib.
//
// Built on the SHARED widgets (gui_list.h, gui_style.h via gui.h, theme.h) per
// docs/UI_STYLE_GUIDE.md, and on the SHARED named-locations model (places.h),
// so the Places sidebar here and the one in the Files app cannot disagree about
// what a place is called or where it resolves to.
#include "gui.h"          // types/syscall/keys/gui_mods/gui_style/textfield
#include "gui_list.h"
#include "theme.h"
#include "gui_filedlg.h"
#include "contract.h"
#include "places.h"
#include "dirent.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define FD_MAX_ENTRIES 512
#define FD_ROW_H       22
#define FD_PATH_MAX    400

// Sidebar geometry. FD_SIDE_W is a MEASURED fit, not a taste: the label starts
// 34px in (icon 12px + 16px icon + 6px gap) and the longest label the model can
// produce is "Quick access" as the section heading at 11px, so 136px leaves the
// heading room without clipping. Below FD_SIDE_MIN_CW the sidebar is dropped
// entirely rather than squeezed: on a small host window the FILE LIST is the
// thing the user came for, and a 90px list with a sidebar beside it is worse
// than no sidebar. Keyboard navigation is unaffected either way (the list keeps
// its ".." row), which is what makes dropping it safe.
#define FD_SIDE_W      136
#define FD_SIDE_GAP    12
#define FD_SIDE_MIN_CW 480
#define FD_SIDE_ROW_H  22
#define FD_SIDE_ICON_X 12
#define FD_SIDE_LBL_X  34

typedef struct { char name[256]; int is_dir; } fd_entry_t;

static fd_entry_t   g_entries[FD_MAX_ENTRIES];
static int          g_count;
static char         g_dir[FD_PATH_MAX];
static char         g_exts[128];
static gui_list_t   g_list;
static int          g_sel;
static char         g_result[512];
static char         g_fname[192];
static textfield_t  g_tf;
static int          g_btn_cancel_x, g_btn_ok_x, g_btn_y, g_btn_w, g_btn_h;

// Which of the three panes takes a key. Replaces the old single `tf_focus`
// flag, which could only express "the filename field or not" and therefore left
// the Places sidebar reachable by mouse only, in a dialog whose header comment
// promises every path works without one (#334).
enum { FD_FOCUS_LIST = 0, FD_FOCUS_PLACES = 1, FD_FOCUS_NAME = 2 };
static int g_focus;

// The nested loop's resolution, HOISTED out of the two loop bodies so that a
// contract action can end the dialog exactly as the Open button does. They were
// locals called `done` and `ok`, which meant the only way to resolve the modal
// was a keystroke or a click: a caller with neither had no way to finish it, and
// "the dialog cannot be closed except by a human" is a hang with a nicer name.
static int g_done, g_ok;
// 0 = no dialog up, 1 = Open, 2 = Save. Read by the contract as chooser.mode, and
// used to refuse re-entering the chooser from inside its own nested loop.
static int g_mode;

// Places hit table, rebuilt every draw like the Files sidebar's: the rows are a
// function of the session and the window size, so caching them would be a third
// copy of the same state.
#define FD_SIDE_MAX 16
static int g_side_place[FD_SIDE_MAX];   // index into places_at()
static int g_side_y[FD_SIDE_MAX];
static int g_side_n;
static int g_side_x, g_side_w, g_side_shown;
static int g_place_sel;                 // keyboard highlight within the sidebar

// --- path helpers -----------------------------------------------------------
static void fd_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

static void fd_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}

static void fd_parent(char *dir) {
    int n = (int)strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    while (n > 1 && dir[n - 1] != '/') n--;
    if (n > 1) dir[n - 1] = 0; else { dir[0] = '/'; dir[1] = 0; }
}

// CASE-INSENSITIVE, and this is not laziness. The ext2 root folds case, so the
// path a places accessor builds (uppercase, because that is what
// users_make_home_skeleton() creates) and the path produced by walking there
// through readdir (whatever case the directory entry actually carries) name the
// SAME directory and must highlight the same row. Comparing exactly would leave
// the sidebar showing nothing selected while the user is standing in it.
//
// This MATCHES, it does not NORMALISE: nothing here rewrites a path, so no
// case-mixed name is ever created by this file. The create side is
// users_make_home_skeleton(), which writes one case.
static int fd_path_eq(const char *a, const char *b) {
    int i = 0;
    for (;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// --- directory listing ------------------------------------------------------
// exts: concatenated dot-prefixed tokens, e.g. ".docx.odt" -> {".docx",".odt"}.
// Empty/NULL means "match everything" (used by the Save dialog's browser).
static int fd_ext_match(const char *name, const char *exts) {
    if (!exts || !*exts) return 1;
    int namelen = (int)strlen(name);
    const char *p = exts;
    while (*p) {
        const char *q = p + 1;
        while (*q && *q != '.') q++;
        int toklen = (int)(q - p);
        if (namelen >= toklen) {
            int match = 1;
            for (int i = 0; i < toklen; i++) {
                char x = name[namelen - toklen + i], y = p[i];
                if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                if (x != y) { match = 0; break; }
            }
            if (match) return 1;
        }
        p = q;
    }
    return 0;
}

static int fd_cmp(const void *pa, const void *pb) {
    const fd_entry_t *a = (const fd_entry_t *)pa, *b = (const fd_entry_t *)pb;
    int ap = !strcmp(a->name, ".."), bp = !strcmp(b->name, "..");
    if (ap != bp) return ap ? -1 : 1;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void fd_scan(const char *dir, const char *exts) {
    g_count = 0;
    if (strcmp(dir, "/") != 0 && g_count < FD_MAX_ENTRIES) {
        fd_entry_t *e = &g_entries[g_count++];
        fd_copy(e->name, sizeof(e->name), "..");
        e->is_dir = 1;
    }
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != 0 && g_count < FD_MAX_ENTRIES) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            int isdir = (de->d_type == DT_DIR);
            if (!isdir && !fd_ext_match(de->d_name, exts)) continue;
            fd_entry_t *e = &g_entries[g_count++];
            fd_copy(e->name, sizeof(e->name), de->d_name);
            e->is_dir = isdir;
        }
        closedir(d);
    }
    if (g_count > 0) qsort(g_entries, g_count, sizeof(fd_entry_t), fd_cmp);
}

static const char *fd_label(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    if (index < 0 || index >= g_count) return "";
    fd_entry_t *e = &g_entries[index];
    if (e->is_dir) snprintf(buf, cap, "%s%s", e->name, !strcmp(e->name, "..") ? "" : "/");
    else           snprintf(buf, cap, "%s", e->name);
    return buf;
}

static void fd_goto(const char *path) {
    fd_copy(g_dir, sizeof(g_dir), path);
    fd_scan(g_dir, g_exts);
    g_sel = (g_count > 0) ? 0 : -1;
    g_list.scroll.offset = 0;
}

static void fd_enter_dir(fd_entry_t *e) {
    char nd[FD_PATH_MAX];
    if (!strcmp(e->name, "..")) { fd_copy(nd, sizeof(nd), g_dir); fd_parent(nd); }
    else fd_join(nd, sizeof(nd), g_dir, e->name);
    fd_goto(nd);
}

// Open-mode activation: a directory navigates; a file confirms (*confirm=1).
static void fd_activate_open(int *confirm) {
    if (confirm) *confirm = 0;
    if (g_sel < 0 || g_sel >= g_count) return;
    fd_entry_t *e = &g_entries[g_sel];
    if (e->is_dir) { fd_enter_dir(e); return; }
    fd_join(g_result, sizeof(g_result), g_dir, e->name);
    if (confirm) *confirm = 1;
}

// Save-mode list activation: a directory navigates; a file copies its name into
// the filename field (does not confirm - the field is authoritative).
static void fd_activate_save_list(void) {
    if (g_sel < 0 || g_sel >= g_count) return;
    fd_entry_t *e = &g_entries[g_sel];
    if (e->is_dir) { fd_enter_dir(e); return; }
    tf_set_text(&g_tf, e->name);
    g_focus = FD_FOCUS_NAME;
}

// --- layout ----------------------------------------------------------------
static void fd_geom(const filedlg_host_t *h, int *cx, int *cy, int *cw, int *ch) {
    int w = 640, hh = 480;
    if (h->size) h->size(h->ctx, &w, &hh);
    if (w < 80) w = 80;
    if (hh < 80) hh = 80;
    // Wider than the pre-sidebar 560 cap, because a sidebar that ate 148px out
    // of the same card would have shrunk the file list by a quarter. The height
    // cap is unchanged.
    int dw = w - 64; if (dw > 680) dw = 680; if (dw < 300) dw = 300;
    int dh = hh - 64; if (dh > 460) dh = 460; if (dh < 240) dh = 240;
    *cw = dw; *ch = dh; *cx = (w - dw) / 2; *cy = (hh - dh) / 2;
}

// --- drawing ---------------------------------------------------------------
// THE ICON, AND WHY IT IS DRAWN RATHER THAN LOADED.
//
// place_t carries an icon NAME ("HOME", "FOLDER") because that is what the
// icon-font assets under /ICONS are keyed by, and the Files sidebar resolves
// exactly those names through its own `draw_mico()`. The loader for them is NOT
// in libc: it exists as `draw_mico()` in apps/files/main.c, again in
// apps/settings/main.c, again in apps/setup/main.rs, and a fourth time as
// officelib/officeui/oicon.c, whose own header says "Its intended home is libc
// as gui_icon.h ... It lives here because this change does not touch libc".
//
// Moving it is the right fix and it is NOT this change: oicon.c pulls in
// officelib's private oui.h, so the move is an officelib refactor with six
// dependent files, and doing it inside a change about user folder paths would
// make both harder to review and to bisect. So the sidebar draws the same two
// shapes those loaders fall back to when an asset is missing, the model keeps
// the icon NAME, and the day gui_icon.h lands in libc this function becomes one
// call to it with no change above. Recorded as a named follow-up rather than
// left as a silent TODO.
static void fd_place_icon(int win, int x, int y, const char *name, uint32_t ink) {
    int home = name && name[0] == 'H';     // "HOME"; everything else is a folder
    if (home) {
        // A roof over a body: five rows widening, then a 8x6 block beneath.
        for (int r = 0; r < 5; r++) win_draw_rect(win, x + 6 - r, y + 2 + r, 1 + 2 * r, 1, ink);
        win_draw_rect(win, x + 3, y + 7, 9, 6, ink);
    } else {
        // A tab over a body, the folder shape every sidebar in the OS falls
        // back to.
        win_draw_rect(win, x + 1, y + 3, 5, 2, ink);
        win_draw_rect(win, x, y + 5, 14, 8, ink);
    }
}

static void fd_draw_places(int win, int y0, int band_h, uint32_t surface,
                           uint32_t ink, uint32_t dim, uint32_t border) {
    g_side_n = 0;
    if (!g_side_shown) return;

    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);
    uint32_t sel_bg = theme_color(THEME_COLOR_SELECTION);   // [filesui] zero test removed: 0 is BLACK, never unset (libc/theme.h).

    // The sidebar sits on its own recessed panel, the same grammar as the file
    // list's field background beside it, so the two read as sibling panes
    // rather than one pane and some loose text.
    uint32_t panel = theme_color(THEME_COLOR_TEXTBOX_BG);
    if (!panel) panel = surface;

    // [filesui] Floored against THE PANEL, which is what these glyphs land on,
    // and NOT against the card the caller measured its own inks against. See
    // the long note in fd_draw(): a selected row is ink on sel_bg and an
    // unselected row is ink on panel, and on retro_unix those are a dark teal
    // and pure white, so one ink cannot serve both. Deriving each from the
    // ground it sits on is the only version of this that is right on every
    // theme rather than on the one that was open at the time.
    ink = gui_ensure_contrast(ink, panel, GUI_FLOOR_TEXT);
    dim = gui_ensure_contrast(dim, panel, GUI_FLOOR_TEXT);
    uint32_t sel_ink = gui_ensure_contrast(theme_color(THEME_COLOR_SELECTION_TEXT), sel_bg, GUI_FLOOR_TEXT);
    gui_fill_rounded(win, g_side_x, y0, g_side_w, band_h, radius, panel);
    gui_draw_rect_outline(win, g_side_x, y0, g_side_w, band_h,
                          g_focus == FD_FOCUS_PLACES ? sel_bg : border);

    int y = y0 + 8;
    int last_sect = -1;
    for (int i = 0; i < places_count() && g_side_n < FD_SIDE_MAX; i++) {
        const place_t *pl = places_at(i);
        char p[FD_PATH_MAX];
        if (!pl || places_path(i, p, sizeof(p)) != 0) continue;

        // A heading only when the section CHANGES, so an empty section leaves
        // no orphan header. Same rule as the Files sidebar.
        if ((int)pl->sect != last_sect) {
            if (last_sect >= 0) y += 6;
            if (y + 16 > y0 + band_h) break;
            char up[32]; int k = 0;
            const char *s = places_section_label(pl->sect);
            for (; s[k] && k < 30; k++) up[k] = (s[k] >= 'a' && s[k] <= 'z') ? (char)(s[k] - 32) : s[k];
            up[k] = 0;
            win_draw_text_ttf(win, g_side_x + FD_SIDE_ICON_X, y, up, 11, dim);
            y += 18;
            last_sect = (int)pl->sect;
        }
        if (y + FD_SIDE_ROW_H > y0 + band_h) break;

        int cur = fd_path_eq(g_dir, p);
        int kbd = (g_focus == FD_FOCUS_PLACES && g_side_n == g_place_sel);
        if (cur || kbd)
            gui_fill_rounded(win, g_side_x + 4, y - 2, g_side_w - 8, FD_SIDE_ROW_H,
                             theme_metric(THEME_METRIC_RADIUS_BTN), sel_bg);
        uint32_t row_ink = (cur || kbd) ? sel_ink : ink;
        fd_place_icon(win, g_side_x + FD_SIDE_ICON_X, y, pl->icon, row_ink);
        win_draw_text_ttf(win, g_side_x + FD_SIDE_LBL_X, y + 2, pl->label, 12, row_ink);

        g_side_place[g_side_n] = i;
        g_side_y[g_side_n] = y;
        g_side_n++;
        y += FD_SIDE_ROW_H;
    }
    if (g_place_sel >= g_side_n) g_place_sel = g_side_n > 0 ? g_side_n - 1 : 0;
}

static void fd_draw(const filedlg_host_t *host, const char *title, int save_mode) {
    int win = host->win, w = 640, h = 480;
    if (host->size) host->size(host->ctx, &w, &h);

    // Interlaced-scanline scrim (gui_confirm.c's technique - no framebuffer
    // read-back exists, so this is how a modal dims the page beneath it).
    for (int yy = 0; yy < h; yy += 2) win_draw_rect(win, 0, yy, w, 1, 0x00000000);

    int cx, cy, cw, ch; fd_geom(host, &cx, &cy, &cw, &ch);
    uint32_t surface = theme_color(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t border  = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t fieldbg = theme_color(THEME_COLOR_TEXTBOX_BG);
    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);
    // [filesui] No zero test on `surface` either: it is a populated field like
    // every other. `fieldbg` still falls back to `surface`, which is NOT a zero
    // test on a colour but a layout decision: a theme that gives the text box
    // the same ground as the card should have its panels drawn flush, and that
    // is expressible only as "use the other one".
    if (!fieldbg) fieldbg = surface;

    // ---- INK IS DERIVED FROM CONTRAST, NEVER FROM A ZERO TEST --------------
    //
    // [filesui] THIS IS A FIXED BUG AND IT IS WORTH THE PARAGRAPH, because the
    // idiom that caused it is everywhere in this tree. The line here used to be
    //
    //     uint32_t ink = theme_color(THEME_COLOR_ON_SURFACE);
    //     if (!ink) ink = 0x00FFFFFF;
    //
    // which reads as "if the theme did not set this colour, use white". It is
    // not what it does. theme_color() returns a raw 0x00RRGGBB and has NO
    // out-of-band value for "unset", so **0 means BLACK and it also means
    // unset, and nothing can tell them apart.** retro_unix, the DEFAULT theme
    // on every shipping image, sets color.on_surface=0x00000000. So this branch
    // fired on the default theme and turned the theme's BLACK text into WHITE,
    // on a 0x00D4D4D4 card: 1.4:1, well under the 4.5:1 text floor. The dialog's
    // own "Open Document" title had been drawn that way since the dialog was
    // written; it was only obvious once a Places sidebar put ten more labels on
    // a WHITE panel, where white-on-white is not low contrast, it is INVISIBLE.
    //
    // The fix is not a better fallback, because there is no question a fallback
    // could answer. It is to stop asking. What a glyph needs is not "was this
    // configured" but "can it be read on the ground it lands on", and
    // gui_ensure_contrast() answers exactly that and returns the colour
    // UNCHANGED when it already clears the floor. Black on the light card stays
    // black; an unset-and-therefore-black ink on a dark card gets walked to
    // something legible. Both cases are correct without distinguishing them.
    //
    // AND IT MUST BE FLOORED AGAINST THE GROUND IT ACTUALLY SITS ON. The card,
    // the file list and the Places panel are three different backgrounds. An
    // ink floored against the card and then drawn on the white list is floored
    // against the wrong thing, which is the same class of bug one layer up.
    uint32_t ink      = gui_ensure_contrast(theme_color(THEME_COLOR_ON_SURFACE), surface, GUI_FLOOR_TEXT);
    uint32_t list_ink = gui_ensure_contrast(theme_color(THEME_COLOR_ON_SURFACE), fieldbg, GUI_FLOOR_TEXT);
    // Muted ink on the overlay card: THEME_COLOR_MUTED raw is 2.36:1 on
    // retro_unix's raised surface, under the text floor.
    uint32_t dim      = gui_ensure_contrast(theme_color(THEME_COLOR_MUTED), surface, GUI_FLOOR_TEXT);

    gui_fill_rounded(win, cx, cy, cw, ch, radius, surface);
    gui_draw_rect_outline(win, cx, cy, cw, ch, border);
    win_draw_text_ttf(win, cx + 20, cy + 16, title, 16, ink);
    win_draw_rect(win, cx + 20, cy + 44, cw - 40, 1, border);
    win_draw_text_ttf(win, cx + 20, cy + 52, g_dir, 12, dim);

    int list_y = cy + 76;
    int list_h = ch - 76 - 56 - (save_mode ? 36 : 0);
    if (list_h < 40) list_h = 40;

    g_side_shown = (cw >= FD_SIDE_MIN_CW);
    g_side_x = cx + 20;
    g_side_w = FD_SIDE_W;
    int list_x = cx + 20, list_w = cw - 40;
    if (g_side_shown) {
        list_x = g_side_x + g_side_w + FD_SIDE_GAP;
        list_w = cw - 20 - FD_SIDE_GAP - g_side_w - 20;
    }
    fd_draw_places(win, list_y, list_h, surface, ink, dim, border);

    gui_list_config(&g_list, list_x, list_y, list_w, list_h, FD_ROW_H, g_count);
    {
        uint32_t sb = theme_color(THEME_COLOR_SELECTION);   // see libc/theme.h
        gui_list_draw(win, &g_list, g_sel, fieldbg, border, list_ink, sb,
                      gui_ensure_contrast(theme_color(THEME_COLOR_SELECTION_TEXT), sb, GUI_FLOOR_TEXT),
                      fd_label, 0);
    }
    // The focused pane is the one that takes a key, so it has to be visible.
    if (g_focus == FD_FOCUS_LIST) {
        uint32_t f = theme_color(THEME_COLOR_SELECTION);
        gui_draw_rect_outline(win, list_x, list_y, list_w, list_h, f ? f : ink);
    }

    int by = cy + ch - 40;
    if (save_mode) {
        int fy = list_y + list_h + 14;
        win_draw_text_ttf(win, cx + 20, fy, "File name:", 12, dim);
        int fx = cx + 110, fw = cw - 40 - 110;
        gui_textfield_tf(win, fx, fy - 5, fw, 24, g_tf.buf, g_tf.len, g_tf.cursor,
                         g_tf.sel_anchor, g_focus == FD_FOCUS_NAME, "filename.docx");
        by = fy + 30;
    }

    int bw = 90, bh = 28, gap = 12;
    g_btn_ok_x = cx + cw - 20 - bw;
    g_btn_cancel_x = g_btn_ok_x - bw - gap;
    g_btn_y = by; g_btn_w = bw; g_btn_h = bh;
    gui_button(win, g_btn_cancel_x, by, bw, bh, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(win, g_btn_ok_x, by, bw, bh, save_mode ? "Save" : "Open", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    win_invalidate(win);
}

static int fd_hit_button(int mx, int my, int bx) {
    return mx >= bx && mx < bx + g_btn_w && my >= g_btn_y && my < g_btn_y + g_btn_h;
}

// Returns the places-row index under the pointer, or -1.
static int fd_hit_place(int mx, int my) {
    if (!g_side_shown) return -1;
    if (mx < g_side_x || mx >= g_side_x + g_side_w) return -1;
    for (int i = 0; i < g_side_n; i++)
        if (my >= g_side_y[i] - 2 && my < g_side_y[i] - 2 + FD_SIDE_ROW_H) return i;
    return -1;
}

static void fd_goto_place(int row) {
    if (row < 0 || row >= g_side_n) return;
    char p[FD_PATH_MAX];
    if (places_path(g_side_place[row], p, sizeof(p)) != 0) return;
    g_place_sel = row;
    fd_goto(p);
}

// Tab order. The filename field only exists in Save mode, and the sidebar only
// when the card is wide enough for it, so the cycle is computed rather than
// hardcoded: a Tab that lands on a pane that is not on screen is a dead key.
static void fd_focus_next(int save_mode) {
    for (int i = 0; i < 3; i++) {
        g_focus = (g_focus + 1) % 3;
        if (g_focus == FD_FOCUS_PLACES && !g_side_shown) continue;
        if (g_focus == FD_FOCUS_NAME && !save_mode) continue;
        return;
    }
    g_focus = FD_FOCUS_LIST;
}

// ===========================================================================
// THE VERB TABLE, AND WHY ENTER AND THE OK BUTTON NOW GO THROUGH IT
//
// Before this, "what Enter does" was written out once inside filedlg_open()'s
// key handler and again, differently shaped, inside filedlg_save()'s, and "what
// the OK button does" was written out twice more inside the two mouse handlers.
// Four copies of two behaviours, kept in agreement by hand: the Save copy of
// Enter had to remember that the filename field is authoritative, the Open copy
// had to remember that a directory navigates rather than confirms, and each
// mouse copy had to remember its own half again.
//
// Now there is ONE definition of each verb and three callers of it: the key
// handler, the button handler and the contract. That is the same move as
// side_activate() in the Files sidebar, and it has the same consequence:
// exercising a verb through the contract is evidence about the keystroke and the
// click, because there is only one route.
static void fdv_enter(void) {
    if (g_focus == FD_FOCUS_PLACES) {
        fd_goto_place(g_place_sel);
        g_focus = (g_mode == 2) ? FD_FOCUS_NAME : FD_FOCUS_LIST;
        return;
    }
    if (g_mode == 2) {
        // Save: the list fills the name field, the field confirms. The field is
        // authoritative because the user may be naming a file that does not
        // exist yet, which is the whole difference from Open.
        if (g_focus == FD_FOCUS_LIST) { fd_activate_save_list(); return; }
        if (g_tf.len > 0) { fd_join(g_result, sizeof(g_result), g_dir, g_tf.buf); g_done = 1; g_ok = 1; }
        return;
    }
    int confirm = 0;
    fd_activate_open(&confirm);
    if (confirm) { g_done = 1; g_ok = 1; }
}
static void fdv_accept(void) {
    if (g_mode == 2) {
        if (g_tf.len > 0) { fd_join(g_result, sizeof(g_result), g_dir, g_tf.buf); g_done = 1; g_ok = 1; }
        return;
    }
    int confirm = 0;
    fd_activate_open(&confirm);
    if (confirm) { g_done = 1; g_ok = 1; }
}
static void fdv_cancel(void)      { g_done = 1; g_ok = 0; }
static void fdv_up(void)          { char nd[FD_PATH_MAX]; fd_copy(nd, sizeof(nd), g_dir); fd_parent(nd); fd_goto(nd); }
static void fdv_focus_next(void)  { fd_focus_next(g_mode == 2); }
static void fdv_sel_next(void)    { gui_list_move_sel(&g_list, &g_sel, 1); }
static void fdv_sel_prev(void)    { gui_list_move_sel(&g_list, &g_sel, -1); }
static void fdv_page_next(void)   { gui_list_move_sel(&g_list, &g_sel, 8); }
static void fdv_page_prev(void)   { gui_list_move_sel(&g_list, &g_sel, -8); }
static void fdv_sel_first(void)   { gui_list_move_sel(&g_list, &g_sel, -g_count); }
static void fdv_sel_last(void)    { gui_list_move_sel(&g_list, &g_sel, g_count); }
static void fdv_place_next(void)  { if (g_place_sel < g_side_n - 1) g_place_sel++; g_focus = FD_FOCUS_PLACES; }
static void fdv_place_prev(void)  { if (g_place_sel > 0) g_place_sel--; g_focus = FD_FOCUS_PLACES; }
static void fdv_place_go(void)    { fd_goto_place(g_place_sel); }

typedef struct {
    const char *label;
    const char *tok;
    void      (*fn)(void);
    int       (*afn)(int argc, char **argv, char *out, int ocap);
    const char *desc;
    const ct_terms_t *terms;
} fdv_t;

// ---- terms ----------------------------------------------------------------
// Every verb below is INSTANT and LIVE_ONLY: it manipulates a dialog that only
// exists inside a running instance, so a spawned process can describe it and
// cannot achieve it. That is the honest statement, and CT_TF_LIVE_ONLY is how a
// caller learns it before wasting a spawn on it.
static const ct_terms_t k_fdv_nav = {
    CT_TIME_FAST, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE | CT_FAIL_NOTFOUND | CT_FAIL_IO, "chooser.dir", 0
};
static const ct_terms_t k_fdv_sel = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE | CT_FAIL_RANGE, "chooser.selected", 0
};
static const ct_terms_t k_fdv_place = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE | CT_FAIL_RANGE, "chooser.place_row", 0
};
static const ct_terms_t k_fdv_focus = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE, "chooser.focus", 0
};
static const ct_terms_t k_fdv_name = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE, "chooser.filename", 0
};
// RESOLVING the dialog is what the caller most needs and is the one verb whose
// success is NOT the dialog's own state: it is the dialog going away with a
// result, which chooser.open and chooser.result report together.
static const ct_terms_t k_fdv_resolve = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NEEDSLIVE | CT_FAIL_NOTFOUND, "chooser.open", 0
};

static const fdv_t FD_VERBS[] = {
    { "Open / OK",        "chooser.accept",       fdv_accept,     0,
      "Resolve the dialog as its primary button does: confirm the selected file, or the typed name in Save mode", &k_fdv_resolve },
    { "Cancel",           "chooser.cancel",       fdv_cancel,     0,
      "Dismiss the dialog with no result, as Cancel and Esc do", &k_fdv_resolve },
    { "Activate",         "chooser.enter",        fdv_enter,      0,
      "Act on whichever pane has focus, exactly as Enter does: navigate, fill the name field, or confirm", &k_fdv_resolve },
    { "Parent folder",    "chooser.up",           fdv_up,         0,
      "Navigate to the parent of the folder being shown, as the '..' row does", &k_fdv_nav },
    { "Next pane",        "chooser.focus_next",   fdv_focus_next, 0,
      "Move focus to the next pane that is actually on screen, as Tab does", &k_fdv_focus },
    { "Select next",      "chooser.select_next",  fdv_sel_next,   0,
      "Move the file-list selection down one row (Down)", &k_fdv_sel },
    { "Select previous",  "chooser.select_prev",  fdv_sel_prev,   0,
      "Move the file-list selection up one row (Up)", &k_fdv_sel },
    { "Page down",        "chooser.page_next",    fdv_page_next,  0,
      "Move the file-list selection down eight rows (PgDn)", &k_fdv_sel },
    { "Page up",          "chooser.page_prev",    fdv_page_prev,  0,
      "Move the file-list selection up eight rows (PgUp)", &k_fdv_sel },
    { "First entry",      "chooser.select_first", fdv_sel_first,  0,
      "Select the first row of the file list (Home)", &k_fdv_sel },
    { "Last entry",       "chooser.select_last",  fdv_sel_last,   0,
      "Select the last row of the file list (End)", &k_fdv_sel },
    { "Next place",       "chooser.place_next",   fdv_place_next, 0,
      "Move the Places sidebar highlight down one row and focus the sidebar", &k_fdv_place },
    { "Previous place",   "chooser.place_prev",   fdv_place_prev, 0,
      "Move the Places sidebar highlight up one row and focus the sidebar", &k_fdv_place },
    { "Go to place",      "chooser.place_go",     fdv_place_go,   0,
      "Navigate to the highlighted Places row, as Enter on the sidebar does", &k_fdv_nav },
};
#define FDV_N ((int)(sizeof(FD_VERBS) / sizeof(FD_VERBS[0])))

// ---- the arg-taking verbs -------------------------------------------------
static int fda_goto(int argc, char **argv, char *out, int ocap) {
    if (!g_mode) { snprintf(out, ocap, "no-dialog-open"); return -1; }
    if (argc < 1 || !argv[0] || argv[0][0] != '/') { snprintf(out, ocap, "need-absolute-path"); return -1; }
    fd_goto(argv[0]);
    snprintf(out, ocap, "dir=%s entries=%d", g_dir, g_count);
    return 0;
}
static int fda_select(int argc, char **argv, char *out, int ocap) {
    if (!g_mode) { snprintf(out, ocap, "no-dialog-open"); return -1; }
    if (argc < 1 || !argv[0]) { snprintf(out, ocap, "need-index"); return -1; }
    int v = atoi(argv[0]);
    if (v < 0 || v >= g_count) { snprintf(out, ocap, "out-of-range 0..%d", g_count - 1); return -1; }
    g_sel = v; g_focus = FD_FOCUS_LIST;
    snprintf(out, ocap, "selected=%d name=%s", v, g_entries[v].name);
    return 0;
}
static int fda_set_name(int argc, char **argv, char *out, int ocap) {
    if (g_mode != 2) { snprintf(out, ocap, "not-in-save-mode"); return -1; }
    if (argc < 1 || !argv[0]) { snprintf(out, ocap, "need-name"); return -1; }
    tf_set_text(&g_tf, argv[0]);
    g_focus = FD_FOCUS_NAME;
    snprintf(out, ocap, "filename=%s", g_tf.buf);
    return 0;
}
static const fdv_t FD_ARG_VERBS[] = {
    { "Go to folder", "chooser.goto",         0, fda_goto,
      "Navigate the chooser to an absolute path, as clicking a Places row does", &k_fdv_nav },
    { "Select row",   "chooser.select",       0, fda_select,
      "Select the file-list row at this index and focus the list", &k_fdv_sel },
    { "Set filename", "chooser.set_filename", 0, fda_set_name,
      "Type this name into the Save dialog's filename field", &k_fdv_name },
};
#define FDA_N ((int)(sizeof(FD_ARG_VERBS) / sizeof(FD_ARG_VERBS[0])))

// ---- the readable state ---------------------------------------------------
static int fds_dir(char *o, int n)   { fd_copy(o, n, g_mode ? g_dir : "(no dialog)"); return 0; }
static int fds_sel(char *o, int n)   {
    fd_copy(o, n, (g_mode && g_sel >= 0 && g_sel < g_count) ? g_entries[g_sel].name : "(none)");
    return 0;
}
static int fds_name(char *o, int n)  { fd_copy(o, n, (g_mode == 2 && g_tf.len > 0) ? g_tf.buf : "(none)"); return 0; }
// The RESULT is deliberately "(none)" until the dialog has actually resolved with
// one. g_result holds whatever the previous call left there, and reporting a
// stale path as this dialog's answer is precisely the placeholder-resolves-to-a-
// lie fault that made an earlier promise verdict vacuous.
static int fds_result(char *o, int n) { fd_copy(o, n, (!g_mode && g_ok) ? g_result : "(none)"); return 0; }
static int fdg_open(void)   { return g_mode ? 1 : 0; }
static int fdg_mode(void)   { return g_mode; }
static int fdg_focus(void)  { return g_focus; }
static int fdg_count(void)  { return g_mode ? g_count : 0; }
static int fdg_selidx(void) { return g_mode ? g_sel : -1; }
static int fdg_place(void)  { return g_mode ? g_place_sel : -1; }
static int fdg_places(void) { return g_side_n; }
static int fdg_sideon(void) { return g_side_shown ? 1 : 0; }

static const ct_item_t FD_ITEMS[] = {
    { "chooser.open", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      0, fdg_open, 0, 0, 0, 0, 0,
      "Whether a chooser dialog is up right now in this app", 0, 0 },
    { "chooser.mode", CT_ENUM, CT_READ, CT_SAFE, 0, 0, 2, "Closed|Open|Save",
      0, fdg_mode, 0, 0, 0, 0, 0,
      "Which chooser is up: closed, the Open dialog, or the Save dialog", 0, 0 },
    { "chooser.dir", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, fds_dir, 0, 0,
      "Absolute path of the folder the chooser is showing, or (no dialog)", 0, 0 },
    { "chooser.entry_count", CT_INT, CT_READ, CT_SAFE, 0, 0, FD_MAX_ENTRIES, 0,
      0, fdg_count, 0, 0, 0, 0, 0,
      "How many rows the chooser's file list holds, after the extension filter", 0, 0 },
    { "chooser.selected_index", CT_INT, CT_READ, CT_SAFE, 0, -1, FD_MAX_ENTRIES - 1, 0,
      0, fdg_selidx, 0, 0, 0, 0, 0,
      "Index of the selected file-list row, -1 for none", 0, 0 },
    { "chooser.selected", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, fds_sel, 0, 0,
      "Name of the selected file-list entry, or (none)", 0, 0 },
    { "chooser.focus", CT_ENUM, CT_READ, CT_SAFE, 0, 0, 2, "List|Places|Filename",
      0, fdg_focus, 0, 0, 0, 0, 0,
      "Which pane takes a key: the file list, the Places sidebar or the name field", 0, 0 },
    { "chooser.filename", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, fds_name, 0, 0,
      "Text in the Save dialog's filename field, or (none)", 0, 0 },
    { "chooser.place_row", CT_INT, CT_READ, CT_SAFE, 0, -1, FD_SIDE_MAX - 1, 0,
      0, fdg_place, 0, 0, 0, 0, 0,
      "Index of the highlighted Places sidebar row, -1 when no dialog is up", 0, 0 },
    { "chooser.place_count", CT_INT, CT_READ, CT_SAFE, 0, 0, FD_SIDE_MAX, 0,
      0, fdg_places, 0, 0, 0, 0, 0,
      "How many Places rows the sidebar drew, which is fewer than places.h offers when the card is short", 0, 0 },
    { "chooser.sidebar_shown", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      0, fdg_sideon, 0, 0, 0, 0, 0,
      "Whether the Places sidebar is drawn; it is dropped below a 480px card rather than squeezed", 0, 0 },
    { "chooser.result", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, fds_result, 0, 0,
      "Absolute path the last resolved chooser returned, or (none) if it was cancelled or is still up", 0, 0 },
};
#define FD_ITEMS_N ((int)(sizeof(FD_ITEMS) / sizeof(FD_ITEMS[0])))

// ---- the fragment ---------------------------------------------------------
// Rows are: the state items, then the verb table, then the arg verbs, then one
// action per PLACES ROW. The places rows are projected from places.h, the same
// model fd_draw_places() walks and the same one the Files sidebar walks, so all
// three agree about what a place is called and where it goes by construction.
static char g_fdp_tok[FD_SIDE_MAX][48];
static char g_fdp_desc[FD_SIDE_MAX][128];
static int  g_fdp_n = -1;

static void fdp_build(void) {
    places_init();
    g_fdp_n = 0;
    for (int i = 0; i < places_count() && g_fdp_n < FD_SIDE_MAX; i++) {
        const place_t *pl = places_at(i);
        char p[FD_PATH_MAX];
        if (!pl || places_path(i, p, sizeof(p)) != 0) continue;
        int n = 0;
        const char *pfx = "chooser.place.";
        for (int k = 0; pfx[k] && n < 46; k++) g_fdp_tok[g_fdp_n][n++] = pfx[k];
        for (int k = 0; pl->label[k] && n < 46; k++) {
            char c = pl->label[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
            g_fdp_tok[g_fdp_n][n++] = c;
        }
        g_fdp_tok[g_fdp_n][n] = 0;
        snprintf(g_fdp_desc[g_fdp_n], 128,
                 "Navigate the chooser to the %s place (%s), the same sidebar row", pl->label, p);
        g_fdp_n++;
    }
}

// The places index is carried in ctx as an offset into the built table, not as a
// pointer into places.h's own array, because places_at() may be rebuilt and a
// stale pointer into it would be a use-after-free waiting for a hot-plug.
static int fdp_press(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)argc; (void)argv;
    if (!g_mode) { snprintf(out, ocap, "no-dialog-open"); return -1; }
    int row = (int)((const char (*)[48])it->ctx - g_fdp_tok);
    if (row < 0 || row >= g_fdp_n) { snprintf(out, ocap, "no-such-place"); return -1; }
    char p[FD_PATH_MAX];
    // Resolved through places_path() at CALL time, which is the same call
    // fd_draw_places() makes for the row it draws: a path cached at build time
    // could name a directory that has since gone.
    if (places_path(row, p, sizeof(p)) != 0) { snprintf(out, ocap, "place-does-not-resolve"); return -1; }
    fd_goto(p);
    g_place_sel = row < g_side_n ? row : g_place_sel;
    snprintf(out, ocap, "dir=%s entries=%d", g_dir, g_count);
    return 0;
}

static int fdv_press(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    const fdv_t *v = (const fdv_t *)it->ctx;
    if (!v) return -1;
    if (v->afn) return v->afn(argc, argv, out, ocap);
    if (!v->fn) return -1;
    if (!g_mode) { snprintf(out, ocap, "no-dialog-open"); return -1; }
    v->fn();                 // the SAME function the key handler and the button run
    snprintf(out, ocap, "mode=%d dir=%s entries=%d sel=%d focus=%d open=%d",
             g_mode, g_dir, g_count, g_sel, g_focus, g_mode ? 1 : 0);
    return 0;
}

int filedlg_contract_count(void) {
    if (g_fdp_n < 0) fdp_build();
    return FD_ITEMS_N + FDV_N + FDA_N + g_fdp_n;
}

int filedlg_contract_at(int idx, ct_item_t *out) {
    if (g_fdp_n < 0) fdp_build();
    if (idx < 0 || !out) return 0;
    if (idx < FD_ITEMS_N) { *out = FD_ITEMS[idx]; return 1; }
    idx -= FD_ITEMS_N;

    ct_item_t it;
    memset(&it, 0, sizeof(it));
    it.type = CT_ACTION;
    it.access = CT_WRITE;
    // Every chooser verb is view-depth: it moves a selection or a folder view and
    // nothing outlives the dialog. The one thing that DOES reach the filesystem
    // is what the HOST does with the path afterwards, which is the host's own
    // contract row to declare, not this one's to claim.
    it.risk = CT_SAFE;
    it.actfn = fdv_press;

    if (idx < FDV_N)  { const fdv_t *v = &FD_VERBS[idx];
                        it.name = v->tok; it.desc = v->desc; it.terms = v->terms; it.ctx = v;
                        *out = it; return 1; }
    idx -= FDV_N;
    if (idx < FDA_N)  { const fdv_t *v = &FD_ARG_VERBS[idx];
                        it.name = v->tok; it.desc = v->desc; it.terms = v->terms; it.ctx = v;
                        *out = it; return 1; }
    idx -= FDA_N;
    if (idx < g_fdp_n) { it.name = g_fdp_tok[idx]; it.desc = g_fdp_desc[idx];
                         it.terms = &k_fdv_nav; it.actfn = fdp_press; it.ctx = g_fdp_tok[idx];
                         *out = it; return 1; }
    return 0;
}

// --- the two entry points --------------------------------------------------
static void fd_begin(const filedlg_host_t *host, const char *exts) {
    fd_copy(g_exts, sizeof(g_exts), exts);
    // THE START DIRECTORY, and the whole reason this file changed. It was:
    //
    //     oapp_copy(g_od_dir, sizeof(g_od_dir), "/");
    //     DIR *t = opendir("/DOCS");
    //     if (t) { closedir(t); oapp_copy(g_od_dir, sizeof(g_od_dir), "/DOCS"); }
    //
    // "/DOCS" does not exist on any shipping image (grepped: officeui.c was its
    // only mention anywhere in the tree), so in practice every Open and every
    // Save in Writer, Calc and Slides began at the filesystem ROOT, beside
    // BOOT.BMP and eighty wallpapers, with the user's own documents four
    // keystrokes away and no indication of where they were.
    //
    // Now it is the session's Documents folder, resolved by places.c through the
    // userconf accessors, with the scratch directory for a session that has no
    // home. No literal appears here at all.
    places_init();
    char start[FD_PATH_MAX];
    places_default_dir(start, sizeof(start));
    memset(&g_list, 0, sizeof(g_list));
    g_place_sel = 0;
    fd_copy(g_dir, sizeof(g_dir), start);
    fd_scan(g_dir, g_exts);
    g_sel = (g_count > 0) ? 0 : -1;
    (void)host;
}

static void fd_repaint_host(const filedlg_host_t *host) {
    if (host->repaint) host->repaint(host->ctx);
}

const char *filedlg_open(const filedlg_host_t *host, const char *exts, const char *title) {
    if (!host) return 0;
    if (!title || !*title) title = "Open Document";
    fd_begin(host, exts);
    g_focus = FD_FOCUS_LIST;
    g_done = 0; g_ok = 0; g_mode = 1;

    fd_repaint_host(host);
    fd_draw(host, title, 0);

    while (!g_done) {
        gui_event_t ev;
        // A BOUNDED timeout only when the host opted into live contract
        // delivery: with no contract this blocks exactly as it always has, so a
        // caller that does not opt in sees no behaviour change at all.
        int et = gui_mods_next_event(host->win, &ev, host->contract ? 100 : -1);
        if (host->contract && contract_live_poll(host->contract))
            fd_draw(host, title, 0);
        if (g_done) break;
        if (et <= 0) continue;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: g_done = 1; g_ok = 0; break;
            case EVENT_RESIZE:
                if (host->note_resize) host->note_resize(host->ctx, ev.mouse_x, ev.mouse_y);
                fd_repaint_host(host);
                break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode; char c = ev.key_char;
                if (c == GUI_KEY_ESC) { g_done = 1; g_ok = 0; }
                else if (kc == GUI_KEY_TAB) fd_focus_next(0);
                else if (c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) fdv_enter();
                else if (g_focus == FD_FOCUS_PLACES) {
                    if (kc == GUI_KEY_UP   && g_place_sel > 0) g_place_sel--;
                    else if (kc == GUI_KEY_DOWN && g_place_sel < g_side_n - 1) g_place_sel++;
                    else if (kc == GUI_KEY_HOME) g_place_sel = 0;
                    else if (kc == GUI_KEY_END)  g_place_sel = g_side_n > 0 ? g_side_n - 1 : 0;
                }
                else if (kc == GUI_KEY_UP)   gui_list_move_sel(&g_list, &g_sel, -1);
                else if (kc == GUI_KEY_DOWN) gui_list_move_sel(&g_list, &g_sel, 1);
                else if (kc == GUI_KEY_PGUP) gui_list_move_sel(&g_list, &g_sel, -8);
                else if (kc == GUI_KEY_PGDN) gui_list_move_sel(&g_list, &g_sel, 8);
                else if (kc == GUI_KEY_HOME) gui_list_move_sel(&g_list, &g_sel, -g_count);
                else if (kc == GUI_KEY_END)  gui_list_move_sel(&g_list, &g_sel, g_count);
                break;
            }
            case EVENT_MOUSE_DOWN: {
                int mx = ev.mouse_x, my = ev.mouse_y;
                if (fd_hit_button(mx, my, g_btn_cancel_x)) { g_done = 1; g_ok = 0; break; }
                if (fd_hit_button(mx, my, g_btn_ok_x)) { fdv_accept(); break; }
                int pr = fd_hit_place(mx, my);
                if (pr >= 0) { g_focus = FD_FOCUS_PLACES; fd_goto_place(pr); break; }
                int r = gui_list_press(&g_list, mx, my);
                if (r >= 0) {
                    g_focus = FD_FOCUS_LIST;
                    g_sel = r;
                    if (r < g_count && g_entries[r].is_dir) { int c2 = 0; fd_activate_open(&c2); }
                }
                break;
            }
            case EVENT_MOUSE_MOVE:   gui_list_motion(&g_list, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:     gui_list_release(&g_list); break;
            case EVENT_MOUSE_SCROLL: gui_list_wheel(&g_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta); break;
            default: break;
        }
        if (!g_done) fd_draw(host, title, 0);
    }
    g_mode = 0;
    fd_repaint_host(host);
    return g_ok ? g_result : 0;
}

const char *filedlg_save(const filedlg_host_t *host, const char *defname, const char *title) {
    if (!host) return 0;
    if (!title || !*title) title = "Save Document";
    // The Save browser shows every entry: the extension comes from the typed
    // name, so filtering the list would hide the file the user is about to
    // overwrite.
    fd_begin(host, "");
    tf_init(&g_tf, g_fname, sizeof(g_fname));
    tf_set_text(&g_tf, defname ? defname : "");
    g_focus = FD_FOCUS_NAME;
    g_done = 0; g_ok = 0; g_mode = 2;

    fd_repaint_host(host);
    fd_draw(host, title, 1);

    while (!g_done) {
        gui_event_t ev;
        int et = gui_mods_next_event(host->win, &ev, host->contract ? 100 : -1);
        if (host->contract && contract_live_poll(host->contract))
            fd_draw(host, title, 1);
        if (g_done) break;
        if (et <= 0) continue;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: g_done = 1; g_ok = 0; break;
            case EVENT_RESIZE:
                if (host->note_resize) host->note_resize(host->ctx, ev.mouse_x, ev.mouse_y);
                fd_repaint_host(host);
                break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode; char c = ev.key_char;
                if (c == GUI_KEY_ESC) { g_done = 1; g_ok = 0; }
                else if (kc == GUI_KEY_TAB) fd_focus_next(1);
                else if (c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) fdv_enter();
                else if (g_focus == FD_FOCUS_PLACES) {
                    if (kc == GUI_KEY_UP   && g_place_sel > 0) g_place_sel--;
                    else if (kc == GUI_KEY_DOWN && g_place_sel < g_side_n - 1) g_place_sel++;
                    else if (kc == GUI_KEY_HOME) g_place_sel = 0;
                    else if (kc == GUI_KEY_END)  g_place_sel = g_side_n > 0 ? g_side_n - 1 : 0;
                }
                else if (g_focus == FD_FOCUS_NAME) tf_handle_key(&g_tf, &ev);
                else if (kc == GUI_KEY_UP)   gui_list_move_sel(&g_list, &g_sel, -1);
                else if (kc == GUI_KEY_DOWN) gui_list_move_sel(&g_list, &g_sel, 1);
                else if (kc == GUI_KEY_PGUP) gui_list_move_sel(&g_list, &g_sel, -8);
                else if (kc == GUI_KEY_PGDN) gui_list_move_sel(&g_list, &g_sel, 8);
                break;
            }
            case EVENT_MOUSE_DOWN: {
                int mx = ev.mouse_x, my = ev.mouse_y;
                if (fd_hit_button(mx, my, g_btn_cancel_x)) { g_done = 1; g_ok = 0; break; }
                if (fd_hit_button(mx, my, g_btn_ok_x)) { fdv_accept(); break; }
                int pr = fd_hit_place(mx, my);
                if (pr >= 0) { g_focus = FD_FOCUS_PLACES; fd_goto_place(pr); break; }
                int r = gui_list_press(&g_list, mx, my);
                if (r >= 0) {
                    g_sel = r;
                    if (r < g_count && g_entries[r].is_dir) fd_enter_dir(&g_entries[r]);
                    else fd_activate_save_list();
                }
                break;
            }
            case EVENT_MOUSE_MOVE:   gui_list_motion(&g_list, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:     gui_list_release(&g_list); break;
            case EVENT_MOUSE_SCROLL: gui_list_wheel(&g_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta); break;
            default: break;
        }
        if (!g_done) fd_draw(host, title, 1);
    }
    g_mode = 0;
    fd_repaint_host(host);
    return g_ok ? g_result : 0;
}
