// terminal - GUI Terminal for MayteraOS (user-space version)
// A full-featured terminal with shell functionality and ANSI escape support
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/aiclient.h"

// Terminal dimensions
#define TERM_CHAR_W     8
#define TERM_CHAR_H     16
// Maximum grid the static cell buffer can hold (sized for a full-screen window:
// 1280/8 = 160 cols, ~720/16 = 45 rows, with headroom).
#define TERM_MAX_COLS   170
#define TERM_MAX_ROWS   52
// Initial grid used when the window is first created.
#define TERM_INIT_COLS  80
#define TERM_INIT_ROWS  24
#define TERM_WIDTH      (TERM_INIT_COLS * TERM_CHAR_W + 4)   // +4 for padding
#define TERM_HEIGHT     (TERM_INIT_ROWS * TERM_CHAR_H + 24)  // +24 for title bar adjustment

// Scrollback buffer size
#define SCROLLBACK_LINES 100

// ANSI color indices
#define COLOR_INDEX_BLACK   0
#define COLOR_INDEX_RED     1
#define COLOR_INDEX_GREEN   2
#define COLOR_INDEX_YELLOW  3
#define COLOR_INDEX_BLUE    4
#define COLOR_INDEX_MAGENTA 5
#define COLOR_INDEX_CYAN    6
#define COLOR_INDEX_WHITE   7

// ANSI colors table
static const uint32_t ansi_colors[16] = {
    0x00000000,  // Black
    0x00AA0000,  // Red
    0x0000AA00,  // Green
    0x00AAAA00,  // Yellow
    0x000000AA,  // Blue
    0x00AA00AA,  // Magenta
    0x0000AAAA,  // Cyan
    0x00AAAAAA,  // White
    0x00555555,  // Bright Black
    0x00FF5555,  // Bright Red
    0x0055FF55,  // Bright Green
    0x00FFFF55,  // Bright Yellow
    0x005555FF,  // Bright Blue
    0x00FF55FF,  // Bright Magenta
    0x0055FFFF,  // Bright Cyan
    0x00FFFFFF   // Bright White
};

// Terminal cell
typedef struct {
    char ch;
    uint8_t fg;  // Color index
    uint8_t bg;
} term_cell_t;

// Terminal state
static int window_handle = -1;
static term_cell_t cells[TERM_MAX_ROWS][TERM_MAX_COLS];
// Runtime grid size (changes when the window is resized/maximized/restored).
static int term_cols = TERM_INIT_COLS;
static int term_rows = TERM_INIT_ROWS;
// Current content pixel size, used to clear the full content area on redraw.
static int term_px_w = TERM_INIT_COLS * TERM_CHAR_W;
static int term_px_h = TERM_INIT_ROWS * TERM_CHAR_H;
static int cursor_x = 0;
static int cursor_y = 0;
static bool cursor_visible = true;
static uint8_t current_fg = 7;  // White
static uint8_t current_bg = 0;  // Black

// Input handling
#define INPUT_MAX 256
#define HISTORY_SIZE 32
static char input_line[INPUT_MAX];
static int input_pos = 0;
static char history[HISTORY_SIZE][INPUT_MAX];
static int history_count = 0;
static int history_pos = 0;

// Current working directory
static char cwd[256] = "/";

// ANSI escape sequence state
#define STATE_NORMAL  0
#define STATE_ESCAPE  1
#define STATE_CSI     2
static int escape_state = STATE_NORMAL;
static int escape_params[16];
static int escape_param_count = 0;

// Window position for coordinate conversion
static int win_x = 100;
static int win_y = 50;

// Environment variables (simple implementation)
#define MAX_ENV_VARS 32
static char env_names[MAX_ENV_VARS][64];
static char env_values[MAX_ENV_VARS][256];
static int env_count = 0;

// Forward declarations
static void term_redraw(void);
static void term_scroll(void);
static void term_newline(void);
static void term_putc(char c);
static void term_puts(const char *str);
static void term_clear(void);
static void execute_command(const char *cmd);

// String helper functions

static void str_copy(char *dest, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}


