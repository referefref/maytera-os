// dosexec.c - MS-DOS real-mode program loader + runner (#201)
//
// Goal: run real-mode 16-bit MS-DOS games (e.g. THE INCREDIBLE MACHINE, TIM.EXE)
// in a window on MayteraOS, using the existing x86_16 real-mode interpreter
// (exec/x86_16.c). This file provides:
//   - an MZ .EXE loader (header parse + relocation) and a .COM loader
//   - a PSP at the load segment so DOS programs have a valid environment
//   - INT 21h: a usable DOS API subset (file I/O on FAT, memory, dir, exit, ...)
//   - INT 10h: VGA BIOS, in particular set-mode 13h (320x200x256)
//   - INT 33h: Microsoft mouse driver subset (from the kernel cursor)
//   - INT 16h / INT 21h key fns: keyboard from the kernel keyboard buffer
//   - I/O port hooks: VGA DAC palette (0x3C8/0x3C9) + status reads (0x3DA)
//   - a present loop: expand 0xA0000 (320x200x8) through the palette into a
//     2x-scaled ARGB host-window content buffer the compositor draws.
//
// The interpreter exposes a single global int handler + io handlers, and win16
// already owns those while a Win16 app runs. DOS and Win16 are mutually
// exclusive at runtime (one foreground 16-bit task), which matches how the OS
// launches them (own kernel proc, one at a time). We use a private cpu + memory.

#include "dosexec.h"
#include "diskimg.h"
#include "dospath.h"
#include "int21svc.h"   // #736: THE one INT 21h service core
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"     // #708: R_OK/W_OK/X_OK
#include "../fs/guestfs.h"   // #708: the DOS/Win16 guest filesystem gate
#include "../exec/x86_16.h"
#include "../video/font.h"
#include "../cpu/mono.h"   // sched_now_ms(): the shared monotonic clock
#include "../drivers/keyboard.h"  // KEY_MOD_* and the shared scancode-to-char table

// ---- kernel imports ------------------------------------------------------
extern fat_fs_t g_fat_fs;
extern void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out);
extern int  proc_create(const char *name, void (*entry)(void *), void *arg, int prio);
extern void proc_sleep(uint32_t ms);
extern void proc_yield(void);

struct window;
extern int  win16_host_create(const char *title, int x, int y, int w, int h,
                              uint32_t **out_buf, int *out_w, int *out_h,
                              struct window **out_win);
extern int  win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh);
extern void win16_host_invalidate(int slot);
extern void win16_host_destroy(int slot);
extern void win16_host_route_close_to_dos(int slot);

// Global kernel input state (drivers/mouse.c, drivers/keyboard.c).
extern int32_t mouse_x;
extern int32_t mouse_y;
extern uint8_t mouse_buttons;
extern int keyboard_has_char(void);
extern int keyboard_get_char(void);

// Raw scancode tap (cpu/isr.c) for DOS games (#202).
extern volatile int g_dos_scancode_tap;
extern int  dos_scancode_get(void);
extern void dos_scancode_clear(void);
// Reused, NOT re-implemented: the PS/2 driver already owns the scancode tables
// and the live modifier state. A private copy in the DOS layer would be a second
// keymap to keep in step with the first.
extern char     keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers);
extern uint32_t keyboard_get_modifiers(void);

extern void x86_16_request_stop(void);

// Forward decls for the mem-hook trampolines (defined below).
struct x86_16_cpu;
static void     ega_mem_w(struct x86_16_cpu *c, uint32_t lin, uint16_t val, int width);
static uint16_t ega_mem_r(struct x86_16_cpu *c, uint32_t lin, int width);

#ifndef PRIO_NORMAL
#define PRIO_NORMAL 2   // #385 real enum: IDLE=0,LOW=1,NORMAL=2,HIGH=3
#endif
#ifndef PRIO_HIGH
#define PRIO_HIGH 3
#endif

// ---- DOS task state ------------------------------------------------------
// ---- guest pacing --------------------------------------------------------
// The interpreter runs in bursts of DOS_SLICE_INSNS instructions with a
// DOS_SLICE_SLEEP_MS sleep between them, so the guest's own instruction count,
// NOT wall time, is the only monotonic clock that is uniform inside a burst.
// THROUGHPUT. 100000 insns then a 15 ms sleep gave the guest about a FIFTH of
// what the interpreter can do: the burst takes roughly 4 ms of the 19 ms round
// trip, so a measured ~26 M insn/s interpreter delivered ~5.3 M insn/s to the
// guest. That ceiling, not the emulation itself, is why a busy title feels
// laggy while a light one does not. A longer burst with a shorter yield keeps
// the same ~60 present/input cycles a second while raising the guest's share.
// ---- ADAPTIVE PACING (replaces the two fixed constants) -------------------
// The pacing WAS two fixed constants: run 250,000 instructions, then
// proc_sleep(5). Two constants multiplied together are wrong at every load they
// were not measured at, and this pair was wrong at the load that matters.
// MEASURED on build 1730, Commander Keen 5 on a Xeon Gold 6248: the guest got
// 12.8-14.6 M insn/s out of an interpreter that does 25.8-26.7 M on the same
// hardware, and the scheduler reported idle:45 over the SAME interval. The
// missing half did not go to the compositor or to anything else. The DOS thread
// slept through it while the ready queue was empty.
//
// The replacement states a target and closes the loop on it:
//
//   SPEED  - each burst runs for DOS_SLICE_MS of WALL CLOCK. The instruction
//            count that takes is recomputed from the MEASURED delivered rate
//            (the dos_emu_hz() sampler, which already existed), so it
//            self-corrects across hosts and across guest code of different
//            cost instead of assuming a number.
//   YIELD  - after every burst the thread calls proc_yield(): a HANDOFF, not a
//            sleep. Anything else runnable runs immediately; an empty ready
//            queue hands the core straight back, so idle time goes to the guest
//            rather than to hlt. There is no fixed sleep left to be wrong.
//
// Everything the GUEST can observe about time is deliberately NOT derived from
// these: see dos_emu_pit_now(). Pacing may change; guest speed must not.
#define DOS_SLICE_MS       4             // wall-clock ms of interpretation per burst
#define DOS_SLICE_MIN      20000UL       // floor: always make real forward progress
#define DOS_SLICE_MAX      4000000UL     // ceiling: bound input/present latency
#define DOS_PRESENT_MS     14            // ~70 Hz present cadence (see dos_present call)
#define DOS_RATE_SAMPLE_MS 200           // re-measure the delivered rate this often
#define DOS_MAX_RUN_MS     (6UL*3600UL*1000UL)  // runaway cap, stated in the unit it means
// Seed only, and only for the first DOS_RATE_SAMPLE_MS of a run: from then on
// the rate is measured, never assumed. It used to be DERIVED from the slice
// constants, which assumed a burst cost nothing and overstated the rate ~4x.
#define DOS_EMU_INSN_HZ    20000000UL
#define DOS_PIT_HZ         1193182UL     // 8253/8254 input clock

#define DOS_MEM_SIZE   0x100000          // 1 MiB real-mode address space
#define DOS_PSP_SEG    0x0100            // PSP paragraph (so image loads at 0x0110)
#define DOS_LOAD_SEG   (DOS_PSP_SEG + 0x10) // program load segment (PSP is 0x100 bytes)
#define VGA_A000       0xA0000           // mode-13h linear framebuffer base (linear)
#define VGA_A000_END   0xB0000           // end of the 64KB EGA aperture
#define MODE13_W       320
#define MODE13_H       200
#define WIN_SCALE      2                 // 320x200 -> 640x400 on screen

// EGA mode 0Dh (320x200x16, 4 planar bitplanes). Used by Commander Keen and
// other id Galaxy-engine games. Each plane is 64KB; a CPU byte at 0xA0000+off
// maps to bit (7-(x&7)) across the 4 planes for pixel x = off*8 + (7-bit).
#define EGA_PLANE_SIZE 0x10000           // 64KB per plane

// #736: the file-handle table, the DTA and the find cursor used to live here.
// They are per-guest STATE, so they moved into dos_svc_ctx_t (dos/int21svc.h)
// and are now shared with the Win16 guest layer instead of duplicated by it.

// A real MCB (memory control block) table for INT 21h 48h/49h/4Ah.
// alloc_top_para alone was a bump pointer fixed at load time, so 4Ah always
// answered "yes, and you may grow to 0xA000 - ES". A runtime that asked to grow
// its own PSP block believed it, then far-malloc'd from an address INSIDE its
// own DGROUP and destroyed its near-heap free list.
#define DOS_MAX_MCB 512
typedef struct {
    uint16_t seg;     // block start paragraph
    uint16_t para;    // block size in paragraphs
    int      live;    // cleared by 49h
} dos_mcb_t;

typedef struct {
    x86_16_cpu_t cpu;
    uint8_t     *mem;                     // 1 MiB
    // #736: THE per-guest INT 21h state (handle table, DTA, find cursor,
    // per-drive CWD, PSP segment, identity slot). The DOS task owns one; the
    // Win16 layer owns another; a future DPMI host owns a third. The SERVICE
    // code that acts on them exists exactly once, in dos/int21svc.c.
    dos_svc_ctx_t svc;
    char         appdir[128];             // dir of the .EXE for relative opens
    uint16_t     alloc_top_para;          // next free paragraph for INT 21h 48h
    uint16_t     alloc_floor_para;        // load-time top; the bump never drops below it
    dos_mcb_t    mcb[DOS_MAX_MCB];        // live blocks handed out by 48h (+ the PSP block)
    int          mcb_n;

    // PIT channel 0 (ports 0x40/0x43). Driven from cpu.insn_count.
    uint16_t     pit_divisor;             // 0 means 65536
    uint16_t     pit_latch;               // value captured by a counter-latch command
    int          pit_latched;
    int          pit_rd_hi, pit_wr_hi;    // lo/hi byte toggles
    uint8_t      pit_access;              // RW field of the last control word (1,2,3)

    // VGA / mode 13h
    int          video_mode;             // current INT 10h mode (0x13 = mode 13, 0x0D = EGA)
    int          gfx_w, gfx_h;           // active graphics resolution
    // Text mode 03h state. The page itself lives in guest RAM at B800:0000 as
    // 80x25 (char, attribute) pairs, exactly as on real hardware, so a program
    // that pokes B800 directly and a program that goes through the BIOS/DOS both
    // land in the same place and dos_present_text() has one thing to draw.
    uint8_t      cur_row, cur_col;       // BIOS cursor position (page 0)
    uint8_t      text_attr;              // attribute used by BIOS/DOS TTY writes
    uint8_t      pal[256][3];             // 6-bit DAC palette (r,g,b 0..63)
    uint16_t     dac_widx, dac_ridx;      // DAC write/read index latches
    int          dac_phase;               // 0=r,1=g,2=b within a triplet

    // EGA planar framebuffer (mode 0Dh). 4 hidden bitplanes; CPU sees one
    // address space at 0xA0000 but writes/reads are filtered by the VGA
    // sequencer + graphics-controller registers.
    uint8_t      ega_plane[4][EGA_PLANE_SIZE];
    uint8_t      ega_latch[4];           // per-plane read latches
    uint8_t      seq_idx;                // 0x3C4 index latch
    uint8_t      seq_map_mask;           // SEQ reg 2: which planes a write targets
    uint8_t      gc_idx;                 // 0x3CE index latch
    uint8_t      gc_set_reset;           // GC reg 0
    uint8_t      gc_en_set_reset;        // GC reg 1
    uint8_t      gc_color_cmp;           // GC reg 2
    uint8_t      gc_data_rotate;         // GC reg 3 (rotate count + function bits 3-4)
    uint8_t      gc_read_map;            // GC reg 4: plane selected for reads (mode 0)
    uint8_t      gc_mode;                // GC reg 5 (write mode 0-3 in bits 0-1, read mode bit 3)
    uint8_t      gc_misc;                // GC reg 6
    uint8_t      gc_color_dont_care;     // GC reg 7
    uint8_t      gc_bit_mask;            // GC reg 8
    // Attribute controller (0x3C0): 16 EGA palette regs -> 6-bit colour index
    uint8_t      atc_idx;                // 0x3C0 index latch
    int          atc_flipflop;          // 0=index next, 1=data next
    uint8_t      atc_pal[16];            // EGA palette registers (index into DAC)
    uint8_t      atc_reg[32];            // the REST of the attribute controller:
                                         // 0x10 mode control, 0x11 overscan,
                                         // 0x12 colour plane enable,
                                         // 0x13 horizontal pixel panning,
                                         // 0x14 colour select. Indices 16-31 used
                                         // to be parsed and then DISCARDED, which
                                         // is why smooth horizontal scrolling was
                                         // quantised to 8 pixels.
    // CRTC (0x3D4 index / 0x3D5 data, mono mirror 0x3B4/0x3B5). Backed as a
    // plain register file so VGA-detection read-after-write probes succeed.
    uint8_t      crtc_idx;
    uint8_t      crtc[32];
    uint8_t      misc_out;               // Misc Output register (0x3C2 write / 0x3CC read)
    uint8_t      seq_reg[8];             // full sequencer register file (for readback)
    int          ega_dirty;             // a plane write happened since last present

    // Keyboard hardware emulation for INT 9 delivery (#202 Keen).
    uint8_t      kbd_port60;            // last scancode latched at port 0x60
    int          kbd_has_int9;         // guest installed its own INT 9 vector
    int          has_int8;             // guest installed its own INT 8 (timer) vector
    int          has_int1c;            // guest installed its own INT 1Ch (BIOS user tick)
    uint32_t     int8_accum;           // accumulator for INT 8 rate division

    // ---- EMULATED TIMEBASE (guest time must not depend on host pacing) ----
    // ONE monotonic clock, counted in 1.193182 MHz PIT ticks, drives everything
    // the guest can use to tell the time: PIT counter reads on ports 0x40/0x43,
    // IRQ0/INT 8 delivery, and the BIOS 18.2 Hz tick at 0040:006C.
    //
    // Before this, IRQ0 was delivered ONCE PER SLICE and the BIOS tick was
    // incremented once per slice, so the guest's sense of time was a function of
    // the host's pacing constants: at ~50 slices/s a Galaxy-engine game that
    // programs the PIT for 70 Hz ran its game clock at 5/7 speed, and the BIOS
    // tick ran 2.7x fast. Any change to the pacing silently changed the speed of
    // every DOS program. Deriving both from insn_count against the MEASURED
    // instruction rate makes emulated time track real time whatever the pacing.
    //
    // emu_pit_base/emu_insn_base exist so that re-measuring the rate REBASES the
    // clock instead of rescaling it. Recomputing ticks as insn_count*HZ/rate with
    // a fresh rate moves every past instant, and a rate that went UP moves time
    // BACKWARDS: a guest delay loop waiting for the counter to pass a threshold
    // would then hang. See dos_emu_rebase().
    uint64_t      emu_pit_base;        // PIT ticks accumulated before emu_insn_base
    unsigned long emu_insn_base;       // insn_count at which emu_pit_base was taken
    uint64_t      next_irq0_pit;       // emulated PIT tick at which IRQ0 fires next
    uint32_t      bios_tick_last;      // last 18.2 Hz tick written to 0040:006C


    // Mouse (INT 33h) state, in mode-13h virtual coords
    int          mouse_on;
    int          mx, my, mbtn;            // current
    int          mouse_initialized;

    // window host
    int          host_slot;
    uint32_t    *win_buf;                 // ARGB content buffer
    int          win_w, win_h;            // content buffer size

    volatile int running;
} dos_task_t;

static dos_task_t g_dos;                  // single foreground DOS task
// Command tail for the next launch (PSP:0080). Plenty of DOS titles document a
// command-line switch as the ONLY way past some startup path, and until now the
// PSP tail was hard-coded empty so none of them could be reached.
static char g_dos_cmdtail[128] = "";
static volatile int g_dos_busy = 0;

// Standard 16-colour EGA/VGA default palette (6-bit DAC values per the default
// attribute-controller mapping). Defined here so INT 10h mode-set + present share it.
static const uint8_t ega_default_dac[16][3] = {
    { 0, 0, 0},{ 0, 0,42},{ 0,42, 0},{ 0,42,42},
    {42, 0, 0},{42, 0,42},{42,21, 0},{42,42,42},
    {21,21,21},{21,21,63},{21,63,21},{21,63,63},
    {63,21,21},{63,21,63},{63,63,21},{63,63,63},
};

// EGA/VGA attribute-controller palette register -> one of the 16 standard
// colours.
//
// MEASURED, not assumed. Keen 5 sets its palette with INT 10h AH=10h AL=02h and
// the table 00 01 02 03 04 05 06 07 18 19 1A 1B 1C 1D 1E 1F, and the DOSBox
// reference renders that as the 16 standard colours in order. We treated the
// register value as a DAC index into a table that was only ever seeded for
// 0x00-0x0F, so every value in 0x18-0x1F landed in the leftover grayscale ramp
// (pal[i] = i >> 2) and ALL EIGHT BRIGHT COLOURS came out near-black: measured
// 8 distinct colours on screen against DOSBox's 15, with #1C1C1C and #181818
// standing in for white, light gray, dark gray and the five bright hues. That is
// the "black where it should be a lighter colour or white" the user reported.
//
// The register is 6 bits, r'g'b'RGB. In the 200-line modes the adapter drives a
// CGA-compatible I/R/G/B, taking Intensity from bit 4 and R/G/B from bits 2/1/0.
// That is the rule below, and it ALSO handles the other common encoding (the
// 0x38-0x3F bright half used by 350-line code and by the BIOS default) because
// those values have bit 4 set too. 0x14 is the one documented exception, the
// 350-line "brown fix", so it is special-cased.
static uint8_t ega_pal_to_index(uint8_t v) {
    if ((v & 0x3F) == 0x14) return 6;                       // brown
    return (uint8_t)((v & 0x07) | ((v & 0x10) >> 1));
}

