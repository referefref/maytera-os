// window.h - Window structures and API for MayteraOS GUI
#ifndef WINDOW_H
#define WINDOW_H

#include "../types.h"
#include "themes.h"

// Forward declarations
struct widget;
struct window;

// Maximum limits
#define MAX_WINDOWS         32
#define MAX_WINDOW_TITLE    64
#define MAX_WIDGETS         64
#define MAX_APPS            16
#define EVENT_QUEUE_SIZE    64
#define MAX_DIRTY_RECTS     16

// Window flags
#define WINDOW_FLAG_VISIBLE     (1 << 0)
#define WINDOW_FLAG_FOCUSED     (1 << 1)
#define WINDOW_FLAG_DRAGGING    (1 << 2)
#define WINDOW_FLAG_MOVABLE     (1 << 3)
#define WINDOW_FLAG_CLOSABLE    (1 << 4)
#define WINDOW_FLAG_RESIZABLE   (1 << 5)
#define WINDOW_FLAG_RESIZING    (1 << 6)
#define WINDOW_FLAG_MINIMIZED   (1 << 7)
#define WINDOW_FLAG_MAXIMIZED   (1 << 8)
#define WINDOW_FLAG_NOCHROME    (1 << 9)  // #185: borderless panel (no titlebar/border/buttons)
// #745: this window asks the userland compositor to paint a soft drop shadow in
// the desktop pixels immediately OUTSIDE its rectangle. OPT-IN, and it has to
// be: #189 removed the blanket app-window drop shadow by explicit user
// decision, so "every window" is a setting the user already said no to. The
// flag is orthogonal to NOCHROME (the first caller, the OOBE wizard, is a
// NOCHROME panel; a chromed window may set it too) and is set once via
// SYS_WIN_SET_SHADOW. The kernel does NOT draw the shadow: it owns no pixel
// outside a window rect. All it does is carry this bit out to the compositor
// through sys_wm_get_windows(), which is the layer that composited those
// pixels in the first place.
#define WINDOW_FLAG_SHADOW      (1 << 10)

// Default window flags
#define WINDOW_FLAGS_DEFAULT (WINDOW_FLAG_VISIBLE | WINDOW_FLAG_MOVABLE | WINDOW_FLAG_CLOSABLE)

// ===========================================================================
// Title bar / chrome dimensions (#711: DATA, not compiled-in constants)
//
// These were five literal #defines, which is why changing a titlebar height
// meant rebuilding and redeploying a kernel. They now read metric.* from the
// ACTIVE .mtheme file through the same live theme table SYS_THEME_COLOR reads,
// so editing the file changes the frame on the next redraw with no build.
//
// They are deliberately still MACROS with the same names: every existing call
// site in window.c and the kernel fallback apps becomes data-driven with no
// edit, so there is no way to miss one and leave a silently-hardcoded frame
// (the exact failure mode decor_palette() demonstrated). They are no longer
// integer constant expressions, so they cannot be used as an array bound or a
// case label; nothing in the tree did.
//
// The _FALLBACK values are the pre-#711 compiled constants and are what a
// theme file that says nothing about geometry still gets, so an unported
// .mtheme keeps its current metrics exactly.
// ===========================================================================
#define TITLEBAR_HEIGHT_FALLBACK    20
#define CLOSE_BUTTON_SIZE_FALLBACK  16
#define BORDER_WIDTH_FALLBACK       2
#define RESIZE_GRIP_SIZE_FALLBACK   10
#define TITLEBAR_BUTTON_SPACING_FALLBACK 2

static inline int32_t win_metric_or(int metric_id, int32_t fallback) {
    int32_t v = theme_get_metric_by_id(-1, metric_id);
    return (v > 0) ? v : fallback;
}

#define TITLEBAR_HEIGHT     win_metric_or(TM_TITLEBAR_H,   TITLEBAR_HEIGHT_FALLBACK)
#define CLOSE_BUTTON_SIZE   win_metric_or(TM_TITLEBAR_BTN, CLOSE_BUTTON_SIZE_FALLBACK)
#define BORDER_WIDTH        win_metric_or(TM_BORDER_W,     BORDER_WIDTH_FALLBACK)
#define RESIZE_GRIP_SIZE    win_metric_or(TM_GRIP,         RESIZE_GRIP_SIZE_FALLBACK)

// Minimum window size constraints
#define WINDOW_MIN_WIDTH    100
#define WINDOW_MIN_HEIGHT   50

