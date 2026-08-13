#!/usr/bin/env python3
"""#28 ClassiCube: add a PLAT_MAYTERA platform branch to src/Core.h.

Idiomatic upstream style: one #elif-chain branch like PLAT_ATARIOS/MSDOS, plus
one new CC_WIN_BACKEND_* id. No other upstream file is touched. Every anchor is
asserted, so a source bump that moves these lines fails loudly instead of
silently patching the wrong place.
"""
import sys, io

path = sys.argv[1]
src = io.open(path, encoding='utf-8', newline='').read()

if 'PLAT_MAYTERA' in src:
    print('Core.h: already patched, nothing to do')
    sys.exit(0)

# --- anchor 1: the CC_WIN_BACKEND id table. 8 is the only free id (1-7,9 used).
NL = '\r\n' if '\r\n' in src else '\n'
A1 = '#define CC_WIN_BACKEND_WIN32CE  9' + NL
assert src.count(A1) == 1, 'anchor 1 (CC_WIN_BACKEND_WIN32CE) not found exactly once'
for taken in range(1, 10):
    pass
assert '#define CC_WIN_BACKEND_MAYTERA' not in src
# prove id 8 is genuinely unused before claiming it
import re
ids = set(int(m) for m in re.findall(r'#define CC_WIN_BACKEND_\w+\s+(\d+)', src))
assert 8 not in ids, 'id 8 is no longer free upstream: %s' % sorted(ids)
src = src.replace(A1, A1 + '#define CC_WIN_BACKEND_MAYTERA  8' + NL, 1)

# --- anchor 1b: the CC_AUD_BACKEND id table. Audio_Maytera.c is a real backend,
# not a variant of NULL, so it gets its own id rather than impersonating one.
A1B = '#define CC_AUD_BACKEND_OS2      5' + NL
assert src.count(A1B) == 1, 'anchor 1b (CC_AUD_BACKEND_OS2) not found exactly once'
aud_ids = set(int(m) for m in re.findall(r'#define CC_AUD_BACKEND_\w+\s+(\d+)', src))
assert 6 not in aud_ids, 'audio id 6 is no longer free upstream: %s' % sorted(aud_ids)
src = src.replace(A1B, A1B + '#define CC_AUD_BACKEND_MAYTERA  6' + NL, 1)

# --- anchor 2: head of the platform detection #if-chain.
A2 = NL.join(['#ifndef CC_BUILD_MANUAL','#if defined NXDK','']) 
assert src.count(A2) == 1, 'anchor 2 (CC_BUILD_MANUAL / NXDK chain head) not found exactly once'

BRANCH = '''#ifndef CC_BUILD_MANUAL
#if defined PLAT_MAYTERA
\t/* MayteraOS (#28). Ring 3 app, freestanding libc, no dynamic loader.
\t   Every DEFAULT_*_BACKEND below is overridable from the command line,
\t   because Core.h only applies it under #ifndef CC_<X>_BACKEND. */
\t#define CC_BUILD_MAYTERA
\t#undef  CC_BUILD_FREETYPE   /* no freetype; use the built-in bitmap font */
\t#undef  CC_BUILD_PLUGINS    /* no dlopen: nothing loads shared objects */
\t#define DEFAULT_NET_BACKEND CC_NET_BACKEND_BUILTIN
\t#define DEFAULT_SSL_BACKEND CC_SSL_BACKEND_NONE
\t#define DEFAULT_AUD_BACKEND CC_AUD_BACKEND_MAYTERA
\t#define DEFAULT_GFX_BACKEND CC_GFX_BACKEND_SOFTGPU
\t#define DEFAULT_WIN_BACKEND CC_WIN_BACKEND_MAYTERA
#elif defined NXDK
'''
src = src.replace(A2, BRANCH.replace('\n', NL), 1)

io.open(path, 'w', encoding='utf-8', newline='').write(src)
print('Core.h: patched (CC_WIN_BACKEND_MAYTERA=8, PLAT_MAYTERA branch)')
