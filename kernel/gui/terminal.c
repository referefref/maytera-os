// terminal.c - XTerm-compatible GUI Terminal for MayteraOS
// Supports ANSI escape sequences and full shell commands
#include "terminal.h"
#include "window.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../cpu/isr.h"
#include "../cpu/idt.h"
#include "../drivers/mouse.h"
#include "../fs/fat.h"
#include "../boot_info.h"
#include "../exec/pe.h"
#include "../exec/elf.h"
#include "../proc/process.h"
#include "desktop.h"
#include "syslog.h"
#include "../net/wget.h"
#include "../net/ftp.h"

// Global terminal for launch callback
static terminal_t *g_active_terminal = NULL;

// ANSI color table
static const uint32_t ansi_colors[16] = {
    TERM_COLOR_BLACK,        TERM_COLOR_RED,
    TERM_COLOR_GREEN,        TERM_COLOR_YELLOW,
    TERM_COLOR_BLUE,         TERM_COLOR_MAGENTA,
    TERM_COLOR_CYAN,         TERM_COLOR_WHITE,
    TERM_COLOR_BRIGHT_BLACK, TERM_COLOR_BRIGHT_RED,
    TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BRIGHT_YELLOW,
    TERM_COLOR_BRIGHT_BLUE,  TERM_COLOR_BRIGHT_MAGENTA,
    TERM_COLOR_BRIGHT_CYAN,  TERM_COLOR_BRIGHT_WHITE
};

// Draw character at pixel coordinates with color
static void term_draw_char_at(int32_t x, int32_t y, char c, uint32_t color) {
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < TERM_CHAR_H; r++) {
                uint8_t bits = glyph[r];
                for (int col_bit = 0; col_bit < FONT_WIDTH; col_bit++) {
                    if (bits & (0x80 >> col_bit)) {
                        fb_put_pixel(x + col_bit, y + r, color & 0xFFFFFF);
                    }
                }
            }
        }
    }
}

// Draw a single character at terminal position with attributes
static void term_draw_char(terminal_t *term, int col, int row, char c, uint8_t fg_idx, uint8_t bg_idx) {
    if (!term || !term->window) return;

    // Get window content area
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(term->window, &wx, &wy, &ww, &wh);

    int32_t x = wx + col * TERM_CHAR_W;
    int32_t y = wy + row * TERM_CHAR_H;

    // Get colors
    uint32_t fg = ansi_colors[fg_idx & 0x0F];
    uint32_t bg = ansi_colors[bg_idx & 0x0F];
    if (bg_idx == 0) bg = TERM_BG_COLOR;  // Use default dark background

    // Draw background
    fb_fill_rect(x, y, TERM_CHAR_W, TERM_CHAR_H, bg & 0xFFFFFF);

    // Draw character
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < TERM_CHAR_H; r++) {
                uint8_t bits = glyph[r];
                for (int col_bit = 0; col_bit < FONT_WIDTH; col_bit++) {
                    if (bits & (0x80 >> col_bit)) {
                        fb_put_pixel(x + col_bit, y + r, fg & 0xFFFFFF);
                    }
                }
            }
        }
    }
}

