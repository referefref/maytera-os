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
#include "dpmi.h"       // #740: THE DPMI host core (INT 31h)
#include "../fs/bootlog.h"   // #205: audiolog_write() -> /AUDIOLOG.TXT
#include "dpmi_rmcs.h"  // #740: DPMI 0300h + the guest-memory chokepoint
#include "dos4gw.h"
#include "doslinger.h"   // the post-exit linger policy (rustkern/doslinger.rs)
#include "dosfmq.h"      // (#fmbridge) THE FM event queue, behind a ring-neutral seam
#include "../exec/go32.h"     // #740: the DOS/4GW guest bridge (rustkern/dos4gw.rs)
#include "../exec/le.h"      // #740: LE parse/load
#include "../exec/x86_32.h"  // #740: the 32-bit protected-mode core
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"     // #708: R_OK/W_OK/X_OK
#include "../fs/guestfs.h"
#include "../proc/users.h"   // #221b: uid -> home for the %HOME% token   // #708: the DOS/Win16 guest filesystem gate
#include "../exec/x86_16.h"
#include "../video/font.h"
#include "../cpu/mono.h"   // sched_now_ms(): the shared monotonic clock
#include "../cpu/wallclock.h" // #234a: the ONE clock, in its DOS/BIOS shape
#include "../drivers/keyboard.h"  // KEY_MOD_* and the shared scancode-to-char table
#include "../sync/spinlock.h"     // the shared irqsave spinlock (host-window handover)
#include "../sync/waitq.h"        // #181: the ONE blocking primitive (#426)
#include "../gui/fb_syscall.h"    // g_fb_flip_wq: the "screen updated" wake source
#include "../drivers/audio.h"     // #181: audio_is_available(), the shared resampler
#include "../drivers/audio_pcm.h" // #181: the ONE PCM sink door, kernel side
#include "../drivers/usb_audio.h" // #181: uac_is_ready(), part of the sink test

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
// (#flipfix) The framebuffer present count, through the SAME narrow
// win16_host_* seam this file already uses for the window geometry, rather than
// as a direct read of gui/fb_syscall.c's g_fb_flip_count.
//
// WHY IT HAD TO STOP BEING A VARIABLE. These sources are compiled BYTE-
// IDENTICALLY into the Ring-3 DOS host (userland/apps/dosring3), which is the
// whole point of that port: one implementation of DOS semantics, so the two
// paths cannot drift into guest-visible disagreement. A Ring-3 process cannot
// read a kernel variable, and a variable READ cannot be turned into a syscall,
// so the shim had to define a symbol of that name - and it was a stub that
// nothing could ever write. dos_frame_due() below then saw a screen that had
// never once moved on, skipped every frame, and left the 200 ms staleness
// backstop as the only thing publishing anything: 5.005 flips/s measured,
// against 24.98 for the identical guest in-kernel.
//
// A CALL is answerable by different code in each ring with this file none the
// wiser. Ring 0 (proc/syscall.c) returns the same global this file used to
// read, so nothing about the in-kernel path changes but the call. Ring 3
// (dosring3/shim/kshim.c) asks the kernel through SYS_FB_FLIP_COUNT, which
// returns that same one global - there is still exactly one counter of this
// event in the system.
extern uint64_t win16_host_flip_count(void);

extern void win16_host_destroy(int slot);
extern void win16_host_route_close_to_dos(int slot);

// ---------------------------------------------------------------------------
// (no-ticket) THE POST-EXIT LINGER, and the two facts it is built on.
//
// This used to be `if (t->running) proc_sleep(2000);` at the top of the
// teardown, under a comment saying "keep the final frame visible for a
// moment". See rustkern/doslinger.rs for what was wrong with that and what
// the policy is now; the two pieces of KERNEL state it needs live here.
//
// g_dos_publish_flip / g_dos_published: the present counter at the moment the
// DOS layer last told the WM its window was dirty, and whether it ever did.
// "The final frame is on the glass" is exactly "the compositor has presented
// at least once SINCE that moment", and that is a condition, not a duration,
// so it can be waited for instead of slept through.
//
// One guest runs at a time (g_dos_busy enforces it), so file scope is the
// right scope and a torn read of the pair is harmless: the worst case is one
// exit paying the 250 ms backstop.
static volatile uint64_t g_dos_publish_flip = 0;
static volatile int      g_dos_published    = 0;

// Woken by dos_request_close() (the titlebar X). ALWAYS ARMED for the whole
// linger, which is the point: before this the X did nothing at all once the
// run loop had exited, so a self-exited guest's window sat there for two
// seconds refusing to close - the exact "reads as a hang" the old comment
// warned about for the other path.
static wait_queue_head_t g_dos_exit_wq = { .head = NULL, .lock = SPINLOCK_INIT };

// Record "the DOS layer has just published a frame". Called at every
// win16_host_invalidate() site (16-bit loop, halt path, and the DOS/4GW
// loop), so a 32-bit guest is not silently excluded the way a per-loop frame
// counter would have excluded it.
static inline void dos_publish_mark(void) {
    g_dos_publish_flip = win16_host_flip_count();
    g_dos_published    = 1;
}

// The linger itself. self_exit is 1 when the guest stopped by itself and 0
// when the user asked for the window to close.
//
// #426 SHAPE, stated explicitly because a reviewer should not have to infer
// it: there are two waits and NEITHER is a poll.
//   1. "has the final frame reached the screen" - a real condition with a
//      real, always-armed wake (sys_fb_flip wakes g_fb_flip_wq after every
//      present). The timeout is a BACKSTOP for a compositor that never
//      presents, which is a fault, so it is logged loudly and durably.
//   2. the deliberate visible hold - here the timeout IS the intent, not a
//      workaround for a missing wake, and the wake exists to CUT IT SHORT
//      when the user clicks the X.
// Neither is paced off timer_ticks: sched_now_ms() is the TSC-backed clock.
static void dos_exit_linger(int self_exit) {
    if (!dos_linger_wanted_rs(self_exit, g_dos_published ? 1u : 0u)) return;

    uint64_t mark = g_dos_publish_flip;
    uint64_t t0   = sched_now_ms();

    int rc = wait_event_timeout(&g_fb_flip_wq,
                                dos_linger_frame_done_rs(win16_host_flip_count() - mark),
                                wq_ms_to_ticks(dos_linger_frame_backstop_ms_rs()));
    if (rc != WAIT_OK) {
        // DURABLE, not just serial: serial is silent in GUI mode, and this
        // firing means the compositor did not present for a quarter of a
        // second while a window was dirty, which is worth knowing about on a
        // machine that has no serial cable.
        kprintf("[dos] exit linger: NO present within %u ms of the final frame "
                "(flip=%lu mark=%lu) - tearing down anyway\n",
                (unsigned)dos_linger_frame_backstop_ms_rs(),
                (unsigned long)win16_host_flip_count(), (unsigned long)mark);
        bootlog_write("[dos] exit linger: no present within %u ms of the final "
                      "frame (flip=%lu mark=%lu)",
                      (unsigned)dos_linger_frame_backstop_ms_rs(),
                      (unsigned long)win16_host_flip_count(),
                      (unsigned long)mark);
    }

    dos_linger_arm_hold_rs(sched_now_ms());
    (void)wait_event_timeout(&g_dos_exit_wq,
                             dos_linger_hold_done_rs(sched_now_ms()),
                             wq_ms_to_ticks(dos_linger_hold_ms_rs()));

    kprintf("[dos] exit linger %lu ms (frame-wait %s)\n",
            (unsigned long)(sched_now_ms() - t0),
            rc == WAIT_OK ? "ok" : "TIMED OUT");
}
// #156: does OUR host window currently hold compositor focus? See the
// definition in proc/syscall.c for why this is the one authoritative focus
// register rather than a second notion the DOS layer would have to keep in
// step by hand.
extern int  win16_host_is_focused(int slot);
extern void win16_host_focus(int slot);

// (#745 local 105) WHERE THE GUEST'S PICTURE GOES INSIDE A RESIZABLE WINDOW.
//
// The rectangle is decided in Rust (rustkern/doswin.rs), which also documents
// the aspect policy: LETTERBOX a fixed 8:5 logical screen, never stretch to
// fill. The pixel loops stay here in C because they read this file's private
// guest state (VGA RAM, the EGA planes, the DAC) and are the hot path.
typedef struct { int32_t x, y, w, h; } dos_rect_t;
_Static_assert(sizeof(dos_rect_t) == 16, "dos_rect_t must match Rust DosRect");
extern int dos_letterbox_rs(int32_t cw, int32_t ch, int32_t aw, int32_t ah,
                            dos_rect_t *out);
extern int dos_letterbox_selftest_rs(void);

// (#dosfs) HOW BIG the picture is, which dos_letterbox_rs() alone never asked.
// The policy, the pixel budget and the whole argument for both live in
// rustkern/doswin.rs; this is only the seam. Both the DRAW path and the INPUT
// path go through dos_present_geom() below, which is the single caller, for the
// same reason #745 made dos_letterbox_rs() the single geometry function: a
// scaled picture with unscaled input is worse than no scaling.
typedef struct { int32_t budget_px, integer, max_w, max_h, aspect, frameskip; } dos_view_policy_t;
_Static_assert(sizeof(dos_view_policy_t) == 24,
               "dos_view_policy_t must match Rust DosViewPolicy");
extern int dos_present_rect_rs(int32_t cw, int32_t ch, int32_t aw, int32_t ah,
                               int32_t gw, int32_t gh,
                               const dos_view_policy_t *pol, dos_rect_t *out);
extern int dos_view_parse_rs(const uint8_t *buf, uint32_t len,
                             dos_view_policy_t *pol);
extern int dos_view_selftest_rs(void);
// (#dosfs) `aspect=crt`: correct the fixed 8:5 box to the 4:3 the original
// hardware displayed it at. Applied inside dos_present_aspect() below, which is
// the ONE source of the box for both the draw path and the input path, so the
// two cannot end up disagreeing about the shape of the picture.
extern int dos_aspect_apply_rs(int32_t aspect, int32_t *aw, int32_t *ah);

// THE DEFAULT LIVES IN RUST AND ONLY IN RUST (blame.md, the #mickey re-home
// interval: a Rust `pub const` mirrored by a C `#define` that the C side then
// assigned over the top, so changing the constant did nothing and cost a whole
// verification run). This C copy is initialised from the Rust constant by
// dos_view_init() and is only ever written where DOSVIEW.CFG actually asked.
extern int32_t dos_view_default_budget_rs(void);
// (no-ticket) The cap on the OPENING WINDOW, which is a different question from
// the budget that governs the PICTURE. See rustkern/doswin.rs.
extern int32_t dos_view_open_budget_rs(int32_t picture_budget);
static dos_view_policy_t g_dos_view = { 0, 1, 0, 0, 0, 1 };

// (no-ticket) THE DISPLAY-RATE BACK PRESSURE. The logic, and the measurement
// that motivated it, are in rustkern/dosdisp.rs; this is the seam and the one
// instance of the state.
typedef struct { uint64_t last_flips, last_present_ms, presented, skipped; } dosdisp_state_t;
_Static_assert(sizeof(dosdisp_state_t) == 32,
               "dosdisp_state_t must match Rust DosDispState");
extern void dosdisp_reset_rs(dosdisp_state_t *st);
extern int  dosdisp_should_present_rs(dosdisp_state_t *st, int32_t enabled,
                                      int32_t force, uint64_t flips, uint64_t now_ms);
extern int  dosdisp_selftest_rs(void);
static dosdisp_state_t g_dosdisp;
// gui/fb_syscall.c: the monotonic count of framebuffer presents, already read
// by [FLIPPROF] in main.c. Read here rather than counted again: a second
// counter of the same event is the fork the reuse rule forbids. Reached through
// win16_host_flip_count() (declared at the top of this file) so that the Ring-3
// host, which compiles this file unchanged, can answer it too - see there.

static int g_dos_view_loaded = 0;

// The screen minus the dock/taskbar insets, through the SAME narrow
// win16_host_* seam this file already uses for the window's content rect. Not a
// local copy of rect_t: a mirrored struct definition is the private fork of a
// shared type that the reuse rule exists to prevent, and it would agree with
// gui/window.h right up until the day rect_t gained a field. Defined in
// proc/syscall.c beside win16_host_content_rect().
extern int win16_host_work_area(int *ox, int *oy, int *ow, int *oh);

// (#dosfs) WHAT THE PRESENT ACTUALLY COSTS, in REAL microseconds.
//
// mono_us(), never timer_ticks: blame.md records that timer_ticks counts ticks
// DELIVERED and that KVM replays lost ticks in bursts, so an elapsed time
// derived from it under-counts exactly when the machine is busiest, which is
// exactly when this is being measured. There is no floating point anywhere near
// it either - the kernel is built -mno-sse and a %f in a kprintf silently
// prints 0.00 (blame.md, #740 VESA), so every ratio below is integer.
//
// Reported on the existing /CONFIG/DOSSPEED.CFG gate rather than a new one: a
// second perf flag would be a second thing to remember to arm, and this belongs
// beside the delivered-instruction-rate line it explains.
static uint64_t g_dosv_us_tot, g_dosv_us_max, g_dosv_n, g_dosv_bars;
static int32_t  g_dosv_pw, g_dosv_ph;      // the picture size last presented
static uint64_t g_dosv_report_ms;

// (#dosfs) LOAD THE VIEW POLICY. Idempotent and called from BOTH the window
// sizing (which needs it before the window exists) and the launch self-test
// block (which reports it), because a policy that is read after the window has
// already been created would size the window from the compiled-in default and
// then silently disagree with the file for the rest of the session.
static void dos_view_init(int *read_out, int *applied_out) {
    if (g_dos_view_loaded) {
        if (read_out) *read_out = -1;
        if (applied_out) *applied_out = -1;
        return;
    }
    g_dos_view_loaded = 1;
    // The present-cost counters are file statics, so unlike everything in
    // dos_task_t they survive the memset at launch. Reset them here so a second
    // guest in one session does not report the first one's average.
    g_dosv_us_tot = 0; g_dosv_us_max = 0; g_dosv_n = 0;
    g_dosv_bars = 0; g_dosv_report_ms = 0;
    g_dosv_pw = 0; g_dosv_ph = 0;
    g_dos_view.budget_px = dos_view_default_budget_rs();
    g_dos_view.integer   = 1;
    g_dos_view.max_w     = 0;
    g_dos_view.max_h     = 0;
    g_dos_view.aspect    = 0;   // square pixels: what has always shipped
    // /CONFIG is on the ext2 ROOT, not the FAT ESP; fat_read_file() routes it
    // there through fat_path_on_ext2(). This uses the same call as the three
    // DOS config files beside it rather than a private one, because blame.md
    // records autorun_worker() hardcoding a path the ESP has no CONFIG
    // directory for, leaving it unreachable on every two-partition golden.
    uint32_t vz = 0;
    void *vc = fat_read_file(&g_fat_fs, "/CONFIG/DOSVIEW.CFG", &vz);
    int applied = 0;
    if (vc) {
        applied = dos_view_parse_rs((const uint8_t *)vc, vz, &g_dos_view);
        kfree(vc);
    }
    if (read_out) *read_out = vc ? 1 : 0;
    if (applied_out) *applied_out = applied;
}


// #163: displayed rows from the CRTC's vertical timing. See rustkern/doswin.rs.
extern uint32_t dos_vga_rows_rs(const uint8_t *crtc, uint32_t ncrtc);
extern int dos_vga_rows_selftest_rs(void);

// (#mickey) WHAT A DOS GUEST IS TOLD THE MOUSE DID. See rustkern/dosmick.rs for
// the whole argument; the short version is that a DOS mouse driver is RELATIVE
// and reports mickeys, our pointer is ABSOLUTE, and re-deriving a mickey stream
// from an absolute position by multiplying by the guest's own mickeys-per-pixel
// ratio doubles vertical motion for any guest that integrates the counters
// instead of dividing by that ratio. The arithmetic and the homing state
// machine live in Rust; the DELIVERY of an upcall stays in C because it drives
// the two interpreter cores.
typedef struct {
    int32_t  rep_x, rep_y;      // the pair most recently handed over in SI/DI
    int32_t  rel_x, rel_y;      // the 0Bh accumulator, a SEPARATE number
    int32_t  prev_x, prev_y;    // last absolute position folded in
    int32_t  have_prev;
    int32_t  home_ph;           // 0 = synced; 1/2/3 = the homing phase due
    uint32_t home_n;            // completed homings
    uint32_t since_home;        // move events delivered since the last one
    uint32_t home_every;        // re-home interval, in move events; 0 = off
    int32_t  gain_ratio;        // 1 = scale the counters by the guest's ratio
} dos_mick_t;
_Static_assert(sizeof(dos_mick_t) == 48, "dos_mick_t must match Rust DosMick");
// NO SECOND COPY OF THE DEFAULT. A #define here that mirrored
// DOS_MICK_HOME_EVERY in rustkern/dosmick.rs was measured doing exactly what a
// duplicated constant always does: the Rust value was changed from 240 to 8,
// the C copy still said 120, and because dos_mouse_defaults() assigns the C
// value over the one dos_mick_reset_rs() just set, the C copy won. Run a2
// therefore re-homed ZERO times in eleven move events and looked like a broken
// state machine rather than a stale number. The default now lives in Rust
// alone; the C side overrides it only when DOSMOUSE.CFG actually said so.
#define DOS_MICK_HOME_UNSET 0xFFFFFFFFu
extern void dos_mick_reset_rs(dos_mick_t *st);
extern void dos_mick_arm_home_rs(dos_mick_t *st);
extern int  dos_mick_move_rs(dos_mick_t *st, int32_t x, int32_t y,
                             int32_t min_x, int32_t max_x,
                             int32_t min_y, int32_t max_y,
                             int32_t ratio_x, int32_t ratio_y);
extern void dos_mick_take_rel_rs(dos_mick_t *st, int32_t *ox, int32_t *oy);
extern int  dos_mick_next_rs(dos_mick_t *st, int32_t x, int32_t y,
                             int32_t min_x, int32_t max_x,
                             int32_t min_y, int32_t max_y,
                             int32_t *osi, int32_t *odi);
extern void dos_mick_phase_done_rs(dos_mick_t *st, int32_t x, int32_t y,
                                   int32_t min_x, int32_t max_x,
                                   int32_t min_y, int32_t max_y);
extern int  dos_mick_tick_rs(dos_mick_t *st);
extern int  dos_mick_selftest_rs(void);

// (#740) MODE X: unchained 256-colour VGA.
//
// WHAT IS IN RUST AND WHAT IS NOT. The CRTC/ATC/Sequencer registers Mode X
// shares with every other mode - start address, Offset/stride, Line Compare,
// pixel pan, Map Mask, Chain-4, Read Map Select - are decoded ONCE by
// dos_vga_decode_geom() below, and Mode X reads that like every other
// presenter. Rust owns only the part no other mode has: WHAT RESOLUTION IS THE
// GUEST IN. Every other mode answers that from its INT 10h mode number; Mode X
// has no mode number (AH=0Fh answers 0x13 forever), so the answer has to be
// derived from the CRTC's own timing registers. See rustkern/modex.rs.
typedef struct {
    int32_t unchained;
    int32_t w, h;
    int32_t aspect_w, aspect_h;
} dos_modex_geom_t;
_Static_assert(sizeof(dos_modex_geom_t) == 20,
               "dos_modex_geom_t must match Rust ModeXGeom");
extern int dos_modex_geom_rs(const uint8_t *crtc, uint8_t seq4,
                             dos_modex_geom_t *out);
extern int dos_modex_selftest_rs(void);
// (#740) The CRTC state an INT 10h mode set leaves behind, and the ONE
// definition of the CRTC-0x13 "unprogrammed" sentinel that decides stride.
// See rustkern/doscrtc.rs for why a mode set must write the sentinel registers
// EXPLICITLY rather than inherit them from the previous mode.
extern int dos_crtc_seed_mode13_rs(uint8_t *crtc, uint32_t n);
extern uint32_t dos_crtc_stride_rs(uint8_t crtc_13, uint32_t default_stride_bytes);
extern int dos_crtc_selftest_rs(void);

// (#745) XMS 3.0 (extended memory) and LIM EMS 4.0 (expanded memory).
//
// The managers themselves are rustkern/dosmem.rs; everything here is the
// PLUMBING that makes a DOS program able to FIND them, which is the half that
// is easy to get wrong because each manager is discovered by a different
// mechanism and neither is "call the obvious interrupt":
//
//   XMS  INT 2Fh AX=4300h must answer AL=80h, then AX=4310h hands back a far
//        ENTRY POINT in ES:BX that the program CALLs. There is no XMS
//        interrupt, so the entry point is a segment the interpreter's far-call
//        trap is armed on (dos_xms_farcall below).
//   EMS  the program OPENS "EMMXXXX0" AS A FILE and IOCTLs the handle to
//        confirm it is a character device (dos/int21svc.c, inside the one #736
//        service core), or reads the INT 67h vector and checks for the name
//        "EMMXXXX0" at offset 0Ah of the segment it points at. Only then does
//        it issue INT 67h. Answering INT 67h alone makes the manager INVISIBLE:
//        measured, an EMS stub that did only that recorded zero INT 67h calls.
//
// The second EMS path is the same shape of bug as #163's mouse detection, where
// every unhooked vector pointed at an IRET stub, so the documented "is a driver
// installed" test answered no forever and int33() was never reached. Hence the
// real device header planted at DOS_EMM_SEG below.
//
// TURNING XMS ON IS A BEHAVIOUR CHANGE FOR EVERY DOS GUEST. dosexec.c answered
// "not installed" deliberately, and the old comment said why: Commander Keen
// then falls back to conventional memory and works. That was correct while
// there was nothing to offer. It is not a free change now, which is why the
// arenas are allocated UP FRONT and the answer to AX=4300h is 80h only if the
// memory backing the promise actually exists.
typedef struct {
    uint16_t ax, bx, cx, dx, si, di, ds, es, flags, pad;
} dos_regs_t;
_Static_assert(sizeof(dos_regs_t) == 20, "dos_regs_t must match Rust DosRegs");
// (#172) rustkern/dosmcb.rs. dos_mcb_largest_free_rs takes the MCB table
// BY POINTER, so the two definitions of a record must not drift: the
// _Static_asserts next to dos_mcb_t below lock size and every offset.
extern uint16_t dos_mcb_largest_free_rs(const void *mcb, int n,
                                        uint16_t floor, uint16_t ceiling);
extern int      dos_psp_create_rs(uint8_t *mem, uint16_t cur_psp,
                                  uint16_t new_psp, uint16_t block_end_para);
extern int      dos_mcb_selftest_rs(void);
// (#172) rustkern/dospit.rs. The PIT channel register protocol, one copy,
// used by channels 0 and 2. dos_pit_ch_t below is layout-locked to DosPitCh.
extern void     dos_pit_ctrl_rs(void *ch, uint8_t rw, uint16_t live);
extern uint8_t  dos_pit_read_rs(void *ch, uint16_t live);
extern void     dos_pit_write_rs(void *ch, uint8_t b);
extern int      dos_pit_selftest_rs(void);
// (#175) rustkern/opl2.rs. dos_opl2_t is declared further down (it has to sit
// beside dos_task_t, which embeds it); `void *` here for the same reason the
// dospit block above uses it. The layout contract is enforced by the
// _Static_asserts on dos_opl2_t, not by these prototypes.
extern uint8_t  dos_opl2_status_rs(void *o, uint64_t now);
// (#176) rustkern/dosbus.rs: the emulated cost of a guest port access. Layout
// locked below, because a drift here would not crash: it would corrupt the
// emulated clock, which is the exact failure the module exists to fix.
typedef struct {
    uint32_t ns_per_access;
    uint32_t armed;
    uint64_t q32_per_access;
    uint64_t frac_q32;
    uint64_t n_access;
    uint64_t ticks_charged;
} dos_bus_t;
extern void     dos_bus_init_rs(dos_bus_t *b, uint32_t ns);
extern uint64_t dos_bus_charge_rs(dos_bus_t *b);
// (#252) Charge an explicit interval of emulated time, in microseconds,
// through the SAME accumulator. See rustkern/dosbus.rs for why a BIOS wait
// must not get a counter of its own.
extern uint64_t dos_bus_charge_us_rs(dos_bus_t *b, uint32_t us);
// (#252) INT 15h decode. 1 = serviced, and *wait_us is what to charge.
extern uint32_t dos_int15_rs(uint16_t ax, uint16_t cx, uint16_t dx, uint32_t *wait_us);
extern uint32_t dos_int15_selftest_rs(void);
extern uint32_t dos_bus_selftest_rs(void);
_Static_assert(sizeof(dos_bus_t) == 40, "dos_bus_t must match rustkern DosBus");
_Static_assert(__builtin_offsetof(dos_bus_t, ns_per_access)  == 0,  "DosBus.ns_per_access");
_Static_assert(__builtin_offsetof(dos_bus_t, armed)          == 4,  "DosBus.armed");
_Static_assert(__builtin_offsetof(dos_bus_t, q32_per_access) == 8,  "DosBus.q32_per_access");
_Static_assert(__builtin_offsetof(dos_bus_t, frac_q32)       == 16, "DosBus.frac_q32");
_Static_assert(__builtin_offsetof(dos_bus_t, n_access)       == 24, "DosBus.n_access");
_Static_assert(__builtin_offsetof(dos_bus_t, ticks_charged)  == 32, "DosBus.ticks_charged");
// The shipped cost, in nanoseconds per 8-bit port access. Kept in step with
// rustkern/dosbus.rs::DOS_BUS_NS_DEFAULT by the boot self-test, which asserts
// the two agree rather than trusting a comment to keep them together.
#define DOS_BUS_NS_DEFAULT 1000u
// (#181) rustkern/dossb.rs: the Sound Blaster DSP command protocol and the
// 8237A channel state. Layout locked below on the structs themselves, for the
// same reason dos_bus_t is: a drift here would not crash, it would play the
// wrong bytes of guest memory at the wrong rate, which is a bug that sounds
// like a bug in the sink.
typedef struct {
    uint16_t base_addr;
    uint16_t base_count;
    uint16_t cur_addr;
    uint16_t cur_count;
    uint8_t  page;
    uint8_t  mode;
    uint8_t  masked;
    uint8_t  tc;
} dos_dma_ch_t;
typedef struct {
    dos_dma_ch_t ch[4];
    uint8_t  ff;
    uint8_t  _pad[3];
    uint32_t n_prog;
    uint32_t n_count_reads;
} dos_dma_t;
_Static_assert(sizeof(dos_dma_ch_t) == 12, "dos_dma_ch_t must match rustkern/dossb.rs DosDmaCh");
_Static_assert(sizeof(dos_dma_t) == 60, "dos_dma_t must match rustkern/dossb.rs DosDma");
_Static_assert(__builtin_offsetof(dos_dma_t, ff) == 48, "dos_dma_t.ff");
_Static_assert(__builtin_offsetof(dos_dma_t, n_prog) == 52, "dos_dma_t.n_prog");

typedef struct {
    uint8_t  installed;
    uint8_t  base_hi;
    uint8_t  irq;
    uint8_t  dma;
    uint8_t  dsp_major;
    uint8_t  dsp_minor;
    uint8_t  speaker;
    uint8_t  in_reset;
    uint8_t  cmd;
    uint8_t  argn;
    uint8_t  argv[2];
    uint8_t  out[4];
    uint8_t  out_head;
    uint8_t  out_tail;
    uint8_t  active;
    uint8_t  autoinit;
    uint8_t  high_speed;
    uint8_t  gen;
    uint8_t  time_const;
    uint32_t rate;
    uint16_t block_len;
    uint16_t block_len_48;
    uint8_t  irq_pending;
    uint8_t  _pad[3];
    uint32_t irq_raised;
    uint32_t irq_acked;
    uint32_t cmd_hist[256];
    uint32_t cmd_unknown;
    uint32_t resets;
} dos_sb_t;
_Static_assert(sizeof(dos_sb_t) == 1076, "dos_sb_t must match rustkern/dossb.rs DosSb");
_Static_assert(__builtin_offsetof(dos_sb_t, installed) == 0,  "dos_sb_t.installed");
_Static_assert(__builtin_offsetof(dos_sb_t, cmd)       == 8,  "dos_sb_t.cmd");
_Static_assert(__builtin_offsetof(dos_sb_t, out)       == 12, "dos_sb_t.out");
_Static_assert(__builtin_offsetof(dos_sb_t, active)    == 18, "dos_sb_t.active");
_Static_assert(__builtin_offsetof(dos_sb_t, rate)      == 24, "dos_sb_t.rate");
_Static_assert(__builtin_offsetof(dos_sb_t, block_len) == 28, "dos_sb_t.block_len");
_Static_assert(__builtin_offsetof(dos_sb_t, irq_pending) == 32, "dos_sb_t.irq_pending");
_Static_assert(__builtin_offsetof(dos_sb_t, cmd_hist)  == 44, "dos_sb_t.cmd_hist");
_Static_assert(__builtin_offsetof(dos_sb_t, resets)    == 1072, "dos_sb_t.resets");

extern void     dos_sb_init_rs(dos_sb_t *s, uint8_t installed, uint8_t base_hi,
                               uint8_t irq, uint8_t dma, uint8_t major, uint8_t minor);
extern void     dos_sb_reset_rs(dos_sb_t *s, uint8_t val);
extern uint8_t  dos_sb_read_rs(dos_sb_t *s, uint16_t off);
extern uint8_t  dos_sb_write_rs(dos_sb_t *s, uint8_t val);
extern void     dos_sb_raise_irq_rs(dos_sb_t *s);
extern uint8_t  dos_sb_irq_pending_rs(const dos_sb_t *s);
extern void     dos_sb_block_done_rs(dos_sb_t *s);
extern void     dos_sb_u8_to_s16_rs(const uint8_t *src, int16_t *dst, uint32_t n);
extern int      dos_sb_selftest_rs(void);
extern void     dos_dma_init_rs(dos_dma_t *d);
extern void     dos_dma_out_rs(dos_dma_t *d, uint16_t port, uint8_t val);
extern uint8_t  dos_dma_in_rs(dos_dma_t *d, uint16_t port);
extern void     dos_dma_page_rs(dos_dma_t *d, uint8_t chan, uint8_t val);
extern uint32_t dos_dma_cur_phys_rs(const dos_dma_t *d, uint8_t chan);
extern uint32_t dos_dma_phys_at_rs(const dos_dma_t *d, uint8_t chan, uint32_t off);
extern uint8_t  dos_dma_playback_armed_rs(const dos_dma_t *d, uint8_t chan);
extern uint8_t  dos_dma_autoinit_rs(const dos_dma_t *d, uint8_t chan);
extern uint32_t dos_dma_block_bytes_rs(const dos_dma_t *d, uint8_t chan);
extern uint8_t  dos_dma_set_played_rs(dos_dma_t *d, uint8_t chan, uint32_t played);

// (#181) The card's identity. MEASURED from the corpus, not assumed: see the
// port-map block at the top of rustkern/dossb.rs. Aladdin's own SOUND.CFG
// records base 0x0220, IRQ 5, DMA 1, and its port traffic independently
// confirms all three (0x226 reset => base 0x220; page register 0x83 => DMA
// channel 1). The IRQ is the one thing SOUND.CFG alone attests, because a card
// with no IRQ activity leaves no trace of its jumper.
#define DOS_SB_BASE       0x220
#define DOS_SB_IRQ        5
#define DOS_SB_IRQ_VEC    0x0D      /* IRQ5 -> INT 0Dh (master PIC, 08h + IRQ) */
#define DOS_SB_DMA_CHAN   1
// Sound Blaster 2.0, DSP 2.01: the last DSP that is purely 8-bit mono, which
// is exactly the surface implemented. Claiming a 4.xx (SB16) would advertise
// 16-bit DMA and a mixer that are NOT implemented, and a guest that believed it
// would program a transfer we would decline. Version numbers are a contract.
#define DOS_SB_DSP_MAJOR  2
#define DOS_SB_DSP_MINOR  1
// The rate we hand the sink. Chosen because audio_open() CLAMPS a requested
// rate into the device's [min,max] and does NOT resample: HDA/AC97 report a
// minimum of 44100 and the USB DAC 44100..48000, so asking for the guest's
// 11 kHz would silently become 44100 and play everything at four times the
// pitch. 44100 is inside every sink's range, so the clamp is a no-op and the
// rate conversion happens where it can be done correctly.
#define DOS_SB_SINK_RATE  44100u
// Guest bytes moved per pump iteration. Small enough that stopping is prompt
// and the published DMA position is fine-grained (a guest polling the count
// register sees it move ~43 times a second at 11 kHz), large enough that the
// per-iteration overhead is negligible.
#define DOS_SB_CHUNK      256u
// Hard ceiling on how far the pump may run ahead of the DAC, in sink frames.
// 8192 at 44100 Hz is 186 ms. THIS IS NOT A LATENCY TUNING KNOB: an auto-init
// guest refills the half of the buffer we are not playing, so reading further
// ahead than this would read bytes the guest has not written yet.
#define DOS_SB_LEAD       8192u
// ===========================================================================
// (#182/#fmbridge) THE FM BRIDGE lives in dos/dosfmq.c now, and this file
// reaches it only through the dos/dosfmq.h seam included above.
//
// It used to be right here: dos_fm_queue_t, its offsetof locks, the
// rustkern/fmq.rs externs, `static dos_fm_queue_t g_dos_fmq` and its
// spinlock. That is correct in Ring 0 and unfixable in Ring 3, because THIS
// FILE IS ALSO COMPILED INTO /APPS/DOSUSER (userland/apps/dosring3), where a
// file-scope static is a SECOND queue in a SECOND address space. The guest's
// OPL2 writes filled it correctly and nothing ever drained it, because
// /APPS/FMSYNTH drains the KERNEL's queue through SYS_DOS_FM_EVENTS. Ring-3
// DOS guests therefore had no music, and fm_launch_synth() returned -1 so the
// chip would at least report ABSENT honestly rather than advertise a
// synthesiser with nothing behind it.
//
// A variable access cannot become a syscall (the #flipfix lesson), so it
// becomes a CALL. dos/dosfmq.h explains why the transport is a PUSH here
// where #flipfix chose a PULL, and dos/dosfmq.c is the one owner of the one
// queue.
// ===========================================================================


extern void     dos_opl2_addr_rs(void *o, uint8_t val);
extern void     dos_opl2_data_rs(void *o, uint8_t val, uint64_t now);
extern void     dos_opl2_init_rs(void *o, uint8_t installed);
extern uint32_t dos_opl2_writes_rs(const void *o);
extern int      dos_opl2_selftest_rs(void);
// (#176) declared beside its siblings; the definition is in the block above.
extern uint32_t dos_xms_state_size_rs(void);
extern int      dos_xms_init_rs(void *st, uint8_t *pool, uint32_t pool_kb);
extern int      dos_xms_dispatch_rs(void *st, dos_regs_t *r, uint8_t *mem);
extern void     dos_xms_report_rs(void *st);
extern uint32_t dos_ems_state_size_rs(void);
extern int      dos_ems_init_rs(void *st, uint8_t *pool, uint32_t pool_pages,
                                uint16_t frame_seg);
extern int      dos_ems_dispatch_rs(void *st, dos_regs_t *r, uint8_t *mem);
extern void     dos_ems_report_rs(void *st);
extern int      dos_mem_selftest_rs(void *xs, uint8_t *xp, void *es, uint8_t *ep,
                                    uint8_t *mem);

// The XMS far entry point. F200 is inside the emulated BIOS ROM region, which
// dos_load_image() already refuses to load a program over, and nothing else
// claims it. The byte planted there is a RETF: the far-call trap fires before
// any byte is fetched, so it is unreachable in practice, and a RETF is the
// benign landing if some path ever reaches it.
#define DOS_XMS_SEG      0xF200
#define DOS_XMS_OFF      0x0010
#define DOS_XMS_POOL_KB  4096            // 4 MiB of extended memory

// The EMM driver's device header and INT 67h landing pad. The name MUST sit at
// offset 000Ah of the segment the INT 67h vector points at: that offset is the
// device-header name field and is what the vector-based detection reads.
#define DOS_EMM_SEG      0xF100
#define DOS_EMM_ENTRY    0x0020          // CD 67 CB: INT 67h; RETF
#define DOS_EMS_FRAME    0xD000          // 64 KiB page frame, four 16 KiB windows
#define DOS_EMS_PAGES    128             // 2 MiB of expanded memory


// (#740) VESA BIOS Extensions for the DOS guest. EVERY decision - the mode
// table, the two spec-defined structure layouts, the guest-visible video-BIOS
// ROM blob, and the whole AH=4Fh dispatcher - lives in rustkern/vbe.rs. This
// file is glue: it resolves ES:DI to a bounds-checked pointer, owns the VRAM
// allocation, routes the 0xA0000 window through the current bank, and runs the
// present loop. Read that module's header for WHY this is banked at A000:0000
// and not a linear framebuffer (short version: the DOS guest goes through
// x86_16.c's real-mode lin(), which masks to 20 bits, so an LFB above the
// first megabyte is UNREACHABLE, not merely slow).
//
// Layouts are locked to the Rust side by the _Static_asserts below, per the
// established FFI pattern.
typedef struct {
    uint16_t mode;          // 0 = no VBE mode active, else the mode number
    uint16_t width, height;
    uint8_t  bpp, dac8;
    uint32_t bpl;           // bytes per scan line (4F06h can widen it)
    uint32_t bank;          // window A position, in 64 KB granularity units
    uint32_t disp_start;    // display start, BYTES into VRAM (4F07h)
    uint32_t vram;          // bytes of VRAM actually allocated
} vbe_state_t;
_Static_assert(sizeof(vbe_state_t) == 24, "vbe_state_t must match rustkern/vbe.rs VbeState");

typedef struct {
    uint16_t ax, bx, cx, dx;
    uint8_t *buf;           // ES:DI resolved and bounds-checked, or NULL
    uint32_t buflen;
    uint8_t *pal;           // t->pal, flat 256*3
    uint32_t action;        // out: what this file must do next
    uint32_t miss;          // out: subfunction to log as a MISS, 0 = none
} vbe_call_t;
_Static_assert(sizeof(vbe_call_t) == 40, "vbe_call_t must match rustkern/vbe.rs VbeCall");

#define VBE_ACT_NONE     0u
#define VBE_ACT_SET_MODE 1u
#define VBE_ACT_SET_VGA  2u

extern uint32_t vbe_build_rom_rs(uint8_t *buf, uint32_t len);
extern int      vbe_dispatch_rs(vbe_state_t *st, vbe_call_t *c);
extern int      vbe_selftest_rs(void);
// The single definition of how much video memory exists. 4F00h reports it and
// this file allocates it; taking both from one place is why a guest's own mode
// filtering cannot disagree with what was actually allocated.
extern uint32_t vbe_vram_bytes_rs(void);

typedef struct {
    uint32_t       *dst;    // host window ARGB content buffer
    const uint8_t  *src;    // guest VRAM
    const uint8_t  *pal;    // 256 * (r,g,b)
    int32_t         dst_stride, dst_w, dst_h;
    int32_t         x, y, w, h;   // letterboxed picture rect (dos_letterbox_rs)
    int32_t         gw, gh;       // guest resolution
    uint32_t        src_len, bpl, disp_start;
    uint8_t         dac8;
} vbe_present_t;
_Static_assert(sizeof(vbe_present_t) == 80, "vbe_present_t must match rustkern/vbe.rs VbePresent");

extern int vbe_present_rs(const vbe_present_t *p);

// (#212) CGA graphics present. Mirrors #[repr(C)] CgaPresent in
// rustkern/cga.rs. The _Static_asserts are the ONLY thing checking that the
// two declarations still agree: nothing in either compiler does, and the
// failure mode of a drifted offset here is a pixel loop reading from a wrong
// pointer, which is a corrupted picture rather than a build error.
typedef struct {
    uint32_t       *dst;          // host window content buffer (ARGB)
    const uint8_t  *src;          // guest memory at 0xB8000
    uint32_t        dst_stride;   // pixels per destination row (win_w)
    uint32_t        dst_x, dst_y, dst_w, dst_h;
    uint32_t        src_w, src_h; // 320x200 (04h/05h) or 640x200 (06h)
    uint32_t        src_len;      // readable bytes at src: the bounds check
    uint8_t         mode;         // 0x04, 0x05 or 0x06
    uint8_t         pal_reg;      // the live 0x3D9 Color Select Register
    uint8_t         pad[2];
} cga_present_t;
_Static_assert(sizeof(cga_present_t) == 56,
               "cga_present_t must match rustkern/cga.rs CgaPresent");
_Static_assert(__builtin_offsetof(cga_present_t, src) == 8, "cga src offset");
_Static_assert(__builtin_offsetof(cga_present_t, dst_stride) == 16, "cga stride offset");
_Static_assert(__builtin_offsetof(cga_present_t, src_len) == 44, "cga src_len offset");
_Static_assert(__builtin_offsetof(cga_present_t, mode) == 48, "cga mode offset");
extern int cga_present_rs(const cga_present_t *p);
extern int cga_selftest_rs(uint8_t *scratch, uint32_t scratch_len, uint32_t *checks);

// The CGA graphics aperture. 16 KB at B800:0000, two 8 KB banks holding the
// EVEN and ODD scanlines; see rustkern/cga.rs for why that matters.
#define CGA_B800       0xB8000
#define CGA_APERTURE   0x4000
// What a real BIOS leaves in the Color Select Register after a mode 04h set:
// palette 1 + intensity, i.e. the black/cyan/magenta/white everyone pictures.
#define CGA_PAL_RESET  0x30

// The one logical screen every video mode is scaled into, and the size a DOS
// host window is created at. Keeping these the same is what makes the default
// window's output identical to what shipped before this change.
#define DOS_SURF_W 640
#define DOS_SURF_H 400
// The letterbox margin. Black, because that is what a DOS screen's border was.
#define DOS_BAR_ARGB 0xFF000000u
// How many superseded content buffers we can be holding at once. One per
// resize that lands while a present is in flight; a present is at most a few
// milliseconds and they arrive at mouse-move rate, so four is slack, not a
// budget. Overflow LEAKS one buffer and says so, because the alternative -
// letting the window manager free a buffer this thread is mid-blit into - is
// the exact use-after-free this whole change exists to remove.
#define DOS_PEND_FREE_MAX 4

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

// ---- guest CPU SPEED CAP (#232) ------------------------------------------
// EVERYTHING ABOVE THIS BLOCK IS ABOUT HOST PACING - how the interpreter's
// work is chopped up so input and presents stay responsive - and it is
// deliberately invisible to the guest. THIS block is the opposite: it is about
// how fast the guest's own CPU appears to BE, which the guest can absolutely
// observe, and which for a large class of DOS titles is the only thing that
// decides whether the game is playable.
//
// WHY IT IS NEEDED AT ALL. The emulated PIT already tracks real time exactly
// (dos_emu_pit_now), so a game that paces itself off the timer, off INT 1Ch, or
// off the CGA retrace runs at the right speed with no cap and always did:
// Commander Keen's Galaxy engine busy-waits on its own INT 8 TimeCount, so it
// is self-limiting. But a 1983-era title written for ONE machine often has no
// clock in its game loop at all. Joust (Atarisoft PC, /DOS/JOUST/JOUST.COM) is
// the pure case, PROVEN by disassembly, not assumed: it DOES hook INT 8 and
// phase-lock it to vertical retrace at ~59.9 Hz, but that tick only drives the
// attract-mode timeout counter at guest [0x1390]; the gameplay loop at guest
// linear 0x977A..0x97A8 is thirteen calls and a `jmp short` back to the top,
// with no hlt, no retrace poll, no frame compare and no delay call anywhere in
// it. Its frame rate IS the host's instruction rate. Its PC-speaker tone loops
// (guest 0xC8FE, 0xC953) are bit-banged `dec/jnz` half-periods, so its PITCH is
// host-speed-dependent too.
//
// UNIT: emulated instructions per emulated MILLISECOND. That is deliberately
// DOSBox's "cycles" unit and scale, because it is the number the world already
// publishes per title AND the number our own game packs already ship:
// /DOS/JOUST/START.bat contains, verbatim, `config -set "cpu cycles=500"`.
//
// WHAT THE ORIGINAL HARDWARE DID, so the next person does not have to guess:
//   IBM PC / PC-XT   8088  @ 4.77 MHz  ~   315 cycles  (~0.31 MIPS)  <- Joust's era
//   IBM PC-AT        80286 @ 6   MHz   ~   900 cycles
//   386DX                  @ 33  MHz   ~  6000 cycles
//   486DX2                 @ 66  MHz   ~ 20000 cycles
// MEASURED on this project's own hardware (Xeon Gold 6248 host, VM <vmid>, golden
// build 2053, Joust title loop, 4.38e9 insns over 192 s): an UNCAPPED MayteraOS
// DOS guest delivers about 22,800 cycles, i.e. it runs a PC-XT title roughly
// 45x too fast. That is the whole bug.
#define DOS_CYCLES_OFF          0u        // no cap at all: run as fast as the host can
#define DOS_CYCLES_MIN          20u       // sanity floor for a configured value
#define DOS_CYCLES_MAX          200000u   // sanity ceiling (200 M insn/s)
// The longest single uninterrupted burst, expressed in GUEST milliseconds. Also
// the catch-up headroom: the host tick is 250 Hz (4 ms), so a sleep asked for in
// ms can land a tick late, and the burst after it must be allowed to be several
// ms long or the cap would under-deliver instead of hitting its target.
//
// (#speedcap) 60, NOT 12, AND THE OLD VALUE MADE THE CAP DELIVER A THIRD OF WHAT
// IT PROMISED. The paragraph above states the correct requirement and 12 did not
// meet it. MEASURED on golden 2300, Commander Keen 5, /CONFIG/DOSSPEED.CFG armed,
// SPEED.CFG=3000:
//
//   [dos] #232 speed: 1006932 insn/s (1006 cycles) target=3000 cycles credit=36000
//   [DOSFRAME] wall=2027ms | interp 3.6% (n=98) | present 1.2% | resid 94.8%
//
// Two numbers give the whole diagnosis. `credit=36000` is EXACTLY the old
// 3000 * 12 ceiling, i.e. the account was PINNED at the clamp on every pass, so
// entitlement the guest had genuinely earned was being discarded rather than
// spent. And `resid 94.8%` with n=98 passes over 2027 ms says a pass takes about
// 20 ms of wall clock, essentially all of it inside the throttle's own
// proc_sleep(): a sleep ASKED for in single milliseconds returns about 20 ms
// later, because the host tick is 4 ms and the ready queue now has a PRIO_HIGH
// compositor in it. The account is designed to absorb exactly that (an over-long
// sleep earns a proportionally longer next burst), and a 12 guest-ms ceiling
// silently defeated the compensation for any sleep longer than 12 ms, which was
// every single one of them.
//
// So the delivered rate was not `cycles`, it was `cycles * BURST_MS /
// pass_wall_ms`, about a third. This was #232's behaviour as shipped, on BOTH
// run loops, and it means the two values in the tree before this change were
// also wrong in practice: SimCity asked for 6000 and got about 2000.
//
// WHY 60 AND NOT "BIG". It has to exceed the worst legitimate sleep, and
// DOS_THROTTLE_SLEEP_MAX is 25 ms, so 60 is that plus better than 2x headroom for
// scheduler overshoot. It is still a real bound: the reason a bound exists at all
// is that an idle stretch must not bank unlimited credit and spend it in one
// burst, and 60 guest-ms is a small fraction of a second.
//
// AND THE WALL-CLOCK COST IS NOT 60 ms. This ceiling is in GUEST time; the wall
// time a burst costs is guest_ms * cap / host_rate. On this hardware the
// interpreter runs at about 18,000 cycles, so a 60 guest-ms burst at a 3000-cycle
// cap is 180,000 instructions and about 10 ms of wall clock, which is under one
// present interval. A cap high enough for 60 guest-ms to be a long WALL time is
// by definition a cap at or above what the interpreter can deliver, and such a
// cap never binds, so it never issues a long burst either.
#define DOS_THROTTLE_BURST_MS   60u
#define DOS_THROTTLE_BURST_MIN  64u       // never issue a zero-length burst
#define DOS_THROTTLE_SLEEP_MAX  25u       // longest single sleep, ms
// The furthest into DEBT the account may go, in guest milliseconds. A guest
// whose synthesized-ISR cost alone exceeds the cap would otherwise run up
// unbounded debt and then park for a very long time paying it off. Clamping
// means such a guest simply runs faster than its cap and says so in the
// /CONFIG/DOSSPEED.CFG line, which is a diagnosable outcome rather than a hang.
#define DOS_THROTTLE_DEBT_MS    200u
// How many instructions dos_deliver_int() runs between checks for "the handler
// has IRETed". See the long note there; this is a granularity, not a budget.
#define DOS_IRQ_RETURN_CHUNK    128UL
// Diagnostic only (/CONFIG/DOSSPEED.CFG), off in the golden.
#define DOS_SPEED_REPORT_MS     2000u
// #778 LIVE SPEED CONTROL: how often the run loop re-reads the SAME
// dos_speed_cycles_for() chain it read at launch, so a per-window Speed
// control (compositor) can change a running guest without a relaunch. This is
// a periodic re-check inside a loop that is already iterating every
// DOS_SLICE_MS (never a new wait/sleep of its own - see the call site in the
// per-slice bookkeeping block, which already runs unconditionally), so it
// adds no #426 busy-wait or blocking. 500 ms is fast enough that a slider
// drag feels live and slow enough that the FAT/ext2 read it costs is noise
// next to the guest's own I/O.
#define DOS_SPEED_LIVE_POLL_MS  500u

#define DOS_MEM_SIZE   0x100000          // 1 MiB real-mode address space
// (#211) How many of the guest's most recent service calls the ring keeps.
// 64 covers djgpp's whole startup sequence twice over and costs 1 KiB on the
// task, which is heap, not the DOS task's 64 KiB kernel stack (#212).
#define GO32_TRACE_RING 64u

#define DOS_PSP_SEG    0x0100            // PSP paragraph (so image loads at 0x0110)
#define DOS_LOAD_SEG   (DOS_PSP_SEG + 0x10) // program load segment (PSP is 0x100 bytes)
// (#digrun) The 16-bit guest's ENVIRONMENT BLOCK, at a paragraph below the PSP.
// Segments 0x0050-0x00FF are the region a real DOS keeps its own data in and
// nothing in this file writes there: the only sub-PSP writer is the BIOS data
// area at 0x0040. 0x0C00-0x0FFF leaves 1 KiB, and dos32_build_env() writes
// about 120 bytes.
#define DOS_ENV_SEG    0x00C0
#define DOS_ENV_LIN    ((uint32_t)DOS_ENV_SEG << 4)
#define VGA_A000       0xA0000           // mode-13h linear framebuffer base (linear)
#define VGA_A000_END   0xB0000           // end of the 64KB EGA aperture
#define MODE13_W       320
#define MODE13_H       200
#define WIN_SCALE      2                 // 320x200 -> 640x400 on screen

// EGA mode 0Dh (320x200x16, 4 planar bitplanes). Used by Commander Keen and
// other id Galaxy-engine games. Each plane is 64KB; a CPU byte at 0xA0000+off
// maps to bit (7-(x&7)) across the 4 planes for pixel x = off*8 + (7-bit).
#define EGA_PLANE_SIZE 0x10000           // 64KB per plane

// (#740) The emulated video-BIOS ROM page. 4F00h hands the guest FAR POINTERS
// to an OEM string and a mode list, and WinFuncPtr is a far pointer to a real
// bank-switch routine, so all three must point at memory the guest can read.
// C000:0000 is where a real video BIOS lives and dos_load_image() already
// refuses to load a program past 0xA0000, so nothing else is there.
#define VBE_ROM_LIN    0xC0000
#define VBE_ROM_SIZE   0x100
#define VBE_WIN_BYTES  (64u * 1024u)     // window granularity == window size
// #163: reserved BIOS-ROM offsets in segment F000 for the stubs the IVT points
// at. FF53 is the classic dummy-IRET address (#385). The other two exist
// because "the first byte is CFh" is a DOCUMENTED "no driver installed" test,
// so a vector that must look INSTALLED cannot point at an IRET.
#define DOS_IRET_STUB   0xFF53           // CF            (IRET)
#define DOS_INT33_STUB  0xFF54           // CD 33 CB      (INT 33h; RETF)
// The SAME IRET stub, as the FLAT address a 32-bit guest sees. Real-mode
// F000:FF53 is guest linear 0xFFF53, and 0xCF is IRET at 16-bit operand size
// and IRETD at 32-bit, so one byte serves both guests. Derived from the two
// constants above rather than written out, so it cannot drift from them.
#define DOS4GW_PM_IRET_LIN  (((uint32_t)0xF000u << 4) + DOS_IRET_STUB)
#define DOS_MEVRET_STUB 0xFF58           // EB FE         (JMP $)
// (#dpmi301) The far-return landing pad for a DPMI 0300h that EXECUTES a
// real-mode vector the guest published itself. Its own pad rather than a reuse
// of DOS_MEVRET_STUB: a mouse upcall and a 0300h are both "run 16-bit code from
// a slice boundary and detect the return", and sharing one pad would leave them
// unable to nest without silently stealing each other's return address. Two
// bytes, and there is room before DOS_BIOSTIMER_STUB at 0xFF60.
#define DOS_RMCALLRET_STUB 0xFF5C        // EB FE         (JMP $)
// (#740) The BIOS TIMER handler, and it must NOT be the bare IRET stub.
//
// A real BIOS INT 8 ends by invoking INT 1Ch, the documented user tick. Games
// that take over IRQ0 SAVE the old INT 8 vector and CHAIN to it at the original
// 18.2 Hz, which is how INT 1Ch keeps firing on hardware even when the game
// owns the timer. Pointing the seeded INT 8 at a plain IRET broke that chain
// silently, and it also made the saved vector INDISTINGUISHABLE from "nothing
// installed" - see dos_vec_hooked() and the INT 88h note in dos_int_handler().
//
// The BIOS tick counter at 0040:006C is NOT touched here: the run loop already
// maintains it from emulated PIT time, independently of the guest.
#define DOS_BIOSTIMER_STUB 0xFF60        // CD 1C CF      (INT 1Ch; IRET)

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
// (#172) Layout lock for rustkern/dosmcb.rs::DosMcb, which reads this array
// through a raw pointer. A silent drift here would not crash: it would make
// the free-space report wrong, which is indistinguishable from the bug #172
// exists to fix.
_Static_assert(sizeof(dos_mcb_t) == 8, "dos_mcb_t must match rustkern DosMcb");
_Static_assert(__builtin_offsetof(dos_mcb_t, seg)  == 0, "dos_mcb_t.seg");
_Static_assert(__builtin_offsetof(dos_mcb_t, para) == 2, "dos_mcb_t.para");
_Static_assert(__builtin_offsetof(dos_mcb_t, live) == 4, "dos_mcb_t.live");

// (#172) Mirrors DosPitCh in rustkern/dospit.rs. Locked below, because a
// silent drift here would not crash: it would corrupt a timer read, and a
// wrong PIT count is the exact shape of failure this ticket exists to fix.
typedef struct {
    uint16_t divisor;    // 0 means 65536
    uint16_t latch;      // captured by a counter-latch command
    uint8_t  latched;
    uint8_t  rd_hi;
    uint8_t  wr_hi;
    uint8_t  access;     // RW field: 1 lobyte, 2 hibyte, 3 both. NEVER 0.
    uint8_t  gate;       // channel 2 only: port 0x61 bit 0
    uint8_t  _pad[3];
} dos_pit_ch_t;
_Static_assert(sizeof(dos_pit_ch_t) == 12, "dos_pit_ch_t must match rustkern DosPitCh");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, divisor) == 0, "dos_pit_ch_t.divisor");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, latch)   == 2, "dos_pit_ch_t.latch");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, latched) == 4, "dos_pit_ch_t.latched");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, rd_hi)   == 5, "dos_pit_ch_t.rd_hi");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, wr_hi)   == 6, "dos_pit_ch_t.wr_hi");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, access)  == 7, "dos_pit_ch_t.access");
_Static_assert(__builtin_offsetof(dos_pit_ch_t, gate)    == 8, "dos_pit_ch_t.gate");

// (#175) Mirrors DosOpl2 in rustkern/opl2.rs. Locked below. A silent drift
// here would not crash: it would corrupt a detection answer, and a detection
// that lies is the entire subject of this ticket.
typedef struct {
    uint8_t  addr;          // register index latched by a write to 0x388
    uint8_t  installed;     // 0 = empty socket. See opl2_installed_policy().
    uint8_t  t1_preset;     // reg 0x02
    uint8_t  t2_preset;     // reg 0x03
    uint8_t  t1_run;
    uint8_t  t2_run;
    uint8_t  t1_mask;
    uint8_t  t2_mask;
    uint8_t  flags;         // latched overflow flags, in status-bit positions
    uint8_t  _pad[7];
    uint64_t t1_deadline;   // on the guest's emulated PIT clock
    uint64_t t2_deadline;
    uint32_t n_reg_writes;
    uint32_t n_status_reads;
    uint8_t  regs[256];     // accepted and remembered; never sounded
} dos_opl2_t;
_Static_assert(sizeof(dos_opl2_t) == 296, "dos_opl2_t must match rustkern/opl2.rs DosOpl2");
_Static_assert(__builtin_offsetof(dos_opl2_t, addr)        == 0,  "dos_opl2_t.addr");
_Static_assert(__builtin_offsetof(dos_opl2_t, installed)   == 1,  "dos_opl2_t.installed");
_Static_assert(__builtin_offsetof(dos_opl2_t, t1_preset)   == 2,  "dos_opl2_t.t1_preset");
_Static_assert(__builtin_offsetof(dos_opl2_t, t2_preset)   == 3,  "dos_opl2_t.t2_preset");
_Static_assert(__builtin_offsetof(dos_opl2_t, t1_run)      == 4,  "dos_opl2_t.t1_run");
_Static_assert(__builtin_offsetof(dos_opl2_t, t2_run)      == 5,  "dos_opl2_t.t2_run");
_Static_assert(__builtin_offsetof(dos_opl2_t, t1_mask)     == 6,  "dos_opl2_t.t1_mask");
_Static_assert(__builtin_offsetof(dos_opl2_t, t2_mask)     == 7,  "dos_opl2_t.t2_mask");
_Static_assert(__builtin_offsetof(dos_opl2_t, flags)       == 8,  "dos_opl2_t.flags");
_Static_assert(__builtin_offsetof(dos_opl2_t, t1_deadline) == 16, "dos_opl2_t.t1_deadline");
_Static_assert(__builtin_offsetof(dos_opl2_t, t2_deadline) == 24, "dos_opl2_t.t2_deadline");
_Static_assert(__builtin_offsetof(dos_opl2_t, n_reg_writes) == 32, "dos_opl2_t.n_reg_writes");
_Static_assert(__builtin_offsetof(dos_opl2_t, n_status_reads) == 36, "dos_opl2_t.n_status_reads");
_Static_assert(__builtin_offsetof(dos_opl2_t, regs)        == 40, "dos_opl2_t.regs");

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

    // (#745) XMS / EMS. Both arenas are kmalloc'd once at task start and freed
    // at task end. They are NOT lazy: a manager is advertised as installed only
    // when the memory backing that promise already exists, so a guest can never
    // be told "XMS is here" and then refused every allocation, which is the
    // failure mode most likely to send a program down a path it cannot recover
    // from. A NULL state means "not installed" everywhere it is tested.
    void        *xms_state;
    uint8_t     *xms_pool;
    void        *ems_state;
    uint8_t     *ems_pool;

    // PIT channel 0 (ports 0x40/0x43). Driven from cpu.insn_count.
    // (#172) THREE channels, not one set of loose fields. This used to be five
    // scalars named pit_*, which by construction described exactly one channel;
    // dos_out's control-word handler said `if (ch != 0) break;` and dos_in had
    // no case for port 0x42, so channel 2 read as 0xFF and any channel-2 delay
    // loop ran forever (Stunts, measured). The register protocol itself lives
    // once, in rustkern/dospit.rs; this is just its state, per channel.
    // Layout-locked to DosPitCh by the _Static_asserts below.
    dos_pit_ch_t pit[3];
    dos_opl2_t   opl2;                    // (#175) AdLib / OPL2 at 0x388
    uint8_t      opl2_reported;           // (#175) the silence line is once per guest
    uint8_t      port61;                  // last write to the PPI/speaker port
    uint8_t      port61_toggle;           // refresh bit 0x10, flipped per read

    // VGA / mode 13h
    int          video_mode;             // current INT 10h mode (0x13 = mode 13, 0x0D = EGA)
    int          gfx_w, gfx_h;           // active graphics resolution
    // (#212) The CGA Color Select Register (port 0x3D9). It is the WHOLE of
    // colour in modes 04h/05h/06h: there is no DAC to program, so this one
    // byte is the entire palette state and it is kept here rather than
    // recomputed, because INT 10h AH=0Bh and a raw OUT to 0x3D9 are two ways
    // of writing the SAME register and must not end up with two answers.
    uint8_t      cga_pal;
    // (#740) VESA. `vbe.mode != 0` is the ONE test every VBE path in this file
    // makes, so every pre-existing VGA path is bit-for-bit untouched until a
    // guest actually sets a VBE mode.
    vbe_state_t  vbe;
    uint8_t     *vbe_vram;               // kmalloc'd, vbe.vram bytes; NULL when not in a VBE mode
    uint32_t     vbe_missed;             // bitmap of subfunctions already logged, so a MISS logs ONCE
    // Text mode 03h state. The page itself lives in guest RAM at B800:0000 as
    // 80x25 (char, attribute) pairs, exactly as on real hardware, so a program
    // that pokes B800 directly and a program that goes through the BIOS/DOS both
    // land in the same place and dos_present_text() has one thing to draw.
    uint8_t      cur_row, cur_col;       // BIOS cursor on the ACTIVE page
    // (#234b) TEXT DISPLAY PAGES. Mode 2/3 give the guest 8 pages of 4 KiB in
    // the 32 KiB at B8000 and INT 10h AH=05h says which one the CRTC shows.
    // This layer used to answer AH=05h with a comment ("we only implement page
    // 0") and an empty body, and every text write, the presenter and the debug
    // dump all hardcoded page 0. MEASURED on Epyx Rogue: it selects PAGE 3 and
    // draws its whole dungeon there, so the window showed an empty page 0 and
    // the game looked dead. The cursor is per-page on real hardware (the BIOS
    // keeps eight pairs at 0040:0050), so it is per-page here.
    uint8_t      text_page;              // active display page, 0..7
    uint8_t      pg_row[8], pg_col[8];   // saved cursor for the INACTIVE pages
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
    // (rakbd) The 32-bit path delivers ONE scancode per pass and must not lose
    // it if the guest happens to have interrupts off at that instant, so the
    // byte in flight is latched here until a delivery actually succeeds.
    int          k9_pending;           // a scancode is latched and not yet delivered
    uint8_t      k9_code;              // the latched scancode
    int          k9_route_said;        // the "keyboard ISR route" line is one-shot
    int          k9_none_said;         // (rakbd2) the "no ISR at all" line is its OWN
                                       // one-shot. Sharing k9_route_said made the two
                                       // mutually exclusive: the NONE line fires on the
                                       // first pass, BEFORE any guest has had a chance
                                       // to install anything, and then permanently
                                       // suppressed the line that says which route was
                                       // eventually found. MEASURED: with the free-vector
                                       // fix in, the run printed "kbd ISR route: NONE"
                                       // and never printed the 0205h route it then used.
    int          kbd_int9_pm;          // (rakbd2) guest installed INT 9 via DPMI 0205h
    int          k9_first_said;        // the first-delivery bootlog line is one-shot
    int          focus_said;           // the "#156 first pass" line is one-shot
    // (rakbd) THE 8042 OUTPUT BUFFER, for a guest that polls the controller
    // instead of installing an INT 9 handler. Fed by dos_keyq_pump(), which
    // runs only while the guest has NOT hooked INT 9, so this FIFO and
    // dos_deliver_int9()'s replay can never both be live.
    uint8_t      p60_fifo[64];
    uint8_t      p60_rd, p60_wr;
    uint32_t     p60_reads, p64_reads;  // evidence, not control flow
    uint32_t     keyq_pushes;           // keys actually placed in the BIOS ring
    uint32_t     int16_calls;           // guest INT 16h invocations
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
    // (#176) The bus-cycle cost of guest port I/O, which is charged straight
    // into emu_pit_base below. Charging the BASE rather than adding a fourth
    // term to dos_emu_pit_now() is what makes it rebase-safe for free: a rebase
    // freezes the clock at emu_pit_base and restarts the instruction term from
    // zero, so ticks already charged are carried and cannot be counted twice.
    dos_bus_t     bus;
    // (#176) How many rate windows the guest's port traffic alone would have
    // filled more than DOS_BUS_MAX_SHARE_PCT of. Not in DosBus because it is a
    // property of the SAMPLER, not of the bus, and putting it there would mean
    // a layout change every time the sampler grows a counter.
    uint32_t      bus_saturated;
    // Did the MOST RECENT window saturate? Separate from the cumulative count
    // because a cumulative counter used as a status flag never clears, and a
    // report that says "saturated" forever after one burst is worse than no
    // report: it stops being read.
    uint32_t      bus_sat_now;
    uint64_t      emu_pit_base;        // PIT ticks accumulated before emu_insn_base
    unsigned long emu_insn_base;       // insn_count at which emu_pit_base was taken
    uint64_t      next_irq0_pit;       // emulated PIT tick at which IRQ0 fires next
    uint32_t      bios_tick_last;      // last 18.2 Hz tick written to 0040:006C
    // (#234a) The tick counter is TIME OF DAY, not time since launch. The base is
    // seeded from the real clock at guest start and folded down by one day on
    // every midnight crossing, so 0040:006C and INT 1Ah AH=00 report the same
    // number as a real BIOS would, and a guest that seeds its RNG from either
    // gets a different value on every run. It used to start at 0 every time.
    uint32_t      bios_tick_base;      // ticks at guest start, minus days folded
    uint8_t       bios_tick_roll;      // midnight crossings not yet reported

    // #740 M4: the census for asynchronous interrupt delivery into the 32-bit
    // guest. Counted rather than only logged, because the interesting number is
    // a RATIO (delivered vs masked) and a per-delivery log line at 35 Hz would
    // drown the trace it is supposed to make readable.
    uint32_t      irq_deliv[256];      // per-vector deliveries into the LE guest
    uint32_t      irq_masked;          // refused: the guest had EFLAGS.IF clear
    uint32_t      irq_nofit;           // refused: the frame did not fit the arena
    uint32_t      irq_novec;           // due, but no handler is installed


    // Mouse (INT 33h) state, in mode-13h virtual coords
    int          mouse_on;
    int          mx, my, mbtn;            // current
    int          mouse_initialized;
    // #163: the rest of the INT 33h driver surface. int33() implemented only
    // 00/01/02/03/04/07/08/0B, and a game of this era that detects clicks with
    // the PRESS/RELEASE COUNTERS (05/06) rather than by polling 03 saw a mouse
    // that moved nothing and clicked nothing. The counters are the interesting
    // part: they are EDGE state, so they cannot be synthesised at call time
    // from the current button mask, and there is nowhere to put them except
    // here.
    int          mouse_hide_count;        // 01/02 nest depth; >0 means hidden
    int          mprev_x, mprev_y;        // last pumped position (mickey source)
    // (#mickey) THE COUNTER MODEL. `int mick_x, mick_y` used to be ONE
    // free-running accumulator serving BOTH function 0Bh and the SI/DI that a
    // 0Ch upcall carries, scaled by the guest's mickeys-per-pixel ratio. Three
    // separate defects lived in that one line: the scaling doubled vertical
    // motion for a guest that integrates the counters rather than dividing (The
    // Dig, measured), a 0Bh read cleared the counter out from under a callback,
    // and the 16-bit hand-off wrapped after ~32k pixels of travel. See
    // rustkern/dosmick.rs.
    dos_mick_t   mick;
    int          mick_si, mick_di;        // the pair the NEXT upcall hands over
    uint32_t     mtrack_addr;             // DOSDIAG: guest linear of its OWN pointer pair
    uint32_t     mtrack_n;                // lines that instrument has printed
    int          mtrack_lx, mtrack_ly;    // last value it saw, so it logs changes
    int          mbtn_prev;               // previous mask, for edge detection
    uint16_t     mpress_n[3], mrel_n[3];  // 05/06 counts since last read
    uint16_t     mpress_x[3], mpress_y[3];
    uint16_t     mrel_x[3], mrel_y[3];
    int          mmin_x, mmax_x, mmin_y, mmax_y;   // 07/08 clamp range
    uint16_t     mev_seg, mev_off, mev_mask;       // 0Ch/14h user event handler
    uint16_t     mev_pending;             // events seen since the last upcall
    // (raplay) IS the installed 0Ch handler a REAL-MODE far pointer we may
    // execute with x86_16_run()? For a 16-bit guest the answer is always yes.
    // For a 32-bit (LE) guest it depends on HOW the install arrived, which is
    // why dos4gw_rm_dispatch() flags the reflected route: see mev_rm's use in
    // dos4gw_run(). Measured on Red Alert, whose ONLY mouse route is DPMI
    // 0300h BL=33h with a real-mode ES:DX into a DPMI-0100 DOS block.
    uint8_t      mev_rm;
    // (#740 digplay) THE OTHER ROUTE: a NATIVE protected-mode INT 33h 0Ch/14h
    // from a 32-bit client, whose ES is a selector and whose EDX is a 32-bit
    // offset. mev_seg/mev_off cannot hold it: mev_off is 16 bits wide and the
    // handler The Dig installs is at guest linear 0x0013F360, so the top half
    // is exactly the part that matters. Recorded here as the RESOLVED linear
    // address, computed once at the install from the descriptor the guest's ES
    // actually names, and range-checked there rather than at every delivery.
    uint8_t      mev_pm;                  // 1 = a protected-mode handler is armed
    uint16_t     mev_pm_sel;              // the client's ES at the install
    uint32_t     mev_pm_off;              // its full 32-bit EDX
    uint32_t     mev_pm_lin;              // ES.base + EDX, the address we call
    uint32_t     mev_pm_calls;            // delivered
    uint32_t     mev_pm_masked;           // declined: the guest had IF clear
    uint32_t     mev_pm_noret;            // ran out of budget without returning
    uint32_t     mev_pm_dbg;              // DOSDIAG: guest linear to hex-dump per upcall
    // The interrupted register file, saved across a mouse upcall. A far call is
    // made from a driver ISR that has already pushed the interrupted context, so
    // the handler must not be able to perturb it. On the HEAP-allocated task
    // rather than on the stack, deliberately: the DOS run path is deep and this
    // tree has a scar from a self-test buffer on that stack (#212).
    x86_32_cpu_t mev_save;
    // Set for exactly the duration of a DPMI 0300h (simulate real-mode
    // interrupt) reflection, so a service can tell a REAL-MODE frame from a
    // protected-mode one without threading a parameter through every handler.
    uint8_t      rm_reflect;
    int          mratio_x, mratio_y;      // 0Fh/1Ah mickeys per 8 pixels
    uint32_t     mcall_n[64];             // #163 diag: calls by AX (0..63)
    uint32_t     mcall_other;

    // ---- (#181) SOUND BLASTER --------------------------------------------
    // The card, the DMA controller, and the one thread that turns the two into
    // audio. The pump is created lazily on the first armed transfer, because a
    // title that never touches the card must not pay for a thread.
    dos_sb_t     sb;
    dos_dma_t    dma;
    int          sb_pump_live;
    volatile int sb_pump_stop;
    wait_queue_head_t sb_wq;          // pump sleeps here; dos_out wakes it
    uint32_t     sb_blocks;           // census: blocks played
    uint64_t     sb_bytes;            // census: guest bytes DMAed
    uint32_t     sb_irq_deliv;        // census: IRQs pushed into the guest
    uint32_t     sb_irq_unacked;      // census: guest never read base+0xE
    uint32_t     sb_open_fail;        // census: sink refused (EBUSY/ENODEV)
    // (#sbirq32) One-shot reporting for the 32-bit guest's IRQ5 route. Three
    // separate flags, not one, for the reason k9_none_said records: "this guest
    // has no handler" and "this guest has one and here is where" are different
    // facts that need opposite fixes, and a shared flag silences whichever
    // happens second.
    int          sb_route_said;       // the "SB IRQ route" line is one-shot
    int          sb_none_said;        // the "no handler in any table" line is its own
    int          sb_first_said;       // the first-delivery bootlog line is one-shot
    uint32_t     sb_irq_latched;      // census: passes with an IRQ up and no handler

    // window host
    int          host_slot;
    // #156: last-known focus state of host_slot, so the main loop can detect
    // an EDGE (focus gained/lost) and flush stale queued input on it rather
    // than replaying a backlog of keys/clicks typed into another window the
    // instant this one regains focus. See dos_run_file()'s main loop.
    int          last_focused;
    uint32_t    *win_buf;                 // ARGB content buffer
    int          win_w, win_h;            // content buffer size
    // (#745 local 105) Host-window RESIZE HANDOVER. The window manager
    // reallocates this buffer on another thread; see dos_host_rebind().
    uint32_t    *pend_buf;                // buffer to adopt at the next present
    int          pend_w, pend_h;
    int          pend_new;                // a rebind is waiting to be adopted
    uint32_t    *pend_free[DOS_PEND_FREE_MAX];  // old buffers WE must free
    int          pend_free_n;
    int          presenting;              // this thread is inside dos_present()
    // (#dosfs) THE SURROUND, PAINTED ONCE. dos_fill_bars() used to repaint the
    // whole letterbox margin every frame, on the stated grounds that it was "a
    // few percent of the pixels the scale itself writes". That was true while
    // the picture filled the window. Under the pixel budget the picture is
    // deliberately SMALLER than a large window, so on a maximised 3840x2160
    // one the margin is 5.99 Mpx against the picture's 2.30 Mpx: the margin
    // becomes the majority of the frame and repainting it every time would
    // spend most of the saving on drawing black over black.
    //
    // So it is painted when it CHANGES, and the cache key is everything that
    // can change it: the buffer it was painted into, that buffer's size, and
    // the picture rectangle inside it. Nothing else writes outside the picture.
    // The one case a value-only key would miss is a recycled allocation (the WM
    // frees a buffer and kmalloc hands the same address back at the same size),
    // so dos_present() clears bars_buf at the handover rather than relying on
    // the pointer having changed.
    uint32_t    *bars_buf;
    int          bars_w, bars_h;
    dos_rect_t   bars_pic;

    volatile int running;

    // ---- #740: the DOS/4GW guest -----------------------------------------
    // A 32-bit protected-mode guest SHARES this task rather than getting one of
    // its own, and that is the point of putting it here. It reuses, unchanged:
    // the host window and the present path, the console sink (svc.con.putc),
    // the guest-fs identity armed by dos_launch_common(), the INT 21h service
    // context, and the whole teardown block at the end of dos_run_file(). What
    // it replaces is the loader and the interpreter, which is exactly the part
    // that differs. dos/dos4gw.h has the memory map and why it is one buffer.
    int             le_active;      // this task is running an LE, not an MZ
    // (#67/#168) Frames dos4gw_run() presented. It exists because the run
    // summary below is printed by the SHARED teardown, which cannot see a
    // 32-bit run loop local. See the note on that kprintf.
    uint32_t        le_frames;
    x86_32_cpu_t    le_cpu;
    le_module_t     le_mod;
    dpmi_arena_t    le_arena;       // the first megabyte of le_cpu's own space
    // (#211) The WHOLE flat space, for the DPMI services that move a structure
    // to or from the client's own memory (000B/000C). Separate from le_arena
    // because that one is the 1 MiB real-mode window and bounds a different
    // kind of pointer; one struct serving both would have to be the larger,
    // and then an RMCS could name memory a real-mode program cannot reach.
    dpmi_arena_t    flat_arena;
    int             go32_active;    // this LE-style guest is a DJGPP COFF, not an LE
    // (#211) The service-call RING. Written per call, printed ONCE at teardown.
    // See go32_trace(): the serial console drops characters under load, so a
    // per-call kprintf loses exactly the evidence a first contact needs.
    uint32_t        go32_trace_n;   // total calls seen (may exceed the ring)
    uint8_t         go32_tr_vec[GO32_TRACE_RING];
    uint16_t        go32_tr_ax[GO32_TRACE_RING];
    uint32_t        go32_tr_eip[GO32_TRACE_RING];
    uint32_t        go32_tr_esp[GO32_TRACE_RING];
    uint32_t        go32_tr_edi[GO32_TRACE_RING];
    // (#211) Service-call cost, per vector, in microseconds. See go32_trace().
    uint64_t        go32_us[256];
    uint32_t        go32_calls[256];
    void           *le_state;       // opaque rustkern/dos4gw.rs state
    uint32_t        le_arena_size;
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
// (#740) Defined further down next to the other IVT logic; dos_int_handler()
// needs it and runs first in this file.
static int dos_vec_hooked(dos_task_t *t, uint8_t vec);
static uint16_t dos_vec_seed_stub(uint8_t vec);
// (raplay) rustkern/dpmi.rs: seed one entry of the DPMI host's real-mode vector
// shadow (0200h/0201h) so it agrees with the stub we write into the arena.
extern void dpmi_rmvec_seed_rs(uint8_t vec, uint16_t seg, uint16_t off);
// (#dpmi301) Is this real-mode vector one the GUEST published with DPMI 0201h,
// as opposed to a stub this host seeded? Returns 1 and fills seg/off if so.
// The provenance bit it reads is set ONLY by the 0201h arm; see the comment on
// RMVEC_GUEST in rustkern/dpmi.rs for why a value comparison cannot answer it.
extern int dpmi_rmvec_guest_rs(uint8_t vec, uint16_t *seg, uint16_t *off);
// (#sbirq32) Deliver a pending Sound Blaster end-of-block IRQ to a 32-bit
// DOS/4GW guest. Declared here because dos4gw_rm_exec_guest() calls it and runs
// ~1500 lines earlier in this file than the vector-routing code it is built on.
// `nested` = 1 means "we are already inside a 16-bit run on t->cpu".
static void dos4gw_sb_irq(dos_task_t *t, int nested);
// (#sbirq32) Publish the BIOS 18.2 Hz tick at 0040:006C. Declared here for the
// same reason: dos4gw_rm_exec_guest() must keep it moving while it runs a
// guest handler, and it is defined next to dos4gw_timebase(), its other caller.
static void dos4gw_bios_tick(dos_task_t *t);
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

// (#rafault) THE 0Ch UPCALL KILL SWITCH, /CONFIG/DOSMEV.CFG = "0".
//
// The mouse-event upcall to a 32-bit guest is recent (raplay, 2026-08-27), it
// runs GUEST code from a slice boundary, and it was therefore the first suspect
// when Red Alert derailed later the same day. "Did the new thing break it" is a
// question worth ONE run rather than a rebuild, so it is a config file: 1 =
// deliver (the shipped behaviour), 0 = do not.
//
// It was NOT the cause that time (the answer was a 128-byte DTA copy-back over
// a 43-byte result; see rustkern/dos4gw.rs's DTA_FIND_LEN), and the switch is
// kept precisely because that is only knowable by trying it. It costs one
// comparison in a path that already reads three task fields.
static int g_dos_mev_upcall = 1;
// (#mickey) /CONFIG/DOSMOUSE.CFG. `home=<n>` sets the re-home interval in
// delivered move events and `home=off` disables it; `gain=ratio` puts the
// guest's mickeys-per-pixel ratio back into the counters, which is the
// pre-fix behaviour and is correct only for a guest that divides by it again.
// Policy, not per-run state, so an INT 33h 00h reset must not clear it.
static uint32_t g_dos_mick_home_every = DOS_MICK_HOME_UNSET;
static int      g_dos_mick_gain_ratio = 0;
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
// (#175) /CONFIG/DOSIO.CFG: log every guest I/O port access dos_in/dos_out do
// not decode. Diagnostic only, off in the golden. Rate-limited per port so a
// tight poll on one port cannot bury the first touch of another, which is the
// access that names the next wall.
// 0 = off, 1 = #175 behaviour (undecoded ports only), 2 = #176 (every access).
static int g_dos_iotrace = 0;
#define DOS_IOTRACE_PORTS 64
#define DOS_IOTRACE_EACH  40
static struct { uint16_t port; uint8_t rw; uint32_t n; } g_iot[DOS_IOTRACE_PORTS];
static int g_iot_n = 0;

// ---- (#177) 0x3DA BURST DIAGNOSTIC --------------------------------------
// TEMPORARY INVESTIGATION SCAFFOLDING. /CONFIG/DOS3DA.CFG holds N; the first
// time the guest's cumulative 0x3DA/0x3BA read count reaches N, and at 5N, 25N
// and 125N after that, this dumps the EXISTING instruction ring
// (x86_16_ring_dump, #201) plus a per-call-site census of which guest CS:IP
// issued the reads. Deliberately reuses the ring rather than adding a third
// instrument: the ring already carries the opcode bytes and the register file,
// which is exactly what "which loop is this and what is it testing" needs, and
// the guest image is LZEXE-packed on disk so the unpacked code exists ONLY at
// runtime. Off in the golden, like every other DOS*.CFG gate.
// (#177) THE BITS AS A FUNCTION OF THE BEAM, NOT OF THE READ COUNT.
//
// The legacy handler flips 0x01/0x08/0x80 on EVERY read. That guarantees a
// one-bit poll makes progress, which is why it was written, but it also makes
// the register a period-2 function of the READ COUNT, and a guest that reads
// TWICE and expects the two answers to agree is then broken BY CONSTRUCTION.
// Keen 5 is exactly such a guest (2013:0779..078A, reconstructed from the
// instruction ring):
//
//      779: in al,dx / test al,1 / jne 779    wait while display-DISABLED
//      77e: in al,dx / test al,1 / je  77e    wait FOR the next blank to start
//      783: in al,dx / test al,8 / jne 76f    restart if inside vertical retrace
//           .......... test al,1 / je  76f    restart if the blank already ended
//      78c: <the safe window: write the CRTC start address and the ATC pan>
//
// This is the ordinary snow-avoidance idiom: find a horizontal blanking
// interval that is not inside vertical retrace, then touch the CRTC. Under the
// alternating handler the loop at 77e can only exit on a read that returned
// bit0 SET, so the very next read at 783 ALWAYS returns bit0 CLEAR and the
// `je 76f` at 78a is ALWAYS taken. 78c is unreachable. The guest is not
// polling; it is trapped, and the 13.8 million reads #177 was opened about are
// that trap, not a workload.
//
// On real hardware each state persists for THOUSANDS of bus cycles because it
// is a function of where the beam is. So derive it from the one emulated clock
// this subsystem already has (dos_emu_pit_now, #172/#176) rather than from a
// counter of our own: 70 Hz, 449 total scanlines of 800 dot columns, 400x640
// active, vertical retrace over lines 412..413, which are the standard VGA
// 320x200/360x400 timings.
//
// WHY THIS CANNOT REINTRODUCE AN UNTERMINATABLE POLL, which is the property the
// alternating handler was protecting. Under the SHIPPED configuration a port
// access charges the bus 1000 ns (#176), so dos_emu_pit_now() advances by about
// 1.193 PIT ticks, i.e. about 25 dot columns, on EVERY read, strictly and with
// no dependence on the instruction term. The narrowest window a guest can wait
// for is the 160-column horizontal blank, which is therefore about 6 reads
// wide: it cannot be stepped over. With the bus cost forced to 0 the
// instruction term alone still advances the clock (about 6 columns per read),
// so the window only gets wider. Every wait is bounded by one frame.
//
// BIT 7 IS DELIBERATELY LEFT ALTERNATING. It is not a VGA status bit at all
// (real VGA reads 0 there); it toggles because the existing comment records
// that id's VGA-detection routine watches it change. Making bits 0 and 3
// truthful is the whole fix, and re-deciding bit 7 at the same time would put
// two changes behind one measurement.
static uint64_t dos_emu_pit_now(dos_task_t *t);   // defined below, with the clock
#define DOS_VGA_LINES_TOTAL   449u
#define DOS_VGA_LINES_ACTIVE  400u
#define DOS_VGA_COLS_TOTAL    800u
#define DOS_VGA_COLS_ACTIVE   640u
#define DOS_VGA_VR_FIRST      412u
#define DOS_VGA_VR_LAST       413u
#define DOS_VGA_FRAME_TICKS   (DOS_PIT_HZ / 70UL)
#define DOS_VGA_FRAME_DOTS    ((uint64_t)DOS_VGA_LINES_TOTAL * DOS_VGA_COLS_TOTAL)
static int g_dos_3da_timebase = 0;   // /CONFIG/DOS3DAT.CFG = 1
static uint8_t dos_3da_beam(dos_task_t *t) {
    uint64_t p   = dos_emu_pit_now(t) % (uint64_t)DOS_VGA_FRAME_TICKS;
    uint64_t dot = (p * DOS_VGA_FRAME_DOTS) / (uint64_t)DOS_VGA_FRAME_TICKS;
    uint32_t line = (uint32_t)(dot / DOS_VGA_COLS_TOTAL);
    uint32_t col  = (uint32_t)(dot % DOS_VGA_COLS_TOTAL);
    uint8_t v = 0;
    // bit 0 is DISPLAY DISABLED: set in horizontal blank or in vertical blank.
    if (line >= DOS_VGA_LINES_ACTIVE || col >= DOS_VGA_COLS_ACTIVE) v |= 0x01;
    if (line >= DOS_VGA_VR_FIRST && line <= DOS_VGA_VR_LAST)        v |= 0x08;
    return v;
}
static uint64_t g_dos_3da_n = 0;      // cumulative reads this run
static uint64_t g_dos_3da_trip = 0;   // next dump threshold; 0 = disarmed
#define DOS_3DA_SITES 12
static struct { uint32_t csip; uint64_t n; } g_3da_site[DOS_3DA_SITES];
static int g_3da_site_n = 0;
static uint64_t g_3da_site_lost = 0;
static void dos_3da_census(uint16_t cs, uint16_t ip) {
    uint32_t k = ((uint32_t)cs << 16) | ip;
    for (int i = 0; i < g_3da_site_n; i++)
        if (g_3da_site[i].csip == k) { g_3da_site[i].n++; return; }
    if (g_3da_site_n >= DOS_3DA_SITES) { g_3da_site_lost++; return; }
    g_3da_site[g_3da_site_n].csip = k;
    g_3da_site[g_3da_site_n].n = 1;
    g_3da_site_n++;
}
static void dos_iotrace(uint16_t port, int is_write, uint16_t val, int width) {
    if (!g_dos_iotrace) return;
    int i;
    for (i = 0; i < g_iot_n; i++)
        if (g_iot[i].port == port && g_iot[i].rw == (uint8_t)is_write) break;
    if (i == g_iot_n) {
        if (g_iot_n >= DOS_IOTRACE_PORTS) return;
        g_iot[i].port = port; g_iot[i].rw = (uint8_t)is_write; g_iot[i].n = 0;
        g_iot_n++;
    }
    g_iot[i].n++;
    if (g_iot[i].n <= DOS_IOTRACE_EACH)
        kprintf("[IOTRACE] %s port=0x%03X val=0x%04X w=%d n=%u\n",
                is_write ? "OUT" : "IN ", port, val, width, g_iot[i].n);
}

// (#176) EVERY access, decoded or not, at trace level 2. Called from the ONE
// place each of dos_in/dos_out already charges the bus, so the census the
// histogram reports and the census the charge is applied to are the SAME
// census and cannot diverge.
//
// #175 scoped this instrument to UNDECODED ports because it was hunting the
// next WALL, and a wall is by definition somewhere we decode nothing. #176
// needs the opposite question answered - where does this guest's port traffic
// actually GO - and for that the DECODED ports are the interesting ones,
// because a port we decode is a port a guest can poll in a tight loop.
static void dos_iotrace_all(uint16_t port, int is_write, int width) {
    if (g_dos_iotrace < 2) return;
    dos_iotrace(port, is_write, 0xFFFF, width);
}
// Called at guest exit AND periodically from the run loop, because the case
// this instrument exists to catch is a guest that never reaches INT 21h 4Ch.
static void dos_iotrace_dump(void) {
    if (!g_dos_iotrace) return;
    kprintf("[IOTRACE] ---- unclaimed port summary (%d entries) ----\n", g_iot_n);
    for (int i = 0; i < g_iot_n; i++)
        kprintf("[IOTRACE] %s port=0x%03X count=%u\n",
                g_iot[i].rw ? "OUT" : "IN ", g_iot[i].port, g_iot[i].n);
    kprintf("[IOTRACE] ---- end ----\n");
}
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

// ---- guest CPU speed cap: where the number comes from (#232) --------------
//
// GENERAL, NOT A PER-TITLE HACK. Every DOS guest gets the same resolver and the
// same sources; Joust is simply the first title that needed a non-default
// answer, and it supplies that answer itself (see source 2). Nothing in this
// file names a game.
//
// Sources, FIRST HIT WINS:
//   1. <program dir>/SPEED.CFG        one decimal number, the cycles value.
//                                     "0", "off" or "max" means "no cap".
//                                     The explicit per-title override: use this
//                                     for a game with no DOSBox packaging.
//   2. <program dir>/START.bat        a DOSBox `cpu cycles=N` line. These game
//                                     packs already ship one and it is already
//                                     on the disk, so the value a human already
//                                     chose for this title is used rather than
//                                     re-derived. Read only for the number; the
//                                     batch file is NOT executed.
//   3. /CONFIG/DOSCYCLES.CFG          one decimal number: a system-wide default
//                                     for every DOS guest that has neither of
//                                     the above.
//   4. nothing                        DOS_CYCLES_OFF, i.e. exactly the
//                                     behaviour before this change. A guest
//                                     that is already correctly paced (Keen,
//                                     anything DOS/4GW) is untouched.
static uint32_t dos_cycles_parse(const char *b, uint32_t n) {
    // Skip leading blanks, then read one unsigned decimal. "off"/"max"/"0" ->
    // DOS_CYCLES_OFF. Anything unparseable is treated as absent, not as zero:
    // a typo must not silently uncap a guest that asked to be capped.
    uint32_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    if (i + 2 < n && (b[i] == 'o' || b[i] == 'O') &&
        (b[i+1] == 'f' || b[i+1] == 'F')) return DOS_CYCLES_OFF;
    if (i + 2 < n && (b[i] == 'm' || b[i] == 'M') &&
        (b[i+1] == 'a' || b[i+1] == 'A')) return DOS_CYCLES_OFF;
    if (i >= n || b[i] < '0' || b[i] > '9') return 0xFFFFFFFFu;   // "absent"
    uint64_t v = 0;
    while (i < n && b[i] >= '0' && b[i] <= '9') {
        v = v * 10u + (uint32_t)(b[i] - '0');
        if (v > 0xFFFFFFFFull) return 0xFFFFFFFFu;
        i++;
    }
    if (v == 0) return DOS_CYCLES_OFF;
    if (v < DOS_CYCLES_MIN) v = DOS_CYCLES_MIN;
    if (v > DOS_CYCLES_MAX) v = DOS_CYCLES_MAX;
    return (uint32_t)v;
}

// Find `cycles=` (case-insensitive) anywhere in a buffer and parse the number
// after it. That is the shape DOSBox uses in both a .conf ([cpu] cycles=500)
// and in the `config -set "cpu cycles=500"` form our packs ship, so one matcher
// covers both without knowing which it is looking at.
static uint32_t dos_cycles_from_conf(const char *b, uint32_t n) {
    static const char k[] = "cycles";
    if (n < sizeof(k)) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i + (sizeof(k) - 1) < n; i++) {
        uint32_t j = 0;
        while (j < sizeof(k) - 1) {
            char c = b[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != k[j]) break;
            j++;
        }
        if (j != sizeof(k) - 1) continue;
        uint32_t q = i + (sizeof(k) - 1);
        while (q < n && (b[q] == ' ' || b[q] == '\t')) q++;
        if (q < n && b[q] == '=') q++;
        while (q < n && (b[q] == ' ' || b[q] == '\t')) q++;
        uint32_t r = dos_cycles_parse(&b[q], n - q);
        if (r != 0xFFFFFFFFu) return r;
    }
    return 0xFFFFFFFFu;
}

// (#speedcap) MAKE THE FILE THE SPEED DIALOG HAS TO REWRITE ACTUALLY WRITABLE.
//
// THE BUG THIS FIXES, MEASURED, NOT INFERRED. On golden 2300 the #778 per-window
// Speed dialog opens, renders, reads the current value and accepts a preset, and
// then its Save DOES NOTHING, silently. The kernel says why:
//
//   [PERMS-DENY] proc=COMPOSIT uid=1000 gid=1000 want=-wx path=/DOS/KEEN5
//
// /DOS/<GAME> is root-owned 0755 on purpose (a user who can rewrite the
// executable hands the next user a different program), the compositor runs as
// the logged-in desktop user, and dosspeed.c's ds_write_cycles() returns without
// a word when the open fails. So the control changed a number on screen and
// nothing else, which is the exact failure mode #778's own CHANGELOG entry
// admitted had never been tested.
//
// THE FIX IS THE FILE, NOT THE DIRECTORY. sys_open()'s rule (proc/syscall.c) is
// "creating a NAME is a write to the parent directory; writing an EXISTING file
// is a write to that file". So a SPEED.CFG that already exists and is mode 0666
// is rewritable by the desktop session while the directory and the executable
// beside it stay root-owned 0755 and the blast radius does not move. That is why
// this pairs with shipping a SPEED.CFG for every title: the default is what makes
// the control work, not merely what sets the initial speed.
//
// GENERAL, NOT A PER-TITLE LIST. Nothing here names a game, which is the property
// dos_speed_cycles_for()'s own comment insists on; the path is derived from the
// binary being launched. Applied on EVERY launch rather than once, the same way
// dos_overlay_prepare() re-applies its 0750, so a file written before this
// existed is corrected rather than left wrong forever.
//
// Precedent, not a new policy: /DOS/NETHACK/RECORD and /GAMES/SIMCITY already
// ship writable-by-everyone for exactly this reason (perms_shared_state_seed[]
// in fs/perms.c). The difference is that this one needs no entry per game.
static void dos_speed_cfg_make_writable(const char *path) {
    if (!path) return;
    char fp[224];
    uint32_t last = 0, k = 0;
    for (uint32_t i = 0; path[i] && i < sizeof(fp) - 16; i++) {
        fp[i] = path[i];
        if (path[i] == '/') last = i + 1;
    }
    k = last;
    static const char nm[] = "SPEED.CFG";
    for (uint32_t j = 0; j < sizeof(nm) - 1 && k < sizeof(fp) - 1; j++) fp[k++] = nm[j];
    fp[k] = 0;
    // Only an EXISTING file. Creating one here would invent a cap nobody asked
    // for, and an absent SPEED.CFG is a legitimate state (the title's number may
    // come from START.bat or from the system default).
    if (!fat_exists(&g_fat_fs, fp)) return;
    perms_set(fp, 0, 0, 0666);
}

static uint32_t dos_speed_cycles_for(const char *path, const char **src_out) {
    char dir[192];
    uint32_t dl = 0, last = 0;
    for (uint32_t i = 0; path && path[i] && i < sizeof(dir) - 24; i++) {
        dir[i] = path[i];
        dl = i + 1;
        if (path[i] == '/') last = i + 1;
    }
    (void)dl;
    dir[last] = 0;                    // "/DOS/JOUST/" (keeps the trailing slash)

    struct { const char *name; int conf; } cand[2] = {
        { "SPEED.CFG", 0 },           // 1: explicit per-title override
        { "START.BAT", 1 },           // 2: the DOSBox line the pack already ships
    };
    for (int c = 0; c < 2; c++) {
        char fp[224];
        uint32_t k = 0;
        while (dir[k] && k < sizeof(fp) - 16) { fp[k] = dir[k]; k++; }
        const char *nm = cand[c].name;
        while (*nm && k < sizeof(fp) - 1) fp[k++] = *nm++;
        fp[k] = 0;
        uint32_t sz = 0;
        char *buf = (char *)fat_read_file(&g_fat_fs, fp, &sz);
        if (!buf) continue;
        uint32_t v = cand[c].conf ? dos_cycles_from_conf(buf, sz)
                                  : dos_cycles_parse(buf, sz);
        kfree(buf);
        if (v != 0xFFFFFFFFu) {
            if (src_out) *src_out = cand[c].name;
            return v;
        }
    }
    {   uint32_t sz = 0;
        char *buf = (char *)fat_read_file(&g_fat_fs, "/CONFIG/DOSCYCLES.CFG", &sz);
        if (buf) {
            uint32_t v = dos_cycles_parse(buf, sz);
            kfree(buf);
            if (v != 0xFFFFFFFFu) {
                if (src_out) *src_out = "/CONFIG/DOSCYCLES.CFG";
                return v;
            }
        }
    }
    if (src_out) *src_out = "default";
    return DOS_CYCLES_OFF;
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
// #232 diagnostic gate: armed by /CONFIG/DOSSPEED.CFG at guest launch, same
// family as g_x86_dbgring (DOSDIAG.CFG) and g_dos_iotrace (DOSIO.CFG).
static volatile int g_dos_speedlog = 0;

static uint32_t dos_emu_hz(void) {
    return g_dos_emu_hz ? g_dos_emu_hz : (uint32_t)DOS_EMU_INSN_HZ;
}

// WHICH GUEST'S RETIRED-INSTRUCTION COUNT IS THE CLOCK. Asked here and nowhere
// else.
//
// There is ONE emulated timebase, and its unit of elapsed time is a retired
// guest instruction. A task runs EITHER the 16-bit interpreter or the 32-bit
// one and never both (dos_run_file dispatches on the image), so "which counter"
// has a single answer per task. Answering it in one function rather than at
// each of the four call sites is what stops the 32-bit guest ending up with a
// working IRQ0 pace and a PIT counter on ports 0x40/0x43 that never moved, or
// the reverse: both are derived from this, so they cannot disagree.
// (#sbirq32) AND SINCE #dpmi301 A TASK DOES RUN BOTH, SO THE CLOCK MUST COUNT
// BOTH. The paragraph above says "a task runs EITHER the 16-bit interpreter or
// the 32-bit one and never both". That was true when it was written and stopped
// being true when DPMI 0300h began EXECUTING a guest's own real-mode handler on
// t->cpu (dos4gw_rm_exec_guest). While one of those runs, le_cpu.insn_count is
// frozen, so this function returned a CONSTANT and every clock derived from it
// - dos_emu_pit_now(), dos_bios_tick_now(), the 0040:006C dword, the 0x3DA beam
// - stopped dead for the whole call.
//
// MEASURED consequence, Discworld II: SBLASTER.DIG's hardware probe (AIL
// function 0304h) waits for the BIOS tick to change with
//     mov ax,[es:0x046C] ; L: cmp ax,[es:0x046C] ; jz L
// which has no escape at all, twice over, and then checks whether its IRQ
// handler fired. With the tick frozen the first of those two spins can never
// end: 2,000,000 instructions and ZERO port I/O, cut off by the budget, AX
// still holding the tick value it read. The driver's own CX=10 timeout was
// unreachable, so even the "no card" answer could not be produced.
//
// Summing is the correct answer and not a patch: both counters count THIS
// guest's retired instructions, the sum is monotonically non-decreasing (which
// is the only property a clock derived from it needs), and dos_emu_rebase()
// captures its base through this same function, so nothing can disagree with
// it.
static unsigned long dos_emu_insns(const dos_task_t *t) {
    return t->le_active
        ? (unsigned long)(t->le_cpu.insn_count + t->cpu.insn_count)
        : t->cpu.insn_count;
}

// The emulated clock, in PIT ticks. Monotonic BY CONSTRUCTION: only the part of
// the run since the last rebase is converted at the current rate.
static uint64_t dos_emu_pit_now(dos_task_t *t) {
    unsigned long d = dos_emu_insns(t) - t->emu_insn_base;
    return t->emu_pit_base + ((uint64_t)d * DOS_PIT_HZ) / dos_emu_hz();
}

// Adopt a newly measured instruction rate WITHOUT moving any instant that has
// already happened: freeze the elapsed ticks at the old rate, then switch.
static void dos_emu_rebase(dos_task_t *t, uint32_t new_hz) {
    if (!new_hz) return;
    t->emu_pit_base  = dos_emu_pit_now(t);
    t->emu_insn_base = dos_emu_insns(t);
    g_dos_emu_hz     = new_hz;
}

// (#176) THE RATE THAT KEEPS EMULATED TIME EQUAL TO REAL TIME once port I/O
// has a cost of its own.
//
// This subsystem has a STATED CONTRACT, written at the emu_pit_base field and
// load-bearing for every DOS title: "deriving both from insn_count against the
// MEASURED instruction rate makes emulated time track real time whatever the
// pacing". A guest's clock must advance at about DOS_PIT_HZ ticks per REAL
// second, because that is what makes a game run at the right speed on the wall
// clock however fast or slow the host interpreter happens to be.
//
// Charging the bus in absolute microseconds ON TOP of the instruction term
// BREAKS that contract, and not by a rounding error. MEASURED on Commander
// Keen 5 (build 17602, one 70 s run per arm): its EGA planar drawing writes the
// sequencer and graphics-controller INDEX ports about 160,000 times a second,
// which at 1 us each is 14% of a second of bus time per second, so its clock,
// and therefore its animation, ran 14% fast; and during its startup VGA sync,
// where it reads 0x3DA 13.8 MILLION times, the bus term reached 74% of the
// elapsed clock, i.e. an instantaneous 3.8x.
//
// THE FIX IS NOT TO CHARGE LESS. It is to stop charging the bus in ADDITION to
// a full second of instruction time, and start charging it OUT OF the same
// second. Real hardware has exactly one second per second: a machine that
// spends 140 ms of it driving the ISA bus has 860 ms left for instructions, and
// its CPU does not run faster to compensate. So:
//
//     bus_ticks + insns * PIT_HZ / rate  ==  PIT_HZ * wall_seconds
//   =>            rate  ==  insns * PIT_HZ / (PIT_HZ * wall_seconds - bus_ticks)
//
// which is what this returns. Note the degenerate case, which is the proof that
// this is a strict generalisation and not a new policy: with no bus charge,
// bus_ticks is 0 and the expression collapses to insns * 1000 / wall_ms, the
// EXACT formula this replaces. The cost=0 arm is therefore bit-identical in
// behaviour to the pre-#176 kernel, which is what makes the two arms a
// controlled comparison.
//
// WHAT IT COSTS THE GUEST: nothing. The instruction rate is REPORTED higher, so
// each instruction is worth LESS emulated time; the interpreter still runs
// flat out and retires exactly as many instructions per real second as before.
// What changes is only the SHARE of the guest's clock that an instruction and a
// bus cycle each get, which is the one thing #176 exists to correct.
//
// THE SATURATION CASE IS REAL AND IS REPORTED RATHER THAN HIDDEN. An 8-bit ISA
// bus carries at most about 1e6 accesses per second. A guest issuing more than
// that (Keen 5's 3.3 M/s burst does) is executing a loop that no real machine
// could execute that fast, so no consistent timebase exists for it: the bus
// term alone already exceeds the second it has to fit in. The instruction term
// is clamped to a floor rather than driven negative, the event is counted, and
// [IOCOST] says so. That number is a genuine diagnosis signal in its own right:
// it means our emulation let a poll run far longer than the hardware would
// have, which is the shape of #172's and #175's walls.
#define DOS_BUS_MAX_SHARE_PCT 90u
// (#234a) THE BIOS 18.2065 Hz TICK, ONE GENERATOR FOR BOTH RUN LOOPS.
//
// Both the 16-bit loop and dos4gw_run() maintain the dword at 0040:006C, and
// both used to compute it as `dos_emu_pit_now(t) >> 16`: ticks since THIS GUEST
// STARTED, i.e. always 0 at launch. A real BIOS counts from midnight, and a
// surprising number of DOS programs seed their random generator from it. Two
// copies of a wrong formula is how the defect survives a fix to one of them, so
// there is now one function and both loops call it.
//
// The midnight fold is done by moving the BASE, not by masking the result, so
// the rollover is counted exactly once and INT 1Ah AH=00's AL flag is true.
static uint32_t dos_bios_tick_now(dos_task_t *t) {
    uint32_t raw = t->bios_tick_base + (uint32_t)(dos_emu_pit_now(t) >> 16);
    if (raw >= KDOS_TICKS_PER_DAY) {
        uint32_t days = raw / KDOS_TICKS_PER_DAY;
        raw -= days * KDOS_TICKS_PER_DAY;
        t->bios_tick_base -= days * KDOS_TICKS_PER_DAY;
        uint32_t r = (uint32_t)t->bios_tick_roll + days;
        t->bios_tick_roll = (uint8_t)(r > 255u ? 255u : r);
    }
    return raw;
}

// Binary -> packed BCD, for the INT 1Ah AH=02h/04h RTC reads, which report in
// BCD because the MC146818 does. Values above 99 cannot occur: every caller
// passes a field ktime_dos_clock_rs has already range-reduced.
static inline uint8_t dos_bcd8(uint8_t v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static uint32_t dos_emu_clock_rate(dos_task_t *t, unsigned long di,
                                   uint64_t dt_ms, uint64_t *bus_prev,
                                   uint32_t raw_hz) {
    uint64_t dbus = t->bus.ticks_charged - *bus_prev;
    *bus_prev = t->bus.ticks_charged;
    if (!dt_ms || !di) return raw_hz;
    uint64_t budget = ((uint64_t)DOS_PIT_HZ * dt_ms) / 1000ull;
    if (!budget) return raw_hz;
    uint64_t cap = (budget * DOS_BUS_MAX_SHARE_PCT) / 100ull;
    t->bus_sat_now = 0;
    if (dbus > cap) { t->bus_saturated++; t->bus_sat_now = 1; dbus = cap; }
    uint64_t avail = budget - dbus;
    if (!avail) return raw_hz;
    uint64_t r = ((uint64_t)di * (uint64_t)DOS_PIT_HZ) / avail;
    if (r > 0xFFFFFFFFull) r = 0xFFFFFFFFull;
    if (r < 100000ull)     r = 100000ull;
    return (uint32_t)r;
}

// (#176) ONE guest port access has happened. Charge the emulated clock the
// cost of an ISA bus cycle.
//
// WHY THIS ADDS TO emu_pit_base RATHER THAN TO dos_emu_pit_now(). The clock is
//     emu_pit_base + (insns - emu_insn_base) * PIT_HZ / rate
// and emu_pit_base means "ticks that had already elapsed before emu_insn_base".
// Bus time is exactly that: elapsed, already banked, and NOT a function of the
// instruction count. Adding it here makes the clock stay monotonic by
// construction and makes dos_emu_rebase() correct with no change, because a
// rebase freezes the whole clock into emu_pit_base and restarts the
// instruction term at zero. A fourth term in dos_emu_pit_now() would have had
// to be excluded from the rebase by hand, which is the kind of coupling that
// goes wrong silently.
//
// COST TO THE NON-I/O PATH: ZERO. dos_emu_pit_now() is unchanged, so every
// instruction that is not an IN or an OUT pays nothing for this.
static inline void dos_bus_tick(dos_task_t *t) {
    t->emu_pit_base += dos_bus_charge_rs(&t->bus);
}

// (#252) Advance the guest's emulated clock by an explicit interval, for a BIOS
// service that IS a delay. Same accumulator, same ticks_charged, therefore same
// rate correction: the time comes out of the second rather than on top of it.
//
// NOT A WAIT. Nothing blocks, sleeps or spins; the interpreter carries straight
// on and the guest simply finds that its own clock has moved. #426 has nothing
// to catch here because there is no loop.
static uint64_t g_dos_int15_wait_us = 0;   // census, reported with the FM lines
static uint32_t g_dos_int15_waits   = 0;
static inline void dos_bus_charge_us(dos_task_t *t, uint32_t us) {
    t->emu_pit_base += dos_bus_charge_us_rs(&t->bus, us);
    g_dos_int15_wait_us += us;
    g_dos_int15_waits++;
}

// (#172) Now per-channel. All three run off the SAME emulated timebase (the
// guest's retired-instruction clock), which is correct: on the hardware all
// three counters are clocked by the one 1.193182 MHz oscillator and differ
// only in their reload value. Channel 2's gate (port 0x61 bit 0) is not
// consulted, deliberately and stated rather than left to be discovered: a
// gated-off counter that freezes would reintroduce the unterminatable loop
// for any guest that forgets to open the gate, and no guest can tell the
// difference on a free-running read.
static uint16_t dos_pit_count_ch(dos_task_t *t, int ch) {
    uint32_t div = t->pit[ch].divisor ? t->pit[ch].divisor : 65536u;
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

// (#234b) 4 KiB per page, which is what the IBM BIOS uses for an 80x25 mode
// and therefore what every program that does its own page arithmetic assumes.
// It is deliberately NOT 80*25*2 = 4000: pages are 4096 apart on the hardware.
#define DOS_TEXT_PAGE_BYTES 0x1000u
#define DOS_TEXT_PAGES      8

// Cell address on an EXPLICIT page. INT 10h AH=02/03/08/09/0A all take the page
// in BH and a program may legitimately write a page it is not currently
// displaying (that is the point of having pages), so the page is a parameter
// rather than always the active one.
static inline uint32_t dos_text_cell_pg(int page, int row, int col) {
    if (page < 0 || page >= DOS_TEXT_PAGES) page = 0;
    return VGA_B800 + (uint32_t)page * DOS_TEXT_PAGE_BYTES
                    + (uint32_t)((row * TEXT_COLS + col) * 2);
}
static inline uint32_t dos_text_cell(const dos_task_t *t, int row, int col) {
    return dos_text_cell_pg(t ? (int)t->text_page : 0, row, col);
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
            uint32_t o = dos_text_cell(t, r, c2);
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
                uint32_t d = dos_text_cell(t, r, c2), sc = dos_text_cell(t, r + n, c2);
                t->mem[d] = t->mem[sc]; t->mem[d + 1] = t->mem[sc + 1];
            }
        dos_text_fill(t, bot - n + 1, left, bot, right, ' ', attr);
    } else {
        for (int r = bot; r >= top + n; r--)
            for (int c2 = left; c2 <= right; c2++) {
                uint32_t d = dos_text_cell(t, r, c2), sc = dos_text_cell(t, r - n, c2);
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
        uint32_t o = dos_text_cell(t, t->cur_row < TEXT_ROWS ? t->cur_row : TEXT_ROWS - 1,
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
    case 0x26: { // create new PSP at DX (a copy of the current one)
        // (#172) MEASURED: /DOS/STUNTS/LOAD.EXE issues this with DX = the block
        // it just allocated high, then copies its own image in behind the new
        // PSP and re-enters there with " HIGHLOAD " as the command tail. The
        // command tail lives in the PSP, so without this the high instance
        // starts with 256 bytes of uninitialised guest RAM where its arguments,
        // its memory-size word and its INT 22/23/24 save area should be.
        //
        // It is here rather than in the service core for the same reason 48h/
        // 49h/4Ah are: the memory-size word at +02h comes from THIS task's MCB
        // chain, which is the DOS machine, not the DOS API.
        //
        // WHY IT WAS INVISIBLE UNTIL NOW: the generic unimplemented default
        // answers CF=1 AX=1 for an in-range function, and LOAD.EXE's error test
        // is `or ax,ax / jnz`, i.e. it treats ZERO as failure. AX=1 read as
        // success, so the guest logged one MISS and carried on with a PSP that
        // was never created. A stub that returns "error" in a shape the caller
        // reads as "fine" is worse than a MISS line, because the damage lands
        // somewhere else entirely.
        uint16_t dst = c->dx;
        dos_mcb_t *m = dos_mcb_find(t, dst);
        uint16_t end = m ? (uint16_t)(m->seg + m->para) : dos_mcb_next_above(t, dst);
        if (dos_psp_create_rs(t->mem, t->svc.psp_seg, dst, end) != 0) {
            // The only failure is "outside the 1 MiB image", which DOS cannot
            // express on this function. Say invalid-function rather than lie.
            c->ax = 1; SET_CF(c);
            kprintf("[dos] 26h create-PSP at %04x REFUSED (outside 1 MiB)\n", dst);
            break;
        }
        CLR_CF(c);
        if (g_x86_dbgring)
            kprintf("[dos] 26h create-PSP at %04x from %04x, memtop=%04x\n",
                    dst, t->svc.psp_seg, end);
        break;
    }
    case 0x48: { // allocate BX paragraphs -> AX=segment ; on fail BX=largest avail
        // The bump pointer sits above every LIVE block (see dos_mcb_retop), so
        // an allocation cannot land inside a block the program already resized
        // itself into.
        uint16_t para = c->bx;
        uint16_t avail = (uint16_t)(0xA000 - t->alloc_top_para);  // free above the bump
        uint16_t at = t->alloc_top_para;
        // (#172) WHAT A FAILED 48h REPORTS IN BX IS NOT `avail`.
        //
        // `avail` is the space above the BUMP POINTER. The allocator itself has
        // not been limited to that since dos_mcb_first_fit() existed: a request
        // that does not fit above the bump is served from a hole left by a 49h.
        // But the FAILURE REPORT still said `avail`, and BX=FFFFh - the standard
        // "how much is free?" probe, which is REQUIRED to fail - takes that path
        // every single time. So the one call whose only purpose is to report free
        // memory reported the wrong number by construction.
        //
        // MEASURED (golden b1978, /DOS/STUNTS/LOAD.EXE, one run): after the guest
        // does the classic allocate-high dance (grab everything, allocate the
        // piece you want so it lands at the top, free the big low block), its own
        // high block pins the bump at 0x9FFE, `avail` is 2 paragraphs, and a
        // 0x7651-paragraph (464 KB) hole sits between the program's block and it.
        // The guest was told 32 bytes were free, printed "Not enough memory to
        // load program." and exited 1. Aladdin and Monkey Island never showed it
        // because neither does the dance, so for them the bump pointer IS the top
        // of free memory and `avail` was accidentally right.
        uint16_t largest = dos_mcb_largest_free_rs(t->mcb, t->mcb_n,
                                                   t->alloc_floor_para, 0xA000);
        if (largest < avail) largest = avail;   // belt and braces; never under-report
        if (para == 0) { c->ax = 8; SET_CF(c); c->bx = largest; break; }
        if (para > avail) {
            // No room above the bump pointer: look for a hole left by a 49h free.
            at = dos_mcb_first_fit(t, para);
            if (!at) {
                c->ax = 8; SET_CF(c);   // insufficient memory
                c->bx = largest;        // report the largest FREE RUN, not the tail
                if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x FAIL largest=%04x avail=%04x top=%04x ss=%04x\n", para, largest, avail, t->alloc_top_para, c->ss);
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

// (#175) Defined beside dos_present(), its frame-path caller; also called from
// the clean-exit path below, which comes first in the file.
static void dos_opl2_report_silence(dos_task_t *t);

// (#205) Defined beside dos_fm_launch(), which owns the two statics it reads.
// Forward-declared here for the same reason dos_opl2_report_silence() is: the
// clean-exit path comes FIRST in this file, and one function is how the two
// paths are kept from growing different wording.
static void dos_fm_report_exit(uint32_t pushed, uint32_t dropped);

// A DOS title that exits for no visible reason is the hardest thing to
// diagnose here, so spend the instruction ring on exactly that moment.
static void dos_on_terminate(dos_svc_ctx_t *ctx, int code) {
    (void)code;
    if (g_dos_ring_on) x86_16_ring_dump("exit-4Ch", g_dos_ring_dump_n);
    dos_iotrace_dump();   // (#175)
    // (#175) Same one line, on the clean-exit path too, for a guest that quits
    // before the frame path ever reported. ONE function, so the two paths
    // cannot grow different thresholds or different wording.
    {   dos_task_t *t = (dos_task_t *)ctx->owner;
        if (t) dos_opl2_report_silence(t);
    }
    // (#182) Tell the Ring-3 synthesiser the guest is gone. It drains what is
    // left FIRST (dos_fm_drain only reports ENODEV once the queue is EMPTY and
    // inactive), so the final note-off of the session is never lost. Losing it
    // would leave the last note of a game sounding forever, which is a real
    // failure mode and the reason the ordering is this way round.
    {   uint32_t push = 0, drop = 0;
        dos_fmq_host_close(&push, &drop);
        kprintf("[dos] (#182) FM bridge: %u register writes carried to Ring 3, "
                "%u DROPPED%s\n", push, drop,
                drop ? " (the ring overflowed; expect wrong or stuck notes)" : "");
        // #205: DURABLY, and on BOTH arms. A zero here is the single most
        // useful fact about a silent DOS game, because it separates "the
        // synthesiser produced nothing" from "the guest never asked for a
        // note" - and the kprintf above reaches a serial port the owner's
        // laptop has not got, so it recorded neither.
        dos_fm_report_exit(push, drop);
    }
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
    // #221b: the launching user's home, for the "%HOME%" token in a guest path.
    //
    // Read from the identity the #708 gate captured AT LAUNCH, never from
    // proc_current(): by the time this runs we are inside the guest's own
    // kernel thread, whose uid is 0 by construction. That is the same reason
    // dos_launch_common() arms the slot before proc_create().
    {
        uint32_t uid = 0, gid = 0;
        t->svc.homedir[0] = '\0';
        if (guestfs_cred_rs(GUESTFS_SLOT_DOS, &uid, &gid) == 0) {
            user_entry_t *u = user_lookup_uid(uid);
            // A home of "/" is users.c's "this account has no home" fallback.
            // Expanding %HOME% to the filesystem root would aim a guest's save
            // files at a directory it cannot write and must not be able to, so
            // leave the token unexpanded and let the open fail visibly instead.
            if (u && u->home[0] == '/' && u->home[1] != '\0') {
                int n = 0;
                for (; u->home[n] && n < (int)sizeof(t->svc.homedir) - 1; n++)
                    t->svc.homedir[n] = u->home[n];
                while (n > 1 && t->svc.homedir[n - 1] == '/') n--;
                t->svc.homedir[n] = '\0';
            }
        }
        kprintf("[dos] guest home for %%HOME%%: '%s'\n",
                t->svc.homedir[0] ? t->svc.homedir : "(none)");

        // #rawrite: THE PER-USER WRITE OVERLAY, if this title has one.
        //
        // Set up HERE, in the same block as homedir, because it is derived from
        // the same launch-time identity and for the same reason: by the time
        // the guest is running we are inside its own kernel thread, whose uid
        // is 0 by construction, and an overlay computed from THAT would put
        // every player's saves in root's home.
        //
        // Fails soft in every direction. No home, an unlisted title, or a
        // directory that cannot be created all leave the overlay unconfigured,
        // and an unconfigured overlay is byte-for-byte the behaviour this layer
        // had before it existed: the write goes to the install directory and
        // the #708 gate refuses it, visibly, with a [GUESTFS-DENY] line.
        dos_svc_set_overlay(&t->svc, 0, 0);
        if (t->svc.homedir[0]) {
            char tail[64];
            if (dosovl_title_rs((const unsigned char *)t->svc.appdir,
                                (unsigned char *)tail, (int)sizeof(tail))) {
                char ovl[128];
                int on = 0;
                for (; t->svc.homedir[on] && on < (int)sizeof(ovl) - 2; on++)
                    ovl[on] = t->svc.homedir[on];
                if (on > 0 && ovl[on - 1] != '/') ovl[on++] = '/';
                for (int i = 0; tail[i] && on < (int)sizeof(ovl) - 1; i++) ovl[on++] = tail[i];
                ovl[on] = '\0';
                if (dos_overlay_prepare(t->svc.appdir, ovl, uid, gid) == 0) {
                    dos_svc_set_overlay(&t->svc, t->svc.appdir, ovl);
                    kprintf("[dos] #rawrite write overlay ARMED: %s -> %s (uid=%u gid=%u)\n",
                            t->svc.appdir, ovl, uid, gid);
                }
            }
        }
    }
}


// #rawrite: rustkern/dosovl.rs registry lookup. Declared here rather than in a
// private extern block for the #742 reason: see dos/dospath.h and int21svc.h,
// which own the other two halves of this interface.
// ---- (#740) VESA VRAM lifecycle ------------------------------------------
//
// Guest video memory is kmalloc'd from the mode being set and freed on the way
// out, deliberately NOT a fixed array in the file-scope `static dos_task_t
// g_dos` the way ega_plane[4][0x10000] is. That pattern already puts 256 KB in
// kernel BSS for every boot whether a DOS guest runs or not; a VESA VRAM big
// enough for 1024x768 would add another megabyte on the same terms.
static void vbe_free_vram(dos_task_t *t) {
    if (t->vbe_vram) { kfree(t->vbe_vram); t->vbe_vram = NULL; }
    t->vbe.vram = 0;
}

// Leave VBE and go back to the plain VGA paths. Called from the standard
// INT 10h AH=00h set-mode too, so a program that ends with `mov ax,3 ; int 10h`
// really does get its text screen back rather than a stale VESA picture.
static void vbe_leave(dos_task_t *t) {
    if (!t->vbe.mode && !t->vbe_vram) return;
    t->vbe.mode = 0; t->vbe.bank = 0; t->vbe.disp_start = 0;
    t->vbe.width = t->vbe.height = 0; t->vbe.bpl = 0; t->vbe.bpp = 0;
    // dac8 deliberately survives: the VBE spec makes 4F08h persist across mode
    // sets, and the DAC is hardware state, not mode state.
    vbe_free_vram(t);
}

// ---- INT 10h (VGA BIOS) --------------------------------------------------
// #163: THE IBM BIOS CRTC TABLES FOR THE PLANAR MODES, AND WHY SEEDING THEM IS
// A FIX AND NOT TIDINESS.
//
// dos_task_t::crtc is memset to zero and port 0x3D5 READS BACK OUT OF IT
// (dos_in), so before a program writes a register the guest reads ZERO where
// real hardware returns the BIOS value. The mode-13h arm was given a full seed
// by #740; the PLANAR arm still seeded three bytes (0x18, and single bits of
// 0x07 and 0x09), chosen because they were the three the present path read.
//
// That is a lie told to the guest, and the guests it hurts are the careful
// ones: the documented way to change one CRTC field is READ-MODIFY-WRITE, and
// an RMW against a zero readback writes back zero for every field the program
// did not mean to touch.
//
// MEASURED, The Incredible Machine, build 1929 (mode 12h): it RMWs the Overflow
// register to set Line Compare bit 8 and wrote back 0x18 where real hardware
// would have produced 0x3E, clearing Vertical Display End bit 8 and the
// Vertical Total/Retrace bits in the same store. With the table seeded, build
// 1930 measured the SAME program writing NO 0x07/0x09/0x18 at all and using
// Start Vertical Blank instead: i.e. the split screen this ticket was chasing
// in TIM was itself an artefact of the readback, not something the game asked
// for. A wrong readback does not just lose information, it invents behaviour.
//
// The three bytes this replaces are all present in the tables with the same
// values, and dos_vga_rows_selftest_rs() checks that each table reproduces its
// mode's nominal height, so nothing that already worked changes.
//
// Indices 0x00-0x18, from the IBM VGA BIOS.
static const uint8_t crtc_mode_0d[25] = {   // 320x200x16 planar
    0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x9C,0x8E,0x8F,0x14,0x00,0x96,0xB9,0xE3,0xFF };
static const uint8_t crtc_mode_0e[25] = {   // 640x200x16 planar
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x00,0x96,0xB9,0xE3,0xFF };
static const uint8_t crtc_mode_10[25] = {   // 640x350x16 planar
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x40,0x00,0x00,0x00,
    0x00,0x00,0x00,0x83,0x85,0x5D,0x28,0x0F,0x63,0xBA,0xE3,0xFF };
static const uint8_t crtc_mode_12[25] = {   // 640x480x16 planar
    0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,0x00,0x40,0x00,0x00,0x00,
    0x00,0x00,0x00,0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF };

static void dos_crtc_seed_planar(dos_task_t *t, const uint8_t *tab) {
    for (int i = 0; i < 25; i++) t->crtc[i] = tab[i];
    t->atc_reg[0x13] = 0;      // no pixel panning
}

// (#740) CHAIN-4 REGISTER TRACE. The chained-vs-Mode-X question must be
// answered by WATCHING THE BIT, not by inferring it from which presenter ran.
// Sequencer register 4 bit 3 SET = chained (plain mode 13h); CLEAR = unchained
// (Mode X). Bounded, and only on a CHANGE, so a title that never unchains costs
// one line at its mode set and nothing afterwards.
//
// WHY AN INFERENCE WAS NOT ENOUGH: #740 read "no Mode X line appeared after the
// mode set" as evidence that the present path chose the chained route. It had
// chosen the chained route, and correctly, but that log line could not have
// shown it either way - dos_geom_note()'s dedupe key had no video_mode in it,
// so the mode-13h geometry was suppressed as a duplicate of the mode-0Dh one it
// had just replaced. An absent log line is not a measurement.
//
// THE TRAP THIS ALSO WATCHES FOR (blame.md): dos_task_t is memset(0), so an
// UNSEEDED seq_reg[4] reads 0x00, which has Chain-4 CLEAR and therefore claims
// UNCHAINED on a fresh guest, inverting the obvious one-line test. The mode-set
// seeding writes 0x0E; this trace is how you see on the serial log that it did.
static void dos_seq4_note(dos_task_t *t, const char *where) {
    static int n = 0;
    static int last = -1;
    int c4 = (t->seq_reg[4] & 0x08) ? 1 : 0;
    if (c4 == last || n >= 16) return;
    last = c4; n++;
    kprintf("[dos] #740 SEQ4=%02x Chain-4=%s -> %s (%s)\n",
            t->seq_reg[4], c4 ? "SET" : "CLEAR",
            c4 ? "CHAINED mode 13h" : "UNCHAINED Mode X", where);
}

static void int10(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    if (g_dos_trace21)
        kprintf("[dos] INT10 AH=%02x al=%02x bx=%04x cx=%04x dx=%04x cs:ip=%04x:%04x\n",
                ah, AL(c), c->bx, c->cx, c->dx, c->cs, c->ip);
    switch (ah) {
    case 0x00: {  // set video mode AL
        uint8_t m = AL(c) & 0x7F;   // bit7 = "don't clear memory"
        // (#740) A standard mode set ends any VESA mode. Without this a game
        // that restores mode 3 on exit would keep presenting from freed-shaped
        // VESA state and never show its text screen again.
        vbe_leave(t);
        t->video_mode = m;
        kprintf("[dos] INT 10h set mode 0x%02x\n", m);
        if (m == 0x13) {
            t->gfx_w = MODE13_W; t->gfx_h = MODE13_H;
            for (int i = 0; i < MODE13_W * MODE13_H; i++)
                t->mem[VGA_A000 + i] = 0;
            // #740: dos_present_chain4() now honours CRTC start address/
            // Offset/Line Compare via the same dos_vga_decode_geom() the EGA
            // path uses, where it previously ignored them (hardcoded 320
            // bytes/row at offset 0). Reset the same "power-on/mode-set"
            // Line Compare state the EGA branch below already resets, so an
            // UNPROGRAMMED register reads as a real BIOS leaves it (no
            // split) instead of whatever a PRIOR mode left there (or,
            // before this fix, simply going unread). Offset (0x13) and
            // start address (0x0C/0x0D) are deliberately left at their
            // memset-zero default: the decode falls back to the historical
            // 320-byte/offset-0 behaviour whenever they read as
            // unprogrammed, so a title that never touches these registers
            // renders byte-identically to before this change.
            t->crtc[0x18] = 0xFF;
            t->crtc[0x07] |= 0x10;
            t->crtc[0x09] |= 0x40;
            // (#740) SEED THE REST OF THE STATE A REAL BIOS LEAVES BEHIND.
            //
            // Mode X is reached by TWEAKING the BIOS mode 13h tables: a game
            // writes the six or seven registers that differ and leaves the rest
            // alone. With the BIOS values absent, the derived resolution is
            // nonsense built from zeroes - and, far worse, seq_reg[4] read back
            // as 0x00, which has Chain-4 CLEAR, i.e. UNCHAINED. The obvious
            // one-line Chain-4 test would then have sent every plain mode 13h
            // game (i.e. every 256-colour DOS title that works today) down the
            // brand-new Mode X path. See blame.md.
            //
            // DELIBERATELY OMITTED, and this is load-bearing: CRTC 0x13
            // (Offset) and 0x0C/0x0D (start address). dos_present_chain4()
            // treats stride_bytes as the LINEAR row length and needs 320, which
            // it gets from the default_stride_bytes fallback precisely because
            // 0x13 reads as unprogrammed; the real BIOS value there is 0x28
            // (40 words = 80 bytes), which is right for the four-plane layout
            // and four times too small for the chained view. Seeding it would
            // have quietly broken every mode 13h game. Mode X gets the same 80
            // from ITS fallback (width/4), and a 360-wide title that programs
            // Offset for real is honoured either way.
            // (#740) A REAL BIOS MODE SET WRITES ALL 25 CRTC REGISTERS. This
            // used to write a subset and inherit the rest from whatever mode
            // ran before, and the three it left alone - 0x0C/0x0D (start
            // address) and 0x13 (Offset) - are exactly the three whose ZERO
            // value dos_vga_decode_geom() reads as the "unprogrammed, use the
            // mode default" sentinel. A stale non-zero value silently disables
            // the fallback.
            //
            // MEASURED, not theoretical: Aladdin sets mode 0Dh and then mode
            // 13h. crtc_mode_0d[0x13] = 0x14 (40 bytes, right for 320x200x16
            // planar) survived into mode 13h, so the chained presenter read the
            // framebuffer at 40 bytes per row instead of 320 and drew EIGHT
            // identical vertical columns (320 / 40 = 8). The #740 screendump
            // has a column-energy autocorrelation peak at exactly 40 source
            // pixels. The game was drawing correctly the whole time.
            //
            // dos_crtc_seed_mode13_rs() writes the full register file including
            // the sentinels, so the mode set no longer depends on its
            // predecessor. Its self-test replays 0Dh-then-13h and fails on any
            // stride but 320. See rustkern/doscrtc.rs.
            dos_crtc_seed_mode13_rs(t->crtc, (uint32_t)sizeof(t->crtc));
            dos_seq4_note(t, "INT 10h mode 13h set");
            t->seq_reg[0] = 0x03; t->seq_reg[1] = 0x01; t->seq_reg[2] = 0x0F;
            t->seq_reg[3] = 0x00; t->seq_reg[4] = 0x0E;   // bit3 = Chain-4 ON
            t->seq_map_mask = 0x0F;
            t->gc_set_reset = t->gc_en_set_reset = 0;
            t->gc_data_rotate = 0; t->gc_read_map = 0; t->gc_mode = 0x40;
            t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
            t->atc_reg[0x10] = 0x41;   // 256-colour + graphics
            t->atc_reg[0x13] = 0x00;   // no pixel pan
            t->misc_out = 0x63;
            // A real mode set clears ALL of video memory. Without this, a Mode X
            // game that unchains and draws without clearing first would see the
            // previous program's planes.
            for (int p = 0; p < 4; p++) memset(t->ega_plane[p], 0, EGA_PLANE_SIZE);
            t->ega_dirty = 1;
        } else if (m <= 0x03 || m == 0x07) {
            // Text modes. AL bit 7 (masked off above) means "do not clear".
            t->gfx_w = 0; t->gfx_h = 0;
            t->text_attr = 0x07;
            // (#234b) A mode set returns to page 0 and, on real hardware, clears
            // the WHOLE text buffer, not just the page that happened to be
            // active. Clearing one page left the others holding the previous
            // program's screen, which a page-flipping guest would then show.
            t->text_page = 0;
            for (int p = 0; p < DOS_TEXT_PAGES; p++) t->pg_row[p] = t->pg_col[p] = 0;
            wr8 (t, 0x0040, 0x0062, 0);
            wr16(t, 0x0040, 0x004E, 0);
            if (!(AL(c) & 0x80)) {
                for (uint32_t i = 0; i < DOS_TEXT_PAGES * DOS_TEXT_PAGE_BYTES; i += 2) {
                    t->mem[VGA_B800 + i]     = ' ';
                    t->mem[VGA_B800 + i + 1] = 0x07;
                }
                t->cur_row = t->cur_col = 0;
                dos_text_sync_bda(t);
            }
        } else if (m == 0x04 || m == 0x05 || m == 0x06) {
            // (#212) CGA GRAPHICS. Before this arm existed these three modes
            // set video_mode and nothing else: gfx_w/gfx_h kept whatever the
            // PREVIOUS mode left, and dos_present_inner() had no arm for them
            // at all, so the window was never written and stayed the
            // compositor's grey. That is #212 (Joust opens with AX=0004).
            //
            // 06h is 640x200 one bit per pixel; 04h and 05h are 320x200 two
            // bits per pixel. Both are 80 bytes per row in the 16 KB aperture
            // at B800:0000, which is where the CLEAR below has to reach: the
            // mode 13h arm clears 0xA0000 and the planar arm clears the four
            // plane buffers, and neither of those is CGA memory.
            if (m == 0x06) { t->gfx_w = 640; t->gfx_h = 200; }
            else           { t->gfx_w = 320; t->gfx_h = 200; }
            // A real BIOS mode set reloads the Color Select Register. Doing
            // this here and not in the presenter means a title that never
            // touches 0x3D9 (most of them) gets the hardware default rather
            // than whatever the last title selected.
            t->cga_pal = CGA_PAL_RESET;
            if (!(AL(c) & 0x80))
                memset(&t->mem[CGA_B800], 0, CGA_APERTURE);
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
            // #163: the FULL BIOS CRTC table, not just the three bytes the
            // present path happened to read (0x18 plus single bits of 0x07 and
            // 0x09, which are all present in these tables with the same
            // values). The register file is what port 0x3D5 reads back, and an
            // RMW against a zeroed readback destroys every field the program did
            // not mean to touch. See the note above crtc_mode_0d.
            //
            // Unlike the mode 13h seed, the Offset register (0x13) and the start
            // address (0x0C/0x0D) ARE seeded here: the planar presenter treats
            // stride as bytes-per-plane-row, which is exactly what the BIOS
            // Offset means in a planar mode, and each table's value equals the
            // W/8 fallback the presenter already used (0x14*2 = 40 for 0Dh,
            // 0x28*2 = 80 for the 640-wide modes). Mode 13h's chained presenter
            // needs a stride four times larger than its BIOS Offset, which is
            // why #740 deliberately left 0x13 unseeded THERE and why that
            // reasoning does not carry over to here.
            dos_crtc_seed_planar(t, m == 0x0D ? crtc_mode_0d :
                                    m == 0x0E ? crtc_mode_0e :
                                    m == 0x10 ? crtc_mode_10 : crtc_mode_12);
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
    {   // (#234b) BH selects the PAGE. Writing another page's cursor must not
        // move the one we are displaying.
        uint8_t pg = (uint8_t)((c->bx >> 8) & 7);
        uint8_t rr = (uint8_t)((c->dx >> 8) & 0xFF);
        uint8_t cc = (uint8_t)(c->dx & 0xFF);
        if (rr > TEXT_ROWS - 1) rr = TEXT_ROWS - 1;
        if (cc > TEXT_COLS - 1) cc = TEXT_COLS - 1;
        if (pg == t->text_page) { t->cur_row = rr; t->cur_col = cc; dos_text_sync_bda(t); }
        else                    { t->pg_row[pg] = rr; t->pg_col[pg] = cc; }
        break;
    }
    case 0x03: {  // get cursor -> DH=row DL=col, CX=shape  (BH = page)
        uint8_t pg = (uint8_t)((c->bx >> 8) & 7);
        uint8_t rr = (pg == t->text_page) ? t->cur_row : t->pg_row[pg];
        uint8_t cc = (pg == t->text_page) ? t->cur_col : t->pg_col[pg];
        c->dx = (uint16_t)((rr << 8) | cc);
        c->cx = rd16(t, 0x0040, 0x0060);
        break;
    }
    case 0x05: {  // (#234b) select active display page (AL = page)
        // Was an empty body under the comment "we only implement page 0", and
        // that was the whole of Epyx Rogue's blank screen: it draws on page 3.
        uint8_t pg = (uint8_t)(AL(c) & 7);
        if (pg != t->text_page) {
            t->pg_row[t->text_page] = t->cur_row;
            t->pg_col[t->text_page] = t->cur_col;
            t->text_page = pg;
            t->cur_row = t->pg_row[pg];
            t->cur_col = t->pg_col[pg];
            dos_text_sync_bda(t);
            // The BDA fields a program reads to find the page itself. Both, or
            // a guest that trusts 0040:004E lands on the page we are not
            // showing, which is the bug this fixes wearing a different hat.
            wr8 (t, 0x0040, 0x0062, pg);
            wr16(t, 0x0040, 0x004E,
                 (uint16_t)((uint32_t)pg * DOS_TEXT_PAGE_BYTES));
            kprintf("[dos] #234b INT 10h AH=05: active display page -> %u\n", pg);
        }
        break;
    }
    case 0x06:  // scroll window up   AL=lines CH/CL=top/left DH/DL=bot/right BH=attr
    case 0x07:  // scroll window down
        dos_text_scroll(t, (c->cx >> 8) & 0xFF, c->cx & 0xFF,
                        (c->dx >> 8) & 0xFF, c->dx & 0xFF,
                        AL(c), (uint8_t)((c->bx >> 8) & 0xFF), ah == 0x07);
        break;
    case 0x08: { // read char+attr at cursor -> AL=char AH=attr
        uint32_t o = dos_text_cell_pg((c->bx >> 8) & 7, t->cur_row, t->cur_col);
        AL_SET(c, t->mem[o]);
        AH_SET(c, t->mem[o + 1]);
        break;
    }
    case 0x09:   // write char+attr CX times at cursor (cursor does NOT advance)
    case 0x0A: { // write char CX times, keep the existing attribute
        uint16_t n = c->cx ? c->cx : 1;
        for (uint16_t i = 0; i < n; i++) {
            int col = t->cur_col + i, row = t->cur_row;
            int wpg = (c->bx >> 8) & 7;
            while (col >= TEXT_COLS) { col -= TEXT_COLS; row++; }
            if (row > TEXT_ROWS - 1) break;
            uint32_t o = dos_text_cell_pg(wpg, row, col);
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
    case 0x0B:  // (#212) set CGA background/border colour or palette
        // BH=00: BL = background/border, which in the 320x200 modes is also
        //        colour 0, and in 640x200 is the FOREGROUND. Bits 0-4.
        // BH=01: BL bit 0 selects the colour triple (0 = green/red/brown,
        //        1 = cyan/magenta/white).
        // Both land in the SAME hardware register a guest could equally have
        // written with an OUT to 0x3D9, which is why there is one field.
        if (((c->bx >> 8) & 0xFF) == 0x00)
            t->cga_pal = (uint8_t)((t->cga_pal & 0xE0) | (c->bx & 0x1F));
        else if (((c->bx >> 8) & 0xFF) == 0x01)
            t->cga_pal = (uint8_t)((t->cga_pal & ~0x20) | ((c->bx & 1) ? 0x20 : 0));
        break;
    case 0x0F:  // get video mode -> AL=mode, AH=cols, BH=active page
        AL_SET(c, t->video_mode);
        AH_SET(c, dos_text_is(t) ? TEXT_COLS : 40);
        c->bx = (uint16_t)(c->bx & 0x00FF);
        break;

    case 0x4F: {  // (#740) VESA BIOS Extensions
        vbe_call_t vc;
        vc.ax = c->ax; vc.bx = c->bx; vc.cx = c->cx; vc.dx = c->dx;
        vc.buf = NULL; vc.buflen = 0;
        vc.pal = &t->pal[0][0];
        vc.action = VBE_ACT_NONE; vc.miss = 0;
        // Resolve ES:DI ONCE, HERE, and bounds-check it ONCE, so that no
        // subfunction over in the Rust can reach outside the guest's megabyte
        // however it is called or however it is later extended. 4F09h's table
        // is 4 bytes per palette entry; every other buffer-taking subfunction
        // takes a 256-byte block.
        {
            uint32_t need = 256;
            if (AL(c) == 0x09) need = (uint32_t)c->cx * 4u;
            uint32_t lin = (((uint32_t)c->es << 4) + c->di) & 0xFFFFF;
            if (need > 0 && (uint64_t)lin + need <= (uint64_t)DOS_MEM_SIZE) {
                vc.buf = &t->mem[lin];
                vc.buflen = need;
            }
        }
        vbe_dispatch_rs(&t->vbe, &vc);
        c->ax = vc.ax; c->bx = vc.bx; c->cx = vc.cx; c->dx = vc.dx;

        if (vc.miss) {
            // Log a MISS exactly ONCE per subfunction. The register effect has
            // already been applied by the dispatcher (AX = 0x014F, the spec's
            // explicit failure) - this is a breadcrumb, never the response.
            // blame.md: an interpreter that logs and returns with the registers
            // untouched leaves the guest reading whatever was in AX, and a
            // program that tests only AL == 0x4F then believes it succeeded.
            uint32_t bit = 1u << (AL(c) & 0x1F);
            if (!(t->vbe_missed & bit)) {
                t->vbe_missed |= bit;
                kprintf("[dos] VBE MISS ax=%04x bx=%04x cx=%04x (code %08x) -> AX=%04x\n",
                        c->ax, vc.bx, vc.cx, vc.miss, vc.ax);
            }
        }

        if (vc.action == VBE_ACT_SET_VGA) {
            // 4F02h with a mode below 0x100 is a legal way to ask for a plain
            // VGA mode, and programs use it to restore mode 3 on exit. Route it
            // through the SAME handler rather than duplicating the mode-set
            // logic: recurse into this function with AH=00h.
            uint16_t save = c->ax;
            c->ax = (uint16_t)(vc.bx & 0x00FF);
            int10(t);
            c->ax = save;
        } else if (vc.action == VBE_ACT_SET_MODE) {
            uint32_t need = t->vbe.bpl * (uint32_t)t->vbe.height;
            uint32_t want = vbe_vram_bytes_rs();
            if (need > want) want = need;
            if (!t->vbe_vram || t->vbe.vram < want) {
                vbe_free_vram(t);
                t->vbe_vram = (uint8_t *)kmalloc(want);
                if (!t->vbe_vram) {
                    // Out of memory is a REAL failure and the guest is told so,
                    // rather than being given a mode with nowhere to draw.
                    kprintf("[dos] VBE mode %03x: kmalloc(%u) failed\n",
                            t->vbe.mode, want);
                    t->vbe.mode = 0;
                    c->ax = 0x014F;
                    break;
                }
                t->vbe.vram = want;
            }
            // BX bit 15 = "do not clear video memory".
            if (!(vc.bx & 0x8000)) memset(t->vbe_vram, 0, t->vbe.vram);
            t->video_mode = 0x13;          // graphics, not text: keeps dos_text_is() false
            t->gfx_w = t->vbe.width; t->gfx_h = t->vbe.height;
            t->ega_dirty = 1;
            kprintf("[dos] VBE set mode %03x = %ux%ux%u bpl=%u vram=%u\n",
                    t->vbe.mode, t->vbe.width, t->vbe.height, t->vbe.bpp,
                    t->vbe.bpl, t->vbe.vram);
        }
        break;
    }

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
//
// #163. The bridge below this (dos_pump_input) has always mapped the host
// cursor into the guest's virtual coordinate space; what was thin was the API
// on top of it. int33() implemented 00/01/02/03/04/07/08/0B and NOTHING else,
// and the omissions are not exotic:
//
//   05h / 06h  button PRESS and RELEASE counters. A great many games of this
//              era never poll 03h for a click at all: they ask "how many times
//              has button N been pressed since I last asked", because that
//              cannot miss a click between two polls. With 05h/06h unhandled
//              the registers came back holding whatever the caller passed in,
//              so the count read as the button number and the coordinates as
//              garbage. That is not a sluggish mouse, it is a dead one.
//   0Ch / 14h  install / exchange the user event handler. A program that
//              installs a callback and then waits for it waits forever.
//   0Fh / 1Ah  mickey ratio and sensitivity, which a program may set before it
//              trusts 0Bh.
//   21h / 24h / 26h  software reset, driver version, maximum coordinates.
//
// THE COUNTERS ARE EDGE STATE. They cannot be derived at call time from the
// current button mask, which is why the fix is not local to this function: the
// edges are latched in dos_pump_input(), the one place that sees every button
// transition.
//
// WHAT WE DELIBERATELY DO NOT DO: draw a guest mouse cursor. The compositor
// already draws the host cursor over the DOS window, so 09h/0Ah (define cursor
// shape) and 10h (screen exclusion area) are accepted and have no effect. That
// is a stated limitation, not an oversight: two cursors would be worse than one.

// Mouse event bits, as passed to a 0Ch handler in AX.
#define M_EV_MOVE      0x0001
#define M_EV_LPRESS    0x0002
#define M_EV_LRELEASE  0x0004
#define M_EV_RPRESS    0x0008
#define M_EV_RRELEASE  0x0010

static void dos_mouse_clamp(dos_task_t *t) {
    // Guarded rather than unconditional: a task whose range has never been set
    // has min == max == 0, and clamping to that would pin the cursor to the
    // top-left corner. dos_run_file seeds a real range, so this is a belt.
    if (t->mmax_x > t->mmin_x) {
        if (t->mx < t->mmin_x) t->mx = t->mmin_x;
        if (t->mx > t->mmax_x) t->mx = t->mmax_x;
    }
    if (t->mmax_y > t->mmin_y) {
        if (t->my < t->mmin_y) t->my = t->mmin_y;
        if (t->my > t->mmax_y) t->my = t->mmax_y;
    }
}

static void dos_mouse_defaults(dos_task_t *t) {
    t->mouse_hide_count = 1;              // a reset leaves the cursor HIDDEN
    t->mouse_on = 0;
    t->mmin_x = 0; t->mmax_x = MODE13_W * 2 - 1;   // the 640x200 virtual screen
    t->mmin_y = 0; t->mmax_y = MODE13_H - 1;
    t->mratio_x = 8; t->mratio_y = 16;    // the documented driver defaults
    // (#mickey) The counter model resets with the rest of the driver, but the
    // two POLICY fields come from /CONFIG/DOSMOUSE.CFG and are re-applied
    // rather than reset: a guest calling 00h must not silently turn a
    // deliberate setting back into the built-in default.
    dos_mick_reset_rs(&t->mick);
    if (g_dos_mick_home_every != DOS_MICK_HOME_UNSET)
        t->mick.home_every = g_dos_mick_home_every;
    t->mick.gain_ratio = g_dos_mick_gain_ratio;
    t->mick_si = 0; t->mick_di = 0;
    t->mev_seg = 0; t->mev_off = 0; t->mev_mask = 0; t->mev_pending = 0;
    t->mev_rm = 0;
    t->mev_pm = 0; t->mev_pm_sel = 0; t->mev_pm_off = 0; t->mev_pm_lin = 0;
    for (int i = 0; i < 3; i++) {
        t->mpress_n[i] = 0; t->mrel_n[i] = 0;
        t->mpress_x[i] = 0; t->mpress_y[i] = 0;
        t->mrel_x[i] = 0;   t->mrel_y[i] = 0;
    }
    t->mbtn_prev = 0;
}

// (raplay) MAY WE EXECUTE THE 0Ch HANDLER WE HAVE JUST BEEN GIVEN?
//
// dos_mouse_events() delivers the handler by pointing the 16-bit interpreter at
// a real-mode (seg, off). That is correct for a 16-bit guest and it is correct
// for a 32-bit guest whose install arrived through DPMI 0300h, because a
// simulated real-mode interrupt carries real-mode register VALUES by
// definition. It is NOT correct for a native protected-mode INT 33h from a
// 32-bit client, whose ES is a selector: seg*16+off there is an address the
// client never wrote, and running it would execute arbitrary arena bytes.
//
// MEASURED, and this is why the distinction is worth making rather than
// declining both. Red Alert (Rational DOS/4G, GAME.DAT) issues NO direct
// protected-mode INT 33h at all: all seven of its mouse calls are DPMI 0300h
// with BL=33h, and the two 0Ch installs write ES from a DPMI-0100 DOS block's
// segment (`mov 0x1ac8b,%edx ; shr $4,%edx ; mov %dx,<RMCS+0x22>`) with mask
// 0x1F. It never calls 03h, the poll, so a polled-only driver leaves it with a
// cursor that cannot move and a menu that cannot be clicked.
static int dos_mev_route_is_realmode(dos_task_t *t) {
    if (!t->mev_seg && !t->mev_off) return 0;   // uninstall: nothing to arm
    if (!t->le_active) return 1;                // 16-bit guest: always real mode
    if (!t->rm_reflect) return 0;               // native PM INT 33h: ES is a selector
    // A reflected frame's ES:DX must still land inside the low megabyte AND
    // inside the arena we actually allocated. Checked rather than assumed: the
    // RMCS is guest-writable, so this is a Ring-3-reachable value.
    uint32_t flat = ((uint32_t)t->mev_seg << 4) + (uint32_t)t->mev_off;
    if (flat >= 0x00100000u) return 0;
    if (t->le_arena_size && flat >= t->le_arena_size) return 0;
    return 1;
}

// (#740 digplay) MAY WE FAR-CALL THE HANDLER A 32-BIT CLIENT JUST GAVE US,
// AND WHERE IS IT?
//
// dos_mev_route_is_realmode() answers the first half for the DPMI-0300h route.
// This answers it for the OTHER one, the native protected-mode INT 33h that
// route deliberately declines, and it answers it by RESOLVING rather than by
// assuming: the handler is ES:EDX in the client's own descriptor space, so the
// address is the base of the descriptor its ES names plus the FULL 32-bit EDX.
//
// Two things the 16-bit path's (seg, off) pair cannot carry, both measured on
// The Dig:
//   * EDX is 32 bits. mev_off is 16, and the handler is at 0x0013F360, so the
//     truncated value 0xF360 is not merely imprecise, it is a different address
//     in a different part of the module.
//   * ES is a selector, not a paragraph. The Dig's is 0x000F, the FIRST
//     descriptor this host's DPMI 0000h hands out; its base is what turns EDX
//     into a linear address, and seg*16 is meaningless for it.
//
// The range checks are the same two dos4gw_deliver() makes for an asynchronous
// interrupt, for the same reasons: an address inside the vector table means the
// table has been overwritten, and one outside the arena is not a protected-mode
// handler at all. Refusing here, where the value is still printable, is how a
// bad pointer stays a diagnosed refusal instead of a derail thousands of
// instructions later.
// (#740/#mickey) ONE HEX GUEST-LINEAR ADDRESS FROM A CONFIG FILE, OR 0.
// Both diagnostic probe windows want exactly this, and two copies of the same
// six-line parser is the shape this tree's reuse rule exists to stop.
static uint32_t dos_cfg_hex_addr(const char *path) {
    uint32_t sz = 0;
    void *buf = fat_read_file(&g_fat_fs, path, &sz);
    if (!buf) return 0;
    const char *s = (const char *)buf;
    uint32_t v = 0, i = 0;
    if (sz > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    for (; i < sz; i++) {
        char ch = s[i];
        if (ch >= '0' && ch <= '9') v = (v << 4) | (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v = (v << 4) | (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v = (v << 4) | (uint32_t)(ch - 'A' + 10);
        else break;
    }
    kfree(buf);
    return v;
}

static void dos4gw_mev_pm_arm(dos_task_t *t) {
    t->mev_pm = 0; t->mev_pm_sel = 0; t->mev_pm_off = 0; t->mev_pm_lin = 0;
    if (!t->le_active || t->rm_reflect) return;
    uint16_t sel = t->le_cpu.seg[X32_ES];
    uint32_t off = t->le_cpu.regs[X32_EDX];
    if (!sel && !off) return;                       // uninstall: nothing to arm
    uint32_t lin = t->le_cpu.seg_base[X32_ES] + off;
    if (lin < 0x400u || (t->le_arena_size && lin >= t->le_arena_size)) {
        kprintf("[4GW] INT 33h event handler NOT armed: ES:EDX = %04x:%08x -> "
                "linear 0x%08x (ES base 0x%08x), which is %s. The guest is left "
                "with a mouse that reports position but never calls back.\n",
                sel, off, lin, t->le_cpu.seg_base[X32_ES],
                lin < 0x400u ? "inside the interrupt vector table"
                             : "outside the arena");
        return;
    }
    t->mev_pm_sel = sel; t->mev_pm_off = off; t->mev_pm_lin = lin;
    t->mev_pm = 1;
    // (#740 digplay) DOSDIAG probe window. /CONFIG/DOSMEVA.CFG holds one hex
    // guest-linear address; the first upcalls dump 48 bytes there. It exists
    // because "the handler ran and returned" and "the handler's stores reached
    // the variables its own polling loop reads" are DIFFERENT claims, and only
    // the second one is the feature.
    t->mev_pm_dbg = dos_cfg_hex_addr("/CONFIG/DOSMEVA.CFG");
    if (t->mev_pm_dbg)
        kprintf("[4GW] mev probe window armed at guest linear 0x%08x "
                "(/CONFIG/DOSMEVA.CFG)\n", t->mev_pm_dbg);
    // (#mickey) THE AIM INSTRUMENT. /CONFIG/DOSMPTR.CFG holds one hex guest
    // linear address: the guest's OWN pointer pair, two 16-bit words. It is the
    // only way to see the defect this ticket is about. A guest that draws no
    // cursor of its own (The Dig does not; the arrow over the window is the
    // compositor's) keeps its pointer entirely private, so "the picture
    // changed" cannot say WHERE the guest believes it is pointing, and a
    // percentage of changed pixels is not evidence that a click landed on the
    // control the user aimed at.
    t->mtrack_addr = dos_cfg_hex_addr("/CONFIG/DOSMPTR.CFG");
    t->mtrack_n = 0;
    if (t->mtrack_addr)
        kprintf("[dos] (#mickey) mtrack armed at guest linear 0x%08x: the "
                "guest's own pointer pair (/CONFIG/DOSMPTR.CFG)\n",
                t->mtrack_addr);
}

static void int33(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint16_t fn = c->ax;
    // #163 instrument: which mouse functions does this program actually use,
    // and does it call the driver at all? One line per DISTINCT function, so it
    // answers the question without becoming the 46,390-line flood that an
    // unbounded INT 33h log once was (see x86_16.h's #736 note).
    //
    // UNCONDITIONAL, deliberately not behind g_x86_dbgring. This one line per
    // function is what turned #163 from a guess into a measurement: it showed
    // The Incredible Machine calling 00/01/02/04/07/08/0F/0C and NEVER 03,
    // i.e. relying entirely on the user event handler that was not implemented.
    // A diagnostic that needs a config file on the right partition to fire is
    // one that is silent exactly when someone is trying to diagnose something,
    // and a silent log reads as "this program does nothing interesting".
    if (fn < 64) t->mcall_n[fn]++; else t->mcall_other++;
    if (((fn < 64) ? t->mcall_n[fn] : t->mcall_other) == 1)
        kprintf("[dos] #163 INT 33h AX=%04x FIRST call (bx=%04x cx=%04x dx=%04x es=%04x)\n",
                fn, c->bx, c->cx, c->dx, c->es);

    switch (fn) {
    case 0x0000: // reset driver / installed?
        c->ax = 0xFFFF;   // installed
        c->bx = 2;        // 2 buttons
        t->mouse_initialized = 1;
        dos_mouse_defaults(t);
        t->mprev_x = t->mx; t->mprev_y = t->my;
        break;
    case 0x0001: // show cursor (decrements the hide counter)
        if (t->mouse_hide_count > 0) t->mouse_hide_count--;
        t->mouse_on = (t->mouse_hide_count == 0);
        break;
    case 0x0002: // hide cursor (increments the hide counter)
        t->mouse_hide_count++;
        t->mouse_on = 0;
        break;
    case 0x0003: // get position/buttons -> CX=x DX=y BX=buttons
        c->cx = (uint16_t)t->mx;
        c->dx = (uint16_t)t->my;
        c->bx = (uint16_t)t->mbtn;
        break;
    case 0x0004: // set position CX=x DX=y
        t->mx = (int)(int16_t)c->cx; t->my = (int)(int16_t)c->dx;
        dos_mouse_clamp(t);
        t->mprev_x = t->mx; t->mprev_y = t->my;
        break;
    case 0x0005: { // button PRESS info. In: BX=button. Out: AX=current mask,
                   // BX=press count since the last 05h for that button (and it
                   // is CLEARED by the read), CX/DX=position at the last press.
        int b = (int)(c->bx & 0xFFFF);
        if (b < 0 || b > 2) b = 0;
        c->ax = (uint16_t)t->mbtn;
        c->bx = t->mpress_n[b];
        c->cx = t->mpress_x[b];
        c->dx = t->mpress_y[b];
        t->mpress_n[b] = 0;
        break;
    }
    case 0x0006: { // button RELEASE info, same shape as 05h
        int b = (int)(c->bx & 0xFFFF);
        if (b < 0 || b > 2) b = 0;
        c->ax = (uint16_t)t->mbtn;
        c->bx = t->mrel_n[b];
        c->cx = t->mrel_x[b];
        c->dx = t->mrel_y[b];
        t->mrel_n[b] = 0;
        break;
    }
    case 0x0007: { // set horizontal range CX=min DX=max
        int lo = (int)(int16_t)c->cx, hi = (int)(int16_t)c->dx;
        if (lo > hi) { int s = lo; lo = hi; hi = s; }
        t->mmin_x = lo; t->mmax_x = hi;
        dos_mouse_clamp(t);
        t->mprev_x = t->mx;
        // (#mickey) The counter is an affine function of the position IN THIS
        // RANGE, so moving the range moves it. Re-home rather than step.
        dos_mick_arm_home_rs(&t->mick);
        break;
    }
    case 0x0008: { // set vertical range CX=min DX=max
        int lo = (int)(int16_t)c->cx, hi = (int)(int16_t)c->dx;
        if (lo > hi) { int s = lo; lo = hi; hi = s; }
        t->mmin_y = lo; t->mmax_y = hi;
        dos_mouse_clamp(t);
        t->mprev_y = t->my;
        dos_mick_arm_home_rs(&t->mick);
        break;
    }
    case 0x0009: // define graphics cursor shape (ES:DX = mask pair)
    case 0x000A: // define text cursor
    case 0x0010: // define screen exclusion area
        // Accepted and ignored ON PURPOSE: the compositor draws the host cursor
        // over this window, so there is no guest cursor to shape or to hide
        // around a redraw. None of the three returns anything, so leaving the
        // registers untouched IS the documented effect.
        break;
    case 0x000B: { // read motion counters -> CX=dx DX=dy in mickeys, and CLEAR
        // (#mickey) ITS OWN ACCUMULATOR, not the one the 0Ch upcall carries.
        // On the shared-counter model a guest that polled 0Bh anywhere cleared
        // the counter its own event handler differences against, and the next
        // event handed that handler the whole travel since the last read as a
        // single jump. The two channels are now separate numbers.
        int32_t rx = 0, ry = 0;
        dos_mick_take_rel_rs(&t->mick, &rx, &ry);
        c->cx = (uint16_t)(int16_t)rx;
        c->dx = (uint16_t)(int16_t)ry;
        break;
    }
    case 0x000C: // install user event handler: CX=event mask, ES:DX=handler
        t->mev_mask = c->cx;
        t->mev_seg = c->es;
        t->mev_off = c->dx;
        t->mev_pending = 0;
        t->mev_rm = dos_mev_route_is_realmode(t);
        if (!t->mev_rm) dos4gw_mev_pm_arm(t);
        // (#mickey) A fresh handler has a STALE saved counter, or none, so the
        // first difference it computes is meaningless. Home before trusting it.
        dos_mick_arm_home_rs(&t->mick);
        kprintf("[dos] #163 INT 33h 0Ch install event handler %04x:%04x mask=%04x"
                " (%s)\n",
                c->es, c->dx, c->cx,
                t->mev_rm ? "real-mode: upcalls ARMED"
                          : t->mev_pm ? "protected-mode far call: upcalls ARMED"
                                      : "protected-mode: upcalls DECLINED");
        if (t->mev_pm)
            kprintf("[4GW] INT 33h 0Ch handler ES:EDX = %04x:%08x -> guest linear "
                    "0x%08x (ES base 0x%08x); delivered as a far CALL returning "
                    "to 0x%08x\n",
                    t->mev_pm_sel, t->mev_pm_off, t->mev_pm_lin,
                    t->le_cpu.seg_base[X32_ES],
                    (((uint32_t)0xF000u << 4) + DOS_MEVRET_STUB));
        break;
    case 0x0014: { // exchange user event handler: returns the OLD one
        uint16_t oseg = t->mev_seg, ooff = t->mev_off, omask = t->mev_mask;
        uint32_t opmoff = t->mev_pm_off;
        t->mev_mask = c->cx; t->mev_seg = c->es; t->mev_off = c->dx;
        t->mev_pending = 0;
        t->mev_rm = dos_mev_route_is_realmode(t);
        if (!t->mev_rm) dos4gw_mev_pm_arm(t);
        dos_mick_arm_home_rs(&t->mick);
        c->cx = omask; c->es = oseg; c->dx = ooff;
        // THE OLD OFFSET IS 32 BITS FOR A PROTECTED-MODE CLIENT, and the 16-bit
        // service frame cannot carry it back: dos4gw_int_post_rs writes only the
        // low half of each GPR. Write the full value into the guest's EDX here
        // and leave the low half matching in the frame, so the copy-back is a
        // no-op rather than a truncation. ES is NOT returned: nothing in the
        // 32-bit bridge marshals segment registers in either direction, and a
        // client exchanging its own handler passes the same selector back in.
        if (t->le_active && !t->rm_reflect) {
            t->le_cpu.regs[X32_EDX] = opmoff;
            c->dx = (uint16_t)opmoff;
        }
        kprintf("[dos] #163 INT 33h 14h exchange event handler -> %04x:%04x mask=%04x\n",
                t->mev_seg, t->mev_off, t->mev_mask);
        if (t->mev_pm)
            kprintf("[4GW] INT 33h 14h handler ES:EDX = %04x:%08x -> guest linear "
                    "0x%08x (ES base 0x%08x); delivered as a far CALL returning "
                    "to 0x%08x\n",
                    t->mev_pm_sel, t->mev_pm_off, t->mev_pm_lin,
                    t->le_cpu.seg_base[X32_ES],
                    (((uint32_t)0xF000u << 4) + DOS_MEVRET_STUB));
        break;
    }
    case 0x000F: // set mickeys per 8 pixels: CX=horizontal, DX=vertical
        if (c->cx) t->mratio_x = (int)c->cx;
        if (c->dx) t->mratio_y = (int)c->dx;
        break;
    case 0x0015: // get driver state storage size -> BX = bytes needed
        // Zero is the truthful answer: our driver state is not in guest memory,
        // so there is nothing for the program to save. 16h/17h then have
        // nothing to do, which is consistent rather than merely convenient.
        c->bx = 0;
        break;
    case 0x0016: // save driver state to ES:DX (BX bytes, and BX is 0)
    case 0x0017: // restore driver state from ES:DX
        break;
    case 0x001A: // set sensitivity: BX=horizontal, CX=vertical, DX=threshold
        if (c->bx) t->mratio_x = (int)c->bx;
        if (c->cx) t->mratio_y = (int)c->cx;
        break;
    case 0x001B: // get sensitivity -> BX/CX ratios, DX=double-speed threshold
        c->bx = (uint16_t)t->mratio_x;
        c->cx = (uint16_t)t->mratio_y;
        c->dx = 64;
        break;
    case 0x001C: // set interrupt rate (InPort mice only)
        // Accepted and ignored, for the same reason as 09h/0Ah/10h: there is no
        // hardware behind this driver whose report rate could be changed. The
        // Dig calls it, and answering "unimplemented" to a function that
        // returns nothing at all is noise, not honesty.
        break;
    case 0x001F: // disable driver -> AX=001Fh on success, ES:BX=old INT 33h vector
        c->ax = 0x001F;
        c->es = 0xF000; c->bx = DOS_INT33_STUB;
        break;
    case 0x0020: // enable driver
        break;
    case 0x0021: // software reset: like 00h but does not re-detect the hardware
        c->ax = 0xFFFF;
        c->bx = 2;
        dos_mouse_defaults(t);
        t->mprev_x = t->mx; t->mprev_y = t->my;
        break;
    case 0x0024: // get driver version/type -> BX=version BCD, CH=type, CL=IRQ
        c->bx = 0x0603;   // 6.03: the version essentially every game was tested against
        c->cx = 0x0400;   // CH=4 (PS/2 style), CL=0 (no IRQ line)
        break;
    case 0x0026: // get maximum virtual coordinates -> BX=disabled, CX/DX=max
        c->bx = 0;
        c->cx = (uint16_t)t->mmax_x;
        c->dx = (uint16_t)t->mmax_y;
        break;
    default:
        // Unimplemented. Registers are left EXACTLY as the caller set them,
        // which for an INT 33h function that returns values is the "nothing
        // happened" encoding a driver that does not support the call gives, and
        // never a fabricated success. Bounded so a program that spins on an
        // unknown function cannot flood the log (it has happened: 46,390 lines).
        {
            static int nmiss = 0;
            if (nmiss < 24) {
                nmiss++;
                kprintf("[dos] INT 33h AX=%04x UNIMPLEMENTED (bx=%04x cx=%04x dx=%04x)\n",
                        fn, c->bx, c->cx, c->dx);
            }
        }
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
    t->keyq_pushes++;   // (rakbd) evidence that a key reached the guest's ring
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
        // (rakbd) EVERY RAW BYTE ALSO GOES TO THE 8042 OUTPUT BUFFER, BEFORE
        // the BIOS-ring filtering below throws most of them away.
        //
        // This pump used to be the ONLY consumer of the raw ring for a guest
        // with no INT 9 handler, and it published its result in exactly one
        // place: the BIOS keyboard ring that INT 16h and a direct BDA read
        // look at. A guest that instead polls the 8042 - reads port 0x64 for
        // "data ready" and port 0x60 for the scancode - saw NOTHING, because
        // t->kbd_port60 is written only by dos_deliver_int9(), which by
        // definition does not run for a guest with no INT 9 handler. So the
        // byte at port 0x60 was the memset zero for the whole run.
        //
        // MEASURED on Red Alert: it installs no INT 09h handler by either the
        // low vector table or DPMI 0205h (the "[4GW] kbd ISR route: NONE" line
        // says so), reaches a live mission map, and ignores every key.
        //
        // The BREAK codes and the E0 prefix are kept here and dropped below on
        // purpose: the BIOS ring holds cooked make codes, and a raw poller
        // wants the byte stream the hardware would have produced, including
        // releases, or it cannot maintain a key-down table.
        uint8_t nx = (uint8_t)((t->p60_wr + 1u) % (uint8_t)sizeof t->p60_fifo);
        if (nx != t->p60_rd) {           // full: drop, exactly as the 8042 does
            t->p60_fifo[t->p60_wr] = b;
            t->p60_wr = nx;
        }
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
        // BLOCKING, which is what the BIOS documents AH=00h/10h as (#221). The
        // interpreter thread is still never blocked: with an empty queue this
        // writes NOTHING and raises svc.input_blocked, and the caller re-issues
        // the same INT on a later run-loop pass, after that loop has pumped
        // input. See the long note in dos/int21svc.h.
        //
        // MEASURED, NetHack (build 2010): its DEFAULTS.NH says
        // "OPTIONS=rawio,BIOS", so iflags.BIOS is set and tgetch() takes
        // BIOSgetch(), which is INT 16h AH=00h followed by AH=02h - NOT the
        // INT 21h AH=07h path. Answering AX=0 here made tty_nhgetch() return
        // ESC, and tty_askname() bails after ten of those.
        if (dos_keyq_pop(t, &k)) c->ax = k;
        else                     t->svc.input_blocked = 1;
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

// (#745) Marshal the interpreter's register file into the 20-byte window the
// Rust memory managers see, and back again. Only these ten words cross the FFI,
// so x86_16_cpu_t's layout is not part of the memory managers' ABI and cannot
// be broken by a change to the interpreter.
static void dos_regs_out(const x86_16_cpu_t *c, dos_regs_t *r) {
    r->ax = c->ax; r->bx = c->bx; r->cx = c->cx; r->dx = c->dx;
    r->si = c->si; r->di = c->di; r->ds = c->ds; r->es = c->es;
    r->flags = c->flags; r->pad = 0;
}
static void dos_regs_in(x86_16_cpu_t *c, const dos_regs_t *r) {
    c->ax = r->ax; c->bx = r->bx; c->cx = r->cx; c->dx = r->dx;
    c->si = r->si; c->di = r->di; c->ds = r->ds; c->es = r->es;
    c->flags = r->flags;
}

static uint16_t dos_pop16(dos_task_t *t) {
    uint16_t v = rd16(t, t->cpu.ss, t->cpu.sp);
    t->cpu.sp = (uint16_t)(t->cpu.sp + 2);
    return v;
}

// The XMS entry point, reached by a FAR CALL to DOS_XMS_SEG:anything. x86_16.c
// has already pushed the caller's CS:IP by the time the trap fires, so this
// must unwind that frame itself, exactly as the RETF the guest is expecting
// would. Returning 0 means "I have set cs:ip, resume there", which is what the
// pop below does; returning nonzero would stop the interpreter burst.
static int dos_xms_farcall(x86_16_cpu_t *c, uint16_t off) {
    dos_task_t *t = (dos_task_t *)c->owner;
    (void)off;
    if (t && t->xms_state) {
        dos_regs_t r;
        dos_regs_out(c, &r);
        dos_xms_dispatch_rs(t->xms_state, &r, t->mem);
        dos_regs_in(c, &r);
    }
    // Pascal-style return, whether or not the call was serviced: the stack
    // frame exists either way, and leaving it there would desynchronise every
    // subsequent return the guest makes. blame.md records that exact failure
    // for the Win16 layer's missing imports.
    c->ip = dos_pop16(t);
    c->cs = dos_pop16(t);
    return 0;
}

// Allocate both arenas and plant everything a guest uses to DISCOVER them.
// Called once, from dos_run_file, before the guest executes an instruction.
static void dos_mem_init(dos_task_t *t) {
    // ---- XMS ----
    uint32_t xsz = dos_xms_state_size_rs();
    void    *xst = kmalloc(xsz);
    uint8_t *xpl = (uint8_t *)kmalloc((uint64_t)DOS_XMS_POOL_KB * 1024);
    if (xst && xpl && dos_xms_init_rs(xst, xpl, DOS_XMS_POOL_KB) == 0) {
        t->xms_state = xst;
        t->xms_pool  = xpl;
        // A RETF at the advertised entry point (see the DOS_XMS_SEG comment).
        wr8(t, DOS_XMS_SEG, DOS_XMS_OFF, 0xCB);
        x86_16_set_farcall_trap(&t->cpu, DOS_XMS_SEG, dos_xms_farcall);
        kprintf("[xms] installed: %u KB extended memory, entry %04x:%04x\n",
                (unsigned)DOS_XMS_POOL_KB, DOS_XMS_SEG, DOS_XMS_OFF);
    } else {
        if (xst) kfree(xst);
        if (xpl) kfree(xpl);
        kprintf("[xms] arena kmalloc(%u KB) FAILED: reporting NOT INSTALLED, so the "
                "guest takes its own conventional-memory fallback\n",
                (unsigned)DOS_XMS_POOL_KB);
    }

    // ---- EMS ----
    uint32_t esz = dos_ems_state_size_rs();
    void    *est = kmalloc(esz);
    uint8_t *epl = (uint8_t *)kmalloc((uint64_t)DOS_EMS_PAGES * 16384);
    if (est && epl && dos_ems_init_rs(est, epl, DOS_EMS_PAGES, DOS_EMS_FRAME) == 0) {
        t->ems_state = est;
        t->ems_pool  = epl;
        // The device header. A program that walks the driver chain or reads the
        // vector finds a real one here, with the name at the offset the
        // documented test reads.
        wr16(t, DOS_EMM_SEG, 0x0000, 0xFFFF);   // next driver: none
        wr16(t, DOS_EMM_SEG, 0x0002, 0xFFFF);
        wr16(t, DOS_EMM_SEG, 0x0004, 0xC000);   // character device, IOCTL supported
        wr16(t, DOS_EMM_SEG, 0x0006, DOS_EMM_ENTRY);  // strategy
        wr16(t, DOS_EMM_SEG, 0x0008, DOS_EMM_ENTRY);  // interrupt
        static const char emm[8] = { 'E','M','M','X','X','X','X','0' };
        for (int i = 0; i < 8; i++)
            wr8(t, DOS_EMM_SEG, (uint16_t)(0x000A + i), (uint8_t)emm[i]);
        // The landing pad, for a program that FAR CALLs the vector instead of
        // issuing the interrupt. Its first byte is CDh, not CFh, for the #163
        // reason: CFh (IRET) is a documented "no driver installed" marker.
        wr8(t, DOS_EMM_SEG, DOS_EMM_ENTRY,     0xCD);
        wr8(t, DOS_EMM_SEG, DOS_EMM_ENTRY + 1, 0x67);
        wr8(t, DOS_EMM_SEG, DOS_EMM_ENTRY + 2, 0xCB);
        // Point the vector at it. This runs BEFORE the IVT seeding loop in
        // dos_run_file, which only fills vectors that are still 0000:0000, so
        // this survives rather than being overwritten by the IRET stub.
        wr16(t, 0x0000, 0x67 * 4,       DOS_EMM_ENTRY);
        wr16(t, 0x0000, 0x67 * 4 + 2,   DOS_EMM_SEG);
        // And tell the ONE INT 21h service core that the device is openable.
        // State, not identity: the core never asks whose guest this is.
        t->svc.has_ems = 1;
        kprintf("[ems] installed: %u pages (%u KB) expanded memory, page frame %04x:0000\n",
                (unsigned)DOS_EMS_PAGES, (unsigned)DOS_EMS_PAGES * 16, DOS_EMS_FRAME);
    } else {
        if (est) kfree(est);
        if (epl) kfree(epl);
        kprintf("[ems] arena kmalloc(%u KB) FAILED: reporting NOT INSTALLED\n",
                (unsigned)DOS_EMS_PAGES * 16);
    }
}

static void dos_mem_free(dos_task_t *t) {
    if (t->xms_state) { dos_xms_report_rs(t->xms_state); kfree(t->xms_state); t->xms_state = NULL; }
    if (t->xms_pool)  { kfree(t->xms_pool);  t->xms_pool  = NULL; }
    if (t->ems_state) { dos_ems_report_rs(t->ems_state); kfree(t->ems_state); t->ems_state = NULL; }
    if (t->ems_pool)  { kfree(t->ems_pool);  t->ems_pool  = NULL; }
}

// Master interrupt dispatcher for the DOS task.
// (#221) The one place the 16-bit path turns "the service could not answer
// yet" into "execute that INT again". Every vector routes through
// dos_int_handler(), so one wrapper covers INT 16h and INT 21h alike and there
// is no per-vector bookkeeping to forget. The flag is cleared by whoever set
// it having its entry point clear it (dos_svc_int21) or, for INT 16h, here.
static int dos_int_handler_inner(x86_16_cpu_t *c, uint8_t intno);

static int dos_int_handler(x86_16_cpu_t *c, uint8_t intno) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (t) t->svc.input_blocked = 0;
    int r = dos_int_handler_inner(c, intno);
    if (t && t->svc.input_blocked) {
        t->svc.input_blocked = 0;
        return X86_16_INT_RETRY;
    }
    return r;
}

static int dos_int_handler_inner(x86_16_cpu_t *c, uint8_t intno) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return 0;
    switch (intno) {
    case 0x20: t->cpu.halted = 1; t->cpu.exit_code = 0; return 0;  // legacy terminate
    case 0x21: dos_svc_int21(&t->svc, &t->cpu); return 0;
    case 0x10: int10(t); return 0;
    case 0x33: int33(t); return 0;
    case 0x16: t->int16_calls++; int16(t); return 0;
    case 0x1A: {  // (#234a) BIOS time-of-day. Was: CX=DX=0, unconditionally.
        // Read from the SAME four bytes at 0040:006C that a guest reading the
        // BDA directly sees, so the two cannot disagree. The run loop keeps
        // that dword; this is the documented way to ask for it, not a second
        // copy of it.
        switch (AH(c)) {
        case 0x00: {   // read tick count -> CX:DX, AL = midnight rollovers
            uint32_t bt = (uint32_t)rd16(t, 0x0040, 0x006C) |
                          ((uint32_t)rd16(t, 0x0040, 0x006E) << 16);
            c->cx = (uint16_t)(bt >> 16);
            c->dx = (uint16_t)(bt & 0xFFFF);
            AL_SET(c, t->bios_tick_roll);
            t->bios_tick_roll = 0;      // real BIOS clears the flag on read
            break;
        }
        case 0x01: {   // set tick count from CX:DX
            uint32_t want = ((uint32_t)c->cx << 16) | c->dx;
            t->bios_tick_base = want - (uint32_t)(dos_emu_pit_now(t) >> 16);
            t->bios_tick_roll = 0;
            t->bios_tick_last = want;
            wr16(t, 0x0040, 0x006C, (uint16_t)(want & 0xFFFF));
            wr16(t, 0x0040, 0x006E, (uint16_t)(want >> 16));
            break;
        }
        case 0x02: {   // read RTC time -> CH=hour CL=min DH=sec, all BCD
            kdos_clock_t k; kdos_clock_now(&k);
            c->cx = (uint16_t)((dos_bcd8(k.hour) << 8) | dos_bcd8(k.minute));
            c->dx = (uint16_t)((dos_bcd8(k.second) << 8) | 0);  // DL=0: no DST
            c->flags &= (uint16_t)~0x0001;                      // CF=0: clock ok
            break;
        }
        case 0x04: {   // read RTC date -> CH=century CL=year DH=month DL=day, BCD
            kdos_clock_t k; kdos_clock_now(&k);
            c->cx = (uint16_t)((dos_bcd8((uint8_t)(k.year / 100)) << 8) |
                               dos_bcd8((uint8_t)(k.year % 100)));
            c->dx = (uint16_t)((dos_bcd8(k.month) << 8) | dos_bcd8(k.day));
            c->flags &= (uint16_t)~0x0001;
            break;
        }
        default:
            // Unimplemented sub-function: say so rather than return junk that
            // looks like success. CF=1 is what a BIOS with no RTC returns.
            c->flags |= 0x0001;
            break;
        }
        return 0;
    }
    case 0x67: {  // (#745) LIM EMS 4.0. Manager in rustkern/dosmem.rs.
        // A NULL state means the arena could not be allocated, which is the one
        // case where "EMM software not present" is the truth rather than a
        // placeholder. Note the guest normally never gets here in that case: it
        // could not open EMMXXXX0 either, so it concluded there was no EMS.
        if (!t->ems_state) { AH_SET(c, 0x80); return 0; }
        dos_regs_t r;
        dos_regs_out(c, &r);
        dos_ems_dispatch_rs(t->ems_state, &r, t->mem);
        dos_regs_in(c, &r);
        return 0;
    }
    case 0x2F: // multiplex: XMS install check + MSCDEX (#196)
        kprintf("[CDTRACE] 2Fh IN ax=%04x bx=%04x cx=%04x dx=%04x es=%04x\n",
                c->ax, c->bx, c->cx, c->dx, c->es);
        // AX=4300h: XMS driver install check. Real HIMEM returns AL=0x80. We
        // have no XMS, so return AL!=0x80 to report "not installed" (Keen then
        // falls back to conventional memory).
        // (#745) AL=80h means "an XMS driver is installed", and it is answered
        // from whether the arena EXISTS, not from a compile-time constant. The
        // old unconditional 00h was deliberate (Keen falls back to conventional
        // memory and works); it is now conditional on there being real memory
        // behind the yes.
        if (c->ax == 0x4300) { AL_SET(c, t->xms_state ? 0x80 : 0x00); return 0; }
        // AX=4310h: hand back the far entry point. There is no XMS interrupt;
        // the guest CALLs this address and the interpreter's far-call trap
        // (armed in dos_mem_init) turns that into dos_xms_farcall(). If XMS is
        // not installed, ES:BX is left exactly as the caller set it and AX=4300h
        // already told the guest not to look.
        if (c->ax == 0x4310) {
            if (t->xms_state) { c->es = DOS_XMS_SEG; c->bx = DOS_XMS_OFF; }
            return 0;
        }

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
                kprintf("[CDTRACE] 2F/1500 -> bx=%u(count) cx=%u(first)\n",
                        (unsigned)mi.count, (unsigned)mi.first);
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
                kprintf("[CDTRACE] 2F/150B cx=%u -> is_cd=%d\n",
                        (unsigned)c->cx, is_cd);
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
        kprintf("[CDTRACE] 2Fh ax=%04x NOT HANDLED (fall-through)\n", c->ax);
        return 0;
    case 0x15:
        // (#252) BIOS misc services. EXACTLY ONE function is serviced: AH=86h,
        // WAIT. It is the only INT 15h call whose correct answer is "time
        // passed", so returning from it instantly is not an unimplemented
        // service, it is a WRONG one, and it is the measured reason Monkey
        // Island's AdLib probe concludes there is no chip. See
        // rustkern/dosint15.rs for the trace.
        //
        // THIS CASE SITS IMMEDIATELY ABOVE `default:` AND FALLS INTO IT ON
        // PURPOSE. The guest-hook dispatch and the MISS log live in `default:`,
        // so a `break` here would silently stop a guest that installs its own
        // INT 15h handler from ever being dispatched to it, and would delete
        // the MISS line the harness ranks. Every AH except 86h must reach that
        // code byte for byte, which is what makes this a one-function change.
        {
            uint32_t us = 0;
            if (dos_int15_rs(c->ax, c->cx, c->dx, &us)) {
                dos_bus_charge_us(t, us);
                c->flags &= (uint16_t)~0x0001;   // CF=0: the wait completed
                {   // ONE line per boot, not per call: Monkey Island issues
                    // these in threes around every OPL register write.
                    static uint8_t said = 0;
                    if (!said) {
                        said = 1;
                        kprintf("[dos] (#252) INT 15h AH=86h WAIT is SERVICED: "
                                "first call asked for %u us (previously ignored, "
                                "charging 0)\n", (unsigned)us);
                    }
                }
                return 0;
            }
        }
        __attribute__((fallthrough));
    default:
        // An interrupt we do not implement. THE CALL SITE IS THE POINT, and it
        // was missing: without it a storm of these is indistinguishable between
        // (a) a guest genuinely using an interrupt we have not implemented and
        // (b) the interpreter having desynchronised and executing DATA, because
        // a stray `CD nn` byte pair reads as an INT and there are hundreds of
        // those in any binary.
        //
        // #745 hit exactly that ambiguity and could not resolve it: Aladdin
        // produced 2234 INT 88h calls, and its 154 KB image contains exactly
        // ONE `CD 88` byte pair, which is consistent with either explanation.
        // One address settles it. cs:ip here is the RETURN address, so a
        // two-byte `INT nn` is at ip-2; if that address is not inside the
        // guest's code, this is a desync and not a missing feature.
        //
        // Deliberately NOT rate-limited: tools/dos-harness ranks these by line
        // count, so collapsing duplicates would silently change a measurement
        // other work is scoped from. The regex there reads `[^\n]*` between the
        // ax field and "(ignored)", so this extra field does not break it.
        //
        // (#740) BUT FIRST: THE GUEST MAY OWN THIS VECTOR, in which case the
        // only correct thing is to run ITS handler, and "unimplemented" is the
        // wrong question entirely.
        //
        // Aladdin produced 2871 of these lines, every one from 0428:ecc7 inside
        // the timer ISR it had hooked at 0428:ec7d. INT 88h is not a service and
        // not an audio driver. Its installer at 0428:ec13 reads the ORIGINAL
        // INT 8 vector and stores it in the INT 88h slot BY HAND:
        //
        //     mov ax,3508h / int 21h        ; get old INT 8 -> ES:BX
        //     xor ax,ax / mov ds,ax         ; DS = 0000, the IVT
        //     mov si,0220h                  ; 0x220 == 0x88 * 4
        //     mov [si],bx / mov [si+2],es   ; IVT[88h] := old INT 8
        //     mov ax,2508h / int 21h        ; install its own INT 8
        //     out 43h/40h                   ; PIT to ~4x 18.2 Hz
        //
        // (the direct table write is why no AH=25h for 88h appears anywhere).
        // Its ISR then does `test cs:[0f50h],3 / jne skip / int 88h`, i.e. it
        // chains to the BIOS timer every 4th tick to keep the original 18.2 Hz
        // rate. We were shadowing a vector the guest itself installed and
        // answering nothing.
        //
        // Dispatch exactly as hardware does: push FLAGS/CS/IP, clear IF, load
        // CS:IP from the vector. c->ip already points PAST the two-byte INT, so
        // it is the correct return address. Every case in this handler returns
        // 0 ("continue"), so the interpreter simply carries on inside the
        // handler and the guest's IRET returns here.
        //
        // ONLY the genuinely unserviced case still logs a MISS, so the harness
        // ranking of missing services keeps working.
        if (dos_vec_hooked(t, intno)) {
            uint16_t voff = rd16(t, 0x0000, (uint16_t)(intno * 4));
            uint16_t vseg = rd16(t, 0x0000, (uint16_t)(intno * 4 + 2));
            {   // One line per vector, not per call: this is a 13.5/s event.
                static uint8_t said[32];
                if (!(said[intno >> 3] & (1 << (intno & 7)))) {
                    said[intno >> 3] |= (uint8_t)(1 << (intno & 7));
                    kprintf("[dos] INT %02xh is GUEST-OWNED -> %04x:%04x "
                            "(dispatching, was ignored)\n", intno, vseg, voff);
                }
            }
            c->sp = (uint16_t)(c->sp - 2); wr16(t, c->ss, c->sp, c->flags);
            c->sp = (uint16_t)(c->sp - 2); wr16(t, c->ss, c->sp, c->cs);
            c->sp = (uint16_t)(c->sp - 2); wr16(t, c->ss, c->sp, c->ip);
            c->flags &= ~0x0200;            // CLI during the ISR, as INT does
            c->cs = vseg; c->ip = voff;
            return 0;
        }
        // (#740) SOME VECTORS ARE HOOK POINTS, NOT SERVICES, and an unhooked one
        // is a documented NO-OP rather than something we failed to implement.
        // Logging those as MISSes is not just noise, it is a WRONG measurement:
        // the harness ranks MISS counts to scope the next milestone, so a
        // no-op-by-design entry at the top of that ranking sends the next
        // person to implement a function that does not exist.
        //
        // This became visible the moment INT 88h was fixed. Aladdin's ISR
        // chains to the saved BIOS timer, which is now a real stub that ends in
        // `INT 1Ch` exactly as a BIOS does, so INT 1Ch started arriving 18.2
        // times a second from f000:ff60 - OUR OWN stub - and inherited the
        // whole storm the INT 88h line used to carry.
        //
        //   1Bh  Ctrl-Break        1Ch  user timer tick        28h  DOS idle
        //
        // all three are defined as "the BIOS/DOS invokes this; if nobody
        // installed a handler, nothing happens". When the GUEST does hook one,
        // the dispatch above has already run and this line is never reached.
        if (intno == 0x1B || intno == 0x1C || intno == 0x28) return 0;
        kprintf("[dos] INT %02xh ax=%04x bx=%04x from %04x:%04x (ignored)\n",
                intno, c->ax, c->bx, c->cs, (uint16_t)(c->ip - 2));
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

// (#740) Is the guest in an unchained 256-colour mode right now?
//
// There is no INT 10h mode number for Mode X. A game sets mode 13h and then
// clears Chain-4 (bit 3 of Sequencer register 4) behind the BIOS's back, and
// AH=0Fh keeps answering 0x13. So the ONLY authoritative answer is that
// sequencer bit, which this file has stored since #202 and never read.
//
// The SAME BIT dos_vga_decode_geom() decodes as `chain4`, kept as an inline
// test here rather than a call for one reason: this gates every guest byte
// written to the 0xA0000 aperture (millions per second on a real game), where
// the decode runs once per PRESENT. If the two ever need to disagree, that is a
// bug in this comment, not a feature.
static inline int dos_modex_active(const dos_task_t *t) {
    return t->video_mode == 0x13 && !(t->seq_reg[4] & 0x08);
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

// (#740 doom-present) THE SHARED CORE, used by BOTH interpreters.
//
// Before this, this logic existed exactly once, inlined into ega_mem_w/r
// below, and was reachable ONLY from the 16-bit interpreter (t->cpu). A
// DOS/4GW guest's pixel-plotting code runs on the 32-BIT engine (t->le_cpu,
// rustkern/x86_32.rs), which had no hook mechanism at all until this change:
// every byte a 32-bit guest wrote at 0xA0000 landed as a flat store into the
// t->mem arena (see the "THE SWAP" comment in the DOS/4GW loader) and NEVER
// reached t->ega_plane[]. In a chained mode that is harmless, because the
// chained presenter also reads t->mem[VGA_A000+...] directly. In an unchained
// (Mode X) guest it is fatal: four planes share one address range and are
// told apart ONLY by the Sequencer Map Mask, which a flat store cannot honour
// (it can only overwrite whatever was in t->mem at that address, discarding
// the other three planes' data). dos_present_modex() reads EXCLUSIVELY from
// t->ega_plane[], which a 32-bit Mode X guest therefore never wrote to at
// all: the array stayed zero for the guest's entire run, so every sampled
// pixel came back palette index 0, i.e. a perfectly flat fill that tracked
// nothing but that one palette entry's own fade. See CHANGELOG and blame.md.
//
// `width` is 1, 2 or 4: the 32-bit core moves pixels with stosb/stosw/stosd
// and movsd, not only byte stores, so this cannot stay a 1-or-2 special case.
static void dos_vga_write(dos_task_t *t, uint32_t lin, uint32_t val, int width) {
    if (!t) return;

    // (#740) In a VESA mode the 0xA0000 aperture is a 64 KB WINDOW onto a
    // larger VRAM, positioned by 4F05h. Everything above the window's 64 KB is
    // reached by moving the window, which is exactly why this is the only VBE
    // design that fits a 20-bit guest address space.
    if (t->vbe.mode && t->vbe_vram) {
        for (int i = 0; i < width; i++) {
            uint32_t off = (lin + (uint32_t)i - VGA_A000) + t->vbe.bank * VBE_WIN_BYTES;
            if (off < t->vbe.vram) t->vbe_vram[off] = (uint8_t)(val >> (i * 8));
        }
        t->ega_dirty = 1;
        return;
    }

    // (#740) CHAINED mode 13h only. Unchained (Mode X) falls through to the
    // plane machinery below, which is the whole point of unchaining: the guest
    // now addresses four planes through the same 64 KB window, gated by the
    // Sequencer Map Mask, with the latches carrying plane-to-plane block copies.
    // ega_write() already implements exactly that (map mask, write modes 0-3,
    // the latch copy of write mode 1, set/reset, bit mask), because a byte
    // written to a plane is a byte written to a plane whether the guest reads it
    // back as eight 1-bit pixels or as one 8-bit pixel. Nothing here is Mode X
    // specific; the INTERPRETATION of those bytes is, and that is the
    // presenter's job.
    if (t->video_mode == 0x13 && !dos_modex_active(t)) {
        // Mode 13h is a plain linear byte buffer; write straight to mem[].
        for (int i = 0; i < width; i++) {
            uint32_t a = lin + (uint32_t)i;
            if (a < DOS_MEM_SIZE) t->mem[a] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    for (int i = 0; i < width; i++)
        ega_write(t, lin + (uint32_t)i, (uint8_t)(val >> (i * 8)));
}
static uint32_t dos_vga_read(dos_task_t *t, uint32_t lin, int width) {
    if (!t) return 0xFFFFFFFFu;

    if (t->vbe.mode && t->vbe_vram) {
        uint32_t v = 0;
        for (int i = 0; i < width; i++) {
            uint32_t off = (lin + (uint32_t)i - VGA_A000) + t->vbe.bank * VBE_WIN_BYTES;
            uint8_t b = (off < t->vbe.vram) ? t->vbe_vram[off] : 0xFF;
            v |= (uint32_t)b << (i * 8);
        }
        return v;
    }

    if (t->video_mode == 0x13 && !dos_modex_active(t)) {
        uint32_t v = 0;
        for (int i = 0; i < width; i++) {
            uint32_t a = lin + (uint32_t)i;
            uint8_t b = (a < DOS_MEM_SIZE) ? t->mem[a] : 0xFF;
            v |= (uint32_t)b << (i * 8);
        }
        return v;
    }
    // (#740) Unchained reads go through the Graphics Controller Read Map Select
    // (GC index 4) and, just as importantly, RELOAD ALL FOUR LATCHES. A Mode X
    // blit is `lodsb` (load latches) then `stosb` with write mode 1 (store
    // latches), which moves four bytes per instruction pair and is the idiom
    // every Mode X game uses for scrolling and sprite masking. ega_read() has
    // always reloaded the latches, so this needed no new code, only routing.
    uint32_t v = 0;
    for (int i = 0; i < width; i++)
        v |= (uint32_t)ega_read(t, lin + (uint32_t)i) << (i * 8);
    return v;
}

// Mem-hook trampolines registered with the 16-bit interpreter (t->cpu).
static void ega_mem_w(x86_16_cpu_t *c, uint32_t lin, uint16_t val, int width) {
    dos_vga_write((dos_task_t *)c->owner, lin, val, width);
}
static uint16_t ega_mem_r(x86_16_cpu_t *c, uint32_t lin, int width) {
    return (uint16_t)dos_vga_read((dos_task_t *)c->owner, lin, width);
}

// (#740 doom-present) Mem-hook trampolines registered with the 32-bit DOS/4GW
// engine (t->le_cpu). Same shared core as ega_mem_w/r above; see the block
// comment on dos_vga_write() for why a second registration is required at
// all rather than the 16-bit hook being reused.
static uint32_t ega_mem_w32(x86_32_cpu_t *c, uint32_t lin, uint32_t val, int width) {
    dos_vga_write((dos_task_t *)c->owner, lin, val, width);
    return 0;
}
static uint32_t ega_mem_r32(x86_32_cpu_t *c, uint32_t lin, int width) {
    return dos_vga_read((dos_task_t *)c->owner, lin, width);
}

// ---- #175: is there an OPL2 in this machine? -----------------------------
//
// THE ONE PLACE THIS IS DECIDED. Everything else in the OPL2 path derives from
// it, so there is no second opinion to drift.
//
// The answer is NO, and it is no because it is TRUE: this kernel contains no FM
// synthesis and #175 is scoped to detection only. Saying yes would mean
// accepting a guest's instrument patches and note-ons and emitting silence,
// leaving the user with an options screen that reads "Music: AdLib" and a
// machine that makes no sound, with nothing anywhere to explain the difference.
// That is a fabricated capability, the same defect #120 removed from fstat, and
// this project's standing rule is that we do not invent plausible values.
//
// MEASURED, on golden 1989 + the /CONFIG/DOSIO.CFG port trace (2026-08-20), for
// what the shipped corpus actually does with the NO answer:
//
//   Keen 5 (KEEN5E.EXE)  runs the canonical AdLib timer probe at 0x388/0x389
//                        verbatim, reads the empty-socket 0xFF, concludes
//                        absent, and plays. VERDICT PASS, live under input.
//   SkyRoads             scans six Sound Blaster bases, finds none, then writes
//                        2389 FM registers to 0x389 ANYWAY without ever
//                        believing a detection. VERDICT PASS, live.
//   Aladdin              trusts its own SOUND.CFG rather than probing at all.
//                        VERDICT PASS, live.
//
// NOT ONE TITLE IN THE CORPUS DIES IN THE OPL2 PROBE. The ticket's premise did
// not reproduce; see the CHANGELOG entry for the full table.
//
// And there is a positive hazard in saying yes, which is why "harmless
// optimism" is not available here: an AdLib music driver that believes the chip
// is present commonly arms OPL timer 1 and paces its song from the timer
// INTERRUPT. We raise no OPL interrupt. Answering PRESENT would therefore trade
// a working game with no music for a game that waits for an IRQ that never
// comes. Measured as the `installed=1` arm of this build; see the CHANGELOG.
//
// WHEN FM SYNTHESIS LANDS, THIS FUNCTION IS THE WHOLE CHANGE ON THIS LAYER, BUT
// IT WILL NOT BE ENOUGH ON ITS OWN, AND HERE IS THE MEASUREMENT SO THAT NOBODY
// SPENDS A DAY BLAMING rustkern/opl2.rs FOR IT.
//
// Forced INSTALLED via /CONFIG/DOSOPL.CFG, Keen 5 STILL concludes "no AdLib".
// The probe is correct and the chip is correct; the guest runs out of patience.
// Reassembled from the [OPLPROBE] trace, one run, build 19756:
//
//   WRITE reg=0x04 val=0x60   pit_now=102603   mask both
//   WRITE reg=0x04 val=0x80   pit_now=102607   reset the flags
//   WRITE reg=0x02 val=0xFF   pit_now=102611   timer 1 = ONE 80us period
//   WRITE reg=0x04 val=0x21   pit_now=102614   start timer 1 unmasked
//                                              -> t1_deadline = 102709 (+95 ticks)
//   ... 143 status reads, pit_now 102614 -> 102636 ...
//   WRITE reg=0x04 val=0x60   pit_now=102636   give up and put the chip back
//
// The guest's whole "delay at least 80 microseconds" step spans 22 PIT ticks,
// which is 18.4 us. It needed 95. It abandons the probe 73 ticks (61 us) EARLY,
// so the second status read is 0x00 rather than 0xC0 and the game's own test
// correctly reports absent.
//
// MEASURED: 143 reads in 22 ticks is 0.129 us per port read.
// INFERRED (not measured here): the cause is that this interpreter charges an
// `in al,dx` the same emulated time as any other instruction, while a real ISA
// 8-bit I/O cycle is on the order of a microsecond. AdLib detection delays are
// written AS PORT READS precisely because they were self-calibrating on real
// hardware, so an interpreter with no bus-cycle cost runs them roughly 7-8x too
// fast against its own PIT. That is a TIMEBASE issue, deliberately not touched
// by #175: changing what an I/O read costs would move every delay loop in every
// DOS title in the corpus.
//
// So: return 1 here AND give port I/O a bus-cycle cost, and the protocol below
// passes with no rewrite. The timers already run on the guest's own clock.
//
// (#176, 2026-08-20) THE SECOND HALF OF THAT SENTENCE IS NOW DONE AND PROVEN.
// A guest port access costs an ISA bus cycle (rustkern/dosbus.rs, 1000 ns,
// charged out of the same second rather than on top of it, see
// dos_emu_clock_rate). Re-measured on the shipping kernel, one run per arm,
// arms selected only by /CONFIG/DOSBUS.CFG:
//
//   bus cost 0     : the delay loop spans  22 PIT ticks, 95 needed. 0 FM writes.
//   bus cost 1000ns: the delay loop spans 194 PIT ticks, 95 needed. 264 FM writes.
//
// 264 FM register writes is Keen 5 loading its instrument bank, which it only
// does for a chip it believes is there, and its own startup panel changes from
// "Sound Blaster/AdLib" to "/Sound Blaster/AdLib". So the ONLY thing now
// standing between this corpus and a detected AdLib is the `return 0` below,
// and that is a POLICY decision about honesty, not a technical gap. When FM
// synthesis lands, this function really is the whole change on this layer.
//
// /CONFIG/DOSOPL.CFG containing "1" forces the installed arm. It is a
// DIAGNOSTIC gate of the same family as DOSDIAG.CFG / DOSRING.CFG / DOSIO.CFG,
// it is not shipped in the golden, and it exists so that the both-arms
// differential above can be re-run rather than re-argued.
static int g_dos_opl2_force = 0;

// ===========================================================================
// (#182) THE FM SYNTHESISER EXISTS NOW, SO THE POLICY CHANGES.
// ---------------------------------------------------------------------------
// Everything above this line is #175's reasoning and it was correct WHEN IT WAS
// WRITTEN. Its conclusion was not "an OPL2 must never be reported"; it was
// "reporting one while producing silence is a fabrication". #175 said so
// explicitly: "WHEN FM SYNTHESIS LANDS, THIS FUNCTION IS THE WHOLE CHANGE ON
// THIS LAYER", and then measured the one remaining technical gap (the guest's
// delay loop ran 7-8x too fast) and #176 closed it.
//
// THE GATE ON FLIPPING THIS IS THAT SOUND ACTUALLY COMES OUT, and the evidence
// is recorded in the CHANGELOG entry for #182 rather than asserted here. What
// backs it:
//   - userland/lib/opl2 synthesises A440 measured at 439998 mHz against an
//     expected 439990, and its envelopes decay on a rate ladder anchored to a
//     published figure (20317 us measured against 20270).
//   - the guest's register writes reach that core through the queue above and
//     the writes are timestamped, so the music is not quantised to audio blocks.
//   - /APPS/FMSYNTH pushes the result at the SAME PCM sink every other Ring-3
//     audio source uses.
//
// The honest reading of the "positive hazard" paragraph above still applies and
// is NOT waved away: a music driver that believes the chip is present may pace
// its song from the OPL timer INTERRUPT, and this kernel still raises no OPL
// IRQ. That is why the flip is gated on `g_dos_fm_ready` below and not simply
// hard-coded to 1: if the Ring-3 synthesiser is not present on this image, the
// chip goes back to reporting ABSENT and every guest falls back cleanly, which
// is exactly the state #175 shipped and verified.
//
// So: PRESENT when we can actually make the sound, ABSENT when we cannot. That
// is the same rule #175 applied, evaluated against a world where the second
// half is now true.
// ===========================================================================
static int g_dos_fm_ready = 0;   // /APPS/FMSYNTH exists and was launched
static int g_dos_fm_force_off = 0;  // /CONFIG/DOSFM.CFG containing "0"

static uint8_t opl2_installed_policy(void) {
    if (g_dos_opl2_force) return 1;          // the #175 diagnostic arm, unchanged
    if (g_dos_fm_force_off) return 0;        // explicit opt-out, for A/B measurement
    return g_dos_fm_ready ? 1 : 0;           // PRESENT only if we can sound it
}

// ===========================================================================
// (#181) IS THERE A SOUND BLASTER IN THIS MACHINE? ONE place decides.
// ---------------------------------------------------------------------------
// #175 answered the same question for the OPL2 with a flat NO, and the reason
// it gave was that reporting PRESENT would be a fabrication: there is no FM
// synthesis, so a guest that detected the chip would play a song into silence.
//
// THE DSP CASE IS THE OPPOSITE, AND ONLY BECAUSE THE SINK ALREADY EXISTS. The
// guest hands us finished PCM; drivers/audio_pcm.c already carries PCM to a USB
// DAC or to HDA/AC97 through the same pump the music player uses. So PRESENT is
// a TRUE statement about this machine exactly when there is somewhere for the
// samples to go, and a FABRICATION when there is not.
//
// Hence: the card exists if and only if a real output device exists.
// audio_is_available() deliberately excludes the PC-speaker fallback, so a
// machine with no DAC reports ABSENT and every guest falls back to whatever it
// does today, byte for byte, because every port below is gated on this flag.
//
// /CONFIG/DOSSB.CFG forces the arm ("0" = absent, "1" = present) so that the
// both-directions differential can be RE-RUN rather than re-argued. It is a
// diagnostic gate of the same family as DOSDIAG/DOSRING/DOSIO/DOSOPL/DOSBUS and
// is not shipped in the golden.
static int g_dos_sb_force = -1;      // -1 = no override
// /CONFIG/DOSSBCAP.CFG: capture the first N guest DMA bytes to the serial log.
// This is how the PCM is verified on a machine with no speakers: the captured
// bytes are compared against the .RAW file the guest loaded, which proves we
// read the right memory in the right order, and the per-block amplitude
// summary proves the conversion produced audio rather than silence.
static uint32_t g_dos_sbcap = 0;
static uint32_t g_dos_sbcap_done = 0;

static uint8_t sb_installed_policy(void) {
    if (g_dos_sb_force >= 0) return (uint8_t)g_dos_sb_force;
    if (uac_is_ready()) return 1;
    return audio_is_available() ? 1 : 0;
}

// (#fmbridge) The drain latch moved to dos/dosfmq.c with the queue it guards.
// #205: the pid of the /APPS/FMSYNTH we launched, so its exit can be noticed.
static uint32_t       g_dos_fm_synth_pid = 0;

// (#182) Launch the Ring-3 FM synthesiser and report whether it is running.
//
// THIS IS WHAT GATES opl2_installed_policy(). If /APPS/FMSYNTH is missing or
// will not load, this returns 0, the chip reports ABSENT, and every guest falls
// back to no music exactly as it did before #182. That fallback is not a
// nicety: it is the thing that keeps the PRESENT answer honest, because
// PRESENT then means "a synthesiser is running", not "a synthesiser exists in
// the source tree".
// ONE extern, to ONE function, defined in gui/desktop.c beside the other
// app launchers. dosexec.c carries its own hand-written `extern int
// proc_create(...)` that conflicts with proc/process.h, so it cannot include
// the header that declares proc_create_user_as and proc_as_session. Writing a
// SECOND set of hand-made signatures beside the first is exactly how the wrong
// proc_as_session declaration happened, so the launch lives where the headers
// already are and this file asks for it by name.
extern int fm_launch_synth(void);

// #205: THE LATCH THAT NEVER CLEARED, AND WHY IT SILENCED THE SECOND GAME.
//
// g_dos_fm_ready was set here on the first launch and cleared NOWHERE, while
// /APPS/FMSYNTH exits at the END OF EVERY DOS SESSION by design (it renders the
// last note's tail once the guest is gone, then stops). So from the second DOS
// game of a boot onwards this function short-circuited to "already running" for
// a process that had already exited, opl2_installed_policy() kept answering
// PRESENT, the guest happily wrote its whole score into g_dos_fmq, and NOTHING
// DRAINED IT. The chip advertised itself with nothing behind it: precisely the
// fabrication #175 refused to ship, reintroduced by a stale boolean.
//
// It is now a LIVE question rather than a latch: dos_fm_proc_exit() clears both
// this flag and the drain-owner pid when that process dies, so the next guest
// launches a fresh synthesiser. Clearing g_dos_fm_pid matters just as much,
// because dos_fm_drain() latches the first caller and answers EPERM to everyone
// else: without that half, a relaunched FMSYNTH would start, be refused the
// queue, and exit, leaving exactly the same silence one step further along.
static int dos_fm_launch(void) {
    if (g_dos_fm_ready) return 1;         // a synthesiser really is running
    if (g_dos_fm_force_off) return 0;
    int pid = fm_launch_synth();
    if (pid <= 0) {
        audiolog_write("[FM] /APPS/FMSYNTH did NOT launch (%d): the OPL2 will "
                       "report ABSENT and this guest gets no music.", pid);
        return 0;
    }
    g_dos_fm_ready    = 1;
    g_dos_fm_synth_pid = (uint32_t)pid;
    audiolog_write("[FM] /APPS/FMSYNTH launched as pid %d; the OPL2 reports "
                   "PRESENT and FM register writes will be rendered.", pid);
    return 1;
}

// (#fmbridge) THE ONE PLACE THAT DECIDES WHETHER A SYNTHESISER IS RUNNING,
// now that the Ring-3 DOS host can ask for one too (SYS_DOS_FM_HOST LAUNCH,
// dos/dosfmq.c).
//
// It must not be a second copy of dos_fm_launch(): that function owns
// g_dos_fm_ready and g_dos_fm_synth_pid, the pair whose staleness WAS #205. If
// the syscall called fm_launch_synth() directly it would spawn a SECOND
// /APPS/FMSYNTH beside a live one; the drain latch means the newcomer gets
// EPERM and exits, so it is not a correctness bug, but it is a process spawned
// for nothing and a second thing that believes it is the synthesiser.
//
// Returns the pid of a LIVE synthesiser, launching one if there is none, or <=0
// if one cannot be started (no audio sink on this machine, or /APPS/FMSYNTH
// missing - both of which keep the OPL2 honestly ABSENT).
int dos_fm_synth_ensure(void) {
    if (dos_fm_launch()) return (int)g_dos_fm_synth_pid;
    return -1;
}

// #205: the durable half of the FM-bridge exit summary. See the forward
// declaration near dos_on_terminate() for why it lives down here.
static void dos_fm_report_exit(uint32_t pushed, uint32_t dropped) {
    audiolog_write("[FM] guest exited: %u OPL2 register writes were carried to "
                   "Ring 3, %u dropped%s. Synthesiser %s (pid %u).",
                   pushed, dropped,
                   dropped ? " (queue overflow: expect wrong or stuck notes)" : "",
                   g_dos_fm_ready ? "was running" : "was NOT running: NO MUSIC",
                   g_dos_fm_synth_pid);
}

// #205: called from proc_exit() for EVERY process. Cheap pid compare; only the
// synthesiser's own exit does anything. MUST NOT block: proc_exit() runs under
// cli() and this takes the same irqsave spinlock the push path uses.
void dos_fm_proc_exit(uint32_t pid) {
    if (!pid) return;
    int was_synth = 0, was_drainer = 0;
    if (g_dos_fm_synth_pid == pid) { g_dos_fm_synth_pid = 0; g_dos_fm_ready = 0; was_synth = 1; }
    // (#fmbridge) The queue's own latches are released by their owner. That is
    // also what catches a Ring-3 DOS host that DIED without closing the queue:
    // without it the queue stays active forever, FMSYNTH never sees ENODEV,
    // never renders its tail and never exits.
    was_drainer = dos_fmq_host_release_pid(pid);
    if (was_synth || was_drainer) {
        audiolog_write("[FM] the FM synthesiser (pid %u) exited%s. The OPL2 goes "
                       "back to ABSENT until the next guest launches a fresh one.",
                       pid, was_drainer ? " and released the FM queue" : "");
    }
}

// #182: called from dos_out on every guest write to port 0x389. Never waits.
static inline void dos_fm_note_write(dos_task_t *t, uint8_t val) {
    if (!dos_fmq_host_active()) return;   // cheap pre-check; re-tested at the queue
    uint8_t reg = t->opl2.addr;
    uint64_t now = mono_us();
    dos_fmq_host_push(reg, val, now);
}

// (#fmbridge) dos_fm_drain() and dos_fm_event_size() - the SYS_DOS_FM_EVENTS
// backend - moved to dos/dosfmq.c with the queue and the drain latch they
// operate on. They are Ring-0-only by nature (they hand kernel memory to a
// syscall) and were the last thing here that needed the struct.

// The AdLib base. MEASURED, not assumed: #175 was filed saying the probe is at
// port 0x218 and it is not. 0x218 is the OPL alias of a Sound Blaster based at
// 0x210; the corpus probes 0x388, which is where a real AdLib card lives and
// where every Sound Blaster mirrors its OPL as well. SkyRoads scans base 0x210
// for a DSP and still never touches 0x218.
#define DOS_ADLIB_ADDR  0x388   // write: register index.  read: status.
#define DOS_ADLIB_DATA  0x389   // write: data for the latched register.

// ===========================================================================
// (#181) THE DMA PUMP: guest memory -> the shared PCM sink.
// ---------------------------------------------------------------------------
// A SEPARATE KERNEL THREAD, and it has to be. The DAC is a real-time device and
// the interpreter is not: audio_pcm_write_kernel() BLOCKS when the ring is
// full, which is precisely the pacing this wants and precisely what the
// interpreter thread must never do. Running the transfer inline in dos_out
// would stall the guest for the duration of the sample.
//
// #426 - the three waits, and what wakes each. Every one of them is a
// wait_event; there is no poll, no proc_yield spin and no proc_sleep pacing:
//   1. Idle, no transfer armed:
//        wait_event_timeout(&t->sb_wq, armed || stop, 200 ms)
//      WOKEN BY: dos_sb_arm(), called from dos_out the instant the guest writes
//      a transfer command. The timeout exists because the wake is the GUEST's
//      to give and a guest may simply never play another sound; it is a
//      liveness backstop for teardown, not the mechanism.
//   2. Running too far ahead of the DAC:
//        audio_pcm_wait_below_kernel(h, DOS_SB_LEAD, ...)
//      WOKEN BY: the PCM pump's wake_up_all(&s->wq_space) on EVERY consume and
//      on every teardown path (audio_pcm.c), so a dead sink releases us too.
//   3. Waiting for the tail of a block to actually reach the DAC:
//        audio_pcm_wait_consumed_kernel(h, target, ...)
//      Same wake source. This one is what makes the end-of-block interrupt
//      honest: it fires when the last sample has been PLAYED, not queued.
//
// AND THE INTERRUPT IS NOT RAISED FROM HERE INTO THE GUEST. This thread only
// sets a flag; the interpreter thread pushes the frame, because it is the only
// context that may touch the guest's stack and CS:IP.
static void dos_sb_arm(dos_task_t *t);

static void dos_sb_capture(const uint8_t *src, uint32_t n,
                           uint32_t phys, uint32_t total, uint32_t rate,
                           const int16_t *pcm, uint32_t pcm_n) {
    if (!g_dos_sbcap) return;
    // MEASURED, and the reason this filter exists: Aladdin's first 2672 DMA
    // bytes are every one 0x80. It keeps an auto-init mixing buffer running
    // whether or not a sound effect is playing, so a capture of the FIRST N
    // bytes captures silence and settles nothing. Skip all-silent chunks until
    // a non-silent one appears, and COUNT the skipped ones, so that "this
    // title played only silence in the observed window" is a reported result
    // rather than an empty capture someone has to interpret.
    static uint32_t nsilent = 0;
    int quiet = 1;
    for (uint32_t i = 0; i < n; i++)
        if (src[i] != 0x80) { quiet = 0; break; }
    if (quiet && g_dos_sbcap_done == 0) {
        nsilent++;
        if (nsilent == 1 || (nsilent % 64) == 0)
            kprintf("[SBCAP] skipped %u all-silent chunk(s) (0x80 throughout) "
                    "phys=0x%06X bytes=%u rate=%u\n", nsilent, phys, total, rate);
        return;
    }
    // Per-block header, and then the raw guest bytes as hex. The hex is what
    // makes this evidence rather than a claim: it can be diffed byte for byte
    // against the file the guest loaded off the image.
    if (g_dos_sbcap_done == 0)
        kprintf("[SBCAP] FIRST NON-SILENT chunk after %u silent: phys=0x%06X "
                "bytes=%u rate=%u sink=%u\n",
                nsilent, phys, total, rate, DOS_SB_SINK_RATE);
    // Amplitude summary of the CONVERTED samples. A capture that only showed
    // the input bytes could not tell "we converted it correctly" from "we
    // converted it to silence".
    int32_t lo = 32767, hi = -32768; int64_t absum = 0;
    for (uint32_t i = 0; i < pcm_n; i++) {
        int32_t v = pcm[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        absum += (v < 0) ? -v : v;
    }
    kprintf("[SBCAP] pcm n=%u min=%d max=%d meanabs=%u\n",
            pcm_n, (int)lo, (int)hi,
            (unsigned)(pcm_n ? (uint32_t)(absum / pcm_n) : 0));
    uint32_t left = (g_dos_sbcap > g_dos_sbcap_done)
                        ? (g_dos_sbcap - g_dos_sbcap_done) : 0;
    if (left > n) left = n;
    for (uint32_t off = 0; off < left; off += 32) {
        uint32_t run = (left - off > 32) ? 32 : (left - off);
        char line[80];
        for (uint32_t i = 0; i < run; i++) {
            static const char hx[] = "0123456789ABCDEF";
            line[i * 2 + 0] = hx[(src[off + i] >> 4) & 0xF];
            line[i * 2 + 1] = hx[src[off + i] & 0xF];
        }
        line[run * 2] = 0;
        kprintf("[SBCAP] raw off=%u %s\n", g_dos_sbcap_done + off, line);
    }
    g_dos_sbcap_done += left;
}

static void dos_sb_pump(void *arg) {
    dos_task_t *t = (dos_task_t *)arg;
    int h = -1;
    // Latched, not re-tested per block: a stream that could not be opened once
    // is not going to be retried on every sound effect, which would turn a busy
    // DAC into a per-effect stall.
    int dry = 0;
    int16_t *src16 = NULL;
    int16_t *dst16 = NULL;
    // Worst case expansion: the sink runs at 44100 and the lowest rate the DSP
    // model will report is 4000, so one guest byte can become 12 sink frames.
    const uint32_t dstcap = DOS_SB_CHUNK * (DOS_SB_SINK_RATE / 4000u + 1u) + 4u;

    src16 = (int16_t *)kmalloc(DOS_SB_CHUNK * sizeof(int16_t));
    dst16 = (int16_t *)kmalloc((size_t)dstcap * sizeof(int16_t));
    if (!src16 || !dst16) {
        kprintf("[dos] (#181) SB pump: out of memory, no digitised audio\n");
        if (src16) kfree(src16);
        if (dst16) kfree(dst16);
        t->sb_pump_live = 0;
        wake_up_all(&t->sb_wq);
        return;
    }

    while (!t->sb_pump_stop && t->running) {
        if (!t->sb.active || !dos_dma_playback_armed_rs(&t->dma, t->sb.dma)) {
            (void)wait_event_timeout(&t->sb_wq,
                    t->sb_pump_stop || !t->running ||
                    (t->sb.active && dos_dma_playback_armed_rs(&t->dma, t->sb.dma)),
                    wq_ms_to_ticks(200));
            continue;
        }

        // Open the sink lazily and ONCE per task. A failure is not fatal and is
        // not retried in a tight loop: the guest keeps running with the card
        // present and silent, and the census says so at exit. That is the
        // honest behaviour for "another process holds the one DAC".
        if (h < 0 && !dry) {
            int64_t rc = audio_pcm_open_kernel(DOS_SB_SINK_RATE, 1,
                                               AUDIO_FORMAT_S16_LE);
            if (rc < 1) {
                // NOT retried in a loop, and NOT fatal. One stream exists on
                // this machine and something else may hold it; see the header
                // of this patch for why the transfer still has to RUN.
                t->sb_open_fail++;
                dry = 1;
                kprintf("[dos] (#181) SB pump: PCM sink unavailable (rc=%d); "
                        "running the DMA DRY so the guest still completes its "
                        "transfers, but there will be no sound\n", (int)rc);
            } else {
                h = (int)rc;
                kprintf("[dos] (#181) SB pump: PCM stream open, sink %u Hz mono\n",
                        DOS_SB_SINK_RATE);
            }
        }

        uint8_t  chan  = t->sb.dma;
        uint8_t  gen   = t->sb.gen;
        uint32_t total = dos_dma_block_bytes_rs(&t->dma, chan);
        uint32_t rate  = t->sb.rate ? t->sb.rate : 11111u;
        uint32_t phys  = dos_dma_cur_phys_rs(&t->dma, chan);
        // The DSP's own length and the 8237's count can disagree (a driver may
        // program a shorter DSP block inside a larger DMA buffer). The SHORTER
        // one wins: transferring past either is reading memory the guest did
        // not offer.
        if (t->sb.block_len && (uint32_t)t->sb.block_len < total)
            total = t->sb.block_len;
        // The bound is on the PAGE, not on phys+total: with the wrap above, a
        // transfer cannot leave its own 64 KB page, so the only thing that can
        // be out of range is the page itself.
        if (total == 0 || (phys & 0xFF0000u) >= DOS_MEM_SIZE) {
            kprintf("[dos] (#181) SB block refused: phys=0x%06X bytes=%u "
                    "(outside the guest's 1 MiB)\n", phys, total);
            dos_sb_block_done_rs(&t->sb);
            t->sb.active = 0;
            continue;
        }

        // ---- the DRY path: no sink, so the CLOCK is the clock ------------
        // A bounded wait_event_timeout per chunk of guest time, which is the
        // sanctioned timed primitive (#426). It is not a poll of a condition
        // somebody else is supposed to set: the thing being waited for IS the
        // passage of time, because that is what paces a DAC.
        if (dry) {
            uint64_t t0 = sched_now_ms();
            uint32_t step_ms = (DOS_SB_CHUNK * 1000u) / rate;
            if (step_ms == 0) step_ms = 1;
            for (;;) {
                if (t->sb_pump_stop || !t->running || !t->sb.active ||
                    t->sb.gen != gen) break;
                uint64_t el = sched_now_ms() - t0;
                uint64_t played = (el * (uint64_t)rate) / 1000ull;
                if (played >= total) break;
                (void)dos_dma_set_played_rs(&t->dma, chan, (uint32_t)played);
                (void)wait_event_timeout(&t->sb_wq,
                        t->sb_pump_stop || !t->running || !t->sb.active ||
                        t->sb.gen != gen, wq_ms_to_ticks(step_ms));
            }
            if (!t->sb.active || t->sb.gen != gen) continue;
            t->sb_bytes += total;
            (void)dos_dma_set_played_rs(&t->dma, chan, total);
            t->sb_blocks++;
            dos_sb_raise_irq_rs(&t->sb);
            dos_sb_block_done_rs(&t->sb);
            continue;
        }

        audio_resample_state_t rs;
        audio_resample_stream_init(&rs);
        uint32_t start_cons = audio_pcm_consumed_kernel(h);
        uint32_t written    = 0;      // sink frames written for this block
        uint32_t sent       = 0;      // guest bytes read for this block

        while (sent < total && !t->sb_pump_stop && t->running &&
               t->sb.active && t->sb.gen == gen) {
            // #426 wait 2: never run more than DOS_SB_LEAD ahead of the DAC.
            (void)audio_pcm_wait_below_kernel(h, DOS_SB_LEAD, 2000);
            uint32_t n = total - sent;
            if (n > DOS_SB_CHUNK) n = DOS_SB_CHUNK;
            // The read address comes from the CHIP MODEL, so the 16-bit wrap
            // inside the page is applied in one place and not re-derived here.
            // A chunk that would straddle the wrap is split at it.
            uint32_t rd = dos_dma_phys_at_rs(&t->dma, chan, sent);
            uint32_t to_page_end = 0x10000u - (rd & 0xFFFFu);
            if (n > to_page_end) n = to_page_end;
            if (rd >= DOS_MEM_SIZE) break;
            dos_sb_u8_to_s16_rs(&t->mem[rd], src16, n);
            uint32_t m = audio_resample_stream(&rs, src16, n, rate,
                                               dst16, dstcap, DOS_SB_SINK_RATE, 1);
            dos_sb_capture(&t->mem[rd], n, phys, total, rate, dst16, m);
            if (m) {
                if (t->sb.speaker) {
                    int64_t wr = audio_pcm_write_kernel(h, dst16, m);
                    if (wr > 0) written += (uint32_t)wr;
                } else {
                    // Speaker off: a real card still runs the DMA, it just
                    // does not drive the amplifier. Feed silence rather than
                    // skipping, so the guest's timing is unchanged.
                    memset(dst16, 0, (size_t)m * sizeof(int16_t));
                    int64_t wr = audio_pcm_write_kernel(h, dst16, m);
                    if (wr > 0) written += (uint32_t)wr;
                }
            }
            sent += n;
            t->sb_bytes += n;
            // Publish the PLAYED position, derived from the sink's own consume
            // counter. Never from `sent`: `sent` is what we have QUEUED, and a
            // guest polling the count register would then see the transfer
            // finish up to DOS_SB_LEAD frames early and start the next one on
            // top of audio that is still playing.
            uint32_t cons = audio_pcm_consumed_kernel(h) - start_cons;
            uint64_t played = ((uint64_t)cons * rate) / DOS_SB_SINK_RATE;
            if (played > total) played = total;
            (void)dos_dma_set_played_rs(&t->dma, chan, (uint32_t)played);
        }

        if (!t->sb.active || t->sb.gen != gen) continue;   // halted or re-armed

        // #426 wait 3: the tail. Wait for the sink to consume everything this
        // block wrote, so the interrupt below reports a sample that has been
        // PLAYED. Bounded generously (the block's own duration plus a second)
        // because the wake belongs to the sink and a dead sink must not wedge
        // the guest.
        uint32_t ms = (written * 1000u) / DOS_SB_SINK_RATE + 1000u;
        uint64_t tail_t0 = sched_now_ms();
        int tw = audio_pcm_wait_consumed_kernel(h, start_cons + written, ms);
        uint64_t tail_ms = sched_now_ms() - tail_t0;
        // (#sbirq32) HOW LONG THE ACKNOWLEDGE ACTUALLY TOOK, for the first few
        // blocks of every run. A guest cannot see how long we took to raise its
        // end-of-block interrupt, and a DRIVER PROBE CAN: SBLASTER.DIG's detect
        // arms a SIXTEEN-BYTE transfer and gives up after twenty BIOS ticks, so
        // "the interrupt is correct but late" and "there is no card" are the
        // same observation from inside the guest. This is the number that tells
        // the two apart, and it costs six lines once per run.
        if (t->sb_blocks < 6) {
            kprintf("[dos] (#181) SB block %u ack: %u guest bytes at %u Hz -> "
                    "%u sink frames, tail wait %s after %llu ms (bound %u ms)\n",
                    t->sb_blocks, total, rate, written,
                    tw == WAIT_OK ? "SATISFIED" : "TIMED OUT",
                    (unsigned long long)tail_ms, ms);
        }
        (void)dos_dma_set_played_rs(&t->dma, chan, total);
        t->sb_blocks++;
        dos_sb_raise_irq_rs(&t->sb);
        dos_sb_block_done_rs(&t->sb);
    }

    if (h >= 0) audio_pcm_close_kernel(h);
    kfree(src16);
    kfree(dst16);
    t->sb_pump_live = 0;
    // The joiner in dos_run_file()'s teardown waits on this exact flag through
    // this exact queue, so the wake belongs here and on no other path out.
    wake_up_all(&t->sb_wq);
}

// Called from dos_out the instant a DSP transfer command completes. Creates the
// pump on first use, then wakes it.
static void dos_sb_arm(dos_task_t *t) {
    if (!t->sb_pump_live) {
        t->sb_pump_stop = 0;
        t->sb_pump_live = 1;
        if (proc_create("dossbpump", dos_sb_pump, t, PRIO_NORMAL) < 0) {
            t->sb_pump_live = 0;
            kprintf("[dos] (#181) SB pump thread could not be created\n");
            return;
        }
    }
    wake_up_all(&t->sb_wq);
}

// ---- I/O port hooks (VGA DAC + status) -----------------------------------
static uint16_t dos_in(x86_16_cpu_t *c, uint16_t port, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return 0xFFFF;
    (void)width;
    dos_bus_tick(t);   // (#176) charge FIRST: the CPU latches at the end of the cycle
    dos_iotrace_all(port, 0, width);
    if (port == 0x3DA || port == 0x3BA) {
        // VGA input status #1. Reading it resets the attribute-controller
        // flip-flop. We toggle a broad set of bits (display-enable 0x01,
        // vertical-retrace 0x08, plus 0x80) on every read so any "wait for the
        // status to change / wait for retrace" polling loop makes progress and
        // exits. id's VGA-detection routine watches bit 0x80 toggle.
        t->atc_flipflop = 0;
        static uint8_t tog = 0;
        tog = (uint8_t)(tog ^ 0x89);
        if (g_dos_3da_timebase) tog = (uint8_t)(dos_3da_beam(t) | (tog & 0x80));
        if (g_dos_3da_trip) {
            dos_3da_census(c->cs, c->ip);
            if (++g_dos_3da_n >= g_dos_3da_trip) {
                g_dos_3da_trip = g_dos_3da_n * 5;
                kprintf("[3DA] ---- read #%lu mode=0x%02X cs:ip=%04X:%04X "
                        "ax=%04X dx=%04X cx=%04X returning=0x%02X "
                        "emupit=%lu insn=%lu busacc=%lu ----\n",
                        (unsigned long)g_dos_3da_n, t->video_mode,
                        c->cs, c->ip, c->ax, c->dx, c->cx, tog,
                        (unsigned long)dos_emu_pit_now(t),
                        (unsigned long)dos_emu_insns(t),
                        (unsigned long)t->bus.n_access);
                for (int i = 0; i < g_3da_site_n; i++)
                    kprintf("[3DA] site %04X:%04X n=%lu\n",
                            (unsigned)(g_3da_site[i].csip >> 16),
                            (unsigned)(g_3da_site[i].csip & 0xFFFF),
                            (unsigned long)g_3da_site[i].n);
                if (g_3da_site_lost)
                    kprintf("[3DA] sites beyond the table: %lu reads\n",
                            (unsigned long)g_3da_site_lost);
                x86_16_ring_dump("#177-3DA-burst", 40);
            }
        }
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
    if (port >= 0x40 && port <= 0x42) {
        // PIT data ports. Originally there was NO case for 0x40/0x43 at all, so
        // this returned 0xFF: "a delay-loop calibration read start == end ==
        // 0xFFFF, its delta was always 0, its `cmp ax,imm` never passed, and the
        // loop was unterminatable BY CONSTRUCTION." (#740)
        //
        // (#172) That was fixed for CHANNEL 0 ONLY, and channel 2 kept the exact
        // defect the sentence describes: Stunts sits in a channel-2 delay loop
        // reading port 0x42 and comparing, and read 0xFFFF forever. All three
        // data ports now go through the one register protocol.
        int ch = port - 0x40;
        return dos_pit_read_rs(&t->pit[ch], dos_pit_count_ch(t, ch));
    }
    if (port == 0x61) {
        // (#172) PPI port B / speaker control. Never handled, so a read-modify-
        // write of the channel-2 gate (`in al,0x61 / or al,1 / out 0x61,al`,
        // which is what Stunts does immediately before its delay loop) read
        // 0xFF and wrote 0xFF back, setting every bit including the parity and
        // NMI-disable bits.
        //
        // Bit 4 is the RAM-refresh bit, which real hardware toggles at ~66 kHz
        // and which some timing loops poll INSTEAD of the counter. It is
        // toggled on every read for the same reason 0x3DA's bits are: a poll
        // loop must be able to make progress. Bit 5 is the channel-2 output,
        // derived from the counter's own phase so that it is consistent with
        // what port 0x42 reports rather than being a second, disagreeing
        // opinion about the same counter.
        t->port61_toggle ^= 0x10;
        uint8_t out2 = (uint8_t)((dos_pit_count_ch(t, 2) & 0x8000u) ? 0x20 : 0x00);
        return (uint8_t)((t->port61 & 0x0F) | t->port61_toggle | out2);
    }
    if (port == DOS_ADLIB_ADDR) {
        // (#175) OPL2 status. Returns 0xFF while the socket is empty, which is
        // what an undriven ISA data bus floats to and is therefore the honest
        // answer, not a placeholder. The protocol is real either way: see
        // rustkern/opl2.rs and opl2_installed_policy() above.
        uint64_t pnow = dos_emu_pit_now(t);
        uint8_t st = dos_opl2_status_rs(&t->opl2, pnow);
        if (g_dos_iotrace) {
            // The probe's own arithmetic, so "did timer 1's deadline arrive
            // before the guest looked" is READ, not reasoned about. Twelve
            // lines is the whole detect sequence and no more.
            static int nprobe = 0;
            // Only the reads that can carry information: once timer 1 is
            // running, every read is decisive. Before that they are delay
            // reads and 400 of them would bury the sequence.
            if (nprobe < 400) {
                nprobe++;
                kprintf("[OPLPROBE] status=0x%02X pit_now=%lu t1_deadline=%lu "
                        "run=%u mask=%u preset=0x%02X\n",
                        st, (unsigned long)pnow,
                        (unsigned long)t->opl2.t1_deadline,
                        t->opl2.t1_run, t->opl2.t1_mask, t->opl2.t1_preset);
            }
        }
        return st;
    }
    // (rakbd) THE 8042, AND THE CONSTANT THAT SAID THE OPPOSITE OF WHAT IT MEANT.
    //
    // Port 0x64 returned a flat 0x14 with the comment "output buffer full +
    // system flag". 0x14 is 0b0001_0100: bit 2 (system flag) and bit 4
    // (keyboard not inhibited). BIT 0 IS THE OUTPUT-BUFFER-FULL FLAG AND IT IS
    // CLEAR. So the status byte said "there is no data" on every read, forever,
    // while its own comment claimed the reverse. A guest that polls the
    // controller the documented way -
    //
    //     wait:  in al, 0x64 ; test al, 1 ; jz wait ; in al, 0x60
    //
    // - could never leave that loop, and a guest that skipped the status check
    // read a port 0x60 that nothing had written since the memset. Both halves
    // of the polled path were dead, which is the whole of why Red Alert reaches
    // gameplay and ignores the keyboard.
    //
    // SCOPED TO THE GUESTS THAT MUST POLL. A guest with its own INT 9 handler
    // keeps the previous behaviour byte for byte: its ISR is entered by
    // dos_deliver_int9(), which latches the scancode into kbd_port60 first, and
    // Commander Keen's Galaxy engine reads 0x60 in exactly that context without
    // consulting 0x64. Changing the status byte underneath a working ISR to fix
    // a poller would have been trading one broken guest for another.
    if (port == 0x60) {
        t->p60_reads++;
        // (rakbd2) ...and a 0205h guest is an ISR-driven guest, so it reads the
        // byte dos4gw_deliver_int9() latched, not the poller FIFO.
        if (!t->kbd_has_int9 && !t->kbd_int9_pm && t->p60_rd != t->p60_wr) {
            t->kbd_port60 = t->p60_fifo[t->p60_rd];
            t->p60_rd = (uint8_t)((t->p60_rd + 1u) % (uint8_t)sizeof t->p60_fifo);
        }
        return t->kbd_port60;                      // keyboard data port
    }
    if (port == 0x64) {
        t->p64_reads++;
        // 0x14 keeps the system flag and the not-inhibited bit exactly as
        // before; bit 0 now tells the truth about whether a byte is waiting.
        if (!t->kbd_has_int9 && !t->kbd_int9_pm)
            return (uint16_t)(0x14u | ((t->p60_rd != t->p60_wr) ? 0x01u : 0x00u));
        return 0x14;                               // ISR-driven guest: unchanged
    }
    // (#181) The Sound Blaster DSP, and the 8237 that feeds it. BOTH are gated
    // on the same installed flag: the DMA controller is emulated here only in
    // order to serve this card, and decoding it for a machine with no card
    // would change what every DOS title in the corpus reads from ports
    // 0x00..0x0F for no benefit at all. With the flag clear, every path below
    // is skipped and these ports fall through to the 0xFF default exactly as
    // they did before #181.
    if (t->sb.installed) {
        if ((port & 0xFFF0u) == DOS_SB_BASE) {
            uint16_t off = port & 0x0Fu;
            if (off == 0x6 || off == 0xA || off == 0xC || off == 0xE)
                return dos_sb_read_rs(&t->sb, off);
        }
        if (port <= 0x0F) return dos_dma_in_rs(&t->dma, port);
    }
    dos_iotrace(port, 0, 0xFF, width);   // (#175) the wall-finder
    return 0xFF;
}

static void dos_out(x86_16_cpu_t *c, uint16_t port, uint16_t val, int width) {
    dos_task_t *t = (dos_task_t *)c->owner;
    if (!t) return;
    (void)width;
    dos_bus_tick(t);   // (#176)
    dos_iotrace_all(port, 1, width);
    switch (port) {
    case 0x43: {  // PIT control word
        uint8_t cw = (uint8_t)(val & 0xFF);
        uint8_t ch = (uint8_t)(cw >> 6);
        uint8_t rw = (uint8_t)((cw >> 4) & 3);
        // (#172) This used to be `if (ch != 0) break;` with the comment "only
        // channel 0 is emulated", which is why Stunts' `out 0x43,0x80` (latch
        // counter 2) was silently dropped and its two reads of port 0x42 got
        // 0xFF twice. ch == 3 is the 8254 read-back command, which no guest in
        // this corpus issues; it is still ignored, but as a decoded case
        // rather than as everything-that-is-not-zero.
        if (ch > 2) break;
        dos_pit_ctrl_rs(&t->pit[ch], rw, dos_pit_count_ch(t, ch));
        break;
    }
    case 0x40:    // PIT channel 0 reload (IRQ0 rate)
    case 0x41:    // PIT channel 1 reload (DRAM refresh; state kept, unused)
    case 0x42:    // PIT channel 2 reload (speaker / delay loops)
        dos_pit_write_rs(&t->pit[port - 0x40], (uint8_t)(val & 0xFF));
        break;
    case 0x61:    // (#172) PPI port B: bit 0 gates PIT channel 2, bit 1 the speaker
        t->port61 = (uint8_t)(val & 0xFF);
        t->pit[2].gate = (uint8_t)(val & 1);
        break;
    case DOS_ADLIB_ADDR:   // (#175) OPL2 register-index latch
        dos_opl2_addr_rs(&t->opl2, (uint8_t)(val & 0xFF));
        break;
    case DOS_ADLIB_DATA:   // (#175) OPL2 register data
        if (g_dos_iotrace && t->opl2.addr >= 0x02 && t->opl2.addr <= 0x04) {
            static int nctl = 0;
            if (nctl < 40) {
                nctl++;
                kprintf("[OPLPROBE] WRITE reg=0x%02X val=0x%02X pit_now=%lu\n",
                        t->opl2.addr, (uint8_t)(val & 0xFF),
                        (unsigned long)dos_emu_pit_now(t));
            }
        }
        dos_opl2_data_rs(&t->opl2, (uint8_t)(val & 0xFF), dos_emu_pit_now(t));
        // (#182) AND out to the Ring-3 synthesiser. AFTER dos_opl2_data_rs, so
        // the register index used is the one that was latched for THIS write:
        // reg 0x04 bit 7 is an IRQ reset that the detection path consumes and
        // must not be reordered around.
        dos_fm_note_write(t, (uint8_t)(val & 0xFF));
        break;
    case 0x3C8:  // DAC write index
        t->dac_widx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C7:  // DAC read index
        t->dac_ridx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C9:  // DAC data write (r,g,b sequence, 6-bit each)
        // (#740) After VBE 4F08h selects an 8-bit DAC the guest writes the FULL
        // byte and the 0x3F mask would quarter every colour. The mask is gated
        // on dac8 rather than removed, so the 6-bit path stays bit-identical.
        t->pal[t->dac_widx & 0xFF][t->dac_phase] =
            (uint8_t)(val & (t->vbe.dac8 ? 0xFF : 0x3F));
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
            if (t->seq_idx == 4) dos_seq4_note(t, "OUT 3C4h word");
        }
        break;
    case 0x3C5:
        t->seq_reg[t->seq_idx & 7] = (uint8_t)(val & 0xFF);
        if (t->seq_idx == 2) t->seq_map_mask = (uint8_t)(val & 0x0F);
        if ((t->seq_idx & 7) == 4) dos_seq4_note(t, "OUT 3C5h");
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

    // ---- (#212) CGA Color Select Register ----
    // The other half of the AH=0Bh pair above. A CGA-era title is as likely
    // to poke this port directly as to call the BIOS, and a palette change
    // that only one of the two routes honours is a title whose colours change
    // depending on which route it happened to take.
    case 0x3D9:
        t->cga_pal = (uint8_t)(val & 0xFF);
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
        // (#181) The Sound Blaster and the 8237. See the matching note in
        // dos_in: one gate, both devices, and a no-op when the card is absent.
        if (t->sb.installed) {
            uint8_t v8 = (uint8_t)(val & 0xFF);
            if ((port & 0xFFF0u) == DOS_SB_BASE) {
                uint16_t off = port & 0x0Fu;
                if (off == 0x6) { dos_sb_reset_rs(&t->sb, v8); return; }
                if (off == 0xC) {
                    if (dos_sb_write_rs(&t->sb, v8)) dos_sb_arm(t);
                    return;
                }
            }
            if (port <= 0x0F) { dos_dma_out_rs(&t->dma, port, v8); return; }
            // Page registers. The channel mapping is a property of the
            // MOTHERBOARD's address decode, not of the 8237, which is why it
            // is resolved here and passed in rather than being re-derived
            // inside the chip model.
            if (port == 0x87) { dos_dma_page_rs(&t->dma, 0, v8); return; }
            if (port == 0x83) { dos_dma_page_rs(&t->dma, 1, v8); return; }
            if (port == 0x81) { dos_dma_page_rs(&t->dma, 2, v8); return; }
            if (port == 0x82) { dos_dma_page_rs(&t->dma, 3, v8); return; }
        }
        dos_iotrace(port, 1, val, width);   // (#175) the wall-finder
        break;
    }
}

// ===========================================================================
// #740: ONE CRTC/ATC/Sequencer state model, read by every presenter.
//
// Before this, dos_present_ega() was the only presenter that read display
// start address, logical line width, Line Compare and pixel panning off the
// register file, and it decoded all four ad hoc inline. The mode 13h path
// (below, dos_present_chain4()) read none of them: a hardcoded 320-byte
// stride from offset 0, always. That asymmetry is the suspected cause of a
// live user-reported bug, and with modex/vesa/crtc13 about to add three more
// presenters to this file, three more ad hoc decodes would have turned one
// inconsistency into five. dos_vga_decode_geom() is now the ONE place that
// happens; every presenter (existing or new) calls it instead of reading
// crtc[]/atc_reg[]/seq_reg[] directly.
//
// The formulas below are lifted VERBATIM from the pre-existing, measured-good
// dos_present_ega() code (#385): stride is the CRTC Offset register (0x13,
// words) x2 for bytes; Line Compare is assembled from CRTC 0x18 (bits 0-7) +
// CRTC 0x07 bit 4 (bit 8) + CRTC 0x09 bit 6 (bit 9); pixel pan is ATC 0x13
// bits 0-2; start address is CRTC 0x0C:0x0D taken as a raw byte offset (the
// EGA path has never applied a word/dword unit multiplier to it, and Keen
// 5/7 already render correctly against that formula, so this extraction does
// not change it - a "more textbook-accurate" multiplier is EXPLICITLY not
// applied here because it is unverified against this emulation's addressing
// and the existing formula is proven, not merely plausible).
//
// map_mask/chain4/read_map are decoded but NOT yet consumed by any presenter
// in this file: they exist for Mode X (unchained VGA), which needs Sequencer
// Map Mask to know which of the 4 planes a byte write targets and Graphics
// Controller Read Map Select to know which plane a byte read comes from, and
// Chain-4 to tell mode 13h/VESA linear addressing apart from Mode X's
// unchained addressing. Centralizing the decode now means modex/vesa/crtc13
// read these fields instead of re-decoding the same three registers a third,
// fourth and fifth time.
typedef struct {
    uint32_t start_off;      // CRTC 0x0C/0x0D: display start, raw byte offset
    uint32_t stride_bytes;   // CRTC 0x13 (Offset, words) * 2, or a mode default
    uint32_t line_compare;   // CRTC 0x18 + 0x07 bit4 (bit8) + 0x09 bit6 (bit9)
    uint32_t pan_x;          // ATC 0x13 bits 0-2: horizontal pixel panning
    uint8_t  map_mask;       // SEQ reg 2 (Map Mask): plane(s) a CPU write hits
    uint8_t  chain4;         // SEQ reg 4 bit 3: Chain-4 (linear byte-per-pixel)
    uint8_t  read_map;       // GC reg 4 (Read Map Select): plane a CPU read hits
    // #163: DISPLAYED ROWS, from the CRTC's vertical timing (Vertical Display
    // End, Start Vertical Blank, and the scanlines-per-row divisor). 0 means
    // "could not be derived, keep the mode's nominal gfx_h". A presenter that
    // draws gfx_h rows when the CRTC says fewer runs its fetch off the end of
    // the programmed layout, which on The Incredible Machine wrapped the read
    // back to address 0 and put the top of the screen at the bottom. See
    // dos_vga_rows_rs in rustkern/doswin.rs for the measurement.
    uint32_t rows;
} dos_vga_geom_t;

static void dos_vga_decode_geom(const dos_task_t *t, dos_vga_geom_t *g,
                                uint32_t default_stride_bytes) {
    g->start_off    = ((uint32_t)t->crtc[0x0C] << 8) | t->crtc[0x0D];
    // (#740) ONE definition of the sentinel rule, shared with the mode-set
    // seeding that has to know what "unprogrammed" means. See doscrtc.rs.
    g->stride_bytes = dos_crtc_stride_rs(t->crtc[0x13], default_stride_bytes);
    g->line_compare = (uint32_t)t->crtc[0x18]
                    | ((uint32_t)(t->crtc[0x07] & 0x10) << 4)
                    | ((uint32_t)(t->crtc[0x09] & 0x40) << 3);
    g->pan_x     = t->atc_reg[0x13] & 0x07;
    g->map_mask  = t->seq_reg[2] & 0x0F;
    g->chain4    = (t->seq_reg[4] & 0x08) ? 1 : 0;
    g->read_map  = t->gc_read_map & 0x03;
    g->rows      = dos_vga_rows_rs(t->crtc, (uint32_t)sizeof(t->crtc));
}

// #163 instrument. UNCONDITIONAL and one-shot-per-distinct-value, deliberately
// NOT behind g_x86_dbgring: a diagnostic that needs a config file on the right
// partition to fire is a diagnostic that is usually silent when you need it,
// and a silent log then reads as "the program does nothing interesting", which
// is the exact wrong conclusion. Bounded to 24 lines per boot, and the geometry
// only changes when a program reprograms the CRTC, so a normal session emits
// one or two.
static void dos_geom_note(const char *what, const dos_task_t *t,
                          const dos_vga_geom_t *g) {
    static uint32_t last[6] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    static int n = 0;
    // (#740) THE VIDEO MODE IS PART OF THE KEY, and leaving it out cost a whole
    // investigation. Aladdin's mode 13h decoded to the SAME tuple as the mode
    // 0Dh it had just left (stride 40, start 0, lc 1023, pan 0, rows 200) -
    // because the stale mode-0Dh Offset was the bug - so this note suppressed
    // the mode-13h line as a duplicate. The absence of that line was then read
    // as evidence about the present path, when it was really the diagnostic
    // hiding the defect. A dedupe key that can collide ACROSS modes cannot
    // report a mode change, which is the one event this note exists to report.
    if (last[0] == g->stride_bytes && last[1] == g->start_off &&
        last[2] == g->line_compare && last[3] == g->pan_x && last[4] == g->rows &&
        last[5] == (uint32_t)t->video_mode)
        return;
    last[0] = g->stride_bytes; last[1] = g->start_off; last[2] = g->line_compare;
    last[3] = g->pan_x;        last[4] = g->rows;
    last[5] = (uint32_t)t->video_mode;
    if (n >= 24) return;
    n++;
    kprintf("[dos] #163 %s mode=%02x %dx%d stride=%u start=%u lc=%u pan=%u rows=%u\n",
            what, t->video_mode, t->gfx_w, t->gfx_h, g->stride_bytes,
            g->start_off, g->line_compare, g->pan_x, g->rows);
    kprintf("[dos] #163   CRTC");
    for (int i = 0; i <= 0x18; i++) kprintf(" %02x", t->crtc[i]);
    kprintf("\n");
}

// ---- ONE scaling/blit path -------------------------------------------------
// Every presenter upscales a small guest surface into the host window's
// letterboxed rect with nearest-neighbour sampling. Two ideas made that fast
// (measured on the original mode-13h and EGA loops, #385) and every presenter
// must keep both:
//   ROW REUSE - upscaling repeats source rows (e.g. 400 destination rows from
//     200 source rows repeats every row once), and a repeated row is
//     byte-identical because every term below derives from sy alone, so
//     memcpy beats recomputing it.
//   DIVIDE-FREE X MAPPING - an accumulator reaches the same sx = dx*src_w/dst_w
//     value as a per-pixel divide, without the divide.
// These were previously three independent copies (text/EGA/mode13h). One copy
// now; a fourth presenter gets it by calling these, not by retyping the loop.
typedef struct { int sxi, sxacc, src_w, dst_w; } dos_xscale_t;

static inline void dos_xscale_init(dos_xscale_t *s, int src_w, int dst_w) {
    s->sxi = 0; s->sxacc = 0; s->src_w = src_w; s->dst_w = dst_w;
}
// Returns the next source x (unclamped - callers that add a pixel-pan offset
// must clamp AFTER adding it, exactly as the pre-existing EGA loop did).
static inline int dos_xscale_step(dos_xscale_t *s) {
    int sx = s->sxi;
    s->sxacc += s->src_w;
    while (s->sxacc >= s->dst_w) { s->sxacc -= s->dst_w; s->sxi++; }
    return sx;
}

// Row reuse: if sy is the same source row as the row just written, memcpy it
// and tell the caller to skip recomputing. Returns 1 (row was copied, caller
// should `continue`) or 0 (caller must compute this row, and has now become
// the new "previous row" for the next call).
static inline int dos_row_reuse(int sy, int *prev_sy, uint32_t **prev_row,
                                uint32_t *drow, int sw) {
    if (sy == *prev_sy && *prev_row) {
        memcpy(drow, *prev_row, (size_t)sw * sizeof(uint32_t));
        return 1;
    }
    *prev_sy = sy;
    *prev_row = drow;
    return 0;
}

// Present EGA mode 0Dh: combine the 4 planes into 4-bit pixels, map through the
// attribute-controller palette + DAC, scale into the host window.
static void dos_present_ega(dos_task_t *t, const dos_rect_t *r) {
    int W = t->gfx_w ? t->gfx_w : MODE13_W;
    int H = t->gfx_h ? t->gfx_h : MODE13_H;
    // sw/sh are the SCALED PICTURE, not the buffer: the buffer can be larger
    // (letterbox bars) and its stride is separate.
    int sw = r->w, sh = r->h;
    const int stride = t->win_w;
    uint32_t *base = t->win_buf + (size_t)r->y * stride + r->x;
    // #740: was an inline decode of CRTC 0x13/0x0C/0x0D/0x18/0x07/0x09 and ATC
    // 0x13, now the ONE shared decode (dos_vga_decode_geom, above). #385's
    // original reasoning for WHY these four registers matter (id Galaxy engine
    // virtual-screen scrolling, EGA/VGA split-screen status bars) still holds;
    // it lives in the comment on dos_vga_geom_t now, not duplicated here.
    dos_vga_geom_t geom;
    dos_vga_decode_geom(t, &geom, (uint32_t)(W / 8));
    dos_geom_note("ega", t, &geom);
    // #163: the DISPLAYED height, not the BIOS mode's nominal one. Equal for
    // every mode nobody reprogrammed (checked table by table in
    // dos_vga_rows_selftest_rs); different only for a program that moved the
    // vertical timing, and for that program this is the correct height.
    if (geom.rows) H = (int)geom.rows;
    uint32_t bytes_per_row  = geom.stride_bytes;
    uint32_t line_compare   = geom.line_compare;
    uint32_t pan            = geom.pan_x;
    uint32_t start_off      = geom.start_off;
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
    // ONE scaling/blit path (dos_row_reuse + dos_xscale_t, above): row reuse
    // (740 destination rows from 200 source rows repeats 73% of them) plus a
    // divide-free x mapping.
    int prev_sy = -1;
    uint32_t *prev_row = 0;
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * H / sh;
        if (sy >= H) sy = H - 1;
        uint32_t *drow = base + (size_t)dy * stride;
        if (dos_row_reuse(sy, &prev_sy, &prev_row, drow, sw)) continue;
        // Below the split, the hardware refetches from address 0 with no panning.
        int split = ((uint32_t)sy > line_compare);
        uint32_t rowoff = split ? ((uint32_t)(sy - (int)line_compare - 1) * bytes_per_row)
                                : (start_off + (uint32_t)sy * bytes_per_row);
        uint32_t rowpan = split ? 0u : pan;
        dos_xscale_t xs;
        dos_xscale_init(&xs, W, sw);
        for (int dx = 0; dx < sw; dx++) {
            int sx = dos_xscale_step(&xs) + (int)rowpan;
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
    // (#234b) THE ACTIVE PAGE, not page 0. Hashing page 0 while the guest drew
    // on page 3 is what made this dump report "blank" for a screen that was
    // fully painted, i.e. the diagnostic agreed with the bug.
    uint32_t base = dos_text_cell(t, 0, 0);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 80 * 25 * 2; i += 2) h = (h ^ t->mem[base + i]) * 16777619u;
    if (h == last_hash) return;
    last_hash = h; dumps++;
    kprintf("[dos] --- TEXT PAGE B800 page %u (80x25) dump %d ---\n",
            t->text_page, dumps);
    for (int row = 0; row < 25; row++) {
        char line[81];
        int any = 0;
        for (int col = 0; col < 80; col++) {
            uint8_t ch = t->mem[base + (row * 80 + col) * 2];
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

static void dos_present_text(dos_task_t *t, const dos_rect_t *r) {
    const int LW = TEXT_COLS * FONT_WIDTH;      // 640
    const int LH = TEXT_ROWS * FONT_HEIGHT;     // 400
    int sw = r->w, sh = r->h;
    const int stride = t->win_w;
    uint32_t *base = t->win_buf + (size_t)r->y * stride + r->x;
    // ONE scaling/blit path (dos_row_reuse + dos_xscale_t, above). The cell
    // lookup and glyph fetch are done once per source COLUMN, not once per
    // destination pixel: at 640x400 that is 80 lookups a row, not 640.
    int prev_sy = -1;
    uint32_t *prev_row = 0;
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * LH / sh; if (sy >= LH) sy = LH - 1;
        int row = sy >> 4, gy = sy & 15;
        uint32_t *drow = base + (size_t)dy * stride;
        if (dos_row_reuse(sy, &prev_sy, &prev_row, drow, sw)) continue;
        const uint8_t *cells = &t->mem[dos_text_cell(t, row, 0)];
        int last_col = -1;
        uint8_t bits = 0; uint32_t fg = 0, bg = 0;
        // (#234e) THE CURSOR IS THE SHAPE THE GUEST ASKED FOR, OR NOTHING.
        //
        // This drew a fixed two-scanline underline on the cursor cell whatever
        // INT 10h AH=01h had been told, so a guest that TURNED THE CURSOR OFF
        // (CH bit 5, which every full-screen text program does before it starts
        // drawing) still got one, parked on top of whatever character sat at the
        // cursor position. On a roguelike that position is usually the player,
        // so the one cell you most need to read is the one carrying a stray
        // underline. Reading the shape from the BDA word AH=01h already writes
        // means the two cannot disagree.
        //
        // 0040:0060 = CX as written: CH = start scanline (bit 5 set = cursor
        // hidden), CL = end scanline. A shape a real VGA would not draw
        // (start > end) is also no cursor, which is the documented way some
        // programs hide it without using bit 5.
        uint16_t cshape = rd16(t, 0x0040, 0x0060);
        int cstart = (cshape >> 8) & 0x1F, cend = cshape & 0x1F;
        int chidden = ((cshape >> 8) & 0x20) || cstart > cend;
        if (cend >= FONT_HEIGHT) cend = FONT_HEIGHT - 1;
        int cursor_row = (!chidden && row == t->cur_row &&
                          gy >= cstart && gy <= cend);
        dos_xscale_t xs;
        dos_xscale_init(&xs, LW, sw);
        for (int dx = 0; dx < sw; dx++) {
            int sx = dos_xscale_step(&xs);
            if (sx >= LW) sx = LW - 1;
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

// Paint the letterbox margin.
//
// (#dosfs) NOW CONDITIONAL, and the comment this replaces argued the opposite
// case honestly enough that it is worth saying what changed rather than just
// deleting it. It read: "Unconditional rather than 'only when the geometry
// changed': it is a few percent of the pixels the scale itself writes, and a
// cached-geometry flag is one more piece of state that can be stale after a
// mode change or a rebind. Cheap correctness beats a saved memset."
//
// The premise was "a few percent", and the pixel budget falsifies it: a
// maximised 3840x2160 window now holds a 1920x1200 picture, so the margin is
// 5.99 Mpx against 2.30 Mpx of picture, i.e. 72% of the frame. It is no longer
// a saved memset, it is most of the work.
//
// The staleness worry was the right worry, so the key is EVERY input that can
// change the margin - the buffer, its size, and the picture rectangle - and a
// mode change reaches it through the rectangle, which dos_present_geom()
// recomputes from scratch every frame. The recycled-allocation case that a
// value-only key cannot see is closed at the source: dos_present() clears
// bars_buf when it adopts a buffer from the WM.
static void dos_fill_bars(dos_task_t *t, const dos_rect_t *r) {
    int W = t->win_w, H = t->win_h;
    if (t->bars_buf == t->win_buf && t->bars_w == W && t->bars_h == H &&
        t->bars_pic.x == r->x && t->bars_pic.y == r->y &&
        t->bars_pic.w == r->w && t->bars_pic.h == r->h)
        return;
    t->bars_buf = t->win_buf;
    t->bars_w = W; t->bars_h = H;
    t->bars_pic = *r;
    g_dosv_bars++;
    for (int y = 0; y < H; y++) {
        uint32_t *row = t->win_buf + (size_t)y * W;
        if (y < r->y || y >= r->y + r->h) {
            for (int x = 0; x < W; x++) row[x] = DOS_BAR_ARGB;
        } else {
            for (int x = 0; x < r->x; x++) row[x] = DOS_BAR_ARGB;
            for (int x = r->x + r->w; x < W; x++) row[x] = DOS_BAR_ARGB;
        }
    }
}

// ---- present: 0xB8000 (CGA 04h/05h/06h) -> ARGB host window --------------
// (#212) Marshalling only; the pixel loop and the palette are in
// rustkern/cga.rs. Deliberately ALL the marshalling: every field the loop
// needs is named once, here, so a future CGA-ish mode cannot reach the loop
// without passing this list. Same shape as dos_present_vbe() above.
//
// The source length handed over is the real 16 KB aperture, not the mode's
// nominal frame size, and the Rust side refuses rather than clamps if the
// mode needs more than that: a short read there would be a picture with a
// plausible-looking corner, which is far harder to notice than a blank one.
static void dos_present_cga(dos_task_t *t, const dos_rect_t *r) {
    cga_present_t p;
    p.dst        = t->win_buf;
    p.src        = &t->mem[CGA_B800];
    p.dst_stride = (uint32_t)t->win_w;
    p.dst_x      = (uint32_t)r->x;
    p.dst_y      = (uint32_t)r->y;
    p.dst_w      = (uint32_t)r->w;
    p.dst_h      = (uint32_t)r->h;
    p.src_w      = (uint32_t)(t->gfx_w ? t->gfx_w : 320);
    p.src_h      = (uint32_t)(t->gfx_h ? t->gfx_h : 200);
    p.src_len    = CGA_APERTURE;
    p.mode       = (uint8_t)t->video_mode;
    p.pal_reg    = t->cga_pal;
    p.pad[0] = p.pad[1] = 0;
    cga_present_rs(&p);
}

// ---- present: 0xA0000 (320x200x8, chained/linear) -> ARGB host window -----
// Named "chain4" for the VGA addressing mode (one CPU byte = one pixel,
// unlike dos_present_ega's 4-planes-packed-per-byte), not for the mode
// number: VESA linear 256-colour framebuffers use the identical addressing
// and can call this once vesa's mode-set path lands.
//
// #740: this used to be inlined in dos_present_inner() with a hardcoded
// 320-byte stride read from a hardcoded offset 0, ignoring start address,
// Offset and Line Compare entirely - the asymmetry with dos_present_ega
// (which already honoured all three, #385) that #740 exists to remove. It
// now decodes the same dos_vga_geom_t every presenter uses. For the
// BIOS/game-default case (Offset and start address unprogrammed, Line
// Compare reset to max by the mode-set defaults added above) this is
// BYTE-IDENTICAL to the old hardcoded behaviour, so no shipped title's
// rendering changes unless it actually reprograms these registers - which
// is exactly the case that was previously invisible.
//
// NOT CLAIMED: a hardware-exact byte/word/dword addressing-unit multiplier
// for chain-4 mode. Real VGA silicon scales the Offset/start-address units
// differently depending on the CRTC's byte/word/dword mode bits, and this
// reuses the *2 (word->byte) multiplier already proven correct for EGA's
// planar addressing rather than a guessed chain-4-specific one, because an
// unverified "more accurate" formula risks being wrong in a way that is
// harder to notice than "not supported at all". If a title is found that
// needs a different multiplier here, that is the next measurement to make,
// not a corner to cut silently now.
static void dos_present_chain4(dos_task_t *t, const dos_rect_t *r) {
    int W = t->gfx_w ? t->gfx_w : MODE13_W;
    int H = t->gfx_h ? t->gfx_h : MODE13_H;
    int sw = r->w, sh = r->h;
    const int stride = t->win_w;
    uint32_t *base = t->win_buf + (size_t)r->y * stride + r->x;
    const uint8_t *vga = &t->mem[VGA_A000];
    dos_vga_geom_t geom;
    dos_vga_decode_geom(t, &geom, (uint32_t)MODE13_W);
    dos_geom_note("chain4", t, &geom);
    if (geom.rows) H = (int)geom.rows;   // #163, see dos_present_ega
    uint32_t bytes_per_row = geom.stride_bytes;
    uint32_t line_compare  = geom.line_compare;
    uint32_t start_off     = geom.start_off;
    // Build an ARGB LUT from the 6-bit palette.
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) {
        uint32_t rr = (uint32_t)t->pal[i][0] * 255 / 63;
        uint32_t gg = (uint32_t)t->pal[i][1] * 255 / 63;
        uint32_t bb = (uint32_t)t->pal[i][2] * 255 / 63;
        lut[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
    }
    int prev_sy = -1;
    uint32_t *prev_row = 0;
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * H / sh;
        if (sy >= H) sy = H - 1;
        uint32_t *drow = base + (size_t)dy * stride;
        if (dos_row_reuse(sy, &prev_sy, &prev_row, drow, sw)) continue;
        // Same split-screen handling as dos_present_ega: below Line Compare
        // the hardware refetches from address 0. Chain-4/mode 13h has no
        // sub-pixel pan register worth honouring here (256-colour panning
        // is coarse and unused by the titles in the DOS test corpus), so
        // unlike the EGA path there is no rowpan term.
        int split = ((uint32_t)sy > line_compare);
        uint32_t rowoff = split ? ((uint32_t)(sy - (int)line_compare - 1) * bytes_per_row)
                                : (start_off + (uint32_t)sy * bytes_per_row);
        dos_xscale_t xs;
        dos_xscale_init(&xs, W, sw);
        for (int dx = 0; dx < sw; dx++) {
            int sx = dos_xscale_step(&xs);
            if (sx >= W) sx = W - 1;
            // Masked to the 64KB VGA aperture (0xA0000-0xB0000), same as the
            // EGA path's per-plane mask: a game-reprogrammed start
            // address/Offset must not be able to walk this read past the
            // aperture into the text page or guest code at higher addresses.
            uint32_t bo = (rowoff + (uint32_t)sx) & (EGA_PLANE_SIZE - 1);
            drow[dx] = lut[vga[bo]];
        }
    }
}

// ---- present: 0xA0000 unchained (Mode X) -> ARGB host window --------------
//
// The three things the chained presenter cannot express, all of them the reason
// games used this mode at all:
//   - FOUR PLANES. A pixel at (x, y) is plane x&3, byte y*stride + (x>>2).
//   - PAGE FLIPPING via the display start address, which is why 320x240 (19200
//     bytes a page, three pages inside the 64 KB window) is the mode of choice.
//   - A RESOLUTION THAT IS NOT 320x200, and that no mode number can tell you.
//
// Everything except that resolution comes from the SHARED decode
// (dos_vga_decode_geom) and the SHARED scaler (dos_row_reuse + dos_xscale_t),
// deliberately: this is the fourth presenter in this file and the foundation
// those two pieces were extracted for. Two notes where Mode X differs from the
// shared defaults, both handled here rather than by widening the shared decode
// (which is proven against the EGA path and should not be perturbed):
//
//   STRIDE. `default_stride_bytes` is W/4, not W: a Mode X row is a quarter as
//   many bytes IN EACH PLANE. A 320-wide title that never touches CRTC 0x13
//   (Abrash's 320x240 table does not) lands on 80, which is correct; a 360-wide
//   one that does program it is honoured by the shared decode as-is.
//
//   PIXEL PAN. dos_vga_geom_t.pan_x is ATC 0x13 bits 0-2, which is the PLANAR
//   unit. In a 256-colour mode the same register counts HALF pixels, so the
//   useful values are 0/2/4/6 and the pixel shift is value >> 1. Using the
//   planar figure here would pan up to four times too far.
//
//   START ADDRESS. The shared decode takes CRTC 0x0C:0x0D as a raw byte offset
//   and deliberately applies no addressing-unit multiplier (see its comment).
//   That is EXACTLY right for Mode X and not a compromise: Mode X sets CRTC
//   0x17 bit 6 (byte mode) and clears CRTC 0x14 bit 6 (doubleword mode), which
//   is precisely the configuration in which the counter counts plain bytes.
//   MEASURED: a page flip to 0x4B00 (240 * 80) lands on the second page.
static void dos_present_modex(dos_task_t *t, const dos_rect_t *r,
                              const dos_modex_geom_t *mg) {
    const int W = mg->w, H = mg->h;
    const int sw = r->w, sh = r->h;
    const int stride = t->win_w;
    uint32_t *base = t->win_buf + (size_t)r->y * stride + r->x;
    dos_vga_geom_t geom;
    dos_vga_decode_geom(t, &geom, (uint32_t)(W / 4));
    const uint32_t bytes_per_row = geom.stride_bytes;
    const uint32_t line_compare  = geom.line_compare;
    const uint32_t start_off     = geom.start_off;
    const int pan = (int)((geom.pan_x >> 1) & 3);
    // 256-entry ARGB LUT from the 6-bit DAC, same as the chained path.
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) {
        uint32_t rr = (uint32_t)t->pal[i][0] * 255 / 63;
        uint32_t gg = (uint32_t)t->pal[i][1] * 255 / 63;
        uint32_t bb = (uint32_t)t->pal[i][2] * 255 / 63;
        lut[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
    }
    // EVIDENCE, NOT A GATED DIAGNOSTIC. The obvious home for these two lines
    // was g_x86_dbgring, the "/CONFIG/DOSDIAG.CFG gate" this file names in four
    // comments. MEASURED: that variable is defined once in stubs.c and ASSIGNED
    // NOWHERE IN THE TREE, so everything behind it is dead and no file you can
    // put on the disk turns it on. Shipping a new video mode whose only
    // proof-of-life is behind a dead flag is this project's characteristic
    // failure with a fresh coat on. So: one-shot, unconditional, latched
    // against a 70 Hz present loop. See blame.md.
    {
        static int announced = 0, flipped = 0;
        if (!announced) {
            announced = 1;
            kprintf("[dos] MODE X ACTIVE: %dx%d stride=%u start=0x%x split=0x%x pan=%d\n",
                    W, H, (unsigned)bytes_per_row, (unsigned)start_off,
                    (unsigned)line_compare, pan);
        }
        if (!flipped && start_off != 0) {
            flipped = 1;
            kprintf("[dos] MODE X page flip observed: display start = 0x%x\n",
                    (unsigned)start_off);
        }
    }
    int prev_sy = -1;
    uint32_t *prev_row = 0;
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * H / sh;
        if (sy >= H) sy = H - 1;
        uint32_t *drow = base + (size_t)dy * stride;
        if (dos_row_reuse(sy, &prev_sy, &prev_row, drow, sw)) continue;
        // Below Line Compare the CRTC refetches from address 0 with no panning,
        // exactly as in the EGA and chained paths.
        int split = ((uint32_t)sy > line_compare);
        uint32_t rowoff = split
            ? ((uint32_t)(sy - (int)line_compare - 1) * bytes_per_row)
            : (start_off + (uint32_t)sy * bytes_per_row);
        int rowpan = split ? 0 : pan;
        dos_xscale_t xs;
        dos_xscale_init(&xs, W, sw);
        for (int dx = 0; dx < sw; dx++) {
            int sx = dos_xscale_step(&xs) + rowpan;
            if (sx >= W) sx = W - 1;
            // THE ONE LINE THAT IS MODE X: plane = x & 3, offset = x >> 2.
            // Masked into the 64 KB aperture like every other presenter here,
            // so a guest-programmed start address cannot walk the read out of
            // its own VGA RAM.
            uint32_t bo = (rowoff + (uint32_t)(sx >> 2)) & (EGA_PLANE_SIZE - 1);
            drow[dx] = lut[t->ega_plane[sx & 3][bo]];
        }
    }
}

// (#740) WHICH aspect box the active video mode letterboxes into. Every mode
// today shares one fixed 8:5 box (DOS_SURF_W x DOS_SURF_H): text, mode 13h
// and the EGA planar modes are all letterboxed into it regardless of their
// own native aspect. VESA 640x480 (4:3) and Mode X's 320x240 (4:3) do not
// fit that box, so a future presenter needs a DIFFERENT (aw,ah) - but it
// must NOT get it by calling dos_letterbox_rs() with a second, independently
// maintained box, because dos_letterbox_rs() is deliberately the ONE
// function BOTH the present path (here) and the input path (dos_pump_input,
// below) call (#745 local 105): a cursor at the left edge of the window
// must map to the left edge of the guest screen, and that only holds if
// both paths agree on the box. So this is the ONE place that decides
// (aw,ah) from t->video_mode, called from both present and input, and
// widening it (e.g. a per-mode table) is how a future mode picks its own
// box without the two paths ever being able to disagree.
static void dos_present_aspect(const dos_task_t *t, int32_t *aw, int32_t *ah) {
    // (#740 VESA) A VBE mode's own resolution IS its box: 640x480 is 4:3 and
    // does not fit the fixed 8:5 one, which is precisely the case the comment
    // above anticipated. This is deliberately the widening described there
    // rather than a second box maintained beside it: #740 first carried its own
    // because two independently maintained aspect sources is exactly how the
    // present path and the input path come to disagree about where the picture
    // is, which is the fault dos_letterbox_rs() exists to make impossible.
    if (t->vbe.mode) {
        *aw = (int32_t)t->vbe.width;
        *ah = (int32_t)t->vbe.height;
        return;
    }
    *aw = DOS_SURF_W;
    *ah = DOS_SURF_H;
    // (#740) Mode X is the first mode to need a different box, and this is the
    // seam that was left for it. 320x240 has SQUARE pixels on a 4:3 CRT, which
    // is the entire reason a game chose it over 320x200; presenting it into the
    // 8:5 box would squash the artwork by a fifth. Every chained mode keeps the
    // 8:5 box, so nothing that ships today changes shape by one pixel.
    if (!dos_modex_active(t)) {
        // (#dosfs) The 8:5 box, and the ONE place `aspect=crt` can change it.
        // Placed after the VBE early return above and before the Mode X arm
        // below on purpose: those two modes carry their OWN true aspect
        // (640x480 and 320x240 are 4:3 on square pixels, which is exactly why a
        // game picked them), so correcting them would be a second, wrong
        // correction of something already right. dos_aspect_apply_rs() checks
        // the 8:5 cross-multiply itself and refuses anything else, so this is
        // belt and braces rather than the only guard.
        dos_aspect_apply_rs(g_dos_view.aspect, aw, ah);
        return;
    }
    dos_modex_geom_t mg;
    if (dos_modex_geom_rs(t->crtc, t->seq_reg[4], &mg)
        && mg.aspect_w > 0 && mg.aspect_h > 0) {
        *aw = mg.aspect_w;
        *ah = mg.aspect_h;
    }
}

// (#dosfs) THE GUEST'S OWN RESOLUTION, which is neither the aspect box nor the
// window size. dos_present_aspect() above answers "what SHAPE is the picture";
// this answers "how many pixels does the guest actually have", and the
// integer-scale policy needs both: the shape decides the rectangle, the pixel
// count decides which whole multiples of it exist.
//
// It is ONE function called from ONE place (dos_present_geom, below), for
// exactly the reason dos_present_aspect() is: the draw path and the input path
// must not be able to disagree about where the picture is.
static void dos_native_res(const dos_task_t *t, int32_t *gw, int32_t *gh) {
    if (t->vbe.mode) {
        *gw = (int32_t)t->vbe.width; *gh = (int32_t)t->vbe.height; return;
    }
    if (dos_text_is(t)) {
        // The GLYPH GRID is the native picture here. dos_present_text() scales
        // 80x25 cells of the shared 8x16 font, so 640x400 is what a 1:1 present
        // draws and 2x is whole 8x16 cells - the same arithmetic that
        // dos_run_file() sizes the default window by.
        *gw = TEXT_COLS * FONT_WIDTH; *gh = TEXT_ROWS * FONT_HEIGHT; return;
    }
    if (dos_modex_active(t)) {
        dos_modex_geom_t mg;
        if (dos_modex_geom_rs(t->crtc, t->seq_reg[4], &mg) && mg.w > 0 && mg.h > 0) {
            *gw = mg.w; *gh = mg.h; return;
        }
    }
    *gw = t->gfx_w ? t->gfx_w : MODE13_W;
    *gh = t->gfx_h ? t->gfx_h : MODE13_H;
    // #163: every presenter draws the CRTC's derived row count in preference to
    // the mode's nominal height, so the scale decision must read the SAME
    // number or it would pick a whole multiple of a height nothing draws. The
    // Incredible Machine is the measured case: 400 rows out of a nominal 480,
    // which at a 2.4 Mpx budget is the difference between a 3x and a 2x fit.
    uint32_t rows = dos_vga_rows_rs(t->crtc, (uint32_t)sizeof(t->crtc));
    if (rows) *gh = (int32_t)rows;
}

// (#dosfs) THE ONE geometry call, and the only caller of dos_present_aspect(),
// dos_native_res() and dos_present_rect_rs(). Present and input both come here
// and nowhere else. `cw x ch` is the caller's own idea of the content area
// (the buffer size on the draw path, the WM's content rect on the input path),
// which are the same thing except transiently across a resize.
static int dos_present_geom(const dos_task_t *t, int cw, int ch, dos_rect_t *r) {
    int32_t aw, ah, gw, gh;
    dos_present_aspect(t, &aw, &ah);
    dos_native_res(t, &gw, &gh);
    return dos_present_rect_rs(cw, ch, aw, ah, gw, gh, &g_dos_view, r);
}

// (#740) Present an 8bpp packed VESA mode.
//
// The pixel work is in rustkern/vbe.rs. It was FIRST written here in C on the
// assumption that a per-destination-pixel loop earns the 2026-07-16 rule's
// performance exemption, and then the assumption was MEASURED: the identical
// algorithm compiled with the kernel's own flags came out within 1-4% between
// gcc 12.2 and rustc 1.97.0, and at 1024x768 the Rust was the faster one. No
// measured performance reason means no exemption, so the loop moved. Numbers
// in the CHANGELOG entry for #740.
//
// What is left here is marshalling, and it is deliberately all of it: every
// field the loop needs is named once, in one place, so a future mode with a
// different stride or start cannot reach the pixel loop without passing
// through this list.
static void dos_present_vbe(dos_task_t *t, const dos_rect_t *r) {
    if (!t->vbe_vram || !t->win_buf) return;
    vbe_present_t p;
    p.dst        = t->win_buf;
    p.src        = t->vbe_vram;
    p.pal        = &t->pal[0][0];
    p.dst_stride = t->win_w;
    p.dst_w      = t->win_w;
    p.dst_h      = t->win_h;
    p.x = r->x; p.y = r->y; p.w = r->w; p.h = r->h;
    p.gw         = t->vbe.width;
    p.gh         = t->vbe.height;
    p.src_len    = t->vbe.vram;
    p.bpl        = t->vbe.bpl;
    p.disp_start = t->vbe.disp_start;
    p.dac8       = t->vbe.dac8;
    vbe_present_rs(&p);
}

static void dos_present_inner(dos_task_t *t) {
    if (!t->win_buf) return;
    // The scaled picture's rectangle inside the content buffer. Recomputed
    // every present from the CURRENT buffer size rather than cached, so a
    // resize cannot leave the draw path and the input path disagreeing.
    dos_rect_t r;
    if (!dos_present_geom(t, t->win_w, t->win_h, &r)) return;
    g_dosv_pw = r.w; g_dosv_ph = r.h;
    if (g_x86_dbgring) {
        // DOSDIAG-gated, and only when the geometry actually changes: the one
        // line that says where the picture is, so a resize can be checked
        // against the screen instead of inferred from it.
        static int lw = -1, lh = -1;
        if (lw != t->win_w || lh != t->win_h) {
            lw = t->win_w; lh = t->win_h;
            int ox, oy, ow, oh;
            if (win16_host_content_rect(t->host_slot, &ox, &oy, &ow, &oh) != 0)
                ox = oy = ow = oh = -1;
            kprintf("[dos] present buf=%dx%d pic=%d,%d,%dx%d content=%d,%d,%dx%d\n",
                    t->win_w, t->win_h, r.x, r.y, r.w, r.h, ox, oy, ow, oh);
        }
    }
    dos_fill_bars(t, &r);
    // Mode 3 is the power-on default, so a program that never calls INT 10h
    // set-mode leaves video_mode at 0x00 and the dump used to never fire for
    // exactly the programs that most needed it.
    if (g_x86_dbgring && dos_text_is(t))
        dos_dump_text_page(t);
    // (#740) Checked BEFORE every VGA path, and it is the only test: a VESA
    // mode owns the screen outright, so no pre-existing path changes behaviour.
    if (t->vbe.mode) { dos_present_vbe(t, &r); return; }
    if (dos_text_is(t)) { dos_present_text(t, &r); return; }
    if (t->video_mode == 0x0D || t->video_mode == 0x0E ||
        t->video_mode == 0x10 || t->video_mode == 0x12) {
        dos_present_ega(t, &r);
        return;
    }
    // (#212) CGA graphics. Placed after the EGA planar arm and before the
    // mode-13h-only bail that used to swallow these three modes entirely.
    if (t->video_mode == 0x04 || t->video_mode == 0x05 || t->video_mode == 0x06) {
        dos_present_cga(t, &r);
        return;
    }
    if (t->video_mode != 0x13) return;
    // (#740) Unchained? Then the flat 64000-byte buffer dos_present_chain4()
    // draws is not where the picture is; the four planes are.
    // dos_modex_geom_rs() returns 0 for a half-programmed CRTC, in which case
    // we fall through to the chained view for that frame rather than draw a
    // garbage resolution.
    {
        dos_modex_geom_t mg;
        if (dos_modex_active(t) && dos_modex_geom_rs(t->crtc, t->seq_reg[4], &mg)) {
            dos_present_modex(t, &r, &mg);
            return;
        }
    }
    dos_present_chain4(t, &r);
}

// (#745 local 105) The buffer handover, and why it is not just a pointer store.
//
// dos_host_rebind() runs on the WINDOW MANAGER's thread while this thread may
// be part-way through writing a frame into the buffer being replaced. Storing
// the new pointer there and letting the WM free the old one immediately would
// leave a narrower version of the very use-after-free being fixed: a present
// takes single-digit milliseconds at a maximised size, so a resize would land
// inside one often, not rarely.
//
// So the swap happens HERE, at a point where this thread provably is not
// blitting: a present adopts any pending buffer before it draws anything, and
// frees any superseded buffer the WM handed over. The WM only hands one over
// when it saw `presenting`; otherwise it frees its own, which is safe for the
// same reason - outside this window, nothing on this thread touches win_buf.
//
// The lock is the shared irqsave spinlock (sync/spinlock.h), held for a few
// assignments and never across kfree(), so it never nests inside the heap lock.
static spinlock_t g_dos_win_lock = SPINLOCK_INIT;

// (#175) The honesty line, reported ONCE per guest, from the frame path.
//
// NOT gated on a diagnostic file: "your game is programming an FM chip that is
// not there, so its music is silent" is a user-facing fact, not a debug trace,
// and it is the single thing that stops silent-music from being unexplainable.
//
// The threshold separates a DETECTION PROBE from a MUSIC DRIVER. The canonical
// AdLib probe writes six registers (MEASURED: Keen 5 does exactly 6, Monkey
// Island 7). A driver that is actually playing blows past 64 immediately
// (MEASURED: SkyRoads writes 2389). So a game that correctly detects nothing
// and stays quiet says nothing here, and only a game that really is playing
// into the void reports.
static void dos_opl2_report_silence(dos_task_t *t) {
    if (t->opl2_reported) return;
    uint32_t fmw = dos_opl2_writes_rs(&t->opl2);
    if (fmw < 64) return;
    t->opl2_reported = 1;
    // The wording has to be true in BOTH arms, and the two arms are silent for
    // DIFFERENT reasons: with no chip the writes go to an empty socket, with
    // the diagnostic arm forced on they are accepted and then not sounded. A
    // single message that said "empty socket" would be a false statement half
    // the time, which is the exact failure this line exists to prevent.
    // (#182) THREE ARMS NOW, NOT TWO, and the old wording was true in none of
    // them once FM synthesis existed. It said "this kernel implements AdLib
    // DETECTION only", which stopped being true, and it declared the silence
    // "by design", which would actively mislead someone debugging a bridge that
    // had broken. Each arm now says what is actually the case.
    if (t->opl2.installed && g_dos_fm_ready) {
        kprintf("[dos] (#182) guest has issued %u OPL2 FM register writes; they were "
                "carried to the Ring-3 synthesiser. If this title is silent, the "
                "fault is in the bridge or the synthesiser, NOT by design.\n", fmw);
    } else if (t->opl2.installed) {
        kprintf("[dos] (#182) guest has issued %u OPL2 FM register writes and the chip "
                "reports PRESENT via the DOSOPL.CFG override, but NO Ring-3 "
                "synthesiser is running, so this title's music IS silent. That "
                "combination is a diagnostic arm, not a shipping state.\n", fmw);
    } else {
        kprintf("[dos] (#182) guest has issued %u OPL2 FM register writes into an empty "
                "socket; they were discarded. The chip truthfully reported ABSENT, "
                "so this title's music is silent by the guest's own choice.\n", fmw);
    }
}

// (#176) Every 5 s of wall clock, from the frame path, in EVERY arm including
// the shipped one.
//
// It is deliberately not behind a /CONFIG gate. The one number that decides
// whether charging port I/O is safe for a given title is its port accesses per
// second, and a diagnostic that can only be read by someone who already
// suspected there was a problem is how #175's silence counter came to be dead
// code in the only configuration that ships.
//
// THE INFLATION FIGURE IS THE POINT. Emulated time advances at about PIT_HZ
// ticks per real second by construction, so the fraction of the guest's clock
// that came from the bus rather than from instructions is directly how much
// faster than real time this title's clock now runs. Printed as a permille so
// a 1-in-1000 effect is still visible as a digit.
static void dos_iocost_periodic(dos_task_t *t) {
    static uint64_t last_ms = 0;
    static uint64_t last_acc = 0, last_ticks = 0, last_pit = 0;
    static unsigned long last_insn = 0;
    static uint32_t last_sat = 0;
    static uint32_t last_push = 0;   // (#187) previous g_dos_fmq.n_pushed
    uint64_t now = sched_now_ms();
    if (!last_ms) {
        last_ms = now; last_acc = t->bus.n_access;
        last_ticks = t->bus.ticks_charged; last_pit = dos_emu_pit_now(t);
        last_insn = dos_emu_insns(t);
        last_sat = t->bus_saturated;
        dos_fmq_host_stats(&last_push, 0, 0, 0);   // (#187)
        return;
    }
    if (now - last_ms < 5000) return;
    uint64_t dms   = now - last_ms;
    uint64_t dacc  = t->bus.n_access - last_acc;
    uint64_t dtick = t->bus.ticks_charged - last_ticks;
    uint64_t pnow  = dos_emu_pit_now(t);
    uint64_t dpit  = pnow - last_pit;
    // Guard the divisions rather than assume forward motion: a guest parked in
    // a HLT retires no instructions, so dpit can legitimately be the bus term
    // alone, or zero.
    unsigned long inow = dos_emu_insns(t);
    unsigned long dins = inow - last_insn;
    uint32_t dsat = t->bus_saturated - last_sat;
    char satbuf[96];
    const char *satmsg = "";
    if (dsat) {
        // Rendered here rather than as a bare count in the format string so the
        // line SAYS what the number means. A reader who has not read this
        // function should not have to guess what "sat=3" implies about the
        // guest's clock.
        snprintf(satbuf, sizeof(satbuf),
                  " BUS-SATURATED in %u of the last rate windows "
                  "(clock ahead of real time)", dsat);
        satmsg = satbuf;
    } else if (t->bus_saturated) {
        satmsg = " (bus saturated earlier in this run, not now)";
    }
    uint32_t per_s  = dms ? (uint32_t)((dacc * 1000ull) / dms) : 0;
    uint32_t permil = dpit ? (uint32_t)((dtick * 1000ull) / dpit) : 0;
    uint32_t kips   = dms ? (uint32_t)(((uint64_t)dins) / dms) : 0;  // insn per ms == kinsn/s
    // THE SATURATION LINE IS NOT DECORATION. An 8-bit ISA bus carries about
    // 1e6 accesses per second. A title above that is running a poll no real
    // machine could run that fast, so its emulated clock is genuinely ahead of
    // real time for that window and the compensation in dos_emu_clock_rate()
    // could not fix it: the bus term alone already overflows the second.
    // Saying so is the difference between a known limit and an unexplained
    // speed-up someone spends a day on later.
    kprintf("[IOCOST] cost=%uns io=%lu (+%lu in %lums = %u/s) charged=+%lu of "
            "%lu ticks (%u permille) guest=%u kinsn/s%s\n",
            t->bus.ns_per_access,
            (unsigned long)t->bus.n_access, (unsigned long)dacc,
            (unsigned long)dms, per_s,
            (unsigned long)dtick, (unsigned long)dpit, permil, kips,
            satmsg);
    // (rakbd) WHERE THE GUEST LOOKS FOR THE KEYBOARD, as a number.
    //
    // "It ignores every key" has at least four distinct causes (no scancode
    // reaching the ring, no ISR installed, an ISR we cannot reach, or a guest
    // polling a controller we answer wrongly) and they need opposite fixes.
    // These two counters separate the polled path from the rest in one line:
    // a guest that polls the 8042 shows thousands of p60/p64 reads, and a guest
    // that uses INT 16h shows zero. Guessing which one Red Alert was cost most
    // of this ticket.
    // (rakbd) ONLY WHEN IT CHANGES. A line every 5 s for every DOS guest is
    // noise on a shipping golden; a line only when one of these counters moves
    // is a keyboard trace. The FIRST call always prints, so "all zero" - the
    // answer for Red Alert, and the whole finding - is stated rather than
    // inferred from an absent line. That distinction is what cost this ticket
    // its longest detour.
    {
        static uint32_t l60, l64, l16, lpush; static int said;
        if (said && l60 == t->p60_reads && l64 == t->p64_reads &&
            l16 == t->int16_calls && lpush == t->keyq_pushes) goto kbdio_done;
        said = 1; l60 = t->p60_reads; l64 = t->p64_reads;
        l16 = t->int16_calls; lpush = t->keyq_pushes;
    }
    // (rakbd2) int9_pm is here because its ABSENCE is what made the previous
    // reading of this line wrong. "int9_hooked=0" was true and complete for the
    // low table and said nothing about the 0205h table, so a guest that had
    // installed a keyboard ISR read as a guest that had not.
    kprintf("[KBDIO] port60=%lu port64=%lu int16=%lu ringpush=%lu int9_hooked=%d "
            "int9_pm=%d bda_head=%04x bda_tail=%04x\n",
            (unsigned long)t->p60_reads, (unsigned long)t->p64_reads,
            (unsigned long)t->int16_calls, (unsigned long)t->keyq_pushes,
            t->kbd_has_int9, t->kbd_int9_pm,
            rd16(t, BDA_SEG, BDA_KB_HEAD), rd16(t, BDA_SEG, BDA_KB_TAIL));
kbdio_done: ;
    // (#187) FM QUEUE PRESSURE, on the SAME 5 s cadence and the same frame
    // path, so it adds no timer and no waiting.
    //
    // It answers the question #182 left open. The ring DROPS THE NEWEST event
    // on overflow, which is the right policy for state transitions, but a drop
    // policy nobody has ever observed in a real title is a comment, not a fact.
    // peak is the high-water depth out of DOS_FMQ_CAP; drop is how much music
    // was actually lost. A run that ends with peak far below capacity and
    // drop=0 is the evidence that the queue is correctly sized, and a run that
    // drops says so in the SHIPPED arm rather than only under a diagnostic gate.
    //
    // pushed is the guest's cumulative OPL2 register-write count. It is printed
    // because the once-only silence report latches after the first 64 writes,
    // so without this there is NO ONGOING WAY to tell a title that is playing
    // music from one that merely loaded an instrument bank and then went quiet.
    // Those two look IDENTICAL in every other counter on this line, and the
    // difference is the whole cost being measured.
    {   uint32_t q_push = 0, q_drop = 0, q_peak = 0, q_used = 0;
        dos_fmq_host_stats(&q_push, &q_drop, &q_peak, &q_used);
        // (#252) i15w/i15us ride along here rather than getting a line of
        // their own, because the question they answer is always asked next to
        // this one: "the guest is writing FM registers, is its BIOS wait
        // actually charging it any time?". A zero i15us beside a non-zero
        // pushed count is the pre-#252 state exactly.
        kprintf("[FMQ] pushed=%u (+%u in %lums) drop=%u peak=%u/%u now=%u "
                "i15w=%u i15us=%lu\n",
                q_push, q_push - last_push, (unsigned long)dms,
                q_drop, q_peak, (unsigned)dos_fmq_host_capacity(), q_used,
                (unsigned)g_dos_int15_waits, (unsigned long)g_dos_int15_wait_us);
        last_push = q_push;
    }
    last_ms = now; last_acc = t->bus.n_access;
    last_ticks = t->bus.ticks_charged; last_pit = pnow; last_insn = inow;
    last_sat = t->bus_saturated;
}

// (#175) Every 5s of wall clock, while the trace is armed. Piggybacks on the
// present cadence rather than adding a timer of its own; adds no waiting.
static void dos_iotrace_periodic(void) {
    if (!g_dos_iotrace) return;
    static uint64_t last_ms = 0;
    uint64_t now = sched_now_ms();
    if (last_ms && now - last_ms < 5000) return;
    last_ms = now;
    dos_iotrace_dump();
}

// #232 GUEST REDRAW COUNTER, the frame-rate instrument.
//
// "The game runs too fast" is a claim about FRAMES, so it has to be answered in
// frames, and the guest's instruction rate alone is only a proxy for that. This
// counts how many times the guest's VIDEO MEMORY actually changed between one
// present and the next, i.e. how many distinct pictures the guest produced -
// which for a game whose loop redraws every iteration IS its frame rate, up to
// the DOS_PRESENT_MS ceiling of ~70/s. A reading AT the ceiling means only
// ">= 70", so measure at a low cap (where the answer is unambiguous) and scale:
// the gameplay loop has no wait in it (PROVEN by disassembly, see the CPU-cap
// block near the top of this file), so frames are exactly proportional to
// instructions.
//
// Gated on /CONFIG/DOSSPEED.CFG so the golden pays nothing: it is a strided
// 32-bit FNV over the two video apertures, which is cheap but not free, and it
// runs at the present cadence.
static unsigned long g_dos_redraw_n = 0;
static uint32_t      g_dos_redraw_h = 0;

// A change detector, not a checksum, so it samples rather than reads every
// byte. A fixed SAMPLE COUNT rather than a fixed stride, because the buffers
// below differ by 16x in size and a stride tuned for 128 KB would either crawl
// over 1 MB of VESA VRAM or skip most of a 64 KB plane.
#define DOS_REDRAW_SAMPLES 32768u

static void dos_redraw_hash(uint32_t *h, const uint8_t *p, uint32_t len) {
    if (!p || !len) return;
    uint32_t stride = len / DOS_REDRAW_SAMPLES;
    if (stride < 4) stride = 4;   // any redraw touching a sprite touches >> 4 bytes
    for (uint32_t a = 0; a < len; a += stride)
        *h = (*h ^ p[a]) * 16777619u;
}

// (no-ticket) HASH WHERE THE PIXELS ACTUALLY ARE, NOT WHERE THEY USED TO BE.
//
// This hashed t->mem[0xA0000..0xC0000] unconditionally. That is the real-mode
// VGA aperture, and it is the right buffer for exactly TWO of the five modes
// this file presents - chained mode 13h, and text. It is the WRONG buffer for
// the other three, silently:
//
//   VESA          the pixels are in t->vbe_vram. dos_vga_write() stores there
//                 and RETURNS; it never touches t->mem in a VBE mode at all.
//   EGA planar
//   and Mode X    the pixels are in t->ega_plane[0..3].
//
// So for a VESA guest this was hashing a region the guest never writes:
// constant for the entire run, and the counter therefore reported ZERO frames,
// forever, while looking like a working instrument. #232 was developed against
// Joust, which is chained mode 13h, so nothing ever exercised the other arms.
//
// IT COST THIS TICKET A WRONG CONCLUSION. Discworld II's instruction rate
// (41-47 M/s) was measured, looked healthy, and was reported as evidence that
// the guest was fine - while the one instrument that could have contradicted
// it was structurally incapable of reporting anything. An instruction rate is
// not a frame rate for a VESA guest: a single `rep movsd` retires as ONE
// instruction and can carry up to 16,384 per-byte calls into dos_vga_write().
//
// The dispatch below is in the SAME ORDER as dos_present_inner's, because the
// two answer the same question - where is the picture - and a second ordering
// is how they come to disagree.
// (dosplay 2026-08-28) CALLED ON THE PRESENT *CADENCE*, NOT FROM dos_present().
//
// This used to be the first line of dos_present(), which was correct while
// every 14 ms tick presented. dos_frame_due() (rustkern/dosdisp.rs) can now
// DECLINE a present, so the sampler inherited the DISPLAY's rate and stopped
// reporting the GUEST's. MEASURED on golden 2267, Aladdin on its animated title
// screen with the compositor frozen: the guest was publishing at the 200 ms
// staleness floor, and this counter dutifully read "4.9 redraw/s" for a guest
// that was retiring 20.7 M instructions a second. That is the same shape of
// fault dos_view_report() records against itself two screens up - an instrument
// that quietly starts measuring something else the moment the thing upstream of
// it changes - and it is worse here, because a frame RATE is the exact number
// the "is the guest slow, or is the picture slow" question turns on.
//
// So the sampler keeps its own 14 ms clock and runs whether or not the frame is
// presented. It stays gated on /CONFIG/DOSSPEED.CFG (the golden pays nothing)
// and it is the same strided FNV as before: no new logic, only a new call site.
static void dos_redraw_sample(dos_task_t *t) {
    if (!g_dos_speedlog || !t->mem) return;
    uint32_t h = 2166136261u;
    if (t->vbe.mode && t->vbe_vram) {
        // The VISIBLE page, not all of VRAM: 4F07h can park several pages in a
        // 1 MB buffer and a change to one that is not being displayed is not a
        // frame. Same bytes dos_present_vbe() reads.
        uint32_t start = t->vbe.disp_start;
        if (start < t->vbe.vram) {
            uint32_t vis = t->vbe.bpl * (uint32_t)t->vbe.height;
            if (vis > t->vbe.vram - start) vis = t->vbe.vram - start;
            dos_redraw_hash(&h, t->vbe_vram + start, vis);
        }
    } else if (t->video_mode == 0x0D || t->video_mode == 0x0E ||
               t->video_mode == 0x10 || t->video_mode == 0x12 ||
               dos_modex_active(t)) {
        for (int pl = 0; pl < 4; pl++)
            dos_redraw_hash(&h, t->ega_plane[pl], EGA_PLANE_SIZE);
    } else {
        dos_redraw_hash(&h, &t->mem[0xA0000], 0xC0000u - 0xA0000u);
    }
    if (h != g_dos_redraw_h) { g_dos_redraw_h = h; g_dos_redraw_n++; }
}

// Mean and worst-case microseconds per present, the picture size that produced
// them, and the fraction of ONE CORE the present path is using at the current
// cadence - because on the target machine there IS one core
// (g_smp_user_sched = 0) and a per-frame microsecond figure means nothing until
// it is divided by the frame interval.
//
// CALLED FROM dos_present(), NOT FROM THE 16-BIT RUN LOOP, and that is the
// whole point of it being a function. It was first written inline beside the
// #232 speed line in dos_run_file()'s loop, and MEASURED on Red Alert
// (/DOS/RA/GAME.DAT) that reported NOTHING AT ALL: a DOS/4GW guest is an LE
// module driven by its own 32-bit loop, which presents through the same
// dos_present() but never reaches the 16-bit loop's reporting block. The
// instrument was therefore blind to exactly the "Red Alert era" guests the
// pixel budget was asked for. Every present path calls dos_present(); nothing
// else is common to all of them.
static void dos_view_report(void) {
    if (!g_dos_speedlog || !g_dosv_n) return;
    uint64_t now = sched_now_ms();
    if (g_dosv_report_ms && now - g_dosv_report_ms < DOS_SPEED_REPORT_MS) return;
    uint64_t prev = g_dosv_report_ms;
    g_dosv_report_ms = now;
    uint64_t mean = g_dosv_us_tot / g_dosv_n;
    // Permille of one core, integer (the kernel is -mno-sse and a %f in a
    // kprintf silently prints 0.00).
    //
    // MEASURED AGAINST THE REAL INTERVAL, NOT AGAINST DOS_PRESENT_MS. This
    // divided the mean present cost by the 14 ms cadence, which assumed a
    // present every 14 ms. That was true while nothing could decline one; since
    // dos_frame_due() can, the assumption became a fabrication, and it was
    // caught reporting "5.7% of one core" for a guest that had presented TEN
    // times in two seconds and was actually using 0.4%. An instrument that
    // states a rate it did not measure will lie the moment anything upstream of
    // it changes, which is exactly what happened here to the instrument's own
    // author. It now divides the ACTUAL total by the ACTUAL elapsed time.
    uint64_t iv_ms = (prev && now > prev) ? (now - prev) : (uint64_t)DOS_SPEED_REPORT_MS;
    uint64_t permille = (g_dosv_us_tot * 1000ull) / (iv_ms * 1000ull);
    kprintf("[dos] #dosfs present: %llu us mean, %llu us max, %llu presents, "
            "pic=%dx%d (%llu kpx), bars painted %llu since launch, "
            "%llu.%llu%% of one core over the last %llu ms\n",
            (unsigned long long)mean,
            (unsigned long long)g_dosv_us_max,
            (unsigned long long)g_dosv_n,
            g_dosv_pw, g_dosv_ph,
            (unsigned long long)(((uint64_t)g_dosv_pw * (uint64_t)g_dosv_ph) / 1000ull),
            (unsigned long long)g_dosv_bars,
            (unsigned long long)(permille / 10ull),
            (unsigned long long)(permille % 10ull),
            (unsigned long long)iv_ms);
    g_dosv_us_tot = 0; g_dosv_us_max = 0; g_dosv_n = 0;
}

// ---- #dw2perf: WHERE THE DOS FRAME'S TIME ACTUALLY GOES -------------------
//
// The accumulators and their logic live in rustkern/dosprof.rs (2026-07-16
// rule). What is here is the SEAM: the phase boundaries only exist in this
// file's two run loops, and a timestamp taken anywhere else would be timing a
// different thing. Every microsecond the DOS thread spends is charged to
// exactly one bucket, and what is left over is printed as `resid` rather than
// hidden, so the profile can admit to being incomplete.
//
// Gated on /CONFIG/DOSSPEED.CFG, the gate the #232 speed line already uses:
// with it absent (the golden) dosprof_t0() returns 0 and every t1() is a
// single predictable branch.
extern void dosprof_add_rs(uint32_t bucket, uint64_t us);
extern void dosprof_add_publish_bytes_rs(uint64_t bytes);
#define DOSPROF_INTERP  0u
#define DOSPROF_PRESENT 1u
#define DOSPROF_PUBLISH 2u
#define DOSPROF_INPUT   3u
#define DOSPROF_YIELD   4u
#define DOSPROF_NBUCK   5u
typedef struct {
    uint64_t us[DOSPROF_NBUCK];
    uint64_t n[DOSPROF_NBUCK];
    uint64_t max_us[DOSPROF_NBUCK];
    uint64_t publish_bytes;
} dosprof_report_t;
_Static_assert(sizeof(dosprof_report_t) == 8u * (3u * DOSPROF_NBUCK + 1u),
               "dosprof_report_t must match rustkern/dosprof.rs DosProfReport");
extern int dosprof_report_rs(dosprof_report_t *out);
extern int dosprof_selftest_rs(void);

static inline uint64_t dosprof_t0(void) {
    return (g_dos_speedlog && mono_ready()) ? mono_us() : 0;
}
// t0 == 0 means "not armed", which is also the only value mono_us() can never
// legitimately return at a point we would want to measure, so one test covers
// both the gate and a clock that is not ready yet.
static inline void dosprof_t1(uint32_t bucket, uint64_t t0) {
    if (t0) dosprof_add_rs(bucket, mono_us() - t0);
}

// Present unless the compositor has not yet shown the frame we published last.
// `force` covers the two cases where a frame is not optional: a pending content
// buffer from a resize (which also carries the surround repaint), and a halted
// guest whose current picture is the last one it will ever draw.
static inline int dos_frame_due(dos_task_t *t, uint64_t now_ms, int force) {
    return dosdisp_should_present_rs(&g_dosdisp, g_dos_view.frameskip,
                                     (force || t->pend_new) ? 1 : 0,
                                     win16_host_flip_count(), now_ms);
}

static uint64_t g_dosprof_wall0;

// ONE LINE, not one per bucket. A profile spread over six serial lines cannot
// be read as a table at a glance, and on a busy log the lines get separated by
// other subsystems' output and stop being one measurement.
// Takes the task because the two numbers everyone actually wants - the guest's
// instruction rate and its FRAME rate - are per-guest, and until now the frame
// rate was printed only on the 16-bit loop's #232 line. A DOS/4GW guest
// therefore reported no frame rate at all, anywhere, which is why nobody had
// ever measured Discworld II's.
static void dos_prof_report(dos_task_t *t) {
    if (!g_dos_speedlog || !mono_ready()) return;
    uint64_t now = mono_us();
    if (!g_dosprof_wall0) { g_dosprof_wall0 = now; return; }
    if (now - g_dosprof_wall0 < (uint64_t)DOS_SPEED_REPORT_MS * 1000ull) return;
    uint64_t wall = now - g_dosprof_wall0;
    g_dosprof_wall0 = now;
    dosprof_report_t r;
    if (dosprof_report_rs(&r) != 0) return;
    if (!wall) return;
    uint64_t sum = 0;
    for (unsigned i = 0; i < DOSPROF_NBUCK; i++) sum += r.us[i];
    // Permille, integer: the kernel is -mno-sse and a %f in a kprintf silently
    // prints 0.00 (the same reason dos_view_report() does this).
    uint64_t pm[DOSPROF_NBUCK];
    for (unsigned i = 0; i < DOSPROF_NBUCK; i++) pm[i] = (r.us[i] * 1000ull) / wall;
    uint64_t resid = (sum < wall) ? (wall - sum) : 0;
    uint64_t rpm   = (resid * 1000ull) / wall;
    // Publish bandwidth in KB/s, so the figure is comparable with FLIPPROF's.
    uint64_t kbs = (r.publish_bytes / 1024ull) * 1000000ull / wall;
    // Read-and-reset, so this line describes its own interval like every other
    // number on it. Straight field reads: single writer, this thread.
    uint64_t d_pres = g_dosdisp.presented, d_skip = g_dosdisp.skipped;
    g_dosdisp.presented = 0; g_dosdisp.skipped = 0;
    // THE GUEST'S OWN NUMBERS, from the ONE accessor that knows which
    // interpreter is running (dos_emu_insns), so this cannot disagree with the
    // emulated clock about whose instructions they are.
    static unsigned long s_insn0; static unsigned long s_redraw0;
    unsigned long insn_now = t ? dos_emu_insns(t) : 0;
    unsigned long d_insn = (insn_now >= s_insn0) ? (insn_now - s_insn0) : 0;
    s_insn0 = insn_now;
    unsigned long d_redraw = (g_dos_redraw_n >= s_redraw0)
                             ? (g_dos_redraw_n - s_redraw0) : 0;
    s_redraw0 = g_dos_redraw_n;
    uint64_t insn_s   = (uint64_t)d_insn   * 1000000ull / wall;
    // Tenths of a frame per second: Discworld II on the owner's hardware is
    // reported at one frame every 3-5 seconds, i.e. 0.2-0.3, and an integer
    // frames-per-second would print that as 0.
    uint64_t redraw_ds = (uint64_t)d_redraw * 10000000ull / wall;
    kprintf("[DOSFRAME] wall=%llums | interp %llu.%llu%% (%lluus n=%llu max=%llu) "
            "| present %llu.%llu%% (%lluus n=%llu max=%llu) "
            "| publish %llu.%llu%% (%lluus n=%llu max=%llu %lluKB/s) "
            "| input %llu.%llu%% | yield %llu.%llu%% | resid %llu.%llu%% "
            "| frames shown %llu skipped %llu "
            "| GUEST %llu insn/s, %llu.%llu redraw/s\n",
            (unsigned long long)(wall / 1000ull),
            (unsigned long long)(pm[DOSPROF_INTERP] / 10ull),  (unsigned long long)(pm[DOSPROF_INTERP] % 10ull),
            (unsigned long long)r.us[DOSPROF_INTERP],  (unsigned long long)r.n[DOSPROF_INTERP],  (unsigned long long)r.max_us[DOSPROF_INTERP],
            (unsigned long long)(pm[DOSPROF_PRESENT] / 10ull), (unsigned long long)(pm[DOSPROF_PRESENT] % 10ull),
            (unsigned long long)r.us[DOSPROF_PRESENT], (unsigned long long)r.n[DOSPROF_PRESENT], (unsigned long long)r.max_us[DOSPROF_PRESENT],
            (unsigned long long)(pm[DOSPROF_PUBLISH] / 10ull), (unsigned long long)(pm[DOSPROF_PUBLISH] % 10ull),
            (unsigned long long)r.us[DOSPROF_PUBLISH], (unsigned long long)r.n[DOSPROF_PUBLISH], (unsigned long long)r.max_us[DOSPROF_PUBLISH],
            (unsigned long long)kbs,
            (unsigned long long)(pm[DOSPROF_INPUT] / 10ull),   (unsigned long long)(pm[DOSPROF_INPUT] % 10ull),
            (unsigned long long)(pm[DOSPROF_YIELD] / 10ull),   (unsigned long long)(pm[DOSPROF_YIELD] % 10ull),
            (unsigned long long)(rpm / 10ull), (unsigned long long)(rpm % 10ull),
            (unsigned long long)d_pres, (unsigned long long)d_skip,
            (unsigned long long)insn_s,
            (unsigned long long)(redraw_ds / 10ull),
            (unsigned long long)(redraw_ds % 10ull));
}

static void dos_present(dos_task_t *t) {
    // dos_redraw_sample() used to be here; it is now on its own 14 ms cadence in
    // both run loops, so that it measures the GUEST's frame rate rather than the
    // display's. See the comment on dos_redraw_sample().
    dos_iotrace_periodic();
    dos_iocost_periodic(t);   // (#176)
    dos_opl2_report_silence(t);
    uint32_t *tofree[DOS_PEND_FREE_MAX];
    int nfree = 0;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_win_lock);
    if (t->pend_new) {
        t->win_buf = t->pend_buf;
        t->win_w   = t->pend_w;
        t->win_h   = t->pend_h;
        t->pend_new = 0;
        // (#dosfs) A FRESH BUFFER HAS UNDEFINED CONTENTS, so the surround must
        // be repainted into it even if the pointer, the size and the picture
        // rectangle all happen to match what was painted last time. kmalloc can
        // and does hand back an address it just freed; keying the bar cache on
        // the pointer alone would then skip the paint and leave whatever the
        // previous owner of that memory wrote showing around the picture.
        t->bars_buf = 0;
    }
    for (int i = 0; i < t->pend_free_n && nfree < DOS_PEND_FREE_MAX; i++)
        tofree[nfree++] = t->pend_free[i];
    t->pend_free_n = 0;
    t->presenting  = 1;
    spinlock_release_irqrestore(&g_dos_win_lock, fl);

    for (int i = 0; i < nfree; i++) kfree(tofree[i]);

    uint64_t _pt0 = mono_ready() ? mono_us() : 0;
    dos_present_inner(t);
    if (_pt0) {
        uint64_t d = mono_us() - _pt0;
        g_dosv_us_tot += d;
        if (d > g_dosv_us_max) g_dosv_us_max = d;
        g_dosv_n++;
    }
    dos_view_report();

    fl = spinlock_acquire_irqsave(&g_dos_win_lock);
    t->presenting = 0;
    spinlock_release_irqrestore(&g_dos_win_lock, fl);
}

// ---- input forwarding: kernel cursor/keys -> DOS mouse state -------------
static void dos_pump_input(dos_task_t *t) {
    int ox, oy, ow, oh;
    if (win16_host_content_rect(t->host_slot, &ox, &oy, &ow, &oh) == 0 && ow > 0 && oh > 0) {
        // (#745 local 105) THE SAME RECTANGLE THE PICTURE IS DRAWN INTO. This
        // used to map the whole content area onto the guest screen, which was
        // right only while the two were the same thing. Once the picture is
        // letterboxed inside a resized window, a cursor at the left edge of the
        // window is NOT at the left edge of the guest screen, and pointing at a
        // menu item would land somewhere else entirely. Both paths call
        // dos_letterbox_rs so they cannot drift apart, and both derive the
        // (aw,ah) box from the SAME dos_present_aspect() (#740) so a future
        // mode with a different aspect cannot desync present from input
        // either.
        dos_rect_t r;
        if (!dos_present_geom(t, ow, oh, &r)) return;
        int cx = (int)mouse_x - ox - r.x;
        int cy = (int)mouse_y - oy - r.y;
        if (cx < 0) cx = 0;
        if (cx >= r.w) cx = r.w - 1;
        if (cy < 0) cy = 0;
        if (cy >= r.h) cy = r.h - 1;
        // #163: MAP INTO THE RANGE THE GUEST ASKED FOR, not a hardcoded one.
        //
        // This mapped the picture onto 0..639 x 0..199 unconditionally, which is
        // the DEFAULT virtual screen and is correct only until a program sets
        // its own with INT 33h 07h/08h. MEASURED on The Incredible Machine: it
        // asks for 0..2556 x 0..1916. With the hardcoded mapping the cursor
        // could only ever reach the top-left quarter of what the game believes
        // the screen is, so even a working event path would have delivered
        // coordinates that are wrong everywhere except the origin. The range
        // defaults to 0..639 x 0..199 at launch, so a program that never sets
        // one sees exactly the previous behaviour.
        int rngx = t->mmax_x - t->mmin_x, rngy = t->mmax_y - t->mmin_y;
        if (rngx <= 0) rngx = MODE13_W * 2 - 1;
        if (rngy <= 0) rngy = MODE13_H - 1;
        t->mx = t->mmin_x + cx * rngx / (r.w > 1 ? r.w - 1 : 1);
        t->my = t->mmin_y + cy * rngy / (r.h > 1 ? r.h - 1 : 1);
        if (g_x86_dbgring) {
            // DOSDIAG-gated, throttled to actual movement: the guest coordinate
            // the host cursor maps to. A scaled picture with unscaled input is
            // worse than no scaling, so the transform gets its own instrument
            // rather than being inferred from whether a game reacted.
            static int lmx = -1, lmy = -1;
            if (t->mx != lmx || t->my != lmy) {
                lmx = t->mx; lmy = t->my;
                kprintf("[dos] mouse screen=%d,%d -> guest=%d,%d (pic %d,%d,%dx%d)\n",
                        (int)mouse_x, (int)mouse_y, t->mx, t->my,
                        ox + r.x, oy + r.y, r.w, r.h);
            }
        }
        dos_mouse_clamp(t);
        // #163: MICKEYS AND BUTTON EDGES ARE LATCHED HERE, not in int33().
        //
        // This is the only place that sees every transition, so it is the only
        // place a press that happened BETWEEN two calls can be counted. int33()
        // 05h/06h then answer from a real counter instead of returning the
        // caller's own registers, and 0Bh returns real relative motion instead
        // of a hardcoded zero.
        // (#mickey) UNIT GAIN. This used to be
        //     t->mick_y += dym * t->mratio_y / 8;
        // with the documented default ratio of 16, i.e. every host pixel of
        // vertical motion became TWO mickeys. That is faithful to a relative
        // mouse and wrong against an absolute pointer: The Dig integrates the
        // counters straight into its own cursor, so its pointer moved at 2x
        // vertically and walked away from the arrow the user aims with
        // (measured: internal y +219 for a host +107). rustkern/dosmick.rs owns
        // the counters now, and dos_mouse_deliver() homes them.
        if (dos_mick_move_rs(&t->mick, t->mx, t->my,
                             t->mmin_x, t->mmax_x, t->mmin_y, t->mmax_y,
                             t->mratio_x, t->mratio_y)) {
            t->mprev_x = t->mx; t->mprev_y = t->my;
            t->mev_pending |= M_EV_MOVE;
        }
        int b = 0;
        if (mouse_buttons & 0x01) b |= 0x01;   // left
        if (mouse_buttons & 0x02) b |= 0x02;   // right
        int changed = b ^ t->mbtn_prev;
        for (int i = 0; i < 2; i++) {
            if (!(changed & (1 << i))) continue;
            if (b & (1 << i)) {
                if (t->mpress_n[i] < 0xFFFF) t->mpress_n[i]++;
                t->mpress_x[i] = (uint16_t)t->mx;
                t->mpress_y[i] = (uint16_t)t->my;
                t->mev_pending |= (uint16_t)((i == 0) ? M_EV_LPRESS : M_EV_RPRESS);
            } else {
                if (t->mrel_n[i] < 0xFFFF) t->mrel_n[i]++;
                t->mrel_x[i] = (uint16_t)t->mx;
                t->mrel_y[i] = (uint16_t)t->my;
                t->mev_pending |= (uint16_t)((i == 0) ? M_EV_LRELEASE : M_EV_RRELEASE);
            }
        }
        t->mbtn_prev = b;
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
//
// (#740) THE TEST IS PER-VECTOR, and it has to be. The first cut of this asked
// "does the vector hold ANY of our stubs", which is a different and wrong
// question: Aladdin copies the seeded INT 8 handler into the INT 88h slot, so
// IVT[88h] legitimately holds one of our stubs AND is guest-installed. Under
// the any-stub test INT 88h still read as "nothing installed" and the 2871
// ignored lines survived the fix. Compare against the stub WE SEEDED FOR THIS
// VECTOR instead: IVT[88h] was seeded to the IRET stub and now holds the BIOS
// timer stub, so it differs, so the guest owns it. IVT[8] still holds exactly
// what we put there, so it does not.
static uint16_t dos_vec_seed_stub(uint8_t vec) {
    if (vec == 0x33) return DOS_INT33_STUB;      // #163: must not look like IRET
    if (vec == 0x08) return DOS_BIOSTIMER_STUB;  // #740: a real BIOS timer
    return DOS_IRET_STUB;
}

// (rakbd2) THE OTHER HALF OF THE SAME QUESTION: WHICH VECTORS MUST READ BACK AS
// 0000:0000, i.e. FREE.
//
// dos_vec_seed_stub() answers "which stub does this vector get". raplay derived
// the rule "seed a vector-specific stub only where the vector's VALUE is
// something the guest INSPECTS", and got 33h right. It missed the complementary
// case: a guest can inspect a vector expecting to find NOTHING there, and a
// stub is then exactly as wrong as an IRET was for the mouse.
//
// 60h-66h are the DOS "user interrupt" vectors. DOS does not initialise them
// and the BIOS does not either, so on a real machine they are 0000:0000 until a
// TSR claims one. That is not a detail: it is the documented way a program
// FINDS a vector it may use.
//
// MEASURED, Red Alert 1.04 DOS (GAME.DAT, LE object 1 at runtime base
// 0x00110000). Install_Keyboard_Interrupt allocates a DOS memory block (DPMI
// 0100h), locks it (0600h), copies its real-mode-callable ISR into it, and then
// looks for a free user vector to publish it at:
//
//   00250b6f  mov  bl, 0x60          ; first user vector
//   00250b71  mov  bh, 6             ; try 60h..65h
//   00250b73  mov  eax, 0x200        ; DPMI: get REAL-MODE interrupt vector
//   00250b78  int  0x31
//   00250b7a  jb   0x250d40          ; -> failure exit
//   00250b80  or   cx, dx            ; CX:DX == 0000:0000 ?
//   00250b83  je   0x250b90          ; yes -> this one is free, take it
//   00250b85  inc  bl
//   00250b87  dec  bh
//   00250b89  jne  0x250b78
//   00250b8b  jmp  0x250d40          ; all six taken -> FAILURE
//
//   00250d40  xor  eax, eax  ...  ret      ; returns 0, installs nothing
//
// Because every one of 60h..65h answered F000:FF53, the loop exhausted and the
// routine returned at 0x250d40 BEFORE the code at 0x250c17 that actually
// installs the keyboard: 0204h/0200h (save the old INT 9), 0205h (set the
// protected-mode INT 9 handler to obj1+0x141707 = runtime 0x00251707, the ISR
// that does `in al,0x60` at 0x00251782), then 0201h. The DPMI census of the
// failing run shows exactly that shape: 0100h, 0200h, 0600h, 0601h present and
// 0204h, 0205h, 0201h NEVER CALLED, and the guest reported
// "[4GW] kbd ISR route: NONE". Three delivery-side fixes (#779, #779b, #779c)
// were correct and none of them could ever have helped: the game never asked.
//
// The range is 60h-66h and stops there deliberately. 67h is EMS on a machine
// with an EMM, 68h-6Fh are unused but are not what the documented scan uses,
// and widening the set turns a stray INT into a wild jump for no measured gain.
static int dos_vec_seed_free(uint8_t vec) {
    return (vec >= 0x60 && vec <= 0x66);
}

// (raplay) THE SAME CHOICE, IN THE 32-BIT GUEST'S ENCODING.
//
// A protected-mode client's vector table holds a FLAT address, so the stub a
// vector is seeded with is the flat address of the SAME ROM byte the 16-bit
// path points at: F000:<offset> is linear 0xF0000 + offset. Derived from
// dos_vec_seed_stub() rather than restated, so the two encodings cannot come
// to disagree about which stub a vector gets.
// ONLY VECTOR 33h, AND THE RESTRICTION IS MEASURED, NOT CAUTION.
//
// dos_vec_seed_stub() also gives vector 08h the BIOS timer stub `CD 1C CF`,
// which is right for a 16-bit guest because INT 1Ch is serviced for one. It is
// wrong here: dos4gw_rm_routable() does not carry 1Ch, so a 32-bit guest that
// reaches that stub gets a MISS with CF=1/AX=0001 instead of a tick.
//
// And it DOES reach it. Red Alert hooks INT 08h, saves the previous vector with
// AH=35h and chains to it from its own ISR. With 08h seeded to the IRET that
// chain is a harmless no-op; with it seeded to the timer stub the chain runs
// `INT 1Ch` every tick. MEASURED on build 2233, which seeded both: the run
// logged `MISS int 1Ch func 004Fh x1 first at EIP 0x000FFF62` and
// `MISS int 1Ch func 0000h x1`, and the game exited before it had set a video
// mode (exit=1, mode=0x03) where build 2231 with 08h left alone had reached its
// own mouse dialog in mode 13h and stayed there.
//
// So the rule is: seed a vector-specific stub only where the vector's VALUE is
// something the guest INSPECTS. 33h is that case and the only one in this
// table; everything else keeps the IRET, which is what a saved-and-chained
// vector wants.
static inline uint32_t dos4gw_pm_seed_lin(uint8_t vec) {
    if (vec != 0x33) return DOS4GW_PM_IRET_LIN;
    return ((uint32_t)0xF000u << 4) + (uint32_t)DOS_INT33_STUB;
}

static int dos_vec_hooked(dos_task_t *t, uint8_t vec) {
    uint16_t off = rd16(t, 0x0000, (uint16_t)(vec * 4));
    uint16_t seg = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
    if (seg == 0x0000 && off == 0x0000) return 0;   // null: nothing installed
    if (seg == 0xF000 && off == dos_vec_seed_stub(vec)) return 0;  // untouched seed
    // THE SAME QUESTION IN THE 32-BIT GUEST'S ENCODING. A protected-mode
    // client's vector table holds a FLAT address split across the two words
    // (rustkern/dos4gw.rs vec_pack), so the no-op handler dos4gw_seed_pm_ivt()
    // points every unhooked vector at reads as (seg 0x000F, off 0xFF53) rather
    // than as a real-mode (0xF000, 0xFF53). It belongs here, in the ONE
    // predicate, and not in a 32-bit copy of it: the question is "has the guest
    // taken this vector?", the answer is "no" in both encodings, and a second
    // function would be a second answer. That mistake was already made once on
    // this ticket and is recorded in blame.md.
    // (raplay) PER-VECTOR, for the same reason the 16-bit line above is
    // per-vector: since dos4gw_seed_pm_ivt() stopped pointing all 256 vectors
    // at one IRET, "the value we seeded" differs by vector here too. The old
    // IRET address is still accepted because it IS what most vectors are
    // seeded with, and because a table written by an older kernel and read by
    // this one must not suddenly read as guest-owned.
    uint32_t flat = (((uint32_t)seg << 16) | off);
    if (flat == DOS4GW_PM_IRET_LIN) return 0;
    if (flat == dos4gw_pm_seed_lin(vec)) return 0;
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
// #232: instructions retired inside a SYNTHESIZED interrupt delivery, summed
// for the run. These are real guest instructions and they count against the
// #232 CPU cap like any other - but they are retired HERE, outside the run
// loop's throttled burst, so when the cap is being missed this counter is the
// first thing to look at. A guest whose ISR alone costs more than the cap
// allows cannot be capped to that value by any amount of sleeping.
static unsigned long g_dos_irq_insns = 0;

// (#sbirq32) THE HANDLER ADDRESS IS NOW A PARAMETER, because a second vector
// table holds handlers this one cannot see.
//
// dos_deliver_int() reads the arena's low IVT and nothing else. A DPMI client
// has THREE vector spaces (the low table, the 0201h real-mode shadow, the 0205h
// protected-mode table) and a real-mode driver published through 0201h lives in
// one this function cannot address at all. Splitting the address out is the
// same move dos4gw_deliver_at() made for the 32-bit core, and for the same
// reason: every check below (the null refusal, the IRET detection, the #232
// accounting) applies identically to a handler from any table, and forking the
// function would have meant the second route silently lacking them.
static void dos_deliver_int_at(dos_task_t *t, uint16_t vseg, uint16_t voff,
                               unsigned long budget) {
    x86_16_cpu_t *c = &t->cpu;
    if (vseg == 0 && voff == 0) return;
    dos_push16(t, c->flags);
    dos_push16(t, c->cs);
    dos_push16(t, c->ip);
    c->flags &= ~0x0200;   // CLI during ISR
    c->cs = vseg;
    c->ip = voff;
    unsigned long i0 = c->insn_count;
    // #232 STOP AT THE IRET. This function's own header has always said it
    // "run[s] a bounded burst so the handler runs and IRETs back to the
    // interrupted code" - and then it ran the WHOLE budget, unconditionally.
    // x86_16_run() has no notion of an interrupt return, so the extra 19,975
    // instructions after a 25-instruction handler were ordinary MAINLINE code
    // being executed inside the interrupt-delivery helper.
    //
    // MEASURED, VM <vmid>, Joust, /CONFIG/DOSSPEED.CFG armed: with the guest CPU
    // capped to 500 cycles, NINETY-NINE PERCENT of every instruction the guest
    // retired was retired in here, and the delivered rate sat at 1.0-1.4 M
    // insn/s against a 500 kHz target. The arithmetic is not subtle: IRQ0 is
    // driven by the emulated PIT, which tracks REAL time, so at Joust's 62 Hz
    // that is 62 x 20,000 = 1.24 M instructions a second entering the guest
    // through a path the run loop's pacing never sees. No amount of sleeping
    // between bursts can cap a guest that is being run somewhere else.
    //
    // HOW "IRETED" IS DECIDED. The interrupt frame is exactly 6 bytes, so the
    // handler has returned the moment SP is back at (or above) its pre-push
    // value on the SAME stack. Two other exits are deliberate:
    //   * SS CHANGED: the handler switched stacks, so SP is not comparable.
    //     Keep running to the budget, i.e. exactly the old behaviour, rather
    //     than guess.
    //   * SP JUMPED FORWARD without an IRET: Joust's INT 9 handler does
    //     `mov sp,0x3D11` and JMPs into the game rather than returning
    //     (PROVEN by disassembly at guest linear 0xCCE6). That trips the same
    //     test, which is the RIGHT answer: control is never coming back, so
    //     handing it to the run loop is what should happen.
    // The budget still bounds a handler that does neither.
    //
    // Chunked rather than single-stepped: 128 instructions is far more than any
    // real ISR prologue needs before it could plausibly return, and it keeps
    // the check off the per-instruction path.
    uint16_t sp0 = (uint16_t)(c->sp + 6);   // SP as it was before the 3 pushes
    uint16_t ss0 = c->ss;
    unsigned long left = budget;
    while (left) {
        unsigned long chunk = left < DOS_IRQ_RETURN_CHUNK ? left : DOS_IRQ_RETURN_CHUNK;
        if (x86_16_run(&t->cpu, chunk) != 1) break;   // halted or stopped
        left -= chunk;
        if (c->ss == ss0 && (uint16_t)(c->sp - sp0) < 0x8000u) break;  // returned
    }
    g_dos_irq_insns += c->insn_count - i0;
}

static void dos_deliver_int(dos_task_t *t, uint8_t vec, unsigned long budget) {
    uint16_t voff = rd16(t, 0x0000, (uint16_t)(vec * 4));
    uint16_t vseg = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
    dos_deliver_int_at(t, vseg, voff, budget);
}
// #163: deliver the INT 33h 0Ch/14h user event handler.
//
// A driver calls this handler from its own interrupt context as a FAR CALL,
// with AX = the event bits, BX = the button mask, CX/DX = the position and
// SI/DI = the mickey counters. Storing the handler without ever calling it is
// the log-only stub the house rules ban: a program that installs a callback and
// then waits on it waits forever, and it would look exactly like a dead mouse.
//
// TWO THINGS MAKE THIS SAFE TO DO FROM AN ARBITRARY INSTRUCTION BOUNDARY, which
// is where a real mouse IRQ lands too:
//   - The full CPU context is saved and restored around the call, so whatever
//     the handler leaves in the registers cannot reach the interrupted code.
//     insn_count and `halted` are carried FORWARD rather than restored: the
//     emulated clock is derived from insn_count (dos_emu_pit_now), and rewinding
//     it would move guest time backwards, which is the one thing a delay loop
//     waiting for a threshold cannot survive.
//   - The far-return address is a `JMP $` stub in the reserved BIOS ROM region,
//     so "the handler returned" is a CS:IP TEST rather than a guess, and a
//     handler that never returns is bounded by the instruction budget instead
//     of running forever. The budget is spent in chunks so a prompt return
//     costs a prompt exit, not the whole allowance.
static void dos_mouse_events(dos_task_t *t) {
    if (!t->mev_seg && !t->mev_off) { t->mev_pending = 0; return; }
    uint16_t ev = (uint16_t)(t->mev_pending & t->mev_mask);
    t->mev_pending = 0;
    if (!ev) return;

    x86_16_cpu_t *c = &t->cpu;
    x86_16_cpu_t save = *c;

    // (raplay) A 16-bit guest has a live real-mode stack in t->cpu and we push
    // onto it exactly as a real driver's IRQ would. A 32-bit guest does not:
    // t->cpu is the scratch frame, and its SS:SP is whatever the last DPMI
    // 0300h reflection left there. Point it at the same host real-mode stack
    // dpmi_rmcs_call_rs() uses for that reflection, which is the carve-out at
    // the top of the transfer window and cannot be live here, because a mouse
    // upcall is delivered at a slice boundary and never nested inside a 0300h.
    if (t->le_active) {
        c->ss = (uint16_t)(DOS4GW_XFER_LIN >> 4);
        c->sp = (uint16_t)(DOS4GW_XFER_LEN - 0x100);
    }

    dos_push16(t, 0xF000);
    dos_push16(t, DOS_MEVRET_STUB);
    c->ax = ev;
    c->bx = (uint16_t)t->mbtn;
    c->cx = (uint16_t)t->mx;
    c->dx = (uint16_t)t->my;
    c->si = (uint16_t)t->mick_si;
    c->di = (uint16_t)t->mick_di;
    c->cs = t->mev_seg;
    c->ip = t->mev_off;
    int returned = 0;
    for (int k = 0; k < 8; k++) {
        if (c->cs == 0xF000 && c->ip == DOS_MEVRET_STUB) { returned = 1; break; }
        if (x86_16_run(c, 4000) != 1) break;
    }
    if (!returned && c->cs == 0xF000 && c->ip == DOS_MEVRET_STUB) returned = 1;
    uint16_t ecs = c->cs, eip = c->ip;
    unsigned long insns = c->insn_count;
    int halted = c->halted;
    int exit_code = c->exit_code;
    *c = save;
    c->insn_count = insns;
    c->halted = halted;
    c->exit_code = exit_code;

    {
        static int n = 0;
        if (n < 8) {
            n++;
            kprintf("[dos] #163 mouse upcall ev=%04x -> %04x:%04x %s (at %04x:%04x)\n",
                    ev, t->mev_seg, t->mev_off,
                    returned ? "returned" : "DID NOT RETURN", ecs, eip);
        }
    }
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
// (#234d) A HUMAN NAME FOR A DOS GUEST'S WINDOW, derived from its path.
//
// The shipped layout is /DOS/<GAME>/<PROG>.EXE, so the directory under /DOS is
// the game and the basename is an abbreviation of it (ROGUE/ROGUE.EXE,
// NETHACK/NETHACK.EXE, SKYROADS/SKYROADS.EXE). Prefer the directory; fall back
// to the basename without its extension for a program launched from anywhere
// else. Case is left as the filesystem stores it rather than prettified: a
// guessed capitalisation is wrong for NETHACK the moment you try, and an
// 8.3 name in a taskbar reads unmistakably as a DOS program.
//
// DERIVED, NOT A TABLE. A hand-written map of the ten games on today's image
// would be stale the first time an eleventh is added, and would say "DOS" for
// anything a user copies onto the disk themselves. If a launcher label is ever
// plumbed down to dos_launch(), prefer it and keep this as the fallback.
static void dos_guest_title(const char *path, char *out, int outlen) {
    const char *seg[8]; int slen[8]; int nseg = 0;
    for (const char *p = path; *p && nseg < 8; ) {
        while (*p == '/') p++;
        if (!*p) break;
        int n = 0; while (p[n] && p[n] != '/') n++;
        seg[nseg] = p; slen[nseg] = n; nseg++;
        p += n;
    }
    const char *name = "DOS"; int len = 3;
    if (nseg >= 3) {                 // /DOS/<GAME>/<PROG>.EXE -> <GAME>
        name = seg[nseg - 2]; len = slen[nseg - 2];
    } else if (nseg >= 1) {          // anywhere else -> the basename, no ".EXE"
        name = seg[nseg - 1]; len = slen[nseg - 1];
        for (int k = len; k > 0; k--)
            if (name[k - 1] == '.') { len = k - 1; break; }
    }
    if (len <= 0) { name = "DOS"; len = 3; }
    int i = 0;
    for (; i < len && i < outlen - 7; i++) out[i] = name[i];
    // The suffix keeps the DOS subsystem visible, which matters here because
    // TWO Rogues ship: the native Ring-3 port and this emulated one. A guest
    // that is genuinely called DOS does not get "DOS (DOS)".
    if (!(i == 3 && out[0] == 'D' && out[1] == 'O' && out[2] == 'S')) {
        const char *sfx = " (DOS)";
        for (int j = 0; sfx[j] && i < outlen - 1; j++) out[i++] = sfx[j];
    }
    out[i] = 0;
}

static void dos32_dospath(const char *path, char cur_drive, char *out, int outlen);
static uint32_t dos32_build_env(dos_task_t *t, uint32_t env_lin, const char *argv0);

static void dos_build_psp(dos_task_t *t, const char *path) {
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

    // (#digrun) PSP:2Ch - THE ENVIRONMENT SEGMENT. It was left at zero, and a
    // zero there is not 'no environment', it is 'your environment is the
    // interrupt vector table'.
    //
    // MEASURED on The Dig's IMUSE.EXE (a Rational DOS/16M bundle): the loader
    // stub reads PSP:2Ch, walks the block to the double-NUL, skips the WORD
    // count and reads the ASCIIZ program path DOS 3.0+ puts there, so that it
    // can REOPEN ITS OWN FILE and read the extender out of it. With segment 0
    // it opened '/WINDIR/DRIVE_C/<one garbage byte>' and then
    // '<garbage>.ETX', both failed, and it exited 0 after 4751 instructions
    // looking exactly like a program that had decided not to run.
    //
    // The block layout is dos32_build_env()'s, deliberately: it is the same
    // layout the two 32-bit loaders hand their guests, and a second copy of a
    // layout is how two loaders come to disagree about where the WORD count
    // goes (the comment on that function says so about ITS two callers; this
    // is the third).
    if (path) {
        char dospath[128];
        dos32_dospath(path, t->svc.cur_drive, dospath, (int)sizeof dospath);
        dos32_build_env(t, DOS_ENV_LIN, dospath);
        wr16(t, DOS_PSP_SEG, 0x2C, DOS_ENV_SEG);
    }
}

// (#740 digsel) SHARED BY BOTH 32-BIT LOADERS, defined with the go32 loader
// below because that is where they were written. Forward-declared so
// dos4gw_prepare() calls the SAME two functions rather than growing its own.
// (#digrun) The declarations MOVED UP, to just above dos_build_psp(), because
// the 16-bit PSP builder is now the THIRD caller and it is defined earlier in
// this file than they were declared.

// ---- run -----------------------------------------------------------------

// ===========================================================================
// #740: THE DOS/4GW GUEST
//
// Everything below is the HOST SIDE of a 32-bit protected-mode guest: it
// decides where memory goes, when to present a frame, and which of the existing
// service paths an interrupt belongs to. It contains no marshalling and no
// service logic, both of which are in rustkern/dos4gw.rs and in the one INT 21h
// core respectively.
//
// It is C, and that is a departure from the standing Rust-first rule, so here is
// the reason rather than an excuse: every line of it reaches file-static state
// in this translation unit that has no header and no external linkage
// (dos_int_handler, dos_present, dos_in, dos_out, dos_task_t itself). Rust
// cannot call a C static, so a Rust version would have required exporting five
// internals of a 3,152-line file first. That refactor is worth doing and is NOT
// worth entangling with the first change that makes a real DOS/4GW binary
// execute; the parts that could be Rust are Rust, and they are the parts where
// the bugs live.
// ===========================================================================

// The DPMI extension hook. rustkern/dpmi.rs calls this for every INT 31h AX it
// does not service itself; we return 1 if we handled it and 0 to let it fall
// through to its own logged MISS, which is where anything neither of us
// implements belongs.
//
// dos/dpmi.h carries a paste-ready sketch of the 0300h half, written by the
// agent that built the marshaller, against the real signature. This is that
// sketch with its stated caveat honoured: THE TWO CARRY FLAGS ARE NOT THE SAME
// CARRY FLAG. `rc` says whether the DPMI call worked; the simulated interrupt's
// own CF lives in the RMCS flags word where the guest looks for it. Reporting a
// failed DOS call as a failed DPMI call is the one thing an implementer of this
// hook gets wrong.
// DPMI 0100h/0101h: allocate and free REAL DOS MEMORY.
//
// dos/dpmi.h and rustkern/dpmi.rs are emphatic that 0100h is a THIN WRAPPER
// over the EXISTING INT 21h AH=48h MCB allocator and that the DPMI host
// contains no allocator, because two allocators over one megabyte is the
// memory-shaped version of the three-INT-21h fault. These two functions are
// that wrapper and nothing else: they build a register frame and call the same
// dos_extend_int21() the 16-bit DOS task calls.
//
// THIS IS NOT OPTIONAL POLISH. Doom's V_Init() allocates its four 320x200
// screens through I_AllocLow(), which is a raw DPMI 0100h, and a CF=1 from it
// is an immediate I_Error. Measured: with 0100h unbound, Doom prints
// "V_Init: allocate screens." and never prints another line.
static int dos4gw_dosmem_alloc(void *user, uint16_t paras,
                               uint16_t *out_seg, uint16_t *out_largest) {
    dos_task_t *t = (dos_task_t *)user;
    x86_16_cpu_t f;
    memset(&f, 0, sizeof f);
    f.mem = t->mem;
    f.flags = 0x0002;
    f.ax = 0x4800;
    f.bx = paras;
    dos_extend_int21(&t->svc, &f, 0x48);
    if (f.flags & 1) {
        if (out_largest) *out_largest = f.bx;   // Descent probes with BX=0xFFFF
        return -1;
    }
    if (out_seg) *out_seg = f.ax;
    return 0;
}

static int dos4gw_dosmem_free(void *user, uint16_t seg) {
    dos_task_t *t = (dos_task_t *)user;
    x86_16_cpu_t f;
    memset(&f, 0, sizeof f);
    f.mem = t->mem;
    f.flags = 0x0002;
    f.ax = 0x4900;
    f.es = seg;
    dos_extend_int21(&t->svc, &f, 0x49);
    return (f.flags & 1) ? -1 : 0;
}

// ===========================================================================
// THE 0300h SERVICE ROUTER (#740, Discworld II).
//
// dpmi_rmcs_dos_dispatch() routes INT 21h and nothing else, and says so: the
// other real-mode services live in THIS file as statics bound to a dos_task_t,
// so from dos/dpmi_rmcs.c the honest answer for them was a logged MISS.
//
// From here they are reachable, so route them. NOT by reimplementing anything:
// the marshalled real-mode frame is copied into the task's own 16-bit register
// file and dos_int_handler() (the ONE implementation of INT 10h/16h/1Ah/2Fh/
// 33h, the same one the 16-bit interpreter calls) runs against it, then the
// result is copied back for the marshaller to store into the RMCS.
//
// MEASURED, and this is what the routing is for: Discworld II's first act as a
// protected-mode program is INT 31h AX=0300h with BL=10h and RMCS AX=4F00h,
// i.e. "get VESA VBE controller info". It has no mode 13h fallback, so a MISS
// there is the end of the game, not a degradation.
//
// WHY AN ALLOWLIST AND NOT "TRY IT AND SEE". dos_int_handler() answers on
// t->cpu for a vector it knows and falls through for one it does not, and a
// fall-through returns to the guest with CF UNTOUCHED, i.e. looking like
// success. That is precisely the Win16 stack-desync failure recorded in
// blame.md, one layer down. Only vectors with an explicit case in
// dos_int_handler() are claimed here; anything else is declined so the
// marshaller applies its own documented MISS effect (CF=1, AX=0001) and logs.
static int dos4gw_rm_routable(uint8_t intno) {
    switch (intno) {
    case 0x10:   // video, including the whole 4Fxx VESA/VBE surface
    case 0x16:   // keyboard
    case 0x1A:   // timer tick count
    case 0x2F:   // multiplex: XMS install check + MSCDEX (#196)
    case 0x33:   // mouse
        return 1;
    default:
        return 0;
    }
}

// Copy the marshalled real-mode register file in and out of the task's 16-bit
// CPU. mem/owner/int_fn/... are deliberately NOT touched: they are the task's
// environment, not the guest's registers, and dos_int_handler() reaches its
// task through cpu->owner.
static void dos4gw_rm_regs_in(dos_task_t *t, const x86_16_cpu_t *f) {
    t->cpu.ax = f->ax; t->cpu.bx = f->bx; t->cpu.cx = f->cx; t->cpu.dx = f->dx;
    t->cpu.si = f->si; t->cpu.di = f->di; t->cpu.bp = f->bp; t->cpu.sp = f->sp;
    t->cpu.cs = f->cs; t->cpu.ds = f->ds; t->cpu.es = f->es; t->cpu.ss = f->ss;
    t->cpu.ip = f->ip; t->cpu.flags = f->flags;
    t->cpu.fs = f->fs; t->cpu.gs = f->gs;
    for (int i = 0; i < 8; i++) t->cpu.exhi[i] = f->exhi[i];
}

static void dos4gw_rm_regs_out(const dos_task_t *t, x86_16_cpu_t *f) {
    f->ax = t->cpu.ax; f->bx = t->cpu.bx; f->cx = t->cpu.cx; f->dx = t->cpu.dx;
    f->si = t->cpu.si; f->di = t->cpu.di; f->bp = t->cpu.bp; f->sp = t->cpu.sp;
    f->cs = t->cpu.cs; f->ds = t->cpu.ds; f->es = t->cpu.es; f->ss = t->cpu.ss;
    f->ip = t->cpu.ip; f->flags = t->cpu.flags;
    f->fs = t->cpu.fs; f->gs = t->cpu.gs;
    for (int i = 0; i < 8; i++) f->exhi[i] = t->cpu.exhi[i];
}

// (#dpmi301) DPMI 0300h ON A VECTOR THE GUEST ITSELF PUBLISHED: EXECUTE IT.
//
// WHAT THIS IS FOR, AND WHY IT IS NOT 0301h.
// -------------------------------------------------------------------------
// This was scoped as "implement the DPMI 03xx family so the Miles Sound System
// can far-call its 16-bit driver", on the inference that a protected-mode
// client reaches a real-mode driver through 0301h. MEASURED on Discworld II
// (golden 2270, run3-inifmt), that inference is wrong: the guest never issues
// 0301h, 0302h, 0303h or 0304h even once. What Miles actually does is
//
//   INT 21h AH=3Dh   open SBLASTER.DIG          (an AIL3DIG driver blob)
//   INT 21h AH=3Fh   read all 3125 bytes
//   INT 31h AX=0100  allocate 0xC4 paragraphs of DOS memory, copy it down
//   INT 21h AH=35h   get real-mode vector 66h   (is this user vector free?)
//   INT 31h AX=0201  SET real-mode vector 66h to the driver's entry point
//   INT 31h AX=0300  simulate INT 66h, AX = the AIL function number
//
// i.e. it publishes the driver as a real-mode interrupt handler and then calls
// it through the ordinary simulate-real-mode-interrupt service. The AX values
// observed were 0300h, 0301h and 0304h, which are AIL FUNCTION numbers and NOT
// DPMI ones; that coincidence is precisely how this was read as a missing DPMI
// 03xx family. The whole 03xx family could have been implemented perfectly and
// this title would not have moved one instruction.
//
// Before this function, 0300h answered INT 66h with the documented MISS
// (CF=1, AX=0001) because dos4gw_rm_routable() correctly declines a vector no
// HOST service implements. That answer was right about the host and wrong
// about the guest: nothing in this kernel implements INT 66h, but the GUEST
// does, and it said so with 0201h. The MISS is only correct for a vector whose
// provenance is ours.
//
// WHY RUNNING 16-BIT CODE HERE NEEDS NO NEW INFRASTRUCTURE:
//   * t->cpu is ALREADY bound to this guest's arena and already carries the
//     hooks (dos_int_handler, dos_in, dos_out, the EGA memory hook), because
//     the 32-bit core routes its own port I/O through dos_in/dos_out with
//     &t->cpu as the carrier (X32_EXIT_IO_IN/OUT in dos4gw_run). So an OUT to
//     0x22C from driver code reaches the SAME Sound Blaster emulation the
//     32-bit path reaches, with no new wiring and no second copy of anything.
//   * t->cpu is scratch under an LE guest: no 16-bit program exists, which is
//     the same reason dos4gw_rm_dispatch() may already write it freely.
//   * The driver image lives in DOS memory the guest allocated through 0100h,
//     i.e. inside the low megabyte the 16-bit interpreter addresses.
//
// HOW THE RETURN IS DETECTED. A handler published as an interrupt vector ends
// with IRET, so an interrupt frame is pushed whose CS:IP is a two-byte `JMP $`
// landing pad in the BIOS stub area. The handler's IRET lands there and the
// loop stops on CS:IP, which is dos_mouse_events()'s proven idiom rather than a
// new one. The instruction budget is a BACKSTOP for a handler that never
// returns, not the normal exit: a budget used as the normal exit is how
// dos_deliver_int() came to run 19,975 instructions of mainline code inside an
// interrupt helper (#232).
//
// ORDERING, AND THE REGRESSION STORY. This is tried only AFTER
// dos4gw_rm_routable() has declined, so for every vector this host already
// services the behaviour is byte-identical to before and no baselined guest can
// change. A real DPMI host would prefer the client's own handler even for a
// vector it services; that is deliberately NOT done here, because it would put
// every existing guest's INT 10h/16h/1Ah/2Fh/33h on a new path to buy
// correctness nothing has yet asked for.
//
// Returns 1 if the handler was executed (the frame then carries its result),
// 0 if this vector is not the guest's and the caller should MISS as before.
// THE BACKSTOP FOR A HANDLER THAT NEVER RETURNS, EXPRESSED IN GUEST TIME.
//
// (#sbirq32) It used to be a flat 2,000,000 instructions, and that number means
// a different length of time on every host and a different number of BIOS TICKS
// at every dos_emu_hz(). MEASURED on Discworld II from the call that eventually
// succeeded - 8,699,264 instructions bought 8 BIOS ticks - the old ceiling was
// 1.8 ticks of GUEST time, and separately 74 ms of real time at the ~27 million
// instructions a second delivered here. SBLASTER.DIG's probe waits for TWO tick
// edges before it even looks at whether its interrupt arrived, and its no-card
// path wants twenty, so the old ceiling could not have been survived by a
// correct driver no matter what the emulation did. A bound that a legitimate
// handler cannot fit inside is not a backstop, it is the behaviour. The
// guest-tick count is the load-bearing number: a guest counts ticks, not
// seconds, so real time is the wrong unit for deciding whether its own timeout
// can elapse.
//
// Stated in milliseconds of GUEST time and converted through the same
// dos_emu_hz() every other clock in this file uses, so it means the same thing
// everywhere. The floor keeps it from collapsing before the interpreter has
// measured a rate; the ceiling keeps a pathological hz from removing the bound.
// It is NOT a pace: the loop exits the instant the handler IRETs onto the
// landing pad, so this costs nothing for a handler that returns.
#define DOS_RMEXEC_MS        2000UL
#define DOS_RMEXEC_INSN_MIN  2000000UL
#define DOS_RMEXEC_INSN_MAX  200000000UL

static unsigned long dos_rmexec_budget(void) {
    uint64_t n = ((uint64_t)dos_emu_hz() * DOS_RMEXEC_MS) / 1000ull;
    if (n < DOS_RMEXEC_INSN_MIN) n = DOS_RMEXEC_INSN_MIN;
    if (n > DOS_RMEXEC_INSN_MAX) n = DOS_RMEXEC_INSN_MAX;
    return (unsigned long)n;
}
static int dos4gw_rm_exec_guest(dos_task_t *t, uint8_t intno, x86_16_cpu_t *frame) {
    uint16_t vseg = 0, voff = 0;
    if (!dpmi_rmvec_guest_rs(intno, &vseg, &voff)) return 0;

    x86_16_cpu_t *c = &t->cpu;
    dos4gw_rm_regs_in(t, frame);

    // The marshaller substitutes a host stack when the client passes SS:SP =
    // 0:0, so there is normally always a real stack to push onto. If there is
    // not, refuse rather than push onto 0000:0000, which is the interrupt
    // vector table.
    if (c->ss == 0 && c->sp == 0) {
        kprintf("[4GW] 0300h INT %02Xh: guest handler at %04x:%04x but SS:SP is 0:0 "
                "after marshalling; refusing to execute on a null stack\n",
                (unsigned)intno, (unsigned)vseg, (unsigned)voff);
        return 0;
    }

    dos_push16(t, c->flags);
    dos_push16(t, 0xF000);
    dos_push16(t, DOS_RMCALLRET_STUB);
    c->flags &= ~0x0200;        // IF clear inside the handler, as a real INT does
    c->cs = vseg;
    c->ip = voff;

    unsigned long i0 = c->insn_count;
    uint64_t rt0 = sched_now_ms();
    uint32_t bt0 = dos_bios_tick_now(t);
    int returned = 0;
    unsigned long budget = dos_rmexec_budget();
    unsigned long left = budget;
    // (#sbirq32) THE HARDWARE INTERRUPT THAT THIS CODE IS WAITING FOR.
    //
    // A driver function published on a real-mode vector and invoked through
    // 0300h is not a pure computation: SBLASTER.DIG's 0304h arms an 8-bit DMA
    // transfer and then spins on a flag its own IRQ5 handler sets. Without a
    // delivery point inside THIS loop that flag can never change, and the only
    // possible outcome is the budget exhausting - which is exactly what was
    // MEASURED (2,000,000 instructions, zero port I/O in all of them).
    //
    // The cap is a backstop, not a pace. Each delivery is one raised IRQ and
    // the pump raises one per block PLAYED, i.e. at real-time rate, so a single
    // burst can legitimately see only a handful. A larger number means the card
    // is being re-armed inside the ISR, and 64 of them bounds the ISR
    // instructions this loop can retire without letting a runaway handler hide
    // behind a budget that only counts the mainline.
    int sb_deliveries = 0;
    while (left) {
        if (c->cs == 0xF000 && c->ip == DOS_RMCALLRET_STUB) { returned = 1; break; }
        if (sb_deliveries < 64 && dos_sb_irq_pending_rs(&t->sb)) {
            sb_deliveries++;
            dos4gw_sb_irq(t, 1);
            if (c->cs == 0xF000 && c->ip == DOS_RMCALLRET_STUB) { returned = 1; break; }
        }
        // (#sbirq32) AND THE GUEST'S CLOCK KEEPS RUNNING. dos_emu_insns() now
        // counts this core too, so the emulated PIT advances while we are here;
        // this is what PUBLISHES it where a real-mode driver reads it. Without
        // it the tick dword holds whatever dos4gw_timebase() last wrote, which
        // is what SBLASTER.DIG's probe spun on forever.
        dos4gw_bios_tick(t);
        unsigned long chunk = left < DOS_IRQ_RETURN_CHUNK ? left : DOS_IRQ_RETURN_CHUNK;
        if (x86_16_run(c, chunk) != 1) break;   // halted, stopped, or a MISS
        left -= chunk;
    }
    if (!returned && c->cs == 0xF000 && c->ip == DOS_RMCALLRET_STUB) returned = 1;

    // FIRST CALL PER VECTOR IS LOUD, and a handler that does not return is
    // ALWAYS loud. A driver that faults on its second instruction and a driver
    // that runs correctly are indistinguishable from outside, and this
    // project's recorded failure mode is exactly a feature that executes and
    // produces nothing.
    {
        static uint8_t logged[256];
        // (#sbirq32) GUEST TIME AND REAL TIME, SIDE BY SIDE, because the whole
        // question this call raises is whether they agree. A guest that spends
        // twenty of its own BIOS ticks waiting for a device gives that device
        // twenty ticks of REAL time only if the two clocks run at the same
        // rate, and the emulated PIT is derived from retired instructions, not
        // from a wall clock. Printing one without the other is how "the driver
        // timed out" and "we were too slow" stay indistinguishable.
        if (!logged[intno] || !returned || g_x86_dbgring) {
            logged[intno] = 1;
            kprintf("[4GW] 0300h EXECUTED guest INT %02Xh handler at %04x:%04x "
                    "(AX=%04x in), %s after %lu of %lu instructions (a %lu ms "
                    "guest-time budget); %lu guest ticks in %llu real ms "
                    "(emu_hz %lu); AX=%04x CF=%u out\n",
                    (unsigned)intno, (unsigned)vseg, (unsigned)voff,
                    (unsigned)frame->ax,
                    returned ? "IRETed" : "DID NOT RETURN (budget exhausted)",
                    c->insn_count - i0, budget, DOS_RMEXEC_MS,
                    (unsigned long)(dos_bios_tick_now(t) - bt0),
                    (unsigned long long)(sched_now_ms() - rt0),
                    (unsigned long)dos_emu_hz(),
                    (unsigned)c->ax, (unsigned)(c->flags & 1u));
        }
    }

    dos4gw_rm_regs_out(t, frame);
    return 1;
}

static int dos4gw_rm_dispatch(void *user, uint8_t intno, struct x86_16_cpu *frame) {
    dos_task_t *t = (dos_task_t *)user;
    if (!t || !frame) return 0;

    // INT 21h keeps going through THE one service core by the existing route,
    // so there is exactly one code path into it from here.
    if (intno == 0x21) return dpmi_rmcs_dos_dispatch(&t->svc, intno, frame);

    if (!dos4gw_rm_routable(intno)) {
        // (#dpmi301) Nothing HERE services this vector, but the guest may have
        // published its own real-mode handler on it with 0201h, in which case
        // the honest answer is to run it rather than to report it missing.
        if (dos4gw_rm_exec_guest(t, intno, (x86_16_cpu_t *)frame)) return 1;
        return 0;   // -> the marshaller's MISS
    }

    // t->cpu is SCRATCH under an LE guest: no 16-bit code executes, and
    // dos4gw_service_int() already uses it exactly this way for the guest's own
    // INT 21h/10h/16h/33h. So it is written, used, and left; there is no 16-bit
    // register state to preserve because there is no 16-bit program.
    dos4gw_rm_regs_in(t, (const x86_16_cpu_t *)frame);
    // (raplay) The frame we are about to hand the service holds REAL-MODE
    // register values: the client filled an RMCS and asked for a real-mode
    // interrupt, so its ES:DX is a seg:off inside the low megabyte, not a
    // protected-mode selector. int33() 0Ch/14h needs that distinction to know
    // whether the handler it is being given is one we may execute.
    t->rm_reflect = 1;
    dos_int_handler(&t->cpu, intno);
    t->rm_reflect = 0;
    dos4gw_rm_regs_out(t, (x86_16_cpu_t *)frame);
    return 1;
}

static int dos4gw_dpmi_ext(void *user, dpmi_regs_t *r, uint16_t ax) {
    dos_task_t *t = (dos_task_t *)user;
    if (!t || !t->le_active) return 0;

    // 0500/0501/0502: the guest's memory, answered against the arena we
    // actually allocated, so the number is true rather than generous.
    if (dos4gw_dpmi_mem_rs(t->le_state, (struct dpmi_regs *)r, ax)) return 1;

    if (ax != 0x0300) return 0;   // decline -> the host's MISS path, correctly

    // WHERE THE RMCS ACTUALLY IS (#740, measured on Discworld II).
    //
    // DPMI says ES:EDI points at the RMCS in the CLIENT'S PROTECTED-MODE
    // address space. Our client is FLAT: exec/x86_32 is initialised with base 0
    // and no segment bases at all (dos4gw_prepare: x86_32_init(cpu, arena, 0,
    // total)), and its segment registers are carried as VALUES, never applied
    // to an effective address. So the flat address of the block is EDI, full
    // stop. The old `(es << 4) + edi` was a real-mode conversion applied to a
    // protected-mode selector; it silently added selector*16 of garbage to a
    // correct address, and only looked harmless because Discworld II happens to
    // call 0300h with ES = 0.
    //
    // The limit is the WHOLE arena, not t->le_arena.size. le_arena.size is the
    // 1 MiB real-mode window that bounds the seg:off pointers INSIDE the RMCS;
    // the block itself lives wherever the 32-bit client put it, which for this
    // game is its own 32-bit stack at flat 0x0020F944. Checking the block
    // against the 1 MiB window refused every 0300h the game made.
    // (#740 dw2) ES MAY HAVE A BASE. The paragraph above is right that a flat
    // client's block is at EDI, and it was the only expressible answer while
    // nothing could resolve a selector. It can now, and this is the same
    // mechanism that put Discworld II's VbeInfoBlock on the interrupt vector
    // table, so it is answered the same way and in one place: ask the host,
    // and take 0 when the selector is not one it handed out. For a client that
    // passes ES = 0, as the measured one does, that is EDI unchanged.
    uint32_t es_base = 0;
    int es_known = (dpmi_sel_lookup_rs((uint16_t)r->es, &es_base, 0, 0) == 0);
    if (!es_known) es_base = 0;
    uint32_t rmcs_flat = es_base + r->edi;
    {   // (#740 dw2) WHICH real-mode interrupt, and the AX it carries. A 0300h
        // that simulates INT 21h AH=25h installs a REAL-MODE vector, and the
        // 32-bit delivery path reads the same table as a FLAT address; if that
        // is what is happening, this line is where it becomes visible instead
        // of being inferred from a corrupted table three thousand lines later.
        extern volatile int g_x86_dbgring;
        if (g_x86_dbgring) {
            uint16_t rax = 0;
            (void)x86_32_read_guest(&t->le_cpu, rmcs_flat + 0x1C,
                                    (uint8_t *)&rax, 2);
            kprintf("[4GW] 0300h simulate INT %02Xh: RMCS at flat 0x%08x, AX=%04x\n",
                    (unsigned)(r->ebx & 0xFF), rmcs_flat, (unsigned)rax);
        }
    }
    static int logged_once;
    if (!logged_once) {
        logged_once = 1;
        kprintf("[4GW] 0300h RMCS: ES:EDI = %04x:%08x -> flat 0x%08x "
                "(ES base 0x%08x, %s; client arena 0x%08x, real-mode window 0x%08x)\n",
                (unsigned)r->es, (unsigned)r->edi, rmcs_flat, es_base,
                es_known ? "a descriptor this host handed out"
                         : "not one of ours, so base 0 as before",
                t->le_arena_size, t->le_arena.size);
    }

    x86_16_cpu_t frame;
    memset(&frame, 0, sizeof frame);
    int rc = dpmi_rmcs_call_rs(&t->le_arena, rmcs_flat, t->le_arena_size,
                               (uint16_t)r->ebx,
                               &frame, dos4gw_rm_dispatch, t,
                               (uint16_t)(DOS4GW_XFER_LIN >> 4),
                               (uint16_t)(DOS4GW_XFER_LEN - 0x100));
    if (rc < 0) r->eflags |= 1; else r->eflags &= ~1u;
    return 1;
}

// Load an LE module into a flat arena that also holds the guest's first
// megabyte. Replaces dos_load_image() for this file format; everything the
// caller has already set up (the fs gate, the service context, the identity)
// is kept.
//
// On EVERY return path t->mem is a valid, kmalloc'd buffer the caller may free,
// including the failure paths, because the caller's contract is `kfree(t->mem)`
// and a function that swapped the pointer and then failed would otherwise leave
// it dangling or double-freed.
static int dos4gw_prepare(dos_task_t *t, const char *path,
                          const uint8_t *file, uint32_t size) {
    // Size the arena from the module's own relocated top. A probe parse is the
    // documented way to do this (exec/le.c does the same for its private
    // arena); it costs microseconds over an in-RAM buffer.
    le_image_t probe;
    int e = le_parse_rs(file, size, &probe);
    if (e != LE_OK) {
        kprintf("[4GW] %s: LE parse failed: %s\n", path, le_strerror_rs(e));
        return -1;
    }
    e = le_relocate_rs(&probe, DOS4GW_LOW_SIZE);
    if (e != LE_OK) {
        kprintf("[4GW] %s: relocate failed: %s\n", path, le_strerror_rs(e));
        return -1;
    }

    uint64_t top = ((uint64_t)probe.lin_hi + 0xFFFu) & ~0xFFFull;
    uint64_t want = top + DOS4GW_HEAP_SLACK;
    if (want > DOS4GW_ARENA_MAX) want = DOS4GW_ARENA_MAX;
    if (want < top) {
        kprintf("[4GW] %s: module top 0x%08x exceeds the %u MiB arena ceiling\n",
                path, probe.lin_hi, DOS4GW_ARENA_MAX >> 20);
        return -1;
    }
    uint32_t total = (uint32_t)want;

    uint8_t *arena = (uint8_t *)kmalloc(total);
    if (!arena) {
        kprintf("[4GW] %s: arena kmalloc(%u) FAILED\n", path, total);
        return -1;
    }
    memset(arena, 0, total);

    e = le_load_into(path, file, size, DOS4GW_LOW_SIZE, arena, total, 0, &t->le_mod);
    if (e != LE_OK) {
        kfree(arena);
        return -1;
    }

    // THE POST-LOAD INVARIANT IS THE CALLER'S TO JUDGE, and this caller judges
    // it fatal. le_load_into() reports it; a module whose fixups did not all
    // land inside a declared object is one whose code will jump somewhere we
    // cannot diagnose, and running it anyway would turn a clean, located
    // failure into an arbitrary one.
    if (t->le_mod.va.checked == 0 || t->le_mod.va.outside || t->le_mod.va.unreadable) {
        kprintf("[4GW] %s: POST-LOAD INVARIANT FAILED (%u of %u checked fixups inside "
                "an object, outside=%u unreadable=%u). Refusing to execute.\n",
                path, t->le_mod.va.inside_object, t->le_mod.va.checked,
                t->le_mod.va.outside, t->le_mod.va.unreadable);
        kfree(arena);
        return -1;
    }

    // THE SWAP. From here the guest's flat space and the DOS task's first
    // megabyte are the same bytes: t->mem[0xA0000] is both "the VGA aperture the
    // present path reads" and "guest linear 0xA0000". dos/dos4gw.h explains why
    // that is one buffer and not two.
    kfree(t->mem);
    t->mem = arena;
    t->cpu.mem = arena;          // the 16-bit frame's view, still the low MiB
    t->le_arena_size = total;
    dos_build_psp(t, path);      // rebuild it in the new buffer

    x86_32_init(&t->le_cpu, arena, 0, total);
    t->le_cpu.owner = t;
    // (#740 doom-present) Route the VGA aperture through the SAME plane-aware
    // core the 16-bit interpreter uses (x86_16_set_mem_hook below), instead of
    // letting it fall through to the flat arena write "THE SWAP" describes
    // above. Without this a DOS/4GW guest's Mode X writes silently collapsed
    // four planes into one flat byte range that dos_present_modex() never even
    // reads, presenting a permanently zero-filled flat colour. Registered here,
    // once, right after x86_32_init() (which zeroes the hook fields) and before
    // the guest's first instruction runs.
    x86_32_set_mem_hook(&t->le_cpu, VGA_A000, VGA_A000_END, ega_mem_w32, ega_mem_r32);
    // (#740 dw2) A DPMI SELECTOR HAS A BASE, AND UNTIL NOW THIS CORE THREW IT
    // AWAY. seg_base[] was threaded through every effective address, every
    // stack access and every string operation, and was never written by
    // anything: a `mov es, dx` after INT 31h AX=0100h left ES addressing flat
    // zero instead of the DOS block the host had just allocated. Discworld II
    // wrote its 512-byte VbeInfoBlock straight over the interrupt vector table
    // and the next timer tick was delivered to what it had left in vector 08h.
    // dpmi_sel_lookup_rs() is bound directly rather than through an adapter,
    // because x86_32_sel_base_fn is its signature; a selector the host did not
    // hand out still resolves to base 0, so a flat guest is unchanged.
    x86_32_set_sel_base_cb(&t->le_cpu, dpmi_sel_lookup_rs);
    t->le_cpu.eip = t->le_mod.entry_lin;
    // The module's own SS:ESP if it has one (all three measured games do);
    // otherwise the top of the arena, above the DPMI heap's reserved tail.
    t->le_cpu.regs[X32_ESP] = t->le_mod.stack_lin ? t->le_mod.stack_lin
                                                  : (total - 16u);

    // The DOS memory pool. alloc_top_para is the MCB allocator's bump pointer
    // and it is ZERO on a fresh task, because the 16-bit path seeds it from the
    // MZ image's own load address and there is no MZ image here. Left at zero,
    // the first AH=48h would hand out segment 0x0000, i.e. the interrupt vector
    // table. Seeded here to the first paragraph above the transfer buffer.
    t->alloc_top_para = DOS4GW_DOSMEM_FLOOR;
    // (RA4GW) AND THE FLOOR, WHICH WAS NEVER SET, SO THE BUMP POINTER COULD
    // FALL ONTO THE INTERRUPT VECTOR TABLE.
    //
    // dos_mcb_retop() recomputes alloc_top_para as max(alloc_floor_para, the
    // top of every LIVE block). Both 16-bit load paths set alloc_floor_para
    // right next to alloc_top_para; this one set only the top, and a dos_task_t
    // is zeroed, so the floor was 0. The moment a guest freed every DOS block
    // it held, retop dropped the bump pointer to the bottom of memory and the
    // NEXT AH=48h handed out a segment inside the first kilobyte.
    //
    // MEASURED, Discworld II, 2026-08-27: `48h alloc req=9fb0 -> seg=0010`,
    // i.e. linear 0x100, straight across the interrupt vector table; 272 IVT
    // stores followed, and the guest was executing at linear 0 sixteen frames
    // later (FAULT_MEM, address 0xffffffff, EIP 0x00000000).
    //
    // IT HAD BEEN MASKED BY A SECOND BUG. Before the AH=4Ah interception above,
    // a 32-bit guest's `sbrk` reached this same MCB allocator, which recorded a
    // bogus live block at segment 0x0000 spanning thousands of paragraphs. That
    // block pinned retop's answer well above the vector table, so the missing
    // floor never showed. Fixing the routing removed the accident, which is the
    // ordinary shape of this: one wrong thing was holding another wrong thing
    // still.
    t->alloc_floor_para = DOS4GW_DOSMEM_FLOOR;

    // The guest's real-mode-addressable window, and THE bounds chokepoint for
    // every DOS-side access. Bound into the service context so the INT 21h core
    // reaches guest memory through the counted, refusing accessors rather than
    // through the 16-bit CPU's unchecked ones.
    memset(&t->le_arena, 0, sizeof t->le_arena);
    t->le_arena.base = arena;
    t->le_arena.size = DOS4GW_LOW_SIZE;
    dpmi_rmcs_bind_arena(&t->svc, &t->le_arena);
    t->svc.has_ivt = 1;

    t->le_state = kmalloc(dos4gw_state_size_rs());
    if (!t->le_state ||
        dos4gw_init_rs(t->le_state, arena, total, DOS4GW_XFER_LIN, DOS4GW_XFER_LEN,
                       (uint32_t)top, total - DOS4GW_STACK_RESERVE) != 0) {
        if (t->le_state) { kfree(t->le_state); t->le_state = 0; }
        kprintf("[4GW] %s: bridge state init FAILED\n", path);
        return -1;   // t->mem is the arena and is valid; the caller frees it
    }

    // One host, one guest (rustkern/dpmi.rs). Reset it so a second launch
    // cannot inherit the first guest's descriptors.
    dpmi_host_reset_rs();
    dpmi_set_ext_rs(dos4gw_dpmi_ext, t);
    dpmi_bind_dosmem_rs(dos4gw_dosmem_alloc, dos4gw_dosmem_free, t);

    // ---- (#740 digsel) THE THREE SELECTORS A WATCOM GUEST NAMES -----------
    //
    // Every Watcom 32-bit program runs an extender ladder about 0x13E bytes
    // past its entry: Phar Lap (INT 21h AH=30h with EBX='PHAR'), then Rational
    // DOS/4G (AX compared with 0x4243), then INT 21h AX=FF00h. We answer FF00h
    // with AL=0, which is the true answer for a plain DPMI host and is why the
    // ladder ends in its LAST arm. That arm asks this host for NOTHING. It
    // names three selectors as constants and uses them:
    //
    //     0x0017  the flat alias        (it stores DS through it)
    //     0x0024  the PSP               (the command tail is at PSP:0x80)
    //     0x002C  the environment       (walked from offset 0; argv[0] is in it)
    //
    // MEASURED on The Dig, DIG.EXE, LE at file offset 0x2c90. Its startup does
    //     mov ax,0x24 / mov [_pspsel],ax            (entry+0x8A)
    //     ...
    //     o16 mov es,[_pspsel] / mov cl,[es:edi-1]  (entry+0x1B0, EDI = 0x81)
    // so the tail LENGTH byte is ES:0x80. With no descriptor for 0x24 the
    // resolver returned "not mine", the core fell back to base 0, and that
    // read landed on guest linear 0x80: interrupt vector 20h. The Dig parsed
    // the vector table as its command line and exited 255 with
    // "Unknown flag: '<two unprintable bytes>'".
    //
    // These are REAL DESCRIPTORS in the host's own table, not constants
    // special-cased in the resolver. That is the difference between "0x24
    // works" and "a selector that is not one of ours still fails", which is
    // the property that made this bug findable in the first place.
    {
        char dospath[80];
        dos32_dospath(path, t->svc.cur_drive, dospath, (int)sizeof dospath);
        uint32_t env_len = dos32_build_env(t, DOS4GW_ENV_LIN, dospath);
        if (env_len > DOS4GW_ENV_MAX) {
            kprintf("[4GW] %s: environment block is %u bytes, which does not fit the "
                    "%u reserved below the transfer buffer. Refusing.\n",
                    path, env_len, DOS4GW_ENV_MAX);
            return -1;
        }
        int e17 = dpmi_seed_desc_rs(0x0017, 0, 0xFFFFFFFFu, 0xF3, 0xC0);
        int e24 = dpmi_seed_desc_rs(0x0024, (uint32_t)DOS_PSP_SEG << 4, 0xFFu, 0xF3, 0x40);
        int e2c = dpmi_seed_desc_rs(0x002C, DOS4GW_ENV_LIN, env_len - 1u, 0xF3, 0x40);
        if (e17 || e24 || e2c) {
            kprintf("[4GW] %s: could not seed the fixed startup selectors "
                    "(0017=%d 0024=%d 002C=%d). Refusing to enter a guest that "
                    "would read its command line out of the vector table.\n",
                    path, e17, e24, e2c);
            return -1;
        }
        // PSP[0x2C] HOLDS A SELECTOR FOR A PROTECTED-MODE GUEST, not a
        // paragraph: the extender it replaces allocates a descriptor for the
        // environment and patches the PSP with it. The type-0 arm above does
        // not read this word (it uses the constant), but the DOS/4G and DPMI
        // arms do, and nothing in this tree reads PSP[0x2C] as a paragraph.
        wr16(t, DOS_PSP_SEG, 0x2C, 0x002C);
        kprintf("[4GW] %s: fixed startup selectors seeded: 0017 -> flat base 0 "
                "(4 GB), 0024 -> PSP at 0x%08x (command tail '%s' at PSP:0x80, "
                "length %u), 002C -> environment, %u bytes at 0x%08x, argv[0] = "
                "'%s'\n",
                path, (uint32_t)DOS_PSP_SEG << 4,
                g_dos_cmdtail[0] ? g_dos_cmdtail : "(none)",
                (uint32_t)t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x80],
                env_len, DOS4GW_ENV_LIN, dospath);
    }

    kprintf("[4GW] %s: LE guest ready. arena %u KiB (linear 0x00000000..0x%08x), "
            "entry CS:EIP=0x%08x SS:ESP=0x%08x, DPMI heap 0x%08x..0x%08x, "
            "DOS memory %u KiB from paragraph 0x%04x\n",
            path, total >> 10, total, t->le_cpu.eip, t->le_cpu.regs[X32_ESP],
            (uint32_t)top, total - DOS4GW_STACK_RESERVE,
            (0xA000u - DOS4GW_DOSMEM_FLOOR) >> 6, DOS4GW_DOSMEM_FLOOR);
    t->le_active = 1;
    return 0;
}


// ===========================================================================
// #211: THE go32 / DJGPP v2 GUEST
//
// Same host, same run loop, same interrupt routing as the DOS/4GW guest above.
// Two things differ, and both follow from ONE fact about the format.
//
// THE FACT: a DJGPP COFF HAS NO RELOCATIONS (f_flags carries F_RELFLG on every
// image djgpp emits, and the measured NetHack has nreloc == 0 in every
// section). Its addresses are OFFSETS INSIDE ITS OWN SEGMENT and cannot be
// slid. `.text` starts at 0x10D0, `.data` at 0x176C00, `.bss` runs to
// 0x1A9600.
//
// CONSEQUENCE 1: THE SEGMENT BASE CANNOT BE ZERO. An LE module is relocated by
// +1 MiB so it can be flat (dos4gw_prepare passes DOS4GW_LOW_SIZE to
// le_relocate_rs). A COFF cannot be, and a flat go32 guest would therefore
// have its 1.5 MB of code laid across the guest's own first megabyte: over the
// interrupt vector table at 0, over the PSP at 0x1000, over the INT 21h
// transfer buffer at 0x10000 and straight through the VGA aperture at 0xA0000,
// where every store would be intercepted by the EGA plane hook and never reach
// the instruction stream that follows it. So CS/DS/ES/SS get a real DESCRIPTOR
// BASE of GO32_LOAD_BASE and the guest's addresses are segment offsets, which
// is exactly what a real DPMI host gives a djgpp program.
//
// That works because rustkern/x86_32.rs adds seg_base[] to every effective
// address INCLUDING the instruction fetch (fetch8 uses seg_base[S_CS]), so a
// non-zero code base is expressible without a core change. It also means the
// DOS-memory selector djgpp allocates for itself (base 0, limit 0x10FFFF, from
// 0000 + 0008) still reaches the first megabyte, which is where the BDA
// keyboard ring at 0x41E and the text page at 0xB8000 actually are.
//
// CONSEQUENCE 2: FS MUST POINT AT A stubinfo. The eleventh instruction djgpp's
// crt0 executes is `mov esi, fs:0x18`. The address of that structure is
// communicated ONLY through FS's descriptor base: not a register, not a fixed
// address, not a pointer on the stack. Get it wrong and the failure is not a
// fault, it is a program that reads four plausible zeroes and sizes its heap
// and its stack to nothing.
//
// WHY THE STUB IS NOT RUN. The MZ half of the file is `go32stub, v 2.04`,
// whose whole job is to find a DPMI host: INT 2Fh AX=1687h, and on failure
// load CWSDPMI.EXE, a real-mode program that switches the CPU into protected
// mode itself. exec/x86_16.c cannot execute a mode switch, so there is no
// version of running the stub that ends anywhere but a derail. #211's first
// pass recorded the matching rule for the other end of this: dosexec.c's INT
// 2Fh 1687h answer stays 0xFFFF, because a truthful refusal on the REAL-MODE
// route is still correct - a 16-bit guest genuinely cannot get a DPMI host out
// of us. This path never goes near INT 2Fh.
// ===========================================================================

// The program's segment base, which is also the top of the guest's first
// megabyte. Equal to DOS4GW_LOW_SIZE by construction, not by coincidence:
// below it is the real-mode-addressable window every DOS service uses.
#define GO32_LOAD_BASE     DOS4GW_LOW_SIZE

// The guest's own DOS transfer buffer. djgpp calls it __tb and reaches it as
// stubinfo.ds_segment << 4; every DOS file operation the C library performs
// copies its filename and its data through it. It is NOT the bridge's transfer
// buffer at DOS4GW_XFER_LIN: that one belongs to dos4gw_int21_pre_rs and is
// used for marshalling a 32-bit pointer down to a seg:off, and sharing them
// would mean a filename and its own marshalled copy occupying one buffer.
#define GO32_TB_LIN        0x00020000u
#define GO32_TB_LEN        0x00004000u
#define GO32_TB_SEG        ((uint16_t)(GO32_TB_LIN >> 4))

// The stubinfo FS points at, the exit stub cs_selector points at, and the
// environment block the PSP points at. All three are DOS memory, below the
// allocator's floor, so nothing can be handed them by AH=48h.
#define GO32_SI_LIN        0x00024000u
#define GO32_EXIT_LIN      0x00024100u
#define GO32_ENV_LIN       0x00024200u
#define GO32_ENV_SEG       ((uint16_t)(GO32_ENV_LIN >> 4))
#define GO32_DOSMEM_FLOOR  ((uint16_t)0x2500)

// The program's segment, and the DPMI 0501 pool above it.
//
// 12 MiB IS SIZED FROM THE BINARY, NOT ROUND. djgpp's ___brk hands out memory
// from inside the initial segment and only calls DPMI 0501 when the break
// would pass stubinfo.initial_size, so a segment large enough for the image,
// the program's own minstack (2 MiB for the measured NetHack, read from its
// stubinfo template rather than assumed) and its heap means the 0501 path -
// which relocates the whole segment base and is the most delicate code in
// djgpp's runtime - is never entered at all. 1.7 MiB image + 2 MiB stack
// leaves over 8 MiB of C heap, which is more than a text-mode roguelike has
// ever wanted.
#define GO32_SEG_SIZE      (12u * 1024u * 1024u)
#define GO32_HEAP_SIZE     (4u * 1024u * 1024u)

_Static_assert(DOS4GW_ENV_LIN >= ((uint32_t)DOS_PSP_SEG << 4) + 0x100u,
               "the 32-bit guest's environment block must start above the PSP");
_Static_assert(DOS4GW_ENV_LIN + DOS4GW_ENV_MAX <= DOS4GW_XFER_LIN,
               "the 32-bit guest's environment block must end below the INT 21h "
               "transfer buffer, which is overwritten by every marshalled call");
_Static_assert(DOS4GW_ENV_LIN + DOS4GW_ENV_MAX
                   <= ((uint32_t)DOS4GW_DOSMEM_FLOOR << 4),
               "the environment must be below the MCB allocator's floor, or "
               "AH=48h can hand a guest its own argv[0]");

_Static_assert(GO32_LOAD_BASE == DOS4GW_LOW_SIZE,
               "the program segment must start exactly above the guest's "
               "real-mode-addressable megabyte");
_Static_assert(GO32_TB_LIN >= DOS4GW_XFER_LIN + DOS4GW_XFER_LEN,
               "the guest's __tb must not overlap the bridge's own transfer "
               "buffer: a filename would share bytes with its marshalled copy");
_Static_assert(GO32_SI_LIN >= GO32_TB_LIN + GO32_TB_LEN,
               "the stubinfo must not sit inside __tb, which the guest fills "
               "with a filename on its first file open");
_Static_assert(GO32_ENV_LIN >= GO32_EXIT_LIN + 16u,
               "the environment must not overlap the exit stub");
_Static_assert(((uint32_t)GO32_DOSMEM_FLOOR << 4) >= GO32_ENV_LIN + 0x100u,
               "the MCB allocator's floor must be above everything this loader "
               "reserved, or the first AH=48h hands out the stubinfo");
_Static_assert(GO32_LOAD_BASE + GO32_SEG_SIZE + GO32_HEAP_SIZE <= DOS4GW_ARENA_MAX,
               "the whole go32 arena must fit the ceiling one guest may reach");
_Static_assert(GO32_STUBINFO_SIZE == 0x54,
               "djgpp's crt0 copies stubinfo.size bytes and crt1 indexes fixed "
               "offsets inside it; a different length is a different structure");

// Allocate ONE descriptor and give it a base, access rights and a limit,
// THROUGH THE HOST'S OWN INT 31h PATH.
//
// Not by reaching into rustkern/dpmi.rs's table: the selectors this loader
// hands the program must be the same kind of object the program's own
// 0000/0007/0008/0009 calls produce, or there are two ways to make a
// descriptor and two sets of rules for what one contains. It also means the
// loader is exercising the services the guest is about to use, at boot, on
// every launch.
//
// ORDER MATTERS: 0009 before 0008. 0008 sets or clears the granularity bit in
// the stored extended byte according to the limit, and 0009 overwrites that
// byte wholesale. Doing them the other way round produces a 4 GB selector
// recorded as byte-granular, which LSL then reports 4096 times too small.
//
// Returns 0 on any failure, which is never a valid selector here because
// rustkern/dpmi.rs never hands out index 0.
static uint16_t go32_make_sel(uint32_t base, uint32_t byte_limit, uint8_t ar,
                              uint8_t ext, const char *what) {
    dpmi_regs_t r;
    memset(&r, 0, sizeof r);
    r.eax = 0x0000; r.ecx = 1;
    dpmi_int31_rs(&r);
    if (r.eflags & 1u) {
        kprintf("[go32] %s: DPMI 0000 (allocate descriptor) FAILED, AX=%04x\n",
                what, (unsigned)(r.eax & 0xFFFFu));
        return 0;
    }
    uint16_t sel = (uint16_t)(r.eax & 0xFFFFu);

    memset(&r, 0, sizeof r);
    r.eax = 0x0007; r.ebx = sel;
    r.ecx = base >> 16; r.edx = base & 0xFFFFu;
    dpmi_int31_rs(&r);
    if (r.eflags & 1u) {
        kprintf("[go32] %s: DPMI 0007 (set base 0x%08x) FAILED, AX=%04x\n",
                what, base, (unsigned)(r.eax & 0xFFFFu));
        return 0;
    }

    memset(&r, 0, sizeof r);
    r.eax = 0x0009; r.ebx = sel;
    r.ecx = (uint32_t)ar | ((uint32_t)ext << 8);
    dpmi_int31_rs(&r);
    if (r.eflags & 1u) {
        kprintf("[go32] %s: DPMI 0009 (access rights %02x/%02x) FAILED, AX=%04x\n",
                what, ar, ext, (unsigned)(r.eax & 0xFFFFu));
        return 0;
    }

    memset(&r, 0, sizeof r);
    r.eax = 0x0008; r.ebx = sel;
    r.ecx = byte_limit >> 16; r.edx = byte_limit & 0xFFFFu;
    dpmi_int31_rs(&r);
    if (r.eflags & 1u) {
        kprintf("[go32] %s: DPMI 0008 (set limit 0x%08x) FAILED, AX=%04x\n",
                what, byte_limit, (unsigned)(r.eax & 0xFFFFu));
        return 0;
    }
    return sel;
}

// THE PROGRAM'S DOS PATH, which is what argv[0] has to be.
//
// A guest that wants to find its own data files looks at argv[0] and takes the
// directory part. NetHack does exactly that (its HACKDIR defaults to
// exepath(argv[0])) and calls error() if it cannot chdir there, which is what
// "Cannot chdir to .:C:\NETHACK.EXE." on the guest's text page was.
//
// The native path maps to a DOS path the same way dos_run_file() already maps
// it when it seeds the current directory, and it MUST agree with that seeding
// or the guest is told it is in one place and its files are in another.
static void dos32_dospath(const char *path, char cur_drive, char *out, int outlen) {
    const char *pfx = "/WINDIR/DRIVE_";
    const char *rel = path;
    char drive = cur_drive;
    int i = 0;
    while (pfx[i] && path[i] == pfx[i]) i++;
    if (pfx[i] == '\0') {
        char dl = path[i];
        if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 32);
        if (dl >= 'A' && dl <= 'Z' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
            drive = dl;
            rel = path + i + 1;
        }
    }
    while (*rel == '/') rel++;
    if (drive < 'A' || drive > 'Z') drive = 'C';
    int o = 0;
    if (outlen > 4) { out[o++] = drive; out[o++] = ':'; out[o++] = '\\'; }
    while (*rel && o < outlen - 1) {
        char c = *rel++;
        if (c == '/') c = '\\';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[o++] = c;
    }
    out[o] = 0;
}

// The 8.3 NAME half, which is the half stubinfo.argv0 carries. Sixteen bytes is
// not an oversight in that structure: it is a field sized for a basename.
static void go32_basename(const char *dospath, char *out, int outlen) {
    const char *b = dospath;
    for (const char *q = dospath; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    int i = 0;
    while (*b && i < outlen - 1) out[i++] = *b++;
    out[i] = 0;
}

// The environment block, in the DOS layout: NUL-terminated VAR=VALUE strings,
// an empty string to end them, a WORD count of what follows, then the program's
// full path. djgpp copies the whole thing in one go (see the note at the top of
// this change) and takes both getenv() and the directory half of argv[0] from
// it, so the path at the end is not optional decoration.
//
// Returns the total byte length, which the caller must put in stubinfo.env_size
// AND use as the limit of the descriptor it puts in the PSP.
//
// (#740 digsel) THE ADDRESS IS NOW AN ARGUMENT, because BOTH 32-bit loaders
// need this block and they put it in different places. It is one function and
// not two: the LAYOUT is what a guest walks, and a second copy of a layout is
// how two loaders come to disagree about where the WORD count goes.
//
// A NON-EMPTY BLOCK IS THE CORRECT ONE, and that is not a stylistic choice.
// The Watcom walker advances past one string, then tests the next byte for the
// block terminator; handed a block that begins with its own terminator it
// consumes that byte, finds the WORD count non-zero and walks the count and
// the program path as though they were environment strings. Real DOS never
// hands a program an empty environment (COMMAND.COM always exports at least
// PATH and COMSPEC), so a block with these two entries is both what the guest
// expects and what a real one would have contained.
static uint32_t dos32_build_env(dos_task_t *t, uint32_t env_lin, const char *argv0) {
    uint32_t o = env_lin;
    static const char *vars[] = { "PATH=C:\\", "COMSPEC=C:\\COMMAND.COM", 0 };
    for (int v = 0; vars[v]; v++) {
        for (const char *c = vars[v]; *c; c++) t->mem[o++] = (uint8_t)*c;
        t->mem[o++] = 0;
    }
    t->mem[o++] = 0;              // end of the environment strings
    t->mem[o++] = 1; t->mem[o++] = 0;   // DOS 3.0+: one string follows
    for (const char *c = argv0; *c; c++) t->mem[o++] = (uint8_t)*c;
    t->mem[o++] = 0;
    return o - env_lin;
}

// Load a DJGPP v2 COFF image and enter it in 32-bit protected mode.
//
// On EVERY return path t->mem is a valid, kmalloc'd buffer the caller may
// free, including the failure paths, exactly as dos4gw_prepare() promises: the
// caller's contract is kfree(t->mem) and a function that swapped the pointer
// and then failed would leave it dangling or double-freed.
static int go32_prepare(dos_task_t *t, const char *path, const uint8_t *file,
                        uint32_t size, const go32_image_t *img) {
    // The RUNTIME half of the layout lock. The _Static_assert above pins the C
    // side's idea of the structure; this pins rustc's, which is the one that
    // actually writes the bytes the guest reads.
    if (go32_stubinfo_size_rs() != GO32_STUBINFO_SIZE) {
        kprintf("[go32] %s: stubinfo size disagreement (Rust says %u, C says %u). "
                "Refusing: the guest reads fixed offsets inside this structure.\n",
                path, go32_stubinfo_size_rs(), (unsigned)GO32_STUBINFO_SIZE);
        return -1;
    }

    uint32_t minstack = img->minstack ? img->minstack : 0x40000u;
    if ((uint64_t)img->image_top + minstack + 0x10000u > GO32_SEG_SIZE) {
        kprintf("[go32] %s: image top 0x%08x plus a %u KiB minimum stack does not fit "
                "the %u MiB segment. Refusing rather than starting a guest that will "
                "run out of stack somewhere unrelated.\n",
                path, img->image_top, minstack >> 10, GO32_SEG_SIZE >> 20);
        return -1;
    }

    uint32_t total = GO32_LOAD_BASE + GO32_SEG_SIZE + GO32_HEAP_SIZE;
    uint8_t *arena = (uint8_t *)kmalloc(total);
    if (!arena) {
        kprintf("[go32] %s: arena kmalloc(%u) FAILED\n", path, total);
        return -1;
    }
    memset(arena, 0, total);

    int e = go32_load_rs(img, file, size, arena, total, GO32_LOAD_BASE, GO32_SEG_SIZE);
    if (e != GO32_OK) {
        kprintf("[go32] %s: load failed: %s\n", path, go32_strerror_rs(e));
        kfree(arena);
        return -1;
    }

    // THE SWAP, as dos/dos4gw.h describes it: from here the guest's flat space
    // and the DOS task's first megabyte are the same bytes.
    kfree(t->mem);
    t->mem = arena;
    t->cpu.mem = arena;
    t->le_arena_size = total;
    dos_build_psp(t, path);

    x86_32_init(&t->le_cpu, arena, 0, total);
    t->le_cpu.owner = t;
    x86_32_set_mem_hook(&t->le_cpu, VGA_A000, VGA_A000_END, ega_mem_w32, ega_mem_r32);
    x86_32_set_sel_base_cb(&t->le_cpu, dpmi_sel_lookup_rs);

    t->alloc_top_para = GO32_DOSMEM_FLOOR;
    // (RA4GW) Same omission, same consequence, same fix: see the long note on
    // alloc_floor_para in dos4gw_prepare() above. A go32 guest reaches the same
    // dos_mcb_retop(), so a floor of 0 would put its next AH=48h on the vector
    // table too. Fixed here rather than left as a second instance of a fault
    // already understood.
    t->alloc_floor_para = GO32_DOSMEM_FLOOR;

    memset(&t->le_arena, 0, sizeof t->le_arena);
    t->le_arena.base = arena;
    t->le_arena.size = DOS4GW_LOW_SIZE;
    dpmi_rmcs_bind_arena(&t->svc, &t->le_arena);
    t->svc.has_ivt = 1;

    t->le_state = kmalloc(dos4gw_state_size_rs());
    if (!t->le_state ||
        dos4gw_init_rs(t->le_state, arena, total, DOS4GW_XFER_LIN, DOS4GW_XFER_LEN,
                       GO32_LOAD_BASE + GO32_SEG_SIZE,
                       total - DOS4GW_STACK_RESERVE) != 0) {
        if (t->le_state) { kfree(t->le_state); t->le_state = 0; }
        kprintf("[go32] %s: bridge state init FAILED\n", path);
        return -1;
    }

    dpmi_host_reset_rs();
    dpmi_set_ext_rs(dos4gw_dpmi_ext, t);
    dpmi_bind_dosmem_rs(dos4gw_dosmem_alloc, dos4gw_dosmem_free, t);
    // (#211) DPMI 000B/000C move a raw 8-byte descriptor to and from a buffer
    // in the CLIENT'S OWN memory, which for a go32 guest is above the first
    // megabyte. rustkern/dpmi.rs bounds every such access against ONE bound
    // arena, so it is given the WHOLE flat space here. t->le_arena stays the
    // 1 MiB window, because that one bounds the seg:off pointers INSIDE an
    // RMCS and must not grow.
    memset(&t->flat_arena, 0, sizeof t->flat_arena);
    t->flat_arena.base = arena;
    t->flat_arena.size = total;
    dpmi_bind_arena_rs(&t->flat_arena);

    // ---- the descriptors -------------------------------------------------
    // CS and DS get a 4 GB limit because djgpp's crt0 immediately asks for one
    // (0008 with CX:DX = FFFF:FFFF on its DS alias) and then LSLs it to find
    // out whether it got it. Refusing would not break it, but there is nothing
    // to protect: the interpreter bounds every access against the arena
    // window, which is a real check, unlike a limit nothing enforces.
    uint16_t cs_sel  = go32_make_sel(GO32_LOAD_BASE, 0xFFFFFFFFu, 0xFB, 0xC0, "program CS");
    uint16_t ds_sel  = go32_make_sel(GO32_LOAD_BASE, 0xFFFFFFFFu, 0xF3, 0xC0, "program DS/ES/SS");
    uint16_t fs_sel  = go32_make_sel(GO32_SI_LIN, GO32_STUBINFO_SIZE - 1u, 0xF3, 0x40, "FS (stubinfo)");
    uint16_t tb_sel  = go32_make_sel(GO32_TB_LIN, GO32_TB_LEN - 1u, 0xF3, 0x40, "stub DS (transfer buffer)");
    uint16_t psp_sel = go32_make_sel((uint32_t)DOS_PSP_SEG << 4, 0xFFu, 0xF3, 0x40, "PSP");
    uint16_t xcs_sel = go32_make_sel(GO32_EXIT_LIN, 0xFFu, 0xFB, 0x40, "exit stub CS");
    if (!cs_sel || !ds_sel || !fs_sel || !tb_sel || !psp_sel || !xcs_sel) {
        kprintf("[go32] %s: could not build the entry descriptors. Refusing to "
                "enter a guest whose FS does not name its stubinfo.\n", path);
        return -1;
    }

    // ---- the exit path ---------------------------------------------------
    // djgpp's ___exit copies sixteen bytes of REAL-MODE code into the stub's
    // DOS block and far-jumps to cs_selector:0 to run them. Those bytes are
    // 16-bit (`b8 01 00` is `mov ax,1`), and this core would decode them as
    // `mov eax,0x31cd0001`. So cs_selector points somewhere ELSE: at six bytes
    // that mean the same thing in both operand sizes.
    //   88 D0  mov al, dl      (dl holds the exit code)
    //   B4 4C  mov ah, 4Ch
    //   CD 21  int 21h
    // The guest's own sixteen bytes still land in the DOS block, where they
    // are harmless, and the far jump lands here instead.
    {
        static const uint8_t exit_stub[6] = { 0x88, 0xD0, 0xB4, 0x4C, 0xCD, 0x21 };
        for (unsigned i = 0; i < sizeof exit_stub; i++)
            arena[GO32_EXIT_LIN + i] = exit_stub[i];
    }

    // ---- the stubinfo ----------------------------------------------------
    // The full path goes in the ENVIRONMENT (which has room for it and is
    // where djgpp reads the directory from); the basename goes in the
    // stubinfo. Two halves of one answer, in the two places go32 puts them.
    char dospath[80], base[16];
    dos32_dospath(path, t->svc.cur_drive, dospath, (int)sizeof dospath);
    go32_basename(dospath, base, (int)sizeof base);
    if (GO32_ENV_LIN + 0x200u > ((uint32_t)GO32_DOSMEM_FLOOR << 4)) {
        kprintf("[go32] %s: the environment area overlaps the DOS memory pool\n", path);
        return -1;
    }
    uint32_t env_len = dos32_build_env(t, GO32_ENV_LIN, dospath);

    // PSP[0x2C] HOLDS A SELECTOR HERE, NOT A PARAGRAPH. See the note at the top
    // of this change: djgpp reads that word and passes it straight to movedata
    // as a source selector, because the go32 stub it is replacing allocates a
    // descriptor for the environment and patches the PSP with it.
    uint16_t env_sel = go32_make_sel(GO32_ENV_LIN, env_len - 1u, 0xF3, 0x40,
                                     "environment");
    if (!env_sel) {
        kprintf("[go32] %s: no descriptor for the environment; the guest would "
                "copy %u bytes from a selector that is not one of ours.\n",
                path, env_len);
        return -1;
    }
    wr16(t, DOS_PSP_SEG, 0x2C, env_sel);

    uint32_t minkeep = img->minkeep;
    if (minkeep == 0 || minkeep > GO32_TB_LEN) minkeep = GO32_TB_LEN;

    uint8_t si[GO32_STUBINFO_SIZE];
    // memory_handle is a TOKEN, not an address. The guest stores it and passes
    // it back to 0502 (free memory block) on the way out and to 0507 (set page
    // attributes) on the way in; neither reaches a real block here, and both
    // are answered without consulting it. A non-zero value is used so that a
    // guest testing for "no handle" sees a handle.
    if (go32_stubinfo_build_rs(si, sizeof si, minstack, 1u, GO32_SEG_SIZE,
                               (uint16_t)minkeep, tb_sel, GO32_TB_SEG,
                               psp_sel, xcs_sel, (uint16_t)env_len,
                               base) != GO32_OK) {
        kprintf("[go32] %s: stubinfo build FAILED\n", path);
        return -1;
    }
    for (unsigned i = 0; i < sizeof si; i++) arena[GO32_SI_LIN + i] = si[i];

    // ---- the entry state -------------------------------------------------
    t->le_cpu.eip = img->entry;
    // The stack the stub hands over. crt0 runs on it only until it sbrk's its
    // own (minstack bytes) and switches, so it is at the top of the segment
    // where nothing else will be placed.
    t->le_cpu.regs[X32_ESP] = GO32_SEG_SIZE - 16u;
    t->le_cpu.seg[X32_CS] = cs_sel;
    t->le_cpu.seg[X32_DS] = ds_sel;
    t->le_cpu.seg[X32_ES] = ds_sel;
    t->le_cpu.seg[X32_SS] = ds_sel;
    t->le_cpu.seg[X32_FS] = fs_sel;
    t->le_cpu.seg[X32_GS] = 0;
    t->le_cpu.seg_base[X32_CS] = GO32_LOAD_BASE;
    t->le_cpu.seg_base[X32_DS] = GO32_LOAD_BASE;
    t->le_cpu.seg_base[X32_ES] = GO32_LOAD_BASE;
    t->le_cpu.seg_base[X32_SS] = GO32_LOAD_BASE;
    t->le_cpu.seg_base[X32_FS] = GO32_SI_LIN;
    t->le_cpu.seg_base[X32_GS] = 0;

    kprintf("[go32] %s: argv[0] = '%s' (basename '%s'), environment %u bytes at "
            "flat 0x%08x through selector %04x in PSP[0x2C]; a guest that derives "
            "its data directory from argv[0] will look there\n",
            path, dospath, base, env_len, GO32_ENV_LIN, env_sel);
    kprintf("[go32] %s: DJGPP guest ready. arena %u KiB, segment base 0x%08x size %u KiB, "
            "entry CS:EIP=%04x:%08x SS:ESP=%04x:%08x, FS=%04x -> stubinfo at flat 0x%08x, "
            "__tb %04x:0000 (%u bytes), PSP sel %04x, minstack %u KiB, DPMI heap "
            "0x%08x..0x%08x\n",
            path, total >> 10, GO32_LOAD_BASE, GO32_SEG_SIZE >> 10,
            cs_sel, t->le_cpu.eip, ds_sel, t->le_cpu.regs[X32_ESP],
            fs_sel, GO32_SI_LIN, GO32_TB_SEG, minkeep, psp_sel, minstack >> 10,
            GO32_LOAD_BASE + GO32_SEG_SIZE, total - DOS4GW_STACK_RESERVE);
    {   // The one line that says the entry contract is really in place, read
        // back OUT OF GUEST MEMORY through the same accessor the guest uses,
        // rather than out of the local that was just written.
        uint8_t chk[8];
        if (x86_32_read_guest(&t->le_cpu, GO32_SI_LIN, chk, 8) == 0)
            kprintf("[go32] stubinfo magic in guest memory: %c%c%c%c%c%c%c%c "
                    "(FS:0x18 will read the memory handle, FS:0x1C the segment size)\n",
                    chk[0], chk[1], chk[2], chk[3], chk[4], chk[5], chk[6], chk[7]);
    }
    t->le_active = 1;
    t->go32_active = 1;
    t->go32_trace_n = 0;
    for (unsigned i = 0; i < 256; i++) { t->go32_us[i] = 0; t->go32_calls[i] = 0; }
    for (unsigned i = 0; i < GO32_TRACE_RING; i++) {
        t->go32_tr_vec[i] = 0; t->go32_tr_ax[i] = 0;
        t->go32_tr_eip[i] = 0; t->go32_tr_esp[i] = 0; t->go32_tr_edi[i] = 0;
    }
    return 0;
}

static const char *dos4gw_exit_name(uint32_t r) {
    switch (r) {
        case X32_EXIT_BUDGET:      return "BUDGET";
        case X32_EXIT_STOP_EIP:    return "STOP_EIP";
        case X32_EXIT_INT:         return "INT";
        case X32_EXIT_HLT:         return "HLT";
        case X32_EXIT_IO_IN:       return "IO_IN";
        case X32_EXIT_IO_OUT:      return "IO_OUT";
        case X32_EXIT_MISS:        return "MISS";
        case X32_EXIT_FAULT_UD:    return "FAULT_UD";
        case X32_EXIT_FAULT_MEM:   return "FAULT_MEM";
        case X32_EXIT_FAULT_DIV:   return "FAULT_DIV";
        case X32_EXIT_FAULT_LIMIT: return "FAULT_LIMIT";
        default:                   return "?";
    }
}

// INT 21h AH=3Fh / AH=40h with a count that does not fit the 16-bit core.
//
// THE BUG THIS FIXES WAS A SILENT SUCCESS, which is the worst shape available.
// The bridge marshals a buffered call through the existing 16-bit service core,
// whose count lives in CX. A 32-bit client's count lives in ECX and is not
// bounded by 16 bits, so `dos4gw_int21_pre_rs` took `ECX & 0xFFFF` and the call
// SUCCEEDED, transferring the truncated amount and reporting it as the amount
// transferred. MEASURED: DOOM asked for 68168 bytes of lump 235, the bridge
// asked for 2632 (68168 & 0xFFFF), the read returned 2632, and DOOM printed
// "W_ReadLump: only read 2632 of 68168 on lump 235" and exited with code 1.
//
// Enlarging the transfer window does NOT fix it and it is worth saying why: CX
// is sixteen bits, so no single call through this core can move more than
// 0xFFFF bytes however large the window becomes. The count has to be split.
//
// Returns 1 if it handled the call (the ordinary path must not also run), 0 if
// the request fits and the ordinary path should take it.
static int dos4gw_int21_bulk(dos_task_t *t, uint32_t ah) {
    uint32_t total = t->le_cpu.regs[X32_ECX];
    uint32_t maxch = dos4gw_xfer_max_rs();
    if (total <= maxch) return 0;

    uint32_t base = t->le_cpu.regs[X32_EDX];
    uint32_t eax0 = t->le_cpu.regs[X32_EAX];
    uint32_t done = 0;
    int cf = 0;

    while (done < total) {
        uint32_t chunk = total - done;
        if (chunk > maxch) chunk = maxch;
        t->le_cpu.regs[X32_EAX] = (eax0 & 0xFFFF00FFu) | (ah << 8);
        t->le_cpu.regs[X32_ECX] = chunk;
        t->le_cpu.regs[X32_EDX] = base + done;
        if (!dos4gw_int21_pre_rs(t->le_state, &t->le_cpu,
                                 (struct x86_16_cpu *)&t->cpu)) { cf = 1; break; }
        dos_svc_int21(&t->svc, &t->cpu);
        dos4gw_int21_post_rs(t->le_state, &t->le_cpu,
                             (const struct x86_16_cpu *)&t->cpu);
        if (t->le_cpu.eflags & 1u) { cf = 1; break; }   // CF: a real DOS error
        uint32_t got = t->le_cpu.regs[X32_EAX] & 0xFFFFu;
        done += got;
        // A SHORT CHUNK ENDS THE TRANSFER and is not an error: it is end of
        // file for a read, or a full disk for a write, and both are reported by
        // the byte count rather than by CF. Continuing would spin on a handle
        // that has nothing left to give.
        if (got < chunk) break;
    }

    // Put the guest's own registers back the way a 32-bit DOS extender leaves
    // them: ECX and EDX as they were, and the FULL 32-bit transferred count in
    // EAX, which is what a DPMI client reads and what the 16-bit path cannot
    // express.
    t->le_cpu.regs[X32_ECX] = total;
    t->le_cpu.regs[X32_EDX] = base;
    t->le_cpu.regs[X32_EAX] = done;
    if (cf) t->le_cpu.eflags |= 1u; else t->le_cpu.eflags &= ~1u;

    {   static int said = 0;
        if (said < 4) {
            said++;
            kprintf("[4GW] INT 21h AH=%02Xh chunked: %u bytes requested, %u moved in "
                    "%u-byte pieces%s (CX is 16 bits; the whole count never fit)\n",
                    ah, total, done, maxch, cf ? ", ended on CF" : "");
        }
    }
    return 1;
}

// Service one software interrupt taken by the 32-bit guest.
//
// THE ROUTING TABLE, AND NOTHING ELSE. Each vector goes to the ONE existing
// implementation of that service; nothing is implemented here.
//   INT 21h -> dos_svc_int21()      (dos/int21svc.c, #736, #713)
//   INT 31h -> dpmi_int31_rs()      (rustkern/dpmi.rs), whose extension hook
//              reaches dpmi_rmcs_call_rs() for 0300h and our own 05xx memory
//   others  -> dos_int_handler()    (this file: INT 10h/16h/33h and the rest)
// Anything with no route is refused with that vector's own stub effect.
// (#211) THE FIRST N SERVICE CALLS, WITH THE EIP THAT MADE THEM.
//
// A MISS histogram answers "what did we not implement". It cannot answer "why
// is the guest doing this at all", which is the question a repeating,
// fully-routed service pattern asks. This prints the caller's EIP so the
// answer can be read straight out of the binary's own symbol table.
#define GO32_TRACE_MAX 96u
static void go32_trace(dos_task_t *t, uint32_t vec) {
    // (#rafault) NO LONGER GATED ON go32_active. This ring is the only record
    // of WHERE a 32-bit guest asked for a service, and a DOS/4GW guest that
    // derails needs it for exactly the reason a go32 one does. Five stores per
    // service call is not a cost worth a gate; the DUMP is still gated, so a
    // healthy run prints nothing new.
    uint32_t i = t->go32_trace_n % GO32_TRACE_RING;
    t->go32_tr_vec[i] = (uint8_t)vec;
    t->go32_tr_ax[i]  = (uint16_t)(t->le_cpu.regs[X32_EAX] & 0xFFFFu);
    t->go32_tr_eip[i] = t->le_cpu.eip;
    t->go32_tr_esp[i] = t->le_cpu.regs[X32_ESP];
    t->go32_tr_edi[i] = t->le_cpu.regs[X32_EDI];
    t->go32_trace_n++;
}

// THE GUEST'S LAST WORDS.
//
// A DOS program that gives up prints a sentence and exits. The sentence goes to
// the 80x25 text page at 0xB8000, and the host window is destroyed as soon as
// the guest exits, so a screendump taken a second later shows the desktop and
// the sentence is gone. This prints the page as text, once, at teardown, with
// trailing blanks trimmed and blank lines dropped, so "exit=1" becomes a reason.
//
// It reads the SAME bytes dos_present_text() draws (there is no shadow copy),
// so what this prints is what was on the screen.
static void go32_dump_text_page(dos_task_t *t) {
    if (!t->go32_active || !t->mem) return;
    char line[TEXT_COLS + 1];
    int printed = 0;
    for (int r = 0; r < TEXT_ROWS; r++) {
        int last = -1;
        for (int c = 0; c < TEXT_COLS; c++) {
            uint8_t ch = t->mem[dos_text_cell(t, r, c)];
            if (ch == 0) ch = ' ';
            line[c] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
            if (line[c] != ' ') last = c;
        }
        if (last < 0) continue;
        line[last + 1] = 0;
        if (!printed) {
            printed = 1;
            kprintf("[go32t] the guest's text page at teardown "
                    "(blank rows omitted; this is what was on screen):\n");
        }
        kprintf("[go32t] %02d| %s\n", r, line);
    }
    if (!printed)
        kprintf("[go32t] the guest's text page is entirely blank at teardown\n");
}

// Printed ONCE, when the guest has stopped. EIP is the address of the
// instruction AFTER the INT, which is what the binary's own symbol table
// resolves to a function name.
// What the guest's service calls COST. Printed once, at teardown.
//
// The last line is the one #211 asked for: a full 80x25 text redraw through
// this guest's own drawing path, priced from the MEASURED cost of the calls it
// actually makes rather than from an assumption about which mechanism it uses.
// djgpp's int86() is a DIRECT protected-mode INT (it patches the instruction
// byte and executes it), so a video call costs one trip through this router,
// NOT a DPMI 0300h real-mode reflection.
static void go32_cost_dump(dos_task_t *t) {
    if (!t->go32_active) return;
    uint64_t total = 0;
    uint32_t calls = 0;
    for (int v = 0; v < 256; v++) { total += t->go32_us[v]; calls += t->go32_calls[v]; }
    if (!calls) return;
    kprintf("[go32t] service-call cost: %u calls, %u us total\n", calls, (uint32_t)total);
    for (int v = 0; v < 256; v++) {
        if (!t->go32_calls[v]) continue;
        uint32_t n = t->go32_calls[v];
        uint32_t us = (uint32_t)t->go32_us[v];
        kprintf("[go32t]   INT %02Xh: %u calls, %u us total, %u.%02u us each\n",
                v, n, us, us / n, (uint32_t)(((uint64_t)(us % n) * 100u) / n));
    }
    if (t->go32_calls[0x10]) {
        uint32_t n = t->go32_calls[0x10];
        uint32_t us = (uint32_t)t->go32_us[0x10];
        // txt_xputs() per character: gotoxy, get_cursor, write-char, gotoxy,
        // gotoxy - five INT 10h calls, measured by reading _txt_xputc and
        // _txt_xputs in the binary, not assumed.
        uint64_t redraw = (uint64_t)us * 5ull * 80ull * 25ull / (uint64_t)n;
        kprintf("[go32t]   a full 80x25 redraw at 5 INT 10h calls per cell is "
                "%u BIOS calls costing about %u ms of service time\n",
                5u * 80u * 25u, (uint32_t)(redraw / 1000ull));
    }
}

static void go32_trace_dump(dos_task_t *t) {
    if (t->go32_trace_n == 0) return;
    uint32_t total = t->go32_trace_n;
    uint32_t n = total < GO32_TRACE_RING ? total : GO32_TRACE_RING;
    uint32_t first = total - n;
    kprintf("[go32t] the last %u of %u service calls the guest made "
            "(EIP is the instruction AFTER the INT):\n", n, total);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (first + k) % GO32_TRACE_RING;
        kprintf("[go32t] %4u INT %02Xh AX=%04x EIP=%08x ESP=%08x EDI=%08x\n",
                first + k + 1, t->go32_tr_vec[i], t->go32_tr_ax[i],
                t->go32_tr_eip[i], t->go32_tr_esp[i], t->go32_tr_edi[i]);
    }
}
// (#211) The service router, wrapped so the cost of each vector is measured
// rather than estimated. dos4gw_service_int() is the ONE entry point for every
// interrupt a 32-bit guest executes, so one wrapper covers all of them and
// there is no per-vector bookkeeping to forget.
static void dos4gw_service_int_inner(dos_task_t *t, uint32_t vec);

static void dos4gw_service_int(dos_task_t *t, uint32_t vec) {
    if (!t->go32_active) { dos4gw_service_int_inner(t, vec); return; }
    uint64_t t0 = mono_us();
    dos4gw_service_int_inner(t, vec);
    t->go32_us[vec & 0xFF] += mono_us() - t0;
    t->go32_calls[vec & 0xFF]++;
}

static void dos4gw_service_int_inner(dos_task_t *t, uint32_t vec) {
    go32_trace(t, vec);
    // (#221) see dos_int_handler(): cleared before the service, read after.
    t->svc.input_blocked = 0;
    if (vec == 0x31) {
        dpmi_regs_t r;
        memset(&r, 0, sizeof r);
        dos4gw_int31_pre_rs(t->le_state, &t->le_cpu, (struct dpmi_regs *)&r);
        dpmi_int31_rs(&r);
        dos4gw_int31_post_rs(&t->le_cpu, (struct dpmi_regs *)&r);
        return;
    }

    // The 16-bit frame. THE SAME x86_16_cpu_t the 16-bit DOS task uses, so
    // dos_int_handler() finds its task through cpu->owner exactly as it always
    // has (#736 Stage 1b), and every hook that reads the task back out of the
    // cpu keeps working with no case for "who is asking".
    uint16_t save_flags = t->cpu.flags;
    if (vec == 0x21) {
        uint32_t ah = (t->le_cpu.regs[X32_EAX] >> 8) & 0xFF;
        if ((ah == 0x3F || ah == 0x40) && dos4gw_int21_bulk(t, ah)) return;
        if (!dos4gw_int21_pre_rs(t->le_state, &t->le_cpu,
                                 (struct x86_16_cpu *)&t->cpu)) {
            return;   // refused; the guest's own CF/AX already say so
        }
        dos_svc_int21(&t->svc, &t->cpu);
        // (#221) A blocked console read must leave the 32-bit register file
        // ALONE, so the retry re-runs pre_rs on exactly the registers the guest
        // executed the INT with. Skipping post_rs is how "touch nothing" is
        // expressed here; the run loop reads the flag and re-offers the vector.
        if (t->svc.input_blocked) return;
        dos4gw_int21_post_rs(t->le_state, &t->le_cpu,
                             (const struct x86_16_cpu *)&t->cpu);
        return;
    }

    if (vec == 0x2F)
        kprintf("[CDTRACE] 32-bit guest INT 2Fh eax=%08x ebx=%08x ecx=%08x\n",
                t->le_cpu.regs[X32_EAX], t->le_cpu.regs[X32_EBX],
                t->le_cpu.regs[X32_ECX]);
    if (!dos4gw_int_pre_rs(t->le_state, &t->le_cpu,
                           (struct x86_16_cpu *)&t->cpu, vec)) {
        if (vec == 0x2F)
            kprintf("[CDTRACE] 32-bit INT 2Fh REFUSED by the bridge (MISS)\n");
        t->cpu.flags = save_flags;
        return;
    }
    dos_int_handler_inner(&t->cpu, (uint8_t)vec);
    if (t->svc.input_blocked) { t->cpu.flags = save_flags; return; }   // (#221)
    dos4gw_int_post_rs(t->le_state, &t->le_cpu,
                       (const struct x86_16_cpu *)&t->cpu);
}

// ===========================================================================
// ASYNCHRONOUS INTERRUPTS FOR THE 32-BIT GUEST (#740 M4)
//
// Measured before this existed: DOOM.EXE ran its whole startup, set mode 13h,
// and then made no service call for 800 seconds. No fault, no MISS, no STOP;
// the run loop kept retiring instructions. It was spinning in TryRunTics on a
// tic counter that its OWN timer ISR increments, and the bridge serviced only
// the interrupts the guest EXECUTES. The events-IN direction did not exist.
//
// Three parts, and only one of them is new state:
//   1. WHERE the handler lives. Already recorded: INT 21h AH=25h packs the
//      client's flat 32-bit handler through the frame's (seg, off) pair and
//      int21svc.c stores it in the guest's IVT at linear vec*4. There is no
//      second table here and there must never be one.
//   2. WHEN to deliver. Already built: the emulated PIT (dos_emu_pit_now) is
//      derived from retired guest instructions and now follows whichever
//      interpreter is running, so the 32-bit guest gets IRQ0 at the rate IT
//      programmed the PIT for, not at whatever the host pacing happens to be.
//   3. HOW to deliver. x86_32_inject_int(), plus IRETD in the core so the
//      handler can get back. That is the only genuinely new mechanism.
// ===========================================================================

// Seed the 32-bit guest's vector table with a PROTECTED-MODE no-op handler.
//
// WHY A GUEST NEEDS THIS AT ALL. An ISR does not just service its interrupt, it
// CHAINS: it saves the previous vector with INT 21h AH=35h and, on the tick
// where it wants the old behaviour, does `pushfd` + `call far [saved]`. So the
// value AH=35h hands back is not a token the guest stores and forgets, it is an
// address the guest EXECUTES.
//
// MEASURED, build 1954: with the table left holding the real-mode stub, AH=35h
// returned it packed as the flat address 0xF000FF53, DOOM's timer ISR far-called
// there on its seventh tick, and the run died with FAULT_MEM at EIP 0xF000FF54.
// Implementing CALL FAR turned a MISS into a fault one instruction later, which
// is progress of exactly one instruction and a good demonstration that the
// missing opcode was never the whole problem.
//
// THE STUB ALREADY EXISTS AND IS ALREADY THE RIGHT BYTE. The per-launch setup
// writes 0xCF at real-mode F000:FF53, i.e. guest linear 0xFFF53. 0xCF is IRET
// with a 16-bit operand size and IRETD with a 32-bit one: ONE byte that is a
// correct no-op handler for both guests, so there is no second stub to write
// and no second address to keep in step. All this does is point the 32-bit
// guest's table at it in the encoding a protected-mode client reads.
//
// The address is above the MCB allocator's 0xA0000 ceiling and below the
// module's own load base, so nothing can be handed it and nothing loads over it.
static void dos4gw_seed_pm_ivt(dos_task_t *t) {
    uint8_t stub = 0;
    // Do not seed a table to point at a byte we have not checked is there. If
    // the stub is missing, leaving the table alone is the safer failure: the
    // guest then chains to whatever was there before, which is what it did
    // before this function existed.
    if (x86_32_read_guest(&t->le_cpu, DOS4GW_PM_IRET_LIN, &stub, 1) != 0 || stub != 0xCF) {
        kprintf("[4GW] PM vector seeding SKIPPED: no IRET stub at guest linear 0x%05x "
                "(read 0x%02x, wanted 0xCF). A guest that chains to a saved vector "
                "will fault; that is visible rather than silent.\n",
                DOS4GW_PM_IRET_LIN, stub);
        return;
    }
    // (raplay) NOT ALL 256 VECTORS GET THE IRET, AND THE EXCEPTION IS THE
    // WHOLE POINT.
    //
    // This loop used to write one address into every entry. dosexec.c already
    // records, at the 16-bit IVT seeding, why that is wrong for INT 33h:
    //
    //   "The documented way to detect a mouse driver [...] read the INT 33h
    //    vector; if it is 0000:0000 OR THE BYTE IT POINTS AT IS 0CFh (IRET),
    //    no driver is installed. The loop below pointed every unhooked vector
    //    at an IRET, so a program using that test concluded there was no mouse
    //    and never issued a single INT 33h call."
    //
    // The 16-bit path was fixed for that. This one re-introduced it for every
    // DOS/4GW guest, because it runs AFTER the per-launch seeding and
    // overwrites it. MEASURED on Red Alert: the game reads the vector, finds
    // the 0xCF at flat 0x000fff53, and puts up "Red Alert is unable to detect
    // your mouse driver." It never issues INT 33h at all, so no amount of work
    // in int33() can be reached.
    //
    // dos_vec_seed_stub() is THE chooser and stays the only one; this loop just
    // asks it per vector and converts F000:<off> to the flat form a
    // protected-mode table holds. Vector 33h gets `CD 33 CB`, whose first byte
    // is 0xCD and therefore is not an IRET; vector 08h gets the BIOS timer
    // stub; everything else still gets the IRET, so AH=35h still returns an
    // address a guest can far-call.
    //
    // A stub whose first byte is not what we expect is NOT seeded: that vector
    // keeps the IRET, which is the previous behaviour, and the fact is logged
    // rather than assumed.
    unsigned specific = 0;
    for (unsigned v = 0; v < 256; v++) {
        uint32_t lin = dos4gw_pm_seed_lin((uint8_t)v);
        uint8_t  b   = 0;
        if (lin != DOS4GW_PM_IRET_LIN) {
            if (x86_32_read_guest(&t->le_cpu, lin, &b, 1) != 0 || b == 0xCF) {
                kprintf("[4GW] PM vector %02Xh: stub at flat 0x%05x reads 0x%02x, "
                        "not usable; falling back to the IRET stub\n",
                        v, lin, b);
                lin = DOS4GW_PM_IRET_LIN;
            } else {
                specific++;
            }
        }
        uint8_t e[4] = { (uint8_t)(lin & 0xFF), (uint8_t)((lin >> 8) & 0xFF),
                         (uint8_t)((lin >> 16) & 0xFF), (uint8_t)((lin >> 24) & 0xFF) };
        x86_32_write_guest(&t->le_cpu, (uint32_t)v * 4u, e, 4);
    }
    kprintf("[4GW] PM vector table seeded: 256 vectors -> IRETD stub at flat 0x%08x, "
            "except %u vector-specific (33h mouse -> 0x%05x) so a driver-detect "
            "test does not read IRET\n",
            DOS4GW_PM_IRET_LIN, specific, dos4gw_pm_seed_lin(0x33));
}

// The C twin of rustkern/dos4gw.rs's vec_unpack, and the only place the C side
// performs this transform.
//
// "A NON-ZERO ENTRY MEANS THE GUEST INSTALLED A HANDLER" IS FALSE, and this
// comment used to say it was true. The LE arena IS memset(0) at load, which is
// what made the claim look safe, but the per-launch setup that runs AFTER the
// arena swap seeds the whole vector table with the F000:FF53 real-mode IRET
// stub, exactly as it does for a 16-bit task. MEASURED: the first run of this
// code read vectors 08h, 09h and 1Ch as "hooked by the guest" at instruction
// zero, recomposed the stub into flat 0xF000FF53, and delivered DOOM's first
// timer interrupt to an address outside the arena. It died at EIP 0xF000FF54
// with FAULT_MEM, 1,098,532 instructions in, before it had hooked anything.
//
// dos_vec_hooked() already knew all of this and has since #202. Use it; do not
// write a second answer to the same question. This function only DECODES.
static uint32_t dos4gw_pm_vector(dos_task_t *t, uint8_t vec) {
    uint8_t w[4];
    if (x86_32_read_guest(&t->le_cpu, (uint32_t)vec * 4u, w, 4) != 0) return 0;
    uint32_t off = (uint32_t)w[0] | ((uint32_t)w[1] << 8);
    uint32_t seg = (uint32_t)w[2] | ((uint32_t)w[3] << 8);
    return (seg << 16) | off;
}

// Re-derive which vectors the guest owns, from the TABLE rather than from a
// flag latched inside AH=25h. Same reasoning as dos_refresh_vector_hooks() for
// the 16-bit path: a guest may install a handler by writing the table directly,
// and it may put it back on the way out. One line per genuine edge.
static int dos4gw_pmvec_flat(uint8_t vec, uint32_t *out);

static void dos4gw_refresh_hooks(dos_task_t *t) {
    // (rakbd2) THE SECOND VECTOR SPACE HAS TO BE REFRESHED HERE TOO, BECAUSE
    // THIS FLAG IS WHAT DECIDES WHO DRAINS THE RAW SCANCODE RING.
    //
    // kbd_has_int9 answers "did the guest hook INT 9 in the LOW table". #779b
    // added a second, equally real way to hook it (DPMI 0205h) and a delivery
    // path for it, but nothing taught the OWNERSHIP question about it. So for a
    // 0205h guest kbd_has_int9 stayed 0, dos_keyq_pump() ran on every pass and
    // drained up to 16 scancodes into the BIOS ring, and dos4gw_deliver_int9()
    // then found dos_scancode_get() empty. Two consumers, one stream, and the
    // one that owns the hardware lost every byte.
    //
    // MEASURED on Red Alert with the free-user-vector fix in place and this one
    // absent: "[dpmi] INT 09h PROTECTED-MODE handler installed by the guest
    // (0205h) -> 0000:00251707" and, for the same three keystrokes,
    // "[KBDIO] port60=0 ... ringpush=3 ... bda_tail=0024" - delivered to the
    // ring, consumed by the pump, never seen by the ISR that was installed.
    //
    // It is a SEPARATE flag from kbd_has_int9 on purpose: dos4gw_deliver_int9()
    // uses kbd_has_int9 to choose the LOW-table handler, so folding the two
    // would send a 0205h guest's scancodes to whatever the low table holds.
    {
        uint32_t h9 = 0;
        int pm9 = (dos4gw_pmvec_flat(0x09, &h9) == 0);
        if (pm9 != t->kbd_int9_pm) {
            t->kbd_int9_pm = pm9;
            kprintf("[4GW] INT 09h %s in the DPMI 0205h vector table -> flat 0x%08x "
                    "(the guest now owns the raw scancode stream: BIOS-ring pump %s)\n",
                    pm9 ? "installed" : "removed", h9, pm9 ? "OFF" : "ON");
            bootlog_write("[4GW] INT 09h PM route %s handler=0x%08x",
                          pm9 ? "INSTALLED" : "removed", h9);
        }
    }
    // (#211) kbd_has_int9 decides which of the two consumers drains the raw
    // scancode stream, so it has to be refreshed here too now that the 32-bit
    // loop pumps the BIOS ring. dos_vec_hooked() is THE predicate for both
    // loops and both encodings; there is no 32-bit copy of the question.
    // (rakbd) The pre-assignment that used to be here made the INT 09h edge
    // UNREPORTABLE. It set the flag to the very value the loop below then
    // compares against, so `now != *flag` was false on the one pass that
    // mattered and the "INT 09h hooked" line never printed for any 32-bit
    // guest. The loop already refreshes 0x09 from dos_vec_hooked() exactly like
    // the other two vectors; letting it do so gives both the correct value AND
    // a visible edge. A diagnostic that is structurally incapable of firing is
    // worse than none, because its silence reads as evidence of absence.
    static const uint8_t vecs[3] = { 0x08, 0x09, 0x1C };
    for (int i = 0; i < 3; i++) {
        uint32_t h = dos4gw_pm_vector(t, vecs[i]);
        int *flag = (vecs[i] == 0x08) ? &t->has_int8
                  : (vecs[i] == 0x09) ? &t->kbd_has_int9
                                      : &t->has_int1c;
        // ONE definition of "the guest owns this vector", shared with the
        // 16-bit path. It rejects null and both seeded ROM stubs, which is the
        // whole of the difference between a working timer and a guest killed
        // at its first tick.
        int now = dos_vec_hooked(t, vecs[i]);
        if (now != *flag) {
            *flag = now;
            kprintf("[4GW] INT %02Xh %s by the 32-bit guest -> PM handler 0x%08x\n",
                    vecs[i], now ? "hooked" : "released", h);
        }
    }
}

// Deliver one interrupt. Returns the X32_INJ_* code so the caller can tell a
// masked tick (drop it, the guest is inside a cli region) from a broken one.
// (rakbd) Deliver `vec` to an EXPLICIT flat handler address.
//
// Split out of dos4gw_deliver() rather than copied, because a second route to
// the same guest now exists (the DPMI 0205h protected-mode vector table) and
// every one of the checks below applies to it identically: the arena bound, the
// vector-table bound, the masked/no-fit distinction, and the first-delivery
// line that separates "the mechanism ran" from "the mechanism linked". Forking
// them would have meant the PM route silently lacking the two refusals that
// exist because delivering to a bad pointer once cost 6.5 million instructions
// between cause and symptom.
static int dos4gw_deliver_at(dos_task_t *t, uint8_t vec, uint32_t h) {
    // A HANDLER OUTSIDE THE ARENA IS REFUSED HERE, not discovered by the guest
    // faulting on its first instruction. A protected-mode client's handler is a
    // flat address in its own space by definition, so one that is not in the
    // window is either a real-mode vector we failed to recognise or a guest
    // that has corrupted its own table, and neither is worth killing the run
    // for. Counted as no-fit and reported once.
    if (h >= t->le_arena_size) {
        t->irq_nofit++;
        if (t->irq_nofit == 1)
            kprintf("[4GW] INT %02Xh NOT delivered: handler 0x%08x is outside the arena "
                    "(window 0x00000000..0x%08x). Not a protected-mode vector; the guest "
                    "is left running.\n", vec, h, t->le_arena_size);
        return X32_INJ_FAULT;
    }
    // A HANDLER INSIDE THE VECTOR TABLE IS NOT A HANDLER (#740 dw2). Nothing
    // executable can live in the first 1 KiB, which is the table itself, so a
    // vector pointing there means the table has been corrupted. Delivering to
    // it is how a bad pointer became a derail at EIP 0x0000002c with 6.5
    // million instructions between the cause and the symptom. Refuse it here,
    // where the address is still in a register and can be printed, and leave
    // the guest running: it is no worse off than with no timer.
    if (h < 0x400u) {
        t->irq_nofit++;
        if (t->irq_nofit == 1)
            kprintf("[4GW] INT %02Xh NOT delivered: handler 0x%08x is INSIDE the vector "
                    "table (0x000..0x3FF), so the table has been overwritten. The guest "
                    "is left running rather than being made to execute its own vectors.\n",
                    vec, h);
        return X32_INJ_FAULT;
    }
    uint32_t was_eip = t->le_cpu.eip;          // captured BEFORE, or it is the handler
    int r = x86_32_inject_int(&t->le_cpu, h);
    if (r == X32_INJ_DELIVERED) {
        t->irq_deliv[vec]++;
        // The FIRST delivery of each vector, and only the first. It is the one
        // line that separates "the mechanism ran" from "the mechanism linked",
        // which is this project's characteristic failure.
        if (t->irq_deliv[vec] == 1)
            kprintf("[4GW] first INT %02Xh delivered to the 32-bit guest: handler 0x%08x, "
                    "interrupted EIP 0x%08x, ESP now 0x%08x\n",
                    vec, h, was_eip, t->le_cpu.regs[X32_ESP]);
    } else if (r == X32_INJ_MASKED) {
        t->irq_masked++;
    } else {
        t->irq_nofit++;
        if (t->irq_nofit == 1)
            kprintf("[4GW] INT %02Xh NOT delivered: the interrupt frame does not fit "
                    "at ESP 0x%08x (handler 0x%08x). The guest is left running.\n",
                    vec, t->le_cpu.regs[X32_ESP], h);
    }
    return r;
}

// The ORIGINAL route: the handler is a flat 32-bit address the guest wrote into
// the low table with INT 21h AH=25h, which dos_vec_hooked() can see.
static int dos4gw_deliver(dos_task_t *t, uint8_t vec) {
    if (!dos_vec_hooked(t, vec)) { t->irq_novec++; return X32_INJ_FAULT; }
    return dos4gw_deliver_at(t, vec, dos4gw_pm_vector(t, vec));
}

// (rakbd) THE 32-BIT KEYBOARD ISR, and why dos_deliver_int9() could not simply
// be called from the 32-bit loop the way the other three input calls were.
//
// WHAT WAS BROKEN. #211 gave dos4gw_run() the input pump it had never had, and
// its own comment says it makes "the SAME four calls the 16-bit loop makes".
// It makes three. dos_deliver_int9() is the fourth and it was never added, and
// grep agreed: the only call site in the whole file was the 16-bit loop's.
//
// That is not a cosmetic gap, because the two consumers are MUTUALLY EXCLUSIVE
// by design. dos_keyq_pump() feeds the BIOS ring that INT 16h reads and runs
// only while kbd_has_int9 is clear; dos_deliver_int9() runs the guest's own ISR
// and runs only while it is set. dos4gw_refresh_hooks() maintains that flag for
// 32-bit guests too (it is called from dos4gw_timebase() on every pass), so the
// instant a protected-mode guest hooked INT 9 the pump switched ITSELF off and
// nothing replaced it. Both keyboard channels were dead at once, and the guest
// that caused the switch-off is the one that triggered it.
//
// A guest that reads keys through INT 16h was unaffected, which is exactly why
// this survived #211's verification: NetHack does, and it was the guest the fix
// was tested on. Action titles hook INT 9 instead. MEASURED symptom on Red
// Alert: it reaches gameplay and no key does anything.
//
// WHY NOT JUST CALL dos_deliver_int9(). It reads the REAL-MODE IVT with rd16(),
// pushes a real-mode interrupt frame and runs x86_16_run() to completion against
// t->cpu. A DOS/4GW client's INT 9 is none of those things: the handler is a
// flat 32-bit address in the protected-mode vector table that dos4gw_pm_vector()
// reads, and it is entered by x86_32_inject_int(), which returns immediately and
// lets the handler run in the next interpreter burst. Same question, different
// core, different frame - the same relationship dos4gw_mouse_events() has to
// dos_mouse_events().
//
// ONE SCANCODE PER PASS, DELIBERATELY. dos4gw_timebase() delivers up to four
// IRQ0s in a row because nesting timer frames is harmless. A keyboard cannot do
// that: port 0x60 holds ONE byte (dos_in(): port 0x60 -> t->kbd_port60) and the
// guest's handler does not read it until it actually runs, so injecting a second
// scancode before the first handler has executed would overwrite the first and
// lose it. Deliver one, return, let the guest run it. The loop is back within a
// slice, so held or fast typing drains at interpreter speed, not at the user's.
//
// AND THE BYTE IS LATCHED UNTIL DELIVERY SUCCEEDS. A guest inside a cli region
// returns X32_INJ_MASKED, which is normal and transient; taking the scancode off
// the ring and dropping it there would lose precisely the keystrokes pressed
// while the game was in a critical section.
// (rakbd) THE SECOND ROUTE, and the one Red Alert actually uses.
//
// MEASURED on Red Alert reaching gameplay (build 2295): it hooks INT 08h
// through INT 21h AH=25h, so "[4GW] INT 08h hooked ... -> PM handler
// 0x0027687d" prints and its timer works, and it NEVER hooks INT 09h that way.
// dos_vec_hooked(0x09) is therefore false, kbd_has_int9 stays 0, and the route
// above cannot fire. The game reached a live mission map and ESC opened
// nothing.
//
// A DPMI client has THREE vector spaces and only one of them is the low table.
// rustkern/dpmi.rs says so and says what it costs: 0205h stores a
// protected-mode handler "FAITHFULLY" and "nothing in this kernel ever DELIVERS
// to them". From the guest's side a faithful store is indistinguishable from a
// working install - it can even read its own handler back with 0204h and get
// the right answer - so the install SUCCEEDS and the interrupt never arrives.
//
// Resolving it is the same shape as any far pointer: the selector through the
// descriptor table the guest's own code uses (dpmi_sel_lookup_rs, the one
// x86_32 is bound to), plus the offset, gives the flat address that
// dos4gw_deliver_at() then bounds-checks exactly as it does for the low-table
// route. There is no second set of rules for a PM-installed handler.
static int dos4gw_pmvec_flat(uint8_t vec, uint32_t *out) {
    extern int dpmi_pmvec_get_rs(uint8_t vec, uint16_t *out_sel, uint32_t *out_off);
    uint16_t sel = 0; uint32_t off = 0, base = 0;
    if (dpmi_pmvec_get_rs(vec, &sel, &off) != 0) return -1;
    // (rakbd2) A SELECTOR THAT IS NOT ONE OF OURS IS FLAT, BASE 0, AND SAYING
    // OTHERWISE MADE THIS WHOLE ROUTE UNREACHABLE.
    //
    // DPMI 0205h takes CX = the handler's code selector, and a DOS/4GW client
    // passes its own CS. Our LE guests run with CS = 0000: the 32-bit core is
    // flat and dpmi_sel_lookup_rs() only knows LDT selectors, which are
    // (index << 3) | 7, so TI is set. Selector 0 has TI clear, idx_of_live()
    // rejects it, the lookup failed and this function reported "no protected
    // mode handler" for a guest that had just installed one.
    //
    // MEASURED on Red Alert with the free-user-vector fix in: the host logged
    // "[dpmi] INT 09h PROTECTED-MODE handler installed by the guest (0205h) ->
    // 0000:00251707" and, in the very same run, "[4GW] kbd ISR route: NONE".
    // Both lines were true of their own table and the pair was nonsense.
    //
    // The rule is not new and is not invented here: rustkern/dpmi.rs's
    // resolve_es_edi() already says "A TI=0 selector is a GDT selector, which
    // under a real DOS/4GW is one of the extender's own flat selectors with
    // base 0, so the flat address is EDI." Same question, same answer, and now
    // in both places rather than one.
    if ((sel & 4) != 0) {
        if (dpmi_sel_lookup_rs(sel, &base, 0, 0) != 0) {
            static uint8_t said[256];
            if (!said[vec]) {
                said[vec] = 1;
                kprintf("[4GW] INT %02Xh has a DPMI 0205h handler at %04x:%08x but "
                        "selector %04x is not a live LDT descriptor: cannot resolve a "
                        "flat address, so nothing will be delivered to it\n",
                        vec, sel, off, sel);
            }
            return -1;
        }
    }
    *out = base + off;
    return 0;
}

static void dos4gw_deliver_int9(dos_task_t *t) {
    // WHICH ROUTE OWNS THE KEYBOARD THIS PASS. The low table first, because a
    // guest that used AH=25h expects that handler; the DPMI 0205h table only
    // when the low table has nothing, so a guest that installed both cannot be
    // delivered to twice for one scancode.
    uint32_t handler = 0;
    int via_pm = 0;
    if (t->kbd_has_int9) {
        handler = dos4gw_pm_vector(t, 0x09);
    } else if (dos4gw_pmvec_flat(0x09, &handler) == 0) {
        via_pm = 1;
    } else {
        // NEITHER ROUTE. Said once, because "the guest has no keyboard handler
        // at all" and "the guest has one and we cannot reach it" look identical
        // from outside and need opposite fixes. Silence used to be the only
        // report of both.
        if (!t->k9_none_said) {
            t->k9_none_said = 1;
            kprintf("[4GW] no keyboard ISR installed by this guest: neither the "
                    "low vector table nor DPMI 0205h has INT 09h. Keys go to the "
                    "BIOS ring for INT 16h and nowhere else.\n");
            bootlog_write("[4GW] kbd ISR route: NONE (no INT 09h in either table)");
        }
        return;
    }

    if (!t->k9_pending) {
        int sc = dos_scancode_get();
        if (sc < 0) return;
        t->k9_code    = (uint8_t)sc;
        t->k9_pending = 1;
    }
    // The byte the guest's `in al,0x60` will read (dos_in(): 0x60 ->
    // kbd_port60). Set BEFORE delivery, because the handler reads it as its
    // first act.
    t->kbd_port60 = t->k9_code;

    // (rakbd) AND IT GOES TO /BOOTLOG.TXT AS WELL AS SERIAL.
    //
    // The owner's laptop HAS NO SERIAL PORT. Every DOS diagnostic in this file
    // is a kprintf, so his durable log from a real boot of the build that
    // showed this bug contains not one [dos], int21 or keyq line: the question
    // "does Red Alert have a keyboard ISR on YOUR machine" was unanswerable
    // from the only evidence his hardware can produce, and it had to be
    // reproduced in a VM instead. That is the third time this shape has cost a
    // round trip; #NETDIAG already fixed it the same way for the network.
    //
    // These are ONE-SHOT lines, one per guest launch, so they are free and are
    // deliberately NOT gated behind /CONFIG/DOSDIAG.CFG: a diagnostic that has
    // to be armed in advance is no use for the boot that already happened.
    if (!t->k9_route_said) {
        t->k9_route_said = 1;
        const char *rname = via_pm ? "DPMI 0205h protected-mode vector"
                                   : "low vector table (INT 21h AH=25h)";
        kprintf("[4GW] keyboard ISR route: %s (handler 0x%08x)\n", rname, handler);
        bootlog_write("[4GW] kbd ISR route: %s handler=0x%08x", rname, handler);
    }
    int dr = dos4gw_deliver_at(t, 0x09, handler);
    if (dr == X32_INJ_DELIVERED) {
        t->k9_pending = 0;
        if (!t->k9_first_said) {
            t->k9_first_said = 1;
            bootlog_write("[4GW] first scancode 0x%02x DELIVERED to the guest "
                          "keyboard ISR", (unsigned)t->k9_code);
        }
    }
}


// ---------------------------------------------------------------------------
// (#sbirq32) THE SOUND BLASTER IRQ FOR A 32-BIT DOS/4GW GUEST.
// ---------------------------------------------------------------------------
//
// WHAT WAS BROKEN. dos_sb_irq_pending_rs() had exactly ONE consumer in this
// file, the 16-bit run loop. dos4gw_run() never asked, so no protected-mode
// guest could ever be told that its sound card had finished a block. This is
// the same shape #779 found one vector along (dos4gw_run() never called
// dos_deliver_int9(), so a 32-bit guest that hooked INT 9 lost the keyboard),
// and it is fixed the same way: ask the question in the loop that was not
// asking it.
//
// IT IS NOT ONLY THE SAME SHAPE, THOUGH, AND THE DIFFERENCE IS THE WHOLE POINT.
// MEASURED on Discworld II (golden 2270, /ssdmirror/dpmi301/run9-wav): the
// Miles Sound System driver is a 16-bit real-mode blob, SBLASTER.DIG, which the
// game loads into DOS memory, publishes as real-mode vector 66h with DPMI
// 0201h, and then CALLS with DPMI 0300h. So the code that waits for the
// interrupt does not run in the 32-bit core at all: it runs inside
// dos4gw_rm_exec_guest(), a nested 16-bit interpreter burst. Its function 0304h
// programmed the 8237 for channel 1, wrote six DSP bytes, wrote 0xDF to port
// 0x21 (IRQ5 UNMASKED, which is how we know which vector it wants), and then
// executed 2,000,000 instructions with ZERO further port I/O before the budget
// cut it off. Zero port I/O means it is spinning on a MEMORY flag, and the only
// thing that sets that flag is its own interrupt handler.
//
// A fix that only touched dos4gw_run() would therefore have changed nothing:
// the run loop is not running while 0304h spins. That is why dos4gw_sb_irq()
// takes a `nested` flag and is called from BOTH places.
//
// THE THREE VECTOR TABLES, AND WHY THE LOW ONE IS AMBIGUOUS.
// A DPMI client has three places an INT 0Dh handler can live, and this guest
// can plausibly use any of them:
//   * the arena's low vector table, written by 16-bit driver code (a real-mode
//     seg:off) or by the 32-bit game through INT 21h AH=25h under DPMI (a FLAT
//     address, packed into the same four bytes by rustkern/dos4gw.rs vec_pack);
//   * the DPMI 0201h real-mode shadow, which dos_vec_hooked() cannot see;
//   * the DPMI 0205h protected-mode table, which #779b made readable.
// The low table's four bytes do NOT say which encoding they are in. They are
// disambiguated the only way they can be without tracking the writer: a flat
// address that is inside this arena and above the vector table itself IS a
// usable protected-mode handler and is treated as one; anything else cannot be,
// so it is read as a real-mode seg:off. dos4gw_deliver_at() already refuses the
// out-of-arena case loudly, so before this rule the only outcome for a
// real-mode-written vector was that refusal.
//
// AND INSIDE A NESTED 16-BIT RUN THERE IS NO AMBIGUITY AT ALL: the code that
// installed the handler was 16-bit real-mode code and the code that must run it
// is the 16-bit interpreter. Injecting into the 32-bit core there would vector
// a guest that is suspended mid-INT-31h. So `nested` disables the two
// protected-mode routes outright rather than merely deprioritising them.
//
// A PENDING IRQ WITH NO HANDLER IS LATCHED, NOT DROPPED. The 16-bit loop drops
// an unacknowledged interrupt after one delivery, and that is right: a handler
// that never reads base+0x0E would otherwise be re-entered forever. It is NOT
// right for an interrupt that was never delivered to anything. Real hardware
// holds the line asserted until the card is acknowledged, and the case that
// matters here is precisely the race this fix exists for: the driver arms the
// transfer, the pump thread finishes the block and raises the IRQ, and the
// driver installs its handler a few hundred instructions later. Dropping the
// interrupt in that window would hang the wait we are trying to end. So the
// drop-and-count happens only after a delivery was actually attempted.
enum dos_sb_route {
    SBROUTE_NONE = 0,
    SBROUTE_RM_LOW,   // arena low vector table, read as a real-mode seg:off
    SBROUTE_RM_201,   // DPMI 0201h real-mode vector shadow
    SBROUTE_PM_LOW,   // arena low vector table, read as a flat PM address
    SBROUTE_PM_205    // DPMI 0205h protected-mode vector
};

static const char *dos_sb_route_name(int r) {
    switch (r) {
    case SBROUTE_RM_LOW: return "real-mode handler in the low vector table";
    case SBROUTE_RM_201: return "real-mode handler published with DPMI 0201h";
    case SBROUTE_PM_LOW: return "protected-mode handler in the low vector table "
                                "(INT 21h AH=25h)";
    case SBROUTE_PM_205: return "protected-mode handler installed with DPMI 0205h";
    default:             return "none";
    }
}

// Resolve the route ONCE per delivery, from the tables rather than from a flag
// latched at install time, for the reason dos4gw_refresh_hooks() states: a guest
// may write a vector table directly and may put it back on the way out.
static int dos4gw_sb_route(dos_task_t *t, int nested,
                           uint16_t *rseg, uint16_t *roff, uint32_t *pmflat) {
    uint16_t off = rd16(t, 0x0000, (uint16_t)(DOS_SB_IRQ_VEC * 4));
    uint16_t seg = rd16(t, 0x0000, (uint16_t)(DOS_SB_IRQ_VEC * 4 + 2));
    int low = dos_vec_hooked(t, DOS_SB_IRQ_VEC);
    uint32_t flat = ((uint32_t)seg << 16) | off;

    if (!nested) {
        // Could these four bytes be a protected-mode handler in THIS arena?
        // The two bounds are dos4gw_deliver_at()'s own refusals, applied here
        // as a test instead of as a rejection, so a real-mode vector falls
        // through to the real-mode route rather than being reported as broken.
        if (low && flat >= 0x400u && flat < t->le_arena_size) {
            *pmflat = flat;
            return SBROUTE_PM_LOW;
        }
        uint32_t h = 0;
        if (dos4gw_pmvec_flat(DOS_SB_IRQ_VEC, &h) == 0 &&
            h >= 0x400u && h < t->le_arena_size) {
            *pmflat = h;
            return SBROUTE_PM_205;
        }
    }
    if (low) { *rseg = seg; *roff = off; return SBROUTE_RM_LOW; }
    {
        uint16_t s2 = 0, o2 = 0;
        if (dpmi_rmvec_guest_rs(DOS_SB_IRQ_VEC, &s2, &o2) && (s2 || o2)) {
            *rseg = s2; *roff = o2;
            return SBROUTE_RM_201;
        }
    }
    return SBROUTE_NONE;
}

// The ISR's instruction budget. The same 20000 the 16-bit run loop passes to
// dos_deliver_int() for this exact vector; a Sound Blaster end-of-block handler
// that needs more than that is not returning, and the budget is a BACKSTOP for
// that case rather than the normal exit (#232).
#define DOS_SB_ISR_BUDGET 20000UL

static void dos4gw_sb_irq(dos_task_t *t, int nested) {
    if (!t || !t->le_active) return;
    if (!dos_sb_irq_pending_rs(&t->sb)) return;

    uint16_t rseg = 0, roff = 0;
    uint32_t pmflat = 0;
    int route = dos4gw_sb_route(t, nested, &rseg, &roff, &pmflat);

    if (route == SBROUTE_NONE) {
        t->sb_irq_latched++;
        if (!t->sb_none_said) {
            t->sb_none_said = 1;
            kprintf("[4GW] Sound Blaster IRQ%u (INT %02Xh) is asserted and this guest "
                    "has NO handler for it in any of its three vector tables (low "
                    "table %04x:%04x, no DPMI 0201h, no DPMI 0205h). The interrupt "
                    "stays LATCHED, as the card's line would, so a handler installed "
                    "later still receives it.\n",
                    (unsigned)DOS_SB_IRQ, (unsigned)DOS_SB_IRQ_VEC,
                    rd16(t, 0x0000, (uint16_t)(DOS_SB_IRQ_VEC * 4 + 2)),
                    rd16(t, 0x0000, (uint16_t)(DOS_SB_IRQ_VEC * 4)));
            bootlog_write("[4GW] SB IRQ route: NONE (no INT %02Xh handler in any table)",
                          (unsigned)DOS_SB_IRQ_VEC);
        }
        return;
    }

    if (!t->sb_route_said) {
        t->sb_route_said = 1;
        if (route == SBROUTE_PM_LOW || route == SBROUTE_PM_205)
            kprintf("[4GW] Sound Blaster IRQ route: %s, flat 0x%08x\n",
                    dos_sb_route_name(route), pmflat);
        else
            kprintf("[4GW] Sound Blaster IRQ route: %s, %04x:%04x\n",
                    dos_sb_route_name(route), rseg, roff);
        bootlog_write("[4GW] SB IRQ route: %s (%04x:%04x / flat 0x%08x)",
                      dos_sb_route_name(route), rseg, roff, pmflat);
    }

    // THE FLAG THE GUEST WILL BE JUDGED BY, CAPTURED BEFORE DELIVERY AND FROM
    // THE RIGHT CORE. A real-mode route is entered from t->cpu and a
    // protected-mode one from t->le_cpu, and reading the wrong one gives a
    // number that is not wrong-looking, just wrong: the 16-bit scratch frame
    // under an LE guest holds whatever the last 0300h left in it.
    int if_before = (route == SBROUTE_PM_LOW || route == SBROUTE_PM_205)
                  ? ((t->le_cpu.eflags & 0x200u) != 0)
                  : ((t->cpu.flags   & 0x200u) != 0);
    int delivered = 0;
    if (route == SBROUTE_PM_LOW || route == SBROUTE_PM_205) {
        int r = dos4gw_deliver_at(t, DOS_SB_IRQ_VEC, pmflat);
        if (r != X32_INJ_DELIVERED) {
            // MASKED means the guest is inside a cli region, which is normal
            // and transient; a refusal has already been reported once by
            // dos4gw_deliver_at(). Either way the interrupt stays latched and
            // is re-offered, exactly as dos4gw_deliver_int9() re-offers a
            // scancode rather than losing it.
            t->sb_irq_latched++;
            return;
        }
        delivered = 1;
    } else {
        x86_16_cpu_t *c = &t->cpu;
        if (nested) {
            // The driver's OWN live real-mode stack is the right one: we are
            // interrupting its wait loop exactly where the card's IRQ line
            // would have.
            dos_deliver_int_at(t, rseg, roff, DOS_SB_ISR_BUDGET);
        } else {
            // At a slice boundary t->cpu is scratch under an LE guest and its
            // SS:SP is whatever the last DPMI 0300h reflection left there.
            // Point it at the same host real-mode stack dpmi_rmcs_call_rs()
            // and dos_mouse_events() use, and restore the frame afterwards,
            // carrying insn_count/halted FORWARD rather than restoring them:
            // the emulated PIT is derived from insn_count and rewinding it
            // moves guest time backwards.
            x86_16_cpu_t save = *c;
            c->ss = (uint16_t)(DOS4GW_XFER_LIN >> 4);
            c->sp = (uint16_t)(DOS4GW_XFER_LEN - 0x100);
            dos_deliver_int_at(t, rseg, roff, DOS_SB_ISR_BUDGET);
            unsigned long insns = c->insn_count;
            int halted = c->halted, exit_code = c->exit_code;
            *c = save;
            c->insn_count = insns;
            c->halted = halted;
            c->exit_code = exit_code;
        }
        delivered = 1;
    }

    if (delivered) {
        t->sb_irq_deliv++;
        if (!t->sb_first_said) {
            t->sb_first_said = 1;
            // IF IS REPORTED, NOT ENFORCED. The 16-bit loop delivers this
            // vector without consulting IF and this path stays symmetrical with
            // it. But dos4gw_rm_exec_guest() enters a 0300h handler with IF
            // CLEAR ("as a real INT does"), and a driver that waits on an
            // interrupt must have re-enabled them, so the flag's value at the
            // first delivery is the cheap evidence for whether that entry
            // convention is right. A number in a log beats a rule nobody has
            // measured.
            kprintf("[4GW] first Sound Blaster INT %02Xh delivered (%s, nested=%d, "
                    "guest IF was %d at delivery)\n",
                    (unsigned)DOS_SB_IRQ_VEC, dos_sb_route_name(route), nested,
                    if_before);
            bootlog_write("[4GW] first SB INT %02Xh DELIVERED route=%s nested=%d",
                          (unsigned)DOS_SB_IRQ_VEC, dos_sb_route_name(route), nested);
        }
        // Now, and only now, the 16-bit loop's rule applies: a handler that did
        // not read base+0x0E has not acknowledged the card, and re-entering it
        // forever would turn a silent interrupt storm into a hang. Drop it once
        // and COUNT it, so "the guest's ISR does not acknowledge" is a number in
        // the exit census rather than an unexplained freeze.
        if (dos_sb_irq_pending_rs(&t->sb)) {
            t->sb.irq_pending = 0;
            t->sb_irq_unacked++;
        }
    }
}

// The BIOS 18.2065 Hz tick at 0040:006C, one per 65536 PIT ticks, which is the
// exact hardware relationship. A DOS/4GW client reads it through its own flat
// DS, so it is the same four bytes of the same arena.
//
// (#sbirq32) SPLIT OUT OF dos4gw_timebase() BECAUSE THE TICK HAS A SECOND
// CALLER THAT MUST NOT HAVE THE REST. dos4gw_rm_exec_guest() runs a guest's own
// real-mode handler for up to a second of guest time, during which the run loop
// - and therefore dos4gw_timebase() - is not running at all, and a driver that
// waits on this dword waits forever. It needs the tick and it must NOT get the
// IRQ0 delivery beside it: for a 32-bit guest the low vector table's INT 08h
// holds a FLAT protected-mode address (rustkern/dos4gw.rs vec_pack), and
// vectoring the 16-bit interpreter at it as a real-mode seg:off would run
// whatever bytes sit at seg*16+off. So the tick moves, and IRQ0 does not fire,
// for the duration of a 0300h. That is a deliberate and stated asymmetry.
static void dos4gw_bios_tick(dos_task_t *t) {
    uint32_t bt = dos_bios_tick_now(t);     // (#234a) time of day, not uptime
    if (bt != t->bios_tick_last) {
        t->bios_tick_last = bt;
        uint8_t b[4] = { (uint8_t)bt, (uint8_t)(bt >> 8),
                         (uint8_t)(bt >> 16), (uint8_t)(bt >> 24) };
        x86_32_write_guest(&t->le_cpu, 0x46C, b, 4);
    }
}

// One pass of the timebase, called at an instruction boundary once per slice.
//
// The catch-up is BOUNDED and the debt is DROPPED, exactly as the 16-bit path
// does it: a host stall must not turn into a thousand queued IRQ0s that then
// run the game forward all at once. A masked tick is dropped for the same
// reason, and is counted so "the guest spends its life with interrupts off"
// would be visible as a number rather than as unexplained slowness.
static void dos4gw_timebase(dos_task_t *t) {
    dos4gw_refresh_hooks(t);

    uint32_t div = t->pit[0].divisor ? t->pit[0].divisor : 65536u;
    uint64_t now_pit = dos_emu_pit_now(t);
    if (t->next_irq0_pit == 0) t->next_irq0_pit = now_pit + div;

    int fired = 0;
    while (now_pit >= t->next_irq0_pit && fired < 4) {
        // The guest owns IRQ0 if it hooked 08h. If it hooked only 1Ch, WE are
        // the BIOS timer handler and the last thing a real one does on every
        // tick is `int 1Ch`; the 16-bit path already carries that reasoning and
        // the reason it belongs inside the IRQ0 pace rather than on the 18.2 Hz
        // tick counter.
        uint8_t vec = t->has_int8 ? 0x08 : (t->has_int1c ? 0x1C : 0x00);
        if (!vec) break;
        if (dos4gw_deliver(t, vec) != X32_INJ_DELIVERED) break;
        t->next_irq0_pit += div;
        fired++;
    }
    if (now_pit >= t->next_irq0_pit)
        t->next_irq0_pit = now_pit + div;      // still behind: resync, drop the debt

    dos4gw_bios_tick(t);
}


// ===========================================================================
// (#740 digplay) THE INT 33h 0Ch/14h UPCALL FOR A NATIVE PROTECTED-MODE CLIENT
//
// WHAT WAS MISSING, MEASURED. The Dig's launcher is a mouse-only panel. It
// calls INT 33h 00h (driver present), 07h/08h (range 0..639 x 0..479), 14h
// (exchange event handler, mask 0x7F) and 1Ch, and it NEVER calls 03h. So the
// only channel by which a click can reach it is the event handler, and this
// host declined to call it because the install came from protected mode. The
// mouse position was maintained perfectly and nothing ever read it: the menu
// could not be operated at all, which reads exactly like "the launcher renders
// but nothing responds".
//
// WHY IT IS A SEPARATE FUNCTION FROM dos_mouse_events(). That one points the
// 16-bit interpreter at a real-mode (seg, off) and runs x86_16_run(). Here the
// handler is 32-bit protected-mode code at a flat linear address, reached with
// a far CALL and returning with RETF. Different core, different frame,
// different return test. What the two DO share is the shape of the answer, and
// deliberately so: save the interrupted context, build the frame a real driver
// would build, run BOUNDED, test for the return address rather than guessing,
// and restore.
//
// THE ARGUMENTS ARE ZERO-EXTENDED FROM 16 BITS, and that is not cosmetic. The
// Dig's handler reads its previous mickey value with `mov <mem>,%dx`, a 16-bit
// load that leaves the top half of EDX alone, and then does a 32-BIT subtract
// against ESI. A non-zero high half in EDX would poison the delta by 65536 per
// count. The same argument applies to ESI/EDI, whose previous values the guest
// stores as 16-bit words. This is also what a real DOS/4GW does, so the
// faithful thing and the working thing agree.
//
// SI/DI ARE A CUMULATIVE COUNTER, not this event's delta. The Dig subtracts the
// value it saw LAST time and adds the difference to its own cursor, so a
// per-event delta would make its cursor jump by the difference between two
// deltas.
//
// (#mickey) WHAT CHANGED, AND WHY IT IS NO LONGER THE 0Bh COUNTER. This used to
// pass t->mick_x, the same accumulator function 0Bh reads and CLEARS, on the
// ground that the documented driver keeps one pair. Three defects came out of
// that against an absolute host pointer: the accumulator was scaled by the
// guest's mickeys-per-pixel ratio, so vertical motion arrived doubled for a
// guest that integrates rather than divides; a 0Bh call anywhere in the guest
// cleared the number its own handler differences against; and a free-running
// accumulator handed over 16 bits wide wrapped after ~32k pixels of travel,
// which this handler reads zero-extended and subtracts in 32, turning the wrap
// into a -65535 jump. t->mick_si is now an exact affine function of the
// absolute position, so differencing it yields the host motion, it cannot wrap
// inside the virtual range, and nothing else can clear it. See
// rustkern/dosmick.rs.
// ===========================================================================
#define DOS4GW_MEV_CHUNKS  16u        // bounded passes; a handler that will not
#define DOS4GW_MEV_SLICE   20000u     // return costs 320k instructions, not the run

static void dos4gw_mouse_events(dos_task_t *t) {
    if (!t->mev_pm || !t->mev_pm_lin) { t->mev_pending = 0; return; }
    uint16_t ev = (uint16_t)(t->mev_pending & t->mev_mask);
    if (!ev) { t->mev_pending = 0; return; }

    x86_32_cpu_t *c = &t->le_cpu;
    const uint32_t ret = ((uint32_t)0xF000u << 4) + DOS_MEVRET_STUB;

    // The WHOLE register file, including stop_eip, is put back afterwards. A
    // real driver far-calls this from an ISR that has already saved the
    // interrupted context, so the handler cannot be allowed to perturb it.
    t->mev_save = *c;
    c->stop_eip = ret;
    c->stop_eip_en = 1;

    int inj = x86_32_inject_farcall(c, t->mev_pm_lin, ret);
    if (inj != X32_INJ_DELIVERED) {
        *c = t->mev_save;
        if (inj == X32_INJ_MASKED) {
            // NOT an error and NOT a dropped event: the guest is inside a cli
            // region, a real IRQ would have been held too, and mev_pending is
            // deliberately left set so the next slice offers it again.
            if (t->mev_pm_masked++ == 0)
                kprintf("[4GW] INT 33h mouse upcall held: the guest has "
                        "interrupts masked. It is re-offered every slice.\n");
        } else {
            kprintf("[4GW] INT 33h mouse upcall NOT delivered: the far-call frame "
                    "does not fit at ESP 0x%08x. The guest is left running.\n",
                    c->regs[X32_ESP]);
        }
        return;
    }

    c->regs[X32_EAX] = (uint32_t)ev;                        // condition mask
    c->regs[X32_EBX] = (uint32_t)(uint16_t)t->mbtn;          // button state
    c->regs[X32_ECX] = (uint32_t)(uint16_t)t->mx;            // cursor column
    c->regs[X32_EDX] = (uint32_t)(uint16_t)t->my;            // cursor row
    c->regs[X32_ESI] = (uint32_t)(uint16_t)t->mick_si;
    c->regs[X32_EDI] = (uint32_t)(uint16_t)t->mick_di;
    t->mev_pending = 0;

    int returned = 0, derailed = 0;
    uint32_t last = X32_EXIT_BUDGET;
    for (unsigned k = 0; k < DOS4GW_MEV_CHUNKS && !returned && !derailed; k++) {
        uint32_t r = x86_32_run(c, DOS4GW_MEV_SLICE);
        last = r;
        if (r == X32_EXIT_STOP_EIP) { returned = 1; break; }
        if (r == X32_EXIT_INT) {
            // The handler may make service calls; it is ordinary guest code.
            // A blocking console read inside a mouse callback is not something
            // this can wait for, so it ends the upcall rather than the run.
            dos4gw_service_int(t, c->exit_arg);
            if (t->svc.input_blocked) { t->svc.input_blocked = 0; derailed = 1; }
            if (t->cpu.halted) derailed = 1;
        } else if (r == X32_EXIT_IO_IN) {
            uint16_t v = dos_in(&t->cpu, (uint16_t)c->exit_arg, (int)c->io_size);
            uint32_t m = (c->io_size == 1) ? 0xFFu
                       : (c->io_size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
            c->regs[X32_EAX] = (c->regs[X32_EAX] & ~m) | (v & m);
        } else if (r == X32_EXIT_IO_OUT) {
            dos_out(&t->cpu, (uint16_t)c->exit_arg, (uint16_t)c->io_val,
                    (int)c->io_size);
        } else if (r == X32_EXIT_BUDGET) {
            /* keep going: the chunk ran out, not the handler */
        } else {
            derailed = 1;                    // MISS / UD / fault inside the handler
        }
    }

    uint32_t eeip = c->eip;
    uint64_t insns = c->insn_count;
    *c = t->mev_save;
    // Guest time only ever moves FORWARD. The emulated PIT is derived from
    // insn_count (dos_emu_pit_now), so rewinding it would move the guest's
    // clock backwards, which is the one thing a delay loop cannot survive.
    c->insn_count = insns;

    // (#mickey) WHERE DOES THE GUEST THINK IT IS POINTING? err= is the whole
    // report and it must read (0,0): that is the difference between "the guest
    // pointer moved" and "the guest pointer is under the user's finger".
    // Throttled to actual change and hard-bounded, because a mouse-rate log is
    // how this file got a 46,390-line flood once before.
    if (t->mtrack_addr && t->mtrack_n < 600) {
        uint8_t pb[4];
        if (x86_32_read_guest(c, t->mtrack_addr, pb, 4) == 0) {
            int gpx = (int)(int16_t)(uint16_t)(pb[0] | (pb[1] << 8));
            int gpy = (int)(int16_t)(uint16_t)(pb[2] | (pb[3] << 8));
            if (gpx != t->mtrack_lx || gpy != t->mtrack_ly) {
                t->mtrack_lx = gpx; t->mtrack_ly = gpy;
                t->mtrack_n++;
                kprintf("[dos] mtrack host=(%d,%d) drv=(%d,%d) si/di=(%d,%d) "
                        "guest=(%d,%d) err=(%d,%d) homes=%u ph=%d\n",
                        (int)mouse_x, (int)mouse_y, t->mx, t->my,
                        t->mick_si, t->mick_di, gpx, gpy,
                        gpx - t->mx, gpy - t->my,
                        t->mick.home_n, t->mick.home_ph);
            }
        }
    }

    {
        extern volatile int g_x86_dbgring;
        if (g_x86_dbgring && t->mev_pm_calls < 6) {
            uint8_t w[48];
            uint32_t probe = t->mev_pm_dbg ? t->mev_pm_dbg : 0;
            if (probe && x86_32_read_guest(c, probe, w, 48) == 0) {
                kprintf("[4GW] mev probe 0x%08x after: "
                        "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                        "%02x%02x%02x%02x | %02x%02x%02x%02x %02x%02x%02x%02x "
                        "%02x%02x%02x%02x %02x%02x%02x%02x | %02x%02x%02x%02x "
                        "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                        probe,
                        w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],
                        w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15],
                        w[16],w[17],w[18],w[19],w[20],w[21],w[22],w[23],
                        w[24],w[25],w[26],w[27],w[28],w[29],w[30],w[31],
                        w[32],w[33],w[34],w[35],w[36],w[37],w[38],w[39],
                        w[40],w[41],w[42],w[43],w[44],w[45],w[46],w[47]);
            }
            uint8_t h[16];
            if (x86_32_read_guest(c, t->mev_pm_lin, h, 16) == 0)
                kprintf("[4GW] mev handler bytes at 0x%08x: %02x %02x %02x %02x "
                        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                        "%02x\n", t->mev_pm_lin,
                        h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],
                        h[8],h[9],h[10],h[11],h[12],h[13],h[14],h[15]);
        }
    }
    if (returned) {
        t->mev_pm_calls++;
        if (t->mev_pm_calls <= 8)
            kprintf("[4GW] INT 33h mouse upcall #%u ev=%04x btn=%d at (%d,%d) "
                    "mickeys (%d,%d) -> 0x%08x returned\n",
                    t->mev_pm_calls, ev, t->mbtn, t->mx, t->my,
                    t->mick_si, t->mick_di, t->mev_pm_lin);
    } else {
        // LOUD, and it needs to be. The Dig's handler sets a reentrancy flag on
        // entry and clears it on the way out, so one abandoned upcall makes the
        // guest ignore EVERY later event. That is a silent dead mouse ten
        // minutes later unless it is said here.
        if (t->mev_pm_noret++ < 4)
            kprintf("[4GW] INT 33h mouse upcall DID NOT RETURN (%s) at EIP "
                    "0x%08x after %u chunks; handler 0x%08x. A guest that sets a "
                    "reentrancy flag on entry will now ignore every later "
                    "event.\n",
                    dos4gw_exit_name(last), eeip, DOS4GW_MEV_CHUNKS,
                    t->mev_pm_lin);
    }
}

// (#mickey) DELIVER THE INT 33h 0Ch UPCALL, HOMING THE COUNTERS FIRST.
//
// Unit-gain counters make a mickey-integrating guest move by the RIGHT AMOUNT.
// They do not put its pointer in the right PLACE: The Dig starts its own at
// (320,260) whatever the host cursor is doing, so a pure delta stream leaves a
// permanent offset and pointing at one menu entry highlights another. The
// pointer is private and invisible, so it cannot be read back and corrected -
// but it CAN be homed against the clamp the guest applies on every event
// (0x0013F360: negative clamps to 0, above width-1 clamps to width-1), and any
// guest that integrates a relative mouse must clamp or its pointer would leave
// the screen on real hardware and never come back.
//
// THE THREE PHASES GO OUT INSIDE THIS ONE SLICE, back to back, so the guest's
// main loop never runs between them and the intermediate corner position is
// never drawn. See rustkern/dosmick.rs for what each phase reports and why.
//
// `pm` selects the route the caller has already established: 0 = the 16-bit
// interpreter (a real-mode handler, or one reflected through DPMI 0300h),
// 1 = a native protected-mode far call into the 32-bit core.
static void dos_mouse_deliver(dos_task_t *t, int pm) {
    // Two conditions, and both are about not counting a home that nobody
    // received. A guest whose event mask excludes MOVE cannot be homed because
    // the phases ARE move events (and such a guest does not take motion from
    // the callback in the first place); and the 16-bit call site is not gated
    // on a handler existing, so without the armed test the phases would be
    // "delivered" to nothing, complete, and increment the homing counter that
    // the instrument reports.
    int armed = pm ? (t->mev_pm != 0) : (t->mev_seg != 0 || t->mev_off != 0);
    if (armed && (t->mev_mask & M_EV_MOVE)) {
        for (int ph = 0; ph < 3; ph++) {
            int32_t si = 0, di = 0;
            if (!dos_mick_next_rs(&t->mick, t->mx, t->my,
                                  t->mmin_x, t->mmax_x, t->mmin_y, t->mmax_y,
                                  &si, &di))
                break;                       // nothing armed: the ordinary path
            t->mick_si = si; t->mick_di = di;
            t->mev_pending |= M_EV_MOVE;
            if (pm) dos4gw_mouse_events(t); else dos_mouse_events(t);
            // A 32-bit guest inside a cli region leaves mev_pending SET and the
            // upcall undelivered. Do NOT advance the phase on an event the
            // guest never saw; it is re-offered on the next slice.
            if (t->mev_pending) break;
            dos_mick_phase_done_rs(&t->mick, t->mx, t->my,
                                   t->mmin_x, t->mmax_x, t->mmin_y, t->mmax_y);
        }
    }
    int32_t si = 0, di = 0;
    dos_mick_next_rs(&t->mick, t->mx, t->my, t->mmin_x, t->mmax_x,
                     t->mmin_y, t->mmax_y, &si, &di);
    t->mick_si = si; t->mick_di = di;
    int had_move = (t->mev_pending & t->mev_mask & M_EV_MOVE) != 0;
    if (pm) dos4gw_mouse_events(t); else dos_mouse_events(t);
    // Periodic re-home, counted in DELIVERED move events rather than in time:
    // a guest that re-centres its own pointer (a new screen, a mode change)
    // would otherwise stay offset until it was relaunched, and a wall-clock
    // deadline in this tree is not a wall clock (KVM replays lost ticks in
    // bursts).
    if (had_move && !t->mev_pending) dos_mick_tick_rs(&t->mick);
}

// The run loop.
//
// Modelled on the 16-bit loop below it, deliberately: same present cadence
// (DOS_PRESENT_MS, followed by win16_host_invalidate() or the compositor never
// blits), same proc_yield() HANDOFF rather than a sleep, same wall-clock
// runaway cap. It is not a wake-wait and has nothing to wait for: every pass
// retires at least one instruction slice of real forward progress.
//
// `max_insns` bounds a diagnostic run (the first contact with a real binary,
// where the useful artifact is a MISS histogram over the first N instructions
// rather than a game). 0 means "until it exits".
// (#740 dw2) THE LOW-WRITE WATCH CALLBACK. Reports, once per distinct
// (linear, EIP) pair and at most DW2_LOW_MAX times, any guest store into the
// interrupt vector table. Bounded twice on purpose: an unbounded per-store
// kprintf inside the interpreter's hot path would change the timing it is
// being used to diagnose, and a table this small fills up fast under a memset.
#define DW2_LOW_MAX 48
static uint32_t g_low_seen_la[DW2_LOW_MAX];
static uint32_t g_low_seen_eip[DW2_LOW_MAX];
static int      g_low_n;
static uint32_t g_low_dropped;

static void dos4gw_low_write(x86_32_cpu_t *cpu, uint32_t lin, uint32_t val,
                             int width, uint32_t eip) {
    (void)cpu;
    for (int i = 0; i < g_low_n; i++)
        if (g_low_seen_la[i] == lin && g_low_seen_eip[i] == eip) return;
    if (g_low_n >= DW2_LOW_MAX) { g_low_dropped++; return; }
    g_low_seen_la[g_low_n] = lin;
    g_low_seen_eip[g_low_n] = eip;
    g_low_n++;
    kprintf("[4GWLOW] guest store into the IVT: linear 0x%03x = 0x%08x (w=%d) "
            "from EIP 0x%08x  [vector %02Xh %s]\n",
            lin, val, width, eip, (unsigned)(lin >> 2),
            (lin & 3) ? "MISALIGNED" : ((lin & 2) ? "segment half" : "offset half"));
}

// The evidence a derail needs and the STOP block did not print: what the three
// vectors we deliver actually hold, and the top of the guest stack, which for
// a guest that has jumped into the vector table is the return address of
// whatever jumped there.
static void dos4gw_dump_derail(dos_task_t *t) {
    static const uint8_t vecs[4] = { 0x08, 0x09, 0x1C, 0x0B };
    for (int i = 0; i < 4; i++)
        kprintf("[4GW]   vector %02Xh -> 0x%08x\n", vecs[i],
                dos4gw_pm_vector(t, vecs[i]));
    // (#211) SS AND CS MAY HAVE A BASE. A DOS/4GW guest is flat, so these two
    // additions are zero for it and nothing changes; a go32 guest's ESP and
    // EIP are OFFSETS, and dumping them as flat addresses printed sixteen
    // bytes of the guest's first megabyte and called it the stack.
    uint32_t sp = t->le_cpu.regs[X32_ESP] + t->le_cpu.seg_base[X32_SS];
    for (int row = 0; row < 4; row++) {
        uint32_t w[4] = { 0, 0, 0, 0 };
        uint32_t a = sp + (uint32_t)row * 16u;
        int ok = 1;
        for (int k = 0; k < 4; k++)
            if (x86_32_read_guest(&t->le_cpu, a + (uint32_t)k * 4u,
                                  (uint8_t *)&w[k], 4) != 0) ok = 0;
        if (!ok) break;
        kprintf("[4GW]   stack 0x%08x: %08x %08x %08x %08x\n",
                a, w[0], w[1], w[2], w[3]);
    }
    uint8_t code[16];
    uint32_t cbase = (t->le_cpu.eip + t->le_cpu.seg_base[X32_CS]) & ~0xFu;
    if (x86_32_read_guest(&t->le_cpu, cbase, code, 16) == 0) {
        kprintf("[4GW]   bytes at 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n", cbase,
                code[0], code[1], code[2], code[3], code[4], code[5], code[6],
                code[7], code[8], code[9], code[10], code[11], code[12],
                code[13], code[14], code[15]);
    }
    if (g_low_dropped)
        kprintf("[4GW]   (%u further IVT stores were not reported: the watch "
                "table holds %d distinct sites)\n", g_low_dropped, DW2_LOW_MAX);
    // (#rafault) THE EDGE, not just the wreckage: what branched where, and the
    // last service calls with the EIP that made them. Both are no-ops unless
    // /CONFIG/DOSDIAG.CFG armed them.
    x86_32_btrace_dump(96);
    if (x86_32_btrace_on()) go32_trace_dump(t);
}

// (#speedcap, extends #232/#778) THE CAP, NOW ON THIS LOOP TOO.
//
// #232 capped only the 16-bit interpreter and #778 gave only that loop a live
// re-poll; dos4gw_run() was explicitly excluded, on the stated ground that "no
// shipped DOS/4GW title has been shown to need it, and every one of them is
// 386-era software that paces itself off the timer". Both halves of that turned
// out to be the wrong test. Red Alert and NetHack are the two heaviest guests in
// the catalog and are precisely the ones that starve the compositor, and "paces
// itself off the timer" is a statement about the GUEST's animation, not about
// how much of the host it consumes: an uncapped interpreter burns every cycle it
// is given whatever the guest does with them.
//
// `cap` is the value dos_run_file() already resolved and already printed, passed
// in rather than re-resolved, so the launch line and the enforced cap cannot
// disagree. `path` is kept only for the #778 live re-poll, which must run the
// SAME dos_speed_cycles_for() chain the launch did or the compositor's Speed
// dialog would move 16-bit guests and silently not 32-bit ones.
static void dos4gw_run(dos_task_t *t, uint64_t max_insns, const char *path,
                       uint32_t cap) {
    uint64_t run_t0 = sched_now_ms();
    uint64_t last_present_ms = 0;
    uint64_t last_sample_ms  = 0;   // #dosplay redraw sampler, independent of the present
    uint64_t last_alive_ms = 0;      // (#740 digplay) alive sampler
    unsigned alive_n = 0;
    // Seeded from the same constant the 16-bit loop seeds from, and re-sized
    // from the MEASURED rate below. It is not decoration: this slice length is
    // the resolution of the emulated PIT, because an interrupt can only be
    // delivered at a slice boundary. A slice of 200000 instructions at a
    // measured ~2 M insn/s is 100 ms, which cannot express a 35 Hz timer at
    // all; DOS_SLICE_MS of wall clock can.
    unsigned long slice = (unsigned long)(DOS_EMU_INSN_HZ * DOS_SLICE_MS / 1000UL);
    uint64_t rate_t0 = run_t0;
    uint64_t rate_i0 = 0;
    uint64_t rate_b0 = 0;            // (#176) bus ticks at the window start
    int bus_sat_prev = 0;            // (#176) did the PREVIOUS window saturate
    uint32_t frames = 0;
    const char *why = "guest exited";
    // (#221) A blocking console read that found no key: the vector to re-offer
    // on the next pass, or -1. See the note at the re-issue site below.
    int      retry_vec   = -1;
    uint32_t input_waits = 0;
    // (#speedcap) The SAME entitlement account the 16-bit loop keeps, over the
    // SAME constants, against t->le_cpu.insn_count instead of t->cpu.insn_count.
    // Deliberately the same shape and not a second scheme: the reason the 16-bit
    // cap delivers its target rate rather than merely bounding it (a 250 Hz host
    // tick means proc_sleep(1) really sleeps ~4 ms, so a fixed burst plus a fixed
    // sleep under-delivers by 4x) applies identically here.
    uint32_t thr_cycles     = cap;
    const char *thr_src     = "launch";
    int64_t  thr_credit     = 0;
    uint64_t thr_last_ms    = run_t0;
    unsigned long thr_last_insns = 0;
    uint64_t thr_live_poll_ms = run_t0;

    // Point the guest's vector table at a protected-mode no-op BEFORE the first
    // instruction, so a vector it saves and later chains to is an address it can
    // execute. Done here rather than in the loader because the per-launch setup
    // that writes the real-mode stubs runs after the arena swap, and seeding
    // before it would simply be overwritten.
    dos4gw_seed_pm_ivt(t);
    // (#740 digsel) THE FIRST BYTES OF THE GUEST'S FIRST PAGE, printed once,
    // AFTER the vectors are seeded, because that is the state the guest's first
    // instruction sees. A Watcom guest reads its command-tail length at
    // PSP:0x80; through a selector this host never created that read is guest
    // linear 0x80, which is interrupt vector 20h. Printing both makes "it
    // parsed the vector table as argv" a comparison rather than an inference.
    kprintf("[4GW] guest linear 0x80..0x8F (interrupt vectors 20h-23h): "
            "%02x %02x %02x %02x  %02x %02x %02x %02x  "
            "%02x %02x %02x %02x  %02x %02x %02x %02x\n",
            t->mem[0x80], t->mem[0x81], t->mem[0x82], t->mem[0x83],
            t->mem[0x84], t->mem[0x85], t->mem[0x86], t->mem[0x87],
            t->mem[0x88], t->mem[0x89], t->mem[0x8A], t->mem[0x8B],
            t->mem[0x8C], t->mem[0x8D], t->mem[0x8E], t->mem[0x8F]);
    kprintf("[4GW] PSP:0x80 (guest linear 0x%08x): length %02x, then "
            "%02x %02x %02x %02x %02x %02x %02x\n",
            ((uint32_t)DOS_PSP_SEG << 4) + 0x80u,
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x80],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x81],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x82],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x83],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x84],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x85],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x86],
            t->mem[((uint32_t)DOS_PSP_SEG << 4) + 0x87]);
    {   // Armed only under /CONFIG/DOSDIAG.CFG, which sets g_x86_dbgring and is
        // not shipped in the golden. The seed above uses x86_32_write_guest(),
        // which is the HOST's accessor and deliberately does not go through the
        // watch, so the table starts quiet and every line that follows is a
        // store the GUEST's own instruction stream made.
        extern volatile int g_x86_dbgring;
        if (g_x86_dbgring) {
            g_low_n = 0; g_low_dropped = 0;
            x86_32_set_low_watch(&t->le_cpu, 0x400, dos4gw_low_write);
            kprintf("[4GW] IVT write watch ARMED over guest linear 0x000..0x3FF\n");
            // (#rafault) A DOS/4GW module's lowest legitimate code is the
            // seeded PM stub at 0x000FFF53 and the DOS memory block at
            // 0x00020000. Nothing this guest runs lives in the first 4 KiB, so
            // a transfer there is a derail by definition and the latch cannot
            // fire on ordinary control flow.
            x86_32_btrace(1, 0x1000);
            kprintf("[4GW] branch trace ARMED (ring 256, derail latch below "
                    "guest linear 0x1000)\n");
        }
    }

    // A fresh run measures its own rate. Left over from a previous guest, this
    // is a wrong time base for the first 200 ms of every run after the first.
    g_dos_emu_hz = 0;
    t->emu_pit_base = 0;
    t->emu_insn_base = 0;
    t->next_irq0_pit = 0;

    kprintf("[4GW] running: entry 0x%08x, budget %s\n", t->le_cpu.eip,
            max_insns ? "bounded" : "unbounded");

    for (;;) {
        if (!t->running) { why = "window closed"; break; }
        if (max_insns && t->le_cpu.insn_count >= max_insns) {
            why = "instruction budget reached";
            break;
        }

        uint32_t r;
        if (retry_vec >= 0) {
            // (#221) RE-ISSUE, do not re-execute. The guest's last INT was a
            // blocking console read with an empty keyboard queue; the service
            // wrote nothing and the 32-bit register file is untouched, so
            // calling the service again with the same registers is exactly
            // equivalent to the guest executing the same INT again - without
            // needing to rewind EIP, which would have to know the instruction's
            // length. One pass of this loop has run in between, which is the
            // point: it pumped input, presented a frame and yielded.
            r = X32_EXIT_INT;
            t->le_cpu.exit_arg = (uint32_t)retry_vec;
            retry_vec = -1;
        } else {
            if (thr_cycles) {
                // Entitlement accounting, identical to the 16-bit loop's (see
                // the long note there for why the debt is REMEMBERED rather than
                // forgiven once a second). One sleep and one re-read per pass,
                // never a loop: every pass still retires at least
                // DOS_THROTTLE_BURST_MIN instructions of real forward progress,
                // which is what keeps this out of the busy-wait class CLAUDE.md
                // bans (#426).
                uint64_t tnow = sched_now_ms();
                thr_credit += (int64_t)((tnow - thr_last_ms) * (uint64_t)thr_cycles);
                thr_last_ms = tnow;
                thr_credit -= (int64_t)((unsigned long)t->le_cpu.insn_count - thr_last_insns);
                thr_last_insns = (unsigned long)t->le_cpu.insn_count;
                int64_t maxc = (int64_t)thr_cycles * (int64_t)DOS_THROTTLE_BURST_MS;
                int64_t maxd = -(int64_t)thr_cycles * (int64_t)DOS_THROTTLE_DEBT_MS;
                if (thr_credit > maxc) thr_credit = maxc;
                if (thr_credit < maxd) thr_credit = maxd;
                if (thr_credit <= 0) {
                    uint64_t ahead = (uint64_t)(-thr_credit) / thr_cycles + 1u;
                    if (ahead > DOS_THROTTLE_SLEEP_MAX) ahead = DOS_THROTTLE_SLEEP_MAX;
                    proc_sleep((uint32_t)ahead);
                    tnow = sched_now_ms();
                    thr_credit += (int64_t)((tnow - thr_last_ms) * (uint64_t)thr_cycles);
                    thr_last_ms = tnow;
                    if (thr_credit > maxc) thr_credit = maxc;
                }
                int64_t b = thr_credit > 0 ? thr_credit : 0;
                if (b > maxc) b = maxc;
                if (b < (int64_t)DOS_THROTTLE_BURST_MIN) b = (int64_t)DOS_THROTTLE_BURST_MIN;
                slice = (unsigned long)b;
            }
            uint64_t _pt = dosprof_t0();
            r = x86_32_run(&t->le_cpu, slice);
            dosprof_t1(DOSPROF_INTERP, _pt);
        }

        if (r == X32_EXIT_INT) {
            uint32_t vec = t->le_cpu.exit_arg;
            dos4gw_service_int(t, vec);
            if (t->svc.input_blocked) {
                t->svc.input_blocked = 0;
                retry_vec = (int)vec;
                if (input_waits++ == 0)
                    kprintf("[4GW] #221 guest is WAITING on a blocking key read "
                            "(INT %02Xh); re-issuing until a key arrives\n",
                            (unsigned)vec);
            }
            // dos_svc_int21()'s AH=4Ch path calls ctx->on_terminate, which sets
            // halted on the frame. That is how the guest says it is done, and
            // it is the same signal the 16-bit task uses.
            if (t->cpu.halted) { why = "guest exited (INT 21h AH=4Ch)"; break; }
        } else if (r == X32_EXIT_IO_IN) {
            uint16_t v = dos_in(&t->cpu, (uint16_t)t->le_cpu.exit_arg,
                                (int)t->le_cpu.io_size);
            uint32_t m = (t->le_cpu.io_size == 1) ? 0xFFu
                       : (t->le_cpu.io_size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
            t->le_cpu.regs[X32_EAX] = (t->le_cpu.regs[X32_EAX] & ~m) | (v & m);
        } else if (r == X32_EXIT_IO_OUT) {
            dos_out(&t->cpu, (uint16_t)t->le_cpu.exit_arg,
                    (uint16_t)t->le_cpu.io_val, (int)t->le_cpu.io_size);
        } else if (r == X32_EXIT_BUDGET) {
            /* nothing wrong: the slice ran out */
        } else {
            // MISS / UD / a fault. STOP, and say exactly where.
            //
            // Skipping a MISSed instruction is not on the table: several of the
            // unimplemented forms carry a push or a pop, and skipping that
            // desynchronises the guest's stack so the eventual crash is nowhere
            // near its cause (blame.md, 2026-08-07). A stop at the first
            // unimplemented opcode is a LOCATED failure, and the located
            // failure is the whole deliverable of a first run.
            kprintf("[4GW] STOP: exit=%s at EIP 0x%08x after %u instructions\n",
                    dos4gw_exit_name(r), t->le_cpu.eip,
                    (uint32_t)t->le_cpu.insn_count);
            if (r == X32_EXIT_MISS || r == X32_EXIT_FAULT_UD) {
                kprintf("[4GW]   opcode %02x op2=%03x modrm=%03x len=%u  "
                        "(EIP is ON the instruction; skipping by len would land "
                        "correctly but would invent an effect we do not know)\n",
                        t->le_cpu.miss_op, t->le_cpu.miss_op2,
                        t->le_cpu.miss_modrm, t->le_cpu.miss_len);
            } else if (r == X32_EXIT_FAULT_MEM) {
                kprintf("[4GW]   address 0x%08x is outside the arena "
                        "(window 0x00000000..0x%08x)\n",
                        t->le_cpu.fault_addr, t->le_arena_size);
            }
            kprintf("[4GW]   EAX=%08x ECX=%08x EDX=%08x EBX=%08x\n"
                    "[4GW]   ESP=%08x EBP=%08x ESI=%08x EDI=%08x EFLAGS=%08x\n",
                    t->le_cpu.regs[0], t->le_cpu.regs[1], t->le_cpu.regs[2],
                    t->le_cpu.regs[3], t->le_cpu.regs[4], t->le_cpu.regs[5],
                    t->le_cpu.regs[6], t->le_cpu.regs[7], t->le_cpu.eflags);
            dos4gw_dump_derail(t);
            why = dos4gw_exit_name(r);
            break;
        }

        // (#211) INPUT. The SAME four calls the 16-bit loop makes, in the same
        // order, under the same focus gate - not a 32-bit copy of them.
        //
        // This loop had none of it. A 32-bit guest could draw, compute and make
        // service calls, and every key pressed at it went into the raw scancode
        // ring and stayed there, because dos_keyq_pump() is what moves a
        // scancode into the BIOS ring that INT 16h and a direct BDA read both
        // look at. NetHack sat on "Hit <Enter> to continue." through two
        // injected Enters that the kernel had definitely received.
        //
        // The focus gate is #156's and matters for the same reason: keystrokes
        // aimed at another window must not burst into the guest when focus
        // returns.
        {
            int now_focused = win16_host_is_focused(t->host_slot);
            // (rakbd) THE FIRST PASS IS STATED, NOT INFERRED FROM AN ABSENT EDGE.
            //
            // This block reports only EDGES, which is right for a steady stream
            // and wrong for the one question that mattered: "was the tap ever
            // armed at all?" With last_focused starting at 0 (the task is
            // memset), a guest that is never focused produces NO line, and a
            // guest that is focused from the start produces one - so silence
            // meant "never armed" and it took a 16-bit control run to establish
            // that, because the 16-bit path seeds last_focused=1 and its silence
            // means the OPPOSITE. Two paths whose identical silence means
            // opposite things is not a diagnostic. Say it once, explicitly.
            if (!t->focus_said) {
                t->focus_said = 1;
                kprintf("[4GW] #156 first pass: host_slot=%d win16_host_is_focused=%d "
                        "(input tap %s)\n", t->host_slot, now_focused,
                        now_focused ? "ARMED" : "NOT ARMED - the guest will receive "
                                                "no keystrokes at all");
                bootlog_write("[4GW] #156 first pass slot=%d focused=%d",
                              t->host_slot, now_focused);
            }
            if (now_focused != t->last_focused) {
                dos_scancode_clear();
                dos_keyq_reset(t);
                t->mbtn = 0;
                t->last_focused = now_focused;
                kprintf("[4GW] #156 host_slot=%d focus -> %s (input tap %s)\n",
                        t->host_slot, now_focused ? "GAINED" : "LOST",
                        now_focused ? "ARMED" : "DISARMED");
            }
            g_dos_scancode_tap = now_focused;
            if (now_focused) {
                // (rakbd2) EITHER route means the guest owns the raw stream.
                if (!t->kbd_has_int9 && !t->kbd_int9_pm) dos_keyq_pump(t);
                { uint64_t _pt = dosprof_t0(); dos_pump_input(t); dosprof_t1(DOSPROF_INPUT, _pt); }
                // (rakbd) THE FOURTH CALL, the one the comment above said this
                // loop already made and did not. Drains the same raw scancode
                // ring dos_keyq_pump() would have, for exactly the guests where
                // that pump is switched off.
                dos4gw_deliver_int9(t);
                // (raplay) THE 0Ch UPCALL, AND WHY IT IS NOW CONDITIONAL RATHER
                // THAN ABSENT.
                //
                // This used to be a comment declining dos_mouse_events()
                // outright, on the ground that "a 32-bit client's handler is a
                // protected-mode flat address, so delivering to it would run
                // whatever bytes sit at seg*16+off". That is true of a handler
                // installed by a native protected-mode INT 33h, and it is FALSE
                // of one installed through DPMI 0300h, where the client filled
                // an RMCS with real-mode register values on purpose. The two
                // are now distinguished at the install (dos_mev_route_is_
                // realmode), so the safe case is delivered and the unsafe one
                // is still declined.
                //
                // It is not an optimisation. MEASURED on Red Alert: every one
                // of its INT 33h calls is a 0300h reflection, and function 03h
                // (read position and buttons, the poll) is not among them. With
                // upcalls off, dos_pump_input() maintains a mouse state nothing
                // ever reads: the cursor cannot move and no click can land.
                if (t->mev_rm && g_dos_mev_upcall) {
                    dos_mouse_deliver(t, 0);
                } else if (t->mev_rm) {
                    static int mev_off_once;
                    if (!mev_off_once) {
                        mev_off_once = 1;
                        kprintf("[4GW] 0Ch upcalls DISABLED by /CONFIG/DOSMEV.CFG "
                                "(handler %04x:%04x would have been called)\n",
                                t->mev_seg, t->mev_off);
                    }
                } else if (t->mev_pm && g_dos_mev_upcall) {
                    // (digplay) THE THIRD ROUTE, and the one The Dig uses. A
                    // native protected-mode INT 33h 0Ch/14h: the handler is
                    // 32-bit code at a resolved flat address, far-CALLED and
                    // returning with RETF, so it goes to the 32-bit core rather
                    // than the 16-bit interpreter. Under the SAME
                    // /CONFIG/DOSMEV.CFG kill switch as the real-mode route,
                    // because a switch that turns off half of a mechanism is
                    // worse than none.
                    dos_mouse_deliver(t, 1);
                } else if (t->mev_pm) {
                    static int mev_pm_off_once;
                    if (!mev_pm_off_once) {
                        mev_pm_off_once = 1;
                        kprintf("[4GW] 0Ch upcalls DISABLED by /CONFIG/DOSMEV.CFG "
                                "(protected-mode handler 0x%08x would have been "
                                "far-called)\n", t->mev_pm_lin);
                    }
                } else if (t->mev_seg || t->mev_off) {
                    static int declined_once;
                    if (!declined_once) {
                        declined_once = 1;
                        kprintf("[4GW] MISS INT33/0Ch handler %04x:%04x installed "
                                "from PROTECTED mode and NOT resolvable to a "
                                "linear address inside the arena; upcalls "
                                "declined\n",
                                t->mev_seg, t->mev_off);
                    }
                }
            }
        }

        // THE INJECTION POINT. x86_32_run() has exactly one exit and it is an
        // instruction boundary, so this is a safe place to interrupt the guest
        // by construction rather than by care. Nothing here blocks: the whole
        // path is a table read, an arithmetic comparison and three guest
        // stack writes, so wq_assert_may_block() has nothing to fire on and the
        // concurrency lint has no loop to object to.
        dos4gw_timebase(t);
        // (#sbirq32) And the card's end-of-block interrupt, at the same
        // instruction boundary and for the same reason. NOT inside the focus
        // gate above: a window that has lost focus must stop receiving KEYS,
        // but its sound card has not stopped playing, and a driver waiting on
        // the interrupt would hang the guest for as long as another window is
        // in front of it.
        dos4gw_sb_irq(t, 0);

        {   uint64_t pnow = sched_now_ms();
            // The guest's own frame rate, on its own clock, whether or not this
            // frame is going to be shown. Diagnostic-gated inside.
            if (pnow - last_sample_ms >= DOS_PRESENT_MS) {
                last_sample_ms = pnow;
                dos_redraw_sample(t);
            }
            if (pnow - last_present_ms >= DOS_PRESENT_MS && dos_frame_due(t, pnow, 0)) {
                last_present_ms = pnow;
                { uint64_t _pt = dosprof_t0(); dos_present(t); dosprof_t1(DOSPROF_PRESENT, _pt); }
                if (t->host_slot >= 0) {
                    uint64_t _pt = dosprof_t0();
                    win16_host_invalidate(t->host_slot); dos_publish_mark();
                    dosprof_t1(DOSPROF_PUBLISH, _pt);
                    if (_pt) dosprof_add_publish_bytes_rs((uint64_t)t->win_w * (uint64_t)t->win_h * 4ull);
                }
                frames++;
            }
        }

        // (#740 digplay) WHERE IS A "RUNNING BUT UNRESPONSIVE" GUEST ACTUALLY
        // EXECUTING? DOSDIAG-gated, once every 5 seconds.
        //
        // The Dig's launcher renders, redraws its mouse cursor, burns 78% of a
        // core and answers neither a key nor a click. Every one of those facts
        // is consistent with BOTH "the main loop is spinning on something" and
        // "the main loop is fine and the input never reaches it", and no
        // screendump can tell the two apart. One EIP sample plus the branch
        // ring can: a loop that is spinning has a small, repeating set of
        // branch targets, and a loop that is working does not.
        {
            extern volatile int g_x86_dbgring;
            if (g_x86_dbgring) {
                uint64_t anow = sched_now_ms();
                if (anow - last_alive_ms >= 5000) {
                    last_alive_ms = anow;
                    kprintf("[4GW] alive EIP=0x%08x ESP=0x%08x EAX=%08x EBX=%08x "
                            "ECX=%08x EDX=%08x ESI=%08x EDI=%08x EFL=%08x "
                            "insns=%lu\n",
                            t->le_cpu.eip, t->le_cpu.regs[X32_ESP],
                            t->le_cpu.regs[X32_EAX], t->le_cpu.regs[X32_EBX],
                            t->le_cpu.regs[X32_ECX], t->le_cpu.regs[X32_EDX],
                            t->le_cpu.regs[X32_ESI], t->le_cpu.regs[X32_EDI],
                            t->le_cpu.eflags,
                            (unsigned long)t->le_cpu.insn_count);
                    if (++alive_n % 4 == 0) x86_32_btrace_dump(24);
                }
            }
        }

        if (sched_now_ms() - run_t0 > DOS_MAX_RUN_MS) {
            kprintf("[4GW] run cap reached (%lu ms)\n", (unsigned long)DOS_MAX_RUN_MS);
            why = "run cap";
            break;
        }

        // A scheduler HANDOFF, not a wait. The pass above retired a full slice
        // of guest instructions, so there is no condition being polled and
        // nothing a wait_event() could be armed on; converting this to one
        // would be wrong for the same reason the 16-bit loop's own yield is
        // allowlisted (concurrency-lint allowlist.txt, #dospace 2026-08-07).
        //
        // (#speedcap) Skipped entirely while CAPPED, exactly as the 16-bit loop
        // skips its own: the cap's proc_sleep() above is a strictly better
        // handoff than a yield (it parks the thread on the timer instead of
        // re-entering the ready queue, so a capped guest gives its core back),
        // and yielding as well would only add a scheduler round trip between the
        // sleep and the burst.
        if (!thr_cycles) { uint64_t _pt = dosprof_t0(); proc_yield(); dosprof_t1(DOSPROF_YIELD, _pt); }
        dos_prof_report(t);

        {   // CLOSE THE LOOP, the same way the 16-bit loop does: measure the
            // rate the guest ACTUALLY got and re-size the next burst to
            // DOS_SLICE_MS of wall clock at that rate. This is not a
            // performance tweak here, it is the correctness of the emulated
            // clock: the PIT tick count is instructions * PIT_HZ / rate, so a
            // rate that is wrong by 10x makes a 35 Hz game timer tick at 3.5 Hz.
            uint64_t now = sched_now_ms();
            // (#778, extended by #speedcap to this loop) LIVE SPEED CONTROL.
            // Re-runs the SAME dos_speed_cycles_for() chain the launch did, so
            // the compositor's per-window Speed dialog moves a RUNNING 32-bit
            // guest exactly as it already moved a 16-bit one. One small file
            // read every DOS_SPEED_LIVE_POLL_MS, inside a block that already
            // runs once per already-scheduled burst, so it adds no wait of its
            // own (#426).
            if (path && now - thr_live_poll_ms >= DOS_SPEED_LIVE_POLL_MS) {
                thr_live_poll_ms = now;
                const char *new_src = "default";
                uint32_t newc = dos_speed_cycles_for(path, &new_src);
                if (newc != thr_cycles) {
                    kprintf("[4GW] #778 CPU cap changed live: %u -> %u cycles "
                            "(now from %s)\n", thr_cycles, newc, new_src);
                    thr_cycles = newc;
                    thr_src = new_src;
                    // The new cap pays in from THIS instant, exactly as a fresh
                    // launch would: neither a windfall credit nor a debt run up
                    // under the old target carries across.
                    thr_credit = 0;
                    thr_last_ms = now;
                    thr_last_insns = (unsigned long)t->le_cpu.insn_count;
                }
            }
            if (now - rate_t0 >= DOS_RATE_SAMPLE_MS) {
                uint64_t di = t->le_cpu.insn_count - rate_i0;
                uint64_t dt = now - rate_t0;
                uint32_t hz = (uint32_t)(di * 1000ull / dt);
                // (#176) The 32-bit guest routes its IN/OUT through the SAME
                // dos_in/dos_out, so it is charged the same way and has to be
                // compensated the same way, or DOS/4GW titles would be the only
                // ones whose clock outran real time. See dos_emu_clock_rate():
                // `ch` drives the CLOCK, `hz` still sizes the SLICE.
                uint32_t ch = dos_emu_clock_rate(t, (unsigned long)di, dt,
                                                 &rate_b0, hz);
                // (#speedcap) THE SAME FLOOR BUG #232 HAD TO FIX IN THE 16-BIT
                // LOOP. A bare 100 kHz floor is ABOVE a legitimately capped
                // guest at any cap under 400 cycles, and rejecting the sample
                // would leave g_dos_emu_hz at the old UNCAPPED value, so the
                // guest's PIT would then run tens of times fast: the cap would
                // have broken the one thing this subsystem guarantees. Scale the
                // floor to the cap, exactly as the 16-bit loop does.
                uint32_t hz_floor = thr_cycles ? (thr_cycles * 1000u / 4u) : 100000u;
                if (hz > hz_floor) {
                    // (#176) see the 16-bit loop: a saturated window is a
                    // discontinuity and must not be blended into the average.
                    uint32_t nh = (g_dos_emu_hz && !t->bus_sat_now && !bus_sat_prev)
                        ? (uint32_t)(((uint64_t)g_dos_emu_hz * 3 + ch) / 4)
                        : ch;
                    bus_sat_prev = t->bus_sat_now;
                    dos_emu_rebase(t, nh);      // adopt WITHOUT moving past instants
                    // (#speedcap) The burst size belongs to the CAP while there
                    // is one, same reasoning as the 16-bit loop: DOS_SLICE_MIN
                    // is 20,000 instructions, which at a 500-cycle cap is 40 ms
                    // of guest time in one go, so input and presents would be
                    // sampled 25 times a second instead of ~250.
                    if (!thr_cycles) {
                        unsigned long ns =
                            (unsigned long)(((uint64_t)hz * DOS_SLICE_MS) / 1000ull);
                        if (ns < DOS_SLICE_MIN) ns = DOS_SLICE_MIN;
                        if (ns > DOS_SLICE_MAX) ns = DOS_SLICE_MAX;
                        slice = ns;
                    }
                }
                rate_t0 = now; rate_i0 = t->le_cpu.insn_count;
            }
        }
    }

    dos_present(t);
    if (t->host_slot >= 0) { win16_host_invalidate(t->host_slot); dos_publish_mark(); }

    go32_trace_dump(t);
    go32_cost_dump(t);
    go32_dump_text_page(t);
    t->le_frames = frames;   // for the shared teardown summary below
    kprintf("[4GW] FINISHED: %s. %u instructions retired, %u frames presented.\n",
            why, (uint32_t)t->le_cpu.insn_count, frames);
    kprintf("[4GW] #221 blocking key-read waits: %u\n", input_waits);
    // (#speedcap) The cap this run ACTUALLY enforced, printed unconditionally
    // and at the end, so an exit census says whether the guest was capped
    // without needing the DOSSPEED diagnostic gate armed. "none" is a real
    // answer and is printed, because the absence of a line cannot distinguish
    // "uncapped" from "this kernel does not cap 32-bit guests at all", which is
    // exactly the ambiguity this change removes.
    if (thr_cycles)
        kprintf("[4GW] #232 CPU cap enforced: %u cycles (%u insn/s) from %s\n",
                thr_cycles, thr_cycles * 1000u, thr_src);
    else
        kprintf("[4GW] #232 CPU cap enforced: none (uncapped, host speed)\n");
    {   uint32_t div = t->pit[0].divisor ? t->pit[0].divisor : 65536u;
        kprintf("[4GW] IRQ delivery: INT 08h x%u, INT 09h x%u, INT 1Ch x%u; "
                "masked(IF=0) %u, no-fit %u, no-handler %u\n",
                t->irq_deliv[0x08], t->irq_deliv[0x09], t->irq_deliv[0x1C],
                t->irq_masked, t->irq_nofit, t->irq_novec);
        kprintf("[4GW] guest timebase: PIT divisor %u (%u.%02u Hz), measured %lu insn/s, "
                "emulated elapsed %u PIT ticks\n",
                div, (uint32_t)(DOS_PIT_HZ / div),
                (uint32_t)(((DOS_PIT_HZ % div) * 100u) / div),
                (unsigned long)dos_emu_hz(),
                (uint32_t)dos_emu_pit_now(t));
    }
    // (#740 digsel) THE UNRESOLVED-SELECTOR LINE. It is printed even when the
    // count is zero, because "no selector went unresolved" is the fact that
    // makes the rest of the run's addresses trustworthy, and a line that only
    // appears when something is wrong cannot say that.
    if (t->le_cpu.sel_miss_n) {
        kprintf("[4GW] UNRESOLVED SELECTORS: %u segment loads named a selector no "
                "descriptor describes; first was %04x at EIP 0x%08x. Each one "
                "addressed BASE 0, so a read at offset N landed on guest linear N, "
                "i.e. inside the interrupt vector table for a small N.\n",
                t->le_cpu.sel_miss_n, t->le_cpu.sel_miss_first_sel,
                t->le_cpu.sel_miss_first_eip);
    } else {
        kprintf("[4GW] unresolved selectors: 0 (every LDT-form segment load "
                "resolved to a descriptor this host handed out)\n");
    }
    if (t->le_cpu.sel_gdt_n) {
        kprintf("[4GW]   (plus %u load(s) of a TI=0 value, i.e. a GDT selector or "
                "a real-mode paragraph, which this host never hands out and which "
                "keeps the base-0 behaviour it always had)\n",
                t->le_cpu.sel_gdt_n);
    }
    dos4gw_report_rs(t->le_state);
    dpmi_report_rs();
    dpmi_set_ext_rs(0, 0);
    dpmi_bind_dosmem_rs(0, 0, 0);
}

// (#fmbridge) THE WRAPPER EXISTS SO THAT NO EXIT CAN LEAK THE ARMED FM QUEUE.
//
// MEASURED, not theorised. dos_run_file_inner() calls dos_fmq_host_open() early
// (the queue must be armed before the chip is constructed, or a guest's opening
// instrument bank is dropped), and then has SIX early `return -1` paths for a
// file it cannot read, an image it cannot load, an LE or DOS/4GW prepare that
// fails, or an out-of-memory. Every one of them left the queue ACTIVE forever:
// /APPS/FMSYNTH then never sees ENODEV, never renders its tail, never exits, and
// holds a PCM stream for the rest of the boot. That is the #205 failure with a
// different cause, and it is exactly what a harness run hit - the canonical way
// to arm a Ring-3 guest (dosring3run.py) points DOSRUN.CFG at a path that does
// not exist, so the in-kernel launcher armed the queue and then failed to load,
// and the Ring-3 host that followed was refused a queue nobody was using.
//
// FIX THE MECHANISM, NOT THE SIX INSTANCES. Closing at each early return would
// work today and leak again the first time a seventh is added; this cannot.
// The close is idempotent (dos_fmq_close_rs clears only `active`, and the
// counters survive until the next open), so the normal path - which closes in
// dos_on_terminate() and prints the session summary there - is unaffected.
static int dos_run_file_inner(const char *path);

int dos_run_file(const char *path) {
    int rc = dos_run_file_inner(path);
    dos_fmq_host_close(0, 0);
    // AND g_dos_busy, WHICH IS THE BIGGER HALF OF THE SAME LEAK.
    //
    // dos_launch_common() sets g_dos_busy = 1 before proc_create(), and the
    // ONLY places that clear it are the tail of dos_run_file_inner() and the
    // proc_create-failed branch. So every one of the six early returns below
    // leaves it SET - and dos_launch_common() refuses to start anything while
    // it is set (`[dos] busy (a DOS task is already running)`), as does
    // proc/dosroute.c:199 when deciding whether a guest may go to Ring 3.
    //
    // READ FROM THE CODE, not observed at runtime: ONE DOS program that fails
    // to load - a missing file, a corrupt image, an LE/DOS4GW prepare that
    // fails, an OOM - wedges the ENTIRE DOS subsystem, BOTH paths, for the rest
    // of the boot. Nothing in the tree resets it. The FM queue leak this
    // wrapper was written for is the same bug in a smaller variable, which is
    // why both are cleared in the same place rather than in six.
    //
    // Idempotent on the normal path: dos_run_file_inner() already ends with
    // g_dos_busy = 0, and a second store of 0 is a no-op. Safe against a
    // concurrent launch, because there cannot be one: this runs on the guest's
    // own thread, after that guest has finished, and the launcher that would
    // start the next one is exactly the thing this unblocks.
    g_dos_busy = 0;
    return rc;
}

static int dos_run_file_inner(const char *path) {
    dos_task_t *t = &g_dos;
    memset(t, 0, sizeof(*t));
    // (#dosfs) RE-READ THE VIEW POLICY ON EVERY LAUNCH, not once per boot. The
    // flag inside dos_view_init() only exists to stop the two callers within a
    // single launch (the window sizing, then the self-test report) from reading
    // the file twice; clearing it here is what makes editing DOSVIEW.CFG and
    // relaunching the game enough, with no reboot. A knob that needs a reboot to
    // try is a knob nobody tries.
    g_dos_view_loaded = 0;

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
    // memset(0) would leave access == 0, which means "latch command".
    // (#172) EVERY channel, not just 0. A channel left at access 0 reads as a
    // permanently-latched zero, which is a different wrong answer from the
    // 0xFF that channel 2 used to give but just as unterminatable.
    for (int pc = 0; pc < 3; pc++) {
        t->pit[pc].access  = 3;
        t->pit[pc].divisor = 0;
        t->pit[pc].latched = 0;
        t->pit[pc].rd_hi   = 0;
        t->pit[pc].wr_hi   = 0;
        t->pit[pc].gate    = 0;
    }
    // (#175) OPL2 power-on state. Same block as the PIT above for the same
    // reason: both are timer devices the guest can read, and a reset done
    // somewhere else is a reset that eventually is not done at all.
    //
    // The DOSOPL.CFG gate is read HERE, immediately above its only consumer,
    // and NOT with the other diagnostic gates further down. It was down there
    // for one build and the chip had already been constructed by the time the
    // flag was set, so the forced arm printed that it was on and was not: see
    // the header of tools' patch8 note and the blame.md entry. A gate that is
    // parsed after the device it configures is decoration.
    {   uint32_t _oz = 0;
        void *_oc = fat_read_file(&g_fat_fs, "/CONFIG/DOSOPL.CFG", &_oz);
        if (_oc) {
            char c0 = _oz ? ((char *)_oc)[0] : '0';
            kfree(_oc);
            g_dos_opl2_force = (c0 == '1');
            kprintf("[dos] (#175) DOSOPL.CFG: OPL2 forced %s\n",
                    g_dos_opl2_force ? "INSTALLED (diagnostic arm)" : "ABSENT");
        }
    }
    // (#181) Sound Blaster power-on state, and its gates, parsed HERE for the
    // reason the OPL2 block above gives at length: a gate read after the device
    // it configures has been constructed is decoration, and that exact mistake
    // was made once already on #176.
    {   uint32_t _sz = 0;
        void *_sc = fat_read_file(&g_fat_fs, "/CONFIG/DOSSB.CFG", &_sz);
        if (_sc) {
            char c0 = _sz ? ((char *)_sc)[0] : '0';
            kfree(_sc);
            g_dos_sb_force = (c0 == '1') ? 1 : 0;
            kprintf("[dos] (#181) DOSSB.CFG: Sound Blaster forced %s\n",
                    g_dos_sb_force ? "PRESENT (diagnostic arm)"
                                   : "ABSENT (diagnostic arm)");
        }
        uint32_t _cz = 0;
        void *_cc = fat_read_file(&g_fat_fs, "/CONFIG/DOSSBCAP.CFG", &_cz);
        if (_cc) {
            uint32_t nn = 0;
            for (uint32_t i = 0; i < _cz; i++) {
                char ch = ((char *)_cc)[i];
                if (ch < '0' || ch > '9') break;
                nn = nn * 10 + (uint32_t)(ch - '0');
            }
            kfree(_cc);
            g_dos_sbcap = nn;
            g_dos_sbcap_done = 0;
            kprintf("[dos] (#181) DOSSBCAP.CFG: capturing the first %u guest "
                    "DMA bytes to the serial log\n", g_dos_sbcap);
        }
    }
    wait_queue_head_init(&t->sb_wq);
    t->sb_pump_live = 0;
    t->sb_pump_stop = 0;
    t->sb_blocks = 0;
    t->sb_bytes = 0;
    t->sb_irq_deliv = 0;
    t->sb_irq_unacked = 0;
    t->sb_open_fail = 0;
    dos_dma_init_rs(&t->dma);
    dos_sb_init_rs(&t->sb, sb_installed_policy(), (uint8_t)(DOS_SB_BASE >> 4),
                   DOS_SB_IRQ, DOS_SB_DMA_CHAN, DOS_SB_DSP_MAJOR, DOS_SB_DSP_MINOR);
    // STATE WHICH ARM YOU BELIEVE YOU ARE IN. #176 nearly produced a false
    // conclusion because a diagnostic could not distinguish "present" from
    // "absent", so this line names the arm, the reason, and the sink.
    // The REASON, not just the verdict. The first version of this line said
    // "ABSENT (no PCM sink)" in the arm where DOSSB.CFG had forced it absent on
    // a machine that DID have a sink, which is precisely the defect #176 nearly
    // shipped: a diagnostic that cannot tell you which arm it is really in.
    const char *why;
    if (t->sb.installed)
        why = (g_dos_sb_force == 1) ? "PRESENT (forced by DOSSB.CFG)" : "PRESENT";
    else if (g_dos_sb_force == 0)
        why = "ABSENT (forced by DOSSB.CFG; a sink may well exist)";
    else
        why = "ABSENT (no PCM sink on this machine; nothing changes)";
    kprintf("[dos] (#181) Sound Blaster at 0x%03X IRQ %u DMA %u: %s (DSP %u.%02u, "
            "sink %s)\n", DOS_SB_BASE, DOS_SB_IRQ, DOS_SB_DMA_CHAN, why,
            DOS_SB_DSP_MAJOR, DOS_SB_DSP_MINOR,
            uac_is_ready() ? "USB DAC" : (audio_is_available() ? "HDA/AC97" : "none"));

    // (#182) Open the FM bridge BEFORE the chip is constructed, so that the
    // very first register write a guest makes is already being carried. Opening
    // it after would drop whatever the guest wrote in between, and what a guest
    // writes first is its instrument bank.
    {   uint32_t _fz = 0;
        void *_fc = fat_read_file(&g_fat_fs, "/CONFIG/DOSFM.CFG", &_fz);
        if (_fc) {
            char c0 = _fz ? ((char *)_fc)[0] : '1';
            kfree(_fc);
            g_dos_fm_force_off = (c0 == '0');
            kprintf("[dos] (#182) DOSFM.CFG: FM synthesis %s\n",
                    g_dos_fm_force_off ? "FORCED OFF (measurement arm)" : "enabled");
        }
    }
    dos_fmq_host_open();
    // The queue is open BEFORE the synthesiser starts, so the synthesiser
    // cannot miss a write that arrives while it is still loading.
    dos_fm_launch();
    dos_opl2_init_rs(&t->opl2, opl2_installed_policy());
    // (#182) STATE WHICH ARM THIS IS. #176's diagnostic nearly produced a false
    // conclusion because its config was parsed 350 lines after the chip it
    // configured, and it was caught only because the message named its arm. So
    // this line names the arm and the REASON, every boot, in every arm.
    kprintf("[dos] (#182) FM synthesis arm: %s (fm_ready=%d force_off=%d opl2_force=%d)\n",
            t->opl2.installed
                ? (g_dos_opl2_force ? "PRESENT via DOSOPL.CFG diagnostic override"
                                    : "PRESENT, Ring-3 synthesiser is running")
                : (g_dos_fm_force_off ? "ABSENT, FM forced off by DOSFM.CFG"
                                      : "ABSENT, no Ring-3 synthesiser on this image"),
            g_dos_fm_ready, g_dos_fm_force_off, g_dos_opl2_force);
    // (#182) The "still no FM synthesis" half of this line stopped being true
    // when userland/lib/opl2 landed. A message that describes the world as it
    // was is worse than no message: this one would have told a reader debugging
    // silent music that silence was expected, which is now the wrong answer.
    audiolog_write("[FM] DOS guest starting: OPL2 %s, synthesiser %s (pid %u), "
                   "force_off=%d opl2_force=%d",
                   t->opl2.installed ? "PRESENT" : "ABSENT",
                   g_dos_fm_ready ? "RUNNING" : "not running",
                   g_dos_fm_synth_pid, g_dos_fm_force_off, g_dos_opl2_force);
    kprintf("[dos] (#175/#182) OPL2 at 0x%03X: %s\n", DOS_ADLIB_ADDR,
            t->opl2.installed
                ? (g_dos_fm_ready ? "INSTALLED (detection passes; Ring-3 FM synthesis is LIVE)"
                                  : "INSTALLED via DOSOPL.CFG override (no synthesiser: music WILL be silent)")
                : "ABSENT (empty socket; detection truthfully fails)");
    t->opl2_reported = 0;
    t->port61 = 0;
    t->port61_toggle = 0;
    // #163: INT 33h driver defaults. memset(0) leaves the coordinate range at
    // 0..0 and the mickey ratio at 0, i.e. a cursor pinned to the top-left
    // corner reporting no motion, for any program that uses the driver without
    // first calling function 00h. Every one of these is a value a real driver
    // has before its first call.
    dos_mouse_defaults(t);
    t->mprev_x = 0; t->mprev_y = 0;
    t->mcall_other = 0;
    for (int i = 0; i < 64; i++) t->mcall_n[i] = 0;

    // Emulated timebase starts at zero for this run.
    t->emu_pit_base = 0;
    t->emu_insn_base = 0;
    t->next_irq0_pit = 0;
    t->bios_tick_last = 0;
    t->bios_tick_base = 0;      // (#234a) real value seeded in dos_mem_init()
    t->bios_tick_roll = 0;
    t->text_page = 0;           // (#234b) power-on page
    for (int p = 0; p < DOS_TEXT_PAGES; p++) t->pg_row[p] = t->pg_col[p] = 0;
    // (#176) THE BUS COST, and its gate, parsed HERE because dos_bus_init_rs()
    // on the next line is its only consumer. /CONFIG/DOSBUS.CFG holds the
    // nanoseconds per 8-bit port access; "0" restores the pre-#176 behaviour
    // of charging a port access exactly what a nop costs. Absent means the
    // shipped default, so the file's ABSENCE is the shipping arm and the
    // measurement below is the one that ships.
    {   uint32_t bns = DOS_BUS_NS_DEFAULT;
        uint32_t _bz = 0;
        void *_bc = fat_read_file(&g_fat_fs, "/CONFIG/DOSBUS.CFG", &_bz);
        if (_bc) {
            uint32_t n = 0; int got = 0;
            for (uint32_t i = 0; i < _bz; i++) {
                char ch = ((char *)_bc)[i];
                if (ch < '0' || ch > '9') break;
                n = n * 10 + (uint32_t)(ch - '0'); got = 1;
            }
            kfree(_bc);
            if (got) {
                bns = n;
                kprintf("[dos] (#176) DOSBUS.CFG: port I/O bus cost overridden "
                        "to %u ns per access (diagnostic arm)\n", bns);
            }
        }
        dos_bus_init_rs(&t->bus, bns);
        t->bus_saturated = 0;
        t->bus_sat_now = 0;
        kprintf("[dos] (#176) port I/O bus cost: %u ns per access (%s)\n",
                t->bus.ns_per_access,
                t->bus.ns_per_access ? "ISA 8-bit cycle; delay loops written as "
                                       "port reads now self-calibrate"
                                     : "FREE - pre-#176 behaviour, a port read "
                                       "costs what a nop costs");
    }

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
    //
    // (#740) AND IF THE PROGRAM LIVES ON A DRIVE LETTER, START ON THAT DRIVE.
    // A binary at /WINDIR/DRIVE_E/DWB.EXE is, to the guest, E:\DWB.EXE, so the
    // current drive is E: and E:'s current directory is its parent. Seeding the
    // whole native path as C:'s directory (the old behaviour, kept below for
    // the native namespace where there IS no drive letter) told a guest that
    // asked AH=19h it was on C: while every one of its files was on E:. That
    // matters now that a bare relative name resolves through the current
    // directory: the two answers have to name the same place.
    {
        const char *d = t->appdir;
        const char *pfx = "/WINDIR/DRIVE_";
        int i = 0;
        while (pfx[i] && d[i] == pfx[i]) i++;
        char dl = (pfx[i] == '\0') ? d[i] : 0;
        if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 32);
        if (dl >= 'A' && dl <= 'Z' && (d[i + 1] == '/' || d[i + 1] == '\0')) {
            const char *rel = d + i + 1;
            while (*rel == '/') rel++;
            t->svc.cur_drive = dl;
            t->svc.cwd_set(&t->svc, dl, rel);
            kprintf("[dos] program is on drive %c:; current directory '%s'\n", dl, rel);
        } else {
            if (*d == '/') d++;
            t->svc.cwd_set(&t->svc, t->svc.cur_drive, d);
        }
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
    dos_build_psp(t, path);
    {   // #740: WHICH FORMAT IS THIS?
        //
        // A wbind-ed DOS/4GW binary IS an MZ: the extender's 16-bit stub is a
        // real MZ image with a real entry point, and before this branch existed
        // dos_load_image() loaded and ran it, which is why an LE used to print
        // the extender's complaint or derail rather than failing loudly. The LE
        // is found through an INNER MZ (docs/DOS4GW_LE_FORMAT.md), which is
        // exactly what le_find_rs() looks for, over bytes we already hold.
        uint32_t _mzo = 0, _leo = 0;
        int _isle = (le_find_rs((const uint8_t *)data, size, &_mzo, &_leo) == LE_OK);
        // (#211) THE SECOND STUBBED FORMAT. A DJGPP v2 program is ALSO an MZ,
        // and before this branch existed dos_load_image() loaded and ran its
        // 16-bit go32 stub, which probed for a DPMI host, found none and
        // printed its own "Load error: no DPMI". The parser is silent and
        // refuses a plain MZ (GO32_E_NOSTUB), so an ordinary DOS program is
        // unaffected: the marker has to be in the file.
        go32_image_t _g;
        int _isgo32 = 0;
        if (!_isle)
            _isgo32 = (go32_parse_rs((const uint8_t *)data, size, &_g) == GO32_OK);
        if (_isgo32) {
            kprintf("[dos] '%s' is a stubbed DJGPP v2 i386 COFF: loading the COFF "
                    "directly and entering it in 32-bit protected mode, rather than "
                    "running its go32 stub (which would look for CWSDPMI)\n", path);
            go32_report_rs(path, &_g);
            if (go32_prepare(t, path, (const uint8_t *)data, size, &_g) != 0) {
                kfree(data); kfree(t->mem); return -1;
            }
        } else if (_isle) {
            kprintf("[dos] '%s' is a Linear Executable (LE at file offset 0x%08x, "
                    "anchor MZ 0x%08x): running it as a 32-bit DOS/4GW guest, not "
                    "as its 16-bit extender stub\n", path, _leo, _mzo);
            if (dos4gw_prepare(t, path, (const uint8_t *)data, size) != 0) {
                kfree(data); kfree(t->mem); return -1;
            }
        } else if (dos_load_image(t, (const uint8_t *)data, size) != 0) {
            kfree(data); kfree(t->mem); return -1;
        }
    }
    kfree(data);

    // default grayscale palette so something is visible before the app sets it
    for (int i = 0; i < 256; i++) { t->pal[i][0] = t->pal[i][1] = t->pal[i][2] = (uint8_t)(i >> 2); }

    x86_16_set_int_handler(&t->cpu, dos_int_handler);
    x86_16_set_io_handlers(&t->cpu, dos_in, dos_out);
    // Route 0xA0000-0xAFFFF through the EGA planar emulation (#202).
    x86_16_set_mem_hook(&t->cpu, VGA_A000, VGA_A000_END, ega_mem_w, ega_mem_r);
    // (#745) XMS + EMS: allocate the arenas and plant the discovery stubs (the
    // XMS far entry point, the EMM device header, the INT 67h vector). This
    // MUST precede the IVT seeding below, which fills only still-null vectors.
    dos_mem_init(t);
    // (#740) Plant the emulated video-BIOS ROM page so the far pointers that
    // 4F00h and 4F01h hand out point at something the guest can actually read.
    // Built by the Rust that also builds the structures pointing INTO it, so
    // the pointer and its target have one definition.
    vbe_build_rom_rs(&t->mem[VBE_ROM_LIN], VBE_ROM_SIZE);
    // EGA defaults until the game sets a mode.
    t->seq_map_mask = 0x0F; t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
    // (#740) Chain-4 ON at power-on. A zeroed seq_reg[4] reads as UNCHAINED,
    // which would make every plain mode 13h game take the Mode X path.
    t->seq_reg[4] = 0x0E;
    for (int i = 0; i < 16; i++) t->atc_pal[i] = (uint8_t)i;

    // Create the host window (compositor draws it). win16_host_create() takes the
    // OUTER size and the decoration eats the difference, so asking for 640x404
    // produced a 636x380 CONTENT rect: mode 13h was being scaled by 1.9875 x 1.9
    // instead of a clean 2x, and the 80x25 text grid lost one pixel column in 160
    // and one row in 20. At an 8x16 cell that is the difference between "Shareware"
    // and "Sharcwarc". Measure the decoration once and ask again, so the content is
    // EXACTLY 640x400: an integer 2x for 320x200 and whole 8x16 cells for 80x25.
    int want_w = TEXT_COLS * FONT_WIDTH;    // 640 == MODE13_W * WIN_SCALE
    int want_h = TEXT_ROWS * FONT_HEIGHT;   // 400 == MODE13_H * WIN_SCALE
    // (#dosfs) AND THEN OPEN IT AS BIG AS THE ERA-APPROPRIATE POLICY ALLOWS.
    //
    // The owner's request was that DOS games "open in a full screen view
    // that's using a 640x480 or 800x600 resolution". 640x400 of content is an
    // eighth of a 3840x2160 panel by area, so the window opened postage-stamp
    // sized and every user had to resize it by hand before anything looked
    // right. This asks the SAME policy the present path uses (rustkern/
    // doswin.rs) how big a picture it would allow on THIS screen, and opens the
    // window at exactly that, centred.
    //
    // WHY NOT NATIVE FULLSCREEN. window_fullscreen_enter() would be the literal
    // reading of the request, and it is deliberately not used: it requires the
    // window to hold FOCUS to stay fullscreen (wm_fullscreen_active()), it is
    // policed by a compositor watchdog keyed on the content-COMMIT sequence
    // that this subsystem does not use (it invalidates instead), and the
    // fullscreen/maximise scaling path is under active repair elsewhere. A
    // plain, correctly sized window needs none of that machinery and cannot be
    // dragged into its failure modes. The user can still maximise or fullscreen
    // it by hand and the present path handles either, because the geometry is
    // recomputed from the CURRENT buffer size every frame.
    //
    // 80x25 TEXT IS THE RIGHT MODE TO SIZE FROM, even though most guests are
    // about to switch away from it: it is the mode every guest starts in, and
    // sizing from a mode the guest has not chosen yet would be a guess. The
    // window stays put across the mode change and the picture inside it is
    // re-fitted every present, so a 320x200 game in a window sized for 640x400
    // simply lands on a larger whole multiple (6x rather than 3x).
    {
        dos_view_init(0, 0);
        int wax, way, waw, wah;
        win16_host_work_area(&wax, &way, &waw, &wah);
        (void)wax; (void)way;
        // The work area is the screen minus the dock/taskbar insets, and it is
        // already clamped non-empty, so no zero check is needed. Leave a margin
        // so the frame's own decoration cannot push the window off it.
        int32_t aw = (int32_t)waw - 64, ah = (int32_t)wah - 96;
        dos_view_policy_t open_pol = g_dos_view;
        // integer=ALWAYS for the OPENING SIZE specifically. Everywhere else the
        // default is "integer only when a limit binds", so that no window size
        // that ships today changes by a pixel; but here there is no existing
        // size to preserve, and a window whose content is an exact multiple of
        // 640x400 is the one that renders the 80x25 text grid on whole 8x16
        // cells - the same argument that made the original 640x400 window
        // exactly 640x400 (see just above: "Sharcwarc").
        open_pol.integer = 2;
        // (no-ticket) AND ITS OWN CAP, not the picture's. One number governing
        // both is what let a budget raise multiply the DEFAULT window sixteen-
        // fold on a 3840x2160 panel while nobody was looking: the opening size
        // is not something the user asked for, and the budget is.
        open_pol.budget_px = dos_view_open_budget_rs(g_dos_view.budget_px);
        dos_rect_t orect;
        if (aw > 0 && ah > 0 &&
            dos_present_rect_rs(aw, ah, want_w, want_h, want_w, want_h,
                                &open_pol, &orect) &&
            orect.w >= want_w && orect.h >= want_h) {
            want_w = orect.w;
            want_h = orect.h;
        }
    }
    // (#234d) NAME THE WINDOW AFTER THE PROGRAM. Every DOS guest used to open a
    // window and a taskbar button labelled "DOS", so with two of them up you
    // could not tell Rogue from NetHack without looking inside, and with one up
    // the taskbar told you the subsystem rather than the thing you launched.
    char wtitle[40];
    dos_guest_title(path, wtitle, (int)sizeof(wtitle));
    // (#dosfs) CENTRED, not at a fixed (80,60). A 1920x1200 window pinned to
    // the top-left of a 3840x2160 panel is not what "full screen view" means,
    // and the old constant only ever looked right because the window was small.
    int win_x = 80, win_y = 60;
    {
        int wax, way, waw, wah;
        win16_host_work_area(&wax, &way, &waw, &wah);
        int32_t cx = (int32_t)wax + ((int32_t)waw - want_w) / 2;
        int32_t cy = (int32_t)way + ((int32_t)wah - want_h) / 2;
        if (cx > win_x) win_x = (int)cx;
        if (cy > win_y) win_y = (int)cy;
    }
    t->host_slot = win16_host_create(wtitle, win_x, win_y, want_w, want_h,
                                     &t->win_buf, &t->win_w, &t->win_h, 0);
    if (t->host_slot >= 0 && (t->win_w != want_w || t->win_h != want_h)) {
        int dw = want_w - t->win_w, dh = want_h - t->win_h;
        win16_host_destroy(t->host_slot);
        t->host_slot = win16_host_create(wtitle, win_x, win_y, want_w + dw, want_h + dh,
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
    // (#211) AND TAKE KEYBOARD FOCUS. #156 gates every keystroke on this window
    // holding it, and nothing was giving it to a launched guest, so a DOS
    // program that waits for a key waited forever. See the note in
    // proc/syscall.c's win16_host_focus().
    win16_host_focus(t->host_slot);
    kprintf("[dos] host window %d focused (input tap will arm on the next loop "
            "pass; #156 gates keyboard and mouse on this)\n", t->host_slot);
    {   // (#745 local 105) A geometry rule that has never been watched being
        // right is a comment. This costs a few dozen integer ops, once.
        int bad = dos_letterbox_selftest_rs();
        kprintf("[dos] letterbox selftest: %s (%d failing)\n",
                bad == 0 ? "PASS" : "FAIL", bad);
        {   // (#dosfs) THE VIEW POLICY: load it, prove it, and PRINT THE
            // EFFECTIVE VALUE. All three, because blame.md records the #mickey
            // re-home interval that had a Rust default, a mirrored C default and
            // no line saying which one won - so a changed constant did nothing
            // and it cost a whole verification run to find out.
            //
            // The default comes from Rust and is only overwritten where the
            // config file actually asked. An absent DOSVIEW.CFG is the NORMAL
            // state (it is not in the golden) and says nothing beyond the
            // effective line below.
            //
            // /CONFIG is on the ext2 ROOT, not the FAT ESP. fat_read_file()
            // routes it there via fat_path_on_ext2(), which is why this uses the
            // same call the three DOS config files beside it use rather than a
            // private one (blame.md: autorun_worker() hardcoded a path that the
            // ESP has no CONFIG directory for, and has been unreachable on every
            // two-partition golden since).
            int _vread = 0, _vn = 0;
            dos_view_init(&_vread, &_vn);   // already done by the window sizing
            int vbad = dos_view_selftest_rs();
            kprintf("[dos] (#dosfs) view selftest: %s (%d failing)\n",
                    vbad == 0 ? "PASS" : "FAIL", vbad);
            kprintf("[dos] (#dosfs) view policy EFFECTIVE: budget=%d px "
                    "integer=%s max=%dx%d aspect=%s (DOSVIEW.CFG %s%s)\n",
                    g_dos_view.budget_px,
                    g_dos_view.integer == 2 ? "always"
                                            : (g_dos_view.integer ? "on" : "off"),
                    g_dos_view.max_w, g_dos_view.max_h,
                    g_dos_view.aspect ? "crt(4:3)" : "square",
                    _vread > 0 ? "read" : (_vread == 0 ? "absent, defaults"
                                                        : "loaded at window sizing"),
                    _vread > 0 ? (_vn ? ", applied" : ", nothing parsed") : "");
        }
        // (#mickey) The mouse-counter model, driven against a MODEL OF THE
        // GUEST taken from The Dig's own handler (integrate the differences,
        // clamp to the box). It asserts the reported defect directly: a host
        // move of +107 down must move that model by +107, not +219, and the
        // first event must land it ON the cursor rather than 210 pixels away.
        int kbad = dos_mick_selftest_rs();
        kprintf("[dos] (#mickey) mouse counter selftest: %s (%d failing); "
                "counter gain %s, re-home every %u delivered move events (%s)\n",
                kbad == 0 ? "PASS" : "FAIL", kbad,
                t->mick.gain_ratio ? "GUEST RATIO (pre-fix compat)" : "unit",
                t->mick.home_every, t->mick.home_every ? "on" : "OFF");
        // (#172) The free-space report decides whether a guest starts at all,
        // and its regression case is the exact MCB geometry that made Stunts
        // print "Not enough memory to load program." with 464 KB free.
        int mbad = dos_mcb_selftest_rs();
        kprintf("[dos] #172 MCB free-space selftest: %s (%d failing)\n",
                mbad == 0 ? "PASS" : "FAIL", mbad);
        // (#172) And the PIT register protocol, whose channel-2 half is the
        // reason Stunts reached mode 13h and then span forever.
        int pbad = dos_pit_selftest_rs();
        kprintf("[dos] #172 PIT channel selftest: %s (%d failing)\n",
                pbad == 0 ? "PASS" : "FAIL", pbad);
        // (#175) The AdLib probe, BOTH arms. The absent arm is what ships, so
        // testing only that arm would score a pass for `return ABSENT;` and
        // prove nothing about the protocol. The installed arm is the one that
        // has to keep working the day FM synthesis lands.
        int bbad = (int)dos_bus_selftest_rs();
        // The C-side constant and the Rust one are asserted to agree here
        // rather than left to a comment: DOS_BUS_NS_DEFAULT appears in both
        // files and nothing else would notice them drifting apart.
        {   dos_bus_t probe;
            dos_bus_init_rs(&probe, DOS_BUS_NS_DEFAULT);
            if (probe.ns_per_access != DOS_BUS_NS_DEFAULT) bbad++;
        }
        kprintf("[dos] #176 port-I/O bus-cost selftest: %s (%d failing, "
                "shipped cost %u ns/access)\n",
                bbad ? "FAIL" : "PASS", bbad, DOS_BUS_NS_DEFAULT);
        {   // (#252) INT 15h AH=86h. The self-test's load-bearing case is that
            // 80 us charges 95 PIT ticks, which is the AdLib datasheet period
            // the detection protocol waits out: below 95 and the probe fails
            // exactly as it did before this change, with nothing to see.
            uint32_t i15 = dos_int15_selftest_rs();
            kprintf("[dos] #252 INT 15h AH=86h WAIT selftest: %s (%u failing)\n",
                    i15 ? "FAIL" : "PASS", (unsigned)i15);
        }
        {   // (#182) The FM bridge's own self-test, run against the REAL queue.
            // It exercises the overflow path explicitly, because "drop the
            // NEWEST, not the oldest" is a decision that would otherwise be a
            // comment nobody ever ran.
            //
            // IT LEAVES THE QUEUE CLOSED, WHICH IS CORRECT FOR THE TEST AND
            // WRONG FOR THIS CALL SITE, so this call site puts it back.
            //
            // Measured on VM <vmid> build 2001 before the re-open existed: the
            // caller opens the queue for the guest that is starting, then this
            // test closed it, then FMSYNTH's first drain returned ENODEV and it
            // exited, and Keen 5 went on to write its whole 264-register
            // instrument bank into a queue nobody was reading. The chip said
            // PRESENT and the machine was silent, which is the fabrication
            // #175 refused to ship.
            //
            // The postcondition stays in the Rust (a self-test must not leave a
            // device armed); the knowledge that a guest is starting lives HERE,
            // so the restore lives here too.
            // (#fmbridge) the re-open is part of dos_fmq_host_selftest() now,
            // beside the close that makes it necessary, so both rings get it.
            int fbad = dos_fmq_host_selftest();
            kprintf("[dos] #182 FM bridge selftest: %s (%d failing); queue "
                    "re-opened after the selftest closed it\n",
                    fbad == 0 ? "PASS" : "FAIL", fbad);
        }
        int obad = dos_opl2_selftest_rs();
        kprintf("[dos] #175 OPL2 detect selftest: %s (%d failing)\n",
                obad == 0 ? "PASS" : "FAIL", obad);
        // (#181) BOTH ARMS, ALWAYS, for the reason #175 states: a detection
        // exercised only in the arm it currently ships in would score a pass
        // for `return ABSENT;` and prove nothing.
        int sbad = dos_sb_selftest_rs();
        kprintf("[dos] #181 SB DSP+8237 selftest: %s (%d failing)\n",
                sbad == 0 ? "PASS" : "FAIL", sbad);
        // #163: same argument for the displayed-row derivation. It now decides
        // how much of display memory every graphics frame shows, so an untested
        // version of it is a whole-screen defect waiting for a program that
        // reprograms the vertical timing.
        int rbad = dos_vga_rows_selftest_rs();
        kprintf("[dos] #163 vga-rows selftest: %s (%d failing)\n",
                rbad == 0 ? "PASS" : "FAIL", rbad);
        int crbad = dos_crtc_selftest_rs();
        kprintf("[dos] #740 CRTC mode-set selftest: %d failing case(s)%s\n",
                crbad, crbad ? " -- FAIL" : " (0Dh->13h stride is 320)");
        int mxbad = dos_modex_selftest_rs();
        kprintf("[dos] modex geometry selftest: %d failing case(s)\n", mxbad);
        if (mxbad) kprintf("[dos] WARNING: Mode X geometry decode is WRONG on this build\n");
    }
    {   // (#740) The same argument for the VBE structure layouts, the spec byte
        // orders, and every deliberate refusal: a structure that has never been
        // watched being right is a comment. This prints on every guest launch so
        // the evidence lands in the boot log rather than in a test nobody runs.
        int bad = vbe_selftest_rs();
        kprintf("[dos] VBE selftest: %s (%d failing)\n",
                bad == 0 ? "PASS" : "FAIL", bad);
    }
    {   // (#212) CGA. The check COUNT is printed for the reason #514 gives:
        // a self-test that ran zero assertions and one that passed look
        // identical otherwise. The interesting assertions are the even/odd
        // bank interleave and the BIOS palette default, because both are
        // wrong-but-plausible failures: an ignored odd bank draws half a
        // picture, and a wrong default makes every CGA title the wrong hue.
        // The 16 KB aperture is kmalloc'd, NOT a stack array: see the
        // self-test's own header for the panic that taught us the difference.
        // A skipped self-test is reported as a SKIP and never as a pass, which
        // is the entire reason the check count is printed (#514).
        uint32_t ck = 0;
        uint8_t *scratch = (uint8_t *)kmalloc(CGA_APERTURE);
        if (!scratch) {
            kprintf("[dos] #212 CGA present selftest SKIPPED: kmalloc(%u) failed\n",
                    (unsigned)CGA_APERTURE);
        } else {
            int bad = cga_selftest_rs(scratch, CGA_APERTURE, &ck);
            kprintf("[dos] #212 CGA present selftest: %s (%u checks%s)\n",
                    bad == 0 ? "PASS" : "FAIL", ck,
                    bad == 0 ? "" : ", first failing check id above");
            kfree(scratch);
        }
    }
    {   // (#745) XMS/EMS. Same argument again, and it bites harder here than
        // anywhere above: a memory manager that returns a WRONG-BUT-PLAUSIBLE
        // answer does not crash, it corrupts. A free-space figure that is too
        // large, a block move that drops the last byte, an EMS window that is
        // not written back before it is remapped - none of those fail visibly.
        // They fail as a guest that loads slightly wrong data and dies later
        // somewhere unrelated, which is the most expensive kind of bug this
        // tree produces.
        //
        // Once per boot, not once per launch: the scratch is a whole megabyte
        // of guest address space plus two arenas, and the code under test does
        // not change between launches.
        static int mem_selftested = 0;
        if (!mem_selftested) {
            mem_selftested = 1;
            void    *sx = kmalloc(dos_xms_state_size_rs());
            void    *se = kmalloc(dos_ems_state_size_rs());
            uint8_t *px = (uint8_t *)kmalloc(64 * 1024);          // 64 KB arena
            uint8_t *pe = (uint8_t *)kmalloc(4 * 16384);          // 4 EMS pages
            uint8_t *sm = (uint8_t *)kmalloc(DOS_MEM_SIZE);       // scratch guest RAM
            if (sx && se && px && pe && sm) {
                memset(sx, 0, dos_xms_state_size_rs());
                memset(se, 0, dos_ems_state_size_rs());
                memset(sm, 0, DOS_MEM_SIZE);
                int mbad = dos_mem_selftest_rs(sx, px, se, pe, sm);
                kprintf("[dos] XMS/EMS selftest: %s (first failing check: %d)\n",
                        mbad == 0 ? "PASS" : "FAIL", mbad);
                if (mbad)
                    kprintf("[dos] WARNING: extended/expanded memory is WRONG on this "
                            "build; a guest that uses it will be given bad data\n");
            } else {
                kprintf("[dos] XMS/EMS selftest SKIPPED: scratch kmalloc failed\n");
            }
            if (sx) kfree(sx);
            if (se) kfree(se);
            if (px) kfree(px);
            if (pe) kfree(pe);
            if (sm) kfree(sm);
        }
    }
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
    // (#234e) Power-on cursor shape. Zero here would mean "scanlines 0..0", a
    // one-pixel bar across the TOP of every cursor cell, which is not what a
    // BIOS leaves behind and is not what dos_present_text should draw before
    // the guest has said anything. Two scanlines at the bottom of the cell is
    // the standard VGA 8x16 underline.
    wr16(t, 0x0040, 0x0060,
         (uint16_t)(((FONT_HEIGHT - 2) << 8) | (FONT_HEIGHT - 1)));
    {   // (#234a) SEED THE BIOS TICK FROM THE REAL TIME OF DAY. Zero here is
        // what made every clock-seeded DOS game deterministic, and hung the
        // ones (Epyx Rogue) whose generator has zero as a fixed point.
        kdos_clock_t kc; kdos_clock_now(&kc);
        t->bios_tick_base = kc.ticks;
        t->bios_tick_roll = 0;
        t->bios_tick_last = kc.ticks;
        wr16(t, 0x0040, 0x006C, (uint16_t)(kc.ticks & 0xFFFF));
        wr16(t, 0x0040, 0x006E, (uint16_t)(kc.ticks >> 16));
        kprintf("[dos] #234a BIOS tick seeded %u = %02u:%02u:%02u.%02u "
                "(%04u-%02u-%02u, clock %s)\n",
                (unsigned)kc.ticks, kc.hour, kc.minute, kc.second, kc.hundredth,
                kc.year, kc.month, kc.day,
                kc.known ? "RTC" : "uptime-only (no RTC date)");
    }

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
        wr8(t, 0xF000, DOS_IRET_STUB, 0xCF);      // IRET
        // #163: THE MOUSE VECTOR CANNOT BE AN IRET STUB.
        //
        // The documented way to detect a mouse driver, printed in every DOS
        // programming reference of the period, is: read the INT 33h vector; if
        // it is 0000:0000 OR THE BYTE IT POINTS AT IS 0CFh (IRET), no driver is
        // installed. The loop below pointed every unhooked vector at an IRET,
        // so a program using that test concluded there was no mouse and never
        // issued a single INT 33h call. Nothing downstream of that decision can
        // be fixed by improving int33(): it is never reached.
        //
        // INT 33h is serviced natively by dos_int_handler and never goes
        // through the IVT, so the stub's own body only matters if a program
        // FAR CALLS the vector instead of issuing the interrupt (a real and
        // period-typical idiom, done for speed). `INT 33h; RETF` handles that
        // case correctly and its first byte is 0CDh, which is not 0CFh.
        wr8(t, 0xF000, DOS_INT33_STUB,     0xCD);  // INT 33h
        wr8(t, 0xF000, DOS_INT33_STUB + 1, 0x33);
        wr8(t, 0xF000, DOS_INT33_STUB + 2, 0xCB);  // RETF
        // #163: the far-return landing pad for a 0Ch mouse event upcall. See
        // dos_mouse_events().
        wr8(t, 0xF000, DOS_MEVRET_STUB,     0xEB);  // JMP $
        wr8(t, 0xF000, DOS_MEVRET_STUB + 1, 0xFE);
        // (#dpmi301) the landing pad a 0300h-executed real-mode handler IRETs
        // to. See dos4gw_rm_exec_guest().
        wr8(t, 0xF000, DOS_RMCALLRET_STUB,     0xEB);  // JMP $
        wr8(t, 0xF000, DOS_RMCALLRET_STUB + 1, 0xFE);
        // (#740) The BIOS timer handler. See DOS_BIOSTIMER_STUB.
        wr8(t, 0xF000, DOS_BIOSTIMER_STUB,     0xCD);  // INT 1Ch
        wr8(t, 0xF000, DOS_BIOSTIMER_STUB + 1, 0x1C);
        wr8(t, 0xF000, DOS_BIOSTIMER_STUB + 2, 0xCF);  // IRET
        for (int v = 0; v < 256; v++) {
            // (raplay) THE DPMI HOST'S REAL-MODE VECTOR SHADOW IS SEEDED FROM
            // THE SAME CHOOSER, unconditionally and before the "is it null"
            // test below, because the shadow describes what a real-mode vector
            // WOULD hold and that is the stub whether or not this particular
            // arena entry needed writing. rustkern/dpmi.rs used to initialise
            // all 256 of its entries to the IRET stub, so DPMI 0200h answered
            // F000:FF53 for vector 33h and every protected-mode guest that ran
            // the documented mouse-driver test was told there is no mouse.
            // One chooser, two consumers; see the note at RMVEC.
            //
            // (rakbd2) ...EXCEPT THE VECTORS WHOSE CORRECT CONTENT IS NOTHING.
            // dos_vec_seed_free() names them and is the only place that does,
            // so the shadow, the arena and dos_vec_hooked() cannot come to
            // disagree about whether 60h is taken. Leaving the arena entry at
            // 0000:0000 is not "skipping" it: 0000:0000 is what a real DOS
            // leaves there, and it is what Red Alert reads to find a vector it
            // may use.
            if (dos_vec_seed_free((uint8_t)v)) {
                dpmi_rmvec_seed_rs((uint8_t)v, 0x0000, 0x0000);
                continue;
            }
            dpmi_rmvec_seed_rs((uint8_t)v, 0xF000, dos_vec_seed_stub((uint8_t)v));
            uint16_t off = rd16(t, 0x0000, (uint16_t)(v * 4));
            uint16_t seg = rd16(t, 0x0000, (uint16_t)(v * 4 + 2));
            if (seg == 0 && off == 0) {
                uint16_t stub = dos_vec_seed_stub((uint8_t)v);
                wr16(t, 0x0000, (uint16_t)(v * 4),     stub);
                wr16(t, 0x0000, (uint16_t)(v * 4 + 2), 0xF000);
            }
        }
    }

    t->running = 1;
    // Raw scancode mirroring is on for the WHOLE run, not only once a guest
    // hooks INT 9: it is now also the source for INT 16h, which needs the scan
    // code and not just an ASCII byte. cpu/isr.c mirrors rather than diverts, so
    // the kernel's own keyboard path is unaffected.
    //
    // #156: it used to also be on for the WHOLE run regardless of whether this
    // window held compositor focus, which is a straight duplicate delivery
    // ("two consumers, one keystroke", same shape as #68's AI-launcher leak):
    // the compositor's own SYS_INJECT_KEY path already delivers correctly to
    // whichever window is actually focused (Task Manager, Settings, anything),
    // and this tap ALSO fed every one of those same keystrokes (and, via
    // dos_pump_input()'s unconditional read of the global cursor, every mouse
    // move and click) straight into the DOS guest, focused or not. The tap is
    // now re-armed every loop iteration below, gated on win16_host_is_focused().
    // Start focused: win16_host_create() (called by our caller, just above)
    // already focused this window, so the initial state below is consistent
    // with reality rather than a guess.
    t->last_focused = 1;
    g_dos_scancode_tap = 1;
    dos_scancode_clear();
    dos_keyq_reset(t);
    {   /* #201 derail ring: only when /CONFIG/DOSDIAG.CFG is present */
        extern volatile int g_x86_dbgring;
        uint32_t _dz = 0;
        void *_dc = fat_read_file(&g_fat_fs, "/CONFIG/DOSDIAG.CFG", &_dz);
        // (#740) g_int21_trace has existed in dos/int21svc.c since #736 with the
        // comment "Set from a debugger / RC to log every INT 21h call", and
        // NOTHING EVER SET IT: `grep -rn g_int21_trace kernel/` found its
        // definition and its one use and no writer at all. A diagnostic with no
        // way to turn it on is this project's characteristic dead-feature
        // shape, so it is wired to the gate that already exists rather than to
        // a new one. Off in the golden, because DOSDIAG.CFG is not shipped.
        extern int g_int21_trace;
        if (_dc) { kfree(_dc); g_x86_dbgring = 1; g_int21_trace = 1; }
        {   // (#rafault) /CONFIG/DOSMEV.CFG: "0" declines the INT 33h 0Ch
            // upcall to a 32-bit guest. See g_dos_mev_upcall.
            uint32_t _mz = 0;
            void *_mc = fat_read_file(&g_fat_fs, "/CONFIG/DOSMEV.CFG", &_mz);
            if (_mc) {
                char c0 = _mz ? ((char *)_mc)[0] : '1';
                kfree(_mc);
                g_dos_mev_upcall = (c0 == '0') ? 0 : 1;
                kprintf("[dos] #rafault INT 33h 0Ch upcalls to a 32-bit guest: %s "
                        "(/CONFIG/DOSMEV.CFG)\n",
                        g_dos_mev_upcall ? "ENABLED" : "DISABLED");
            }
        }
        {   // (#mickey) /CONFIG/DOSMOUSE.CFG: how the guest's mouse counters
            // behave. Two settings, both of which exist because the right
            // answer depends on the guest and cannot be detected:
            //   home=off | home=<n>   re-home interval in delivered move
            //                         events (default 120). Homing is what puts
            //                         a mickey-integrating guest's private
            //                         pointer under the host cursor.
            //   gain=ratio            put the guest's mickeys-per-pixel ratio
            //                         back into the counters. That is the
            //                         pre-fix behaviour and is correct ONLY for
            //                         a guest that divides by the same ratio;
            //                         it is the documented lever for a title
            //                         that turns out to do so, so a future
            //                         regression needs no rebuild.
            uint32_t _mmz = 0;
            void *_mmc = fat_read_file(&g_fat_fs, "/CONFIG/DOSMOUSE.CFG", &_mmz);
            if (_mmc) {
                const char *ms = (const char *)_mmc;
                for (uint32_t i = 0; i + 5 <= _mmz; i++) {
                    if (ms[i] == 'h' && ms[i+1] == 'o' && ms[i+2] == 'm' &&
                        ms[i+3] == 'e' && ms[i+4] == '=') {
                        uint32_t j = i + 5, v = 0; int digits = 0;
                        if (j + 3 <= _mmz && ms[j] == 'o' && ms[j+1] == 'f' &&
                            ms[j+2] == 'f') { g_dos_mick_home_every = 0; break; }
                        for (; j < _mmz && ms[j] >= '0' && ms[j] <= '9'; j++) {
                            v = v * 10u + (uint32_t)(ms[j] - '0');
                            digits++;
                        }
                        if (digits) g_dos_mick_home_every = v;
                        break;
                    }
                }
                for (uint32_t i = 0; i + 10 <= _mmz; i++) {
                    if (ms[i] == 'g' && ms[i+1] == 'a' && ms[i+2] == 'i' &&
                        ms[i+3] == 'n' && ms[i+4] == '=' && ms[i+5] == 'r' &&
                        ms[i+6] == 'a' && ms[i+7] == 't' && ms[i+8] == 'i' &&
                        ms[i+9] == 'o') { g_dos_mick_gain_ratio = 1; break; }
                }
                kfree(_mmc);
                if (g_dos_mick_home_every != DOS_MICK_HOME_UNSET)
                    t->mick.home_every = g_dos_mick_home_every;
                t->mick.gain_ratio = g_dos_mick_gain_ratio;
                kprintf("[dos] (#mickey) DOSMOUSE.CFG: re-home every %u move "
                        "events (%s), counter gain %s\n",
                        t->mick.home_every,
                        t->mick.home_every ? "on" : "OFF",
                        t->mick.gain_ratio ? "GUEST RATIO (pre-fix)" : "unit");
            }
        }
        {   // #232 /CONFIG/DOSSPEED.CFG: arm the periodic delivered-rate line.
            // Separate from DOSDIAG.CFG on purpose: DOSDIAG also turns on the
            // INT 21h trace, which floods the serial port and would itself
            // change the rate being measured.
            uint32_t _sz = 0;
            void *_sc = fat_read_file(&g_fat_fs, "/CONFIG/DOSSPEED.CFG", &_sz);
            if (_sc) {
                kfree(_sc);
                g_dos_speedlog = 1;
                // #dw2perf: a self-test nobody has watched go red is
                // indistinguishable from one that is not wired up (#514/#665).
                kprintf("[DOSFRAME] dosprof selftest: %d failure(s) (0 = pass)\n",
                        dosprof_selftest_rs());
                kprintf("[DOSFRAME] dosdisp selftest: %d failure(s) (0 = pass)\n",
                        dosdisp_selftest_rs());
                kprintf("[dos] #232 speed log ARMED (one line every %ums)\n",
                        DOS_SPEED_REPORT_MS);
            }
        }
        uint32_t _rz = 0;
        uint32_t _iz = 0;
        void *_ic = fat_read_file(&g_fat_fs, "/CONFIG/DOSIO.CFG", &_iz);
        if (_ic) {
            char c0 = _iz ? ((char *)_ic)[0] : '1';
            kfree(_ic);
            // (#176) An existing DOSIO.CFG holding "1", or holding anything
            // else, keeps the #175 meaning. Only an explicit "2" opts in to
            // the every-access census, which is expensive: it is a linear scan
            // of the port table on EVERY guest IN and OUT.
            g_dos_iotrace = (c0 == '2') ? 2 : 1;
            kprintf("[dos] I/O-port trace ARMED at level %d (%s)\n",
                    g_dos_iotrace,
                    g_dos_iotrace >= 2 ? "#176: EVERY access, decoded or not"
                                       : "#175: undecoded ports only");
        }
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
        // (#177) 0x3DA burst diagnostic. Arms the SAME ring DOSRING.CFG uses,
        // but without g_dos_ring_on, so the ring is dumped only at the 0x3DA
        // thresholds and not at first-console-write / exit: fewer confounds in
        // a run whose whole purpose is one loop.
        uint32_t _tz = 0;
        void *_tc = fat_read_file(&g_fat_fs, "/CONFIG/DOS3DA.CFG", &_tz);
        if (_tc) {
            uint64_t n = 0;
            for (uint32_t i = 0; i < _tz; i++) {
                char ch = ((char *)_tc)[i];
                if (ch < '0' || ch > '9') break;
                n = n * 10 + (uint64_t)(ch - '0');
            }
            kfree(_tc);
            if (!n) n = 200000;
            g_dos_3da_trip = n;
            g_dos_3da_n = 0;
            g_3da_site_n = 0;
            g_3da_site_lost = 0;
            x86_16_ring_enable(1);
            kprintf("[dos] (#177) 0x3DA burst diagnostic ARMED, first dump at "
                    "read %lu\n", (unsigned long)n);
        }
        // (#177) ARM: 0x3DA bits 0 and 3 from the emulated beam position rather
        // than from the read count. ONE build, TWO arms, differing only by this
        // file, which is the #176/#187 arm discipline.
        uint32_t _bz = 0;
        void *_bc = fat_read_file(&g_fat_fs, "/CONFIG/DOS3DAT.CFG", &_bz);
        if (_bc) {
            char c0 = _bz ? ((char *)_bc)[0] : '1';
            kfree(_bc);
            g_dos_3da_timebase = (c0 != '0');
            kprintf("[dos] (#177) 0x3DA source: %s\n",
                    g_dos_3da_timebase ? "BEAM POSITION (emulated clock)"
                                       : "legacy per-read toggle");
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
    uint64_t rate_b0 = 0;            // (#176) bus ticks at the window start
    int bus_sat_prev = 0;            // (#176) did the PREVIOUS window saturate
    unsigned long rate_acc = 0;      // insns since the last printed line
    uint64_t rate_acc_ms = 0;        // ms since the last printed line
    uint64_t run_t0 = rate_t0;       // wall clock at which this program started
    uint64_t last_present_ms = 0;    // present cadence, independent of the pacing
    uint64_t last_sample_ms  = 0;    // #dosplay redraw sampler, independent of the present
    int dbg_last_frame = -1;         // de-dup for the @frame trace below
    // #232 guest CPU speed cap. thr_cycles == DOS_CYCLES_OFF means "uncapped",
    // which is what every guest gets unless a config says otherwise, so this is
    // a no-op for every title that does not opt in.
    const char *thr_src = "default";
    uint32_t thr_cycles = dos_speed_cycles_for(path, &thr_src);
    // (#speedcap) See the note at this function: without it the per-window Speed
    // dialog's Save is refused by perms and fails silently.
    dos_speed_cfg_make_writable(path);
    // SCOPE, STATED RATHER THAN IMPLIED (#speedcap): the cap is now enforced in
    // BOTH run loops. A 16-bit guest is throttled by the entitlement block in
    // the loop below; a 32-bit DOS/4GW or go32 guest is throttled by the
    // identical block inside dos4gw_run(), which is handed this same resolved
    // value below so the launch line and the enforced cap cannot disagree. The
    // previous version forced the cap OFF here for le_active guests, which
    // excluded Red Alert and NetHack, the two heaviest titles in the catalog and
    // the two most able to starve the compositor.
    int64_t  thr_credit = 0;                         // signed instruction account
    uint64_t thr_last_ms = rate_t0;                  // when it was last paid in
    unsigned long thr_last_insns = 0;                // insn_count at that instant
    uint64_t thr_report_ms = rate_t0;                // /CONFIG/DOSSPEED.CFG report cadence
    unsigned long thr_report_i0 = 0;
    unsigned long thr_report_irq0 = 0;
    unsigned long thr_report_rd0 = 0;
    // #778: cadence for the LIVE re-read below, independent of the (usually
    // off) DOSSPEED.CFG diagnostic report above - this one runs unconditionally
    // so the compositor's Speed control works whether or not that diagnostic
    // is armed.
    uint64_t thr_live_poll_ms = rate_t0;
    if (thr_cycles) {
        kprintf("[dos] #232 CPU cap: %u cycles (%u insn/s, ~%s) from %s\n",
                thr_cycles, thr_cycles * 1000u,
                thr_cycles <= 400u   ? "PC-XT 8088 4.77MHz" :
                thr_cycles <= 1500u  ? "286-class"          :
                thr_cycles <= 8000u  ? "386-class"          : "486-class",
                thr_src);
    } else {
        kprintf("[dos] #232 CPU cap: none (uncapped, host speed) from %s\n", thr_src);
    }
    g_dos_emu_hz = 0;
    uint32_t bios_ticks = 0;
    uint16_t prev_cs = 0, prev_ip = 0, prev_cs2 = 0, prev_ip2 = 0;
    // #740: a DOS/4GW guest runs its own loop, then makes the 16-bit loop below
    // a no-op so that the ENTIRE teardown block after it runs unchanged: the
    // service context is closed and reported, the window is destroyed, the
    // pending buffers are drained, t->mem is freed and guestfs is finished by
    // dos_proc_entry(). Sharing the teardown is not a saving, it is the reason
    // a second guest type cannot acquire a second, subtly different exit path.
    if (t->le_active) {
        uint64_t budget = 0;
        {   // /CONFIG/DOS4GW.CFG, one decimal number: an instruction budget for
            // a DIAGNOSTIC run. The useful artifact of a first contact with a
            // real game is a MISS histogram over the first N instructions, and
            // that wants a bound. Absent the file the guest runs until it exits,
            // which is what a launched game should do.
            uint32_t csz = 0;
            char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/DOS4GW.CFG", &csz);
            if (cfg) {
                for (uint32_t i = 0; i < csz && cfg[i] >= '0' && cfg[i] <= '9'; i++)
                    budget = budget * 10 + (uint64_t)(cfg[i] - '0');
                kfree(cfg);
                if (budget) kprintf("[4GW] DOS4GW.CFG: instruction budget %u\n",
                                    (uint32_t)budget);
            }
        }
        dos4gw_run(t, budget, path, thr_cycles);
        if (t->le_state) { kfree(t->le_state); t->le_state = 0; }
        t->cpu.halted = 1;
    }
    while (!t->cpu.halted && t->running) {
        // Which vectors does the guest own THIS pass? Derived from the IVT, so a
        // handler installed by a direct table write counts the same as one
        // installed through INT 21h 25h.
        dos_refresh_vector_hooks(t);
        // #156: only tap/consume host keyboard+mouse input while this DOS
        // window actually holds compositor focus (see the note on
        // t->last_focused's declaration and the one above g_dos_scancode_tap's
        // initial arming). On a focus EDGE, drop whatever is already queued -
        // both the raw scancode ring (cpu/isr.c) and the guest's own BIOS
        // keyboard buffer - so keystrokes sent to another window while we were
        // unfocused do not burst into the guest the instant focus returns, and
        // clear the guest's mouse button latch so a click delivered to some
        // other window cannot read here as a held button.
        int now_focused = win16_host_is_focused(t->host_slot);
        if (now_focused != t->last_focused) {
            dos_scancode_clear();
            dos_keyq_reset(t);
            t->mbtn = 0;
            t->last_focused = now_focused;
            // #156: one line per genuine focus edge (not per loop iteration,
            // not per keystroke), so this is cheap enough to leave in always.
            // Lets a future #156-class report be settled by grepping the
            // serial log for whether the tap actually followed focus.
            kprintf("[dos] #156 host_slot=%d focus -> %s (input tap %s)\n",
                    t->host_slot, now_focused ? "GAINED" : "LOST",
                    now_focused ? "ARMED" : "DISARMED");
        }
        g_dos_scancode_tap = now_focused;
        if (now_focused) {
            // Feed INT 16h while the guest has no INT 9 handler of its own.
            // When it does have one, dos_deliver_int9() consumes the same raw
            // stream, so only one of the two ever drains it.
            // (rakbd2) EITHER route means the guest owns the raw stream.
            if (!t->kbd_has_int9 && !t->kbd_int9_pm) dos_keyq_pump(t);
            { uint64_t _pt = dosprof_t0(); dos_pump_input(t); dosprof_t1(DOSPROF_INPUT, _pt); }
            dos_mouse_deliver(t, 0);   // #163/#mickey: the 0Ch upcall, homed
            dos_deliver_int9(t);   // synthesize keyboard IRQs for the guest ISR (#202)
        }
        // TIMER INTERRUPTS COME FROM THE EMULATED CLOCK, NOT FROM THE SLICE.
        // IRQ0 fires when emulated time crosses the guest's OWN programmed PIT
        // period (divisor written to ports 0x43/0x40), so a game that asks for
        // 70 Hz gets 70 Hz whatever the host pacing is doing. id's Galaxy engine
        // (Keen 4/5/6) busy-waits on the TimeCount its INT 8 handler increments,
        // so this IS the game clock.
        {
            uint32_t div = t->pit[0].divisor ? t->pit[0].divisor : 65536u;
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
        // (#181) THE SOUND BLASTER'S END-OF-BLOCK INTERRUPT.
        //
        // Raised by the DMA pump thread when the last sample of a block has
        // actually been PLAYED, and delivered HERE because the interpreter
        // thread is the only context that may push an interrupt frame onto the
        // guest's stack. This is the same split every other synthesized IRQ in
        // this file uses (INT 8 from the emulated PIT, INT 9 from the scancode
        // ring): the SOURCE decides, the interpreter DELIVERS.
        //
        // A guest acknowledges by READING base+0xE, which clears the flag
        // inside dos_sb_read_rs. A handler that does not read it would
        // otherwise be re-entered on every pass forever, so an unacknowledged
        // interrupt is dropped after one delivery and COUNTED, which turns a
        // silent interrupt storm into a number in the exit census.
        if (dos_sb_irq_pending_rs(&t->sb) && !t->cpu.halted) {
            if (dos_vec_hooked(t, DOS_SB_IRQ_VEC)) {
                t->sb_irq_deliv++;
                dos_deliver_int(t, DOS_SB_IRQ_VEC, 20000);
            }
            if (dos_sb_irq_pending_rs(&t->sb)) {
                t->sb.irq_pending = 0;
                t->sb_irq_unacked++;
            }
        }
            // BIOS timer tick at the true 18.2065 Hz: one per 65536 PIT ticks,
            // exactly the hardware relationship, instead of one per slice.
            uint32_t bt = dos_bios_tick_now(t);   // (#234a) time of day
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
            // #232 THE SPEED CAP. Entitlement accounting, not a fixed burst +
            // fixed sleep: at any instant the guest is entitled to
            // (elapsed_ms * cycles) instructions since the epoch, and the burst
            // is whatever it is short by. That is what makes the DELIVERED rate
            // equal the TARGET rate rather than merely bounded by it: the host
            // tick is 250 Hz, so a proc_sleep(1) really sleeps ~4 ms, and a
            // fixed-burst scheme would silently deliver a quarter of what was
            // asked for. Here an over-long sleep simply earns a proportionally
            // longer next burst.
            //
            // NOT A POLL, and deliberately not a loop: there is exactly one
            // sleep and one re-read per pass. If the guest is STILL ahead after
            // the sleep it runs DOS_THROTTLE_BURST_MIN instructions anyway, and
            // that small overrun becomes debt the next pass sleeps off. Every
            // pass therefore makes real forward progress, which is the property
            // that distinguishes this from the busy-wait class CLAUDE.md bans.
            //
            // proc_sleep() rather than proc_yield(): while capped, the guest is
            // MEANT to be idle for most of every millisecond, and handing the
            // core back to the scheduler is the whole point. The uncapped path
            // below still yields, because there the guest wants every cycle.
            if (thr_cycles) {
                // A SIGNED CREDIT ACCOUNT, in instructions.
                //   +credit = the guest is owed instructions (it may run)
                //   -credit = the guest has already run ahead (it must sleep)
                // Elapsed real time PAYS IN at `cycles` instructions per ms;
                // every instruction the guest retires SPENDS, and that is read
                // straight off insn_count, so it counts the instructions a
                // synthesized ISR retired outside this burst (dos_deliver_int)
                // exactly like the ones the burst itself retired.
                //
                // THE FIRST VERSION OF THIS DROPPED THE DEBT once a second, on
                // the "resynchronise rather than accumulate" reasoning the IRQ0
                // catch-up above uses. MEASURED on VM <vmid> with Joust: that is
                // right for a missed INTERRUPT and wrong for a spent
                // INSTRUCTION. Forgiving overspend turns the cap into a
                // suggestion, and it held the guest at 0.9-1.4 M insn/s against
                // a 500 kHz target - because a 20,000-instruction INT 8
                // delivery, 60 times a second, is 1.2 M/s of demand that the
                // account must be allowed to remember in order to pay back.
                //
                // Both ends are CLAMPED, which is the part that has to be kept:
                // an idle stretch must not bank unlimited credit to spend in
                // one burst, and a pathological overspend must not park the
                // guest for a minute paying it off.
                uint64_t tnow = sched_now_ms();
                thr_credit += (int64_t)((tnow - thr_last_ms) * (uint64_t)thr_cycles);
                thr_last_ms = tnow;
                thr_credit -= (int64_t)(t->cpu.insn_count - thr_last_insns);
                thr_last_insns = t->cpu.insn_count;
                int64_t maxc = (int64_t)thr_cycles * (int64_t)DOS_THROTTLE_BURST_MS;
                int64_t maxd = -(int64_t)thr_cycles * (int64_t)DOS_THROTTLE_DEBT_MS;
                if (thr_credit > maxc) thr_credit = maxc;
                if (thr_credit < maxd) thr_credit = maxd;
                if (thr_credit <= 0) {
                    uint64_t ahead = (uint64_t)(-thr_credit) / thr_cycles + 1u;
                    if (ahead > DOS_THROTTLE_SLEEP_MAX) ahead = DOS_THROTTLE_SLEEP_MAX;
                    proc_sleep((uint32_t)ahead);
                    // ONE re-read, no loop: whatever is still owed stays in the
                    // account and the next pass sleeps again. Every pass still
                    // retires at least DOS_THROTTLE_BURST_MIN instructions of
                    // real forward progress, which is what keeps this out of the
                    // busy-wait class CLAUDE.md bans.
                    tnow = sched_now_ms();
                    thr_credit += (int64_t)((tnow - thr_last_ms) * (uint64_t)thr_cycles);
                    thr_last_ms = tnow;
                    if (thr_credit > maxc) thr_credit = maxc;
                }
                int64_t b = thr_credit > 0 ? thr_credit : 0;
                if (b > maxc) b = maxc;
                if (b < (int64_t)DOS_THROTTLE_BURST_MIN) b = (int64_t)DOS_THROTTLE_BURST_MIN;
                slice = (unsigned long)b;
            }
            uint64_t _pt = dosprof_t0();
            r = x86_16_run(&t->cpu, slice);
            dosprof_t1(DOSPROF_INTERP, _pt);
        }
        prev_cs2 = prev_cs; prev_ip2 = prev_ip;
        prev_cs = t->cpu.cs; prev_ip = t->cpu.ip;
        // PRESENT CADENCE, decoupled from the pacing. dos_present() is a full
        // 640x400 scale-and-convert blit (a divide per destination pixel); doing
        // it once per burst tied its cost to the burst size, so shortening the
        // burst would have spent the reclaimed CPU on redundant blits instead of
        // on the guest.
        //
        // "70 Hz is at or above the compositor's refresh, so nothing is lost
        // visually" IS WHAT THIS COMMENT USED TO SAY, AND IT WAS FALSE ON THE
        // MACHINE THAT MATTERS. MEASURED on golden 2259 at 2560x1600, Aladdin
        // maximised: the guest presented ~65 frames a second and the FRAMEBUFFER
        // was presented 13-16 times a second ([FLIPPROF] flips, [COMPIDLE]
        // ticks=407(13/s) busy=27%), because on ONE core (g_smp_user_sched = 0)
        // the compositor's full-screen composite costs ~20 ms and it is
        // competing with an interpreter taking 80% of the machine. Four frames
        // in five were scaled, published (a full-window memcpy, 920 MB/s) and
        // overwritten before anything composited them: 11-12% of a core in the
        // publish plus 4.4-4.8% in the scaler, thrown away. dos_frame_due()
        // below is the correction, and the sentence above is left here in the
        // negative because a confident wrong sentence is what stopped anyone
        // looking.
        {
            uint64_t pnow = sched_now_ms();
            // The guest's own frame rate, on its own clock, whether or not this
            // frame is going to be shown. Diagnostic-gated inside.
            if (pnow - last_sample_ms >= DOS_PRESENT_MS) {
                last_sample_ms = pnow;
                dos_redraw_sample(t);
            }
            if ((pnow - last_present_ms >= DOS_PRESENT_MS || t->cpu.halted) &&
                dos_frame_due(t, pnow, t->cpu.halted)) {
                last_present_ms = pnow;
                { uint64_t _pt = dosprof_t0(); dos_present(t); dosprof_t1(DOSPROF_PRESENT, _pt); }
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
                if (t->host_slot >= 0) {
                    uint64_t _pt = dosprof_t0();
                    win16_host_invalidate(t->host_slot); dos_publish_mark();
                    dosprof_t1(DOSPROF_PUBLISH, _pt);
                    if (_pt) dosprof_add_publish_bytes_rs((uint64_t)t->win_w * (uint64_t)t->win_h * 4ull);
                }
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
        //
        // #232: skipped entirely while the guest is CAPPED. The cap's own
        // proc_sleep() (just above the burst) is a strictly better handoff than
        // a yield: it parks the thread on the timer instead of re-entering the
        // ready queue, so a capped guest gives its core back rather than
        // holding it. Yielding as well would only add a scheduler round trip
        // between the burst and the sleep.
        if (!thr_cycles) { uint64_t _pt = dosprof_t0(); proc_yield(); dosprof_t1(DOSPROF_YIELD, _pt); }
        dos_prof_report(t);

        {   // CLOSE THE LOOP: measure what the guest actually got, then re-size
            // the next burst to hit DOS_SLICE_MS of wall clock at that rate.
            uint64_t now = sched_now_ms();
            // #778 LIVE SPEED CONTROL: re-run the SAME resolution
            // dos_speed_cycles_for() did at launch (SPEED.CFG in the program
            // dir, else a START.bat `cycles=` line, else /CONFIG/DOSCYCLES.CFG,
            // else uncapped) so a per-window Speed control can change a
            // running guest. Unconditional (not gated on g_dos_speedlog, unlike
            // the diagnostic report just below) because the feature must work
            // whether or not that diagnostic is armed. One small file read
            // every DOS_SPEED_LIVE_POLL_MS is not a busy-wait: this whole block
            // already runs once per already-scheduled burst, so the poll adds
            // no new wait/sleep of its own (#426).
            // (#speedcap) The !le_active test is belt and braces, not a scope
            // limit: a 32-bit guest never reaches this loop (dos4gw_run() runs
            // first and then sets t->cpu.halted), and it now carries its own
            // copy of this re-poll.
            if (!t->le_active && now - thr_live_poll_ms >= DOS_SPEED_LIVE_POLL_MS) {
                thr_live_poll_ms = now;
                const char *new_src = "default";
                uint32_t newc = dos_speed_cycles_for(path, &new_src);
                if (newc != thr_cycles) {
                    kprintf("[dos] #778 CPU cap changed live: %u -> %u cycles "
                            "(now from %s)\n", thr_cycles, newc, new_src);
                    thr_cycles = newc;
                    thr_src = new_src;
                    // Reset the entitlement account rather than let a huge swing
                    // (e.g. 486-class down to 8088) show up as either a windfall
                    // credit or a debt run up under the OLD target - the new cap
                    // starts paying in from this instant, exactly as it would at
                    // a fresh launch.
                    thr_credit = 0;
                    thr_last_ms = now;
                    thr_last_insns = t->cpu.insn_count;
                }
            }
            // #232 /CONFIG/DOSSPEED.CFG: one line every DOS_SPEED_REPORT_MS
            // saying what the guest is ACTUALLY getting against what it was
            // told to get. Same diagnostic-gate family as DOSDIAG/DOSRING/
            // DOSIO/DOSOPL/DOSBUS; absent from the golden, so off by default.
            // It exists because "is the cap working" must be a number, and
            // because the ONLY other way to get one was to close the window and
            // read the exit census.
            if (g_dos_speedlog && now - thr_report_ms >= DOS_SPEED_REPORT_MS) {
                unsigned long d = t->cpu.insn_count - thr_report_i0;
                unsigned long dq = g_dos_irq_insns - thr_report_irq0;
                unsigned long dr = g_dos_redraw_n - thr_report_rd0;
                uint64_t dt = now - thr_report_ms;
                kprintf("[dos] #232 speed: %lu insn/s (%lu cycles) target=%u cycles"
                        " isr=%lu%% redraw=%lu/s(%lu in %lums) credit=%ld\n",
                        (unsigned long)(((uint64_t)d * 1000ull) / dt),
                        (unsigned long)(((uint64_t)d) / dt),
                        thr_cycles,
                        d ? (unsigned long)(((uint64_t)dq * 100ull) / d) : 0ul,
                        (unsigned long)(((uint64_t)dr * 1000ull) / dt),
                        dr, (unsigned long)dt,
                        (long)thr_credit);
                thr_report_ms = now;
                thr_report_i0 = t->cpu.insn_count;
                thr_report_irq0 = g_dos_irq_insns;
                thr_report_rd0 = g_dos_redraw_n;
            }
            if (now - rate_t0 >= DOS_RATE_SAMPLE_MS) {
                unsigned long di = t->cpu.insn_count - rate_i0;
                uint64_t dt = now - rate_t0;
                uint32_t hz = (uint32_t)((uint64_t)di * 1000ull / dt);
                // #232: the "ignore stalled samples" floor was a bare 100 kHz,
                // which is ABOVE a legitimately capped guest (a PC-XT cap is
                // 315 cycles = 315 kHz, and anything under 100 cycles is under
                // the floor). Rejecting the sample would leave g_dos_emu_hz at
                // the old uncapped value and the guest's PIT would then run
                // ~45x fast - i.e. the cap would have broken the one thing this
                // subsystem guarantees. Scale the floor to the cap.
                uint32_t hz_floor = thr_cycles ? (thr_cycles * 1000u / 4u) : 100000u;
                if (hz > hz_floor) {         // ignore stalled samples
                    // (#176) TWO RATES, DELIBERATELY, WHERE THERE USED TO BE
                    // ONE. `hz` is raw host throughput and is what the BURST
                    // LENGTH must be sized from: how many instructions fit in
                    // DOS_SLICE_MS of wall clock is a host-pacing question and
                    // must not move when the guest's I/O mix changes.
                    // `ch` is the rate the CLOCK is derived from, which now has
                    // to account for the bus's share of the same second. With
                    // no bus charge the two are equal, by construction.
                    uint32_t ch = dos_emu_clock_rate(t, di, dt, &rate_b0, hz);
                    // Damped, because this rate is the PIT's time base: an
                    // undamped sample would make the guest's clock rate jitter
                    // with the host's scheduling noise.
                    // (#176) EXCEPT across a saturated window, which is a
                    // DISCONTINUITY, not noise: its clamped remainder yields a
                    // rate ~10x real, and blending that in carried the error
                    // for several windows AFTER the burst ended and ran the
                    // clock at 0.64x real time. Adopt whole on either side of
                    // one, and skip damping again on the first clean window.
                    uint32_t nh = (g_dos_emu_hz && !t->bus_sat_now && !bus_sat_prev)
                        ? (uint32_t)(((uint64_t)g_dos_emu_hz * 3 + ch) / 4)
                        : ch;
                    bus_sat_prev = t->bus_sat_now;
                    dos_emu_rebase(t, nh);   // adopt WITHOUT moving past instants
                    // #232: the burst size belongs to the CAP while there is
                    // one. DOS_SLICE_MIN is 20,000 instructions, which at a
                    // 500-cycle cap is 40 ms of guest time in one go: the cap
                    // would still hold on average but input and presents would
                    // be sampled 25 times a second instead of ~250, and a 60 Hz
                    // game would drop every other frame of input.
                    if (!thr_cycles) {
                        unsigned long ns =
                            (unsigned long)(((uint64_t)hz * DOS_SLICE_MS) / 1000ull);
                        if (ns < DOS_SLICE_MIN) ns = DOS_SLICE_MIN;
                        if (ns > DOS_SLICE_MAX) ns = DOS_SLICE_MAX;
                        slice = ns;
                    }
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

    // (#67/#168) THROUGH dos_emu_insns(), NOT t->cpu.insn_count.
    //
    // This line is the shared teardown, reached by BOTH engines, and it used
    // to read the 16-BIT register file unconditionally. A DOS/4GW or go32
    // guest runs dos4gw_run() and then sets t->cpu.halted to make the 16-bit
    // loop below a no-op, so t->cpu.insn_count is still zero from
    // x86_16_init(): every 32-bit guest that ever exited reported
    // 'insns=0 frames=0' however far it had actually got. MEASURED: NetHack
    // retired 661362 instructions and presented 10 frames in the same run
    // this line called 0 and 0, and tools/dos-harness/doscorpus.py PREFERS
    // this line over the '[4GW] alive' counter when a guest exits
    // (best_insns(): "exact (guest exited)"), so the wrong number was the
    // one the oracle recorded. dos_emu_insns() is the accessor that already
    // answers 'which counter' once for the whole file; this was the one
    // reader that had not been routed through it.
    kprintf("[dos] '%s' finished exit=%d insns=%lu frames=%d mode=0x%02x\n",
            path, t->cpu.exit_code, dos_emu_insns(t),
            t->le_active ? (int)t->le_frames : frames, t->video_mode);

    // (no-ticket) Keep the final frame visible for a moment, then tear down.
    // t->running is still 1 on every self-exit path (guest halted, interpreter
    // error, run cap) and 0 only on a close request, so the flag distinguishes
    // the two without a second one.
    //
    // This WAS `if (t->running) proc_sleep(2000);`, a fixed two-second delay
    // standing in for the condition "the final frame has reached the screen".
    // dos_exit_linger() waits for that condition instead, on the shared
    // wait-queue primitives, and can be cut short by the titlebar X - which
    // did nothing at all during the old sleep. See rustkern/doslinger.rs.
    //
    // The tap is disarmed BEFORE the linger now, not after. The guest is
    // already gone; there is no reason to keep mirroring every scancode into
    // a ring whose only consumer was the interpreter that has just exited.
    g_dos_scancode_tap = 0;
    dos_exit_linger(t->running ? 1 : 0);
    // #736 Stage 1b: these now clear THIS task's cpu, not a process-wide slot,
    // so tearing a DOS guest down can no longer disarm a Win16 guest's hooks
    // (or, as it did, leave the DOS guest running on the Win16 guest's).
    x86_16_set_mem_hook(&t->cpu, 0, 0, 0, 0);
    x86_16_set_int_handler(&t->cpu, 0);
    x86_16_set_io_handlers(&t->cpu, 0, 0);
    win16_host_destroy(t->host_slot);
    // (#745 local 105) FORGET THE WINDOW, in the same breath as destroying it.
    // dos_host_rebind() decides "is this resize mine?" from these two fields,
    // and user_windows[] slots are REUSED: leaving a freed buffer pointer and a
    // live-looking slot number behind would make the next app to land in this
    // slot look like our window. The guard is only exact if the state it reads
    // is cleared here.
    {   // Drop the window identity and any handover still in flight, under the
        // same lock the WM takes, so a resize racing this teardown either lands
        // before it (and is adopted by a present that still happens) or sees no
        // window at all. Anything we still owe a free for is freed here: this
        // thread is not presenting, it is tearing down.
        uint32_t *tofree[DOS_PEND_FREE_MAX]; int nfree = 0;
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_win_lock);
        for (int i = 0; i < t->pend_free_n && nfree < DOS_PEND_FREE_MAX; i++)
            tofree[nfree++] = t->pend_free[i];
        t->pend_free_n = 0; t->pend_new = 0; t->pend_buf = NULL;
        t->win_buf = NULL; t->win_w = 0; t->win_h = 0; t->host_slot = -1;
        spinlock_release_irqrestore(&g_dos_win_lock, fl);
        for (int i = 0; i < nfree; i++) kfree(tofree[i]);
    }
    // (#181) STOP AND JOIN THE DMA PUMP BEFORE THE GUEST'S MEMORY IS FREED.
    // The pump reads t->mem directly, which is the whole point of it (the
    // guest's PCM is already there), and that makes this join mandatory rather
    // than tidy: freeing the buffer under a live reader is a use-after-free
    // that would present as noise out of the speakers or as a fault inside the
    // audio stack, i.e. as a bug in someone else's subsystem.
    //
    // The wake is ours and is always armed (the pump wakes sb_wq on the way
    // out), so the timeout is a backstop. If it ever fires we deliberately
    // LEAK the 1 MiB rather than free it, and say so, because a bounded leak
    // is recoverable and a use-after-free is not.
    if (t->sb_pump_live) {
        t->sb_pump_stop = 1;
        wake_up_all(&t->sb_wq);
        int _pr = wait_event_timeout(&t->sb_wq, !t->sb_pump_live,
                                     wq_ms_to_ticks(5000));
        if (_pr != WAIT_OK)
            kprintf("[dos] (#181) SB pump still live after 5 s: leaking the "
                    "guest's 1 MiB rather than freeing it under a live reader\n");
    }
    // (#181) The census. Printed unconditionally when the card was installed,
    // because the question "did this title use the Sound Blaster, and did it
    // get audio" must be answerable from a shipped-configuration serial log
    // and not only from a diagnostic arm.
    if (t->sb.installed) {
        uint32_t used = 0;
        for (int i = 0; i < 256; i++) if (t->sb.cmd_hist[i]) used++;
        kprintf("[dos] (#181) SB census: %u resets, %u distinct DSP commands, "
                "%u unknown, %u blocks, %llu guest bytes, %u IRQs delivered "
                "(%u unacknowledged), %u sink-open failures, %u latched with no "
                "handler\n",
                t->sb.resets, used, t->sb.cmd_unknown, t->sb_blocks,
                (unsigned long long)t->sb_bytes, t->sb_irq_deliv,
                t->sb_irq_unacked, t->sb_open_fail, t->sb_irq_latched);
        // Which commands, by value. This is the measurement that answers "what
        // does the corpus actually need" instead of the guess this ticket
        // would otherwise have had to make.
        for (int i = 0; i < 256; i++)
            if (t->sb.cmd_hist[i])
                kprintf("[dos] (#181) SB cmd 0x%02X x%u\n", i, t->sb.cmd_hist[i]);
        kprintf("[dos] (#181) SB 8237: %u channel programmings, %u count reads, "
                "last rate %u Hz (TC 0x%02X), speaker %s\n",
                t->dma.n_prog, t->dma.n_count_reads, t->sb.rate,
                t->sb.time_const, t->sb.speaker ? "ON" : "off");
    }
    vbe_free_vram(t);   // (#740) guest VRAM is kmalloc'd, not BSS
    dos_mem_free(t);    // (#745) XMS/EMS arenas + their one-line usage census
    if (!t->sb_pump_live) { kfree(t->mem); t->mem = NULL; }
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
    // (#172) Take the WHOLE line; dos_launch_common() owns the
    // <path>[ <tail>] split now, so both launch paths cannot disagree about
    // what a launch line means. This used to do its own split, and the
    // syscall path did none, which is why the Start menu could not pass a
    // DOS program a single argument.
    char path[256];
    int n = 0;
    const char *p = (const char *)cfg;
    for (uint32_t i = 0; i < sz && n < (int)sizeof(path) - 1; i++) {
        char ch = p[i];
        if (ch == '\r' || ch == '\n') break;
        path[n++] = ch;
    }
    path[n] = '\0';
    // Trim trailing blanks so an empty tail stays empty.
    while (n > 0 && (path[n - 1] == ' ' || path[n - 1] == '\t')) path[--n] = '\0';
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
void dos_request_close(void) {
    g_dos.running = 0;
    // (no-ticket) AND end the post-exit linger. Clearing g_dos.running is only
    // half a close: nothing but the run loop reads that flag, so once the loop
    // had exited the X was inert and the window ignored it for the whole
    // two-second linger. Latch it where the linger can see it, then wake.
    //
    // Only g_dos_exit_wq is woken here. The frame-wait parks on g_fb_flip_wq,
    // which every present wakes, so a click that lands during that phase is
    // seen on the next present and at worst after its 250 ms backstop; the
    // WM/compositor thread has no business reaching into the framebuffer
    // layer's wait queue to shave that.
    dos_linger_close_rs();
    wake_up_all(&g_dos_exit_wq);
}

// (#745 local 105) The window manager reallocated this window's content buffer.
//
// user_window_handle_resize() (proc/syscall.c) kmallocs a buffer at the new
// size, copies what fits, and KFREES THE OLD ONE. Until this existed the DOS
// layer was never told, so t->win_buf stayed pointing at freed memory and the
// present loop wrote a megabyte of ARGB into the allocator's free list ~70
// times a second. That is not a cosmetic bug: it wedged the machine inside
// heap_acquire_lock with interrupts off, twice, reproducibly.
//
// Self-guarded on OUR slot, deliberately, rather than being installed as a
// callback the WM holds: user_windows[] slots are reused, and a stale callback
// pointer left behind by a closed DOS window would fire for whatever app landed
// in that slot next. Ownership is a question only this subsystem can answer, so
// it answers it here. This is the same shape as win16_host_rebind_canvas().
//
// Runs on the WM/compositor thread. It only publishes three fields the present
// loop reads on the next pass; the geometry is recomputed there from win_w/
// win_h, so there is no derived state to keep in step.
// Returns 1 if THIS layer has taken responsibility for freeing `old_buf`, in
// which case the caller must NOT free it. 0 means "not mine, or safe for you to
// free now" - the caller keeps its normal kfree.
int dos_host_rebind(int slot, uint32_t *buf, int w, int h, uint32_t *old_buf) {
    dos_task_t *t = &g_dos;
    int took = 0, overflow = 0;
    if (!buf || w <= 0 || h <= 0) return 0;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_win_lock);
    if (t->win_buf && slot == t->host_slot) {
        t->pend_buf = buf; t->pend_w = w; t->pend_h = h; t->pend_new = 1;
        if (old_buf && t->presenting) {
            // Mid-blit. The old buffer must outlive this present, so we own it.
            if (t->pend_free_n < DOS_PEND_FREE_MAX) {
                t->pend_free[t->pend_free_n++] = old_buf;
            } else {
                overflow = 1;   // leak it rather than free it under a live blit
            }
            took = 1;
        }
    }
    spinlock_release_irqrestore(&g_dos_win_lock, fl);
    if (overflow)
        kprintf("[dos] WARNING: %d resizes in flight, leaking one content buffer\n",
                DOS_PEND_FREE_MAX);
    return took;
}

void dos_start_deferred_launch(void) {
    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/DOSRUN.CFG", &sz);
    if (!cfg) return;
    kfree(cfg);
    proc_create("dosrun", dos_deferred_entry, NULL, PRIO_HIGH);
}

// (#67/#168) The PROGRAM half of a launch line, as a pure function.
//
// Lifted out of dos_split_launch_line() below because the ROUTING layer
// (proc/dosroute.c) has to decide where a guest runs from its program path
// alone, before any launch happens, and must not write the g_dos_* statics to
// find it out. Matching a routing rule against the whole line instead would
// make an override depend on the arguments a title happened to be launched
// with. Same rule, one definition, per #172 - which is the ticket that exists
// because two copies of this split disagreed.
//
// Returns the index into `line` just past the program half, so the caller can
// carry on parsing the tail from there without re-deriving it.
int dos_launch_program_half(const char *line, char *out, int outsz) {
    if (!out || outsz <= 0) return 0;
    out[0] = '\0';
    if (!line) return 0;
    int i = 0;
    for (; i < outsz - 1 && line[i]
           && line[i] != ' ' && line[i] != '\t'; i++)
        out[i] = line[i];
    out[i] = '\0';
    return i;
}

// (#67/#168) Is an in-kernel DOS guest running? The routing layer needs this to
// keep "one guest at a time" true ACROSS both paths: g_dos_busy is static to
// this file and guards only the in-kernel launcher, so a Ring-3 launch could
// otherwise start beside a running in-kernel guest and the two would fight over
// the raw-scancode tap and the host window.
int dos_is_busy(void) { return g_dos_busy ? 1 : 0; }

// (#172, extended #67/#168) THE <path>[ <tail>] SPLIT, in ONE function.
//
// It used to live inline in dos_launch_common(), with the comment "Splitting
// HERE rather than in each caller is deliberate: two copies of this rule is how
// the DOSRUN.CFG path and the syscall path came to disagree in the first
// place." A THIRD launch path then appeared that is not a caller of
// dos_launch_common() at all: the Ring-3 host (/APPS/DOSUSER) is already a
// process, so it has no proc_create() to sit in front of and calls
// dos_run_file() directly. It therefore did no split, and a guest launched
// down that path lost its command tail silently - the exact failure #172
// existed to end, e.g. Stunts' LOAD.EXE without `/u MCGA` exits 1 at 36,738
// instructions instead of 0 at 68,818.
//
// Lifting the rule into a function that BOTH paths call keeps one definition.
// Writes g_dos_cmdtail unconditionally, INCLUDING the empty case: it is a
// static that outlives a run, so a launch with no arguments must clear the
// previous guest's tail rather than inherit it.
static void dos_split_launch_line(const char *line) {
    int i = dos_launch_program_half(line, g_dos_path, (int)sizeof(g_dos_path));
    int j = i;
    while (line[j] == ' ' || line[j] == '\t') j++;
    int a = 0;
    for (; line[j] && a < (int)sizeof(g_dos_cmdtail) - 1; j++)
        g_dos_cmdtail[a++] = line[j];
    g_dos_cmdtail[a] = '\0';
    if (a) kprintf("[dos] command tail = '%s'\n", g_dos_cmdtail);
}

// Blocking run of a whole LAUNCH LINE, for a caller that has no
// dos_launch_common() in front of it. The Ring-3 host is the only one: it is
// handed the raw line from /CONFIG/DOSRING3.CFG and would otherwise treat a
// tail as part of the filename. Same split, same statics, same runner - so the
// in-kernel and Ring-3 paths cannot disagree about what a launch line means.
int dos_run_line(const char *line) {
    if (!line || !line[0]) return -1;
    dos_split_launch_line(line);
    if (!g_dos_path[0]) return -1;
    return dos_run_file(g_dos_path);
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
    // (#172) ONE definition of "a DOS launch line": <path>[ <command tail>].
    //
    // The DOSRUN.CFG reader already split on the first space and filled
    // g_dos_cmdtail; dos_launch() (SYS_DOS_RUN, which is how the Start menu and
    // the AI launcher start a DOS game) did not, so a menu entry could not pass
    // arguments AT ALL. That is not a cosmetic gap: Stunts' LOAD.EXE takes the
    // graphics driver as its first argument, and the game's own SETUP.EXE
    // writes the exact line into /DOS/STUNTS/SETUP.DAT, which reads
    // `load.exe /u MCGA  /ssb `. Launched bare it reaches the relocated high
    // copy, finds no driver name among ega/cga/tdy/mcga, prints "Invalid file
    // name." and exits 1. MEASURED both ways on golden b1978 + the #172 kernel:
    // bare = exit 1 at 36,738 instructions, with `mcga` = exit 0 at 68,818.
    //
    // Splitting HERE rather than in each caller is deliberate: two copies of
    // this rule is how the DOSRUN.CFG path and the syscall path came to
    // disagree in the first place. The reader now hands over the whole line.
    // Safe for every existing caller: no DOS path in the tree contains a
    // space (8.3 names on FAT and on the ext2 /DOS tree alike).
    dos_split_launch_line(path);
    g_dos_busy = 1;
    // (no-ticket) A close request and a publish mark belong to ONE guest.
    // Carrying either into the next launch would cancel its linger before it
    // began, which is the process-wide-latch bug shape #736 removed elsewhere
    // in this file.
    dos_linger_reset_rs();
    g_dos_publish_flip = 0;
    g_dos_published    = 0;
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
