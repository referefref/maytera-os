#ifndef WIN16API_H
#define WIN16API_H
// win16api.h - Win16 API dispatch layer for the NE loader (#129, Phase 2 Step A).
// Clean-room minimal KERNEL/USER/GDI implementation. The NE loader (exec/ne.c)
// resolves every NE import to a far pointer WIN16_THUNK_SEG:id; when the x86_16
// interpreter traps a far CALL into that segment it invokes win16_api_dispatch().
// This module owns the dispatch table, the Pascal-stack cleanup, and the actual
// KERNEL startup contract (InitTask etc) so a real Win16 C runtime can proceed.
#include "../types.h"
#include "x86_16.h"

// Loader info the API layer needs to honour the InitTask contract. ne.c fills
// this in after loading an NE image and before running the interpreter.
typedef struct {
    uint16_t hinstance;      // app instance handle == autodata segment paragraph
    uint16_t hprev;          // previous instance (always 0, single-instance)
    uint16_t module_handle;  // pseudo HMODULE for the app
    uint16_t ss;             // initial SS (autodata stack segment)
    uint16_t sp;             // initial SP (top of stack)
    uint16_t ds;             // initial DS (autodata)
    uint16_t cmdline_seg;    // segment of the (empty) command line
    uint16_t cmdline_off;    // offset of the (empty) command line
    uint16_t lheap_base;     // near offset in DGROUP where the local heap starts
    uint16_t lheap_top;      // near offset in DGROUP where the local heap ends
    uint16_t ne_stack;       // NE-declared stack size (bytes); stack bottom = sp - ne_stack
    uint16_t segcount;       // (#278 p24) NE segment count; gate large-app heap handling
} win16_loader_info_t;

// (#278 Word6 pass26) Size of the dedicated DGROUP scratch window carved for
// Word's shared bump-allocator (cursor [DGROUP:0x0a], used by both seg132:0x108e
// and WWINTL seg2:0xc2). Without a valid scratch base that cursor sat at 0 and
// every scratch allocation returned a near-null DGROUP pointer, whose writes
// destroyed the MS-C local-heap descriptor globals at [0x0e]/[0x16] -> seg231
// INT 03. Carved from the top of the (over-large, mostly free) MS-C near heap.
#define WIN16_W6_SCRATCH 0x1000

// Import descriptor shared with ne.c (kept identical layout).
#define WIN16_NAME_MAX 32
typedef struct {
    char     module[WIN16_NAME_MAX];
    char     name[WIN16_NAME_MAX];
    uint16_t ordinal;
    int      by_ordinal;
} win16_import_t;

// Called once per win16_run_file() before the interpreter starts. Resets the
// per-run allocator and records loader info + the resolved import table so the
// dispatcher can name calls and honour the startup contract.
void win16_api_begin(const win16_loader_info_t *info,
                     const win16_import_t *imports, int import_count);

// Far-call trap target installed by ne.c for WIN16_THUNK_SEG. `off` is the
// import id. Logs every call, dispatches to a handler when known, performs the
// Pascal far-return with proper argument cleanup, and sets AX/DX. Returns
// non-zero to stop the interpreter (cap reached / fatal).
int win16_api_dispatch(x86_16_cpu_t *c, uint16_t off);

// Called by ne.c after the interpreter run finishes. Restores framebuffer state
// (direct mode) that the GUI bridge enabled during the run.
void win16_api_end(void);

// Append a formatted line to the per-run Win16 trace, written to /WIN16LOG.TXT
// at the end of the run (kprintf is not visible on this VM's serial socket).
void win16_trace(const char *fmt, ...);

// Record one cross-module (app DLL) import resolution to a side buffer during
// relocation; win16_api_begin folds it into the run trace afterwards. Lets us
// confirm e.g. CARDS.cdtInit/cdtDraw resolved to REAL DLL code, not a thunk.
void win16_reloc_log(const char *mod, const char *name, unsigned ord,
                     int real, unsigned seg, unsigned off);

#endif
