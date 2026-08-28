// main.c - MayteraOS Unit Converter
// Eight-category unit converter (length, mass, temperature, data, speed,
// time, area, volume). Fills a real gap: calc and bc do arithmetic only;
// the OS had no way to convert between units. Layout follows the modern
// converter pattern: category sidebar, From/To value cards with a swap
// button, two unit-selection columns, and an on-screen keypad so the app
// is fully usable with mouse only, plus complete keyboard input.
//
// Freestanding: no libm and no %f in printf, so this file carries its own
// small decimal parser and formatter (same approach as apps/calc). All
// conversions are scale+offset to a per-category base unit, which handles
// temperature (affine) with the same code path as everything else.
//
// Keyboard: digits and '.' type the value, Backspace deletes, C clears,
// - toggles sign, S swaps units, Up/Down change category.

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "string.h"

#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

static int g_win_w = 620, g_win_h = 560;   // live content size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h
#define MIN_W 540
#define MIN_H 470
#define SIDEBAR_W 150
#define PAD 14

// ---------------------------------------------------------------------------
// Theme palette (same mapping idiom as Notes/Settings)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_SIDEBAR, COL_CARD, COL_SEP;
static uint32_t COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEL, COL_HOVER;
static int g_last_theme = -1;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_SIDEBAR=0x00F0F0F0; COL_CARD=0x00F6F6F6; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00CCCCCC;
            COL_SEL=0x00D6E4FB; COL_HOVER=0x00E8E8E8; break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_SIDEBAR=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00808080;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_SEL=0x00A0A0A0; COL_HOVER=0x00D0D0D0; break;
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
            COL_BG=0x00252525; COL_SIDEBAR=0x001E1E1E; COL_CARD=0x002E2E2E; COL_SEP=0x00404040;
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
// Unit tables: value_in_base = value * scale + offset
// ---------------------------------------------------------------------------
typedef struct { const char *name; const char *abbr; double scale; double offset; } unit_t;
typedef struct {
    const char *name;
    const unit_t *units;
    int n;
    int def_from, def_to;
} cat_t;

static const unit_t U_LEN[] = {
    { "Millimeter", "mm", 0.001, 0 },
    { "Centimeter", "cm", 0.01, 0 },
    { "Meter",      "m",  1.0, 0 },
    { "Kilometer",  "km", 1000.0, 0 },
    { "Inch",       "in", 0.0254, 0 },
    { "Foot",       "ft", 0.3048, 0 },
    { "Yard",       "yd", 0.9144, 0 },
    { "Mile",       "mi", 1609.344, 0 },
};
static const unit_t U_MASS[] = {
    { "Milligram", "mg", 0.000001, 0 },
    { "Gram",      "g",  0.001, 0 },
    { "Kilogram",  "kg", 1.0, 0 },
    { "Tonne",     "t",  1000.0, 0 },
    { "Ounce",     "oz", 0.028349523125, 0 },
    { "Pound",     "lb", 0.45359237, 0 },
    { "Stone",     "st", 6.35029318, 0 },
};
static const unit_t U_TEMP[] = {   // base: Kelvin
    { "Celsius",    "C", 1.0, 273.15 },
    { "Fahrenheit", "F", 0.5555555555555556, 255.3722222222222 },
    { "Kelvin",     "K", 1.0, 0 },
};
static const unit_t U_DATA[] = {   // base: byte (binary multiples)
    { "Bit",      "bit", 0.125, 0 },
    { "Byte",     "B",  1.0, 0 },
    { "Kilobyte", "KB", 1024.0, 0 },
    { "Megabyte", "MB", 1048576.0, 0 },
    { "Gigabyte", "GB", 1073741824.0, 0 },
    { "Terabyte", "TB", 1099511627776.0, 0 },
};
static const unit_t U_SPEED[] = {  // base: m/s
    { "Meters/sec", "m/s",  1.0, 0 },
    { "Km/hour",    "km/h", 0.2777777777777778, 0 },
    { "Miles/hour", "mph",  0.44704, 0 },
    { "Knot",       "kn",   0.5144444444444445, 0 },
    { "Feet/sec",   "ft/s", 0.3048, 0 },
};
static const unit_t U_TIME[] = {   // base: second
    { "Millisecond", "ms",  0.001, 0 },
    { "Second",      "s",   1.0, 0 },
    { "Minute",      "min", 60.0, 0 },
    { "Hour",        "hr",  3600.0, 0 },
    { "Day",         "day", 86400.0, 0 },
    { "Week",        "wk",  604800.0, 0 },
};
static const unit_t U_AREA[] = {   // base: square meter
    { "Sq cm",      "cm2", 0.0001, 0 },
    { "Sq meter",   "m2",  1.0, 0 },
    { "Hectare",    "ha",  10000.0, 0 },
    { "Sq km",      "km2", 1000000.0, 0 },
    { "Sq inch",    "in2", 0.00064516, 0 },
    { "Sq foot",    "ft2", 0.09290304, 0 },
    { "Acre",       "ac",  4046.8564224, 0 },
};
static const unit_t U_VOL[] = {    // base: liter (US customary)
    { "Milliliter", "mL",   0.001, 0 },
    { "Liter",      "L",    1.0, 0 },
    { "Cubic meter","m3",   1000.0, 0 },
    { "Fluid ounce","floz", 0.0295735295625, 0 },
    { "Cup",        "cup",  0.2365882365, 0 },
    { "Pint",       "pt",   0.473176473, 0 },
    { "Gallon",     "gal",  3.785411784, 0 },
};

