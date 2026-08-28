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
// #120: RECLAIMED. #745 deleted an unimplemented SYS_FSTAT declaration from
// this number and wrote down that a real kernel fstat should take it back and
// REPLACE userland's seek-and-fabricate, not sit beside it. That is what 101 is
// now. Numbered here rather than appended at the end so it stays next to the
// syscall it shares its implementation with.
#define SYS_FSTAT          101  // Get file status by descriptor
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

// SYS_MPROTECT - change page protection on a mapped range. (#404 / #522)
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly.
// 23 is taken DELIBERATELY as a GAP, not as the top of the range. Swept
// 2026-08-23 across every ref in refs/heads + refs/remotes (200+ refs, via
// `git show <ref>:kernel/proc/syscall.h` and the libc twin): 23 is defined
// nowhere, on any branch, and has never even been an old SYS_MAX value (the
// lowest sentinel anywhere in history is 215). It sits immediately after its
// own family - SYS_BRK 20, SYS_MMAP 21, SYS_MUNMAP 22 - which is where a
// reader will look for it.
//
// Taking a gap rather than the sentinel's value is the tree's written policy
// and it is a direct product of the #238 collision: two agents each gap-scanned
// honestly, both took the TOP of the range, and both were right when they
// looked. A number deep inside the range cannot be reached by a concurrent
// top-scanning agent. SYS_MAX is deliberately NOT bumped: 23 is far below it
// and 400 remains a correct sentinel.
//
// Note 23 is also the number userland/apps/mmtest/main.c has assumed since
// #522 wrote that harness against a kernel that did not yet have the syscall,
// so adopting it makes an existing (previously no-op) call site correct.
#define SYS_MPROTECT        23  // Change protection of a mapped range

// mprotect refusal codes. Ring 3 sees these as the syscall's return value.
// They are DISTINCT on purpose: a single -1 for every refusal cannot tell a
// test which guard fired, and a guard that has never been watched firing is
// indistinguishable from one that is absent. /APPS/MMTEST subtest (8) asserts
// each code individually. Mirrored in kernel/rustkern/mprotect.rs (the MP_E_*
// consts) and locked against drift by proc/syscall_mprotect_lock.c.
#define MP_OK               0   // accepted
#define MP_E_PROT_BITS    (-1)  // prot carries bits that are not PROT_READ/WRITE/EXEC
#define MP_E_LEN          (-2)  // zero length
#define MP_E_ALIGN        (-3)  // addr is not page-aligned
#define MP_E_WX           (-4)  // W^X: PROT_WRITE|PROT_EXEC refused
#define MP_E_OVERFLOW     (-5)  // addr+len wraps the address space
#define MP_E_RANGE        (-6)  // range is not entirely user memory
#define MP_E_NOMAP        (-7)  // well-formed, but the range is not fully mapped

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
#define SYS_WIN_SET_NOCHROME_BG 398  // #216: same, but does not grab keyboard focus
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
// ---------------------------------------------------------------------------
// #238 THE PACKET FILTER.
//
//   SYS_NET_FW(int op, fw_xfer_t *xfer) -> 0, or negative on refusal
//
// ONE transfer struct for EVERY op (fw_xfer_t = fw_config_t + fw_stats_t,
// net/firewall.h). That is deliberate: an op-dependent pointer shape cannot be
// described by a single argtab.rs Desc, and a syscall whose user-pointer
// contract changes with an argument is one the pointer validator has to be
// told about twice. One fixed 240-byte read-write buffer, always.
//
// Every op fills the WHOLE struct from the live kernel state on the way out,
// so a caller always sees what is ACTUALLY in force, not what it asked for.
// That read-back is the same discipline the #233 contract API applies, and for
// the same reason: a setter that echoes its input cannot report being ignored.
//
// FW_OP_GET is unprivileged (knowing the policy is not a capability). The
// three mutating ops require euid 0.
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly:
// 399, with SYS_MAX bumped to 400 (the #216 precedent, four lines below the
// old sentinel note). THIS NUMBER MOVED ONCE, and the reason is the whole
// lesson: #238 first took 282 as "the bottom of the unused 282-289 block",
// having checked every branch and found it free. Concurrently #236 took 282
// by the same reasoning, with its own written proof that 282-289 was free on
// any branch. Both proofs were true WHEN WRITTEN and both were stale by the
// time either merged. syscall-number-lint caught it, but only on the first
// build AFTER the merge - and #238 was interrupted between merging dev and
// rebuilding, so the collision sat in a committed branch looking green.
// #236 was already in dev, so #238 is the one that moved.
// A FREE-NUMBER CHECK IS ONLY VALID AT MERGE TIME. Rebuild after every merge.
#define SYS_NET_FW                     399