static void int_to_str(int num, char *buf) {
    char tmp[20];
    int i = 0;
    int neg = 0;
    
    if (num < 0) {
        neg = 1;
        num = -num;
    }
    
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// Draw a single cell
static void draw_cell(int col, int row) {
    term_cell_t *cell = &cells[row][col];
    int x = col * TERM_CHAR_W + 2;  // +2 for left padding
    int y = row * TERM_CHAR_H + 2;  // +2 for top padding

    // ANSI colors (lines ~33-50) stay semantic; only the *default* fg/bg
    // (the terminal chrome) follows the active theme so the terminal matches
    // the rest of the desktop. cell->bg==0 / cell->fg==7 are the defaults.
    uint32_t fg = ansi_colors[cell->fg & 0x0F];
    uint32_t bg = ansi_colors[cell->bg & 0x0F];
    if (cell->bg == 0) bg = theme_color(THEME_COLOR_TEXTBOX_BG);
    if (cell->fg == 7) fg = theme_color(THEME_COLOR_TEXTBOX_TEXT);

    // Draw background
    win_draw_rect(window_handle, x, y, TERM_CHAR_W, TERM_CHAR_H, bg);

    // Draw character if printable
    if (cell->ch >= ' ' && cell->ch < 127) {
        char str[2] = { cell->ch, '\0' };
        win_draw_text(window_handle, x, y, str, fg);
    }
}

// Recompute the grid for a new content size (called on EVENT_RESIZE).
static void term_handle_resize(int content_w, int content_h) {
    int nc = content_w / TERM_CHAR_W;
    int nr = content_h / TERM_CHAR_H;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    if (nc > TERM_MAX_COLS) nc = TERM_MAX_COLS;
    if (nr > TERM_MAX_ROWS) nr = TERM_MAX_ROWS;

    // Initialise any cells newly exposed by growing the grid so they hold a
    // valid blank cell rather than stale memory.
    for (int row = 0; row < nr; row++) {
        for (int col = 0; col < nc; col++) {
            if (row >= term_rows || col >= term_cols) {
                cells[row][col].ch = ' ';
                cells[row][col].fg = current_fg;
                cells[row][col].bg = current_bg;
            }
        }
    }

    term_cols = nc;
    term_rows = nr;
    term_px_w = content_w;
    term_px_h = content_h;

    if (cursor_x >= term_cols) cursor_x = term_cols - 1;
    if (cursor_y >= term_rows) cursor_y = term_rows - 1;
}

// Redraw the terminal
static void term_redraw(void) {
    // Clear the full content area (covers the kernel's resize fill colour)
    win_draw_rect(window_handle, 0, 0, term_px_w, term_px_h, theme_color(THEME_COLOR_TEXTBOX_BG));

    // Draw all cells
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            draw_cell(col, row);
        }
    }

    // Draw cursor
    if (cursor_visible) {
        int cx = cursor_x * TERM_CHAR_W + 2;
        int cy = cursor_y * TERM_CHAR_H + TERM_CHAR_H - 2 + 2;
        win_draw_rect(window_handle, cx, cy, TERM_CHAR_W, 2, theme_color(THEME_COLOR_TEXTBOX_CURSOR));  // Themed cursor
    }

    win_invalidate(window_handle);
}

// Scroll terminal up one line
static void term_scroll(void) {
    // Move all rows up
    for (int row = 0; row < term_rows - 1; row++) {
        for (int col = 0; col < term_cols; col++) {
            cells[row][col] = cells[row + 1][col];
        }
    }
    // Clear last row
    for (int col = 0; col < term_cols; col++) {
        cells[term_rows - 1][col].ch = ' ';
        cells[term_rows - 1][col].fg = current_fg;
        cells[term_rows - 1][col].bg = current_bg;
    }
}

// Handle newline
static void term_newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= term_rows) {
        cursor_y = term_rows - 1;
        term_scroll();
    }
}

// Parse ANSI escape sequence parameter
static void parse_csi_params(void) {
    escape_param_count = 0;
    escape_params[0] = 0;
}

