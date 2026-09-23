// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// SHEETS - MayteraOS native office suite spreadsheet ("Calc"). Built on the
// shared office_app shell (officelib/officeui) and the shared office UI
// toolkit (officelib/officeui/officetk.h: otoolbar, ofxbar, otabs, omodal).
// Opens/saves .xlsx/.ods (xlsx_save/ods_save) and reads legacy .xls
// read-only (xls_ole_load, via office_open_workbook()). Formula evaluation is
// officelib/model/formula.c (fx_recalc) - this app never re-implements
// arithmetic itself.
//
// Chrome (docs/OFFICE_UI_DESIGN.md 2.2): menu bar | toolbar | formula bar |
// column header + grid | sheet-tab strip | status bar. The toolbar, formula
// bar and tab strip are the toolkit widgets, not bespoke drawing; the modals
// (Format Cells, Insert Function, Rename Sheet, Unsaved changes) are omodal
// forms. Include order per officetk.h: gui.h, gui_menu.h, officeui.h,
// officetk.h.
#include "../../libc/gui.h"
#include "../../libc/gui_menu.h"
#include "../../libc/gui_scroll.h"
#include "../../libc/gui_font.h"
#include "../../libc/theme.h"
#include "../../libc/stdio.h"
#include "../../officelib/include/officeui.h"
#include "../../officelib/officeui/officetk.h"
#include "../../officelib/include/formats.h"
#include "../../officelib/include/formula.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// --- Grid geometry constants (spec 2.2; ROW_H/COL_W/RH_W/CH_H are the shipped values)
#define ROW_H   22
#define COL_W   78
#define RH_W    44   // row-header gutter width
#define CH_H    22   // column-header row height
#define CELL_PT 12   // cell text size (fits the 22 px row with 1 px clear)
#define DBLCLICK_MS 400
#define UNDO_MAX 64

// --- Toolbar item ids (delivered as OAPP_EVENT_TOOL) -------------------------
enum { T_NEW = 1, T_OPEN, T_SAVE, T_UNDO, T_REDO, T_BOLD, T_ITALIC,
       T_FMTNUM, T_FMTCUR, T_FMTPCT, T_SUM, T_FX, T_BORDERS };
// --- App menu item ids (delivered as OAPP_EVENT_MENU; 1..9 are the shell's) -
enum { M_EDIT_UNDO = 20, M_EDIT_REDO, M_EDIT_DELETE,
       M_FMT_CELLS = 30, M_FMT_NUM, M_FMT_CUR, M_FMT_PCT, M_FMT_BOLD, M_FMT_ITALIC, M_FMT_RENAME,
       M_INS_FUNC = 40, M_INS_SHEET,
       M_VIEW_FXBAR = 50, M_VIEW_GRID };

// Calc toolbar (spec 2.2 / 4): SCNEW SCOPEN SCSAVE | WRUNDO WRREDO (borrowed:
// the SC set has no undo/redo) | SCBOLD SCITALIC | SCFMTNUM SCFMTCUR SCFMTPCT |
// SCSUM SCFX | SCBRDALL. 384 px at retro metrics, fits 640 without overflow.
static const otb_item_t TB_ITEMS[] = {
    { OTB_BUTTON, T_NEW,     "SCNEW",    "New (Ctrl+N)",       0, 0 },
    { OTB_BUTTON, T_OPEN,    "SCOPEN",   "Open (Ctrl+O)",      0, 0 },
    { OTB_BUTTON, T_SAVE,    "SCSAVE",   "Save (Ctrl+S)",      0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_UNDO,    "WRUNDO",   "Undo (Ctrl+Z)",      0, 0 },
    { OTB_BUTTON, T_REDO,    "WRREDO",   "Redo (Ctrl+Y)",      0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_BOLD,    "SCBOLD",   "Bold (Ctrl+B)",      0, 0 },
    { OTB_TOGGLE, T_ITALIC,  "SCITALIC", "Italic (Ctrl+I)",    0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_FMTNUM,  "SCFMTNUM", "Number Format",      0, 1 },
    { OTB_TOGGLE, T_FMTCUR,  "SCFMTCUR", "Currency",           0, 1 },
    { OTB_TOGGLE, T_FMTPCT,  "SCFMTPCT", "Percent",            0, 1 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_SUM,     "SCSUM",    "AutoSum",            0, 0 },
    { OTB_BUTTON, T_FX,      "SCFX",     "Insert Function...", 0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    // DISABLED: wb_style (sheetmodel.h) carries numfmt_code/bold/italic/color
    // and no border field, and neither xlsx_save nor ods_save writes borders.
    // Enabling the toggle would draw a state the file cannot keep.
    { OTB_TOGGLE, T_BORDERS, "SCBRDALL", "Borders (not stored by the workbook model yet)", 0, 0 },
};
#define N_TB_ITEMS ((int)(sizeof(TB_ITEMS) / sizeof(TB_ITEMS[0])))

static const gui_menu_item_t FILE_ITEMS[] = {
    { "New",        "Ctrl+N", OAPP_MID_FILE_NEW,     true, false },
    { "Open...",    "Ctrl+O", OAPP_MID_FILE_OPEN,    true, false },
    { "Save",       "Ctrl+S", OAPP_MID_FILE_SAVE,    true, false },
    { "Save As...", 0,        OAPP_MID_FILE_SAVE_AS, true, false },
    { 0, 0, 0, false, false },
    { "Close",      0,        OAPP_MID_FILE_CLOSE,   true, false },
};
// Non-const: Undo/Redo enable state follows the undo stacks.
static gui_menu_item_t EDIT_ITEMS[] = {
    { "Undo",   "Ctrl+Z", M_EDIT_UNDO,   false, false },
    { "Redo",   "Ctrl+Y", M_EDIT_REDO,   false, false },
    { 0, 0, 0, false, false },
    { "Delete", "Del",    M_EDIT_DELETE, true,  false },
};
static const gui_menu_item_t FORMAT_ITEMS[] = {
    { "Cells...",        0,        M_FMT_CELLS,  true, false },
    { 0, 0, 0, false, false },
    { "Number",          0,        M_FMT_NUM,    true, false },
    { "Currency",        0,        M_FMT_CUR,    true, false },
    { "Percent",         0,        M_FMT_PCT,    true, false },
    { 0, 0, 0, false, false },
    { "Bold",            "Ctrl+B", M_FMT_BOLD,   true, false },
    { "Italic",          "Ctrl+I", M_FMT_ITALIC, true, false },
    { 0, 0, 0, false, false },
    { "Rename Sheet...", 0,        M_FMT_RENAME, true, false },
};
static const gui_menu_item_t INSERT_ITEMS[] = {
    { "Function...", 0, M_INS_FUNC,  true, false },
    { "Sheet",       0, M_INS_SHEET, true, false },
};
// Non-const: the check marks track the live view state.
static gui_menu_item_t VIEW_ITEMS[] = {
    { "Formula Bar", 0, M_VIEW_FXBAR, true, true },
    { "Gridlines",   0, M_VIEW_GRID,  true, true },
};
static const gui_menu_t MENUS[] = {
    { "File",   FILE_ITEMS,   6 },
    { "Edit",   EDIT_ITEMS,   4 },
    { "Format", FORMAT_ITEMS, 10 },
    { "Insert", INSERT_ITEMS, 2 },
    { "View",   VIEW_ITEMS,   2 },
};

// --- State ----------------------------------------------------------------------
static office_app *g_app;
static workbook   *g_wb;
static char        g_path[512];
static int         g_has_path;
static int         g_readonly;
static int         g_cur_sheet;
static int         g_sel_row, g_sel_col;
static int         g_col_off;      // first visible column
static int         g_max_row, g_max_col;  // navigable ceiling (grows with content)
static gui_scroll_t g_vscroll;

static int         g_editing;      // in-cell edit live (g_edit_tf)
static char        g_edit_buf[512];
static textfield_t g_edit_tf;

static otb_toolbar_t g_tb;
static ofxbar_t      g_fx;
static otabs_t       g_tabs;
static int           g_show_fx = 1;
static int           g_gridlines = 1;
static int           g_last_tab = -1;            // double-click detection on the tab strip
static unsigned long g_last_tab_ms;
static int           g_face, g_face_style;       // UI font face for cell text (bold/italic via style bits)

// Geometry cached by relayout(), reused by draw() and the event handlers so
// hit-testing (mouse clicks, keyboard paging) matches what was last drawn.
static int g_cx, g_cy, g_cw, g_ch;         // content rect (grid area: col header + cells)
static int g_grid_x, g_grid_y, g_grid_w;   // grid_y = top of column-header row
static int g_rows_h;                       // height of the cell rows area (below col header)
static int g_n_vis_rows, g_n_vis_cols;

// --- Small helpers -----------------------------------------------------------------
static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static int has_ext_ci(const char *path, const char *ext) {
    int pl = (int)strlen(path), el = (int)strlen(ext);
    if (pl < el) return 0;
    return strcasecmp(path + pl - el, ext) == 0;
}

static sheet *cur_sheet(void) {
    if (!g_wb || g_cur_sheet < 0 || g_cur_sheet >= g_wb->nsheet) return 0;
    return &g_wb->sheets[g_cur_sheet];
}

// Read-only peek: NULL if outside the sheet's current allocated extent (an
// unfilled cell reads as empty without growing the dense array - growing is
// reserved for an actual edit, via sheet_cell()).
static const cell *peek_cell(const sheet *s, int row, int col) {
    if (!s || row < 0 || col < 0 || row >= s->nrows || col >= s->ncols) return 0;
    return &s->cells[(long)row * s->ncols + col];
}

static const wb_style *cell_style(const cell *c) {
    if (!c || !g_wb || c->style_id < 0 || c->style_id >= g_wb->nstyle) return 0;
    return &g_wb->styles[c->style_id];
}

// Column index -> letters ("A".."Z","AA"...). Reuses rc_to_a1 (the shared
// primitive): rc_to_a1(0,col,..) yields the letters followed by exactly one
// digit ("1"), so stripping the last byte recovers the bare column label.
static void col_letters(int col, char *out, int cap) {
    char tmp[32];
    rc_to_a1(0, col, tmp, sizeof(tmp));
    int n = (int)strlen(tmp);
    if (n > 0) tmp[n - 1] = 0;
    int i = 0; for (; tmp[i] && i < cap - 1; i++) out[i] = tmp[i];
    out[i] = 0;
}

static int try_parse_number(const char *s, double *out) {
    if (!s || !*s) return 0;
    char *end = 0;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ') end++;
    if (*end != 0) return 0;
    *out = v;
    return 1;
}

// Theme token with a fallback for a kernel that answers 0 for the id (the
// same pairing discipline as officeui.c: 0 is a legitimate black for an ink,
// so a fallback is only ever applied to a GROUND token, never an ink alone).
static uint32_t tok(theme_color_id_t id, uint32_t fallback) {
    uint32_t c = theme_color(id);
    return c ? c : fallback;
}

// ---------------------------------------------------------------------------
// Number formats. wb_style.numfmt_code holds an Excel-style code ("0.00",
// "#,##0.00", "$#,##0.00", "0.00%", "yyyy-mm-dd", "@", with an optional
// ";[Red]-..." negative section). The importer hands these through verbatim
// and xlsx_save writes them back, so the code string IS the stored format;
// this parser only needs to recognise the family the app itself writes plus
// the common built-ins (xlsx_builtin_numfmt).
// ---------------------------------------------------------------------------
enum { NF_GENERAL = 0, NF_NUMBER, NF_CURRENCY, NF_PERCENT, NF_DATE, NF_TEXT };
static const char *NF_CAT_NAMES[] = { "General", "Number", "Currency", "Percent", "Date", "Text" };

typedef struct { int kind; int dec; int thousands; int red_neg; } numfmt_t;

static int str_has_ci(const char *s, const char *needle) {
    int n = (int)strlen(needle);
    for (; *s; s++) if (!strncasecmp(s, needle, n)) return 1;
    return 0;
}

static void numfmt_parse(const char *code, numfmt_t *f) {
    memset(f, 0, sizeof(*f));
    f->dec = -1;
    if (!code || !*code || !strcasecmp(code, "General")) { f->kind = NF_GENERAL; return; }
    if (strchr(code, '@')) { f->kind = NF_TEXT; return; }
    if (str_has_ci(code, "yy") || str_has_ci(code, "dd") || str_has_ci(code, "d/") || str_has_ci(code, "mmm")) { f->kind = NF_DATE; return; }
    if (strchr(code, '%')) f->kind = NF_PERCENT;
    else if (strchr(code, '$') || str_has_ci(code, "[$")) f->kind = NF_CURRENCY;
    else f->kind = NF_NUMBER;
    const char *dot = strchr(code, '.');
    if (dot) { int d = 0; for (const char *p = dot + 1; *p == '0' || *p == '#'; p++) d++; f->dec = d; }
    else f->dec = 0;
    const char *semi = strchr(code, ';');
    for (const char *p = code; *p && (!semi || p < semi); p++) if (*p == ',') { f->thousands = 1; break; }
    f->red_neg = str_has_ci(code, "[Red]");
}

// Build the code for a (kind, decimals, thousands, red) tuple. NULL = General.
static const char *numfmt_build(int kind, int dec, int thousands, int red, char *out, int cap) {
    char base[48]; int n = 0;
    if (dec < 0) dec = 0;
    if (dec > 10) dec = 10;
    switch (kind) {
        case NF_GENERAL: return 0;
        case NF_TEXT: snprintf(out, cap, "@"); return out;
        case NF_DATE: snprintf(out, cap, "yyyy-mm-dd"); return out;
        default: break;
    }
    if (kind == NF_CURRENCY) base[n++] = '$';
    if (thousands) { memcpy(base + n, "#,##0", 5); n += 5; } else base[n++] = '0';
    if (dec > 0) { base[n++] = '.'; for (int i = 0; i < dec; i++) base[n++] = '0'; }
    if (kind == NF_PERCENT) base[n++] = '%';
    base[n] = 0;
    if (red) snprintf(out, cap, "%s;[Red]-%s", base, base);
    else     snprintf(out, cap, "%s", base);
    return out;
}

// Excel serial day -> "yyyy-mm-dd" (days since 1899-12-30; civil_from_days).
static void date_from_serial(double serial, char *out, int cap) {
    long z = (long)serial - 25569;   // -> days since 1970-01-01
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    unsigned long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, cap, "%04ld-%02lu-%02lu", y, m, d);
}

