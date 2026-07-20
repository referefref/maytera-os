// mvi - MayteraOS mini-vi
// Modal editor: normal/insert/command modes.
// Motions: h j k l, arrow keys, 0 (line start), $ (line end), gg, G.
// Edits: i (insert), a (append), o (open line), x (del char), dd (del line).
// Command: :w :q :wq :q!
// Works over raw termios (cfmakeraw); redraws full screen each tick.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "errno.h"
#include "signal.h"
#include "termios.h"
#include "sys/ioctl.h"

#define MAX_LINES 4096
#define MAX_LINE  1024

static char *g_lines[MAX_LINES];
static int g_nlines;

static int g_cur_row, g_cur_col;   // buffer coords
static int g_top;                  // viewport top line
static int g_rows = 24, g_cols = 80;

static const char *g_file_path;
static int g_dirty;

static struct termios g_saved_t;
static int g_tsaved;

// ---- terminal control ----
static void cls(void)    { write(1, "\x1b[2J", 4); }
static void home(void)   { write(1, "\x1b[H", 3); }
static void gotoxy(int r, int c) {
    char b[32];
    int n = snprintf(b, sizeof(b), "\x1b[%d;%dH", r, c);
    write(1, b, n);
}
static void set_reverse(int on) { write(1, on ? "\x1b[7m" : "\x1b[27m", on ? 4 : 5); }
static void hide_cursor(void) { write(1, "\x1b[?25l", 6); }
static void show_cursor(void) { write(1, "\x1b[?25h", 6); }

// ---- line helpers ----
static void line_insert(int idx, const char *s) {
    if (g_nlines >= MAX_LINES) return;
    for (int i = g_nlines; i > idx; i--) g_lines[i] = g_lines[i-1];
    char *d = (char *)malloc(strlen(s) + 1);
    strcpy(d, s);
    g_lines[idx] = d;
    g_nlines++;
}
static void line_remove(int idx) {
    if (idx < 0 || idx >= g_nlines) return;
    free(g_lines[idx]);
    for (int i = idx; i + 1 < g_nlines; i++) g_lines[i] = g_lines[i+1];
    g_nlines--;
    if (g_nlines == 0) line_insert(0, "");
}

// ---- file I/O ----
static void load_file(const char *path) {
    g_nlines = 0;
    g_file_path = path;
    FILE *f = fopen(path, "r");
    if (!f) { line_insert(0, ""); return; }
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), f)) {
        int n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        line_insert(g_nlines, buf);
    }
    fclose(f);
    if (g_nlines == 0) line_insert(0, "");
}

static int save_file(const char *path) {
    FILE *f = fopen(path ? path : g_file_path, "w");
    if (!f) return -1;
    for (int i = 0; i < g_nlines; i++) {
        fputs(g_lines[i], f);
        fputc('\n', f);
    }
    fclose(f);
    g_dirty = 0;
    return 0;
}

// ---- render ----
static void redraw(const char *status_msg) {
    hide_cursor();
    home();
    int text_rows = g_rows - 1;
    for (int r = 0; r < text_rows; r++) {
        int lno = g_top + r;
        write(1, "\x1b[K", 3);
        if (lno < g_nlines) {
            const char *l = g_lines[lno];
            int len = strlen(l);
            if (len > g_cols) len = g_cols;
            write(1, l, len);
        } else {
            write(1, "~", 1);
        }
        if (r + 1 < text_rows) write(1, "\r\n", 2);
    }
    gotoxy(g_rows, 1);
    write(1, "\x1b[K", 3);
    set_reverse(1);
    if (status_msg && *status_msg) {
        write(1, status_msg, strlen(status_msg));
    } else {
        char tmp[160];
        int n = snprintf(tmp, sizeof(tmp), "  %s%s  %d,%d  (lines: %d)",
                         g_file_path ? g_file_path : "[No Name]",
                         g_dirty ? " [+]" : "",
                         g_cur_row + 1, g_cur_col + 1, g_nlines);
        write(1, tmp, n);
    }
    set_reverse(0);
    gotoxy(g_cur_row - g_top + 1, g_cur_col + 1);
    show_cursor();
}

// ---- input ----
static int read_byte(unsigned char *out) {
    long r = read(0, out, 1);
    return (r == 1) ? 0 : -1;
}

// Read an escape sequence after ESC received. Returns a synthesised key code.
// 0x100 = up, 0x101 = down, 0x102 = right, 0x103 = left
static int read_escape(void) {
    unsigned char c;
    if (read_byte(&c) < 0) return 0x1b;   // lone ESC
    if (c != '[') return c;
    if (read_byte(&c) < 0) return 0x1b;
    switch (c) {
        case 'A': return 0x100;
        case 'B': return 0x101;
        case 'C': return 0x102;
        case 'D': return 0x103;
    }
    return 0;
}

