// sshterm.c - GUI SSH terminal app for MayteraOS. See sshterm.h.
//
// A windowed 80x24 VT100 terminal driven by net/ssh/ssh2. The handshake + I/O
// run in a background kernel thread so the desktop stays responsive; output is
// fed through a compact VT100 parser into a cell grid and rendered each frame.
#include "sshterm.h"
#include "window.h"
#include "desktop.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../fs/fat.h"
#include "../proc/process.h"
#include "../net/ssh/ssh2.h"

#define ST_COLS   80
#define ST_ROWS   24
#define ST_CW     FONT_WIDTH    // 8
#define ST_CH     FONT_HEIGHT   // 16
#define ST_PAD    3

#define ST_BG     0x101418      // default background (near-black)
#define ST_FG     0xCCCCCC      // default foreground

// ANSI 16-colour palette (0xRRGGBB)
static const uint32_t st_pal[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};

typedef struct {
    char     ch;
    uint8_t  fg;     // palette index, 0xFF = default
    uint8_t  bg;     // palette index, 0xFF = default
    uint8_t  bold;
    uint8_t  inv;
} st_cell_t;

typedef struct {
    window_t *window;
    int app_id;
    int dock_index;

    ssh2_client_t *cli;
    char host[80], user[80], pass[160];
    int  port;

    st_cell_t grid[ST_ROWS][ST_COLS];
    int cx, cy;
    uint8_t cur_fg, cur_bg, cur_bold, cur_inv;

    // VT100 parser
    int  esc;            // 0 normal, 1 ESC, 2 CSI, 3 OSC
    int  params[8];
    int  nparam;
    int  priv;           // CSI private '?'/'>' seen
    int  has_param;

    volatile int running;
    int pump_pid;
    int connected;
    char status[96];
} sshterm_t;

static sshterm_t *g_active_ssh = 0;

// Parse "a.b.c.d" -> stack IP convention (same as net/remote_ctrl rc_parse_ip).
static uint32_t st_parse_ip(const char *s) {
    uint32_t parts[4] = {0,0,0,0}; int idx = 0; const char *p = s;
    while (*p && idx < 4) {
        while (*p >= '0' && *p <= '9') { parts[idx] = parts[idx]*10 + (uint32_t)(*p - '0'); p++; }
        idx++;
        if (*p == '.') p++;
    }
    if (idx < 4) return 0;
    return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
}

// ---- grid helpers ----------------------------------------------------------
static void st_clear_cell(sshterm_t *t, int r, int c) {
    t->grid[r][c].ch = ' ';
    t->grid[r][c].fg = 0xFF; t->grid[r][c].bg = 0xFF;
    t->grid[r][c].bold = 0; t->grid[r][c].inv = 0;
}
static void st_clear_all(sshterm_t *t) {
    for (int r = 0; r < ST_ROWS; r++) for (int c = 0; c < ST_COLS; c++) st_clear_cell(t, r, c);
    t->cx = t->cy = 0;
}
static void st_scroll(sshterm_t *t) {
    for (int r = 0; r < ST_ROWS - 1; r++)
        for (int c = 0; c < ST_COLS; c++) t->grid[r][c] = t->grid[r + 1][c];
    for (int c = 0; c < ST_COLS; c++) st_clear_cell(t, ST_ROWS - 1, c);
}
static void st_newline(sshterm_t *t) {
    t->cy++;
    if (t->cy >= ST_ROWS) { st_scroll(t); t->cy = ST_ROWS - 1; }
}