static void general_number(double v, char *out, int cap) {
    double av = v < 0 ? -v : v;
    if (v == (double)(long long)v && av < 1e15) snprintf(out, cap, "%lld", (long long)v);
    else snprintf(out, cap, "%.6g", v);
}

static void group_thousands(const char *src, char *dst, int cap) {
    int len = (int)strlen(src), dot = len;
    for (int i = 0; i < len; i++) if (src[i] == '.') { dot = i; break; }
    int n = 0;
    for (int i = 0; i < len && n < cap - 2; i++) {
        if (i > 0 && i < dot && (dot - i) % 3 == 0) dst[n++] = ',';
        dst[n++] = src[i];
    }
    dst[n] = 0;
}

// Format a numeric value under `f`. *red set when the value is negative and
// the format asks for red negatives.
static void numfmt_value(const numfmt_t *f, double v, char *out, int cap, int *red) {
    if (red) *red = 0;
    switch (f->kind) {
        case NF_GENERAL: case NF_TEXT: general_number(v, out, cap); return;
        case NF_DATE: date_from_serial(v, out, cap); return;
        default: break;
    }
    int dec = f->dec < 0 ? 2 : f->dec;
    if (f->kind == NF_PERCENT) v *= 100.0;
    int neg = v < 0;
    double av = neg ? -v : v;
    char num[64], grouped[80];
    snprintf(num, sizeof(num), "%.*f", dec, av);
    if (f->thousands) group_thousands(num, grouped, sizeof(grouped));
    else { strncpy(grouped, num, sizeof(grouped) - 1); grouped[sizeof(grouped) - 1] = 0; }
    snprintf(out, cap, "%s%s%s%s", neg ? "-" : "", f->kind == NF_CURRENCY ? "$" : "", grouped,
             f->kind == NF_PERCENT ? "%" : "");
    if (red && neg && f->red_neg) *red = 1;
}

// Display text for a cell (right-aligned numeric types, left-aligned text),
// honouring the cell's number format.
static void format_cell_display(const cell *c, char *out, int cap, int *is_numeric, int *red) {
    if (is_numeric) *is_numeric = 0;
    if (red) *red = 0;
    if (!c || c->type == CELL_EMPTY) { out[0] = 0; return; }
    switch (c->type) {
        case CELL_STR: snprintf(out, cap, "%s", c->str ? c->str : ""); break;
        case CELL_ERR: snprintf(out, cap, "%s", c->str ? c->str : "#ERR"); break;
        case CELL_BOOL: snprintf(out, cap, "%s", c->num != 0.0 ? "TRUE" : "FALSE"); break;
        case CELL_NUM:
        case CELL_FORMULA: {
            numfmt_t f; const wb_style *st = cell_style(c);
            numfmt_parse(st ? st->numfmt_code : 0, &f);
            numfmt_value(&f, c->num, out, cap, red);
            if (is_numeric) *is_numeric = 1;
            break;
        }
        default: out[0] = 0; break;
    }
}

// Editable "source" text for the formula bar: a formula shows "=<src>", a
// number shows its unformatted value, text shows itself (so re-committing an
// unedited cell is a no-op round trip).
static void cell_source_text(const cell *c, char *out, int cap) {
    if (!c || c->type == CELL_EMPTY) { out[0] = 0; return; }
    if (c->type == CELL_FORMULA) { snprintf(out, cap, "=%s", c->formula ? c->formula : ""); return; }
    if (c->type == CELL_NUM) { general_number(c->num, out, cap); return; }
    format_cell_display(c, out, cap, 0, 0);
}