// ---------------------------------------------------------------------------
// #221 phase 0. SYS_KEY_MODS() -> the LIVE physical modifier bitmask
// (KEY_MOD_SHIFT/CTRL/ALT/CAPS, kernel/drivers/keymod.h), the same state
// cpu/isr.c uses to fold case and to turn Ctrl+letter into a control
// character. No arguments, no pointers, cannot fail.
//
// WHY A USERLAND APP NEEDS THIS AT ALL, given that Shift/Ctrl/Alt press AND
// release already arrive as ordinary keycoded events (MEASURED, see
// tools/testing/probes/keyprobe.c): tracking those events is EXACT while the
// window has focus, and that is the whole shortcut case. It goes wrong in
// exactly one way. The kernel emits NO focus/blur event to an app - grep
// EVENT_WINDOW_BLUR across kernel/, there is one hit and it is the enum
// declaration - so if the user holds Shift, clicks another window, and
// releases it there, the first window never sees the release and believes
// Shift is held forever. There is no event that can tell it otherwise.
// libc/gui_mods.c resyncs off this call at a quiescent moment (its own event
// queue empty), which is the only moment at which a LIVE read cannot
// contradict an event still sitting in that queue.
//
// This is not new state. keyboard_get_modifiers() has existed in cpu/isr.c
// since the driver was written and had exactly one consumer, the DOS INT 16h
// shim. This exposes it; it computes nothing.
//
// CAPS LOCK is included because it is otherwise INVISIBLE to Ring 3: isr.c
// toggles caps_lock and pushes NO key event for it, so no amount of event
// tracking can ever recover it, and without it "the character arrived
// uppercase" does not imply "Shift was held".
//
// NUMBER CHOICE: 400. It WAS the SYS_MAX sentinel value when this was
// written, and by the time it merged it was a GAP: the cross-window drag work
// landed 401-406 concurrently and left 400 unused, so SYS_MAX is 407 and needed
// no bump here. That near-miss is the #238 lesson repeating. Read the
// #238 note above before assuming a number is free: a free-number check is
// only valid AT MERGE TIME, so rebuild after every merge.
//   SYS_KEY_MODS(void) -> uint32 bitmask (never negative)
#define SYS_KEY_MODS                   400
#define FW_OP_GET       0   // read policy + counters
#define FW_OP_SET       1   // install xfer.cfg, persist it, read back
#define FW_OP_RESET     2   // zero the counters, read back
#define FW_OP_RELOAD    3   // re-read /CONFIG/FWRULES.CFG, install, read back

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
// #182 (AUDIO 2/3): drain the DOS guest's OPL2 register writes to the Ring-3
// FM synthesiser.
//
//   SYS_DOS_FM_EVENTS(dos_fm_event_t *buf, uint32_t max_events)
//     ->  n >= 0   events copied
//     ->  -5       EPERM: another process already latched the queue
//     ->  -6       ENODEV: no DOS guest holds the chip AND the queue is empty,
//                  i.e. "you may stop now". This is how /APPS/FMSYNTH exits.
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly.
// 377 is a GAP, not the top of the range. 374, 375 and 376 are taken
// (SYS_SET_LOGIN_MODE, SYS_GET_LOGIN_MODE, SYS_WIN_SET_SHADOW) and SYS_MAX is
// 397. Checked with `git show <branch>:kernel/proc/syscall.h` across every ref
// in refs/heads on 2026-08-20: 377 appears on seven agent branches ONLY as an
// old value of the SYS_MAX sentinel, never as a real syscall.
//
// Taking a gap rather than the top is deliberate: a concurrent agent
// gap-scanning for a free number takes the TOP of the range, and 373 was
// allocated twice exactly that way (see kernel/tools/syscall-number-lint).
// SYS_MAX is NOT bumped: 377 is below it and it remains a correct sentinel.
//
// THE DRAIN IS NON-BLOCKING AND THAT IS NOT AN OVERSIGHT (#426). The consumer
// is paced by the AUDIO SINK: it blocks in sys_audio_pcm_write's wait queue,
// which sleeps until the pump has consumed frames, and drains this queue once
// per audio block on the way round. Making this call block would DEADLOCK the
// design, because a sustaining note must keep producing samples with no
// register writes arriving at all; a consumer asleep on the event queue would
// stop feeding the sink and the note would cut off.
// ===========================================================================
#define SYS_DOS_FM_EVENTS              377

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

// ===========================================================================
// #745 (local 102): display rotation (video/framebuffer.h fb_rotation_t).
// Reboot-to-apply (see the Settings Display panel hint) - these do NOT touch
// the running session's fb_rotation, they persist the CHOICE for the next
// boot's fb_init() to read. No pointer args, so no rustkern/argtab.rs entry
// needed (same posture as SYS_SET_DISPLAY_FX/SYS_SET_WALLPAPER above).
//   SYS_SET_ROTATION(int value)  -> 0 on success, -1 if value not in 0..3
//                                    (0=none 1=90cw 2=180 3=270cw)
//   SYS_GET_ROTATION()           -> the ACTIVE (this-session) rotation, 0..3
// ===========================================================================
#define SYS_SET_ROTATION               384
#define SYS_GET_ROTATION               385

// (#745 local 109) ftruncate(int fd, long length) -> 0 / -1. Shrink only; a
// grow is refused rather than silently ignored. See sys_ftruncate in
// proc/fdlayer.c for why a real one had to exist.
#define SYS_FTRUNCATE                  386
// #115 (local 120): utime(2). (const char *path, int64_t atime, int64_t mtime)
// Times are seconds since the UNIX epoch. Two sentinels, both deliberate:
//   UTIME_KEEP (-1)  leave this timestamp unchanged.
//   UTIME_NOW  (-2)  set it from the KERNEL's wall clock (cpu/wallclock.h).
// UTIME_NOW exists because userland has no correct clock to send: time() and
// gettimeofday() return SECONDS SINCE BOOT (#113). A `touch` that sent its own
// time() would stamp files with 1970-01-01 plus the uptime, which is precisely
// the plausible-looking wrong value this ticket is about. So the caller asks
// for "now" and the kernel, which does have a calendar, decides what now is.
// That is why #115 does NOT have to wait for #113.
#define SYS_UTIME                      387

// perf62 (#62 revalidation): TSC-backed monotonic microseconds since boot
// (cpu/mono.h mono_us(), NOT timer_ticks). Exists because SYS_UPTIME_MS and
// SYS_GET_TICKS are BOTH derived from timer_ticks, and blame.md's own
// "timer-ticks-is-not-a-wall-clock" entry records that KVM replays a
// starved vCPU's lost tick IRQs in BURSTS, so a tick-derived interval can
// misreport how much real time actually passed during a stall - exactly the
// failure mode a frame-interval instrument exists to catch, not fall for.
// Zero args, read-only, no argtab.rs entry needed: a descriptor-less syscall
// is simply not pointer-validated, which is correct here (nothing to
// validate).
#define SYS_MONO_US                     388

