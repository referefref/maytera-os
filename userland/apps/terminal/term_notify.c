// term_notify.c
// PHASE 1 (terminal uplift): Konsole-class terminal NOTIFICATIONS.
// See term_notify.h for the rules this file encodes and why they are the way
// they are. In one line: toasts are expensive and fire only when the user
// cannot already see the answer; marks are cheap and fire whenever an
// unattended tab wants attention.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_theme.h"
#include "term_render.h"
#include "term_notify.h"
#include "../../libc/notify.h"

// ---------------------------------------------------------------------------
// Configuration. Defaults are Konsole's, and the file is CREATED on first run
// (same reason term_prefs_load() creates TERMPREF.CFG: "the default is dark"
// must be a fact on disk, not a fact about a compiled-in constant that stops
// being true the moment anything else writes the file).
//
// This is a SECOND FILE, not a second config SYSTEM: same flat key=value
// convention, same userconf_* path resolution, same throttled re-read as
// TERMPREF.CFG. It is separate only because term_prefs_save() rewrites
// TERMPREF.CFG wholesale from the five fields it knows about, so any key this
// module added there would be silently erased the next time the user pressed
// F9. Fold these into TERMPREF.CFG the day its writer learns about them.
// ---------------------------------------------------------------------------
static int g_cfg_enable                 = 1;      // master switch for TOASTS (marks are unaffected)
static int g_cfg_finish_ms              = 10000;  // a command must run at least this long to be worth a toast; 0 = never
static int g_cfg_finish_unattended_only = 1;      // ... and the window must not be attended
static int g_cfg_bell_notify            = 1;      // BEL toasts (only when unattended)
static int g_cfg_visual_bell            = 1;      // BEL always flashes, attended or not
static int g_cfg_visual_bell_ms         = 400;
static int g_cfg_activity               = 0;      // Konsole default: OFF
static int g_cfg_silence                = 0;      // Konsole default: OFF
static int g_cfg_silence_ms             = 10000;  // Konsole default: 10s

#define BELL_TOAST_MIN_GAP_MS  5000   // a program in a bell loop must not become a toast loop

typedef struct {
    int           in_use;
    unsigned long start_ms;        // when the current foreground child began; 0 = none running
    char          prog[40];        // its basename
    unsigned long last_out_ms;     // last time output landed in this tab
    int           had_output;      // this tab has produced output at least once while unattended
    int           mark;            // TERM_MARK_*
    int           activity_fired;  // activity toast already sent for this idle->active transition
    int           silence_fired;
    unsigned long last_bell_ms;    // rate limit for bell toasts
    int           mon[TERM_MON_COUNT];
} tn_tab_t;

static tn_tab_t g_tab[TERM_NOTIFY_MAX_TABS];
static int  g_focused_tab = TERM_TAB_DEFAULT;

static int  g_win_state = -1;              // cached WIN_STATE_* bitmask
static unsigned long g_win_poll_ms = 0;
static unsigned long g_flash_until_ms = 0; // visual bell deadline
static int  g_flash_drawn = 0;             // was the frame painted on the last redraw
static int  g_flash_pane  = -1;            // pane that rang; -1 = "not pane-scoped"
static int  g_cur_pane    = -1;            // pane the module globals describe now
static unsigned int  g_cfg_hash = 0;
static int  g_cfg_have_baseline = 0;
static unsigned long g_cfg_poll_ms = 0;
static int  g_inited = 0;

static tn_tab_t *tab_of(int tab) {
    if (tab < 0 || tab >= TERM_NOTIFY_MAX_TABS) tab = TERM_TAB_DEFAULT;
    return &g_tab[tab];
}

