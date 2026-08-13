// syscall.h - System call interface for MayteraOS
#ifndef SYSCALL_H
#define SYSCALL_H

#include "../types.h"

// ============================================================================
// System Call Numbers
// ============================================================================

// Process control
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

// POSIX process groups and sessions (#745 local 82). These four numbers
// were declared in userland/libc/syscall.h with working wrappers in
// unistd.c and defined NOWHERE here, so every call hit the dispatcher
// default and returned -1. process_t already carries pgrp/session, fork
// already inherits them, and drivers/tty.c already raises SIGINT/SIGQUIT/
// SIGTSTP/SIGHUP at the foreground pgrp: the only missing piece was any way
// to PUT a process into a group. Policy lives in rustkern/pgrp.rs.
#define SYS_SETSID          95  // () -> new session id (== caller pid), or -EPERM
#define SYS_SETPGID         96  // (pid, pgid) -> 0 / -errno
#define SYS_GETPGID         97  // (pid) -> pgrp / -ESRCH
#define SYS_GETSID          106 // (pid) -> session / -ESRCH

// POSIX poll(2) (#745 local 82). Implemented in rustkern/pollsys.rs over
// the file_poll() readiness primitive that select() already used.
#define SYS_POLL            104 // (struct pollfd *, nfds, timeout_ms) -> ready count

// Memory management
#define SYS_BRK             20  // Change data segment size
#define SYS_MMAP            21  // Map memory
#define SYS_MUNMAP          22  // Unmap memory

// GUI syscalls (MayteraOS specific)
#define SYS_WIN_CREATE      30  // Create window
#define SYS_WIN_DESTROY     31  // Destroy window
#define SYS_WIN_DRAW_RECT   32  // Draw rectangle
#define SYS_WIN_DRAW_TEXT   33  // Draw text
#define SYS_WIN_DRAW_PIXEL  34  // Draw pixel
#define SYS_WIN_BLIT        35  // Blit bitmap
#define SYS_WIN_GET_EVENT   36  // Get window event
#define SYS_WIN_INVALIDATE  37  // Invalidate window (request redraw)
#define SYS_WIN_GET_SIZE    38  // Get window content dimensions

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

// Filesystem manipulation
#define SYS_RENAME          70  // Rename file/directory

// #430: Signals (numbers fixed to match userland libc/syscall.h). The kernel
// signal engine lives in proc/signal.c; these wire it into the dispatcher.
#define SYS_KILL            80  // kill(pid, sig)
#define SYS_SIGACTION       81  // sigaction(signo, act, oact)
#define SYS_SIGPROCMASK     82  // sigprocmask(how, set, oldset)
#define SYS_SIGRETURN       83  // rt_sigreturn (invoked from the libc trampoline)
#define SYS_ALARM           84  // alarm(seconds)
#define SYS_PAUSE           85  // pause()

// #430: Threading + futex (numbers fixed to match userland libc/syscall.h).
#define SYS_CLONE           110 // clone(flags, stack, ptid, ctid, tls)
#define SYS_GETTID          111 // gettid()
#define SYS_SET_TID_ADDRESS 112 // set_tid_address(tidptr)
#define SYS_FUTEX           113 // futex(addr, op, val, timeout, addr2, val3)
#define SYS_TKILL           114 // tkill(tid, sig)
#define SYS_TGKILL          115 // tgkill(tgid, tid, sig)

// Working directory
#define SYS_GETCWD          99  // Get current working directory
#define SYS_CHDIR           100 // Change directory

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
#define SYS_GET_NET_INFO    146 // Fill net_info_t buffer
#define SYS_NTP_SYNC        147 // Sync from NTP (default server); 0=ok -1=fail
#define SYS_NTP_SYNC_SERVER 367 // #797 Sync from a CALLER-NAMED NTP server; 0=ok, else -SNTP_E_*
#define SYS_SET_CURSOR_THEME 148 // Set cursor theme: 0=Retro, 1=Light, 2=Dark
#define SYS_GET_CURSOR_THEME 149 // Get current cursor theme
#define SYS_WIN_GET_POS     150 // Get window screen position: arg1=handle, arg2=*x, arg3=*y

// Maximum syscall number
// Window manager query (for compositor)
#define SYS_WM_GET_WINDOWS      155
#define SYS_WM_FOCUS_WINDOW    157
#define SYS_WM_MINIMIZE_WINDOW 158
#define SYS_WM_MAXIMIZE_WINDOW 260  // toggle maximize/restore of focused window (F11)
#define SYS_RUN_NEXT_ON_AP 261
#define SYS_WIN_SET_NOCHROME 262  // #185: mark a window borderless (no chrome)
#define SYS_WIN_SET_SHADOW   376  // #745: opt a window in to the compositor drop shadow
#define SYS_GET_MOUSE_SCROLL 263  // OS-wide wheel: read+clear kernel scroll delta  // #279: route the next user proc this caller launches onto an application processor
#define SYS_GET_GLOBAL_MOUSE 264
#define SYS_HTTP_POST_START     265  // #264 async POST: start (url,headers,body) -> job id
#define SYS_HTTP_POST_POLL      266  // #264 async POST: poll  (id,&status,&len) -> 0run/1done/2err
#define SYS_HTTP_POST_READ      267  // #264 async POST: read  (id,buf,max) -> bytes, frees job
#define SYS_HTTP_POST_CANCEL    268  // #264 async POST: cancel (id)  // #185: read-only global cursor for non-compositor windows
#define SYS_NET_MOUNT          269  // #317: mount an SMB share with creds (server,share,user,pass) -> 0/-1
#define SYS_NET_LIST_SHARES    270  // #317: enumerate a server's shares (server,buf,maxlen) -> count
#define SYS_NET_UNMOUNT        271  // #317: unmount an SMB share (server,share) -> 0/-1
// #325 Device Manager: read-only hardware enumeration (structs in devinfo.h)
#define SYS_DEV_PCI_LIST       272  // (devinfo_pci_t *buf, int max) -> count
#define SYS_DEV_USB_LIST       273  // (devinfo_usb_t *buf, int max) -> count
#define SYS_DEV_IRQ_LIST       274  // (devinfo_irq_t *buf, int max) -> count
#define SYS_SYSINFO            275  // (devinfo_sysinfo_t *out) -> 0/-1

