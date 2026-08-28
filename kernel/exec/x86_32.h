// exec/x86_32.h - C view of the #740 32-bit guest execution core.
//
// The implementation is rustkern/x86_32.rs. This header is the FFI contract and
// nothing else: it declares no behaviour, it restates no semantics, and every
// structural claim it makes about the Rust struct is locked by a
// _Static_assert that the compiler checks, plus one RUNTIME check against
// x86_32_cpu_size(), which is computed by rustc. A layout that drifts on one
// side therefore cannot reach a boot.
//
// See docs/DOS4GW_DESIGN.md section 3 (option A) for why this core exists, and
// the header comment of rustkern/x86_32.rs for the memory model and the MISS
// policy, which are the two things a caller has to understand.

#ifndef EXEC_X86_32_H
#define EXEC_X86_32_H

#include "../types.h"

// ---------------------------------------------------------------------------
// Exit reasons, mirrored from rustkern/x86_32.rs.
// ---------------------------------------------------------------------------
#define X32_EXIT_BUDGET       0u  // the instruction budget ran out; nothing wrong
#define X32_EXIT_STOP_EIP     1u  // EIP reached cpu->stop_eip (harness breakpoint)
#define X32_EXIT_INT          2u  // software INT n: exit_arg = vector
#define X32_EXIT_HLT          3u
#define X32_EXIT_IO_IN        4u  // exit_arg = port, io_size = 1/2/4
#define X32_EXIT_IO_OUT       5u  // ... and io_val = the value written
#define X32_EXIT_MISS         6u  // unimplemented: EIP unmoved, miss_len is the length
#define X32_EXIT_FAULT_UD     7u
#define X32_EXIT_FAULT_MEM    8u  // outside the arena window; fault_addr says where
#define X32_EXIT_FAULT_DIV    9u
#define X32_EXIT_FAULT_LIMIT 10u

// The EFLAGS bits a guest's arithmetic is answerable for. IF, TF, IOPL, NT, RF,
// VM and AC are not guest arithmetic and are excluded from every comparison.
#define X32_EFLAGS_ARITH_MASK 0x00000CD5u

// Register indices, in ModRM encoding order.
#define X32_EAX 0
#define X32_ECX 1
#define X32_EDX 2
#define X32_EBX 3
#define X32_ESP 4
#define X32_EBP 5
#define X32_ESI 6
#define X32_EDI 7

// Segment indices.
#define X32_ES 0
#define X32_CS 1
#define X32_SS 2
#define X32_DS 3
#define X32_FS 4
#define X32_GS 5

// (#740 doom-present) Memory-hook window, mirroring x86_16.h's mh_lo/mh_hi/
// mh_w/mh_r (see that header's comment on why the tag must be forward-declared
// before these typedefs). Added because this core had NO interception
// mechanism at all: a DOS/4GW guest's writes into the VGA aperture at 0xA0000
// landed as flat stores into the shared `mem` arena (dos/dosexec.c's "THE
// SWAP") and never reached the per-plane storage dos_present_modex() reads
// for an unchained (Mode X) guest, which presented a permanently zero-filled,
// palette-fade-tinted flat colour for every Mode X DOS/4GW title (DOOM among
// them; see CHANGELOG). width is 1, 2 or 4: this core moves pixels with
// stosb/stosw/stosd/movsd, not only byte stores.
struct x86_32_cpu_s;
typedef uint32_t (*x86_32_mem_w_fn)(struct x86_32_cpu_s *cpu, uint32_t lin, uint32_t val, int width);
typedef uint32_t (*x86_32_mem_r_fn)(struct x86_32_cpu_s *cpu, uint32_t lin, int width);
// (#740 dw2) Low-memory write watch: linear, value, width, EIP of the store.
typedef void (*x86_32_low_w_fn)(struct x86_32_cpu_s *cpu, uint32_t lin, uint32_t val,
                                int width, uint32_t eip);
// (#740 dw2) selector -> descriptor base/limit/access. Deliberately the exact
// signature of dpmi_sel_lookup_rs(), so the DPMI host's resolver is bound
// directly with no adapter to keep in step. 0 = resolved, -1 = not ours.
typedef int (*x86_32_sel_base_fn)(uint16_t sel, uint32_t *out_base,
                                  uint32_t *out_limit, uint8_t *out_ar);

