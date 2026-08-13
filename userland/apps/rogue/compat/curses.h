
#ifndef COMPAT_CURSES_H
#define COMPAT_CURSES_H

#include <stdarg.h>

/* ── Screen geometry ─────────────────────────────────────────────── */
#define NUMLINES  24
#define NUMCOLS   80

extern int LINES;
extern int COLS;

/* ── Attribute bits (chtype) ─────────────────────────────────────── */
typedef unsigned long chtype;
#define A_NORMAL    0UL
#define A_STANDOUT  (1UL << 8)
#define A_REVERSE   (1UL << 9)
#define A_BOLD      (1UL << 10)
#define A_CHARTEXT  0xFFUL

/* ── WINDOW type ─────────────────────────────────────────────────── */
typedef struct _WINDOW {
    int cury, curx;     /* current cursor position */
    int maxy, maxx;     /* window dimensions */
    int begy, begx;     /* window origin on screen */
    chtype attr;        /* current attribute */
} WINDOW;

extern WINDOW *stdscr;
extern WINDOW *curscr;

/* MayteraOS port note: traditional BSD curses exposes the "true" cursor
 * position via the struct fields _cury/_curx. Rogue's upstream main.c
 * (tstp(), unmodified) reads/writes curscr->_cury/_curx directly. Alias
 * them to our cury/curx fields instead of duplicating the state. */
#define _cury cury
#define _curx curx

/* ── Arrow key codes (returned by getch) ─────────────────────────── */
#define KEY_MIN    0x101
#define KEY_UP     0x103
#define KEY_DOWN   0x102
#define KEY_LEFT   0x104
#define KEY_RIGHT  0x105
#define KEY_ENTER  0x157
#define KEY_BACKSPACE 0x107

/* ── Initialisation ──────────────────────────────────────────────── */
WINDOW *initscr(void);
int     endwin(void);

/* ── Input modes (no-ops on MayteraOS) ──────────────────────────── */
int noecho(void);
int cbreak(void);
int raw(void);
int nonl(void);
int nl(void);
int keypad(WINDOW *win, int bf);
int curs_set(int visibility);
int intrflush(WINDOW *win, int bf);
int meta(WINDOW *win, int bf);
int scrollok(WINDOW *win, int bf);

/* ── Window management ───────────────────────────────────────────── */
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
WINDOW *subwin(WINDOW *orig, int nlines, int ncols, int begin_y, int begin_x);
int     mvwin(WINDOW *win, int y, int x);
int     delwin(WINDOW *win);
int     touchwin(WINDOW *win);
int     wnoutrefresh(WINDOW *win);
int     doupdate(void);
int     getmaxx(WINDOW *win);
int     getmaxy(WINDOW *win);
int     isendwin(void);
int     baudrate(void);
int     mvcur(int oldrow, int oldcol, int newrow, int newcol);
int     erasechar(void);
int     killchar(void);
int     flushinp(void);
char   *unctrl(chtype ch);

/* ── Output ──────────────────────────────────────────────────────── */
int refresh(void);
int wrefresh(WINDOW *win);
int clear(void);
int wclear(WINDOW *win);
int erase(void);
int werase(WINDOW *win);
int clrtoeol(void);
int wclrtoeol(WINDOW *win);
int clrtobot(void);
int wclrtobot(WINDOW *win);

int move(int y, int x);
int wmove(WINDOW *win, int y, int x);

int addch(chtype ch);
int waddch(WINDOW *win, chtype ch);
int mvaddch(int y, int x, chtype ch);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);

int addstr(const char *str);
int waddstr(WINDOW *win, const char *str);
int mvaddstr(int y, int x, const char *str);
int mvwaddstr(WINDOW *win, int y, int x, const char *str);

int printw(const char *fmt, ...);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvprintw(int y, int x, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);

int standout(void);
int standend(void);
int wstandout(WINDOW *win);
int wstandend(WINDOW *win);
int wattron(WINDOW *win, chtype attr);
int wattroff(WINDOW *win, chtype attr);

/* ── Input ───────────────────────────────────────────────────────── */
int getch(void);
int wgetch(WINDOW *win);
int getstr(char *str);
int wgetstr(WINDOW *win, char *str);
int wgetnstr(WINDOW *win, char *str, int n);
int mvgetstr(int y, int x, char *str);

/* ── Read-back ───────────────────────────────────────────────────── */
chtype inch(void);
chtype winch(WINDOW *win);
chtype mvinch(int y, int x);
chtype mvwinch(WINDOW *win, int y, int x);

/* ── Screen state ────────────────────────────────────────────────── */
int beep(void);
int flash(void);

/* ── Macros matching traditional curses ──────────────────────────── */
#define nocbreak()         cbreak()   /* no-op either way */
#define noraw()            raw()
#define clearok(w,bf)      (0)
#define leaveok(w,bf)      (0)
#define idlok(w,bf)        (0)
#define immedok(w,bf)      (0)
#define wbkgd(w,ch)        (0)
#define wbkgdset(w,ch)     (void)(0)
#define bkgd(ch)           (0)
#define getyx(w,y,x)       ((y)=(w)->cury,(x)=(w)->curx)
#define getbegyx(w,y,x)    ((y)=(w)->begy,(x)=(w)->begx)
#define getmaxyx(w,y,x)    ((y)=(w)->maxy,(x)=(w)->maxx)

#endif /* COMPAT_CURSES_H */