// ---------------------------------------------------------------------------
// Undo / redo: a bounded stack of single-cell snapshots (content + style id).
// Every mutation path (commit, clear, style change) captures the cell BEFORE
// touching it. Sheets are only ever appended, so a record's sheet index stays
// valid for the life of the workbook; the stacks are cleared on New/Open.
// ---------------------------------------------------------------------------
typedef struct { int sheet, row, col; int type; double num; char *str, *formula; int style_id; } urec_t;
static urec_t g_undo[UNDO_MAX]; static int g_undo_n;
static urec_t g_redo[UNDO_MAX]; static int g_redo_n;

static void urec_free(urec_t *r) { free(r->str); free(r->formula); r->str = 0; r->formula = 0; }

static void urec_capture(urec_t *r, int sh, int row, int col) {
    memset(r, 0, sizeof(*r));
    r->sheet = sh; r->row = row; r->col = col; r->type = CELL_EMPTY; r->style_id = -1;
    const cell *c = (g_wb && sh >= 0 && sh < g_wb->nsheet) ? peek_cell(&g_wb->sheets[sh], row, col) : 0;
    if (c) {
        r->type = c->type; r->num = c->num; r->style_id = c->style_id;
        r->str = c->str ? strdup(c->str) : 0;
        r->formula = c->formula ? strdup(c->formula) : 0;
    }
}

static void urec_apply(const urec_t *r) {
    if (!g_wb || r->sheet < 0 || r->sheet >= g_wb->nsheet) return;
    cell *c = sheet_cell(&g_wb->sheets[r->sheet], r->row, r->col);
    if (!c) return;
    free(c->str); free(c->formula);
    c->type = (cell_type)r->type; c->num = r->num; c->style_id = r->style_id;
    c->str = r->str ? strdup(r->str) : 0;
    c->formula = r->formula ? strdup(r->formula) : 0;
}

static void stack_push(urec_t *st, int *n, const urec_t *r) {
    if (*n == UNDO_MAX) { urec_free(&st[0]); memmove(st, st + 1, (UNDO_MAX - 1) * sizeof(*st)); (*n)--; }
    st[(*n)++] = *r;
}

static void stack_clear(urec_t *st, int *n) { for (int i = 0; i < *n; i++) urec_free(&st[i]); *n = 0; }

static void push_undo(int row, int col) {
    urec_t r; urec_capture(&r, g_cur_sheet, row, col);
    stack_push(g_undo, &g_undo_n, &r);
    stack_clear(g_redo, &g_redo_n);
}

// ---------------------------------------------------------------------------
// Chrome sync: formula bar, toolbar toggles, menus and status follow the
// active cell. Called after every selection move and every mutation.
// ---------------------------------------------------------------------------
static void sync_selection(void) {
    sheet *s = cur_sheet();
    const cell *c = s ? peek_cell(s, g_sel_row, g_sel_col) : 0;
    char ref[16]; rc_to_a1(g_sel_row, g_sel_col, ref, sizeof(ref));
    char src[512]; cell_source_text(c, src, sizeof(src));
    if (g_editing) ofxbar_set(&g_fx, ref, g_edit_tf.buf);
    else if (g_fx.focus != OFX_FOCUS_FX) ofxbar_set(&g_fx, ref, src);
    else ofxbar_set(&g_fx, ref, 0);
    const wb_style *st = cell_style(c);
    numfmt_t f; numfmt_parse(st ? st->numfmt_code : 0, &f);
    otb_set_checked(&g_tb, T_BOLD,   st && st->bold);
    otb_set_checked(&g_tb, T_ITALIC, st && st->italic);
    otb_set_checked(&g_tb, T_FMTNUM, f.kind == NF_NUMBER);
    otb_set_checked(&g_tb, T_FMTCUR, f.kind == NF_CURRENCY);
    otb_set_checked(&g_tb, T_FMTPCT, f.kind == NF_PERCENT);
    otb_set_enabled(&g_tb, T_UNDO, g_undo_n > 0);
    otb_set_enabled(&g_tb, T_REDO, g_redo_n > 0);
    EDIT_ITEMS[0].enabled = g_undo_n > 0;
    EDIT_ITEMS[1].enabled = g_redo_n > 0;
    char msg[400];
    snprintf(msg, sizeof(msg), "%s    %s    %s%s",
             (s && s->name) ? s->name : "Sheet1", ref,
             g_has_path ? g_path : "Untitled", g_readonly ? "  (read-only)" : "");
    oapp_set_status(g_app, msg);
}

static void set_view_bounds(void) {
    sheet *s = cur_sheet();
    int nr = s ? s->nrows : 0, nc = s ? s->ncols : 0;
    g_max_row = nr + 50; if (g_max_row < 200) g_max_row = 200;
    g_max_col = nc + 20; if (g_max_col < 60) g_max_col = 60;
}

static void relayout(void) {
    if (!g_app) return;
    int cx, cy, cw, ch;
    oapp_content_rect(g_app, &cx, &cy, &cw, &ch);
    int ty, th, by, bh;
    oapp_chrome_rects(g_app, &ty, &th, &by, &bh);
    if (th > 0) ofxbar_move(&g_fx, 0, ty, cw);
    otabs_move(&g_tabs, 0, by, cw);
    g_cx = cx; g_cy = cy; g_cw = cw; g_ch = ch;

    g_grid_x = cx + RH_W;
    g_grid_y = cy;
    g_grid_w = cw - RH_W; if (g_grid_w < COL_W) g_grid_w = COL_W;
    int usable_col_w = g_grid_w - GUI_SCROLL_W - 4;
    g_n_vis_cols = usable_col_w / COL_W; if (g_n_vis_cols < 1) g_n_vis_cols = 1;

    g_rows_h = ch - CH_H; if (g_rows_h < ROW_H) g_rows_h = ROW_H;
    g_n_vis_rows = g_rows_h / ROW_H; if (g_n_vis_rows < 1) g_n_vis_rows = 1;

    gui_scroll_config(&g_vscroll, g_grid_x, g_grid_y + CH_H, g_grid_w, g_rows_h,
                      g_max_row * ROW_H, ROW_H);
    g_vscroll.snap = 1;
}

static void update_chrome(void) {
    char title[220];
    snprintf(title, sizeof(title), "SHEETS - %s%s",
            g_has_path ? basename_of(g_path) : "Untitled",
            g_readonly ? " [Read-Only]" : "");
    oapp_set_title(g_app, title);
    sync_selection();
}

static void reset_view_state(void) {
    g_cur_sheet = 0; g_sel_row = 0; g_sel_col = 0; g_col_off = 0; g_editing = 0;
    ofxbar_blur(&g_fx);
    g_tabs.first_visible = 0;
    stack_clear(g_undo, &g_undo_n);
    stack_clear(g_redo, &g_redo_n);
    oapp_set_dirty(g_app, 0);
}

static void new_workbook(void) {
    if (g_wb) { wb_free(g_wb); g_wb = 0; }
    g_wb = wb_new();
    if (g_wb) wb_add_sheet(g_wb, "Sheet1");
    reset_view_state();
    g_has_path = 0; g_readonly = 0; g_path[0] = 0;
    set_view_bounds();
    relayout();
    update_chrome();
}

static void load_workbook(const char *path) {
    workbook *nwb = 0; int ro = 0;
    int rc = office_open_workbook(path, &nwb, &ro);
    if (rc != 0 || !nwb) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Failed to open %s", path);
        oapp_set_status(g_app, msg);
        return;
    }
    if (nwb->nsheet == 0) wb_add_sheet(nwb, "Sheet1");   // defensive: never show a tabless book
    if (g_wb) wb_free(g_wb);
    g_wb = nwb;
    g_readonly = ro;
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    reset_view_state();
    // The importer may hand back cached formula results as-is; recompute so a
    // dependent cell (e.g. a SUM) is guaranteed correct under THIS evaluator,
    // not merely whatever the producing app last cached.
    fx_recalc(g_wb);
    set_view_bounds();
    relayout();
    update_chrome();
}

// Returns 1 on success (the workbook is now clean and bound to `path`).
static int save_workbook(const char *path) {
    if (g_wb) fx_recalc(g_wb);
    unsigned char *out = 0; unsigned long outlen = 0;
    int rc = has_ext_ci(path, ".ods") ? ods_save(g_wb, &out, &outlen)
                                     : xlsx_save(g_wb, &out, &outlen);
    if (rc != 0 || !out) {
        oapp_set_status(g_app, "Save failed: could not serialize workbook");
        if (out) free(out);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        oapp_set_status(g_app, "Save failed: could not write file");
        free(out);
        return 0;
    }
    unsigned long written = 0;
    while (written < outlen) {
        long w = write(fd, out + written, (size_t)(outlen - written));
        if (w <= 0) break;
        written += (unsigned long)w;
    }
    close(fd);
    free(out);
    if (written != outlen) {
        oapp_set_status(g_app, "Save failed: short write");
        return 0;
    }
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    g_readonly = 0;
    oapp_set_dirty(g_app, 0);
    update_chrome();
    return 1;
}

