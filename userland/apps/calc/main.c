// calc - Calculator for MayteraOS (Standard + Scientific), glass edition
//
// A large result display with an expression line above it, a memory row, mode
// tabs (Standard / Scientific) and a button grid. Scientific mode adds trig,
// logs, powers, roots, factorial, constants and parentheses, with a 2nd toggle
// for inverse functions and a DEG/RAD toggle.
//
// (calcglass) The chrome is the shared dark-teal glass language
// (docs/UI_GLASS_DESIGN_SYSTEM.md sections 1, 10, 11), the fourth surface in
// that restyle after the App Repo, the browser chrome and the Task Manager
// (tmglass): a frosted-wallpaper backdrop in the margins, two rounded glass
// panels (display, keypad), pill tabs on the backdrop, and keys built from the
// shared rounded primitives. Everything the calculator DOES (arithmetic,
// keyboard, memory keys, the projected tool contract) is untouched.
//
// The userland C library is freestanding (no libm and no %f in printf), so
// this file carries its own small double-precision math library and an
// expression evaluator (recursive descent with operator precedence).

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"    // gui_fill_rounded_aa / gui_rounded_border / gui_soft_shadow / gui_mix / gui_glass_backdrop_*

// #301: all in-window text goes through the antialiased TrueType path
// (SYS_WIN_DRAW_TTF, size packed in the top byte of the colour by the libc
// wrapper). Sizes are TTF pixel sizes, one per role (glass doc section 5).
#define TTF_TAB    12   // tab pill labels, memory row, history / echo lines
#define TTF_IND    11   // DEG/RAD, M, 2nd indicators
#define TTF_DIGIT  16   // digit and operator key faces
#define TTF_FUNC   14   // function key faces
#define TTF_EQ     18   // the "=" key face
#define TTF_BIG    30   // large result

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// #436: bumped from 460x600 (#301) to 460x624 for extra headroom, and the
// window content size is now re-synced from the compositor every frame (see
// draw_all()) via win_get_size(), the same idiom used by devmgr/taskmanager/
// solitaire/irc/glcube. #301 sized this purely from the *requested* creation
// size; on hardware where the compositor grants a shorter content area than
// requested (e.g. a lower real screen resolution than the dev VMs use), the
// button grid kept computing its geometry from the stale requested WIN_H,
// so the bottom scientific-mode rows were clipped by the actual (shorter)
// window. Syncing to the real granted size each frame lets the existing
// dynamic grid (cell_geom) reflow to whatever height the window actually has.
static int g_win_w = 460, g_win_h = 624;  // #89/#301/#436: live window size
#define WIN_W g_win_w
#define WIN_H g_win_h
#define MIN_WIN_W 340   // #436: floor so the grid never degenerates
#define MIN_WIN_H 460

// (calcglass) ONE definition of every band, shared by the draw and hit-test
// paths (glass doc section 8: "one definition for anything three places agree
// about"). Before this, draw_tabs()/hit_tab() and draw_memory_row()/
// hit_memory() each carried their own copy of the same literals. Values match
// the Task Manager's logic.rs (PAD 10, TAB_H 26, TAB_W 96, TAB_GAP 6,
// PANEL_R 12, PANEL_IN 12) so the two windows share one geometry.
#define PAD       10                       // window margin: the backdrop shows here
#define TAB_H     26                       // tab pill height (radius TAB_H/2)
#define TAB_W     96
#define TAB_GAP   6
#define DISP_Y    (PAD + TAB_H + 10)       // display panel top (46)
#define DISP_H    110
#define KEY_Y     (DISP_Y + DISP_H + 8)    // keypad panel top (164)
#define PANEL_R   12                       // panel corner radius
#define PANEL_IN  12                       // inset from a panel edge to its content
#define MEM_Y     (KEY_Y + 8)              // memory row top
#define MEM_H     22
#define GRID_X    (PAD + PANEL_IN)         // key grid left (22)
#define GRID_W    (WIN_W - 2 * GRID_X)     // key grid width (416)
#define GRID_TOP  (MEM_Y + MEM_H + 8)      // key grid top (202)
#define KEY_GAP   6
#define KEY_RAD     6                        // key corner radius (section 6 nested card)

// (calcglass) Glass tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// exact names and values the Task Manager (tmglass) and App Repo use, so the
// windows read as one material side by side. Fixed dark glass regardless of
// the active theme (owner decision recorded at glasstm): the window carries
// its own backdrop, so there is no light-theme surface for these to sit on.
// Copy the NAMES as well as the values; a second name for the same hex is how
// two surfaces drift apart.
#define C_PANEL       0x00122420   // WEL_BG_MID: panel fill, the outer colour every key AA-blends toward
#define C_CARD        0x000E1D1B   // DK_CARD_FILL: function / operator key fill
#define C_EDGE        0x002C4A44   // DK_STROKE_UNSEL: panel border, card-key border
#define C_INK         0x00F3FBF9   // DK_HEADLINE: result, digit faces
#define C_INK_DIM     0x00A9D9CC   // DK_BODY: history, echo, function faces, tab labels
#define C_ACCENT      0x006AE2CF   // DK_ACCENT: operator faces, selected pill, active toggle
#define C_ACCENT_INK  0x0004231A   // text on the accent
#define C_ERR         0x00FFAAA2   // DK_ERROR: the "Error" history line
#define C_BTN_TOP     0x000F8068   // DK_BTN_TOP: "=" gradient top (section 6 primary button)
#define C_BTN_BOTTOM  0x000A5D4C   // DK_BTN_BOTTOM: "=" gradient bottom
#define WEL_BG_TOP    0x000A1614   // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM 0x00050A09   // backdrop gradient fallback, bottom stop
#define C_EQ_INK      0x00FFFFFF   // DK_BTN_TEXT

// ---------------------------------------------------------------------------
// (glasslib) The frosted-wallpaper backdrop: now the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*), consolidated out of this
// file, the Task Manager, the Media Player and the Image Viewer, which all
// carried an identical copy (blame.md tmglass/calcglass/audglass/imgglass).
// This app owns only the two small persistent pieces the API asks for; the
// scratch (raw thumbnail bytes, blur temp plane) lives in gui.c.
// ---------------------------------------------------------------------------
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make draw_all() blit the backdrop

// The ONE self-committing call in this app (see draw_all()): SYS_WIN_BLIT
// copies the backdrop into the window content scaled to the content rect
// (x/y are ignored by the kernel) and publishes that frame on its own.
static void bd_blit(int win){
    gui_glass_backdrop_blit(win, g_bd);
}

// The backdrop colour under content pixel (x, y): what the kernel's nearest-
// neighbour scale put there, to within the blur. Every AA edge and shadow
// drawn onto the backdrop takes its outer colour from here; a flat guess is
// what produces a square halo around a round corner (glass doc section 2).
static uint32_t bd_at(int x, int y){
    return gui_glass_backdrop_at(g_bd, WIN_W, WIN_H, x, y);
}

