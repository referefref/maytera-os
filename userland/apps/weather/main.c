// weather - Weather app for MayteraOS (user-mode)
// Full GUI weather client: current conditions with a drawn condition icon and
// big-digit temperature, detail grid (feels-like, humidity, wind, pressure,
// visibility, UV), and a 3-day forecast strip with sunrise/sunset.
// Data source: wttr.in "?format=j1" JSON over the kernel HTTPS helper. The
// fetch is fully async (http_fetch_start/poll/read) so the UI never blocks;
// wttr.in is the proven-working endpoint on this TLS stack (see netinfo).
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"

#define WIN_W 640
#define WIN_H 470
#define CHAR_W 8
#define CHAR_H 16

#define TOPBAR_H 34
#define STATUS_H 20

// Fetch buffer: wttr.in j1 responses are ~40-60 KB.
#define FETCH_MAX (128 * 1024)
static char g_fetch[FETCH_MAX];

// ---- Theme roles (live mapped from the kernel theme, same scheme as irc) --
static uint32_t BG_COLOR     = 0x001E1E1E;
static uint32_t PANEL_BG     = 0x00282828;
static uint32_t INPUT_BG     = 0x00353535;
static uint32_t TOPBAR_BG    = 0x00404060;
static uint32_t TEXT_COLOR   = 0x00E0E0E0;
static uint32_t DIM_COLOR    = 0x00909090;
static uint32_t BORDER_COLOR = 0x00505050;
static uint32_t ACCENT_BG    = 0x00385078;
// Semantic (fixed across themes)
#define WARN_COLOR   0x00FF6666
#define OK_COLOR     0x0066FF66
#define SUN_COLOR    0x00FFD040
#define CLOUD_COLOR  0x00B0B8C0
#define CLOUD_DARK   0x00808890
#define RAIN_COLOR   0x0060A0FF
#define SNOW_COLOR   0x00F0F0FF
#define BOLT_COLOR   0x00FFE040

static int g_theme_last = -1;

static void apply_theme(void) {
    int kt = get_theme();
    switch (kt) {
        case 2:  // Light
            BG_COLOR=0x00FFFFFF; PANEL_BG=0x00F0F0F0; INPUT_BG=0x00FFFFFF;
            TOPBAR_BG=0x00E8E8E8; TEXT_COLOR=0x00202020; DIM_COLOR=0x00707070;
            BORDER_COLOR=0x00CCCCCC; ACCENT_BG=0x00D6E4FB;
            break;
        case 4:  // Classic
            BG_COLOR=0x00C0C0C0; PANEL_BG=0x00D0D0D0; INPUT_BG=0x00FFFFFF;
            TOPBAR_BG=0x00000080; TEXT_COLOR=0x00000000; DIM_COLOR=0x00404040;
            BORDER_COLOR=0x00808080; ACCENT_BG=0x00A0A0C0;
            break;
        case 5:  // Ocean
            BG_COLOR=0x00224455; PANEL_BG=0x001E4050; INPUT_BG=0x00183040;
            TOPBAR_BG=0x00305060; TEXT_COLOR=0x00E0F0FF; DIM_COLOR=0x0090B0C0;
            BORDER_COLOR=0x00406070; ACCENT_BG=0x00305060;
            break;
        case 9:  // Nord
            BG_COLOR=0x003B4252; PANEL_BG=0x00343B49; INPUT_BG=0x002B303B;
            TOPBAR_BG=0x00434C5E; TEXT_COLOR=0x00ECEFF4; DIM_COLOR=0x00A0A8B8;
            BORDER_COLOR=0x004C566A; ACCENT_BG=0x00434C5E;
            break;
        default: // Dark
            BG_COLOR=0x001E1E1E; PANEL_BG=0x00282828; INPUT_BG=0x00353535;
            TOPBAR_BG=0x00404060; TEXT_COLOR=0x00E0E0E0; DIM_COLOR=0x00909090;
            BORDER_COLOR=0x00505050; ACCENT_BG=0x00385078;
            break;
    }
    g_theme_last = kt;
}