// Save (as = 0 reuses the bound path when writable, else prompts). 1 = saved.
static int do_save(int as) {
    if (!as && g_has_path && !g_readonly) return save_workbook(g_path);
    const char *path = oapp_save_dialog(g_app, g_has_path ? basename_of(g_path) : "Untitled.xlsx");
    return path ? save_workbook(path) : 0;
}

// Unsaved-changes gate before New/Open/Close/window close. 1 = proceed.
static int confirm_discard(void) {
    if (!oapp_is_dirty(g_app)) return 1;
    char body[300];
    snprintf(body, sizeof(body), "Save changes to %s before closing?",
             g_has_path ? basename_of(g_path) : "Untitled.xlsx");
    int r = omodal_confirm3(g_app, "Unsaved changes", body, "Save", "Discard");
    if (r == OM_THIRD) return do_save(0);
    return r == OM_OK;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------
static void start_edit(const char *prefill) {
    g_edit_buf[0] = 0;
    tf_init(&g_edit_tf, g_edit_buf, sizeof(g_edit_buf));
    if (prefill && *prefill) tf_set_text(&g_edit_tf, prefill);
    g_editing = 1;
    sync_selection();
}

// Parse `buf` and store it into the selected cell. The cell* is fetched fresh
// right here and never held past this function (sheet_cell() can grow and
// realloc the sheet's dense cell array, and fx_recalc() re-walks every sheet
// from scratch - neither is safe to call while holding a stale cell*).
static void commit_text(const char *buf) {
    sheet *s = cur_sheet();
    if (s) {
        char cur[512]; cell_source_text(peek_cell(s, g_sel_row, g_sel_col), cur, sizeof(cur));
        if (!strcmp(cur, buf)) return;                 // unchanged: no undo record, no dirty
        push_undo(g_sel_row, g_sel_col);
        cell *c = sheet_cell(s, g_sel_row, g_sel_col);
        if (c) {
            free(c->str);     c->str = 0;
            free(c->formula); c->formula = 0;
            if (buf[0] == '=' && buf[1] != 0) {
                c->formula = strdup(buf + 1);
                c->type = CELL_FORMULA;
                c->num = 0.0;
            } else if (buf[0] == 0) {
                c->type = CELL_EMPTY;
                c->num = 0.0;
            } else {
                double v;
                if (try_parse_number(buf, &v)) { c->type = CELL_NUM; c->num = v; }
                else { c->str = strdup(buf); c->type = CELL_STR; }
            }
        }
        oapp_set_dirty(g_app, 1);
    }
    if (g_wb) fx_recalc(g_wb);
    set_view_bounds();
    relayout();
}

static void commit_edit(void) {
    if (!g_editing) return;
    g_editing = 0;
    commit_text(g_edit_tf.buf);
    sync_selection();
}

static void cancel_edit(void) { g_editing = 0; sync_selection(); }

static void clear_selected_cell(void) {
    if (g_editing) { g_editing = 0; }
    commit_text("");
    sync_selection();
}

static void move_selection(int dr, int dc) {
    if (g_editing) commit_edit();
    int nr = g_sel_row + dr, nc = g_sel_col + dc;
    if (nr < 0) nr = 0;
    if (nr >= g_max_row) nr = g_max_row - 1;
    if (nc < 0) nc = 0;
    if (nc >= g_max_col) nc = g_max_col - 1;
    g_sel_row = nr; g_sel_col = nc;
    gui_scroll_reveal(&g_vscroll, g_sel_row * ROW_H, ROW_H);
    if (g_sel_col < g_col_off) g_col_off = g_sel_col;
    else if (g_sel_col >= g_col_off + g_n_vis_cols) g_col_off = g_sel_col - g_n_vis_cols + 1;
    if (g_col_off < 0) g_col_off = 0;
    sync_selection();
}

static void select_cell(int row, int col) { move_selection(row - g_sel_row, col - g_sel_col); }

static void switch_sheet(int idx) {
    if (!g_wb || idx < 0 || idx >= g_wb->nsheet || idx == g_cur_sheet) return;
    if (g_editing) commit_edit();
    g_cur_sheet = idx;
    g_sel_row = 0; g_sel_col = 0; g_col_off = 0;
    gui_scroll_reveal(&g_vscroll, 0, ROW_H);
    set_view_bounds();
    relayout();
    sync_selection();
}

static void add_sheet(void) {
    if (!g_wb || g_wb->nsheet >= OTAB_MAX) return;
    if (g_editing) commit_edit();
    char name[32];
    for (int n = g_wb->nsheet + 1; ; n++) {
        snprintf(name, sizeof(name), "Sheet%d", n);
        int taken = 0;
        for (int i = 0; i < g_wb->nsheet; i++) if (g_wb->sheets[i].name && !strcmp(g_wb->sheets[i].name, name)) taken = 1;
        if (!taken) break;
    }
    if (!wb_add_sheet(g_wb, name)) return;
    oapp_set_dirty(g_app, 1);
    switch_sheet(g_wb->nsheet - 1);
}

// Sheet names: 1..31 chars, none of [ ] : * ? / \ (spec 5.2.3).
static int sheet_name_ok(const char *s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\') return 0;
    }
    return n >= 1 && n <= 31;
}

static void do_undo(void) {
    if (!g_undo_n) return;
    if (g_editing) cancel_edit();
    urec_t r = g_undo[--g_undo_n];
    urec_t cur; urec_capture(&cur, r.sheet, r.row, r.col);
    stack_push(g_redo, &g_redo_n, &cur);
    urec_apply(&r);
    urec_free(&r);
    if (r.sheet != g_cur_sheet) { g_cur_sheet = r.sheet; set_view_bounds(); relayout(); }
    if (g_wb) fx_recalc(g_wb);
    oapp_set_dirty(g_app, 1);
    select_cell(r.row, r.col);
}

static void do_redo(void) {
    if (!g_redo_n) return;
    if (g_editing) cancel_edit();
    urec_t r = g_redo[--g_redo_n];
    urec_t cur; urec_capture(&cur, r.sheet, r.row, r.col);
    stack_push(g_undo, &g_undo_n, &cur);
    urec_apply(&r);
    urec_free(&r);
    if (r.sheet != g_cur_sheet) { g_cur_sheet = r.sheet; set_view_bounds(); relayout(); }
    if (g_wb) fx_recalc(g_wb);
    oapp_set_dirty(g_app, 1);
    select_cell(r.row, r.col);
}

// Style mutation on the active cell: copy its style (or a blank one), let
// `mut` edit the copy, register it as a new workbook style and point the cell
// at it. The copy is taken BEFORE wb_add_style (which reallocs the styles
// array) and the cell* is re-fetched after it.
static void apply_style(void (*mut)(wb_style *st, void *arg), void *arg) {
    sheet *s = cur_sheet();
    if (!s || !g_wb) return;
    if (g_editing) commit_edit();
    push_undo(g_sel_row, g_sel_col);
    wb_style st; memset(&st, 0, sizeof(st));
    { const wb_style *cs = cell_style(peek_cell(s, g_sel_row, g_sel_col)); if (cs) st = *cs; }
    mut(&st, arg);
    int id = wb_add_style(g_wb, &st);
    cell *c = sheet_cell(s, g_sel_row, g_sel_col);
    if (c && id >= 0) c->style_id = id;
    oapp_set_dirty(g_app, 1);
    sync_selection();
}

static void mut_bold(wb_style *st, void *arg)   { st->bold = *(int *)arg; }
static void mut_italic(wb_style *st, void *arg) { st->italic = *(int *)arg; }
typedef struct { int kind, dec, thousands, red, bold, italic; unsigned int color; int set_font; } fmt_args_t;
static char g_code_buf[96];
static void mut_numfmt(wb_style *st, void *arg) {
    const fmt_args_t *a = (const fmt_args_t *)arg;
    st->numfmt_code = (char *)numfmt_build(a->kind, a->dec, a->thousands, a->red, g_code_buf, sizeof(g_code_buf));
    if (a->set_font) { st->bold = a->bold; st->italic = a->italic; st->color = a->color; }
}

// Toolbar/menu Number / Currency / Percent: keep the cell's decimals when it
// already has a numeric format, else the family default.
static void apply_format_kind(int kind) {
    sheet *s = cur_sheet();
    const wb_style *cs = s ? cell_style(peek_cell(s, g_sel_row, g_sel_col)) : 0;
    numfmt_t cur; numfmt_parse(cs ? cs->numfmt_code : 0, &cur);
    fmt_args_t a; memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.dec = cur.dec >= 0 ? cur.dec : 2;
    a.thousands = (kind == NF_CURRENCY) ? 1 : cur.thousands;
    a.red = cur.red_neg;
    apply_style(mut_numfmt, &a);
}

