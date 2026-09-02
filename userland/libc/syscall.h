// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
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
// #695. Defined HERE, next to SYS_CLOSE, and not down with the high numbers:
// sys_fsync()'s inline wrapper is ~100 lines above that block, and a #define
// that appears after its only use is an "undeclared" error, not a style issue.
// (#745 local 109) ftruncate(fd, length): shrink an open file. Shrink only;
// a grow is refused rather than silently ignored, which is what the previous
// no-op stub in posixextra.c did.
#define SYS_FTRUNCATE       386
// #115 (local 120): utime(path, atime, mtime). Seconds since the UNIX epoch.
// -1 keeps a timestamp, -2 means "the KERNEL's now" - userland must use -2
// rather than sending time(), which returns seconds since boot (#113).
// The UTIME_KEEP / UTIME_NOW sentinels are declared in <utime.h>, next to the
// functions that take them, so there is exactly one definition.
#define SYS_UTIME           387

#define SYS_FSYNC           358 // (#695) fsync(fd): commit this fd's buffered
                                // bytes to the medium WITHOUT consuming the fd
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
#define SYS_MPROTECT        23  // Change protection of a mapped range (#404)

// mprotect protection bits (POSIX values; kernel mm/demand.c decodes these).
#define PROT_NONE           0x0
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define PROT_EXEC           0x4

// mprotect refusal codes, mirrored from kernel/proc/syscall.h. Distinct so a
// caller can tell WHICH check refused it rather than only that it failed.
#define MP_OK               0
#define MP_E_PROT_BITS    (-1)
#define MP_E_LEN          (-2)
#define MP_E_ALIGN        (-3)
#define MP_E_WX           (-4)   // W^X: PROT_WRITE|PROT_EXEC is refused
#define MP_E_OVERFLOW     (-5)
#define MP_E_RANGE        (-6)
#define MP_E_NOMAP        (-7)

// Graphics/Window (for GUI apps)
#define SYS_WIN_CREATE      30  // Create window
// #148 (local 164, 2026-08-18): defined here (not down by the other #390s,
// where its number would suggest) because win_create_bg() below needs it -
// C macros are textual and only see #defines that appear earlier in the
// file. See kernel/proc/syscall.h SYS_WIN_CREATE_BG for the full contract.
#define SYS_WIN_CREATE_BG   393 // Create window WITHOUT taking keyboard focus
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
#define SYS_TIME            50  // #113: seconds since the UNIX EPOCH (UTC). Was seconds since BOOT.
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
// #745 (local 82): IMPLEMENTED in the kernel (proc/syscall.c glue, policy in
// rustkern/pgrp.rs). Before that these four were declared here, called by
// real wrappers in unistd.c, and defined NOWHERE in the kernel, so every one
// hit the dispatcher default and returned -1: setpgid() reported EPERM and
// getpgid() reported failure, which is why nothing in this tree has job
// control.
#define SYS_SETSID          95  // Create new session
#define SYS_SETPGID         96  // Set process group
#define SYS_GETPGID         97  // Get process group
#define SYS_GETSID          106 // Get session ID
#define SYS_WAITPID         98  // Wait for specific child
#define SYS_GETCWD          99  // Get current working directory
#define SYS_CHDIR           100 // Change directory
// 101 SYS_FSTAT: RECLAIMED AND REAL (#120). #745 deleted the unimplemented
// declaration that used to sit on this number and recorded the exact condition
// for taking it back: "if fstat ever needs to report a real st_mode/st_dev for
// a directory or a device ... the right change is a kernel fstat that
// sys/stat.c calls INSTEAD of the seek trick, not a second path alongside it."
// That is what happened. The SEEK-based version was not a working function, it
// was a fabrication - it hardcoded S_IFREG|0644 and returned 0 even for a
// closed fd - and it is GONE from the success path, kept only as the
// old-kernel -1 fallback. The kernel handler reuses the one per-backend fill
// SYS_STAT uses (proc/syscall.c sc_stat_fill), so there is still exactly one
// copy of the stat logic.
#define SYS_FSTAT          101  // Get file status by descriptor
// 102 SYS_GETDENTS: DELETED, NOT MISSING (#745 local 82). Directory reads go
// through SYS_OPEN + SYS_READDIR (19), which is what dirent.c's opendir/
// readdir/closedir already use and what the kernel already implements.
#define SYS_EXECVE          103 // Execute with argv and envp
// #745 (local 82): IMPLEMENTED in the kernel (rustkern/pollsys.rs), over the
// same file_poll() readiness primitive select() uses. See libc poll.h.
#define SYS_POLL            104 // Poll file descriptors
// 105 SYS_NANOSLEEP: DELETED, NOT MISSING (#745 local 82). nanosleep() is
// implemented in posixextra.c on top of SYS_SLEEP, at that syscall's real
// millisecond resolution. A kernel SYS_NANOSLEEP would not have been more
// precise: the scheduler's wake sweep is tick-granular (4ms at 250Hz), so
// sub-millisecond sleeps are not deliverable by anything below it either.

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
// #162 (2026-08-19): packed system volume state in ONE syscall.
//   bits 0..7 level | bit 8 muted | bits 16..31 seq | bits 32..47 keyseq
// Defined here next to its siblings (not down with the other 39x numbers)
// because vol_state() below needs it, and C macros are textual: an inline
// function only sees #defines that appear earlier in the file. Same reason
// SYS_WIN_CREATE_BG (393) is defined up next to SYS_WIN_CREATE (30).
#define SYS_VOL_STATE       395
#define SYS_GET_DISK_TOTAL  138 // Get disk total size in MB
#define SYS_GET_DISK_FREE   139 // Get disk free space in MB
#define SYS_SET_MOUSE_SPEED 140 // Set mouse sensitivity (1-10)
#define SYS_GET_MOUSE_SPEED 141 // Get mouse sensitivity (1-10)
// #236: gap, not the top of the range - see the block comment in
// kernel/proc/syscall.h next to this same number.
#define SYS_SET_DBLCLICK_MS 282 // Push Double-click Speed (ms) to gui/window.c
// #113: epoch MICROSECONDS (UTC). See kernel/cpu/wallclock.h. Zero args.
#define SYS_REALTIME_US     396
#define SYS_GET_RTC_TIME    142 // Get RTC time packed: (hour<<16)|(min<<8)|sec
#define SYS_GET_RTC_DATE    143 // Get RTC date packed: (year<<16)|(month<<8)|day
#define SYS_SET_RTC_TIME    144 // Set RTC time: packed (hour<<16)|(min<<8)|sec
#define SYS_SET_RTC_DATE    145 // Set RTC date: packed (year<<16)|(month<<8)|day
#define SYS_GET_NET_INFO    146 // Get network info into net_info_t buffer
#define SYS_NTP_SYNC        147 // Sync time from NTP (default server); 0=ok -1=fail
#define SYS_NTP_SYNC_SERVER 367 // #797 Sync from a CALLER-NAMED NTP server; 0=ok, else -SNTP_E_*
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

