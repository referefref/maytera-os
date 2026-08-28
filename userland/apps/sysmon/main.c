// sysmon - MayteraOS System Monitor (#349)
//
// A deeper monitoring tool than the basic Task Manager: live history graphs
// for total CPU, RAM and network throughput, per-core CPU load bars, and a
// sortable process table with signal-based task control (SIGTERM / SIGKILL)
// plus editable scheduling priority. Every value on screen comes from a real
// kernel syscall:
//   SYS_PROC_LIST         process snapshot (pid/ppid/name/state/mem/ticks/core)
//   SYS_GET_CPU_USAGE     total CPU %
//   SYS_GET_CPU_PER_CORE  per-core CPU % ([0]=count, [1..]=pct)
//   SYS_GET_MEM_INFO      physical memory total/used
//   SYS_GET_NET_BYTES     cumulative NIC byte counter (delta -> KB/s)
//   SYS_SYSINFO           CPU brand string for the header
//   SYS_UPTIME_MS         monotonic sample clock
//   SYS_KILL              End Task (SIGTERM) / Force Kill (SIGKILL)
//   SYS_SETPRIORITY       set process priority (PRIO_LOW/NORMAL/HIGH)
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/devinfo.h"
#include "../../libc/stdio.h"
#include "../../libc/signal.h"
#include "../../libc/proccpu.h"   /* #178: the ONE CPU ranking, shared with taskmgr and top */

#define WIN_W  760
#define WIN_H  560
#define PAD    12
#define ROW_H  22
#define HIST   120         // history samples kept per graph
#define MAXPROC 64
#define MAXCORE 64

// SYS_SETPRIORITY (kernel proc/syscall.h) is implemented in the kernel but has
// no libc wrapper yet; define it locally the same way devinfo.h does.
#ifndef SYS_SETPRIORITY
#define SYS_SETPRIORITY 244
#endif
#define PRIO_LOW     1
#define PRIO_NORMAL  2
#define PRIO_HIGH    3
static inline int sys_setpriority(int pid, int level) {
    return (int)syscall2(SYS_SETPRIORITY, (long)pid, (long)level);
}

static int win = -1, DW = WIN_W, DH = WIN_H;

// ---- sampled state ----------------------------------------------------------
static devinfo_sysinfo_t g_sys;
static int g_sys_ok = 0;

/* #178: MAXPROC is now PROCCPU_MAX, the one bound, which is the kernel's
   MAX_PROCESSES. This app used to pick its own; picking your own is how a row
   ends up with no baseline slot. */
_Static_assert(MAXPROC == PROCCPU_MAX, "sysmon's snapshot bound must be the shared one");
static proc_info_t procs[MAXPROC];
static int nproc = 0;
static unsigned int cpu_pct[MAXPROC];
/* #178: the whole "diff two snapshots by pid" baseline, owned by libc. This
   app no longer holds any CPU accounting state of its own to get wrong. */
static proccpu_t cpu_state;

static int cpu_total = 0;
static unsigned long mem_total = 0, mem_used = 0;
static unsigned int core_pct[MAXCORE];
static int ncore = 0;

static unsigned char hist_cpu[HIST];            // 0..100
static unsigned char hist_mem[HIST];            // 0..100
static unsigned int  hist_net[HIST];            // KB/s
static unsigned long prev_net_bytes = 0;
static unsigned long prev_sample_ms = 0;
static unsigned int net_kbs = 0;

static int sel_pid = -1, scroll = 0;
static int sort_mode = 0;                        // 0=CPU 1=Mem 2=PID 3=Name
static char status_msg[96];
static unsigned long status_ms = 0;

// ---- palette (Settings/Files design language) --------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_DIM, C_ACC, C_SEL, C_SELTX;