// The attribute-controller palette the BIOS leaves after setting a 16-colour
// mode. This used to be seeded as the IDENTITY 0..15, which is not what any
// adapter does and which made attributes 8-15 depend on DAC entries 8-15 that
// nothing had a reason to program.
static const uint8_t ega_atc_default[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

// ---- small helpers -------------------------------------------------------
static inline uint8_t  rd8 (dos_task_t *t, uint16_t s, uint16_t o){ return x86_16_rd8 (&t->cpu,s,o);}
static inline uint16_t rd16(dos_task_t *t, uint16_t s, uint16_t o){ return x86_16_rd16(&t->cpu,s,o);}
static inline void     wr8 (dos_task_t *t, uint16_t s, uint16_t o, uint8_t v){ x86_16_wr8 (&t->cpu,s,o,v);}
static inline void     wr16(dos_task_t *t, uint16_t s, uint16_t o, uint16_t v){ x86_16_wr16(&t->cpu,s,o,v);}

#define SET_CF(c)   ((c)->flags |= 0x0001)
#define CLR_CF(c)   ((c)->flags &= ~0x0001)
#define AH_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0x00FF) | ((v) << 8)))
#define AL_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0xFF00) | ((v) & 0xFF)))
#define AH(c)       ((uint8_t)((c)->ax >> 8))
#define AL(c)       ((uint8_t)((c)->ax & 0xFF))

// #736: dos_fs_allow(), rd_asciiz(), dos_path_exists(), dos_native_fallback(),
// dos_to_fat_path(), dos_fh_alloc(), dos_upper(), dos_wild_match(),
// fat11_to_dotname(), dos_write_find_result() and dos_find_step() ALL moved to
// dos/int21svc.c. Every one of them had a near-twin in exec/ne.c or
// exec/win16api.c. They are now singular, and this file no longer owns any
// filesystem policy at all: it owns the MACHINE (video, keyboard, PIT, MCBs)
// and hands the API surface to the service core.

// ---- DOS INT 21h ---------------------------------------------------------
extern volatile int g_x86_dbgring;   // #385: reuse DOSDIAG.CFG gate for verbose keen traces
static int g_dos_trace21 = 0;   // #385 diag   // #202 bring-up: log every INT 21h call (off for ship)
// /CONFIG/DOSRING.CFG: turn on the interpreter instruction ring and dump it at
// INT 21h 4Ch. Deliberately a SEPARATE gate from DOSDIAG.CFG: recording every
// instruction perturbs timing, and the Keen/TIM regression runs must not pay for
// a diagnostic they are not using. The file's contents are the number of
// instructions to dump (default 400).
static int g_dos_ring_on = 0;
static int g_dos_ring_dump_n = 400;
static int g_dos_ring_dumped = 0;
static volatile int g_dos_sstep = 0;   // #202: single-step N instructions when >0
// ---- MCB helpers ---------------------------------------------------------
static dos_mcb_t *dos_mcb_find(dos_task_t *t, uint16_t seg) {
    for (int i = 0; i < t->mcb_n; i++)
        if (t->mcb[i].live && t->mcb[i].seg == seg) return &t->mcb[i];
    return NULL;
}

// Lowest live block strictly above `seg`, or 0xA000 (the VGA aperture) if none.
// This is what makes 4Ah's maxpara truthful instead of "everything to 640K".
static uint16_t dos_mcb_next_above(dos_task_t *t, uint16_t seg) {
    uint16_t best = 0xA000;
    for (int i = 0; i < t->mcb_n; i++) {
        if (!t->mcb[i].live) continue;
        if (t->mcb[i].seg > seg && t->mcb[i].seg < best) best = t->mcb[i].seg;
    }
    return best;
}

// Move the bump pointer above every live block. Clamped at the load-time top so
// it can never drop into the program image or its stack: that clamp is what
// keeps this strictly conservative for titles that already worked.
static void dos_mcb_retop(dos_task_t *t) {
    uint16_t top = t->alloc_floor_para;
    for (int i = 0; i < t->mcb_n; i++) {
        if (!t->mcb[i].live) continue;
        uint16_t end = (uint16_t)(t->mcb[i].seg + t->mcb[i].para);
        if (end > top) top = end;
    }
    t->alloc_top_para = top;
}

// Returns 0 on success, -1 if the table is full. The caller MUST fail the
// allocation on -1. Dropping the record instead is not a benign degradation:
// an unrecorded block does not move the bump pointer, so the very next 48h
// hands out the SAME segment again. A 64-entry table did exactly that to
// TIM.EXE, which made 312 allocations against 50 frees and got segment 0x5b29
// back 207 times, reintroducing the double-allocation this table exists to fix.
// Lowest paragraph at or above the load-time floor where `para` paragraphs are
// free, i.e. no LIVE block overlaps [start, start+para) and the block still ends
// below 0xA000. Returns 0 when there is no such hole.
//
// Why this exists: INT 21h 48h was a pure BUMP allocator over dos_mcb_retop(),
// which sets the top above every LIVE block. That means a block freed by 49h
// BELOW a live one never lowers the bump pointer, so its paragraphs are gone for
// the rest of the run. bats25 frees five 32 KB far-heap segments and then cannot
// get 1.3 KB back, dying in its own "Out of memory in Malloc_Frame_And_Mask."
// The scan is O(live blocks) per step, so it is only used when the bump path has
// already failed: the common allocate-and-keep case still takes the fast path
// and existing titles see byte-identical behaviour.
static uint16_t dos_mcb_first_fit(dos_task_t *t, uint16_t para) {
    uint32_t start = t->alloc_floor_para;
    while (start + para <= 0xA000u) {
        uint32_t bump = 0;
        for (int i = 0; i < t->mcb_n; i++) {
            if (!t->mcb[i].live || t->mcb[i].para == 0) continue;
            uint32_t bs = t->mcb[i].seg, be = bs + t->mcb[i].para;
            if (start < be && (start + para) > bs && be > bump) bump = be;
        }
        if (!bump) return (uint16_t)start;
        start = bump;
    }
    return 0;
}

static int dos_mcb_add(dos_task_t *t, uint16_t seg, uint16_t para) {
    for (int i = 0; i < t->mcb_n; i++) {
        if (t->mcb[i].live) continue;
        t->mcb[i].seg = seg; t->mcb[i].para = para; t->mcb[i].live = 1;
        return 0;
    }
    if (t->mcb_n < DOS_MAX_MCB) {
        t->mcb[t->mcb_n].seg = seg; t->mcb[t->mcb_n].para = para;
        t->mcb[t->mcb_n].live = 1; t->mcb_n++;
        return 0;
    }
    return -1;
}

// ---- PIT channel 0 -------------------------------------------------------
// Current value of the channel-0 down-counter, derived from the guest's own
// instruction count. A wall clock would be wrong here: the interpreter runs in
// DOS_SLICE_INSNS bursts with a sleep between them, so a slice boundary landing
// inside a delay-loop calibration interval makes the measured delta nonsense.
// The guest's ACTUAL instruction rate, measured over the run rather than assumed
// from the slice constants. Sampled once per slice by the run loop; until there
// is a sample, fall back to the derived constant.
static uint32_t g_dos_emu_hz = 0;

static uint32_t dos_emu_hz(void) {
    return g_dos_emu_hz ? g_dos_emu_hz : (uint32_t)DOS_EMU_INSN_HZ;
}

// The emulated clock, in PIT ticks. Monotonic BY CONSTRUCTION: only the part of
// the run since the last rebase is converted at the current rate.
static uint64_t dos_emu_pit_now(dos_task_t *t) {
    unsigned long d = t->cpu.insn_count - t->emu_insn_base;
    return t->emu_pit_base + ((uint64_t)d * DOS_PIT_HZ) / dos_emu_hz();
}

// Adopt a newly measured instruction rate WITHOUT moving any instant that has
// already happened: freeze the elapsed ticks at the old rate, then switch.
static void dos_emu_rebase(dos_task_t *t, uint32_t new_hz) {
    if (!new_hz) return;
    t->emu_pit_base  = dos_emu_pit_now(t);
    t->emu_insn_base = t->cpu.insn_count;
    g_dos_emu_hz     = new_hz;
}

static uint16_t dos_pit_count(dos_task_t *t) {
    uint32_t div = t->pit_divisor ? t->pit_divisor : 65536u;
    uint32_t phase = (uint32_t)(dos_emu_pit_now(t) % div);
    return (uint16_t)(div - phase);
}


// ---- text mode 03h: the 80x25 character/attribute page at B800 -----------
// dos_present() used to render ONLY mode 13h and the EGA planar modes, so any
// DOS program sitting at a text prompt or a nag screen showed a blank window
// and looked wedged when it was fine. That cost diagnosis time on three
// separate titles. The page is plain guest RAM, so nothing here is a shadow
// copy: direct B800 writes, INT 10h TTY and INT 21h stdout all mutate the one
// buffer that dos_present_text() draws.
// One definition of "this is a text mode", so the diagnostic dump, the
// renderer and INT 10h AH=0Fh can never disagree about it again.
static inline int dos_text_is(const dos_task_t *t) {
    return t->video_mode <= 0x03 || t->video_mode == 0x07;
}
#define TEXT_COLS   80
#define TEXT_ROWS   25
#define VGA_B800    0xB8000u

static inline uint32_t dos_text_cell(int row, int col) {
    return VGA_B800 + (uint32_t)((row * TEXT_COLS + col) * 2);
}

// Mirror the cursor into the BIOS data area, so a program that reads 0040:0050
// directly (plenty do) agrees with INT 10h AH=03h and with what we draw.
static void dos_text_sync_bda(dos_task_t *t) {
    wr8(t, 0x0040, 0x0050, t->cur_col);
    wr8(t, 0x0040, 0x0051, t->cur_row);
}

static void dos_text_fill(dos_task_t *t, int top, int left, int bot, int right,
                          uint8_t ch, uint8_t attr) {
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    if (bot > TEXT_ROWS - 1) bot = TEXT_ROWS - 1;
    if (right > TEXT_COLS - 1) right = TEXT_COLS - 1;
    for (int r = top; r <= bot; r++)
        for (int c2 = left; c2 <= right; c2++) {
            uint32_t o = dos_text_cell(r, c2);
            t->mem[o] = ch; t->mem[o + 1] = attr;
        }
}

static void dos_text_clear(dos_task_t *t, uint8_t attr) {
    dos_text_fill(t, 0, 0, TEXT_ROWS - 1, TEXT_COLS - 1, ' ', attr);
    t->cur_row = t->cur_col = 0;
    dos_text_sync_bda(t);
}

// INT 10h AH=06h/07h window scroll. n==0 means "blank the whole window".
static void dos_text_scroll(dos_task_t *t, int top, int left, int bot, int right,
                            int n, uint8_t attr, int down) {
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    if (bot > TEXT_ROWS - 1) bot = TEXT_ROWS - 1;
    if (right > TEXT_COLS - 1) right = TEXT_COLS - 1;
    if (bot < top || right < left) return;
    int rows = bot - top + 1;
    if (n <= 0 || n >= rows) { dos_text_fill(t, top, left, bot, right, ' ', attr); return; }
    if (!down) {
        for (int r = top; r <= bot - n; r++)
            for (int c2 = left; c2 <= right; c2++) {
                uint32_t d = dos_text_cell(r, c2), sc = dos_text_cell(r + n, c2);
                t->mem[d] = t->mem[sc]; t->mem[d + 1] = t->mem[sc + 1];
            }
        dos_text_fill(t, bot - n + 1, left, bot, right, ' ', attr);
    } else {
        for (int r = bot; r >= top + n; r--)
            for (int c2 = left; c2 <= right; c2++) {
                uint32_t d = dos_text_cell(r, c2), sc = dos_text_cell(r - n, c2);
                t->mem[d] = t->mem[sc]; t->mem[d + 1] = t->mem[sc + 1];
            }
        dos_text_fill(t, top, left, top + n - 1, right, ' ', attr);
    }
}

// One character through the BIOS teletype path (INT 10h AH=0Eh), which is also
// where INT 21h 02h/06h/09h/40h(stdout,stderr) end up, exactly as INT 29h routes
// them on a real machine. Real BIOS TTY PRESERVES the attribute already in the
// cell; we do too, except that an all-zero attribute is black-on-black and would
// render the text invisible, so a zero cell falls back to the tracked attribute.
static void dos_tty_putc(dos_task_t *t, uint8_t ch) {
    switch (ch) {
    case '\r': t->cur_col = 0; break;
    case '\n': if (t->cur_row < TEXT_ROWS) t->cur_row++; break;
    case '\b': if (t->cur_col) t->cur_col--; break;
    case '\t': t->cur_col = (uint8_t)((t->cur_col + 8) & ~7); break;
    case 0x07: break;                       // BEL: nothing to ring
    default: {
        uint32_t o = dos_text_cell(t->cur_row < TEXT_ROWS ? t->cur_row : TEXT_ROWS - 1,
                                   t->cur_col);
        t->mem[o] = ch;
        if (t->mem[o + 1] == 0) t->mem[o + 1] = t->text_attr;
        t->cur_col++;
        break;
    }
    }
    if (t->cur_col >= TEXT_COLS) { t->cur_col = 0; t->cur_row++; }
    if (t->cur_row >= TEXT_ROWS) {
        dos_text_scroll(t, 0, 0, TEXT_ROWS - 1, TEXT_COLS - 1, 1, t->text_attr, 0);
        t->cur_row = TEXT_ROWS - 1;
    }
    dos_text_sync_bda(t);
}

// #736: dos_tty_write() went with INT 21h AH=09h into dos/int21svc.c, where
// the $-terminated string is emitted one character at a time through the
// context's console vtable (which lands in dos_tty_putc below).


// ---- ONE keyboard source for the guest ------------------------------------
// Everything the DOS guest can use to read a key (INT 16h, INT 21h AH=01/06/
// 0B/3Fh, and a direct read of the BDA ring) is served from the SAME BIOS
// keyboard buffer, filled from the raw scancode tap. See dos_keyq_push().
//
// What it replaced, and why that was broken: the INT 21h handlers called
// keyboard_has_char()/keyboard_get_char(), which read the KERNEL's console
// keyboard ring. That ring is also drained by the desktop/compositor for its
// own event routing, so the DOS guest was in a race with the window system for
// every keystroke and normally lost it. The scancode tap is a MIRROR installed
// in the IRQ1 ISR (cpu/isr.c) with no other consumer, so taking input from
// there cannot race anything.
//
// MEASURED, "Invasion of the Mutant Space Bats of Doom", build 1738: its main
// menu polls INT 21h 14,043,560 times and INT 33h 6,692,553 times in a single
// session (a 2:1 poll loop), and makes ZERO INT 16h calls and ZERO port 0x60
// reads. So INT 21h console input IS its keyboard, and it never received a
// keystroke: the selection diamond did not move for three DOWN presses and
// PLAY did nothing.
static int dos_keyq_peek(dos_task_t *t, uint16_t *out);
static int dos_keyq_pop(dos_task_t *t, uint16_t *out);


// ===========================================================================
// #736: THE BINDINGS. Everything below hands the DOS TASK's machine to the
// shared service core; the core does the DOS API work. There is no INT 21h
// switch statement left in this file except the DOS task's own memory model,
// which is machine state (a real MCB chain in guest RAM), not an API service.
// ===========================================================================

// ---- console ------------------------------------------------------------
// The DOS task's console is the serial port AND the emulated text page, which
// is what makes a text-mode game visible in its window at all.
static void svc_con_putc(void *u, uint8_t ch) {
    dos_task_t *t = (dos_task_t *)u;
    // The instruction ring is spent on the FIRST thing the program prints,
    // which is where a title that dies early says why. This used to trigger
    // only on AH=40h to stdout; it now covers AH=02h/06h/09h as well, which is
    // strictly more of the cases it was meant to catch.
    if (g_dos_ring_on && !g_dos_ring_dumped) {
        g_dos_ring_dumped = 1;
        x86_16_ring_dump("first-console-write", g_dos_ring_dump_n);
    }
    serial_write(COM1, (char)ch);
    dos_tty_putc(t, ch);
}
static int svc_con_getkey (void *u, uint16_t *k) { return dos_keyq_pop ((dos_task_t *)u, k); }
static int svc_con_peekkey(void *u, uint16_t *k) { return dos_keyq_peek((dos_task_t *)u, k); }

// ---- the DOS task's own INT 21h functions -------------------------------
// 48h/49h/4Ah are the MS-DOS memory-control-block allocator. They belong here
// and not in the service core because they are the DOS MACHINE: they hand out
// paragraphs of this task's 1 MiB real-mode image and maintain an MCB chain in
// it. A Win16 guest gets its memory from KERNEL's global heap and a DOS/4GW
// guest from DPMI 0501h, so each caller supplies its own, through this hook.
static int dos_extend_int21(dos_svc_ctx_t *ctx, x86_16_cpu_t *c, uint8_t ah) {
    dos_task_t *t = (dos_task_t *)ctx->owner;
    switch (ah) {
    case 0x48: { // allocate BX paragraphs -> AX=segment ; on fail BX=largest avail
        // The bump pointer sits above every LIVE block (see dos_mcb_retop), so
        // an allocation cannot land inside a block the program already resized
        // itself into.
        uint16_t para = c->bx;
        uint16_t avail = (uint16_t)(0xA000 - t->alloc_top_para);  // free below VGA
        uint16_t at = t->alloc_top_para;
        if (para == 0) { c->ax = 8; SET_CF(c); c->bx = avail; break; }
        if (para > avail) {
            // No room above the bump pointer: look for a hole left by a 49h free.
            at = dos_mcb_first_fit(t, para);
            if (!at) {
                c->ax = 8; SET_CF(c);   // insufficient memory
                c->bx = avail;          // report largest available
                if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x FAIL avail=%04x top=%04x ss=%04x\n", para, avail, t->alloc_top_para, c->ss);
                break;
            }
            if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x REUSE hole at %04x (top=%04x)\n", para, at, t->alloc_top_para);
        }
        if (dos_mcb_add(t, at, para) != 0) {
            c->ax = 8; SET_CF(c); c->bx = 0;
            kprintf("[dos] 48h alloc req=%04x FAIL: MCB table full (%d)\n", para, DOS_MAX_MCB);
            break;
        }
        c->ax = at;
        dos_mcb_retop(t);
        if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x -> seg=%04x newtop=%04x ss=%04x\n", para, c->ax, t->alloc_top_para, c->ss);
        break;
    }
    case 0x49: {  // free memory block at ES
        dos_mcb_t *m = dos_mcb_find(t, c->es);
        if (m) { m->live = 0; dos_mcb_retop(t); }
        if (g_x86_dbgring) kprintf("[dos] 49h free es=%04x %s newtop=%04x\n",
                                   c->es, m ? "ok" : "unknown", t->alloc_top_para);
        CLR_CF(c);
        break;
    }
    case 0x4A: { // resize block (ES, BX paragraphs)
        // maxpara is the gap to the next LIVE block, not "everything up to
        // 640K". That answer is SMALLER, which is the point: a grow request
        // that would overlap a live block is correctly DENIED and the runtime
        // retries with the reported size.
        uint16_t seg = c->es;
        uint16_t maxpara = (uint16_t)(dos_mcb_next_above(t, seg) - seg);
        if (g_x86_dbgring) kprintf("[dos] 4Ah resize es=%04x req=%04x maxpara=%04x top=%04x ss=%04x\n", seg, c->bx, maxpara, t->alloc_top_para, c->ss);
        if (c->bx > maxpara) {
            c->ax = 8; SET_CF(c);
            c->bx = maxpara;
        } else {
            dos_mcb_t *m = dos_mcb_find(t, seg);
            if (m) m->para = c->bx;
            else if (dos_mcb_add(t, seg, c->bx) != 0) {
                c->ax = 8; SET_CF(c); c->bx = 0;
                kprintf("[dos] 4Ah resize FAIL: MCB table full (%d)\n", DOS_MAX_MCB);
                break;
            }
            dos_mcb_retop(t);
            CLR_CF(c);
        }
        break;
    }
    default:
        return 0;   // not ours: let the core report the miss
    }
    return 1;
}

// A DOS title that exits for no visible reason is the hardest thing to
// diagnose here, so spend the instruction ring on exactly that moment.
static void dos_on_terminate(dos_svc_ctx_t *ctx, int code) {
    (void)ctx; (void)code;
    if (g_dos_ring_on) x86_16_ring_dump("exit-4Ch", g_dos_ring_dump_n);
}

// Bind this task's machine to a fresh service context. Called once per run,
// AFTER appdir is known and BEFORE the first gated filesystem access.
static void dos_svc_bind(dos_task_t *t) {
    dos_svc_ctx_init(&t->svc, GUESTFS_SLOT_DOS, "dos");
    t->svc.owner        = t;
    dos_svc_bind_x86_16(&t->svc, &t->cpu);
    t->svc.con_u        = t;
    t->svc.con.putc     = svc_con_putc;
    t->svc.con.getkey   = svc_con_getkey;
    t->svc.con.peekkey  = svc_con_peekkey;
    t->svc.has_ivt      = 1;              // a real IVT at 0000:0000
    t->svc.psp_seg      = DOS_PSP_SEG;    // AH=62h
    // THE DEFAULT DTA (disc-identification work). DOS gives every freshly loaded program a Disk
    // Transfer Area at PSP:0080, and AH=1Ah only MOVES it. dta_off was already
    // 0x0080 but dta_seg was left at 0 by the memset in dos_svc_ctx_init(), so
    // until a program called 1Ah every find result was written to 0000:0080,
    // which is inside the interrupt vector table and not in the program's PSP
    // at all. A program using the default DTA therefore got a successful
    // findfirst whose result it could not read: measured on the RED run of this
    // change, where a 4Eh that the kernel logged as "hit" left the guest's DTA
    // holding an empty name and a zero attribute.
    //
    // It survived this long because the runtimes that dominate this tree's test
    // set point the DTA at their own buffer first (Microsoft C's
    // _dos_findfirst() issues 1Ah with the address of the caller's find_t), so
    // the default was never exercised by them.
    t->svc.dta_seg      = DOS_PSP_SEG;
    t->svc.dta_off      = 0x0080;
    t->svc.dos_version  = 0x0005;         // DOS 5.0, as this task has always said
    t->svc.cur_drive    = dos_current_drive();
    t->svc.extend       = dos_extend_int21;
    t->svc.on_terminate = dos_on_terminate;
    {
        int n = 0;
        for (; t->appdir[n] && n < (int)sizeof(t->svc.appdir) - 1; n++)
            t->svc.appdir[n] = t->appdir[n];
        t->svc.appdir[n] = '\0';
    }
}


// ---- INT 10h (VGA BIOS) --------------------------------------------------
static void int10(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    if (g_dos_trace21)
        kprintf("[dos] INT10 AH=%02x al=%02x bx=%04x cx=%04x dx=%04x cs:ip=%04x:%04x\n",
                ah, AL(c), c->bx, c->cx, c->dx, c->cs, c->ip);
    switch (ah) {
    case 0x00: {  // set video mode AL
        uint8_t m = AL(c) & 0x7F;   // bit7 = "don't clear memory"
        t->video_mode = m;
        kprintf("[dos] INT 10h set mode 0x%02x\n", m);
        if (m == 0x13) {
            t->gfx_w = MODE13_W; t->gfx_h = MODE13_H;
            for (int i = 0; i < MODE13_W * MODE13_H; i++)
                t->mem[VGA_A000 + i] = 0;
        } else if (m <= 0x03 || m == 0x07) {
            // Text modes. AL bit 7 (masked off above) means "do not clear".
            t->gfx_w = 0; t->gfx_h = 0;
            t->text_attr = 0x07;
            if (!(AL(c) & 0x80)) dos_text_clear(t, 0x07);
        } else if (m == 0x0D || m == 0x0E || m == 0x10 || m == 0x12) {
            // EGA/VGA planar graphics modes.
            if (m == 0x0D)      { t->gfx_w = 320; t->gfx_h = 200; }
            else if (m == 0x0E) { t->gfx_w = 640; t->gfx_h = 200; }
            else if (m == 0x10) { t->gfx_w = 640; t->gfx_h = 350; }
            else                { t->gfx_w = 640; t->gfx_h = 480; }
            // clear all 4 planes; reset VGA register state to power-on defaults.
            for (int p = 0; p < 4; p++) memset(t->ega_plane[p], 0, EGA_PLANE_SIZE);
            t->seq_map_mask = 0x0F;
            t->gc_set_reset = t->gc_en_set_reset = 0;
            t->gc_data_rotate = 0; t->gc_read_map = 0; t->gc_mode = 0;
            t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
            // Seed the attribute controller with the palette a real BIOS leaves,
            // and seed the WHOLE 6-bit DAC space (0x00-0x3F) that those registers
            // can select, not just 0x00-0x0F. Seeding only the first 16 was the
            // colour bug: a program that programs the bright half as 0x18-0x1F
            // (Keen) or 0x38-0x3F (the BIOS default form) indexed DAC entries
            // that still held the boot-time grayscale ramp.
            for (int i = 0; i < 16; i++) t->atc_pal[i] = ega_atc_default[i];
            for (int v = 0; v < 64; v++) {
                const uint8_t *c8 = ega_default_dac[ega_pal_to_index((uint8_t)v)];
                t->pal[v][0] = c8[0];
                t->pal[v][1] = c8[1];
                t->pal[v][2] = c8[2];
            }
            // Power-on/mode-set CRTC defaults that MATTER to the present path.
            // crtc[] is memset to 0, and a zero Line Compare means "split at
            // scanline 0", i.e. the entire screen refetches from address 0 and
            // nothing scrolls. The BIOS leaves Line Compare at its maximum.
            t->crtc[0x18] = 0xFF;
            t->crtc[0x07] |= 0x10;
            t->crtc[0x09] |= 0x40;
            t->atc_reg[0x13] = 0;
            t->ega_dirty = 1;
        }
        wr8(t, 0x0040, 0x0049, m);                                  // BDA current mode
        wr16(t, 0x0040, 0x004A, dos_text_is(t) ? TEXT_COLS : 40);   // BDA columns
        break;
    }
    case 0x01:  // set cursor shape (CH=start, CL=end) -> BDA only
        wr16(t, 0x0040, 0x0060, c->cx);
        break;
    case 0x02:  // set cursor position: DH=row DL=col (BH=page, page 0 only)
        t->cur_row = (uint8_t)((c->dx >> 8) & 0xFF);
        t->cur_col = (uint8_t)(c->dx & 0xFF);
        if (t->cur_row > TEXT_ROWS - 1) t->cur_row = TEXT_ROWS - 1;
        if (t->cur_col > TEXT_COLS - 1) t->cur_col = TEXT_COLS - 1;
        dos_text_sync_bda(t);
        break;
    case 0x03:  // get cursor -> DH=row DL=col, CX=shape
        c->dx = (uint16_t)((t->cur_row << 8) | t->cur_col);
        c->cx = rd16(t, 0x0040, 0x0060);
        break;
    case 0x05:  // set active display page: we only implement page 0
        break;
    case 0x06:  // scroll window up   AL=lines CH/CL=top/left DH/DL=bot/right BH=attr
    case 0x07:  // scroll window down
        dos_text_scroll(t, (c->cx >> 8) & 0xFF, c->cx & 0xFF,
                        (c->dx >> 8) & 0xFF, c->dx & 0xFF,
                        AL(c), (uint8_t)((c->bx >> 8) & 0xFF), ah == 0x07);
        break;
    case 0x08: { // read char+attr at cursor -> AL=char AH=attr
        uint32_t o = dos_text_cell(t->cur_row, t->cur_col);
        AL_SET(c, t->mem[o]);
        AH_SET(c, t->mem[o + 1]);
        break;
    }
    case 0x09:   // write char+attr CX times at cursor (cursor does NOT advance)
    case 0x0A: { // write char CX times, keep the existing attribute
        uint16_t n = c->cx ? c->cx : 1;
        for (uint16_t i = 0; i < n; i++) {
            int col = t->cur_col + i, row = t->cur_row;
            while (col >= TEXT_COLS) { col -= TEXT_COLS; row++; }
            if (row > TEXT_ROWS - 1) break;
            uint32_t o = dos_text_cell(row, col);
            t->mem[o] = AL(c);
            if (ah == 0x09) t->mem[o + 1] = (uint8_t)(c->bx & 0xFF);
            else if (t->mem[o + 1] == 0) t->mem[o + 1] = t->text_attr;
        }
        if (ah == 0x09) t->text_attr = (uint8_t)(c->bx & 0xFF);
        break;
    }
    case 0x0E:  // teletype output: AL=char, BL=colour (graphics modes only)
        dos_tty_putc(t, AL(c));
        serial_write(COM1, (char)AL(c));
        break;
    case 0x0F:  // get video mode -> AL=mode, AH=cols, BH=active page
        AL_SET(c, t->video_mode);
        AH_SET(c, dos_text_is(t) ? TEXT_COLS : 40);
        c->bx = (uint16_t)(c->bx & 0x00FF);
        break;

    case 0x12:  // alternate select / EGA-VGA info
        if (AL(c) == 0x10 || (c->bx & 0xFF) == 0x10) {
            // BL=10h "get EGA info" answers in BOTH halves of BX:
            //   BH = 0 colour (3Dx ports) / 1 mono (3Bx),  BL = memory (3 = 256KB),
            //   CH = feature bits, CL = switch settings.
            // This used to write only BL and PRESERVE BH, while the comment next to
            // it claimed BH=0. That is not a cosmetic gap: the textbook EGA probe is
            //     mov bx,0FF10h / mov ah,12h / int 10h / cmp bh,0FFh / je no_ega
            // which loads BH with a 0xFF SENTINEL precisely so an absent BIOS leaves
            // it untouched. Preserving BH means answering "no EGA card" to every
            // program that probes the standard way. bats25 does exactly this, printed
            // "Sorry, aber Sie benoetigen eine EGA oder VGA Karte." and exited 0, and
            // that was misread as a joystick problem for a whole session.
            c->bx = 0x0003;                      // BH=0 (colour), BL=3 (256KB)
            c->cx = 0x0009;                      // CH=0 features, CL=9 switches
        }
        // other AL subfunctions: accept silently
        break;

    case 0x1A:  // display combination code (VGA BIOS)
        if (AL(c) == 0x00) {
            AL_SET(c, 0x1A);            // function supported
            c->bx = 0x0008;             // BL=8 (VGA colour analog), BH=0 (none)
        }
        break;

    case 0x1B:  // get functionality/state info -> AL=1B if supported (report not)
        break;
    case 0x10:  // palette / DAC functions
        if (g_x86_dbgring) {
            static int n10 = 0;
            if (n10 < 20) { n10++;
                kprintf("[dos] INT10 AH=10 al=%02x bx=%04x cx=%04x dx=%04x es=%04x\n",
                        AL(c), c->bx, c->cx, c->dx, c->es);
                if (AL(c) == 0x02) {
                    kprintf("[dos]   pal table:");
                    for (int i = 0; i < 17; i++)
                        kprintf(" %02x", rd8(t, c->es, (uint16_t)(c->dx + i)));
                    kprintf("\n");
                }
            }
        }
        if (AL(c) == 0x00) {        // set single EGA palette reg: BL=reg, BH=value
            uint8_t reg = (uint8_t)(c->bx & 0x0F);
            t->atc_pal[reg] = (uint8_t)((c->bx >> 8) & 0x3F);
        } else if (AL(c) == 0x02) { // set all 16 EGA palette regs + overscan from ES:DX (17 bytes)
            for (int i = 0; i < 16; i++)
                t->atc_pal[i] = rd8(t, c->es, (uint16_t)(c->dx + i)) & 0x3F;
        } else if (AL(c) == 0x10) {        // set single DAC register: BX=index, DH=r DL? -> CH=g CL=b DH=r
            uint16_t idx = c->bx & 0xFF;
            t->pal[idx][0] = (uint8_t)(c->dx >> 8) & 0x3F;   // DH = red
            t->pal[idx][1] = (uint8_t)(c->cx >> 8) & 0x3F;   // CH = green
            t->pal[idx][2] = (uint8_t)(c->cx & 0xFF) & 0x3F; // CL = blue
        } else if (AL(c) == 0x12) { // set block of DAC: BX=start, CX=count, ES:DX=table(3 bytes each)
            uint16_t start = c->bx, count = c->cx;
            for (uint16_t i = 0; i < count && (start + i) < 256; i++) {
                uint16_t o = (uint16_t)(c->dx + i * 3);
                t->pal[start + i][0] = rd8(t, c->es, o)     & 0x3F;
                t->pal[start + i][1] = rd8(t, c->es, (uint16_t)(o + 1)) & 0x3F;
                t->pal[start + i][2] = rd8(t, c->es, (uint16_t)(o + 2)) & 0x3F;
            }
        }
        break;
    default:
        // many INT 10h fns (cursor, teletype) are harmless to ignore
        break;
    }
}

// ---- INT 33h (mouse) -----------------------------------------------------
static void int33(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    switch (c->ax) {
    case 0x0000: // reset / installed?
        c->ax = 0xFFFF;   // installed
        c->bx = 2;        // 2 buttons
        t->mouse_initialized = 1;
        break;
    case 0x0001: t->mouse_on = 1; break;  // show cursor
    case 0x0002: t->mouse_on = 0; break;  // hide cursor
    case 0x0003: // get position/buttons -> CX=x DX=y BX=buttons
        c->cx = (uint16_t)t->mx;
        c->dx = (uint16_t)t->my;
        c->bx = (uint16_t)t->mbtn;
        break;
    case 0x0004: // set position CX=x DX=y
        t->mx = c->cx; t->my = c->dx;
        break;
    case 0x0007: case 0x0008: // set horiz/vert range -> ignore
        break;
    case 0x000B: // read motion counters -> CX=dx DX=dy (return 0)
        c->cx = 0; c->dx = 0;
        break;
    default:
        break;
    }
}

// ---- INT 16h (keyboard BIOS) ---------------------------------------------
// ---- BIOS keyboard queue for INT 16h -------------------------------------
//
// INT 16h returns a PAIR: AH = the make scan code, AL = the ASCII character (0
// when the key has none). The old implementation returned `c->ax = ch & 0xFF`,
// i.e. AL = ascii and AH ALWAYS ZERO, sourced from keyboard_get_char() which
// only ever yields printable characters.
//
// That makes every key WITHOUT an ASCII code invisible: the arrow keys, the
// function keys, Home/End/PgUp/PgDn. A DOS menu navigated with UP-ARROW and
// DOWN-ARROW therefore receives nothing at all, because for those keys AL is 0
// and AH is where the information lives. Measured on "Invasion of the Mutant
// Space Bats of Doom": three DOWN presses at its main menu left the selection
// diamond on PLAY, pixel-identical. Its intro slideshow advanced fine on SPACE,
// which is exactly the tell, SPACE has an ASCII code and the arrows do not.
//
// The queue is fed from the raw scancode tap (the same mirror the guest-INT-9
// path uses) and translated with the PS/2 driver's OWN table and modifier
// state, so there is no second keymap in the tree to drift.
// THE BUFFER LIVES IN THE GUEST'S BIOS DATA AREA, NOT IN A PRIVATE ARRAY.
//
// That is not a detail, it is the whole bug. A large fraction of DOS programs
// never call INT 16h at all: they read the BIOS keyboard ring DIRECTLY out of
// the BDA, because it needs no interrupt, no BIOS call, and works with
// interrupts disabled. The layout is fixed and every one of them agrees on it:
//
//   0040:001A  head  (offset within segment 0x40 of the next key to read)
//   0040:001C  tail  (offset where the next key will be written)
//   0040:001E  ring  16 entries of (scan<<8 | ascii), ending at 0040:003E
//   0040:0080  ring start pointer (0x001E)
//   0040:0082  ring end pointer   (0x003E)
//   0040:0017  shift flags
//
// MEASURED on "Invasion of the Mutant Space Bats of Doom", build 1736, with the
// counters this replaced: ZERO INT 16h calls and ZERO port 0x60 reads over a
// whole session, while the host side was correctly decoding every keystroke
// (`keyq push scan=1c ascii=0d` for ENTER, `scan=39 ascii=20` for SPACE) into a
// private queue THE GUEST COULD NOT SEE. Its main menu had therefore never
// received a keystroke by any path: the selection diamond did not move for
// three DOWN presses and PLAY did nothing, which is exactly "it gets to the game
// start screen but the game doesn't start". A private queue plus a correct INT
// 16h fixes nothing for a program that reads the BDA.
//
// Writing the real ring also gives ONE source of truth: INT 16h below reads the
// same words the guest reads, so the two can never disagree.
#define BDA_SEG          0x0040
#define BDA_KB_SHIFT1    0x0017
#define BDA_KB_HEAD      0x001A
#define BDA_KB_TAIL      0x001C
#define BDA_KB_RING      0x001E
#define BDA_KB_RING_END  0x003E
#define BDA_KB_START_PTR 0x0080
#define BDA_KB_END_PTR   0x0082

static uint8_t dos_bios_shift_flags(void);

static uint16_t dos_bda_kb_next(uint16_t off) {
    uint16_t n = (uint16_t)(off + 2);
    return (n >= BDA_KB_RING_END) ? (uint16_t)BDA_KB_RING : n;
}

static void dos_keyq_reset(dos_task_t *t) {
    wr16(t, BDA_SEG, BDA_KB_START_PTR, BDA_KB_RING);
    wr16(t, BDA_SEG, BDA_KB_END_PTR,   BDA_KB_RING_END);
    wr16(t, BDA_SEG, BDA_KB_HEAD,      BDA_KB_RING);
    wr16(t, BDA_SEG, BDA_KB_TAIL,      BDA_KB_RING);
}

static void dos_keyq_push(dos_task_t *t, uint8_t scan, uint8_t ascii) {
    uint16_t head = rd16(t, BDA_SEG, BDA_KB_HEAD);
    uint16_t tail = rd16(t, BDA_SEG, BDA_KB_TAIL);
    // A guest that has never touched the pointers leaves them zero; treat that
    // as "not initialised yet" rather than writing key data over the BDA.
    if (head < BDA_KB_RING || head >= BDA_KB_RING_END ||
        tail < BDA_KB_RING || tail >= BDA_KB_RING_END) {
        dos_keyq_reset(t);
        head = tail = BDA_KB_RING;
    }
    uint16_t next = dos_bda_kb_next(tail);
    if (next == head) return;          // full: real BIOS beeps and drops
    wr16(t, BDA_SEG, tail, (uint16_t)(((uint16_t)scan << 8) | ascii));
    wr16(t, BDA_SEG, BDA_KB_TAIL, next);
}

static int dos_keyq_peek(dos_task_t *t, uint16_t *out) {
    uint16_t head = rd16(t, BDA_SEG, BDA_KB_HEAD);
    uint16_t tail = rd16(t, BDA_SEG, BDA_KB_TAIL);
    if (head == tail) return 0;
    if (head < BDA_KB_RING || head >= BDA_KB_RING_END) return 0;
    *out = rd16(t, BDA_SEG, head);
    return 1;
}

static int dos_keyq_pop(dos_task_t *t, uint16_t *out) {
    if (!dos_keyq_peek(t, out)) return 0;
    uint16_t head = rd16(t, BDA_SEG, BDA_KB_HEAD);
    wr16(t, BDA_SEG, BDA_KB_HEAD, dos_bda_kb_next(head));
    return 1;
}

// Drain the raw scancode tap into the BIOS queue. Called from the run loop only
// while the guest has NOT hooked INT 9, so the raw stream has exactly one
// consumer: a guest with its own INT 9 handler owns the hardware and gets the
// scancodes replayed to it instead (dos_deliver_int9).
static void dos_keyq_pump(dos_task_t *t) {
    for (int n = 0; n < 16; n++) {
        int sc = dos_scancode_get();
        if (sc < 0) break;
        uint8_t b = (uint8_t)sc;
        if (b == 0xE0 || b == 0xE1) continue;   // extended prefix: the NEXT byte
                                                // carries the same make code the
                                                // BIOS reports in AH
        if (b & 0x80) continue;                 // break (release) code
        char ch = keyboard_scancode_to_char(b, keyboard_get_modifiers());
        // BIOS convention, not a driver bug: the BIOS reports ENTER as CR
        // (0x0D). The kernel's table is written for a console and yields LF
        // (0x0A), and a DOS menu that compares against 13 ignores 10.
        if (b == KEY_ENTER) ch = '\r';
        dos_keyq_push(t, b, (uint8_t)ch);
    }
    // Keep the BDA shift-flag byte live too: programs read 0040:0017 directly
    // for exactly the same reason they read the ring directly.
    wr8(t, BDA_SEG, BDA_KB_SHIFT1, dos_bios_shift_flags());
}

// BIOS shift-status byte (INT 16h AH=02), assembled from the driver's live
// modifier state rather than a second copy of it.
static uint8_t dos_bios_shift_flags(void) {
    uint32_t m = keyboard_get_modifiers();
    uint8_t f = 0;
    if (m & KEY_MOD_SHIFT)  f |= 0x03;   // right|left shift (we do not split them)
    if (m & KEY_MOD_CTRL)   f |= 0x04;
    if (m & KEY_MOD_ALT)    f |= 0x08;
    if (m & KEY_MOD_SCROLL) f |= 0x10;
    if (m & KEY_MOD_NUM)    f |= 0x20;
    if (m & KEY_MOD_CAPS)   f |= 0x40;
    return f;
}

static void int16(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    uint16_t k = 0;
    switch (ah) {
    case 0x00: case 0x10: // read key -> AH=scan AL=ascii
        // Non-blocking by design: this returns AX=0 when the queue is empty and
        // the guest loops. Blocking here would block the interpreter thread,
        // which is the banned #426 pattern, and would also stop us pumping the
        // very input it is waiting for.
        if (dos_keyq_pop(t, &k)) c->ax = k;
        else                     c->ax = 0;
        break;
    case 0x01: case 0x11: // key available? -> ZF clear + AX = the key if so
        if (dos_keyq_peek(t, &k)) { c->flags &= ~0x0040; c->ax = k; }
        else                      { c->flags |=  0x0040; c->ax = 0; }
        break;
    case 0x02: case 0x12: // shift status
        AL_SET(c, dos_bios_shift_flags());
        break;
    default:
        break;
    }
}

// Master interrupt dispatcher for the DOS task.
static int dos_int_handler(x86_16_cpu_t *c, uint8_t intno) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return 0;
    switch (intno) {
    case 0x20: t->cpu.halted = 1; t->cpu.exit_code = 0; return 0;  // legacy terminate
    case 0x21: dos_svc_int21(&t->svc, &t->cpu); return 0;
    case 0x10: int10(t); return 0;
    case 0x33: int33(t); return 0;
    case 0x16: int16(t); return 0;
    case 0x1A: t->cpu.cx = 0; t->cpu.dx = 0; return 0;  // timer tick count
    case 0x67: // EMS (LIM) - report "not installed" so the app uses conventional mem
        AH_SET(c, 0x80);   // status: EMM software not present / general failure
        return 0;
    case 0x2F: // multiplex: XMS install check + MSCDEX (#196)
        // AX=4300h: XMS driver install check. Real HIMEM returns AL=0x80. We
        // have no XMS, so return AL!=0x80 to report "not installed" (Keen then
        // falls back to conventional memory).
        if (c->ax == 0x4300) { AL_SET(c, 0x00); return 0; }

        // ---- AX=1600h / 1686h / 1687h: Windows and DPMI presence -----------
        //
        // THERE IS NO DPMI HOST AND NO PROTECTED MODE HERE, AND THAT IS
        // WORTH SAYING OUT LOUD RATHER THAN BY OMISSION.
        //
        // These three fell through to the "not handled" return at the bottom,
        // which leaves every register exactly as the guest set it. That happens
        // to leave AX nonzero, which happens to be the "absent" encoding, so the
        // answer was accidentally right and structurally silent: nothing in the
        // code said "no", the correctness depended on no future arm touching AX
        // first, and ES:DI came back holding whatever the caller had in it. A
        // program that tests the entry pointer instead of AX (they exist, the
        // spec's own wording invites it) would FAR CALL into its own stale
        // registers. An absent host must be indistinguishable from a broken one
        // only in the sense that both refuse; it must never look like a present
        // one.
        //
        // The encodings, all from the DPMI 1.0 spec and the Windows INT 2Fh
        // interface, all of which report absence with a NONZERO AX (this is why
        // "return 0" is not an answer here):
        //   1600h  Windows enhanced-mode check. AL = 00h: no Windows 3.x
        //          enhanced mode. AL is the whole answer; AH is reserved.
        //   1686h  "am I in protected mode under DPMI": AX = 0 YES, nonzero NO.
        //          We are in real mode, so nonzero.
        //   1687h  get the DPMI host entry point: AX = 0 present, nonzero
        //          absent. On absence the other outputs are undefined, so they
        //          are ZEROED: a caller that skips the AX test then gets a NULL
        //          selector:offset it will fault on immediately, which is a
        //          diagnosable failure, rather than a stale pointer it will call
        //          and land somewhere plausible-looking.
        //
        // This is NOT a DPMI host and is not a step toward one. It is the
        // truthful "no" that a 32-bit extender needs in order to fail where the
        // problem actually is.
        if (c->ax == 0x1600 || c->ax == 0x1686 || c->ax == 0x1687) {
            uint16_t fn = c->ax;
            if (fn == 0x1600) {
                c->ax = 0x0000;              // AL = 0: no Windows enhanced mode
            } else {
                c->ax = 0xFFFF;              // nonzero: no DPMI / not in PM
                if (fn == 0x1687) {
                    c->bx = 0; c->cx = 0; c->si = 0; c->di = 0; c->es = 0;
                }
            }
            CLR_CF(c);
            kprintf("[dos] INT 2Fh AX=%04x -> no DPMI host (ax=%04x)\n", fn, c->ax);
            return 0;
        }

        // AX=15xxh: MSCDEX. A DOS program does NOT reach a CD through INT 21h
        // alone; it asks MSCDEX (the CD-ROM redirector) which drive letters are
        // CD-ROMs, and only then opens files on that letter. Without these
        // calls a mounted disc is invisible to a DOS guest no matter how well
        // the filesystem works, so this is the piece that actually connects
        // #196 to a DOS game.
        //
        // Implemented: the DISCOVERY subset (1500/150B/150C/150D/1501). File
        // access itself then goes through the ordinary INT 21h handle calls,
        // which reach the disc via the fat_open() redirect. NOT implemented:
        // the raw-sector and device-request calls (1508 absolute read, 1509,
        // 150E, 1510 device request, and the audio/CD-DA group). A program that
        // reads the disc as raw sectors, or drives CD audio, will not work; it
        // gets a clean "not supported" rather than a plausible lie, because a
        // fabricated success there returns garbage data the caller cannot
        // distinguish from a real disc.
        if ((c->ax & 0xFF00) == 0x1500) {
            // #739: the answer is DERIVED from the live mount table by
            // drvmap_mscdex_rs(), not written down here. It used to be the two
            // constants `bx = 1` and `cx = 4`, which was correct only while
            // exactly one CD could exist and it was always E:. A drive is
            // reported as a CD-ROM only while a disc is mounted on it, so "no
            // disc" and "no drive" remain the same observable state, which is
            // what an eject should look like to a guest.
            extern void diskimg_mscdex(mscdex_info_t *out);
            mscdex_info_t mi;
            diskimg_mscdex(&mi);
            int have = (mi.count > 0) ? 1 : 0;
            switch (c->ax) {
            case 0x1500:   // installation check: BX = #drives, CX = first drive
                c->bx = (uint16_t)mi.count;
                c->cx = (uint16_t)mi.first;
                return 0;
            case 0x1501:   // get driver header list -> ES:BX array of headers.
                // We have no real device driver header to hand out. Leave the
                // caller's buffer untouched and report zero drives; a program
                // that insists on walking driver headers is in the raw-device
                // group we do not support.
                c->bx = 0;
                return 0;
            case 0x150B: { // drive check: CX = drive number
                //BX = 0xADAD is MSCDEX's signature; AX != 0 means "is a CD".
                // This is the AUTHORITATIVE per-drive answer, and it matters
                // more now: 1500h's (count, first) pair implies one contiguous
                // block, and ejecting the middle of three discs leaves a hole.
                // A program that walks first..first+count-1 and asks here about
                // each is told the truth about every letter.
                c->bx = 0xADAD;
                int is_cd = 0;
                for (uint32_t k = 0; k < mi.count && k < sizeof mi.letters; k++)
                    if ((uint16_t)mi.letters[k] == c->cx) is_cd = 1;
                c->ax = (uint16_t)(is_cd ? 1 : 0);
                return 0; }
            case 0x150C:   // get version -> BH.BL
                c->bx = have ? 0x020A : 0x0000;   // MSCDEX 2.10
                return 0;
            case 0x150D: { // get CD-ROM drive letters -> ES:BX, one byte each
                // One byte per mounted CD, ascending. This, not 1500h, is where
                // a non-contiguous set of letters is reported exactly.
                for (uint32_t k = 0; k < mi.count && k < sizeof mi.letters; k++)
                    wr8(t, c->es, (uint16_t)(c->bx + k), mi.letters[k]);
                return 0;
            }
            default:
                // Unimplemented MSCDEX function. Say so on serial rather than
                // returning a silent 0 that reads as success: an unhandled 2Fh
                // that looks handled is how the Win16 layer's MISS imports used
                // to desync a whole interpreter (blame.md).
                kprintf("[dos] MSCDEX AX=%04x UNIMPLEMENTED (raw-sector/audio "
                        "group is not supported)\n", c->ax);
                SET_CF(c);
                return 0;
            }
        }
        // All other 2Fh multiplex calls: not handled.
        return 0;
    default:
        // ignore other interrupts (vectors, DOS internal)
        kprintf("[dos] INT %02xh ax=%04x bx=%04x (ignored)\n", intno, c->ax, c->bx);
        return 0;
    }
}