// (#404) mprotect(addr, len, prot). Returns 0, or a negative MP_E_* code.
// NOTE: this kernel refuses PROT_WRITE|PROT_EXEC together (W^X) with MP_E_WX,
// which is a deliberate divergence from POSIX. It also only covers ranges that
// are backed by a VMA, i.e. memory obtained from mmap(): the ELF image, the brk
// heap and the initial stack have no VMA in this kernel and return MP_E_NOMAP.
static inline int sys_mprotect(void *addr, unsigned long len, int prot) {
    return (int)syscall3(SYS_MPROTECT, (long)addr, (long)len, (long)prot);
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

// #695: fsync, not close, is the call that lets a program protect data. close()
// consumes the fd whether or not it reports an error, so a program that only
// checks close() has no handle left to retry with. See fsync() in stdlib.h.
static inline int sys_fsync(int fd) {
    return (int)syscall1(SYS_FSYNC, fd);
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

// #113: epoch MICROSECONDS (UTC), or 0 if the kernel has no calendar yet.
// This is what gettimeofday()/clock_gettime(CLOCK_REALTIME) are built on.
// It shares ONE anchor with SYS_TIME in the kernel, so a sys_time() and a
// sys_realtime_us() taken in the same instant agree about which second it is.
// TIMEZONE IS NOT APPLIED: this is UTC, per POSIX. Use libc/tz.c to present it.
static inline long long sys_realtime_us(void) {
    return (long long)syscall0(SYS_REALTIME_US);
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

// #148 (local 164, 2026-08-18): same contract as win_create(), but the new
// window does NOT take keyboard focus (kernel/proc/syscall.h SYS_WIN_CREATE_-
// BG - the window is created and shown, whatever currently has focus keeps
// it). For a window that must appear without disturbing what the user is
// typing into (the PrintScreen-opens-Snapshot-with-a-preview flow).
static inline int win_create_bg(const char *title, int x, int y, int width, int height) {
    return (int)syscall5(SYS_WIN_CREATE_BG, (long)title, x, y, width, height);
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
// #554: filesystem-aware permission/attribute info. See kernel proc/syscall.h
// and rustkern/fsperm.rs. Backs the Files Properties permissions tab and the
// details-view attribute columns/filtering, and (indirectly, via chmod()/
// chown()) the terminal chmod/chown commands.
#define SYS_FS_PERM_INFO    333
// #565: file-based theme loader. Parse a /THEMES/*.mtheme file and add/update
// it in the live theme table (no reboot needed). See kernel proc/syscall.h
// and userland/libc/gui_theme.h.
// NOTE: 334 is reserved by userland/libc/pkgsig.c's SYS_APP_VERIFY_SIG
// (#563 app/kernel signing-key split) - do not reuse 334 for anything else.
#define SYS_THEME_LOAD_FILE 335
#define SYS_THEME_METRIC    357 // (#711) mtheme v2 integer metric getter

// The GLOBAL UI SCALE FACTOR. MUST match kernel/proc/syscall.h exactly.
// See userland/libc/uiscale.h for the Ring 3 API and rustkern/uiscale.rs for
// the design.
#define SYS_UI_SCALE        409
#define UISC_GET     0
#define UISC_SET     1
#define UISC_AUTO    2
#define UISC_MAX     3
#define UISC_SRC     4
#define UISC_GEN     5
#define UISC_SAVE    6
#define UISC_LAPTOP  7
#define UISC_PX      8
#define UISC_UNPX    9
#define UISC_SPAN   10
#define UISC_NATIVE 11   // mark the CALLING process as thinking in real screen
                         // pixels (the compositor, and nothing else). Call it
                         // FIRST, before any SYS_FB_INFO: the framebuffer-owner
                         // backstop is not yet true at that point.
// Mirrors UI_SRC_* in kernel/gui/uiscale.h.
#define UI_SRC_DEFAULT 0
#define UI_SRC_AUTO    1
#define UI_SRC_CONFIG  2
#define UI_SRC_USER    3
#define UI_SRC_ESP     4   // pinned by /UISCALE.TXT on the FAT boot partition
// (themes ticket, 2026-08-07) how many fg/bg pairs the runtime contrast floor
// had to force-correct the last time this theme index was parsed. See
// kernel proc/syscall.h and userland/libc/gui_theme.c.
#define SYS_THEME_CONTRAST_CORRECTIONS 364
#define SYS_INST_ENUM                  365
#define SYS_INST_INSTALL               366
#define SYS_WM_FORCE_REDRAW_ALL 359 // (#704) () -> count; compositor-only, arms redraw_pending on every open app window on a theme change
// #566 secure session lock + autologin
// #745 lock REASONS; must match proc/syscall.h and rustkern/sessionid.rs.
// 0 is IDLE so a caller that passes nothing keeps the pre-#745 behaviour.
#define SESSION_LOCK_IDLE      0    // idle timer fired; autologin declines it
#define SESSION_LOCK_EXPLICIT  1    // the user asked; always honoured if unlockable
#define SYS_SESSION_LOCK       336  // (int reason) -> 0 locked / -1 declined
#define SYS_SESSION_UNLOCK     337  // (const char *user, const char *pass) -> 0 ok / -1 bad / -2 locked
#define SYS_SESSION_IS_LOCKED  338  // () -> 1 locked / 0 unlocked (display cache read)
#define SYS_USER_CREATE_PW     362  // (#745) (user, pass, uid, gid, home) -> uid / <0
#define SYS_SET_AUTOLOGIN      339  // (const char *user, const char *pass, int enable) -> 0/-1
#define SYS_GET_AUTOLOGIN      340  // (char *buf, int cap) -> len of configured user (0 = disabled)
#define SYS_AUTH_LOCKOUT       341  // (const char *user) -> seconds of lockout remaining (0 = none)
// #745 sign-in screen mode, the SECOND key of /CONFIG/LOGIN.CFG. That file
// is 0600 root:root (kernel/fs/perms.c), so a non-root process CANNOT read
// it - not because the parse is hard but because open() fails. That is the
// whole reason this is a syscall pair and not a file the lock screen opens.
// MIRRORED from kernel/proc/syscall.h; the same numbers appear in the kernel
// dispatch table and (for 373's two string arguments) in rustkern/argtab.rs.
#define SYS_SET_LOGIN_MODE     374  // (int mode, const char *user, const char *pass) -> 0/-1
#define SYS_GET_LOGIN_MODE     375  // () -> 0 list / 1 typed / <0 error
// The REAL display geometry, ((width << 16) | height), regardless of the
// caller's scale. SYS_FB_INFO deliberately answers a scale-transparent app in
// LOGICAL pixels, because that is the coordinate system it draws in - but
// Settings has to SHOW the user the resolution their panel is actually
// running at, and "1280 x 720" on a 1920x1080 display is a lie even though it
// is the right answer to a different question.
#define UISC_FBPHYS 12

// Control-method battery (#battmeter). MUST match kernel/proc/syscall.h
// exactly. See drivers/battery.h and rustkern/battery.rs for the design.
#define SYS_BATTERY    410
#define BATT_PRESENT   0   // -> 1 battery declared, 0 none, -1 could not ask
#define BATT_PCT       1   // -> 0-100, or -1 unknown
#define BATT_STATE     2   // -> BATT_ST_* below, or 0 (unknown)
#define BATT_MINUTES   3   // -> minutes remaining, or -1 unknown
#define BATT_GEN       4   // -> generation counter, bumped on every change
#define BATT_ST_UNKNOWN     0
#define BATT_ST_DISCHARGING 1
#define BATT_ST_CHARGING    2
#define BATT_ST_FULL        3

// (#wizflash) UNSCALED (1x, theme-file-native) counterpart of SYS_THEME_METRIC
// above. MUST match kernel/proc/syscall.h exactly. See that header's comment
// on SYS_THEME_METRIC_RAW for why a caller (gui_theme_win_preview()) needs the
// raw value rather than the pre-scaled one when it is about to draw through a
// window that will scale the result itself.
#define SYS_THEME_METRIC_RAW 412

// (#231r) THE 5-BAND GRAPHIC EQUALISER. MUST match kernel/proc/syscall.h
// exactly. See that header for the design and for why the number is 413.
//
// The faders in the tray Sound panel drive THIS, and this reaches real
// per-band biquad DSP in the kernel's PCM mixer. #231 deleted the previous
// EQ precisely because no such syscall existed, so nothing behind the faders
// was real; do not add a UI control here that does not end up in
// rustkern/pcmeq.rs.
#define SYS_AUDIO_EQ   413
#define AEQ_BANDS     0   // ()          -> number of bands (5)
#define AEQ_GET       1   // (band)      -> fader 0..100 (50 = flat), -1 bad band
#define AEQ_SET       2   // (band, pos) -> 0, or -1 bad band. pos clamps to 0..100
#define AEQ_FREQ      3   // (band)      -> centre/corner frequency in Hz
#define AEQ_DB10      4   // (band)      -> gain in TENTHS of a dB, signed
#define AEQ_ACTIVE    5   // ()          -> 1 if any band is off flat
#define AEQ_RESET     6   // ()          -> every band back to flat
#define AEQ_LOG       7   // ()          -> write the current EQ to /AUDIOLOG.TXT
#define AEQ_SELFTEST  8   // ()          -> boot spectral self-test mask, 0 = pass
#define AEQ_POS_FLAT  50
#define AEQ_RANGE_DB10 120


// ===========================================================================
// #229 FIRST-RUN (OOBE) STATE. Same argument as SYS_SET_LOGIN_MODE above, for
// the same directory: /CONFIG is root-owned 0711 because it holds SHADOW and
// the owner's API keys, so a Ring-3 wizard running as the session user cannot
// create a name in it. Measured on golden 2011, all four of the first-boot
// wizard's durable writes were refused, INCLUDING its "Skip to Desktop" escape
// hatch - which is what turned a failed setup into a machine with no way out.
//
// So the state is asked for, not written. The kernel owns the set of legal
// keys; there is no path from this call to any other name in that directory.
// See kernel/rustkern/firstrun.rs for the full reasoning, including why
// SETUPSKIP and SETUPNEW stopped being files at all (they are one-boot signals,
// and storing a one-boot signal on persistent media manufactured the same
// stale-marker bug twice, #136 and #203).
//
// MIRRORED from kernel/proc/syscall.h and kernel/rustkern/firstrun.rs. The
// first-boot wizard is a no_std Rust app that cannot include either, so it
// keeps a fifth copy in its own const block; firstrun_selftest_rs() asserts
// these numeric values on every boot for exactly that reason.
// ===========================================================================
#define SYS_FIRSTRUN           397  // (int op) -> see FR_* below

#define FR_MARK_DONE            0   // durable: /CONFIG/SETUPDONE. -> 0, or -2 if the machine has no account
#define FR_SKIP_SET             1   // per-boot: escaped to the desktop. -> 0, ALWAYS (it cannot fail)
#define FR_SKIP_GET             2   // per-boot: -> 1 if escaped this boot, else 0. Non-consuming
#define FR_SKIP_CLEAR           3   // per-boot: arming a fresh first run. -> 0
#define FR_HANDOVER_SET         4   // per-boot: the machine just changed hands. -> 0
#define FR_HANDOVER_TAKE        5   // per-boot: CONSUMING read of the above. -> 1 once, then 0
#define FR_BOOTSTRAP_QUERY      6   // #OOBEAUTH: -> 1 iff this caller may call SYS_FIRSTBOOT_ADMIN right now, else 0

// ===========================================================================
// #745 ELEVATION (kernel/proc/elevate.h). System-wide package installs.
//
// WHAT AN APP CAN DO WITH THIS, in full: ask (378), and read the verdict (379).
// It cannot draw the prompt, cannot see a keystroke while it is up, and cannot
// see the password, ever. 380 and 381 are the COMPOSITOR's half and are
// rejected for every other process by the framebuffer latch in the kernel.
//
// 382 answers one boolean about the CALLER's own account, so an app can show
// the "only an administrator can install for all users" state up front instead
// of letting the user start something that will be refused.
// ===========================================================================
#define SYS_ELEV_REQUEST   378
#define SYS_ELEV_STATUS    379
#define SYS_ELEV_VIEW      380
#define SYS_ELEV_RESOLVE   381
#define SYS_ELEV_MAY       382

#define ELEV_ST_IDLE     0
#define ELEV_ST_OPEN     1
#define ELEV_ST_GRANTED  2
#define ELEV_ST_DENIED   3

#define ELEV_EARG      (-1)
#define ELEV_EBUSY     (-2)   /* a prompt is already open: REFUSED, not queued */
#define ELEV_ESTALE    (-3)
#define ELEV_EPERM     (-4)   /* not in the admin set */
#define ELEV_EROOT     (-5)   /* uid 0: root is never prompted, just install */
#define ELEV_EATTEMPTS (-6)
#define ELEV_ELOCKED   (-7)
#define ELEV_ENOINPUT  (-8)   /* not raised in response to recent input */

#define ELEV_ACT_CANCEL   0
#define ELEV_ACT_SUBMIT   1
#define ELEV_ACT_LOCKSECS 2

/* App-supplied DISPLAY text. The kernel sanitises every field (control bytes
 * and non-ASCII dropped, truncated to 40 glyphs) and supplies the destination
 * itself, so nothing here decides where the privilege applies. */
typedef struct {
    char name[64];
    char version[32];
    char source[64];
} elev_request_t;

static inline long sys_elev_request(const elev_request_t *r) {
    return syscall1(SYS_ELEV_REQUEST, (long)r); }
static inline long sys_elev_status(unsigned long long seq) {
    return syscall1(SYS_ELEV_STATUS, (long)seq); }
static inline long sys_elev_view(void *out) {
    return syscall1(SYS_ELEV_VIEW, (long)out); }
static inline long sys_elev_resolve(unsigned long long seq, int action, const char *pw) {
    return syscall3(SYS_ELEV_RESOLVE, (long)seq, (long)action, (long)pw); }
static inline long sys_elev_may(void) {
    return syscall0(SYS_ELEV_MAY); }
// Mode encoding, mirrored from kernel/proc/syscall.h (locked there by
// _Static_assert) and from rustkern/loginmode.rs (locked there by the boot
// self-test). ANY error is treated as TYPED by every caller: showing every
// account name is a disclosure and must follow a recorded decision.
#define LOGIN_MODE_LIST        0
#define LOGIN_MODE_TYPED       1
#define SYS_WM_APPS_DIRTY      342  // () -> 1 if any KWM window changed (move/resize/focus/create/close/win_invalidate) since the last SYS_COMPOSITOR_RENDER_WINDOWS composite, else 0. #564 idle-CPU render gate.

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

// ---------------------------------------------------------------------------
// #554: filesystem-aware permission/attribute info (see docs/UI_STYLE_GUIDE.md
// / CHANGELOG #554 for the ext2-vs-FAT design decision). Byte-for-byte mirror
// of k_fsperm_info_t (kernel proc/syscall.c, private to that TU) and FsPermInfo
// (rustkern/fsperm.rs) - kept as an independent copy on purpose, the same
// convention the kernel already uses for k_stat_t / dirent_t vs their kernel
// twins (no shared header crosses the Ring 3/Ring 0 boundary in this tree).
// ---------------------------------------------------------------------------
#define FSPERM_TYPE_POSIX 0   // ext2/root path: uid/gid/mode via perms.c
#define FSPERM_TYPE_FAT    1   // genuine FAT (ESP: /boot, /EFI): fat_attr only
#define FSPERM_TYPE_OTHER  2   // SMB/NFS: no local permission model
// FAT attribute bits (mirrors kernel fs/fat.h FAT_ATTR_*).
#define FSPERM_FAT_READONLY  0x01
#define FSPERM_FAT_HIDDEN    0x02
#define FSPERM_FAT_SYSTEM    0x04
#define FSPERM_FAT_VOLUME_ID 0x08
#define FSPERM_FAT_DIRECTORY 0x10
#define FSPERM_FAT_ARCHIVE   0x20
typedef struct {
    unsigned char  fs_type;        // FSPERM_TYPE_*
    unsigned char  is_dir;
    unsigned char  has_perm_entry; // fs_type==POSIX only
    unsigned char  fat_attr;       // fs_type==FAT only
    unsigned short mode;           // fs_type==POSIX only: rwxrwxrwx bits
    unsigned short _reserved;
    unsigned int   uid;            // fs_type==POSIX only
    unsigned int   gid;            // fs_type==POSIX only
} fsperm_info_t;
static inline int sys_fs_perm_info(const char *path, fsperm_info_t *out) {
    return (int)syscall3(SYS_FS_PERM_INFO, (long)path, 0, (long)out);
}
// #565: parse a /THEMES/*.mtheme file and add/update it in the kernel's live
// theme table. Returns the resulting theme index (>=0), or -1 on failure.
static inline int theme_load_file(const char *path) {
    return (int)syscall1(SYS_THEME_LOAD_FILE, (long)path);
}
// (themes ticket) 0 = the theme parsed clean; >0 = this many fg/bg pairs
// needed forcing to black/white for readability. theme_id<0 = current theme.
static inline int theme_contrast_corrections(int theme_id) {
    return (int)syscall1(SYS_THEME_CONTRAST_CORRECTIONS, (long)theme_id);
}

// (#306) Install-to-disk. Mirrors kernel/gui/installer.h inst_target_t; the
// kernel-side _Static_assert locks the layout, and sys_inst_enum() rejects a
// mismatched size at runtime so a stale userland build cannot misread it.
#define INST_KIND_ATA   0
#define INST_KIND_AHCI  1
#define INST_KIND_USB   2
#define INST_MAX_TARGETS 16
typedef struct {
    unsigned char  kind;
    unsigned char  index;
    unsigned char  is_boot;
    unsigned char  _pad;
    unsigned long long sectors;
} inst_target_t;

// Fills out[] with up to max targets. Returns the count, or negative on error.
static inline int inst_enum(inst_target_t *out, int max) {
    return (int)syscall3(SYS_INST_ENUM, (long)out, (long)max, (long)sizeof(inst_target_t));
}

// DESTRUCTIVE: repartitions and overwrites the target disk. Root only.
// Identifies the disk by (kind,index) ONLY - the kernel re-enumerates and
// supplies capacity and boot-disk status itself, so this cannot be aimed at
// the running system by forging a descriptor. Blocks until the clone finishes.
static inline int inst_install(int kind, int index) {
    return (int)syscall2(SYS_INST_INSTALL, (long)kind, (long)index);
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

// #DOSRING3 Stage 1: drain RAW set-1 make/break scancodes for `handle`.
//
// For guests that read the keyboard hardware themselves rather than through a
// cooked event: a DOS program's own INT 9 handler reads port 0x60 and keeps its
// own Keyboard[] array (id Software's Galaxy engine does exactly this). The
// bytes include 0xE0 prefixes and break codes, which EVENT_KEY_DOWN cannot
// express - gui_event_t has no scancode field.
//
// SCOPED, and the scope is the point: the kernel delivers only while THIS
// process owns `handle` AND that window has focus, re-checked on every call.
// Returns bytes written (0 when not focused, which also drops the subscription
// and flushes the ring so keys typed elsewhere are never handed back), or -1 if
// the caller does not own the window. Call it unconditionally in your event
// loop; arming is idempotent and needs no setup call.
#define SYS_WIN_GET_SCANCODES 416
static inline int win_get_scancodes(int handle, unsigned char *buf, int cap) {
    return (int)syscall3(SYS_WIN_GET_SCANCODES, handle, (long)buf, cap);
}

// (#704) Compositor-only: force every open app window to repaint its content
// on the next event loop tick (not just recomposite stale pixels). Call this
// once, edge-triggered, when a theme change is detected (index switch or a
// live .mtheme file reload) - see sys_wm_force_redraw_all() in the kernel
// for why this is needed and why it is safe to call repeatedly.
static inline int wm_force_redraw_all(void) {
    return (int)syscall0(SYS_WM_FORCE_REDRAW_ALL);
}

// Get window content dimensions (for resize handling)
static inline int win_get_size(int handle, int *width, int *height) {
    return (int)syscall3(SYS_WIN_GET_SIZE, handle, (long)width, (long)height);
}

// #221: THIS WINDOW'S OWN STATE. Until this existed an app could not discover
// that it was minimized, and could not discover that it was unfocused either:
// the kernel emits no focus/blur/minimize event to an app (EVENT_WINDOW_FOCUS
// and EVENT_WINDOW_BLUR appear exactly once each in the kernel tree and it is
// the enum declaration), and although wm_get_windows() reports `minimized` per
// window, an app cannot find its own row - win_create() returns the
// user_windows[] slot index while wm_window_info_t.id is the window manager's
// window id, and nothing maps one to the other.
//
// Pass the handle win_create() returned. Returns the bitmask below, or -1 if
// the handle is not a live window. Cannot block; safe to call on a throttle
// from a draw loop (#426).
#define SYS_WIN_GET_STATE   407
#define WIN_STATE_VISIBLE   0x01
#define WIN_STATE_MINIMIZED 0x02
#define WIN_STATE_FOCUSED   0x04
#define WIN_STATE_MAXIMIZED 0x08
static inline int win_get_state(int handle) {
    return (int)syscall1(SYS_WIN_GET_STATE, (long)handle);
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
// perf62 (#62 revalidation): TSC-backed monotonic microseconds (cpu/mono.h
// mono_us(), NOT timer_ticks). SYS_UPTIME_MS/SYS_GET_TICKS are tick-derived,
// and blame.md's "timer-ticks-is-not-a-wall-clock" entry records that KVM
// replays a starved vCPU's lost tick IRQs in BURSTS, so a tick interval can
// misreport real elapsed time during exactly the kind of stall a frame-
// interval instrument needs to see honestly.
#define SYS_MONO_US         388
#define SYS_DECODE_IMAGE    253
#define SYS_WIN_DRAW_IMAGE  254
#define SYS_HTTP_FETCH_START    255
#define SYS_HTTP_FETCH_POLL     256
#define SYS_HTTP_FETCH_READ     257
#define SYS_HTTP_FETCH_CANCEL   258
#define SYS_HTTP_FETCH_PROGRESS 368  // (#25)
#define SYS_SET_SETTINGS_TAB 229
#define SYS_GET_SETTINGS_TAB 230
// #745 (local 102): display rotation. MIRRORS kernel/proc/syscall.h exactly;
// syscall-number-lint rule 3 checks the two agree.
#define SYS_SET_ROTATION     384
#define SYS_GET_ROTATION     385
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

// (#231r) 5-band graphic EQ. Helpers rather than raw syscall3() at every call
// site, for the same reason vol_state() has them just above: an opcode copied
// wrong at one of several call sites is the silent-wrong-value class of bug
// this project keeps finding.
//
// The fader unit is the SAME 0..100 the original panel's faders travelled,
// with 50 as the centre detent. The kernel owns the fader-to-dB mapping
// (rustkern/pcmeq.rs), so ask it with eq_band_db10() rather than
// re-deriving it here - two copies of that mapping is how a UI ends up
// labelling a gain it is not applying.
static inline int eq_band_count(void) {
    return (int)syscall1(SYS_AUDIO_EQ, AEQ_BANDS);
}
static inline int eq_band_get(int band) {
    return (int)syscall2(SYS_AUDIO_EQ, AEQ_GET, band);
}
static inline int eq_band_set(int band, int pos) {
    return (int)syscall3(SYS_AUDIO_EQ, AEQ_SET, band, pos);
}
static inline int eq_band_freq(int band) {
    return (int)syscall2(SYS_AUDIO_EQ, AEQ_FREQ, band);
}
static inline int eq_band_db10(int band) {
    return (int)syscall2(SYS_AUDIO_EQ, AEQ_DB10, band);
}
static inline int eq_is_active(void) {
    return (int)syscall1(SYS_AUDIO_EQ, AEQ_ACTIVE);
}
static inline int eq_reset(void) {
    return (int)syscall1(SYS_AUDIO_EQ, AEQ_RESET);
}
// Call ONCE when a fader drag ends, never during it: the kernel side rewrites
// the whole of /AUDIOLOG.TXT per call.
static inline void eq_log(void) {
    (void)syscall1(SYS_AUDIO_EQ, AEQ_LOG);
}
static inline unsigned eq_selftest_mask(void) {
    return (unsigned)syscall1(SYS_AUDIO_EQ, AEQ_SELFTEST);
}

// #162: one packed read of the system volume state. See SYS_VOL_STATE above.
// Helpers rather than raw bit-fiddling at every call site, because a shifted
// mask copied wrong at one of them is exactly the silent-wrong-value class of
// bug this project keeps finding.
static inline unsigned long long vol_state(void) {
    return (unsigned long long)syscall0(SYS_VOL_STATE);
}
static inline int vol_state_level(unsigned long long st) { return (int)(st & 0xFF); }
static inline int vol_state_muted(unsigned long long st) { return (int)((st >> 8) & 1); }
static inline unsigned vol_state_seq(unsigned long long st) {
    return (unsigned)((st >> 16) & 0xFFFF);
}
static inline unsigned vol_state_keyseq(unsigned long long st) {
    return (unsigned)((st >> 32) & 0xFFFF);
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

// #236: push the live Double-click Speed setting (ms) into the kernel's
// title-bar maximize/restore detector (gui/window.c). See the
// SYS_SET_DBLCLICK_MS block comment in kernel/proc/syscall.h. Kernel clamps
// to [100, 3000]; this never fails from userland's point of view.
static inline int sys_set_dblclick_ms(int ms) {
    return (int)syscall1(SYS_SET_DBLCLICK_MS, ms);
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
// #745 (local 102): persists the rotation CHOICE (0=none 1=90cw 2=180
// 3=270cw) to \ROTATE.TXT on the FAT ESP for the NEXT boot's fb_init() to
// read; it does not re-rotate the running session (see kernel/proc/syscall.c
// SYS_SET_ROTATION comment for why that is out of scope). Returns 0 on
// success, -1 if value is out of range or the ESP is not mounted.
static inline int set_display_rotation(int value) {
    return (int)syscall1(SYS_SET_ROTATION, value);
}
// The rotation ACTIVE for this already-running session (what fb_init() chose
// at boot from the persisted marker), 0..3. This can differ from the last
// value passed to set_display_rotation() until the next reboot.
static inline int get_display_rotation(void) {
    return (int)syscall0(SYS_GET_ROTATION);
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
// perf62: TSC-backed monotonic microseconds since boot. See SYS_MONO_US.
static inline unsigned long long mono_us(void) {
    return (unsigned long long)syscall0(SYS_MONO_US);
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

// --- TCP connection states (#640) -------------------------------------------
// MUST MATCH kernel/net/tcp.h tcp_state_t EXACTLY. tcp_get_state() returns one
// of these. Userland previously had the syscall and the wrapper but no names
// for the return values, so callers referenced kernel-only identifiers and did
// not compile. Kept as an enum (not #defines) so the compiler type-checks it.
#define MAYTERA_TCP_STATES 1
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

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

// NTP sync against the built-in default server: 0 on success, -1 on failure.
static inline long ntp_sync(void) {
    return syscall0(SYS_NTP_SYNC);
}

// #797 NTP sync against a CALLER-NAMED server (the first-boot wizard's
// "NTP server" field, and Settings). `server` may be a hostname or a
// dotted-quad; NULL or "" uses the default. `timeout_ms` of 0 uses the
// default 5s budget; it is capped in-kernel at 30s.
//
// Returns 0 on success. On failure it returns the NEGATIVE reason rather
// than a flat -1, so a caller can say something useful:
//   -20 no carrier      -21 no IP        -22 name did not resolve
//   -23 bind failed     -24 send failed  -25 no reply (timeout)
//   -26 busy            -27 self-test failed
//   -3..-9 the server answered but the reply FAILED VALIDATION
//          (-3 unsynchronised, -5 not a server reply, -6 bad stratum /
//           kiss-o'-death, -7 did not echo our request, -8 zero timestamp,
//           -9 date outside the sanity window)
static inline long ntp_sync_server(const char *server, unsigned int timeout_ms) {
    return syscall2(SYS_NTP_SYNC_SERVER, (long)server, (long)timeout_ms);
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
// (#flipfix) The monotonic count of framebuffer presents (kernel
// gui/fb_syscall.c g_fb_flip_count). Read-only, zero args. Use it to pace work
// against what the SCREEN is actually managing rather than against a constant:
// a value that has not moved since your last paint means your last paint has
// not been composited yet, so painting again only overwrites a frame nobody
// saw. First caller: the Ring-3 DOS host's frame gate (rustkern/dosdisp.rs).
#define SYS_FB_FLIP_COUNT       418
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
    // #745: APPENDED, never reordered. Mirrors kernel/gui/window.h, which pins
    // the same size with the same assert. sys_wm_get_windows() writes
    // sizeof(wm_window_info_t) per entry into THIS array, so a one-sided edit
    // is a buffer overrun, not a cosmetic mismatch. Non-zero = this window
    // asked for the compositor drop shadow (WINDOW_FLAG_SHADOW).
    int  shadow;
    // #41 (2026-08-12): stable app identity, the BINARY BASENAME the kernel
    // spawned this window's owning process from (e.g. "/APPS/PAINT" ->
    // "PAINT"), resolved kernel-side from window_t.owner_pid. NOT the
    // window's own title string. Empty ("") means no identity is available
    // (kernel-desktop-fallback window, or the owning process already
    // exited) - callers MUST fall back to another heuristic in that case,
    // never treat an empty app_id as a match.
    char app_id[32];
    // #44 (2026-08-12): APPENDED, never reordered - mirrors kernel/gui/window.h.
    // Non-zero = WINDOW_FLAG_MAXIMIZED. Lets a caller (taskbar.c's dock
    // context menu) label "Maximize" vs "Restore" correctly: the kernel's
    // SYS_WM_MAXIMIZE_WINDOW is a TOGGLE (wm_toggle_maximize_focused()), so
    // calling it on an already-maximized window restores it - without this
    // bit a menu could not tell which way the toggle was about to go.
    int  maximized;
} wm_window_info_t;
_Static_assert(sizeof(wm_window_info_t) == 136,
               "#745/#41/#44: wm_window_info_t layout is duplicated in kernel/gui/window.h; "
               "change both or neither");

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
// (#flipfix) Presents since boot, or a NEGATIVE value on a kernel that does not
// have the call. Callers must distinguish those two: a screen that is genuinely
// not flipping and a syscall that is not there both look like "no progress",
// and only one of them is a reason to stop drawing.
static inline long     fb_flip_count(void)    { return syscall0(SYS_FB_FLIP_COUNT); }
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
// #564: peek-only "did anything in the kernel WM change" for the compositor's
// render gate - see kernel/gui/window.h sys_wm_apps_dirty() for the rationale.
static inline int      wm_apps_dirty(void) {
    return (int)syscall0(SYS_WM_APPS_DIRTY); }

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

// #566 secure session lock + autologin (compositor lock overlay, Settings
// Users & Accounts panel). The kernel is the sole authority for lock state:
// these are thin syscall wrappers only, never a place to cache a decision.
// #745: takes a REASON. Returns 0 when the session is now locked, -1 when the
// kernel declined (autologin idle lock, or a session user with no usable
// password). The kernel is the authority either way: read sys_session_is_locked()
// for the truth, never assume this returning 0 means the overlay should show.
static inline int sys_session_lock(int reason) {
    return (int)syscall1(SYS_SESSION_LOCK, reason); }
// #745: create an account AND set its password in one call, or create nothing.
// Root only. `uid` 0 means "kernel allocates the next free human uid" and is
// what every caller should pass: computing a uid in userland is how Settings
// ended up colliding with an existing account. Returns the new uid, or <0.
static inline int sys_user_create_pw(const char *username, const char *password,
                                     int uid, int gid, const char *home) {
    return (int)syscall5(SYS_USER_CREATE_PW, (long)username, (long)password,
                         uid, gid, (long)home); }

// #745: pass "" as `user` to mean THE SESSION USER, which is what the lock
// screen should always do. The kernel already knows who the session is; a
// caller-supplied name can only agree with it or break unlocking, and passing a
// hardcoded "root" is exactly how the lock screen became unopenable at uid 1000.
static inline int sys_session_unlock(const char *user, const char *pass) {
    return (int)syscall2(SYS_SESSION_UNLOCK, (long)user, (long)pass); }
static inline int sys_session_is_locked(void) {
    return (int)syscall0(SYS_SESSION_IS_LOCKED); }
static inline int sys_set_autologin(const char *user, const char *pass, int enable) {
    return (int)syscall3(SYS_SET_AUTOLOGIN, (long)user, (long)pass, enable); }
static inline int sys_get_autologin(char *buf, int cap) {
    return (int)syscall2(SYS_GET_AUTOLOGIN, (long)buf, cap); }
static inline int sys_auth_lockout(const char *user) {
    return (int)syscall1(SYS_AUTH_LOCKOUT, (long)user); }
// #745. set: authorization is IDENTICAL to sys_set_autologin (root for
// anyone; non-root only for its own uid and only with that account's
// password). get: ungated, and it returns a MODE, never a list of names.
static inline int sys_set_login_mode(int mode, const char *user, const char *pass) {
    return (int)syscall3(SYS_SET_LOGIN_MODE, mode, (long)user, (long)pass); }
// Any negative return is folded to TYPED right here, so no caller has to
// remember to do it and none can get the safe direction wrong.
static inline int sys_get_login_mode(void) {
    int r = (int)syscall0(SYS_GET_LOGIN_MODE);
    return r == LOGIN_MODE_LIST ? LOGIN_MODE_LIST : LOGIN_MODE_TYPED; }

// #120 was defined but never wrapped (no callers anywhere in userland before
// #566): the Settings autologin UI needs to know whether the current process
// is root (root may set autologin for any account with no password; a
// non-root caller may only set it for their own account and must supply
// their password, per the kernel ABI comment on SYS_SET_AUTOLOGIN above).
static inline int sys_getuid(void) {
    return (int)syscall0(SYS_GETUID); }

// #229 first-run state. One wrapper, one op argument, no pointers.
//
// THE ESCAPE-HATCH CALLS CANNOT FAIL AND CALLERS MUST NOT TREAT THEM AS IF
// THEY COULD. FR_SKIP_SET/CLEAR and FR_HANDOVER_SET mutate a bit of kernel
// RAM and always return 0; the previous implementation of the same signals was
// a file write, and a caller that refused to proceed on a failed write is
// exactly how a failed setup became a machine with no way to the desktop.
// FR_MARK_DONE is the one op that can legitimately fail (no disk, or no
// account yet) and its callers do check it.
static inline int sys_firstrun(int op) {
    return (int)syscall1(SYS_FIRSTRUN, op); }

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

// ===========================================================================
// #250 REMOVABLE VOLUMES (USB hotplug).
//
// disk_info_t above describes FIXED ATA disks and has no mount point, no
// removable flag and no way to eject, so it cannot answer "what USB drive is
// plugged in right now". These do.
//
// vol_list() is cheap enough to call from a UI tick: it is one syscall that
// writes at most 8 records and touches no device. Compare the result with
// what you drew last time and only redraw on a change; do NOT poll it from a
// render path.
// ===========================================================================
#define SYS_VOL_LIST        283
#define SYS_VOL_EJECT       284

// MUST match sc_volume_t in kernel/proc/syscall.h (size locked there by
// _Static_assert) and ScVolume in kernel/rustkern/hotplug.rs.
typedef struct {
    int            index;          // opaque handle: pass to vol_eject()
    unsigned int   flags;          // MOSVOL_*
    unsigned int   fs_type;        // informational; display `fsname`
    unsigned int   pad;
    unsigned long long total_bytes;
    unsigned long long free_bytes; // meaningless when MOSVOL_FREE_UNKNOWN
    char           name[64];       // "SanDisk Cruzer Blade"
    char           mount[32];      // "/USB0" - a real, browsable path
    char           fsname[8];      // "FAT32", "exFAT", ...
} sc_volume_t;

#define MOSVOL_MOUNTED       0x01
#define MOSVOL_REMOVABLE     0x02
// Files on this volume can actually be opened and read. CLEAR means the
// volume is mounted and its size is known but its filesystem's file
// operations are not implemented (exFAT today). Do not offer it as
// browsable; say why instead.
#define MOSVOL_READABLE      0x04
// free_bytes is not meaningful. Show the total size only.
#define MOSVOL_FREE_UNKNOWN  0x08
// #234i. Writes to this volume are refused by the kernel. Set for every
// mounted disk image (fs/fat.c refuses fat_write on an image-backed handle),
// so a UI must not offer New / Rename / Delete / Paste there. Whether it is
// enforced is not the UI's business; whether it is OFFERED is.
#define MOSVOL_READONLY      0x10
// #234i. What the volume IS, for the icon and the label. Exactly one of these
// is set for a disk image and neither for a USB device.
#define MOSVOL_OPTICAL       0x20  // CD-ROM / ISO 9660 disc image
#define MOSVOL_FLOPPY        0x40  // floppy disk image

// #234i: see kernel/proc/syscall.h. Buffer this many records and you can never
// be truncated.
#define SC_VOL_MAX           16
#define SC_VOL_IMAGE_BASE    1000

// Returns the number of records written (0..max), or -1.
static inline int vol_list(sc_volume_t *buf, int max) {
    return (int)syscall2(SYS_VOL_LIST, (long)buf, (long)max); }
// Flush, unmount and tell the device it is safe to remove. 0 on success.
static inline int vol_eject(int index) {
    return (int)syscall1(SYS_VOL_EJECT, (long)index); }

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
// (#182) Drain the DOS guest's OPL2 register writes. See kernel/proc/syscall.h
// for the number choice and for why this call is deliberately NON-BLOCKING.
// Rule 3 of kernel/tools/syscall-number-lint requires this to agree with the
// kernel header, and a silent divergence would mean userland calling one
// syscall believing it is another.
#ifndef SYS_DOS_FM_EVENTS
#define SYS_DOS_FM_EVENTS   377
#endif

// (#fmbridge) SYS_DOS_FM_HOST - the producer door into the kernel's ONE OPL2
// event queue, for /APPS/DOSUSER (the Ring-3 DOS host). Its counterpart
// SYS_DOS_FM_EVENTS (377) is the consumer door /APPS/FMSYNTH already uses.
//
// Only the process that OPENs the queue may push to, close, self-test or launch
// against it; every other caller gets -13. That latch is not decoration: it is
// what stops any app injecting OPL2 register writes that the FM synthesiser
// renders on the machine's speakers.
//
// Scalars only, in and out. Success is >= 0, failure < 0.
#define SYS_DOS_FM_HOST         423
// The op selector, arg1. Mirrored in kernel/proc/syscall.h beside the same
// syscall number, where the full argument is written down.
#define DOS_FM_HOST_OPEN           0
#define DOS_FM_HOST_PUSH           1   // a=reg, b=val, c=t_us (monotonic)
#define DOS_FM_HOST_CLOSE          2
#define DOS_FM_HOST_ACTIVE         3
#define DOS_FM_HOST_STAT_PUSHED    4
#define DOS_FM_HOST_STAT_DROPPED   5
#define DOS_FM_HOST_STAT_PEAK      6
#define DOS_FM_HOST_STAT_USED      7
#define DOS_FM_HOST_CAPACITY       8
#define DOS_FM_HOST_SELFTEST       9
#define DOS_FM_HOST_LAUNCH        10
static inline long sys_dos_fm_host(int op, long a, long b, long c) {
    return syscall4(SYS_DOS_FM_HOST, (long)op, a, b, c); }

#ifndef SYS_AUDIO_PCM_OPEN
#define SYS_AUDIO_PCM_OPEN  315
#define SYS_AUDIO_PCM_WRITE 316
#define SYS_AUDIO_PCM_CLOSE 317
// (#181 Ring-3 audio) One scalar ctl call carrying the five per-stream
// accessors a real-time PCM producer needs, plus the device-present question.
#define SYS_AUDIO_PCM_CTL   417
#define AUDIO_PCM_CTL_CONSUMED      0
#define AUDIO_PCM_CTL_QUEUED        1
#define AUDIO_PCM_CTL_UNDERRUNS     2
#define AUDIO_PCM_CTL_WAIT_BELOW    3
#define AUDIO_PCM_CTL_WAIT_CONSUMED 4
#define AUDIO_PCM_CTL_AVAIL         5
#endif
static inline int sys_audio_pcm_open(unsigned rate, unsigned channels, unsigned format) {
    return (int)syscall3(SYS_AUDIO_PCM_OPEN, (long)rate, (long)channels, (long)format); }
static inline int sys_audio_pcm_write(int handle, const void *pcm, unsigned frames) {
    return (int)syscall3(SYS_AUDIO_PCM_WRITE, (long)handle, (long)pcm, (long)frames); }
static inline int sys_audio_pcm_close(int handle) {
    return (int)syscall1(SYS_AUDIO_PCM_CLOSE, (long)handle); }
static inline long sys_audio_pcm_ctl(int handle, unsigned op, unsigned a, unsigned b) {
    return (long)syscall4(SYS_AUDIO_PCM_CTL, (long)handle, (long)op, (long)a, (long)b); }
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

// #221 phase 0. The LIVE physical modifier bitmask, straight out of the state
// cpu/isr.c uses to fold case and to turn Ctrl+letter into a control
// character. Bits are kernel/drivers/keymod.h's KEY_MOD_*, re-spelled as
// GUI_MOD_* in gui_mods.h. Never fails, never blocks, no pointer arguments.
//
// DO NOT POLL THIS TO IMPLEMENT A SHORTCUT. It answers "is Shift down NOW",
// and an event you dequeued a moment ago was stamped by an earlier NOW. Track
// the modifier press/release events instead (gui_mods.h does), which are in
// the SAME ordered queue as the key they modify and therefore cannot race it.
// This call exists for ONE job: correcting a held bit that went stale because
// the release was delivered to a window that had taken focus. See
// gui_mods_resync().
#define SYS_KEY_MODS        400
static inline unsigned int sys_key_mods(void) {
    return (unsigned int)syscall0(SYS_KEY_MODS); }

static inline int sys_spawn(const char *path) {
    return (int)syscall1(SYS_SPAWN, (long)path); }

#define SYS_WM_MAXIMIZE_WINDOW 260
static inline int sys_wm_maximize_focused(void) { return (int)syscall0(SYS_WM_MAXIMIZE_WINDOW); }

// #158 NATIVE FULLSCREEN. See kernel/proc/syscall.h for the full contract.
#define SYS_WM_FULLSCREEN_ENTER  389  // caller's own focused window only
#define SYS_WM_FULLSCREEN_EXIT   390  // unconditional, safe from anywhere
#define SYS_WM_FULLSCREEN_RENDER 391  // compositor per-frame fast-path blit
#define SYS_WM_FULLSCREEN_STATUS 392  // watchdog probe: (id<<32)|commit_seq, or -1
static inline int sys_wm_fullscreen_enter(void) { return (int)syscall0(SYS_WM_FULLSCREEN_ENTER); }
static inline int sys_wm_fullscreen_exit(void)  { return (int)syscall0(SYS_WM_FULLSCREEN_EXIT); }
static inline int sys_wm_fullscreen_render(void){ return (int)syscall0(SYS_WM_FULLSCREEN_RENDER); }
static inline long long sys_wm_fullscreen_status(void) { return (long long)syscall0(SYS_WM_FULLSCREEN_STATUS); }

// (#745) Publish the desktop work area: px reserved at each screen edge by the
// ACTIVE dock style. Compositor-only (returns -1 for anyone else). The kernel
// window manager then uses ONE definition of the reachable area for initial
// placement, maximize, restore-from-minimize and the title-bar drag, so a
// window's header can never end up under a panel. Derive the arguments with
// taskbar_{left,top,right,bottom}_inset(); never pass literals.
#define SYS_WM_SET_WORK_AREA 373
static inline int sys_wm_set_work_area(int left, int top, int right, int bottom) {
    return (int)syscall4(SYS_WM_SET_WORK_AREA, (long)left, (long)top,
                         (long)right, (long)bottom);
}
// #185: borderless panel + OS-wide mouse wheel
#define SYS_WIN_SET_NOCHROME 262
static inline int win_set_nochrome(int handle) { return (int)syscall1(SYS_WIN_SET_NOCHROME, (long)handle); }
#define SYS_WIN_SET_NOCHROME_BG 398
static inline int win_set_nochrome_bg(int handle) { return (int)syscall1(SYS_WIN_SET_NOCHROME_BG, (long)handle); }  // #216: nochrome, no focus grab
#define SYS_WIN_SET_ALPHA_CONTENT 414
// #198v2: mark a window's content_buffer as carrying REAL per-pixel alpha
// (top byte of every BGRA word) that the compositor blends against the live
// framebuffer at blit time, instead of ignoring that byte. See
// WINDOW_FLAG_ALPHA_CONTENT (kernel/gui/window.h) for the full contract.
// Fill the areas you want fully transparent with alpha 0 (e.g. a
// win_draw_rect() of 0x00000000) before drawing any real content - a
// freshly created window's buffer starts OPAQUE (0xFFF5F5F5).
static inline int win_set_alpha_content(int handle) { return (int)syscall1(SYS_WIN_SET_ALPHA_CONTENT, (long)handle); }
// #745: opt in to the compositor-drawn soft drop shadow around this window.
// One-way; see kernel/gui/window.h WINDOW_FLAG_SHADOW for why it is opt-in.
#define SYS_WIN_SET_SHADOW   376
static inline int win_set_shadow(int handle) { return (int)syscall1(SYS_WIN_SET_SHADOW, (long)handle); }
#define SYS_GET_MOUSE_SCROLL 263
static inline int get_mouse_scroll(void) { return (int)syscall0(SYS_GET_MOUSE_SCROLL); }
#define SYS_GET_GLOBAL_MOUSE 264  // #185: read-only global cursor for any process
static inline int get_global_mouse(int *x, int *y, unsigned int *buttons) {
    return (int)syscall3(SYS_GET_GLOBAL_MOUSE, (long)x, (long)y, (long)buttons); }
#define MOUSE_EVENT_SCROLL 3


#define SYS_RUN_NEXT_ON_AP 261
static inline int sys_run_next_on_ap(void) { return (int)syscall0(SYS_RUN_NEXT_ON_AP); }

// ---- #affinity: per-process CPU affinity ---------------------------------
// A SOFT preference, not a pin: the scheduler drops the mask rather than
// strand a task on a core that is not running work. pid 0 means "me". A mask
// of 0 is refused (it would be a request to hang). Setting another process's
// affinity requires root.
#define SYS_SET_AFFINITY   420
#define SYS_GET_AFFINITY   421
#define SYS_GET_MIGRATIONS 422
static inline int sched_setaffinity_maytera(int pid, unsigned long long mask) {
    return (int)syscall2(SYS_SET_AFFINITY, (long)pid, (long)mask);
}
static inline long long sched_getaffinity_maytera(int pid) {
    return (long long)syscall1(SYS_GET_AFFINITY, (long)pid);
}
// Migrations and switch-ins for pid, the pair that says whether an affinity
// change achieved anything. A migration count without its switch-in
// denominator cannot be compared between two runs of different length.
static inline int sched_getmigrations(int pid, unsigned long long *migrations,
                                      unsigned long long *switchins) {
    return (int)syscall3(SYS_GET_MIGRATIONS, (long)pid, (long)migrations,
                         (long)switchins);
}
static inline int sys_inject_key(int key) {
    return (int)syscall1(SYS_INJECT_KEY, (long)key); }
#define SYS_SPAWN_ARGS      198

// #112: THESE TWO NOW CARRY THE CALLER'S ENVIRONMENT.
//
// They used to be raw syscall3/syscall6 to 198/247, which carry argv and
// nothing else, so every child started with an empty `environ` no matter what
// its parent had exported. They route through __spawn_with_env() (stdlib.c)
// instead, which fills an sc_spawn_req_t with `environ` and issues 394.
//
// Done HERE rather than at the call sites on purpose: roughly a hundred places
// in this tree call these two, and a facility that each caller has to opt into
// is a facility most callers never get. The two raw numbers are still defined
// below for anything that deliberately wants the no-environment shape.
int __spawn_with_env(const char *path, char **argv, int argc,
                     const char *infile, const char *outfile, int append);

static inline int sys_spawn_args(const char *path, char **argv, int argc) {
    return __spawn_with_env(path, argv, argc, 0, 0, 0);
}

// The pre-#112 spellings, kept because "spawn with NO environment" has to stay
// expressible and because the syscall numbers are part of the ABI.
static inline int sys_spawn_args_noenv(const char *path, char **argv, int argc) {
    return (int)syscall3(SYS_SPAWN_ARGS, (long)path, (long)argv, (long)argc);
}

#define SYS_SPAWN_REDIR     247
// Spawn a child with stdin/stdout redirected to files (shell I/O redirection).
// infile/outfile may be NULL; append != 0 opens the output file for append.
static inline int sys_spawn_redir(const char *path, char **argv, int argc,
                                  const char *infile, const char *outfile, int append) {
    return __spawn_with_env(path, argv, argc, infile, outfile, append);
}

static inline int sys_spawn_redir_noenv(const char *path, char **argv, int argc,
                                        const char *infile, const char *outfile, int append) {
    return (int)syscall6(SYS_SPAWN_REDIR, (long)path, (long)argv, (long)argc,
                         (long)infile, (long)outfile, (long)append);
}

// ---------------------------------------------------------------------------
// #112: SPAWN WITH AN ENVIRONMENT.
//
// SYS_SPAWN_REDIR above already uses all six argument registers, so an
// environment cannot be a seventh operand: 394 takes ONE pointer to this
// fixed-size, version-locked request struct instead. The layout MUST match
// sc_spawn_req_t in kernel/proc/syscall.h byte for byte; a _Static_assert
// there locks its size to the 56 that rustkern/argtab.rs validates.
//
// envc IS NOT A COUNT WITH A CONVENIENT SENTINEL, it is three different
// requests, and posix_spawn() in spawn.c depends on the distinction:
//   envc <  0   no environment supplied. The child gets the kernel default
//               block (PATH/SHELL/TERM), which is also what the two older
//               spawn syscalls give it.
//   envc == 0   an EMPTY environment, on purpose. This is `env -i`.
//   envc >  0   envp points at envc "NAME=VALUE" strings.
//
// An entry that is not NAME=VALUE, or one longer than 511 bytes, REFUSES the
// spawn (returns -1). It is never truncated and never dropped: a half PATH
// still looks like a PATH to whatever reads it.
// ---------------------------------------------------------------------------
#define SYS_SPAWN_ENV       394
typedef struct {
    const char  *path;
    char       **argv;
    int          argc;
    int          envc;
    char       **envp;
    const char  *infile;
    const char  *outfile;
    int          append;
    int          reserved;   // MUST be 0
} sc_spawn_req_t;

static inline int sys_spawn_env(const sc_spawn_req_t *req) {
    return (int)syscall1(SYS_SPAWN_ENV, (long)req);
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
// (#845) Per-app real/protected mode request, second argument to
// win16_run_mode(). AUTO lets the kernel derive the mode from the NE header
// (win16_decide_pmode() in kernel/exec/ne.c); the FORCE_* values are an
// explicit override that wins over the derived answer either way.
#define WIN16_MODE_AUTO       (-1)
#define WIN16_MODE_FORCE_REAL 0
#define WIN16_MODE_FORCE_PM   1
// Run a Win16 (NE) executable by path, requesting a specific execution mode
// (see WIN16_MODE_* above). NON-BLOCKING: the kernel arms the guest's
// identity, spawns a dedicated proc + host window and returns immediately, 0
// on a successful launch and <0 on error (an already-running Win16 guest, or
// no usable identity).
// (This comment previously said it "blocks until the app window closes".
// win16_launch() in kernel/proc/syscall.c has never done that - it returns
// straight after proc_create(). A caller that believed the old text would have
// designed around a wait that does not happen.)
static inline int win16_run_mode(const char *path, int mode) {
    return (int)syscall2(SYS_WIN16_RUN, (long)path, (long)mode);
}
// Same as win16_run_mode(path, WIN16_MODE_AUTO): let the kernel derive the
// mode from the NE header. Used by the Terminal (when a named file turns out
// to carry an NE header) and the AI tool-contract launcher, neither of which
// has an opinion on mode; the Start menu uses win16_run_mode() directly with
// whatever its .MENU fragment or /WIN16GRP.CFG entry configured.
static inline int win16_run(const char *path) {
    return win16_run_mode(path, WIN16_MODE_AUTO);
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

// ============================================================================
// (#745) STRUCTURED network status + non-blocking probe.
//
// WHY THIS AND NOT SYS_NET_INFO (243): 243 returns a VERBOSE HUMAN REPORT
// ("  Netmask:       255.255.255.0\n"). Recovering addresses from it means an
// app string-matches kernel prose, which breaks SILENTLY the first time a
// label is reworded: the parse yields nothing, the app renders a page that
// looks fine and says nothing, and no compiler or linker notices. A struct of
// integers cannot fail that way - the _Static_assert below goes RED at build
// time if the layout ever drifts.
//
// WHY NOT SYS_GET_NET_INFO (146): every field there is a pre-formatted string
// and it carries no link state, no DHCP state and no fault flag, so it cannot
// tell "no address yet" from "address but no route". Widening it was rejected
// as an ABI break for a published layout with live consumers.
//
// ALL ADDRESS FIELDS ARE HOST BYTE ORDER: (a<<24)|(b<<16)|(c<<8)|d for
// a.b.c.d. Nothing here byte-swaps. 0 means NOT CONFIGURED, which is a real
// state to render honestly, never a blank.
// ============================================================================
#define SYS_NET_STATUS 371
#define SYS_NET_PROBE  372

// Mirror of net_status_t in kernel/proc/syscall.h and NetStatus in
// kernel/rustkern/netstat.rs. Keep all three in step.
typedef struct {
    unsigned int ip;             // live source address; 0 = none
    unsigned int netmask;
    unsigned int gateway;        // 0 = no default route
    unsigned int dns_active;     // resolver the stack WILL query
    unsigned int dns_dhcp;       // what DHCP offered; 0 = it offered none
    unsigned int dhcp_ip;        // leased address (0 while a DORA is in flight)
    unsigned int link_up;        // carrier, 0/1
    unsigned int dhcp_state;     // 0 idle 1 discovering 2 requesting 3 bound
    unsigned int config_static;  // config came from /CONFIG/NETIP.CFG
    unsigned int faulty;         // #549 persistently unreachable
    unsigned int driver;         // 0 = NO NIC AT ALL (distinct from no carrier)
    unsigned int prefix_len;     // popcount(netmask)
} net_status_t;
_Static_assert(sizeof(net_status_t) == 48,
               "net_status_t sizeof lock: must match kernel/proc/syscall.h and "
               "SZ_NET_STATUS in kernel/rustkern/argtab.rs");

// DHCP state values (kernel/net/dhcp.h), mirrored so a caller can name them.
#define NET_DHCP_IDLE         0
#define NET_DHCP_DISCOVERING  1
#define NET_DHCP_REQUESTING   2
#define NET_DHCP_BOUND        3

// SYS_NET_PROBE ops. Mirrored in kernel/proc/syscall.h and rustkern/netstat.rs.
#define NET_PROBE_PING_START    0   // arg = destination IPv4, HOST order
#define NET_PROBE_PING_POLL     1   // -> rtt ms (>=0), or a NET_PROBE_E* code
#define NET_PROBE_PING_CANCEL   2
#define NET_PROBE_DHCP_RESTART  3   // non-blocking dhcp_reset + dhcp_discover

#define NET_PROBE_PENDING      (-1) // no answer yet; poll again
#define NET_PROBE_EINVAL       (-2)
#define NET_PROBE_ENOTSTARTED  (-3)
#define NET_PROBE_ELINK        (-4) // no carrier: instant no-op (#381)

// ============================================================================
// #238 THE PACKET FILTER.
//
//   sys_net_fw(op, &xfer) -> 0, or negative
//
// The KERNEL owns the firewall policy AND the file it lives in
// (/CONFIG/FWRULES.CFG). Userland does not parse that file and must not write
// it: there is exactly one parser and one serialiser, both in
// kernel/rustkern/fwfilter.rs. An app that wants to show or change the policy
// reads it here and writes it here. That is the whole point of #238 - the
// previous arrangement was a Settings panel writing a format with no reader,
// over a capability the kernel did not have.
//
// EVERY OP FILLS THE WHOLE STRUCT FROM THE LIVE KERNEL STATE ON THE WAY OUT.
// So after a set, `xfer.cfg` is what is ACTUALLY in force, which is not
// necessarily what you asked for: an invalid ruleset is refused whole and the
// previous policy stays. Compare what you sent with what came back; do not
// assume.
//
// PORT SEMANTICS: a rule's port matches the DESTINATION port of the packet
// that OPENS a flow. `in tcp 22` = "permit connections to my port 22";
// `out tcp 443` = "permit me to connect to a remote port 443". Replies on a
// flow the filter already allowed are not re-evaluated, which is why a
// default inbound policy of DENY does not break outbound browsing.
//
// NOT FILTERED, and the UI must say so: ARP, ICMP and every IP protocol that
// is not TCP or UDP (the rule model cannot express them), the DHCP client
// exchange, and loopback.
// ============================================================================
#define SYS_NET_FW 399

#define FW_OP_GET     0   // read policy + counters
#define FW_OP_SET     1   // install cfg, persist it, read back
#define FW_OP_RESET   2   // zero the counters, read back
#define FW_OP_RELOAD  3   // re-read /CONFIG/FWRULES.CFG, install, read back

#define FW_MAX_RULES  12
#define FW_CFG_VERSION 1
#define FW_DIR_IN     0
#define FW_DIR_OUT    1
#define FW_ACT_ALLOW  0
#define FW_ACT_DENY   1
#define FW_PROTO_TCP  0
#define FW_PROTO_UDP  1

// Mirror of fw_rule_t/fw_config_t/fw_stats_t/fw_xfer_t in kernel/net/firewall.h
// and FwRule/FwConfig/FwStats/FwXfer in kernel/rustkern/fwfilter.rs. Four
// copies of one layout; all four are sizeof-locked.
typedef struct {
    unsigned char  dir;        // FW_DIR_*
    unsigned char  action;     // FW_ACT_*
    unsigned char  proto;      // FW_PROTO_*
    unsigned char  reserved;   // must be 0
    unsigned short port;       // 1..65535
    unsigned short reserved2;  // must be 0
} fw_rule_t;

typedef struct {
    unsigned int  version;     // FW_CFG_VERSION
    unsigned char enabled;
    unsigned char pol_in;      // FW_ACT_*
    unsigned char pol_out;     // FW_ACT_*
    unsigned char rule_count;  // 0..FW_MAX_RULES
    fw_rule_t     rules[FW_MAX_RULES];
} fw_config_t;

typedef struct {
    unsigned long long calls;       // filter invocations
    unsigned long long ct_hit;      // matched a tracked flow, skipped the rules
    unsigned long long new_in, new_out;      // flow-opening packets evaluated
    unsigned long long pass_in, pass_out;
    unsigned long long drop_in, drop_out;    // THE proof the filter does anything
    unsigned long long exempt;      // DHCP client / loopback
    unsigned long long unfiltered;  // protocol the rule model cannot express
    unsigned long long malformed;   // L4 header too short -> dropped
    unsigned long long frag;        // non-first fragment -> dropped
    unsigned long long ct_evict;
    unsigned long long cyc_tot, cyc_max;     // TSC cycles inside the filter
    unsigned int ct_used;
    unsigned int enabled;
    unsigned int rule_count;
    unsigned int pol;               // pol_in | (pol_out << 8)
} fw_stats_t;

typedef struct {
    fw_config_t cfg;
    fw_stats_t  stats;
} fw_xfer_t;

_Static_assert(sizeof(fw_rule_t) == 8,
               "fw_rule_t sizeof lock: must match kernel/net/firewall.h and "
               "FwRule in kernel/rustkern/fwfilter.rs");
_Static_assert(sizeof(fw_config_t) == 104,
               "fw_config_t sizeof lock: must match kernel/net/firewall.h and "
               "FwConfig in kernel/rustkern/fwfilter.rs");
_Static_assert(sizeof(fw_stats_t) == 136,
               "fw_stats_t sizeof lock: must match kernel/net/firewall.h and "
               "FwStats in kernel/rustkern/fwfilter.rs");
_Static_assert(sizeof(fw_xfer_t) == 240,
               "fw_xfer_t sizeof lock: must match kernel/net/firewall.h and "
               "SZ_FW_XFER in kernel/rustkern/argtab.rs");

// Never blocks. Mutating ops (SET/RESET/RELOAD) require euid 0 and return -1
// otherwise; FW_OP_GET is unprivileged.
//   0    ok
//  -1    refused (not root, bad op, null pointer, reload found nothing usable)
//  -5    SET: the policy IS installed but could not be written to disk, so it
//        will not survive a reboot. Distinct from -22 on purpose.
// -14    bad user pointer
// -22    SET: the ruleset failed validation. NOTHING changed; the previous
//        policy is still in force.
static inline int sys_net_fw(int op, fw_xfer_t *xfer) {
    return (int)syscall2(SYS_NET_FW, op, (long)xfer);
}

// Returns 0 on success. Never blocks.
static inline int sys_net_status(net_status_t *out) {
    return (int)syscall1(SYS_NET_STATUS, (long)out);
}

// ============================================================================
// (#786) Change the LIVE DNS resolver and persist it, in one call.
//
// There was NO way to do this from Ring 3 at all. net_set_static() (217) takes
// ip/mask/gw only, so Settings' "DNS Server" field went nowhere: the app copied
// the typed text into its own display variable and the resolver never changed.
// Proven on VM <vmid> / build 2054 - after a real OK click, [NETDIAG] still read
// dns=8.8.8.8 and a host packet capture showed a fresh hostname resolved
// through the old server.
//
// The kernel does the persisting (kernel/net/net.c net_persist_netcfg), NOT
// this app: /CONFIG is root-owned 0711, so a Ring-3 write of
// /CONFIG/NETIP.CFG is refused for every ordinary user, silently, and the
// app's own failure log lives under "/" and was refused too.
//
//   ->  0  applied live AND saved
//   -> -2  not a usable resolver address
//   -> -3  applied LIVE but NOT saved: it will revert on reboot. Say so.
#define SYS_NET_SET_DNS 408

#define NET_SET_DNS_OK        0
#define NET_SET_DNS_EINVAL  (-2)
#define NET_SET_DNS_EPERSIST (-3)

// Parse "a.b.c.d" into the HOST-ORDER u32 the syscall takes. Returns 0 for
// anything that is not four decimal octets, and 0 is itself rejected by the
// kernel, so a malformed string cannot be mistaken for a valid address.
static inline unsigned int net_parse_ipv4(const char *s) {
    unsigned int parts[4] = {0, 0, 0, 0};
    int pi = 0, val = 0, digits = 0;
    if (!s) return 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*s == '.' || *s == '\0') {
            if (digits == 0 || pi > 3) return 0;
            parts[pi++] = (unsigned int)val;
            val = 0; digits = 0;
            if (*s == '\0') break;
        } else {
            return 0;
        }
    }
    if (pi != 4) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

// Convenience: takes the dotted-quad the user typed. Returns NET_SET_DNS_*.
static inline int net_set_dns(const char *dotted) {
    unsigned int v = net_parse_ipv4(dotted);
    if (v == 0) return NET_SET_DNS_EINVAL;
    return (int)syscall1(SYS_NET_SET_DNS, (long)v);
}

// NEVER BLOCKS, by design. Unlike sys_ping() (66), which sleeps the caller for
// up to timeout_ms in two hand-rolled poll loops, this returns immediately:
// START puts one echo request on the wire, each POLL drains the RX ring and
// answers "rtt" or NET_PROBE_PENDING. THE DEADLINE IS THE CALLER'S, which is
// correct - only the caller knows how long its user will wait - and it means
// a GUI can run a reachability test from its own event loop without a freeze.
static inline long sys_net_probe(int op, unsigned long arg) {
    return (long)syscall2(SYS_NET_PROBE, (long)op, (long)arg);
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

// #549: start-side refusal code shared by sys_http_fetch_start() and
// sys_http_post_start(). MIRRORS NET_ERR_FAULTY in kernel/proc/syscall.h.
// A global failure-streak breaker can latch the interface FAULTY and refuse
// every fetch BEFORE it reaches the wire; that is reported as this distinct
// value rather than the generic -1 so a caller can tell "temporarily refused,
// try again" from "this request is bad". It is RETRYABLE: the breaker clears
// itself on the first fetch that completes, so treating it as fatal keeps the
// system in the faulty state forever (see blame.md, "A safety gate that
// suppresses its own recovery evidence").
#define NET_ERR_FAULTY (-3)

// #745: a POST REFUSED by the kernel's prompt-injection screen. MIRRORS
// NET_ERR_AIGUARD in kernel/proc/syscall.h. Distinct from -1 (request failed)
// and -3 (circuit breaker) on purpose: this one is NOT a network problem and
// NOT retryable, and reporting it as "network error" would both lie to the user
// and hide a security event. Call SYS_AI_SCAN (libc/aiguard.h) on the text you
// were about to send to find out WHICH rule refused it.
#define NET_ERR_AIGUARD (-4)

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
// #25: real fetch progress (phase + bytes_recv + content_len). Any out
// pointer may be NULL if the caller does not want that field. Returns 0 on
// success, -1 if `id` is not a live job.
static inline int http_fetch_progress(int id, int *phase, unsigned int *bytes_recv,
                                      unsigned int *content_len) {
    return (int)syscall4(SYS_HTTP_FETCH_PROGRESS, id, (long)phase,
                         (long)bytes_recv, (long)content_len);
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
// #745 SYS_AI_SCAN. Kept beside the POST numbers on purpose: the enforcement
// that matters happens inside SYS_HTTP_POST_START, and this is the read-only
// companion that lets a client NAME what was refused. MIRRORS the kernel
// header; syscall-number-lint rule 3 checks the two agree.
#define SYS_AI_SCAN          383

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
    unsigned int       flags;       // #145: PROC_INFO_F_*
} proc_info_t;

// #145: this row is a per-core IDLE process. Idle is CPU capacity nothing asked
// for, so it must never appear in, or win, a ranked "what is eating the CPU"
// list. It must however stay in the DENOMINATOR of a per-process share, because
// that denominator is what makes a percentage mean "share of total CPU
// capacity" and keeps it consistent with sys_get_cpu_usage(). Drop idle from
// the denominator too and a compositor using 1% of a quiet machine reads 60%.
// Kernel-authoritative bit; never match on the NAME "idle".
#define PROC_INFO_F_IDLE   0x00000001u
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
// (#785) Enable/disable, which unlike start/stop is DURABLE: it rewrites the
// service's own config file so the state survives a reboot. svc_enable()
// returns non-zero when it could not make the change durable, and this verb
// passes that straight through rather than flattening it to success.
#define PI_SVC_DISABLE 2
#define PI_SVC_ENABLE  3
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


// ===========================================================================
// #739 DISK IMAGES: mount an .iso as a CD-ROM drive or an .img as a floppy,
// eject, and read back what is on every drive letter.
//
// Drive letters are INDICES here, 0 = A: .. 25 = Z:. The kernel decides which
// letter an image lands on (rustkern/drvmap.rs): A:/B: are floppies, C: is the
// fixed disk and is never mountable, D: is reserved, E:..Z: are CD-ROMs handed
// out lowest-free-first. Pass DISKIMG_LETTER_AUTO to let it choose, which is
// what a UI should normally do; pass a specific letter to SWAP the disc in a
// drive that is already occupied, which keeps the letter (a swap that moved
// the disc to another letter would break the program that asked for it).
// ===========================================================================
#define SYS_DISKIMG         361

#define DISKIMG_CMD_INFO        0   // (letter) -> *out
#define DISKIMG_CMD_MOUNT       1   // (path, letter or AUTO) -> letter index
#define DISKIMG_CMD_EJECT       2   // (letter) -> 0
#define DISKIMG_CMD_MAX_MOUNTS  3   // () -> how many images may be mounted at once
#define DISKIMG_CMD_VOLINFO     4   // (letter) -> *out, mediated + capability-filtered

#define DISKIMG_LETTER_AUTO  (-1)

// Image formats (DISKIMG_FMT_* in kernel/dos/diskimg.h).
#define DISKIMG_FMT_NONE     0
#define DISKIMG_FMT_ISO9660  1
#define DISKIMG_FMT_FAT12    2

// Drive classes (DRV_CLASS_* in kernel/rustkern/drvmap.rs).
#define DISKIMG_CLASS_NONE   0
#define DISKIMG_CLASS_FLOPPY 1
#define DISKIMG_CLASS_FIXED  2
#define DISKIMG_CLASS_CDROM  3

#define DISKIMG_F_MOUNTED   0x01u
#define DISKIMG_F_JOLIET    0x02u
#define DISKIMG_F_READONLY  0x04u
#define DISKIMG_F_INUSE     0x08u   // a read is in flight on this image right now
#define DISKIMG_F_MOUNTABLE 0x10u   // an image MAY be mounted on this letter

// Mirrors diskimg_info_t in kernel/dos/diskimg.h. The kernel _Static_asserts
// its size at 288 against rustkern/argtab.rs; keep the two in step.
typedef struct {
    unsigned int   letter;      // 0 = A: .. 25 = Z:
    unsigned int   cls;         // DISKIMG_CLASS_*
    unsigned int   fmt;         // DISKIMG_FMT_*
    unsigned int   flags;       // DISKIMG_F_*
    unsigned int   gen;         // mount generation, bumped on every mount/eject
    unsigned int   readers;     // in-flight reads
    unsigned long long size;    // image bytes, 0 if nothing mounted
    char           name[64];    // image basename, "" if nothing mounted
    char           path[192];   // full image path, "" if nothing mounted
} diskimg_info_t;

// Read one drive letter's state. Succeeds for an EMPTY letter too: a UI has to
// show the drives that have no disc in them. Returns 0, or negative.
static inline int sys_diskimg_info(int letter, diskimg_info_t *out) {
    return (int)syscall4(SYS_DISKIMG, DISKIMG_CMD_INFO, 0, (long)out, letter);
}

// Mount `path` on `letter` (or DISKIMG_LETTER_AUTO). Returns the letter index
// used (>= 0), or a negative error:
//   -1 bad arguments        -2 not an ISO and not a FAT12 floppy
//   -3 that letter cannot hold that kind of image
//   -4 no free letter of that class      -5 too many images already mounted
//   -6 the path was refused (not absolute, contains "..", or has bad characters)
//  -10 cannot open the file  -11 unrecognised format  -12 too large for a floppy
//  -13 out of memory         -13 (EACCES is -13 in errno terms; the kernel
//      returns -MOS_EACCES for "you may not read that file")
static inline int sys_diskimg_mount(const char *path, int letter) {
    return (int)syscall4(SYS_DISKIMG, DISKIMG_CMD_MOUNT, (long)path, 0, letter);
}

// Eject whatever is on `letter`. Always succeeds if something was mounted.
// Handles a guest still holds are INVALIDATED, not redirected: the next read on
// one fails rather than returning bytes off whatever disc is mounted next.
static inline int sys_diskimg_eject(int letter) {
    return (int)syscall4(SYS_DISKIMG, DISKIMG_CMD_EJECT, 0, 0, letter);
}

// How many images may be mounted at once (each pins a 256 KiB cache).
static inline int sys_diskimg_max_mounts(void) {
    return (int)syscall4(SYS_DISKIMG, DISKIMG_CMD_MAX_MOUNTS, 0, 0, 0);
}

// ===========================================================================
// #VOLAPI: THE MEDIATED VOLUME GATEWAY, FOR ORDINARY USERLAND APPS.
//
// The requirement was "a proxy/gateway/api to allow virtual cd access to
// usermode apps". This is that API, and it is deliberately two-thirds nothing:
//
//   1. sys_vol_info() tells you WHAT a volume is - its class, whether it is a
//      CD-ROM, whether it is read-only, its geometry, the disc's own LABEL, and
//      one VFS path (`root`).
//   2. Everything else is ORDINARY FILE I/O beneath that root. open(), read(),
//      lseek(), close(), opendir()/readdir(). There is no vol_open(), no
//      vol_read() and no volume file-descriptor family, because the files are
//      already in the namespace and every credential check already applies to
//      them. A second handle family would only have been a second place to
//      forget a check.
//
// SO THE WHOLE USAGE IS:
//
//     dimg_vol_t v;
//     for (int i = 0; i < 26; i++) {
//         if (sys_vol_info(i, &v) != 0) continue;        // not yours, or none
//         if (!(v.flags & DISKIMG_F_MOUNTED)) continue;  // no disc in it
//         if (v.cls != DISKIMG_CLASS_CDROM) continue;    // "is this a CD?"
//         if (strcmp(v.label, "CD1") != 0)   continue;   // find it BY LABEL
//         char p[256]; snprintf(p, sizeof p, "%s/MAIN.MIX", v.root);
//         int fd = open(p, O_RDONLY, 0);                 // ordinary open()
//     }
//
// WHAT YOU CANNOT DO THROUGH IT, by construction rather than by check:
//   - reach the block device (there is no LBA, no channel, no drive number, and
//     blk_read is not exposed to Ring 3 at all);
//   - reach a volume you were not granted (sys_vol_info() refuses with -13 for a
//     root your credentials cannot traverse, and the files under it would refuse
//     identically, because it is one perms_check() answering both);
//   - escape the volume (`root` is a normal path and ".." is canonicalised by
//     the same path chokepoint every other syscall uses);
//   - write to it (read-only media is refused in fs/fat.c, for root too).
//
// GENERIC, NOT DOS-SPECIFIC. The Ring-3 DOS host is the first consumer, not the
// intended only one: a file manager, a media player or an installer wanting the
// mounted disc gets the same three fields it needs (is it a CD, what is it
// called, where are its files) with no DOS concepts in sight.
// ===========================================================================

// Mirrors dimg_vol_t in kernel/dos/diskimg.h. The kernel _Static_asserts its
// size at 288 so it can share syscall 361's argtab entry with diskimg_info_t;
// keep the two in step or the validator's write window stops matching.
typedef struct {
    unsigned int   letter;               // 0 = A: .. 25 = Z:
    unsigned int   cls;                  // DISKIMG_CLASS_* (is it a CD-ROM?)
    unsigned int   fmt;                  // DISKIMG_FMT_*
    unsigned int   flags;                // DISKIMG_F_*
    unsigned int   gen;                  // which disc this is; changes on swap
    unsigned int   bytes_per_sector;     // 2048 on ISO 9660
    unsigned int   sectors_per_cluster;  // 1 on ISO 9660
    unsigned int   total_clusters;       // clamped to 0xFFFF, as DOS reports it
    unsigned long long size;             // media bytes, 0 if nothing mounted
    char           label[40];            // the disc's OWN name, "" if none
    char           root[64];             // "/WINDIR/DRIVE_E": where its files are
    unsigned char  reserved[144];        // width lock; always zero
} dimg_vol_t;

// Describe drive letter index `letter` (0 = A: .. 25 = Z:).
// Returns 0 and fills *out; -1 for a bad index; -13 when your credentials may
// not traverse that volume's root; -14 on a bad output pointer.
//
// A letter with NO disc in it still returns 0, with DISKIMG_F_MOUNTED clear and
// an empty label. That is on purpose: "there is a drive here and it is empty" is
// a different fact from "there is no such drive", and an app that cannot tell
// them apart cannot offer to insert anything.
static inline int sys_vol_info(int letter, dimg_vol_t *out) {
    return (int)syscall4(SYS_DISKIMG, DISKIMG_CMD_VOLINFO, 0, (long)out, letter);
}



// ===========================================================================
// CROSS-WINDOW DRAG ("docking"): SYS_DRAG_* 401-406
// ===========================================================================
//
// Drag a thing out of one process's window and drop it into another's. See the
// long block comment at the same numbers in kernel/proc/syscall.h for why this
// exists, why it is small, and why the payload is serialized state rather than
// a file descriptor (this kernel has no cross-process fd passing, and the
// terminal has no long-lived shell to hand over even if it did; measured
// 2026-08-25).
//
// TYPICAL SOURCE FLOW (the window the user dragged FROM):
//   press on a tab, move past the drag threshold
//     sys_drag_begin(my_win, DRAG_KIND_TERMTAB, blob, n, "tabname");
//   ... the compositor now draws the ghost; you draw nothing outside your own
//       window, and you stop receiving mouse events once the cursor leaves you.
//   you are told the outcome by EVENT_DRAG_END, whose target_id is the
//   accepting window handle + 1, or 0 for "nobody took it" (drop on empty
//   desktop = detach into a new window at mouse_x/mouse_y, which are SCREEN
//   coordinates for this event, not content coordinates).
//
// TYPICAL TARGET FLOW (a window willing to receive):
//   once, after win_create:  sys_drag_accept(my_win, DRAG_KIND_TERMTAB);
//   on EVENT_MOUSE_MOVE with a button held: sys_drag_peek(&info) tells you
//     whether to draw an insertion caret. It returns -1 when nothing is being
//     dragged, which is the common case, so this is cheap.
//   on EVENT_DRAG_DROP (mouse_x/mouse_y are CONTENT coordinates):
//     sys_drag_take(my_win, buf, sizeof buf);
//
// A window that never calls sys_drag_accept() is never resolved as a target
// and can never be reached by this protocol.
#define SYS_DRAG_BEGIN    401
#define SYS_DRAG_PEEK     402
#define SYS_DRAG_TAKE     403
#define SYS_DRAG_ACCEPT   404
#define SYS_DRAG_RELEASE  405
#define SYS_DRAG_END      406

#define DRAG_KIND_TERMTAB 0x1
#define DRAG_KIND_TEXT    0x2
#define DRAG_KIND_FILE    0x4

#define DRAG_LABEL_CAP    64
#define DRAG_PAYLOAD_CAP  4096

// Must match drag_info_t in kernel/proc/syscall.h byte for byte, and DragInfo
// in kernel/rustkern/dragsess.rs. The payload is deliberately NOT here: peek is
// called every frame by the compositor to caption the drag ghost, and a
// terminal payload can carry scrollback. Only sys_drag_take() returns bytes.
typedef struct {
    int          active;
    int          src_win;
    unsigned int src_pid;
    unsigned int kind;
    int          payload_len;
    int          released;
    int          drop_x;
    int          drop_y;
    int          target_win;
    int          label_len;
    unsigned char label[DRAG_LABEL_CAP];
} drag_info_t;
_Static_assert(sizeof(drag_info_t) == 104,
               "drag_info_t layout is duplicated in kernel/proc/syscall.h and "
               "kernel/rustkern/dragsess.rs; a one-sided edit breaks SYS_DRAG_PEEK");

// Begin a drag. `payload` is opaque to everyone except the window that
// eventually takes it. An over-length payload is REFUSED (returns -1), never
// truncated: a half scrollback still looks like a scrollback. `label` is a
// short caption the compositor draws in the ghost, and IS clamped, because a
// shortened caption is cosmetic where a shortened payload is corruption.
// Returns 0, or -1 (bad handle, unknown kind, payload too large, or a drag is
// already in flight: exactly one exists system-wide).
static inline int sys_drag_begin(int win, unsigned int kind,
                                 const void *payload, int plen,
                                 const char *label) {
    int llen = 0;
    if (label) { while (label[llen] && llen < DRAG_LABEL_CAP) llen++; }
    return (int)syscall6(SYS_DRAG_BEGIN, (long)win, (long)kind,
                         (long)payload, (long)plen, (long)label, (long)llen);
}

// Non-destructive look at the current session. Returns 0 and fills `info`, or
// -1 when no drag is in flight (the overwhelmingly common case).
static inline int sys_drag_peek(drag_info_t *info) {
    return (int)syscall1(SYS_DRAG_PEEK, (long)info);
}

// Claim the payload. Only the window the kernel resolved as the drop target
// may do this, and only after the button is up. Returns bytes copied
// (min(payload_len, cap)), or -1.
static inline int sys_drag_take(int win, void *dst, int cap) {
    return (int)syscall3(SYS_DRAG_TAKE, (long)win, (long)dst, (long)cap);
}

// Declare which payload kinds this window will receive; mask 0 unregisters.
// Call it once after win_create(). Returns 0, or -1 on a bad handle or an
// unknown kind bit.
static inline int sys_drag_accept(int win, unsigned int kind_mask) {
    return (int)syscall2(SYS_DRAG_ACCEPT, (long)win, (long)kind_mask);
}

// COMPOSITOR ONLY: the mouse button came up at (x, y). The KERNEL resolves the
// drop target itself from its own hit-test, so this call cannot name one.
// Returns the resolved target handle, -1 for none, or -1 when idle.
static inline int sys_drag_release(int x, int y) {
    return (int)syscall2(SYS_DRAG_RELEASE, (long)x, (long)y);
}

// Abandon a drag (the source pressed ESC, or is exiting). Returns 0, or -1 if
// there was nothing in flight.
static inline int sys_drag_end(void) {
    return (int)syscall0(SYS_DRAG_END);
}

#endif // _SYSCALL_H