// #265 cron-like timer/scheduler (structs in proc/cron.h)
#define SYS_CRON_ADD           276  // (cron_job_t *job) -> new id (>0) / <0
#define SYS_CRON_LIST          277  // (cron_job_t *buf, int max) -> count
#define SYS_CRON_REMOVE        278  // (uint32_t id) -> 0/-1
#define SYS_CRON_ENABLE        279  // (uint32_t id, int enable) -> 0/-1
#define SYS_WIN_MOVE           280  // #334: move a window to absolute screen x,y (handle,x,y) -> 0/-1
#define SYS_WIN_MOVE_BY        281  // #334: move a window by dx,dy (handle,dx,dy) -> 0/-1
#define SYS_DNS_START          215
#define SYS_DNS_POLL           216
#define SYS_LIST_USERS         190
#define SYS_AUTHENTICATE       191
#define SYS_DELETE_USER        159
#define SYS_GET_DISK_INFO      199
#define SYS_PLAY_WAV           192
#define SYS_POWEROFF           206
#define SYS_REBOOT             207
#define SYS_SET_WALLPAPER      204
#define SYS_GET_WALLPAPER      205
#define SYS_SET_ICON_SIZE   208
#define SYS_GET_ICON_SIZE   209
#define SYS_NET_SET_STATIC  217
#define SYS_NET_DHCP        218
#define SYS_NET_IS_UP       299  // #374 network-up gate (link+IP+stack)
#define SYS_DESKTOP_MENU_RELOAD 300  // (#402) reload Start menu after App Store install
#define SYS_PKG_WRITE           301  // (#402) package manager: write a file to the FAT ESP
#define SYS_HTTP_FETCH_HDR      302  // (#414) blocking GET with auth headers (haservice)
// #443: expose the kernel's existing tcp_listen()/tcp_accept() to userland so a
// user process can be a normal listening TCP server (previously only the
// in-kernel sshd used them; userland could only reverse-connect out).
#define SYS_LISTEN              303  // (sock, port, backlog) -> 0/-1: bind+listen
#define SYS_ACCEPT              304  // (sock) -> new socket fd, or WOULD_BLOCK/-1

// #524 - real BSD sockets over the TCP/UDP stack. fd-integrated (return real
// file descriptors, not TCB slot indices), blocking + non-blocking, TCP + UDP.
// DISTINCT from the legacy raw SYS_SOCKET/SYS_CONNECT/... surface above.
// close() reuses SYS_CLOSE (VFS release); O_NONBLOCK reuses SYS_FCNTL.
#define SYS_SOCK_OPEN           343  // (domain,type,proto) -> fd
#define SYS_SOCK_BIND           344  // (fd, sockaddr_in*, addrlen) -> 0/-err
#define SYS_SOCK_CONNECT        345  // (fd, sockaddr_in*, addrlen) -> 0/-err (blocking)
#define SYS_SOCK_LISTEN         346  // (fd, backlog) -> 0/-err
#define SYS_SOCK_ACCEPT         347  // (fd, sockaddr_in*, addrlen*) -> newfd/-err (blocking)
#define SYS_SOCK_SEND           348  // (fd, buf, len, flags) -> n/-err
#define SYS_SOCK_RECV           349  // (fd, buf, len, flags) -> n/-err (blocking)
#define SYS_SOCK_SENDTO         350  // (fd, buf, len, flags, sockaddr_in*, addrlen) -> n/-err
#define SYS_SOCK_RECVFROM       351  // (fd, buf, len, flags, sockaddr_in*, addrlen*) -> n/-err (blocking)
#define SYS_SOCK_SETOPT         352  // (fd, level, optname, optval*, optlen) -> 0/-err
#define SYS_SOCK_GETOPT         353  // (fd, level, optname, optval*, optlen*) -> 0/-err
#define SYS_SOCK_SELECT         354  // (nfds, readfds*, writefds*, exceptfds*, timeout*) -> count/-err
#define SYS_SOCK_SHUTDOWN       355  // (fd, how) -> 0/-err
// #443: set the PHYSICAL mouse button bitmask (same variable the real PS/2 IRQ
// path writes). sys_set_mouse() only ever moved the cursor; the desktop's own
// icon/taskbar/start-menu click handling reads the physical mouse_buttons
// global directly (not sys_inject_mouse's window-manager relay), so injected
// remote clicks landed on app windows but never registered on the desktop.
#define SYS_SET_MOUSE_BUTTONS   305  // (mask) -> 0/-1, compositor-only
#define SYS_SET_DISPLAY_FX  219
#define SYS_GET_DISPLAY_FX  220
#define SYS_DRAW_TTF        221
#define SYS_MEASURE_TTF     222
#define SYS_SET_FONT_SIZE   223
#define SYS_GET_FONT_SIZE   224
#define SYS_SET_SCREENSAVER 225
#define SYS_GET_SCREENSAVER 226
#define SYS_SCREENSAVER_TEST 227
#define SYS_GET_SS_TEST     228
#define SYS_SET_SETTINGS_TAB 229  // compositor -> Settings: open this panel index
#define SYS_GET_SETTINGS_TAB 230  // Settings reads + should clear (-1)
#define SYS_SET_WIN_BLANK    231  // Compositor: suppress kernel user-window fb_blits (screensaver active)
#define SYS_WIN_DRAW_TEXT_SMALL 232  // Draw 4x8 small text into a window (tooltips/hints)
#define SYS_WIN_DRAW_TTF        235  // Antialiased TTF text into a window (size in top byte of color)
#define SYS_SET_WIN_OPACITY     233  // Global default window opacity (0-255), applied to all windows
#define SYS_GET_WIN_OPACITY     236  // Read global default window opacity
#define SYS_GET_NET_BYTES       234  // Total link bytes (rx+tx) for the network gauge
#define SYS_WIN16_RUN           237  // Run a Win16 NE/.COM executable by path (GUI launch)
#define SYS_DOS_RUN             240  // Run an MS-DOS .EXE/.COM by path in its own window (GUI launch, #208)
#define SYS_SSH_CLIENT          242  // Interactive SSH client bridged to caller's stdin/stdout
#define SYS_NET_INFO            243  // Verbose network interface report into a user buffer
#define SYS_SETPRIORITY         244  // Set a process's scheduling priority (nice)
#define SYS_GET_TICKS           245  // Monotonic 100Hz tick counter (10ms units)
#define SYS_GET_VERSION         246  // Copy the OS version string into a user buffer
#define SYS_SPAWN_REDIR         247  // Spawn a child with stdin/stdout redirected to files (#shell redirection)
#define SYS_SET_CURSOR          248  // Set live mouse-cursor style + size (#116)
#define SYS_GET_CURSOR          249
// #610: run the READ-ONLY ext2 consistency check and copy the report out.
// arg1 = user pointer to an ext2_fsck_report_t (200 bytes), arg2 = its size,
// arg3 = mode (0 run the full check, 1 report the superblock state only).
// There is deliberately NO repair mode and no way to ask for one.
#define SYS_FSCK                356  // Read live mouse-cursor style + size (packed: style|size<<8)
#define SYS_THEME_METRIC        357  // (#711) mtheme v2 integer metric: (theme_id, theme_metric_v2_t) -> int32
// #695 Phase 1. fsync(fd): commit this fd's buffered bytes to the medium WITHOUT
// consuming the fd, so the caller can check the result and still act on it.
// 357 was named as free when this task was scoped and was NOT: #711 took it
// while #695 Phase 0 was landing. Re-measured against the header, not the plan.
#define SYS_FSYNC               358  // (#695) fsync(int fd) -> 0 durable / <0 failed
#define SYS_WM_FORCE_REDRAW_ALL 359  // (#704) () -> count; arms redraw_pending on every open app window (compositor-only, called on a theme change)
// (#739) 361 checked against the WHOLE header, every branch and the libc
// copy on 2026-08-07, not against a plan: 300-360 is contiguous, 360 is the
// true maximum anywhere in the tree, and no gate anywhere fails a build for a
// duplicate number (two agents both claimed 357 for exactly that reason).
#define SYS_DISKIMG             361  // (#739) diskimg(cmd, path, out, letter): mount/eject/query a disk image on a DOS drive letter
#define SYS_SET_SS_DELAY        250  // (#115) Set screensaver activation delay (seconds)
#define SYS_GET_SS_DELAY        251
#define SYS_THEME_COLOR         290  // (#285) Get active theme color by theme_color_id_t  // (#115) Get screensaver activation delay (seconds)

