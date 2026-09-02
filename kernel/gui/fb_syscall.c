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
#include "../fs/bootstage.h"  // the screen + flight-recorder breadcrumb trail
#include "../sync/spinlock.h"   // b740: partial-present damage accumulator
#include "../sync/waitq.h"      // g_fb_flip_wq: the "screen updated" wake source
#include "../cpu/mono.h"       // #affinity: THE monotonic clock (never timer_ticks)
#include "../cpu/inputlat.h"   // #affinity: close the input-to-present sample
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
        // BACKSTOP for the explicit UISC_NATIVE mark: whatever owns the
        // framebuffer draws in real screen pixels by definition, so it must
        // never have its coordinates scaled underneath it.
        extern void uiscale_mark_native_rs(int32_t pid);
        uiscale_mark_native_rs((int32_t)p->pid);
        kprintf("[FB] Process %u registered as compositor\n", p->pid);
        stage_set(STAGE_COMPOSITOR_UP, NULL);  // #418 breadcrumb
        // Same event, the other breadcrumb trail. fs/bootstage.h's enum is the
        // one that reaches the SCREEN and the raw flight recorder, which are the
        // two channels that survive a machine with no serial port and no
        // mounted filesystem. Wired HERE, next to its sibling, rather than left
        // as an enum value with no writer: that is exactly the shape of dead
        // instrumentation this work was written up in blame.md for.
        boot_stage(BSTAGE_COMPOSITOR);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The C glue over rustkern/fbown.rs. Declared in gui/fbown.h.
// ---------------------------------------------------------------------------

void fb_owner_arm(uint32_t pid) {
    // THE EXACT MOMENT THE COMPOSITOR'S IDENTITY IS KNOWN, AND THEREFORE THE
    // RIGHT PLACE TO MARK IT SCALE-NATIVE.
    //
    // The compositor must see REAL screen pixels while every other Ring 3
    // program sees logical ones (see rustkern/uiscale.rs). Two weaker tests
    // were tried and both are wrong in the same way - they are only true AFTER
    // something else has happened:
    //   * "is the framebuffer owner" is false until it claims, and it calls
    //     SYS_FB_INFO to learn the screen size BEFORE claiming. Measured at
    //     150%: it laid its whole desktop out for 1280x720 on a 1920x1080
    //     display and left two thirds of the screen unpainted.
    //   * "did it call UISC_NATIVE" requires the compositor to cooperate, and a
    //     capability that depends on a program remembering to ask for it is the
    //     kind of control this project has watched fail three times.
    // The kernel LAUNCHES the compositor and arms this window with the exact
    // pid it launched. Nothing is inferred and nothing has to be remembered.
    extern void uiscale_mark_native_rs(int32_t pid);
    uiscale_mark_native_rs((int32_t)pid);
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
    // Drop the scale-native mark with the same pid that took it, so a recycled
    // pid can never inherit it and start being handed physical coordinates.
    extern void uiscale_clear_native_rs(int32_t pid);
    uiscale_clear_native_rs((int32_t)pid);
    if (fbown_release_rs(pid)) {
        kprintf("[FB] compositor pid %u exited; framebuffer latch released\n",
                pid);
    }
}

// ============================================================================
// #COMPRESPAWN: THE sys_fb_map() WINDOW MUST BE UNMAPPED WHEN ITS MAPPER DIES.
//
// The full argument is at the call site in proc/process.c's proc_exit(). The
// short version: sys_fb_map() below maps the framebuffer BACK BUFFER, which is
// kmalloc_aligned() KERNEL HEAP, into a Ring-3 address space with the USER bit
// set. vmm_destroy_user_space() frees every PRESENT|USER leaf it can prove it
// owns, and it can prove it owns PML4[192] because the kernel has no such slot.
// So without this, every compositor exit donated ~8 MB of live kernel heap to
// the PMM free list.
//
// Only ONE mapping can exist at a time (is_compositor() gates sys_fb_map on the
// framebuffer ownership latch, which is single-holder), so one record is
// enough. If a second mapper ever becomes possible this must become a table -
// and the record below will make that obvious rather than silent, because the
// arm would overwrite a live entry.
// ============================================================================
static uint32_t g_fbmap_pid   = 0;
static uint64_t g_fbmap_cr3   = 0;
static uint64_t g_fbmap_vaddr = 0;
static uint64_t g_fbmap_pages = 0;

