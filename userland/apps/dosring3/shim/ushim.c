// ushim.c - the LIBC-universe half of the dosring3 shim (#DOSRING3).
//
// Compiled with the userland include path ONLY. It never sees a kernel header,
// so the kernel/libc type collision cannot arise here. Everything it exports is
// declared in kbridge.h using primitive types alone.
#include "../../../libc/syscall.h"
#include "../../../libc/stdlib.h"
#include "../../../libc/string.h"
#include "../../../libc/unistd.h"
#include "../../../libc/fcntl.h"
#include "../../../libc/time.h"
#include "../../../libc/dirent.h"
#include "../../../libc/sys/stat.h"
#include "../../../libc/pthread.h"
#include "../../../libc/userconf.h"
#include "kbridge.h"

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void kb_log(const char *bytes, unsigned long len) {
    // FD 1, NOT FD 2, AND THIS COST A WHOLE VM RUN TO FIND.
    //
    // The first live Ring-3 run produced the host's own printf() line and then
    // NOTHING - no kprintf output at all from ~40 seconds of an interpreter
    // demonstrably burning 85% of a core. It read exactly like a hang. It was
    // not: printf goes to fd 1 and arrived, every kprintf went to fd 2 and did
    // not, so the entire DOS layer's diagnostic stream was being written to a
    // descriptor that goes nowhere for a kernel-spawned process.
    //
    // An observability bug is indistinguishable from a liveness bug from the
    // outside, and it is more expensive, because it makes every subsequent
    // measurement blind. Route to the descriptor that is PROVEN to reach the
    // console, which is the one printf already uses.
    if (bytes && len) (void)write(1, bytes, (size_t)len);
}