// Rebuild the backdrop if the wallpaper changed. SYS_GET_WALLPAPER is the
// same poll the compositor makes; a changed index marks the chrome dirty.
static void sync_backdrop(void){
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, C_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

// Button kinds (drive coloring only)
enum { K_DIGIT, K_FUNC, K_OP, K_EQ };

typedef struct {
    const char *face;    // label shown normally
    const char *face2;   // label shown when 2nd is active (NULL = same)
    const char *tok;     // action token (normal)
    const char *tok2;    // action token when 2nd (NULL = same)
    int gx, gy, gw;      // grid column, row, column-span
    int kind;
} btn_t;

// (calcglass) FACE BYTES ARE LATIN-1, NOT CP437. The kernel TTF path
// (kernel/gui/ttf.c ttf_draw_string: `ttf_get_glyph((unsigned char)str[i])`)
// maps each BYTE straight to a codepoint, with no UTF-8 decoding, so a face
// written in the old CP437 habit ("x\xfd" for x squared, "\xfb" for the
// radical, "\xf6" for the divide sign, "\xe3" for pi) has been rendering as
// "xý", "û", "ö" and "ã" ever since #301 moved the faces onto TTF. Latin-1
// has ² (0xB2), ³ (0xB3), ÷ (0xF7), × (0xD7), ¯ (0xAF) and ¹ (0xB9); it has
// no radical, pi or backspace glyph, so those three faces are STROKED in
// draw_key() (glass doc section 6: "stroked, not typed. Nothing guarantees
// the loaded TTF carries the codepoint"), keyed on the token, and the table
// carries a readable ASCII face for the projected contract's descriptions.
// Standard: 4 columns x 6 rows
static const btn_t std_btns[] = {
    {"%",  0,"pct",0,  0,0,1,K_FUNC}, {"CE",0,"CE",0, 1,0,1,K_FUNC}, {"C",0,"C",0, 2,0,1,K_FUNC}, {"backspace",0,"back",0, 3,0,1,K_FUNC},
    {"1/x",0,"inv",0, 0,1,1,K_FUNC}, {"x\xb2",0,"sqr",0, 1,1,1,K_FUNC}, {"sqrt x",0,"sqrt(",0, 2,1,1,K_FUNC}, {"\xf7",0,"/",0, 3,1,1,K_OP},
    {"7",0,"7",0, 0,2,1,K_DIGIT}, {"8",0,"8",0, 1,2,1,K_DIGIT}, {"9",0,"9",0, 2,2,1,K_DIGIT}, {"\xd7",0,"*",0, 3,2,1,K_OP},
    {"4",0,"4",0, 0,3,1,K_DIGIT}, {"5",0,"5",0, 1,3,1,K_DIGIT}, {"6",0,"6",0, 2,3,1,K_DIGIT}, {"-",0,"-",0, 3,3,1,K_OP},
    {"1",0,"1",0, 0,4,1,K_DIGIT}, {"2",0,"2",0, 1,4,1,K_DIGIT}, {"3",0,"3",0, 2,4,1,K_DIGIT}, {"+",0,"+",0, 3,4,1,K_OP},
    {"+/-",0,"neg",0, 0,5,1,K_DIGIT}, {"0",0,"0",0, 1,5,1,K_DIGIT}, {".",0,".",0, 2,5,1,K_DIGIT}, {"=",0,"=",0, 3,5,1,K_EQ},
};
#define STD_N ((int)(sizeof(std_btns)/sizeof(std_btns[0])))
#define STD_COLS 4
#define STD_ROWS 6

// Scientific: 6 columns x 7 rows
static const btn_t sci_btns[] = {
    {"2nd",0,"2nd",0, 0,0,1,K_FUNC}, {"pi",0,"pi",0, 1,0,1,K_FUNC}, {"e",0,"e",0, 2,0,1,K_FUNC}, {"C",0,"C",0, 3,0,1,K_FUNC}, {"CE",0,"CE",0, 4,0,1,K_FUNC}, {"backspace",0,"back",0, 5,0,1,K_FUNC},
    {"sin","sin\xaf\xb9","sin(","asin(", 0,1,1,K_FUNC}, {"x\xb2","x\xb3","sqr","cube", 1,1,1,K_FUNC}, {"1/x",0,"inv",0, 2,1,1,K_FUNC}, {"|x|",0,"abs(",0, 3,1,1,K_FUNC}, {"exp",0,"exp(",0, 4,1,1,K_FUNC}, {"\xf7",0,"/",0, 5,1,1,K_OP},
    {"cos","cos\xaf\xb9","cos(","acos(", 0,2,1,K_FUNC}, {"sqrt x","cbrt x","sqrt(","cbrt(", 1,2,1,K_FUNC}, {"(",0,"(",0, 2,2,1,K_FUNC}, {")",0,")",0, 3,2,1,K_FUNC}, {"n!",0,"fact",0, 4,2,1,K_FUNC}, {"\xd7",0,"*",0, 5,2,1,K_OP},
    {"tan","tan\xaf\xb9","tan(","atan(", 0,3,1,K_FUNC}, {"x^y",0,"pow",0, 1,3,1,K_FUNC}, {"7",0,"7",0, 2,3,1,K_DIGIT}, {"8",0,"8",0, 3,3,1,K_DIGIT}, {"9",0,"9",0, 4,3,1,K_DIGIT}, {"-",0,"-",0, 5,3,1,K_OP},
    {"log",0,"log(",0, 0,4,1,K_FUNC}, {"10^x",0,"tenx",0, 1,4,1,K_FUNC}, {"4",0,"4",0, 2,4,1,K_DIGIT}, {"5",0,"5",0, 3,4,1,K_DIGIT}, {"6",0,"6",0, 4,4,1,K_DIGIT}, {"+",0,"+",0, 5,4,1,K_OP},
    {"ln","e^x","ln(","expe", 0,5,1,K_FUNC}, {"%",0,"pct",0, 1,5,1,K_FUNC}, {"1",0,"1",0, 2,5,1,K_DIGIT}, {"2",0,"2",0, 3,5,1,K_DIGIT}, {"3",0,"3",0, 4,5,1,K_DIGIT}, {"mod",0,"mod",0, 5,5,1,K_OP},
    {"DEG",0,"deg",0, 0,6,1,K_FUNC}, {"+/-",0,"neg",0, 1,6,1,K_DIGIT}, {"0",0,"0",0, 2,6,1,K_DIGIT}, {".",0,".",0, 3,6,1,K_DIGIT}, {"=",0,"=",0, 4,6,2,K_EQ},
};
#define SCI_N ((int)(sizeof(sci_btns)/sizeof(sci_btns[0])))
#define SCI_COLS 6
#define SCI_ROWS 7

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int window_handle = -1;
static int mode = 0;        // 0 = standard, 1 = scientific
static int second = 0;      // 2nd function toggle (scientific)
static int deg = 1;         // 1 = degrees, 0 = radians
static double memory = 0.0;
static int  mem_set = 0;
static char expr[160] = "";
static char prevline[160] = "";   // history / last expression
static int  err = 0;
static int  hover = -1;     // hovered button index, -1 none
static int  hover_tab = -1; // 0 std tab, 1 sci tab, -1 none
static int  hover_mem = -1; // (calcglass) hovered memory key 0..4, -1 none

// ---------------------------------------------------------------------------
// Minimal double math library (freestanding: no libm)
// ---------------------------------------------------------------------------
#define M_PI    3.14159265358979323846
#define M_E     2.71828182845904523536
#define M_LN2   0.69314718055994530942
#define M_LN10  2.30258509299404568402

static double m_fabs(double x){ return x < 0 ? -x : x; }

static double m_floor(double x){
    if (x >= 9.2e18 || x <= -9.2e18) return x;
    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

static double m_sqrt(double x){
    if (x < 0) { err = 1; return 0; }
    if (x == 0) return 0;
    double g = x > 1 ? x : 1.0;
    for (int i = 0; i < 60; i++) g = 0.5 * (g + x / g);
    return g;
}

static double m_exp(double x){
    if (x > 709) return 1e308;
    if (x < -745) return 0;
    // range reduce: x = k*ln2 + r,  e^x = 2^k * e^r
    double kf = m_floor(x / M_LN2 + 0.5);
    int k = (int)kf;
    double r = x - kf * M_LN2;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 18; n++){ term *= r / n; sum += term; }
    // multiply by 2^k
    double p = 1.0;
    int ak = k < 0 ? -k : k;
    for (int i = 0; i < ak; i++) p *= 2.0;
    return k < 0 ? sum / p : sum * p;
}

static double m_ln(double x){
    if (x <= 0) { err = 1; return 0; }
    int k = 0;
    while (x >= 2.0){ x /= 2.0; k++; }
    while (x < 1.0){ x *= 2.0; k--; }
    // x in [1,2): ln(x) = 2*(y + y^3/3 + y^5/5 + ...), y=(x-1)/(x+1)
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y, term = y, sum = 0.0;
    for (int n = 1; n <= 25; n += 2){ sum += term / n; term *= y2; }
    return 2.0 * sum + k * M_LN2;
}

static double m_log10(double x){ return m_ln(x) / M_LN10; }

static double m_pow(double b, double e){
    // integer exponent: exact, supports negative base
    double re = m_floor(e);
    if (re == e && m_fabs(e) < 1024){
        int n = (int)e, an = n < 0 ? -n : n;
        double p = 1.0;
        for (int i = 0; i < an; i++) p *= b;
        return n < 0 ? 1.0 / p : p;
    }
    if (b < 0){ err = 1; return 0; }
    if (b == 0) return e > 0 ? 0 : (err = 1, 0);
    return m_exp(e * m_ln(b));
}

static double m_sin(double x){
    // reduce to [-pi, pi]
    double t = x / (2 * M_PI);
    x -= 2 * M_PI * m_floor(t + 0.5);
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 12; n++){
        term *= -x2 / ((2*n) * (2*n + 1));
        sum += term;
    }
    return sum;
}
static double m_cos(double x){
    double t = x / (2 * M_PI);
    x -= 2 * M_PI * m_floor(t + 0.5);
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int n = 1; n < 12; n++){
        term *= -x2 / ((2*n - 1) * (2*n));
        sum += term;
    }
    return sum;
}
static double m_tan(double x){ double c = m_cos(x); if (m_fabs(c) < 1e-12){ err=1; return 0;} return m_sin(x)/c; }

