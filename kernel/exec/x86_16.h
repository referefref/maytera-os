#ifndef X86_16_H
#define X86_16_H
#include "../types.h"

// ===========================================================================
// #736 Stage 1b: THE PER-GUEST ENVIRONMENT LIVES ON THE CPU, NOT IN STATICS.
//
// These callbacks used to be file-scope statics in x86_16.c with global
// setters, so the LAST guest to call a setter owned them for EVERY guest. That
// is not a style problem, it is a measured bug: launching a Win16 app while a
// DOS game runs redirected the game's INT 21h, its I/O ports and its EGA memory
// hook into the Win16 layer. On golden 1744 Commander Keen 5 DERAILED and died;
// on build 1750 it survived and froze, emitting 46,390 "unhandled INT 33" lines.
//
// It is also WHY the tree grew three INT 21h implementations: with one global
// handler slot, a second guest layer that wanted INT 21h had no way to ask for
// "this CPU's handler", so writing its own was the only thing the interface
// could express. Consolidating the services (Stage 1) without fixing this would
// have left the next subsystem in the same position, making the same choice.
//
// Every hook already receives the cpu it is running on; it simply had nowhere
// to look. Now it does: cpu->owner is the caller's own object.
// ===========================================================================
// The tag MUST be forward-declared at FILE scope before these prototypes. In C,
// a struct tag first mentioned inside a function prototype has scope limited to
// that prototype, so without this line each typedef below would declare its own
// distinct incomplete `struct x86_16_cpu` and gcc reports the memorable
// "expected 'struct x86_16_cpu *' but argument is of type 'x86_16_cpu_t *'
// {aka 'struct x86_16_cpu *'}".
struct x86_16_cpu;

// int_fn return values. 0 = serviced, carry on. Nonzero = stop this burst and
// return to the caller's run loop. X86_16_INT_RETRY additionally means "I did
// NOT service it: rewind to cpu->ins_ip so the guest executes the same INT
// again" - the mechanism a blocking console read uses to wait without blocking
// the interpreter thread (#221, and see #426 on why it must not block).
#define X86_16_INT_STOP   1
#define X86_16_INT_RETRY  2
typedef int      (*x86_16_int_fn)   (struct x86_16_cpu *cpu, uint8_t intno);
typedef int      (*x86_16_farcall_fn)(struct x86_16_cpu *cpu, uint16_t off);
typedef uint16_t (*x86_16_in_fn)    (struct x86_16_cpu *cpu, uint16_t port, int width);
typedef void     (*x86_16_out_fn)   (struct x86_16_cpu *cpu, uint16_t port, uint16_t val, int width);
typedef void     (*x86_16_mem_w_fn) (struct x86_16_cpu *cpu, uint32_t lin, uint16_t val, int width);
typedef uint16_t (*x86_16_mem_r_fn) (struct x86_16_cpu *cpu, uint32_t lin, int width);

// x87 stack depth. The software FPU's state was a file-scope static too, and
// its failure mode is the nastiest of the set: the bugs it produces are stack
// DEPTH errors, so once two guests share one stack every subsequent result in
// BOTH drifts arbitrarily rather than failing cleanly.
#define X86_16_FP_STACK 8