// ---- EGA planar framebuffer (mode 0Dh) -----------------------------------
// Standard VGA/EGA write-mode + read-mode logic. The CPU writes a single byte
// to 0xA0000+off; the sequencer Map Mask + graphics-controller registers fan it
// out across the 4 hidden bitplanes. Commander Keen's renderer uses write mode 0
// (with map mask / set-reset for solid colours and bit mask for masked sprites)
// plus write mode 1 (latch copy) for fast plane-to-plane block copies/scrolling.

static uint8_t ega_rotate(dos_task_t *t, uint8_t v) {
    uint8_t rot = t->gc_data_rotate & 0x07;
    if (rot) v = (uint8_t)((v >> rot) | (v << (8 - rot)));
    return v;
}

// Apply the GC logical-operation (data rotate reg bits 3-4) between the CPU/ALU
// value and the corresponding plane latch.
static uint8_t ega_alu(dos_task_t *t, uint8_t val, uint8_t latch) {
    switch ((t->gc_data_rotate >> 3) & 0x03) {
        case 1: return (uint8_t)(val & latch);
        case 2: return (uint8_t)(val | latch);
        case 3: return (uint8_t)(val ^ latch);
        default: return val;
    }
}

// Write a CPU byte to the EGA aperture at linear address `lin` (off into plane).
static void ega_write(dos_task_t *t, uint32_t lin, uint8_t cpu_val) {
    uint32_t off = lin - VGA_A000;
    if (off >= EGA_PLANE_SIZE) return;
    uint8_t wmode = t->gc_mode & 0x03;
    uint8_t bitmask = t->gc_bit_mask;
    t->ega_dirty = 1;

    for (int p = 0; p < 4; p++) {
        if (!(t->seq_map_mask & (1 << p))) continue;  // map mask gates writes per plane
        uint8_t latch = t->ega_latch[p];
        uint8_t res;
        switch (wmode) {
        case 0: {
            // write mode 0: rotate CPU value, then for planes whose enable-set/reset
            // bit is set use the set/reset colour byte (all 0s or all 1s) instead.
            uint8_t v;
            if (t->gc_en_set_reset & (1 << p))
                v = (t->gc_set_reset & (1 << p)) ? 0xFF : 0x00;
            else
                v = ega_rotate(t, cpu_val);
            v = ega_alu(t, v, latch);
            res = (uint8_t)((v & bitmask) | (latch & ~bitmask));
            break;
        }
        case 1:
            // write mode 1: copy the latches straight through (CPU value ignored).
            res = latch;
            break;
        case 2: {
            // write mode 2: each plane gets bit (cpu_val>>p)&1 expanded to 0x00/0xFF.
            uint8_t v = (cpu_val & (1 << p)) ? 0xFF : 0x00;
            v = ega_alu(t, v, latch);
            res = (uint8_t)((v & bitmask) | (latch & ~bitmask));
            break;
        }
        case 3: {
            // write mode 3: rotated CPU value ANDed with bit mask forms the mask;
            // colour comes from set/reset. (Rarely used by Keen but cheap to add.)
            uint8_t v = ega_rotate(t, cpu_val) & bitmask;
            uint8_t col = (t->gc_set_reset & (1 << p)) ? 0xFF : 0x00;
            res = (uint8_t)((col & v) | (latch & ~v));
            break;
        }
        default: res = latch; break;
        }
        t->ega_plane[p][off] = res;
    }
}

