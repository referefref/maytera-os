// syscall.h - System call numbers and prototypes for MayteraOS user space
#ifndef _SYSCALL_H
#define _SYSCALL_H

// System call numbers (must match kernel/proc/syscall.h)

// Process management
#define SYS_EXIT            0   // Exit process
#define SYS_FORK            1   // Fork process
#define SYS_EXEC            2   // Execute program
#define SYS_WAIT            3   // Wait for child
#define SYS_GETPID          4   // Get process ID
#define SYS_GETPPID         5   // Get parent process ID
#define SYS_YIELD           6   // Yield CPU
#define SYS_SLEEP           7   // Sleep for milliseconds

// File I/O
#define SYS_OPEN            10  // Open file
#define SYS_CLOSE           11  // Close file
#define SYS_READ            12  // Read from file
#define SYS_WRITE           13  // Write to file
#define SYS_SEEK            14  // Seek in file
#define SYS_STAT            15  // Get file status
#define SYS_MKDIR           16  // Create directory
#define SYS_RMDIR           17  // Remove directory
#define SYS_UNLINK          18  // Delete file
#define SYS_READDIR         19  // Read directory entry

// Memory management
#define SYS_BRK             20  // Change data segment size
#define SYS_MMAP            21  // Map memory
#define SYS_MUNMAP          22  // Unmap memory

// Graphics/Window (for GUI apps)
#define SYS_WIN_CREATE      30  // Create window
#define SYS_WIN_DESTROY     31  // Destroy window
#define SYS_WIN_DRAW_RECT   32  // Draw rectangle
#define SYS_WIN_DRAW_TEXT   33  // Draw text
#define SYS_WIN_DRAW_PIXEL  34  // Draw pixel
#define SYS_WIN_BLIT        35  // Blit bitmap
#define SYS_WIN_GET_EVENT   36  // Get window event
#define SYS_WIN_INVALIDATE  37  // Invalidate window
#define SYS_WIN_GET_SIZE    38  // Get window content size

// Filesystem manipulation
#define SYS_RENAME          70  // Rename file/directory

// Console I/O
#define SYS_PUTCHAR         40  // Write character
#define SYS_GETCHAR         41  // Read character

// Time
#define SYS_TIME            50  // Get current time
#define SYS_CLOCK           51  // Get system clock ticks

// Network
#define SYS_SOCKET          60  // Create TCP socket
#define SYS_CONNECT         61  // Connect socket (sock, ip, port)
#define SYS_SEND            62  // Send data (sock, buf, len)
#define SYS_RECV            63  // Receive data (sock, buf, len)
#define SYS_TCP_CLOSE       64  // Close TCP socket (sock)
#define SYS_TCP_STATE       65  // Get TCP socket state (sock)
#define SYS_PING            66  // ICMP echo (dest_ip, timeout_ms) -> rtt ms or -1
// #443: listen()/accept() so a userland process can be a normal TCP server
// (previously only in-kernel sshd could; userland could only reverse-connect).
#define SYS_LISTEN          303 // (sock, port, backlog) -> 0/-1: bind+listen
#define SYS_ACCEPT          304 // (sock) -> new socket fd, or -9 (would-block) / -1

// Signals
#define SYS_KILL            80  // Send signal to process
#define SYS_SIGACTION       81  // Set signal handler
#define SYS_SIGPROCMASK     82  // Block/unblock signals
#define SYS_SIGRETURN       83  // Return from signal handler
#define SYS_ALARM           84  // Set alarm timer
#define SYS_PAUSE           85  // Wait for signal

// Network: high-level HTTP fetch
#define SYS_HTTP_FETCH      86  // Fetch HTTP/HTTPS URL into user buffer

// Process control (extended)
#define SYS_DUP             90  // Duplicate file descriptor
#define SYS_DUP2            91  // Duplicate fd to specific number
#define SYS_PIPE            92  // Create pipe

#define SYS_FCNTL           93  // File control
#define SYS_IOCTL           94  // Device control
#define SYS_SETSID          95  // Create new session
#define SYS_SETPGID         96  // Set process group
#define SYS_GETPGID         97  // Get process group
#define SYS_GETSID          106 // Get session ID
#define SYS_WAITPID         98  // Wait for specific child
#define SYS_GETCWD          99  // Get current working directory
#define SYS_CHDIR           100 // Change directory
#define SYS_FSTAT           101 // Get file status by fd
#define SYS_GETDENTS        102 // Get directory entries
#define SYS_EXECVE          103 // Execute with argv and envp
#define SYS_POLL            104 // Poll file descriptors
#define SYS_NANOSLEEP       105 // Sleep with nanosecond precision

// User identity
#define SYS_GETUID          120 // Get real user ID
#define SYS_SETUID          121 // Set user ID
#define SYS_GETGID          122 // Get real group ID
#define SYS_SETGID          123 // Set group ID
#define SYS_GETEUID         124 // Get effective user ID
#define SYS_GETEGID         125 // Get effective group ID
#define SYS_SETEUID         126 // Set effective user ID
#define SYS_SETEGID         127 // Set effective group ID
#define SYS_CHMOD           128 // Change file permissions
#define SYS_CHOWN           129 // Change file ownership
#define SYS_PASSWD_CHANGE   130 // Change user password
#define SYS_SU              131 // Switch user (verify + setuid)
#define SYS_ADDUSER         132 // Create new user (root only)
#define SYS_SET_THEME       133 // Set system-wide UI theme
#define SYS_GET_THEME       134 // Get current system theme ID
#define SYS_THEME_COLOR     290 // (#285) Get active theme color by theme_color_id_t
#define SYS_SET_VOLUME      135 // Set master audio volume (0-100)
#define SYS_GET_VOLUME      136 // Get master audio volume (0-100)
#define SYS_SET_MUTE        137 // Set audio mute state (0=unmute, 1=mute)
#define SYS_GET_DISK_TOTAL  138 // Get disk total size in MB
#define SYS_GET_DISK_FREE   139 // Get disk free space in MB
#define SYS_SET_MOUSE_SPEED 140 // Set mouse sensitivity (1-10)
#define SYS_GET_MOUSE_SPEED 141 // Get mouse sensitivity (1-10)
#define SYS_GET_RTC_TIME    142 // Get RTC time packed: (hour<<16)|(min<<8)|sec
#define SYS_GET_RTC_DATE    143 // Get RTC date packed: (year<<16)|(month<<8)|day
#define SYS_SET_RTC_TIME    144 // Set RTC time: packed (hour<<16)|(min<<8)|sec
#define SYS_SET_RTC_DATE    145 // Set RTC date: packed (year<<16)|(month<<8)|day
#define SYS_GET_NET_INFO    146 // Get network info into net_info_t buffer
#define SYS_NTP_SYNC        147 // Sync time from NTP server; returns 0=ok -1=fail
#define SYS_SET_CURSOR_THEME 148 // Set cursor theme: 0=Retro, 1=Light, 2=Dark
#define SYS_GET_CURSOR_THEME 149 // Get current cursor theme
#define SYS_WIN_GET_POS     150 // Get window screen position

// Threading and Futex (Task #25)
#define SYS_CLONE           110 // Clone process/thread
#define SYS_GETTID          111 // Get thread ID
#define SYS_SET_TID_ADDRESS 112 // Set clear_child_tid address
#define SYS_FUTEX           113 // Fast userspace mutex
#define SYS_TKILL           114 // Send signal to thread
#define SYS_TGKILL          115 // Send signal to thread group

// Clone flags
#define CLONE_VM        0x00000100  // Share virtual memory
#define CLONE_FS        0x00000200  // Share filesystem info
#define CLONE_FILES     0x00000400  // Share file descriptor table
#define CLONE_SIGHAND   0x00000800  // Share signal handlers
#define CLONE_THREAD    0x00010000  // Same thread group
#define CLONE_SETTLS    0x00080000  // Set TLS
#define CLONE_PARENT_SETTID  0x00100000  // Store TID in parent
#define CLONE_CHILD_CLEARTID 0x00200000  // Clear TID on exit
#define CLONE_CHILD_SETTID   0x01000000  // Store TID in child

