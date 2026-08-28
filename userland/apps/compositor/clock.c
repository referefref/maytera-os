// clock.c - Digital clock state + rendering for the MayteraOS compositor.
//
// The digital clock is a NORMAL desktop widget (id 5): the widget framework in
// widgets.c owns its position / drag / lock / right-click menu and calls
// digclk_geom() + digclk_draw() during the normal widget pass.
//
// Design: NO box / card. The time is drawn as large, thin typography directly
// on the desktop with a soft drop shadow for legibility on any wallpaper. The
// user picks one of several layout VARIATIONS (g_digclk_style) from the widget
// right-click menu, alongside a seconds toggle; both persist in the UI profile.
// The 12/24-hour FORMAT is deliberately NOT one of them - see digclk_12h()
// below (#235). clock_render() is kept as a no-op for legacy callers.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/tz.h"   // #49: THE local-clock helper; never read the RTC here
#include "../../libc/settingscfg.h"  // #235: THE 12/24-hour preference reader

int g_show_digclock  = 1;     // visible (toggled from the widgets tray menu)
                               // default ON, 2026-08-18 owner decision: OOBE
                               // dock/widgets page (setup/main.rs
                               // WIDGETS_DEFAULT_MASK) reads this compiled-in
                               // value via UIPROFIL.YML on a fresh install
                               // (profile_load()+profile_save() at
                               // compositor_init() run before the wizard),
                               // so this IS the fresh-install default, not
                               // just a fallback - keep it in lockstep with
                               // WIDX_DIGCLOCK there.
int g_digclk_x       = -1;    // top-left x (-1 = default on first render)
int g_digclk_y       = -1;    // top-left y
int g_digclk_locked  = 0;     // per-widget drag lock (persisted)
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
// #uiscale: this whole widget was ALREADY scale-ready by construction (every
// dimension below is measured with text_width_ttf()/derived from SZ_* TTF
// point sizes, both of which already scale at the draw_text_ttf chokepoint
// as long as g_ui_scale_native_text is 0) - see digclk_draw()/digclk_geom()
// being pulled OUT of widgets_render()'s native-text opt-out, in widgets.c.
// Only these small hand-picked pixel gaps needed a literal ui_px() wrap.
#define SHADOW_DX ui_px(2)
#define SHADOW_DY ui_px(2)

static const char *wday3[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *wdayfull[] = {"Sunday","Monday","Tuesday","Wednesday",
                                 "Thursday","Friday","Saturday"};
static const char *mon3[]  = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};

static void fmt_2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + (v % 10); }

// #235: THE 12/24-hour choice. It is /CONFIG/SETTINGS.CFG key 'h', owned by the
// Settings app's contract row clock.use_24hour, and read here through the ONE
// shared throttled reader libc/settingscfg.c (#230) - the same reader the
// taskbar clock's tb_clock_str() uses, so the desktop clock and the taskbar
// clock can no longer disagree.
//
// WHAT WAS HERE BEFORE, so nobody re-adds it. This file carried its own
// `int g_digclk_12h`, persisted separately as "dc12" in UIPROFIL.YML and
// toggled from this widget's own right-click menu. The Settings control
// therefore governed the taskbar clock and NOT the widget: a user who switched
// to 12-hour got a 12-hour bar clock and a 24-hour desktop clock at the same
// time. That is a second, private description of a preference a shared one
// already governs, which is the exact fault #230 removed elsewhere; the #233
// contract API surfaced it on its FIRST live run (docs/CONTRACT_API.md s7).
// The widget menu no longer offers a private switch - its "Date & Time..."
// item deep-links to the one control in Settings, which both clocks follow.
static int digclk_12h(void) { return !settingscfg_use24h(); }

// #50: the private day-of-week routine that used to live here is gone; tz_wday()
// in libc/tz.c is THE one, shared with widgets.c's calendar which had its own
// separate Zeller copy. Two implementations of the same congruence is exactly
// the pattern these tickets are about.

// Large text with a soft drop shadow (no card). 2 TTF draws.
static void halo(int x, int y, const char *s, int size, uint32_t fg) {
    draw_text_ttf(x + SHADOW_DX, y + SHADOW_DY, s, size, DIGCLK_SHADOW);
    draw_text_ttf(x, y, s, size, fg);
}

