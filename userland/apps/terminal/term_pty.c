// term_pty.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_grid.h"
#include "term_parse.h"
#include "term_render.h"
#include "term_prefs.h"
#include "term_emu.h"
#include "term_pty.h"
#include "term_notify.h"

// The pty master fd of the CURRENTLY running foreground child (#586's
// run_foreground_pty), so a Device Status Report (CSI n) has somewhere to
// write its reply. -1 when no foreground child owns the terminal (the
// built-in shell prompt never receives DSR queries, so this is enough).
int g_active_master_fd = -1;
// ---- #586: interactive foreground execution on a real pty ------------------
// Translate a window key event into the byte(s) a tty program expects on stdin.
// Returns the count written into out[] (0 = drop this key). Printable and
// control chars (ESC=0x1b, Tab, Ctrl-*) arrive as key_char; arrows/Enter/Backspace
// may arrive as a special keycode with key_char==0.
int key_event_to_bytes(const gui_event_t *ev, char *out) {
    char c = ev->key_char;
    uint32_t kc = ev->keycode;
    if (c != 0) {
        if (c == '\n') c = '\r';   // Enter -> CR
        out[0] = c;
        return 1;
    }
    switch (kc) {
        case GUI_KEY_UP:    out[0] = 0x1b; out[1] = '['; out[2] = 'A'; return 3;
        case GUI_KEY_DOWN:  out[0] = 0x1b; out[1] = '['; out[2] = 'B'; return 3;
        case GUI_KEY_LEFT:  out[0] = 0x1b; out[1] = '['; out[2] = 'D'; return 3;
        case GUI_KEY_RIGHT: out[0] = 0x1b; out[1] = '['; out[2] = 'C'; return 3;
        // #243 NAVIGATION / EDITING KEYS. These SIX had no case here and could
        // not have had one: they arrived as 0x47/0x4F/0x49/0x51/0x52/0x53 with
        // ev.key_char set to the colliding capital letter, so the `if (c != 0)`
        // fast path above returned that letter and the switch was never
        // reached. That is exactly how `vi /BUILDINFO.TXT` opened /BULDNF.TXT
        // and how PgUp entered INSERT mode: the pty received 'I'.
        //
        // The vt220 numeric form (ESC [ n ~) is what vi's own reader decodes -
        // see userland/apps/vi/compat/read_key.c, whose decode_csi() maps
        // 1/7 -> HOME, 2 -> INSERT, 3 -> DELETE, 4/8 -> END, 5 -> PAGEUP,
        // 6 -> PAGEDOWN. Read out of that file, not assumed.
        case GUI_KEY_HOME: out[0]=0x1b; out[1]='['; out[2]='1'; out[3]='~'; return 4;
        case GUI_KEY_INS:  out[0]=0x1b; out[1]='['; out[2]='2'; out[3]='~'; return 4;
        case GUI_KEY_DEL:  out[0]=0x1b; out[1]='['; out[2]='3'; out[3]='~'; return 4;
        case GUI_KEY_END:  out[0]=0x1b; out[1]='['; out[2]='4'; out[3]='~'; return 4;
        case GUI_KEY_PGUP: out[0]=0x1b; out[1]='['; out[2]='5'; out[3]='~'; return 4;
        case GUI_KEY_PGDN: out[0]=0x1b; out[1]='['; out[2]='6'; out[3]='~'; return 4;
        case 0x1C: out[0] = '\r'; return 1;    // Enter
        case 0x0E: out[0] = 0x7f; return 1;    // Backspace
        default:   return 0;
    }
}

// ---------------------------------------------------------------------------
// #227: THE winsize fill. This file carried FOUR VERBATIM COPIES of the block
// below (pty setup, EVENT_RESIZE, the F9 reflow, the TERMPREF.CFG live
// reload), recorded as debt by docs/TERMINAL_MODULES.md. It is the
// SIGWINCH-critical path: a terminal whose row/column count changed and whose
// child was not told is a full-screen program drawing to the wrong size, which
// is what #227 was. With splits there is one of these per PANE, so a fifth,
// sixth and seventh copy were exactly what was about to be written. There is
// now one. A master of -1 is a no-op, so no caller needs a guard.
void term_pty_set_winsize(int master, int rows, int cols) {
    if (master < 0) return;
    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master, TIOCSWINSZ, &ws);
}

