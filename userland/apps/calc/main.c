// calc - Windows 11 style Calculator for MayteraOS (Standard + Scientific)
//
// A flat, dark, modern calculator inspired by the Windows 11 Calculator: a
// large result display with an expression line above it, a memory row, mode
// tabs (Standard / Scientific) and a flat button grid. Scientific mode adds
// trig, logs, powers, roots, factorial, constants and parentheses, with a
// 2nd toggle for inverse functions and a DEG/RAD toggle.
//
// The userland C library is freestanding (no libm and no %f in printf), so
// this file carries its own small double-precision math library and an
// expression evaluator (recursive descent with operator precedence).

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"   // #280: theme-aware colors

// #301: route all in-window text through the antialiased TrueType path so the
// calculator matches the modern look of Settings/Files/Task Manager (which all
// render via win_draw_text_ttf). The bitmap-font win_draw_text is replaced here.
#define win_draw_text(h, x, y, s, c)        win_draw_text_ttf((h), (x), (y), (s), 15, (c))
#define win_draw_text_small(h, x, y, s, c)  win_draw_text_ttf((h), (x), (y), (s), 12, (c))
#define CALC_TTF_BTN   15   // button-face / general text size
#define CALC_TTF_SMALL 12   // history / indicator size
#define CALC_TTF_BIG   30   // large result display size

// ---------------------------------------------------------------------------
// Layout / palette (Windows 11 dark)
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
#define PAD          8