// ---- VT100 parser ----------------------------------------------------------
static void st_exec_csi(sshterm_t *t, char fin) {
    int p0 = t->nparam > 0 ? t->params[0] : 0;
    int p1 = t->nparam > 1 ? t->params[1] : 0;
    switch (fin) {
    case 'm': {  // SGR
        if (t->nparam == 0) { t->cur_fg = 0xFF; t->cur_bg = 0xFF; t->cur_bold = 0; t->cur_inv = 0; break; }
        for (int i = 0; i < t->nparam; i++) {
            int p = t->params[i];
            if (p == 0) { t->cur_fg = 0xFF; t->cur_bg = 0xFF; t->cur_bold = 0; t->cur_inv = 0; }
            else if (p == 1) t->cur_bold = 1;
            else if (p == 7) t->cur_inv = 1;
            else if (p == 22) t->cur_bold = 0;
            else if (p == 27) t->cur_inv = 0;
            else if (p >= 30 && p <= 37) t->cur_fg = (uint8_t)(p - 30);
            else if (p == 39) t->cur_fg = 0xFF;
            else if (p >= 40 && p <= 47) t->cur_bg = (uint8_t)(p - 40);
            else if (p == 49) t->cur_bg = 0xFF;
            else if (p >= 90 && p <= 97) t->cur_fg = (uint8_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) t->cur_bg = (uint8_t)(p - 100 + 8);
        }
        break;
    }
    case 'H': case 'f': {  // cursor position (1-based)
        int row = (t->nparam > 0 ? p0 : 1); int col = (t->nparam > 1 ? p1 : 1);
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        t->cy = row - 1; t->cx = col - 1;
        if (t->cy >= ST_ROWS) t->cy = ST_ROWS - 1;
        if (t->cx >= ST_COLS) t->cx = ST_COLS - 1;
        break;
    }
    case 'A': t->cy -= (p0 ? p0 : 1); if (t->cy < 0) t->cy = 0; break;
    case 'B': t->cy += (p0 ? p0 : 1); if (t->cy >= ST_ROWS) t->cy = ST_ROWS - 1; break;
    case 'C': t->cx += (p0 ? p0 : 1); if (t->cx >= ST_COLS) t->cx = ST_COLS - 1; break;
    case 'D': t->cx -= (p0 ? p0 : 1); if (t->cx < 0) t->cx = 0; break;
    case 'G': t->cx = (p0 ? p0 - 1 : 0); if (t->cx < 0) t->cx = 0; if (t->cx >= ST_COLS) t->cx = ST_COLS - 1; break;
    case 'd': t->cy = (p0 ? p0 - 1 : 0); if (t->cy < 0) t->cy = 0; if (t->cy >= ST_ROWS) t->cy = ST_ROWS - 1; break;
    case 'J': {  // erase display
        int mode = p0;
        if (mode == 2 || mode == 3) { st_clear_all(t); }
        else if (mode == 0) { for (int c = t->cx; c < ST_COLS; c++) st_clear_cell(t, t->cy, c);
                              for (int r = t->cy + 1; r < ST_ROWS; r++) for (int c = 0; c < ST_COLS; c++) st_clear_cell(t, r, c); }
        else if (mode == 1) { for (int r = 0; r < t->cy; r++) for (int c = 0; c < ST_COLS; c++) st_clear_cell(t, r, c);
                              for (int c = 0; c <= t->cx && c < ST_COLS; c++) st_clear_cell(t, t->cy, c); }
        break;
    }
    case 'K': {  // erase line
        int mode = p0;
        if (mode == 0) { for (int c = t->cx; c < ST_COLS; c++) st_clear_cell(t, t->cy, c); }
        else if (mode == 1) { for (int c = 0; c <= t->cx && c < ST_COLS; c++) st_clear_cell(t, t->cy, c); }
        else if (mode == 2) { for (int c = 0; c < ST_COLS; c++) st_clear_cell(t, t->cy, c); }
        break;
    }
    default: break;   // h/l (modes), r (scroll region), etc. ignored
    }
}

