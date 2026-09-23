// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// otkdemo.c - the office UI toolkit EXAMPLE app (not shipped in the golden;
// built by hand with the Makefile beside it). It exercises every widget the
// per-app agents code against: the shell's menus/toolbar/chrome hooks, a
// Writer-shaped toolbar with two combos (font family from the /FONTS
// registry, font size), the ruler, a form modal (Paragraph), a modal with a
// preview pane (Insert Table), the three-button unsaved-changes prompt, the
// sheet-tab strip, the thumbnail strip and the formula bar (drawn below the
// page so one screenshot covers all of them).
//
// OTKDEMO_AUTOMODAL: a throwaway VERIFICATION build flag. After N ticks it
// opens the Paragraph modal by itself, so a headless screendump can show the
// modal without a mouse or a key reaching the guest.
#include "../../../libc/gui.h"
#include "../../../libc/gui_menu.h"
#include "../../../libc/theme.h"
#include "../../../libc/stdio.h"
#include "../../../libc/string.h"
#include <stdlib.h>
#include "../../include/officeui.h"
#include "../officetk.h"

enum { T_NEW = 1, T_OPEN, T_SAVE, T_UNDO, T_REDO, T_BOLD, T_ITAL, T_UNDL, T_FONT, T_SIZE,
       T_ALNL, T_ALNC, T_ALNR, T_ALNJ, T_LSTB, T_LSTN, T_TABLE, T_IMAGE };
enum { M_EDIT_UNDO = 20, M_EDIT_REDO, M_FMT_PARA = 30, M_INS_TABLE, M_INS_IMAGE, M_VIEW_RULER = 40, M_HELP_ABOUT = 50 };