// ---------------------------------------------------------------------------
// PHASE 1 (tabs/splits): run_foreground_pty() is GONE, and this is why.
//
// It opened the pty, spawned the child, and then ran a NESTED EVENT LOOP,
// calling win_get_event() itself, until the child exited. That was correct for
// a terminal with exactly one grid: there was nothing else to draw. It cannot
// survive a second pane. While pane A ran `vi`, pane B's child could produce
// output that nobody read and pane B could not repaint, because the only
// thread in the process was sitting inside A's loop. "Two panes are genuinely
// independent" and "a command owns the event loop until it exits" are not
// simultaneously satisfiable.
//
// The function is therefore split into the three phases the one event loop in
// main.c needs: START (non-blocking), DRAIN (called every idle tick, per
// pane), FINISH (on the master's EOF). The F9 / EVENT_RESIZE / prefs-reload
// handling that the nested loop duplicated from main.c is not reproduced here
// at all - there is one copy of each, in term_layout.c, and it now reaches
// EVERY pane instead of just the one that happened to be running a child.
// ---------------------------------------------------------------------------

// Open a pty, spawn `path` on it, hand the child its own process group and the
// terminal. Returns the child pid (>0) with *out_master set, or <=0 having
// already printed the reason into the grid. Never blocks.
int term_pty_start(const char *path, char **argv, int argc, int *out_master) {
    *out_master = -1;
    int master = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
    if (master < 0) {
        term_puts("\033[31mpty: /dev/ptmx unavailable\033[0m\n");
        return -1;
    }
    int pts_idx = -1;
    if (ioctl(master, TIOCGPTN, &pts_idx) != 0 || pts_idx < 0) {
        term_puts("\033[31mpty: TIOCGPTN failed\033[0m\n");
        close(master);
        return -1;
    }
    // Tell the tty its window size so the child (vi) sizes its screen
    // correctly. term_rows/term_cols here are the ACTIVE pane's, which is the
    // pane this child belongs to: term_layout.c activates it before calling.
    term_pty_set_winsize(master, term_rows, term_cols);

    char slavepath[24];
    { const char *pre = "/dev/pts/"; int i = 0;
      while (pre[i]) { slavepath[i] = pre[i]; i++; }
      if (pts_idx >= 10) slavepath[i++] = (char)('0' + pts_idx / 10);
      slavepath[i++] = (char)('0' + pts_idx % 10);
      slavepath[i] = '\0'; }
    int slave = open(slavepath, O_RDWR);
    if (slave < 0) {
        term_puts("\033[31mpty: slave open failed\033[0m\n");
        close(master);
        return -1;
    }
    // Point the child's stdio at the slave: save ours, dup2 slave, spawn (the
    // spawn inherits fds 0/1/2), then restore ours and drop the slave so only
    // the child holds it (its close on exit gives the master EOF).
    int s0 = dup(0), s1 = dup(1), s2 = dup(2);
    dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
    int pid = sys_spawn_args(path, argv, argc);
    // #745 (local 99): restore ONLY from a save that succeeded, and where it
    // did not, CLOSE the descriptor rather than leaving it where it is. A
    // failed dup() returns -1 and the old unconditional `dup2(sN, N)` then
    // silently did nothing, which does not restore the fd -- it LEAVES IT ON
    // THE PTY SLAVE. That is the state that makes the master's EOF
    // unreachable, so this loop is what stops the bug fixed above from being
    // reintroduced by any future caller that hands us a closed fd. Making the
    // bad state unrepresentable, rather than fixing the one caller that
    // reached it.
    int sv[3] = { s0, s1, s2 };
    for (int i = 0; i < 3; i++) {
        if (sv[i] >= 0) { dup2(sv[i], i); close(sv[i]); }
        else            { close(i); }
    }
    close(slave);

    // #745 (local 99): make the child its own process group and hand that
    // group the terminal, so drivers/tty.c's ISIG path has somebody to signal.
    //
    // The kernel half of job control was ALREADY complete and enforced:
    // tty_input_byte() raises SIGINT/SIGQUIT/SIGTSTP at t->fg_pgrp, TIOCSWINSZ
    // raises SIGWINCH there and tty_hangup() raises SIGHUP there. But nothing
    // in the tree ever called TIOCSPGRP, so fg_pgrp was 0 on every pty ever
    // opened and every one of those `if (t->fg_pgrp)` guards was false.
    // MEASURED on golden build 1872: ^C against a foreground `cat` did
    // nothing at all.
    //
    // A NEW group per child is required, not the terminal's own: signalling
    // the group the Terminal is in would kill the Terminal along with the
    // child. setpgid(pid, pid) is permitted here because the target is our
    // child, in our session, and pgid == target_pid creates a brand-new group
    // (see kernel/rustkern/pgrp.rs rules 2/3/5). A child that has already
    // exited returns ESRCH, which is normal and ignored.
    if (pid > 0) {
        setpgid(pid, pid);
        unsigned int pg = (unsigned int)pid;
        ioctl(master, TIOCSPGRP, &pg);
    }
    if (pid <= 0) {
        term_puts("\033[31mFailed to run: ");
        term_puts(path);
        term_puts("\033[0m\n");
        close(master);
        return -1;
    }
    // Tier 1 (docs/TERMINAL_PARITY.md): DSR (CSI n / CSI 6n) needs somewhere
    // to write its reply, and this is the only fd it could ever go to. It is
    // BANKED PER PANE by term_layout.c, so a DSR arriving in pane B is
    // answered on pane B's master, never on pane A's.
    g_active_master_fd = master;
    *out_master = master;
    return pid;
}