void kb_abort(const char *msg) {
    if (msg) { (void)write(2, msg, strlen(msg)); (void)write(2, "\n", 1); }
    exit(70);   // EX_SOFTWARE: an internal invariant failed, loudly.
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// Memory. The DOS arenas are all heap in the kernel too (kmalloc), so this is
// a like-for-like substitution rather than a redesign. The libc heap ceiling
// is 512 MB with page-granular mmap backing, comfortably above the 32 MiB
// DOS/4GW arena and the 1 MiB conventional-memory arena.
// ---------------------------------------------------------------------------
void *kb_malloc(unsigned long n) { return malloc((size_t)n); }
void  kb_free(void *p)           { free(p); }

// ---------------------------------------------------------------------------
// Filesystem
//
// WHY POSIX open() IS THE RIGHT TARGET AND NOT A DOWNGRADE. In the kernel the
// DOS layer calls fat_*() against g_fat_fs, and on the two-partition image
// that FAT API is ALREADY the routing layer that dispatches to ext2 by path
// (fat_path_on_ext2()). The kernel's own open() does the same routing. So this
// is the same dispatch, reached through the syscall boundary instead of a
// direct call - and it additionally applies THIS PROCESS's real credentials to
// every access, which the Ring-0 path could not do (see the guestfs note in
// kshim.c).
// ---------------------------------------------------------------------------
long kb_open(const char *path, int mode) {
    int fl = (mode == KB_O_WRITE) ? (O_WRONLY | O_CREAT | O_TRUNC)
           : (mode == KB_O_RDWR)  ? (O_RDWR   | O_CREAT)
                                  :  O_RDONLY;
    int fd = open(path, fl, 0644);
    return (long)fd;
}
long kb_read (long fd, void *buf, unsigned long n)       { return read((int)fd, buf, (size_t)n); }
long kb_write(long fd, const void *buf, unsigned long n) { return write((int)fd, buf, (size_t)n); }
long kb_seek (long fd, long off, int whence)             { return (long)lseek((int)fd, (off_t)off, whence); }
void kb_close(long fd)                                   { if (fd >= 0) close((int)fd); }

// ---------------------------------------------------------------------------
// #VOLAPI: the mediated volume gateway, userland side.
//
// One syscall, no caching. Caching WAS considered and rejected for now: a disc
// SWAP is the case this feature exists for (Red Alert ships one disc per faction
// and asks for the other one mid-game), and a stale cache is how a swap becomes
// "the game still wants a disc that is already in the drive". `gen` exists to
// detect that, so a cache is possible later, but it is a correctness risk taken
// for a saving nobody has measured a need for: the DOS layer already makes a
// real syscall per INT 21h open and read, so one more per drive-validity query
// is not the expensive thing here. Say what is measured and what is not: this
// cost has NOT been measured, and if Red Alert turns out to be slower for it,
// this is the first place to look.
// ---------------------------------------------------------------------------
_Static_assert(sizeof(dimg_vol_t) == 288,
               "#VOLAPI: the libc mirror of dimg_vol_t must stay 288 bytes; "
               "the kernel asserts the same width against syscall 361's argtab");

int kb_volinfo(int letter, void *out288, unsigned long size) {
    if (!out288 || size != sizeof(dimg_vol_t)) return -1;
    return sys_vol_info(letter, (dimg_vol_t *)out288);
}

long kb_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

int kb_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

int kb_isdir(const char *path) {
    // stat + S_ISDIR, NOT "opendir succeeded".
    //
    // The opendir test was wrong and the failure was loud in the right way once
    // the rest worked: MayteraOS's opendir() returns a handle for a REGULAR
    // FILE too, so every file the guest opened was classified as a directory,
    // and int21svc.c:1565 correctly refused 3Dh on it. Rogue could not open
    // ROGUE.PIC or ROGUE.OPT and gave up. "It opened, therefore it is a
    // directory" was never a sound inference; ask for the type directly.
    struct stat st;
    if (!path || stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int kb_mkdir (const char *path) { return mkdir(path, 0755); }
int kb_unlink(const char *path) { return unlink(path); }
int kb_rename(const char *a, const char *b) { return rename(a, b); }

unsigned long kb_opendir(const char *path) { return (unsigned long)(void *)opendir(path); }
void kb_closedir(unsigned long h) { if (h) closedir((DIR *)(void *)h); }

int kb_readdir(unsigned long h, char *name_out, unsigned long cap, int *is_dir) {
    if (!h || !name_out || cap == 0) return 0;
    struct dirent *e = readdir((DIR *)(void *)h);
    if (!e) return 0;
    unsigned long n = strlen(e->d_name);
    if (n >= cap) n = cap - 1;
    memcpy(name_out, e->d_name, n);
    name_out[n] = '\0';
    if (is_dir) *is_dir = (e->d_type == DT_DIR);
    return 1;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static unsigned long long ts_to_us(const struct timespec *ts) {
    return (unsigned long long)ts->tv_sec * 1000000ULL
         + (unsigned long long)ts->tv_nsec / 1000ULL;
}

// (#flipfix) The screen's own progress, from the kernel that owns the counter.
//
// A NEGATIVE return means the running kernel does not have SYS_FB_FLIP_COUNT
// (an unknown syscall number returns -1 from the dispatcher). That is reported
// as 0, and kshim.c's win16_host_flip_count() turns 0 into a value that always
// differs from the last one, so an older kernel degrades to "present every
// frame" - the behaviour before the gate existed - rather than to a 5 Hz
// picture. An instrument must never be the reason a picture stops appearing.
unsigned long long kb_fb_flips(void) {
    long v = fb_flip_count();
    return (v < 0) ? 0ULL : (unsigned long long)v;
}

unsigned long long kb_kernel_mono_us(void) { return mono_us(); }

// ===========================================================================
// THE FM BRIDGE (#fmbridge) - the libc-universe half.
//
// The op numbers live HERE and nowhere else on this side of the wall, beside
// the syscall that carries them, exactly as the PCM ctl ops do. They are
// mirrored in kernel/proc/syscall.h beside the same syscall number, which is
// where the design argument is written down.
//
// EVERY CALL IS A SYSCALL AND THAT IS FINE. Mutant Space Bats of Doom, an OPL2
// title whose audio is entirely FM music, makes 179 register writes in a 145 s
// session; at ~134 ns per syscall that is 24 microseconds. The push is also
// non-blocking on both sides of the boundary: the kernel takes an irqsave
// spinlock around a ring-buffer append and returns. There is no wait here and
// there must never be one - this runs on the guest's interpreter thread.
// ===========================================================================
int kb_fm_open(void) {
    long r = sys_dos_fm_host(DOS_FM_HOST_OPEN, 0, 0, 0);
    return (int)(r < 0 ? r : 0);
}

void kb_fm_push(unsigned reg, unsigned val, unsigned long long t_us) {
    // Return value deliberately ignored: a full queue DROPS and counts the
    // drop (rustkern/fmq.rs), and the drop count is reported at guest exit.
    // There is nothing useful a caller on the guest's port-write path could do
    // with a failure, and anything it did would be a wait on the guest's
    // interpreter thread.
    (void)sys_dos_fm_host(DOS_FM_HOST_PUSH, (long)reg, (long)val, (long)t_us);
}

int  kb_fm_close(void)    { return (int)sys_dos_fm_host(DOS_FM_HOST_CLOSE, 0, 0, 0); }
long kb_fm_pushed(void)   { return sys_dos_fm_host(DOS_FM_HOST_STAT_PUSHED, 0, 0, 0); }
long kb_fm_dropped(void)  { return sys_dos_fm_host(DOS_FM_HOST_STAT_DROPPED, 0, 0, 0); }
long kb_fm_peak(void)     { return sys_dos_fm_host(DOS_FM_HOST_STAT_PEAK, 0, 0, 0); }
long kb_fm_used(void)     { return sys_dos_fm_host(DOS_FM_HOST_STAT_USED, 0, 0, 0); }
long kb_fm_capacity(void) { return sys_dos_fm_host(DOS_FM_HOST_CAPACITY, 0, 0, 0); }
int  kb_fm_selftest(void) { return (int)sys_dos_fm_host(DOS_FM_HOST_SELFTEST, 0, 0, 0); }
int  kb_fm_launch(void)   { return (int)sys_dos_fm_host(DOS_FM_HOST_LAUNCH, 0, 0, 0); }


unsigned long long kb_mono_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts_to_us(&ts);
}
unsigned long long kb_mono_ms(void) { return kb_mono_us() / 1000ULL; }

unsigned long long kb_realtime_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return ts_to_us(&ts);
}

void kb_localtime(int *h, int *m, int *s, int *day, int *mon, int *year, int *wday) {
    time_t now = (time_t)(kb_realtime_us() / 1000000ULL);
    struct tm tmv;
    struct tm *t = localtime_r(&now, &tmv);
    if (!t) {
        // localtime_r failed: report the DOS epoch rather than a garbage date,
        // which is what a real PC with a dead RTC battery presents to a guest.
        if (h)    *h    = 0;
        if (m)    *m    = 0;
        if (s)    *s    = 0;
        if (day)  *day  = 1;
        if (mon)  *mon  = 1;
        if (year) *year = 1980;
        if (wday) *wday = 0;
        return;
    }
    if (h)    *h    = t->tm_hour;
    if (m)    *m    = t->tm_min;
    if (s)    *s    = t->tm_sec;
    if (day)  *day  = t->tm_mday;
    if (mon)  *mon  = t->tm_mon + 1;
    if (year) *year = t->tm_year + 1900;
    if (wday) *wday = t->tm_wday;
}

void kb_sleep_ms(unsigned int ms) { usleep((unsigned long)ms * 1000UL); }

// A YIELD IS NOT A ZERO-LENGTH SLEEP, AND THIS ONE COST A 36x SLOWDOWN.
//
// kb_yield() was usleep(0). libc's usleep rounds UP and then clamps:
// `ms = (us+999)/1000; if (ms == 0) ms = 1;` (userland/libc/unistd.c:180), so
// usleep(0) is SYS_SLEEP(1) - a real one-millisecond sleep, not a yield.
//
// The DOS interpreter's main loop yields ON DEMAND, many times per 20000-
// instruction slice, so every one of those became a millisecond of doing
// nothing. MEASURED on Commander Keen 5: the Ring-3 host ran at a median
// 744,932 insn/s against the in-kernel path's 27,221,087 - 36x slower - and the
// syscall profile named the culprit outright, SYS_SLEEP with 1587 calls
// totalling 170 SECONDS (a 107 ms mean, one call of 30 s).
//
// SYS_YIELD (6) is the actual primitive and libc already wraps it as yield().
// Same lesson as the memset_fast cycle earlier in this port: the target
// environment already had the right thing.
void kb_yield(void)               { yield(); }

// ---------------------------------------------------------------------------
// Threads and blocking.
//
// #426 COMPLIANCE. This is where the kernel's wait_queue primitive is
// re-implemented for Ring 3, and it is a real blocking primitive, not a poll:
// kb_cond_wait sleeps in the kernel until broadcast or deadline. The mapping
// onto the wait_event macros is exact and loses no wakes - see the protocol
// argument in kshim.c above __wait_prepare.
// ---------------------------------------------------------------------------
int kb_thread_start(void (*entry)(void *), void *arg, const char *name) {
    (void)name;
    pthread_t th;
    // pthread takes void *(*)(void *); the DOS side hands us void (*)(void *).
    // Wrap rather than cast the function pointer: calling through an
    // incompatible pointer type is undefined behaviour, and this costs nothing.
    struct shim_thunk { void (*fn)(void *); void *arg; };
    struct shim_thunk *t = (struct shim_thunk *)malloc(sizeof *t);
    if (!t) return -1;
    t->fn = entry; t->arg = arg;
    extern void *dosring3_thread_trampoline(void *);
    if (pthread_create(&th, 0, dosring3_thread_trampoline, t) != 0) { free(t); return -1; }
    return 0;
}

void *dosring3_thread_trampoline(void *p) {
    struct shim_thunk { void (*fn)(void *); void *arg; };
    struct shim_thunk *t = (struct shim_thunk *)p;
    void (*fn)(void *) = t->fn; void *a = t->arg;
    free(t);
    fn(a);
    return 0;
}

void *kb_mutex_new(void) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m);
    if (!m) return 0;
    pthread_mutex_init(m, 0);
    return m;
}
void kb_mutex_lock  (void *m) { if (m) pthread_mutex_lock  ((pthread_mutex_t *)m); }
void kb_mutex_unlock(void *m) { if (m) pthread_mutex_unlock((pthread_mutex_t *)m); }

