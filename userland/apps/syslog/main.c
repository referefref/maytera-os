// System Log Viewer for MayteraOS  (#273 uplift)
// Real system/status log viewer: level filter, text search, tail/auto-scroll,
// severity colors + status icons, clear, and copy/export.
//
// LOG SOURCE NOTE: the kernel keeps an in-kernel syslog ring (gui/syslog.c,
// g_log_entries) but it is NOT exposed to user mode: there is no read syscall,
// no /dev node, and no /var or /LOG file written by the kernel. So this viewer
// sources its entries from:
//   1) /LOG.TXT if it exists on disk (parsed line-by-line), and
//   2) live system status synthesized from userland-readable syscalls
//      (version, uptime, time, network info, disk free, process table).
// The live-status path means the viewer always shows REAL current system state
// even when no on-disk log is present. A true kernel-ring feed would require a
// new read syscall in the kernel, which is out of scope here.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"

#define LOG_WIDTH       640
#define LOG_HEIGHT      460
#define MAX_LINES       512
#define MAX_LINE_LEN    160
#define FONT_H          16

// Layout
#define TITLE_H         26
#define TOOLBAR_H       30
#define STATUS_H        20
#define TITLEBAR_INSET  22
#define BORDER_INSET    2

// Severity levels
typedef enum {
    LV_DEBUG = 0,
    LV_INFO,
    LV_OK,
    LV_WARN,
    LV_ERR,
    LV_COUNT
} level_t;

// Theme-aware chrome palette (live updated by apply_theme()). Defaults = Dark.
// Same names as the old #define block so all draw call sites are untouched.
static uint32_t COL_BG          = 0x001E1E1E;
static uint32_t COL_PANEL       = 0x00252526;
static uint32_t COL_TITLE       = 0x00333337;
static uint32_t COL_TOOLBAR     = 0x002D2D30;
static uint32_t COL_BORDER      = 0x00404045;
static uint32_t COL_ROW_ALT     = 0x0023232A;
static uint32_t COL_ROW_SEL     = 0x00094771;
static uint32_t COL_TEXT        = 0x00D4D4D4;
static uint32_t COL_DIM         = 0x00858585;
static uint32_t COL_BTN         = 0x003C3C40;
static uint32_t COL_BTN_ON      = 0x00094771;
static uint32_t COL_BTN_HOVER   = 0x00505056;
static uint32_t COL_FIELD       = 0x003C3C42;  // search box bg while active
static uint32_t COL_SCROLL_TRK  = 0x00303033;  // scrollbar track
static uint32_t COL_SCROLL_THM  = 0x00686870;  // scrollbar thumb

// Log-severity colors stay SEMANTIC across all themes (do not theme these).
#define COL_DEBUG       0x00888888
#define COL_INFO        0x0066CCFF
#define COL_OK          0x0066FF66
#define COL_WARN        0x00FFCC00
#define COL_ERR         0x00FF6666