// Drain whatever the child has produced into the ACTIVE pane's grid. Returns 1
// if anything was written (the caller should repaint that pane, and treat it
// as ACTIVITY for notification purposes). Sets *alive to 0 on EOF.
//
// #426: `master` is O_NONBLOCK and this loop is bounded by the data actually
// available. It never waits for the child; the event loop's own timeout is the
// only place this process ever sleeps.
int term_pty_drain(int master, int *alive) {
    char rbuf[1024];
    int drew = 0;
    if (master < 0) { *alive = 0; return 0; }
    for (;;) {
        int n = read(master, rbuf, sizeof(rbuf));
        if (n > 0) {
            for (int i = 0; i < n; i++) term_putc(rbuf[i]);
            drew = 1;
            if (n < (int)sizeof(rbuf)) break;   // likely drained
            continue;
        }
        if (n == 0) { *alive = 0; break; }      // EOF: child exited
        break;                                   // EAGAIN / no data
    }
    return drew;
}

// Reap the child and put the ACTIVE pane back into a sane state for the shell
// prompt. Must be called with that pane active (term_layout.c does). Returns
// the child's raw exit status: the notification decision ("was this worth a
// toast") belongs to whoever knows which TAB this child was in, which is
// term_layout.c, not this file.
int term_pty_finish(int master, int pid) {
    int st = 0;
    if (pid > 0) sys_waitpid(pid, &st, 0);
    g_active_master_fd = -1;   // this master is about to be closed; DSR has nowhere to reply once it is
    if (master >= 0) close(master);
    // Reset rendition and leave the cursor visible for the shell prompt.
    // Tier 1: also leave autowrap on and drop any leftover scroll region/alt
    // screen state, so a child that exited mid full-screen-mode (crashed
    // without its own cleanup sequence) cannot leave the shell prompt stuck
    // with a half-height scroll region or the alternate screen's content.
    term_emu_sgr_reset(&g_pen); cursor_visible = true; cursor_blink_on = true;
    // A child that exited without restoring these leaves the SHELL prompt
    // emitting mouse-report bytes as text, which reads as a keyboard fault.
    term_emu_modes_reset();
    // Abandon any half-received sequence or UTF-8 character the child died
    // mid-way through, so its tail cannot be mis-parsed against the prompt.
    term_emu_reset(&g_parser);
    term_autowrap = 1; scroll_top = 0; scroll_bottom = -1;
    if (in_alt_screen) {
        for (int r = 0; r < TERM_MAX_ROWS; r++)
            for (int cc = 0; cc < TERM_MAX_COLS; cc++)
                cells[r][cc] = alt_saved_cells[r][cc];
        cursor_x = alt_saved_cursor_x;
        cursor_y = alt_saved_cursor_y;
        in_alt_screen = 0;
    }
    term_redraw();
    return st;
}
