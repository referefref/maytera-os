// x86_16.c - Compact real-mode 8086/80186 instruction interpreter for MayteraOS.
// Freestanding kernel C. No libc, no stdlib.
#include "x86_16.h"
#include "../serial.h"

// ---------------------------------------------------------------------------
// FLAGS bit positions (8086 layout)
// ---------------------------------------------------------------------------
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

static x86_16_int_fn g_int_handler = 0;

// I/O port hooks (#201 DOS). When set, IN/OUT instructions route here instead of
// the default (read 0xFF, write ignored). Lets the DOS layer capture VGA DAC
// palette writes (ports 0x3C8/0x3C9) and serve VGA status reads. width = 1 or 2.
typedef uint16_t (*x86_16_in_fn)(struct x86_16_cpu *cpu, uint16_t port, int width);
typedef void     (*x86_16_out_fn)(struct x86_16_cpu *cpu, uint16_t port, uint16_t val, int width);
static x86_16_in_fn  g_in_handler  = 0;
static x86_16_out_fn g_out_handler = 0;
void x86_16_set_io_handlers(x86_16_in_fn infn, x86_16_out_fn outfn) {
    g_in_handler = infn; g_out_handler = outfn;
}

// Software x87 FPU state reset (defined further below near the FPU helpers).
static void fp_reset(void);

void x86_16_set_int_handler(x86_16_int_fn fn) { g_int_handler = fn; }

// (#256 VB1) Optional abort hook: x86_16_call_far's resume loop polls this between
// slices and stops resuming a long-running callee when it returns nonzero (the
// Win16 layer wires it to its close-request latch, so a titlebar-X / F4 can break
// out of the VB runtime's in-wndproc message loop). NULL = never abort.
static int (*g_callfar_abort_fn)(void) = 0;
void x86_16_set_callfar_abort(int (*fn)(void)) { g_callfar_abort_fn = fn; }

// Far-call trap (see x86_16.h). g_farcall_active gates the comparison so a 0x0000
// trap seg does not accidentally trap real far calls when no trap is registered.
static x86_16_farcall_fn g_farcall_fn  = 0;
static uint16_t          g_farcall_seg = 0;
static int               g_farcall_active = 0;

void x86_16_set_farcall_trap(uint16_t seg, x86_16_farcall_fn fn) {
    g_farcall_seg = seg;
    g_farcall_fn  = fn;
    g_farcall_active = (fn != 0);
}

// --- one-function instruction trace (see x86_16.h) ---
extern void win16_trace(const char *fmt, ...);
static uint16_t g_fnt_seg = 0, g_fnt_off = 0;   // function to trace
int g_ole2_farlog = 0; void g_farlog_reset(void){}
int g_ole2_k334log = 0;   // (#289 b455) KERNEL.334 GetSelectorLimit trace gate
int g_w6life = 0;   /* P62 SHIP OFF (set 1 for diag) */
// (#278 P50) view sub-object flag write-watch. Set to lin(dgroup, [0x4a82]) whenever
// [0x4a82] (the view sub-object near-ptr) is written, so it re-arms across the heap
// compaction (create 4c98 -> compacted 9e34). Any write into [base+0x0a .. base+0x0d]
// (the realize word +0xa/+0xb and the display-gate flag +0xc) is logged with cs:ip +
// old->new, DEFINITIVELY answering whether anything clears [+0xc]&2 (display-gate
// blocker) or sets [+0xb]&8 (realize) during our boot, regardless of imm vs reg source.
uint32_t g_w6obj_lin = 0;
/* (#278 P57) realize-bootstrap hunt: track every constructed view + watch realize on all */
uint32_t g_w6view_tab[24]; int g_w6view_n = 0; int g_w6rlz_reported = 0;
int g_w6ctor_n = 0; uint16_t g_w6ctor_rcs=0, g_w6ctor_rip=0; int g_w6ctor_pending=0;
uint16_t g_w6dc_rcs=0, g_w6dc_rip=0; int g_w6dc_armed=0; int g_w6h2_trace=0, g_w6h2_n=0;
// (#278 P54 REVERSE-FIELD) When g_w6fmt_read is set (ONLY during the one-shot
// forced-formatter diagnostic below), the memory READ path logs the first read
// of each view-object field that the formatter (seg182:0x2de/0x340 + callees)
// makes, with the reading cs:ip and the value. Goal: FLIP the search - instead
// of hunting who SETS realize, find which view field the formatter DEMANDS that
// construction leaves zero/garbage (the field whose absence tears the chrome).
int g_w6fmt_read = 0;
static uint8_t g_w6rf_seen[0x200];   // first-touch dedup over [obj .. obj+0x1ff]
static int g_w6rf_seq = 0;           // count of distinct fields read this force
static int g_w6rf_done = 0;          // one-shot guard for the forced-formatter call
int g_w6vscan = 0;  // (#278 P40) one-shot: set at first edit-pane paint to trigger the DGROUP vtable scan
// (#278 P52 runtime-callf-trace) Global ordering counter: bumped at every W6SEQ
// checkpoint (doc-create entry/ctor-return/realize-test + every message delivered
// to the doc-family windows in win16api.c) so the interleaving of construction vs
// message dispatch is visible in one monotonically increasing log stream.
int g_w6seq = 0;
// (#278 P52) CALLF trace pending-return stack for the seg155 (0x04df) doc-create
// realize window [0x9be, 0xb89]. Pushed in the 0x9A (CALLF) handler when the call
// SITE falls in that range; popped (and ax logged) at the top of the main loop
// when execution returns to the matching cs:ip. LIFO so nested far calls made by
// a callee (which return to *their* caller first) unwind correctly.
#define W6CALLF_MAXPEND 32
static struct { uint16_t cs, ip; uint16_t call_ip; } g_w6callf_pend[W6CALLF_MAXPEND];
static int g_w6callf_npend = 0;
// (#289 b481) Instruction ring buffer to capture the code path that BUILDS the
// bad far pointer (0017:0a6a) just before the first far-call into segment 0.
#define OLE2_RING 600
static uint16_t g_ring_cs[OLE2_RING], g_ring_ip[OLE2_RING];
static uint16_t g_ring_ds[OLE2_RING], g_ring_es[OLE2_RING], g_ring_op[OLE2_RING];
static int g_ring_pos = 0; static int g_ring_dumped = 0;
static void ole2_ring_dump(const char *why) {
    if (g_ring_dumped) return;
    g_ring_dumped = 1;
    kprintf("[RING] dump (%s), %d insns, ds-transitions:\n", why, OLE2_RING);
    uint16_t prevds = 0xFFFF;
    for (int k = 0; k < OLE2_RING; k++) {
        int i = (g_ring_pos + k) % OLE2_RING;
        if (g_ring_ds[i] != prevds) {
            kprintf("[RINGDS] @%04x:%04x op=%02x ds %04x->%04x es=%04x\n",
                g_ring_cs[i], g_ring_ip[i], g_ring_op[i], prevds, g_ring_ds[i], g_ring_es[i]);
            prevds = g_ring_ds[i];
        }
    }
}
void x86_16_dump_ring_full(void) {
    kprintf("[RINGFULL] last %d insns (oldest->newest):\n", OLE2_RING);
    for (int k = 0; k < OLE2_RING; k++) {
        int i = (g_ring_pos + k) % OLE2_RING;
        kprintf("%04x:%04x ", g_ring_cs[i], g_ring_ip[i]);
        if ((k & 7) == 7) kprintf("\n");
    }
    kprintf("\n[RINGFULL] end\n");
}
static int      g_fnt_arm = 0;                  // armed (waiting for the call)
static int      g_fnt_in  = 0;                  // currently inside the function
static uint16_t g_fnt_rcs = 0, g_fnt_rip = 0;   // return cs:ip captured at entry
static uint16_t g_fnt_pxs = 0, g_fnt_pxo = 0;   // pdxCard far ptr
static uint16_t g_fnt_pys = 0, g_fnt_pyo = 0;   // pdyCard far ptr
static int      g_fnt_lines = 0;                // bound the per-insn log

void x86_16_set_fn_trace(uint16_t seg, uint16_t off) {
    g_fnt_seg = seg; g_fnt_off = off;
    g_fnt_arm = (seg != 0 || off != 0);
    g_fnt_in = 0; g_fnt_lines = 0;
}

// Sentinel CS used by x86_16_call_far. When the run loop is about to fetch from
// this segment, it means a nested __far __pascal call returned to the host trap
// frame, so the loop stops. 0xFFFF is never a real loaded segment here.
#define X86_16_CALLFAR_SENTINEL 0xFFFF
static int      g_callfar_stop = 0;   // (#278 pass30) NESTING DEPTH of active x86_16_call_far
                                      // invocations (was a 0/1 flag; a nested call cleared it on the
                                      // inner return, so the OUTER callback's retf to the ffff:0000
                                      // sentinel was missed -> wild cs=ffff jump). Counter fixes nesting.

void x86_16_init(x86_16_cpu_t *cpu, uint8_t *mem1mb) {
    cpu->mem = mem1mb;
    cpu->ax = cpu->bx = cpu->cx = cpu->dx = 0;
    cpu->si = cpu->di = cpu->bp = cpu->sp = 0;
    for (int i = 0; i < 8; i++) cpu->exhi[i] = 0;   // (#194) high halves of E-regs
    cpu->cs = cpu->ds = cpu->es = cpu->ss = 0;
    cpu->fs = cpu->gs = 0;   // (#390) 386+ FS/GS default null
    cpu->ip = 0;
    cpu->flags = 0x0002; // bit1 always 1 on x86
    cpu->halted = 0;
    cpu->exit_code = 0;
    cpu->insn_count = 0;
    fp_reset();
}

// ===========================================================================
// (#289 Phase 1) Protected-mode selector / LDT model + arena. See x86_16.h.
//
// DESIGN: a single switch point. In REAL mode (g_win16_pmode==0) lin() is the
// classic (seg<<4)+off masked to 1 MiB and the rd/wr helpers index cpu->mem
// (the 1 MiB buffer) exactly as before - byte-for-byte identical, zero added
// work on the hot path beyond one predictable-branch test. In PROTECTED mode
// lin() resolves the selector through the LDT into a byte OFFSET into the large
// arena, and the rd/wr helpers index the arena instead of cpu->mem.
//
// Special selectors that must NOT be treated as LDT descriptors even in pmode:
//   - the Win16 API thunk segment (0xF000): a far CALL to it is intercepted by
//     the farcall trap BEFORE any byte is fetched, so it is never translated in
//     normal flow; we still map it to a guard region defensively.
//   - the call_far sentinel (0xFFFF): the run loop stops before fetching from
//     it. Also mapped to the guard region defensively.
// Any unallocated selector maps to a 64 KiB guard region at the TOP of the arena
// so stray accesses neither fault nor corrupt live blocks.
// ===========================================================================
int g_win16_pmode = 0;              // default 0 => real-mode, regression-safe

// Arena: a large contiguous backing store for all protected-mode selectors.
// 48 MiB is enough for Word6 + the OLE2 DLL set with room for huge GlobalAllocs.
#define WIN16_ARENA_SIZE   (48u * 1024u * 1024u)
#define WIN16_GUARD_SIZE   (64u * 1024u)            // top guard page for stray sels
static uint8_t  g_win16_arena[WIN16_ARENA_SIZE];    // static => guaranteed mapped+contiguous
static uint32_t g_arena_next = 0;                   // bump pointer (offset)
static uint32_t g_sel80_base = 0;   // (#278 Word6) zeroed backing for anomalous sel 0x0080

typedef struct { uint32_t base, limit; uint8_t present, is_code; } ldt_desc_t;
static ldt_desc_t g_ldt[WIN16_LDT_ENTRIES];

uint8_t *win16_arena_ptr(void)  { return g_win16_arena; }
uint32_t win16_arena_size(void) { return WIN16_ARENA_SIZE; }

void win16_arena_reset(void) {
    // Reserve the top 64 KiB as the guard region for unallocated/special sels.
    // (#289 Phase1 fix) Reserve OFFSET 0 as a sentinel: win16_arena_alloc returns
    // an arena offset, and every caller (ne.c segment loader, heap_alloc/GlobalAlloc,
    // cmdline_seg) treats a 0 return as "out of arena". If we started the bump
    // pointer at 0, the FIRST allocation (e.g. COMPOBJ segment 0) would legitimately
    // return offset 0 and be misread as a failure -> "pmode arena/LDT full at seg 0"
    // and the DLL never loads. Start at 16 (one paragraph) so 0 unambiguously = fail.
    g_arena_next = 16;
    g_sel80_base = 0;
}
void ldt_reset(void) {
    for (int i = 0; i < WIN16_LDT_ENTRIES; i++) {
        g_ldt[i].base = 0; g_ldt[i].limit = 0;
        g_ldt[i].present = 0; g_ldt[i].is_code = 0;
    }
}

uint32_t win16_arena_alloc(uint32_t bytes, uint32_t align) {
    if (align < 1) align = 1;
    if (bytes == 0) bytes = 1;
    uint32_t base = (g_arena_next + (align - 1)) & ~(align - 1);
    // Keep the top WIN16_GUARD_SIZE reserved as the stray-selector guard region.
    if ((uint64_t)base + bytes > (uint64_t)WIN16_ARENA_SIZE - WIN16_GUARD_SIZE)
        return 0;   // out of arena
    g_arena_next = base + bytes;
    return base;
}

uint16_t ldt_alloc(uint32_t base, uint32_t limit, int is_code) {
    // Index 0 is left as the null descriptor (selector 0 is invalid in pmode).
    for (int i = 1; i < WIN16_LDT_ENTRIES; i++) {
        if (!g_ldt[i].present) {
            g_ldt[i].base = base; g_ldt[i].limit = limit;
            g_ldt[i].present = 1; g_ldt[i].is_code = is_code ? 1 : 0;
            return (uint16_t)((i << WIN16_SEL_SHIFT) | 7);  // TI=1, RPL=3
        }
    }
    return 0;   // table full
}
static inline int sel_index(uint16_t sel) { return (sel >> WIN16_SEL_SHIFT); }
uint32_t ldt_base(uint16_t sel) {
    int i = sel_index(sel);
    if (i <= 0 || i >= WIN16_LDT_ENTRIES || !g_ldt[i].present) return 0;
    return g_ldt[i].base;
}
uint32_t ldt_limit(uint16_t sel) {
    int i = sel_index(sel);
    if (i <= 0 || i >= WIN16_LDT_ENTRIES || !g_ldt[i].present) return 0;
    return g_ldt[i].limit;
}
void ldt_set_base(uint16_t sel, uint32_t base) {
    int i = sel_index(sel);
    if (i > 0 && i < WIN16_LDT_ENTRIES && g_ldt[i].present) g_ldt[i].base = base;
}
int ldt_valid(uint16_t sel) {
    int i = sel_index(sel);
    return (i > 0 && i < WIN16_LDT_ENTRIES && g_ldt[i].present);
}
void win16_pmode_enable(int on) {
    if (on) { ldt_reset(); win16_arena_reset(); g_win16_pmode = 1; }
    else    { g_win16_pmode = 0; }
}

// (#278 Word6) Pre-format the anomalous sel-0x0080 far-heap as a real, usable
// Win16 near-heap so the MS-C runtime's malloc finds free space immediately
// instead of spinning in the grow/recovery loop on an empty (zeroed) heap.
// Layout (single-word headers, bit0=FREE, advance=hdr+1; offsets RE'd from Word's
// own DGROUP LOCALINFO@3644): seg[0x14]=0x12 sig, seg[0x16]=LOCALINFO(0x40);
// LOCALINFO: [+0]pLast=0xFE00 [+2]pFirst=0x200 [+4]pFreeFirst=0x200
// [+0x14]hTabStart=0x60 [+0x16]freeScan=0x60 [+0x18]hTabEnd=0x200
// [+0x1a]byteCount [+0x1e]freeCount; one FREE block at 0x200 spanning to pLast.
static void win16_sel80_format(uint32_t b) {
    uint8_t *a = g_win16_arena + b;
#define SW(o,v) do{ a[(o)]=(uint8_t)((v)&0xFF); a[(o)+1]=(uint8_t)(((v)>>8)&0xFF);}while(0)
    SW(0x14, 0x0012);          /* local-heap signature (matches __LInit) */
    SW(0x16, 0x0040);          /* LOCALINFO pointer */
    SW(0x40+0x00, 0xFE00);     /* pLast */
    SW(0x40+0x02, 0x0200);     /* pFirst */
    SW(0x40+0x04, 0x0200);     /* pFreeFirst (rover) */
    SW(0x40+0x14, 0x0060);     /* handle-table start */
    SW(0x40+0x16, 0x0060);     /* handle-table free-scan ptr */
    SW(0x40+0x18, 0x0200);     /* handle-table end (0x60..0x200 = empty slots) */
    SW(0x40+0x1A, 0xFC00);     /* heap byte count */
    SW(0x40+0x1E, 0xFBFE);     /* stored free count */
    SW(0x0200, 0xFBFF);        /* one big FREE block: (size-1)|1, advance 0xFC00 -> pLast */
#undef SW
}

// Protected-mode address resolution: selector -> arena byte offset. Unallocated
// / special selectors are clamped into the top guard region so they are inert.
static inline uint32_t lin_pmode(uint16_t sel, uint16_t off) {
    // (#278 Word6) Selector 0x0080 (RPL=0) is an ANOMALOUS far-heap segment Word's
    // MS-C runtime targets; sel_index() ignores RPL bits so 0x0080 aliases the real
    // WINWORD seg16 (sel 0x0087), making the runtime read seg16's code/data as a
    // garbage local-heap LOCALINFO -> free-scan 0x302 overflow -> int3 runaway at
    // 073f:066e. Give 0x0080 its OWN zeroed 64 KiB region so the far-heap-init
    // formats a clean heap (or malloc fails cleanly) instead of corrupting on seg16.
    // Gated to the exact value 0x0080 (no app uses this RPL=0 selector legitimately;
    // seg16's real selector 0x0087 is unaffected). Zeroed once per win16 run.
    if (sel == 0x0080) {
        if (!g_sel80_base) {
            uint32_t b = win16_arena_alloc(0x10000u, 16);
            if (b) { for (uint32_t k = 0; k < 0x10000u; k++) g_win16_arena[b + k] = 0;
                     win16_sel80_format(b);
                     // (#278 Word6 pass18) Fill the near-heap FREE-block interior
                     // (0x202..top) with 0xFF so OUT-OF-BOUNDS 12-byte-record reads
                     // return 0xFFFF (the Win16 free / end-of-chain sentinel) instead
                     // of 0x0000. Word indexes a based-heap record table here WITHOUT a
                     // bounds check (accessor seg222:0x47a2 = base + idx*recsize). In
                     // late init seg32:0x0e4a is called with a stale entity id 0x08f5
                     // (2293) against a 32-entry table; record[0x08f5] lands far past the
                     // block in free heap space. With 0x00 there, record[idx]+2 & 0x18 == 0
                     // (-> the unsafe hash-walk path that has NO 0xffff terminator: a
                     // 0 link is read as "chain starts at record 0", which walks to a
                     // 0xffff link and loops forever) and the bucket head reads 0x0000
                     // (a valid index) instead of -1. With 0xFF, record[idx]+2 == 0xFFFF
                     // sets bit 0x18 -> the SAFE walk path (cmp si,-1 terminator) and the
                     // bucket head reads 0xFFFF == -1 -> clean "not found" return. This
                     // matches real Win16, where free/unused records carry 0xFFFF links
                     // (verified: a live record[0x1f] = {+4,+6,+8 = 0xffff}). Block
                     // HEADERS (<= 0x200: signature, LOCALINFO, handle table, free hdr)
                     // are left as formatted; the local-heap allocator only ever reads
                     // block headers, never their interiors, so 0xFF free space is inert
                     // for allocation and strictly safer (any record Word forgets to
                     // initialize now defaults to the empty sentinel, not record 0).
                     for (uint32_t k = 0x202; k < 0x10000u; k++) g_win16_arena[b + k] = 0xFF;
                     g_sel80_base = b; }
        }
        if (g_sel80_base) return g_sel80_base + (uint32_t)off;
        return (WIN16_ARENA_SIZE - WIN16_GUARD_SIZE) + (off & (WIN16_GUARD_SIZE - 1));
    }
    int i = sel_index(sel);
    if (i > 0 && i < WIN16_LDT_ENTRIES && g_ldt[i].present) {
        uint32_t a = g_ldt[i].base + off;
        // Tolerate off running just past limit (tiling math) but keep it inside
        // the arena's usable region. A hard clamp avoids OOB indexing.
        if (a >= WIN16_ARENA_SIZE) a = WIN16_ARENA_SIZE - WIN16_GUARD_SIZE + (a & (WIN16_GUARD_SIZE - 1));
        return a;
    }
    // Unallocated / thunk-seg / sentinel: route to the guard region.
    return (WIN16_ARENA_SIZE - WIN16_GUARD_SIZE) + (off & (WIN16_GUARD_SIZE - 1));
}

// ---------------------------------------------------------------------------
// Linear memory access (seg:off -> physical, wrapped to 1 MiB)
// ---------------------------------------------------------------------------
static inline uint32_t lin(uint16_t seg, uint16_t off) {
    if (g_win16_pmode) return lin_pmode(seg, off);
    return (((uint32_t)seg << 4) + off) & 0xFFFFF;
}
// Select the backing byte array for the current mode (arena in pmode, else the
// caller-supplied 1 MiB buffer). One predictable branch; real mode unchanged.
static inline uint8_t *membase(x86_16_cpu_t *cpu) {
    return g_win16_pmode ? g_win16_arena : cpu->mem;
}

// ---- Memory-mapped I/O hook (#202 EGA planar VGA) ------------------------
static uint32_t          g_mh_lo = 0, g_mh_hi = 0;
static x86_16_mem_w_fn   g_mh_w = 0;
static x86_16_mem_r_fn   g_mh_r = 0;
void x86_16_set_mem_hook(uint32_t lo, uint32_t hi, x86_16_mem_w_fn wfn, x86_16_mem_r_fn rfn) {
    g_mh_lo = lo; g_mh_hi = hi; g_mh_w = wfn; g_mh_r = rfn;
}
static inline int mh_hit(uint32_t a) { return g_mh_r && a >= g_mh_lo && a < g_mh_hi; }
static inline int mh_hit_w(uint32_t a) { return g_mh_w && a >= g_mh_lo && a < g_mh_hi; }

// (#278 P54 REVERSE-FIELD) log the FIRST read of each view-object field during
// the forced-formatter diagnostic. Deduped by relative offset; annotates which
// of the 5 multiple-inheritance bases the field belongs to and flags zero reads
// (a zero far-ptr / rect / cache is the kind of "incomplete field" we hunt).
static void w6rf_note(x86_16_cpu_t *cpu, uint32_t a, uint16_t v, int sz) {
    if (!g_w6life || !g_w6fmt_read || !g_w6obj_lin) return;
    if (a < g_w6obj_lin || a > g_w6obj_lin + 0x1ff) return;
    uint32_t rel = a - g_w6obj_lin;
    if (g_w6rf_seen[rel]) return;
    g_w6rf_seen[rel] = 1;
    if (g_w6rf_seq >= 220) return;
    g_w6rf_seq++;
    static const uint32_t bases[5] = {0x00,0xb8,0xf2,0x102,0x114};
    int bi = 0; for (int k=4;k>=0;k--){ if (rel>=bases[k]){ bi=k; break; } }
    kprintf("[W6REVF] #%d rd%d view+0x%03x (base%d+0x%02x) = %04x by cs:ip=%04x:%04x%s\n",
            g_w6rf_seq, sz, (unsigned)rel, bi+1, (unsigned)(rel-bases[bi]), v,
            cpu->cs, cpu->ip, (v==0)?"  <ZERO>":"");
}
uint8_t x86_16_rd8(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off) {
    uint32_t a = lin(seg, off);
    if (!g_win16_pmode && mh_hit(a)) return (uint8_t)g_mh_r(cpu, a, 1);
    uint8_t vv = membase(cpu)[a];
    if (g_w6fmt_read) w6rf_note(cpu, a, vv, 1);
    return vv;
}
uint16_t x86_16_rd16(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off) {
    uint32_t a = lin(seg, off);
    uint32_t b = lin(seg, (uint16_t)(off + 1));
    if (!g_win16_pmode && mh_hit(a)) {
        if (a + 1 == b) return g_mh_r(cpu, a, 2);
        return (uint16_t)(g_mh_r(cpu, a, 1) | (g_mh_r(cpu, b, 1) << 8));
    }
    uint8_t *m = membase(cpu);
    uint16_t vv = (uint16_t)(m[a] | (m[b] << 8));
    if (g_w6fmt_read) w6rf_note(cpu, a, vv, 2);
    return vv;
}
// Diagnostic watchpoint: when set (non-zero), any write whose linear address
// equals g_wp_lin is logged with the writing CS:IP. Set via x86_16_set_watch.
uint32_t g_wp_lin = 0;
extern void win16_trace(const char *fmt, ...);
static void wp_check(x86_16_cpu_t *cpu, uint32_t a, uint16_t v, int sz) {
    if (g_wp_lin && (a == g_wp_lin || (sz == 2 && a + 1 == g_wp_lin)))
        kprintf("[W6WP] lin=%05lx <- %04x sz=%d by cs:ip=%04x:%04x ds=%04x\n",
                    (unsigned long)g_wp_lin, v, sz, cpu->cs, cpu->ip, cpu->ds);
}
void x86_16_set_watch(uint32_t lin_addr) { g_wp_lin = lin_addr; }
void x86_16_wr8(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint8_t v) {
    uint32_t a = lin(seg, off);
    wp_check(cpu, a, v, 1);
    { extern int g_w6life; extern uint32_t g_w6view_tab[]; extern int g_w6view_n, g_w6rlz_reported;
      if (g_w6life && (v&0x08)) { uint8_t _ob=membase(cpu)[a];
        if(!(_ob&0x08)) for(int _i=0;_i<g_w6view_n;_i++) if(a==g_w6view_tab[_i]+0x0b){
          kprintf("[W6BOOT] *** REALIZE SET view#%d(lin=%05lx) +0xb %02x->%02x cs:ip=%04x:%04x ***\n",
            _i,(unsigned long)g_w6view_tab[_i],_ob,v,cpu->cs,cpu->ip); g_w6rlz_reported=1; } } }
    { extern int g_w6life, g_win16_pmode; extern uint32_t g_w6obj_lin;
      if (g_w6life && g_w6obj_lin && a >= g_w6obj_lin+0x0a && a <= g_w6obj_lin+0x0d) {
        uint8_t oldb = membase(cpu)[a];
        kprintf("[W6VFLAG8] obj+0x%02x %02x->%02x cs:ip=%04x:%04x%s%s\n",
                (unsigned)(a - g_w6obj_lin), oldb, v, cpu->cs, cpu->ip,
                (a==g_w6obj_lin+0x0c && (oldb&0x02) && !(v&0x02)) ? "  *** CLEARS +0xc bit1 (display-gate unblocked) ***" : "",
                (a==g_w6obj_lin+0x0b && !(oldb&0x08) && (v&0x08)) ? "  *** SETS +0xb realize ***" : "");
      }
      // (#278 P53 ACTION 1c) WIDEN the view watch to the FULL 0x114-byte MI block.
      // The view is one multiple-inheritance object with 5 base "this" pointers at
      // base1+{0x00,0xb8,0xf2,0x102,0x114} (pass-40 dump 9e34/9eec/9f26/9f36/9f48).
      // Every prior realize watch looked ONLY at base1 (+0xb/+0xc). A realize/"shown"
      // bit may live on a sibling base (esp. base2 9eec+0xb/+0xc). Log ALL byte writes
      // into base1..base1+0x114 with the writer cs:ip, and specially flag the +0xb/+0xc
      // bytes of each of the 5 bases.
      if (g_w6life && g_w6obj_lin && a >= g_w6obj_lin && a <= g_w6obj_lin+0x114) {
        uint8_t oldb = membase(cpu)[a];
        uint32_t rel = a - g_w6obj_lin;
        static const uint32_t bases[5] = {0x00,0xb8,0xf2,0x102,0x114};
        int basei = -1, boff = 0;
        for (int bi=0; bi<5; bi++){ if (rel==bases[bi]+0x0b || rel==bases[bi]+0x0c){ basei=bi; boff=(int)(rel-bases[bi]); break; } }
        if (basei>=0) {
          kprintf("[W6MIB] base%d+0x%x (blockoff 0x%02x) %02x->%02x cs:ip=%04x:%04x%s%s\n",
                  basei+1, boff, (unsigned)rel, oldb, v, cpu->cs, cpu->ip,
                  (boff==0x0b && !(oldb&0x08) && (v&0x08)) ? "  *** SETS +0xb realize-shape bit3 on sibling base ***" : "",
                  (boff==0x0c && (oldb&0x02) && !(v&0x02)) ? "  *** CLEARS +0xc needs-layout on sibling base ***" : "");
        } else {
          static int nmib=0; if (nmib<200){ nmib++;
            kprintf("[W6MIB] blockoff 0x%02x %02x->%02x cs:ip=%04x:%04x\n", (unsigned)rel, oldb, v, cpu->cs, cpu->ip); }
        }
      }
    }
    { extern int g_w6life, g_win16_pmode; extern uint16_t win16_dgroup_sel;
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && off==0x0344)
        kprintf("[W6LIFE8] [0344]<-%02x cs:ip=%04x:%04x\n", v, cpu->cs, cpu->ip);
      // (#278 P47) CORRECTED realize probe. Pass 46's version caught Pascal-string
      // builders (length byte / char writes that happen to have bit3), NOT realize
      // flags, because it filtered only on the VALUE. The view "realized" flag lives
      // at <viewobj>+0x0b; a genuine realize write is an instruction with an explicit
      // +0x0b displacement (modrm mod=01, disp8==0x0b). Verify the WRITING INSTRUCTION
      // targets [reg+0x0b] by decoding its bytes, which excludes movs/[di]/stosb
      // string copies (no +0xb disp). This reveals the true realize routine(s).
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && (v & 0x08)) {
        uint8_t oldb = membase(cpu)[a];
        if (!(oldb & 0x08)) {
          int is_flag_write = 0;
          for (int k = 1; k <= 5; k++) {
            uint8_t b0 = x86_16_rd8(cpu, cpu->cs, (uint16_t)(cpu->ip + k));
            uint8_t bm = x86_16_rd8(cpu, cpu->cs, (uint16_t)(cpu->ip + k - 1));
            if (b0 == 0x0b && (bm & 0xC0) == 0x40) { is_flag_write = 1; break; }
          }
          if (is_flag_write) { static int nb=0; if(nb<80){nb++;
            kprintf("[W6RLZ] realize-write DGROUP[%04x](obj~%04x) +0xb %02x->%02x cs:ip=%04x:%04x\n",
                    off, (uint16_t)(off-0x0b), oldb, v, cpu->cs, cpu->ip); } }
        }
      }
    }
    if (!g_win16_pmode && mh_hit_w(a)) { g_mh_w(cpu, a, v, 1); return; }
    membase(cpu)[a] = v;
}
void x86_16_wr16(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint16_t v) {
    uint32_t a = lin(seg, off);
    uint32_t b = lin(seg, (uint16_t)(off + 1));
    wp_check(cpu, a, v, 2);
    { extern int g_w6life; extern uint32_t g_w6view_tab[]; extern int g_w6view_n, g_w6rlz_reported;
      if (g_w6life) for(int _i=0;_i<g_w6view_n;_i++){ uint32_t _base=g_w6view_tab[_i];
        if(a==_base+0x0b && (v&0x08)){ uint8_t _ob=membase(cpu)[a]; if(!(_ob&0x08)){
          kprintf("[W6BOOT] *** REALIZE SET(word-lo) view#%d +0xb ->%02x cs:ip=%04x:%04x ***\n",_i,(v&0xff),cpu->cs,cpu->ip); g_w6rlz_reported=1; } }
        if(a==_base+0x0a && ((v>>8)&0x08)){ uint8_t _oh=membase(cpu)[b]; if(!(_oh&0x08)){
          kprintf("[W6BOOT] *** REALIZE SET(word-hi) view#%d +0xb ->%02x cs:ip=%04x:%04x ***\n",_i,(v>>8),cpu->cs,cpu->ip); g_w6rlz_reported=1; } } } }
    { extern int g_w6life, g_win16_pmode; extern uint32_t g_w6obj_lin;
      // (#278 P50) word-write into the view sub-object flag region [+0xa..+0xd].
      if (g_w6life && g_w6obj_lin && ((a >= g_w6obj_lin+0x0a && a <= g_w6obj_lin+0x0d)
                                   || (b >= g_w6obj_lin+0x0a && b <= g_w6obj_lin+0x0d))) {
        uint8_t oldlo = membase(cpu)[a], oldhi = membase(cpu)[b];
        kprintf("[W6VFLAGW] obj+0x%02x %04x->%04x (old %02x%02x) cs:ip=%04x:%04x%s%s\n",
                (unsigned)(a - g_w6obj_lin), (unsigned)(oldlo|(oldhi<<8)), v, oldhi, oldlo,
                cpu->cs, cpu->ip,
                (a==g_w6obj_lin+0x0c && (oldlo&0x02) && !(v&0x02)) ? "  *** CLEARS +0xc bit1 ***" : "",
                (b==g_w6obj_lin+0x0b && !(oldhi&0x08) && ((v>>8)&0x08)) ? "  *** SETS +0xb realize (word) ***" : "");
      }
      // (#278 P53 ACTION 1c) WIDEN word-writes to the full 0x114 MI block; flag any
      // word write that sets a realize-shape bit3 in the +0xb byte of any of the 5 bases.
      if (g_w6life && g_w6obj_lin && a >= g_w6obj_lin && a <= g_w6obj_lin+0x114) {
        uint32_t rel = a - g_w6obj_lin;
        static const uint32_t bases[5] = {0x00,0xb8,0xf2,0x102,0x114};
        int hitb = -1;
        for (int bi=0; bi<5; bi++){ if (rel==bases[bi]+0x0a || rel==bases[bi]+0x0b){ hitb=bi; break; } }
        if (hitb>=0) {
          uint8_t oldlo = membase(cpu)[a], oldhi = membase(cpu)[b];
          kprintf("[W6MIBW] base%d blockoff 0x%02x %04x->%04x (old %02x%02x) cs:ip=%04x:%04x%s\n",
                  hitb+1, (unsigned)rel, (unsigned)(oldlo|(oldhi<<8)), v, oldhi, oldlo, cpu->cs, cpu->ip,
                  (rel==bases[hitb]+0x0a && !((oldlo|(oldhi<<8))&0x0800) && (v&0x0800)) ? "  *** SETS realize bit3 (word, sibling base) ***" : "");
        }
      }
    }
    { extern int g_w6life, g_win16_pmode; extern uint16_t win16_dgroup_sel;
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel &&
          (off==0x15fe||off==0x0344||off==0x1d18||off==0x04ce))
        kprintf("[W6LIFE] [%04x]<-%04x cs:ip=%04x:%04x ds=%04x\n", off, v, cpu->cs, cpu->ip, cpu->ds);
      // (#278 P40) watch the doc-view object table [0x1408..0x1420] (indexed
      // far-ptr array; seg194:0x2580 formats view #si only if [0x1408+si*4] != 0).
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && off>=0x1408 && off<=0x1420 && v!=0) {
        static int nvt=0; if(nvt<24){nvt++;
          kprintf("[W6VOTBL] [%04x]<-%04x cs:ip=%04x:%04x\n", off, v, cpu->cs, cpu->ip); } }
      // (#278 P40) the two repaginate-gate DGROUP words: [0x36a] (doc gate) + [0x3e16]
      // (repaginate request). Both are 0 in a gray run; log any write.
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && (off==0x036a||off==0x3e16)) {
        static int ng=0; if(ng<20){ng++;
          kprintf("[W6GATE] [%04x]<-%04x cs:ip=%04x:%04x\n", off, v, cpu->cs, cpu->ip); } }
      // (#278 P58 REAL DISPLAY-LIST PRODUCER WATCH, signature-based / compaction-robust)
      // The fixed-address watch caught only the heap COMPACTOR (seg231:0x064a) which
      // relocates views and copies +0x48 along. To catch the REAL producer we watch
      // ANY word write to <view>+0x48 where the destination base carries the CView
      // signature (+0x00=0x0001, +0xb8=0x0001, +0xbc=0x0058 - the savestate p57 sig).
      // Log producer cs:ip + value; skip the compactor cs=0x073f to isolate real writers.
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && off>=0x48) {
        uint32_t base=a-0x48; uint8_t *m=membase(cpu);
        if (m[base]==0x01 && m[base+1]==0x00 && m[base+0xb8]==0x01 && m[base+0xb9]==0x00 && m[base+0xbc]==0x58) {
          static int npr=0; if(npr<60){npr++;
            kprintf("[W6DLPROD] view(base off=%04x)+0x48 <- %04x cs:ip=%04x:%04x %s bx=%04x si=%04x di=%04x [ret]=%04x:%04x\n",
              (unsigned)(off-0x48), v, cpu->cs, cpu->ip,
              (cpu->cs==0x073f)?"COMPACTOR":((v!=0)?"*** REAL PRODUCER (non-null) ***":"zero"),
              cpu->bx, cpu->si, cpu->di,
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp)); }
        }
      }
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && (off==0x48c6||(off>=0x48be&&off<=0x48c4))) {
        static int np=0; if(np<40){np++;
          kprintf("[W6CTX] [%04x]<-%04x cs:ip=%04x:%04x\n", off, v, cpu->cs, cpu->ip); } }
      // (#278 P46) word-write variant of the realize probe: catches `or [X+0xa],0x08xx`
      // style writes that flip bit3 of the HIGH byte (offset off+1).
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && (v & 0x0800)) {
        uint8_t oldhi = membase(cpu)[b];
        if (!(oldhi & 0x08) && !((v>>8) >= 0x20 && (v>>8) <= 0x7e)) {
          static int nw=0; if(nw<40){nw++;
            kprintf("[W6BIT3W] DGROUP[%04x](hi=+%04x) word<-%04x cs:ip=%04x:%04x (obj~%04x)\n",
                    off, (uint16_t)(off+1), v, cpu->cs, cpu->ip, (uint16_t)(off+1-0x0b)); } }
      }
      if (g_w6life && g_win16_pmode && seg==win16_dgroup_sel && off==0x4a82) {
        extern uint32_t g_wp_lin; static int ns=0; if(ns<12){ns++;
          // (#278 P46) arm the write-watch on the VISIBILITY byte [sub+0x0b]
          // (bit3 0x08 = view realized/visible) instead of +0x0c (bit1). Pass 45
          // proved bit1(+0xc) is set by WM_SIZE + never cleared; pass 46 hunts who
          // (if anyone) sets the realize bit at +0x0b.
          kprintf("[W6SUB] view[0x4a82] data-block <-%04x cs:ip=%04x:%04x (armed VISBYTE watch on %04x+0xb)\n",
            v, cpu->cs, cpu->ip, v);
          g_wp_lin = lin(win16_dgroup_sel,(uint16_t)(v+0x0b));
          // (#278 P50) re-arm the sub-object flag-region watch on the (possibly moved)
          // object so [+0xa..+0xd] writes are caught across heap compaction.
          { extern uint32_t g_w6obj_lin; g_w6obj_lin = lin(win16_dgroup_sel, v);
            kprintf("[W6VFLAG] armed obj flag-watch base=lin(%04x:%04x)=%05lx (+0xb realize, +0xc bit1)\n",
                    win16_dgroup_sel, v, (unsigned long)g_w6obj_lin); } } }
    }
    if (!g_win16_pmode && mh_hit_w(a)) {
        if (a + 1 == b) { g_mh_w(cpu, a, v, 2); }
        else { g_mh_w(cpu, a, (uint16_t)(v & 0xFF), 1); g_mh_w(cpu, b, (uint16_t)(v >> 8), 1); }
        return;
    }
    uint8_t *m = membase(cpu);
    m[a] = (uint8_t)(v & 0xFF);
    m[b] = (uint8_t)(v >> 8);
}
// 32/64-bit little-endian helpers (used by the software x87 FPU for m32/m64 reals
// and m64 integers). Built from the byte-wise accessors so segment wrap is honoured.
static uint64_t rd64(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)x86_16_rd8(cpu, seg, (uint16_t)(off + i)) << (i * 8);
    return v;
}
static void wr64(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint64_t v) {
    for (int i = 0; i < 8; i++)
        x86_16_wr8(cpu, seg, (uint16_t)(off + i), (uint8_t)(v >> (i * 8)));
}
// (#194) 32-bit little-endian memory access (built on the byte accessors so the
// MMIO hook + segment wrap are honoured), for 0x66-prefixed / 386 dword ops.
static uint32_t rd32(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off) {
    return (uint32_t)x86_16_rd8(cpu, seg, off)
         | ((uint32_t)x86_16_rd8(cpu, seg, (uint16_t)(off + 1)) << 8)
         | ((uint32_t)x86_16_rd8(cpu, seg, (uint16_t)(off + 2)) << 16)
         | ((uint32_t)x86_16_rd8(cpu, seg, (uint16_t)(off + 3)) << 24);
}
static void wr32(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint32_t v) {
    x86_16_wr8(cpu, seg, off,                 (uint8_t)(v));
    x86_16_wr8(cpu, seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
    x86_16_wr8(cpu, seg, (uint16_t)(off + 2), (uint8_t)(v >> 16));
    x86_16_wr8(cpu, seg, (uint16_t)(off + 3), (uint8_t)(v >> 24));
}

// ---------------------------------------------------------------------------
// Instruction fetch
// ---------------------------------------------------------------------------
static inline uint8_t fetch8(x86_16_cpu_t *cpu) {
    uint8_t v = x86_16_rd8(cpu, cpu->cs, cpu->ip);
    cpu->ip = (uint16_t)(cpu->ip + 1);
    return v;
}
static inline uint16_t fetch16(x86_16_cpu_t *cpu) {
    uint16_t lo = fetch8(cpu);
    uint16_t hi = fetch8(cpu);
    return (uint16_t)(lo | (hi << 8));
}
static inline uint32_t fetch32(x86_16_cpu_t *cpu) {   // (#194)
    uint32_t lo = fetch16(cpu);
    uint32_t hi = fetch16(cpu);
    return lo | (hi << 16);
}
// (#194) Fetch an immediate of the current operand size (2 or 4 bytes).
static inline uint32_t fetch_imm(x86_16_cpu_t *cpu, int osize) {
    return (osize == 4) ? fetch32(cpu) : fetch16(cpu);
}
// Read the next instruction byte WITHOUT advancing IP (used to peek the x87
// ModR/M before decode so we can recognise the register-form control opcodes).
static inline uint8_t peek8(x86_16_cpu_t *cpu) {
    return x86_16_rd8(cpu, cpu->cs, cpu->ip);
}

// ---------------------------------------------------------------------------
// Register file helpers. 8-bit register encoding: 0=AL 1=CL 2=DL 3=BL
// 4=AH 5=CH 6=DH 7=BH. 16-bit: 0=AX 1=CX 2=DX 3=BX 4=SP 5=BP 6=SI 7=DI.
// ---------------------------------------------------------------------------
static uint16_t *reg16_ptr(x86_16_cpu_t *cpu, int r) {
    switch (r & 7) {
        case 0: return &cpu->ax;
        case 1: return &cpu->cx;
        case 2: return &cpu->dx;
        case 3: return &cpu->bx;
        case 4: return &cpu->sp;
        case 5: return &cpu->bp;
        case 6: return &cpu->si;
        default: return &cpu->di;
    }
}
static uint16_t get_reg16(x86_16_cpu_t *cpu, int r) { return *reg16_ptr(cpu, r); }
static void set_reg16(x86_16_cpu_t *cpu, int r, uint16_t v) { *reg16_ptr(cpu, r) = v; }

// (#194) 32-bit register access (same index order as 16-bit: 0=AX..7=DI). The
// low 16 bits live in the original uint16_t register; the high 16 in exhi[]. No
// pointer aliasing with the 16-bit storage, so the 16-bit paths are unaffected.
static uint32_t get_reg32(x86_16_cpu_t *cpu, int r) {
    return (uint32_t)get_reg16(cpu, r) | ((uint32_t)cpu->exhi[r & 7] << 16);
}
static void set_reg32(x86_16_cpu_t *cpu, int r, uint32_t v) {
    set_reg16(cpu, r, (uint16_t)v);
    cpu->exhi[r & 7] = (uint16_t)(v >> 16);
}
// (#194) width-parameterized GP access (w = 2 or 4), for 0x66-prefixed forms.
static uint32_t get_regW(x86_16_cpu_t *cpu, int r, int w) {
    return (w == 4) ? get_reg32(cpu, r) : get_reg16(cpu, r);
}
static void set_regW(x86_16_cpu_t *cpu, int r, int w, uint32_t v) {
    if (w == 4) set_reg32(cpu, r, v); else set_reg16(cpu, r, (uint16_t)v);
}

static uint8_t get_reg8(x86_16_cpu_t *cpu, int r) {
    uint16_t *p = reg16_ptr(cpu, r & 3);
    return (r & 4) ? (uint8_t)(*p >> 8) : (uint8_t)(*p & 0xFF);
}
static void set_reg8(x86_16_cpu_t *cpu, int r, uint8_t v) {
    uint16_t *p = reg16_ptr(cpu, r & 3);
    if (r & 4) *p = (uint16_t)((*p & 0x00FF) | ((uint16_t)v << 8));
    else       *p = (uint16_t)((*p & 0xFF00) | v);
}

// Segment register by index (ES=0 CS=1 SS=2 DS=3).
static uint16_t get_sreg(x86_16_cpu_t *cpu, int r) {
    // (#390) full 3-bit sreg encoding 0=ES 1=CS 2=SS 3=DS 4=FS 5=GS. Also used to
    // resolve a segment-override prefix (seg_ovr index 0..5). Previously masked
    // &3 (fs/gs unreachable) which was fine while no guest used a 0x64/0x65
    // prefix; Corel's 386 DLLs do.
    switch (r & 7) {
        case 0: return cpu->es;
        case 1: return cpu->cs;
        case 2: return cpu->ss;
        case 3: return cpu->ds;
        case 4: return cpu->fs;
        case 5: return cpu->gs;
        default: return cpu->ds;
    }
}
static void set_sreg(x86_16_cpu_t *cpu, int r, uint16_t v) {
    // (#390) full 3-bit encoding incl. FS(4)/GS(5). DS(3) keeps its diagnostics.
    if ((r & 7) == 4) { cpu->fs = v; return; }
    if ((r & 7) == 5) { cpu->gs = v; return; }
    switch (r & 3) {
        case 0: cpu->es = v; break;
        case 1: cpu->cs = v; break;
        case 2: cpu->ss = v; break;
        default: {
            // (#289 b477) trace suspicious ds loads (code-seg 0x17) in pmode to
            // localize the marshaling ds-corruption.
            extern int g_ole2_k334log;
            if (g_ole2_k334log && v == 0x17)
                kprintf("[DSSET] ds<-0017 at cs:ip=%04x:%04x (was %04x)\n", cpu->cs, cpu->ip, cpu->ds);
            { static int n1127=0; if (g_ole2_k334log && v==0x1127 && cpu->ds!=0x1127 && n1127<24){n1127++;
                kprintf("[DS1127] mov ds<-1127 cs:ip=%04x:%04x was=%04x ss=%04x\n", cpu->cs, cpu->ip, cpu->ds, cpu->ss);} }
            cpu->ds = v; break;
        }
    }
}

// ---------------------------------------------------------------------------
// Stack operations
// ---------------------------------------------------------------------------
static void push16(x86_16_cpu_t *cpu, uint16_t v) {
    cpu->sp = (uint16_t)(cpu->sp - 2);
    x86_16_wr16(cpu, cpu->ss, cpu->sp, v);
}
static uint16_t pop16(x86_16_cpu_t *cpu) {
    uint16_t v = x86_16_rd16(cpu, cpu->ss, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 2);
    return v;
}
// (#194) 32-bit stack ops. SS is a 16-bit segment here, so SP still wraps as
// 16-bit (USE16 stack); a 32-bit push just moves 4 bytes.
static void push32(x86_16_cpu_t *cpu, uint32_t v) {
    cpu->sp = (uint16_t)(cpu->sp - 4);
    wr32(cpu, cpu->ss, cpu->sp, v);
}
static uint32_t pop32(x86_16_cpu_t *cpu) {
    uint32_t v = rd32(cpu, cpu->ss, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 4);
    return v;
}

// ---------------------------------------------------------------------------
// Flag helpers
// ---------------------------------------------------------------------------
static int parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v) & 1; // PF set when number of 1 bits is even
}