// #49: LOCAL time, not the raw RTC. tz_local_hms() applies the user's chosen
// zone (which the RTC does not carry: the RTC is UTC). Every clock this file
// feeds - the digital desktop widget AND, via lock_clock_hms/_date below, the
// lock screen and the login screen - therefore follows the setting.
static void get_hms(int *hh, int *mm, int *ss, const char **ampm) {
    int h = 0, m = 0, s = 0;
    tz_local_hms(&h, &m, &s);
    *ampm = "";
    if (digclk_12h()) {
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
    tz_local_date(&d, &m, &y);
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
    tz_local_date(&d, &m, &y);
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    return wdayfull[tz_wday(d, m, y) & 7];
}

static char *date_str(char *buf) {
    int d = 19, m = 6, y = 2026;
    tz_local_date(&d, &m, &y);
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    int w = tz_wday(d, m, y);
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
//
// #uiscale: `ww` is already scale-correct wherever it comes straight from
// text_width_ttf() - that call measures the ACTUAL rendered (scaled) glyphs,
// same chokepoint as draw_text_ttf(). What was NOT scale-correct is every
// literal added to it (small inter-block gaps: +6, +4) and every `hgt`,
// which was the raw LOGICAL point size used directly as a pixel height (a
// standalone-length usage, never itself passed through the chokepoint at
// this exact spot) - ui_px() is the correct wrapper for both per
// docs/UI_SCALE.md ("Use ui_px() for a coordinate or a standalone length").
// Wrapping the WHOLE 1x literal expression (e.g. ui_px(SZ_BIG + 6), not
// ui_px(SZ_BIG) + 6) keeps every case byte-identical to its old value at
// 100% (ui_px is the identity there) while scaling proportionally above it.
void digclk_geom(int *w, int *h) {
    char t[16], dt[24], sc[4]; const char *ap;
    int hh, mm, ss; get_hms(&hh, &mm, &ss, &ap);
    int ww = 0, hgt = ui_px(SZ_BIG + 6);
    switch (g_digclk_style) {
        case 1: { // all-large HH:MM:SS, no date (default)
            hm_str(t, g_digclk_secs);
            ww = text_width_ttf(t, SZ_HOURS);
            if (digclk_12h()) ww += text_width_ttf(ap, SZ_AMPM) + ui_px(6);
            hgt = ui_px(SZ_HOURS);
        } break;
        case 2: { // big time, small seconds superscript
            hm_str(t, 0);
            ww = text_width_ttf(t, SZ_BIG);
            if (g_digclk_secs) ww += text_width_ttf(sec_str(sc), SZ_SEC) + ui_px(4);
            if (digclk_12h())  ww += text_width_ttf(ap, SZ_AMPM) + ui_px(6);
            hgt = ui_px(SZ_BIG + 6);
        } break;
        case 3: { // stacked HH over MM
            char hb[4], mb[4]; fmt_2(hb, hh); hb[2]=0; fmt_2(mb, mm); mb[2]=0;
            int a = text_width_ttf(hb, SZ_STACK), b = text_width_ttf(mb, SZ_STACK);
            ww = (a > b) ? a : b;
            if (g_digclk_secs) ww += text_width_ttf("00", SZ_SEC) + ui_px(6);
            hgt = ui_px(SZ_STACK * 2 + 4);
        } break;
        case 4: { // time + date under it (legacy style)
            hm_str(t, g_digclk_secs);
            int tw = text_width_ttf(t, SZ_BIG);
            int dw = text_width_ttf(date_str(dt), SZ_DATE);
            ww = (tw > dw) ? tw : dw;
            hgt = ui_px(SZ_BIG + SZ_DATE + 6);
        } break;
        default: { // 0: big single line
            hm_str(t, g_digclk_secs);
            ww = text_width_ttf(t, SZ_BIG);
            if (digclk_12h()) ww += text_width_ttf(ap, SZ_AMPM) + ui_px(6);
            hgt = ui_px(SZ_BIG + 6);
        } break;
    }
    if (w) *w = ww + ui_px(6);
    if (h) *h = hgt;
}

void digclk_draw(int x, int y) {
    char t[16], dt[24], sc[4]; const char *ap;
    int hh, mm, ss; get_hms(&hh, &mm, &ss, &ap);

    switch (g_digclk_style) {
        case 1: { // all-large HH:MM:SS, no date (default)
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_HOURS, DIGCLK_INK);
            if (digclk_12h()) {
                int tw = text_width_ttf(t, SZ_HOURS);
                draw_text_ttf(x + tw + ui_px(6), y + ui_px(SZ_HOURS) - ui_px(SZ_AMPM) - ui_px(4),
                              ap, SZ_AMPM, DIGCLK_DIM);
            }
        } break;
        case 2: { // big time, small seconds as superscript at top-right
            hm_str(t, 0);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            int tw = text_width_ttf(t, SZ_BIG);
            int sx = x + tw + ui_px(4);
            if (g_digclk_secs) {
                sec_str(sc);
                draw_text_ttf(sx + ui_px(1), y + ui_px(1), sc, SZ_SEC, DIGCLK_SHADOW);
                draw_text_ttf(sx, y, sc, SZ_SEC, DIGCLK_DIM);
                sx += text_width_ttf(sc, SZ_SEC) + ui_px(6);
            }
            if (digclk_12h())
                draw_text_ttf(sx, y + ui_px(SZ_BIG) - ui_px(SZ_AMPM) - ui_px(2), ap, SZ_AMPM, DIGCLK_DIM);
        } break;
        case 3: { // stacked HH / MM
            char hb[4], mb[4]; fmt_2(hb, hh); hb[2]=0; fmt_2(mb, mm); mb[2]=0;
            halo(x, y, hb, SZ_STACK, DIGCLK_INK);
            halo(x, y + ui_px(SZ_STACK), mb, SZ_STACK, DIGCLK_INK);
            if (g_digclk_secs) {
                int mw = text_width_ttf(mb, SZ_STACK);
                fmt_2(sc, ss); sc[2]=0;
                draw_text_ttf(x + mw + ui_px(6),
                              y + ui_px(SZ_STACK) + ui_px(SZ_STACK) - ui_px(SZ_SEC) - ui_px(2),
                              sc, SZ_SEC, DIGCLK_DIM);
            }
        } break;
        case 4: { // time (big) + date (small, dim) under it (legacy style)
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            if (digclk_12h()) {
                int tw = text_width_ttf(t, SZ_BIG);
                draw_text_ttf(x + tw + ui_px(6), y + ui_px(SZ_BIG) - ui_px(SZ_AMPM) - ui_px(2), ap, SZ_AMPM, DIGCLK_DIM);
            }
            date_str(dt);
            draw_text_ttf(x + ui_px(1), y + ui_px(SZ_BIG) + ui_px(1), dt, SZ_DATE, DIGCLK_SHADOW);
            draw_text_ttf(x, y + ui_px(SZ_BIG), dt, SZ_DATE, DIGCLK_DIM);
        } break;
        default: { // 0: big single line
            hm_str(t, g_digclk_secs);
            halo(x, y, t, SZ_BIG, DIGCLK_INK);
            if (digclk_12h()) {
                int tw = text_width_ttf(t, SZ_BIG);
                draw_text_ttf(x + tw + ui_px(6), y + ui_px(SZ_BIG) - ui_px(SZ_AMPM) - ui_px(2), ap, SZ_AMPM, DIGCLK_DIM);
            }
        } break;
    }
}

void clock_render(void) { }   // legacy entry: the widget framework draws it now

// #566: thin public wrappers around this file's own static RTC formatters, so
// the lock screen's big live clock (lockscreen.c) reuses the exact same
// time/date formatting instead of a second hand-rolled copy. Independent of
// the clock format/g_digclk_secs (the lock screen always shows 24h + seconds, the
// liveness indicator per docs/SECURE_LOGIN_DESIGN.md section 3.6/4.3).
void lock_clock_hms(char *buf, int with_secs) {
    // #49: goes STRAIGHT to tz_local_hms() rather than through get_hms(). The
    // comment above has always said this readout is independent of
    // digclk_12h(), but routing it through get_hms() meant a user who set the
    // desktop widget to 12-hour also got a 12-hour lock screen with no AM/PM
    // marker (get_hms() returns the marker separately and this function
    // dropped it), i.e. an ambiguous clock. Now the claim is true.
    int hh = 0, mm = 0, ss = 0;
    tz_local_hms(&hh, &mm, &ss);
    int i = 0;
    fmt_2(buf + i, hh); i += 2;
    buf[i++] = ':'; fmt_2(buf + i, mm); i += 2;
    if (with_secs) { buf[i++] = ':'; fmt_2(buf + i, ss); i += 2; }
    buf[i] = '\0';
}
void lock_clock_date(char *buf) {
    date_str(buf);
}