// #318 network printing (IPP client)
#define SYS_PRINT_LIST          291  // (printer_cfg_t *out, int max) -> count
#define SYS_PRINT_JOB           292  // (const char *printer, const char *title, const char *text) -> 0/-1
#define SYS_PRINT_ADD           293  // (name, host, port, queue, make_default) -> 0/-1
#define SYS_PRINT_REMOVE        294  // (const char *name) -> 0/-1
#define SYS_AUDIO_POS_MS        295  // (#335) elapsed ms of the current DAC stream (frames/rate), -1 if idle
#define SYS_HDA_DBG             306  // (#71) userland HDA audio bring-up debug: op,a,b,c -> see hda_debug_op()

// ---------------------------------------------------------------------------
// Ring-3 PCM push (Phase 1 of the Ring-0 media-decode exit).
//
// Lets userland DECODE and push raw PCM, so that the ~121,000 lines of vendored
// decoder C in media/ (faad2/opus/tremor/dr_flac/libmad) that today parse
// attacker-controlled files IN RING 0 behind SYS_PLAY_WAV can move to Ring 3.
// MAYTERA-SEC-2026-0009 (CWE-125 OOB read in media/aac.c mp4_parse, reachable
// from Ring 3 with a crafted .m4a) is exactly the class this contains: in
// userland it becomes a process crash, not a kernel heap over-read.
//
// ADDITIVE: SYS_PLAY_WAV (192) is unchanged and still works. This is a second,
// lower-privilege way in, not a replacement. See drivers/audio_pcm.h.
//
// Typical use: OPEN(rate,ch,fmt) -> h; WRITE(h,buf,frames) repeatedly (BLOCKS on
// a wait queue when the ring is full, never spins); CLOSE(h) drains and joins.
#define SYS_AUDIO_PCM_OPEN      315  // (rate, channels, format) -> handle >=1, or <0
#define SYS_AUDIO_PCM_WRITE     316  // (handle, const void *pcm, frames) -> frames accepted, or <0
#define SYS_AUDIO_PCM_CLOSE     317

// ---- #487/#349 Ring-3 process introspection (Task Manager / Process Explorer)
// The userland Task Manager (/apps/taskmgr) is the app the Start menu actually
// opens; before these it could only call SYS_PROC_LIST, so it could only draw a
// process list. Already exposed, do NOT duplicate: SYS_PROC_LIST 238,
// SYS_KILL 80, SYS_SETPRIORITY 244, SYS_GET_CPU_USAGE 193, SYS_GET_MEM_INFO
// 194, SYS_GET_CPU_PER_CORE 259, SYS_CRON_* 276-279.
#define SYS_PROC_HANDLES       318  // (pid, handle_info_t *buf, int max) -> count / -1
#define SYS_NET_CONNS          319  // (pid|PI_PID_ALL, tcp_conn_info_t *buf, int max) -> count / -1
#define SYS_SVC_LIST           320  // (svc_info_t *buf, int max) -> count / -1
#define SYS_SVC_CONTROL        321  // (const char *name, PI_SVC_START/STOP) -> 0 / -1
#define SYS_PROC_DETAIL        322  // (pid, proc_detail_t *out) -> 1 / -1
#define SYS_SECTEST            323  // (void *ubuf, uint64_t len) -> failing controls
                                    // #500/#503 DEBUG ONLY: only dispatched under
                                    // -DSECTEST_SYSCALL, never in a shipped kernel.
  // (handle) -> 0, or <0