// Draw entire terminal buffer
static void term_redraw(terminal_t *term) {
    if (!term || !term->window) return;

    // Get window content area and fill with background
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(term->window, &wx, &wy, &ww, &wh);
    fb_fill_rect(wx, wy, ww, wh, TERM_BG_COLOR & 0xFFFFFF);

    int scroll = term->scroll_offset;

    // Draw each cell
    for (int row = 0; row < TERM_ROWS; row++) {
        term_cell_t *cell_row;

        if (scroll > 0 && row < scroll && term->scrollback) {
            // Draw from scrollback buffer
            // scrollback_pos points to next write position
            // We want to go back (scroll - row) lines from the most recent
            int scrollback_idx = term->scrollback_pos - scroll + row;
            if (scrollback_idx < 0) {
                scrollback_idx += TERM_TOTAL_LINES;
            }
            scrollback_idx = scrollback_idx % TERM_TOTAL_LINES;
            cell_row = &term->scrollback[scrollback_idx * TERM_COLS];
        } else {
            // Draw from current cells
            int cells_row = row - scroll;
            if (cells_row < 0) cells_row = 0;
            if (cells_row >= TERM_ROWS) cells_row = TERM_ROWS - 1;
            cell_row = term->cells[cells_row];
        }

        for (int col = 0; col < TERM_COLS; col++) {
            term_cell_t *cell = &cell_row[col];
            if (cell->ch != '\0' && cell->ch != ' ') {
                term_draw_char(term, col, row, cell->ch, cell->fg, cell->bg);
            } else if (cell->ch == ' ' && cell->bg != 0) {
                // Draw colored space
                term_draw_char(term, col, row, ' ', cell->fg, cell->bg);
            }
        }
    }

    // Draw cursor only when not scrolled back
    if (term->cursor_visible && term->scroll_offset == 0) {
        int32_t cx = wx + term->cursor_x * TERM_CHAR_W;
        int32_t cy = wy + term->cursor_y * TERM_CHAR_H;
        fb_fill_rect(cx, cy + TERM_CHAR_H - 2, TERM_CHAR_W, 2, TERM_CURSOR_COLOR & 0xFFFFFF);
    }

    // Draw scroll indicator if scrolled back
    if (term->scroll_offset > 0) {
        // Draw a small indicator showing we're in scrollback mode
        char indicator[32];
        int offset = term->scroll_offset;
        // Simple integer to string
        int len = 0;
        char tmp[16];
        int n = offset;
        do { tmp[len++] = '0' + (n % 10); n /= 10; } while (n > 0);
        indicator[0] = '[';
        for (int i = 0; i < len; i++) indicator[1 + i] = tmp[len - 1 - i];
        indicator[1 + len] = ']';
        indicator[2 + len] = '\0';

        // Draw at top-right of content area
        int ind_len = 2 + len;
        int ind_x = wx + ww - ind_len * TERM_CHAR_W - 4;
        int ind_y = wy + 2;
        fb_fill_rect(ind_x - 2, ind_y, ind_len * TERM_CHAR_W + 4, TERM_CHAR_H, 0x404040);
        for (int i = 0; i < ind_len; i++) {
            term_draw_char_at(ind_x + i * TERM_CHAR_W, ind_y, indicator[i], 0x00FFFF);
        }
    }
}

// Scroll terminal up one line
static void term_scroll(terminal_t *term) {
    // Save top line to scrollback buffer before scrolling
    if (term->scrollback) {
        memcpy(&term->scrollback[term->scrollback_pos * TERM_COLS],
               term->cells[0], sizeof(term_cell_t) * TERM_COLS);
        term->scrollback_pos = (term->scrollback_pos + 1) % TERM_TOTAL_LINES;
        if (term->scrollback_count < TERM_TOTAL_LINES) {
            term->scrollback_count++;
        }
    }

    // Move all cells up
    for (int row = 0; row < TERM_ROWS - 1; row++) {
        memcpy(term->cells[row], term->cells[row + 1], sizeof(term_cell_t) * TERM_COLS);
        memcpy(term->buffer[row], term->buffer[row + 1], TERM_COLS + 1);
    }
    // Clear last line
    memset(term->cells[TERM_ROWS - 1], 0, sizeof(term_cell_t) * TERM_COLS);
    memset(term->buffer[TERM_ROWS - 1], 0, TERM_COLS + 1);

    // Reset scroll offset when new content appears
    term->scroll_offset = 0;
}

// Scroll terminal view (positive = scroll back into history, negative = scroll forward)
static void term_scroll_view(terminal_t *term, int delta) {
    if (!term || !term->scrollback) return;

    int new_offset = term->scroll_offset + delta;

    // Clamp to valid range
    int max_scroll = term->scrollback_count > TERM_ROWS
                     ? term->scrollback_count - TERM_ROWS
                     : 0;
    if (new_offset < 0) new_offset = 0;
    if (new_offset > max_scroll) new_offset = max_scroll;

    if (new_offset != term->scroll_offset) {
        term->scroll_offset = new_offset;
        // Redraw will happen automatically
    }
}

// Reset terminal attributes
static void term_reset_attrs(terminal_t *term) {
    term->current_fg = 7;   // White
    term->current_bg = 0;   // Black
    term->current_attr = TERM_ATTR_NORMAL;
}