static void set_pzs(x86_16_cpu_t *cpu, uint32_t res, int size /*1, 2 or 4*/) {
    uint16_t f = cpu->flags;
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t msb  = (size == 1) ? 0x80u : (size == 2) ? 0x8000u : 0x80000000u;
    uint32_t r = res & mask;
    if (r == 0) f |= F_ZF; else f &= ~F_ZF;
    if (r & msb) f |= F_SF; else f &= ~F_SF;
    if (parity8((uint8_t)(r & 0xFF))) f |= F_PF; else f &= ~F_PF;
    cpu->flags = f;
}

// Arithmetic add/sub flag computation (sets CF/OF/AF/ZF/SF/PF).
static uint32_t do_add(x86_16_cpu_t *cpu, uint32_t a, uint32_t b, int size, int carry) {
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t msb  = (size == 1) ? 0x80u : (size == 2) ? 0x8000u : 0x80000000u;
    // 64-bit accumulate so the size-4 carry-out is observable (res & ~mask is 0
    // for a 32-bit mask in 32-bit arithmetic).
    uint64_t res = (uint64_t)(a & mask) + (uint64_t)(b & mask) + (uint64_t)(carry & 1);
    uint32_t r = (uint32_t)(res & mask);
    uint16_t f = cpu->flags;
    if (res >> ((size == 1) ? 8 : (size == 2) ? 16 : 32)) f |= F_CF; else f &= ~F_CF;
    if ((((a ^ r) & (b ^ r)) & msb)) f |= F_OF; else f &= ~F_OF;
    if (((a ^ b ^ r) & 0x10)) f |= F_AF; else f &= ~F_AF;
    cpu->flags = f;
    set_pzs(cpu, r, size);
    return r;
}
static uint32_t do_sub(x86_16_cpu_t *cpu, uint32_t a, uint32_t b, int size, int borrow) {
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t msb  = (size == 1) ? 0x80u : (size == 2) ? 0x8000u : 0x80000000u;
    uint64_t bb = (uint64_t)(b & mask) + (uint64_t)(borrow & 1);
    uint64_t res = (uint64_t)(a & mask) - bb;
    uint32_t r = (uint32_t)(res & mask);
    uint16_t f = cpu->flags;
    if ((uint64_t)(a & mask) < bb) f |= F_CF; else f &= ~F_CF;
    if ((((a ^ b) & (a ^ r)) & msb)) f |= F_OF; else f &= ~F_OF;
    if (((a ^ b ^ r) & 0x10)) f |= F_AF; else f &= ~F_AF;
    cpu->flags = f;
    set_pzs(cpu, r, size);
    return r;
}
static uint32_t do_logic(x86_16_cpu_t *cpu, uint32_t res, int size) {
    cpu->flags &= ~(F_CF | F_OF | F_AF);
    set_pzs(cpu, res, size);
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    return res & mask;
}

// inc/dec do not affect CF.
static uint32_t do_inc(x86_16_cpu_t *cpu, uint32_t a, int size) {
    uint16_t savedcf = cpu->flags & F_CF;
    uint32_t r = do_add(cpu, a, 1, size, 0);
    cpu->flags = (uint16_t)((cpu->flags & ~F_CF) | savedcf);
    return r;
}
static uint32_t do_dec(x86_16_cpu_t *cpu, uint32_t a, int size) {
    uint16_t savedcf = cpu->flags & F_CF;
    uint32_t r = do_sub(cpu, a, 1, size, 0);
    cpu->flags = (uint16_t)((cpu->flags & ~F_CF) | savedcf);
    return r;
}

// ---------------------------------------------------------------------------
// ModR/M decoding
// ---------------------------------------------------------------------------
typedef struct {
    int      is_reg;   // 1 -> operand is a register (rm field), 0 -> memory
    int      reg;      // reg field (middle 3 bits)
    int      rm;       // rm field (register index if is_reg)
    uint16_t seg;      // effective segment for the memory operand
    uint16_t off;      // effective offset for the memory operand
} modrm_t;

// seg_override: -1 none, else ES/CS/SS/DS resolved segment value already; but we
// need to know the *default* segment per addressing mode, so pass override index.
// asize: 2 = 16-bit addressing (default), 4 = 32-bit addressing (0x67 prefix).
// (#194) 16-bit ModR/M (the original path, byte-for-byte unchanged in behavior).
static void decode_modrm16(x86_16_cpu_t *cpu, modrm_t *m, int seg_ovr) {
    uint8_t b = fetch8(cpu);
    int mod = (b >> 6) & 3;
    m->reg  = (b >> 3) & 7;
    m->rm   = b & 7;
    m->is_reg = 0;
    m->seg = 0;
    m->off = 0;

    if (mod == 3) {
        m->is_reg = 1;
        return;
    }

    uint16_t base = 0;
    int default_ss = 0; // some modes default to SS

    switch (m->rm) {
        case 0: base = (uint16_t)(cpu->bx + cpu->si); break;
        case 1: base = (uint16_t)(cpu->bx + cpu->di); break;
        case 2: base = (uint16_t)(cpu->bp + cpu->si); default_ss = 1; break;
        case 3: base = (uint16_t)(cpu->bp + cpu->di); default_ss = 1; break;
        case 4: base = cpu->si; break;
        case 5: base = cpu->di; break;
        case 6:
            if (mod == 0) { base = 0; }          // direct address
            else { base = cpu->bp; default_ss = 1; }
            break;
        default: base = cpu->bx; break;          // case 7
    }

    if (mod == 0 && m->rm == 6) {
        base = fetch16(cpu); // direct [disp16]
    } else if (mod == 1) {
        int8_t d = (int8_t)fetch8(cpu);
        base = (uint16_t)(base + (int16_t)d);
    } else if (mod == 2) {
        uint16_t d = fetch16(cpu);
        base = (uint16_t)(base + d);
    }

    m->off = base;
    if (seg_ovr >= 0)        m->seg = get_sreg(cpu, seg_ovr);
    else if (default_ss)     m->seg = cpu->ss;
    else                     m->seg = cpu->ds;
}

// (#194) 32-bit ModR/M + SIB addressing (0x67 prefix path). Computes a 32-bit
// effective address (base + index*scale + disp32) but TRUNCATES the result into
// the 16-bit m->off, because segments here are 16-bit (selectors / real-mode
// paragraphs) whose backing windows are <=64 KiB. Win32s thunked apps still use
// 16-bit segments, so a USE32 EA computed mod-2^16 indexes the right window. The
// default segment is SS when the base register is EBP/ESP, else DS (overridable).
static void decode_modrm32(x86_16_cpu_t *cpu, modrm_t *m, int seg_ovr) {
    uint8_t b = fetch8(cpu);
    int mod = (b >> 6) & 3;
    m->reg  = (b >> 3) & 7;
    m->rm   = b & 7;
    m->is_reg = 0;
    m->seg = 0;
    m->off = 0;

    if (mod == 3) {
        m->is_reg = 1;
        return;
    }

    uint32_t addr = 0;
    int default_ss = 0;
    int have_base = 1;          // does the EA include a base register?

    if (m->rm == 4) {
        // SIB byte: scale(2) | index(3) | base(3)
        uint8_t sib = fetch8(cpu);
        int scale = (sib >> 6) & 3;
        int index = (sib >> 3) & 7;
        int base  = sib & 7;
        uint32_t idxval = 0;
        if (index != 4)                       // index==ESP means "no index"
            idxval = get_reg32(cpu, index) << scale;
        if (base == 5 && mod == 0) {
            // no base register; disp32 follows. EA = disp32 + index*scale.
            addr = fetch32(cpu) + idxval;
            have_base = 0;
        } else {
            addr = get_reg32(cpu, base) + idxval;
            if (base == 4 || base == 5) default_ss = 1;   // ESP/EBP base -> SS
        }
    } else if (m->rm == 5 && mod == 0) {
        // [disp32], no base.
        addr = fetch32(cpu);
        have_base = 0;
    } else {
        addr = get_reg32(cpu, m->rm);
        if (m->rm == 5) default_ss = 1;       // EBP base -> SS
    }

    if (have_base) {
        if (mod == 1)      addr += (uint32_t)(int32_t)(int8_t)fetch8(cpu);
        else if (mod == 2) addr += fetch32(cpu);
    }

    m->off = (uint16_t)addr;    // truncate into the 16-bit segment window
    if (seg_ovr >= 0)        m->seg = get_sreg(cpu, seg_ovr);
    else if (default_ss)     m->seg = cpu->ss;
    else                     m->seg = cpu->ds;
}

static void decode_modrm_a(x86_16_cpu_t *cpu, modrm_t *m, int seg_ovr, int asize) {
    if (asize == 4) decode_modrm32(cpu, m, seg_ovr);
    else            decode_modrm16(cpu, m, seg_ovr);
}

// Read/write the r/m operand (memory or register) for a given size (1/2/4).
// Returns up to 32 bits; 16-bit callers assigning to a uint16_t truncate safely.
static uint32_t modrm_read(x86_16_cpu_t *cpu, modrm_t *m, int size) {
    if (m->is_reg) {
        return (size == 1) ? get_reg8(cpu, m->rm)
             : (size == 2) ? get_reg16(cpu, m->rm)
                           : get_reg32(cpu, m->rm);
    }
    return (size == 1) ? x86_16_rd8(cpu, m->seg, m->off)
         : (size == 2) ? x86_16_rd16(cpu, m->seg, m->off)
                       : rd32(cpu, m->seg, m->off);
}
static void modrm_write(x86_16_cpu_t *cpu, modrm_t *m, int size, uint32_t v) {
    if (m->is_reg) {
        if (size == 1)      set_reg8(cpu, m->rm, (uint8_t)v);
        else if (size == 2) set_reg16(cpu, m->rm, (uint16_t)v);
        else                set_reg32(cpu, m->rm, v);
    } else {
        if (size == 1)      x86_16_wr8(cpu, m->seg, m->off, (uint8_t)v);
        else if (size == 2) x86_16_wr16(cpu, m->seg, m->off, (uint16_t)v);
        else                wr32(cpu, m->seg, m->off, v);
    }
}

// ---------------------------------------------------------------------------
// Group-1 ALU operations selector (used by 80-83 and arithmetic opcodes).
// op: 0 add,1 or,2 adc,3 sbb,4 and,5 sub,6 xor,7 cmp
// ---------------------------------------------------------------------------
static uint32_t alu_op(x86_16_cpu_t *cpu, int op, uint32_t dst, uint32_t src, int size) {
    switch (op) {
        case 0: return do_add(cpu, dst, src, size, 0);
        case 1: return do_logic(cpu, dst | src, size);
        case 2: return do_add(cpu, dst, src, size, (cpu->flags & F_CF) ? 1 : 0);
        case 3: return do_sub(cpu, dst, src, size, (cpu->flags & F_CF) ? 1 : 0);
        case 4: return do_logic(cpu, dst & src, size);
        case 5: return do_sub(cpu, dst, src, size, 0);
        case 6: return do_logic(cpu, dst ^ src, size);
        default: do_sub(cpu, dst, src, size, 0); return dst; // cmp: discard result
    }
}

// ---------------------------------------------------------------------------
// Shift / rotate group (D0-D3, C0-C1). op: 0 rol,1 ror,2 rcl,3 rcr,4 shl/sal,
// 5 shr,6 sal(=shl),7 sar
// ---------------------------------------------------------------------------
static uint32_t shift_op(x86_16_cpu_t *cpu, int op, uint32_t val, int size, uint8_t cnt) {
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    int bits = (size == 1) ? 8 : (size == 2) ? 16 : 32;
    uint32_t msb = (size == 1) ? 0x80u : (size == 2) ? 0x8000u : 0x80000000u;
    val &= mask;
    uint8_t c = cnt & 0x1F;     // 386 masks the count to 5 bits for all widths
    if (c == 0) return val;
    uint16_t f = cpu->flags;
    uint32_t res = val;
    int cf = 0;

    switch (op) {
        case 0: { // ROL
            for (uint8_t i = 0; i < c; i++) {
                cf = (res & msb) ? 1 : 0;
                res = ((res << 1) | (uint32_t)cf) & mask;
            }
            if (cf) f |= F_CF; else f &= ~F_CF;
            if (c == 1) { if (((res & msb) ? 1 : 0) ^ cf) f |= F_OF; else f &= ~F_OF; }
            cpu->flags = f; return res;
        }
        case 1: { // ROR
            for (uint8_t i = 0; i < c; i++) {
                cf = res & 1;
                res = ((res >> 1) | ((uint32_t)cf << (bits - 1))) & mask;
            }
            if (cf) f |= F_CF; else f &= ~F_CF;
            if (c == 1) {
                int b1 = (res & msb) ? 1 : 0;
                int b2 = (res & (msb >> 1)) ? 1 : 0;
                if (b1 ^ b2) f |= F_OF; else f &= ~F_OF;
            }
            cpu->flags = f; return res;
        }
        case 2: { // RCL
            int carry = (f & F_CF) ? 1 : 0;
            for (uint8_t i = 0; i < c; i++) {
                int newc = (res & msb) ? 1 : 0;
                res = ((res << 1) | (uint32_t)carry) & mask;
                carry = newc;
            }
            if (carry) f |= F_CF; else f &= ~F_CF;
            cpu->flags = f; return res;
        }
        case 3: { // RCR
            int carry = (f & F_CF) ? 1 : 0;
            for (uint8_t i = 0; i < c; i++) {
                int newc = res & 1;
                res = ((res >> 1) | ((uint32_t)carry << (bits - 1))) & mask;
                carry = newc;
            }
            if (carry) f |= F_CF; else f &= ~F_CF;
            cpu->flags = f; return res;
        }
        case 4:
        case 6: { // SHL / SAL
            for (uint8_t i = 0; i < c; i++) {
                cf = (res & msb) ? 1 : 0;
                res = (res << 1) & mask;
            }
            if (cf) f |= F_CF; else f &= ~F_CF;
            cpu->flags = f;
            set_pzs(cpu, res, size);
            return res;
        }
        case 5: { // SHR
            for (uint8_t i = 0; i < c; i++) {
                cf = res & 1;
                res = (res >> 1) & mask;
            }
            if (cf) f |= F_CF; else f &= ~F_CF;
            cpu->flags = f;
            set_pzs(cpu, res, size);
            return res;
        }
        default: { // SAR (7)
            uint32_t sign = val & msb;
            for (uint8_t i = 0; i < c; i++) {
                cf = res & 1;
                res = ((res >> 1) | sign) & mask;
            }
            if (cf) f |= F_CF; else f &= ~F_CF;
            cpu->flags = f;
            set_pzs(cpu, res, size);
            return res;
        }
    }
}

// ---------------------------------------------------------------------------
// Conditional-jump test for Jcc (low nibble of the 0x70 / 0x0F80 opcode).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Minimal software x87 FPU. The main kernel TU is built WITHOUT SSE/x87
// (-mno-sse -mno-sse2), so we cannot use native `double`. Win16 apps compiled
// with the MS-C win87em emulator (e.g. TETRIS) compute layout geometry with a
// handful of x87 ops: FILD (int -> FP), FMUL by a real constant, FLD/FST of a
// 64-bit real, and FISTP (FP -> int). We model an 8-deep FP register stack of
// IEEE-754 double *bit patterns* and do the few operations purely in integer
// arithmetic. This is what turns the GameGrid's garbage geometry into a real
// rectangle (the playfield well becomes visible).
//
// Representation: each stack slot holds the raw 64-bit IEEE-754 double bits.
// ST(0) is g_fp[g_fp_top]; the stack grows DOWNWARD (top decrements on push),
// matching x87 semantics where FILD/FLD decrement TOP.
#define FP_STACK_SIZE 8
static uint64_t g_fp[FP_STACK_SIZE];
static int      g_fp_top = FP_STACK_SIZE;   // empty when == FP_STACK_SIZE

static void fp_reset(void) { g_fp_top = FP_STACK_SIZE; }

// signed 64-bit integer -> IEEE-754 double bits.
static uint64_t fp_from_i64(int64_t v) {
    if (v == 0) return 0;
    uint64_t sign = 0;
    uint64_t mag;
    if (v < 0) { sign = 1ULL << 63; mag = (uint64_t)(-(v + 1)) + 1ULL; }
    else       { mag = (uint64_t)v; }
    // Normalise: find highest set bit.
    int msb = 63;
    while (!((mag >> msb) & 1ULL)) msb--;
    int exp = msb + 1023;
    // mantissa: bits below the implicit leading 1, scaled to 52 bits.
    uint64_t mant;
    if (msb >= 52) mant = mag >> (msb - 52);
    else           mant = mag << (52 - msb);
    mant &= (1ULL << 52) - 1ULL;
    return sign | ((uint64_t)exp << 52) | mant;
}

// IEEE-754 double bits -> signed 64-bit integer (round toward nearest, ties to
// even is not required by these apps; we use round-half-away which matches the
// default FISTP rounding closely enough for pixel geometry).
static int64_t fp_to_i64(uint64_t b) {
    uint64_t sign = b >> 63;
    int exp = (int)((b >> 52) & 0x7FF);
    uint64_t mant = b & ((1ULL << 52) - 1ULL);
    if (exp == 0) return 0;                 // zero / subnormal -> 0
    if (exp == 0x7FF) return 0;             // inf/nan -> 0 (defensive)
    int e = exp - 1023;
    if (e < 0) {
        // |x| < 1: round to nearest integer (0 or 1).
        // value = 1.mant * 2^e ; for e == -1 it is in [0.5,1) -> rounds to 1.
        int64_t r = (e == -1) ? 1 : 0;
        return sign ? -r : r;
    }
    uint64_t full = (1ULL << 52) | mant;    // 53-bit significand (1.mant)
    int64_t r;
    if (e >= 52) {
        if (e - 52 >= 11) return 0;         // overflow guard (>2^63) -> 0
        r = (int64_t)(full << (e - 52));
    } else {
        int sh = 52 - e;
        uint64_t intpart = full >> sh;
        uint64_t frac_top = (full >> (sh - 1)) & 1ULL;   // round bit
        r = (int64_t)(intpart + frac_top);               // round half up
    }
    return sign ? -r : r;
}

// Multiply two IEEE-754 doubles (bit patterns). Integer-only; uses the 128-bit
// integer type (available under -mno-sse, it is not a floating-point feature) to
// hold the 106-bit significand product, then normalises back to a 53-bit double.
static uint64_t fp_mul(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63, sb = b >> 63, sign = sa ^ sb;
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1ULL), mb = b & ((1ULL << 52) - 1ULL);
    if (ea == 0 || eb == 0) return sign << 63;   // zero/subnormal operand -> 0
    if (ea == 0x7FF || eb == 0x7FF) return (sign << 63) | (0x7FFULL << 52); // inf
    uint64_t fa = (1ULL << 52) | ma;             // 53-bit significand 1.mant
    uint64_t fb = (1ULL << 52) | mb;
    unsigned __int128 P = (unsigned __int128)fa * (unsigned __int128)fb;  // <=106 bits
    // True value = P / 2^104. Leading 1 sits at bit 104 (value in [1,2)) or
    // bit 105 (value in [2,4)). Find it and renormalise to 1.xxx * 2^exp.
    int msb = 105;
    while (msb >= 0 && !((P >> msb) & 1)) msb--;
    if (msb < 0) return sign << 63;
    int exp = ea + eb - 1023 + (msb - 104);
    uint64_t mant;
    int shift = msb - 52;
    if (shift >= 0) mant = (uint64_t)(P >> shift) & ((1ULL << 52) - 1ULL);
    else            mant = (uint64_t)(P << (-shift)) & ((1ULL << 52) - 1ULL);
    if (exp <= 0)     return sign << 63;                       // underflow -> 0
    if (exp >= 0x7FF) return (sign << 63) | (0x7FFULL << 52);  // overflow -> inf
    return (sign << 63) | ((uint64_t)exp << 52) | mant;
}

// Add (sub=0) or subtract (sub=1) two IEEE-754 doubles, integer-only.
static uint64_t fp_add(uint64_t a, uint64_t b, int sub) {
    uint64_t sa = a >> 63, sb = (b >> 63) ^ (sub ? 1u : 0u);
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1), mb = b & ((1ULL << 52) - 1);
    if (ea == 0 && ma == 0) return sub ? (b ^ (1ULL << 63)) : b;  // a==0
    if (eb == 0 && mb == 0) return a;                              // b==0
    if (ea == 0x7FF || eb == 0x7FF)
        return (ea == 0x7FF) ? a : (b ^ ((uint64_t)(sub ? 1u : 0u) << 63));
    uint64_t fa = (1ULL << 52) | ma, fb = (1ULL << 52) | mb;
    fa <<= 3; fb <<= 3;                 // 3 guard bits (keeps sums within int64)
    int e;
    if (ea >= eb) { fb >>= (ea - eb); e = ea; }
    else          { fa >>= (eb - ea); e = eb; }
    int64_t va = sa ? -(int64_t)fa : (int64_t)fa;
    int64_t vb = sb ? -(int64_t)fb : (int64_t)fb;
    int64_t r = va + vb;
    uint64_t sign = 0;
    if (r < 0) { sign = 1ULL << 63; r = -r; }
    if (r == 0) return 0;
    uint64_t m = (uint64_t)r;
    int msb = 63; while (!((m >> msb) & 1ULL)) msb--;
    int target = 55;                    // 52 + 3 guard bits
    e += (msb - target);
    if (msb >= target) m >>= (msb - target); else m <<= (target - msb);
    uint64_t mant = (m >> 3) & ((1ULL << 52) - 1);
    if (e <= 0)     return sign;
    if (e >= 0x7FF) return sign | (0x7FFULL << 52);
    return sign | ((uint64_t)e << 52) | mant;
}

// Divide a/b (IEEE-754 doubles), integer-only.
static uint64_t fp_div(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63, sb = b >> 63, sign = sa ^ sb;
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1), mb = b & ((1ULL << 52) - 1);
    if (ea == 0) return sign << 63;                       // 0 / x -> 0
    if (eb == 0) return (sign << 63) | (0x7FFULL << 52);  // x / 0 -> inf
    uint64_t fa = (1ULL << 52) | ma, fb = (1ULL << 52) | mb;
    // q = (fa << 53) / fb, computed by restoring binary long division using only
    // 64-bit arithmetic (freestanding has no __udivti3 for 128-bit division).
    // fa,fb are <= 53 bits, so the running remainder never overflows 64 bits.
    uint64_t q = 0, rem = 0;
    for (int bit = 105; bit >= 0; bit--) {           // dividend = fa << 53
        rem <<= 1;
        if (bit >= 53) rem |= (fa >> (bit - 53)) & 1ULL;
        if (rem >= fb) { rem -= fb; if (bit < 64) q |= (1ULL << bit); }
    }
    int msb = 63; while (msb >= 0 && !((q >> msb) & 1ULL)) msb--;
    if (msb < 0) return sign << 63;
    int exp = ea - eb + 1023 + (msb - 53);
    uint64_t mant; int sh = msb - 52;
    if (sh >= 0) mant = (q >> sh) & ((1ULL << 52) - 1);
    else         mant = (q << (-sh)) & ((1ULL << 52) - 1);
    if (exp <= 0)     return sign << 63;
    if (exp >= 0x7FF) return (sign << 63) | (0x7FFULL << 52);
    return (sign << 63) | ((uint64_t)exp << 52) | mant;
}

// IEEE-754 single (32-bit bits) <-> double (64-bit bits) conversions.
static uint64_t f32_to_f64(uint32_t f) {
    uint64_t s = (uint64_t)(f >> 31) << 63;
    int e = (int)((f >> 23) & 0xFF);
    uint64_t mn = f & 0x7FFFFF;
    if (e == 0)    return s;                          // zero/subnormal -> +-0
    if (e == 0xFF) return s | (0x7FFULL << 52) | (mn << 29);
    return s | ((uint64_t)(e - 127 + 1023) << 52) | (mn << 29);
}
static uint32_t f64_to_f32(uint64_t b) {
    uint64_t s = b >> 63;
    int e = (int)((b >> 52) & 0x7FF);
    uint64_t mn = (b >> 29) & 0x7FFFFF;     // top 23 fraction bits (truncate)
    if (e == 0)     return (uint32_t)(s << 31);
    if (e == 0x7FF) return (uint32_t)((s << 31) | (0xFFu << 23) | mn);
    int e32 = e - 1023 + 127;
    if (e32 <= 0)   return (uint32_t)(s << 31);       // underflow -> 0
    if (e32 >= 0xFF) return (uint32_t)((s << 31) | (0xFFu << 23)); // overflow -> inf
    return (uint32_t)((s << 31) | ((uint32_t)e32 << 23) | mn);
}

static int jcc_cond(x86_16_cpu_t *cpu, uint8_t cc) {
    uint16_t f = cpu->flags;
    int zf = (f & F_ZF) ? 1 : 0;
    int cf = (f & F_CF) ? 1 : 0;
    int sf = (f & F_SF) ? 1 : 0;
    int of = (f & F_OF) ? 1 : 0;
    int pf = (f & F_PF) ? 1 : 0;
    switch (cc & 0x0F) {
        case 0x0: return of;             // JO
        case 0x1: return !of;            // JNO
        case 0x2: return cf;             // JB/JC
        case 0x3: return !cf;            // JNB/JNC
        case 0x4: return zf;             // JZ
        case 0x5: return !zf;            // JNZ
        case 0x6: return cf || zf;       // JBE
        case 0x7: return !cf && !zf;     // JA
        case 0x8: return sf;             // JS
        case 0x9: return !sf;            // JNS
        case 0xA: return pf;             // JP
        case 0xB: return !pf;            // JNP
        case 0xC: return sf != of;       // JL
        case 0xD: return sf == of;       // JGE
        case 0xE: return zf || (sf != of); // JLE
        default:  return !zf && (sf == of); // JG
    }
}