// ===========================================================================
// (#745) WORK AREA - the part of the screen no desktop panel covers.
//
// The shell panel is NOT always at the bottom. The XFCE dock style (#26) puts
// its panel at the TOP of the screen, and Lumina / Retro Bench put a menu bar
// there. The compositor renders the taskbar AFTER the kernel has painted app
// windows, so a window whose top edge sits at y=0 under one of those styles
// has its ENTIRE title bar painted over: it cannot be moved, focused or
// closed by its header. That was reachable from four separate paths (initial
// placement, maximize, a title-bar drag, and SYS_WIN_MOVE), each with its own
// idea of "the screen".
//
// The strut is deliberately NOT computed here, and NOT hardcoded to "top".
// The compositor is the only component that knows the active dock style, so
// it DERIVES the four insets from that style's own edge and thickness
// (taskbar_{top,bottom,left,right}_inset()) and pushes them down through
// SYS_WM_SET_WORK_AREA. This file consumes four numbers. A future left- or
// right-edge dock therefore needs no kernel change at all, and a bottom-panel
// style keeps reserving the BOTTOM, which a hardcoded top strut would have
// silently broken.
//
// ONE definition, shared by the placement path AND the hit-test/drag path: a
// placement rule the drag path does not also obey is not a rule.
// ===========================================================================

// Minimum px of a window that must stay inside the work area horizontally, so
// the title bar always has a grabbable sliver. Was an unnamed literal 48 in
// sys_win_move()'s private clamp; now named and shared.
#define WM_WORK_AREA_MIN_VISIBLE 48

// Window colors - Modern grey/white theme
#define COLOR_TITLEBAR_ACTIVE       0x003A3A3A  // Dark grey
#define COLOR_TITLEBAR_INACTIVE     0x002D2D2D  // Darker grey
#define COLOR_TITLEBAR_TEXT         0x00E0E0E0  // Light grey/white
#define COLOR_WINDOW_BG             0x00F5F5F5  // Very light grey (almost white)
#define COLOR_WINDOW_BORDER         0x00505050  // Medium dark grey
#define COLOR_CLOSE_BUTTON          0x00606060  // Grey close button
#define COLOR_CLOSE_BUTTON_HOVER    0x00E04040  // Subtle red on hover only
#define COLOR_MINIMIZE_BUTTON       0x00707070  // Grey
#define COLOR_MAXIMIZE_BUTTON       0x00707070  // Grey

// Title bar button spacing (#711: metric.titlebar_btn_gap)
#define TITLEBAR_BUTTON_SPACING \
    win_metric_or(TM_TITLEBAR_BTN_GAP, TITLEBAR_BUTTON_SPACING_FALLBACK)

