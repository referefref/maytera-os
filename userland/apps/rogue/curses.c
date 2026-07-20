
/*
 * curses.c - MayteraOS curses shim for Rogue
 *
 * Maps the curses API to the MayteraOS GUI window system.
 * Screen: 80x24 character grid, each cell 8x16 pixels -> 640x384 window.
 */

#include "../../libc/maytera.h"
#include "compat/curses.h"

/* ── Screen constants ─────────────────────────────────────────────── */
#define CHAR_W   8
#define CHAR_H  16
#define SCRCOLS 80
#define SCRLINES 24
#define WIN_W   (SCRCOLS  * CHAR_W)   /* 640 */
#define WIN_H   (SCRLINES * CHAR_H)   /* 384 */

/* ── Colours ──────────────────────────────────────────────────────── */
#define COL_BG      0x00000000u   /* black background */
#define COL_FG      0x00CCCCCCu   /* light grey text  */
#define COL_REV_BG  0x00CCCCCCu   /* reverse: bg becomes fg */
#define COL_REV_FG  0x00000000u   /* reverse: fg becomes bg */

/* ── Cell ─────────────────────────────────────────────────────────── */
typedef struct {
    char     ch;
    unsigned int fg, bg;
    int      dirty;
} cell_t;

static cell_t screen[SCRLINES][SCRCOLS];
static int    win_handle = -1;

/* ── Exported globals ─────────────────────────────────────────────── */
int LINES = SCRLINES;
int COLS  = SCRCOLS;

static WINDOW _stdscr = { 0, 0, SCRLINES, SCRCOLS, 0, 0, A_NORMAL };
static WINDOW _curscr = { 0, 0, SCRLINES, SCRCOLS, 0, 0, A_NORMAL };
WINDOW *stdscr = &_stdscr;
WINDOW *curscr = &_curscr;

/* ── Event type ───────────────────────────────────────────────────── */
typedef struct {
    int          type;
    unsigned int target_id;
    int          mouse_x, mouse_y;
    unsigned int mouse_buttons;
    signed char  scroll_delta;
    /* 3 bytes pad */
    unsigned int keycode;
    char         key_char;
    /* 3 bytes pad */
} win_ev_t;

#define EV_KEY_DOWN   5
#define EV_KEY_UP     6
#define EV_WIN_CLOSE  7

/* Syscall numbers */
#define SYS_WIN_CREATE    30
#define SYS_WIN_DRAW_RECT 32
#define SYS_WIN_DRAW_TEXT 33
#define SYS_WIN_GET_EVENT 36
#define SYS_WIN_INVALIDATE 37

static inline int  _win_create(const char *t, int x, int y, int w, int h) {
    return (int)syscall5(SYS_WIN_CREATE, (long)t, x, y, w, h);
}
static inline void _win_draw_rect(int h, int x, int y, int w, int ht, unsigned int c) {
    syscall6(SYS_WIN_DRAW_RECT, h, x, y, w, ht, c);
}
static inline void _win_draw_text(int h, int x, int y, const char *s, unsigned int c) {
    syscall5(SYS_WIN_DRAW_TEXT, h, x, y, (long)s, c);
}
static inline int _win_get_event(int h, win_ev_t *ev, int timeout) {
    return (int)syscall3(SYS_WIN_GET_EVENT, h, (long)ev, timeout);
}
static inline void _win_invalidate(int h) { syscall1(SYS_WIN_INVALIDATE, h); }

/* ── Internal helpers ─────────────────────────────────────────────── */

static void screen_init(void) {
    for (int r = 0; r < SCRLINES; r++)
        for (int c = 0; c < SCRCOLS; c++) {
            screen[r][c].ch = ' ';
            screen[r][c].fg = COL_FG;
            screen[r][c].bg = COL_BG;
            screen[r][c].dirty = 1;
        }
}

/* Render a single cell to the physical window */
static void render_cell(int row, int col) {
    if (win_handle < 0) return;
    cell_t *c = &screen[row][col];
    int px = col * CHAR_W;
    int py = row * CHAR_H;
    /* background */
    _win_draw_rect(win_handle, px, py, CHAR_W, CHAR_H, c->bg);
    /* character */
    if (c->ch != ' ') {
        char buf[2] = { c->ch, 0 };
        _win_draw_text(win_handle, px, py, buf, c->fg);
    }
    c->dirty = 0;
}

/* ── WINDOW helpers ───────────────────────────────────────────────── */
static cell_t *cell_at(WINDOW *w, int row, int col) {
    int r = w->begy + row;
    int c = w->begx + col;
    if (r < 0 || r >= SCRLINES || c < 0 || c >= SCRCOLS) return 0;
    return &screen[r][c];
}