#define SYS_PRINT_IMAGE         296  // (#318) (const char *printer, const char *path) -> 0/-1 (print image file)

// OS-wide font registry (multi-face TrueType)
#define SYS_FONT_COUNT          307  // () -> number of installed faces (>=1)
#define SYS_FONT_NAME           308  // (int idx, char *buf, int cap) -> name length
#define SYS_FONT_GLYPH          309  // (packed[face|size<<8|style<<24], cp, int meta[5], u8 *bmp, cap) -> advance or -1
#define SYS_FONT_METRICS        310  // (packed[face|size<<8], int out[3]={asc,desc,linegap}) -> 0
#define SYS_WIN_DRAW_TTF_EX     311  // (win, x|y<<16, str, face|size<<8|style<<24, color) -> 0 (face-aware window TTF)
#define SYS_FONT_KERN           312
#define SYS_FONT_STYLE      324
#define SYS_FONT_RESCAN     325
#define SYS_FONT_REMOVE     326
#define SYS_FONT_SET_UI     327
#define SYS_FONT_GET_UI     328
#define SYS_FONT_FIND       329  // (packed[face|size<<8], cp1, cp2) -> kerning px
// #542 OS-wide system clipboard (kernel-held, cross-app). Bounded 64 KiB store.
#define SYS_CLIP_SET        330  // (const void *src, uint64_t len) -> bytes stored
#define SYS_CLIP_GET        331  // (void *dst, uint64_t cap) -> total bytes held (copies min)
#define SYS_CLIP_LEN        332  // () -> total bytes currently on the clipboard
#define SYS_FS_PERM_INFO    333  // #554: (const char *path, int reserved, k_fsperm_info_t *out) -> 0/-1: filesystem-aware permission/attribute info (ext2 uid/gid/mode via perms.c, or native FAT attribute byte). See rustkern/fsperm.rs.
// #565: file-based theme loader. (const char *path to a /THEMES/*.mtheme
// file) -> resulting theme index (>=0), or -1 on parse/read failure. Adds a
// new slot or updates an existing one (matched by the file's name= line)
// without a reboot; see kernel/gui/themes.c theme_load_file_runtime() and the
// userland loader userland/libc/gui_theme.c.
#define SYS_THEME_LOAD_FILE 335
// #566 secure session lock + autologin
// #745 lock REASONS. 0 stays IDLE so a stale caller that passes nothing keeps
// the pre-#745 semantics (declined under autologin) instead of silently
// acquiring the new always-honoured explicit behaviour. Policy lives in
// rustkern/sessionid.rs; these must match the constants there and in libc.
#define SESSION_LOCK_IDLE      0    // idle timer fired; autologin declines it
#define SESSION_LOCK_EXPLICIT  1    // the user asked; always honoured if unlockable
#define SYS_SESSION_LOCK       336  // (int reason) -> 0 locked / -1 declined
#define SYS_SESSION_UNLOCK     337  // (const char *user, const char *pass) -> 0 ok / -1 bad / -2 locked
#define SYS_SESSION_IS_LOCKED  338  // () -> 1 locked / 0 unlocked (display cache read)
#define SYS_SET_AUTOLOGIN      339  // (const char *user, const char *pass, int enable) -> 0/-1
#define SYS_GET_AUTOLOGIN      340  // (char *buf, int cap) -> len of configured user (0 = disabled)
#define SYS_AUTH_LOCKOUT       341  // (const char *user) -> seconds of lockout remaining (0 = none)
// #745 sign-in screen mode. SECOND key of /CONFIG/LOGIN.CFG, written by the
// same composer as autologin= so neither key can erase the other
// (rustkern/loginmode.rs). Authorization is IDENTICAL to SYS_SET_AUTOLOGIN:
// root for anyone, non-root only for its own uid and only with the password.
// This is a disclosure setting (it decides whether every account NAME on the
// machine is shown before anyone signs in), so it gets the same gate, not a
// weaker one. The numbers are 373/374 because SYS_NET_PROBE 372 was the
// highest defined and the space below it has no gaps.
#define SYS_SET_LOGIN_MODE     374  // (int mode, const char *user, const char *pass) -> 0/-1
#define SYS_GET_LOGIN_MODE     375  // () -> 0 list / 1 typed / <0 error (caller treats any error as typed)

// Sign-in screen mode encoding. MIRRORED in rustkern/loginmode.rs
// (LOGIN_MODE_LIST / LOGIN_MODE_TYPED) and in libc/syscall.h. Locked to 0/1
// on this side by _Static_assert in proc/syscall_argtab_lock.c, and on the
// Rust side by bit 17 of loginmode_selftest_rs(), which runs at boot.
//
// TYPED IS THE FALLBACK, and deliberately NOT the wizard's default. They
// answer different questions: the default is a recommendation to somebody who
// is present and can change it, while the fallback is what happens when the
// machine cannot tell what was decided. Showing every account name is a
// disclosure and must follow a recorded decision, never a missing file.
#define LOGIN_MODE_LIST        0
#define LOGIN_MODE_TYPED       1