// #158 NATIVE FULLSCREEN. ENTER acts on the CALLER'S OWN focused window only
// (kernel checks owner_pid == proc_current()->pid against wm_state.focused_
// window - a background/unfocused process cannot claim or retain the screen).
// EXIT is unconditional and safe from anywhere (no-op if nothing is
// fullscreen) - it is the escape hatch F11 and the watchdog both call.
// RENDER is the compositor's per-frame fast path: blits the fullscreen
// window's content_presented straight into the back buffer with none of the
// per-window chrome/corner/widget work SYS_COMPOSITOR_RENDER_WINDOWS does;
// returns -1 (compositor must fall back to a normal composite) if nothing
// is validly fullscreen right now. STATUS is a cheap liveness probe for the
// compositor's watchdog: packs (window id << 32) | content commit sequence,
// or -1 if nothing is fullscreen - an unchanging sequence over wall-clock
// time is a wedged app, exactly #157's g_fb_flip_count discriminator shape.
#define SYS_WM_FULLSCREEN_ENTER         389
#define SYS_WM_FULLSCREEN_EXIT          390
#define SYS_WM_FULLSCREEN_RENDER        391
#define SYS_WM_FULLSCREEN_STATUS        392

// #148 (local 164, 2026-08-18): a window created WITHOUT taking keyboard
// focus. Before this, sys_win_create() (SYS_WIN_CREATE) unconditionally
// called wm_focus_window(win) at the end - every window creation stole
// focus, with no way to opt out. That is a real gap, not a theoretical one:
// PrintScreen on a normal desktop is specified to open Maytera Snap showing
// the new capture WITHOUT moving focus away from whatever the user was
// typing into (the snipping-tool / macOS-screenshot pattern). Same args,
// same geometry contract as SYS_WIN_CREATE (arg1 title, arg2/3 x/y, arg4/5
// w/h) - only the focus behavior differs, so a caller converts by changing
// the syscall number and nothing else. See kernel/gui/window.c
// window_create_ex() / sys_win_create_bg() and userland/libc/syscall.h
// win_create_bg().
#define SYS_WIN_CREATE_BG               393

// #162 (2026-08-19): one packed read of the system volume state, so the
// compositor learns level, mute and BOTH change counters in a single syscall
// per frame instead of three. Returns
//   bits  0..7  level 0-100 | bit 8 muted | bits 16..31 seq | bits 32..47 keyseq
// where `seq` changes on ANY volume change (tray slider, Settings, media key)
// and `keyseq` changes only on a MEDIA-KEY change. The compositor mirrors its
// tray gauge off seq and shows the volume OSD off keyseq, so dragging the tray
// slider does not pop an OSD on top of the slider the user is looking at.
// Scalar-only: no user pointer, so no rustkern/argtab.rs descriptor is needed
// (same as SYS_WM_SET_WORK_AREA / SYS_SET_ROTATION). See drivers/sysvol.h.
#define SYS_VOL_STATE                   395

// #113: epoch MICROSECONDS (UTC), from cpu/wallclock.h realtime_us_rs().
// Exists because gettimeofday() needs SUB-SECOND resolution and SYS_TIME can
// only carry whole seconds. Both come from ONE anchor, so the two fields of a
// gettimeofday() can never disagree about which second it is - the exact
// desync #594 had to repair when tv_sec and tv_usec came from two counters.
// Returns 0 if the RTC has never presented a plausible date; 0 is "unknown",
// not 1970. Zero args, read-only, no argtab entry needed.
#define SYS_REALTIME_US                 396

// ============================================================================
// #112: SPAWN WITH AN ENVIRONMENT.
//
// Until this ticket MayteraOS had NO cross-process environment. The two spawn
// syscalls above carry a path, argv and argc and nothing else, and
// proc_create_user_as() met its own envp parameter with "(void)envp;", so
// every process started with an empty `environ` no matter what its parent had
// set. /APPS/ENV refused NAME=VALUE for exactly that reason and said so.
//
// WHY A NEW NUMBER AND NOT AN EXTRA ARGUMENT ON 247. SYS_SPAWN_REDIR already
// uses all six argument registers (path, argv, argc, infile, outfile, append).
// An environment is a seventh operand, so it cannot be added there, and
// widening the ABI is not an option.
//
// WHY A REQUEST STRUCT AND NOT A SEVENTH REGISTER. There is no seventh
// register. Passing one pointer to a fixed-size, version-locked struct means
// the rustkern/argtab.rs descriptor is a single fixed-length validation, and
// the next operand this call needs (a file-actions array, see spawn.h's
// SPAWN_FDACT note) costs a reserved field rather than another syscall number.
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly. 394
// is a GAP, not the top of the range: it was SYS_MAX's own value before
// #162/#148 pushed the sentinel to 397, and no branch in the repository -
// checked with `git show <branch>:kernel/proc/syscall.h` across every
// agent/* ref on 2026-08-20 - defines a real syscall at 394. Taking a gap
// rather than the sentinel's value is deliberate: a concurrent agent
// gap-scanning for a free number takes the TOP of the range, and 373 was
// allocated twice that way (see kernel/tools/syscall-number-lint).
//
// SYS_MAX is deliberately NOT bumped: 394 is below it and it is still a
// correct sentinel.
//
// envc SEMANTICS, and they are not the same request:
//   envc <  0                "no environment supplied": the child gets the
//                            kernel default block (rustkern/envblock.rs).
//   envc == 0                a deliberately EMPTY environment. This is what
//                            `env -i` means and it is honoured as such.
//   envc >  0                envp points at envc "NAME=VALUE" strings.
// An entry that is not NAME=VALUE REFUSES the spawn; it is never dropped.
// ============================================================================
typedef struct {
    const char  *path;      // required, absolute or cwd-relative
    char       **argv;      // argc entries; may be NULL when argc == 0
    int32_t      argc;
    int32_t      envc;      // see the envc semantics above
    char       **envp;      // envc entries of "NAME=VALUE"
    const char  *infile;    // NULL = no stdin redirect
    const char  *outfile;   // NULL = no stdout redirect
    int32_t      append;    // outfile: append rather than truncate
    int32_t      reserved;  // MUST be 0; the next operand goes here
} sc_spawn_req_t;
#define SYS_SPAWN_ENV                   394

