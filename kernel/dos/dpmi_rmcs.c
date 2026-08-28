// dos/dpmi_rmcs.c - #740: DPMI 0300h, the C surface, the layout locks, and the
// runnable proof.
//
// Read dos/dpmi_rmcs.h first, then rustkern/dpmi_rmcs.rs (the marshaller). This
// file holds three things and nothing else:
//
//   1. The _Static_asserts that lock the C view of the RMCS and of the register
//      frame to the Rust view. These are the whole reason a prefix mirror of
//      x86_16_cpu_t is safe rather than a landmine.
//   2. The arena binding and the default service router.
//   3. dpmi_rmcs_selftest(), compiled in ONLY under -DDPMI_RMCS_SELFTEST
//      (`make DPMITEST=1`), which drives the bridge with synthesised register
//      blocks against the REAL service core and prints the before/after
//      register files to serial.
//
// WHY (3) EXISTS AND IS NOT OPTIONAL. This project has a documented history of
// kernel code that linked and never ran: validate_user_ptr, sse_save/restore,
// increment_build.sh, and the concurrency lint that could not fail for two
// independent reasons. "It compiles" and "the symbol is in the archive" are not
// evidence. No 32-bit execution path exists yet (the LE loader is a separate
// piece of #740), so a real DOS/4GW guest cannot drive this today; a
// deterministic harness against the real services can, and does.

#include "dpmi_rmcs.h"
#include "int21svc.h"
#include "../exec/x86_16.h"
#include "../serial.h"
#include "../string.h"

// ===========================================================================
// LAYOUT LOCKS
// ---------------------------------------------------------------------------
// A wrong offset here corrupts ONE register in EVERY DOS call the guest makes.
// It never crashes; it presents as "the game is subtly broken", which is the
// most expensive possible failure to find. So both structures are pinned at
// COMPILE time, in both languages:
//
//   here          __builtin_offsetof against the real C structs, every build
//   Rust side     core::mem::offset_of! in dpmi_rmcs_layout_selftest_rs
//
// __builtin_offsetof, not the offsetof in types.h: that one is the
// &((T*)0)->member form, which is not a constant expression a _Static_assert is
// required to accept.
// ===========================================================================
_Static_assert(sizeof(dpmi_rmcs_t) == 50, "RMCS is 50 bytes, packed (DPMI 0.9 s.11)");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, edi)      == 0x00, "RMCS EDI");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, esi)      == 0x04, "RMCS ESI");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ebp)      == 0x08, "RMCS EBP");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, reserved) == 0x0C, "RMCS reserved");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ebx)      == 0x10, "RMCS EBX");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, edx)      == 0x14, "RMCS EDX");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ecx)      == 0x18, "RMCS ECX");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, eax)      == 0x1C, "RMCS EAX");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, flags)    == 0x20, "RMCS flags sit AFTER the dwords");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, es)       == 0x22, "RMCS ES");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ds)       == 0x24, "RMCS DS");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, fs)       == 0x26, "RMCS FS");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, gs)       == 0x28, "RMCS GS");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ip)       == 0x2A, "RMCS IP");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, cs)       == 0x2C, "RMCS CS");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, sp)       == 0x2E, "RMCS SP");
_Static_assert(__builtin_offsetof(dpmi_rmcs_t, ss)       == 0x30, "RMCS SS");

// The register frame. rustkern/dpmi_rmcs.rs mirrors the LEADING FIELDS of
// x86_16_cpu_t as X86RegFrame and writes them individually. #736 Stage 1b
// appended the per-instance environment specifically so these offsets stayed
// byte-identical; these asserts are what turns that intention into a guarantee.
// If someone inserts a field into the middle of x86_16_cpu_t, THIS BUILD FAILS,
// which is the entire point.
_Static_assert(__builtin_offsetof(x86_16_cpu_t, mem)        ==  0, "frame mem");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, ax)         ==  8, "frame ax");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, bx)         == 10, "frame bx");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, cx)         == 12, "frame cx");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, dx)         == 14, "frame dx");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, si)         == 16, "frame si");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, di)         == 18, "frame di");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, bp)         == 20, "frame bp");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, sp)         == 22, "frame sp");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, cs)         == 24, "frame cs");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, ds)         == 26, "frame ds");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, es)         == 28, "frame es");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, ss)         == 30, "frame ss");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, ip)         == 32, "frame ip");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, flags)      == 34, "frame flags");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, fs)         == 36, "frame fs");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, gs)         == 38, "frame gs");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, halted)     == 40, "frame halted");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, exit_code)  == 44, "frame exit_code");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, insn_count) == 48, "frame insn_count");
_Static_assert(__builtin_offsetof(x86_16_cpu_t, exhi)       == 56, "frame exhi");