static double m_atan(double x){
    int neg = 0, inv = 0; double add = 0;
    if (x < 0){ neg = 1; x = -x; }
    if (x > 1){ inv = 1; x = 1.0 / x; add = M_PI / 2; }
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 60; n++){
        term *= -x2;
        sum += term / (2*n + 1);
    }
    double r = inv ? add - sum : sum;
    return neg ? -r : r;
}
static double m_asin(double x){
    if (x < -1 || x > 1){ err = 1; return 0; }
    if (x == 1) return M_PI/2; if (x == -1) return -M_PI/2;
    return m_atan(x / m_sqrt(1 - x*x));
}
static double m_acos(double x){ return M_PI/2 - m_asin(x); }

static double m_fmod(double a, double b){
    if (b == 0){ err = 1; return 0; }
    double q = m_floor(a / b);
    return a - q * b;
}

static double m_fact(double v){
    double n = m_floor(v + 0.5);
    if (n < 0 || m_fabs(n - v) > 1e-9 || n > 170){ err = 1; return 0; }
    double r = 1.0;
    for (int i = 2; i <= (int)n; i++) r *= i;
    return r;
}

// ---------------------------------------------------------------------------
// Expression parser (recursive descent, operator precedence)
// ---------------------------------------------------------------------------
static const char *P;

static void skipsp(void){ while (*P == ' ') P++; }
static double parse_expr(void);

static int matchkw(const char *kw){
    const char *p = P; int i = 0;
    while (kw[i]){ if (p[i] != kw[i]) return 0; i++; }
    // ensure not part of a longer identifier
    char c = p[i];
    if ((c >= 'a' && c <= 'z')) return 0;
    P += i;
    return 1;
}

static double to_rad(double a){ return deg ? a * M_PI / 180.0 : a; }
static double from_rad(double a){ return deg ? a * 180.0 / M_PI : a; }

static double parse_number(void){
    double v = 0;
    while (*P >= '0' && *P <= '9') v = v * 10 + (*P++ - '0');
    if (*P == '.'){
        P++;
        double f = 0.1;
        while (*P >= '0' && *P <= '9'){ v += (*P++ - '0') * f; f *= 0.1; }
    }
    return v;
}

static double parse_primary(void){
    skipsp();
    if (*P == '('){ P++; double v = parse_expr(); skipsp(); if (*P == ')') P++; else err = 1; return v; }
    if ((*P >= '0' && *P <= '9') || *P == '.') return parse_number();
    // identifiers: constants and functions
    if (matchkw("pi")) return M_PI;
    if (*P == 'e' && !(P[1] >= 'a' && P[1] <= 'z')){ P++; return M_E; }
    // functions (consume name then "( expr )")
    struct { const char *n; int id; } fns[] = {
        {"asin",1},{"acos",2},{"atan",3},{"sin",4},{"cos",5},{"tan",6},
        {"sqrt",7},{"cbrt",8},{"ln",9},{"log",10},{"abs",11},{"exp",12},
    };
    for (int i = 0; i < 12; i++){
        if (matchkw(fns[i].n)){
            skipsp(); if (*P == '(') P++; else { err = 1; return 0; }
            double a = parse_expr();
            skipsp(); if (*P == ')') P++; else err = 1;
            switch (fns[i].id){
                case 1: return from_rad(m_asin(a));
                case 2: return from_rad(m_acos(a));
                case 3: return from_rad(m_atan(a));
                case 4: return m_sin(to_rad(a));
                case 5: return m_cos(to_rad(a));
                case 6: return m_tan(to_rad(a));
                case 7: return m_sqrt(a);
                case 8: return a < 0 ? -m_pow(-a, 1.0/3.0) : m_pow(a, 1.0/3.0);
                case 9: return m_ln(a);
                case 10: return m_log10(a);
                case 11: return m_fabs(a);
                case 12: return m_exp(a);
            }
        }
    }
    err = 1;
    return 0;
}

static double parse_postfix(void){
    double v = parse_primary();
    for (;;){
        skipsp();
        if (*P == '!'){ P++; v = m_fact(v); }
        else if (*P == '%'){ P++; v = v / 100.0; }
        else break;
    }
    return v;
}