// Process ANSI CSI sequence (ESC [ ... )
static void term_process_csi(terminal_t *term, char final_char) {
    int *params = term->escape_params;
    int count = term->escape_param_count;

    switch (final_char) {
        case 'A':  // Cursor up
            term->cursor_y -= (count > 0 && params[0] > 0) ? params[0] : 1;
            if (term->cursor_y < 0) term->cursor_y = 0;
            break;

        case 'B':  // Cursor down
            term->cursor_y += (count > 0 && params[0] > 0) ? params[0] : 1;
            if (term->cursor_y >= TERM_ROWS) term->cursor_y = TERM_ROWS - 1;
            break;

        case 'C':  // Cursor forward
            term->cursor_x += (count > 0 && params[0] > 0) ? params[0] : 1;
            if (term->cursor_x >= TERM_COLS) term->cursor_x = TERM_COLS - 1;
            break;

        case 'D':  // Cursor backward
            term->cursor_x -= (count > 0 && params[0] > 0) ? params[0] : 1;
            if (term->cursor_x < 0) term->cursor_x = 0;
            break;

        case 'H':  // Cursor position (row;col)
        case 'f':
            term->cursor_y = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
            term->cursor_x = (count > 1 && params[1] > 0) ? params[1] - 1 : 0;
            if (term->cursor_y >= TERM_ROWS) term->cursor_y = TERM_ROWS - 1;
            if (term->cursor_x >= TERM_COLS) term->cursor_x = TERM_COLS - 1;
            break;

        case 'J':  // Erase in display
            {
                int mode = (count > 0) ? params[0] : 0;
                if (mode == 0) {
                    // Erase from cursor to end
                    for (int col = term->cursor_x; col < TERM_COLS; col++) {
                        term->cells[term->cursor_y][col].ch = ' ';
                    }
                    for (int row = term->cursor_y + 1; row < TERM_ROWS; row++) {
                        memset(term->cells[row], 0, sizeof(term_cell_t) * TERM_COLS);
                    }
                } else if (mode == 1) {
                    // Erase from start to cursor
                    for (int row = 0; row < term->cursor_y; row++) {
                        memset(term->cells[row], 0, sizeof(term_cell_t) * TERM_COLS);
                    }
                    for (int col = 0; col <= term->cursor_x; col++) {
                        term->cells[term->cursor_y][col].ch = ' ';
                    }
                } else if (mode == 2) {
                    // Erase entire display
                    for (int row = 0; row < TERM_ROWS; row++) {
                        memset(term->cells[row], 0, sizeof(term_cell_t) * TERM_COLS);
                    }
                }
            }
            break;

        case 'K':  // Erase in line
            {
                int mode = (count > 0) ? params[0] : 0;
                if (mode == 0) {
                    // Erase from cursor to end of line
                    for (int col = term->cursor_x; col < TERM_COLS; col++) {
                        term->cells[term->cursor_y][col].ch = ' ';
                    }
                } else if (mode == 1) {
                    // Erase from start to cursor
                    for (int col = 0; col <= term->cursor_x; col++) {
                        term->cells[term->cursor_y][col].ch = ' ';
                    }
                } else if (mode == 2) {
                    // Erase entire line
                    memset(term->cells[term->cursor_y], 0, sizeof(term_cell_t) * TERM_COLS);
                }
            }
            break;

        case 'm':  // SGR - Set Graphics Rendition
            if (count == 0) {
                term_reset_attrs(term);
            } else {
                for (int i = 0; i < count; i++) {
                    int p = params[i];
                    if (p == 0) {
                        term_reset_attrs(term);
                    } else if (p == 1) {
                        term->current_attr |= TERM_ATTR_BOLD;
                        if (term->current_fg < 8) term->current_fg += 8; // Bright
                    } else if (p == 4) {
                        term->current_attr |= TERM_ATTR_UNDERLINE;
                    } else if (p == 7) {
                        term->current_attr |= TERM_ATTR_INVERSE;
                    } else if (p >= 30 && p <= 37) {
                        term->current_fg = p - 30;
                        if (term->current_attr & TERM_ATTR_BOLD) term->current_fg += 8;
                    } else if (p == 39) {
                        term->current_fg = 7;  // Default foreground
                    } else if (p >= 40 && p <= 47) {
                        term->current_bg = p - 40;
                    } else if (p == 49) {
                        term->current_bg = 0;  // Default background
                    } else if (p >= 90 && p <= 97) {
                        term->current_fg = p - 90 + 8;  // Bright foreground
                    } else if (p >= 100 && p <= 107) {
                        term->current_bg = p - 100 + 8; // Bright background
                    }
                }
            }
            break;

        case 's':  // Save cursor position
            // TODO: implement
            break;

        case 'u':  // Restore cursor position
            // TODO: implement
            break;

        default:
            // Unknown sequence
            break;
    }
}