_Static_assert(sizeof(dpmi_arena_t) == 24, "dpmi_arena_t must match Rust DpmiArena");
_Static_assert(__builtin_offsetof(dpmi_arena_t, base)   ==  0, "arena base");
_Static_assert(__builtin_offsetof(dpmi_arena_t, size)   ==  8, "arena size");
_Static_assert(__builtin_offsetof(dpmi_arena_t, oob_rd) == 12, "arena oob_rd");
_Static_assert(__builtin_offsetof(dpmi_arena_t, oob_wr) == 16, "arena oob_wr");

// ===========================================================================
// REPORTING for the Rust side. kprintf is variadic and is deliberately kept out
// of the Rust FFI: the DECISION is Rust, the REPORTING is C.
// ===========================================================================

// One line per distinct interrupt number, so a guest that MISSes in a loop
// produces a diagnosis rather than a flood. The MISS is the measuring
// instrument (docs/DPMI_BRIDGE_DESIGN.md 2.7): the first run of a real binary
// is supposed to be a MEASUREMENT of what it needs, which a flood destroys.
static uint8_t g_miss_seen[256];
static uint32_t g_miss_total;

void dpmi_rmcs_log_miss(uint8_t intno, uint16_t ax) {
    g_miss_total++;
    if (g_miss_seen[intno]) return;
    g_miss_seen[intno] = 1;
    kprintf("[dpmi] RMINT %02x NO SERVICE (ax=%04x) -> stubbed CF=1 AX=0001\n",
            intno, ax);
}

void dpmi_rmcs_log_fault(uint32_t kind, uint32_t flat, uint32_t len) {
    static uint32_t n;
    if (n >= 16) return;              // bounded: a wild guest must not own the log
    n++;
    const char *what = (kind == 1) ? "RMCS block"
                     : (kind == 2) ? "guest read"
                     : (kind == 3) ? "guest write" : "?";
    kprintf("[dpmi] REFUSED %s outside the arena: flat=0x%08x len=%u\n",
            what, flat, len);
    if (n == 16) kprintf("[dpmi] further arena-bound refusals silent this boot\n");
}

void dpmi_rmcs_log_hoststack(uint16_t seg, uint16_t sp) {
    static int once;
    if (once) return;
    once = 1;
    kprintf("[dpmi] RMCS SS:SP was 0:0; host stack %04x:%04x substituted "
            "(no real-mode code executes, so nothing is pushed on it)\n", seg, sp);
}

// ===========================================================================
// THE C SURFACE
// ===========================================================================

void dpmi_rmcs_bind_arena(dos_svc_ctx_t *ctx, dpmi_arena_t *arena) {
    ctx->mem_u    = arena;
    ctx->mem.rd8  = dpmi_arena_rd8_rs;
    ctx->mem.wr8  = dpmi_arena_wr8_rs;
    ctx->mem.rd16 = dpmi_arena_rd16_rs;
    ctx->mem.wr16 = dpmi_arena_wr16_rs;
}

int dpmi_rmcs_dos_dispatch(void *user, uint8_t intno, struct x86_16_cpu *frame) {
    dos_svc_ctx_t *ctx = (dos_svc_ctx_t *)user;
    if (!ctx || !frame) return 0;
    if (intno == 0x21) {
        dos_svc_int21(ctx, frame);
        return 1;
    }
    // Everything else: not handled. The bridge logs the MISS and applies the
    // stub effect. Do NOT add a service here; add it to the one core, or route
    // to a factored-out one, and the whole tree gains it at once.
    return 0;
}