static void st_putc(sshterm_t *t, uint8_t b) {
    switch (t->esc) {
    case 0:
        if (b == 0x1b) { t->esc = 1; return; }
        if (b == '\r') { t->cx = 0; return; }
        if (b == '\n') { st_newline(t); return; }
        if (b == '\b') { if (t->cx > 0) t->cx--; return; }
        if (b == '\t') { t->cx = (t->cx / 8 + 1) * 8; if (t->cx >= ST_COLS) t->cx = ST_COLS - 1; return; }
        if (b == 0x07) return;          // bell
        if (b < 0x20) return;           // other control
        if (t->cx >= ST_COLS) { t->cx = 0; st_newline(t); }
        t->grid[t->cy][t->cx].ch = (char)b;
        t->grid[t->cy][t->cx].fg = t->cur_fg;
        t->grid[t->cy][t->cx].bg = t->cur_bg;
        t->grid[t->cy][t->cx].bold = t->cur_bold;
        t->grid[t->cy][t->cx].inv = t->cur_inv;
        t->cx++;
        return;
    case 1:  // after ESC
        if (b == '[') { t->esc = 2; t->nparam = 0; t->priv = 0; t->has_param = 0; t->params[0] = 0; return; }
        if (b == ']') { t->esc = 3; return; }     // OSC
        // ESC (B, ESC = etc: consume one byte and return to normal
        t->esc = 0; return;
    case 2:  // CSI
        if (b == '?' || b == '>' || b == '=') { t->priv = b; return; }
        if (b >= '0' && b <= '9') {
            if (t->nparam == 0) t->nparam = 1;
            t->params[t->nparam - 1] = t->params[t->nparam - 1] * 10 + (b - '0');
            t->has_param = 1;
            return;
        }
        if (b == ';') { if (t->nparam < 8) { t->nparam++; t->params[t->nparam - 1] = 0; } return; }
        if (b >= 0x40 && b <= 0x7e) { st_exec_csi(t, (char)b); t->esc = 0; return; }
        t->esc = 0; return;
    case 3:  // OSC: consume until BEL or ESC
        if (b == 0x07 || b == 0x1b) t->esc = 0;
        return;
    }
}

static void st_feed(void *ctx, const uint8_t *data, int len) {
    sshterm_t *t = (sshterm_t *)ctx;
    for (int i = 0; i < len; i++) st_putc(t, data[i]);
    if (t->window) wm_invalidate_rect(&t->window->bounds);
}
static void st_puts(sshterm_t *t, const char *s) { while (*s) st_putc(t, (uint8_t)*s++); }
static void st_log(void *ctx, const char *msg) { st_puts((sshterm_t *)ctx, msg); }

// ---- rendering -------------------------------------------------------------
static void st_draw(sshterm_t *t) {
    if (!t || !t->window) return;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(t->window, &wx, &wy, &ww, &wh);
    fb_fill_rect(wx, wy, ww, wh, ST_BG);
    for (int r = 0; r < ST_ROWS; r++) {
        for (int c = 0; c < ST_COLS; c++) {
            st_cell_t *cell = &t->grid[r][c];
            int px = wx + ST_PAD + c * ST_CW;
            int py = wy + ST_PAD + r * ST_CH;
            uint32_t fg = (cell->fg == 0xFF) ? ST_FG : st_pal[cell->fg & 15];
            uint32_t bg = (cell->bg == 0xFF) ? ST_BG : st_pal[cell->bg & 15];
            if (cell->bold && cell->fg != 0xFF && (cell->fg & 15) < 8) fg = st_pal[(cell->fg & 7) + 8];
            if (cell->inv) { uint32_t tmp = fg; fg = bg; bg = tmp; }
            if (bg != ST_BG) fb_fill_rect(px, py, ST_CW, ST_CH, bg);
            char ch = cell->ch;
            if (ch > ' ' && ch < 127) {
                const uint8_t *g = font_get_glyph(ch);
                if (g) for (int yy = 0; yy < ST_CH && yy < FONT_HEIGHT; yy++) {
                    uint8_t bits = g[yy];
                    for (int xx = 0; xx < FONT_WIDTH; xx++)
                        if (bits & (0x80 >> xx)) fb_put_pixel(px + xx, py + yy, fg);
                }
            }
        }
    }
    // cursor (block underline) when connected
    if (t->connected && t->cx >= 0 && t->cx < ST_COLS && t->cy >= 0 && t->cy < ST_ROWS) {
        int px = wx + ST_PAD + t->cx * ST_CW;
        int py = wy + ST_PAD + t->cy * ST_CH;
        fb_fill_rect(px, py + ST_CH - 2, ST_CW, 2, 0x33FF33);
    }
}