void fb_unmap_proc_exit(uint32_t pid, uint64_t cr3) {
    if (g_fbmap_pid == 0 || g_fbmap_pid != pid) return;
    // cr3 is taken from the dying process rather than the record, so a stale
    // record can never cause a write into some other address space's tables.
    // If they disagree, trust neither and drop the record.
    if (cr3 != 0 && cr3 == g_fbmap_cr3) {
        for (uint64_t i = 0; i < g_fbmap_pages; i++) {
            vmm_unmap_page_in(cr3, g_fbmap_vaddr + i * VMM_PAGE_SIZE_4K);
        }
        kprintf("[FB] pid %u exited; unmapped %lu framebuffer pages at 0x%lx "
                "(they are KERNEL HEAP, not this process's memory)\n",
                pid, g_fbmap_pages, g_fbmap_vaddr);
    } else {
        kprintf("[FB] pid %u exited with cr3 0x%lx but the fb-map record says "
                "0x%lx; NOT unmapping, dropping the record\n",
                pid, cr3, g_fbmap_cr3);
    }
    g_fbmap_pid = 0; g_fbmap_cr3 = 0; g_fbmap_vaddr = 0; g_fbmap_pages = 0;
}

// The back buffer's virtual and physical addresses.
//
// KEPT BECAUSE THE ANSWER IS NOT WHAT ANYONE ASSUMES, AND IT IS AN OPEN
// QUESTION. MEASURED on VM <vmid>, 2026-08-26: fbvirt=0x10001000,
// fbphys=0x7a7000. The kernel heap is NOT identity-mapped - mm/heap.c maps it
// at virtual 0x10000000 from whatever pages pmm_alloc_page() returned - so the
// back buffer's virtual address is not its physical address. sys_fb_map() below
// nevertheless passes fb_get_back_buffer() (a VIRTUAL address) as the PHYSICAL
// address of the pages it maps into the compositor. On the face of it that maps
// the wrong physical memory, and yet the compositor demonstrably renders, so
// one of those three facts is not what it looks like and NONE of them has been
// run to ground. Do not "fix" sys_fb_map on the strength of this comment;
// measure vmm_get_physical() against the table the kernel is actually running
// on first (mm/vmm.c's current_pml4_phys is a software shadow the scheduler
// never updates, which is the obvious suspect).
void fb_backbuffer_addrs(uint64_t *virt, uint64_t *phys) {
    uint64_t v = (uint64_t)fb_get_back_buffer();
    if (virt) *virt = v;
    if (phys) *phys = v ? vmm_get_physical(v) : 0;
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
    
    // #COMPRESPAWN: remember it so proc_exit() can take it back down. See
    // fb_unmap_proc_exit() above for why leaving it mapped corrupts the heap.
    if (g_fbmap_pid != 0 && g_fbmap_pid != p->pid) {
        kprintf("[FB] WARNING: fb-map record still held by pid %u while pid %u "
                "maps; the old mapping will not be torn down\n",
                g_fbmap_pid, p->pid);
    }
    g_fbmap_pid   = p->pid;
    g_fbmap_cr3   = p->cr3;
    g_fbmap_vaddr = vaddr;
    g_fbmap_pages = num_pages;

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
    // THE GLOBAL UI SCALE FACTOR. An app asks for the screen size to centre a
    // window or to decide which of two layouts fits, and it thinks in LOGICAL
    // pixels - the same coordinate system win_create(), win_get_size() and its
    // mouse events use. Reporting the PHYSICAL size here while scaling the
    // window it then creates puts every self-centring window off-centre, which
    // includes the first-run wizard: it reads this, computes (sw - 688)/2, and
    // would land a 1032px-wide card at x=616 instead of x=444.
    //
    // The COMPOSITOR is the exception and must see the real screen: it owns the
    // framebuffer and draws the dock and desktop at absolute physical
    // coordinates, applying the scale factor to its own chrome itself.
    extern uint32_t fbown_owner_rs(void);
    extern int32_t uiscale_unpx_rs(int32_t v);
    extern int32_t uiscale_pct_rs(void);
    extern int32_t uiscale_is_native_rs(int32_t pid);
    uint32_t rw = g_fb_width, rh = g_fb_height;
    {
        process_t *cp = proc_current();
        // NOT is_compositor(): that function CLAIMS the framebuffer as a side
        // effect, and a size query must never take the screen. NOT bare
        // ownership either: the compositor asks for the size BEFORE it claims,
        // which is precisely the call that was being answered wrongly. The
        // explicit scale-native mark is the test; ownership is the backstop.
        int is_comp = cp && (uiscale_is_native_rs((int32_t)cp->pid) ||
                             fbown_owner_rs() == cp->pid);
        if (cp && !is_comp && uiscale_pct_rs() != 100) {
            rw = (uint32_t)uiscale_unpx_rs((int32_t)g_fb_width);
            rh = (uint32_t)uiscale_unpx_rs((int32_t)g_fb_height);
        }
    }

    uaccess_ac_t __ac = uaccess_begin();
    info->width = rw;
    info->height = rh;
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

// (no-ticket) The wake that goes with the counter above. See fb_syscall.h.
// Woken AFTER the sti below, never inside the interrupts-off present window:
// the masked window is already the machine's worst latency source (#632) and
// nothing may be added to it for a waiter's convenience.
wait_queue_head_t g_fb_flip_wq = { .head = NULL, .lock = SPINLOCK_INIT };

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
// #COMPIDLE: a SECOND accumulator for the same quantity, owned by the durable
// /HEARTBEAT.TXT record. This is not duplication for its own sake - it is the
// rule this file already learned the hard way for g_flip_gap_max (see its
// comment): two consumers sharing one read-and-reset maximum means whichever
// runs first zeroes it and the second reads 0 forever. [FLIPPROF] owns
// g_flip_cli_max_cyc; the heartbeat owns this one.
volatile uint64_t g_flip_cli_max_hb_cyc = 0;
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
// 0 forever (MEASURED: gapmax=0ms in every sample on VM <vmid>, with the
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

// (#flipfix) The present counter, read-only, for a Ring-3 caller.
//
// WHY A SYSCALL AT ALL. The DOS frame gate (rustkern/dosdisp.rs) needs to know
// whether the screen has moved on since the frame it last published. In Ring 0
// dos/dosexec.c reads g_fb_flip_count directly. The Ring-3 DOS host has no such
// option: its shim's copy of the symbol was a stub that nothing could ever
// write, so the gate saw no progress on any frame and every picture came out of
// the 200 ms staleness backstop instead - a measured 5.005 flips/s against the
// in-kernel path's 24.98 on the same guest.
//
// NO SECOND COUNTER. This returns THE counter, the one incremented in
// sys_fb_flip() below and printed by [FLIPPROF]; a private count of the same
// event is the fork the reuse rule exists to prevent, and it would agree with
// this one right up until someone added a present path.
//
// Unprivileged on purpose: it is a monotonic count of screen updates, which
// leaks nothing a Ring-3 app could not obtain by watching its own window, and
// every windowed app has an equally good reason to pace itself on it.
int64_t sys_fb_flip_count(void) {
    // Plain read of a volatile uint64_t. Single writer (sys_fb_flip), 8-byte
    // aligned, so the read is atomic on x86-64 and needs no lock; a reader
    // racing the increment gets either the old or the new value and both are
    // correct answers to "how many presents had happened when you asked".
    return (int64_t)g_fb_flip_count;
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
    uint64_t _il_area = 0;   // #affinity: damaged pixels in this present

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

        // #affinity: total damaged pixel area of THIS present, so the
        // responsiveness instrument can record what kind of frame closed a
        // sample. A cursor-only present damages a few hundred pixels; a
        // keystroke echo damages at least a character cell. The instrument
        // cannot REFUSE such a close without guessing, so it records the area
        // and lets the reader see it, rather than filtering on a threshold
        // nobody has justified. 0 means a full-screen present.
        if (!lany || lfull || lcount == 0) {
            _il_area = 0;
        } else {
            for (int i = 0; i < lcount; i++)
                _il_area += (uint64_t)local[i].w * (uint64_t)local[i].h;
        }

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
        if (cli_cyc > g_flip_cli_max_hb_cyc) g_flip_cli_max_hb_cyc = cli_cyc;
        if (cpy_cyc > g_flip_cpy_max_cyc) g_flip_cpy_max_cyc = cpy_cyc;
        // 1ms threshold, in cycles, from the calibrated TSC rate (khz cycles == 1ms).
        extern uint64_t mono_tsc_khz_rs(void);
        uint64_t khz = mono_tsc_khz_rs();
        if (khz && cli_cyc > khz) g_flip_cli_over1ms++;
    }
    __asm__ volatile("sti");
    // (no-ticket) The screen now shows what the compositor just drew. One
    // spinlock acquire on an almost-always-empty queue, at most ~70 times a
    // second, and it turns "wait a while and hope it painted" into a real
    // condition for every future caller.
    wake_up_all(&g_fb_flip_wq);
    // #affinity: T2. The screen now shows what the compositor drew, so this is
    // as close to a photon as the kernel can observe. AFTER the sti, beside the
    // wake, and never inside the interrupts-off present window: #632 records
    // what happens in this exact function when a measurement is moved inside
    // that region and the instrument becomes the fault it was measuring.
    //
    // This closes the FIRST present after a key was delivered, which may not be
    // the frame that actually shows the key. See rustkern/inputlat.rs: the
    // resulting S_PRESENT is a LOWER BOUND on true input-to-photon, and it is
    // named for what it measures rather than for what it is wanted for.
    inputlat_present_rs(mono_us(), _il_area);
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

    // #197: the SAMPLED leg of the click ledger. This call sits at the point of
    // return with the value actually handed over, which is what makes the
    // counter mean "the compositor's own edge detector fired": this syscall is
    // is_compositor()-gated with a single caller (process_input() in
    // userland/apps/compositor/main.c), and that caller's test is
    // `(buttons & 1) && !(prev & 1)` over exactly this sequence of values.
    //
    // On an EDGE (rare - only a real button transition) wake the test-input
    // channel, which may be latching an injected click until it is observed.
    // No edge, no wake, so the poll-every-frame hot path costs one atomic add
    // and one compare.
    {
        extern int  clickacct_note_sample_rs(uint32_t buttons);
        extern void testinput_click_edge(int edge);
        int __edge = clickacct_note_sample_rs(cb);
        if (__edge) testinput_click_edge(__edge);
    }

    return 0;
}