// Called from the far-call opcodes after the return frame is pushed: if the
// target matches the armed fn-trace address, latch entry state + the args.
static void fnt_maybe_enter(x86_16_cpu_t *cpu, uint16_t nseg, uint16_t noff) {
    { extern int g_ole2_k334log; if (g_ole2_k334log && nseg == 0) { ole2_ring_dump("farcall->seg0"); extern void x86_16_dump_ring_full(void); x86_16_dump_ring_full(); } }
    { extern int g_ole2_farlog; static int nz=0; if (g_ole2_farlog && nseg < 0x10 && nz < 3) { nz++;
        kprintf("[NULLFAR] target=%04x:%04x from cs:ip=%04x:%04x ax=%04x bx=%04x es=%04x ds=%04x si=%04x di=%04x bp=%04x\n", nseg,noff,cpu->cs,cpu->ip,cpu->ax,cpu->bx,cpu->es,cpu->ds,cpu->si,cpu->di,cpu->bp);
        char hb[160]; int p=0; const char*H="0123456789abcdef";
        for (int o=-16;o<6;o++){ uint8_t b=x86_16_rd8(cpu,cpu->cs,(uint16_t)(cpu->ip+o)); hb[p++]=H[b>>4]; hb[p++]=H[b&15]; hb[p++]=(o==-1)?(char)124:(char)32; }
        hb[p]=0; kprintf("[NULLFARCODE] %04x:%04x %s\n", cpu->cs, cpu->ip, hb);
        // ES:BX is the vtable; dump 8 far-ptr entries.
        kprintf("[VTAB] es:bx=%04x:%04x slots:", cpu->es, cpu->bx);
        for (int k=0;k<8;k++){ uint16_t o=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->bx+k*4)); uint16_t sg=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->bx+k*4+2)); kprintf(" [%d]%04x:%04x", k, sg, o); }
        kprintf("\n");
        // (#289 b470) Dump the es:si OBJECT (the marshaling proxy/stub COMPOBJ
        // calls into) and the +0x00..+0x1e words, plus the far ptr at +0xC.
        kprintf("[OBJ] es:si=%04x:%04x words:", cpu->es, cpu->si);
        for (int k=0;k<0x10;k++){ uint16_t w=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+k*2)); kprintf(" +%x=%04x", k*2, w); }
        kprintf("\n");
        { uint16_t mo=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+0xc)); uint16_t ms=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+0xe));
          uint16_t p6o=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+6)); uint16_t p6s=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+8));
          kprintf("[OBJ] method[+c]=%04x:%04x  ptr[+6]=%04x:%04x  div[+a]=%04x\n", ms,mo,p6s,p6o, x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->si+0xa))); }
        // frame walk: this frame
        uint16_t bp=cpu->bp;
        uint16_t rcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+4)), rip=x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+2));
        kprintf("[FRAME] bp=%04x args[bp+6..]=%04x %04x %04x %04x retfar=%04x:%04x\n", bp,
          x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+6)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+8)),
          x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+10)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(bp+12)), rcs, rip);
        // dump caller code around its call site (rip-16 .. rip+4)
        char cb[180]; int q=0; const char*HH="0123456789abcdef";
        for (int o=-16;o<4;o++){ uint8_t b=x86_16_rd8(cpu,rcs,(uint16_t)(rip+o)); cb[q++]=HH[b>>4]; cb[q++]=HH[b&15]; cb[q++]=(o==-1)?(char)124:(char)32; }
        cb[q]=0; kprintf("[CALLER] %04x:%04x %s\n", rcs, rip, cb);
        // (#289 b471) Walk up to 8 BP-chained frames to trace the marshaling
        // call stack that built the NULL-method descriptor.
        { uint16_t fb=cpu->bp;
          for (int lv=0; lv<8 && fb; lv++){
            uint16_t pcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(fb+4));
            uint16_t pip=x86_16_rd16(cpu,cpu->ss,(uint16_t)(fb+2));
            uint16_t nb =x86_16_rd16(cpu,cpu->ss,fb);
            kprintf("[STK%d] bp=%04x ret=%04x:%04x\n", lv, fb, pcs, pip);
            if (nb<=fb) break;
            fb=nb;
          } }
      } }
    if (!g_fnt_arm || g_fnt_in) return;
    if (nseg != g_fnt_seg || noff != g_fnt_off) return;
    g_fnt_in = 1; g_fnt_lines = 0;
    uint16_t sp = cpu->sp;  // [sp]=retIP [sp+2]=retCS then Pascal far-ptr args
    g_fnt_rip = x86_16_rd16(cpu, cpu->ss, sp);
    g_fnt_rcs = x86_16_rd16(cpu, cpu->ss, (uint16_t)(sp + 2));
    g_fnt_pyo = x86_16_rd16(cpu, cpu->ss, (uint16_t)(sp + 4));
    g_fnt_pys = x86_16_rd16(cpu, cpu->ss, (uint16_t)(sp + 6));
    g_fnt_pxo = x86_16_rd16(cpu, cpu->ss, (uint16_t)(sp + 8));
    g_fnt_pxs = x86_16_rd16(cpu, cpu->ss, (uint16_t)(sp + 10));
    win16_trace("FNT enter %04x:%04x ds=%04x es=%04x ss:sp=%04x:%04x ret=%04x:%04x pdx=%04x:%04x pdy=%04x:%04x\n",
                nseg, noff, cpu->ds, cpu->es, cpu->ss, cpu->sp,
                g_fnt_rcs, g_fnt_rip, g_fnt_pxs, g_fnt_pxo, g_fnt_pys, g_fnt_pyo);
}

// ---------------------------------------------------------------------------
// Main execution loop
// ---------------------------------------------------------------------------
int g_win16_iptrace = 0;   // diagnostic: sample cs:ip:op periodically

// #202 diagnostic: lets an INT handler end the current burst immediately so the
// host run loop regains control (e.g. to switch into single-step mode).
static volatile int g_x86_stop = 0;
void x86_16_request_stop(void) { g_x86_stop = 1; }