static const otb_item_t ITEMS[] = {
    { OTB_BUTTON, T_NEW,  "WRNEW",  "New (Ctrl+N)",   0, 0 },
    { OTB_BUTTON, T_OPEN, "WROPEN", "Open (Ctrl+O)",  0, 0 },
    { OTB_BUTTON, T_SAVE, "WRSAVE", "Save (Ctrl+S)",  0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_UNDO, "WRUNDO", "Undo (Ctrl+Z)",  0, 0 },
    { OTB_BUTTON, T_REDO, "WRREDO", "Redo (Ctrl+Y)",  0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_BOLD, "WRBOLD", "Bold (Ctrl+B)",  0, 0 },
    { OTB_TOGGLE, T_ITAL, "WRITAL", "Italic (Ctrl+I)", 0, 0 },
    { OTB_TOGGLE, T_UNDL, "WRUNDL", "Underline (Ctrl+U)", 0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_COMBO,  T_FONT, 0, "Font", 150, 0 },
    { OTB_COMBO,  T_SIZE, 0, "Size", 56, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_ALNL, "WRALNL", "Align Left",  0, 1 },
    { OTB_TOGGLE, T_ALNC, "WRALNC", "Center",      0, 1 },
    { OTB_TOGGLE, T_ALNR, "WRALNR", "Align Right", 0, 1 },
    { OTB_TOGGLE, T_ALNJ, "WRALNJ", "Justify",     0, 1 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_LSTB, "WRLSTB", "Bullet List",   0, 0 },
    { OTB_TOGGLE, T_LSTN, "WRLSTN", "Numbered List", 0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_TABLE, "WRTABLE", "Insert Table...", 0, 0 },
    { OTB_BUTTON, T_IMAGE, "WRIMAGE", "Insert Image...", 0, 0 },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

static const gui_menu_item_t FILE_ITEMS[] = {
    { "New",        "Ctrl+N", OAPP_MID_FILE_NEW,     true, false },
    { "Open...",    "Ctrl+O", OAPP_MID_FILE_OPEN,    true, false },
    { "Save",       "Ctrl+S", OAPP_MID_FILE_SAVE,    true, false },
    { "Save As...", 0,        OAPP_MID_FILE_SAVE_AS, true, false },
    { 0, 0, 0, false, false },
    { "Close",      0,        OAPP_MID_FILE_CLOSE,   true, false },
};
static const gui_menu_item_t EDIT_ITEMS[] = {
    { "Undo", "Ctrl+Z", M_EDIT_UNDO, true, false },
    { "Redo", "Ctrl+Y", M_EDIT_REDO, true, false },
};
static const gui_menu_item_t FORMAT_ITEMS[] = {
    { "Paragraph...", 0, M_FMT_PARA, true, false },
};
static const gui_menu_item_t INSERT_ITEMS[] = {
    { "Table...", 0, M_INS_TABLE, true, false },
    { "Image...", 0, M_INS_IMAGE, true, false },
};
static const gui_menu_item_t VIEW_ITEMS[] = {
    { "Ruler", 0, M_VIEW_RULER, true, true },
};
static const gui_menu_t MENUS[] = {
    { "File", FILE_ITEMS, 6 }, { "Edit", EDIT_ITEMS, 2 }, { "Format", FORMAT_ITEMS, 1 },
    { "Insert", INSERT_ITEMS, 2 }, { "View", VIEW_ITEMS, 1 },
};

static office_app   *g_app;
static otb_toolbar_t g_tb;
static otabs_t       g_tabs;
static othumbs_t     g_thumbs;
static ofxbar_t      g_fx;
static int           g_ticks;
static char          g_last[120] = "Ready";
static int           g_ruler = 1;

// Font family list from the registry (deduplicated).
static char g_fam[32][48]; static int g_nfam;
static void build_families(void) {
    int n = font_count(); if (n > 64) n = 64;
    for (int i = 0; i < n && g_nfam < 32; i++) {
        char nm[48];
        if (font_name(i, nm, sizeof(nm)) <= 0) continue;
        int dup = 0;
        for (int k = 0; k < g_nfam; k++) if (!strcmp(g_fam[k], nm)) { dup = 1; break; }
        if (!dup) strncpy(g_fam[g_nfam++], nm, 47);
    }
    if (!g_nfam) { strncpy(g_fam[0], "DejaVu Sans", 47); g_nfam = 1; }
}
static const char *fam_label(void *ctx, int i, char *buf, int cap) { (void)ctx; (void)buf; (void)cap; return i >= 0 && i < g_nfam ? g_fam[i] : ""; }
static const int SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72 };
#define NSIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))
static const char *size_label(void *ctx, int i, char *buf, int cap) { (void)ctx; if (i < 0 || i >= NSIZES) return ""; snprintf(buf, cap, "%d", SIZES[i]); return buf; }

static const char *tab_label(void *ctx, int i) { (void)ctx; static const char *n[] = { "Sheet1", "Budget 2026", "A very long sheet name that ellipsizes", "Notes" }; return i >= 0 && i < 4 ? n[i] : ""; }
static void thumb_render(int win, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    uint32_t ink = theme_color(THEME_COLOR_ON_SURFACE), mut = theme_color(THEME_COLOR_MUTED);
    win_draw_rect(win, x + w / 10, y + h / 8, w * 8 / 10, 2, ink);
    for (int k = 0; k < 3 + (i % 3); k++) win_draw_rect(win, x + w / 10, y + h / 3 + k * 8, w * (5 + (k % 3)) / 10, 1, mut);
}