// Live theme tracking. apply_theme() maps the kernel theme id (get_theme():
// 1=Dark, 2=Light, 4=Classic, 5=Ocean, 9=Nord) onto the chrome palette above.
static int g_theme_last = -1;
static void apply_theme(void) {
    int kt = get_theme();
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_PANEL=0x00F8F8F8; COL_TITLE=0x00F0F0F0;
            COL_TOOLBAR=0x00F0F0F0; COL_BORDER=0x00CCCCCC; COL_ROW_ALT=0x00E8E8E8;
            COL_ROW_SEL=0x00D6E4FB; COL_TEXT=0x00202020; COL_DIM=0x00606060;
            COL_BTN=0x00FFFFFF; COL_BTN_ON=0x00D6E4FB; COL_BTN_HOVER=0x00E8E8E8;
            COL_FIELD=0x00FFFFFF; COL_SCROLL_TRK=0x00F0F0F0; COL_SCROLL_THM=0x00CCCCCC;
            break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_PANEL=0x00D0D0D0; COL_TITLE=0x00000080;
            COL_TOOLBAR=0x00C0C0C0; COL_BORDER=0x00808080; COL_ROW_ALT=0x00D0D0D0;
            COL_ROW_SEL=0x00000080; COL_TEXT=0x00000000; COL_DIM=0x00404040;
            COL_BTN=0x00C0C0C0; COL_BTN_ON=0x00000080; COL_BTN_HOVER=0x00D0D0D0;
            COL_FIELD=0x00FFFFFF; COL_SCROLL_TRK=0x00C0C0C0; COL_SCROLL_THM=0x00808080;
            break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_PANEL=0x001E4050; COL_TITLE=0x00305060;
            COL_TOOLBAR=0x001A3A4A; COL_BORDER=0x00406070; COL_ROW_ALT=0x00254555;
            COL_ROW_SEL=0x00305060; COL_TEXT=0x00E0F0FF; COL_DIM=0x0090B0C0;
            COL_BTN=0x00183040; COL_BTN_ON=0x00305060; COL_BTN_HOVER=0x00254555;
            COL_FIELD=0x00183040; COL_SCROLL_TRK=0x001A3A4A; COL_SCROLL_THM=0x00406070;
            break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_PANEL=0x00343B49; COL_TITLE=0x00434C5E;
            COL_TOOLBAR=0x002E3440; COL_BORDER=0x004C566A; COL_ROW_ALT=0x00343B49;
            COL_ROW_SEL=0x00434C5E; COL_TEXT=0x00ECEFF4; COL_DIM=0x00AEB6C5;
            COL_BTN=0x002B303B; COL_BTN_ON=0x00434C5E; COL_BTN_HOVER=0x00343B49;
            COL_FIELD=0x002B303B; COL_SCROLL_TRK=0x002E3440; COL_SCROLL_THM=0x004C566A;
            break;
        default: // Dark
            COL_BG=0x001E1E1E; COL_PANEL=0x00252526; COL_TITLE=0x00333337;
            COL_TOOLBAR=0x002D2D30; COL_BORDER=0x00404045; COL_ROW_ALT=0x0023232A;
            COL_ROW_SEL=0x00094771; COL_TEXT=0x00D4D4D4; COL_DIM=0x00858585;
            COL_BTN=0x003C3C40; COL_BTN_ON=0x00094771; COL_BTN_HOVER=0x00505056;
            COL_FIELD=0x003C3C42; COL_SCROLL_TRK=0x00303033; COL_SCROLL_THM=0x00686870;
            break;
    }
    g_theme_last = kt;
}

static int window_handle = -1;
static int win_x = 150, win_y = 90;

// Log storage
static char    log_text[MAX_LINES][MAX_LINE_LEN];
static level_t log_level[MAX_LINES];
static int     line_count = 0;

// View state
static int  scroll_offset = 0;
static int  visible_lines = 20;
static int  filter_level = 0;       // minimum level shown (0 = all)
static bool tail_mode = true;       // auto-scroll to newest
static char search[64] = "";
static int  search_len = 0;
static bool search_active = false;  // typing into the search box

// Filtered index map (rebuilt each draw)
static int  filtered[MAX_LINES];
static int  filtered_count = 0;

static const char *level_name[LV_COUNT] = { "DEBUG", "INFO", "OK", "WARN", "ERR" };
static uint32_t level_color(level_t l) {
    switch (l) {
        case LV_DEBUG: return COL_DEBUG;
        case LV_INFO:  return COL_INFO;
        case LV_OK:    return COL_OK;
        case LV_WARN:  return COL_WARN;
        case LV_ERR:   return COL_ERR;
        default:       return COL_TEXT;
    }
}
// Glyph status icon per level (kept ASCII so the bitmap font renders it).
static const char *level_icon(level_t l) {
    switch (l) {
        case LV_DEBUG: return ".";
        case LV_INFO:  return "i";
        case LV_OK:    return "+";
        case LV_WARN:  return "!";
        case LV_ERR:   return "x";
        default:       return " ";
    }
}

// --- log building ----------------------------------------------------------
static void add_line(level_t lv, const char *s) {
    if (line_count >= MAX_LINES) return;
    int i = 0;
    while (s[i] && i < MAX_LINE_LEN - 1) { log_text[line_count][i] = s[i]; i++; }
    log_text[line_count][i] = '\0';
    log_level[line_count] = lv;
    line_count++;
}

