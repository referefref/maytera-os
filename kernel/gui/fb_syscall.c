// fb_syscall.c - Framebuffer syscall implementation for userland compositor
#include "fb_syscall.h"
#include "../proc/syscall.h"
#include "../video/framebuffer.h"
#include "../drivers/mouse.h"
// keyboard types defined in fb_syscall.h
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../proc/process.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../string.h"
#include "../fs/panic.h"   // #418: STAGE_COMPOSITOR_UP / STAGE_DESKTOP_READY breadcrumbs
#include "../sync/spinlock.h"   // b740: partial-present damage accumulator
#include "../cpu/dlprof.h"      // #632: dp_tsc() - the shared rdtsc helper
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "fbown.h"        // #745 task #59: the framebuffer ownership latch
#include "../security/uaccess_smap.h"  // #19/#645: AC brackets for Ring-3 out-params

// Framebuffer physical address and size (from boot)
extern uint64_t g_fb_phys_addr;
extern uint32_t g_fb_width;
extern uint32_t g_fb_height;
extern uint32_t g_fb_pitch;
extern uint32_t g_fb_bpp;

// Kernel window-manager mouse handlers (gui/window.c). Declared here because
// fb_syscall.c does not include gui/window.h. window_get_at_point returns a
// window_t* but we only need a non-NULL test, so void* suffices for this TU.
extern void  wm_handle_mouse_move(int32_t x, int32_t y);
extern void  wm_handle_mouse_down(int32_t x, int32_t y, uint32_t button);
extern void  wm_handle_mouse_up(int32_t x, int32_t y, uint32_t button);
extern void *window_get_at_point(int32_t x, int32_t y);
extern void wm_inject_app_mouse(int32_t x, int32_t y, int32_t type, uint32_t button);
extern void wm_inject_app_scroll(int32_t x, int32_t y, int32_t delta);

// Keyboard event queue
#define KEY_QUEUE_SIZE 64
static key_event_t key_queue[KEY_QUEUE_SIZE];
static volatile int key_queue_head = 0;
static volatile int key_queue_tail = 0;

// WHO OWNS THE FRAMEBUFFER lives in rustkern/fbown.rs now, not in a file-scope
// `uint32_t compositor_pid` here.
//
// #745 task #59. That variable latched to the first pid ever to call
// sys_fb_map() and was cleared in exactly one place, fb_syscall_init(), which
// runs ONCE at boot. Switch User and Log Out both work by EXITING
// /APPS/COMPOSIT, so from the first Log Out of a boot the latch held a dead pid
// and every relaunch was refused here, exit(1)'d, and bounced to the login gate
// - which, under the shipped autologin=root, re-launched it immediately: an
// infinite crash-respawn loop pinning a core (measured: 59 respawns, 57 FB-map
// failures, idle 0%, in ~40 s on golden 1851).
//
// The claim is now ARMED by whoever launches the compositor (gui/desktop.c),
// CLAIMED here, and RELEASED at the proc_exit() chokepoint via
// fb_owner_proc_exit(). See gui/fbown.h and rustkern/fbown.rs.
// Set as soon as /APPS/COMPOSIT is launched (before it grabs the FB). The kernel
// desktop stops drawing once this is set, so the boot splash stays up until the
// usermode compositor's first frame (seamless handoff, no kernel-desktop flash).
int g_compositor_launched = 0;

// Set by the Win16 layer (exec/win16api.c) while a Win16 window is shown. The
// userland compositor presents a full frame to the FRONT buffer on every
// sys_fb_flip; left unchecked that wipes the kernel's directly-drawn Win16
// window every frame (the "flashing grey window" symptom). While this is set,
// sys_fb_flip still services the network but SKIPS the buffer swap, so the
// Win16 window the kernel painted to the front buffer stays on screen.
volatile int g_win16_owns_screen = 0;

