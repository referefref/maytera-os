// taskbar.c - Taskbar with system gauges for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/bt_client.h"   // #372: Bluetooth tray indicator (mock-backed)
#include "../../libc/wifi_client.h" // #384: Network/Wi-Fi tray indicator (wifi mock)

// ---------------------------------------------------------------------------
// Maytera-logo command-launcher button (next to Start). Reuses the real Maytera
// logo asset /MAYLOGO.DAT (u16 w, u16 h LE, then w*h RGBA) - the same asset the
// Settings "About" tab shows - scaled to fit the taskbar button and alpha
// composited over the button background so it reads on any theme.
// ---------------------------------------------------------------------------
static unsigned char s_maylogo[80 * 80 * 4];
static int s_maylogo_w = 0, s_maylogo_h = 0, s_maylogo_state = -1; // -1 untried,0 fail,1 ok

static void maylogo_load_once(void) {
    if (s_maylogo_state >= 0) return;
    s_maylogo_state = 0;
    int fd = sys_open("/MAYLOGO.DAT", 0);
    if (fd >= 0) {
        unsigned char hd[4];
        if (sys_read(fd, hd, 4) == 4) {
            int w = hd[0] | (hd[1] << 8), h = hd[2] | (hd[3] << 8);
            if (w > 0 && h > 0 && w <= 80 && h <= 80) {
                int need = w * h * 4;
                if (sys_read(fd, s_maylogo, need) == need) {
                    s_maylogo_w = w; s_maylogo_h = h; s_maylogo_state = 1;
                }
            }
        }
        sys_close(fd);
    }
}

// Draw the logo scaled (nearest-neighbour) into a `size`x`size` box at (bx,by)
// over background `bg`. Returns 1 if the real logo was drawn, 0 if unavailable.
// Draw the Maytera logo asset as a MONOCHROME silhouette in `ink` (the same
// color the Start 9-dots use), using the asset's alpha channel purely as a
// coverage mask blended over the button `bg`. Previously it rendered the logo's
// native RGBA, which blended into the taskbar; the user wants it to match the
// Start button ink and contrast the bar.
static int maylogo_draw(int bx, int by, int size, uint32_t bg, uint32_t ink) {
    maylogo_load_once();
    if (s_maylogo_state != 1) return 0;
    int br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
    for (int yy = 0; yy < size; yy++) {
        int sy = yy * s_maylogo_h / size;
        for (int xx = 0; xx < size; xx++) {
            int sx = xx * s_maylogo_w / size;
            unsigned char *p = s_maylogo + (sy * s_maylogo_w + sx) * 4;
            int a = p[3];
            if (a < 8) continue;
            int r = (ir * a + br  * (255 - a)) / 255;
            int g = (ig * a + bgc * (255 - a)) / 255;
            int b = (ib * a + bb  * (255 - a)) / 255;
            draw_putpixel(bx + xx, by + yy, (uint32_t)((r << 16) | (g << 8) | b));
        }
    }
    return 1;
}

// #387 Alternate dock/taskbar layout metrics (used by both the renderers below
// and taskbar_collect_damage above them, so defined here at file scope).
#define LUMINA_MENUBAR_H     24
#define LUMINA_DOCK_ICON     40
#define LUMINA_DOCK_PAD      8
#define LUMINA_DOCK_RESERVE  64     // work-area reserved at bottom for the dock
#define CLASSIC_UNIX_PANEL_H       58
#define RETRO_BENCH_BAR_H       20

// ============================================================================
// Static state
// ============================================================================

static int32_t g_taskbar_y;
static int     g_cpu_percent;
static unsigned int g_cpu_cores[65];   // [0]=count, [1..]=per-core % (#279 per-core meter)
static int     g_cpu_ncores = 1;
static int     g_ram_percent;
static int     g_disk_percent;
static int     g_net_percent;
static uint64_t g_gauge_update_time;
static char    g_cpu_str[8];
static char    g_ram_str[8];
static char    g_disk_str[8];
static char    g_net_str[8];

// #241 Performance popup: clicking a taskbar gauge opens a detailed, themed
// read-only system-performance panel anchored above the gauges.
static int      g_perf_open = 0;     // popup visible?
static int      g_perf_sel  = 0;     // highlighted gauge row (0=CPU..3=NET)
static int32_t  g_gauge_x0  = 0;     // left edge of the gauge block (this frame)
static int32_t  g_gauge_y   = 0;     // top of the gauge row (this frame)
static int32_t  g_pp_x, g_pp_y, g_pp_w, g_pp_h;             // popup rect
static int32_t  g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h; // Task Manager button
// Absolute readings captured during taskbar_update for the detail view.
static unsigned long g_ram_used_b = 0, g_ram_total_b = 0;   // bytes
static long          g_disk_total_mb = 0, g_disk_free_mb = 0; // MB
static unsigned long g_net_bps = 0;                          // bytes/sec estimate

// Open-window list for the taskbar app buttons.
#define TB_MAX_WINS 16
static wm_window_info_t g_tb_wins[TB_MAX_WINS];
static int             g_tb_win_count;

// Compact window buttons: icon + short label.
#define TB_BTN_W   120   // preferred per-button width (shrinks to fit)
#define TB_BTN_GAP 3
#define TB_ICON_SZ 16

// Hitboxes recorded each frame so taskbar_handle_mouse can focus on click.
static int32_t g_tb_btn_x[TB_MAX_WINS];       // left edge of each drawn button
static int32_t g_tb_btn_id[TB_MAX_WINS];      // window id for that button
static int     g_tb_btn_focused[TB_MAX_WINS]; // is that window currently focused?
static int     g_tb_btn_n;                    // number of buttons drawn
static int32_t g_tb_btn_w;                    // per-button width this frame
static int32_t g_tb_btn_y;                    // top of the button row
static int32_t g_tb_btn_h;                    // button height

// A window is "real" (worth a taskbar button) if it has a title. Minimized
// windows are still shown (so they can be restored from the taskbar, like
// Windows). The root/desktop window has no title, so it is skipped naturally.
// #341: NOCHROME helper / sub-windows that belong to another app's main window
// (or are docked widget-style panels) must NOT get their own taskbar tile. The
// kernel wm_window_info_t exposes neither the WINDOW_FLAG_NOCHROME flag nor the
// owner PID, and we deploy only the compositor (a struct change would break the
// ABI against the already-deployed kernels), so companion windows are matched by
// their known titles instead. Result: AI Chat = no tile (docked panel); Maytera
// HiFi keeps exactly ONE tile (its main window) while Album Art / Playlist /
// Equalizer / Library / Viz sub-windows are suppressed. Prefix match, so
// "Maytera Viz - MilkDrop" is caught while "Maytera HiFi" is preserved.
static int tb_is_companion(const char *t) {
    static const char *sub[] = {
        "AI Chat",
        "HiFi Equalizer", "HiFi Playlist",
        "Album Art", "Music Library",
        "Maytera Viz",
        0
    };
    for (int i = 0; sub[i]; i++) {
        int j = 0;
        while (sub[i][j] && sub[i][j] == t[j]) j++;
        if (sub[i][j] == '\0') return 1;   // t starts with sub[i]
    }
    return 0;
}

static int tb_window_is_app(const wm_window_info_t *w) {
    if (w->title[0] == '\0') return 0;
    // Show a tile for live, user-facing windows only: visible (normal) or
    // minimized. A hidden window (closed via the default X action: visible=0,
    // minimized=0) is no longer user-facing, so it must not keep a tile.
    if (!w->visible && !w->minimized) return 0;
    // #341: drop borderless companion/sub-windows so each app gets one tile.
    if (tb_is_companion(w->title)) return 0;
    return 1;
}