// ---------------------------------------------------------------------------
// Window attention state.
//
// THIS IS THE PART THAT DID NOT EXIST. Before this pass a MayteraOS app could
// not discover that it was minimized, and could not discover that it was
// unfocused either: the kernel emits no focus/blur/minimize event to an app
// (EVENT_WINDOW_FOCUS and EVENT_WINDOW_BLUR appear exactly once each in the
// whole kernel tree and it is the enum declaration - kernel/proc/syscall.h
// says so in its own words), and although sys_wm_get_windows() reports a
// `minimized` flag per window, an app cannot pick ITS OWN row out of that
// list: win_create() returns the user_windows[] SLOT INDEX, while
// wm_window_info_t.id is the window manager's window id, and nothing maps one
// to the other. So the flag was visible to everybody except the window's own
// process.
//
// SYS_WIN_GET_STATE (kernel/proc/syscall.c) closes exactly that gap and
// nothing wider: given the caller's OWN window handle, it returns that one
// window's WINDOW_FLAG_* bits mapped onto the stable WIN_STATE_* bitmask.
// It reads state the kernel already maintains, takes no pointer, cannot
// block, and tells a caller nothing about anyone else's windows.
// ---------------------------------------------------------------------------
int term_notify_window_state(void) { return g_win_state; }

int term_notify_window_attended(void) {
    if (g_win_state < 0) return 1;   // unknown: assume attended, i.e. stay QUIET
    if (g_win_state & WIN_STATE_MINIMIZED) return 0;
    if (!(g_win_state & WIN_STATE_VISIBLE)) return 0;
    if (!(g_win_state & WIN_STATE_FOCUSED)) return 0;
    return 1;
}

int term_notify_window_minimized(void) {
    if (g_win_state < 0) return 0;
    return (g_win_state & WIN_STATE_MINIMIZED) ? 1 : 0;
}

// A tab is unattended if its window is unattended OR it is not the tab on
// screen. With one tab the second half is always false, so this reduces to
// the window test; when tabs land it becomes the Konsole rule unchanged.
static int tab_attended(int tab) {
    if (!term_notify_window_attended()) return 0;
    return (tab == g_focused_tab);
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------
static void cfg_defaults_text(char *buf, int cap) {
    snprintf(buf, cap,
             "enable=%d\nfinish_ms=%d\nfinish_unattended_only=%d\n"
             "bell_notify=%d\nvisual_bell=%d\nvisual_bell_ms=%d\n"
             "activity=%d\nsilence=%d\nsilence_ms=%d\n",
             g_cfg_enable, g_cfg_finish_ms, g_cfg_finish_unattended_only,
             g_cfg_bell_notify, g_cfg_visual_bell, g_cfg_visual_bell_ms,
             g_cfg_activity, g_cfg_silence, g_cfg_silence_ms);
}

static void cfg_apply_line(const char *k, const char *v) {
    int n = atoi(v);
    if      (str_eq(k, "enable"))                 g_cfg_enable = n ? 1 : 0;
    else if (str_eq(k, "finish_ms"))              g_cfg_finish_ms = (n < 0) ? 0 : n;
    else if (str_eq(k, "finish_unattended_only")) g_cfg_finish_unattended_only = n ? 1 : 0;
    else if (str_eq(k, "bell_notify"))            g_cfg_bell_notify = n ? 1 : 0;
    else if (str_eq(k, "visual_bell"))            g_cfg_visual_bell = n ? 1 : 0;
    else if (str_eq(k, "visual_bell_ms"))         g_cfg_visual_bell_ms = (n < 0) ? 0 : (n > 5000 ? 5000 : n);
    else if (str_eq(k, "activity"))               g_cfg_activity = n ? 1 : 0;
    else if (str_eq(k, "silence"))                g_cfg_silence = n ? 1 : 0;
    else if (str_eq(k, "silence_ms"))             g_cfg_silence_ms = (n < 1000) ? 1000 : n;
}

// Push the file's values onto every tab's monitor switches.
//
// THIS FUNCTION IS THE FIX FOR A MEASURED DEAD KNOB (verified on golden 2046).
// `activity=1` in TERMNOTI.CFG did NOTHING: term_notify_output() gates the
// toast on BOTH the global g_cfg_activity AND the per-tab
// mon[TERM_MON_ACTIVITY], and the per-tab switch was initialised to 0 with no
// code path anywhere that could ever set it (the View menu that will own it
// does not exist yet). Two switches for one decision, one of them
// unreachable, is not "defence in depth", it is a knob that lies. Silence had
// the identical shape. Bell and Finish did not, only because their per-tab
// defaults happen to be 1 - so the bug was invisible in exactly the two cases
// that were tested first.
//
// SEMANTICS, stated because they matter once the View menu lands: the FILE is
// the default and the per-tab switch is the override. Re-applying on every
// CHANGE of the file (not on every poll) means a hand-edit or a future
// preferences page reaches open tabs, while a per-tab toggle made from a menu
// survives until the file itself changes.
static void cfg_apply_to_tabs(void) {
    for (int i = 0; i < TERM_NOTIFY_MAX_TABS; i++) {
        g_tab[i].mon[TERM_MON_FINISH]   = (g_cfg_finish_ms > 0) ? 1 : 0;
        g_tab[i].mon[TERM_MON_BELL]     = g_cfg_bell_notify;
        g_tab[i].mon[TERM_MON_ACTIVITY] = g_cfg_activity;
        g_tab[i].mon[TERM_MON_SILENCE]  = g_cfg_silence;
    }
}

static void cfg_parse(char *buf) {
    char *line = buf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n' && *end != '\r') end++;
        char saved = *end;
        *end = '\0';
        if (*line && *line != '#') {
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') { *eq = '\0'; cfg_apply_line(line, eq + 1); }
        }
        if (!saved) break;
        line = end + 1;
    }
}