// ============================================================================
// #236: push the live "Double-click Speed" setting (Settings > Mouse,
// SETTINGS.CFG 'k' key, read by userland/libc/settingscfg.c) into the ONE
// kernel-side double-click detector, gui/window.c's title-bar
// maximize/restore toggle. Before this the kernel used a hardcoded ~0.5s
// (freq/2) with zero knowledge of the userland preference - one of THREE
// independent hardcoded double-click detectors found on golden 2016 (the
// other two were userland: compositor/main.c's dead fallback path and
// compositor/desktop.c's real desktop-icon path, both fixed the same ticket
// by reading settingscfg_dblclick_ms() directly). This is precedented by
// SYS_SET_MOUSE_SPEED (kernel/proc/syscall.h #133/#140): a userland-owned
// preference pushed into a kernel global via a one-way setter syscall, not a
// kernel-side file read (Ring 0 has no notion of the session's home
// directory the way userconf.c's per-user path resolution does).
//
//   SYS_SET_DBLCLICK_MS(int ms) -> 0 always (kernel clamps ms to a sane
//   [100, 3000] range rather than refusing; a caller supplying garbage gets a
//   clamped detector, not an error to handle).
//
// Called from userland/apps/compositor/main.c's dbl_click_threshold_push(),
// once per frame but gated on an actual change, so this is a rare syscall in
// practice (SETTINGS.CFG itself is read on a 2s throttle).
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly. 282
// is a GAP: checked with `git show <ref>:kernel/proc/syscall.h` across every
// ref in refs/heads on 2026-08-22, nothing in [282,289] is defined as a real
// syscall on any branch. SYS_MAX is NOT bumped: 282 is well below 399.
#define SYS_SET_DBLCLICK_MS             282
int64_t sys_set_dblclick_ms(int ms);

// ============================================================================
// #229 FIRST-RUN (OOBE) STATE. THE chokepoint; see kernel/rustkern/firstrun.rs
// for the full argument, including why relaxing /CONFIG was the wrong answer.
//
//   SYS_FIRSTRUN(int op) -> per-op result below, or a negative refusal
//
// The first-boot wizard is a Ring-3 app running as the session user (uid 1000
// since #226 removed autologin=root). Every durable thing it did was an
// open(O_CREAT) under /CONFIG, which is root-owned 0711 because it holds
// SHADOW, AUTHKEYS, SSHD.CFG and the owner's API keys. Measured on golden
// 2011: all four writes were refused, the wizard could not complete, and - the
// part that made it a dead end - neither could its escape hatch, because
// "Skip to Desktop" was itself one of those writes.
//
// So the wizard stops writing /CONFIG and asks the kernel instead, exactly as
// it already does for its ONE mandatory step (SYS_FIRSTBOOT_ADMIN) and exactly
// as /CONFIG/LOGIN.CFG is already written from Ring 0 behind
// login_cfg_authorize(). The set of legal keys is fixed HERE and there is no
// path from this call to any other name in that directory.
//
// TWO OF THE FOUR WERE NEVER CONFIGURATION. SETUPSKIP and SETUPNEW are one-way
// wizard -> compositor signals meaningful for exactly one boot; storing them on
// persistent media manufactured a stale-marker bug twice (#136, #203), and the
// unlink-based cleanups added to fix those were themselves /CONFIG writes and
// so failed on the same session. They are now two bits of kernel state that
// start clear because .bss starts clear. The class stops existing.
//
// NOT INCLUDED: /CONFIG/NETIP.CFG, the fourth refused write. The Network page
// is compile-disabled, so an op for it would ship with zero callers. Re-enable
// the page and add FR_SET_NETIP with a dotted-quad validator; do not reach for
// open() again.
//
// NUMBER CHOICE, stated because a guessed constant does not fail loudly. 397 is
// a GAP, not the top of the range: SYS_WIN_SET_NOCHROME_BG is 398 and SYS_MAX
// is 399. Checked with `git show <ref>:kernel/proc/syscall.h` across every ref
// in refs/heads on 2026-08-22: 397 appears on eighteen agent branches ONLY as
// an old value of the SYS_MAX sentinel, never as a real syscall. Taking a gap
// rather than the top is deliberate: a concurrent agent gap-scanning for a free
// number takes the TOP of the range, and 373 was allocated twice exactly that
// way (see kernel/tools/syscall-number-lint). SYS_MAX is NOT bumped.
//
// AUTHORIZATION. FR_MARK_DONE is the only op that touches a disk and is gated
// on a fact the kernel checks rather than a claim the caller makes: the machine
// must already have at least one active account, which is precisely what the
// marker asserts. HONEST LIMIT: on a machine that has accounts, any Ring-3
// process may suppress the first-run wizard with it. That is a nuisance, not a
// privilege boundary, and it is a far smaller surface than write access to the
// directory holding SHADOW. Every call is logged with the caller's uid. The
// per-boot ops are ungated: the SKIP bit's whole job is to be an escape hatch
// that CANNOT FAIL, and a policy in front of it is how it acquires a failure
// mode.
// ============================================================================
#define SYS_FIRSTRUN                    397