// Read a CPU byte from the EGA aperture. Always reloads all 4 latches (real
// hardware latches every plane on any read), then returns per the read mode.
static uint8_t ega_read(dos_task_t *t, uint32_t lin) {
    uint32_t off = lin - VGA_A000;
    if (off >= EGA_PLANE_SIZE) return 0xFF;
    for (int p = 0; p < 4; p++) t->ega_latch[p] = t->ega_plane[p][off];
    if (t->gc_mode & 0x08) {
        // read mode 1: colour-compare. Each result bit set where all planes
        // (masked by color-dont-care) match the color-compare register.
        uint8_t result = 0;
        for (int b = 0; b < 8; b++) {
            int match = 1;
            for (int p = 0; p < 4; p++) {
                if (!(t->gc_color_dont_care & (1 << p))) continue;
                int planebit = (t->ega_plane[p][off] >> b) & 1;
                int cmpbit   = (t->gc_color_cmp >> p) & 1;
                if (planebit != cmpbit) { match = 0; break; }
            }
            if (match) result |= (1 << b);
        }
        return result;
    }
    // read mode 0: return the plane selected by GC read-map.
    return t->ega_plane[t->gc_read_map & 3][off];
}

// Mem-hook trampolines registered with the interpreter.
static void ega_mem_w(x86_16_cpu_t *c, uint32_t lin, uint16_t val, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return;

    if (t->video_mode == 0x13) {
        // Mode 13h is a plain linear byte buffer; write straight to mem[].
        if (lin < DOS_MEM_SIZE) t->mem[lin] = (uint8_t)(val & 0xFF);
        if (width == 2 && lin + 1 < DOS_MEM_SIZE) t->mem[lin + 1] = (uint8_t)(val >> 8);
        return;
    }
    ega_write(t, lin, (uint8_t)(val & 0xFF));
    if (width == 2) ega_write(t, lin + 1, (uint8_t)(val >> 8));
}
static uint16_t ega_mem_r(x86_16_cpu_t *c, uint32_t lin, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return 0xFFFF;

    if (t->video_mode == 0x13) {
        uint16_t v = (lin < DOS_MEM_SIZE) ? t->mem[lin] : 0xFF;
        if (width == 2) v |= (uint16_t)((lin + 1 < DOS_MEM_SIZE ? t->mem[lin + 1] : 0xFF) << 8);
        return v;
    }
    uint16_t v = ega_read(t, lin);
    if (width == 2) v |= (uint16_t)(ega_read(t, lin + 1) << 8);
    return v;
}

