// terminal.h - GUI Terminal application for MayteraOS
// XTerm-compatible terminal with ANSI escape sequence support
#ifndef TERMINAL_H
#define TERMINAL_H

#include "../types.h"
#include "window.h"

// Terminal dimensions
#define TERM_COLS       80
#define TERM_ROWS       24
#define TERM_CHAR_W     8
#define TERM_CHAR_H     16

// Terminal colors (ANSI standard 16 colors)
#define TERM_COLOR_BLACK        0xFF000000
#define TERM_COLOR_RED          0xFFAA0000
#define TERM_COLOR_GREEN        0xFF00AA00
#define TERM_COLOR_YELLOW       0xFFAAAA00
#define TERM_COLOR_BLUE         0xFF0000AA
#define TERM_COLOR_MAGENTA      0xFFAA00AA
#define TERM_COLOR_CYAN         0xFF00AAAA
#define TERM_COLOR_WHITE        0xFFAAAAAA
#define TERM_COLOR_BRIGHT_BLACK 0xFF555555
#define TERM_COLOR_BRIGHT_RED   0xFFFF5555
#define TERM_COLOR_BRIGHT_GREEN 0xFF55FF55
#define TERM_COLOR_BRIGHT_YELLOW 0xFFFFFF55
#define TERM_COLOR_BRIGHT_BLUE  0xFF5555FF
#define TERM_COLOR_BRIGHT_MAGENTA 0xFFFF55FF
#define TERM_COLOR_BRIGHT_CYAN  0xFF55FFFF
#define TERM_COLOR_BRIGHT_WHITE 0xFFFFFFFF

// Default colors
#define TERM_FG_COLOR   TERM_COLOR_BRIGHT_WHITE
#define TERM_BG_COLOR   0xFF1E1E1E  // Dark gray
#define TERM_CURSOR_COLOR 0xFF00FF00  // Green

// ANSI escape sequence states
#define TERM_STATE_NORMAL   0
#define TERM_STATE_ESCAPE   1   // Got ESC
#define TERM_STATE_CSI      2   // Got ESC [
#define TERM_STATE_OSC      3   // Got ESC ]

// Maximum ESC sequence parameters
#define TERM_MAX_PARAMS     16

// History buffer
#define TERM_HISTORY_SIZE   10
#define TERM_MAX_INPUT      256

// Scrollback buffer
#define TERM_SCROLLBACK_LINES 500  // Lines of scrollback history
#define TERM_TOTAL_LINES (TERM_ROWS + TERM_SCROLLBACK_LINES)
#define TERM_SCROLL_LINES 3        // Lines to scroll per mouse wheel notch

// Terminal cell with attributes
typedef struct {
    char    ch;         // Character
    uint8_t fg;         // Foreground color index (0-15)
    uint8_t bg;         // Background color index (0-15)
    uint8_t attr;       // Attributes (bold, underline, etc.)
} term_cell_t;

// Cell attributes
#define TERM_ATTR_NORMAL    0x00
#define TERM_ATTR_BOLD      0x01
#define TERM_ATTR_UNDERLINE 0x04
#define TERM_ATTR_BLINK     0x08
#define TERM_ATTR_INVERSE   0x10

// Terminal structure
typedef struct {
    window_t *window;
    term_cell_t *scrollback;                 // Scrollback buffer (TERM_TOTAL_LINES * TERM_COLS)
    term_cell_t cells[TERM_ROWS][TERM_COLS]; // Current visible cell buffer with attributes
    char buffer[TERM_ROWS][TERM_COLS + 1];   // Legacy text buffer
    int cursor_x;
    int cursor_y;
    int scroll_offset;                       // Lines scrolled back (0 = at bottom)
    int scrollback_pos;                      // Current write position in scrollback (0 to TERM_TOTAL_LINES-1)
    int scrollback_count;                    // Number of lines in scrollback (max TERM_TOTAL_LINES)
    bool cursor_visible;

    // Current attributes
    uint8_t current_fg;      // Current foreground color (0-15)
    uint8_t current_bg;      // Current background color (0-15)
    uint8_t current_attr;    // Current attributes

    // ANSI escape sequence parsing
    int escape_state;
    int escape_params[TERM_MAX_PARAMS];
    int escape_param_count;

    // Input handling
    char input_line[TERM_MAX_INPUT];
    int input_pos;

    // Command history
    char history[TERM_HISTORY_SIZE][TERM_MAX_INPUT];
    int history_count;
    int history_pos;

    // Current directory
    char cwd[256];

    bool running;

    // Window manager integration
    int app_id;              // WM app registration ID
    int dock_index;          // Dock/taskbar index

    // Phase J2: PTY-backed shell (msh running on /dev/pts/N)
    struct file *ptmx;       // master side; keystrokes written here, output read here
    int          child_pid;  // pid of the spawned /APPS/MSH
    int          pump_pid;   // pid of the kernel pump thread
    int          slave_idx;  // pts index (N in /dev/pts/N)
    volatile int pump_running;
} terminal_t;

// Create and show a terminal window
terminal_t *terminal_create(void);

// Destroy terminal
void terminal_destroy(terminal_t *term);

// Run terminal main loop (returns when closed)
void terminal_run(terminal_t *term);

// Print string to terminal
void terminal_print(terminal_t *term, const char *str);

// Print character to terminal
void terminal_putc(terminal_t *term, char c);

// Clear terminal
void terminal_clear(terminal_t *term);

// Draw terminal content
void terminal_draw(terminal_t *term);

// Launch callback for dock (non-blocking, registers with WM)
void terminal_launch(void);

// ============================================================================
// Window Manager Callbacks (internal use)
// ============================================================================

void terminal_on_event(void *app_data, gui_event_t *event);
void terminal_on_draw(void *app_data);
void terminal_on_destroy(void *app_data);

#endif // TERMINAL_H