void *kb_cond_new(void) {
    pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof *c);
    if (!c) return 0;
    pthread_cond_init(c, 0);
    return c;
}

int kb_cond_wait(void *c, void *m, unsigned long long deadline_ms) {
    if (!c || !m) return 1;
    if (deadline_ms == 0ULL) {
        pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m);
        return 0;
    }
    struct timespec ts;
    ts.tv_sec  = (long)(deadline_ms / 1000ULL);
    ts.tv_nsec = (long)((deadline_ms % 1000ULL) * 1000000ULL);
    int rc = pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, &ts);
    return rc == 0 ? 0 : 1;
}
void kb_cond_broadcast(void *c) { if (c) pthread_cond_broadcast((pthread_cond_t *)c); }

// ---------------------------------------------------------------------------
// Host window. This is the same seam DOOM uses (userland/apps/doom/i_video.c:
// 183,186): SYS_WIN_BLIT of an ARGB buffer, then win_invalidate. The DOS
// present functions rasterise into a buffer exactly as they do in the kernel;
// only the delivery of that buffer to the compositor changes.
// ---------------------------------------------------------------------------
int kb_win_create(const char *title, int x, int y, int w, int h) {
    return win_create(title, x, y, w, h);
}
void kb_win_destroy(int handle) { if (handle >= 0) win_destroy(handle); }