// ===========================================================================
// THE RUNNABLE PROOF (-DDPMI_RMCS_SELFTEST, i.e. `make DPMITEST=1`)
// ===========================================================================
#ifdef DPMI_RMCS_SELFTEST

#include "../mm/heap.h"
#include "../fs/guestfs.h"
#include "../proc/process.h"   // PROC_AS_UID

#define HARNESS_ARENA 0x100000u          // the guest's first megabyte

// Segment layout inside the harness arena, chosen so every buffer is at a
// paragraph boundary and every flat address is legible in the log.
#define SEG_STR   0x2000u   // 0x20000  the AH=09h message
#define SEG_PATH  0x2100u   // 0x21000  an ASCIIZ DOS path
#define SEG_DATA  0x2200u   // 0x22000  bytes to write
#define SEG_READ  0x2300u   // 0x23000  bytes read back
#define SEG_RMCS  0x3000u   // 0x30000  the real-mode call structure
#define SEG_STACK 0x4000u   // 0x40000  the host-supplied real-mode stack

static int h_checks, h_fails;

static void h_ok(const char *what, int cond) {
    h_checks++;
    if (!cond) h_fails++;
    kprintf("[DPMI300]   %-52s %s\n", what, cond ? "PASS" : "FAIL");
}

// ---- arena helpers. Every one goes through the Rust chokepoint on purpose:
// the harness must not have a private path into guest memory, or it would be
// testing something the guest cannot reach.
static void a_wr8(dpmi_arena_t *a, uint16_t seg, uint16_t off, uint8_t v) {
    dpmi_arena_wr8_rs(a, seg, off, v);
}
static uint8_t a_rd8(dpmi_arena_t *a, uint16_t seg, uint16_t off) {
    return dpmi_arena_rd8_rs(a, seg, off);
}
static int a_put(dpmi_arena_t *a, uint16_t seg, const char *s) {
    int i = 0;
    for (; s[i]; i++) a_wr8(a, seg, (uint16_t)i, (uint8_t)s[i]);
    a_wr8(a, seg, (uint16_t)i, 0);
    return i;
}

// ---- the RMCS, read and written as the guest would ------------------------
static void rmcs_store(dpmi_arena_t *a, const dpmi_rmcs_t *r) {
    const uint8_t *p = (const uint8_t *)r;
    for (int i = 0; i < (int)sizeof(*r); i++) a_wr8(a, SEG_RMCS, (uint16_t)i, p[i]);
}
static void rmcs_load(dpmi_arena_t *a, dpmi_rmcs_t *r) {
    uint8_t *p = (uint8_t *)r;
    for (int i = 0; i < (int)sizeof(*r); i++) p[i] = a_rd8(a, SEG_RMCS, (uint16_t)i);
}
static void rmcs_dump(const char *when, const dpmi_rmcs_t *r) {
    kprintf("[DPMI300]   %-6s EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n",
            when, r->eax, r->ebx, r->ecx, r->edx);
    kprintf("[DPMI300]   %-6s ESI=%08x EDI=%08x EBP=%08x FL=%04x%s\n",
            "", r->esi, r->edi, r->ebp, r->flags,
            (r->flags & 1) ? " CF" : "");
    kprintf("[DPMI300]   %-6s DS=%04x ES=%04x FS=%04x GS=%04x CS:IP=%04x:%04x SS:SP=%04x:%04x\n",
            "", r->ds, r->es, r->fs, r->gs, r->cs, r->ip, r->ss, r->sp);
}

// One bridged call: store the RMCS, dump it, cross the bridge, dump it back.
static int h_call(dpmi_arena_t *a, const char *label,
                  dpmi_rmcs_t *r, uint16_t bx, dpmi_rm_dispatch_fn disp, void *user) {
    x86_16_cpu_t frame;
    memset(&frame, 0, sizeof(frame));
    kprintf("[DPMI300] %s: INT 31h AX=0300h BX=%04x (intno=%02x)\n",
            label, bx, bx & 0xFF);
    rmcs_store(a, r);
    rmcs_dump("in", r);
    int rc = dpmi_rmcs_call_rs(a, (uint32_t)SEG_RMCS << 4, a->size, bx, &frame,
                               disp, user, SEG_STACK, 0x0FFE);
    rmcs_load(a, r);
    rmcs_dump("out", r);
    kprintf("[DPMI300]   rc=%d (0 = the simulation ran; the interrupt's own "
            "result is the RMCS CF)\n", rc);
    return rc;
}