static int win = -1;
static int g_w = WIN_W, g_h = WIN_H;

// ---- Fetch state machine ---------------------------------------------------
enum { ST_IDLE, ST_FETCHING, ST_HAVE_DATA, ST_ERROR };
static int  g_state = ST_IDLE;
static int  g_job = -1;
static int  g_poll_frames = 0;          // frames spent in the current fetch
#define FETCH_TIMEOUT_FRAMES 600        // ~30 s at the 50 ms event timeout
static char g_status_msg[96] = "Enter a city and press Enter (blank = auto-locate)";

// City input field (always focused; the only field in the app)
static char g_city[64] = "";
static textfield_t g_tf;

// ---- Parsed weather model ----------------------------------------------
typedef struct {
    char date[16];
    char desc[40];
    char code[8];
    char maxc[8];
    char minc[8];
    char sunrise[12];
    char sunset[12];
} day_t;

static char w_city[64], w_country[48];
static char w_temp[8], w_feels[8], w_desc[40], w_code[8];
static char w_hum[8], w_wind[8], w_winddir[8], w_press[8], w_vis[8], w_uv[8];
static day_t w_days[3];
static int  w_day_count = 0;

// ---- Tiny JSON scanners (wttr.in j1: every value is a quoted string) -----
// Find `"key"` (with quotes, so "weather" never matches "weatherDesc") and
// return a pointer just past the following ':'.
static const char *jkey(const char *p, const char *key) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *q = strstr(p, pat);
    if (!q) return 0;
    q += strlen(pat);
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q != ':') return 0;
    return q + 1;
}

// Extract the string value for key, starting the search at p. Transparently
// unwraps the wttr nesting pattern  key: [ { "value": "X" } ].
static int jstr(const char *p, const char *key, char *out, int cap) {
    out[0] = '\0';
    const char *q = jkey(p, key);
    if (!q) return 0;
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q == '[') {
        q = jkey(q, "value");
        if (!q) return 0;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    }
    if (*q != '"') return 0;
    q++;
    int i = 0;
    while (*q && *q != '"' && i < cap - 1) {
        if (*q == '\\' && q[1]) q++;
        out[i++] = *q++;
    }
    out[i] = '\0';
    return i;
}

static int parse_weather(const char *j) {
    const char *cur = jkey(j, "current_condition");
    if (!cur) return -1;
    jstr(cur, "FeelsLikeC",     w_feels,   sizeof(w_feels));
    jstr(cur, "humidity",       w_hum,     sizeof(w_hum));
    jstr(cur, "pressure",       w_press,   sizeof(w_press));
    jstr(cur, "temp_C",         w_temp,    sizeof(w_temp));
    jstr(cur, "uvIndex",        w_uv,      sizeof(w_uv));
    jstr(cur, "visibility",     w_vis,     sizeof(w_vis));
    jstr(cur, "weatherCode",    w_code,    sizeof(w_code));
    jstr(cur, "weatherDesc",    w_desc,    sizeof(w_desc));
    jstr(cur, "winddir16Point", w_winddir, sizeof(w_winddir));
    jstr(cur, "windspeedKmph",  w_wind,    sizeof(w_wind));
    if (!w_temp[0]) return -1;

    const char *area = jkey(j, "nearest_area");
    if (area) {
        jstr(area, "areaName", w_city,    sizeof(w_city));
        jstr(area, "country",  w_country, sizeof(w_country));
    }

    // Forecast days. j1 emits keys alphabetically inside each day object:
    // astronomy (sunrise/sunset), avgtempC, date, hourly[...], maxtempC,
    // mintempC, ... so a forward sequential scan per day is safe.
    w_day_count = 0;
    const char *d = jkey(j, "weather");
    for (int i = 0; i < 3 && d; i++) {
        day_t *dy = &w_days[i];
        jstr(d, "sunrise", dy->sunrise, sizeof(dy->sunrise));
        jstr(d, "sunset",  dy->sunset,  sizeof(dy->sunset));
        if (!jstr(d, "date", dy->date, sizeof(dy->date))) break;
        // Noon hourly entry: find "time":"1200", then its code+desc follow.
        const char *p = jkey(d, "date");
        const char *noon = 0;
        const char *t = p;
        for (int guard = 0; guard < 16 && t; guard++) {
            char tv[8];
            const char *nt = jkey(t, "time");
            if (!nt) break;
            t = nt;
            { // parse the quoted time value in place
                const char *v = t;
                while (*v == ' ') v++;
                int k = 0; tv[0] = '\0';
                if (*v == '"') {
                    v++;
                    while (*v && *v != '"' && k < 7) tv[k++] = *v++;
                    tv[k] = '\0';
                }
            }
            if (strcmp(tv, "1200") == 0) { noon = t; break; }
        }
        if (noon) {
            jstr(noon, "weatherCode", dy->code, sizeof(dy->code));
            jstr(noon, "weatherDesc", dy->desc, sizeof(dy->desc));
            p = noon;
        } else {
            dy->code[0] = '\0'; dy->desc[0] = '\0';
        }
        jstr(p, "maxtempC", dy->maxc, sizeof(dy->maxc));
        jstr(p, "mintempC", dy->minc, sizeof(dy->minc));
        w_day_count = i + 1;
        d = jkey(p, "mintempC");     // advance past this day block
    }
    return 0;
}

