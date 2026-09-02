// profile.c - persist UI settings to ~/.ui-profile.yaml (#92).
// The compositor reads the profile on launch and applies it (theme, wallpaper,
// fonts, display fx, screensaver, volume, widgets, sheep), and writes it back
// whenever any of those change (captures both Settings and tray-menu edits), so
// the look is per-user and survives reboots.

#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/pwd.h"
#include "../../libc/stdio.h"      // (#profbase) the save-failure report
#include "../../libc/unistd.h"
#include "../../libc/userconf.h"  // (#profbase2) userconf_write_all(): no destructive
                                    // unlink-before-open, full write + fsync + checked close
#include "../../libc/dock_opacity.h"  // #132: shared DOCK_OPACITY_MIN/MAX/DEFAULT
// #deskicons: ui_px()/ui_unpx() for the logical<->physical icon-position
// round trip below. compositor.h itself is NOT includable here (see
// profile_current_username()'s comment further down re: the bool typedef
// clash with pwd.h/unistd.h), but uiscale.h only pulls in syscall.h and has
// no such conflict.
#include "../../libc/uiscale.h"

extern int g_show_clock, g_show_calendar, g_sheep_enabled;
extern int g_aichat_enabled;   // #185
extern int g_show_digclock;
extern int g_show_weather, g_show_crypto, g_show_stocks;
// #274 new widgets
extern int g_show_sysmon, g_sysmon_x, g_sysmon_y, g_sysmon_locked;
extern int g_show_timer, g_timer_x, g_timer_y, g_timer_locked;
extern int g_show_worldtime, g_worldtime_x, g_worldtime_y, g_worldtime_locked;
extern int g_show_uptime, g_uptime_x, g_uptime_y, g_uptime_locked;
extern int g_show_ha, g_ha_x, g_ha_y, g_ha_locked;   // #414 Home Assistant widget
extern int g_ha_mode, g_ha_min, g_ha_max;            // #419 HA display mode + gauge range
#ifndef WT_ZONES
#define WT_ZONES 3
#endif
extern int g_wt_off[WT_ZONES];
extern int g_show_stickies;   // #270 sticky-notes master visibility
extern int g_weather_x, g_weather_y, g_crypto_x, g_crypto_y, g_stocks_x, g_stocks_y;
extern int g_weather_locked, g_crypto_locked, g_stocks_locked;
extern int g_weather_verbose, g_crypto_verbose, g_stocks_verbose;
extern int g_sheep_speed, g_sheep_size, g_sheep_style, g_sheep_count;
extern int g_dog_enabled;
extern int g_brightness, g_nightlight;   // main.c
extern int g_win_opacity;                // main.c
extern int g_clock_cx, g_clock_cy, g_cal_x, g_cal_y;  // widgets.c positions
extern int g_clock_locked, g_cal_locked;             // widgets.c lock flags
extern int g_digclk_x, g_digclk_y, g_digclk_locked, g_digclk_secs, g_digclk_style; // clock.c
extern int g_cursor_style, g_cursor_size;  // main.c (#116)
extern int g_dock_style;                   // taskbar.c (#387 dock layout)
extern int g_dock_opacity;                 // draw.c (#745 glass opacity, percent OPAQUE)
extern int g_dock_height;                  // taskbar.c (#123 marble dock height px)
extern int g_dock_zoom;                    // taskbar.c (#123 hover zoom percent)

// NOTE: the FAT driver is 8.3-only (no long/leading-dot names), so the profile
// is stored as <home>/UIPROFIL.YML rather than the literal ~/.ui-profile.yaml.
// (LFN support to use the exact name is tracked separately.)
static void prof_path(char *out) {
    struct passwd *pw = getpwuid(getuid());
    const char *d = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/";
    int i = 0;
    if (!(d[0] == '/' && d[1] == '\0')) {        // home is not just "/"
        while (d[i] && i < 70) { out[i] = d[i]; i++; }
        if (i > 0 && out[i-1] == '/') i--;        // strip trailing slash
    }
    const char *f = "/UIPROFIL.YML"; int j = 0;
    while (f[j] && i < 110) out[i++] = f[j++];
    out[i] = '\0';
}

// Current logged-in session username, e.g. for keying the Start menu's
// per-user config directory (startmenu_model.rs's user layer, see startmenu.c
// sm_user_fragment_dir()). Exposed here rather than open-coded in startmenu.c
// because startmenu.c includes compositor.h, whose unguarded
// `typedef int bool;` conflicts with libc/types.h's `typedef _Bool bool;`
// pulled in by pwd.h/unistd.h (the same landmine documented at the top of
// startmenu.c re: gui_scroll.h) - profile.c has no such conflict, so the
// getpwuid() call belongs here, with just the resulting name string crossing
// the file boundary. Writes an empty string (never a placeholder name) if the
// session identity cannot be resolved, so callers see "no user identity" as
// exactly that rather than a synthetic default.
void profile_current_username(char *out, int outsz) {
    if (outsz <= 0) return;
    out[0] = '\0';
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_name || !pw->pw_name[0]) return;
    int i = 0;
    while (pw->pw_name[i] && i < outsz - 1) { out[i] = pw->pw_name[i]; i++; }
    out[i] = '\0';
}

