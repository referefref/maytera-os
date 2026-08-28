#!/usr/bin/env python3
"""Reproduce the SHAPE of the workload the owner reported: an agentic CLI
running inside an SSH session in the MayteraOS Terminal, updating a spinner and
a status block many times a second while streaming output.

It is NOT a mock of any particular program. It emits the three things that make
such a UI expensive for a terminal with no damage tracking, and nothing else:

  1. A SPINNER rewritten ~10 times a second. One or two cells change per tick.
  2. A PERSISTENT STATUS BLOCK pinned below the transcript, erased and redrawn
     on every one of those ticks with cursor-up / erase-line. It is drawn with
     BOX-DRAWING characters, which are non-ASCII: the terminal's non-ASCII path
     goes through win_draw_image(), and that syscall SELF-COMMITS the whole
     window in the kernel. Almost none of the block changes between ticks.
  3. STREAMING TOKENS appended to the transcript, which is the only part that
     legitimately scrolls.

Run under script(1) on a real pty so the byte stream AND its timing are the ones
a pty actually produces. See traces/README.
"""
import sys, time, random

W = 76
BLOCK = 3                       # status lines pinned at the bottom
random.seed(7)

BOX_T = "╭" + "─" * (W - 2) + "╮"
BOX_B = "╰" + "─" * (W - 2) + "╯"
SPIN  = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"

WORDS = ("reading the file and checking whether the parser handles a partial "
         "escape sequence correctly before the next chunk arrives so nothing "
         "is lost across the boundary between two reads of the master side of "
         "the pty which is where this class of bug usually hides").split()


def out(s):
    sys.stdout.write(s)
    sys.stdout.flush()


def erase_block():
    """Cursor rests at column 1 of the block's FIRST line. Clear the block in
    place and come back to it. ESC[B (cursor down), never a newline: at the
    bottom of the screen a newline SCROLLS, and a status block that scrolls the
    transcript once per spinner tick is not what any of these programs do."""
    for i in range(BLOCK):
        out("\033[K")
        if i < BLOCK - 1:
            out("\033[B\r")
    out("\033[%dA\r" % (BLOCK - 1))


def draw_block(frame, tokens):
    """Same contract: starts and ends at column 1 of the block's first line."""
    mid = " %s working   %6d tokens   %5.1fs   esc to interrupt" % (
        SPIN[frame % len(SPIN)], tokens, frame / 10.0)
    mid = mid[:W - 2].ljust(W - 2)
    lines = (BOX_T, "\u2502" + mid + "\u2502", BOX_B)
    for i, ln in enumerate(lines):
        out("\033[K" + ln + "\r")
        if i < BLOCK - 1:
            out("\033[B")
    out("\033[%dA\r" % (BLOCK - 1))


def main():
    out("\033[2J\033[H")
    out("claude> make the terminal repaint only what changed\r\n\r\n")
    # Reserve the block's rows, then park the cursor on the first of them.
    out("\r\n" * (BLOCK - 1))
    out("\033[%dA\r" % (BLOCK - 1))
    draw_block(0, 0)
    tokens = 0
    wi = 0
    for frame in range(300):            # 30 s at 10 Hz
        erase_block()
        if frame % 4 == 0:              # a token burst every ~400 ms
            n = random.randint(3, 9)
            # Printing a transcript line where the block was pushes the block
            # down and, at the bottom of the screen, scrolls. That is exactly
            # what the real thing does, and it is the one part of this workload
            # that damage tracking cannot make cheap.
            out("\033[K" + " ".join(WORDS[(wi + k) % len(WORDS)] for k in range(n)) + "\r\n")
            wi += n
            tokens += n
            out("\r\n" * (BLOCK - 1))
            out("\033[%dA\r" % (BLOCK - 1))
        draw_block(frame, tokens)
        time.sleep(0.1)
    out("\r\n" * BLOCK)
    out("done\r\n")


main()