// ---- Fetch driving --------------------------------------------------------
static void build_url(char *url, int cap) {
    int u = 0;
    const char *pre = "https://wttr.in/";
    for (const char *p = pre; *p && u < cap - 1; p++) url[u++] = *p;
    for (int i = 0; g_city[i] && u < cap - 14; i++) {
        char c = g_city[i];
        if (c == ' ') {
            url[u++] = '%'; url[u++] = '2'; url[u++] = '0';
        } else {
            url[u++] = c;
        }
    }
    const char *suf = "?format=j1";
    for (const char *p = suf; *p && u < cap - 1; p++) url[u++] = *p;
    url[u] = '\0';
}

static void start_fetch(void) {
    if (g_job >= 0) { http_fetch_cancel(g_job); g_job = -1; }
    if (!sys_net_is_up()) {
        g_state = ST_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg), "Network is down");
        return;
    }
    char url[160];
    build_url(url, sizeof(url));
    g_job = http_fetch_start(url);
    if (g_job < 0) {
        g_state = ST_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg), "Could not start fetch");
        return;
    }
    g_state = ST_FETCHING;
    g_poll_frames = 0;
    snprintf(g_status_msg, sizeof(g_status_msg), "Fetching weather%s%s...",
             g_city[0] ? " for " : "", g_city[0] ? g_city : "");
}

static void poll_fetch(void) {
    if (g_state != ST_FETCHING || g_job < 0) return;
    int status = 0;
    unsigned int len = 0;
    int st = http_fetch_poll(g_job, &status, &len);
    if (st == 0) {
        if (++g_poll_frames > FETCH_TIMEOUT_FRAMES) {
            http_fetch_cancel(g_job); g_job = -1;
            g_state = ST_ERROR;
            snprintf(g_status_msg, sizeof(g_status_msg), "Fetch timed out");
        }
        return;
    }
    if (st == 1) {
        int n = http_fetch_read(g_job, g_fetch, FETCH_MAX - 1);
        g_job = -1;
        if (n <= 0 || (status != 200 && status != 0)) {
            g_state = ST_ERROR;
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "HTTP error (status %d)", status);
            return;
        }
        g_fetch[n] = '\0';
        if (parse_weather(g_fetch) == 0) {
            g_state = ST_HAVE_DATA;
            snprintf(g_status_msg, sizeof(g_status_msg), "Updated: %s%s%s",
                     w_city[0] ? w_city : "current location",
                     w_country[0] ? ", " : "", w_country);
        } else {
            g_state = ST_ERROR;
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "Unexpected response (city not found?)");
        }
    } else {
        http_fetch_read(g_job, g_fetch, 0);   // free the job
        g_job = -1;
        g_state = ST_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg), "Fetch failed");
    }
}

