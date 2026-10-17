// main.c - MayteraOS MFA / TOTP authenticator (#271, rebuilt on the secret-use
// proxy at #secretuse).
//
// ===========================================================================
// THIS APP NO LONGER HOLDS THE SECRETS, AND THAT IS THE ENTIRE POINT.
// ===========================================================================
// It used to. Every enrolled TOTP shared secret was in this process's memory and
// on disk under a single fixed XOR byte (`#define OBF_KEY 0x5A`), and this file
// computed the codes with its own private SHA-1, HMAC, base32 and truncation in
// totp.c. The app's own status line said "Secrets stored obfuscated (not
// encrypted)", which was honest and was also the problem: a fixed single-byte XOR
// is not a key, it is a formatting convention, so anything that could read the
// file WAS the second factor.
//
// Now the kernel holds them in an authenticated container encrypted under a key
// derived from the user's login password, and this app asks for A CODE through
// SYS_SECRET_USE. What was DELETED in the same change is what makes that real:
//   * db_load() / db_save()          - the app no longer reads or writes the store
//   * base32_encode / base32_decode  - it never sees a seed to encode
//   * totp.c / totp.h / totp_test.c  - SHA-1, HMAC-SHA1/256, TOTP, all gone
//   * account_t.secret[64]           - there is no field to put a secret in
//   * the RTC epoch clock            - the kernel owns the counter now
//
// A missing "read the secret" syscall would prove nothing on its own while this
// app still read the file. The deletions above are the proof; the container's
// file is root-only, so there is no route left, not merely no convenience.
//
// WHAT THIS APP CAN STILL DO, stated plainly so nobody thinks it is more than it
// is: it can read the CURRENT code for the logged-in user's accounts, which is
// what the user is looking at on screen anyway. It cannot obtain a seed, cannot
// ask for a code at another time, and stops working entirely when the session
// locks and the kernel wipes the key. See kernel/proc/secretuse.h.
//
// TIME: this app no longer has a clock. sys_time() returns SECONDS SINCE BOOT,
// which is useless for TOTP, and the RTC-to-epoch conversion that used to live
// here is deleted. SECRET_OP_CODES returns the code AND the seconds remaining,
// both computed against the KERNEL's clock, because a caller-chosen time is the
// whole difference between a proxy and an oracle (docs/OPEN_BACKLOG.md A4).

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
// NO totp.h: this app has no crypto of its own any more, and no userconf.h:
// it does not know or care where the store lives, because it cannot read it.

// Render all text as TTF to match Settings.
#undef win_draw_text
#define win_draw_text(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

#define WIN_W 560
#define WIN_H 460
#define MAX_ACCOUNTS SECRET_MAX_ACCOUNTS

// THERE IS NO db_path() ANY MORE, and its absence is deliberate. This app does
// not know where the store is, cannot name it, and would be refused if it tried:
// the container lives under a root-owned 0700 directory and is encrypted under a
// key this process has never seen. The previous version of this comment spent
// twenty lines reasoning about which directory the seeds should sit in and what
// the file mode should be, which was the right argument to have while Ring 3 owned
// the file. The answer turned out to be that Ring 3 should not own the file.

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
// NOTE WHAT IS NOT IN THIS STRUCT. There is no `secret` and no `secret_len`. A
// field that does not exist cannot be filled in by a later change that forgets
// why, which is a stronger guarantee than a comment asking nobody to add one.
// These are the kernel's secret_totp_info_t fields, and the kernel's struct has
// no secret field either.
typedef struct {
    char issuer[SECRET_NAME_MAX];
    char label[SECRET_NAME_MAX];
    int digits;           // 6 or 8
    int period;           // seconds
    int alg;              // 1 = SHA1, 256 = SHA256
} account_t;

static account_t g_accts[MAX_ACCOUNTS];
// The CURRENT code and countdown for each account, as the kernel computed them.
// Refreshed once a second by one syscall; never derived here.
static secret_totp_code_t g_codes[MAX_ACCOUNTS];
static int g_count = 0;
// The kernel refused because there is no session key (the user has not
// authenticated, or the session was locked and the key was wiped). This is NOT
// "no accounts", and telling the two apart is one of the reasons the change was
// worth making: the old app reported every failure as an empty list.
static int g_locked = 0;
static int g_selected = -1;     // selected account row (for delete)
static int g_status_flash = 0;  // ms-ish counter for transient status
static char g_status[80] = "";