// ============================================================================
// Partial-present damage accumulation (b740 - kernel half of #379 / #102 idle)
// ============================================================================
// The compositor already recomposites ONLY the changed rectangles (#379), but
// every sys_fb_flip still did a full ~4MB back->front memcpy because
// sys_fb_damage() was a no-op stub, so an idle desktop kept a core busy. Now the
// compositor reports the rects it actually redrew via sys_fb_damage() and
// sys_fb_flip() copies ONLY those rows back->front. A whole-screen rect, an
// overflow of the small fixed set, OR a flip with no damage reported all fall
// back to a full copy (safe default for the first frame, the login screen, and
// any legacy fb_flip caller). Guarded by a spinlock for SMP hygiene even though
// only the single-threaded compositor (BSP-only syscalls) ever touches it.
#define FB_DAMAGE_MAX 32
typedef struct { int32_t x, y, w, h; } fb_damage_rect_t;
static fb_damage_rect_t g_fb_damage[FB_DAMAGE_MAX];
static int        g_fb_damage_count = 0;
static bool       g_fb_damage_full  = false;
static bool       g_fb_damage_any   = false;
static spinlock_t g_fb_damage_lock  = SPINLOCK_INIT;

// ============================================================================
// Access Control
// ============================================================================

static bool is_compositor(void) {
    process_t *p = proc_current();
    if (!p) return false;

    // Fast path: this IS the owner. Every fb_flip/fb_damage/input syscall comes
    // through here many times a second, so it must be one atomic load.
    uint32_t owner = fbown_owner_rs();
    if (owner == p->pid) return true;

    if (owner != 0) {
        // BACKSTOP, not the mechanism. The release is supposed to happen at
        // proc_exit() (fb_owner_proc_exit). If a teardown path ever misses that
        // chokepoint, a dead pid must not be able to hold the screen hostage
        // until reboot - that is the exact bug this change removes, and a
        // second copy of it hiding behind a different exit path would look
        // identical. Clearing the owner does NOT arm the window, so this can
        // never hand the framebuffer to a process the kernel did not launch.
        process_t *op = proc_get(owner);
        if (!op || op->state == PROC_STATE_ZOMBIE ||
                   op->state == PROC_STATE_UNUSED) {
            fbown_note_stale_rs();
            if (fbown_release_rs(owner)) {
                kprintf("[FB] stale framebuffer owner pid %u was already gone; "
                        "latch released by the liveness backstop\n", owner);
            }
        } else {
            return false;   // somebody else owns it, and is alive
        }
    }

    if (fbown_claim_rs(p->pid) == 1) {
        kprintf("[FB] Process %u registered as compositor\n", p->pid);
        stage_set(STAGE_COMPOSITOR_UP, NULL);  // #418 breadcrumb
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The C glue over rustkern/fbown.rs. Declared in gui/fbown.h.
// ---------------------------------------------------------------------------

void fb_owner_arm(uint32_t pid) {
    if (fbown_arm_rs(pid)) {
        kprintf("[FB] framebuffer claim window armed for pid %u\n", pid);
    }
}

void fb_owner_disarm(void) {
    if (fbown_disarm_rs()) {
        kprintf("[FB] framebuffer claim window closed (no compositor)\n");
    }
}

int fb_owner_is(uint32_t pid) { return fbown_is_owner_rs(pid); }

uint32_t fb_owner_pid(void) { return fbown_owner_rs(); }

// PROCESS EXIT HOOK. Called from proc_exit() for every dying process, under
// cli(), on the dying process's own stack. One atomic compare-exchange for the
// 99.9% of processes that never owned the framebuffer; no allocation, no lock,
// no block.
void fb_owner_proc_exit(uint32_t pid) {
    if (fbown_release_rs(pid)) {
        kprintf("[FB] compositor pid %u exited; framebuffer latch released\n",
                pid);
    }
}

// Boot-time proof that the state machine's rules hold, printed so the guard is
// OBSERVED rather than assumed. Run from main.c before anything can claim.
void fbown_boot_check(void) {
    int st = fbown_selftest_rs();
    if (st != 0) {
        kprintf("[FB] fbown self-test FAILED case %d\n", st);
    } else {
        kprintf("[FB] fbown self-test OK (arm/claim/release/re-arm, 7 cases)\n");
    }
}

// ============================================================================
// Framebuffer Syscalls
// ============================================================================

int64_t sys_fb_map(void) {
    if (!is_compositor()) {
        kprintf("[FB] ERROR: Non-compositor process tried to map framebuffer\n");
        return 0;
    }
    
    process_t *p = proc_current();
    if (!p || !p->cr3) return 0;
    
    // Calculate framebuffer size
    uint64_t fb_size = (uint64_t)g_fb_height * g_fb_pitch;
    fb_size = (fb_size + 0xFFF) & ~0xFFFULL;  // Page align
    
    // Choose virtual address for mapping (in user space)
    uint64_t vaddr = 0x0000600000000000ULL;  // User-space address
    
    // Map framebuffer pages into user address space
    uint64_t num_pages = fb_size / VMM_PAGE_SIZE_4K;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_vaddr = vaddr + (i * VMM_PAGE_SIZE_4K);
        uint64_t page_paddr = (uint64_t)fb_get_back_buffer() + (i * VMM_PAGE_SIZE_4K);
        
        // Map as user read/write, write-combining for better performance
        if (vmm_map_page_in(p->cr3, page_vaddr, page_paddr, 
                           VMM_USER_RW) != 0) {
            kprintf("[FB] ERROR: Failed to map page %lu\n", i);
            // Rollback
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_page_in(p->cr3, vaddr + (j * VMM_PAGE_SIZE_4K));
            }
            return 0;
        }
    }
    
    kprintf("[FB] Mapped %lu pages (%lu KB) at 0x%lx for compositor\n",
            num_pages, fb_size / 1024, vaddr);
    
    return (int64_t)vaddr;
}