void kb_win_blit(int handle, int x, int y, int w, int h, const void *argb) {
    if (handle < 0 || !argb || w <= 0 || h <= 0) return;
    syscall5(SYS_WIN_BLIT, handle, x, y,
             (long)((unsigned int)w | ((unsigned int)h << 16)), (long)argb);
}
void kb_win_invalidate(int handle) { if (handle >= 0) win_invalidate(handle); }

int kb_win_content_size(int handle, int *w, int *h) {
    return win_get_size(handle, w, h);
}

// ---------------------------------------------------------------------------
// Input and window events.
//
// The pump thread owns SYS_WIN_GET_EVENT for this window and turns it into the
// three things the DOS layer reads: the raw scancode ring (guest INT 9), the
// modifier word, and the mouse state. It also latches focus, which is what
// gates the scancode tap: an unfocused DOS window must not eat the user's keys.
// ---------------------------------------------------------------------------
typedef struct {
    int type; unsigned int target_id;
    int mouse_x, mouse_y; unsigned int mouse_buttons;
    signed char scroll_delta; unsigned int keycode; char key_char;
} shim_gui_event_t;

#define SHIM_EVENT_MOUSE_MOVE   1
#define SHIM_EVENT_MOUSE_DOWN   2
#define SHIM_EVENT_MOUSE_UP     3
#define SHIM_EVENT_KEY_DOWN     5
#define SHIM_EVENT_KEY_UP       6
#define SHIM_EVENT_WINDOW_CLOSE 7
#define SHIM_EVENT_WINDOW_FOCUS 8
#define SHIM_EVENT_WINDOW_BLUR  9