// THE RTC-TO-EPOCH CONVERSION THAT USED TO BE HERE IS DELETED. It existed only
// to build the TOTP counter, and the counter is now the kernel's: it reads
// realtime_sec_rs() and returns the code plus the seconds remaining. The app
// keeps a seconds-since-boot tick (sys_time()) purely to decide WHEN to redraw,
// which needs no notion of the actual date and cannot influence any code.

// ---------------------------------------------------------------------------
// The store, as seen from Ring 3: ONE syscall, and nothing kept.
//
// Everything that used to live in this section is gone: db_save(), db_load(),
// base32_encode(), the tab-separated line parser and the XOR key. The app cannot
// open the store, so there is nothing here to parse.
//
// refresh_names() is called after a change; refresh_codes() once a second. They
// are separate because names and geometry do not change every second and a code
// does, and because SECRET_OP_LIST cannot ever return a secret while
// SECRET_OP_CODES cannot ever return anything but the CURRENT one.
// ---------------------------------------------------------------------------
static int g_last_rc = 0;

static void refresh_names(void) {
    secret_totp_info_t info[MAX_ACCOUNTS];
    int n = sys_secret_list(info, MAX_ACCOUNTS);
    g_last_rc = n;
    if (n == SECRET_E_LOCKED) { g_locked = 1; g_count = 0; return; }
    g_locked = 0;
    if (n < 0) { g_count = 0; return; }
    if (n > MAX_ACCOUNTS) n = MAX_ACCOUNTS;
    for (int i = 0; i < n; i++) {
        strlcpy(g_accts[i].issuer, info[i].issuer, sizeof(g_accts[i].issuer));
        strlcpy(g_accts[i].label,  info[i].label,  sizeof(g_accts[i].label));
        g_accts[i].digits = info[i].digits;
        g_accts[i].period = info[i].period;
        g_accts[i].alg    = info[i].alg;
    }
    g_count = n;
    if (g_selected >= g_count) g_selected = g_count - 1;
}

static void refresh_codes(void) {
    if (g_count <= 0) return;
    int n = sys_secret_codes(g_codes, MAX_ACCOUNTS);
    if (n == SECRET_E_LOCKED) { g_locked = 1; g_count = 0; return; }
    if (n < 0) {
        // Blank every code rather than leaving the last good ones on screen. A
        // stale code looks exactly like a current one and the user cannot tell,
        // so showing nothing is the only honest failure.
        for (int i = 0; i < MAX_ACCOUNTS; i++) {
            g_codes[i].code[0] = 0;
            g_codes[i].remaining = 0;
        }
        g_last_rc = n;
    }
}

static void refresh_all(void) { refresh_names(); refresh_codes(); }

