#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) MayteraOS contributors.
# Full license text: userland/libc/LICENSE (MIT License).
#
# Regenerate tests/fixtures/getopt.c.no-ddash from the live getopt.c.
#
# The fixture is the NEGATIVE CONTROL for tests/run_posixhdr.sh: getopt.c with
# the "--" end-of-options block removed and nothing else touched. It has to be
# regenerated whenever getopt.c changes, or the control drifts away from the
# code it is supposed to be a broken twin of and stops proving anything.
#
# Run it from anywhere; paths are derived from this file's location.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "getopt.c"))
DST = os.path.join(HERE, "getopt.c.no-ddash")

BLOCK = '''        // "--" ends option processing and is itself consumed.
        if (optind != argc && strcmp(av[optind], "--") == 0) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(av);
            else if (first_nonopt == last_nonopt) first_nonopt = optind;
            last_nonopt = argc;
            optind      = argc;
        }
'''

BANNER = '''// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// FIXTURE, NOT SHIPPING CODE. DO NOT LINK THIS INTO ANYTHING.
//
// This is userland/libc/getopt.c with the "--" end-of-options block deleted,
// and nothing else changed. tests/run_posixhdr.sh builds it as the NEGATIVE
// CONTROL: a getopt without that block treats "--" as an unknown option and
// then goes on to parse the words after it, which is how a hand-rolled getopt
// silently eats a filename. The battery must FAIL against this file. If it
// ever passes, the battery has stopped testing the thing it exists to test.
//
// Regenerate with tests/fixtures/regen-getopt-fixture.py after any change to
// getopt.c, or the control stops being a control.
'''

def main():
    with open(SRC) as f:
        s = f.read()
    if BLOCK not in s:
        sys.stderr.write(
            "regen-getopt-fixture.py: the end-of-options block was not found in\n"
            "%s. getopt.c has been restructured, so this script (and the BLOCK\n"
            "text in it) must be updated before the fixture can be trusted.\n" % SRC)
        return 1
    s = s.replace(BLOCK,
                  '        // (fixture: the "--" end-of-options block is deliberately absent)\n')
    s = BANNER + "\n" + s[s.index('#include "getopt.h"'):]
    with open(DST, "w") as f:
        f.write(s)
    print("wrote", DST)
    return 0

if __name__ == "__main__":
    sys.exit(main())
