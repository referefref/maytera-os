// testhook.h - #334 headless GUI verification hook.
//
// THIS FILE AND testhook.c ARE NEVER PART OF A NORMAL BUILD. They are only
// compiled when the Makefile is invoked as `make TESTHOOK=1` (see the
// `ifdef TESTHOOK` block there), which defines MAYTERA_TESTHOOK and adds
// testhook.c to SRCS. The default `make`/`make install` used by
// build/build-golden.sh does neither, so a shipping COMPOSIT binary
// contains zero bytes of this code and zero of its symbols - not a
// runtime-gated no-op, an ABSENT feature. Verify with a normal build:
// `nm COMPOSIT | grep -i testhook` and `grep -a TESTHOOK COMPOSIT` should
// both find nothing.
//
// See testhook.c for the full design rationale and command grammar.

#ifndef TESTHOOK_H
#define TESTHOOK_H

#ifdef MAYTERA_TESTHOOK
void testhook_poll(void);
#endif

#endif