// What went wrong, in words a user can act on. The old app had one failure mode
// ("no accounts") for every cause, which is how a broken store looked like an
// empty one.
static const char *secret_err_text(int rc) {
    switch (rc) {
        case SECRET_E_LOCKED:  return "Locked: log in again to unlock your accounts";
        case SECRET_E_AUTH:    return "Store will not open: wrong key, or it was altered";
        case SECRET_E_FORMAT:  return "Store is not readable (report this)";
        case SECRET_E_NOCLOCK: return "No reliable clock: codes would be wrong";
        case SECRET_E_FULL:    return "Account list full";
        case SECRET_E_NOSPACE: return "Too many accounts to display";
        case SECRET_E_NOTIMPL: return "This secret type is not supported yet";
        case SECRET_E_IO:      return "Could not reach the secret store";
        default:               return "Refused";
    }
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

// Hand the seed to the kernel. THIS IS THE ONLY DIRECTION A SECRET EVER TRAVELS.
//
// It has to travel this way once, because the user is typing or pasting it, so
// Ring 3 necessarily holds it for the duration of this call. The property is that
// it never comes back: there is no operation that returns it, the local copy is
// wiped below, and after this function the app has no way to learn it again.
//
// Returns 0, or the kernel's negative SECRET_E_*.
static int add_account(const char *issuer, const char *label, const char *b32secret,
                       int digits, int period, int alg) {
    if (g_count >= MAX_ACCOUNTS) return SECRET_E_FULL;
    secret_totp_add_t a;
    memset(&a, 0, sizeof(a));
    strlcpy(a.issuer, (issuer && *issuer) ? issuer : "(none)", sizeof(a.issuer));
    strlcpy(a.label, (label && *label) ? label : "account", sizeof(a.label));
    strlcpy(a.b32, b32secret ? b32secret : "", sizeof(a.b32));
    a.digits = (digits == 8) ? 8 : 6;
    a.period = (period > 0) ? period : 30;
    a.alg = (alg == 256) ? 256 : 1;
    int rc = sys_secret_add(&a);
    // Wipe our copy of the seed as soon as the kernel has it. The base32 text is
    // also still sitting in the modal field, which clear_secret_fields() handles
    // on the caller's side; both matter, because either one left behind is the
    // seed in Ring 3 memory that this whole change exists to remove.
    memset(&a, 0, sizeof(a));
    if (rc == 0) refresh_all();
    return rc;
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

    char secret[128] = ""; int digits = 6, period = 30; int alg = 1;
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
                alg = (strcasecmp(val, "SHA256") == 0) ? 256 : 1;
        }
        if (!amp) break;
        tok = amp + 1;
    }
    if (!secret[0]) return SECRET_E_ARG;
    int rc = add_account(issuer, label, secret, digits, period, alg);
    memset(secret, 0, sizeof(secret));   // the parsed seed does not outlive the call
    return rc;
}

static int delete_account(int idx) {
    if (idx < 0 || idx >= g_count) return SECRET_E_ARG;
    // The kernel removes the record, wipes its secret from kernel memory, and
    // re-seals the store. The app does not edit a list and save it; it asks.
    int rc = sys_secret_del((unsigned)idx);
    if (rc == 0) refresh_all();
    return rc;
}

// ---------------------------------------------------------------------------
// Add dialog state (modal)
// ---------------------------------------------------------------------------
static int g_modal = 0;          // 0 none, 1 add-fields, 2 add-uri
static int f_field = 0;          // active modal field index
// fields: 0 issuer, 1 label, 2 secret, 3 digits(6/8 toggle), 4 alg toggle
static char f_issuer[48], f_label[48], f_secret[128], f_uri[400];
static int f_digits8 = 0, f_alg256 = 0;

// memset, not `[0]=0`. Truncating the string leaves the typed base32 seed in this
// process's memory for as long as the buffer lives, which is the exact thing this
// change exists to stop. It costs nothing to clear it properly.
static void modal_reset(void) {
    memset(f_issuer, 0, sizeof(f_issuer));
    memset(f_label,  0, sizeof(f_label));
    memset(f_secret, 0, sizeof(f_secret));
    memset(f_uri,    0, sizeof(f_uri));
    f_field=0; f_digits8=0; f_alg256=0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static int window_handle;

static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); g_status_flash = 24; }

