// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// termseq - emit terminal escape sequences ONE AT A TIME, labelled, so a
// screenshot is evidence about a specific sequence.
//
// WHY: the tier-1 parity pass could not claim insert/delete-line because it had
// only ever been exercised through vi. "vi looked right" is evidence about vi.
// This app is the other half of the evidence the host test cannot give: the
// host test proves the PARSER sees the sequence correctly, this proves the GRID
// does the right thing with it, on the real terminal, on real hardware paths.
//
// Every case draws a known pattern, applies exactly one sequence, and prints
// the expected result next to it. A screenshot either matches or does not.
//
//   termseq        page 1: rendition, colour, UTF-8, and the leak set
//   termseq 2      page 2: cursor motion, erasure, insert/delete, scrolling
//   termseq 3      page 3: mouse reporting (interactive)

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/termios.h"
#include "../../libc/sys/ioctl.h"

static void w(const char *s) { write(1, s, strlen(s)); }

// Move to (row,col), both 1-based, with CUP.
static void at(int r, int c) {
    char b[32];
    snprintf(b, sizeof(b), "\033[%d;%dH", r, c);
    w(b);
}

static void page1(void) {
    w("\033[2J\033[H");
    w("\033[1mTERMSEQ page 1: rendition, colour, UTF-8, leaks\033[0m\r\n");

    // ---- SGR attributes: each on its own, so one broken attribute is obvious
    w("\r\nSGR  ");
    w("\033[0mnormal \033[1mbold\033[22m ");
    w("\033[2mdim\033[22m ");
    w("\033[3mitalic\033[23m ");
    w("\033[4munderline\033[24m ");
    w("\033[7mreverse\033[27m ");
    w("\033[9mstrike\033[29m");
    w("\033[0m\r\n");

    // ---- bold must NOT eat a following colour (the old `fg |= 8` bug)
    w("\r\nBOLD+COL  \033[1;31mbold red\033[0m  \033[1m\033[31mbold then red\033[0m");
    w("   (both must be BOLD and RED)\r\n");

    // ---- 256 colour: an indexed ramp. Any flat band = 38;5;n ignored.
    w("\r\n256  ");
    for (int i = 16; i < 52; i++) { char b[24]; snprintf(b, sizeof(b), "\033[48;5;%dm ", i); w(b); }
    w("\033[0m\r\n");
    w("     ");
    for (int i = 232; i < 256; i++) { char b[24]; snprintf(b, sizeof(b), "\033[48;5;%dm ", i); w(b); }
    w("\033[0m  (cube, then grey ramp)\r\n");

    // ---- true colour: a smooth gradient. Banding into 16 steps = not real.
    w("\r\nRGB  ");
    for (int i = 0; i < 48; i++) {
        char b[32];
        snprintf(b, sizeof(b), "\033[48;2;%d;%d;%dm ", i * 5, 255 - i * 5, 128);
        w(b);
    }
    w("\033[0m\r\n");
    w("RGB: ");
    for (int i = 0; i < 48; i++) {
        char b[32];
        snprintf(b, sizeof(b), "\033[48:2:%d:%d:%dm ", 255 - i * 5, i * 5, 64);
        w(b);
    }
    w("\033[0m  (colon form)\r\n");

    // ---- 39/49: restore the DEFAULT, not "white on black"
    w("\r\n39/49  \033[31;44mred on blue\033[39m default-fg\033[49m default-bg\033[0m\r\n");

    // ---- the leak set. Each prints [ then a sequence then ]. Correct output
    //      is "[]" with NOTHING between the brackets.
    w("\r\nLEAKS (each must read exactly []):\r\n");
    w("  OSC title   [\033]0;termseq title\007]\r\n");
    w("  OSC ST      [\033]2;termseq title\033\\]\r\n");
    w("  ESC ( B     [\033(B]\r\n");
    w("  CSI >4;2m   [\033[>4;2m]\r\n");
    w("  CSI SP q    [\033[5 q]\r\n");
    w("  CSI ! p     [\033[!p]\r\n");
    w("  CSI ?1049$p [\033[?1049$p]\r\n");
    w("  DCS         [\033P1$r0m\033\\]\r\n");
    w("  APC         [\033_Gaction=x;payload\033\\]\r\n");

    // ---- UTF-8
    w("\r\nUTF-8  latin: \xc3\xa9\xc3\xa8\xc3\xaa\xc3\xbc\xc3\xb1  box: \xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90 \xe2\x94\x82 \xe2\x94\x94\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98  arrows: \xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93\r\n");
    w("       combining: e+U+0301 = e\xcc\x81   a+U+0308 = a\xcc\x88   (must be 1 cell each)\r\n");

    // ---- wide characters must occupy exactly two cells. The ruler below the
    //      CJK line is the proof: column 1 of the ruler must sit under the
    //      first half of the first ideograph, and the '|' must line up.
    w("\r\nWIDE   |\xe4\xb8\x96\xe7\x95\x8c\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf|  <- 7 wide chars = 14 cells\r\n");
    w("       |--------------|  <- 14 dashes; the two | must align\r\n");

    // NOT at(24,1): a maximized window is taller than 24 rows, so an absolute
    // CUP to row 24 lands in the middle of the output and overwrites it.
    w("\r\n\033[7m page 1/4  -- also: termseq 2 (grid), 3 (mouse), 4 (size) \033[0m\r\n");
}

