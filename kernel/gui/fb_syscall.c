// fb_syscall.c - Framebuffer syscall implementation for userland compositor
#include "fb_syscall.h"
#include "../syscall.h"
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

// PID of the compositor process (only this can access FB directly)
// Non-static so sys_compositor_render_windows() in syscall.c can check it
uint32_t compositor_pid = 0;
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
    
    // First caller to map FB becomes the compositor
    if (compositor_pid == 0) {
        compositor_pid = p->pid;
        kprintf("[FB] Process %u registered as compositor\n", p->pid);
        stage_set(STAGE_COMPOSITOR_UP, NULL);  // #418 breadcrumb
        return true;
    }
    
    return (p->pid == compositor_pid);
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
    
    info->width = g_fb_width;
    info->height = g_fb_height;
    info->pitch = g_fb_pitch;
    info->bpp = g_fb_bpp;
    info->phys_addr = g_fb_phys_addr;
    
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

int64_t sys_fb_flip(void) {
    // #307: the compositor's per-frame present is the longest single syscall
    // (net_poll + a ~4 MB back->front memcpy). A timer preemption anywhere in
    // it context-switches the compositor mid-syscall and it is resumed with a
    // corrupted register/RIP context (SYSRET target correct at ..be5 yet the
    // process resumes at ..be7 with a wild fault) so the compositor page-faults
    // on its FIRST present and the desktop never comes up. Make the whole
    // present atomic wrt preemption. Interrupts are re-enabled on exit.
    __asm__ volatile("cli");
    {
        extern void net_poll(void);
        extern volatile uint64_t timer_ticks;
        extern uint32_t g_timer_hz;
        static uint64_t s_last_net_tick = 0;
        uint64_t intv = (g_timer_hz >= 50) ? (g_timer_hz / 50) : 1;
        if (timer_ticks - s_last_net_tick >= intv) {
            s_last_net_tick = timer_ticks;
            net_poll();
        }
    }
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
        extern void bootlog_write(const char *fmt, ...);
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

int64_t sys_get_mouse(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (!is_compositor()) return -1;

    int32_t  cx = mouse_x;
    int32_t  cy = mouse_y;
    uint32_t cb = mouse_buttons;

    g_last_mouse_x       = cx;
    g_last_mouse_y       = cy;
    g_last_mouse_buttons = cb;

    if (x) *x = cx;
    if (y) *y = cy;
    if (buttons) *buttons = cb;

    return 0;
}

// Read-only global cursor for non-compositor processes (#185). Position only,
// never -1 throttling: docked panels poll this to track the OS cursor.
int64_t sys_get_global_mouse(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
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
    *event = key_queue[key_queue_tail];
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
    compositor_pid = 0;
    key_queue_head = 0;
    key_queue_tail = 0;
}