static void toggle_bold(void) {
    sheet *s = cur_sheet();
    const wb_style *cs = s ? cell_style(peek_cell(s, g_sel_row, g_sel_col)) : 0;
    int v = !(cs && cs->bold);
    apply_style(mut_bold, &v);
}
static void toggle_italic(void) {
    sheet *s = cur_sheet();
    const wb_style *cs = s ? cell_style(peek_cell(s, g_sel_row, g_sel_col)) : 0;
    int v = !(cs && cs->italic);
    apply_style(mut_italic, &v);
}

// AutoSum: the numeric run directly above the active cell, else the run to
// its left, else an "=SUM(" skeleton to complete by hand.
static void autosum(void) {
    sheet *s = cur_sheet();
    if (!s) return;
    if (g_editing) commit_edit();
    int r0 = -1;
    for (int r = g_sel_row - 1; r >= 0; r--) {
        const cell *c = peek_cell(s, r, g_sel_col);
        if (c && (c->type == CELL_NUM || c->type == CELL_FORMULA)) r0 = r; else break;
    }
    char a[16], b[16], f[64];
    if (r0 >= 0) {
        rc_to_a1(r0, g_sel_col, a, sizeof(a)); rc_to_a1(g_sel_row - 1, g_sel_col, b, sizeof(b));
        snprintf(f, sizeof(f), "=SUM(%s:%s)", a, b);
        commit_text(f); sync_selection(); return;
    }
    int c0 = -1;
    for (int cc = g_sel_col - 1; cc >= 0; cc--) {
        const cell *c = peek_cell(s, g_sel_row, cc);
        if (c && (c->type == CELL_NUM || c->type == CELL_FORMULA)) c0 = cc; else break;
    }
    if (c0 >= 0) {
        rc_to_a1(g_sel_row, c0, a, sizeof(a)); rc_to_a1(g_sel_row, g_sel_col - 1, b, sizeof(b));
        snprintf(f, sizeof(f), "=SUM(%s:%s)", a, b);
        commit_text(f); sync_selection(); return;
    }
    start_edit("=SUM(");
}

// ---------------------------------------------------------------------------
// Modals (omodal forms, spec 5.2)
// ---------------------------------------------------------------------------
static const char *FC_COLOR_NAMES[] = { "Black", "Gray", "Red", "Green", "Blue", "Orange", "Purple", "White" };
static const unsigned int FC_COLOR_RGB[] = { 0x000000, 0x808080, 0xC00000, 0x008000, 0x0000C0, 0xE07000, 0x700090, 0xFFFFFF };
#define N_FC_COLORS 8

typedef struct { int cat, thousands, red, bold, italic, color; char dec[8]; } fc_state_t;
static fc_state_t g_fc;

static int fc_color_index(unsigned int rgb) {
    for (int i = 0; i < N_FC_COLORS; i++) if (FC_COLOR_RGB[i] == (rgb & 0xFFFFFF)) return i;
    return 0;
}

// Cell ink: the style colour (0 = automatic) floored 4.5:1 against the paper.
static uint32_t cell_ink_for(unsigned int color, uint32_t paper, uint32_t fallback) {
    uint32_t ink = color ? (uint32_t)(color & 0xFFFFFF) : fallback;
    return gui_ensure_contrast(ink, paper, GUI_FLOOR_TEXT);
}

static void fc_preview(int win, int x, int y, int w, int h, void *ctx) {
    (void)ctx; (void)w;
    numfmt_t f; char code[96];
    numfmt_parse(numfmt_build(g_fc.cat, atoi(g_fc.dec), g_fc.thousands, g_fc.red, code, sizeof(code)), &f);
    uint32_t paper = tok(THEME_COLOR_TEXTBOX_BG, 0x00FFFFFF);
    uint32_t ink   = cell_ink_for(g_fc.color >= 0 ? FC_COLOR_RGB[g_fc.color] : 0, paper, tok(THEME_COLOR_TEXTBOX_TEXT, gui_ink_on(paper)));
    uint32_t red   = gui_ensure_contrast(tok(THEME_COLOR_DANGER, 0x00C00000), paper, GUI_FLOOR_TEXT);
    int style = g_face_style | (g_fc.bold ? FONT_STYLE_BOLD : 0) | (g_fc.italic ? FONT_STYLE_ITALIC : 0);
    char pos[64], neg[64]; int isred = 0;
    numfmt_value(&f, 1234.5, pos, sizeof(pos), 0);
    numfmt_value(&f, -1234.5, neg, sizeof(neg), &isred);
    if (f.kind == NF_DATE) { numfmt_value(&f, 45000.0, pos, sizeof(pos), 0); neg[0] = 0; }
    int size = 14, ty = y + (h - size) / 2;
    win_draw_text_ttf_ex(win, x + 12, ty, pos, g_face, size, style, ink);
    if (neg[0]) win_draw_text_ttf_ex(win, x + 12 + gui_ttf_render_width(pos, size) + 24, ty, neg, g_face, size, style, isred ? red : ink);
}

static void format_cells_modal(void) {
    sheet *s = cur_sheet();
    if (!s) return;
    if (g_editing) commit_edit();
    const wb_style *cs = cell_style(peek_cell(s, g_sel_row, g_sel_col));
    numfmt_t cur; numfmt_parse(cs ? cs->numfmt_code : 0, &cur);
    memset(&g_fc, 0, sizeof(g_fc));
    g_fc.cat = cur.kind; g_fc.thousands = cur.thousands; g_fc.red = cur.red_neg;
    snprintf(g_fc.dec, sizeof(g_fc.dec), "%d", cur.dec >= 0 ? cur.dec : 2);
    g_fc.bold = cs ? cs->bold : 0; g_fc.italic = cs ? cs->italic : 0;
    g_fc.color = cs ? fc_color_index(cs->color) : 0;
    om_field_t f[] = {
        { OM_LIST,   "Category",            0, 0, 0, 0,  NF_CAT_NAMES, 6, &g_fc.cat, 0, 6, 0 },
        { OM_NUMBER, "Decimal places",      g_fc.dec, sizeof(g_fc.dec), 0, 10, 0, 0, 0, 0, 0, 0 },
        { OM_CHECK,  "Thousands separator", 0, 0, 0, 0, 0, 0, 0, &g_fc.thousands, 0, 0 },
        { OM_CHECK,  "Negative in red",     0, 0, 0, 0, 0, 0, 0, &g_fc.red, 0, 0 },
        { OM_CHECK,  "Bold",                0, 0, 0, 0, 0, 0, 0, &g_fc.bold, 0, 1 },
        { OM_CHECK,  "Italic",              0, 0, 0, 0, 0, 0, 0, &g_fc.italic, 0, 0 },
        { OM_COMBO,  "Text colour",         0, 0, 0, 0, FC_COLOR_NAMES, N_FC_COLORS, &g_fc.color, 0, 0, 0 },
    };
    om_spec_t spec = { "Format Cells", OM_SIZE_M, f, 7, 0, 0, 0, 0, 40, fc_preview, 0, 0 };
    if (omodal_run(g_app, &spec) != OM_OK) return;
    fmt_args_t a; memset(&a, 0, sizeof(a));
    a.kind = g_fc.cat; a.dec = atoi(g_fc.dec); a.thousands = g_fc.thousands; a.red = g_fc.red;
    a.set_font = 1; a.bold = g_fc.bold; a.italic = g_fc.italic;
    a.color = (g_fc.color >= 0 && g_fc.color < N_FC_COLORS) ? FC_COLOR_RGB[g_fc.color] : 0;
    apply_style(mut_numfmt, &a);
}

// Insert Function: the v1 evaluator set (formula.h scope).
static const char *FN_NAMES[] = { "SUM", "AVERAGE", "MIN", "MAX", "COUNT", "IF", "ABS", "ROUND" };
static const char *FN_SIGS[]  = {
    "SUM(range, ...)", "AVERAGE(range, ...)", "MIN(range, ...)", "MAX(range, ...)",
    "COUNT(range, ...)", "IF(condition, then, else)", "ABS(number)", "ROUND(number, digits)" };
static const char *FN_DESCS[] = {
    "Adds all the numbers in the ranges.", "Arithmetic mean of the numbers in the ranges.",
    "Smallest number in the ranges.", "Largest number in the ranges.",
    "How many cells in the ranges hold numbers.", "Returns then when condition is true, else else.",
    "Absolute value of a number.", "Rounds a number to the given number of digits." };
