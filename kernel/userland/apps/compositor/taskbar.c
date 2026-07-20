// taskbar.c - Taskbar with system gauges for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Static state
// ============================================================================

static int32_t g_taskbar_y;
static int     g_cpu_percent;
static int     g_ram_percent;
static int     g_disk_percent;
static int     g_net_percent;
static uint64_t g_gauge_update_time;
static char    g_cpu_str[8];
static char    g_ram_str[8];
static char    g_disk_str[8];
static char    g_net_str[8];

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
    draw_text(x + 4, text_y, label, CLR_TEXT_WHITE);

    // Value: right-aligned with small padding.
    int32_t val_x = x + w - text_width(value) - 4;
    draw_text(val_x, text_y, value, CLR_TEXT_WHITE);
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

    // Throttle updates: only refresh gauge readings every ~100 clock ticks.
    if (now - g_gauge_update_time < 100) {
        return;
    }
    g_gauge_update_time = now;

    // CPU usage (0-100).
    g_cpu_percent = sys_get_cpu_usage();
    fmt_percent(g_cpu_str, g_cpu_percent);

    // RAM usage derived from page counts reported by the kernel.
    {
        unsigned long total = 0;
        unsigned long used  = 0;
        sys_get_mem_info(&total, &used);
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
        if (total > 0) {
            long used = total - free;
            if (used < 0) used = 0;
            g_disk_percent = (int)(used * 100L / total);
        } else {
            g_disk_percent = 0;
        }
        fmt_percent(g_disk_str, g_disk_percent);
    }

    // Network: no live syscall available; hold at a nominal 50% ("Up").
    g_net_percent = 50;
    fmt_percent(g_net_str, g_net_percent);
}

void taskbar_render(void) {
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
    icon_draw_scaled(ICON_CATEGORIES, btn_x + 2, btn_y + 2, 24, CLR_TEXT_WHITE);

    // d) Four gauges, right-aligned.
    // Total block width occupied by all gauges.
    int32_t gauge_block_w = 4 * GAUGE_WIDTH + 3 * GAUGE_SPACING;
    int32_t gauge_start_x = g_fb_width - TASKBAR_PADDING - gauge_block_w;
    int32_t gauge_y       = g_taskbar_y + (TASKBAR_HEIGHT - GAUGE_HEIGHT) / 2;

    // CPU gauge.
    draw_gauge(gauge_start_x,
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_cpu_percent, CLR_GAUGE_CPU,
               "CPU", g_cpu_str);

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
}

bool taskbar_handle_mouse(int32_t x, int32_t y, bool clicked) {
    // Not inside the taskbar strip.
    if (y < g_taskbar_y) {
        return false;
    }

    // Check whether the click landed on the start button.
    int32_t btn_x = TASKBAR_PADDING;
    int32_t btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;

    if (clicked &&
        x >= btn_x && x < btn_x + TASKBAR_BTN_SIZE &&
        y >= btn_y && y < btn_y + TASKBAR_BTN_SIZE) {
        // Toggle start menu visibility.
        g_start_menu_open = !g_start_menu_open;
        g_needs_redraw    = true;
    }

    // The event is consumed by the taskbar regardless of where in the strip
    // it landed, so underlying desktop or windows do not receive it.
    return true;
}

int32_t taskbar_get_y(void) {
    return g_taskbar_y;
}