// ---- Icon drawing ---------------------------------------------------------
enum { IC_SUN, IC_PARTLY, IC_CLOUD, IC_FOG, IC_RAIN, IC_SNOW, IC_THUNDER };

static int code_to_icon(const char *codestr, const char *desc) {
    int c = atoi(codestr);
    if (c == 113) return IC_SUN;
    if (c == 116) return IC_PARTLY;
    if (c == 119 || c == 122) return IC_CLOUD;
    if (c == 143 || c == 248 || c == 260) return IC_FOG;
    if (c == 200 || c == 386 || c == 389 || c == 392 || c == 395) return IC_THUNDER;
    static const int snow[] = {179,182,185,227,230,317,320,323,326,329,332,
                               335,338,350,368,371,374,377};
    for (unsigned i = 0; i < sizeof(snow)/sizeof(snow[0]); i++)
        if (c == snow[i]) return IC_SNOW;
    if (c >= 176) return IC_RAIN;
    // fall back on the description text
    if (strstr(desc, "Sun") || strstr(desc, "Clear")) return IC_SUN;
    if (strstr(desc, "Partly")) return IC_PARTLY;
    if (strstr(desc, "Rain") || strstr(desc, "Drizzle") || strstr(desc, "Shower"))
        return IC_RAIN;
    return IC_CLOUD;
}

static void fill_circle(int cx, int cy, int r, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        dx--;
        if (dx >= 0) win_draw_rect(win, cx - dx, cy + dy, 2 * dx + 1, 1, col);
    }
}

// Cloud shape occupying roughly (x,y,w,h)
static void draw_cloud(int x, int y, int w, int h, uint32_t col) {
    int r = h / 2;
    fill_circle(x + r, y + h - r, r, col);
    fill_circle(x + w - r, y + h - r, r, col);
    fill_circle(x + w / 2, y + r, r + r / 3, col);
    win_draw_rect(win, x + r, y + h - 2 * r, w - 2 * r, 2 * r, col);
}

static void draw_sun(int cx, int cy, int r) {
    fill_circle(cx, cy, r, SUN_COLOR);
    int rl = r + r / 2 + 3;
    // 4 straight + 4 diagonal rays as thin rects
    win_draw_rect(win, cx - rl, cy - 1, rl - r - 2, 2, SUN_COLOR);
    win_draw_rect(win, cx + r + 2, cy - 1, rl - r - 2, 2, SUN_COLOR);
    win_draw_rect(win, cx - 1, cy - rl, 2, rl - r - 2, SUN_COLOR);
    win_draw_rect(win, cx - 1, cy + r + 2, 2, rl - r - 2, SUN_COLOR);
    int d = (r * 3) / 4 + 2;
    win_draw_rect(win, cx - d - 2, cy - d - 2, 3, 3, SUN_COLOR);
    win_draw_rect(win, cx + d,     cy - d - 2, 3, 3, SUN_COLOR);
    win_draw_rect(win, cx - d - 2, cy + d,     3, 3, SUN_COLOR);
    win_draw_rect(win, cx + d,     cy + d,     3, 3, SUN_COLOR);
}