// Draw a 20-char ruler on `row` starting at column `col`.
static void ruler(int row, int col, const char *pat) {
    at(row, col);
    w(pat);
}

static void page2(void) {
    w("\033[2J\033[H");
    w("\033[1mTERMSEQ page 2: cursor motion, erasure, insert/delete, scroll\033[0m");

    // Each case: draw ABCDEFGHIJKLMNOPQRST at column 10, park the cursor,
    // apply ONE sequence, and label the expected result at column 40.
    const char *P = "ABCDEFGHIJKLMNOPQRST";

    // --- EL (erase in line) with all three parameters. The old parser IGNORED
    //     this parameter: 1K and 2K both behaved like 0K.
    ruler(3, 10, P);  at(3, 20); w("\033[0K");   at(3, 40); w("EL 0  expect ABCDEFGHIJ + blanks");
    ruler(4, 10, P);  at(4, 20); w("\033[1K");   at(4, 40); w("EL 1  expect blanks + KLMNOPQRST");
    ruler(5, 10, P);  at(5, 20); w("\033[2K");   at(5, 40); w("EL 2  expect the WHOLE line blank");

    // --- ECH: erase in place, no shifting.
    ruler(6, 10, P);  at(6, 15); w("\033[5X");   at(6, 40); w("ECH 5 expect ABCDE_____KLMNOPQRST");

    // --- ICH / DCH
    ruler(7, 10, P);  at(7, 15); w("\033[3@");   at(7, 40); w("ICH 3 expect ABCDE___FGHIJKLMNOPQ");
    ruler(8, 10, P);  at(8, 15); w("\033[3P");   at(8, 40); w("DCH 3 expect ABCDEIJKLMNOPQRST");

    // --- CHA / VPA: absolute positioning. Absent before; the cursor did not
    //     move and the next write landed in the wrong column.
    ruler(9, 10, P);  at(9, 1); w("\033[15G*");  at(9, 40); w("CHA 15 expect * at column 15 (on the F)");
    at(10, 60); w("\033[11d\033[10G#");          at(10, 40); w("VPA 11 expect # on the NEXT row, col 10");

    // --- REP
    at(12, 10); w("x\033[9b");                   at(12, 40); w("REP 9  expect ten x");

    // --- CBT / CHT (tab stops)
    at(13, 1); w("\033[3I|");                    at(13, 40); w("CHT 3  expect | at column 25");

    // --- IL / DL inside a scroll region. The tier-1 pass shipped these but
    //     could never show them in isolation.
    at(15, 1); w("\033[16;19r");                 // scroll region rows 16-19
    at(16, 1); w("R1 line one");
    at(17, 1); w("R2 line two");
    at(18, 1); w("R3 line three");
    at(19, 1); w("R4 line four");
    at(17, 1); w("\033[L");                      // IL at row 17
    at(15, 40); w("IL 1 in region 16-19:");
    at(16, 40); w("expect R1, blank, R2, R3");
    at(20, 1); w("\033[r");                      // release the region

    // --- SU / SD
    at(21, 1); w("\033[22;23r");
    at(22, 1); w("SU/SD row A");
    at(23, 1); w("SU/SD row B");
    at(22, 1); w("\033[S");                      // scroll region up one
    at(21, 40); w("SU 1 in region 22-23: expect row B then blank");
    w("\033[r");

    at(25, 1);
    w("\033[7m page 2/4 \033[0m\r\n");
}