static const cat_t CATS[] = {
    { "Length",      U_LEN,   8, 2, 5 },   // m -> ft
    { "Mass",        U_MASS,  7, 2, 5 },   // kg -> lb
    { "Temperature", U_TEMP,  3, 0, 1 },   // C -> F
    { "Data",        U_DATA,  6, 3, 4 },   // MB -> GB
    { "Speed",       U_SPEED, 5, 1, 2 },   // km/h -> mph
    { "Time",        U_TIME,  6, 2, 3 },   // min -> hr
    { "Area",        U_AREA,  7, 1, 5 },   // m2 -> ft2
    { "Volume",      U_VOL,   7, 1, 6 },   // L -> gal
};
#define NCATS ((int)(sizeof(CATS)/sizeof(CATS[0])))
#define MAX_UNITS 8

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int g_window = -1;
static int g_cat = 0;
static int g_from = 2, g_to = 5;          // unit indices in current category
static char g_input[18] = "1";            // typed value (from side)

// hover tracking
static int g_hov_cat = -1;                // sidebar row
static int g_hov_col = -1, g_hov_row = -1;// unit list (0 from col, 1 to col)
static int g_hov_key = -1;                // keypad index
static int g_hov_swap = 0;

// ---------------------------------------------------------------------------
// Decimal parse / format (freestanding: no strtod, no %f)
// ---------------------------------------------------------------------------
static double parse_input(void) {
    const char *p = g_input;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    double v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10.0 + (*p++ - '0');
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f *= 0.1; }
    }
    return neg ? -v : v;
}

// Format with up to 10 significant fraction digits, trimmed; scientific
// notation for very large/small magnitudes. Same approach as apps/calc.
static void fmt_double(double v, char *out, int cap) {
    char *p = out;
    char *end = out + cap - 1;
    if (v != v) { strlcpy(out, "nan", cap); return; }
    if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
    if (v > 1e300) { strlcpy(out, "inf", cap); return; }

    int sci = 0, ex = 0;
    if (v != 0 && (v >= 1e15 || v < 1e-6)) {
        sci = 1;
        while (v >= 10.0) { v /= 10.0; ex++; }
        while (v < 1.0)   { v *= 10.0; ex--; }
    }

    long long ip = (long long)v;
    double fp = v - (double)ip;

    char tmp[24]; int n = 0;
    if (ip == 0) tmp[n++] = '0';
    long long t = ip;
    while (t > 0) { tmp[n++] = '0' + (int)(t % 10); t /= 10; }
    for (int i = n - 1; i >= 0 && p < end; i--) *p++ = tmp[i];

    // fraction: keep enough digits for ~11 significant digits total
    int keep = 11 - n;
    if (keep > 10) keep = 10;
    if (keep < 0) keep = 0;
    char frac[16]; int fn = 0;
    for (int i = 0; i < keep; i++) {
        fp *= 10.0;
        int d = (int)fp;
        if (d > 9) d = 9;
        frac[fn++] = (char)('0' + d);
        fp -= d;
    }
    // round the last digit up if the remainder says so
    if (fn > 0 && fp >= 0.5) {
        int i = fn - 1;
        while (i >= 0) {
            if (frac[i] < '9') { frac[i]++; break; }
            frac[i] = '0'; i--;
        }
        // note: carry past the integer part is dropped (display precision only)
    }
    while (fn > 0 && frac[fn - 1] == '0') fn--;
    if (fn > 0 && p < end) {
        *p++ = '.';
        for (int i = 0; i < fn && p < end; i++) *p++ = frac[i];
    }

    if (sci && p + 5 < end) {
        *p++ = 'e';
        if (ex < 0) { *p++ = '-'; ex = -ex; } else *p++ = '+';
        char eb[8]; int en = 0;
        if (ex == 0) eb[en++] = '0';
        while (ex > 0) { eb[en++] = '0' + (ex % 10); ex /= 10; }
        for (int i = en - 1; i >= 0 && p < end; i--) *p++ = eb[i];
    }
    *p = '\0';
}