// Draw a condition icon inside box (x,y,size,size). size >= 24.
static void draw_icon(int icon, int x, int y, int size) {
    int cx = x + size / 2, cy = y + size / 2;
    switch (icon) {
        case IC_SUN:
            draw_sun(cx, cy, size / 4);
            break;
        case IC_PARTLY:
            draw_sun(x + size / 3, y + size / 3, size / 6);
            draw_cloud(x + size / 6, cy - size / 8, (2 * size) / 3, size / 3,
                       CLOUD_COLOR);
            break;
        case IC_CLOUD:
            draw_cloud(x + size / 10, y + size / 4, (4 * size) / 5, size / 3,
                       CLOUD_DARK);
            draw_cloud(x + size / 6, y + size / 2 - size / 10, (2 * size) / 3,
                       size / 3, CLOUD_COLOR);
            break;
        case IC_FOG:
            for (int i = 0; i < 4; i++)
                win_draw_rect(win, x + size / 8 + (i % 2) * (size / 12),
                              y + size / 3 + i * (size / 8),
                              (3 * size) / 4 - (i % 2) * (size / 6),
                              size / 16 + 1, CLOUD_COLOR);
            break;
        case IC_RAIN:
            draw_cloud(x + size / 8, y + size / 5, (3 * size) / 4, size / 3,
                       CLOUD_DARK);
            for (int i = 0; i < 4; i++)
                win_draw_rect(win, x + size / 4 + i * (size / 6),
                              y + (3 * size) / 5 + (i % 2) * (size / 12),
                              2, size / 5, RAIN_COLOR);
            break;
        case IC_SNOW:
            draw_cloud(x + size / 8, y + size / 5,
                       (3 * size) / 4, size / 3, CLOUD_COLOR);
            for (int i = 0; i < 4; i++) {
                int sx = x + size / 4 + i * (size / 6);
                int sy = y + (2 * size) / 3 + (i % 2) * (size / 10);
                win_draw_rect(win, sx - 2, sy, 5, 1, SNOW_COLOR);
                win_draw_rect(win, sx, sy - 2, 1, 5, SNOW_COLOR);
            }
            break;
        case IC_THUNDER: {
            draw_cloud(x + size / 8, y + size / 6, (3 * size) / 4, size / 3,
                       CLOUD_DARK);
            int bx = cx - size / 12, by = y + size / 2;
            win_draw_rect(win, bx, by, size / 10, size / 6, BOLT_COLOR);
            win_draw_rect(win, bx - size / 12, by + size / 6, size / 10,
                          size / 6, BOLT_COLOR);
            break;
        }
        default: break;
    }
}

// ---- Big digit rendering (3x5 cell font, scaled) --------------------------
// Glyphs: '0'-'9', '-', degree mark, 'C'
static const unsigned char bigfont[13][5] = {
    {7,5,5,5,7},   // 0
    {2,6,2,2,7},   // 1
    {7,1,7,4,7},   // 2
    {7,1,7,1,7},   // 3
    {5,5,7,1,1},   // 4
    {7,4,7,1,7},   // 5
    {7,4,7,5,7},   // 6
    {7,1,1,2,2},   // 7
    {7,5,7,5,7},   // 8
    {7,5,7,1,7},   // 9
    {0,0,7,0,0},   // -
    {7,5,7,0,0},   // degree (small square, top-aligned)
    {7,4,4,4,7},   // C
};

static int big_glyph_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == '*') return 11;   // degree
    if (c == 'C') return 12;
    return -1;
}

// Draw string s ("12*C") with cell scale px; returns total width.
static int draw_big(int x, int y, const char *s, int scale, uint32_t col) {
    int ox = x;
    for (int i = 0; s[i]; i++) {
        int gi = big_glyph_index(s[i]);
        if (gi >= 0) {
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++)
                    if (bigfont[gi][r] & (4 >> c))
                        win_draw_rect(win, x + c * scale, y + r * scale,
                                      scale, scale, col);
        }
        x += 4 * scale;
    }
    return x - ox;
}

// ---- Layout + drawing ------------------------------------------------------
static int btn_x, btn_y, btn_w, btn_h;      // Refresh button hit box
static int field_x, field_y, field_w;       // City field hit box