// Futex operations
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128

// Raw syscall interface (implemented in syscall.asm)
// Uses x86-64 syscall convention:
//   rax = syscall number
//   rdi = arg1, rsi = arg2, rdx = arg3, r10 = arg4, r8 = arg5, r9 = arg6
//   return value in rax

extern long syscall0(long num);
extern long syscall1(long num, long arg1);
extern long syscall2(long num, long arg1, long arg2);
extern long syscall3(long num, long arg1, long arg2, long arg3);
extern long syscall4(long num, long arg1, long arg2, long arg3, long arg4);
extern long syscall5(long num, long arg1, long arg2, long arg3, long arg4, long arg5);
extern long syscall6(long num, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6);

// Low-level sys_* wrappers used internally by the libc (stdlib.c, unistd.c).
// These are always available regardless of which headers are included.

static inline void sys_exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

static inline void *sys_mmap(void *addr, unsigned long len, int prot, int flags) {
    return (void *)syscall4(SYS_MMAP, (long)addr, len, prot, flags);
}

static inline int sys_munmap(void *addr, unsigned long len) {
    return (int)syscall2(SYS_MUNMAP, (long)addr, len);
}

static inline void *sys_brk(void *addr) {
    return (void *)syscall1(SYS_BRK, (long)addr);
}

static inline int sys_open(const char *path, int flags) {
    return (int)syscall2(SYS_OPEN, (long)path, flags);
}

static inline int sys_close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

static inline long sys_read(int fd, void *buf, unsigned long count) {
    return syscall3(SYS_READ, fd, (long)buf, count);
}

static inline long sys_write(int fd, const void *buf, unsigned long count) {
    return syscall3(SYS_WRITE, fd, (long)buf, count);
}

static inline long sys_seek(int fd, long offset, int whence) {
    return syscall3(SYS_SEEK, fd, offset, whence);
}

static inline long sys_clock(void) {
    return syscall0(SYS_CLOCK);
}

static inline long sys_time(void) {
    return syscall0(SYS_TIME);
}

#define SYS_GET_TICKS 245
// Monotonic 100Hz tick counter (each tick = 10ms). For double-click timing etc.
static inline long get_ticks(void) {
    return syscall0(SYS_GET_TICKS);
}

#define SYS_GET_VERSION 246
// Copy the OS version string "X.Y.Z (build N)" into buf; returns length.
static inline int get_version(char *buf, int len) {
    return (int)syscall2(SYS_GET_VERSION, (long)buf, (long)len);
}

static inline void yield(void) {
    syscall0(SYS_YIELD);
}

static inline int sys_readdir_raw(int fd, void *entry_buf) {
    return (int)syscall2(SYS_READDIR, fd, (long)entry_buf);
}

// ============================================================================
// Window/GUI syscalls
// ============================================================================

// Create a window
// Returns: window handle (>=0) on success, -1 on failure
static inline int win_create(const char *title, int x, int y, int width, int height) {
    return (int)syscall5(SYS_WIN_CREATE, (long)title, x, y, width, height);
}

// Destroy a window
static inline int win_destroy(int handle) {
    return (int)syscall1(SYS_WIN_DESTROY, handle);
}

// Draw a filled rectangle in window with specified color
static inline int win_draw_rect(int handle, int x, int y, int w, int h, unsigned int color) {
    return (int)syscall6(SYS_WIN_DRAW_RECT, handle, x, y, w, h, color);
}

// Draw a single pixel in window
static inline int win_draw_pixel(int handle, int x, int y, unsigned int color) {
    return (int)syscall4(SYS_WIN_DRAW_PIXEL, handle, x, y, color);
}

// Draw text in window
static inline int win_draw_text(int handle, int x, int y, const char *text, unsigned int color) {
    return (int)syscall5(SYS_WIN_DRAW_TEXT, handle, x, y, (long)text, color);
}
#define SYS_WIN_DRAW_TEXT_SMALL 232
#define SYS_WIN_DRAW_TTF 235  // antialiased TTF text into a window (size packed in top byte of color)
// 4x8 tooltip-size text into a window (5px advance per glyph). For hints/captions.
static inline int win_draw_text_small(int handle, int x, int y, const char *text, unsigned int color) {
    return (int)syscall5(SYS_WIN_DRAW_TEXT_SMALL, handle, x, y, (long)text, color);
}

// Antialiased TrueType text into a window (window-relative, clipped). Apps opt
// into TTF with this; size is packed into the top byte of the color argument.
static inline int win_draw_text_ttf(int handle, int x, int y, const char *text, int size, unsigned int color) {
    return (int)syscall5(SYS_WIN_DRAW_TTF, handle, x, y, (long)text,
                         (color & 0xFFFFFF) | (((unsigned)size & 0xFF) << 24));
}

// ---- OS-wide font registry (multi-face TrueType) ----
#define SYS_FONT_COUNT      307
#define SYS_FONT_NAME       308
#define SYS_FONT_GLYPH      309
#define SYS_FONT_METRICS    310
#define SYS_WIN_DRAW_TTF_EX 311
#define SYS_FONT_KERN       312
#define SYS_FONT_STYLE      324
#define SYS_FONT_RESCAN     325
#define SYS_FONT_REMOVE     326
#define SYS_FONT_SET_UI     327
#define SYS_FONT_GET_UI     328
#define SYS_FONT_FIND       329
// #542 OS-wide system clipboard (kernel-held, cross-app). See kernel proc/syscall.h.
#define SYS_CLIP_SET        330
#define SYS_CLIP_GET        331
#define SYS_CLIP_LEN        332

#define FONT_STYLE_NORMAL 0
#define FONT_STYLE_BOLD   1
#define FONT_STYLE_ITALIC 2

typedef struct { int width, height, xoff, yoff, advance; } font_glyph_meta_t;

// Number of installed faces (>=1; face 0 = default UI font).
static inline int font_count(void) { return (int)syscall0(SYS_FONT_COUNT); }
// Copy face idx's family name into buf (returns length).
static inline int font_name(int idx, char *buf, int cap) {
    return (int)syscall3(SYS_FONT_NAME, (long)idx, (long)buf, (long)cap);
}
// Rasterize a codepoint to an 8-bit alpha bitmap (row-major, width*height bytes)
// plus metrics. Returns the advance (>=0) or -1. Pass a bitmap buffer >= max
// glyph area (e.g. size*size*4) and its capacity.
static inline int font_glyph(int face, int size, int style, int cp,
                             font_glyph_meta_t *meta, unsigned char *bmp, int cap) {
    long packed = (face & 0xFF) | (((long)size & 0xFFFF) << 8) | (((long)style & 0xFF) << 24);
    return (int)syscall5(SYS_FONT_GLYPH, packed, cp, (long)meta, (long)bmp, cap);
}
// Vertical metrics for a face/size: out3 = {ascent, descent, line_gap}.
static inline int font_metrics(int face, int size, int out3[3]) {
    long packed = (face & 0xFF) | (((long)size & 0xFFFF) << 8);
    return (int)syscall2(SYS_FONT_METRICS, packed, (long)out3);
}
// Kerning adjustment (px) between two codepoints for a face/size.
static inline int font_kern(int face, int size, int cp1, int cp2) {
    long packed = (face & 0xFF) | (((long)size & 0xFFFF) << 8);
    return (int)syscall3(SYS_FONT_KERN, packed, cp1, cp2);
}
// Copy face idx's SUBFAMILY ("Regular"/"Bold"/"Semibold Italic") into buf.
// Returns 0 for an empty or uninstalled slot; face indices are stable and may
// contain holes, so enumerate 0..font_count()-1 and skip zero-length names.
static inline int font_style(int idx, char *buf, int cap) {
    return (int)syscall3(SYS_FONT_STYLE, (long)idx, (long)buf, (long)cap);
}
// Re-walk /FONTS so a font copied in at runtime is usable without a reboot.
// Returns the number of faces added.
static inline int font_rescan(void) { return (int)syscall0(SYS_FONT_RESCAN); }
// Uninstall a face (hide it from enumeration). Face 0 cannot be removed.
static inline int font_remove(int idx) { return (int)syscall1(SYS_FONT_REMOVE, (long)idx); }
// System UI font. Every legacy (non-_f) text path draws with the active face, so
// setting this restyles the running desktop live, OS-wide. Returns the previous
// face. Persist the choice separately (gui_font_set_system() does both).
static inline int font_set_ui(int face) { return (int)syscall1(SYS_FONT_SET_UI, (long)face); }
static inline int font_get_ui(void) { return (int)syscall0(SYS_FONT_GET_UI); }
// Face registered from `path`, or -1. An installer needs this because rescan is
// additive: re-installing a known font moves no counter, so the count cannot
// identify the face.
static inline int font_find(const char *path) {
    return (int)syscall1(SYS_FONT_FIND, (long)path);
}