int64_t sys_fb_info(fb_info_user_t *info) {
    if (!info) return -1;
    
    // #503 / MAYTERA-SEC-2026-0016: this was an address-range test
    //
    //     if ((uint64_t)info < 0x1000 || (uint64_t)info >= 0x800000000000ULL)
    //
    // described as "basic check", and on this OS it checked nothing that
    // mattered: the kernel is identity-mapped at 0x400000, comfortably inside
    // that window, so a Ring-3 caller passing info = 0x400000 passed the test
    // and had this function write 24 bytes over kernel text.
    //
    // It is not replaced with a validate_user_ptr() call here, because the
    // DISPATCHER already validates this exact buffer at the shared choke point
    // (rustkern.rs declares SYS_FB_INFO num 201 as W/Fixed(24), and negative
    // control A-N1 proves kernel text is rejected there). sys_fb_info() has
    // exactly one caller: that dispatcher case. Re-checking here would be a
    // second, per-path copy of a check that already exists at the chokepoint,
    // which is the pattern #503 exists to remove. The NULL guard above stays
    // because NULL is deliberately SKIPPED (not rejected) by the argtab.
    
    // #19/#645: five stores into a Ring-3 struct; the bracket is the five
    // stores and nothing else.
    uaccess_ac_t __ac = uaccess_begin();
    info->width = g_fb_width;
    info->height = g_fb_height;
    info->pitch = g_fb_pitch;
    info->bpp = g_fb_bpp;
    info->phys_addr = g_fb_phys_addr;
    uaccess_end(__ac);
    
    return 0;
}

// #373 real-HW freeze diagnostic: count actual framebuffer presents. The kernel
// heartbeat (main.c) reads this so the next iMac boot log tells us WHICH way the
// desktop froze: if the flip count keeps climbing while the desktop looks stuck,
// the compositor is still looping+presenting and the real GOP display is not
// showing later frames; if the flip count is stuck at 1 while the heartbeat
// still advances, the compositor thread wedged after its first present; if BOTH
// the flip count and the heartbeat stop, the whole kernel wedged (e.g. the
// old growing bootlog write). Declared non-static so main.c can extern it.
volatile uint64_t g_fb_flip_count = 0;

// ---------------------------------------------------------------------------
// #632: the network stack's heartbeat was the compositor's frame present, with
// interrupts off.
//
// sys_fb_flip() used to call net_poll() from INSIDE its cli region. net_poll()
// takes net_lock() and drains up to 64 packets, so every single present held
// interrupts off across a packet drain whose cost scales with real inbound
// traffic. On a quiet or DOWN link that is nearly free, which is why the #546
// and #549 no-carrier fixes never covered it; on a genuinely UP, busy LAN
// (mDNS/SSDP/ARP/DHCP broadcast chatter) it is paid on EVERY frame, including
// the cursor-only partial present the compositor uses while the mouse merely
// moves over a bare desktop.
//
// The cli itself must stay: the #307 ROOT CAUSE was that the back->front copy
// runs on the KERNEL CR3 while the caller is the compositor, and no preemption
// may observe or clobber that temporary CR3. That requirement covers the CR3
// switch and the copy, and nothing else. The packet drain was simply sitting
// inside a window it never needed to be in.
//
// FLIP_NET_INSIDE_CLI=1 restores the old placement. It exists ONLY so the #632
// before/after measurement can be reproduced from this tree with byte-identical
// instrumentation on both arms; it is not a supported configuration.
#ifndef FLIP_NET_INSIDE_CLI
#define FLIP_NET_INSIDE_CLI 0
#endif