// Process a single character with ANSI support
void terminal_putc(terminal_t *term, char c) {
    if (!term) return;

    // Handle escape sequences
    if (term->escape_state == TERM_STATE_ESCAPE) {
        if (c == '[') {
            term->escape_state = TERM_STATE_CSI;
            term->escape_param_count = 0;
            memset(term->escape_params, 0, sizeof(term->escape_params));
            return;
        } else if (c == ']') {
            term->escape_state = TERM_STATE_OSC;
            return;
        } else {
            term->escape_state = TERM_STATE_NORMAL;
            // Fall through to normal processing
        }
    }

    if (term->escape_state == TERM_STATE_CSI) {
        if (c >= '0' && c <= '9') {
            // Build parameter
            int idx = term->escape_param_count;
            if (idx >= TERM_MAX_PARAMS) idx = TERM_MAX_PARAMS - 1;
            term->escape_params[idx] = term->escape_params[idx] * 10 + (c - '0');
            return;
        } else if (c == ';') {
            // Next parameter
            if (term->escape_param_count < TERM_MAX_PARAMS - 1) {
                term->escape_param_count++;
            }
            return;
        } else if (c >= 0x40 && c <= 0x7E) {
            // Final character
            if (term->escape_param_count < TERM_MAX_PARAMS) {
                term->escape_param_count++;
            }
            term_process_csi(term, c);
            term->escape_state = TERM_STATE_NORMAL;
            return;
        }
        return;
    }

    if (term->escape_state == TERM_STATE_OSC) {
        if (c == '\007' || c == '\033') {  // BEL or ESC ends OSC
            term->escape_state = TERM_STATE_NORMAL;
        }
        return;
    }

    // Normal character processing
    if (c == '\033') {  // ESC
        term->escape_state = TERM_STATE_ESCAPE;
        return;
    }

    if (c == '\n') {
        term->cursor_x = 0;
        term->cursor_y++;
    } else if (c == '\r') {
        term->cursor_x = 0;
    } else if (c == '\b') {
        if (term->cursor_x > 0) {
            term->cursor_x--;
            term->cells[term->cursor_y][term->cursor_x].ch = ' ';
            term->buffer[term->cursor_y][term->cursor_x] = ' ';
        }
    } else if (c == '\t') {
        // Tab to next 8-column boundary
        int next = (term->cursor_x + 8) & ~7;
        if (next >= TERM_COLS) next = TERM_COLS - 1;
        term->cursor_x = next;
    } else if (c >= ' ' && c < 127) {
        if (term->cursor_x < TERM_COLS) {
            term->cells[term->cursor_y][term->cursor_x].ch = c;
            term->cells[term->cursor_y][term->cursor_x].fg = term->current_fg;
            term->cells[term->cursor_y][term->cursor_x].bg = term->current_bg;
            term->cells[term->cursor_y][term->cursor_x].attr = term->current_attr;
            term->buffer[term->cursor_y][term->cursor_x] = c;
            term->cursor_x++;
        }
        if (term->cursor_x >= TERM_COLS) {
            term->cursor_x = 0;
            term->cursor_y++;
        }
    }

    // Handle scrolling
    while (term->cursor_y >= TERM_ROWS) {
        term_scroll(term);
        term->cursor_y--;
    }
}

// Print string to terminal
void terminal_print(terminal_t *term, const char *str) {
    if (!term || !str) return;
    while (*str) {
        terminal_putc(term, *str++);
    }
}

// Print colored string
static void term_print_colored(terminal_t *term, const char *str, int color) {
    uint8_t old_fg = term->current_fg;
    term->current_fg = color;
    terminal_print(term, str);
    term->current_fg = old_fg;
}

// Clear terminal
void terminal_clear(terminal_t *term) {
    if (!term) return;
    memset(term->cells, 0, sizeof(term->cells));
    memset(term->buffer, 0, sizeof(term->buffer));
    term->cursor_x = 0;
    term->cursor_y = 0;
}

