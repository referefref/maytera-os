# Browser JS/DOM differential fixtures (#245 / #275)

Four pages that exercise the Duktape-to-libdom binding (`../duktape/duk_dom.c`)
and the JS clock. They exist so a change to the binding can be shown NOT to have
changed what a page does, and so the clock's resolution is a measured number
rather than an assumption.

## What each one covers

| file | covers |
| --- | --- |
| `dom1.html` | `getElementById`, `tagName`, `textContent` get + set, `getAttribute` / `setAttribute`, `createElement`, `appendChild`, `innerHTML`, a miss returning `null` |
| `dom2.html` | 600-deep chained `appendChild` (through the `#245` depth cap), 500 flat `appendChild`, `Math`, `String`, `Array.sort`, `JSON.stringify` |
| `dom3.html` | `try`/`catch` and `instanceof TypeError`, `new Date().getFullYear()`, `window === globalThis`, `document.write`, `JSON.parse`, computed property keys |
| `clock.html` | `performance.now()` and `Date.now()`: presence, monotonicity, epoch plausibility, integrality, and RESOLUTION |

## How `clock.html` measures resolution, and why that way

It reports the SMALLEST NON-ZERO STEP each clock ever takes over 200000 reads.
That is the only definition that separates a one-second clock from a
one-millisecond one without depending on how long the loop happened to run: a
1 s clock steps by 1000 ms, a 1 ms clock by 1, a microsecond clock by about
0.001. Counting "distinct values seen" does not have that property, because it
also measures the loop's duration.

MEASURED on the stock golden 2571 browser, before the `#275` fix: the smallest
non-zero step of BOTH clocks was 1000 ms. Duktape 2.7 ships both APIs and this
port had them; what it did not have was any resolution, because
`duktape/build.sh` undefines `__linux__` and `duk_config.h` then falls back to
`DUK_USE_DATE_NOW_TIME`, which is `time(NULL) * 1000.0`.

## Running them

In the OS: copy one onto the ext2 root as `/TEST.HTML`; the browser loads it at
startup. Every fixture writes its result into the page as well as to
`console.log`, so it is readable from a screendump even on a browser whose
console goes nowhere. With a `make PERF=1` browser the same lines also appear on
serial as `[BRJS]`.

On the host, through the native rig: see `jsdom-native-diff.sh` in this
directory. It builds the engine twice from the same NetSurf/Duktape roots,
once with the committed `duk_dom.c` and once with whatever is in the live tree,
and compares the three DOM fixtures byte for byte. It also builds a DELIBERATELY
BROKEN third arm and FAILS if that arm does not diverge, because a differential
that has never been seen to fail is not evidence.

## What they do NOT cover

No layout, no CSS, no paint: these compare the DOM and the console output, not
pixels. `tools/browser-pixelprobe` and the `[BRDUMP]` box dump are the tools for
that. No network, no external `<script src=>` (the binding does not run those).