// The configured mode, for KERNEL readers (gui/login.c, the boot gate).
// Defined in proc/syscall.c beside the syscall that returns the same value,
// so the gate and Ring 3 read one file through one parse.
int login_mode_configured(void);
#define SYS_WM_APPS_DIRTY      342  // () -> 1 if any KWM window changed (move/resize/focus/create/close/win_invalidate) since the last SYS_COMPOSITOR_RENDER_WINDOWS composite, else 0. #564 idle-CPU render gate; see kernel/gui/window.c sys_wm_apps_dirty().
// #492 Stage 1a: kernel self-update (OTA) write path. arg1=new_kernel image
// ptr, arg2=len, arg3=expected_sha256[32] ptr, arg4=target_build. Returns a
// SELFUPD_* code (0=OK). Reboot afterwards via the existing SYS_REBOOT (207).
#define SYS_KERNEL_SELFUPDATE   313
#define SYS_OTA_VERIFY_SIG      314  // (digest[32], sig, sig_len) -> 0 valid / -1: verify a signed OTA manifest against the baked-in pubkey
#define SYS_APP_VERIFY_SIG      334  // (digest[32], sig, sig_len) -> 0 valid / -1: verify a signed APP-REPO manifest against the baked-in APP key (#563 key split; domain-separated from the OTA key). RESTORED: dropped by 68199dd (#565) which silently defeated the key split.
#define SYS_PRINT_SCREEN        297  // (#318) (const char *printer) -> 0/-1 (print the framebuffer)
#define SYS_BOOTLOG_WRITE       298  // (#307 real-hw) (const char *msg) -> 0/-1, appends to persistent /BOOTLOG.TXT
#define SYS_UPTIME_MS           252  // Monotonic ms since boot (ticks*1000/g_timer_hz)
#define SYS_DECODE_IMAGE        253  // Decode+point-sample an image (BMP/PNG/JPEG) to a target box
#define SYS_WIN_DRAW_IMAGE      254
#define SYS_HTTP_FETCH_START    255  // async fetch: start  (url) -> job id
#define SYS_HTTP_FETCH_POLL     256  // async fetch: poll   (id,&status,&len) -> 0run/1done/2err
#define SYS_HTTP_FETCH_READ     257  // async fetch: read   (id,buf,max) -> bytes, frees job
#define SYS_HTTP_FETCH_CANCEL   258
#define SYS_HTTP_FETCH_PROGRESS 368  // async fetch: progress (id,&phase,&bytes_recv,&content_len) -> 0/-1 (#25)
#define SYS_GET_CPU_PER_CORE    259  // per-core CPU %: buf[0]=count, buf[1..]=pct  // async fetch: cancel (id)  // Blit a w*h BGRA buffer into a window's content at (x,y)
#define SYS_WIN16_ACTIVE        241  // Query whether a Win16 app owns the foreground (#200 SkiFree)

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

// Framebuffer / Compositor (matches gui/fb_syscall.h)
#define SYS_FB_MAP              200
#define SYS_FB_INFO             201
#define SYS_FB_FLIP             202
#define SYS_FB_DAMAGE           203
#define SYS_GET_MOUSE           210
#define SYS_SET_MOUSE           211
#define SYS_GET_KEY             212
#define SYS_GRAB_INPUT          213
#define SYS_INJECT_MOUSE        214  // Compositor: inject mouse event into KWM handlers

#define SYS_COMPOSITOR_RENDER_WINDOWS  156  // Compositor: draw KWM windows onto compositor FB
#define SYS_DUP                        90
#define SYS_DUP2                       91
#define SYS_PIPE                       92
#define SYS_FCNTL                      93  // #359: fcntl(fd,cmd,arg)
#define SYS_IOCTL                      94  // Device control (termios, winsize, ...)
#define SYS_WAITPID         98  // Wait for specific child process
#define SYS_GET_CPU_USAGE              193
#define SYS_GET_MEM_INFO               194
#define SYS_GET_KEYBOARD               195  // Compositor: read raw key from hardware queue
#define SYS_SPAWN                      196  // Spawn process: path (no args)
#define SYS_INJECT_KEY                 197  // Compositor: inject key event into KWM queue
#define SYS_SPAWN_ARGS                 198  // Spawn process with argv: path, argv[], argc
// (#745) OOBE Network page. STRUCTURED live IPv4 status, so Ring 3 never has
// to string-match the human report SYS_NET_INFO (243) emits. See
// rustkern/netstat.rs for why this is a new accessor and not a widening of
// SYS_GET_NET_INFO (146).
//   SYS_NET_STATUS(net_status_t *out) -> 0, -1 bad ptr, -14 EFAULT
#define SYS_NET_STATUS                 371
//   SYS_NET_PROBE(op, arg) -> see NET_PROBE_* below. NON-BLOCKING: it never
//   sleeps, so the caller owns the deadline. No pointer args (nothing for
//   rustkern/argtab.rs to describe).
#define SYS_NET_PROBE                  372

// SYS_NET_PROBE op codes. Mirrored in rustkern/netstat.rs and
// userland/libc/syscall.h.
#define NET_PROBE_PING_START    0   // arg = destination IPv4, HOST order
#define NET_PROBE_PING_POLL     1   // -> rtt ms (>=0), or a NET_PROBE_E* code
#define NET_PROBE_PING_CANCEL   2
#define NET_PROBE_DHCP_RESTART  3   // non-blocking dhcp_reset + dhcp_discover

// SYS_NET_PROBE return codes. A successful POLL returns an RTT in ms, which
// is >= 0, so every failure is negative and distinguishable.
#define NET_PROBE_PENDING      (-1) // no answer yet; poll again
#define NET_PROBE_EINVAL       (-2)
#define NET_PROBE_ENOTSTARTED  (-3) // POLL without a START
#define NET_PROBE_ELINK        (-4) // no carrier: instant no-op (#381)