static void draw_topbar(void) {
    win_draw_rect(win, 0, 0, g_w, TOPBAR_H, TOPBAR_BG);
    win_draw_text(win, 8, (TOPBAR_H - CHAR_H) / 2, "City:", TEXT_COLOR);

    field_x = 8 + 6 * CHAR_W;
    field_y = (TOPBAR_H - 22) / 2;
    btn_w = 9 * CHAR_W;
    btn_h = 22;
    btn_x = g_w - btn_w - 8;
    btn_y = field_y;
    field_w = btn_x - field_x - 10;
    if (field_w < 8 * CHAR_W) field_w = 8 * CHAR_W;

    win_draw_rect(win, field_x, field_y, field_w, 22, INPUT_BG);
    win_draw_rect(win, field_x, field_y, field_w, 1, BORDER_COLOR);
    win_draw_rect(win, field_x, field_y + 21, field_w, 1, BORDER_COLOR);
    win_draw_rect(win, field_x, field_y, 1, 22, BORDER_COLOR);
    win_draw_rect(win, field_x + field_w - 1, field_y, 1, 22, BORDER_COLOR);

    // Horizontal scroll so the caret stays visible.
    int maxc = (field_w - 10) / CHAR_W;
    int start = 0;
    if (maxc > 2 && g_tf.cursor > maxc - 1) start = g_tf.cursor - (maxc - 1);
    win_draw_text(win, field_x + 5, field_y + 3, g_city + start, TEXT_COLOR);
    int caret_x = field_x + 5 + (g_tf.cursor - start) * CHAR_W;
    win_draw_rect(win, caret_x, field_y + 2, 1, 18, TEXT_COLOR);

    win_draw_rect(win, btn_x, btn_y, btn_w, btn_h, ACCENT_BG);
    win_draw_rect(win, btn_x, btn_y, btn_w, 1, BORDER_COLOR);
    win_draw_rect(win, btn_x, btn_y + btn_h - 1, btn_w, 1, BORDER_COLOR);
    win_draw_rect(win, btn_x, btn_y, 1, btn_h, BORDER_COLOR);
    win_draw_rect(win, btn_x + btn_w - 1, btn_y, 1, btn_h, BORDER_COLOR);
    win_draw_text(win, btn_x + (btn_w - 7 * CHAR_W) / 2, btn_y + 3, "Refresh",
                  TEXT_COLOR);
}

static void draw_statusline(void) {
    int y = g_h - STATUS_H;
    win_draw_rect(win, 0, y, g_w, STATUS_H, PANEL_BG);
    win_draw_rect(win, 0, y, g_w, 1, BORDER_COLOR);
    uint32_t col = TEXT_COLOR;
    if (g_state == ST_ERROR) col = WARN_COLOR;
    else if (g_state == ST_HAVE_DATA) col = OK_COLOR;
    win_draw_text(win, 6, y + 2, g_status_msg, col);
}

static void detail_row(int x, int y, const char *label, const char *val,
                       const char *unit) {
    char buf[64];
    win_draw_text(win, x, y, label, DIM_COLOR);
    snprintf(buf, sizeof(buf), "%s%s", val[0] ? val : "-", val[0] ? unit : "");
    win_draw_text(win, x + 12 * CHAR_W, y, buf, TEXT_COLOR);
}