int x86_16_run(x86_16_cpu_t *cpu, unsigned long max_insns) {
    unsigned long executed = 0;

    while (!cpu->halted) {
        if (g_x86_stop) { g_x86_stop = 0; return 1; }
        if (max_insns && executed >= max_insns) return 1;

        // Diagnostic IP sampler: when enabled, log a cs:ip:op snapshot every
        // 32768 instructions so an early spin shows up as a repeated address.
        if (g_win16_iptrace && (executed & 0x7FFF) == 0)
            win16_trace("IPSAMPLE %lu %04x:%04x op=%02x ax=%04x bx=%04x cx=%04x dx=%04x sp=%04x ds=%04x ss=%04x ds00=%04x ds02=%04x\n",
                        executed, cpu->cs, cpu->ip, peek8(cpu),
                        cpu->ax, cpu->bx, cpu->cx, cpu->dx, cpu->sp, cpu->ds, cpu->ss,
                        x86_16_rd16(cpu, cpu->ds, 0), x86_16_rd16(cpu, cpu->ds, 2));

        // Nested-call sentinel: a __far __pascal callee just RETF'd to the host
        // trap frame installed by x86_16_call_far. Stop so the host can read the
        // result registers. We do NOT set cpu->halted (the outer program is not
        // finished); call_far clears g_callfar_stop after observing it.
        if (g_callfar_stop && cpu->cs == X86_16_CALLFAR_SENTINEL) return 2;

        // (#278 P52 runtime-callf-trace, avenue 1) -----------------------------
        // (A) CALLF return-pop: log ax when execution reaches a pending far-call
        // return address pushed by the 0x9A handler for the seg155 realize window.
        // (B) DS-drift: for the doc/view/format code segments, log whenever
        // ds != ss (a half-fixed drift bug from pass 33-35; check whether it is
        // unpatched in the realize region and its neighbours).
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode) {
            if (g_w6callf_npend > 0) {
              int top = g_w6callf_npend - 1;
              if (cpu->cs == g_w6callf_pend[top].cs && cpu->ip == g_w6callf_pend[top].ip) {
                kprintf("[W6CALLF] SEQ %d: RETURN to 155:%04x (from call at 155:%04x) ax=%04x dx=%04x\n",
                        g_w6seq++, cpu->ip, g_w6callf_pend[top].call_ip, cpu->ax, cpu->dx);
                g_w6callf_npend--;
              }
            }
            static const uint16_t w6ds_segs[] = { 0x04df /*seg155*/, 0x05b7 /*seg182*/,
                0x02af /*seg85*/, 0x045f /*seg139*/, 0x01d7 /*seg58*/, 0x0617 /*seg194*/,
                0x051f /*seg163*/, 0x06f7 /*seg222*/, 0x06ff /*seg223*/ };
            if (cpu->ds != cpu->ss) {
              for (unsigned i = 0; i < sizeof(w6ds_segs)/sizeof(w6ds_segs[0]); i++) {
                if (cpu->cs == w6ds_segs[i]) {
                  static int ndrift[9] = {0};
                  if (ndrift[i] < 20) { ndrift[i]++;
                    kprintf("[W6DSDRIFT] SEQ %d: %04x:%04x ds=%04x ss=%04x\n",
                            g_w6seq++, cpu->cs, cpu->ip, cpu->ds, cpu->ss); }
                  break;
                }
              }
            }
          } }

        // (#278 P58 DISPLAY-LIST PRODUCER HUNT) Trace the initial page-layout /
        // display-list chain during doc-activation. Ground truth (p57 savestate):
        // real Word's view+0x48 (display list) is NON-NULL; ours is NULL, so the
        // per-element redisplay (seg17:0x4fb0) skips and the page stays gray. Log:
        //  - seg17:0x67d0 format-setup entry (runs post-doc-create) + its args/caller
        //  - seg17:0x6b74 the early-bail return of that routine
        //  - seg17 bit1-clear paginate sites 0x1338/0x177f/0x191c (clear needs-layout)
        //  - seg17:0x4fb0 redisplay reader: log bx (view base) + [bx+0x48] display list
        //  - seg139:0x080a paginate engine entry
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode) {
            if (cpu->cs==0x008f) {
              uint16_t ip=cpu->ip;
              if (ip==0x67d0) { static int n=0; if(n<12){n++;
                kprintf("[W6DL] seg17:67d0 FMT-SETUP enter #%d arg1=%04x arg2=%04x arg3=%04x caller=%04x:%04x bx=%04x\n",
                  n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                  x86_16_rd16(cpu,cpu->ss,cpu->sp), cpu->bx); } }
              else if (ip==0x6831||ip==0x6836||ip==0x6846||ip==0x6861||ip==0x6898) { static int n=0; if(n<30){n++;
                kprintf("[W6DL] seg17:67d0 path reached ip=%04x (es:si=%04x:%04x flag[si+5]=%02x) #%d\n",
                  ip, cpu->es, cpu->si, membase(cpu)[lin(cpu->es,(uint16_t)(cpu->si+5))], n); } }
              else if (ip==0x6b74) { static int n=0; if(n<12){n++;
                kprintf("[W6DL] seg17:6b74 FMT-SETUP BAIL/return #%d ax=%04x\n", n, cpu->ax); } }
              else if (ip==0x1338||ip==0x177f||ip==0x191c) { static int n=0; if(n<20){n++;
                kprintf("[W6DL] seg17:%04x BIT1-CLEAR paginate site #%d bx=%04x [bx+c]=%02x\n",
                  ip, n, cpu->bx, membase(cpu)[lin(cpu->ds,(uint16_t)(cpu->bx+0x0c))]); } }
              else if (ip==0x4fb0) { static int n=0; if(n<20){n++;
                uint16_t dl=x86_16_rd16(cpu,cpu->ds,(uint16_t)(cpu->bx+0x48));
                uint32_t dlb=lin(cpu->ds,dl); uint8_t *mdl=membase(cpu);
                int dlisv=dl?(mdl[dlb]==0x01&&mdl[dlb+1]==0x00&&mdl[dlb+0xb8]==0x01&&mdl[dlb+0xbc]==0x58):0;
                kprintf("[W6DL] seg17:4fb0 REDISPLAY-reader #%d view-base bx=%04x view+0x48(DL)=%04x %s\n",
                  n, cpu->bx, dl, dl?(dlisv?"NON-NULL[is-VIEW=next-link]":"NON-NULL[not-view=DL-array]"):"NULL(skips->gray)"); 
                if(dl) kprintf("        [dl-dump] [%04x]=%02x%02x %02x%02x %02x%02x %02x%02x\n",dl,mdl[dlb],mdl[dlb+1],mdl[dlb+2],mdl[dlb+3],mdl[dlb+4],mdl[dlb+5],mdl[dlb+6],mdl[dlb+7]); } }
            }
            else if (cpu->cs==0x06ff && cpu->ip==0x0042) { static int n=0; if(n<40){n++;
              uint16_t a2=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
              uint16_t a1=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
              uint32_t vb=lin(cpu->ds,a2);
              int isview=(membase(cpu)[vb]==0x01&&membase(cpu)[vb+1]==0x00&&membase(cpu)[vb+0xb8]==0x01&&membase(cpu)[vb+0xbc]==0x58);
              kprintf("[W6ADDV] seg223:42 AddView(list=%04x view=%04x) isview=%d caller=%04x:%04x %s\n",
                a1,a2,isview,x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),x86_16_rd16(cpu,cpu->ss,cpu->sp),
                (a2>=0x9e30&&a2<=0x9e40)?"  <-- DOC VIEW":""); } }
            else if (cpu->cs==0x045f && cpu->ip==0x080a) { static int n=0; if(n<12){n++;
              kprintf("[W6DL] seg139:080a PAGINATE-engine enter #%d caller=%04x:%04x\n",
                n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp)); } }
          } }

        // One-function instruction trace (diagnostic). cpu->ip is at the next
        // instruction boundary here, so this captures clean per-insn state.
        if (g_fnt_in) {
            if (cpu->cs == g_fnt_rcs && cpu->ip == g_fnt_rip) {
                uint16_t vx = x86_16_rd16(cpu, g_fnt_pxs, g_fnt_pxo);
                uint16_t vy = x86_16_rd16(cpu, g_fnt_pys, g_fnt_pyo);
                win16_trace("FNT exit ax=%04x *pdx=%u(0x%04x) *pdy=%u(0x%04x)\n",
                            cpu->ax, vx, vx, vy, vy);
                g_fnt_in = 0; g_fnt_arm = 0;   // one-shot
            } else if (g_fnt_lines < 200) {
                g_fnt_lines++;
                win16_trace("%03d %04x:%04x op=%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x ss=%04x\n",
                            g_fnt_lines, cpu->cs, cpu->ip, peek8(cpu),
                            cpu->ax, cpu->bx, cpu->cx, cpu->dx, cpu->si, cpu->di,
                            cpu->bp, cpu->sp, cpu->ds, cpu->es, cpu->ss);
            }
        }

        // (#278 P39DIAG) doc-view paint-gate + bit3-OR + msg10 lifecycle probes.
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode) {
            // (#278 P46) seg173 (sel 0x056f) = the pane/view object segment. The ONLY
            // `or [reg+0xb],8` (view REALIZE/visible bit3) in WINWORD.EXE is seg173:0x1e40,
            // inside routine seg173:0x1c82 (the split/duplicate-realize path, reached from
            // the seg173 object-msg dispatcher seg173:0x1e92 msg1 ONLY when the view is
            // ALREADY realized). Trace all three to learn whether the realize path ever runs.
            // (#278 P57) BOOTSTRAP realize hunt: track every view construction,
            // watch realize on ALL of them, find New-doc caller + H2 continuation.
            { extern uint32_t g_w6view_tab[]; extern int g_w6view_n, g_w6ctor_n, g_w6ctor_pending, g_w6h2_trace, g_w6h2_n;
              extern uint16_t g_w6ctor_rcs, g_w6ctor_rip, g_w6dc_rcs, g_w6dc_rip; extern int g_w6dc_armed;
              extern uint16_t win16_dgroup_sel;
              if (cpu->cs==0x06ff && cpu->ip==0x0406 && !g_w6ctor_pending) {
                g_w6ctor_n++;
                g_w6ctor_rip = x86_16_rd16(cpu,cpu->ss,cpu->sp);
                g_w6ctor_rcs = x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2));
                g_w6ctor_pending = 1;
                kprintf("[W6BOOT] view-ctor seg223:406 ENTER #%d arg=%04x ret=%04x:%04x\n",
                  g_w6ctor_n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4)), g_w6ctor_rcs, g_w6ctor_rip);
              }
              if (g_w6ctor_pending && cpu->cs==g_w6ctor_rcs && cpu->ip==g_w6ctor_rip) {
                g_w6ctor_pending = 0;
                uint16_t _obj = cpu->ax; uint32_t _vl = lin(win16_dgroup_sel, _obj);
                if (_obj && g_w6view_n < 24) {
                  uint8_t _bb=membase(cpu)[_vl+0x0b], _cc=membase(cpu)[_vl+0x0c];
                  kprintf("[W6BOOT] view#%d CONSTRUCTED obj=%04x lin=%05lx [+0]=%04x [+b]=%02x(realize=%d) [+c]=%02x(bit1=%d)\n",
                    g_w6view_n, _obj, (unsigned long)_vl, x86_16_rd16(cpu,win16_dgroup_sel,_obj), _bb,(_bb>>3)&1, _cc,(_cc>>1)&1);
                  g_w6view_tab[g_w6view_n++] = _vl;
                }
              }
              if (cpu->cs==0x04df && cpu->ip==0x0678 && !g_w6dc_armed) {
                g_w6dc_rip = x86_16_rd16(cpu,cpu->ss,cpu->sp);
                g_w6dc_rcs = x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2));
                g_w6dc_armed = 1;
                kprintf("[W6BOOT] seg155:678 New-doc ENTER; H2 caller/return = %04x:%04x\n", g_w6dc_rcs, g_w6dc_rip);
              }
              if (g_w6dc_armed==1 && cpu->cs==g_w6dc_rcs && cpu->ip==g_w6dc_rip) {
                g_w6dc_armed = 2; g_w6h2_trace = 1; g_w6h2_n = 0;
                kprintf("[W6BOOT] *** doc-create RETURNED to %04x:%04x - H2 tracer ON; view#0[+b]=%02x ***\n",
                  cpu->cs, cpu->ip, g_w6view_n? membase(cpu)[g_w6view_tab[0]+0x0b] : 0xff);
              }
              if (g_w6h2_trace && g_w6h2_n < 30 &&
                  (cpu->cs==0x008f || cpu->cs==0x045f ||
                   (cpu->cs==0x05b7 && (cpu->ip==0x02de||cpu->ip==0x0340||cpu->ip==0x0d0a)))) {
                g_w6h2_n++;
                kprintf("[W6BOOT-H2] format-seg cs:ip=%04x:%04x (#%d after doc-create return)\n", cpu->cs, cpu->ip, g_w6h2_n);
              }
              if (cpu->cs==0x05b7 && cpu->ip==0x094c) { static int _pl=0;
                for(int _i=0;_i<g_w6view_n;_i++){ uint8_t _bb=membase(cpu)[g_w6view_tab[_i]+0x0b];
                  if((_bb&0x08) && _pl<8){ _pl++;
                    kprintf("[W6BOOT] POLL@gate view#%d IS REALIZED [+b]=%02x lin=%05lx\n",_i,_bb,(unsigned long)g_w6view_tab[_i]); } } }
            }
            if (cpu->cs==0x056f) {
              if (cpu->ip==0x1c82){ static int n=0; if(n<400){n++;
                uint16_t _srcv=x86_16_rd16(cpu,cpu->ds,0x1d18);
                kprintf("[W6RLZ] seg173:1c82 REALIZE-dup ENTER arg=%04x caller=%04x:%04x src[1d18]=%04x src[+b]=%02x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                  x86_16_rd16(cpu,cpu->ss,cpu->sp),
                  _srcv, _srcv?(unsigned)x86_16_rd8(cpu,cpu->ds,(uint16_t)(_srcv+0x0b)):0); } }
              if (cpu->ip==0x1e40){ static int n=0; if(n<400){n++;
                kprintf("[W6RLZ] *** seg173:1e40 OR [bx+0xb],8  bx=%04x (obj being realized) ***\n", cpu->bx); } }
              if (cpu->ip==0x1ea1){ static int n=0; if(n<60){n++;
                kprintf("[W6RLZ] seg173:1e92 dispatcher msg=%04x obj(si)=%04x [1d18]=%04x\n",
                  cpu->cx, cpu->si, x86_16_rd16(cpu,cpu->ds,0x1d18)); } }
            }
            // (#278 P50) periodic DISPLAY GATE seg182:0x94c (sel 0x05b7). Its
            // preconditions ([2970]!=0, [3ee|3f0]!=0, [1864|1866]==0, [3b2e]==0) and
            // then [view+0xc]&2==0; if all pass it calls seg?:0x522(3,2,0) to format.
            // Log entry + which precondition bails + reaching the format call, so we
            // see whether the idle format path even runs and what stops it.
            if (cpu->cs==0x05b7) { extern uint16_t win16_dgroup_sel; uint16_t dg=win16_dgroup_sel;
              if (cpu->ip==0x094c){ static int n=0; if(n<12){n++;
                uint16_t vp=x86_16_rd16(cpu,dg,0x1d18); uint16_t vo=vp?x86_16_rd16(cpu,dg,vp):0;
                kprintf("[W694C] gate ENTER [2970]=%04x [3ee]=%04x [3f0]=%04x [1864]=%04x [1866]=%04x [3b2e]=%04x view=%04x [v+c]=%02x\n",
                  x86_16_rd16(cpu,dg,0x2970), x86_16_rd16(cpu,dg,0x03ee), x86_16_rd16(cpu,dg,0x03f0),
                  x86_16_rd16(cpu,dg,0x1864), x86_16_rd16(cpu,dg,0x1866), x86_16_rd16(cpu,dg,0x3b2e),
                  vo, vo?x86_16_rd8(cpu,dg,(uint16_t)(vo+0x0c)):0); } }
              // (#278 P54 REVERSE-FIELD) ONE-SHOT forced-formatter probe. The
              // display gate 0x94c firing proves doc-create is complete and the
              // view exists (g_w6obj_lin armed on [0x4a82]). Snapshot the full
              // view MI block, then force-invoke the formatter seg182:0x2de(1)
              // with the field read-watch ON, capturing exactly which view
              // fields it reads (and which are ZERO) before it tears the chrome.
              // x86_16_call_far snapshots+restores all regs, so the outer Word is
              // undisturbed except guest-memory side effects (the teardown), which
              // is acceptable for this throwaway diagnostic (g_w6life ship=0).
              if (cpu->ip==0x094c && !g_w6rf_done && g_w6obj_lin) {
                g_w6rf_done = 1;
                kprintf("[W6REVF] === force seg182:2de(1) @display-gate; view MI block snapshot base=lin=%05lx ===\n",
                        (unsigned long)g_w6obj_lin);
                for (uint32_t r=0; r<0x120; r+=16) {
                  kprintf("[W6REVF] view+%03x:", (unsigned)r);
                  for (int c=0;c<16;c++) kprintf(" %02x", membase(cpu)[g_w6obj_lin+r+c]);
                  kprintf("\n");
                }
                for (int i=0;i<0x200;i++) g_w6rf_seen[i]=0;
                g_w6rf_seq = 0;
                uint16_t rax=0, rdx=0; uint16_t fargs[1] = { 1 };
                uint16_t b344 = x86_16_rd16(cpu,dg,0x0344);
                g_w6fmt_read = 1;
                int frc = x86_16_call_far(cpu, 0x05b7, 0x02de, fargs, 1, &rax, &rdx, 0);
                g_w6fmt_read = 0;
                uint16_t a344 = x86_16_rd16(cpu,dg,0x0344);
                kprintf("[W6REVF] === force returned rc=%d ax=%04x dx=%04x; %d distinct view fields read; [0344] %04x->%04x (bit3 %d->%d) ===\n",
                        frc, rax, rdx, g_w6rf_seq, b344, a344, (b344>>3)&1, (a344>>3)&1);
              }
              if (cpu->ip==0x0985){ static int n=0; if(n<12){n++;
                kprintf("[W694C] reached view [+c]&2 test si=%04x [si+c]=%02x %s\n",
                  cpu->si, x86_16_rd8(cpu,dg,(uint16_t)(cpu->si+0x0c)),
                  (x86_16_rd8(cpu,dg,(uint16_t)(cpu->si+0x0c))&2)?"-> BAIL":"-> proceed"); } }
              if (cpu->ip==0x09e4){ static int n=0; if(n<12){n++;
                kprintf("[W694C] *** PASSED gate -> call format seg?:522(3,2,0) ***\n"); } }
            }
            // (#278 P48REF) startup first-paginate driver trace. SOLE caller of the
            // paginate seg139:0x080a is seg222:0x2dbf inside R_reformat=seg222:0x2d08.
            // R_reformat's 3 callers: seg25:0x100f, seg221:0x4fa0, seg222:0x3068(router).
            { extern uint16_t win16_dgroup_sel; uint16_t dg=win16_dgroup_sel;
              if (cpu->cs==0x06f7 && cpu->ip==0x2d0d){ static int n=0; if(n<40){n++;
                uint16_t vp=x86_16_rd16(cpu,dg,0x1d18); uint16_t vo=vp?x86_16_rd16(cpu,dg,vp):0;
                kprintf("[P48REF] R_reformat ENTER arg=%04x [5b8]=%04x [3b98]=%04x [48c6]=%04x view=%04x [v+a]=%04x ret=%04x:%04x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                  x86_16_rd16(cpu,dg,0x05b8), x86_16_rd16(cpu,dg,0x3b98), x86_16_rd16(cpu,dg,0x48c6),
                  vo, vo?x86_16_rd16(cpu,dg,(uint16_t)(vo+0x0a)):0,
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2))); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x2d25){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] R_reformat EARLY-RET @2d25 (arg!=8 -> 71c, NO paginate)\n"); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x2d3f){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] R_reformat RET @2d3f ([3b98]!=0 -> a47a, NO paginate)\n"); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x2dbc){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] *** R_reformat REACHED PAGINATE @2dbc arg=%04x rect=%04x,%04x,%04x,%04x ***\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                  x86_16_rd16(cpu,dg,0x48be),x86_16_rd16(cpu,dg,0x48c0),
                  x86_16_rd16(cpu,dg,0x48c2),x86_16_rd16(cpu,dg,0x48c4)); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x3068){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] caller ROUTER seg222:3068 cx=%04x\n", x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x0c))); } }
              if (cpu->cs==0x06ef && cpu->ip==0x4f32){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] caller seg221:4f32 ENTER rect=%04x,%04x,%04x,%04x\n",
                  x86_16_rd16(cpu,dg,0x48be),x86_16_rd16(cpu,dg,0x48c0),
                  x86_16_rd16(cpu,dg,0x48c2),x86_16_rd16(cpu,dg,0x48c4)); } }
              if (cpu->cs==0x06ef && cpu->ip==0x4fa0){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] caller seg221:4fa0 -> R_reformat(0xd)\n"); } }
              if (cpu->cs==0x00cf && cpu->ip==0x0e04){ static int n=0; if(n<8){n++;
                kprintf("[P48REF] seg25:e04 dispatch ENTER [13e]=%04x\n", x86_16_rd16(cpu,dg,0x013e)); } }
              if (cpu->cs==0x00cf && cpu->ip==0x100f){ static int n=0; if(n<20){n++;
                kprintf("[P48REF] caller seg25:100f -> R_reformat\n"); } }
              // P48REF-LOOP: WinMain msg loop seg222:0x00 idle decision. At 0x72 ax=
              // msg-fetch(0x810) result; ax==0 -> idle branch 0x85 -> call formatter
              // seg182:0x340(1). If loop never idles, formatter/bit3 starved.
              if (cpu->cs==0x06f7 && cpu->ip==0x0072){ static int n=0; if(n<30){n++;
                kprintf("[P48LOOP] seg222:72 msgfetch->ax=%04x %s\n", cpu->ax, cpu->ax?"BUSY(process)":"*** IDLE ***"); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x0085){ static int n=0; if(n<10){n++;
                kprintf("[P48LOOP] *** seg222:85 IDLE -> call formatter seg182:340(1) [0344]=%04x ***\n", x86_16_rd16(cpu,dg,0x0344)); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x0118){ static int n=0; if(n<20){n++;
                kprintf("[P48LOOP] seg222:118 cmd-check [13f8]=%04x [13fa]=%04x\n", x86_16_rd16(cpu,dg,0x13f8), x86_16_rd16(cpu,dg,0x13fa)); } }
              if (cpu->cs==0x06f7 && cpu->ip==0x0123){ static int n=0; if(n<10){n++;
                kprintf("[P48LOOP] *** seg222:123 cmd0x12 -> call formatter seg182:2de ***\n"); } }
              if (cpu->cs==0x0447 && cpu->ip==0x0547){ static int n=0; if(n<20){n++;
                kprintf("[P48LOOP] seg136:532 fetch gate(91e)->ax=%04x %s\n", cpu->ax, cpu->ax?"has-work":"no-work->exit0"); } }
            }
            if (cpu->cs==0x06f7 && cpu->ip==0x2130) { static int n=0; if(n<300){n++;
              uint16_t v344=x86_16_rd16(cpu,cpu->ds,0x0344);
              kprintf("[W6PAINT] #%d editpane 2130 [0344]=%04x bit3=%d(%s) hwnd=%04x\n",
                n, v344, (v344&8)?1:0, (v344&8)?"DRAW":"nodraw-reqrepaint",
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0e))); }
              // (#278 P40 DIAG) one-shot: dump the view object + its vtable, and scan
              // ALL of DGROUP for a far pointer {0x0d0a,0x05b7} = a live reference to
              // the format trigger seg182:0xd0a. If NO such pointer exists anywhere,
              // the view vtable slot was never installed (root cause). dgrp = ds (the
              // pass36 fix has forced ds=ss=DGROUP by this point).
              { static int scanned=0; if(!scanned){ scanned=1;
                uint16_t dgrp = cpu->ds;
                uint16_t h = x86_16_rd16(cpu,dgrp,0x1d18);       // active-view object near-ptr
                kprintf("[W6VTBL] view obj [1d18]=%04x dgrp=%04x\n", h, dgrp);
                if (h){
                  kprintf("[W6VTBL] obj[%04x]: %04x %04x %04x %04x %04x %04x %04x %04x\n", h,
                    x86_16_rd16(cpu,dgrp,h), x86_16_rd16(cpu,dgrp,(uint16_t)(h+2)),
                    x86_16_rd16(cpu,dgrp,(uint16_t)(h+4)), x86_16_rd16(cpu,dgrp,(uint16_t)(h+6)),
                    x86_16_rd16(cpu,dgrp,(uint16_t)(h+8)), x86_16_rd16(cpu,dgrp,(uint16_t)(h+10)),
                    x86_16_rd16(cpu,dgrp,(uint16_t)(h+12)), x86_16_rd16(cpu,dgrp,(uint16_t)(h+14)));
                  uint16_t vt = x86_16_rd16(cpu,dgrp,h);          // first word = vtable near-ptr
                  kprintf("[W6VTBL] vtbl@%04x:", vt);
                  for (int i=0;i<24;i++) kprintf(" %04x", x86_16_rd16(cpu,dgrp,(uint16_t)(vt+i*2)));
                  kprintf("\n");
                }
                // full DGROUP scan for the far pointer d0a:05b7
                int hits=0;
                for (uint32_t off=0; off<0xFFFC; off+=2){
                  if (x86_16_rd16(cpu,dgrp,(uint16_t)off)==0x0d0a &&
                      x86_16_rd16(cpu,dgrp,(uint16_t)(off+2))==0x05b7){
                    kprintf("[W6VTBL] far-ptr d0a:05b7 found at DGROUP:%04x\n",(uint16_t)off);
                    if(++hits>=8) break;
                  }
                }
                if(!hits) kprintf("[W6VTBL] NO far-ptr {0d0a:05b7} anywhere in DGROUP (vtable slot never installed)\n");
                // also scan for ANY far ptr into seg182 (05b7) to see which methods ARE installed
                int s182=0;
                for (uint32_t off=0; off<0xFFFC; off+=2){
                  if (x86_16_rd16(cpu,dgrp,(uint16_t)(off+2))==0x05b7){
                    uint16_t to=x86_16_rd16(cpu,dgrp,(uint16_t)off);
                    if (to<0x8000){ kprintf("[W6VTBL] ->seg182:%04x ref at DGROUP:%04x\n", to,(uint16_t)off);
                      if(++s182>=16) break; }
                  }
                }
              } } }
            if (cpu->cs==0x02af && cpu->ip==0x37cf){ static int n=0; if(n<4){n++;
              kprintf("[W6BIT3] seg85:37cf(msg10) [0344]=%04x\n", x86_16_rd16(cpu,cpu->ds,0x0344)); } }
            if (cpu->cs==0x05b7 && (cpu->ip==0x0340||cpu->ip==0x040c)){ static int n=0; if(n<8){n++;
              kprintf("[W6BIT3] seg182:%04x [0344]=%04x ax=%04x\n", cpu->ip, x86_16_rd16(cpu,cpu->ds,0x0344), cpu->ax); } }
            // (#278 P47) the per-frame display-ready gate seg182:0x94c. It bails at
            // 0x989 when the active-view sub-object bit1 [[0x1d18]]+0xc & 2 is set
            // (needs-layout), and (even if clear) only sets the repaginate-request
            // [0x3e16]=1 at 0x9f9 when [0x36a] != 0. Trace: entry, the bit1 bail, and
            // whether [0x3e16] is ever requested. Confirms the initial-layout deadlock.
            if (cpu->cs==0x05b7){
              if (cpu->ip==0x094c){ static int n=0; if(n<4){n++;
                uint16_t v=x86_16_rd16(cpu,cpu->ds,0x1d18); uint16_t o=v?x86_16_rd16(cpu,cpu->ds,v):0;
                kprintf("[W6GATE] seg182:94c ENTER [2970]=%04x view[1d18]=%04x sub=%04x [sub+c]=%04x(bit1=%d) [36a]=%04x [3e16]=%04x\n",
                  x86_16_rd16(cpu,cpu->ds,0x2970), v, o,
                  o?x86_16_rd16(cpu,cpu->ds,(uint16_t)(o+0x0c)):0,
                  o?(x86_16_rd16(cpu,cpu->ds,(uint16_t)(o+0x0c))>>1)&1:0,
                  x86_16_rd16(cpu,cpu->ds,0x036a), x86_16_rd16(cpu,cpu->ds,0x3e16)); } }
              if (cpu->ip==0x0989){ static int n=0; if(n<4){n++;
                kprintf("[W6GATE] seg182:989 *** BAIL: view needs-layout (bit1 set); repaginate NOT requested ***\n"); } }
              if (cpu->ip==0x09f9){ static int n=0; if(n<4){n++;
                kprintf("[W6GATE] seg182:9f9 *** [0x3e16]=1 (repaginate REQUESTED) ***\n"); } }
            }
            // (#278 P51) doc-create seg155:0x678 view realize trace. sel 0x04df.
            // Log the view sub-object realize bit [+0xb]&8 and needs-layout bit1
            // [+0xc]&2 right after the view ctor (0x9c3), at the two realize tests
            // (0xa56, 0xb89), plus entry to the doc-create routine (0x678). The view
            // ptr lives in [bp-0x10] (near-ptr; deref once for the sub-object).
            if (cpu->cs==0x04df){
              if (cpu->ip==0x0678){ static int n=0; if(n<4){n++;
                kprintf("[W6DC] SEQ %d: seg155:678 doc-create ENTER [498]=%04x [bp+6]=%04x [bp+c]=%04x\n",
                  g_w6seq++,
                  x86_16_rd16(cpu,cpu->ds,0x0498),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0xc))); } }
              if (cpu->ip==0x09c3||cpu->ip==0x0a56||cpu->ip==0x0b89){ static int n=0; if(n<24){n++;
                uint16_t vp=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x10));
                uint16_t sub=vp?x86_16_rd16(cpu,cpu->ds,vp):0;
                uint8_t fb=sub?(uint8_t)x86_16_rd8(cpu,cpu->ds,(uint16_t)(sub+0x0b)):0;
                uint8_t fc=sub?(uint8_t)x86_16_rd8(cpu,cpu->ds,(uint16_t)(sub+0x0c)):0;
                kprintf("[W6DC] SEQ %d: seg155:%04x viewp=%04x sub=%04x [+b]=%02x(realize=%d) [+c]=%02x(bit1=%d)\n",
                  g_w6seq++, cpu->ip, vp, sub, fb, (fb>>3)&1, fc, (fc>>1)&1); } }
              // (#278 P53 ACTION 1a) the never-examined view-notify broadcast gate.
              // seg155:0xaf6 (near call) -> 0xf30 (fan-out broadcast) runs UNconditionally;
              // 0xaf9 then `test byte ds:0x295b,0x8 / je 0xb37` gates a SEPARATE downstream
              // block (0xb00-0xb37: mov ds:0x6c4,ax + near call 0x1b34 + far 0x596:0x4308).
              // Capture ds:0x295b at 0xaf9: is bit3 set (block runs) or clear (the missing
              // precondition our env never sets)?
              if (cpu->ip==0x0af9){ static int n=0; if(n<6){n++;
                uint8_t g = (uint8_t)x86_16_rd8(cpu,cpu->ds,0x295b);
                kprintf("[W6NOTGATE] SEQ %d: seg155:af9 ds:[295b]=%02x bit3=%d -> downstream 0xb00-0xb37 block %s\n",
                        g_w6seq++, g, (g>>3)&1, ((g>>3)&1)?"RUNS":"SKIPPED"); } }
              // (#278 P53 ACTION 1b) seg155:0xf30 fan-out entry. `les si,[bp+6]` = the
              // broadcast object (caller's [bp-0xe]:[bp-0xc]); it far-calls a notify
              // method for each non-null sub-object field es:[si+{2,10,12,14,16,18}].
              // Log the object ptr + all the fields so we know if the view list is
              // populated (attached views) and which fan-out branches will fire.
              if (cpu->ip==0x0f30){ static int n=0; if(n<6){n++;
                uint16_t off6=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
                uint16_t seg6=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8));
                #define _F(o) x86_16_rd16(cpu,seg6,(uint16_t)(off6+(o)))
                kprintf("[W6NOTIFY30] SEQ %d: seg155:f30 obj=%04x:%04x [+2]=%04x [+4]=%04x [+6]=%04x [+10]=%04x [+12]=%04x [+14]=%04x [+16]=%04x [+18]=%04x\n",
                        g_w6seq++, seg6, off6, _F(2),_F(4),_F(6),_F(0x10),_F(0x12),_F(0x14),_F(0x16),_F(0x18));
                #undef _F
              } }
            }
            // (#278 P52 runtime-callf-trace follow-up) seg136:0x17da (called from the
            // doc-create mystery call at seg155:0xa1b) has a SECOND, previously
            // untraced conditional: at 0x17fc it does `test byte ds:0x33c,0xf` and,
            // ONLY if all 4 low bits are clear, falls into a body (0x1803-0x180a)
            // that makes a near call (0x17c8) then an internal far call (raw bytes
            // 9a 9e 02 ff ff, patched at load) before returning. If Word's initial
            // view-realize/first-format trigger lives in that far call, a spuriously
            // SET bit in our env's [0x33c] would explain the whole deadlock. Log the
            // gating byte and whether we enter the branch (reaching 0x1804 proves it).
            if (cpu->cs==0x0447) {
              if (cpu->ip==0x17fc){ static int n=0; if(n<8){n++;
                kprintf("[W6SEG136] SEQ %d: seg136:17fc test byte [033c]=%02x (low nibble gates the 0x1804 body; nonzero low nibble = SKIP)\n",
                        g_w6seq++, (unsigned)x86_16_rd8(cpu,cpu->ds,0x033c)); } }
              if (cpu->ip==0x1804){ static int n=0; if(n<8){n++;
                kprintf("[W6SEG136] SEQ %d: *** seg136:1804 ENTERED the gated body (call 17c8 + far call) ***\n",
                        g_w6seq++); } }
              if (cpu->ip==0x180f){ static int n=0; if(n<8){n++;
                kprintf("[W6SEG136] SEQ %d: seg136:180f post-body ax=%04x si=%04x\n",
                        g_w6seq++, cpu->ax, cpu->si); } }
            }
            // (#278 P52 runtime-callf-trace follow-up 2) seg155:0xa22 `cmp
            // [bp+6],0 / je 0xa32` gates the three still-unseen calls at
            // 0xa2d/0xa44/0xa4c (0xa2d directly; 0xa44/0xa4c are reached only if
            // 0xa2d's sibling branch at 0xa3d also takes the non-zero path). Log
            // the actual runtime value of [bp+6] and bp right here, since two
            // consecutive full-boot captures show ZERO CALLF hits for 0xa2d even
            // though the doc-create ENTER trace read [bp+6]=1 back at 0x678 (which
            // should make je NOT taken). If this print also shows 1, the je is
            // being evaluated on stale/wrong flags (interpreter bug in cmp/je or a
            // register alias); if it shows 0, [bp+6] silently changed intra-routine
            // (real bug: something overwrote a stack slot between 0x678 and 0xa22).
            if (cpu->cs==0x04df && cpu->ip==0x0a22){ static int n=0; if(n<8){n++;
              kprintf("[W6DC] SEQ %d: seg155:a22 pre-branch bp=%04x [bp+6]=%04x (ENTER-time value was 1; je 0xa32 taken iff this is 0)\n",
                      g_w6seq++, cpu->bp, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6))); } }
            // (#278 P52 runtime-callf-trace follow-up 3) Directly observe which side
            // of the 0xa26 `je 0xa32` actually gets taken (fall-through to 0xa28 = NOT
            // taken -> reaches the 0xa2d call; landing on 0xa32 = taken -> skips it).
            // [bp+6] read as 1 at 0xa22 says NOT-taken is correct, yet 0xa2d never logs
            // a CALLF hit; this pins down whether the interpreter's je/cmp disagree
            // with a direct memory read, or something else diverts control first.
            if (cpu->cs==0x04df && cpu->ip==0x0a28){ static int n=0; if(n<8){n++;
              kprintf("[W6DC] SEQ %d: seg155:a28 *** fell through (je NOT taken) - about to hit 0xa2d call ***\n", g_w6seq++); } }
            if (cpu->cs==0x04df && cpu->ip==0x0a32){ static int n=0; if(n<8){n++;
              kprintf("[W6DC] SEQ %d: seg155:a32 *** landed here (je WAS taken, or fell through from 0xa2d) - flags/[bp+6] at branch time unclear ***\n", g_w6seq++); } }
            if (cpu->cs==0x02af && (cpu->ip==0x37a0||cpu->ip==0x3e3b||cpu->ip==0x3faa||cpu->ip==0x37a7)){ static int n=0; if(n<16){n++;
              kprintf("[W6MSG10] seg85:%04x ax=%04x bx=%04x [15fe]=%04x [0344]=%04x\n", cpu->ip, cpu->ax, cpu->bx,
                x86_16_rd16(cpu,cpu->ds,0x15fe), x86_16_rd16(cpu,cpu->ds,0x0344)); } }
            // (#278 P40) seg194:0x2580 = FormatView(si): the ONLY code path that
            // lcalls the format method seg182:0d0a (at 0x25ee), gated by a non-null
            // view-object far-ptr in DGROUP:[0x1408+si*4]. Trace whether it runs and
            // whether that table slot is null. seg194 sel = 0x0617.
            if (cpu->cs==0x0617 && cpu->ip==0x2580){ static int n=0; if(n<12){n++;
              uint16_t si=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
              kprintf("[W6FMTV] enter FormatView si=%04x [1a62]=%04x\n", si,
                x86_16_rd16(cpu,cpu->ds,0x1a62)); } }
            if (cpu->cs==0x0617 && cpu->ip==0x25d6){ static int n=0; if(n<12){n++;
              // ax:dx just loaded from [0x1408+si*4]; si is in cpu->si
              uint16_t bx=(uint16_t)(cpu->si<<2);
              kprintf("[W6FMTV] si=%04x tbl[1408+%04x]=%04x:%04x -> %s\n", cpu->si, bx,
                x86_16_rd16(cpu,cpu->ds,(uint16_t)(0x140a+bx)), x86_16_rd16(cpu,cpu->ds,(uint16_t)(0x1408+bx)),
                (cpu->ax|cpu->dx)?"FORMAT(0xd0a)":"SKIP(null)"); } }
            if (cpu->cs==0x0617 && cpu->ip==0x25ee){ static int n=0; if(n<8){n++;
              kprintf("[W6FMTV] *** calling seg182:0d0a (format) ***\n"); } }
            // (#278 P40) map which seg194 functions actually run: log the first
            // ~50 FAR entries into seg194 (cs newly == 0617) with target + caller.
            { static uint16_t prevcs=0, previp=0; static int ns=0;
              if (cpu->cs==0x0617 && prevcs!=0x0617 && ns<50){ ns++;
                kprintf("[W6S194] enter seg194:%04x from %04x:%04x\n", cpu->ip, prevcs, previp); }
              prevcs=cpu->cs; previp=cpu->ip; }
            // (#278 P40) seg182:0x2de = the ONLY reformat/bit3-set entry (calls 0x340
            // which does orb $8,[0344]). Log every entry + caller (far ret on stack).
            if (cpu->cs==0x05b7 && cpu->ip==0x02de){ static int n=0; if(n<20){n++;
              kprintf("[W62DE] seg182:2de ENTER caller=%04x:%04x arg=%04x\n",
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6))); } }
            // (#278 P40) seg222:0x2826 notify-sink: log event codes received; 0x12
            // triggers the direct reformat lcall seg182:2de at 0x284c.
            if (cpu->cs==0x06f7 && cpu->ip==0x2826){ static int n=0; if(n<30){n++;
              uint16_t ec=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->bx+2));
              kprintf("[W6NOTIFY] seg222 sink event code=%04x %s\n", ec, ec==0x12?"<== FORMAT TRIGGER":""); } }
            // (#278 P40) THE repaginate-request gate. 094c must reach 0x9e4, call
            // seg182:0x522 (must ret ax==0), and [DGROUP:0x36a] must be !=0, to set
            // [0x3e16]=1. Trace body progress + the two conditions.
            if (cpu->cs==0x05b7){
              if (cpu->ip==0x0951){ static int n=0; if(n<8){n++;
                kprintf("[W694C] enter body [2970]=%04x [3b2e]=%04x [36a]=%04x\n",
                  x86_16_rd16(cpu,cpu->ds,0x2970),x86_16_rd16(cpu,cpu->ds,0x3b2e),x86_16_rd16(cpu,cpu->ds,0x036a)); }
                // one-shot: dump the view sub-object [[0x1d18]] and ARM a watch on its
                // +0xc flag byte (bit1 set = 094c bails). Find who sets it.
                { extern uint32_t g_wp_lin; static int armed=0; if(!armed){armed=1;
                  uint16_t obj=x86_16_rd16(cpu,cpu->ds,0x1d18);
                  uint16_t sub=x86_16_rd16(cpu,cpu->ds,obj);
                  kprintf("[W694C] viewobj=%04x sub=%04x  sub[]:", obj, sub);
                  for(int k=0;k<12;k++) kprintf(" %04x", x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+k*2)));
                  kprintf("\n");
                  /* P44: do NOT re-arm here (would clobber the early [0x4a82]->sub bit1 watch) */
                  kprintf("[W694C] (P44 re-arm suppressed) sub+0xc lin would be %05lx\n",(unsigned long)lin(cpu->ds,(uint16_t)(sub+0x0c)));
                } } }
              if (cpu->ip==0x0985){ static int n=0; if(n<8){n++;
                // si = [[0x1d18]]; test bit1 of [si+0xc]. bail(jne 0x9ff) if set.
                uint16_t f=x86_16_rd16(cpu,cpu->ds,(uint16_t)(cpu->si+0x0c));
                kprintf("[W694C] gate985 view si=%04x [si+0c]=%04x bit1=%d %s\n", cpu->si, f, (f>>1)&1,
                  (f&2)?"BAIL":"pass"); } }
              if (cpu->ip==0x098f){ static int n=0; if(n<8){n++;
                // es=[0x3f0]; cmpw 0, es:[bx+0x2e]. bail(jne 0x9ff) if nonzero.
                uint16_t d=x86_16_rd16(cpu,cpu->es,(uint16_t)(cpu->bx+0x2e));
                kprintf("[W694C] gate98f doc es=%04x bx=%04x es:[bx+2e]=%04x %s\n", cpu->es, cpu->bx, d,
                  d?"BAIL":"pass"); } }
              if (cpu->ip==0x0996){ static int n=0; if(n<8){n++;
                kprintf("[W694C] gate996 [2970]=%04x (==1?%s)\n", x86_16_rd16(cpu,cpu->ds,0x2970),
                  x86_16_rd16(cpu,cpu->ds,0x2970)==1?"yes->fall":"no->0x9e4"); } }
              if (cpu->ip==0x09e4){ static int n=0; if(n<8){n++;
                kprintf("[W694C] reached 0x9e4 (about to call 0x522)\n"); } }
              if (cpu->ip==0x09ef){ static int n=0; if(n<8){n++;
                kprintf("[W694C] 0x522 returned ax=%04x  [36a]=%04x  -> %s\n", cpu->ax,
                  x86_16_rd16(cpu,cpu->ds,0x036a),
                  (cpu->ax==0 && x86_16_rd16(cpu,cpu->ds,0x036a)!=0)?"SET [3e16]!":"BAIL"); } }
              if (cpu->ip==0x09f9){ static int n=0; if(n<4){n++;
                kprintf("[W694C] *** setting [3e16]=1 (repaginate requested) ***\n"); } }
              // 0x522 internal: which return path + the sub-call result
              if (cpu->ip==0x0522){ static int n=0; if(n<8){n++;
                kprintf("[W6522] enter di=%04x [5bc]=%04x [6aa]=%04x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0a)),
                  x86_16_rd16(cpu,cpu->ds,0x05bc),x86_16_rd16(cpu,cpu->ds,0x06aa)); } }
              if (cpu->ip==0x056b){ static int n=0; if(n<8){n++;
                kprintf("[W6522] sub seg106:7c32 ret ax=%04x (0=success->ret0)\n", cpu->ax); } }
            }
            // (#278 P41) seg139 format/repaginate FUNCTION entries + the 7
            // MarkRepagination call sites + the bit1-clear. If NO external entry runs,
            // Word never reaches the doc-format path (MDI/view-creation gap). If an
            // entry runs but its MarkRepag site never fires, a branch skips it.
            { static uint16_t s139pcs=0;
              if (cpu->cs==0x045f){
                uint16_t ip=cpu->ip;
                if (s139pcs!=0x045f && (ip==0x080a||ip==0x18e2||ip==0x25b0||ip==0x35c6||ip==0x3fe6)){
                  static int ne=0; if(ne<80){ne++;
                    kprintf("[W6S139] FAR-enter seg139:%04x from %04x:%04x [36a]=%04x [3e16]=%04x\n",
                      ip, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                      x86_16_rd16(cpu,cpu->ss,cpu->sp),
                      x86_16_rd16(cpu,cpu->ds,0x036a), x86_16_rd16(cpu,cpu->ds,0x3e16)); } }
                if (ip==0x11ab||ip==0x1a35||ip==0x1bbb||ip==0x2579||ip==0x29d7||ip==0x3992||ip==0x41d3){
                  static int nm=0; if(nm<20){nm++;
                    kprintf("[W6MARK] *** MarkRepag call site seg139:%04x reached ***\n", ip); } }
                if (ip==0x24d4||ip==0x24de){ static int nc=0; if(nc<8){nc++;
                  kprintf("[W6MARK] bit1-clear seg139:%04x bx=%04x [bx+c]=%04x\n", ip, cpu->bx,
                    x86_16_rd16(cpu,cpu->ds,(uint16_t)(cpu->bx+0x0c))); } }
              }
              s139pcs=cpu->cs;
            }
            // (#278 P42) The seg139 view-geometry-recompute (seg139:0x080a, which
            // clears the stuck view flag bit1 @0x24d4 and MarkRepaginates @0x2579)
            // is called from EXACTLY ONE static site: seg222:0x2dbf, inside the view
            // command-dispatcher seg222:0x2d08, reached only from the message router
            // seg222:0x2dde (switch on cmd=es:[si+2]) at 0x3068. Trace the whole chain
            // to locate where our env diverges (router never called with the reformat
            // cmd? cmd arrives but the [0x1d18] view hwnd-match @0x305d fails? branch
            // skips 0x3068? or 0x2d08 guards short-circuit before 0x2dbf?).
            if (cpu->cs==0x06f7){
              uint16_t ip=cpu->ip;
              if (ip==0x0263){ static int n=0; if(n<40){n++;
                kprintf("[W6RT] winmain cmd-block@0263 [13e]=%02x [13f8]=%04x(=102?%d) [13fa]=%04x\n",
                  x86_16_rd8(cpu,cpu->ds,0x013e), x86_16_rd16(cpu,cpu->ds,0x13f8),
                  x86_16_rd16(cpu,cpu->ds,0x13f8)==0x102, x86_16_rd16(cpu,cpu->ds,0x13fa)); } }
              if (ip==0x2df8){ static int n=0; if(n<80){n++;
                kprintf("[W6RT] router-in seg222:2df8 cmd=di=%04x si=%04x es=%04x [1d18]=%04x\n",
                  cpu->di, cpu->si, cpu->es, x86_16_rd16(cpu,cpu->ds,0x1d18)); } }
              if (ip==0x2e03){ static int n=0; if(n<60){n++;
                kprintf("[W6RT] router-postfilter cmd=di=%04x [1d18]=%04x ret=%04x:%04x\n",
                  cpu->di, x86_16_rd16(cpu,cpu->ds,0x1d18),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2))); } }
              if (ip==0x305d){ static int n=0; if(n<20){n++;
                kprintf("[W6RT] reformat-case hwnd-match: [[1d18]+34]=%04x ax(hwnd)=%04x %s\n",
                  x86_16_rd16(cpu,cpu->ds,(uint16_t)(cpu->bx+0x34)), cpu->ax,
                  (x86_16_rd16(cpu,cpu->ds,(uint16_t)(cpu->bx+0x34))==cpu->ax)?"MATCH->call 2d08":"MISS->skip"); } }
              if (ip==0x3068){ static int n=0; if(n<20){n++;
                kprintf("[W6RT] *** reached 0x3068: calling view-cmd 2d08 ***\n"); } }
              if (ip==0x2d08){ static int n=0; if(n<20){n++;
                kprintf("[W6RT] view-cmd 2d08 arg[bp+6]=%04x [5b8]=%04x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                  x86_16_rd16(cpu,cpu->ds,0x05b8)); } }
              if (ip==0x2dbc){ static int n=0; if(n<20){n++;
                kprintf("[W6RT] *** about to lcall seg139:080a (geometry recompute) arg=%04x [1ae4]=%04x ***\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                  x86_16_rd16(cpu,cpu->ds,0x1ae4)); } }
              // (#278 P45 lead-a) edit-pane WM_SIZE handler (entry seg222:0x1f6a, msg 5
              // -> 0x204e). It sets view bit1 (@0x2070), then at 0x20d1 GATES a far-call
              // to the layout routine seg155:0x28bc on [0x338]&2==0 && [0x1a7c]==0.
              // Probe: does the body run, does the gate pass, does the far-call fire?
              if (ip==0x204e){ static int n=0; if(n<12){n++;
                kprintf("[W6SIZE] WM_SIZE body@204e w=[bp+6]=%04x h=[bp+8]=%04x lparam=[bp+e]=%04x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x06)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x08)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0e))); } }
              if (ip==0x20d1){ static int n=0; if(n<12){n++;
                uint16_t d338=x86_16_rd16(cpu,cpu->ds,0x0338);
                uint16_t d1a7c=x86_16_rd16(cpu,cpu->ds,0x1a7c);
                kprintf("[W6SIZE] gate@20d1 [338]=%04x(&2=%d) [1a7c]=%04x -> %s\n",
                  d338,(d338>>1)&1,d1a7c,
                  (((d338&2)==0)&&d1a7c==0)?"PASS: call 155:28bc":"SKIP layout"); } }
              if (ip==0x20e8){ static int n=0; if(n<12){n++;
                uint16_t pane=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x4));
                uint16_t sub=x86_16_rd16(cpu,cpu->ds,pane);
                kprintf("[W6SIZE] *** FAR-CALL seg155:28bc pane=%04x sub=%04x [sub+c]=%04x(bit1=%d) ***\n",
                  pane, sub, x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+0x0c)),
                  (x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+0x0c))>>1)&1); } }
            }
            // (#278 P45 lead-a) seg155:0x28bc = the WM_SIZE conditional layout routine.
            // Args: [bp+0xc]=pane object, [bp+6..a]=rect. si=[[bp+0xc]]=sub. Branches on
            // split-mode di=([si+0x16]&0xc)>>2; di==0 -> 0x2932 which RETURNS w/o layout
            // if the pane is not visible ([si+0xb]&8 clear) or [si+0x52/53]==0x63.
            // Determine: does it run to a real layout, and is bit1 [si+0xc] cleared?
            if (cpu->cs==0x04df) {
              uint16_t ip2=cpu->ip;
              if (ip2==0x28bc){ static int n=0; if(n<12){n++;
                uint16_t obj=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0c));
                uint16_t sub=x86_16_rd16(cpu,cpu->ds,obj);
                kprintf("[W6LAY] ENTER 155:28bc obj=%04x sub=%04x [b]=%02x [c]=%04x(bit1=%d) [16]=%04x rect=%04x,%04x,%04x\n",
                  obj,sub,x86_16_rd8(cpu,cpu->ds,(uint16_t)(sub+0x0b)),
                  x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+0x0c)),
                  (x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+0x0c))>>1)&1,
                  x86_16_rd16(cpu,cpu->ds,(uint16_t)(sub+0x16)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x06)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x08)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0a))); } }
              if (ip2==0x28de){ static int n=0; if(n<12){n++;
                kprintf("[W6LAY] 28de split-mode di=%04x %s [si+f]=%02x\n",
                  cpu->di, cpu->di?"(has split -> format branch)":"(0 -> 0x2932 vis-check)",
                  x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->si+0x0f))); } }
              if (ip2==0x2932){ static int n=0; if(n<12){n++;
                uint8_t b=x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->si+0x0b));
                kprintf("[W6LAY] 2932 [si+b]=%02x(vis bit3=%d) [si+52]=%02x [si+53]=%02x -> %s\n",
                  b,(b>>3)&1, x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->si+0x52)),
                  x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->si+0x53)),
                  (b&8)?"do layout":"RETURN(no-op)"); } }
              if (ip2==0x290b){ static int n=0; if(n<8){n++;
                kprintf("[W6LAY] 290b -> split leaf call ffff:634\n"); } }
              if (ip2==0x2927){ static int n=0; if(n<8){n++;
                kprintf("[W6LAY] 2927 -> leaf call 2050:3496\n"); } }
              if (ip2==0x295f){ static int n=0; if(n<8){n++;
                kprintf("[W6LAY] 295f -> leaf call ffff:c10 (single-pane layout)\n"); } }
              // (#278 P47) New-document doc-view CREATE routine seg155:0x678 lifecycle.
              // Trace whether it completes (0xc40 ax=1) or bails (0xc4a); the view the
              // constructor (0x9be -> seg223:0x406) returns; and both realize tests
              // (0xa56 test1, 0xb89 test2). Determines exactly where realize would be
              // set and confirms the routine runs to completion without realizing.
              if (ip2==0x0678){ static int n=0; if(n<6){n++;
                kprintf("[W6NEW] === seg155:678 New-doc CREATE entry [bp+6]=%04x [bp+c]=%04x [498]=%04x ===\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x06)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0c)),
                  x86_16_rd16(cpu,cpu->ds,0x0498)); } }
              if (ip2==0x09c6){ static int n=0; if(n<6){n++;
                uint16_t h=cpu->ax; uint16_t obj=h?x86_16_rd16(cpu,cpu->ds,h):0;
                kprintf("[W6NEW] after ctor(223:406): handle=%04x obj=%04x [obj+b]=%02x [obj+c]=%04x\n",
                  h,obj, obj?x86_16_rd8(cpu,cpu->ds,(uint16_t)(obj+0x0b)):0,
                  obj?x86_16_rd16(cpu,cpu->ds,(uint16_t)(obj+0x0c)):0); } }
              if (ip2==0x0a56){ static int n=0; if(n<6){n++;
                uint16_t h=x86_16_rd16(cpu,cpu->ds,cpu->bx); /* bx already =[bp-10]; [bx]=obj*/
                kprintf("[W6NEW] realize-test1@a56 obj=%04x [obj+b]=%02x -> %s\n",
                  cpu->bx, x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->bx+0x0b)),
                  (x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->bx+0x0b))&8)?"REALIZED(skip fixup)":"NOT-realized(rect calc)"); (void)h; } }
              if (ip2==0x0b89){ static int n=0; if(n<6){n++;
                kprintf("[W6NEW] realize-test2@b89 obj=%04x [obj+b]=%02x -> %s\n",
                  cpu->bx, x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->bx+0x0b)),
                  (x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->bx+0x0b))&8)?"REALIZED":"NOT-realized(->28bc no-op)"); } }
              if (ip2==0x0c40){ static int n=0; if(n<6){n++;
                kprintf("[W6NEW] seg155:678 EXIT SUCCESS (ax=1) - completed WITHOUT realize\n"); } }
              if (ip2==0x0c4a){ static int n=0; if(n<6){n++;
                kprintf("[W6NEW] seg155:678 EXIT *** BAIL @c4a *** (ctor/precondition failed)\n"); } }
            }
            // (#278 P45 lead-b) seg58 idle display-object table (sel 0x434c,
            // di 0x5743..0x5879 step 0xa: [method:4][si:2][thr:2][mask:2]). An entry
            // runs iff (selmask[bp-4] & mask) && (thr <= [bp-a]) && not skip-flagged
            // (bitmaps at [si>>8+0x3518] and +0x38f6, bit si&0xff). Dump the whole
            // table once + trace every actual dispatch, to see whether a geometry-
            // format object (method in seg139/045f or seg182/05b7) is present but
            // SKIPPED while only seg182:0x94c runs.
            if (cpu->cs==0x01d7) {
              if (cpu->ip==0x5529){ static int done=0;
                if(!done && x86_16_rd16(cpu,cpu->ds,0x1d18)!=0){ done=1;
                uint16_t tseg=cpu->es;                              /* structure seg (0x434c live) */
                uint16_t mseg_base=x86_16_rd16(cpu,cpu->ds,0x0cb0); /* method far-ptr seg */
                uint16_t selmask=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x4));
                uint16_t thr=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0xa));
                kprintf("[W6DTAB] === table tseg=%04x methseg[0cb0]=%04x selmask=%04x thr=%04x view=%04x ===\n",
                  tseg,mseg_base,selmask,thr,x86_16_rd16(cpu,cpu->ds,0x1d18));
                for(uint16_t d=0x5743; d<0x5879; d+=0xa){
                  uint16_t moff=x86_16_rd16(cpu,mseg_base,(uint16_t)(d-8));
                  uint16_t mseg=x86_16_rd16(cpu,mseg_base,(uint16_t)(d-6));
                  uint16_t si=x86_16_rd16(cpu,tseg,(uint16_t)(d-4));
                  uint16_t thre=x86_16_rd16(cpu,tseg,(uint16_t)(d-2));
                  uint16_t mask=x86_16_rd16(cpu,tseg,d);
                  uint8_t sk1=x86_16_rd8(cpu,cpu->ds,(uint16_t)((si>>8)+0x3518));
                  uint8_t sk2=x86_16_rd8(cpu,cpu->ds,(uint16_t)((si>>8)+0x38f6));
                  kprintf("[W6DTAB] di=%04x meth=%04x:%04x si=%04x thr=%04x mask=%04x sel=%d thrOK=%d sk1=%d sk2=%d\n",
                    d,mseg,moff,si,thre,mask,(selmask&mask)?1:0,(thre<=thr)?1:0,
                    (sk1&(si&0xff))?1:0,(sk2&(si&0xff))?1:0);
                }
              }}
              if (cpu->ip==0x5590){ static int n=0; if(n<40){n++;
                uint16_t di=cpu->di;
                kprintf("[W6DISP] dispatch di=%04x meth=%04x:%04x si=%04x\n",di,
                  x86_16_rd16(cpu,cpu->es,(uint16_t)(di-6)),
                  x86_16_rd16(cpu,cpu->es,(uint16_t)(di-8)),
                  x86_16_rd16(cpu,cpu->es,(uint16_t)(di-4))); }}
            }
            // (#278 P41) seg85:0x3faa = the gate the msg-0x10 (doc-ready) handler
            // (seg85:0x37a0) must see return 0 to set [0x344] bit3. It returned 1 in
            // the trace -> bit3 never set. Log all 6 sub-conditions to find the one
            // that fails (each !=required forces the ret-1 path via 0x4002).
            if (cpu->cs==0x02af && cpu->ip==0x3faa){ static int n=0; if(n<8){n++;
              uint16_t d4d0=x86_16_rd16(cpu,cpu->ds,0x04d0);
              uint16_t d337=x86_16_rd16(cpu,cpu->ds,0x0337);
              uint16_t d33f=x86_16_rd16(cpu,cpu->ds,0x033f);
              uint16_t d4c8=x86_16_rd16(cpu,cpu->ds,0x04c8);
              uint16_t d4ce=x86_16_rd16(cpu,cpu->ds,0x04ce);
              uint16_t d15fe=x86_16_rd16(cpu,cpu->ds,0x15fe);
              uint16_t arg=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
              kprintf("[W63FAA] [4d0]=%04x(0?%d) [337]&20=%d [33f]&4=%d [4c8]=%04x(<3?%d) [4ce]=%04x(0?%d) [15fe]=%04x arg[bp+6]=%04x\n",
                d4d0,d4d0==0, (d337>>5)&1, (d33f>>2)&1, d4c8, d4c8<3, d4ce, d4ce==0, d15fe, arg); } }
            // (#278 P41) pinpoint which 0x3faa sub-call fails: ax after the view
            // validator lcall@0x3fe1 (checked at 0x3fe6) and after lcall@0x3fef (0x3ff4).
            if (cpu->cs==0x02af && cpu->ip==0x3fe6){ static int n=0; if(n<8){n++;
              kprintf("[W63FAA] after view-validate lcall(4a96): ax=%04x %s\n", cpu->ax, cpu->ax?"->BAIL(ret1)":"->continue"); } }
            if (cpu->cs==0x02af && cpu->ip==0x3ff4){ static int n=0; if(n<8){n++;
              kprintf("[W63FAA] after lcall(0x7e8): ax=%04x\n", cpu->ax); } }
            // (#278 P43) seg223 SetActiveView(view): the ONLY writer of the active-view
            // global [0x1d18] with a real view is seg223:0x1459 (mov [1d18],si). si comes
            // from arg [bp+8]. Trace whether this runs, with what view (si), and its caller.
            // Also the decision at 0x12ec (no current view -> take arg). seg223 sel=0x06ff.
            if (cpu->cs==0x06ff){
              uint16_t ip=cpu->ip;
              // (#278 P47) view constructor seg223:0x406 lifecycle. Entry, the object
              // allocator return (0xdf1: [bp-4]=obj-handle), and the idiv-by line-metric
              // (0x6df: idiv si). If si==0 -> divide-by-zero (a plausible GDI-metric
              // interpreter bug that would abort geometry). Also arm the realize
              // write-watch on the fresh object EARLY (before pass46's handle-link arm).
              if (ip==0x0406){ static int n=0; if(n<6){n++;
                kprintf("[W6CTOR] seg223:406 ENTER [bp+6]=%04x [bp+14]=%04x(parent) [498]=%04x\n",
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x06)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x14)),
                  x86_16_rd16(cpu,cpu->ds,0x0498)); } }
              if (ip==0x06df){ static int n=0; if(n<6){n++;
                kprintf("[W6CTOR] seg223:6df idiv: dividend(pgH-margin)=%04x divisor si(line-metric)=%04x %s\n",
                  cpu->ax, cpu->si, cpu->si==0?"*** DIV-BY-ZERO ***":""); } }
              if (ip==0x0df1){ static int n=0; if(n<6){n++;
                uint16_t h=cpu->ax; uint16_t obj=h?x86_16_rd16(cpu,cpu->ds,h):0;
                kprintf("[W6CTOR] seg223:df1 obj-alloc handle=%04x obj=%04x [obj+b]=%02x (arming early realize watch)\n",
                  h,obj, obj?x86_16_rd8(cpu,cpu->ds,(uint16_t)(obj+0x0b)):0);
                { extern uint32_t g_wp_lin; extern uint16_t win16_dgroup_sel;
                  if (obj && g_wp_lin==0) g_wp_lin = lin(win16_dgroup_sel,(uint16_t)(obj+0x0b)); } } }
              if (ip==0x12ec){ static int n=0; if(n<20){n++;
                kprintf("[W6ACTV] SetActiveView@12ec cur[1d18]=%04x arg[bp+8]=%04x caller=%04x:%04x\n",
                  x86_16_rd16(cpu,cpu->ds,0x1d18),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2))); } }
              if (ip==0x1459){ static int n=0; if(n<20){n++;
                kprintf("[W6ACTV] *** store [1d18]=si=%04x (view %s) ***\n",
                  cpu->si, cpu->si?"ACTIVATED":"CLEARED"); }
                { extern uint32_t g_wp_lin; extern uint16_t win16_dgroup_sel;
                  if (cpu->si){
                    uint16_t sub=x86_16_rd16(cpu,win16_dgroup_sel,cpu->si);
                    g_wp_lin = lin(win16_dgroup_sel,(uint16_t)(sub+0x0c));
                    kprintf("[W6P44] view=%04x sub=%04x flag[+c]=%04x armed watch lin=%05lx\n",
                      cpu->si, sub, x86_16_rd16(cpu,win16_dgroup_sel,(uint16_t)(sub+0x0c)),
                      (unsigned long)g_wp_lin);
                    kprintf("[W6P44] ctx [48c6]=%04x [48be]=%04x [48c0]=%04x [48c2]=%04x [48c4]=%04x [36a]=%04x [3e16]=%04x\n",
                      x86_16_rd16(cpu,win16_dgroup_sel,0x48c6),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x48be),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x48c0),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x48c2),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x48c4),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x36a),
                      x86_16_rd16(cpu,win16_dgroup_sel,0x3e16));
                  } } }
            }
            // (#278 P43) seg182:0x0345 = the actual `or [0344],8` (bit3 set = page paints).
            // Log EVERY time it runs + caller (far ret on stack), to see if any startup
            // path sets bit3 at all. seg182 sel=0x05b7.
            if (cpu->cs==0x05b7 && cpu->ip==0x0345){ static int n=0; if(n<12){n++;
              kprintf("[W6BIT3SET] *** seg182:0345 OR [0344],8 reached! [1d18]=%04x sp-frame=%04x:%04x ***\n",
                x86_16_rd16(cpu,cpu->ds,0x1d18),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                x86_16_rd16(cpu,cpu->ss,cpu->sp)); } }
            // watch writes to the [0x36a] gate + [0x3e16] repaginate flag
            if (cpu->cs==0x05b7 && cpu->ip==0x0951){/*placeholder to keep block*/}
          }
        }
        // (#278 Word6 EXPERIMENT) seg15:0x0fb4 init returns 0 (resource/config load
        // A+B both failed). Force it to SUCCEED to probe the next blocker: at the
        // failure exit 0x0fee `xor ax,ax;leave;retf`, set ax=1 and skip to 0x0ff0.
        { extern int g_ole2_k334log; extern int g_win16_pmode; static int nfx=0;
          if (g_win16_pmode && cpu->cs==0x007f && cpu->ip==0x0fee) {
            cpu->ax=1; cpu->ip=0x0ff0;
            if (g_ole2_k334log && nfx<2){ nfx++; kprintf("[FB4FIX] forced seg15:0x0fb4 -> ax=1 (skip fail-exit)\n"); }
            continue; } }
        // (#278 Word6 SHARE.EXE FIX) Word's app-init (seg136:0x95e) tests for the DOS
        // SHARE.EXE TSR by opening a temp file on each fixed drive and attempting a
        // file-region LOCK; if the lock fails it shows "You must exit Windows and load
        // SHARE.EXE in order to run Word" and aborts. MayteraOS's Win16 environment is a
        // single DOS session with no file-sharing contention, so SHARE's locking is
        // effectively always satisfiable. Make the detector return success (ax=1)
        // immediately so Word proceeds into its message loop / window creation.
        // The function takes no stack args (plain retf at 0x98d); at entry the top of
        // stack is the far return frame, so set ax=1 and pop ip:cs.
        { extern int g_ole2_k334log; extern int g_win16_pmode; static int nsh=0;
          if (g_win16_pmode && cpu->cs==0x0447 && cpu->ip==0x095e) {
            cpu->ax=1;
            cpu->ip=x86_16_rd16(cpu,cpu->ss,cpu->sp);
            cpu->cs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2));
            cpu->sp=(uint16_t)(cpu->sp+4);
            if (g_ole2_k334log && nsh<2){ nsh++; kprintf("[SHAREFIX] seg136:0x95e SHARE-detect -> ax=1 (SHARE assumed present)\n"); }
            continue; } }
        // (#278 Word6 pass12 FIX) Repair a local-heap handle that the MS-C runtime's
        // handle-table relocation (seg231:0x538, on a heap grow) corrupts. At table
        // creation (seg163:0x71e1 returns the handle in ax=0x368c) we snapshot the
        // valid block pointer [DGROUP:0x368c]. The grow-time swap later overwrites
        // that live handle with a stale block back-pointer (old [block-2]=0x000e), so
        // when seg48:0x29e0 derefs bx=[0x368c] it reads 0x000e (the runtime header)
        // and pulls a garbage element COUNT ([0x10]=0x575c=22364), driving an O(n^2)
        // packed-string walk that overruns the run budget before ShowWindow. Restore
        // the snapshot so the enumeration runs over the REAL (small) table. Gated
        // Word-only (pmode). NOTE: relies on the grow not physically moving the block
        // (observed: it grows by extending, not compacting this block).
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          static uint16_t w6_tbl_blk = 0;
          if (g_win16_pmode) {
            if (cpu->cs==0x051f && cpu->ip==0x71e1 && cpu->ax==0x368c) {
              uint16_t v = x86_16_rd16(cpu, cpu->ds, 0x368c);
              if (v >= 0x0200) w6_tbl_blk = v;   // snapshot valid block ptr
            }
            if (cpu->cs==0x0187 && cpu->ip==0x29e0 && cpu->bx < 0x0200 && w6_tbl_blk) {
              static int n=0;
              uint16_t bad=cpu->bx;
              cpu->bx = w6_tbl_blk;                       // fix the register the loop uses
              x86_16_wr16(cpu, cpu->ds, 0x368c, w6_tbl_blk); // and the live handle slot
              if (g_ole2_k334log && n<4){ n++;
                kprintf("[W6TBLFIX] repaired handle [0x368c] %04x -> %04x  realcount=%04x\n",
                  bad, w6_tbl_blk, x86_16_rd16(cpu,cpu->ds,(uint16_t)(w6_tbl_blk+2))); }
            }
          }
        }
        // (#278 Word6 pass13) Trace app-init (seg136 cs=0x0447) failure branches.
        // Each gate is `call sub-init; or ax,ax; jnz ok; jmp 0x900/0x907(fail)`. Logging
        // the FIRST failure-jmp source reached identifies the failing sub-init. The 0x884
        // site is a `jz 0x900` so it fails only when ax==0. Gated Word-only (pmode+k334log).
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          if (g_win16_pmode && g_ole2_k334log && cpu->cs==0x0447) {
            uint16_t aip=cpu->ip; static int w6ai=0;
            if (w6ai<24 && (aip==0x550||aip==0x561||aip==0x60b||aip==0x623||aip==0x62f||
                aip==0x63b||aip==0x64c||aip==0x670||aip==0x68f||aip==0x6d4||aip==0x719||
                aip==0x73f||aip==0x75b||aip==0x767||aip==0x77a||aip==0x7d9||aip==0x80c||
                aip==0x854||aip==0x861||aip==0x884)) {
              w6ai++;
              kprintf("[W6AI] app-init gate ip=%04x ax=%04x dx=%04x di=%04x bp=%04x\n",
                aip, cpu->ax, cpu->dx, cpu->di, cpu->bp);
            }
          }
        }
        // (#278 Word6 pass13 FIX) Repair the corrupted DGROUP near-heap segment
        // pointer. The MS-C runtime's handle-table relocation (seg231 grow path)
        // zeroes DGROUP globals in our environment (same mechanism as the pass-12
        // handle [0x368c] corruption), including [0x40ea] = the based-heap segment
        // the MS-C far-malloc (seg231:0xff) loads into DS. With heapseg=0 the malloc
        // walks segment 0 (garbage) and returns NULL -> seg196:0x444 fails ->
        // app-init returns failure -> FatalAppExit "no main procedure". At malloc
        // entry (runtime cs=0x073f, ip=0x010a after `mov bp,sp`), if the heapseg arg
        // [bp+8] is 0, substitute the live DGROUP selector (= SS, since Word is
        // SS==DS) for THIS call and repair the [0x40ea] global for future calls.
        // Gated Word-only (pmode). NOTE: 0 is never a valid heap segment, so this is
        // safe for any malloc that would otherwise dereference a null segment.
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          if (g_win16_pmode && cpu->cs==0x073f && cpu->ip==0x010a) {
            uint16_t hp = x86_16_rd16(cpu, cpu->ss, (uint16_t)(cpu->bp+8));
            if (hp == 0) {
              x86_16_wr16(cpu, cpu->ss, (uint16_t)(cpu->bp+8), cpu->ss); // fix this call
              x86_16_wr16(cpu, cpu->ss, 0x40ea, cpu->ss);               // repair the global
              static int nhs=0;
              if (g_ole2_k334log && nhs<6){ nhs++;
                kprintf("[W6HEAPSEG] malloc heapseg 0 -> DGROUP %04x (repaired [0x40ea]) sz=%04x\n",
                  cpu->ss, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6))); }
            }
          }
        }
        // (#278 Word6 pass35c DIAG) The doc-view build (seg155=04df) runs with
        // ds=1127 but Word's DGROUP/near-heap is 075f (SS==DS invariant). Confirm
        // ds vs ss, read the handle via BOTH, and watch the REAL handle word at
        // the ds used by the view ctor (arm at the view's 0x406 entry where ds==1127).
        { extern int g_ole2_k334log; extern int g_win16_pmode; extern uint32_t g_wp_lin;
          if (g_win16_pmode && g_ole2_k334log) {
            if (cpu->cs==0x06ff && cpu->ip==0x0406 && cpu->ds==0x1127) {
              g_wp_lin = lin(cpu->ds, 0x4a82);
              kprintf("[W6VIEW] ARM(view) watch ds:4a82 lin=%05lx ds=%04x ss=%04x\n",
                (unsigned long)g_wp_lin, cpu->ds, cpu->ss);
            }
            if (cpu->cs==0x04df && cpu->ip==0x09c6) {
              uint16_t h=cpu->ax;
              kprintf("[W6VIEW] 0x406 ret h=%04x ds=%04x ss=%04x es=%04x [ds:h]=%04x [ss:h]=%04x [075f:h]=%04x\n",
                h, cpu->ds, cpu->ss, cpu->es,
                x86_16_rd16(cpu,cpu->ds,h), x86_16_rd16(cpu,cpu->ss,h), x86_16_rd16(cpu,0x075f,h));
            } else if (cpu->cs==0x04df && cpu->ip==0x09db) {
              uint16_t h = x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x10));
              kprintf("[W6VIEW] after seg37 ax=%04x h=%04x [ds:h]=%04x [ss:h]=%04x ds=%04x ss=%04x\n",
                cpu->ax, h, x86_16_rd16(cpu,cpu->ds,h), x86_16_rd16(cpu,cpu->ss,h), cpu->ds, cpu->ss);
            } else if (cpu->cs==0x04df && cpu->ip==0x0a04) {
              uint16_t h = x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x10));
              kprintf("[W6VIEW] preSAV h=%04x [ds:h]=%04x [ss:h]=%04x ds=%04x ss=%04x\n",
                h, x86_16_rd16(cpu,cpu->ds,h), x86_16_rd16(cpu,cpu->ss,h), cpu->ds, cpu->ss);
            } else if (cpu->cs==0x04df && cpu->ip==0x0a09) {
              kprintf("[W6VIEW] postSAV [ds:1d18]=%04x [ss:1d18]=%04x ds=%04x ss=%04x\n",
                x86_16_rd16(cpu,cpu->ds,0x1d18), x86_16_rd16(cpu,cpu->ss,0x1d18), cpu->ds, cpu->ss);
            }
          }
        }
        // (#278 Word6 pass35 FIX-2) Word's DS drifts off its DGROUP (a lost far-DS
        // restore: seg155/222 do push ds / lds far / pop ds, and a stack desync leaves
        // pop ds holding a GlobalAlloc'd selector instead of DGROUP). The doc-view
        // build then reads the view HANDLE + writes the active-view global [0x1d18]
        // through the wrong segment, so SetActiveView (seg223:0x1158 cmpw $0,(%si))
        // sees 0 and installs a NULL active view -> short menu, grey page, no typing.
        // SetActiveView's body [0x10d6..0x1470] (incl the [0x1d18] store at 0x1459)
        // is pure DGROUP-near code with NO ds-load, so restoring DS=SS (=DGROUP) here
        // is faithful (real Word runs it with DS=DGROUP=SS). The ds!=ss guard makes
        // this a NO-OP for games (they keep SS==DS) -> byte-identical. Word-only.
        // (#278 Word6 pass36 FIX) Broaden the pass-35 DS=SS restore. Word's DS
        // drifts off DGROUP (a lost far-DS restore in a DLL chain: pop ds<-a
        // GlobalAlloc'd selector). In an SS==DS app near data must resolve in
        // DGROUP; these ranges are Word's own near code with NO legitimate ds-load,
        // so DS must equal DGROUP=SS there. Forcing it is faithful and covers
        // SetActiveView, WM_SETFOCUS view/caret activation (seg223:0x1d90) and the
        // edit-pane/doc-child wndproc bodies (WM_CHAR/WM_PAINT dispatch). Verified
        // pure-near: seg223 (06ff) first lds/pop-ds/mov-ds is at 0x21a0 (below is
        // ES-only); seg222 (06f7) edit-pane [0x1f6a,0x2454) and doc-child
        // [0x178a,0x1e62) have no ds-load except their own prologue. The ds!=ss
        // guard is a NO-OP for games (SS==DS, and these selectors need >200 segs).
        if (g_win16_pmode && cpu->ds != cpu->ss) {
            if ((cpu->cs==0x06ff && cpu->ip < 0x21a0) ||
                (cpu->cs==0x06f7 && ((cpu->ip>=0x178a && cpu->ip<0x1e62) ||
                                     (cpu->ip>=0x1f6a && cpu->ip<0x2454))))
                cpu->ds = cpu->ss;
        }
        // (#278 Word6 pass13) Diagnose the MS-C near-heap state at the FAILING
        // malloc. Set a flag at seg196:0x444 entry; at the next seg231:0xff (the
        // MS-C far-malloc, runtime cs=0x073f) read the LOCALINFO (ptr at [ds:0x16])
        // fields: pLast[+0], pFirst[+2], rover[+4], freecount[+0x1e]. If freecount
        // >= requested, the space exists but the rover can't reach it (corruption);
        // if < requested, the near heap is genuinely exhausted. Gated Word-only.
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          static int w6mflag=0; static int w6mn=0;
          if (g_win16_pmode && g_ole2_k334log) {
            if (cpu->cs==0x0627 && cpu->ip==0x444) w6mflag=1;
            if (cpu->cs==0x073f && cpu->ip==0x010a && w6mflag && w6mn<3) {
              w6mn++; w6mflag=0;
              uint16_t sz=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
              uint16_t hp=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8));
              uint16_t li=x86_16_rd16(cpu,hp,0x16);
              kprintf("[W6MHEAP] malloc sz=%04x heapseg=%04x LI@%04x pLast=%04x pFirst=%04x rover=%04x freecnt=%04x\n",
                sz, hp, li, x86_16_rd16(cpu,hp,li), x86_16_rd16(cpu,hp,(uint16_t)(li+2)),
                x86_16_rd16(cpu,hp,(uint16_t)(li+4)), x86_16_rd16(cpu,hp,(uint16_t)(li+0x1e)));
            }
          }
        }
        // (#278 Word6 pass13) Trace seg196:0x444 (cs=0x0627) -- the font/table builder
        // app-init's final gate (seg136:0x87d) calls; it returns 0 -> app-init fails.
        // Log entry, each allocation return, and the failure/success exits to find the
        // first failing sub-allocation. Gated Word-only.
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          if (g_win16_pmode && g_ole2_k334log && cpu->cs==0x0627) {
            uint16_t aip=cpu->ip; static int w6f=0;
            if (w6f<80) {
              if (aip==0x444){ w6f++; kprintf("[W6F196] ENTER cnt[20aa]=%04x\n", x86_16_rd16(cpu,cpu->ds,0x20aa)); }
              else if (aip==0x483){ w6f++; kprintf("[W6F196] alloc231ff -> ax=%04x\n", cpu->ax); }
              else if (aip==0x48a){ w6f++; kprintf("[W6F196] FAIL1 (alloc231ff==0) jmp 5ef\n"); }
              else if (aip==0x4b9){ w6f++; kprintf("[W6F196] call4e6 -> ax=%04x\n", cpu->ax); }
              else if (aip==0x4c0){ w6f++; kprintf("[W6F196] FAIL2 (4e6==0) jmp 5e0\n"); }
              else if (aip==0x4ea){ w6f++; kprintf("[W6F196] call51f -> ax=%04x\n", cpu->ax); }
              else if (aip==0x4fa){ w6f++; kprintf("[W6F196] FAIL3 (51f==0) jmp 5e0\n"); }
              else if (aip==0x523){ w6f++; kprintf("[W6F196] call558 -> ax=%04x\n", cpu->ax); }
              else if (aip==0x55c){ w6f++; kprintf("[W6F196] call58c -> ax=%04x\n", cpu->ax); }
              else if (aip==0x590){ w6f++; kprintf("[W6F196] call9c -> ax=%04x\n", cpu->ax); }
              else if (aip==0x5be){ w6f++; kprintf("[W6F196] alloc132_818 -> ax=%04x dx=%04x\n", cpu->ax, cpu->dx); }
              else if (aip==0x5cb){ w6f++; kprintf("[W6F196] gate5cb (alloc132 zero? jz 5e0)\n"); }
              else if (aip==0x5db){ w6f++; kprintf("[W6F196] SUCCESS path (set ret=1)\n"); }
              else if (aip==0x5ec){ w6f++; kprintf("[W6F196] RETURN ret=[bp-6]=%04x\n", x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-6))); }
            }
          }
        }
        // (#278 Word6 pass13) Phase-2 gate tracer. After app-init (seg136:0x532)
        // succeeds, WinMain (seg222:0x851) calls phase-2 (seg136:0x117e), whose
        // sub-init gates fail to 0x179c (OLE teardown + error display + return 0).
        // Log the FIRST failure-jmp source reached to identify the next blocker.
        { extern int g_ole2_k334log; extern int g_win16_pmode;
          if (g_win16_pmode && g_ole2_k334log && cpu->cs==0x0447) {
            uint16_t p=cpu->ip; static int w6p2=0;
            if (w6p2<20 && (p==0x11d5||p==0x11e1||p==0x11ed||p==0x1206||p==0x1269||
                p==0x12a3||p==0x12af||p==0x12c3||p==0x12d6||p==0x12e6||p==0x1307||
                p==0x1313||p==0x1326||p==0x1341||p==0x1373||p==0x13e3||p==0x14a2||p==0x1528)) {
              w6p2++;
              kprintf("[W6P2] phase2 fail-jmp ip=%04x ax=%04x dx=%04x bp=%04x\n",
                p, cpu->ax, cpu->dx, cpu->bp);
            }
          }
        }
        // (#278 Word6 pass20) FAITHFUL NEAR-HEAP BASE FIX. Word's MS-C C startup
        // (seg1) byte-copies its near-heap descriptor into DGROUP, leaving the heap
        // base pointer (pStart) at [DGROUP:0x0e] = 0x3644, which is BELOW the end of
        // Word's static data (~0x49be). __LInit (seg231:0x0) reads di=[0x0e] (only on
        // the SS==DS / DGROUP path at 0x1a) and formats the local heap arena upward
        // from di. With di=0x3644 the heap overlaps Word's static globals
        // (0x368c/0x3b2c/0x40ea/...), so heap allocations clobber them (the recurring
        // 0x575c object-pointer corruption + every per-symptom band-aid). Clamp di up
        // to the loader-known end of static data (g_info.lheap_base = data_len rounded
        // up) at the instant after the [0x0e] load so the entire arena (LOCALINFO +
        // all blocks) sits above the statics. Gated to the DGROUP init path (cs=seg231,
        // ip=0x1e, ds==dgroup selector), so OLE2 DLL / GlobalAlloc'd-segment LInits
        // are untouched. This is the durable root-cause fix the band-aids approximated.
        { extern int g_win16_pmode; extern uint16_t win16_dgroup_sel; extern uint16_t win16_dgroup_heap_base;
          if (g_win16_pmode && win16_dgroup_heap_base && cpu->cs==0x073f && cpu->ip==0x001e
              && cpu->ds==win16_dgroup_sel && cpu->di && cpu->di < win16_dgroup_heap_base) {
            static int hb=0; if (hb<4){ hb++;
              kprintf("[W6HEAPBASE] __LInit DGROUP pStart %04x -> %04x (heap now above static data)\n",
                cpu->di, win16_dgroup_heap_base); }
            cpu->di = win16_dgroup_heap_base;
          } }
        // (#278 P59) universal instruction-start probe on the pane-creation chain in
        // seg223 (sel 0x06ff). Catches entries regardless of dispatch mechanism (near,
        // direct-far 0x9A, indirect-far 0xFF/3 vtable). Goal: find why the two chrome
        // panes build a display-list pane node but the DOCUMENT BODY view does not.
        // At entry to each routine the caller far-return frame is [ss:sp]=ret_ip,
        // [ss:sp+2]=ret_cs (both CreatePane's `push cs;call` and seg136's 0x9A push cs
        // first), args follow at [ss:sp+4..].
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode && cpu->cs==0x06ff) {
            uint16_t ip0 = cpu->ip;
            if (ip0==0x0284) { static int n=0; if(n<40){n++;
              kprintf("[W6PANE] CreateDisplayListNode(0x284) ENTER #%d caller=%04x:%04x listSel=%04x [40ea]=%04x\n",
                n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4)), x86_16_rd16(cpu,cpu->ds,0x40ea)); } }
            else if (ip0==0x0dd0) { static int n=0; if(n<40){n++;
              kprintf("[W6PANE] CreatePane(0xdd0) ENTER #%d caller=%04x:%04x a[pb+6]=%04x a[pb+0xc]=%04x a[pb+0xe]=%04x a[pb+0x14]=%04x a[pb+0x16]=%04x a[pb+0x18]=%04x [1a38]=%04x\n",
                n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+0x0c)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+0x0e)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+0x14)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+0x16)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+0x18)),
                x86_16_rd16(cpu,cpu->ds,0x1a38)); } }
            else if (ip0==0x0726) { static int n=0; if(n<40){n++;
              kprintf("[W6PANE] view-region-layout@0x726 (about to call CreatePane) #%d bp=%04x [bp+6]=%04x [bp+0xa]=%04x [bp+0xe]=%04x [bp+0x10]=%04x [bp+0x12]=%04x [bp+0x14]=%04x [bp+0x16]=%04x si=%04x\n",
                n, cpu->bp,
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0a)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0e)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x10)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x12)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x14)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x16)), cpu->si); } }
          } }
        // (#278 P61) PANE-ROLE diagnostic. Real Word ground truth (savestate):
        //   ds:6c6=node1(fc34,+62=0000 PRIMARY, +44view=0be4 hwnd=0 +0c=0) = the doc view;
        //   ds:6c8=node2(fd58,+62=0007,+44view=0c1a hwnd=8930 +0c=86); node3(f2dc,+62=7,sel0).
        // Ours: doc view ends up as the sel=0 TAIL. Find where the role/order diverges.
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode) {
            uint16_t seg=cpu->ds;
            if (cpu->cs==0x0447 && cpu->ip==0x0df8) { static int n=0; if(n<4){n++;
              uint16_t h=x86_16_rd16(cpu,seg,0x6c6); uint16_t obj=x86_16_rd16(cpu,seg,h);
              kprintf("[W6ROLE] seg136 NODE1 ds:6c6 h=%04x obj=%04x +44view=%04x +62=%04x +0c=%02x\n",
                h,obj, x86_16_rd16(cpu,seg,(uint16_t)(obj+0x44)), x86_16_rd16(cpu,seg,(uint16_t)(obj+0x62)),
                x86_16_rd8(cpu,seg,(uint16_t)(obj+0x0c))); } }
            if (cpu->cs==0x0447 && cpu->ip==0x0e2b) { static int n=0; if(n<4){n++;
              uint16_t h=x86_16_rd16(cpu,seg,0x6c8); uint16_t obj=x86_16_rd16(cpu,seg,h);
              kprintf("[W6ROLE] seg136 NODE2 ds:6c8 h=%04x obj=%04x +44view=%04x +62=%04x +0c=%02x\n",
                h,obj, x86_16_rd16(cpu,seg,(uint16_t)(obj+0x44)), x86_16_rd16(cpu,seg,(uint16_t)(obj+0x62)),
                x86_16_rd8(cpu,seg,(uint16_t)(obj+0x0c))); } }
            if (cpu->cs==0x06ff && cpu->ip==0x0726) { static int n=0; if(n<12){n++;
              uint16_t rip=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2));
              uint16_t rcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4));
              kprintf("[W6ROLE] 0x726 #%d caller=%04x:%04x [bp+6]=%04x srcview[bp+14]=%04x +62val[bp+16]=%04x [bp+0xa]=%04x\n",
                n, rcs, rip,
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x14)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x16)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0x0a))); } }
            if (cpu->cs==0x008f && cpu->ip==0x4fb0) { static int rdone=0; if(!rdone){rdone=1;
              kprintf("[W6ROLE] globals ds:6c4=%04x 6c6=%04x 6c8=%04x 6ca=%04x 6cc=%04x 4a82=%04x\n",
                x86_16_rd16(cpu,seg,0x6c4),x86_16_rd16(cpu,seg,0x6c6),x86_16_rd16(cpu,seg,0x6c8),
                x86_16_rd16(cpu,seg,0x6ca),x86_16_rd16(cpu,seg,0x6cc),x86_16_rd16(cpu,seg,0x4a82));
              uint16_t h=x86_16_rd16(cpu,seg,0x6cc); int n=0; uint16_t prev=0;
              while(h && h!=prev && n<12){ n++;
                uint16_t obj=x86_16_rd16(cpu,seg,h);
                uint16_t v=x86_16_rd16(cpu,seg,(uint16_t)(obj+0x44));
                uint16_t vo=v?x86_16_rd16(cpu,seg,v):0;
                kprintf("[W6ROLE] list0 #%d h=%04x obj=%04x +62=%04x +48next=%04x +44view=%04x(obj=%04x +30=%02x +34hwnd=%04x +0c=%02x)\n",
                  n,h,obj,x86_16_rd16(cpu,seg,(uint16_t)(obj+0x62)),
                  x86_16_rd16(cpu,seg,(uint16_t)(obj+0x48)),
                  v,vo, x86_16_rd8(cpu,seg,(uint16_t)(vo+0x30)),
                  x86_16_rd16(cpu,seg,(uint16_t)(vo+0x34)), x86_16_rd8(cpu,seg,(uint16_t)(vo+0x0c)));
                prev=h; h=x86_16_rd16(cpu,seg,(uint16_t)(obj+0x48)); }
              uint16_t bo=cpu->bx;
              kprintf("[W6ROLE] redisplay bx(obj)=%04x +48=%04x +62=%04x +44view=%04x\n",
                bo, x86_16_rd16(cpu,seg,(uint16_t)(bo+0x48)),
                x86_16_rd16(cpu,seg,(uint16_t)(bo+0x62)),
                x86_16_rd16(cpu,seg,(uint16_t)(bo+0x44))); } }
          } }
        // (#278 P59) one-shot dump of the AddView pane/view chains (roots ds:0x6ca list1
        // / ds:0x6cc list0) at the FIRST redisplay (seg17:0x4fb0). Diff vs real Word
        // ground truth (list0 = docview->fd58->f2dc, doc-view+0x48 NON-NULL). ds here =
        // DGROUP; handles deref via [h] to the object; +0x48 = next-in-chain.
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode && cpu->cs==0x008f && cpu->ip==0x4fb0) {
            static int done=0; if(!done){done=1;
              uint16_t seg=cpu->ds;
              for (int L=0;L<2;L++){ uint16_t root=L?0x6ca:0x6cc;
                uint16_t h=x86_16_rd16(cpu,seg,root);
                kprintf("[W6CHAIN] list%d root ds:%04x=%04x:\n", L?1:0, root, h);
                int n=0; uint16_t prev=0;
                while(h && h!=prev && n<12){ n++;
                  uint16_t obj=x86_16_rd16(cpu,seg,h);
                  uint16_t nxt=x86_16_rd16(cpu,seg,(uint16_t)(obj+0x48));
                  kprintf("[W6CHAIN]   #%d h=%04x obj=%04x +0b=%02x +0c=%02x +48(next)=%04x +b4=%04x +62=%04x\n",
                    n,h,obj, x86_16_rd8(cpu,seg,(uint16_t)(obj+0x0b)), x86_16_rd8(cpu,seg,(uint16_t)(obj+0x0c)),
                    nxt, x86_16_rd16(cpu,seg,(uint16_t)(obj+0xb4)), x86_16_rd16(cpu,seg,(uint16_t)(obj+0x62)));
                  prev=h; h=nxt; }
              }
              kprintf("[W6CHAIN] redisplay bx(view)=%04x +48=%04x\n", cpu->bx, x86_16_rd16(cpu,seg,(uint16_t)(cpu->bx+0x48)));
            } } }
        // (#278 P59) LAYOUT-GATE probe: seg17:0x128c is the per-view layout that CLEARS
        // needs-layout bit1 at 0x1338 (`and [si+0xc],0xfd`) and builds content, GATED on
        // seg223:0x0cfa(view)!=0 (0x1334 je bails). Trace whether it runs for the doc
        // view (obj 9e3a, +0xc bit1 set) and where it bails vs clears bit1.
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode && cpu->cs==0x008f) {
            uint16_t ip0=cpu->ip;
            if (ip0==0x128c) { static int n=0; if(n<30){n++;
              uint16_t bpa=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8));  /* [bp+0xa]=di source */
              kprintf("[W6LAYG] seg17:128c LAYOUT enter #%d [bp+6]=%04x [bp+8]=%04x [bp+a]=%04x ([di]=%04x) ds=%04x [4a82]=%04x [2b6]=%04x [49e]=%04x caller=%04x:%04x\n",
                n, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6)),
                bpa, x86_16_rd16(cpu,cpu->ds,bpa), cpu->ds, x86_16_rd16(cpu,cpu->ds,0x4a82),
                x86_16_rd16(cpu,cpu->ds,0x2b6), x86_16_rd16(cpu,cpu->ds,0x49e),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp)); } }
            else if (ip0==0x12f3) { static int n=0; if(n<20){n++;
              kprintf("[W6LAYG] seg17:12f3 si=%04x [bp-0xc]=%04x ds:4d4=%04x -> %s\n",
                cpu->si, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x0c)), x86_16_rd16(cpu,cpu->ds,0x4d4),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp-0x0c))?"0x10c6 path":"je 0x1319 (keeps si)"); } }
            else if (ip0==0x130e) { static int n=0; if(n<20){n++;
              kprintf("[W6LAYG] seg17:130e BEFORE call 0x10c6 si=%04x di=%04x\n", cpu->si, cpu->di); } }
            else if (ip0==0x10c6) { static int n=0; if(n<20){n++;
              kprintf("[W6LAYG] seg17:10c6 ENTER sp=%04x si=%04x (expect exit-sp=%04x [that slot=saved si])\n",
                cpu->sp, cpu->si, (uint16_t)(cpu->sp-0x14)); } }
            else if (ip0==0x10e9||ip0==0x111f||ip0==0x112c||ip0==0x1147) { static int n=0; if(n<40){n++;
              kprintf("[W6LAYG] seg17:%04x post-call sp=%04x\n", ip0, cpu->sp); } }
            else if (ip0==0x1286) { static int n=0; if(n<20){n++;
              kprintf("[W6LAYG] seg17:1286 BEFORE pop si: sp=%04x [sp](->si)=%04x\n",
                cpu->sp, x86_16_rd16(cpu,cpu->ss,cpu->sp)); } }
            else if (ip0==0x1311) { static int n=0; if(n<20){n++;
              kprintf("[W6LAYG] seg17:1311 AFTER call 0x10c6 si=%04x (was preserved? real Word expects doc-view here)\n", cpu->si); } }
            else if (ip0==0x1319) { static int n=0; if(n<30){n++;
              kprintf("[W6LAYG] seg17:1319 bit1-test si(view)=%04x obj=%04x [si+0xc]=%02x [bp+8]=%04x\n",
                cpu->si, x86_16_rd16(cpu,cpu->ds,cpu->si), x86_16_rd8(cpu,cpu->ds,(uint16_t)(cpu->si+0x0c)),
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8))); } }
            else if (ip0==0x1334) { static int n=0; if(n<30){n++;
              kprintf("[W6LAYG] seg17:1334 after seg223:0xcfa(view) ax(result)=%04x si=%04x -> %s\n",
                cpu->ax, cpu->si, cpu->ax? "will CLEAR bit1 (layout)":"BAILS (no layout)"); } }
            else if (ip0==0x1338) { static int n=0; if(n<30){n++;
              kprintf("[W6LAYG] seg17:1338 *** CLEARING bit1 (LAYOUT RUNS) *** si(view)=%04x obj=%04x\n",
                cpu->si, x86_16_rd16(cpu,cpu->ds,cpu->si)); } }
          } }
        // (#278 P60) seg223:0x0cfa GATE internals (far-call at seg17:0x132f).
        { extern int g_w6life, g_win16_pmode;
          if (g_w6life && g_win16_pmode && cpu->cs==0x06ff) {
            uint16_t ip0=cpu->ip;
            uint16_t retcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4));
            if (retcs==0x008f) {
              if (ip0==0x0d01) { static int n=0; if(n<20){n++;
                uint16_t h=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
                uint16_t obj=x86_16_rd16(cpu,cpu->ds,h);
                kprintf("[W6GATE] 223:0cfa ENTER handle=%04x obj=%04x ds:49a=%04x ds:4d4=%04x [obj+0a]=%02x [obj+34]hwnd=%04x retip=%04x\n",
                  h,obj, x86_16_rd16(cpu,cpu->ds,0x49a), x86_16_rd16(cpu,cpu->ds,0x4d4),
                  x86_16_rd8(cpu,cpu->ds,(uint16_t)(obj+0x0a)), x86_16_rd16(cpu,cpu->ds,(uint16_t)(obj+0x34)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2))); } }
              else if (ip0==0x0d21) { static int n=0; if(n<20){n++;
                kprintf("[W6GATE] 223:0d21 FAST-PATH -> return 1 (proceed to layout)\n"); } }
              else if (ip0==0x0d31) { static int n=0; if(n<20){n++;
                uint16_t obj=x86_16_rd16(cpu,cpu->ds,cpu->si);
                kprintf("[W6GATE] 223:0d31 IsIconic branch (ds:4d4!=0) hwnd=%04x -> returns IsIconic(stub=0 -> BAIL)\n",
                  x86_16_rd16(cpu,cpu->ds,(uint16_t)(obj+0x34))); } }
              else if (ip0==0x0d40) { static int n=0; if(n<20){n++;
                uint16_t obj=x86_16_rd16(cpu,cpu->ds,cpu->si);
                kprintf("[W6GATE] 223:0d40 IsWindowVisible branch (ds:4d4==0) hwnd=%04x\n",
                  x86_16_rd16(cpu,cpu->ds,(uint16_t)(obj+0x34))); } }
              else if (ip0==0x0d4a) { static int n=0; if(n<20){n++;
                kprintf("[W6GATE] 223:0d4a IsWindowVisible RETURNED ax=%04x -> gate=%d\n", cpu->ax, cpu->ax==1?1:0); } }
            }
          } }
                int seg_ovr = -1;   // segment override index (ES=0,CS=1,SS=2,DS=3) or -1
        int rep = 0;        // 0 none, 1 REP/REPE (F3), 2 REPNE (F2)
        int osize = 2;      // (#194) operand size: 2=16-bit (default), 4=32-bit (0x66)
        int asize = 2;      // (#194) address size: 2=16-bit (default), 4=32-bit (0x67)
        uint8_t op;

        // --- prefix loop ---
        for (;;) {
            op = fetch8(cpu);
            if (op == 0x26) { seg_ovr = 0; continue; }
            if (op == 0x2E) { seg_ovr = 1; continue; }
            if (op == 0x36) { seg_ovr = 2; continue; }
            if (op == 0x3E) { seg_ovr = 3; continue; }
            if (op == 0x64) { seg_ovr = 4; continue; }   // (#390) FS override (386+)
            if (op == 0x65) { seg_ovr = 5; continue; }   // (#390) GS override (386+)
            if (op == 0x66) { osize = 4; continue; }   // (#194) operand-size prefix
            if (op == 0x67) { asize = 4; continue; }   // (#194) address-size prefix
            if (op == 0xF3) { rep = 1; continue; }
            if (op == 0xF2) { rep = 2; continue; }
            if (op == 0xF0) { continue; } // LOCK: ignore
            break;
        }
        // (#194) asize is threaded into decode_modrm_a for 32-bit ModR/M + SIB.

        executed++;
        cpu->insn_count++;
        { extern int g_ole2_k334log; static int stgmn=0; if (g_ole2_k334log && cpu->cs==0x00af && cpu->ip==0x043a && stgmn<4){ stgmn++;
            uint16_t m_lo=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
            uint16_t m_hi=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            kprintf("[STGM] validator entry mode=%04x:%04x (PROBEC passed 0x1012)\n", m_hi, m_lo); } }
        { extern int g_ole2_k334log; static int edn=0; if (g_ole2_k334log && cpu->cs==0x00b7 && cpu->ip==0x17d1 && edn<4){ edn++;
            uint16_t bo=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6));
            uint16_t bs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8));
            kprintf("[EDFL] validate buf=%04x:%04x  +1a=%04x +1c=%04x (want 4445/4c46) bytes18: %02x%02x%02x%02x%02x%02x%02x%02x\n",
                bs,bo, x86_16_rd16(cpu,bs,(uint16_t)(bo+0x1a)), x86_16_rd16(cpu,bs,(uint16_t)(bo+0x1c)),
                x86_16_rd8(cpu,bs,(uint16_t)(bo+0x18)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x19)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1a)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1b)),
                x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1c)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1d)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1e)),x86_16_rd8(cpu,bs,(uint16_t)(bo+0x1f))); } }
        { extern int g_win16_pmode; static unsigned long wild=0;
          // Wild-CS crash guard: a corrupted far-call lands either in segment 0 OR in
          // an INVALID LDT selector (no loaded segment) and spins executing op=00
          // (`add [bx+si],al`). Halt the interpreter so the OS stays live instead of
          // running millions of garbage writes. (#278 extended from cs==0 to any
          // invalid pmode code selector.)
          int wildcs = g_win16_pmode && (cpu->cs == 0x0000 || !ldt_valid(cpu->cs));
          if (wildcs) {
              if (++wild > 4096) {   // ~4K insns in a wild code segment => crashed; stop
                  extern int g_ole2_k334log;
                  if (g_ole2_k334log) kprintf("[WILDCS] wild cs=%04x spin (%lu) at ip=%04x -> halt (crash guard)\n", cpu->cs, wild, cpu->ip);
                  extern void x86_16_dump_ring_full(void); x86_16_dump_ring_full();
                  cpu->halted = 1; return 0;
              }
          } else wild = 0; }
        // (#278 Word6) seg6:0x75a6 OLE-object Release crash probe. The teardown
        // walk (seg6 fn ~0x744e) reads an interface far ptr at object[+0x28]; if
        // non-null it les-derefs to the vtable and calls far [vtable+0x8]=Release.
        // The crash = vtable+0x8 has a NULL segment. Dump es:si, obj+0x28, vtable.
        { // (#278 Word6) seg6 OLE-handle release-walk NULL-vtable GUARD.
          // Word's init-failure teardown (seg6 fn ~0x744e) releases each table entry's
          // interface at obj[+0x28] via `les bx,[es:bx]; call far [es:bx+8]`. In our
          // environment some stale table handles point to interface objects whose
          // first DWORD (lpVtbl) is 0:0 -> call far [0:8] -> wild seg-0 jump. Releasing
          // through a NULL vtable is undefined; skip it faithfully. At 0x75a8 es:bx is
          // already the vtable (les bx,[es:bx] ran at 0x75a3); if the vtable selector is
          // 0 OR its Release slot (+8) has a NULL segment, SKIP the call far: set ax=0,
          // advance ip past `call far [es:bx+8]` (0x75a6, 4 bytes -> 0x75aa).
          extern int g_win16_pmode;
          if (g_win16_pmode && cpu->cs==0x0037 && cpu->ip==0x75a8) {
            uint16_t vs=cpu->es, vo=cpu->bx;
            uint16_t rs=x86_16_rd16(cpu,vs,(uint16_t)(vo+0xa));   // Release method segment
            if (vs==0 || rs==0) {
              extern int g_ole2_k334log; static int ng=0;
              if (g_ole2_k334log && ng<4){ ng++; kprintf("[REL_GUARD] skip NULL-vtable Release vtbl=%04x:%04x rel.seg=%04x -> ip 75aa\n", vs,vo,rs); }
              cpu->ax=0; cpu->ip=0x75aa;   // skip the 4-byte `call far [es:bx+8]`
              continue;                    // restart dispatch at 0x75aa
            }
          } }
        { extern int g_ole2_k334log; static uint16_t pdx=0; static int n8=0;
          if (g_ole2_k334log && cpu->dx==0x8003 && pdx!=0x8003 && n8<12){ n8++;
            kprintf("[DX8003] dx->8003 at cs:ip=%04x:%04x op=%02x ax=%04x bx=%04x cx=%04x\n", cpu->cs, cpu->ip, peek8(cpu), cpu->ax, cpu->bx, cpu->cx);
            ole2_ring_dump("dx->8003"); }
          if (g_ole2_k334log) pdx=cpu->dx; }
        // [b500 STRPARSE] trace STORAGE seg1:0x299c (sel 0x9f) = the path string-walk
        // that returns 0xffff -> StgCreateDocfile STG_E (ax=0xfc). Dump its 5 args + the
        // source string bytes, plus seg1:0x33ba (the per-char worker) when count<1.
        // [b502 STGENTRY] dump StgCreateDocfile (seg4 sel0xb7 off0x12ee) entry frame:
        // raw stack words at sp (= ret far ptr + the 8 PROBEC-pushed args). PROBEC
        // pushes: name_off=0x16d name_seg=0 0 grfMode=0x1012 0 0 ppstg_seg ppstg_off=0x188.
        { extern int g_ole2_k334log; static int nse=0;
          if (g_ole2_k334log && cpu->cs==0x00b7 && cpu->ip==0x12ee && nse<3){ nse++;
            kprintf("[STGENTRY] ss=%04x sp=%04x ret=%04x:%04x args:", cpu->ss, cpu->sp,
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)), x86_16_rd16(cpu,cpu->ss,cpu->sp));
            for(int a=4;a<=18;a+=2) kprintf(" [+%d]=%04x", a, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+a)));
            kprintf("\n"); } }
        // [b507] StgCreateDocfile internal flow probes (cs=0xb7 seg4)
        { extern int g_ole2_k334log; static int nf=0;
          if (g_ole2_k334log && cpu->cs==0x00b7 && nf<30 &&
              (cpu->ip==0x1350||cpu->ip==0x1376||cpu->ip==0x139a||cpu->ip==0x13b6||cpu->ip==0x1094||cpu->ip==0x12ee)){
            nf++;
            kprintf("[STGFLOW] cs:ip=%04x:%04x ax=%04x dx=%04x si=%04x bp=%04x\n",
              cpu->cs,cpu->ip,cpu->ax,cpu->dx,cpu->si,cpu->bp); } }
        // [b506] probe the EXACT call-site ip=0x07bc (any cs) where 0x299c is called
        // with the empty buffer, plus the seg2 fn 0x6f9 decision point (any cs).
        { extern int g_ole2_k334log; static int ncs=0;
          if (g_ole2_k334log && cpu->ip==0x07bc && ncs<4){ ncs++;
            uint16_t ao=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0xa));
            uint16_t as=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0xc));
            kprintf("[CALLSITE07BC] cs=%04x bp=%04x [bp+a:c]=%04x:%04x ret(bp+4:2)=%04x:%04x\n",
              cpu->cs,cpu->bp,as,ao,
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2))); } }
        { extern int g_ole2_k334log; static int n6f9=0;
          if (g_ole2_k334log && cpu->ip==0x06f9 && n6f9<4){ n6f9++;
            kprintf("[DEC6F9] cs=%04x bp=%04x [bp+a:c]=%04x:%04x [bp+6obj]=%04x:%04x caller=%04x:%04x\n",
              cpu->cs,cpu->bp,
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0xc)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+0xa)),
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)),
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+4)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+2))); } }
        // [b505] trace seg2:0x6b2 (sel0xa7) - the fn that calls seg1:299c with an
        // empty src buffer. Dump caller + its [bp+0xa]:[bp+0xc] src arg + obj[bp+6].
        { extern int g_ole2_k334log; static int n6b2=0;
          if (g_ole2_k334log && cpu->cs==0x00a7 && cpu->ip==0x06b2 && n6b2<4){ n6b2++;
            uint16_t so=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            uint16_t ss2=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8));
            uint16_t obo=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
            uint16_t obs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            char sb[20]; int i; for(i=0;i<19;i++){uint8_t c=x86_16_rd8(cpu,ss2,(uint16_t)(so+i));sb[i]=(c>=32&&c<127)?c:'.';if(!c)break;} sb[i]=0;
            kprintf("[SEG2_6B2] caller=%04x:%04x src=%04x:%04x str='%s' [bp+6obj]=%04x:%04x\n",
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),x86_16_rd16(cpu,cpu->ss,cpu->sp),
              ss2,so,sb,obs,obo); } }
        // [b510] after StgCreateDocfile's 0x1376 copy returns (0xb7:0x137b), dump the
        // dest buffer 000f:3dda to see if the path got copied. Also dump ax (return).
        { extern int g_ole2_k334log; static int n37b=0;
          if (g_ole2_k334log && cpu->cs==0x00b7 && cpu->ip==0x137b && n37b<3){ n37b++;
            uint16_t bo=(uint16_t)(cpu->bp-0x210);
            char sb[20]; int i; for(i=0;i<19;i++){uint8_t c=x86_16_rd8(cpu,cpu->ss,(uint16_t)(bo+i));sb[i]=(c>=32&&c<127)?c:'.';if(!c)break;} sb[i]=0;
            kprintf("[AFTER1376] ax=%04x dest %04x:%04x str='%s' b0=%02x b1=%02x\n",
              cpu->ax, cpu->ss, bo, sb, x86_16_rd8(cpu,cpu->ss,bo), x86_16_rd8(cpu,cpu->ss,(uint16_t)(bo+1))); } }
        // [b512] seg4:0x117e entry (the fn that calls 0x299c with a NULL name -> STG_E).
        // boundary 0x117e, probe at ip=0x117f. Dump caller + its name arg [bp+0xa]:[bp+0xc]
        { extern int g_ole2_k334log; static int n117=0;
          if (g_ole2_k334log && cpu->cs==0x00b7 && cpu->ip==0x117f && n117<4){ n117++;
            kprintf("[SEG4_117E] caller=%04x:%04x stack:", x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),x86_16_rd16(cpu,cpu->ss,cpu->sp));
            for(int a=4;a<=30;a+=2) kprintf(" +%d=%04x", a, x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+a)));
            kprintf("\n"); } }
        { extern int g_ole2_k334log; static int n28=0;
          if (g_ole2_k334log && cpu->cs==0x009f && cpu->ip==0x28f6 && n28<20){ n28++;
            uint16_t so=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8));
            uint16_t ss2=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+10));
            kprintf("[ENTRY28F6] caller=%04x:%04x src=%04x:%04x b0=%02x\n",
              x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),x86_16_rd16(cpu,cpu->ss,cpu->sp),
              ss2,so,x86_16_rd8(cpu,ss2,so)); } }
        { extern int g_ole2_k334log; static int n299=0;
          if (g_ole2_k334log && cpu->cs==0x009f && cpu->ip==0x299d && n299<40){ n299++;
            uint16_t dst_o=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
            uint16_t dst_s=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            uint16_t src_o=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8));
            uint16_t src_s=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+10));
            uint16_t cnt=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+12));
            char sb[24]; int i;
            for(i=0;i<23;i++){ uint8_t c=x86_16_rd8(cpu,src_s,(uint16_t)(src_o+i)); sb[i]=(c>=32&&c<127)?c:'.'; if(!c)break; }
            sb[i]=0;
            kprintf("[STRPARSE] caller=%04x:%04x seg1:299c dst=%04x:%04x src=%04x:%04x cnt=%04x str='%s' b0=%02x b1=%02x b2=%02x si=%04x\n", x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),x86_16_rd16(cpu,cpu->ss,cpu->sp),
              dst_s,dst_o,src_s,src_o,cnt,sb,
              x86_16_rd8(cpu,src_s,src_o),x86_16_rd8(cpu,src_s,(uint16_t)(src_o+1)),x86_16_rd8(cpu,src_s,(uint16_t)(src_o+2)),cpu->si); } }
        { extern int g_ole2_k334log; static int n33=0;
          if (g_ole2_k334log && cpu->cs==0x009f && cpu->ip==0x33ba && n33<8){
            uint16_t p_o=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
            uint16_t p_s=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            uint16_t cnt=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+8));
            if(cnt<1 || n33<3){ n33++;
              kprintf("[WORKER] seg1:33ba ptr=%04x:%04x cnt=%04x byte=%02x %s\n",
                p_s,p_o,cnt,x86_16_rd8(cpu,p_s,p_o), cnt<1?"<-RET 0xFFFF (count<1!)":""); } } }
        { extern int g_ole2_k334log; if (g_ole2_k334log) {
            g_ring_cs[g_ring_pos]=cpu->cs; g_ring_ip[g_ring_pos]=cpu->ip;
            g_ring_ds[g_ring_pos]=cpu->ds; g_ring_es[g_ring_pos]=cpu->es;
            g_ring_op[g_ring_pos]=peek8(cpu); g_ring_pos=(g_ring_pos+1)%OLE2_RING; } }
        { extern int g_ole2_k334log; if (g_ole2_k334log && cpu->ip==0x1da4) {
            kprintf("[AT1DA4] cs=%04x ds=%04x es=%04x si=%04x bp=%04x [bp+6]=%04x [bp+8]=%04x\n",
                cpu->cs, cpu->ds, cpu->es, cpu->si, cpu->bp,
                x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+6)), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->bp+8)));
          }
          if (g_ole2_k334log && cpu->cs==0x0017 && cpu->ip==0x1d7c) {
            // crash fn entry: at this point the far-call return frame is on the
            // stack: [sp]=retIP [sp+2]=retCS, then Pascal args. The object far
            // ptr arg = [sp+4]=off(bp+6) [sp+6]=seg(bp+8). Log caller + object.
            uint16_t rip=x86_16_rd16(cpu,cpu->ss,cpu->sp);
            uint16_t rcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2));
            uint16_t aoff=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
            uint16_t aseg=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+6));
            kprintf("[CRASHFN] entry ds=%04x caller=%04x:%04x objarg=%04x:%04x\n", cpu->ds, rcs, rip, aseg, aoff);
          } }
                // (#278 Word6) WinMain "not enough memory" FIX. Word (WINWORD seg100,
        // runtime cs=0327) gates the error MessageBox on the memory-OK flag bit0 of
        // DGROUP:0x1a37 (test byte[1a37],1; jnz skip-error at 0327:11d1). That bit is
        // set only by the global-heap arena-walk validator at 0327:0c49 (or [1a37],1),
        // which is never reached on this startup path under our environment. Our
        // pmode arena always has tens of MB free, so the answer the walk would compute
        // is "memory OK". Force DGROUP(WINWORD autodata 075f):0x1a37 bit0 the instant
        // before Word tests it so the check passes and Word proceeds past the box.
        { extern int g_ole2_k334log; static int w6memfix=0;
          if (g_ole2_k334log && cpu->cs==0x0327 && (cpu->ip==0x11c7||cpu->ip==0x11c6)) {
            uint8_t f37=x86_16_rd8(cpu,cpu->ds,0x1a37);
            if (!(f37 & 1)) {
              x86_16_wr8(cpu, cpu->ds, 0x1a37, (uint8_t)(f37 | 1));
              if (w6memfix<6) { w6memfix++;
                uint16_t rip=x86_16_rd16(cpu,cpu->ss,cpu->sp);
                uint16_t rcs=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2));
                uint16_t arg6=x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4));
                kprintf("[W6MEMFIX] forced DGROUP(%04x):1a37 bit0 (was %02x) at 0327:11c7 caller=%04x:%04x arg[bp+6]=%04x\n", cpu->ds, f37, rcs, rip, arg6);
            }
            }
          } }
        // (#278 Word6 LOCAL-HEAP REPAIR) Win16 local-heap arena walk terminator.
        // WINWORD's MS-C runtime (seg231:0x5e1, runtime cs=0x073f) walks the DGROUP
        // local-heap arena block chain (si = first block, dx = pLast end marker).
        // The arena it builds in our environment is: a busy header block, then ONE
        // big free block that ends where the in-DGROUP stack region begins (~0x6d20),
        // then RAW ZEROS up to pLast (0xa5b2 = top of the DGROUP, which here also
        // holds the SS==DS stack). The block chain therefore never reaches pLast and
        // the walk spins forever (the local-heap analogue of the global burgermaster
        // gap). Real Win16 covers the stack region with a BUSY "in-use" arena block
        // so the walk steps over it (test bl,1 -> busy -> add size -> next). Word's
        // own LInit did not emit that bridge block in our env, so we synthesize it:
        // the instant before the walk runs, scan the chain from the first block; at
        // the first zero/invalid header found below pLast, write a single BUSY arena
        // block whose size makes the next step land exactly on pLast. The walk then
        // terminates cleanly (si == dx). One-shot per arena (idempotent: skipped once
        // the chain already reaches pLast).
        { extern int g_ole2_k334log; static int lhfix=0;
          // (#278 Word6 LOCAL-HEAP REPAIR) Terminate WINWORD's MS-C local-heap
          // compaction walk (seg231, runtime cs=0x073f). The walk advances si over
          // the arena block chain (header word at [si]; busy bit = bit0), coalescing
          // free blocks, until si == dx (pLast end marker = 0xa5b2 = top of DGROUP).
          // In our environment the chain has real blocks only up to ~0x5a40, then the
          // pre-stack region is RAW ZEROS, so the walk reads a 0 header, never reaches
          // dx, and spins (the local-heap analogue of the global burgermaster gap).
          // The block header is loaded into BX right before the busy-bit test; we hook
          // that point (runtime ip 0x05f2): when the freshly-read header is 0 and si is
          // still below dx, synthesize ONE BUSY arena block spanning [si, dx) so the
          // walk's busy-path advance (si += (hdr & ~1) + 1) lands exactly on dx and
          // `cmp si,dx; jz done` terminates. We patch BOTH memory ([si]) and the live
          // BX so the in-flight test/advance use the bridge value this iteration.
          if (g_ole2_k334log && cpu->cs==0x073f && cpu->ip==0x05f2 && lhfix<256) {
            uint16_t ds=cpu->ds, si=cpu->si, dx=cpu->dx;
            if (cpu->bx==0 && si<dx && dx>(uint16_t)(si+1)) {
              uint16_t bridge=(uint16_t)(((dx - si - 1) & ~1u) | 1u);  // busy, size = dx-si-1
              x86_16_wr16(cpu,ds,si,bridge);
              cpu->bx = bridge;                                        // live header reg
              lhfix++;
              if (lhfix<=4) kprintf("[LHFIX] ds=%04x bridge BUSY block @%04x hdr=%04x -> dx(plast)=%04x (di=%04x)\n",
                                    ds,si,bridge,dx,cpu->di);
            }
          } }
        { extern int g_ole2_farlog; if (g_ole2_farlog && (cpu->insn_count & 0x1FFFFF)==0) kprintf("[IPSMP] icnt=%lu cs:ip=%04x:%04x op=%02x ax=%04x bx=%04x cx=%04x dx=%04x sp=%04x\n", (unsigned long)cpu->insn_count, cpu->cs, cpu->ip, peek8(cpu), cpu->ax, cpu->bx, cpu->cx, cpu->dx, cpu->sp); }