static const int FN_CAT[] = { 1, 2, 2, 2, 2, 3, 1, 1 };
static const char *FN_CATS[] = { "All", "Math", "Statistics", "Logical" };
#define N_FN 8
static const char *g_fn_opts[N_FN]; static int g_fn_map[N_FN];
static int g_fn_cat, g_fn_sel;
static char g_fn_sig[64], g_fn_desc[96], g_fn_prev[32];
static om_field_t g_fn_fields[5];

// on_change: re-filter the list by category and refresh the description rows
// (the list field's n_options is edited in place; the modal re-reads it).
static void fn_refilter(void *ctx) {
    (void)ctx;
    int n = 0;
    for (int i = 0; i < N_FN; i++) if (g_fn_cat == 0 || FN_CAT[i] == g_fn_cat) { g_fn_opts[n] = FN_NAMES[i]; g_fn_map[n] = i; n++; }
    g_fn_fields[1].n_options = n;
    if (g_fn_sel < 0 || g_fn_sel >= n) g_fn_sel = 0;
    int k = g_fn_map[g_fn_sel];
    snprintf(g_fn_sig, sizeof(g_fn_sig), "%s", FN_SIGS[k]);
    snprintf(g_fn_desc, sizeof(g_fn_desc), "%s", FN_DESCS[k]);
    snprintf(g_fn_prev, sizeof(g_fn_prev), "=%s(", FN_NAMES[k]);
}

static void insert_text_at_caret(textfield_t *tf, const char *s) { for (; *s; s++) tf_insert(tf, *s); }