// Handle ANSI escape sequences
static void handle_escape_char(char c) {
    if (escape_state == STATE_ESCAPE) {
        if (c == '[') {
            escape_state = STATE_CSI;
            parse_csi_params();
        } else {
            escape_state = STATE_NORMAL;
        }
        return;
    }

    if (escape_state == STATE_CSI) {
        if (c >= '0' && c <= '9') {
            escape_params[escape_param_count] = escape_params[escape_param_count] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (escape_param_count < 15) {
                escape_param_count++;
                escape_params[escape_param_count] = 0;
            }
            return;
        }

        // End of sequence - process command
        escape_param_count++;  // Count includes last param

        switch (c) {
            case 'm':  // SGR - Set Graphics Rendition
                for (int i = 0; i < escape_param_count; i++) {
                    int p = escape_params[i];
                    if (p == 0) {
                        // Reset
                        current_fg = 7;
                        current_bg = 0;
                    } else if (p >= 30 && p <= 37) {
                        // Foreground color
                        current_fg = p - 30;
                    } else if (p >= 40 && p <= 47) {
                        // Background color
                        current_bg = p - 40;
                    } else if (p >= 90 && p <= 97) {
                        // Bright foreground
                        current_fg = p - 90 + 8;
                    } else if (p >= 100 && p <= 107) {
                        // Bright background
                        current_bg = p - 100 + 8;
                    } else if (p == 1) {
                        // Bold - use bright color
                        current_fg |= 0x08;
                    }
                }
                break;

            case 'H':  // Cursor position
            case 'f':
                {
                    int row = escape_params[0] > 0 ? escape_params[0] - 1 : 0;
                    int col = escape_param_count > 1 && escape_params[1] > 0 ? escape_params[1] - 1 : 0;
                    cursor_y = row < term_rows ? row : term_rows - 1;
                    cursor_x = col < term_cols ? col : term_cols - 1;
                }
                break;

            case 'J':  // Erase display
                if (escape_params[0] == 2) {
                    term_clear();
                }
                break;

            case 'K':  // Erase line
                for (int col = cursor_x; col < term_cols; col++) {
                    cells[cursor_y][col].ch = ' ';
                    cells[cursor_y][col].fg = current_fg;
                    cells[cursor_y][col].bg = current_bg;
                }
                break;

            case 'A':  // Cursor up
                {
                    int n = escape_params[0] > 0 ? escape_params[0] : 1;
                    cursor_y -= n;
                    if (cursor_y < 0) cursor_y = 0;
                }
                break;

            case 'B':  // Cursor down
                {
                    int n = escape_params[0] > 0 ? escape_params[0] : 1;
                    cursor_y += n;
                    if (cursor_y >= term_rows) cursor_y = term_rows - 1;
                }
                break;

            case 'C':  // Cursor forward
                {
                    int n = escape_params[0] > 0 ? escape_params[0] : 1;
                    cursor_x += n;
                    if (cursor_x >= term_cols) cursor_x = term_cols - 1;
                }
                break;

            case 'D':  // Cursor back
                {
                    int n = escape_params[0] > 0 ? escape_params[0] : 1;
                    cursor_x -= n;
                    if (cursor_x < 0) cursor_x = 0;
                }
                break;
        }

        escape_state = STATE_NORMAL;
    }
}

// Put a character to terminal
static void term_putc(char c) {
    if (escape_state != STATE_NORMAL) {
        handle_escape_char(c);
        return;
    }

    if (c == '\033') {
        escape_state = STATE_ESCAPE;
        return;
    }

    if (c == '\n') {
        term_newline();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\t') {
        int spaces = 8 - (cursor_x % 8);
        for (int i = 0; i < spaces && cursor_x < term_cols; i++) {
            cells[cursor_y][cursor_x].ch = ' ';
            cells[cursor_y][cursor_x].fg = current_fg;
            cells[cursor_y][cursor_x].bg = current_bg;
            cursor_x++;
        }
        if (cursor_x >= term_cols) {
            term_newline();
        }
        return;
    }

    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        }
        return;
    }

    // Printable character
    if (c >= ' ' && c < 127) {
        cells[cursor_y][cursor_x].ch = c;
        cells[cursor_y][cursor_x].fg = current_fg;
        cells[cursor_y][cursor_x].bg = current_bg;
        cursor_x++;
        if (cursor_x >= term_cols) {
            term_newline();
        }
    }
}

// Print a string
static void term_puts(const char *str) {
    while (*str) {
        term_putc(*str++);
    }
}

// Print an integer
static void term_put_int(int num) {
    char buf[20];
    int_to_str(num, buf);
    term_puts(buf);
}