static double parse_unary(void){
    skipsp();
    if (*P == '-'){ P++; return -parse_unary(); }
    if (*P == '+'){ P++; return parse_unary(); }
    return parse_postfix();
}

static double parse_power(void){
    double b = parse_unary();
    skipsp();
    if (*P == '^'){ P++; double e = parse_power(); return m_pow(b, e); }
    return b;
}

static double parse_term(void){
    double a = parse_power();
    for (;;){
        skipsp();
        if (*P == '*'){ P++; a *= parse_power(); }
        else if (*P == '/'){ P++; double b = parse_power(); if (b == 0){ err = 1; } else a /= b; }
        else if (*P == 'm' && matchkw("mod")){ a = m_fmod(a, parse_power()); }
        else break;
    }
    return a;
}

static double parse_expr(void){
    double a = parse_term();
    for (;;){
        skipsp();
        if (*P == '+'){ P++; a += parse_term(); }
        else if (*P == '-'){ P++; a -= parse_term(); }
        else break;
    }
    return a;
}

// Evaluate the current expression string. Returns ok flag via *ok.
static double evaluate(const char *s, int *ok){
    err = 0;
    P = s;
    skipsp();
    if (*P == '\0'){ *ok = 0; return 0; }
    double v = parse_expr();
    skipsp();
    if (*P != '\0') err = 1;   // trailing junk
    *ok = !err;
    return v;
}

// ---------------------------------------------------------------------------
// Number formatting (no %f in libc)
// ---------------------------------------------------------------------------
static void fmt_double(double v, char *out){
    char *p = out;
    if (v != v){ out[0]='n';out[1]='a';out[2]='n';out[3]=0; return; }
    if (v < 0){ *p++ = '-'; v = -v; }

    double av = v;
    int sci = 0, ex = 0;
    if (av != 0 && (av >= 1e16 || av < 1e-6)){
        sci = 1;
        while (av >= 10.0){ av /= 10.0; ex++; }
        while (av < 1.0){ av *= 10.0; ex--; }
        v = av;
    }

    // round to 12 significant digits
    long long ip = (long long)v;
    double fp = v - (double)ip;

    // integer part
    char tmp[24]; int n = 0;
    if (ip == 0) tmp[n++] = '0';
    long long t = ip;
    while (t > 0){ tmp[n++] = '0' + (int)(t % 10); t /= 10; }
    for (int i = n - 1; i >= 0; i--) *p++ = tmp[i];

    // fractional part, up to 10 digits, trimmed
    char frac[16]; int fn = 0;
    for (int i = 0; i < 10; i++){
        fp *= 10.0;
        int d = (int)fp;
        if (d > 9) d = 9;
        frac[fn++] = '0' + d;
        fp -= d;
    }
    // round last
    while (fn > 0 && frac[fn-1] == '0') fn--;   // trim trailing zeros
    if (fn > 0){
        *p++ = '.';
        for (int i = 0; i < fn; i++) *p++ = frac[i];
    }

    if (sci){
        *p++ = 'e';
        if (ex < 0){ *p++ = '-'; ex = -ex; } else *p++ = '+';
        char eb[8]; int en = 0;
        if (ex == 0) eb[en++] = '0';
        while (ex > 0){ eb[en++] = '0' + (ex % 10); ex /= 10; }
        for (int i = en - 1; i >= 0; i--) *p++ = eb[i];
    }
    *p = '\0';
}

// ---------------------------------------------------------------------------
// Expression editing helpers
// ---------------------------------------------------------------------------
static int slen(const char *s){ int n=0; while(s[n]) n++; return n; }

static void expr_append(const char *s){
    int l = slen(expr), a = slen(s);
    if (l + a >= (int)sizeof(expr) - 1) return;
    for (int i = 0; i < a; i++) expr[l + i] = s[i];
    expr[l + a] = '\0';
}

static void expr_backspace(void){
    int l = slen(expr);
    if (l > 0) expr[l - 1] = '\0';
}

static void expr_clear_entry(void){
    // remove trailing run of digits / '.'
    int l = slen(expr);
    while (l > 0 && ((expr[l-1] >= '0' && expr[l-1] <= '9') || expr[l-1] == '.')) l--;
    expr[l] = '\0';
}

static void result_to_expr(double v){
    fmt_double(v, expr);
}

// ---------------------------------------------------------------------------
// Action dispatch
// ---------------------------------------------------------------------------
static void do_token(const char *tok){
    if (slen(tok) == 1){
        char c = tok[0];
        if ((c>='0'&&c<='9') || c=='.' || c=='+' || c=='-' || c=='*' || c=='/' ||
            c=='^' || c=='(' || c==')' || c=='%'){
            expr_append(tok);
            return;
        }
    }
    if (!__builtin_strcmp(tok, "C"))      { expr[0]='\0'; prevline[0]='\0'; return; }
    if (!__builtin_strcmp(tok, "CE"))     { expr_clear_entry(); return; }
    if (!__builtin_strcmp(tok, "back"))   { expr_backspace(); return; }
    if (!__builtin_strcmp(tok, "pi"))     { expr_append("pi"); return; }
    if (!__builtin_strcmp(tok, "e"))      { expr_append("e"); return; }
    if (!__builtin_strcmp(tok, "mod"))    { expr_append(" mod "); return; }
    if (!__builtin_strcmp(tok, "pct"))    { expr_append("%"); return; }
    if (!__builtin_strcmp(tok, "sqr"))    { expr_append("^2"); return; }
    if (!__builtin_strcmp(tok, "cube"))   { expr_append("^3"); return; }
    if (!__builtin_strcmp(tok, "inv"))    { expr_append("^-1"); return; }
    if (!__builtin_strcmp(tok, "pow"))    { expr_append("^"); return; }
    if (!__builtin_strcmp(tok, "tenx"))   { expr_append("10^"); return; }
    if (!__builtin_strcmp(tok, "expe"))   { expr_append("exp("); return; }
    if (!__builtin_strcmp(tok, "fact"))   { expr_append("!"); return; }
    // prefix functions ending in '('
    {
        int l = slen(tok);
        if (l > 1 && tok[l-1] == '('){ expr_append(tok); return; }
    }
    if (!__builtin_strcmp(tok, "neg")){
        // negate whole expression
        if (slen(expr) == 0) return;
        char buf[160];
        if (expr[0] == '-' && expr[1] == '('){
            // already negated: strip "-(" ... ")"
            int l = slen(expr);
            if (expr[l-1] == ')'){
                int j = 0;
                for (int i = 2; i < l - 1; i++) buf[j++] = expr[i];
                buf[j] = '\0';
                __builtin_memcpy(expr, buf, j + 1);
                return;
            }
        }
        buf[0] = '-'; buf[1] = '(';
        int j = 2, i = 0;
        while (expr[i] && j < (int)sizeof(buf) - 2) buf[j++] = expr[i++];
        buf[j++] = ')'; buf[j] = '\0';
        __builtin_memcpy(expr, buf, j + 1);
        return;
    }
    if (!__builtin_strcmp(tok, "=")){
        int ok; double v = evaluate(expr, &ok);
        if (ok){
            char res[64]; fmt_double(v, res);
            // history: "expr ="
            int j = 0, i = 0;
            while (expr[i] && j < (int)sizeof(prevline) - 3) prevline[j++] = expr[i++];
            prevline[j++] = ' '; prevline[j++] = '='; prevline[j] = '\0';
            __builtin_memcpy(expr, res, slen(res) + 1);
        } else {
            __builtin_memcpy(prevline, "Error", 6);
        }
        return;
    }
    if (!__builtin_strcmp(tok, "2nd")){ second = !second; return; }
    if (!__builtin_strcmp(tok, "deg")){ deg = !deg; return; }
    // memory
    if (!__builtin_strcmp(tok, "MC")){ memory = 0; mem_set = 0; return; }
    if (!__builtin_strcmp(tok, "MR")){ if (mem_set){ char b[64]; fmt_double(memory,b); expr_append(b);} return; }
    if (!__builtin_strcmp(tok, "MS")){ int ok; double v=evaluate(expr,&ok); if(ok){ memory=v; mem_set=1; } return; }
    if (!__builtin_strcmp(tok, "M+")){ int ok; double v=evaluate(expr,&ok); if(ok){ memory+=v; mem_set=1; } return; }
    if (!__builtin_strcmp(tok, "M-")){ int ok; double v=evaluate(expr,&ok); if(ok){ memory-=v; mem_set=1; } return; }
}