// ---- background connect + I/O pump ----------------------------------------
static void st_pump_thread(void *arg) {
    sshterm_t *t = (sshterm_t *)arg;
    if (!t) return;
    t->running = 1;

    ssh2_set_log(st_log, t);
    st_puts(t, "Connecting to ");
    st_puts(t, t->host);
    st_puts(t, " ...\r\n");
    if (t->window) wm_invalidate_rect(&t->window->bounds);

    uint32_t ip = st_parse_ip(t->host);
    int rc = -1;
    if (ip) rc = ssh2_connect(t->cli, ip, (uint16_t)t->port, t->user, t->pass,
                              ST_COLS, ST_ROWS, st_feed, t);
    ssh2_set_log(0, 0);

    if (rc != 0) {
        st_puts(t, "\r\nSSH connect failed: ");
        st_puts(t, ip ? t->cli->err : "bad host address");
        st_puts(t, "\r\n");
        if (t->window) wm_invalidate_rect(&t->window->bounds);
        t->running = 0;
        return;
    }
    t->connected = 1;
    if (t->window) wm_invalidate_rect(&t->window->bounds);

    while (t->running && !t->cli->closed) {
        int n = ssh2_pump(t->cli);   // feeds st_feed on data
        if (n <= 0) proc_sleep(15);
    }
    if (t->cli->closed) {
        st_puts(t, "\r\n[connection closed]\r\n");
        t->connected = 0;
        if (t->window) wm_invalidate_rect(&t->window->bounds);
    }
    t->running = 0;
}

// ---- WM callbacks ----------------------------------------------------------
static void st_send_key(sshterm_t *t, int c) {
    if (!t->connected || !t->cli) return;
    if (c >= 1 && c < 0x80) {
        uint8_t b = (uint8_t)c;
        if (b == '\n') b = '\r';
        ssh2_send_input(t->cli, &b, 1);
        return;
    }
    const char *seq = 0;
    switch (c) {
    case 0x80: seq = "\x1b[A"; break;   // up
    case 0x81: seq = "\x1b[B"; break;   // down
    case 0x82: seq = "\x1b[D"; break;   // left
    case 0x83: seq = "\x1b[C"; break;   // right
    default: return;
    }
    int n = 0; while (seq[n]) n++;
    ssh2_send_input(t->cli, seq, n);
}

static void st_on_event(void *app_data, gui_event_t *ev) {
    sshterm_t *t = (sshterm_t *)app_data;
    if (!t || !ev) return;
    switch (ev->type) {
    case EVENT_KEY_DOWN:
        st_send_key(t, (unsigned char)ev->key_char);
        break;
    case EVENT_WINDOW_CLOSE:
        t->running = 0;
        if (t->cli) ssh2_close(t->cli);
        wm_unregister_app(t->app_id);
        if (t->dock_index >= 0) dock_remove_app(t->dock_index);
        if (g_active_ssh == t) g_active_ssh = 0;
        window_hide(t->window);
        wm_invalidate_all();
        break;
    default:
        break;
    }
}
static void st_on_draw(void *app_data) { st_draw((sshterm_t *)app_data); }
static void st_on_destroy(void *app_data) {
    sshterm_t *t = (sshterm_t *)app_data;
    if (!t) return;
    t->running = 0;
    if (t->cli) { ssh2_close(t->cli); kfree(t->cli); t->cli = 0; }
    if (g_active_ssh == t) g_active_ssh = 0;
    kfree(t);
}