static void insert_function_modal(void) {
    g_fn_cat = 0; g_fn_sel = 0;
    om_field_t f[] = {
        { OM_COMBO, "Category",    0, 0, 0, 0, FN_CATS, 4, &g_fn_cat, 0, 0, 0 },
        { OM_LIST,  "Function",    0, 0, 0, 0, g_fn_opts, N_FN, &g_fn_sel, 0, 8, 0 },
        { OM_LABEL, "Signature",   g_fn_sig,  sizeof(g_fn_sig),  0, 0, 0, 0, 0, 0, 0, 0 },
        { OM_LABEL, "Description", g_fn_desc, sizeof(g_fn_desc), 0, 0, 0, 0, 0, 0, 0, 0 },
        { OM_LABEL, "Inserts",     g_fn_prev, sizeof(g_fn_prev), 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    memcpy(g_fn_fields, f, sizeof(f));
    fn_refilter(0);
    om_spec_t spec = { "Insert Function", OM_SIZE_L, g_fn_fields, 5, "Insert", 0, 0, 0, 0, 0, fn_refilter, 0 };
    if (omodal_run(g_app, &spec) != OM_OK) return;
    fn_refilter(0);
    const char *name = FN_NAMES[g_fn_map[g_fn_sel]];
    char skel[40]; snprintf(skel, sizeof(skel), "%s(", name);
    if (g_fx.focus == OFX_FOCUS_FX) {
        if (g_fx.fx_tf.len == 0) tf_insert(&g_fx.fx_tf, '=');
        insert_text_at_caret(&g_fx.fx_tf, skel);
        return;
    }
    if (g_editing) {
        if (g_edit_tf.len == 0) tf_insert(&g_edit_tf, '=');
        insert_text_at_caret(&g_edit_tf, skel);
        sync_selection();
        return;
    }
    char full[48]; snprintf(full, sizeof(full), "=%s(", name);
    start_edit(full);
}

static void rename_sheet_modal(int idx) {
    if (!g_wb || idx < 0 || idx >= g_wb->nsheet) return;
    if (g_editing) commit_edit();
    static char name[OTAB_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_wb->sheets[idx].name ? g_wb->sheets[idx].name : "");
    om_field_t f[] = { { OM_TEXT, "Name", name, sizeof(name), 0, 0, 0, 0, 0, 0, 0, 0 } };
    om_spec_t spec = { "Rename Sheet", OM_SIZE_S, f, 1, "Rename", 0, 0, 0, 0, 0, 0, 0 };
    if (omodal_run(g_app, &spec) != OM_OK) return;
    if (!sheet_name_ok(name)) { oapp_set_status(g_app, "Sheet name must be 1..31 characters without [ ] : * ? / \\"); return; }
    if (g_wb->sheets[idx].name && !strcmp(g_wb->sheets[idx].name, name)) return;
    free(g_wb->sheets[idx].name);
    g_wb->sheets[idx].name = strdup(name);
    oapp_set_dirty(g_app, 1);
    sync_selection();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static const char *tab_label(void *ctx, int i) {
    (void)ctx;
    if (!g_wb || i < 0 || i >= g_wb->nsheet) return "";
    return g_wb->sheets[i].name ? g_wb->sheets[i].name : "Sheet";
}

static void draw_cell_text(int win, int x, int y, const char *s, int style, uint32_t ink) {
    win_draw_text_ttf_ex(win, x, y, s, g_face, CELL_PT, g_face_style | style, ink);
}

static void draw(office_app *a) {
    int win = oapp_win(a);
    // Grounds (spec 1.1): paper = surface_sunken (TEXTBOX_BG), headers/strips =
    // surface_raised, gridlines = border_subtle floored 3:1 against the paper.
    // Inks are only ever floored against the ground they sit on.
    uint32_t paper  = tok(THEME_COLOR_TEXTBOX_BG, 0x00FFFFFF);
    uint32_t raised = tok(THEME_COLOR_SURFACE_RAISED, 0x00C8C8C8);
    uint32_t ink    = gui_ensure_contrast(tok(THEME_COLOR_TEXTBOX_TEXT, gui_ink_on(paper)), paper, GUI_FLOOR_TEXT);
    uint32_t hdrink = gui_ensure_contrast(tok(THEME_COLOR_MUTED, 0x00808080), raised, GUI_FLOOR_TEXT);
    uint32_t grid   = gui_ensure_contrast(tok(THEME_COLOR_BORDER_SUBTLE, 0x00808080), paper, GUI_FLOOR_NONTEXT);
    uint32_t rule   = gui_ensure_contrast(tok(THEME_COLOR_BORDER_SUBTLE, 0x00808080), raised, GUI_FLOOR_NONTEXT);
    uint32_t accent = tok(THEME_COLOR_ACCENT, 0x00336666);
    uint32_t ring   = gui_ensure_contrast(accent, paper, GUI_FLOOR_NONTEXT);
    uint32_t onacc  = gui_ensure_contrast(tok(THEME_COLOR_ON_ACCENT, 0x00FFFFFF), accent, GUI_FLOOR_TEXT);
    uint32_t errink = gui_ensure_contrast(tok(THEME_COLOR_ERROR, 0x00C00000), paper, GUI_FLOOR_TEXT);
    uint32_t redink = gui_ensure_contrast(tok(THEME_COLOR_DANGER, 0x00C00000), paper, GUI_FLOOR_TEXT);
    int cap = theme_metric_or(THEME_METRIC_TYPE_CAPTION, 11);

    sheet *s = cur_sheet();

    // --- formula bar (the toolkit strip in the top chrome band) ---------------
    if (g_show_fx) {
        if (g_editing) ofxbar_set(&g_fx, 0, g_edit_tf.buf);   // mirror the in-cell edit live
        ofxbar_draw(win, &g_fx);
    }

    // --- column headers ---------------------------------------------------------
    win_draw_rect(win, g_cx, g_grid_y, g_cw, CH_H, raised);
    for (int ci = 0; ci < g_n_vis_cols; ci++) {
        int col = g_col_off + ci;
        if (col >= g_max_col) break;
        int x = g_grid_x + ci * COL_W;
        char cl[8]; col_letters(col, cl, sizeof(cl));
        int selh = (col == g_sel_col);
        if (selh) win_draw_rect(win, x, g_grid_y, COL_W, CH_H, accent);
        gui_text_ttf_centered(win, x, g_grid_y, COL_W, CH_H, cl, selh ? onacc : hdrink, cap);
        win_draw_rect(win, x + COL_W - 1, g_grid_y, 1, CH_H, rule);
    }
    win_draw_rect(win, g_cx, g_grid_y + CH_H - 1, g_cw, 1, rule);

    // --- row headers + cells --------------------------------------------------
    int first_row = gui_scroll_first_item(&g_vscroll);
    int fx_live = (g_show_fx && g_fx.focus == OFX_FOCUS_FX);
    for (int ri = 0; ri < g_n_vis_rows; ri++) {
        int row = first_row + ri;
        if (row >= g_max_row) break;
        int y = g_grid_y + CH_H + ri * ROW_H;

        int selr = (row == g_sel_row);
        win_draw_rect(win, g_cx, y, RH_W, ROW_H, selr ? accent : raised);
        char rn[12]; snprintf(rn, sizeof(rn), "%d", row + 1);
        int rw = gui_ttf_render_width(rn, cap);
        win_draw_text_ttf(win, g_cx + RH_W - 6 - rw, y + (ROW_H - cap) / 2, rn, cap, selr ? onacc : hdrink);
        win_draw_rect(win, g_cx, y + ROW_H - 1, RH_W, 1, rule);
        win_draw_rect(win, g_cx + RH_W - 1, y, 1, ROW_H, rule);

        for (int ci = 0; ci < g_n_vis_cols; ci++) {
            int col = g_col_off + ci;
            if (col >= g_max_col) break;
            int x = g_grid_x + ci * COL_W;
            int selected = (row == g_sel_row && col == g_sel_col);

            win_draw_rect(win, x, y, COL_W, ROW_H, paper);
            if (g_gridlines) {
                win_draw_rect(win, x, y + ROW_H - 1, COL_W, 1, grid);
                win_draw_rect(win, x + COL_W - 1, y, 1, ROW_H, grid);
            }
            if (selected && g_editing) continue;            // the edit field is drawn after the loop
            if (selected && fx_live) {                      // formula-bar edit mirrored in place
                if (g_fx.fx_buf[0]) draw_cell_text(win, x + 4, y + 4, g_fx.fx_buf, 0, ink);
                continue;
            }
            const cell *c = s ? peek_cell(s, row, col) : 0;
            char disp[128]; int isnum = 0, isred = 0;
            format_cell_display(c, disp, sizeof(disp), &isnum, &isred);
            if (!disp[0]) continue;
            const wb_style *st = cell_style(c);
            int style = (st && st->bold ? FONT_STYLE_BOLD : 0) | (st && st->italic ? FONT_STYLE_ITALIC : 0);
            uint32_t tc = (c && c->type == CELL_ERR) ? errink : (isred ? redink : cell_ink_for(st ? st->color : 0, paper, ink));
            if (isnum) {
                int tw = gui_ttf_render_width(disp, CELL_PT);
                draw_cell_text(win, x + COL_W - 6 - tw, y + 4, disp, style, tc);
            } else {
                draw_cell_text(win, x + 4, y + 4, disp, style, tc);
            }
        }
    }
    // Selected-cell ring (2 px accent, spec 2.2) + fill handle, drawn last so
    // they read over every gridline; the in-cell edit field on top of that.
    {
        int ri = g_sel_row - first_row, ci = g_sel_col - g_col_off;
        if (ri >= 0 && ri < g_n_vis_rows && ci >= 0 && ci < g_n_vis_cols) {
            int x = g_grid_x + ci * COL_W, y = g_grid_y + CH_H + ri * ROW_H;
            if (g_editing) {
                int ew = gui_ttf_render_width(g_edit_tf.buf, CELL_PT) + 24;
                if (ew < COL_W) ew = COL_W;
                int maxw = g_grid_x + g_n_vis_cols * COL_W - x;
                if (ew > maxw) ew = maxw;
                gui_textfield_tf(win, x, y, ew, ROW_H, g_edit_tf.buf, g_edit_tf.len, g_edit_tf.cursor,
                                 g_edit_tf.sel_anchor, 1, 0);
            } else {
                win_draw_rect(win, x - 1, y - 1, COL_W + 1, 2, ring);
                win_draw_rect(win, x - 1, y - 1, 2, ROW_H + 1, ring);
                win_draw_rect(win, x - 1, y + ROW_H - 2, COL_W + 1, 2, ring);
                win_draw_rect(win, x + COL_W - 2, y - 1, 2, ROW_H + 1, ring);
                win_draw_rect(win, x + COL_W - 4, y + ROW_H - 4, 5, 5, ring);
            }
        }
    }
    gui_scroll_draw(win, &g_vscroll);

    // --- sheet tabs (the toolkit strip in the bottom chrome band) -------------
    otabs_layout(&g_tabs, g_wb ? g_wb->nsheet : 0, g_cur_sheet, tab_label, 0);
    otabs_draw(win, &g_tabs, tab_label, 0);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
// The formula field's text becomes the cell (Enter/Tab/click elsewhere).
static void fx_commit(void) {
    commit_text(g_fx.fx_buf);
    ofxbar_blur(&g_fx);
    sync_selection();
}

// A toolbar/menu action while the formula field holds an uncommitted edit
// commits it first (the shell routes toolbar clicks before the app sees a
// mouse-down, so handle_mouse_down's own commit never ran). Insert Function
// is the one exception: it inserts at that field's caret.
static void settle_fx_edit(void) {
    if (g_show_fx && g_fx.focus == OFX_FOCUS_FX) fx_commit();
}

// Name box: "B4" (or "$C$5") jumps; anything else is restored.
static void name_box_jump(void) {
    int row, col;
    if (a1_to_rc(g_fx.name_buf, &row, &col) == 0 && row >= 0 && col >= 0) {
        if (row >= g_max_row) g_max_row = row + 50;
        if (col >= g_max_col) g_max_col = col + 20;
        relayout();
        select_cell(row, col);
    } else sync_selection();
}

static void handle_mouse_down(int mx, int my) {
    if (g_show_fx) {
        int r = ofxbar_press(&g_fx, mx, my);
        if (r == OFX_FOCUS_FX) {
            // Continue an in-cell edit in the bar: hand the buffer over.
            if (g_editing) { ofxbar_set(&g_fx, 0, g_edit_tf.buf); g_editing = 0; }
            return;
        }
        if (r == OFX_FOCUS_NAME) { tf_select_all(&g_fx.name_tf); return; }
        if (r == OFX_PRESS_FXBTN) return;
    }
    if (g_fx.focus == OFX_FOCUS_FX) fx_commit();
    else if (g_fx.focus == OFX_FOCUS_NAME) { ofxbar_blur(&g_fx); sync_selection(); }

    int t = otabs_press(&g_tabs, mx, my);
    if (t >= 0) {
        unsigned long now = uptime_ms();
        if (t == g_last_tab && now - g_last_tab_ms < DBLCLICK_MS) {
            g_last_tab = -1; g_last_tab_ms = 0;
            rename_sheet_modal(t);
        } else {
            g_last_tab = t; g_last_tab_ms = now;
            switch_sheet(t);
        }
        return;
    }
    if (t == -2) return;

    if (gui_scroll_press(&g_vscroll, mx, my)) return;

    if (mx >= g_grid_x && my >= g_grid_y + CH_H && my < g_grid_y + CH_H + g_rows_h) {
        int ri = (my - (g_grid_y + CH_H)) / ROW_H;
        int row = gui_scroll_first_item(&g_vscroll) + ri;
        int ci = (mx - g_grid_x) / COL_W;
        int col = g_col_off + ci;
        if (row >= 0 && row < g_max_row && col >= 0 && col < g_max_col) select_cell(row, col);
    }
}

static void handle_key(uint32_t kc, char c) {
    if (g_editing) {
        if (c == GUI_KEY_ESC) { cancel_edit(); return; }
        if (c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) { commit_edit(); move_selection(1, 0); return; }
        if (kc == GUI_KEY_TAB || c == GUI_KEY_TAB) { commit_edit(); move_selection(0, 1); return; }
        gui_event_t fake; memset(&fake, 0, sizeof(fake));
        fake.type = EVENT_KEY_DOWN; fake.keycode = kc; fake.key_char = c;
        tf_handle_key(&g_edit_tf, &fake);
        return;
    }

    if (kc == GUI_KEY_UP)   { move_selection(-1, 0); return; }
    if (kc == GUI_KEY_DOWN) { move_selection(1, 0);  return; }
    if (kc == GUI_KEY_LEFT) { move_selection(0, -1); return; }
    if (kc == GUI_KEY_RIGHT){ move_selection(0, 1);  return; }
    if (kc == GUI_KEY_PGUP) { move_selection(-g_n_vis_rows, 0); return; }
    if (kc == GUI_KEY_PGDN) { move_selection(g_n_vis_rows, 0);  return; }
    if (kc == GUI_KEY_HOME) { move_selection(0, -g_sel_col); return; }
    if (kc == GUI_KEY_END)  {
        sheet *s = cur_sheet();
        int lastcol = (s && s->ncols > 0) ? s->ncols - 1 : 0;
        move_selection(0, lastcol - g_sel_col);
        return;
    }
    if (kc == GUI_KEY_DEL || c == GUI_KEY_BKSP) { clear_selected_cell(); return; }
    if (kc == GUI_KEY_F2 || c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) {
        sheet *s = cur_sheet();
        const cell *pc = s ? peek_cell(s, g_sel_row, g_sel_col) : 0;
        char src[512]; cell_source_text(pc, src, sizeof(src));
        start_edit(src);
        return;
    }
    if (c >= 0x20 && c <= 0x7E) {
        char one[2] = { c, 0 };
        start_edit(one);
        return;
    }
}

static int on_tool(int id, int value) {
    if (id != T_FX) settle_fx_edit();
    switch (id) {
        case T_NEW:    if (confirm_discard()) new_workbook(); break;
        case T_OPEN: {
            if (!confirm_discard()) break;
            const char *path = oapp_open_dialog(g_app, ".xlsx.ods.xls");
            if (path) load_workbook(path);
            break;
        }
        case T_SAVE:   do_save(0); break;
        case T_UNDO:   do_undo(); break;
        case T_REDO:   do_redo(); break;
        case T_BOLD:   { int v = value ? 1 : 0; apply_style(mut_bold, &v); break; }
        case T_ITALIC: { int v = value ? 1 : 0; apply_style(mut_italic, &v); break; }
        case T_FMTNUM: apply_format_kind(NF_NUMBER); break;
        case T_FMTCUR: apply_format_kind(NF_CURRENCY); break;
        case T_FMTPCT: apply_format_kind(NF_PERCENT); break;
        case T_SUM:    autosum(); break;
        case T_FX:     insert_function_modal(); break;
        default: break;   // T_BORDERS is disabled (see TB_ITEMS)
    }
    return 1;
}

static int on_menu(int id) {
    if (id != M_INS_FUNC) settle_fx_edit();
    switch (id) {
        case M_EDIT_UNDO:   do_undo(); break;
        case M_EDIT_REDO:   do_redo(); break;
        case M_EDIT_DELETE: clear_selected_cell(); break;
        case M_FMT_CELLS:   format_cells_modal(); break;
        case M_FMT_NUM:     apply_format_kind(NF_NUMBER); break;
        case M_FMT_CUR:     apply_format_kind(NF_CURRENCY); break;
        case M_FMT_PCT:     apply_format_kind(NF_PERCENT); break;
        case M_FMT_BOLD:    toggle_bold(); break;
        case M_FMT_ITALIC:  toggle_italic(); break;
        case M_FMT_RENAME:  rename_sheet_modal(g_cur_sheet); break;
        case M_INS_FUNC:    insert_function_modal(); break;
        case M_INS_SHEET:   add_sheet(); break;
        case M_VIEW_FXBAR:
            g_show_fx = !g_show_fx; VIEW_ITEMS[0].checked = g_show_fx ? true : false;
            ofxbar_blur(&g_fx);
            oapp_set_chrome(g_app, g_show_fx ? ofxbar_height() : 0, OTAB_H);
            relayout();
            break;
        case M_VIEW_GRID:
            g_gridlines = !g_gridlines; VIEW_ITEMS[1].checked = g_gridlines ? true : false;
            break;
        default: break;
    }
    return 1;
}

static int on_event(office_app *a, int type, int p1, int p2) {
    switch (type) {
        case OAPP_EVENT_TOOL: return on_tool(p1, p2);
        case OAPP_EVENT_MENU: return on_menu(p1);
        case OAPP_EVENT_TICK:
#ifdef SHEETS_VERIFY_AUTOMODAL
            // Verification build only (see main()): 1 = Format Cells, 2 = Insert Function.
            { static int fired; if (!fired) { fired = 1; oapp_set_tick_ms(a, 0);
                if (SHEETS_VERIFY_AUTOMODAL == 2) insert_function_modal(); else format_cells_modal(); } }
#endif
            return 1;
        case OAPP_EVENT_FILE_NEW:
            if (confirm_discard()) new_workbook();
            return 1;
        case OAPP_EVENT_FILE_OPEN: {
            if (!confirm_discard()) return 1;
            const char *path = oapp_open_dialog(a, ".xlsx.ods.xls");
            if (path) load_workbook(path);
            return 1;
        }
        case OAPP_EVENT_FILE_SAVE:    do_save(0); return 1;
        case OAPP_EVENT_FILE_SAVE_AS: do_save(1); return 1;
        case OAPP_EVENT_FILE_CLOSE:
            if (confirm_discard()) new_workbook();
            return 1;
        case EVENT_WINDOW_CLOSE:
            return confirm_discard() ? 0 : 1;
        case EVENT_WINDOW_BLUR:
            otabs_leave(&g_tabs);
            return 1;
        case EVENT_RESIZE:
            relayout();
            return 1;
        case EVENT_MOUSE_SCROLL:
            // (a,b) = (scroll_delta, mouse_y) - see officeui.h's mapping table.
            gui_scroll_wheel(&g_vscroll, p1);
            return 1;
        case EVENT_KEY_DOWN: {
            // (a,b) = (keycode, key_char).
            gui_event_t ev; memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_KEY_DOWN; ev.keycode = (uint32_t)p1; ev.key_char = (char)p2;
            if (g_show_fx && g_fx.focus != OFX_FOCUS_NONE) {
                int had = g_fx.focus;
                int r = ofxbar_key(&g_fx, &ev);
                if (r == 2) { if (had == OFX_FOCUS_NAME) name_box_jump(); else { fx_commit(); move_selection(1, 0); } }
                else if (r == 3) { if (had == OFX_FOCUS_NAME) name_box_jump(); else { fx_commit(); move_selection(0, 1); } }
                else if (r == 4) sync_selection();
                return 1;
            }
            int t = otabs_key(&g_tabs, (unsigned)p1, (char)p2, (int)gui_mods_get());
            if (t == OTAB_PREV) { switch_sheet(g_cur_sheet - 1); return 1; }
            if (t == OTAB_NEXT) { switch_sheet(g_cur_sheet + 1); return 1; }
            if (gui_mods_is(GUI_MOD_CTRL)) {
                switch (gui_mods_letter(&ev)) {
                    case 'n': return on_tool(T_NEW, 0);
                    case 'o': return on_tool(T_OPEN, 0);
                    case 's': return on_tool(T_SAVE, 0);
                    case 'z': do_undo(); return 1;
                    case 'y': do_redo(); return 1;
                    case 'b': toggle_bold(); return 1;
                    case 'i': toggle_italic(); return 1;
                    default: break;
                }
            }
            handle_key((uint32_t)p1, (char)p2);
            return 1;
        }
        case EVENT_MOUSE_DOWN:
            handle_mouse_down(p1, p2);
            return 1;
        case EVENT_MOUSE_MOVE:
            gui_scroll_motion(&g_vscroll, p1, p2);
            otabs_motion(&g_tabs, p1, p2);
            if (g_show_fx) ofxbar_motion(&g_fx, p1, p2);
            return 1;
        case EVENT_MOUSE_UP:
            gui_scroll_release(&g_vscroll);
            if (otabs_release(&g_tabs, p1, p2) == OTAB_ADD) add_sheet();
            if (g_show_fx && ofxbar_release(&g_fx, p1, p2) == OFX_PRESS_FXBTN) insert_function_modal();
            return 1;
        default:
            return 1;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_app = oapp_create("SHEETS", 960, 640);
    if (!g_app) return 1;
    {
        gui_font_sel_t sel; memset(&sel, 0, sizeof(sel));
        gui_font_sel_default(&sel);
        g_face = sel.face; g_face_style = sel.style_bits;
    }
    oapp_set_menus(g_app, MENUS, 5);
    otb_init(&g_tb, TB_ITEMS, N_TB_ITEMS, 0, 0, 960);   // the shell re-lays it out to the real width
    otb_set_enabled(&g_tb, T_BORDERS, 0);
    otb_set_enabled(&g_tb, T_UNDO, 0);
    otb_set_enabled(&g_tb, T_REDO, 0);
    oapp_set_toolbar(g_app, &g_tb);
    ofxbar_init(&g_fx, 0, 0, 960);
    otabs_init(&g_tabs, 0, 0, 960);
    oapp_set_chrome(g_app, ofxbar_height(), OTAB_H);
    new_workbook();
#ifdef SHEETS_VERIFY_AUTOOPEN
    // THROWAWAY VERIFICATION BUILD ONLY (never in the shipped/landed source,
    // never defined by the normal Makefile target) - see writer/main.c's own
    // WRITER_VERIFY_AUTOOPEN comment for why: headless QMP keyboard injection
    // into this VM is unreliable past the first synthesized key per boot, so
    // this bypasses the interactive File>Open dialog to screendump the render
    // path directly (xlsx/ods/xls load -> grid draw -> fx_recalc).
    load_workbook(SHEETS_VERIFY_AUTOOPEN);
#ifdef SHEETS_VERIFY_MOVESEL
    // Liveness proof for the verification screendump: moves the selection
    // away from A1 (and pans/scrolls if needed) with no interactive input,
    // since headless QMP keyboard injection into this VM is unreliable
    // (#334/#440). Never defined by the normal Makefile target.
    move_selection(3, 1);
#endif
#ifdef SHEETS_VERIFY_FORMAT
    // Liveness proof 2: applies the Currency format to the active cell so a
    // second screendump shows the chrome (radio toggle) and the grid change.
    apply_format_kind(NF_CURRENCY);
#endif
#endif
#ifdef SHEETS_VERIFY_AUTOMODAL
    // Opens a modal from the first tick so a headless screendump can show it
    // without a mouse or a key reaching the guest (otkdemo's OTKDEMO_AUTOMODAL).
    oapp_set_tick_ms(g_app, 1500);
#endif
    oapp_run(g_app, draw, on_event);
    if (g_wb) wb_free(g_wb);
    oapp_destroy(g_app);
    return 0;
}