static void draw_current(int x, int y, int w, int h) {
    win_draw_rect(win, x, y, w, h, PANEL_BG);
    win_draw_rect(win, x, y, w, 1, BORDER_COLOR);
    win_draw_rect(win, x, y + h - 1, w, 1, BORDER_COLOR);
    win_draw_rect(win, x, y, 1, h, BORDER_COLOR);
    win_draw_rect(win, x + w - 1, y, 1, h, BORDER_COLOR);

    if (g_state != ST_HAVE_DATA) {
        const char *m = (g_state == ST_FETCHING) ? "Fetching..."
                       : (g_state == ST_ERROR) ? "No data" : "No data yet";
        win_draw_text(win, x + (w - (int)strlen(m) * CHAR_W) / 2,
                      y + h / 2 - CHAR_H / 2, m, DIM_COLOR);
        return;
    }

    // Location header
    char loc[96];
    snprintf(loc, sizeof(loc), "%s%s%s", w_city[0] ? w_city : "Weather",
             w_country[0] ? ", " : "", w_country);
    win_draw_text(win, x + 12, y + 8, loc, TEXT_COLOR);

    int icon_sz = h - 60;
    if (icon_sz > 96) icon_sz = 96;
    if (icon_sz < 40) icon_sz = 40;
    draw_icon(code_to_icon(w_code, w_desc), x + 16, y + 34, icon_sz);

    // Big temperature next to the icon
    char big[12];
    snprintf(big, sizeof(big), "%s*C", w_temp);
    int scale = (h >= 150) ? 6 : 4;
    int bx = x + 16 + icon_sz + 24;
    int by = y + 40;
    draw_big(bx, by, big, scale, TEXT_COLOR);
    win_draw_text(win, bx, by + 5 * scale + 8, w_desc, DIM_COLOR);

    // Detail grid on the right half
    int dx = x + w / 2 + 24;
    if (dx < bx + 14 * CHAR_W) dx = bx + 16 * CHAR_W;
    int dy = y + 36;
    char windval[24];
    snprintf(windval, sizeof(windval), "%s km/h %s", w_wind[0] ? w_wind : "-",
             w_winddir);
    detail_row(dx, dy,                "Feels like", w_feels, " C");
    detail_row(dx, dy + CHAR_H + 6,   "Humidity",   w_hum,   " %");
    win_draw_text(win, dx, dy + 2 * (CHAR_H + 6), "Wind", DIM_COLOR);
    win_draw_text(win, dx + 12 * CHAR_W, dy + 2 * (CHAR_H + 6), windval,
                  TEXT_COLOR);
    detail_row(dx, dy + 3 * (CHAR_H + 6), "Pressure",   w_press, " hPa");
    detail_row(dx, dy + 4 * (CHAR_H + 6), "Visibility", w_vis,   " km");
    detail_row(dx, dy + 5 * (CHAR_H + 6), "UV index",   w_uv,    "");
}

static void draw_forecast(int x, int y, int w, int h) {
    win_draw_rect(win, x, y, w, h, PANEL_BG);
    win_draw_rect(win, x, y, w, 1, BORDER_COLOR);
    win_draw_rect(win, x, y + h - 1, w, 1, BORDER_COLOR);
    win_draw_rect(win, x, y, 1, h, BORDER_COLOR);
    win_draw_rect(win, x + w - 1, y, 1, h, BORDER_COLOR);

    if (g_state != ST_HAVE_DATA || w_day_count == 0) {
        win_draw_text(win, x + 12, y + 8, "Forecast", DIM_COLOR);
        return;
    }

    int cols = w_day_count;
    int cw = w / cols;
    static const char *labels[3] = { "Today", "Tomorrow", "Day after" };
    for (int i = 0; i < cols; i++) {
        int cx = x + i * cw;
        if (i > 0) win_draw_rect(win, cx, y + 6, 1, h - 12, BORDER_COLOR);
        day_t *dy = &w_days[i];
        win_draw_text(win, cx + 10, y + 6, labels[i], TEXT_COLOR);
        win_draw_text(win, cx + 10, y + 6 + CHAR_H, dy->date, DIM_COLOR);

        int isz = h - 78;
        if (isz > 48) isz = 48;
        if (isz < 28) isz = 28;
        draw_icon(code_to_icon(dy->code, dy->desc), cx + 10,
                  y + 10 + 2 * CHAR_H, isz);

        char t[40];
        snprintf(t, sizeof(t), "%s / %s C", dy->maxc[0] ? dy->maxc : "-",
                 dy->minc[0] ? dy->minc : "-");
        int tx = cx + 16 + isz;
        win_draw_text(win, tx, y + 14 + 2 * CHAR_H, t, TEXT_COLOR);
        // condition text, truncated to the column
        char dsc[40];
        int maxc = (cw - (16 + isz) - 12) / CHAR_W;
        if (maxc > (int)sizeof(dsc) - 1) maxc = sizeof(dsc) - 1;
        if (maxc < 4) maxc = 4;
        int k = 0;
        for (; dy->desc[k] && k < maxc; k++) dsc[k] = dy->desc[k];
        dsc[k] = '\0';
        win_draw_text(win, tx, y + 14 + 3 * CHAR_H + 2, dsc, DIM_COLOR);

        char sun[40];
        snprintf(sun, sizeof(sun), "^ %s", dy->sunrise);
        win_draw_text(win, cx + 10, y + h - 2 * CHAR_H - 8, sun, SUN_COLOR);
        snprintf(sun, sizeof(sun), "v %s", dy->sunset);
        win_draw_text(win, cx + 10, y + h - CHAR_H - 4, sun, DIM_COLOR);
    }
}

