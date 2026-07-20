// main.c - MayteraOS MFA / TOTP authenticator (#271)
// RFC 6238 time-based one-time passwords. Self-contained crypto in totp.c.
// Styled to match Settings (theme-following palette + TTF text).
//
// SECURITY NOTE: account secrets are persisted to /CONFIG/MFA.DB in a lightly
// obfuscated form (XOR scramble below). This is NOT real encryption; it only
// stops the secret being trivially readable. There is no passphrase / KDF.
//
// TIME NOTE: this OS's sys_time() returns SECONDS SINCE BOOT, not Unix epoch
// time, so it is useless for TOTP. We build a real UTC Unix timestamp from the
// RTC (date + time) syscalls instead. The RTC is assumed to be UTC.

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "totp.h"

// Render all text as TTF to match Settings.
#undef win_draw_text
#define win_draw_text(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

#define WIN_W 560
#define WIN_H 460
#define MAX_ACCOUNTS 32
#define DB_PATH "/CONFIG/MFA.DB"

// ---------------------------------------------------------------------------
// Theme palette (mirrors Settings color choices, picked from kernel get_theme)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_PANEL, COL_CARD, COL_SEP;
static uint32_t COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_BTN, COL_BTN_HOVER;
static uint32_t COL_RING_BG;

static void apply_theme(int kt) {
    // kt is kernel theme id (1=Dark,2=Light,4=Classic,5=Ocean,9=Nord, ...)
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_PANEL=0x00F0F0F0; COL_CARD=0x00F8F8F8; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00CCCCCC;
            COL_BTN=0x00E8E8E8; COL_BTN_HOVER=0x00D8D8D8; COL_RING_BG=0x00DDDDDD; break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_PANEL=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00808080;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_BTN=0x00C0C0C0; COL_BTN_HOVER=0x00D0D0D0; COL_RING_BG=0x00808080; break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_PANEL=0x001A3A4A; COL_CARD=0x001E4050; COL_SEP=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_TEXT_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_BTN=0x00305060; COL_BTN_HOVER=0x00406070; COL_RING_BG=0x00305060; break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_PANEL=0x002E3440; COL_CARD=0x00343B49; COL_SEP=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_TEXT_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_BTN=0x00434C5E; COL_BTN_HOVER=0x004C566A; COL_RING_BG=0x00434C5E; break;
        default: // Dark
            COL_BG=0x00252525; COL_PANEL=0x001E1E1E; COL_CARD=0x002A2A2A; COL_SEP=0x00404040;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_TEXT_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_BTN=0x00404040; COL_BTN_HOVER=0x00505050; COL_RING_BG=0x00404040; break;
    }
    // Mirror the palette into the style engine so gui_button etc. follow theme.
    gui_set_style(kt == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface = COL_BG; p.surface_raised = COL_CARD; p.ink = COL_TEXT; p.ink_dim = COL_TEXT2;
    p.accent = COL_ACCENT; p.accent_hover = COL_ACCENT; p.border = COL_SEP;
    p.field_bg = COL_FIELD; p.field_border = COL_FIELD_BORDER; p.track = COL_RING_BG;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Account model
// ---------------------------------------------------------------------------
typedef struct {
    char issuer[48];
    char label[48];
    uint8_t secret[64];   // decoded key bytes
    int secret_len;
    int digits;           // 6 or 8
    int period;           // seconds (default 30)
    totp_alg_t alg;       // SHA1 / SHA256
} account_t;

static account_t g_accts[MAX_ACCOUNTS];
static int g_count = 0;
static int g_selected = -1;     // selected account row (for delete)
static int g_status_flash = 0;  // ms-ish counter for transient status
static char g_status[80] = "";

// ---------------------------------------------------------------------------
// Unix time from RTC (UTC assumed)
// ---------------------------------------------------------------------------
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static uint64_t rtc_unix_time(void) {
    int hour, min, sec, day, mon, year;
    get_rtc_time(&hour, &min, &sec);
    get_rtc_date(&day, &mon, &year);
    if (year < 1970) return 0;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days = 0;
    for (int y = 1970; y < year; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 1; m < mon; m++) {
        days += mdays[m - 1];
        if (m == 2 && is_leap(year)) days += 1;
    }
    days += (uint64_t)(day - 1);
    return days * 86400ULL + (uint64_t)hour * 3600 + (uint64_t)min * 60 + (uint64_t)sec;
}

// ---------------------------------------------------------------------------
// Persistence (lightly obfuscated, NOT encrypted)
// File format (text-ish, line based) after a 1-byte XOR scramble of the whole
// payload with a fixed key. Plaintext layout:
//   MFADB1\n
//   <issuer>\t<label>\t<base32secret>\t<digits>\t<period>\t<alg>\n  (repeated)
// alg: 1=SHA1, 256=SHA256
// ---------------------------------------------------------------------------
#define OBF_KEY 0x5A

// Re-encode raw secret bytes back to base32 for storage.
static const char B32A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static int base32_encode(const uint8_t *in, int len, char *out, int cap) {
    int n = 0; uint32_t buf = 0; int bits = 0;
    for (int i = 0; i < len; i++) {
        buf = (buf << 8) | in[i]; bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (n >= cap - 1) return -1;
            out[n++] = B32A[(buf >> bits) & 0x1F];
        }
    }
    if (bits > 0) {
        if (n >= cap - 1) return -1;
        out[n++] = B32A[(buf << (5 - bits)) & 0x1F];
    }
    out[n] = 0;
    return n;
}