static int prof_atoi(const char *s) {
    int v = 0, neg = 0; while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// Desktop icon position accessors (desktop.c).
int  desktop_icon_count(void);
void desktop_get_icon_pos(int idx, int *x, int *y);
void desktop_set_icon_pos(int idx, int x, int y);
// (#231) desktop_positions_hash() is gone: profile_tick() hashes
// profile_build()'s own serialized bytes now, see profile_build()'s comment.
int  desktop_builtin_count(void);                                  // (#745)
int  desktop_icon_key(int idx, char *out, int cap);                // (#745)
void desktop_set_icon_pos_by_key(const char *key, int axis, int v);// (#745)
void desktop_place_unplaced(void);                                 // (#745)

// (#745) HOW ICON POSITIONS ARE KEYED, AND HOW AN EXISTING DESKTOP MIGRATES.
//
// They used to be "ico<N>x"/"ico<N>y", keyed by the icon's INDEX. That is only
// correct while the icon list is a fixed compiled-in array. Now that icons also
// come from <home>/DESKTOP, adding or deleting ONE file renumbers every icon
// after it, and every saved coordinate would be applied to the wrong icon. The
// new form is "ic<key>x"/"ic<key>y", where <key> is the icon's stable identity
// ('s'+app basename, or 'u'+file name+hash) and always begins 's' or 'u'.
//
// MIGRATION IS A READ-PATH RULE, not a conversion pass:
//   READ  - both forms are accepted. A legacy "ico<N>" key is applied ONLY when
//           N is below desktop_builtin_count(), i.e. only to the system icons
//           those keys were actually written for. That is what preserves an
//           existing user's arrangement of Computer / Recycle Bin / Terminal /
//           Settings / Browser / App Store exactly as they left it.
//   WRITE - always the new form. The first save after upgrade rewrites the file
//           and the "ico<N>" lines simply stop being emitted.
// So no arrangement is discarded, nothing needs a "have I migrated" flag that
// could be wrong, and there is no half-completed one-shot copy to recover from.
//
// The bounded legacy read is also what stops a stale "ico6" (written when a
// Start-menu "Add to Desktop" icon happened to sit at index 6 in some earlier
// session) from being applied to whatever file now occupies index 6.

// Parse keys of the form "ico<N>x" / "ico<N>y" into an icon index + axis.
// Returns 1 on a match (sets *idx and *axis: 0=x, 1=y), 0 otherwise.
static int parse_icon_key(const char *k, int *idx, int *axis) {
    if (!(k[0] == 'i' && k[1] == 'c' && k[2] == 'o')) return 0;
    const char *p = k + 3;
    int n = 0, any = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; any = 1; }
    if (!any) return 0;
    if (p[0] == 'x' && p[1] == '\0') { *idx = n; *axis = 0; return 1; }
    if (p[0] == 'y' && p[1] == '\0') { *idx = n; *axis = 1; return 1; }
    return 0;
}