// #745 (task #62): the A/B arm for "can a failing network starve the desktop".
// FLIP_NET_BLOCKING=1 restores the PRE-FIX mechanism (the present blocks on
// net_lock via net_poll(), and the dedicated netpump thread is not started)
// while keeping every counter on the [NETSTARVE] line byte-identical, so the
// before/after comparison differs only in the mechanism under test. It is a
// measurement arm, not a supported configuration - the same role
// FLIP_NET_INSIDE_CLI plays for #632.
#ifndef FLIP_NET_BLOCKING
#define FLIP_NET_BLOCKING 0
#endif

// #632 instrumentation, read by the [FLIPPROF] line in main.c. Cycles, not
// microseconds: the conversion needs mono_tsc_khz() and is done once per
// heartbeat rather than on the hot path.
volatile uint64_t g_flip_cli_tot_cyc  = 0;  // TOTAL cycles with interrupts off
volatile uint64_t g_flip_net_tot_cyc  = 0;  // TOTAL cycles in the throttled net_poll
volatile uint64_t g_flip_cpy_tot_cyc  = 0;  // TOTAL cycles in CR3 switch + copy
volatile uint64_t g_flip_cli_max_cyc  = 0;  // longest whole interrupts-off region
volatile uint64_t g_flip_net_max_cyc  = 0;  // longest net_poll() within a present
volatile uint64_t g_flip_cpy_max_cyc  = 0;  // longest CR3-switch + back->front copy
volatile uint64_t g_flip_cli_over1ms  = 0;  // presents whose cli region exceeded 1ms
volatile uint64_t g_flip_net_calls    = 0;  // presents that actually ran net_poll

// #745 (task #62) INSTRUMENTATION. Two numbers that turn "it feels laggy" into
// a measurement, both reported on the [NETSTARVE] serial line:
//
//   g_flip_gap_max_cyc - the longest interval between two consecutive presents.
//     This IS the user's symptom. "The cursor only responds one frame in a few
//     seconds" is the claim; a gap of 3,000,000us is the evidence. It is
//     measured in sys_fb_flip itself, so it counts every cause of a stalled
//     present, not only network ones - which is what makes it useful as the
//     first question ("did the desktop actually stall?") before the second
//     ("was it the network?").
//
//   g_flip_net_skips - presents that DECLINED to pump the stack because
//     net_lock was held by another context. Before this change those presents
//     would have SPUN on that lock with interrupts off. A non-zero, growing
//     skip count on a laggy machine is the direct fingerprint of network/UI
//     contention; a zero skip count rules it out.
//
// ONE OWNER PER READ-AND-RESET MAXIMUM. There are two independent consumers of
// the present-gap maximum - the [NETSTARVE] serial line and the enriched
// /HEARTBEAT.TXT record - and they run in the SAME heartbeat loop iteration.
// Sharing one variable meant whichever ran first zeroed it and the second read
// 0 forever (MEASURED: gapmax=0ms in every sample on VM 2462, with the
// compositor genuinely presenting only 8 frames in 132s, so the true value was
// tens of seconds). Each consumer now owns its own accumulator, both updated
// at the single producer site below.
volatile uint64_t g_flip_gap_max_cyc  = 0;  // longest present gap: [NETSTARVE] owns
volatile uint64_t g_flip_gap_max_hb   = 0;  // longest present gap: /HEARTBEAT.TXT owns
volatile uint64_t g_flip_net_skips    = 0;  // pumps declined (net_lock busy)

