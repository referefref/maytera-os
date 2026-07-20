// clock.c - Digital clock state + rendering for the MayteraOS compositor.
//
// The digital clock is a NORMAL desktop widget (id 5): the widget framework in
// widgets.c owns its position / drag / lock / right-click menu and calls
// digclk_geom() + digclk_draw() during the normal widget pass.
//
// Design: NO box / card. The time is drawn as large, thin typography directly
// on the desktop with a soft drop shadow for legibility on any wallpaper. The
// user picks one of several layout VARIATIONS (g_digclk_style) from the widget
// right-click menu, alongside 12/24-hour and seconds toggles. All persisted in
// the UI profile. clock_render() is kept as a no-op for legacy callers.

#include "compositor.h"
#include "../../libc/syscall.h"

int g_show_digclock  = 0;     // visible (toggled from the widgets tray menu)
int g_digclk_x       = -1;    // top-left x (-1 = default on first render)
int g_digclk_y       = -1;    // top-left y
int g_digclk_locked  = 0;     // per-widget drag lock (persisted)
int g_digclk_12h     = 0;     // 0 = 24-hour, 1 = 12-hour (persisted)
int g_digclk_secs    = 1;     // show seconds (persisted)
int g_digclk_style   = 1;     // 0=big line, 1=bighours(default), 2=secs sup, 3=stacked, 4=time+date

#define DIGCLK_STYLES 5

// Type sizes (TTF points) for the typographic layouts.
#define SZ_BIG    46
#define SZ_STACK  40
#define SZ_DATE   16
#define SZ_SEC    18
#define SZ_AMPM   15
// Big-hours layout (style 1): tall hour digits on the left, a 3-line block
// (min/sec, weekday, month-day) stacked to their right spanning the hour height.
#define SZ_HOURS  56
#define SZ_MIN    20
#define SZ_SM     16
#define GAP_HB    8

// Colors: light ink + dark shadow so the time reads on any wallpaper (no box).
#define DIGCLK_INK    0xFFF2F2F6
#define DIGCLK_DIM    0xFFC8CCD4
#define DIGCLK_SHADOW 0xFF14161C
#define SHADOW_DX 2
#define SHADOW_DY 2

static const char *wday3[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *wdayfull[] = {"Sunday","Monday","Tuesday","Wednesday",
                                 "Thursday","Friday","Saturday"};
static const char *mon3[]  = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};

static void fmt_2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + (v % 10); }

// Zeller-ish day-of-week, 0=Sunday.
static int digclk_dow(int d, int m, int y) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    int w = (y + y/4 - y/100 + y/400 + t[(m-1) & 15] + d) % 7;
    return (w < 0) ? w + 7 : w;
}

// Large text with a soft drop shadow (no card). 2 TTF draws.
static void halo(int x, int y, const char *s, int size, uint32_t fg) {
    draw_text_ttf(x + SHADOW_DX, y + SHADOW_DY, s, size, DIGCLK_SHADOW);
    draw_text_ttf(x, y, s, size, fg);
}

static void get_hms(int *hh, int *mm, int *ss, const char **ampm) {
    long rtc = sys_get_rtc_time();
    int h = (int)((rtc >> 16) & 0xFF);
    int m = (int)((rtc >> 8) & 0xFF);
    int s = (int)(rtc & 0xFF);
    *ampm = "";
    if (g_digclk_12h) {
        *ampm = (h >= 12) ? "PM" : "AM";
        h = h % 12; if (h == 0) h = 12;
    }
    *hh = h; *mm = m; *ss = s;
}

// "HH:MM" (+":SS" when withsecs). Returns buf.
static char *hm_str(char *buf, int withsecs) {
    int hh, mm, ss; const char *ap;
    get_hms(&hh, &mm, &ss, &ap);
    int i = 0;
    fmt_2(buf + i, hh); i += 2;
    buf[i++] = ':'; fmt_2(buf + i, mm); i += 2;
    if (withsecs) { buf[i++] = ':'; fmt_2(buf + i, ss); i += 2; }
    buf[i] = '\0';
    return buf;
}