static unsigned int lum_ink(unsigned int bg) {
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    return ((r * 30 + g * 59 + b * 11) / 100) > 140 ? 0x00181818u : 0x00F0F0F0u;
}
static unsigned int dim_ink(unsigned int bg) {
    unsigned int k = lum_ink(bg);
    int ir = (k >> 16) & 255, ig = (k >> 8) & 255, ib = k & 255;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    return (((ir + br) / 2) << 16) | (((ig + bgc) / 2) << 8) | ((ib + bb) / 2);
}
static unsigned int tint(unsigned int base, unsigned int acc, int pct) {
    int br = (base >> 16) & 255, bg = (base >> 8) & 255, bb = base & 255;
    int ar = (acc >> 16) & 255, ag = (acc >> 8) & 255, ab = acc & 255;
    return ((((br * (100 - pct) + ar * pct) / 100) & 255) << 16) |
           ((((bg * (100 - pct) + ag * pct) / 100) & 255) << 8) |
           (((bb * (100 - pct) + ab * pct) / 100) & 255);
}
static void apply_style(void) {
    int tid = theme_get_active();
    gui_set_style(tid == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    unsigned int wb = theme_color(THEME_COLOR_WINDOW_BG);
    int r = (wb >> 16) & 255, g = (wb >> 8) & 255, b = wb & 255;
    int dark = ((r * 30 + g * 59 + b * 11) / 100) < 128;
    C_ACC = theme_color(THEME_COLOR_ACCENT);
    C_BG    = tint(dark ? 0x00262A30 : 0x00F5F6F8, C_ACC, 5);
    C_CARD  = tint(dark ? 0x002C313B : 0x00EDEFF3, C_ACC, 6);
    C_FIELD = dark ? 0x00333A45 : 0x00FFFFFF;
    C_BORDER= dark ? 0x003A424F : 0x00CDD3DB;
    C_INK = lum_ink(C_BG); C_DIM = dim_ink(C_BG); C_SEL = C_ACC; C_SELTX = lum_ink(C_ACC);
    gui_palette_t p;
    p.surface = C_BG; p.surface_raised = C_CARD; p.ink = C_INK; p.ink_dim = C_DIM;
    p.accent = C_ACC; p.accent_hover = gui_lighten(C_ACC, 24); p.border = C_BORDER;
    p.field_bg = C_FIELD; p.field_border = C_BORDER; p.track = tint(C_BG, C_ACC, 20);
    gui_set_palette(&p);
}

// ---- helpers -----------------------------------------------------------------
static const char *state_name(unsigned int s) {
    switch (s) { case 1: return "Ready"; case 2: return "Running"; case 3: return "Sleep";
                 case 4: return "Blocked"; case 5: return "Zombie"; default: return "-"; }
}
static void mem_str(unsigned int kb, char *b, int cap) {
    if (kb >= 1024) snprintf(b, cap, "%u.%u MB", kb / 1024, ((kb % 1024) * 10) / 1024);
    else            snprintf(b, cap, "%u KB", kb);
}
static void set_status(const char *msg) {
    snprintf(status_msg, sizeof(status_msg), "%s", msg);
    status_ms = uptime_ms();
}

// ---- sorting -----------------------------------------------------------------
// #178: deliberately NOT proccpu_sort(). That helper is CPU-then-memory only,
// and this table lets the user sort by memory, pid or name as well, so folding
// it in would delete a feature. The RANKING is shared; the column choice is
// this app's own and is not duplicated anywhere.
static int proc_before(int a, int b) {
    switch (sort_mode) {
        case 1: return procs[a].mem_kb > procs[b].mem_kb;
        case 2: return procs[a].pid < procs[b].pid;
        case 3: return strncmp(procs[a].name, procs[b].name, 32) < 0;
        default:
            if (cpu_pct[a] != cpu_pct[b]) return cpu_pct[a] > cpu_pct[b];
            return procs[a].mem_kb > procs[b].mem_kb;
    }
}
static void sort_procs(void) {
    for (int a = 0; a < nproc - 1; a++)
        for (int b = 0; b < nproc - 1 - a; b++)
            if (!proc_before(b, b + 1) && proc_before(b + 1, b)) {
                proc_info_t tp = procs[b]; procs[b] = procs[b + 1]; procs[b + 1] = tp;
                unsigned int tc = cpu_pct[b]; cpu_pct[b] = cpu_pct[b + 1]; cpu_pct[b + 1] = tc;
            }
}

// ---- sampling ----------------------------------------------------------------
static void push_hist(void) {
    for (int i = 0; i < HIST - 1; i++) {
        hist_cpu[i] = hist_cpu[i + 1];
        hist_mem[i] = hist_mem[i + 1];
        hist_net[i] = hist_net[i + 1];
    }
    hist_cpu[HIST - 1] = (unsigned char)(cpu_total < 0 ? 0 : (cpu_total > 100 ? 100 : cpu_total));
    int mp = mem_total ? (int)((unsigned long long)mem_used * 100 / mem_total) : 0;
    hist_mem[HIST - 1] = (unsigned char)(mp > 100 ? 100 : mp);
    hist_net[HIST - 1] = net_kbs;
}

static void sample(void) {
    unsigned long now = uptime_ms();

    // #178: all three #145 invariants (idle in the denominator, idle out of the
    // list, baselines matched BY PID) now live in libc/proccpu.c and nowhere
    // else. This app previously carried its own copy, complete with its own
    // bound; that copy and the Task Manager's disagreed on the bound, which is
    // how one of the two defects survived a fix aimed at both.
    nproc = sys_proc_list(procs, MAXPROC);
    if (nproc < 0) nproc = 0;
    nproc = proccpu_rank(&cpu_state, procs, nproc, cpu_pct);

    cpu_total = sys_get_cpu_usage();
    sys_get_mem_info(&mem_total, &mem_used);

    unsigned int cb[MAXCORE + 1];
    int n = sys_get_cpu_per_core(cb);
    if (n > 0) {
        ncore = (int)cb[0];
        if (ncore > MAXCORE) ncore = MAXCORE;
        for (int i = 0; i < ncore; i++) core_pct[i] = cb[1 + i];
    } else ncore = 0;

    unsigned long nb = get_net_bytes();
    unsigned long dt = (prev_sample_ms && now > prev_sample_ms) ? (now - prev_sample_ms) : 0;
    if (dt > 0 && nb >= prev_net_bytes)
        net_kbs = (unsigned int)(((unsigned long long)(nb - prev_net_bytes) * 1000ULL) / dt / 1024ULL);
    else
        net_kbs = 0;
    prev_net_bytes = nb;
    prev_sample_ms = now;

    push_hist();
    sort_procs();
}

// ---- layout ------------------------------------------------------------------
#define PERF_H 168
static int list_top(void)  { return PAD + PERF_H + 10 + 18; }   // headers row above
static int list_bot(void)  { return DH - 52; }
static int list_rows(void) { int r = (list_bot() - list_top()) / ROW_H; return r < 1 ? 1 : r; }

// ---- graph drawing -----------------------------------------------------------
static void draw_graph(int gx, int gy, int gw, int gh, const unsigned char *vals,
                       unsigned int col) {
    win_draw_rect(win, gx, gy, gw, gh, C_FIELD);
    win_draw_rect(win, gx, gy, gw, 1, C_BORDER);
    win_draw_rect(win, gx, gy + gh - 1, gw, 1, C_BORDER);
    int cols = gw / 2; if (cols > HIST) cols = HIST;
    for (int i = 0; i < cols; i++) {
        int v = vals[HIST - cols + i];
        int h = (v * (gh - 2)) / 100;
        if (h <= 0) continue;
        win_draw_rect(win, gx + gw - (cols - i) * 2, gy + gh - 1 - h, 2, h, col);
    }
}
static void draw_graph_u32(int gx, int gy, int gw, int gh, const unsigned int *vals,
                           unsigned int maxv, unsigned int col) {
    win_draw_rect(win, gx, gy, gw, gh, C_FIELD);
    win_draw_rect(win, gx, gy, gw, 1, C_BORDER);
    win_draw_rect(win, gx, gy + gh - 1, gw, 1, C_BORDER);
    if (maxv == 0) maxv = 1;
    int cols = gw / 2; if (cols > HIST) cols = HIST;
    for (int i = 0; i < cols; i++) {
        unsigned int v = vals[HIST - cols + i];
        int h = (int)((unsigned long long)v * (gh - 2) / maxv);
        if (h <= 0) continue;
        if (h > gh - 2) h = gh - 2;
        win_draw_rect(win, gx + gw - (cols - i) * 2, gy + gh - 1 - h, 2, h, col);
    }
}

// ---- drawing -----------------------------------------------------------------
static void draw(void) {
    apply_style();
    win_get_size(win, &DW, &DH);
    if (DW < 480) DW = WIN_W;
    if (DH < 320) DH = WIN_H;
    win_draw_rect(win, 0, 0, DW, DH, C_BG);
    char buf[96];

    // ---- performance card ----
    int cx = PAD, cy = PAD, cw = DW - 2 * PAD, ch = PERF_H;
    gui_card(win, cx, cy, cw, ch);
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    win_draw_text_ttf(win, cx + 12, cy + 8, "System Monitor", 15, cink);
    if (g_sys_ok) {
        snprintf(buf, sizeof(buf), "%s  -  %u cores", g_sys.cpu_brand, g_sys.cpu_count);
        const char *bp = buf; while (*bp == ' ') bp++;
        int bw = gui_ttf_width(bp, 11);
        win_draw_text_ttf(win, cx + cw - 12 - bw, cy + 10, bp, 11, cdim);
    }

    int gy = cy + 32, gh = 62;
    int half = (cw - 36) / 2;
    // CPU history (left)
    snprintf(buf, sizeof(buf), "CPU  %d%%", cpu_total);
    win_draw_text_ttf(win, cx + 12, gy - 2, buf, 11, cdim);
    draw_graph(cx + 12, gy + 14, half - 12, gh, hist_cpu, C_ACC);
    // RAM history (right)
    snprintf(buf, sizeof(buf), "RAM  %lu / %lu MB", mem_used / 1048576UL, mem_total / 1048576UL);
    win_draw_text_ttf(win, cx + 24 + half, gy - 2, buf, 11, cdim);
    draw_graph(cx + 24 + half, gy + 14, half - 12, gh, hist_mem, gui_lighten(C_ACC, 40));

    // per-core bars (left) + net sparkline (right)
    int by = gy + 14 + gh + 8, bh = 34;
    win_draw_text_ttf(win, cx + 12, by - 2, "Cores", 10, cdim);
    int bx = cx + 56;
    int nshow = ncore > 24 ? 24 : ncore;
    for (int i = 0; i < nshow; i++) {
        int v = (int)core_pct[i]; if (v > 100) v = 100;
        int h = (v * (bh - 2)) / 100;
        int x = bx + i * 12;
        if (x + 10 > cx + 12 + half - 12) break;
        win_draw_rect(win, x, by + 8, 10, bh, C_FIELD);
        if (h > 0) win_draw_rect(win, x, by + 8 + bh - h, 10, h, C_ACC);
    }
    unsigned int netmax = 16;
    for (int i = 0; i < HIST; i++) if (hist_net[i] > netmax) netmax = hist_net[i];
    snprintf(buf, sizeof(buf), "NET  %u KB/s  (peak %u)", net_kbs, netmax);
    win_draw_text_ttf(win, cx + 24 + half, by - 2, buf, 10, cdim);
    draw_graph_u32(cx + 24 + half, by + 8, half - 12, bh, hist_net, netmax, gui_lighten(C_ACC, 64));

    // ---- process table headers (clickable to change sort) ----
    int hy = PAD + PERF_H + 10;
    int cName = PAD + 10, cPid = DW - 340, cCore = DW - 278, cState = DW - 218,
        cCpu = DW - 138, cMem = DW - 90;
    win_draw_text_ttf(win, cName, hy, sort_mode == 3 ? "Name v" : "Name", 11,
                      sort_mode == 3 ? C_ACC : C_DIM);
    win_draw_text_ttf(win, cPid, hy, sort_mode == 2 ? "PID v" : "PID", 11,
                      sort_mode == 2 ? C_ACC : C_DIM);
    win_draw_text_ttf(win, cCore, hy, "Core", 11, C_DIM);
    win_draw_text_ttf(win, cState, hy, "State", 11, C_DIM);
    win_draw_text_ttf(win, cCpu, hy, sort_mode == 0 ? "CPU v" : "CPU", 11,
                      sort_mode == 0 ? C_ACC : C_DIM);
    win_draw_text_ttf(win, cMem, hy, sort_mode == 1 ? "Mem v" : "Mem", 11,
                      sort_mode == 1 ? C_ACC : C_DIM);
    win_draw_rect(win, PAD, hy + 16, DW - 2 * PAD, 1, C_BORDER);

    // ---- process rows ----
    int rows = list_rows();
    if (scroll > nproc - rows) scroll = nproc - rows;
    if (scroll < 0) scroll = 0;
    for (int rr = 0; rr < rows && (rr + scroll) < nproc; rr++) {
        int i = rr + scroll, ry = list_top() + rr * ROW_H;
        int selrow = (procs[i].pid == (unsigned)sel_pid);
        if (selrow) gui_fill_rounded_aa(win, PAD, ry, DW - 2 * PAD, ROW_H - 2, 4, C_SEL, C_BG);
        unsigned int tx = selrow ? C_SELTX : C_INK, td = selrow ? C_SELTX : C_DIM;
        win_draw_text_ttf(win, cName, ry + 3, procs[i].name, 12, tx);
        snprintf(buf, sizeof(buf), "%u", procs[i].pid);
        win_draw_text_ttf(win, cPid, ry + 3, buf, 12, tx);
        if (procs[i].running_cpu < 1) snprintf(buf, sizeof(buf), "-");
        else snprintf(buf, sizeof(buf), "AP%d", procs[i].running_cpu);
        win_draw_text_ttf(win, cCore, ry + 3, buf, 11, td);
        win_draw_text_ttf(win, cState, ry + 3, state_name(procs[i].state), 11, td);
        snprintf(buf, sizeof(buf), "%u%%", cpu_pct[i]);
        win_draw_text_ttf(win, cCpu, ry + 3, buf, 12, tx);
        mem_str(procs[i].mem_kb, buf, sizeof(buf));
        win_draw_text_ttf(win, cMem, ry + 3, buf, 11, td);
    }

    // ---- footer ----
    int fy = DH - 44;
    win_draw_rect(win, PAD, fy - 6, DW - 2 * PAD, 1, C_BORDER);
    int have_sel = sel_pid > 1;
    if (status_msg[0] && (uptime_ms() - status_ms) < 4000) {
        win_draw_text_ttf(win, PAD, fy + 8, status_msg, 11, C_ACC);
    } else if (have_sel) {
        // show which pid the action buttons target
        const char *nm = "?";
        for (int i = 0; i < nproc; i++)
            if (procs[i].pid == (unsigned)sel_pid) { nm = procs[i].name; break; }
        snprintf(buf, sizeof(buf), "PID %d  %s", sel_pid, nm);
        win_draw_text_ttf(win, PAD, fy + 8, buf, 11, C_DIM);
    } else {
        snprintf(buf, sizeof(buf), "%d processes", nproc);
        win_draw_text_ttf(win, PAD, fy + 8, buf, 11, C_DIM);
    }
    gui_state_t st = have_sel ? GUI_ST_NORMAL : GUI_ST_DISABLED;
    // priority group + kill buttons, right-aligned
    gui_button(win, DW - 496, fy, 56, 28, "Low",    GUI_BTN_SECONDARY, st);
    gui_button(win, DW - 434, fy, 56, 28, "Normal", GUI_BTN_SECONDARY, st);
    gui_button(win, DW - 372, fy, 56, 28, "High",   GUI_BTN_SECONDARY, st);
    gui_button(win, DW - 306, fy, 96, 28, "End Task",   GUI_BTN_PRIMARY, st);
    gui_button(win, DW - 204, fy, 96, 28, "Force Kill", GUI_BTN_PRIMARY, st);
    gui_button(win, DW - 102, fy, 90, 28, "Refresh",    GUI_BTN_GHOST, GUI_ST_NORMAL);

    win_invalidate(win);
}

// ---- actions -----------------------------------------------------------------
static void do_kill(int sig) {
    if (sel_pid <= 1) return;
    int r = kill(sel_pid, sig);
    char m[64];
    snprintf(m, sizeof(m), r == 0 ? "Signal %d sent to PID %d" : "kill(%d) failed for PID %d",
             sig, sel_pid);
    set_status(m);
    if (r == 0 && sig == SIGKILL) sel_pid = -1;
    sample();
}
static void do_prio(int level) {
    if (sel_pid <= 1) return;
    int r = sys_setpriority(sel_pid, level);
    char m[64];
    const char *ln = level == PRIO_LOW ? "Low" : level == PRIO_HIGH ? "High" : "Normal";
    if (r == 0) snprintf(m, sizeof(m), "PID %d priority set to %s", sel_pid, ln);
    else        snprintf(m, sizeof(m), "setpriority failed for PID %d", sel_pid);
    set_status(m);
}

// ---- main --------------------------------------------------------------------
int main(void) {
    win = win_create("System Monitor", 110, 60, WIN_W, WIN_H);
    if (win < 0) return 1;
    g_sys_ok = (sys_sysinfo(&g_sys) == 0);
    sample();
    draw();
    int running = 1;
    unsigned long last = uptime_ms();
    while (running) {
        gui_event_t ev;
        int et = win_get_event(win, &ev, 1000);
        unsigned long now = uptime_ms();
        if (now - last >= 950) { sample(); last = now; draw(); }
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_REDRAW:
            case EVENT_RESIZE:
                draw(); break;
            case EVENT_WINDOW_CLOSE:
                running = 0; break;
            case EVENT_KEY_DOWN:
                if (ev.key_char == 27) running = 0;
                else if (ev.key_char == 127) do_kill(SIGTERM);
                else if (ev.key_char == 'r' || ev.key_char == 'R') { sample(); draw(); }
                else if (ev.keycode == 0x80 || ev.keycode == 0x81) {   // up/down
                    int cur = -1;
                    for (int i = 0; i < nproc; i++)
                        if (procs[i].pid == (unsigned)sel_pid) { cur = i; break; }
                    if (ev.keycode == 0x80 && cur > 0) cur--;
                    else if (ev.keycode == 0x81 && cur < nproc - 1) cur++;
                    else if (cur < 0 && nproc > 0) cur = 0;
                    if (cur >= 0) {
                        sel_pid = (int)procs[cur].pid;
                        if (cur < scroll) scroll = cur;
                        if (cur >= scroll + list_rows()) scroll = cur - list_rows() + 1;
                    }
                    draw();
                }
                break;
            case EVENT_MOUSE_DOWN: {
                int lx = ev.mouse_x, ly = ev.mouse_y;
                int fy = DH - 44;
                if (ly >= fy && ly < fy + 28) {
                    if      (lx >= DW - 496 && lx < DW - 440) do_prio(PRIO_LOW);
                    else if (lx >= DW - 434 && lx < DW - 378) do_prio(PRIO_NORMAL);
                    else if (lx >= DW - 372 && lx < DW - 316) do_prio(PRIO_HIGH);
                    else if (lx >= DW - 306 && lx < DW - 210) do_kill(SIGTERM);
                    else if (lx >= DW - 204 && lx < DW - 108) do_kill(SIGKILL);
                    else if (lx >= DW - 102 && lx < DW - 12)  { sample(); }
                    draw();
                    break;
                }
                int hy = PAD + PERF_H + 10;
                if (ly >= hy - 2 && ly < hy + 16) {          // header click: sort
                    int cPid = DW - 340, cState = DW - 218, cCpu = DW - 138, cMem = DW - 90;
                    if      (lx >= cMem)  sort_mode = 1;
                    else if (lx >= cCpu && lx < cState) sort_mode = 0;
                    else if (lx >= cPid && lx < DW - 278) sort_mode = 2;
                    else if (lx < cPid)  sort_mode = 3;
                    sort_procs();
                    draw();
                    break;
                }
                if (ly >= list_top() && ly < list_bot()) {
                    int rr = (ly - list_top()) / ROW_H, idx = rr + scroll;
                    if (idx >= 0 && idx < nproc) { sel_pid = (int)procs[idx].pid; draw(); }
                }
                break;
            }
            case EVENT_MOUSE_SCROLL:
                scroll += (ev.scroll_delta > 0) ? 2 : -2;
                if (scroll < 0) scroll = 0;
                if (scroll > nproc - 1) scroll = nproc - 1;
                draw();
                break;
            default: break;
        }
    }
    win_destroy(win);
    return 0;
}