// Create and show a terminal window
terminal_t *terminal_create(void) {
    terminal_t *term = (terminal_t *)kmalloc(sizeof(terminal_t));
    if (!term) {
        kprintf("[Term] Failed to allocate terminal\n");
        return NULL;
    }

    memset(term, 0, sizeof(terminal_t));

    // Allocate scrollback buffer
    term->scrollback = (term_cell_t *)kmalloc(sizeof(term_cell_t) * TERM_TOTAL_LINES * TERM_COLS);
    if (term->scrollback) {
        memset(term->scrollback, 0, sizeof(term_cell_t) * TERM_TOTAL_LINES * TERM_COLS);
        term->scrollback_pos = 0;
        term->scrollback_count = 0;
    }
    term->scroll_offset = 0;

    // Calculate window size
    int width = TERM_COLS * TERM_CHAR_W + 10;  // Add padding
    int height = TERM_ROWS * TERM_CHAR_H + TITLEBAR_HEIGHT + 10;

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - width) / 2;
    int y = (screen_h - height) / 2 - 50;

    // Create window
    term->window = window_create("Terminal", x, y, width, height);
    if (!term->window) {
        kprintf("[Term] Failed to create window\n");
        kfree(term);
        return NULL;
    }

    // Set window colors
    term->window->bg_color = TERM_BG_COLOR & 0xFFFFFF;

    term->cursor_x = 0;
    term->cursor_y = 0;
    term->cursor_visible = true;
    term->running = true;
    strcpy(term->cwd, "/");

    // Initialize colors
    term_reset_attrs(term);

    // Phase J2: the shell (/APPS/MSH) draws its own prompt. Show a small
    // banner while it boots so the window isn't blank, then let the pty feed
    // everything else.
    term_print_colored(term, "MayteraOS Terminal v3.0", 10);
    terminal_print(term, " (PTY + msh)\n");
    terminal_print(term, "Starting shell on /dev/pts/...\n");

    term->ptmx = NULL;
    term->child_pid = -1;
    term->pump_pid = -1;
    term->slave_idx = -1;
    term->pump_running = 0;

    return term;
}

// Destroy terminal
void terminal_destroy(terminal_t *term) {
    if (!term) return;

    // Phase J2: tear down the PTY-backed shell. Signal the pump to stop,
    // drop our master ref (fires SIGHUP on the slave fg_pgrp and wakes any
    // slave readers with EOF so msh can exit cleanly), then let the pump
    // thread die on its own on the next tick.
    term->running = false;
    term->pump_running = 0;
    if (term->ptmx) {
        extern void file_put(struct file *f);
        file_put(term->ptmx);
        term->ptmx = NULL;
    }

    if (term->scrollback) {
        kfree(term->scrollback);
        term->scrollback = NULL;
    }
    if (term->window) {
        window_destroy(term->window);
    }
    kfree(term);
}


// Forward declarations for Phase J2 helpers defined later in this file.
static void term_key_to_master(terminal_t *term, int c);
static void terminal_pump_thread(void *arg);
static int  terminal_spawn_shell(terminal_t *term);

// Public draw function
void terminal_draw(terminal_t *term) {
    if (!term || !term->window) return;
    // Frame drawn by wm_draw_all() - only redraw content
    term_redraw(term);
}

// ============================================================================
// Window Manager Callback Functions (non-blocking model)
// ============================================================================