typedef struct x86_16_cpu {
    uint8_t  *mem;     // pointer to a 1 MiB (0x100000) byte array supplied by caller
    // 16-bit register file (layout UNCHANGED from the original so every existing
    // access path is byte-identical). The HIGH 16 bits of the 386 E-registers are
    // kept separately in *_hi below and composed by get_reg32/set_reg32 (#194).
    // We deliberately do NOT use anonymous unions: taking &cpu->ax (uint16_t*)
    // while also accessing a overlapping uint32_t miscompiled 16-bit code at -O2.
    uint16_t ax,bx,cx,dx,si,di,bp,sp;
    uint16_t cs,ds,es,ss,ip,flags;
    // (#390 Corel) 386+ extra segment registers FS/GS. Added at the END of the
    // 16-bit register block so the existing field offsets are byte-identical and
    // no established access path shifts. Both default to 0 (x86_16_init memsets
    // the struct); a guest loads them via `mov gs,rm16` before using a GS/FS
    // segment-override prefix (0x64/0x65).
    uint16_t fs,gs;
    int halted;        // set when the program ends (INT 21/4Ch, HLT, or exit)
    int exit_code;
    unsigned long insn_count;
    // (#194) high 16 bits of the 386 E-registers. THE INDEX IS THE x86 ModRM
    // REGISTER ENCODING, NOT ALPHABETICAL ORDER:
    //     0=AX  1=CX  2=DX  3=BX  4=SP  5=BP  6=SI  7=DI
    // which is what reg16_ptr() (x86_16.c) returns and what get_reg32()/
    // set_reg32() index with. This line used to read "index order 0=AX..7=DI,
    // matching reg16_ptr", which invites the reading 0=AX,1=BX,2=CX. Anything
    // marshalling 32-bit registers in or out of this struct (dos/dpmi_rmcs.c,
    // #740) has to index exhi[] directly, and getting it wrong puts the high
    // half of EAX into ECX: invisible to every 16-bit path, silent at runtime.
    // See blame.md, 2026-08-18.
    uint16_t exhi[8];

    // ---- #736 Stage 1b: per-instance environment (appended, so every field
    // offset above is byte-identical and no established access path shifts).
    void             *owner;          // the caller's own object; hooks read this
                                      // instead of reaching for a file static
    x86_16_int_fn     int_fn;
    x86_16_in_fn      in_fn;
    x86_16_out_fn     out_fn;
    uint32_t          mh_lo, mh_hi;   // memory-hook window [lo, hi)
    x86_16_mem_w_fn   mh_w;
    x86_16_mem_r_fn   mh_r;
    x86_16_farcall_fn farcall_fn;
    uint16_t          farcall_seg;
    int               farcall_active;
    int             (*callfar_abort_fn)(void);
    int               pmode;          // 0 = real mode, 1 = Win16 selectors/LDT
    // Software x87. ST(0) is fp[fp_top]; the stack grows DOWNWARD and is empty
    // when fp_top == X86_16_FP_STACK.
    uint64_t          fp[X86_16_FP_STACK];
    int               fp_top;
    // (#221) IP of the instruction currently executing, captured before the
    // prefix loop consumes anything. An int_fn that wants the guest to RE-EXECUTE
    // the INT it was called for (a blocking BIOS/DOS read with no key yet) sets
    // cpu->ip = cpu->ins_ip and returns X86_16_INT_RETRY. Computed rather than
    // derived as "ip - 2" because an instruction may carry prefixes, and a
    // rewind that lands one byte inside an instruction is the kind of failure
    // that shows up nowhere near its cause. Appended, so every offset above is
    // byte-identical (see the #736 note on `owner`).
    uint16_t          ins_ip;
} x86_16_cpu_t;

// Far-CALL trap callback. Registered for a single trap segment (e.g. the Win16
// thunk segment 0xF000). When a far CALL (opcode 0x9A or 0xFF /3) resolves to a
// target whose SEGMENT == the registered trap seg, the interpreter does NOT jump
// to it. The normal far-call return frame (caller CS:IP) is pushed onto the stack
// FIRST, then fn(cpu, target_off) is invoked. fn is responsible for performing the
// Pascal-style return itself: it reads the caller's return CS:IP off the stack,
// sets cpu->cs/cpu->ip to that, and adjusts cpu->sp accordingly. After fn returns,
// the interpreter simply continues fetching at cpu->cs:cpu->ip.
void x86_16_init(x86_16_cpu_t *cpu, uint8_t *mem1mb);
int  x86_16_selftest_386(void);   // (#194) 386-opcode self-test, logs PASS/FAIL