// ---- motions ----
static void clamp_col(void) {
    int len = strlen(g_lines[g_cur_row]);
    if (g_cur_col > len) g_cur_col = len;
    if (g_cur_col < 0) g_cur_col = 0;
}
static void scroll_into_view(void) {
    int text_rows = g_rows - 1;
    if (g_cur_row < g_top) g_top = g_cur_row;
    if (g_cur_row >= g_top + text_rows) g_top = g_cur_row - text_rows + 1;
    if (g_top < 0) g_top = 0;
}

static void move_left(void)  { if (g_cur_col > 0) g_cur_col--; }
static void move_right(void) { int l = strlen(g_lines[g_cur_row]); if (g_cur_col < l) g_cur_col++; }
static void move_up(void)    { if (g_cur_row > 0) g_cur_row--; clamp_col(); scroll_into_view(); }
static void move_down(void)  { if (g_cur_row + 1 < g_nlines) g_cur_row++; clamp_col(); scroll_into_view(); }

// ---- insert mode ----
static void insert_char(int c) {
    char *l = g_lines[g_cur_row];
    int len = strlen(l);
    if (len + 2 >= MAX_LINE) return;
    char *nl = (char *)malloc(len + 2);
    int i;
    for (i = 0; i < g_cur_col; i++) nl[i] = l[i];
    nl[i] = (char)c;
    for (int j = g_cur_col; j < len; j++) nl[j+1] = l[j];
    nl[len+1] = 0;
    free(l);
    g_lines[g_cur_row] = nl;
    g_cur_col++;
    g_dirty = 1;
}

static void insert_newline(void) {
    char *l = g_lines[g_cur_row];
    int len = strlen(l);
    char *before = (char *)malloc(g_cur_col + 1);
    char *after  = (char *)malloc(len - g_cur_col + 1);
    for (int i = 0; i < g_cur_col; i++) before[i] = l[i];
    before[g_cur_col] = 0;
    for (int i = g_cur_col; i < len; i++) after[i - g_cur_col] = l[i];
    after[len - g_cur_col] = 0;
    free(l);
    g_lines[g_cur_row] = before;
    // insert after-line
    if (g_nlines < MAX_LINES) {
        for (int i = g_nlines; i > g_cur_row + 1; i--) g_lines[i] = g_lines[i-1];
        g_lines[g_cur_row + 1] = after;
        g_nlines++;
    } else {
        free(after);
    }
    g_cur_row++;
    g_cur_col = 0;
    scroll_into_view();
    g_dirty = 1;
}

static void del_char_before(void) {
    if (g_cur_col == 0) {
        if (g_cur_row == 0) return;
        // join with previous line
        char *prev = g_lines[g_cur_row - 1];
        char *cur  = g_lines[g_cur_row];
        int pl = strlen(prev);
        int cl = strlen(cur);
        char *nl = (char *)malloc(pl + cl + 1);
        int i;
        for (i = 0; i < pl; i++) nl[i] = prev[i];
        for (int j = 0; j < cl; j++) nl[pl + j] = cur[j];
        nl[pl + cl] = 0;
        free(prev);
        g_lines[g_cur_row - 1] = nl;
        line_remove(g_cur_row);
        g_cur_row--;
        g_cur_col = pl;
        g_dirty = 1;
        return;
    }
    char *l = g_lines[g_cur_row];
    int len = strlen(l);
    for (int i = g_cur_col - 1; i < len; i++) l[i] = l[i+1];
    g_cur_col--;
    g_dirty = 1;
}

// ---- command mode ----
static int prompt_cmd(char *out, int outsz) {
    gotoxy(g_rows, 1);
    write(1, "\x1b[K:", 4);
    int n = 0;
    while (1) {
        unsigned char c;
        if (read_byte(&c) < 0) return -1;
        if (c == '\n' || c == '\r') break;
        if (c == 0x7F || c == 0x08) {
            if (n > 0) { n--; write(1, "\b \b", 3); }
            continue;
        }
        if (c == 0x1b) return -1;
        if (n < outsz - 1) {
            out[n++] = c;
            write(1, &c, 1);
        }
    }
    out[n] = 0;
    return 0;
}

// ---- main loop ----
static void quit_cleanup(void) {
    if (g_tsaved) tcsetattr(0, TCSANOW, &g_saved_t);
    show_cursor();
    cls();
    home();
}