// Clear terminal
static void term_clear(void) {
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            cells[row][col].ch = ' ';
            cells[row][col].fg = current_fg;
            cells[row][col].bg = current_bg;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

// Print the prompt
static void print_prompt(void) {
    // Green color for prompt
    term_puts("\033[32muser@maytera\033[0m:\033[34m");
    term_puts(cwd);
    term_puts("\033[0m$ ");
}

// Set environment variable
static void setenv_local(const char *name, const char *value) {
    // Check if already exists
    for (int i = 0; i < env_count; i++) {
        if (str_eq(env_names[i], name)) {
            str_copy(env_values[i], value, 256);
            return;
        }
    }
    // Add new
    if (env_count < MAX_ENV_VARS) {
        str_copy(env_names[env_count], name, 64);
        str_copy(env_values[env_count], value, 256);
        env_count++;
    }
}

// Read a terminal-local environment variable, or NULL if unset.
static const char *getenv_local(const char *name) {
    for (int i = 0; i < env_count; i++)
        if (str_eq(env_names[i], name)) return env_values[i];
    return (void *)0;
}

// Open a candidate path; on success copy it to out[256] and return 1.
static int try_open_path(const char *cand, char *out) {
    int fd = open(cand, 0);
    if (fd >= 0) { close(fd); str_copy(out, cand, 256); return 1; }
    return 0;
}

// Build "<dir>/<name>" into out[256]. up=1 uppercases name; ext=1 appends ".ELF".
static void make_candidate(char *out, const char *dir, int dirlen,
                           const char *name, int up, int ext) {
    int o = 0;
    for (int i = 0; i < dirlen && o < 255; i++) out[o++] = dir[i];
    if (o == 0 || out[o-1] != '/') { if (o < 255) out[o++] = '/'; }
    for (int i = 0; name[i] && o < 255; i++) {
        char c = name[i];
        if (up && c >= 'a' && c <= 'z') c -= 32;
        out[o++] = c;
    }
    if (ext) { const char *e = ".ELF"; for (int i = 0; e[i] && o < 255; i++) out[o++] = e[i]; }
    out[o] = '\0';
}

// Resolve a command name to an executable path the way a real shell does:
// search each ':'-separated directory in $PATH (default "/APPS") for the name
// in its common forms (exact, uppercased, and with a ".ELF" extension), then
// fall back to the per-game nested convention /GAMES/<NAME>/<NAME>.ELF (games
// ship in their own directory next to their data files). A name containing '/'
// is treated as a literal path. No program name is ever special-cased: install
// anything into a $PATH directory and it runs by name. Returns 1 + out on hit.
static int resolve_program(const char *name, char *out) {
    if (!name || !name[0]) return 0;

    for (const char *q = name; *q; q++)
        if (*q == '/') return try_open_path(name, out);   // explicit path

    const char *path = getenv_local("PATH");
    if (!path || !path[0]) path = "/APPS";

    char cand[256];
    const char *p = path;
    while (*p) {
        const char *start = p;
        while (*p && *p != ':') p++;
        int dirlen = (int)(p - start);
        if (dirlen > 0 && dirlen < 200) {
            make_candidate(cand, start, dirlen, name, 0, 0); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 1, 0); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 0, 1); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 1, 1); if (try_open_path(cand, out)) return 1;
        }
        if (*p == ':') p++;
    }

    // Per-game nested convention: /GAMES/<NAME>/<NAME>.ELF
    char up[64]; int j = 0;
    for (; name[j] && j < 63; j++) { char c = name[j]; if (c >= 'a' && c <= 'z') c -= 32; up[j] = c; }
    up[j] = '\0';
    int o = 0; const char *pre = "/GAMES/";
    for (int i = 0; pre[i] && o < 255; i++) cand[o++] = pre[i];
    for (int i = 0; up[i] && o < 255; i++) cand[o++] = up[i];
    if (o < 255) cand[o++] = '/';
    for (int i = 0; up[i] && o < 255; i++) cand[o++] = up[i];
    { const char *e = ".ELF"; for (int i = 0; e[i] && o < 255; i++) cand[o++] = e[i]; }
    cand[o] = '\0';
    if (try_open_path(cand, out)) return 1;

    return 0;
}

// A GUI app creates its own window and runs its own event loop forever
// (it does not exit and may not produce stdout). Such apps must be launched
// detached: no stdout pipe capture and no blocking waitpid, otherwise the
// terminal would block forever in waitpid and the app would stall once the
// captured-output pipe fills. Games under /GAMES/ are GUI apps.
static int is_gui_app(const char *path) {
    return str_starts(path, "/GAMES/");
}

// Execute a shell command
// pipe_resolve: tokenize a single command string in place and resolve its
// program path via the /APPS lookup. Returns 1 on success.
static int pipe_resolve(char *line, char **argv, int *argcp, char *pathout) {
    int argc = 0;
    char *p = line;
    while (*p && argc < 31) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = (void *)0;
    if (argc == 0) return 0;
    *argcp = argc;
    char *prog = argv[0];
    if (prog[0] == '/') {
        int fd = open(prog, 0);
        if (fd >= 0) { close(fd); str_copy(pathout, prog, 256); return 1; }
        return 0;
    }
    char path[256];
    str_copy(path, "/APPS/", 256);
    int pl = 6, k = 0;
    while (prog[k] && pl < 255) {
        char c = prog[k++];
        if (c >= 'a' && c <= 'z') c -= 32;
        path[pl++] = c;
    }
    path[pl] = '\0';
    int fd = open(path, 0);
    if (fd >= 0) { close(fd); str_copy(pathout, path, 256); return 1; }
    str_copy(path, "/APPS/", 256);
    pl = 6; k = 0;
    while (prog[k] && pl < 255) path[pl++] = prog[k++];
    path[pl] = '\0';
    fd = open(path, 0);
    if (fd >= 0) { close(fd); str_copy(pathout, path, 256); return 1; }
    return 0;
}