// ---- I/O port hooks (VGA DAC + status) -----------------------------------
static uint16_t dos_in(x86_16_cpu_t *c, uint16_t port, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return 0xFFFF;
    (void)width;
    if (port == 0x3DA || port == 0x3BA) {
        // VGA input status #1. Reading it resets the attribute-controller
        // flip-flop. We toggle a broad set of bits (display-enable 0x01,
        // vertical-retrace 0x08, plus 0x80) on every read so any "wait for the
        // status to change / wait for retrace" polling loop makes progress and
        // exits. id's VGA-detection routine watches bit 0x80 toggle.
        t->atc_flipflop = 0;
        static uint8_t tog = 0;
        tog = (uint8_t)(tog ^ 0x89);
        return tog;
    }
    if (port == 0x3C9) {   // DAC data read
        uint8_t v = t->pal[t->dac_ridx & 0xFF][t->dac_phase];
        t->dac_phase++;
        if (t->dac_phase >= 3) { t->dac_phase = 0; t->dac_ridx++; }
        return v;
    }
    if (port == 0x3CF) {   // graphics-controller data read
        switch (t->gc_idx) {
        case 0: return t->gc_set_reset;
        case 1: return t->gc_en_set_reset;
        case 2: return t->gc_color_cmp;
        case 3: return t->gc_data_rotate;
        case 4: return t->gc_read_map;
        case 5: return t->gc_mode;
        case 6: return t->gc_misc;
        case 7: return t->gc_color_dont_care;
        case 8: return t->gc_bit_mask;
        }
        return 0xFF;
    }
    if (port == 0x3C4) return t->seq_idx;
    if (port == 0x3CE) return t->gc_idx;
    if (port == 0x3D4 || port == 0x3B4) return t->crtc_idx;
    if (port == 0x3D5 || port == 0x3B5)            // CRTC data: read back the register file
        return t->crtc[t->crtc_idx & 0x1F];
    if (port == 0x3CC || port == 0x3C2) return t->misc_out;  // Misc Output read
    if (port == 0x3C5) { if (t->seq_idx < 8) return t->seq_reg[t->seq_idx]; return 0xFF; }
    if (port == 0x40) {
        // PIT channel 0 data. There was NO case for 0x40/0x43 at all, so this
        // returned 0xFF: a delay-loop calibration read start == end == 0xFFFF,
        // its delta was always 0, its `cmp ax,imm` never passed, and the loop
        // was unterminatable BY CONSTRUCTION.
        uint16_t v = t->pit_latched ? t->pit_latch : dos_pit_count(t);
        uint8_t b;
        if (t->pit_access == 1) {          // lobyte only
            b = (uint8_t)(v & 0xFF); t->pit_latched = 0;
        } else if (t->pit_access == 2) {   // hibyte only
            b = (uint8_t)(v >> 8); t->pit_latched = 0;
        } else {                            // lobyte/hibyte pair
            if (!t->pit_rd_hi) { b = (uint8_t)(v & 0xFF); t->pit_rd_hi = 1; }
            else { b = (uint8_t)(v >> 8); t->pit_rd_hi = 0; t->pit_latched = 0; }
        }
        return b;
    }
    if (port == 0x60) return t->kbd_port60;       // keyboard data port (last scancode)
    if (port == 0x64) return 0x14;                 // 8042 status: output buffer full + system flag
    return 0xFF;
}

static void dos_out(x86_16_cpu_t *c, uint16_t port, uint16_t val, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return;
    (void)width;
    switch (port) {
    case 0x43: {  // PIT control word
        uint8_t cw = (uint8_t)(val & 0xFF);
        uint8_t ch = (uint8_t)(cw >> 6);
        uint8_t rw = (uint8_t)((cw >> 4) & 3);
        if (ch != 0) break;              // only channel 0 is emulated
        if (rw == 0) {                    // counter-latch command
            t->pit_latch = dos_pit_count(t);
            t->pit_latched = 1;
            t->pit_rd_hi = 0;
        } else {
            t->pit_access = rw;
            t->pit_latched = 0;
            t->pit_rd_hi = 0; t->pit_wr_hi = 0;
        }
        break;
    }
    case 0x40: {  // PIT channel 0 divisor
        uint8_t b = (uint8_t)(val & 0xFF);
        if (t->pit_access == 1)
            t->pit_divisor = (uint16_t)((t->pit_divisor & 0xFF00) | b);
        else if (t->pit_access == 2)
            t->pit_divisor = (uint16_t)((t->pit_divisor & 0x00FF) | ((uint16_t)b << 8));
        else if (!t->pit_wr_hi) {
            t->pit_divisor = (uint16_t)((t->pit_divisor & 0xFF00) | b); t->pit_wr_hi = 1;
        } else {
            t->pit_divisor = (uint16_t)((t->pit_divisor & 0x00FF) | ((uint16_t)b << 8)); t->pit_wr_hi = 0;
        }
        break;
    }
    case 0x3C8:  // DAC write index
        t->dac_widx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C7:  // DAC read index
        t->dac_ridx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C9:  // DAC data write (r,g,b sequence, 6-bit each)
        t->pal[t->dac_widx & 0xFF][t->dac_phase] = (uint8_t)(val & 0x3F);
        t->dac_phase++;
        if (t->dac_phase >= 3) { t->dac_phase = 0; t->dac_widx++; }
        break;

    // ---- EGA sequencer (0x3C4 index / 0x3C5 data) ----
    case 0x3C4:
        t->seq_idx = (uint8_t)(val & 0x07);
        if (width == 2) {  // word OUT: high byte is the data
            uint8_t d = (uint8_t)(val >> 8);
            t->seq_reg[t->seq_idx] = d;
            if (t->seq_idx == 2) t->seq_map_mask = d & 0x0F;
        }
        break;
    case 0x3C5:
        t->seq_reg[t->seq_idx & 7] = (uint8_t)(val & 0xFF);
        if (t->seq_idx == 2) t->seq_map_mask = (uint8_t)(val & 0x0F);
        break;

    // ---- CRTC (0x3D4 index / 0x3D5 data, mono mirror 0x3B4/0x3B5) ----
    case 0x3D4:
    case 0x3B4:
        t->crtc_idx = (uint8_t)(val & 0x1F);
        if (width == 2) t->crtc[t->crtc_idx] = (uint8_t)(val >> 8);
        break;
    case 0x3D5:
    case 0x3B5:
        if (g_x86_dbgring &&
            (t->crtc_idx == 0x18 || t->crtc_idx == 0x07 || t->crtc_idx == 0x09)) {
            static int nlc = 0;
            if (nlc < 24) { nlc++;
                kprintf("[dos] CRTC %02x = %02x (line-compare group)\n",
                        t->crtc_idx, (unsigned)(val & 0xFF)); }
        }
        t->crtc[t->crtc_idx & 0x1F] = (uint8_t)(val & 0xFF);
        break;

    // ---- Misc Output register ----
    case 0x3C2:
        t->misc_out = (uint8_t)(val & 0xFF);
        break;

    // ---- EGA graphics controller (0x3CE index / 0x3CF data) ----
    case 0x3CE:
    case 0x3CF: {
        uint8_t d;
        if (port == 0x3CE) {
            t->gc_idx = (uint8_t)(val & 0xFF);
            if (width != 2) break;        // index-only write
            d = (uint8_t)(val >> 8);      // word OUT: high byte is the data
        } else {
            d = (uint8_t)(val & 0xFF);
        }
        switch (t->gc_idx) {
        case 0: t->gc_set_reset      = d & 0x0F; break;
        case 1: t->gc_en_set_reset   = d & 0x0F; break;
        case 2: t->gc_color_cmp      = d & 0x0F; break;
        case 3: t->gc_data_rotate    = d & 0x1F; break;
        case 4: t->gc_read_map       = d & 0x03; break;
        case 5: t->gc_mode           = d;        break;
        case 6: t->gc_misc           = d;        break;
        case 7: t->gc_color_dont_care= d & 0x0F; break;
        case 8: t->gc_bit_mask       = d;        break;
        }
        break;
    }

    // ---- EGA attribute controller (0x3C0 index+data, shared via flip-flop) ----
    case 0x3C0:
        if (t->atc_flipflop == 0) {
            t->atc_idx = (uint8_t)(val & 0x1F);   // bit5 = palette-address-source
            t->atc_flipflop = 1;
        } else {
            uint8_t d = (uint8_t)(val & 0xFF);
            if ((t->atc_idx & 0x1F) < 16)
                t->atc_pal[t->atc_idx & 0x0F] = d & 0x3F;
            else
                t->atc_reg[t->atc_idx & 0x1F] = d;
            if (g_x86_dbgring && (t->atc_idx & 0x1F) >= 0x10) {
                static int natc = 0;
                if (natc < 24) { natc++;
                    kprintf("[dos] ATC reg %02x = %02x\n", t->atc_idx & 0x1F, d); }
            }
            t->atc_flipflop = 0;
        }
        break;

    default:
        break;
    }
}

// Present EGA mode 0Dh: combine the 4 planes into 4-bit pixels, map through the
// attribute-controller palette + DAC, scale into the host window.
static void dos_present_ega(dos_task_t *t) {
    int W = t->gfx_w ? t->gfx_w : MODE13_W;
    int H = t->gfx_h ? t->gfx_h : MODE13_H;
    int sw = t->win_w, sh = t->win_h;
    // #385: honour the CRTC logical line width (Offset reg 0x13, in words) and the
    // display start address (0x0C hi / 0x0D lo, in the same word units). The id
    // Galaxy engine draws into an offscreen page in a virtual screen that is
    // WIDER than the visible 320 px (for smooth scrolling) and pans by moving the
    // start address, so a present that assumes width=40 bytes @ offset 0 shows a
    // sheared/garbled image.
    // #385: the id Galaxy engine renders into a virtual screen whose scanline
    // stride is the CRTC Offset register (0x13, in WORDS -> *2 bytes) and displays
    // a page selected by the start-address register (0x0C hi/0x0D lo). In EGA
    // planar mode that address is a direct BYTE offset into each plane (NOT
    // doubled). A present that assumes 40 bytes/row @ offset 0 shears the image
    // and shows the wrong (often empty) page.
    int bytes_per_row = t->crtc[0x13] ? (t->crtc[0x13] * 2) : (W / 8);
    // EGA/VGA SPLIT SCREEN. Line Compare is spread over three registers, and
    // none of them were read: CRTC 0x18 holds bits 0-7, CRTC 0x07 (Overflow)
    // bit 4 is bit 8, CRTC 0x09 (Maximum Scan Line) bit 6 is bit 9. Below that
    // scanline the CRTC restarts fetching at address 0 with panning reset, which
    // is how a scrolling game pins a status bar to the screen. Without it the
    // status area scrolls with the playfield and shows playfield memory.
    uint32_t line_compare = (uint32_t)t->crtc[0x18]
                          | ((uint32_t)(t->crtc[0x07] & 0x10) << 4)
                          | ((uint32_t)(t->crtc[0x09] & 0x40) << 3);
    // Horizontal pixel panning (ATC index 0x13), 0-7 pixels in the planar modes.
    uint32_t pan = t->atc_reg[0x13] & 0x07;
    uint32_t start_off = ((uint32_t)t->crtc[0x0C] << 8) | t->crtc[0x0D];
    // 16-entry ARGB LUT. atc_pal[i] selects a DAC entry, and the whole 6-bit DAC
    // space is seeded at mode set, so an all-zero entry now means the program
    // really did ask for black. The old "if the entry is black, substitute the
    // default colour for attribute i" fallback existed only to paper over the
    // unseeded DAC, and it actively corrupted any palette that deliberately maps
    // a colour to black.
    uint32_t lut[16];
    if (g_x86_dbgring) {
        // Diagnostic: the ATC palette registers and the DAC entries they select.
        // "wrong colours" in a planar mode is always one of these two tables.
        static uint8_t last[16];
        int chg = 0;
        for (int i = 0; i < 16; i++) if (last[i] != t->atc_pal[i]) chg = 1;
        if (chg) {
            for (int i = 0; i < 16; i++) last[i] = t->atc_pal[i];
            kprintf("[dos] ATC pal:");
            for (int i = 0; i < 16; i++) kprintf(" %02x", t->atc_pal[i]);
            kprintf("\n[dos] DAC via ATC:");
            for (int i = 0; i < 16; i++) {
                uint8_t d = t->atc_pal[i] & 0x3F;
                kprintf(" %d/%d/%d", t->pal[d][0], t->pal[d][1], t->pal[d][2]);
            }
            kprintf("\n");
        }
    }
    for (int i = 0; i < 16; i++) {
        uint8_t di = t->atc_pal[i] & 0x3F;
        uint8_t r6 = t->pal[di][0], g6 = t->pal[di][1], b6 = t->pal[di][2];
        uint32_t r = (uint32_t)r6 * 255 / 63;
        uint32_t g = (uint32_t)g6 * 255 / 63;
        uint32_t b = (uint32_t)b6 * 255 / 63;
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * H / sh;
        if (sy >= H) sy = H - 1;
        uint32_t *drow = t->win_buf + (size_t)dy * sw;
        // Below the split, the hardware refetches from address 0 with no panning.
        int split = ((uint32_t)sy > line_compare);
        uint32_t rowoff = split ? ((uint32_t)(sy - (int)line_compare - 1) * bytes_per_row)
                                : (start_off + (uint32_t)sy * bytes_per_row);
        uint32_t rowpan = split ? 0u : pan;
        for (int dx = 0; dx < sw; dx++) {
            int sx = dx * W / sw + (int)rowpan;
            if (sx >= W) sx = W - 1;
            uint32_t bo = (rowoff + (sx >> 3)) & (EGA_PLANE_SIZE - 1);
            int bit = 7 - (sx & 7);
            int pix = ((t->ega_plane[0][bo] >> bit) & 1)
                    | (((t->ega_plane[1][bo] >> bit) & 1) << 1)
                    | (((t->ega_plane[2][bo] >> bit) & 1) << 2)
                    | (((t->ega_plane[3][bo] >> bit) & 1) << 3);
            drow[dx] = lut[pix];
        }
    }
}

// ---- present: 0xA0000 (320x200x8) -> ARGB host window (scaled 2x) ---------
// #725 DIAGNOSTIC (gated on /CONFIG/DOSDIAG.CFG, off in the golden). dos_present()
// renders ONLY mode 13h and the EGA planar modes; TEXT mode 03h is not drawn at
// all, so a DOS program sitting at a text prompt shows a blank grey window and
// there is no way to read what it said. Dump the 80x25 page at B800:0000 to
// serial once per distinct page content, so a text-mode stop is diagnosable.
// This does not render anything; see the separate ticket for a real text-mode
// path in dos_present().
static void dos_dump_text_page(dos_task_t *t) {
    static uint32_t last_hash = 0;
    static int dumps = 0;
    if (dumps >= 8) return;                 // bounded: never floods the log
    uint32_t h = 2166136261u;
    for (int i = 0; i < 80 * 25 * 2; i += 2) h = (h ^ t->mem[0xB8000 + i]) * 16777619u;
    if (h == last_hash) return;
    last_hash = h; dumps++;
    kprintf("[dos] --- TEXT PAGE B800 (80x25) dump %d ---\n", dumps);
    for (int row = 0; row < 25; row++) {
        char line[81];
        int any = 0;
        for (int col = 0; col < 80; col++) {
            uint8_t ch = t->mem[0xB8000 + (row * 80 + col) * 2];
            if (ch < 32 || ch > 126) ch = (ch == 0) ? ' ' : '.';
            if (ch != ' ') any = 1;
            line[col] = (char)ch;
        }
        line[80] = 0;
        int end = 79; while (end >= 0 && line[end] == ' ') line[end--] = 0;
        if (any) kprintf("[dos] |%s\n", line);
    }
    kprintf("[dos] --- end text page ---\n");
}


