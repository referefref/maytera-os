# userland/libcompat

Compat translation units SHARED by more than one MayteraOS userland port.

## Why this directory exists

Ports arrive by copy-paste. AssaultCube landed first, OpenArena was started
from it, and both ended up carrying private copies of the same files. Two of
those copies were still BYTE-IDENTICAL when #745 measured them:

    cxxsupp.cpp    114 lines, md5 fcf8c1d9981e1d7f94cb94372e1ffc1b in both
    libc_gap.cpp   117 lines, md5 84af59bdbdc62d70af504810db568207 in both

Byte-identical is the lucky case. The same copy-paste produced files that had
DRIFTED, and the drift cost real debugging time: the two ports also carry a
private zlib_shim.c each, one of which had its one-shot DEFLATE output buffer
sized from a fixed 16x compression-ratio heuristic. A real font atlas
decompresses at about 30.6x, so the decode never succeeded and the read loop
span forever, presenting as a blank white window. That took two debugging
passes to find, the fix landed in ONE copy, and nothing propagated it to the
other. The AssaultCube copy still had the original bug when #745 measured it.

## The rule

NEVER REINVENT A WHEEL INSIDE OUR OWN PROJECT.

- Use what is already here.
- If a shared file is missing something you need, EXTEND THIS COPY. Do not
  fork a private copy back into an app directory.
- Having extended it, GO BACK AND CONFIRM EVERY EXISTING CONSUMER STILL
  BUILDS AND STILL WORKS. An extension that regresses the original consumers
  is a fork with extra steps.

## Contents and consumers

| File | Consumers |
| --- | --- |
| `cxxsupp.cpp` | userland/apps/assaultcube, userland/apps/openarena |
| `libc_gap.cpp` | userland/apps/assaultcube, userland/apps/openarena |

Each consumer compiles these with its OWN flags (AssaultCube at -O2, OpenArena
at -O1), so this directory shares SOURCE, not a prebuilt object or a library.
There is no libcompat.a and there should not be one until two consumers
actually agree on a flag set.

## Deliberately NOT moved here

- `userland/apps/curaslice/cxxsupp.cpp` is an older, genuinely DIFFERENT
  variant (344 lines against 114). It is not a duplicate and merging it is a
  real change, not bookkeeping.
- `sdlshim.cpp` and `zlib_shim.c` have DIVERGED between the two ports and
  merging them changes behaviour. That is a different risk class and is
  tracked separately under #745.
- The nine private `limits.h` copies (eight distinct versions) across
  kernel/media and userland/apps. Headers change how code compiles; that
  merge needs its own verification.

## Licence

First-party MayteraOS code. Nothing in this directory is vendored, so it
carries no third-party attribution obligation and has no row in
tools/license-audit/components.tsv.