// ---- config: /CONFIG/SSH.CFG (host=/user=/pass=/port=) ----------------------
static void st_trim(char *s) {
    int n = 0; while (s[n]) n++;
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ')) s[--n] = 0;
}
static int st_load_cfg(sshterm_t *t) {
    extern fat_fs_t g_fat_fs;
    uint32_t sz = 0;
    char *buf = (char *)fat_read_file(&g_fat_fs, "/CONFIG/SSH.CFG", &sz);
    if (!buf || sz == 0) { if (buf) kfree(buf); return -1; }
    t->port = 22;
    char line[200]; int li = 0;
    for (uint32_t i = 0; i <= sz; i++) {
        char c = (i < sz) ? buf[i] : '\n';
        if (c == '\n' || li >= (int)sizeof(line) - 1) {
            line[li] = 0; li = 0;
            st_trim(line);
            char *eq = 0; for (char *p = line; *p; p++) if (*p == '=') { eq = p; break; }
            if (eq) {
                *eq = 0; const char *k = line; const char *v = eq + 1;
                if (!strcmp(k, "host")) { strncpy(t->host, v, sizeof(t->host) - 1); }
                else if (!strcmp(k, "user")) { strncpy(t->user, v, sizeof(t->user) - 1); }
                else if (!strcmp(k, "pass")) { strncpy(t->pass, v, sizeof(t->pass) - 1); }
                else if (!strcmp(k, "port")) { int p = 0; for (const char *q = v; *q >= '0' && *q <= '9'; q++) p = p*10 + (*q - '0'); if (p > 0) t->port = p; }
            }
        } else line[li++] = c;
    }
    kfree(buf);
    return (t->host[0] && t->user[0]) ? 0 : -1;
}

void sshterm_launch(void) {
    kprintf("[SSHterm] launching\n");
    sshterm_t *t = (sshterm_t *)kmalloc(sizeof(sshterm_t));
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->app_id = -1; t->dock_index = -1; t->pump_pid = -1;
    t->cur_fg = 0xFF; t->cur_bg = 0xFF;
    st_clear_all(t);

    t->cli = (ssh2_client_t *)kmalloc(sizeof(ssh2_client_t));
    if (!t->cli) { kfree(t); return; }
    memset(t->cli, 0, sizeof(*t->cli));

    int have_cfg = (st_load_cfg(t) == 0);

    int width = ST_COLS * ST_CW + 2 * ST_PAD + 4;
    int height = ST_ROWS * ST_CH + 2 * ST_PAD + TITLEBAR_HEIGHT + 4;
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    int x = ((int)sw - width) / 2; int y = ((int)sh - height) / 2 - 40;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    t->window = window_create("SSH", x, y, width, height);
    if (!t->window) { kfree(t->cli); kfree(t); return; }
    t->window->bg_color = ST_BG;

    t->dock_index = dock_add_app("SSH", 0x00AAAA, NULL);
    t->app_id = wm_register_app(t->window, t, st_on_event, st_on_draw, st_on_destroy);
    if (t->app_id < 0) { window_hide(t->window); kfree(t->cli); kfree(t); return; }
    g_active_ssh = t;
    wm_focus_window(t->window);

    if (!have_cfg) {
        st_puts(t, "MayteraOS SSH client\r\n\r\n");
        st_puts(t, "No /CONFIG/SSH.CFG found. Create it with:\r\n");
        st_puts(t, "  host=192.0.2.10\r\n  user=root\r\n  pass=secret\r\n  port=22\r\n");
        wm_invalidate_all();
        return;
    }
    wm_invalidate_all();

    int pid = proc_create("ssh_pump", st_pump_thread, t, PRIO_NORMAL);
    if (pid < 0) { st_puts(t, "ssh: failed to start pump thread\r\n"); wm_invalidate_all(); }
    else t->pump_pid = pid;
}