// Vertical offset of the titlebar control buttons inside the frame. Pre-#711
// this was a literal "+ 2" repeated at five call sites, which pinned the
// buttons to the top of a 20px titlebar; with a themeable titlebar height they
// have to be centred in whatever height the file asks for.
#define TITLEBAR_BUTTON_Y_OFFSET \
    (BORDER_WIDTH + (TITLEBAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2)

// Event types
typedef enum {
    EVENT_NONE = 0,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_SCROLL,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_WINDOW_CLOSE,
    EVENT_WINDOW_FOCUS,
    EVENT_WINDOW_BLUR,
    EVENT_BUTTON_CLICK,
    EVENT_REDRAW,
    EVENT_RESIZE
} event_type_t;

// Mouse button flags
#define MOUSE_BUTTON_LEFT   (1 << 0)
#define MOUSE_BUTTON_RIGHT  (1 << 1)
#define MOUSE_BUTTON_MIDDLE (1 << 2)

// Event structure
typedef struct {
    event_type_t type;
    uint32_t target_id;     // Window or widget ID
    int32_t mouse_x;        // Mouse X position (screen coords)
    int32_t mouse_y;        // Mouse Y position (screen coords)
    uint32_t mouse_buttons; // Mouse button state
    int8_t scroll_delta;    // Scroll wheel delta (positive=up, negative=down)
    uint32_t keycode;       // Keyboard keycode
    char key_char;          // Printable character (if any)
} gui_event_t;

// Event handler function type
typedef void (*event_handler_t)(struct window *win, gui_event_t *event);

// ============================================================================
// Window Manager Event Queue and App Registration
// ============================================================================

// Event queue for centralized event handling
typedef struct {
    gui_event_t events[EVENT_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} event_queue_t;

// App callback function types
typedef void (*app_event_handler_t)(void *app_data, gui_event_t *event);
typedef void (*app_draw_handler_t)(void *app_data);
typedef void (*app_destroy_handler_t)(void *app_data);

// Rectangle structure (must be defined before dirty_region_t)
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} rect_t;

// App registration structure
typedef struct {
    struct window *window;          // Associated window
    void *app_data;                 // Application-specific data
    app_event_handler_t on_event;   // Event callback
    app_draw_handler_t on_draw;     // Draw callback
    app_destroy_handler_t on_destroy; // Destroy callback
    bool active;                    // Is this registration active
} app_registration_t;

// Dirty region tracking for efficient redraws
typedef struct {
    rect_t rects[MAX_DIRTY_RECTS];
    uint32_t count;
    bool full_redraw;               // If true, redraw entire screen
} dirty_region_t;

// Window structure
typedef struct window {
    uint32_t id;                        // Unique window ID
    char title[MAX_WINDOW_TITLE];       // Window title
    rect_t bounds;                      // Position and size (including title bar)
    uint32_t flags;                     // Window flags
    uint32_t z_order;                   // Z-ordering (higher = on top)

    // Colors (customizable per window)
    uint32_t bg_color;                  // Background color
    uint32_t border_color;              // Border color
    uint32_t titlebar_color;            // Title bar color

    // Widgets
    struct widget *widgets;             // Linked list of widgets
    uint32_t widget_count;              // Number of widgets

    // Event handling
    event_handler_t on_close;           // Close handler
    event_handler_t on_click;           // Click handler
    event_handler_t on_key;             // Key handler
    void *user_data;                    // User-defined data

    // Drag state
    int32_t drag_offset_x;
    int32_t drag_offset_y;

    // Resize state
    uint32_t resize_edge;           // Which edge(s) are being resized

    // Stored bounds for restore from maximize
    rect_t stored_bounds;           // Original bounds before maximize

    // Transparency (0-255, 255=opaque) + lock-in-place
    uint8_t opacity;
    uint8_t locked;
    // Per-window decorator state (Task A redesign).
    uint8_t theme_override;   // 0 = follow system, 1 = force dark, 2 = force light
    // #711 loop 2 (designer 1): which titlebar button the cursor is currently
    // over. 0 none, 1 filter/cog, 2 minimize, 3 maximize, 4 close. Set by
    // wm_handle_mouse_move, read by window_draw's button hover fills.
    uint8_t tb_hover_btn;
    uint8_t scale_pct;        // window content scale percent (50..200, default 100)
    int32_t scale_base_w;     // natural width captured for scale baseline (0 = uncaptured)
    int32_t scale_base_h;     // natural height captured for scale baseline

    // #41 task (dock duplicate-icon fix): the process that created this
    // window via window_create(), i.e. proc_current()->pid at creation time.
    // 0 means "no Ring-3 owner" (a kernel-desktop-fallback window, or created
    // from a context with no current process) - NOT pid 0 (idle); callers
    // that resolve this to a process MUST special-case 0 as "no identity"
    // rather than calling proc_get(0), which would resolve to idle and hand
    // out a bogus app_id. See sys_wm_get_windows() below.
    uint32_t owner_pid;

    // Linked list pointers
    struct window *next;
    struct window *prev;
} window_t;

// Window manager functions

// Initialize the window manager
void wm_init(void);

// Create a new window
window_t *window_create(const char *title, int32_t x, int32_t y, int32_t width, int32_t height);
void wm_set_default_opacity(int opacity);  // global default + apply to all windows
extern uint8_t g_default_window_opacity;

// Destroy a window
void window_destroy(window_t *win);

// Show/hide a window
void window_show(window_t *win);
void window_hide(window_t *win);

// Minimize/maximize a window
void window_minimize(window_t *win);
void window_maximize(window_t *win);
void window_restore(window_t *win);
void wm_toggle_maximize_focused(void);

// Move a window
void window_move(window_t *win, int32_t x, int32_t y);

// Resize a window
void window_resize(window_t *win, int32_t width, int32_t height);

// Set window title
void window_set_title(window_t *win, const char *title);

// Z-ordering
void window_bring_to_front(window_t *win);
void window_send_to_back(window_t *win);
window_t *window_get_at_point(int32_t x, int32_t y);

// Focus management
void window_set_focus(window_t *win);
window_t *window_get_focused(void);

// Event handling
void window_set_close_handler(window_t *win, event_handler_t handler);
void window_set_click_handler(window_t *win, event_handler_t handler);
void window_set_key_handler(window_t *win, event_handler_t handler);

// Process events (call this from main loop)
void wm_process_events(void);

// Handle mouse events
void wm_handle_mouse_move(int32_t x, int32_t y);
void wm_handle_mouse_down(int32_t x, int32_t y, uint32_t button);
void wm_handle_mouse_up(int32_t x, int32_t y, uint32_t button);

// Handle keyboard events
void wm_handle_key_down(uint32_t keycode, char key_char);
void wm_handle_key_up(uint32_t keycode);

// Drawing
void window_draw(window_t *win);
void window_invalidate(window_t *win);
void wm_draw_all(void);
void wm_draw_winmenu(void);   // decorator popup, drawn on top after wm_draw_apps()
void wm_redraw_screen(void);

// Utility functions
bool rect_contains_point(const rect_t *rect, int32_t x, int32_t y);

// #27: themed window corners. `radius.window` in the active .mtheme is the
// chamfer extent in pixels of a window's four outer corners (0 = square).
// window_point_is_cut() is the single definition of that shape: the renderer
// and window_get_at_point()'s hit test both use it.
int32_t window_corner_bevel(const window_t *win);
bool    window_point_is_cut(const window_t *win, int32_t x, int32_t y);
void    wm_capture_corners(void);
void    window_punch_corners(window_t *win);
bool rect_intersects(const rect_t *a, const rect_t *b);
rect_t window_get_client_rect(const window_t *win);

// Get the content area (excluding title bar and borders)
void window_get_content_bounds(const window_t *win, int32_t *x, int32_t *y, int32_t *w, int32_t *h);

// Draw text in window coordinates
void window_draw_text(window_t *win, int32_t x, int32_t y, const char *text, uint32_t color);

// ============================================================================
// Event Queue Functions
// ============================================================================

// Queue an event for later processing
void wm_queue_event(gui_event_t *event);

// Poll the next event from the queue (returns false if empty)
bool wm_poll_event(gui_event_t *event);

// Clear all queued events
void wm_clear_event_queue(void);

// ============================================================================
// App Registration Functions
// ============================================================================

// Register an app with the window manager
// Returns app_id (>= 0) on success, -1 on failure
int wm_register_app(window_t *win, void *app_data,
                    app_event_handler_t on_event,
                    app_draw_handler_t on_draw,
                    app_destroy_handler_t on_destroy);

// Unregister an app
void wm_unregister_app(int app_id);

// Get app registration by window
app_registration_t *wm_get_app_by_window(window_t *win);

// Get app registration by id
app_registration_t *wm_get_app_by_id(int app_id);

// Get all windows (for task manager, etc.)
// Returns count, fills array pointer
int wm_get_window_count(void);
window_t *wm_get_window_at_index(int index);

// Focus a window (brings to front and sets focus)
void wm_focus_window(window_t *win);

// Close a window through app registration (triggers proper cleanup)
void window_close(window_t *win);

// ============================================================================
// Dirty Region Tracking
// ============================================================================

// Mark a rectangle as needing redraw
// (#745) Work area. Insets are in pixels reserved at each screen edge; the
// rect wm_get_work_area() returns is the screen minus those insets, already
// sanity-clamped so it can never be empty. wm_set_work_area() is idempotent
// and, on an actual change, re-applies the new geometry to every existing
// window (a live dock-style switch, or a position restored from a profile
// written under a different style, can otherwise strand a header under a
// panel that is only now reserved).
void wm_set_work_area(int32_t left, int32_t top, int32_t right, int32_t bottom);
void wm_get_work_area(rect_t *out);
// Clamp a PROPOSED top-left so this window's title bar stays reachable.
void wm_clamp_to_work_area(const window_t *win, int32_t *x, int32_t *y);

void wm_invalidate_rect(const rect_t *rect);

// Mark entire screen as needing redraw
void wm_invalidate_all(void);
// Mark a region dirty from a NON-WM thread (kernel-owned host windows: the DOS
// layer, and the Win16 subsystem if it is ever wired up). Marks only, never
// draws. See the definition in window.c for why the plain form is unsafe there.
void wm_invalidate_rect_async(const rect_t *rect);

// Check if any region needs redraw
bool wm_is_dirty(void);

// Clear dirty state after redraw
void wm_clear_dirty(void);

// Get dirty region info
const dirty_region_t *wm_get_dirty_region(void);

// ============================================================================
// Main Frame Processing
// ============================================================================

// Process one frame: dispatch events to apps, render if dirty
// Returns false if desktop should exit
bool wm_process_frame(void);

// Dispatch a single event to the appropriate app
void wm_dispatch_event(gui_event_t *event);

// Draw all registered app contents (call after wm_draw_all)
void wm_draw_apps(void);

// Exclusive fullscreen mode (used by DOOM and other full-screen apps)
void wm_enter_exclusive_mode(void);
void wm_exit_exclusive_mode(void);
bool wm_is_exclusive_mode(void);

// Notify user-mode apps when their window is resized (called from window_resize)
void user_window_handle_resize(window_t *win);


// Window info for userland compositor query (sys_wm_get_windows)
typedef struct {
    int     id;
    int     x, y, width, height;
    int     visible;
    int     minimized;
    int     focused;
    char    title[64];
    // #745: APPENDED, never reordered. Userland's mirror of this struct
    // (userland/libc/syscall.h) pins the same size with the same assert, so a
    // one-sided edit fails the build instead of overrunning the caller's array
    // by sizeof(int) per window.
    int     shadow;
    // #41 task (dock duplicate-icon fix, 2026-08-12): a stable app identity,
    // NOT a display name. This is win->owner_pid resolved through proc_get()
    // to that process's process_t.name - which is the BINARY BASENAME the
    // kernel actually spawned (launch_userspace_app()/SYS_EXEC both derive
    // `name` from the last path component of the exec path, e.g. "/APPS/PAINT"
    // -> "PAINT"), never a window's own chosen title string. Empty string
    // means "no identity available" (owner_pid was 0, or the owning process
    // has already exited) - callers MUST treat that as a fallback case, not
    // a real empty app, and should not silently match on it.
    char    app_id[32];
    // #44 (dock context-menu task, 2026-08-12): APPENDED, never reordered,
    // same discipline as app_id above. wm_window_info_t carried no way to
    // tell an already-maximized window from a normal one, which is why an
    // earlier pass (taskbar.c right-click menu) deliberately did NOT offer
    // "Maximize": SYS_WM_MAXIMIZE_WINDOW's kernel side
    // (wm_toggle_maximize_focused()) is a TOGGLE, and calling it on a window
    // that is already maximized RESTORES it instead - a menu item labeled
    // "Maximize" would sometimes do the opposite of what it says. This bit
    // is what lets userland pick the correct label and skip the call when
    // it would already be a no-op/wrong-way toggle.
    int     maximized;
} wm_window_info_t;
_Static_assert(sizeof(wm_window_info_t) == 136,
               "#745/#41/#44: wm_window_info_t layout is duplicated in userland/libc/syscall.h; "
               "change both or neither");

// Syscall: fill buf with info about up to max_count windows
// Returns number of windows filled in
int64_t sys_wm_get_windows(wm_window_info_t *buf, int max_count);
int64_t sys_wm_focus_window(int id);

// #745: eligibility for the compositor-drawn drop shadow. Defined next to
// window_corner_bevel() in window.c; see there for why the theme gate applies
// to chromed windows only.
bool window_wants_shadow(const window_t *win);

// #745: opt this window in to the compositor-drawn drop shadow. One-way, like
// sys_win_set_nochrome(): nothing clears it, so a window cannot flicker its
// shadow on and off frame to frame.
int64_t sys_win_set_shadow(int handle);

// #564: cheap PEEK for the userland compositor's render gate - "has anything
// in the kernel WM changed since the last full composite" (window move/
// resize/focus/create/close - all already call wm_invalidate_rect()/
// wm_invalidate_all() below - or an app's own win_invalidate(), which
// syscall.c's sys_win_invalidate() already routes into window_invalidate() ->
// wm_invalidate_rect()). Backed by the SAME wm_is_dirty()/dirty_region_t this
// file already used internally for its own wm_process_frame() fallback path
// (only reachable when /APPS/COMPOSIT is absent - see kernel/gui/desktop.c);
// while the real userland compositor is running that flag previously had NO
// consumer at all, so this is exposing existing, comprehensive infrastructure
// rather than adding new tracking (#379/#548 - "the damage infra may exist
// but the render gate isn't wired to it"). Read-only: does NOT clear the
// flag, so the compositor can decide whether to render BEFORE it draws
// anything; SYS_COMPOSITOR_RENDER_WINDOWS (syscall.c) clears it once it has
// actually recomposited, so nothing is ever marked clean before it is truly
// on screen. Returns 1 if dirty, 0 if not - no user pointers, so it needs no
// rustkern/argtab.rs entry (syscall_validate_args returns 0/"not validated"
// for any syscall with no descriptor, which is correct here: there is
// nothing to validate).
int64_t sys_wm_apps_dirty(void);
int64_t sys_wm_minimize_window(int id);
void wm_inject_app_mouse(int32_t x, int32_t y, int32_t type, uint32_t button);
void wm_inject_app_scroll(int32_t x, int32_t y, int32_t delta);

#endif // WINDOW_H