// Case-insensitive substring test (no libc strstr dependency here).
static int tb_contains(const char *s, const char *sub) {
    for (int i = 0; s[i]; i++) {
        int j = 0;
        while (sub[j]) {
            char a = s[i + j], b = sub[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (!sub[j]) return 1;
    }
    return 0;
}

// Pick an icon for a window based on its title; ICON_WINDOW is the fallback.
static icon_id_t tb_icon_for_title(const char *t) {
    if (tb_contains(t, "term"))                          return ICON_TERMINAL;
    if (tb_contains(t, "calc"))                          return ICON_CALCULATOR;
    if (tb_contains(t, "file") || tb_contains(t, "folder")) return ICON_FOLDER;
    if (tb_contains(t, "edit"))                          return ICON_HIGHLIGHT;
    if (tb_contains(t, "paint"))                         return ICON_PAINT;
    if (tb_contains(t, "image") || tb_contains(t, "view")) return ICON_IMAGE;
    if (tb_contains(t, "audio") || tb_contains(t, "music") ||
        tb_contains(t, "media") || tb_contains(t, "player")) return ICON_MUSIC;
    if (tb_contains(t, "clock"))                         return ICON_CLOCK;
    if (tb_contains(t, "setting"))                       return ICON_COG;
    if (tb_contains(t, "task"))                          return ICON_TASK_MANAGER;
    if (tb_contains(t, "log"))                           return ICON_LOG_VIEWER;
    if (tb_contains(t, "recycle") || tb_contains(t, "trash")) return ICON_TRASH;
    if (tb_contains(t, "doom"))                          return ICON_GAME_DOOM;
    if (tb_contains(t, "irc") || tb_contains(t, "browser") ||
        tb_contains(t, "network") || tb_contains(t, "chat")) return ICON_INFO_CIRCLE;
    return ICON_WINDOW;
}

// ============================================================================
// Internal helpers
// ============================================================================

// Write an integer percentage as "N%" into buf (max 7 chars + NUL).
// No libc sprintf available, so format manually.
static void fmt_percent(char *buf, int pct) {
    // Clamp to valid range.
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    int i = 0;

    if (pct == 100) {
        buf[i++] = '1';
        buf[i++] = '0';
        buf[i++] = '0';
    } else if (pct >= 10) {
        buf[i++] = (char)('0' + pct / 10);
        buf[i++] = (char)('0' + pct % 10);
    } else {
        buf[i++] = (char)('0' + pct);
    }

    buf[i++] = '%';
    buf[i]   = '\0';
}

// Draw a single horizontal gauge bar with label on the left and value on the
// right. No dynamic allocation.
static void draw_gauge(int32_t x, int32_t y, int32_t w, int32_t h,
                       int percent, uint32_t color,
                       const char *label, const char *value) {
    // Background.
    draw_fill_rect(x, y, w, h, CLR_GAUGE_BG);

    // Filled portion. Guard against zero or negative width.
    if (percent > 0) {
        int32_t fill_w = w * percent / 100;
        if (fill_w < 1) fill_w = 1;
        draw_fill_rect(x, y, fill_w, h, color);
    }

    // Border drawn on top so it is always crisp.
    draw_rect_outline(x, y, w, h, CLR_GAUGE_BORDER);

    // Vertical center for text.
    int32_t text_y = y + (h - FONT_CHAR_H) / 2 + 1;

    // Label: left-aligned with small padding.
    draw_text(x + 4, text_y, label, readable_ink(CLR_GAUGE_BG));

    // Value: right-aligned with small padding.
    int32_t val_x = x + w - text_width(value) - 4;
    draw_text(val_x, text_y, value, readable_ink(CLR_GAUGE_BG));
}

// Per-core CPU gauge (#279): same footprint as draw_gauge, but the fill area is
// split into one vertical bar per core (height proportional to that core's %).
// Single-core systems fall back to looking like a normal horizontal gauge.
static void draw_cpu_gauge(int32_t x, int32_t y, int32_t w, int32_t h,
                           int ncores, const unsigned int *cores) {
    draw_fill_rect(x, y, w, h, CLR_GAUGE_BG);

    // Reserve the left side for the "CPU" label; bars fill the rest.
    int32_t label_w = text_width("CPU") + 6;
    int32_t bars_x  = x + label_w;
    int32_t bars_w  = w - label_w - 3;
    if (ncores < 1) ncores = 1;
    if (bars_w < ncores) bars_w = ncores;

    int32_t inner_h = h - 4;
    if (inner_h < 1) inner_h = 1;
    int32_t gap = (ncores > 1) ? 1 : 0;
    int32_t bw  = (bars_w - gap * (ncores - 1)) / ncores;
    if (bw < 1) bw = 1;

    for (int i = 0; i < ncores; i++) {
        int pct = (int)cores[1 + i];
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int32_t bx = bars_x + i * (bw + gap);
        int32_t fh = inner_h * pct / 100;
        // Track (empty part) then the filled portion from the bottom.
        draw_fill_rect(bx, y + 2, bw, inner_h, CLR_GAUGE_BORDER);
        if (fh > 0)
            draw_fill_rect(bx, y + 2 + (inner_h - fh), bw, fh, CLR_GAUGE_CPU);
    }

    draw_rect_outline(x, y, w, h, CLR_GAUGE_BORDER);
    int32_t text_y = y + (h - FONT_CHAR_H) / 2 + 1;
    draw_text(x + 4, text_y, "CPU", readable_ink(CLR_GAUGE_BG));
}

// ============================================================================
// #241 Performance popup helpers
// ============================================================================

// Unsigned -> decimal string (no libc here).
static void fmt_uint(char *buf, unsigned long v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0 && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = '\0';
}
// Append a C string at offset p; returns the new offset.
static int sappend(char *dst, int p, const char *s) {
    while (*s) dst[p++] = *s++;
    dst[p] = '\0';
    return p;
}
static int sappend_u(char *dst, int p, unsigned long v) {
    char num[24];
    fmt_uint(num, v);
    return sappend(dst, p, num);
}

// Return the gauge index (0=CPU,1=RAM,2=DSK,3=NET) under (x,y), or -1.
static int gauge_hit(int32_t x, int32_t y) {
    if (g_gauge_x0 == 0) return -1;
    if (y < g_gauge_y || y >= g_gauge_y + GAUGE_HEIGHT) return -1;
    for (int i = 0; i < 4; i++) {
        int32_t gx = g_gauge_x0 + i * (GAUGE_WIDTH + GAUGE_SPACING);
        if (x >= gx && x < gx + GAUGE_WIDTH) return i;
    }
    return -1;
}

// Draw the detailed performance popup, anchored above the gauge block. Themed
// via the menu palette (CLR_MENU_*), drawn last in taskbar_render so it sits on
// top of the bar.
static void draw_perf_popup(void) {
    static const char *names[4]   = { "CPU", "RAM", "DSK", "NET" };
    const uint32_t accents[4]     = { CLR_GAUGE_CPU, CLR_GAUGE_RAM,
                                      CLR_GAUGE_DSK, CLR_GAUGE_NET };
    const int pcts[4]             = { g_cpu_percent, g_ram_percent,
                                      g_disk_percent, g_net_percent };

    const int32_t PAD   = 10;
    const int32_t TITLE = 24;
    const int32_t ROW_H = 44;
    const int32_t BTN_H = 26;
    g_pp_w = 252;
    g_pp_h = TITLE + 4 * ROW_H + 8 + BTN_H + PAD;

    // Right-align with the gauge block; sit just above the taskbar.
    g_pp_x = (g_fb_width - TASKBAR_PADDING) - g_pp_w;
    if (g_pp_x < 2) g_pp_x = 2;
    g_pp_y = g_taskbar_y - g_pp_h - 4;
    if (g_pp_y < 2) g_pp_y = 2;

    // Shadow + panel + border.
    draw_fill_rect(g_pp_x + 4, g_pp_y + 4, g_pp_w, g_pp_h, CLR_MENU_SHADOW);
    draw_fill_rect(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BG);
    draw_rect_outline(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BORDER);

    // Title bar.
    draw_fill_rect(g_pp_x + 1, g_pp_y + 1, g_pp_w - 2, TITLE - 1, CLR_MENU_CAT_BG);
    draw_text(g_pp_x + PAD, g_pp_y + 7, "System Performance", CLR_MENU_TEXT);

    int32_t ry = g_pp_y + TITLE + 2;
    for (int i = 0; i < 4; i++) {
        int32_t rx = g_pp_x + 6;
        int32_t rw = g_pp_w - 12;

        // Highlight the gauge that opened the popup.
        if (i == g_perf_sel)
            draw_fill_rect(rx, ry, rw, ROW_H - 4, CLR_MENU_ITEM_HOVER);

        // Accent swatch + label + percent.
        draw_fill_rect(rx + 4, ry + 6, 10, 10, accents[i]);
        draw_rect_outline(rx + 4, ry + 6, 10, 10, CLR_MENU_BORDER);
        draw_text(rx + 22, ry + 5, names[i], CLR_MENU_TEXT);

        char pv[8];
        fmt_percent(pv, pcts[i]);
        draw_text(rx + rw - text_width(pv) - 6, ry + 5, pv, CLR_MENU_TEXT);

        // Progress bar.
        int32_t bx = rx + 22, by = ry + 19, bw = rw - 28, bh = 8;
        int p = pcts[i]; if (p < 0) p = 0; if (p > 100) p = 100;
        draw_fill_rect(bx, by, bw, bh, CLR_GAUGE_BG);
        if (p > 0) {
            int32_t fw = bw * p / 100; if (fw < 1) fw = 1;
            draw_fill_rect(bx, by, fw, bh, accents[i]);
        }
        draw_rect_outline(bx, by, bw, bh, CLR_GAUGE_BORDER);

        // Detail line.
        char det[64];
        int  dp = 0;
        switch (i) {
            case 0: {  // CPU: core count
                dp = sappend_u(det, dp, (unsigned long)g_cpu_ncores);
                dp = sappend(det, dp, g_cpu_ncores == 1 ? " core" : " cores");
                break;
            }
            case 1: {  // RAM: used / total MB
                dp = sappend_u(det, dp, g_ram_used_b  / 1048576UL);
                dp = sappend(det, dp, " / ");
                dp = sappend_u(det, dp, g_ram_total_b / 1048576UL);
                dp = sappend(det, dp, " MB");
                break;
            }
            case 2: {  // DSK: free of total MB
                long fr = g_disk_free_mb  < 0 ? 0 : g_disk_free_mb;
                long tt = g_disk_total_mb < 0 ? 0 : g_disk_total_mb;
                dp = sappend_u(det, dp, (unsigned long)fr);
                dp = sappend(det, dp, " MB free of ");
                dp = sappend_u(det, dp, (unsigned long)tt);
                dp = sappend(det, dp, " MB");
                break;
            }
            default: { // NET: throughput
                unsigned long kbps = g_net_bps / 1024UL;
                if (kbps >= 1024UL) {
                    dp = sappend_u(det, dp, kbps / 1024UL);
                    dp = sappend(det, dp, " MB/s");
                } else {
                    dp = sappend_u(det, dp, kbps);
                    dp = sappend(det, dp, " KB/s");
                }
                break;
            }
        }
        draw_text(rx + 22, ry + 30, det, readable_ink_dim(CLR_MENU_BG));

        ry += ROW_H;
    }

    // "Open Task Manager" button.
    g_pp_tm_w = g_pp_w - 2 * PAD;
    g_pp_tm_h = BTN_H;
    g_pp_tm_x = g_pp_x + PAD;
    g_pp_tm_y = ry + 4;
    draw_fill_rect(g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h, CLR_MENU_CAT_BG);
    draw_rect_outline(g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h, CLR_MENU_BORDER);
    {
        const char *lbl = "Open Task Manager";
        int32_t lx = g_pp_tm_x + (g_pp_tm_w - text_width(lbl)) / 2;
        draw_text(lx, g_pp_tm_y + 9, lbl, CLR_MENU_TEXT);
    }
}

// ============================================================================
// Public API
// ============================================================================

void taskbar_init(void) {
    g_taskbar_y = g_fb_height - TASKBAR_HEIGHT;

    g_cpu_percent  = 0;
    g_ram_percent  = 0;
    g_disk_percent = 0;
    g_net_percent  = 50;  // Network gauge starts at a nominal placeholder.

    fmt_percent(g_cpu_str,  g_cpu_percent);
    fmt_percent(g_ram_str,  g_ram_percent);
    fmt_percent(g_disk_str, g_disk_percent);
    fmt_percent(g_net_str,  g_net_percent);

    g_gauge_update_time = 0;
}

void taskbar_update(void) {
    uint64_t now = (uint64_t)sys_clock();

    // Throttle updates. #102b idle-CPU fix: sampling the gauges ~10Hz made the
    // CPU%/RAM%/NET% integers jitter (e.g. 2->3->2) every ~100ms; because
    // taskbar_collect_damage() marks the bar dirty on any displayed-value change,
    // that jitter forced a full-screen present (~4MB back->front swap) ~10 times a
    // second on an otherwise idle desktop - the dominant residual idle-CPU cost.
    // A taskbar meter only needs ~1Hz, so sample once a second normally and only
    // speed up (~5Hz) while the detailed performance popup is open so its live
    // graph stays smooth. sys_clock() is ~milliseconds.
    uint64_t interval = g_perf_open ? 200 : 1000;
    if (now - g_gauge_update_time < interval) {
        return;
    }
    g_gauge_update_time = now;

    // CPU usage (0-100) aggregate + per-core (#279).
    g_cpu_percent = sys_get_cpu_usage();
    fmt_percent(g_cpu_str, g_cpu_percent);
    g_cpu_ncores = sys_get_cpu_per_core(g_cpu_cores);
    if (g_cpu_ncores < 1) g_cpu_ncores = 1;
    if (g_cpu_ncores > 64) g_cpu_ncores = 64;

    // RAM usage derived from page counts reported by the kernel.
    {
        unsigned long total = 0;
        unsigned long used  = 0;
        sys_get_mem_info(&total, &used);
        g_ram_total_b = total;
        g_ram_used_b  = used;
        if (total > 0) {
            g_ram_percent = (int)(used * 100UL / total);
        } else {
            g_ram_percent = 0;
        }
        fmt_percent(g_ram_str, g_ram_percent);
    }

    // Disk usage derived from cluster counts.
    {
        long total = sys_get_disk_total();
        long free  = sys_get_disk_free();
        g_disk_total_mb = total;
        g_disk_free_mb  = free;
        if (total > 0) {
            long used = total - free;
            if (used < 0) used = 0;
            g_disk_percent = (int)(used * 100L / total);
        } else {
            g_disk_percent = 0;
        }
        fmt_percent(g_disk_str, g_disk_percent);
    }

    // Network gauge: real throughput as % of the 1 Gbit link (so 100% really
    // means the link is saturated; idle reads ~0). Sampled per gauge update
    // (~100 ms), where a full 1 Gbit link moves ~12.5 MB per 100 ms.
    {
        static unsigned long s_last_net_bytes = 0;
        static int s_net_primed = 0;
        unsigned long now_bytes = get_net_bytes();
        if (!s_net_primed) { s_net_primed = 1; g_net_percent = 0; }
        else {
            unsigned long d = now_bytes - s_last_net_bytes;   // bytes in ~100 ms
            g_net_percent = (int)((d * 100UL) / 12500000UL);  // 12.5 MB = 1 Gbit/100ms
            if (g_net_percent > 100) g_net_percent = 100;
            g_net_bps = d * 10UL;                             // ~bytes/sec
        }
        s_last_net_bytes = now_bytes;
    }
    fmt_percent(g_net_str, g_net_percent);
}

// #102: expose the taskbar's sampled CPU% + per-core array so the System Monitor
// widget reads the identical source (the two meters can no longer disagree).
int taskbar_cpu_snapshot(unsigned int *cores, int *ncores)
{
    if (cores) {
        int n = g_cpu_ncores; if (n < 1) n = 1; if (n > 64) n = 64;
        cores[0] = (unsigned int)n;
        for (int i = 1; i <= n; i++) cores[i] = g_cpu_cores[i];
    }
    if (ncores) *ncores = g_cpu_ncores;
    return g_cpu_percent;
}

// #102/#379 dirty-rect: damage the taskbar strip only when something it shows
// actually changed (gauge readings, unread-notification badge, or the set of
// open windows). At idle these are constant, so the taskbar contributes no
// damage and forces no present.
void taskbar_collect_damage(void)
{
    static int l_cpu = -1, l_ram = -1, l_disk = -1, l_net = -1, l_unread = -1, l_wins = -1, l_bt = -1;
    int unread = notif_unread();
    int nwins  = wm_get_windows(g_tb_wins, TB_MAX_WINS);
    if (nwins < 0) nwins = 0;
    // #372/#384: re-sync the shared Bluetooth + Wi-Fi state (self-throttled to
    // ~400ms) so the tray icons follow toggles made in the Settings window.
    static int l_conn = -1, l_min = -1, l_style = -1;
    bt_tick(); wifi_tick();
    int bt = bt_tray_state();
    // Combined connectivity signature: BT state + net-up + wifi tray state.
    int conn = bt * 100 + (sys_net_is_up() ? 10 : 0) + wifi_tray_state();
    // #387: the alternate layouts show a clock in their bar; track the minute so
    // an idle desktop still repaints the bar when the time rolls over.
    int minute = (int)((sys_get_rtc_time() >> 8) & 0xFF);
    if (g_cpu_percent != l_cpu || g_ram_percent != l_ram || g_disk_percent != l_disk ||
        g_net_percent != l_net || unread != l_unread || nwins != l_wins ||
        bt != l_bt || conn != l_conn || minute != l_min || g_dock_style != l_style) {
        l_cpu = g_cpu_percent; l_ram = g_ram_percent; l_disk = g_disk_percent;
        l_net = g_net_percent; l_unread = unread; l_wins = nwins; l_bt = bt; l_conn = conn;
        l_min = minute; l_style = g_dock_style;
        // #387: damage the actual bar region(s) for the active layout.
        int ti = taskbar_top_inset(), bi = taskbar_bottom_inset();
        if (ti > 0) damage_add(0, 0, g_fb_width, ti);
        if (g_dock_style == DOCK_LUMINA)
            damage_add(0, g_fb_height - (LUMINA_DOCK_ICON + 2 * LUMINA_DOCK_PAD + 12),
                       g_fb_width, LUMINA_DOCK_ICON + 2 * LUMINA_DOCK_PAD + 12);
        else if (bi > 0)
            damage_add(0, g_fb_height - bi, g_fb_width, bi);
    }
}

// ---- System tray / quick settings (#tray) --------------------------------
extern int g_widgets_enabled;   // widgets.c
extern int g_sheep_enabled;     // widgets.c
int g_tray_muted = 0;   // #336: global so the analog EQ popup (traymenu.c) can toggle it
// Tray slot order (left->right). #384 adds Network next to Sound; #372 Bluetooth.
#define TRAY_WIDGETS  0
#define TRAY_SOUND    1
#define TRAY_NET      2
#define TRAY_BT       3
#define TRAY_SHEEP    4
#define TRAY_BELL     5
#define TRAY_N        6
#define TRAY_ICON_W   26
#define TRAY_ICON_GAP 2
static int32_t g_tray_x[TRAY_N];
static int32_t g_tray_y = 0, g_tray_h = 0;

static void tray_draw_widgets(int x, int y, int on) {
    uint32_t c = on ? readable_ink(CLR_TASKBAR_BG) : readable_ink_dim(CLR_TASKBAR_BG);
    draw_fill_rect(x + 6,  y + 6,  6, 6, c);
    draw_fill_rect(x + 14, y + 6,  6, 6, c);
    draw_fill_rect(x + 6,  y + 14, 6, 6, c);
    draw_fill_rect(x + 14, y + 14, 6, 6, c);
}
static void tray_draw_sound(int x, int y, int muted) {
    uint32_t c = muted ? readable_ink_dim(CLR_TASKBAR_BG) : readable_ink(CLR_TASKBAR_BG);
    // Zest "sliders" glyph for the quick-settings / audio tray slot, tinted to
    // the readable ink (dimmer when muted). Falls back to the programmatic
    // speaker if the color icon failed to load.
    if (icon_draw_color_tinted(ICON_SLIDERS, x + 1, y + 1, 24, c)) {
        if (muted)
            for (int i = 0; i < 13; i++) draw_putpixel(x + 6 + i, y + 6 + i, 0x00FF4040);
        return;
    }
    // speaker body + cone (fallback)
    draw_fill_rect(x + 5, y + 10, 4, 6, c);
    for (int i = 0; i < 7; i++)
        draw_fill_rect(x + 9 + i, y + 13 - i, 1, 1 + 2 * i, c);
    if (muted) {
        for (int i = 0; i < 13; i++) draw_putpixel(x + 6 + i, y + 6 + i, 0x00FF4040);
    } else {
        draw_circle_outline(x + 17, y + 13, 4, c);
        draw_circle_outline(x + 17, y + 13, 7, c);
    }
}
static void tray_draw_sheep(int x, int y, int on) {
    // White + consistent with the other tray glyphs: white wool when on, dim
    // when off; the head is drawn in dim ink (not a background punch-hole) so
    // the whole glyph reads as one inked shape like the bell/BT. (desktop UX pass)
    uint32_t wool = on ? readable_ink(CLR_TASKBAR_BG) : readable_ink_dim(CLR_TASKBAR_BG);
    uint32_t face = readable_ink_dim(CLR_TASKBAR_BG);
    draw_circle_filled(x + 11, y + 14, 6, wool);
    draw_circle_filled(x + 15, y + 12, 5, wool);
    draw_circle_filled(x + 20, y + 14, 3, face);
}

static void tray_draw_bell(int x, int y, int unread) {
    uint32_t c = readable_ink(CLR_TASKBAR_BG);
    int cx = x + 12, cy = y + 12;
    draw_circle_filled(cx, cy, 5, c);            // bell dome
    draw_fill_rect(cx - 6, cy, 13, 5, c);        // skirt
    draw_fill_rect(cx - 1, y + 3, 3, 3, c);      // top knob
    draw_fill_rect(cx - 7, cy + 5, 15, 2, c);    // base rim
    draw_fill_rect(cx - 1, cy + 7, 3, 3, c);     // clapper
    if (unread > 0) {
        int bx = x + TRAY_ICON_W - 8, by = y + 2;
        draw_circle_filled(bx, by + 1, 7, 0xFFE5484D);   // unread badge
        char n[3];
        if (unread > 9) { n[0]=57; n[1]=43; n[2]=0; } else { n[0]=(char)(48+unread); n[1]=0; }
        int tw = text_width_ttf(n, 11);
        draw_text_ttf(bx - tw/2, by - 6, n, 11, 0xFFFFFFFF);
    }
}

// #372: tiny Bresenham line (the compositor has no draw_line primitive).
static void tb_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    for (;;) {
        draw_putpixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}

// #372: Bluetooth indicator. state 0 = off (dim, slashed), 1 = on/idle,
// 2 = on/connected (accent-tinted rune + small link dot).
static void tray_draw_bt(int x, int y, int state) {
    // White + consistent with the other tray glyphs (bell/sound/widgets): the
    // rune is the readable ink (near-white on the dark bar) whether idle or
    // connected; only the OFF state dims. No blue accent. (desktop UX pass)
    uint32_t c = (state == 0) ? readable_ink_dim(CLR_TASKBAR_BG)
                              : readable_ink(CLR_TASKBAR_BG);
    // Rune geometry in a ~14px box centred in the 26px slot.
    int cx = x + 13;
    int top = y + 4, bot = y + 21, mid = y + 12;
    int lft = x + 7, rgt = x + 19;
    draw_vline(cx, top, bot - top, c);              // spine
    tb_line(cx, top, rgt, top + 4, c);              // upper diamond
    tb_line(rgt, top + 4, cx, mid, c);
    tb_line(cx, bot, rgt, bot - 4, c);              // lower diamond
    tb_line(rgt, bot - 4, cx, mid, c);
    tb_line(cx, mid, lft, top + 4, c);              // cross strokes to left tips
    tb_line(cx, mid, lft, bot - 4, c);
    if (state == 0) {
        // "off" slash in dim ink (consistent, not red)
        uint32_t sl = readable_ink_dim(CLR_TASKBAR_BG);
        for (int i = 0; i < 18; i++) draw_putpixel(x + 4 + i, y + 21 - i, sl);
    } else if (state == 2) {
        // connected link dot in white ink
        draw_circle_filled(x + 21, y + 6, 3, readable_ink(CLR_TASKBAR_BG));
    }
}

// #384: Network indicator. state 0 = disconnected (dim, slashed), 1 = wired
// connected (real net-up: monitor + plug glyph), 2 = Wi-Fi connected (arcs with
// `bars` filled from the mock signal).
static void tray_draw_net(int x, int y, int state, int bars) {
    if (state == 1) {
        // Wired: little monitor/ethernet plug.
        uint32_t c = readable_ink(CLR_TASKBAR_BG);
        draw_rect_outline(x + 5, y + 6, 16, 11, c);
        draw_fill_rect(x + 10, y + 17, 6, 2, c);
        draw_fill_rect(x + 8, y + 19, 10, 2, c);
        return;
    }
    // Wi-Fi arcs (three). White + consistent with the other tray glyphs: lit
    // arcs use the readable ink (near-white), unlit/disconnected arcs dim. No
    // blue accent. (desktop UX pass)
    uint32_t ink  = readable_ink(CLR_TASKBAR_BG);
    uint32_t base = (state == 0) ? readable_ink_dim(CLR_TASKBAR_BG) : ink;
    int cx = x + 13, by = y + 19;
    draw_fill_rect(cx - 1, by, 3, 3, base);                 // base dot
    int lvl = state == 2 ? bars : 0;                        // arcs filled
    for (int a = 1; a <= 3; a++) {
        int r = a * 4;
        uint32_t col = (a <= lvl) ? ink : base;
        for (int dx = -r; dx <= r; dx++) {
            int dy = (dx * dx) / (r * 2);
            draw_putpixel(cx + dx, by - r + dy - 1, col);
            draw_putpixel(cx + dx, by - r + dy - 2, col);
        }
    }
    if (state == 0)  // disconnected slash in dim ink (consistent, not red)
        for (int i = 0; i < 18; i++) draw_putpixel(x + 4 + i, y + 21 - i, readable_ink_dim(CLR_TASKBAR_BG));
}

// #387: tray metrics + top/bottom anchor state, shared by every dock layout.
int      g_tray_bar_top = 0;   // 1 = tray sits on a TOP bar (menus drop DOWN)
int32_t  g_tray_bar_y   = 0;   // current tray-row top (traymenu anchors to it)
int32_t  g_tray_bar_h   = 26;
static int tray_total_w(void) { return TRAY_N * TRAY_ICON_W + (TRAY_N - 1) * TRAY_ICON_GAP; }

// Render the 6-icon system tray starting at (x0,y). The glyph tint follows the
// current bar background: we briefly retarget CLR_TASKBAR_BG (readable_ink's
// reference) to bar_bg so a glass/blue/beveled bar still gets legible icons,
// then restore it. is_top marks a top-bar tray so its menus drop downward.
static void tray_render_core(int x0, int y, uint32_t bar_bg, int is_top) {
    // #372/#384: re-sync shared BT/Wi-Fi state every frame (self-throttled).
    bt_tick(); wifi_tick();
    g_tray_y = y; g_tray_h = 26;
    g_tray_bar_y = y; g_tray_bar_h = 26; g_tray_bar_top = is_top;
    for (int i = 0; i < TRAY_N; i++) g_tray_x[i] = x0 + i * (TRAY_ICON_W + TRAY_ICON_GAP);
    // Network state: REAL wired link wins; otherwise the Wi-Fi mock.
    int net_state, net_bars = 0;
    if (sys_net_is_up()) net_state = 1;
    else if (wifi_tray_state() == 2) {
        net_state = 2;
        int sig = wifi_tray_signal();
        net_bars = sig >= 75 ? 3 : sig >= 45 ? 2 : sig > 0 ? 1 : 0;
    } else net_state = 0;
    uint32_t saved = CLR_TASKBAR_BG;
    CLR_TASKBAR_BG = bar_bg;
    tray_draw_widgets(g_tray_x[TRAY_WIDGETS], g_tray_y, g_widgets_enabled);
    tray_draw_sound  (g_tray_x[TRAY_SOUND],   g_tray_y, g_tray_muted);
    tray_draw_net    (g_tray_x[TRAY_NET],     g_tray_y, net_state, net_bars);
    tray_draw_bt     (g_tray_x[TRAY_BT],      g_tray_y, bt_tray_state());
    tray_draw_sheep  (g_tray_x[TRAY_SHEEP],   g_tray_y, g_sheep_enabled);
    tray_draw_bell   (g_tray_x[TRAY_BELL],    g_tray_y, notif_unread());
    CLR_TASKBAR_BG = saved;
}

static void tray_render(int right_edge) {
    int x0 = right_edge - tray_total_w();
    int y  = g_taskbar_y + (TASKBAR_HEIGHT - 26) / 2;
    draw_vline(x0 - 6, g_taskbar_y + 6, TASKBAR_HEIGHT - 12, CLR_GAUGE_BORDER);
    tray_render_core(x0, y, CLR_TASKBAR_BG, 0);
}

// Shared tray hit-test: works for every layout because each records g_tray_x[]
// + g_tray_y/g_tray_h before a click can arrive. Returns true if consumed.
static bool tray_click(int32_t x, int32_t y) {
    if (g_tray_h <= 0 || y < g_tray_y || y >= g_tray_y + g_tray_h) return false;
    for (int i = 0; i < TRAY_N; i++) {
        if (x >= g_tray_x[i] && x < g_tray_x[i] + TRAY_ICON_W) {
            if (i == TRAY_BELL) notif_toggle_center();          // #168 bell -> center
            else traymenu_open_for_icon(i, g_tray_x[i] + TRAY_ICON_W / 2);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

static void taskbar_render_default(void) {
    // a) Background fill.
    draw_fill_rect(0, g_taskbar_y, g_fb_width, TASKBAR_HEIGHT, CLR_TASKBAR_BG);

    // b) Top border line.
    draw_hline(0, g_taskbar_y, g_fb_width, CLR_TASKBAR_BORDER);

    // c) Start button on the left.
    int32_t btn_x = TASKBAR_PADDING;
    int32_t btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;

    uint32_t btn_bg = g_start_menu_open ? CLR_TASKBAR_HOVER : CLR_START_BTN;
    draw_fill_rect(btn_x, btn_y, TASKBAR_BTN_SIZE, TASKBAR_BTN_SIZE, btn_bg);

    // Icon is drawn 2px inside the button bounds.
    icon_draw_scaled(ICON_CATEGORIES, btn_x + 2, btn_y + 2, 24, CLR_CHROME_TEXT);

    // c2) Maytera-logo command-launcher (Spotlight) button, immediately to the
    // right of Start. Opens the centered AI prompt overlay.
    int32_t logo_x = btn_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
    int32_t logo_y = btn_y;
    uint32_t logo_bg = g_launcher_open ? CLR_TASKBAR_HOVER : CLR_START_BTN;
    draw_fill_rect(logo_x, logo_y, TASKBAR_BTN_SIZE, TASKBAR_BTN_SIZE, logo_bg);
    if (!maylogo_draw(logo_x + 2, logo_y + 2, TASKBAR_BTN_SIZE - 4, logo_bg, CLR_CHROME_TEXT)) {
        // Fallback if /MAYLOGO.DAT is missing: a simple "M" glyph placeholder.
        draw_text_large(logo_x + 8, logo_y + 6, "M", CLR_CHROME_TEXT, 2);
    }

    // d) Four gauges, right-aligned.
    // Total block width occupied by all gauges.
    int32_t gauge_block_w = 4 * GAUGE_WIDTH + 3 * GAUGE_SPACING;
    int32_t gauge_start_x = g_fb_width - TASKBAR_PADDING - gauge_block_w;

    // c2) Open-window buttons, filling the strip between the start button and
    // the gauges. Refresh the list every frame so it stays current.
    {
        int n = wm_get_windows(g_tb_wins, TB_MAX_WINS);
        if (n < 0) n = 0;
        g_tb_win_count = n;

        // Count app windows (visible + titled).
        int app_count = 0;
        for (int i = 0; i < n; i++) {
            if (tb_window_is_app(&g_tb_wins[i])) app_count++;
        }

        // Start with the logo-launcher button occupying one more slot after Start.
        int32_t strip_x = logo_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
        int32_t tray_reserve = TRAY_N * TRAY_ICON_W + (TRAY_N - 1) * TRAY_ICON_GAP + 14;
    int32_t strip_end = gauge_start_x - TASKBAR_ICON_SPACE - tray_reserve;
        int32_t strip_w = strip_end - strip_x;

        g_tb_btn_n = 0;
        if (app_count > 0 && strip_w > 40) {
            // Fit buttons in the strip: preferred width TB_BTN_W, shrink if needed.
            int32_t bw = TB_BTN_W;
            int32_t need = app_count * (bw + TB_BTN_GAP);
            if (need > strip_w) bw = (strip_w / app_count) - TB_BTN_GAP;
            if (bw < TB_ICON_SZ + 8) bw = TB_ICON_SZ + 8;   // at least fit the icon
            g_tb_btn_w = bw;

            int32_t by = g_taskbar_y + 6;
            int32_t bh = TASKBAR_HEIGHT - 12;               // compact height
            int32_t bx = strip_x;
            g_tb_btn_y = by;
            g_tb_btn_h = bh;

            for (int i = 0; i < n; i++) {
                if (!tb_window_is_app(&g_tb_wins[i])) continue;
                if (bx + bw > strip_end) break;

                int is_focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
                int is_min     = g_tb_wins[i].minimized;
                // Focused window: lighter "pressed/active" button. Minimized:
                // dimmer. Otherwise the normal hover shade.
                uint32_t bg  = is_focused ? CLR_START_BTN
                             : (is_min ? CLR_TASKBAR_BG : CLR_TASKBAR_HOVER);
                uint32_t txt = is_min ? CLR_GAUGE_BORDER : CLR_CHROME_TEXT;
                draw_fill_rect(bx, by, bw, bh, bg);
                // Focused tile gets a subtle mid-grey outline (never white); other
                // tiles keep the themed gauge border. (desktop UX pass)
                extern uint32_t CLR_TASK_FOCUS_BORDER;
                draw_rect_outline(bx, by, bw, bh,
                                  is_focused ? CLR_TASK_FOCUS_BORDER : CLR_GAUGE_BORDER);

                // Icon on the left, vertically centred.
                int32_t icon_y = by + (bh - TB_ICON_SZ) / 2;
                icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                                 bx + 3, icon_y, TB_ICON_SZ, txt);

                // Label after the icon. #341: measure the pixel width and
                // ellipsize ("Maytera HiFi...") instead of hard-cutting mid-glyph.
                int32_t text_x = bx + 3 + TB_ICON_SZ + 4;
                int32_t avail  = (bx + bw) - text_x - 3;
                if (avail > 6) {
                    char label[72];
                    int  n = 0;
                    while (g_tb_wins[i].title[n] && n < 63) {
                        label[n] = g_tb_wins[i].title[n]; n++;
                    }
                    label[n] = '\0';
                    if (text_width(label) > avail) {
                        // Trim characters until title + "..." fits the width.
                        int ew = text_width("...");
                        while (n > 0 && text_width(label) + ew > avail)
                            label[--n] = '\0';
                        label[n] = '.'; label[n + 1] = '.';
                        label[n + 2] = '.'; label[n + 3] = '\0';
                    }
                    draw_text(text_x, by + (bh - 8) / 2, label, txt);
                }

                // Record hitbox + focus state for click handling.
                if (g_tb_btn_n < TB_MAX_WINS) {
                    g_tb_btn_x[g_tb_btn_n]       = bx;
                    g_tb_btn_id[g_tb_btn_n]      = g_tb_wins[i].id;
                    g_tb_btn_focused[g_tb_btn_n] = is_focused;
                    g_tb_btn_n++;
                }

                bx += bw + TB_BTN_GAP;
            }
        } else {
            g_tb_btn_w = 0;
        }
    }
    int32_t gauge_y       = g_taskbar_y + (TASKBAR_HEIGHT - GAUGE_HEIGHT) / 2;

    // Record gauge geometry so gauge_hit()/draw_perf_popup() can use it (#241).
    g_gauge_x0 = gauge_start_x;
    g_gauge_y  = gauge_y;

    // CPU gauge: per-core vertical bars (#279).
    draw_cpu_gauge(gauge_start_x,
                   gauge_y,
                   GAUGE_WIDTH, GAUGE_HEIGHT,
                   g_cpu_ncores, g_cpu_cores);

    // RAM gauge.
    draw_gauge(gauge_start_x + (GAUGE_WIDTH + GAUGE_SPACING),
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_ram_percent, CLR_GAUGE_RAM,
               "RAM", g_ram_str);

    // Disk gauge.
    draw_gauge(gauge_start_x + 2 * (GAUGE_WIDTH + GAUGE_SPACING),
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_disk_percent, CLR_GAUGE_DSK,
               "DSK", g_disk_str);

    // Network gauge.
    draw_gauge(gauge_start_x + 3 * (GAUGE_WIDTH + GAUGE_SPACING),
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_net_percent, CLR_GAUGE_NET,
               "NET", g_net_str);

    // Quick-settings system tray just left of the gauges.
    tray_render(gauge_start_x - 8);

    // #241: detailed performance popup, drawn on top of the bar.
    if (g_perf_open) draw_perf_popup();
}

static bool taskbar_handle_default(int32_t x, int32_t y, bool clicked) {
    // Not inside the taskbar strip.
    if (y < g_taskbar_y) {
        return false;
    }

    // System-tray icons: widgets toggle, sound mute, sheep toggle.
    if (clicked && tray_click(x, y)) return true;

    // #241: click a gauge to open the detailed performance popup.
    if (clicked) {
        int gh = gauge_hit(x, y);
        if (gh >= 0) {
            g_perf_open = 1;
            g_perf_sel  = gh;
            g_needs_redraw = true;
            return true;
        }
    }

    // Check whether the click landed on the start button.
    int32_t btn_x = TASKBAR_PADDING;
    int32_t btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;

    // Maytera-logo command-launcher button sits directly after Start.
    int32_t logo_x = btn_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;

    if (clicked &&
        x >= btn_x && x < btn_x + TASKBAR_BTN_SIZE &&
        y >= btn_y && y < btn_y + TASKBAR_BTN_SIZE) {
        // Toggle start menu visibility.
        g_start_menu_open = !g_start_menu_open;
        g_needs_redraw    = true;
    } else if (clicked &&
               x >= logo_x && x < logo_x + TASKBAR_BTN_SIZE &&
               y >= btn_y && y < btn_y + TASKBAR_BTN_SIZE) {
        // Open/close the centered AI command launcher (Spotlight).
        launcher_toggle();
        g_start_menu_open = false;    // never leave the start menu behind it
        g_needs_redraw    = true;
    } else if (clicked && g_tb_btn_w > 0 &&
               y >= g_tb_btn_y && y < g_tb_btn_y + g_tb_btn_h) {
        // Click on an open-window button: focus/raise that window.
        for (int i = 0; i < g_tb_btn_n; i++) {
            if (x >= g_tb_btn_x[i] && x < g_tb_btn_x[i] + g_tb_btn_w) {
                // Windows-style toggle: clicking the already-focused app's
                // button minimizes it; otherwise focus/raise (and restore).
                if (g_tb_btn_focused[i]) wm_minimize(g_tb_btn_id[i]);
                else                     wm_focus(g_tb_btn_id[i]);
                g_needs_redraw = true;
                break;
            }
        }
    }

    // The event is consumed by the taskbar regardless of where in the strip
    // it landed, so underlying desktop or windows do not receive it.
    return true;
}

// ============================================================================
// #387 Alternate dock/taskbar layouts
// ============================================================================
// g_dock_style selects the active layout; DOCK_DEFAULT is the classic bar above
// (byte-identical). The three alternates below each: draw their chrome + the
// shared 6-icon system tray, expose their bar geometry for menu anchoring, and
// hit-test the launcher, running-app switcher, and tray. They keep the desktop
// work area correct via taskbar_top_inset()/taskbar_bottom_inset().

int g_dock_style = DOCK_DEFAULT;
extern int g_draw_blend;   // draw.c global alpha (255 = opaque)

// (Layout metrics LUMINA_*/CLASSIC_UNIX_*/RETRO_BENCH_* are #defined at the top of this file.)

// Shared running-app / launcher slot records (Lumina / Classic UNIX / Retro Bench hit-testing).
// id == -1 marks the launcher/start slot; otherwise it is a window id.
#define LAY_MAX 24
static int32_t g_lay_x[LAY_MAX], g_lay_w[LAY_MAX], g_lay_id[LAY_MAX];
static int     g_lay_focused[LAY_MAX];
static int     g_lay_n;
static int32_t g_lay_y, g_lay_h;
static void lay_reset(int32_t y, int32_t h) { g_lay_n = 0; g_lay_y = y; g_lay_h = h; }
static void lay_add(int32_t x, int32_t w, int32_t id, int focused) {
    if (g_lay_n >= LAY_MAX) return;
    g_lay_x[g_lay_n] = x; g_lay_w[g_lay_n] = w;
    g_lay_id[g_lay_n] = id; g_lay_focused[g_lay_n] = focused; g_lay_n++;
}
static bool lay_click(int32_t x, int32_t y) {
    if (y < g_lay_y || y >= g_lay_y + g_lay_h) return false;
    for (int i = 0; i < g_lay_n; i++) {
        if (x >= g_lay_x[i] && x < g_lay_x[i] + g_lay_w[i]) {
            int32_t id = g_lay_id[i];
            if (id < 0)               g_start_menu_open = !g_start_menu_open;
            else if (g_lay_focused[i]) wm_minimize(id);
            else                       wm_focus(id);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

// "HH:MM" from the RTC (packed h<<16 | m<<8 | s).
static void tb_clock_str(char *out) {
    long rtc = sys_get_rtc_time();
    int h = (int)((rtc >> 16) & 0xFF), m = (int)((rtc >> 8) & 0xFF);
    out[0] = (char)('0' + (h / 10) % 10); out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + (m / 10) % 10); out[4] = (char)('0' + m % 10);
    out[5] = '\0';
}

// Refresh g_tb_wins/g_tb_win_count for the alternate layouts.
static int lay_refresh_windows(void) {
    int n = wm_get_windows(g_tb_wins, TB_MAX_WINS);
    if (n < 0) n = 0;
    g_tb_win_count = n;
    return n;
}

// A 3D bevel frame (Motif). raised: light top/left, dark bottom/right.
static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, int raised,
                  uint32_t lt, uint32_t dk) {
    uint32_t tl = raised ? lt : dk;
    uint32_t br = raised ? dk : lt;
    draw_hline(x, y, w, tl);              draw_vline(x, y, h, tl);
    draw_hline(x, y + h - 1, w, br);      draw_vline(x + w - 1, y, h, br);
}

// ---------------------------------------------------------------------------
// Lumina: top glass menu bar + floating bottom glass dock.
// ---------------------------------------------------------------------------
static int32_t g_lumina_menu_w = 64;   // clickable width of the left "Maytera" menu

static void taskbar_render_lumina(void) {
    g_gauge_x0 = 0;   // no gauges -> disable perf-popup hit testing
    int W = g_fb_width, mb = LUMINA_MENUBAR_H;
    int n = lay_refresh_windows();

    // ---- Top menu bar (translucent glass) ----
    uint32_t glass = 0xFFF3F4F7;
    int ob = g_draw_blend; g_draw_blend = 205;
    draw_fill_rect(0, 0, W, mb, glass);
    g_draw_blend = ob;
    draw_hline(0, mb - 1, W, 0x40202020);
    uint32_t ink = readable_ink(glass);

    // Left: system/app menu label (opens start menu). Bold-ish Maytera + app.
    draw_text_ttf(12, 5, "Maytera", 15, ink);
    g_lumina_menu_w = text_width_ttf("Maytera", 15) + 20;
    for (int i = 0; i < n; i++) {
        if (g_tb_wins[i].focused && g_tb_wins[i].visible && !g_tb_wins[i].minimized &&
            tb_window_is_app(&g_tb_wins[i])) {
            draw_text_ttf(g_lumina_menu_w + 6, 6, g_tb_wins[i].title, 13, ink);
            break;
        }
    }

    // Right: clock then the system tray (glassy).
    char clk[6]; tb_clock_str(clk);
    int clkw = text_width_ttf(clk, 13);
    int clk_x = W - 12 - clkw;
    draw_text_ttf(clk_x, 6, clk, 13, ink);
    tray_render_core(clk_x - 8 - tray_total_w(), (mb - 26) / 2, glass, 1);

    // ---- Bottom floating dock (glass pill, magnifying icons) ----
    int items = 1;   // launcher + running apps
    for (int i = 0; i < n; i++) if (tb_window_is_app(&g_tb_wins[i])) items++;
    int icon = LUMINA_DOCK_ICON, pad = LUMINA_DOCK_PAD;
    int dock_w = items * icon + (items + 1) * pad;
    if (dock_w > W - 24) dock_w = W - 24;
    int dh = icon + 2 * pad;
    int dx = (W - dock_w) / 2;
    int dy = g_fb_height - dh - 8;

    ob = g_draw_blend; g_draw_blend = 175;
    draw_rounded_rect(dx, dy, dock_w, dh, 18, 0xFFECEDF1);
    g_draw_blend = ob;
    // Subtle top highlight line inside the pill.
    ob = g_draw_blend; g_draw_blend = 90;
    draw_hline(dx + 18, dy + 1, dock_w - 36, 0xFFFFFFFF);
    g_draw_blend = ob;

    lay_reset(dy, dh);
    int step = icon + pad;
    int base_y = dy + pad + icon;      // icon baseline (bottom) so they grow up
    int mouse_in = (g_mouse_y >= dy - 24 && g_mouse_y <= g_fb_height);
    int slot = 0;

    // Draw one dock slot: base cell at index `slot`, icon id, running/focused.
    // Magnifies with cursor proximity; hit-tests on the base cell.
    #define LUMINA_SLOT_CX(s) (dx + pad + (s) * step + icon / 2)
    // Launcher (Categories/start icon).
    {
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        icon_draw_scaled(ICON_CATEGORIES, cx - sz / 2, base_y - sz, sz, ink);
        lay_add(dx + pad + slot * step, icon, -1, 0);
        slot++;
    }
    for (int i = 0; i < n && slot < items; i++) {
        if (!tb_window_is_app(&g_tb_wins[i])) continue;
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
        icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                         cx - sz / 2, base_y - sz, sz,
                         g_tb_wins[i].minimized ? 0xFF8A8A90 : 0xFF303038);
        // Running indicator dot below the icon.
        draw_circle_filled(cx, dy + dh - 3, 2, focused ? 0xFF2A2A30 : 0xFF6A6A72);
        lay_add(dx + pad + slot * step, icon, g_tb_wins[i].id, focused);
        slot++;
    }
    #undef LUMINA_SLOT_CX
}

static bool taskbar_handle_lumina(int32_t x, int32_t y, bool clicked) {
    int mb = LUMINA_MENUBAR_H;
    // Menu bar band.
    if (y >= 0 && y < mb) {
        if (clicked) {
            if (x < g_lumina_menu_w) { g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true; }
            if (tray_click(x, y)) return true;
        }
        return true;   // swallow all menu-bar clicks
    }
    // Dock band.
    if (clicked && lay_click(x, y)) return true;
    if (y >= g_lay_y && y < g_lay_y + g_lay_h) return true;  // swallow clicks on the pill
    return false;
}

// ---------------------------------------------------------------------------
// Classic UNIX: beveled bottom panel (CDE/Motif-style), launcher + apps, a workspace
// switcher in the middle, tray + clock at the right.
// ---------------------------------------------------------------------------
static int g_cu_ws = 0;   // highlighted workspace (cosmetic switcher)
static int32_t g_cu_ws_x[4], g_cu_ws_y, g_cu_ws_w, g_cu_ws_h;

static void taskbar_render_classic_unix(void) {
    g_gauge_x0 = 0;
    int W = g_fb_width, py = g_fb_height - CLASSIC_UNIX_PANEL_H;
    uint32_t face = 0xFFB6B6A8;   // Motif gray
    uint32_t lt = 0xFFE6E6DA, dk = 0xFF70706A;
    int n = lay_refresh_windows();

    // Panel background, raised bevel with a thin top rule.
    draw_fill_rect(0, py, W, CLASSIC_UNIX_PANEL_H, face);
    bevel(0, py, W, CLASSIC_UNIX_PANEL_H, 1, lt, dk);
    draw_hline(0, py, W, 0xFF3A3A34);

    uint32_t ink = readable_ink(face);
    int cy = py + 6, ch = CLASSIC_UNIX_PANEL_H - 12;   // tile row
    lay_reset(cy, ch);

    // Launcher tile (start).
    int lx = 6, lw = 46;
    bevel(lx, cy, lw, ch, g_start_menu_open ? 0 : 1, lt, dk);
    draw_fill_rect(lx + 1, cy + 1, lw - 2, ch - 2, face);
    icon_draw_scaled(ICON_CATEGORIES, lx + (lw - 28) / 2, cy + (ch - 28) / 2, 28, ink);
    lay_add(lx, lw, -1, 0);
    int bx = lx + lw + 6;

    // Right cluster width (tray + clock), reserved so app tiles stop short.
    char clk[6]; tb_clock_str(clk);
    int right_w = tray_total_w() + 12 + text_width(clk) + 12;
    // Workspace switcher block (centered-ish, fixed).
    int ws_w = 4 * 22 + 10, ws_h = ch;
    int ws_x = (W - ws_w) / 2;

    // App tiles fill from bx to just before the workspace block.
    int apps_end = ws_x - 8;
    int napps = 0; for (int i = 0; i < n; i++) if (tb_window_is_app(&g_tb_wins[i])) napps++;
    if (napps > 0 && apps_end - bx > 40) {
        int tw = 128; int need = napps * (tw + 4);
        if (need > apps_end - bx) tw = (apps_end - bx) / napps - 4;
        if (tw < 40) tw = 40;
        for (int i = 0; i < n; i++) {
            if (!tb_window_is_app(&g_tb_wins[i])) continue;
            if (bx + tw > apps_end) break;
            int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
            bevel(bx, cy, tw, ch, focused ? 0 : 1, lt, dk);
            draw_fill_rect(bx + 1, cy + 1, tw - 2, ch - 2, focused ? 0xFFC8C8BC : face);
            icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                             bx + 4, cy + (ch - 20) / 2, 20,
                             g_tb_wins[i].minimized ? dk : ink);
            // Ellipsized label.
            char lbl[48]; int k = 0;
            while (g_tb_wins[i].title[k] && k < 40) { lbl[k] = g_tb_wins[i].title[k]; k++; }
            lbl[k] = '\0';
            int avail = tw - 30;
            while (k > 0 && text_width(lbl) > avail) lbl[--k] = '\0';
            draw_text(bx + 28, cy + (ch - 8) / 2, lbl, g_tb_wins[i].minimized ? dk : ink);
            lay_add(bx, tw, g_tb_wins[i].id, focused);
            bx += tw + 4;
        }
    }

    // Workspace switcher: recessed box with 4 buttons (One..Four).
    bevel(ws_x, cy, ws_w, ws_h, 0, lt, dk);
    draw_fill_rect(ws_x + 1, cy + 1, ws_w - 2, ws_h - 2, 0xFF9A9A8E);
    g_cu_ws_y = cy + 3; g_cu_ws_h = ws_h - 6; g_cu_ws_w = 20;
    static const char *wsl[4] = { "1", "2", "3", "4" };
    for (int i = 0; i < 4; i++) {
        int wx = ws_x + 5 + i * 22;
        g_cu_ws_x[i] = wx;
        int on = (i == g_cu_ws);
        bevel(wx, g_cu_ws_y, 20, g_cu_ws_h, on ? 0 : 1, lt, dk);
        draw_fill_rect(wx + 1, g_cu_ws_y + 1, 18, g_cu_ws_h - 2, on ? 0xFF3C6EB4 : face);
        draw_text(wx + 6, g_cu_ws_y + (g_cu_ws_h - 8) / 2, wsl[i], on ? 0xFFFFFFFF : ink);
    }

    // Right: tray + clock in a recessed strip.
    int rx = W - right_w;
    tray_render_core(rx + 4, cy + (ch - 26) / 2, face, 0);
    draw_text(W - text_width(clk) - 10, cy + (ch - 8) / 2, clk, ink);
    (void)ws_h;
}

static bool taskbar_handle_classic_unix(int32_t x, int32_t y, bool clicked) {
    int py = g_fb_height - CLASSIC_UNIX_PANEL_H;
    if (y < py) return false;
    if (clicked) {
        // Workspace switcher.
        if (y >= g_cu_ws_y && y < g_cu_ws_y + g_cu_ws_h) {
            for (int i = 0; i < 4; i++)
                if (x >= g_cu_ws_x[i] && x < g_cu_ws_x[i] + g_cu_ws_w) {
                    g_cu_ws = i; g_needs_redraw = true; return true;
                }
        }
        if (tray_click(x, y)) return true;
        if (lay_click(x, y)) return true;
    }
    return true;   // swallow all clicks on the panel
}

// ---------------------------------------------------------------------------
// Retro Bench: top screen title bar with depth/zoom gadgets at the right.
// Blue/orange/white/black palette, blocky retro bitmap font.
// ---------------------------------------------------------------------------
#define RB_BLUE   0xFF0055AA
#define RB_WHITE  0xFFFFFFFF
#define RB_BLACK  0xFF000000
#define RB_ORANGE 0xFFFF8800

static void rb_gadget_depth(int x, int y) {
    // Two overlapping windows (the classic depth-arrangement gadget).
    draw_fill_rect(x, y + 2, 12, 10, RB_WHITE); draw_rect_outline(x, y + 2, 12, 10, RB_BLACK);
    draw_fill_rect(x + 6, y - 2, 12, 10, RB_ORANGE); draw_rect_outline(x + 6, y - 2, 12, 10, RB_BLACK);
}
static void rb_gadget_zoom(int x, int y) {
    // Box with a smaller box in the top-left corner (RB zoom gadget).
    draw_fill_rect(x, y, 16, 14, RB_WHITE); draw_rect_outline(x, y, 16, 14, RB_BLACK);
    draw_fill_rect(x + 1, y + 1, 8, 6, RB_ORANGE); draw_rect_outline(x + 1, y + 1, 8, 6, RB_BLACK);
}

static int32_t g_rb_gadg_x = 0;   // left edge of the gadget cluster (non-clickable chrome)

static void taskbar_render_retro_bench(void) {
    g_gauge_x0 = 0;
    int W = g_fb_width, bh = RETRO_BENCH_BAR_H;
    int n = lay_refresh_windows();

    // Bar background + black baseline (Retro Bench screen title bar).
    draw_fill_rect(0, 0, W, bh, RB_BLUE);
    draw_hline(0, bh - 1, W, RB_BLACK);
    draw_hline(0, bh, W, RB_BLACK);

    // Left: screen title (retro look via the 8px bitmap font).
    draw_text(6, (bh - 8) / 2, "Retro Bench Screen", RB_WHITE);

    // Right: gadget cluster (zoom then depth), reserved.
    int gadg_w = 44;
    g_rb_gadg_x = W - gadg_w;
    rb_gadget_zoom(W - 42, (bh - 14) / 2);
    rb_gadget_depth(W - 22, (bh - 14) / 2 + 2);

    // Clock + tray to the left of the gadgets.
    char clk[6]; tb_clock_str(clk);
    int clk_x = g_rb_gadg_x - 8 - text_width(clk);
    draw_text(clk_x, (bh - 8) / 2, clk, RB_WHITE);
    int tray_right = clk_x - 8;
    tray_render_core(tray_right - tray_total_w(), (bh - 26) / 2, RB_BLUE, 1);

    // Running apps: text buttons after the title, focused = orange field.
    lay_reset(0, bh);
    int bx = 6 + text_width("Retro Bench Screen") + 16;
    int apps_end = g_tray_x[0] - 10;   // stop before the tray
    for (int i = 0; i < n; i++) {
        if (!tb_window_is_app(&g_tb_wins[i])) continue;
        char lbl[40]; int k = 0;
        while (g_tb_wins[i].title[k] && k < 32) { lbl[k] = g_tb_wins[i].title[k]; k++; }
        lbl[k] = '\0';
        int tw = text_width(lbl) + 12;
        if (bx + tw > apps_end) break;
        int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
        if (focused) {
            draw_fill_rect(bx, 2, tw, bh - 4, RB_ORANGE);
            draw_text(bx + 6, (bh - 8) / 2, lbl, RB_BLACK);
        } else {
            draw_text(bx + 6, (bh - 8) / 2, lbl, g_tb_wins[i].minimized ? 0xFFAAC4E4 : RB_WHITE);
        }
        lay_add(bx, tw, g_tb_wins[i].id, focused);
        bx += tw + 4;
    }
}

static bool taskbar_handle_retro_bench(int32_t x, int32_t y, bool clicked) {
    if (y >= RETRO_BENCH_BAR_H) return false;
    if (clicked) {
        // Title label acts as the launcher (Retro Bench menu).
        if (x < 6 + text_width("Retro Bench Screen")) {
            g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true;
        }
        if (tray_click(x, y)) return true;
        if (lay_click(x, y)) return true;
    }
    return true;   // swallow bar clicks
}

// ---------------------------------------------------------------------------
// Dispatchers + work-area insets.
// ---------------------------------------------------------------------------
void taskbar_render(void) {
    switch (g_dock_style) {
        case DOCK_LUMINA: g_taskbar_y = g_fb_height - LUMINA_DOCK_RESERVE; taskbar_render_lumina(); break;
        case DOCK_CLASSIC_UNIX:   g_taskbar_y = g_fb_height - CLASSIC_UNIX_PANEL_H;      taskbar_render_classic_unix();   break;
        case DOCK_RETRO_BENCH: g_taskbar_y = 0;                             taskbar_render_retro_bench(); break;
        default:         g_taskbar_y = g_fb_height - TASKBAR_HEIGHT;   taskbar_render_default(); break;
    }
}

bool taskbar_handle_mouse(int32_t x, int32_t y, bool clicked) {
    switch (g_dock_style) {
        case DOCK_LUMINA: return taskbar_handle_lumina(x, y, clicked);
        case DOCK_CLASSIC_UNIX:   return taskbar_handle_classic_unix(x, y, clicked);
        case DOCK_RETRO_BENCH: return taskbar_handle_retro_bench(x, y, clicked);
        default:         return taskbar_handle_default(x, y, clicked);
    }
}

int taskbar_top_inset(void) {
    return (g_dock_style == DOCK_LUMINA) ? LUMINA_MENUBAR_H
         : (g_dock_style == DOCK_RETRO_BENCH) ? RETRO_BENCH_BAR_H : 0;
}
int taskbar_bottom_inset(void) {
    switch (g_dock_style) {
        case DOCK_LUMINA: return LUMINA_DOCK_RESERVE;
        case DOCK_CLASSIC_UNIX:   return CLASSIC_UNIX_PANEL_H;
        case DOCK_RETRO_BENCH: return 0;
        default:         return TASKBAR_HEIGHT;
    }
}
int taskbar_menu_drops_from_top(void) {
    return (g_dock_style == DOCK_LUMINA) || (g_dock_style == DOCK_RETRO_BENCH);
}
void taskbar_set_style(int s) {
    if (s < 0 || s >= DOCK_COUNT) s = DOCK_DEFAULT;
    if (s == g_dock_style) return;
    g_dock_style = s;
    g_perf_open = 0;
    g_start_menu_open = false;
    g_tray_menu_open = 0;
    g_needs_redraw = true;
}

// #241: is the performance popup currently showing?
bool taskbar_popup_active(void) {
    return g_perf_open != 0;
}

// #241: handle mouse while the performance popup is open. Runs in main.c BEFORE
// taskbar_handle_mouse so it can intercept clicks anywhere on screen.
bool taskbar_popup_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_perf_open) return false;

    int inside_popup = (x >= g_pp_x && x < g_pp_x + g_pp_w &&
                        y >= g_pp_y && y < g_pp_y + g_pp_h);

    if (!clicked) {
        // Swallow hover only while over the popup, so it does not bleed through.
        return inside_popup != 0;
    }

    if (inside_popup) {
        // Task Manager button?
        if (x >= g_pp_tm_x && x < g_pp_tm_x + g_pp_tm_w &&
            y >= g_pp_tm_y && y < g_pp_tm_y + g_pp_tm_h) {
            sys_spawn("/APPS/taskmanager");
            g_perf_open = 0;
        }
        g_needs_redraw = true;
        return true;
    }

    // Click on a gauge: toggle (same gauge closes) or switch.
    int gh = gauge_hit(x, y);
    if (gh >= 0) {
        if (gh == g_perf_sel) g_perf_open = 0;
        else                  g_perf_sel  = gh;
        g_needs_redraw = true;
        return true;
    }

    // Click anywhere else: dismiss the popup and consume the click.
    g_perf_open = 0;
    g_needs_redraw = true;
    return true;
}

int32_t taskbar_get_y(void) {
    // Top of the reserved BOTTOM work-area edge (layout-aware, #387). For a
    // top-bar-only layout (Retro Bench) the bottom inset is 0 so this is the screen
    // bottom, i.e. no bottom reservation.
    return g_fb_height - taskbar_bottom_inset();
}