// C mirror of `NetStatus` in rustkern/netstat.rs. ALL ADDRESS FIELDS ARE HOST
// BYTE ORDER ((a<<24)|(b<<16)|(c<<8)|d for a.b.c.d), which is what the IP
// layer stores; nothing here byte-swaps. 0 means "not configured" and is a
// REAL state to render, not a blank.
typedef struct {
    uint32_t ip;             // ip_get_address(): the live source address
    uint32_t netmask;        // ip_get_netmask()
    uint32_t gateway;        // ip_get_gateway(); 0 = no default route
    uint32_t dns_active;     // dns_get_server(): what the resolver WILL query
    uint32_t dns_dhcp;       // dhcp_get_dns(): what DHCP offered; 0 = none
    uint32_t dhcp_ip;        // dhcp_get_ip(): leased address (0 while a DORA runs)
    uint32_t link_up;        // nic_link_up() as 0/1
    uint32_t dhcp_state;     // DHCP_STATE_* (0 idle 1 discovering 2 requesting 3 bound)
    uint32_t config_static;  // g_net_static_configured as 0/1
    uint32_t faulty;         // net_is_faulty() as 0/1 (#549)
    uint32_t driver;         // net_driver_type_t; 0 = NO NIC AT ALL
    uint32_t prefix_len;     // popcount(netmask), precomputed
} net_status_t;
_Static_assert(sizeof(net_status_t) == 48,
               "net_status_t sizeof lock (Rust NetStatus in rustkern/netstat.rs, "
               "SZ_NET_STATUS in rustkern/argtab.rs, and the userland mirrors)");

// #549: a fetch/POST REFUSED because the interface is marked NET_FAULTY, as
// distinct from a request that ran and failed (-1). The UI needs the difference:
// "adapter marked faulty" is actionable, "could not start the request" is not.
// Only sys_http_fetch_start() returns this today; the synchronous entry points
// keep -1 for ABI stability. net_status_t.faulty carries the same truth for any
// caller that would rather poll the state than decode a return code.
#define NET_ERR_FAULTY                 (-3)

// #745: a POST REFUSED by the prompt-injection screen, as distinct from a
// request that failed (-1) and from one refused by the circuit breaker (-3).
// The client needs the difference: this one is NOT retryable and NOT a network
// problem, and telling the user "network error" for it would be a lie that also
// hides a security event. A silent block is its own bug, so this code exists
// specifically so the refusal can be reported honestly.
#define NET_ERR_AIGUARD                (-4)

// (#745) Publish the desktop WORK AREA to the kernel window manager: the four
// px insets the active dock style reserves at the left/top/right/bottom screen
// edges. Compositor-only. No pointer args, so nothing for rustkern/argtab.rs
// to describe. See kernel/gui/window.h for why the strut is derived in the
// compositor and merely consumed here.
//   SYS_WM_SET_WORK_AREA(left, top, right, bottom) -> 0, or -1 if not the compositor
#define SYS_WM_SET_WORK_AREA           373

// SYS_MAX is a SENTINEL: one past the highest defined syscall number.
// It read 215 for a long time while real syscalls ran to 364, which was
// harmless ONLY because it had zero callers. Anyone who had used it as a
// bounds check would have rejected 149 working syscalls. If you add a
// syscall above this value, bump it in the same commit.
// It was ALSO wrong in the other direction when this was written: it read
// 372, which is SYS_NET_PROBE's own number, not one past it. Still harmless
// (zero callers), still fixed here rather than left for whoever first uses it
// as a bound and silently loses the top syscall.
// ===========================================================================
// #745 ELEVATION (proc/elevate.h). Five numbers, allocated in one block after
// 377 so the syscall-number-lint's "strictly past the top" rule is satisfied
// once rather than five times.
//
// 380 and 381 are COMPOSITOR ONLY, enforced by the framebuffer latch in
// gui/fb_syscall.c, not by a uid. An ordinary app calling them gets ELEV_EPERM.
// ===========================================================================
#define SYS_ELEV_REQUEST               378  // (const elev_request_t *) -> seq(>0) | ELEV_E*
#define SYS_ELEV_STATUS                379  // (uint64 seq) -> ELEV_ST_* | ELEV_ESTALE
#define SYS_ELEV_VIEW                  380  // COMPOSITOR ONLY (elev_view_t *) -> 1 open / 0 none
#define SYS_ELEV_RESOLVE               381  // COMPOSITOR ONLY (seq, action, const char *pw)
#define SYS_ELEV_MAY                   382  // () -> 1 if the CALLER may elevate, else 0

// ===========================================================================
// #745 AI PROMPT-INJECTION SCREEN (security/aiguard.h + security/nova.c).
//
// Informational, NOT the enforcement point. The enforcement is unconditional
// and lives in sys_http_post_start(); this exists so a Ring-3 client can name
// the rule that fired when it tells the user why a turn was refused.
//   SYS_AI_SCAN(const char *text, aiguard_verdict_t *out)
//     -> AIGUARD_ALLOW / _ANNOTATE / _BLOCK, or -1 bad args / -14 EFAULT
// ===========================================================================
#define SYS_AI_SCAN                    383

#define SYS_MAX                        384

// ============================================================================
// Syscall Register Convention (AMD64 System V ABI)
// ============================================================================
// Syscall number: RAX
// Arguments: RDI, RSI, RDX, R10, R8, R9
// Return value: RAX
// Clobbered by syscall: RCX, R11
// ============================================================================

// Syscall context (saved on kernel stack)
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;   // Clobbered by SYSCALL (contains RFLAGS)
    uint64_t r10;   // arg4
    uint64_t r9;    // arg5
    uint64_t r8;    // arg6
    uint64_t rbp;
    uint64_t rdi;   // arg1
    uint64_t rsi;   // arg2
    uint64_t rdx;   // arg3
    uint64_t rcx;   // Clobbered by SYSCALL (contains RIP)
    uint64_t rbx;
    uint64_t rax;   // syscall number / return value
} __attribute__((packed)) syscall_context_t;

// ============================================================================
// Syscall API
// ============================================================================

// Initialize syscall mechanism (set up MSRs)
void syscall_init(void);

// Main syscall dispatcher (called from assembly)
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6);