static void db_save(void) {
    char *buf = (char *)malloc(8192);
    if (!buf) return;
    int p = 0;
    p += snprintf(buf + p, 8192 - p, "MFADB1\n");
    for (int i = 0; i < g_count; i++) {
        char b32[128];
        base32_encode(g_accts[i].secret, g_accts[i].secret_len, b32, sizeof(b32));
        p += snprintf(buf + p, 8192 - p, "%s\t%s\t%s\t%d\t%d\t%d\n",
                      g_accts[i].issuer, g_accts[i].label, b32,
                      g_accts[i].digits, g_accts[i].period,
                      g_accts[i].alg == TOTP_SHA256 ? 256 : 1);
        if (p > 8000) break;
    }
    for (int i = 0; i < p; i++) buf[i] ^= OBF_KEY;
    sys_unlink(DB_PATH);
    int fd = sys_open(DB_PATH, O_WRONLY | O_CREAT);
    if (fd >= 0) { sys_write(fd, buf, (unsigned long)p); sys_close(fd); }
    free(buf);
}

static char *next_field(char **pp, char sep) {
    char *s = *pp;
    char *t = s;
    while (*t && *t != sep && *t != '\n') t++;
    if (*t) { *t = 0; *pp = t + 1; } else { *pp = t; }
    return s;
}

static void db_load(void) {
    g_count = 0;
    int fd = sys_open(DB_PATH, O_RDONLY);
    if (fd < 0) return;                 // missing -> start empty
    char *buf = (char *)malloc(8192);
    if (!buf) { sys_close(fd); return; }
    long n = sys_read(fd, buf, 8191);
    sys_close(fd);
    if (n <= 0) { free(buf); return; }
    for (long i = 0; i < n; i++) buf[i] ^= OBF_KEY;
    buf[n] = 0;
    char *p = buf;
    // header line
    if (strncmp(p, "MFADB1", 6) != 0) { free(buf); return; }   // bad/old file -> empty
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    while (*p && g_count < MAX_ACCOUNTS) {
        char *issuer = next_field(&p, '\t');
        char *label  = next_field(&p, '\t');
        char *b32    = next_field(&p, '\t');
        char *sd     = next_field(&p, '\t');
        char *sp     = next_field(&p, '\t');
        char *sa     = next_field(&p, '\t');
        if (!*b32) break;
        account_t *a = &g_accts[g_count];
        memset(a, 0, sizeof(*a));
        strlcpy(a->issuer, issuer, sizeof(a->issuer));
        strlcpy(a->label, label, sizeof(a->label));
        int sl = base32_decode(b32, a->secret, sizeof(a->secret));
        if (sl <= 0) continue;          // skip corrupt row, never crash
        a->secret_len = sl;
        a->digits = atoi(sd); if (a->digits != 6 && a->digits != 8) a->digits = 6;
        a->period = atoi(sp); if (a->period <= 0) a->period = 30;
        a->alg = (atoi(sa) == 256) ? TOTP_SHA256 : TOTP_SHA1;
        g_count++;
    }
    free(buf);
}

// ---------------------------------------------------------------------------
// otpauth:// URI parsing + add helpers
// ---------------------------------------------------------------------------
static void url_decode(char *s) {
    char *o = s;
    for (char *i = s; *i; ) {
        if (*i == '%' && i[1] && i[2]) {
            int hi = (i[1] >= 'a') ? i[1]-'a'+10 : (i[1] >= 'A') ? i[1]-'A'+10 : i[1]-'0';
            int lo = (i[2] >= 'a') ? i[2]-'a'+10 : (i[2] >= 'A') ? i[2]-'A'+10 : i[2]-'0';
            *o++ = (char)((hi << 4) | lo); i += 3;
        } else if (*i == '+') { *o++ = ' '; i++; }
        else *o++ = *i++;
    }
    *o = 0;
}