// The throttled stack pump, factored out so both arms of FLIP_NET_INSIDE_CLI
// run identical code. Returns the cycles it consumed.
static inline uint64_t flip_net_pump(void)
{
    extern int net_poll_try(int max_pkts);
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    static uint64_t s_last_net_tick = 0;
    uint64_t intv = (g_timer_hz >= 50) ? (g_timer_hz / 50) : 1;
    if (timer_ticks - s_last_net_tick < intv) return 0;
    s_last_net_tick = timer_ticks;
    uint64_t t0 = dp_tsc();
    // #745 (task #62): NON-BLOCKING. The compositor's present must never wait
    // on net_lock - net_lock() does `cli` before it spins, so waiting here is
    // an unpreemptible stall on the one syscall that puts pixels on screen, and
    // its worst case is the worst case of every network context in the kernel.
    // If the lock is busy we skip: the dedicated netpump thread is the
    // always-armed service source, so the cost of skipping is bounded by one
    // pump interval. The drain bound is 16 rather than net_poll()'s 64 so the
    // present's own worst case is bounded by construction too.
#if FLIP_NET_BLOCKING
    { extern void net_poll(void); net_poll(); }   // PRE-FIX arm: blocks on net_lock
#else
    if (!net_poll_try(16)) g_flip_net_skips++;
#endif
    uint64_t d = dp_tsc() - t0;
    g_flip_net_calls++;
    g_flip_net_tot_cyc += d;
    if (d > g_flip_net_max_cyc) g_flip_net_max_cyc = d;
    return d;
}