static double convert(double v, const unit_t *from, const unit_t *to) {
    double base = v * from->scale + from->offset;
    return (base - to->offset) / to->scale;
}

// ---------------------------------------------------------------------------
// Layout helpers (all derived from the live window size)
// ---------------------------------------------------------------------------
#define CARD_H 58
static int main_x(void)   { return SIDEBAR_W + PAD; }
static int main_w(void)   { return WIN_W - SIDEBAR_W - 2 * PAD; }
static int from_y(void)   { return 16; }
static int to_y(void)     { return from_y() + CARD_H + 10; }
static int hint_y(void)   { return to_y() + CARD_H + 8; }
static int list_y(void)   { return hint_y() + 24; }
static int keypad_h(void) { return 4 * 32 + 3 * 6; }
static int keypad_y(void) { return WIN_H - keypad_h() - 12; }
static int list_h(void)   { return keypad_y() - 10 - list_y(); }
#define ROW_H 24

// swap button rect (between the two cards, right side)
static void swap_rect(int *x, int *y, int *w, int *h) {
    *w = 64; *h = 26;
    *x = main_x() + main_w() - *w;
    *y = from_y() + CARD_H - 8;
}

// keypad: 4 rows x 4 cols
static const char *KEYS[16] = {
    "7", "8", "9", "C",
    "4", "5", "6", "\x7f",     // backspace glyph
    "1", "2", "3", "+/-",
    "0", ".",  "",  "",        // "0" spans handled below; last two unused
};
static void key_rect(int i, int *x, int *y, int *w, int *h) {
    int r = i / 4, c = i % 4;
    int kw = (main_w() - 3 * 6) / 4;
    *x = main_x() + c * (kw + 6);
    *y = keypad_y() + r * (32 + 6);
    *w = kw; *h = 32;
    if (i == 12) { *w = kw; }                       // "0"
    if (i == 13) { }                                 // "."
    if (i == 14) { *w = 2 * kw + 6; }                // wide Swap key
}
static const char *key_label(int i) {
    if (i == 14) return "Swap";
    return KEYS[i];
}
static int key_active(int i) { return i != 15 && key_label(i)[0] != '\0'; }

// ---------------------------------------------------------------------------
// Input editing
// ---------------------------------------------------------------------------
static void input_clear(void) { g_input[0] = '0'; g_input[1] = 0; }