static void draw(office_app *a) {
    int win = oapp_win(a);
    int cx, cy, cw, ch; oapp_content_rect(a, &cx, &cy, &cw, &ch);
    int ty, th, by, bh; oapp_chrome_rects(a, &ty, &th, &by, &bh);
    uint32_t bg = theme_color(THEME_COLOR_WINDOW_BG);
    win_draw_rect(win, cx, cy, cw, ch, bg);
    // Page sheet: 8.5 in wide at 96 ppi if it fits, centred.
    int page_w = 816; if (page_w > cw - 32) page_w = cw - 32;
    int page_x = cx + (cw - page_w) / 2, page_y = cy + 16;
    int margin = 96;
    if (g_ruler && th > 0) oruler_draw(win, 0, ty, cw, page_x, page_w, margin, margin, 96);
    win_draw_rect(win, page_x, page_y, page_w, 150, theme_color(THEME_COLOR_TEXTBOX_BG));
    gui_draw_rect_outline(win, page_x, page_y, page_w, 150, theme_color(THEME_COLOR_WINDOW_BORDER));
    win_draw_text_ttf(win, page_x + margin, page_y + 24, "Office toolkit demo: toolbar, combos, ruler, tabs, thumbnails, formula bar.", 14, theme_color(THEME_COLOR_ON_SURFACE));
    win_draw_text_ttf(win, page_x + margin, page_y + 48, g_last, 14, theme_color(THEME_COLOR_ON_SURFACE));
    win_draw_text_ttf(win, page_x + margin, page_y + 72, "Alt+F opens File. Alt+T focuses the toolbar. Format > Paragraph... opens a modal.", 12, theme_color(THEME_COLOR_MUTED));
    // The Calc/Slides widgets, laid out below the page so one frame shows them.
    int y2 = page_y + 150 + 16;
    ofxbar_move(&g_fx, cx, y2, cw);
    ofxbar_draw(win, &g_fx);
    y2 += ofxbar_height() + 8;
    otabs_move(&g_tabs, cx, y2, cw);
    otabs_layout(&g_tabs, 4, g_tabs.active < 0 ? 1 : g_tabs.active, tab_label, 0);
    otabs_draw(win, &g_tabs, tab_label, 0);
    y2 += OTAB_H + 8;
    int sh = cy + ch - y2; if (sh < 120) sh = 120;
    othumbs_config(&g_thumbs, cx, y2, OTHUMB_STRIP_W, sh, 7, g_thumbs.sel);
    othumbs_draw(win, &g_thumbs, thumb_render, 0);
}