// ---------------------------------------------------------------------------
// #542 OS-wide system clipboard. The store is a single bounded (64 KiB) buffer
// held in the kernel, so copy in ANY app / paste in ANY other app just works.
// clipboard_set replaces the whole clipboard with len bytes from buf (a
// NULL/zero clears it) and returns the bytes stored. clipboard_get copies up to
// cap bytes into buf and returns the TOTAL bytes held, so a return > cap
// means the caller saw a truncated view and should retry with a larger buffer.
// These carry raw bytes, not a C string: they do NOT append a NUL. For text,
// pass strlen(s) to set, and NUL-terminate yourself after get (see the _text
// helpers below). clipboard_len queries the held size without copying.
// ---------------------------------------------------------------------------
static inline int clipboard_set(const void *buf, int len) {
    if (len < 0) len = 0;
    return (int)syscall2(SYS_CLIP_SET, (long)buf, (long)len);
}
static inline int clipboard_get(void *buf, int cap) {
    if (cap < 0) cap = 0;
    return (int)syscall2(SYS_CLIP_GET, (long)buf, (long)cap);
}
static inline int clipboard_len(void) {
    return (int)syscall0(SYS_CLIP_LEN);
}
// Text convenience: copy a NUL-terminated string (its bytes, without the NUL).
static inline int clipboard_set_text(const char *s) {
    int n = 0; if (s) { while (s[n]) n++; }
    return clipboard_set(s, n);
}
// Text convenience: fetch into a caller buffer and NUL-terminate. cap is the
// full buffer size including the terminator. Returns the number of bytes placed
// before the NUL (<= cap-1).
static inline int clipboard_get_text(char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    int held = clipboard_get(buf, cap - 1);
    int n = held; if (n > cap - 1) n = cap - 1; if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}
// Face-aware antialiased TTF into a window (face + style + real point size).
static inline int win_draw_text_ttf_ex(int handle, int x, int y, const char *text,
                                       int face, int size, int style, unsigned int color) {
    long xy  = ((long)(x & 0xFFFF)) | (((long)(y & 0xFFFF)) << 16);
    long fss = (face & 0xFF) | (((long)size & 0xFFFF) << 8) | (((long)style & 0xFF) << 24);
    return (int)syscall5(SYS_WIN_DRAW_TTF_EX, handle, xy, (long)text, fss, (long)(color & 0xFFFFFF));
}
#define SYS_SET_WIN_OPACITY 233
// Global default window opacity 0-255 (255=opaque), applied to all windows.
static inline int set_win_opacity(int opacity) {
    return (int)syscall1(SYS_SET_WIN_OPACITY, opacity);
}
#define SYS_GET_WIN_OPACITY 236
// Read the global default window opacity (0-255).
static inline int get_win_opacity(void) {
    return (int)syscall0(SYS_GET_WIN_OPACITY);
}

// Get window event (returns event type, fills event_buf)
// timeout: 0 = non-blocking, >0 = wait up to timeout ms, -1 = wait forever
static inline int win_get_event(int handle, void *event_buf, int timeout) {
    return (int)syscall3(SYS_WIN_GET_EVENT, handle, (long)event_buf, timeout);
}

// Invalidate window (request redraw)
static inline int win_invalidate(int handle) {
    return (int)syscall1(SYS_WIN_INVALIDATE, handle);
}

// Get window content dimensions (for resize handling)
static inline int win_get_size(int handle, int *width, int *height) {
    return (int)syscall3(SYS_WIN_GET_SIZE, handle, (long)width, (long)height);
}

// Get current window screen position (must call per-event as window may have moved)
static inline void win_get_pos(int handle, int *x, int *y) {
    syscall3(SYS_WIN_GET_POS, (long)handle, (long)x, (long)y);
}

// #334: move a window (by handle) to an absolute screen position, or by a delta.
// Lets borderless apps (e.g. the Maytera HiFi) implement WinAmp-style dragging.
#define SYS_WIN_MOVE     280
#define SYS_WIN_MOVE_BY  281
static inline int win_move(int handle, int x, int y) {
    return (int)syscall3(SYS_WIN_MOVE, (long)handle, (long)x, (long)y);
}
static inline int win_move_by(int handle, int dx, int dy) {
    return (int)syscall3(SYS_WIN_MOVE_BY, (long)handle, (long)dx, (long)dy);
}

// ============================================================================
// Process/system wrappers for apps using raw sys_* interface
// ============================================================================

static inline int sys_getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

static inline int sys_fork(void) {
    return (int)syscall0(SYS_FORK);
}

static inline int sys_sleep(unsigned int ms) {
    return (int)syscall1(SYS_SLEEP, ms);
}

static inline int sys_exec(const char *path) {
    return (int)syscall1(SYS_EXEC, (long)path);
}

static inline int sys_wait(int *status) {
    return (int)syscall1(SYS_WAIT, (long)status);
}

static inline int sys_getppid(void) {
    return (int)syscall0(SYS_GETPPID);
}

static inline int sys_putchar(int c) {
    return (int)syscall1(SYS_PUTCHAR, c);
}

static inline int sys_getchar(void) {
    return (int)syscall0(SYS_GETCHAR);
}

static inline int sys_stat(const char *path, void *buf) {
    return (int)syscall2(SYS_STAT, (long)path, (long)buf);
}

static inline int sys_mkdir(const char *path, int mode) {
    return (int)syscall2(SYS_MKDIR, (long)path, mode);
}

static inline int sys_rmdir(const char *path) {
    return (int)syscall1(SYS_RMDIR, (long)path);
}

static inline int sys_unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}

static inline int sys_rename(const char *old, const char *new_path) {
    return (int)syscall2(SYS_RENAME, (long)old, (long)new_path);
}

// ============================================================================
// Directory entry type for raw readdir
// ============================================================================

typedef struct {
    char name[256];
    unsigned int type;      // 0 = file, 1 = directory (matches kernel layout)
    unsigned int size;      // file size in bytes
} dirent_t;

#define DIRENT_IS_DIR(d) ((d).type == 1)

// Read directory entry by path and index (compatibility wrapper)
// Opens directory, reads entries sequentially up to index, closes fd
static inline int sys_readdir(const char *path, int index, dirent_t *entry) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    for (int i = 0; i <= index; i++) {
        int r = (int)syscall2(SYS_READDIR, fd, (long)entry);
        /* kernel sys_readdir: 0 = entry filled, >0 = end-of-dir, <0 = error */
        if (r != 0) { sys_close(fd); return -1; }
    }
    sys_close(fd);
    return 0;
}

// ============================================================================
// Threading syscalls (Task #25)
// ============================================================================

// Get current thread ID
static inline int gettid(void) {
    return (int)syscall0(SYS_GETTID);
}

// Set clear_child_tid address for futex wake on thread exit
static inline int set_tid_address(unsigned int *tidptr) {
    return (int)syscall1(SYS_SET_TID_ADDRESS, (long)tidptr);
}

// Clone process/thread with specified flags
// flags: CLONE_* flags
// stack: child stack pointer (or NULL)
// parent_tid: address to store child TID in parent
// child_tid: address for CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID
// tls: thread-local storage descriptor
static inline int clone(unsigned int flags, void *stack, unsigned int *parent_tid,
                        unsigned int *child_tid, void *tls) {
    return (int)syscall5(SYS_CLONE, flags, (long)stack, (long)parent_tid,
                         (long)child_tid, (long)tls);
}

