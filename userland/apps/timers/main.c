// main.c - MayteraOS Timers app
// Stopwatch, countdown timer, and pomodoro in one utility. Fills a real gap:
// the clock app only displays the time of day; nothing in the OS could
// actually time anything. Uses the monotonic uptime_ms() syscall for all
// timing (immune to RTC changes) and posts a notification toast via
// notify_post() when a countdown or pomodoro phase completes.
// Styled to match Settings/Notes: theme-following palette, shared style
// engine widgets, antialiased TTF text, live resize reflow.
//
// Keyboard: 1/2/3 or Left/Right switch tabs, Space start/pause,
//           L lap (stopwatch), N skip (pomodoro), R reset.

#include "syscall.h"
#include "gui.h"
#include "notify.h"
#include "stdio.h"
#include "string.h"

#define draw_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

static int g_win_w = 430, g_win_h = 540;   // live content size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h
#define MIN_W 380
#define MIN_H 460
#define PAD   16

// ---------------------------------------------------------------------------
// Theme palette (same mapping idiom as Notes/Settings)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_CARD, COL_SEP;
static uint32_t COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEL, COL_WARN;
static int g_last_theme = -1;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_CARD=0x00F4F4F4; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00CCCCCC;
            COL_SEL=0x00D6E4FB; COL_WARN=0x00C0392B; break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00808080;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_SEL=0x00A0A0A0; COL_WARN=0x00A00000; break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_CARD=0x001E4050; COL_SEP=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_TEXT_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_SEL=0x00305060; COL_WARN=0x00FF7060; break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_CARD=0x00343B49; COL_SEP=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_TEXT_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_SEL=0x00434C5E; COL_WARN=0x00BF616A; break;
        default: // Dark
            COL_BG=0x00252525; COL_CARD=0x002E2E2E; COL_SEP=0x00404040;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_TEXT_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_SEL=0x0037527A; COL_WARN=0x00E74C3C; break;
    }
    gui_set_style(kt == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface=COL_BG; p.surface_raised=COL_CARD; p.ink=COL_TEXT; p.ink_dim=COL_TEXT2;
    p.accent=COL_ACCENT; p.accent_hover=COL_ACCENT; p.border=COL_SEP;
    p.field_bg=COL_FIELD; p.field_border=COL_FIELD_BORDER; p.track=COL_SEP;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int g_window = -1;
static int g_tab = 0;               // 0 stopwatch, 1 timer, 2 pomodoro

typedef long long ms_t;
static ms_t now_ms(void) { return (ms_t)uptime_ms(); }

// Stopwatch
static int  sw_running = 0;
static ms_t sw_accum = 0;           // elapsed while paused
static ms_t sw_start = 0;           // uptime at last start
#define MAX_LAPS 64
static ms_t sw_laps[MAX_LAPS];      // total elapsed at each lap
static int  sw_lap_count = 0;

static ms_t sw_elapsed(void) {
    return sw_running ? sw_accum + (now_ms() - sw_start) : sw_accum;
}

// Countdown timer
static ms_t tm_set = 5 * 60000;     // configured duration
static ms_t tm_remain = 5 * 60000;  // remaining when paused
static ms_t tm_deadline = 0;        // uptime deadline when running
static int  tm_running = 0;
static int  tm_finished = 0;        // flash "time's up" state

static ms_t tm_remaining(void) {
    if (!tm_running) return tm_remain;
    ms_t r = tm_deadline - now_ms();
    return r > 0 ? r : 0;
}

// Pomodoro
#define PO_FOCUS_MS   (25 * 60000LL)
#define PO_SHORT_MS   ( 5 * 60000LL)
#define PO_LONG_MS    (15 * 60000LL)
static int  po_phase = 0;           // 0 focus, 1 short break, 2 long break
static int  po_running = 0;
static ms_t po_remain = PO_FOCUS_MS;
static ms_t po_deadline = 0;
static int  po_done = 0;            // completed focus sessions

static ms_t po_phase_len(int ph) {
    return ph == 0 ? PO_FOCUS_MS : (ph == 1 ? PO_SHORT_MS : PO_LONG_MS);
}
static ms_t po_remaining(void) {
    if (!po_running) return po_remain;
    ms_t r = po_deadline - now_ms();
    return r > 0 ? r : 0;
}

// ---------------------------------------------------------------------------
// Time formatting
// ---------------------------------------------------------------------------
static void fmt_hms(ms_t ms, char *out, int cap, int tenths) {
    if (ms < 0) ms = 0;
    int t  = (int)((ms / 100) % 10);
    long long s = ms / 1000;
    int h = (int)(s / 3600);
    int m = (int)((s % 3600) / 60);
    int sec = (int)(s % 60);
    if (h > 0) {
        if (tenths) snprintf(out, cap, "%d:%02d:%02d.%d", h, m, sec, t);
        else        snprintf(out, cap, "%d:%02d:%02d", h, m, sec);
    } else {
        if (tenths) snprintf(out, cap, "%02d:%02d.%d", m, sec, t);
        else        snprintf(out, cap, "%02d:%02d", m, sec);
    }
}

// ---------------------------------------------------------------------------
// Button table: rebuilt each frame from the live window size, shared by
// drawing and hit testing so geometry can never drift apart.
// ---------------------------------------------------------------------------
enum {
    B_SW_STARTPAUSE, B_SW_LAP, B_SW_RESET,
    B_TM_P1, B_TM_P3, B_TM_P5, B_TM_P10, B_TM_P25, B_TM_P60,
    B_TM_M1M, B_TM_M10S, B_TM_A10S, B_TM_A1M,
    B_TM_STARTPAUSE, B_TM_RESET,
    B_PO_STARTPAUSE, B_PO_SKIP, B_PO_RESET,
};

typedef struct { int id; int x, y, w, h; char label[16]; int variant; int enabled; } btn_t;
static btn_t g_btns[20];
static int g_nbtns = 0;
static int g_hover = -1;            // index into g_btns
static int g_hover_tab = -1;

static void add_btn(int id, int x, int y, int w, int h, const char *label,
                    int variant, int enabled) {
    btn_t *b = &g_btns[g_nbtns++];
    b->id = id; b->x = x; b->y = y; b->w = w; b->h = h;
    strlcpy(b->label, label, sizeof(b->label));
    b->variant = variant; b->enabled = enabled;
}

#define TAB_H 32
#define TAB_Y 10
static const char *TAB_NAMES[3] = { "Stopwatch", "Timer", "Pomodoro" };

static int content_top(void) { return TAB_Y + TAB_H + 12; }

static void build_buttons(void) {
    g_nbtns = 0;
    int cw = WIN_W - 2 * PAD;
    int ct = content_top();

    if (g_tab == 0) {
        int bw = (cw - 2 * 10) / 3, by = ct + 118;
        add_btn(B_SW_STARTPAUSE, PAD, by, bw, 38,
                sw_running ? "Pause" : (sw_accum > 0 ? "Resume" : "Start"),
                GUI_BTN_PRIMARY, 1);
        add_btn(B_SW_LAP, PAD + bw + 10, by, bw, 38, "Lap",
                GUI_BTN_SECONDARY, sw_running);
        add_btn(B_SW_RESET, PAD + 2 * (bw + 10), by, bw, 38, "Reset",
                GUI_BTN_SECONDARY, sw_accum > 0 || sw_running || sw_lap_count > 0);
    } else if (g_tab == 1) {
        int py = ct + 128;
        int pw = (cw - 2 * 8) / 3;
        static const char *pl[6] = { "1 min", "3 min", "5 min", "10 min", "25 min", "60 min" };
        static const int   pid[6] = { B_TM_P1, B_TM_P3, B_TM_P5, B_TM_P10, B_TM_P25, B_TM_P60 };
        for (int i = 0; i < 6; i++) {
            int r = i / 3, c = i % 3;
            add_btn(pid[i], PAD + c * (pw + 8), py + r * 36, pw, 30, pl[i],
                    GUI_BTN_SECONDARY, !tm_running);
        }
        int ay = py + 2 * 36 + 8;
        int aw = (cw - 3 * 8) / 4;
        add_btn(B_TM_M1M,  PAD,                 ay, aw, 30, "-1m",  GUI_BTN_GHOST, 1);
        add_btn(B_TM_M10S, PAD + (aw + 8),      ay, aw, 30, "-10s", GUI_BTN_GHOST, 1);
        add_btn(B_TM_A10S, PAD + 2 * (aw + 8),  ay, aw, 30, "+10s", GUI_BTN_GHOST, 1);
        add_btn(B_TM_A1M,  PAD + 3 * (aw + 8),  ay, aw, 30, "+1m",  GUI_BTN_GHOST, 1);
        int gy = ay + 40;
        int gw = (cw - 10) / 2;
        add_btn(B_TM_STARTPAUSE, PAD, gy, gw, 38,
                tm_running ? "Pause" : "Start", GUI_BTN_PRIMARY, tm_remaining() > 0);
        add_btn(B_TM_RESET, PAD + gw + 10, gy, gw, 38, "Reset", GUI_BTN_SECONDARY, 1);
    } else {
        int by = ct + 208;
        int bw = (cw - 2 * 10) / 3;
        add_btn(B_PO_STARTPAUSE, PAD, by, bw, 38,
                po_running ? "Pause" : "Start", GUI_BTN_PRIMARY, 1);
        add_btn(B_PO_SKIP,  PAD + bw + 10,       by, bw, 38, "Skip",  GUI_BTN_SECONDARY, 1);
        add_btn(B_PO_RESET, PAD + 2 * (bw + 10), by, bw, 38, "Reset", GUI_BTN_SECONDARY, 1);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void draw_tabs(void) {
    int tw = (WIN_W - 2 * PAD - 2 * 6) / 3;
    for (int i = 0; i < 3; i++) {
        int x = PAD + i * (tw + 6);
        uint32_t bg = (i == g_tab) ? COL_SEL : (i == g_hover_tab ? COL_CARD : COL_BG);
        gui_fill_rounded_aa(g_window, x, TAB_Y, tw, TAB_H, 5, bg, COL_BG);
        gui_text_ttf_centered(g_window, x, TAB_Y, tw, TAB_H, TAB_NAMES[i],
                              i == g_tab ? COL_TEXT : COL_TEXT2, 14);
        if (i == g_tab)
            win_draw_rect(g_window, x + 10, TAB_Y + TAB_H - 3, tw - 20, 3, COL_ACCENT);
    }
}

static void draw_big_time(int y, const char *s, uint32_t color) {
    int size = 46;
    if (gui_ttf_width(s, size) > WIN_W - 2 * PAD) size = 34;
    gui_text_ttf_centered(g_window, 0, y, WIN_W, 56, s, color, size);
}

static void draw_buttons(void) {
    for (int i = 0; i < g_nbtns; i++) {
        btn_t *b = &g_btns[i];
        gui_state_t st = !b->enabled ? GUI_ST_DISABLED
                        : (i == g_hover ? GUI_ST_HOVER : GUI_ST_NORMAL);
        gui_button(g_window, b->x, b->y, b->w, b->h, b->label,
                   (gui_btn_variant_t)b->variant, st);
    }
}

static void draw_stopwatch(void) {
    int ct = content_top();
    char t[24];
    fmt_hms(sw_elapsed(), t, sizeof(t), 1);
    draw_big_time(ct + 30, t, COL_TEXT);
    draw_text_sz(g_window, PAD, ct + 4, sw_running ? "Running" : "Stopped", 12,
                 sw_running ? COL_ACCENT : COL_TEXT_DIM);

    // lap list card
    int ly = ct + 170;
    int lh = WIN_H - ly - PAD;
    if (lh > 40) {
        gui_card(g_window, PAD, ly, WIN_W - 2 * PAD, lh);
        draw_text_sz(g_window, PAD + 12, ly + 8, "Lap", 12, COL_TEXT_DIM);
        draw_text_sz(g_window, PAD + 80, ly + 8, "Split", 12, COL_TEXT_DIM);
        int tx = WIN_W - PAD - 12 - gui_ttf_width("Total", 12);
        draw_text_sz(g_window, tx, ly + 8, "Total", 12, COL_TEXT_DIM);
        win_draw_rect(g_window, PAD + 8, ly + 28, WIN_W - 2 * PAD - 16, 1, COL_SEP);

        int rows = (lh - 36) / 22;
        int shown = 0;
        for (int i = sw_lap_count - 1; i >= 0 && shown < rows; i--, shown++) {
            int ry = ly + 34 + shown * 22;
            char num[8], split[24], total[24];
            snprintf(num, sizeof(num), "%d", i + 1);
            ms_t prev = (i > 0) ? sw_laps[i - 1] : 0;
            fmt_hms(sw_laps[i] - prev, split, sizeof(split), 1);
            fmt_hms(sw_laps[i], total, sizeof(total), 1);
            draw_text_sz(g_window, PAD + 12, ry, num, 12, COL_TEXT2);
            draw_text_sz(g_window, PAD + 80, ry, split, 12, COL_TEXT);
            draw_text_sz(g_window, WIN_W - PAD - 12 - gui_ttf_width(total, 12), ry,
                         total, 12, COL_TEXT2);
        }
        if (sw_lap_count == 0)
            draw_text_sz(g_window, PAD + 12, ly + 36, "No laps yet", 12, COL_TEXT_DIM);
    }
}

static void draw_timer(void) {
    int ct = content_top();
    ms_t rem = tm_remaining();
    char t[24];
    fmt_hms(rem, t, sizeof(t), 0);

    uint32_t tc = COL_TEXT;
    if (tm_finished) {
        // flash between warn color and dim while unacknowledged
        tc = ((now_ms() / 400) & 1) ? COL_WARN : COL_TEXT_DIM;
    } else if (tm_running && rem < 10000) {
        tc = COL_WARN;
    }
    draw_big_time(ct + 26, t, tc);

    const char *sub = tm_finished ? "Time's up!  (any key dismisses)"
                     : tm_running ? "Counting down"
                     : "Set a duration and press Start";
    draw_text_sz(g_window, PAD, ct + 4, sub, 12,
                 tm_finished ? COL_WARN : COL_TEXT_DIM);

    // progress bar: share of configured time remaining
    int pct = (tm_set > 0) ? (int)((rem * 100) / tm_set) : 0;
    gui_progress(g_window, PAD, ct + 92, WIN_W - 2 * PAD, 10, pct);

    draw_text_sz(g_window, PAD, ct + 110, "Presets", 12, COL_TEXT_DIM);
}

static void draw_pomodoro(void) {
    int ct = content_top();
    static const char *PHASE[3] = { "Focus", "Short break", "Long break" };
    uint32_t pc = po_phase == 0 ? COL_ACCENT : COL_TEXT2;
    gui_text_ttf_centered(g_window, 0, ct + 2, WIN_W, 22, PHASE[po_phase], pc, 17);

    char t[24];
    fmt_hms(po_remaining(), t, sizeof(t), 0);
    draw_big_time(ct + 30, t, COL_TEXT);

    ms_t len = po_phase_len(po_phase);
    ms_t rem = po_remaining();
    int pct = len > 0 ? (int)(((len - rem) * 100) / len) : 0;
    gui_progress(g_window, PAD, ct + 96, WIN_W - 2 * PAD, 10, pct);

    // cycle dots: 4 focus sessions per long-break cycle
    int cyc = po_done % 4;
    int dots_w = 4 * 14 + 3 * 10;
    int dx = (WIN_W - dots_w) / 2;
    for (int i = 0; i < 4; i++) {
        uint32_t c = (i < cyc || (i == cyc && po_phase == 0 && po_running))
                     ? COL_ACCENT : COL_SEP;
        gui_fill_circle_aa(g_window, dx + i * 24, ct + 120, 14, c, COL_BG);
    }

    char stats[64];
    snprintf(stats, sizeof(stats), "Focus sessions completed: %d", po_done);
    gui_text_ttf_centered(g_window, 0, ct + 148, WIN_W, 18, stats, COL_TEXT2, 12);
    gui_text_ttf_centered(g_window, 0, ct + 170, WIN_W, 16,
                          "25 min focus, 5 min break, 15 min every 4th",
                          COL_TEXT_DIM, 11);
}

static void draw_all(void) {
    // re-sync live content size from the compositor (devmgr/taskmanager idiom)
    { int w = g_win_w, h = g_win_h;
      win_get_size(g_window, &w, &h);
      if (w >= MIN_W) g_win_w = w;
      if (h >= MIN_H) g_win_h = h;
    }
    build_buttons();
    win_draw_rect(g_window, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_tabs();
    if (g_tab == 0) draw_stopwatch();
    else if (g_tab == 1) draw_timer();
    else draw_pomodoro();
    draw_buttons();
    win_invalidate(g_window);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void sw_startpause(void) {
    if (sw_running) { sw_accum += now_ms() - sw_start; sw_running = 0; }
    else            { sw_start = now_ms(); sw_running = 1; }
}
static void sw_lap(void) {
    if (!sw_running || sw_lap_count >= MAX_LAPS) return;
    sw_laps[sw_lap_count++] = sw_elapsed();
}
static void sw_reset(void) {
    sw_running = 0; sw_accum = 0; sw_lap_count = 0;
}

static void tm_apply_set(ms_t ms) {
    if (ms < 10000) ms = 10000;
    if (ms > 99LL * 3600000) ms = 99LL * 3600000;
    tm_set = ms; tm_remain = ms; tm_running = 0; tm_finished = 0;
}
static void tm_adjust(ms_t delta) {
    if (tm_running) {
        tm_deadline += delta;
        if (tm_deadline < now_ms()) tm_deadline = now_ms();
        if (tm_set + delta > 0) tm_set += delta;
    } else {
        ms_t r = tm_remain + delta;
        if (r < 0) r = 0;
        tm_remain = r;
        if (tm_set < r) tm_set = r;
        tm_finished = 0;
    }
}
static void tm_startpause(void) {
    if (tm_finished) { tm_finished = 0; return; }
    if (tm_running) {
        tm_remain = tm_remaining();
        tm_running = 0;
    } else if (tm_remain > 0) {
        tm_deadline = now_ms() + tm_remain;
        tm_running = 1;
    }
}
static void tm_reset(void) {
    tm_running = 0; tm_finished = 0; tm_remain = tm_set;
}

static void po_enter_phase(int ph, int keep_running) {
    po_phase = ph;
    po_remain = po_phase_len(ph);
    if (keep_running) { po_deadline = now_ms() + po_remain; po_running = 1; }
    else po_running = 0;
}
static void po_advance(int notify) {
    if (po_phase == 0) {
        po_done++;
        int longb = (po_done % 4) == 0;
        if (notify)
            notify_post("Pomodoro", longb ? "Focus done. Take a long break."
                                          : "Focus done. Take a short break.",
                        NOTIFY_SUCCESS);
        po_enter_phase(longb ? 2 : 1, po_running);
    } else {
        if (notify)
            notify_post("Pomodoro", "Break over. Back to focus.", NOTIFY_INFO);
        po_enter_phase(0, po_running);
    }
}
static void po_startpause(void) {
    if (po_running) { po_remain = po_remaining(); po_running = 0; }
    else { po_deadline = now_ms() + po_remain; po_running = 1; }
}
static void po_reset(void) {
    po_running = 0; po_phase = 0; po_remain = PO_FOCUS_MS; po_done = 0;
}

static void do_button(int id) {
    switch (id) {
        case B_SW_STARTPAUSE: sw_startpause(); break;
        case B_SW_LAP:        sw_lap(); break;
        case B_SW_RESET:      sw_reset(); break;
        case B_TM_P1:  tm_apply_set(1  * 60000LL); break;
        case B_TM_P3:  tm_apply_set(3  * 60000LL); break;
        case B_TM_P5:  tm_apply_set(5  * 60000LL); break;
        case B_TM_P10: tm_apply_set(10 * 60000LL); break;
        case B_TM_P25: tm_apply_set(25 * 60000LL); break;
        case B_TM_P60: tm_apply_set(60 * 60000LL); break;
        case B_TM_M1M:  tm_adjust(-60000); break;
        case B_TM_M10S: tm_adjust(-10000); break;
        case B_TM_A10S: tm_adjust( 10000); break;
        case B_TM_A1M:  tm_adjust( 60000); break;
        case B_TM_STARTPAUSE: tm_startpause(); break;
        case B_TM_RESET:      tm_reset(); break;
        case B_PO_STARTPAUSE: po_startpause(); break;
        case B_PO_SKIP:       po_advance(0); break;
        case B_PO_RESET:      po_reset(); break;
        default: break;
    }
}

// Called on every loop iteration: fire expirations.
static void tick(void) {
    if (tm_running && tm_remaining() == 0) {
        tm_running = 0;
        tm_remain = 0;
        tm_finished = 1;
        char body[48];
        fmt_hms(tm_set, body, sizeof(body), 0);
        char msg[80];
        snprintf(msg, sizeof(msg), "%s countdown finished", body);
        notify_post("Timer", msg, NOTIFY_INFO);
    }
    if (po_running && po_remaining() == 0)
        po_advance(1);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static int hit_button(int mx, int my) {
    for (int i = 0; i < g_nbtns; i++) {
        btn_t *b = &g_btns[i];
        if (mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h)
            return i;
    }
    return -1;
}
static int hit_tab(int mx, int my) {
    if (my < TAB_Y || my >= TAB_Y + TAB_H) return -1;
    int tw = (WIN_W - 2 * PAD - 2 * 6) / 3;
    for (int i = 0; i < 3; i++) {
        int x = PAD + i * (tw + 6);
        if (mx >= x && mx < x + tw) return i;
    }
    return -1;
}

static void on_key(gui_event_t *ev) {
    char c = ev->key_char;
    if (tm_finished && g_tab == 1) { tm_finished = 0; return; }  // any key dismisses
    if (c == '1' || c == '2' || c == '3') { g_tab = c - '1'; g_hover = -1; return; }
    // #191: these were 0x4B/0x4D, PS/2 scancodes the kernel never delivers, so
    // arrow tab-switching had never worked; 0x4B/0x4D are ASCII 'K'/'M', so
    // shift-K and shift-M switched tabs instead.
    if (ev->keycode == GUI_KEY_LEFT)  { g_tab = (g_tab + 2) % 3; g_hover = -1; return; }
    if (ev->keycode == GUI_KEY_RIGHT) { g_tab = (g_tab + 1) % 3; g_hover = -1; return; }
    if (c == ' ') {
        if (g_tab == 0) sw_startpause();
        else if (g_tab == 1) tm_startpause();
        else po_startpause();
        return;
    }
    if ((c == 'l' || c == 'L') && g_tab == 0) { sw_lap(); return; }
    if ((c == 'n' || c == 'N') && g_tab == 2) { po_advance(0); return; }
    if (c == 'r' || c == 'R') {
        if (g_tab == 0) sw_reset();
        else if (g_tab == 1) tm_reset();
        else po_reset();
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_last_theme = get_theme();
    apply_theme(g_last_theme);
    g_window = win_create("Timers", 240, 110, WIN_W, WIN_H);
    if (g_window < 0) { printf("timers: failed to create window\n"); return 1; }

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        { int th = get_theme();
          if (th != g_last_theme) { g_last_theme = th; apply_theme(th); draw_all(); } }
        tick();
        int busy = sw_running || tm_running || po_running || tm_finished;
        int et = win_get_event(g_window, &ev, busy ? 100 : 250);
        if (et == 0) {
            if (busy) draw_all();   // live clock update
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) {
                    g_win_w = ev.mouse_x; g_win_h = ev.mouse_y;
                    if (g_win_w < MIN_W) g_win_w = MIN_W;
                    if (g_win_h < MIN_H) g_win_h = MIN_H;
                }
                draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN: on_key(&ev); draw_all(); break;
            case EVENT_MOUSE_MOVE: {
                int nb = hit_button(ev.mouse_x, ev.mouse_y);
                int nt = hit_tab(ev.mouse_x, ev.mouse_y);
                if (nb != g_hover || nt != g_hover_tab) {
                    g_hover = nb; g_hover_tab = nt; draw_all();
                }
                break;
            }
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    if (tm_finished && g_tab == 1) { tm_finished = 0; draw_all(); break; }
                    int t = hit_tab(ev.mouse_x, ev.mouse_y);
                    if (t >= 0) { g_tab = t; g_hover = -1; draw_all(); break; }
                    int bi = hit_button(ev.mouse_x, ev.mouse_y);
                    if (bi >= 0 && g_btns[bi].enabled) {
                        do_button(g_btns[bi].id);
                        draw_all();
                    }
                }
                break;
            default: break;
        }
    }
    win_destroy(g_window);
    return 0;
}
