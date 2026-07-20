// profile.c - persist UI settings to ~/.ui-profile.yaml (#92).
// The compositor reads the profile on launch and applies it (theme, wallpaper,
// fonts, display fx, screensaver, volume, widgets, sheep), and writes it back
// whenever any of those change (captures both Settings and tray-menu edits), so
// the look is per-user and survives reboots.

#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/pwd.h"
#include "../../libc/unistd.h"

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
extern int g_digclk_x, g_digclk_y, g_digclk_locked, g_digclk_12h, g_digclk_secs, g_digclk_style; // clock.c
extern int g_cursor_style, g_cursor_size;  // main.c (#116)
extern int g_dock_style;                   // taskbar.c (#387 dock layout)

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
int  desktop_positions_hash(void);

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
        int x = 0, y = 0;
        desktop_get_icon_pos(icoi, &x, &y);
        if (axis == 0) x = v; else y = v;
        desktop_set_icon_pos(icoi, x, y);
        return;
    }
    if      (!strcmp(k, "theme"))        set_theme(v);
    else if (!strcmp(k, "wallpaper"))    set_wallpaper(v);
    else if (!strcmp(k, "font_size"))    set_font_size(v);
    else if (!strcmp(k, "icon_size"))    set_icon_size(v);
    else if (!strcmp(k, "screensaver"))  set_screensaver(v);
    else if (!strcmp(k, "volume"))       set_volume(v);
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
    else if (!strcmp(k, "dc12"))         g_digclk_12h = v;
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
    else if (!strcmp(k, "dock_style"))      g_dock_style = (v >= 0 && v < 4) ? v : 0;  // #387
    // Mouse sensitivity (1-10): the kernel is the live authority (SYS_SET_MOUSE_SPEED,
    // applied to every PS/2 + USB delta). Seed it from the persisted profile on load.
    else if (!strcmp(k, "mouse_sens"))      set_mouse_speed((v >= 1 && v <= 10) ? v : 7);
}

void profile_load(void) {
    char path[120]; prof_path(path);
    int fd = sys_open(path, 0);
    if (fd < 0) fd = sys_open("/UIPROFIL.YML", 0);
    if (fd < 0) return;
    static char buf[2048];
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

// Build an "ico<N><axis>" key into out (e.g. "ico12x"). out must hold >=8 bytes.
static void icon_key(char *out, int idx, char axis) {
    int i = 0;
    out[i++] = 'i'; out[i++] = 'c'; out[i++] = 'o';
    char t[6]; int n = 0;
    if (idx == 0) t[n++] = '0';
    while (idx) { t[n++] = '0' + idx % 10; idx /= 10; }
    while (n) out[i++] = t[--n];
    out[i++] = axis;
    out[i] = '\0';
}

void profile_save(void) {
    char buf[2048]; char *p = buf;
    p = put_kv(p, "theme",        get_theme());
    p = put_kv(p, "wallpaper",    get_wallpaper());
    p = put_kv(p, "font_size",    get_font_size());
    p = put_kv(p, "icon_size",    get_icon_size());
    p = put_kv(p, "screensaver",  get_screensaver());
    p = put_kv(p, "volume",       get_volume());
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
    p = put_kv(p, "dc12",         g_digclk_12h);
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
    p = put_kv(p, "mouse_sens",     get_mouse_speed());  // mouse feel (kernel sensitivity 1-10)

    // Desktop icon positions (#: movable icons). One pair of keys per icon.
    {
        int ic = desktop_icon_count();
        for (int i = 0; i < ic; i++) {
            int x = 0, y = 0;
            desktop_get_icon_pos(i, &x, &y);
            char k[8];
            icon_key(k, i, 'x'); p = put_kv(p, k, x);
            icon_key(k, i, 'y'); p = put_kv(p, k, y);
        }
    }

    char path[120]; prof_path(path);
    sys_unlink(path);                       // clean truncate
    int fd = sys_open(path, 0x41);          // O_WRONLY | O_CREAT
    if (fd < 0 && path[0] != '/') return;
    if (fd < 0) { fd = sys_open("/UIPROFIL.YML", 0x41); if (fd < 0) return; }
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
}

// Throttled change-detection: save when any tracked value changes.
void profile_tick(void) {
    static int last = -1;
    static int throttle = 0;
    if (++throttle < 30) return;            // ~ once a second
    throttle = 0;
    // (#116) cursor style/size now live via SYS_GET/SET_CURSOR in the kernel (read
    // each frame in cursor_render); the old SETTINGS.CFG poll was removed. The
    // hash below still includes g_cursor_* so the value persists to UIPROFIL.YML.
    int h = get_theme()*7 + get_wallpaper()*13 + get_font_size()*17 + get_icon_size()*19
          + get_screensaver()*23 + get_volume()*29 + g_brightness*31 + g_nightlight*37
          + g_show_clock*41 + g_show_calendar*43 + g_sheep_enabled*47 + g_sheep_speed*53
          + g_show_digclock*163
          + g_sheep_size*59 + g_sheep_style*61 + g_sheep_count*67 + g_dog_enabled*71
          + g_weather_x*73 + g_weather_y*79 + g_crypto_x*83 + g_crypto_y*89
          + g_stocks_x*97 + g_stocks_y*101 + g_weather_locked*103 + g_crypto_locked*107
          + g_stocks_locked*109 + g_show_weather*113 + g_show_crypto*127 + g_show_stocks*131
          + g_weather_verbose*137 + g_crypto_verbose*139 + g_stocks_verbose*149
          + get_win_opacity()*151 + desktop_positions_hash()*157
          + g_digclk_x*167 + g_digclk_y*173 + g_digclk_locked*179 + g_digclk_12h*181 + g_digclk_secs*191
          + g_cursor_style*193 + g_cursor_size*197   // (#116) persist cursor + detect poll change
          + g_show_sysmon*199 + g_sysmon_x*211 + g_sysmon_y*223 + g_sysmon_locked*227
          + g_show_timer*229 + g_timer_x*233 + g_timer_y*239 + g_timer_locked*241
          + g_show_worldtime*251 + g_worldtime_x*257 + g_worldtime_y*263 + g_worldtime_locked*269
          + g_wt_off[0]*271 + g_wt_off[1]*277 + g_wt_off[2]*281   // #274
          + g_show_stickies*283   // #270
          + g_show_uptime*307 + g_uptime_x*311 + g_uptime_y*313 + g_uptime_locked*317
          + g_show_ha*331 + g_ha_x*337 + g_ha_y*347 + g_ha_locked*349
          + g_ha_mode*353 + g_ha_min*359 + g_ha_max*367   // #419
          + g_aichat_enabled*293   // #185
          + g_dock_style*331       // #387 dock layout
          + get_mouse_speed()*373; // mouse feel: persist sensitivity changes
    if (last == -1) { last = h; return; }   // first sample: don't rewrite
    if (h != last) { last = h; profile_save(); }
}