// ============================================================================
// Futex syscalls (Task #25)
// ============================================================================

// Futex wait: sleep if *addr == val
// Returns 0 on wake, -EAGAIN if *addr != val, -ETIMEDOUT on timeout
static inline int futex_wait(volatile unsigned int *addr, unsigned int val, unsigned long timeout) {
    return (int)syscall4(SYS_FUTEX, (long)addr, FUTEX_WAIT, val, timeout);
}

// Futex wake: wake up to count waiters
// Returns number of waiters woken
static inline int futex_wake(volatile unsigned int *addr, int count) {
    return (int)syscall3(SYS_FUTEX, (long)addr, FUTEX_WAKE, count);
}

// Full futex syscall (for advanced operations)
static inline int futex(volatile unsigned int *addr, int op, unsigned int val,
                        unsigned long timeout, unsigned int *addr2, unsigned int val3) {
    return (int)syscall6(SYS_FUTEX, (long)addr, op, val, timeout, (long)addr2, val3);
}

// Theme management
#ifndef SYS_SET_ICON_SIZE
#define SYS_SET_ICON_SIZE 208
#define SYS_GET_ICON_SIZE 209
#define SYS_NET_SET_STATIC 217
#define SYS_NET_DHCP       218
#define SYS_NET_IS_UP      299 // #374 network usable? 1=up 0=down
#define SYS_SET_DISPLAY_FX 219
#define SYS_GET_DISPLAY_FX 220
#define SYS_DRAW_TTF       221
#define SYS_MEASURE_TTF    222
#define SYS_SET_FONT_SIZE  223
#define SYS_GET_FONT_SIZE  224
#define SYS_SET_SCREENSAVER 225
#define SYS_GET_SCREENSAVER 226
#define SYS_SCREENSAVER_TEST 227
#define SYS_GET_SS_TEST     228
#define SYS_SET_SS_DELAY    250
#define SYS_GET_SS_DELAY    251
#define SYS_UPTIME_MS       252
#define SYS_DECODE_IMAGE    253
#define SYS_WIN_DRAW_IMAGE  254
#define SYS_HTTP_FETCH_START    255
#define SYS_HTTP_FETCH_POLL     256
#define SYS_HTTP_FETCH_READ     257
#define SYS_HTTP_FETCH_CANCEL   258
#define SYS_SET_SETTINGS_TAB 229
#define SYS_GET_SETTINGS_TAB 230
#endif
static inline int set_icon_size(int sz) {
    return (int)syscall1(SYS_SET_ICON_SIZE, sz);
}
static inline int get_icon_size(void) {
    return (int)syscall0(SYS_GET_ICON_SIZE);
}
static inline int set_theme(int theme_id) {
    return (int)syscall1(SYS_SET_THEME, theme_id);
}
static inline int get_theme(void) {
    return (int)syscall0(SYS_GET_THEME);
}

// Audio volume / mute
static inline int set_volume(int volume) {
    return (int)syscall1(SYS_SET_VOLUME, volume);
}
static inline int get_volume(void) {
    return (int)syscall0(SYS_GET_VOLUME);
}
static inline int set_mute(int mute) {
    return (int)syscall1(SYS_SET_MUTE, mute);
}

// Disk info (returns MB)
static inline long get_disk_total_mb(void) {
    return (long)syscall0(SYS_GET_DISK_TOTAL);
}
static inline long get_disk_free_mb(void) {
    return (long)syscall0(SYS_GET_DISK_FREE);
}

// Mouse sensitivity (1=slow, 5=normal, 10=fast)
static inline int set_mouse_speed(int speed) {
    return (int)syscall1(SYS_SET_MOUSE_SPEED, speed);
}
static inline int get_mouse_speed(void) {
    return (int)syscall0(SYS_GET_MOUSE_SPEED);
}

// RTC time/date
static inline void get_rtc_time(int *hour, int *min, int *sec) {
    long packed = syscall0(SYS_GET_RTC_TIME);
    *hour = (int)((packed >> 16) & 0xFF);
    *min  = (int)((packed >> 8)  & 0xFF);
    *sec  = (int)(packed & 0xFF);
}
static inline void get_rtc_date(int *day, int *month, int *year) {
    long packed = syscall0(SYS_GET_RTC_DATE);
    *year  = (int)((packed >> 16) & 0xFFFF);
    *month = (int)((packed >> 8)  & 0xFF);
    *day   = (int)(packed & 0xFF);
}

// Password change (username, old password, new password)
static inline int passwd_change(const char *user, const char *old_pass, const char *new_pass) {
    return (int)syscall3(SYS_PASSWD_CHANGE, (long)user, (long)old_pass, (long)new_pass);
}

// Add user (username, uid, gid, home dir, shell)
static inline int adduser(const char *username, int uid, int gid,
                           const char *home, const char *shell) {
    return (int)syscall5(SYS_ADDUSER, (long)username, uid, gid, (long)home, (long)shell);
}

// Network info struct (must match kernel net_info_t exactly)
typedef struct {
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char mac[18];
    int  connected;
} net_info_t;

// Set RTC time (h, m, s all 0-59/23)
static inline long set_rtc_time(int h, int m, int s) {
    return syscall1(SYS_SET_RTC_TIME,
                    (long)(((h) << 16) | ((m) << 8) | (s)));
}

// Set RTC date (year full e.g. 2026, mo 1-12, day 1-31)
static inline long set_rtc_date(int y, int mo, int d) {
    return syscall1(SYS_SET_RTC_DATE,
                    (long)(((y) << 16) | ((mo) << 8) | (d)));
}

// Get network info
static inline int net_set_static(const char *ip, const char *mask, const char *gw) {
    return (int)syscall3(SYS_NET_SET_STATIC, (long)ip, (long)mask, (long)gw);
}
static inline int set_display_fx(int brightness, int nightlight) {
    return (int)syscall2(SYS_SET_DISPLAY_FX, brightness, nightlight);
}
static inline void ttf_text(int x, int y, const char *str, int size, unsigned int color) {
    syscall5(SYS_DRAW_TTF, x, y, (long)str, size, (long)color);
}
static inline int ttf_measure(const char *str, int size) {
    return (int)syscall2(SYS_MEASURE_TTF, (long)str, size);
}
static inline int set_screensaver(int t) {
    return (int)syscall1(SYS_SET_SCREENSAVER, t);
}
static inline int get_screensaver(void) {
    return (int)syscall0(SYS_GET_SCREENSAVER);
}
static inline int set_ss_delay(int seconds) {   // (#115)
    return (int)syscall1(SYS_SET_SS_DELAY, seconds);
}
static inline int get_ss_delay(void) {          // (#115)
    return (int)syscall0(SYS_GET_SS_DELAY);
}
static inline unsigned long uptime_ms(void) {   // monotonic ms since boot
    return (unsigned long)syscall0(SYS_UPTIME_MS);
}
#define SYS_SET_WIN_BLANK 231
static inline int set_win_blank(int on) {
    return (int)syscall1(SYS_SET_WIN_BLANK, on);
}

// #74: ask Settings which panel to open on next launch (one-shot, -1 = none).
static inline int set_settings_tab(int tab) {
    return (int)syscall1(SYS_SET_SETTINGS_TAB, (long)tab);
}
static inline int get_settings_tab(void) {
    return (int)syscall0(SYS_GET_SETTINGS_TAB);
}
static inline int test_screensaver(void) {
    return (int)syscall0(SYS_SCREENSAVER_TEST);
}
static inline int get_ss_test(void) {
    return (int)syscall0(SYS_GET_SS_TEST);
}
static inline int set_font_size(int sz) {
    return (int)syscall1(SYS_SET_FONT_SIZE, sz);
}
static inline int get_font_size(void) {
    return (int)syscall0(SYS_GET_FONT_SIZE);
}
static inline int get_display_fx(void) {
    return (int)syscall0(SYS_GET_DISPLAY_FX);
}
static inline int net_dhcp(void) {
    return (int)syscall0(SYS_NET_DHCP);
}
static inline long get_net_info(net_info_t *buf, long len) {
    return syscall2(SYS_GET_NET_INFO, (long)buf, len);
}