// --- Modals -------------------------------------------------------------------
static const char *ALIGN[] = { "Left", "Center", "Right", "Justify" };
static const char *SPACING[] = { "Single", "1.15", "1.5", "Double" };
static const char *LISTS[] = { "None", "Bullet", "Numbered" };
static void paragraph_modal(void) {
    static char li[8] = "0", ri[8] = "0", fi[8] = "0", sb[8] = "0", sa[8] = "8";
    static int align = 0, spacing = 0, list = 0;
    om_field_t f[] = {
        { OM_RADIO,  "Alignment",    0, 0, 0, 0, ALIGN, 4, &align, 0, 0, 0 },
        { OM_NUMBER, "Left indent",  li, sizeof(li), 0, 288, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "Right indent", ri, sizeof(ri), 0, 288, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "First line",   fi, sizeof(fi), -144, 288, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "Space before", sb, sizeof(sb), 0, 144, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "Space after",  sa, sizeof(sa), 0, 144, 0, 0, 0, 0, 0, 0 },
        { OM_COMBO,  "Line spacing", 0, 0, 0, 0, SPACING, 4, &spacing, 0, 0, 0 },
        { OM_COMBO,  "List",         0, 0, 0, 0, LISTS, 3, &list, 0, 0, 0 },
    };
    om_spec_t s = { "Paragraph", OM_SIZE_M, f, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
    int r = omodal_run(g_app, &s);
    snprintf(g_last, sizeof(g_last), "Paragraph: %s align=%s left=%s right=%s first=%s spacing=%s list=%s",
             r == OM_OK ? "OK" : "Cancel", ALIGN[align], li, ri, fi, SPACING[spacing], LISTS[list]);
}

static int g_rows = 3, g_cols = 3;
static void table_preview(int win, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    uint32_t line = theme_color(THEME_COLOR_BORDER_SUBTLE);
    int r = g_rows < 1 ? 1 : (g_rows > 12 ? 12 : g_rows), c = g_cols < 1 ? 1 : (g_cols > 12 ? 12 : g_cols);
    for (int i = 0; i <= r; i++) win_draw_rect(win, x + 8, y + 8 + i * (h - 16) / r, w - 16, 1, line);
    for (int j = 0; j <= c; j++) win_draw_rect(win, x + 8 + j * (w - 16) / c, y + 8, 1, h - 16, line);
}
static char g_rows_s[8] = "3", g_cols_s[8] = "3";
static void table_change(void *ctx) { (void)ctx; g_rows = atoi(g_rows_s); g_cols = atoi(g_cols_s); }
static void table_modal(void) {
    static int header = 1, borders = 1;
    om_field_t f[] = {
        { OM_NUMBER, "Rows",       g_rows_s, sizeof(g_rows_s), 1, 100, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "Columns",    g_cols_s, sizeof(g_cols_s), 1, 20,  0, 0, 0, 0, 0, 0 },
        { OM_CHECK,  "Header row", 0, 0, 0, 0, 0, 0, 0, &header, 0, 0 },
        { OM_CHECK,  "Borders",    0, 0, 0, 0, 0, 0, 0, &borders, 0, 0 },
    };
    om_spec_t s = { "Insert Table", OM_SIZE_S, f, 4, "Insert", 0, 0, 0, 120, table_preview, table_change, 0 };
    int r = omodal_run(g_app, &s);
    snprintf(g_last, sizeof(g_last), "Insert Table: %s %sx%s header=%d borders=%d", r == OM_OK ? "Insert" : "Cancel", g_rows_s, g_cols_s, header, borders);
}

static int on_event(office_app *a, int type, int p1, int p2) {
    switch (type) {
        case OAPP_EVENT_TOOL: {
            ocombo_t *c = otb_combo(&g_tb, p1);
            if (p1 == T_FONT || p1 == T_SIZE) snprintf(g_last, sizeof(g_last), "Combo %d -> index %d (%s)", p1, p2, c ? c->text : "");
            else snprintf(g_last, sizeof(g_last), "Tool id=%d value=%d", p1, p2);
            if (p1 == T_TABLE) table_modal();
            if (p1 == T_IMAGE) { const char *p = oapp_open_dialog_titled(a, ".png.jpg.jpeg.bmp", "Insert Image"); snprintf(g_last, sizeof(g_last), "Image: %s", p ? p : "(cancelled)"); }
            if (p1 == T_BOLD) oapp_set_dirty(a, 1);
            return 1;
        }
        case OAPP_EVENT_MENU:
            snprintf(g_last, sizeof(g_last), "Menu id=%d", p1);
            if (p1 == M_FMT_PARA) paragraph_modal();
            if (p1 == M_INS_TABLE) table_modal();
            if (p1 == M_VIEW_RULER) { g_ruler = !g_ruler; oapp_set_chrome(a, g_ruler ? ORULER_H : 0, 0); }
            return 1;
        case OAPP_EVENT_FILE_NEW: case OAPP_EVENT_FILE_OPEN: case OAPP_EVENT_FILE_SAVE:
        case OAPP_EVENT_FILE_SAVE_AS: case OAPP_EVENT_FILE_CLOSE:
            snprintf(g_last, sizeof(g_last), "File event %d", type);
            if (type == OAPP_EVENT_FILE_OPEN) { const char *p = oapp_open_dialog(a, ".docx.odt"); snprintf(g_last, sizeof(g_last), "Open: %s", p ? p : "(cancelled)"); }
            return 1;
        case OAPP_EVENT_TICK:
            g_ticks++;
#ifdef OTKDEMO_AUTOMODAL
            if (g_ticks == OTKDEMO_AUTOMODAL) paragraph_modal();
#endif
            return 1;
        case EVENT_WINDOW_CLOSE:
            if (oapp_is_dirty(a)) {
                int r = omodal_confirm3(a, "Unsaved changes", "Save changes to Untitled.docx before closing?", "Save", "Discard");
                if (r == OM_CANCEL) return 1;
            }
            return 0;
        case EVENT_MOUSE_DOWN: {
            int r = otabs_press(&g_tabs, p1, p2);
            if (r >= 0) { g_tabs.active = r; snprintf(g_last, sizeof(g_last), "Tab %d", r); return 1; }
            if (r != -1) return 1;
            r = othumbs_press(&g_thumbs, p1, p2);
            if (r >= 0) { snprintf(g_last, sizeof(g_last), "Slide %d", r + 1); return 1; }
            if (r != -1) return 1;
            r = ofxbar_press(&g_fx, p1, p2);
            if (r != -1) return 1;
            ofxbar_blur(&g_fx);
            return 1;
        }
        case EVENT_MOUSE_UP:
            if (otabs_release(&g_tabs, p1, p2) == OTAB_ADD) snprintf(g_last, sizeof(g_last), "Add sheet");
            othumbs_release(&g_thumbs);
            if (ofxbar_release(&g_fx, p1, p2) == OFX_PRESS_FXBTN) snprintf(g_last, sizeof(g_last), "Insert Function...");
            return 1;
        case EVENT_MOUSE_MOVE:
            otabs_motion(&g_tabs, p1, p2); othumbs_motion(&g_thumbs, p1, p2); ofxbar_motion(&g_fx, p1, p2);
            return 1;
        case EVENT_MOUSE_SCROLL:
            othumbs_wheel(&g_thumbs, 0, p2, p1);
            return 1;
        case EVENT_KEY_DOWN: {
            gui_event_t ev; memset(&ev, 0, sizeof(ev)); ev.type = EVENT_KEY_DOWN; ev.keycode = (uint32_t)p1; ev.key_char = (char)p2;
            int r = ofxbar_key(&g_fx, &ev);
            if (r) { if (r == 2) snprintf(g_last, sizeof(g_last), "Formula commit: %s", g_fx.fx_buf); return 1; }
            r = otabs_key(&g_tabs, (unsigned)p1, (char)p2, (int)gui_mods_get());
            if (r == OTAB_RENAMED) snprintf(g_last, sizeof(g_last), "Renamed tab %d to %s", g_tabs.renaming_tab, g_tabs.rename_buf);
            if (r != -1) return 1;
            if (p2 == 'r' && gui_mods_is(GUI_MOD_CTRL)) { otabs_begin_rename(&g_tabs, g_tabs.active, tab_label(0, g_tabs.active)); return 1; }
            if (p2 == 0x10) { paragraph_modal(); return 1; }   // Ctrl+P
            if (othumbs_key(&g_thumbs, (unsigned)p1)) { othumbs_reveal(&g_thumbs); return 1; }
            return 1;
        }
        default: return 1;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
#ifdef OTKDEMO_NARROW
    // Verification build: a 640 px window forces the toolbar's whole-group
    // overflow into the MORE button (spec 3.1).
    g_app = oapp_create("OFFICE TOOLKIT DEMO", 640, 480);
#else
    g_app = oapp_create("OFFICE TOOLKIT DEMO", 900, 640);
#endif
    if (!g_app) return 1;
    build_families();
    oapp_set_menus(g_app, MENUS, 5);
    otb_init(&g_tb, ITEMS, N_ITEMS, 0, 0, 900);   // the shell re-lays it out to the real width
    ocombo_init(otb_combo(&g_tb, T_FONT), 0, 0, 150, g_nfam, 0, 0, fam_label, 0);
    ocombo_init(otb_combo(&g_tb, T_SIZE), 0, 0, 56, NSIZES, 5, 1, size_label, 0);
    otb_set_checked(&g_tb, T_ALNL, 1);
    otb_set_checked(&g_tb, T_BOLD, 1);
    otb_set_enabled(&g_tb, T_REDO, 0);
    oapp_set_toolbar(g_app, &g_tb);
    oapp_set_chrome(g_app, ORULER_H, 0);
    otabs_init(&g_tabs, 0, 0, 900); g_tabs.active = 1;
    othumbs_config(&g_thumbs, 0, 0, OTHUMB_STRIP_W, 200, 7, 2);
    ofxbar_init(&g_fx, 0, 0, 900);
    ofxbar_set(&g_fx, "B4", "=SUM(A1:A3)*2");
    oapp_set_tick_ms(g_app, 1000);
    oapp_run(g_app, draw, on_event);
    oapp_destroy(g_app);
    return 0;
}