// ---------------------------------------------------------------------------
// Geometry (one definition each; draw and hit-test both call these)
// ---------------------------------------------------------------------------
static const btn_t *cur_btns(int *n, int *cols, int *rows){
    if (mode == 1){ *n = SCI_N; *cols = SCI_COLS; *rows = SCI_ROWS; return sci_btns; }
    *n = STD_N; *cols = STD_COLS; *rows = STD_ROWS; return std_btns;
}

// Tab pill i (0 = Standard, 1 = Scientific).
static void tab_rect(int i, int *x, int *y, int *w, int *h){
    *x = PAD + i * (TAB_W + TAB_GAP); *y = PAD; *w = TAB_W; *h = TAB_H;
}

// The two glass panels.
static void disp_rect(int *x, int *y, int *w, int *h){
    *x = PAD; *y = DISP_Y; *w = WIN_W - 2 * PAD; *h = DISP_H;
}
static void keys_rect(int *x, int *y, int *w, int *h){
    *x = PAD; *y = KEY_Y; *w = WIN_W - 2 * PAD; *h = WIN_H - PAD - KEY_Y;
}

// Memory key i (0..4), five equal cells across the grid width.
static void mem_rect(int i, int *x, int *y, int *w, int *h){
    int cw = (GRID_W - 4 * KEY_GAP) / 5;
    *x = GRID_X + i * (cw + KEY_GAP); *y = MEM_Y; *w = cw; *h = MEM_H;
}

// Key grid: fills the keypad panel below the memory row, inset PANEL_IN.
static void cell_geom(int cols, int rows, int *cw, int *ch, int *ox, int *oy){
    int gh = (KEY_Y + (WIN_H - PAD - KEY_Y) - PANEL_IN) - GRID_TOP;
    *cw = (GRID_W - (cols - 1) * KEY_GAP) / cols;
    *ch = (gh - (rows - 1) * KEY_GAP) / rows;
    if (*cw < 1) *cw = 1;   // #436: never let the grid go negative/zero-sized
    if (*ch < 1) *ch = 1;
    *ox = GRID_X;
    *oy = GRID_TOP;
}