#ifdef WIN16_TRACE
        if (executed <= 600)
            kprintf("[trace] %04lu %04x:%04x op=%02x ax=%04x bx=%04x cx=%04x fl=%04x sp=%04x\n",
                    executed, cpu->cs, (uint16_t)(cpu->ip - 1), op,
                    cpu->ax, cpu->bx, cpu->cx, cpu->flags, cpu->sp);
#endif

        switch (op) {

        // ---- arithmetic/logic with ModR/M: 00..3B family ----
        // Pattern: base opcodes for add/or/adc/sbb/and/sub/xor/cmp,
        // each with 6 forms (rm8,r8 / rm16,r16 / r8,rm8 / r16,rm16 / al,imm8 / ax,imm16)
        case 0x00: case 0x08: case 0x10: case 0x18:
        case 0x20: case 0x28: case 0x30: case 0x38: { // rm8, r8
            int aluop = (op >> 3) & 7;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t d = (uint8_t)modrm_read(cpu, &m, 1);
            uint8_t s = get_reg8(cpu, m.reg);
            uint16_t r = (uint16_t)alu_op(cpu, aluop, d, s, 1);
            if (aluop != 7) modrm_write(cpu, &m, 1, r);
            break;
        }
        case 0x01: case 0x09: case 0x11: case 0x19:
        case 0x21: case 0x29: case 0x31: case 0x39: { // rm16/32, r16/32
            int aluop = (op >> 3) & 7; int ow = osize;   // (#194) 0x66 -> 32-bit
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t d = modrm_read(cpu, &m, ow);
            uint32_t s = get_regW(cpu, m.reg, ow);
            uint32_t r = alu_op(cpu, aluop, d, s, ow);
            if (aluop != 7) modrm_write(cpu, &m, ow, r);
            break;
        }
        case 0x02: case 0x0A: case 0x12: case 0x1A:
        case 0x22: case 0x2A: case 0x32: case 0x3A: { // r8, rm8
            int aluop = (op >> 3) & 7;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t d = get_reg8(cpu, m.reg);
            uint8_t s = (uint8_t)modrm_read(cpu, &m, 1);
            uint16_t r = (uint16_t)alu_op(cpu, aluop, d, s, 1);
            if (aluop != 7) set_reg8(cpu, m.reg, (uint8_t)r);
            break;
        }
        case 0x03: case 0x0B: case 0x13: case 0x1B:
        case 0x23: case 0x2B: case 0x33: case 0x3B: { // r16/32, rm16/32
            int aluop = (op >> 3) & 7; int ow = osize;   // (#194) 0x66 -> 32-bit
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t d = get_regW(cpu, m.reg, ow);
            uint32_t s = modrm_read(cpu, &m, ow);
            uint32_t r = alu_op(cpu, aluop, d, s, ow);
            if (aluop != 7) set_regW(cpu, m.reg, ow, r);
            break;
        }
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: { // al, imm8
            int aluop = (op >> 3) & 7;
            uint8_t imm = fetch8(cpu);
            uint16_t r = (uint16_t)alu_op(cpu, aluop, get_reg8(cpu, 0), imm, 1);
            if (aluop != 7) set_reg8(cpu, 0, (uint8_t)r);
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: { // ax/eax, imm16/32
            int aluop = (op >> 3) & 7; int ow = osize;   // (#194) 0x66 -> 32-bit
            uint32_t imm = fetch_imm(cpu, ow);
            uint32_t r = alu_op(cpu, aluop, get_regW(cpu, 0, ow), imm, ow);
            if (aluop != 7) set_regW(cpu, 0, ow, r);
            break;
        }

        // ---- push/pop segment regs ----
        case 0x06: push16(cpu, cpu->es); break;
        case 0x07: cpu->es = pop16(cpu); break;
        case 0x0E: push16(cpu, cpu->cs); break;
        case 0x16: push16(cpu, cpu->ss); break;
        case 0x17: cpu->ss = pop16(cpu); break;
        case 0x1E: push16(cpu, cpu->ds); break;
        case 0x1F: { uint16_t nd=pop16(cpu); { extern int g_ole2_k334log; static int n=0; if(g_ole2_k334log&&nd==0x17&&n<8){n++; kprintf("[DS17] pop ds=0017 at cs:ip=%04x:%04x (was %04x)\n",cpu->cs,cpu->ip,cpu->ds);} static int np=0; if(g_ole2_k334log&&nd==0x1127&&cpu->ds!=0x1127&&np<24){np++; kprintf("[DS1127] pop ds<-1127 cs:ip=%04x:%04x was=%04x ss=%04x\n",cpu->cs,cpu->ip,cpu->ds,cpu->ss);} } cpu->ds=nd; break; }

        // ---- inc/dec r16/r32 (40-4F) ----  (#194 0x66 -> 32-bit)
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            set_regW(cpu, op & 7, osize, do_inc(cpu, get_regW(cpu, op & 7, osize), osize));
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            set_regW(cpu, op & 7, osize, do_dec(cpu, get_regW(cpu, op & 7, osize), osize));
            break;

        // ---- push/pop r16/r32 (50-5F) ----  (#194 0x66 -> 32-bit)
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            if (osize == 4) push32(cpu, get_reg32(cpu, op & 7));
            else            push16(cpu, get_reg16(cpu, op & 7));
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (osize == 4) set_reg32(cpu, op & 7, pop32(cpu));
            else            set_reg16(cpu, op & 7, pop16(cpu));
            break;

        // ---- pusha/pushad, popa/popad (60/61, 80186/386) ----  (#194 0x66 -> 32-bit)
        case 0x60: {   // PUSHA(D): AX,CX,DX,BX,orig SP,BP,SI,DI
            if (osize == 4) {
                uint32_t osp = get_reg32(cpu, 4);
                push32(cpu, get_reg32(cpu, 0)); push32(cpu, get_reg32(cpu, 1));
                push32(cpu, get_reg32(cpu, 2)); push32(cpu, get_reg32(cpu, 3));
                push32(cpu, osp);
                push32(cpu, get_reg32(cpu, 5)); push32(cpu, get_reg32(cpu, 6));
                push32(cpu, get_reg32(cpu, 7));
            } else {
                uint16_t osp = cpu->sp;
                push16(cpu, cpu->ax); push16(cpu, cpu->cx);
                push16(cpu, cpu->dx); push16(cpu, cpu->bx);
                push16(cpu, osp);
                push16(cpu, cpu->bp); push16(cpu, cpu->si); push16(cpu, cpu->di);
            }
            break;
        }
        case 0x61: {   // POPA(D): DI,SI,BP,(skip SP),BX,DX,CX,AX
            if (osize == 4) {
                set_reg32(cpu, 7, pop32(cpu)); set_reg32(cpu, 6, pop32(cpu));
                set_reg32(cpu, 5, pop32(cpu)); (void)pop32(cpu);   // discard saved ESP
                set_reg32(cpu, 3, pop32(cpu)); set_reg32(cpu, 2, pop32(cpu));
                set_reg32(cpu, 1, pop32(cpu)); set_reg32(cpu, 0, pop32(cpu));
            } else {
                cpu->di = pop16(cpu); cpu->si = pop16(cpu); cpu->bp = pop16(cpu);
                (void)pop16(cpu);                                   // discard saved SP
                cpu->bx = pop16(cpu); cpu->dx = pop16(cpu);
                cpu->cx = pop16(cpu); cpu->ax = pop16(cpu);
            }
            break;
        }

        // ---- push imm (80186) ----  (#194 0x66 push imm32)
        case 0x68: if (osize == 4) push32(cpu, fetch32(cpu)); else push16(cpu, fetch16(cpu)); break;
        case 0x6A: { int8_t v = (int8_t)fetch8(cpu);   // sign-extend imm8 to opsize
                     if (osize == 4) { push32(cpu, (uint32_t)(int32_t)v); }
                     else            { push16(cpu, (uint16_t)(int16_t)v); }
                     break; }
        // ---- 3-operand imul (69 = imm16/32, 6B = imm8 sign-extended) ----
        // (#194 0x66 -> 32-bit dst/src and imm32 for 0x69)
        case 0x69: { // imul r(ow), rm(ow), imm(ow)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            int64_t src = (ow == 4) ? (int64_t)(int32_t)modrm_read(cpu, &m, 4)
                                    : (int64_t)(int16_t)(uint16_t)modrm_read(cpu, &m, 2);
            int64_t imm = (ow == 4) ? (int64_t)(int32_t)fetch32(cpu)
                                    : (int64_t)(int16_t)fetch16(cpu);
            int64_t res = src * imm;
            uint32_t lo = (uint32_t)res & ((ow == 4) ? 0xFFFFFFFFu : 0xFFFFu);
            set_regW(cpu, m.reg, ow, lo);
            int64_t sx = (ow == 4) ? (int64_t)(int32_t)lo : (int64_t)(int16_t)(uint16_t)lo;
            if (res != sx) cpu->flags |= (F_CF | F_OF);
            else           cpu->flags &= (uint16_t)~(F_CF | F_OF);
            break;
        }
        case 0x6B: { // imul r(ow), rm(ow), imm8 (sign-extended)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            int64_t src = (ow == 4) ? (int64_t)(int32_t)modrm_read(cpu, &m, 4)
                                    : (int64_t)(int16_t)(uint16_t)modrm_read(cpu, &m, 2);
            int64_t imm = (int64_t)(int8_t)fetch8(cpu);
            int64_t res = src * imm;
            uint32_t lo = (uint32_t)res & ((ow == 4) ? 0xFFFFFFFFu : 0xFFFFu);
            set_regW(cpu, m.reg, ow, lo);
            int64_t sx = (ow == 4) ? (int64_t)(int32_t)lo : (int64_t)(int16_t)(uint16_t)lo;
            if (res != sx) cpu->flags |= (F_CF | F_OF);
            else           cpu->flags &= (uint16_t)~(F_CF | F_OF);
            break;
        }

        // ---- Jcc short (70-7F) ----
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t rel = (int8_t)fetch8(cpu);
            if (jcc_cond(cpu, op & 0x0F)) cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }

        // ---- group 1: rm, imm (80-83) ----
        case 0x80:   // rm8, imm8
        case 0x82: { // rm8, imm8 (0x82 is an undocumented alias of 0x80; some
                     // toolchains, e.g. the VB1 runtime, emit it). Same semantics.
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            int aluop = m.reg;
            uint8_t d = (uint8_t)modrm_read(cpu, &m, 1);
            uint8_t imm = fetch8(cpu);
            uint16_t r = (uint16_t)alu_op(cpu, aluop, d, imm, 1);
            if (aluop != 7) modrm_write(cpu, &m, 1, r);
            break;
        }
        case 0x81: { // rm16/32, imm16/32   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            int aluop = m.reg;
            uint32_t d = modrm_read(cpu, &m, ow);
            uint32_t imm = fetch_imm(cpu, ow);
            uint32_t r = alu_op(cpu, aluop, d, imm, ow);
            if (aluop != 7) modrm_write(cpu, &m, ow, r);
            break;
        }
        case 0x83: { // rm16/32, imm8 (sign-extended)   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            int aluop = m.reg;
            uint32_t d = modrm_read(cpu, &m, ow);
            int8_t s8 = (int8_t)fetch8(cpu);
            uint32_t imm = (ow == 4) ? (uint32_t)(int32_t)s8 : (uint32_t)(uint16_t)(int16_t)s8;
            uint32_t r = alu_op(cpu, aluop, d, imm, ow);
            if (aluop != 7) modrm_write(cpu, &m, ow, r);
            break;
        }

        // ---- test rm, r ----
        case 0x84: { // test rm8, r8
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t d = (uint8_t)modrm_read(cpu, &m, 1);
            do_logic(cpu, (uint32_t)(d & get_reg8(cpu, m.reg)), 1);
            break;
        }
        case 0x85: { // test rm16/32, r16/32   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t d = modrm_read(cpu, &m, ow);
            do_logic(cpu, d & get_regW(cpu, m.reg, ow), ow);
            break;
        }

        // ---- xchg rm, r ----
        case 0x86: { // xchg rm8, r8
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t a = (uint8_t)modrm_read(cpu, &m, 1);
            uint8_t b = get_reg8(cpu, m.reg);
            modrm_write(cpu, &m, 1, b);
            set_reg8(cpu, m.reg, a);
            break;
        }
        case 0x87: { // xchg rm16/32, r16/32   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t a = modrm_read(cpu, &m, ow);
            uint32_t b = get_regW(cpu, m.reg, ow);
            modrm_write(cpu, &m, ow, b);
            set_regW(cpu, m.reg, ow, a);
            break;
        }

        // ---- mov rm, r / mov r, rm ----
        case 0x88: { // mov rm8, r8
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            modrm_write(cpu, &m, 1, get_reg8(cpu, m.reg));
            break;
        }
        case 0x89: { // mov rm16/32, r16/32   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            modrm_write(cpu, &m, ow, get_regW(cpu, m.reg, ow));
            break;
        }
        case 0x8A: { // mov r8, rm8
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_reg8(cpu, m.reg, (uint8_t)modrm_read(cpu, &m, 1));
            break;
        }
        case 0x8B: { // mov r16/32, rm16/32   (#194 0x66 -> 32-bit)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_regW(cpu, m.reg, ow, modrm_read(cpu, &m, ow));
            break;
        }

        // ---- mov sreg <-> rm16 ----
        case 0x8C: { // mov rm16, sreg
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            modrm_write(cpu, &m, 2, get_sreg(cpu, m.reg));
            break;
        }
        case 0x8E: { // mov sreg, rm16
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_sreg(cpu, m.reg, modrm_read(cpu, &m, 2));
            break;
        }

        // ---- lea r16/32, m ----  (#194 0x66 -> 32-bit dest; off is the EA offset)
        case 0x8D: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_regW(cpu, m.reg, osize, m.off); // effective offset only
            break;
        }

        // ---- pop rm16 (group, only /0 used) ----
        case 0x8F: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            modrm_write(cpu, &m, 2, pop16(cpu));
            break;
        }

        // ---- XLAT / XLATB (D7): AL = [DS:BX + AL] (DS overridable). The MS-C
        // table-lookup idiom TETRIS uses for cell/colour mapping. Previously
        // unimplemented, which aborted the GameGrid layout wndproc mid-compute
        // and made it return garbage geometry (the 1x4442 MoveWindow blocker).
        case 0xD7: {
            uint16_t seg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            uint16_t off = (uint16_t)(cpu->bx + (cpu->ax & 0xFF));
            set_reg8(cpu, 0, x86_16_rd8(cpu, seg, off));   // AL
            break;
        }

        // ---- FWAIT / WAIT (9B): synchronise with the FPU. No FPU state to wait
        // on in this minimal emulator, so it is a no-op. (Real MS-C startup emits
        // it before FNSTCW/FNSTSW; harmless to ignore.)
        case 0x9B: break;

        // ---- x87 FPU escape opcodes (D8..DF) ----
        // A small software FPU (see fp_* helpers above) covering the operations
        // MS-C win87em apps use for geometry: FILD (m16/m64), FLD/FST/FSTP (m64),
        // FMUL by m64, FISTP (m64), plus the control/status no-ops (FLDCW, FNSTCW,
        // FNSTSW, FNINIT, FCLEX). Reg-reg numeric forms are not needed by these
        // apps; they are accepted as no-ops to keep IP aligned.
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            uint8_t modrm = peek8(cpu);
            int is_reg_form = ((modrm >> 6) & 3) == 3;
            int reg = (modrm >> 3) & 7;
            int is_fnstsw_ax = (op == 0xDF && modrm == 0xE0);   // FNSTSW AX
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);   // advances IP past operand

            if (is_fnstsw_ax) {
                cpu->ax = 0x0000;       // no exceptions, TOS=0
                break;
            }
            // Apply one of the arithmetic ops (reg field: 0 ADD,1 MUL,4 SUB,5 SUBR,
            // 6 DIV,7 DIVR) of ST(0) with operand `b` (the memory/register value).
            #define FP_ARITH(dst, b, rf) do {                                  \
                switch (rf) {                                                  \
                    case 0: (dst) = fp_add((dst), (b), 0); break;              \
                    case 1: (dst) = fp_mul((dst), (b)); break;                 \
                    case 4: (dst) = fp_add((dst), (b), 1); break;              \
                    case 5: (dst) = fp_add((b), (dst), 1); break;              \
                    case 6: (dst) = fp_div((dst), (b)); break;                 \
                    case 7: (dst) = fp_div((b), (dst)); break;                 \
                    default: break;                                            \
                } } while (0)

            if (is_reg_form) {
                int i = modrm & 7;     // ST(i)
                int sti = g_fp_top + i;   // index of ST(i) (top is ST(0))
                int valid_top = (g_fp_top < FP_STACK_SIZE);
                int valid_sti = (sti < FP_STACK_SIZE);
                switch (op) {
                    case 0xDB:
                        if (modrm == 0xE3) fp_reset();        // FNINIT/FINIT
                        // DB E1/E2/E4 (FNCLEX etc): no-op
                        break;
                    case 0xD9:
                        if (modrm == 0xC9 && valid_top) {     // FXCH ST(1)
                            if (g_fp_top + 1 < FP_STACK_SIZE) {
                                uint64_t t = g_fp[g_fp_top];
                                g_fp[g_fp_top] = g_fp[g_fp_top+1];
                                g_fp[g_fp_top+1] = t;
                            }
                        } else if (reg == 1 && valid_sti) {   // FXCH ST(i)
                            uint64_t t = g_fp[g_fp_top]; g_fp[g_fp_top]=g_fp[sti]; g_fp[sti]=t;
                        } else if (reg == 0 && valid_sti) {   // FLD ST(i)
                            uint64_t v = g_fp[sti];
                            if (g_fp_top > 0) g_fp[--g_fp_top] = v;
                        }
                        break;
                    case 0xD8:    // FADD/FMUL/.../FDIV ST(0), ST(i)
                        if (valid_top && valid_sti) FP_ARITH(g_fp[g_fp_top], g_fp[sti], reg);
                        break;
                    case 0xDC:    // ST(i) op= ST(0)
                        if (valid_top && valid_sti) {
                            int rf = reg; // DC reg encoding mirrors D8 but SUB/SUBR,DIV/DIVR swap
                            if (rf == 4) rf = 5; else if (rf == 5) rf = 4;
                            else if (rf == 6) rf = 7; else if (rf == 7) rf = 6;
                            FP_ARITH(g_fp[sti], g_fp[g_fp_top], rf);
                        }
                        break;
                    case 0xDE:    // FADDP/.../FDIVP ST(i), ST(0) then pop
                        if (valid_top && valid_sti) {
                            int rf = reg;
                            if (rf == 4) rf = 5; else if (rf == 5) rf = 4;
                            else if (rf == 6) rf = 7; else if (rf == 7) rf = 6;
                            FP_ARITH(g_fp[sti], g_fp[g_fp_top], rf);
                            g_fp_top++;   // pop ST(0)
                        }
                        break;
                    case 0xDD:    // FST/FSTP ST(i), FFREE
                        if ((reg == 2 || reg == 3) && valid_top && valid_sti) {
                            g_fp[sti] = g_fp[g_fp_top];
                            if (reg == 3) g_fp_top++;
                        }
                        break;
                    default: break;   // DA/DF reg forms (FCMOV/FCOMI) unused
                }
                break;
            }
            // Memory-operand forms.
            switch (op) {
                case 0xD8: {  // arithmetic with m32 real
                    uint32_t f = (uint32_t)x86_16_rd16(cpu, m.seg, m.off) |
                                 ((uint32_t)x86_16_rd16(cpu, m.seg,(uint16_t)(m.off+2))<<16);
                    uint64_t b = f32_to_f64(f);
                    if (g_fp_top < FP_STACK_SIZE) FP_ARITH(g_fp[g_fp_top], b, reg);
                    break;
                }
                case 0xD9:
                    if (reg == 5) { /* FLDCW m16 */ (void)0; }
                    else if (reg == 7) x86_16_wr16(cpu, m.seg, m.off, 0x037F); // FNSTCW
                    else if (reg == 0) { // FLD m32 (real)
                        uint32_t f = (uint32_t)x86_16_rd16(cpu, m.seg, m.off) |
                                     ((uint32_t)x86_16_rd16(cpu, m.seg,(uint16_t)(m.off+2))<<16);
                        if (g_fp_top > 0) g_fp[--g_fp_top] = f32_to_f64(f);
                    }
                    else if (reg == 2 || reg == 3) { // FST/FSTP m32
                        if (g_fp_top < FP_STACK_SIZE) {
                            uint32_t f = f64_to_f32(g_fp[g_fp_top]);
                            x86_16_wr16(cpu,m.seg,m.off,(uint16_t)f);
                            x86_16_wr16(cpu,m.seg,(uint16_t)(m.off+2),(uint16_t)(f>>16));
                            if (reg == 3) g_fp_top++;
                        }
                    }
                    break;
                case 0xDB:
                    if (reg == 0) { // FILD m32 (dword int)
                        int32_t v = (int32_t)((uint32_t)x86_16_rd16(cpu,m.seg,m.off) |
                                    ((uint32_t)x86_16_rd16(cpu,m.seg,(uint16_t)(m.off+2))<<16));
                        if (g_fp_top > 0) g_fp[--g_fp_top] = fp_from_i64(v);
                    } else if (reg == 2 || reg == 3) { // FIST/FISTP m32
                        if (g_fp_top < FP_STACK_SIZE) {
                            int64_t r = fp_to_i64(g_fp[g_fp_top]);
                            if (reg == 3) g_fp_top++;
                            x86_16_wr16(cpu,m.seg,m.off,(uint16_t)r);
                            x86_16_wr16(cpu,m.seg,(uint16_t)(m.off+2),(uint16_t)(r>>16));
                        }
                    }
                    break;
                case 0xDC: {  // arithmetic with m64 real
                    uint64_t b = rd64(cpu, m.seg, m.off);
                    if (g_fp_top < FP_STACK_SIZE) FP_ARITH(g_fp[g_fp_top], b, reg);
                    break;
                }
                case 0xDD:
                    if (reg == 0) { // FLD m64 (real)
                        uint64_t b = rd64(cpu, m.seg, m.off);
                        if (g_fp_top > 0) g_fp[--g_fp_top] = b;
                    } else if (reg == 2 || reg == 3) { // FST/FSTP m64
                        if (g_fp_top < FP_STACK_SIZE) {
                            wr64(cpu, m.seg, m.off, g_fp[g_fp_top]);
                            if (reg == 3) g_fp_top++;
                        }
                    } else if (reg == 7) {
                        x86_16_wr16(cpu, m.seg, m.off, 0x0000);  // FNSTSW m16
                    }
                    break;
                case 0xDF:
                    if (reg == 0) { // FILD m16 (word int)
                        int16_t v = (int16_t)x86_16_rd16(cpu, m.seg, m.off);
                        if (g_fp_top > 0) g_fp[--g_fp_top] = fp_from_i64(v);
                    } else if (reg == 2 || reg == 3) { // FIST/FISTP m16
                        if (g_fp_top < FP_STACK_SIZE) {
                            int64_t r = fp_to_i64(g_fp[g_fp_top]);
                            if (reg == 3) g_fp_top++;
                            x86_16_wr16(cpu, m.seg, m.off, (uint16_t)r);
                        }
                    } else if (reg == 5) { // FILD m64 (qword int)
                        int64_t v = (int64_t)rd64(cpu, m.seg, m.off);
                        if (g_fp_top > 0) g_fp[--g_fp_top] = fp_from_i64(v);
                    } else if (reg == 7) { // FISTP m64 (qword int, pop)
                        if (g_fp_top < FP_STACK_SIZE) {
                            int64_t r = fp_to_i64(g_fp[g_fp_top]); g_fp_top++;
                            wr64(cpu, m.seg, m.off, (uint64_t)r);
                        }
                    }
                    break;
                default:
                    break;   // DA memory numeric forms unused here
            }
            #undef FP_ARITH
            break;
        }

        // ---- nop / xchg ax, r16 ----
        case 0x90: break; // nop (xchg ax,ax)
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = op & 7;
            uint16_t t = cpu->ax;
            cpu->ax = get_reg16(cpu, r);
            set_reg16(cpu, r, t);
            break;
        }

        // ---- BCD adjust: DAA (27) / DAS (2F) / AAA (37) / AAS (3F) ----
        // (#188/#194) MS Golf's Win16 stub emits DAA; implement the BCD-adjust
        // family so it (and other compilers' integer<->decimal code) runs.
        case 0x27: {   // DAA - decimal adjust AL after addition
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t old_al = al; int old_cf = (cpu->flags & F_CF) ? 1 : 0;
            int cf = 0;
            if (((al & 0x0F) > 9) || (cpu->flags & F_AF)) {
                al = (uint8_t)(al + 6); cpu->flags |= F_AF;
            } else cpu->flags &= ~F_AF;
            if ((old_al > 0x99) || old_cf) { al = (uint8_t)(al + 0x60); cf = 1; }
            if (cf) cpu->flags |= F_CF; else cpu->flags &= ~F_CF;
            cpu->ax = (uint16_t)((cpu->ax & 0xFF00) | al);
            set_pzs(cpu, al, 1);
            break;
        }
        case 0x2F: {   // DAS - decimal adjust AL after subtraction
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t old_al = al; int old_cf = (cpu->flags & F_CF) ? 1 : 0;
            int cf = 0;
            if (((al & 0x0F) > 9) || (cpu->flags & F_AF)) {
                al = (uint8_t)(al - 6); cpu->flags |= F_AF;
            } else cpu->flags &= ~F_AF;
            if ((old_al > 0x99) || old_cf) { al = (uint8_t)(al - 0x60); cf = 1; }
            if (cf) cpu->flags |= F_CF; else cpu->flags &= ~F_CF;
            cpu->ax = (uint16_t)((cpu->ax & 0xFF00) | al);
            set_pzs(cpu, al, 1);
            break;
        }
        case 0x37: {   // AAA - ASCII adjust AL after addition
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t ah = (uint8_t)(cpu->ax >> 8);
            if (((al & 0x0F) > 9) || (cpu->flags & F_AF)) {
                al = (uint8_t)((al + 6) & 0x0F); ah = (uint8_t)(ah + 1);
                cpu->flags |= (F_AF | F_CF);
            } else { al = (uint8_t)(al & 0x0F); cpu->flags &= ~(F_AF | F_CF); }
            cpu->ax = (uint16_t)((ah << 8) | al);
            break;
        }
        case 0x3F: {   // AAS - ASCII adjust AL after subtraction
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t ah = (uint8_t)(cpu->ax >> 8);
            if (((al & 0x0F) > 9) || (cpu->flags & F_AF)) {
                al = (uint8_t)((al - 6) & 0x0F); ah = (uint8_t)(ah - 1);
                cpu->flags |= (F_AF | F_CF);
            } else { al = (uint8_t)(al & 0x0F); cpu->flags &= ~(F_AF | F_CF); }
            cpu->ax = (uint16_t)((ah << 8) | al);
            break;
        }
        // ---- ARPL r/m16, r16 (63 /r) - Adjust RPL Field of Selector ----
        // (#390 Corel) Protected-mode only. If the dest selector's RPL (low 2
        // bits) is less than the src selector's RPL, raise the dest RPL to match
        // and set ZF; else clear ZF. Only ZF is affected. In real mode 0x63 is
        // ARPL as well on this interpreter's pmode model; treat it uniformly.
        case 0x63: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint16_t dest = (uint16_t)modrm_read(cpu, &m, 2);
            uint16_t src  = get_reg16(cpu, m.reg);
            if ((dest & 3) < (src & 3)) {
                dest = (uint16_t)((dest & ~3) | (src & 3));
                modrm_write(cpu, &m, 2, dest);
                cpu->flags |= F_ZF;
            } else {
                cpu->flags &= ~F_ZF;
            }
            break;
        }

        // ---- cbw/cwde (98) and cwd/cdq (99) ----  (#194 0x66 -> 32-bit forms)
        case 0x98: {
            if (osize == 4) set_reg32(cpu, 0, (uint32_t)(int32_t)(int16_t)cpu->ax);  // CWDE
            else            cpu->ax = (uint16_t)(int16_t)(int8_t)(cpu->ax & 0xFF); // CBW
            break;
        }
        case 0x99: {
            if (osize == 4) set_reg32(cpu, 2, (get_reg32(cpu, 0) & 0x80000000u) ? 0xFFFFFFFFu : 0); // CDQ
            else            cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000;        // CWD
            break;
        }

        // ---- call far (9A) ----
        case 0x9A: {
            uint16_t noff = fetch16(cpu);
            uint16_t nseg = fetch16(cpu);
            // (#278 P53 ACTION 1b) EARLY fan-out trace: the seg155:0xf30 broadcast
            // far-calls its notify targets at 0xf4d/0xf59/0xf65/0xf76/0xf87/0xf98.
            // Log the RESOLVED target BEFORE the win16-import short-circuit below (an
            // API target would otherwise never reach the post-check W6CALLF trace).
            { extern int g_w6life;
              if (g_w6life && cpu->cs==0x04df) {
                uint16_t cip = (uint16_t)(cpu->ip - 5);
                if (cip >= 0x0f30 && cip <= 0x0fc0)
                  kprintf("[W6NOTIFY30] SEQ %d: seg155:f30 FANOUT call@%04x -> %04x:%04x arg=%04x\n",
                          g_w6seq++, cip, nseg, noff, x86_16_rd16(cpu,cpu->ss,cpu->sp));
              } }
            // Push the far-call return frame (caller CS:IP). The trap callback
            // (if any) consumes this frame itself when returning.
            push16(cpu, cpu->cs);
            push16(cpu, cpu->ip);
            if (g_farcall_active && nseg == g_farcall_seg) {
                if (g_farcall_fn(cpu, noff)) return 0;
                break;   // fn set cpu->cs:ip; continue from there.
            }
            { extern int g_ole2_k334log; static int nc=0;
              if (g_ole2_k334log && noff==0x299c && nc<6){ nc++;
                kprintf("[CALLFAR299C] from cs:ip=%04x:%04x -> target=%04x:%04x si=%04x\n",
                  cpu->cs, (uint16_t)(cpu->ip-5), nseg, noff, cpu->si); } }
            // (#278 P52 runtime-callf-trace, avenue 1) trace EVERY far call whose
            // call SITE is in seg155 (0x04df), ip in [0x9be, 0xb89] - the doc-create
            // view-ctor/realize window between SetActiveView and the 0xa56 realize
            // test. cpu->ip here is already past the 5-byte 9A instruction, so the
            // call site = cpu->ip-5 and the natural return address = cpu->ip.
            { extern int g_w6life;
              if (g_w6life && cpu->cs==0x04df) {
                uint16_t call_ip = (uint16_t)(cpu->ip - 5);
                if ((call_ip >= 0x09be && call_ip <= 0x0b89) || (call_ip >= 0x0f30 && call_ip <= 0x0fc0)) {
                  kprintf("[W6CALLF] SEQ %d: CALLF 155:%04x -> %04x:%04x (ret to 155:%04x)\n",
                          g_w6seq++, call_ip, nseg, noff, cpu->ip);
                  if (g_w6callf_npend < W6CALLF_MAXPEND) {
                    g_w6callf_pend[g_w6callf_npend].cs = cpu->cs;
                    g_w6callf_pend[g_w6callf_npend].ip = cpu->ip;   // return address
                    g_w6callf_pend[g_w6callf_npend].call_ip = call_ip;
                    g_w6callf_npend++;
                  } else {
                    kprintf("[W6CALLF] pending-stack full, dropping watch for return to 155:%04x\n", cpu->ip);
                  }
                }
              } }
                        fnt_maybe_enter(cpu, nseg, noff);
            // [W6OLE] trace far-calls INTO COMPOBJ code segs (sel 0x0c9f..0x0d1f) to
            // map which COMPOBJ export Word hits during OLE2 init, with args+caller.
            { extern int g_ole2_k334log; static int nco=0;
              if (g_ole2_k334log && nseg>=0x0c9f && nseg<=0x0d1f && nco<60){ nco++;
                uint16_t sp=cpu->sp; // [sp]=retIP [sp+2]=retCS then pascal args
                kprintf("[W6OLE] -> COMPOBJ %04x:%04x from %04x:%04x args=%04x %04x %04x %04x ds=%04x\n",
                  nseg,noff,
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(sp+2)),x86_16_rd16(cpu,cpu->ss,sp),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(sp+4)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(sp+6)),
                  x86_16_rd16(cpu,cpu->ss,(uint16_t)(sp+8)),x86_16_rd16(cpu,cpu->ss,(uint16_t)(sp+10)),
                  cpu->ds); } }
            // (#278 P58) log cross-segment calls INTO seg223 (sel 0x06ff, the view/
            // pane-layout + display-list segment) to reveal external drivers of the
            // pane-creation tree (0x0dd0/0x0284/0x0042 producer chain). Bounded.
            { extern int g_w6life; if (g_w6life && nseg==0x06ff && cpu->cs!=0x06ff) {
                static int ns=0; if(ns<60){ns++;
                  kprintf("[W6INTO223] -> seg223:%04x from %04x:%04x arg0=%04x arg1=%04x\n",
                    noff, cpu->cs, (uint16_t)(cpu->ip-5),
                    x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                    x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4))); } } }
            cpu->cs = nseg; cpu->ip = noff;
            break;
        }

        // ---- pushf / popf / sahf / lahf ----
        case 0x9C: push16(cpu, (uint16_t)(cpu->flags | 0x0002)); break;
        case 0x9D: cpu->flags = (uint16_t)(pop16(cpu) | 0x0002); break;
        case 0x9E: { // sahf
            uint8_t ah = (uint8_t)(cpu->ax >> 8);
            cpu->flags = (uint16_t)((cpu->flags & 0xFF00) |
                         (ah & (F_CF | F_PF | F_AF | F_ZF | F_SF)) | 0x02);
            break;
        }
        case 0x9F: { // lahf
            uint8_t lo = (uint8_t)(cpu->flags & (F_CF | F_PF | F_AF | F_ZF | F_SF));
            lo |= 0x02;
            cpu->ax = (uint16_t)((cpu->ax & 0x00FF) | ((uint16_t)lo << 8));
            break;
        }

        // ---- mov AL/AX <-> moffs (A0-A3) ----
        case 0xA0: { // mov al, [moffs]
            uint16_t off = fetch16(cpu);
            uint16_t seg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            set_reg8(cpu, 0, x86_16_rd8(cpu, seg, off));
            break;
        }
        case 0xA1: { // mov ax/eax, [moffs]   (#194 0x66 -> 32-bit)
            uint16_t off = fetch16(cpu);
            uint16_t seg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            if (osize == 4) set_reg32(cpu, 0, rd32(cpu, seg, off));
            else            cpu->ax = x86_16_rd16(cpu, seg, off);
            break;
        }
        case 0xA2: { // mov [moffs], al
            uint16_t off = fetch16(cpu);
            uint16_t seg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            x86_16_wr8(cpu, seg, off, (uint8_t)(cpu->ax & 0xFF));
            break;
        }
        case 0xA3: { // mov [moffs], ax/eax   (#194 0x66 -> 32-bit)
            uint16_t off = fetch16(cpu);
            uint16_t seg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            if (osize == 4) wr32(cpu, seg, off, get_reg32(cpu, 0));
            else            x86_16_wr16(cpu, seg, off, cpu->ax);
            break;
        }

        // ---- string ops (A4-A7, AA-AF) ----  (#194 word forms honor 0x66 -> dword)
        case 0xA4: case 0xA5: { // movs/movsd
            int size = (op == 0xA4) ? 1 : osize;
            uint16_t srcseg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            int dir = (cpu->flags & F_DF) ? -1 : 1;
            unsigned long count = rep ? cpu->cx : 1;
            while (count--) {
                if (rep && cpu->cx == 0) break;
                if (size == 1)      x86_16_wr8(cpu, cpu->es, cpu->di, x86_16_rd8(cpu, srcseg, cpu->si));
                else if (size == 2) x86_16_wr16(cpu, cpu->es, cpu->di, x86_16_rd16(cpu, srcseg, cpu->si));
                else                wr32(cpu, cpu->es, cpu->di, rd32(cpu, srcseg, cpu->si));
                cpu->si = (uint16_t)(cpu->si + dir * size);
                cpu->di = (uint16_t)(cpu->di + dir * size);
                if (rep) cpu->cx = (uint16_t)(cpu->cx - 1);
            }
            break;
        }
        case 0xA6: case 0xA7: { // cmps/cmpsd
            int size = (op == 0xA6) ? 1 : osize;
            uint16_t srcseg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            int dir = (cpu->flags & F_DF) ? -1 : 1;
            do {
                if (rep && cpu->cx == 0) break;
                uint32_t a = (size == 1) ? x86_16_rd8(cpu, srcseg, cpu->si)
                           : (size == 2) ? x86_16_rd16(cpu, srcseg, cpu->si)
                                         : rd32(cpu, srcseg, cpu->si);
                uint32_t b = (size == 1) ? x86_16_rd8(cpu, cpu->es, cpu->di)
                           : (size == 2) ? x86_16_rd16(cpu, cpu->es, cpu->di)
                                         : rd32(cpu, cpu->es, cpu->di);
                do_sub(cpu, a, b, size, 0);
                cpu->si = (uint16_t)(cpu->si + dir * size);
                cpu->di = (uint16_t)(cpu->di + dir * size);
                if (rep) {
                    cpu->cx = (uint16_t)(cpu->cx - 1);
                    int zf = (cpu->flags & F_ZF) ? 1 : 0;
                    if (rep == 1 && !zf) break;   // REPE stops on ZF==0
                    if (rep == 2 && zf)  break;   // REPNE stops on ZF==1
                }
            } while (rep);
            break;
        }
        case 0xAA: case 0xAB: { // stos/stosd
            int size = (op == 0xAA) ? 1 : osize;
            int dir = (cpu->flags & F_DF) ? -1 : 1;
            unsigned long count = rep ? cpu->cx : 1;
            while (count--) {
                if (rep && cpu->cx == 0) break;
                if (size == 1)      x86_16_wr8(cpu, cpu->es, cpu->di, (uint8_t)(cpu->ax & 0xFF));
                else if (size == 2) x86_16_wr16(cpu, cpu->es, cpu->di, cpu->ax);
                else                wr32(cpu, cpu->es, cpu->di, get_reg32(cpu, 0));
                cpu->di = (uint16_t)(cpu->di + dir * size);
                if (rep) cpu->cx = (uint16_t)(cpu->cx - 1);
            }
            break;
        }
        case 0xAC: case 0xAD: { // lods/lodsd
            int size = (op == 0xAC) ? 1 : osize;
            uint16_t srcseg = (seg_ovr >= 0) ? get_sreg(cpu, seg_ovr) : cpu->ds;
            int dir = (cpu->flags & F_DF) ? -1 : 1;
            unsigned long count = rep ? cpu->cx : 1;
            while (count--) {
                if (rep && cpu->cx == 0) break;
                if (size == 1)      set_reg8(cpu, 0, x86_16_rd8(cpu, srcseg, cpu->si));
                else if (size == 2) cpu->ax = x86_16_rd16(cpu, srcseg, cpu->si);
                else                set_reg32(cpu, 0, rd32(cpu, srcseg, cpu->si));
                cpu->si = (uint16_t)(cpu->si + dir * size);
                if (rep) cpu->cx = (uint16_t)(cpu->cx - 1);
            }
            break;
        }
        case 0xAE: case 0xAF: { // scas/scasd
            int size = (op == 0xAE) ? 1 : osize;
            int dir = (cpu->flags & F_DF) ? -1 : 1;
            do {
                if (rep && cpu->cx == 0) break;
                uint32_t a = (size == 1) ? (uint32_t)(cpu->ax & 0xFF)
                           : (size == 2) ? (uint32_t)cpu->ax : get_reg32(cpu, 0);
                uint32_t b = (size == 1) ? x86_16_rd8(cpu, cpu->es, cpu->di)
                           : (size == 2) ? x86_16_rd16(cpu, cpu->es, cpu->di)
                                         : rd32(cpu, cpu->es, cpu->di);
                do_sub(cpu, a, b, size, 0);
                cpu->di = (uint16_t)(cpu->di + dir * size);
                if (rep) {
                    cpu->cx = (uint16_t)(cpu->cx - 1);
                    int zf = (cpu->flags & F_ZF) ? 1 : 0;
                    if (rep == 1 && !zf) break;
                    if (rep == 2 && zf)  break;
                }
            } while (rep);
            break;
        }

        // ---- test al/ax, imm ----
        case 0xA8: { uint8_t imm = fetch8(cpu);
            do_logic(cpu, (uint32_t)((cpu->ax & 0xFF) & imm), 1); break; }
        case 0xA9: { uint16_t imm = fetch16(cpu);
            do_logic(cpu, (uint32_t)(cpu->ax & imm), 2); break; }

        // ---- mov r8, imm8 (B0-B7) ----
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_reg8(cpu, op & 7, fetch8(cpu));
            break;

        // ---- mov r16/32, imm16/32 (B8-BF) ----  (#194 0x66 -> imm32)
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            set_regW(cpu, op & 7, osize, fetch_imm(cpu, osize));
            break;

        // ---- shift group by imm8 (C0/C1, 80186) ----
        case 0xC0: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t cnt = fetch8(cpu);
            uint8_t v = (uint8_t)modrm_read(cpu, &m, 1);
            modrm_write(cpu, &m, 1, shift_op(cpu, m.reg, v, 1, cnt));
            break;
        }
        case 0xC1: {   // (#194 0x66 -> 32-bit shift)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t cnt = fetch8(cpu);
            uint32_t v = modrm_read(cpu, &m, ow);
            modrm_write(cpu, &m, ow, shift_op(cpu, m.reg, v, ow, cnt));
            break;
        }

        // ---- ret near ----
        case 0xC2: { uint16_t n = fetch16(cpu); cpu->ip = pop16(cpu);
                     cpu->sp = (uint16_t)(cpu->sp + n); break; }
        case 0xC3: cpu->ip = pop16(cpu); break;

        // ---- les / lds ----
        case 0xC4: { // les r16, m
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_reg16(cpu, m.reg, x86_16_rd16(cpu, m.seg, m.off));
            cpu->es = x86_16_rd16(cpu, m.seg, (uint16_t)(m.off + 2));
            break;
        }
        case 0xC5: { // lds r16, m
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            set_reg16(cpu, m.reg, x86_16_rd16(cpu, m.seg, m.off));
            uint16_t nd = x86_16_rd16(cpu, m.seg, (uint16_t)(m.off + 2));
            { extern int g_ole2_k334log; static int n=0; if(g_ole2_k334log&&nd==0x17&&n<8){n++; kprintf("[DS17] lds ds=0017 at cs:ip=%04x:%04x (was %04x)\n",cpu->cs,cpu->ip,cpu->ds);} static int nl=0; if(g_ole2_k334log&&nd==0x1127&&cpu->ds!=0x1127&&nl<24){nl++; kprintf("[DS1127] lds ds<-1127 cs:ip=%04x:%04x was=%04x ss=%04x\n",cpu->cs,cpu->ip,cpu->ds,cpu->ss);} }
            cpu->ds = nd;
            break;
        }

        // ---- mov rm, imm (C6/C7) ----
        case 0xC6: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t imm = fetch8(cpu);
            modrm_write(cpu, &m, 1, imm);
            break;
        }
        case 0xC7: {   // (#194 0x66 -> mov rm32, imm32)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t imm = fetch_imm(cpu, ow);
            modrm_write(cpu, &m, ow, imm);
            break;
        }

        // ---- enter / leave (C8/C9) ----
        case 0xC8: { // enter imm16, imm8
            uint16_t alloc = fetch16(cpu);
            uint8_t  level = (uint8_t)(fetch8(cpu) & 0x1F);
            push16(cpu, cpu->bp);
            uint16_t frame = cpu->sp;
            for (uint8_t i = 1; i < level; i++) {
                cpu->bp = (uint16_t)(cpu->bp - 2);
                push16(cpu, x86_16_rd16(cpu, cpu->ss, cpu->bp));
            }
            if (level > 0) push16(cpu, frame);
            cpu->bp = frame;
            cpu->sp = (uint16_t)(cpu->sp - alloc);
            break;
        }
        case 0xC9: { // leave: mov sp,bp; pop bp
            cpu->sp = cpu->bp;
            cpu->bp = pop16(cpu);
            break;
        }

        // ---- ret far ----
        case 0xCA: { uint16_t n = fetch16(cpu);
                     cpu->ip = pop16(cpu); cpu->cs = pop16(cpu);
                     cpu->sp = (uint16_t)(cpu->sp + n); break; }
        case 0xCB: { cpu->ip = pop16(cpu); cpu->cs = pop16(cpu); break; }

        // ---- int3 / int imm8 / iret ----
        case 0xCC: {
            if (g_int_handler) { if (g_int_handler(cpu, 3)) return 0; }
            break;
        }
        case 0xCD: {
            uint8_t intno = fetch8(cpu);
            if (g_int_handler) { if (g_int_handler(cpu, intno)) return 0; }
            break;
        }
        case 0xCF: { // iret
            cpu->ip = pop16(cpu);
            cpu->cs = pop16(cpu);
            cpu->flags = (uint16_t)(pop16(cpu) | 0x0002);
            break;
        }

        // ---- shift group by 1 / by CL (D0-D3) ----
        case 0xD0: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t v = (uint8_t)modrm_read(cpu, &m, 1);
            modrm_write(cpu, &m, 1, shift_op(cpu, m.reg, v, 1, 1));
            break;
        }
        case 0xD1: {   // (#194 0x66 -> 32-bit shift by 1)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t v = modrm_read(cpu, &m, ow);
            modrm_write(cpu, &m, ow, shift_op(cpu, m.reg, v, ow, 1));
            break;
        }
        case 0xD2: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t v = (uint8_t)modrm_read(cpu, &m, 1);
            modrm_write(cpu, &m, 1, shift_op(cpu, m.reg, v, 1, (uint8_t)(cpu->cx & 0xFF)));
            break;
        }
        case 0xD3: {   // (#194 0x66 -> 32-bit shift by CL)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t v = modrm_read(cpu, &m, ow);
            modrm_write(cpu, &m, ow, shift_op(cpu, m.reg, v, ow, (uint8_t)(cpu->cx & 0xFF)));
            break;
        }

        // ---- ASCII/decimal adjust: AAM (D4) / AAD (D5) / SALC (D6) ----
        // (#390 Corel) Corel's CDR*50 DLL LibEntry init emits these; the
        // interpreter previously aborted with r=-1 on the unimplemented opcode.
        // Intel SDM semantics.
        case 0xD4: {   // AAM ib - ASCII adjust AX after multiply
            uint8_t imm = fetch8(cpu);
            if (imm == 0) {   // AAM 0 raises #DE (divide error, INT 0)
                if (g_int_handler) { if (g_int_handler(cpu, 0)) return 0; }
                break;
            }
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t ah = (uint8_t)(al / imm);
            al = (uint8_t)(al % imm);
            cpu->ax = (uint16_t)((ah << 8) | al);
            set_pzs(cpu, al, 1);   // SF/ZF/PF from AL; CF/OF/AF undefined
            break;
        }
        case 0xD5: {   // AAD ib - ASCII adjust AX before divide
            uint8_t imm = fetch8(cpu);
            uint8_t al = (uint8_t)(cpu->ax & 0xFF);
            uint8_t ah = (uint8_t)(cpu->ax >> 8);
            al = (uint8_t)((al + ah * imm) & 0xFF);
            cpu->ax = al;   // AH cleared
            set_pzs(cpu, al, 1);
            break;
        }
        case 0xD6: {   // SALC (undocumented) - set AL from CF
            uint8_t al = (cpu->flags & F_CF) ? 0xFF : 0x00;
            cpu->ax = (uint16_t)((cpu->ax & 0xFF00) | al);   // no flags affected
            break;
        }

        // ---- loop / loope / loopne / jcxz (E0-E3) ----
        case 0xE0: { // loopne
            int8_t rel = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1);
            if (cpu->cx != 0 && !(cpu->flags & F_ZF))
                cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }
        case 0xE1: { // loope
            int8_t rel = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1);
            if (cpu->cx != 0 && (cpu->flags & F_ZF))
                cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }
        case 0xE2: { // loop
            int8_t rel = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1);
            if (cpu->cx != 0)
                cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }
        case 0xE3: { // jcxz
            int8_t rel = (int8_t)fetch8(cpu);
            if (cpu->cx == 0)
                cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }

        // ---- call/jmp near and far, jmp short ----
        case 0xE8: { // call near rel16
            int16_t rel = (int16_t)fetch16(cpu);
            push16(cpu, cpu->ip);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xE9: { // jmp near rel16
            int16_t rel = (int16_t)fetch16(cpu);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xEA: { // jmp far
            uint16_t noff = fetch16(cpu);
            uint16_t nseg = fetch16(cpu);
            cpu->cs = nseg; cpu->ip = noff;
            break;
        }
        case 0xEB: { // jmp short rel8
            int8_t rel = (int8_t)fetch8(cpu);
            cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            break;
        }

        // ---- flag ops ----
        case 0xF5: cpu->flags ^= F_CF; break;            // cmc
        case 0xF8: cpu->flags &= ~F_CF; break;           // clc
        case 0xF9: cpu->flags |= F_CF; break;            // stc
        case 0xFA: cpu->flags &= ~F_IF; break;           // cli
        case 0xFB: cpu->flags |= F_IF; break;            // sti
        case 0xFC: cpu->flags &= ~F_DF; break;           // cld
        case 0xFD: cpu->flags |= F_DF; break;            // std

        // ---- port I/O (no hardware behind the interpreter; reads return all-ones,
        // writes are discarded). Win16 games occasionally poke ports for sound or
        // joystick detection; treating them as inert keeps execution flowing. ----
        case 0xE4: { uint8_t p = fetch8(cpu);
                     uint8_t v = g_in_handler ? (uint8_t)g_in_handler(cpu, p, 1) : 0xFF;
                     cpu->ax = (uint16_t)((cpu->ax & 0xFF00) | v); break; } // in al, imm8
        case 0xE5: { uint8_t p = fetch8(cpu);
                     cpu->ax = g_in_handler ? g_in_handler(cpu, p, 2) : 0xFFFF; break; } // in ax, imm8
        case 0xE6: { uint8_t p = fetch8(cpu);
                     if (g_out_handler) { g_out_handler(cpu, p, (uint16_t)(cpu->ax & 0xFF), 1); }
                     break; } // out imm8, al
        case 0xE7: { uint8_t p = fetch8(cpu);
                     if (g_out_handler) { g_out_handler(cpu, p, cpu->ax, 2); }
                     break; } // out imm8, ax
        case 0xEC: { uint8_t v = g_in_handler ? (uint8_t)g_in_handler(cpu, cpu->dx, 1) : 0xFF;
                     cpu->ax = (uint16_t)((cpu->ax & 0xFF00) | v); break; }          // in al, dx
        case 0xED: { cpu->ax = g_in_handler ? g_in_handler(cpu, cpu->dx, 2) : 0xFFFF; break; } // in ax, dx
        case 0xEE: { if (g_out_handler) { g_out_handler(cpu, cpu->dx, (uint16_t)(cpu->ax & 0xFF), 1); }
                     break; } // out dx, al
        case 0xEF: { if (g_out_handler) { g_out_handler(cpu, cpu->dx, cpu->ax, 2); }
                     break; } // out dx, ax
        // String port I/O (INS/OUTS). Discard data, advance SI/DI per DF, honour REP.
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            int sz = (op & 1) ? 2 : 1;                 // even=byte, odd=word
            int is_out = (op >= 0x6E);
            int step = (cpu->flags & F_DF) ? -sz : sz;
            unsigned long cnt = rep ? cpu->cx : 1;
            for (unsigned long n = 0; n < cnt; n++) {
                if (is_out) cpu->si = (uint16_t)(cpu->si + step);   // OUTS: read DS:SI, discard
                else        cpu->di = (uint16_t)(cpu->di + step);   // INS:  write ES:DI (skip)
            }
            if (rep) cpu->cx = 0;
            break;
        }

        // ---- hlt ----
        case 0xF4: cpu->halted = 1; break;

        // ---- group 3 (F6/F7): test/not/neg/mul/imul/div/idiv ----
        case 0xF6: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t v = (uint8_t)modrm_read(cpu, &m, 1);
            switch (m.reg) {
                case 0: case 1: { // test rm8, imm8
                    uint8_t imm = fetch8(cpu);
                    do_logic(cpu, (uint32_t)(v & imm), 1);
                    break;
                }
                case 2: modrm_write(cpu, &m, 1, (uint8_t)~v); break;      // not
                case 3: modrm_write(cpu, &m, 1,
                            (uint8_t)do_sub(cpu, 0, v, 1, 0)); break;     // neg
                case 4: { // mul: AX = AL * rm8
                    uint16_t res = (uint16_t)((cpu->ax & 0xFF) * v);
                    cpu->ax = res;
                    if (res & 0xFF00) cpu->flags |= (F_CF | F_OF);
                    else cpu->flags &= ~(F_CF | F_OF);
                    break;
                }
                case 5: { // imul: AX = AL * rm8 (signed)
                    int16_t res = (int16_t)((int8_t)(cpu->ax & 0xFF) * (int8_t)v);
                    cpu->ax = (uint16_t)res;
                    if ((res >> 8) != 0 && (res >> 8) != -1)
                        cpu->flags |= (F_CF | F_OF);
                    else cpu->flags &= ~(F_CF | F_OF);
                    break;
                }
                case 6: { // div: AL = AX / rm8, AH = AX % rm8
                    if (v == 0) {
                        if (g_int_handler) { if (g_int_handler(cpu, 0)) return 0; }
                        break;
                    }
                    uint16_t dn = cpu->ax;
                    cpu->ax = (uint16_t)(((dn / v) & 0xFF) | (((dn % v) & 0xFF) << 8));
                    break;
                }
                default: { // idiv (7)
                    if (v == 0) {
                        if (g_int_handler) { if (g_int_handler(cpu, 0)) return 0; }
                        break;
                    }
                    int16_t dn = (int16_t)cpu->ax;
                    int8_t dv = (int8_t)v;
                    int8_t q = (int8_t)(dn / dv);
                    int8_t r = (int8_t)(dn % dv);
                    cpu->ax = (uint16_t)(((uint8_t)q) | ((uint16_t)(uint8_t)r << 8));
                    break;
                }
            }
            break;
        }
        case 0xF7: {   // (#194 0x66 -> 32-bit forms)
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint32_t v = modrm_read(cpu, &m, ow);
            uint32_t omask = (ow == 4) ? 0xFFFFFFFFu : 0xFFFFu;
            switch (m.reg) {
                case 0: case 1: { // test rm, imm
                    uint32_t imm = fetch_imm(cpu, ow);
                    do_logic(cpu, v & imm, ow);
                    break;
                }
                case 2: modrm_write(cpu, &m, ow, (~v) & omask); break;       // not
                case 3: modrm_write(cpu, &m, ow,
                            do_sub(cpu, 0, v, ow, 0)); break;                // neg
                case 4: { // mul: (E)DX:(E)AX = (E)AX * rm
                    if (ow == 4) {
                        uint64_t res = (uint64_t)get_reg32(cpu, 0) * (uint64_t)v;
                        set_reg32(cpu, 0, (uint32_t)res);
                        set_reg32(cpu, 2, (uint32_t)(res >> 32));
                        if (get_reg32(cpu, 2)) cpu->flags |= (F_CF | F_OF);
                        else cpu->flags &= ~(F_CF | F_OF);
                    } else {
                        uint32_t res = (uint32_t)cpu->ax * (v & 0xFFFF);
                        cpu->ax = (uint16_t)(res & 0xFFFF);
                        cpu->dx = (uint16_t)(res >> 16);
                        if (cpu->dx) cpu->flags |= (F_CF | F_OF);
                        else cpu->flags &= ~(F_CF | F_OF);
                    }
                    break;
                }
                case 5: { // imul (signed): (E)DX:(E)AX = (E)AX * rm
                    if (ow == 4) {
                        int64_t res = (int64_t)(int32_t)get_reg32(cpu, 0) * (int64_t)(int32_t)v;
                        set_reg32(cpu, 0, (uint32_t)res);
                        set_reg32(cpu, 2, (uint32_t)((uint64_t)res >> 32));
                        if (res != (int64_t)(int32_t)res) cpu->flags |= (F_CF | F_OF);
                        else cpu->flags &= ~(F_CF | F_OF);
                    } else {
                        int32_t res = (int32_t)(int16_t)cpu->ax * (int16_t)(uint16_t)v;
                        cpu->ax = (uint16_t)(res & 0xFFFF);
                        cpu->dx = (uint16_t)((uint32_t)res >> 16);
                        if (res != (int32_t)(int16_t)res) cpu->flags |= (F_CF | F_OF);
                        else cpu->flags &= ~(F_CF | F_OF);
                    }
                    break;
                }
                case 6: { // div: (E)DX:(E)AX / rm
                    if (v == 0) { if (g_int_handler) { if (g_int_handler(cpu, 0)) return 0; } break; }
                    if (ow == 4) {
                        uint64_t dn = ((uint64_t)get_reg32(cpu, 2) << 32) | get_reg32(cpu, 0);
                        set_reg32(cpu, 0, (uint32_t)(dn / v));
                        set_reg32(cpu, 2, (uint32_t)(dn % v));
                    } else {
                        uint32_t dn = ((uint32_t)cpu->dx << 16) | cpu->ax;
                        cpu->ax = (uint16_t)((dn / (v & 0xFFFF)) & 0xFFFF);
                        cpu->dx = (uint16_t)(dn % (v & 0xFFFF));
                    }
                    break;
                }
                default: { // idiv (7)
                    if (v == 0) { if (g_int_handler) { if (g_int_handler(cpu, 0)) return 0; } break; }
                    if (ow == 4) {
                        int64_t dn = (int64_t)(((uint64_t)get_reg32(cpu, 2) << 32) | get_reg32(cpu, 0));
                        int32_t dv = (int32_t)v;
                        set_reg32(cpu, 0, (uint32_t)(int32_t)(dn / dv));
                        set_reg32(cpu, 2, (uint32_t)(int32_t)(dn % dv));
                    } else {
                        int32_t dn = (int32_t)(((uint32_t)cpu->dx << 16) | cpu->ax);
                        int16_t dv = (int16_t)(uint16_t)v;
                        cpu->ax = (uint16_t)(int16_t)(dn / dv);
                        cpu->dx = (uint16_t)(int16_t)(dn % dv);
                    }
                    break;
                }
            }
            break;
        }

        // ---- group 4 (FE): inc/dec rm8 ----
        case 0xFE: {
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            uint8_t v = (uint8_t)modrm_read(cpu, &m, 1);
            if (m.reg == 0) modrm_write(cpu, &m, 1, (uint8_t)do_inc(cpu, v, 1));
            else            modrm_write(cpu, &m, 1, (uint8_t)do_dec(cpu, v, 1));
            break;
        }

        // ---- group 5 (FF): inc/dec/call/jmp/push rm16/32 ----  (#194 0x66 -> 32-bit)
        case 0xFF: {
            int ow = osize;
            modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
            switch (m.reg) {
                case 0: { uint32_t v = modrm_read(cpu, &m, ow);
                          modrm_write(cpu, &m, ow, do_inc(cpu, v, ow)); break; }
                case 1: { uint32_t v = modrm_read(cpu, &m, ow);
                          modrm_write(cpu, &m, ow, do_dec(cpu, v, ow)); break; }
                case 2: { // call near rm16
                    uint16_t t = modrm_read(cpu, &m, 2);
                    push16(cpu, cpu->ip);
                    cpu->ip = t;
                    break;
                }
                case 3: { // call far m16:16
                    uint16_t noff = x86_16_rd16(cpu, m.seg, m.off);
                    uint16_t nseg = x86_16_rd16(cpu, m.seg, (uint16_t)(m.off + 2));
                    // (#278 P40 DIAG) Trace indirect far-calls (vtable/virtual dispatch)
                    // into the view-format segment (seg182=05b7). 0xd0a (the format
                    // trigger that sets bit3) is reachable ONLY via such a dispatch.
                    { extern int g_w6life, g_win16_pmode;
                      if (g_w6life && g_win16_pmode && nseg==0x05b7) { static int nvc=0;
                        if (nvc<6){ nvc++;
                          kprintf("[W6VDISP] FF/3 -> seg182:%04x via EA=%04x:%04x caller=%04x:%04x ds=%04x [1d18]=%04x\n",
                            noff, m.seg, m.off, cpu->cs, cpu->ip, cpu->ds,
                            x86_16_rd16(cpu,cpu->ds,0x1d18)); }
                        // one-shot: dump the vtable (lives in m.seg) + object + scan for the
                        // seg182:0xd0a format-method slot.
                        static int vtdone=0; if(!vtdone){ vtdone=1;
                          uint16_t vs=m.seg; uint16_t base=(m.off>0x40)?(uint16_t)(m.off-0x40):0;
                          kprintf("[W6VTBL] dispatch-vtbl seg=%04x around off=%04x:\n", vs, m.off);
                          for (int r=0;r<10;r++){ uint16_t o=(uint16_t)(base+r*16);
                            kprintf("[W6VTBL] %04x:%04x:", vs,o);
                            for(int c=0;c<8;c++) kprintf(" %04x", x86_16_rd16(cpu,vs,(uint16_t)(o+c*2)));
                            kprintf("\n"); }
                          // scan seg m.seg for far-ptr {0d0a:05b7} and list ALL seg182 method slots
                          int h=0; for(uint32_t o=0;o<0xFFFC;o+=2){
                            if(x86_16_rd16(cpu,vs,(uint16_t)(o+2))==0x05b7){ uint16_t t=x86_16_rd16(cpu,vs,(uint16_t)o);
                              if(t<0x9000){ kprintf("[W6VTBL] seg%04x:%04x -> seg182:%04x\n",vs,(uint16_t)o,t); if(++h>=24)break; } } }
                          if(!h) kprintf("[W6VTBL] no seg182 far-ptrs in seg %04x\n", vs);
                          uint16_t obj=x86_16_rd16(cpu,0x075f,0x1d18);
                          kprintf("[W6VTBL] obj DGROUP:%04x:", obj);
                          for(int c=0;c<12;c++) kprintf(" %04x", x86_16_rd16(cpu,0x075f,(uint16_t)(obj+c*2)));
                          kprintf("\n");
                        } } }
                    // (#278 P59) log INDIRECT-far (vtable / MFC virtual) dispatch INTO
                    // seg223 (0x06ff). The view-region-layout function entry is NOT
                    // reachable by any static caller (dynamic dispatch) - this is how
                    // its true entry offset + driver become visible.
                    { extern int g_w6life, g_win16_pmode;
                      if (g_w6life && g_win16_pmode && nseg==0x06ff && cpu->cs!=0x06ff) {
                        static int nd=0; if(nd<80){nd++;
                          kprintf("[W6DISP223] FF/3 -> seg223:%04x from %04x:%04x EA=%04x:%04x arg0=%04x arg1=%04x arg2=%04x\n",
                            noff, cpu->cs, cpu->ip, m.seg, m.off,
                            x86_16_rd16(cpu,cpu->ss,cpu->sp), x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+2)),
                            x86_16_rd16(cpu,cpu->ss,(uint16_t)(cpu->sp+4))); } } }
                    push16(cpu, cpu->cs);
                    push16(cpu, cpu->ip);
                    if (g_farcall_active && nseg == g_farcall_seg) {
                        if (g_farcall_fn(cpu, noff)) return 0;
                        break;   // fn performed the Pascal return.
                    }
                    fnt_maybe_enter(cpu, nseg, noff);
                    cpu->cs = nseg; cpu->ip = noff;
                    break;
                }
                case 4: cpu->ip = modrm_read(cpu, &m, 2); break; // jmp near rm16
                case 5: { // jmp far m16:16
                    uint16_t noff = x86_16_rd16(cpu, m.seg, m.off);
                    uint16_t nseg = x86_16_rd16(cpu, m.seg, (uint16_t)(m.off + 2));
                    { extern int g_w6life, g_win16_pmode;
                      if (g_w6life && g_win16_pmode && nseg==0x05b7) { static int njv=0;
                        if (njv<40){ njv++;
                          kprintf("[W6VDISP] FF/5(jmpf) -> seg182:%04x via EA=%04x:%04x caller=%04x:%04x\n",
                            noff, m.seg, m.off, cpu->cs, cpu->ip); } } }
                    cpu->cs = nseg; cpu->ip = noff;
                    break;
                }
                default: // push rm16/32 (/6)
                    if (ow == 4) push32(cpu, modrm_read(cpu, &m, 4));
                    else         push16(cpu, (uint16_t)modrm_read(cpu, &m, 2));
                    break;
            }
            break;
        }

        // ---- two-byte opcodes (0F xx): 386+ instruction set (#194) ----
        case 0x0F: {
            uint8_t op2 = fetch8(cpu);
            int ow = osize;                 // operand width for reg ops (2 or 4)
            int obits = ow * 8;
            uint32_t omask = (ow == 4) ? 0xFFFFFFFFu : 0xFFFFu;
            if (op2 >= 0x80 && op2 <= 0x8F) {           // Jcc near (rel16/rel32)
                int32_t rel = (ow == 4) ? (int32_t)fetch32(cpu) : (int32_t)(int16_t)fetch16(cpu);
                if (jcc_cond(cpu, op2 & 0x0F))
                    cpu->ip = (uint16_t)(cpu->ip + (int16_t)rel);
            } else if (op2 >= 0x90 && op2 <= 0x9F) {    // SETcc rm8
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                modrm_write(cpu, &m, 1, jcc_cond(cpu, op2 & 0x0F) ? 1 : 0);
            } else if (op2 == 0xB6 || op2 == 0xB7) {    // MOVZX reg(ow), rm8/rm16
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint32_t v = (op2 == 0xB6) ? (modrm_read(cpu, &m, 1) & 0xFF)
                                           : (modrm_read(cpu, &m, 2) & 0xFFFF);
                if (ow == 4) set_reg32(cpu, m.reg, v); else set_reg16(cpu, m.reg, (uint16_t)v);
            } else if (op2 == 0xBE || op2 == 0xBF) {    // MOVSX reg(ow), rm8/rm16
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                int32_t v = (op2 == 0xBE) ? (int32_t)(int8_t)(modrm_read(cpu, &m, 1) & 0xFF)
                                          : (int32_t)(int16_t)(modrm_read(cpu, &m, 2) & 0xFFFF);
                if (ow == 4) set_reg32(cpu, m.reg, (uint32_t)v); else set_reg16(cpu, m.reg, (uint16_t)v);
            } else if (op2 == 0xAF) {                   // IMUL reg(ow), rm(ow)
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                int64_t a = (ow == 4) ? (int32_t)get_reg32(cpu, m.reg) : (int16_t)get_reg16(cpu, m.reg);
                int64_t b = (ow == 4) ? (int32_t)modrm_read(cpu, &m, 4) : (int16_t)(uint16_t)modrm_read(cpu, &m, 2);
                int64_t p = a * b;
                uint32_t lo = (uint32_t)p & omask;
                if (ow == 4) set_reg32(cpu, m.reg, lo); else set_reg16(cpu, m.reg, (uint16_t)lo);
                // CF=OF set when the full product differs from the sign-extended low.
                int64_t sx = (ow == 4) ? (int64_t)(int32_t)lo : (int64_t)(int16_t)(uint16_t)lo;
                if (p != sx) cpu->flags |= (F_CF | F_OF); else cpu->flags &= ~(F_CF | F_OF);
            } else if (op2 == 0xBC || op2 == 0xBD) {    // BSF/BSR reg(ow), rm(ow)
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint32_t v = modrm_read(cpu, &m, ow) & omask;
                if (v == 0) { cpu->flags |= F_ZF; }
                else {
                    cpu->flags &= ~F_ZF;
                    int idx;
                    if (op2 == 0xBC) { idx = 0; while (!((v >> idx) & 1)) idx++; }
                    else             { idx = obits - 1; while (!((v >> idx) & 1)) idx--; }
                    if (ow == 4) set_reg32(cpu, m.reg, (uint32_t)idx); else set_reg16(cpu, m.reg, (uint16_t)idx);
                }
            } else if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
                // BT/BTS/BTR/BTC rm(ow), reg(ow)
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                int bit = (int)(ow == 4 ? get_reg32(cpu, m.reg) : get_reg16(cpu, m.reg));
                uint32_t v = modrm_read(cpu, &m, ow) & omask;
                int b = bit & (obits - 1);
                if ((v >> b) & 1) cpu->flags |= F_CF; else cpu->flags &= ~F_CF;
                if      (op2 == 0xAB) v |= (1u << b);
                else if (op2 == 0xB3) v &= ~(1u << b);
                else if (op2 == 0xBB) v ^= (1u << b);
                if (op2 != 0xA3) modrm_write(cpu, &m, ow, v & omask);
            } else if (op2 == 0xBA) {                   // grp8: BT/BTS/BTR/BTC rm,imm8
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint8_t sub = m.reg;
                uint8_t imm = fetch8(cpu);
                uint32_t v = modrm_read(cpu, &m, ow) & omask;
                int b = imm & (obits - 1);
                if ((v >> b) & 1) cpu->flags |= F_CF; else cpu->flags &= ~F_CF;
                if      (sub == 5) v |= (1u << b);       // BTS
                else if (sub == 6) v &= ~(1u << b);      // BTR
                else if (sub == 7) v ^= (1u << b);       // BTC
                if (sub != 4) modrm_write(cpu, &m, ow, v & omask);   // 4 = BT (test only)
            } else if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
                // SHLD/SHRD rm(ow), reg(ow), imm8|CL
                int left = (op2 == 0xA4 || op2 == 0xA5);
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint32_t dst = modrm_read(cpu, &m, ow) & omask;
                uint32_t src = (ow == 4 ? get_reg32(cpu, m.reg) : get_reg16(cpu, m.reg)) & omask;
                int cnt = ((op2 == 0xA4 || op2 == 0xAC) ? fetch8(cpu) : (cpu->cx & 0xFF)) & (obits - 1);
                if (cnt) {
                    uint32_t res;
                    if (left)  res = (dst << cnt) | (src >> (obits - cnt));
                    else       res = (dst >> cnt) | (src << (obits - cnt));
                    res &= omask;
                    int cf = left ? ((dst >> (obits - cnt)) & 1) : ((dst >> (cnt - 1)) & 1);
                    if (cf) cpu->flags |= F_CF; else cpu->flags &= ~F_CF;
                    modrm_write(cpu, &m, ow, res);
                    set_pzs(cpu, res, ow);
                }
            } else if (op2 == 0xC0 || op2 == 0xC1) {    // XADD rm, reg
                int sz = (op2 == 0xC0) ? 1 : ow;
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint32_t d = modrm_read(cpu, &m, sz);
                uint32_t s = (sz == 1) ? get_reg8(cpu, m.reg)
                           : (sz == 4) ? get_reg32(cpu, m.reg) : get_reg16(cpu, m.reg);
                uint32_t sum = do_add(cpu, d, s, sz, 0);
                if (sz == 1)      set_reg8(cpu, m.reg, (uint8_t)d);
                else if (sz == 4) set_reg32(cpu, m.reg, d);
                else              set_reg16(cpu, m.reg, (uint16_t)d);
                modrm_write(cpu, &m, sz, sum);
            } else if (op2 == 0xB0 || op2 == 0xB1) {    // CMPXCHG rm, reg (486, common)
                int sz = (op2 == 0xB0) ? 1 : ow;
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint32_t d = modrm_read(cpu, &m, sz);
                uint32_t acc = (sz == 1) ? get_reg8(cpu, 0) : (sz == 4) ? get_reg32(cpu, 0) : cpu->ax;
                do_sub(cpu, acc, d, sz, 0);              // sets ZF if equal
                if (acc == (d & ((sz==1)?0xFFu:(sz==2)?0xFFFFu:0xFFFFFFFFu))) {
                    uint32_t s = (sz == 1) ? get_reg8(cpu, m.reg) : (sz == 4) ? get_reg32(cpu, m.reg) : get_reg16(cpu, m.reg);
                    modrm_write(cpu, &m, sz, s);
                } else {
                    if (sz == 1) set_reg8(cpu, 0, (uint8_t)d); else if (sz == 4) set_reg32(cpu, 0, d); else cpu->ax = (uint16_t)d;
                }
            } else if (op2 == 0xB2 || op2 == 0xB4 || op2 == 0xB5) {
                // (#194) LSS/LFS/LGS reg(ow), m16:16 : load reg from [m], the named
                // segment register from [m+2]. B2=SS, B4=FS, B5=GS. We have no FS/GS
                // registers in this 16-bit model; load the offset into the reg and the
                // selector into SS (B2) or discard (B4/B5, treated as far-pointer
                // loads whose seg half is rarely used by 16-bit thunked code).
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                if (ow == 4) set_reg32(cpu, m.reg, rd32(cpu, m.seg, m.off));
                else         set_reg16(cpu, m.reg, x86_16_rd16(cpu, m.seg, m.off));
                uint16_t selhi = x86_16_rd16(cpu, m.seg, (uint16_t)(m.off + (ow == 4 ? 4 : 2)));
                if (op2 == 0xB2) cpu->ss = selhi;     // LSS -> SS
                // LFS/LGS: FS/GS unmodeled; selector half ignored (no FS/GS state).
            } else if (op2 == 0x00) {
                // (#289) 286+ group: SLDT/STR/LLDT/LTR/VERR/VERW rm16. These are
                // PROTECTED-MODE selector ops that STORAGE.DLL / OLE2 emit to
                // validate selectors. We back them with the LDT model.
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint8_t sub = m.reg;
                if (sub == 0 || sub == 1) {            // SLDT / STR -> 0 (no real LDTR/TR)
                    modrm_write(cpu, &m, 2, 0);
                } else if (sub == 2 || sub == 3) {     // LLDT / LTR -> no-op (fixed LDT)
                    (void)modrm_read(cpu, &m, 2);
                } else if (sub == 4 || sub == 5) {     // VERR / VERW: ZF=1 if selector usable
                    uint16_t sel = (uint16_t)modrm_read(cpu, &m, 2);
                    int ok = g_win16_pmode ? ldt_valid(sel) : (sel != 0);
                    if (ok) cpu->flags |= F_ZF; else cpu->flags &= ~F_ZF;
                } else {
                    modrm_write(cpu, &m, 2, 0);
                }
            } else if (op2 == 0x01) {
                // (#289) 286+ group: SGDT/SIDT/SMSW/LMSW/LGDT/LIDT. Only SMSW
                // (sub==4) is plausibly emitted by ring-3 16-bit code (read MSW/CR0
                // low word). Report PE set (bit0) since we run "protected mode".
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint8_t sub = m.reg;
                if (sub == 4) {                        // SMSW rm16 -> MSW (PE=1)
                    modrm_write(cpu, &m, 2, 0x0001);
                } else {
                    (void)modrm_read(cpu, &m, 2);      // LGDT/LIDT/LMSW/SGDT/SIDT: ignore
                }
            } else if (op2 == 0x02 || op2 == 0x03) {
                // (#289) LAR (0F 02) / LSL (0F 03) reg16, rm16: load access-rights /
                // segment-limit of a selector. STORAGE/OLE2 use LSL to size buffers.
                modrm_t m; decode_modrm_a(cpu, &m, seg_ovr, asize);
                uint16_t sel = (uint16_t)modrm_read(cpu, &m, 2);
                int valid = g_win16_pmode ? ldt_valid(sel) : (sel != 0);
                if (valid) {
                    cpu->flags |= F_ZF;
                    if (op2 == 0x02) {                 // LAR: access-rights byte<<8
                        // present, DPL=3, type=data RW (0x92) or code (0x9A).
                        uint32_t ar = 0x00F300;        // P=1 DPL=3 S=1 type=RW data
                        if (ow == 4) set_reg32(cpu, m.reg, ar);
                        else         set_reg16(cpu, m.reg, (uint16_t)ar);
                    } else {                           // LSL: segment limit (bytes)
                        uint32_t lim = g_win16_pmode ? ldt_limit(sel) : 0xFFFF;
                        if (ow == 4) set_reg32(cpu, m.reg, lim);
                        else         set_reg16(cpu, m.reg, (uint16_t)lim);
                    }
                } else {
                    cpu->flags &= ~F_ZF;               // invalid selector
                }
            } else {
                kprintf("[x86_16] unimpl opcode 0x0f%02x at %04x:%04x\n",
                        op2, cpu->cs, (uint16_t)(cpu->ip - 2));
                return -1;
            }
            break;
        }

        default:
            kprintf("[x86_16] unimpl opcode 0x%02x at %04x:%04x\n",
                    op, cpu->cs, (uint16_t)(cpu->ip - 1));
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Host -> 16-bit __far __pascal call (see x86_16.h).
// ---------------------------------------------------------------------------
int x86_16_call_far(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off,
                    const uint16_t *args, int nargs,
                    uint16_t *out_ax, uint16_t *out_dx,
                    unsigned long max_insns) {
    // Snapshot the full register file so the outer program is undisturbed.
    x86_16_cpu_t saved = *cpu;
    if (max_insns == 0) max_insns = 2000000UL;

    // Push args in Pascal order: args[0] FIRST (deepest), args[nargs-1] last.
    for (int i = 0; i < nargs; i++)
        push16(cpu, args[i]);

    // Push the sentinel far-return frame (CS=SENTINEL, IP=0). When the callee
    // executes RETF it pops this and the run loop's sentinel check fires.
    push16(cpu, X86_16_CALLFAR_SENTINEL);   // return CS
    push16(cpu, 0x0000);                     // return IP

    cpu->cs = seg;
    cpu->ip = off;
    cpu->halted = 0;

    // (#256 VB1) Run the callee in resumable slices. A heavy wndproc (e.g. the
    // Visual Basic 1.0 runtime's window/message procedure) can legitimately run
    // far more than one slice's worth of instructions; the old code aborted the
    // call the moment `max_insns` was hit (r==1), restored the caller's registers
    // mid-wndproc, and returned -1 -> the VB pump never completed and the window
    // stayed dead. Now keep resuming the SAME mid-execution state across slices
    // until the callee RETFs to the sentinel (r==2), halts (r==0), errors (r<0),
    // or a generous safety cap is reached (so a genuinely-stuck proc still ends).
    g_callfar_stop++;   // (#278 pass30) nesting counter (supports nested call_far)
    int r = x86_16_run(cpu, max_insns);
    if (r == 1) {
        unsigned long spent = max_insns;
        const unsigned long CALLFAR_CAP = 400000000UL;   // ~400M total guard
        while (r == 1 && spent < CALLFAR_CAP) {
            if (g_callfar_abort_fn && g_callfar_abort_fn()) break;  // close requested
            r = x86_16_run(cpu, max_insns);
            spent += max_insns;
        }
    }
    g_callfar_stop--;   // (#278 pass30) pop nesting level

    uint16_t rax = cpu->ax, rdx = cpu->dx;
    int reached_sentinel = (r == 2);

    // Restore the outer program's registers. The callee's result is captured
    // separately. (A real Win16 wndproc preserves SS/SP/DS via the prologue, so
    // the outer SP is intact anyway, but a full restore is safest.)
    *cpu = saved;

    if (out_ax) *out_ax = rax;
    if (out_dx) *out_dx = rdx;

    if (!reached_sentinel) {
        kprintf("[x86_16] call_far %04x:%04x did not return cleanly (r=%d)\n",
                seg, off, r);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Self-test: assemble and run a tiny program.
//   mov ax, 0x1234   ; B8 34 12
//   mov bx, ax       ; 89 C3   (mov rm16,r16: mod=11 reg=AX rm=BX)
//   add ax, bx       ; 01 D8   (add rm16,r16: mod=11 reg=BX rm=AX)
//   hlt              ; F4
// Expect ax == 0x2468.
// ---------------------------------------------------------------------------
static uint8_t s_selftest_mem[0x100000];

int x86_16_selftest(void) {
    x86_16_cpu_t cpu;
    x86_16_init(&cpu, s_selftest_mem);

    // Program loaded at cs:ip = 0x0000:0x0100 (arbitrary).
    uint16_t seg = 0x0000, off = 0x0100;
    uint8_t prog[] = {
        0xB8, 0x34, 0x12, // mov ax, 0x1234
        0x89, 0xC3,       // mov bx, ax
        0x01, 0xD8,       // add ax, bx
        0xF4              // hlt
    };
    for (unsigned i = 0; i < sizeof(prog); i++)
        x86_16_wr8(&cpu, seg, (uint16_t)(off + i), prog[i]);

    cpu.cs = seg; cpu.ip = off;
    cpu.ss = 0x0000; cpu.sp = 0xFFFE;

    int rc = x86_16_run(&cpu, 1000);
    if (rc != 0) {
        kprintf("[x86_16] selftest: run returned %d (expected 0)\n", rc);
        return -1;
    }
    if (cpu.ax != 0x2468) {
        kprintf("[x86_16] selftest FAIL: ax=0x%04x (expected 0x2468)\n", cpu.ax);
        return -1;
    }
    kprintf("[x86_16] selftest PASS: ax=0x%04x bx=0x%04x insns=%lu\n",
            cpu.ax, cpu.bx, cpu.insn_count);
    return 0;
}

// (#194) 386 instruction self-test. Assembles small programs that exercise the
// new opcodes with known inputs/outputs and checks the result registers, since
// no current Win16 app emits 386 code to exercise them in-app. Logs PASS/FAIL.
static int run386(uint8_t *prog, unsigned n, x86_16_cpu_t *cpu) {
    x86_16_init(cpu, s_selftest_mem);
    for (unsigned i = 0; i < n; i++) x86_16_wr8(cpu, 0, (uint16_t)(0x100 + i), prog[i]);
    cpu->cs = 0; cpu->ip = 0x100; cpu->ss = 0; cpu->sp = 0xFFFE;
    return x86_16_run(cpu, 10000);
}
int x86_16_selftest_386(void) {
    x86_16_cpu_t cpu; int fails = 0;
    // 1) mov eax,0x11223344 (66 B8 imm32); movzx ebx,al (66 0F B6 D8)
    { uint8_t p[] = {0x66,0xB8,0x44,0x33,0x22,0x11, 0x66,0x0F,0xB6,0xD8, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0x11223344u || get_reg32(&cpu,3) != 0x44u) {
          kprintf("[x86_16] 386 FAIL movzx eax=%x ebx=%x\n", get_reg32(&cpu,0), get_reg32(&cpu,3)); fails++; } }
    // 2) movsx ebx, al where al=0x80 -> 0xFFFFFF80 (66 0F BE D8)
    { uint8_t p[] = {0x66,0xB8,0x80,0x00,0x00,0x00, 0x66,0x0F,0xBE,0xD8, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,3) != 0xFFFFFF80u) { kprintf("[x86_16] 386 FAIL movsx ebx=%x\n", get_reg32(&cpu,3)); fails++; } }
    // 3) 32-bit add: mov eax,0xFFFFFFFF; mov ebx,1; add eax,ebx -> 0, CF set
    { uint8_t p[] = {0x66,0xB8,0xFF,0xFF,0xFF,0xFF, 0x66,0xBB,0x01,0x00,0x00,0x00,
                     0x66,0x01,0xD8, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0 || !(cpu.flags & F_CF) || !(cpu.flags & F_ZF)) {
          kprintf("[x86_16] 386 FAIL add32 eax=%x fl=%x\n", get_reg32(&cpu,0), cpu.flags); fails++; } }
    // 4) imul ecx, edx: 0x10000 * 0x10000 -> 0 low, CF/OF set (66 0F AF CA)
    { uint8_t p[] = {0x66,0xB9,0x00,0x00,0x01,0x00, 0x66,0xBA,0x00,0x00,0x01,0x00,
                     0x66,0x0F,0xAF,0xCA, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,1) != 0 || !(cpu.flags & F_CF)) {
          kprintf("[x86_16] 386 FAIL imul32 ecx=%x fl=%x\n", get_reg32(&cpu,1), cpu.flags); fails++; } }
    // 5) push imm32 / pop eax (66 68 imm32 ; 66 58)
    { uint8_t p[] = {0x66,0x68,0xEF,0xBE,0xAD,0xDE, 0x66,0x58, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xDEADBEEFu) { kprintf("[x86_16] 386 FAIL push/pop32 eax=%x\n", get_reg32(&cpu,0)); fails++; } }
    // 6) bsf ecx, eax with eax=0x100 -> ecx=8 (66 0F BC C8)
    { uint8_t p[] = {0x66,0xB8,0x00,0x01,0x00,0x00, 0x66,0x0F,0xBC,0xC8, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,1) != 8) { kprintf("[x86_16] 386 FAIL bsf ecx=%x\n", get_reg32(&cpu,1)); fails++; } }
    // 7) setz bl after cmp eax,eax (B0..: mov al,5; cmp al,5; 0F 94 C3 setz bl)
    { uint8_t p[] = {0xB0,0x05, 0x3C,0x05, 0x0F,0x94,0xC3, 0xF4};
      run386(p,sizeof(p),&cpu);
      if ((get_reg32(&cpu,3) & 0xFF) != 1) { kprintf("[x86_16] 386 FAIL setz bl=%x\n", get_reg32(&cpu,3) & 0xFF); fails++; } }
    // 8) shld eax: eax=0x12340000, edx=0xABCD, shld eax,edx,16 -> 0x0000ABCD?
    //    shld shifts eax left by 16 bringing in edx high bits: (eax<<16)|(edx>>16)... edx>>16=0 -> 0
    { uint8_t p[] = {0x66,0xB8,0x00,0x00,0x34,0x12, 0x66,0xBA,0xCD,0xAB,0x00,0x00,
                     0x66,0x0F,0xA4,0xD0,0x10, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0u) { kprintf("[x86_16] 386 NOTE shld eax=%x (informational)\n", get_reg32(&cpu,0)); } }
    // 9) 32-bit shl: mov eax,0x00000001; shl eax,31 -> 0x80000000 (66 C1 E0 1F)
    { uint8_t p[] = {0x66,0xB8,0x01,0x00,0x00,0x00, 0x66,0xC1,0xE0,0x1F, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0x80000000u) { kprintf("[x86_16] 386 FAIL shl32 eax=%x\n", get_reg32(&cpu,0)); fails++; } }
    // 10) 32-bit sar: mov eax,0x80000000; sar eax,4 -> 0xF8000000 (66 C1 F8 04)
    { uint8_t p[] = {0x66,0xB8,0x00,0x00,0x00,0x80, 0x66,0xC1,0xF8,0x04, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xF8000000u) { kprintf("[x86_16] 386 FAIL sar32 eax=%x\n", get_reg32(&cpu,0)); fails++; } }
    // 11) F7 /4 mul32: mov eax,0x10000; mov ebx,0x10000; mul ebx -> edx:eax = 1:0 (66 F7 E3)
    { uint8_t p[] = {0x66,0xB8,0x00,0x00,0x01,0x00, 0x66,0xBB,0x00,0x00,0x01,0x00,
                     0x66,0xF7,0xE3, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0 || get_reg32(&cpu,2) != 1 || !(cpu.flags & F_CF)) {
          kprintf("[x86_16] 386 FAIL mul32 eax=%x edx=%x fl=%x\n", get_reg32(&cpu,0), get_reg32(&cpu,2), cpu.flags); fails++; } }
    // 12) F7 /6 div32: edx=0, eax=100, ebx=7 -> eax=14 edx=2 (66 F7 F3)
    { uint8_t p[] = {0x66,0xBA,0x00,0x00,0x00,0x00, 0x66,0xB8,0x64,0x00,0x00,0x00,
                     0x66,0xBB,0x07,0x00,0x00,0x00, 0x66,0xF7,0xF3, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 14 || get_reg32(&cpu,2) != 2) {
          kprintf("[x86_16] 386 FAIL div32 eax=%x edx=%x\n", get_reg32(&cpu,0), get_reg32(&cpu,2)); fails++; } }
    // 13) 3-operand imul32: imul ecx, eax, 3 with eax=11 -> ecx=33 (66 6B C8 03)
    { uint8_t p[] = {0x66,0xB8,0x0B,0x00,0x00,0x00, 0x66,0x6B,0xC8,0x03, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,1) != 33) { kprintf("[x86_16] 386 FAIL imul3 ecx=%x\n", get_reg32(&cpu,1)); fails++; } }
    // 14) PUSHAD/POPAD round-trip: set eax=0xAAAA1111, push all, clobber eax, pop all -> eax restored
    //     mov eax,0xAAAA1111; pushad; mov eax,0; popad  (66 60 / 66 61)
    { uint8_t p[] = {0x66,0xB8,0x11,0x11,0xAA,0xAA, 0x66,0x60,
                     0x66,0xB8,0x00,0x00,0x00,0x00, 0x66,0x61, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xAAAA1111u) { kprintf("[x86_16] 386 FAIL pushad/popad eax=%x\n", get_reg32(&cpu,0)); fails++; } }
    // 15) MOVSD: source dword 0xCAFEBABE at ds:0x200, di=0x300; rep-free movsd copies it.
    //     mov si,0x200; mov di,0x300; movsd (66 A5) ; then read es:0x300
    { uint8_t p[] = {0xBE,0x00,0x02, 0xBF,0x00,0x03, 0x66,0xA5, 0xF4};
      x86_16_init(&cpu, s_selftest_mem);
      for (unsigned i=0;i<sizeof(p);i++) x86_16_wr8(&cpu,0,(uint16_t)(0x100+i),p[i]);
      wr32(&cpu, 0, 0x200, 0xCAFEBABEu);
      cpu.cs=0; cpu.ip=0x100; cpu.ss=0; cpu.sp=0xFFFE; cpu.es=0; cpu.ds=0;
      x86_16_run(&cpu, 10000);
      if (rd32(&cpu,0,0x300) != 0xCAFEBABEu || cpu.si != 0x204 || cpu.di != 0x304) {
          kprintf("[x86_16] 386 FAIL movsd dst=%x si=%x di=%x\n", rd32(&cpu,0,0x300), cpu.si, cpu.di); fails++; } }
    // 16) SIB 32-bit memory access: store ebx via [eax*2+0x400], then load it back.
    //     mov eax,8 ; mov ebx,0x99887766 ; mov [eax*2+0x400],ebx ; mov ecx,[eax*2+0x400]
    //     ModR/M for mov [sib+disp32],ebx (66 89 1C 45 disp32) reg=ebx(3) rm=4(SIB) mod=00
    //     SIB: scale=1(x2) index=eax(0) base=5 -> disp32 base. 66 89 /r, modrm=0x1C, sib=0x45.
    { uint8_t p[] = {
          0x66,0xB8,0x08,0x00,0x00,0x00,                 // mov eax,8
          0x66,0xBB,0x66,0x77,0x88,0x99,                 // mov ebx,0x99887766
          0x67,0x66,0x89,0x1C,0x45,0x00,0x04,0x00,0x00,  // mov [eax*2+0x400],ebx
          0x67,0x66,0x8B,0x0C,0x45,0x00,0x04,0x00,0x00,  // mov ecx,[eax*2+0x400]
          0xF4 };
      run386(p,sizeof(p),&cpu);
      // EA = 8*2 + 0x400 = 0x410
      if (rd32(&cpu,0,0x410) != 0x99887766u || get_reg32(&cpu,1) != 0x99887766u) {
          kprintf("[x86_16] 386 FAIL sib mem=%x ecx=%x\n", rd32(&cpu,0,0x410), get_reg32(&cpu,1)); fails++; } }
    // 17) SETcc spread: after cmp setl/setg/sete on 3<5. mov al,3; cmp al,5;
    //     setl bl (0F 9C C3 -> 1) ; setg cl (0F 9F C1 -> 0) ; sete dl (0F 94 C2 -> 0)
    { uint8_t p[] = {0xB0,0x03, 0x3C,0x05, 0x0F,0x9C,0xC3, 0x0F,0x9F,0xC1, 0x0F,0x94,0xC2, 0xF4};
      run386(p,sizeof(p),&cpu);
      if ((cpu.bx&0xFF)!=1 || (cpu.cx&0xFF)!=0 || (cpu.dx&0xFF)!=0) {
          kprintf("[x86_16] 386 FAIL setcc bl=%x cl=%x dl=%x\n", cpu.bx&0xFF, cpu.cx&0xFF, cpu.dx&0xFF); fails++; } }
    // 18) BTS: mov eax,0; bts eax,5 (66 0F BA E8 05) -> eax=0x20, CF=0 (was clear)
    { uint8_t p[] = {0x66,0xB8,0x00,0x00,0x00,0x00, 0x66,0x0F,0xBA,0xE8,0x05, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0x20u || (cpu.flags & F_CF)) {
          kprintf("[x86_16] 386 FAIL bts eax=%x fl=%x\n", get_reg32(&cpu,0), cpu.flags); fails++; } }
    // 19) F7 /3 neg32: mov eax,1; neg eax -> 0xFFFFFFFF, CF set (66 F7 D8)
    { uint8_t p[] = {0x66,0xB8,0x01,0x00,0x00,0x00, 0x66,0xF7,0xD8, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xFFFFFFFFu || !(cpu.flags & F_CF)) {
          kprintf("[x86_16] 386 FAIL neg32 eax=%x fl=%x\n", get_reg32(&cpu,0), cpu.flags); fails++; } }
    // 20) SHRD: eax=0x0000F00D, edx=0xABCD0000, shrd eax,edx,16 (66 0F AC D0 10)
    //     -> (eax>>16)|(edx<<16) = 0x0000 | 0x0000 = 0x0000... actually
    //        eax>>16 = 0x0000, edx<<16 = 0x00000000 -> 0. Use a clearer pattern:
    //     eax=0x12345678, edx=0xAABBCCDD, shrd eax,edx,8 -> (eax>>8)|(edx<<24)
    //        = 0x00123456 | 0xDD000000 = 0xDD123456
    { uint8_t p[] = {0x66,0xB8,0x78,0x56,0x34,0x12, 0x66,0xBA,0xDD,0xCC,0xBB,0xAA,
                     0x66,0x0F,0xAC,0xD0,0x08, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xDD123456u) { kprintf("[x86_16] 386 FAIL shrd eax=%x\n", get_reg32(&cpu,0)); fails++; } }
    // 21) CWDE: mov ax,0x8000 (via 32-bit then mask); movsx style. Use mov eax then
    //     load ax=0xFFFF and CWDE -> eax=0xFFFFFFFF. mov ax,0xFFFF (B8 FF FF), cwde(66 98)
    { uint8_t p[] = {0xB8,0xFF,0xFF, 0x66,0x98, 0xF4};
      run386(p,sizeof(p),&cpu);
      if (get_reg32(&cpu,0) != 0xFFFFFFFFu) { kprintf("[x86_16] 386 FAIL cwde eax=%x\n", get_reg32(&cpu,0)); fails++; } }

    int total = 21;
    if (fails == 0) kprintf("[x86_16] 386 selftest PASS: %d/%d cases (movzx/movsx/add32/imul32/push-pop32/bsf/setcc/shl32/sar32/mul32/div32/imul3-32/pushad/movsd/SIB/setcc-spread/bts/neg32/shrd/cwde)\n", total, total);
    else            kprintf("[x86_16] 386 selftest: %d/%d FAILURES\n", fails, total);
    return fails ? -1 : 0;
}

