// ncdemo - a tiny ncurses program that proves the MayteraOS ncurses port (#745)
// renders a real TUI and takes input, driven through the Terminal's pty.
//
// It sets NO environment: the port is built with --with-default-terminfo-dir=
// /TERMINFO and patched to default TERM to "maytera", so an unmodified ncurses
// program works with no env plumbing (a pty-spawned child has no environ).
//
// It exercises: initscr, colour pairs (start_color/init_pair), a subwindow with
// box() (which uses the DEC line-drawing charset that term_parse.c now maps to
// Unicode box characters), cursor addressing (mvwprintw), refresh, keypad input
// (KEY_UP/DOWN/LEFT/RIGHT via wgetch), and clean teardown (endwin).
#include <curses.h>

int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int use_color = has_colors();
    if (use_color) {
        start_color();
        init_pair(1, COLOR_YELLOW, COLOR_BLUE);
        init_pair(2, COLOR_WHITE,  COLOR_RED);
        init_pair(3, COLOR_CYAN,   COLOR_BLACK);
    }

    int H, W;
    getmaxyx(stdscr, H, W);
    if (use_color) {
        attron(COLOR_PAIR(3));
        mvprintw(0, 2, "MayteraOS ncurses %s  (%dx%d)  -  press arrows, q to quit", NCURSES_VERSION, W, H);
        attroff(COLOR_PAIR(3));
    } else {
        mvprintw(0, 2, "MayteraOS ncurses %s  (%dx%d)  -  press arrows, q to quit", NCURSES_VERSION, W, H);
    }
    refresh();

    int wh = 9, ww = 46;
    WINDOW *win = newwin(wh, ww, (H - wh) / 2, (W - ww) / 2);
    keypad(win, TRUE);   // arrow/nav keys are decoded on the window that reads them
    if (use_color) wbkgd(win, COLOR_PAIR(1));
    box(win, 0, 0);                          // real line drawing via ACS
    mvwhline(win, 2, 1, 0, ww - 2);          // ACS_HLINE across the box
    mvwprintw(win, 1, 2, "ncurses TUI is alive.");
    mvwprintw(win, 4, 2, "Last key: (none yet)");
    mvwprintw(win, 6, 2, "Colors: %s   Box: real line-drawing", use_color ? "yes (256)" : "no");
    wrefresh(win);

    int ch;
    while ((ch = wgetch(win)) != 'q') {
        const char *name = "?";
        if      (ch == KEY_UP)    name = "KEY_UP";
        else if (ch == KEY_DOWN)  name = "KEY_DOWN";
        else if (ch == KEY_LEFT)  name = "KEY_LEFT";
        else if (ch == KEY_RIGHT) name = "KEY_RIGHT";
        else if (ch >= 32 && ch < 127) name = "char";

        if (use_color) wattron(win, COLOR_PAIR(2));
        mvwprintw(win, 4, 2, "Last key: %-10s (code %d)   ", name, ch);
        if (use_color) wattroff(win, COLOR_PAIR(2));
        // keep the frame crisp after writes
        box(win, 0, 0);
        mvwhline(win, 2, 1, 0, ww - 2);
        wrefresh(win);
    }

    endwin();
    return 0;
}