typedef struct x86_32_cpu_s {
    uint32_t regs[8];
    uint32_t eip;
    uint32_t eflags;
    uint16_t seg[6];
    uint16_t pad0;
    uint16_t pad1;
    uint32_t seg_base[6];

    uint8_t *mem;          // host pointer to guest linear address mem_base
    uint32_t mem_base;
    uint32_t mem_size;

    uint64_t insn_count;
    uint32_t exit_reason;
    uint32_t exit_arg;

    uint32_t fault_eip;
    uint32_t fault_addr;

    uint32_t stop_eip;
    uint32_t stop_eip_en;

    uint32_t io_val;
    uint32_t io_size;

    uint32_t miss_count;
    uint32_t miss_op;
    uint32_t miss_op2;     // 0x100 when the 0F map was not entered
    uint32_t miss_modrm;   // 0x100 when no ModRM was consumed
    uint32_t miss_len;     // true length of the missing instruction

    void    *owner;        // opaque per-guest object for the host

    // (#740 doom-present) see the mem-hook comment above the struct tag.
    uint32_t          mh_lo, mh_hi;   // memory-hook window [lo, hi)
    x86_32_mem_w_fn   mh_w;
    x86_32_mem_r_fn   mh_r;

    // (#740 dw2) see x86_32_set_low_watch(). Diagnostic; zero = off.
    uint32_t          lw_hi, lw_pad;
    x86_32_low_w_fn   lw_cb;

    // (#740 dw2) see x86_32_set_sel_base_cb(). NULL = every selector base 0.
    x86_32_sel_base_fn sb_cb;

    // (#211) Software x87 state. See rustkern/x87.rs. ST(0) is fp[fp_top], the
    // stack grows DOWNWARD, and fp_top == 8 means empty - the SAME model as
    // exec/x86_16.c's software FPU, deliberately, so there is one thing to
    // learn rather than two. Appended at the end of the struct so that every
    // offset asserted below is unchanged by its arrival.
    uint64_t fp[8];
    uint32_t fp_top;
    uint32_t fp_cw;
    uint32_t fp_sw;
    uint32_t fp_pad;

    // (#740 digsel) Unresolved-selector accounting. See the same-named fields
    // in rustkern/x86_32.rs for what a missing descriptor silently did before
    // these existed. Appended at the end so every offset asserted below is
    // unchanged by their arrival.
    uint32_t sel_miss_n;
    uint32_t sel_miss_logged;
    uint32_t sel_miss_first_sel;
    uint32_t sel_miss_first_eip;
    uint32_t sel_gdt_n;
    uint32_t sel_pad;
} x86_32_cpu_t;

// Offsets the C side actually reads. If a field is inserted on the Rust side
// without being inserted here, one of these fires at compile time rather than
// producing a subtly wrong register dump at run time.
_Static_assert(sizeof(((x86_32_cpu_t *)0)->regs) == 32, "x86_32: regs[8] u32");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, eip) == 32, "x86_32: eip offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, eflags) == 36, "x86_32: eflags offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, seg) == 40, "x86_32: seg offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, seg_base) == 56, "x86_32: seg_base offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, mem) == 80, "x86_32: mem offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, insn_count) == 96, "x86_32: insn_count offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, exit_reason) == 104, "x86_32: exit_reason offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, stop_eip) == 120, "x86_32: stop_eip offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, miss_count) == 136, "x86_32: miss_count offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, owner) == 160, "x86_32: owner offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, mh_lo) == 168, "x86_32: mh_lo offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, mh_w) == 176, "x86_32: mh_w offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, lw_hi) == 192, "x86_32: lw_hi offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, lw_cb) == 200, "x86_32: lw_cb offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, sb_cb) == 208, "x86_32: sb_cb offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, sb_cb) == 208, "x86_32: sb_cb offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, fp) == 216, "x86_32: fp offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, fp_top) == 280, "x86_32: fp_top offset");
_Static_assert(__builtin_offsetof(x86_32_cpu_t, sel_miss_n) == 296, "x86_32: sel_miss_n offset");
_Static_assert(sizeof(x86_32_cpu_t) == 320, "x86_32: struct size");

// ---------------------------------------------------------------------------
// The Rust core.
// ---------------------------------------------------------------------------

// sizeof(X8632Cpu) as rustc computed it. The runtime lock for the asserts above.
uint32_t x86_32_cpu_size(void);

// Reset to a defined 32-bit flat state over [mem_base, mem_base+mem_size).
void x86_32_init(x86_32_cpu_t *cpu, uint8_t *mem, uint32_t mem_base, uint32_t mem_size);

// Register the VGA-aperture mem hook, mirroring x86_16_set_mem_hook().
// wfn/rfn may be NULL to clear the hook (mh_hi left at whatever hi was
// passed; pass hi=0 too if the intent is fully off).
void x86_32_set_mem_hook(x86_32_cpu_t *cpu, uint32_t lo, uint32_t hi,
                         x86_32_mem_w_fn wfn, x86_32_mem_r_fn rfn);

// Retire up to max_insns guest instructions. Returns an X32_EXIT_* code, which
// is also left in cpu->exit_reason.
uint32_t x86_32_run(x86_32_cpu_t *cpu, uint64_t max_insns);

// (#740 dw2) Arm/disarm the low-memory write watch: every guest store below
// `hi` calls `cb` before the store happens. hi = 0 disarms. Diagnostic only.
void x86_32_set_low_watch(x86_32_cpu_t *cpu, uint32_t hi, x86_32_low_w_fn cb);