// Ops. MIRRORED in kernel/rustkern/firstrun.rs, userland/libc/syscall.h and the
// private const block of userland/apps/setup/main.rs (a no_std app cannot
// include this header, which is how a fifth copy went stale once before; see
// kernel/tools/syscall-number-lint rule 5). firstrun_selftest_rs() asserts
// these numeric values on every boot.
#define FR_MARK_DONE       0  // durable: write /CONFIG/SETUPDONE. -> 0, or -2 if no account exists
#define FR_SKIP_SET        1  // per-boot: the wizard escaped to the desktop. -> 0, always
#define FR_SKIP_GET        2  // per-boot: -> 1 if escaped this boot, else 0. Non-consuming
#define FR_SKIP_CLEAR      3  // per-boot: arming a fresh first run (compositor). -> 0
#define FR_HANDOVER_SET    4  // per-boot: the machine just changed hands. -> 0
#define FR_HANDOVER_TAKE   5  // per-boot: CONSUMING read of the above. -> 1 once, then 0
// #OOBEAUTH (2026-08-23): -> 1 iff THIS caller may currently call
// SYS_FIRSTBOOT_ADMIN as a non-root bootstrap session (a direct child of the
// compositor, account table still empty), else 0. Never a refusal: the
// wizard uses this to decide whether to SHOW its account page at all, and a
// page that is never shown cannot report a failure. See
// firstboot_bootstrap_ok_rs() in rustkern/firstrun.rs for the predicate and
// sys_firstboot_admin() in proc/syscall.c for the ONE OTHER place it is
// evaluated - the two must never disagree.
#define FR_BOOTSTRAP_QUERY 6


// ===========================================================================
// #250 REMOVABLE VOLUMES (USB hotplug), 283 / 284.
//
// The kernel has auto-mounted a hot-plugged USB stick since #418. Nothing in
// Ring 3 could SEE that it had: the shipping shell and file manager are
// userland processes, and there was no syscall carrying the volume list, so
// the drive mounted in silence with no sidebar row, no desktop icon and no
// way to eject it. These two are that missing surface.
//
// Numbers 283 and 284 are taken from the 283-289 GAP below SYS_MAX (400), not
// from the top of the range, per the SYS_MAX-is-a-sentinel rule above.
// kernel/tools/syscall-number-lint enforces that these agree with
// userland/libc/syscall.h and with the Rust argument table.
#define SYS_VOL_LIST                   283  // (sc_volume_t *buf, int max) -> count written, or -1
#define SYS_VOL_EJECT                  284  // (int index) -> 0, or -1

// Volume record. MUST match sc_volume_t in userland/libc/syscall.h and
// ScVolume in kernel/rustkern/hotplug.rs; the size is locked by
// _Static_assert in proc/syscall.c.
typedef struct {
    int32_t  index;          // opaque handle: pass to SYS_VOL_EJECT
    uint32_t flags;          // MOSVOL_*
    uint32_t fs_type;        // informational; use fsname for display
    uint32_t pad;
    uint64_t total_bytes;
    uint64_t free_bytes;     // meaningless when MOSVOL_FREE_UNKNOWN is set
    char     name[64];       // "SanDisk Cruzer Blade"
    char     mount[32];      // "/USB0" - a real path, browsable when READABLE
    char     fsname[8];      // "FAT32", "exFAT", ...
} sc_volume_t;

#define MOSVOL_MOUNTED       0x01  // filesystem is mounted
#define MOSVOL_REMOVABLE     0x02  // always set today; every entry is USB
// Files on this volume can actually be OPENED AND READ. Clear means the
// filesystem is recognised and mounted but its file operations are not
// implemented (exFAT: fs/exfat.c has mount/unmount/free-space and nothing
// else). A UI must NOT offer such a volume as browsable; it should say why.
#define MOSVOL_READABLE      0x04
// free_bytes is not meaningful. Set on removable FAT volumes, where the
// whole-FAT free scan is deliberately skipped so that plugging a large stick
// in does not cost thousands of uncached SCSI reads before it appears.
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

// #234i: how many records SYS_VOL_LIST can ever write. Two producers now feed
// it: up to HOTPLUG_MAX_DEVICES (8) physical USB volumes and up to
// DISKIMG_MAX_MOUNTS (8) mounted disk images. A _Static_assert in
// proc/syscall.c ties this to both, and rustkern/argtab.rs's CAP_VOLUMES must
// match, or the pointer validator proves fewer bytes than the handler writes.
#define SC_VOL_MAX           16
// #234i: the index namespace split. index < SC_VOL_IMAGE_BASE is a USB
// hot-plug slot; index >= it is SC_VOL_IMAGE_BASE + drive-letter index for a
// mounted disk image. Mirrored as VOL_INDEX_IMAGE_BASE in
// rustkern/hotplug.rs, which is what STAMPS the index.
#define SC_VOL_IMAGE_BASE    1000

// ---------------------------------------------------------------------------
// #221 terminal notifications phase 1. SYS_WIN_GET_STATE(handle) -> the
// WIN_STATE_* bitmask (userland/libc/syscall.h) for the CALLER'S OWN window.
//
// WHAT IT CLOSES. An app could not discover that it was minimized, and could
// not discover that it was unfocused either. This kernel emits no focus, blur
// or minimize event to an app - EVENT_WINDOW_FOCUS and EVENT_WINDOW_BLUR
// appear exactly once each in the whole tree and it is the enum declaration,
// which the SYS_KEY_MODS note above already had to work around for the same
// reason. sys_wm_get_windows() does report `minimized` per window, but an app
// cannot pick its own row out of that list: sys_win_create() returns the
// user_windows[] SLOT INDEX and wm_window_info_t.id is the window manager's
// window id, and nothing maps one to the other. So a window's state was
// legible to the compositor, the taskbar and any app that enumerates windows,
// and illegible to the one process that owns it.
//
// SCOPE, deliberately narrow: `handle` is validated against the caller's own
// user_windows[] slot exactly as SYS_WIN_GET_SIZE validates it, so this tells
// a caller nothing about anybody else's windows that wm_get_windows() did not
// already tell everybody. No pointer arguments (nothing for argtab.rs to
// describe), no allocation, no lock, cannot block. Returns -1 for a handle
// that is not a live window.
//
// NUMBER CHOICE: 407, which was the SYS_MAX SENTINEL's own value and is
// therefore the first genuinely unallocated number, with SYS_MAX bumped to
// 408 in the same edit. Read the #238 note above before assuming that is
// still true when you read this: a free-number check is only valid AT MERGE
// TIME, so this was re-run through tools/syscall-number-lint after the rebase
// onto dev and before the build.
//   SYS_WIN_GET_STATE(int handle) -> WIN_STATE_* bitmask, or -1
#define SYS_WIN_GET_STATE              407