// ---- console: where the guest's AH=09h / AH=02h output goes ---------------
static char g_cap[256];
static int  g_cap_n;
static void h_putc(void *u, uint8_t ch) {
    (void)u;
    if (g_cap_n < (int)sizeof(g_cap) - 1) g_cap[g_cap_n++] = (char)ch;
    kputc((char)ch);
}

// ---------------------------------------------------------------------------
// TEST 1: the field mapping itself, against a PROBE service.
//
// This is the test that can actually fail, and it is first for that reason. It
// proves, in both directions and with 32-bit values whose halves differ:
//   - every RMCS dword reaches the right register, INCLUDING its high half,
//     which lives in a separately-indexed exhi[] array in x86 ModRM order
//     (AX,CX,DX,BX,SP,BP,SI,DI). A 0=AX,1=BX ordering mistake would put the
//     high half of EAX into ECX and would be INVISIBLE to any test using
//     16-bit values.
//   - the write-back MASK: everything comes back except SS, SP, CS and IP.
// ---------------------------------------------------------------------------
static int g_probe_seen_ok;
static int h_probe(void *user, uint8_t intno, struct x86_16_cpu *f) {
    (void)user;
    int ok = 1;
    ok &= (intno == 0x21);                       // BL, with BH masked off
    ok &= (f->ax == 0x2222 && f->exhi[0] == 0x1111);
    ok &= (f->cx == 0x6666 && f->exhi[1] == 0x5555);
    ok &= (f->dx == 0x8888 && f->exhi[2] == 0x7777);
    ok &= (f->bx == 0x4444 && f->exhi[3] == 0x3333);
    ok &= (f->bp == 0xEEEE && f->exhi[5] == 0xDDDD);
    ok &= (f->si == 0xAAAA && f->exhi[6] == 0x9999);
    ok &= (f->di == 0xCCCC && f->exhi[7] == 0xBBBB);
    ok &= (f->flags == 0x0202);
    ok &= (f->es == 0x1234 && f->ds == 0x2345 && f->fs == 0x3456 && f->gs == 0x4567);
    ok &= (f->cs == 0x6789 && f->ip == 0x5678);  // copied in, never copied out
    ok &= (f->ss == 0x89AB && f->sp == 0x789A);  // the guest's own stack, kept
    g_probe_seen_ok = ok;

    // Answer with a different pattern so the return path is proved too.
    f->ax = 0x0F0E; f->exhi[0] = 0x0D0C;
    f->bx = 0x1F1E; f->exhi[3] = 0x1D1C;
    f->cx = 0x2F2E; f->exhi[1] = 0x2D2C;
    f->dx = 0x3F3E; f->exhi[2] = 0x3D3C;
    f->si = 0x4F4E; f->exhi[6] = 0x4D4C;
    f->di = 0x5F5E; f->exhi[7] = 0x5D5C;
    f->bp = 0x6F6E; f->exhi[5] = 0x6D6C;
    f->flags = 0x0247;
    f->es = 0x7777; f->ds = 0x8888; f->fs = 0x9999; f->gs = 0xAAAA;
    // Scribble on the four the host owns. None must reach the guest's RMCS.
    f->cs = 0xDEAD; f->ip = 0xBEEF; f->ss = 0xFEED; f->sp = 0xFACE;
    return 1;
}