// run_pipe: execute "c1 | c2" with c1's stdout feeding c2's stdin and c2's
// stdout captured for display. Single pipe (two stages). Uses the VFS fd
// table (pipe/dup2/spawn-inherit all share it), so this works app-to-app.
static void run_pipe(char *c1, char *c2) {
    char *argv1[32], *argv2[32];
    int argc1 = 0, argc2 = 0;
    char path1[256], path2[256];
    if (!pipe_resolve(c1, argv1, &argc1, path1) ||
        !pipe_resolve(c2, argv2, &argc2, path2)) {
        term_puts("\033[31mCommand not found in pipe\033[0m\n");
        return;
    }
    argv1[0] = path1;
    argv2[0] = path2;

    int P[2] = {-1, -1}, CAP[2] = {-1, -1};
    if (pipe(P) != 0) return;
    if (pipe(CAP) != 0) { close(P[0]); close(P[1]); return; }

    dup2(P[1], 1);                                  // stage 1 stdout -> pipe
    int pid1 = sys_spawn_args(path1, argv1, argc1);

    dup2(P[0], 0);                                  // stage 2 stdin  <- pipe
    dup2(CAP[1], 1);                                // stage 2 stdout -> capture
    int pid2 = sys_spawn_args(path2, argv2, argc2);

    // Drop all terminal-held ends so EOF propagates; keep CAP[0] to read.
    close(P[0]); close(P[1]); close(CAP[1]);
    close(0); close(1);

    int st = 0;
    if (pid1 > 0) sys_waitpid(pid1, &st, 0);
    if (pid2 > 0) sys_waitpid(pid2, &st, 0);

    char buf[512];
    int n;
    while ((n = read(CAP[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        term_puts(buf);
    }
    close(CAP[0]);

    // Restore stdio fds (console = fd 2) for subsequent commands.
    dup2(2, 0);
    dup2(2, 1);

    if (pid1 <= 0 || pid2 <= 0)
        term_puts("\033[31mpipe: failed to start a command\033[0m\n");
}

// AI prefix (#224/#292): a terminal line beginning with '?' is sent to the
// built-in Kimi assistant via the SHARED aiclient module (the SAME tools and
// permission gate as the aichat widget and msh). The conversation persists across
// '?' lines for this terminal session, so the AI's two-step confirm flow for
// high-risk writes works (it asks; you reply "? yes"; it acts). The call is
// bounded (capped tool actions + a kernel HTTPS deadline) so it cannot wedge the
// terminal.
static int g_ai_ready = 0;   // 0=uninit, 1=ready, -1=no key

static void term_ai_handle(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) {
        term_puts("Usage: ? <question or command for the AI>\n");
        return;
    }
    if (g_ai_ready == 0) {
        term_puts("\033[2mAI: connecting...\033[0m\n");
        term_redraw();
        g_ai_ready = aiclient_init() ? 1 : -1;
        if (g_ai_ready == 1) aiclient_reset();   // seed system prompt once per session
    }
    if (g_ai_ready != 1) {
        term_puts("AI unavailable: no API key at /CONFIG/KIMI.KEY\n");
        return;
    }
    aiclient_add(0, rest);
    static char answer[8192];
    term_puts("\033[2mAI: thinking...\033[0m\n");
    term_redraw();
    int rc = aiclient_run_turn(answer, sizeof(answer), 0);
    if (rc != 0) { term_puts("\033[31mAI error:\033[0m "); term_puts(answer); term_puts("\n"); }
    else         { term_puts("\033[36mAI:\033[0m ");       term_puts(answer); term_puts("\n"); }
}

static void execute_command(const char *cmd) {
    // Skip leading whitespace
    while (*cmd == ' ') cmd++;

    // Empty command
    if (!*cmd) return;

    // AI prefix: a line starting with '?' goes to the built-in assistant.
    if (*cmd == '?') { term_ai_handle(cmd + 1); return; }

    // Built-in commands (only ones that MUST be builtins)
    if (str_eq(cmd, "help")) {
        term_puts("\033[1;33mMayteraOS Terminal - Commands:\033[0m\n");
        term_puts("\nBuilt-in: cd, pwd, clear, history, exit, export, help\n");
        term_puts("External: ls, cat, head, tail, grep, echo, touch, date,\n");
        term_puts("  uptime, whoami, env, cp, mv, rm, mkdir, rmdir, sort,\n");
        term_puts("  wc, tac, less, test, tee, tr, cut, uniq, stat\n");
        term_puts("\nAll commands in /APPS/ are available.\n");
        term_puts("Use UP/DOWN arrows for command history.\n");
        return;
    }

    if (str_eq(cmd, "clear")) {
        term_clear();
        return;
    }

    if (str_eq(cmd, "pwd")) {
        term_puts(cwd);
        term_puts("\n");
        return;
    }

    if (str_eq(cmd, "history")) {
        if (history_count == 0) {
            term_puts("No commands in history.\n");
        } else {
            int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
            for (int i = start; i < history_count; i++) {
                int idx = i % HISTORY_SIZE;
                term_puts("  ");
                term_put_int(i + 1);
                term_puts("  ");
                term_puts(history[idx]);
                term_puts("\n");
            }
        }
        return;
    }

    if (str_starts(cmd, "export ")) {
        const char *arg = cmd + 7;
        while (*arg == ' ') arg++;
        const char *eq = arg;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            char varname[64];
            int i = 0;
            while (arg < eq && i < 63) {
                varname[i++] = *arg++;
            }
            varname[i] = '\0';
            setenv_local(varname, eq + 1);
        } else {
            term_puts("\033[31mUsage: export VAR=VALUE\033[0m\n");
        }
        return;
    }

    if (str_starts(cmd, "cd ")) {
        const char *dir = cmd + 3;
        while (*dir == ' ') dir++;

        if (!*dir || str_eq(dir, "~")) {
            cwd[0] = '/';
            cwd[1] = '\0';
            return;
        }

        // Build the candidate path first, then validate it actually exists
        // (and is a directory) via SYS_CHDIR before committing to cwd. The old
        // code edited cwd unconditionally, so `cd /does/not/exist` silently
        // "succeeded".
        char cand[256];
        if (*dir == '/') {
            int i = 0;
            while (*dir && i < 255) { cand[i++] = *dir++; }
            cand[i] = '\0';
        } else if (str_eq(dir, "..")) {
            int len = 0;
            while (cwd[len] && len < 255) { cand[len] = cwd[len]; len++; }
            cand[len] = '\0';
            if (len > 1) {
                len--;
                while (len > 0 && cand[len] != '/') len--;
                if (len == 0) len = 1;
                cand[len] = '\0';
            }
        } else if (str_eq(dir, ".")) {
            return;
        } else {
            int len = 0;
            while (cwd[len] && len < 255) { cand[len] = cwd[len]; len++; }
            if (len > 1 && len < 255) cand[len++] = '/';
            while (*dir && len < 255) { cand[len++] = *dir++; }
            cand[len] = '\0';
        }

        if (syscall1(SYS_CHDIR, (long)cand) < 0) {
            term_puts("cd: ");
            term_puts(cand);
            term_puts(": No such file or directory\n");
            return;
        }
        { int i = 0; while (cand[i]) { cwd[i] = cand[i]; i++; } cwd[i] = '\0'; }
        return;
    }

    if (str_eq(cmd, "cd")) {
        cwd[0] = '/';
        cwd[1] = '\0';
        return;
    }

    if (str_eq(cmd, "exit")) {
        term_puts("Goodbye!\n");
        return;
    }

    // Pipe: "cmd1 | cmd2" (single pipe, two stages).
    {
        const char *bar = (void *)0;
        for (const char *q = cmd; *q; q++) { if (*q == '|') { bar = q; break; } }
        if (bar) {
            static char lbuf[256], rbuf[256];
            int li = 0;
            for (const char *q = cmd; q < bar && li < 255; q++) lbuf[li++] = *q;
            lbuf[li] = '\0';
            int ri = 0;
            for (const char *q = bar + 1; *q && ri < 255; q++) rbuf[ri++] = *q;
            rbuf[ri] = '\0';
            run_pipe(lbuf, rbuf);
            return;
        }
    }

    // All other commands: try to run as external program with arguments
    {
        // Parse command into argv
        char cmd_copy[512];
        int ci = 0;
        while (cmd[ci] && ci < 511) { cmd_copy[ci] = cmd[ci]; ci++; }
        cmd_copy[ci] = '\0';

        char *argv_ptrs[32];
        int argc = 0;
        char *p = cmd_copy;
        while (*p && argc < 31) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                argv_ptrs[argc++] = p;
                while (*p && *p != q) p++;
                if (*p) *p++ = '\0';
            } else {
                argv_ptrs[argc++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = '\0';
            }
        }
        argv_ptrs[argc] = (void *)0;

        if (argc == 0) return;

        // --- I/O redirection: pull >, >>, <, 2> (and their filenames) out of
        // argv so they are not passed to the child. Open flags are numeric to
        // avoid header deps: O_WRONLY=0x1 O_CREAT=0x40 O_TRUNC=0x200 O_APPEND=0x400.
        const char *redir_out = (void *)0; int redir_append = 0;
        const char *redir_in  = (void *)0;
        const char *redir_err = (void *)0;
        {
            int w = 1;  // keep argv[0]
            for (int r = 1; r < argc; r++) {
                char *t = argv_ptrs[r];
                int is_out = str_eq(t, ">");
                int is_app = str_eq(t, ">>");
                int is_in  = str_eq(t, "<");
                int is_err = str_eq(t, "2>");
                if ((is_out || is_app || is_in || is_err) && r + 1 < argc) {
                    const char *fn = argv_ptrs[++r];
                    if (is_in)       redir_in = fn;
                    else if (is_err) redir_err = fn;
                    else { redir_out = fn; redir_append = is_app; }
                } else {
                    argv_ptrs[w++] = t;
                }
            }
            argv_ptrs[w] = (void *)0;
            argc = w;
        }
        if (argc == 0) return;

        // Resolve command path via $PATH (default /APPS) + nested-game
        // convention. Fully generic: no program name is special-cased.
        char prog_name[64];
        str_copy(prog_name, argv_ptrs[0], 64);

        char path[256];
        int found = resolve_program(prog_name, path);

        if (!found) {
            term_puts("\033[31mCommand not found: ");
            term_puts(prog_name);
            term_puts("\033[0m\n");
            term_puts("Type 'help' for available commands.\n");
            return;
        }

        // Update argv[0] to the resolved path
        argv_ptrs[0] = path;

        // GUI apps (e.g. games under /GAMES/) create their own window and run
        // forever. Launch them detached: no stdout pipe, no waitpid. The child
        // inherits the terminal's fd 1 (NULL -> kernel routes putchar to the
        // serial console), exactly like a compositor icon launch.
        if (is_gui_app(path)) {
            int pid = sys_spawn_args(path, argv_ptrs, argc);
            if (pid > 0) {
                term_puts("Launched ");
                term_puts(path);
                term_puts("\n");
            } else {
                term_puts("\033[31mFailed to run: ");
                term_puts(path);
                term_puts("\033[0m\n");
            }
            return;
        }

        // Capture the child's stdout through a pipe (pipes, dup2 and spawn fd
        // inheritance all share the VFS per-process fd table). For "> file"
        // we buffer the captured output, then CLOSE the pipe (freeing its VFS
        // fds) BEFORE opening the file: file opens use the legacy FAT fd table,
        // whose fd numbers would otherwise collide with the still-open VFS pipe
        // fds (sys_write checks VFS first, so the write would hit the pipe, not
        // the file). ("< file" and "2> file" are parsed but not yet wired.)
        int pipefd[2] = {-1, -1};
        int use_pipe = (pipe(pipefd) == 0);
        if (use_pipe) {
            dup2(pipefd[1], 1);  // fd 1 = pipe write end
            close(pipefd[1]);
            pipefd[1] = -1;
        }

        int pid = sys_spawn_args(path, argv_ptrs, argc);

        if (use_pipe) close(1);  // drop our write end so reads see EOF

        static char capbuf[65536];   // captured stdout when redirecting to a file
        int caplen = 0;

        if (pid > 0) {
            int status = 0;
            sys_waitpid(pid, &status, 0);

            if (use_pipe && pipefd[0] >= 0) {
                char buf[512];
                int n;
                while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
                    if (redir_out) {
                        for (int i = 0; i < n && caplen < (int)sizeof(capbuf); i++)
                            capbuf[caplen++] = buf[i];
                    } else {
                        buf[n] = '\0';
                        term_puts(buf);
                    }
                }
            }
        } else {
            term_puts("\033[31mFailed to run: ");
            term_puts(path);
            term_puts("\033[0m\n");
        }

        if (pipefd[0] >= 0) close(pipefd[0]);  // free VFS pipe fds before file open

        // Write the captured output to the redirection target.
        if (redir_out && pid > 0) {
            if (!redir_append) sys_unlink(redir_out);     // truncate: drop old file
            int outfd = open(redir_out, 0x41);            // O_WRONLY | O_CREAT
            if (outfd >= 0) {
                if (redir_append) sys_seek(outfd, 0, 2);  // SEEK_END
                if (caplen > 0) sys_write(outfd, capbuf, caplen);
                close(outfd);
            } else {
                term_puts("\033[31mcannot open: ");
                term_puts(redir_out);
                term_puts("\033[0m\n");
            }
        }
    }
}