static void prof_apply(const char *k, int v) {
    int icoi, axis;
    if (parse_icon_key(k, &icoi, &axis)) {
        // (#745) LEGACY form, bounded to the system icons it was written for.
        if (icoi >= desktop_builtin_count()) return;
        int x = 0, y = 0;
        desktop_get_icon_pos(icoi, &x, &y);
        // #deskicons: every stored icon coordinate is a LOGICAL (100%-basis)
        // pixel, converted to the CURRENT screen's physical pixels here on
        // load - see the matching ui_unpx() at the write side in
        // profile_build() below, and the top-of-file note for why this needs
        // no format bump or migration flag. At 100% ui_px() is the identity,
        // so a profile saved before UI scaling existed (necessarily at 100%)
        // reads back byte-identical.
        if (axis == 0) x = ui_px(v); else y = ui_px(v);
        desktop_set_icon_pos(icoi, x, y);
        return;
    }
    // (#745) Current form: "ic" + <key> + axis, where <key> starts 's' or 'u'.
    // Disjoint from the legacy "ico<digit>" form by construction.
    if (k[0] == 'i' && k[1] == 'c' && (k[2] == 's' || k[2] == 'u')) {
        int L = 0; while (k[L]) L++;
        char ax = (L >= 4) ? k[L - 1] : 0;
        if (ax == 'x' || ax == 'y') {
            char kk[24]; int m = 0;
            for (int i = 2; i < L - 1 && m < (int)sizeof(kk) - 1; i++) kk[m++] = k[i];
            kk[m] = '\0';
            // #deskicons: v is LOGICAL; ui_px() to physical before applying.
            desktop_set_icon_pos_by_key(kk, (ax == 'x') ? 0 : 1, ui_px(v));
            return;
        }
    }
    if      (!strcmp(k, "theme"))        set_theme(v);
    else if (!strcmp(k, "wallpaper"))    set_wallpaper(v);
    else if (!strcmp(k, "font_size"))    set_font_size(v);
    else if (!strcmp(k, "icon_size"))    set_icon_size(v);
    else if (!strcmp(k, "screensaver"))  set_screensaver(v);
    else if (!strcmp(k, "volume"))       set_volume(v);
    // (#231r) The five graphic-EQ band positions, 0..100 with 50 flat, on
    // exactly the path "volume" above already uses. #231's complaint about
    // the old EQ was, verbatim, that it "is not even persisted"; this is
    // that half. Because profile_tick()'s change hash is an FNV-1a over
    // profile_build()'s REAL serialized bytes (#231 fixed it to be, replacing
    // a hand-maintained key list), adding the write below is ALSO what puts
    // these in the change hash - so they cannot land in the "applies live and
    // vanishes at reboot" hole that the same ticket found for
    // g_clock_locked/g_cal_locked/g_digclk_style.
    //
    // An unknown-band write is refused by the kernel, so an old or
    // hand-edited profile carrying more bands than this build has is ignored
    // rather than mis-applied.
    else if (k[0] == 'e' && k[1] == 'q' && k[2] >= '0' && k[2] <= '9' && k[3] == 0)
        eq_band_set(k[2] - '0', v);
    else if (!strcmp(k, "winopacity"))   { g_win_opacity = v; set_win_opacity(v); }
    else if (!strcmp(k, "clkx"))         g_clock_cx = v;
    else if (!strcmp(k, "clky"))         g_clock_cy = v;
    else if (!strcmp(k, "calx"))         g_cal_x = v;
    else if (!strcmp(k, "caly"))         g_cal_y = v;
    else if (!strcmp(k, "clklk"))        g_clock_locked = v;
    else if (!strcmp(k, "callk"))        g_cal_locked = v;
    else if (!strcmp(k, "dcx"))          g_digclk_x = v;
    else if (!strcmp(k, "dcy"))          g_digclk_y = v;
    else if (!strcmp(k, "dclk"))         g_digclk_locked = v;
    // #235: "dc12" (the digital clock's private 12/24-hour flag) is no longer
    // read or written. The format is /CONFIG/SETTINGS.CFG key 'h', owned by
    // Settings' clock.use_24hour row and read through libc/settingscfg.c; a
    // second persisted copy here is what let the two clocks disagree. An old
    // profile still carrying dc12= falls through this chain and is ignored,
    // which is correct: it names a preference this file no longer owns.
    else if (!strcmp(k, "dcsec"))        g_digclk_secs = v;
    else if (!strcmp(k, "dcsty"))        g_digclk_style = v;
    else if (!strcmp(k, "curstyle"))     g_cursor_style = v;   // (#116)
    else if (!strcmp(k, "cursize"))      g_cursor_size = v;    // (#116)
    else if (!strcmp(k, "wxx"))          g_weather_x = v;
    else if (!strcmp(k, "wxy"))          g_weather_y = v;
    else if (!strcmp(k, "crx"))          g_crypto_x = v;
    else if (!strcmp(k, "cry"))          g_crypto_y = v;
    else if (!strcmp(k, "stx"))          g_stocks_x = v;
    else if (!strcmp(k, "sty"))          g_stocks_y = v;
    else if (!strcmp(k, "wxlk"))         g_weather_locked = v;
    else if (!strcmp(k, "crlk"))         g_crypto_locked = v;
    else if (!strcmp(k, "stlk"))         g_stocks_locked = v;
    else if (!strcmp(k, "show_weather")) g_show_weather = v;
    else if (!strcmp(k, "show_crypto"))  g_show_crypto = v;
    else if (!strcmp(k, "show_stocks"))  g_show_stocks = v;
    else if (!strcmp(k, "wxvb"))         g_weather_verbose = v;
    else if (!strcmp(k, "crvb"))         g_crypto_verbose = v;
    else if (!strcmp(k, "stvb"))         g_stocks_verbose = v;
    else if (!strcmp(k, "brightness"))   g_brightness = v;
    else if (!strcmp(k, "nightlight"))   g_nightlight = v;
    else if (!strcmp(k, "show_digclock")) g_show_digclock = v;
    else if (!strcmp(k, "show_clock"))   g_show_clock = v;
    else if (!strcmp(k, "show_calendar")) g_show_calendar = v;
    else if (!strcmp(k, "sheep"))        g_sheep_enabled = v;
    else if (!strcmp(k, "sheep_speed"))  g_sheep_speed = v;
    else if (!strcmp(k, "sheep_size"))   g_sheep_size = v;
    else if (!strcmp(k, "sheep_style"))  g_sheep_style = v;
    else if (!strcmp(k, "sheep_count"))  g_sheep_count = v;
    else if (!strcmp(k, "dog"))          g_dog_enabled = v;
    // #274 system monitor / timer / world time
    else if (!strcmp(k, "show_sysmon"))    g_show_sysmon = v;
    else if (!strcmp(k, "smx"))            g_sysmon_x = v;
    else if (!strcmp(k, "smy"))            g_sysmon_y = v;
    else if (!strcmp(k, "smlk"))           g_sysmon_locked = v;
    else if (!strcmp(k, "show_timer"))     g_show_timer = v;
    else if (!strcmp(k, "tmx"))            g_timer_x = v;
    else if (!strcmp(k, "tmy"))            g_timer_y = v;
    else if (!strcmp(k, "tmlk"))           g_timer_locked = v;
    else if (!strcmp(k, "show_worldtime")) g_show_worldtime = v;
    else if (!strcmp(k, "wtx"))            g_worldtime_x = v;
    else if (!strcmp(k, "wty"))            g_worldtime_y = v;
    else if (!strcmp(k, "wtlk"))           g_worldtime_locked = v;
    else if (!strcmp(k, "show_uptime"))    g_show_uptime = v;
    else if (!strcmp(k, "upx"))            g_uptime_x = v;
    else if (!strcmp(k, "upy"))            g_uptime_y = v;
    else if (!strcmp(k, "uplk"))           g_uptime_locked = v;
    else if (!strcmp(k, "show_ha"))        g_show_ha = v;
    else if (!strcmp(k, "hax"))            g_ha_x = v;
    else if (!strcmp(k, "hay"))            g_ha_y = v;
    else if (!strcmp(k, "halk"))           g_ha_locked = v;
    else if (!strcmp(k, "hamode"))         g_ha_mode = v;
    else if (!strcmp(k, "hamin"))          g_ha_min = v;
    else if (!strcmp(k, "hamax"))          g_ha_max = v;
    else if (!strcmp(k, "wtz0"))           g_wt_off[0] = v;
    else if (!strcmp(k, "wtz1"))           g_wt_off[1] = v;
    else if (!strcmp(k, "wtz2"))           g_wt_off[2] = v;
    else if (!strcmp(k, "show_stickies"))  g_show_stickies = v;
    else if (!strcmp(k, "show_aichat"))     g_aichat_enabled = v;
    else if (!strcmp(k, "dock_style"))      g_dock_style = (v >= 0 && v < 5) ? v : 0;  // #387 (#26: 5 = DOCK_COUNT in compositor.h, not included here)
    // #745 glass opacity, percent OPAQUE. The floor is DERIVED, not chosen, and
    // moved 60 -> 70 (dockgrey, 2026-08-12) in the same change that lightened
    // CLR_GLASS_TINT's dark-branch derivation (main.c glass_theme_apply(), 58%
    // -> 78%, so the marble dock/taskbar read as dark grey rather than black,
    // per user report). A lighter tint at a low opacity lets more of a bright
    // wallpaper wash through the glass, so the safe floor moved with it: a full
    // black/white backdrop sweep against every glass-enabled shipped theme's
    // OWN ink (readable_ink(taskbar_bg), see draw.c) puts the worst case
    // (Ocean, white backdrop) at 4.62:1 for op=70 with the new tint, clearing
    // WCAG AA's 4.5:1 text floor with a small margin - see
    // /tmp/dockgrey_harness.py, do not hand-recompute this. A corrupt or
    // out-of-range value falls back to the DOCK_OPACITY_DEFAULT rather than to
    // 0, which would render an invisible dock.
    // (#132) The valid RANGE accepted from a persisted file widened to
    // DOCK_OPACITY_MIN..MAX; 70 stopped being the floor everywhere else, so a
    // profile saved under the new, lower value must not get silently reset to
    // 75 the next time it loads - that would be the exact bug this ticket
    // exists to fix, reappearing one boot later.
    else if (!strcmp(k, "dock_opacity"))    g_dock_opacity = (v >= DOCK_OPACITY_MIN && v <= DOCK_OPACITY_MAX) ? v : DOCK_OPACITY_DEFAULT;
    // (#123 items 2/3) Marble dock geometry. The clamps here are DELIBERATE and
    // are the same numbers taskbar.c's xfce_dock_h()/xfce_dock_zoom() enforce at
    // the point of use (XFCE_DOCK_H_MIN/MAX, XFCE_DOCK_ZOOM_MIN/MAX); they are
    // repeated rather than shared because profile.c does not include taskbar.c's
    // private geometry block, and a value that is out of range in the FILE is a
    // different event from one that is out of range at use. An out-of-range
    // value falls back to the default, matching dock_opacity above and for the
    // same reason: 0 would render an invisible dock and 0% zoom is not a legal
    // magnification. Note the substitution is SILENT here (consistent with every
    // other key in this function); the visible signal is that Settings' slider
    // shows the default rather than the bad number.
    else if (!strcmp(k, "dock_height"))     g_dock_height = (v >= 44 && v <= 96) ? v : 59;
    else if (!strcmp(k, "dock_zoom"))       g_dock_zoom = (v >= 100 && v <= 200) ? v : 128;
    // Mouse sensitivity (1-10): the kernel is the live authority (SYS_SET_MOUSE_SPEED,
    // applied to every PS/2 + USB delta). Seed it from the persisted profile on load.
    else if (!strcmp(k, "mouse_sens"))      set_mouse_speed((v >= 1 && v <= 10) ? v : 7);
}