static void input_digit(char d) {
    int l = (int)strlen(g_input);
    if (l >= (int)sizeof(g_input) - 1) return;
    if (g_input[0] == '0' && g_input[1] == 0) { g_input[0] = d; return; }
    if (g_input[0] == '-' && g_input[1] == '0' && g_input[2] == 0) { g_input[1] = d; return; }
    g_input[l] = d; g_input[l + 1] = 0;
}
static void input_dot(void) {
    for (int i = 0; g_input[i]; i++) if (g_input[i] == '.') return;
    int l = (int)strlen(g_input);
    if (l >= (int)sizeof(g_input) - 1) return;
    if (l == 0) { strlcpy(g_input, "0.", sizeof(g_input)); return; }
    g_input[l] = '.'; g_input[l + 1] = 0;
}
static void input_backspace(void) {
    int l = (int)strlen(g_input);
    if (l > 0) g_input[l - 1] = 0;
    if (g_input[0] == 0 || (g_input[0] == '-' && g_input[1] == 0)) input_clear();
}
static void input_negate(void) {
    if (g_input[0] == '-') {
        int l = (int)strlen(g_input);
        for (int i = 0; i < l; i++) g_input[i] = g_input[i + 1];
    } else {
        int l = (int)strlen(g_input);
        if (l >= (int)sizeof(g_input) - 2) return;
        for (int i = l; i >= 0; i--) g_input[i + 1] = g_input[i];
        g_input[0] = '-';
    }
}
static void do_swap(void) {
    int t = g_from; g_from = g_to; g_to = t;
}
static void select_cat(int c) {
    if (c < 0 || c >= NCATS || c == g_cat) return;
    g_cat = c;
    g_from = CATS[c].def_from;
    g_to = CATS[c].def_to;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void draw_sidebar(void) {
    win_draw_rect(g_window, 0, 0, SIDEBAR_W, WIN_H, COL_SIDEBAR);
    win_draw_rect(g_window, SIDEBAR_W - 1, 0, 1, WIN_H, COL_SEP);
    draw_text_sz(g_window, 14, 12, "Convert", 18, COL_TEXT);

    int y = 48;
    for (int i = 0; i < NCATS; i++) {
        if (i == g_cat)
            gui_fill_rounded_aa(g_window, 6, y, SIDEBAR_W - 12, 30, 5, COL_SEL, COL_SIDEBAR);
        else if (i == g_hov_cat)
            gui_fill_rounded_aa(g_window, 6, y, SIDEBAR_W - 12, 30, 5, COL_HOVER, COL_SIDEBAR);
        uint32_t tc = (i == g_cat && COL_SEL == 0x00000080) ? 0x00FFFFFF
                    : (i == g_cat ? COL_TEXT : COL_TEXT2);
        draw_text_sz(g_window, 16, y + 7, CATS[i].name, 13, tc);
        y += 34;
    }
}

static void draw_value_card(int y, const char *label, const char *value,
                            const unit_t *u, int is_from) {
    int x = main_x(), w = main_w();
    gui_card(g_window, x, y, w, CARD_H);
    draw_text_sz(g_window, x + 12, y + 6, label, 11, COL_TEXT_DIM);

    // unit abbreviation, right aligned
    char ab[24];
    snprintf(ab, sizeof(ab), "%s", u->abbr);
    int aw = gui_ttf_width(ab, 13);
    draw_text_sz(g_window, x + w - 12 - aw, y + 6, ab, 13, COL_TEXT2);

    // big value; shrink if too wide
    int size = 26;
    if (gui_ttf_width(value, size) > w - 24) size = 19;
    if (gui_ttf_width(value, size) > w - 24) size = 14;
    uint32_t vc = is_from ? COL_TEXT : COL_ACCENT;
    draw_text_sz(g_window, x + 12, y + CARD_H - size - 8, value, size, vc);

    if (is_from) {
        // text cursor after the typed value
        int cx = x + 12 + gui_ttf_width(value, size) + 2;
        if (cx < x + w - 14)
            win_draw_rect(g_window, cx, y + CARD_H - size - 8, 1, size + 2, COL_ACCENT);
    }
}

static void draw_unit_lists(void) {
    int x = main_x(), w = main_w();
    int colw = (w - 10) / 2;
    int y0 = list_y();
    const cat_t *c = &CATS[g_cat];

    draw_text_sz(g_window, x + 4, y0, "From unit", 11, COL_TEXT_DIM);
    draw_text_sz(g_window, x + colw + 10 + 4, y0, "To unit", 11, COL_TEXT_DIM);

    int rows_fit = (list_h() - 20) / ROW_H;
    int nrows = c->n < rows_fit ? c->n : rows_fit;

    for (int col = 0; col < 2; col++) {
        int cx = x + col * (colw + 10);
        int sel = col == 0 ? g_from : g_to;
        for (int i = 0; i < nrows; i++) {
            int ry = y0 + 18 + i * ROW_H;
            if (i == sel)
                gui_fill_rounded_aa(g_window, cx, ry, colw, ROW_H - 3, 4, COL_SEL, COL_BG);
            else if (col == g_hov_col && i == g_hov_row)
                gui_fill_rounded_aa(g_window, cx, ry, colw, ROW_H - 3, 4, COL_HOVER, COL_BG);
            uint32_t tc = (i == sel && COL_SEL == 0x00000080) ? 0x00FFFFFF
                        : (i == sel ? COL_TEXT : COL_TEXT2);
            draw_text_sz(g_window, cx + 10, ry + 5, c->units[i].name, 12, tc);
            int aw = gui_ttf_width(c->units[i].abbr, 11);
            uint32_t ac = (i == sel && COL_SEL == 0x00000080) ? 0x00DDDDDD : COL_TEXT_DIM;
            draw_text_sz(g_window, cx + colw - 10 - aw, ry + 6, c->units[i].abbr, 11, ac);
        }
    }
}

static void draw_keypad(void) {
    for (int i = 0; i < 16; i++) {
        if (!key_active(i)) continue;
        if (i == 15) continue;
        int x, y, w, h; key_rect(i, &x, &y, &w, &h);
        const char *lb = key_label(i);
        gui_btn_variant_t var = GUI_BTN_SECONDARY;
        if (i == 14) var = GUI_BTN_PRIMARY;                    // Swap
        gui_state_t st = (i == g_hov_key) ? GUI_ST_HOVER : GUI_ST_NORMAL;
        gui_button(g_window, x, y, w, h, lb, var, st);
    }
}

static void draw_all(void) {
    // re-sync live content size from the compositor each frame
    { int w = g_win_w, h = g_win_h;
      win_get_size(g_window, &w, &h);
      if (w >= MIN_W) g_win_w = w;
      if (h >= MIN_H) g_win_h = h;
    }
    win_draw_rect(g_window, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_sidebar();

    const cat_t *c = &CATS[g_cat];
    const unit_t *uf = &c->units[g_from];
    const unit_t *ut = &c->units[g_to];

    double v = parse_input();
    double r = convert(v, uf, ut);
    char res[40]; fmt_double(r, res, sizeof(res));

    draw_value_card(from_y(), "From", g_input, uf, 1);
    draw_value_card(to_y(), "To", res, ut, 0);

    // swap button
    { int sx, sy, sw, sh; swap_rect(&sx, &sy, &sw, &sh);
      gui_button(g_window, sx, sy, sw, sh, "Swap", GUI_BTN_GHOST,
                 g_hov_swap ? GUI_ST_HOVER : GUI_ST_NORMAL); }

    // rate hint: 1 from = X to
    { char one[40]; fmt_double(convert(1.0, uf, ut), one, sizeof(one));
      char hint[96];
      snprintf(hint, sizeof(hint), "1 %s = %s %s", uf->abbr, one, ut->abbr);
      draw_text_sz(g_window, main_x() + 4, hint_y(), hint, 12, COL_TEXT_DIM); }

    draw_unit_lists();
    draw_keypad();
    win_invalidate(g_window);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
static int hit_cat(int mx, int my) {
    if (mx >= SIDEBAR_W) return -1;
    if (my < 48) return -1;
    int i = (my - 48) / 34;
    if (i >= 0 && i < NCATS && my < 48 + i * 34 + 30) return i;
    return -1;
}
static int hit_unit(int mx, int my, int *col, int *row) {
    int x = main_x(), w = main_w();
    int colw = (w - 10) / 2;
    int y0 = list_y() + 18;
    int rows_fit = (list_h() - 20) / ROW_H;
    int nrows = CATS[g_cat].n < rows_fit ? CATS[g_cat].n : rows_fit;
    for (int cidx = 0; cidx < 2; cidx++) {
        int cx = x + cidx * (colw + 10);
        if (mx >= cx && mx < cx + colw && my >= y0 && my < y0 + nrows * ROW_H) {
            *col = cidx; *row = (my - y0) / ROW_H;
            return 1;
        }
    }
    return 0;
}
static int hit_key(int mx, int my) {
    for (int i = 0; i < 16; i++) {
        if (!key_active(i) || i == 15) continue;
        int x, y, w, h; key_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}
static int hit_swap(int mx, int my) {
    int x, y, w, h; swap_rect(&x, &y, &w, &h);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void do_key(int i) {
    const char *lb = key_label(i);
    if (lb[0] >= '0' && lb[0] <= '9' && lb[1] == 0) { input_digit(lb[0]); return; }
    if (lb[0] == '.') { input_dot(); return; }
    if (lb[0] == 'C') { input_clear(); return; }
    if (lb[0] == '\x7f') { input_backspace(); return; }
    if (!strcmp(lb, "+/-")) { input_negate(); return; }
    if (!strcmp(lb, "Swap")) { do_swap(); return; }
}

static void on_key(gui_event_t *ev) {
    char ch = ev->key_char;
    if (ch >= '0' && ch <= '9') { input_digit(ch); return; }
    if (ch == '.') { input_dot(); return; }
    if (ch == '\b' || ch == 127) { input_backspace(); return; }
    if (ch == '-') { input_negate(); return; }
    if (ch == 'c' || ch == 'C' || ch == 27) { input_clear(); return; }
    if (ch == 's' || ch == 'S' || ch == '\t') { do_swap(); return; }
    // #191: these were 0x48/0x50, PS/2 scancodes the kernel never delivers, so
    // arrow selection had never worked; 0x48/0x50 are ASCII 'H'/'P', so
    // shift-H and shift-P moved the category instead.
    if (ev->keycode == GUI_KEY_UP)   { select_cat((g_cat + NCATS - 1) % NCATS); return; }
    if (ev->keycode == GUI_KEY_DOWN) { select_cat((g_cat + 1) % NCATS); return; }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_last_theme = get_theme();
    apply_theme(g_last_theme);
    select_cat(0);
    g_cat = 0; g_from = CATS[0].def_from; g_to = CATS[0].def_to;

    g_window = win_create("Unit Converter", 180, 90, WIN_W, WIN_H);
    if (g_window < 0) { printf("convert: failed to create window\n"); return 1; }

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        { int th = get_theme();
          if (th != g_last_theme) { g_last_theme = th; apply_theme(th); draw_all(); } }
        int et = win_get_event(g_window, &ev, 250);
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) {
                    g_win_w = ev.mouse_x; g_win_h = ev.mouse_y;
                    if (g_win_w < MIN_W) g_win_w = MIN_W;
                    if (g_win_h < MIN_H) g_win_h = MIN_H;
                }
                draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN: on_key(&ev); draw_all(); break;
            case EVENT_MOUSE_MOVE: {
                int nc = hit_cat(ev.mouse_x, ev.mouse_y);
                int col = -1, row = -1;
                hit_unit(ev.mouse_x, ev.mouse_y, &col, &row);
                int nk = hit_key(ev.mouse_x, ev.mouse_y);
                int ns = hit_swap(ev.mouse_x, ev.mouse_y);
                if (nc != g_hov_cat || col != g_hov_col || row != g_hov_row ||
                    nk != g_hov_key || ns != g_hov_swap) {
                    g_hov_cat = nc; g_hov_col = col; g_hov_row = row;
                    g_hov_key = nk; g_hov_swap = ns;
                    draw_all();
                }
                break;
            }
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int c = hit_cat(ev.mouse_x, ev.mouse_y);
                    if (c >= 0) { select_cat(c); draw_all(); break; }
                    int col, row;
                    if (hit_unit(ev.mouse_x, ev.mouse_y, &col, &row)) {
                        if (row >= 0 && row < CATS[g_cat].n) {
                            if (col == 0) g_from = row; else g_to = row;
                        }
                        draw_all(); break;
                    }
                    if (hit_swap(ev.mouse_x, ev.mouse_y)) { do_swap(); draw_all(); break; }
                    int k = hit_key(ev.mouse_x, ev.mouse_y);
                    if (k >= 0) { do_key(k); draw_all(); }
                }
                break;
            default: break;
        }
    }
    win_destroy(g_window);
    return 0;
}