static void page3(void) {
    w("\033[2J\033[H");
    w("\033[1mTERMSEQ page 3: mouse reporting\033[0m\r\n\r\n");
    w("Enabling ?1002 (button + drag) with ?1006 (SGR encoding).\r\n");
    w("Click and drag anywhere in the window. Each report is printed below\r\n");
    w("as it arrives. Correct output looks like:  <0;12;7M  ... <0;12;7m\r\n");
    w("Press q to finish (mouse reporting is turned back off on exit).\r\n\r\n");
    w("\033[?1002h\033[?1006h");

    char b[64];
    int shown = 0;
    for (;;) {
        int n = read(0, b, sizeof(b));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (b[i] == 'q' || b[i] == 'Q') { w("\033[?1002l\033[?1006l\r\n\r\ndone.\r\n"); return; }
            // Print the report bytes visibly: ESC as a caret so the terminal
            // does not re-interpret what it just sent us.
            char o[8];
            if ((unsigned char)b[i] == 0x1B) { o[0] = '^'; o[1] = 0; }
            else { o[0] = b[i]; o[1] = 0; }
            w(o);
        }
        if (++shown > 40) { w("\r\n"); shown = 0; }
    }
}

// Page 4: WHAT SIZE DOES A CHILD THINK THE TERMINAL IS?
//
// busybox vi asks TIOCGWINSZ on STDIN (the pty SLAVE) and, only if that fails,
// falls back to CUP-to-999;999 + CPR. If the two disagree - or if TIOCGWINSZ on
// the slave reports a size the terminal never set - vi writes `columns`
// characters per line into a grid of a different width, every line wraps into
// the next, and the content area ends up blank or scrambled. That is exactly
// what the shipped terminal does today. This page prints both numbers so the
// disagreement is a measurement, not a theory.
static void page4(void) {
    w("\033[2J\033[H");
    w("\033[1mTERMSEQ page 4: what size does a CHILD think this terminal is?\033[0m\r\n\r\n");

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    int r = ioctl(0, TIOCGWINSZ, &ws);
    char b[160];
    snprintf(b, sizeof(b), "  TIOCGWINSZ(stdin)  rc=%d  rows=%u cols=%u\r\n",
             r, (unsigned)ws.ws_row, (unsigned)ws.ws_col);
    w(b);

    memset(&ws, 0, sizeof(ws));
    r = ioctl(1, TIOCGWINSZ, &ws);
    snprintf(b, sizeof(b), "  TIOCGWINSZ(stdout) rc=%d  rows=%u cols=%u\r\n",
             r, (unsigned)ws.ws_row, (unsigned)ws.ws_col);
    w(b);

    // CUP to an impossible position, then CPR. A correct terminal clamps and
    // reports its real last row/column.
    w("\033[999;999H\033[6n");
    char rb[64];
    int n = read(0, rb, sizeof(rb) - 1);
    w("\033[3;1H");
    if (n > 0) {
        rb[n] = 0;
        // print it with ESC shown as ^ so the terminal does not re-parse it
        for (int i = 0; i < n; i++) if ((unsigned char)rb[i] == 0x1B) rb[i] = '^';
        snprintf(b, sizeof(b), "  CPR reply        \"%s\"  (expect ^[<rows>;<cols>R)\r\n", rb);
    } else {
        snprintf(b, sizeof(b), "  CPR reply        NONE (read returned %d)\r\n", n);
    }
    w("\033[4;1H"); w(b);
    w("\r\n  If these two disagree, a full-screen program draws lines of the WRONG\r\n");
    w("  width and the content area wraps into itself. That is the vi symptom.\r\n");
    w("\r\n\033[7m page 4/4 \033[0m\r\n");
}

int main(int argc, char **argv) {
    int page = 1;
    if (argc > 1 && argv[1]) page = argv[1][0] - '0';
    if (page == 2) page2();
    else if (page == 3) page3();
    else if (page == 4) page4();
    else page1();
    return 0;
}