// Add account from explicit fields. Returns 0 ok, -1 bad secret.
static int add_account(const char *issuer, const char *label, const char *b32secret,
                       int digits, int period, totp_alg_t alg) {
    if (g_count >= MAX_ACCOUNTS) return -2;
    account_t a;
    memset(&a, 0, sizeof(a));
    int sl = base32_decode(b32secret, a.secret, sizeof(a.secret));
    if (sl <= 0) return -1;
    a.secret_len = sl;
    strlcpy(a.issuer, (issuer && *issuer) ? issuer : "(none)", sizeof(a.issuer));
    strlcpy(a.label, (label && *label) ? label : "account", sizeof(a.label));
    a.digits = (digits == 8) ? 8 : 6;
    a.period = (period > 0) ? period : 30;
    a.alg = alg;
    g_accts[g_count++] = a;
    db_save();
    return 0;
}

// Parse an otpauth://totp/Label?secret=...&issuer=...&digits=...&period=...&algorithm=...
// Returns 0 on success.
static int add_from_uri(const char *uri) {
    static char tmp[512];
    strlcpy(tmp, uri, sizeof(tmp));
    const char *pfx = "otpauth://totp/";
    if (strncasecmp(tmp, pfx, strlen(pfx)) != 0) return -1;
    char *rest = tmp + strlen(pfx);
    char *q = strchr(rest, '?');
    char labelpart[128] = "";
    if (q) { *q = 0; strlcpy(labelpart, rest, sizeof(labelpart)); rest = q + 1; }
    else   { strlcpy(labelpart, rest, sizeof(labelpart)); rest = rest + strlen(rest); }
    url_decode(labelpart);

    // labelpart may be "Issuer:Account"
    char issuer[48] = "", label[48] = "";
    char *colon = strchr(labelpart, ':');
    if (colon) { *colon = 0; strlcpy(issuer, labelpart, sizeof(issuer)); strlcpy(label, colon + 1, sizeof(label)); }
    else strlcpy(label, labelpart, sizeof(label));

    char secret[128] = ""; int digits = 6, period = 30; totp_alg_t alg = TOTP_SHA1;
    char *tok = rest;
    while (*tok) {
        char *amp = strchr(tok, '&');
        if (amp) *amp = 0;
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = 0; char *key = tok, *val = eq + 1; url_decode(val);
            if (strcasecmp(key, "secret") == 0)        strlcpy(secret, val, sizeof(secret));
            else if (strcasecmp(key, "issuer") == 0)   strlcpy(issuer, val, sizeof(issuer));
            else if (strcasecmp(key, "digits") == 0)   digits = atoi(val);
            else if (strcasecmp(key, "period") == 0)   period = atoi(val);
            else if (strcasecmp(key, "algorithm") == 0)
                alg = (strcasecmp(val, "SHA256") == 0) ? TOTP_SHA256 : TOTP_SHA1;
        }
        if (!amp) break;
        tok = amp + 1;
    }
    if (!secret[0]) return -1;
    return add_account(issuer, label, secret, digits, period, alg);
}

static void delete_account(int idx) {
    if (idx < 0 || idx >= g_count) return;
    for (int i = idx; i < g_count - 1; i++) g_accts[i] = g_accts[i + 1];
    g_count--;
    if (g_selected >= g_count) g_selected = g_count - 1;
    db_save();
}

// ---------------------------------------------------------------------------
// Add dialog state (modal)
// ---------------------------------------------------------------------------
static int g_modal = 0;          // 0 none, 1 add-fields, 2 add-uri
static int f_field = 0;          // active modal field index
// fields: 0 issuer, 1 label, 2 secret, 3 digits(6/8 toggle), 4 alg toggle
static char f_issuer[48], f_label[48], f_secret[128], f_uri[400];
static int f_digits8 = 0, f_alg256 = 0;

static void modal_reset(void) {
    f_issuer[0]=f_label[0]=f_secret[0]=f_uri[0]=0; f_field=0; f_digits8=0; f_alg256=0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static int window_handle;

static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); g_status_flash = 24; }