// ============================================================================
// Individual syscall handlers
// ============================================================================

// Process
int64_t sys_exit(int exit_code);
int64_t sys_fork(void);
int64_t sys_exec(const char *path);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_yield(void);
int64_t sys_sleep(uint32_t ms);

// File I/O
int64_t sys_open(const char *path, int flags);
int64_t sys_fcntl(int fd, int cmd, long arg);  // #359
int64_t sys_play_wav(const char *path);
// Ring-3 PCM push (see drivers/audio_pcm.h). Additive; sys_play_wav unchanged.
int64_t sys_audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format);
int64_t sys_audio_pcm_write(int handle, const void *pcm, uint32_t frames);
int64_t sys_audio_pcm_close(int handle);
int64_t sys_close(int fd);

// #745: read-only accessor for the live wallpaper ordinal the
// compositor is displaying (see syscall.c). Used by the kernel login
// gate for backdrop continuity across a Switch User / Log Out.
int syscall_get_wallpaper_idx(void);
int64_t sys_fsync(int fd);   // #695 Phase 1
int64_t sys_read(int fd, void *buf, size_t count);
int64_t sys_write(int fd, const void *buf, size_t count);
int64_t sys_seek(int fd, int64_t offset, int whence);
int64_t sys_stat_path(const char *path, void *ubuf);
#define SYS_HTTP_FETCH 86
int64_t sys_http_fetch(const char *url, char *ubuf, uint32_t max_len, uint32_t *ubytes, int *ustatus);
int64_t sys_http_fetch_start(const char *url);
int64_t sys_http_fetch_poll(int id, int *ustatus, uint32_t *ulen);
int64_t sys_http_fetch_read(int id, char *ubuf, uint32_t max);
int64_t sys_http_fetch_cancel(int id);
int64_t sys_http_fetch_progress(int id, int *uphase, uint32_t *ubytes, uint32_t *ucontent_len);   // #25
int64_t sys_http_post_start(const char *url, const char *headers, const char *body);

// #745 SYS_AI_SCAN. `uout` is an aiguard_verdict_t* (security/aiguard.h); it is
// declared void* here so syscall.h does not have to pull that header in.
int64_t sys_ai_scan(const char *utext, void *uout);
int64_t sys_http_fetch_hdr(const char *url, const char *headers, char *ubuf, uint32_t max_len, uint32_t *ubytes, int *ustatus);
int64_t sys_http_post_poll(int id, int *ustatus, uint32_t *ulen);
int64_t sys_http_post_read(int id, char *ubuf, uint32_t max);
int64_t sys_http_post_cancel(int id);

// Memory
int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags);
int64_t sys_munmap(uint64_t addr, uint64_t len);

// Console
int64_t sys_putchar(int c);
int64_t sys_getchar(void);

// Time
int64_t sys_time(void);
int64_t sys_clock(void);

// File system (directories)
int64_t sys_mkdir(const char *path, int mode);
int64_t sys_rmdir(const char *path);
int64_t sys_unlink(const char *path);
int64_t sys_rename(const char *oldpath, const char *newpath);
int64_t sys_getcwd(char *buf, uint64_t size);
int64_t sys_chdir(const char *path);

// POSIX process groups / sessions (#745 local 82). Glue in proc/syscall.c;
// every rule is in rustkern/pgrp.rs.
int64_t sys_setsid(void);
int64_t sys_setpgid(int64_t pid_arg, int64_t pgid_arg);
int64_t sys_getpgid(int64_t pid_arg);
int64_t sys_getsid(int64_t pid_arg);

// POSIX poll(2) (#745 local 82). The WHOLE handler is Rust
// (rustkern/pollsys.rs); the dispatcher calls it directly, so there is no C
// wrapper to drift from it.
int64_t sys_poll_rs(void *ufds, uint64_t nfds, int64_t timeout_ms);
int64_t sys_readdir(int fd, void *entry_buf);

// Window/Graphics
int64_t sys_win_create(const char *title, int x, int y, int width, int height);
// (#745) see SYS_WM_SET_WORK_AREA above
int64_t sys_wm_set_work_area(int32_t left, int32_t top, int32_t right, int32_t bottom);
int64_t sys_win_set_nochrome(int handle);
int64_t sys_win_destroy(int handle);
int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color);
int64_t sys_win_draw_text(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color);
int64_t sys_win_get_event(int handle, void *event_buf, int timeout);
int64_t sys_win_invalidate(int handle);
int64_t sys_wm_force_redraw_all(void);  // (#704)
int64_t sys_win_get_size(int handle, int *width, int *height);

// User identity
int64_t sys_getuid(void);
int64_t sys_setuid(uint32_t uid);
int64_t sys_getgid(void);
int64_t sys_setgid(uint32_t gid);
int64_t sys_geteuid(void);
int64_t sys_getegid(void);
int64_t sys_seteuid(uint32_t euid);
int64_t sys_setegid(uint32_t egid);
int64_t sys_chmod(const char *path, uint16_t mode);
int64_t sys_chown(const char *path, uint32_t uid, uint32_t gid);
int64_t sys_fs_perm_info(const char *path, int reserved, void *ubuf); // #554
int64_t sys_theme_load_file(const char *path); // #565
int64_t sys_theme_contrast_corrections(int64_t theme_id); // (themes ticket)
int64_t sys_passwd_change(const char *username, const char *old_pass, const char *new_pass);
int64_t sys_su(const char *username, const char *password);
// #745: create an account AND set its password in ONE call, or create nothing.
// sys_adduser() takes no password and never touched the shadow table, so every
// account it created was unauthenticatable; the shipped `ref` account at uid
// 1002 is exactly that. A two-call create-then-set-password sequence has the
// same failure mode the moment the second call is skipped, mis-ordered or
// fails, so the fix is a single call that rolls back rather than a convention.
// uid 0 means ALLOCATE (kernel picks the next free human uid); gid 0 means
// "same as uid". Returns the uid on success, negative on failure.
// (#306) Install-to-disk handlers. Declared here, with the other handler
// prototypes, because the dispatcher calls them well above their definitions.
int64_t sys_inst_enum(void *ubuf, int max, int usize);
int64_t sys_inst_install(int kind, int index);