// ===========================================================================
// (#289 Phase 1) PROTECTED-MODE SELECTOR / LDT MEMORY MODEL.
//
// The interpreter is real-mode FLAT by default: a linear address is
// (seg<<4)+off, wrapped to the 1 MiB backing buffer supplied to x86_16_init.
// That cannot host >1 MiB Win16 apps (Word6 / the 16-bit OLE2 DLLs) which run
// in 16-bit PROTECTED mode: a segment register holds a SELECTOR (an index into
// a descriptor table), and the linear address is descriptor.base + off.
//
// When g_win16_pmode == 0 (the default) EVERYTHING below is dormant and the
// interpreter behaves byte-for-byte as before (the real-mode path). Set it to 1
// per protected-mode NE process (the loader does this) to switch seg_to_lin and
// all rd/wr / CS:DS:SS:ES resolution onto the LDT + a large arena.
//
// A Win16 selector encodes: bits [15:3] = descriptor index, bit2 = TI (table
// indicator, ignored here - we only have one table), bits[1:0] = RPL (ignored
// for translation). ldt_alloc() returns selectors with TI=1 (LDT) and RPL=3
// (i.e. index<<3 | 7), exactly as Win16 KERNEL hands them out, so the low 3
// bits survive arithmetic the apps do. Lookups mask them off.
// ===========================================================================
// #736 Stage 1b: THIS IS THE WIN16 LAYER'S FLAG, NOT THE INTERPRETER'S.
// The interpreter translates addresses using cpu->pmode, so a DOS guest is
// unaffected by a Win16 guest entering protected mode (it used to be
// catastrophically affected: membase() handed the DOS CPU the Win16 ARENA).
// This global remains as the Win16 layer's record of which mode it asked for,
// read by ne.c and win16api.c, whose "the guest" is unambiguously the single
// Win16 guest. win16_pmode_enable() writes both and nothing else writes either.
extern int g_win16_pmode;

#define WIN16_LDT_ENTRIES  4096     // max selectors (one per segment / GlobalAlloc tile)
#define WIN16_SEL_SHIFT    3        // selector = (index << 3) | 7

// Allocate one LDT descriptor with the given linear base (offset into the arena)
// and byte limit. is_code is informational (data vs code) for future #GP checks.
// Returns a Win16 selector (index<<3 | 7), or 0 on table-full.
uint16_t ldt_alloc(uint32_t base, uint32_t limit, int is_code);
uint32_t ldt_base(uint16_t sel);    // descriptor base (linear arena offset); 0 if unallocated
void     ldt_set_base(uint16_t sel, uint32_t base);  // (#289 b468) SetSelectorBase backing
uint32_t ldt_limit(uint16_t sel);   // descriptor byte limit; 0 if unallocated
int      ldt_valid(uint16_t sel);   // 1 if the selector's descriptor is allocated
void     ldt_reset(void);           // clear the whole table (per-run)

// The protected-mode arena: one large contiguous byte region that all selector
// bases point into. Returns the arena base pointer (the rd/wr helpers index it
// when g_win16_pmode). win16_arena_alloc() bump-allocates a block and returns
// its OFFSET within the arena (NOT a pointer), or 0 on out-of-arena. align is a
// power-of-two byte alignment (use 16 for paragraph alignment).
uint8_t *win16_arena_ptr(void);
uint32_t win16_arena_size(void);
uint32_t win16_arena_alloc(uint32_t bytes, uint32_t align);
void     win16_arena_reset(void);

// Convenience: enable/disable protected mode for the next run. Resets the LDT +
// arena when turning on. Safe to call with on==0 to return to real mode.
void     win16_pmode_enable(x86_16_cpu_t *cpu, int on);

// (#289 Phase 1) Protected-mode selector self-test. Proves LDT translation, the
// >64 KiB consecutive-selector tiling, and inter-selector far calls. Logs
// PASS/FAIL and leaves g_win16_pmode == 0. Called at boot like the 386 selftest.
int  x86_16_selftest_pmode(void);
void x86_16_set_int_handler(x86_16_cpu_t *cpu, x86_16_int_fn fn);
void x86_16_set_farcall_trap(x86_16_cpu_t *cpu, uint16_t seg, x86_16_farcall_fn fn);
// (#256) Abort hook for x86_16_call_far's resume loop (return nonzero to stop).
void x86_16_set_callfar_abort(x86_16_cpu_t *cpu, int (*fn)(void));