static void itoa6(uint32_t v, int digits, char *out) {
    // zero-padded code, grouped as "123 456" for 6 digits
    char tmp[16]; int n = 0;
    for (int i = 0; i < digits; i++) { tmp[i] = '0' + (v % 10); v /= 10; }
    // tmp is reversed
    int o = 0;
    for (int i = digits - 1; i >= 0; i--) {
        out[o++] = tmp[i];
        if (digits == 6 && i == 3) out[o++] = ' ';
        if (digits == 8 && i == 4) out[o++] = ' ';
    }
    out[o] = 0;
    n = o; (void)n;
}

// Fixed-point sin/cos tables (value scaled by 256), 0..360 inclusive.
static int g_sin256[361];
static int g_cos256[361];

// Draw a circular countdown ring using a fixed-point trig table (libm-free).
static void draw_ring(int cx, int cy, int r, int remaining, int period) {
    // background disc ring
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= r*r && d2 >= (r-3)*(r-3))
                win_draw_pixel(window_handle, cx+dx, cy+dy, COL_RING_BG);
        }
    }
    // foreground arc proportional to remaining/period, drawn clockwise from top
    int steps = 60;
    int lit = remaining * steps / (period > 0 ? period : 30);
    for (int s = 0; s < lit; s++) {
        int ang = (s * 360 / steps) % 361;   // degrees
        for (int t = 0; t < 3; t++) {
            int rr = r - 1 - t;
            int px = cx + (g_sin256[ang] * rr) / 256;
            int py = cy - (g_cos256[ang] * rr) / 256;
            win_draw_pixel(window_handle, px, py, COL_ACCENT);
        }
    }
}
static void build_trig(void) {
    // Taylor-free: integrate small angle steps. We approximate using the
    // identity rotation by 1 degree (cos1, sin1 in 1/256 units).
    // cos(1deg)=0.99985, sin(1deg)=0.01745 -> scaled by 65536 for precision.
    long c = 65536, s = 0;            // cos0=1, sin0=0 in Q16
    const long C1 = 65526;            // cos(1deg)*65536
    const long S1 = 1144;             // sin(1deg)*65536
    for (int a = 0; a <= 360; a++) {
        g_cos256[a] = (int)((c * 256) >> 16);
        g_sin256[a] = (int)((s * 256) >> 16);
        long nc = (c * C1 - s * S1) >> 16;
        long ns = (s * C1 + c * S1) >> 16;
        c = nc; s = ns;
    }
}

static void draw_account_row(int y, int idx, uint64_t now) {
    account_t *a = &g_accts[idx];
    int rowh = 64;
    uint32_t bg = (idx == g_selected) ? COL_BTN_HOVER : COL_CARD;
    gui_fill_rounded_aa(window_handle, 12, y, WIN_W - 24, rowh - 6, 6, bg, COL_BG);

    // issuer / label
    char title[100];
    if (a->issuer[0] && strcmp(a->issuer, "(none)") != 0)
        snprintf(title, sizeof(title), "%s", a->issuer);
    else
        snprintf(title, sizeof(title), "%s", a->label);
    draw_text_sz(window_handle, 24, y + 8, title, 14, COL_TEXT);
    if (a->label[0]) draw_text_sz(window_handle, 24, y + 30, a->label, 11, COL_TEXT2);

    // current code
    uint32_t code = totp_at_time(a->secret, a->secret_len, now, a->period, a->digits, a->alg);
    char cbuf[20]; itoa6(code, a->digits, cbuf);
    draw_text_sz(window_handle, 250, y + 14, cbuf, 22, COL_ACCENT);

    // countdown
    int rem = a->period - (int)(now % (uint64_t)a->period);
    int ringcx = WIN_W - 60, ringcy = y + (rowh - 6) / 2;
    draw_ring(ringcx, ringcy, 18, rem, a->period);
    char rb[8]; snprintf(rb, sizeof(rb), "%d", rem);
    int tw = (int)strlen(rb) * 7;
    draw_text_sz(window_handle, ringcx - tw/2, ringcy - 7, rb, 12, COL_TEXT);
}

static void draw_field(int x, int y, int w, const char *lbl, const char *val, int active) {
    draw_text_sz(window_handle, x, y, lbl, 11, COL_TEXT2);
    int fy = y + 16;
    gui_fill_rounded_aa(window_handle, x, fy, w, 26, 4, COL_FIELD, COL_CARD);
    gui_rounded_border(window_handle, x, fy, w, 26, 4, active ? COL_ACCENT : COL_FIELD_BORDER);
    draw_text_sz(window_handle, x + 6, fy + 5, val, 13, COL_TEXT);
    if (active) {
        int cx = x + 6 + (int)strlen(val) * 8;
        win_draw_rect(window_handle, cx, fy + 4, 1, 18, COL_TEXT);
    }
}