// ---- present: text mode 03h (80x25 char+attr at B800) -> ARGB window -----
// 80x25 cells of the shared 8x16 bitmap font is exactly 640x400, which is also
// what mode 13h occupies after its 2x scale, so this uses the same logical
// surface and the same nearest-neighbour scale into the host content rect.
// LIMITS, stated rather than hidden: page 0 only, and attribute bit 7 is
// treated as "ignore" (no blink, no bright background), which is how a
// non-blinking VGA text screen looks and is what the DOSBox reference shows.
static const uint32_t cga_argb[16] = {
    0xFF000000u, 0xFF0000AAu, 0xFF00AA00u, 0xFF00AAAAu,
    0xFFAA0000u, 0xFFAA00AAu, 0xFFAA5500u, 0xFFAAAAAAu,
    0xFF555555u, 0xFF5555FFu, 0xFF55FF55u, 0xFF55FFFFu,
    0xFFFF5555u, 0xFFFF55FFu, 0xFFFFFF55u, 0xFFFFFFFFu,
};

static void dos_present_text(dos_task_t *t) {
    const int LW = TEXT_COLS * FONT_WIDTH;      // 640
    const int LH = TEXT_ROWS * FONT_HEIGHT;     // 400
    int sw = t->win_w, sh = t->win_h;
    // The cell lookup and glyph fetch are done once per source COLUMN, not once
    // per destination pixel: at 640x400 that is 80 lookups a row, not 640.
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * LH / sh; if (sy >= LH) sy = LH - 1;
        int row = sy >> 4, gy = sy & 15;
        uint32_t *drow = t->win_buf + (size_t)dy * sw;
        const uint8_t *cells = &t->mem[dos_text_cell(row, 0)];
        int last_col = -1;
        uint8_t bits = 0; uint32_t fg = 0, bg = 0;
        int cursor_row = (row == t->cur_row && gy >= FONT_HEIGHT - 2);
        for (int dx = 0; dx < sw; dx++) {
            int sx = dx * LW / sw; if (sx >= LW) sx = LW - 1;
            int col = sx >> 3;
            if (col != last_col) {
                uint8_t ch = cells[col * 2], at = cells[col * 2 + 1];
                bits = font_get_glyph_cp437(ch)[gy];
                fg = cga_argb[at & 0x0F];
                bg = cga_argb[(at >> 4) & 0x07];
                if (cursor_row && col == t->cur_col) bits = 0xFF;
                last_col = col;
            }
            drow[dx] = (bits & (0x80 >> (sx & 7))) ? fg : bg;
        }
    }
}

static void dos_present(dos_task_t *t) {
    if (!t->win_buf) return;
    // Mode 3 is the power-on default, so a program that never calls INT 10h
    // set-mode leaves video_mode at 0x00 and the dump used to never fire for
    // exactly the programs that most needed it.
    if (g_x86_dbgring && dos_text_is(t))
        dos_dump_text_page(t);
    if (dos_text_is(t)) { dos_present_text(t); return; }
    if (t->video_mode == 0x0D || t->video_mode == 0x0E ||
        t->video_mode == 0x10 || t->video_mode == 0x12) {
        dos_present_ega(t);
        return;
    }
    if (t->video_mode != 0x13) return;
    int sw = t->win_w, sh = t->win_h;             // host content size (>=640x400)
    const uint8_t *vga = &t->mem[VGA_A000];
    // Build an ARGB LUT from the 6-bit palette.
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)t->pal[i][0] * 255 / 63;
        uint32_t g = (uint32_t)t->pal[i][1] * 255 / 63;
        uint32_t b = (uint32_t)t->pal[i][2] * 255 / 63;
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    // Nearest-neighbour scale 320x200 into sw x sh.
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * MODE13_H / sh;
        if (sy >= MODE13_H) sy = MODE13_H - 1;
        const uint8_t *srow = vga + sy * MODE13_W;
        uint32_t *drow = t->win_buf + (size_t)dy * sw;
        for (int dx = 0; dx < sw; dx++) {
            int sx = dx * MODE13_W / sw;
            if (sx >= MODE13_W) sx = MODE13_W - 1;
            drow[dx] = lut[srow[sx]];
        }
    }
}

// ---- input forwarding: kernel cursor/keys -> DOS mouse state -------------
static void dos_pump_input(dos_task_t *t) {
    int ox, oy, ow, oh;
    if (win16_host_content_rect(t->host_slot, &ox, &oy, &ow, &oh) == 0 && ow > 0 && oh > 0) {
        int cx = (int)mouse_x - ox;
        int cy = (int)mouse_y - oy;
        if (cx < 0) cx = 0;
        if (cx >= ow) cx = ow - 1;
        if (cy < 0) cy = 0;
        if (cy >= oh) cy = oh - 1;
        // map content coords -> mode-13h virtual coords (0..639 x, 0..199 y typical)
        t->mx = cx * (MODE13_W * 2) / ow;   // DOS mouse uses 640-wide virtual range
        t->my = cy * MODE13_H / oh;
        int b = 0;
        if (mouse_buttons & 0x01) b |= 0x01;   // left
        if (mouse_buttons & 0x02) b |= 0x02;   // right
        t->mbtn = b;
    }
}

// ---- INT 9 (keyboard IRQ) delivery (#202 Keen) ---------------------------
// We have no real IRQs in the interpreter, so we synthesize them: when a raw
// scancode is available and the guest installed its own INT 9 handler, we latch
// the scancode at the emulated port 0x60, build a hardware-interrupt frame
// (push FLAGS, CS, IP) on the guest stack, and vector to the handler. The
// handler reads port 0x60, updates its Keyboard[] state, ACKs the PIC (we
// ignore the OUT to 0x20) and IRETs back to the interrupted code. We deliver at
// most a few per slice so a key cannot starve the game loop.
static void dos_push16(dos_task_t *t, uint16_t v) {
    t->cpu.sp = (uint16_t)(t->cpu.sp - 2);
    wr16(t, t->cpu.ss, t->cpu.sp, v);
}

// Has the GUEST taken ownership of interrupt vector `vec`?
//
// DERIVED FROM THE IVT, not latched when a particular API is called. This used
// to be set only inside INT 21h AH=25h (Set Interrupt Vector), which assumes
// every program installs handlers through DOS. Most do not: hooking an IRQ by
// writing the vector table directly
//
//     cli / mov es,0 / mov word ptr es:[9*4],offset / mov es:[9*4+2],cs / sti
//
// is the ordinary idiom and leaves no trace in any INT 21h call. "Invasion of
// the Mutant Space Bats of Doom" does exactly that, so kbd_has_int9 and
// has_int8 both stayed 0 and the DOS layer delivered it NEITHER keyboard IRQs
// NOR timer IRQs. Its main menu could not be operated at all: the cursor did
// not move for 3 DOWN presses, and PLAY did nothing, which reads exactly like
// "it gets to the game start screen but the game doesn't start". The game was
// not stuck; it was never given an interrupt.
//
// Asking the IVT instead makes the mechanism independent of HOW the guest
// installed the handler, and it also lets the flag clear again when a program
// restores the vector on exit. Every unhooked vector was seeded to the
// F000:FF53 IRET stub when the image was loaded (see the IVT seeding in
// dos_run_file), so "not the stub and not null" means the guest owns it.
static int dos_vec_hooked(dos_task_t *t, uint8_t vec) {
    uint16_t off = rd16(t, 0x0000, (uint16_t)(vec * 4));
    uint16_t seg = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
    if (seg == 0x0000 && off == 0x0000) return 0;   // null: nothing installed
    if (seg == 0xF000 && off == 0xFF53) return 0;   // our seeded IRET stub
    return 1;
}

// Re-derive the hooked-vector state. Cheap (four 16-bit guest reads), called
// once per interpreter burst, so a vector hooked mid-run is picked up within
// one DOS_SLICE_MS.
static void dos_refresh_vector_hooks(dos_task_t *t) {
    int k9 = dos_vec_hooked(t, 0x09);
    if (k9 != t->kbd_has_int9) {
        t->kbd_has_int9 = k9;
        // The tap is a MIRROR in the IRQ1 ISR (cpu/isr.c): it pushes the raw
        // scancode for the guest ISR and still calls keyboard_process_scancode(),
        // so the BIOS buffer that INT 16h reads keeps working at the same time.
        if (k9) { dos_scancode_clear(); dos_keyq_reset(t); }
        kprintf("[dos] INT 9 %s by guest -> %04x:%04x (raw kbd %s)\n",
                k9 ? "hooked" : "released",
                rd16(t, 0x0000, 0x0026), rd16(t, 0x0000, 0x0024),
                k9 ? "enabled" : "disabled");
    }
    int k8 = dos_vec_hooked(t, 0x08);
    if (k8 != t->has_int8) {
        t->has_int8 = k8;
        kprintf("[dos] INT 8 %s by guest -> %04x:%04x (timer IRQ %s)\n",
                k8 ? "hooked" : "released",
                rd16(t, 0x0000, 0x0022), rd16(t, 0x0000, 0x0020),
                k8 ? "enabled" : "disabled");
    }
    int k1c = dos_vec_hooked(t, 0x1C);
    if (k1c != t->has_int1c) {
        t->has_int1c = k1c;
        kprintf("[dos] INT 1Ch %s by guest -> %04x:%04x (BIOS user tick %s)\n",
                k1c ? "hooked" : "released",
                rd16(t, 0x0000, 0x0072), rd16(t, 0x0000, 0x0070),
                k1c ? "enabled" : "disabled");
    }
}

// Deliver a synthesized hardware interrupt `vec` to the guest: push the IRET
// frame and vector to the installed handler, then run a bounded burst so the
// handler runs and IRETs back to the interrupted code.
static void dos_deliver_int(dos_task_t *t, uint8_t vec, unsigned long budget) {
    x86_16_cpu_t *c = &t->cpu;
    uint16_t voff = rd16(t, 0x0000, (uint16_t)(vec * 4));
    uint16_t vseg = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
    if (vseg == 0 && voff == 0) return;
    dos_push16(t, c->flags);
    dos_push16(t, c->cs);
    dos_push16(t, c->ip);
    c->flags &= ~0x0200;   // CLI during ISR
    c->cs = vseg;
    c->ip = voff;
    x86_16_run(&t->cpu, budget);
}
static void dos_deliver_int9(dos_task_t *t) {
    if (!t->kbd_has_int9) return;
    x86_16_cpu_t *c = &t->cpu;
    int delivered = 0;
    while (delivered < 8) {
        int sc = dos_scancode_get();
        if (sc < 0) break;
        t->kbd_port60 = (uint8_t)sc;
        // read the guest INT 9 vector (IVT entry 9 -> linear 0x24).
        uint16_t voff = rd16(t, 0x0000, 0x0024);
        uint16_t vseg = rd16(t, 0x0000, 0x0026);
        if (vseg == 0 && voff == 0) break;
        // push hardware-interrupt frame: FLAGS, CS, IP (IRET pops IP, CS, FLAGS).
        dos_push16(t, c->flags);
        dos_push16(t, c->cs);
        dos_push16(t, c->ip);
        c->flags &= ~0x0200;   // CLI during ISR (IF cleared)
        c->cs = vseg;
        c->ip = voff;
        // Run the handler to completion. The IRET we pushed for restores cs:ip,
        // so the burst returns to the interrupted code. Keyboard ISRs are tiny.
        x86_16_run(&t->cpu, 20000);
        delivered++;
        if (t->cpu.halted) break;
    }
}

// ---- MZ / COM loader -----------------------------------------------------
// Returns 0 and sets initial cpu regs, <0 on error.
static int dos_load_image(dos_task_t *t, const uint8_t *f, uint32_t size) {
    if (size >= 2 && f[0] == 'M' && f[1] == 'Z') {
        // MZ header fields (all little-endian words):
        uint16_t bytes_last = f[2]  | (f[3]  << 8);   // bytes in last page
        uint16_t pages      = f[4]  | (f[5]  << 8);   // 512-byte pages
        uint16_t nreloc     = f[6]  | (f[7]  << 8);
        uint16_t hdr_para   = f[8]  | (f[9]  << 8);   // header size in paragraphs
        uint16_t ss         = f[14] | (f[15] << 8);   // initial SS (relative)
        uint16_t sp         = f[16] | (f[17] << 8);
        uint16_t ip         = f[20] | (f[21] << 8);
        uint16_t cs         = f[22] | (f[23] << 8);   // initial CS (relative)
        uint16_t reloc_off  = f[24] | (f[25] << 8);

        uint32_t hdr_bytes = (uint32_t)hdr_para * 16;
        uint32_t img_bytes = (uint32_t)pages * 512;
        if (bytes_last) img_bytes = img_bytes - 512 + bytes_last;
        if (img_bytes > size) img_bytes = size;
        uint32_t load_bytes = (img_bytes > hdr_bytes) ? (img_bytes - hdr_bytes) : 0;

        // Copy program image to DOS_LOAD_SEG:0000.
        uint32_t base_lin = (uint32_t)DOS_LOAD_SEG << 4;
        if (base_lin + load_bytes > VGA_A000) {
            kprintf("[dos] image too large (%u bytes)\n", load_bytes);
            return -1;
        }
        for (uint32_t i = 0; i < load_bytes; i++)
            t->mem[base_lin + i] = f[hdr_bytes + i];

        // Apply relocations: each is a word offset + word segment, relative to
        // the load segment. Add DOS_LOAD_SEG to the word at that location.
        for (uint16_t r = 0; r < nreloc; r++) {
            uint32_t e = reloc_off + (uint32_t)r * 4;
            if (e + 4 > size) break;
            uint16_t roff = f[e]   | (f[e + 1] << 8);
            uint16_t rseg = f[e + 2] | (f[e + 3] << 8);
            uint16_t fixseg = (uint16_t)(DOS_LOAD_SEG + rseg);
            uint16_t cur = rd16(t, fixseg, roff);
            wr16(t, fixseg, roff, (uint16_t)(cur + DOS_LOAD_SEG));
        }

        // alloc bump starts ABOVE the whole program block. The program's own
        // stack (SS:SP) usually sits high inside its block, above the image, so
        // the free pool for INT 21h 48h must begin past max(image_end, stack_top).
        uint16_t img_end_para  = (uint16_t)(DOS_LOAD_SEG + ((load_bytes + 15) >> 4));
        uint16_t stack_seg     = (uint16_t)(DOS_LOAD_SEG + ss);
        uint16_t stack_top_para = (uint16_t)(stack_seg + ((sp + 15) >> 4) + 1);
        uint16_t top = (img_end_para > stack_top_para) ? img_end_para : stack_top_para;
        t->alloc_top_para = (uint16_t)(top + 0x10);
        // The bump may never drop below this, whatever the guest asks for.
        t->alloc_floor_para = t->alloc_top_para;
        // Seed the table with the program's OWN block so a 4Ah on ES=PSP resizes
        // a real record instead of being answered with a lie. Its first 4Ah is
        // still answered 0xA000-ES, which is correct: nothing else is live yet.
        t->mcb_n = 0;
        dos_mcb_add(t, DOS_PSP_SEG, (uint16_t)(t->alloc_floor_para - DOS_PSP_SEG));

        t->cpu.cs = (uint16_t)(DOS_LOAD_SEG + cs);
        t->cpu.ip = ip;
        t->cpu.ss = (uint16_t)(DOS_LOAD_SEG + ss);
        t->cpu.sp = sp;
        t->cpu.ds = DOS_PSP_SEG;
        t->cpu.es = DOS_PSP_SEG;
        kprintf("[dos] MZ loaded: img=%u reloc=%u entry=%04x:%04x ss:sp=%04x:%04x\n",
                load_bytes, nreloc, t->cpu.cs, t->cpu.ip, t->cpu.ss, t->cpu.sp);
        return 0;
    }

    // .COM: load at PSP:0100, all segs = PSP.
    uint32_t n = size; if (n > 0xFE00) n = 0xFE00;
    uint32_t base_lin = ((uint32_t)DOS_PSP_SEG << 4) + 0x100;
    for (uint32_t i = 0; i < n; i++) t->mem[base_lin + i] = f[i];
    t->cpu.cs = t->cpu.ds = t->cpu.es = t->cpu.ss = DOS_PSP_SEG;
    t->cpu.ip = 0x100;
    t->cpu.sp = 0xFFFE;
    t->alloc_top_para = DOS_PSP_SEG + 0x1000;
    t->alloc_floor_para = t->alloc_top_para;
    t->mcb_n = 0;
    dos_mcb_add(t, DOS_PSP_SEG, (uint16_t)(t->alloc_floor_para - DOS_PSP_SEG));
    kprintf("[dos] COM loaded: %u bytes at %04x:0100\n", n, DOS_PSP_SEG);
    return 0;
}