void profile_load(void) {
    char path[120]; prof_path(path);
    int fd = sys_open(path, 0);
    if (fd < 0) fd = sys_open("/UIPROFIL.YML", 0);
    if (fd < 0) return;
    // (#745) 5120, matching profile_save()'s buffer: the icon list is now
    // dynamic (up to DESKTOP_ICON_MAX entries, two lines each), so a profile
    // written by profile_save() must fit through the reader too. A short read
    // here would silently drop the tail of the file, which is the last lines,
    // which is exactly the icon positions.
    static char buf[5120];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int i = 0;
    while (buf[i]) {
        char key[28]; int ks = 0;
        while (buf[i] && buf[i] != ':' && buf[i] != '\n') { if (ks < 27) key[ks++] = buf[i]; i++; }
        while (ks > 0 && (key[ks-1] == ' ' || key[ks-1] == '\t')) ks--;
        key[ks] = '\0';
        if (buf[i] == ':') {
            i++;
            char val[28]; int vs = 0;
            while (buf[i] && buf[i] != '\n') { if (vs < 27) val[vs++] = buf[i]; i++; }
            val[vs] = '\0';
            if (key[0]) prof_apply(key, prof_atoi(val));
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
    }
    set_display_fx(g_brightness, g_nightlight);   // apply combined display fx
}
// (#745) NOTE: the second-pass icon placement is NOT called from in here. It is
// called by the caller, immediately after profile_load() returns, because
// profile_load() has three early returns (no file, empty file, unreadable) and
// a placement pass that only runs on the success path would leave a first-boot
// machine, the exact case with no profile at all, without one.

static char *put_kv(char *p, const char *k, int v) {
    while (*k) *p++ = *k++;
    *p++ = ':'; *p++ = ' ';
    char t[12]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    if (neg) *p++ = '-';
    while (n) *p++ = t[--n];
    *p++ = '\n';
    return p;
}

// (#745) The old icon_key() builder ("ico<N><axis>") is gone with the writer
// that used it. parse_icon_key() above stays, because the READ path must still
// understand a profile written by an older build.

// Serialize every persisted key into buf (caller-owned, must be >= 5120
// bytes - see profile_save()'s old comment on that size, unchanged) and
// return the number of bytes written. This is the ONE place the set of
// persisted keys is defined.
//
// (#231) profile_tick() used to keep a SECOND, hand-maintained list of these
// same values (a weighted sum with its own private prime per key) purely to
// detect whether anything had changed. That is the same defect shape as a
// multi-copy constant: nothing forces the two lists to agree, so a key added
// here (put_kv'd, loaded back by prof_apply()) could silently be missing from
// the other list. Three were: g_clock_locked, g_cal_locked and
// g_digclk_style ("Lock" on the analog clock/calendar widgets, and digital
// clock "Next design") applied live and read back correctly within the
// session, but were never in the change-detection hash, so profile_save()
// was never triggered for them and they reverted at the next boot.
//
// The fix is to stop maintaining a second list at all: profile_tick() now
// calls THIS function and hashes the bytes it actually produces (see
// profile_hash_buf() below), so change detection is derived from the real
// serialized output instead of a parallel accounting that has to be updated
// by hand every time a key is added. A key that is missing from this
// function is missing from the FILE too (profile_save() calls it as well),
// which is a visibly different and much easier bug to find than "saves fine,
// forgets on next boot".
static int profile_build(char *buf) {
    char *p = buf;
    p = put_kv(p, "theme",        get_theme());
    p = put_kv(p, "wallpaper",    get_wallpaper());
    p = put_kv(p, "font_size",    get_font_size());
    p = put_kv(p, "icon_size",    get_icon_size());
    p = put_kv(p, "screensaver",  get_screensaver());
    p = put_kv(p, "volume",       get_volume());
    // (#231r) One key per graphic-EQ band, read back from the kernel's live
    // DSP state (there is no compositor-side copy to go stale). The count
    // comes from the kernel too, so a build with a different number of bands
    // writes a different number of keys rather than silently truncating.
    {
        int nb = eq_band_count();
        if (nb < 0) nb = 0;
        if (nb > 9) nb = 9;          // one digit, matching prof_apply's parse
        char ek[4] = { 'e', 'q', '0', 0 };
        for (int i = 0; i < nb; i++) {
            ek[2] = (char)('0' + i);
            p = put_kv(p, ek, eq_band_get(i));
        }
    }
    g_win_opacity = get_win_opacity();   /* capture live value (Settings slider) */
    p = put_kv(p, "winopacity",   g_win_opacity);
    p = put_kv(p, "clkx",         g_clock_cx);
    p = put_kv(p, "clky",         g_clock_cy);
    p = put_kv(p, "calx",         g_cal_x);
    p = put_kv(p, "caly",         g_cal_y);
    p = put_kv(p, "clklk",        g_clock_locked);
    p = put_kv(p, "callk",        g_cal_locked);
    p = put_kv(p, "dcx",          g_digclk_x);
    p = put_kv(p, "dcy",          g_digclk_y);
    p = put_kv(p, "dclk",         g_digclk_locked);
    p = put_kv(p, "dcsec",        g_digclk_secs);
    p = put_kv(p, "dcsty",        g_digclk_style);
    p = put_kv(p, "curstyle",     g_cursor_style);   // (#116)
    p = put_kv(p, "cursize",      g_cursor_size);    // (#116)
    p = put_kv(p, "wxx",          g_weather_x);
    p = put_kv(p, "wxy",          g_weather_y);
    p = put_kv(p, "crx",          g_crypto_x);
    p = put_kv(p, "cry",          g_crypto_y);
    p = put_kv(p, "stx",          g_stocks_x);
    p = put_kv(p, "sty",          g_stocks_y);
    p = put_kv(p, "wxlk",         g_weather_locked);
    p = put_kv(p, "crlk",         g_crypto_locked);
    p = put_kv(p, "stlk",         g_stocks_locked);
    p = put_kv(p, "show_weather", g_show_weather);
    p = put_kv(p, "show_crypto",  g_show_crypto);
    p = put_kv(p, "show_stocks",  g_show_stocks);
    p = put_kv(p, "wxvb",         g_weather_verbose);
    p = put_kv(p, "crvb",         g_crypto_verbose);
    p = put_kv(p, "stvb",         g_stocks_verbose);
    p = put_kv(p, "brightness",   g_brightness);
    p = put_kv(p, "nightlight",   g_nightlight);
    p = put_kv(p, "show_digclock", g_show_digclock);
    p = put_kv(p, "show_clock",   g_show_clock);
    p = put_kv(p, "show_calendar", g_show_calendar);
    p = put_kv(p, "sheep",        g_sheep_enabled);
    p = put_kv(p, "sheep_speed",  g_sheep_speed);
    p = put_kv(p, "sheep_size",   g_sheep_size);
    p = put_kv(p, "sheep_style",  g_sheep_style);
    p = put_kv(p, "sheep_count",  g_sheep_count);
    p = put_kv(p, "dog",          g_dog_enabled);
    // #274 system monitor / timer / world time
    p = put_kv(p, "show_sysmon",    g_show_sysmon);
    p = put_kv(p, "smx",            g_sysmon_x);
    p = put_kv(p, "smy",            g_sysmon_y);
    p = put_kv(p, "smlk",           g_sysmon_locked);
    p = put_kv(p, "show_timer",     g_show_timer);
    p = put_kv(p, "tmx",            g_timer_x);
    p = put_kv(p, "tmy",            g_timer_y);
    p = put_kv(p, "tmlk",           g_timer_locked);
    p = put_kv(p, "show_worldtime", g_show_worldtime);
    p = put_kv(p, "wtx",            g_worldtime_x);
    p = put_kv(p, "wty",            g_worldtime_y);
    p = put_kv(p, "wtlk",           g_worldtime_locked);
    p = put_kv(p, "show_uptime",    g_show_uptime);
    p = put_kv(p, "upx",            g_uptime_x);
    p = put_kv(p, "upy",            g_uptime_y);
    p = put_kv(p, "uplk",           g_uptime_locked);
    p = put_kv(p, "show_ha",        g_show_ha);
    p = put_kv(p, "hax",            g_ha_x);
    p = put_kv(p, "hay",            g_ha_y);
    p = put_kv(p, "halk",           g_ha_locked);
    p = put_kv(p, "hamode",         g_ha_mode);
    p = put_kv(p, "hamin",          g_ha_min);
    p = put_kv(p, "hamax",          g_ha_max);
    p = put_kv(p, "wtz0",           g_wt_off[0]);
    p = put_kv(p, "wtz1",           g_wt_off[1]);
    p = put_kv(p, "wtz2",           g_wt_off[2]);
    p = put_kv(p, "show_stickies",  g_show_stickies);
    p = put_kv(p, "show_aichat",    g_aichat_enabled);
    p = put_kv(p, "dock_style",     g_dock_style);   // #387
    p = put_kv(p, "dock_opacity",   g_dock_opacity); // #745
    p = put_kv(p, "dock_height",    g_dock_height);  // #123
    p = put_kv(p, "dock_zoom",      g_dock_zoom);    // #123
    p = put_kv(p, "mouse_sens",     get_mouse_speed());  // mouse feel (kernel sensitivity 1-10)

    // Desktop icon positions. (#745) Keyed by the icon's STABLE IDENTITY, not
    // by its index in the list: the list is dynamic now, and an index-keyed
    // coordinate lands on a different icon the moment a file is added or
    // removed ahead of it. See the migration note at the top of this file.
    //
    // #deskicons: stored in LOGICAL (100%-basis) pixels via ui_unpx(), not
    // the raw physical (px,py) the compositor draws with. Saved absolute
    // physical pixels do not follow a scale change: a position saved at 100%
    // and reloaded at 200% used to land in whatever the new grid happened to
    // put at that same raw coordinate, which is how a built-in icon and a
    // freshly-mounted volume icon ended up overlapping on a 4K/200% panel.
    // ui_unpx()/ui_px() is the SAME primitive the kernel uses at the window
    // syscall boundary for ordinary apps (see docs/UI_SCALE.md), applied here
    // because the compositor draws its own chrome in physical pixels and so
    // is the one place that primitive is needed on this side of that
    // boundary. No format bump, no migration flag, no one-shot conversion
    // pass: at 100% ui_px()/ui_unpx() are identities, and every existing
    // saved position was necessarily written before UI scaling existed (i.e.
    // at 100%), so it reads back and writes out unchanged on an unscaled
    // machine, and is scaled correctly the first time it round-trips through
    // a non-100% machine. A previously-saved position is therefore preserved
    // exactly relative to the desktop layout at every scale, not merely left
    // alone at 1x pixels.
    {
        int ic = desktop_icon_count();
        for (int i = 0; i < ic; i++) {
            int x = 0, y = 0;
            desktop_get_icon_pos(i, &x, &y);
            char kb[24];
            if (desktop_icon_key(i, kb, sizeof(kb)) != 0) continue;
            char k[28]; int m = 0;
            k[m++] = 'i'; k[m++] = 'c';
            for (int j = 0; kb[j] && m < 26; j++) k[m++] = kb[j];
            k[m] = 'x'; k[m + 1] = '\0'; p = put_kv(p, k, ui_unpx(x));
            k[m] = 'y';                   p = put_kv(p, k, ui_unpx(y));
        }
    }

    return (int)(p - buf);
}

// (#profbase) THE CHANGE-DETECTION BASELINE. It means "the hash of what is
// ON DISK", and it is set by the WRITE, which is what makes the window below
// impossible rather than merely small. See profile_tick().
// profile_hash_buf() is defined below this point (it sits with the #231
// FNV-1a note), so it needs a forward declaration to be reachable from
// profile_save().
static unsigned int profile_hash_buf(const char *buf, int len);

static unsigned int g_prof_disk_hash = 0;
static int          g_prof_disk_known = 0;
// Consecutive failed saves. Bounded so a permanently-unwritable profile
// cannot turn into an unbounded retry, and so the log cannot be flooded.
static unsigned int g_prof_fail = 0;
#define PROF_FAIL_MAX      20   // stop retrying after this many in a row
#define PROF_FAIL_LOG_MAX   3   // report the first few, then once more at the cap

// A save that fails MUST say so somewhere. Until now this function returned
// silently on every failure path, so a machine where the profile could not be
// written presented as "my settings do not stick after a reboot" with nothing
// in any log to look at - which cost a multi-hour investigation on 2026-08-27
// that a single line would have ended.
//
// It goes to the SERIAL CONSOLE (stdout), deliberately, NOT to a desktop
// notification: the notification spool is <home>/CONFIG/NOTIFY.TXT, in the
// same directory tree whose unwritability is the most likely cause of the
// failure being reported, so a toast would be the first thing to disappear in
// exactly the case that matters. Serial is in every boot log the owner is ever
// asked to send back.
static void prof_save_failed(const char *path) {
    g_prof_fail++;
    if (g_prof_fail <= PROF_FAIL_LOG_MAX) {
        printf("[PROFILE] SAVE FAILED (%u) for '%s'. UI settings changed this "
               "session will NOT survive a reboot.\n", g_prof_fail, path);
    } else if (g_prof_fail == PROF_FAIL_MAX) {
        printf("[PROFILE] SAVE has now failed %u times in a row for '%s'; "
               "giving up for this session. Nothing further will be persisted "
               "until a save succeeds again.\n", g_prof_fail, path);
    }
}

void profile_save(void) {
    // (#745) 5120 rather than 3072: this is ~75 fixed keys plus TWO lines per
    // desktop icon, and it was a stack array sized with no margin. Adding a
    // key to profile_build() as-is is how a stack smash starts. The icon
    // lines are name-keyed and the icon set is DYNAMIC up to
    // DESKTOP_ICON_MAX (32), so the worst case is ~75*20 + 32*2*26 = ~3164
    // bytes; 5120 keeps real headroom. profile_load()'s reader is sized to
    // match, or the tail (the icon positions) would be silently truncated.
    static char buf[5120];
    int len = profile_build(buf);
    // Hash the EXACT bytes about to be written, before any of them can fail.
    unsigned int h = profile_hash_buf(buf, len);

    // (#profbase2 2026-08-27) THE SAME PRIMITIVE #743 ALREADY GAVE EVERY OTHER
    // CONFIG SAVE IN THIS TREE, not a second hand-rolled writer. The old
    // sequence here was sys_unlink() then sys_open(O_CREAT) then a
    // return-unchecked sys_write()/sys_close(): the exact five-step shape
    // userconf.c's #743 comment names verbatim, including the unlink-first
    // ordering that destroys the existing file BEFORE it is known whether a
    // replacement can be written at all. A failed profile_save() used to
    // leave NO file, which falls through to the wizard-era /UIPROFIL.YML at
    // the filesystem root on the next boot, reading exactly like "the old
    // value came back" when what actually happened is that the good file was
    // deleted and nothing replaced it.
    //
    // userconf_write_all() opens O_WRONLY|O_CREAT|O_TRUNC (truncate only
    // AFTER a successful open, never an unlink), writes every byte in a loop,
    // fsyncs, and checks sys_close()'s return, which is where fdlayer.c's
    // buffered write-fd actually commits and can report failure. Checking
    // only sys_write(), as this function used to, is blind to exactly the
    // failure that matters.
    char path[120]; prof_path(path);
    int rc = userconf_write_all(path, buf, (unsigned long)len);
    if (rc != 0 && path[0] == '/') {
        rc = userconf_write_all("/UIPROFIL.YML", buf, (unsigned long)len);
    }
    if (rc != 0) { prof_save_failed(path); return; }

    // ONLY NOW. The baseline is advanced by a write that actually landed, so
    // a failed save leaves the baseline where it was and the next tick tries
    // again instead of concluding "nothing has changed".
    g_prof_disk_hash  = h;
    g_prof_disk_known = 1;
    g_prof_fail       = 0;
}

// FNV-1a, 32-bit. Not cryptographic and does not need to be: it only has to
// distinguish "the serialized profile changed" from "it didn't" between two
// ticks a second apart, and a same-length text buffer that XORs and
// multiplies every byte through the state makes a same-value collision
// between two DIFFERENT real profiles vanishingly unlikely for this purpose.
static unsigned int profile_hash_buf(const char *buf, int len) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)buf[i];
        h *= 16777619u;
    }
    return h;
}

