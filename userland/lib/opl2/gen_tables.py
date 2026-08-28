#!/usr/bin/env python3
"""#182: generate the OPL2 log-sine and exponent tables from their FORMULAS.

WHY A GENERATOR AND NOT A PASTED TABLE
--------------------------------------
Both tables are pure functions of an index. Pasting 512 magic numbers into a
source file would make them unverifiable by inspection, unattributable, and
indistinguishable from numbers copied out of somebody else's emulator. Deriving
them from the published formulas makes the derivation the artifact: anyone can
re-run this script and diff, which is exactly what tools/opl-table-gate.sh does
in CI. A table that cannot drift from its formula cannot be silently wrong.

THE FORMULAS (public, from the YM3812/YMF262 documentation and the
"OPLx decapsulated" die analysis by Gambrell and Niemitalo):

  logsin[i] = round(-log2(sin((i + 0.5) * pi / 512)) * 256)      i in 0..255
  exp[i]    = round((2 ** (i / 256.0) - 1) * 1024)               i in 0..255

logsin holds a QUARTER of a sine wave in the log domain: 256 entries covering
0..pi/2, at a resolution of 1/256 of a log2 unit. The other three quarters are
recovered by index reflection and a sign flip, which is what the hardware does
and why the ROM is only a quarter wave.

exp is the inverse: it turns a log-domain attenuation back into a linear
amplitude. It stores 2**x - 1 rather than 2**x so the value fits in 10 bits;
the implicit leading 1 is re-added as | 0x400 at lookup time.

ANCHORS, checked below so a bad edit fails here rather than sounding wrong:
  logsin[0]   = 2137   (sin of half a step: the quietest point of the quarter)
  logsin[255] = 0      (the peak of the sine: no attenuation)
  exp[0]      = 0      (2**0 - 1 = 0)
  exp[255]    = 1018   (2**(255/256) - 1 = 0.99458..., times 1024)
"""

import math
import sys

def logsin_table():
    t = []
    for i in range(256):
        s = math.sin((i + 0.5) * math.pi / 512.0)
        t.append(int(round(-math.log2(s) * 256.0)))
    return t

def exp_table():
    return [int(round((2.0 ** (i / 256.0) - 1.0) * 1024.0)) for i in range(256)]

def fmt(name, vals, width):
    out = ["pub static %s: [u16; 256] = [" % name]
    for r in range(0, 256, 8):
        row = ", ".join("%*d" % (width, v) for v in vals[r:r + 8])
        out.append("    %s," % row)
    out.append("];")
    return "\n".join(out)

def main():
    ls = logsin_table()
    ex = exp_table()

    # Anchors. A generator that cannot fail is not a check.
    assert ls[0] == 2137, "logsin[0] = %d, expected 2137" % ls[0]
    assert ls[255] == 0, "logsin[255] = %d, expected 0" % ls[255]
    assert ex[0] == 0, "exp[0] = %d, expected 0" % ex[0]
    assert ex[255] == 1018, "exp[255] = %d, expected 1018" % ex[255]
    assert all(0 <= v <= 0xFFFF for v in ls + ex)
    # logsin must be monotonically NON-INCREASING: the quarter wave rises, so
    # its attenuation falls. A table that is not monotone is a corrupted table.
    assert all(ls[i] >= ls[i + 1] for i in range(255)), "logsin not monotone"
    assert all(ex[i] <= ex[i + 1] for i in range(255)), "exp not monotone"

    hdr = """// GENERATED FILE. DO NOT EDIT BY HAND.
//
// Regenerate with:   python3 userland/lib/opl2/gen_tables.py > userland/lib/opl2/opl2_tables.rs
// Verified in CI by: userland/lib/opl2/table-gate.sh
//
// #182: the YM3812 (OPL2) log-sine and exponent ROMs, derived from their
// published formulas rather than copied from any implementation. See
// gen_tables.py for the formulas, the anchor values and why this is a
// generator instead of a pasted table.
"""
    sys.stdout.write(hdr)
    sys.stdout.write("\n")
    sys.stdout.write("// logsin[i] = round(-log2(sin((i + 0.5) * pi / 512)) * 256)\n")
    sys.stdout.write(fmt("LOGSIN", ls, 4))
    sys.stdout.write("\n\n")
    sys.stdout.write("// exp[i] = round((2 ** (i / 256) - 1) * 1024)\n")
    sys.stdout.write(fmt("EXPTAB", ex, 4))
    sys.stdout.write("\n")

if __name__ == "__main__":
    main()