// Read TERMNOTI.CFG if it changed. Returns 1 if the file was applied.
// Throttled to 2s; the read itself is an ordinary non-blocking file read.
static int cfg_poll(int force) {
    unsigned long now = uptime_ms();
    if (!force && g_cfg_have_baseline && (now - g_cfg_poll_ms) < 2000) return 0;
    g_cfg_poll_ms = now;

    int fd = userconf_open_read("TERMNOTI.CFG", TERM_NOTIFY_CFG);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    unsigned int h = 0;
    for (int i = 0; buf[i]; i++) h = h * 131 + (unsigned char)buf[i];
    if (g_cfg_have_baseline && h == g_cfg_hash) return 0;
    g_cfg_hash = h;
    g_cfg_have_baseline = 1;
    cfg_parse(buf);
    cfg_apply_to_tabs();
    return 1;
}

void term_notify_init(void) {
    if (g_inited) return;
    g_inited = 1;
    cfg_apply_to_tabs();          // from the compiled defaults, before any file
    g_tab[TERM_TAB_DEFAULT].in_use = 1;

    // Create the file on a virgin image so the knobs are discoverable, then
    // load it. If it already exists, the load below is the only thing that
    // runs and the user's values win.
    int fd = userconf_open_read("TERMNOTI.CFG", TERM_NOTIFY_CFG);
    if (fd < 0) {
        char def[512];
        cfg_defaults_text(def, sizeof(def));
        int wfd = userconf_open_write("TERMNOTI.CFG");
        // Checked, not discarded (#743): a seed write that silently fails is
        // how a "default" stops being a fact on disk. Nothing user-visible
        // happens on failure though - the compiled defaults above are already
        // in effect and are exactly what the file would have contained - so
        // this deliberately does NOT toast about its own config file.
        if (userconf_finish_write(wfd, def, strlen(def)) != 0) {
            /* keep compiled defaults; retried on the next terminal launch */
        }
    } else {
        close(fd);
    }
    cfg_poll(1);
    // Prime the window-state cache immediately: a terminal launched into a
    // minimized state (or behind another window) must not spend its first
    // 250ms believing it is attended.
    g_win_state = win_get_state(window_handle);
    g_win_poll_ms = uptime_ms();
}

// ---------------------------------------------------------------------------
// Toast helpers. EVERY toast in this file goes through here, so there is one
// place that knows the title format and one place a rate limit could go.
// ---------------------------------------------------------------------------
static void tn_toast(int tab, const char *body, int severity) {
    if (!g_cfg_enable) return;
    tn_tab_t *t = tab_of(tab);
    char title[64];
    if (t->prog[0]) snprintf(title, sizeof(title), "Terminal: %s", t->prog);
    else            snprintf(title, sizeof(title), "Terminal");
    notify_post(title, body, severity);
}