static char *sec_str(char *buf) {
    int hh, mm, ss; const char *ap;
    get_hms(&hh, &mm, &ss, &ap);
    fmt_2(buf, ss); buf[2] = '\0';
    return buf;
}

// "MM SS" (or "MM" when seconds off) - the right-block top line for style 1.
__attribute__((unused)) static char *minsec_str(char *buf) {
    int hh, mm, ss; const char *ap;
    get_hms(&hh, &mm, &ss, &ap);
    int i = 0;
    fmt_2(buf + i, mm); i += 2;
    if (g_digclk_secs) { buf[i++] = ' '; fmt_2(buf + i, ss); i += 2; }
    buf[i] = '\0';
    return buf;
}

// "Mon DD" (no weekday) - the right-block bottom line for style 1.
__attribute__((unused)) static char *monthday_str(char *buf) {
    int d = 19, m = 6, y = 2026;
    get_rtc_date(&d, &m, &y);
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    int i = 0;
    const char *mo = mon3[m-1];
    for (int k = 0; mo[k]; k++) buf[i++] = mo[k];
    buf[i++] = ' ';
    if (d >= 10) buf[i++] = '0' + (d/10);
    buf[i++] = '0' + (d%10);
    buf[i] = '\0';
    return buf;
}

// Full weekday name for the current date (style 1 middle line).
__attribute__((unused)) static const char *weekday_full(void) {
    int d = 19, m = 6, y = 2026;
    get_rtc_date(&d, &m, &y);
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    return wdayfull[digclk_dow(d, m, y) & 7];
}

static char *date_str(char *buf) {
    int d = 19, m = 6, y = 2026;
    get_rtc_date(&d, &m, &y);
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    int w = digclk_dow(d, m, y);
    int i = 0;
    const char *wd = wday3[w & 7];
    for (int k = 0; wd[k]; k++) buf[i++] = wd[k];
    buf[i++] = ','; buf[i++] = ' ';
    const char *mo = mon3[m-1];
    for (int k = 0; mo[k]; k++) buf[i++] = mo[k];
    buf[i++] = ' ';
    if (d >= 10) buf[i++] = '0' + (d/10);
    buf[i++] = '0' + (d%10);
    buf[i] = '\0';
    return buf;
}

// Compute the typographic bounding box of the active layout (for hit-testing).
void digclk_geom(int *w, int *h) {
    char t[16], dt[24], sc[4]; const char *ap;
    int hh, mm, ss; get_hms(&hh, &mm, &ss, &ap);
    int ww = 0, hgt = SZ_BIG + 6;
    switch (g_digclk_style) {
        case 1: { // all-large HH:MM:SS, no date (default)
            hm_str(t, g_digclk_secs);
            ww = text_width_ttf(t, SZ_HOURS);
            if (g_digclk_12h) ww += text_width_ttf(ap, SZ_AMPM) + 6;
            hgt = SZ_HOURS;
        } break;
        case 2: { // big time, small seconds superscript
            hm_str(t, 0);
            ww = text_width_ttf(t, SZ_BIG);
            if (g_digclk_secs) ww += text_width_ttf(sec_str(sc), SZ_SEC) + 4;
            if (g_digclk_12h)  ww += text_width_ttf(ap, SZ_AMPM) + 6;
            hgt = SZ_BIG + 6;
        } break;
        case 3: { // stacked HH over MM
            char hb[4], mb[4]; fmt_2(hb, hh); hb[2]=0; fmt_2(mb, mm); mb[2]=0;
            int a = text_width_ttf(hb, SZ_STACK), b = text_width_ttf(mb, SZ_STACK);
            ww = (a > b) ? a : b;
            if (g_digclk_secs) ww += text_width_ttf("00", SZ_SEC) + 6;
            hgt = SZ_STACK * 2 + 4;
        } break;
        case 4: { // time + date under it (legacy style)
            hm_str(t, g_digclk_secs);
            int tw = text_width_ttf(t, SZ_BIG);
            int dw = text_width_ttf(date_str(dt), SZ_DATE);
            ww = (tw > dw) ? tw : dw;
            hgt = SZ_BIG + SZ_DATE + 6;
        } break;
        default: { // 0: big single line
            hm_str(t, g_digclk_secs);
            ww = text_width_ttf(t, SZ_BIG);
            if (g_digclk_12h) ww += text_width_ttf(ap, SZ_AMPM) + 6;
            hgt = SZ_BIG + 6;
        } break;
    }
    if (w) *w = ww + 6;
    if (h) *h = hgt;
}