// TCP socket wrappers
static inline int tcp_socket(void) {
    return (int)syscall1(SYS_SOCKET, 0);
}
static inline int tcp_connect(int sock, unsigned int ip, int port) {
    return (int)syscall3(SYS_CONNECT, sock, ip, port);
}
static inline int tcp_send(int sock, const void *buf, int len) {
    return (int)syscall3(SYS_SEND, sock, (long)buf, len);
}
static inline int tcp_recv(int sock, void *buf, int len) {
    return (int)syscall3(SYS_RECV, sock, (long)buf, len);
}
static inline int tcp_close(int sock) {
    return (int)syscall1(SYS_TCP_CLOSE, sock);
}
static inline int tcp_get_state(int sock) {
    return (int)syscall1(SYS_TCP_STATE, sock);
}
// #443: bind()+listen() in one call (mirrors the kernel-side SYS_LISTEN, which
// folds tcp_bind()+tcp_listen() together since this codebase never exposed a
// standalone bind() to userland). accept() is non-blocking, like tcp_recv():
// returns TCP_ERR_WOULD_BLOCK (-9) if no connection is pending yet.
static inline int tcp_listen(int sock, int port, int backlog) {
    return (int)syscall3(SYS_LISTEN, sock, port, backlog);
}
static inline int tcp_accept(int sock) {
    return (int)syscall1(SYS_ACCEPT, sock);
}

// ICMP ping: dest_ip in host byte order, timeout in ms.
// Returns approximate round-trip ms (>=0) on reply, -1 on timeout/error.
static inline int sys_ping(unsigned int dest_ip, int timeout_ms) {
    return (int)syscall2(SYS_PING, dest_ip, timeout_ms);
}

// NTP sync: returns 0 on success, -1 on failure
static inline long ntp_sync(void) {
    return syscall0(SYS_NTP_SYNC);
}
static inline long set_cursor_theme(int theme) {
    return syscall1(SYS_SET_CURSOR_THEME, (long)theme);
}
static inline long get_cursor_theme(void) {
    return syscall0(SYS_GET_CURSOR_THEME);
}


// ============================================================================
// IPC + Compositor Syscall Numbers (must match kernel/proc/syscall.h)
// ============================================================================

// Window manager query
#define SYS_WM_GET_WINDOWS      155
#define SYS_WM_FOCUS_WINDOW    157

// Compositor: render windows (kernel draws on behalf of compositor)
#define SYS_COMPOSITOR_RENDER_WINDOWS  156

// IPC: Message Passing
#define SYS_MSG_CREATE_CHANNEL  160
#define SYS_MSG_CONNECT         161
#define SYS_MSG_SEND            162
#define SYS_MSG_RECV            163
#define SYS_MSG_ACCEPT          164
#define SYS_MSG_CLOSE           165
#define SYS_MSG_DESTROY         166

// IPC: Shared Memory
#define SYS_SHM_CREATE          170
#define SYS_SHM_MAP             171
#define SYS_SHM_UNMAP           172
#define SYS_SHM_DESTROY         173
#define SYS_SHM_INFO            174

// IPC: Name Service
#define SYS_IPC_REGISTER_NAME   180
#define SYS_IPC_LOOKUP_NAME     181

// Framebuffer / Compositor
#define SYS_FB_MAP              200
#define SYS_FB_INFO             201
#define SYS_FB_FLIP             202
#define SYS_FB_DAMAGE           203
#define SYS_GET_MOUSE           210
#define SYS_SET_MOUSE           211
#define SYS_GET_KEY             212
#define SYS_GRAB_INPUT          213
#define SYS_INJECT_MOUSE        214
// #443: set the PHYSICAL mouse button bitmask (same variable the real PS/2 IRQ
// path writes). set_mouse_pos() above only ever warps the cursor; the
// desktop's own icon/taskbar/start-menu click handling polls the physical
// button state directly, not sys_inject_mouse()'s window-manager relay, so
// injected clicks reached app windows but never registered on the desktop.
#define SYS_SET_MOUSE_BUTTONS   305

// Mouse event types for sys_inject_mouse()
#define MOUSE_EVENT_MOVE        0
#define MOUSE_EVENT_DOWN        1
#define MOUSE_EVENT_UP          2

// ============================================================================
// Window Info (must match kernel gui/window.h wm_window_info_t exactly)
// ============================================================================

typedef struct {
    int  id, x, y, width, height, visible;
    int  minimized, focused;
    char title[64];
} wm_window_info_t;

static inline int wm_get_windows(wm_window_info_t *buf, int n) {
    return (int)syscall2(SYS_WM_GET_WINDOWS, (long)buf, (long)n);
}

static inline int wm_focus(int id) {
    return (int)syscall1(SYS_WM_FOCUS_WINDOW, id);
}

#define SYS_WM_MINIMIZE_WINDOW 158
static inline int wm_minimize(int id) {
    return (int)syscall1(SYS_WM_MINIMIZE_WINDOW, id);
}

// Non-blocking DNS (poll-split). dns_start: 1=resolved now (*ip set), 0=pending,
// <0=error. dns_poll: 1=done (*ip set), 0=pending, -1=failed. IP is in
// (a<<24)|(b<<16)|(c<<8)|d form, ready for tcp_connect().
#define SYS_DNS_START 215
#define SYS_DNS_POLL  216
static inline int dns_start(const char *host, unsigned int *ip) {
    return (int)syscall2(SYS_DNS_START, (long)host, (long)ip);
}
static inline int dns_poll(unsigned int *ip) {
    return (int)syscall1(SYS_DNS_POLL, (long)ip);
}

// ============================================================================
// Framebuffer structures (must match kernel gui/fb_syscall.h exactly)
// ============================================================================

typedef struct { unsigned int width, height, pitch, bpp; unsigned long phys_addr; } fb_info_t;
typedef struct { unsigned int keycode, scancode, modifiers; int pressed; unsigned long ts; } key_evt_t;
typedef struct { int x, y, dx, dy; unsigned int buttons; int scroll; unsigned long ts; } mouse_evt_t;

static inline long     fb_map(void)           { return syscall0(SYS_FB_MAP); }
static inline int      fb_info(fb_info_t *i)  { return (int)syscall1(SYS_FB_INFO, (long)i); }
static inline int      fb_flip(void)          { return (int)syscall0(SYS_FB_FLIP); }
static inline int      fb_damage(int x, int y, int w, int h) {
    return (int)syscall4(SYS_FB_DAMAGE, x, y, w, h); }
static inline int      get_mouse_evt(mouse_evt_t *m) { return (int)syscall1(SYS_GET_MOUSE, (long)m); }
static inline int      get_mouse(int *x, int *y, unsigned int *buttons) {
    return (int)syscall3(SYS_GET_MOUSE, (long)x, (long)y, (long)buttons); }
static inline int      set_mouse_pos(int x, int y)   { return (int)syscall2(SYS_SET_MOUSE, x, y); }
static inline int      set_mouse_buttons(unsigned int mask) {
    return (int)syscall1(SYS_SET_MOUSE_BUTTONS, (long)mask); }
static inline int      get_key_evt(key_evt_t *k)     { return (int)syscall1(SYS_GET_KEY, (long)k); }
static inline int      grab_input(int grab)           { return (int)syscall1(SYS_GRAB_INPUT, (long)grab); }
// Forward a mouse event from the compositor into the kernel window manager.
// type: MOUSE_EVENT_MOVE/DOWN/UP. Returns >0 if a window consumed a DOWN event.
static inline int      sys_inject_mouse(int x, int y, int type, int button) {
    return (int)syscall4(SYS_INJECT_MOUSE, x, y, type, button); }
static inline int      compositor_render_windows(void) {
    return (int)syscall0(SYS_COMPOSITOR_RENDER_WINDOWS); }

// ============================================================================
// IPC: Message Passing Wrappers
// ============================================================================