int64_t sys_user_create_pw(const char *username, const char *password,
                           uint32_t uid, uint32_t gid, const char *home);

// (#745) The password policy, readable from Ring 3 WITHOUT setting anything.
// The wizard could previously accept a password the kernel then refused at the
// last step of setup, because its client-side rule was a 6-character length
// test and the kernel's is eight rules plus a 50,000-entry breached-password
// table. Duplicating that table in every app is not an option, and a copy of
// the RULES would drift, so the app asks the kernel the same question the
// kernel will answer later.
//
// It is a courtesy for immediate feedback and NOT an enforcement point: the
// kernel still applies the policy at user_set_password() regardless of whether
// anyone called this. Returns PW_OK..PW_ERR_LAST (>= 0), or -14 for an
// unreadable pointer. `username` may be NULL, which skips the
// contains-username rule.
int64_t sys_pw_check(const char *username, const char *password);

// (#745) First-boot provisioning, as ONE call. Creates the interactive account
// and gives `root` its own, DIFFERENT password. Root only. Returns the new
// account's uid, or a negative code: -1 generic, -14 bad pointer, PW_RC(code)
// if the ACCOUNT password was refused, PW_RC_ROOT(code) if root's was (which
// includes PW_ERR_SAME_AS_OTHER when the two are identical).
int64_t sys_firstboot_admin(const char *username, const char *user_password,
                            const char *root_password);

int64_t sys_adduser(const char *username, uint32_t uid, uint32_t gid,
                    const char *home, const char *shell);
int64_t sys_set_theme(int theme_id);
int64_t sys_get_theme(void);
int64_t sys_set_volume(int volume);
int64_t sys_get_volume(void);
int64_t sys_set_mute(int mute);
int64_t sys_get_disk_total(void);
int64_t sys_get_disk_free(void);
int64_t sys_set_mouse_speed(int speed);
int64_t sys_get_mouse_speed(void);
int64_t sys_get_rtc_time(void);
int64_t sys_get_rtc_date(void);

// Assembly functions
extern void syscall_set_kernel_stack(uint64_t stack_ptr);

// Network info structure (used by SYS_GET_NET_INFO)
typedef struct {
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char mac[18];
    int  connected;
} net_info_t;

int64_t sys_set_rtc_time(uint64_t packed);
int64_t sys_set_rtc_date(uint64_t packed);
int64_t sys_get_net_info(void *buf, uint64_t len);
int64_t sys_net_set_static(const char *ip, const char *mask, const char *gw);
int64_t sys_ntp_sync(void);
int64_t sys_ntp_sync_server(const char *userver, uint32_t timeout_ms);

#define SYS_PROC_LIST 238  // Task Manager: snapshot process table
#define SYS_HTTP_POST 239
// #711: GraphFS journal integrity check. READ-ONLY: Ring 3 can verify the
// audit trail but has no append syscall at all, by design (an audit trail
// userland can write is not an audit trail).
#define SYS_USER_CREATE_PW 362  // (#745) create an account AND set its password atomically (root only)
#define SYS_PW_CHECK       369  // (#745) PURE policy check: (username|NULL, password) -> PW_* code, 0..PW_ERR_LAST
#define SYS_FIRSTBOOT_ADMIN 370 // (#745) first boot: create the human account AND set root's OWN password (root only)
// (themes ticket, 2026-08-07) how many fg/bg pairs the runtime contrast floor
// (kernel/gui/themes.c theme_ensure_all_contrast()/theme_ensure_v2_contrast())
// had to force-correct the LAST time this theme index was parsed. 0 = clean
// file. (int theme_id, theme_id<0 = current) -> int32 count. The userland
// loader (gui_theme.c) checks this right after SYS_THEME_LOAD_FILE so a
// downloaded/edited theme that needed fixing is visible to the user, not
// only on serial.
#define SYS_THEME_CONTRAST_CORRECTIONS 364

// (#306) Install-to-disk, exposed to Ring 3 so /APPS/INSTALL can drive it.
// SYS_INST_ENUM  (buf, max)     -> count, fills buf with inst_target_t
// SYS_INST_INSTALL(kind, index) -> 0 on success, negative installer rc
// INSTALL IS DESTRUCTIVE AND ROOT-ONLY. It deliberately does NOT take a
// caller-supplied descriptor: see sys_inst_install().
#define SYS_INST_ENUM                  365
#define SYS_INST_INSTALL               366
#define SYS_GFS_VERIFY 360  // HTTPS POST (url, headers, body, buf, cap, &status) for LLM/REST clients
// (#711 slice 2) gfs_query(cmd, a1, a2, out, out_bytes) -> bytes written.
// READ-ONLY, by design: docs/GRAPHFS_DESIGN.md section 8. Ring 3 has NO append
// syscall at all, because an audit trail userland can write is not an audit
// trail, and a contract an app can issue to itself is not a contract. Every
// mutation reaches the journal because a kernel subsystem decided it should.
#define SYS_GFS_QUERY 363
int64_t sys_http_post(const char *url, const char *headers, const char *body,
                      char *ubuf, uint32_t max_len, int *ustatus);

// #318 network printing
int64_t sys_print_list(void *out, int max);
int64_t sys_print_job(const char *printer, const char *title, const char *text);
int64_t sys_print_add(const char *name, const char *host, int port,
                      const char *queue, int make_default);
int64_t sys_print_remove(const char *name);
int64_t sys_print_image(const char *printer, const char *path);
int64_t sys_print_screen(const char *printer);
int64_t sys_bootlog_write(const char *msg);

#endif // SYSCALL_H
