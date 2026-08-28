// term_parse.h
// The ANSI/escape front end. The BYTE-LEVEL state machine, the UTF-8 decoder,
// wcwidth, the SGR grammar and the DEC private-mode state live in
// term_emu.{c,h}, which makes no syscalls and includes no GUI header and is
// therefore driven directly by a host unit test (tests/run.sh, 100 assertions).
// What lives HERE is what those parse results MEAN to this terminal's grid:
// cursor motion, erasure, scrolling, modes and replies.
//
// The old STATE_NORMAL/STATE_ESCAPE/STATE_CSI machine this header used to
// export is GONE, and so are escape_state/escape_params/csi_private/
// handle_escape_char. It was not replaced like for like: it treated any byte it
// did not recognise as a FINAL byte, so every unimplemented sequence PRINTED
// THE REST OF ITSELF on the screen. See docs/TERMINAL_EMULATION.md.
#ifndef TERM_PARSE_H
#define TERM_PARSE_H

#include "term_common.h"
#include "term_grid.h"
#include "term_emu.h"

// The parser instance for the ACTIVE pane. Banked per pane by term_layout.c,
// the same way the grid and cursor are: a half-received escape sequence belongs
// to the pane whose child sent it, and must not be completed by the next pane's
// output.
extern term_parser_t g_parser;

// Feed bytes from the child (or this app's own messages) through the emulator
// into the grid. Everything the terminal displays goes through here, so there
// is exactly ONE interpretation of a sequence in the program.
void term_feed(const unsigned char *b, int n);
void term_putc(char c);
void term_puts(const char *str);
void term_put_int(int num);

// Write a reply on the foreground child's pty master (DSR/CPR, DA, DECRQM, and
// mouse reports). Silently dropped when no child owns the terminal.
void term_reply(const char *b, int n);

// DECSTR (CSI ! p). Distinct from RIS: it does not clear the screen, which is
// the whole reason a program uses it.
void term_soft_reset(void);

// The last title an application asked for via OSC 0/2. RECORDED, NOT APPLIED:
// window chrome for a kernel-WM window is drawn by the kernel and there is no
// set-title syscall for it. The valuable half of OSC support is that the title
// no longer prints itself onto the prompt, and that half is real.
extern char g_osc_title[128];

#endif // TERM_PARSE_H