// (#786) Set the LIVE DNS resolver and persist it to /CONFIG/NETIP.CFG in the
// same call. Body is net_set_dns_rs() in rustkern/netstat.rs, which carries the
// full account of what was broken and why this is one syscall and not two.
//
// NO POINTER ARGUMENTS: the address arrives as a HOST-ORDER u32
// ((a<<24)|(b<<16)|(c<<8)|d), parsed by the caller, so there is nothing for
// rustkern/argtab.rs to describe and nothing for syscall_validate_args() to
// check. Userland mirror + the dotted-quad parser: net_set_dns() in
// userland/libc/syscall.h.
//
//   SYS_NET_SET_DNS(uint32 dns_host_order)
//     ->  0  applied live AND saved
//     -> -2  not a usable resolver address (0, 255.255.255.255, 0.x, 127.x)
//     -> -3  applied LIVE but could NOT be saved: it will revert on reboot.
//            This is a REPORTABLE outcome, not a rounding error; the caller
//            must say so rather than showing plain success.
//
// NUMBER CHOICE: 408, which was the SYS_MAX SENTINEL's own value and is
// therefore the first genuinely unallocated number, with SYS_MAX bumped to 409
// in the same commit (the #216/#221 precedent immediately above). Re-check this
// after any merge: a free-number check is only valid AT MERGE TIME.
#define SYS_NET_SET_DNS                408

// The GLOBAL UI SCALE FACTOR (gui/uiscale.h, rustkern/uiscale.rs).
// ONE number with an opcode rather than eight syscall numbers: these are all
// facets of one small display setting, and the syscall number space is a
// scarce shared resource that this file already warns about above.
// NUMBER CHOICE: 409, the SYS_MAX sentinel's own value and therefore the first
// genuinely unallocated number, with SYS_MAX bumped to 410 in the same edit.
#define SYS_UI_SCALE                   409  // (op, arg) -> int; see UISC_* below
#define UISC_GET     0   // -> live percent (100 = 1x)
#define UISC_SET     1   // arg = percent -> percent ACTUALLY ADOPTED after clamping
#define UISC_AUTO    2   // -> what auto-detection said for this display
#define UISC_MAX     3   // -> the largest percent this framebuffer can carry
#define UISC_SRC     4   // -> UI_SRC_* : default / auto / config / user
#define UISC_GEN     5   // -> generation counter, bumped on every change
#define UISC_SAVE    6   // persist the live value to /CONFIG/DISPLAY.CFG; 0 = ok
#define UISC_LAPTOP  7   // -> 1 battery declared, 0 none, -1 could not ask
// ORACLE OPCODES. The arithmetic has to exist in Ring 3 as well: the
// compositor scales its own chrome and cannot afford a syscall per coordinate.
// That is a second copy of a formula, which is the exact fault this project
// keeps paying for, so these three exist to make the copy CHECKABLE instead of
// merely promised: userland computes a range of values locally, asks the
// kernel for the same ones through these, and refuses to trust its fast path
// if they ever disagree. A duplicated formula with a live oracle is a
// different thing from a duplicated formula with a comment.
#define UISC_PX      8   // arg = logical  -> physical, by the LIVE factor
#define UISC_UNPX    9   // arg = physical -> logical,  by the LIVE factor
#define UISC_SPAN   10   // arg = (origin<<16)|extent (both 16-bit) -> scaled extent
#define UISC_NATIVE 11   // mark the CALLING process as thinking in real screen
                         // pixels (the compositor, and nothing else). Call it
                         // FIRST, before any SYS_FB_INFO: the framebuffer-owner
                         // backstop is not yet true at that point.
// The REAL display geometry, ((width << 16) | height), regardless of the
// caller's scale. SYS_FB_INFO deliberately answers a scale-transparent app in
// LOGICAL pixels, because that is the coordinate system it draws in - but
// Settings has to SHOW the user the resolution their panel is actually
// running at, and "1280 x 720" on a 1920x1080 display is a lie even though it
// is the right answer to a different question.
#define UISC_FBPHYS 12


// Control-method battery (#battmeter): percentage/state/time-remaining, for
// the tray meter and Settings. See drivers/battery.h and rustkern/battery.rs.
// One syscall with an opcode, following SYS_UI_SCALE's precedent (#409)
// rather than consuming several numbers for one small facet-group.
// NUMBER CHOICE: 410, the SYS_MAX sentinel's own value and therefore the
// first genuinely unallocated number, with SYS_MAX bumped to 411 in the
// same edit. Checked against both kernel/proc/syscall.h and
// userland/libc/syscall.h at the time of this change: neither used 410.
#define SYS_BATTERY                    410
#define BATT_PRESENT   0   // -> 1 battery declared, 0 none, -1 could not ask
#define BATT_PCT       1   // -> 0-100, or -1 unknown
#define BATT_STATE     2   // -> BATT_ST_* below, or 0 (unknown)
#define BATT_MINUTES   3   // -> minutes remaining, or -1 unknown
#define BATT_GEN       4   // -> generation counter, bumped on every change
#define BATT_ST_UNKNOWN     0
#define BATT_ST_DISCHARGING 1
#define BATT_ST_CHARGING    2
#define BATT_ST_FULL        3