void digclk_draw(int x, int y) {
    char t[16], dt[24], sc[4]; const char *ap;
    int hh, mm, ss; get_hms(&hh, &mm, &ss, &ap);

    switch (g_digclk_style) {
        case 1: { // all-large HH:MM:SS, no date (default)
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_HOURS, DIGCLK_INK);
            if (g_digclk_12h) {
                int tw = text_width_ttf(t, SZ_HOURS);
                draw_text_ttf(x + tw + 6, y + SZ_HOURS - SZ_AMPM - 4, ap, SZ_AMPM, DIGCLK_DIM);
            }
        } break;
        case 2: { // big time, small seconds as superscript at top-right
            hm_str(t, 0);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            int tw = text_width_ttf(t, SZ_BIG);
            int sx = x + tw + 4;
            if (g_digclk_secs) {
                sec_str(sc);
                draw_text_ttf(sx + 1, y + 1, sc, SZ_SEC, DIGCLK_SHADOW);
                draw_text_ttf(sx, y, sc, SZ_SEC, DIGCLK_DIM);
                sx += text_width_ttf(sc, SZ_SEC) + 6;
            }
            if (g_digclk_12h)
                draw_text_ttf(sx, y + SZ_BIG - SZ_AMPM - 2, ap, SZ_AMPM, DIGCLK_DIM);
        } break;
        case 3: { // stacked HH / MM
            char hb[4], mb[4]; fmt_2(hb, hh); hb[2]=0; fmt_2(mb, mm); mb[2]=0;
            halo(x, y, hb, SZ_STACK, DIGCLK_INK);
            halo(x, y + SZ_STACK, mb, SZ_STACK, DIGCLK_INK);
            if (g_digclk_secs) {
                int mw = text_width_ttf(mb, SZ_STACK);
                fmt_2(sc, ss); sc[2]=0;
                draw_text_ttf(x + mw + 6, y + SZ_STACK + SZ_STACK - SZ_SEC - 2,
                              sc, SZ_SEC, DIGCLK_DIM);
            }
        } break;
        case 4: { // time (big) + date (small, dim) under it (legacy style)
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            if (g_digclk_12h) {
                int tw = text_width_ttf(t, SZ_BIG);
                draw_text_ttf(x + tw + 6, y + SZ_BIG - SZ_AMPM - 2, ap, SZ_AMPM, DIGCLK_DIM);
            }
            date_str(dt);
            draw_text_ttf(x + 1, y + SZ_BIG + 1, dt, SZ_DATE, DIGCLK_SHADOW);
            draw_text_ttf(x, y + SZ_BIG, dt, SZ_DATE, DIGCLK_DIM);
        } break;
        default: { // 0: big single line
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            if (g_digclk_12h) {
                int tw = text_width_ttf(t, SZ_BIG);
                draw_text_ttf(x + tw + 6, y + SZ_BIG - SZ_AMPM - 2, ap, SZ_AMPM, DIGCLK_DIM);
            }
        } break;
    }
}

void clock_render(void) { }   // legacy entry: the widget framework draws it now