static void btn_rect(const btn_t *b, int cols, int rows, int *x, int *y, int *w, int *h){
    int cw, ch, ox, oy; cell_geom(cols, rows, &cw, &ch, &ox, &oy);
    *x = ox + b->gx * (cw + KEY_GAP);
    *y = oy + b->gy * (ch + KEY_GAP);
    *w = b->gw * cw + (b->gw - 1) * KEY_GAP;
    *h = ch;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Centred TTF label. The rasterizer's y is the top of the LINE box, so the
// label is centred on the control by its nominal size (glass doc section 5).
static void text_center(int x, int y, int w, int h, const char *s, int size, uint32_t color){
    int tw = gui_ttf_width(s, size);
    win_draw_text_ttf(window_handle, x + (w - tw) / 2, y + (h - size) / 2 - 1, s, size, color);
}

// (calcglass) One glass panel: soft shadow, AA rounded fill, 1px border, 1px
// top highlight; the same layering the Task Manager's draw_panel() uses.
// Every outer colour is sampled from the backdrop under that edge, so the
// fringe and corners match what the blit put there. Drawn EVERY frame on
// purpose: the same inputs give the same pixels, so the repaint is
// idempotent and there is no static/dynamic chrome split to keep in step
// (section 11: only the backdrop blit itself is a commit).
static void draw_panel(int x, int y, int w, int h){
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    // Mean of the four corner samples: gui_fill_rounded_aa takes ONE outer
    // colour, and the blur keeps the four within a few levels of each other.
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8){
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(window_handle, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(window_handle, x, y, w, h, PANEL_R, C_PANEL, outer);
    gui_rounded_border(window_handle, x, y, w, h, PANEL_R, C_EDGE);
    win_draw_rect(window_handle, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(C_PANEL, 16));
}

// (calcglass) The tab strip: pills sitting directly on the frosted backdrop.
// Selected = accent fill with the accent ink; the rest = panel-coloured glass
// with the panel border (hover lightens the fill). Each pill's AA edge takes
// the backdrop colour at its own centre, and the rectangle is tab_rect(), the
// one the click handler tests.
static void draw_tabs(void){
    static const char *names[2] = {"Standard", "Scientific"};
    for (int i = 0; i < 2; i++){
        int x, y, w, h; tab_rect(i, &x, &y, &w, &h);
        int sel = (mode == i);
        uint32_t outer = bd_at(x + w / 2, y + h / 2);
        uint32_t fill = sel ? C_ACCENT : (hover_tab == i ? gui_lighten(C_PANEL, 18) : C_PANEL);
        gui_fill_rounded_aa(window_handle, x, y, w, h, h / 2, fill, outer);
        if (!sel) gui_rounded_border(window_handle, x, y, w, h, h / 2, C_EDGE);
        text_center(x, y, w, h, names[i], TTF_TAB, sel ? C_ACCENT_INK : C_INK_DIM);
    }
}

static void draw_display(void){
    int px, py, pw, ph; disp_rect(&px, &py, &pw, &ph);
    draw_panel(px, py, pw, ph);
    int left = px + PANEL_IN, right = px + pw - PANEL_IN;

    // history / previous line (top right, dim; "Error" in the error ink)
    if (prevline[0]){
        int is_err = !__builtin_strcmp(prevline, "Error");
        int w = gui_ttf_width(prevline, TTF_TAB);
        win_draw_text_ttf(window_handle, right - w, py + 10, prevline, TTF_TAB, is_err ? C_ERR : C_INK_DIM);
    }

    // mode indicators (top left): DEG/RAD in the dim ink, M and 2nd in the
    // accent so an armed modifier is visible at a glance.
    {
        int ix = left, iy = py + 10;
        const char *dr = deg ? "DEG" : "RAD";
        win_draw_text_ttf(window_handle, ix, iy, dr, TTF_IND, C_INK_DIM);
        ix += gui_ttf_width(dr, TTF_IND) + 8;
        if (mem_set){ win_draw_text_ttf(window_handle, ix, iy, "M", TTF_IND, C_ACCENT); ix += gui_ttf_width("M", TTF_IND) + 8; }
        if (second)  win_draw_text_ttf(window_handle, ix, iy, "2nd", TTF_IND, C_ACCENT);
    }

    // main line: live value if expr parses, else the expression text
    const char *show = expr[0] ? expr : "0";
    char live[64]; int ok = 0;
    if (expr[0]){ double v = evaluate(expr, &ok); if (ok) fmt_double(v, live); }
    const char *big = (expr[0] && ok) ? live : show;

    int by = py + ph - PANEL_IN - TTF_BIG;
    int w = gui_ttf_width(big, TTF_BIG);
    int bx = right - w;
    if (bx < left) bx = left;   // overflow guard (will clip on the right)
    win_draw_text_ttf(window_handle, bx, by, big, TTF_BIG, C_INK);

    // if showing live preview, also echo the typed expression small above it
    if (expr[0] && ok){
        int ew = gui_ttf_width(expr, TTF_TAB);
        win_draw_text_ttf(window_handle, right - ew, by - 16, expr, TTF_TAB, C_INK_DIM);
    }
}

static void draw_memory_row(void){
    static const char *mem[5] = {"MC","MR","M+","M-","MS"};
    for (int i = 0; i < 5; i++){
        int x, y, w, h; mem_rect(i, &x, &y, &w, &h);
        // MC/MR do nothing with an empty memory: dim them (still 3:1 on the panel).
        int idle = (i <= 1 && !mem_set);
        uint32_t fg = idle ? gui_mix(C_INK_DIM, C_PANEL, 140) : C_INK_DIM;
        if (hover_mem == i && !idle){
            gui_fill_rounded_aa(window_handle, x, y, w, h, h / 2, gui_lighten(C_PANEL, 8), C_PANEL);
            fg = C_INK;
        }
        text_center(x, y, w, h, mem[i], TTF_TAB, fg);
    }
}

// (calcglass) One key. Four classes, each from the shared rounded primitives
// (the same composition the Task Manager's tab pills use): digit = raised
// panel fill; function = nested card (DK_CARD_FILL + DK_STROKE_UNSEL); operator
// = nested card with the accent face; "=" = the section 6 primary button
// gradient. An armed 2nd is the selected-pill grammar (accent fill, accent
// ink). Hover lightens the fill by 18, gui_button()'s own rule. gui_button
// itself is not used here because its five variants cannot express four key
// classes with two face sizes, and its per-button soft shadow is 42 shadows
// per frame on the scientific grid.
static void draw_key(const btn_t *b, int x, int y, int w, int h, int hov){
    const char *face = (second && b->face2) ? b->face2 : b->face;
    if (!__builtin_strcmp(b->tok, "deg")) face = deg ? "DEG" : "RAD";
    int on = (!__builtin_strcmp(b->tok, "2nd") && second);

    if (b->kind == K_EQ && !on){
        uint32_t top = C_BTN_TOP, bot = C_BTN_BOTTOM;
        if (hov){ top = gui_lighten(top, 18); bot = gui_lighten(bot, 18); }
        gui_fill_rounded_aa(window_handle, x, y, w, h, KEY_RAD, bot, C_PANEL);
        gui_fill_rounded_grad(window_handle, x + 1, y + 1, w - 2, h - 2, KEY_RAD - 1, top, bot);
        text_center(x, y, w, h, face, TTF_EQ, C_EQ_INK);
        return;
    }
    uint32_t fill, ink; int size, bordered;
    if (on)                    { fill = C_ACCENT;                 ink = C_ACCENT_INK; size = TTF_FUNC;  bordered = 0; }
    else if (b->kind == K_DIGIT){ fill = gui_lighten(C_PANEL, 12); ink = C_INK;        size = TTF_DIGIT; bordered = 0; }
    else if (b->kind == K_OP)  { fill = C_CARD;                   ink = C_ACCENT;     size = TTF_DIGIT; bordered = 1; }
    else                       { fill = C_CARD;                   ink = C_INK_DIM;    size = TTF_FUNC;  bordered = 1; }
    if (hov) fill = gui_lighten(fill, 18);
    gui_fill_rounded_aa(window_handle, x, y, w, h, KEY_RAD, fill, C_PANEL);
    if (bordered) gui_rounded_border(window_handle, x, y, w, h, KEY_RAD, C_EDGE);
    win_draw_rect(window_handle, x + KEY_RAD, y + 1, w - 2 * KEY_RAD, 1, gui_lighten(fill, 16));  // top highlight

    // Stroked faces (see the note above std_btns[]): the ACTIVE token decides,
    // so the sqrt key shows the cube-root glyph while 2nd is armed.
    const char *atok = (second && b->tok2) ? b->tok2 : b->tok;
    int cx = x + w / 2, cy = y + h / 2;
    if (!__builtin_strcmp(atok, "sqrt(") || !__builtin_strcmp(atok, "cbrt(")){
        // radical: short down-stroke, long up-stroke, vinculum, then "x"
        gui_thick_line(window_handle, cx - 13, cy,     cx - 9, cy + 7, 2, ink);
        gui_thick_line(window_handle, cx - 9,  cy + 7, cx - 3, cy - 8, 2, ink);
        gui_thick_line(window_handle, cx - 3,  cy - 8, cx + 9, cy - 8, 1, ink);
        win_draw_text_ttf(window_handle, cx - 1, y + (h - TTF_FUNC) / 2, "x", TTF_FUNC, ink);
        if (atok[0] == 'c') win_draw_text_ttf(window_handle, cx - 18, cy - 14, "3", TTF_IND, ink);
        return;
    }
    if (!__builtin_strcmp(atok, "pi")){
        gui_thick_line(window_handle, cx - 7, cy - 5, cx + 7, cy - 5, 2, ink);   // top bar
        gui_thick_line(window_handle, cx - 4, cy - 5, cx - 4, cy + 7, 2, ink);   // left leg
        gui_thick_line(window_handle, cx + 3, cy - 5, cx + 3, cy + 7, 2, ink);   // right leg
        return;
    }
    if (!__builtin_strcmp(atok, "back")){
        // backspace: left-pointing outline with an x inside
        gui_thick_line(window_handle, cx - 11, cy,     cx - 5,  cy - 6, 1, ink);
        gui_thick_line(window_handle, cx - 5,  cy - 6, cx + 11, cy - 6, 1, ink);
        gui_thick_line(window_handle, cx + 11, cy - 6, cx + 11, cy + 6, 1, ink);
        gui_thick_line(window_handle, cx + 11, cy + 6, cx - 5,  cy + 6, 1, ink);
        gui_thick_line(window_handle, cx - 5,  cy + 6, cx - 11, cy,     1, ink);
        gui_thick_line(window_handle, cx - 1,  cy - 3, cx + 5,  cy + 3, 1, ink);
        gui_thick_line(window_handle, cx + 5,  cy - 3, cx - 1,  cy + 3, 1, ink);
        return;
    }
    text_center(x, y, w, h, face, size, ink);
}

static void draw_buttons(void){
    int n, cols, rows; const btn_t *bs = cur_btns(&n, &cols, &rows);
    for (int i = 0; i < n; i++){
        int x, y, w, h; btn_rect(&bs[i], cols, rows, &x, &y, &w, &h);
        draw_key(&bs[i], x, y, w, h, i == hover);
    }
}

static void draw_all(void){
    // #436: re-sync the live content size from the compositor every frame
    // (same idiom as devmgr/taskmanager/solitaire/irc/glcube). If the window
    // was granted a different size than requested -- including a shorter one
    // on a real screen where the compositor could not honor the full 624px
    // request -- the grid below reflows to fit the ACTUAL window instead of
    // clipping against it.
    { int w = g_win_w, h = g_win_h;
      win_get_size(window_handle, &w, &h);
      if (w < MIN_WIN_W) w = g_win_w > 0 ? g_win_w : MIN_WIN_W;
      if (h < MIN_WIN_H) h = g_win_h > 0 ? g_win_h : MIN_WIN_H;
      g_win_w = w; g_win_h = h;
    }
    // (calcglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md
    // section 11). SYS_WIN_BLIT self-commits: the kernel publishes the window
    // the instant the backdrop lands, and a compositor sample taken between
    // that commit and the win_invalidate() below would show a backdrop with
    // no content on it. So the blit runs ONLY when the chrome is dirty (start,
    // EVENT_RESIZE, EVENT_REDRAW, wallpaper change), never on a hover or a
    // keystroke. Everything else is plain draws, which accumulate unpublished
    // until the single invalidate at the end. The window is never cleared
    // with a flat fill: the panels and pills cover every pixel that changes,
    // and the margins are the backdrop.
    sync_backdrop();
    if (g_chrome_dirty){
        bd_blit(window_handle);
        g_chrome_dirty = 0;
    }
    draw_tabs();
    draw_display();
    { int x, y, w, h; keys_rect(&x, &y, &w, &h); draw_panel(x, y, w, h); }
    draw_memory_row();
    draw_buttons();
    win_invalidate(window_handle);
}

// ---------------------------------------------------------------------------
// Hit testing (the same rect functions the draw path uses)
// ---------------------------------------------------------------------------
static int in_rect(int lx, int ly, int x, int y, int w, int h){
    return lx >= x && lx < x + w && ly >= y && ly < y + h;
}

static int hit_button(int lx, int ly){
    int n, cols, rows; const btn_t *bs = cur_btns(&n, &cols, &rows);
    for (int i = 0; i < n; i++){
        int x, y, w, h; btn_rect(&bs[i], cols, rows, &x, &y, &w, &h);
        if (in_rect(lx, ly, x, y, w, h)) return i;
    }
    return -1;
}

static int hit_tab(int lx, int ly){
    for (int i = 0; i < 2; i++){
        int x, y, w, h; tab_rect(i, &x, &y, &w, &h);
        if (in_rect(lx, ly, x, y, w, h)) return i;
    }
    return -1;
}

static int hit_memory(int lx, int ly){
    for (int i = 0; i < 5; i++){
        int x, y, w, h; mem_rect(i, &x, &y, &w, &h);
        if (in_rect(lx, ly, x, y, w, h)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Keyboard mapping
// ---------------------------------------------------------------------------
static void on_key(char c){
    if (c >= '0' && c <= '9'){ char s[2]={c,0}; expr_append(s); return; }
    switch (c){
        case '.': case '+': case '-': case '*': case '/':
        case '(': case ')': case '^': case '%': { char s[2]={c,0}; expr_append(s); break; }
        case '\n': case '\r': case '=': do_token("="); break;
        case 8: case 127: expr_backspace(); break;
        case 27: expr[0]='\0'; prevline[0]='\0'; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Tool contract (#233) - PROJECTED FROM THE BUTTON TABLES, NOT WRITTEN OUT.
//
// This is the worked example of the completeness invariant in
// libc/contract.h. Nothing below lists a calculator key. The action surface is
// derived by walking std_btns[] and sci_btns[] - the SAME const tables that
// draw_buttons() renders from and that hit_button()/btn_rect() hit-test
// against - plus the five memory tokens the memory row dispatches.
//
// The consequence is the point: a key that is not in those tables cannot be
// drawn, cannot be clicked, and cannot appear in the contract; a key that IS
// there appears in all three automatically. Completeness is structural, so
// there is no lint rule to forget and no second list to update. Adding a
// scientific function to sci_btns[] gives it a contract action in the same
// commit, with no edit here at all.
//
// COVERAGE, stated honestly (see docs/CONTRACT_API.md section 6): this covers
// every token the UI can dispatch. on_key() (the keyboard path) is a strict
// SUBSET of that token set - digits, . + - * / ( ) ^ %, Enter/=, Backspace and
// Escape each map onto a token that is also a button - so the contract covers
// the keyboard too. What is NOT covered, and is deliberately not: hover
// highlighting and window chrome, neither of which is a feature of the app.
// ---------------------------------------------------------------------------
#include "../../libc/contract.h"

#define CT_MAX_TOKS 96

static const char *CT_MEM_TOKS[5] = { "MC", "MR", "M+", "M-", "MS" };

static const char *g_ct_tok[CT_MAX_TOKS];       // distinct action tokens
static const char *g_ct_face[CT_MAX_TOKS];      // the key face that produces it
static char        g_ct_name[CT_MAX_TOKS][16];  // "key.<tok>", stable storage
static int         g_ct_ntok = -1;

// Build the distinct-token list by walking the app's own tables. Both the
// normal token and the 2nd-function token of every button count, because both
// are things the UI can do.
static void ct_build_toks(void) {
    if (g_ct_ntok >= 0) return;
    g_ct_ntok = 0;
    for (int pass = 0; pass < 3; pass++) {
        const btn_t *bs; int n;
        if      (pass == 0) { bs = std_btns; n = STD_N; }
        else if (pass == 1) { bs = sci_btns; n = SCI_N; }
        else                { bs = 0;        n = 5;     }
        for (int i = 0; i < n; i++) {
            const char *toks[2], *faces[2];
            if (bs) {
                toks[0] = bs[i].tok;  faces[0] = bs[i].face;
                toks[1] = bs[i].tok2; faces[1] = bs[i].face2 ? bs[i].face2 : bs[i].face;
            } else {
                toks[0] = CT_MEM_TOKS[i]; faces[0] = CT_MEM_TOKS[i];
                toks[1] = 0;              faces[1] = 0;
            }
            for (int k = 0; k < 2; k++) {
                const char *t = toks[k];
                if (!t || !t[0]) continue;
                int dup = 0;
                for (int j = 0; j < g_ct_ntok; j++)
                    if (!__builtin_strcmp(g_ct_tok[j], t)) { dup = 1; break; }
                if (dup || g_ct_ntok >= CT_MAX_TOKS) continue;
                g_ct_tok[g_ct_ntok]  = t;
                g_ct_face[g_ct_ntok] = faces[k];
                // "key.<tok>". The token is used verbatim: a prettifying
                // translation table would be one more thing to drift.
                g_ct_name[g_ct_ntok][0] = 'k'; g_ct_name[g_ct_ntok][1] = 'e';
                g_ct_name[g_ct_ntok][2] = 'y'; g_ct_name[g_ct_ntok][3] = '.';
                int w = 4;
                for (int q = 0; t[q] && w < 15; q++) g_ct_name[g_ct_ntok][w++] = t[q];
                g_ct_name[g_ct_ntok][w] = '\0';
                g_ct_ntok++;
            }
        }
    }
}

// One action handler for every projected key. The token comes from it->ctx,
// which the projection pointed at the app's own table entry, and it is passed
// straight to do_token() - the SAME function the mouse and keyboard paths
// call. A contract key press is therefore not a parallel implementation of
// pressing the key; it IS pressing the key.
static int ct_key_press(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)argc; (void)argv;
    const char *tok = (const char *)it->ctx;
    if (!tok) return -1;
    do_token(tok);
    // Mirror the mouse path's 2nd-auto-reset so the two cannot diverge.
    if (second && __builtin_strcmp(tok, "2nd")) second = 0;
    strlcpy(out, expr[0] ? expr : "0", (size_t)ocap);
    return 0;
}

static int calc_project(int idx, ct_item_t *out) {
    ct_build_toks();
    if (idx < 0 || idx >= g_ct_ntok) return 0;
    // The desc buffer is shared and is valid only until the next projection
    // call. contract.c uses desc immediately (describe) and never retains it.
    static char desc[96];
    snprintf(desc, sizeof(desc), "Calculator key \"%s\" (token %s), via do_token()",
             g_ct_face[idx] ? g_ct_face[idx] : g_ct_tok[idx], g_ct_tok[idx]);

    ct_item_t it;
    __builtin_memset(&it, 0, sizeof(it));
    it.name   = g_ct_name[idx];
    it.type   = CT_ACTION;
    it.access = CT_WRITE;
    it.risk   = CT_SAFE;      // a calculator key is reversible and touches nothing outside the app
    it.actfn  = ct_key_press;
    it.ctx    = g_ct_tok[idx];
    it.desc   = desc;
    *out = it;
    return 1;
}

// ---- readable state and mode, backed by the app's own variables ----------
static int ct_display_get(char *o, int n) { strlcpy(o, expr[0] ? expr : "0", (size_t)n); return 0; }
static int ct_history_get(char *o, int n) { strlcpy(o, prevline, (size_t)n); return 0; }
static int ct_memory_get(char *o, int n)  { char b[64]; fmt_double(memory, b); strlcpy(o, b, (size_t)n); return 0; }

static const ct_item_t CALC_ITEMS[] = {
    // `mode` is the variable the tab click handler assigns to, so this row and
    // the tabs are the same store, not two.
    { "mode", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 1, "Standard|Scientific",
      &mode, 0, 0, 0, 0, 0, 0,
      "Keypad mode; the same variable the Standard/Scientific tabs set" },
    { "display", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, ct_display_get, 0, 0,
      "The current expression/result shown in the display" },
    { "history", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, ct_history_get, 0, 0,
      "The previous expression line shown above the display" },
    { "memory", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, ct_memory_get, 0, 0,
      "Calculator memory register value (set through the MS/M+/M- keys)" },
    { "memory_set", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      &mem_set, 0, 0, 0, 0, 0, 0,
      "Whether the memory register currently holds a value" },
    { "second", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      &second, 0, 0, 0, 0, 0, 0,
      "Whether the 2nd modifier is active (set through the 2nd key)" },
    { "degrees", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      &deg, 0, 0, 0, 0, 0, 0,
      "Trig angle unit: 1 degrees, 0 radians (set through the DEG key)" },
    { "error", CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0,
      &err, 0, 0, 0, 0, 0, 0,
      "Whether the last evaluation raised an error (domain, divide by zero)" },
};

static const ct_contract_t CALC_CONTRACT = {
    "calc", "Calculator",
    "Key actions are PROJECTED from std_btns[]/sci_btns[], the same tables "
    "draw_buttons() and hit_button() use, so the contract cannot omit a key "
    "that exists or offer one that does not.",
    CALC_ITEMS, (int)(sizeof(CALC_ITEMS) / sizeof(CALC_ITEMS[0])),
    calc_project,
    0,   // no persisted state to load
    0    // nothing to commit: do_token() already mutated the live state
};

int main(int argc, char **argv){
    // #233: a contract invocation must never open a window. Answering here,
    // before win_create(), is what makes the API usable from a headless test
    // harness and from the AI tool loop without a compositor.
    if (contract_is_invocation(argc, argv))
        return contract_cli(argc, argv, &CALC_CONTRACT);

    window_handle = win_create("Calculator", 280, 90, WIN_W, WIN_H);
    if (window_handle < 0){ printf("calc: failed to create window\n"); return 1; }

    // (calcglass) the palette is the fixed dark glass (see the token block), so
    // there is no theme poll: the window carries its own backdrop. draw_all()
    // starts with g_chrome_dirty set, so the first frame blits the backdrop.
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running){
        int et = win_get_event(window_handle, &ev, 100);
        if (et == 0) continue;

        switch (ev.type){
            case EVENT_RESIZE:
                // (calcglass) a resize reallocates the content buffer, so the
                // backdrop must be blitted again: chrome dirty.
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; }
                g_chrome_dirty = 1;
                draw_all();
                break;
            case EVENT_REDRAW:
                g_chrome_dirty = 1;
                draw_all();
                break;
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
            case EVENT_KEY_DOWN:
                on_key(ev.key_char);
                draw_all();
                break;
            case EVENT_MOUSE_MOVE: {
                int wx, wy; win_get_pos(window_handle, &wx, &wy);
                int lx = ev.mouse_x;
                int ly = ev.mouse_y;
                int nb = hit_button(lx, ly);
                int nt = hit_tab(lx, ly);
                int nm = hit_memory(lx, ly);
                if (nb != hover || nt != hover_tab || nm != hover_mem){ hover = nb; hover_tab = nt; hover_mem = nm; draw_all(); }
                break;
            }
            case EVENT_MOUSE_UP:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT){
                    int wx, wy; win_get_pos(window_handle, &wx, &wy);
                    int lx = ev.mouse_x;
                    int ly = ev.mouse_y;
                    int t = hit_tab(lx, ly);
                    if (t >= 0){ mode = t; hover = -1; draw_all(); break; }
                    int m = hit_memory(lx, ly);
                    if (m >= 0){ const char *mm[5]={"MC","MR","M+","M-","MS"}; do_token(mm[m]); draw_all(); break; }
                    int bi = hit_button(lx, ly);
                    if (bi >= 0){
                        int n, cols, rows; const btn_t *bs = cur_btns(&n, &cols, &rows);
                        const btn_t *b = &bs[bi];
                        const char *tok = (second && b->tok2) ? b->tok2 : b->tok;
                        do_token(tok);
                        // 2nd auto-resets after one function use (Win behavior), except toggles
                        if (second && __builtin_strcmp(b->tok,"2nd")) second = 0;
                        draw_all();
                    }
                }
                break;
            default:
                break;
        }
    }

    win_destroy(window_handle);
    return 0;
}