static inline int  msg_create_channel(void) {
    return (int)syscall0(SYS_MSG_CREATE_CHANNEL); }
static inline int  msg_connect(int ch) {
    return (int)syscall1(SYS_MSG_CONNECT, (long)ch); }
static inline long msg_send(int conn, const void *data, unsigned long len) {
    return syscall3(SYS_MSG_SEND, (long)conn, (long)data, (long)len); }
static inline long msg_recv(int conn, void *buf, unsigned long len, int timeout) {
    return syscall4(SYS_MSG_RECV, (long)conn, (long)buf, (long)len, (long)timeout); }
static inline int  msg_accept(int ch, int timeout) {
    return (int)syscall2(SYS_MSG_ACCEPT, (long)ch, (long)timeout); }
static inline int  msg_close(int conn) {
    return (int)syscall1(SYS_MSG_CLOSE, (long)conn); }
static inline int  msg_destroy_channel(int ch) {
    return (int)syscall1(SYS_MSG_DESTROY, (long)ch); }

// ============================================================================
// IPC: Shared Memory Wrappers
// ============================================================================

#define SHM_FLAG_NONE       0x00
#define SHM_FLAG_READONLY   0x01
#define SHM_FLAG_EXCLUSIVE  0x02

static inline int shm_create(unsigned long size, int flags) {
    return (int)syscall2(SYS_SHM_CREATE, (long)size, (long)flags); }
static inline int shm_map(int id, void **addr) {
    return (int)syscall2(SYS_SHM_MAP, (long)id, (long)addr); }
static inline int shm_unmap(int id) {
    return (int)syscall1(SYS_SHM_UNMAP, (long)id); }
static inline int shm_destroy(int id) {
    return (int)syscall1(SYS_SHM_DESTROY, (long)id); }

// ============================================================================
// IPC: Name Service Wrappers
// ============================================================================

static inline int ipc_register_name(const char *name, int ch) {
    return (int)syscall2(SYS_IPC_REGISTER_NAME, (long)name, (long)ch); }
static inline int ipc_lookup_name(const char *name) {
    return (int)syscall1(SYS_IPC_LOOKUP_NAME, (long)name); }


// ============================================================================
// Compositor support syscalls (Phase 3)
// ============================================================================

#define SYS_LIST_USERS      190
#define SYS_AUTHENTICATE    191
#define SYS_PLAY_WAV        192
#define SYS_GET_CPU_USAGE   193
#define SYS_GET_MEM_INFO    194
#define SYS_GET_KEYBOARD    195
#define SYS_GET_CPU_PER_CORE 259  /* buf[0]=core count, buf[1..]=per-core %% */

typedef struct {
    char username[64];
    char display_name[64];
    unsigned int uid;
    unsigned int gid;
    unsigned char active;
    unsigned char padding[3];
} user_info_t;

static inline int sys_list_users(void *buf, int max_count) {
    return (int)syscall2(SYS_LIST_USERS, (long)buf, max_count); }
static inline int sys_authenticate(const char *username, const char *password) {
    return (int)syscall2(SYS_AUTHENTICATE, (long)username, (long)password); }

#define SYS_DELETE_USER     159
static inline int delete_user(const char *username) {
    return (int)syscall1(SYS_DELETE_USER, (long)username); }

#define SYS_GET_DISK_INFO   199
typedef struct {
    unsigned char  present;
    unsigned char  type;     // 0=ATA, 1=ATAPI
    signed char    smart;    // 1=ok, 0=failing, -1=unknown
    unsigned char  pad;
    unsigned int   size_mb;
    char           model[41];
    char           serial[21];
    char           pad2[2];
} disk_info_t;
static inline int get_disk_info(int idx, disk_info_t *out) {
    return (int)syscall2(SYS_GET_DISK_INFO, (long)idx, (long)out); }

#define SYS_SET_WALLPAPER   204
#define SYS_GET_WALLPAPER   205
#define SYS_POWEROFF        206
#define SYS_REBOOT          207
#define SYS_SET_ICON_SIZE 208
#define SYS_GET_ICON_SIZE 209
#define SYS_NET_SET_STATIC 217
#define SYS_NET_DHCP       218
#define SYS_SET_DISPLAY_FX 219
#define SYS_GET_DISPLAY_FX 220
#define SYS_DRAW_TTF       221
#define SYS_MEASURE_TTF    222
#define SYS_SET_FONT_SIZE  223
#define SYS_GET_FONT_SIZE  224
#define SYS_SET_SCREENSAVER 225
#define SYS_GET_SCREENSAVER 226
#define SYS_SCREENSAVER_TEST 227
#define SYS_GET_SS_TEST     228
#define SYS_SET_SS_DELAY    250
#define SYS_GET_SS_DELAY    251
#define SYS_UPTIME_MS       252
#define SYS_DECODE_IMAGE    253
#define SYS_WIN_DRAW_IMAGE  254
#define SYS_SET_SETTINGS_TAB 229
#define SYS_GET_SETTINGS_TAB 230
static inline void poweroff(void) { syscall0(SYS_POWEROFF); }
static inline void reboot(void)   { syscall0(SYS_REBOOT); }
static inline int set_wallpaper(int idx) {
    return (int)syscall1(SYS_SET_WALLPAPER, (long)idx); }
static inline int get_wallpaper(void) {
    return (int)syscall0(SYS_GET_WALLPAPER); }
static inline int sys_play_wav(const char *path) {
    return (int)syscall1(SYS_PLAY_WAV, (long)path); }

/* ---------------------------------------------------------------------------
 * Ring-3 PCM push (Phase 1 of the Ring-0 media-decode exit).
 *
 * sys_play_wav() above hands a PATH to the kernel, which then parses and
 * decodes the file IN RING 0 via media/ (faad2/opus/tremor/dr_flac/libmad,
 * ~121K LOC of vendored C reading an attacker-controlled file). That is how
 * MAYTERA-SEC-2026-0009 (heap OOB read in media/aac.c mp4_parse, via a crafted
 * .m4a) is reachable from Ring 3.
 *
 * These three let an app DECODE IN USERLAND and push raw PCM instead, so a
 * decoder bug crashes only the app. sys_play_wav() is unchanged and still
 * works: prefer these for new code.
 *
 * Contract:
 *   open  -> handle >= 1, or < 0 on error. format 0 means S16_LE (the only
 *            format accepted in Phase 1). channels 1 or 2.
 *   write -> frames ACCEPTED (may be < frames on signal/teardown), or < 0.
 *            BLOCKS on a kernel wait queue while the DAC ring is full: it does
 *            NOT spin, and you must NOT poll around it. Just keep feeding it.
 *   close -> drains the ring, joins the kernel pump thread, returns 0.
 * The stream is owned by the calling PID; another process cannot write or
 * close it. If the owner dies without closing, proc_exit() tears it down.
 * ------------------------------------------------------------------------- */
#ifndef SYS_AUDIO_PCM_OPEN
#define SYS_AUDIO_PCM_OPEN  315
#define SYS_AUDIO_PCM_WRITE 316
#define SYS_AUDIO_PCM_CLOSE 317
#endif
static inline int sys_audio_pcm_open(unsigned rate, unsigned channels, unsigned format) {
    return (int)syscall3(SYS_AUDIO_PCM_OPEN, (long)rate, (long)channels, (long)format); }
static inline int sys_audio_pcm_write(int handle, const void *pcm, unsigned frames) {
    return (int)syscall3(SYS_AUDIO_PCM_WRITE, (long)handle, (long)pcm, (long)frames); }
static inline int sys_audio_pcm_close(int handle) {
    return (int)syscall1(SYS_AUDIO_PCM_CLOSE, (long)handle); }
static inline int sys_get_cpu_usage(void) {
    return (int)syscall0(SYS_GET_CPU_USAGE); }

/* Per-core CPU usage. buf must hold at least 65 uint32: buf[0]=core count,
 * buf[1..count]=per-core %% (0-100). Returns core count. */
static inline int sys_get_cpu_per_core(unsigned int *buf) {
    return (int)syscall1(SYS_GET_CPU_PER_CORE, (long)buf); }