static void draw_all(void) {
    win_get_size(win, &g_w, &g_h);
    if (g_w < 420) g_w = 420;
    if (g_h < 300) g_h = 300;

    win_draw_rect(win, 0, 0, g_w, g_h, BG_COLOR);
    draw_topbar();

    int body_y = TOPBAR_H + 8;
    int body_h = g_h - TOPBAR_H - STATUS_H - 24;
    int cur_h = (body_h * 55) / 100;
    int fc_h = body_h - cur_h - 8;
    draw_current(8, body_y, g_w - 16, cur_h);
    draw_forecast(8, body_y + cur_h + 8, g_w - 16, fc_h);
    draw_statusline();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    win = win_create("Weather", 120, 80, WIN_W, WIN_H);
    if (win < 0) return 1;

    apply_theme();
    tf_init(&g_tf, g_city, sizeof(g_city));

    // Auto-fetch on startup (blank city = wttr.in IP geolocation).
    start_fetch();

    int running = 1;
    while (running) {
        int th = get_theme();
        int theme_changed = (th != g_theme_last);
        if (theme_changed) apply_theme();

        gui_event_t ev;
        int ret = win_get_event(win, &ev, 50);
        if (ret > 0) {
            switch (ev.type) {
                case EVENT_WINDOW_CLOSE:
                    running = 0; break;
                case EVENT_KEY_DOWN:
                    if (ev.keycode == 0x01) {                // Esc
                        running = 0;
                    } else if (ev.keycode == 0x1C || ev.key_char == '\n' ||
                               ev.key_char == '\r') {
                        start_fetch();
                    } else if (ev.keycode == 0x3F || ev.key_char == 18) {
                        start_fetch();                        // F5 / Ctrl+R
                    } else {
                        tf_handle_key(&g_tf, &ev);
                    }
                    break;
                case EVENT_MOUSE_DOWN:
                    if (gui_point_in_rect(ev.mouse_x, ev.mouse_y,
                                          btn_x, btn_y, btn_w, btn_h))
                        start_fetch();
                    break;
                default: break;
            }
        }

        // #548: poll_fetch()/draw_all()/win_invalidate() used to run
        // unconditionally every 50ms poll tick forever, even once the window
        // is fully idle with no fetch in flight and nothing changed - one of
        // the "compositor never reaches its low-CPU idle path because an app
        // keeps invalidating for no reason" cases. Only redraw when something
        // could actually have changed this tick: a real UI event (ret>0), the
        // theme changed, or a fetch was in flight (poll_fetch() may finish it
        // on exactly this tick, so check BEFORE calling it too).
        int was_fetching = (g_state == ST_FETCHING);
        poll_fetch();
        if (ret > 0 || theme_changed || was_fetching) {
            draw_all();
            win_invalidate(win);
        }
    }

    if (g_job >= 0) http_fetch_cancel(g_job);
    win_destroy(win);
    return 0;
}
