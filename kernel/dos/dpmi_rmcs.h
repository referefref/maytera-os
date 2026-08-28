// dos/dpmi_rmcs.h - #740: DPMI 0300h (simulate real-mode interrupt), C surface.
//
// The marshaller itself is Rust (rustkern/dpmi_rmcs.rs); read its header
// comment for the DPMI semantics, the 50-byte RMCS layout and why this function
// contains no DOS service knowledge. This header is what an INT 31h host
// dispatcher calls, and what dos/int21svc.h's "HOW A DPMI HOST FOR DOS/4GW
// ATTACHES" recipe (step 4) refers to.
//
// USING THIS FROM AN INT 31h HOST, in full:
//
//   dpmi_arena_t arena = { .base = guest_first_megabyte, .size = 0x100000 };
//   dos_svc_ctx_t ctx;                       // one per DOS/4GW guest
//   dos_svc_ctx_init(&ctx, GUESTFS_SLOT_DPMI, "dpmi");
//   dpmi_rmcs_bind_arena(&ctx, &arena);      // ctx->mem -> the Rust chokepoint
//   ctx.con.putc = <the guest's stdout>;
//   ...
//   // on INT 31h with AX == 0300h:
//   x86_16_cpu_t frame;  memset(&frame, 0, sizeof frame);
//   int rc = dpmi_rmcs_call_rs(&arena, es_edi_flat, guest_bx, &frame,
//                              dpmi_rmcs_dos_dispatch, &ctx,
//                              host_stack_seg, host_stack_sp);
//   // rc < 0  -> the SIMULATION failed (a bad RMCS pointer): INT 31h returns
//   //            CF=1 to the guest.
//   // rc == 0 -> the simulation ran: INT 31h returns CF=0. Whether the
//   //            INTERRUPT succeeded is in the RMCS flags, where the guest
//   //            looks for it.
//
// THE TWO CARRY FLAGS ARE NOT THE SAME CARRY FLAG. That distinction is the one
// thing an implementer of a DPMI host gets wrong: `rc` is about the DPMI call,
// the RMCS flags word is about the simulated interrupt.
#ifndef DOS_DPMI_RMCS_H
#define DOS_DPMI_RMCS_H

#include "../types.h"
#include "int21svc.h"

struct x86_16_cpu;

// ---------------------------------------------------------------------------
// The DPMI real-mode call structure. 50 bytes, packed, no alignment holes.
// Every offset is locked against rustkern/dpmi_rmcs.rs by _Static_assert in
// dos/dpmi_rmcs.c, in BOTH directions, so a silent divergence cannot build.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t edi;       // 00h
    uint32_t esi;       // 04h
    uint32_t ebp;       // 08h
    uint32_t reserved;  // 0Ch  never read, never written
    uint32_t ebx;       // 10h
    uint32_t edx;       // 14h
    uint32_t ecx;       // 18h
    uint32_t eax;       // 1Ch
    uint16_t flags;     // 20h  note: AFTER the eight dwords, BEFORE ES
    uint16_t es;        // 22h
    uint16_t ds;        // 24h
    uint16_t fs;        // 26h
    uint16_t gs;        // 28h
    uint16_t ip;        // 2Ah  ignored on entry for 0300h, never written back
    uint16_t cs;        // 2Ch  ignored on entry for 0300h, never written back
    uint16_t sp;        // 2Eh  0:0 with SS means "host supplies a stack"
    uint16_t ss;        // 30h
} __attribute__((packed)) dpmi_rmcs_t;

// ---------------------------------------------------------------------------
// The guest's real-mode-addressable memory, and THE bounds-check chokepoint.
// `oob_rd`/`oob_wr` count accesses that left the arena; they are refused, never
// clamped and never folded into a spare page (docs/DPMI_BRIDGE_DESIGN.md 3.10).
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *base;
    uint32_t size;
    uint32_t oob_rd;
    uint32_t oob_wr;
} dpmi_arena_t;

// The host's service router. Returns non-zero if it serviced `intno`, zero if
// nothing implements it (which the bridge turns into a logged MISS with the
// correct register and flag effect, never a silent success).
typedef int (*dpmi_rm_dispatch_fn)(void *user, uint8_t intno, struct x86_16_cpu *frame);

// ---- rustkern/dpmi_rmcs.rs -----------------------------------------------
// `rmcs_limit` bounds THE RMCS BLOCK, which is a PROTECTED-MODE address in the
// client's flat space and is normally far above the 1 MiB `arena->size` window
// that bounds the real-mode seg:off addresses inside it. Pass the client's full
// arena size; 0 means "same as arena->size". See rustkern/dpmi_rmcs.rs for why
// the two limits are different numbers (a real measurement on Discworld II).
int  dpmi_rmcs_call_rs(dpmi_arena_t *arena, uint32_t rmcs_flat, uint32_t rmcs_limit,
                       uint16_t bx,
                       struct x86_16_cpu *frame, dpmi_rm_dispatch_fn dispatch,
                       void *user, uint16_t stack_seg, uint16_t stack_sp);
uint8_t  dpmi_arena_rd8_rs (void *u, uint16_t seg, uint16_t off);
void     dpmi_arena_wr8_rs (void *u, uint16_t seg, uint16_t off, uint8_t v);
uint16_t dpmi_arena_rd16_rs(void *u, uint16_t seg, uint16_t off);
void     dpmi_arena_wr16_rs(void *u, uint16_t seg, uint16_t off, uint16_t v);
void     dpmi_rmcs_stats_rs(uint32_t *calls, uint32_t *miss, uint32_t *hoststack);
int      dpmi_rmcs_layout_selftest_rs(uint32_t *out_checks);

// dpmi_rmcs_call_rs return codes (negative = the SIMULATION failed).
#define DPMI_RMCS_OK       0
#define DPMI_RMCS_EARENA  (-1)   // no arena / no frame: a host bug
#define DPMI_RMCS_EBOUNDS (-2)   // the RMCS block is not inside the arena

// ---- dos/dpmi_rmcs.c ------------------------------------------------------

// Point a service context's guest-memory accessors at `arena`, i.e. at the Rust
// chokepoint above. This is the DPMI twin of dos_svc_bind_x86_16(): the service
// core never computes a linear address itself, so a guest whose memory is a
// flat arena rather than an interpreter's 1 MiB array needs nothing else.
void dpmi_rmcs_bind_arena(dos_svc_ctx_t *ctx, dpmi_arena_t *arena);

// THE DEFAULT SERVICE ROUTER. `user` is a dos_svc_ctx_t *.
//
// INT 21h goes to dos_svc_int21(), THE one service core (#736 Stage 1), and
// nothing else is routed. That is not a gap to be filled with a copy of
// anything: INT 10h/16h/33h currently live inside dos/dosexec.c as statics
// bound to dos_task_t, so until they are factored out the honest answer for
// them is a logged MISS with a correct stub effect, which is diagnosable, and
// not a second implementation, which is not.
int dpmi_rmcs_dos_dispatch(void *user, uint8_t intno, struct x86_16_cpu *frame);

#endif // DOS_DPMI_RMCS_H