int64_t sys_fb_flip(void) {
    // #307: the compositor's per-frame present is the longest single syscall
    // (net_poll + a ~4 MB back->front memcpy). A timer preemption anywhere in
    // it context-switches the compositor mid-syscall and it is resumed with a
    // corrupted register/RIP context (SYSRET target correct at ..be5 yet the
    // process resumes at ..be7 with a wild fault) so the compositor page-faults
    // on its FIRST present and the desktop never comes up. Make the whole
    // present atomic wrt preemption. Interrupts are re-enabled on exit.
    //
    // #632 MEASUREMENT: how long is that interrupts-off window really, and how
    // much of it is the network drain versus the back->front copy? Reported
    // unconditionally on the [FLIPPROF] line next to [HB] (main.c), because a
    // counter nobody reads is not a measurement (#621).
    uint64_t _t_enter = dp_tsc();
    uint64_t _t_net = 0;

    // #745 (task #62): measure the present-to-present gap. See the comment on
    // g_flip_gap_max_cyc. First call has no predecessor and is skipped.
    {
        static uint64_t s_prev_enter = 0;
        if (s_prev_enter) {
            uint64_t gap = _t_enter - s_prev_enter;
            if (gap > g_flip_gap_max_cyc) g_flip_gap_max_cyc = gap;
            if (gap > g_flip_gap_max_hb)  g_flip_gap_max_hb  = gap;
        }
        s_prev_enter = _t_enter;
    }

    // #632: service the TCP/IP stack BEFORE the interrupts-off region, at the
    // SAME 20ms cadence as before, so the stack is pumped exactly as often but
    // no longer with interrupts masked. See the comment above flip_net_pump().
#if !FLIP_NET_INSIDE_CLI
    _t_net = flip_net_pump();
#endif

    __asm__ volatile("cli");
#if FLIP_NET_INSIDE_CLI
    _t_net = flip_net_pump();
#endif
    uint64_t _t_cpy0 = dp_tsc();
    {
        // #307 ROOT CAUSE FIX: the front buffer is the physical framebuffer at
        // its identity-mapped address (QEMU std-VGA: 0x80000000). In the
        // compositor's CR3 that virtual address is the compositor's OWN user
        // image (user.ld base = 0x80000000), so doing the back->front memcpy in
        // the caller's (compositor) address space overwrote the compositor's
        // code with pixel data and it derailed on syscall return (the desktop
        // never came up - kernel splash stayed). Switch to the kernel identity
        // map (g_kernel_cr3) for the copy so 0x80000000 is the real hardware
        // framebuffer, then restore. Interrupts are already disabled above, so
        // no preemption can observe/clobber the temporary CR3.
        extern uint64_t g_kernel_cr3;
        extern void vmm_switch_pml4(uint64_t);

        // b740: snapshot + clear the damage set before switching CR3, so the
        // present copies only the rectangles the compositor redrew. Snapshotting
        // into locals keeps the (brief) lock off the actual memcpy.
        fb_damage_rect_t local[FB_DAMAGE_MAX];
        int  lcount; bool lfull, lany;
        spinlock_acquire(&g_fb_damage_lock);
        lany   = g_fb_damage_any;
        lfull  = g_fb_damage_full;
        lcount = g_fb_damage_count;
        for (int i = 0; i < lcount; i++) local[i] = g_fb_damage[i];
        g_fb_damage_any   = false;
        g_fb_damage_full  = false;
        g_fb_damage_count = 0;
        spinlock_release(&g_fb_damage_lock);

        uint64_t saved_cr3 = read_cr3();
        if (g_kernel_cr3) vmm_switch_pml4(g_kernel_cr3);
        if (!lany || lfull || lcount == 0) {
            fb_swap_buffers();                                    // full present
        } else {
            fb_swap_dirty_rects(local, (uint32_t)lcount, false);  // partial present
        }
        vmm_switch_pml4(saved_cr3);
    }

    // #373 real-HW freeze diagnostic. Count this present, and on the VERY FIRST
    // one log the REAL GOP framebuffer geometry to the durable log - the #307
    // present path copies the back buffer to g_fb_phys_addr assuming the QEMU
    // std-VGA base 0x80000000, but the iMac's UEFI GOP framebuffer lives at a
    // different physical base/stride; if it is not identity-mapped the same way,
    // the first present can succeed (splash cleared) yet a later flip wedge.
    // Logging the real base/size/stride once (cheap, one write) lets the next
    // iMac boot confirm or rule that out. Per-flip logging is deliberately NOT
    // done - that would reintroduce a growing/expensive write.
    {
        extern volatile uint64_t g_fb_flip_count;
        uint64_t c = g_fb_flip_count++;
        if (c == 0) {
            uint32_t sz = g_fb_height * g_fb_pitch;
            bootlog_write("[FB] first present: GOP base=0x%lx size=%u stride=%u "
                          "%ux%u bpp=%u assumed_base=0x80000000 %s",
                          (unsigned long)g_fb_phys_addr, (unsigned)sz,
                          (unsigned)g_fb_pitch, (unsigned)g_fb_width,
                          (unsigned)g_fb_height, (unsigned)g_fb_bpp,
                          (g_fb_phys_addr == 0x80000000ULL) ? "(matches)"
                                                            : "(DIFFERS!)");
            // #418 breadcrumb: the compositor's first successful present is
            // as close to "desktop ready" as the kernel can observe.
            stage_set(STAGE_DESKTOP_READY, NULL);
        }
    }
    // #632: close the measurement of the interrupts-off region BEFORE re-enabling
    // interrupts, so the number is the real masked window and nothing else.
    {
        uint64_t now = dp_tsc();
        uint64_t cli_cyc = now - _t_enter;
#if !FLIP_NET_INSIDE_CLI
        // net_poll ran before the cli, so it is not part of the masked window.
        cli_cyc -= (_t_net < cli_cyc) ? _t_net : 0;
#else
        (void)_t_net;   // measured inside the region; already counted
#endif
        uint64_t cpy_cyc = now - _t_cpy0;
        g_flip_cli_tot_cyc += cli_cyc;
        g_flip_cpy_tot_cyc += cpy_cyc;
        if (cli_cyc > g_flip_cli_max_cyc) g_flip_cli_max_cyc = cli_cyc;
        if (cpy_cyc > g_flip_cpy_max_cyc) g_flip_cpy_max_cyc = cpy_cyc;
        // 1ms threshold, in cycles, from the calibrated TSC rate (khz cycles == 1ms).
        extern uint64_t mono_tsc_khz_rs(void);
        uint64_t khz = mono_tsc_khz_rs();
        if (khz && cli_cyc > khz) g_flip_cli_over1ms++;
    }
    __asm__ volatile("sti");
    return 0;
}

int64_t sys_fb_damage(int32_t x, int32_t y, int32_t w, int32_t h) {
    // b740: accumulate the damaged rectangle for the next sys_fb_flip so it only
    // copies these rows back->front instead of the whole framebuffer.
    if (w <= 0 || h <= 0) return 0;

    // Clamp to screen bounds (drop fully off-screen rects).
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)g_fb_width || y >= (int32_t)g_fb_height) return 0;
    if (x + w > (int32_t)g_fb_width)  w = (int32_t)g_fb_width  - x;
    if (y + h > (int32_t)g_fb_height) h = (int32_t)g_fb_height - y;
    if (w <= 0 || h <= 0) return 0;

    spinlock_acquire(&g_fb_damage_lock);
    g_fb_damage_any = true;
    if (!g_fb_damage_full) {
        // A whole-screen rect, or overflow of the small set, collapses to a full
        // present (cheaper than tracking dozens of rects, and always correct).
        bool whole = (x == 0 && y == 0 &&
                      w >= (int32_t)g_fb_width && h >= (int32_t)g_fb_height);
        if (whole || g_fb_damage_count >= FB_DAMAGE_MAX) {
            g_fb_damage_full  = true;
            g_fb_damage_count = 0;
        } else {
            g_fb_damage[g_fb_damage_count].x = x;
            g_fb_damage[g_fb_damage_count].y = y;
            g_fb_damage[g_fb_damage_count].w = w;
            g_fb_damage[g_fb_damage_count].h = h;
            g_fb_damage_count++;
        }
    }
    spinlock_release(&g_fb_damage_lock);
    return 0;
}