#define SC_RING 256
static volatile unsigned char s_sc_ring[SC_RING];
static volatile unsigned int  s_sc_rd = 0, s_sc_wr = 0;
static pthread_mutex_t        s_sc_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int           s_focused = 1;
static volatile int           s_pump_handle = -1;
static volatile int           s_tap_on  = 0;
static volatile int           s_closed  = 0;
static volatile int           s_mx = 0, s_my = 0;
static volatile unsigned int  s_mbuttons = 0;
static volatile unsigned int  s_mods = 0;

static void sc_push(unsigned char b) {
    pthread_mutex_lock(&s_sc_lock);
    unsigned int nx = (s_sc_wr + 1) % SC_RING;
    if (nx != s_sc_rd) { s_sc_ring[s_sc_wr] = b; s_sc_wr = nx; }
    pthread_mutex_unlock(&s_sc_lock);
}

int kb_scancode_get(void) {
    int v = -1;
    pthread_mutex_lock(&s_sc_lock);
    if (s_sc_rd != s_sc_wr) { v = (int)s_sc_ring[s_sc_rd]; s_sc_rd = (s_sc_rd + 1) % SC_RING; }
    pthread_mutex_unlock(&s_sc_lock);
    return v;
}
void kb_scancode_clear(void) {
    pthread_mutex_lock(&s_sc_lock);
    s_sc_rd = s_sc_wr = 0;
    pthread_mutex_unlock(&s_sc_lock);
}
void kb_scancode_tap(int on)      { s_tap_on = on ? 1 : 0; }
unsigned int kb_key_modifiers(void) { return s_mods; }
void kb_mouse_state(int *x, int *y, unsigned int *b) {
    if (x) *x = s_mx;
    if (y) *y = s_my;
    if (b) *b = s_mbuttons;
}
int kb_win_focused(int handle) { (void)handle; return s_focused; }
int dosring3_window_closed(void) { return s_closed; }

int kb_win_work_area(int *x, int *y, int *w, int *h) {
    // The compositor owns the work area and publishes it to the kernel WM; a
    // plain app is not entitled to query it and does not need to. Report the
    // full screen, which is what the DOS layer uses this for (clamping its
    // initial window size), and let the window manager's own placement rules
    // apply as they do for every other app.
    // No syscall exposes the screen extent to a plain app (SYS_FB_MAP and the
    // compositor queries are all is_compositor()-gated, deliberately). The DOS
    // layer uses this only to clamp its INITIAL window size, and the window
    // manager applies its own placement and clamping to every window anyway, so
    // a conservative default is correct rather than merely convenient: asking
    // for a larger window than the screen is already handled one layer down.
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 1024;
    if (h) *h = 768;
    return 0;
}