static void win_putch(WINDOW *w, chtype ch) {
    char  ascii = (char)(ch & A_CHARTEXT);
    int   rev   = (w->attr & A_STANDOUT) || (w->attr & A_REVERSE) ||
                  (ch & A_STANDOUT)      || (ch & A_REVERSE);
    unsigned int fg = rev ? COL_REV_FG : COL_FG;
    unsigned int bg = rev ? COL_REV_BG : COL_BG;

    if (ascii == '\n') {
        /* fill rest of line with spaces */
        while (w->curx < w->maxx) {
            cell_t *c = cell_at(w, w->cury, w->curx);
            if (c) { c->ch = ' '; c->fg = fg; c->bg = bg; c->dirty = 1; }
            w->curx++;
        }
        w->cury++;
        w->curx = 0;
        return;
    }
    if (ascii == '\b') {
        if (w->curx > 0) w->curx--;
        return;
    }
    if (ascii == '\r') { w->curx = 0; return; }

    cell_t *c = cell_at(w, w->cury, w->curx);
    if (c) { c->ch = ascii; c->fg = fg; c->bg = bg; c->dirty = 1; }
    w->curx++;
    if (w->curx >= w->maxx) { w->curx = 0; w->cury++; }
    if (w->cury >= w->maxy)   w->cury = w->maxy - 1;
}

/* ── Public API ───────────────────────────────────────────────────── */

WINDOW *initscr(void) {
    screen_init();
    win_handle = _win_create("Rogue", 50, 50, WIN_W, WIN_H);
    /* paint black background immediately */
    if (win_handle >= 0) {
        _win_draw_rect(win_handle, 0, 0, WIN_W, WIN_H, COL_BG);
        _win_invalidate(win_handle);
    }
    return stdscr;
}

int endwin(void) { return 0; }

int noecho(void)    { return 0; }
int cbreak(void)    { return 0; }
int raw(void)       { return 0; }
int nonl(void)      { return 0; }
int nl(void)        { return 0; }
int keypad(WINDOW *w, int bf)    { (void)w; (void)bf; return 0; }
int curs_set(int v)              { (void)v; return 0; }
int intrflush(WINDOW *w, int bf) { (void)w; (void)bf; return 0; }
int meta(WINDOW *w, int bf)      { (void)w; (void)bf; return 0; }
int scrollok(WINDOW *w, int bf)  { (void)w; (void)bf; return 0; }

WINDOW *newwin(int nlines, int ncols, int begy, int begx) {
    WINDOW *w = (WINDOW *)malloc(sizeof(WINDOW));
    if (!w) return 0;
    w->maxy = nlines; w->maxx = ncols;
    w->begy = begy;   w->begx = begx;
    w->cury = 0;      w->curx = 0;
    w->attr = A_NORMAL;
    return w;
}
int delwin(WINDOW *w) { if (w && w != stdscr && w != curscr) free(w); return 0; }
int touchwin(WINDOW *w) {
    for (int r = 0; r < w->maxy; r++)
        for (int c = 0; c < w->maxx; c++) {
            cell_t *cell = cell_at(w, r, c);
            if (cell) cell->dirty = 1;
        }
    return 0;
}
int wnoutrefresh(WINDOW *w) { (void)w; return 0; }
int doupdate(void) { return refresh(); }

int wrefresh(WINDOW *w) {
    (void)w;
    /* always refresh full screen */
    if (win_handle < 0) return 0;
    for (int r = 0; r < SCRLINES; r++)
        for (int c = 0; c < SCRCOLS; c++)
            if (screen[r][c].dirty)
                render_cell(r, c);
    _win_invalidate(win_handle);
    return 0;
}
int refresh(void) { return wrefresh(stdscr); }

int wclear(WINDOW *w) {
    for (int r = 0; r < w->maxy; r++)
        for (int c = 0; c < w->maxx; c++) {
            cell_t *cell = cell_at(w, r, c);
            if (cell) { cell->ch = ' '; cell->fg = COL_FG; cell->bg = COL_BG; cell->dirty = 1; }
        }
    w->cury = 0; w->curx = 0;
    return 0;
}
int clear(void)  { return wclear(stdscr); }
int werase(WINDOW *w) { return wclear(w); }
int erase(void)  { return wclear(stdscr); }