static void draw_modal(void) {
    // dim backdrop
    gui_fill_rounded_aa(window_handle, 40, 70, WIN_W - 80, WIN_H - 140, 8, COL_PANEL, COL_BG);
    gui_rounded_border(window_handle, 40, 70, WIN_W - 80, WIN_H - 140, 8, COL_SEP);
    int x = 64, y = 86;
    if (g_modal == 1) {
        draw_text_sz(window_handle, x, y, "Add account", 16, COL_TEXT); y += 30;
        draw_field(x, y, WIN_W - 128, "Issuer (e.g. GitHub)", f_issuer, f_field == 0); y += 50;
        draw_field(x, y, WIN_W - 128, "Label / account", f_label, f_field == 1); y += 50;
        draw_field(x, y, WIN_W - 128, "Secret (base32)", f_secret, f_field == 2); y += 50;
        // digits + alg toggles
        char db[32]; snprintf(db, sizeof(db), "Digits: %s", f_digits8 ? "8" : "6");
        char ab[32]; snprintf(ab, sizeof(ab), "Algo: %s", f_alg256 ? "SHA256" : "SHA1");
        gui_button(window_handle, x, y, 130, 28, db, f_field==3?GUI_BTN_PRIMARY:GUI_BTN_SECONDARY,
                   f_field==3?GUI_ST_FOCUS:GUI_ST_NORMAL);
        gui_button(window_handle, x + 150, y, 150, 28, ab, f_field==4?GUI_BTN_PRIMARY:GUI_BTN_SECONDARY,
                   f_field==4?GUI_ST_FOCUS:GUI_ST_NORMAL);
        y += 44;
        draw_text_sz(window_handle, x, y, "Tab: next field   Space: toggle   Enter: save   Esc: cancel", 11, COL_TEXT_DIM);
    } else if (g_modal == 2) {
        draw_text_sz(window_handle, x, y, "Add from otpauth:// URI", 16, COL_TEXT); y += 30;
        draw_field(x, y, WIN_W - 128, "Paste otpauth://totp/... URI", f_uri, 1); y += 56;
        draw_text_sz(window_handle, x, y, "Enter: parse + save   Esc: cancel", 11, COL_TEXT_DIM);
    }
}

// Button rects on the main toolbar
#define BTN_ADD_X 12
#define BTN_URI_X 120
#define BTN_DEL_X 250
#define BTN_Y 40
#define BTN_W 100
#define BTN_H 30