static void handle_winch(int s) {
    (void)s;
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0) {
        if (w.ws_row) g_rows = w.ws_row;
        if (w.ws_col) g_cols = w.ws_col;
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : 0;

    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row) {
        g_rows = w.ws_row;
        g_cols = w.ws_col;
    }

    if (tcgetattr(0, &g_saved_t) == 0) g_tsaved = 1;
    struct termios t = g_saved_t;
    cfmakeraw(&t);
    tcsetattr(0, TCSANOW, &t);

    signal(SIGWINCH, handle_winch);
    signal(SIGINT, SIG_IGN);

    if (path) load_file(path);
    else      line_insert(0, "");

    cls();
    redraw(0);

    int mode = 'n';  // 'n' normal, 'i' insert
    int last = 0;    // for 'dd', 'gg'
    char status[128] = "";
    int running = 1;

    while (running) {
        unsigned char c;
        if (read_byte(&c) < 0) { running = 0; break; }
        int k = c;
        if (c == 0x1b) k = read_escape();

        if (mode == 'n') {
            if (k == 'h' || k == 0x103) move_left();
            else if (k == 'l' || k == 0x102) move_right();
            else if (k == 'k' || k == 0x100) move_up();
            else if (k == 'j' || k == 0x101) move_down();
            else if (k == '0') g_cur_col = 0;
            else if (k == '$') { g_cur_col = strlen(g_lines[g_cur_row]); if (g_cur_col > 0) g_cur_col--; }
            else if (k == 'G') { g_cur_row = g_nlines - 1; clamp_col(); scroll_into_view(); }
            else if (k == 'g') {
                if (last == 'g') { g_cur_row = 0; g_cur_col = 0; scroll_into_view(); last = 0; continue; }
            }
            else if (k == 'x') {
                char *l = g_lines[g_cur_row];
                int len = strlen(l);
                if (g_cur_col < len) {
                    for (int i = g_cur_col; i < len; i++) l[i] = l[i+1];
                    g_dirty = 1;
                }
            }
            else if (k == 'd') {
                if (last == 'd') { line_remove(g_cur_row); if (g_cur_row >= g_nlines) g_cur_row = g_nlines - 1; clamp_col(); scroll_into_view(); g_dirty = 1; last = 0; continue; }
            }
            else if (k == 'i') { mode = 'i'; }
            else if (k == 'a') { mode = 'i'; int len = strlen(g_lines[g_cur_row]); if (g_cur_col < len) g_cur_col++; }
            else if (k == 'A') { mode = 'i'; g_cur_col = strlen(g_lines[g_cur_row]); }
            else if (k == 'o') { mode = 'i'; g_cur_col = strlen(g_lines[g_cur_row]); insert_newline(); }
            else if (k == ':') {
                char cmd[64];
                if (prompt_cmd(cmd, sizeof(cmd)) == 0) {
                    if (strcmp(cmd, "q") == 0) {
                        if (g_dirty) { strcpy(status, "E37: no write since last change (use :q!)"); }
                        else running = 0;
                    }
                    else if (strcmp(cmd, "q!") == 0) running = 0;
                    else if (strcmp(cmd, "w") == 0) {
                        if (!g_file_path) strcpy(status, "E32: no file name");
                        else if (save_file(0) == 0) snprintf(status, sizeof(status), "\"%s\" written", g_file_path);
                        else snprintf(status, sizeof(status), "write failed: %s", strerror(errno));
                    }
                    else if (strcmp(cmd, "wq") == 0) {
                        if (!g_file_path) strcpy(status, "E32: no file name");
                        else if (save_file(0) == 0) running = 0;
                        else snprintf(status, sizeof(status), "write failed: %s", strerror(errno));
                    }
                    else if (cmd[0] == 'w' && cmd[1] == ' ') {
                        if (save_file(cmd + 2) == 0) { g_file_path = 0; snprintf(status, sizeof(status), "\"%s\" written", cmd + 2); }
                        else snprintf(status, sizeof(status), "write failed: %s", strerror(errno));
                    }
                    else snprintf(status, sizeof(status), "E492: not an editor command: %s", cmd);
                }
            }
            last = k;
        } else {
            // insert
            if (k == 0x1b) { mode = 'n'; if (g_cur_col > 0) g_cur_col--; }
            else if (k == '\r' || k == '\n') insert_newline();
            else if (k == 0x7F || k == 0x08) del_char_before();
            else if (k == 0x103) move_left();
            else if (k == 0x102) move_right();
            else if (k == 0x100) move_up();
            else if (k == 0x101) move_down();
            else if (k >= 32 && k < 127) insert_char(k);
            else if (k == '\t') insert_char(' '), insert_char(' '), insert_char(' '), insert_char(' ');
            last = 0;
        }

        clamp_col();
        scroll_into_view();
        redraw(status);
        status[0] = 0;
    }

    quit_cleanup();
    return 0;
}