// Throttled change-detection: save when any tracked value changes.
//
// (#231) This used to hash a hand-maintained list of globals, each given its
// own prime multiplier so two simultaneous +-1 changes could not cancel out
// (see #745's g_dock_style/g_dock_opacity prime collision, fixed by giving
// each its own prime - the SAME bug class this rewrite removes structurally).
// That list had to be updated, by hand, every single time a key was added to
// profile_build()/put_kv(), and nothing enforced that it was: it is exactly
// the multi-copy-constant defect shape. Three keys were missed this way
// (g_clock_locked, g_cal_locked, g_digclk_style) and silently reverted at
// every reboot despite applying live and being present in the saved file's
// OWN key set the moment something else also changed.
//
// Deriving the hash from profile_build()'s actual output byte-for-byte closes
// that class for good: there is no second list to fall out of sync, because
// there is no second list. The cost is rebuilding the ~5KB text buffer once a
// second instead of one integer expression; profile_build() only touches
// in-memory state (no syscalls), so this stays well inside the #426 rule that
// nothing on this path may block.
//
// (#profbase 2026-08-27) AND THE BASELINE HAS TO BE THE DISK, NOT A MEMORY
// SNAPSHOT. THIS IS A SEPARATE, MEASURED DATA-LOSS BUG.
//
// #231 was right that the hash must be derived from profile_build()'s real
// bytes, and it is. What it did not change is what the hash was COMPARED
// AGAINST, and that is where the remaining loss was:
//
//   main.c:400   profile_save()  runs during startup and writes the file with
//                                whatever the state is at that instant.
//   this function                did not take its first sample until ~30
//                                frames into the main loop, and then adopted
//                                the CURRENT IN-MEMORY state as the baseline.
//
// So anything changed in that window - roughly 1.4 s at the measured frame
// rate - was written into the baseline and NEVER INTO THE FILE. The hash then
// agreed with itself for the rest of the session and no save ever fired. The
// change applied live, looked perfect, and was gone at the next boot: exactly
// the class #231 set out to eliminate, reached by a different route.
//
// MEASURED, not theorised (2026-08-27, golden 2226, boots W2/W3): a real
// fader drag set EQ band 4 to 88 in the first frames of the session; the
// kernel honoured it for the whole session (776192 frames filtered) and the
// on-disk profile still read `eq4: 50`, and the next boot restored 50. A
// human never touches a control 200 ms after the desktop appears, which is
// precisely why this survived; an automated hook does it every time.
//
// THE WINDOW IS NOW CLOSED BY CONSTRUCTION, not narrowed. The baseline is
// written by profile_save() itself (g_prof_disk_hash), so it always means
// "the hash of the bytes that are on disk". Moving this first sample earlier,
// or shortening the gap, would have left a race for the next person to
// rediscover; there is no longer an interval in which the two can disagree,
// because only the write advances the reference. That also fixes the OTHER
// call sites for free (main.c:400 startup, main.c:1615 widget-drag release),
// which likewise wrote the file without touching the old baseline.
// (#wizdock) main.c's compositor.h typedef is "typedef int bool" - a 4-byte
// int, NOT the 1-byte _Bool that profile.c gets from pwd.h/unistd.h's
// stdbool.h. Declaring this extern as "bool" here would read one byte of a
// four-byte object (or the reverse) depending on struct layout luck. It is
// declared as the int it actually is, matching main.c's real storage, not
// the name it is spelled with there.
extern int g_setup_pending;