// I/O port hooks (#201 DOS). When set, IN/OUT instructions call these instead of
// the default stub. width is 1 (al/imm8/dx byte) or 2 (ax/dx word). Pass NULLs to
// restore the default (IN -> 0xFF, OUT -> ignored). Used to capture VGA DAC writes.
void x86_16_set_io_handlers(x86_16_cpu_t *cpu, x86_16_in_fn infn, x86_16_out_fn outfn);

// Memory-mapped I/O hooks (#202 EGA planar VGA). When set, any byte/word write
// or read whose LINEAR address falls in [lo, hi) is routed to these callbacks
// instead of the backing mem[] array. This lets the DOS layer emulate the EGA
// planar framebuffer at 0xA0000-0xAFFFF (4 hidden bitplanes selected by the
// VGA sequencer/graphics-controller registers), which Commander Keen and other
// mode-0Dh games rely on. Pass lo==hi (or 0,0) to disable. width is 1 or 2.
void x86_16_set_mem_hook(x86_16_cpu_t *cpu, uint32_t lo, uint32_t hi, x86_16_mem_w_fn wfn, x86_16_mem_r_fn rfn);

// Diagnostic: end the current x86_16_run burst at the next instruction boundary
// (run returns 1, as if it hit the slice cap). Used by the DOS layer to drop
// into single-step tracing right after a specific INT.
void x86_16_request_stop(void);

// Debug: trace one __far __pascal function. When a far CALL targets seg:off, the
// interpreter logs (via win16_trace) the entry DS and the 2 FAR-pointer args, then
// every instruction's registers until the function returns, then the words those
// far pointers point at. Used to diagnose CARDS.cdtInit's garbage card width.
// Pass seg==0 && off==0 to disable.
void x86_16_set_fn_trace(uint16_t seg, uint16_t off);

// Execute up to max_insns instructions (or until halted). Returns:
//  0 = halted normally, 1 = hit max_insns, -1 = unsupported/illegal opcode (logs via kprintf).
int  x86_16_run(x86_16_cpu_t *cpu, unsigned long max_insns);

// Diagnostic instruction ring. x86_16_ring_enable(1) starts recording every
// executed instruction (cs:ip, first three opcode bytes, the register file) into
// a fixed 4096-entry ring; x86_16_ring_dump() prints the last n oldest-first.
// Off by default: recording costs one predictable branch per instruction.
void x86_16_ring_enable(int on);
void x86_16_ring_dump(const char *tag, int n);

// 8/16-bit linear memory helpers (real-mode seg:off -> (seg<<4)+off, wrapped to 1MiB):
uint8_t  x86_16_rd8 (x86_16_cpu_t *cpu, uint16_t seg, uint16_t off);
uint16_t x86_16_rd16(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off);
void     x86_16_wr8 (x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint8_t v);
void     x86_16_wr16(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off, uint16_t v);

// Call a 16-bit __far __pascal procedure from the host (used to invoke a Win16
// window procedure). Pushes `nargs` words from args[] in Pascal order (args[0]
// is pushed FIRST, so it ends up deepest on the stack / is the leftmost C arg),
// then pushes a SENTINEL far return frame whose CS == a reserved segment that is
// never executable. The interpreter runs until control returns to that sentinel
// (RETF pops it), then stops. Returns the callee's 32-bit result as DX:AX with
// AX in *out_ax and DX in *out_dx (both may be NULL). The current CPU register
// file is saved and restored around the call EXCEPT for the bookkeeping needed
// to run; DS/ES are left as the callee set them on return is NOT relied upon.
// Returns 0 on a clean sentinel return, -1 if the call ran away (insn cap) or a
// bad opcode was hit. `max_insns` bounds the nested run (0 = a large default).
int x86_16_call_far(x86_16_cpu_t *cpu, uint16_t seg, uint16_t off,
                    const uint16_t *args, int nargs,
                    uint16_t *out_ax, uint16_t *out_dx,
                    unsigned long max_insns);

// Self-test: runs a tiny hand-assembled program and returns 0 on success, -1 on failure.
int x86_16_selftest(void);

#endif