// #280: colors are theme-driven (see apply_theme); defaults are the dark theme.
static uint32_t COL_BG     = 0x00202020;
static uint32_t COL_DISP   = 0x00202020;
static uint32_t COL_DIGIT  = 0x003B3B3B;
static uint32_t COL_FUNC   = 0x00323232;
static uint32_t COL_HOVER  = 0x004A4A4A;
static uint32_t COL_EQ     = 0x00005FB8;
static uint32_t COL_EQ_HOV = 0x000072D8;
static uint32_t COL_TAB_ON = 0x00383838;
static uint32_t COL_TEXT   = 0x00FFFFFF;
static uint32_t COL_DIM    = 0x00A0A0A0;
static uint32_t COL_BORDER = 0x00161616;
static int g_last_theme = -1;
static void apply_theme(void){
    int kt = get_theme();   // 1=Dark 2=Light 4=Classic 5=Ocean 9=Nord
    switch (kt) {
    case 2: // Light
        COL_BG=0x00F0F0F0; COL_DISP=0x00FFFFFF; COL_DIGIT=0x00FFFFFF; COL_FUNC=0x00E4E4E4;
        COL_HOVER=0x00D8D8D8; COL_EQ=0x00005FB8; COL_EQ_HOV=0x000072D8; COL_TAB_ON=0x00D0D0D0;
        COL_TEXT=0x00202020; COL_DIM=0x00707070; COL_BORDER=0x00B0B0B0; break;
    case 4: // Classic gray
        COL_BG=0x00C0C0C0; COL_DISP=0x00FFFFFF; COL_DIGIT=0x00C0C0C0; COL_FUNC=0x00B0B0B0;
        COL_HOVER=0x00D0D0D0; COL_EQ=0x00000080; COL_EQ_HOV=0x000000A0; COL_TAB_ON=0x00A0A0A0;
        COL_TEXT=0x00000000; COL_DIM=0x00505050; COL_BORDER=0x00000000; break;
    case 5: // Ocean
        COL_BG=0x001A3A4A; COL_DISP=0x00183040; COL_DIGIT=0x00224455; COL_FUNC=0x001E4050;
        COL_HOVER=0x00305060; COL_EQ=0x0040C0E0; COL_EQ_HOV=0x0050D0F0; COL_TAB_ON=0x00305060;
        COL_TEXT=0x00E0F0FF; COL_DIM=0x0090B0C0; COL_BORDER=0x000E2733; break;
    case 9: // Nord
        COL_BG=0x002E3440; COL_DISP=0x002B303B; COL_DIGIT=0x003B4252; COL_FUNC=0x00343B49;
        COL_HOVER=0x00434C5E; COL_EQ=0x0088C0D0; COL_EQ_HOV=0x0098D0E0; COL_TAB_ON=0x00434C5E;
        COL_TEXT=0x00ECEFF4; COL_DIM=0x00AEB6C5; COL_BORDER=0x0020242E; break;
    default: // Dark
        COL_BG=0x00202020; COL_DISP=0x00202020; COL_DIGIT=0x003B3B3B; COL_FUNC=0x00323232;
        COL_HOVER=0x004A4A4A; COL_EQ=0x00005FB8; COL_EQ_HOV=0x000072D8; COL_TAB_ON=0x00383838;
        COL_TEXT=0x00FFFFFF; COL_DIM=0x00A0A0A0; COL_BORDER=0x00161616; break;
    }
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

// Standard: 4 columns x 6 rows
static const btn_t std_btns[] = {
    {"%",  0,"pct",0,  0,0,1,K_FUNC}, {"CE",0,"CE",0, 1,0,1,K_FUNC}, {"C",0,"C",0, 2,0,1,K_FUNC}, {"\x7f",0,"back",0, 3,0,1,K_FUNC},
    {"1/x",0,"inv",0, 0,1,1,K_FUNC}, {"x\xfd",0,"sqr",0, 1,1,1,K_FUNC}, {"\xfb""x",0,"sqrt(",0, 2,1,1,K_FUNC}, {"\xf6",0,"/",0, 3,1,1,K_OP},
    {"7",0,"7",0, 0,2,1,K_DIGIT}, {"8",0,"8",0, 1,2,1,K_DIGIT}, {"9",0,"9",0, 2,2,1,K_DIGIT}, {"x",0,"*",0, 3,2,1,K_OP},
    {"4",0,"4",0, 0,3,1,K_DIGIT}, {"5",0,"5",0, 1,3,1,K_DIGIT}, {"6",0,"6",0, 2,3,1,K_DIGIT}, {"-",0,"-",0, 3,3,1,K_OP},
    {"1",0,"1",0, 0,4,1,K_DIGIT}, {"2",0,"2",0, 1,4,1,K_DIGIT}, {"3",0,"3",0, 2,4,1,K_DIGIT}, {"+",0,"+",0, 3,4,1,K_OP},
    {"+/-",0,"neg",0, 0,5,1,K_DIGIT}, {"0",0,"0",0, 1,5,1,K_DIGIT}, {".",0,".",0, 2,5,1,K_DIGIT}, {"=",0,"=",0, 3,5,1,K_EQ},
};
#define STD_N ((int)(sizeof(std_btns)/sizeof(std_btns[0])))
#define STD_COLS 4
#define STD_ROWS 6

// Scientific: 6 columns x 7 rows
static const btn_t sci_btns[] = {
    {"2nd",0,"2nd",0, 0,0,1,K_FUNC}, {"\xe3",0,"pi",0, 1,0,1,K_FUNC}, {"e",0,"e",0, 2,0,1,K_FUNC}, {"C",0,"C",0, 3,0,1,K_FUNC}, {"CE",0,"CE",0, 4,0,1,K_FUNC}, {"\x7f",0,"back",0, 5,0,1,K_FUNC},
    {"sin","sin\xc4","sin(","asin(", 0,1,1,K_FUNC}, {"x\xfd","x\xfc","sqr","cube", 1,1,1,K_FUNC}, {"1/x",0,"inv",0, 2,1,1,K_FUNC}, {"|x|",0,"abs(",0, 3,1,1,K_FUNC}, {"exp",0,"exp(",0, 4,1,1,K_FUNC}, {"\xf6",0,"/",0, 5,1,1,K_OP},
    {"cos","cos\xc4","cos(","acos(", 0,2,1,K_FUNC}, {"\xfb""x","\xfb\xfd","sqrt(","cbrt(", 1,2,1,K_FUNC}, {"(",0,"(",0, 2,2,1,K_FUNC}, {")",0,")",0, 3,2,1,K_FUNC}, {"n!",0,"fact",0, 4,2,1,K_FUNC}, {"x",0,"*",0, 5,2,1,K_OP},
    {"tan","tan\xc4","tan(","atan(", 0,3,1,K_FUNC}, {"x\xfe",0,"pow",0, 1,3,1,K_FUNC}, {"7",0,"7",0, 2,3,1,K_DIGIT}, {"8",0,"8",0, 3,3,1,K_DIGIT}, {"9",0,"9",0, 4,3,1,K_DIGIT}, {"-",0,"-",0, 5,3,1,K_OP},
    {"log",0,"log(",0, 0,4,1,K_FUNC}, {"10\xfe",0,"tenx",0, 1,4,1,K_FUNC}, {"4",0,"4",0, 2,4,1,K_DIGIT}, {"5",0,"5",0, 3,4,1,K_DIGIT}, {"6",0,"6",0, 4,4,1,K_DIGIT}, {"+",0,"+",0, 5,4,1,K_OP},
    {"ln","e\xfe","ln(","expe", 0,5,1,K_FUNC}, {"%",0,"pct",0, 1,5,1,K_FUNC}, {"1",0,"1",0, 2,5,1,K_DIGIT}, {"2",0,"2",0, 3,5,1,K_DIGIT}, {"3",0,"3",0, 4,5,1,K_DIGIT}, {"mod",0,"mod",0, 5,5,1,K_OP},
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
// Rendering
// ---------------------------------------------------------------------------
static void draw_text_center(int cx, int cy, const char *s, uint32_t color){
    int w = gui_ttf_width(s, CALC_TTF_BTN);
    win_draw_text_ttf(window_handle, cx - w / 2, cy, s, CALC_TTF_BTN, color);
}

static const btn_t *cur_btns(int *n, int *cols, int *rows){
    if (mode == 1){ *n = SCI_N; *cols = SCI_COLS; *rows = SCI_ROWS; return sci_btns; }
    *n = STD_N; *cols = STD_COLS; *rows = STD_ROWS; return std_btns;
}

// Grid geometry
static int grid_top(void){ return 158; }
static void cell_geom(int cols, int rows, int *cw, int *ch, int *ox, int *oy){
    int gap = 5;
    int gw = WIN_W - 2 * PAD;
    int gh = WIN_H - grid_top() - PAD;
    *cw = (gw - (cols - 1) * gap) / cols;
    *ch = (gh - (rows - 1) * gap) / rows;
    if (*cw < 1) *cw = 1;   // #436: never let the grid go negative/zero-sized
    if (*ch < 1) *ch = 1;
    *ox = PAD;
    *oy = grid_top();
}

static void btn_rect(const btn_t *b, int cols, int rows, int *x, int *y, int *w, int *h){
    int cw, ch, ox, oy; cell_geom(cols, rows, &cw, &ch, &ox, &oy);
    int gap = 5;
    *x = ox + b->gx * (cw + gap);
    *y = oy + b->gy * (ch + gap);
    *w = b->gw * cw + (b->gw - 1) * gap;
    *h = ch;
}

static void draw_tabs(void){
    int tw = 110, th = 26, ty = 6;
    // Standard
    win_draw_rect(window_handle, PAD, ty, tw, th, (mode==0||hover_tab==0)?COL_TAB_ON:COL_BG);
    draw_text_center(PAD + tw/2, ty + (th-FONT_HEIGHT)/2, "Standard", mode==0?COL_TEXT:COL_DIM);
    // Scientific
    win_draw_rect(window_handle, PAD + tw + 6, ty, tw, th, (mode==1||hover_tab==1)?COL_TAB_ON:COL_BG);
    draw_text_center(PAD + tw + 6 + tw/2, ty + (th-FONT_HEIGHT)/2, "Scientific", mode==1?COL_TEXT:COL_DIM);
    // accent underline for active tab
    int ax = (mode==0) ? PAD : PAD + tw + 6;
    win_draw_rect(window_handle, ax + 8, ty + th - 2, tw - 16, 2, COL_EQ);
}

static void draw_display(void){
    int dy = 38, dh = 96;
    win_draw_rect(window_handle, 0, dy, WIN_W, dh, COL_DISP);

    // history / previous line (small, right aligned, dim)
    if (prevline[0]){
        int w = gui_ttf_width(prevline, CALC_TTF_SMALL);
        win_draw_text_small(window_handle, WIN_W - PAD - w, dy + 6, prevline, COL_DIM);
    }

    // mode indicators (DEG/RAD, 2nd, M) on the left, small
    char ind[24]; int k = 0;
    const char *dr = deg ? "DEG" : "RAD";
    for (int i = 0; dr[i]; i++) ind[k++] = dr[i];
    if (mem_set){ ind[k++]=' '; ind[k++]='M'; }
    if (second){ ind[k++]=' '; ind[k++]='2'; ind[k++]='n'; ind[k++]='d'; }
    ind[k] = '\0';
    win_draw_text_small(window_handle, PAD, dy + 6, ind, COL_DIM);

    // main line: live value if expr parses, else the expression text
    const char *show = expr[0] ? expr : "0";
    char live[64]; int ok = 0;
    if (expr[0]){ double v = evaluate(expr, &ok); if (ok) fmt_double(v, live); }
    const char *big = (expr[0] && ok) ? live : show;

    int w = gui_ttf_width(big, CALC_TTF_BIG);
    int bx = WIN_W - PAD - w;
    if (bx < PAD) bx = PAD;   // overflow guard (will clip on the left)
    win_draw_text_ttf(window_handle, bx, dy + dh - CALC_TTF_BIG - 6, big, CALC_TTF_BIG, COL_TEXT);

    // if showing live preview, also echo the typed expression small above it
    if (expr[0] && ok){
        int ew = gui_ttf_width(expr, CALC_TTF_SMALL);
        win_draw_text_small(window_handle, WIN_W - PAD - ew, dy + dh - 40, expr, COL_DIM);
    }
}

static void draw_memory_row(void){
    const char *mem[5] = {"MC","MR","M+","M-","MS"};
    int y = 138, h = 18;
    int gap = 4, w = (WIN_W - 2*PAD - 4*gap) / 5;
    for (int i = 0; i < 5; i++){
        int x = PAD + i * (w + gap);
        uint32_t fg = (i==0 && !mem_set) ? 0x00606060 : COL_DIM;
        draw_text_center(x + w/2, y + (h-FONT_HEIGHT)/2, mem[i], fg);
    }
}

static void draw_buttons(void){
    int n, cols, rows; const btn_t *bs = cur_btns(&n, &cols, &rows);
    for (int i = 0; i < n; i++){
        const btn_t *b = &bs[i];
        int x, y, w, h; btn_rect(b, cols, rows, &x, &y, &w, &h);
        uint32_t base;
        switch (b->kind){
            case K_DIGIT: base = COL_DIGIT; break;
            case K_OP:    base = COL_FUNC;  break;
            case K_EQ:    base = COL_EQ;    break;
            default:      base = COL_FUNC;  break;
        }
        if (i == hover) base = (b->kind == K_EQ) ? COL_EQ_HOV : COL_HOVER;
        // active toggles
        if (!__builtin_strcmp(b->tok, "2nd") && second) base = COL_EQ;
        win_draw_rect(window_handle, x, y, w, h, base);
        win_draw_rect(window_handle, x, y, w, 1, 0x18FFFFFF);   // subtle top highlight
        const char *face = (second && b->face2) ? b->face2 : b->face;
        if (!__builtin_strcmp(b->tok, "deg")) face = deg ? "DEG" : "RAD";
        uint32_t tc = (b->kind == K_EQ) ? COL_TEXT : COL_TEXT;
        draw_text_center(x + w/2, y + (h - FONT_HEIGHT)/2, face, tc);
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
    win_draw_rect(window_handle, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_tabs();
    draw_display();
    draw_memory_row();
    draw_buttons();
    win_invalidate(window_handle);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
static int hit_button(int lx, int ly){
    int n, cols, rows; const btn_t *bs = cur_btns(&n, &cols, &rows);
    for (int i = 0; i < n; i++){
        int x, y, w, h; btn_rect(&bs[i], cols, rows, &x, &y, &w, &h);
        if (lx >= x && lx < x + w && ly >= y && ly < y + h) return i;
    }
    return -1;
}

static int hit_tab(int lx, int ly){
    int tw = 110, th = 26, ty = 6;
    if (ly >= ty && ly < ty + th){
        if (lx >= PAD && lx < PAD + tw) return 0;
        if (lx >= PAD + tw + 6 && lx < PAD + tw + 6 + tw) return 1;
    }
    return -1;
}

static int hit_memory(int lx, int ly){
    int y = 138, h = 18, gap = 4, w = (WIN_W - 2*PAD - 4*gap) / 5;
    if (ly >= y && ly < y + h){
        for (int i = 0; i < 5; i++){
            int x = PAD + i * (w + gap);
            if (lx >= x && lx < x + w) return i;
        }
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

int main(int argc, char **argv){
    (void)argc; (void)argv;

    window_handle = win_create("Calculator", 280, 90, WIN_W, WIN_H);
    if (window_handle < 0){ printf("calc: failed to create window\n"); return 1; }

    apply_theme();
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running){
        { int th = get_theme(); if (th != g_last_theme) { g_last_theme = th; apply_theme(); draw_all(); } }
        int et = win_get_event(window_handle, &ev, 100);
        if (et == 0) continue;

        switch (ev.type){
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; }
                draw_all();
                break;
            case EVENT_REDRAW:
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
                if (nb != hover || nt != hover_tab){ hover = nb; hover_tab = nt; draw_all(); }
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
