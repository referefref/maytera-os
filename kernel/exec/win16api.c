// win16api.c - Win16 API dispatch + minimal KERNEL/USER/GDI for the NE loader.
// Clean-room implementation (#129, Phase 2 Step A). See win16api.h for the role.
//
// Calling convention: Win16 APIs are __far __pascal. The CALLEE pops its own
// arguments. Arguments are pushed LEFT-TO-RIGHT, so the LAST declared argument is
// nearest the top of stack (just above the far return address). On entry the
// 16-bit stack is:
//     SS:[SP+0] = return IP, SS:[SP+2] = return CS, SS:[SP+4..] = args.
// Each handler reads its args relative to (SP+4), sets the dispatcher's AX/DX
// return values, and reports how many ARGUMENT BYTES to discard so the dispatcher
// can do the equivalent of "RETF n".
#include "win16api.h"
#include "ne.h"
#include "../serial.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
extern uint32_t fb_get_pixel(uint32_t x, uint32_t y);
extern fat_fs_t g_fat_fs;
// (#257) shared drive-letter FS layer (dos/dospath.c).
void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz);
int  dos_drive_type(char letter);
int  dos_path_writable(const char *in);

// ---------------------------------------------------------------------------
// Win16 debug trace. kprintf is not routed to this VM's serial socket, so the
// per-run API/loader trace is also accumulated into a memory buffer and written
// to /WIN16LOG.TXT at the end of the run (cat it via the RC console). This is
// the discovery tool: it shows every API call (and every UNIMPL one) per app.
// ---------------------------------------------------------------------------
#define WIN16_TRACE_CAP 262144   /* 256KB (diagnostic) */
static char     g_trace[WIN16_TRACE_CAP];
static uint32_t g_trace_len;
static uint32_t g_trace_last_flush_len;  // (#205) only re-flush when the trace grew
static int      g_trace_flushed;       // mid-run flush latch
static uint8_t  g_import_seen[1024];   // first-occurrence trace dedup (per run)
int             g_wndproc_trace_n;     // diagnostic: cap wndproc trace lines
int             g_call_trace_n;        // diagnostic: cap full-call trace lines
int             g_win16_app_kind;      // 0=generic, 1=tetris, 2=card game (set by ne.c)
// (#255 perf C) Set whenever the input pump posts a message (timer/key/mouse) so
// the idle loop can skip the expensive full repaint (2x canvas memcpy + guest
// WM_PAINT re-dispatch) on frames where nothing changed. Starts set so the first
// frame paints; a safety floor in the idle loop repaints anyway at ~2 Hz.
int             g_win16_frame_dirty = 1;
void win16_trace(const char *fmt, ...) {
    if (g_trace_len + 256 >= WIN16_TRACE_CAP) return;   // stop when nearly full
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vsnprintf(g_trace + g_trace_len, 256, fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) g_trace_len += (uint32_t)(n < 256 ? n : 255);
}

// Side buffer for app-DLL import resolution, captured during relocation (before
// win16_api_begin clears g_trace), then folded into the run trace by begin.
static char     g_reloc_log[2048];
static uint32_t g_reloc_log_len;
static void reloc_logf(const char *fmt, ...) {
    if (g_reloc_log_len + 96 >= sizeof(g_reloc_log)) return;
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vsnprintf(g_reloc_log + g_reloc_log_len, 96, fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) g_reloc_log_len += (uint32_t)(n < 96 ? n : 95);
}
void win16_reloc_log(const char *mod, const char *name, unsigned ord,
                     int real, unsigned seg, unsigned off) {
    if (name && name[0])
        reloc_logf("import %s.%s ord=%u -> %s %04x:%04x\n",
                   mod, name, ord, real ? "REAL" : "thunk", seg, off);
    else
        reloc_logf("import %s.#%u -> %s %04x:%04x\n",
                   mod, ord, real ? "REAL" : "thunk", seg, off);
}

// 8086 FLAGS bits we need to influence on return (mirror of x86_16.c's table;
// kept local because the interpreter does not export them in the header).
#define WIN16_F_CF 0x0001   // carry flag

// ---------------------------------------------------------------------------
// Win16 message constants (subset). Values match real Windows.
// ---------------------------------------------------------------------------
#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_SIZE         0x0005
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_ERASEBKGND   0x0014
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_COMMAND      0x0111
#define WM_TIMER        0x0113
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205
#define MK_LBUTTON      0x0001
#define MK_RBUTTON      0x0002

// GetSystemMetrics indices we honour.
#define SM_CXSCREEN     0
#define SM_CYSCREEN     1

// Pseudo HWND returned by GetDesktopWindow (USER.286). Apps query it for the
// screen size or a screen DC; it never maps to a real Win16 window slot.
// MUST be outside the real-window handle range (0x40 + slot, slot 0..7 => 0x40..0x47):
// it was 0x0040, which COLLIDED with the FIRST app window (slot 0). That made
// GetClientRect/GetWindowRect on an app's own main window return the full SCREEN
// size instead of its client, so wide layouts (FreeCell's 8 cascade columns)
// spread across the screen width and only ~half fit the canvas (#152).
#define WIN16_HWND_DESKTOP 0x8000

// ShowWindow commands.
#define SW_HIDE         0
#define SW_SHOWNORMAL   1

// ---------------------------------------------------------------------------
// Per-run state (loader info + import table snapshot).
// ---------------------------------------------------------------------------
static win16_loader_info_t g_info;
static const win16_import_t *g_imports = 0;
static int           g_import_count = 0;
uint16_t             g_win16_master_sel = 0;  /* (#278 Word6) KERNEL.29 */
static unsigned long g_api_calls = 0;
// Raised for interactive runs: a continuously repainting game issues a few
// hundred GDI calls per frame, so a low cap would stop the CPU within seconds.
// The GetMessage idle ceiling (~3 min) is the real interactive bound now.
#define WIN16_API_CALL_CAP 50000000UL

// ---------------------------------------------------------------------------
// 16-bit global/local handle allocator. Backed by a region of the 1 MiB image
// just below the thunk segment so it never collides with the loaded NE segments
// (which start at 0x1000 and grow upward, but real apps stay well under 448 KB).
// Each allocation gets its own paragraph-aligned block; the handle we hand back
// is the segment paragraph itself (so GlobalLock can just return handle:0000,
// which is how real KERNEL behaves for the selector==handle case). This is a
// bump allocator: GlobalFree/LocalFree are accepted but only the last block is
// actually reclaimed. That is fine for startup discovery.
// ---------------------------------------------------------------------------
#define WIN16_HEAP_FIRST_PARA 0x8000   // 0x80000 linear
#define WIN16_HEAP_LAST_PARA  0xEFFF   // just below thunk seg 0xF000
static uint16_t g_heap_next;           // next free paragraph
static uint16_t g_heap_last_block;     // for trivial last-block reclaim
static uint16_t g_heap_last_para_size; // size (paras) of last block
// (#188) Per-block byte size so GlobalSize/GlobalReAlloc report the real size a
// caller asked for. Keyed by (handle - WIN16_HEAP_FIRST_PARA) which is small.
#define WIN16_GBLK_MAX 512
static uint16_t g_gblk_handle[WIN16_GBLK_MAX];
static uint32_t g_gblk_size[WIN16_GBLK_MAX];
static int      g_gblk_count;
static void gblk_record(uint16_t h, uint32_t bytes) {
    if (h == 0) return;
    for (int i = 0; i < g_gblk_count; i++)
        if (g_gblk_handle[i] == h) { g_gblk_size[i] = bytes; return; }
    if (g_gblk_count < WIN16_GBLK_MAX) {
        g_gblk_handle[g_gblk_count] = h; g_gblk_size[g_gblk_count] = bytes; g_gblk_count++;
    }
}
static uint32_t gblk_lookup(uint16_t h) {
    for (int i = 0; i < g_gblk_count; i++)
        if (g_gblk_handle[i] == h) return g_gblk_size[i];
    return 0;
}

// Local (near) heap: a bump allocator returning NEAR offsets inside the app's
// DGROUP segment. LocalAlloc returns a near offset handle; the MS-C startup and
// many Win16 apps (SkiFree #200) use the result directly as a near pointer.
// Range [g_lheap_next .. g_lheap_top) is the reserved ne_heap area in DGROUP.
static x86_16_cpu_t *g_cpu;   // (#289 b487) fwd ref (defined later) for lheap current-DS
static uint16_t g_lheap_next;          // app DGROUP local heap (default/fallback)
static uint16_t g_lheap_top;
// (#278 Word6) DGROUP local-heap geometry. The MS-C runtime's __LInit recomputes
// the local-heap end marker (pLast) as GlobalSize(DGROUP) - hdr; if GlobalSize
// returns the FULL autodata segment (which includes ne_stack at the top) the
// arena's pLast lands OVER the stack, leaving a gap the heap-walk never crosses.
// We therefore record the stack-safe heap top (data_len + ne_heap, below the
// stack) and report THAT as GlobalSize for the DGROUP selector so __LInit builds
// a coherent arena that the walk terminates on naturally (no synthetic bridge).
static uint16_t g_dgroup_sel;        // initial DS (autodata) selector
static uint16_t g_dgroup_heap_top;   // near offset where the local heap ends (below stack)
// (#278 Word6 pass20) Exported DGROUP geometry for the x86_16 heap-base clamp.
// win16_dgroup_heap_base = end of static data (loader-computed lheap_base);
// Word's MS-C __LInit must place its near-heap pStart at [DGROUP:0x0e] HERE,
// not below it, else heap blocks overlap Word static globals.
uint16_t win16_dgroup_sel = 0;
uint16_t win16_dgroup_heap_base = 0;

// (#278 P52 runtime-callf-trace) latched GDI ordinal about to be dispatched;
// see the CreateDC/CreateCompatibleDC alias note at the generic dispatcher and
// at g_createcompatibledc.
int g_w6last_gdi_ord = -1;

// (#289 b487) Per-segment local heaps. STORAGE.DLL (and other OLE2 DLLs) call
// LocalInit() to set up a local heap inside EACH GlobalAlloc'd object segment,
// then LocalAlloc into the segment currently in DS. A single global bump pointer
// conflated all of them and exhausted (LocalAlloc -> 0), so StgCreateDocfile's
// object allocation failed and it returned a NULL IStorage. Track a small table
// keyed by segment selector.
#define WIN16_MAX_LSEG 32
typedef struct { uint16_t seg; uint16_t next; uint16_t top; } lseg_t;
static lseg_t g_lseg[WIN16_MAX_LSEG];
static int    g_lseg_n;
static lseg_t *lseg_find(uint16_t seg) {
    for (int i = 0; i < g_lseg_n; i++) if (g_lseg[i].seg == seg) return &g_lseg[i];
    return 0;
}
static lseg_t *lseg_get_or_add(uint16_t seg) {
    lseg_t *e = lseg_find(seg);
    if (e) return e;
    if (g_lseg_n >= WIN16_MAX_LSEG) return 0;
    e = &g_lseg[g_lseg_n++]; e->seg = seg; e->next = 0; e->top = 0;
    return e;
}
// Allocate within the local heap of segment `seg`. Falls back to the app DGROUP
// bump pointer for the app's own segment (seg not registered via LocalInit).
static uint16_t lheap_alloc_seg(uint16_t seg, uint32_t bytes) {
    if (bytes == 0) bytes = 1;
    bytes = (bytes + 1u) & ~1u;        // word-align
    lseg_t *e = lseg_find(seg);
    if (e && e->top) {
        if ((uint32_t)e->next + bytes > e->top) return 0;
        uint16_t off = e->next; e->next = (uint16_t)(e->next + bytes); return off;
    }
    // fallback: app DGROUP global heap
    if ((uint32_t)g_lheap_next + bytes > g_lheap_top) return 0;
    uint16_t off = g_lheap_next; g_lheap_next = (uint16_t)(g_lheap_next + bytes);
    return off;
}
// Back-compat shim: allocate in the CURRENT DS segment.
static uint16_t lheap_alloc(uint32_t bytes) {
    uint16_t seg = g_cpu ? g_cpu->ds : 0;
    return lheap_alloc_seg(seg, bytes);
}

// FindResource/LoadResource handle table (#148: Chips Challenge loads its tiles
// via the resource-handle API, not LoadBitmap). FindResource locates the bytes
// in the kernel-side module image; LoadResource copies them into the Win16
// global heap so LockResource can hand the app an addressable far pointer.
#define WIN16_MAX_RSRC 32
static struct { const uint8_t *p; uint32_t len; uint16_t seg; } g_rsrc[WIN16_MAX_RSRC];
static int g_rsrc_n;

static void heap_reset(void) {
    g_heap_next = WIN16_HEAP_FIRST_PARA;
    g_heap_last_block = 0;
    g_heap_last_para_size = 0;
    g_lheap_next = 0;
    g_lheap_top = 0;
    g_lseg_n = 0;
    g_rsrc_n = 0;
    g_gblk_count = 0;   // (#188) clear per-run GlobalSize tracking
}

// Allocate `bytes`; return the block's segment paragraph (the handle), or 0.
//
// (#289 Phase1) In protected mode the "handle" is a real SELECTOR backed by an
// arena block. Blocks larger than 64 KiB are tiled across CONSECUTIVE selectors
// (sel, sel+8, sel+16, ...) each based 64 KiB apart in ONE contiguous arena
// block, so a huge far pointer can walk tile boundaries by adding 8 to the
// selector when the offset wraps - matching how real Win16 KERNEL lays out
// huge GlobalAlloc blocks (__AHINCR == 8). The FIRST selector is the handle.
static uint16_t heap_alloc(uint32_t bytes) {
    if (bytes == 0) bytes = 1;

    if (g_win16_pmode) {
        uint32_t base = win16_arena_alloc(bytes, 16);
        if (!base) return 0;
        uint16_t first = 0, prev = 0;
        uint32_t off = 0, remaining = bytes;
        while (remaining > 0) {
            uint32_t tile = remaining > 0x10000u ? 0x10000u : remaining;
            uint16_t sel = ldt_alloc(base + off, tile - 1, 0);  // data selector
            if (sel == 0) return 0;            // LDT full
            if (!first) first = sel;           // (consecutive selectors guaranteed
                                               //  by ldt_alloc's linear scan)
            prev = sel; (void)prev;
            off += 0x10000u;
            remaining = remaining > 0x10000u ? remaining - 0x10000u : 0;
        }
        return first;
    }
    // (#188) Compute paragraphs in 32-bit; do NOT truncate to uint16 (a >1MB
    // request used to wrap to 0 paragraphs => a tiny block => caller overflow).
    uint32_t paras = (bytes + 15) >> 4;
    if (paras == 0) paras = 1;
    if ((uint32_t)g_heap_next + paras > WIN16_HEAP_LAST_PARA) return 0;
    uint16_t blk = g_heap_next;
    g_heap_next = (uint16_t)(g_heap_next + paras);
    g_heap_last_block = blk;
    g_heap_last_para_size = (uint16_t)paras;
    return blk;
}

static void heap_free(uint16_t handle) {
    // Reclaim only when freeing the most-recent block (LIFO). Otherwise leak;
    // the per-run reset reclaims everything for the next program.
    if (handle && handle == g_heap_last_block) {
        g_heap_next = g_heap_last_block;
        g_heap_last_block = 0;
        g_heap_last_para_size = 0;
    }
}

// ---------------------------------------------------------------------------
// Argument helpers. All read relative to the caller's first argument word, which
// sits at SS:[SP+4] (after return IP+CS). a0 = nearest the top = LAST pushed.
// ---------------------------------------------------------------------------
static uint16_t arg16(x86_16_cpu_t *c, int word_index) {
    return x86_16_rd16(c, c->ss, (uint16_t)(c->sp + 4 + word_index * 2));
}
static uint32_t arg32(x86_16_cpu_t *c, int word_index) {
    // A DWORD pushed Pascal-style occupies two words; the LOW word is pushed
    // LAST (nearer top), so it is at the lower word_index.
    uint16_t lo = arg16(c, word_index);
    uint16_t hi = arg16(c, word_index + 1);
    return ((uint32_t)hi << 16) | lo;
}

// ---------------------------------------------------------------------------
// Win16 GUI bridge state. We map each Win16 HWND to a real on-screen rectangle
// drawn straight into the framebuffer front buffer, and remember the registered
// class window-procedure (a 16-bit far pointer) so DispatchMessage can call it.
// This is intentionally a single-top-level-window model good enough to bring a
// TETRIS-style app's main window up and fire WM_PAINT through the wndproc.
// ---------------------------------------------------------------------------
#define WIN16_MAX_CLASSES 64   // real apps register many window classes (Chips
                               // registers 9+ via WEP4UTIL + its own children).
                               // Too small a table makes a RegisterClass return 0,
                               // which apps treat as fatal ("no main procedure").
// (#278 Word6 pass28) Word 6 creates many windows at startup (frame, MDI
// client, document work-area, toolbars, ruler, status bar...). With only 8
// slots the document work-area window (the 9th, CreateWindow at seg155:0xcd0
// in WinMain phase-2) failed -> NULL hwnd -> the seg136:0x13e3 init gate
// returned 0 -> OLE teardown -> PostQuitMessage(seg85:0x3693) = the pass-27
// self-quit. Raised to 64. Games (FreeCell/Chips create <=8 windows) use the
// same low slots and are byte-identical; only window-heavy apps benefit.
#define WIN16_MAX_WINDOWS 64
#define WIN16_MSGQ_SIZE   64

typedef struct {
    int      used;
    char     name[64];             // class name, COPIED at RegisterClass time
                                   // (the WNDCLASS lpszClassName memory may be
                                   //  reused/overwritten before CreateWindow).
    uint16_t proc_seg, proc_off;   // far ptr to lpfnWndProc
    uint16_t hbrBackground;        // background brush handle (GDI object id)
    int      has_menu;             // (#152) class default menu present
    uint16_t menu_id;              // MAKEINTRESOURCE id (when menu_name[0]==0)
    char     menu_name[32];        // named menu resource (else empty)
} win16_class_t;

typedef struct {
    int      used;
    uint16_t hwnd;                 // the handle value handed to the app (== index+0x40)
    uint16_t parent;               // parent HWND (0 = top-level)
    int      is_child;             // WS_CHILD style set
    uint16_t proc_seg, proc_off;   // resolved wndproc (copied from class)
    int      x, y, w, h;           // top-level: outer rect on screen.
                                   // child: rect RELATIVE to parent client area.
    int      cx, cy, cw, ch;       // client rect in SCREEN coords (computed)
    char     title[64];
    uint16_t bg_brush;             // background brush color handle
    uint16_t hmenu;                // attached menu handle (0 = none) (#152)
    int      shown;
    // (#278 P46) The REAL WS_VISIBLE state, tracked separately from `shown` (which
    // we force to 1 for children so they render). Initialized from the create style
    // (WS_VISIBLE 0x10000000). ShowWindow flips it and, on a 0->1 transition, SENDs
    // the WM_SHOWWINDOW that real USER.ShowWindow delivers to the wndproc. Word
    // creates its edit pane (004f) WITHOUT WS_VISIBLE then ShowWindow()s it; our old
    // ShowWindow never delivered WM_SHOWWINDOW, so Word's view-realize path (which
    // hangs off that synchronous message) never ran and the doc view stayed unrealized.
    int      ws_visible;
    uint32_t dwstyle;              // full create dwStyle (for GetWindowLong GWL_STYLE)
    uint32_t z;                    // activation order (#207). Higher == more recently
                                   // created/shown; the highest shown top-level window
                                   // owns input focus so a later popup/About box (which
                                   // overlays the game) actually receives the click/key
                                   // that dismisses it.
    // (#200 SkiFree) Per-window extra words (cbWndExtra) + standard window words.
    // SkiFree stores a back-pointer to its own window object via SetWindowWord
    // (DGROUP offset) at create time and reads it back via GetWindowWord on every
    // paint/timer; the prior no-op stubs returned 0, so SkiFree got a NULL object
    // pointer and tripped its ski2.c null-arg assertions (lines 1005/1129/1204)
    // and never drew. Backed by real storage now. 32 words covers the GWW_*
    // negative indices folded into a small array plus any class cbWndExtra.
    uint16_t wndwords[32];
    // (#219) Per-window invalid region (client coords). InvalidateRect unions a
    // dirty rect here; BeginPaint returns it as rcPaint + latches it as the clip
    // box and validates it. Lets apps (e.g. Golf solitaire) repaint only the
    // changed area instead of re-greening the whole window over their content.
    int inval_valid;
    int inval_l, inval_t, inval_r, inval_b;
    // (#393b) Predefined USER control support. When an app creates a child of a
    // built-in class ("BUTTON"/"STATIC"/"EDIT"/...) it registers NO wndproc of its
    // own, so proc_seg/off are 0 and USER.EXE's own class proc supplies the
    // behaviour. We emulate that here. ctrl_kind: 0=app window, 1=BUTTON, 2=STATIC,
    // 3=EDIT, 4=LISTBOX/other predefined. ctrl_id is the child's control ID (the
    // hMenu argument of CreateWindow for a child). btn_style is the BS_* low nibble;
    // btn_pressed tracks the visual sunken state between WM_LBUTTONDOWN and UP;
    // btn_check is the checkbox/radio checked state.
    uint8_t  ctrl_kind;
    uint8_t  btn_style;
    uint8_t  btn_pressed;
    uint8_t  btn_check;
    uint16_t ctrl_id;
} win16_window_t;

typedef struct {
    uint16_t hwnd;
    uint16_t message;
    uint16_t wParam;
    uint32_t lParam;
} win16_msg_t;

static win16_class_t  g_classes[WIN16_MAX_CLASSES];
static win16_window_t g_windows[WIN16_MAX_WINDOWS];
static win16_msg_t    g_msgq[WIN16_MSGQ_SIZE];
static int            g_msgq_head, g_msgq_tail, g_msgq_count;
static int            g_quit_posted;
// (#200 SkiFree) Accessor so the NE run loop (ne.c) can tell whether a WM_QUIT
// has been latched (ESC / titlebar-X / app PostQuitMessage) and stop resuming.
int g_quit_posted_get(void) { return g_quit_posted; }
// (#215) Close-request flag. Set asynchronously by the kernel desktop when the
// user clicks the host window's titlebar CLOSE (X) button (routed via the host
// window's on_close handler in proc/syscall.c -> win16_request_close()). The
// Win16 message pump polls it and converts it into a WM_QUIT, which makes the
// app's GetMessage loop exit -> WinMain returns -> the interpreter halts ->
// x86_16_run returns -> win16_api_end()/registry_reset() free the slot. This is
// the same teardown path ESC and WM_QUIT already take; the X button simply had
// no route into the interpreter loop before. volatile: written from the desktop
// context, read from the Win16 proc context.
volatile int          g_win16_close_requested = 0;
static uint32_t       g_win16_z_counter;  // (#207) monotonic activation stamp source
// (#209) flicker fix: the idle repaint only does a full z-repaint (which fills
// each client with its background brush) when the z-ORDER actually changes (a
// popup/dialog opens or closes, a window is shown/created/moved). On a quiet
// idle frame the focused window is "soft" repainted instead: its WM_PAINT is
// dispatched WITHOUT first clearing the client to the bg brush, so the app's
// own pixels persist between frames and the playfield no longer flashes
// grey<->black. g_win16_no_bg_fill gates the canvas clear inside
// win16_draw_frame for top-level windows during that soft pass.
static int            g_win16_z_dirty = 1;   // (#209) full z-repaint pending
static int            g_win16_no_bg_fill;    // (#209) suppress top-level client bg clear
static int            g_autostart_done;  // one-shot: auto-issue Game->New (WM_COMMAND 126)
static x86_16_cpu_t  *g_cpu;          // set per dispatch so GDI helpers can read mem

// ---------------------------------------------------------------------------
// Taskbar integration (#139). A Win16 top-level window is drawn directly to the
// framebuffer, but the MayteraOS compositor taskbar enumerates the kernel window
// manager's window list (sys_wm_get_windows). To make the Win16 window appear as
// a real taskbar button (title + clickable), we register a matching kernel
// window_t in that list for the lifetime of the run and tear it down at the end.
// window.h is not included here (it pulls heavy GUI deps); use opaque externs.
// ---------------------------------------------------------------------------
struct window;
extern struct window *window_create(const char *title, int x, int y, int w, int h);
extern void           window_destroy(struct window *win);
extern void           window_set_title(struct window *win, const char *title);
static struct window *g_taskbar_win;   // kernel WM entry mirroring the Win16 top window

// ---------------------------------------------------------------------------
// Compositor-integrated rendering (#144/#145). Instead of drawing the Win16
// window directly to the framebuffer (which froze the desktop because the
// interpreter ran synchronously and owned the screen), the Win16 top window is
// backed by a normal kernel "user window" content buffer. The interpreter runs
// in its own kernel thread and paints into this canvas; the compositor then
// composites it like any other app window, so the desktop + taskbar stay live.
// win16_host_create()/win16_host_destroy() live in proc/syscall.c next to the
// user-window machinery; here we just hold the canvas pointer + client size.
// ---------------------------------------------------------------------------
extern int  win16_host_create(const char *title, int x, int y, int w, int h,
                              uint32_t **out_buf, int *out_w, int *out_h,
                              struct window **out_win);
extern void win16_host_destroy(int slot);
// On-screen content rect of the host window, for mapping the global cursor into
// the Win16 canvas (mouse forwarding, #187).
extern int  win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh);
// Global kernel cursor + button state (drivers/mouse.c). Screen coords.
extern int32_t mouse_x;
extern int32_t mouse_y;
extern uint8_t mouse_buttons;

static uint32_t *g_win16_canvas;    // host window client buffer (NULL until CreateWindow)
static int       g_win16_canvas_w;  // client width  (pixels)
static int       g_win16_canvas_h;  // client height (pixels)
static int       g_win16_host_slot = -1;
// (#345) Win16 .SCR screensaver mode (set in exec/ne.c from a "/s" command tail).
// When set, the host window is created fullscreen + borderless and any real user
// input ends the saver. Ordinary apps/games never set it (byte-identical path).
extern int g_win16_scrsave;
extern volatile unsigned long long g_win16_scrsave_start;
static uint16_t  g_win16_main_hwnd = 0;  // hwnd that owns the host canvas (full-screen app window)
int              g_win16_apilog = 0;     // (#188) per-call API dispatch trace (default OFF; floods serial)
// (#278 word6) Budget-limited paint diagnostic to serial. Set >0 to capture the
// first N drawing calls from boot (covers Word's startup paint), then goes quiet
// so it never floods. Default 0 (off).
int              g_w6log = 0;
#define W6LOG(...) do { if (g_w6log > 0) { g_w6log--; kprintf(__VA_ARGS__); } } while (0)
// (#210) Off-screen scratch buffer for flicker-free full repaints. The compositor
// samples g_win16_canvas asynchronously; if we bg-fill then redraw the board IN
// PLACE every frame, the compositor can catch a half-grey intermediate frame
// (#209 flicker). Instead a soft repaint renders a COMPLETE frame (bg fill + all
// windows + all children, including the black playfield base + grid + pieces)
// into this scratch buffer, then copies the finished frame to the real canvas in
// one memcpy. The compositor only ever sees fully-formed frames, so there is no
// flicker AND the board is fully present every frame (no black playfield between
// the rare z-dirty repaints).
static uint32_t *g_win16_scratch;   // off-screen render target (canvas-sized)
static int       g_win16_scratch_w;
static int       g_win16_scratch_h;
// (#219) Per-frame double buffering for APP-driven paints. A window paint cycle
// (BeginPaint..EndPaint / GetDC..ReleaseDC) redirects drawing into g_win16_scratch
// and publishes the finished frame to the real canvas in one memcpy on the end,
// so the compositor never samples a half-drawn frame (e.g. TETRIS's black
// well-clear before the cells -> the "renders briefly then black" flicker).
static int       g_win16_paint_depth;      // nested window-paint depth
static uint32_t *g_win16_frontbuf;         // real canvas saved while redirected
static int       g_win16_in_soft_repaint;  // soft_repaint owns the scratch then
// (#219) Current paint clip box (client coords) latched by BeginPaint, returned
// by GetClipBox during the paint. Inactive -> GetClipBox reports the full window.
static int       g_win16_clip_active;
static int       g_win16_clip_l, g_win16_clip_t, g_win16_clip_r, g_win16_clip_b;

// Plot one pixel into the canvas (canvas-relative coords).
static inline void canvas_plot(int x, int y, uint32_t color) {
    if (!g_win16_canvas) return;
    if (x < 0 || y < 0 || x >= g_win16_canvas_w || y >= g_win16_canvas_h) return;
    g_win16_canvas[y * g_win16_canvas_w + x] = color;
}
// Read one pixel from the canvas (used by GetPixel).
static inline uint32_t canvas_get(int x, int y) {
    if (!g_win16_canvas) return 0;
    if (x < 0 || y < 0 || x >= g_win16_canvas_w || y >= g_win16_canvas_h) return 0;
    return g_win16_canvas[y * g_win16_canvas_w + x];
}
// Fill a rectangle of the canvas.
static inline void canvas_fill(int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            canvas_plot(xx, yy, color);
}

// (#bugB) WS_CLIPCHILDREN support: a canvas pixel (canvas/screen coords) is "owned"
// by a visible direct child window of `parent` when it falls inside that child's
// client rect. Real USER excludes those child rectangles from the parent's painting
// so a parent never draws over its children; children then paint themselves on top.
// We had no clipping, so Word 6's frame (background brush = white, no WS_CLIPCHILDREN
// set) erased its WHOLE client to white on a repaint after a scroll, painting over
// its own toolbar/ruler/menu-strip child windows; those children were then only
// force-repainted with do_erase=0 (nothing invalidated) so they never re-issued
// their own pixels -> the entire chrome blanked to white (#bugB). Excluding the
// child rectangles from the frame's background erase keeps the chrome pixels intact
// while still clearing the true document area. Only DIRECT children are considered
// (a grandchild is clipped by its own parent) and only shown ones with a real rect.
static int win16_pixel_in_child(uint16_t parent_hwnd, int sx, int sy) {
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *w = &g_windows[i];
        if (!w->used || !w->is_child || w->parent != parent_hwnd) continue;
        if (!w->shown || w->cw <= 0 || w->ch <= 0) continue;
        if (sx >= w->cx && sx < w->cx + w->cw &&
            sy >= w->cy && sy < w->cy + w->ch)
            return 1;
    }
    return 0;
}

// (#bugB) Height of the top "menu strip" the frame owns above its topmost child.
// Word 6's main frame carries a real menu bar ("File Edit View ...") but reports
// hmenu==0 to us (its MDI frame draws the bar itself into the frame DC as if it
// were non-client area, rather than through SetMenu). Our win16_draw_menubar is
// therefore never called for it, and the frame's background erase (which excludes
// only child rectangles) painted over those menu pixels on every scroll repaint
// because no child window covers the y=0..menu strip, blanking the menu row.
// Real USER never erases the non-client menu bar with the client background, so we
// must preserve that strip. We detect it geometrically: the strip is the band of
// rows above the TOPMOST top-anchored direct child (Word's toolbar starts at cy=16,
// so the 0..15 band is the menu). Only a SMALL gap (a menu bar is ~16-24px) counts,
// so a game whose only child is a full-window playfield (gap 0) or a large document
// area is unaffected and its top pixels are still erased normally.
#define WIN16_MENU_STRIP_MAX 32   // a menu bar is never taller than this
// (#bugB) Latch the last observed menu-strip height for the current main frame.
// Word tears down/re-lays-out its command bars while rebuilding the view (Ctrl+Home,
// a page reflow), and for a frame or two NO top-anchored child is present, so the
// live geometry scan momentarily returns 0. A single full background erase in that
// window (clip inactive, bg fill on) would then white over the menu row, and Word
// never repaints its (non-client) menu, so the blank is permanent. Latching the last
// nonzero strip and reusing it across those transient 0-frames keeps the menu intact.
// The latch is keyed to the main hwnd so switching apps (Word -> a game) resets it;
// a game never latches a nonzero strip (it has no small-gap top-anchored child).
static int      g_menu_strip_latch = 0;
static uint16_t g_menu_strip_latch_hwnd = 0;
static int win16_menu_strip_h(uint16_t parent_hwnd) {
    int strip = 0;   // 0 = no menu strip detected this frame
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *w = &g_windows[i];
        if (!w->used || !w->is_child || w->parent != parent_hwnd) continue;
        if (!w->shown || w->cw <= 0 || w->ch <= 0) continue;
        // Only top-anchored children that begin within the menu-bar band and span
        // (roughly) the full width, i.e. the toolbar/command bar directly under the
        // menu. Ignore the status bar (bottom) and narrow/off-screen helpers.
        if (w->cy <= 0 || w->cy > WIN16_MENU_STRIP_MAX) continue;
        if (w->cw < g_win16_canvas_w / 2) continue;
        if (strip == 0 || w->cy < strip) strip = w->cy;
    }
    if (parent_hwnd != g_menu_strip_latch_hwnd) {   // main frame changed: reset latch
        g_menu_strip_latch_hwnd = parent_hwnd;
        g_menu_strip_latch = 0;
    }
    if (strip > 0) g_menu_strip_latch = strip;      // remember it
    else if (g_menu_strip_latch > 0) strip = g_menu_strip_latch;  // reuse across transient 0-frames
    return strip;
}

// (#bugB) Self-healing menu-strip snapshot/restore. The geometric erase-preserve above
// stops OUR background erase from wiping Word's menu row, but Word ALSO fills its whole
// client (which our model reports as starting at canvas y=0, so it spans the menu strip)
// inside its own WM_PAINT and never repaints the (self-drawn, non-client) menu on a
// scroll. So after a full repaint the menu row can still be blanked. This backstop runs
// at the END of a full frame render (on whichever buffer is live): if the strip currently
// contains menu ink it snapshots it; if the strip has been blanked it restores the last
// good snapshot. Result: the "File Edit View ..." row survives any repaint. Keyed to the
// main hwnd + geometry so switching apps (Word -> a game) discards the snapshot, and a
// game (menu_strip==0, no top-anchored command bar) is never touched.
static uint32_t *g_menu_snap = 0;
static int       g_menu_snap_w = 0, g_menu_snap_h = 0;
static int       g_menu_snap_valid = 0;
static uint16_t  g_menu_snap_hwnd = 0;
static win16_window_t *win_from_hwnd(uint16_t hwnd);   // fwd (defined below)
static void win16_menu_strip_maintain(void) {
    if (!g_win16_canvas || g_win16_canvas_w <= 0 || g_win16_canvas_h <= 0) return;
    uint16_t mh = g_win16_main_hwnd;
    if (!mh) return;
    win16_window_t *mw = win_from_hwnd(mh);
    if (!mw || mw->hmenu != 0) return;   // apps whose menu we track redraw it themselves
    int strip = win16_menu_strip_h(mh);
    if (strip <= 0) return;              // no menu strip (e.g. a game): never touch pixels
    if (strip > g_win16_canvas_h) strip = g_win16_canvas_h;
    int w = g_win16_canvas_w, h = strip;
    if (!g_menu_snap || g_menu_snap_w != w || g_menu_snap_h != h || g_menu_snap_hwnd != mh) {
        if (g_menu_snap) { kfree(g_menu_snap); g_menu_snap = 0; }
        g_menu_snap = (uint32_t *)kmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
        g_menu_snap_w = w; g_menu_snap_h = h; g_menu_snap_valid = 0; g_menu_snap_hwnd = mh;
        if (!g_menu_snap) return;
    }
    // Discriminate "menu present" from "menu blanked" by counting ink (dark) pixels in
    // the MENU-TEXT band ONLY. Empirically (per-row dark-pixel dump on VM2160), Word's
    // 16px menu strip is: row 2 = a full-width separator line, rows 11-15 = the toolbar's
    // top edge/bevel, and BOTH of those are dark in either state. The menu glyphs of
    // "File Edit View ... Help" live in rows 3-10 (x in [8,470)): ~1000 dark pixels when
    // the menu is shown, exactly 0 when it has been blanked. So counting rows 3-10 gives
    // a clean present/blank signal, where a whole-strip count could not (chrome lines
    // made it always non-zero).
    int ink = 0;
    int x0 = 8, x1 = (w < 470) ? w : 470;
    int y0 = (h > 11) ? 3 : 0, y1 = (h > 11) ? 11 : h;
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = g_win16_canvas + (size_t)y * g_win16_canvas_w;
        for (int x = x0; x < x1; x++) {
            uint32_t p = row[x];
            if (((p >> 16) & 0xff) < 100 && ((p >> 8) & 0xff) < 100 && (p & 0xff) < 100)
                ink++;
        }
    }
    // Capture-once with hysteresis. Snapshot the FIRST clearly-full menu (ink well above
    // the blank floor: the pristine "File Edit View ... Help" row is ~900 dark pixels)
    // and then never overwrite it, so a modal dialog or any transient that happens to
    // draw ink in the strip cannot corrupt the saved menu. Restore whenever the strip has
    // been blanked (ink at/near zero). Between the two thresholds (a partial/foreign
    // draw) do nothing, leaving whatever is on screen. The snapshot is discarded and
    // re-taken on a geometry or main-hwnd change (handled by the realloc above).
    #define WIN16_MENU_INK_CAP 200   // clearly a full menu row -> safe to snapshot
    #define WIN16_MENU_INK_MIN 40    // at/below this the strip is blank -> restore
    if (ink > WIN16_MENU_INK_CAP && !g_menu_snap_valid) {
        for (int y = 0; y < h; y++)
            memcpy(g_menu_snap + (size_t)y * w,
                   g_win16_canvas + (size_t)y * g_win16_canvas_w,
                   (size_t)w * sizeof(uint32_t));
        g_menu_snap_valid = 1;
    } else if (ink <= WIN16_MENU_INK_MIN && g_menu_snap_valid) {
        for (int y = 0; y < h; y++)
            memcpy(g_win16_canvas + (size_t)y * g_win16_canvas_w,
                   g_menu_snap + (size_t)y * w,
                   (size_t)w * sizeof(uint32_t));
    }
}

// Fill a canvas rectangle but skip pixels owned by a visible child of parent_hwnd,
// and (if preserve_top>0) skip the top `preserve_top` canvas rows (the menu strip).
static inline void canvas_fill_excl_children_top(int x, int y, int w, int h,
                                                 uint32_t color, uint16_t parent_hwnd,
                                                 int preserve_top) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < preserve_top) continue;   // (#bugB) leave the frame's menu strip intact
        for (int xx = x; xx < x + w; xx++)
            if (!win16_pixel_in_child(parent_hwnd, xx, yy))
                canvas_plot(xx, yy, color);
    }
}

// Fill a canvas rectangle but skip pixels owned by a visible child of parent_hwnd.
static inline void canvas_fill_excl_children(int x, int y, int w, int h,
                                             uint32_t color, uint16_t parent_hwnd) {
    canvas_fill_excl_children_top(x, y, w, h, color, parent_hwnd, 0);
}

// ---------------------------------------------------------------------------
// Win16 timers (SetTimer/KillTimer). A SetTimer(hwnd,id,ms,proc) registers a
// periodic WM_TIMER that GetMessage's pump posts to the queue every `period`
// kernel ticks. This is what drives TETRIS's gravity (the piece falling).
// ---------------------------------------------------------------------------
#define WIN16_MAX_TIMERS 8
typedef struct {
    int      used;
    uint16_t hwnd;
    uint16_t id;
    uint64_t period_ticks;   // in kernel timer ticks (g_timer_hz Hz)
    uint64_t next_fire;      // absolute tick of the next WM_TIMER
} win16_timer_t;
static win16_timer_t g_timers[WIN16_MAX_TIMERS];

// Kernel timing + keyboard, used by the GetMessage pump.
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;
extern int keyboard_has_char(void);
extern int keyboard_get_char(void);

// Window-frame geometry (kernel-style chrome).
#define W16_TITLEBAR 20
#define W16_BORDER   2

// GDI object table. Handles are small integers; objects are pens/brushes with a
// COLORREF. Stock objects use fixed handle ids >= 0x100, so app handles from
// win16_alloc_gdiobj must stay strictly below 0x100 (256). (#278 word6) The pool
// was 64, which Word 6 exhausts during startup (dozens of fonts/brushes/pens plus
// a compatible bitmap per toolbar/document back-buffer). Once exhausted,
// CreateCompatibleBitmap could not allocate, its memory DC stayed unbacked, and
// the toolbar/document blits copied black. Bumped to 256 (handles 1..255, all
// < 0x100 so still distinct from stock handles).
#define WIN16_MAX_GDIOBJ 256
typedef struct { int used; int type; uint32_t color; uint32_t *pix; int w, h, bpp; } win16_gdiobj_t;
// type: 1 = solid brush, 2 = pen
static win16_gdiobj_t g_gdiobj[WIN16_MAX_GDIOBJ];

#define STOCK_WHITE_BRUSH  0x100
#define STOCK_LTGRAY_BRUSH 0x101
#define STOCK_GRAY_BRUSH   0x102
#define STOCK_DKGRAY_BRUSH 0x103
#define STOCK_BLACK_BRUSH  0x104
#define STOCK_NULL_BRUSH   0x105
#define STOCK_BLACK_PEN    0x106
#define STOCK_WHITE_PEN    0x107
#define STOCK_NULL_PEN     0x108
// (#278 pass22) Stock FONT object handles. Win16 GetStockObject indices 10..16 are
// fonts (OEM_FIXED_FONT=10, ANSI_FIXED=11, ANSI_VAR=12, SYSTEM_FONT=13,
// DEVICE_DEFAULT_FONT=14, DEFAULT_PALETTE=15, SYSTEM_FIXED_FONT=16). Previously
// these fell through GetStockObject's default and returned a BRUSH handle (0x101),
// so when Word stored the "system font" handle and GetObject'd it, GetObject saw a
// non-font handle and returned 0 -> font-init gate (seg196:0x5f4) failed. Give each
// stock font a distinct handle (0x10a..0x110) that GetObject/SelectObject recognise.
#define STOCK_OEM_FIXED_FONT      0x10a
#define STOCK_ANSI_FIXED_FONT     0x10b
#define STOCK_ANSI_VAR_FONT       0x10c
#define STOCK_SYSTEM_FONT         0x10d
#define STOCK_DEVICE_DEFAULT_FONT 0x10e
#define STOCK_DEFAULT_PALETTE     0x10f
#define STOCK_SYSTEM_FIXED_FONT   0x110
// True for the stock FONT handles above (excludes 0x10f DEFAULT_PALETTE).
#define IS_STOCK_FONT(h) (((h) >= STOCK_OEM_FIXED_FONT && (h) <= STOCK_SYSTEM_FIXED_FONT) && (h) != STOCK_DEFAULT_PALETTE)

// Per-DC state. We model a single device context bound to a window's client area.
typedef struct {
    int      used;
    int      win;            // index into g_windows, or -1 for screen DC
    int      is_printer;     // (#278 P53 ACTION 3) 1 = printer/IC DC (CreateDC).
                             // Makes GetDeviceCaps report Letter page metrics so
                             // Word's paginate gets real physical page dimensions.
    uint32_t pen_color;
    uint32_t brush_color;
    int      brush_null;
    uint32_t text_color;
    uint32_t bk_color;
    int      bk_mode;        // 1=TRANSPARENT, else OPAQUE (Win16 default). OPAQUE
                             // fills each TextOut character cell with bk_color so
                             // redrawn text erases the old glyphs underneath (else
                             // numbers/labels overwrite themselves - SkiFree HUD).
    int      cur_x, cur_y;   // current pen position (client coords)
    // Memory DC backing (CreateCompatibleDC): drawing/BitBlt target a pixel
    // buffer instead of a window's client area. membuf is owned by the selected
    // compatible bitmap (g_gdiobj), not the DC, so it is not freed on ReleaseDC.
    uint32_t *membuf;        // 0 = not a memory DC
    int      mw, mh;         // memory bitmap dimensions
    // Currently-selected GDI objects, so SelectObject can return the PREVIOUS
    // handle (real Windows contract). Many apps (e.g. CARDS.DLL cdtDrawExt) do
    // `old = SelectObject(dc,obj); if (!old) bail;` and skip drawing if 0 comes
    // back, so these MUST start non-zero.
    uint16_t sel_pen, sel_brush, sel_bitmap, sel_font;
    // (#EP3 Fuji Golf) Mapping mode + window/viewport transform. Fuji Golf draws
    // its golf course (polygon coord lists from FUJIGOLF.DAT) in a logical
    // coordinate system and scales it to the client via MM_ANISOTROPIC +
    // SetWindowExt/SetViewportExt. dc_lp2dp() applies dp = (lp-worg)*vext/wext +
    // vorg. Defaults (org 0, ext 1) make it the identity, so every existing game
    // (all MM_TEXT) renders byte-identically.
    int      mapmode;              // 1 = MM_TEXT (default)
    int      wox, woy, wex, wey;   // window (logical) origin + extent
    int      vox, voy, vex, vey;   // viewport (device) origin + extent
} win16_dc_t;
// Non-zero sentinels for a freshly created DC's default bitmap/font, kept above
// the real gdiobj range (1..63) and the stock-object range (0x100..0x108) so a
// later SelectObject(dc, default) restore is a harmless no-op.
#define WIN16_DC_DEFBMP  0x200
#define WIN16_DC_DEFFONT 0x201
#define WIN16_MAX_DC 64  /* (#200 SkiFree) was 8: SkiFree holds ~5 cached sprite memory DCs + GetDC + BeginPaint at once; 8 slots (7 usable) ran out, BeginPaint returned HDC 0 -> ski2.c assert spam + blank slope. 64 < 0x100 stock-object base, so handles stay distinct. */
static win16_dc_t g_dcs[WIN16_MAX_DC];

// COLORREF (Win16: 0x00BBGGRR) -> framebuffer BGRA (FB_COLOR packs as B<<16|G<<8|R).
static uint32_t colorref_to_fb(uint32_t cr) {
    uint8_t r = (uint8_t)(cr & 0xFF);
    uint8_t g = (uint8_t)((cr >> 8) & 0xFF);
    uint8_t b = (uint8_t)((cr >> 16) & 0xFF);
    return FB_COLOR(r, g, b);
}

static uint32_t gdiobj_color(uint16_t h, uint32_t deflt) {
    switch (h) {
        case STOCK_WHITE_BRUSH:  return FB_COLOR(255,255,255);
        case STOCK_LTGRAY_BRUSH: return FB_COLOR(192,192,192);
        case STOCK_GRAY_BRUSH:   return FB_COLOR(128,128,128);
        case STOCK_DKGRAY_BRUSH: return FB_COLOR(64,64,64);
        case STOCK_BLACK_BRUSH:  return FB_COLOR(0,0,0);
        case STOCK_BLACK_PEN:    return FB_COLOR(0,0,0);
        case STOCK_WHITE_PEN:    return FB_COLOR(255,255,255);
        case STOCK_NULL_BRUSH:
        case STOCK_NULL_PEN:     return deflt;
        default: break;
    }
    if (h < WIN16_MAX_GDIOBJ && g_gdiobj[h].used)
        return colorref_to_fb(g_gdiobj[h].color);
    return deflt;
}

static int gdiobj_is_null(uint16_t h) {
    return (h == STOCK_NULL_BRUSH || h == STOCK_NULL_PEN);
}

// (#278 Word6 pass28) Windows 3.1 default system colours as COLORREF (0x00BBGGRR).
// Used by GetSysColor and by class-background resolution. Values match the classic
// Win3.1 grey/teal scheme so apps that build brushes from GetSysColor look right.
static uint32_t win16_syscolor(int idx) {
    switch (idx) {
        case 0:  return 0x00C0C0C0;  // COLOR_SCROLLBAR
        case 1:  return 0x00808080;  // COLOR_BACKGROUND (desktop)
        case 2:  return 0x00800000;  // COLOR_ACTIVECAPTION
        case 3:  return 0x00808080;  // COLOR_INACTIVECAPTION
        case 4:  return 0x00C0C0C0;  // COLOR_MENU
        case 5:  return 0x00FFFFFF;  // COLOR_WINDOW
        case 6:  return 0x00000000;  // COLOR_WINDOWFRAME
        case 7:  return 0x00000000;  // COLOR_MENUTEXT
        case 8:  return 0x00000000;  // COLOR_WINDOWTEXT
        case 9:  return 0x00FFFFFF;  // COLOR_CAPTIONTEXT
        case 10: return 0x00C0C0C0;  // COLOR_ACTIVEBORDER
        case 11: return 0x00C0C0C0;  // COLOR_INACTIVEBORDER
        case 12: return 0x00C0C0C0;  // COLOR_APPWORKSPACE
        case 13: return 0x00800000;  // COLOR_HIGHLIGHT
        case 14: return 0x00FFFFFF;  // COLOR_HIGHLIGHTTEXT
        case 15: return 0x00C0C0C0;  // COLOR_BTNFACE
        case 16: return 0x00808080;  // COLOR_BTNSHADOW
        case 17: return 0x00808080;  // COLOR_GRAYTEXT
        case 18: return 0x00000000;  // COLOR_BTNTEXT
        case 19: return 0x00000000;  // COLOR_INACTIVECAPTIONTEXT
        case 20: return 0x00FFFFFF;  // COLOR_BTNHIGHLIGHT
        default: return 0x00C0C0C0;
    }
}

// (#278 Word6 pass28) Resolve a window class background to an fb colour. Win16
// lets WNDCLASS.hbrBackground be EITHER a real brush handle OR a system-colour
// index encoded as (COLOR_xxx + 1) -- the classic idiom. Real Win16 brush
// handles are never small integers, but our win16_alloc_gdiobj returns small
// handles, so the two ranges collide. Word 6 registers its frame (OpusApp) and
// MDI workspace (OpusDesk) with hbrBackground = COLOR_APPWORKSPACE+1 (0x0d) and
// its document/edit windows with COLOR_WINDOW+1 (0x06); treating those as brush
// handles painted the whole UI black. For Word (segcount>=100) interpret a class
// background value in [1..21] as a system colour index. Games keep prior
// behaviour (gated) so they stay byte-identical.
static uint32_t resolve_class_bg(win16_window_t *win) {
    // (#278 P63) The Word 6 document view edit pane (wndproc seg222:0x1f6a) is the
    // white editing surface (COLOR_WINDOW). Its registered class background did not
    // resolve to a syscolor, so it fell to the generic 0xC0C0C0 grey default and
    // the pane painted grey; Word then only white-erased a ~531px sub-rect (its
    // GetUpdateRect/USER.190 update region), leaving the rest grey -> a
    // partial-width page. Fill the pane WHITE so the full client is the page, as
    // real Word does (its edit pane bg = the window colour). Word-only; games
    // (segcount<100) are byte-identical.
    if (g_info.segcount >= 100 && win->proc_seg == 0x06f7 && win->proc_off == 0x1f6a)
        return FB_COLOR(255,255,255);
    if (g_info.segcount >= 100 && win->bg_brush >= 1 && win->bg_brush <= 21)
        return colorref_to_fb(win16_syscolor((int)win->bg_brush - 1));
    return gdiobj_color(win->bg_brush, FB_COLOR(192,192,192));
}

static int win16_alloc_gdiobj(int type, uint32_t color) {
    int live = 0;
    for (int i = 1; i < WIN16_MAX_GDIOBJ; i++) {
        if (g_gdiobj[i].used) { live++; continue; }
        g_gdiobj[i].used = 1;
        g_gdiobj[i].type = type;
        g_gdiobj[i].color = color;
        return i;
    }
    W6LOG("[W6] GDIOBJ EXHAUSTED (all %d slots live) type=%d\n", WIN16_MAX_GDIOBJ - 1, type);
    (void)live;
    return 0;
}

static win16_window_t *win_from_hwnd(uint16_t hwnd) {
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].hwnd == hwnd) return &g_windows[i];
    return 0;
}

static int dc_alloc(int win) {
    for (int i = 1; i < WIN16_MAX_DC; i++) {
        if (!g_dcs[i].used) {
            g_dcs[i].used = 1;
            g_dcs[i].win = win;
            g_dcs[i].is_printer = 0;
            g_dcs[i].pen_color = FB_COLOR(0,0,0);
            g_dcs[i].brush_color = FB_COLOR(255,255,255);
            g_dcs[i].brush_null = 0;
            g_dcs[i].text_color = FB_COLOR(0,0,0);
            g_dcs[i].bk_color = FB_COLOR(255,255,255);   // Win16 default bk = white
            g_dcs[i].bk_mode = 2;                        // OPAQUE (Win16 default)
            g_dcs[i].cur_x = g_dcs[i].cur_y = 0;
            g_dcs[i].membuf = 0; g_dcs[i].mw = g_dcs[i].mh = 0;
            g_dcs[i].sel_pen = STOCK_BLACK_PEN;
            g_dcs[i].sel_brush = STOCK_WHITE_BRUSH;
            g_dcs[i].sel_bitmap = WIN16_DC_DEFBMP;
            g_dcs[i].sel_font = WIN16_DC_DEFFONT;
            g_dcs[i].mapmode = 1;                        // MM_TEXT
            g_dcs[i].wox = g_dcs[i].woy = 0;
            g_dcs[i].vox = g_dcs[i].voy = 0;
            g_dcs[i].wex = g_dcs[i].wey = 1;
            g_dcs[i].vex = g_dcs[i].vey = 1;
            return i;
        }
    }
    return 0;
}

// Read a NUL-terminated 16-bit far string into a C buffer.
static void rd_far_cstr(x86_16_cpu_t *c, uint16_t seg, uint16_t off,
                        char *out, int outsz) {
    int i = 0;
    for (; i < outsz - 1; i++) {
        uint8_t ch = x86_16_rd8(c, seg, (uint16_t)(off + i));
        if (ch == 0) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

// ---------------------------------------------------------------------------
// Message queue
// ---------------------------------------------------------------------------
static void msgq_post(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (g_msgq_count >= WIN16_MSGQ_SIZE) return;
    g_msgq[g_msgq_tail].hwnd = hwnd;
    g_msgq[g_msgq_tail].message = msg;
    g_msgq[g_msgq_tail].wParam = wp;
    g_msgq[g_msgq_tail].lParam = lp;
    g_msgq_tail = (g_msgq_tail + 1) % WIN16_MSGQ_SIZE;
    g_msgq_count++;
}
static int msgq_get(win16_msg_t *out) {
    if (g_msgq_count == 0) return 0;
    *out = g_msgq[g_msgq_head];
    g_msgq_head = (g_msgq_head + 1) % WIN16_MSGQ_SIZE;
    g_msgq_count--;
    return 1;
}
// (#200 SkiFree) Non-removing peek at the head message (for PeekMessage
// PM_NOREMOVE). Returns 1 and copies the head without dequeuing, else 0.
static int msgq_peek(win16_msg_t *out) {
    if (g_msgq_count == 0) return 0;
    *out = g_msgq[g_msgq_head];
    return 1;
}

// ---------------------------------------------------------------------------
// Window painting (kernel chrome + client fill). Drawn straight to the front
// buffer so the result is visible immediately and survives until the compositor
// (if any) repaints. cw/ch are recomputed here from the outer rect.
// ---------------------------------------------------------------------------
// ---- Win16 menu model (#152): RT_MENU template parse + menu-bar render ------
#define WIN16_MENUBAR_H   18
// (#278 Word6 pass15) Word builds menus dynamically (CreateMenu -> InsertMenu
// MF_POPUP to attach a child as a submenu -> GetSubMenu to recover the child
// handle -> InsertMenu items into the child). That needs many live menu handles
// and real per-popup child-handle tracking (sub_h), so the pool is enlarged and
// GetSubMenu returns the recorded child. Games (RT_MENU template menus) are
// unaffected: they never call GetSubMenu and sub_h defaults to 0.
#define WIN16_MAX_MENUS   64
#define WIN16_MAX_MITEMS  96
#define WIN16_MAX_TOP     32
typedef struct { char text[24]; uint16_t id; uint8_t is_popup; uint8_t is_sep; uint8_t level; uint16_t sub_h; } win16_mitem_t;
typedef struct {
    int used; int count; win16_mitem_t items[WIN16_MAX_MITEMS];
    int ntop; int top_idx[WIN16_MAX_TOP]; int top_x[WIN16_MAX_TOP]; int top_w[WIN16_MAX_TOP];
} win16_menu_t;
static win16_menu_t g_menus[WIN16_MAX_MENUS];

// Parse one menu level (recursive). MF_POPUP=0x10, MF_END=0x80, MF_SEPARATOR=0x800.
static uint32_t menu_parse_level(win16_menu_t *m, const uint8_t *t, uint32_t len,
                                 uint32_t p, int level) {
    while (p < len && m->count < WIN16_MAX_MITEMS) {
        if (p + 2 > len) break;
        uint16_t opt = (uint16_t)(t[p] | (t[p+1] << 8)); p += 2;
        int is_popup = (opt & 0x10) ? 1 : 0;
        uint16_t id = 0;
        if (!is_popup) { if (p + 2 > len) break; id = (uint16_t)(t[p] | (t[p+1] << 8)); p += 2; }
        char name[24]; int n = 0;
        while (p < len && t[p]) { if (n < 23) name[n++] = (char)t[p]; p++; }
        name[n] = 0;
        if (p < len) p++;            // skip the string NUL
        int idx = m->count++;
        win16_mitem_t *it = &m->items[idx];
        int k = 0; for (; name[k] && k < 23; k++) it->text[k] = name[k]; it->text[k] = 0;
        it->id = id; it->is_popup = (uint8_t)is_popup; it->level = (uint8_t)level;
        it->is_sep = (opt & 0x800) ? 1 : 0;
        if (level == 0 && m->ntop < WIN16_MAX_TOP) m->top_idx[m->ntop++] = idx;
        if (is_popup) p = menu_parse_level(m, t, len, p, level + 1);
        if (opt & 0x80) break;       // MF_END: last item at this level
    }
    return p;
}

// Parse an RT_MENU template into a g_menus slot; return hMenu (slot+1) or 0.
static uint16_t win16_parse_menu(const uint8_t *t, uint32_t len) {
    if (!t || len < 4) return 0;
    int slot = -1;
    for (int i = 0; i < WIN16_MAX_MENUS; i++) if (!g_menus[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;          // pool exhausted (>8 loads/run): reuse slot 0
    win16_menu_t *m = &g_menus[slot];
    m->used = 1; m->count = 0; m->ntop = 0;
    uint16_t ver = (uint16_t)(t[0] | (t[1] << 8));
    uint32_t p = 4;                  // skip MENUITEMTEMPLATEHEADER (version, offset)
    if (ver == 0) p = 4 + (uint32_t)(t[2] | (t[3] << 8));
    if (p >= len) p = 4;
    menu_parse_level(m, t, len, p, 0);
    if (m->ntop == 0) { m->used = 0; return 0; }
    return (uint16_t)(slot + 1);
}

// Draw the menu bar (top-level titles) into the host canvas, rows [0,MENUBAR_H).
static void win16_draw_menubar(win16_window_t *win) {
    if (!g_win16_canvas || !win->hmenu || win->hmenu > WIN16_MAX_MENUS) return;
    win16_menu_t *m = &g_menus[win->hmenu - 1];
    if (!m->used) return;
    uint32_t bar_bg = FB_COLOR(200,200,200);
    uint32_t ink    = FB_COLOR(20,20,20);
    uint32_t shadow = FB_COLOR(128,128,128);
    canvas_fill(0, 0, g_win16_canvas_w, WIN16_MENUBAR_H, bar_bg);
    for (int x = 0; x < g_win16_canvas_w; x++) canvas_plot(x, WIN16_MENUBAR_H - 1, shadow);
    int x = 8;
    for (int i = 0; i < m->ntop; i++) {
        win16_mitem_t *it = &m->items[m->top_idx[i]];
        int tx = x; int ty = (WIN16_MENUBAR_H - FONT_HEIGHT) / 2; if (ty < 0) ty = 1;
        for (const char *sp = it->text; *sp; sp++) {
            if (*sp == '&') continue;          // strip mnemonic ampersand
            const uint8_t *gph = font_get_glyph(*sp);
            if (gph) for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
                uint8_t bits = gph[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col)) canvas_plot(tx + col, ty + row, ink);
            }
            tx += FONT_WIDTH;
        }
        m->top_x[i] = x; m->top_w[i] = tx - x;
        x = tx + 16;
        if (x > g_win16_canvas_w - 8) break;
    }
}

static void win16_draw_frame(win16_window_t *win) {
    if (win->w <= 0 || win->h <= 0) return;
    // The window chrome (border + title bar) is drawn by the kernel window
    // manager around the host window; here we only manage the client canvas.
    // win->cx/cy are the window's client origin RELATIVE TO THE CANVAS (the host
    // window's content buffer), not screen coords.
    if (!g_win16_canvas) return;   // host window not created yet

    if (win->is_child) {
        // Child client sits inside the parent's client area at (win->x, win->y).
        win16_window_t *par = win_from_hwnd(win->parent);
        win->cx = (par ? par->cx : 0) + win->x;
        win->cy = (par ? par->cy : 0) + win->y;
        win->cw = win->w;
        win->ch = win->h;
        // (#209) On a soft idle repaint, do NOT clear the child client (e.g. the
        // TETRIS playfield) to its bg brush: the compositor may sample the host
        // canvas between this fill and the child's WM_PAINT, showing a grey flash.
        // Let the child's own WM_PAINT overdraw its persistent pixels.
        if (!g_win16_no_bg_fill) {
            uint32_t bg = resolve_class_bg(win);
            // (#219) During an app BeginPaint with a partial invalid region, erase
            // only that region so we never green over content outside it.
            if (g_win16_clip_active) {
                int fl = win->cx + g_win16_clip_l, ft = win->cy + g_win16_clip_t;
                int fw = g_win16_clip_r - g_win16_clip_l, fh = g_win16_clip_b - g_win16_clip_t;
                if (fw > 0 && fh > 0) canvas_fill(fl, ft, fw, fh, bg);
            } else {
                canvas_fill(win->cx, win->cy, win->cw, win->ch, bg);
            }
        }
        return;
    }

    // (#219) Only the MAIN app window (the one that owns the host canvas) maps to
    // the full canvas. Win3.x apps routinely create extra top-level windows that
    // are NOT the visible app surface: off-screen utility/DDE/clipboard windows
    // (e.g. Golf's hwnd 0041 at x=4588) and on-top popups. Treating each of those
    // as "the whole canvas" made their frame-draw bg-fill the ENTIRE canvas,
    // erasing the main window's freshly drawn content (Golf's dealt cards vanished
    // to green, TETRIS well went black). A secondary top-level window must paint
    // only its OWN rectangle within the canvas, so off-screen ones touch nothing.
    int is_main = (win->hwnd == g_win16_main_hwnd) || (g_win16_main_hwnd == 0);
    int mbar = (is_main && win->hmenu) ? WIN16_MENUBAR_H : 0;
    if (is_main) {
        // The client area is the whole canvas, minus a menu-bar row when the
        // window has a menu (#152) so apps lay out below the bar and
        // GetClientRect/WM_SIZE report the reduced client (per Win16 USER semantics).
        win->cx = 0;
        win->cy = mbar;
        win->cw = g_win16_canvas_w;
        win->ch = g_win16_canvas_h - mbar;
    } else {
        // Secondary top-level: client rect = the window's own geometry, clipped to
        // the canvas. Off-screen windows end up with a zero-area visible rect.
        win->cx = win->x;
        win->cy = win->y;
        win->cw = win->w;
        win->ch = win->h;
    }
    if (win->ch < 0) win->ch = 0;
    uint32_t bg = resolve_class_bg(win);
    // (#209) On a soft idle repaint we recompute the client rect (above) but do
    // NOT wipe the canvas to the bg brush; the app's WM_PAINT redraws its own
    // pixels over the persistent buffer, so the playfield does not flash. The
    // menu bar is still refreshed so its chrome stays correct.
    // (#bugB) When the main frame carries a menu bar it draws itself (Word 6, which
    // reports hmenu==0), preserve that top menu strip through background erases so a
    // scroll repaint does not blank the "File Edit View ..." row. Apps whose menu we
    // track (hmenu!=0) have it redrawn by win16_draw_menubar, so strip=0 for them.
    int menu_strip = (is_main && win->hmenu == 0) ? win16_menu_strip_h(win->hwnd) : 0;
    if (!g_win16_no_bg_fill) {
        if (g_win16_clip_active) {       // (#219) partial app-paint: erase only the invalid rect
            int fl = win->cx + g_win16_clip_l, ft = win->cy + g_win16_clip_t;
            int fw = g_win16_clip_r - g_win16_clip_l, fh = g_win16_clip_b - g_win16_clip_t;
            // (#bugB) keep the chrome children intact even when the app invalidates a
            // region that overlaps them.
            if (fw > 0 && fh > 0) {
                if (is_main) canvas_fill_excl_children_top(fl, ft, fw, fh, bg, win->hwnd, menu_strip);
                else canvas_fill(fl, ft, fw, fh, bg);
            }
        } else if (is_main) {
            // (#bugB) Erase the whole client EXCEPT the chrome child windows
            // (toolbar/ruler/status) AND the top menu strip the frame owns. Painting
            // over them (the frame bg brush is white) blanked the whole window on a
            // scroll repaint; excluding the child rectangles fixed the toolbar/ruler/
            // status, and excluding the top menu strip keeps the "File Edit View" row.
            canvas_fill_excl_children_top(0, 0, g_win16_canvas_w, g_win16_canvas_h, bg, win->hwnd, menu_strip);
        } else {
            // Only fill this secondary window's own (clamped) rect.
            canvas_fill(win->cx, win->cy, win->cw, win->ch, bg);
        }
    }
    if (mbar) win16_draw_menubar(win);
}

// Clip a client-area pixel and plot it.
// (#EP3 Fuji Golf) Logical -> device point transform for the current mapping mode.
// dp = (lp - window_org) * viewport_ext / window_ext + viewport_org. For the
// default (MM_TEXT: org 0, ext 1) this is the identity, so non-mapping apps are
// unaffected. Guards against a zero window extent.
static void dc_lp2dp(win16_dc_t *dc, int *x, int *y) {
    int wex = dc->wex ? dc->wex : 1;
    int wey = dc->wey ? dc->wey : 1;
    long lx = (long)(*x - dc->wox) * dc->vex;
    long ly = (long)(*y - dc->woy) * dc->vey;
    *x = dc->vox + (int)(lx / wex);
    *y = dc->voy + (int)(ly / wey);
}

static void dc_plot(win16_dc_t *dc, int cx, int cy, uint32_t fbcolor) {
    // Memory DC: plot into its bitmap buffer.
    if (dc->membuf) {
        if (cx < 0 || cy < 0 || cx >= dc->mw || cy >= dc->mh) return;
        dc->membuf[cy * dc->mw + cx] = fbcolor;
        return;
    }
    if (dc->win < 0) return;
    win16_window_t *win = &g_windows[dc->win];
    if (cx < 0 || cy < 0 || cx >= win->cw || cy >= win->ch) return;
    // Plot into the host window canvas at this window's client origin. NOTE: we do
    // NOT clip the parent's dc_plot to exclude child rectangles here: Word 6 draws
    // its menu-bar strip through the frame's DC at the very top of the client, which
    // overlaps its top command-bar child window, so clipping it would erase the menu.
    // The frame never fills its document body white via dc_plot (proven by trace),
    // so the whole-window-white regression is handled purely by clipping the frame's
    // background *erase* (canvas_fill_excl_children in win16_draw_frame), not here.
    canvas_plot(win->cx + cx, win->cy + cy, fbcolor);
}

// A handler fills AX/DX (the 32-bit return DX:AX) and *argbytes (bytes to pop).
typedef void (*win16_handler_fn)(x86_16_cpu_t *c, uint16_t *ax,
                                 uint16_t *dx, uint16_t *argbytes);

// ===========================================================================
// KERNEL handlers
// ===========================================================================

// InitTask (KERNEL.91). No stack args. Returns the Win16 task-startup contract:
//   AX = 1            (success; nonzero clears the "init failed" path)
//   CX = stack size   (top-of-stack offset in the autodata segment)
//   DX = nCmdShow     (1 = SW_SHOWNORMAL)
//   ES:BX -> command line (empty NUL string)
//   DI = hInstance    (autodata segment paragraph)
//   SI = hPrevInstance(0)
//   BP is conventionally set so the C startup can chain frames; we leave it.
// The C runtime (libentry/__astartup) reads these and proceeds to RegisterClass
// /CreateWindow when AX != 0.
static void k_inittask(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    *ax = 1;                       // success
    // (#278 pass21) CX = STACK LIMIT (bottom offset), NOT the stack top. The MS-C
    // __astart does `add cx,0x100; jc <exit>` as a stack-headroom check and stores
    // cx as the _STKHQQ low-bound watermark. Returning the stack TOP (sp, =0xFFFE
    // after pass-20 put the stack at the top of DGROUP) overflowed that add, so
    // __astart took the failure/exit path immediately and never ran WinMain (it
    // span re-calling InitTask, filling the stack until a wild far-return). The
    // faithful value is the stack bottom = sp - ne_stack (clamped so the headroom
    // check cannot overflow). Pre-pass-20, sp was small enough not to overflow,
    // which is why this latent bug only surfaced with the full-64KB DGROUP.
    { uint16_t sp = g_info.sp;
      uint16_t nst = g_info.ne_stack;
      uint16_t bottom = (sp > nst && nst != 0) ? (uint16_t)(sp - nst) : (uint16_t)(sp > 0x1000 ? sp - 0x1000 : 0x0100);
      if (bottom > 0xFE00) bottom = 0xFE00;   // guarantee `add cx,0x100` never carries
      if (bottom < 0x0100) bottom = 0x0100;
      c->cx = bottom; }
    *dx   = 1;                     // nCmdShow = SW_SHOWNORMAL
    c->es = g_info.cmdline_seg;
    c->bx = g_info.cmdline_off;    // ES:BX -> empty command line
    c->di = g_info.hinstance;
    c->si = g_info.hprev;          // 0
    *argbytes = 0;
}

// WaitEvent (KERNEL.30): WaitEvent(htask). No real scheduler; just return.
static void k_waitevent(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;   // 1 word arg (HTASK)
}

// GetVersion (KERNEL.3): no args. LOBYTE(AX)=major, HIBYTE(AX)=minor.
// Report Windows 3.10: AL=3, AH=10 -> 0x0A03.
static void k_getversion(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    (void)c; *ax = 0x0A03; *dx = 0; *argbytes = 0;
}

// LocalInit (KERNEL.4): LocalInit(segment, start, end). Inits a local heap in
// the given segment. Return nonzero = success. 3 word args.
static void k_localinit(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    // LocalInit(WORD wSegment, WORD pStart, WORD pEnd). Pascal: a0=last pushed.
    // Push order is (wSegment, pStart, pEnd), so a0=pEnd, a1=pStart, a2=wSegment.
    // Set the near-heap window so subsequent LocalAlloc in this segment succeeds.
    // (#289 b466) STORAGE.DLL LocalInits a heap inside each GlobalAlloc'd object
    // segment; without honoring start/end our bump pointer kept the APP's DGROUP
    // window and the first LocalAlloc in a fresh STORAGE segment failed (->0),
    // leaving a NULL interface object and a null far-call crash.
    uint16_t pend   = arg16(c, 0);
    uint16_t pstart = arg16(c, 1);
    uint16_t wseg   = arg16(c, 2);
    if (pend == 0) pend = 0xFFFE;          // "to end of segment" convention
    if (pstart < 2) pstart = 2;            // keep offset 0 reserved (NULL)
    // wSegment==0 means "current DS" in Win16 LocalInit.
    uint16_t tgt = wseg ? wseg : (c ? c->ds : 0);
    // (#278 Word6 pass24 NOTE) A LocalInit by a no-DGROUP DLL's LibMain (e.g. SDM.DLL,
    // autodata=0, running with DS = the app's DGROUP) sets the SHARED fallback
    // g_lheap_next to a tiny low window [2,0x800). For LARGE apps the main DGROUP has
    // its OWN dedicated lseg arena (registered in win16_api_begin), so its LocalAllocs
    // are immune to that fallback clobber; this routine is otherwise unchanged.
    if (pend > pstart) {
        lseg_t *e = lseg_get_or_add(tgt);
        if (e) { e->next = pstart; e->top = pend; }
        // keep the app-DGROUP fallback in sync if this is the current DS
        if (c && tgt == c->ds) { g_lheap_next = pstart; g_lheap_top = pend; }
    }
    *ax = 1; *dx = 0; *argbytes = 6;
}

// ===========================================================================
// (#133) Win3.x profile (.INI) engine - real file-backed Get/WriteProfile* and
// Get/WritePrivateProfile*. Reads/writes actual .ini files via the routed FS
// (fat_* wrappers -> ext2 root) using the #257 drive-letter translation. WIN.INI
// lives at C:\WINDOWS\WIN.INI; a private profile name with no path defaults to
// the Windows directory.
// ===========================================================================
static char pf_up(char ch) { return (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch; }

// Resolve an .ini name to a native path. A bare name (no drive/slash) defaults
// to C:\WINDOWS; anything with a drive or slash resolves as given (#257).
static void ini_resolve(const char *name, char *out, int outsz) {
    int has = 0;
    for (const char *p = name; *p; p++) if (*p == ':' || *p == '\\' || *p == '/') { has = 1; break; }
    if (has) { dos_resolve_path(name, 0, out, outsz); return; }
    char tmp[160]; int n = 0;
    const char *pre = "C:\\WINDOWS\\";
    for (const char *p = pre;  *p && n < 159; p++) tmp[n++] = *p;
    for (const char *p = name; *p && n < 159; p++) tmp[n++] = *p;
    tmp[n] = 0;
    dos_resolve_path(tmp, 0, out, outsz);
}

// Load an .ini into a NUL-terminated host buffer (kmalloc; caller kfree). NULL if absent.
static char *ini_load(const char *name, uint32_t *out_len) {
    char path[160]; ini_resolve(name, path, sizeof(path));
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    char *txt = (char *)kmalloc(sz + 1);
    if (!txt) { if (data) kfree(data); return 0; }
    if (data) { memcpy(txt, data, sz); kfree(data); }
    txt[sz] = 0;
    if (out_len) *out_len = sz;
    return data ? txt : (kfree(txt), (char *)0);
}

// Case-insensitive compare of a token (text[..] up to a terminator) against name.
// Returns 1 if name equals the token (token = text until one of term chars).
static int ini_token_eq(const char *text, const char *name, const char *term) {
    while (*text && !strchr(term, *text)) {
        if (!*name) return 0;
        if (pf_up(*text) != pf_up(*name)) return 0;
        text++; name++;
    }
    return *name == 0;
}

// Find [section]key= in NUL-terminated text; copy value (trimmed) to val. 1 if found.
static int ini_find(const char *t, const char *section, const char *key, char *val, int cap) {
    int in_sec = 0;
    while (*t) {
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') {
            in_sec = section && ini_token_eq(t + 1, section, "]\r\n");
        } else if (in_sec && *t && *t != ';' && *t != '\r' && *t != '\n') {
            // (#188 Word6) SETUP.LST keys have spaces before '=' ("TmpDirName   ="),
            // so whitespace must terminate the key token too, not just '='.
            if (key && ini_token_eq(t, key, " \t=\r\n")) {
                while (*t && *t != '=' && *t != '\n') t++;
                if (*t == '=') {
                    t++;
                    while (*t == ' ' || *t == '\t') t++;
                    int i = 0;
                    while (*t && *t != '\r' && *t != '\n' && i < cap - 1) val[i++] = *t++;
                    while (i > 0 && (val[i-1] == ' ' || val[i-1] == '\t')) i--;
                    if (cap > 0) val[i] = 0;
                    return 1;
                }
            }
        }
        while (*t && *t != '\n') t++;     // next line
        if (*t) t++;
    }
    return 0;
}

// Read a string profile value into a host buffer; falls back to def. Returns length.
static int ini_get_string(const char *file, const char *section, const char *key,
                          const char *def, char *out, int cap) {
    char val[256];
    uint32_t len = 0;
    char *txt = ini_load(file, &len);
    int found = txt ? ini_find(txt, section, key, val, sizeof(val)) : 0;
    if (txt) kfree(txt);
    const char *src = found ? val : (def ? def : "");
    int i = 0;
    for (; src[i] && i < cap - 1; i++) out[i] = src[i];
    if (cap > 0) out[i] = 0;
    return i;
}

// (#188 Word6) GetPrivateProfileString with a NULL/empty key returns ALL key names
// in the section, each NUL-terminated, the whole list double-NUL terminated. Word's
// SETUP uses this to enumerate the [Files] section. Returns the number of bytes
// written to out (NOT counting the final extra NUL), like Win16.
static int ini_enum_keys(const char *file, const char *section, char *out, int cap) {
    uint32_t len = 0;
    char *txt = ini_load(file, &len);
    int o = 0;
    if (txt && cap > 2) {
        const char *t = txt; int in_sec = 0;
        while (*t) {
            const char *ls = t;
            while (*ls == ' ' || *ls == '\t') ls++;
            if (*ls == '[') {
                in_sec = ini_token_eq(ls + 1, section, "]\r\n");
            } else if (in_sec && *ls && *ls != ';' && *ls != '\r' && *ls != '\n') {
                // copy the key name (up to whitespace or '=')
                const char *k = ls;
                while (*k && *k != ' ' && *k != '\t' && *k != '=' && *k != '\r' && *k != '\n') {
                    if (o < cap - 2) out[o++] = *k;
                    k++;
                }
                if (o < cap - 2) out[o++] = 0;   // terminate this key name
            }
            while (*t && *t != '\n') t++;
            if (*t) t++;
        }
    }
    if (txt) kfree(txt);
    if (o < cap - 1) out[o] = 0;   // final double-NUL
    return o;
}

static int pf_atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

// Write/replace/delete [section]key=value in an .ini file (value NULL = delete key).
static int ini_set_string(const char *file, const char *section, const char *key,
                          const char *value) {
    char path[160]; ini_resolve(file, path, sizeof(path));
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    char *old = (char *)kmalloc(sz + 1);
    if (!old) { if (data) kfree(data); return 0; }
    if (data) { memcpy(old, data, sz); kfree(data); }
    old[sz] = 0;
    // Output buffer: old + room for a new section + line.
    int klen = 0; while (key && key[klen]) klen++;
    int vlen = 0; while (value && value[vlen]) vlen++;
    int slen = 0; while (section && section[slen]) slen++;
    char *outb = (char *)kmalloc(sz + slen + klen + vlen + 16);
    if (!outb) { kfree(old); return 0; }
    int o = 0;
    const char *t = old;
    int in_sec = 0, wrote = 0, saw_sec = 0;
    while (*t) {
        const char *line = t;
        while (*t && *t != '\n') t++;
        int linelen = (int)(t - line);
        if (*t == '\n') t++;
        // classify this line
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            // leaving the target section without having written -> insert before next section
            if (in_sec && !wrote && key && value) {
                for (int i = 0; key[i]; i++) outb[o++] = key[i];
                outb[o++] = '=';
                for (int i = 0; value[i]; i++) outb[o++] = value[i];
                outb[o++] = '\r'; outb[o++] = '\n';
                wrote = 1;
            }
            in_sec = section && ini_token_eq(p + 1, section, "]\r\n");
            if (in_sec) saw_sec = 1;
        } else if (in_sec && key && ini_token_eq(p, key, "=\r\n")) {
            // existing key line: replace (or drop if deleting)
            if (value) {
                for (int i = 0; key[i]; i++) outb[o++] = key[i];
                outb[o++] = '=';
                for (int i = 0; value[i]; i++) outb[o++] = value[i];
                outb[o++] = '\r'; outb[o++] = '\n';
            }
            wrote = 1;
            continue;   // skip copying the old line
        }
        for (int i = 0; i < linelen; i++) outb[o++] = line[i];
        outb[o++] = '\n';
    }
    // section was the last one and key not yet written
    if (in_sec && !wrote && key && value) {
        for (int i = 0; key[i]; i++) outb[o++] = key[i];
        outb[o++] = '=';
        for (int i = 0; value[i]; i++) outb[o++] = value[i];
        outb[o++] = '\r'; outb[o++] = '\n';
        wrote = 1;
    }
    // section never existed: append it
    if (!saw_sec && section && key && value) {
        if (o > 0 && outb[o-1] != '\n') outb[o++] = '\n';
        outb[o++] = '[';
        for (int i = 0; section[i]; i++) outb[o++] = section[i];
        outb[o++] = ']'; outb[o++] = '\r'; outb[o++] = '\n';
        for (int i = 0; key[i]; i++) outb[o++] = key[i];
        outb[o++] = '=';
        for (int i = 0; value[i]; i++) outb[o++] = value[i];
        outb[o++] = '\r'; outb[o++] = '\n';
    }
    int ok = (fat_write_file(&g_fat_fs, path, outb, (uint32_t)o) >= 0);
    kfree(old); kfree(outb);
    return ok;
}

// GetProfileInt (KERNEL.57): GetProfileInt(lpSection, lpKey, nDefault).
// far,far,int = 2+2+1 words = 10 bytes. Reads C:\WINDOWS\WIN.INI.
static void k_getprofileint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    int def = (int16_t)arg16(c, 0);
    uint16_t key_off = arg16(c, 1), key_seg = arg16(c, 2);
    uint16_t sec_off = arg16(c, 3), sec_seg = arg16(c, 4);
    char sec[64], key[64], val[64];
    rd_far_cstr(c, sec_seg, sec_off, sec, sizeof(sec));
    rd_far_cstr(c, key_seg, key_off, key, sizeof(key));
    *ax = (uint16_t)(ini_get_string("WIN.INI", sec, key, "", val, sizeof(val))
                     ? pf_atoi(val) : def);
    *dx = 0; *argbytes = 10;
}

// GetProfileString (KERNEL.58): (lpSec,lpKey,lpDef,lpRet,cbRet) = 9 words = 18 bytes.
// Reads C:\WINDOWS\WIN.INI; falls back to lpDefault.
static void k_getprofilestr(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t cb       = arg16(c, 0);
    uint16_t ret_off  = arg16(c, 1), ret_seg = arg16(c, 2);
    uint16_t def_off  = arg16(c, 3), def_seg = arg16(c, 4);
    uint16_t key_off  = arg16(c, 5), key_seg = arg16(c, 6);
    uint16_t sec_off  = arg16(c, 7), sec_seg = arg16(c, 8);
    char sec[64], key[64], def[256], outb[256];
    rd_far_cstr(c, sec_seg, sec_off, sec, sizeof(sec));
    rd_far_cstr(c, key_seg, key_off, key, sizeof(key));
    rd_far_cstr(c, def_seg, def_off, def, sizeof(def));
    int n = ini_get_string("WIN.INI", sec, key, def, outb,
                           (int)(cb < sizeof(outb) ? cb : sizeof(outb)));
    uint16_t i = 0;
    if (cb > 0) {
        for (; i < (uint16_t)n && i < (uint16_t)(cb - 1); i++)
            x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), (uint8_t)outb[i]);
        x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), 0);
    }
    { extern int g_w6life, g_w6seq;
      if (g_w6life) { static int npf=0; if (npf<30) { npf++;
        kprintf("[W6PROFSTR] SEQ %d: GetProfileString(sec=\"%s\" key=\"%s\" def=\"%s\") -> \"%s\"\n",
                g_w6seq++, sec, key, def, outb); } } }
    *ax = i; *dx = 0; *argbytes = 18;
}

// WriteProfileString (KERNEL.59): (lpSection,lpKey,lpString) = 3 fars = 6w = 12b.
// Writes to C:\WINDOWS\WIN.INI (lpString NULL deletes the key).
static void k_writeprofilestr(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                              uint16_t *argbytes) {
    uint16_t str_off = arg16(c, 0), str_seg = arg16(c, 1);
    uint16_t key_off = arg16(c, 2), key_seg = arg16(c, 3);
    uint16_t sec_off = arg16(c, 4), sec_seg = arg16(c, 5);
    char sec[64], key[64], str[256];
    rd_far_cstr(c, sec_seg, sec_off, sec, sizeof(sec));
    rd_far_cstr(c, key_seg, key_off, key, sizeof(key));
    int has_str = (str_seg || str_off);
    if (has_str) rd_far_cstr(c, str_seg, str_off, str, sizeof(str));
    *ax = (uint16_t)ini_set_string("WIN.INI", sec, key, has_str ? str : 0);
    *dx = 0; *argbytes = 12;
}

// (#188 Word6) DOS-form path of the running module ("A:\\SETUP.EXE"), set by the
// NE loader. GetModuleFileName returns this; many installers (Word SETUP) derive
// their source directory from it, then append SETUP.LST / *.INF, so it MUST be a
// real DOS path with drive letter + back-slashes, not empty.
static char g_module_dospath[128] = "";
// Convert a native MayteraOS path into the closest DOS path. /WINDIR/DRIVE_X/foo/bar
// -> X:\\foo\\bar ; a bare /foo/bar -> C:\\foo\\bar. Forward slashes become '\\'.
void win16_set_dos_module_path(const char *native) {
    g_module_dospath[0] = '\0';
    if (!native) return;
    char drv = 'C'; const char *rest = native;
    // Recognize /WINDIR/DRIVE_X/...
    const char *p = native;
    if (p[0]=='/') p++;
    if ((p[0]=='W'||p[0]=='w') && (p[1]=='I'||p[1]=='i') && (p[2]=='N'||p[2]=='n') &&
        (p[3]=='D'||p[3]=='d') && (p[4]=='I'||p[4]=='i') && (p[5]=='R'||p[5]=='r') &&
        p[6]=='/' ) {
        const char *q = p + 7;   // after WINDIR/
        if ((q[0]=='D'||q[0]=='d') && (q[1]=='R'||q[1]=='r') && (q[2]=='I'||q[2]=='i') &&
            (q[3]=='V'||q[3]=='v') && (q[4]=='E'||q[4]=='e') && q[5]=='_' && q[6] && q[7]=='/') {
            drv = q[6];
            if (drv >= 'a' && drv <= 'z') drv = (char)(drv - 32);
            rest = q + 8;        // after DRIVE_X/
        } else { rest = q; }
    } else {
        rest = native[0]=='/' ? native + 1 : native;
    }
    int o = 0;
    g_module_dospath[o++] = drv; g_module_dospath[o++] = ':'; g_module_dospath[o++] = '\\';
    for (int i = 0; rest[i] && o < (int)sizeof(g_module_dospath) - 1; i++)
        g_module_dospath[o++] = (rest[i] == '/') ? '\\' : rest[i];
    g_module_dospath[o] = '\0';
}

// GetModuleHandle (KERNEL.47): GetModuleHandle(lpModuleName) far ptr = 2 words.
// Return the app module handle for any query (single-module world).
static void k_getmodulehandle(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                              uint16_t *argbytes) {
    (void)c; *ax = g_info.module_handle; *dx = 0; *argbytes = 4;
}

// GetModuleFileName (KERNEL.49): (hModule, lpFileName far, nSize) = 1+2+1 words.
// Write an empty string, return 0 length.
static void k_getmodulefilename(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                uint16_t *argbytes) {
    // Pascal signature GetModuleFileName(HMODULE, LPSTR, int): the LAST pushed arg
    // (nearest the top, index 0) is nSize; then lpFilename (off,seg); then hModule.
    uint16_t n        = arg16(c, 0);   // nSize
    uint16_t name_off = arg16(c, 1);
    uint16_t name_seg = arg16(c, 2);
    /* hModule = arg16(c, 3) */
    // (#188 Word6) Copy the DOS-form module path; installers parse this to locate
    // their source directory. Truncate to the caller's buffer size.
    uint16_t i = 0;
    for (; g_module_dospath[i] && i + 1 < n; i++)
        x86_16_wr8(c, name_seg, (uint16_t)(name_off + i), (uint8_t)g_module_dospath[i]);
    if (n > 0) x86_16_wr8(c, name_seg, (uint16_t)(name_off + i), 0);
    *ax = i; *dx = 0; *argbytes = 8;
}

// GetModuleUsage (KERNEL.48): GetModuleUsage(hModule) = 1 word. Return 1.
static void k_getmoduleusage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 2;
}

// Lock/UnlockSegment (KERNEL.23/24): 1 word arg, no-op, return the arg (selector).
static void k_locksegment(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    *ax = arg16(c, 0); *dx = 0; *argbytes = 2;
}

// GlobalAlloc (KERNEL.15): GlobalAlloc(wFlags, dwBytes) = int + dword = 3 words.
//   Stack top->bottom: dwBytes hi, dwBytes lo, wFlags.
// Return a handle (block segment paragraph) in AX.
static void k_globalalloc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    // (#188 Word6) GlobalAlloc(UINT wFlags, DWORD dwBytes). Pascal pushes args
    // left-to-right and a DWORD as {high word, low word} so the LOW word ends up
    // nearest the top: index0 = dwBytes LOW, index1 = dwBytes HIGH, index2 = wFlags.
    // (Was reading these swapped -> a small request like 0x1000 became 0x10000000,
    // which truncated to 0 paragraphs => a 16-byte block => caller buffer overflow,
    // breaking Word6 SETUP with "initialization file corrupted".)
    uint16_t bytes_lo = arg16(c, 0);
    uint16_t bytes_hi = arg16(c, 1);
    /* wFlags = arg16(c, 2) ignored */
    uint32_t bytes = ((uint32_t)bytes_hi << 16) | bytes_lo;
    uint16_t h = heap_alloc(bytes);
    gblk_record(h, bytes);   // (#188) track real size for GlobalSize
    win16_trace("GlobalAlloc(%lu) -> h=%04x\n", (unsigned long)bytes, h);
    *ax = h; *dx = 0; *argbytes = 6;
}

// GlobalLock (KERNEL.18): GlobalLock(hMem) = 1 word. Return far ptr handle:0000.
//   AX = offset (0), DX = segment (the handle). DX:AX = far pointer.
static void k_globallock(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    *ax = 0; *dx = h; *argbytes = 2;
}

// GlobalUnlock (KERNEL.19): GlobalUnlock(hMem) = 1 word. Return 0 (lock count 0).
static void k_globalunlock(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// GlobalFree (KERNEL.17): GlobalFree(hMem) = 1 word. Return 0 = success.
static void k_globalfree(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    heap_free(arg16(c, 0));
    *ax = 0; *dx = 0; *argbytes = 2;
}

// GlobalCompact (KERNEL.25): GlobalCompact(DWORD dwMinFree) = dword = 4 bytes.
// Returns the size (DWORD in DX:AX) of the largest free contiguous block. Win16
// installers (Word6 SETUP) call this to verify ~900KB is free before continuing.
// Our segmented heap can only physically offer a few hundred KB within the 1MB
// real-mode space, but the check is purely a comparison, so report a generous
// large value (16 MB) so memory-gated installers proceed. (#188 Word6)
static void k_globalcompact(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    (void)c;
    uint32_t free_bytes = 16u * 1024u * 1024u;   // report 16 MB available
    *ax = (uint16_t)(free_bytes & 0xFFFF);
    *dx = (uint16_t)(free_bytes >> 16);
    *argbytes = 4;
}

// GetFreeSpace (KERNEL.169): DWORD GetFreeSpace(UINT wFlags) = 1 word = 2 bytes.
// Returns the number of free bytes in the global heap. Fuji Golf (EP3) queries
// this in WinMain and aborts with a "Not enough memory ... run Fuji Golf"
// MessageBox when it reads too small a value; the previously-UNIMPL path returned
// 0 and tripped that check. Report a generous 16 MB (matching GlobalCompact) so
// memory-gated apps proceed. Additive (a formerly-unimplemented ordinal): no
// existing game calls it, so their behaviour is byte-identical. (#EP3 Fuji Golf)
static void k_getfreespace(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    (void)c;
    uint32_t free_bytes = 16u * 1024u * 1024u;   // 16 MB available
    *ax = (uint16_t)(free_bytes & 0xFFFF);
    *dx = (uint16_t)(free_bytes >> 16);
    *argbytes = 2;
}

// GetWinFlags (KERNEL.132): DWORD GetWinFlags(void) = no args. Real Windows never
// returns 0 here; a 0 tells a 1992 app it is in Windows "real mode" with <1MB.
// Report a protected-mode 386 standard-mode environment with a math coprocessor
// (WF_PMODE|WF_CPU386|WF_STANDARD|WF_80x87) matching the interpreter's actual
// protected-mode segmented model. Enhanced-mode flag intentionally NOT set. (#Excel4)
static void k_getwinflags(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    (void)c;
    uint32_t f = 0x0001u | 0x0004u | 0x0010u | 0x0400u;
    *ax = (uint16_t)(f & 0xFFFF);
    *dx = (uint16_t)(f >> 16);
    *argbytes = 0;
}

// LocalAlloc (KERNEL.5): LocalAlloc(wFlags, wBytes) = 2 words. Returns a NEAR
// handle == near offset into the app's DGROUP local heap. For LMEM_FIXED the
// handle IS the near pointer, and the MS-C startup (and SkiFree, #200) use the
// result directly as a near pointer without calling LocalLock. If LMEM_ZEROINIT
// (flag bit 0x40) is set, zero the block. Returns 0 if the local heap is full.
static void k_localalloc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t flags = arg16(c, 1);
    uint16_t bytes = arg16(c, 0);
    uint16_t off = lheap_alloc(bytes);
    if (off && (flags & 0x0040)) {     // LMEM_ZEROINIT
        for (uint16_t i = 0; i < bytes; i++) x86_16_wr8(c, c->ds, (uint16_t)(off + i), 0);
    }
    *ax = off; *dx = 0; *argbytes = 4;
}

// LocalReAlloc (KERNEL.6): (hMem, wBytes, wFlags) = 3 words. Bump-allocate a new
// block of the requested size (data not copied; adequate for startup tables).
static void k_localrealloc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t bytes = arg16(c, 1);
    *ax = lheap_alloc(bytes); *dx = 0; *argbytes = 6;
}

// LocalLock (KERNEL.8): LocalLock(hMem) = 1 word. The handle already IS the near
// offset, so return it unchanged. SelectObject-style callers expect a non-zero
// near pointer they can dereference.
static void k_locallock(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    *ax = arg16(c, 0); *dx = 0; *argbytes = 2;
}

static void k_localunlock(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 2;
}

// LocalFree (KERNEL.7): LocalFree(hMem) = 1 word. Bump allocator does not
// reclaim; return 0 (NULL handle) to signal success.
static void k_localfree(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// LocalSize (KERNEL.9): LocalSize(hMem) = 1 word. We do not track per-block
// sizes; return a small non-zero size so callers proceed.
static void k_localsize(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// GlobalReAlloc (KERNEL.16): (hMem, dwBytes, wFlags) = 1+2+1 = 4w = 8 bytes.
// Allocate a fresh block of the new size (data not copied; fine for discovery).
static void k_globalrealloc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    // (#278 Word6) Pascal stack (pushed left-to-right): hMem(1w)=arg0,
    // dwBytes(DWORD: LOW word=arg1, HIGH word=arg2), wFlags(1w)=arg3. The prior
    // code read hi=arg1/lo=arg2 (WORDS SWAPPED) so a request for e.g. 0x19f0 bytes
    // was computed as 0x19f00000 (~435 MB) -> heap_alloc fails -> AX=0. WINWORD
    // startup loops  shrinking
    // n until it SUCCEEDS, so the swap made it spin forever. Correct DWORD order:
    uint16_t lo = arg16(c, 1), hi = arg16(c, 2);
    uint32_t bytes = ((uint32_t)hi << 16) | lo;
    uint16_t h = heap_alloc(bytes ? bytes : 16);
    *ax = h; *dx = 0; *argbytes = 8;
}

// GlobalHandle (KERNEL.21): HGLOBAL GlobalHandle(WORD wSel) = 1 word. Given a
// memory selector/segment, return its global handle. In our model the handle IS
// the segment, so return DX:AX = handle:selector (both = the input). (#289 OLE2:
// COMPOBJ's IMalloc uses this to validate task-memory blocks.)
static void k_globalhandle(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t sel = arg16(c, 0);
    *ax = sel; *dx = sel; *argbytes = 2;
}

// KERNEL.334 = a far-pointer VALIDITY check, IsBadReadPtr-style:
//   BOOL IsBad(FARPTR lp, WORD cb)  -> 0 (FALSE) if [lp, lp+cb) is readable,
//                                      nonzero (TRUE) if the pointer is BAD.
// (#278 b553) Re-RE'd from how BOTH callers use it. The Pascal call pushes THREE
// words = a far pointer (seg pushed first/high, off next) + a WORD byte count:
//   push objseg; push objoff; push cb; call KERNEL.334    (argbytes = 6)
// COMPOBJ's object-validator (seg1:0x35d8, the helper behind CoDisconnectObject)
// does:  ...call 334; OR ax,ax; JNZ invalid.  STORAGE's six call sites (seg3/4/7/
// 8/9/10) all GUARD the pointer first (OR ax,reg; JZ skip) then call 334 only on a
// NON-null pointer and likewise treat a NONZERO result as failure. So the contract
// is 0=valid / nonzero=bad, and STORAGE (which only ever passes valid blocks) keeps
// getting 0 = success exactly as before. The OLD code mis-modelled this as
// GetSelectorLimit(WORD sel) with argbytes=2, force-returning 0 unconditionally;
// that worked for STORAGE by accident but wrongly reported a NULL pointer as VALID,
// so COMPOBJ's CoDisconnectObject(NULL) on Word's OLE2 teardown path proceeded to
// dereference a NULL object and call far [0:0] (the null-vtable crash, b551).
// Returning nonzero (bad) for the NULL pointer makes CoDisconnectObject bail
// cleanly with E_INVALIDARG (0x80000003) instead of crashing.
static void k_getselectorlimit(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    uint16_t cb  = arg16(c, 0);          // byte count (last pushed)
    uint16_t off = arg16(c, 1);          // pointer offset
    uint16_t sel = arg16(c, 2);          // pointer segment/selector (first pushed)
    if (cb == 0) cb = 1;
    int bad;
    if (sel == 0) {
        bad = 1;                          // NULL pointer is always bad
    } else if (g_win16_pmode) {
        if (!ldt_valid(sel)) {
            bad = 1;                      // unallocated selector
        } else {
            uint32_t lim = ldt_limit(sel);            // last valid byte offset
            uint32_t last = (uint32_t)off + cb - 1;
            bad = (last > lim) ? 1 : 0;               // out of range => bad
        }
    } else {
        bad = 0;                          // real mode: non-null assumed readable
    }
    extern int g_ole2_k334log;
    if (g_ole2_k334log) kprintf("[K334] ptr=%04x:%04x cb=%u -> bad=%d\n",
                                sel, off, cb, bad);
    *ax = (uint16_t)(bad ? 1 : 0); *dx = 0; *argbytes = 6;
}

// SetSelectorLimit (KERNEL.337): WORD SetSelectorLimit(WORD sel, DWORD limit)
// = 1 word + 1 dword = 3 words. Best-effort: accept silently (our selectors are
// fixed-size arena tiles), return the selector. (#289)
static void k_setselectorlimit(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    // (#289 OLE2) KERNEL.337 as called by STORAGE.StgCreateDocfile returns 0 on
    // success (it does OR ax,dx; JZ-continue / nonzero -> error HRESULT). Our
    // selectors are fixed-size arena tiles, so accept and report success (0).
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;
}

// GlobalHandle (KERNEL.21, not imported). KERNEL.6 in real Win3.1 is GLOBALFREE
// is 17; #6 is reserved/used variously. TETRIS imports #6 and #1; treat both as
// 1-word success no-ops with the most common signature (single WORD handle).
static void k_word1_ok(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    (void)c; *ax = arg16(c, 0); *dx = 0; *argbytes = 2;
}

// MakeProcInstance (KERNEL.51): (lpProc far, hInstance word) = 3w = 6 bytes.
// Return the proc far pointer unchanged (DX:AX = seg:off) so the app calls it
// directly as its callback (our wndproc caller uses the class far ptr anyway).
static void k_makeprocinstance(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    uint16_t proc_off = arg16(c, 1);
    uint16_t proc_seg = arg16(c, 2);
    *ax = proc_off; *dx = proc_seg; *argbytes = 6;
}

// FreeProcInstance (KERNEL.52): (lpProc far) = 2w = 4 bytes.
static void k_freeprocinstance(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}

// ---------------------------------------------------------------------------
// Win16 file I/O (_lopen/_lread/_llseek/_lwrite/_lcreat/_lclose). Backed by the
// FAT volume. On open the whole file is read into memory; reads/seeks operate on
// that buffer; writes go to an in-memory buffer flushed on close (best effort).
// DOS paths are mapped to the app directory (relative) or root, stripping any
// drive letter and back-slashes. Handles are 1..WIN16_MAX_FILES (HFILE_ERROR=
// 0xFFFF). This is what lets Chip's Challenge load CHIPS.DAT level data, etc.
// ---------------------------------------------------------------------------
#define WIN16_MAX_FILES 64   // (#278 pass16) was 8; Word 6 holds many files open
                              // during startup (config/templates), exhausting the
                              // handle table so temp-dir probes + StgCreateDocfile
                              // (OF_CREATE / _lcreat) failed with HFILE_ERROR.
#define HFILE_ERROR 0xFFFF
typedef struct {
    int      used;
    uint8_t *buf;
    uint32_t size;     // valid bytes
    uint32_t cap;      // allocation capacity
    uint32_t pos;
    int      dirty;
    char     path[96]; // resolved MayteraOS path (for write-back)
} win16_file_t;
static win16_file_t g_files[WIN16_MAX_FILES];

// Translate a DOS/Windows path to a native MayteraOS path via the shared
// drive-letter FS layer (#257). Explicit "X:" paths map to /WINDIR/DRIVE_X; a
// bare relative name resolves against the app dir (legacy behavior); a native
// "/" path passes through unchanged. dos_resolve_path uppercases the result.
static void win16_resolve_path(x86_16_cpu_t *c, uint16_t seg, uint16_t off,
                               char *out, int outsz) {
    char dos[128]; rd_far_cstr(c, seg, off, dos, sizeof(dos));
    dos_resolve_path(dos, win16_get_appdir(), out, outsz);
}

// (#188 Word6) Create every missing parent directory of a native path (mkdir -p).
// Installers (Word SETUP) create a temp dir then write files into it, but they
// create the dir implicitly via the file create, so the parent must exist first.
static void win16_make_parent_dirs(const char *path) {
    char acc[160]; int n = 0;
    for (int i = 0; path[i] && n < (int)sizeof(acc) - 1; i++) {
        if (path[i] == '/' && n > 0) {
            acc[n] = '\0';
            // Skip the leading mount components that always exist.
            if (n > 1) fat_mkdir(&g_fat_fs, acc);   // best effort; ignores EEXIST
        }
        acc[n++] = path[i];
    }
    // (do not mkdir the final component: that is the file itself)
}

static int win16_file_open(x86_16_cpu_t *c, uint16_t seg, uint16_t off, int create) {
    int h = -1;
    for (int i = 1; i < WIN16_MAX_FILES; i++) if (!g_files[i].used) { h = i; break; }
    if (h < 0) return HFILE_ERROR;
    win16_file_t *fp = &g_files[h];
    if (create) {                                 // (#257) reject creates on the read-only CD (E:)
        char dchk[128]; rd_far_cstr(c, seg, off, dchk, sizeof(dchk));
        if (!dos_path_writable(dchk)) return HFILE_ERROR;
    }
    win16_resolve_path(c, seg, off, fp->path, sizeof(fp->path));
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, fp->path, &sz);
    if (!data) {
        if (!create) return HFILE_ERROR;
        // (#188) Materialize parent dirs + an empty on-disk file so subsequent
        // GetFileAttributes / dir-exists probes by the installer succeed.
        win16_make_parent_dirs(fp->path);
        fat_write_file(&g_fat_fs, fp->path, "", 0);
        sz = 0; fp->cap = 4096; fp->buf = (uint8_t *)kmalloc(fp->cap);
        if (!fp->buf) return HFILE_ERROR;
        fp->size = 0;
    } else {
        fp->cap = sz ? sz : 16; fp->buf = (uint8_t *)data; fp->size = sz;
    }
    fp->pos = 0; fp->dirty = create ? 1 : 0; fp->used = 1;
    kprintf("[win16] _lopen %s -> handle %d size %u\n", fp->path, h, fp->size);
    return h;
}

// _lclose (KERNEL.81): _lclose(hFile) = 1w = 2 bytes. Return 0 = success.
static void k_lclose(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                     uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    if (h >= 1 && h < WIN16_MAX_FILES && g_files[h].used) {
        win16_file_t *fp = &g_files[h];
        { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[FIO] _lclose h=%d dirty=%d size=%u path=%s b0=%02x%02x%02x%02x\n", h, fp->dirty, fp->size, fp->path, fp->size>0?fp->buf[0]:0, fp->size>1?fp->buf[1]:0, fp->size>2?fp->buf[2]:0, fp->size>3?fp->buf[3]:0); }
        if (fp->dirty) fat_write_file(&g_fat_fs, fp->path, fp->buf, fp->size);
        if (fp->buf) kfree(fp->buf);
        fp->used = 0; fp->buf = 0;
    }
    *ax = 0; *dx = 0; *argbytes = 2;
}
// _lread (KERNEL.82): _lread(hFile, lpBuf far, wBytes) = 1+2+1 = 4w = 8 bytes.
// Returns bytes actually read in AX.
static void k_lread(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                    uint16_t *argbytes) {
    uint16_t cnt     = arg16(c, 0);
    uint16_t buf_off = arg16(c, 1), buf_seg = arg16(c, 2);
    uint16_t h       = arg16(c, 3);
    *argbytes = 8; *ax = 0; *dx = 0;
    if (h < 1 || h >= WIN16_MAX_FILES || !g_files[h].used) return;
    win16_file_t *fp = &g_files[h];
    uint16_t n = 0;
    for (; n < cnt && fp->pos < fp->size; n++)
        x86_16_wr8(c, buf_seg, (uint16_t)(buf_off + n), fp->buf[fp->pos++]);
    *ax = n;
}

// OpenFile (KERNEL.74). The real Win16 KERNEL.74 is OpenFile; GetCurrentTask is
// the SEPARATE ordinal 36 (still registered below). TETRIS and Chips both import
// ordinal 74 alongside the _lread/_lclose/_llseek file set and use it to open
// their data files (Chips reads CHIPS.DAT), so 74 must resolve to OpenFile or the
// read fails ("Corrupt or inaccessible CHIPS.DAT"). (#148)
// OF_EXIST=0x4000, OF_READ=0, OF_WRITE=1, OF_READWRITE=2, OF_CREATE=0x1000.
static void k_openfile(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t wStyle = arg16(c, 0);
    uint16_t of_off = arg16(c, 1), of_seg = arg16(c, 2);
    uint16_t fn_off = arg16(c, 3), fn_seg = arg16(c, 4);
    *argbytes = 10; *dx = 0;
    char path[128]; win16_resolve_path(c, fn_seg, fn_off, path, sizeof(path));
    { extern int g_w6life, g_w6seq; if (g_w6life) { int has=0; for (const char*q=path;q[0]&&q[1]&&q[2]&&q[3];q++){ if(q[0]=='.'&&(q[1]=='D'||q[1]=='d')&&(q[2]=='R'||q[2]=='r')&&(q[3]=='V'||q[3]=='v')) has=1; } if (has) kprintf("[W6OPENFILE] SEQ %d: OpenFile(\"%s\", style=%04x)\n", g_w6seq++, path, wStyle); } }
    if (of_seg || of_off) {                        // fill OFSTRUCT
        x86_16_wr8(c, of_seg, of_off, 0x88);
        x86_16_wr8(c, of_seg, (uint16_t)(of_off+1), 1);
        x86_16_wr16(c, of_seg, (uint16_t)(of_off+2), 0);
        for (int i = 0; i < 4; i++) x86_16_wr8(c, of_seg, (uint16_t)(of_off+4+i), 0);
        int i = 0; for (; path[i] && i < 120; i++)
            x86_16_wr8(c, of_seg, (uint16_t)(of_off+8+i), (uint8_t)path[i]);
        x86_16_wr8(c, of_seg, (uint16_t)(of_off+8+i), 0);
    }
    if (wStyle & 0x4000) {                          // OF_EXIST
        uint32_t sz = 0; void *d = fat_read_file(&g_fat_fs, path, &sz);
        if (d) { kfree(d); *ax = 0; } else { *ax = HFILE_ERROR; }
        return;
    }
    int create = (wStyle & 0x1000) ? 1 : 0;         // OF_CREATE
    *ax = (uint16_t)win16_file_open(c, fn_seg, fn_off, create);
}

// GetCurrentTask (KERNEL.36 real; TETRIS imports #74). No args. Return htask.
static void k_getcurrenttask(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    (void)c; *ax = g_info.module_handle; *dx = 0; *argbytes = 0;
}

// win87em (the MS 80x87 floating-point emulator) ordinal #1 = the FP runtime
// dispatch entry (__fpmath / "FIDRQQ"-style install gate). It is a single far
// entry whose sub-function is selected by BX, called register-only (no Pascal
// stack args), so argbytes = 0 for every sub-function:
//   BX = 0  INITIALIZE: install the emulator / set up the FP environment.
//   BX = 3  set the FP control word + install the exception (cleanup) handler.
//   BX = 2  TERMINATE: tear down at process exit.
// The win87em init gate in the MS-C startup (TETRIS seg1 6d7b..6d99) is:
//     mov  cx, ds:[0xb7a]      ; segment word of the win87em.#1 far pointer
//     jcxz <skip-fp-init>      ; 0 -> no emulator present, skip
//     ... set up DS:AX/SI, xor bx,bx (BX=0)
//     lcall ss:[0xb78]         ; call win87em.#1  (this trap)
//     jae  <ok>                ; CF==0 -> success, continue startup
//     jmp  <abort, exit code 2>; CF==1 -> "FP init failed", terminate
// So the real gate is the CARRY FLAG, not a memory status word: win87em.#1 must
// return CF=0 on success. Our previous no-op left CF in whatever state the prior
// instruction set it, so the gate sometimes took the abort path and TETRIS exited.
// We satisfy the handshake by clearing CF (success) for the init/control/teardown
// sub-functions and returning AX=1 ("emulator installed"). No real x87 math is
// performed yet; that only matters once the app issues FP opcodes.
static void k_win87em_init(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    // Success for INITIALIZE (BX=0), control-word/handler setup (BX=3) and
    // TERMINATE (BX=2): clear carry so the startup's `jae` takes the OK path.
    c->flags &= (uint16_t)~WIN16_F_CF;
    *ax = 1; *dx = 0; *argbytes = 0;
}

// (#188 Word6) GlobalSize (KERNEL.20): GlobalSize(HGLOBAL hMem) = 1 word = 2 bytes.
// Returns the byte size of a global block. Our bump allocator does not track per-
// block size, so report a generous fixed size (any copy into the block fits). The
// caller (MS-C startup / SETUP) uses this for bookkeeping, not exact bounds.
// GlobalMasterHandle (KERNEL.29): DWORD GlobalMasterHandle(void) = no args.
// (#278 Word6) Returns DX:AX where (real Win16) AX = the selector of the global
// arena's master/burgermaster info block and DX = the master handle. Word's
// WinMain memory check calls this; an UNIMPL 0 made it print "not enough memory".
// We have no Win16 burgermaster arena to walk, so return a plausible non-zero
// far value (a valid data selector). DX:AX both non-zero so callers that do
// OR ax,dx / test the handle proceed.
static void k_globalmasterhandle(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                 uint16_t *argbytes) {
    (void)c;
    extern uint16_t g_win16_master_sel;
    uint16_t sel = g_win16_master_sel;
    if (sel == 0) {
        // Allocate a small block once to own a real selector to hand back.
        sel = heap_alloc(0x1000);
        g_win16_master_sel = sel;
    }
    *ax = sel ? sel : 0x0040; *dx = sel ? sel : 0x0040; *argbytes = 0;
}

// SelectorAccessRights (KERNEL.199): SelectorAccessRights(sel, fSet, value)
//   = 3 words = 6 bytes (wine krnl386 ord 199). fSet==0 -> return current access
//   rights word for the selector; fSet!=0 -> set them. We model a flat protected
//   arena; return a plausible present ring-3 read/write data descriptor access
//   byte (0x00F3) so callers that query/sanity-check selector rights proceed.
//   Previously UNIMPL -> argbytes=0 -> Pascal stack desync.
static void k_selectoraccessrights(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                   uint16_t *argbytes) {
    uint16_t fSet = arg16(c, 1);   // 0 = get, nonzero = set
    if (fSet == 0) { *ax = 0x00F3; } else { *ax = 0; }
    *dx = 0; *argbytes = 6;
}
static void k_globalsize(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    uint32_t sz = gblk_lookup(h);
    // (#278 Word6) For a handle that is a real pmode LDT selector but was NOT
    // allocated through our GlobalAlloc tracker (e.g. WINWORD's DGROUP/autodata
    // segment 075f placed by the NE loader), report the selector's LDT byte size
    // (limit+1). Returning 0 here made Word's heap-probe helper believe the block
    // had zero size, so it spun forever in GlobalReAlloc/GlobalSize. Only fall
    // back to the (stale) last-para size when the selector is unknown.
    if (sz == 0 && g_win16_pmode && ldt_valid(h)) {
        sz = (uint32_t)ldt_limit(h) + 1;
    }
    if (sz == 0) sz = (uint32_t)g_heap_last_para_size << 4;   // best-effort fallback
    // (#278 Word6) For the app's DGROUP/autodata selector, the MS-C runtime's
    // __LInit uses GlobalSize to compute the local-heap end marker (pLast). The
    // autodata segment's top region holds the SS==DS stack (ne_stack), so reporting
    // the FULL segment size makes __LInit place pLast OVER the stack -> the arena
    // walk hits a gap it never crosses (the local-heap-over-stack bug). Report the
    // stack-safe heap top (data_len + ne_heap, computed by the NE loader) instead so
    // __LInit builds a coherent arena ending below the stack. Only clamp DOWN.
    if (h == g_dgroup_sel && g_dgroup_heap_top &&
        sz > (uint32_t)g_dgroup_heap_top) {
        sz = (uint32_t)g_dgroup_heap_top;
    }
    *ax = (uint16_t)(sz & 0xFFFF); *dx = (uint16_t)(sz >> 16); *argbytes = 2;
}

// (#188 Word6) WinExec (KERNEL.166): WinExec(LPCSTR lpCmdLine, UINT nShow)
//   = far ptr (2w) + word = 6 bytes. Launches another program. Word's SETUP.EXE
//   stub uses WinExec to chain to the decompressed _MSSETUP.EXE installer. We do
//   not yet support running a second NE in the same interpreter instance (the
//   interpreter holds one global guest state), so we log the request and return a
//   "success" instance handle (>= 32 per the Win16 contract). The actual chained
//   installer launch is a separate task; returning success keeps SETUP's control
//   flow alive instead of treating WinExec failure (< 32) as a fatal error.
static void k_winexec(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                      uint16_t *argbytes) {
    uint16_t show   = arg16(c, 0);
    uint16_t cmd_off = arg16(c, 1), cmd_seg = arg16(c, 2);
    (void)show;
    char cmd[160]; rd_far_cstr(c, cmd_seg, cmd_off, cmd, sizeof(cmd));
    kprintf("[win16] WinExec(\"%s\", %u)\n", cmd, show);
    win16_trace("WinExec(\"%s\")\n", cmd);
    *ax = 33; *dx = 0; *argbytes = 6;   // >= 32 => "succeeded"
}

// (#188 Word6) KEYBOARD.DRV ANSI<->OEM code-page conversion.
// AnsiToOem (KEYBOARD.5): AnsiToOem(LPSTR lpAnsi, LPSTR lpOem) = 2 far = 8 bytes.
// OemToAnsi (KEYBOARD.6): same signature. We use a single code page (Latin1 ~=
// CP437 for the ASCII range), so the conversion is an identity copy for 0x00..0x7F
// (which is all SETUP needs for its ASCII file names / paths). Returns -1 (TRUE).

// (#278 Word6 pass19) VkKeyScan (KEYBOARD.129): WORD VkKeyScan(WORD wChar).
// Returns low byte = virtual-key code, high byte = shift state (bit0 shift,
// bit1 ctrl, bit2 alt). Faithful US-layout mapping for the ASCII range. argbytes=2
// (one WORD). A DLL in Word6 startup (0037:0637) spammed this; the UNIMPL path
// popped argbytes=0 and desynced the Pascal stack.

// (#278 Word6 pass19) Ordinals reached AFTER Word creates its main editing window
// (hwnd=0044). Each was UNIMPL (argbytes=0), desyncing the Pascal stack -> a WILDCS
// halt (cs=ff80). Signatures from the WINE 16-bit specs (user.exe16/gdi.exe16/
// krnl386.exe16). arg16(c,0) = rightmost (last-pushed) arg.

// USER.70 SetCursorPos(word x, word y) -> BOOL16. argbytes=4.
static void u_setcursorpos(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); (void)arg16(c,1); *ax = 1; *dx = 0; *argbytes = 4;
}
// USER.121 SetWindowsHook(s_word idHook, segptr lpfn) -> FARPROC prev hook.
// We do not run the WH_* hook chain; 0 = "no previous hook". argbytes=6.
static void u_setwindowshook(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;
}
// USER.58 GetClassName(hwnd, lpClassName far, nMaxCount) -> int length.
// (#278 Word6 pass23) Reverse-maps the window's resolved wndproc (proc_seg:proc_off,
// copied from its class at CreateWindow time) back to the registered g_classes[].name
// and copies it into the caller's buffer. Word's standalone-instance check
// (seg19:0x33e6 -> EnumTaskWindows callback seg19:0x338c) calls this to identify its
// top-level windows by class. Same arg shape as GetWindowText (USER.36):
// hwnd(1w) + far lpsz(2w) + nMaxCount(1w) = 4w = 8 bytes.
static void u_getclassname(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t n     = arg16(c, 0);
    uint16_t s_off = arg16(c, 1);
    uint16_t s_seg = arg16(c, 2);
    uint16_t hwnd  = arg16(c, 3);
    *dx = 0; *argbytes = 8;
    const char *name = "";
    win16_window_t *win = win_from_hwnd(hwnd);
    if (win) {
        for (int k = 0; k < WIN16_MAX_CLASSES; k++) {
            if (g_classes[k].used &&
                g_classes[k].proc_seg == win->proc_seg &&
                g_classes[k].proc_off == win->proc_off) {
                name = g_classes[k].name; break;
            }
        }
    }
    uint16_t i = 0;
    if (n > 0) {
        for (; i < (uint16_t)(n - 1) && name[i]; i++)
            x86_16_wr8(c, s_seg, (uint16_t)(s_off + i), (uint8_t)name[i]);
        x86_16_wr8(c, s_seg, (uint16_t)(s_off + i), 0);
    }
    *ax = i;
}

// USER.225 EnumTaskWindows(word hTask, segptr lpfn, long lParam) -> BOOL16.
// (#278 Word6 pass23) Enumerate top-level (parent==0) Win16 windows belonging to the
// (single) task and invoke the callback EnumWindowsProc(HWND, LPARAM) for each via
// x86_16_call_far, mirroring win16_call_wndproc's Pascal push order: push hwnd;
// push lParam hi; push lParam lo (retf 6 = 3 words). Stop when the callback returns 0
// (FALSE). Returns TRUE if every window was enumerated, FALSE if stopped early.
// Stack (rightmost = arg0): lParam_lo, lParam_hi, lpfn_off, lpfn_seg, hTask. argbytes=10.
static void u_enumtaskwindows(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    *argbytes = 10; *dx = 0; *ax = 1;
    uint16_t lp_lo    = arg16(c, 0);
    uint16_t lp_hi    = arg16(c, 1);
    uint16_t lpfn_off = arg16(c, 2);
    uint16_t lpfn_seg = arg16(c, 3);
    (void)arg16(c, 4);   // hTask: a single Win16 task here
    if (!g_cpu || (lpfn_seg == 0 && lpfn_off == 0)) return;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        if (!g_windows[i].used || g_windows[i].parent != 0) continue;
        uint16_t cargs[3];
        cargs[0] = g_windows[i].hwnd;   // pushed first (leftmost)
        cargs[1] = lp_hi;
        cargs[2] = lp_lo;               // pushed last
        uint16_t rax = 0, rdx = 0;
        int rc = x86_16_call_far(g_cpu, lpfn_seg, lpfn_off, cargs, 3, &rax, &rdx, 4000000UL);
        if (rc != 0) continue;          // callback ran away: skip, keep enumerating
        if (rax == 0) { *ax = 0; return; }   // callback asked to stop enumeration
    }
}
// USER.268 GlobalAddAtom(str lpString) -> ATOM. Simple global atom table; same
// string -> same atom. Integer atoms (#NNNN, seg==0, off<0xC000) pass through. argbytes=4.
static char g_gatom_name[128][64];
static int  g_gatom_used[128];
static void u_globaladdatom(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off = arg16(c,0), seg = arg16(c,1);
    *dx = 0; *argbytes = 4;
    if (seg == 0 && off != 0 && off < 0xC000) { *ax = off; return; }   // integer atom
    char nm[64]; rd_far_cstr(c, seg, off, nm, sizeof(nm));
    if (!nm[0]) { *ax = 0; return; }
    int freeslot = -1;
    for (int i = 0; i < 128; i++) {
        if (g_gatom_used[i]) {
            int k = 0; while (g_gatom_name[i][k] && nm[k] && g_gatom_name[i][k]==nm[k]) k++;
            if (g_gatom_name[i][k]==0 && nm[k]==0) { *ax=(uint16_t)(0xC000+i); return; }
        } else if (freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) { *ax = 0xC000; return; }
    g_gatom_used[freeslot] = 1;
    int j = 0; for (; nm[j] && j < 63; j++) g_gatom_name[freeslot][j] = nm[j];
    g_gatom_name[freeslot][j] = 0;
    *ax = (uint16_t)(0xC000 + freeslot);
}
// USER.457 DestroyIcon(word hIcon) -> BOOL16. argbytes=2.
static void u_destroyicon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); *ax = 1; *dx = 0; *argbytes = 2;
}
// USER.512 WNetGetConnection(ptr,ptr,ptr) -> no network: WN_NOT_CONNECTED 0x0030. argbytes=12.
static void u_wnetgetconnection(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0x0030; *dx = 0; *argbytes = 12;
}
// USER.513 WNetGetCaps(word) -> no network capabilities: 0. argbytes=2.
static void u_wnetgetcaps(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); *ax = 0; *dx = 0; *argbytes = 2;
}
// GDI.136 RemoveFontResource(str) -> BOOL16. argbytes=4.
static void g_removefontresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); (void)arg16(c,1); *ax = 1; *dx = 0; *argbytes = 4;
}
// KERNEL.348 hmemcpy(ptr lpDest, ptr lpSource, long count): faithful far memcpy.
// Pushed L->R: dst(far) src(far) count(dword); rightmost(arg0)=count. argbytes=12.
static void k_hmemcpy(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t count   = arg32(c,0);
    uint16_t src_off = arg16(c,2), src_seg = arg16(c,3);
    uint16_t dst_off = arg16(c,4), dst_seg = arg16(c,5);
    for (uint32_t i = 0; i < count && i < 0x100000; i++) {
        uint8_t b = x86_16_rd8(c, src_seg, (uint16_t)(src_off + (uint16_t)i));
        x86_16_wr8(c, dst_seg, (uint16_t)(dst_off + (uint16_t)i), b);
    }
    *ax = dst_off; *dx = dst_seg; *argbytes = 12;
}

static void kbd_vkkeyscan(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t ch = (uint16_t)(arg16(c, 0) & 0xFF);
    uint16_t vk, shift = 0;
    if      (ch >= 'a' && ch <= 'z') { vk = (uint16_t)(ch - 0x20); }
    else if (ch >= 'A' && ch <= 'Z') { vk = ch; shift = 1; }
    else if (ch >= '0' && ch <= '9') { vk = ch; }
    else if (ch == ' '  || ch == 0x0d || ch == 0x08 ||
             ch == 0x09 || ch == 0x1b) { vk = ch; }
    else { vk = ch; }                          // best effort: VK == char code
    *ax = (uint16_t)((shift << 8) | (vk & 0xFF));
    *dx = 0; *argbytes = 2;
}
// (#278 pass33) KEYBOARD.131 = MapVirtualKey(UINT wCode, UINT wMapType) -> UINT.
// Win16 Pascal: wCode pushed first (index 1), wMapType last (index 0). 2 words =
// 4 argbytes. Word 6 calls this on the keystroke path; when it was UNIMPL the
// argbytes=0 desynced the Pascal stack in Word's keyboard handler on every key.
//   wMapType 0: VK -> scan code   1: scan -> VK   2: VK -> unshifted char
//   3: scan -> VK (L/R distinguished). We give a faithful VK<->char/scan map;
// the exact scan values do not affect insertion (Word uses WM_CHAR for the
// character), but the correct argbytes keeps the stack synchronised.
static void kbd_mapvirtualkey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t maptype = arg16(c, 0);
    uint16_t code    = arg16(c, 1);
    uint16_t r = 0;
    switch (maptype) {
        case 2:  // VK -> unshifted character (uppercase for letters)
            if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) r = code;
            else r = 0;
            break;
        case 1:  // scan -> VK
        case 3:
            r = code;   // best-effort identity
            break;
        case 0:  // VK -> scan code
        default:
            r = code;   // best-effort identity
            break;
    }
    *ax = r; *dx = 0; *argbytes = 4;
}
// (#278 pass33) KEYBOARD.133 = GetKeyNameText(LONG lParam, LPSTR lpBuffer,
// int nSize) -> int. Win16 Pascal stack top->bottom: nSize(0), lpBuffer off(1),
// lpBuffer seg(2), lParam lo(3), lParam hi(4) = 5 words = 10 argbytes. Word uses
// this to label keys (status/help); return an empty name (length 0). The key
// point is the argbytes: UNIMPL 0 desynced Word's keystroke Pascal stack.
static void kbd_getkeynametext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t nSize   = arg16(c, 0);
    uint16_t buf_off = arg16(c, 1);
    uint16_t buf_seg = arg16(c, 2);
    if (nSize > 0) x86_16_wr8(c, buf_seg, buf_off, 0);   // empty, null-terminated
    *ax = 0; *dx = 0; *argbytes = 10;
}

static void kbd_xlate_copy(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t dst_off = arg16(c,0), dst_seg = arg16(c,1);
    uint16_t src_off = arg16(c,2), src_seg = arg16(c,3);
    for (uint16_t i = 0; i < 0x7FFF; i++) {
        uint8_t ch = x86_16_rd8(c, src_seg, (uint16_t)(src_off + i));
        x86_16_wr8(c, dst_seg, (uint16_t)(dst_off + i), ch);
        if (ch == 0) break;
    }
    *ax = 0xFFFF; *dx = 0; *argbytes = 8;
}

// (#188 Word6) LZEXPAND.DLL: LZ-compressed file expansion. Word's SETUP copies
// the *.DL_/*.EX_ compressed install files to C: via these. Full SZDD/KWAJ
// decompression is implemented in lzexpand_decode() below; LZCopy/LZRead expand
// the source while copying. Handles are our win16_file handles (1..MAX).
//
// LZOpenFile (LZEXPAND.2): LZOpenFile(LPSTR fn, LPOFSTRUCT, WORD mode) =
//   far(2w)+far(2w)+word = 10 bytes. Returns an HFILE (>=0) or HFILE_ERROR.
static void lz_openfile(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* mode */ (void)arg16(c, 0);
    uint16_t of_off = arg16(c, 1), of_seg = arg16(c, 2);
    uint16_t fn_off = arg16(c, 3), fn_seg = arg16(c, 4);
    *argbytes = 10; *dx = 0;
    char path[128]; win16_resolve_path(c, fn_seg, fn_off, path, sizeof(path));
    if (of_seg || of_off) {                          // fill OFSTRUCT (like OpenFile)
        x86_16_wr8(c, of_seg, of_off, 0x88);
        x86_16_wr8(c, of_seg, (uint16_t)(of_off+1), 1);
        x86_16_wr16(c, of_seg, (uint16_t)(of_off+2), 0);
        for (int i = 0; i < 4; i++) x86_16_wr8(c, of_seg, (uint16_t)(of_off+4+i), 0);
        int i = 0; for (; path[i] && i < 120; i++)
            x86_16_wr8(c, of_seg, (uint16_t)(of_off+8+i), (uint8_t)path[i]);
        x86_16_wr8(c, of_seg, (uint16_t)(of_off+8+i), 0);
    }
    *ax = (uint16_t)win16_file_open(c, fn_seg, fn_off, 0);
}
// LZClose (LZEXPAND.6): LZClose(HFILE) = 1 word = 2 bytes.
static void lz_close(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    if (h >= 1 && h < WIN16_MAX_FILES && g_files[h].used) {
        if (g_files[h].buf) kfree(g_files[h].buf);
        g_files[h].used = 0; g_files[h].buf = 0;
    }
    *ax = 0; *dx = 0; *argbytes = 2;
}
// Forward decl: LZ decompressor (SZDD/KWAJ), defined below near the LZ table entry.
static int lzexpand_decode(const uint8_t *in, uint32_t insz, uint8_t **out, uint32_t *outsz);
// LZCopy (LZEXPAND.1): LZCopy(HFILE hfSrc, HFILE hfDst) = 2 words = 4 bytes.
//   Expands the (possibly LZ-compressed) source into the destination. Returns the
//   number of bytes written, or a negative LZ error. We operate on the in-memory
//   file buffers: decode the source (SZDD/KWAJ) if compressed, else raw-copy, then
//   stage the result into the destination file's write-back buffer.
static void lz_copy(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hd = arg16(c, 0);   // dest (last pushed)
    uint16_t hs = arg16(c, 1);   // src
    *argbytes = 4; *ax = 0xFFFF; *dx = 0xFFFF;   // LZERROR_BADVALUE default
    if (hs < 1 || hs >= WIN16_MAX_FILES || !g_files[hs].used) return;
    if (hd < 1 || hd >= WIN16_MAX_FILES || !g_files[hd].used) return;
    win16_file_t *fs = &g_files[hs];
    win16_file_t *fd = &g_files[hd];
    uint8_t *out = 0; uint32_t outsz = 0;
    if (!lzexpand_decode(fs->buf, fs->size, &out, &outsz)) {
        // Not LZ-compressed: raw copy.
        out = (uint8_t *)kmalloc(fs->size ? fs->size : 1);
        if (!out) return;
        for (uint32_t i = 0; i < fs->size; i++) out[i] = fs->buf[i];
        outsz = fs->size;
    }
    if (fd->buf) kfree(fd->buf);
    fd->buf = out; fd->size = outsz; fd->cap = outsz; fd->pos = outsz; fd->dirty = 1;
    *ax = (uint16_t)(outsz & 0xFFFF); *dx = (uint16_t)(outsz >> 16);
}

// (#188 Word6) LZ decompressor for MS-Compress files. Two container formats:
//  - SZDD ("SZDD\x88\xF0\x27\x33"): a single LZSS stream, 16-byte header, the
//    uncompressed length is a LE dword at offset 10. Used by COMPRESS.EXE / most
//    Win 3.1 install kits.
//  - KWAJ ("KWAJ\x88\xF0\x27\xD1"): header has a 2-byte compression method at
//    offset 8 and flags at 10. Method 0 = store, 1 = store(no-copy), 2 = XOR-0xFF,
//    3 = SZDD-style LZSS, 4 = MS-ZIP/LZH (NOT implemented). Word 6 uses method 4.
// Returns 1 and allocates *out (caller frees) on success; 0 if not a recognized
// LZ container OR an unsupported method (caller then falls back to raw copy).
//
// SZDD LZSS: 4096-byte ring window preset to 0x20, init pos 4096-16=4080. Each
// control byte's bits (LSB first): 1 = literal byte; 0 = (match) two bytes giving
// {offset low8, (offset hi4<<8) | (len-3)} -> 4096-window back-ref of len+3.
static int lzss_szdd(const uint8_t *in, uint32_t insz, uint32_t start,
                     uint8_t *out, uint32_t outcap) {
    uint8_t win[4096];
    for (int i = 0; i < 4096; i++) win[i] = 0x20;
    int wpos = 4096 - 16;
    uint32_t ip = start, op = 0;
    while (ip < insz) {
        uint8_t ctrl = in[ip++];
        for (int b = 0; b < 8 && ip < insz; b++) {
            if (ctrl & (1 << b)) {                 // literal
                if (op >= outcap) return (int)op;
                uint8_t v = in[ip++];
                out[op++] = v; win[wpos] = v; wpos = (wpos + 1) & 4095;
            } else {                                // match
                if (ip + 1 >= insz) break;
                uint8_t lo = in[ip++], hi = in[ip++];
                int moff = lo | ((hi & 0xF0) << 4);
                int mlen = (hi & 0x0F) + 3;
                for (int k = 0; k < mlen; k++) {
                    uint8_t v = win[(moff + k) & 4095];
                    if (op >= outcap) return (int)op;
                    out[op++] = v; win[wpos] = v; wpos = (wpos + 1) & 4095;
                }
            }
        }
    }
    return (int)op;
}

static int lzexpand_decode(const uint8_t *in, uint32_t insz, uint8_t **out, uint32_t *outsz) {
    if (!in || insz < 14) return 0;
    // SZDD container.
    if (in[0]=='S'&&in[1]=='Z'&&in[2]=='D'&&in[3]=='D'&&
        in[4]==0x88&&in[5]==0xF0&&in[6]==0x27&&in[7]==0x33) {
        uint32_t ulen = (uint32_t)in[10] | ((uint32_t)in[11]<<8) |
                        ((uint32_t)in[12]<<16) | ((uint32_t)in[13]<<24);
        if (ulen == 0 || ulen > 64u*1024u*1024u) ulen = insz * 8 + 64;
        uint8_t *o = (uint8_t *)kmalloc(ulen + 16);
        if (!o) return 0;
        int n = lzss_szdd(in, insz, 14, o, ulen + 16);
        *out = o; *outsz = (uint32_t)n; return 1;
    }
    // KWAJ container.
    if (in[0]=='K'&&in[1]=='W'&&in[2]=='A'&&in[3]=='J'&&
        in[4]==0x88&&in[5]==0xF0&&in[6]==0x27&&in[7]==0xD1) {
        uint16_t method = (uint16_t)(in[8] | (in[9]<<8));
        uint16_t doff   = (uint16_t)(in[10] | (in[11]<<8));   // offset of compressed data
        if (doff < 14 || doff >= insz) doff = 14;
        if (method == 0 || method == 1) {                     // store
            uint32_t n = insz - doff;
            uint8_t *o = (uint8_t *)kmalloc(n ? n : 1);
            if (!o) return 0;
            for (uint32_t i = 0; i < n; i++) o[i] = in[doff + i];
            *out = o; *outsz = n; return 1;
        }
        if (method == 3) {                                    // SZDD-style LZSS
            uint32_t cap = (insz - doff) * 8 + 64;
            uint8_t *o = (uint8_t *)kmalloc(cap);
            if (!o) return 0;
            int n = lzss_szdd(in, insz, doff, o, cap);
            *out = o; *outsz = (uint32_t)n; return 1;
        }
        // method 2 (XOR) and 4 (LZH/MSZIP) NOT yet supported: signal caller.
        kprintf("[win16] LZEXPAND: unsupported KWAJ method %u\n", method);
        return 0;
    }
    return 0;
}

// ===========================================================================
// USER handlers (Phase 2C entry point). These are the first USER calls the C
// startup makes once it reaches the WinMain prologue. Implemented just well
// enough to keep the Pascal stack balanced and let startup proceed so we can
// observe the RegisterClass / CreateWindow / message-loop calls that follow.
// ===========================================================================

// InitApp (USER.5): InitApp(hInstance). Sets up the app's message queue and
// installs default resources. 1 word arg. Return nonzero = success.
static void u_initapp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                      uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 2;   // 1 word arg (HINSTANCE)
}

// ---------------------------------------------------------------------------
// Call a Win16 window procedure: LRESULT CALLBACK WndProc(HWND, UINT msg,
// WPARAM, LPARAM). Pascal order (leftmost arg pushed first):
//   push hwnd; push msg; push wParam; push lParam(hi); push lParam(lo).
// Returns the 32-bit LRESULT in DX:AX (we return the low word).
// ---------------------------------------------------------------------------
// (#393b) Forward decls for the native predefined-control (BUTTON) class proc,
// which is defined lower down (after dlg_text / the BS_* constants) but called
// from u_createwindow and win16_dispatch_to_window above it.
static int  dlg_ci_eq(const char *a, const char *b);
static void win16_draw_button(win16_window_t *win);
static int  win16_native_ctrl_proc(win16_window_t *win, uint16_t msg,
                                    uint16_t wParam, uint32_t lParam, uint32_t *out);
static uint32_t win16_call_wndproc(uint16_t pseg, uint16_t poff, uint16_t hwnd,
                                   uint16_t msg, uint16_t wParam, uint32_t lParam) {
    if (!g_cpu || (pseg == 0 && poff == 0)) return 0;
    uint16_t args[5];
    args[0] = hwnd;                          // pushed first (leftmost C arg)
    args[1] = msg;
    args[2] = wParam;
    args[3] = (uint16_t)(lParam >> 16);      // lParam hi
    args[4] = (uint16_t)(lParam & 0xFFFF);   // lParam lo (pushed last)
    uint16_t rax = 0, rdx = 0;
    extern int g_win16_apilog;
    if (g_win16_apilog)
        kprintf("[win16api]   -> wndproc %04x:%04x msg=%04x wp=%04x lp=%08x\n",
                pseg, poff, msg, wParam, (unsigned)lParam);
    // (#278 P41) trace messages delivered to Word's MDI document windows
    // (frame 0040, MDI client 004d, doc child 004e, edit pane 004f, ruler 0050).
    { extern int g_w6life, g_w6seq;
      if (g_w6life && g_info.segcount >= 100 &&
          (hwnd==0x004d||hwnd==0x004e||hwnd==0x004f||hwnd==0x0050||hwnd==0x0040)) {
        static int nwm=0; if (nwm<160) { nwm++;
          kprintf("[W6WMSG] SEQ %d: hwnd=%04x msg=%04x wp=%04x lp=%08x proc=%04x:%04x\n",
                  g_w6seq++, hwnd, msg, wParam, (unsigned)lParam, pseg, poff); } } }
    // WM_CREATE for a data-heavy app (Chips parses its 108 KB CHIPS.DAT here) can
    // legitimately run several million instructions; give the wndproc a generous
    // budget so a genuine init pass is not cut off (rc=1 -> the app sees the
    // wndproc "fail" and aborts). (#148)
    // (#278 pass35 FIX) Win16 wndprocs execute with DS = the app's DGROUP (the
    // __loadds / MakeProcInstance convention). Our dispatch otherwise inherits the
    // caller's DS; for Word that DS had drifted off DGROUP (a lost ds restore), so
    // the WM_CREATE doc-view build read its view HANDLE via the wrong segment
    // ([ds:handle]==0 while [DGROUP:handle] held the real block ptr) -> SetActiveView
    // rejected the view -> NULL active view [0x1d18] -> no typeable document. Restore
    // DS=DGROUP for Word (segcount>=100) so near data (view handle, [0x1d18], globals)
    // resolves in DGROUP. Games (<100 segs) are untouched = byte-identical.
    uint16_t w6_save_ds = g_cpu->ds;
    if (g_info.segcount >= 100 && g_info.ds) g_cpu->ds = g_info.ds;
    int rc = x86_16_call_far(g_cpu, pseg, poff, args, 5, &rax, &rdx, 16000000UL);
    g_cpu->ds = w6_save_ds;
    if (rc != 0 && g_win16_apilog) kprintf("[win16api]   wndproc call failed rc=%d\n", rc);
    extern int g_wndproc_trace_n;
    if (g_wndproc_trace_n < 40) {
        g_wndproc_trace_n++;
        win16_trace("wndproc %04x:%04x msg=%04x wp=%04x ret=%04x rc=%d\n",
                    pseg, poff, msg, wParam, rax, rc);
    }
    return ((uint32_t)rdx << 16) | rax;
}

// (#216) Recursively paint a window's child tree (children, grandchildren, ...).
// Real Windows paints children after their parent; we must recurse so nested
// children render (Chips' HUD counter digits are CounterClass grandchildren).
// win16_draw_frame computes each window's client origin from its parent's, so
// painting strictly parent-before-child also keeps nested origins correct.
static void win16_paint_child_tree(uint16_t parent_hwnd, int depth) {
    if (depth > 8) return;   // guard against pathological nesting / cycles
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *ch = &g_windows[i];
        if (!ch->used || !ch->is_child || ch->parent != parent_hwnd) continue;
        if (!ch->shown) continue;   // (#209) skip hidden children
        if (ch->w <= 0 || ch->h <= 0) continue;
        // (#209) Single-player TETRIS creates BOTH a "0 PLayer Game Grid" and a
        // "1 PLayer Game Grid"; the player-2 grid sits at the LEFT, overlapping
        // the score panel, and is never used in 1-player mode. The app does not
        // ShowWindow(SW_HIDE) it, so our forced child paint would draw its empty
        // grid over the score panel every other frame. Skip it (kind 1).
        if (g_win16_app_kind == 1 && ch->title[0] == '1' && ch->title[1] == ' ')
            continue;
        win16_draw_frame(ch);
        win16_call_wndproc(ch->proc_seg, ch->proc_off, ch->hwnd, WM_PAINT, 0, 0);
        // (#393b) A predefined BUTTON child has no app wndproc to render itself, so
        // draw its 3D chrome + label here (matching USER's BUTTONWNDPROC WM_PAINT).
        if (ch->ctrl_kind == 1 && ch->proc_seg == 0 && ch->proc_off == 0)
            win16_draw_button(ch);
        win16_paint_child_tree(ch->hwnd, depth + 1);   // (#216) nested descendants
    }
}

// Dispatch a message either to its window's wndproc, or (for paint) draw the
// frame first. Used by DispatchMessage and during the synthetic CreateWindow
// WM_CREATE / WM_PAINT bring-up.
static uint32_t win16_dispatch_to_window(uint16_t hwnd, uint16_t msg,
                                         uint16_t wParam, uint32_t lParam) {
    win16_window_t *win = win_from_hwnd(hwnd);
    if (!win) return 0;
    // (#393b) A predefined control (BUTTON) child registers no app wndproc; USER's
    // own class proc supplies its behaviour. Emulate it: turn WM_LBUTTONDOWN/UP into
    // the pressed-visual + BN_CLICKED WM_COMMAND-to-parent handshake, and paint the
    // button chrome on WM_PAINT. Only intercept when there genuinely is no app proc.
    if (win->ctrl_kind == 1 && win->proc_seg == 0 && win->proc_off == 0) {
        uint32_t cr = 0;
        if (win16_native_ctrl_proc(win, msg, wParam, lParam, &cr)) return cr;
    }
    if (msg == WM_PAINT)
        W6LOG("[W6] PAINT hwnd=%04x cx=%d cy=%d cw=%d ch=%d bgbr=%u child=%d soft=%d clip=%d,%d,%d,%d\n",
              hwnd, win->cx, win->cy, win->cw, win->ch, win->bg_brush, win->is_child,
              g_win16_in_soft_repaint, g_win16_clip_l, g_win16_clip_t, g_win16_clip_r, g_win16_clip_b);
    // (#219) Do NOT pre-erase the client on WM_PAINT. Real Win16 erases only the
    // update region, and only via WM_ERASEBKGND inside the app's own BeginPaint
    // (which we handle clip-aware). A blanket full-canvas erase here destroyed
    // persistent content for apps that paint once and rely on it (Golf's dealt
    // cards vanished to green; the well went black). Let BeginPaint do the erase.
    uint32_t r = win16_call_wndproc(win->proc_seg, win->proc_off, hwnd, msg, wParam, lParam);
    // Real Windows paints child windows after the parent. Drive WM_PAINT into the
    // FULL descendant tree so each window's wndproc renders. (#216) This must
    // recurse: Chips' HUD counters (LEVEL/TIME/CHIPS-LEFT digits) are CounterClass
    // grandchildren (children of an InfoClass child of the main window); a single
    // level only painted the InfoClass panel labels, never the nested counters, so
    // their digits never drew. Painting parent-before-child also makes each level's
    // client origin (win->cx/cy, computed in win16_draw_frame from the parent's
    // cx/cy) accumulate correctly down the nesting.
    if (msg == WM_PAINT) win16_paint_child_tree(hwnd, 0);
    return r;
}

// ===========================================================================
// USER: window class + window lifecycle
// ===========================================================================

// RegisterClass (USER.57): RegisterClass(lpWndClass far). The WNDCLASS struct in
// Win16 (packed) is:
//   WORD  style              +0
//   DWORD lpfnWndProc        +2   (off,seg)
//   WORD  cbClsExtra         +6
//   WORD  cbWndExtra         +8
//   WORD  hInstance          +10
//   WORD  hIcon              +12
//   WORD  hCursor            +14
//   WORD  hbrBackground      +16
//   DWORD lpszMenuName       +18
//   DWORD lpszClassName      +22
// Returns the atom (nonzero) on success.
static void u_registerclass(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t wc_off = arg16(c, 0);
    uint16_t wc_seg = arg16(c, 1);
    uint16_t proc_off = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 2));
    uint16_t proc_seg = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 4));
    uint16_t hbr      = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 16));
    uint16_t cn_off   = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 22));
    uint16_t cn_seg   = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 24));

    int slot = -1;
    for (int i = 0; i < WIN16_MAX_CLASSES; i++)
        if (!g_classes[i].used) { slot = i; break; }
    uint16_t atom = 0;
    if (slot >= 0) {
        g_classes[slot].used = 1;
        // Copy the class name NOW; the lpszClassName buffer may be reused before
        // CreateWindow re-reads it (real bug: stored far ptr went stale).
        rd_far_cstr(c, cn_seg, cn_off, g_classes[slot].name,
                    sizeof(g_classes[slot].name));
        g_classes[slot].proc_seg = proc_seg;
        g_classes[slot].proc_off = proc_off;
        g_classes[slot].hbrBackground = hbr;
        // (#152) capture the class default menu (WNDCLASS.lpszMenuName +18).
        {
            uint16_t mn_off = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 18));
            uint16_t mn_seg = x86_16_rd16(c, wc_seg, (uint16_t)(wc_off + 20));
            g_classes[slot].has_menu = 0; g_classes[slot].menu_id = 0;
            g_classes[slot].menu_name[0] = 0;
            if (mn_seg == 0) {
                if (mn_off) { g_classes[slot].menu_id = mn_off; g_classes[slot].has_menu = 1; }
            } else {
                rd_far_cstr(c, mn_seg, mn_off, g_classes[slot].menu_name,
                            sizeof(g_classes[slot].menu_name));
                if (g_classes[slot].menu_name[0]) g_classes[slot].has_menu = 1;
            }
        }
        atom = (uint16_t)(0xC000 + slot);
        kprintf("[win16api]   RegisterClass '%s' wndproc=%04x:%04x hbr=%04x\n",
                g_classes[slot].name, proc_seg, proc_off, hbr);
    }
    *ax = atom; *dx = 0; *argbytes = 4;  // 1 far ptr = 2 words
}

// Find a class by name (far string) or by atom. Returns class index or -1.
// A class name passed as an ATOM has a zero (or 0xC0xx) selector with the atom in
// the offset; RegisterClass hands back atom = 0xC000 + slot, so map that back.
static int find_class(x86_16_cpu_t *c, uint16_t seg, uint16_t off) {
    // Atom form: high-segment 0 and offset in the atom range we minted.
    if (seg == 0 && off >= 0xC000 && off < (uint16_t)(0xC000 + WIN16_MAX_CLASSES)) {
        int idx = off - 0xC000;
        if (g_classes[idx].used) return idx;
    }
    char want[64]; rd_far_cstr(c, seg, off, want, sizeof(want));
    for (int i = 0; i < WIN16_MAX_CLASSES; i++) {
        if (!g_classes[i].used) continue;
        const char *have = g_classes[i].name;
        // case-sensitive compare (class names are case-sensitive in Win16)
        int j = 0; for (; want[j] && have[j]; j++) if (want[j] != have[j]) break;
        if (want[j] == 0 && have[j] == 0) return i;
    }
    return -1;
}

// CreateWindow (USER.41): far __pascal. Args (Pascal, leftmost pushed first):
//   lpClassName(far), lpWindowName(far), dwStyle(long), x,y,w,h (s_word x4),
//   hWndParent(word), hMenu(word), hInstance(word), lpParam(far)
// On the stack top->bottom: lpParam(seg,off), hInstance, hMenu, hWndParent,
//   nHeight, nWidth, y, x, dwStyle(hi,lo), lpWindowName(seg,off),
//   lpClassName(seg,off).
// Returns the new HWND in AX.
static void u_createwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    // Count words from top: lpParam=2, hInstance=1, hMenu=1, hWndParent=1,
    // nHeight=1, nWidth=1, y=1, x=1, dwStyle=2, lpWindowName=2, lpClassName=2.
    int i = 0;
    uint16_t param_off = arg16(c, i++); uint16_t param_seg = arg16(c, i++);
    (void)param_off; (void)param_seg;
    /* hInstance */ (void)arg16(c, i++);
    uint16_t hMenu = arg16(c, i++);   // (#152) attached menu
    uint16_t hWndParent = arg16(c, i++);
    int16_t nHeight = (int16_t)arg16(c, i++);
    int16_t nWidth  = (int16_t)arg16(c, i++);
    int16_t wy      = (int16_t)arg16(c, i++);
    int16_t wx      = (int16_t)arg16(c, i++);
    uint32_t dwStyle = arg32(c, i); i += 2;
    uint16_t name_off = arg16(c, i++); uint16_t name_seg = arg16(c, i++);
    uint16_t cls_off  = arg16(c, i++); uint16_t cls_seg  = arg16(c, i++);
    *argbytes = (uint16_t)(i * 2);   // = 28 bytes
    int is_child = (dwStyle & 0x40000000UL) ? 1 : 0;   // WS_CHILD

    int ci = find_class(c, cls_seg, cls_off);
    int slot = -1;
    for (int k = 0; k < WIN16_MAX_WINDOWS; k++)
        if (!g_windows[k].used) { slot = k; break; }
    if (slot < 0) { *ax = 0; *dx = 0; return; }

    win16_window_t *win = &g_windows[slot];
    win->used = 1;
    win->hwnd = (uint16_t)(0x40 + slot);
    win->parent = is_child ? hWndParent : 0;
    win->is_child = is_child;
    win->proc_seg = (ci >= 0) ? g_classes[ci].proc_seg : 0;
    win->proc_off = (ci >= 0) ? g_classes[ci].proc_off : 0;
    win->bg_brush = (ci >= 0) ? g_classes[ci].hbrBackground : STOCK_LTGRAY_BRUSH;
    // (#393b) Predefined USER control classes (BUTTON/STATIC/EDIT/...) are not in
    // g_classes (the app never RegisterClass'd them), so ci<0. Detect them by name
    // (string form) or by the standard control atom (cls_seg==0: 0x80 BUTTON, 0x81
    // EDIT, 0x82 STATIC, 0x83 LISTBOX, 0x84 SCROLLBAR, 0x85 COMBOBOX) and record
    // the kind so our native class proc can drive them. A child gets its control ID
    // from hMenu; a predefined BUTTON also captures its BS_* low-nibble style.
    win->ctrl_kind = 0; win->btn_style = 0; win->btn_pressed = 0; win->btn_check = 0;
    win->ctrl_id = is_child ? hMenu : 0;
    if (ci < 0) {
        if (cls_seg == 0) {
            switch (cls_off) {   // standard control atom
                case 0x0080: win->ctrl_kind = 1; win->btn_style = (uint8_t)(dwStyle & 0x0F); break;
                case 0x0081: win->ctrl_kind = 3; break;
                case 0x0082: win->ctrl_kind = 2; break;
                case 0x0083: case 0x0085: win->ctrl_kind = 4; break;
                default: break;
            }
        } else {
            char cn[32]; rd_far_cstr(c, cls_seg, cls_off, cn, sizeof(cn));
            if      (dlg_ci_eq(cn, "BUTTON")) { win->ctrl_kind = 1; win->btn_style = (uint8_t)(dwStyle & 0x0F); }
            else if (dlg_ci_eq(cn, "STATIC")) { win->ctrl_kind = 2; }
            else if (dlg_ci_eq(cn, "EDIT"))   { win->ctrl_kind = 3; }
            else if (dlg_ci_eq(cn, "LISTBOX") || dlg_ci_eq(cn, "COMBOBOX")) { win->ctrl_kind = 4; }
        }
    }
    win->hmenu = is_child ? 0 : hMenu;   // (#152)
    if (!is_child && win->hmenu == 0 && ci >= 0 && g_classes[ci].has_menu) {
        // (#152) load the class default menu (WNDCLASS.lpszMenuName) when the app
        // passes hMenu=NULL (the common case: menu declared on the window class).
        uint32_t mlen = 0; const uint8_t *mt = 0;
        uint16_t hinst = g_info.module_handle;
        if (g_classes[ci].menu_name[0])
            mt = win16_get_resource_by_name(hinst, 4, g_classes[ci].menu_name, &mlen);
        else
            mt = win16_get_resource(hinst, 4, g_classes[ci].menu_id, &mlen);
        if (!mt) mt = win16_get_resource_first(hinst, 4, &mlen);
        if (mt && mlen >= 4) win->hmenu = win16_parse_menu(mt, mlen);
    }
    win->shown = is_child ? 1 : 0;   // children are visible with their parent
    if (!is_child && g_info.segcount >= 100 && (dwStyle & 0x10000000UL))
        win->shown = 1;   // (#278 pass31) WS_VISIBLE top-level is shown (SDM dialogs)
    // (#278 P46) Track the REAL WS_VISIBLE bit from the create style, independent of
    // the render `shown` flag. Word's edit pane is created without WS_VISIBLE.
    win->dwstyle    = dwStyle;
    win->ws_visible = (dwStyle & 0x10000000UL) ? 1 : 0;
    // (#207) Stamp activation order. A top-level window created later overlays the
    // ones before it, so the newest shown top-level owns input focus. Children do
    // not take focus on their own.
    win->z = is_child ? 0 : ++g_win16_z_counter;
    if (!is_child) g_win16_z_dirty = 1;   // (#209) new top-level -> full repaint

    if (is_child) {
        // Child coords are RELATIVE to the parent's client area. Keep them as-is
        // (0x8000/CW_USEDEFAULT is not used for children); a later MoveWindow may
        // resize a placeholder created at 1x1.
        if (nWidth  == (int16_t)0x8000) nWidth  = 0;
        if (nHeight == (int16_t)0x8000) nHeight = 0;
        if (wx == (int16_t)0x8000) wx = 0;
        if (wy == (int16_t)0x8000) wy = 0;
        win->x = wx; win->y = wy; win->w = nWidth; win->h = nHeight;
    } else {
        // CW_USEDEFAULT == 0x8000; pick a sane default position/size.
        uint32_t fb_w = fb_get_width(), fb_h = fb_get_height();
        // Win3.x apps target 640x480; a small default clipped wide layouts such
        // as FreeCell's 8 cascade columns (#147). Default to a 640x480 window.
        int defw = 640, defh = 480;
        if (nWidth  == (int16_t)0x8000 || nWidth  <= 0) nWidth  = defw;
        if (nHeight == (int16_t)0x8000 || nHeight <= 0) nHeight = defh;
        if (wx == (int16_t)0x8000) wx = (int)((fb_w - (uint32_t)nWidth) / 2);
        if (wy == (int16_t)0x8000) wy = (int)((fb_h - (uint32_t)nHeight) / 2);
        if (wx < 0) wx = 40;
        if (wy < 0) wy = 40;
        win->x = wx; win->y = wy; win->w = nWidth; win->h = nHeight;
    }
    rd_far_cstr(c, name_seg, name_off, win->title, sizeof(win->title));

    kprintf("[win16api]   CreateWindow hwnd=%04x '%s' at %d,%d %dx%d cls=%d child=%d parent=%04x style=%08x shown=%d\n",
            win->hwnd, win->title, win->x, win->y, win->w, win->h, ci,
            is_child, win->parent, (unsigned)dwStyle, win->shown);


    // Compositor-integrated host window (#144/#145): the first top-level Win16
    // window gets a real kernel user-window backed by a content buffer (canvas)
    // that the compositor composites (live desktop + taskbar button), instead of
    // the interpreter drawing straight to the framebuffer. Create it BEFORE
    // win16_draw_frame() so the canvas exists when drawing starts.
    if (!is_child && g_win16_host_slot < 0) {
        const char *tt = win->title[0] ? win->title : "Win16";
        // The main window hosts the whole app; Win3.x apps assume a VGA-ish area.
        // Enforce a usable minimum so wide layouts (FreeCell's 8 columns, #147)
        // are not clipped, capped to the screen.
        uint32_t fbw = fb_get_width(), fbh = fb_get_height();
        // (#278 Word6 pass36) Real Word 6 opens with its frame filling the screen
        // work area; our default centred a 640x480 frame (tiny window, document
        // shoved left). For Word (segcount>=100) maximise the frame to the full
        // work area so the MDI client + document fill it and the page centres.
        // Word lays out its children from GetClientRect(frame) during WM_CREATE,
        // which now returns the full size. Games keep the 640x480 default.
        if (g_win16_scrsave) {
            // (#345) Fullscreen top-most saver: cover the WHOLE screen (incl. the
            // taskbar row). win16_host_create() makes this window borderless when
            // g_win16_scrsave is set, so there is no title bar / frame.
            win->x = 0; win->y = 0; win->w = (int)fbw; win->h = (int)fbh;
        } else {
            if (g_info.segcount >= 100) {
                win->x = 0; win->y = 0;
                win->w = (int)fbw;
                win->h = (int)(fbh - 28);
            }
            if (win->w < 640) win->w = 640;
            if (win->h < 480) win->h = 480;
            if ((uint32_t)win->w > fbw) win->w = (int)fbw;
            if ((uint32_t)win->h > fbh - 28) win->h = (int)fbh - 28;  // leave taskbar room
            if (win->x + win->w > (int)fbw) win->x = (int)fbw - win->w;
            if (win->x < 0) win->x = 0;
        }
        uint32_t *buf = 0; int cw = 0, ch = 0;
        int slot = win16_host_create(tt, win->x, win->y, win->w, win->h,
                                     &buf, &cw, &ch, &g_taskbar_win);
        if (slot >= 0) {
            g_win16_host_slot = slot;
            g_win16_main_hwnd = win->hwnd;   // this window owns the whole canvas
            g_win16_canvas    = buf;
            g_win16_canvas_w  = cw;
            g_win16_canvas_h  = ch;
        }
    }

    // Compute client rect + paint background into the host canvas.
    win16_draw_frame(win);

    // Fire WM_CREATE through the wndproc immediately, as real CreateWindow does.
    win16_call_wndproc(win->proc_seg, win->proc_off, win->hwnd, WM_CREATE, 0, 0);

    // (#278 P41) Real Win16 CreateWindow runs an internal SetWindowPos on the new
    // window; DefWindowProc's WM_WINDOWPOSCHANGED handling then SENDS WM_MOVE and
    // WM_SIZE when the window has a real position/size. We previously omitted this
    // for child windows created directly at a nonzero size, so Word's MDI document
    // child (004e) and edit-pane view (004f) never received the initial WM_SIZE that
    // Word's view code uses to lay out the page + mark repagination. Deliver the
    // faithful WM_MOVE + WM_SIZE now (Word-gated; games keep exact prior behaviour).
    if (g_info.segcount >= 100 && is_child && win->cw > 0 && win->ch > 0) {
        uint32_t movelp = ((uint32_t)(uint16_t)win->y << 16) | (uint16_t)win->x;
        win16_call_wndproc(win->proc_seg, win->proc_off, win->hwnd,
                           0x0003 /*WM_MOVE*/, 0, movelp);
        uint32_t sizelp = ((uint32_t)(uint16_t)win->ch << 16) | (uint16_t)win->cw;
        win16_call_wndproc(win->proc_seg, win->proc_off, win->hwnd,
                           WM_SIZE, 0 /*SIZE_RESTORED*/, sizelp);
    }

    *ax = win->hwnd; *dx = 0;
}

// CreateWindowEx (USER.452 / not imported here) - kept for completeness via name.

// ShowWindow (USER.42): ShowWindow(hwnd, nCmdShow) = 2 words.
static void u_showwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t cmd = arg16(c, 0);
    uint16_t hwnd = arg16(c, 1);
    win16_window_t *win = win_from_hwnd(hwnd);
    uint16_t was = 0;
    // (#278 P46) trace every ShowWindow for Word so we can see whether Word calls
    // ShowWindow(SW_SHOW) on its edit-pane (004f) / doc child (004e). If it does,
    // our omission of WM_SHOWWINDOW delivery is the dropped realize event.
    { extern int g_w6life; if (g_w6life && g_info.segcount >= 100) {
        kprintf("[W6SHOW] ShowWindow(hwnd=%04x, cmd=%u) was_shown=%d child=%d parent=%04x caller=%04x:%04x\n",
                hwnd, cmd, win?win->shown:-1, win?win->is_child:-1, win?win->parent:0,
                x86_16_rd16(c,c->ss,(uint16_t)(c->sp+2)), x86_16_rd16(c,c->ss,c->sp)); } }
    if (win) {
        was = win->shown ? 1 : 0;
        // (#278 P46) Faithful WM_SHOWWINDOW delivery. Real USER.ShowWindow, when the
        // window's WS_VISIBLE state actually changes, SENDs WM_SHOWWINDOW (0x18,
        // wParam = TRUE on show / FALSE on hide) to the wndproc synchronously before
        // returning. We had never delivered it, so Word's edit-pane show/realize path
        // was dropped. Deliver it now on a real visibility transition. Gated to Word
        // (segcount>=100) so games keep byte-identical behaviour.
        int show = (cmd != SW_HIDE);
        if (g_info.segcount >= 100 && show != win->ws_visible) {
            win->ws_visible = show;
            if (show) win->dwstyle |=  0x10000000UL;
            else      win->dwstyle &= ~0x10000000UL;
            win16_call_wndproc(win->proc_seg, win->proc_off, hwnd,
                               0x0018 /*WM_SHOWWINDOW*/, (uint16_t)(show ? 1 : 0), 0);
        }
        win->shown = (cmd != SW_HIDE);
        if (win->shown) {
            // (#207) Showing a top-level window activates it -> it owns input focus.
            // This is what lets a TETRIS/Wep "About" popup (a top-level window shown
            // on top of the game) actually receive the click/Enter/ESC that closes it.
            if (!win->is_child) win->z = ++g_win16_z_counter;
            g_win16_z_dirty = 1;   // (#209) z-order changed -> full repaint next idle
            win16_draw_frame(win);
            // Post a WM_PAINT so the message loop drives the first paint.
            msgq_post(hwnd, WM_SIZE, 0, ((uint32_t)win->ch << 16) | (uint32_t)win->cw);
            msgq_post(hwnd, WM_PAINT, 0, 0);
        }
    }
    *ax = was; *dx = 0; *argbytes = 4;
}

// UpdateWindow (USER.124): UpdateWindow(hwnd) = 1 word. Force an immediate paint.
static void u_updatewindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    win16_dispatch_to_window(hwnd, WM_PAINT, 0, 0);
    *ax = 0; *dx = 0; *argbytes = 2;
}

// DestroyWindow (USER.53): DestroyWindow(hwnd) = 1 word.
static void u_destroywindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    win16_window_t *win = win_from_hwnd(hwnd);
    if (win) { win16_call_wndproc(win->proc_seg, win->proc_off, hwnd, WM_DESTROY, 0, 0);
               win->used = 0; }
    *ax = 1; *dx = 0; *argbytes = 2;
}

// MoveWindow (USER.56): MoveWindow(hwnd, x, y, w, h, bRepaint) = 6w = 12 bytes.
// Stack top->bottom: bRepaint, nHeight, nWidth, y, x, hwnd.
static void u_movewindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t repaint = arg16(c, 0);
    int h = (int16_t)arg16(c, 1);
    int w = (int16_t)arg16(c, 2);
    int y = (int16_t)arg16(c, 3);
    int x = (int16_t)arg16(c, 4);
    uint16_t hwnd = arg16(c, 5);
    win16_window_t *win = win_from_hwnd(hwnd);
    if (win) {
        int old_w = win->w, old_h = win->h;
        win->x = x; win->y = y; win->w = w; win->h = h;
        g_win16_z_dirty = 1;     // (#209) geometry changed -> full repaint next idle
        win16_draw_frame(win);   // recomputes cw/ch (client size) for the new rect
        kprintf("[win16api]   MoveWindow hwnd=%04x -> rel(%d,%d) %dx%d  client %dx%d\n",
                hwnd, x, y, w, h, win->cw, win->ch);
        // A size change must notify the window via WM_SIZE so its wndproc can
        // recompute layout-dependent state (e.g. the Tetris GameGrid derives its
        // per-cell pixel size from the client rect in WM_SIZE; without a fresh
        // WM_SIZE after this resize it keeps the 1x1 cell size => zero-area cells
        // and an invisible piece). WM_SIZE is SENT (synchronous), not posted.
        // lParam = MAKELONG(clientWidth, clientHeight); wParam = SIZE_RESTORED(0).
        if (w != old_w || h != old_h) {
            // POST (async) WM_SIZE rather than SEND (synchronous). A synchronous
            // re-entrant win16_call_wndproc from inside this API handler runs the
            // wndproc on the shared interpreter context; for some apps (SkiFree)
            // the nested return corrupts MoveWindow's own return and jumps to
            // ffff:fe76. Posting lets the app's own GetMessage loop run it safely.
            // Posted BEFORE any WM_PAINT so layout is recomputed first. (#204)
            uint32_t lp = ((uint32_t)(uint16_t)win->ch << 16) | (uint16_t)win->cw;
            msgq_post(hwnd, WM_SIZE, 0, lp);
        }
        if (repaint) msgq_post(hwnd, WM_PAINT, 0, 0);
    }
    *ax = 1; *dx = 0; *argbytes = 12;
}

// SetWindowPos (USER.232): SetWindowPos(hwnd, hwndInsertAfter, x, y, cx, cy, wFlags)
//   = 7w = 14 bytes. Stack top->bottom: wFlags, cy, cx, y, x, hwndInsertAfter, hwnd.
//   SWP_NOSIZE=0x0001, SWP_NOMOVE=0x0002.
static void u_setwindowpos(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t flags = arg16(c, 0);
    int cy = (int16_t)arg16(c, 1);
    int cx = (int16_t)arg16(c, 2);
    int y  = (int16_t)arg16(c, 3);
    int x  = (int16_t)arg16(c, 4);
    /* hwndInsertAfter */ (void)arg16(c, 5);
    uint16_t hwnd = arg16(c, 6);
    win16_window_t *win = win_from_hwnd(hwnd);
    if (win) {
        if (!(flags & 0x0002)) { win->x = x; win->y = y; }   // not SWP_NOMOVE
        if (!(flags & 0x0001)) { win->w = cx; win->h = cy; } // not SWP_NOSIZE
        win16_draw_frame(win);
        msgq_post(hwnd, WM_PAINT, 0, 0);
    }
    *ax = 1; *dx = 0; *argbytes = 14;
}

// PostMessage (USER.110): PostMessage(hwnd, msg, wParam, lParam) = 1+1+1+2 = 5w = 10 bytes.
static void u_postmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    uint32_t lParam = arg32(c, 0);
    uint16_t wParam = arg16(c, 2);
    uint16_t msg    = arg16(c, 3);
    uint16_t hwnd   = arg16(c, 4);
    if (msg == WM_QUIT) {
        extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[W6QUIT] PostMessage WM_QUIT hwnd=%04x caller cs:ip=%04x:%04x\n", hwnd, c->cs, c->ip);
        // (#278 pass27) Inhibit Word's automatic WM_QUIT (keep user-close working).
        if (!(g_info.segcount >= 100 && !g_win16_close_requested)) {
            msgq_post(hwnd, msg, wParam, lParam);
            g_quit_posted = 1;
        }
    } else {
        msgq_post(hwnd, msg, wParam, lParam);
    }
    *ax = 1; *dx = 0; *argbytes = 10;
}

// GetWindowText (USER.36): GetWindowText(hwnd, lpString far, nMaxCount) =
//   1+2+1 = 4w = 8 bytes. Copy the window title; return length copied.
static void u_getwindowtext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t n      = arg16(c, 0);
    uint16_t s_off  = arg16(c, 1);
    uint16_t s_seg  = arg16(c, 2);
    uint16_t hwnd   = arg16(c, 3);
    win16_window_t *win = win_from_hwnd(hwnd);
    uint16_t i = 0;
    if (n > 0 && win) {
        for (; i < (uint16_t)(n - 1) && win->title[i]; i++)
            x86_16_wr8(c, s_seg, (uint16_t)(s_off + i), (uint8_t)win->title[i]);
        x86_16_wr8(c, s_seg, (uint16_t)(s_off + i), 0);
    } else if (n > 0) {
        x86_16_wr8(c, s_seg, s_off, 0);
    }
    *ax = i; *dx = 0; *argbytes = 8;
}

// GetMenu (USER.157): GetMenu(hwnd) = 1w = 2 bytes. No menus -> 0.
static void u_getmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                      uint16_t *argbytes) {
    win16_window_t *win = win_from_hwnd(arg16(c, 0));
    *ax = win ? win->hmenu : 0; *dx = 0; *argbytes = 2;
}
// CheckMenuItem (USER.154) / EnableMenuItem (USER.155): (hMenu, idItem, flags) = 3w = 6 bytes.
static void u_menuitem3(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;
}
// LoadAccelerators (USER.177): (hInst, lpTableName far) = 1+2 = 3w = 6 bytes.
static void u_loadaccel(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    // Return a dummy NON-NULL handle: some apps (e.g. GOLF) treat a NULL accel
    // table as a fatal error and exit before showing their window.
    (void)c; *ax = 0x55; *dx = 0; *argbytes = 6;
}
// GetDialogBaseUnits (USER.243): no args. DWORD = HIWORD cy, LOWORD cx base units.
static void u_getdialogbaseunits(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                 uint16_t *argbytes) {
    (void)c; *ax = 8; *dx = 16; *argbytes = 0;
}

// ---- RECT helpers + the RECT/string utility family (USER) ----------------
// A RECT is four s_word fields (left, top, right, bottom).
static void rect_rd(x86_16_cpu_t *c, uint16_t seg, uint16_t off, int16_t r[4]) {
    for (int i = 0; i < 4; i++) r[i] = (int16_t)x86_16_rd16(c, seg, (uint16_t)(off + i*2));
}
static void rect_wr(x86_16_cpu_t *c, uint16_t seg, uint16_t off, const int16_t r[4]) {
    for (int i = 0; i < 4; i++) x86_16_wr16(c, seg, (uint16_t)(off + i*2), (uint16_t)r[i]);
}
// SetRect (USER.72): (lpRect, left, top, right, bottom) = 6w = 12b.
static void u_setrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t r[4]; r[3] = (int16_t)arg16(c,0); r[2] = (int16_t)arg16(c,1);
    r[1] = (int16_t)arg16(c,2); r[0] = (int16_t)arg16(c,3);
    rect_wr(c, arg16(c,5), arg16(c,4), r);
    *ax = 1; *dx = 0; *argbytes = 12;
}
// SetRectEmpty (USER.73): (lpRect) = 4b.
static void u_setrectempty(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t r[4] = {0,0,0,0}; rect_wr(c, arg16(c,1), arg16(c,0), r);
    *ax = 1; *dx = 0; *argbytes = 4;
}
// CopyRect (USER.74): (lpDst, lpSrc) = 8b.
static void u_copyrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t r[4]; rect_rd(c, arg16(c,1), arg16(c,0), r);
    rect_wr(c, arg16(c,3), arg16(c,2), r);
    *ax = 1; *dx = 0; *argbytes = 8;
}
// IsRectEmpty (USER.75): (lpRect) = 4b.
static void u_isrectempty(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t r[4]; rect_rd(c, arg16(c,1), arg16(c,0), r);
    *ax = (r[0] >= r[2] || r[1] >= r[3]) ? 1 : 0; *dx = 0; *argbytes = 4;
}
// PtInRect (USER.76): (lpRect, pt long) = 8b.
static void u_ptinrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t pt = arg32(c,0);
    int16_t px = (int16_t)(pt & 0xFFFF), py = (int16_t)(pt >> 16);
    int16_t r[4]; rect_rd(c, arg16(c,3), arg16(c,2), r);
    *ax = (px >= r[0] && px < r[2] && py >= r[1] && py < r[3]) ? 1 : 0;
    *dx = 0; *argbytes = 8;
}
// OffsetRect (USER.77): (lpRect, dx, dy) = 8b.
static void u_offsetrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t oy = (int16_t)arg16(c,0), ox = (int16_t)arg16(c,1);
    uint16_t off = arg16(c,2), seg = arg16(c,3);
    int16_t r[4]; rect_rd(c, seg, off, r);
    r[0] += ox; r[2] += ox; r[1] += oy; r[3] += oy;
    rect_wr(c, seg, off, r);
    *ax = 1; *dx = 0; *argbytes = 8;
}
// IntersectRect (USER.79): (lpDst, lpSrc1, lpSrc2) = 12b.
static void u_intersectrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t a[4], b[4];
    rect_rd(c, arg16(c,1), arg16(c,0), b);   // src2
    rect_rd(c, arg16(c,3), arg16(c,2), a);   // src1
    int16_t o[4];
    o[0] = a[0] > b[0] ? a[0] : b[0];
    o[1] = a[1] > b[1] ? a[1] : b[1];
    o[2] = a[2] < b[2] ? a[2] : b[2];
    o[3] = a[3] < b[3] ? a[3] : b[3];
    int empty = (o[0] >= o[2] || o[1] >= o[3]);
    if (empty) { o[0]=o[1]=o[2]=o[3]=0; }
    rect_wr(c, arg16(c,5), arg16(c,4), o);
    *ax = empty ? 0 : 1; *dx = 0; *argbytes = 12;
}
// AnsiNext (USER.472): (lpCurrentChar segptr) = 4b. Return ptr to next char (+1).
static void u_ansinext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off = arg16(c,0), seg = arg16(c,1);
    // Return the SAME pointer if it points at NUL (end of string), else +1.
    uint8_t ch = x86_16_rd8(c, seg, off);
    *ax = (uint16_t)(ch ? off + 1 : off); *dx = seg; *argbytes = 4;
}
// (#188 Word6) AnsiPrev (USER.473): AnsiPrev(LPCSTR lpStart, LPCSTR lpCurrent) =
//   2 far ptrs = 8 bytes. Returns a far pointer to the previous character. With
//   no DBCS that is simply lpCurrent - 1, clamped to lpStart. Args are pushed
//   Pascal order; the LAST pushed (lpCurrent) is nearest the top after the frame.
static void u_ansiprev(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cur_off = arg16(c,0), cur_seg = arg16(c,1);
    uint16_t st_off  = arg16(c,2); /* st_seg = arg16(c,3) (same seg assumed) */
    *argbytes = 8;
    uint16_t prev = (cur_off > st_off) ? (uint16_t)(cur_off - 1) : st_off;
    *ax = prev; *dx = cur_seg;
}
// (#188 Word6) AnsiUpper (USER.431): AnsiUpper(LPSTR lpsz) = 1 far ptr = 4 bytes.
//   Dual contract: if the segment (HIWORD) is 0 the LOWORD is a single character
//   to uppercase (returned in AX). Otherwise it is a far string pointer: uppercase
//   it in place and return the same pointer (DX:AX). Latin1 A..Z mapping only.
// lstrcmpi (USER.471): lstrcmpi(lpStr1 far, lpStr2 far) = 4w = 8b. Case-insensitive
// string compare (wine user.exe16 ord 471). Word's app-init uses it; UNIMPL popped
// argbytes=0 = an 8-byte Pascal stack DESYNC -> Word jumped into garbage and spun.
static void u_lstrcmpi(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t b_off = arg16(c, 0), b_seg = arg16(c, 1);
    uint16_t a_off = arg16(c, 2), a_seg = arg16(c, 3);
    int16_t r = 0; uint16_t i = 0;
    for (;;) {
        uint8_t ca = x86_16_rd8(c, a_seg, (uint16_t)(a_off + i));
        uint8_t cb = x86_16_rd8(c, b_seg, (uint16_t)(b_off + i));
        uint8_t la = (ca >= 'A' && ca <= 'Z') ? (uint8_t)(ca + 32) : ca;
        uint8_t lb = (cb >= 'A' && cb <= 'Z') ? (uint8_t)(cb + 32) : cb;
        if (la != lb) { r = (int16_t)((int)la - (int)lb); break; }
        if (ca == 0) break;
        i++;
    }
    *ax = (uint16_t)r; *dx = 0; *argbytes = 8;
}
// IsCharUpper (USER.435): IsCharUpper(wChar) = 1w = 2b. Nonzero if uppercase.
static void u_ischarupper(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint8_t ch = (uint8_t)(arg16(c, 0) & 0xFF);
    *ax = (ch >= 'A' && ch <= 'Z') ? 1 : 0; *dx = 0; *argbytes = 2;
}
// IsCharLower (USER.436): IsCharLower(wChar) = 1w = 2b. Nonzero if lowercase.
static void u_ischarlower(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint8_t ch = (uint8_t)(arg16(c, 0) & 0xFF);
    *ax = (ch >= 'a' && ch <= 'z') ? 1 : 0; *dx = 0; *argbytes = 2;
}
static void u_ansiupper(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off = arg16(c,0), seg = arg16(c,1);
    *argbytes = 4;
    if (seg == 0) {                       // single character in the low byte
        uint8_t ch = (uint8_t)(off & 0xFF);
        if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 32);
        *ax = ch; *dx = 0; return;
    }
    for (uint16_t i = 0; i < 0x7FFF; i++) {
        uint8_t ch = x86_16_rd8(c, seg, (uint16_t)(off + i));
        if (ch == 0) break;
        if (ch >= 'a' && ch <= 'z') x86_16_wr8(c, seg, (uint16_t)(off + i), (uint8_t)(ch - 32));
    }
    *ax = off; *dx = seg;
}
// UnregisterClass (USER.403): (lpClassName far, hInstance word) = 3w = 6b.
static void u_unregisterclass(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                              uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 6;
}
// GetClassInfo (USER.404): BOOL GetClassInfo(HINSTANCE hInst, lpClassName far,
// lpWndClass far). Pascal stack (last pushed first): lpWndClass(2w),
// lpClassName(2w), hInstance(1w) = 5w = 10b.
// (#278 b562) Word's app-init (seg136:0x1024) does
//   GetClassInfo(0, <name>, &wc); if it returns FALSE Word ABORTS startup.
// It then reads back wc.lpfnWndProc (wc+2/+4) and wc.cbWndExtra (wc+8) into its
// globals. The old stub unconditionally returned FALSE -> Word aborted (the real
// cause of the WinMain failure -> OLE2 teardown null-vtable crash, b551). Now we
// look the class up in g_classes (registered via RegisterClass); if present we
// fill the WNDCLASS out-buffer and return TRUE.
static void u_getclassinfo(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    *argbytes = 10;
    uint16_t wc_off = arg16(c, 0), wc_seg = arg16(c, 1);
    uint16_t cn_off = arg16(c, 2), cn_seg = arg16(c, 3);
    /* hInstance */ (void)arg16(c, 4);
    int idx = find_class(c, cn_seg, cn_off);
    char nm[64]; rd_far_cstr(c, cn_seg, cn_off, nm, sizeof(nm));
    extern int g_ole2_k334log;
    // (#278 b563) The standard predefined USER control classes (EDIT, BUTTON,
    // STATIC, LISTBOX, SCROLLBAR, COMBOBOX, COMBOLBOX, MDICLIENT) are registered
    // by USER.EXE itself in real Windows, not by the app. Word queries
    // GetClassInfo(0,'EDIT',&wc) during startup and ABORTS if it is missing.
    // We do not register them in g_classes, so synthesize a WNDCLASS for them and
    // report success. cbWndExtra matches the documented Win16 values so Word sizes
    // its control structures correctly.
    if (idx < 0) {
        struct { const char *n; uint16_t cbwnd; } pre[] = {
            {"EDIT",6}, {"BUTTON",0}, {"STATIC",0}, {"LISTBOX",0},
            {"SCROLLBAR",0}, {"COMBOBOX",0}, {"COMBOLBOX",0},
            {"MDICLIENT",2}, {"#32770",30}, {"STATIC",0}
        };
        int pi = -1;
        for (unsigned k=0;k<sizeof(pre)/sizeof(pre[0]);k++){
            const char *a=nm, *b=pre[k].n; int j=0;
            for(; a[j] && b[j]; j++){
                char ca=a[j],cb=b[j];
                if(ca>='a'&&ca<='z') ca=(char)(ca-32);
                if(cb>='a'&&cb<='z') cb=(char)(cb-32);
                if(ca!=cb) break;
            }
            if(!a[j] && !b[j]){ pi=(int)k; break; }
        }
        if (pi >= 0) {
            if (g_ole2_k334log) kprintf("[GETCLASSINFO] predefined '%s' -> synth WNDCLASS (cbwnd=%u)\n", nm, pre[pi].cbwnd);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 0),  0x0003);  // style
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 2),  0x0001);  // lpfnWndProc off (non-null sentinel)
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 4),  0x0001);  // lpfnWndProc seg
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 6),  0);       // cbClsExtra
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 8),  pre[pi].cbwnd); // cbWndExtra
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 10), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 12), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 14), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 16), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 18), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 20), 0);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 22), cn_off);
            x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 24), cn_seg);
            *ax = 1; *dx = 0; return;
        }
    }
    if (g_ole2_k334log) kprintf("[GETCLASSINFO] name='%s' (%04x:%04x) -> idx=%d\n", nm, cn_seg, cn_off, idx);
    if (idx < 0) { *ax = 0; *dx = 0; return; }   // class not registered
    // Fill the caller's WNDCLASS (standard 16-bit layout):
    //  +0 style(W) +2 lpfnWndProc(DWORD) +6 cbClsExtra(W) +8 cbWndExtra(W)
    //  +10 hInstance(W) +12 hIcon(W) +14 hCursor(W) +16 hbrBackground(W)
    //  +18 lpszMenuName(DWORD) +22 lpszClassName(DWORD)
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 0),  0x0003);                 // style CS_HREDRAW|CS_VREDRAW
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 2),  g_classes[idx].proc_off);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 4),  g_classes[idx].proc_seg);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 6),  0);                      // cbClsExtra
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 8),  0);                      // cbWndExtra
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 10), 0);                      // hInstance
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 12), 0);                      // hIcon
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 14), 0);                      // hCursor
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 16), g_classes[idx].hbrBackground);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 18), 0);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 20), 0);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 22), cn_off);
    x86_16_wr16(c, wc_seg, (uint16_t)(wc_off + 24), cn_seg);
    *ax = 1; *dx = 0;
}
static uint16_t win16_focus_hwnd(void);  // forward decl (defined below)
// TranslateAccelerator (USER.178): (hwnd, hAccel, lpMsg far) = 1+1+2 = 4w = 8 bytes.
// Generic: load the app's RT_ACCELERATOR table (a 5-byte-per-entry array of
// {BYTE fFlags, WORD key, WORD cmd}; last entry has fFlags bit 0x80). If the
// incoming MSG matches an entry, post WM_COMMAND(cmd) to the window and report
// the message as handled so the app's loop skips DispatchMessage for the key.
// This is what turns menu accelerators like F2 (New deal) into real commands.
static void u_translateaccel(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    *argbytes = 8;
    uint16_t msg_off = arg16(c, 0), msg_seg = arg16(c, 1);
    /* hAccel */ (void)arg16(c, 2);
    uint16_t hwnd = arg16(c, 3);
    uint16_t message = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 2));
    uint16_t wParam  = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 4));
    *ax = 0; *dx = 0;
    if (message != WM_KEYDOWN && message != WM_CHAR) return;
    uint32_t len = 0;
    const uint8_t *t = win16_get_resource_first(0, 9 /*RT_ACCELERATOR*/, &len);
    if (!t || len < 5) return;
    for (uint32_t o = 0; o + 5 <= len; o += 5) {
        uint8_t  fFlags = t[o];
        uint16_t key    = (uint16_t)(t[o+1] | (t[o+2] << 8));
        uint16_t cmd    = (uint16_t)(t[o+3] | (t[o+4] << 8));
        int virt = (fFlags & 0x01); // FVIRTKEY
        int hit = virt ? (message == WM_KEYDOWN && wParam == key)
                       : (message == WM_CHAR    && wParam == key);
        if (hit) {
            uint16_t target = hwnd ? hwnd : win16_focus_hwnd();
            msgq_post(target, WM_COMMAND, cmd, 0);
            *ax = 1;
            return;
        }
        if (fFlags & 0x80) break;   // last entry
    }
}

// DefWindowProc (USER.107): DefWindowProc(hwnd, msg, wParam, lParam) =
//   word,word,word,long = 1+1+1+2 = 5 words = 10 bytes. Returns 0 for most,
//   but WM_CLOSE -> DestroyWindow, WM_DESTROY -> PostQuitMessage(0).
static void u_defwindowproc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint32_t lParam = arg32(c, 0);   (void)lParam;
    uint16_t wParam = arg16(c, 2);   (void)wParam;
    uint16_t msg    = arg16(c, 3);
    uint16_t hwnd   = arg16(c, 4);
    // (#278 pass27 PERSISTENCE) Inhibit Word 6's automatic self-teardown (the
    // phase-2 OLE2 gate cascade fires WM_DESTROY/WM_QUIT) so its main window stays
    // open and visible. A user-initiated close still works via g_win16_close_requested.
    int w6_inhibit = (g_info.segcount >= 100 && !g_win16_close_requested);
    if (msg == WM_CLOSE) {
        win16_window_t *win = win_from_hwnd(hwnd);
        if (win) { win16_call_wndproc(win->proc_seg, win->proc_off, hwnd, WM_DESTROY, 0, 0);
                   win->used = 0; }
        { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[W6QUIT] DefWindowProc WM_CLOSE hwnd=%04x caller cs:ip=%04x:%04x inhibit=%d\n", hwnd, c->cs, c->ip, w6_inhibit); }
        if (!w6_inhibit) { g_quit_posted = 1; msgq_post(hwnd, WM_QUIT, 0, 0); }
    } else if (msg == WM_DESTROY) {
        { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[W6QUIT] DefWindowProc WM_DESTROY hwnd=%04x caller cs:ip=%04x:%04x inhibit=%d\n", hwnd, c->cs, c->ip, w6_inhibit); }
        if (!w6_inhibit) { g_quit_posted = 1; msgq_post(hwnd, WM_QUIT, 0, 0); }
    }
    *ax = 0; *dx = 0; *argbytes = 10;
}

// PostQuitMessage (USER.6): PostQuitMessage(nExitCode) = 1 word.
static void u_postquit(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[W6QUIT] PostQuitMessage caller cs:ip=%04x:%04x\n", c->cs, c->ip); }
    // (#278 pass27 PERSISTENCE) Word 6 (segcount>=100) self-quits very early via a
    // PostQuitMessage from seg85:0x3693 because a phase-2 OLE2/automation init gate
    // returns failure in our environment (the long-standing pass-23 blocker). That
    // tears the app (and its now-visible main window) down after a few seconds.
    // Until that gate is satisfied, SUPPRESS Word's *automatic* self-quit so the
    // window stays open and visible. A genuine user close (titlebar X) routes
    // through win16_request_close()->g_win16_close_requested, which GetMessage
    // still honours, so the app remains closable on demand. Gated to Word only;
    // games (low segcount) keep the normal PostQuitMessage path byte-for-byte.
    if (g_info.segcount >= 100 && !g_win16_close_requested) {
        static int n = 0;
        extern int g_ole2_k334log;
        if (g_ole2_k334log && n < 4) { n++;
            kprintf("[W6PERSIST] suppressed Word self-PostQuitMessage (cs:ip=%04x:%04x); window stays open\n",
                    c->cs, c->ip); }
        *ax = 0; *dx = 0; *argbytes = 2;
        return;
    }
    g_quit_posted = 1; msgq_post(0, WM_QUIT, 0, 0);
    *ax = 0; *dx = 0; *argbytes = 2;
}

// GetMessage (USER.108): GetMessage(lpMsg far, hWnd, wMsgMin, wMsgMax) =
//   2+1+1+1 = 5 words = 10 bytes. Fills the MSG struct and returns FALSE for
//   WM_QUIT (ends the loop), TRUE otherwise. The Win16 MSG layout (packed):
//     WORD hwnd +0, WORD message +2, WORD wParam +4, DWORD lParam +6,
//     DWORD time +10, POINT pt +14 (WORD x, WORD y).
static void msg_write(x86_16_cpu_t *c, uint16_t seg, uint16_t off,
                      const win16_msg_t *m) {
    x86_16_wr16(c, seg, (uint16_t)(off + 0), m->hwnd);
    x86_16_wr16(c, seg, (uint16_t)(off + 2), m->message);
    x86_16_wr16(c, seg, (uint16_t)(off + 4), m->wParam);
    x86_16_wr16(c, seg, (uint16_t)(off + 6), (uint16_t)(m->lParam & 0xFFFF));
    x86_16_wr16(c, seg, (uint16_t)(off + 8), (uint16_t)(m->lParam >> 16));
    x86_16_wr16(c, seg, (uint16_t)(off + 10), 0);
    x86_16_wr16(c, seg, (uint16_t)(off + 12), 0);
    x86_16_wr16(c, seg, (uint16_t)(off + 14), 0);
    x86_16_wr16(c, seg, (uint16_t)(off + 16), 0);
}

// Find the WM_COMMAND id bound to a virtual key in the app's RT_ACCELERATOR
// table. Win16 ACCEL entries are packed 5 bytes: fFlags(1), wKey(2), wId(2);
// the last entry has 0x80 in fFlags. FVIRTKEY is 0x01. Used to deal card games
// generically (post the app's own F2 -> New Game command, e.g. FreeCell 0x66,
// Tut's Tomb 0x41a) instead of any hardcoded id.
extern const uint8_t *win16_get_resource_first(uint16_t hinst, uint16_t type,
                                               uint32_t *out_len);
static uint16_t win16_accel_cmd_for_vk(uint16_t vk) {
    uint32_t len = 0;
    const uint8_t *a = win16_get_resource_first(0, 9 /*RT_ACCELERATOR*/, &len);
    if (!a) return 0;
    uint32_t p = 0;
    while (p + 5 <= len) {
        uint8_t  fl  = a[p];
        uint16_t key = (uint16_t)(a[p + 1] | (a[p + 2] << 8));
        uint16_t cmd = (uint16_t)(a[p + 3] | (a[p + 4] << 8));
        if ((fl & 0x01) && key == vk) return cmd;   // FVIRTKEY match
        if (fl & 0x80) break;                        // last entry
        p += 5;
    }
    return 0;
}

// Find the top-level (non-child) shown Win16 window to route keyboard input to.
static uint16_t win16_focus_hwnd(void) {
    // (#207) Input focus follows the most recently activated top-level window
    // (highest z stamp), not merely the first one in the array. A later popup or
    // About box overlays the game window, so it must receive the click/key that
    // dismisses it. Without this, all input went to the first window (the game)
    // behind a modal-looking splash that could never be closed.
    uint16_t best_hwnd = 0; uint32_t best_z = 0;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].shown && !g_windows[i].is_child &&
            g_windows[i].z >= best_z) { best_z = g_windows[i].z; best_hwnd = g_windows[i].hwnd; }
    if (best_hwnd) return best_hwnd;
    // Fall back to any used window.
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++)
        if (g_windows[i].used) return g_windows[i].hwnd;
    return 0;
}

// (#278 pass31) Find a shown top-level window whose title contains a substring
// (case-insensitive). Used to locate Word's startup "Tip of the Day" SDM dialog
// so it can be auto-dismissed (it is shown unconditionally and blocks typing).
static int w6_title_contains(const char *title, const char *needle) {
    if (!title || !needle) return 0;
    for (const char *p = title; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}
static uint16_t win16_find_toplevel_by_title(const char *needle) {
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *w = &g_windows[i];
        if (w->used && !w->is_child && w6_title_contains(w->title, needle))
            return w->hwnd;
    }
    return 0;
}

// (#210) Topmost top-level window that owns a menu: this is the game's main
// frame, the one a menu command (Game->New) must be routed to. An About/splash
// dialog has no menu, so this skips it. Used by the TETRIS autostart so a
// transient splash stealing focus does not cause the New command to be posted
// to a window that ignores it. Falls back to focus_hwnd when no menu exists.
static uint16_t win16_top_game_hwnd(void) {
    uint16_t best_hwnd = 0; uint32_t best_z = 0;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *w = &g_windows[i];
        if (w->used && w->shown && !w->is_child && w->hmenu && w->z >= best_z) {
            best_z = w->z; best_hwnd = w->hwnd;
        }
    }
    return best_hwnd ? best_hwnd : win16_focus_hwnd();
}

// (#207) Repaint all shown top-level windows in activation (z) order so the most
// recently activated window (e.g. an About popup) is painted LAST and stays on
// top of the game. Painting in raw array order could draw the game over a popup
// that happens to sit at a lower array slot. Used by the idle repaint, the
// menu-close repaint, and dialog erase so z-order is consistent everywhere.
static void win16_draw_caret(void);   // (#278 pass31) fwd
static void win16_paint_all_z(void) {
    int prev = g_win16_in_soft_repaint;
    g_win16_in_soft_repaint = 1;   // (#219) full repaint: BeginPaint uses full clip
    for (uint32_t pass = 1; pass <= g_win16_z_counter; pass++) {
        for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
            win16_window_t *w = &g_windows[i];
            if (!w->used || !w->shown || w->is_child || w->z != pass) continue;
            win16_draw_frame(w);
            win16_dispatch_to_window(w->hwnd, WM_PAINT, 0, 0);
        }
    }
    win16_menu_strip_maintain();   // (#bugB) heal Word's self-drawn menu strip if blanked
    win16_draw_caret();
    g_win16_in_soft_repaint = prev;
}

// (#209) Soft idle repaint: refresh ONLY the focused top-level window's animated
// content by dispatching WM_PAINT WITHOUT clearing its client to the background
// brush first. The app's own paint pixels persist in the host window content
// buffer between frames, so the falling tetromino is redrawn over the existing
// board rather than over a freshly grey-filled canvas (which caused the
// grey<->black flicker). Non-focused windows are left untouched on quiet frames.
static uint16_t win16_focus_hwnd(void);  // fwd (defined below)

// (#200 resize-hang) Called from proc/syscall.c user_window_handle_resize() AFTER
// it reallocates the host window content buffer. Re-point the Win16 canvas at the
// new buffer + size and invalidate the scratch so no drawing ever touches the old
// freed buffer. Same (kernel/WM) context as drawing, so no extra locking needed.
void win16_host_rebind_canvas(int slot, uint32_t *new_buf, int new_w, int new_h) {
    if (slot < 0 || slot != g_win16_host_slot) return;
    if (!new_buf || new_w <= 0 || new_h <= 0) return;
    g_win16_canvas   = new_buf;
    g_win16_canvas_w = new_w;
    g_win16_canvas_h = new_h;
    g_win16_scratch_w = 0;   // force scratch realloc at new geometry next repaint
    g_win16_scratch_h = 0;
}

static void win16_soft_repaint_focus(void) {
    // (#210) Flicker-free FULL repaint via an off-screen scratch buffer.
    //
    // Root cause of the black playfield: the #209 "soft" pass suppressed the
    // client bg fill to avoid a grey<->black flash, but TETRIS (and similar
    // apps) draw their board base + grid by RELYING on the window's bg brush
    // (the playfield child uses a BLACK background brush) and then overpainting
    // only the grid lines and the locked/falling cells. With the bg fill
    // suppressed, that base was never re-established, so between the rare
    // z-dirty repaints (which DO fill the bg) the canvas showed stale/empty
    // pixels and the playfield read as black. Opening a menu set g_win16_z_dirty
    // and triggered a full bg-filled repaint, which is why the real board only
    // flashed up on menu open.
    //
    // The fix is to do the SAME full, bg-filled repaint the menu path does, but
    // render it into an off-screen scratch buffer and then publish it to the
    // real canvas in a single memcpy. The compositor (which samples the canvas
    // asynchronously) therefore never observes a half-grey intermediate frame,
    // so there is no #209 flicker, while the board is fully present every frame.
    if (!g_win16_canvas || g_win16_canvas_w <= 0 || g_win16_canvas_h <= 0)
        return;

    // (Re)allocate the scratch buffer if the canvas geometry changed.
    if (!g_win16_scratch || g_win16_scratch_w != g_win16_canvas_w ||
        g_win16_scratch_h != g_win16_canvas_h) {
        if (g_win16_scratch) { kfree(g_win16_scratch); g_win16_scratch = 0; }
        size_t n = (size_t)g_win16_canvas_w * (size_t)g_win16_canvas_h;
        g_win16_scratch = (uint32_t *)kmalloc(n * sizeof(uint32_t));
        if (!g_win16_scratch) {
            // Out of memory: fall back to the in-place full repaint (may flicker
            // for one frame, but correctness beats a black board).
            for (uint32_t pass = 1; pass <= g_win16_z_counter; pass++)
                for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
                    win16_window_t *w = &g_windows[i];
                    if (!w->used || !w->shown || w->is_child || w->z != pass) continue;
                    win16_draw_frame(w);
                    win16_dispatch_to_window(w->hwnd, WM_PAINT, 0, 0);
                }
            return;
        }
        g_win16_scratch_w = g_win16_canvas_w;
        g_win16_scratch_h = g_win16_canvas_h;
    }

    // Render the complete frame into the scratch buffer. Temporarily point the
    // canvas at the scratch so every canvas_plot/canvas_fill/dc_plot writes
    // there. Seed it with the current canvas so any pixels an app drew outside
    // a WM_PAINT (e.g. via GetDC on a WM_TIMER) are preserved before the bg
    // fill overpaints the client areas.
    uint32_t *real_canvas = g_win16_canvas;
    size_t    npix = (size_t)g_win16_canvas_w * (size_t)g_win16_canvas_h;
    // (#bugB) Capture the menu strip from the live canvas BEFORE this frame's paint
    // (which forces a full client erase and blanks Word's self-drawn menu). If the
    // strip is currently blank (a prior scroll paint already wiped it) this restores
    // it from the last good snapshot; either way the seeded scratch carries the menu.
    win16_menu_strip_maintain();
    memcpy(g_win16_scratch, real_canvas, npix * sizeof(uint32_t));

    g_win16_canvas = g_win16_scratch;   // redirect all drawing to the scratch
    int prev = g_win16_no_bg_fill;
    g_win16_no_bg_fill = 0;             // DO fill bg: re-establish each board base
    g_win16_in_soft_repaint = 1;        // (#219) app BeginPaint/EndPaint no-op here
    for (uint32_t pass = 1; pass <= g_win16_z_counter; pass++) {
        for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
            win16_window_t *w = &g_windows[i];
            if (!w->used || !w->shown || w->is_child || w->z != pass) continue;
            win16_draw_frame(w);
            win16_dispatch_to_window(w->hwnd, WM_PAINT, 0, 0);
        }
    }
    // (#bugB) On the scratch (still live here) heal Word's self-drawn menu strip if
    // this frame's WM_PAINT blanked it, so the published frame carries the menu.
    win16_menu_strip_maintain();
    g_win16_in_soft_repaint = 0;
    g_win16_no_bg_fill = prev;
    g_win16_canvas = real_canvas;       // restore

    // Publish the finished frame to the real canvas in one shot (no tearing /
    // no half-grey frame ever visible to the compositor).
    memcpy(real_canvas, g_win16_scratch, npix * sizeof(uint32_t));
}

// (#219) Per-frame app-paint double buffering was tried here (redirect window
// paint cycles into the scratch buffer + publish on End/ReleaseDC) but it raced
// TETRIS's WM_TIMER piece draws and cached DCs -> the well went black. Disabled;
// the engine relies on win16_soft_repaint_focus for the flicker-free board render.
// Kept as no-op hooks so the call sites stay (no behaviour change).
static void win16_paint_begin(void) { (void)g_win16_paint_depth; (void)g_win16_frontbuf; (void)g_win16_in_soft_repaint; }
static void win16_paint_end(void)   { }
static void win16_paint_flush(void) { g_win16_clip_active = 0; }  // (#219) drop any leaked paint clip

// Map a kernel cooked key code (see cpu/isr.c) to a Win16 virtual-key code.
// Returns 0 if the code has no VK mapping. *is_release is set for release codes.
// *ch returns the ASCII char for WM_CHAR (0 if none).
static uint16_t kernel_key_to_vk(int code, int *is_release, char *ch) {
    *is_release = 0; *ch = 0;
    // Arrow press codes 0x80-0x83, release codes 0x90-0x93 (press + 0x10).
    switch (code) {
        case 0x80: return 0x26;                  // KEY_UP    -> VK_UP
        case 0x81: return 0x28;                  // KEY_DOWN  -> VK_DOWN
        case 0x82: return 0x25;                  // KEY_LEFT  -> VK_LEFT
        case 0x83: return 0x27;                  // KEY_RIGHT -> VK_RIGHT
        case 0x90: *is_release = 1; return 0x26; // KEY_UP release
        case 0x91: *is_release = 1; return 0x28; // KEY_DOWN release
        case 0x92: *is_release = 1; return 0x25; // KEY_LEFT release
        case 0x93: *is_release = 1; return 0x27; // KEY_RIGHT release
        // Function keys (kernel cooked codes from cpu/isr.c) -> Win16 VK_F1..F12.
        // TETRIS uses F3 (VK_F3=0x72) as its New Game key. F4/F11/F12 are claimed
        // by the kernel desktop (close/DOOM) so they never reach us; that is fine.
        case 0x88: return 0x70;  // KEY_F1  -> VK_F1
        case 0x89: return 0x71;  // KEY_F2  -> VK_F2
        case 0x8B: return 0x72;  // KEY_F3  -> VK_F3 (New Game)
        case 0x84: return 0x74;  // KEY_F5  -> VK_F5
        case 0x8D: return 0x76;  // KEY_F7  -> VK_F7
        case 0x8E: return 0x77;  // KEY_F8  -> VK_F8
        case 0x8F: return 0x78;  // KEY_F9  -> VK_F9
        case 0x87: return 0x79;  // KEY_F10 -> VK_F10
        case 0x8C: return 0x73;  // KEY_F4  -> VK_F4
        case 0x8A: return 0x75;  // KEY_F6  -> VK_F6
        case 0x85: return 0x7A;  // KEY_F11 -> VK_F11
        case 0x86: return 0x7B;  // KEY_F12 -> VK_F12
        default: break;
    }
    // Printable-ASCII release: base | 0x80 (only emitted for 0x20-0x7E).
    if (code >= 0xA0 && code <= 0xFE) {
        int base = code & 0x7F;
        if (base >= 0x20 && base <= 0x7E) {
            *is_release = 1;
            *ch = (char)base;
            if (base >= 'a' && base <= 'z') return (uint16_t)(base - 32); // VK = upper
            return (uint16_t)base;
        }
        return 0;
    }
    // Printable-ASCII press.
    if (code == 0x1B) return 0x1B;               // ESC -> VK_ESCAPE (no WM_CHAR needed)
    if (code == '\n' || code == '\r') { *ch = '\r'; return 0x0D; } // VK_RETURN
    if (code == 0x08) { *ch = 0x08; return 0x08; }                 // VK_BACK
    if (code == 0x09) { *ch = 0x09; return 0x09; }                 // VK_TAB
    if (code == ' ')  { *ch = ' '; return 0x20; }                  // VK_SPACE
    if (code >= 0x20 && code <= 0x7E) {
        *ch = (char)code;
        if (code >= 'a' && code <= 'z') return (uint16_t)(code - 32);
        return (uint16_t)code;
    }
    return 0;
}

// Pump real kernel input + timers into the Win16 message queue. Called by the
// GetMessage handler whenever the queue is empty. Translates pending keyboard
// ---------------------------------------------------------------------------
// Mouse forwarding (#187): translate the global kernel cursor into the focused
// Win16 window's client coords and post WM_MOUSEMOVE / button messages so card
// games (FreeCell etc.) become playable, plus menu-bar click -> popup -> WM_COMMAND.
// ---------------------------------------------------------------------------
static uint8_t  g_w16_pbtn = 0;          // previous mouse button bitmap
// (#393) Mouse capture target (SetCapture); 0 = none. When set, all mouse input
// is routed to this hwnd regardless of the cursor position, as real USER does.
static uint16_t g_win16_capture_hwnd = 0;
// (#393) When nonzero, log the WindowFromPoint hit-test routing of each click to
// serial ([MOUSEPATH]). Only fires on button transitions, so it is quiet unless
// the user actually clicks.
static int      g_w16_mousepath = 1;
static uint8_t  g_win16_keydown[256];    // VK down-state for GetKeyState/GetAsyncKeyState
static int      g_w16_pcvx = -100000;    // previous canvas x (move throttle)
static int      g_w16_pcvy = -100000;
static uint64_t g_w16_lclick_t = 0;      // last left-down tick (double-click)
static int      g_w16_lclick_x = 0, g_w16_lclick_y = 0;
static int      g_w16_menu_open = -1;    // open top-level menu index, -1 = none
static int      g_w16_menu_prev = -1;    // (#197) prev menu-open state for close-repaint
// (#219) Bits saved from the canvas under the open popup, restored on close so the
// app content beneath (cards, board, sprites) returns WITHOUT a full-client erase.
static uint32_t *g_w16_menu_save = 0;
static int       g_w16_menu_save_x, g_w16_menu_save_y, g_w16_menu_save_w, g_w16_menu_save_h;
static int       g_w16_menu_save_t = -1;  // top-level title the current save belongs to

// Top-level menu-bar hit-test (canvas coords). Returns the top index or -1.
static int win16_menu_bar_hit(win16_menu_t *m, int cvx, int cvy) {
    if (cvy < 0 || cvy >= WIN16_MENUBAR_H) return -1;
    for (int i = 0; i < m->ntop; i++)
        if (cvx >= m->top_x[i] - 4 && cvx < m->top_x[i] + m->top_w[i] + 8) return i;
    return -1;
}

// Collect the level-1 item indices belonging to top-level title t, and compute
// the popup rectangle. Returns the item count.
static int win16_menu_popup_items(win16_menu_t *m, int t, int *out_idx, int max,
                                  int *px, int *py, int *pw, int *ph) {
    int start = m->top_idx[t] + 1, n = 0, maxw = 0;
    for (int j = start; j < m->count; j++) {
        if (m->items[j].level == 0) break;       // next top-level title
        if (m->items[j].level != 1) continue;    // skip deeper submenus
        if (n < max) out_idx[n] = j;
        int w = 0;
        for (const char *s = m->items[j].text; *s; s++) if (*s != '&') w += FONT_WIDTH;
        if (w > maxw) maxw = w;
        n++;
    }
    *px = m->top_x[t]; *py = WIN16_MENUBAR_H;
    *pw = maxw + 24;   *ph = n * (FONT_HEIGHT + 4) + 4;
    if (*px + *pw > g_win16_canvas_w) *px = g_win16_canvas_w - *pw;
    if (*px < 0) *px = 0;
    return n;
}

// Hit-test an open popup; returns the item id (>0) or 0 if no command item hit.
static int win16_menu_popup_hit(win16_menu_t *m, int t, int cvx, int cvy) {
    int idx[40], px, py, pw, ph;
    int n = win16_menu_popup_items(m, t, idx, 40, &px, &py, &pw, &ph);
    if (cvx < px || cvx >= px + pw || cvy < py || cvy >= py + ph) return 0;
    int row = (cvy - py - 2) / (FONT_HEIGHT + 4);
    if (row < 0 || row >= n) return 0;
    win16_mitem_t *it = &m->items[idx[row]];
    if (it->is_sep || it->is_popup) return 0;
    return it->id;
}

// (#219) Restore the canvas bits previously saved from under an open popup. This
// returns the app's content (cards/board) exactly, with no full-client erase.
static void win16_menu_restore_under(void) {
    if (!g_w16_menu_save || !g_win16_canvas) { g_w16_menu_save_t = -1; return; }
    for (int yy = 0; yy < g_w16_menu_save_h; yy++) {
        int dy = g_w16_menu_save_y + yy;
        if (dy < 0 || dy >= g_win16_canvas_h) continue;
        for (int xx = 0; xx < g_w16_menu_save_w; xx++) {
            int dx = g_w16_menu_save_x + xx;
            if (dx < 0 || dx >= g_win16_canvas_w) continue;
            g_win16_canvas[dy * g_win16_canvas_w + dx] =
                g_w16_menu_save[yy * g_w16_menu_save_w + xx];
        }
    }
    kfree(g_w16_menu_save);
    g_w16_menu_save = 0;
    g_w16_menu_save_t = -1;
}

// (#219) Save the canvas bits a popup is about to cover, so they can be restored
// when it closes. Called from win16_draw_menu_popup before the fill.
static void win16_menu_save_under(int px, int py, int pw, int ph, int t) {
    if (g_w16_menu_save && g_w16_menu_save_t == t &&
        g_w16_menu_save_x == px && g_w16_menu_save_y == py &&
        g_w16_menu_save_w == pw && g_w16_menu_save_h == ph)
        return;   // already saved for this exact popup
    win16_menu_restore_under();   // a different popup was open: put its bits back first
    if (pw <= 0 || ph <= 0 || !g_win16_canvas) return;
    g_w16_menu_save = (uint32_t *)kmalloc((size_t)pw * (size_t)ph * sizeof(uint32_t));
    if (!g_w16_menu_save) return;
    g_w16_menu_save_x = px; g_w16_menu_save_y = py;
    g_w16_menu_save_w = pw; g_w16_menu_save_h = ph; g_w16_menu_save_t = t;
    for (int yy = 0; yy < ph; yy++)
        for (int xx = 0; xx < pw; xx++)
            g_w16_menu_save[yy * pw + xx] = canvas_get(px + xx, py + yy);
}

// Draw an open popup into the canvas (called each idle frame while open).
static void win16_draw_menu_popup(win16_menu_t *m, int t) {
    int idx[40], px, py, pw, ph;
    int n = win16_menu_popup_items(m, t, idx, 40, &px, &py, &pw, &ph);
    if (n <= 0) return;
    win16_menu_save_under(px, py, pw, ph, t);   // (#219) snapshot before overpainting
    uint32_t bg = FB_COLOR(238,238,238), ink = FB_COLOR(20,20,20), bd = FB_COLOR(90,90,90);
    canvas_fill(px, py, pw, ph, bg);
    for (int x = 0; x < pw; x++) { canvas_plot(px + x, py, bd); canvas_plot(px + x, py + ph - 1, bd); }
    for (int y = 0; y < ph; y++) { canvas_plot(px, py + y, bd); canvas_plot(px + pw - 1, py + y, bd); }
    int yy = py + 2;
    for (int r = 0; r < n; r++) {
        win16_mitem_t *it = &m->items[idx[r]];
        if (it->is_sep) {
            for (int x = 3; x < pw - 3; x++) canvas_plot(px + x, yy + (FONT_HEIGHT + 4) / 2, bd);
            yy += FONT_HEIGHT + 4; continue;
        }
        int tx = px + 8, ty = yy + 2;
        for (const char *sp = it->text; *sp; sp++) {
            if (*sp == '&') continue;
            const uint8_t *gph = font_get_glyph(*sp);
            if (gph) for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
                uint8_t bits = gph[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col)) canvas_plot(tx + col, ty + row, ink);
            }
            tx += FONT_WIDTH;
        }
        yy += FONT_HEIGHT + 4;
    }
}

// (#393) ---- Mouse hit-testing (WindowFromPoint semantics) --------------------
// Card games (FreeCell card piles), dialog buttons, and any Win16 app with child
// controls create child windows; real USER routes a mouse click to the DEEPEST
// child window under the cursor, not to the top-level frame. The pump previously
// posted every button/move to the top-level focus window only, so child controls
// never received a click. These helpers walk the window tree so the click reaches
// the correct hwnd, translated into that window's client coordinates.
#ifndef WS_DISABLED
#define WS_DISABLED 0x08000000UL
#endif
// Is canvas point (cvx,cvy) inside window w's client rect (canvas coordinates)?
static int win16_pt_in_client(win16_window_t *w, int cvx, int cvy) {
    return w->cw > 0 && w->ch > 0 &&
           cvx >= w->cx && cvx < w->cx + w->cw &&
           cvy >= w->cy && cvy < w->cy + w->ch;
}
// Descend from parent_hwnd to the deepest shown, enabled child window whose client
// rect contains the canvas point. Among overlapping siblings the topmost (latest
// created == highest array slot, matching Win16 child z-order) wins. Returns
// parent_hwnd when no child contains the point (the frame itself is the target).
static uint16_t win16_child_from_point(uint16_t parent_hwnd, int cvx, int cvy, int depth) {
    if (depth > 16) return parent_hwnd;      // cycle / runaway guard
    uint16_t best = 0; int best_slot = -1;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *ch = &g_windows[i];
        if (!ch->used || !ch->is_child || ch->parent != parent_hwnd) continue;
        if (!ch->shown) continue;                    // not rendered -> not a target
        if (ch->dwstyle & WS_DISABLED) continue;     // disabled -> transparent to hit-test
        if (!win16_pt_in_client(ch, cvx, cvy)) continue;
        if (i > best_slot) { best_slot = i; best = ch->hwnd; }
    }
    if (!best) return parent_hwnd;
    return win16_child_from_point(best, cvx, cvy, depth + 1);
}

// Sample the global cursor and post mouse messages to the focused Win16 window.
static void win16_pump_mouse(void) {
    uint16_t fh = win16_focus_hwnd();
    if (!fh || g_win16_host_slot < 0 || !g_win16_canvas) return;
    win16_window_t *win = win_from_hwnd(fh);
    if (!win) return;

    int ox = 0, oy = 0, ow = 0, oh = 0;
    if (win16_host_content_rect(g_win16_host_slot, &ox, &oy, &ow, &oh) != 0) return;

    int cvx = (int)mouse_x - ox;     // canvas coords (menu bar at top)
    int cvy = (int)mouse_y - oy;
    uint8_t btn = mouse_buttons, pbtn = g_w16_pbtn;
    int ldown = (btn & 1) && !(pbtn & 1);
    int lup   = !(btn & 1) && (pbtn & 1);
    int rdown = (btn & 2) && !(pbtn & 2);
    int rup   = !(btn & 2) && (pbtn & 2);
    g_w16_pbtn = btn;

    int in_canvas = (cvx >= 0 && cvy >= 0 && cvx < g_win16_canvas_w && cvy < g_win16_canvas_h);

    // ---- Menu interaction ----
    if (win->hmenu && win->hmenu <= WIN16_MAX_MENUS) {
        win16_menu_t *m = &g_menus[win->hmenu - 1];
        if (m->used) {
            if (g_w16_menu_open >= 0) {
                if (ldown) {
                    int id = win16_menu_popup_hit(m, g_w16_menu_open, cvx, cvy);
                    int reopen = win16_menu_bar_hit(m, cvx, cvy);
                    g_w16_menu_open = -1;
                    if (id > 0) msgq_post(fh, WM_COMMAND, (uint16_t)id, 0);
                    else if (reopen >= 0) g_w16_menu_open = reopen;
                }
                return;   // popup open: swallow all other mouse input
            }
            if (ldown && in_canvas && cvy < WIN16_MENUBAR_H) {
                int t = win16_menu_bar_hit(m, cvx, cvy);
                if (t >= 0) { g_w16_menu_open = t; g_win16_z_dirty = 1; return; }
            }
        }
    }

    if (!in_canvas) return;

    // (#393) A click in the top-level window's non-client menu-bar row is not a
    // client click; the menu block above already handled bar/popup hits.
    if (cvy - win->cy < 0) return;

    // (#393) WindowFromPoint routing. A window holding the mouse capture receives
    // all mouse input regardless of the cursor position (standard USER behaviour);
    // otherwise walk the tree top-down to the DEEPEST visible, enabled child under
    // the cursor. Post to THAT window with lParam in ITS client coordinates, so a
    // click on a card pile / button / playfield reaches the child, not the frame.
    uint16_t target;
    win16_window_t *tw = 0;
    if (g_win16_capture_hwnd && (tw = win_from_hwnd(g_win16_capture_hwnd)) != 0) {
        target = g_win16_capture_hwnd;
    } else {
        target = win16_child_from_point(fh, cvx, cvy, 0);
        tw = win_from_hwnd(target);
    }
    if (!tw) { target = fh; tw = win; }

    int clx = cvx - tw->cx;           // client coords of the TARGET window
    int cly = cvy - tw->cy;
    uint32_t lp = (((uint32_t)(uint16_t)cly) << 16) | (uint32_t)(uint16_t)clx;
    uint16_t wp = ((btn & 1) ? MK_LBUTTON : 0) | ((btn & 2) ? MK_RBUTTON : 0);

    if (g_w16_mousepath && (ldown || lup || rdown || rup)) {
        kprintf("[MOUSEPATH] cv=%d,%d fh=%04x -> target=%04x child=%d tcx=%d tcy=%d "
                "client=%d,%d cap=%04x %s%s%s%s\n",
                cvx, cvy, fh, target, tw->is_child, tw->cx, tw->cy, clx, cly,
                g_win16_capture_hwnd,
                ldown?"LDOWN ":"", lup?"LUP ":"", rdown?"RDOWN ":"", rup?"RUP ":"");
    }

    if (cvx != g_w16_pcvx || cvy != g_w16_pcvy) {
        g_w16_pcvx = cvx; g_w16_pcvy = cvy;
        msgq_post(target, WM_MOUSEMOVE, wp, lp);
    }
    if (ldown) {
        uint64_t now = timer_ticks; uint32_t hz = g_timer_hz ? g_timer_hz : 250;
        int dbl = (now - g_w16_lclick_t) < (uint64_t)(hz * 2 / 5);   // ~400 ms
        int near = (clx > g_w16_lclick_x - 5 && clx < g_w16_lclick_x + 5 &&
                    cly > g_w16_lclick_y - 5 && cly < g_w16_lclick_y + 5);
        if (dbl && near) {
            msgq_post(target, WM_LBUTTONDBLCLK, wp, lp);
            g_w16_lclick_t = 0;
        } else {
            msgq_post(target, WM_LBUTTONDOWN, wp, lp);
            g_w16_lclick_t = now; g_w16_lclick_x = clx; g_w16_lclick_y = cly;
        }
    }
    if (lup)   msgq_post(target, WM_LBUTTONUP,   wp, lp);
    if (rdown) msgq_post(target, WM_RBUTTONDOWN, wp, lp);
    if (rup)   msgq_post(target, WM_RBUTTONUP,   wp, lp);
}

// scancodes to WM_KEYDOWN/WM_KEYUP (+ WM_CHAR) for the focused top-level window,
// and posts WM_TIMER for any elapsed SetTimer. Returns the number of messages
// posted this call.
// (#215) Called from the kernel desktop (proc/syscall.c host-window on_close
// handler) when the user clicks the titlebar CLOSE (X) button. Just latches a
// flag; the Win16 pump turns it into a WM_QUIT on its own proc, so we never
// touch the interpreter from the desktop context.
void win16_request_close(void) { g_win16_close_requested = 1; }

static int g_w6_tip_dismissed = 0;   // (#278 pass31) one-shot Tip-of-the-Day dismiss
static uint16_t g_win16_focus = 0;   // (#278 pass31) window with input focus (SetFocus)
// (#278 pass31) Find Word's document edit window: the highest-hwnd (latest
// created) used CHILD window with a sizeable client (the MDI document/edit view,
// not a toolbar). Used to restore focus after the Tip dialog is dismissed.
static uint16_t win16_find_doc_window(void) {
    uint16_t best = 0;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) {
        win16_window_t *w = &g_windows[i];
        if (w->used && w->is_child && w->w >= 400 && w->h >= 200 && w->hwnd > best)
            best = w->hwnd;
    }
    return best;
}
// (#278 pass32) End Word's modal Tip-of-the-Day dialog the FAITHFUL way by
// completing SDM's (sdm.dll Standard Dialog Manager) own modal loop instead of
// destroying the dialog window behind SDM's back.
//
// RE of SDM.DLL: Word runs the Tip dialog via SDM's TMCDODLG(ord35) ->
// TMCDODLGDLI(ord36) whose modal message pump lives at seg2:0x822c. That pump
// loops PeekMessage/TranslateMessage/DispatchMessage and BREAKS only when the
// current dialog's "dying" flag is set: it tests word [dlg+0x0a] & 0x1000
// (and & 0x0100) each idle iteration and exits when set. The exported
// ENDDLG(ord38, seg1:0x5994) is exactly the routine that sets that flag
// (`orb [dlg+0x0b],0x10` == word[dlg+0x0a] bit 0x1000), stores the result at
// [dlg+0x4c], and re-enables/re-activates the dialog owner (seg1:0x594c). All of
// this lives in the CALLER's DGROUP because SDM.DLL is a DATA-NONE library
// (autodata==0), so it runs with DS = Word's DGROUP (g_info.ds).
//
// So we resolve SDM.ENDDLG at runtime and CALL it (result=IDOK). SDM sets the
// dying flag; on its next idle iteration the modal pump exits, TMCDODLGDLI tears
// the dialog down via FFREEDLG, and control returns to Word's startup code, which
// then activates the document (CreateCaret, focus to the edit pane, full menu).
// This is called from win16_pump_input (reached via the modal pump's own
// PeekMessage), so the guest CPU is already sitting in the modal loop with the
// SDM state populated. Word-gated by the caller; a no-op if SDM is not loaded.
static void win16_end_sdm_dialog(uint16_t result) {
    static int in_end = 0;               // re-entrancy guard (ENDDLG SendMessages)
    if (in_end || !g_cpu) return;
    uint16_t seg = 0, off = 0;
    if (!win16_module_export("SDM", 38 /*ENDDLG*/, &seg, &off)) return;
    in_end = 1;
    uint16_t save_ds = g_cpu->ds;
    g_cpu->ds = g_info.ds;               // SDM (DATA NONE) uses the app DGROUP
    uint16_t args[1] = { result };
    uint16_t rax = 0, rdx = 0;
    x86_16_call_far(g_cpu, seg, off, args, 1, &rax, &rdx, 8000000UL);
    g_cpu->ds = save_ds;
    in_end = 0;
    static int logged = 0;
    if (logged < 4) { logged++;
        kprintf("[W6TIP] SDM ENDDLG(%u) @%04x:%04x\n", result, seg, off); }
}
static int win16_pump_input(void) {
    // (#345) Screensaver: end on ANY real user input after a short grace period.
    // g_last_input_tick is bumped by the PS/2 mouse + keyboard IRQ handlers, so
    // this catches both mouse and keyboard regardless of window focus/geometry.
    // The saver's own DefScreenSaverProc also posts WM_CLOSE on input; this is a
    // belt-and-suspenders guarantee that input always tears the saver down.
    if (g_win16_scrsave) {
        extern volatile unsigned long long g_last_input_tick;
        uint32_t _sshz = g_timer_hz ? g_timer_hz : 250;
        if (g_last_input_tick > g_win16_scrsave_start + (_sshz / 2))
            g_win16_close_requested = 1;
    }
    int posted = 0;
    uint16_t focus = win16_focus_hwnd();

    // (#215) Titlebar CLOSE (X) button: the desktop latched g_win16_close_requested
    // asynchronously. Convert it to a WM_QUIT exactly like ESC so the app's
    // message loop exits and the full teardown (win16_api_end + registry_reset)
    // runs, freeing the single Win16 slot. focus may be 0 if the window was
    // already torn down; g_quit_posted alone still forces GetMessage to return
    // WM_QUIT, so this works regardless.
    if (g_win16_close_requested) {
        g_quit_posted = 1;
        if (focus) msgq_post(focus, WM_QUIT, 0, 0);
        else       msgq_post(0,     WM_QUIT, 0, 0);
        g_win16_close_requested = 0;
        posted++;
    }

    // (#200 SkiFree) One-shot auto-activate. SkiFree (SKI.EXE) opens
    // "Ski Paused ... Press F3 to continue" and AUTO-PAUSES while its window is
    // not the active window: its WM_ACTIVATE handler latches the active state and
    // a focus-recompute routine RESUMES the game (sets its run flag, records
    // GetTickCount) only when active; it also gates WM_KEYDOWN on that focus flag.
    // Our composited host window never generated WM_ACTIVATE, so the game sat
    // paused forever (even F3 was swallowed by the focus gate). SkiFree runs a
    // PeekMessage(PM_NOREMOVE) busy game loop, NOT a blocking GetMessage, so this
    // must run from win16_pump_input (reached by BOTH Get/PeekMessage). After a
    // short settle (so the window + first paint exist) synthesise
    // WM_ACTIVATE(WA_ACTIVE)+WM_SETFOCUS once. SkiFree then resumes on its own and
    // its loop advances the slope (paced by GetTickCount). Do NOT also send F3:
    // F3 is a pause/resume TOGGLE and would flip it straight back to paused.
    if (g_win16_app_kind == 3 && g_autostart_done < 2 && g_cpu && focus) {
        static uint64_t s_ski_arm_t;
        uint32_t shz = g_timer_hz ? g_timer_hz : 250;
        if (g_autostart_done == 0) {
            s_ski_arm_t = timer_ticks;
            g_autostart_done = 1;
            kprintf("[SKIAS] armed, focus=%04x\n", focus);
        } else if (timer_ticks - s_ski_arm_t >= (shz / 2 ? shz / 2 : 1)) {
            msgq_post(focus, 0x0006 /*WM_ACTIVATE*/, 1, 0);
            msgq_post(focus, 0x0007 /*WM_SETFOCUS*/, 0, 0);
            kprintf("[SKIAS] WM_ACTIVATE(WA_ACTIVE) hwnd=%04x\n", focus);
            g_autostart_done = 2;
        }
    }

    // Keyboard: drain whatever the ISR has queued (bounded so we never spin).
    // (#278 pass31) Word gives its document edit window real input focus via
    // SetFocus; route keys there so typed text lands in the document, not the
    // frame. Gated to Word (segcount>=100) so games keep top-level key routing.
    int drained = 0;
    uint16_t key_target = focus;
    if (g_info.segcount >= 100 && g_win16_focus) {
        win16_window_t *fw = win_from_hwnd(g_win16_focus);
        if (fw && fw->used) key_target = g_win16_focus;
    }
    { extern int g_w6life; if (g_w6life && g_info.segcount>=100 && keyboard_has_char())
        kprintf("[W6KDRAIN] pump: has_char=1 focus=%04x g_win16_focus=%04x key_target=%04x\n",
                focus, g_win16_focus, key_target); }
    while (key_target && keyboard_has_char() && drained < 16) {
        int code = keyboard_get_char();
        drained++;
        // ESC, or F4 (kernel desktop close convention), closes the Win16 app.
        // (#278 pass30) For Word (segcount>=100) ESC is a normal editing key
        // (cancel/dismiss a modal dialog, deselect), NOT an app-close, so the
        // user can clear the Tip-of-the-Day dialog and type. ESC then falls
        // through to kernel_key_to_vk -> VK_ESCAPE (WM_KEYDOWN). The titlebar-X
        // still closes Word via g_win16_close_requested. F4 keeps the convention.
        int esc_closes = (g_info.segcount < 100);
        if ((code == 0x1B && esc_closes) || code == 0x8C) {
            g_quit_posted = 1;
            msgq_post(focus, WM_QUIT, 0, 0);
            posted++;
            continue;
        }
        int is_release; char ch;
        uint16_t vk = kernel_key_to_vk(code, &is_release, &ch);
        if (vk == 0) continue;
        g_win16_keydown[vk & 0xFF] = is_release ? 0 : 1;   // for GetKeyState/GetAsyncKeyState
        { extern int g_w6life; if (g_w6life && g_info.segcount>=100)
            kprintf("[W6KDRAIN] code=%02x vk=%02x ch=%02x rel=%d -> post to %04x\n",
                    code, vk, (unsigned char)ch, is_release, key_target); }
        if (is_release) {
            msgq_post(key_target, WM_KEYUP, vk, 0);
            posted++;
        } else {
            msgq_post(key_target, WM_KEYDOWN, vk, 0);
            posted++;
            // TranslateMessage would synthesise WM_CHAR for character keys; post
            // it directly so apps that read WM_CHAR also work.
            if (ch) { msgq_post(key_target, WM_CHAR, (uint16_t)(uint8_t)ch, 0); posted++; }
        }
    }

    // (#278 pass31) Dismiss Word's startup Tip-of-the-Day SDM dialog so the
    // document becomes the active window and typing is visible. The dialog is a
    // modal SDM window (cls 34) with OWNER-DRAWN buttons (no child controls) and
    // Word's loop does NOT call IsDialogMessage, so neither queued keys nor a
    // WM_COMMAND dismiss it; only a click on its OK button does. We post synthetic
    // WM_LBUTTONDOWN/UP to the dialog, sweeping a coarse client-coord grid until
    // SDM closes it (auto-detected when the window disappears), then restore focus
    // to the document. Word-gated (segcount>=100) so games are untouched.
    if (g_info.segcount >= 100 && g_w6_tip_dismissed == 0) {
        static int s_clk_i = -3; static uint64_t s_clk_t;
        uint32_t hz = g_timer_hz ? g_timer_hz : 250;
        uint16_t tip = win16_find_toplevel_by_title("Tip of the Day");
        if (!tip) {
            if (s_clk_i >= 0) {   // it WAS up and is now gone -> closed
                kprintf("[W6TIP] dialog closed after i=%d\n", s_clk_i);
                g_w6_tip_dismissed = 1;
                g_win16_z_dirty = 1;
                // Activate the MDI document so Word merges the full menu and the
                // document accepts keyboard input. doc=edit pane (004f); its
                // parent=document child (004e 'Document1'); grandparent=MDI client
                // (004d). Drive WM_MDIACTIVATE on the client, then activate+focus.
                uint16_t doc = win16_find_doc_window();
                if (doc) {
                    win16_window_t *dwf = win_from_hwnd(doc);
                    uint16_t child  = dwf ? dwf->parent : 0;
                    win16_window_t *cw = child ? win_from_hwnd(child) : 0;
                    uint16_t client = cw ? cw->parent : 0;
                    // (#278 pass33) Find Word's FRAME window (top-level ancestor of
                    // the doc child). Word 6 rolls its OWN MDI: its client proc is
                    // Word code that forwards to DefWindowProc (USER.107), it does
                    // NOT import DefFrameProc/DefMDIChildProc or the USER caret API,
                    // and it IGNORES WM_MDIACTIVATE/WM_MDICREATE. So MDI messages do
                    // nothing. The USER-level activation is the frame WM_ACTIVATE
                    // handler (seg222:0x09e0 -> dispatcher seg222:0x2ca4 = a modal-
                    // gated SetActiveWindow). Drive WM_NCACTIVATE(1)+WM_ACTIVATE(1)
                    // on the frame + WM_SETFOCUS on the edit pane. NOTE (pass33): this
                    // still does NOT make the document usable, because Word's document
                    // VIEW OBJECT is NULL (active-view global [DGROUP:0x1d18] is only
                    // ever written 0; the view-switch routine seg223:0x1459 runs with
                    // si=0). The GUI windows (004e/004f) exist but no text-engine view
                    // is bound to them, so keystrokes have nowhere to insert and the
                    // menu stays the no-document short menu. Root cause is upstream in
                    // Word's document-object creation (see cl_word6.md pass 33).
                    uint16_t frame = client ? win_from_hwnd(client)->parent : 0;
                    while (frame) {
                        win16_window_t *fw = win_from_hwnd(frame);
                        if (!fw || fw->parent == 0) break;
                        frame = fw->parent;
                    }
                    win16_window_t *fwp = frame ? win_from_hwnd(frame) : 0;
                    if (fwp) {
                        win16_call_wndproc(fwp->proc_seg, fwp->proc_off, frame,
                                           0x0086 /*WM_NCACTIVATE*/, 1, 0);
                        win16_call_wndproc(fwp->proc_seg, fwp->proc_off, frame,
                                           0x0006 /*WM_ACTIVATE*/, 1, 0);
                    }
                    g_win16_focus = doc;
                    if (dwf)
                        win16_call_wndproc(dwf->proc_seg, dwf->proc_off, doc,
                                           0x0007 /*WM_SETFOCUS*/, 0, 0);
                    kprintf("[W6TIP] frame-activate frame=%04x child=%04x client=%04x focus=%04x\n",
                            frame, child, client, doc);
                    // Force a full erase+repaint so the destroyed dialog's stale
                    // pixels are cleared and the document page shows in full (the
                    // GetMessage idle repaint may not run while Word's queue stays
                    // busy). Clear the whole canvas first, then paint every shown
                    // top-level + its child tree.
                    if (g_win16_canvas)
                        canvas_fill(0, 0, g_win16_canvas_w, g_win16_canvas_h,
                                    colorref_to_fb(win16_syscolor(12 /*APPWORKSPACE*/)));
                    win16_paint_all_z();
                    g_win16_z_dirty = 0;
                }
            }
        } else if (timer_ticks - s_clk_t >= (hz/5 ? hz/5 : 1)) {
            s_clk_t = timer_ticks;
            if (s_clk_i < 0) { s_clk_i++; }   // initial settle (~0.6s)
            else if (s_clk_i < 240) {
                // (#278 pass32) Complete SDM's modal loop cleanly (see
                // win16_end_sdm_dialog) instead of clicking/destroying the window.
                // Calling it once should be enough; keep calling (idempotent) each
                // ~0.2s until SDM has actually torn the window down.
                win16_end_sdm_dialog(1 /*IDOK*/);
                s_clk_i++;
                posted++;
            }
        }
    }
    // (#278 Word6 pass37) DOC-READY injection. Word finishes activating the initial
    // document for editing by SENDING its frame an internal message 0x10 whose wParam
    // is the newly built view object; the frame wndproc (seg222:0x0860) routes msg
    // 0x10 through its jump table -> seg222:0x09c8 -> seg85:0x3566 (the extended
    // handler), whose msg-0x10 case (seg85:0x37a0) validates the view (lcall
    // seg182:0), ORs the "document ready" flag [DGROUP:0x344] bit3, and builds the
    // page/view layout + status. In our host that internal SendMessage is never
    // emitted (the doc-creation command chain that would post it does not complete),
    // so the edit page never paints and typing is impossible. Everything the handler
    // needs is now present: the active-view global [DGROUP:0x1d18] is populated (the
    // WM_CREATE doc-view build ran, pass35), and the frame/edit windows exist. Once
    // the Tip is gone and the view is non-null, synthesize that one message with the
    // real view as wParam, exactly as Word would. Word-gated (segcount>=100), one
    // shot; a no-op for games (they never populate [0x1d18] and are <100 segs).
    {
        // (#278 pass49) DOC-READY injection, TIMING-CORRECTED. Static RE (this pass)
        // of the msg-0x10 handler seg85:0x37a0 shows it is Word's OWN initial-render
        // command: gate1 (seg85:0x3faa, arg=1) -> if 0, the handler does
        //   OR [0x344],8   (set paint bit3)
        //   then the page builders 0x3846:0x3e / 0x3855:0x19da / 0x3831:0x25c,
        // laying out + painting the document IN Word's natural wndproc context.
        // gate1 returns 0 iff [0x4d0]==0, [0x337]&0x20==0, [0x33f]&4==0, [0x4c8]<3,
        // [0x4ce]==0, AND ([0x15fe]==0 OR SDM.90([0x15fe])==0). At runtime all five
        // early flags are 0 (pass); the ONLY blocker was [0x15fe] (an SDM.DLL
        // re-entrancy scratch, non-zero only transiently) being non-zero at the one
        // moment pass 37 fired -> SDM.90 -> gate bailed. Fire ONLY while [0x15fe]==0
        // (SDM at rest), retrying across pump iterations, so the gate passes and
        // Word renders itself. Word-gated (segcount>=100); a no-op for games.
        static int s_w6_docready = 0;
        if (!s_w6_docready && g_info.segcount >= 100 && g_cpu && g_info.ds
            && !win16_find_toplevel_by_title("Tip of the Day") && g_win16_focus) {
            uint16_t view  = x86_16_rd16(g_cpu, g_info.ds, 0x1d18);
            uint16_t sdm   = x86_16_rd16(g_cpu, g_info.ds, 0x15fe);
            uint16_t f4d0  = x86_16_rd16(g_cpu, g_info.ds, 0x04d0);
            uint16_t f4c8  = x86_16_rd16(g_cpu, g_info.ds, 0x04c8);
            uint16_t f4ce  = x86_16_rd16(g_cpu, g_info.ds, 0x04ce);
            uint16_t g344b = x86_16_rd16(g_cpu, g_info.ds, 0x0344);
            // Only fire when the SDM scratch is at rest so gate1 does not bail on
            // SDM.90, AND when the other five gate flags are already in the pass
            // state (they are, at rest). This retries every pump pass until the
            // conditions line up rather than burning the one shot on a bad moment.
            int gate_ok = (sdm == 0) && (f4d0 == 0) && (f4ce == 0) && (f4c8 < 3);
            if (view && gate_ok) {
                uint16_t frame = g_win16_focus;
                for (int g = 0; g < 8; g++) {
                    win16_window_t *fw = win_from_hwnd(frame);
                    if (!fw || fw->parent == 0) break;
                    frame = fw->parent;
                }
                win16_window_t *fwp = win_from_hwnd(frame);
                if (fwp) {
                    s_w6_docready = 1;
                    kprintf("[W6RDY49] fire msg0x10 view=%04x frame=%04x [15fe]=%04x "
                            "[4d0]=%04x [4c8]=%04x [4ce]=%04x [344]before=%04x\n",
                            view, frame, sdm, f4d0, f4c8, f4ce, g344b);
                    win16_call_wndproc(fwp->proc_seg, fwp->proc_off, frame,
                                       0x0010 /*Word internal doc-ready*/, view, 0);
                    uint16_t g344a = x86_16_rd16(g_cpu, g_info.ds, 0x0344);
                    kprintf("[W6RDY49] after msg0x10 [344]after=%04x (bit3=%d)\n",
                            g344a, (g344a & 0x08) ? 1 : 0);
                    // Repaint so the freshly laid-out page shows immediately.
                    win16_paint_all_z();
                    g_win16_z_dirty = 0;
                    // (#278 pass61) The initial-render (msg-0x10) set paint-ready
                    // bit3 in [0x344] ONLY NOW, AFTER the document view window's
                    // early WM_PAINTs already ran with bit3 clear (the nodraw /
                    // request-repaint path) and left the page gray. Real Word
                    // repaints the view once the doc is ready; our frame cascade in
                    // win16_paint_all_z does not re-enter the deeply nested MDI edit
                    // pane, so deliver that WM_PAINT explicitly to the doc view
                    // window(s) now that bit3 is set. Word-gated + one-shot (guarded
                    // by s_w6_docready), so games are unaffected.
                    // (#278 p61) clear paint-ready bit3 so the edit pane takes the
                    // FULL-paint dispatcher (seg222:0x2048) that iterates the now-built
                    // display list, not the fast cached-blit (bit3-set) path; and run
                    // it under soft-repaint so BeginPaint uses the FULL client clip (a
                    // partial clip only paints part of the page width).
                    { uint16_t v=x86_16_rd16(g_cpu,g_info.ds,0x0344);
                      x86_16_wr16(g_cpu,g_info.ds,0x0344,(uint16_t)(v & ~0x08)); }
                    { int prevsr = g_win16_in_soft_repaint;
                      g_win16_in_soft_repaint = 1;
                      for (int wi = 0; wi < WIN16_MAX_WINDOWS; wi++) {
                          win16_window_t *ew = &g_windows[wi];
                          if (ew->used && ew->shown && ew->is_child
                              && ew->proc_seg == 0x06f7 && ew->proc_off == 0x1f6a) {
                              ew->inval_valid = 0;
                              win16_dispatch_to_window(ew->hwnd, WM_PAINT, 0, 0);
                          }
                      }
                      g_win16_in_soft_repaint = prevsr;
                    }
                }
            }
        }
    }
    // Mouse: sample the cursor + buttons and post client/menu messages.
    win16_pump_mouse();

    // Timers: post WM_TIMER for each elapsed timer and re-arm.
    uint64_t now = timer_ticks;
    for (int i = 0; i < WIN16_MAX_TIMERS; i++) {
        if (!g_timers[i].used) continue;
        if ((int64_t)(now - g_timers[i].next_fire) >= 0) {
            msgq_post(g_timers[i].hwnd, WM_TIMER, g_timers[i].id, 0);
            posted++;
            // Re-arm relative to now to avoid a burst if the loop fell behind.
            g_timers[i].next_fire = now + g_timers[i].period_ticks;
        }
    }
    // (#255 perf C) Any posted message (key/mouse/timer) means visible content
    // may change this frame, so mark the frame dirty for the idle-loop repaint.
    if (posted > 0) g_win16_frame_dirty = 1;
    return posted;
}

// PeekMessage (USER.109): BOOL PeekMessage(LPMSG lpMsg, HWND hWnd, UINT wMsgMin,
// UINT wMsgMax, UINT wRemoveMsg) PASCAL = 6 words = 12 bytes. NON-BLOCKING:
// returns TRUE and fills lpMsg if a message is available, else returns FALSE
// immediately. (#200 SkiFree) SkiFree's game loop is a PeekMessage(PM_NOREMOVE)
// busy loop: when running+focused and the queue is EMPTY it calls its per-frame
// slope-advance routine, then loops. We previously mapped USER.109 to the
// BLOCKING GetMessage, so the loop blocked on an empty queue and the advance
// never ran (the slope rendered but never scrolled). A real non-blocking peek
// lets the loop fall through to the advance, so the skier descends.
// Stack top->bottom: wRemoveMsg[0], wMsgMax[1], wMsgMin[2], hWnd[3],
// lpMsg off[4], lpMsg seg[5].
static int win16_pump_input(void);   // fwd
static void u_peekmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    { extern int g_w6life; if (g_w6life && g_info.segcount>=100){ static uint32_t n=0; n++;
      if (n<=30 || (n & 0xFFF)==0)
        kprintf("[W6PMSG] n=%u caller=%04x:%04x\n", (unsigned)n,
          x86_16_rd16(c,c->ss,(uint16_t)(c->sp+2)), x86_16_rd16(c,c->ss,c->sp)); } }
    uint16_t remove  = arg16(c, 0);
    /* wMsgMax */ (void)arg16(c, 1);
    /* wMsgMin */ (void)arg16(c, 2);
    /* hWnd    */ (void)arg16(c, 3);
    uint16_t msg_off = arg16(c, 4);
    uint16_t msg_seg = arg16(c, 5);
    *argbytes = 12;

    // Honour a pending close/quit just like GetMessage so the app can exit.
    if (g_win16_close_requested) { g_quit_posted = 1; g_win16_close_requested = 0; }

    // Pump real input (keyboard/mouse/timers) into the queue, then peek.
    win16_pump_input();

    // (#200 ski-flicker) SkiFree (app_kind 3) draws its sprites every frame with
    // NO background erase (GetDC + opaque SRCCOPY) and relies on the OS to clear
    // the slope. Run the flicker-free full repaint (erase + full redraw into the
    // off-screen scratch + atomic publish) on EVERY peek, throttled to ~20 Hz, so
    // the erased full frame is the steady render path: no trails, and no
    // clean<->trail alternation when the cursor floods the queue (the old
    // empty-queue-only repaint was the source of the cursor-dependent flicker).
    // We deliberately do NOT intercept WM_PAINT or alter the message flow, so
    // SkiFree's own loop still reaches its idle branch and the world keeps
    // scrolling autonomously (paced by GetTickCount).
    if (g_win16_app_kind == 3) {
        static uint64_t s_ski_pk_paint;
        uint32_t shz = g_timer_hz ? g_timer_hz : 250;
        uint64_t sintv = (shz / 20) ? (shz / 20) : 1;
        if (timer_ticks - s_ski_pk_paint >= sintv) {
            s_ski_pk_paint = timer_ticks;
            win16_soft_repaint_focus();
        }
    }

    win16_msg_t m;
    int have;
    if (g_quit_posted && g_msgq_count == 0) {
        m.hwnd = 0; m.message = WM_QUIT; m.wParam = 0; m.lParam = 0; have = 1;
    } else {
        // PM_NOREMOVE (bit0==0): peek without dequeuing. PM_REMOVE (bit0==1): pop.
        if (remove & 0x0001) have = msgq_get(&m);
        else                 have = msgq_peek(&m);
    }
    if (have) { msg_write(c, msg_seg, msg_off, &m); *ax = 1; }
    else      { *ax = 0; }
    *dx = 0;

    // (#200/#205) SkiFree's PeekMessage(PM_NOREMOVE) game loop spins flat-out
    // when the queue is empty (it calls its per-frame slope-advance, which is
    // self-paced by GetTickCount, then peeks again). Without a yield this pegs
    // the host CPU and starves the compositor. Sleep a short slice on an empty
    // peek so the proc leaves the ready queue; the slope still advances smoothly
    // because the advance reads real elapsed time, and we cap the sleep so the
    // frame rate stays high (~125 Hz). Only throttle on a *run* of empty peeks so
    // a burst of real input stays snappy.
    if (!have) {
        static uint64_t s_pk_tick; static uint32_t s_pk_empty;
        uint64_t t = timer_ticks;
        if (t != s_pk_tick) { s_pk_tick = t; s_pk_empty = 0; }

        // (#200 ski-erase) Periodic flicker-free full repaint for PeekMessage-driven
        // games. SkiFree (app_kind 3) runs a PeekMessage(PM_NOREMOVE) busy loop and
        // draws every visible sprite each frame with an OPAQUE SRCCOPY BitBlt via
        // GetDC; it NEVER calls InvalidateRect/FillRect/PatBlt, so it relies on the
        // OS to re-establish the (white) slope background each frame. The periodic
        // repaint that does this (win16_soft_repaint_focus, which bg-fills each
        // client and re-dispatches WM_PAINT) lives ONLY in the GetMessage idle loop,
        // which a PeekMessage app never enters. So old sprite pixels were never
        // erased: moving sprites left trails and the slope only cleared when a
        // crash/restart issued an InvalidateRect (which forced a WM_PAINT). Verified
        // by serial blit-trace: 0 PatBlt, 0 FillRect, 0 InvalidateRect, 0 SOFT
        // repaints during play; every visible blit was a lone SRCCOPY sprite.
        // Fix: run the same flicker-free soft repaint from the PeekMessage path,
        // throttled to ~15 Hz. It renders into an off-screen scratch buffer and
        // publishes in one memcpy, so the compositor never sees a half-cleared
        // frame (no flicker), and SkiFree's WM_PAINT redraws the slope cleanly with
        // no accumulated trails. Gated to app_kind 3 ONLY, so TETRIS (kind 1),
        // FreeCell/Chips (kind 2) and generic apps (kind 0) are untouched and keep
        // their existing paint paths (this is why the disabled ROP-aware PatBlt, the
        // #219 TETRIS-protection, is irrelevant here and left exactly as it was).
        // (#200 ski-flicker) The app_kind-3 soft repaint now runs on EVERY peek
        // above (not just empty-queue), so nothing extra is needed here.

        if (++s_pk_empty > 2) {
            extern void proc_sleep(uint32_t ms);
            extern void proc_yield(void);
            // (#255 perf A) A foreground GAME runs a self-paced PeekMessage busy
            // loop; proc_sleep(2) rounds up to one PIT tick (~10ms at 100Hz),
            // which caps its frame rate hard. Yield instead so the compositor
            // still runs but the game is not throttled to ~tick granularity.
            // Generic Win16 apps (kind 0, e.g. a calculator) keep the sleep so an
            // idle app does not peg the CPU (#193).
            if (g_win16_app_kind != 0) proc_yield();
            else                       proc_sleep(2);
        }
    }
}

static void u_getmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    { extern int g_w6life; if (g_w6life && g_info.segcount>=100){ static uint32_t n=0; n++;
      if (n<=30 || (n & 0xFF)==0)
        kprintf("[W6GMSG] n=%u caller=%04x:%04x\n", (unsigned)n,
          x86_16_rd16(c,c->ss,(uint16_t)(c->sp+2)), x86_16_rd16(c,c->ss,c->sp)); } }

    /* wMsgMax */ (void)arg16(c, 0);
    /* wMsgMin */ (void)arg16(c, 1);
    /* hWnd    */ (void)arg16(c, 2);
    uint16_t msg_off = arg16(c, 3);
    uint16_t msg_seg = arg16(c, 4);
    *argbytes = 10;

    // (#215) Titlebar CLOSE (X): even a runaway app whose queue is never empty
    // (so it never reaches the idle-loop pump) passes through GetMessage every
    // dispatch. Latch the close request into g_quit_posted here so it is honoured
    // promptly mid-game (e.g. TETRIS) without waiting for the app to idle.
    if (g_win16_close_requested) {
        g_quit_posted = 1;
        g_win16_close_requested = 0;
    }

    // If a quit was requested (e.g. ESC pressed), return WM_QUIT immediately so
    // the app's message loop exits even when its own queue is full of timer
    // messages (which would otherwise drop the posted WM_QUIT and hang the app).
    if (g_quit_posted) {
        win16_msg_t q; q.hwnd = 0; q.message = WM_QUIT; q.wParam = 0; q.lParam = 0;
        msg_write(c, msg_seg, msg_off, &q);
        *ax = 0; *dx = 0;
        return;
    }

    // (#205) Runaway message-pump throttle. Some Win16 apps (e.g. TETRIS in its
    // running state) keep their message queue perpetually non-empty by reposting
    // a command to themselves every dispatch, so GetMessage NEVER takes the
    // empty-queue idle path below and the guest spins at ~98% CPU. Cap how many
    // times GetMessage may return a message within a single kernel timer tick:
    // once we exceed a sane per-tick budget we proc_sleep(2) so the proc leaves
    // the ready queue and the host CPU is released. A normal app whose pump is
    // paced by input/WM_TIMER stays well under this budget and is never slowed.
    {
        static uint64_t s_gm_tick;       // last tick we counted within
        static uint32_t s_gm_in_tick;    // GetMessage returns so far this tick
        uint64_t t = timer_ticks;
        if (t != s_gm_tick) { s_gm_tick = t; s_gm_in_tick = 0; }
        // Allow a generous burst per tick (~one frame of work) then throttle.
        if (g_msgq_count > 0 && ++s_gm_in_tick > 64) {
            extern void proc_sleep(uint32_t ms);
            extern void proc_yield(void);
            // (#255 perf A) For a foreground game, yield rather than sleep so a
            // timer-reposting game loop is not capped to PIT-tick granularity;
            // generic apps keep the sleep to avoid a true runaway pegging CPU.
            if (g_win16_app_kind != 0) proc_yield();
            else                       proc_sleep(2);
        }
    }

    // Interactive GetMessage: block until a message is available, pumping real
    // keyboard + SetTimer events into the queue so the app's own message loop
    // (while(GetMessage){TranslateMessage;DispatchMessage;}) drives the game.
    // The window is repainted at ~15 Hz while idle so timer/key-driven changes
    // become visible. An overall idle cap guards against a runaway/hung app.
    // ---- Auto-start TETRIS via its OWN menu commands ----------------------
    // Reverse-engineering of TETRIS.EXE seg1 (clean-room, see the win16 notes)
    // established the real Game/Skill menu IDs and the playfield state machine:
    //   * The "&Game / &New" item is WM_COMMAND id 0x7c (124), and the F2
    //     accelerator maps to the same id. New (handler @seg1:0x1622) reloads the
    //     last selected level from autodata[0x152] and jumps into the level path.
    //   * The "Starting &Level N" items are WM_COMMAND ids 0x6e..0x77 (110..119);
    //     they all route to the level handler @seg1:0x15b2, which runs the board
    //     init routine (@0x471e -> @0x4912) that sets the per-player state byte to
    //     2 ("READY-TO-SPAWN") and clears the pause flag autodata[0x1fa0].
    //   * autodata[0x164] is the PLAYER COUNT (1 or 2), NOT a game-active flag.
    //     The prior build poked it to 2, which silently put the game in 2-player
    //     mode and never spawned a piece.
    //
    // The per-player gravity state machine (driven by WM_TIMER id 0, handler
    // @seg1:0x9bc) and the spawn routine @seg1:0x5d4e together require, to spawn
    // the first tetromino: per-player state == 2 (READY, processed by the timer),
    // the inter-piece delay [0x14e] == 0, and the "started" flag [0x158] != 0.
    // In single-player mode the level handler's tail leaves the state at 5 (idle,
    // which the timer ignores) and never sets [0x158] (that path is only taken for
    // the SECOND player), so a real user would have to make the first move for it
    // to come alive. We therefore drive the genuine Level menu command and then
    // satisfy exactly those preconditions so the app's OWN timer/spawn code (its
    // LCG piece generator @0x6f7e, drop routine @0x495a) generates, drops and
    // scores pieces. Per-player state bytes: [0x14a] (player 0), [0x14c] (player 1).
    //
    // Staged across successive GetMessage idle passes so each posted command is
    // actually dispatched before the next step runs:
    //   step 0: post the real Game->Starting-Level-1 command (0x6e).
    //   step 1: once the level handler has recorded the chosen level in [0x152]
    //           (proving board init ran), arm the live-game maintenance below.
    // Card games (any app that loaded CARDS.DLL): deal one new game by posting the
    // app's OWN F2 -> New Game accelerator command (generic, per RT_ACCELERATOR),
    // then stop. We must NOT run the TETRIS poke logic below for these: it writes
    // TETRIS-specific autodata offsets that would corrupt a card game's DS.
    if (g_win16_app_kind == 2) {
        if (g_autostart_done == 0 && g_cpu) {
            uint16_t f = win16_focus_hwnd();
            if (f) {
                uint16_t cmd = win16_accel_cmd_for_vk(0x71 /*VK_F2*/);
                if (cmd) {
                    msgq_post(f, WM_COMMAND, cmd, 0);   // e.g. FreeCell 0x66
                    win16_trace("cardgame deal: post WM_COMMAND 0x%x\n", cmd);
                }
                g_autostart_done = 2;   // one-shot; no TETRIS pokes
            }
        }
    }
    // ---- Auto-start TETRIS the GENUINE way (#210) -------------------------
    // The prior build poked reverse-engineered autodata offsets (0x14a/0x158/
    // 0x1fa0) to force the playfield state machine. Those offsets were WRONG
    // (the diagnostic px/py at 0x140/0x142 read back as ASCII garbage, e.g.
    // 29793/29295 = "ta"/"or", i.e. they pointed into a string in DS), so the
    // poke only corrupted DS and never spawned a piece. That hack is REMOVED.
    //
    // Instead we drive TETRIS's OWN menu commands, parsed from its RT_MENU /
    // RT_ACCELERATOR resources:
    //   * Skill -> Starting Level -> &1   = WM_COMMAND id 0x6e (110)
    //   * Game  -> &New (F2 accelerator)  = WM_COMMAND id 0x7c (124)
    // The New handler runs the app's real board-init + first-piece spawn, after
    // which the app's OWN WM_TIMER gravity routine drops the piece. We only post
    // the commands the game itself would receive from the user, then stop. No
    // direct writes to the game's data segment.
    //
    // Staged across GetMessage idle passes so each command is dispatched before
    // the next is posted:
    //   step 0: post Starting-Level-1 (0x6e) to set a default level.
    //   step 1: post Game->New (0x7c) to actually start the game / spawn a piece.
    //   step 2: done.
    if (g_win16_app_kind == 1 && g_autostart_done < 2 && g_cpu) {
        static uint64_t s_step_t;
        uint32_t hz = g_timer_hz ? g_timer_hz : 250;
        // Menu commands must go to the window that owns the menu (the game's main
        // frame), NOT whatever transient dialog currently holds focus.
        uint16_t target = win16_top_game_hwnd();
        { static uint64_t s_d; if (timer_ticks - s_d >= hz) { s_d = timer_ticks;
            kprintf("[TETAS] diag done=%d target=%04x focus=%04x elapsed=%u need=%u\n",
                g_autostart_done, target, win16_focus_hwnd(),
                (unsigned)(timer_ticks - s_step_t), (unsigned)(hz/4?hz/4:1)); } }
        // At launch TETRIS puts up an "All Rights Reserved" splash (a modal
        // DialogBox running its own message pump). While that is up, focus_hwnd()
        // returns the splash (top z, no menu) but the menu-owning frame
        // (top_game_hwnd) is behind it, so the two differ. Commands posted then
        // are swallowed by the modal pump, so we must first dismiss the splash
        // the way a user would (press a key), then issue the level + New commands
        // once the real frame is frontmost.
        uint16_t focus_now = win16_focus_hwnd();
        int frame_front = (focus_now == target);
        if (target && !frame_front) {
            // Splash / dialog overlays the frame. Periodically send Enter to it
            // so it closes (MS splash dismisses on any key / Enter / click).
            static uint64_t s_dismiss_t;
            if (timer_ticks - s_dismiss_t >= (hz / 2 ? hz / 2 : 1)) {
                s_dismiss_t = timer_ticks;
                msgq_post(focus_now, WM_KEYDOWN, 0x0D /*VK_RETURN*/, 0);
                msgq_post(focus_now, WM_CHAR,    0x0D, 0);
                kprintf("[TETAS] dismiss splash hwnd=%04x (Enter)\n", focus_now);
            }
        }
        if (target && frame_front) {
            if (g_autostart_done == 0) {
                msgq_post(target, WM_COMMAND, 0x6e, 0);   // Skill -> Level 1
                kprintf("[TETAS] post Starting-Level-1 (0x6e) hwnd=%04x\n", target);
                s_step_t = timer_ticks;
                g_autostart_done = 1;
            } else if (timer_ticks - s_step_t >= (hz / 4 ? hz / 4 : 1)) {
                // Settle one beat so the level command is dispatched, then fire
                // the real New Game command (F2 / Game->New). The game's own
                // handler does the board init + first-piece spawn; the app's own
                // WM_TIMER gravity then drops it.
                msgq_post(target, WM_COMMAND, 0x7c, 0);   // Game -> New (spawn)
                kprintf("[TETAS] post Game->New (0x7c) hwnd=%04x\n", target);
                g_autostart_done = 2;
            }
        }
    }


    // Flush the trace to disk every time the app idles in GetMessage, so the
    // post-deal paint sequence is captured even while an interactive app stays up
    // (it never reaches win16_api_end). Cheap relative to the idle wait.
    // (#205) Only write the trace to disk when it has actually GROWN since the
    // last flush. The old code did a full fat_write_file of the whole buffer on
    // EVERY GetMessage call; when an app's message loop spins through GetMessage
    // rapidly (queue refilled each pass), that turned into a flood of disk
    // writes that by itself burned a large slice of CPU. Gating on growth keeps
    // the diagnostic intact (post-deal paint sequence still captured) at near
    // zero idle cost.
    if (g_trace_len > 0 && g_trace_len != g_trace_last_flush_len) {
        g_trace_flushed = 1;
        g_trace_last_flush_len = g_trace_len;
        fat_write_file(&g_fat_fs, "/WIN16LOG.TXT", g_trace, g_trace_len);
    }

    win16_msg_t m;
    if (!msgq_get(&m)) {
        uint32_t hz = g_timer_hz ? g_timer_hz : 250;
        uint64_t idle_start = timer_ticks;
        uint64_t idle_cap   = (uint64_t)hz * 180;   // ~3 min hard idle ceiling
        uint64_t last_paint = 0;
        uint64_t last_soft_paint = 0;   // (#255 perf C) last actual soft repaint
        uint64_t paint_intv = (hz / 15) ? (hz / 15) : 1;
        for (;;) {
            if (g_quit_posted && g_msgq_count == 0) {
                m.hwnd = 0; m.message = WM_QUIT; m.wParam = 0; m.lParam = 0;
                break;
            }
            win16_pump_input();
            // (#200 SkiFree) SkiFree starts PAUSED and, with its WM_TIMER killed,
            // BLOCKS inside this single GetMessage idle wait until a key arrives.
            // The top-of-function autostart only armed (it cannot run its second
            // stage because the function never returns to be re-entered while
            // blocked here), so post VK_F3 from inside the idle loop once the arm
            // delay has elapsed. msgq_get below then hands SkiFree the WM_KEYDOWN
            // that unpauses it; its New-game path re-arms the WM_TIMER -> the
            // slope scrolls. One-shot via g_autostart_done.
            if (msgq_get(&m)) break;
            // Periodic idle repaint so animated content stays fresh on screen.
            // Card games (kind 2) draw a STATIC board incrementally via their own
            // InvalidateRect; once the deal has been issued, stop force-erasing or
            // we would wipe the cards between the app's deal-animation frames (the
            // board would flash green then go blank). Let the app own painting.
            int do_periodic = !(g_win16_app_kind == 2 && g_autostart_done >= 2);
            win16_paint_flush();   // (#219) clear any leaked paint redirection
            // (#bugB) Heal Word's self-drawn menu strip on the real canvas when idle, but
            // throttled to the same cadence as the periodic repaint (not every tight idle
            // spin) so it adds no measurable idle cost. Covers repaint paths that blank
            // the menu without a main-window EndPaint (e.g. a modal dialog closing). No-op
            // for apps with no top menu strip (games) or a tracked menu.
            if (do_periodic && timer_ticks - last_paint >= paint_intv)
                win16_menu_strip_maintain();
            if (do_periodic && timer_ticks - last_paint >= paint_intv) {
                last_paint = timer_ticks;
                if (g_win16_z_dirty) {
                    // (#209) z-order changed (popup/dialog/show/move): full
                    // z-repaint, which fills each client bg and erases stale
                    // popup regions, then settle the dirty flag.
                    win16_paint_all_z();   // (#207) z-ordered so popups stay on top
                    g_win16_z_dirty = 0;
                } else if (g_win16_frame_dirty ||
                           (timer_ticks - last_soft_paint) >= (uint64_t)(hz / 2 ? hz / 2 : 1)) {
                    // (#255 perf C) Only run the full repaint (2x canvas memcpy +
                    // guest WM_PAINT re-dispatch) when content actually changed
                    // since the last frame, with a ~2 Hz safety floor so nothing
                    // can stall. g_win16_frame_dirty is set by win16_pump_input
                    // whenever it posts input/timer messages. This stops the
                    // engine from re-running the guest's paint code 15x/s on
                    // frames where nothing moved.
                    g_win16_frame_dirty = 0;
                    last_soft_paint = timer_ticks;
                    // (#209) quiet frame: refresh only the focused window's
                    // animated content WITHOUT clearing its client to bg, so the
                    // playfield does not flash grey<->black between frames.
                    win16_soft_repaint_focus();
                }
            }
            // Keep an open menu popup painted on top of whatever the app drew
            // (runs even for card games whose periodic repaint is suppressed).
            if (g_w16_menu_open >= 0) {
                win16_window_t *fw = win_from_hwnd(win16_focus_hwnd());
                if (fw && fw->hmenu && fw->hmenu <= WIN16_MAX_MENUS &&
                    g_menus[fw->hmenu - 1].used)
                    win16_draw_menu_popup(&g_menus[fw->hmenu - 1], g_w16_menu_open);
                else
                    g_w16_menu_open = -1;
            }
            if (g_w16_menu_prev >= 0 && g_w16_menu_open < 0) {
                // (#197/#219) menu just closed: restore the canvas bits saved from
                // under the popup so the app content beneath returns EXACTLY, with
                // no full-client erase (which wiped persistent content such as
                // Golf's dealt cards). Refresh the menu bar to clear any highlight.
                win16_menu_restore_under();
                win16_window_t *fw = win_from_hwnd(win16_focus_hwnd());
                if (fw && fw->hwnd == g_win16_main_hwnd && fw->hmenu)
                    win16_draw_menubar(fw);
                g_win16_z_dirty = 0;      // (#209) just settled it
            }
            g_w16_menu_prev = g_w16_menu_open;
            if (timer_ticks - idle_start >= idle_cap) {
                kprintf("[win16api] GetMessage idle cap reached -> quitting\n");
                g_quit_posted = 1;
                m.hwnd = 0; m.message = WM_QUIT; m.wParam = 0; m.lParam = 0;
                break;
            }
            // (#205/#209) Throttle the idle wait so an app sitting in its
            // GetMessage loop with nothing to do does NOT peg the CPU. The old
            // code called proc_yield() here, which only gives up the rest of the
            // slice but leaves this kernel proc RUNNABLE, so the scheduler keeps
            // re-dispatching it immediately -> ~98% guest CPU, starving the
            // compositor (TETRIS flicker + dropped input) and saturating the
            // host. Mirror the DOS path (dosexec.c): sleep between polls so the
            // proc actually leaves the ready queue and the kernel idle proc can
            // hlt (#180 stays intact). We wake EARLY for the next real event so
            // active games stay snappy: compute the soonest of (the next periodic
            // idle repaint) and (the next SetTimer fire), and sleep only until
            // then, floored so we never tight-spin and capped at 12ms so async
            // keyboard input still polls at ~80 Hz.
            uint64_t now_t = timer_ticks;
            uint64_t next_deadline = now_t + (uint64_t)hz;   // 1s fallback
            // Next periodic repaint deadline (only if periodic repaint is active).
            if (do_periodic) {
                uint64_t pd = last_paint + paint_intv;
                if (pd > now_t && pd < next_deadline) next_deadline = pd;
                else if (pd <= now_t) next_deadline = now_t;  // due now
            }
            // Soonest armed Win16 timer (WM_TIMER drives game animation).
            for (int ti = 0; ti < WIN16_MAX_TIMERS; ti++) {
                if (!g_timers[ti].used) continue;
                uint64_t nf = g_timers[ti].next_fire;
                if (nf <= now_t) { next_deadline = now_t; break; }
                if (nf < next_deadline) next_deadline = nf;
            }
            // Convert remaining ticks -> ms, floor 2ms, cap 12ms.
            uint32_t sleep_ms;
            if (next_deadline <= now_t) {
                sleep_ms = 2;
            } else {
                uint64_t ticks_left = next_deadline - now_t;
                uint64_t ms = (ticks_left * 1000ULL) / (hz ? hz : 250);
                if (ms < 2)  ms = 2;
                if (ms > 12) ms = 12;
                sleep_ms = (uint32_t)ms;
            }
            extern void proc_sleep(uint32_t ms);
            proc_sleep(sleep_ms);
        }
    }
    msg_write(c, msg_seg, msg_off, &m);
    { extern int g_w6life; if (g_w6life && g_info.segcount>=100 &&
        (m.message==WM_KEYDOWN||m.message==WM_KEYUP||m.message==WM_CHAR))
        kprintf("[W6GMRET] GetMessage -> msg=%04x hwnd=%04x wp=%04x\n",
                m.message, m.hwnd, m.wParam); }
    *ax = (m.message == WM_QUIT) ? 0 : 1;
    *dx = 0;
}

// TranslateMessage (USER.113): TranslateMessage(lpMsg far) = 2 words.
static void u_translatemessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}

// DispatchMessage (USER.114): DispatchMessage(lpMsg far) = 2 words. Reads the
// MSG and calls the target window's wndproc.
static void u_dispatchmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                              uint16_t *argbytes) {
    uint16_t msg_off = arg16(c, 0);
    uint16_t msg_seg = arg16(c, 1);
    uint16_t hwnd = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 0));
    uint16_t message = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 2));
    uint16_t wParam = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 4));
    uint32_t lParam = (uint32_t)x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 6)) |
                      ((uint32_t)x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 8)) << 16);
    { extern int g_w6life; if (g_w6life && g_info.segcount>=100 &&
        (message==WM_KEYDOWN||message==WM_KEYUP||message==WM_CHAR)) {
        win16_window_t *dw = win_from_hwnd(hwnd);
        kprintf("[W6DISP] DispatchMessage msg=%04x hwnd=%04x wp=%04x proc=%04x:%04x\n",
                message, hwnd, wParam, dw?dw->proc_seg:0, dw?dw->proc_off:0); } }
    uint32_t r = win16_dispatch_to_window(hwnd, message, wParam, lParam);
    *ax = (uint16_t)(r & 0xFFFF); *dx = (uint16_t)(r >> 16); *argbytes = 4;
}

// GetSystemMetrics (USER.179): GetSystemMetrics(nIndex) = 1 word (s_word).
static void u_getsystemmetrics(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    int16_t idx = (int16_t)arg16(c, 0);
    uint16_t v = 0;
    switch (idx) {
        case SM_CXSCREEN: v = (uint16_t)fb_get_width();  break;
        case SM_CYSCREEN: v = (uint16_t)fb_get_height(); break;
        case 4:  v = W16_TITLEBAR; break;   // SM_CYCAPTION
        case 5:  v = W16_BORDER;   break;   // SM_CXBORDER
        case 6:  v = W16_BORDER;   break;   // SM_CYBORDER
        case 32: v = 16; break;             // SM_CXCURSOR
        case 33: v = 16; break;             // SM_CYCURSOR
        default: v = 0; break;
    }
    *ax = v; *dx = 0; *argbytes = 2;
}

// Forward decls (decode_dib + gdiobj alloc are defined further below).
static uint32_t *decode_dib(const uint8_t *p, uint32_t len, int *out_w, int *out_h);
static int win16_alloc_gdiobj(int type, uint32_t color);

// (#192) Load an RT_GROUP_ICON / RT_GROUP_CURSOR directory, pick the best image,
// fetch the matching RT_ICON / RT_CURSOR DIB, decode it, and store the visible
// (XOR/colour) half as a GDI bitmap object. grpType = 14 (icon) or 12 (cursor),
// imgType = 3 (icon) or 1 (cursor). For cursors the RT_CURSOR resource is prefixed
// by a 2-WORD hotspot before the DIB header. Returns a gdiobj handle or 0.
static uint16_t win16_load_icon_cursor(x86_16_cpu_t *c, uint16_t name_off,
                                       uint16_t name_seg, uint16_t hinst,
                                       int grpType, int imgType) {
    uint32_t glen = 0; const uint8_t *grp = 0;
    if (name_seg == 0) {
        grp = win16_get_resource(hinst, (uint16_t)grpType, name_off, &glen);
    } else {
        char nm[40]; rd_far_cstr(c, name_seg, name_off, nm, sizeof(nm));
        if (nm[0] == '#') {
            uint16_t id = 0;
            for (int i = 1; nm[i] >= '0' && nm[i] <= '9'; i++) id = id * 10 + (nm[i] - '0');
            grp = win16_get_resource(hinst, (uint16_t)grpType, id, &glen);
        } else {
            grp = win16_get_resource_by_name(hinst, (uint16_t)grpType, nm, &glen);
        }
    }
    // Fall back to the first group of this type (apps often pass a stale hinst).
    if (!grp) grp = win16_get_resource_first(hinst, (uint16_t)grpType, &glen);
    if (!grp || glen < 6 + 14) return 0;
    // GRPICONDIR: WORD reserved, WORD type, WORD count; then `count` GRPICONDIRENTRY
    // (14 bytes each), last 2 bytes = the ordinal id of the RT_ICON/RT_CURSOR.
    uint16_t count = (uint16_t)(grp[4] | (grp[5] << 8));
    if (count == 0) return 0;
    int best = -1, best_score = -1;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = grp + 6 + (uint32_t)i * 14;
        if ((uint32_t)(e - grp) + 14 > glen) break;
        int w = e[0] ? e[0] : 256;
        int bc = (int)(e[6] | (e[7] << 8));     // bit count
        int score = w * 100 + bc;               // prefer larger + deeper
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) return 0;
    const uint8_t *be = grp + 6 + (uint32_t)best * 14;
    uint16_t img_id = (uint16_t)(be[12] | (be[13] << 8));
    uint32_t ilen = 0;
    const uint8_t *img = win16_get_resource(hinst, (uint16_t)imgType, img_id, &ilen);
    if (!img) return 0;
    const uint8_t *dib = img;
    if (imgType == 1 && ilen > 4) { dib = img + 4; ilen -= 4; }   // skip cursor hotspot
    int dw = 0, dh2 = 0;
    uint32_t *full = decode_dib(dib, ilen, &dw, &dh2);
    if (!full) return 0;
    // Icon/cursor DIBs store XOR colour rows + AND mask rows -> biHeight is 2x the
    // real height. decode_dib returns top-down; the colour half is the TOP half.
    int rh = dh2 / 2; if (rh <= 0) { rh = dh2; }
    uint32_t *pix = (uint32_t *)kmalloc((uint32_t)dw * rh * 4);
    if (!pix) { kfree(full); return 0; }
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < dw; x++) pix[y * dw + x] = full[y * dw + x];
    kfree(full);
    int obj = win16_alloc_gdiobj(4, 0);
    if (!obj) { kfree(pix); return 0; }
    g_gdiobj[obj].pix = pix; g_gdiobj[obj].w = dw; g_gdiobj[obj].h = rh; g_gdiobj[obj].bpp = 32;
    win16_trace("Load%s hinst=%04x -> obj=%d %dx%d (img #%u)\n",
                grpType == 14 ? "Icon" : "Cursor", hinst, obj, dw, rh, img_id);
    return (uint16_t)obj;
}

// LoadIcon (USER.174): LoadIcon(hInstance, lpIconName far) = 1+2 = 3w = 6 bytes.
static void u_loadicon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    uint16_t name_off = arg16(c, 0), name_seg = arg16(c, 1), hinst = arg16(c, 2);
    *argbytes = 6; *dx = 0;
    uint16_t h = win16_load_icon_cursor(c, name_off, name_seg, hinst, 14, 3);
    *ax = h ? h : 1;   // 1 = a non-NULL fallback handle so the caller proceeds
}

// LoadCursor (USER.173): LoadCursor(hInstance, lpCursorName far) = 6 bytes.
static void u_loadcursor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t name_off = arg16(c, 0), name_seg = arg16(c, 1), hinst = arg16(c, 2);
    *argbytes = 6; *dx = 0;
    // System cursors (hInstance == 0 with an IDC_* ordinal) have no app resource;
    // just return a stock handle.
    uint16_t h = (hinst == 0) ? 0 : win16_load_icon_cursor(c, name_off, name_seg, hinst, 12, 1);
    *ax = h ? h : 2;
}

// SetCursor (USER.69): SetCursor(hCursor) = 1 word. Return previous (0).
static void u_setcursor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// (#192) DrawIcon (USER.84): DrawIcon(HDC hdc, int X, int Y, HICON hIcon) =
//   1+1+1+1 = 4 words = 8 bytes. Blits the icon's colour pixels (loaded by
//   LoadIcon into a type-4 gdiobj) onto the DC's window client area at (X,Y).
//   Pixels with a zero/transparent value are skipped (poor-man's transparency).
static void u_drawicon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    uint16_t hicon = arg16(c, 0);
    int Y  = (int16_t)arg16(c, 1);
    int X  = (int16_t)arg16(c, 2);
    uint16_t hdc = arg16(c, 3);
    *argbytes = 8; *dx = 0; *ax = 1;
    win16_dc_t *dc = (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) ? &g_dcs[hdc] : 0;
    if (!dc) return;
    if (hicon == 0 || hicon >= WIN16_MAX_GDIOBJ || !g_gdiobj[hicon].used ||
        g_gdiobj[hicon].type != 4 || !g_gdiobj[hicon].pix) return;
    win16_gdiobj_t *ic = &g_gdiobj[hicon];
    for (int yy = 0; yy < ic->h; yy++)
        for (int xx = 0; xx < ic->w; xx++) {
            uint32_t px = ic->pix[yy * ic->w + xx];
            // Treat fully-black (0x000000) as transparent only for the AND-masked
            // background corners would be wrong; instead draw all colour pixels.
            dc_plot(dc, X + xx, Y + yy, px);
        }
}

// ===========================================================================
// (#193) MMSYSTEM audio: sndPlaySound / PlaySound. Resolve the named WAV from
// disk and play it through the kernel audio path (audio_play_file_async ->
// HDA/AC97). Best-effort: a missing file or an idle output stream is not fatal.
// SND_SYNC=0, SND_ASYNC=1, SND_NODEFAULT=2, SND_MEMORY=4, SND_LOOP=8. We honour
// the named-file case (the overwhelmingly common one); SND_MEMORY (in-RAM WAV)
// is played via audio_play_buffer after a minimal RIFF/WAVE parse.
// ===========================================================================
extern int audio_play_file_async(const char *path);
extern int audio_play_buffer(const void *data, uint32_t size, uint32_t format,
                             uint32_t sample_rate, uint32_t channels);

// Parse a RIFF/WAVE buffer already in kernel memory and submit its PCM via
// audio_play_buffer. Returns 1 on success. (Shared by file + SND_MEMORY paths.)
static int mm_submit_wav(const uint8_t *buf, uint32_t total) {
    if (!buf || total < 44) return 0;
    if (!(buf[0]=='R'&&buf[1]=='I'&&buf[2]=='F'&&buf[3]=='F'&&
          buf[8]=='W'&&buf[9]=='A'&&buf[10]=='V'&&buf[11]=='E')) return 0;
    uint16_t channels = 1; uint32_t rate = 11025; uint16_t bits = 8;
    const uint8_t *data = 0; uint32_t dsize = 0;
    uint32_t p = 12;
    while (p + 8 <= total) {
        uint32_t csz = (uint32_t)buf[p+4]|((uint32_t)buf[p+5]<<8)|
                       ((uint32_t)buf[p+6]<<16)|((uint32_t)buf[p+7]<<24);
        if (buf[p]=='f'&&buf[p+1]=='m'&&buf[p+2]=='t'&&buf[p+3]==' ' && p+24<=total) {
            channels = (uint16_t)(buf[p+10]|(buf[p+11]<<8));
            rate = (uint32_t)buf[p+12]|((uint32_t)buf[p+13]<<8)|
                   ((uint32_t)buf[p+14]<<16)|((uint32_t)buf[p+15]<<24);
            bits = (uint16_t)(buf[p+22]|(buf[p+23]<<8));
        } else if (buf[p]=='d'&&buf[p+1]=='a'&&buf[p+2]=='t'&&buf[p+3]=='a') {
            data = buf + p + 8;
            dsize = (p + 8 + csz <= total) ? csz : (total - p - 8);
            break;
        }
        p += 8 + csz + (csz & 1);
    }
    if (!data || !dsize) return 0;
    int rc = audio_play_buffer(data, dsize, bits, rate, channels);
    kprintf("[win16] WAV submit %lu bytes %uHz %uch %ubit rc=%d\n",
            (unsigned long)dsize, rate, channels, bits, rc);
    return rc >= 0;
}

static int mm_play_named(const char *dospath) {
    if (!dospath || !dospath[0]) return 0;     // NULL/empty = stop current sound (no-op)
    char path[160];
    dos_resolve_path(dospath, win16_get_appdir(), path, sizeof(path));
    // Read the WAV file and submit its PCM directly via audio_play_buffer, which
    // bypasses the kernel AudioDecode layer (that lacks a WAV decoder). This is
    // the path that actually produces output on the HDA/AC97 driver.
    uint32_t sz = 0;
    void *d = fat_read_file(&g_fat_fs, path, &sz);
    int ok = 0;
    if (d) {
        ok = mm_submit_wav((const uint8_t *)d, sz);
        if (!ok) { int rc = audio_play_file_async(path); ok = (rc >= 0); }  // fallback
        kfree(d);
    }
    win16_trace("PlaySound('%s' -> '%s') ok=%d sz=%lu\n", dospath, path, ok, (unsigned long)sz);
    kprintf("[win16] PlaySound '%s' -> '%s' ok=%d sz=%lu\n", dospath, path, ok, (unsigned long)sz);
    return ok;
}

// Parse a RIFF/WAVE buffer (fmt + data chunks) and submit the PCM. Returns 1 on
// a successful submit, 0 otherwise.
static int mm_play_memory(x86_16_cpu_t *c, uint16_t seg, uint16_t off) {
    uint8_t hdr[64];
    for (int i = 0; i < 64; i++) hdr[i] = x86_16_rd8(c, seg, (uint16_t)(off + i));
    if (!(hdr[0]=='R'&&hdr[1]=='I'&&hdr[2]=='F'&&hdr[3]=='F'&&
          hdr[8]=='W'&&hdr[9]=='A'&&hdr[10]=='V'&&hdr[11]=='E')) return 0;
    // Walk chunks to find fmt + data sizes (16-bit guest, cap copy at 256 KB).
    uint32_t riff_sz = (uint32_t)hdr[4] | ((uint32_t)hdr[5]<<8) |
                       ((uint32_t)hdr[6]<<16) | ((uint32_t)hdr[7]<<24);
    uint32_t total = riff_sz + 8;
    if (total > 256u*1024u) total = 256u*1024u;
    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return 0;
    for (uint32_t i = 0; i < total; i++) buf[i] = x86_16_rd8(c, seg, (uint16_t)(off + i));
    int ok = mm_submit_wav(buf, total);
    kfree(buf);
    return ok;
}

// sndPlaySound (MMSYSTEM.2): sndPlaySound(LPCSTR lpsz, UINT fuSound) =
//   far ptr (2w) + word = 6 bytes. Returns BOOL.
static void mm_sndplaysound(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t flags  = arg16(c, 0);
    uint16_t sz_off = arg16(c, 1), sz_seg = arg16(c, 2);
    *argbytes = 6; *dx = 0;
    int ok;
    if (flags & 0x0004) {                         // SND_MEMORY
        ok = mm_play_memory(c, sz_seg, sz_off);
    } else {
        if (sz_seg == 0 && sz_off == 0) { *ax = 1; return; }   // NULL = stop
        char name[160]; rd_far_cstr(c, sz_seg, sz_off, name, sizeof(name));
        ok = mm_play_named(name);
    }
    *ax = (uint16_t)(ok ? 1 : 0);
}

// PlaySound (MMSYSTEM.3): PlaySound(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound)
//   = far ptr (2w) + word + dword = 8 bytes.
static void mm_playsound(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t flags = arg32(c, 0);
    /* hmod */ (void)arg16(c, 2);
    uint16_t sz_off = arg16(c, 3), sz_seg = arg16(c, 4);
    *argbytes = 10; *dx = 0;
    int ok;
    if (flags & 0x0004) {                         // SND_MEMORY
        ok = mm_play_memory(c, sz_seg, sz_off);
    } else {
        if (sz_seg == 0 && sz_off == 0) { *ax = 1; return; }
        char name[160]; rd_far_cstr(c, sz_seg, sz_off, name, sizeof(name));
        ok = mm_play_named(name);
    }
    *ax = (uint16_t)(ok ? 1 : 0);
}

// mmsystemGetVersion (MMSYSTEM.5): no args. Return 0x030A (MMSYSTEM 3.10).
static void mm_getversion(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0x030A; *dx = 0; *argbytes = 0;
}

// ---------------------------------------------------------------------------
// DIB (device-independent bitmap) decoder for NE RT_BITMAP resources. The
// resource holds a BITMAPINFO (BITMAPINFOHEADER or old BITMAPCOREHEADER) with
// NO BITMAPFILEHEADER, followed by the colour table and bottom-up pixel rows.
// Decodes 1/4/8/24/32 bpp BI_RGB into a kmalloc'd top-down FB_COLOR buffer.
// ---------------------------------------------------------------------------
static uint32_t dib_rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t dib_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t *decode_dib(const uint8_t *p, uint32_t len, int *out_w, int *out_h) {
    if (!p || len < 12) return 0;
    uint32_t hdrsize = dib_rd32(p);
    int w, h, bpp, ncolors = 0, comp = 0, pal_entry;
    const uint8_t *pal;
    if (hdrsize == 12) {                      // BITMAPCOREHEADER
        w = (int)dib_rd16(p + 4); h = (int)dib_rd16(p + 6);
        bpp = (int)dib_rd16(p + 10);
        pal = p + 12; pal_entry = 3;
    } else if (hdrsize >= 40 && hdrsize < 512) {   // BITMAPINFOHEADER (or v4/v5)
        w = (int)dib_rd32(p + 4); h = (int)dib_rd32(p + 8);
        bpp = (int)dib_rd16(p + 14);
        comp = (int)dib_rd32(p + 16);
        ncolors = (int)dib_rd32(p + 32);
        pal = p + hdrsize; pal_entry = 4;
    } else {
        return 0;
    }
    int topdown = 0;
    if (h < 0) { h = -h; topdown = 1; }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    // BI_RGB(0) decoded by the linear path below; BI_RLE8(1, 8bpp) and
    // BI_RLE4(2, 4bpp) decoded by the RLE path (Chip's Challenge tiles are
    // RLE4). Anything else is unsupported.
    if (comp != 0 &&
        !(comp == 1 && bpp == 8) && !(comp == 2 && bpp == 4)) return 0;
    if (bpp <= 8) {
        int defc = 1 << bpp;
        if (ncolors <= 0 || ncolors > defc) ncolors = defc;
    }
    const uint8_t *bits = pal + ((bpp <= 8) ? (uint32_t)ncolors * pal_entry : 0);
    if (bits < p || (uint32_t)(bits - p) > len) return 0;
    int rowbytes = ((w * bpp + 31) / 32) * 4;
    uint32_t *out = (uint32_t *)kmalloc((uint32_t)w * h * 4);
    if (!out) return 0;
    if (comp != 0) {
        // RLE4 (comp==2, 4bpp) / RLE8 (comp==1, 8bpp): decompress the run-length
        // stream into a palette-index grid (DIB RLE is bottom-up), then map
        // through the colour table. This is what makes Chip's Challenge tiles
        // render (they ship as RLE4) instead of failing as "not enough memory".
        uint8_t *ib = (uint8_t *)kmalloc((uint32_t)w * h);
        if (!ib) { kfree(out); return 0; }
        for (uint32_t i = 0; i < (uint32_t)w * h; i++) ib[i] = 0;
        const uint8_t *s = bits, *send = p + len;
        int cx = 0, cy = h - 1;
        while (s + 1 < send && cy >= 0) {
            uint8_t cnt = *s++, val = *s++;
            if (cnt) {                              // encoded run of cnt pixels
                for (int i = 0; i < cnt; i++) {
                    if (cx >= w || cy < 0) break;
                    int idx = (comp == 2)
                        ? ((i & 1) ? (val & 0x0F) : (val >> 4))
                        : (int)val;
                    ib[(uint32_t)cy * w + cx] = (uint8_t)idx; cx++;
                }
            } else if (val == 0) {                  // end of line
                cx = 0; cy--;
            } else if (val == 1) {                  // end of bitmap
                break;
            } else if (val == 2) {                  // delta
                if (s + 1 >= send) break;
                cx += *s++; cy -= *s++;
            } else {                                // absolute run of `val` pixels
                int n = val;
                if (comp == 2) {
                    for (int i = 0; i < n; i++) {
                        if (s + (i >> 1) >= send) break;
                        int idx = (i & 1) ? (s[i >> 1] & 0x0F) : (s[i >> 1] >> 4);
                        if (cx < w && cy >= 0) ib[(uint32_t)cy * w + cx++] = (uint8_t)idx;
                    }
                    int nb = (n + 1) / 2; if (nb & 1) nb++;   // pad to word
                    s += nb;
                } else {
                    for (int i = 0; i < n; i++) {
                        if (s + i >= send) break;
                        if (cx < w && cy >= 0) ib[(uint32_t)cy * w + cx++] = s[i];
                    }
                    s += n; if (n & 1) s++;          // pad to word
                }
            }
        }
        for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
            const uint8_t *cc = pal + (uint32_t)ib[i] * pal_entry;
            out[i] = FB_COLOR(cc[2], cc[1], cc[0]);
        }
        kfree(ib);
        *out_w = w; *out_h = h;
        return out;
    }
    for (int y = 0; y < h; y++) {
        int srcy = topdown ? y : (h - 1 - y);
        const uint8_t *row = bits + (uint32_t)srcy * rowbytes;
        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0;
            if (bpp == 8) {
                const uint8_t *cc = pal + (uint32_t)row[x] * pal_entry; b = cc[0]; g = cc[1]; r = cc[2];
            } else if (bpp == 4) {
                int idx = (x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4);
                const uint8_t *cc = pal + (uint32_t)idx * pal_entry; b = cc[0]; g = cc[1]; r = cc[2];
            } else if (bpp == 1) {
                int idx = (row[x >> 3] >> (7 - (x & 7))) & 1;
                const uint8_t *cc = pal + (uint32_t)idx * pal_entry; b = cc[0]; g = cc[1]; r = cc[2];
            } else if (bpp == 24) {
                const uint8_t *cc = row + (uint32_t)x * 3; b = cc[0]; g = cc[1]; r = cc[2];
            } else if (bpp == 32) {
                const uint8_t *cc = row + (uint32_t)x * 4; b = cc[0]; g = cc[1]; r = cc[2];
            }
            out[(uint32_t)y * w + x] = FB_COLOR(r, g, b);
        }
    }
    *out_w = w; *out_h = h;
    return out;
}

// MulDiv (GDI.128): MulDiv(nNumber, nNumerator, nDenominator) = 3 s_word = 6b.
// Returns (nNumber * nNumerator) / nDenominator, rounded to nearest, or -1 on
// divide-by-zero. Pascal pushes left-to-right, so nDenominator (last) is arg0.
// Previously a MISS: it returned 0 AND popped 0 argbytes, corrupting the Pascal
// stack. Apps use it constantly for coordinate/scale math, so a broken MulDiv
// strands sprites at fixed positions (SkiFree) and mislays HUD layout (Chips).
static void g_muldiv(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int32_t den = (int16_t)arg16(c, 0);
    int32_t num = (int16_t)arg16(c, 1);
    int32_t a   = (int16_t)arg16(c, 2);
    *dx = 0; *argbytes = 6;
    if (den == 0) { *ax = 0xFFFF; return; }            // -1
    int32_t prod = a * num;
    if ((prod < 0) ^ (den < 0)) prod -= den / 2; else prod += den / 2;  // round
    *ax = (uint16_t)(int16_t)(prod / den);
}

// SetDIBitsToDevice (GDI.443): blit a device-independent bitmap straight to the
// device. SetDIBitsToDevice(hdc, xDest,yDest, cx,cy, xSrc,ySrc, startScan,
// numScans, lpvBits far, lpbmi far, fuColorUse) = 14 words = 28 bytes. The DIB
// header (lpbmi) and pixel bits (lpvBits) live in 16-bit guest memory and are
// SEPARATE (unlike RT_BITMAP resources), so we marshal them into one packed DIB
// and reuse decode_dib. Chips Challenge draws its LEVEL/TIME/CHIPS readouts and
// inventory via this call; while it was a MISS those boxes stayed black.
static void g_setdibitstodevice(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                uint16_t *argbytes) {
    uint16_t color_use = arg16(c, 0); (void)color_use;
    uint16_t bmi_off = arg16(c, 1), bmi_seg = arg16(c, 2);
    uint16_t bits_off= arg16(c, 3), bits_seg= arg16(c, 4);
    uint16_t numScans= arg16(c, 5);
    uint16_t startScan=arg16(c, 6);
    int ySrc = (int16_t)arg16(c, 7);
    int xSrc = (int16_t)arg16(c, 8);
    int cy   = (int16_t)arg16(c, 9);
    int cx   = (int16_t)arg16(c, 10);
    int yDest= (int16_t)arg16(c, 11);
    int xDest= (int16_t)arg16(c, 12);
    uint16_t hdc = arg16(c, 13);
    *ax = 0; *dx = 0; *argbytes = 28;
    win16_dc_t *dc = (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) ? &g_dcs[hdc] : 0;
    win16_trace("DIB hdc=%d @%d,%d %dx%d src%d,%d scan=%d/%d dmem=%d\n",
                hdc, xDest, yDest, cx, cy, xSrc, ySrc, startScan, numScans,
                dc ? (dc->membuf ? 1 : 0) : -1);
    if (!dc) return;

    uint8_t hdr[64];
    for (int i = 0; i < 64; i++) hdr[i] = x86_16_rd8(c, bmi_seg, (uint16_t)(bmi_off + i));
    uint32_t hdrsize = dib_rd32(hdr);
    int W, H, bpp, ncolors = 0, pal_entry;   // compression is re-read by decode_dib
    if (hdrsize == 12) {
        W = (int)dib_rd16(hdr + 4); H = (int)dib_rd16(hdr + 6);
        bpp = (int)dib_rd16(hdr + 10); pal_entry = 3;
    } else if (hdrsize >= 40 && hdrsize < 512) {
        W = (int)dib_rd32(hdr + 4); H = (int)dib_rd32(hdr + 8);
        bpp = (int)dib_rd16(hdr + 14);
        ncolors = (int)dib_rd32(hdr + 32); pal_entry = 4;
    } else return;
    if (H < 0) H = -H;                          // packed buffer is always bottom-up
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) return;
    if (bpp <= 8) { int defc = 1 << bpp; if (ncolors <= 0 || ncolors > defc) ncolors = defc; }
    else ncolors = 0;
    if ((uint32_t)startScan >= (uint32_t)H) numScans = 0;
    else if ((uint32_t)startScan + numScans > (uint32_t)H) numScans = (uint16_t)(H - startScan);

    int rowbytes = ((W * bpp + 31) / 32) * 4;
    int pal_bytes = ncolors * pal_entry;
    uint32_t total = hdrsize + (uint32_t)pal_bytes + (uint32_t)rowbytes * H;
    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return;
    for (uint32_t i = 0; i < total; i++) buf[i] = 0;
    for (uint32_t i = 0; i < hdrsize; i++)
        buf[i] = (i < 64) ? hdr[i] : x86_16_rd8(c, bmi_seg, (uint16_t)(bmi_off + i));
    for (int i = 0; i < pal_bytes; i++)
        buf[hdrsize + i] = x86_16_rd8(c, bmi_seg, (uint16_t)(bmi_off + hdrsize + i));
    // lpvBits points at scanline `startScan` (bottom-up). Place each provided row
    // at its true position so decode_dib sees a coherent full-height DIB.
    uint8_t *bitbase = buf + hdrsize + pal_bytes;
    for (uint16_t r = 0; r < numScans; r++) {
        uint8_t *dstrow = bitbase + (uint32_t)(startScan + r) * rowbytes;
        uint16_t srcrow = (uint16_t)(bits_off + (uint32_t)r * rowbytes);
        for (int i = 0; i < rowbytes; i++)
            dstrow[i] = x86_16_rd8(c, bits_seg, (uint16_t)(srcrow + i));
    }
    int dw = 0, dh = 0;
    uint32_t *pix = decode_dib(buf, total, &dw, &dh);
    kfree(buf);
    if (!pix) return;
    // decode_dib returns TOP-DOWN pixels, but SetDIBitsToDevice's ySrc is measured
    // from the BOTTOM of the (bottom-up) DIB. (#216) For a PARTIAL source rect this
    // matters: Chip's Challenge draws one 17x23 digit out of a 17x552 strip via
    // ySrc, and reading sy=ySrc+yy from the top hit the unfilled (black) rows, so
    // digits rendered black-on-black. Map the bottom-relative source row to the
    // top-down decoded row. (For a full-image draw cy==dh this reduces to sy=yy.)
    int cw = (cx > 0 && cx <= dw) ? cx : dw;
    int chh = (cy > 0 && cy <= dh) ? cy : dh;
    for (int yy = 0; yy < chh; yy++) {
        int sy = dh - ySrc - chh + yy; if (sy < 0 || sy >= dh) continue;
        for (int xx = 0; xx < cw; xx++) {
            int sx = xSrc + xx; if (sx < 0 || sx >= dw) continue;
            dc_plot(dc, xDest + xx, yDest + yy, pix[(uint32_t)sy * dw + sx]);
        }
    }
    kfree(pix);
    *ax = numScans;
}

// LoadBitmap (USER.175): (hInstance, lpBitmapName far) = 6 bytes. Decode the
// named RT_BITMAP resource into a GDI bitmap object that SelectObject can bind
// into a memory DC (so BitBlt copies it to a window). Returns the object handle.
static void u_loadbitmap(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t name_off = arg16(c, 0);
    uint16_t name_seg = arg16(c, 1);
    uint16_t hinst    = arg16(c, 2);
    *argbytes = 6; *ax = 0; *dx = 0;
    uint16_t id = 0;
    uint32_t len = 0;
    const uint8_t *res = 0;
    if (name_seg == 0) {
        id = name_off;                        // MAKEINTRESOURCE(id)
        res = win16_get_resource(hinst, 2 /*RT_BITMAP*/, id, &len);
    } else {
        char nm[40]; rd_far_cstr(c, name_seg, name_off, nm, sizeof(nm));
        if (nm[0] == '#') {
            for (int i = 1; nm[i] >= '0' && nm[i] <= '9'; i++) id = id * 10 + (nm[i] - '0');
            res = win16_get_resource(hinst, 2 /*RT_BITMAP*/, id, &len);
        } else {
            // String-named RT_BITMAP (e.g. Chips Challenge loads its tiles by name).
            res = win16_get_resource_by_name(hinst, 2 /*RT_BITMAP*/, nm, &len);
            if (!res) {
                // (#EP3 Fuji Golf) FUJIGOLF.EXE references its bitmaps by name in C
                // code, but its resource compiler stored them as INTEGER ids 1..6,
                // so the by-name lookup misses and the opening-screen / golfer-sprite
                // BitBlt source stays empty (a black box). Map the known Fuji Golf
                // bitmap names to their integer ids (verified against the resource
                // dimensions) and retry by id. This ONLY runs after a by-name miss,
                // so games with genuinely named bitmaps (Chips) are unaffected.
                static const struct { const char *n; uint16_t id; } fg[] = {
                    {"GOLFMAN",1},{"MANMONO",2},{"PATMAN",3},{"PATMANMONO",4},
                    {"OPENINGBMP",5},{"ADDOPENINGBMP",6},{0,0}
                };
                char up[40]; int u2 = 0;
                for (; nm[u2] && u2 < 39; u2++)
                    up[u2] = (nm[u2] >= 'a' && nm[u2] <= 'z') ? (char)(nm[u2]-32) : nm[u2];
                up[u2] = '\0';
                for (int k = 0; fg[k].n; k++) {
                    int e = 1, q = 0;
                    for (; fg[k].n[q] && up[q]; q++) if (fg[k].n[q] != up[q]) { e = 0; break; }
                    if (e && fg[k].n[q] == 0 && up[q] == 0) { id = fg[k].id; break; }
                }
                if (id) res = win16_get_resource(hinst, 2 /*RT_BITMAP*/, id, &len);
            }
            if (!res) { win16_trace("LoadBitmap hinst=%04x name='%s' -> NO RES\n", hinst, nm); return; }
            win16_trace("LoadBitmap name='%s' -> id=%u len=%u\n", nm, id, len);
        }
    }
    // (#278 Word6 pass14) OEM system bitmaps (OBM_CLOSE/RESTORE/RESTORED/arrows/
    // checkboxes, ids 0x7F00..0x7FFF) are loaded by Word's window-frame init
    // (seg31:0x2648) via LoadBitmap(NULL, MAKEINTRESOURCE(id)); that routine gates
    // its success on each LoadBitmap returning a non-zero handle, and a failure
    // there aborts WinMain phase-2 (-> OLE teardown -> FatalAppExit). We have no
    // OEM bitmap resources, so synthesize a small blank 8bpp bitmap. The handle is
    // real (GetObject reports its dimensions; the subsequent CreateCompatibleBitmap/
    // BitBlt chain operates on small surfaces), so Word's frame init succeeds. The
    // glyphs render blank, which is cosmetic; the goal is Word staying open. Gated to
    // hinst==0 + the OEM id range so app-owned bitmaps (games, named tiles) are
    // unaffected.
    if (!res && name_seg == 0 && hinst == 0 && id >= 0x7F00) {
        int sw = 16, sh = 16;
        uint32_t *opix = (uint32_t *)kmalloc((uint32_t)sw * sh * 4);
        if (opix) {
            for (int i = 0; i < sw * sh; i++) opix[i] = 0xFFC0C0C0u;  // light gray
            int oobj = win16_alloc_gdiobj(4, 0);
            if (oobj) {
                g_gdiobj[oobj].pix = opix; g_gdiobj[oobj].w = sw;
                g_gdiobj[oobj].h = sh; g_gdiobj[oobj].bpp = 8;
                win16_trace("LoadBitmap OEM id=%u -> synth obj=%d %dx%d\n", id, oobj, sw, sh);
                *ax = (uint16_t)oobj; return;
            }
            kfree(opix);
        }
    }
    if (!res) { win16_trace("LoadBitmap hinst=%04x id=%u -> NO RESOURCE\n", hinst, id); return; }
    int w = 0, h = 0;
    uint32_t *pix = decode_dib(res, len, &w, &h);
    if (!pix) { win16_trace("LoadBitmap id=%u len=%u -> DECODE FAIL\n", id, len); return; }
    int obj = win16_alloc_gdiobj(4, 0);
    if (!obj) { kfree(pix); return; }
    g_gdiobj[obj].pix = pix; g_gdiobj[obj].w = w; g_gdiobj[obj].h = h; g_gdiobj[obj].bpp = 8;
    win16_trace("LoadBitmap hinst=%04x id=%u -> obj=%d %dx%d\n", hinst, id, obj, w, h);
    *ax = (uint16_t)obj;
}

// ---- Resource-handle API (#148): FindResource/LoadResource/LockResource ----
// Map a FindResource type arg (MAKEINTRESOURCE int, or a string name) to the
// integer RT_* id our resource lookup uses.
static int win16_restype_from(x86_16_cpu_t *c, uint16_t seg, uint16_t off) {
    if (seg == 0) return (int)off;            // MAKEINTRESOURCE(type)
    char nm[24]; rd_far_cstr(c, seg, off, nm, sizeof(nm));
    static const struct { const char *n; int t; } m[] = {
        {"CURSOR",1},{"BITMAP",2},{"ICON",3},{"MENU",4},{"DIALOG",5},
        {"STRING",6},{"FONTDIR",7},{"FONT",8},{"ACCELERATOR",9},{"RCDATA",10},
        {0,0}};
    for (int i = 0; m[i].n; i++) {
        int j = 0; while (m[i].n[j] && nm[j] == m[i].n[j]) j++;
        if (!m[i].n[j] && !nm[j]) return m[i].t;
    }
    return 0;
}
// FindResource (KERNEL.60): FindResource(hInst, lpName, lpType) word+far+far.
static void k_findresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t type_off = arg16(c, 0), type_seg = arg16(c, 1);
    uint16_t name_off = arg16(c, 2), name_seg = arg16(c, 3);
    uint16_t hinst    = arg16(c, 4);
    *argbytes = 10; *ax = 0; *dx = 0;
    int type = win16_restype_from(c, type_seg, type_off);
    if (type == 0) return;
    uint32_t len = 0; const uint8_t *r = 0;
    if (name_seg == 0) {
        r = win16_get_resource(hinst, (uint16_t)type, name_off, &len);
    } else {
        char nm[40]; rd_far_cstr(c, name_seg, name_off, nm, sizeof(nm));
        if (nm[0] == '#') {
            uint16_t id = 0;
            for (int i = 1; nm[i] >= '0' && nm[i] <= '9'; i++) id = id * 10 + (nm[i] - '0');
            r = win16_get_resource(hinst, (uint16_t)type, id, &len);
        } else {
            r = win16_get_resource_by_name(hinst, (uint16_t)type, nm, &len);
        }
    }
    if (!r) { win16_trace("FindResource type=%d -> NO RES\n", type); return; }
    if (g_rsrc_n >= WIN16_MAX_RSRC) g_rsrc_n = 0;   // wrap; handles are short-lived
    int h = g_rsrc_n++;
    g_rsrc[h].p = r; g_rsrc[h].len = len; g_rsrc[h].seg = 0;
    win16_trace("FindResource type=%d -> hrsrc=%d len=%lu\n", type, h + 1, (unsigned long)len);
    *ax = (uint16_t)(h + 1);                  // HRSRC = table index + 1
}
// LoadResource (KERNEL.61): LoadResource(hInst, hRsrc) = 2 words. Copies the
// resource bytes into a Win16 global-heap block and returns the HGLOBAL (seg).
static void k_loadresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hrsrc = arg16(c, 0);
    *argbytes = 4; *ax = 0; *dx = 0;
    if (hrsrc == 0 || hrsrc > g_rsrc_n) return;
    int h = hrsrc - 1;
    if (g_rsrc[h].seg == 0) {
        uint16_t seg = heap_alloc(g_rsrc[h].len ? g_rsrc[h].len : 1);
        if (!seg) return;
        for (uint32_t i = 0; i < g_rsrc[h].len; i++)
            x86_16_wr8(c, seg, (uint16_t)i, g_rsrc[h].p[i]);
        g_rsrc[h].seg = seg;
    }
    *ax = g_rsrc[h].seg;                       // HGLOBAL = global-heap segment
}
// LockResource (KERNEL.62): LockResource(hResData) = 1 word. Far ptr seg:0000.
static void k_lockresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t seg = arg16(c, 0); *argbytes = 2; *ax = 0; *dx = seg;
}
// FreeResource (KERNEL.63): FreeResource(hResData) = 1 word. Return 0 (freed).
static void k_freeresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
// SizeofResource (KERNEL.65): SizeofResource(hInst, hRsrc) = 2 words.
static void k_sizeofresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hrsrc = arg16(c, 0); *argbytes = 4; *ax = 0; *dx = 0;
    if (hrsrc && hrsrc <= g_rsrc_n) *ax = (uint16_t)g_rsrc[hrsrc - 1].len;
}

// AccessResource (KERNEL.64): AccessResource(HINSTANCE hInst, HRSRC hResInfo)
//   = 2 words = 4 bytes. The OLD-STYLE resource-loading path: it opens the module
// file positioned at the resource and returns an HFILE from which the app _lread's
// the raw resource bytes (vs the newer LoadResource/LockResource). Excel 4.0 loads
// its startup resources this way; leaving it a stub (fake handle, every _lread
// returned 0) made Excel abort with "Not enough memory to run Microsoft Excel".
// FindResource already located the bytes into g_rsrc[hResInfo-1]; back a real file
// handle with a private copy of them so the existing _lread/_lclose serve the
// sequential reads. (#Excel4)
static void k_accessresource(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    uint16_t hrsrc = arg16(c, 0);       // HRSRC (from FindResource); hInst=arg1 unused
    *argbytes = 4; *dx = 0; *ax = (uint16_t)HFILE_ERROR;
    if (hrsrc == 0 || hrsrc > g_rsrc_n) return;
    int ri = hrsrc - 1;
    uint32_t len = g_rsrc[ri].len;
    int h = -1;
    for (int i = 1; i < WIN16_MAX_FILES; i++) if (!g_files[i].used) { h = i; break; }
    if (h < 0) return;
    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    if (!copy) return;
    for (uint32_t i = 0; i < len; i++) copy[i] = g_rsrc[ri].p[i];
    win16_file_t *fp = &g_files[h];
    fp->buf = copy; fp->size = len; fp->cap = len ? len : 1;
    fp->pos = 0; fp->dirty = 0; fp->used = 1; fp->path[0] = 0;
    win16_trace("AccessResource hrsrc=%d -> hFile=%d len=%lu\n", hrsrc, h,
                (unsigned long)len);
    *ax = (uint16_t)h;
}

// GetDC (USER.66): GetDC(hwnd) = 1 word. Returns an HDC bound to the window.
static void u_getdc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                    uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    win16_window_t *win = win_from_hwnd(hwnd);
    int wi = -1;
    if (win) wi = (int)(win - g_windows);
    win16_paint_begin();              // (#219) redirect this draw to scratch
    int dc = dc_alloc(wi);
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 2;
}

// GetDesktopWindow (USER.286): no args. Return a stable non-NULL pseudo HWND.
static void u_getdesktopwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    (void)c; *ax = WIN16_HWND_DESKTOP; *dx = 0; *argbytes = 0;
}

// IsIconic (USER.31): IsIconic(hwnd) = 1 word. We never minimise -> FALSE.
static void u_isiconic(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
// MessageBeep (USER.104): MessageBeep(uType) = 1 word. No PC speaker wired; no-op TRUE.
static void u_messagebeep(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 2;
}
// GetWindowDC (USER.67): GetWindowDC(hwnd) = 1 word. Like GetDC (no chrome model).
static void u_getwindowdc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    win16_window_t *win = win_from_hwnd(hwnd);
    int wi = win ? (int)(win - g_windows) : -1;
    int dc = dc_alloc(wi);
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 2;
}
// DrawMenuBar (USER.160): DrawMenuBar(hwnd) = 1 word. We draw no menu bar -> no-op.
// AdjustWindowRect (USER.102): (LPRECT lpRect, DWORD dwStyle, BOOL bMenu). (#152)
static void u_adjustwindowrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t bMenu = arg16(c, 0);
    (void)arg32(c, 1);                         // dwStyle (standard frame assumed)
    uint16_t r_off = arg16(c, 3), r_seg = arg16(c, 4);
    int16_t l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+0));
    int16_t t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+2));
    int16_t r = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+4));
    int16_t b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+6));
    l -= W16_BORDER; r += W16_BORDER; b += W16_BORDER;
    t -= (W16_TITLEBAR + W16_BORDER + (bMenu ? WIN16_MENUBAR_H : 0));
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+0), (uint16_t)l);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+2), (uint16_t)t);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+4), (uint16_t)r);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+6), (uint16_t)b);
    *ax = 1; *dx = 0; *argbytes = 10;
}
// AdjustWindowRectEx (USER.454): (LPRECT, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle).
static void u_adjustwindowrectex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg32(c, 0);                         // dwExStyle
    uint16_t bMenu = arg16(c, 2);
    (void)arg32(c, 3);                         // dwStyle
    uint16_t r_off = arg16(c, 5), r_seg = arg16(c, 6);
    int16_t l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+0));
    int16_t t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+2));
    int16_t r = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+4));
    int16_t b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+6));
    l -= W16_BORDER; r += W16_BORDER; b += W16_BORDER;
    t -= (W16_TITLEBAR + W16_BORDER + (bMenu ? WIN16_MENUBAR_H : 0));
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+0), (uint16_t)l);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+2), (uint16_t)t);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+4), (uint16_t)r);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off+6), (uint16_t)b);
    *ax = 1; *dx = 0; *argbytes = 14;
}
// SetMenu (USER.158): SetMenu(hwnd, hMenu) = 2 words. Attach + reshape client. (#152)
static void u_setmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hmenu = arg16(c, 0);
    win16_window_t *win = win_from_hwnd(arg16(c, 1));
    if (win) { win->hmenu = hmenu; win16_draw_frame(win); }
    *ax = 1; *dx = 0; *argbytes = 4;
}
static void u_drawmenubar(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_window_t *win = win_from_hwnd(arg16(c, 0));
    if (win && win->hmenu) win16_draw_menubar(win);
    *ax = 0; *dx = 0; *argbytes = 2;
}
// WinHelp (USER.171): WinHelp(hwnd, lpHelpFile far, wCommand, dwData) = 6w = 12b.
// No help engine -> accept and return TRUE.
static void u_winhelp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 12;
}

// ReleaseDC (USER.68): ReleaseDC(hwnd, hdc) = 2 words.
static void u_releasedc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    uint16_t hdc = arg16(c, 0);
    if (hdc && hdc < WIN16_MAX_DC) g_dcs[hdc].used = 0;
    win16_paint_end();                // (#219) publish the finished frame
    *ax = 1; *dx = 0; *argbytes = 4;
}

// BeginPaint (USER.39): BeginPaint(hwnd, lpPaintStruct far) = 1+2 = 3w = 6 bytes.
// Returns an HDC and fills the PAINTSTRUCT (packed Win16):
//   WORD hdc +0, WORD fErase +2, RECT rcPaint +4 (4 WORDs), ... (we fill hdc+rc).
static void u_beginpaint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t ps_off = arg16(c, 0);
    uint16_t ps_seg = arg16(c, 1);
    uint16_t hwnd   = arg16(c, 2);
    win16_window_t *win = win_from_hwnd(hwnd);
    int wi = win ? (int)(win - g_windows) : -1;
    win16_paint_begin();              // (#219) redirect this frame to scratch
    // (#219) Determine the paint/clip rect. A full repaint (soft_repaint /
    // paint_all_z) forces the whole client; otherwise use the window's accumulated
    // invalid rect so we erase + report only the changed area (cards/cells survive).
    int rl = 0, rt = 0, rr = win ? win->cw : 0, rb = win ? win->ch : 0;
    int do_erase = 1;
    if (win && !g_win16_in_soft_repaint) {
        if (win->inval_valid) {
            // App explicitly invalidated a region (InvalidateRect): erase + report
            // just that area so untouched content (cards/cells/sprites) survives.
            rl = win->inval_l; rt = win->inval_t; rr = win->inval_r; rb = win->inval_b;
            if (rl < 0) rl = 0;
            if (rt < 0) rt = 0;
            if (rr > win->cw) rr = win->cw;
            if (rb > win->ch) rb = win->ch;
            if (rr <= rl || rb <= rt) { rl = 0; rt = 0; rr = win->cw; rb = win->ch; }
        } else {
            // (#219) Synthetic WM_PAINT (ShowWindow/forced) with nothing invalidated.
            // Report the FULL client so apps that key on rcPaint still redraw their
            // UI, but DO NOT erase: one-shot-drawn content (Golf's dealt cards, the
            // TETRIS well) persists. The bg was established at create/show time.
            do_erase = 0;
        }
    }
    g_win16_clip_active = 1; g_win16_clip_l = rl; g_win16_clip_t = rt;
    g_win16_clip_r = rr; g_win16_clip_b = rb;
    if (win) {
        // Always recompute client geometry + refresh the menu bar via draw_frame,
        // but suppress the bg fill when we must not erase (no_bg_fill guard).
        int sav = g_win16_no_bg_fill;
        if (!do_erase) g_win16_no_bg_fill = 1;
        win16_draw_frame(win);
        g_win16_no_bg_fill = sav;
        win->inval_valid = 0;
    }
    int dc = dc_alloc(wi);
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 0), (uint16_t)dc);   // hdc
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 2), 1);             // fErase
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 4), (uint16_t)rl);  // rc.left
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 6), (uint16_t)rt);  // rc.top
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 8), (uint16_t)rr);  // rc.right
    x86_16_wr16(c, ps_seg, (uint16_t)(ps_off + 10), (uint16_t)rb); // rc.bottom
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 6;
}

// EndPaint (USER.40): EndPaint(hwnd, lpPaintStruct far) = 6 bytes. Frees the DC.
static void u_endpaint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    uint16_t ps_off = arg16(c, 0);
    uint16_t ps_seg = arg16(c, 1);
    uint16_t hwnd = arg16(c, 2);
    uint16_t hdc = x86_16_rd16(c, ps_seg, ps_off);
    if (hdc && hdc < WIN16_MAX_DC) g_dcs[hdc].used = 0;
    g_win16_clip_active = 0;           // (#219) end the paint clip
    win16_paint_end();                // (#219) publish the finished frame
    // (#bugB) Word paints the document body (and, when the whole client is invalidated
    // on a scroll, refills the entire client) between BeginPaint and here, but never
    // repaints its own self-drawn menu row. Heal it now: capture the menu strip when it
    // is present, restore it when this paint blanked it. Main frame only; a game/app
    // with no top menu strip is untouched.
    if (hwnd == g_win16_main_hwnd) win16_menu_strip_maintain();
    *ax = 1; *dx = 0; *argbytes = 6;
}

// GetClipBox (GDI.83): GetClipBox(hdc, lpRect far) -> region complexity; fills the
// clip bounding box. We have no sub-clipping, so the clip box is the whole client
// canvas. Args (top->bottom): lpRect(off,seg), hdc. Returns SIMPLEREGION (2).
static void g_getclipbox(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t r_off = arg16(c, 0);
    uint16_t r_seg = arg16(c, 1);
    /* hdc */ (void)arg16(c, 2);
    int l = 0, t = 0;
    int r = (g_win16_canvas_w > 0 ? g_win16_canvas_w : (int)fb_get_width());
    int b = (g_win16_canvas_h > 0 ? g_win16_canvas_h : (int)fb_get_height());
    // (#219) During an app paint, report the actual invalid/clip rect (client
    // coords) so apps (e.g. Golf solitaire) repaint only the changed region
    // instead of re-greening the whole window over their content.
    if (g_win16_clip_active) { l = g_win16_clip_l; t = g_win16_clip_t; r = g_win16_clip_r; b = g_win16_clip_b; }
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 0), (uint16_t)l);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 2), (uint16_t)t);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 4), (uint16_t)r);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 6), (uint16_t)b);
    *ax = 2; *dx = 0; *argbytes = 6;   // SIMPLEREGION
}

// GetPixel (GDI.83): COLORREF GetPixel(HDC hdc, int X, int Y). Returns the pixel
// colour at device (X,Y) as 0x00BBGGRR, or CLR_INVALID (0xFFFFFFFF) if the point
// lies outside the surface. Pascal args (top->bottom): Y, X, hdc.
// (#394) CRITICAL: our GDI dispatch table previously mapped ordinal 83 to
// GetClipBox, but GDI.83 is GetPixel (GetClipBox is GDI.77, which we already have).
// CARDS.DLL calls GetPixel(hdc,x,y) while compositing cards; the mis-mapped
// GetClipBox handler read the (X,Y) coordinate words as an lpRect FAR POINTER and
// wrote a 4-word RECT through it. For MSEP GOLF that bogus pointer (e.g. 0079:ff82)
// landed inside Golf's OWN code segment (linear 0x10712) and zeroed the WM_PAINT
// wndproc epilogue, so the wndproc fell through into an unrelated routine, ran
// away to the 400M-instruction cap and NEVER returned. Golf's message loop was
// therefore stuck forever inside DispatchMessage and never pumped mouse/keyboard,
// which is exactly why every click was ignored. A GetPixel that only READS the
// canvas (never writes) removes the corruption. The canvas already stores
// 0x00BBGGRR (== COLORREF), so no channel swap is needed.
static void g_getpixel(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    int y = (int)(int16_t)arg16(c, 0);
    int x = (int)(int16_t)arg16(c, 1);
    (void)arg16(c, 2);   // hdc
    *argbytes = 6;
    if (g_win16_canvas && x >= 0 && y >= 0 &&
        x < g_win16_canvas_w && y < g_win16_canvas_h) {
        uint32_t cref = (uint32_t)canvas_get(x, y) & 0x00FFFFFFu;
        *ax = (uint16_t)(cref & 0xFFFF);
        *dx = (uint16_t)(cref >> 16);
    } else {
        *ax = 0xFFFF; *dx = 0xFFFF;   // CLR_INVALID
    }
}

// RectVisible (GDI.104): BOOL RectVisible(HDC hdc, const RECT far *lprc). WEP games
// (TetraVex, JezzBall) gate EVERY tile / board-cell draw on this predicate; with no
// sub-region clipping our clip box is always the whole client canvas, so any rect is
// visible. Returning FALSE (the old UNIMPL default) made those games skip all their
// content draws (blank client). Return TRUE. Args: hdc(word)+lprc(far)=3w=6 bytes.
static void g_rectvisible(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                          uint16_t *argbytes) {
    (void)arg16(c, 0); (void)arg16(c, 1); /* lprc far */
    (void)arg16(c, 2); /* hdc */
    *ax = 1; *dx = 0; *argbytes = 6;   // TRUE
}

// CreateWindowEx (USER.452): identical to CreateWindow plus a leading dwExStyle
// DWORD. Pascal args push left->right, so the stack TOP (lpParam..lpClassName) is
// exactly the CreateWindow layout; dwExStyle sits deepest (read last). We reuse
// u_createwindow to read+act on the common args, then pop the 4 extra bytes.
static void u_createwindowex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    u_createwindow(c, ax, dx, argbytes);   // reads/acts on the 28 common bytes
    *argbytes = (uint16_t)(*argbytes + 4); // + dwExStyle DWORD
}

// GetClientRect (USER.33): GetClientRect(hwnd, lpRect far) = 1+2 = 3w = 6 bytes.
static void u_getclientrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t r_off = arg16(c, 0);
    uint16_t r_seg = arg16(c, 1);
    uint16_t hwnd  = arg16(c, 2);
    win16_window_t *win = win_from_hwnd(hwnd);
    uint16_t cw = win ? (uint16_t)win->cw : 0;
    uint16_t ch = win ? (uint16_t)win->ch : 0;
    if (hwnd == WIN16_HWND_DESKTOP) { cw = (uint16_t)fb_get_width(); ch = (uint16_t)fb_get_height(); }
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 0), 0);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 2), 0);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 4), cw);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 6), ch);
    *ax = 1; *dx = 0; *argbytes = 6;
}

// GetWindowRect (USER.32): GetWindowRect(hwnd, lpRect far) = 6 bytes.
static void u_getwindowrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    uint16_t r_off = arg16(c, 0);
    uint16_t r_seg = arg16(c, 1);
    uint16_t hwnd  = arg16(c, 2);
    win16_window_t *win = win_from_hwnd(hwnd);
    // GetWindowRect always returns SCREEN coordinates. For a top-level window the
    // outer rect is (x,y)-(x+w,y+h). For a child window x/y are parent-relative,
    // so use the computed screen client origin (cx,cy); children have no chrome,
    // so window rect == client rect in screen space.
    int x = 0, y = 0, w = 0, h = 0;
    if (hwnd == WIN16_HWND_DESKTOP) {
        w = (int)fb_get_width(); h = (int)fb_get_height();
    } else if (win) {
        if (win->is_child) { x = win->cx; y = win->cy; w = win->w; h = win->h; }
        else               { x = win->x;  y = win->y;  w = win->w; h = win->h; }
    }
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 0), (uint16_t)x);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 2), (uint16_t)y);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 4), (uint16_t)(x + w));
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 6), (uint16_t)(y + h));
    *ax = 1; *dx = 0; *argbytes = 6;
}

// InvalidateRect (USER.125): InvalidateRect(hwnd, lpRect far, bErase) =
//   1+2+1 = 4w = 8 bytes. Queue a WM_PAINT.
static void u_invalidaterect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    /* bErase */ (void)arg16(c, 0);
    uint16_t r_off = arg16(c, 1), r_seg = arg16(c, 2);
    uint16_t hwnd = arg16(c, 3);
    // (#219) Accumulate the dirty rect (client coords) so BeginPaint/GetClipBox
    // can report just the changed area; NULL lpRect invalidates the whole client.
    win16_window_t *w = win_from_hwnd(hwnd);
    if (w) {
        int l, t, rr, b;
        if (r_seg == 0 && r_off == 0) { l = 0; t = 0; rr = w->cw; b = w->ch; }
        else {
            l  = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 0));
            t  = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 2));
            rr = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 4));
            b  = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 6));
        }
        if (!w->inval_valid) { w->inval_l = l; w->inval_t = t; w->inval_r = rr; w->inval_b = b; w->inval_valid = 1; }
        else {
            if (l < w->inval_l) w->inval_l = l;
            if (t < w->inval_t) w->inval_t = t;
            if (rr > w->inval_r) w->inval_r = rr;
            if (b > w->inval_b) w->inval_b = b;
        }
    }
    msgq_post(hwnd, WM_PAINT, 0, 0);
    g_win16_frame_dirty = 1;   // (#255 perf C) app asked for a repaint
    { extern int g_w6life; static int w6iv=0; if (g_w6life && w6iv<300){ w6iv++;
        kprintf("[W6INVAL] #%d InvalidateRect hwnd=%04x erase=%d -> WM_PAINT posted\n", w6iv, hwnd, arg16(c,0)); } }
    *ax = 1; *dx = 0; *argbytes = 8;
}

// (#word6) Shared: union a scroll rect (guest far-ptr rseg:roff, or the clip rect,
// or the full client) into a window's invalid region and post WM_PAINT. Used by
// ScrollWindow/ScrollWindowEx below. We do NOT blit-shift the client pixels (that
// needs a client->canvas coord map and would risk the games' render paths); instead
// we invalidate the scrolled region so the app's OWN WM_PAINT redraws the correctly-
// scrolled content from its model. Word 6 (and other editors) scroll their document
// client with these on every new line / PgUp / PgDn; both were UNIMPLEMENTED
// (ordinal-table-only, silent no-op), so the exposed rows were never invalidated and
// rendered as stale black boxes / a scroll blanked the chrome. Games never call
// ScrollWindow*, so this path is editor-only and cannot regress them.
static void win16_scroll_invalidate(x86_16_cpu_t *c, uint16_t hwnd,
                                    uint16_t rseg, uint16_t roff,
                                    uint16_t cseg, uint16_t coff) {
    win16_window_t *w = win_from_hwnd(hwnd);
    if (!w) return;
    int l, t, rr, b;
    // Prefer the scroll rect; fall back to the clip rect; else the whole client.
    if (rseg == 0 && roff == 0) { rseg = cseg; roff = coff; }
    if (rseg == 0 && roff == 0) { l = 0; t = 0; rr = w->cw; b = w->ch; }
    else {
        l  = (int16_t)x86_16_rd16(c, rseg, (uint16_t)(roff + 0));
        t  = (int16_t)x86_16_rd16(c, rseg, (uint16_t)(roff + 2));
        rr = (int16_t)x86_16_rd16(c, rseg, (uint16_t)(roff + 4));
        b  = (int16_t)x86_16_rd16(c, rseg, (uint16_t)(roff + 6));
    }
    if (!w->inval_valid) { w->inval_l = l; w->inval_t = t; w->inval_r = rr; w->inval_b = b; w->inval_valid = 1; }
    else {
        if (l  < w->inval_l) w->inval_l = l;
        if (t  < w->inval_t) w->inval_t = t;
        if (rr > w->inval_r) w->inval_r = rr;
        if (b  > w->inval_b) w->inval_b = b;
    }
    msgq_post(hwnd, WM_PAINT, 0, 0);
    g_win16_frame_dirty = 1;
}

// ScrollWindow (USER.61): ScrollWindow(hwnd, XAmount, YAmount, lpRect FAR*, lpClipRect
// FAR*) = 1+1+1+2+2 = 7 words = 14 bytes. Stack top-down: clip.off, clip.seg,
// rect.off, rect.seg, YAmount, XAmount, hwnd.
static void u_scrollwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t clip_off = arg16(c, 0), clip_seg = arg16(c, 1);
    uint16_t rect_off = arg16(c, 2), rect_seg = arg16(c, 3);
    /* YAmount=arg16(c,4), XAmount=arg16(c,5) (deltas; see helper note) */
    uint16_t hwnd     = arg16(c, 6);
    win16_scroll_invalidate(c, hwnd, rect_seg, rect_off, clip_seg, clip_off);
    *ax = 1; *dx = 0; *argbytes = 14;
}

// ScrollWindowEx (USER.319): ScrollWindowEx(hwnd, dx, dy, lprcScroll FAR*, lprcClip
// FAR*, hrgnUpdate, lprcUpdate FAR*, flags) = 1+1+1+2+2+1+2+1 = 11 words = 22 bytes.
// Stack top-down: flags, lprcUpdate.off, lprcUpdate.seg, hrgnUpdate, lprcClip.off,
// lprcClip.seg, lprcScroll.off, lprcScroll.seg, dy, dx, hwnd.
static void u_scrollwindowex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    uint16_t clip_off = arg16(c, 4), clip_seg = arg16(c, 5);
    uint16_t rect_off = arg16(c, 6), rect_seg = arg16(c, 7);
    uint16_t hwnd     = arg16(c, 10);
    win16_scroll_invalidate(c, hwnd, rect_seg, rect_off, clip_seg, clip_off);
    *ax = 1; *dx = 0; *argbytes = 22;   // return SIMPLEREGION(1)
}

// FillRect (USER.81): FillRect(hdc, lpRect far, hBrush) = 1+2+1 = 4w = 8 bytes.
static void u_fillrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    uint16_t hbr   = arg16(c, 0);
    uint16_t r_off = arg16(c, 1);
    uint16_t r_seg = arg16(c, 2);
    uint16_t hdc   = arg16(c, 3);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        int l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 0));
        int t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 2));
        int rr = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 4));
        int b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 6));
        uint32_t col = gdiobj_color(hbr, FB_COLOR(255,255,255));
        for (int yy = t; yy < b; yy++)
            for (int xx = l; xx < rr; xx++)
                dc_plot(dc, xx, yy, col);
    }
    *ax = 1; *dx = 0; *argbytes = 8;
}

// FrameRect (USER.83): FrameRect(hdc, lpRect far, hBrush) = 1+2+1 = 4w = 8 bytes.
// Draws a 1-pixel border of the brush colour around the rect (top, bottom, left,
// right edges). SkiFree calls this to outline its status panel and slope. It was
// previously UNIMPLEMENTED, and the MISS path popped argbytes=0, desyncing the
// Pascal stack by 8 bytes; the game's main loop then returned into garbage
// (cs=ffff:fe76) and spun forever, so the slope canvas never rendered. (#204)
static void u_framerect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    uint16_t hbr   = arg16(c, 0);
    uint16_t r_off = arg16(c, 1);
    uint16_t r_seg = arg16(c, 2);
    uint16_t hdc   = arg16(c, 3);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        int l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 0));
        int t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 2));
        int rr = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 4));
        int b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 6));
        uint32_t col = gdiobj_color(hbr, FB_COLOR(0,0,0));
        for (int xx = l; xx < rr; xx++) { dc_plot(dc, xx, t, col); dc_plot(dc, xx, b - 1, col); }
        for (int yy = t; yy < b; yy++) { dc_plot(dc, l, yy, col); dc_plot(dc, rr - 1, yy, col); }
    }
    *ax = 1; *dx = 0; *argbytes = 8;
}

// FindWindow (USER.50): FindWindow(lpClassName far, lpWindowName far) = 4w = 8b.
// We do not track a global class/title registry for arbitrary lookups, so report
// "not found" (0). The argbytes MUST be 8 or the Pascal stack desyncs. (#204)
static void u_findwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;
}

// MessageBox (USER.1): MessageBox(hwnd, lpText far, lpCaption far, wType) =
//   1+2+2+1 = 6 words = 12 bytes. Draw a simple box and log; return IDOK(1).
extern void win16_gui_messagebox(const char *title, const char *text);
static void u_messagebox(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    /* wType */ (void)arg16(c, 0);
    uint16_t cap_off = arg16(c, 1); uint16_t cap_seg = arg16(c, 2);
    uint16_t txt_off = arg16(c, 3); uint16_t txt_seg = arg16(c, 4);
    /* hwnd */ (void)arg16(c, 5);
    char title[64], text[200];
    rd_far_cstr(c, cap_seg, cap_off, title, sizeof(title));
    rd_far_cstr(c, txt_seg, txt_off, text, sizeof(text));
    kprintf("[win16api]   MessageBox '%s': %s\n", title, text);
    {   /* (#278 Word6) dump the caller: ret ip:cs at SS:SP.. */
        uint16_t rip=x86_16_rd16(c,c->ss,c->sp);
        uint16_t rcs=x86_16_rd16(c,c->ss,(uint16_t)(c->sp+2));
        kprintf("[MBCALL] caller=%04x:%04x ss:sp=%04x:%04x ds=%04x\n", rcs, rip, c->ss, c->sp, c->ds);

    }
    win16_trace("MessageBox '%s': %s\n", title, text);
    win16_gui_messagebox(title, text);
    *ax = 1; *dx = 0; *argbytes = 12;   // IDOK
}

// ===========================================================================
// USER: misc no-op-ish entries needed to keep the stack balanced.
// ===========================================================================
// (#278 P59) USER.190 AS IMPORTED BY WORD 6 is an 8-byte (4-word) call: ALL five
// Word call sites (seg6:0x52f4, seg17:0x10e4, seg25:0x1d63, seg197:0x0571,
// seg211:0x1a14) push 4 words before it (HWND [obj+0x34] + seg:off far ptr + a
// BOOL flag). Our stub wrongly mapped ordinal 190 to a 0-arg GetSysModalWindow
// (argbytes=0), so the 8 pushed bytes were NEVER popped -> Pascal-stack DESYNC.
// That desync corrupted the caller-saved SI (the document CView handle 9e3a)
// across the near call seg17:0x10c6 inside the per-view layout seg17:0x128c, so
// the bit1 test at 0x1319 read the WRONG object (si=1) and bailed -> the doc
// view's needs-layout bit1 (view+0x0c &2) was never cleared, its display-list
// node (view+0x48) was never built -> redisplay seg17:0x4fb0 skipped the doc
// body -> GRAY page. Popping the correct 8 bytes keeps SI = the doc view so
// Word's OWN layout runs. Return 0 (no sys-modal window); the callers do not
// use the return value on the layout path.
static void u_user190_word6(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;
}
static void u_word1(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // (#278 pass31) SetFocus(hWnd): track which window owns input focus so the
    // keyboard pump can route WM_KEYDOWN/WM_CHAR to it (e.g. Word's document edit
    // window) instead of always the top-level frame. Returns the previous focus.
    uint16_t hwnd = arg16(c, 0);
    uint16_t prev = g_win16_focus;
    g_win16_focus = hwnd;
    *ax = prev; *dx = 0; *argbytes = 2;
}
// GetFocus (USER.23) / GetActiveWindow (USER.60): both take NO args and return
// the HWND that currently has focus. The old GetFocus stub returned 0 (and
// wrongly popped 2 argbytes), and GetActiveWindow was unimplemented, so games
// that gate their animation loop on GetFocus()/GetActiveWindow()==hwnd (SkiFree)
// believed they were never active: the slope only advanced when an input message
// happened to wake the loop, and sprites left trails because no full redraw ran.
// Returning the focused Win16 window makes the self-check pass so the idle game
// loop runs every tick. Single Win16 app per run, so its window IS the active one.
static void u_getfocus(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c;
    uint16_t f = win16_focus_hwnd();
    if (g_win16_focus) { win16_window_t *w = win_from_hwnd(g_win16_focus); if (w && w->used) f = g_win16_focus; }
    *ax = f; *dx = 0; *argbytes = 0;
}

// (#278 pass31) Text caret API (USER.163-169). Word's document text engine
// creates a caret on the editing window and positions it as text is inserted;
// the prior UNIMPL path popped argbytes=0 and desynced the Pascal stack on
// CreateCaret(8)/SetCaretPos(4)/Show/Hide(2). Implement them faithfully and draw
// a solid bar at the caret's client position so the editing position is visible.
static struct { uint16_t owner; int x, y, w, h; int shown_cnt; } g_caret;
static void u_createcaret(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int nh = (int)arg16(c, 0); int nw = (int)arg16(c, 1); (void)arg16(c, 2);
    uint16_t hwnd = arg16(c, 3);
    g_caret.owner = hwnd; g_caret.w = nw ? nw : 2; g_caret.h = nh ? nh : 14;
    g_caret.x = 0; g_caret.y = 0; g_caret.shown_cnt = 0;
    kprintf("[W6CARET] CreateCaret hwnd=%04x w=%d h=%d\n", hwnd, g_caret.w, g_caret.h);
    *ax = 1; *dx = 0; *argbytes = 8;
}
static void u_destroycaret(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; g_caret.owner = 0; g_caret.shown_cnt = 0; *ax = 1; *dx = 0; *argbytes = 0;
}
static void u_setcaretpos(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_caret.y = (int)(int16_t)arg16(c, 0); g_caret.x = (int)(int16_t)arg16(c, 1);
    *ax = 1; *dx = 0; *argbytes = 4;
}
static void u_hidecaret(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); if (g_caret.shown_cnt > 0) g_caret.shown_cnt--; *ax = 1; *dx = 0; *argbytes = 2;
}
static void u_showcaret(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); g_caret.shown_cnt++; *ax = 1; *dx = 0; *argbytes = 2;
}
static void u_setcaretblink(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); *ax = 1; *dx = 0; *argbytes = 2;
}
static void u_getcaretblink(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 530; *dx = 0; *argbytes = 0;
}
static void win16_draw_caret(void) {
    if (!g_caret.owner || g_caret.shown_cnt <= 0) return;
    win16_window_t *w = win_from_hwnd(g_caret.owner);
    if (!w || !w->used) return;
    int cx = w->cx + g_caret.x, cy = w->cy + g_caret.y;
    int h = g_caret.h > 0 ? g_caret.h : 14, ww = g_caret.w > 0 ? g_caret.w : 2;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < ww; xx++)
            canvas_plot(cx + xx, cy + yy, FB_COLOR(0,0,0));
}
// SetTimer (USER.10): SetTimer(hwnd, idEvent, uElapse, lpTimerFunc far). Leftmost
// (hwnd) is pushed FIRST so it is DEEPEST. Stack words top->bottom:
//   [0]=lpTimerFunc off, [1]=lpTimerFunc seg, [2]=uElapse, [3]=idEvent, [4]=hwnd.
// Registers a periodic WM_TIMER. uElapse is in milliseconds; convert to kernel
// ticks (g_timer_hz). A reused (hwnd,id) pair just re-arms the existing timer.
static void u_settimer(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t uElapse = arg16(c, 2);
    uint16_t id      = arg16(c, 3);
    uint16_t hwnd    = arg16(c, 4);
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    // period (ticks) = ms * hz / 1000, clamped to >= ~50 ms so a 0/tiny elapse
    // does not flood the queue and starve the interpreter.
    uint64_t period = ((uint64_t)uElapse * hz) / 1000ULL;
    uint64_t floor_ticks = (hz / 20) ? (hz / 20) : 1;   // ~50 ms floor
    if (period < floor_ticks) period = floor_ticks;

    int slot = -1, free_slot = -1;
    for (int i = 0; i < WIN16_MAX_TIMERS; i++) {
        if (g_timers[i].used && g_timers[i].hwnd == hwnd && g_timers[i].id == id) { slot = i; break; }
        if (!g_timers[i].used && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot;
    if (slot >= 0) {
        g_timers[slot].used = 1;
        g_timers[slot].hwnd = hwnd;
        g_timers[slot].id   = id;
        g_timers[slot].period_ticks = period;
        g_timers[slot].next_fire    = timer_ticks + period;
        if (g_win16_apilog)
            kprintf("[win16api]   SetTimer hwnd=%04x id=%u elapse=%ums period=%lu ticks\n",
                    hwnd, id, uElapse, (unsigned long)period);
    }
    *ax = id ? id : 1; *dx = 0; *argbytes = 10;
}
// KillTimer (USER.12): KillTimer(hwnd, idEvent). Stack top->bottom: idEvent, hwnd.
static void u_killtimer(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t id   = arg16(c, 0);
    uint16_t hwnd = arg16(c, 1);
    for (int i = 0; i < WIN16_MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].hwnd == hwnd && g_timers[i].id == id)
            g_timers[i].used = 0;
    *ax = 1; *dx = 0; *argbytes = 4;
}
// SelectPalette (USER.282): SelectPalette(hdc, hPalette, bForceBackground) = 3w = 6 bytes.
// We do not model palettes (true-color framebuffer); return a non-zero "previous" handle.
static void u_selectpalette(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 6;
}
// RealizePalette (USER.283): RealizePalette(hdc) = 1w = 2 bytes. Return # entries changed (0).
static void u_realizepalette(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// ===========================================================================
// GDI handlers
// ===========================================================================

// GetStockObject (GDI.87): GetStockObject(nIndex) = 1 word. Map to STOCK_*.
static void g_getstockobject(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    uint16_t idx = arg16(c, 0);
    uint16_t h;
    switch (idx) {
        case 0: h = STOCK_WHITE_BRUSH; break;
        case 1: h = STOCK_LTGRAY_BRUSH; break;
        case 2: h = STOCK_GRAY_BRUSH; break;
        case 3: h = STOCK_DKGRAY_BRUSH; break;
        case 4: h = STOCK_BLACK_BRUSH; break;
        case 5: h = STOCK_NULL_BRUSH; break;
        case 6: h = STOCK_WHITE_PEN; break;
        case 7: h = STOCK_BLACK_PEN; break;
        case 8: h = STOCK_NULL_PEN; break;
        // (#278 pass22) Stock FONT indices 10..16 (were defaulting to a brush handle).
        case 10: h = STOCK_OEM_FIXED_FONT; break;
        case 11: h = STOCK_ANSI_FIXED_FONT; break;
        case 12: h = STOCK_ANSI_VAR_FONT; break;
        case 13: h = STOCK_SYSTEM_FONT; break;
        case 14: h = STOCK_DEVICE_DEFAULT_FONT; break;
        case 15: h = STOCK_DEFAULT_PALETTE; break;
        case 16: h = STOCK_SYSTEM_FIXED_FONT; break;
        default: h = STOCK_LTGRAY_BRUSH; break;
    }
    *ax = h; *dx = 0; *argbytes = 2;
}

// CreateSolidBrush (GDI.66): CreateSolidBrush(crColor long) = 2 words.
static void g_createsolidbrush(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                               uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    int h = win16_alloc_gdiobj(1, cr);
    *ax = (uint16_t)h; *dx = 0; *argbytes = 4;
}

// CreatePen (GDI.61): CreatePen(nStyle s_word, nWidth s_word, crColor long) = 4w.
static void g_createpen(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    int h = win16_alloc_gdiobj(2, cr);
    *ax = (uint16_t)h; *dx = 0; *argbytes = 8;
}

// SelectObject (GDI.45): SelectObject(hdc, hObject) = 2 words. Returns prev obj.
static void g_selectobject(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t hobj = arg16(c, 0);
    uint16_t hdc  = arg16(c, 1);
    uint16_t prev = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        // Decide pen vs bitmap vs font vs brush by handle range/type, and return
        // the PREVIOUSLY selected handle of that class. Apps depend on a non-zero
        // return (e.g. cdtDrawExt skips the card BitBlt when SelectObject -> 0).
        int is_pen = (hobj == STOCK_BLACK_PEN || hobj == STOCK_WHITE_PEN ||
                      hobj == STOCK_NULL_PEN ||
                      (hobj < WIN16_MAX_GDIOBJ && g_gdiobj[hobj].used &&
                       g_gdiobj[hobj].type == 2));
        int is_bitmap = (hobj < WIN16_MAX_GDIOBJ && g_gdiobj[hobj].used &&
                         g_gdiobj[hobj].type == 4);
        int is_font = (hobj < WIN16_MAX_GDIOBJ && g_gdiobj[hobj].used &&
                       g_gdiobj[hobj].type == 5) || IS_STOCK_FONT(hobj);  // (#278 pass22)
        if (is_pen) {
            prev = dc->sel_pen; dc->sel_pen = hobj;
            dc->pen_color = gdiobj_color(hobj, dc->pen_color);
        } else if (is_bitmap) {
            // Selecting a compatible bitmap into a (memory) DC: bind its buffer.
            prev = dc->sel_bitmap; dc->sel_bitmap = hobj;
            dc->membuf = g_gdiobj[hobj].pix;
            dc->mw = g_gdiobj[hobj].w;
            dc->mh = g_gdiobj[hobj].h;
        } else if (is_font) {
            prev = dc->sel_font; dc->sel_font = hobj;
        } else {
            prev = dc->sel_brush; dc->sel_brush = hobj;
            dc->brush_null = gdiobj_is_null(hobj);
            dc->brush_color = gdiobj_color(hobj, dc->brush_color);
        }
    }
    *ax = prev; *dx = 0; *argbytes = 4;
}

// DeleteObject (GDI.69): DeleteObject(hObject) = 1 word.
static void g_deleteobject(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    if (h && h < WIN16_MAX_GDIOBJ && g_gdiobj[h].used) {
        if (g_gdiobj[h].type == 4 && g_gdiobj[h].pix) {
            kfree(g_gdiobj[h].pix); g_gdiobj[h].pix = 0;
        }
        g_gdiobj[h].used = 0;
    }
    *ax = 1; *dx = 0; *argbytes = 2;
}

// SetTextColor (GDI.9): SetTextColor(hdc, crColor long) = 1+2 = 3w = 6 bytes.
static void g_settextcolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    uint16_t hdc = arg16(c, 2);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used)
        g_dcs[hdc].text_color = colorref_to_fb(cr);
    *ax = 0; *dx = 0; *argbytes = 6;
}

// SetBkColor (GDI.1): SetBkColor(hdc, crColor long) = 6 bytes.
static void g_setbkcolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    uint16_t hdc = arg16(c, 2);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used)
        g_dcs[hdc].bk_color = colorref_to_fb(cr);
    *ax = 0; *dx = 0; *argbytes = 6;
}

// SetBkMode (GDI.x not imported) / SetTextAlign (GDI.346): (hdc, mode) = 2w.
static void g_settextalign(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}

// MoveTo (GDI.20): MoveTo(hdc, x s_word, y s_word) = 3 words = 6 bytes.
static void g_moveto(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                     uint16_t *argbytes) {
    int y = (int16_t)arg16(c, 0);
    int x = (int16_t)arg16(c, 1);
    uint16_t hdc = arg16(c, 2);
    uint16_t px = 0, py = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        px = (uint16_t)g_dcs[hdc].cur_x; py = (uint16_t)g_dcs[hdc].cur_y;
        g_dcs[hdc].cur_x = x; g_dcs[hdc].cur_y = y;
    }
    *ax = px; *dx = py; *argbytes = 6;   // returns prev point as DWORD
}

// LineTo (GDI.19): LineTo(hdc, x s_word, y s_word) = 6 bytes.
static void g_lineto(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                     uint16_t *argbytes) {
    int y1 = (int16_t)arg16(c, 0);
    int x1 = (int16_t)arg16(c, 1);
    uint16_t hdc = arg16(c, 2);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        // Endpoints are LOGICAL; map to device for the Bresenham walk. cur_x/cur_y
        // stay logical (updated below to the logical target).
        int x0 = dc->cur_x, y0 = dc->cur_y;
        int lx1 = x1, ly1 = y1;
        dc_lp2dp(dc, &x0, &y0);
        int xd1 = lx1, yd1 = ly1; dc_lp2dp(dc, &xd1, &yd1);
        int ddx = xd1 - x0, ddy = yd1 - y0;
        x1 = xd1; y1 = yd1;
        int sx = ddx < 0 ? -1 : 1, sy = ddy < 0 ? -1 : 1;
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;
        int err = ddx - ddy, x = x0, y = y0;
        for (;;) {
            dc_plot(dc, x, y, dc->pen_color);
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 > -ddy) { err -= ddy; x += sx; }
            if (e2 < ddx)  { err += ddx; y += sy; }
        }
        dc->cur_x = lx1; dc->cur_y = ly1;   // keep current position LOGICAL
    }
    *ax = 1; *dx = 0; *argbytes = 6;
}

// Rectangle (GDI.27): Rectangle(hdc, l,t,r,b s_word x4) = 5 words = 10 bytes.
static void g_rectangle(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    int b = (int16_t)arg16(c, 0);
    int r = (int16_t)arg16(c, 1);
    int t = (int16_t)arg16(c, 2);
    int l = (int16_t)arg16(c, 3);
    uint16_t hdc = arg16(c, 4);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        dc_lp2dp(dc, &l, &t); dc_lp2dp(dc, &r, &b);   // logical -> device corners
        if (r < l) { int tmp = l; l = r; r = tmp; }
        if (b < t) { int tmp = t; t = b; b = tmp; }
        if (!dc->brush_null)
            for (int yy = t; yy < b; yy++)
                for (int xx = l; xx < r; xx++)
                    dc_plot(dc, xx, yy, dc->brush_color);
        for (int xx = l; xx < r; xx++) { dc_plot(dc, xx, t, dc->pen_color);
                                         dc_plot(dc, xx, b-1, dc->pen_color); }
        for (int yy = t; yy < b; yy++) { dc_plot(dc, l, yy, dc->pen_color);
                                         dc_plot(dc, r-1, yy, dc->pen_color); }
    }
    *ax = 1; *dx = 0; *argbytes = 10;
}

// PatBlt (GDI.29): PatBlt(hdc, x,y,w,h s_word x4, dwRop long) = 4+2 = 6w = 12.
// Fill with the selected brush colour. (We deliberately do NOT honour BLACKNESS/
// WHITENESS here: a truly-black BLACKNESS clear made TETRIS's per-frame well-clear
// race the playfield present and read as black; brush-colour fill is what the
// rest of the engine expects.)
static uint32_t blt_get_pixel(win16_dc_t *dc, int x, int y);  // fwd (defined below)
static void g_patblt(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                     uint16_t *argbytes) {
    uint32_t rop = arg32(c, 0); (void)rop;
    int h = (int16_t)arg16(c, 2);
    int w = (int16_t)arg16(c, 3);
    int y = (int16_t)arg16(c, 4);
    int x = (int16_t)arg16(c, 5);
    uint16_t hdc = arg16(c, 6);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++)
                dc_plot(dc, xx, yy, dc->brush_color);
    }
    *ax = 1; *dx = 0; *argbytes = 14;  // (#278 pass30) PatBlt = hdc+x+y+w+h(5w)+dwRop(2w)=14 bytes; was 12 -> 2-byte Pascal-stack desync -> caller lret popped garbage cs:ip (cs=17e2 wild jump)
}
#if 0  /* (#219) ROP-aware PatBlt disabled: regressed TETRIS well rendering */
static void g_patblt_rop(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                     uint16_t *argbytes) {
    uint32_t rop = arg32(c, 0);
    int h = (int16_t)arg16(c, 2);
    int w = (int16_t)arg16(c, 3);
    int y = (int16_t)arg16(c, 4);
    int x = (int16_t)arg16(c, 5);
    uint16_t hdc = arg16(c, 6);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) {
                uint32_t v;
                switch (rop) {
                    case 0x00FF0062UL: v = FB_COLOR(255,255,255); break;          // WHITENESS
                    case 0x00000042UL: v = FB_COLOR(0,0,0);       break;          // BLACKNESS
                    case 0x00550009UL: v = ~blt_get_pixel(dc, xx, yy);  break;    // DSTINVERT
                    case 0x005A0049UL: v = blt_get_pixel(dc, xx, yy) ^ dc->brush_color; break; // PATINVERT
                    default:           v = dc->brush_color;       break;          // PATCOPY etc
                }
                dc_plot(dc, xx, yy, v);
            }
    }
    *ax = 1; *dx = 0; *argbytes = 14;  // (#278 pass30) PatBlt = hdc+x+y+w+h(5w)+dwRop(2w)=14 bytes; was 12 -> 2-byte Pascal-stack desync -> caller lret popped garbage cs:ip (cs=17e2 wild jump)
}
#endif  /* (#219) ROP-aware PatBlt disabled */

// TextOut (GDI.33): TextOut(hdc, x s_word, y s_word, lpString far, nCount word)
//   = 1+1+1+2+1 = 6 words = 12 bytes.
static void g_textout(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                      uint16_t *argbytes) {
    uint16_t n      = arg16(c, 0);
    uint16_t s_off  = arg16(c, 1);
    uint16_t s_seg  = arg16(c, 2);
    int y           = (int16_t)arg16(c, 3);
    int x           = (int16_t)arg16(c, 4);
    uint16_t hdc    = arg16(c, 5);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        if (g_w6log > 0) {
            char sb[17]; int si = 0;
            for (; si < 16 && si < (int)n; si++) {
                uint8_t ch = x86_16_rd8(c, s_seg, (uint16_t)(s_off + si));
                sb[si] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            sb[si] = 0;
            W6LOG("[W6] TO  win=%d mem=%d x=%d y=%d n=%d tx=%06x bk=%06x bkm=%d s='%s'\n",
                  dc->win, dc->membuf ? 1 : 0, x, y, n, dc->text_color, dc->bk_color, dc->bk_mode, sb);
        }
        // (#word6 FIX) Honour the DC's window/viewport mapping for the text anchor,
        // exactly as the shape primitives (LineTo/Rectangle/Polygon) already do.
        // Identity for MM_TEXT DCs (all normal apps), so a no-op for them.
        dc_lp2dp(dc, &x, &y);
        int opaque = (dc->bk_mode != 1);   // OPAQUE (Win16 default) unless TRANSPARENT (SkiFree HUD)
        int cx = x;
        for (uint16_t i = 0; i < n; i++) {
            char ch = (char)x86_16_rd8(c, s_seg, (uint16_t)(s_off + i));
            if (opaque) {
                for (int row = 0; row < FONT_HEIGHT && row < 16; row++)
                    for (int col = 0; col < FONT_WIDTH; col++)
                        dc_plot(dc, cx + col, y + row, dc->bk_color);
            }
            const uint8_t *gph = font_get_glyph(ch);
            if (gph) {
                for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
                    uint8_t bits = gph[row];
                    for (int col = 0; col < 8; col++)
                        if (bits & (0x80 >> col))
                            dc_plot(dc, cx + col, y + row, dc->text_color);
                }
            }
            cx += FONT_WIDTH;
        }
    }
    *ax = 1; *dx = 0; *argbytes = 12;
}

// (#278 Word6 pass28) ExtTextOut (GDI.351): the function Word 6 uses to draw ALL
// its UI text (menu labels, toolbar text, ruler, document). Was UNIMPL -> no text
// + Pascal-stack desync (argbytes 0 vs 22) that aborted Word's paint mid-frame,
// leaving the window black. Signature (wine gdi.exe16): ExtTextOut(HDC hdc,
// int x, int y, UINT fuOptions, const RECT FAR* lprc, LPCSTR lpStr, UINT cb,
// const int FAR* lpDx). Pascal pushes L->R; arg0 (nearest sp) = lpDx low.
//   words: hdc,x,y,opt (4) + lprc(2)+lpStr(2)+cb(1)+lpDx(2) = 11w = 22 bytes.
static void g_exttextout(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                         uint16_t *argbytes) {
    uint16_t dxa_off = arg16(c, 0); uint16_t dxa_seg = arg16(c, 1);  // lpDx
    uint16_t n       = arg16(c, 2);                                  // cbCount
    uint16_t s_off   = arg16(c, 3); uint16_t s_seg   = arg16(c, 4);  // lpString
    uint16_t rc_off  = arg16(c, 5); uint16_t rc_seg  = arg16(c, 6);  // lprc
    uint16_t opts    = arg16(c, 7);                                  // fuOptions
    int y            = (int16_t)arg16(c, 8);
    int x            = (int16_t)arg16(c, 9);
    uint16_t hdc     = arg16(c, 10);
    *ax = 1; *dx = 0; *argbytes = 22;
    if (!(hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used)) return;
    win16_dc_t *dc = &g_dcs[hdc];
    if (g_w6log > 0) {
        char sb[17]; int si = 0;
        for (; si < 16 && si < (int)n; si++) {
            uint8_t ch = x86_16_rd8(c, s_seg, (uint16_t)(s_off + si));
            sb[si] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        sb[si] = 0;
        W6LOG("[W6] ETO win=%d mem=%d x=%d y=%d n=%d opt=%x tx=%06x bk=%06x bkm=%d s='%s'\n",
              dc->win, dc->membuf ? 1 : 0, x, y, n, opts, dc->text_color, dc->bk_color, dc->bk_mode, sb);
    }
    // (#word6 FIX) Honour the DC's window/viewport mapping (SetViewportOrgEx etc)
    // for the text anchor AND the opaque rect, exactly as the shape primitives
    // (LineTo/Rectangle/Polygon) already do. Word 6 reuses ONE 1-line-tall memory
    // bitmap for EVERY document line by setting the memory DC's viewport origin to
    // -lineTop (SetViewportOrgEx), so logical y=lineTop maps to device y=0 in the
    // reused buffer. Without this mapping only the first line (origin 0) landed
    // in-bounds; lines 2+ drew (and were later blitted) OUTSIDE the 16px buffer and
    // read back solid black. dc_lp2dp is the identity for MM_TEXT DCs (every other
    // app + the window DCs), so this is a no-op for them.
    dc_lp2dp(dc, &x, &y);
    int rect_filled = 0;
    // ETO_OPAQUE (0x0002): fill lprc with the background colour first.
    if ((opts & 0x0002) && (rc_seg || rc_off)) {
        int l = (int16_t)x86_16_rd16(c, rc_seg, rc_off);
        int t = (int16_t)x86_16_rd16(c, rc_seg, (uint16_t)(rc_off + 2));
        int r = (int16_t)x86_16_rd16(c, rc_seg, (uint16_t)(rc_off + 4));
        int b = (int16_t)x86_16_rd16(c, rc_seg, (uint16_t)(rc_off + 6));
        dc_lp2dp(dc, &l, &t); dc_lp2dp(dc, &r, &b);   // logical -> device rect corners
        for (int yy = t; yy < b; yy++)
            for (int xx = l; xx < r; xx++)
                dc_plot(dc, xx, yy, dc->bk_color);
        rect_filled = 1;
    }
    // Per-cell opaque erase when bk_mode==OPAQUE and the rect was not pre-filled.
    int opaque = (dc->bk_mode != 1) && !rect_filled;
    int cx = x;
    for (uint16_t i = 0; i < n; i++) {
        char ch = (char)x86_16_rd8(c, s_seg, (uint16_t)(s_off + i));
        if (opaque) {
            for (int row = 0; row < FONT_HEIGHT && row < 16; row++)
                for (int col = 0; col < FONT_WIDTH; col++)
                    dc_plot(dc, cx + col, y + row, dc->bk_color);
        }
        const uint8_t *gph = font_get_glyph(ch);
        if (gph) {
            for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
                uint8_t bits = gph[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col))
                        dc_plot(dc, cx + col, y + row, dc->text_color);
            }
        }
        // Advance: per-char lpDx spacing if provided, else fixed cell width.
        int adv = FONT_WIDTH;
        if (dxa_seg || dxa_off)
            adv = (int16_t)x86_16_rd16(c, dxa_seg, (uint16_t)(dxa_off + i * 2));
        cx += adv;
    }
}

// (#278 Word6 pass28) Coordinate + window-state ordinals Word's paint/layout loop
// calls thousands of times. UNIMPL (argbytes 0) desynced the Pascal stack every
// call, corrupting Word's paint routine. Implemented faithfully with the canvas
// as Word's coordinate space (consistent with dc_plot, which uses win->cx/cy).
// USER.28 ClientToScreen(HWND, POINT FAR*): client -> "screen" (canvas) coords.
static void u_clienttoscreen(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t pt_off = arg16(c,0), pt_seg = arg16(c,1), hwnd = arg16(c,2);
    *argbytes = 6; *ax = 1; *dx = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    int ox = w ? w->cx : 0, oy = w ? w->cy : 0;
    int px = (int16_t)x86_16_rd16(c, pt_seg, pt_off);
    int py = (int16_t)x86_16_rd16(c, pt_seg, (uint16_t)(pt_off+2));
    x86_16_wr16(c, pt_seg, pt_off, (uint16_t)(px + ox));
    x86_16_wr16(c, pt_seg, (uint16_t)(pt_off+2), (uint16_t)(py + oy));
}
// USER.29 ScreenToClient(HWND, POINT FAR*): "screen" (canvas) -> client coords.
static void u_screentoclient(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t pt_off = arg16(c,0), pt_seg = arg16(c,1), hwnd = arg16(c,2);
    *argbytes = 6; *ax = 1; *dx = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    int ox = w ? w->cx : 0, oy = w ? w->cy : 0;
    int px = (int16_t)x86_16_rd16(c, pt_seg, pt_off);
    int py = (int16_t)x86_16_rd16(c, pt_seg, (uint16_t)(pt_off+2));
    x86_16_wr16(c, pt_seg, pt_off, (uint16_t)(px - ox));
    x86_16_wr16(c, pt_seg, (uint16_t)(pt_off+2), (uint16_t)(py - oy));
}
// USER.34 EnableWindow(HWND, BOOL): return TRUE if it was PREVIOUSLY disabled (we
// don't track disable state; report it was enabled -> 0). argbytes=4.
static void u_enablewindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); (void)arg16(c,1); *ax = 0; *dx = 0; *argbytes = 4;
}
// USER.35 IsWindowEnabled(HWND): our windows are always enabled. argbytes=2.
static void u_iswindowenabled(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); *ax = 1; *dx = 0; *argbytes = 2;
}
// USER.49 IsWindowVisible(HWND): visible if the window exists. argbytes=2.
static void u_iswindowvisible(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_window_t *w = win_from_hwnd(arg16(c,0));
    int vis;
    // (#278 P60) Faithful IsWindowVisible for Word: TRUE only if the window AND every
    // ancestor has WS_VISIBLE (ws_visible). Word lays out its document view via the
    // layout gate seg223:0x0cfa, whose IsWindowVisible branch (cmp ax,1; sbb; neg)
    // returns "proceed" (1) ONLY while the view window is NOT visible - Word creates
    // its edit pane without WS_VISIBLE, lays the doc out while hidden, then ShowWindow's
    // it. The old stub returned 1 whenever the window merely existed, so the gate always
    // saw "visible" and skipped the doc-body layout -> view+0x48 never built -> gray.
    // Games keep the old exists->visible behaviour (segcount<100) so they stay identical.
    if (g_info.segcount >= 100) {
        vis = w ? 1 : 0;
        win16_window_t *p = w;
        for (int g = 0; p && vis && g < 64; g++) {
            if (!p->ws_visible) { vis = 0; break; }
            if (!p->parent) break;
            p = win_from_hwnd(p->parent);
        }
    } else {
        vis = w ? 1 : 0;
    }
    *ax = (uint16_t)vis; *dx = 0; *argbytes = 2;
}
// USER.131 GetClassLong(HWND, int): we expose no class longs -> 0. argbytes=4.
static void u_getclasslong(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); (void)arg16(c,1); *ax = 0; *dx = 0; *argbytes = 4;
}
// USER.153 ChangeMenu(HMENU,UINT,LPCSTR,UINT,UINT): legacy menu edit. Accept and
// balance the stack (5 words = 10 bytes? -> wine: word word segstr word word =
// 1+1+2+1+1 = 6w = 12 bytes). Return TRUE. argbytes=12.
static void u_changemenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 12;
}
// USER.262 GetWindow(HWND, UINT cmd): no sibling/child traversal modelled -> 0.
static void u_getwindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c,0); (void)arg16(c,1); *ax = 0; *dx = 0; *argbytes = 4;
}
// USER.2 OldExitWindows(): do NOT actually exit; report failure. argbytes=0.
static void u_oldexitwindows(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 0;
}
// USER.127 ValidateRect(HWND, const RECT FAR*): clear the window's update region.
// UNIMPL desynced the paint loop (25 calls/frame). 1+2=3w=6 bytes.
static void u_validaterect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_window_t *w = win_from_hwnd(arg16(c,2));
    if (w) w->inval_valid = 0;
    *ax = 1; *dx = 0; *argbytes = 6;
}
// GDI.3 SetMapMode(HDC, int): store the mode so the window/viewport transform is
// honoured for MM_ANISOTROPIC/MM_ISOTROPIC (Fuji Golf's course scaling). Return the
// previous mode. argbytes=4.
static void g_setmapmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int mode = (int16_t)arg16(c, 0);
    uint16_t hdc = arg16(c, 1);
    int prev = 1;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        prev = g_dcs[hdc].mapmode;
        g_dcs[hdc].mapmode = mode;
    }
    *ax = (uint16_t)prev; *dx = 0; *argbytes = 4;
}
// GDI.11 SetWindowOrg / .12 SetWindowExt / .13 SetViewportOrg / .14 SetViewportExt.
// Each: (hdc, x s_word, y s_word) = 6 bytes; returns the previous point as DWORD
// (DX:AX = y:x). These set the logical/device window/viewport that dc_lp2dp uses.
// Previously UNIMPL (argbytes=0) -> Fuji Golf's course transform was the identity
// and its LineTo course outline fell outside the client (black course). (#EP3)
static void g_setwindoworg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y = (int16_t)arg16(c,0), x = (int16_t)arg16(c,1); uint16_t hdc = arg16(c,2);
    int ox = 0, oy = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc]; ox = dc->wox; oy = dc->woy; dc->wox = x; dc->woy = y; }
    *ax = (uint16_t)ox; *dx = (uint16_t)oy; *argbytes = 6;
}
static void g_setwindowext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y = (int16_t)arg16(c,0), x = (int16_t)arg16(c,1); uint16_t hdc = arg16(c,2);
    int ox = 1, oy = 1;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc]; ox = dc->wex; oy = dc->wey; dc->wex = x; dc->wey = y; }
    *ax = (uint16_t)ox; *dx = (uint16_t)oy; *argbytes = 6;
}
static void g_setvieworg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y = (int16_t)arg16(c,0), x = (int16_t)arg16(c,1); uint16_t hdc = arg16(c,2);
    int ox = 0, oy = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc]; ox = dc->vox; oy = dc->voy; dc->vox = x; dc->voy = y; }
    *ax = (uint16_t)ox; *dx = (uint16_t)oy; *argbytes = 6;
}
static void g_setviewext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y = (int16_t)arg16(c,0), x = (int16_t)arg16(c,1); uint16_t hdc = arg16(c,2);
    int ox = 1, oy = 1;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc]; ox = dc->vex; oy = dc->vey; dc->vex = x; dc->vey = y; }
    *ax = (uint16_t)ox; *dx = (uint16_t)oy; *argbytes = 6;
}
// GDI.36 Polygon(hdc, LPPOINT, nCount) / .37 Polyline. Fill (Polygon) with the
// current brush via even-odd scanline, outline with the pen. Points are LOGICAL
// (mapped via dc_lp2dp). argbytes = hdc(2)+far ptr(4)+count(2) = 8. (#EP3)
static void g_poly(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes, int fill) {
    int n = (int16_t)arg16(c, 0);
    uint16_t p_off = arg16(c, 1), p_seg = arg16(c, 2);
    uint16_t hdc = arg16(c, 3);
    *ax = 1; *dx = 0; *argbytes = 8;
    if (!(hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) || n < 2 || n > 512) return;
    win16_dc_t *dc = &g_dcs[hdc];
    static int px[512], py[512];
    for (int i = 0; i < n; i++) {
        int x = (int16_t)x86_16_rd16(c, p_seg, (uint16_t)(p_off + i*4));
        int y = (int16_t)x86_16_rd16(c, p_seg, (uint16_t)(p_off + i*4 + 2));
        dc_lp2dp(dc, &x, &y); px[i] = x; py[i] = y;
    }
    if (fill && !dc->brush_null) {
        int ymin = py[0], ymax = py[0];
        for (int i = 1; i < n; i++) { if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
        for (int yy = ymin; yy <= ymax; yy++) {
            int xs[512], xc = 0;
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                int y0 = py[i], y1 = py[j], x0 = px[i], x1 = px[j];
                if ((y0 <= yy && y1 > yy) || (y1 <= yy && y0 > yy)) {
                    int xx = x0 + (int)((long)(yy - y0) * (x1 - x0) / (y1 - y0));
                    if (xc < 512) xs[xc++] = xx;
                }
            }
            for (int a = 0; a < xc - 1; a++)      // insertion sort crossings
                for (int b = a + 1; b < xc; b++)
                    if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
            for (int a = 0; a + 1 < xc; a += 2)
                for (int xx = xs[a]; xx <= xs[a+1]; xx++)
                    dc_plot(dc, xx, yy, dc->brush_color);
        }
    }
    // Outline (pen): consecutive segments; Polygon closes the figure, Polyline does not.
    int segs = fill ? n : (n - 1);
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        int x0 = px[i], y0 = py[i], x1 = px[j], y1 = py[j];
        int ddx = x1 - x0, ddy = y1 - y0;
        int sx = ddx < 0 ? -1 : 1, sy = ddy < 0 ? -1 : 1;
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;
        int err = ddx - ddy, x = x0, y = y0;
        for (;;) {
            dc_plot(dc, x, y, dc->pen_color);
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 > -ddy) { err -= ddy; x += sx; }
            if (e2 < ddx)  { err += ddx; y += sy; }
        }
    }
}
static void g_polygon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_poly(c, ax, dx, argbytes, 1);
}
static void g_polyline(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_poly(c, ax, dx, argbytes, 0);
}
// GDI.90 GetTextColor(HDC): return the DC's current text colour. argbytes=2.
static void g_gettextcolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hdc = arg16(c,0); uint32_t cr = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) cr = g_dcs[hdc].text_color;
    *ax = (uint16_t)(cr & 0xFFFF); *dx = (uint16_t)(cr >> 16); *argbytes = 2;
}
// GDI.92 GetTextFace(HDC, int count, LPSTR lpFace): report a face name. argbytes=8.
static void g_gettextface(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t f_off = arg16(c,0), f_seg = arg16(c,1); int cnt = (int16_t)arg16(c,2);
    (void)arg16(c,3);
    const char *nm = "System"; int i = 0;
    if (cnt > 0) {
        for (; i < cnt-1 && nm[i]; i++) x86_16_wr8(c, f_seg, (uint16_t)(f_off+i), (uint8_t)nm[i]);
        x86_16_wr8(c, f_seg, (uint16_t)(f_off+i), 0);
    }
    *ax = (uint16_t)i; *dx = 0; *argbytes = 8;
}
// SetPixel (GDI.31, not imported) -- skip. GetTextMetrics (GDI.93):
//   (hdc, lpMetrics far) = 1+2 = 3w = 6 bytes. Fill a plausible TEXTMETRIC.
static void g_gettextmetrics(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                             uint16_t *argbytes) {
    uint16_t m_off = arg16(c, 0);
    uint16_t m_seg = arg16(c, 1);
    // TEXTMETRIC (Win16) starts with tmHeight, tmAscent, tmDescent ... (s_word).
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 0), FONT_HEIGHT);   // tmHeight
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 2), FONT_HEIGHT-3); // tmAscent
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 4), 3);             // tmDescent
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 6), 0);             // tmInternalLeading
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 8), 0);             // tmExternalLeading
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 10), FONT_WIDTH);   // tmAveCharWidth
    x86_16_wr16(c, m_seg, (uint16_t)(m_off + 12), FONT_WIDTH);   // tmMaxCharWidth
    *ax = 1; *dx = 0; *argbytes = 6;
}

// ExcludeClipRect (GDI.21): ExcludeClipRect(hdc, x1,y1,x2,y2 s_word x4) = 5w = 10 bytes.
// We do not model clipping regions; accept and balance the stack. Return COMPLEXREGION(3).
static void g_excludecliprect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                              uint16_t *argbytes) {
    (void)c; *ax = 3; *dx = 0; *argbytes = 10;
}

// IntersectClipRect (GDI.22): same signature/cleanup as ExcludeClipRect.
static void g_intersectcliprect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                                uint16_t *argbytes) {
    (void)c; *ax = 3; *dx = 0; *argbytes = 10;
}

// SetBkMode (GDI.2): SetBkMode(hdc, nBkMode s_word) = 2w = 4 bytes. Return prev (OPAQUE=2).
static void g_setbkmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    int16_t mode = (int16_t)arg16(c, 0);
    uint16_t hdc = arg16(c, 1);
    uint16_t prev = 2;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        prev = (uint16_t)(g_dcs[hdc].bk_mode ? g_dcs[hdc].bk_mode : 2);
        g_dcs[hdc].bk_mode = mode;   // 1=TRANSPARENT, 2=OPAQUE
    }
    *ax = prev; *dx = 0; *argbytes = 4;
}

// SetPixel (GDI.31): SetPixel(hdc, x,y s_word, crColor long) = 1+1+1+2 = 5w = 10 bytes.
static void g_setpixel(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                       uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    int y = (int16_t)arg16(c, 2);
    int x = (int16_t)arg16(c, 3);
    uint16_t hdc = arg16(c, 4);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        dc_lp2dp(&g_dcs[hdc], &x, &y);
        dc_plot(&g_dcs[hdc], x, y, colorref_to_fb(cr));
    }
    *ax = (uint16_t)(cr & 0xFFFF); *dx = (uint16_t)(cr >> 16); *argbytes = 10;
}

// CreatePalette (GDI.360): CreatePalette(lpLogPalette far) = 2w = 4 bytes.
// True-color framebuffer: return a non-zero pseudo handle so SelectPalette works.
static void g_createpalette(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    (void)c; *ax = win16_alloc_gdiobj(3, 0); if (*ax == 0) *ax = 0x200;
    *dx = 0; *argbytes = 4;
}

// GetDeviceCaps (GDI.80): GetDeviceCaps(hdc, nIndex s_word) = 2 words.
static void g_getdevicecaps(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                            uint16_t *argbytes) {
    int16_t idx = (int16_t)arg16(c, 0);
    uint16_t hdc = arg16(c, 1);
    win16_dc_t *dc = (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) ? &g_dcs[hdc] : 0;
    int printer = dc ? dc->is_printer : 0;
    uint16_t v = 0;
    // (#278 P53 ACTION 3) Fill in the page/physical-device metrics that were
    // previously returning 0. HORZSIZE/VERTSIZE are the physical mm dimensions;
    // Word reads them to establish the default page geometry when it has no
    // printer. Return US Letter (216x279 mm = 8.5"x11") so pagination has a real
    // page size instead of a degenerate 0. LOGPIXELS stays 96 dpi -> Letter is
    // 816x1056 device px, used for PHYSICALWIDTH/HEIGHT on a printer/IC DC.
    switch (idx) {
        case 0:  v = 0x0300; break; // DRIVERVERSION (3.0)
        case 2:  v = printer ? 4096 : 1; break; // TECHNOLOGY: DT_RASPRINTER / DT_RASDISPLAY
        case 4:  v = 216; break;  // HORZSIZE  (mm; Letter width 8.5in)
        case 6:  v = 279; break;  // VERTSIZE  (mm; Letter height 11in)
        case 8:  v = printer ? 816  : (uint16_t)fb_get_width();  break;  // HORZRES
        case 10: v = printer ? 1056 : (uint16_t)fb_get_height(); break;  // VERTRES
        case 12: v = 1;  break;  // PLANES
        case 14: v = 32; break;  // BITSPIXEL (32bpp truecolor framebuffer)
        // (#389 Magic School Bus) NUMCOLORS. Report 256, i.e. a palettized
        // 256-color device. NOTE: real WINE/Windows returns -1 for a >8bpp
        // truecolor display -- but disassembling KIDSCAT.EXE's color gate shows
        // it does a SIGNED `NUMCOLORS > 16` test, so -1 (0xFFFF) reads as -1 and
        // FAILS. The app fundamentally requires a 256-color PALETTE device (it
        // also requires RC_PALETTE below), which is exactly how it ran under real
        // Windows/WINE in 8bpp mode. So we present a 256-entry palette device:
        // NUMCOLORS=256 (>16 passes) plus RC_PALETTE in RASTERCAPS. Palette GDI
        // calls (CreatePalette/SelectPalette/RealizePalette) are already balanced
        // stubs here (CreatePalette returns a pseudo-handle for the truecolor FB).
        case 24: v = 256; break; // NUMCOLORS
        // RASTERCAPS: raster capabilities of a 256-color palettized display. The
        // key bit for the MSB gate is RC_PALETTE (0x100). Also expose the normal
        // blit/DIB/bigfont/stretch caps so apps that probe RASTERCAPS for BitBlt
        // etc. get realistic answers instead of 0 (the prior default = "no caps").
        //   RC_BITBLT 0x1 | RC_BITMAP64 0x8 | RC_GDI20_OUTPUT 0x10 | RC_DI_BITMAP
        //   0x80 | RC_PALETTE 0x100 | RC_DIBTODEV 0x200 | RC_BIGFONT 0x400 |
        //   RC_STRETCHBLT 0x800 | RC_FLOODFILL 0x1000 | RC_STRETCHDIB 0x2000
        case 38: v = 0x3B99; break; // RASTERCAPS (palettized display, RC_PALETTE set)
        case 88: v = 96; break;  // LOGPIXELSX
        case 90: v = 96; break;  // LOGPIXELSY
        // (#389 MSB) COLORRES: bits of color resolution. For a truecolor device
        // WINE reports 24 (8 bits each R/G/B). Apps use it as a secondary depth
        // check; 0 (our old default) can read as "monochrome".
        case 104: v = 256; break; // SIZEPALETTE (256-entry system palette)
        case 108: v = 24; break; // COLORRES
        case 110: v = printer ? 816  : (uint16_t)fb_get_width();  break;  // PHYSICALWIDTH
        case 111: v = printer ? 1056 : (uint16_t)fb_get_height(); break;  // PHYSICALHEIGHT
        case 112: v = 0; break;  // PHYSICALOFFSETX
        case 113: v = 0; break;  // PHYSICALOFFSETY
        default: v = 0; break;
    }
    // (#278 P52 runtime-callf-trace, avenue 1) log page/device-metric queries
    // (idx 2 TECHNOLOGY, 4 HORZSIZE, 6 VERTSIZE, 110 PHYSICALWIDTH, 111
    // PHYSICALHEIGHT, 112/113 PHYSICALOFFSETX/Y) plus every other index actually
    // hit, since these fall through to the `default: v = 0` arm above. If Word's
    // paginate reads a printer/page-size cap here and gets 0, that is a plausible
    // reason the view stays permanently needs-layout.
    { extern int g_w6life, g_w6seq;
      if (g_w6life) { static int ngdc=0; if (ngdc<40) { ngdc++;
        kprintf("[W6GDICAPS] SEQ %d: GetDeviceCaps(hdc=%04x, idx=%d) -> %u%s\n",
                g_w6seq++, arg16(c,1), idx, v, (v==0)?" ** ZERO **":""); } } }
    // (#389) Bounded, always-on diagnostic: the first N GetDeviceCaps queries of
    // every run, so an app's display-capability check (e.g. Magic School Bus'
    // "requires at least 256 colors" gate) shows the exact index + returned value
    // on the serial. Bounded to keep Word6 / interactive apps quiet.
    { static int ndg=0; if (ndg<48) { ndg++;
        kprintf("[GDICAPS] GetDeviceCaps(hdc=%04x, idx=%d) -> %d\n",
                (unsigned)arg16(c,1), idx, (int16_t)v); } }
    *ax = v; *dx = 0; *argbytes = 4;
}


// ===========================================================================
// Additional KERNEL handlers (file/string/selector/profile stubs) needed by
// FreeCell, JezzBall, Tut's Tomb, Golf, etc. All clean-room: argbytes match the
// documented Win16 Pascal signatures so the stack stays balanced.
// ===========================================================================

// _llseek (KERNEL.84): _llseek(hFile word, lOffset long, nOrigin word) = 4w = 8b.
// Returns the new position (DX:AX). origin: 0=begin, 1=current, 2=end.
static void k_llseek(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t origin = arg16(c, 0);
    int32_t  offset = (int32_t)arg32(c, 1);
    uint16_t h      = arg16(c, 3);
    *argbytes = 8; *ax = 0; *dx = 0;
    if (h < 1 || h >= WIN16_MAX_FILES || !g_files[h].used) { *ax = 0xFFFF; *dx = 0xFFFF; return; }
    win16_file_t *fp = &g_files[h];
    int32_t base = (origin == 1) ? (int32_t)fp->pos : (origin == 2) ? (int32_t)fp->size : 0;
    int32_t np = base + offset;
    if (np < 0) np = 0;
    // (#289 OLE2) Do NOT clamp forward seeks to the current size: STORAGE's
    // compound-file engine seeks past EOF to write sectors (FAT, directory) into
    // a freshly created 0-byte docfile. Clamping made every write land at the
    // wrong offset, corrupting the docfile and spinning StgCreateDocfile. A write
    // at a position beyond size zero-fills the gap (see k_lwrite).
    fp->pos = (uint32_t)np;
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[FIO] _llseek h=%d origin=%u off=%d -> pos=%u size=%u\n", h, origin, (int)offset, fp->pos, fp->size); }
    *ax = (uint16_t)(fp->pos & 0xFFFF); *dx = (uint16_t)(fp->pos >> 16);
}
// _lopen (KERNEL.85): _lopen(lpPathName far, iReadWrite word) = 3w = 6b.
static void k_lopen(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* iReadWrite */ (void)arg16(c, 0);
    uint16_t name_off = arg16(c, 1), name_seg = arg16(c, 2);
    *ax = (uint16_t)win16_file_open(c, name_seg, name_off, 0); *dx = 0; *argbytes = 6;
}
// _lcreat (KERNEL.83): _lcreat(lpPathName far, iAttribute word) = 3w = 6b.
static void k_lcreat(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* iAttribute */ (void)arg16(c, 0);
    uint16_t name_off = arg16(c, 1), name_seg = arg16(c, 2);
    *ax = (uint16_t)win16_file_open(c, name_seg, name_off, 1); *dx = 0; *argbytes = 6;
}
// _lwrite (KERNEL.86): _lwrite(hFile, lpBuf far, wBytes) = 4w = 8b. Returns count.
static void k_lwrite(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cnt     = arg16(c, 0);
    uint16_t buf_off = arg16(c, 1), buf_seg = arg16(c, 2);
    uint16_t h       = arg16(c, 3);
    *argbytes = 8; *ax = 0; *dx = 0;
    if (h < 1 || h >= WIN16_MAX_FILES || !g_files[h].used) return;
    win16_file_t *fp = &g_files[h];
    // Grow the buffer if needed.
    if (fp->pos + cnt > fp->cap) {
        uint32_t ncap = fp->cap ? fp->cap : 4096;
        while (ncap < fp->pos + cnt) ncap *= 2;
        uint8_t *nb = (uint8_t *)kmalloc(ncap);
        if (!nb) return;
        for (uint32_t i = 0; i < fp->size; i++) nb[i] = fp->buf[i];
        if (fp->buf) kfree(fp->buf);
        fp->buf = nb; fp->cap = ncap;
    }
    // (#289 OLE2) Zero-fill any gap created by a seek past EOF so the sparse
    // region reads as 0 (the compound-file engine relies on this).
    for (uint32_t g = fp->size; g < fp->pos; g++) fp->buf[g] = 0;
    for (uint16_t i = 0; i < cnt; i++)
        fp->buf[fp->pos++] = x86_16_rd8(c, buf_seg, (uint16_t)(buf_off + i));
    if (fp->pos > fp->size) fp->size = fp->pos;
    fp->dirty = 1;
    *ax = cnt;
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[FIO] _lwrite h=%d pos->%u cnt=%u b0=%02x%02x%02x%02x size=%u\n", h, fp->pos, cnt, cnt>0?fp->buf[fp->pos-cnt]:0, cnt>1?fp->buf[fp->pos-cnt+1]:0, cnt>2?fp->buf[fp->pos-cnt+2]:0, cnt>3?fp->buf[fp->pos-cnt+3]:0, fp->size); }
}
// lstrcpy (KERNEL.88): lstrcpy(lpDest far, lpSrc far) = 4w = 8b. Returns lpDest.
// KERNEL.346 / KERNEL.347 (#289 OLE2): STORAGE callsites (seg5:0x1980, seg2:0x1336)
// push FOUR words = TWO DWORDs (two far pointers) and check the return with
//  (AX==0 => STG_E 0x80030009). The previous UNIMPL path
// popped argbytes=0, desyncing the Pascal stack mid-Commit and derailing the
// sector-write loop. Pop the correct 8 bytes and return success (AX!=0) so the
// docfile flush continues. Traced so the exact semantics can be confirmed on disk.
static void k_kernel346(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    uint16_t a_off=arg16(c,0), a_seg=arg16(c,1), b_off=arg16(c,2), b_seg=arg16(c,3);
    // STORAGE: nonzero return = FAILURE (seg5:0x1985 or-ax-jnz-error). 0 = OK.
    *ax = 0; *dx = 0; *argbytes = 8;
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[K346] p1=%04x:%04x p2=%04x:%04x -> ok\n", a_seg,a_off,b_seg,b_off); }
}
static void k_kernel347(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    uint16_t a_off=arg16(c,0), a_seg=arg16(c,1), b_off=arg16(c,2), b_seg=arg16(c,3);
    // STORAGE: nonzero return = FAILURE (seg2:0x133b or-ax-jz-success). 0 = OK.
    *ax = 0; *dx = 0; *argbytes = 8;
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[K347] p1=%04x:%04x p2=%04x:%04x -> ok\n", a_seg,a_off,b_seg,b_off); }
}
static void k_hread(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                    uint16_t *argbytes) {
    // _hread (KERNEL.349): LONG _hread(HFILE hf, void _huge *hpvBuf, LONG cb).
    // Pascal push order (STORAGE seg2:0x0b1d, verified): push hFile; push buf_seg;
    // push buf_off; push cb_hi; push cb_lo. So arg16(0)=cb_lo, arg16(1)=cb_hi,
    // arg16(2)=buf_off, arg16(3)=buf_seg, arg16(4)=hFile. Returns bytes read in
    // DX:AX; STORAGE checks read>=cb (else STG_E). Reads from the current file pos
    // (set by the preceding _llseek). 10 argbytes (5 words).
    uint32_t cb     = arg32(c, 0);
    uint16_t buf_off= arg16(c, 2), buf_seg = arg16(c, 3);
    uint16_t h      = arg16(c, 4);
    *argbytes = 10; *ax = 0; *dx = 0;
    if (h < 1 || h >= WIN16_MAX_FILES || !g_files[h].used) return;
    win16_file_t *fp = &g_files[h];
    uint32_t n = 0;
    for (; n < cb && fp->pos < fp->size; n++)
        x86_16_wr8(c, buf_seg, (uint16_t)(buf_off + (n & 0xFFFF)), fp->buf[fp->pos++]);
    *ax = (uint16_t)(n & 0xFFFF); *dx = (uint16_t)(n >> 16);
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[FIO] _hread h=%d cb=%u pos->%u read=%u b0=%02x%02x%02x%02x size=%u\n", h, cb, fp->pos, n, n>0?x86_16_rd8(c,buf_seg,buf_off):0, n>1?x86_16_rd8(c,buf_seg,(uint16_t)(buf_off+1)):0, n>2?x86_16_rd8(c,buf_seg,(uint16_t)(buf_off+2)):0, n>3?x86_16_rd8(c,buf_seg,(uint16_t)(buf_off+3)):0, fp->size); }
}
static void k_kernel350(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                        uint16_t *argbytes) {
    // _hwrite (KERNEL.350): LONG _hwrite(HFILE hf, const void _huge *hpvBuf, LONG cb).
    // (#289) PROVEN by tracing: this is the docfile sector WRITE primitive, not a
    // SetFilePointer echo. STORAGE seg2:0x0c00 push order = push hFile;
    // push buf_seg; push buf_off; push cb_hi; push cb_lo, then it compares the
    // returned byte count vs cb ([bp+0x16]:[bp+0x18]); written<cb => STG_E_MEDIUMFULL
    // (0x80030070). So we must ACTUALLY write cb bytes from buf to the file at the
    // current pos (set by the preceding _llseek) and return the count. The old
    // echo-the-position stub never wrote sector data, so Commit never flushed the
    // D0CF11E0 header/FAT/dir to disk. arg16(0)=cb_lo, arg16(1)=cb_hi,
    // arg16(2)=buf_off, arg16(3)=buf_seg, arg16(4)=hFile. 10 argbytes.
    uint32_t cb     = arg32(c, 0);
    uint16_t buf_off= arg16(c, 2), buf_seg = arg16(c, 3);
    uint16_t h      = arg16(c, 4);
    *argbytes = 10; *ax = 0; *dx = 0;
    if (h < 1 || h >= WIN16_MAX_FILES || !g_files[h].used) return;
    win16_file_t *fp = &g_files[h];
    // Grow the in-memory file buffer to hold pos+cb.
    if (fp->pos + cb > fp->cap) {
        uint32_t ncap = fp->cap ? fp->cap : 4096;
        while (ncap < fp->pos + cb) ncap *= 2;
        uint8_t *nb = (uint8_t *)kmalloc(ncap);
        if (!nb) return;
        for (uint32_t i = 0; i < fp->size; i++) nb[i] = fp->buf[i];
        if (fp->buf) kfree(fp->buf);
        fp->buf = nb; fp->cap = ncap;
    }
    // Zero-fill any gap from a seek past EOF (sparse docfile sectors).
    for (uint32_t g = fp->size; g < fp->pos; g++) fp->buf[g] = 0;
    for (uint32_t i = 0; i < cb; i++)
        fp->buf[fp->pos++] = x86_16_rd8(c, buf_seg, (uint16_t)(buf_off + (i & 0xFFFF)));
    if (fp->pos > fp->size) fp->size = fp->pos;
    fp->dirty = 1;
    *ax = (uint16_t)(cb & 0xFFFF); *dx = (uint16_t)(cb >> 16);
    { extern int g_ole2_k334log; if (g_ole2_k334log) kprintf("[FIO] _hwrite h=%d cb=%u pos->%u b0=%02x%02x%02x%02x size=%u\n", h, cb, fp->pos, cb>0?fp->buf[fp->pos-cb]:0, cb>1?fp->buf[fp->pos-cb+1]:0, cb>2?fp->buf[fp->pos-cb+2]:0, cb>3?fp->buf[fp->pos-cb+3]:0, fp->size); }
}

static void k_lstrcpy(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t src_off = arg16(c, 0), src_seg = arg16(c, 1);
    uint16_t dst_off = arg16(c, 2), dst_seg = arg16(c, 3);
    uint16_t i = 0;
    for (;;) {
        uint8_t ch = x86_16_rd8(c, src_seg, (uint16_t)(src_off + i));
        x86_16_wr8(c, dst_seg, (uint16_t)(dst_off + i), ch);
        if (ch == 0) break;
        if (++i == 0) break;   // wrap guard
    }
    *ax = dst_off; *dx = dst_seg; *argbytes = 8;
}
// lstrcat (KERNEL.89): lstrcat(lpDest far, lpSrc far) = 4w = 8b. Returns lpDest.
static void k_lstrcat(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t src_off = arg16(c, 0), src_seg = arg16(c, 1);
    uint16_t dst_off = arg16(c, 2), dst_seg = arg16(c, 3);
    uint16_t d = 0;
    while (x86_16_rd8(c, dst_seg, (uint16_t)(dst_off + d)) != 0) d++;
    uint16_t i = 0;
    for (;;) {
        uint8_t ch = x86_16_rd8(c, src_seg, (uint16_t)(src_off + i));
        x86_16_wr8(c, dst_seg, (uint16_t)(dst_off + d + i), ch);
        if (ch == 0) break;
        i++;
    }
    *ax = dst_off; *dx = dst_seg; *argbytes = 8;
}
// lstrlen (KERNEL.90): lstrlen(lpStr far) = 2w = 4b.
static void k_lstrlen(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off = arg16(c, 0), seg = arg16(c, 1);
    uint16_t n = 0;
    while (x86_16_rd8(c, seg, (uint16_t)(off + n)) != 0) { n++; if (n == 0) break; }
    *ax = n; *dx = 0; *argbytes = 4;
}
// lstrcmp (KERNEL.94): lstrcmp(lpStr1 far, lpStr2 far) = 4w = 8b.
static void k_lstrcmp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t b_off = arg16(c, 0), b_seg = arg16(c, 1);
    uint16_t a_off = arg16(c, 2), a_seg = arg16(c, 3);
    int16_t r = 0; uint16_t i = 0;
    for (;;) {
        uint8_t ca = x86_16_rd8(c, a_seg, (uint16_t)(a_off + i));
        uint8_t cb = x86_16_rd8(c, b_seg, (uint16_t)(b_off + i));
        if (ca != cb) { r = (int16_t)((int)ca - (int)cb); break; }
        if (ca == 0) break;
        i++;
    }
    *ax = (uint16_t)r; *dx = 0; *argbytes = 8;
}
// LoadLibrary (KERNEL.95): LoadLibrary(lpLibFileName far) = 2w = 4b. We have no
// DLL loader -> return an error code < 32 (2 = file not found) so apps that call
// it for an OPTIONAL helper DLL fall back gracefully.
// LoadLibrary (KERNEL.95): LoadLibrary(lpLibFileName far) = 2w = 4b. Return a
// module handle so a following GetProcAddress can resolve real exports (#148:
// Chips loads a helper DLL and GetProcAddress's an entry in WM_CREATE; a 0 return
// sent it into a long retry that blew the wndproc instruction budget -> WM_CREATE
// returned -1 -> "no main procedure").
// (#278 P55) Synthetic printer-driver handle. Word 6 is WYSIWYG: it reads
// WIN.INI [windows] device= (e.g. "HP LaserJet III,HPPCL5A,LPT1:"), LoadLibrary's
// the driver ("HPPCL5A.DRV"), then GetProcAddress's its DeviceMode/ExtDeviceMode/
// DeviceCapabilities exports to obtain a DEVMODE + page geometry BEFORE it will
// lay out the page. We have no real .DRV, so we synthesize one: a .DRV LoadLibrary
// that is not a loaded module returns WIN16_PRNDRV_HMOD, and GetProcAddress on
// that handle resolves the driver exports to native thunks (see prn_* handlers).
#define WIN16_PRNDRV_HMOD 0x00F5
static int prn_thunk_id(const char *name, uint16_t ordinal);
static int prn_name_is_drv(const char *n) {
    int L = 0; while (n[L]) L++;
    if (L < 4) return 0;
    const char *e = n + L - 4;
    return e[0]=='.' && (e[1]=='D'||e[1]=='d') && (e[2]=='R'||e[2]=='r') && (e[3]=='V'||e[3]=='v');
}
static void k_loadlibrary(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off = arg16(c, 0), seg = arg16(c, 1);
    char name[64]; rd_far_cstr(c, seg, off, name, sizeof(name));
    *ax = name[0] ? win16_load_library(name) : 0x0040;
    // (#278 P55) an unresolved *.DRV LoadLibrary is the printer driver Word wants;
    // hand back a synthetic handle so its exports can be GetProcAddress'd.
    if (*ax == 0x0040 && prn_name_is_drv(name)) *ax = WIN16_PRNDRV_HMOD;
    { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6LOADLIB] SEQ %d: LoadLibrary(\"%s\") -> %04x\n", g_w6seq++, name, *ax); }
    *dx = 0; *argbytes = 4;
}
// FreeLibrary (KERNEL.96): FreeLibrary(hLibModule) = 1w = 2b.
static void k_freelibrary(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
// GetProcAddress (KERNEL.50): (hModule, lpProcName far) = 1+2 = 3w = 6b. None.
// GetProcAddress (KERNEL.50): GetProcAddress(hModule word, lpProcName far) =
// 1+2 = 3w = 6b. lpProcName is a name string, OR (when its segment word is 0) the
// low word is an integer ordinal (MAKEINTRESOURCE style). Returns the export's
// far pointer in DX:AX, or 0:0 if unresolved. (#148)
static void k_getprocaddress(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t name_off = arg16(c, 0), name_seg = arg16(c, 1);
    uint16_t hmod     = arg16(c, 2);
    *argbytes = 6; *ax = 0; *dx = 0;
    // (#278 P55) synthetic printer driver export resolution.
    if (hmod == WIN16_PRNDRV_HMOD) {
        int id;
        if (name_seg == 0) id = prn_thunk_id(0, name_off);
        else { char nmp[64]; rd_far_cstr(c, name_seg, name_off, nmp, sizeof(nmp)); id = prn_thunk_id(nmp, 0); }
        if (id >= 0) { *ax = (uint16_t)id; *dx = 0xF000; }
        { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6PRNDRV] SEQ %d: GetProcAddress(PRNDRV h=%04x) -> thunk %04x:%04x\n", g_w6seq++, hmod, *dx, *ax); }
        return;
    }
    uint16_t seg = 0, off = 0;
    int ok;
    if (name_seg == 0) {
        ok = win16_get_proc_address(hmod, 0, name_off, &seg, &off);
    } else {
        char nm[64]; rd_far_cstr(c, name_seg, name_off, nm, sizeof(nm));
        ok = win16_get_proc_address(hmod, nm, 0, &seg, &off);
    }
    if (ok) { *ax = off; *dx = seg; }
    { extern int g_w6life, g_w6seq; if (g_w6life) {
        if (name_seg == 0) kprintf("[W6GETPROC] SEQ %d: GetProcAddress(h=%04x, #%u) -> %04x:%04x ok=%d\n", g_w6seq++, hmod, name_off, *dx, *ax, ok);
        else { char nm2[64]; rd_far_cstr(c, name_seg, name_off, nm2, sizeof(nm2)); kprintf("[W6GETPROC] SEQ %d: GetProcAddress(h=%04x, \"%s\") -> %04x:%04x ok=%d\n", g_w6seq++, hmod, nm2, *dx, *ax, ok); } } }
}
// Dos3Call (KERNEL.102): register-based INT 21h dispatch, NO Pascal stack args.
// Clear carry (success) and return AX unchanged-ish; argbytes = 0.
// DOS3Call (KERNEL.102): the Win16 gateway to an INT 21h call. Registers in/out are
// the live CPU registers (NOT Pascal stack args), so argbytes = 0. Word's SETUP uses
// it for AH=0x36 GetDiskFreeSpace (the temp-dir space check) and a few file/dir ops.
// We report a generous, fixed free space so installers proceed. (#188 Word6)
// (#188) Resolve a DOS path that sits at guest DS:DX into a native MayteraOS path.
static void dos3_resolve_dsdx(x86_16_cpu_t *c, char *out, int outsz) {
    char dos[160]; rd_far_cstr(c, c->ds, c->dx, dos, sizeof(dos));
    dos_resolve_path(dos, win16_get_appdir(), out, outsz);
}
static void dos3_set_cf(x86_16_cpu_t *c, int set) {
    if (set) c->flags |= WIN16_F_CF; else c->flags &= (uint16_t)~WIN16_F_CF;
}
// (#278 pass16) Last DOS error (INT 21 extended error). Word 6 MS-C runtime,
// after a failed DOS3Call, issues AH=59h (Get Extended Error) to retrieve the
// precise code; its GetTempFileName loop maps the result via a classifier that
// needs the real ENOENT (2) to recognize a free temp name. We were returning 0
// for AH=59h, so the error code was lost (0x8000 instead of 0x8002).
// (#278 pass16) FindFirst/FindNext (AH=4e/4f) + Set DTA (AH=1a) for the win16
// DOS3Call gateway. The MS-C runtime path classifier (used by GetTempFileName)
// calls FindFirst after a failed GetFileAttributes to confirm a candidate name
// is free; our old default returned FindFirst as success, so no temp name was
// ever considered free. Implement against the FAT so a non-existent pattern
// correctly fails (CF=1) and a real file fills the DTA.
static uint16_t g_dta_seg = 0, g_dta_off = 0x0080;
static char     g_find_dir[160];
static char     g_find_pat[16];
static uint32_t g_find_pos;

static int dos_wild_match(const char *pat, const char *name) {
    if (*pat == '\0') return *name == '\0';
    if (*pat == '*') {
        if (pat[1] == '\0') return 1;
        for (const char *n = name; ; n++) {
            if (dos_wild_match(pat + 1, n)) return 1;
            if (*n == '\0') return 0;
        }
    }
    if (*name == '\0') return 0;
    if (*pat != '?') {
        char a = *pat, b = *name;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return dos_wild_match(pat + 1, name + 1);
}

static void dos_dta_write(x86_16_cpu_t *c, const char *name83, uint32_t fsize, uint8_t attr) {
    uint16_t sg = g_dta_seg ? g_dta_seg : c->ds, o = g_dta_off;
    for (int i = 0; i < 0x15; i++) x86_16_wr8(c, sg, (uint16_t)(o + i), 0);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x15), attr);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x16), 0);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x17), 0);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x18), 0x21);  // date = 1980-01-01
    x86_16_wr8(c, sg, (uint16_t)(o + 0x19), 0x00);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x1a), (uint8_t)(fsize & 0xff));
    x86_16_wr8(c, sg, (uint16_t)(o + 0x1b), (uint8_t)((fsize >> 8) & 0xff));
    x86_16_wr8(c, sg, (uint16_t)(o + 0x1c), (uint8_t)((fsize >> 16) & 0xff));
    x86_16_wr8(c, sg, (uint16_t)(o + 0x1d), (uint8_t)((fsize >> 24) & 0xff));
    int i = 0; for (; name83[i] && i < 12; i++) x86_16_wr8(c, sg, (uint16_t)(o + 0x1e + i), (uint8_t)name83[i]);
    x86_16_wr8(c, sg, (uint16_t)(o + 0x1e + i), 0);
}

// Scan g_find_dir for the next entry matching g_find_pat at/after g_find_pos.
// Returns 0 + fills the DTA on a hit, -1 on no match.
static int dos_find_next(x86_16_cpu_t *c) {
    fat_file_t dir;
    const char *dp = g_find_dir[0] ? g_find_dir : "/";
    if (fat_open(&g_fat_fs, dp, &dir) != 0) return -1;
    if (!fat_is_dir(&dir)) { fat_close(&dir); return -1; }
    fat_dir_entry_t e; char nm[256]; uint32_t idx = 0; int hit = -1;
    while (fat_readdir(&dir, &e, nm) == 0) {
        if (idx++ < g_find_pos) continue;
        char up[16]; int k = 0;
        for (; nm[k] && k < 12; k++) { char ch = nm[k]; if (ch >= 'a' && ch <= 'z') ch -= 32; up[k] = ch; }
        up[k] = 0;
        if (dos_wild_match(g_find_pat, up)) {
            g_find_pos = idx;
            dos_dta_write(c, up, e.file_size, e.attr);
            hit = 0; break;
        }
    }
    fat_close(&dir);
    return hit;
}
static uint16_t g_dos3_last_err = 0;
static void k_dos3call(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    *argbytes = 0;
    uint8_t ah = (uint8_t)(c->ax >> 8);
    dos3_set_cf(c, 0);   // default: success (CF=0)
    switch (ah) {
        case 0x36: {   // Get Disk Free Space (DL=drive). Report ~256 MB free.
            c->ax = 8; c->cx = 512; c->bx = 0xFFFF; c->dx = 0xFFFF;
            *ax = c->ax; *dx = c->dx; return;
        }
        case 0x19:            // Get current default drive -> C: (2)
            c->ax = (uint16_t)((c->ax & 0xFF00) | 0x02); *ax = c->ax; *dx = 0; return;
        case 0x0E:            // Select disk (DL=drive). Return # of drives in AL.
            c->ax = (uint16_t)((c->ax & 0xFF00) | 0x1A); *ax = c->ax; return;
        case 0x30:            // Get DOS version -> 3.10
            c->ax = 0x0A03; *ax = c->ax; *dx = 0; return;
        case 0x25:            // Set interrupt vector: accept silently
        case 0x35:            // Get interrupt vector: return NULL (ES:BX = 0)
            c->es = 0; c->bx = 0; *ax = 0; *dx = 0; return;
        case 0x43: {          // Get/Set file attributes (DS:DX path). AL=0 get,1 set.
            uint8_t al = (uint8_t)(c->ax & 0xFF);
            char path[160]; dos3_resolve_dsdx(c, path, sizeof(path));
            if (al == 0) {    // GET: CF=0 + CX=attrs if exists, else CF=1 + AX=2.
                uint32_t sz = 0; void *d = fat_read_file(&g_fat_fs, path, &sz);
                if (d) { kfree(d); c->cx = 0x20; dos3_set_cf(c, 0); *ax = 0; g_dos3_last_err = 0; }
                else { c->ax = 0x02; dos3_set_cf(c, 1); *ax = 0x02; g_dos3_last_err = 2; }  // ENOENT
                *dx = c->dx; return;
            }
            *ax = 0; dos3_set_cf(c, 0); return;   // SET: accept
        }
        case 0x59: {          // Get Extended Error (BX=version). Return the last DOS
            // error so the MS-C runtime can classify it (e.g. ENOENT=2). AX=ext err,
            // BH=class, BL=action, CH=locus. CF cleared.
            c->ax = g_dos3_last_err;
            if (g_dos3_last_err == 2) { c->bx = 0x0801; c->cx = (uint16_t)((c->cx & 0x00FF) | 0x0200); }
            else { c->bx = 0; }
            dos3_set_cf(c, 0);
            *ax = c->ax; *dx = c->dx; return;
        }
        case 0x2A: {          // Get Date: CX=year, DH=month, DL=day, AL=weekday.
            int day = 1, month = 1, year = 2026, wday = 0;
            extern void rtc_read_date(int *, int *, int *, int *);
            rtc_read_date(&day, &month, &year, &wday);
            c->cx = (uint16_t)year;
            c->dx = (uint16_t)(((month & 0xFF) << 8) | (day & 0xFF));
            c->ax = (uint16_t)(wday & 0xFF);
            dos3_set_cf(c, 0); *ax = c->ax; *dx = c->dx; return;
        }
        case 0x2C: {          // Get Time: CH=hour, CL=min, DH=sec, DL=centisec.
            int hh = 0, mm = 0, ss = 0;
            extern void rtc_read_time(int *, int *, int *);
            rtc_read_time(&hh, &mm, &ss);
            c->cx = (uint16_t)(((hh & 0xFF) << 8) | (mm & 0xFF));
            c->dx = (uint16_t)(((ss & 0xFF) << 8) | 0);
            c->ax = 0; dos3_set_cf(c, 0); *ax = 0; *dx = c->dx; return;
        }
        case 0x1A:            // Set Disk Transfer Area (DS:DX)
            g_dta_seg = c->ds; g_dta_off = c->dx;
            *ax = 0; dos3_set_cf(c, 0); return;
        case 0x4E: {          // Find First File (DS:DX pattern, CX=attr mask)
            char path[176]; dos3_resolve_dsdx(c, path, sizeof(path));
            int sl = -1; for (int i = 0; path[i]; i++) if (path[i] == '/') sl = i;
            if (sl < 0) {
                g_find_dir[0] = 0;
                int j = 0; for (; path[j] && j < (int)sizeof(g_find_pat) - 1; j++) g_find_pat[j] = path[j];
                g_find_pat[j] = 0;
            } else {
                int dl = sl ? sl : 1; if (dl > (int)sizeof(g_find_dir) - 1) dl = (int)sizeof(g_find_dir) - 1;
                for (int i = 0; i < dl; i++) { g_find_dir[i] = path[i]; }
                g_find_dir[dl] = 0;
                int j = 0; for (; path[sl + 1 + j] && j < (int)sizeof(g_find_pat) - 1; j++) g_find_pat[j] = path[sl + 1 + j];
                g_find_pat[j] = 0;
            }
            int has_wild = 0; for (int i = 0; g_find_pat[i]; i++) if (g_find_pat[i] == '*' || g_find_pat[i] == '?') has_wild = 1;
            g_find_pos = 0;
            if (dos_find_next(c) == 0) { dos3_set_cf(c, 0); c->ax = 0; *ax = 0; g_dos3_last_err = 0; }
            else { uint16_t err = has_wild ? 0x12 : 0x02;   // 0x12 no-more-files, 0x02 file-not-found
                   dos3_set_cf(c, 1); c->ax = err; *ax = err; g_dos3_last_err = err; }
            *dx = c->dx; return;
        }
        case 0x4F: {          // Find Next File (continues g_find_pos search)
            if (dos_find_next(c) == 0) { dos3_set_cf(c, 0); c->ax = 0; *ax = 0; g_dos3_last_err = 0; }
            else { dos3_set_cf(c, 1); c->ax = 0x12; *ax = 0x12; g_dos3_last_err = 0x12; }
            *dx = c->dx; return;
        }
        case 0x39: {          // MkDir (DS:DX). Actually create the directory.
            char path[160]; dos3_resolve_dsdx(c, path, sizeof(path));
            int rc = fat_mkdir(&g_fat_fs, path);
            dos3_set_cf(c, rc < 0 ? 0 : 0);   // report success regardless (best effort)
            *ax = 0; return;
        }
        case 0x3A:            // RmDir: accept
        case 0x3B:            // ChDir: accept (single working dir model)
        case 0x47:            // Get current directory: write "" into DS:SI
            if (ah == 0x47) x86_16_wr8(c, c->ds, c->si, 0);
            *ax = 0; dos3_set_cf(c, 0); return;
        default:
            c->ax = 0; *ax = 0; *dx = 0; return;
    }
}
// WritePrivateProfileString (KERNEL.129): (lpSection,lpKey,lpString,lpFile) =
//   4 far ptrs = 8w = 16b. (#133) Writes the named .ini via the routed FS.
static void k_writeprivprofile(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t file_off = arg16(c, 0), file_seg = arg16(c, 1);
    uint16_t str_off  = arg16(c, 2), str_seg  = arg16(c, 3);
    uint16_t key_off  = arg16(c, 4), key_seg  = arg16(c, 5);
    uint16_t sec_off  = arg16(c, 6), sec_seg  = arg16(c, 7);
    char file[160], sec[64], key[64], str[256];
    rd_far_cstr(c, file_seg, file_off, file, sizeof(file));
    rd_far_cstr(c, sec_seg,  sec_off,  sec,  sizeof(sec));
    rd_far_cstr(c, key_seg,  key_off,  key,  sizeof(key));
    int has_str = (str_seg || str_off);
    if (has_str) rd_far_cstr(c, str_seg, str_off, str, sizeof(str));
    *ax = (uint16_t)ini_set_string(file, sec, key, has_str ? str : 0);
    *dx = 0; *argbytes = 16;
}
// GetPrivateProfileInt (KERNEL.127): (lpSec,lpKey,nDefault,lpFile) = 2+2+1+2 = 7w = 14b.
static void k_getprivprofileint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t file_off = arg16(c, 0), file_seg = arg16(c, 1);
    int def           = (int16_t)arg16(c, 2);
    uint16_t key_off  = arg16(c, 3), key_seg = arg16(c, 4);
    uint16_t sec_off  = arg16(c, 5), sec_seg = arg16(c, 6);
    char file[160], sec[64], key[64], val[64];
    rd_far_cstr(c, file_seg, file_off, file, sizeof(file));
    rd_far_cstr(c, sec_seg,  sec_off,  sec,  sizeof(sec));
    rd_far_cstr(c, key_seg,  key_off,  key,  sizeof(key));
    *ax = (uint16_t)(ini_get_string(file, sec, key, "", val, sizeof(val))
                     ? pf_atoi(val) : def);
    *dx = 0; *argbytes = 14;
}
// GetPrivateProfileString (KERNEL.128): (lpSec,lpKey,lpDef,lpRet,cbRet,lpFile) =
//   2+2+2+2+1+2 = 11w = 22b. (#133) Reads the named .ini; falls back to lpDefault.
static void k_getprivprofilestr(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t file_off = arg16(c, 0), file_seg = arg16(c, 1);
    uint16_t cb       = arg16(c, 2);
    uint16_t ret_off  = arg16(c, 3), ret_seg = arg16(c, 4);
    uint16_t def_off  = arg16(c, 5), def_seg = arg16(c, 6);
    uint16_t key_off  = arg16(c, 7), key_seg = arg16(c, 8);
    uint16_t sec_off  = arg16(c, 9), sec_seg = arg16(c, 10);
    char file[160], sec[64], key[64], def[256], outb[256];
    rd_far_cstr(c, file_seg, file_off, file, sizeof(file));
    rd_far_cstr(c, sec_seg,  sec_off,  sec,  sizeof(sec));
    rd_far_cstr(c, def_seg,  def_off,  def,  sizeof(def));
    // (#188 Word6) A NULL/empty key means "enumerate every key name in [section]"
    // (returned NUL-separated, double-NUL terminated). Word's SETUP uses this on
    // [Files]. A NULL key pointer arrives as seg:off == 0:0.
    int key_is_null = (key_seg == 0 && key_off == 0);
    if (!key_is_null) rd_far_cstr(c, key_seg, key_off, key, sizeof(key));
    else key[0] = '\0';
    uint16_t i = 0;
    if (key_is_null || key[0] == '\0') {
        char buf[256];
        int n = ini_enum_keys(file, sec, buf, (int)(cb < sizeof(buf) ? cb : sizeof(buf)));
        // copy n bytes verbatim (embedded NULs preserved) + the final NUL.
        for (; i < (uint16_t)n && i < (uint16_t)cb; i++)
            x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), (uint8_t)buf[i]);
        if (i < (uint16_t)cb) x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), 0);
        *ax = i; *dx = 0; *argbytes = 22;
        return;
    }
    int n = ini_get_string(file, sec, key, def, outb,
                           (int)(cb < sizeof(outb) ? cb : sizeof(outb)));
    if (cb > 0) {
        for (; i < (uint16_t)n && i < (uint16_t)(cb - 1); i++)
            x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), (uint8_t)outb[i]);
        x86_16_wr8(c, ret_seg, (uint16_t)(ret_off + i), 0);
    }
    *ax = i; *dx = 0; *argbytes = 22;
}
// GetCurrentPDB (KERNEL.37): no args -> AX = current PSP/PDB segment. VBRUN100's
// MS-C startup calls this, then reads the environment segment at PSP:0x2C and the
// command tail at PSP:0x80 to build argv. Returning 0 (the old UNIMPL default)
// made it dereference a NULL env segment and abort via FatalAppExit("C RUNTIME
// ERROR"). The interpreter already uses paragraph 0x0080 as the PSP (INT21 AH=62
// GetPSP, the command-line block); return that and lazily populate the fields the
// C runtime reads: a real double-NUL-terminated env block with the program path
// appended (DOS 3+ stores it after the env), plus an empty command tail.
static uint16_t g_pdb_env_seg = 0;
static void k_getcurrentpdb(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t psp = 0x0080;
    if (!g_pdb_env_seg) {
        static const char *ev[] = { "PATH=C:\\WINDOWS", "windir=C:\\WINDOWS", 0 };
        static const char prog[] = "C:\\PROG.EXE";
        uint16_t seg = heap_alloc(256);
        if (seg) {
            uint16_t i = 0;
            for (int e = 0; ev[e]; e++) {
                for (const char *p = ev[e]; *p; p++) x86_16_wr8(c, seg, i++, (uint8_t)*p);
                x86_16_wr8(c, seg, i++, 0);
            }
            x86_16_wr8(c, seg, i++, 0);            // double-NUL end of env
            x86_16_wr8(c, seg, i++, 1); x86_16_wr8(c, seg, i++, 0);  // WORD program-count = 1
            for (const char *p = prog; *p; p++) x86_16_wr8(c, seg, i++, (uint8_t)*p);
            x86_16_wr8(c, seg, i++, 0);
            g_pdb_env_seg = seg;
        }
    }
    x86_16_wr16(c, psp, 0x2C, g_pdb_env_seg);      // environment segment
    x86_16_wr8(c, psp, 0x80, 0);                   // command-tail length = 0
    x86_16_wr8(c, psp, 0x81, 0x0D);                // CR terminator
    *ax = psp; *dx = 0; *argbytes = 0;
}

// GetDOSEnvironment (KERNEL.131): no args. (#133) Return a far ptr to a real
// DOS environment block: NUL-separated VAR=VALUE strings, double-NUL terminated.
static void k_getdosenv(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    static const char env[] =
        "PATH=C:\\WINDOWS\0TEMP=C:\\WINDOWS\\TEMP\0windir=C:\\WINDOWS\0COMSPEC=C:\\COMMAND.COM\0";
    uint16_t total = (uint16_t)(sizeof(env) + 1);   // +1 for the final terminating NUL
    uint16_t seg = heap_alloc(total);
    if (seg) {
        for (uint16_t i = 0; i < sizeof(env); i++)
            x86_16_wr8(c, seg, i, (uint8_t)env[i]);
        x86_16_wr8(c, seg, (uint16_t)sizeof(env), 0);
    }
    *ax = 0; *dx = seg; *argbytes = 0;
}
// GetSystemDirectory (KERNEL.135): (lpBuffer far, uSize word) = 3w = 6b.
// Write "C:\\WINDOWS\\SYSTEM" if it fits; return length.
static void k_getsysdir(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t sz   = arg16(c, 0);
    uint16_t off  = arg16(c, 1), seg = arg16(c, 2);
    static const char path[] = "C:\\WINDOWS\\SYSTEM";
    uint16_t n = 0;
    if (sz > 0) {
        for (; path[n] && n < (uint16_t)(sz - 1); n++)
            x86_16_wr8(c, seg, (uint16_t)(off + n), (uint8_t)path[n]);
        x86_16_wr8(c, seg, (uint16_t)(off + n), 0);
    }
    *ax = n; *dx = 0; *argbytes = 6;
}
// GetWindowsDirectory (KERNEL.134): (lpBuffer far, uSize word) = 3w = 6b.
static void k_getwindir(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t sz   = arg16(c, 0);
    uint16_t off  = arg16(c, 1), seg = arg16(c, 2);
    static const char path[] = "C:\\WINDOWS";
    uint16_t n = 0;
    if (sz > 0) {
        for (; path[n] && n < (uint16_t)(sz - 1); n++)
            x86_16_wr8(c, seg, (uint16_t)(off + n), (uint8_t)path[n]);
        x86_16_wr8(c, seg, (uint16_t)(off + n), 0);
    }
    *ax = n; *dx = 0; *argbytes = 6;
}
// GetDriveType (KERNEL.136): (nDrive word) = 1w = 2b. nDrive is 0-based (0=A,
// 2=C, 4=E). Returns the Win16 drive type from the #257 drive layer. (Win16 has
// no DRIVE_CDROM constant; the CD drive E: reports DRIVE_REMOTE.)
static void k_getdrivetype(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t nDrive = arg16(c, 0);
    char letter = (char)('A' + (nDrive & 0x1F));
    *ax = (uint16_t)dos_drive_type(letter);
    *dx = 0; *argbytes = 2;
}
// FatalAppExit (KERNEL.137): (wAction word, lpText far) = 3w = 6b. Log + quit.
static void k_fatalappexit(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t t_off = arg16(c, 0), t_seg = arg16(c, 1);
    char msg[160]; rd_far_cstr(c, t_seg, t_off, msg, sizeof(msg));
    kprintf("[win16api]   FatalAppExit: %s\n", msg);
    { extern int g_ole2_k334log; if (g_ole2_k334log) { kprintf("[W6FAE] stack:");
        for (int q=0;q<16;q++) kprintf(" %04x", x86_16_rd16(c,c->ss,(uint16_t)(c->sp+q*2)));
        kprintf(" cs=%04x ip=%04x bp=%04x\n", c->cs, c->ip, c->bp); } }
    win16_trace("*** FatalAppExit: %s\n", msg);
    // Real FatalAppExit terminates the task and never returns. Halt the CPU so a
    // genuinely-failing app stops cleanly instead of running its post-call code
    // into garbage (we previously saw it spin to the 4M-insn cap at cs=0).
    g_quit_posted = 1; msgq_post(0, WM_QUIT, 0, 0);
    c->halted = 1;
    *ax = 0; *dx = 0; *argbytes = 6;
}
// Selector helpers (KERNEL.170 AllocCStoDSAlias / .171 AllocDStoCSAlias /
//   .176 FreeSelector / .177 PrestoChangoSelector / .172 AllocSelector). In our
// flat 1 MiB real-mode image, selector == segment paragraph, so an "alias" is
// just the same value. Return the input selector for alias calls, 0 for free.
static void k_alloc_alias(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    *ax = arg16(c, 0); *dx = 0; *argbytes = 2;   // AllocCStoDSAlias(selector) -> same
}
static void k_freeselector(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
// PrestoChangoSelector (KERNEL.177): (selSrc, selDst) = 2w = 4b. Return selDst.
static void k_prestochango(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    *ax = arg16(c, 0); *dx = 0; *argbytes = 4;    // selDst is the top word
}
// GetSelectorBase (KERNEL.335): DWORD GetSelectorBase(WORD sel) = 1 word.
// Return the descriptor linear base in DX:AX. In our arena model the base is the
// arena byte offset; STORAGE uses it to convert selector:offset into a flat
// pointer for its own sector-buffer arithmetic. (#289 b468)
static void k_getselectorbase(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // (#289 b489) CORRECTED argbytes: as STORAGE.DLL calls KERNEL.335 (call sites
    // seg2 0x1c72/0x14d4/0x1309), it pushes 3 words = 6 bytes:
    //   push <DWORD ptr/value>(2w); push <WORD count/flag>(1w).
    // So it is NOT the classic GetSelectorBase(WORD sel)=2 bytes; argbytes=2 under-
    // popped 4 bytes and desynced the Pascal stack (-> wild far-call to seg 0x0a00,
    // dx=0x8003 spin). Use argbytes=6. The args: a0 = the WORD (0x48/0x08/0x04 =
    // a size/count), a1:a2 = the DWORD (a far pointer). Return 0 (success); STORAGE
    // OR-checks the result. (If a real value is needed this is the next RE step.)
    uint16_t wcount = arg16(c, 0);          // the WORD (size/flag)
    uint16_t plo    = arg16(c, 1);
    uint16_t phi    = arg16(c, 2);
    extern int g_ole2_k334log;
    if (g_ole2_k334log) kprintf("[K335] w=%04x ptr=%04x:%04x -> 0 (argbytes=6)\n", wcount, phi, plo);
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;
}
// SetSelectorBase (KERNEL.336): WORD SetSelectorBase(WORD sel, DWORD base) =
// 1 word + 1 dword = 3 words. Update the descriptor base, return the selector.
// (#289 b468) Pascal push order: a0=base LOW, a1=base HIGH, a2=sel.
static void k_setselectorbase(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // (#289 b486) CORRECTED: as called by COMPOBJ marshaling (StgCreateDocfile path),
    // KERNEL.336 takes a SINGLE 4-byte arg (a far pointer / DWORD), NOT (WORD sel +
    // DWORD base)=6 bytes. The 6-byte stub over-popped 2 bytes, desyncing the Pascal
    // stack so the caller's epilogue `pop ds` grabbed a code selector (0x17) instead
    // of its DGROUP (0x97); that wrong ds was then used to build a far pointer to a
    // marshaling object, which landed in the CODE segment -> a null/garbage far-call
    // -> the long-standing COMPOBJ marshaling crash. argbytes MUST be 4. The result
    // is checked with OR ax,dx; JNZ->error, so return 0 = success.
    uint16_t lo = arg16(c, 0);   // far-ptr off (or DWORD low)
    uint16_t hi = arg16(c, 1);   // far-ptr seg (or DWORD high)
    extern int g_ole2_k334log;
    if (g_ole2_k334log) kprintf("[K336] arg=%04x:%04x -> 0 (argbytes=4)\n", hi, lo);
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}
// OutputDebugString (KERNEL.115): (lpOutputString far) = 2w = 4b.
static void k_outputdebugstr(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}
// Catch / Throw (KERNEL.55/56) used by some C runtimes; treat as 1 far ptr -> 0.
static void k_far1_zero(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;
}
// KERNEL.56 Throw(lpCatchBuf far, int nThrowBack) = ptr(4)+word(2) = 6 bytes.
// (#278 argbytes audit) Split from KERNEL.55 Catch(lpCatchBuf) which stays 4 on
// k_far1_zero. Sharing one handler under-popped Throw by 2 -> Pascal-stack desync
// in the C-runtime setjmp/longjmp unwinder (same bug class as USER.190).
static void k_throw(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;
}

// MS Entertainment Pack shared helper DLLs (WEP4UTIL/WEPUTIL). Ordinal 2 is a
// per-app init/registration check. We have no DLL backing, so report success
// (AX=1, carry clear) and pop NO args (these are typically register-only or
// no-arg). Returning success keeps TetraVex/JezzBall out of their "init failed"
// path (the empty/Application-Error MessageBox they showed otherwise).
static void k_wep_ok(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    c->flags &= (uint16_t)~WIN16_F_CF;
    *ax = 1; *dx = 0; *argbytes = 0;
}

// ===========================================================================
// Additional USER handlers.
// ===========================================================================

// LoadMenu (USER.150): LoadMenu(hInstance, lpMenuName far) = 1+2 = 3w = 6b.
static void u_loadmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // LoadMenu(hInstance, lpMenuName far). RT_MENU = type 4. (#152)
    uint16_t name_off = arg16(c, 0), name_seg = arg16(c, 1);
    uint16_t hinst    = arg16(c, 2);
    *argbytes = 6; *ax = 0; *dx = 0;
    uint32_t len = 0; const uint8_t *t = 0;
    if (name_seg == 0) {                       // MAKEINTRESOURCE(id)
        t = win16_get_resource(hinst, 4, name_off, &len);
    } else {
        char nm[32]; rd_far_cstr(c, name_seg, name_off, nm, sizeof(nm));
        t = win16_get_resource_by_name(hinst, 4, nm, &len);
        if (!t) t = win16_get_resource_first(hinst, 4, &len);
    }
    if (!t) t = win16_get_resource_first(hinst, 4, &len);
    if (!t || len < 4) return;
    *ax = win16_parse_menu(t, len);
}
// LoadString (USER.176): (hInstance, uID word, lpBuffer far, nBufferMax word) =
//   1+1+2+1 = 5w = 10b. RT_STRING groups 16 strings per block; block id is
//   (uID>>4)+1 and the string index within the block is uID&15. Each string is
//   stored length-prefixed (BYTE count, then ANSI chars, no terminator).
static void u_loadstring(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cb      = arg16(c, 0);
    uint16_t buf_off = arg16(c, 1), buf_seg = arg16(c, 2);
    uint16_t uid     = arg16(c, 3);
    uint16_t hinst   = arg16(c, 4);
    *argbytes = 10; *ax = 0; *dx = 0;
    if (cb == 0) return;
    uint32_t len = 0;
    const uint8_t *blk = win16_get_resource(hinst, 6 /*RT_STRING*/,
                                            (uint16_t)((uid >> 4) + 1), &len);
    if (!blk) { x86_16_wr8(c, buf_seg, buf_off, 0); return; }
    uint32_t p = 0;
    int idx = uid & 15;
    for (int i = 0; i < idx; i++) {
        if (p >= len) { x86_16_wr8(c, buf_seg, buf_off, 0); return; }
        p += 1u + blk[p];
    }
    if (p >= len) { x86_16_wr8(c, buf_seg, buf_off, 0); return; }
    uint8_t slen = blk[p++];
    uint16_t n = 0;
    for (; n < slen && n < (uint16_t)(cb - 1) && p < len; n++)
        x86_16_wr8(c, buf_seg, (uint16_t)(buf_off + n), blk[p++]);
    x86_16_wr8(c, buf_seg, (uint16_t)(buf_off + n), 0);
    *ax = n;
}
// ---------------------------------------------------------------------------
// Shared printf core for wsprintf (USER.420, C-stack varargs) and wvsprintf
// (USER.421, args off an lpArglist far pointer). The OLD parser only SKIPPED
// width digits and the 'l' modifier and dropped everything else (it ignored the
// '.' precision), so a format like "%2.2u" emitted ".2u" literally and "%02u"
// never zero-padded. SkiFree's HUD showed ":.2u  Dist: .2dm" because of exactly
// this. This rewrite honours [flags 0 -][width][.prec][l|h] for d/i/u/x/X/c/s/%.
// ---------------------------------------------------------------------------
typedef struct {
    x86_16_cpu_t *c;
    int           stack_vi;            // C-stack word index (from_list == 0)
    uint16_t      list_seg, list_off;  // lpArglist far pointer (from_list == 1)
    int           from_list;
} w16_va;
static uint16_t w16_va16(w16_va *v) {
    if (v->from_list) { uint16_t r = x86_16_rd16(v->c, v->list_seg, v->list_off);
                        v->list_off = (uint16_t)(v->list_off + 2); return r; }
    return arg16(v->c, v->stack_vi++);
}
static uint32_t w16_va32(w16_va *v) {
    uint16_t lo = w16_va16(v); uint16_t hi = w16_va16(v);
    return ((uint32_t)hi << 16) | lo;   // DWORD pushed low-word-last (Pascal/cdecl)
}
static uint16_t win16_vformat(x86_16_cpu_t *c, uint16_t out_seg, uint16_t out_off,
                              uint16_t fmt_seg, uint16_t fmt_off, w16_va *va) {
    uint16_t fi = 0, oi = 0;
    for (;;) {
        if (oi >= 1024) break;            // Win16 wsprintf buffer is 1KB
        uint8_t ch = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++));
        if (ch == 0) break;
        if (ch != '%') { x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), ch); continue; }
        uint8_t spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++));
        int zero = 0, left = 0;
        for (;;) {                        // flags
            if (spec == '0') zero = 1;
            else if (spec == '-') left = 1;
            else if (spec == '+' || spec == ' ' || spec == '#') { /* accepted */ }
            else break;
            spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++));
        }
        int width = 0;                    // width
        while (spec >= '0' && spec <= '9') { width = width*10 + (spec - '0');
            spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++)); }
        int prec = -1;                    // .precision
        if (spec == '.') { prec = 0; spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++));
            while (spec >= '0' && spec <= '9') { prec = prec*10 + (spec - '0');
                spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++)); } }
        int is_long = 0;                  // length: l/w -> 32-bit, h/N/F -> skip
        while (spec=='l' || spec=='h' || spec=='N' || spec=='F' || spec=='w') {
            if (spec=='l' || spec=='w') is_long = 1;
            spec = x86_16_rd8(c, fmt_seg, (uint16_t)(fmt_off + fi++));
        }
        if (spec == 's') {                // string: stream with width/precision
            uint16_t s_off = w16_va16(va), s_seg = w16_va16(va);
            int slen = 0; { uint16_t t = s_off; while (x86_16_rd8(c, s_seg, t)) { t++; if (++slen > 4096) break; } }
            if (prec >= 0 && slen > prec) slen = prec;
            int pad = width - slen; if (pad < 0) pad = 0;
            if (!left) while (pad-- > 0 && oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), ' ');
            for (int i = 0; i < slen && oi < 1024; i++)
                x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), x86_16_rd8(c, s_seg, (uint16_t)(s_off + i)));
            if (left) while (pad-- > 0 && oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), ' ');
            continue;
        }
        char body[48]; int bl = 0;        // formatted body (forward order)
        if (spec == '%') { body[bl++] = '%'; }
        else if (spec == 'c') { uint16_t v = w16_va16(va); body[bl++] = (char)v; }
        else if (spec == 'd' || spec == 'i' || spec == 'u') {
            uint32_t uv; int neg = 0;
            if (is_long) { uint32_t lv = w16_va32(va);
                if (spec != 'u' && (int32_t)lv < 0) { neg = 1; uv = (uint32_t)(-(int32_t)lv); } else uv = lv; }
            else { uint16_t sv = w16_va16(va);
                if (spec != 'u' && (int16_t)sv < 0) { neg = 1; uv = (uint16_t)(-(int16_t)sv); } else uv = sv; }
            char rev[16]; int rn = 0; if (!uv) rev[rn++] = '0';
            while (uv && rn < 15) { rev[rn++] = (char)('0' + uv % 10); uv /= 10; }
            while (prec >= 0 && rn < prec && rn < 15) rev[rn++] = '0';
            if (neg && rn < 15) rev[rn++] = '-';
            for (int i = rn - 1; i >= 0; i--) body[bl++] = rev[i];
        }
        else if (spec == 'x' || spec == 'X') {
            uint32_t uv = is_long ? w16_va32(va) : (uint32_t)w16_va16(va);
            const char *hx = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[16]; int rn = 0; if (!uv) rev[rn++] = '0';
            while (uv && rn < 15) { rev[rn++] = hx[uv & 0xF]; uv >>= 4; }
            while (prec >= 0 && rn < prec && rn < 15) rev[rn++] = '0';
            for (int i = rn - 1; i >= 0; i--) body[bl++] = rev[i];
        }
        else { body[bl++] = (char)spec; } // unknown conversion: emit literally
        if (bl > (int)sizeof(body)) bl = (int)sizeof(body);
        int pad = width - bl; if (pad < 0) pad = 0;
        int zpad = (zero && !left && prec < 0);
        if (!left) {
            if (zpad && bl > 0 && body[0] == '-') {       // sign before zero pad
                if (oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), '-');
                while (pad-- > 0 && oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), '0');
                for (int i = 1; i < bl && oi < 1024; i++) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), body[i]);
            } else {
                char pc = zpad ? '0' : ' ';
                while (pad-- > 0 && oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), pc);
                for (int i = 0; i < bl && oi < 1024; i++) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), body[i]);
            }
        } else {
            for (int i = 0; i < bl && oi < 1024; i++) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), body[i]);
            while (pad-- > 0 && oi < 1024) x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi++), ' ');
        }
    }
    x86_16_wr8(c, out_seg, (uint16_t)(out_off + oi), 0);
    return oi;
}

// wvsprintf (USER.421): wvsprintf(lpOut far, lpFmt far, lpArglist far) = 6w = 12b.
static void u_wvsprintf(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t arg_off = arg16(c, 0), arg_seg = arg16(c, 1);
    uint16_t fmt_off = arg16(c, 2), fmt_seg = arg16(c, 3);
    uint16_t out_off = arg16(c, 4), out_seg = arg16(c, 5);
    w16_va va = { c, 0, arg_seg, arg_off, 1 };
    *ax = win16_vformat(c, out_seg, out_off, fmt_seg, fmt_off, &va);
    *dx = 0; *argbytes = 12;
}
// IsWindow (USER.47): IsWindow(hwnd) = 1w = 2b.
static void u_iswindow(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    *ax = win_from_hwnd(hwnd) ? 1 : 0; *dx = 0; *argbytes = 2;
}
// GetParent (USER.46): GetParent(hwnd) = 1w = 2b.
static void u_getparent(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hwnd = arg16(c, 0);
    win16_window_t *w = win_from_hwnd(hwnd);
    *ax = w ? w->parent : 0; *dx = 0; *argbytes = 2;
}
// SetWindowText (USER.37): SetWindowText(hwnd, lpString far) = 1+2 = 3w = 6b.
static void u_setwindowtext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t s_off = arg16(c, 0), s_seg = arg16(c, 1);
    uint16_t hwnd  = arg16(c, 2);
    win16_window_t *w = win_from_hwnd(hwnd);
    if (w) { rd_far_cstr(c, s_seg, s_off, w->title, sizeof(w->title));
             // Reflect the title onto the taskbar/host window ONLY for the main
             // top-level window and never with an empty string, so a child's
             // SetWindowText (or a blank update) cannot wipe the taskbar label
             // (#145 - the host window was ending up titleless and filtered out).
             if (g_taskbar_win && !w->is_child && w->title[0])
                 window_set_title(g_taskbar_win, w->title);
             win16_draw_frame(w); }
    *ax = 0; *dx = 0; *argbytes = 6;
}
// SendMessage (USER.111): SendMessage(hwnd, msg, wParam, lParam) = 5w = 10b.
static void u_sendmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t lParam = arg32(c, 0);
    uint16_t wParam = arg16(c, 2);
    uint16_t msg    = arg16(c, 3);
    uint16_t hwnd   = arg16(c, 4);
    uint32_t r = win16_dispatch_to_window(hwnd, msg, wParam, lParam);
    *ax = (uint16_t)(r & 0xFFFF); *dx = (uint16_t)(r >> 16); *argbytes = 10;
}
// True if a Win16 virtual key is currently down. Mouse buttons read the live
// kernel mouse_buttons bitmap; keyboard keys read the down-state bitmap that the
// input pump maintains. Without this, polling apps (SkiFree jump, Golf swing,
// arrow-key games) never saw input even though messages were delivered.
static int win16_vk_down(uint16_t vk) {
    if (vk == 0x01) return (mouse_buttons & 1) ? 1 : 0;   // VK_LBUTTON
    if (vk == 0x02) return (mouse_buttons & 2) ? 1 : 0;   // VK_RBUTTON
    if (vk == 0x04) return (mouse_buttons & 4) ? 1 : 0;   // VK_MBUTTON
    return g_win16_keydown[vk & 0xFF] ? 1 : 0;
}
// GetKeyState (USER.106): GetKeyState(nVirtKey) = 1w = 2b. Returns 0xFF80 (high
// bit set = down) for a pressed key/button, else 0. Apps test the sign bit.
static void u_getkeystate(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t vk = arg16(c, 0);
    *ax = win16_vk_down(vk) ? 0xFF80 : 0;
    *dx = 0; *argbytes = 2;
}
// GetAsyncKeyState (USER.249): like GetKeyState but reads the instantaneous
// physical state. We report the same live down-state (high bit = currently down).
static void u_getasynckeystate(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t vk = arg16(c, 0);
    *ax = win16_vk_down(vk) ? 0x8000 : 0;
    *dx = 0; *argbytes = 2;
}
// SetCapture (USER.18)/ReleaseCapture(USER.19): capture stubs.
static void u_setcapture(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // (#393) Record the capture target so win16_pump_mouse routes ALL mouse input
    // to it (e.g. a scrollbar/slider being dragged). Returns the previous capture.
    uint16_t prev = g_win16_capture_hwnd;
    g_win16_capture_hwnd = arg16(c, 0);
    *ax = prev; *dx = 0; *argbytes = 2;
}
static void u_releasecapture(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; g_win16_capture_hwnd = 0; *ax = 1; *dx = 0; *argbytes = 0;   // (#393)
}
// wsprintf (USER.420): variadic C-stack printf. We cannot know its true argbytes
// (caller cleans up for the variadic USER.420), so format using args ABOVE the
// fmt ptr and report argbytes = 0 (caller pops). Mirrors wvsprintf formatting.
static void u_wsprintf(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // Win16 prototype: int FAR cdecl wsprintf(LPSTR lpOut, LPCSTR lpFmt, ...).
    // cdecl pushes right-to-left, so the LEFTMOST arg (lpOut) is at the TOP of the
    // stack (word index 0,1) and lpFmt follows at (2,3); varargs at [4]... CALLER
    // cleans, so argbytes=0. (#200: this was previously reversed, which made
    // SkiFree's assert wsprintf write its formatted text over the format string
    // and read lpOut as the format -> binary-garbage "Assertion Failed" boxes.)
    uint16_t out_off = arg16(c, 0), out_seg = arg16(c, 1);
    uint16_t fmt_off = arg16(c, 2), fmt_seg = arg16(c, 3);
    w16_va va = { c, 4, 0, 0, 0 };   // varargs start at C-stack word index 4
    *ax = win16_vformat(c, out_seg, out_off, fmt_seg, fmt_off, &va);
    *dx = 0; *argbytes = 0;          // _cdecl: caller cleans the stack
}

// GetTickCount (USER.13): no args. Return milliseconds since boot as a DWORD in
// DX:AX. Apps poll this for animation timing and intro delays; a STATIC value
// makes them spin forever (Tut's Tomb / TetraVex startup hang), so derive it
// from the kernel tick counter.
static void u_gettickcount(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint32_t ms = (uint32_t)((timer_ticks * 1000ULL) / hz);
    *ax = (uint16_t)(ms & 0xFFFF); *dx = (uint16_t)(ms >> 16); *argbytes = 0;
}
// GetCurrentTime (USER.15): alias of GetTickCount in Win16.

// ShowCursor (USER.71): ShowCursor(bShow) = 1w = 2b. Return new show count.
static void u_showcursor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
// GetSysColor (USER.180): GetSysColor(nIndex) = 1w = 2b. Return a COLORREF
// (0x00BBGGRR) for the requested system color. Card games use COLOR_WINDOW for
// the felt/table and COLOR_WINDOWTEXT for labels.
static void u_getsyscolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t idx = (int16_t)arg16(c, 0);
    uint32_t cr = win16_syscolor((int)idx);
    *ax = (uint16_t)(cr & 0xFFFF); *dx = (uint16_t)(cr >> 16); *argbytes = 2;
}

// WaitMessage (USER.112): no args. Pump input so the queue fills, then return.
static void u_waitmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; win16_pump_input();
    // (#205) Block briefly instead of busy-yielding so a WaitMessage-based idle
    // loop does not peg the CPU; GetMessage owns the longer event-aware wait.
    if (g_msgq_count == 0) { extern void proc_sleep(uint32_t ms); proc_sleep(8); }
    *ax = 1; *dx = 0; *argbytes = 0;
}
// InflateRect (USER.78): InflateRect(lpRect far, dx s_word, dy s_word) = 4w = 8b.
static void u_inflaterect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int dy = (int16_t)arg16(c, 0);
    int dxx= (int16_t)arg16(c, 1);
    uint16_t r_off = arg16(c, 2), r_seg = arg16(c, 3);
    int l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 0));
    int t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 2));
    int rr= (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 4));
    int b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 6));
    l -= dxx; rr += dxx; t -= dy; b += dy;
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 0), (uint16_t)l);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 2), (uint16_t)t);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 4), (uint16_t)rr);
    x86_16_wr16(c, r_seg, (uint16_t)(r_off + 6), (uint16_t)b);
    *ax = 1; *dx = 0; *argbytes = 8;
}
// SetWindowWord (USER.134): SetWindowWord(hwnd, nIndex s_word, wNewWord) = 3w = 6b.
// We do not keep per-window extra words; accept and return 0 (previous value).
// (#200 SkiFree) Map a GetWindowWord/SetWindowWord nIndex (a BYTE offset into the
// window's extra bytes) to a wndwords[] slot. Negative GWW_* indices fold into
// the high slots so they stay self-consistent. SkiFree stashes a near pointer to
// its own per-window object here and reads it back every paint/timer; the old
// no-op stubs returned 0, giving SkiFree a NULL pointer -> ski2.c null-arg
// assertions (lines 1005/1129/1204) and a blank slope.
static int win16_wndword_slot(int16_t nIndex) {
    int slot = (nIndex >= 0) ? (nIndex / 2) : (16 + ((-nIndex) / 2));
    if (slot < 0) slot = 0;
    if (slot > 31) slot = 31;
    return slot;
}
// SetWindowWord(hWnd, nIndex, wNewWord) PASCAL: top->bottom wNewWord[0],
// nIndex[1], hWnd[2]. Returns the PREVIOUS word.
static void u_setwindowword(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t wnew = arg16(c, 0);
    int16_t  nidx = (int16_t)arg16(c, 1);
    uint16_t hwnd = arg16(c, 2);
    uint16_t prev = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    if (w) { int sl = win16_wndword_slot(nidx); prev = w->wndwords[sl]; w->wndwords[sl] = wnew; }
    *ax = prev; *dx = 0; *argbytes = 6;
}
// GetWindowWord (USER.133): GetWindowWord(hWnd, nIndex) PASCAL: nIndex[0], hWnd[1].
static void u_getwindowword(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t  nidx = (int16_t)arg16(c, 0);
    uint16_t hwnd = arg16(c, 1);
    uint16_t val = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    if (w) val = w->wndwords[win16_wndword_slot(nidx)];
    *ax = val; *dx = 0; *argbytes = 4;
}
// (#256/#188) GetWindowLong (USER.135): GetWindowLong(HWND, int nIndex) = 2w = 4b.
// Returns a LONG (DX:AX). GWL_WNDPROC(-4) returns the window's wndproc far ptr
// (proc_seg:proc_off) so VB-style subclassers can save + CallWindowProc it.
// Other indices use a pair of wndwords[] slots as 32-bit storage.
static void u_getwindowlong(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int16_t  nidx = (int16_t)arg16(c, 0);
    uint16_t hwnd = arg16(c, 1);
    *argbytes = 4; *ax = 0; *dx = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    if (!w) return;
    if (nidx == -4) { *ax = w->proc_off; *dx = w->proc_seg; return; }  // GWL_WNDPROC
    if (nidx == -16) { *ax = (uint16_t)(w->dwstyle & 0xFFFF);           // GWL_STYLE
                       *dx = (uint16_t)(w->dwstyle >> 16); return; }    // (#278 P46)
    int sl = win16_wndword_slot(nidx);
    if (sl > 30) sl = 30;
    *ax = w->wndwords[sl];
    *dx = w->wndwords[sl + 1];
}
// SetWindowLong (USER.136): SetWindowLong(HWND, int nIndex, LONG) = 1+1+2 = 4w = 8b.
// Returns the previous LONG. GWL_WNDPROC(-4) replaces the wndproc (subclassing).
static void u_setwindowlong(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t lo   = arg16(c, 0);
    uint16_t hi   = arg16(c, 1);
    int16_t  nidx = (int16_t)arg16(c, 2);
    uint16_t hwnd = arg16(c, 3);
    *argbytes = 8; *ax = 0; *dx = 0;
    win16_window_t *w = win_from_hwnd(hwnd);
    if (!w) return;
    if (nidx == -4) {                          // GWL_WNDPROC: subclass
        *ax = w->proc_off; *dx = w->proc_seg;
        w->proc_off = lo; w->proc_seg = hi;
        return;
    }
    int sl = win16_wndword_slot(nidx);
    if (sl > 30) sl = 30;
    *ax = w->wndwords[sl]; *dx = w->wndwords[sl + 1];
    w->wndwords[sl] = lo; w->wndwords[sl + 1] = hi;
}
// (#369) The Win16 API thunk segment (mirrors WIN16_THUNK_SEG in exec/ne.c). A
// far pointer WIN16_THUNK_SEG:id is NOT guest code: `id` indexes g_imports[] and
// a guest CALLF to it is serviced by the farcall trap -> win16_api_dispatch.
#define WIN16_API_THUNK_SEG 0xF000
// Forward decl: the import->handler resolver (defined later in this file).
static win16_handler_fn find_handler(const win16_import_t *im);

// (#369) Invoke a "previous window procedure" that is actually a BUILT-IN API
// thunk (segment 0xF000), e.g. DefWindowProc or a superclassed control's original
// proc that resolved to an imported thunk. Executing such a pointer as guest code
// (via x86_16_call_far) fails, because the farcall trap only fires on a real
// guest CALLF opcode, not on a directly-set cs:ip -> the interpreter runs garbage
// out of the F000 guard region and returns rc=-1. That silently broke every
// subclassing app that chains through CallWindowProc: JezzBall's field never
// painted (its subclass proc chained the TurboWindow default paint via a thunk),
// and WEP games "died" on the first click (their input chain hit the garbage).
// Here we dispatch the thunk through the normal API layer instead. We build the
// exact Pascal argument frame the handler reads via arg16() (arg16(0)=lParam lo
// .. arg16(4)=hwnd), call it, then restore SP. Only fires for F000 prev-procs, so
// apps that chain to real guest code are completely unaffected (byte-identical).
static uint32_t win16_dispatch_thunk_proc(x86_16_cpu_t *c, uint16_t thunk_off,
                                          uint16_t hwnd, uint16_t msg,
                                          uint16_t wParam, uint32_t lParam) {
    const win16_import_t *im =
        (thunk_off < (uint16_t)g_import_count) ? &g_imports[thunk_off] : 0;
    win16_handler_fn fn = im ? find_handler(im) : 0;
    static int s_logged = 0;
    if (s_logged < 24) {
        s_logged++;
        if (im && im->by_ordinal)
            win16_trace("CallWindowProc thunk id=%u -> %s.#%u msg=%04x fn=%d\n",
                        thunk_off, im->module, im->ordinal, msg, fn ? 1 : 0);
        else if (im)
            win16_trace("CallWindowProc thunk id=%u -> %s.%s msg=%04x fn=%d\n",
                        thunk_off, im->module, im->name, msg, fn ? 1 : 0);
        else
            win16_trace("CallWindowProc thunk id=%u (no import) msg=%04x\n",
                        thunk_off, msg);
    }
    if (!fn) return 0;   // unknown prev proc: behave like DefWindowProc (0)
    uint16_t saved_sp = c->sp;
    uint16_t sp = c->sp;
    sp -= 2; x86_16_wr16(c, c->ss, sp, hwnd);                       // arg16(4)
    sp -= 2; x86_16_wr16(c, c->ss, sp, msg);                        // arg16(3)
    sp -= 2; x86_16_wr16(c, c->ss, sp, wParam);                     // arg16(2)
    sp -= 2; x86_16_wr16(c, c->ss, sp, (uint16_t)(lParam >> 16));   // arg16(1) lp hi
    sp -= 2; x86_16_wr16(c, c->ss, sp, (uint16_t)(lParam & 0xFFFF));// arg16(0) lp lo
    sp -= 2; x86_16_wr16(c, c->ss, sp, 0);   // dummy far-return CS (unused)
    sp -= 2; x86_16_wr16(c, c->ss, sp, 0);   // dummy far-return IP (unused)
    c->sp = sp;
    g_cpu = c;
    uint16_t rax = 0, rdx = 0, rargb = 0;
    fn(c, &rax, &rdx, &rargb);
    c->sp = saved_sp;
    return ((uint32_t)rdx << 16) | rax;
}

// CallWindowProc (USER.122): CallWindowProc(WNDPROC lpPrev, HWND, UINT msg,
//   WPARAM, LPARAM) = segptr(2w)+word+word+long(2w) = 12 bytes. Invokes the saved
//   (previous) window procedure. This is how subclassing chains messages, used
//   heavily by the VB runtime (#256). Returns the proc's LRESULT in DX:AX.
static void u_callwindowproc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t lp_lo = arg16(c, 0);
    uint16_t lp_hi = arg16(c, 1);
    uint16_t wParam = arg16(c, 2);
    uint16_t msg    = arg16(c, 3);
    uint16_t hwnd   = arg16(c, 4);
    uint16_t p_off  = arg16(c, 5);
    uint16_t p_seg  = arg16(c, 6);
    *argbytes = 14;
    uint32_t lParam = ((uint32_t)lp_hi << 16) | lp_lo;
    uint32_t r;
    if (p_seg == WIN16_API_THUNK_SEG) {
        // (#369) Previous proc is a built-in API thunk; dispatch it properly
        // instead of running the F000 guard region as code.
        r = win16_dispatch_thunk_proc(c, p_off, hwnd, msg, wParam, lParam);
    } else {
        r = win16_call_wndproc(p_seg, p_off, hwnd, msg, wParam, lParam);
    }
    *ax = (uint16_t)(r & 0xFFFF); *dx = (uint16_t)(r >> 16);
}
// DrawText (USER.85): DrawText(hdc, lpString far, nCount s_word, lpRect far,
//   wFormat word) = 1+2+1+2+1 = 7w = 14b. Draw text at the rect's top-left using
//   the bitmap font (DT_CENTER honoured loosely). Returns the text height.
static void u_drawtext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t fmt    = arg16(c, 0);   (void)fmt;
    uint16_t r_off  = arg16(c, 1), r_seg = arg16(c, 2);
    int16_t  n      = (int16_t)arg16(c, 3);
    uint16_t s_off  = arg16(c, 4), s_seg = arg16(c, 5);
    uint16_t hdc    = arg16(c, 6);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        int l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 0));
        int t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 2));
        int rr= (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off + 4));
        // count text length if n < 0 (NUL-terminated)
        int len = n;
        if (n < 0) { len = 0; while (x86_16_rd8(c, s_seg, (uint16_t)(s_off + len))) len++; }
        int textw = len * FONT_WIDTH;
        int cx = l;
        if (fmt & 0x0001) cx = l + ((rr - l) - textw) / 2;   // DT_CENTER
        if (cx < l) cx = l;
        for (int i = 0; i < len; i++) {
            char ch = (char)x86_16_rd8(c, s_seg, (uint16_t)(s_off + i));
            const uint8_t *gph = font_get_glyph(ch);
            if (gph) {
                for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
                    uint8_t bits = gph[row];
                    for (int col = 0; col < 8; col++)
                        if (bits & (0x80 >> col)) dc_plot(dc, cx + col, t + row, dc->text_color);
                }
            }
            cx += FONT_WIDTH;
        }
    }
    *ax = FONT_HEIGHT; *dx = 0; *argbytes = 14;
}

// ===========================================================================
// Additional GDI handlers.
// ===========================================================================

// Read a pixel from a DC at client/buffer coords (returns 0 if out of range).
static uint32_t blt_get_pixel(win16_dc_t *dc, int x, int y) {
    if (!dc) return 0;
    if (dc->membuf) {
        if (x < 0 || y < 0 || x >= dc->mw || y >= dc->mh) return 0;
        return dc->membuf[y * dc->mw + x];
    }
    if (dc->win < 0) return 0;
    win16_window_t *w = &g_windows[dc->win];
    if (x < 0 || y < 0 || x >= w->cw || y >= w->ch) return 0;
    // Window content lives in the host canvas, not the framebuffer.
    return canvas_get(w->cx + x, w->cy + y);
}
// Combine a source and destination pixel per the Win16 ternary raster op.
// Works directly on packed FB_COLOR values (bitwise ops are packing-agnostic).
static uint32_t rop_combine(uint32_t rop, uint32_t s, uint32_t d) {
    switch (rop) {
        case 0x00CC0020UL: return s;             // SRCCOPY
        case 0x00EE0086UL: return s | d;         // SRCPAINT
        case 0x008800C6UL: return s & d;         // SRCAND
        case 0x00660046UL: return s ^ d;         // SRCINVERT
        case 0x00440328UL: return s & ~d;        // SRCERASE
        case 0x00330008UL: return ~s;            // NOTSRCCOPY
        case 0x001100A6UL: return ~(s | d);      // NOTSRCERASE
        case 0x00BB0226UL: return ~s | d;        // MERGEPAINT
        case 0x00000042UL: return 0;             // BLACKNESS
        case 0x00FF0062UL: return 0x00FFFFFFUL;  // WHITENESS
        default:           return s;             // treat unknown as SRCCOPY
    }
}

// (#255 perf B) Resolve a DC to its flat pixel buffer + stride + the canvas-space
// origin of its (0,0) client pixel and the clipped client bounds. Lets SRCCOPY
// blits (the dominant Win16 raster op) copy whole rows with memmove instead of
// the per-pixel blt_get_pixel/dc_plot loop. Returns 1 on success.
static int dc_surface(win16_dc_t *dc, uint32_t **base, int *stride,
                      int *ox, int *oy, int *maxw, int *maxh) {
    if (!dc) return 0;
    if (dc->membuf) {
        *base = dc->membuf; *stride = dc->mw;
        *ox = 0; *oy = 0; *maxw = dc->mw; *maxh = dc->mh;
        return (*maxw > 0 && *maxh > 0);
    }
    if (dc->win < 0 || !g_win16_canvas) return 0;
    win16_window_t *w = &g_windows[dc->win];
    *base = g_win16_canvas; *stride = g_win16_canvas_w;
    *ox = w->cx; *oy = w->cy;
    int cw = w->cw, ch = w->ch;
    if (w->cx + cw > g_win16_canvas_w) cw = g_win16_canvas_w - w->cx;
    if (w->cy + ch > g_win16_canvas_h) ch = g_win16_canvas_h - w->cy;
    *maxw = cw; *maxh = ch;
    return (cw > 0 && ch > 0);
}

// Copy a w x h block from src(sx,sy) to dst(dx,dy), honouring memory vs window
// and the raster op (so card masks AND/OR correctly).
static void blt_copy_rop(win16_dc_t *dst, int dx, int dy, int w, int h,
                         win16_dc_t *src, int sx, int sy, uint32_t rop) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    // (#255 perf B) Fast path for SRCCOPY: clip the rectangle once, then copy
    // whole rows with memmove (overlap-safe for screen self-blit scrolls). This
    // replaces ~w*h per-pixel function calls + bounds checks with one memmove per
    // row and is identical in output for the common in-bounds sprite blit.
    if (rop == 0x00CC0020UL) {
        uint32_t *sb, *db; int ss, ds, sox, soy, smw, smh, dox, doy, dmw, dmh;
        if (dc_surface(src, &sb, &ss, &sox, &soy, &smw, &smh) &&
            dc_surface(dst, &db, &ds, &dox, &doy, &dmw, &dmh)) {
            int cw = w, ch = h;
            // Clip to dst then src client bounds, adjusting the paired origin.
            if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
            if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
            if (sx < 0) { dx -= sx; cw += sx; sx = 0; }
            if (sy < 0) { dy -= sy; ch += sy; sy = 0; }
            if (dx + cw > dmw) cw = dmw - dx;
            if (dy + ch > dmh) ch = dmh - dy;
            if (sx + cw > smw) cw = smw - sx;
            if (sy + ch > smh) ch = smh - sy;
            if (cw <= 0 || ch <= 0) return;
            int dy0 = doy + dy, sy0 = soy + sy;
            int dx0 = dox + dx, sx0 = sox + sx;
            // Self-blit overlap: if copying down within the same buffer, go
            // bottom-up so we never overwrite a source row before reading it.
            if (sb == db && dy0 > sy0) {
                for (int r = ch - 1; r >= 0; r--)
                    memmove(db + (size_t)(dy0 + r) * ds + dx0,
                            sb + (size_t)(sy0 + r) * ss + sx0,
                            (size_t)cw * sizeof(uint32_t));
            } else {
                for (int r = 0; r < ch; r++)
                    memmove(db + (size_t)(dy0 + r) * ds + dx0,
                            sb + (size_t)(sy0 + r) * ss + sx0,
                            (size_t)cw * sizeof(uint32_t));
            }
            return;
        }
        // fall through to the generic loop if either surface is unavailable
    }
    int simple = (rop == 0x00CC0020UL);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            uint32_t s = blt_get_pixel(src, sx + xx, sy + yy);
            uint32_t v = simple ? s
                       : rop_combine(rop, s, blt_get_pixel(dst, dx + xx, dy + yy));
            dc_plot(dst, dx + xx, dy + yy, v);
        }
}

// ===========================================================================
// (#278 P55) Synthetic printer driver (HP LaserJet III / HPPCL5A). These native
// handlers back the thunks GetProcAddress hands out for the LoadLibrary'd printer
// .DRV, so Word 6 gets a valid DEVMODE + page geometry and completes its WYSIWYG
// layout, exactly as the reference Win3.1 env with a real HP LaserJet III does.
// ===========================================================================
static int prn_ci_eq(const char *a, const char *b) {
    while (*a && *b) { char x=*a,y=*b; if(x>='a'&&x<='z')x-=32; if(y>='a'&&y<='z')y-=32; if(x!=y)return 0; a++;b++; }
    return *a==0 && *b==0;
}
static int prn_find_import(const char *pname) {
    for (int i = 0; i < g_import_count; i++)
        if (!g_imports[i].by_ordinal && prn_ci_eq(g_imports[i].module, "PRNDRV")
            && prn_ci_eq(g_imports[i].name, pname)) return i;
    return -1;
}
static int prn_thunk_id(const char *name, uint16_t ordinal) {
    const char *pn = "GENERIC";
    if (name && name[0]) {
        if (prn_ci_eq(name, "DEVICEMODE")) pn = "DEVICEMODE";
        else if (prn_ci_eq(name, "EXTDEVICEMODE")) pn = "EXTDEVICEMODE";
        else if (prn_ci_eq(name, "DEVICECAPABILITIES")) pn = "DEVICECAPABILITIES";
    } else {
        if (ordinal == 13) pn = "DEVICEMODE";
        else if (ordinal == 90) pn = "EXTDEVICEMODE";
        else if (ordinal == 91) pn = "DEVICECAPABILITIES";
    }
    return prn_find_import(pn);
}
// Win 3.1 DEVMODE (dmSpecVersion 0x0300), 68-byte fixed part, US Letter.
#define PRN_DEVMODE_SIZE 68
static void prn_fill_devmode(x86_16_cpu_t *c, uint16_t seg, uint16_t off) {
    for (int i = 0; i < PRN_DEVMODE_SIZE; i++) x86_16_wr8(c, seg, (uint16_t)(off+i), 0);
    const char *dn = "HP LaserJet III";
    for (int i = 0; dn[i] && i < 31; i++) x86_16_wr8(c, seg, (uint16_t)(off+i), (uint8_t)dn[i]);
    x86_16_wr16(c, seg, (uint16_t)(off+32), 0x0300);          // dmSpecVersion
    x86_16_wr16(c, seg, (uint16_t)(off+34), 0x0300);          // dmDriverVersion
    x86_16_wr16(c, seg, (uint16_t)(off+36), PRN_DEVMODE_SIZE);// dmSize
    x86_16_wr16(c, seg, (uint16_t)(off+38), 0);               // dmDriverExtra
    uint32_t fields = 0x1|0x2|0x4|0x8|0x100|0x200|0x400|0x800;// orient,paper,len,wid,copies,src,qual,color
    x86_16_wr16(c, seg, (uint16_t)(off+40), (uint16_t)(fields & 0xffff));
    x86_16_wr16(c, seg, (uint16_t)(off+42), (uint16_t)(fields >> 16));
    x86_16_wr16(c, seg, (uint16_t)(off+44), 1);               // dmOrientation PORTRAIT
    x86_16_wr16(c, seg, (uint16_t)(off+46), 1);               // dmPaperSize LETTER
    x86_16_wr16(c, seg, (uint16_t)(off+48), 2794);            // dmPaperLength 0.1mm (11in)
    x86_16_wr16(c, seg, (uint16_t)(off+50), 2159);            // dmPaperWidth 0.1mm (8.5in)
    x86_16_wr16(c, seg, (uint16_t)(off+52), 100);             // dmScale
    x86_16_wr16(c, seg, (uint16_t)(off+54), 1);               // dmCopies
    x86_16_wr16(c, seg, (uint16_t)(off+56), 1);               // dmDefaultSource
    x86_16_wr16(c, seg, (uint16_t)(off+58), 300);             // dmPrintQuality (dpi)
    x86_16_wr16(c, seg, (uint16_t)(off+60), 1);               // dmColor MONOCHROME
    x86_16_wr16(c, seg, (uint16_t)(off+62), 1);               // dmDuplex SIMPLEX
    x86_16_wr16(c, seg, (uint16_t)(off+64), 300);             // dmYResolution
    x86_16_wr16(c, seg, (uint16_t)(off+66), 0);               // dmTTOption
}
// DeviceMode(HWND, HMODULE, LPSTR device, LPSTR output) = 6w=12b. Normally shows
// the printer-setup dialog; we succeed silently (no UI at startup).
static void prn_devicemode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 12;
    { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6PRNDRV] SEQ %d: DeviceMode() -> 0\n", g_w6seq++); }
}
// ExtDeviceMode(HWND,HANDLE hDriver,LPDEVMODE out,LPSTR dev,LPSTR port,
//   LPDEVMODE in,LPSTR profile,WORD fwMode) = 13w=26b.
static void prn_extdevicemode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t fwMode  = arg16(c, 0);
    uint16_t out_off = arg16(c, 9), out_seg = arg16(c, 10);
    *argbytes = 26; *dx = 0;
    if (fwMode == 0) { *ax = PRN_DEVMODE_SIZE; }
    else { if ((fwMode & 0x0002) && (out_seg|out_off)) prn_fill_devmode(c, out_seg, out_off); *ax = 1; }
    { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6PRNDRV] SEQ %d: ExtDeviceMode(fwMode=%04x) -> %04x\n", g_w6seq++, fwMode, *ax); }
}
// DeviceCapabilities(LPSTR dev,LPSTR port,WORD cap,LPSTR out,LPDEVMODE dm)=9w=18b.
static void prn_devcaps(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t out_off = arg16(c, 2), out_seg = arg16(c, 3);
    uint16_t cap     = arg16(c, 4);
    *argbytes = 18; *dx = 0; *ax = 0;
    int has_out = (out_seg|out_off) != 0;
    switch (cap) {
        case 1:  *ax = 0x0F0F; break;                         // DC_FIELDS (dmFields low word approx)
        case 2:  if (has_out) x86_16_wr16(c,out_seg,out_off,1);            *ax = 1; break; // DC_PAPERS: {LETTER}
        case 3:  if (has_out){x86_16_wr16(c,out_seg,out_off,2159);x86_16_wr16(c,out_seg,(uint16_t)(out_off+2),2794);} *ax = 1; break; // DC_PAPERSIZE (0.1mm)
        case 4:  if (has_out){int i;for(i=0;i<64;i++)x86_16_wr8(c,out_seg,(uint16_t)(out_off+i),0);const char*nm="Letter 8 1/2 x 11 in";for(i=0;nm[i]&&i<63;i++)x86_16_wr8(c,out_seg,(uint16_t)(out_off+i),(uint8_t)nm[i]);} *ax=1; break; // DC_PAPERNAMES
        case 8:  *ax = 1; break;                              // DC_MINEXTENT / others
        case 11: *ax = 0x0300; break;                        // DC_DRIVER version
        case 18: *ax = 1; break;                             // DC_COPIES
        default: *ax = has_out ? 0 : 1; break;
    }
    { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6PRNDRV] SEQ %d: DeviceCapabilities(cap=%u) -> %04x\n", g_w6seq++, cap, *ax); }
}
// Any other printer-driver export Word resolves: benign no-op (pops nothing; Word
// typically only checks the address is non-NULL for these). Logs if ever called.
static void prn_generic(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 0;
    { extern int g_w6life, g_w6seq; if (g_w6life) kprintf("[W6PRNDRV] SEQ %d: generic printer export CALLED (argbytes=0)\n", g_w6seq++); }
}
// CreateIC (GDI.153): CreateIC(lpDriver,lpDevice,lpOutput,lpInitData far x4) =
//   8w = 16b. When the driver is a printer (not "DISPLAY"), flag it so
//   GetDeviceCaps reports the physical page metrics Word's pagination needs.
static void g_createic(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int dc = dc_alloc(-1);
    char drv[32]; uint16_t drv_off = arg16(c, 6), drv_seg = arg16(c, 7);
    rd_far_cstr(c, drv_seg, drv_off, drv, sizeof(drv));
    int printer = drv[0] && !prn_ci_eq(drv, "DISPLAY");
    if (dc && printer) g_dcs[dc].is_printer = 1;
    { extern int g_w6life, g_w6seq;
      if (g_w6life) kprintf("[W6GDIDC] SEQ %d: CreateIC(drv=\"%s\") -> hdc=%d printer=%d\n", g_w6seq++, drv, dc, printer); }
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 16;
}
// CreateCompatibleDC (GDI.135): CreateCompatibleDC(hdc) = 1w = 2b. Memory DC.
// ALSO aliased from GDI.53 CreateDC (real signature 4 far ptrs = 8w = 16b) via
// the import table above; g_w6last_gdi_ord (latched by the generic dispatcher
// just before this runs) tells us which ordinal actually triggered this call.
// If ord==53 (CreateDC) fires, argbytes=2 below under-pops the caller's stack
// by 14 bytes - a Pascal stack desync. (#278 P52 runtime-callf-trace)
static void g_createcompatibledc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; int dc = dc_alloc(-1);
    { extern int g_w6life, g_w6seq, g_w6last_gdi_ord;
      if (g_w6life) {
        if (g_w6last_gdi_ord == 53)
          kprintf("[W6GDIDC] SEQ %d: *** CreateDC (GDI.53) via CreateCompatibleDC alias -> hdc=%d; "
                  "argbytes=2 hardcoded but real CreateDC pushes 16 - STACK DESYNC by 14 bytes ***\n",
                  g_w6seq++, dc);
        else
          kprintf("[W6GDIDC] SEQ %d: CreateCompatibleDC (ord=%d) -> hdc=%d\n",
                  g_w6seq++, g_w6last_gdi_ord, dc);
      } }
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 2;
}
// (#278 P53 ACTION 3) CreateDC (GDI.53): CreateDC(lpDriver,lpDevice,lpOutput,
// lpInitData) = 4 far ptrs = 8w = 16b. Previously aliased to CreateCompatibleDC
// which hardcodes argbytes=2, under-popping the Pascal stack by 14 bytes if real
// CreateDC ever fired. Give it its own handler: allocate a PRINTER-flagged DC
// (so GetDeviceCaps reports Letter page metrics) and pop the correct 16 bytes.
static void g_createdc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int dc = dc_alloc(-1);
    char drv[32]; uint16_t drv_off = arg16(c, 6), drv_seg = arg16(c, 7);
    rd_far_cstr(c, drv_seg, drv_off, drv, sizeof(drv));
    int printer = drv[0] && !prn_ci_eq(drv, "DISPLAY");
    if (dc && printer) g_dcs[dc].is_printer = 1;
    { extern int g_w6life, g_w6seq;
      if (g_w6life) kprintf("[W6GDIDC] SEQ %d: CreateDC(drv=\"%s\") -> hdc=%d printer=%d\n",
                            g_w6seq++, drv, dc, printer); }
    *ax = (uint16_t)dc; *dx = 0; *argbytes = 16;
}
// (#278 P53 ACTION 3) Escape (GDI.38): Escape(hdc, nEscape, cbInput, lpInData,
// lpOutData) = hdc(1w)+nEscape(1w)+cbInput(1w)+lpInData(2w)+lpOutData(2w) = 7w
// = 14b. Previously an unimplemented stub returning 0. Fill the printer/page
// query escapes so Word's page setup gets real values instead of 0.
//   GETPHYSPAGESIZE(12): lpOutData = POINT{cx,cy} physical page in device units.
//   GETPRINTINGOFFSET(13): lpOutData = POINT{x,y} printable-area origin.
//   GETSCALINGFACTOR(14): lpOutData = POINT{x,y} log2 scaling (0,0 = 1:1).
static void g_escape(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t nEscape = arg16(c, 3);      // hdc=arg5, nEscape=arg4? see order below
    // Pascal args pushed left-to-right: hdc, nEscape, cbInput, lpInData(off,seg),
    // lpOutData(off,seg). arg16(c,0) is the LAST pushed (lpOutData seg). So:
    //   lpOutData: seg=arg16(c,0) off=arg16(c,1); lpInData: seg=arg16(c,2) off=arg16(c,3);
    //   cbInput=arg16(c,4); nEscape=arg16(c,5); hdc=arg16(c,6).
    uint16_t out_seg = arg16(c, 0), out_off = arg16(c, 1);
    nEscape = arg16(c, 5);
    uint16_t hdc = arg16(c, 6);
    win16_dc_t *dc = (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) ? &g_dcs[hdc] : 0;
    int printer = dc ? dc->is_printer : 0;
    uint16_t pw = printer ? 816 : (uint16_t)fb_get_width();
    uint16_t ph = printer ? 1056 : (uint16_t)fb_get_height();
    uint16_t r = 0;
    switch (nEscape) {
        case 12: // GETPHYSPAGESIZE
            if (out_seg|out_off) { x86_16_wr16(c,out_seg,out_off,pw); x86_16_wr16(c,out_seg,(uint16_t)(out_off+2),ph); }
            r = 1; break;
        case 13: // GETPRINTINGOFFSET
            if (out_seg|out_off) { x86_16_wr16(c,out_seg,out_off,0); x86_16_wr16(c,out_seg,(uint16_t)(out_off+2),0); }
            r = 1; break;
        case 14: // GETSCALINGFACTOR
            if (out_seg|out_off) { x86_16_wr16(c,out_seg,out_off,0); x86_16_wr16(c,out_seg,(uint16_t)(out_off+2),0); }
            r = 1; break;
        default: r = 0; break;
    }
    { extern int g_w6life, g_w6seq;
      if (g_w6life) kprintf("[W6ESCAPE] SEQ %d: Escape(hdc=%04x nEscape=%u) -> %u (printer=%d)\n",
                            g_w6seq++, hdc, nEscape, r, printer); }
    *ax = r; *dx = 0; *argbytes = 14;
}
// DeleteDC (GDI.68): DeleteDC(hdc) = 1w = 2b.
static void g_deletedc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hdc = arg16(c, 0);
    if (hdc && hdc < WIN16_MAX_DC) g_dcs[hdc].used = 0;
    *ax = 1; *dx = 0; *argbytes = 2;
}
// CreateCompatibleBitmap (GDI.51): (hdc, w, h) = 3w = 6b. Pseudo handle.
static void g_createcompatiblebitmap(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int h = (int16_t)arg16(c, 0);
    int w = (int16_t)arg16(c, 1);
    /* hdc */ (void)arg16(c, 2);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;
    int obj = win16_alloc_gdiobj(4, 0);
    if (obj) {
        uint32_t *buf = (uint32_t *)kmalloc((uint32_t)w * h * 4);
        if (buf) { for (int i = 0; i < w*h; i++) buf[i] = FB_COLOR(0,0,0); }
        g_gdiobj[obj].pix = buf; g_gdiobj[obj].w = w; g_gdiobj[obj].h = h;
    }
    // (#278 word6) On failure return 0, NOT a bogus 0x300 handle. A >=0x100 fake
    // handle passed to SelectObject() is treated as a stock object, so the memory
    // DC never gets a pixel buffer bound and later BitBlts copy black. Returning 0
    // makes the failure honest (and with the enlarged pool it should not happen).
    W6LOG("[W6] CreateCompatibleBitmap %dx%d -> obj=%d pix=%d\n", w, h, obj, (obj && g_gdiobj[obj].pix) ? 1 : 0);
    *ax = (uint16_t)obj; *dx = 0; *argbytes = 6;
}
// CreateBitmap (GDI.48): CreateBitmap(nWidth, nHeight, nPlanes, nBitCount,
// lpBits far) = 4w + 1 far ptr = 6w = 12 bytes. This was previously
// UNIMPLEMENTED; the MISS path popped argbytes=0, desyncing the Pascal stack by
// 12 bytes and (for SkiFree) leaving its offscreen sprite bitmap uncreated, so
// the slope canvas stayed blank. Allocate a real type-4 bitmap object; if lpBits
// is supplied, decode the raw device-dependent bits (monochrome 1bpp, and
// 4/8/24 bpp) into the RGBA buffer. Monochrome bits map 0->black, 1->white
// (SkiFree builds its sprites/masks as DDBs and blits them per frame). (#204)
static void g_createbitmap(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t b_off = arg16(c, 0);
    uint16_t b_seg = arg16(c, 1);
    int bpp = (int)arg16(c, 2);
    int planes = (int)arg16(c, 3);
    int h = (int16_t)arg16(c, 4);
    int w = (int16_t)arg16(c, 5);
    *argbytes = 12; *ax = 0; *dx = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;
    if (planes > 1 && bpp == 1) bpp = planes;   // some apps pass planes for color
    int obj = win16_alloc_gdiobj(4, 0);
    if (!obj) { *ax = 0; return; }
    uint32_t *buf = (uint32_t *)kmalloc((uint32_t)w * h * 4);
    if (!buf) { g_gdiobj[obj].used = 0; *ax = 0; return; }
    for (int i = 0; i < w * h; i++) buf[i] = FB_COLOR(0, 0, 0);
    if (b_seg != 0 || b_off != 0) {
        // DDB scanlines are WORD-aligned (16-bit), top-down is NOT implied;
        // Win16 DDBs are top-down for CreateBitmap's lpBits.
        int rowbytes = ((w * bpp + 15) / 16) * 2;
        for (int y = 0; y < h; y++) {
            uint16_t roff = (uint16_t)(b_off + (uint32_t)y * rowbytes);
            for (int x = 0; x < w; x++) {
                uint8_t r = 0, g = 0, bl = 0;
                if (bpp == 1) {
                    uint8_t byte = x86_16_rd8(c, b_seg, (uint16_t)(roff + (x >> 3)));
                    int bit = (byte >> (7 - (x & 7))) & 1;
                    r = g = bl = bit ? 255 : 0;
                } else if (bpp == 4) {
                    uint8_t byte = x86_16_rd8(c, b_seg, (uint16_t)(roff + (x >> 1)));
                    int v = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                    r = g = bl = (uint8_t)(v * 17);
                } else if (bpp == 8) {
                    uint8_t v = x86_16_rd8(c, b_seg, (uint16_t)(roff + x));
                    r = g = bl = v;
                } else if (bpp == 24) {
                    bl = x86_16_rd8(c, b_seg, (uint16_t)(roff + x * 3 + 0));
                    g  = x86_16_rd8(c, b_seg, (uint16_t)(roff + x * 3 + 1));
                    r  = x86_16_rd8(c, b_seg, (uint16_t)(roff + x * 3 + 2));
                }
                buf[(uint32_t)y * w + x] = FB_COLOR(r, g, bl);
            }
        }
    }
    g_gdiobj[obj].pix = buf; g_gdiobj[obj].w = w; g_gdiobj[obj].h = h; g_gdiobj[obj].bpp = bpp;
    win16_trace("CreateBitmap %dx%d bpp=%d bits=%s -> obj=%d\n", w, h, bpp, (b_seg||b_off)?"yes":"null", obj);
    *ax = (uint16_t)obj;
}
// GetObject (GDI.82): GetObject(hObject, cbBuffer, lpvObject far) = 4w = 8b.
// Card games call this on a loaded card HBITMAP to read bmWidth/bmHeight before
// BitBlt'ing it; without it the BITMAP struct stays garbage and the blit is
// skipped, so no card faces appear. Fill a Win16 BITMAP (14 bytes) for bitmap
// objects; return the number of bytes written (0 = failure).
static void g_getobject(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t lpv_off = arg16(c, 0);
    uint16_t lpv_seg = arg16(c, 1);
    int      cb      = (int16_t)arg16(c, 2);
    uint16_t hobj    = arg16(c, 3);
    *argbytes = 8; *ax = 0; *dx = 0;
    // (#278 b577) FONT objects (type 5): Word's font-builder (WINWORD seg196:0x779)
    // calls GetObject(hFont, 0x32, &logfont) to read the font's LOGFONT and FAILS
    // app-init if it returns 0. Fill a Win16 LOGFONT (50 bytes) for any font handle
    // (app-created type-5 object or a stock SYSTEM/ANSI font handle) and return cb.
    {
        int is_font = (hobj < WIN16_MAX_GDIOBJ && g_gdiobj[hobj].used && g_gdiobj[hobj].type == 5);
        // Stock font ids 0x0..0xf region from GetStockObject (SYSTEM_FONT=13, etc.)
        // and our pseudo CreateFont fallback handle 0x400 also count as fonts.
        if (!is_font && (hobj == 0x400 || hobj == 13 || hobj == 10 || hobj == 11 ||
                         hobj == 12 || hobj == 17 || hobj == 16))
            is_font = 1;
        // (#278 pass22) Stock font handles from GetStockObject (0x10a..0x110). Word's
        // font-init builder (seg196:0x5f4) does GetStockObject(SYSTEM_FONT), stores the
        // handle at DGROUP:[0x3e36], then GetObject's it for the LOGFONT; recognise it.
        if (!is_font && IS_STOCK_FONT(hobj))
            is_font = 1;
        // (#278 b579) Word's font builder (WINWORD seg196:0x779) calls
        // GetObject([0x3e36]=0, 0x32, &logfont) on a NULL handle expecting the
        // default/system font's LOGFONT. Real Windows would return 0 here, but Word
        // aborts app-init if it gets 0; the [0x3e36] slot that should hold a font
        // handle is 0 in our environment because the predecessor font-setup step is
        // not fully modelled. Treat GetObject(NULL, >=50) as a request for the
        // system LOGFONT so font-init succeeds (the sanctioned font fallback).
        if (!is_font && hobj == 0 && cb >= 50)
            is_font = 1;
        if (is_font) {
            // Win16 LOGFONT (50 bytes): lfHeight(2) lfWidth(2) lfEscapement(2)
            //   lfOrientation(2) lfWeight(2) lfItalic(1) lfUnderline(1) lfStrikeOut(1)
            //   lfCharSet(1) lfOutPrecision(1) lfClipPrecision(1) lfQuality(1)
            //   lfPitchAndFamily(1) lfFaceName(32).
            uint8_t lf[50];
            for (int i = 0; i < 50; i++) lf[i] = 0;
            lf[0]=(uint8_t)(FONT_HEIGHT & 0xFF); lf[1]=(uint8_t)(FONT_HEIGHT >> 8);  // lfHeight
            lf[2]=(uint8_t)(FONT_WIDTH & 0xFF);  lf[3]=(uint8_t)(FONT_WIDTH >> 8);   // lfWidth
            lf[8]=(uint8_t)(400 & 0xFF); lf[9]=(uint8_t)(400 >> 8);                  // lfWeight=normal
            lf[13]=0;     // lfCharSet = ANSI_CHARSET (0)
            lf[17]=0x31;  // lfPitchAndFamily = FIXED_PITCH(1) | FF_MODERN(0x30)
            const char *face = "System";                                            // lfFaceName @ offset 18
            for (int i = 0; face[i] && i < 31; i++) lf[18+i] = (uint8_t)face[i];
            int n = (cb < 50) ? cb : 50;
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++)
                x86_16_wr8(c, lpv_seg, (uint16_t)(lpv_off + i), lf[i]);
            *ax = (uint16_t)n;
            return;
        }
    }
    if (hobj >= WIN16_MAX_GDIOBJ || !g_gdiobj[hobj].used || g_gdiobj[hobj].type != 4)
        return;                                 // only bitmaps supported
    int w = g_gdiobj[hobj].w, h = g_gdiobj[hobj].h;
    int bbp = g_gdiobj[hobj].bpp ? g_gdiobj[hobj].bpp : 8;
    int wbytes = ((w * bbp + 15) & ~15) / 8;    // word-aligned scanline
    // Win16 BITMAP: bmType(2) bmWidth(2) bmHeight(2) bmWidthBytes(2)
    //               bmPlanes(1) bmBitsPixel(1) bmBits(4) = 14 bytes
    uint8_t bm[14];
    bm[0]=0; bm[1]=0;                                   // bmType = 0
    bm[2]=(uint8_t)(w & 0xFF);  bm[3]=(uint8_t)(w >> 8);
    bm[4]=(uint8_t)(h & 0xFF);  bm[5]=(uint8_t)(h >> 8);
    bm[6]=(uint8_t)(wbytes&0xFF);bm[7]=(uint8_t)(wbytes>>8);
    bm[8]=1; bm[9]=(uint8_t)bbp;                        // planes=1, bitsPixel=actual
    bm[10]=0; bm[11]=0; bm[12]=0; bm[13]=0;             // bmBits far = NULL
    int n = (cb < 14) ? cb : 14;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++)
        x86_16_wr8(c, lpv_seg, (uint16_t)(lpv_off + i), bm[i]);
    *ax = (uint16_t)n;
}
// BitBlt (GDI.34): BitBlt(hdcDst,X,Y,W,H,hdcSrc,XSrc,YSrc,dwRop) =
//   1+1+1+1+1+1+1+1+2 = 10w = 20b. We do not model device bitmaps; the playfield
//   is redrawn each frame from primitives, so accept and balance the stack.
static void g_bitblt(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t rop = arg32(c, 0);          (void)rop;
    int ys = (int16_t)arg16(c, 2);
    int xs = (int16_t)arg16(c, 3);
    uint16_t hsrc = arg16(c, 4);
    int h  = (int16_t)arg16(c, 5);
    int w  = (int16_t)arg16(c, 6);
    int yd = (int16_t)arg16(c, 7);
    int xd = (int16_t)arg16(c, 8);
    uint16_t hdst = arg16(c, 9);
    win16_dc_t *dst = (hdst && hdst < WIN16_MAX_DC && g_dcs[hdst].used) ? &g_dcs[hdst] : 0;
    win16_dc_t *src = (hsrc && hsrc < WIN16_MAX_DC && g_dcs[hsrc].used) ? &g_dcs[hsrc] : 0;
    // (#word6 FIX) BitBlt coordinates are LOGICAL: map the destination corner
    // through the dst DC transform and the source corner through the src DC
    // transform, exactly as GDI does (and as the shape primitives already do here).
    // Word 6 reuses one 1-line memory bitmap for every document line by setting that
    // memory DC's viewport origin to -lineTop, so the blit reads logical y=lineTop
    // which must map to device y=0 in the buffer. Without this, lines past the first
    // read OUTSIDE the 16px buffer and copied solid black. dc_lp2dp is the identity
    // for MM_TEXT DCs (window blits, all games), so this is a no-op for them. Extents
    // are 1:1 for these transforms, so w/h are unchanged (use StretchBlt for scaling).
    if (dst) dc_lp2dp(dst, &xd, &yd);
    if (src) dc_lp2dp(src, &xs, &ys);
    win16_trace("BB d%d<-s%d @%d,%d %dx%d src%d,%d rop=%x dmem=%d smem=%d\n",
                hdst, hsrc, xd, yd, w, h, xs, ys, (unsigned)rop,
                dst ? (dst->membuf ? 1 : 0) : -1, src ? (src->membuf ? 1 : 0) : -1);
    if (g_w6log > 0) {
        uint32_t sp0 = (src && src->membuf && src->mw > 0) ? src->membuf[0] : 0xDEAD;
        W6LOG("[W6] BB dwin=%d d@%d,%d %dx%d <- s%d(mem=%d %dx%d) s@%d,%d rop=%06x srcpx0=%06x\n",
              dst ? dst->win : -99, xd, yd, w, h, hsrc, src ? (src->membuf ? 1 : 0) : -1,
              src ? src->mw : 0, src ? src->mh : 0, xs, ys, (unsigned)rop, sp0);
    }
    if (dst && src) blt_copy_rop(dst, xd, yd, w, h, src, xs, ys, rop);
    *ax = 1; *dx = 0; *argbytes = 20;
}
// StretchBlt (GDI.35): adds nSrcW,nSrcH (2 more words) = 12w = 24b.
static void g_stretchblt(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t rop = arg32(c, 0);          (void)rop;
    int sh = (int16_t)arg16(c, 2);
    int sw = (int16_t)arg16(c, 3);
    int ys = (int16_t)arg16(c, 4);
    int xs = (int16_t)arg16(c, 5);
    uint16_t hsrc = arg16(c, 6);
    int dh = (int16_t)arg16(c, 7);
    int dw = (int16_t)arg16(c, 8);
    int yd = (int16_t)arg16(c, 9);
    int xd = (int16_t)arg16(c, 10);
    uint16_t hdst = arg16(c, 11);
    win16_dc_t *dst = (hdst && hdst < WIN16_MAX_DC && g_dcs[hdst].used) ? &g_dcs[hdst] : 0;
    win16_dc_t *src = (hsrc && hsrc < WIN16_MAX_DC && g_dcs[hsrc].used) ? &g_dcs[hsrc] : 0;
    win16_trace("SB d%d<-s%d @%d,%d %dx%d src%d,%d %dx%d dmem=%d smem=%d\n",
                hdst, hsrc, xd, yd, dw, dh, xs, ys, sw, sh,
                dst ? (dst->membuf ? 1 : 0) : -1, src ? (src->membuf ? 1 : 0) : -1);
    if (dst && src && dw > 0 && dh > 0 && sw > 0 && sh > 0) {
        int simple = (rop == 0x00CC0020UL);
        for (int yy = 0; yy < dh; yy++)
            for (int xx = 0; xx < dw; xx++) {
                int sxp = xs + (xx * sw) / dw;
                int syp = ys + (yy * sh) / dh;
                uint32_t s = blt_get_pixel(src, sxp, syp);
                uint32_t v = simple ? s
                           : rop_combine(rop, s, blt_get_pixel(dst, xd + xx, yd + yy));
                dc_plot(dst, xd + xx, yd + yy, v);
            }
    }
    *ax = 1; *dx = 0; *argbytes = 24;
}
// GetTextExtent (GDI.91): GetTextExtent(hdc, lpString far, nCount) = 4w = 8b.
// Returns DWORD: LOWORD=width, HIWORD=height (bitmap font metrics).
static void g_gettextextent(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t n = arg16(c, 0);
    *ax = (uint16_t)(n * FONT_WIDTH); *dx = FONT_HEIGHT; *argbytes = 8;
}
// (#188) GetTextExtentPoint (GDI.471): GetTextExtentPoint(HDC, LPCSTR, int cb,
//   LPSIZE) = 1+2+1+2 = 6 words = 12 bytes. Fills the SIZE{cx,cy} (two 16-bit
//   words) at lpSize with the text dimensions (bitmap font metrics) and returns
//   TRUE. MS Golf's window-init code calls this 8x before drawing.
static void g_gettextextentpoint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t sz_off = arg16(c, 0), sz_seg = arg16(c, 1);
    int16_t  cb     = (int16_t)arg16(c, 2);
    /* lpString = arg16(3)/arg16(4); hdc = arg16(5) */
    *argbytes = 12;
    uint16_t w = (uint16_t)((cb > 0 ? cb : 0) * FONT_WIDTH);
    if (sz_seg || sz_off) {
        x86_16_wr16(c, sz_seg, sz_off, w);                     // SIZE.cx
        x86_16_wr16(c, sz_seg, (uint16_t)(sz_off + 2), FONT_HEIGHT);  // SIZE.cy
    }
    *ax = 1; *dx = 0;
}
// Ellipse (GDI.24): Ellipse(hdc,l,t,r,b) = 5w = 10b. Approximate with a filled
// rectangle inset (good enough that the well/ball shows as a blob).
static void g_ellipse(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int b=(int16_t)arg16(c,0), r=(int16_t)arg16(c,1), t=(int16_t)arg16(c,2), l=(int16_t)arg16(c,3);
    uint16_t hdc=arg16(c,4);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc=&g_dcs[hdc];
        dc_lp2dp(dc, &l, &t); dc_lp2dp(dc, &r, &b);
        if (r < l) { int tmp=l; l=r; r=tmp; }
        if (b < t) { int tmp=t; t=b; b=tmp; }
        int cx=(l+r)/2, cy=(t+b)/2, rx=(r-l)/2, ry=(b-t)/2;
        if (rx<1) rx=1;
        if (ry<1) ry=1;
        for (int yy=t; yy<b; yy++) for (int xx=l; xx<r; xx++) {
            int ddx=xx-cx, ddy=yy-cy;
            if ((long)ddx*ddx*ry*ry + (long)ddy*ddy*rx*rx <= (long)rx*rx*ry*ry) {
                if (!dc->brush_null) dc_plot(dc, xx, yy, dc->brush_color);
            }
        }
    }
    *ax=1; *dx=0; *argbytes=10;
}
// RoundRect (GDI.28): RoundRect(hdc,l,t,r,b,w,h) = 7w = 14b. Treat as Rectangle.
static void g_roundrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* ellipse w,h */ (void)arg16(c,0); (void)arg16(c,1);
    int b=(int16_t)arg16(c,2), r=(int16_t)arg16(c,3), t=(int16_t)arg16(c,4), l=(int16_t)arg16(c,5);
    uint16_t hdc=arg16(c,6);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc=&g_dcs[hdc];
        if (!dc->brush_null)
            for (int yy=t; yy<b; yy++) for (int xx=l; xx<r; xx++) dc_plot(dc, xx, yy, dc->brush_color);
        for (int xx=l; xx<r; xx++){dc_plot(dc,xx,t,dc->pen_color);dc_plot(dc,xx,b-1,dc->pen_color);}
        for (int yy=t; yy<b; yy++){dc_plot(dc,l,yy,dc->pen_color);dc_plot(dc,r-1,yy,dc->pen_color);}
    }
    *ax=1; *dx=0; *argbytes=14;
}
// CreateBrushIndirect (GDI.50): (lpLogBrush far) = 2w = 4b. Read color at +2.
static void g_createbrushindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0), seg=arg16(c,1);
    uint32_t cr = (uint32_t)x86_16_rd16(c, seg, (uint16_t)(off+2)) |
                  ((uint32_t)x86_16_rd16(c, seg, (uint16_t)(off+4)) << 16);
    *ax = (uint16_t)win16_alloc_gdiobj(1, cr); *dx=0; *argbytes=4;
}

// (#278 Word6) GDI object creators Word needs during window paint. They return
// valid GDI object handles (region=type 5) so SelectObject/DeleteObject accept
// them; precise geometry is not required to reach the message loop / first paint.
static void g_createpatternbrush(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)win16_alloc_gdiobj(1, 0xC0C0C0); *dx = 0; *argbytes = 2;  // GDI.60 CreatePatternBrush(HBITMAP)
}
static void g_createpenindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0), seg=arg16(c,1);                                          // GDI.62 CreatePenIndirect(LPLOGPEN)
    uint32_t cr = (uint32_t)x86_16_rd16(c, seg, (uint16_t)(off+6)) |
                  ((uint32_t)x86_16_rd16(c, seg, (uint16_t)(off+8)) << 16);           // LOGPEN: style(2) width(POINT=4) color@+6
    *ax = (uint16_t)win16_alloc_gdiobj(2, cr); *dx = 0; *argbytes = 4;
}
static void g_createpolygonrgn(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)win16_alloc_gdiobj(5, 0); *dx = 0; *argbytes = 8;        // GDI.63 CreatePolygonRgn(pts,count,mode)
}
static void g_createrectrgn(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)win16_alloc_gdiobj(5, 0); *dx = 0; *argbytes = 8;        // GDI.64 CreateRectRgn(x1,y1,x2,y2)
}
static void g_createrectrgnindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)win16_alloc_gdiobj(5, 0); *dx = 0; *argbytes = 4;        // GDI.65 CreateRectRgnIndirect(LPRECT)
}
// CreateFont/CreateFontIndirect (GDI.56/57): return a pseudo font handle.
static void g_createfont(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = win16_alloc_gdiobj(5, 0); if (*ax==0) *ax=0x400; *dx=0; *argbytes=30;
}
static void g_createfontindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = win16_alloc_gdiobj(5, 0); if (*ax==0) *ax=0x400; *dx=0; *argbytes=4;
}

// GetNearestColor (GDI.154): GetNearestColor(hdc, crColor long) = 1+2 = 3w = 6b.
// True-colour framebuffer -> the nearest colour IS the colour. Echo it back.
static void g_getnearestcolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);
    *ax = (uint16_t)(cr & 0xFFFF); *dx = (uint16_t)(cr >> 16); *argbytes = 6;
}

// CreateDIBitmap (GDI.442): CreateDIBitmap(hdc, lpInfoHeader far, dwUsage long,
//   lpInit far, lpInitInfo far, wUsage) = 1+2+2+2+2+1 = 10 words = 20 bytes
//   (wine gdi.exe16.spec: pascal CreateDIBitmap(word ptr long ptr ptr word)).
//   Word's font/UI init calls this; UNIMPL popped argbytes=0 = a 20-byte Pascal
//   stack DESYNC that corrupted the following font-table calls. Return a GDI bitmap
//   handle (we do not actually decode the DIB; the metrics path only needs a valid
//   non-null handle + a balanced stack). argbytes MUST be 20.
static void g_createdibitmap(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = win16_alloc_gdiobj(4, 0);   // type 4 = bitmap
    if (h && h < WIN16_MAX_GDIOBJ) { g_gdiobj[h].w = 1; g_gdiobj[h].h = 1; g_gdiobj[h].bpp = 8; }
    *ax = h ? h : 0x420; *dx = 0; *argbytes = 20;
    (void)c;
}

// GetCharWidth (GDI.350): GetCharWidth(hdc, wFirstChar, wLastChar, lpBuffer far)
//   = word + word + word + far ptr = 5 words = 10 bytes. Real Win16 ordinal 350
//   (wine gdi.exe16.spec: pascal GetCharWidth(word word word ptr)). Word's font
//   builder (WINWORD seg196:0x63e) calls it as GetCharWidth(hdc,0x38,0x38,lpBuf).
//   Fill (wLast-wFirst+1) 16-bit widths in lpBuffer with the fixed system glyph
//   width; return TRUE. Previously UNIMPL -> argbytes=0 -> Pascal stack desync
//   (the recurring font-init failure class).
static void g_getcharwidth(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t buf_off = arg16(c, 0);
    uint16_t buf_seg = arg16(c, 1);
    uint16_t last    = arg16(c, 2);
    uint16_t first   = arg16(c, 3);
    // uint16_t hdc  = arg16(c, 4);  // single fixed system font, hdc ignored
    if (buf_seg && last >= first) {
        unsigned n = (unsigned)(last - first) + 1;
        if (n > 256) n = 256;
        for (unsigned i = 0; i < n; i++)
            x86_16_wr16(c, buf_seg, (uint16_t)(buf_off + i*2), FONT_WIDTH);
    }
    *ax = 1; *dx = 0; *argbytes = 10;
}

// EnumFonts (GDI.70): EnumFonts(hdc, lpFaceName far, lpEnumProc far, lpData long)
//   = word + far + far + long = 7 words = 14 bytes. Real Win16 ordinal 70
//   (wine gdi.exe16.spec: pascal EnumFonts(word str segptr long)). Confirmed by
//   disassembling WINWORD seg196:0x83a and seg163:0xc73 (both push 7 words).
//   We have no enumerable .FON table to walk, so we do NOT invoke the 16-bit
//   callback (calling back into Ring-3 16-bit code mid-dispatch is unsafe and the
//   call sites tolerate a zero enumeration: seg196 forces a default when nothing
//   set its flag). Return 1 (success, callback's nominal "continue" value) and
//   balance the Pascal stack with the correct 14-byte cleanup. Previously UNIMPL
//   -> argbytes=0 -> stack desync = font-init failure.
static void g_enumfonts(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 14;
}


// ===========================================================================
// Dispatch table. Matched by (module, ordinal) for ordinal imports, and by
// (module, NAME) for named imports. Module match is case-insensitive.
// ===========================================================================
typedef struct {
    const char      *module;
    uint16_t         ordinal;     // 0 = match by name only
    const char      *name;        // 0 = match by ordinal only
    win16_handler_fn fn;
} win16_api_entry_t;

// ===========================================================================
// USER: dialog-box subsystem (#191). Modal DialogBox that loads an RT_DIALOG
// template, draws a centered dialog (caption + static text + buttons) onto the
// canvas, sends WM_INITDIALOG to the dialog proc, runs a nested modal loop
// (mouse + ESC/Enter), and returns the EndDialog result. Defensive: if the
// template is missing or parses badly it still shows a captioned box with an OK
// button and never blanks the window. Partial control support: STATIC text and
// BUTTON render + work; EDIT/LISTBOX are drawn as placeholders.
// ===========================================================================
#define WM_INITDIALOG  0x0110
#ifndef IDOK
#define IDOK     1
#define IDCANCEL 2
#endif
#define DS_SETFONT 0x40

#define WIN16_DLG_MAXITEMS 64
// Predefined control classes (DLGITEMTEMPLATE atom bytes) and BUTTON sub-styles.
#define DLGC_BUTTON   0x80
#define DLGC_EDIT     0x81
#define DLGC_STATIC   0x82
#define DLGC_LISTBOX  0x83
#define DLGC_SCROLL   0x84
#define DLGC_COMBO    0x85
// BUTTON low-nibble styles (BS_*).
#define BS_PUSHBUTTON      0
#define BS_DEFPUSHBUTTON   1
#define BS_CHECKBOX        2
#define BS_AUTOCHECKBOX    3
#define BS_RADIOBUTTON     4
#define BS_3STATE          5
#define BS_AUTO3STATE      6
#define BS_GROUPBOX        7
#define BS_AUTORADIOBUTTON 9
typedef struct {
    uint8_t  cls;        // DLGC_* control class
    uint8_t  btype;      // BUTTON low-nibble style (BS_*) when cls==DLGC_BUTTON
    uint8_t  checked;    // check/radio state
    uint8_t  is_button;  // 1 = push/defpush button (clickable, dismisses via id)
    uint16_t id;
    uint32_t style;      // full control style dword
    int   x, y, w, h;    // canvas-relative pixel rect
    char  text[64];
} win16_dlgitem_t;
// Control class predicates.
static int dlg_is_check(const win16_dlgitem_t *it){ return it->cls==DLGC_BUTTON && (it->btype==BS_CHECKBOX||it->btype==BS_AUTOCHECKBOX||it->btype==BS_3STATE||it->btype==BS_AUTO3STATE); }
static int dlg_is_radio(const win16_dlgitem_t *it){ return it->cls==DLGC_BUTTON && (it->btype==BS_RADIOBUTTON||it->btype==BS_AUTORADIOBUTTON); }
static int dlg_is_group(const win16_dlgitem_t *it){ return it->cls==DLGC_BUTTON && it->btype==BS_GROUPBOX; }

static int      g_dlg_active   = 0;      // a modal dialog is running
static int      g_dlg_end      = 0;      // EndDialog called
static int      g_dlg_result   = 0;      // EndDialog result
static uint16_t g_dlg_hwnd     = 0xD000; // synthetic dialog handle
static win16_dlgitem_t g_dlg_items[WIN16_DLG_MAXITEMS];
static int      g_dlg_nitems   = 0;
static int      g_dlg_x, g_dlg_y, g_dlg_w, g_dlg_h;  // dialog rect (canvas px)
static char     g_dlg_caption[64];

// Case-insensitive equality for short class names ("BUTTON"/"STATIC").
static int dlg_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// Draw a string with the 8x16 bitmap font at (tx,ty) in canvas coords.
static void dlg_text(int tx, int ty, const char *s, uint32_t ink) {
    for (const char *p = s; *p; p++) {
        if (*p == '&') continue;
        const uint8_t *g = font_get_glyph((unsigned char)*p);
        if (g) for (int row = 0; row < FONT_HEIGHT && row < 16; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) canvas_plot(tx + col, ty + row, ink);
        }
        tx += FONT_WIDTH;
    }
}
static int dlg_text_w(const char *s) {
    int w = 0; for (const char *p = s; *p; p++) if (*p != '&') w += FONT_WIDTH; return w;
}

// (#393b) Draw a predefined BUTTON control child's chrome into the host canvas.
// win->cx/cy/cw/ch are the button's client rect in canvas coords (computed by
// win16_draw_frame). Renders the standard Win16 3D pushbutton (raised bevel +
// centred label, or sunken when pressed), or a checkbox/radio glyph + label.
static void win16_draw_button(win16_window_t *win) {
    if (!g_win16_canvas || win->cw <= 0 || win->ch <= 0) return;
    int bx = win->cx, by = win->cy, bw = win->cw, bh = win->ch;
    uint32_t face = FB_COLOR(192,192,192);
    uint32_t hi   = FB_COLOR(255,255,255);
    uint32_t sh   = FB_COLOR(110,110,110);
    uint32_t ink  = FB_COLOR(0,0,0);
    int style = win->btn_style;
    int is_check = (style==BS_CHECKBOX||style==BS_AUTOCHECKBOX||style==BS_3STATE||style==BS_AUTO3STATE);
    int is_radio = (style==BS_RADIOBUTTON||style==BS_AUTORADIOBUTTON);
    int is_group = (style==BS_GROUPBOX);
    if (is_group) {   // etched frame + label, no fill
        int gy = by + FONT_HEIGHT/2;
        for (int x = 0; x < bw; x++) { canvas_plot(bx+x, gy, sh); canvas_plot(bx+x, by+bh-1, sh); }
        for (int y = 0; y < bh - FONT_HEIGHT/2; y++) { canvas_plot(bx, gy+y, sh); canvas_plot(bx+bw-1, gy+y, sh); }
        if (win->title[0]) { int tw = dlg_text_w(win->title);
            canvas_fill(bx+8, by, tw+6, FONT_HEIGHT, face); dlg_text(bx+11, by, win->title, ink); }
        return;
    }
    if (is_check || is_radio) {   // glyph + label on the parent's face colour
        canvas_fill(bx, by, bw, bh, face);
        int bs = 13, gy = by + (bh - bs)/2; if (gy < by) gy = by;
        uint32_t white = FB_COLOR(255,255,255);
        canvas_fill(bx, gy, bs, bs, white);
        for (int x = 0; x < bs; x++) { canvas_plot(bx+x, gy, sh); canvas_plot(bx+x, gy+bs-1, hi); }
        for (int y = 0; y < bs; y++) { canvas_plot(bx, gy+y, sh); canvas_plot(bx+bs-1, gy+y, hi); }
        if (win->btn_check) {
            if (is_radio) canvas_fill(bx+4, gy+4, bs-8, bs-8, ink);
            else for (int k = 0; k < bs-6; k++) { canvas_plot(bx+3+k, gy+3+k, ink); canvas_plot(bx+(bs-4)-k, gy+3+k, ink); }
        }
        if (win->title[0]) dlg_text(bx+bs+5, by + (bh-FONT_HEIGHT)/2, win->title, ink);
        return;
    }
    // Push / default push button.
    int p = win->btn_pressed ? 1 : 0;
    canvas_fill(bx, by, bw, bh, face);
    uint32_t tl = p ? sh : hi, br = p ? hi : sh;
    for (int x = 0; x < bw; x++) { canvas_plot(bx+x, by, tl); canvas_plot(bx+x, by+bh-1, br); }
    for (int y = 0; y < bh; y++) { canvas_plot(bx, by+y, tl); canvas_plot(bx+bw-1, by+y, br); }
    if (style == BS_DEFPUSHBUTTON) {   // heavy default outline
        for (int x = 0; x < bw; x++) { canvas_plot(bx+x, by+1, ink); canvas_plot(bx+x, by+bh-2, ink); }
        for (int y = 0; y < bh; y++) { canvas_plot(bx+1, by+y, ink); canvas_plot(bx+bw-2, by+y, ink); }
    }
    if (win->title[0]) {
        int tw = dlg_text_w(win->title);
        int tx = bx + (bw - tw)/2 + p, ty = by + (bh - FONT_HEIGHT)/2 + p;
        if (tx < bx+2) tx = bx+2;
        dlg_text(tx, ty, win->title, ink);
    }
}

// (#393b) Native predefined-BUTTON class proc, emulating USER.EXE's BUTTONWNDPROC
// for a child of class "BUTTON" that the app created with no wndproc of its own.
// Returns 1 (message consumed) with *out set, or 0 to fall through to the generic
// dispatch. Standard behaviour: capture on WM_LBUTTONDOWN + draw sunken; on
// WM_LBUTTONUP inside the client, release + draw raised, toggle auto check/radio
// state, and SEND WM_COMMAND(id, MAKELONG(hwndCtl, BN_CLICKED)) to the PARENT.
static int win16_native_ctrl_proc(win16_window_t *win, uint16_t msg,
                                  uint16_t wParam, uint32_t lParam, uint32_t *out) {
    (void)wParam;
    *out = 0;
    int style = win->btn_style;
    int autotoggle = (style==BS_AUTOCHECKBOX||style==BS_AUTORADIOBUTTON||style==BS_AUTO3STATE);
    switch (msg) {
        case WM_PAINT:
            win16_draw_button(win);
            return 1;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            win->btn_pressed = 1;
            g_win16_capture_hwnd = win->hwnd;   // SetCapture(hwnd)
            win16_draw_button(win);
            if (g_w16_mousepath)
                kprintf("[BTN] hwnd=%04x id=%u DOWN pressed=1 capture set\n", win->hwnd, win->ctrl_id);
            return 1;
        case WM_LBUTTONUP: {
            int was = win->btn_pressed;
            win->btn_pressed = 0;
            if (g_win16_capture_hwnd == win->hwnd) g_win16_capture_hwnd = 0;  // ReleaseCapture
            int clx = (int16_t)(lParam & 0xFFFF), cly = (int16_t)(lParam >> 16);
            int inside = (clx >= 0 && clx < win->cw && cly >= 0 && cly < win->ch);
            win16_draw_button(win);
            if (was && inside) {
                if (autotoggle) {
                    if (style==BS_AUTORADIOBUTTON) win->btn_check = 1;   // (group logic omitted)
                    else win->btn_check = (uint8_t)(win->btn_check ? 0 : 1);
                    win16_draw_button(win);
                }
                // WM_COMMAND to the PARENT: wParam=id, lParam=MAKELONG(hwndCtl, BN_CLICKED=0)
                uint16_t parent = win->parent;
                uint32_t clp = (uint32_t)win->hwnd;   // BN_CLICKED (0) in the hi word
                win16_window_t *pw = win_from_hwnd(parent);
                if (g_w16_mousepath)
                    kprintf("[BTN] hwnd=%04x id=%u UP inside=1 -> WM_COMMAND to parent=%04x proc=%04x:%04x\n",
                            win->hwnd, win->ctrl_id, parent, pw?pw->proc_seg:0, pw?pw->proc_off:0);
                if (pw && (pw->proc_seg || pw->proc_off))
                    win16_call_wndproc(pw->proc_seg, pw->proc_off, parent, WM_COMMAND, win->ctrl_id, clp);
                else
                    msgq_post(parent, WM_COMMAND, win->ctrl_id, clp);
            } else if (g_w16_mousepath) {
                kprintf("[BTN] hwnd=%04x id=%u UP inside=%d (no click)\n", win->hwnd, win->ctrl_id, inside);
            }
            return 1;
        }
        default:
            return 0;   // let the generic path handle other messages
    }
}

// Skip a 16-bit sz_or_ord (menu/class field): 0x00 empty, 0xFF + WORD ordinal,
// else NUL-terminated string. Returns the offset after it (bounded by len).
static uint32_t dlg_skip_szord(const uint8_t *t, uint32_t off, uint32_t len) {
    if (off >= len) return len;
    if (t[off] == 0x00) return off + 1;
    if (t[off] == 0xFF) return off + 3;
    while (off < len && t[off]) off++;
    return (off < len) ? off + 1 : len;
}
// Copy a NUL-terminated string at off into dst; return offset after the NUL.
static uint32_t dlg_read_str(const uint8_t *t, uint32_t off, uint32_t len,
                             char *dst, int cap) {
    int i = 0;
    while (off < len && t[off]) { if (i < cap - 1) dst[i++] = (char)t[off]; off++; }
    dst[i] = 0;
    return (off < len) ? off + 1 : len;
}

// Parse the RT_DIALOG template into g_dlg_* (caption, rect, items). Returns 1 on
// a usable parse, 0 to fall back to a default captioned OK box.
static int dlg_parse_template(const uint8_t *t, uint32_t len) {
    g_dlg_nitems = 0; g_dlg_caption[0] = 0;
    if (!t || len < 14) return 0;
    uint32_t style = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                     ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    int cdit = t[4];
    int dcx  = (int)(t[9]  | (t[10] << 8));
    int dcy  = (int)(t[11] | (t[12] << 8));
    uint32_t off = 13;
    off = dlg_skip_szord(t, off, len);     // menu
    off = dlg_skip_szord(t, off, len);     // class
    off = dlg_read_str(t, off, len, g_dlg_caption, sizeof(g_dlg_caption)); // title
    if (style & DS_SETFONT) {
        if (off + 2 <= len) off += 2;       // point size WORD
        char fn[40]; off = dlg_read_str(t, off, len, fn, sizeof(fn)); // font name
    }
    // Dialog pixel rect: dialog units -> pixels (~2x for our 8x16 font). Center.
    // Dialog units -> pixels: our base units are ~8x16, so x*2 and y*2. The box is
    // client (cdx*2 by cdy*2) plus a 20px title bar.
    int titleh = 20;
    int cw = dcx * 2; if (cw < 200) cw = 200;
    int chh = dcy * 2; if (chh < 70) chh = 70;
    int pw = cw, ph = chh + titleh;
    if (pw > g_win16_canvas_w - 20) pw = g_win16_canvas_w - 20;
    if (ph > g_win16_canvas_h - 20) ph = g_win16_canvas_h - 20;
    g_dlg_w = pw; g_dlg_h = ph;
    g_dlg_x = (g_win16_canvas_w - pw) / 2; if (g_dlg_x < 4) g_dlg_x = 4;
    g_dlg_y = (g_win16_canvas_h - ph) / 2; if (g_dlg_y < 4) g_dlg_y = 4;
    int cx0 = g_dlg_x, cy0 = g_dlg_y + titleh;   // dialog client origin (px)
    // DLGITEMTEMPLATE: x,y,cx,cy,id (5 WORD) + style (DWORD) + class(sz_or_ord) +
    // text(sz_or_ord) + cbCreateParams(BYTE)+data. (The old code advanced 12 not 14
    // and so read the class from inside the style dword -> every control became a
    // button; that is the "incomplete" dialog the user saw.)
    for (int i = 0; i < cdit; i++) {
        if (off + 14 > len) break;
        int ix = (int)(int16_t)(t[off]   | (t[off+1] << 8));
        int iy = (int)(int16_t)(t[off+2] | (t[off+3] << 8));
        int iw = (int)(t[off+4] | (t[off+5] << 8));
        int ih = (int)(t[off+6] | (t[off+7] << 8));
        uint16_t id = (uint16_t)(t[off+8] | (t[off+9] << 8));
        uint32_t st = (uint32_t)t[off+10] | ((uint32_t)t[off+11] << 8) |
                      ((uint32_t)t[off+12] << 16) | ((uint32_t)t[off+13] << 24);
        off += 14;
        uint8_t cls = DLGC_STATIC;
        if (off < len && t[off] >= 0x80 && t[off] <= 0x85) { cls = t[off]; off++; }
        else { char cn[24]; off = dlg_read_str(t, off, len, cn, sizeof(cn));
               if (dlg_ci_eq(cn, "BUTTON")) cls = DLGC_BUTTON;
               else if (dlg_ci_eq(cn, "EDIT")) cls = DLGC_EDIT;
               else if (dlg_ci_eq(cn, "STATIC")) cls = DLGC_STATIC;
               else if (dlg_ci_eq(cn, "LISTBOX")) cls = DLGC_LISTBOX;
               else if (dlg_ci_eq(cn, "SCROLLBAR")) cls = DLGC_SCROLL;
               else if (dlg_ci_eq(cn, "COMBOBOX")) cls = DLGC_COMBO; }
        char txt[64]; txt[0] = 0;
        if (off < len && t[off] == 0xFF) off += 3;          // ordinal text id
        else off = dlg_read_str(t, off, len, txt, sizeof(txt));
        if (off < len) { int cb = t[off]; off += 1 + cb; }  // cbCreateParams
        if (g_dlg_nitems >= WIN16_DLG_MAXITEMS) continue;
        win16_dlgitem_t *it = &g_dlg_items[g_dlg_nitems++];
        it->cls   = cls;
        it->btype = (cls == DLGC_BUTTON) ? (uint8_t)(st & 0x0F) : 0;
        it->checked = 0;
        it->is_button = (cls == DLGC_BUTTON &&
                         (it->btype == BS_PUSHBUTTON || it->btype == BS_DEFPUSHBUTTON));
        it->id = id; it->style = st;
        it->x = cx0 + ix * 2;
        it->y = cy0 + iy * 2;
        it->w = iw * 2; if (it->w < 8)  it->w = dlg_text_w(txt) + 16;
        it->h = ih * 2; if (it->h < 10) it->h = 16;
        int n = 0; for (const char *p = txt; *p && n < 63; p++) it->text[n++] = *p;
        it->text[n] = 0;
    }
    return 1;
}

// Ensure the dialog has at least one button to dismiss it (default OK).
static void dlg_ensure_button(void) {
    for (int i = 0; i < g_dlg_nitems; i++) if (g_dlg_items[i].is_button) return;
    if (g_dlg_nitems >= WIN16_DLG_MAXITEMS) g_dlg_nitems = WIN16_DLG_MAXITEMS - 1;
    win16_dlgitem_t *it = &g_dlg_items[g_dlg_nitems++];
    it->is_button = 1; it->id = IDOK;
    it->w = 64; it->h = 22;
    it->x = g_dlg_x + (g_dlg_w - it->w) / 2;
    it->y = g_dlg_y + g_dlg_h - 30;
    it->text[0] = 'O'; it->text[1] = 'K'; it->text[2] = 0;
}

// Draw the whole dialog onto the canvas (over the app content behind it).
static void dlg_draw(int hot_btn) {
    uint32_t face = FB_COLOR(212,208,200), ink = FB_COLOR(16,16,16);
    uint32_t hi = FB_COLOR(255,255,255), sh = FB_COLOR(110,110,110);
    uint32_t tcap = FB_COLOR(0,0,128), tcapink = FB_COLOR(255,255,255);
    canvas_fill(g_dlg_x, g_dlg_y, g_dlg_w, g_dlg_h, face);
    // outer border
    for (int x = 0; x < g_dlg_w; x++) { canvas_plot(g_dlg_x+x, g_dlg_y, sh); canvas_plot(g_dlg_x+x, g_dlg_y+g_dlg_h-1, sh); }
    for (int y = 0; y < g_dlg_h; y++) { canvas_plot(g_dlg_x, g_dlg_y+y, sh); canvas_plot(g_dlg_x+g_dlg_w-1, g_dlg_y+y, sh); }
    // title bar
    canvas_fill(g_dlg_x+2, g_dlg_y+2, g_dlg_w-4, 18, tcap);
    dlg_text(g_dlg_x+6, g_dlg_y+3, g_dlg_caption[0] ? g_dlg_caption : "Dialog", tcapink);
    uint32_t white = FB_COLOR(255,255,255);
    // items - render each by control class (Win16-style 3D chrome)
    for (int i = 0; i < g_dlg_nitems; i++) {
        win16_dlgitem_t *it = &g_dlg_items[i];
        int cy = it->y + (it->h - FONT_HEIGHT)/2;
        if (it->is_button) {                                  // push / default button
            uint32_t bf = (i == hot_btn) ? FB_COLOR(225,225,225) : face;
            canvas_fill(it->x, it->y, it->w, it->h, bf);
            for (int x = 0; x < it->w; x++) { canvas_plot(it->x+x, it->y, hi); canvas_plot(it->x+x, it->y+it->h-1, sh); }
            for (int y = 0; y < it->h; y++) { canvas_plot(it->x, it->y+y, hi); canvas_plot(it->x+it->w-1, it->y+y, sh); }
            if (it->btype == BS_DEFPUSHBUTTON)               // default: extra outline
                for (int x = -2; x < it->w+2; x++) { canvas_plot(it->x+x, it->y-2, ink); canvas_plot(it->x+x, it->y+it->h+1, ink); }
            int tw = dlg_text_w(it->text);
            dlg_text(it->x + (it->w - tw)/2, cy, it->text, ink);
        } else if (dlg_is_check(it)) {                        // checkbox [ ] / [x]
            int bs = 13, by = it->y + (it->h - bs)/2;
            canvas_fill(it->x, by, bs, bs, white);
            for (int x = 0; x < bs; x++) { canvas_plot(it->x+x, by, sh); canvas_plot(it->x+x, by+bs-1, ink); }
            for (int y = 0; y < bs; y++) { canvas_plot(it->x, by+y, sh); canvas_plot(it->x+bs-1, by+y, ink); }
            if (it->checked)   // X mark
                for (int k = 0; k < bs-6; k++) {
                    canvas_plot(it->x+3+k, by+3+k, ink); canvas_plot(it->x+4+k, by+3+k, ink);
                    canvas_plot(it->x+(bs-4)-k, by+3+k, ink); canvas_plot(it->x+(bs-5)-k, by+3+k, ink);
                }
            dlg_text(it->x + bs + 5, cy, it->text, ink);
        } else if (dlg_is_radio(it)) {                        // radio ( ) / (o)
            int bs = 12, by = it->y + (it->h - bs)/2;
            canvas_fill(it->x+2, by+2, bs-4, bs-4, white);
            for (int x = 1; x < bs-1; x++) { canvas_plot(it->x+x, by, ink); canvas_plot(it->x+x, by+bs-1, ink); }
            for (int y = 1; y < bs-1; y++) { canvas_plot(it->x, by+y, ink); canvas_plot(it->x+bs-1, by+y, ink); }
            if (it->checked) canvas_fill(it->x+4, by+4, bs-8, bs-8, ink);
            dlg_text(it->x + bs + 5, cy, it->text, ink);
        } else if (dlg_is_group(it)) {                        // group box (etched frame + label)
            int gy = it->y + 6;
            for (int x = 0; x < it->w; x++) { canvas_plot(it->x+x, gy, sh); canvas_plot(it->x+x, it->y+it->h-1, sh); }
            for (int y = 0; y < it->h-6; y++) { canvas_plot(it->x, gy+y, sh); canvas_plot(it->x+it->w-1, gy+y, sh); }
            int tw = dlg_text_w(it->text);
            canvas_fill(it->x+8, it->y, tw+6, FONT_HEIGHT, face);
            dlg_text(it->x + 11, it->y, it->text, ink);
        } else if (it->cls == DLGC_EDIT || it->cls == DLGC_LISTBOX || it->cls == DLGC_COMBO) {
            canvas_fill(it->x, it->y, it->w, it->h, white);   // sunken white field
            for (int x = 0; x < it->w; x++) { canvas_plot(it->x+x, it->y, sh); canvas_plot(it->x+x, it->y+it->h-1, hi); }
            for (int y = 0; y < it->h; y++) { canvas_plot(it->x, it->y+y, sh); canvas_plot(it->x+it->w-1, it->y+y, hi); }
            if (it->text[0]) dlg_text(it->x + 4, it->y + 2, it->text, ink);
        } else {                                              // STATIC / default text
            dlg_text(it->x, cy, it->text, ink);
        }
    }
}

// Force the app's top-level windows to repaint (erases the dialog/menu popup).
static void win16_repaint_all(void) {
    g_win16_no_bg_fill = 0;   // (#209) force a real bg-filled erase here
    win16_paint_all_z();      // (#207) z-ordered repaint
    g_win16_z_dirty = 1;      // (#209) ensure the next idle frame settles cleanly
}

// Launch parameters for the shared modal core, set by each DialogBox* wrapper.
static uint16_t g_dlg_dp_seg, g_dlg_dp_off, g_dlg_tmpl_seg, g_dlg_tmpl_off, g_dlg_hinst;
static uint32_t g_dlg_init_param;

// Shared modal dialog core. Parses the RT_DIALOG template from g_dlg_*, draws it,
// sends WM_INITDIALOG (lParam = g_dlg_init_param), runs the nested modal loop, and
// returns the EndDialog result. Used by DialogBox/DialogBoxParam/CreateDialog*.
static int dlg_modal_core(x86_16_cpu_t *c) {
    uint16_t dp_off = g_dlg_dp_off, dp_seg = g_dlg_dp_seg;
    uint16_t tmpl_off = g_dlg_tmpl_off, tmpl_seg = g_dlg_tmpl_seg;
    uint16_t hinst = g_dlg_hinst;

    const uint8_t *t = 0; uint32_t tlen = 0;
    if (tmpl_seg == 0) t = win16_get_resource(hinst, 5, tmpl_off, &tlen);
    else { char nm[64]; rd_far_cstr(c, tmpl_seg, tmpl_off, nm, sizeof(nm));
           t = win16_get_resource_by_name(hinst, 5, nm, &tlen); }
    if (!t) t = win16_get_resource_first(hinst, 5, &tlen);

    if (!t || !g_win16_canvas) {
        // Guard: no template / no canvas -> safe IDCANCEL, do NOT blank.
        win16_trace("DialogBox: no template (hinst=%04x) -> IDCANCEL\n", hinst);
        return IDCANCEL;
    }
    if (!dlg_parse_template(t, tlen)) { g_dlg_caption[0]=0; g_dlg_nitems=0;
        g_dlg_w=260; g_dlg_h=140; g_dlg_x=(g_win16_canvas_w-260)/2; g_dlg_y=(g_win16_canvas_h-140)/2; }
    dlg_ensure_button();
    win16_trace("DialogBox '%s' items=%d\n", g_dlg_caption, g_dlg_nitems);

    g_dlg_active = 1; g_dlg_end = 0; g_dlg_result = 0;
    dlg_draw(-1);
    // WM_INITDIALOG to the dialog proc (lParam = dwInitParam from *Param variants).
    win16_call_wndproc(dp_seg, dp_off, g_dlg_hwnd, WM_INITDIALOG, g_dlg_hwnd, g_dlg_init_param);

    // Nested modal loop.
    uint8_t prevb = mouse_buttons;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t cap = (uint64_t)hz * 180, start = timer_ticks, lastdraw = 0;
    while (!g_dlg_end) {
        // (#210) Auto-dismiss the TETRIS startup splash. TETRIS shows an "All
        // Rights Reserved" copyright DialogBox at WinMain time, BEFORE its main
        // message loop runs, and it stays up until the user presses a key/clicks.
        // That blocks the autostart (which lives in the app's GetMessage, never
        // reached during this nested modal loop). For a TETRIS-kind app whose
        // game has not yet been started (autostart not finished), close the
        // splash automatically after a short visible delay (~1.5s) by routing
        // IDOK through the dialog proc, exactly as an Enter/click would. Dialogs
        // shown AFTER the game starts (High Scores, Skill, etc.) are left alone.
        if (g_win16_app_kind == 1 && g_autostart_done < 2 &&
            timer_ticks - start >= (hz * 3 / 2)) {
            win16_call_wndproc(dp_seg, dp_off, g_dlg_hwnd, WM_COMMAND, 1 /*IDOK*/, 0);
            if (!g_dlg_end) { g_dlg_end = 1; g_dlg_result = IDOK; }
            kprintf("[TETAS] auto-dismiss startup splash '%s'\n", g_dlg_caption);
            break;
        }
        // Mouse: detect a left click and hit-test buttons.
        int ox=0, oy=0, ow=0, oh=0;
        if (win16_host_content_rect(g_win16_host_slot, &ox, &oy, &ow, &oh) == 0) {
            int cvx = (int)mouse_x - ox, cvy = (int)mouse_y - oy;
            uint8_t b = mouse_buttons;
            int ldown = (b & 1) && !(prevb & 1);
            int hot = -1, hit = -1;
            for (int i = 0; i < g_dlg_nitems; i++) {
                win16_dlgitem_t *it = &g_dlg_items[i];
                int interactive = it->is_button || dlg_is_check(it) || dlg_is_radio(it);
                if (interactive && cvx>=it->x && cvx<it->x+it->w &&
                    cvy>=it->y && cvy<it->y+it->h) { hit = i; if (it->is_button) hot = i; break; }
            }
            if (ldown && hit >= 0) {
                win16_dlgitem_t *it = &g_dlg_items[hit];
                uint16_t id = it->id;
                if (dlg_is_check(it)) {                       // toggle (auto)checkbox
                    it->checked = !it->checked; dlg_draw(-1);
                } else if (dlg_is_radio(it)) {                // select within group
                    for (int j = 0; j < g_dlg_nitems; j++)
                        if (dlg_is_radio(&g_dlg_items[j])) g_dlg_items[j].checked = 0;
                    it->checked = 1; dlg_draw(-1);
                }
                win16_call_wndproc(dp_seg, dp_off, g_dlg_hwnd, WM_COMMAND, id, 0);
                if (!g_dlg_end && (id == IDOK || id == IDCANCEL)) { g_dlg_end = 1; g_dlg_result = id; }
            }
            prevb = b;
            if (timer_ticks - lastdraw >= (hz/30?hz/30:1)) { lastdraw = timer_ticks; dlg_draw(hot); }
        }
        // Keyboard: ESC -> cancel, Enter -> OK (route through the dialog proc).
        if (keyboard_has_char()) {
            int code = keyboard_get_char();
            if (code == 0x1B) {
                win16_call_wndproc(dp_seg, dp_off, g_dlg_hwnd, WM_COMMAND, IDCANCEL, 0);
                if (!g_dlg_end) { g_dlg_end = 1; g_dlg_result = IDCANCEL; }
            } else {
                int rel; char ch; uint16_t vk = kernel_key_to_vk(code, &rel, &ch);
                if (!rel && (ch == '\r' || ch == '\n' || vk == 0x0D)) {
                    win16_call_wndproc(dp_seg, dp_off, g_dlg_hwnd, WM_COMMAND, 1 /*IDOK*/, 0);
                    if (!g_dlg_end) { g_dlg_end = 1; g_dlg_result = IDOK; }
                }
            }
        }
        if (timer_ticks - start >= cap) { g_dlg_end = 1; g_dlg_result = IDCANCEL; break; }
        // (#205) Sleep between polls so a modal dialog box does not peg the CPU.
        // 8ms (~125 Hz) stays well under the 30 Hz redraw cadence above, so
        // button clicks and key presses remain responsive.
        extern void proc_sleep(uint32_t ms); proc_sleep(8);
    }

    g_dlg_active = 0;
    win16_repaint_all();    // erase the dialog
    return g_dlg_result;
}

// USER.87 DialogBox(hInst, lpTemplate, hWndParent, lpDlgProc) - modal.
static void u_dialogbox(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_dlg_dp_off = arg16(c, 0); g_dlg_dp_seg = arg16(c, 1);
    /* hWndParent */ (void)arg16(c, 2);
    g_dlg_tmpl_off = arg16(c, 3); g_dlg_tmpl_seg = arg16(c, 4);
    g_dlg_hinst = arg16(c, 5);
    g_dlg_init_param = 0;
    *ax = (uint16_t)dlg_modal_core(c); *dx = 0; *argbytes = 12;
}

// DialogBoxParam(hInst, lpTemplate, hWndParent, lpDlgProc, dwInitParam) - modal,
// passes dwInitParam to WM_INITDIALOG's lParam.
static void u_dialogboxparam(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_dlg_init_param = arg32(c, 0);
    g_dlg_dp_off = arg16(c, 2); g_dlg_dp_seg = arg16(c, 3);
    /* hWndParent */ (void)arg16(c, 4);
    g_dlg_tmpl_off = arg16(c, 5); g_dlg_tmpl_seg = arg16(c, 6);
    g_dlg_hinst = arg16(c, 7);
    *ax = (uint16_t)dlg_modal_core(c); *dx = 0; *argbytes = 16;
}

// DialogBoxIndirect / DialogBoxIndirectParam: template is an in-memory HGLOBAL
// rather than a resource. We do not resolve guest HGLOBAL->bytes here, so run a
// safe default captioned OK box (no template) and return its result. argbytes are
// honoured so the Pascal stack stays balanced.
static void u_dialogboxindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_dlg_dp_off = arg16(c, 0); g_dlg_dp_seg = arg16(c, 1);
    (void)arg16(c, 2); g_dlg_tmpl_off = 0; g_dlg_tmpl_seg = 0xFFFF; g_dlg_hinst = 0;
    g_dlg_init_param = 0;
    *ax = (uint16_t)dlg_modal_core(c); *dx = 0; *argbytes = 10;
}
static void u_dialogboxindirectparam(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    g_dlg_init_param = arg32(c, 0);
    g_dlg_dp_off = arg16(c, 2); g_dlg_dp_seg = arg16(c, 3);
    (void)arg16(c, 4); g_dlg_tmpl_off = 0; g_dlg_tmpl_seg = 0xFFFF; g_dlg_hinst = 0;
    *ax = (uint16_t)dlg_modal_core(c); *dx = 0; *argbytes = 14;
}

// USER.88 EndDialog(hDlg, nResult).
static void u_enddialog(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t res = arg16(c, 0); /* hDlg */ (void)arg16(c, 1);
    g_dlg_end = 1; g_dlg_result = (int)(int16_t)res;
    *ax = 1; *dx = 0; *argbytes = 4;
}

// USER.89 CreateDialog (modeless) - we run it like a modal DialogBox (MVP).
static void u_createdialog(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    u_dialogbox(c, ax, dx, argbytes);   // same args/argbytes; returns a result, not an hwnd
}

// USER.90 IsDialogMessage(hDlg, lpMsg). We do NOT own Word's (SDM) modeless
// dialog message loop, so the host app pumps keys through IsDialogMessage for
// dialog navigation. (#278 pass31) Translate Enter -> the dialog's default
// pushbutton (IDOK) and Esc -> IDCANCEL by sending WM_COMMAND to the dialog
// window proc, and return TRUE so the caller skips its own Translate/Dispatch.
// This completes default-button handling for all Win16 dialogs. For non
// Enter/Esc keys we still return FALSE so existing app pumps are undisturbed.
static void u_isdialogmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t msg_off = arg16(c, 0), msg_seg = arg16(c, 1);
    uint16_t hdlg    = arg16(c, 2);
    *argbytes = 6; *ax = 0; *dx = 0;
    uint16_t mmsg = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 2));
    uint16_t mwp  = x86_16_rd16(c, msg_seg, (uint16_t)(msg_off + 4));
    if (g_info.segcount >= 100 && (mmsg == WM_KEYDOWN || mmsg == WM_CHAR))
        kprintf("[W6IDM] hDlg=%04x msg=%04x wp=%04x\n", hdlg, mmsg, mwp);
    if (mmsg == WM_KEYDOWN && (mwp == 0x0D || mwp == 0x1B)) {
        win16_window_t *dw = win_from_hwnd(hdlg);
        uint16_t id = (mwp == 0x0D) ? (uint16_t)IDOK : (uint16_t)IDCANCEL;
        if (dw) {
            win16_call_wndproc(dw->proc_seg, dw->proc_off, hdlg, WM_COMMAND, id, 0);
            kprintf("[W6IDM] sent WM_COMMAND id=%u to hDlg=%04x\n",
                    (unsigned)id, hdlg);
        }
        *ax = 1;
    }
}

// Find a collected dialog item by control id.
static win16_dlgitem_t *dlg_find(uint16_t id) {
    for (int i = 0; i < g_dlg_nitems; i++) if (g_dlg_items[i].id == id) return &g_dlg_items[i];
    return 0;
}

// USER.91 GetDlgItem(hDlg, id) -> pseudo handle (id | 0xD000).
static void u_getdlgitem(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t id = arg16(c, 0); (void)arg16(c, 1);
    *ax = (uint16_t)(0xD000 | (id & 0xFFF)); *dx = 0; *argbytes = 4;
}

// USER.92 SetDlgItemText(hDlg, id, lpText).
static void u_setdlgitemtext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t t_off = arg16(c, 0), t_seg = arg16(c, 1);
    uint16_t id = arg16(c, 2); (void)arg16(c, 3);
    char buf[64]; rd_far_cstr(c, t_seg, t_off, buf, sizeof(buf));
    win16_dlgitem_t *it = dlg_find(id);
    if (it) { int n=0; for (const char*p=buf; *p && n<63; p++) it->text[n++]=*p; it->text[n]=0;
              if (g_dlg_active) dlg_draw(-1); }
    *ax = 1; *dx = 0; *argbytes = 8;
}

// USER.93 GetDlgItemText(hDlg, id, lpStr, cbMax) -> length copied.
static void u_getdlgitemtext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cb = arg16(c, 0);
    uint16_t s_off = arg16(c, 1), s_seg = arg16(c, 2);
    uint16_t id = arg16(c, 3); (void)arg16(c, 4);
    win16_dlgitem_t *it = dlg_find(id);
    int n = 0;
    if (it) { for (const char *p = it->text; *p && n < cb - 1; p++) { x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), (uint8_t)*p); n++; } }
    if (cb > 0) x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), 0);
    *ax = (uint16_t)n; *dx = 0; *argbytes = 10;
}

// USER.94 SetDlgItemInt(hDlg, id, val, bSigned).
static void u_setdlgitemint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t bSigned = arg16(c, 0); uint16_t val = arg16(c, 1);
    uint16_t id = arg16(c, 2); (void)arg16(c, 3);
    win16_dlgitem_t *it = dlg_find(id);
    if (it) { char b[16]; int v = bSigned ? (int)(int16_t)val : (int)val; int n=0;
              if (v<0){b[n++]='-';v=-v;} char tmp[12]; int m=0; do{tmp[m++]='0'+v%10;v/=10;}while(v); while(m)b[n++]=tmp[--m]; b[n]=0;
              int k=0; for(char*p=b;*p&&k<63;p++)it->text[k++]=*p; it->text[k]=0; if(g_dlg_active)dlg_draw(-1); }
    *ax = 1; *dx = 0; *argbytes = 8;
}

// USER.95 GetDlgItemInt(hDlg, id, lpTransOK, bSigned) -> parsed value.
static void u_getdlgitemint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t bSigned = arg16(c, 0);
    uint16_t ok_off = arg16(c, 1), ok_seg = arg16(c, 2);
    uint16_t id = arg16(c, 3); (void)arg16(c, 4);
    win16_dlgitem_t *it = dlg_find(id);
    int v = 0, neg = 0, got = 0;
    if (it) { const char *p = it->text; if (bSigned && *p=='-'){neg=1;p++;}
              while (*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;got=1;} if(neg)v=-v; }
    if (ok_seg || ok_off) { x86_16_wr16(c, ok_seg, ok_off, (uint16_t)(got?1:0)); }
    *ax = (uint16_t)v; *dx = 0; *argbytes = 10;
}

// SendDlgItemMessage(hDlg, id, msg, wParam, lParam) - route common control
// messages to the item state: BM_SETCHECK/BM_GETCHECK and WM_SETTEXT/WM_GETTEXT.
#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define WM_SETTEXT  0x000C
#define WM_GETTEXT  0x000D
static void u_senddlgitemmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t lParam = arg32(c, 0);
    uint16_t wParam = arg16(c, 2);
    uint16_t msg    = arg16(c, 3);
    uint16_t id     = arg16(c, 4);
    (void)arg16(c, 5);
    win16_dlgitem_t *it = dlg_find(id);
    uint16_t r = 0;
    if (it) {
        if (msg == BM_SETCHECK) { it->checked = (wParam != 0); if (g_dlg_active) dlg_draw(-1); }
        else if (msg == BM_GETCHECK) { r = it->checked ? 1 : 0; }
        else if (msg == WM_SETTEXT) {
            char buf[64]; rd_far_cstr(c, (uint16_t)(lParam >> 16), (uint16_t)lParam, buf, sizeof(buf));
            int n = 0; for (char *p = buf; *p && n < 63; p++) it->text[n++] = *p; it->text[n] = 0;
            if (g_dlg_active) dlg_draw(-1);
        } else if (msg == WM_GETTEXT) {
            uint16_t s_seg = (uint16_t)(lParam >> 16), s_off = (uint16_t)lParam; int n = 0;
            for (const char *p = it->text; *p && n < (int)wParam - 1; p++) { x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), (uint8_t)*p); n++; }
            if (wParam > 0) x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), 0);
            r = (uint16_t)n;
        }
    }
    *ax = r; *dx = 0; *argbytes = 12;
}

// CheckDlgButton(hDlg, id, uCheck) = 3w = 6b.
static void u_checkdlgbutton(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t chk = arg16(c, 0); uint16_t id = arg16(c, 1); (void)arg16(c, 2);
    win16_dlgitem_t *it = dlg_find(id);
    if (it) { it->checked = (chk != 0); if (g_dlg_active) dlg_draw(-1); }
    *ax = 1; *dx = 0; *argbytes = 6;
}
// IsDlgButtonChecked(hDlg, id) -> 0/1 = 2w = 4b.
static void u_isdlgbuttonchecked(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t id = arg16(c, 0); (void)arg16(c, 1);
    win16_dlgitem_t *it = dlg_find(id);
    *ax = (it && it->checked) ? 1 : 0; *dx = 0; *argbytes = 4;
}
// CheckRadioButton(hDlg, idFirst, idLast, idCheck) = 4w = 8b.
static void u_checkradiobutton(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t idc = arg16(c, 0); uint16_t idl = arg16(c, 1); uint16_t idf = arg16(c, 2); (void)arg16(c, 3);
    for (int i = 0; i < g_dlg_nitems; i++) { win16_dlgitem_t *it = &g_dlg_items[i];
        if (it->id >= idf && it->id <= idl) it->checked = (it->id == idc); }
    if (g_dlg_active) dlg_draw(-1);
    *ax = 1; *dx = 0; *argbytes = 8;
}
// GetDlgCtrlID(hwndCtl) -> id (low 12 bits of our 0xD000|id pseudo-handle) = 2b.
static void u_getdlgctrlid(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);
    *ax = (uint16_t)(h & 0x0FFF); *dx = 0; *argbytes = 2;
}
// MapDialogRect(hDlg, lpRect): dialog units -> pixels (x2, matching our base units) = 6b.
static void u_mapdialogrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t r_off = arg16(c, 0), r_seg = arg16(c, 1); (void)arg16(c, 2);
    for (int i = 0; i < 4; i++) { int16_t v = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+i*2));
        x86_16_wr16(c, r_seg, (uint16_t)(r_off+i*2), (uint16_t)(v * 2)); }
    *ax = 1; *dx = 0; *argbytes = 6;
}
// DefDlgProc(hDlg, msg, wParam, lParam) -> 0 (default) = 10b.
static void u_defdlgproc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 10;
}
// CreateDialogParam (modeless) - run modal like DialogBoxParam (MVP).
static void u_createdialogparam(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    u_dialogboxparam(c, ax, dx, argbytes);
}

// SetErrorMode (KERNEL.107): UINT SetErrorMode(UINT fuMode) = 1 word arg.
// Returns the previous error mode (none -> 0). CRITICAL: argbytes MUST be 2.
// A MISS popped argbytes=0, desyncing the Pascal stack by 2 bytes -> Chips
// WM_CREATE returned -1 and a later LoadBitmap read a bogus far ptr (grey board).
static void k_seterrormode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx,
                           uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}

// ===========================================================================
// Misc GDI state functions (ROP2 / poly-fill / stretch / mapper) + frame/pos.
// Stored as globals (per-DC in real Win16; one active DC dominates in practice).
// ===========================================================================
static int      g_gdi_rop2   = 13;   // R2_COPYPEN
static int      g_gdi_pfill  = 1;    // ALTERNATE
static int      g_gdi_sbmode = 3;    // COLORONCOLOR
static uint32_t g_gdi_mapper = 0;
static void g_setrop2(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int m = (int16_t)arg16(c, 0); (void)arg16(c, 1); int prev = g_gdi_rop2; g_gdi_rop2 = m;
    *ax = (uint16_t)prev; *dx = 0; *argbytes = 4;
}
static void g_getrop2(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); *ax = (uint16_t)g_gdi_rop2; *dx = 0; *argbytes = 2;
}
static void g_setpolyfill(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int m = (int16_t)arg16(c, 0); (void)arg16(c, 1); int prev = g_gdi_pfill; g_gdi_pfill = m;
    *ax = (uint16_t)prev; *dx = 0; *argbytes = 4;
}
static void g_getpolyfill(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); *ax = (uint16_t)g_gdi_pfill; *dx = 0; *argbytes = 2;
}
static void g_setstretchmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int m = (int16_t)arg16(c, 0); (void)arg16(c, 1); int prev = g_gdi_sbmode; g_gdi_sbmode = m;
    *ax = (uint16_t)prev; *dx = 0; *argbytes = 4;
}
static void g_getstretchmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); *ax = (uint16_t)g_gdi_sbmode; *dx = 0; *argbytes = 2;
}
static void g_setmapperflags(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t f = arg32(c, 0); (void)arg16(c, 2); uint32_t prev = g_gdi_mapper; g_gdi_mapper = f;
    *ax = (uint16_t)prev; *dx = (uint16_t)(prev >> 16); *argbytes = 6;
}
// GetCurrentPosition(hdc) -> POINT (DX:AX = y:x) = 2b.
static void g_getcurrentpos(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hdc = arg16(c, 0); int x = 0, y = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) { x = g_dcs[hdc].cur_x; y = g_dcs[hdc].cur_y; }
    *ax = (uint16_t)x; *dx = (uint16_t)y; *argbytes = 2;
}
// GetBoundsRect / SetBoundsRect (accumulated-bounds tracking - not modelled) = 8b.
static void g_getboundsrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;
}
static void g_setboundsrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;
}
// FastWindowFrame(hdc, lpRect, width, height, rop): draw a w/h-thick frame around
// the rect using the DC brush colour = 14b.
static void g_fastwindowframe(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* rop */ (void)arg32(c, 0);
    int fh = (int16_t)arg16(c, 2);
    int fw = (int16_t)arg16(c, 3);
    uint16_t r_off = arg16(c, 4), r_seg = arg16(c, 5);
    uint16_t hdc = arg16(c, 6);
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) {
        win16_dc_t *dc = &g_dcs[hdc];
        int l = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+0));
        int t = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+2));
        int rr= (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+4));
        int b = (int16_t)x86_16_rd16(c, r_seg, (uint16_t)(r_off+6));
        if (fw < 1) fw = 1;
        if (fh < 1) fh = 1;
        for (int yy = t; yy < b; yy++) for (int k = 0; k < fw; k++) { dc_plot(dc, l+k, yy, dc->brush_color); dc_plot(dc, rr-1-k, yy, dc->brush_color); }
        for (int xx = l; xx < rr; xx++) for (int k = 0; k < fh; k++) { dc_plot(dc, xx, t+k, dc->brush_color); dc_plot(dc, xx, b-1-k, dc->brush_color); }
    }
    *ax = 1; *dx = 0; *argbytes = 14;
}
// LineDDA(x1,y1,x2,y2, lpLineFunc, lParam): per-point callback. We do not invoke
// the guest callback (heavy + re-entrant); accept and balance the stack = 16b.
static void g_linedda(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 16;
}

// ===========================================================================
// Message API extras: PostAppMessage / RegisterWindowMessage / GetMessagePos /
// GetMessageTime / GetQueueStatus / GetInputState / ReplyMessage / InSendMessage.
// ===========================================================================
static void u_postappmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t lParam = arg32(c, 0);
    uint16_t wParam = arg16(c, 2);
    uint16_t msg    = arg16(c, 3);
    (void)arg16(c, 4);   // hTask
    msgq_post(0, msg, wParam, lParam);
    *ax = 1; *dx = 0; *argbytes = 10;
}
static void u_registerwindowmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    static uint16_t s_next = 0xC000;
    (void)arg16(c, 0); (void)arg16(c, 1);   // lpString (we hand out a fresh atom per call)
    *ax = s_next++; if (s_next == 0) s_next = 0xC000;
    *dx = 0; *argbytes = 4;
}
static void u_getmessagepos(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c;
    int ox=0, oy=0, ow=0, oh=0; int x = (int)mouse_x, y = (int)mouse_y;
    if (win16_host_content_rect(g_win16_host_slot, &ox, &oy, &ow, &oh) == 0) { x -= ox; y -= oy; }
    *ax = (uint16_t)x; *dx = (uint16_t)y; *argbytes = 0;
}
static void u_getmessagetime(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint32_t ms = (uint32_t)((timer_ticks * 1000ULL) / hz);
    *ax = (uint16_t)(ms & 0xFFFF); *dx = (uint16_t)(ms >> 16); *argbytes = 0;
}
static void u_getqueuestatus(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); uint16_t n = (uint16_t)(g_msgq_count ? 0x0004 : 0);   // QS_KEY-ish
    *ax = n; *dx = n; *argbytes = 2;
}
static void u_getinputstate(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)(mouse_buttons ? 1 : 0); *dx = 0; *argbytes = 0;
}
static void u_replymessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg32(c, 0); *ax = 0; *dx = 0; *argbytes = 4;
}
static void u_insendmessage(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 0;
}
static void u_getmessageextrainfo(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 0;
}

// ===========================================================================
// Menu API: programmatic build (CreateMenu/CreatePopupMenu/AppendMenu/...) on the
// existing g_menus flat-with-levels model. AppendMenu(MF_POPUP) flattens the child
// menu's items in at level+1 so the menu-bar/popup renderer shows them.
// ===========================================================================
#define MF_POPUP_F     0x0010
#define MF_SEPARATOR_F 0x0800
static uint16_t menu_alloc(void) {
    for (int i = 0; i < WIN16_MAX_MENUS; i++) if (!g_menus[i].used) {
        g_menus[i].used = 1; g_menus[i].count = 0; g_menus[i].ntop = 0; return (uint16_t)(i+1);
    }
    return 0;
}
static win16_menu_t *menu_get(uint16_t h) {
    if (!h || h > WIN16_MAX_MENUS || !g_menus[h-1].used) return 0;
    return &g_menus[h-1];
}
static void u_createmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = menu_alloc(); *dx = 0; *argbytes = 0;
}
static void u_createpopupmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = menu_alloc(); *dx = 0; *argbytes = 0;
}
// SetMessageQueue (USER.266): BOOL SetMessageQueue(int cMsg) = 1 word = 2 bytes.
// (#278 Word6) WINWORD startup loops: push n; SetMessageQueue; or ax,ax; jz retry,
// shrinking n from 0x40 until it SUCCEEDS (AX!=0). Our queue is effectively
// unbounded, so always report success. Returning 0 (the UNIMPL default) made Word
// spin forever. Real Win16 returns TRUE when it could (re)allocate the queue.
static void u_setmessagequeue(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 2;
}
// RegisterClipboardFormat (USER.145): ATOM RegisterClipboardFormat(LPCSTR lpsz)
// = 1 far ptr = 2 words = 4 bytes. Return a non-zero pseudo-atom in the 0xC000
// range so Word treats the format as registered. (#278 Word6)
static void u_registerclipboardformat(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c;
    static uint16_t s_cf_next = 0xC100;
    *ax = s_cf_next++;
    if (s_cf_next == 0) s_cf_next = 0xC100;
    *dx = 0; *argbytes = 4;
}
static void u_destroymenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0); win16_menu_t *m = menu_get(h);
    if (m) { m->used = 0; m->count = 0; m->ntop = 0; }
    *ax = m ? 1 : 0; *dx = 0; *argbytes = 2;
}
static void u_appendmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t txt_off = arg16(c, 0), txt_seg = arg16(c, 1);
    uint16_t idnew = arg16(c, 2); uint16_t flags = arg16(c, 3); uint16_t h = arg16(c, 4);
    *argbytes = 10; *ax = 0; *dx = 0;
    win16_menu_t *m = menu_get(h); if (!m) return;
    char txt[24]; txt[0] = 0;
    if (!(flags & MF_SEPARATOR_F) && (txt_seg || txt_off)) rd_far_cstr(c, txt_seg, txt_off, txt, sizeof(txt));
    if (flags & MF_POPUP_F) {                       // idnew = child HMENU
        if (m->count < WIN16_MAX_MITEMS) {
            win16_mitem_t *it = &m->items[m->count];
            int k=0; for (; txt[k] && k<23; k++) it->text[k]=txt[k]; it->text[k]=0;
            it->id = 0; it->is_popup = 1; it->is_sep = 0; it->level = 0; it->sub_h = idnew;
            if (m->ntop < WIN16_MAX_TOP) m->top_idx[m->ntop++] = m->count;
            m->count++;
        }
        win16_menu_t *ch = menu_get(idnew);
        if (ch) for (int i = 0; i < ch->count && m->count < WIN16_MAX_MITEMS; i++) {
            win16_mitem_t *d = &m->items[m->count++]; *d = ch->items[i];
            d->level = (uint8_t)(ch->items[i].level + 1);
        }
    } else if (m->count < WIN16_MAX_MITEMS) {
        win16_mitem_t *it = &m->items[m->count++];
        int k=0; for (; txt[k] && k<23; k++) it->text[k]=txt[k]; it->text[k]=0;
        it->id = idnew; it->is_popup = 0; it->is_sep = (flags & MF_SEPARATOR_F) ? 1 : 0; it->level = 0; it->sub_h = 0;
    }
    *ax = 1;
}
// InsertMenu/ModifyMenu: approximate insert as append (correct stack cleanup) =
// 12b. (#278 Word6) MF_POPUP attaches a child menu: record idnew as the item's
// sub_h and register it top-level so GetSubMenu(hMenu,pos) can recover the child
// handle (Word's menu-build pattern). Non-popup items keep the prior append.
static void u_insertmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t txt_off = arg16(c, 0), txt_seg = arg16(c, 1);
    uint16_t idnew = arg16(c, 2); uint16_t flags = arg16(c, 3); /* pos */ (void)arg16(c, 4);
    uint16_t h = arg16(c, 5); win16_menu_t *m = menu_get(h);
    if (m && m->count < WIN16_MAX_MITEMS) {
        win16_mitem_t *it = &m->items[m->count];
        it->text[0]=0;
        if (!(flags & MF_SEPARATOR_F) && (txt_seg || txt_off))
            rd_far_cstr(c, txt_seg, txt_off, it->text, sizeof(it->text));
        if (flags & MF_POPUP_F) {
            it->id = 0; it->is_popup = 1; it->is_sep = 0; it->level = 0; it->sub_h = idnew;
            if (m->ntop < WIN16_MAX_TOP) m->top_idx[m->ntop++] = m->count;
        } else {
            it->id = idnew; it->is_popup = 0; it->is_sep = (flags & MF_SEPARATOR_F)?1:0; it->level = 0; it->sub_h = 0;
        }
        m->count++;
    }
    *ax = m ? 1 : 0; *dx = 0; *argbytes = 12;
}
static void u_modifymenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t txt_off = arg16(c, 0), txt_seg = arg16(c, 1);
    uint16_t idnew = arg16(c, 2); uint16_t flags = arg16(c, 3); uint16_t pos = arg16(c, 4);
    uint16_t h = arg16(c, 5); win16_menu_t *m = menu_get(h);
    int found = 0;
    // (#278 Word6 pass29) Match by position when MF_BYPOSITION (0x400) is set,
    // otherwise by command id. The previous unconditional `i == pos` fallback made
    // a MF_BYCOMMAND modify of item 0 always match item index 0, which (a) hid the
    // real lookup and (b) made Word's `while(ModifyMenu(sysmenu,0,MF_SEPARATOR,-1,
    // NULL))` drain loop never terminate on a populated menu. With command-id
    // matching the drain re-ids the id==0 separators and stops, while Word's
    // ModifyMenu(SC_TASKLIST,...) finds the real command. Games (segcount<100) keep
    // the original lenient matching for byte-identical behaviour.
    int bypos = (flags & 0x0400) != 0;   // MF_BYPOSITION
    if (m) {
        if (g_info.segcount >= 100) {
            for (int i = 0; i < m->count; i++) {
                int match = bypos ? (i == (int)pos) : (m->items[i].id == pos);
                if (!match) continue;
                if (txt_seg || txt_off) rd_far_cstr(c, txt_seg, txt_off, m->items[i].text, sizeof(m->items[i].text));
                m->items[i].id = idnew; found = 1; break;
            }
        } else {
            for (int i = 0; i < m->count; i++) if (m->items[i].id == pos || i == (int)pos) {
                if (txt_seg || txt_off) rd_far_cstr(c, txt_seg, txt_off, m->items[i].text, sizeof(m->items[i].text));
                m->items[i].id = idnew; found = 1; break;
            }
        }
    }
    // (#278 Word6 pass17) Faithful Win16: ModifyMenu returns FALSE when the item
    // does not exist (previously returned TRUE whenever the menu handle was valid).
    // seg31:0x249a does `while (ModifyMenu(sysmenu,0,0x800,-1,NULL)) ;` over the
    // (empty) system menu; the old always-TRUE looped forever once GetSystemMenu
    // returned a real handle.
    *ax = found ? 1 : 0; *dx = 0; *argbytes = 12;
}
// DeleteMenu/RemoveMenu(hMenu, uPos, uFlags): remove by id or position = 6b.
static void u_deletemenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* flags */ (void)arg16(c, 0); uint16_t pos = arg16(c, 1); uint16_t h = arg16(c, 2);
    win16_menu_t *m = menu_get(h); int done = 0;
    if (m) for (int i = 0; i < m->count; i++) if (m->items[i].id == pos) {
        for (int j = i; j < m->count-1; j++) m->items[j] = m->items[j+1];
        m->count--; done = 1; break;
    }
    *ax = done; *dx = 0; *argbytes = 6;
}
// GetSubMenu(hMenu, nPos): return the child HMENU recorded (sub_h) for the popup
// item at position nPos. (#278 Word6) nPos is MF_BYPOSITION; map it to the
// top-level item list (insertion order), falling back to the absolute item
// index. Returns 0 only when the item is not a popup (correct Win16). = 4b.
static void u_getsubmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t pos = arg16(c, 0); uint16_t h = arg16(c, 1);
    win16_menu_t *m = menu_get(h); win16_mitem_t *it = 0; uint16_t sub = 0;
    if (m) {
        if (pos < (uint16_t)m->ntop)        it = &m->items[m->top_idx[pos]];
        else if (pos < (uint16_t)m->count)  it = &m->items[pos];
    }
    if (it && it->is_popup) sub = it->sub_h;
    *ax = sub; *dx = 0; *argbytes = 4;
}
static void u_getmenuitemcount(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0); win16_menu_t *m = menu_get(h);
    *ax = m ? (uint16_t)(m->ntop ? m->ntop : m->count) : 0xFFFF; *dx = 0; *argbytes = 2;
}
static void u_getmenuitemid(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t pos = arg16(c, 0); uint16_t h = arg16(c, 1); win16_menu_t *m = menu_get(h);
    *ax = (m && pos < (uint16_t)m->count) ? m->items[pos].id : 0xFFFF; *dx = 0; *argbytes = 4;
}
static void u_getmenustring(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t flags = arg16(c, 0); uint16_t cb = arg16(c, 1);
    uint16_t s_off = arg16(c, 2), s_seg = arg16(c, 3);
    uint16_t item = arg16(c, 4); uint16_t h = arg16(c, 5);
    win16_menu_t *m = menu_get(h); const char *src = 0;
    if (m) {
        if (flags & 0x400) { if (item < (uint16_t)m->count) src = m->items[item].text; }   // MF_BYPOSITION
        else for (int i = 0; i < m->count; i++) if (m->items[i].id == item) { src = m->items[i].text; break; }
    }
    int n = 0; if (src) for (const char *p = src; *p && n < (int)cb-1; p++) { x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), (uint8_t)*p); n++; }
    if (cb > 0) x86_16_wr8(c, s_seg, (uint16_t)(s_off+n), 0);
    *ax = (uint16_t)n; *dx = 0; *argbytes = 12;
}
static void u_getmenustate(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); (void)arg16(c, 1); (void)arg16(c, 2); *ax = 0; *dx = 0; *argbytes = 6;
}
static void u_ismenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0); *ax = menu_get(h) ? 1 : 0; *dx = 0; *argbytes = 2;
}
static void u_removemenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    u_deletemenu(c, ax, dx, argbytes);   // same args; RemoveMenu keeps submenus but we flatten
}
static void u_hilitemenuitem(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;   // (hWnd,hMenu,id,hilite)
}
// GetSystemMenu(hWnd, bRevert) = 4b. (#278 Word6 pass17) Word's app-init (phase-2
// gate seg136:0x129a -> seg31:0x249a) does:
//   si = GetSystemMenu(hwnd, FALSE); if (!si) return 0;  // <-- this gate
//   while (ModifyMenu(si, 0, MF_SEPARATOR, -1, NULL)) ;  // strip default items
//   di = GetMenuState(si, 0, 0); return (di==0 || di==0xFFFF);
// A stub returning 0 failed the first gate -> OLE2 teardown -> FatalAppExit.
// Return a real (cached, empty) system-menu handle: bRevert==0 -> the window's
// modifiable system-menu copy (non-zero); bRevert!=0 -> reset to default, NULL
// (Win16). The empty menu makes ModifyMenu/GetMenuState benign no-ops so the
// routine reaches its success path, and the handle stays valid for any later
// AppendMenu (e.g. Word's "About" item) instead of silently dropping it.
// (#278 Word6 pass29) Populate a menu with the standard Win16 system-menu items
// (by command id). Word's MDI document-window setup (seg31:0x24de) does
// GetSystemMenu(hwnd,FALSE) then ModifyMenu(SC_TASKLIST 0xF130 -> SC_NEXTWINDOW
// 0xF040, "Next Window"). With an EMPTY system menu that ModifyMenu cannot find
// the item, returns FALSE, and Word's doc-object builder (seg155:0x678) treats the
// FALSE as fatal and surfaces "There is insufficient memory. Close extra windows"
// INSTEAD of the Document1 page. Populating SC_TASKLIST (and the rest) lets the
// modify succeed so the real document renders. SC_ ids match Win16 USER.
static void win16_populate_sysmenu(win16_menu_t *m) {
    static const struct { const char *t; uint16_t id; uint8_t sep; } it[] = {
        {"Restore",      0xF120, 0}, {"Move",     0xF010, 0}, {"Size", 0xF000, 0},
        {"Minimize",     0xF020, 0}, {"Maximize", 0xF030, 0},
        {"", 0, 1},
        {"Close",        0xF060, 0},
        {"", 0, 1},
        {"Switch To...", 0xF130, 0},
    };
    m->count = 0; m->ntop = 0;
    for (unsigned i = 0; i < sizeof(it)/sizeof(it[0]) && m->count < WIN16_MAX_MITEMS; i++) {
        win16_mitem_t *e = &m->items[m->count++];
        int k = 0; for (; it[i].t[k] && k < 23; k++) e->text[k] = it[i].t[k]; e->text[k] = 0;
        e->id = it[i].id; e->is_popup = 0; e->is_sep = it[i].sep; e->level = 0; e->sub_h = 0;
    }
}
// Per-window cache of the system-menu handle (Word path). Each window keeps its
// own populated copy so one window's ModifyMenu does not affect another's.
static uint16_t g_sysmenu_hwnd[64];
static uint16_t g_sysmenu_h[64];
static int      g_sysmenu_n;
static void u_getsystemmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t bRevert = arg16(c, 0); uint16_t hwnd = arg16(c, 1);
    static uint16_t g_sysmenu = 0;
    *dx = 0; *argbytes = 4;
    if (bRevert) { *ax = 0; return; }
    if (g_info.segcount >= 100) {     // Word: per-window populated system menu
        for (int i = 0; i < g_sysmenu_n; i++)
            if (g_sysmenu_hwnd[i] == hwnd && menu_get(g_sysmenu_h[i])) { *ax = g_sysmenu_h[i]; return; }
        uint16_t h = menu_alloc();
        if (h) {
            win16_populate_sysmenu(menu_get(h));
            if (g_sysmenu_n < 64) { g_sysmenu_hwnd[g_sysmenu_n] = hwnd; g_sysmenu_h[g_sysmenu_n] = h; g_sysmenu_n++; }
        }
        *ax = h; return;
    }
    if (!g_sysmenu || !menu_get(g_sysmenu)) g_sysmenu = menu_alloc();
    *ax = g_sysmenu;
}
// TrackPopupMenu(hMenu, wFlags, x, y, nReserved, hWnd, lprc) -> 0 (no cmd) = 16b.
static void u_trackpopupmenu(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 16;
}


// ===========================================================================
// (#278 Word6) SHELL.DLL registration-database (Win16 reg.dat) ordinals.
// Word 6 + OLE2/COMPOBJ register their OLE class info via the Win3.1 SHELL.DLL
// registry API and then READ IT BACK; a stub that returns "not found" makes Word
// declare "The Windows registration database file is not valid". So provide a real
// (small) in-memory store: RegSetValue records the DEFAULT value of a subkey path;
// RegQueryValue returns it. Keyed by the full subkey path string (Word registers a
// flat set of CLSID\\... paths). Returns LONG error codes in DX:AX (0 = success).
// ---------------------------------------------------------------------------
#define SHREG_MAX     256
#define SHREG_KEYLEN  96
#define SHREG_VALLEN  128
typedef struct { char key[SHREG_KEYLEN]; char val[SHREG_VALLEN]; int used; } shreg_ent_t;
static shreg_ent_t g_shreg[SHREG_MAX];
static uint16_t    g_shell_next_hkey = 0x0100;

static void shreg_read_str(x86_16_cpu_t *c, uint16_t seg, uint16_t off, char *dst, int max) {
    int i = 0;
    if (seg || off) {
        for (; i < max - 1; i++) {
            uint8_t ch = x86_16_rd8(c, seg, (uint16_t)(off + i));
            if (!ch) break;
            dst[i] = (char)ch;
        }
    }
    dst[i] = 0;
}
static int shreg_streq(const char *a, const char *b) {
    while (*a && *b) { char ca=*a, cb=*b; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32; if(ca!=cb)return 0; a++;b++; }
    return *a==0 && *b==0;
}
static shreg_ent_t *shreg_find(const char *key) {
    for (int i=0;i<SHREG_MAX;i++) if (g_shreg[i].used && shreg_streq(g_shreg[i].key,key)) return &g_shreg[i];
    return 0;
}
static shreg_ent_t *shreg_alloc(const char *key) {
    shreg_ent_t *e = shreg_find(key);
    if (e) return e;
    for (int i=0;i<SHREG_MAX;i++) if (!g_shreg[i].used) {
        int j=0; for(; key[j] && j<SHREG_KEYLEN-1; j++) g_shreg[i].key[j]=key[j]; g_shreg[i].key[j]=0;
        g_shreg[i].used=1; g_shreg[i].val[0]=0; return &g_shreg[i];
    }
    return 0;
}

extern int g_ole2_k334log;
#define SHLOG(...) do { if (g_ole2_k334log) kprintf("[SHELL] " __VA_ARGS__); } while(0)

// (#278 Word6) Seed a minimal-but-valid Win3.1 registration database so Word's
// startup validity check ("The Windows registration database file is not valid")
// is satisfied. In a real REG.DAT the root key '\' carries a default value and a
// small set of standard class/ProgID entries exist. We store values keyed by the
// absolute subkey path (the root "" gets a non-empty default). seeded at run start.
static void shreg_seed_str(const char *key, const char *val) {
    shreg_ent_t *e = shreg_alloc(key);
    if (e) { int j=0; for(; val[j] && j<SHREG_VALLEN-1; j++) e->val[j]=val[j]; e->val[j]=0; }
}
static void shreg_seed_root(void) {
    // root '\' default value -- presence of a real default is what makes the DB "valid".
    shreg_seed_str("", ".reg-db");
    // (#278 Word6) THE validity check (RE'd in b636): at startup Word does
    //   RegOpenKey(HKEY_CLASSES_ROOT,"CLSID\\{00020901-...}\\LocalServer");
    //   RegQueryValue(thatkey, default="")
    // and if the default value (the path to its OLE LocalServer = WINWORD.EXE) is
    // NOT FOUND it shows "The Windows registration database file is not valid."
    // {00020901-0000-0000-C000-000000000046} is Word.Document.6's OLE class. Seed the
    // full standard set Word 6 self-registers so the read-back succeeds.
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}",
                   "Microsoft Word 6.0 Document");
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}\\LocalServer",
                   "WINWORD.EXE");
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}\\LocalServer32",
                   "WINWORD.EXE");
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}\\InprocHandler",
                   "ole2.dll");
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}\\ProgID",
                   "Word.Document.6");
    shreg_seed_str("CLSID\\{00020901-0000-0000-C000-000000000046}\\InsertableObject",
                   "");
    shreg_seed_str("CLSID\\{00020906-0000-0000-C000-000000000046}",
                   "Microsoft Word 6.0 Picture");
    shreg_seed_str("CLSID\\{00020906-0000-0000-C000-000000000046}\\LocalServer",
                   "WINWORD.EXE");
    // ProgID <-> CLSID mapping Word reads back.
    shreg_seed_str(".doc", "Word.Document.6");
    shreg_seed_str("Word.Document.6", "Microsoft Word 6.0 Document");
    shreg_seed_str("Word.Document.6\\CLSID",
                   "{00020901-0000-0000-C000-000000000046}");
    shreg_seed_str("Word.Document", "Microsoft Word Document");
    shreg_seed_str("Word.Document\\CurVer", "Word.Document.6");
}

// (#278 Word6) The Win3.1 registry is HIERARCHICAL: RegOpenKey/RegCreateKey return
// an HKEY for a subkey path, and later RegQueryValue/RegSetValue take that HKEY plus
// a RELATIVE subkey. Word (and OLE2) open HKEY_CLASSES_ROOT (==1) then query values
// relative to opened keys. To make our flat path-keyed store behave hierarchically we
// map each HKEY -> its absolute path and compose absolute = hkeypath + "\\" + relsub.
#define SHKEY_MAX  64
typedef struct { uint16_t hkey; char path[SHREG_KEYLEN]; int used; } shkey_t;
static shkey_t g_shkeys[SHKEY_MAX];
// HKEY_CLASSES_ROOT in Win16 == 1; treat as the registry root ("").
static const char *shkey_path(uint16_t hkey) {
    if (hkey == 0 || hkey == 1) return "";   // root / HKEY_CLASSES_ROOT
    for (int i=0;i<SHKEY_MAX;i++) if (g_shkeys[i].used && g_shkeys[i].hkey==hkey) return g_shkeys[i].path;
    return "";
}
static uint16_t shkey_make(const char *path) {
    uint16_t h = g_shell_next_hkey++;
    for (int i=0;i<SHKEY_MAX;i++) if (!g_shkeys[i].used) {
        int j=0; for(; path[j] && j<SHREG_KEYLEN-1; j++) g_shkeys[i].path[j]=path[j]; g_shkeys[i].path[j]=0;
        g_shkeys[i].hkey=h; g_shkeys[i].used=1; return h;
    }
    return h;
}
// compose absolute key path = base + "\\" + sub (skip separators when either empty)
static void shkey_compose(char *out, int max, const char *base, const char *sub) {
    int n=0;
    for (; base && base[n] && n<max-1; n++) out[n]=base[n];
    if (sub && sub[0]) {
        if (n>0 && n<max-1) out[n++]='\\';
        for (int j=0; sub[j] && n<max-1; j++,n++) out[n]=sub[j];
    }
    out[n]=0;
}

// SHELL.1 RegOpenKey(HKEY hKey, LPCSTR lpSubKey, HKEY FAR* phkResult)
static void sh_regopenkey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t phk_off = arg16(c, 0), phk_seg = arg16(c, 1);
    char sub[SHREG_KEYLEN]; shreg_read_str(c, arg16(c,3), arg16(c,2), sub, SHREG_KEYLEN);
    uint16_t hkey = arg16(c, 4);
    char abs[SHREG_KEYLEN]; shkey_compose(abs, SHREG_KEYLEN, shkey_path(hkey), sub);
    uint16_t nh = shkey_make(abs);
    if (phk_seg || phk_off) x86_16_wr16(c, phk_seg, phk_off, nh);
    SHLOG("RegOpenKey(h=%04x,'%s') -> abs='%s' nh=%04x\n", hkey, sub, abs, nh);
    *ax = 0; *dx = 0; *argbytes = 10;
}
// SHELL.2 RegCreateKey(HKEY, LPCSTR, HKEY FAR*)
static void sh_regcreatekey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t phk_off = arg16(c, 0), phk_seg = arg16(c, 1);
    char sub[SHREG_KEYLEN]; shreg_read_str(c, arg16(c,3), arg16(c,2), sub, SHREG_KEYLEN);
    uint16_t hkey = arg16(c, 4);
    char abs[SHREG_KEYLEN]; shkey_compose(abs, SHREG_KEYLEN, shkey_path(hkey), sub);
    if (abs[0]) shreg_alloc(abs);
    uint16_t nh = shkey_make(abs);
    if (phk_seg || phk_off) x86_16_wr16(c, phk_seg, phk_off, nh);
    SHLOG("RegCreateKey(h=%04x,'%s') -> abs='%s' nh=%04x\n", hkey, sub, abs, nh);
    *ax = 0; *dx = 0; *argbytes = 10;
}
// SHELL.3 RegCloseKey(HKEY)
static void sh_regclosekey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hkey = arg16(c, 0);
    for (int i=0;i<SHKEY_MAX;i++) if (g_shkeys[i].used && g_shkeys[i].hkey==hkey) g_shkeys[i].used=0;
    *ax = 0; *dx = 0; *argbytes = 2;
}
// SHELL.4 RegDeleteKey(HKEY, LPCSTR lpSubKey)
//   args: lpSubKey(0,1) hKey(2)
static void sh_regdeletekey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    char sub[SHREG_KEYLEN]; shreg_read_str(c, arg16(c,1), arg16(c,0), sub, SHREG_KEYLEN);
    uint16_t hkey = arg16(c, 2);
    char abs[SHREG_KEYLEN]; shkey_compose(abs, SHREG_KEYLEN, shkey_path(hkey), sub);
    shreg_ent_t *e = shreg_find(abs); if (e) e->used=0;
    SHLOG("RegDeleteKey(h=%04x,'%s') abs='%s'\n", hkey, sub, abs);
    *ax = 0; *dx = 0; *argbytes = 6;
}
// SHELL.5 RegSetValue(HKEY hKey, LPCSTR lpSubKey, DWORD dwType, LPCSTR lpData, DWORD cbData)
//   args: cbData(0,1) lpData(2,3) dwType(4,5) lpSubKey(6,7) hKey(8)
static void sh_regsetvalue(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t data_off=arg16(c,2), data_seg=arg16(c,3);
    uint16_t sk_off=arg16(c,6), sk_seg=arg16(c,7);
    uint16_t hkey = arg16(c, 8);
    char sub[SHREG_KEYLEN]; shreg_read_str(c, sk_seg, sk_off, sub, SHREG_KEYLEN);
    char abs[SHREG_KEYLEN]; shkey_compose(abs, SHREG_KEYLEN, shkey_path(hkey), sub);
    char data[SHREG_VALLEN]; shreg_read_str(c, data_seg, data_off, data, SHREG_VALLEN);
    shreg_ent_t *e = shreg_alloc(abs[0] ? abs : "");
    if (e) { int j=0; for(; data[j] && j<SHREG_VALLEN-1; j++) e->val[j]=data[j]; e->val[j]=0; }
    SHLOG("RegSetValue(h=%04x,'%s'='%s') abs='%s'\n", hkey, sub, data, abs);
    *ax = 0; *dx = 0; *argbytes = 18;
}
// SHELL.6 RegQueryValue(HKEY hKey, LPCSTR lpSubKey, LPSTR lpValue, LONG FAR* lpcbValue)
//   args (last-pushed first): lpcb(0,1) lpValue(2,3) lpSubKey(4,5) hKey(6)
static void sh_regqueryvalue(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cb_off=arg16(c,0), cb_seg=arg16(c,1);
    uint16_t val_off=arg16(c,2), val_seg=arg16(c,3);
    uint16_t sk_off=arg16(c,4), sk_seg=arg16(c,5);
    uint16_t hkey = arg16(c, 6);
    char sub[SHREG_KEYLEN]; shreg_read_str(c, sk_seg, sk_off, sub, SHREG_KEYLEN);
    char abs[SHREG_KEYLEN]; shkey_compose(abs, SHREG_KEYLEN, shkey_path(hkey), sub);
    shreg_ent_t *e = shreg_find(abs[0] ? abs : "");
    if (e) {
        int n=0; for(; e->val[n]; n++);
        if (val_seg||val_off){ int i=0; for(;i<n;i++) x86_16_wr8(c,val_seg,(uint16_t)(val_off+i),(uint8_t)e->val[i]); x86_16_wr8(c,val_seg,(uint16_t)(val_off+i),0);}
        if (cb_seg||cb_off){ x86_16_wr16(c,cb_seg,cb_off,(uint16_t)(n+1)); x86_16_wr16(c,cb_seg,(uint16_t)(cb_off+2),0);}
        SHLOG("RegQueryValue(h=%04x,'%s') abs='%s' -> '%s' OK\n", hkey, sub, abs, e->val);
        *ax=0; *dx=0;          // ERROR_SUCCESS
    } else {
        if (val_seg||val_off) x86_16_wr8(c,val_seg,val_off,0);
        if (cb_seg||cb_off){ x86_16_wr16(c,cb_seg,cb_off,0); x86_16_wr16(c,cb_seg,(uint16_t)(cb_off+2),0);}
        SHLOG("RegQueryValue(h=%04x,'%s') abs='%s' -> NOT FOUND\n", hkey, sub, abs);
        *ax=2; *dx=0;          // ERROR_FILE_NOT_FOUND
    }
    *argbytes = 14;
}
// SHELL.7 RegEnumKey(HKEY hKey, DWORD iSubKey, LPSTR lpName, DWORD cbName) -> 0 / ERROR_NO_MORE_ITEMS
// Pascal arg layout (RE'd b641, last-pushed = arg16(0)):
//   cbName=arg32(0)  lpName(far)=arg16(3):arg16(2)  iSubKey=arg32(4)  hKey=arg16(6)
// Enumerates the immediate (one level) child key names of hKey's absolute path
// from the flat g_shreg[] store. Word's app-init (WINWORD seg15:0x81c) enumerates
// HKEY_CLASSES_ROOT's subkeys to (re)build its OLE class list; the old stub always
// returned NO_MORE_ITEMS so its loop never ran and app-init returned FAILURE (R6021).
static void sh_regenumkey(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t cbName = arg16(c,0);
    uint16_t name_off = arg16(c,2), name_seg = arg16(c,3);
    uint32_t iSubKey = arg32(c,4);
    uint16_t hkey = arg16(c,6);
    *argbytes = 14;
    const char *base = shkey_path(hkey);
    int blen = 0; while (base[blen]) blen++;
    // Collect the iSubKey-th DISTINCT immediate child name under `base`.
    // An entry key K is a descendant of base if it starts with base (and, when base
    // is non-empty, the next char is '\'). Its immediate child = the path component
    // right after base up to the next '\'.
    char found[SHREG_KEYLEN]; found[0]=0;
    uint32_t idx = 0; int got = 0;
    // Track already-emitted names to keep the enumeration distinct + ordered by
    // first appearance; iSubKey selects the (iSubKey)-th distinct one.
    for (int i=0; i<SHREG_MAX && !got; i++) {
        if (!g_shreg[i].used) continue;
        const char *k = g_shreg[i].key;
        // does k live under base?
        const char *child;
        if (blen == 0) {
            if (!k[0]) continue;                 // the root "" itself, skip
            child = k;
        } else {
            int j=0; for (; j<blen && k[j]==base[j]; j++) {}
            if (j != blen) continue;             // not a prefix
            if (k[blen] != '\\') continue;     // must be exactly under base
            child = k + blen + 1;
        }
        // extract the immediate component name (up to next '\\')
        char comp[SHREG_KEYLEN]; int cn=0;
        for (; child[cn] && child[cn]!='\\' && cn<SHREG_KEYLEN-1; cn++) comp[cn]=child[cn];
        comp[cn]=0;
        if (cn==0) continue;
        // is this comp already counted (distinct)? rescan earlier entries
        int dup = 0;
        for (int p=0; p<i && !dup; p++) {
            if (!g_shreg[p].used) continue;
            const char *pk = g_shreg[p].key;
            const char *pc;
            if (blen==0) { if(!pk[0]) continue; pc=pk; }
            else { int j2=0; for(; j2<blen && pk[j2]==base[j2]; j2++){} if(j2!=blen||pk[blen]!='\\') continue; pc=pk+blen+1; }
            int q=0; for(; comp[q] && pc[q]==comp[q]; q++){}
            if (comp[q]==0 && (pc[q]==0 || pc[q]=='\\')) { dup=1; }
        }
        if (dup) continue;
        if (idx == iSubKey) {
            int z=0; for(; comp[z] && z<SHREG_KEYLEN-1; z++) found[z]=comp[z]; found[z]=0;
            got = 1; break;
        }
        idx++;
    }
    if (got) {
        int n=0; for(; found[n]; n++);
        if (cbName && n >= cbName) n = cbName-1;
        if (name_seg || name_off) {
            int i=0; for(; i<n; i++) x86_16_wr8(c,name_seg,(uint16_t)(name_off+i),(uint8_t)found[i]);
            x86_16_wr8(c,name_seg,(uint16_t)(name_off+i),0);
        }
        SHLOG("RegEnumKey(h=%04x base='%s' i=%lu) -> '%s' OK\n", hkey, base, (unsigned long)iSubKey, found);
        *ax = 0; *dx = 0;                 // ERROR_SUCCESS
    } else {
        if (name_seg || name_off) x86_16_wr8(c,name_seg,name_off,0);
        SHLOG("RegEnumKey(h=%04x base='%s' i=%lu) -> NO_MORE_ITEMS\n", hkey, base, (unsigned long)iSubKey);
        *ax = 0x103; *dx = 0;             // ERROR_NO_MORE_ITEMS
    }
}

// ===========================================================================
// (#188 ordinal-coverage pass) Additional GDI/USER handlers. All ADDITIVE: the
// prior code left these UNIMPL (argbytes=0 -> Pascal stack desync). Existing
// identity-path apps (FreeCell/Chips/Solitaire/Fuji Golf) never call them, so
// they are byte-unaffected. argbytes cross-checked against the WINE gdi.exe16 /
// user.exe16 specs. Real values / drawing where cheap so DIB / region /
// coordinate-mapping-heavy apps render instead of black-boxing or crashing.
// ===========================================================================

// ---- DC save/restore stack (GDI.30 SaveDC / GDI.39 RestoreDC) ----
#define WIN16_DC_SAVE_MAX 64
static win16_dc_t g_dc_save[WIN16_DC_SAVE_MAX];
static uint16_t   g_dc_save_hdc[WIN16_DC_SAVE_MAX];
int               g_dc_save_top;   // reset in win16_api_begin (non-static: reset there)

static void g_savedc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hdc = arg16(c, 0);
    *argbytes = 2; *dx = 0; *ax = 0;
    if (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used && g_dc_save_top < WIN16_DC_SAVE_MAX) {
        g_dc_save[g_dc_save_top]     = g_dcs[hdc];
        g_dc_save_hdc[g_dc_save_top] = hdc;
        g_dc_save_top++;
        *ax = (uint16_t)g_dc_save_top;   // saved level (>=1)
    }
}
static void g_restoredc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int level = (int16_t)arg16(c, 0);
    uint16_t hdc = arg16(c, 1);
    *argbytes = 4; *dx = 0; *ax = 0;
    if (!(hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) || g_dc_save_top <= 0) return;
    int target = (level < 0) ? (g_dc_save_top + level) : (level - 1);
    if (target < 0) target = 0;
    if (target >= g_dc_save_top) target = g_dc_save_top - 1;
    int idx = target;
    while (idx >= 0 && g_dc_save_hdc[idx] != hdc) idx--;
    if (idx >= 0) {
        int used = g_dcs[hdc].used;
        g_dcs[hdc] = g_dc_save[idx];
        g_dcs[hdc].used = used;
    }
    g_dc_save_top = target;             // pop target and everything above
    *ax = 1;
}

// ---- DC state getters (return the real DC fields) ----
static win16_dc_t *dcref(uint16_t hdc) {
    return (hdc && hdc < WIN16_MAX_DC && g_dcs[hdc].used) ? &g_dcs[hdc] : 0;
}
// GetBkColor (GDI.75) long-return. GetBkMode (GDI.76) / GetMapMode (GDI.81) /
// GetTextAlign (GDI.345) 16-bit. All 1 word arg = 2 bytes.
static void g_getbkcolor(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); uint32_t v = dc ? dc->bk_color : 0xFFFFFF;
    *ax = (uint16_t)(v & 0xFFFF); *dx = (uint16_t)(v >> 16); *argbytes = 2;
}
static void g_getbkmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc ? dc->bk_mode : 2); *dx = 0; *argbytes = 2;
}
static void g_getmapmode(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc ? dc->mapmode : 1); *dx = 0; *argbytes = 2;
}
static void g_gettextalign_r(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0 /*TA_LEFT|TA_TOP*/; *dx = 0; *argbytes = 2;
}
// GetViewportExt/Org, GetWindowExt/Org (GDI.94-97): DWORD (x/cx in AX, y/cy in DX).
static void g_getviewportext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc?dc->vex:1); *dx = (uint16_t)(dc?dc->vey:1); *argbytes = 2;
}
static void g_getviewportorg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc?dc->vox:0); *dx = (uint16_t)(dc?dc->voy:0); *argbytes = 2;
}
static void g_getwindowext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc?dc->wex:1); *dx = (uint16_t)(dc?dc->wey:1); *argbytes = 2;
}
static void g_getwindoworg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    win16_dc_t *dc = dcref(arg16(c,0)); *ax = (uint16_t)(dc?dc->wox:0); *dx = (uint16_t)(dc?dc->woy:0); *argbytes = 2;
}
// ...Ex variants (GDI.470,472-475): write POINT/SIZE to lpBuf far, return TRUE. 1w+ptr = 6b.
static void ex_write2(x86_16_cpu_t *c, uint16_t off, uint16_t seg, int a, int b) {
    if (seg || off) { x86_16_wr16(c, seg, off, (uint16_t)a); x86_16_wr16(c, seg, (uint16_t)(off+2), (uint16_t)b); }
}
static void g_getcurposex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc = dcref(arg16(c,2));
    ex_write2(c, off, seg, dc?dc->cur_x:0, dc?dc->cur_y:0); *ax=1; *dx=0; *argbytes=6;
}
static void g_getviewportextex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc = dcref(arg16(c,2));
    ex_write2(c, off, seg, dc?dc->vex:1, dc?dc->vey:1); *ax=1; *dx=0; *argbytes=6;
}
static void g_getviewportorgex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc = dcref(arg16(c,2));
    ex_write2(c, off, seg, dc?dc->vox:0, dc?dc->voy:0); *ax=1; *dx=0; *argbytes=6;
}
static void g_getwindowextex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc = dcref(arg16(c,2));
    ex_write2(c, off, seg, dc?dc->wex:1, dc?dc->wey:1); *ax=1; *dx=0; *argbytes=6;
}
static void g_getwindoworgex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc = dcref(arg16(c,2));
    ex_write2(c, off, seg, dc?dc->wox:0, dc?dc->woy:0); *ax=1; *dx=0; *argbytes=6;
}
// MoveToEx (GDI.483): (hdc,x,y,lpPoint) = 10b. Sets pos, writes OLD pos to lpPoint.
static void g_movetoex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); int y=(int16_t)arg16(c,2),x=(int16_t)arg16(c,3);
    win16_dc_t *dc = dcref(arg16(c,4)); int ox=0,oy=0;
    if (dc) { ox=dc->cur_x; oy=dc->cur_y; dc->cur_x=x; dc->cur_y=y; }
    ex_write2(c, off, seg, ox, oy); *ax=1; *dx=0; *argbytes=10;
}
// Set*OrgEx / Set*ExtEx (GDI.479-482): (hdc,x,y,lpOld) = 10b. Update + write old.
static void g_setviewportextex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); int y=(int16_t)arg16(c,2),x=(int16_t)arg16(c,3);
    win16_dc_t *dc = dcref(arg16(c,4)); int ox=1,oy=1;
    if (dc){ ox=dc->vex; oy=dc->vey; dc->vex=x; dc->vey=y; } ex_write2(c,off,seg,ox,oy); *ax=1;*dx=0;*argbytes=10;
}
static void g_setviewportorgex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); int y=(int16_t)arg16(c,2),x=(int16_t)arg16(c,3);
    win16_dc_t *dc = dcref(arg16(c,4)); int ox=0,oy=0;
    if (dc){ ox=dc->vox; oy=dc->voy; dc->vox=x; dc->voy=y; } ex_write2(c,off,seg,ox,oy); *ax=1;*dx=0;*argbytes=10;
}
static void g_setwindowextex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); int y=(int16_t)arg16(c,2),x=(int16_t)arg16(c,3);
    win16_dc_t *dc = dcref(arg16(c,4)); int ox=1,oy=1;
    if (dc){ ox=dc->wex; oy=dc->wey; dc->wex=x; dc->wey=y; } ex_write2(c,off,seg,ox,oy); *ax=1;*dx=0;*argbytes=10;
}
static void g_setwindoworgex(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); int y=(int16_t)arg16(c,2),x=(int16_t)arg16(c,3);
    win16_dc_t *dc = dcref(arg16(c,4)); int ox=0,oy=0;
    if (dc){ ox=dc->wox; oy=dc->woy; dc->wox=x; dc->woy=y; } ex_write2(c,off,seg,ox,oy); *ax=1;*dx=0;*argbytes=10;
}
// Offset/Scale window/viewport (GDI.15,16,17,18). Return old org/ext as DWORD.
static void g_offsetwindoworg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y=(int16_t)arg16(c,0),x=(int16_t)arg16(c,1); win16_dc_t *dc=dcref(arg16(c,2)); int ox=0,oy=0;
    if(dc){ ox=dc->wox; oy=dc->woy; dc->wox+=x; dc->woy+=y; } *ax=(uint16_t)ox;*dx=(uint16_t)oy;*argbytes=6;
}
static void g_offsetvieworg(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int y=(int16_t)arg16(c,0),x=(int16_t)arg16(c,1); win16_dc_t *dc=dcref(arg16(c,2)); int ox=0,oy=0;
    if(dc){ ox=dc->vox; oy=dc->voy; dc->vox+=x; dc->voy+=y; } *ax=(uint16_t)ox;*dx=(uint16_t)oy;*argbytes=6;
}
static void g_scalewindowext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int yd=(int16_t)arg16(c,0),yn=(int16_t)arg16(c,1),xd=(int16_t)arg16(c,2),xn=(int16_t)arg16(c,3);
    win16_dc_t *dc=dcref(arg16(c,4)); int ox=1,oy=1;
    if(dc){ ox=dc->wex; oy=dc->wey; if(xd) dc->wex=dc->wex*xn/xd; if(yd) dc->wey=dc->wey*yn/yd; }
    *ax=(uint16_t)ox;*dx=(uint16_t)oy;*argbytes=10;
}
static void g_scaleviewext(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int yd=(int16_t)arg16(c,0),yn=(int16_t)arg16(c,1),xd=(int16_t)arg16(c,2),xn=(int16_t)arg16(c,3);
    win16_dc_t *dc=dcref(arg16(c,4)); int ox=1,oy=1;
    if(dc){ ox=dc->vex; oy=dc->vey; if(xd) dc->vex=dc->vex*xn/xd; if(yd) dc->vey=dc->vey*yn/yd; }
    *ax=(uint16_t)ox;*dx=(uint16_t)oy;*argbytes=10;
}
// LPtoDP (GDI.99) / DPtoLP (GDI.67): (hdc,lppt,count) = 8b. Transform in place.
static void dc_dp2lp(win16_dc_t *dc, int *x, int *y) {
    int vex = dc->vex ? dc->vex : 1, vey = dc->vey ? dc->vey : 1;
    long lx = (long)(*x - dc->vox) * dc->wex;
    long ly = (long)(*y - dc->voy) * dc->wey;
    *x = dc->wox + (int)(lx / vex); *y = dc->woy + (int)(ly / vey);
}
static void g_lptodp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int count=(int16_t)arg16(c,0); uint16_t off=arg16(c,1),seg=arg16(c,2); win16_dc_t *dc=dcref(arg16(c,3));
    *argbytes=8; *dx=0; *ax=1;
    if(!dc || count<0 || count>2048) return;
    for(int i=0;i<count;i++){ int x=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+i*4)); int y=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+i*4+2));
        dc_lp2dp(dc,&x,&y); x86_16_wr16(c,seg,(uint16_t)(off+i*4),(uint16_t)x); x86_16_wr16(c,seg,(uint16_t)(off+i*4+2),(uint16_t)y); }
}
static void g_dptolp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int count=(int16_t)arg16(c,0); uint16_t off=arg16(c,1),seg=arg16(c,2); win16_dc_t *dc=dcref(arg16(c,3));
    *argbytes=8; *dx=0; *ax=1;
    if(!dc || count<0 || count>2048) return;
    for(int i=0;i<count;i++){ int x=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+i*4)); int y=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+i*4+2));
        dc_dp2lp(dc,&x,&y); x86_16_wr16(c,seg,(uint16_t)(off+i*4),(uint16_t)x); x86_16_wr16(c,seg,(uint16_t)(off+i*4+2),(uint16_t)y); }
}
// GetRgnBox (GDI.134): (hRgn,lpRect) = 6b. We do not track region geometry; give
// an empty box + NULLREGION so callers do not divide by an unset rect.
static void g_getrgnbox(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1);
    if(seg||off){ for(int i=0;i<8;i++) x86_16_wr8(c,seg,(uint16_t)(off+i),0); }
    *ax=1; *dx=0; *argbytes=6;   // NULLREGION
}
// ---- GDI object creators (return real GDI handles) ----
static void g_createhatchbrush(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint32_t cr = arg32(c, 0);   /* nStyle = arg16(2) ignored (rendered solid) */
    *ax = (uint16_t)win16_alloc_gdiobj(1, cr); *dx = 0; *argbytes = 6;
}
static void g_createdibpatternbrush(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = (uint16_t)win16_alloc_gdiobj(1, 0xC0C0C0); *dx = 0; *argbytes = 4;
}
static void g_createbitmapindirect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1);
    int w = (int16_t)x86_16_rd16(c,seg,(uint16_t)(off+2));   // BITMAP.bmWidth @2
    int h = (int16_t)x86_16_rd16(c,seg,(uint16_t)(off+4));   // BITMAP.bmHeight @4
    w = (w<1)?1:((w>2048)?2048:w); h = (h<1)?1:((h>2048)?2048:h);
    int obj = win16_alloc_gdiobj(4, 0);
    if(obj){ uint32_t *b=(uint32_t*)kmalloc((uint32_t)w*h*4); if(b){ for(int i=0;i<w*h;i++)b[i]=FB_COLOR(0,0,0);}
             g_gdiobj[obj].pix=b; g_gdiobj[obj].w=w; g_gdiobj[obj].h=h; g_gdiobj[obj].bpp=8; }
    *ax=(uint16_t)(obj?obj:0); *dx=0; *argbytes=4;
}
static void g_creatdiscardablebmp(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int h=(int16_t)arg16(c,0), w=(int16_t)arg16(c,1); /*hdc=arg16(2)*/
    w = (w<1)?1:((w>2048)?2048:w); h = (h<1)?1:((h>2048)?2048:h);
    int obj=win16_alloc_gdiobj(4,0);
    if(obj){ uint32_t *b=(uint32_t*)kmalloc((uint32_t)w*h*4); if(b){for(int i=0;i<w*h;i++)b[i]=FB_COLOR(0,0,0);}
             g_gdiobj[obj].pix=b; g_gdiobj[obj].w=w; g_gdiobj[obj].h=h; g_gdiobj[obj].bpp=8; }
    *ax=(uint16_t)(obj?obj:0); *dx=0; *argbytes=6;
}
static void g_createregion8(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax=(uint16_t)win16_alloc_gdiobj(5,0); *dx=0; *argbytes=8;   // CreateEllipticRgn
}
static void g_createregion4(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax=(uint16_t)win16_alloc_gdiobj(5,0); *dx=0; *argbytes=4;   // CreateEllipticRgnIndirect
}
static void g_createroundrectrgn(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax=(uint16_t)win16_alloc_gdiobj(5,0); *dx=0; *argbytes=12;  // CreateRoundRectRgn
}
// ---- drawing approximations (draw the bounding shape; never a black box) ----
static int ellipse_in(int x, int y, int cx, int cy, int rx, int ry) {
    long dxp=x-cx, dyp=y-cy; return (dxp*dxp*ry*ry + dyp*dyp*rx*rx) <= (long)rx*rx*ry*ry;
}
static void g_ellipse_outline(win16_dc_t *dc, int l, int t, int r, int b, int fill) {
    if (r < l) { int q=l; l=r; r=q; } if (b < t) { int q=t; t=b; b=q; }
    if (r-l>4096 || b-t>4096) return;
    int cx=(l+r)/2, cy=(t+b)/2, rx=(r-l)/2, ry=(b-t)/2; if(rx<1)rx=1; if(ry<1)ry=1;
    for(int yy=t; yy<=b; yy++) for(int xx=l; xx<=r; xx++){
        if(!ellipse_in(xx,yy,cx,cy,rx,ry)) continue;
        if(fill && !dc->brush_null) dc_plot(dc,xx,yy,dc->brush_color);
        // boundary pixel: inside but a 4-neighbour is outside -> pen outline
        if(!ellipse_in(xx-1,yy,cx,cy,rx,ry)||!ellipse_in(xx+1,yy,cx,cy,rx,ry)||
           !ellipse_in(xx,yy-1,cx,cy,rx,ry)||!ellipse_in(xx,yy+1,cx,cy,rx,ry))
            dc_plot(dc,xx,yy,dc->pen_color);
    }
}
static void g_arc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* (hdc,l,t,r,b,xs,ys,xe,ye) */ int b=(int16_t)arg16(c,4),r=(int16_t)arg16(c,5),t=(int16_t)arg16(c,6),l=(int16_t)arg16(c,7);
    win16_dc_t *dc=dcref(arg16(c,8)); if(dc){ dc_lp2dp(dc,&l,&t); dc_lp2dp(dc,&r,&b); g_ellipse_outline(dc,l,t,r,b,0); }
    *ax=1; *dx=0; *argbytes=18;
}
static void g_pie(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int b=(int16_t)arg16(c,4),r=(int16_t)arg16(c,5),t=(int16_t)arg16(c,6),l=(int16_t)arg16(c,7);
    win16_dc_t *dc=dcref(arg16(c,8)); if(dc){ dc_lp2dp(dc,&l,&t); dc_lp2dp(dc,&r,&b); g_ellipse_outline(dc,l,t,r,b,1); }
    *ax=1; *dx=0; *argbytes=18;
}
// PolyPolygon (GDI.450): (hdc,lppt,lpcounts,ncount) = 12b. Outline each sub-poly.
static void g_polypolygon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    int ncnt=(int16_t)arg16(c,0); uint16_t cnt_off=arg16(c,1),cnt_seg=arg16(c,2);
    uint16_t p_off=arg16(c,3),p_seg=arg16(c,4); win16_dc_t *dc=dcref(arg16(c,5));
    *ax=1; *dx=0; *argbytes=12;
    if(!dc || ncnt<1 || ncnt>256) return;
    int base=0;
    for(int k=0;k<ncnt;k++){
        int n=(int16_t)x86_16_rd16(c,cnt_seg,(uint16_t)(cnt_off+k*2)); if(n<2||n>512){ base+=n; continue; }
        int px0=0,py0=0,pxp=0,pyp=0;
        for(int i=0;i<n;i++){
            int x=(int16_t)x86_16_rd16(c,p_seg,(uint16_t)(p_off+(base+i)*4));
            int y=(int16_t)x86_16_rd16(c,p_seg,(uint16_t)(p_off+(base+i)*4+2));
            dc_lp2dp(dc,&x,&y);
            if(i==0){ px0=pxp=x; py0=pyp=y; }
            else { /* bresenham segment */ int x0=pxp,y0=pyp,x1=x,y1=y; int ddx=x1-x0,ddy=y1-y0; int sx=ddx<0?-1:1,sy=ddy<0?-1:1; if(ddx<0)ddx=-ddx; if(ddy<0)ddy=-ddy; int err=ddx-ddy,cxp=x0,cyp=y0; for(;;){ dc_plot(dc,cxp,cyp,dc->pen_color); if(cxp==x1&&cyp==y1)break; int e2=2*err; if(e2>-ddy){err-=ddy;cxp+=sx;} if(e2<ddx){err+=ddx;cyp+=sy;} } pxp=x; pyp=y; }
        }
        { int x0=pxp,y0=pyp,x1=px0,y1=py0; int ddx=x1-x0,ddy=y1-y0; int sx=ddx<0?-1:1,sy=ddy<0?-1:1; if(ddx<0)ddx=-ddx; if(ddy<0)ddy=-ddy; int err=ddx-ddy,cxp=x0,cyp=y0; for(;;){ dc_plot(dc,cxp,cyp,dc->pen_color); if(cxp==x1&&cyp==y1)break; int e2=2*err; if(e2>-ddy){err-=ddy;cxp+=sx;} if(e2<ddx){err+=ddx;cyp+=sy;} } }
        base+=n;
    }
}
// StretchDIBits (GDI.439): decode the DIB + nearest-neighbour blit src->dest rect.
static void g_stretchdibits(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    /* dwRop=arg32(0); iUsage=arg16(2); bmi=arg16(3/4); bits=arg16(5/6);
       sh=arg16(7); sw=arg16(8); ys=arg16(9); xs=arg16(10);
       dh=arg16(11); dw=arg16(12); yd=arg16(13); xd=arg16(14); hdc=arg16(15) */
    uint16_t bmi_off=arg16(c,3),bmi_seg=arg16(c,4);
    uint16_t bits_off=arg16(c,5),bits_seg=arg16(c,6);
    int sh=(int16_t)arg16(c,7), sw=(int16_t)arg16(c,8);
    int ysrc=(int16_t)arg16(c,9), xsrc=(int16_t)arg16(c,10);
    int dh=(int16_t)arg16(c,11), dw=(int16_t)arg16(c,12);
    int yd=(int16_t)arg16(c,13), xd=(int16_t)arg16(c,14);
    win16_dc_t *dc = dcref(arg16(c,15));
    *ax=0; *dx=0; *argbytes=32;
    if(!dc) return;
    uint8_t hdr[64]; for(int i=0;i<64;i++) hdr[i]=x86_16_rd8(c,bmi_seg,(uint16_t)(bmi_off+i));
    uint32_t hdrsize=dib_rd32(hdr); int W,H,bpp,ncolors=0,pal_entry;
    if(hdrsize==12){ W=(int)dib_rd16(hdr+4); H=(int)dib_rd16(hdr+6); bpp=(int)dib_rd16(hdr+10); pal_entry=3; }
    else if(hdrsize>=40 && hdrsize<512){ W=(int)dib_rd32(hdr+4); H=(int)dib_rd32(hdr+8); bpp=(int)dib_rd16(hdr+14); ncolors=(int)dib_rd32(hdr+32); pal_entry=4; }
    else return;
    if(H<0)H=-H;
    if(W<=0||H<=0||W>4096||H>4096) return;
    if(bpp<=8){ int defc=1<<bpp; if(ncolors<=0||ncolors>defc)ncolors=defc; } else ncolors=0;
    int rowbytes=((W*bpp+31)/32)*4; int pal_bytes=ncolors*pal_entry;
    uint32_t total=hdrsize+(uint32_t)pal_bytes+(uint32_t)rowbytes*H;
    uint8_t *buf=(uint8_t*)kmalloc(total); if(!buf) return;
    for(uint32_t i=0;i<total;i++)buf[i]=0;
    for(uint32_t i=0;i<hdrsize;i++) buf[i]=(i<64)?hdr[i]:x86_16_rd8(c,bmi_seg,(uint16_t)(bmi_off+i));
    for(int i=0;i<pal_bytes;i++) buf[hdrsize+i]=x86_16_rd8(c,bmi_seg,(uint16_t)(bmi_off+hdrsize+i));
    uint8_t *bitbase=buf+hdrsize+pal_bytes;
    for(int r=0;r<H;r++){ uint8_t *drow=bitbase+(uint32_t)r*rowbytes; uint16_t srow=(uint16_t)(bits_off+(uint32_t)r*rowbytes);
        for(int i=0;i<rowbytes;i++) drow[i]=x86_16_rd8(c,bits_seg,(uint16_t)(srow+i)); }
    int gw=0,gh=0; uint32_t *pix=decode_dib(buf,total,&gw,&gh); kfree(buf);
    if(!pix) return;
    if(sw<=0)sw=gw;
    if(sh<=0)sh=gh;
    if(dw==0||dh==0){ kfree(pix); *ax=(uint16_t)H; return; }
    int adw=dw<0?-dw:dw, adh=dh<0?-dh:dh;
    for(int yy=0; yy<adh; yy++){
        int syr = ysrc + (yy*sh)/adh;              // source row from bottom of DIB
        int sy = gh - 1 - syr; if(sy<0||sy>=gh) continue;
        for(int xx=0; xx<adw; xx++){
            int sx = xsrc + (xx*sw)/adw; if(sx<0||sx>=gw) continue;
            dc_plot(dc, xd+xx, yd+yy, pix[(uint32_t)sy*gw+sx]);
        }
    }
    kfree(pix); *ax=(uint16_t)H;
}
// ---- USER additions ----
// IsZoomed (USER.272): report not-maximized (safe default).
static void u_iszoomed(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax=0; *dx=0; *argbytes=2;
}
// InvertRect (USER.82): (hdc,lpRect) = 6b. XOR the rect pixels (visible feedback).
static void u_invertrect(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t off=arg16(c,0),seg=arg16(c,1); win16_dc_t *dc=dcref(arg16(c,2));
    *ax=1; *dx=0; *argbytes=6;
    if(!dc || !(seg||off)) return;
    int l=(int16_t)x86_16_rd16(c,seg,off), t=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+2));
    int r=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+4)), b=(int16_t)x86_16_rd16(c,seg,(uint16_t)(off+6));
    if(r<l){int q=l;l=r;r=q;} if(b<t){int q=t;t=b;b=q;}
    if(r-l>4096||b-t>4096) return;
    for(int yy=t;yy<b;yy++) for(int xx=l;xx<r;xx++){ uint32_t p=blt_get_pixel(dc,xx,yy); dc_plot(dc,xx,yy,(~p)&0x00FFFFFF); }
}

// ---- SHELL.DLL by-name coverage (#188 pass). Win3.1 apps import these by NAME.
// argbytes per the Win16 prototypes; safe returns (success sentinels). Additive:
// only fires on a named SHELL import that would otherwise be UNIMPL.
static void sh_shellexecute(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 33; *dx = 0; *argbytes = 20;   // HINSTANCE > 32 = "succeeded"
}
static void sh_findexecutable(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 33; *dx = 0; *argbytes = 12;
}
static void sh_dragacceptfiles(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 4;      // void
}
static void sh_dragqueryfile(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    // DragQueryFile(hDrop,iFile,lpsz,cch): with no drop we report 0 files / write "".
    uint16_t sz_off=arg16(c,1),sz_seg=arg16(c,2);
    if (sz_seg || sz_off) x86_16_wr8(c, sz_seg, sz_off, 0);
    *ax = 0; *dx = 0; *argbytes = 10;
}
static void sh_dragfinish(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 2;
}
static void sh_dragquerypoint(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 6;      // FALSE (not in client area)
}
static void sh_extracticon(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 0; *dx = 0; *argbytes = 8;      // no icon
}
static void sh_shellabout(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)c; *ax = 1; *dx = 0; *argbytes = 12;     // IDOK
}

// ===========================================================================
// OLESVR: OLE 1.0 server registration ordinals (task #407, 2026-07-08). Word
// 2.0 for Windows registers itself as an OLE server during WinMain startup via
// OLESVR.#2 (OleRegisterServer); previously OLESVR was entirely absent from
// both g_api_table and g_stub_table, so every OLESVR call fell through to the
// generic UNIMPL path at the bottom of win16_api_dispatch (ax=0, dx=0,
// argbytes=0). Zero argbytes on a Pascal far call desyncs the caller's stack
// -- the same UNIMPL-argbytes-0 class of bug documented throughout this file
// -- which is why Word's registration call returned garbage and Word decided
// its "session is too complex" and self-terminated before ever creating its
// main window.
//
// We do not implement real cross-process OLE 1.0 IPC: there is no second OLE
// client/server app running concurrently in this environment for Word to talk
// to. Word only needs the registration bookkeeping to report success with a
// stable, revocable handle. This mirrors WINE's own OLESVR.DLL16
// implementation (dlls/olesvr.dll16/olesvr.c), whose header comment literally
// reads "At the moment, these are only empty stubs": it allocates an
// incrementing LHSERVER/LHSERVERDOC handle and unconditionally returns
// OLE_OK, never actually invoking the server's callback vtable. Ordinal
// numbers and Pascal argbyte counts below come from WINE's
// dlls/olesvr.dll16/olesvr.dll16.spec (the same class of source this file
// already cites for GDI/USER/KERNEL16 argbyte counts elsewhere), not guesses:
//   2  pascal OleRegisterServer(str ptr ptr word word)
//   3  pascal OleRevokeServer(long)
//   4  pascal OleBlockServer(long)
//   5  pascal OleUnblockServer(long ptr)
//   6  pascal OleRegisterServerDoc(long str ptr ptr)
//   7  pascal OleRevokeServerDoc(long)
//   8  pascal OleRenameServerDoc(long str)
//   9  pascal OleRevertServerDoc(long)
//   10 pascal OleSavedServerDoc(long)
// LHSERVER/LHSERVERDOC are LONG (32-bit) handles; every handle this
// interpreter hands out fits in the low word (high word always 0 on both the
// way out and the way back in), which keeps the code simple without breaking
// any real caller (Word only ever round-trips the exact value we gave it).
//
// Handle model follows the SAME fixed-array "used flag, index+1 = handle"
// idiom already used for menu handles (menu_alloc/menu_get, WIN16_MAX_MENUS
// above) rather than inventing a new scheme.
// ===========================================================================
// A small subset of the real OLESTATUS enum (WINE dlls/olesvr.dll16/olesvr.c):
// only the values this file actually returns.
#define OLE_OK16           0   // OLE_OK
#define OLE_ERROR_MEMORY16 4   // OLE_ERROR_MEMORY
#define OLE_ERROR_HANDLE16 18  // OLE_ERROR_HANDLE

typedef struct {
    int      used;
    uint16_t hinst;            // caller's HINSTANCE16 (debug only)
    uint16_t use;               // OLE_SERVER_USE single/multi (debug only)
    uint16_t srv_off, srv_seg;  // caller's LPOLESERVER vtable ptr (debug only)
    char     classname[40];
} win16_oleserver_t;
#define WIN16_MAX_OLESERVERS 8
static win16_oleserver_t g_oleservers[WIN16_MAX_OLESERVERS];

static uint16_t oleserver_alloc(void) {
    for (int i = 0; i < WIN16_MAX_OLESERVERS; i++) if (!g_oleservers[i].used) {
        g_oleservers[i].used = 1;
        return (uint16_t)(i + 1);
    }
    return 0;
}
static win16_oleserver_t *oleserver_get(uint16_t h) {
    if (!h || h > WIN16_MAX_OLESERVERS || !g_oleservers[h - 1].used) return 0;
    return &g_oleservers[h - 1];
}

typedef struct {
    int      used;
    uint16_t server;   // owning LHSERVER (low word)
    char     docname[64];
} win16_oleserverdoc_t;
#define WIN16_MAX_OLESERVERDOCS 8
static win16_oleserverdoc_t g_oleserverdocs[WIN16_MAX_OLESERVERDOCS];

static uint16_t oleserverdoc_alloc(void) {
    for (int i = 0; i < WIN16_MAX_OLESERVERDOCS; i++) if (!g_oleserverdocs[i].used) {
        g_oleserverdocs[i].used = 1;
        return (uint16_t)(i + 1);
    }
    return 0;
}
static win16_oleserverdoc_t *oleserverdoc_get(uint16_t h) {
    if (!h || h > WIN16_MAX_OLESERVERDOCS || !g_oleserverdocs[h - 1].used) return 0;
    return &g_oleserverdocs[h - 1];
}

// OLESVR.2 OleRegisterServer(LPCSTR name, LPOLESERVER serverStruct,
//   LHSERVER FAR *hRet, HINSTANCE16 hServer, OLE_SERVER_USE use) =
//   str(4) + ptr(4) + ptr(4) + word(2) + word(2) = 16 argbytes.
static void os_registerserver(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t use       = arg16(c, 0);
    uint16_t hinst      = arg16(c, 1);
    uint16_t hret_off   = arg16(c, 2), hret_seg  = arg16(c, 3);
    uint16_t srv_off    = arg16(c, 4), srv_seg   = arg16(c, 5);
    uint16_t name_off   = arg16(c, 6), name_seg  = arg16(c, 7);
    *argbytes = 16; *dx = 0;

    char classname[40];
    rd_far_cstr(c, name_seg, name_off, classname, sizeof(classname));

    uint16_t h = oleserver_alloc();
    if (!h) {
        kprintf("[OLESVR] OleRegisterServer '%s': server table full -> OLE_ERROR_MEMORY\n", classname);
        *ax = OLE_ERROR_MEMORY16; return;  // table full: fail cleanly
    }
    win16_oleserver_t *s = &g_oleservers[h - 1];
    s->hinst = hinst; s->use = use; s->srv_off = srv_off; s->srv_seg = srv_seg;
    { int i = 0; for (; classname[i] && i < (int)sizeof(s->classname) - 1; i++) s->classname[i] = classname[i]; s->classname[i] = 0; }

    if (hret_seg || hret_off) {
        x86_16_wr16(c, hret_seg, (uint16_t)(hret_off + 0), h);  // LHSERVER low word
        x86_16_wr16(c, hret_seg, (uint16_t)(hret_off + 2), 0);  // LHSERVER high word
    }
    win16_trace("OLESVR OleRegisterServer '%s' hinst=%04x use=%u -> hServer=%u\n",
                classname, hinst, use, h);
    kprintf("[OLESVR] OleRegisterServer '%s' hinst=%04x use=%u -> hServer=%u\n",
            classname, hinst, use, h);
    *ax = OLE_OK16;
}

// OLESVR.3 OleRevokeServer(LHSERVER hServer) = long(4) = 4 argbytes.
static void os_revokeserver(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0);   // LHSERVER low word
    (void)arg16(c, 1);          // LHSERVER high word (always 0 for our handles)
    *argbytes = 4; *dx = 0;
    win16_oleserver_t *s = oleserver_get(h);
    if (!s) { *ax = OLE_ERROR_HANDLE16; return; }
    s->used = 0;
    win16_trace("OLESVR OleRevokeServer hServer=%u\n", h);
    kprintf("[OLESVR] OleRevokeServer hServer=%u\n", h);
    *ax = OLE_OK16;
}

// OLESVR.4 OleBlockServer(LHSERVER hServer) = long(4) = 4 argbytes. We never
// actually queue/dispatch cross-process OLE messages, so blocking is a
// pure success no-op (matches WINE's own stub behaviour).
static void os_blockserver(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); (void)arg16(c, 1);
    *argbytes = 4; *ax = OLE_OK16; *dx = 0;
}

// OLESVR.5 OleUnblockServer(LHSERVER hServer, BOOL16 FAR *lpbBlocked) =
//   long(4) + ptr(4) = 8 argbytes. Nothing is ever left blocked here.
static void os_unblockserver(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t blk_off = arg16(c, 0), blk_seg = arg16(c, 1);
    (void)arg16(c, 2); (void)arg16(c, 3);   // hServer
    *argbytes = 8; *dx = 0;
    if (blk_seg || blk_off) x86_16_wr16(c, blk_seg, blk_off, 0);  // FALSE
    *ax = OLE_OK16;
}

// OLESVR.6 OleRegisterServerDoc(LHSERVER hServer, LPCSTR docname,
//   LPOLESERVERDOC document, LHSERVERDOC FAR *hRet) =
//   long(4) + str(4) + ptr(4) + ptr(4) = 16 argbytes.
static void os_registerserverdoc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t hret_off = arg16(c, 0), hret_seg = arg16(c, 1);
    uint16_t doc_off  = arg16(c, 2), doc_seg  = arg16(c, 3);  // LPOLESERVERDOC (no real IPC target)
    uint16_t name_off = arg16(c, 4), name_seg = arg16(c, 5);
    uint16_t hserver  = arg16(c, 6); (void)arg16(c, 7);
    (void)doc_off; (void)doc_seg;
    *argbytes = 16; *dx = 0;

    if (!oleserver_get(hserver)) { *ax = OLE_ERROR_HANDLE16; return; }
    uint16_t h = oleserverdoc_alloc();
    if (!h) { *ax = OLE_ERROR_MEMORY16; return; }
    win16_oleserverdoc_t *d = &g_oleserverdocs[h - 1];
    d->server = hserver;
    rd_far_cstr(c, name_seg, name_off, d->docname, sizeof(d->docname));

    if (hret_seg || hret_off) {
        x86_16_wr16(c, hret_seg, (uint16_t)(hret_off + 0), h);
        x86_16_wr16(c, hret_seg, (uint16_t)(hret_off + 2), 0);
    }
    win16_trace("OLESVR OleRegisterServerDoc hServer=%u doc='%s' -> hServerDoc=%u\n",
                hserver, d->docname, h);
    *ax = OLE_OK16;
}

// OLESVR.7 OleRevokeServerDoc(LHSERVERDOC hServerDoc) = long(4) = 4 argbytes.
static void os_revokeserverdoc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t h = arg16(c, 0); (void)arg16(c, 1);
    *argbytes = 4; *dx = 0;
    win16_oleserverdoc_t *d = oleserverdoc_get(h);
    if (!d) { *ax = OLE_ERROR_HANDLE16; return; }
    d->used = 0;
    win16_trace("OLESVR OleRevokeServerDoc hServerDoc=%u\n", h);
    *ax = OLE_OK16;
}

// OLESVR.8 OleRenameServerDoc(LHSERVERDOC hDoc, LPCSTR newName) =
//   long(4) + str(4) = 8 argbytes.
static void os_renameserverdoc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    uint16_t name_off = arg16(c, 0), name_seg = arg16(c, 1);
    uint16_t h = arg16(c, 2); (void)arg16(c, 3);
    *argbytes = 8; *dx = 0;
    win16_oleserverdoc_t *d = oleserverdoc_get(h);
    if (!d) { *ax = OLE_ERROR_HANDLE16; return; }
    rd_far_cstr(c, name_seg, name_off, d->docname, sizeof(d->docname));
    *ax = OLE_OK16;
}

// OLESVR.9 OleRevertServerDoc(LHSERVERDOC hDoc) = long(4) = 4 argbytes.
static void os_revertserverdoc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); (void)arg16(c, 1);
    *argbytes = 4; *ax = OLE_OK16; *dx = 0;
}

// OLESVR.10 OleSavedServerDoc(LHSERVERDOC hDoc) = long(4) = 4 argbytes.
static void os_savedserverdoc(x86_16_cpu_t *c, uint16_t *ax, uint16_t *dx, uint16_t *argbytes) {
    (void)arg16(c, 0); (void)arg16(c, 1);
    *argbytes = 4; *ax = OLE_OK16; *dx = 0;
}

static const win16_api_entry_t g_api_table[] = {
    { "KERNEL", 91, "INITTASK",          k_inittask },
    // (#278 Word6) CORRECTED Win3.1 SHELL.DLL registration ordinals (per WINE
    // shell.dll16.spec): 1 RegOpenKey 2 RegCreateKey 3 RegCloseKey 4 RegDeleteKey
    // 5 RegSetValue 6 RegQueryValue 7 RegEnumKey. The pass-7 table swapped 4-7
    // (had 5=Query 6=Set 7=Delete), so values never round-tripped -> Word declared
    // the registration database invalid.
    { "SHELL", 1, "REGOPENKEY",          sh_regopenkey },     // (#278 Word6)
    { "SHELL", 2, "REGCREATEKEY",        sh_regcreatekey },   // (#278 Word6)
    { "SHELL", 3, "REGCLOSEKEY",         sh_regclosekey },    // (#278 Word6)
    { "SHELL", 4, "REGDELETEKEY",        sh_regdeletekey },   // (#278 Word6)
    { "SHELL", 5, "REGSETVALUE",         sh_regsetvalue },    // (#278 Word6)
    { "SHELL", 6, "REGQUERYVALUE",       sh_regqueryvalue },  // (#278 Word6)
    { "SHELL", 7, "REGENUMKEY",          sh_regenumkey },     // (#278 Word6)
    // (#391 Photoshop 2.5.1, 2026-07-09) Photoshop imports SHELL Drag*
    // ordinals BY ORDINAL (not by name), so the existing by-name-only
    // entries below (ordinal=0, matched only for non-ordinal imports) never
    // fire for it: find_handler() requires e->ordinal == im->ordinal for an
    // ordinal import. Photoshop calls SHELL.#9 (DragAcceptFiles) right after
    // creating its main frame window; hitting the generic UNIMPL path there
    // used argbytes=0 instead of the real 4, desyncing the Pascal stack by 4
    // bytes (the same UNIMPL-argbytes-0 class documented throughout this
    // file). Fixing this is CORRECT and NECESSARY regardless (a real bug),
    // but an A/B kernel test proved it is NOT SUFFICIENT to fix Photoshop:
    // the very next call_far into the app's own wndproc (0307:1a88, its
    // WM_CREATE handler) still fails to return cleanly with this ordinal
    // fixed, landing in cs=0000 via a genuine app-side CALL FAR through a
    // zeroed far-pointer slot at es:bx+0x30 (see #391 golden/blame.md for
    // the full trace) -- a separate, independent crash, not caused by this
    // stack-desync class. Ordinal numbers + argbyte counts are from WINE's
    // dlls/shell.dll16/shell.dll16.spec: 9 DragAcceptFiles(word,word)=4,
    // 11 DragQueryFile(word,s_word,ptr,s_word)=10, 12 DragFinish(word)=2,
    // 13 DragQueryPoint(word,ptr)=6 (10 is absent from the spec). Reuses the
    // EXISTING sh_drag* handlers below (by-name coverage, #188 pass) rather
    // than adding new ones, per the reuse-not-reinvent rule.
    { "SHELL", 9, "DRAGACCEPTFILES",     sh_dragacceptfiles },  // (#391)
    { "SHELL", 11, "DRAGQUERYFILE",      sh_dragqueryfile },    // (#391)
    { "SHELL", 12, "DRAGFINISH",         sh_dragfinish },       // (#391)
    { "SHELL", 13, "DRAGQUERYPOINT",     sh_dragquerypoint },   // (#391)
    { "KERNEL", 30, "WAITEVENT",         k_waitevent },
    { "KERNEL",  3, "GETVERSION",        k_getversion },
    { "KERNEL",  4, "LOCALINIT",         k_localinit },
    { "KERNEL", 57, "GETPROFILEINT",     k_getprofileint },
    { "KERNEL", 58, "GETPROFILESTRING",  k_getprofilestr },
    { "KERNEL", 59, "WRITEPROFILESTRING", k_writeprofilestr },
    { "KERNEL", 47, "GETMODULEHANDLE",   k_getmodulehandle },
    { "KERNEL", 49, "GETMODULEFILENAME", k_getmodulefilename },
    { "KERNEL", 48, "GETMODULEUSAGE",    k_getmoduleusage },
    { "KERNEL", 23, "LOCKSEGMENT",       k_locksegment },
    { "KERNEL", 24, "UNLOCKSEGMENT",     k_locksegment },
    { "KERNEL", 15, "GLOBALALLOC",       k_globalalloc },
    { "KERNEL", 60, "FINDRESOURCE",      k_findresource },
    { "KERNEL", 61, "LOADRESOURCE",      k_loadresource },
    { "KERNEL", 64, "ACCESSRESOURCE",   k_accessresource },   // (#Excel4)
    { "KERNEL", 62, "LOCKRESOURCE",      k_lockresource },
    { "KERNEL", 63, "FREERESOURCE",      k_freeresource },
    { "KERNEL", 65, "SIZEOFRESOURCE",    k_sizeofresource },
    { "KERNEL", 18, "GLOBALLOCK",        k_globallock },
    { "KERNEL", 29, "GLOBALMASTERHANDLE", k_globalmasterhandle }, // (#278 Word6)
    { "KERNEL", 199, "SELECTORACCESSRIGHTS", k_selectoraccessrights }, // (#278)
    { "KERNEL", 19, "GLOBALUNLOCK",      k_globalunlock },
    { "KERNEL", 17, "GLOBALFREE",        k_globalfree },
    { "KERNEL", 25, "GLOBALCOMPACT",     k_globalcompact },
    { "KERNEL", 169, "GETFREESPACE",     k_getfreespace },      // (#EP3 Fuji Golf)
    { "KERNEL", 132, "GETWINFLAGS",      k_getwinflags },      // (#Excel4)
    { "KERNEL", 20, "GLOBALSIZE",        k_globalsize },        // (#188 Word6)
    { "KERNEL", 166, "WINEXEC",          k_winexec },           // (#188 Word6)
    { "KERNEL",  5, "LOCALALLOC",        k_localalloc },
    { "KERNEL",  6, "LOCALREALLOC",      k_localrealloc },
    { "KERNEL",  7, "LOCALFREE",         k_localfree },
    { "KERNEL",  8, "LOCALLOCK",         k_locallock },
    { "KERNEL",  9, "LOCALUNLOCK",       k_localunlock },
    { "KERNEL", 10, "LOCALSIZE",         k_localsize },
    { "KERNEL", 16, "GLOBALREALLOC",     k_globalrealloc },
    { "KERNEL",  1, "FATALEXIT",         k_word1_ok },
    { "KERNEL", 21, "GLOBALHANDLE",      k_globalhandle },     // (#289 OLE2)
    { "KERNEL", 334, "GETSELECTORLIMIT", k_getselectorlimit }, // (#289 OLE2)
    { "KERNEL", 350, "_HWRITE",          k_kernel350 },        // (#289 OLE2 _hwrite)
    { "KERNEL", 349, "_HREAD",           k_hread },            // (#289 OLE2 _hread)
    { "KERNEL", 346, "KERNEL346",        k_kernel346 },        // (#289 OLE2)
    { "KERNEL", 347, "KERNEL347",        k_kernel347 },        // (#289 OLE2)
    { "KERNEL", 337, "SETSELECTORLIMIT", k_setselectorlimit }, // (#289 OLE2)
    { "KERNEL", 51, "MAKEPROCINSTANCE",  k_makeprocinstance },
    { "KERNEL", 52, "FREEPROCINSTANCE",  k_freeprocinstance },
    { "KERNEL", 74, "OPENFILE",          k_openfile },         // real KERNEL.74 = OpenFile (#148)
    { "KERNEL", 36, "GETCURRENTTASK",    k_getcurrenttask },  // real ordinal (#152)
    { "KERNEL",  0, "GETCURRENTTASK",    k_getcurrenttask },   // also by name
    { "KERNEL", 81, "_LCLOSE",           k_lclose },
    { "KERNEL", 82, "_LREAD",            k_lread },
    { "KERNEL", 83, "_LCREAT",          k_lcreat },
    { "KERNEL", 84, "_LLSEEK",           k_llseek },
    { "KERNEL", 85, "_LOPEN",            k_lopen },
    { "KERNEL", 86, "_LWRITE",           k_lwrite },
    { "KERNEL", 88, "LSTRCPY",           k_lstrcpy },
    { "KERNEL", 89, "LSTRCAT",           k_lstrcat },
    { "KERNEL", 90, "LSTRLEN",           k_lstrlen },
    { "KERNEL", 94, "LSTRCMP",           k_lstrcmp },
    { "KERNEL", 95, "LOADLIBRARY",       k_loadlibrary },
    { "KERNEL", 96, "FREELIBRARY",       k_freelibrary },
    { "KERNEL", 50, "GETPROCADDRESS",    k_getprocaddress },
    { "KERNEL", 102, "DOS3CALL",         k_dos3call },
    { "KERNEL", 107, "SETERRORMODE",     k_seterrormode },
    { "KERNEL", 115, "OUTPUTDEBUGSTRING",k_outputdebugstr },
    { "KERNEL", 127, "GETPRIVATEPROFILEINT",    k_getprivprofileint },
    { "KERNEL", 128, "GETPRIVATEPROFILESTRING", k_getprivprofilestr },
    { "KERNEL", 129, "WRITEPRIVATEPROFILESTRING", k_writeprivprofile },
    { "KERNEL", 131, "GETDOSENVIRONMENT", k_getdosenv },
    { "KERNEL", 37, "GETCURRENTPDB",     k_getcurrentpdb },
    { "KERNEL", 134, "GETWINDOWSDIRECTORY", k_getwindir },
    { "KERNEL", 135, "GETSYSTEMDIRECTORY", k_getsysdir },
    { "KERNEL", 136, "GETDRIVETYPE",       k_getdrivetype },   // (#257)
    { "KERNEL", 137, "FATALAPPEXIT",     k_fatalappexit },
    { "KERNEL", 170, "ALLOCCSTODSALIAS", k_alloc_alias },
    { "KERNEL", 171, "ALLOCDSTOCSALIAS", k_alloc_alias },
    { "KERNEL", 172, "ALLOCSELECTOR",    k_alloc_alias },
    { "KERNEL", 176, "FREESELECTOR",     k_freeselector },
    { "KERNEL", 177, "PRESTOCHANGOSELECTOR", k_prestochango },
    { "KERNEL", 335, "GETSELECTORBASE",  k_getselectorbase },  // (#289 b468)
    { "KERNEL", 336, "SETSELECTORBASE",  k_setselectorbase },  // (#289 b468)
    { "KERNEL", 55, "CATCH",             k_far1_zero },
    { "KERNEL", 56, "THROW",             k_throw },
    { "WIN87EM", 1, 0,                   k_win87em_init },
    { "WEP4UTIL", 2, 0,                 k_wep_ok },
    { "WEPUTIL",  2, 0,                 k_wep_ok },

    // ---- USER ----
    { "USER",    1, "MESSAGEBOX",        u_messagebox },
    { "USER",   87, "DIALOGBOX",         u_dialogbox },
    { "USER",   88, "ENDDIALOG",         u_enddialog },
    { "USER",   89, "CREATEDIALOG",      u_createdialog },
    { "USER",   90, "ISDIALOGMESSAGE",   u_isdialogmessage },
    { "USER",   91, "GETDLGITEM",        u_getdlgitem },
    { "USER",   92, "SETDLGITEMTEXT",    u_setdlgitemtext },
    { "USER",   93, "GETDLGITEMTEXT",    u_getdlgitemtext },
    { "USER",   94, "SETDLGITEMINT",     u_setdlgitemint },
    { "USER",   95, "GETDLGITEMINT",     u_getdlgitemint },
    { "USER",  101, "SENDDLGITEMMESSAGE", u_senddlgitemmessage },
    { "USER",   96, "CHECKRADIOBUTTON",  u_checkradiobutton },
    { "USER",   97, "CHECKDLGBUTTON",    u_checkdlgbutton },
    { "USER",   98, "ISDLGBUTTONCHECKED",u_isdlgbuttonchecked },
    { "USER",  103, "MAPDIALOGRECT",     u_mapdialogrect },
    { "USER",  277, "GETDLGCTRLID",      u_getdlgctrlid },
    { "USER",  308, "DEFDLGPROC",        u_defdlgproc },
    { "USER",  239, "DIALOGBOXPARAM",    u_dialogboxparam },
    { "USER",  240, "DIALOGBOXINDIRECTPARAM", u_dialogboxindirectparam },
    { "USER",  218, "DIALOGBOXINDIRECT", u_dialogboxindirect },
    { "USER",  241, "CREATEDIALOGPARAM", u_createdialogparam },
    { "USER",  242, "CREATEDIALOGINDIRECTPARAM", u_dialogboxindirectparam },
    { "USER",  219, "CREATEDIALOGINDIRECT", u_dialogboxindirect },
    { "USER",    5, "INITAPP",           u_initapp },
    { "USER",    6, "POSTQUITMESSAGE",   u_postquit },
    { "USER",   10, "SETTIMER",          u_settimer },
    { "USER",   12, "KILLTIMER",         u_killtimer },
    { "USER",   22, "SETFOCUS",          u_word1 },
    { "USER",   23, "GETFOCUS",          u_getfocus },
    { "USER",   60, "GETACTIVEWINDOW",   u_getfocus },
    { "USER",   32, "GETWINDOWRECT",     u_getwindowrect },
    { "USER",   33, "GETCLIENTRECT",     u_getclientrect },
    { "USER",   39, "BEGINPAINT",        u_beginpaint },
    { "USER",   40, "ENDPAINT",          u_endpaint },
    { "USER",   41, "CREATEWINDOW",      u_createwindow },
    { "USER",  452, "CREATEWINDOWEX",    u_createwindowex },
    { "USER",  102, "ADJUSTWINDOWRECT",  u_adjustwindowrect },   // (#152)
    { "USER",  454, "ADJUSTWINDOWRECTEX",u_adjustwindowrectex }, // (#152)
    { "USER",   42, "SHOWWINDOW",        u_showwindow },
    { "USER",   36, "GETWINDOWTEXT",     u_getwindowtext },
    { "USER",   53, "DESTROYWINDOW",     u_destroywindow },
    { "USER",   56, "MOVEWINDOW",        u_movewindow },
    { "USER",   57, "REGISTERCLASS",     u_registerclass },
    { "USER",  110, "POSTMESSAGE",       u_postmessage },
    { "USER",  154, "CHECKMENUITEM",     u_menuitem3 },
    { "USER",  155, "ENABLEMENUITEM",    u_menuitem3 },
    { "USER",  157, "GETMENU",           u_getmenu },
    { "USER",  158, "SETMENU",           u_setmenu },            // (#152)
    { "USER",  177, "LOADACCELERATORS",  u_loadaccel },
    { "USER",  178, "TRANSLATEACCELERATOR", u_translateaccel },
    { "USER",  232, "SETWINDOWPOS",      u_setwindowpos },
    { "USER",   66, "GETDC",             u_getdc },
    { "USER",   68, "RELEASEDC",         u_releasedc },
    { "USER",   69, "SETCURSOR",         u_setcursor },
    { "USER",   81, "FILLRECT",          u_fillrect },
    { "USER",   61, "SCROLLWINDOW",      u_scrollwindow },
    { "USER",  319, "SCROLLWINDOWEX",    u_scrollwindowex },
    { "USER",  107, "DEFWINDOWPROC",     u_defwindowproc },
    { "USER",  108, "GETMESSAGE",        u_getmessage },
    { "USER",  109, "PEEKMESSAGE",       u_peekmessage },
    { "USER",  112, "WAITMESSAGE",       u_waitmessage },
    { "USER",  113, "TRANSLATEMESSAGE",  u_translatemessage },
    { "USER",  114, "DISPATCHMESSAGE",   u_dispatchmessage },
    { "USER",  124, "UPDATEWINDOW",      u_updatewindow },
    { "USER",  125, "INVALIDATERECT",    u_invalidaterect },
    { "USER",  173, "LOADCURSOR",        u_loadcursor },
    { "USER",  174, "LOADICON",          u_loadicon },
    { "USER",   84, "DRAWICON",          u_drawicon },           // (#192)
    { "USER",  175, "LOADBITMAP",        u_loadbitmap },
    { "USER",  179, "GETSYSTEMMETRICS",  u_getsystemmetrics },
    { "USER",   13, "GETTICKCOUNT",      u_gettickcount },
    { "USER",   15, "GETCURRENTTIME",    u_gettickcount },
    { "USER",   71, "SHOWCURSOR",        u_showcursor },
    { "USER",   78, "INFLATERECT",       u_inflaterect },
    { "USER",   85, "DRAWTEXT",          u_drawtext },
    { "USER",  133, "GETWINDOWWORD",     u_getwindowword },
    { "USER",  134, "SETWINDOWWORD",     u_setwindowword },
    { "USER",  135, "GETWINDOWLONG",     u_getwindowlong },      // (#256/#188)
    { "USER",  136, "SETWINDOWLONG",     u_setwindowlong },      // (#256/#188)
    { "USER",  122, "CALLWINDOWPROC",    u_callwindowproc },     // (#256/#188)
    { "USER",  180, "GETSYSCOLOR",       u_getsyscolor },
    { "USER",   18, "SETCAPTURE",        u_setcapture },
    { "USER",   19, "RELEASECAPTURE",    u_releasecapture },
    { "USER",   37, "SETWINDOWTEXT",     u_setwindowtext },
    { "USER",   46, "GETPARENT",         u_getparent },
    { "USER",   47, "ISWINDOW",          u_iswindow },
    { "USER",   50, "FINDWINDOW",        u_findwindow },
    { "USER",   83, "FRAMERECT",         u_framerect },
    { "USER",  106, "GETKEYSTATE",       u_getkeystate },
    { "USER",  249, "GETASYNCKEYSTATE",  u_getasynckeystate },
    { "USER",  111, "SENDMESSAGE",       u_sendmessage },
    { "USER",  150, "LOADMENU",          u_loadmenu },
    { "USER",  176, "LOADSTRING",        u_loadstring },
    { "USER",  420, "WSPRINTF",          u_wsprintf },
    { "USER",  421, "WVSPRINTF",         u_wvsprintf },
    { "USER",  282, "SELECTPALETTE",     u_selectpalette },
    { "USER",  283, "REALIZEPALETTE",    u_realizepalette },
    { "USER",  286, "GETDESKTOPWINDOW",  u_getdesktopwindow },
    { "USER",   31, "ISICONIC",          u_isiconic },
    { "USER",  104, "MESSAGEBEEP",       u_messagebeep },
    { "USER",   67, "GETWINDOWDC",       u_getwindowdc },
    { "USER",  160, "DRAWMENUBAR",       u_drawmenubar },
    // Menu API (programmatic build + query)
    { "USER",  151, "CREATEMENU",        u_createmenu },
    { "USER",  415, "CREATEPOPUPMENU",   u_createpopupmenu },
    { "USER",  152, "DESTROYMENU",       u_destroymenu },
    { "USER",  411, "APPENDMENU",        u_appendmenu },
    { "USER",  410, "INSERTMENU",        u_insertmenu },
    { "USER",  414, "MODIFYMENU",        u_modifymenu },
    { "USER",  413, "DELETEMENU",        u_deletemenu },
    { "USER",  412, "REMOVEMENU",        u_removemenu },
    { "USER",  159, "GETSUBMENU",        u_getsubmenu },
    { "USER",  263, "GETMENUITEMCOUNT",  u_getmenuitemcount },
    { "USER",  264, "GETMENUITEMID",     u_getmenuitemid },
    { "USER",  161, "GETMENUSTRING",     u_getmenustring },
    { "USER",  266, "SETMESSAGEQUEUE",   u_setmessagequeue },        // (#278 Word6)
    { "USER",  145, "REGISTERCLIPBOARDFORMAT", u_registerclipboardformat }, // (#278 Word6)
    { "USER",  250, "GETMENUSTATE",      u_getmenustate },
    { "USER",  358, "ISMENU",            u_ismenu },
    { "USER",  162, "HILITEMENUITEM",    u_hilitemenuitem },
    { "USER",  156, "GETSYSTEMMENU",     u_getsystemmenu },
    { "USER",  416, "TRACKPOPUPMENU",    u_trackpopupmenu },
    // Message API extras
    { "USER",  116, "POSTAPPMESSAGE",    u_postappmessage },
    { "USER",  118, "REGISTERWINDOWMESSAGE", u_registerwindowmessage },
    { "USER",  119, "GETMESSAGEPOS",     u_getmessagepos },
    { "USER",  120, "GETMESSAGETIME",    u_getmessagetime },
    { "USER",  334, "GETQUEUESTATUS",    u_getqueuestatus },
    { "USER",  335, "GETINPUTSTATE",     u_getinputstate },
    { "USER",  115, "REPLYMESSAGE",      u_replymessage },
    { "USER",  163, "CREATECARET",       u_createcaret },
    { "USER",  164, "DESTROYCARET",      u_destroycaret },
    { "USER",  165, "SETCARETPOS",       u_setcaretpos },
    { "USER",  166, "HIDECARET",         u_hidecaret },
    { "USER",  167, "SHOWCARET",         u_showcaret },
    { "USER",  168, "SETCARETBLINKTIME", u_setcaretblink },
    { "USER",  169, "GETCARETBLINKTIME", u_getcaretblink },
    { "USER",  190, "GETSYSMODALWINDOW", u_user190_word6 },  // (#278 P59) 8-byte call in Word6, was argbytes=0 -> stack desync
    { "USER",  192, "INSENDMESSAGE",     u_insendmessage },
    { "USER",  288, "GETMESSAGEEXTRAINFO", u_getmessageextrainfo },
    { "USER",  171, "WINHELP",           u_winhelp },
    { "USER",  243, "GETDIALOGBASEUNITS", u_getdialogbaseunits },
    { "USER",  403, "UNREGISTERCLASS",   u_unregisterclass },
    { "USER",  404, "GETCLASSINFO",      u_getclassinfo },
    { "USER",   72, "SETRECT",           u_setrect },
    { "USER",   73, "SETRECTEMPTY",      u_setrectempty },
    { "USER",   74, "COPYRECT",          u_copyrect },
    { "USER",   75, "ISRECTEMPTY",       u_isrectempty },
    { "USER",   76, "PTINRECT",          u_ptinrect },
    { "USER",   77, "OFFSETRECT",        u_offsetrect },
    { "USER",   79, "INTERSECTRECT",     u_intersectrect },
    { "USER",  472, "ANSINEXT",          u_ansinext },
    { "USER",  473, "ANSIPREV",          u_ansiprev },          // (#188 Word6)
    { "USER",  431, "ANSIUPPER",         u_ansiupper },         // (#188 Word6)
    { "USER",  435, "ISCHARUPPER",       u_ischarupper },       // (#278 Word6)
    { "USER",  436, "ISCHARLOWER",       u_ischarlower },       // (#278 Word6)
    { "USER",  471, "LSTRCMPI",          u_lstrcmpi },          // (#278 Word6)

    // ---- GDI ----
    { "GDI",     1, "SETBKCOLOR",        g_setbkcolor },
    { "GDI",     2, "SETBKMODE",         g_setbkmode },
    { "GDI",     9, "SETTEXTCOLOR",      g_settextcolor },
    { "GDI",    19, "LINETO",            g_lineto },
    { "GDI",    20, "MOVETO",            g_moveto },
    { "GDI",    11, "SETWINDOWORG",      g_setwindoworg },   // (#EP3 Fuji Golf)
    { "GDI",    12, "SETWINDOWEXT",      g_setwindowext },   // (#EP3 Fuji Golf)
    { "GDI",    13, "SETVIEWPORTORG",    g_setvieworg },     // (#EP3 Fuji Golf)
    { "GDI",    14, "SETVIEWPORTEXT",    g_setviewext },     // (#EP3 Fuji Golf)
    { "GDI",    36, "POLYGON",           g_polygon },        // (#EP3 Fuji Golf)
    { "GDI",    37, "POLYLINE",          g_polyline },       // (#EP3 Fuji Golf)
    { "GDI",    21, "EXCLUDECLIPRECT",   g_excludecliprect },
    { "GDI",    22, "INTERSECTCLIPRECT", g_intersectcliprect },
    { "GDI",    27, "RECTANGLE",         g_rectangle },
    { "GDI",    31, "SETPIXEL",          g_setpixel },
    { "GDI",    29, "PATBLT",            g_patblt },
    { "GDI",    33, "TEXTOUT",           g_textout },
    { "GDI",   351, "EXTTEXTOUT",        g_exttextout },
    { "GDI",     3, "SETMAPMODE",        g_setmapmode },
    { "GDI",    90, "GETTEXTCOLOR",      g_gettextcolor },
    { "GDI",    92, "GETTEXTFACE",       g_gettextface },
    { "USER",    2, "OLDEXITWINDOWS",    u_oldexitwindows },
    { "USER",   28, "CLIENTTOSCREEN",    u_clienttoscreen },
    { "USER",   29, "SCREENTOCLIENT",    u_screentoclient },
    { "USER",   34, "ENABLEWINDOW",      u_enablewindow },
    { "USER",   35, "ISWINDOWENABLED",   u_iswindowenabled },
    { "USER",   49, "ISWINDOWVISIBLE",   u_iswindowvisible },
    { "USER",  131, "GETCLASSLONG",      u_getclasslong },
    { "USER",  153, "CHANGEMENU",        u_changemenu },
    { "USER",  262, "GETWINDOW",         u_getwindow },
    { "USER",  127, "VALIDATERECT",      u_validaterect },
    { "GDI",    45, "SELECTOBJECT",      g_selectobject },
    { "GDI",    61, "CREATEPEN",         g_createpen },
    { "GDI",    66, "CREATESOLIDBRUSH",  g_createsolidbrush },
    { "GDI",    69, "DELETEOBJECT",      g_deleteobject },
    { "GDI",    80, "GETDEVICECAPS",     g_getdevicecaps },
    { "GDI",    82, "GETOBJECT",         g_getobject },
    { "GDI",    83, "GETPIXEL",          g_getpixel },   // (#394) was wrongly GetClipBox
    { "GDI",    77, "GETCLIPBOX",        g_getclipbox },  // GetClipBox is GDI.77
    { "GDI",   104, "RECTVISIBLE",       g_rectvisible },
    { "GDI",    87, "GETSTOCKOBJECT",    g_getstockobject },
    { "GDI",    24, "ELLIPSE",           g_ellipse },
    { "GDI",    28, "ROUNDRECT",         g_roundrect },
    { "GDI",    34, "BITBLT",            g_bitblt },
    { "GDI",    35, "STRETCHBLT",        g_stretchblt },
    { "GDI",    50, "CREATEBRUSHINDIRECT", g_createbrushindirect },
    { "GDI",    60, "CREATEPATTERNBRUSH", g_createpatternbrush },
    { "GDI",    62, "CREATEPENINDIRECT",  g_createpenindirect },
    { "GDI",    63, "CREATEPOLYGONRGN",   g_createpolygonrgn },
    { "GDI",    64, "CREATERECTRGN",      g_createrectrgn },
    { "GDI",    65, "CREATERECTRGNINDIRECT", g_createrectrgnindirect },
    { "GDI",    48, "CREATEBITMAP",       g_createbitmap },
    { "GDI",    51, "CREATECOMPATIBLEBITMAP", g_createcompatiblebitmap },
    { "GDI",    56, "CREATEFONT",        g_createfont },
    { "GDI",    57, "CREATEFONTINDIRECT", g_createfontindirect },
    { "GDI",    68, "DELETEDC",          g_deletedc },
    { "GDI",    91, "GETTEXTEXTENT",     g_gettextextent },
    { "GDI",   471, "GETTEXTEXTENTPOINT", g_gettextextentpoint },  // (#188)
    { "GDI",    52, "CREATECOMPATIBLEDC", g_createcompatibledc },
    { "GDI",    53, "CREATEDC",          g_createdc },
    { "GDI",    38, "ESCAPE",            g_escape },
    { "GDI",   153, "CREATEIC",          g_createic },
    { "PRNDRV",  0, "DEVICEMODE",         prn_devicemode },
    { "PRNDRV",  0, "EXTDEVICEMODE",      prn_extdevicemode },
    { "PRNDRV",  0, "DEVICECAPABILITIES", prn_devcaps },
    { "PRNDRV",  0, "GENERIC",            prn_generic },
    { "GDI",   154, "GETNEARESTCOLOR",   g_getnearestcolor },
    { "GDI",    93, "GETTEXTMETRICS",    g_gettextmetrics },
    { "GDI",   128, "MULDIV",            g_muldiv },
    { "GDI",     4, "SETROP2",           g_setrop2 },
    { "GDI",    85, "GETROP2",           g_getrop2 },
    { "GDI",     6, "SETPOLYFILLMODE",   g_setpolyfill },
    { "GDI",    84, "GETPOLYFILLMODE",   g_getpolyfill },
    { "GDI",     7, "SETSTRETCHBLTMODE", g_setstretchmode },
    { "GDI",    88, "GETSTRETCHBLTMODE", g_getstretchmode },
    { "GDI",   349, "SETMAPPERFLAGS",    g_setmapperflags },
    { "GDI",    78, "GETCURRENTPOSITION",g_getcurrentpos },
    { "GDI",   194, "GETBOUNDSRECT",     g_getboundsrect },
    { "GDI",   193, "SETBOUNDSRECT",     g_setboundsrect },
    { "GDI",   400, "FASTWINDOWFRAME",   g_fastwindowframe },
    { "GDI",   100, "LINEDDA",           g_linedda },
    { "GDI",   443, "SETDIBITSTODEVICE", g_setdibitstodevice },
    { "GDI",   346, "SETTEXTALIGN",      g_settextalign },
    { "GDI",   360, "CREATEPALETTE",     g_createpalette },
    { "GDI",   350, "GETCHARWIDTH",      g_getcharwidth },
    { "GDI",    70, "ENUMFONTS",         g_enumfonts },
    { "GDI",   442, "CREATEDIBITMAP",    g_createdibitmap },
    // (#188 Word6) KEYBOARD.DRV ANSI<->OEM conversion (identity for ASCII).
    { "KEYBOARD", 5, "ANSITOOEM",        kbd_xlate_copy },
    { "KEYBOARD", 6, "OEMTOANSI",        kbd_xlate_copy },
    { "KEYBOARD", 129, "VKKEYSCAN",       kbd_vkkeyscan },
    // (#278 pass33) keystroke-path ordinals Word 6 calls once the document is up.
    { "KEYBOARD", 131, "MAPVIRTUALKEY",   kbd_mapvirtualkey },
    { "KEYBOARD", 133, "GETKEYNAMETEXT",  kbd_getkeynametext },
    // (#278 Word6 pass19) post-window-creation ordinals (faithful argbytes).
    { "USER",    58,  "GETCLASSNAME",     u_getclassname },
    { "USER",    70,  "SETCURSORPOS",     u_setcursorpos },
    { "USER",   121,  "SETWINDOWSHOOK",   u_setwindowshook },
    { "USER",   225,  "ENUMTASKWINDOWS",  u_enumtaskwindows },
    { "USER",   268,  "GLOBALADDATOM",    u_globaladdatom },
    { "USER",   457,  "DESTROYICON",      u_destroyicon },
    { "USER",   512,  "WNETGETCONNECTION", u_wnetgetconnection },
    { "USER",   513,  "WNETGETCAPS",      u_wnetgetcaps },
    { "GDI",    136,  "REMOVEFONTRESOURCE", g_removefontresource },
    { "KERNEL", 348,  "HMEMCPY",          k_hmemcpy },
    // (#188 Word6) LZEXPAND.DLL LZ-compressed file expansion.
    { "LZEXPAND", 1, "LZCOPY",           lz_copy },
    { "LZEXPAND", 2, "LZOPENFILE",       lz_openfile },
    { "LZEXPAND", 6, "LZCLOSE",          lz_close },
    // (#193) MMSYSTEM audio.
    { "MMSYSTEM", 2, "SNDPLAYSOUND",     mm_sndplaysound },
    { "MMSYSTEM", 3, "PLAYSOUND",        mm_playsound },
    { "MMSYSTEM", 5, "MMSYSTEMGETVERSION", mm_getversion },
    { "WINMM",    2, "SNDPLAYSOUND",     mm_sndplaysound },
    { "WINMM",    0, "PLAYSOUNDA",       mm_playsound },
    // (#188 ordinal-coverage pass) real GDI/USER handlers (additive).
    { "GDI",  30, "SAVEDC",             g_savedc },
    { "GDI",  39, "RESTOREDC",          g_restoredc },
    { "GDI",  75, "GETBKCOLOR",         g_getbkcolor },
    { "GDI",  76, "GETBKMODE",          g_getbkmode },
    { "GDI",  81, "GETMAPMODE",         g_getmapmode },
    { "GDI", 345, "GETTEXTALIGN",       g_gettextalign_r },
    { "GDI",  94, "GETVIEWPORTEXT",     g_getviewportext },
    { "GDI",  95, "GETVIEWPORTORG",     g_getviewportorg },
    { "GDI",  96, "GETWINDOWEXT",       g_getwindowext },
    { "GDI",  97, "GETWINDOWORG",       g_getwindoworg },
    { "GDI", 470, "GETCURRENTPOSITIONEX", g_getcurposex },
    { "GDI", 472, "GETVIEWPORTEXTEX",   g_getviewportextex },
    { "GDI", 473, "GETVIEWPORTORGEX",   g_getviewportorgex },
    { "GDI", 474, "GETWINDOWEXTEX",     g_getwindowextex },
    { "GDI", 475, "GETWINDOWORGEX",     g_getwindoworgex },
    { "GDI", 483, "MOVETOEX",           g_movetoex },
    { "GDI", 479, "SETVIEWPORTEXTEX",   g_setviewportextex },
    { "GDI", 480, "SETVIEWPORTORGEX",   g_setviewportorgex },
    { "GDI", 481, "SETWINDOWEXTEX",     g_setwindowextex },
    { "GDI", 482, "SETWINDOWORGEX",     g_setwindoworgex },
    { "GDI",  15, "OFFSETWINDOWORG",    g_offsetwindoworg },
    { "GDI",  17, "OFFSETVIEWPORTORG",  g_offsetvieworg },
    { "GDI",  16, "SCALEWINDOWEXT",     g_scalewindowext },
    { "GDI",  18, "SCALEVIEWPORTEXT",   g_scaleviewext },
    { "GDI",  99, "LPTODP",             g_lptodp },
    { "GDI",  67, "DPTOLP",             g_dptolp },
    { "GDI", 134, "GETRGNBOX",          g_getrgnbox },
    { "GDI",  58, "CREATEHATCHBRUSH",   g_createhatchbrush },
    { "GDI", 445, "CREATEDIBPATTERNBRUSH", g_createdibpatternbrush },
    { "GDI",  49, "CREATEBITMAPINDIRECT", g_createbitmapindirect },
    { "GDI", 156, "CREATEDISCARDABLEBITMAP", g_creatdiscardablebmp },
    { "GDI",  54, "CREATEELLIPTICRGN",  g_createregion8 },
    { "GDI",  55, "CREATEELLIPTICRGNINDIRECT", g_createregion4 },
    { "GDI", 444, "CREATEROUNDRECTRGN", g_createroundrectrgn },
    { "GDI",  23, "ARC",                g_arc },
    { "GDI",  26, "PIE",                g_pie },
    { "GDI", 348, "CHORD",              g_pie },
    { "GDI", 450, "POLYPOLYGON",        g_polypolygon },
    { "GDI", 439, "STRETCHDIBITS",      g_stretchdibits },
    { "USER", 272, "ISZOOMED",          u_iszoomed },
    { "USER",  82, "INVERTRECT",        u_invertrect },
    // SHELL.DLL by-name (Win3.1 apps import these by name).
    { "SHELL", 0, "SHELLEXECUTE",       sh_shellexecute },
    { "SHELL", 0, "FINDEXECUTABLE",     sh_findexecutable },
    { "SHELL", 0, "DRAGACCEPTFILES",    sh_dragacceptfiles },
    { "SHELL", 0, "DRAGQUERYFILE",      sh_dragqueryfile },
    { "SHELL", 0, "DRAGFINISH",         sh_dragfinish },
    { "SHELL", 0, "DRAGQUERYPOINT",     sh_dragquerypoint },
    { "SHELL", 0, "EXTRACTICON",        sh_extracticon },
    { "SHELL", 0, "SHELLABOUT",         sh_shellabout },

    // (#407 2026-07-08) OLESVR: OLE 1.0 server registration, needed by Word 2.0
    // for Windows' WinMain startup gate. See the block comment above
    // os_registerserver for the full rationale and WINE spec citation.
    { "OLESVR", 2,  "OLEREGISTERSERVER",    os_registerserver },
    { "OLESVR", 3,  "OLEREVOKESERVER",      os_revokeserver },
    { "OLESVR", 4,  "OLEBLOCKSERVER",       os_blockserver },
    { "OLESVR", 5,  "OLEUNBLOCKSERVER",     os_unblockserver },
    { "OLESVR", 6,  "OLEREGISTERSERVERDOC", os_registerserverdoc },
    { "OLESVR", 7,  "OLEREVOKESERVERDOC",   os_revokeserverdoc },
    { "OLESVR", 8,  "OLERENAMESERVERDOC",   os_renameserverdoc },
    { "OLESVR", 9,  "OLEREVERTSERVERDOC",   os_revertserverdoc },
    { "OLESVR", 10, "OLESAVEDSERVERDOC",    os_savedserverdoc },
};
#define API_TABLE_LEN ((int)(sizeof(g_api_table)/sizeof(g_api_table[0])))

// Case-insensitive ASCII compare.
static int ci_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static win16_handler_fn find_handler(const win16_import_t *im) {
    for (int i = 0; i < API_TABLE_LEN; i++) {
        const win16_api_entry_t *e = &g_api_table[i];
        if (!ci_eq(e->module, im->module)) continue;
        if (im->by_ordinal) {
            if (e->ordinal != 0 && e->ordinal == im->ordinal) return e->fn;
        } else {
            if (e->name && ci_eq(e->name, im->name)) return e->fn;
        }
    }
    return 0;
}

// (#188 ordinal-coverage pass) Additive stub table: EXACT Pascal argbytes (from
// the WINE gdi/user/krnl386 .exe16 specs) for every remaining ordinal not in the
// main table. When an ordinal import is otherwise UNIMPL, this supplies the
// correct stack cleanup (retval + argbytes) instead of argbytes=0, which killed
// the whole "wrong-argbytes desyncs the Pascal stack -> WILDCS/black box" class.
// retval: 0 for handle/value returns (safe "no object"), 1 for BOOL success.
typedef struct { const char *module; uint16_t ordinal; uint16_t argbytes; uint16_t retval; } win16_stub_entry_t;
static const win16_stub_entry_t g_stub_table[] = {
    // ---- GDI additive coverage (297 ordinals) ----
    { "GDI", 5, 4, 1 },  // SetRelAbs
    { "GDI", 8, 4, 1 },  // SetTextCharacterExtra
    { "GDI", 10, 6, 1 },  // SetTextJustification
    { "GDI", 25, 10, 1 },  // FloodFill
    { "GDI", 32, 6, 1 },  // OffsetClipRgn
    { "GDI", 38, 14, 0 },  // Escape
    { "GDI", 40, 6, 1 },  // FillRgn
    { "GDI", 41, 10, 1 },  // FrameRgn
    { "GDI", 42, 4, 1 },  // InvertRgn
    { "GDI", 43, 4, 1 },  // PaintRgn
    { "GDI", 44, 4, 0 },  // SelectClipRgn
    { "GDI", 46, 0, 1 },  // BITMAPBITS
    { "GDI", 47, 8, 1 },  // CombineRgn
    { "GDI", 71, 12, 0 },  // EnumObjects
    { "GDI", 72, 4, 1 },  // EqualRgn
    { "GDI", 73, 10, 1 },  // ExcludeVisRect
    { "GDI", 74, 10, 0 },  // GetBitmapBits
    { "GDI", 79, 2, 0 },  // GetDCOrg
    { "GDI", 86, 2, 0 },  // GetRelAbs
    { "GDI", 89, 2, 0 },  // GetTextCharacterExtra
    { "GDI", 98, 10, 1 },  // IntersectVisRect
    { "GDI", 101, 6, 1 },  // OffsetRgn
    { "GDI", 102, 6, 1 },  // OffsetVisRgn
    { "GDI", 103, 6, 1 },  // PtVisible
    { "GDI", 105, 4, 0 },  // SelectVisRgn
    { "GDI", 106, 10, 0 },  // SetBitmapBits
    { "GDI", 117, 6, 0 },  // SetDCOrg
    { "GDI", 118, 0, 1 },  // InternalCreateDC
    { "GDI", 119, 4, 1 },  // AddFontResource
    { "GDI", 120, 0, 0 },  // GetContinuingTextExtent
    { "GDI", 121, 2, 1 },  // Death
    { "GDI", 122, 14, 1 },  // Resurrection
    { "GDI", 123, 4, 1 },  // PlayMetaFile
    { "GDI", 124, 4, 0 },  // GetMetaFile
    { "GDI", 125, 4, 0 },  // CreateMetaFile
    { "GDI", 126, 2, 1 },  // CloseMetaFile
    { "GDI", 127, 2, 1 },  // DeleteMetaFile
    { "GDI", 129, 2, 0 },  // SaveVisRgn
    { "GDI", 130, 2, 1 },  // RestoreVisRgn
    { "GDI", 131, 2, 0 },  // InquireVisRgn
    { "GDI", 132, 10, 1 },  // SetEnvironment
    { "GDI", 133, 10, 0 },  // GetEnvironment
    { "GDI", 137, 0, 1 },  // GSV
    { "GDI", 138, 0, 1 },  // DPXlate
    { "GDI", 139, 0, 1 },  // SetWinViewExt
    { "GDI", 140, 0, 1 },  // ScaleExt
    { "GDI", 141, 0, 1 },  // WordSet
    { "GDI", 142, 0, 1 },  // RectStuff
    { "GDI", 143, 0, 1 },  // OffsetOrg
    { "GDI", 144, 0, 1 },  // LockDC
    { "GDI", 145, 0, 1 },  // UnlockDC
    { "GDI", 146, 0, 1 },  // LockUnlock
    { "GDI", 147, 0, 1 },  // GDI_FarFrame
    { "GDI", 148, 6, 0 },  // SetBrushOrg
    { "GDI", 149, 2, 0 },  // GetBrushOrg
    { "GDI", 150, 2, 1 },  // UnrealizeObject
    { "GDI", 151, 6, 0 },  // CopyMetaFile
    { "GDI", 152, 0, 1 },  // GDIInitApp
    { "GDI", 155, 4, 0 },  // QueryAbort
    { "GDI", 157, 0, 1 },  // CompatibleBitmap
    { "GDI", 158, 0, 0 },  // EnumCallback
    { "GDI", 159, 2, 0 },  // GetMetaFileBits
    { "GDI", 160, 2, 1 },  // SetMetaFileBits
    { "GDI", 161, 6, 1 },  // PtInRegion
    { "GDI", 162, 2, 0 },  // GetBitmapDimension
    { "GDI", 163, 6, 0 },  // SetBitmapDimension
    { "GDI", 164, 0, 1 },  // PixToLine
    { "GDI", 169, 0, 1 },  // IsDCDirty
    { "GDI", 170, 0, 1 },  // SetDCStatus
    { "GDI", 171, 0, 1 },  // LVBUNION
    { "GDI", 172, 10, 1 },  // SetRectRgn
    { "GDI", 173, 2, 0 },  // GetClipRgn
    { "GDI", 174, 0, 1 },  // BLOAT
    { "GDI", 175, 12, 0 },  // EnumMetaFile
    { "GDI", 176, 12, 1 },  // PlayMetaFileRecord
    { "GDI", 177, 0, 1 },  // RCOS
    { "GDI", 178, 0, 1 },  // RSIN
    { "GDI", 179, 2, 0 },  // GetDCState
    { "GDI", 180, 4, 1 },  // SetDCState
    { "GDI", 181, 6, 1 },  // RectInRegionOld
    { "GDI", 182, 0, 1 },  // REQUESTSEM
    { "GDI", 183, 0, 1 },  // CLEARSEM
    { "GDI", 184, 0, 1 },  // STUFFVISIBLE
    { "GDI", 185, 0, 1 },  // STUFFINREGION
    { "GDI", 186, 0, 1 },  // DELETEABOVELINEFONTS
    { "GDI", 188, 0, 0 },  // GetTextExtentEx
    { "GDI", 190, 10, 1 },  // SetDCHook
    { "GDI", 191, 6, 0 },  // GetDCHook
    { "GDI", 192, 4, 1 },  // SetHookFlags
    { "GDI", 195, 0, 0 },  // SelectBitmap
    { "GDI", 196, 2, 1 },  // SetMetaFileBitsBetter
    { "GDI", 201, 0, 1 },  // DMBITBLT
    { "GDI", 202, 0, 1 },  // DMCOLORINFO
    { "GDI", 206, 0, 1 },  // dmEnumDFonts
    { "GDI", 207, 0, 1 },  // DMENUMOBJ
    { "GDI", 208, 0, 1 },  // DMOUTPUT
    { "GDI", 209, 0, 1 },  // DMPIXEL
    { "GDI", 210, 0, 1 },  // dmRealizeObject
    { "GDI", 211, 0, 1 },  // DMSTRBLT
    { "GDI", 212, 0, 1 },  // DMSCANLR
    { "GDI", 213, 0, 1 },  // BRUTE
    { "GDI", 214, 0, 1 },  // DMEXTTEXTOUT
    { "GDI", 215, 0, 1 },  // DMGETCHARWIDTH
    { "GDI", 216, 0, 1 },  // DMSTRETCHBLT
    { "GDI", 217, 0, 1 },  // DMDIBBITS
    { "GDI", 218, 0, 1 },  // DMSTRETCHDIBITS
    { "GDI", 219, 0, 1 },  // DMSETDIBTODEV
    { "GDI", 220, 0, 1 },  // DMTRANSPOSE
    { "GDI", 230, 2, 0 },  // CreatePQ
    { "GDI", 231, 2, 1 },  // MinPQ
    { "GDI", 232, 2, 0 },  // ExtractPQ
    { "GDI", 233, 6, 1 },  // InsertPQ
    { "GDI", 234, 4, 1 },  // SizePQ
    { "GDI", 235, 2, 1 },  // DeletePQ
    { "GDI", 240, 10, 0 },  // OpenJob
    { "GDI", 241, 8, 1 },  // WriteSpool
    { "GDI", 242, 8, 1 },  // WriteDialog
    { "GDI", 243, 2, 1 },  // CloseJob
    { "GDI", 244, 4, 1 },  // DeleteJob
    { "GDI", 245, 6, 0 },  // GetSpoolJob
    { "GDI", 246, 2, 1 },  // StartSpoolPage
    { "GDI", 247, 2, 1 },  // EndSpoolPage
    { "GDI", 248, 0, 0 },  // QueryJob
    { "GDI", 250, 10, 0 },  // Copy
    { "GDI", 253, 0, 1 },  // DeleteSpoolPage
    { "GDI", 254, 0, 1 },  // SpoolFile
    { "GDI", 267, 0, 1 },  // StartDocPrintEra
    { "GDI", 268, 0, 1 },  // StartPagePrinter
    { "GDI", 269, 0, 1 },  // WritePrinter
    { "GDI", 270, 0, 1 },  // EndPagePrinter
    { "GDI", 271, 0, 1 },  // AbortPrinter
    { "GDI", 272, 0, 1 },  // EndDocPrinter
    { "GDI", 274, 0, 1 },  // ClosePrinter
    { "GDI", 280, 0, 0 },  // GetRealDriverInfo
    { "GDI", 281, 20, 0 },  // DrvSetPrinterData
    { "GDI", 282, 24, 0 },  // DrvGetPrinterData
    { "GDI", 299, 0, 1 },  // ENGINEGETCHARWIDTHEX
    { "GDI", 300, 12, 0 },  // EngineEnumerateFont
    { "GDI", 301, 4, 1 },  // EngineDeleteFont
    { "GDI", 302, 12, 0 },  // EngineRealizeFont
    { "GDI", 303, 12, 1 },  // EngineGetCharWidth
    { "GDI", 304, 6, 1 },  // EngineSetFontContext
    { "GDI", 305, 22, 1 },  // EngineGetGlyphBMP
    { "GDI", 306, 10, 0 },  // EngineMakeFontDir
    { "GDI", 307, 10, 0 },  // GetCharABCWidths
    { "GDI", 308, 8, 0 },  // GetOutlineTextMetrics
    { "GDI", 309, 22, 0 },  // GetGlyphOutline
    { "GDI", 310, 14, 0 },  // CreateScalableFontResource
    { "GDI", 311, 18, 0 },  // GetFontData
    { "GDI", 312, 0, 1 },  // ConvertOutLineFontFile
    { "GDI", 313, 6, 0 },  // GetRasterizerCaps
    { "GDI", 314, 0, 1 },  // EngineExtTextOut
    { "GDI", 315, 16, 0 },  // EngineRealizeFontExt
    { "GDI", 316, 0, 1 },  // EngineGetCharWidthStr
    { "GDI", 317, 0, 1 },  // EngineGetGlyphBmpExt
    { "GDI", 330, 14, 0 },  // EnumFontFamilies
    { "GDI", 332, 8, 0 },  // GetKerningPairs
    { "GDI", 347, 0, 1 },  // MFDRAWTEXT
    { "GDI", 352, 0, 0 },  // GetPhysicalFontHandle
    { "GDI", 353, 0, 0 },  // GetAspectRatioFilter
    { "GDI", 354, 0, 1 },  // ShrinkGDIHeap
    { "GDI", 355, 0, 1 },  // FTrapping0
    { "GDI", 361, 6, 1 },  // GDISelectPalette
    { "GDI", 362, 2, 1 },  // GDIRealizePalette
    { "GDI", 363, 10, 0 },  // GetPaletteEntries
    { "GDI", 364, 10, 1 },  // SetPaletteEntries
    { "GDI", 365, 2, 0 },  // RealizeDefaultPalette
    { "GDI", 366, 2, 1 },  // UpdateColors
    { "GDI", 367, 10, 1 },  // AnimatePalette
    { "GDI", 368, 4, 1 },  // ResizePalette
    { "GDI", 370, 6, 0 },  // GetNearestPaletteIndex
    { "GDI", 372, 12, 1 },  // ExtFloodFill
    { "GDI", 373, 4, 1 },  // SetSystemPaletteUse
    { "GDI", 374, 2, 0 },  // GetSystemPaletteUse
    { "GDI", 375, 10, 0 },  // GetSystemPaletteEntries
    { "GDI", 376, 6, 1 },  // ResetDC
    { "GDI", 377, 6, 1 },  // StartDoc
    { "GDI", 378, 2, 1 },  // EndDoc
    { "GDI", 379, 2, 1 },  // StartPage
    { "GDI", 380, 2, 1 },  // EndPage
    { "GDI", 381, 6, 1 },  // SetAbortProc
    { "GDI", 382, 2, 1 },  // AbortDoc
    { "GDI", 401, 0, 1 },  // GDIMOVEBITMAP
    { "GDI", 402, 0, 1 },  // GDIGETBITSGLOBAL
    { "GDI", 403, 4, 1 },  // GdiInit2
    { "GDI", 404, 0, 0 },  // GetTTGlyphIndexMap
    { "GDI", 405, 2, 1 },  // FinalGdiInit
    { "GDI", 406, 0, 1 },  // CREATEREALBITMAPINDIRECT
    { "GDI", 407, 12, 0 },  // CreateUserBitmap
    { "GDI", 408, 0, 1 },  // CREATEREALBITMAP
    { "GDI", 409, 6, 0 },  // CreateUserDiscardableBitmap
    { "GDI", 410, 2, 1 },  // IsValidMetaFile
    { "GDI", 411, 2, 0 },  // GetCurLogFont
    { "GDI", 412, 2, 1 },  // IsDCCurrentPalette
    { "GDI", 440, 18, 1 },  // SetDIBits
    { "GDI", 441, 18, 0 },  // GetDIBits
    { "GDI", 449, 0, 1 },  // DEVICECOLORMATCH
    { "GDI", 451, 12, 0 },  // CreatePolyPolygonRgn
    { "GDI", 452, 8, 0 },  // GdiSeeGdiDo
    { "GDI", 460, 0, 1 },  // GDITASKTERMINATION
    { "GDI", 461, 4, 1 },  // SetObjectOwner
    { "GDI", 462, 2, 1 },  // IsGDIObject
    { "GDI", 463, 4, 0 },  // MakeObjectPrivate
    { "GDI", 464, 0, 1 },  // FIXUPBOGUSPUBLISHERMETAFILE
    { "GDI", 465, 6, 1 },  // RectVisible
    { "GDI", 466, 6, 1 },  // RectInRegion
    { "GDI", 467, 0, 1 },  // UNICODETOANSI
    { "GDI", 468, 6, 0 },  // GetBitmapDimensionEx
    { "GDI", 469, 6, 0 },  // GetBrushOrgEx
    { "GDI", 476, 10, 1 },  // OffsetViewportOrgEx
    { "GDI", 477, 10, 1 },  // OffsetWindowOrgEx
    { "GDI", 478, 10, 1 },  // SetBitmapDimensionEx
    { "GDI", 484, 14, 1 },  // ScaleViewportExtEx
    { "GDI", 485, 14, 1 },  // ScaleWindowExtEx
    { "GDI", 486, 6, 0 },  // GetAspectRatioFilterEx
    { "GDI", 489, 20, 0 },  // CreateDIBSection
    { "GDI", 490, 0, 1 },  // CloseEnhMetafile
    { "GDI", 491, 0, 0 },  // CopyEnhMetafile
    { "GDI", 492, 0, 0 },  // CreateEnhMetafile
    { "GDI", 493, 0, 1 },  // DeleteEnhMetafile
    { "GDI", 495, 0, 1 },  // GDIComment
    { "GDI", 496, 0, 0 },  // GetEnhMetafile
    { "GDI", 497, 0, 0 },  // GetEnhMetafileBits
    { "GDI", 498, 0, 0 },  // GetEnhMetafileDescription
    { "GDI", 499, 0, 0 },  // GetEnhMetafileHeader
    { "GDI", 501, 0, 0 },  // GetEnhMetafilePaletteEntries
    { "GDI", 502, 8, 1 },  // PolyBezier
    { "GDI", 503, 8, 1 },  // PolyBezierTo
    { "GDI", 504, 0, 1 },  // PlayEnhMetafileRecord
    { "GDI", 505, 0, 1 },  // SetEnhMetafileBits
    { "GDI", 506, 0, 1 },  // SetMetaRgn
    { "GDI", 508, 6, 1 },  // ExtSelectClipRgn
    { "GDI", 511, 2, 1 },  // AbortPath
    { "GDI", 512, 2, 1 },  // BeginPath
    { "GDI", 513, 2, 1 },  // CloseFigure
    { "GDI", 514, 2, 1 },  // EndPath
    { "GDI", 515, 2, 1 },  // FillPath
    { "GDI", 516, 2, 1 },  // FlattenPath
    { "GDI", 517, 12, 0 },  // GetPath
    { "GDI", 518, 2, 1 },  // PathToRegion
    { "GDI", 519, 4, 0 },  // SelectClipPath
    { "GDI", 520, 2, 1 },  // StrokeAndFillPath
    { "GDI", 521, 2, 1 },  // StrokePath
    { "GDI", 522, 2, 1 },  // WidenPath
    { "GDI", 523, 0, 1 },  // ExtCreatePen
    { "GDI", 524, 2, 0 },  // GetArcDirection
    { "GDI", 525, 4, 1 },  // SetArcDirection
    { "GDI", 526, 0, 0 },  // GetMiterLimit
    { "GDI", 527, 0, 1 },  // SetMiterLimit
    { "GDI", 528, 0, 1 },  // GDIParametersInfo
    { "GDI", 529, 2, 0 },  // CreateHalftonePalette
    { "GDI", 530, 0, 1 },  // RawTextOut
    { "GDI", 531, 0, 1 },  // RawExtTextOut
    { "GDI", 532, 8, 1 },  // RawGetTextExtent
    { "GDI", 536, 0, 1 },  // BiDiLayout
    { "GDI", 538, 0, 1 },  // BiDiCreateTabString
    { "GDI", 540, 0, 1 },  // BiDiGlyphOut
    { "GDI", 543, 0, 1 },  // BiDiGetStringExtent
    { "GDI", 555, 0, 1 },  // BiDiDeleteString
    { "GDI", 556, 0, 1 },  // BiDiSetDefaults
    { "GDI", 558, 0, 1 },  // BiDiGetDefaults
    { "GDI", 560, 0, 1 },  // BiDiShape
    { "GDI", 561, 0, 1 },  // BiDiFontComplement
    { "GDI", 564, 0, 1 },  // BiDiSetKashida
    { "GDI", 565, 0, 1 },  // BiDiKExtTextOut
    { "GDI", 566, 0, 1 },  // BiDiShapeEx
    { "GDI", 569, 0, 1 },  // BiDiCreateStringEx
    { "GDI", 571, 0, 0 },  // GetTextExtentRtoL
    { "GDI", 572, 0, 0 },  // GetHDCCharSet
    { "GDI", 573, 0, 1 },  // BiDiLayoutEx
    { "GDI", 602, 10, 1 },  // SetDIBColorTable
    { "GDI", 603, 10, 0 },  // GetDIBColorTable
    { "GDI", 604, 6, 1 },  // SetSolidBrush
    { "GDI", 605, 2, 1 },  // SysDeleteObject
    { "GDI", 606, 8, 1 },  // SetMagicColors
    { "GDI", 607, 10, 0 },  // GetRegionData
    { "GDI", 608, 0, 1 },  // ExtCreateRegion
    { "GDI", 609, 4, 1 },  // GdiFreeResources
    { "GDI", 610, 14, 1 },  // GdiSignalProc32
    { "GDI", 611, 0, 0 },  // GetRandomRgn
    { "GDI", 612, 2, 0 },  // GetTextCharset
    { "GDI", 613, 18, 0 },  // EnumFontFamiliesEx
    { "GDI", 614, 0, 1 },  // AddLpkToGDI
    { "GDI", 615, 0, 0 },  // GetCharacterPlacement
    { "GDI", 616, 2, 0 },  // GetFontLanguageInfo
    { "GDI", 650, 0, 1 },  // BuildInverseTableDIB
    { "GDI", 701, 0, 1 },  // GDITHKCONNECTIONDATALS
    { "GDI", 702, 0, 1 },  // FT_GDIFTHKTHKCONNECTIONDATA
    { "GDI", 703, 0, 1 },  // FDTHKCONNECTIONDATASL
    { "GDI", 704, 0, 1 },  // ICMTHKCONNECTIONDATASL
    { "GDI", 820, 0, 1 },  // ICMCreateTransform
    { "GDI", 821, 0, 1 },  // ICMDeleteTransform
    { "GDI", 822, 0, 1 },  // ICMTranslateRGB
    { "GDI", 823, 0, 1 },  // ICMTranslateRGBs
    { "GDI", 824, 0, 1 },  // ICMCheckColorsInGamut
    { "GDI", 1000, 6, 1 },  // SetLayout
    { "GDI", 1001, 0, 0 },  // GetLayout
    // ---- USER additive coverage (365 ordinals) ----
    { "USER", 3, 0, 1 },  // EnableOEMLayer
    { "USER", 4, 0, 1 },  // DisableOEMLayer
    { "USER", 7, 6, 1 },  // ExitWindows
    { "USER", 11, 10, 1 },  // SetSystemTimer
    { "USER", 14, 0, 0 },  // GetTimerResolution
    { "USER", 16, 4, 1 },  // ClipCursor
    { "USER", 17, 4, 0 },  // GetCursorPos
    { "USER", 20, 2, 1 },  // SetDoubleClickTime
    { "USER", 21, 0, 0 },  // GetDoubleClickTime
    { "USER", 24, 6, 1 },  // RemoveProp
    { "USER", 25, 6, 0 },  // GetProp
    { "USER", 26, 8, 1 },  // SetProp
    { "USER", 27, 6, 0 },  // EnumProps
    { "USER", 30, 4, 1 },  // WindowFromPoint
    { "USER", 38, 2, 0 },  // GetWindowTextLength
    { "USER", 43, 2, 1 },  // CloseWindow
    { "USER", 44, 2, 0 },  // OpenIcon
    { "USER", 45, 2, 1 },  // BringWindowToTop
    { "USER", 48, 4, 1 },  // IsChild
    { "USER", 51, 0, 1 },  // BEAR51
    { "USER", 52, 0, 1 },  // AnyPopup
    { "USER", 54, 8, 0 },  // EnumWindows
    { "USER", 55, 10, 0 },  // EnumChildWindows
    { "USER", 59, 2, 1 },  // SetActiveWindow
    { "USER", 61, 14, 1 },  // ScrollWindow
    { "USER", 62, 8, 1 },  // SetScrollPos
    { "USER", 63, 4, 0 },  // GetScrollPos
    { "USER", 64, 10, 1 },  // SetScrollRange
    { "USER", 65, 12, 0 },  // GetScrollRange
    { "USER", 80, 12, 1 },  // UnionRect
    { "USER", 86, 0, 0 },  // IconSize
    { "USER", 99, 8, 1 },  // DlgDirSelect
    { "USER", 100, 12, 1 },  // DlgDirList
    { "USER", 105, 4, 1 },  // FlashWindow
    { "USER", 117, 2, 1 },  // WindowFromDC
    { "USER", 123, 6, 1 },  // CallMsgFilter
    { "USER", 126, 6, 1 },  // InvalidateRgn
    { "USER", 128, 4, 1 },  // ValidateRgn
    { "USER", 129, 4, 0 },  // GetClassWord
    { "USER", 130, 6, 1 },  // SetClassWord
    { "USER", 132, 8, 0 },  // SetClassLong
    { "USER", 137, 2, 0 },  // OpenClipboard
    { "USER", 138, 0, 1 },  // CloseClipboard
    { "USER", 139, 0, 1 },  // EmptyClipboard
    { "USER", 140, 0, 0 },  // GetClipboardOwner
    { "USER", 141, 4, 1 },  // SetClipboardData
    { "USER", 142, 2, 0 },  // GetClipboardData
    { "USER", 143, 0, 1 },  // CountClipboardFormats
    { "USER", 144, 2, 0 },  // EnumClipboardFormats
    { "USER", 146, 8, 0 },  // GetClipboardFormatName
    { "USER", 147, 2, 1 },  // SetClipboardViewer
    { "USER", 148, 0, 0 },  // GetClipboardViewer
    { "USER", 149, 4, 1 },  // ChangeClipboardChain
    { "USER", 170, 2, 1 },  // ArrangeIconicWindows
    { "USER", 172, 4, 1 },  // SwitchToThisWindow
    { "USER", 181, 10, 1 },  // SetSysColors
    { "USER", 182, 4, 1 },  // KillSystemTimer
    { "USER", 183, 4, 0 },  // GetCaretPos
    { "USER", 184, 0, 0 },  // QuerySendMessage
    { "USER", 185, 22, 1 },  // GrayString
    { "USER", 186, 2, 1 },  // SwapMouseButton
    { "USER", 187, 0, 1 },  // EndMenu
    { "USER", 188, 2, 1 },  // SetSysModalWindow
    { "USER", 189, 0, 0 },  // GetSysModalWindow
    { "USER", 191, 6, 1 },  // ChildWindowFromPoint
    { "USER", 193, 2, 1 },  // IsClipboardFormatAvailable
    { "USER", 194, 8, 1 },  // DlgDirSelectComboBox
    { "USER", 195, 12, 1 },  // DlgDirListComboBox
    { "USER", 196, 20, 0 },  // TabbedTextOut
    { "USER", 197, 14, 0 },  // GetTabbedTextExtent
    { "USER", 198, 4, 1 },  // CascadeChildWindows
    { "USER", 199, 4, 1 },  // TileChildWindows
    { "USER", 200, 8, 0 },  // OpenComm
    { "USER", 201, 4, 1 },  // SetCommState
    { "USER", 202, 6, 0 },  // GetCommState
    { "USER", 203, 6, 0 },  // GetCommError
    { "USER", 204, 8, 1 },  // ReadComm
    { "USER", 205, 8, 1 },  // WriteComm
    { "USER", 206, 4, 1 },  // TransmitCommChar
    { "USER", 207, 2, 1 },  // CloseComm
    { "USER", 208, 4, 0 },  // SetCommEventMask
    { "USER", 209, 4, 0 },  // GetCommEventMask
    { "USER", 210, 2, 1 },  // SetCommBreak
    { "USER", 211, 2, 1 },  // ClearCommBreak
    { "USER", 212, 4, 1 },  // UngetCommChar
    { "USER", 213, 8, 1 },  // BuildCommDCB
    { "USER", 214, 4, 0 },  // EscapeCommFunction
    { "USER", 215, 4, 1 },  // FlushComm
    { "USER", 216, 8, 0 },  // UserSeeUserDo
    { "USER", 217, 4, 1 },  // LookupMenuHandle
    { "USER", 220, 4, 0 },  // LoadMenuIndirect
    { "USER", 221, 20, 1 },  // ScrollDC
    { "USER", 222, 4, 0 },  // GetKeyboardState
    { "USER", 223, 4, 1 },  // SetKeyboardState
    { "USER", 224, 2, 0 },  // GetWindowTask
    { "USER", 226, 0, 1 },  // LockInput
    { "USER", 227, 6, 0 },  // GetNextDlgGroupItem
    { "USER", 228, 6, 0 },  // GetNextDlgTabItem
    { "USER", 229, 2, 0 },  // GetTopWindow
    { "USER", 230, 4, 0 },  // GetNextWindow
    { "USER", 231, 0, 0 },  // GetSystemDebugState
    { "USER", 233, 4, 1 },  // SetParent
    { "USER", 234, 6, 1 },  // UnhookWindowsHook
    { "USER", 235, 12, 0 },  // DefHookProc
    { "USER", 236, 0, 0 },  // GetCapture
    { "USER", 237, 6, 0 },  // GetUpdateRgn
    { "USER", 238, 4, 1 },  // ExcludeUpdateRgn
    { "USER", 244, 8, 1 },  // EqualRect
    { "USER", 245, 8, 1 },  // EnableCommNotification
    { "USER", 246, 8, 1 },  // ExitWindowsExec
    { "USER", 247, 0, 0 },  // GetCursor
    { "USER", 248, 0, 0 },  // GetOpenClipboardWindow
    { "USER", 251, 12, 0 },  // SendDriverMessage
    { "USER", 252, 12, 0 },  // OpenDriver
    { "USER", 253, 10, 0 },  // CloseDriver
    { "USER", 254, 2, 0 },  // GetDriverModuleHandle
    { "USER", 255, 16, 0 },  // DefDriverProc
    { "USER", 256, 6, 0 },  // GetDriverInfo
    { "USER", 257, 6, 0 },  // GetNextDriver
    { "USER", 258, 10, 1 },  // MapWindowPoints
    { "USER", 259, 2, 1 },  // BeginDeferWindowPos
    { "USER", 260, 16, 1 },  // DeferWindowPos
    { "USER", 261, 2, 1 },  // EndDeferWindowPos
    { "USER", 265, 4, 1 },  // ShowOwnedPopups
    { "USER", 267, 6, 1 },  // ShowScrollBar
    { "USER", 269, 2, 1 },  // GlobalDeleteAtom
    { "USER", 270, 4, 1 },  // GlobalFindAtom
    { "USER", 271, 8, 1 },  // GlobalGetAtomName
    { "USER", 273, 8, 1 },  // ControlPanelInfo
    { "USER", 274, 0, 0 },  // GetNextQueueWindow
    { "USER", 275, 0, 1 },  // RepaintScreen
    { "USER", 276, 0, 1 },  // LockMyTask
    { "USER", 278, 0, 0 },  // GetDesktopHwnd
    { "USER", 279, 0, 1 },  // OldSetDeskPattern
    { "USER", 280, 4, 1 },  // SetSystemMenu
    { "USER", 281, 2, 0 },  // GetSysColorBrush
    { "USER", 284, 2, 0 },  // GetFreeSystemResources
    { "USER", 285, 4, 1 },  // SetDeskWallpaper
    { "USER", 287, 2, 0 },  // GetLastActivePopup
    { "USER", 289, 0, 0 },  // keybd_event
    { "USER", 290, 10, 1 },  // RedrawWindow
    { "USER", 291, 10, 0 },  // SetWindowsHookEx
    { "USER", 292, 4, 1 },  // UnhookWindowsHookEx
    { "USER", 293, 12, 0 },  // CallNextHookEx
    { "USER", 294, 2, 1 },  // LockWindowUpdate
    { "USER", 299, 0, 0 },  // mouse_event
    { "USER", 300, 0, 1 },  // UnloadInstalledDrivers
    { "USER", 301, 0, 1 },  // EDITWNDPROC
    { "USER", 302, 0, 1 },  // STATICWNDPROC
    { "USER", 303, 0, 1 },  // BUTTONWNDPROC
    { "USER", 304, 0, 1 },  // SBWNDPROC
    { "USER", 305, 0, 1 },  // DESKTOPWNDPROC
    { "USER", 306, 0, 1 },  // MENUWNDPROC
    { "USER", 307, 0, 1 },  // LBOXCTLWNDPROC
    { "USER", 309, 4, 0 },  // GetClipCursor
    { "USER", 314, 10, 1 },  // SignalProc
    { "USER", 319, 22, 1 },  // ScrollWindowEx
    { "USER", 320, 0, 1 },  // SysErrorBox
    { "USER", 321, 4, 0 },  // SetEventHook
    { "USER", 322, 0, 1 },  // WinOldAppHackOMatic
    { "USER", 323, 0, 0 },  // GetMessage2
    { "USER", 324, 8, 1 },  // FillWindow
    { "USER", 325, 12, 1 },  // PaintRect
    { "USER", 326, 6, 0 },  // GetControlBrush
    { "USER", 331, 2, 1 },  // EnableHardwareInput
    { "USER", 332, 0, 1 },  // UserYield
    { "USER", 333, 0, 1 },  // IsUserIdle
    { "USER", 336, 6, 0 },  // LoadCursorIconHandler
    { "USER", 337, 0, 0 },  // GetMouseEventProc
    { "USER", 338, 0, 1 },  // ECGETDS
    { "USER", 343, 0, 0 },  // GetFilePortName
    { "USER", 344, 0, 1 },  // COMBOBOXCTLWNDPROC
    { "USER", 345, 0, 1 },  // BEAR345
    { "USER", 356, 6, 0 },  // LoadDIBCursorHandler
    { "USER", 357, 6, 0 },  // LoadDIBIconHandler
    { "USER", 359, 8, 0 },  // GetDCEx
    { "USER", 362, 12, 1 },  // DCHook
    { "USER", 364, 12, 1 },  // LookupIconIdFromDirectoryEx
    { "USER", 368, 4, 0 },  // CopyIcon
    { "USER", 369, 4, 0 },  // CopyCursor
    { "USER", 370, 6, 0 },  // GetWindowPlacement
    { "USER", 371, 6, 1 },  // SetWindowPlacement
    { "USER", 372, 0, 0 },  // GetInternalIconHeader
    { "USER", 373, 12, 1 },  // SubtractRect
    { "USER", 374, 16, 1 },  // DllEntryPoint
    { "USER", 375, 0, 1 },  // DrawTextEx
    { "USER", 376, 0, 1 },  // SetMessageExtraInfo
    { "USER", 378, 0, 1 },  // SetPropEx
    { "USER", 379, 0, 0 },  // GetPropEx
    { "USER", 380, 0, 1 },  // RemovePropEx
    { "USER", 382, 0, 1 },  // SetWindowContextHelpID
    { "USER", 383, 0, 0 },  // GetWindowContextHelpID
    { "USER", 384, 4, 1 },  // SetMenuContextHelpId
    { "USER", 385, 2, 0 },  // GetMenuContextHelpId
    { "USER", 389, 14, 0 },  // LoadImage
    { "USER", 390, 10, 0 },  // CopyImage
    { "USER", 391, 14, 1 },  // SignalProc32
    { "USER", 394, 18, 1 },  // DrawIconEx
    { "USER", 395, 6, 0 },  // GetIconInfo
    { "USER", 397, 4, 1 },  // RegisterClassEx
    { "USER", 398, 10, 0 },  // GetClassInfoEx
    { "USER", 399, 8, 1 },  // ChildWindowFromPointEx
    { "USER", 400, 0, 1 },  // FinalUserInit
    { "USER", 402, 6, 0 },  // GetPriorityClipboardFormat
    { "USER", 406, 18, 0 },  // CreateCursor
    { "USER", 407, 18, 0 },  // CreateIcon
    { "USER", 408, 14, 0 },  // CreateCursorIconIndirect
    { "USER", 409, 4, 1 },  // InitThreadInput
    { "USER", 417, 0, 0 },  // GetMenuCheckMarkDimensions
    { "USER", 418, 10, 1 },  // SetMenuItemBitmaps
    { "USER", 422, 10, 1 },  // DlgDirSelectEx
    { "USER", 423, 10, 1 },  // DlgDirSelectComboBoxEx
    { "USER", 427, 12, 0 },  // FindWindowEx
    { "USER", 428, 0, 1 },  // TileWindows
    { "USER", 429, 0, 1 },  // CascadeWindows
    { "USER", 430, 8, 1 },  // lstrcmp
    { "USER", 432, 4, 0 },  // AnsiLower
    { "USER", 433, 2, 1 },  // IsCharAlpha
    { "USER", 434, 2, 1 },  // IsCharAlphaNumeric
    { "USER", 437, 6, 1 },  // AnsiUpperBuff
    { "USER", 438, 6, 1 },  // AnsiLowerBuff
    { "USER", 441, 10, 1 },  // InsertMenuItem
    { "USER", 443, 0, 0 },  // GetMenuItemInfo
    { "USER", 445, 12, 0 },  // DefFrameProc
    { "USER", 446, 0, 1 },  // SetMenuItemInfo
    { "USER", 447, 10, 0 },  // DefMDIChildProc
    { "USER", 448, 12, 1 },  // DrawAnimatedRects
    { "USER", 449, 24, 1 },  // DrawState
    { "USER", 450, 20, 0 },  // CreateIconFromResourceEx
    { "USER", 451, 6, 1 },  // TranslateMDISysAccel
    { "USER", 455, 6, 0 },  // GetIconID
    { "USER", 456, 4, 0 },  // LoadIconHandler
    { "USER", 458, 2, 1 },  // DestroyCursor
    { "USER", 459, 16, 0 },  // DumpIcon
    { "USER", 460, 10, 0 },  // GetInternalWindowPos
    { "USER", 461, 12, 1 },  // SetInternalWindowPos
    { "USER", 462, 4, 1 },  // CalcChildScroll
    { "USER", 463, 10, 1 },  // ScrollChildren
    { "USER", 464, 12, 0 },  // DragObject
    { "USER", 465, 6, 1 },  // DragDetect
    { "USER", 466, 6, 1 },  // DrawFocusRect
    { "USER", 470, 0, 1 },  // StringFunc
    { "USER", 475, 10, 1 },  // SetScrollInfo
    { "USER", 476, 8, 0 },  // GetScrollInfo
    { "USER", 477, 4, 0 },  // GetKeyboardLayoutName
    { "USER", 478, 0, 0 },  // LoadKeyboardLayout
    { "USER", 479, 0, 1 },  // MenuItemFromPoint
    { "USER", 480, 0, 0 },  // GetUserLocalObjType
    { "USER", 482, 6, 1 },  // EnableScrollBar
    { "USER", 483, 10, 1 },  // SystemParametersInfo
    { "USER", 489, 0, 1 },  // USER_489
    { "USER", 490, 0, 1 },  // USER_490
    { "USER", 492, 0, 1 },  // USER_492
    { "USER", 496, 0, 1 },  // USER_496
    { "USER", 498, 0, 1 },  // BEAR498
    { "USER", 499, 8, 1 },  // WNetErrorText
    { "USER", 500, 0, 1 },  // FARCALLNETDRIVER
    { "USER", 501, 14, 1 },  // WNetOpenJob
    { "USER", 502, 10, 1 },  // WNetCloseJob
    { "USER", 503, 6, 1 },  // WNetAbortJob
    { "USER", 504, 6, 1 },  // WNetHoldJob
    { "USER", 505, 6, 1 },  // WNetReleaseJob
    { "USER", 506, 6, 1 },  // WNetCancelJob
    { "USER", 507, 8, 1 },  // WNetSetJobCopies
    { "USER", 508, 12, 1 },  // WNetWatchQueue
    { "USER", 509, 4, 1 },  // WNetUnwatchQueue
    { "USER", 510, 12, 1 },  // WNetLockQueueData
    { "USER", 511, 4, 1 },  // WNetUnlockQueueData
    { "USER", 514, 2, 1 },  // WNetDeviceMode
    { "USER", 515, 8, 1 },  // WNetBrowseDialog
    { "USER", 516, 8, 1 },  // WNetGetUser
    { "USER", 517, 12, 1 },  // WNetAddConnection
    { "USER", 518, 6, 1 },  // WNetCancelConnection
    { "USER", 519, 4, 1 },  // WNetGetError
    { "USER", 520, 10, 1 },  // WNetGetErrorText
    { "USER", 521, 0, 1 },  // WNetEnable
    { "USER", 522, 0, 1 },  // WNetDisable
    { "USER", 523, 6, 1 },  // WNetRestoreConnection
    { "USER", 524, 10, 1 },  // WNetWriteJob
    { "USER", 525, 4, 1 },  // WNetConnectDialog
    { "USER", 526, 4, 1 },  // WNetDisconnectDialog
    { "USER", 527, 4, 1 },  // WNetConnectionDialog
    { "USER", 528, 6, 1 },  // WNetViewQueueDialog
    { "USER", 529, 12, 1 },  // WNetPropertyDialog
    { "USER", 530, 8, 1 },  // WNetGetDirectoryType
    { "USER", 531, 8, 1 },  // WNetDirectoryNotify
    { "USER", 532, 16, 1 },  // WNetGetPropertyText
    { "USER", 533, 0, 1 },  // WNetInitialize
    { "USER", 534, 0, 1 },  // WNetLogon
    { "USER", 535, 0, 1 },  // WOWWORDBREAKPROC
    { "USER", 537, 0, 1 },  // MOUSEEVENT
    { "USER", 538, 0, 1 },  // KEYBDEVENT
    { "USER", 595, 0, 1 },  // OLDEXITWINDOWS
    { "USER", 600, 0, 0 },  // GetShellWindow
    { "USER", 601, 0, 1 },  // DoHotkeyStuff
    { "USER", 602, 0, 1 },  // SetCheckCursorTimer
    { "USER", 604, 0, 1 },  // BroadcastSystemMessage
    { "USER", 605, 0, 1 },  // HackTaskMonitor
    { "USER", 606, 22, 1 },  // FormatMessage
    { "USER", 608, 0, 0 },  // GetForegroundWindow
    { "USER", 609, 2, 1 },  // SetForegroundWindow
    { "USER", 610, 4, 1 },  // DestroyIcon32
    { "USER", 620, 8, 0 },  // ChangeDisplaySettings
    { "USER", 621, 12, 0 },  // EnumDisplaySettings
    { "USER", 640, 20, 0 },  // MsgWaitForMultipleObjects
    { "USER", 650, 0, 1 },  // ActivateKeyboardLayout
    { "USER", 651, 0, 0 },  // GetKeyboardLayout
    { "USER", 652, 0, 0 },  // GetKeyboardLayoutList
    { "USER", 654, 0, 1 },  // UnloadKeyboardLayout
    { "USER", 655, 0, 1 },  // PostPostedMessages
    { "USER", 656, 10, 1 },  // DrawFrameControl
    { "USER", 657, 18, 1 },  // DrawCaptionTemp
    { "USER", 658, 0, 1 },  // DispatchInput
    { "USER", 659, 10, 1 },  // DrawEdge
    { "USER", 660, 10, 1 },  // DrawCaption
    { "USER", 661, 0, 1 },  // SetSysColorsTemp
    { "USER", 662, 0, 1 },  // DrawMenubarTemp
    { "USER", 663, 0, 0 },  // GetMenuDefaultItem
    { "USER", 664, 0, 1 },  // SetMenuDefaultItem
    { "USER", 665, 10, 0 },  // GetMenuItemRect
    { "USER", 666, 10, 1 },  // CheckMenuRadioItem
    { "USER", 667, 0, 1 },  // TrackPopupMenuEx
    { "USER", 668, 6, 1 },  // SetWindowRgn
    { "USER", 669, 0, 0 },  // GetWindowRgn
    { "USER", 800, 0, 1 },  // CHOOSEFONT_CALLBACK16
    { "USER", 801, 0, 1 },  // FINDREPLACE_CALLBACK16
    { "USER", 802, 0, 1 },  // OPENFILENAME_CALLBACK16
    { "USER", 803, 0, 1 },  // PRINTDLG_CALLBACK16
    { "USER", 804, 0, 1 },  // CHOOSECOLOR_CALLBACK16
    { "USER", 819, 14, 1 },  // PeekMessage32
    { "USER", 820, 12, 0 },  // GetMessage32
    { "USER", 821, 6, 1 },  // TranslateMessage32
    { "USER", 822, 6, 0 },  // DispatchMessage32
    { "USER", 823, 8, 1 },  // CallMsgFilter32
    { "USER", 825, 0, 1 },  // PostMessage32
    { "USER", 826, 0, 1 },  // PostThreadMessage32
    { "USER", 827, 4, 1 },  // MessageBoxIndirect
    { "USER", 851, 0, 1 },  // MsgThkConnectionDataLS
    { "USER", 853, 0, 1 },  // FT_USRFTHKTHKCONNECTIONDATA
    { "USER", 854, 0, 1 },  // FT__USRF2THKTHKCONNECTIONDATA
    { "USER", 855, 0, 1 },  // Usr32ThkConnectionDataSL
    { "USER", 890, 0, 1 },  // InstallIMT
    { "USER", 891, 0, 1 },  // UninstallIMT
    { "USER", 902, 12, 0 },  // LoadSystemLanguageString
    { "USER", 905, 0, 1 },  // ChangeDialogTemplate
    { "USER", 906, 0, 0 },  // GetNumLanguages
    { "USER", 907, 10, 0 },  // GetLanguageName
    { "USER", 909, 8, 1 },  // SetWindowTextEx
    { "USER", 910, 0, 1 },  // BiDiMessageBoxEx
    { "USER", 911, 10, 1 },  // SetDlgItemTextEx
    { "USER", 912, 4, 0 },  // ChangeKeyboardLanguage
    { "USER", 913, 4, 0 },  // GetCodePageSystemFont
    { "USER", 914, 10, 0 },  // QueryCodePage
    { "USER", 915, 2, 0 },  // GetAppCodePage
    { "USER", 916, 26, 0 },  // CreateDialogIndirectParamML
    { "USER", 918, 24, 1 },  // DialogBoxIndirectParamML
    { "USER", 919, 12, 0 },  // LoadLanguageString
    { "USER", 920, 8, 0 },  // SetAppCodePage
    { "USER", 922, 0, 0 },  // GetBaseCodePage
    { "USER", 923, 12, 0 },  // FindLanguageResource
    { "USER", 924, 4, 0 },  // ChangeKeyboardCodePage
    { "USER", 930, 14, 1 },  // MessageBoxEx
    { "USER", 1000, 4, 1 },  // SetProcessDefaultLayout
    { "USER", 1001, 4, 0 },  // GetProcessDefaultLayout
    { "USER", 1010, 14, 0 },  // __wine_call_wndproc
    // ---- KERNEL additive coverage (367 ordinals) ----
    { "KERNEL", 2, 0, 1 },  // ExitKernel
    { "KERNEL", 11, 2, 1 },  // LocalHandle
    { "KERNEL", 12, 2, 1 },  // LocalFlags
    { "KERNEL", 13, 2, 1 },  // LocalCompact
    { "KERNEL", 14, 4, 0 },  // LocalNotify
    { "KERNEL", 22, 2, 1 },  // GlobalFlags
    { "KERNEL", 26, 2, 1 },  // GlobalFreeAll
    { "KERNEL", 27, 8, 0 },  // GetModuleName
    { "KERNEL", 28, 0, 0 },  // GlobalMasterHandle
    { "KERNEL", 31, 2, 1 },  // PostEvent
    { "KERNEL", 32, 4, 1 },  // SetPriority
    { "KERNEL", 33, 2, 1 },  // LockCurrentTask
    { "KERNEL", 34, 4, 1 },  // SetTaskQueue
    { "KERNEL", 35, 2, 0 },  // GetTaskQueue
    { "KERNEL", 38, 6, 0 },  // SetTaskSignalProc
    { "KERNEL", 39, 0, 1 },  // SetTaskSwitchProc
    { "KERNEL", 40, 0, 1 },  // SetTaskInterchange
    { "KERNEL", 41, 0, 1 },  // EnableDos
    { "KERNEL", 42, 0, 1 },  // DisableDos
    { "KERNEL", 43, 0, 1 },  // IsScreenGrab
    { "KERNEL", 44, 0, 1 },  // BuildPDB
    { "KERNEL", 45, 8, 0 },  // LoadModule
    { "KERNEL", 46, 2, 1 },  // FreeModule
    { "KERNEL", 53, 0, 1 },  // CallProcInstance
    { "KERNEL", 54, 6, 0 },  // GetInstanceData
    /* KERNEL.64 AccessResource: real handler (#Excel4) */
    { "KERNEL", 66, 8, 0 },  // AllocResource
    { "KERNEL", 67, 10, 0 },  // SetResourceHandler
    { "KERNEL", 68, 2, 1 },  // InitAtomTable
    { "KERNEL", 69, 4, 0 },  // FindAtom
    { "KERNEL", 70, 4, 1 },  // AddAtom
    { "KERNEL", 71, 2, 1 },  // DeleteAtom
    { "KERNEL", 72, 8, 0 },  // GetAtomName
    { "KERNEL", 73, 2, 0 },  // GetAtomHandle
    { "KERNEL", 75, 0, 0 },  // OpenPathName
    { "KERNEL", 76, 0, 1 },  // DeletePathName
    { "KERNEL", 77, 4, 0 },  // Reserved1
    { "KERNEL", 78, 8, 0 },  // Reserved2
    { "KERNEL", 79, 4, 0 },  // Reserved3
    { "KERNEL", 80, 4, 0 },  // Reserved4
    { "KERNEL", 87, 8, 1 },  // Reserved5
    { "KERNEL", 92, 2, 0 },  // GetTempDrive
    { "KERNEL", 93, 4, 0 },  // GetCodeHandle
    { "KERNEL", 97, 12, 0 },  // GetTempFileName
    { "KERNEL", 98, 0, 0 },  // GetLastDiskChange
    { "KERNEL", 99, 0, 0 },  // GetLPErrMode
    { "KERNEL", 100, 0, 1 },  // ValidateCodeSegments
    { "KERNEL", 101, 0, 1 },  // NoHookDosCall
    { "KERNEL", 103, 0, 0 },  // NetBIOSCall
    { "KERNEL", 104, 8, 0 },  // GetCodeInfo
    { "KERNEL", 105, 0, 0 },  // GetExeVersion
    { "KERNEL", 106, 2, 0 },  // SetSwapAreaSize
    { "KERNEL", 108, 6, 1 },  // SwitchStackTo
    { "KERNEL", 109, 0, 0 },  // SwitchStackBack
    { "KERNEL", 110, 2, 0 },  // PatchCodeHandle
    { "KERNEL", 111, 2, 0 },  // GlobalWire
    { "KERNEL", 112, 2, 1 },  // GlobalUnWire
    { "KERNEL", 116, 0, 1 },  // InitLib
    { "KERNEL", 117, 0, 1 },  // OldYield
    { "KERNEL", 118, 0, 0 },  // GetTaskQueueDS
    { "KERNEL", 119, 0, 0 },  // GetTaskQueueES
    { "KERNEL", 120, 0, 1 },  // UndefDynLink
    { "KERNEL", 121, 4, 1 },  // LocalShrink
    { "KERNEL", 122, 0, 1 },  // IsTaskLocked
    { "KERNEL", 123, 0, 1 },  // KbdRst
    { "KERNEL", 124, 0, 1 },  // EnableKernel
    { "KERNEL", 125, 0, 1 },  // DisableKernel
    { "KERNEL", 126, 0, 1 },  // MemoryFreed
    { "KERNEL", 130, 4, 0 },  // FileCDR
    /* KERNEL.132 GetWinFlags: real handler (#Excel4) */
    { "KERNEL", 133, 2, 0 },  // GetExePtr
    { "KERNEL", 138, 2, 0 },  // GetHeapSpaces
    { "KERNEL", 139, 0, 1 },  // DoSignal
    { "KERNEL", 140, 16, 1 },  // SetSigHandler
    { "KERNEL", 141, 0, 1 },  // InitTask1
    { "KERNEL", 142, 6, 0 },  // GetProfileSectionNames
    { "KERNEL", 143, 10, 0 },  // GetPrivateProfileSectionNames
    { "KERNEL", 144, 8, 0 },  // CreateDirectory
    { "KERNEL", 145, 4, 1 },  // RemoveDirectory
    { "KERNEL", 146, 4, 1 },  // DeleteFile
    { "KERNEL", 147, 4, 1 },  // SetLastError
    { "KERNEL", 148, 0, 0 },  // GetLastError
    { "KERNEL", 149, 4, 0 },  // GetVersionEx
    { "KERNEL", 150, 2, 1 },  // DirectedYield
    { "KERNEL", 151, 0, 1 },  // WinOldApCall
    { "KERNEL", 152, 0, 0 },  // GetNumTasks
    { "KERNEL", 154, 4, 1 },  // GlobalNotify
    { "KERNEL", 155, 0, 0 },  // GetTaskDS
    { "KERNEL", 156, 4, 0 },  // LimitEMSPages
    { "KERNEL", 157, 4, 0 },  // GetCurPID
    { "KERNEL", 158, 2, 1 },  // IsWinOldApTask
    { "KERNEL", 159, 2, 0 },  // GlobalHandleNoRIP
    { "KERNEL", 160, 0, 1 },  // EMSCopy
    { "KERNEL", 161, 0, 1 },  // LocalCountFree
    { "KERNEL", 162, 0, 1 },  // LocalHeapSize
    { "KERNEL", 163, 2, 1 },  // GlobalLRUOldest
    { "KERNEL", 164, 2, 1 },  // GlobalLRUNewest
    { "KERNEL", 165, 2, 1 },  // A20Proc
    { "KERNEL", 167, 2, 0 },  // GetExpWinVer
    { "KERNEL", 168, 6, 1 },  // DirectResAlloc
    { "KERNEL", 175, 2, 0 },  // AllocSelector
    { "KERNEL", 180, 8, 1 },  // LongPtrAdd
    { "KERNEL", 184, 4, 0 },  // GlobalDOSAlloc
    { "KERNEL", 185, 2, 1 },  // GlobalDOSFree
    { "KERNEL", 186, 2, 0 },  // GetSelectorBase
    { "KERNEL", 187, 6, 1 },  // SetSelectorBase
    { "KERNEL", 188, 2, 0 },  // GetSelectorLimit
    { "KERNEL", 189, 6, 1 },  // SetSelectorLimit
    { "KERNEL", 191, 2, 1 },  // GlobalPageLock
    { "KERNEL", 192, 2, 1 },  // GlobalPageUnlock
    { "KERNEL", 196, 6, 0 },  // SelectorAccessRights
    { "KERNEL", 197, 2, 1 },  // GlobalFix
    { "KERNEL", 198, 2, 1 },  // GlobalUnfix
    { "KERNEL", 200, 0, 1 },  // ValidateFreeSpaces
    { "KERNEL", 201, 0, 1 },  // ReplaceInst
    { "KERNEL", 202, 0, 1 },  // RegisterPtrace
    { "KERNEL", 203, 0, 0 },  // DebugBreak
    { "KERNEL", 204, 0, 1 },  // SwapRecording
    { "KERNEL", 205, 0, 1 },  // CVWBreak
    { "KERNEL", 206, 2, 0 },  // AllocSelectorArray
    { "KERNEL", 207, 2, 1 },  // IsDBCSLeadByte
    { "KERNEL", 208, 14, 0 },  // K208
    { "KERNEL", 209, 14, 0 },  // K209
    { "KERNEL", 210, 18, 0 },  // K210
    { "KERNEL", 211, 10, 0 },  // K211
    { "KERNEL", 213, 12, 0 },  // K213
    { "KERNEL", 214, 10, 0 },  // K214
    { "KERNEL", 215, 6, 0 },  // K215
    { "KERNEL", 216, 16, 0 },  // RegEnumKey
    { "KERNEL", 217, 12, 0 },  // RegOpenKey
    { "KERNEL", 218, 12, 0 },  // RegCreateKey
    { "KERNEL", 219, 8, 0 },  // RegDeleteKey
    { "KERNEL", 220, 4, 0 },  // RegCloseKey
    { "KERNEL", 221, 20, 0 },  // RegSetValue
    { "KERNEL", 222, 8, 0 },  // RegDeleteValue
    { "KERNEL", 223, 32, 0 },  // RegEnumValue
    { "KERNEL", 224, 16, 0 },  // RegQueryValue
    { "KERNEL", 225, 24, 0 },  // RegQueryValueEx
    { "KERNEL", 226, 24, 0 },  // RegSetValueEx
    { "KERNEL", 227, 4, 0 },  // RegFlushKey
    { "KERNEL", 228, 2, 1 },  // K228
    { "KERNEL", 229, 4, 1 },  // K229
    { "KERNEL", 230, 2, 0 },  // GlobalSmartPageLock
    { "KERNEL", 231, 2, 0 },  // GlobalSmartPageUnlock
    { "KERNEL", 232, 0, 1 },  // RegLoadKey
    { "KERNEL", 233, 0, 1 },  // RegUnloadKey
    { "KERNEL", 234, 0, 1 },  // RegSaveKey
    { "KERNEL", 235, 0, 1 },  // InvalidateNlsCache
    { "KERNEL", 236, 0, 0 },  // GetProductName
    { "KERNEL", 237, 0, 1 },  // K237
    { "KERNEL", 262, 0, 1 },  // WOWWaitForMsgAndEvent
    { "KERNEL", 263, 0, 1 },  // WOWMsgBox
    { "KERNEL", 273, 0, 1 },  // K273
    { "KERNEL", 274, 10, 0 },  // GetShortPathName
    { "KERNEL", 310, 2, 1 },  // LocalHandleDelta
    { "KERNEL", 311, 4, 0 },  // GetSetKernelDOSProc
    { "KERNEL", 314, 0, 1 },  // DebugDefineSegment
    { "KERNEL", 315, 0, 1 },  // WriteOutProfiles
    { "KERNEL", 316, 0, 0 },  // GetFreeMemInfo
    { "KERNEL", 318, 0, 1 },  // FatalExitHook
    { "KERNEL", 319, 0, 1 },  // FlushCachedFileHandle
    { "KERNEL", 320, 2, 1 },  // IsTask
    { "KERNEL", 323, 2, 1 },  // IsRomModule
    { "KERNEL", 324, 6, 1 },  // LogError
    { "KERNEL", 325, 10, 1 },  // LogParamError
    { "KERNEL", 326, 2, 1 },  // IsRomFile
    { "KERNEL", 327, 0, 0 },  // K327
    { "KERNEL", 328, 6, 1 },  // _DebugOutput
    { "KERNEL", 329, 6, 1 },  // K329
    { "KERNEL", 338, 4, 1 },  // HasGPHandler
    { "KERNEL", 339, 0, 1 },  // DiagQuery
    { "KERNEL", 340, 4, 1 },  // DiagOutput
    { "KERNEL", 341, 4, 0 },  // ToolHelpHook
    { "KERNEL", 343, 0, 1 },  // RegisterWinOldApHook
    { "KERNEL", 344, 0, 0 },  // GetWinOldApHooks
    { "KERNEL", 345, 2, 1 },  // IsSharedSelector
    { "KERNEL", 351, 0, 1 },  // BUNNY_351
    { "KERNEL", 352, 10, 0 },  // lstrcatn
    { "KERNEL", 353, 10, 0 },  // lstrcpyn
    { "KERNEL", 354, 2, 0 },  // GetAppCompatFlags
    { "KERNEL", 355, 6, 0 },  // GetWinDebugInfo
    { "KERNEL", 356, 4, 1 },  // SetWinDebugInfo
    { "KERNEL", 357, 4, 0 },  // MapSL
    { "KERNEL", 358, 4, 0 },  // MapLS
    { "KERNEL", 359, 4, 0 },  // UnMapLS
    { "KERNEL", 360, 10, 0 },  // OpenFileEx
    { "KERNEL", 361, 0, 1 },  // PIGLET_361
    { "KERNEL", 362, 0, 1 },  // ThunkTerminateProcess
    { "KERNEL", 365, 4, 0 },  // GlobalChangeLockCount
    { "KERNEL", 403, 4, 1 },  // FarSetOwner
    { "KERNEL", 404, 2, 1 },  // FarGetOwner
    { "KERNEL", 406, 18, 1 },  // WritePrivateProfileStruct
    { "KERNEL", 407, 18, 0 },  // GetPrivateProfileStruct
    { "KERNEL", 408, 0, 1 },  // KERNEL_408
    { "KERNEL", 409, 0, 1 },  // KERNEL_409
    { "KERNEL", 410, 0, 0 },  // CreateProcessFromWinExec
    { "KERNEL", 411, 8, 0 },  // GetCurrentDirectory
    { "KERNEL", 412, 4, 1 },  // SetCurrentDirectory
    { "KERNEL", 413, 8, 0 },  // FindFirstFile
    { "KERNEL", 414, 6, 0 },  // FindNextFile
    { "KERNEL", 415, 2, 0 },  // FindClose
    { "KERNEL", 416, 12, 1 },  // WritePrivateProfileSection
    { "KERNEL", 417, 8, 1 },  // WriteProfileSection
    { "KERNEL", 418, 14, 0 },  // GetPrivateProfileSection
    { "KERNEL", 419, 10, 0 },  // GetProfileSection
    { "KERNEL", 420, 4, 0 },  // GetFileAttributes
    { "KERNEL", 421, 8, 1 },  // SetFileAttributes
    { "KERNEL", 422, 20, 0 },  // GetDiskFreeSpace
    { "KERNEL", 423, 4, 1 },  // LogApiThk
    { "KERNEL", 431, 6, 1 },  // IsPeFormat
    { "KERNEL", 432, 0, 1 },  // FileTimeToLocalFileTime
    { "KERNEL", 434, 10, 1 },  // UnicodeToAnsi
    { "KERNEL", 435, 0, 0 },  // GetTaskFlags
    { "KERNEL", 436, 4, 1 },  // _ConfirmSysLevel
    { "KERNEL", 437, 4, 1 },  // _CheckNotSysLevel
    { "KERNEL", 438, 8, 1 },  // _CreateSysLevel
    { "KERNEL", 439, 4, 1 },  // _EnterSysLevel
    { "KERNEL", 440, 4, 1 },  // _LeaveSysLevel
    { "KERNEL", 441, 24, 0 },  // CreateThread16
    { "KERNEL", 442, 0, 0 },  // VWin32_EventCreate
    { "KERNEL", 443, 4, 0 },  // VWin32_EventDestroy
    { "KERNEL", 444, 6, 1 },  // Local32Info
    { "KERNEL", 445, 6, 1 },  // Local32First
    { "KERNEL", 446, 4, 1 },  // Local32Next
    { "KERNEL", 447, 0, 1 },  // WIN32_OldYield
    { "KERNEL", 448, 0, 1 },  // KERNEL_448
    { "KERNEL", 449, 0, 0 },  // GetpWin16Lock
    { "KERNEL", 450, 4, 0 },  // VWin32_EventWait
    { "KERNEL", 451, 4, 0 },  // VWin32_EventSet
    { "KERNEL", 452, 4, 0 },  // LoadLibrary32
    { "KERNEL", 453, 8, 0 },  // GetProcAddress32
    { "KERNEL", 456, 6, 0 },  // DefResourceHandler
    { "KERNEL", 457, 8, 0 },  // CreateW32Event
    { "KERNEL", 458, 4, 0 },  // SetW32Event
    { "KERNEL", 459, 4, 0 },  // ResetW32Event
    { "KERNEL", 460, 8, 0 },  // WaitForSingleObject
    { "KERNEL", 461, 16, 0 },  // WaitForMultipleObjects
    { "KERNEL", 462, 0, 0 },  // GetCurrentThreadId
    { "KERNEL", 463, 6, 0 },  // SetThreadQueue
    { "KERNEL", 464, 4, 0 },  // GetThreadQueue
    { "KERNEL", 465, 0, 1 },  // NukeProcess
    { "KERNEL", 466, 2, 1 },  // ExitProcess
    { "KERNEL", 467, 0, 1 },  // WOACreateConsole
    { "KERNEL", 468, 0, 1 },  // WOASpawnConApp
    { "KERNEL", 469, 0, 1 },  // WOAGimmeTitle
    { "KERNEL", 470, 0, 1 },  // WOADestroyConsole
    { "KERNEL", 471, 0, 0 },  // GetCurrentProcessId
    { "KERNEL", 472, 0, 0 },  // MapHInstLS
    { "KERNEL", 473, 0, 0 },  // MapHInstSL
    { "KERNEL", 474, 4, 0 },  // CloseW32Handle
    { "KERNEL", 475, 0, 0 },  // GetTEBSelectorFS
    { "KERNEL", 476, 4, 0 },  // ConvertToGlobalHandle
    { "KERNEL", 477, 0, 1 },  // WOAFullScreen
    { "KERNEL", 478, 0, 1 },  // WOATerminateProcess
    { "KERNEL", 479, 4, 0 },  // KERNEL_479
    { "KERNEL", 480, 0, 1 },  // _EnterWin16Lock
    { "KERNEL", 481, 0, 1 },  // _LeaveWin16Lock
    { "KERNEL", 482, 4, 0 },  // LoadSystemLibrary32
    { "KERNEL", 483, 4, 0 },  // MapProcessHandle
    { "KERNEL", 484, 10, 0 },  // SetProcessDword
    { "KERNEL", 485, 6, 0 },  // GetProcessDword
    { "KERNEL", 486, 4, 0 },  // FreeLibrary32
    { "KERNEL", 487, 10, 0 },  // GetModuleFileName32
    { "KERNEL", 488, 4, 0 },  // GetModuleHandle32
    { "KERNEL", 489, 0, 1 },  // KERNEL_489
    { "KERNEL", 490, 2, 1 },  // KERNEL_490
    { "KERNEL", 491, 8, 0 },  // RegisterServiceProcess
    { "KERNEL", 492, 0, 1 },  // WOAAbort
    { "KERNEL", 493, 16, 1 },  // UTInit
    { "KERNEL", 494, 0, 1 },  // KERNEL_494
    { "KERNEL", 495, 20, 0 },  // WaitForMultipleObjectsEx
    { "KERNEL", 500, 6, 0 },  // WOW16Call
    { "KERNEL", 501, 0, 1 },  // KDDBGOUT
    { "KERNEL", 502, 0, 1 },  // WOWGETNEXTVDMCOMMAND
    { "KERNEL", 503, 0, 1 },  // WOWREGISTERSHELLWINDOWHANDLE
    { "KERNEL", 504, 0, 1 },  // WOWLOADMODULE
    { "KERNEL", 505, 0, 1 },  // WOWQUERYPERFORMANCECOUNTER
    { "KERNEL", 506, 0, 1 },  // WOWCURSORICONOP
    { "KERNEL", 507, 0, 1 },  // WOWFAILEDEXEC
    { "KERNEL", 508, 0, 1 },  // WOWCLOSECOMPORT
    { "KERNEL", 511, 0, 1 },  // WOWKILLREMOTETASK
    { "KERNEL", 512, 0, 1 },  // WOWQUERYDEBUG
    { "KERNEL", 513, 12, 0 },  // LoadLibraryEx32W
    { "KERNEL", 514, 4, 0 },  // FreeLibrary32W
    { "KERNEL", 515, 8, 0 },  // GetProcAddress32W
    { "KERNEL", 516, 6, 0 },  // GetVDMPointer32W
    { "KERNEL", 517, 12, 0 },  // CallProc32W
    { "KERNEL", 518, 12, 0 },  // _CallProcEx32W
    { "KERNEL", 519, 0, 1 },  // EXITKERNELTHUNK
    { "KERNEL", 533, 0, 1 },  // ConvertDDEHandleLS
    { "KERNEL", 534, 0, 1 },  // ConvertDDEHandleSL
    { "KERNEL", 535, 8, 0 },  // VWin32_BoostThreadGroup
    { "KERNEL", 536, 8, 0 },  // VWin32_BoostThreadStatic
    { "KERNEL", 537, 0, 1 },  // KERNEL_537
    { "KERNEL", 538, 0, 1 },  // ThunkTheTemplateHandle
    { "KERNEL", 540, 0, 1 },  // KERNEL_540
    { "KERNEL", 541, 0, 1 },  // WOWSETEXITONLASTAPP
    { "KERNEL", 542, 0, 1 },  // KERNEL_542
    { "KERNEL", 543, 0, 1 },  // KERNEL_543
    { "KERNEL", 544, 0, 1 },  // WOWSETCOMPATHANDLE
    { "KERNEL", 560, 8, 0 },  // SetThunkletCallbackGlue
    { "KERNEL", 561, 8, 0 },  // AllocLSThunkletCallback
    { "KERNEL", 562, 8, 0 },  // AllocSLThunkletCallback
    { "KERNEL", 563, 8, 0 },  // FindLSThunkletCallback
    { "KERNEL", 564, 8, 0 },  // FindSLThunkletCallback
    { "KERNEL", 566, 0, 1 },  // KERNEL_566
    { "KERNEL", 567, 10, 0 },  // AllocLSThunkletCallbackEx
    { "KERNEL", 568, 10, 0 },  // AllocSLThunkletCallbackEx
    { "KERNEL", 600, 0, 0 },  // AllocCodeAlias
    { "KERNEL", 601, 0, 1 },  // FreeCodeAlias
    { "KERNEL", 602, 0, 0 },  // GetDummyModuleHandleDS
    { "KERNEL", 603, 0, 1 },  // KERNEL_603
    { "KERNEL", 604, 0, 0 },  // CBClientGlueSL
    { "KERNEL", 605, 8, 0 },  // AllocSLThunkletCallback_dup
    { "KERNEL", 606, 8, 0 },  // AllocLSThunkletCallback_dup
    { "KERNEL", 607, 12, 0 },  // AllocLSThunkletSysthunk
    { "KERNEL", 608, 12, 0 },  // AllocSLThunkletSysthunk
    { "KERNEL", 609, 8, 0 },  // FindLSThunkletCallback_dup
    { "KERNEL", 610, 8, 0 },  // FindSLThunkletCallback_dup
    { "KERNEL", 611, 8, 1 },  // FreeThunklet
    { "KERNEL", 612, 4, 1 },  // IsSLThunklet
    { "KERNEL", 613, 0, 1 },  // HugeMapLS
    { "KERNEL", 614, 0, 1 },  // HugeUnMapLS
    { "KERNEL", 615, 12, 1 },  // ConvertDialog32To16
    { "KERNEL", 616, 12, 1 },  // ConvertMenu32To16
    { "KERNEL", 617, 4, 0 },  // GetMenu32Size
    { "KERNEL", 618, 4, 0 },  // GetDialog32Size
    { "KERNEL", 619, 10, 1 },  // RegisterCBClient
    { "KERNEL", 620, 0, 0 },  // CBClientThunkSL
    { "KERNEL", 621, 0, 0 },  // CBClientThunkSLEx
    { "KERNEL", 622, 10, 1 },  // UnRegisterCBClient
    { "KERNEL", 623, 4, 1 },  // InitCBClient
    { "KERNEL", 624, 8, 0 },  // SetFastQueue
    { "KERNEL", 625, 0, 0 },  // GetFastQueue
    { "KERNEL", 626, 0, 1 },  // SmashEnvironment
    { "KERNEL", 627, 10, 1 },  // IsBadFlatReadWritePtr
    { "KERNEL", 630, 0, 0 },  // C16ThkSL
    { "KERNEL", 631, 0, 0 },  // C16ThkSL01
    { "KERNEL", 651, 24, 0 },  // ThunkConnect16
    { "KERNEL", 652, 0, 1 },  // IsThreadId
    { "KERNEL", 653, 0, 1 },  // OkWithKernelToChangeUsers
    { "KERNEL", 666, 16, 0 },  // UTGlue16
    { "KERNEL", 667, 4, 0 },  // EntryAddrProc
    { "KERNEL", 668, 6, 0 },  // MyAlloc
    { "KERNEL", 669, 16, 1 },  // DllEntryPoint
    { "KERNEL", 700, 0, 0 },  // SSInit
    { "KERNEL", 701, 0, 1 },  // SSOnBigStack
    { "KERNEL", 702, 0, 1 },  // SSCall
    { "KERNEL", 703, 0, 1 },  // CallProc32WFix
    { "KERNEL", 704, 0, 0 },  // SSConfirmSmallStack
    { "KERNEL", 901, 0, 0 },  // __wine_vxd_vmm
    { "KERNEL", 905, 0, 0 },  // __wine_vxd_timer
    { "KERNEL", 909, 0, 0 },  // __wine_vxd_reboot
    { "KERNEL", 910, 0, 0 },  // __wine_vxd_vdd
    { "KERNEL", 912, 0, 0 },  // __wine_vxd_vmd
    { "KERNEL", 914, 0, 0 },  // __wine_vxd_comm
    { "KERNEL", 923, 0, 0 },  // __wine_vxd_shell
    { "KERNEL", 933, 0, 0 },  // __wine_vxd_pagefile
    { "KERNEL", 938, 0, 0 },  // __wine_vxd_apm
    { "KERNEL", 939, 0, 0 },  // __wine_vxd_vxdloader
    { "KERNEL", 945, 0, 0 },  // __wine_vxd_win32s
    { "KERNEL", 951, 0, 0 },  // __wine_vxd_configmg
    { "KERNEL", 955, 0, 0 },  // __wine_vxd_enable
    { "KERNEL", 1990, 0, 0 },  // __wine_vxd_timerapi
    { "KERNEL", 2000, 2, 0 },  // __wine_call_int_handler
    { "KERNEL", 2001, 0, 0 },  // __wine_snoop_entry
    { "KERNEL", 2002, 0, 0 },  // __wine_snoop_return
};
#define STUB_TABLE_LEN ((int)(sizeof(g_stub_table)/sizeof(g_stub_table[0])))

static const win16_stub_entry_t *find_stub(const win16_import_t *im) {
    if (!im || !im->by_ordinal) return 0;
    for (int i = 0; i < STUB_TABLE_LEN; i++) {
        const win16_stub_entry_t *e = &g_stub_table[i];
        if (e->ordinal == im->ordinal && ci_eq(e->module, im->module)) return e;
    }
    return 0;
}

// ---------------------------------------------------------------------------
void win16_api_begin(const win16_loader_info_t *info,
                     const win16_import_t *imports, int import_count) {
    g_info = *info;
    g_imports = imports;
    g_import_count = import_count;
    g_api_calls = 0;
    heap_reset();
    g_lheap_next = g_info.lheap_base;
    g_lheap_top  = g_info.lheap_top;
    // (#278 Word6 pass24) Give the MAIN APP's DGROUP its OWN local-heap window in
    // the per-segment lseg table so its LocalAllocs do NOT fall back to the SHARED
    // bump pointer (g_lheap_next). A DLL with no DGROUP of its own (SDM.DLL,
    // autodata=0) -- or any DLL LibMain -- calls LocalInit, which sets the shared
    // fallback to its own tiny window (next=2, top=0x800); without a dedicated entry
    // Word's DGROUP allocs then used that fallback, basing the C-runtime argv/_setargv
    // tables at offset 2 ON TOP of Word's static globals (corrupting the object-handle
    // global at DGROUP:0x158 -> NULL object -> seg227 pure-compute hang). A dedicated
    // entry keyed by the app's DGROUP selector is immune to the shared-fallback clobber.
    if (g_info.ds && g_info.segcount >= 100) {
        // Dedicated kernel C-runtime arena in the GAP between the guest MS-C local-heap
        // top and the in-DGROUP stack. KERNEL.5 LocalAlloc (MS-C _setargv/_nmalloc) is
        // served from HERE so it does not land in -- and corrupt the free-block headers
        // of -- the guest MS-C heap [end-of-static, heap_top); that corruption garbled
        // Word's object free-chain into a cycle (seg132 stack-overflow recursion).
        // (#278 Word6 pass26) The dedicated scratch window occupies
        // [lheap_top, lheap_top+WIN16_W6_SCRATCH); the KERNEL.5 C-runtime arena
        // starts ABOVE it so the two never overlap.
        uint16_t cbase = (uint16_t)(g_info.lheap_top + WIN16_W6_SCRATCH);
        uint32_t sfloor = (uint32_t)g_info.sp > (uint32_t)g_info.ne_stack
                          ? (uint32_t)g_info.sp - (uint32_t)g_info.ne_stack : (uint32_t)g_info.sp;
        uint16_t ctop = (sfloor > (uint32_t)cbase + 0x80) ? (uint16_t)(sfloor - 0x80) : cbase;
        if (ctop > cbase) {
            lseg_t *e = lseg_get_or_add(g_info.ds);
            if (e) { e->next = cbase; e->top = ctop; }
        }
    }
    g_dgroup_sel      = g_info.ds;
    g_dgroup_heap_top = g_info.lheap_top;
    win16_dgroup_sel = g_info.ds;
    win16_dgroup_heap_base = g_info.lheap_base;
    win16_trace("LHEAP base=%04x top=%04x (size=%u)\n",
                g_lheap_next, g_lheap_top,
                (unsigned)(g_lheap_top - g_lheap_next));

    // Reset the Win16 GUI bridge state for this run.
    if (g_w16_menu_save) { kfree(g_w16_menu_save); g_w16_menu_save = 0; }
    g_w16_menu_save_t = -1;
    for (int i = 0; i < WIN16_MAX_CLASSES; i++) g_classes[i].used = 0;
    for (int i = 0; i < WIN16_MAX_WINDOWS; i++) g_windows[i].used = 0;
    for (int i = 0; i < WIN16_MAX_GDIOBJ; i++) {
        if (g_gdiobj[i].used && g_gdiobj[i].type == 4 && g_gdiobj[i].pix)
            kfree(g_gdiobj[i].pix);
        g_gdiobj[i].used = 0; g_gdiobj[i].pix = 0;
    }
    for (int i = 0; i < WIN16_MAX_DC; i++) g_dcs[i].used = 0;
    for (int i = 0; i < WIN16_MAX_FILES; i++) {
        if (g_files[i].used && g_files[i].buf) kfree(g_files[i].buf);
        g_files[i].used = 0; g_files[i].buf = 0;
    }
    for (int i = 0; i < WIN16_MAX_TIMERS; i++) g_timers[i].used = 0;
    g_msgq_head = g_msgq_tail = g_msgq_count = 0;
    g_dc_save_top = 0;   // (#188) reset DC save stack
    g_quit_posted = 0;
    // (#278 Word6) reset + seed the SHELL.DLL registration database for this run.
    for (int i = 0; i < SHREG_MAX; i++) g_shreg[i].used = 0;
    for (int i = 0; i < SHKEY_MAX; i++) g_shkeys[i].used = 0;
    g_shell_next_hkey = 0x0100;
    shreg_seed_root();
    g_win16_z_counter = 0;   // (#207) reset activation stamps per run
    g_win16_z_dirty = 1;     // (#209) first idle frame does a full repaint
    g_win16_no_bg_fill = 0;  // (#209) default: bg-filled paints
    g_autostart_done = 0;
    g_cpu = 0;
    g_taskbar_win = 0;   // no kernel taskbar entry until CreateWindow registers one
    g_trace_len = 0;
    g_trace_last_flush_len = 0;
    g_trace_flushed = 0;
    g_wndproc_trace_n = 0;
    g_call_trace_n = 0;
    g_win16_app_kind = 0;   // ne.c sets this right after begin (per loaded modules)
    for (int i = 0; i < 1024; i++) g_import_seen[i] = 0;
    win16_trace("=== Win16 run, %d imports ===\n", import_count);
    // Fold the app-DLL import resolution captured during relocation (it was
    // recorded before this reset) into the run trace, then clear it for next run.
    if (g_reloc_log_len > 0) {
        if (g_trace_len + g_reloc_log_len + 1 < WIN16_TRACE_CAP) {
            for (uint32_t i = 0; i < g_reloc_log_len; i++)
                g_trace[g_trace_len + i] = g_reloc_log[i];
            g_trace_len += g_reloc_log_len;
        }
        g_reloc_log_len = 0;
    }

    // Grab the keyboard for the Win16 app while it runs: SYS_GET_KEYBOARD returns
    // -1 to the compositor so the Win16 message pump (win16_pump_input) is the
    // sole keyboard consumer. The Win16 window itself is a normal composited user
    // window (see win16_host_create in CreateWindow), so the desktop, taskbar and
    // mouse stay live; we no longer take over the framebuffer (#144/#145).
    extern volatile int g_win16_owns_screen;
    g_win16_owns_screen = 1;
    g_w6_tip_dismissed = 0;   // (#278 pass31) re-arm Tip dismiss for this run

    // (#215) Clear any stale close request from a prior run before this one starts.
    g_win16_close_requested = 0;

    // Mouse-forwarding state (#187) reset per run.
    g_w16_pbtn = 0; g_w16_pcvx = -100000; g_w16_pcvy = -100000;
    g_w16_lclick_t = 0; g_w16_menu_open = -1;
    g_w16_menu_prev = -1; g_dlg_active = 0; g_dlg_end = 0; g_dlg_nitems = 0;

    // No host window / canvas yet; CreateWindow allocates it.
    g_win16_host_slot = -1;
    g_win16_main_hwnd = 0;
    g_win16_canvas    = 0;
    g_win16_canvas_w  = 0;
    g_win16_canvas_h  = 0;

    // (#210) Reset the soft-repaint scratch buffer for the new run (freed in
    // win16_api_end; a non-NULL stale pointer here would be from a prior run
    // that already freed it, so just clear it).
    if (g_win16_scratch) { kfree(g_win16_scratch); g_win16_scratch = 0; }
    g_win16_scratch_w = 0;
    g_win16_scratch_h = 0;
}

// Timer tick counter (250 Hz) used to hold the painted Win16 window on screen
// briefly after the run so it is visible / screenshot-able before the desktop
// or userland compositor repaints over the front buffer. (timer_ticks is
// declared extern near the top of this file alongside the GetMessage pump.)

void win16_api_end(void) {
    // The interpreter ran the message loop until WM_QUIT and has now exited.
    extern volatile int g_win16_owns_screen;
    // Flush + close any files the app left open (best-effort write-back).
    for (int i = 0; i < WIN16_MAX_FILES; i++) {
        if (!g_files[i].used) continue;
        if (g_files[i].dirty)
            fat_write_file(&g_fat_fs, g_files[i].path, g_files[i].buf, g_files[i].size);
        if (g_files[i].buf) kfree(g_files[i].buf);
        g_files[i].used = 0; g_files[i].buf = 0;
    }

    // Destroy the composited host window + free its canvas (#144/#145) so the
    // Win16 window + taskbar button disappear when the app exits.
    // (#278 pass27 PERSISTENCE) EXCEPTION for Word 6 (segcount>=100): Word's
    // phase-2 OLE2 init gate fails in our environment and Word self-terminates
    // moments after creating + painting its main window. Tearing the host window
    // down here would make that window vanish after ~1s. Instead, KEEP the host
    // window + canvas alive so Word's painted main editing window stays visible
    // and persistent on the desktop (composited every frame by the fb_swap_buffers
    // hook). The window is kernel-owned (owner_pid 0) so it already survives the
    // Win16 process exit; we just skip the destroy. A user can still close it via
    // the titlebar X (win16_host_close_handler -> win16_request_close).
    if (g_info.segcount >= 100) {
        kprintf("[W6PERSIST] keeping Word host window (slot %d) visible after process exit\n",
                g_win16_host_slot);
        // Leave g_win16_host_slot / canvas pointers intact for the kept window.
    } else {
        if (g_win16_host_slot >= 0) {
            win16_host_destroy(g_win16_host_slot);
            g_win16_host_slot = -1;
        }
        g_taskbar_win    = 0;   // destroyed by win16_host_destroy
        g_win16_canvas   = 0;
        g_win16_canvas_w = 0;
        g_win16_canvas_h = 0;
    }

    // (#210) Free the off-screen soft-repaint scratch buffer.
    if (g_win16_scratch) { kfree(g_win16_scratch); g_win16_scratch = 0; }
    g_win16_scratch_w = 0;
    g_win16_scratch_h = 0;

    // Release the keyboard back to the compositor.
    g_win16_owns_screen = 0;
    g_cpu = 0;

    // (#215) Slot is now free; clear the close-request latch so a future launch
    // does not immediately self-quit.
    g_win16_close_requested = 0;

    // Persist the API/loader trace so it can be inspected from the RC console
    // (kprintf is not routed to this VM's serial socket).
    if (g_trace_len > 0)
        fat_write_file(&g_fat_fs, "/WIN16LOG.TXT", g_trace, g_trace_len);
}

// Trace sink for the NE loader (ne.c) to record module/DLL/resource events.
void win16_trace_str(const char *s) { win16_trace("%s", s); }

// (#278 pass62) Forward-progress signal for the NE runaway guard. Every serviced
// Win16 API call bumps this. ne.c's per-run instruction-budget cap resets its
// budget whenever this advances during a slice, so a HEALTHY interactive app
// (one that keeps pumping messages / making API calls, e.g. Word 6 idling in its
// PeekMessage loop) is never killed by the cap, while a genuinely stuck runaway
// (a null far-call spin executing op=00 forever, which makes NO API calls) still
// self-terminates at the cap. This is what lets Word stay alive so its message
// pump keeps delivering keystrokes to the edit pane (typing).
volatile unsigned long g_win16_api_progress = 0;
int win16_api_dispatch(x86_16_cpu_t *c, uint16_t off) {
    g_win16_api_progress++;
    g_cpu = c;   // GDI/wndproc helpers read 16-bit memory through this CPU.
    // Read the caller's far-return frame (pushed by the interpreter trap).
    uint16_t ret_ip = x86_16_rd16(c, c->ss, c->sp);
    uint16_t ret_cs = x86_16_rd16(c, c->ss, (uint16_t)(c->sp + 2));

    const win16_import_t *im = 0;
    if (off < (uint16_t)g_import_count) im = &g_imports[off];

    // (#278 P52 runtime-callf-trace follow-up 4) name EVERY win16 API call whose
    // call site is in seg155's doc-create window [0x9be,0xb89] (sel 0x04df). The
    // 0x9A CALLF handler's own [W6CALLF] trace SILENTLY MISSES these because the
    // g_farcall_active/g_farcall_seg trap short-circuits (return/break) BEFORE
    // reaching that trace code whenever the far-call target lands in the win16
    // API landing-pad segment - so real API calls (confirmed: ShowWindow at
    // 0xa2d) never appear there. This hook is upstream of that short-circuit.
    { extern int g_w6life, g_w6seq;
      if (g_w6life && ret_cs==0x04df && ret_ip>=0x09c3 && ret_ip<=0x0b8e) {
        static int napi=0; if (napi<40) { napi++;
          if (im && im->by_ordinal)
            kprintf("[W6DCAPI] SEQ %d: seg155:%04x call site ~%04x -> %s.#%u\n",
                    g_w6seq++, ret_ip, (unsigned)(ret_ip-5), im->module, im->ordinal);
          else if (im)
            kprintf("[W6DCAPI] SEQ %d: seg155:%04x call site ~%04x -> %s.%s\n",
                    g_w6seq++, ret_ip, (unsigned)(ret_ip-5), im->module, im->name);
        } } }

    // (#188) Per-call API dispatch trace. Logging EVERY Win16 API call to the
    // 115200 serial port blocks for ~2.6ms/line, so a running app (which makes
    // thousands of calls/sec) pins the CPU in serial output and starves the rest
    // of the OS (it was drowning unrelated subsystems' logs and stalling the net
    // stack). Gated behind g_win16_apilog (default OFF); flip to 1 for ordinal
    // discovery. The first-seen MISS trace below is unaffected.
    extern int g_win16_apilog;
    if (g_win16_apilog) {
        if (im) {
            if (im->by_ordinal)
                kprintf("[win16api] %s.#%u\n", im->module, im->ordinal);
            else
                kprintf("[win16api] %s.%s\n", im->module, im->name);
        } else {
            kprintf("[win16api] <unknown import id %u>\n", off);
        }
    }

    uint16_t ax = 0, dx = 0, argbytes = 0;
    win16_handler_fn fn = im ? find_handler(im) : 0;
    // Trace each distinct import once (handled or not) so the disk log shows the
    // exact API surface an app uses without flooding on per-frame calls.
    // Trace each distinct import once (handled or not) so the disk log shows the
    // full API surface + lifecycle without flooding on per-frame calls.
    if (off < 1024 && !g_import_seen[off]) {
        g_import_seen[off] = 1;
        if (im) {
            if (im->by_ordinal)
                win16_trace("%s %s.#%u\n", fn ? "  ok" : "MISS", im->module, im->ordinal);
            else
                win16_trace("%s %s.%s\n", fn ? "  ok" : "MISS", im->module, im->name);
        }
    }
    // (#278 P52 runtime-callf-trace, avenue 1) latch the GDI ordinal about to be
    // dispatched so g_createcompatibledc (which serves BOTH GDI.52 CreateCompatibleDC
    // [1w=2b] and GDI.53 CreateDC [8w=16b] via the SAME handler, hardcoding
    // argbytes=2) can tell which one actually fired. If Word calls real CreateDC
    // through this alias, the Pascal far-return only pops 2 of the 16 argument
    // bytes the caller pushed, leaving 14 stray bytes on the stack after return -
    // a stack desync exactly the class of bug pass-35 fixed for DS drift.
    { extern int g_w6last_gdi_ord; g_w6last_gdi_ord = (im && im->module[0]=='G') ? (int)im->ordinal : -1; }
    if (fn) {
        fn(c, &ax, &dx, &argbytes);
        // (#289 b464) OLE2 memory/selector ordinal trace: show every KERNEL alloc/
        // lock/handle call STORAGE makes plus its return DX:AX, so a NULL object
        // (segment 0) can be traced to the allocation that produced it. Gated by
        // the pmode-only g_ole2_k334log.
        extern int g_ole2_k334log;
        if (g_ole2_k334log && im && im->by_ordinal &&
            im->module[0]=='K' && im->module[1]=='E' && im->module[2]=='R') {
            unsigned o = im->ordinal;
            if (o==4||o==5||o==6||o==8||o==9||o==10||o==15||o==16||o==17||o==18||o==19||
                o==20||o==21||o==23||o==24||o==25||o==334||o==337||o==350)
                kprintf("[KIMP] KERNEL.#%u a0=%04x a1=%04x a2=%04x -> dx:ax=%04x:%04x\n",
                        o, arg16(c,0), arg16(c,1), arg16(c,2), dx, ax);
        }
    } else {
        const win16_stub_entry_t *st = find_stub(im);
        if (st) {
            // (#188) Known ordinal, no dedicated handler: clean the Pascal stack
            // with the SPEC-exact argbytes so the caller does not desync.
            ax = st->retval; dx = 0; argbytes = st->argbytes;
            // (#278 P52 runtime-callf-trace, avenue 1) GDI.38 Escape is an
            // unimplemented stub (always returns 0/argbytes=14). If Word calls
            // Escape(GETPHYSPAGESIZE/GETPRINTINGOFFSET/...) to size the page for
            // layout and gets a hardcoded 0, that is a plausible reason pagination
            // never completes. Log every hit with the raw first two stack words
            // (nEscape code is conventionally the first Pascal-pushed word).
            { extern int g_w6life, g_w6seq;
              if (g_w6life && im && im->module[0]=='G' && im->ordinal==38) {
                static int nesc=0; if (nesc<20) { nesc++;
                  kprintf("[W6ESCAPE] SEQ %d: GDI.38 Escape() args[0]=%04x args[1]=%04x -> stub ax=0\n",
                          g_w6seq++, arg16(c,0), arg16(c,1)); } } }
        } else {
            if (im) {
                if (im->by_ordinal)
                    kprintf("[win16api] UNIMPL %s.#%u (args unknown)\n",
                            im->module, im->ordinal);
                else
                    kprintf("[win16api] UNIMPL %s.%s (args unknown)\n",
                            im->module, im->name);
            }
            // Unknown arg count: cannot clean the stack. Return 0 and pop only the
            // far frame (Phase-1 behaviour) so discovery can continue.
            ax = 0; dx = 0; argbytes = 0;
        }
    }

    { extern int g_ole2_k334log; static int nd=0;
      if (g_ole2_k334log && off==0x000f && nd<8){ nd++;
        kprintf("[P21DISP] off=000f im=%s.%u byord=%d fn=%p ret=%04x:%04x ax=%04x argb=%04x sp=%04x\n",
          im?im->module:"?", im?im->ordinal:0, im?im->by_ordinal:-1, (void*)fn, ret_cs, ret_ip, ax, argbytes, c->sp); } }
    // Pascal-style far return: pop IP+CS, then discard the callee's arguments.
    c->cs = ret_cs;
    c->ip = ret_ip;
    c->sp = (uint16_t)(c->sp + 4 + argbytes);
    c->ax = ax;
    c->dx = dx;

    if (++g_api_calls > WIN16_API_CALL_CAP) {
        kprintf("[win16] API call cap (%d) reached -> stopping CPU\n",
                WIN16_API_CALL_CAP);
        c->halted = 1;
        return 1;
    }
    return 0;
}

/* --- #278 Word6/OLE2 merge: WIN16PM.RUN boot auto-launcher (ported from ole2c) --- */
extern void *proc_create(const char *name, void (*fn)(void *), void *arg, int prio);
extern void  proc_sleep(unsigned ms);
extern int   g_win16_want_pmode;
 extern int win16_launch(const char *path);
extern char  g_win16_cmdtail[128];
static void win16_autolaunch_thread(void *arg) {
    (void)arg;
    kprintf("[WIN16AUTO] thread alive, settling before probe\n");
    for (int i = 0; i < 200; i++) proc_sleep(50);
    uint32_t sz = 0; void *d = 0;
    for (int tries = 0; tries < 20 && !d; tries++) {
        d = fat_read_file(&g_fat_fs, "/CONFIG/WIN16PM.RUN", &sz);
        if (!d) { for (int k = 0; k < 20; k++) proc_sleep(50); }
    }
    kprintf("[WIN16AUTO] WIN16PM.RUN read: d=%d sz=%lu\n", d ? 1 : 0, (unsigned long)sz);
    if (!d || sz == 0) { if (d) kfree(d); return; }
    char path[96]; uint32_t n = 0; char *p = (char *)d;
    while (n < sz && n < sizeof(path) - 1) { char ch = p[n]; if (ch==13||ch==10||ch==32||ch==0) break; path[n]=ch; n++; }
    path[n] = 0;
    int want_pm = 0;
    { uint32_t m = n; while (m<sz && (p[m]==32||p[m]==9||p[m]==13||p[m]==10)) m++;
      if (m+1<sz && (p[m]==0x70||p[m]==0x50) && (p[m+1]==0x6d||p[m+1]==0x4d)) want_pm=1; }
    kfree(d);
    if (!path[0]) return;
    kprintf("[WIN16AUTO] launching %s (%s)\n", path, want_pm?"pmode":"realmode");
    g_win16_cmdtail[0] = 0;
    g_win16_want_pmode = want_pm;
    win16_launch(path);
}
void win16_autolaunch_init(void) { proc_create("win16auto", win16_autolaunch_thread, 0, 2); }