int wclrtoeol(WINDOW *w) {
    for (int c = w->curx; c < w->maxx; c++) {
        cell_t *cell = cell_at(w, w->cury, c);
        if (cell) { cell->ch = ' '; cell->fg = COL_FG; cell->bg = COL_BG; cell->dirty = 1; }
    }
    return 0;
}
int clrtoeol(void) { return wclrtoeol(stdscr); }

int wclrtobot(WINDOW *w) {
    wclrtoeol(w);
    for (int r = w->cury + 1; r < w->maxy; r++)
        for (int c = 0; c < w->maxx; c++) {
            cell_t *cell = cell_at(w, r, c);
            if (cell) { cell->ch = ' '; cell->fg = COL_FG; cell->bg = COL_BG; cell->dirty = 1; }
        }
    return 0;
}
int clrtobot(void) { return wclrtobot(stdscr); }

int wmove(WINDOW *w, int y, int x) { w->cury = y; w->curx = x; return 0; }
int move(int y, int x) { return wmove(stdscr, y, x); }

int waddch(WINDOW *w, chtype ch)            { win_putch(w, ch); return 0; }
int addch(chtype ch)                        { return waddch(stdscr, ch); }
int mvaddch(int y, int x, chtype ch)        { move(y, x); return addch(ch); }
int mvwaddch(WINDOW *w, int y, int x, chtype ch) { wmove(w, y, x); return waddch(w, ch); }

int waddstr(WINDOW *w, const char *s) {
    while (s && *s) win_putch(w, (unsigned char)*s++);
    return 0;
}
int addstr(const char *s)                      { return waddstr(stdscr, s); }
int mvaddstr(int y, int x, const char *s)      { move(y, x); return addstr(s); }
int mvwaddstr(WINDOW *w, int y, int x, const char *s) { wmove(w, y, x); return waddstr(w, s); }

int wprintw(WINDOW *w, const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    return waddstr(w, buf);
}
int printw(const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    return addstr(buf);
}
int mvprintw(int y, int x, const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    move(y, x);
    return addstr(buf);
}
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    wmove(w, y, x);
    return waddstr(w, buf);
}

int wstandout(WINDOW *w) { w->attr |=  A_STANDOUT; return 0; }
int wstandend(WINDOW *w) { w->attr &= ~A_STANDOUT; return 0; }
int standout(void)  { return wstandout(stdscr); }
int standend(void)  { return wstandend(stdscr); }
int wattron(WINDOW *w, chtype a)  { w->attr |=  a; return 0; }
int wattroff(WINDOW *w, chtype a) { w->attr &= ~a; return 0; }

/* ── Key mapping ─────────────────────────────────────────────────── */
static int map_keycode(unsigned int kc) {
    /* Arrow keys from MayteraOS kernel */
    if (kc == 128) return KEY_UP;
    if (kc == 129) return KEY_DOWN;
    if (kc == 130) return KEY_LEFT;
    if (kc == 131) return KEY_RIGHT;
    if (kc == 0x0D || kc == 0x0A) return '\n';
    if (kc == 0x7F || kc == 0x08) return KEY_BACKSPACE;
    if (kc == 0x1B) return 0x1B;  /* ESC */
    if (kc >= 32 && kc < 127) return (int)kc;
    return -1;
}

int wgetch(WINDOW *w) {
    (void)w;
    if (win_handle < 0) return -1;
    /* Flush display before blocking for input */
    wrefresh(stdscr);
    win_ev_t ev;
    for (;;) {
        int r = _win_get_event(win_handle, &ev, 50);
        if (r <= 0) { sys_sleep(10); continue; }
        if (ev.type == EV_WIN_CLOSE) sys_exit(0);
        if (ev.type != EV_KEY_DOWN)  continue;
        int k = map_keycode(ev.keycode);
        if (k >= 0) return k;
    }
}
int getch(void) { return wgetch(stdscr); }

int wgetstr(WINDOW *w, char *str) {
    int i = 0;
    for (;;) {
        int c = wgetch(w);
        if (c == '\n' || c == '\r') { str[i] = '\0'; return 0; }
        if ((c == KEY_BACKSPACE || c == '\b' || c == 0x7F) && i > 0) {
            i--;
            w->curx--;
            win_putch(w, ' ');
            w->curx--;
            wrefresh(w);
            continue;
        }
        if (c >= 32 && c < 127) {
            str[i++] = (char)c;
            win_putch(w, (chtype)c);
            wrefresh(w);
        }
    }
}
int getstr(char *s) { return wgetstr(stdscr, s); }
int mvgetstr(int y, int x, char *s) { move(y, x); return getstr(s); }

int beep(void)  { return 0; }
int flash(void) { return 0; }