// Classify a raw text line by keywords (used for /LOG.TXT entries).
static level_t classify(const char *s) {
    if (strstr(s, "ERROR") || strstr(s, "error") || strstr(s, "FAIL") ||
        strstr(s, "fail") || strstr(s, "CRIT") || strstr(s, "PANIC")) return LV_ERR;
    if (strstr(s, "WARN") || strstr(s, "warn")) return LV_WARN;
    if (strstr(s, "[OK]") || strstr(s, "success") || strstr(s, "SUCCESS") ||
        strstr(s, "ready") || strstr(s, "READY")) return LV_OK;
    if (strstr(s, "INFO") || strstr(s, "info")) return LV_INFO;
    if (strstr(s, "DEBUG") || strstr(s, "debug")) return LV_DEBUG;
    return LV_INFO;
}

// Pull live system status from userland-readable syscalls.
static void add_live_status(void) {
    char line[MAX_LINE_LEN];
    char tmp[128];

    add_line(LV_OK, "===== MayteraOS live system status =====");

    // Version
    if (get_version(tmp, sizeof(tmp)) > 0) {
        snprintf(line, sizeof(line), "[OK] MayteraOS version %s", tmp);
        add_line(LV_OK, line);
    }

    // Uptime
    unsigned long up = uptime_ms();
    unsigned long secs = up / 1000UL;
    unsigned long h = secs / 3600UL, m = (secs % 3600UL) / 60UL, s = secs % 60UL;
    snprintf(line, sizeof(line), "[INFO] Uptime: %luh %lum %lus (%lu ms)", h, m, s, up);
    add_line(LV_INFO, line);

    // Wall clock (epoch seconds)
    long t = sys_time();
    snprintf(line, sizeof(line), "[INFO] System time (epoch): %ld", t);
    add_line(LV_INFO, line);

    // Disk free
    long df = sys_get_disk_free();
    if (df >= 0) {
        snprintf(line, sizeof(line), "[INFO] Disk free: %ld MB", df);
        add_line(df < 16 ? LV_WARN : LV_INFO, line);
    }

    // Network info (multi-line report); split on newlines into entries.
    static char netbuf[1024];
    int nn = net_info(netbuf, sizeof(netbuf) - 1);
    if (nn > 0) {
        netbuf[nn] = '\0';
        add_line(LV_OK, "[OK] Network interface report:");
        char *p = netbuf;
        while (*p) {
            char *e = p;
            while (*e && *e != '\n') e++;
            int len = (int)(e - p);
            if (len > 0) {
                if (len > MAX_LINE_LEN - 8) len = MAX_LINE_LEN - 8;
                snprintf(line, sizeof(line), "  %.*s", len, p);
                add_line(LV_INFO, line);
            }
            p = (*e) ? e + 1 : e;
        }
    } else {
        add_line(LV_WARN, "[WARN] Network info unavailable");
    }

    // Process table snapshot
    static proc_info_t procs[64];
    int np = sys_proc_list(procs, 64);
    if (np > 0) {
        snprintf(line, sizeof(line), "[OK] Process table: %d running", np);
        add_line(LV_OK, line);
        for (int i = 0; i < np && i < 64; i++) {
            snprintf(line, sizeof(line),
                     "  pid %u  %-16s  mem %u KB",
                     procs[i].pid, procs[i].name, procs[i].mem_kb);
            add_line(LV_DEBUG, line);
        }
    } else {
        add_line(LV_WARN, "[WARN] Process list unavailable");
    }
}

static void load_log(void) {
    line_count = 0;

    // 1) on-disk /LOG.TXT, if present
    int fd = sys_open("/LOG.TXT", 0);
    if (fd >= 0) {
        static char buf[8192];
        int n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            add_line(LV_OK, "===== /LOG.TXT =====");
            char *p = buf;
            while (*p && line_count < MAX_LINES) {
                char *e = p;
                while (*e && *e != '\n') e++;
                int len = (int)(e - p);
                if (len > MAX_LINE_LEN - 1) len = MAX_LINE_LEN - 1;
                char tmp[MAX_LINE_LEN];
                for (int i = 0; i < len; i++) tmp[i] = p[i];
                tmp[len] = '\0';
                if (len > 0) add_line(classify(tmp), tmp);
                p = (*e) ? e + 1 : e;
            }
        }
    }

    // 2) always append live system status (real state from syscalls)
    add_live_status();

    if (line_count == 0) {
        add_line(LV_WARN, "[WARN] No log data available");
    }
}