// (#rafault) THE BRANCH TRACE. x86_32_btrace(1, low) arms a 256-entry ring of
// every control transfer the guest takes, plus a one-shot LATCH on the first
// transfer whose target is below `low` (a derail into the vector table). Both
// are printed by x86_32_btrace_dump(n). Diagnostic; off unless armed, and the
// golden never arms it. See the ring at the foot of rustkern/x86_32.rs.
void x86_32_btrace(uint32_t on, uint32_t low);
uint32_t x86_32_btrace_on(void);
void x86_32_btrace_dump(uint32_t n);

// (#740 dw2) Bind the selector-to-base resolver used by every segment-register
// load. Pass dpmi_sel_lookup_rs. NULL means every selector has base 0, which is
// what this core assumed unconditionally before the resolver existed.
void x86_32_set_sel_base_cb(x86_32_cpu_t *cpu, x86_32_sel_base_fn cb);

// ---------------------------------------------------------------------------
// Asynchronous interrupt delivery: the direction the core did not have.
//
// x86_32_run() reports the interrupts a guest EXECUTES. This delivers one the
// guest is WAITING TO RECEIVE: it builds the interrupt-gate frame (EFLAGS, CS,
// EIP), clears IF, and points EIP at the handler. The handler then runs in the
// caller's ordinary x86_32_run() slices and returns through IRETD, so there is
// no nested interpreter and no second budget.
//
// CALL IT ONLY IMMEDIATELY AFTER x86_32_run() RETURNS. That is the instruction
// boundary; the core has no other exit, so no caller can get this wrong.
// ---------------------------------------------------------------------------
#define X32_INJ_DELIVERED   0   // frame pushed, EIP is at the handler
#define X32_INJ_MASKED      1   // EFLAGS.IF is clear; nothing done, NOT an error
#define X32_INJ_FAULT     (-1)  // the frame did not fit; ESP and EIP unchanged
int x86_32_inject_int(x86_32_cpu_t *cpu, uint32_t handler);

// (#740 digplay) A far CALL into the guest, for the one thing an interrupt
// frame cannot express: an INT 33h 0Ch/14h mouse event handler, which a driver
// CALLS and which returns with RETF, not IRET. Pushes CS then EIP (so the
// matching RETF pops EIP then CS), leaves EFLAGS alone and does NOT clear IF,
// and sets EIP to `handler`. `ret_eip` is the address the handler will RETF to;
// set cpu->stop_eip to it first so the return is DETECTED rather than executed.
// Same X32_INJ_* return codes, same instruction-boundary rule.
int x86_32_inject_farcall(x86_32_cpu_t *cpu, uint32_t handler, uint32_t ret_eip);

// Bounds-checked guest memory access for the host. Use these; never index
// cpu->mem directly, because the whole security property of this subsystem is
// that a guest-controlled 32-bit address cannot escape the window.
// Return 0 on success, -1 if the range is outside it.
int x86_32_read_guest(const x86_32_cpu_t *cpu, uint32_t la, uint8_t *dst, uint32_t len);
int x86_32_write_guest(x86_32_cpu_t *cpu, uint32_t la, const uint8_t *src, uint32_t len);

// ---------------------------------------------------------------------------
// The oracle self-test (exec/x86_32_test.c). Runs every vector in
// exec/x86_32_vectors.h, which was generated by executing those exact bytes on
// real 32-bit silicon. Returns the number of MISMATCHING vectors; 0 is a pass.
// Prints one summary line and a detail block per failure.
// ---------------------------------------------------------------------------
int x86_32_oracle_selftest(void);

// The end-to-end demonstration: a 32-bit protected-mode program that prints a
// string through INT 21h AH=09h and exits through AH=4Ch, driven by the
// run/exit loop a DPMI host will use. Returns 0 if it printed and exited.
int x86_32_hello_selftest(void);

// (#211) The x87 unit's self-test: runs the EXACT instruction sequence djgpp's
// __detect_80387 executes, plus the load/store/arithmetic forms, against a
// caller-supplied CPU. Returns the number of FAILING checks and writes the
// number that RAN to *out_checks, so "passed" and "never ran" are different
// answers (#514). The cpu is left with a reset FPU.
//
// TAKE THE CPU FROM THE HEAP OR FROM AN EXISTING GUEST, NEVER FROM THE STACK:
// x86_32_cpu_t is ~300 bytes today but the DOS launch path is already deep and
// this tree has a #212-shaped scar from a self-test buffer on that stack.
uint32_t x87_selftest_rs(x86_32_cpu_t *cpu, uint32_t *out_checks);

// Proves the MISS policy fires and reports the RIGHT length: EIP is left on the
// instruction and skipping by miss_len lands exactly on the next one. Returns
// the number of failures; 0 is a pass.
int x86_32_miss_selftest(void);

#endif // EXEC_X86_32_H