// Add command to history
static void add_to_history(const char *cmd) {
    if (!*cmd) return;  // Don't add empty commands

    // Don't add duplicates
    if (history_count > 0) {
        int last = (history_count - 1) % HISTORY_SIZE;
        if (str_eq(history[last], cmd)) return;
    }

    int idx = history_count % HISTORY_SIZE;
    int i = 0;
    while (cmd[i] && i < INPUT_MAX - 1) {
        history[idx][i] = cmd[i];
        i++;
    }
    history[idx][i] = '\0';
    history_count++;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Initialize default environment variables
    setenv_local("PATH", "/APPS");
    setenv_local("HOME", "/");
    setenv_local("SHELL", "/bin/terminal");
    setenv_local("USER", "user");
    setenv_local("TERM", "maytera-256color");

    // Sync the kernel per-process working directory with our tracked cwd ("/")
    // at startup. Children we spawn (ls, cat, ...) inherit the kernel cwd, and
    // tools like "ls" with no argument resolve "." via getcwd(); without this
    // they would see whatever cwd the compositor happened to launch us with.
    syscall1(SYS_CHDIR, (long)cwd);

    // Create window
    window_handle = win_create("Terminal", win_x, win_y, TERM_WIDTH, TERM_HEIGHT);
    if (window_handle < 0) {
        return 1;
    }

    printf("Terminal window created (handle=%d)\n", window_handle);

    // Initialize terminal
    term_clear();

    // Welcome message
    term_puts("\033[1;36m");
    term_puts("MayteraOS Terminal v1.0\n");
    term_puts("\033[0m");
    term_puts("Type 'help' for available commands.\n\n");

    // Print initial prompt
    print_prompt();
    term_redraw();

    // Event loop
    gui_event_t event;
    int running = 1;

    while (running) {
        int event_type = win_get_event(window_handle, &event, 100);

        if (event_type == 0) {
            // Toggle cursor blink
            static int blink_counter = 0;
            blink_counter++;
            if (blink_counter >= 5) {
                cursor_visible = !cursor_visible;
                blink_counter = 0;
                term_redraw();
            }
            continue;
        }

        switch (event.type) {
            case EVENT_REDRAW:
                term_redraw();
                break;

            case EVENT_RESIZE:
                // Window was resized/maximized/restored: mouse_x/mouse_y carry
                // the new content size in pixels. Reflow the grid and repaint.
                term_handle_resize(event.mouse_x, event.mouse_y);
                term_redraw();
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                {
                    char c = event.key_char;
                    uint32_t keycode = event.keycode;

                    // Handle special keys
                    if (keycode == 0x1C || c == '\n' || c == '\r') {
                        // Enter - execute command
                        term_puts("\n");
                        input_line[input_pos] = '\0';

                        if (str_eq(input_line, "exit")) {
                            running = 0;
                        } else {
                            add_to_history(input_line);
                            execute_command(input_line);
                            print_prompt();
                        }

                        input_pos = 0;
                        input_line[0] = '\0';
                        history_pos = history_count;
                        term_redraw();
                    }
                    else if (c == '\b' || keycode == 0x0E) {
                        // Backspace
                        if (input_pos > 0) {
                            input_pos--;
                            input_line[input_pos] = '\0';
                            term_putc('\b');
                            term_putc(' ');
                            term_putc('\b');
                            term_redraw();
                        }
                    }
                    else if (c == 27) {
                        // ESC - clear input
                        while (input_pos > 0) {
                            term_putc('\b');
                            term_putc(' ');
                            term_putc('\b');
                            input_pos--;
                        }
                        input_line[0] = '\0';
                        term_redraw();
                    }
                    else if (keycode == 0x80) {
                        // Up arrow - history previous
                        if (history_pos > 0 && history_count > 0) {
                            // Clear current input
                            while (input_pos > 0) {
                                term_putc('\b');
                                term_putc(' ');
                                term_putc('\b');
                                input_pos--;
                            }
                            history_pos--;
                            int idx = history_pos % HISTORY_SIZE;
                            input_pos = 0;
                            while (history[idx][input_pos]) {
                                input_line[input_pos] = history[idx][input_pos];
                                term_putc(input_line[input_pos]);
                                input_pos++;
                            }
                            input_line[input_pos] = '\0';
                            term_redraw();
                        }
                    }
                    else if (keycode == 0x81) {
                        // Down arrow - history next
                        if (history_pos < history_count) {
                            // Clear current input
                            while (input_pos > 0) {
                                term_putc('\b');
                                term_putc(' ');
                                term_putc('\b');
                                input_pos--;
                            }
                            history_pos++;
                            if (history_pos < history_count) {
                                int idx = history_pos % HISTORY_SIZE;
                                while (history[idx][input_pos]) {
                                    input_line[input_pos] = history[idx][input_pos];
                                    term_putc(input_line[input_pos]);
                                    input_pos++;
                                }
                            }
                            input_line[input_pos] = '\0';
                            term_redraw();
                        }
                    }
                    else if (c >= ' ' && c < 127) {
                        // Printable character
                        if (input_pos < INPUT_MAX - 1) {
                            input_line[input_pos++] = c;
                            input_line[input_pos] = '\0';
                            term_putc(c);
                            term_redraw();
                        }
                    }
                }
                break;

            default:
                break;
        }
    }

    // Cleanup
    win_destroy(window_handle);
    printf("Terminal closed\n");

    return 0;
}