static void test_mapping(dpmi_arena_t *a) {
    dpmi_rmcs_t r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x11112222; r.ebx = 0x33334444; r.ecx = 0x55556666; r.edx = 0x77778888;
    r.esi = 0x9999AAAA; r.edi = 0xBBBBCCCC; r.ebp = 0xDDDDEEEE;
    r.flags = 0x0202;
    r.es = 0x1234; r.ds = 0x2345; r.fs = 0x3456; r.gs = 0x4567;
    r.ip = 0x5678; r.cs = 0x6789; r.sp = 0x789A; r.ss = 0x89AB;
    r.reserved = 0x5A5A5A5A;

    g_probe_seen_ok = 0;
    int rc = h_call(a, "T1 field mapping (probe service)", &r, 0xFF21, h_probe, 0);
    h_ok("T1 simulation ran (rc == 0)", rc == DPMI_RMCS_OK);
    h_ok("T1 every register arrived intact, incl. 32-bit halves", g_probe_seen_ok == 1);
    h_ok("T1 EAX written back", r.eax == 0x0D0C0F0E);
    h_ok("T1 EBX written back", r.ebx == 0x1D1C1F1E);
    h_ok("T1 ECX written back", r.ecx == 0x2D2C2F2E);
    h_ok("T1 EDX written back", r.edx == 0x3D3C3F3E);
    h_ok("T1 ESI written back", r.esi == 0x4D4C4F4E);
    h_ok("T1 EDI written back", r.edi == 0x5D5C5F5E);
    h_ok("T1 EBP written back", r.ebp == 0x6D6C6F6E);
    h_ok("T1 flags written back", r.flags == 0x0247);
    h_ok("T1 DS/ES/FS/GS written back",
         r.es == 0x7777 && r.ds == 0x8888 && r.fs == 0x9999 && r.gs == 0xAAAA);
    h_ok("T1 CS NOT written back", r.cs == 0x6789);
    h_ok("T1 IP NOT written back", r.ip == 0x5678);
    h_ok("T1 SS NOT written back", r.ss == 0x89AB);
    h_ok("T1 SP NOT written back", r.sp == 0x789A);
    h_ok("T1 reserved dword untouched", r.reserved == 0x5A5A5A5A);
}

// ---------------------------------------------------------------------------
// TEST 2: a real character write, through the bridge, into the ONE core.
// ---------------------------------------------------------------------------
static void test_ah09(dpmi_arena_t *a, dos_svc_ctx_t *ctx) {
    static const char *msg = "DPMI 0300h -> INT 21h AH=09h reached the one service core$";
    a_put(a, SEG_STR, msg);

    dpmi_rmcs_t r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x00000900;
    r.ds  = SEG_STR;
    r.edx = 0;
    // SS:SP left 0:0 on purpose: this is the "host supplies the stack" path.

    g_cap_n = 0;
    kprintf("[DPMI300] --- guest output begins ---\n");
    int rc = h_call(a, "T2 AH=09h print string", &r, 0x0021,
                    dpmi_rmcs_dos_dispatch, ctx);
    kprintf("\n[DPMI300] --- guest output ends ---\n");
    g_cap[g_cap_n] = 0;

    h_ok("T2 simulation ran", rc == DPMI_RMCS_OK);
    h_ok("T2 no CF from the interrupt", (r.flags & 1) == 0);
    // AH=09h prints up to but not including the '$'.
    h_ok("T2 the console received exactly the guest's string",
         g_cap_n == (int)strlen(msg) - 1 && strncmp(g_cap, msg, (uint32_t)g_cap_n) == 0);
}