static inline int sys_get_mem_info(unsigned long *total, unsigned long *used) {
    return (int)syscall2(SYS_GET_MEM_INFO, (long)total, (long)used); }
#define SYS_GET_NET_BYTES 234
static inline unsigned long get_net_bytes(void) {
    return (unsigned long)syscall0(SYS_GET_NET_BYTES); }
static inline int sys_get_keyboard(void) {
    return (int)syscall0(SYS_GET_KEYBOARD); }
static inline long sys_get_rtc_time(void) {
    return syscall0(SYS_GET_RTC_TIME); }
static inline long sys_get_disk_total(void) {
    return syscall0(SYS_GET_DISK_TOTAL); }
#define SYS_SPAWN           196
#define SYS_INJECT_KEY      197

static inline int sys_spawn(const char *path) {
    return (int)syscall1(SYS_SPAWN, (long)path); }

#define SYS_WM_MAXIMIZE_WINDOW 260
static inline int sys_wm_maximize_focused(void) { return (int)syscall0(SYS_WM_MAXIMIZE_WINDOW); }
// #185: borderless panel + OS-wide mouse wheel
#define SYS_WIN_SET_NOCHROME 262
static inline int win_set_nochrome(int handle) { return (int)syscall1(SYS_WIN_SET_NOCHROME, (long)handle); }
#define SYS_GET_MOUSE_SCROLL 263
static inline int get_mouse_scroll(void) { return (int)syscall0(SYS_GET_MOUSE_SCROLL); }
#define SYS_GET_GLOBAL_MOUSE 264  // #185: read-only global cursor for any process
static inline int get_global_mouse(int *x, int *y, unsigned int *buttons) {
    return (int)syscall3(SYS_GET_GLOBAL_MOUSE, (long)x, (long)y, (long)buttons); }
#define MOUSE_EVENT_SCROLL 3


#define SYS_RUN_NEXT_ON_AP 261
static inline int sys_run_next_on_ap(void) { return (int)syscall0(SYS_RUN_NEXT_ON_AP); }
static inline int sys_inject_key(int key) {
    return (int)syscall1(SYS_INJECT_KEY, (long)key); }
#define SYS_SPAWN_ARGS      198

static inline int sys_spawn_args(const char *path, char **argv, int argc) {
    return (int)syscall3(SYS_SPAWN_ARGS, (long)path, (long)argv, (long)argc);
}

#define SYS_SPAWN_REDIR     247
// Spawn a child with stdin/stdout redirected to files (shell I/O redirection).
// infile/outfile may be NULL; append != 0 opens the output file for append.
static inline int sys_spawn_redir(const char *path, char **argv, int argc,
                                  const char *infile, const char *outfile, int append) {
    return (int)syscall6(SYS_SPAWN_REDIR, (long)path, (long)argv, (long)argc,
                         (long)infile, (long)outfile, (long)append);
}

// (#116) Live mouse-cursor style/size. Settings calls set_cursor() on change; the
// compositor reads get_cursor() each frame (same live-apply pattern as opacity).
#define SYS_SET_CURSOR      248
#define SYS_GET_CURSOR      249
static inline int set_cursor(int style, int size) {
    return (int)syscall2(SYS_SET_CURSOR, (long)style, (long)size);
}
static inline int get_cursor(void) {   // packed: style (low 8) | size<<8
    return (int)syscall0(SYS_GET_CURSOR);
}

#define SYS_WIN16_RUN       237
// Run a Win16 (NE/.COM) executable by path. Blocks until the app window
// closes, then returns 0 on success or <0 on error. Used by the Start menu
// to launch installed Win3.x programs.
static inline int win16_run(const char *path) {
    return (int)syscall1(SYS_WIN16_RUN, (long)path);
}

#define SYS_DOS_RUN         240
// Run an MS-DOS (.EXE/.COM) program by path in its own window (#208). The
// kernel spawns a dedicated proc + host window (non-blocking) and returns 0 on
// spawn, <0 on error. Used by the Start menu to launch DOS games (TIM, Keen).
static inline int dos_run(const char *path) {
    return (int)syscall1(SYS_DOS_RUN, (long)path);
}

#define SYS_SSH_CLIENT      242
// Interactive SSH-2 client (used by /APPS/SSH). Bridges the kernel SSH client to
// this process's stdin/stdout; blocks until the remote session closes. ip is an
// IPv4 in a.b.c.d-packed host order; csr packs (cols<<16)|rows. Returns 0 / <0.
static inline int ssh_client(unsigned int ip, const char *user, const char *pass,
                             int csr, int port) {
    return (int)syscall5(SYS_SSH_CLIENT, (long)ip, (long)user, (long)pass,
                         (long)csr, (long)port);
}

#define SYS_NET_INFO        243
// Fill `buf` with a verbose network-interface report (used by /APPS/IP).
// Returns bytes written, or <0 on error.
static inline int net_info(char *buf, int len) {
    return (int)syscall2(SYS_NET_INFO, (long)buf, (long)len);
}

#define SYS_WIN16_ACTIVE    241
// (#200 SkiFree) Returns 1 while a Win16 app owns the foreground/keyboard, else
// 0. The compositor treats a running Win16 app as continuous activity so its
// idle screensaver does not black out the game (the Win16 message pump is the
// sole keyboard consumer, so SYS_GET_KEYBOARD returns -1 to the compositor and
// it would otherwise time out into the screensaver).
static inline int sys_win16_active(void) {
    return (int)syscall0(SYS_WIN16_ACTIVE);
}

// dup/dup2/pipe are provided by unistd.{h,c} (libc.a). They used to also be
// inline here, which collided with unistd.c when both were included; removed.

// waitpid: wait for specific child process
static inline int sys_waitpid(int pid, int *status, int options) {
    return (int)syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}


static inline long sys_get_disk_free(void) {
    return syscall0(SYS_GET_DISK_FREE); }


// ============================================================================
// Network: HTTP/HTTPS Fetch
// ============================================================================

// #374: authoritative network-up gate. Widgets/services consult this FIRST and
// skip network work entirely when it returns 0 (link down / no IP / stack down),
// so a machine with no working NIC never burns a multi-second connect timeout.
static inline int sys_net_is_up(void) {
    return (int)syscall0(SYS_NET_IS_UP);
}

// #414 Home Assistant: blocking GET with auth headers (e.g. Authorization: Bearer).
#define SYS_HTTP_FETCH_HDR 302
static inline int sys_http_fetch_hdr(const char *url, const char *headers, char *buf,
                                     unsigned int max_len, unsigned int *bytes_read, int *http_status) {
    return (int)syscall6(SYS_HTTP_FETCH_HDR, (long)url, (long)headers, (long)buf,
                         (long)max_len, (long)bytes_read, (long)http_status);
}

static inline int sys_http_fetch(const char *url, char *buf, unsigned int max_len,
                                 unsigned int *bytes_read, int *http_status) {
    return (int)syscall5(SYS_HTTP_FETCH, (long)url, (long)buf,
                         (long)max_len, (long)bytes_read, (long)http_status);
}

// Async (non-blocking) HTTP fetch (#277): start -> poll each frame -> read body.
static inline int http_fetch_start(const char *url) {
    return (int)syscall1(SYS_HTTP_FETCH_START, (long)url);
}
static inline int http_fetch_poll(int id, int *status, unsigned int *len) {
    return (int)syscall3(SYS_HTTP_FETCH_POLL, id, (long)status, (long)len);
}
static inline int http_fetch_read(int id, char *buf, unsigned int max) {
    return (int)syscall3(SYS_HTTP_FETCH_READ, id, (long)buf, (long)max);
}
static inline int http_fetch_cancel(int id) {
    return (int)syscall1(SYS_HTTP_FETCH_CANCEL, id);
}

// HTTPS POST: headers = extra CRLF-terminated header lines (Authorization etc.;
// Content-Type/Length added by the kernel). body = request body (JSON). Returns
// response bytes written to buf, or -1; *http_status gets the HTTP status.
#define SYS_HTTP_POST 239
static inline int sys_http_post(const char *url, const char *headers, const char *body,
                                char *buf, unsigned int max_len, int *http_status) {
    return (int)syscall6(SYS_HTTP_POST, (long)url, (long)headers, (long)body,
                         (long)buf, (long)max_len, (long)http_status);
}