// Event callback - handles events from window manager
void terminal_on_event(void *app_data, gui_event_t *event) {
    terminal_t *term = (terminal_t *)app_data;
    if (!term || !term->window || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            {
                int c = (unsigned char)event->key_char;

                // ESC is a regular byte now - msh and mvi need it. There is
                // no longer a GUI-level close-on-ESC shortcut; use the window
                // close button or Ctrl-D inside the shell.

                // Any typing snaps the view back to the live prompt.
                if (term->scroll_offset != 0) term->scroll_offset = 0;

                term_key_to_master(term, c);

                // Don't invalidate here: the pump thread will invalidate when
                // the pty echoes (cooked mode) or the app responds (raw mode).
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[Term] Close button clicked\n");
            wm_unregister_app(term->app_id);
            if (term->dock_index >= 0) {
                dock_remove_app(term->dock_index);
            }
            if (g_active_terminal == term) {
                g_active_terminal = NULL;
            }
            window_hide(term->window);
            wm_invalidate_all();
            break;

        case EVENT_MOUSE_SCROLL:
            // Scroll through terminal history
            // Positive scroll_delta = scroll up (back in history)
            // Negative scroll_delta = scroll down (forward)
            if (event->scroll_delta != 0) {
                term_scroll_view(term, event->scroll_delta * TERM_SCROLL_LINES);
                wm_invalidate_rect(&term->window->bounds);
            }
            break;

        default:
            break;
    }
}

// Draw callback
void terminal_on_draw(void *app_data) {
    terminal_t *term = (terminal_t *)app_data;
    if (term) {
        terminal_draw(term);
    }
}

// Destroy callback
void terminal_on_destroy(void *app_data) {
    terminal_t *term = (terminal_t *)app_data;
    if (term) {
        kprintf("[Term] Destroying terminal instance\n");
        if (g_active_terminal == term) {
            g_active_terminal = NULL;
        }
        terminal_destroy(term);
    }
}

// ============================================================================
// Phase J2: PTY + shell plumbing
// ============================================================================

// Translate a key_char from EVENT_KEY_DOWN into the byte sequence a POSIX
// terminal app expects, and write it to the pty master. The kernel-internal
// KEY_* codes live in cpu/isr.h; arrows and function keys map to the xterm
// CSI sequences (msh/mvi read these directly).
static void term_key_to_master(terminal_t *term, int c) {
    if (!term || !term->ptmx) return;
    extern int64_t file_write(struct file *f, const void *buf, uint64_t count);

    // Printable ASCII and common control bytes go through unchanged. The
    // ldisc (in cooked mode) handles erase/kill; in raw mode msh/mvi get the
    // bytes verbatim.
    if (c >= 1 && c < 0x80) {
        uint8_t b = (uint8_t)c;
        // Window manager passes Enter as '\n'; most shells want '\r' on TTY.
        if (b == '\n') b = '\r';
        file_write(term->ptmx, &b, 1);
        return;
    }

    // Special keys: cpu/isr.h defines KEY_UP=0x80 .. KEY_RSHIFT=0x88 etc.
    const char *seq = NULL;
    switch (c) {
        case 0x80: seq = "\x1b[A"; break;  // KEY_UP
        case 0x81: seq = "\x1b[B"; break;  // KEY_DOWN
        case 0x82: seq = "\x1b[D"; break;  // KEY_LEFT
        case 0x83: seq = "\x1b[C"; break;  // KEY_RIGHT
        default: return;                    // ignore F-keys, modifiers, etc.
    }
    uint64_t n = 0;
    while (seq[n]) n++;
    file_write(term->ptmx, seq, n);
}

// Pump kernel thread: drain pty master output -> cell grid, detect child exit.
// Runs until pump_running is cleared or the child becomes a zombie/unused.
static void terminal_pump_thread(void *arg) {
    terminal_t *term = (terminal_t *)arg;
    if (!term) return;

    extern int64_t file_read(struct file *f, void *buf, uint64_t count);
    extern int     file_poll(struct file *f, int events);
    extern process_t *proc_get(uint32_t pid);

    uint8_t buf[256];
    term->pump_running = 1;

    while (term->pump_running && term->running && term->ptmx) {
        // Check child lifecycle; if msh has died, show a note and stop.
        if (term->child_pid > 0) {
            process_t *child = proc_get((uint32_t)term->child_pid);
            if (!child ||
                child->state == PROC_STATE_ZOMBIE ||
                child->state == PROC_STATE_UNUSED) {
                const char *msg = "\r\n[shell exited]\r\n";
                for (const char *p = msg; *p; p++) terminal_putc(term, *p);
                if (term->window) wm_invalidate_rect(&term->window->bounds);
                term->child_pid = -1;
                break;
            }
        }

        int mp = file_poll(term->ptmx, 0x01 /*POLL_IN*/);
        if (mp & 0x01) {
            int64_t n = file_read(term->ptmx, buf, sizeof(buf));
            if (n > 0) {
                for (int64_t i = 0; i < n; i++) terminal_putc(term, (char)buf[i]);
                if (term->window) wm_invalidate_rect(&term->window->bounds);
                continue;   // drain more if available
            }
            if (n == 0) {
                // Slave closed -> will show up as zombie child next iteration.
            }
        } else if (mp & 0x10 /*POLL_HUP*/) {
            break;
        }

        proc_sleep(15);
    }
    term->pump_running = 0;
}

// Spawn /APPS/MSH on a fresh /dev/pts/N and start the pump thread.
// Returns 0 on success, -1 on failure (terminal stays alive with banner).
static int terminal_spawn_shell(terminal_t *term) {
    extern fat_fs_t g_fat_fs;
    extern struct file *dev_open(const char *name, int flags);
    extern void file_put(struct file *f);
    extern int  file_ioctl(struct file *f, unsigned cmd, void *arg2);

    if (!g_fat_fs.mounted) {
        terminal_print(term, "shell: no filesystem\n");
        return -1;
    }

    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/MSH", &sz);
    if (!data || sz == 0) {
        terminal_print(term, "shell: cannot load /APPS/MSH\n");
        if (data) kfree(data);
        return -1;
    }

    // O_RDWR|O_NONBLOCK on master (matches remote_ctrl's rc_cmd_shell).
    struct file *master = dev_open("ptmx", 0x0002 | 0x0800);
    if (!master) {
        terminal_print(term, "shell: /dev/ptmx unavailable\n");
        kfree(data);
        return -1;
    }

    int pts_idx = -1;
    // TIOCGPTN = 0x80045430
    if (file_ioctl(master, 0x80045430, &pts_idx) != 0 || pts_idx < 0) {
        terminal_print(term, "shell: TIOCGPTN failed\n");
        file_put(master);
        kfree(data);
        return -1;
    }

    int pid = proc_create_user_tty("msh", data, sz, pts_idx);
    kfree(data);
    if (pid < 0) {
        terminal_print(term, "shell: spawn failed\n");
        file_put(master);
        return -1;
    }

    // Apply session UID/GID to spawned shell
    {
        extern uint32_t desktop_get_session_uid(void);
        extern uint32_t desktop_get_session_gid(void);
        extern process_t *proc_get(uint32_t pid);
        process_t *child = proc_get((uint32_t)pid);
        if (child) {
            child->uid  = desktop_get_session_uid();
            child->gid  = desktop_get_session_gid();
            child->euid = desktop_get_session_uid();
            child->egid = desktop_get_session_gid();
        }
    }

    term->ptmx      = master;
    term->child_pid = pid;
    term->slave_idx = pts_idx;

    // Drop the boot banner now that the shell has the surface.
    terminal_clear(term);

    // Spawn the pump as a kernel thread. It takes ownership of draining.
    int pump = proc_create("term_pump", terminal_pump_thread, term,
                           PRIO_NORMAL);
    if (pump < 0) {
        terminal_print(term, "shell: pump thread failed\n");
        file_put(master);
        term->ptmx = NULL;
        return -1;
    }
    term->pump_pid = pump;
    return 0;
}

// Launch callback for dock (non-blocking)
void terminal_launch(void) {
    LOG_INFO("[Terminal] Application launched");
    kprintf("[Term] Launching terminal (non-blocking)...\n");

    terminal_t *term = terminal_create();
    if (!term) {
        LOG_ERROR("[Terminal] Failed to create terminal window");
        kprintf("[Term] Failed to create terminal\n");
        return;
    }

    // Initialize WM integration fields
    term->app_id = -1;
    term->dock_index = -1;

    // Add to taskbar
    term->dock_index = dock_add_app("Terminal", DOCK_ICON_TERMINAL, NULL);

    // Register with window manager
    term->app_id = wm_register_app(
        term->window,
        term,
        terminal_on_event,
        terminal_on_draw,
        terminal_on_destroy
    );

    if (term->app_id < 0) {
        kprintf("[Term] Failed to register with window manager\n");
        if (term->dock_index >= 0) {
            dock_remove_app(term->dock_index);
        }
        terminal_destroy(term);
        return;
    }

    g_active_terminal = term;
    wm_focus_window(term->window);
    wm_invalidate_all();

    kprintf("[Term] Terminal registered as app %d\n", term->app_id);

    // Phase J2: spawn /APPS/MSH on a fresh pty and start pumping bytes.
    if (terminal_spawn_shell(term) != 0) {
        kprintf("[Term] Shell spawn failed; terminal alive but input inert\n");
    } else {
        kprintf("[Term] Shell spawned pid=%d on /dev/pts/%d\n",
                term->child_pid, term->slave_idx);
    }
}