// ============================================================================
// Input Syscalls
// ============================================================================

// Mouse state from driver
extern volatile int32_t mouse_x;
extern volatile int32_t mouse_y;
extern volatile uint8_t mouse_buttons;

// Track last-reported mouse state so sys_get_mouse returns -1 when nothing changed.
// This prevents compositor drain loops from spinning at maximum syscall rate.
static int32_t  g_last_mouse_x = 0;
static int32_t  g_last_mouse_y = 0;
static uint32_t g_last_mouse_buttons = 0xFFFFFFFF;  // impossible initial value

// #334: count of compositor SYS_GET_MOUSE polls, so the serial test-input
// channel can confirm the compositor actually SAMPLED an injected click.
volatile uint64_t g_mouse_poll_count = 0;

int64_t sys_get_mouse(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (!is_compositor()) return -1;
    g_mouse_poll_count++;   // #334: compositor sampled the (injected) cursor

    int32_t  cx = mouse_x;
    int32_t  cy = mouse_y;
    uint32_t cb = mouse_buttons;

    g_last_mouse_x       = cx;
    g_last_mouse_y       = cy;
    g_last_mouse_buttons = cb;

    // #19/#645: THE hottest user write in the kernel - the compositor polls
    // this every frame - so the window is three stores, opened once.
    uaccess_ac_t __ac = uaccess_begin();
    if (x) *x = cx;
    if (y) *y = cy;
    if (buttons) *buttons = cb;
    uaccess_end(__ac);

    return 0;
}

// Read-only global cursor for non-compositor processes (#185). Position only,
// never -1 throttling: docked panels poll this to track the OS cursor.
int64_t sys_get_global_mouse(int32_t *x, int32_t *y, uint32_t *buttons) {
    // #19/#645: three stores into Ring-3 out-params.
    uaccess_ac_t __ac = uaccess_begin();
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    uaccess_end(__ac);
    return 0;
}

int64_t sys_set_mouse(int32_t x, int32_t y) {
    if (!is_compositor()) return -1;
    
    // Clamp to screen bounds
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int32_t)g_fb_width) x = g_fb_width - 1;
    if (y >= (int32_t)g_fb_height) y = g_fb_height - 1;
    
    mouse_x = x;
    mouse_y = y;

    return 0;
}

// #443: set the PHYSICAL button state (mirrors sys_set_mouse's cursor warp).
// sys_inject_mouse() below already relays button transitions into the kernel
// window manager for app windows, but the desktop's own icon/taskbar/start-menu
// click handling reads the `mouse_buttons` global directly (the same one the
// real PS/2 IRQ path writes), not the window-manager relay. Without this, an
// injected remote click could select/drag app windows but never registered as
// a click on the desktop itself. Gated the same way as sys_set_mouse.
int64_t sys_set_mouse_buttons(uint32_t mask) {
    if (!is_compositor()) return -1;

    mouse_buttons = (uint8_t)mask;

    return 0;
}

int64_t sys_get_key(key_event_t *event) {
    if (!is_compositor()) return -1;
    if (!event) return -1;
    
    // Check if queue empty
    if (key_queue_head == key_queue_tail) {
        return -1;  // No events
    }
    
    // Dequeue event
    // #19/#645: one struct store into a Ring-3 buffer.
    {   uaccess_ac_t __ac = uaccess_begin();
        *event = key_queue[key_queue_tail];
        uaccess_end(__ac); }
    key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    
    return 0;
}