// Insert a readability space in the middle of the kernel's code STRING: "123 456"
// for 6 digits, "1234 5678" for 8. It used to take a uint32_t and do the decimal
// conversion itself, which it can no longer do because it is never given a number.
// Purely cosmetic, and bounded by `cap`.
static void group_code(const char *code, char *out, int cap) {
    int n = (int)strlen(code);
    int mid = (n == 6) ? 3 : (n == 8) ? 4 : -1;
    int o = 0;
    for (int i = 0; i < n && o < cap - 2; i++) {
        if (i == mid) out[o++] = ' ';
        out[o++] = code[i];
    }
    out[o] = 0;
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

static void draw_account_row(int y, int idx) {
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

    // The current code, AS THE KERNEL COMPUTED IT. Nothing here derives it, and
    // there is no secret in scope with which to try.
    char cbuf[24];
    group_code(g_codes[idx].code, cbuf, sizeof(cbuf));
    draw_text_sz(window_handle, 250, y + 14, cbuf[0] ? cbuf : "------", 22, COL_ACCENT);

    // The countdown is also the kernel's: it came back with the code, from the
    // same clock reading, so the two can never disagree about which window this is.
    int rem = g_codes[idx].remaining;
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
    gui_rounded_border(window_handle, x, fy, w, 26, 4, active ? gui_pal()->focus : COL_FIELD_BORDER);  // #745
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

    if (g_locked) {
        // NOT the same screen as "no accounts", deliberately. The old app could
        // not tell these apart and showed the empty state for both, so a user
        // whose store had failed to load was told they had never enrolled.
        draw_text_sz(window_handle, 24, 120, "Accounts are locked.", 14, COL_TEXT2);
        draw_text_sz(window_handle, 24, 144,
                     "Your codes are encrypted with your login password. Log in "
                     "again to unlock them.", 12, COL_TEXT_DIM);
    } else if (g_count == 0 && g_last_rc < 0) {
        draw_text_sz(window_handle, 24, 120, secret_err_text(g_last_rc), 14, COL_TEXT2);
        draw_text_sz(window_handle, 24, 144,
                     "Nothing was changed. Your accounts are still in the store.",
                     12, COL_TEXT_DIM);
    } else if (g_count == 0) {
        draw_text_sz(window_handle, 24, 120, "No accounts yet.", 14, COL_TEXT2);
        draw_text_sz(window_handle, 24, 144, "Click + Add or Add URI to create one.", 12, COL_TEXT_DIM);
    } else {
        int y = 92;
        for (int i = 0; i < g_count && y < WIN_H - 40; i++) {
            draw_account_row(y, i);
            y += 64;
        }
    }

    // status line
    if (g_status_flash > 0 && g_status[0])
        draw_text_sz(window_handle, 14, WIN_H - 22, g_status, 12, COL_ACCENT);
    else
        {
            // The old line here read "Secrets stored obfuscated (not encrypted)"
            // and named the file. Both facts have changed, and the replacement is
            // written to be true rather than reassuring: it says where the
            // protection comes from and it does not claim the app is trusted.
            draw_text_sz(window_handle, 14, WIN_H - 22,
                         "Seeds are held by the kernel, encrypted with your login "
                         "password. This app can read codes, not seeds.",
                         10, COL_TEXT_DIM);
        }

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
    if (c == 27) { modal_reset(); g_modal = 0; return; }  // Esc: also wipes the seed
    if (c == '\t') { f_field = (f_field + 1) % 5; return; }
    if (c == '\r' || c == '\n') {
        if (g_modal == 2) {
            int r = add_from_uri(f_uri);
            if (r == 0) { set_status("Account added from URI"); modal_reset(); g_modal = 0; }
            else if (r == SECRET_E_ARG) set_status("Invalid otpauth URI");
            else set_status(secret_err_text(r));
        } else {
            int r = add_account(f_issuer, f_label, f_secret, f_digits8 ? 8 : 6, 30,
                                f_alg256 ? 256 : 1);
            // modal_reset() on success is what wipes the typed seed out of this
            // process. On failure the field is deliberately kept so the user can
            // fix a typo, which means the seed is still in memory until they
            // cancel or succeed; that is a real and unavoidable window, and it is
            // the same one they are looking at on screen.
            if (r == 0) { set_status("Account added"); modal_reset(); g_modal = 0; }
            else if (r == SECRET_E_FORMAT) set_status("Invalid base32 secret");
            else set_status(secret_err_text(r));
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
            int r = delete_account(g_selected);
            set_status(r == 0 ? "Account deleted" : secret_err_text(r));
            return;
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

    refresh_all();

    // If launched with an otpauth:// URI argument, import it.
    if (argc > 1 && argv && argv[1] && strncasecmp(argv[1], "otpauth://", 10) == 0) {
        if (add_from_uri(argv[1]) == 0) set_status("Imported account from argument");
    }

    draw_all();

    gui_event_t ev;
    int running = 1;
    // SECONDS SINCE BOOT, and that is all this is for: deciding when a second has
    // elapsed so the codes are re-fetched and redrawn. It is NOT a wall clock and
    // is never used to compute anything, because the kernel owns the TOTP counter.
    unsigned long last_step = 0;
    while (running) {
        int et = win_get_event(window_handle, &ev, 250);
        if (et == 0) {
            unsigned long tick = (unsigned long)sys_time();
            if (tick != last_step) {
                last_step = tick;
                if (g_status_flash > 0) g_status_flash--;
                refresh_codes();
                draw_all();
            }
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