void profile_tick(void) {
    // (#wizdock) THE WIZARD IS THE AUTHORITATIVE WRITER DURING FIRST BOOT.
    //
    // write_dock_style() (userland/apps/setup/main.rs) appends a
    // "dock_style: N" line DIRECTLY to /UIPROFIL.YML on purpose (see its own
    // comment: root's home during OOBE is "/", the same fallback path
    // profile_load() already uses, so a fresh account's first session picks
    // it up without needing a real per-user home to exist yet). It is a raw
    // append, not a call through this file's machinery, because at that
    // point there IS no real account yet for profile_save() to attribute the
    // write to.
    //
    // profile_save() does not know about that append. It does not read the
    // old file and merge; it REBUILDS the whole file from this process's
    // OWN live state (profile_build()) and replaces it. If this function's
    // periodic tick fires between the wizard's write_dock_style_live()
    // (which only takes effect once dock_style_poll() next runs, throttled
    // to every 10 ticks - #387) and that poll catching up, g_dock_style is
    // still the OLD value in THIS process's memory. Any profile_save() in
    // that window - and one is likely, since the very next wizard substep
    // sets the wallpaper via a real, SYNCHRONOUS syscall (SYS_SET_WALLPAPER)
    // that changes profile_build()'s hash on the next frame, which is
    // exactly what wakes this function up - rewrites the ENTIRE file from
    // that stale memory and erases the wizard's own correct, already-landed
    // append. REPORTED by the owner (2026-08-27): dock style reverted to
    // the Maytera default the instant the wizard finished. This mechanism is
    // CODE-TRACED, not independently timed live: it is the only path found
    // that explains the report without contradicting anything already
    // measured about profile_load()/profile_save() sharing the correct
    // fallback path.
    //
    // The fix is not a tighter poll (that shrinks the window, it does not
    // close it) and not a merge-on-write (that reintroduces the "two
    // sources of truth" shape #231 already removed). It is ownership: while
    // the wizard is up, IT is the one persisting choices to this exact file,
    // on purpose, synchronously, per choice. This function's OWN periodic
    // full-rewrite has nothing useful to say yet (the compositor's live
    // desktop state here is a generic pre-personalisation default, not the
    // user's choices) and every time it fires in this window it can only
    // make things worse. Suppressed for exactly the wizard's lifetime;
    // profile_tick() resumes the instant g_setup_pending clears (Apply, or
    // the reduced #126 flow's own finish), which is also when this
    // process's own live state has actually caught up to being personal.
    if (g_setup_pending) return;

    static int throttle = 0;
    if (++throttle < 30) return;            // ~ once a second
    throttle = 0;

    // A permanently-unwritable profile must not become a retry every tick.
    if (g_prof_fail >= PROF_FAIL_MAX) return;

    static char buf[5120];
    int len = profile_build(buf);
    unsigned int h = profile_hash_buf(buf, len);

    // Nothing is known to be on disk (the startup save failed, or has not run
    // yet). Write, rather than adopting memory as the truth - adopting memory
    // is the bug this block is about.
    if (!g_prof_disk_known) { profile_save(); return; }

    if (h != g_prof_disk_hash) profile_save();
}
