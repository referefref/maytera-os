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
#define SYS_NTP_SYNC        147 // Sync from NTP; returns 0=ok -1=fail
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
#define SYS_GET_CURSOR          249  // Read live mouse-cursor style + size (packed: style|size<<8)
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
// #492 Stage 1a: kernel self-update (OTA) write path. arg1=new_kernel image
// ptr, arg2=len, arg3=expected_sha256[32] ptr, arg4=target_build. Returns a
// SELFUPD_* code (0=OK). Reboot afterwards via the existing SYS_REBOOT (207).
#define SYS_KERNEL_SELFUPDATE   313
#define SYS_OTA_VERIFY_SIG      314  // (digest[32], sig, sig_len) -> 0 valid / -1: verify a signed OTA manifest against the baked-in pubkey
#define SYS_PRINT_SCREEN        297  // (#318) (const char *printer) -> 0/-1 (print the framebuffer)
#define SYS_BOOTLOG_WRITE       298  // (#307 real-hw) (const char *msg) -> 0/-1, appends to persistent /BOOTLOG.TXT
#define SYS_UPTIME_MS           252  // Monotonic ms since boot (ticks*1000/g_timer_hz)
#define SYS_DECODE_IMAGE        253  // Decode+point-sample an image (BMP/PNG/JPEG) to a target box
#define SYS_WIN_DRAW_IMAGE      254
#define SYS_HTTP_FETCH_START    255  // async fetch: start  (url) -> job id
#define SYS_HTTP_FETCH_POLL     256  // async fetch: poll   (id,&status,&len) -> 0run/1done/2err
#define SYS_HTTP_FETCH_READ     257  // async fetch: read   (id,buf,max) -> bytes, frees job
#define SYS_HTTP_FETCH_CANCEL   258
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
#define SYS_MAX                        215

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
int64_t sys_http_post_start(const char *url, const char *headers, const char *body);
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
int64_t sys_readdir(int fd, void *entry_buf);

// Window/Graphics
int64_t sys_win_create(const char *title, int x, int y, int width, int height);
int64_t sys_win_set_nochrome(int handle);
int64_t sys_win_destroy(int handle);
int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color);
int64_t sys_win_draw_text(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color);
int64_t sys_win_get_event(int handle, void *event_buf, int timeout);
int64_t sys_win_invalidate(int handle);
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
int64_t sys_passwd_change(const char *username, const char *old_pass, const char *new_pass);
int64_t sys_su(const char *username, const char *password);
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

#define SYS_PROC_LIST 238  // Task Manager: snapshot process table
#define SYS_HTTP_POST 239  // HTTPS POST (url, headers, body, buf, cap, &status) for LLM/REST clients
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
