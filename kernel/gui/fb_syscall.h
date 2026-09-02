// fb_syscall.h - Framebuffer syscall interface for userland compositor
// Kernel provides these syscalls for direct framebuffer access by Ring 3 compositor
#ifndef FB_SYSCALL_H
#define FB_SYSCALL_H

#include "../types.h"

// ============================================================================
// (no-ticket) "THE SCREEN HAS BEEN UPDATED" WAKE SOURCE
// ============================================================================
// sys_fb_flip() wakes this after every present, outside the interrupts-off
// window, so any subsystem that needs to know its pixels actually reached the
// glass can wait for the present counter to advance instead of sleeping for a
// guessed number of milliseconds. The first caller is the DOS guest's
// post-exit linger (dos/dosexec.c), which used to be a fixed proc_sleep(2000).
//
// Declared with a forward struct tag rather than by including sync/waitq.h:
// this header is included by userland-facing compositor code and has never
// pulled in the scheduler's headers.
struct wait_queue_head;                       /* sync/waitq.h */
extern struct wait_queue_head g_fb_flip_wq;

// ============================================================================
// Syscall Numbers for Compositor Support
// ============================================================================

// Framebuffer
#define SYS_FB_MAP          200     // Map framebuffer into user address space
#define SYS_FB_INFO         201     // Get framebuffer dimensions/info
#define SYS_FB_FLIP         202     // Flip double buffer
#define SYS_FB_DAMAGE       203     // Mark region as damaged (for partial updates)

// Input devices
#define SYS_GET_MOUSE       210
#define SYS_SET_MOUSE       211
#define SYS_GET_KEY         212
// Note: authoritative numbers are in proc/syscall.h (200-213)
#define SYS_GRAB_INPUT      213

// ============================================================================
// Framebuffer Info Structure
// ============================================================================

typedef struct {
    uint32_t width;         // Screen width in pixels
    uint32_t height;        // Screen height in pixels
    uint32_t pitch;         // Bytes per row
    uint32_t bpp;           // Bits per pixel (32 for ARGB8888)
    uint64_t phys_addr;     // Physical address of framebuffer
} fb_info_user_t;

// ============================================================================
// Keyboard Event Structure
// ============================================================================

typedef struct {
    uint32_t keycode;       // Key code
    uint32_t scancode;      // Hardware scan code
    uint32_t modifiers;     // Modifier keys (shift, ctrl, alt, etc.)
    int pressed;            // 1 = pressed, 0 = released
    uint64_t timestamp;     // Event timestamp
} key_event_t;

// ============================================================================
// Mouse Event Structure
// ============================================================================

typedef struct {
    int32_t x, y;           // Current position
    int32_t dx, dy;         // Delta since last event
    uint32_t buttons;       // Button state (bit 0 = left, 1 = right, 2 = middle)
    int32_t scroll;         // Scroll wheel delta
    uint64_t timestamp;     // Event timestamp
} mouse_event_t;

// ============================================================================
// Syscall Implementations (kernel side)
// ============================================================================

/**
 * Map the framebuffer into the calling process's address space
 * Only allowed for the compositor process (checked by capability)
 * @return      Virtual address of mapped framebuffer, or 0 on error
 */
int64_t sys_fb_map(void);

/**
 * Get framebuffer information
 * @param info  Pointer to user-space fb_info_user_t structure
 * @return      0 on success, -1 on error
 */
int64_t sys_fb_info(fb_info_user_t *info);

/**
 * Flip double buffer (if enabled)
 * @return      0 on success
 */
int64_t sys_fb_flip(void);

/**
 * (#flipfix) Read the monotonic framebuffer present counter.
 *
 * The same g_fb_flip_count the kernel's own [FLIPPROF] line and the DOS
 * frame gate read; this only hands it to Ring 3, where the DOS host cannot
 * reach a kernel variable. Read-only, no arguments, no side effects.
 *
 * @return  presents since boot, as a non-negative int64_t.
 */
int64_t sys_fb_flip_count(void);

/**
 * Mark a region of the screen as damaged (needs hardware update)
 * Used with smart update hardware
 * @param x, y, w, h    Damaged region
 * @return              0 on success
 */
int64_t sys_fb_damage(int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Get current mouse state
 * @param x, y      Output: mouse position
 * @param buttons   Output: button state
 * @return          0 on success
 */
int64_t sys_get_mouse(int32_t *x, int32_t *y, uint32_t *buttons);
int64_t sys_get_global_mouse(int32_t *x, int32_t *y, uint32_t *buttons);  // #185

/**
 * Set mouse position (cursor warp)
 * Only compositor can do this
 * @param x, y      New position
 * @return          0 on success
 */
int64_t sys_set_mouse(int32_t x, int32_t y);

/**
 * #443: Set the PHYSICAL mouse button bitmask (the same volatile mouse_buttons
 * the real PS/2 IRQ handler writes). sys_set_mouse() only ever warped the
 * cursor position; without this, remote/injected clicks (VNC) reached app
 * windows via sys_inject_mouse()'s window-manager relay but never registered
 * on the desktop's own icon/taskbar/start-menu handling, which polls the
 * physical button state directly. Only compositor may call this.
 * @param mask  Button bitmask (bit 0 = left, bit 1 = right, bit 2 = middle)
 * @return      0 on success, -1 on error
 */
int64_t sys_set_mouse_buttons(uint32_t mask);

/**
 * Get next keyboard event from queue
 * @param event     Output: key event data
 * @return          0 if event returned, -1 if queue empty
 */
int64_t sys_get_key(key_event_t *event);

// #DOSRING3 Stage 1: focus-scoped RAW set-1 scancodes for a Ring-3 DOS host.
// Requires the caller to OWN `handle` AND that window to HAVE FOCUS; both are
// re-checked on every call. Returns bytes written, 0 if not focused (the
// subscription is dropped and the ring flushed), -1 if not entitled.
// See the contract block above the definition in fb_syscall.c.
int64_t sys_win_get_scancodes(int handle, uint8_t *buf, int cap);

/**
 * Grab exclusive input access
 * Only compositor should call this
 * @param grab      1 = grab, 0 = release
 * @return          0 on success
 */
int64_t sys_grab_input(int grab);
int64_t sys_inject_mouse(int32_t x, int32_t y, int32_t type, int32_t button);

// ============================================================================
// Kernel Initialization
// ============================================================================

/**
 * Initialize framebuffer syscall support
 * Registers syscall handlers and sets up access control
 */
void fb_syscall_init(void);

#endif // FB_SYSCALL_H