static void draw_all(void) {
    uint64_t now = rtc_unix_time();
    win_draw_rect(window_handle, 0, 0, WIN_W, WIN_H, COL_BG);

    // header
    draw_text_sz(window_handle, 14, 10, "Authenticator", 18, COL_TEXT);
    char tb[40];
    snprintf(tb, sizeof(tb), "%d account%s", g_count, g_count == 1 ? "" : "s");
    int tw = gui_ttf_width(tb, 11);
    draw_text_sz(window_handle, WIN_W - tw - 14, 16, tb, 11, COL_TEXT2);

    // toolbar buttons
    gui_button(window_handle, BTN_ADD_X, BTN_Y, BTN_W, BTN_H, "+ Add", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    gui_button(window_handle, BTN_URI_X, BTN_Y, 120, BTN_H, "Add URI", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(window_handle, BTN_DEL_X, BTN_Y, BTN_W, BTN_H, "Delete",
               GUI_BTN_SECONDARY, g_selected >= 0 ? GUI_ST_NORMAL : GUI_ST_DISABLED);

    win_draw_rect(window_handle, 12, 80, WIN_W - 24, 1, COL_SEP);

    if (g_count == 0) {
        draw_text_sz(window_handle, 24, 120, "No accounts yet.", 14, COL_TEXT2);
        draw_text_sz(window_handle, 24, 144, "Click + Add or Add URI to create one.", 12, COL_TEXT_DIM);
    } else {
        int y = 92;
        for (int i = 0; i < g_count && y < WIN_H - 40; i++) {
            draw_account_row(y, i, now);
            y += 64;
        }
    }

    // status line
    if (g_status_flash > 0 && g_status[0])
        draw_text_sz(window_handle, 14, WIN_H - 22, g_status, 12, COL_ACCENT);
    else
        draw_text_sz(window_handle, 14, WIN_H - 22, "Secrets stored obfuscated (not encrypted) in /CONFIG/MFA.DB", 10, COL_TEXT_DIM);

    if (g_modal) draw_modal();
    win_invalidate(window_handle);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void modal_char(char c) {
    char *target = NULL; int cap = 0;
    if (g_modal == 2) { target = f_uri; cap = sizeof(f_uri); }
    else switch (f_field) {
        case 0: target = f_issuer; cap = sizeof(f_issuer); break;
        case 1: target = f_label;  cap = sizeof(f_label);  break;
        case 2: target = f_secret; cap = sizeof(f_secret); break;
        default: target = NULL; break;
    }
    if (c == 27) { g_modal = 0; return; }                 // Esc
    if (c == '\t') { f_field = (f_field + 1) % 5; return; }
    if (c == '\r' || c == '\n') {
        if (g_modal == 2) {
            if (add_from_uri(f_uri) == 0) { set_status("Account added from URI"); g_modal = 0; }
            else set_status("Invalid otpauth URI");
        } else {
            int r = add_account(f_issuer, f_label, f_secret, f_digits8 ? 8 : 6, 30,
                                f_alg256 ? TOTP_SHA256 : TOTP_SHA1);
            if (r == 0) { set_status("Account added"); g_modal = 0; }
            else if (r == -1) set_status("Invalid base32 secret");
            else set_status("Account list full");
        }
        return;
    }
    if (g_modal == 1 && (f_field == 3 || f_field == 4)) {
        if (c == ' ') { if (f_field == 3) f_digits8 = !f_digits8; else f_alg256 = !f_alg256; }
        return;
    }
    if (!target) return;
    int len = (int)strlen(target);
    if (c == '\b') { if (len > 0) target[len - 1] = 0; return; }
    if (c >= 32 && c < 127 && len < cap - 1) { target[len] = c; target[len + 1] = 0; }
}

static void handle_click(int mx, int my) {
    if (g_modal) {
        if (g_modal == 1) {
            // toggle buttons live near bottom of dialog
            int x = 64, ty = 86 + 30 + 50*3 + 0;  // matches draw layout digit row
            // recompute: title30 + 3 fields*50 = 86+30+150 = 266 then y+=... toggles at 266
            ty = 266;
            if (my >= ty && my <= ty + 28) {
                if (mx >= x && mx <= x + 130) f_digits8 = !f_digits8;
                else if (mx >= x + 150 && mx <= x + 300) f_alg256 = !f_alg256;
            }
        }
        return;
    }
    // toolbar
    if (my >= BTN_Y && my <= BTN_Y + BTN_H) {
        if (mx >= BTN_ADD_X && mx <= BTN_ADD_X + BTN_W) { modal_reset(); g_modal = 1; return; }
        if (mx >= BTN_URI_X && mx <= BTN_URI_X + 120)   { modal_reset(); g_modal = 2; return; }
        if (mx >= BTN_DEL_X && mx <= BTN_DEL_X + BTN_W && g_selected >= 0) {
            delete_account(g_selected); set_status("Account deleted"); return;
        }
    }
    // account rows
    if (my >= 92) {
        int idx = (my - 92) / 64;
        if (idx >= 0 && idx < g_count) g_selected = (g_selected == idx) ? -1 : idx;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    build_trig();

    int kt = get_theme();
    apply_theme(kt);

    window_handle = win_create("Authenticator", 180, 120, WIN_W, WIN_H);
    if (window_handle < 0) return 1;

    db_load();

    // If launched with an otpauth:// URI argument, import it.
    if (argc > 1 && argv && argv[1] && strncasecmp(argv[1], "otpauth://", 10) == 0) {
        if (add_from_uri(argv[1]) == 0) set_status("Imported account from argument");
    }

    draw_all();

    gui_event_t ev;
    int running = 1;
    uint64_t last_step = 0;
    while (running) {
        int et = win_get_event(window_handle, &ev, 250);
        if (et == 0) {
            // periodic refresh so codes + countdown update
            uint64_t now = rtc_unix_time();
            if (now != last_step) { last_step = now; if (g_status_flash > 0) g_status_flash--; draw_all(); }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) { handle_click(ev.mouse_x, ev.mouse_y); draw_all(); }
                break;
            case EVENT_KEY_DOWN:
                if (g_modal) { modal_char(ev.key_char); draw_all(); }
                else if (ev.key_char == 27) running = 0;
                break;
            default: break;
        }
    }
    win_destroy(window_handle);
    return 0;
}