static void mark_set(int tab, int mark) {
    tn_tab_t *t = tab_of(tab);
    if (mark > t->mark) t->mark = mark;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void term_notify_cmd_start(int tab, const char *program) {
    tn_tab_t *t = tab_of(tab);
    t->in_use = 1;
    t->start_ms = uptime_ms();
    if (t->start_ms == 0) t->start_ms = 1;   // 0 is the "nothing running" sentinel
    // basename only: "/APPS/VI" -> "VI"
    const char *base = program ? program : "";
    for (const char *q = base; *q; q++) if (*q == '/') base = q + 1;
    int i = 0;
    while (base[i] && i < (int)sizeof(t->prog) - 1) { t->prog[i] = base[i]; i++; }
    t->prog[i] = '\0';
    t->activity_fired = 0;
    t->silence_fired = 0;
    t->had_output = 0;
}

void term_notify_cmd_end(int tab, int exit_status) {
    tn_tab_t *t = tab_of(tab);
    unsigned long now = uptime_ms();
    unsigned long dur = (t->start_ms && now >= t->start_ms) ? (now - t->start_ms) : 0;
    t->start_ms = 0;

    // THE RULE, and why it is this and not something more eager:
    //   fire when   duration >= finish_ms (default 10s)
    //         AND   the tab was not being watched at the moment it finished.
    // Every `ls`, `cd`, `cat` and failed typo finishes in well under 10s, so
    // none of them toast, which is the whole point: the boot warning that
    // always fires is the warning nobody reads. A command long enough to walk
    // away from is exactly the one worth a toast, and if the user did NOT
    // walk away they watched it finish and do not need telling.
    // The failure case is deliberately NOT given a lower threshold: a command
    // that failed in 200ms failed in front of the user.
    if (!g_cfg_enable || g_cfg_finish_ms <= 0) goto done;
    if (!t->mon[TERM_MON_FINISH]) goto done;
    if (dur < (unsigned long)g_cfg_finish_ms) goto done;
    if (g_cfg_finish_unattended_only && tab_attended(tab)) goto done;
    {
        char body[128];
        int secs = (int)(dur / 1000);
        if (exit_status == 0)
            snprintf(body, sizeof(body), "%s finished after %ds",
                     t->prog[0] ? t->prog : "command", secs);
        else
            snprintf(body, sizeof(body), "%s exited (code %d) after %ds",
                     t->prog[0] ? t->prog : "command", exit_status, secs);
        tn_toast(tab, body, exit_status == 0 ? NOTIFY_SUCCESS : NOTIFY_ERROR);
        mark_set(tab, TERM_MARK_ACTIVITY);
    }
done:
    (void)0;
}

// See term_notify.h. tl_activate() is the ONE place that says which pane the
// module globals describe, so it is the one place that tells us too.
void term_notify_note_pane(int pane) { g_cur_pane = pane; }

void term_notify_bell(int tab) {
    tn_tab_t *t = tab_of(tab);
    unsigned long now = uptime_ms();

    // The visual bell fires ALWAYS, attended or not. That is the half of this
    // the brief calls out: before this pass a BEL byte fell off the end of
    // term_putc()'s if-chain (0x07 is below ' ', so the printable branch
    // rejected it) and was a SILENT DROP. There is no PC speaker path in
    // reach of a Ring-3 app here, so the visible flash IS the bell.
    if (g_cfg_visual_bell && g_cfg_visual_bell_ms > 0) {
        g_flash_until_ms = now + (unsigned long)g_cfg_visual_bell_ms;
        // The pump activates the ringing pane before draining its pty, so the
        // currently-active pane IS the one that rang.
        g_flash_pane = g_cur_pane;
    }

    if (tab_attended(tab)) return;   // on screen and focused: the flash was enough
    mark_set(tab, TERM_MARK_BELL);
    if (!g_cfg_bell_notify || !t->mon[TERM_MON_BELL]) return;
    if (t->last_bell_ms && (now - t->last_bell_ms) < BELL_TOAST_MIN_GAP_MS) return;
    t->last_bell_ms = now;
    tn_toast(tab, term_notify_window_minimized()
                    ? "Bell (window minimized)" : "Bell", NOTIFY_INFO);
}

void term_notify_output(int tab) {
    tn_tab_t *t = tab_of(tab);
    unsigned long now = uptime_ms();
    t->last_out_ms = now;
    if (tab_attended(tab)) {
        // Output the user is watching resets the activity/silence machine, so
        // the next time they look away the first output starts a fresh cycle.
        t->activity_fired = 0;
        t->silence_fired = 0;
        t->had_output = 0;
        return;
    }
    t->had_output = 1;
    mark_set(tab, TERM_MARK_ACTIVITY);
    if (t->activity_fired) return;      // ONCE per idle->active transition, never per line
    t->activity_fired = 1;
    t->silence_fired = 0;
    if (!g_cfg_activity || !t->mon[TERM_MON_ACTIVITY]) return;
    tn_toast(tab, "Activity in a background tab", NOTIFY_INFO);
}

// ---------------------------------------------------------------------------
// Tab wiring
// ---------------------------------------------------------------------------
void term_notify_set_focused_tab(int tab) {
    if (tab < 0 || tab >= TERM_NOTIFY_MAX_TABS) return;
    g_focused_tab = tab;
    term_notify_tab_clear(tab);
}
int term_notify_focused_tab(void) { return g_focused_tab; }
int term_notify_tab_mark(int tab) { return tab_of(tab)->mark; }

void term_notify_tab_clear(int tab) {
    tn_tab_t *t = tab_of(tab);
    t->mark = TERM_MARK_NONE;
    t->activity_fired = 0;
    t->silence_fired = 0;
    t->had_output = 0;
}

int term_notify_any_mark(void) {
    for (int i = 0; i < TERM_NOTIFY_MAX_TABS; i++)
        if (g_tab[i].mark != TERM_MARK_NONE) return 1;
    return 0;
}

void term_notify_set_monitor(int tab, int monitor, int on) {
    if (monitor < 0 || monitor >= TERM_MON_COUNT) return;
    tab_of(tab)->mon[monitor] = on ? 1 : 0;
}
int term_notify_get_monitor(int tab, int monitor) {
    if (monitor < 0 || monitor >= TERM_MON_COUNT) return 0;
    return tab_of(tab)->mon[monitor];
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
int term_notify_tick(void) {
    if (!g_inited) return 0;
    int repaint = 0;
    unsigned long now = uptime_ms();

    cfg_poll(0);

    // Window state, throttled to 4Hz. win_get_state() is a pure read of flags
    // the kernel already holds; it takes no pointer and cannot sleep.
    if ((now - g_win_poll_ms) >= 250) {
        g_win_poll_ms = now;
        int prev_attended = term_notify_window_attended();
        int st = win_get_state(window_handle);
        if (st >= 0) g_win_state = st;
        int nowatt = term_notify_window_attended();
        if (nowatt && !prev_attended) {
            // The user came back. Everything the terminal was holding up as
            // "you were away" is now answered by them looking at it.
            if (g_tab[g_focused_tab].mark != TERM_MARK_NONE) repaint = 1;
            term_notify_tab_clear(g_focused_tab);
        }
    }

    // Monitor for silence: output stopped for silence_ms in an unattended tab
    // that HAD been producing output. Fires once, and only after activity has
    // already been seen, so a permanently idle background shell is silent.
    if (g_cfg_silence) {
        for (int i = 0; i < TERM_NOTIFY_MAX_TABS; i++) {
            tn_tab_t *t = &g_tab[i];
            if (!t->in_use || !t->had_output || t->silence_fired) continue;
            if (tab_attended(i)) continue;
            if (!t->mon[TERM_MON_SILENCE]) continue;
            if ((now - t->last_out_ms) < (unsigned long)g_cfg_silence_ms) continue;
            t->silence_fired = 1;
            mark_set(i, TERM_MARK_SILENCE);
            repaint = 1;
            tn_toast(i, "Silence in a background tab", NOTIFY_INFO);
        }
    }

    // The visual bell has to be un-drawn when it expires, and drawn when it
    // starts; both are a repaint, and neither involves waiting for anything.
    {
        int want = (g_flash_until_ms > now) ? 1 : 0;
        if (want != g_flash_drawn) repaint = 1;
    }
    return repaint;
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
// A small filled disc, built from horizontal runs. There is no circle
// primitive in the userland app toolkit (draw.c's belongs to the compositor
// process and is not linkable here), and 8 win_draw_rect() calls that only
// happen while a mark is pending is not worth a new shared widget.
static void pip_disc(int x, int y, uint32_t col) {
    win_draw_rect(window_handle, x + 2, y + 0, 4, 1, col);
    win_draw_rect(window_handle, x + 1, y + 1, 6, 1, col);
    win_draw_rect(window_handle, x + 0, y + 2, 8, 4, col);
    win_draw_rect(window_handle, x + 1, y + 6, 6, 1, col);
    win_draw_rect(window_handle, x + 2, y + 7, 4, 1, col);
}

void term_notify_paint_overlay(void) {
    if (!g_inited) return;
    unsigned long now = uptime_ms();

    // --- visual bell: a 3px frame just inside the content edge -------------
    // A frame rather than an inverted screen: it is unmistakable, it costs
    // four rects, and it does not destroy a single character of the output the
    // user is presumably reading when the program rings.
    g_flash_drawn = (g_flash_until_ms > now) ? 1 : 0;
    if (!g_flash_drawn) g_flash_pane = -1;   // no stale pane id outlives a flash
    // term_notify_paint_overlay() runs once PER PANE (tl_draw_pane activates a
    // pane then redraws it), so without the pane test every pane would flash.
    if (g_flash_drawn && (g_flash_pane < 0 || g_cur_pane == g_flash_pane)) {
        // ansi_colors[11] is the ACTIVE COLOUR SCHEME's bright yellow, so the
        // bell reads as part of whichever scheme the user picked instead of
        // introducing a bespoke constant (docs/UI_STYLE_GUIDE.md).
        uint32_t bc = ansi_colors[11];
        // term_origin_x/y ALREADY include the menu-bar band, the tab strip and
        // the per-pane header (tl_relayout -> tl_layout_leaf). Recomputing any
        // of that here would double-count it; this is the same pairing every
        // other correct consumer uses (term_render.c, term_select.c).
        int x = term_origin_x, y = term_origin_y;
        int w = term_px_w, h = term_px_h;
        win_draw_rect(window_handle, x,         y,         w, 3, bc);
        win_draw_rect(window_handle, x,         y + h - 3, w, 3, bc);
        win_draw_rect(window_handle, x,         y,         3, h, bc);
        win_draw_rect(window_handle, x + w - 3, y,         3, h, bc);
    }

    // --- attention pip -----------------------------------------------------
    // WHERE THIS GOES ONCE TABS EXIST: nowhere. The tab strip draws a dot per
    // tab from term_notify_tab_mark() and this fallback comes out. Until then
    // there is no strip to draw in, and a mark the user cannot see is not a
    // notification, so it is drawn in the scrollbar gutter (GUI_SCROLL_W px
    // that term_handle_resize() already reserves and text never occupies).
    // It can overlap the scrollbar's top arrow while scrollback is long; that
    // is a deliberate trade, it is transient, and it clears the moment the
    // window is looked at again.
    int mark = TERM_MARK_NONE;
    for (int i = 0; i < TERM_NOTIFY_MAX_TABS; i++)
        if (g_tab[i].mark > mark) mark = g_tab[i].mark;
    if (mark != TERM_MARK_NONE) {
        uint32_t col;
        if (mark == TERM_MARK_BELL)         col = ansi_colors[11];  // bright yellow
        else if (mark == TERM_MARK_SILENCE) col = ansi_colors[8];   // bright black / grey
        else                                col = ansi_colors[10];  // bright green
        // Same mixed-coordinate-space bug as the frame above: pane WIDTH at
        // the WINDOW origin. Pair the size with the matching origin.
        int px = term_origin_x + term_px_w - GUI_SCROLL_W + 3;
        if (px < 0) px = 0;
        pip_disc(px, term_origin_y + 3, col);
    }
}