// Build a minimal PSP at DOS_PSP_SEG.
static void dos_build_psp(dos_task_t *t) {
    // PSP[0..1] = INT 20h (CD 20), PSP[0x80] = cmdline length, PSP[0x81]= CR.
    wr8(t, DOS_PSP_SEG, 0x00, 0xCD);
    wr8(t, DOS_PSP_SEG, 0x01, 0x20);
    wr16(t, DOS_PSP_SEG, 0x02, 0x9FFF);  // top of memory segment
    int cl = 0;
    while (g_dos_cmdtail[cl] && cl < 120) cl++;
    wr8(t, DOS_PSP_SEG, 0x80, (uint8_t)(cl ? cl + 1 : 0));  // length includes the leading space
    if (cl) {
        wr8(t, DOS_PSP_SEG, 0x81, ' ');
        for (int i = 0; i < cl; i++)
            wr8(t, DOS_PSP_SEG, (uint16_t)(0x82 + i), (uint8_t)g_dos_cmdtail[i]);
        wr8(t, DOS_PSP_SEG, (uint16_t)(0x82 + cl), 0x0D);
    } else {
        wr8(t, DOS_PSP_SEG, 0x81, 0x0D);
    }
}

// ---- run -----------------------------------------------------------------
int dos_run_file(const char *path) {
    dos_task_t *t = &g_dos;
    memset(t, 0, sizeof(*t));

    t->mem = (uint8_t *)kmalloc(DOS_MEM_SIZE);
    if (!t->mem) { kprintf("[dos] OOM allocating 1MB\n"); return -1; }
    memset(t->mem, 0, DOS_MEM_SIZE);

    // appdir from path
    {
        int last = -1;
        for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
        if (last <= 0) { t->appdir[0] = '/'; t->appdir[1] = '\0'; }
        else {
            int n = last; if (n > (int)sizeof(t->appdir) - 1) n = sizeof(t->appdir) - 1;
            for (int i = 0; i < n; i++) t->appdir[i] = path[i];
            t->appdir[n] = '\0';
        }
    }

    // PIT power-on state: lobyte/hibyte access, divisor 0 (= 65536, 18.2 Hz).
    // memset(0) would leave pit_access == 0, which means "latch command".
    t->pit_access = 3;
    t->pit_divisor = 0;
    // Emulated timebase starts at zero for this run.
    t->emu_pit_base = 0;
    t->emu_insn_base = 0;
    t->next_irq0_pit = 0;
    t->bios_tick_last = 0;

    // FIX 4: seed this drive's CWD from the app directory so INT 21h 47h and
    // "X:NAME" resolution name the SAME namespace. Without this a program that
    // does the standard getcwd-then-build-absolute-path could not open its own
    // files: 47h answered "DOS\\PRINCE" (native root) and the resulting
    // "C:\\DOS\\PRINCE\\FOO" mapped to /WINDIR/DRIVE_C/DOS/PRINCE/FOO, which does
    // not exist.
    // #736: bind the machine to a service context BEFORE anything gated runs.
    dos_svc_bind(t);

    // FIX 4: seed THIS GUEST's CWD from the app directory so INT 21h 47h and
    // "X:NAME" resolution name the SAME namespace. The store is now PRIVATE to
    // this context (dos_svc_ctx_t), so a Win16 app started afterwards can no
    // longer inherit the game's directory: that cross-guest leak used to be
    // patched by clearing the shared store at teardown, and is now impossible.
    {
        const char *d = t->appdir;
        if (*d == '/') d++;
        t->svc.cwd_set(&t->svc, t->svc.cur_drive, d);
    }

    // #708: reading the guest's own image is a filesystem access by the
    // launching user, not a free kernel action. Without this a uid-1000 caller
    // could pass /CONFIG/SHADOW to SYS_DOS_RUN and have the kernel slurp a
    // root-only file into a buffer on its behalf: the MZ/COM parse would fail,
    // but the read would already have happened.
    if (!dos_svc_allow(&t->svc, path, R_OK | X_OK, "launch: read program image")) {
        kprintf("[dos] launch of %s DENIED by the guest fs gate\n", path);
        kfree(t->mem); t->mem = NULL; return -1;
    }
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) {
        kprintf("[dos] cannot read %s\n", path);
        kfree(t->mem); return -1;
    }

    x86_16_init(&t->cpu, t->mem);
    // #736 Stage 1b: every hook below reads its task back out of the cpu it
    // was handed, instead of reaching for the g_dos file static. That static
    // was never the bug on its own; taking the cpu pointer and THROWING IT
    // AWAY was, because it made "whose guest is this?" unanswerable.
    t->cpu.owner = t;
    dos_build_psp(t);
    if (dos_load_image(t, (const uint8_t *)data, size) != 0) {
        kfree(data); kfree(t->mem); return -1;
    }
    kfree(data);

    // default grayscale palette so something is visible before the app sets it
    for (int i = 0; i < 256; i++) { t->pal[i][0] = t->pal[i][1] = t->pal[i][2] = (uint8_t)(i >> 2); }

    x86_16_set_int_handler(&t->cpu, dos_int_handler);
    x86_16_set_io_handlers(&t->cpu, dos_in, dos_out);
    // Route 0xA0000-0xAFFFF through the EGA planar emulation (#202).
    x86_16_set_mem_hook(&t->cpu, VGA_A000, VGA_A000_END, ega_mem_w, ega_mem_r);
    // EGA defaults until the game sets a mode.
    t->seq_map_mask = 0x0F; t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
    for (int i = 0; i < 16; i++) t->atc_pal[i] = (uint8_t)i;

    // Create the host window (compositor draws it). win16_host_create() takes the
    // OUTER size and the decoration eats the difference, so asking for 640x404
    // produced a 636x380 CONTENT rect: mode 13h was being scaled by 1.9875 x 1.9
    // instead of a clean 2x, and the 80x25 text grid lost one pixel column in 160
    // and one row in 20. At an 8x16 cell that is the difference between "Shareware"
    // and "Sharcwarc". Measure the decoration once and ask again, so the content is
    // EXACTLY 640x400: an integer 2x for 320x200 and whole 8x16 cells for 80x25.
    const int want_w = TEXT_COLS * FONT_WIDTH;    // 640 == MODE13_W * WIN_SCALE
    const int want_h = TEXT_ROWS * FONT_HEIGHT;   // 400 == MODE13_H * WIN_SCALE
    t->host_slot = win16_host_create("DOS", 80, 60, want_w, want_h,
                                     &t->win_buf, &t->win_w, &t->win_h, 0);
    if (t->host_slot >= 0 && (t->win_w != want_w || t->win_h != want_h)) {
        int dw = want_w - t->win_w, dh = want_h - t->win_h;
        win16_host_destroy(t->host_slot);
        t->host_slot = win16_host_create("DOS", 80, 60, want_w + dw, want_h + dh,
                                         &t->win_buf, &t->win_w, &t->win_h, 0);
    }
    if (t->host_slot < 0) {
        kprintf("[dos] host window create failed\n");
        x86_16_set_io_handlers(&t->cpu, 0, 0);
        kfree(t->mem); return -1;
    }
    // (#745) The X on THIS window must stop THIS guest. win16_host_create()
    // installs the Win16 close latch on everything it creates, and nothing on
    // the DOS side reads that latch, so before this line the button was inert:
    // the window would not close and the interpreter kept ~85% of the machine
    // until the 6-hour cap. Route it to the DOS stop flag, which the run loop
    // below tests once per burst.
    win16_host_route_close_to_dos(t->host_slot);
    kprintf("[dos] window slot=%d buf=%dx%d\n", t->host_slot, t->win_w, t->win_h);

    // Power-on video state: 80x25 colour text (mode 3) with a cleared page. This
    // is what a real machine hands a program, and it matters: video_mode used to
    // start at 0x00, so INT 10h AH=0Fh answered "mode 0, 40 columns" (40x25 BW)
    // to any program that asked what card it was running on.
    t->video_mode = 0x03;
    t->text_attr  = 0x07;
    dos_text_clear(t, 0x07);

    // Seed the BIOS data area: equipment word, base memory size (640KB), and the
    // timer-tick dword at 0040:006C (many DOS programs busy-wait on it for timing).
    wr16(t, 0x0040, 0x0013, 640);          // base memory in KB
    wr8 (t, 0x0040, 0x0049, 0x03);         // current video mode
    wr16(t, 0x0040, 0x004A, TEXT_COLS);    // columns on screen
    wr16(t, 0x0040, 0x004C, TEXT_COLS * TEXT_ROWS * 2); // page size in bytes
    wr16(t, 0x0040, 0x0063, 0x03D4);       // CRTC port base (colour)
    wr8 (t, 0x0040, 0x0084, TEXT_ROWS - 1);// rows on screen minus one (EGA+)
    wr8 (t, 0x0040, 0x0087, 0x60);         // EGA info: 256KB, EGA active, colour
    wr8 (t, 0x0040, 0x0088, 0x09);         // EGA feature bits / switch settings
    wr16(t, 0x0040, 0x0085, FONT_HEIGHT);  // character height in scan lines
    wr16(t, 0x0040, 0x006C, 0);            // timer ticks low
    wr16(t, 0x0040, 0x006E, 0);            // timer ticks high

    // #385: Seed the IVT with valid default handlers. Real BIOS/DOS point every
    // vector at a routine; a program that hooks a hardware IRQ (INT 8 timer,
    // INT 9 keyboard, INT 1C user-tick, ...) FIRST saves the previous vector via
    // INT 21h AH=35h and CHAINS to it (pushf; call far [old]). If we leave the
    // IVT zeroed, that saved "old handler" is 0000:0000 and the chain call
    // derails into the IVT. Point a small IRET stub at F000:FF53 (the classic
    // BIOS dummy-IRET address) and default every otherwise-empty vector to it, so
    // a chained call returns cleanly (IRET pops the pushf FLAGS + return CS:IP).
    {
        // IRET stub in the reserved BIOS ROM region (never touched by the game).
        wr8(t, 0xF000, 0xFF53, 0xCF);      // IRET
        for (int v = 0; v < 256; v++) {
            uint16_t off = rd16(t, 0x0000, (uint16_t)(v * 4));
            uint16_t seg = rd16(t, 0x0000, (uint16_t)(v * 4 + 2));
            if (seg == 0 && off == 0) {
                wr16(t, 0x0000, (uint16_t)(v * 4),     0xFF53);
                wr16(t, 0x0000, (uint16_t)(v * 4 + 2), 0xF000);
            }
        }
    }

    t->running = 1;
    // Raw scancode mirroring is on for the WHOLE run, not only once a guest
    // hooks INT 9: it is now also the source for INT 16h, which needs the scan
    // code and not just an ASCII byte. cpu/isr.c mirrors rather than diverts, so
    // the kernel's own keyboard path is unaffected.
    g_dos_scancode_tap = 1;
    dos_scancode_clear();
    dos_keyq_reset(t);
    {   /* #201 derail ring: only when /CONFIG/DOSDIAG.CFG is present */
        extern volatile int g_x86_dbgring;
        uint32_t _dz = 0;
        void *_dc = fat_read_file(&g_fat_fs, "/CONFIG/DOSDIAG.CFG", &_dz);
        if (_dc) { kfree(_dc); g_x86_dbgring = 1; }
        uint32_t _rz = 0;
        void *_rc = fat_read_file(&g_fat_fs, "/CONFIG/DOSRING.CFG", &_rz);
        if (_rc) {
            int n = 0;
            for (uint32_t i = 0; i < _rz; i++) {
                char ch = ((char *)_rc)[i];
                if (ch < '0' || ch > '9') break;
                n = n * 10 + (ch - '0');
            }
            kfree(_rc);
            if (n > 0) g_dos_ring_dump_n = n;
            g_dos_ring_on = 1;
            g_dos_trace21 = 1;
            x86_16_ring_enable(1);
            kprintf("[dos] instruction ring ARMED (dump %d at exit)\n", g_dos_ring_dump_n);
        }
    }
    // Run in slices so we can pump input + present frames between bursts.
    // Seed the burst from the seed rate; the first measurement (DOS_RATE_SAMPLE_MS
    // in) replaces it, and every one after that re-tunes it.
    unsigned long slice = (unsigned long)(DOS_EMU_INSN_HZ * DOS_SLICE_MS / 1000UL);
    int frames = 0;
    // Measure what the guest actually gets, and report it, so "the DOS layer is
    // slow" is a number rather than an impression.
    uint64_t rate_t0 = sched_now_ms();
    unsigned long rate_i0 = 0;
    unsigned long rate_acc = 0;      // insns since the last printed line
    uint64_t rate_acc_ms = 0;        // ms since the last printed line
    uint64_t run_t0 = rate_t0;       // wall clock at which this program started
    uint64_t last_present_ms = 0;    // present cadence, independent of the pacing
    int dbg_last_frame = -1;         // de-dup for the @frame trace below
    g_dos_emu_hz = 0;
    uint32_t bios_ticks = 0;
    uint16_t prev_cs = 0, prev_ip = 0, prev_cs2 = 0, prev_ip2 = 0;
    while (!t->cpu.halted && t->running) {
        // Which vectors does the guest own THIS pass? Derived from the IVT, so a
        // handler installed by a direct table write counts the same as one
        // installed through INT 21h 25h.
        dos_refresh_vector_hooks(t);
        // Feed INT 16h while the guest has no INT 9 handler of its own. When it
        // does have one, dos_deliver_int9() consumes the same raw stream, so
        // only one of the two ever drains it.
        if (!t->kbd_has_int9) dos_keyq_pump(t);
        dos_pump_input(t);
        dos_deliver_int9(t);   // synthesize keyboard IRQs for the guest ISR (#202)
        // TIMER INTERRUPTS COME FROM THE EMULATED CLOCK, NOT FROM THE SLICE.
        // IRQ0 fires when emulated time crosses the guest's OWN programmed PIT
        // period (divisor written to ports 0x43/0x40), so a game that asks for
        // 70 Hz gets 70 Hz whatever the host pacing is doing. id's Galaxy engine
        // (Keen 4/5/6) busy-waits on the TimeCount its INT 8 handler increments,
        // so this IS the game clock.
        {
            uint32_t div = t->pit_divisor ? t->pit_divisor : 65536u;
            uint64_t now_pit = dos_emu_pit_now(t);   // snapshot: the ISR advances it
            if (t->next_irq0_pit == 0) t->next_irq0_pit = now_pit + div;
            // Bounded catch-up. A host stall must not turn into a thousand queued
            // IRQ0s that then run the game forward at once; deliver at most a few
            // and resynchronise rather than accumulate debt.
            int fired = 0;
            while (now_pit >= t->next_irq0_pit && fired < 4) {
                if (!t->cpu.halted) {
                    if (t->has_int8) {
                        // The guest owns IRQ0. Its handler is then responsible
                        // for whatever chaining it wants, exactly as on real
                        // hardware.
                        dos_deliver_int(t, 0x08, 20000);
                    } else if (t->has_int1c) {
                        // WE are the BIOS INT 8 handler, and the last thing a
                        // real one does on every tick is `int 1Ch`. Nothing did
                        // that, so a program that hooks ONLY the user tick got
                        // no timer callback at all.
                        //
                        // This is not a corner case. INT 1Ch is the documented,
                        // supported way for an application to get a periodic
                        // callback without owning IRQ0 or having to chain to the
                        // previous handler, so it is what a well-behaved program
                        // uses. "Invasion of the Mutant Space Bats of Doom" hooks
                        // 1Ch and NOTHING else: measured on build 1737, its IVT
                        // read 08=f000:ff53 09=f000:ff53 16=f000:ff53 with
                        // 1C=0294:000e, and it made zero INT 16h calls and zero
                        // port 0x60 reads in a whole session. Its menu logic and
                        // its input polling both live in that handler, so with
                        // 1Ch never fired the game had no clock and no input:
                        // the selection diamond never moved and PLAY did
                        // nothing, which presents as "it gets to the game start
                        // screen but the game doesn't start".
                        //
                        // Rate: on real hardware the BIOS handler runs at
                        // whatever rate IRQ0 is programmed to and calls 1Ch every
                        // time, so this belongs here, inside the IRQ0 pacing,
                        // and not on the fixed 18.2 Hz tick-counter update
                        // below. BATS programs divisor 0x7FFF, i.e. 36.41 Hz.
                        dos_deliver_int(t, 0x1C, 20000);
                    }
                }
                t->next_irq0_pit += div;
                fired++;
            }
            if (now_pit >= t->next_irq0_pit)
                t->next_irq0_pit = now_pit + div;    // still behind: resync, drop the debt
            // BIOS timer tick at the true 18.2065 Hz: one per 65536 PIT ticks,
            // exactly the hardware relationship, instead of one per slice.
            uint32_t bt = (uint32_t)(dos_emu_pit_now(t) >> 16);
            if (bt != t->bios_tick_last) {
                t->bios_tick_last = bt;
                wr16(t, 0x0040, 0x006C, (uint16_t)(bt & 0xFFFF));
                wr16(t, 0x0040, 0x006E, (uint16_t)(bt >> 16));
            }
        }
        // #201 derail diagnosis: single-step near the known derail (~3.3M insns)
        // so the ring buffer captures the exact transfer into zeroed memory.
        if (0) g_dos_sstep = 1;   /* disabled: use interpreter g_x86_dbgring */
        int r;
        if (g_dos_sstep != 0) {
            // Single-step until the derail (op 00 00). Keep a ring of the last 24
            // instructions and dump it when control wanders into zeros, so we see
            // the exact transition that caused the derail (#202).
            g_dos_sstep = 0;
            #define SSRING 8000
            static char ring[SSRING][160];
            int rh = 0, filled = 0;
            r = 1;
            unsigned long guard = 0;
            for (;;) {
                if (t->cpu.halted) { r = 0; break; }
                uint8_t op0 = rd8(t, t->cpu.cs, t->cpu.ip);
                uint8_t op1 = rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip + 1));
                if (op0 == 0x00 && op1 == 0x00) {
                    kprintf("[dos] === DERAIL ring dump (oldest first) ===\n");
                    int start = filled ? rh : 0;
                    for (int k = 0; k < (filled ? SSRING : rh); k++)
                        kprintf("%s", ring[(start + k) % SSRING]);
                    kprintf("[dos] DERAIL at cs:ip=%04x:%04x es=%04x\n", t->cpu.cs, t->cpu.ip, t->cpu.es);
                    break;
                }
                snprintf(ring[rh], sizeof(ring[rh]),
                    "[dos] SS %04x:%04x sp=%04x bp=%04x op=%02x%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x ds=%04x es=%04x fl=%04x\n",
                    t->cpu.cs, t->cpu.ip, t->cpu.sp, t->cpu.bp, op0, op1,
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx,
                    t->cpu.si, t->cpu.di, t->cpu.ds, t->cpu.es, t->cpu.flags);
                rh = (rh + 1) % SSRING; if (rh == 0) filled = 1;
                r = x86_16_run(&t->cpu, 1);
                if (r < 0) break;
                if ((guard % 100000UL) == 0) {   // keep BIOS tick advancing like slice mode
                    bios_ticks++;
                    wr16(t, 0x0040, 0x006C, (uint16_t)(bios_ticks & 0xFFFF));
                    wr16(t, 0x0040, 0x006E, (uint16_t)(bios_ticks >> 16));
                }
                if (++guard > 6000000UL) { kprintf("[dos] sstep guard hit\n"); break; }
            }
            #undef SSRING
        } else {
            r = x86_16_run(&t->cpu, slice);
        }
        prev_cs2 = prev_cs; prev_ip2 = prev_ip;
        prev_cs = t->cpu.cs; prev_ip = t->cpu.ip;
        // PRESENT CADENCE, decoupled from the pacing. dos_present() is a full
        // 640x400 scale-and-convert blit (a divide per destination pixel); doing
        // it once per burst tied its cost to the burst size, so shortening the
        // burst would have spent the reclaimed CPU on redundant blits instead of
        // on the guest. 70 Hz is at or above the compositor's refresh, so nothing
        // is lost visually.
        {
            uint64_t pnow = sched_now_ms();
            if (pnow - last_present_ms >= DOS_PRESENT_MS || t->cpu.halted) {
                last_present_ms = pnow;
                dos_present(t);
                // AND THEN TELL THE WM. dos_present() only fills the window's
                // content buffer; the compositor blits that buffer when the
                // window's region is dirty, and nothing here was dirtying it.
                // Measured on build 1732 before this line existed: with BATS
                // running its game loop at 18.5 M insn/s and issuing ~1.5 M EGA
                // plane writes per second, three-page-flipping the CRTC display
                // start between 0x0500/0x2900/0x4d00, the WHOLE 1280x800
                // framebuffer was byte-identical over 3 seconds; a single
                // keystroke changed 27,456 pixels, all inside the DOS window,
                // and then it froze again. The game was never stuck: its frames
                // were not being presented. This is the same call a userland app
                // makes after painting (sys_win_invalidate), through the same
                // function, so the two cannot drift.
                if (t->host_slot >= 0) win16_host_invalidate(t->host_slot);
                frames++;
            }
        }
        // #385: periodic where-am-I so a busy-wait loop shows as a repeated cs:ip.
        // Gated on the frame COUNTER CHANGING, not just on its value: the
        // present is no longer once per pass, so `frames` now holds the same
        // value for many passes and the plain (frames & 0x3F) test fired the
        // same line several times in a row.
        if (g_x86_dbgring && frames != dbg_last_frame && (frames & 0x3F) == 0) {
            dbg_last_frame = frames;
            kprintf("[dos] @frame%d cs:ip=%04x:%04x op=%02x%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x ds=%04x es=%04x mode=%02x t=%u\n",
                    frames, t->cpu.cs, t->cpu.ip,
                    rd8(t, t->cpu.cs, t->cpu.ip), rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip+1)),
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx, t->cpu.si, t->cpu.di,
                    t->cpu.ds, t->cpu.es, t->video_mode, (unsigned)rd16(t,0x0040,0x006C));
        }
        // Periodic where-am-I trace (#202 diagnostics): every ~64 slices print
        // cs:ip + key VGA state so we can locate busy-wait loops during bring-up.
        // Runaway/derail detector: if the CPU is executing 0x00 opcodes (it has
        // wandered into zeroed memory) stop early so the log isn't flooded.
        if (rd8(t, t->cpu.cs, t->cpu.ip) == 0x00 &&
            rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip + 1)) == 0x00) {
            kprintf("[dos] DERAIL: zeros at cs:ip=%04x:%04x (prev=%04x:%04x prev2=%04x:%04x) ss:sp=%04x:%04x ds=%04x es=%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x insns=%lu\n",
                    t->cpu.cs, t->cpu.ip, prev_cs2, prev_ip2, prev_cs, prev_ip,
                    t->cpu.ss, t->cpu.sp, t->cpu.ds, t->cpu.es,
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx, t->cpu.si, t->cpu.di,
                    t->cpu.bp, t->cpu.insn_count);
            // Single-step the NEXT run from prev_cs2 region won't help (already
            // derailed); instead arm a re-run hint by dumping stack near sp.
            break;
        }
        if (r < 0) {
            kprintf("[dos] interpreter stop r=%d at %04x:%04x insns=%lu\n",
                    r, t->cpu.cs, t->cpu.ip, t->cpu.insn_count);
            break;
        }
        if (r == 0) break;   // halted normally
        // Safety cap so a runaway/busy-wait program cannot pin a CPU forever.
        // This was a FRAME count, which only meant a wall-clock bound while the
        // frame rate was pinned to the slice rate. With the present decoupled
        // from the pacing, the same constant would have meant a different amount
        // of time, so state it in the unit it always meant. Generous: a user
        // playing Keen must not be killed mid-session.
        if (sched_now_ms() - run_t0 > DOS_MAX_RUN_MS) {
            kprintf("[dos] run cap reached (%lu ms) insns=%lu mode=0x%02x\n",
                    (unsigned long)(sched_now_ms() - run_t0),
                    t->cpu.insn_count, t->video_mode);
            break;
        }
        // r == 1: the burst hit its instruction cap.
        //
        // THE YIELD DISCIPLINE. proc_yield() is a HANDOFF, not a wait: it puts
        // this thread back on the ready queue and runs the highest-priority
        // runnable thread. If the compositor (or anything else) is runnable it
        // runs NOW, which is why this is "yield on demand" and not a timer. If
        // the ready queue is empty the scheduler hands the core straight back,
        // so the 38-45% that used to go to hlt now goes to the guest.
        //
        // This is why the DOS proc runs at PRIO_NORMAL and not PRIO_HIGH (see
        // dos_launch()): the ready queue is strictly priority-ordered, so a
        // PRIO_HIGH yield re-inserts AHEAD of every peer and the handoff never
        // happens. At PRIO_HIGH the only thing that could ever dislodge a
        // never-sleeping DOS thread is the 500 ms anti-starvation sweep
        // (SCHED_STARVE_TICKS), i.e. a desktop that responds 2 times a second.
        //
        // concurrency-lint flags this as YIELD_SPIN and that is CORRECT and
        // WANTED: the site stays visible in allowlist.txt with its justification
        // rather than disappearing from review. It is not a wait: it never spins
        // on a condition, every pass executes >= DOS_SLICE_MIN guest
        // instructions of real forward progress, and the run is bounded by
        // DOS_MAX_RUN_MS above.
        proc_yield();

        {   // CLOSE THE LOOP: measure what the guest actually got, then re-size
            // the next burst to hit DOS_SLICE_MS of wall clock at that rate.
            uint64_t now = sched_now_ms();
            if (now - rate_t0 >= DOS_RATE_SAMPLE_MS) {
                unsigned long di = t->cpu.insn_count - rate_i0;
                uint64_t dt = now - rate_t0;
                uint32_t hz = (uint32_t)((uint64_t)di * 1000ull / dt);
                if (hz > 100000u) {          // ignore stalled samples
                    // Damped, because this rate is the PIT's time base: an
                    // undamped sample would make the guest's clock rate jitter
                    // with the host's scheduling noise.
                    uint32_t nh = g_dos_emu_hz
                        ? (uint32_t)(((uint64_t)g_dos_emu_hz * 3 + hz) / 4)
                        : hz;
                    dos_emu_rebase(t, nh);   // adopt WITHOUT moving past instants
                    unsigned long ns =
                        (unsigned long)(((uint64_t)nh * DOS_SLICE_MS) / 1000ull);
                    if (ns < DOS_SLICE_MIN) ns = DOS_SLICE_MIN;
                    if (ns > DOS_SLICE_MAX) ns = DOS_SLICE_MAX;
                    slice = ns;
                }
                rate_acc += di; rate_acc_ms += dt;
                if (rate_acc_ms >= 1000) {
                    if (g_x86_dbgring)
                        kprintf("[dos] rate %lu insn/s (slice=%lu target=%dms yield=on-demand)\n",
                                (unsigned long)(((uint64_t)rate_acc * 1000ull) / rate_acc_ms),
                                slice, DOS_SLICE_MS);
                    rate_acc = 0; rate_acc_ms = 0;
                }
                rate_t0 = now; rate_i0 = t->cpu.insn_count;
            }
        }
    }

    kprintf("[dos] '%s' finished exit=%d insns=%lu frames=%d mode=0x%02x\n",
            path, t->cpu.exit_code, t->cpu.insn_count, frames, t->video_mode);

    // Keep the final frame visible for a moment, then tear down. NOT when the
    // user asked to close: a window that sits there for two more seconds after
    // you click its X reads as a hang. t->running is still 1 on every self-exit
    // path (guest halted, interpreter error, run cap) and 0 only on a close
    // request, so the flag distinguishes the two without a second one.
    if (t->running) proc_sleep(2000);
    g_dos_scancode_tap = 0;
    // #736 Stage 1b: these now clear THIS task's cpu, not a process-wide slot,
    // so tearing a DOS guest down can no longer disarm a Win16 guest's hooks
    // (or, as it did, leave the DOS guest running on the Win16 guest's).
    x86_16_set_mem_hook(&t->cpu, 0, 0, 0, 0);
    x86_16_set_int_handler(&t->cpu, 0);
    x86_16_set_io_handlers(&t->cpu, 0, 0);
    win16_host_destroy(t->host_slot);
    kfree(t->mem); t->mem = NULL;
    // #736: close every handle the guest left open, COMMITTING anything dirty,
    // and print the service core's one-line usage/enforcement line. This runs
    // BEFORE guestfs_finish() disarms the identity slot, because a write-back
    // is a filesystem access and is gated like any other: disarming first
    // would silently lose the data the guest thought it had saved.
    dos_svc_ctx_close_all(&t->svc);
    dos_svc_report(&t->svc);
    // The per-drive CWD is now PRIVATE to t->svc and dies with the task, so
    // the old "clear the shared store so a later Win16 app does not inherit
    // this game's directory" teardown step is no longer needed: the leak it
    // patched cannot happen.
    g_dos_busy = 0;
    return t->cpu.exit_code;
}