// --- filtering -------------------------------------------------------------
static bool line_matches_search(int idx) {
    if (search_len == 0) return true;
    // case-insensitive contains
    const char *h = log_text[idx];
    for (; *h; h++) {
        int k = 0;
        while (search[k]) {
            char a = h[k], b = search[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (search[k] == '\0') return true;
    }
    return false;
}

static void rebuild_filter(void) {
    filtered_count = 0;
    for (int i = 0; i < line_count; i++) {
        if ((int)log_level[i] < filter_level) continue;
        if (!line_matches_search(i)) continue;
        filtered[filtered_count++] = i;
    }
}

// --- drawing ---------------------------------------------------------------
static void draw_button(int x, int y, int w, int h, const char *label,
                        bool on, uint32_t label_col) {
    win_draw_rect(window_handle, x, y, w, h, on ? COL_BTN_ON : COL_BTN);
    gui_draw_rect_outline(window_handle, x, y, w, h, COL_BORDER);
    int tw = gui_string_width(label);
    win_draw_text(window_handle, x + (w - tw) / 2, y + (h - FONT_H) / 2 + 1,
                  label, label_col);
}

static void draw_log(void) {
    rebuild_filter();

    int content_y = TITLE_H + TOOLBAR_H;
    int content_h = LOG_HEIGHT - content_y - STATUS_H - TITLEBAR_INSET;
    visible_lines = content_h / FONT_H;

    if (tail_mode) {
        scroll_offset = filtered_count - visible_lines;
        if (scroll_offset < 0) scroll_offset = 0;
    }
    if (scroll_offset > filtered_count - visible_lines)
        scroll_offset = filtered_count - visible_lines;
    if (scroll_offset < 0) scroll_offset = 0;

    // Background
    win_draw_rect(window_handle, 0, 0, LOG_WIDTH, LOG_HEIGHT, COL_BG);

    // Title bar
    win_draw_rect(window_handle, 0, 0, LOG_WIDTH, TITLE_H, COL_TITLE);
    win_draw_text(window_handle, 10, 5, "System Log Viewer", COL_TEXT);

    // Toolbar
    int ty = TITLE_H;
    win_draw_rect(window_handle, 0, ty, LOG_WIDTH, TOOLBAR_H, COL_TOOLBAR);

    // Level filter buttons: All / Info / Warn / Err
    int bx = 6, bw = 48, bh = 20, by = ty + 5;
    draw_button(bx, by, 44, bh, "All",  filter_level == 0,        filter_level == 0 ? 0x00FFFFFF : COL_DIM);
    draw_button(bx + 48, by, bw, bh, "Info", filter_level == LV_INFO, COL_INFO);
    draw_button(bx + 100, by, bw, bh, "Warn", filter_level == LV_WARN, COL_WARN);
    draw_button(bx + 152, by, bw, bh, "Err",  filter_level == LV_ERR,  COL_ERR);

    // Tail toggle
    draw_button(bx + 206, by, 56, bh, tail_mode ? "Tail:On" : "Tail:Off",
                tail_mode, tail_mode ? 0x0066FF66 : COL_DIM);

    // Refresh / Clear / Copy
    draw_button(LOG_WIDTH - 196, by, 56, bh, "Refresh", false, COL_TEXT);
    draw_button(LOG_WIDTH - 136, by, 50, bh, "Clear",   false, COL_TEXT);
    draw_button(LOG_WIDTH - 82,  by, 72, bh, "Copy>File", false, COL_TEXT);

    // Search box (second short strip drawn inside toolbar on the far right is
    // tight; place it just under filters via a thin field row at toolbar right)
    int sx = bx + 270, sw = LOG_WIDTH - 270 - 210, sh = 20, sy = ty + 5;
    if (sw < 60) sw = 60;
    win_draw_rect(window_handle, sx, sy, sw, sh, search_active ? COL_FIELD : COL_PANEL);
    gui_draw_rect_outline(window_handle, sx, sy, sw, sh, search_active ? COL_INFO : COL_BORDER);
    if (search_len == 0 && !search_active) {
        win_draw_text(window_handle, sx + 4, sy + 2, "Search...", COL_DIM);
    } else {
        win_draw_text(window_handle, sx + 4, sy + 2, search, COL_TEXT);
        if (search_active) {
            int cx = sx + 4 + gui_string_width(search);
            win_draw_rect(window_handle, cx, sy + 3, 1, FONT_H - 2, COL_TEXT);
        }
    }

    // Log rows
    win_draw_rect(window_handle, 0, content_y, LOG_WIDTH, content_h, COL_PANEL);
    for (int i = 0; i < visible_lines && (scroll_offset + i) < filtered_count; i++) {
        int idx = filtered[scroll_offset + i];
        int ry = content_y + i * FONT_H;
        if (i & 1) win_draw_rect(window_handle, 0, ry, LOG_WIDTH - 14, FONT_H, COL_ROW_ALT);

        uint32_t col = level_color(log_level[idx]);
        // status icon
        win_draw_text(window_handle, 6, ry, level_icon(log_level[idx]), col);
        // message
        win_draw_text(window_handle, 22, ry, log_text[idx], col);
    }

    // Scrollbar
    if (filtered_count > visible_lines) {
        int sb_h = content_h;
        int thumb = (visible_lines * sb_h) / filtered_count;
        if (thumb < 20) thumb = 20;
        int range = filtered_count - visible_lines;
        int thumb_y = content_y + (range > 0 ? (scroll_offset * (sb_h - thumb)) / range : 0);
        win_draw_rect(window_handle, LOG_WIDTH - 12, content_y, 10, sb_h, COL_SCROLL_TRK);
        win_draw_rect(window_handle, LOG_WIDTH - 11, thumb_y, 8, thumb, COL_SCROLL_THM);
    }

    // Status bar
    char st[96];
    int show_end = scroll_offset + visible_lines;
    if (show_end > filtered_count) show_end = filtered_count;
    const char *flt = (filter_level == 0) ? "all" : level_name[filter_level];
    snprintf(st, sizeof(st), "Filter:%s  Total:%d  Shown:%d  View:%d-%d%s",
             flt, line_count, filtered_count,
             filtered_count ? scroll_offset + 1 : 0, show_end,
             search_len ? "  [search]" : "");
    win_draw_rect(window_handle, 0, LOG_HEIGHT - STATUS_H - TITLEBAR_INSET, LOG_WIDTH, STATUS_H, COL_TITLE);
    win_draw_text(window_handle, 8, LOG_HEIGHT - STATUS_H - TITLEBAR_INSET + 2, st, COL_DIM);

    win_invalidate(window_handle);
}

// Export the currently filtered view to /SYSLOG.TXT (no system clipboard
// syscall exists, so "Copy" writes a file the user can open/share).
static void copy_to_file(void) {
    int fd = sys_open("/SYSLOG.TXT", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < filtered_count; i++) {
        int idx = filtered[i];
        sys_write(fd, log_text[idx], strlen(log_text[idx]));
        sys_write(fd, "\n", 1);
    }
    sys_close(fd);
}

// Hit-test a toolbar button by toolbar-local coords.
static void handle_toolbar_click(int lx, int ly) {
    int ty = TITLE_H;
    int by = ty + 5, bh = 20;
    if (ly < by || ly >= by + bh) return;
    int bx = 6;
    if (lx >= bx && lx < bx + 44)                  { filter_level = 0; }
    else if (lx >= bx + 48 && lx < bx + 96)        { filter_level = LV_INFO; }
    else if (lx >= bx + 100 && lx < bx + 148)      { filter_level = LV_WARN; }
    else if (lx >= bx + 152 && lx < bx + 200)      { filter_level = LV_ERR; }
    else if (lx >= bx + 206 && lx < bx + 262)      { tail_mode = !tail_mode; }
    else if (lx >= LOG_WIDTH - 196 && lx < LOG_WIDTH - 140) { load_log(); }
    else if (lx >= LOG_WIDTH - 136 && lx < LOG_WIDTH - 86)  { line_count = 0; filtered_count = 0; }
    else if (lx >= LOG_WIDTH - 82 && lx < LOG_WIDTH - 10)   { copy_to_file(); }
    else {
        // search box region
        int sx = bx + 270, sw = LOG_WIDTH - 270 - 210;
        if (sw < 60) sw = 60;
        if (lx >= sx && lx < sx + sw) search_active = true;
        else search_active = false;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    window_handle = win_create("System Log", win_x, win_y, LOG_WIDTH, LOG_HEIGHT);
    if (window_handle < 0) {
        printf("Failed to create System Log window\n");
        return 1;
    }

    printf("System Log window created (handle=%d)\n", window_handle);

    apply_theme();
    load_log();
    draw_log();

    gui_event_t event;
    int running = 1;
    int refresh_ticks = 0;

    while (running) {
        int ev = win_get_event(window_handle, &event, 100);

        // Live theme switch: re-apply palette and redraw when the theme changes.
        {
            int th = get_theme();
            if (th != g_theme_last) { g_theme_last = th; apply_theme(); draw_log(); }
        }

        // Auto-refresh in tail mode (~ every 2s) so new status is picked up.
        if (ev == 0) {
            if (tail_mode && ++refresh_ticks >= 20) {
                refresh_ticks = 0;
                load_log();
                draw_log();
            }
            continue;
        }

        win_get_pos(window_handle, &win_x, &win_y);

        switch (event.type) {
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN: {
                char c = event.key_char;
                uint32_t kc = event.keycode;
                if (search_active) {
                    if (c == 27) { search_active = false; }
                    else if (kc == 0x1C || c == '\n' || c == '\r') { search_active = false; }
                    else if (c == '\b' || kc == 0x0E) {
                        if (search_len > 0) { search_len--; search[search_len] = '\0'; }
                    } else if (c >= ' ' && c < 127 && search_len < (int)sizeof(search) - 1) {
                        search[search_len++] = c; search[search_len] = '\0';
                    }
                    draw_log();
                    break;
                }
                if (c == 27) { running = 0; }
                else if (c == 'r' || c == 'R') { load_log(); draw_log(); }
                else if (c == 'c' || c == 'C') { line_count = 0; filtered_count = 0; draw_log(); }
                else if (c == 't' || c == 'T') { tail_mode = !tail_mode; draw_log(); }
                else if (c == '/' || c == 6) { search_active = true; draw_log(); } // '/' or Ctrl+F
                else if (c == 'a' || c == 'A') { filter_level = 0; draw_log(); }
                else if (c == 'i' || c == 'I') { filter_level = LV_INFO; draw_log(); }
                else if (c == 'w' || c == 'W') { filter_level = LV_WARN; draw_log(); }
                else if (c == 'e' || c == 'E') { filter_level = LV_ERR; draw_log(); }
                else if (kc == 0x80) { tail_mode = false; if (scroll_offset > 0) scroll_offset--; draw_log(); }
                else if (kc == 0x81) { tail_mode = false; scroll_offset++; draw_log(); }
                else if (kc == 0x49) { tail_mode = false; scroll_offset -= visible_lines; if (scroll_offset < 0) scroll_offset = 0; draw_log(); }
                else if (kc == 0x51) { tail_mode = false; scroll_offset += visible_lines; draw_log(); }
                break;
            }

            case EVENT_MOUSE_SCROLL:
                tail_mode = false;
                if (event.scroll_delta > 0) {
                    scroll_offset = (scroll_offset > 3) ? scroll_offset - 3 : 0;
                } else {
                    scroll_offset += 3;
                }
                draw_log();
                break;

            case EVENT_MOUSE_DOWN: {
                int lx = event.mouse_x - win_x - BORDER_INSET;
                int ly = event.mouse_y - win_y - TITLEBAR_INSET;
                if (ly >= TITLE_H && ly < TITLE_H + TOOLBAR_H) {
                    handle_toolbar_click(lx, ly);
                } else {
                    search_active = false;
                }
                draw_log();
                break;
            }

            case EVENT_REDRAW:
                draw_log();
                break;

            default:
                break;
        }
    }

    win_destroy(window_handle);
    printf("System Log closed\n");
    return 0;
}