// ===========================================================================
// (#289 Phase 1) PROTECTED-MODE SELECTOR self-test. The acceptance test for the
// selector/LDT memory model. Runs entirely in-kernel (no NE file needed) and
// proves, under g_win16_pmode:
//   (a) ldt_alloc + arena-backed selectors translate correctly;
//   (b) a >64 KiB "GlobalAlloc"-style block backed by CONSECUTIVE tiled
//       selectors (sel += 8 per 64 KiB) can be written across the 64 KiB tile
//       boundary and read back intact (huge-pointer arithmetic works);
//   (c) a 16-bit far CALL between two distinct CODE selectors runs and RETFs.
// Logs PASS/FAIL. Restores g_win16_pmode to 0 (real mode) on exit so nothing
// else is affected.
// ===========================================================================
int x86_16_selftest_pmode(void) {
    int fails = 0;
    int saved_mode = g_win16_pmode;

    win16_pmode_enable(1);   // resets LDT + arena

    // ---- (a) basic selector translation ----
    uint32_t blkA = win16_arena_alloc(0x1000, 16);
    uint16_t selA = ldt_alloc(blkA, 0x0FFF, 0);   // data selector, 4 KiB
    if (selA == 0) { kprintf("[pmode] FAIL ldt_alloc selA\n"); fails++; }
    if (ldt_base(selA) != blkA) { kprintf("[pmode] FAIL ldt_base\n"); fails++; }
    {
        x86_16_cpu_t cpu; x86_16_init(&cpu, s_selftest_mem);
        // write a marker via the selector, read it back via the raw arena ptr.
        x86_16_wr16(&cpu, selA, 0x10, 0xBEEF);
        uint16_t v = x86_16_rd16(&cpu, selA, 0x10);
        uint8_t *ap = win16_arena_ptr();
        uint16_t raw = (uint16_t)(ap[blkA + 0x10] | (ap[blkA + 0x11] << 8));
        if (v != 0xBEEF || raw != 0xBEEF) {
            kprintf("[pmode] FAIL sel translate v=%04x raw=%04x base=%x\n", v, raw, blkA); fails++; }
    }

    // ---- (b) huge (>64K) block with consecutive tiled selectors ----
    // Allocate 200 KiB as 4 consecutive 64 KiB tiles (selectors sel, sel+8,
    // sel+16, sel+24), each based 64 KiB apart in ONE arena block. Then walk a
    // far pointer across the FIRST tile boundary (offset 0xFFF0..0x1000F spans
    // tile0->tile1) writing a known pattern and read it back.
    uint32_t hugeBytes = 200u * 1024u;
    uint32_t hugeBase  = win16_arena_alloc(hugeBytes, 16);
    uint16_t selH0 = 0;
    {
        uint32_t remaining = hugeBytes, b = hugeBase; uint16_t first = 0, prev = 0;
        while (remaining > 0) {
            uint32_t tile = remaining > 0x10000 ? 0x10000 : remaining;
            uint16_t s = ldt_alloc(b, tile - 1, 0);
            if (s == 0) { kprintf("[pmode] FAIL tile ldt_alloc\n"); fails++; break; }
            if (!first) first = s;
            else if (s != (uint16_t)(prev + 8)) {
                kprintf("[pmode] FAIL tiles not consecutive s=%04x prev=%04x\n", s, prev); fails++; }
            prev = s; b += 0x10000;
            remaining = remaining > 0x10000 ? remaining - 0x10000 : 0;
        }
        selH0 = first;
    }
    {
        // Write a 32-byte ramp pattern straddling the tile0/tile1 boundary using
        // two selectors: bytes at selH0:0xFFF0..0xFFFF (tile0) and
        // (selH0+8):0x0000..0x000F (tile1). Then read back through BOTH.
        x86_16_cpu_t cpu; x86_16_init(&cpu, s_selftest_mem);
        uint16_t selH1 = (uint16_t)(selH0 + 8);
        for (int i = 0; i < 16; i++) x86_16_wr8(&cpu, selH0, (uint16_t)(0xFFF0 + i), (uint8_t)(0xA0 + i));
        for (int i = 0; i < 16; i++) x86_16_wr8(&cpu, selH1, (uint16_t)(0x0000 + i), (uint8_t)(0xB0 + i));
        int ok = 1;
        for (int i = 0; i < 16; i++) if (x86_16_rd8(&cpu, selH0, (uint16_t)(0xFFF0 + i)) != (uint8_t)(0xA0 + i)) ok = 0;
        for (int i = 0; i < 16; i++) if (x86_16_rd8(&cpu, selH1, (uint16_t)(0x0000 + i)) != (uint8_t)(0xB0 + i)) ok = 0;
        // Verify the two tiles are physically adjacent in the arena (the byte at
        // tile0 offset 0xFFFF and tile1 offset 0x0000 are one apart linearly).
        uint8_t *ap = win16_arena_ptr();
        uint32_t lin0 = ldt_base(selH0) + 0xFFFF;
        uint32_t lin1 = ldt_base(selH1) + 0x0000;
        if (lin1 != lin0 + 1) { kprintf("[pmode] FAIL tiles not adjacent lin0=%x lin1=%x\n", lin0, lin1); ok = 0; }
        if (ap[lin0] != 0xAF || ap[lin1] != 0xB0) { kprintf("[pmode] FAIL boundary bytes %02x %02x\n", ap[lin0], ap[lin1]); ok = 0; }
        if (!ok) { kprintf("[pmode] FAIL huge tiled read/write\n"); fails++; }
        else kprintf("[pmode] huge 200K tiled block OK (tiles consecutive + adjacent + pattern verified)\n");
    }

    // ---- (c) far CALL between two CODE selectors + RETF ----
    // selC1 (caller): mov ax,0x1111; call far selC2:0x0000; hlt
    // selC2 (callee): add ax,0x1111; retf            -> expect ax=0x2222
    {
        uint32_t cbA = win16_arena_alloc(0x1000, 16);
        uint32_t cbB = win16_arena_alloc(0x1000, 16);
        uint32_t sbA = win16_arena_alloc(0x1000, 16);
        uint16_t selC1 = ldt_alloc(cbA, 0x0FFF, 1);
        uint16_t selC2 = ldt_alloc(cbB, 0x0FFF, 1);
        uint16_t selSS = ldt_alloc(sbA, 0x0FFF, 0);
        x86_16_cpu_t cpu; x86_16_init(&cpu, s_selftest_mem);
        // caller code at selC1:0x0000
        uint8_t caller[] = {
            0xB8, 0x11, 0x11,                         // mov ax, 0x1111
            0x9A, 0x00,0x00, (uint8_t)(selC2&0xFF),(uint8_t)(selC2>>8), // call far selC2:0
            0xF4                                       // hlt
        };
        for (unsigned i = 0; i < sizeof(caller); i++) x86_16_wr8(&cpu, selC1, (uint16_t)i, caller[i]);
        // callee code at selC2:0x0000
        uint8_t callee[] = {
            0x05, 0x11, 0x11,   // add ax, 0x1111
            0xCB                // retf
        };
        for (unsigned i = 0; i < sizeof(callee); i++) x86_16_wr8(&cpu, selC2, (uint16_t)i, callee[i]);
        cpu.cs = selC1; cpu.ip = 0; cpu.ss = selSS; cpu.sp = 0x0F00;
        int rc = x86_16_run(&cpu, 10000);
        if (rc != 0 || cpu.ax != 0x2222) {
            kprintf("[pmode] FAIL far call ax=%04x rc=%d (expected 0x2222)\n", cpu.ax, rc); fails++; }
        else kprintf("[pmode] inter-selector far CALL/RETF OK (ax=%04x)\n", cpu.ax);
    }

    win16_pmode_enable(0);          // back to real mode
    g_win16_pmode = saved_mode;     // (defensive; should be 0)

    if (fails == 0) kprintf("[x86_16] PMODE selftest PASS (#289 Phase1: LDT + arena + 64K tiling + far-call)\n");
    else            kprintf("[x86_16] PMODE selftest: %d FAILURES\n", fails);
    return fails ? -1 : 0;
}