int64_t sys_grab_input(int grab) {
    if (!is_compositor()) return -1;

    extern void wm_enter_exclusive_mode(void);
    extern void wm_exit_exclusive_mode(void);
    extern void fb_set_direct_mode(bool);

    if (grab) {
        wm_enter_exclusive_mode();
        // Keep fb_addr = fb_back for double buffering; compositor maps back buffer
    } else {
        // fb_addr stays as fb_back (double buffering always active)
        wm_exit_exclusive_mode();
    }

    return 0;
}

// Forward a mouse event from the userland compositor into the kernel window
// manager. Under exclusive (compositor) mode the kernel desktop loop no longer
// processes input, so the compositor must relay mouse activity here for window
// dragging, the title-bar buttons (minimize/maximize/close), resize grips, and
// click-to-focus to work. The actual logic lives in the existing kernel WM
// handlers; this is a thin gated relay.
//   type:   0 = move, 1 = button down, 2 = button up
//   button: hardware button mask (bit 0 = left)
// Returns 1 if a DOWN event landed on a window (so the compositor can suppress
// its own desktop-icon / right-click handling for that click); 0 otherwise.
int64_t sys_inject_mouse(int32_t x, int32_t y, int32_t type, int32_t button) {
    if (!is_compositor()) return -1;

    int64_t hit = 0;
    switch (type) {
        case 0:  // move
            wm_handle_mouse_move(x, y);
            wm_inject_app_mouse(x, y, 0, (uint32_t)button);
            break;
        case 1:  // button down
            if (window_get_at_point(x, y)) hit = 1;
            // Right button (2) only routes a content event to the app under the
            // cursor (for its own context menu); it must NOT drive window chrome
            // (focus/drag/resize/min/max/close all belong to the left button),
            // otherwise a right-press could start a drag with no matching up.
            if (button != 2) wm_handle_mouse_down(x, y, (uint32_t)button);
            wm_inject_app_mouse(x, y, 1, (uint32_t)button);
            break;
        case 2:  // button up
            wm_handle_mouse_up(x, y, (uint32_t)button);
            wm_inject_app_mouse(x, y, 2, (uint32_t)button);
            break;
        case 3:  // scroll wheel (button carries the signed delta)
            wm_inject_app_scroll(x, y, button);
            break;
        default:
            return -1;
    }
    return hit;
}

// (#745) Publish the desktop work area (the screen minus whatever the active
// dock style reserves at each edge). The COMPOSITOR owns the dock style, so it
// derives the four insets from that style's own edge and thickness and pushes
// them here; the kernel window manager then uses one definition for initial
// placement, maximize, restore and the title-bar drag. Compositor-only, gated
// exactly like sys_inject_mouse: a random Ring-3 app must not be able to
// reserve the whole screen and strand every window.
int64_t sys_wm_set_work_area(int32_t left, int32_t top, int32_t right, int32_t bottom) {
    if (!is_compositor()) return -1;
    extern void wm_set_work_area(int32_t, int32_t, int32_t, int32_t);
    wm_set_work_area(left, top, right, bottom);
    return 0;
}

// ============================================================================
// Keyboard Event Queue (called by keyboard driver)
// ============================================================================

void fb_queue_key_event(uint32_t keycode, uint32_t scancode, 
                        uint32_t modifiers, int pressed) {
    // Calculate next position
    int next_head = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    
    // Check for overflow
    if (next_head == key_queue_tail) {
        // Queue full, drop oldest event
        key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    }
    
    // Enqueue event
    key_queue[key_queue_head].keycode = keycode;
    key_queue[key_queue_head].scancode = scancode;
    key_queue[key_queue_head].modifiers = modifiers;
    key_queue[key_queue_head].pressed = pressed;
    key_queue[key_queue_head].timestamp = 0;  // TODO: Add timestamp
    
    key_queue_head = next_head;
}

// ============================================================================
// Initialization
// ============================================================================

void fb_syscall_init(void) {
    kprintf("[FB] Framebuffer syscall support initialized\n");
    kprintf("[FB] Screen: %ux%u, %u bpp, pitch=%u\n",
            g_fb_width, g_fb_height, g_fb_bpp, g_fb_pitch);
    
    // Register syscall handlers (done in main syscall dispatcher)
    fbown_reset_rs();   // #745 task #59: cold state, nothing armed, no owner
    key_queue_head = 0;
    key_queue_tail = 0;
}