static void pump_thread(void *arg) {
    (void)arg;
    for (;;) {
        int handle = s_pump_handle;   // follows window recreation
        if (handle < 0) { usleep(50000); continue; }
        shim_gui_event_t ev;
        memset(&ev, 0, sizeof ev);
        // Block in the kernel for up to 50 ms. This is a real blocking wait
        // (SYS_WIN_GET_EVENT sleeps); the bound exists so window CLOSE is
        // still noticed if the compositor never sends another event, which is
        // the wake source we do not own. #426 preference order, case 2.
        int rc = win_get_event(handle, &ev, 50);

        // (#fmzombie) A DESTROYED WINDOW TURNS THIS THREAD INTO A BUSY-WAIT,
        // AND IT IS THE ONE THAT BURNS A CORE AFTER THE GUEST IS GONE.
        //
        // MEASURED, golden 2320 + this build: after the guest ended and the
        // host's main() returned, this thread reported
        // `[DOSRING3-PUMP] pass=1558601 ... last=-1` and the heartbeat read
        // `top=DOSUSER:99,heartbeat:0` for the rest of the run. Before the
        // guest ended the same counter advanced at about 20 passes per second,
        // which is the 50 ms block below doing its job.
        //
        // WHY. sys_win_get_event() (proc/syscall.c:8096) returns -1
        // IMMEDIATELY when `!user_windows[handle].window`, i.e. once the window
        // has been destroyed. The loop's only pacing is that call's 50 ms
        // block, and `if (rc <= 0) continue;` treated an instant -1 exactly
        // like a 50 ms timeout - so the pacing vanished and the loop ran at the
        // speed of the CPU. Identical in shape to the zero-length PCM write
        // that made /APPS/FMSYNTH spin: a call that normally blocks starts
        // returning immediately, and a caller that does not distinguish
        // "nothing happened" from "there is nothing left" has no pacing at all.
        //
        // A THREAD OUTLIVING ITS PROCESS IS NORMAL HERE, WHICH IS WHY THIS
        // MATTERS. sys_kill() raises a signal on ONE process_t and there is no
        // exit_group() anywhere in proc/, so when main() returns only the
        // LEADER runs proc_exit(). This pthread keeps running with the whole
        // address space alive behind it.
        //
        // So: a window that is merely BETWEEN incarnations (the DOS layer
        // destroys and recreates its window on a mode change, which is what
        // s_pump_handle "follows window recreation" above is for) gets the same
        // 50 ms cadence the no-handle branch already uses - not a new poll, the
        // existing one applied to the second way of having no window. And a
        // window that is gone AFTER a close is final: there is nothing left to
        // pump and the thread returns, rather than idling for the life of a
        // process that has already exited.
        if (rc < 0) {
            if (s_closed) return;
            usleep(50000);
            continue;
        }

        // DRAIN THE RAW SCANCODE RING EVERY PASS, NOT ONLY ON A KEY EVENT.
        //
        // This was inside the KEY_DOWN/KEY_UP case and it could not work, for a
        // reason that is obvious once seen and invisible before: the SAME call
        // both ARMS the subscription and drains it. Arming only on a key event
        // means nothing is ever armed until a key event arrives, and no raw
        // byte is ever captured until something is armed. The guest sat on
        // "Press any key to continue" through every keystroke.
        //
        // Draining unconditionally also removes the dependency on the
        // compositor routing cooked key events to this window at all, which the
        // raw path should never have needed. Cost is one syscall per 50 ms idle
        // pass; it returns 0 immediately when the ring is empty or the window
        // is not focused.
        {
            unsigned char raw[64];
            int n = win_get_scancodes(handle, raw, (int)sizeof raw);
            for (int i = 0; i < n; i++) sc_push(raw[i]);
            // PUMP LIVENESS. The kernel-side census can say bytes were pushed
            // and not drained; only this can say whether the drainer is even
            // running. Printed every 40 passes (~2 s) so a stalled pump is
            // visible as a line that stops, not as an absence to be inferred.
            static unsigned long pass = 0, got = 0;
            if (n > 0) got += (unsigned long)n;
            if ((pass++ % 40) == 0)
                kb_pump_note((long)pass, (long)got, (long)n);
        }

        if (rc <= 0) continue;
        switch (ev.type) {
            case SHIM_EVENT_WINDOW_FOCUS: s_focused = 1; break;
            case SHIM_EVENT_WINDOW_BLUR:  s_focused = 0; kb_scancode_clear(); break;
            case SHIM_EVENT_WINDOW_CLOSE:
                // (#fmzombie) THE CLOSE BUTTON USED TO SET A FLAG NOBODY READ.
                //
                // s_closed was written here and its only reader,
                // dosring3_window_closed(), had ZERO callers anywhere in the
                // tree. So closing a Ring-3 DOS guest's window did nothing at
                // all: the kernel window manager delivers EVENT_WINDOW_CLOSE
                // and then, finding no on_close handler, HIDES the window
                // (kernel/gui/window.c:1663-1680). The guest carried on
                // executing headless until DOS_MAX_RUN_MS - six hours - with
                // its window gone.
                //
                // That is the whole of the reported defect. dos_run_file()
                // never returned, so its wrapper never reached
                // dos_fmq_host_close(); this process never exited, so
                // dos_fmq_host_release_pid() never fired from proc_exit(). The
                // FM queue therefore stayed `active` for ever,
                // SYS_DOS_FM_EVENTS never answered DOS_FM_ENODEV, and
                // /APPS/FMSYNTH - whose ONLY exit condition is that ENODEV -
                // could never learn the guest had gone. "fmsynth wasn't
                // killed" was never a synthesiser bug: nothing had told it to
                // stop, because nothing had told the GUEST to stop.
                //
                // THE FIX IS THE EXISTING PRIMITIVE, NOT A NEW ONE.
                // dos_request_close() (dos/dosexec.c) is what the in-kernel
                // path's titlebar X already calls, via dos_host_close_handler()
                // in proc/syscall.c. dosexec.c is compiled into this binary
                // byte-identically, so the same function is right here; the
                // in-kernel path was simply the only one wired to it. It clears
                // t->running, which the interpreter's run loops test at the top
                // of every burst ("window closed"), so the guest stops within
                // one slice and then runs its OWN normal teardown - which is
                // the teardown that closes the FM queue and prints the session
                // summary. Writing a second close path here would have been the
                // fork CLAUDE.md forbids, and it would have given the two DOS
                // paths two different ideas of what closing a window means.
                //
                // Same thread contract as Ring 0: this runs on the window-event
                // pump, not on the interpreter, exactly as the kernel version
                // runs on the WM thread and not on the interpreter. It sets a
                // flag and wakes; it touches neither the interpreter nor the
                // window.
                s_closed = 1;
                {
                    extern void dos_request_close(void);
                    static const char m[] =
                        "[DOSRING3] window closed: asking the guest to stop "
                        "(dos_request_close)\n";
                    (void)write(1, m, sizeof m - 1);
                    dos_request_close();
                }
                break;
            case SHIM_EVENT_MOUSE_MOVE:
            case SHIM_EVENT_MOUSE_DOWN:
            case SHIM_EVENT_MOUSE_UP:
                s_mx = ev.mouse_x; s_my = ev.mouse_y; s_mbuttons = ev.mouse_buttons;
                break;
            case SHIM_EVENT_KEY_DOWN:
            case SHIM_EVENT_KEY_UP:
                // The RAW bytes are drained above, every pass. Nothing to do
                // here: the cooked event carries no scancode (gui_event_t has
                // no such field, which is why SYS_WIN_GET_SCANCODES exists).
                break;
            default: break;
        }
    }
}