// ---- async launch --------------------------------------------------------
static char g_dos_path[128];
static void dos_proc_entry(void *arg) {
    (void)arg;
    dos_run_file(g_dos_path);
    // #708: disarm the slot and print the enforcement report from the single
    // place every dos_run_file() exit reaches, rather than at each of its
    // early returns. After this the slot denies, so nothing that outlives the
    // guest can keep using its authority.
    guestfs_finish(GUESTFS_SLOT_DOS);
}

// ---- boot-gated launch (RC-independent test harness, #201/#276) ----------
// Reads /CONFIG/DOSRUN.CFG (a single path line, e.g. "/DOS/TIM/TIM.EXE") and
// launches it a few seconds after boot, so a DOS game can be brought up and its
// serial trace captured without depending on the RC channel or a GUI launcher.
static void dos_deferred_entry(void *arg) {
    (void)arg;
    kprintf("[dos] deferred_entry ENTERED (task385 keen), sleeping 3s\n");
    proc_sleep(3000);   // let the desktop/compositor come up first
    kprintf("[dos] deferred_entry past sleep (task385 keen)\n");
    // #385: the FAT config read can transiently fail in a post-boot proc context
    // (concurrent FS activity from widgets). Retry a bounded number of times and
    // trace each attempt so a failure is diagnosable rather than a silent return.
    void *cfg = 0; uint32_t sz = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        cfg = fat_read_file(&g_fat_fs, "/CONFIG/DOSRUN.CFG", &sz);
        kprintf("[dos] DOSRUN.CFG read attempt %d: cfg=%p sz=%u\n", attempt, cfg, sz);
        if (cfg && sz > 0) break;
        if (cfg) { kfree(cfg); cfg = 0; }
        proc_sleep(1000);
    }
    if (!cfg || sz == 0) { if (cfg) kfree(cfg); kprintf("[dos] DOSRUN.CFG unreadable, giving up\n"); return; }
    char path[128];
    int n = 0;
    const char *p = (const char *)cfg;
    uint32_t i = 0;
    for (; i < sz && n < (int)sizeof(path) - 1; i++) {
        char ch = p[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') break;
        path[n++] = ch;
    }
    path[n] = '\0';
    // Anything after the first space on the line is the DOS command tail.
    while (i < sz && (p[i] == ' ' || p[i] == '\t')) i++;
    int a = 0;
    for (; i < sz && a < (int)sizeof(g_dos_cmdtail) - 1; i++) {
        if (p[i] == '\r' || p[i] == '\n') break;
        g_dos_cmdtail[a++] = p[i];
    }
    g_dos_cmdtail[a] = '\0';
    if (a) kprintf("[dos] DOSRUN.CFG command tail = '%s'\n", g_dos_cmdtail);
    kfree(cfg);
    if (n == 0) { kprintf("[dos] DOSRUN.CFG empty path\n"); return; }
    kprintf("[dos] DOSRUN.CFG -> launching '%s'\n", path);
    // #708: no Ring-3 caller here, so this is the service launch. It runs as
    // the authenticated desktop session, and is REFUSED outright if nobody has
    // logged in yet (rather than silently running as root).
    dos_launch_kernel(path);
}

// (#745) Stop request from the window manager's titlebar X (see
// dos_host_close_handler in proc/syscall.c). This runs on the WM/compositor
// thread, so it does the one thing that is safe from there: clear the run
// flag. It touches neither the interpreter nor the window.
//
// t->running was written exactly once in the whole tree (to 1, at launch) and
// cleared nowhere, so until now the ONLY ways out of the run loop were the
// guest halting itself, an interpreter error, or DOS_MAX_RUN_MS = 6 hours.
// There was no way for a user to stop a DOS program at all.
//
// The run loop tests this at the top of every burst, so the guest stops within
// one DOS_SLICE_MS (4 ms) and then runs its OWN normal teardown, which frees
// the 1 MiB guest image, closes the guest's open handles, destroys the window
// and clears g_dos_busy so the next DOS program can launch. No wait queue is
// involved and none is wanted: the run loop is not waiting for anything, it is
// executing guest instructions, and this is a request to stop doing that.
void dos_request_close(void) { g_dos.running = 0; }

void dos_start_deferred_launch(void) {
    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/DOSRUN.CFG", &sz);
    if (!cfg) return;
    kfree(cfg);
    proc_create("dosrun", dos_deferred_entry, NULL, PRIO_HIGH);
}

// #708: the two launchers differ ONLY in where the guest's identity comes
// from, and they are separate functions for the reason win16_launch /
// win16_launch_kernel already are: the distinction is a property of the
// CALLER, and a caller cannot get it wrong if it cannot express it.
//
//   dos_launch()        syscall-facing (SYS_DOS_RUN). A Ring-3 process asked
//                       for this guest, so the guest runs as that process.
//   dos_launch_kernel() service-facing (/CONFIG/DOSRUN.CFG boot harness).
//                       There is no Ring-3 caller, so the guest runs as the
//                       authenticated desktop session, and NOT as root just
//                       because a kernel thread happened to start it.
//
// Both arm BEFORE proc_create(), while the launcher's context still exists: by
// the time the guest thread runs, proc_current() is a kernel thread whose uid
// is 0 by construction, which is exactly the identity that must not be used.
static int dos_launch_common(const char *path, int from_session) {
    if (g_dos_busy) { kprintf("[dos] busy (a DOS task is already running)\n"); return -1; }
    int rc = from_session ? guestfs_arm_session(GUESTFS_SLOT_DOS)
                          : guestfs_arm_caller(GUESTFS_SLOT_DOS);
    if (rc != 0) {
        // FAIL CLOSED AT THE LAUNCH, not merely at the first file access. A
        // guest with no resolvable identity has no business running: it would
        // start, render, and then fail every single file operation, which is a
        // far more confusing failure than refusing to start.
        kprintf("[dos] launch of '%s' REFUSED: no usable identity for the guest\n", path);
        return -1;
    }
    int i = 0;
    for (; i < (int)sizeof(g_dos_path) - 1 && path[i]; i++) g_dos_path[i] = path[i];
    g_dos_path[i] = '\0';
    g_dos_busy = 1;
    // PRIO_NORMAL, NOT PRIO_HIGH. The interpreter loop no longer sleeps between
    // bursts; it yields. The ready queue is strictly priority-ordered, so a
    // PRIO_HIGH thread that yields is re-inserted ahead of every peer and picked
    // straight back: the handoff would never happen and the compositor would
    // only ever run via the 500 ms anti-starvation sweep. At PRIO_NORMAL the
    // yield is a real round-robin handoff to any peer that wants the CPU, and
    // when nobody does the guest keeps the core. High priority was buying
    // scheduling order that a 4 ms yield cadence now gives without the
    // starvation risk.
    if (proc_create("dos", dos_proc_entry, NULL, PRIO_NORMAL) < 0) {
        g_dos_busy = 0;
        guestfs_disarm_rs(GUESTFS_SLOT_DOS);
        return -1;
    }
    kprintf("[dos] launched '%s'\n", g_dos_path);
    return 0;
}

int dos_launch(const char *path)        { return dos_launch_common(path, 0); }
int dos_launch_kernel(const char *path) { return dos_launch_common(path, 1); }