// (#wizflash) The UNSCALED (1x, theme-file-native) counterpart of
// SYS_THEME_METRIC. theme_get_metric_by_id() (SYS_THEME_METRIC's handler)
// already multiplies every TM_PX metric by the current UI scale factor
// (kernel/gui/themes.c: "THE GLOBAL UI SCALE FACTOR IS APPLIED HERE AND ONLY
// HERE"), which is correct for a caller about to draw the real value onto the
// SCREEN. It is WRONG for gui_theme_win_preview() (userland/libc/gui_theme.c):
// that function draws a MINIATURE 1:1 window crop through the caller's own
// window, which is itself a scale_on window whose draw syscalls (win_draw_rect
// et al) ALREADY multiply every coordinate by the same scale factor at the
// window boundary. Feeding it an already-scaled metric compounds the two
// multiplies, so at 200% a title bar authored as 20px was measured coming out
// 80px (2x from SYS_THEME_METRIC, 2x again at the window boundary) instead of
// the correct 40px, overflowing the fixed preview crop. theme_get_metric_raw()
// (kernel/gui/themes.c) already existed for exactly this - its own comment
// names "the theme-preview thumbnail in Settings" as a caller that "would
// overflow" without it - but was never wired to a syscall, so no Ring 3 caller
// could actually reach it. This is that wiring: a thin passthrough to the
// existing, unchanged accessor, not new logic.
// NUMBER CHOICE: 412, the SYS_MAX sentinel's own value and therefore the first
// genuinely unallocated number, with SYS_MAX bumped to 413 in the same edit.
// Checked against both kernel/proc/syscall.h and userland/libc/syscall.h at
// the time of this change: neither used 412.
#define SYS_THEME_METRIC_RAW           412

// (#231r) THE 5-BAND GRAPHIC EQUALISER. One syscall with an opcode, the same
// shape as SYS_UI_SCALE and SYS_BATTERY above and for the same reason:
// several small facets of one feature, not several syscall numbers.
//
// WHY THIS EXISTS AT ALL. #231 (commit 8a5fcee5) removed the tray Sound
// panel's 5-band EQ, and its reason was exactly right: the faders wrote "a
// static int g_eq[5] that only the fader itself reads - no EQ syscall exists
// and it is not even persisted". THIS is that missing syscall. It reaches
// real per-band DSP (rustkern/pcmeq.rs, applied post-mix in
// drivers/audio_pcm.c's mix_render), so a fader that moves now changes the
// PCM the hardware receives. Restoring the faders WITHOUT this would rebuild
// the defect #231 deleted.
//
// NUMBER CHOICE: 413, the SYS_MAX sentinel's own value and therefore the
// first genuinely unallocated number, with SYS_MAX bumped to 414 in the same
// edit. Checked against BOTH kernel/proc/syscall.h and
// userland/libc/syscall.h at the time of this change: neither used 413.
// SYS_BATTERY=410 and SYS_THEME_METRIC_RAW=412 had both been added days
// earlier, so a guessed constant would have collided; a guessed syscall
// number does not fail loudly, it ships a control that renders, moves, and
// does nothing, which is this exact ticket.
#define SYS_AUDIO_EQ                   413
#define AEQ_BANDS     0   // ()          -> number of bands (5)
#define AEQ_GET       1   // (band)      -> fader 0..100 (50 = flat), -1 bad band
#define AEQ_SET       2   // (band, pos) -> 0, or -1 bad band. pos clamps to 0..100
#define AEQ_FREQ      3   // (band)      -> centre/corner frequency in Hz
#define AEQ_DB10      4   // (band)      -> gain in TENTHS of a dB, signed
#define AEQ_ACTIVE    5   // ()          -> 1 if any band is off flat
#define AEQ_RESET     6   // ()          -> every band back to flat
#define AEQ_LOG       7   // ()          -> write the current EQ to /AUDIOLOG.TXT
#define AEQ_SELFTEST  8   // ()          -> boot spectral self-test mask, 0 = pass
// Fader travel and gain range, so Ring 3 does not hardcode its own copy of
// the mapping the kernel actually applies (kernel/rustkern/pcmeq.rs owns it).
#define AEQ_POS_FLAT  50
#define AEQ_RANGE_DB10 120

#define SYS_MAX                        414  // SYS_AUDIO_EQ=413 is the new top, so the sentinel is 414

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
// (#182) Drain the DOS guest OPL2 register writes to the Ring-3 FM core.
int64_t sys_dos_fm_events(void *ubuf, uint32_t max_events);
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
int64_t sys_fstat(int fd, void *ubuf);   // #120
// #115: set access/modification time. -1 = keep, -2 = kernel's "now".
int64_t sys_utime(const char *u_path, int64_t atime, int64_t mtime);
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
// (#404) mprotect. Takes RAW uint64_t, never a pointer type, for the same
// reason sys_mmap/sys_munmap do: `addr` is an address to be looked up in a page
// table, NOT memory the kernel dereferences. Casting it to a pointer in the
// dispatcher would make syscall-ptr-lint discover a user pointer that does not
// exist and demand an argtab descriptor proving bytes readable, which is the
// wrong question for this call entirely.
int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot);

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
// #148 (local 164): see SYS_WIN_CREATE_BG above. Same signature, does not
// steal keyboard focus.
int64_t sys_win_create_bg(const char *title, int x, int y, int width, int height);
// (#745) see SYS_WM_SET_WORK_AREA above
int64_t sys_wm_set_work_area(int32_t left, int32_t top, int32_t right, int32_t bottom);
int64_t sys_win_set_nochrome(int handle);
int64_t sys_win_set_nochrome_bg(int handle);   // #216: nochrome without the focus grab
int64_t sys_win_destroy(int handle);
int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color);
int64_t sys_win_draw_text(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color);
int64_t sys_win_get_event(int handle, void *event_buf, int timeout);
int64_t sys_win_invalidate(int handle);
int64_t sys_wm_force_redraw_all(void);  // (#704)
int64_t sys_win_get_size(int handle, int *width, int *height);
// #221: the caller's OWN window's WIN_STATE_* bits. See SYS_WIN_GET_STATE above.
int64_t sys_win_get_state(int handle);

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