// #264 async HTTPS POST (kernel worker proc; user app never runs net code).
// START copies url/headers/body into the kernel and spawns the worker, returning
// a job id (>=0) or -1. POLL returns 0=running/1=done/2=error (and fills status,
// len). READ copies the response body out and frees the job. CANCEL aborts.
#define SYS_HTTP_POST_START  265
#define SYS_HTTP_POST_POLL   266
#define SYS_HTTP_POST_READ   267
#define SYS_HTTP_POST_CANCEL 268
static inline int http_post_start(const char *url, const char *headers, const char *body) {
    return (int)syscall3(SYS_HTTP_POST_START, (long)url, (long)headers, (long)body);
}
static inline int http_post_poll(int id, int *status, unsigned int *len) {
    return (int)syscall3(SYS_HTTP_POST_POLL, id, (long)status, (long)len);
}
static inline int http_post_read(int id, char *buf, unsigned int max) {
    return (int)syscall3(SYS_HTTP_POST_READ, id, (long)buf, (long)max);
}
static inline int http_post_cancel(int id) {
    return (int)syscall1(SYS_HTTP_POST_CANCEL, id);
}

// ---- #317 SMB network mounts ----
#define SYS_NET_MOUNT       269
#define SYS_NET_LIST_SHARES 270
#define SYS_NET_UNMOUNT     271
// Mount an SMB share with credentials (cached for "/SMB/<server>/<share>/..."
// access). user/pass may be NULL/"" for guest. Returns 0 on success, -1 on error.
static inline int net_mount(const char *server, const char *share,
                            const char *user, const char *pass) {
    return (int)syscall4(SYS_NET_MOUNT, (long)server, (long)share,
                         (long)user, (long)pass);
}
// Enumerate a server's shares (srvsvc). Fills buf with newline-separated names;
// returns the share count (>=0) or -1.
static inline int net_list_shares(const char *server, char *buf, unsigned int maxlen) {
    return (int)syscall3(SYS_NET_LIST_SHARES, (long)server, (long)buf, (long)maxlen);
}
static inline int net_unmount(const char *server, const char *share) {
    return (int)syscall2(SYS_NET_UNMOUNT, (long)server, (long)share);
}

// ---- Task Manager: process table snapshot (#159) ----
#define SYS_PROC_LIST 238
typedef struct {
    unsigned int       pid;
    unsigned int       ppid;
    char               name[32];
    unsigned int       state;
    unsigned int       mem_kb;
    unsigned long long cpu_ticks;
    int                running_cpu; // #279: AP id or -1
} proc_info_t;
static inline int sys_proc_list(proc_info_t *buf, int max) {
    return (int)syscall2(SYS_PROC_LIST, (long)buf, (long)max);
}

// ---- #487/#349 Ring-3 process introspection --------------------------------
// The kernel knew about open handles, socket ownership, services and scheduled
// tasks; before these it had no way to tell Ring 3 about any of it, so the
// Task Manager could only ever draw a process list. Layouts MUST match the
// kernel's proc/procinfo.h (each is _Static_assert-locked on the kernel side
// and const-assert-locked in the Rust app).
#define SYS_PROC_HANDLES       318
#define SYS_NET_CONNS          319
#define SYS_SVC_LIST           320
#define SYS_SVC_CONTROL        321
#define SYS_PROC_DETAIL        322

#define PI_PATH_MAX  96
#define PI_NAME_MAX  32
#define PI_KIND_FILE    0
#define PI_KIND_DEV     1
#define PI_KIND_PIPE    2
#define PI_KIND_SOCKET  3
#define PI_KIND_UNKNOWN 4
#define PI_SVC_STOP   0
#define PI_SVC_START  1
// Every connection regardless of owner. Not 0: pid 0 is a legitimate query
// meaning "kernel-internal / unowned".
#define PI_PID_ALL 0xFFFFFFFFu

typedef struct {
    int fd;
    int flags;
    unsigned int kind;           // PI_KIND_*
    unsigned int _pad;
    char path[PI_PATH_MAX];      // always NUL-terminated; "" = anonymous
} handle_info_t;

typedef struct {
    unsigned char  state;        // tcp_state_t
    unsigned char  is_listener;
    unsigned short local_port;
    unsigned short remote_port;
    unsigned int   remote_ip;
    unsigned short recv_len;
    unsigned int   send_len;
    unsigned int   owner_pid;    // 0 = unowned / kernel-internal
} tcp_conn_info_t;

typedef struct {
    unsigned int running, autostart, perms, pid;
    char name[PI_NAME_MAX];
    char account[PI_NAME_MAX];
} svc_info_t;

typedef struct {
    unsigned int pid, ppid;
    unsigned int working_set_kb, private_kb, virt_kb, heap_kb;
    unsigned int threads, handles, uid, gid, priority, privilege;
    unsigned int state, vma_count, mem_flags, is_service;
    unsigned long long cpu_ticks;
    unsigned long long cr3;
    char name[PI_NAME_MAX];
} proc_detail_t;

// A process's open handles, named. Returns rows written, or -1.
static inline int sys_proc_handles(unsigned int pid, handle_info_t *buf, int max) {
    return (int)syscall3(SYS_PROC_HANDLES, pid, (long)buf, max);
}
// TCP connections owned by `pid`, or every connection when pid == PI_PID_ALL.
static inline int sys_net_conns(unsigned int pid, tcp_conn_info_t *buf, int max) {
    return (int)syscall3(SYS_NET_CONNS, pid, (long)buf, max);
}
// The service registry (#95). Returns rows written, or -1.
static inline int sys_svc_list(svc_info_t *buf, int max) {
    return (int)syscall2(SYS_SVC_LIST, (long)buf, max);
}
// Start/stop a service by name (PI_SVC_START / PI_SVC_STOP).
static inline int sys_svc_control(const char *name, int action) {
    return (int)syscall2(SYS_SVC_CONTROL, (long)name, action);
}
// Full per-process detail incl. the real memory breakdown. Returns 1, or -1.
static inline int sys_proc_detail(unsigned int pid, proc_detail_t *out) {
    return (int)syscall2(SYS_PROC_DETAIL, pid, (long)out);
}


// Decode+point-sample an image to a target box. dims[0]/dims[1] get the result
// w/h. Returns bytes written (dw*dh*4) or -1. Pixels are BGRA (framebuffer order).
static inline int decode_image(const void *data, unsigned int len,
                               int tw, int th, void *out,
                               unsigned int out_cap, int *dims) {
    unsigned int target = (((unsigned int)tw) << 16) | (((unsigned int)th) & 0xFFFFu);
    return (int)syscall6(SYS_DECODE_IMAGE, (long)data, (long)len, (long)target,
                         (long)out, (long)out_cap, (long)dims);
}


// Blit a w*h BGRA buffer into the window's content at (x,y) (clipped).
static inline int win_draw_image(int handle, int x, int y, int w, int h,
                                 const void *pixels) {
    return (int)syscall6(SYS_WIN_DRAW_IMAGE, handle, x, y, w, h, (long)pixels);
}

// ---- #307 real-hardware boot log ----
#define SYS_BOOTLOG_WRITE 298
// Append a short single-line diagnostic to the persistent /BOOTLOG.TXT the
// kernel is (also) writing to. Added so the compositor's login screen - where
// the physical iMac14,4 "No user accounts found" bug is actually visible -
// can record its own state transitions for later offline diagnosis.
static inline int sys_bootlog(const char *msg) {
    return (int)syscall1(SYS_BOOTLOG_WRITE, (long)msg);
}


/* #71 userland HDA audio bring-up debug (op,a,b,c). See hdadbg app. */
#ifndef SYS_HDA_DBG
#define SYS_HDA_DBG 306
#endif
static inline long sys_hda_dbg(int op, long a, long b, long c) {
    return syscall4(SYS_HDA_DBG, (long)op, a, b, c);
}

#endif // _SYSCALL_H