void kb_pump_note(long pass, long got, long last) {
    char b[128];
    int n = snprintf(b, sizeof b,
                     "[DOSRING3-PUMP] pass=%ld drained_total=%ld last=%ld\n",
                     pass, got, last);
    if (n > 0) (void)write(1, b, (size_t)n);
}

void kb_pump_start(int win_handle) {
    // ONE PUMP, EVER. The DOS layer creates its host window, measures the
    // content rectangle, destroys it and recreates it at a corrected size, so
    // win16_host_create() runs TWICE per guest and this was called twice,
    // leaving two polling threads alive for the life of the process. They then
    // fought over the kernel's raw-scancode subscription and destroyed every
    // byte between them (see the kernel-side note in sys_win_get_scancodes).
    //
    // The handle is updated so the surviving pump follows the window that
    // actually exists; only the THREAD is not duplicated.
    static int started = 0;
    s_pump_handle = win_handle;
    if (started) return;
    started = 1;
    (void)kb_thread_start(pump_thread, 0, "dospump");
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
unsigned int kb_uid(void) { return (unsigned int)getuid(); }
unsigned int kb_gid(void) { return (unsigned int)getgid(); }

// THE home lookup, reused rather than reimplemented. libc/userconf.c's
// userhome_root() reads the session user's home out of /CONFIG/PASSWD via
// getpwuid(getuid()) and is described in its own comment as "THE home lookup,
// and the only one"; every other userland consumer of a per-user path already
// goes through it. The DOS layer needs the same answer to expand "%HOME%" in a
// guest path (dos/int21svc.c dos_svc_resolve()), so it gets the same function.
//
// A second implementation here would be the exact shape of the bug this port
// has already been bitten by twice: supplying something that already exists.
int kb_home(char *out, unsigned long cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    return userhome_root(out, cap);
}

// ---------------------------------------------------------------------------
// AUDIO (#181 Ring-3 audio) - the PCM sink, and the capture tap that proves it.
//
// These are thin: every semantic (the blocking write, the consume counter, the
// two #426 waits) lives in kernel/drivers/audio_pcm.c and is shared with the
// in-kernel DOS path. Nothing here re-implements pacing, and there is no sleep
// and no poll anywhere in this section.
// ---------------------------------------------------------------------------
long kb_pcm_open(unsigned rate, unsigned channels, unsigned format) {
    return (long)sys_audio_pcm_open(rate, channels, format);
}
long kb_pcm_write(long handle, const void *pcm, unsigned frames) {
    return (long)sys_audio_pcm_write((int)handle, pcm, frames);
}
long kb_pcm_close(long handle) {
    return (long)sys_audio_pcm_close((int)handle);
}
long kb_pcm_consumed(long handle) {
    return sys_audio_pcm_ctl((int)handle, AUDIO_PCM_CTL_CONSUMED, 0, 0);
}
long kb_pcm_wait_below(long handle, unsigned max_used, unsigned ms) {
    return sys_audio_pcm_ctl((int)handle, AUDIO_PCM_CTL_WAIT_BELOW, max_used, ms);
}
long kb_pcm_wait_consumed(long handle, unsigned target, unsigned ms) {
    return sys_audio_pcm_ctl((int)handle, AUDIO_PCM_CTL_WAIT_CONSUMED, target, ms);
}

// A ONE SECOND CACHE, and why it is a cache and not a latch.
//
// sb_installed_policy() asks this on emulated-port reads, and a guest probing
// for a Sound Blaster polls those hard, so a syscall per read would be a real
// slowdown of the interpreter. A permanent latch would be wrong in the other
// direction: a USB DAC can be plugged in after boot and the kernel path would
// notice. One second is short enough that a hotplug is picked up within a
// sound effect and long enough that the polling cost disappears.
//
// This is NOT a wait: nothing here blocks, sleeps or spins. It answers from a
// cached value or makes one syscall and returns.
int kb_pcm_avail(void) {
    static int cached = -1;
    static unsigned long long at = 0;
    unsigned long long now = kb_mono_ms();
    if (cached >= 0 && now - at < 1000ULL) return cached;
    long r = sys_audio_pcm_ctl(0, AUDIO_PCM_CTL_AVAIL, 0, 0);
    cached = (r == 1) ? 1 : 0;
    at = now;
    return cached;
}

// THE CAPTURE TAP, and why it exists.
//
// A previous run shipped a complete DOS audio stack whose honest summary was
// that nobody had ever HEARD any audio. A log line saying frames were written
// is not evidence that the samples were the guest's samples, in the right
// order, at the right rate. This writes every frame handed to the sink into a
// raw file, so the claim can be settled by looking at the waveform.
//
// Raw S16-LE mono at the sink rate: no header, so it concatenates cleanly
// across blocks and can be read by anything. OFF unless /CONFIG/DOSPCMCAP.CFG
// exists with a first byte other than 0, and it is not shipped in the golden.
// Hard byte cap so a long session cannot fill the disk.
#define PCMTAP_CAP_BYTES (16u * 1024u * 1024u)
long kb_pcm_tap(const void *pcm, unsigned frames) {
    static int state = -1;          // -1 unknown, 0 off, 1 on
    static int fd = -1;
    static unsigned written = 0;
    if (state == 0) return -1;
    if (state < 0) {
        state = 0;
        int c = open("/CONFIG/DOSPCMCAP.CFG", O_RDONLY);
        if (c >= 0) {
            char b = 0;
            long n = read(c, &b, 1);
            close(c);
            if (n == 1 && b != '0') {
                fd = open("/DOSPCM.RAW", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) state = 1;
            }
        }
        if (state == 0) return -1;
    }
    if (fd < 0) return -1;
    unsigned bytes = frames * 2u;
    if (written >= PCMTAP_CAP_BYTES) return 0;
    if (written + bytes > PCMTAP_CAP_BYTES) bytes = PCMTAP_CAP_BYTES - written;
    long w = write(fd, pcm, bytes);
    if (w > 0) written += (unsigned)w;
    return w;
}