// ---------------------------------------------------------------------------
// TEST 3: a real file, created, written, closed, reopened, read back and
// deleted, entirely through DPMI 0300h.
//
// Every byte of the path and of the payload lives in the arena and is fetched
// by the service core through the Rust accessors, exactly as a DOS/4GW guest's
// would be. This is the round trip the ticket asks to see.
// ---------------------------------------------------------------------------
static void test_file(dpmi_arena_t *a, dos_svc_ctx_t *ctx) {
    static const char *path = "\\DPMI300.TXT";
    static const char *data = "MayteraOS #740: this file crossed the DPMI 0300h bridge.\n";
    int dlen = (int)strlen(data);

    a_put(a, SEG_PATH, path);
    a_put(a, SEG_DATA, data);

    dpmi_rmcs_t r;
    uint16_t h;

    // --- AH=3Ch create ----------------------------------------------------
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003C00; r.ecx = 0; r.ds = SEG_PATH; r.edx = 0;
    h_call(a, "T3a AH=3Ch create", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);
    int created = ((r.flags & 1) == 0);
    h_ok("T3a create succeeded (CF clear)", created);
    if (!created) {
        kprintf("[DPMI300]   create refused with AX=%04x. If AX=0005 the #708 "
                "guest gate denied it; the bridge itself still reported the "
                "failure correctly, which is what T3a measures.\n",
                (uint16_t)r.eax);
        return;
    }
    h = (uint16_t)r.eax;

    // --- AH=40h write -----------------------------------------------------
    memset(&r, 0, sizeof(r));
    r.eax = 0x00004000; r.ebx = h; r.ecx = (uint32_t)dlen;
    r.ds = SEG_DATA; r.edx = 0;
    h_call(a, "T3b AH=40h write", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T3b every byte written", (r.flags & 1) == 0 && (int)(uint16_t)r.eax == dlen);

    // --- AH=3Eh close (this is where a buffered file reaches the medium) ---
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003E00; r.ebx = h;
    h_call(a, "T3c AH=3Eh close", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T3c close reported success (the write LANDED)", (r.flags & 1) == 0);

    // --- AH=3Dh open ------------------------------------------------------
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003D00; r.ds = SEG_PATH; r.edx = 0;
    h_call(a, "T3d AH=3Dh open", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);
    int opened = ((r.flags & 1) == 0);
    h_ok("T3d reopened the file the guest created", opened);
    if (!opened) return;
    h = (uint16_t)r.eax;

    // --- AH=3Fh read ------------------------------------------------------
    for (int i = 0; i < 128; i++) a_wr8(a, SEG_READ, (uint16_t)i, 0);
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003F00; r.ebx = h; r.ecx = 128; r.ds = SEG_READ; r.edx = 0;
    h_call(a, "T3e AH=3Fh read", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);
    int got = (int)(uint16_t)r.eax;
    h_ok("T3e read back the same byte count", (r.flags & 1) == 0 && got == dlen);

    int same = (got == dlen);
    for (int i = 0; i < got && i < dlen; i++)
        if (a_rd8(a, SEG_READ, (uint16_t)i) != (uint8_t)data[i]) same = 0;
    h_ok("T3e the bytes in guest memory are the bytes written", same);

    kprintf("[DPMI300]   guest buffer at %04x:0000 (%d bytes) reads: \"", SEG_READ, got);
    for (int i = 0; i < got; i++) {
        uint8_t c = a_rd8(a, SEG_READ, (uint16_t)i);
        kputc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
    }
    kprintf("\"\n");

    // --- AH=3Eh close, AH=41h delete --------------------------------------
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003E00; r.ebx = h;
    h_call(a, "T3f AH=3Eh close", &r, 0x0021, dpmi_rmcs_dos_dispatch, ctx);

    memset(&r, 0, sizeof(r));
    r.eax = 0x00004100; r.ds = SEG_PATH; r.edx = 0;
    h_call(a, "T3g AH=41h delete (leave no trace)", &r, 0x0021,
           dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T3g deleted", (r.flags & 1) == 0);

    // Prove it is gone by asking the same way the guest would.
    memset(&r, 0, sizeof(r));
    r.eax = 0x00003D00; r.ds = SEG_PATH; r.edx = 0;
    h_call(a, "T3h AH=3Dh reopen must FAIL", &r, 0x0021,
           dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T3h the deleted file is really gone (CF=1, AX=0002)",
         (r.flags & 1) == 1 && (uint16_t)r.eax == 2);
}

// ---------------------------------------------------------------------------
// TEST 4: MISS discipline. An interrupt with no service behind it must LOG and
// STUB WITH THE CORRECT REGISTER AND FLAG EFFECT, never return quietly. A
// log-only MISS is what desynchronised the Win16 interpreter (blame.md), and
// the DOS layer's own INT 21h default arm STILL returns carry-clear today.
// ---------------------------------------------------------------------------
static void test_miss(dpmi_arena_t *a, dos_svc_ctx_t *ctx) {
    dpmi_rmcs_t r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x00004300;      // EMS "get version", a plausible thing to ask
    r.flags = 0x0202;        // CF deliberately CLEAR on the way in
    h_call(a, "T4 INT 67h (EMS): nothing implements it", &r, 0x0067,
           dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T4 MISS sets CF in the simulated interrupt", (r.flags & 1) == 1);
    h_ok("T4 MISS returns AX=0001 (invalid function)", r.eax == 0x00000001);
}

// ---------------------------------------------------------------------------
// TEST 5: the bounds chokepoint. Guest-controlled addresses are REFUSED, not
// clamped, and not folded into a spare page the way the Win16 arena does it.
// ---------------------------------------------------------------------------
static void test_bounds(dpmi_arena_t *a, dos_svc_ctx_t *ctx) {
    uint32_t rd0 = a->oob_rd, wr0 = a->oob_wr;

    // (a) an RMCS block that straddles the end of the arena.
    x86_16_cpu_t frame;
    memset(&frame, 0, sizeof(frame));
    int rc = dpmi_rmcs_call_rs(a, a->size - 8, a->size, 0x0021, &frame,
                               dpmi_rmcs_dos_dispatch, ctx, SEG_STACK, 0x0FFE);
    h_ok("T5a an RMCS straddling the arena end is refused", rc == DPMI_RMCS_EBOUNDS);

    // (b) an RMCS pointer whose flat+50 would wrap 32 bits.
    memset(&frame, 0, sizeof(frame));
    rc = dpmi_rmcs_call_rs(a, 0xFFFFFFF0u, a->size, 0x0021, &frame,
                           dpmi_rmcs_dos_dispatch, ctx, SEG_STACK, 0x0FFE);
    h_ok("T5b a near-wrap RMCS pointer is refused", rc == DPMI_RMCS_EBOUNDS);

    // (d) THE REGRESSION THIS FILE EXISTS TO PIN (#740, Discworld II). An RMCS
    //     ABOVE the real-mode window but INSIDE the client's flat space is the
    //     normal case for a 32-bit guest, which builds it on its own 32-bit
    //     stack. It must be ACCEPTED, and the real-mode pointers inside it must
    //     still be bounded by the small window. The harness arena is one flat
    //     megabyte, so the two limits are simulated: the RMCS goes at 0x0C0000
    //     with a real-mode limit of 0x080000 below it.
    //
    //     Before the rmcs_limit parameter this returned EBOUNDS, which is what
    //     the game hit: two 0300h calls, both refused, no interrupt simulated.
    {
        uint32_t save_size = a->size;
        uint32_t hi_flat   = 0x000C0000u;
        uint32_t rd1 = a->oob_rd;
        a->size = 0x00080000u;              // the pretend real-mode window
        memset(&frame, 0, sizeof(frame));
        for (uint32_t k = 0; k < 50; k++) a->base[hi_flat + k] = 0;
        a->base[hi_flat + 0x1C] = 0x00;     // RMCS EAX low  = AL
        a->base[hi_flat + 0x1D] = 0x30;     // RMCS EAX high = AH=30h, no pointers
        rc = dpmi_rmcs_call_rs(a, hi_flat, save_size, 0x0021, &frame,
                               dpmi_rmcs_dos_dispatch, ctx, SEG_STACK, 0x0FFE);
        h_ok("T5d an RMCS above the real-mode window but inside the client is served",
             rc == DPMI_RMCS_OK);
        h_ok("T5d and nothing was counted out-of-arena for it", a->oob_rd == rd1);
        a->size = save_size;
    }

    // (c) a DS:DX the service will follow, pointing past the arena. Real-mode
    //     seg:off reaches 0x10FFEF, which is past a 1 MiB arena.
    dpmi_rmcs_t r;
    memset(&r, 0, sizeof(r));
    r.eax = 0x00000900; r.ds = 0xFFFFu; r.edx = 0xFFF0u;
    h_call(a, "T5c AH=09h with DS:DX outside the arena", &r, 0x0021,
           dpmi_rmcs_dos_dispatch, ctx);
    h_ok("T5c the out-of-arena read was counted and refused", a->oob_rd > rd0);
    h_ok("T5c nothing outside the arena was written", a->oob_wr == wr0);
}

// ---------------------------------------------------------------------------
void dpmi_rmcs_selftest(void) {
    kprintf("\n[DPMI300] ============================================================\n");
    kprintf("[DPMI300] #740 DPMI 0300h simulate-real-mode-interrupt bridge\n");
    kprintf("[DPMI300] Rust marshaller + the ONE INT 21h service core (#736).\n");
    kprintf("[DPMI300] ============================================================\n");

    uint32_t nchk = 0;
    int lrc = dpmi_rmcs_layout_selftest_rs(&nchk);
    kprintf("[DPMI300] RMCS/frame layout: %s (%u checks%s)\n",
            lrc == 0 ? "PASS" : "FAIL", nchk, lrc ? ", first failure noted" : "");
    if (lrc != 0) kprintf("[DPMI300]   first failing check index: %d\n", lrc);
    h_checks = 0; h_fails = 0;
    h_ok("layout self-test", lrc == 0 && nchk > 0);

    uint8_t *mem = (uint8_t *)kmalloc(HARNESS_ARENA);
    if (!mem) { kprintf("[DPMI300] ABORT: no memory for the arena\n"); return; }
    memset(mem, 0, HARNESS_ARENA);

    dpmi_arena_t arena;
    memset(&arena, 0, sizeof(arena));
    arena.base = mem;
    arena.size = HARNESS_ARENA;

    dos_svc_ctx_t *ctx = (dos_svc_ctx_t *)kmalloc(sizeof(dos_svc_ctx_t));
    if (!ctx) { kfree(mem); kprintf("[DPMI300] ABORT: no memory for the context\n"); return; }

    // A DPMI guest is a THIRD guest lineage, so it gets its own identity slot.
    // Sharing GUESTFS_SLOT_DOS would make the gate's log say "dos" for accesses
    // a DOS/4GW guest made, which is the identity confusion #708 exists to stop.
    dos_svc_ctx_init(ctx, GUESTFS_SLOT_DPMI, "dpmi");
    dpmi_rmcs_bind_arena(ctx, &arena);
    ctx->con.putc = h_putc;
    ctx->has_ivt  = 0;          // no real-mode IVT behind a 32-bit guest
    ctx->psp_seg  = 0x0100;

    // DEBUG BUILD ONLY. This harness runs before any user has authenticated, so
    // there is no session to inherit an identity from and guestfs_arm_session()
    // would (correctly) refuse. Arming explicitly as root is exactly why this
    // whole function is behind -DDPMI_RMCS_SELFTEST and never in a golden.
    kprintf("[DPMI300] DEBUG BUILD: arming the DPMI guest slot as uid 0. This "
            "path is compiled out of a normal/golden kernel.\n");
    int arc = guestfs_arm_rs(GUESTFS_SLOT_DPMI, PROC_AS_UID, 0);
    kprintf("[DPMI300] guestfs arm: rc=%d\n", arc);

    test_mapping(&arena);
    test_ah09(&arena, ctx);
    test_file(&arena, ctx);
    test_miss(&arena, ctx);
    test_bounds(&arena, ctx);

    dos_svc_ctx_close_all(ctx);
    dos_svc_report(ctx);
    guestfs_finish(GUESTFS_SLOT_DPMI);

    uint32_t calls = 0, miss = 0, hstk = 0;
    dpmi_rmcs_stats_rs(&calls, &miss, &hstk);
    kprintf("[DPMI300] bridge counters: calls=%u miss=%u (logged-distinct, "
            "total %u) host-stack=%u arena-oob rd=%u wr=%u\n",
            calls, miss, g_miss_total, hstk, arena.oob_rd, arena.oob_wr);
    kprintf("[DPMI300] VERDICT: %d/%d checks passed -> %s\n",
            h_checks - h_fails, h_checks, h_fails == 0 ? "PASS" : "FAIL");
    kprintf("[DPMI300] ============================================================\n\n");

    kfree(ctx);
    kfree(mem);
}

#endif // DPMI_RMCS_SELFTEST