// #229 first-run state chokepoint. See SYS_FIRSTRUN above and
// kernel/rustkern/firstrun.rs. Takes no pointer, so there is nothing to bounce.
int64_t sys_firstrun(int op);

// #238 packet filter control. See SYS_NET_FW above and net/firewall.h.
int64_t sys_net_fw(int op, void *user);

int64_t sys_adduser(const char *username, uint32_t uid, uint32_t gid,
                    const char *home, const char *shell);
int64_t sys_set_theme(int theme_id);
int64_t sys_get_theme(void);
int64_t sys_set_volume(int volume);
int64_t sys_get_volume(void);
int64_t sys_set_mute(int mute);
int64_t sys_vol_state(void);   // #162
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

// Cross-window drag ("docking"). Defined in proc/syscall.c beside
// user_windows[], because each one proves the caller owns the window handle it
// is acting for.
int64_t sys_drag_begin(int win, unsigned int kind, const void *payload,
                       int plen, const char *label, int llen);
int64_t sys_drag_peek(void *out);
int64_t sys_drag_take(int win, void *dst, int cap);
int64_t sys_drag_accept(int win, unsigned int mask);
int64_t sys_drag_release(int x, int y);
int64_t sys_drag_end(void);


// ===========================================================================
// CROSS-WINDOW DRAG ("docking"): SYS_DRAG_* 401-406
// ===========================================================================
//
// The owner asked for Konsole-style docking: drag a terminal tab or pane out
// into its own window, and back into another one. That is not a terminal
// feature. It is a WINDOWING feature that did not exist: every drag in the
// tree (desktop icons, sticky notes, the desktop pet, widget panels, and the
// kernel WM's own title-bar move/resize) is intra-process or WM-internal, so
// no drop could ever cross a process boundary.
//
// WHY THIS IS SMALL. wm_dispatch_event() already routes every mouse event to
// window_get_at_point(), the topmost window under the cursor, and there is NO
// pointer grab anywhere in this window manager. So window B ALREADY receives
// EVENT_MOUSE_MOVE with the button held the moment the cursor crosses into it,
// and ALREADY receives EVENT_MOUSE_UP on release. The only thing missing is a
// shared fact: "a drag is in flight, it carries this kind, and here is who
// will take it". That is all this protocol adds. There is NO hook in
// gui/window.c, no change to input routing, and no grab.
//
// WHAT RUNS WHEN NOBODY IS DRAGGING: nothing. The session lives in
// rustkern/dragsess.rs behind one integer, every syscall below returns on that
// integer test, and the compositor only calls SYS_DRAG_PEEK while a mouse
// button is physically held.
//
// LIVE PTY HAND-OFF IS NOT WHAT THIS DOES, and deliberately so. It was
// measured on 2026-08-25: this kernel has NO cross-process fd passing (no
// AF_UNIX, no SCM_RIGHTS; proc->fds[] is per-process and only spawn_impl(),
// parent -> brand-new child, fds 0-2 only, ever writes another process's
// table), and ptmx_open() ALWAYS allocates a fresh pair so an existing pty
// master cannot be reopened. More decisively still, the terminal has no
// long-lived shell: a pty is created per foreground command and destroyed when
// it exits, so there is no durable session object to hand over in the first
// place. The payload here is therefore SERIALIZED STATE (cwd, title,
// scrollback), not a descriptor. That loses a mid-run foreground command and
// nothing else, and it is the only honest design available today.
#define SYS_DRAG_BEGIN    401
#define SYS_DRAG_PEEK     402
#define SYS_DRAG_TAKE     403
#define SYS_DRAG_ACCEPT   404
#define SYS_DRAG_RELEASE  405
#define SYS_DRAG_END      406

// Payload kinds. Mirrored in userland/libc/syscall.h and rustkern/dragsess.rs.
#define DRAG_KIND_TERMTAB 0x1
#define DRAG_KIND_TEXT    0x2
#define DRAG_KIND_FILE    0x4

#define DRAG_LABEL_CAP    64
#define DRAG_PAYLOAD_CAP  4096

// The non-destructive view of the session returned by SYS_DRAG_PEEK.
//
// THE PAYLOAD IS NOT IN HERE, ON PURPOSE. The compositor peeks on every frame
// of every drag so it can draw the ghost that follows the cursor, and a
// terminal pane payload can contain scrollback: the output of the last command
// the user ran, which routinely contains secrets. A caption does not justify
// handing that to the desktop shell. Bytes come only from SYS_DRAG_TAKE, only
// to the window the KERNEL resolved as the drop target, and only after the
// button is actually up.
//
// Layout is duplicated in userland/libc/syscall.h and mirrored by DragInfo in
// rustkern/dragsess.rs; all three are locked to the same size by
// _Static_assert (the wm_window_info_t discipline), so a one-sided edit fails
// the build instead of silently misreading fields at runtime.
typedef struct {
    int32_t  active;        // 1 while a session exists
    int32_t  src_win;       // window handle that began the drag
    uint32_t src_pid;       // its owning pid
    uint32_t kind;          // DRAG_KIND_*
    int32_t  payload_len;   // bytes available to the resolved target
    int32_t  released;      // 1 once the button is up and a target is resolved
    int32_t  drop_x;        // screen coords of the release
    int32_t  drop_y;
    int32_t  target_win;    // resolved target handle, or -1 for none
    int32_t  label_len;
    uint8_t  label[DRAG_LABEL_CAP];   // short caption for the drag ghost
} drag_info_t;
_Static_assert(sizeof(drag_info_t) == 104,
               "SYS_DRAG_PEEK: drag_info_t layout is duplicated in "
               "userland/libc/syscall.h and rustkern/dragsess.rs, and its size "
               "is SZ_DRAG_INFO in rustkern/argtab.rs. Update all four.");

#endif // SYSCALL_H
