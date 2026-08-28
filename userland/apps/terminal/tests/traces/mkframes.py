#!/usr/bin/env python3
"""Turn script(1)'s timing log into the terminal's own FRAME PLAN.

The Terminal drains its pty and repaints once per event-loop tick, and that
tick is 10 ms while any pane has a live child (term_layout_timeout_ms()). So
"how many cells does an update cost" is only meaningful against the byte
batches a 10 ms pump would actually have seen. This reads script's
--log-timing file (delay, bytes per record) and re-buckets it into 10 ms
frames, printing one byte count per frame.

    mkframes.py <timing-file> [tick_ms] > frames.txt

Frames in which no byte arrived are dropped rather than emitted as 0: the app
does repaint on an empty tick (the cursor blink), but that repaint is measured
separately and counting it here would flatter the damage-tracked number by
adding frames that paint nothing.
"""
import sys

def main():
    tm = sys.argv[1]
    tick = float(sys.argv[2]) / 1000.0 if len(sys.argv) > 2 else 0.010
    t = 0.0
    acc = 0
    bucket = 0
    out = []
    for line in open(tm):
        parts = line.split()
        if len(parts) != 2:
            continue
        delay, nbytes = float(parts[0]), int(parts[1])
        t += delay
        b = int(t / tick)
        if b != bucket:
            if acc:
                out.append(acc)
            # Ticks with no data in between are real idle ticks; they are not
            # emitted (see the module docstring).
            bucket = b
            acc = 0
        acc += nbytes
    if acc:
        out.append(acc)
    sys.stdout.write("\n".join(str(v) for v in out) + "\n")

main()