// Read-only global cursor for non-compositor processes (#185). Position only,
// never -1 throttling: docked panels poll this to track the OS cursor.
//
// #aiflap (2026-08-28): THE ONE POSITION SYSCALL THAT SKIPPED THE UI-SCALE
// BOUNDARY. sys_fb_info() (above), sys_win_get_pos(), sys_win_get_size() and
// every mouse EVENT delivered to a window (kernel/proc/syscall.c's uwu()
// calls at the click/move/resize sites) all report LOGICAL coordinates to a
// non-scale-native app: physical pixels divided by the live UI scale factor.
// This function was the lone holdout, returning the RAW physical mouse_x/
// mouse_y unconditionally to every caller. At 100% scale physical==logical
// and the bug is invisible; the owner's report (AI Chat docked panel opening
// and closing with the mouse "nowhere near it", closing it freeing CPU) came
// from his real 4K panel at 200% scale, where it is not invisible.
//
// aichat's poll_dock() (userland/apps/aichat/main.c) is exactly the shape
// that breaks: it compares this call's amx/amy against g_screen_w (from
// fb_info(), LOGICAL) and against win_get_pos()/its own g_win_w/g_win_h
// (LOGICAL). At 200% scale, physical mouse_x ranges 0..~3839 while every
// other value in that comparison ranges 0..~1919 - so "on_edge" (hover the
// right dock edge) reads true across the ENTIRE RIGHT HALF of the physical
// screen instead of the ~28px sliver it is meant to be, and DOCK_PEEK's
// "outside the panel" retract check reads true almost immediately after
// opening because the panel's LOGICAL rect never contains a PHYSICAL cursor
// coordinate. 200ms dwell-open + near-instant retract-closed is a ~700ms
// self-sustaining flap for as long as the cursor sits anywhere in that
// mismatched half of the screen - matching the report exactly. musicplayer's
// drag code (get_global_mouse() mixed with win_get_pos()/win_move(), all
// assumed to be one coordinate system) has the same latent bug for window
// dragging at non-100% scale; this fix corrects it too, for free, because
// the fix is in the one shared syscall rather than a patch in aichat alone.
//
// Fix mirrors sys_fb_info()'s exact pattern: the compositor (scale-native or
// the current framebuffer owner) gets the real physical cursor, because it
// draws the cursor and owns absolute screen coordinates; every other caller
// gets the same LOGICAL value fb_info/win_get_pos/window events already give
// it, so one app never has to reconcile two coordinate systems for one cursor.
int64_t sys_get_global_mouse(int32_t *x, int32_t *y, uint32_t *buttons) {
    extern uint32_t fbown_owner_rs(void);
    extern int32_t uiscale_unpx_rs(int32_t v);
    extern int32_t uiscale_pct_rs(void);
    extern int32_t uiscale_is_native_rs(int32_t pid);

    int32_t rx = mouse_x, ry = mouse_y;
    {
        process_t *cp = proc_current();
        int is_comp = cp && (uiscale_is_native_rs((int32_t)cp->pid) ||
                             fbown_owner_rs() == cp->pid);
        if (cp && !is_comp && uiscale_pct_rs() != 100) {
            rx = uiscale_unpx_rs(mouse_x);
            ry = uiscale_unpx_rs(mouse_y);
        }
    }

    // #19/#645: three stores into Ring-3 out-params.
    uaccess_ac_t __ac = uaccess_begin();
    if (x) *x = rx;
    if (y) *y = ry;
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

// ---------------------------------------------------------------------------
// #DOSRING3 Stage 1: focus-scoped RAW SCANCODES for a Ring-3 DOS host.
//
// sys_get_key() above hands Ring 3 a cooked key_event_t and is
// is_compositor()-gated, so exactly one process can ever use it. A DOS guest
// needs something that gate cannot express: the raw set-1 MAKE and BREAK bytes
// (including the 0xE0 prefix) for its own INT 9 handler, delivered to an
// ORDINARY app rather than to the compositor.
//
// This is not a privilege increase over what Ring 3 has today. SYS_GET_KEYBOARD
// already returns keystrokes to unprivileged apps; these are the same
// keystrokes in a less processed form. The thing that must be controlled is
// SCOPE - a keylogger wants the bytes while ANOTHER window is focused - so
// delivery requires BOTH conditions, re-checked on every call and never
// latched:
//
//   1. the caller OWNS the window handle it names, and
//   2. that window currently HAS FOCUS.
//
// On losing focus the subscription is dropped and the ring flushed, so bytes
// typed into another window cannot be read out afterwards, and there is no
// teardown step a caller could skip. That is STRICTER than g_dos_scancode_tap,
// the in-kernel splice this is designed to replace, which is a global on/off
// flag with no window scoping at all.
//
// Returns the number of bytes written to `buf`, or -1 if not entitled. The
// policy and the ring live in rustkern/rawsc.rs (new kernel code is Rust).
extern void rawsc_arm_rs(uint32_t pid, int32_t handle);
extern void rawsc_disarm_rs(uint32_t pid);
extern uint32_t rawsc_drain_rs(uint8_t *out, uint32_t cap);

int64_t sys_win_get_scancodes(int handle, uint8_t *buf, int cap) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (!buf || cap <= 0) return -1;
    if (cap > 64) cap = 64;      // one PS/2 burst; bounds the kernel bounce buffer

    // Ownership and focus are BOTH properties of the window table, which lives
    // in proc/syscall.c. Reuse its accessors rather than reaching into
    // user_windows[] from here: they already handle a destroyed window and a
    // stale handle, and a second copy of that logic is how the two would drift.
    extern int  uw_caller_owns_window(int handle);
    extern int  win16_host_is_focused(int slot);

    if (!uw_caller_owns_window(handle)) {
        static uint32_t s_noown = 0;
        if (++s_noown <= 3)
            kprintf("[RAWSC-SC] REFUSED not-owner pid=%u handle=%d\n", p->pid, handle);
        return -1;
    }
    if (!win16_host_is_focused(handle)) {
        static uint32_t s_nofoc = 0;
        if (++s_nofoc <= 3)
            kprintf("[RAWSC-SC] not-focused pid=%u handle=%d (subscription dropped)\n",
                    p->pid, handle);
        // Not focused: drop the subscription so the keyboard path stops
        // buffering for us at the source, and discard anything already
        // buffered. Doing this HERE rather than on a window-manager focus hook
        // means there is no edge to miss - every path that could deliver bytes
        // passes through this check first.
        rawsc_disarm_rs(p->tgid ? p->tgid : p->pid);
        return 0;
    }

    // THE SUBSCRIPTION BELONGS TO THE PROCESS, NOT THE THREAD.
    //
    // This passed p->pid, and in this kernel a thread is a process_t of its
    // own. The Ring-3 DOS host polls from a worker thread, and it briefly had
    // TWO of them (its window is created, destroyed and recreated at a
    // corrected size, and each creation started a pump). rawsc_arm_rs() clears
    // the ring whenever the subscriber CHANGES, which is right for a genuinely
    // different subscriber and catastrophic here: threads 33 and 34 took turns
    // arming, so every 50 ms each one's arm wiped the ring, and a scancode
    // pushed between two polls was destroyed before either could drain it.
    // MEASURED as pushed climbing while drained never moved, with
    // [RAWSC-SC] showing call#1 pid=33, call#2 pid=34, call#3 pid=33.
    //
    // Keying on the thread-group makes the identity match the thing that
    // actually owns the window, and is the same correction applied to
    // uw_caller_owns_window(). Two threads of one process now share one
    // subscription instead of fighting over it.
    uint32_t owner = p->tgid ? p->tgid : p->pid;
    rawsc_arm_rs(owner, handle);

    uint8_t tmp[64];
    uint32_t n = rawsc_drain_rs(tmp, (uint32_t)cap);

    // Rate-limited truth about this call. The census says bytes were pushed and
    // not drained; only the syscall itself can say which of its own gates the
    // caller is failing, and "returns 0" is ambiguous between "not focused",
    // "armed but ring empty" and "drained nothing". Logged on the first few
    // calls and then only when it actually yields bytes, so it cannot flood.
    {
        // First three calls only. It logged on every drain too while the
        // two-pump bug was being chased, which is one serial line PER
        // KEYSTROKE - fine for an afternoon, wrong for a shipping image.
        static uint32_t s_calls = 0;
        s_calls++;
        if (s_calls <= 3) {
            kprintf("[RAWSC-SC] call#%u pid=%u handle=%d owns=1 focused=1 "
                    "drained=%u\n", s_calls, p->pid, handle, n);
        }
    }
    if (n) {
        uaccess_ac_t __ac = uaccess_begin();
        for (uint32_t i = 0; i < n; i++) buf[i] = tmp[i];
        uaccess_end(__ac);
    }
    return (int64_t)n;
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
            // #197: the ROUTED leg of the click ledger. Reaching here means the
            // compositor sampled the edge AND its whole chrome hit-test chain
            // (start menu, tray, notifications, taskbar, ...) declined to
            // consume the click, so it made it down to the window manager. Left
            // button only: the ledger tracks the left-click path.
            if (button == 1) {
                extern void clickacct_note_routed_rs(int hit);
                clickacct_note_routed_rs((int)hit);
            }
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
