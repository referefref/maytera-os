// syscall.c - System call implementation for MayteraOS
#include "syscall.h"
#include "procinfo.h"   // #487: Ring-3 process introspection backends
#include "syscall_argtab.h"  // #503: central pointer-arg validation
#ifdef SECTEST_SYSCALL
extern int64_t validate_selftest(void *ubuf, uint64_t ubuf_len);
#endif
#include "../gui/image.h"

int64_t sys_decode_image(const void *, uint32_t, uint32_t, void *, uint32_t, int *);
int64_t sys_win_draw_image(int, int, int, int, int, uint32_t *);
#include "process.h"
#include "../sync/waitq.h"   // #453: wait-queue for win_get_event blocking
#include "../drivers/audio_pcm.h"  // Ring-3 PCM push (Ring-0 media exit, phase 1)
#include "../version.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/gdt.h"
#include "../cpu/isr.h"
#include "../mm/vmm.h"
#include "../mm/heap.h"
#include "../sync/spinlock.h"
#include "../fs/fat.h"
#include "../fs/ext2.h"
#include "../fs/perms.h"
#include "../fs/bootlog.h"
#include "users.h"
#include "../gui/window.h"
#include "../gui/ttf.h"
#include "../gui/syslog.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../net/net.h"
#include "../net/dhcp.h"
#include "../net/udp.h"
#include "../net/tcp.h"
#include "../net/icmp.h"
#include "../net/smb.h"
#include "../net/nfs.h"
#include "../ipc/msg.h"
#include "../ipc/shm.h"
#include "../gui/fb_syscall.h"
#include "../exec/elf.h"
#include "../exec/ne.h"
#include "../fs/vfs.h"
#include "services.h"
#include "cron.h"
#include "../devinfo.h"

// WM blit debug log toggle (see user_window_draw_handler). Default OFF.
volatile int g_wm_blit_debug = 0;
static int64_t sys_spawn_args(const char *path, char **argv, int argc);
static int64_t sys_spawn_redir(const char *path, char **argv, int argc,
                               const char *infile, const char *outfile, int append);
// VFS file backings used for shell redirection (open a path as a struct file_t
// so it can be installed directly into a child's stdin/stdout fd).
extern file_t *fat_vfs_open(const char *path, int flags);
extern file_t *ext2_vfs_open(const char *path, int flags);
// ext2 root-cutover path helpers (defined alongside sys_open below).
static int         path_is_ext2(const char *p);
static const char *ext2_relpath(const char *p);
static int         path_root_ext2(const char *p);
// #317 pass 2: SMB network mount control syscalls (defined below).
int64_t sys_net_mount(const char *server, const char *share,
                      const char *user, const char *pass);
int64_t sys_net_list_shares(const char *server, char *ubuf, uint32_t maxlen);
int64_t sys_net_unmount(const char *server, const char *share);

// External filesystem
extern fat_fs_t g_fat_fs;

// External timer
extern volatile uint64_t timer_ticks;

// MSR addresses for SYSCALL/SYSRET
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083  // 32-bit SYSCALL (not used)
#define MSR_SFMASK          0xC0000084

// EFER bits
#define EFER_SCE            (1 << 0)    // SYSCALL enable

// Forward declaration of assembly entry point
extern void syscall_entry(void);

// Set kernel stack for syscall handling
extern void syscall_set_kernel_stack(uint64_t stack_top);

// Note: rdmsr/wrmsr are already defined in types.h

// ============================================================================
// Syscall Initialization
// ============================================================================

void syscall_init(void) {
    kprintf("[SYSCALL] Initializing syscall mechanism...\n");

    // Enable SYSCALL/SYSRET in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // Set up STAR register:
    // Bits 63:48 = SYSRET CS and SS (user mode selectors)
    // Bits 47:32 = SYSCALL CS and SS (kernel mode selectors)
    // SYSRET loads CS from bits 63:48 + 16 for 64-bit, SS from bits 63:48 + 8
    // For user mode: CS = 0x1B (GDT_USER_CODE | 3), SS = 0x23 (GDT_USER_DATA | 3)
    // But SYSRET adds 16 to get 64-bit CS, so we use base selector 0x18
    // Kernel: CS = 0x08, SS = 0x10
    uint64_t star = ((uint64_t)(GDT_USER_CODE - 16) << 48) |  // User base (SYSRET adds 16)
                    ((uint64_t)GDT_KERNEL_CODE << 32);         // Kernel CS
    wrmsr(MSR_STAR, star);

    // Set LSTAR to syscall entry point (64-bit)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // Set SFMASK - flags to clear on syscall
    // Clear IF (disable interrupts) and TF (trap flag)
    wrmsr(MSR_SFMASK, 0x200 | 0x100);  // IF | TF

    kprintf("[SYSCALL] SYSCALL/SYSRET enabled\n");
    kprintf("[SYSCALL] LSTAR = 0x%lx\n", (uint64_t)syscall_entry);
}

// ============================================================================
// Syscall Dispatcher
// ============================================================================

// Forward declarations for functions defined later in this file
int64_t sys_win_blit(int handle, int x, int y, int src_w, int src_h, uint32_t *src_buffer);
int64_t sys_win_draw_text_small(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_text_ttf(int handle, int x, int y, const char *text, uint32_t color, int size);
int64_t sys_win_draw_text_ttf_ex(int handle, int x, int y, const char *text, uint32_t color, int size, int face, int style);
int64_t sys_win_get_pos(int handle, int *x, int *y);
int64_t sys_win_move(int handle, int x, int y);
int64_t sys_win_move_by(int handle, int dx, int dy);

// Network TX from a user-process syscall context runs on the process CR3, which
// only copies the kernel's UPPER-half PML4 entries (256-511). The NIC's MMIO
// registers and DMA ring buffers live in the kernel's LOWER-half identity map
// (entries 0-255), which is absent from the process CR3, so a raw eth_send from
// here silently fails (no packet reaches the wire, not even ARP requests).
// kprintf still works because serial uses port I/O, which needs no mapping.
//
// Fix: switch CR3 to the kernel master page table (vmm_get_pml4(), which maps
// BOTH kernel code/stack upper-half AND the NIC lower-half) for the duration of
// the send, with interrupts disabled, then restore the caller's CR3. This
// mirrors the proven scheduler pattern in process.c (mov %cr3). RX replies are
// drained by net_poll() running on the kernel CR3 in the desktop loop while this
// process sleeps between retries, so the ARP cache gets populated out of band.
extern uint64_t vmm_get_pml4(void);

static inline uint64_t net_cr3_enter(void) {
    uint64_t saved;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved));
    uint64_t kcr3 = vmm_get_pml4();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    return saved;
}

static inline void net_cr3_exit(uint64_t saved) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(saved) : "memory");
}

// Non-blocking DNS for userland (poll-split, like TCP connect/state).
// User-info struct as seen by userland (must match libc user_info_t).
typedef struct {
    char     username[64];
    char     display_name[64];
    uint32_t uid;
    uint32_t gid;
    uint8_t  active;
    uint8_t  padding[3];
} sc_user_info_t;
// #503 argtab sizeof-lock. The Rust table (rustkern.rs, SZ_SC_USER_INFO)
// validates SYS_LIST_USERS as max*140 writable bytes. If this struct grows and
// that constant does not, the tail of every row the kernel writes goes
// unvalidated. Grow one, grow the other.
_Static_assert(sizeof(sc_user_info_t) == 140,
               "#503 argtab: SZ_SC_USER_INFO in rustkern.rs is stale");

// Wallpaper index shared with the userland compositor: Settings sets it via
// SYS_SET_WALLPAPER, the compositor polls SYS_GET_WALLPAPER each frame and
// reloads the desktop background when it changes.
static int g_wallpaper_idx = 0;
// Desktop icon size (0=Small,1=Medium,2=Large); compositor polls it. (#63)
static int g_icon_size = 1;
// Display effects: brightness 0-100 (100=normal), night-light 0-100 (0=off).
// Compositor polls SYS_GET_DISPLAY_FX and post-processes the framebuffer. (#57)
static int g_brightness = 100;
static int g_nightlight = 0;
// System font size index (0=Small,1=Medium,2=Large,3=X-Large); compositor polls. (#58)
static int g_font_size = 1;
static int g_screensaver_type = 2;
static int g_screensaver_delay = 120;  // (#115) screensaver activation delay, seconds
int g_win_blit_suppressed = 0;   // set by compositor while the screensaver owns the FB
// (#116) Live mouse-cursor style/size. Settings sets these via SYS_SET_CURSOR; the
// compositor reads them every frame via SYS_GET_CURSOR (same live-apply pattern as
// theme/opacity), so changing the cursor in Settings updates it without a reboot.
static int g_cursor_style = 0;   // 0=Light, 1=Dark, 2=Glow
static int g_cursor_size  = 100; // percent (100 = 1.0x)
static int g_settings_tab = -1;  // #74 one-shot: panel for the next Settings launch
static int g_ss_test = 0; // one-shot screensaver test trigger // SS_STARFIELD; compositor polls (#screensaver)

// Win16 launcher (#144): the interpreter runs in its own kernel process (the
// proven proc_create path, same primitive RemoteCtrl/terminal use) so the caller
// (compositor or RC) is never blocked and the desktop keeps drawing. One Win16
// app at a time (the interpreter uses global state).
static volatile int g_win16_busy = 0;
static char         g_win16_path[128];
static void win16_proc_entry(void *arg) {
    (void)arg;
    win16_run_file(g_win16_path);
    g_win16_busy = 0;
}
// Defined in dos/dosexec.c: launch an MS-DOS program in its own kernel proc +
// host window (non-blocking). Declared here so the SYS_DOS_RUN dispatch (#208)
// can call it without pulling in dos/dosexec.h.
int dos_launch(const char *path);

int win16_launch(const char *path) {
    if (g_win16_busy) return -1;
    if (!path || !path[0]) return -1;
    int i = 0;
    for (; i < (int)sizeof(g_win16_path) - 1 && path[i]; i++)
        g_win16_path[i] = path[i];
    g_win16_path[i] = '\0';
    g_win16_busy = 1;
    if (proc_create("win16", win16_proc_entry, NULL, PRIO_NORMAL) < 0) {
        g_win16_busy = 0;
        return -1;
    }
    return 0;
}

// Disk info struct as seen by userland (must match libc disk_info_t).
typedef struct {
    uint8_t  present;
    uint8_t  type;        // 0=ATA, 1=ATAPI
    int8_t   smart;       // 1=ok, 0=failing, -1=unknown
    uint8_t  pad;
    uint32_t size_mb;
    char     model[41];
    char     serial[21];
    char     pad2[2];
} sc_disk_info_t;
// #503: SYS_GET_DISK_INFO's descriptor (rustkern.rs SZ_SC_DISK_INFO) hardcodes
// this size, because Rust cannot see a C struct private to this TU. If a field
// is added here and the table is not updated, the validator would prove fewer
// bytes writable than sys_get_disk_info() zeroes and fills, and the tail of
// every one of those writes would go unchecked. Fail the build instead. Value is
// compiler ground truth (nm -S on a probe TU with the real kernel CFLAGS).
_Static_assert(sizeof(sc_disk_info_t) == 72, "#503 argtab: SZ_SC_DISK_INFO in rustkern.rs is stale");
int64_t sys_get_disk_info(int idx, void *buf);

int64_t sys_list_users(sc_user_info_t *ubuf, int max) {
    if (!ubuf || max <= 0) return -1;
    int n = 0;
    user_entry_t *t = users_all(&n);
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        if (!t[i].active) continue;
        int k;
        for (k = 0; k < 63 && t[i].username[k]; k++) ubuf[out].username[k] = t[i].username[k];
        ubuf[out].username[k] = '\0';
        for (k = 0; k < 63 && t[i].display_name[k]; k++) ubuf[out].display_name[k] = t[i].display_name[k];
        ubuf[out].display_name[k] = '\0';
        ubuf[out].uid = t[i].uid;
        ubuf[out].gid = t[i].gid;
        ubuf[out].active = 1;
        out++;
    }
    return out;
}

int64_t sys_authenticate(const char *uname, const char *upass) {
    if (!uname || !upass) return -1;
    if (user_verify_password(uname, upass) != 0) return -1;
    user_entry_t *u = user_lookup_name(uname);
    return u ? (int64_t)u->uid : -1;
}

int64_t sys_delete_user(const char *uname) {
    process_t *p = proc_current();
    if (p && p->euid != 0) return -1;   // root only
    if (!uname) return -1;
    return user_delete_by_name(uname);
}

int64_t sys_dns_start(const char *uhost, uint32_t *uip) {
    extern int dns_resolve_start(const char *hostname, uint32_t *ip_out);
    extern int dns_resolve_check(uint32_t *ip_out);
    extern void net_poll(void);
    if (!uhost || !uip) return -1;
    char host[256];
    int i = 0;
    for (; i < 255 && uhost[i]; i++) host[i] = uhost[i];
    host[i] = '\0';
    uint32_t ip = 0;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = dns_resolve_start(host, &ip);
    net_poll();
    if (rc == 0 && dns_resolve_check(&ip) == 1) rc = 1;
    net_cr3_exit(saved);
    if (flags & 0x200) __asm__ volatile("sti");
    if (rc == 1) *uip = ip;
    return rc;
}

int64_t sys_dns_poll(uint32_t *uip) {
    extern int dns_resolve_check(uint32_t *ip_out);
    extern void net_poll(void);
    uint32_t ip = 0;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_poll();
    int rc = dns_resolve_check(&ip);
    net_cr3_exit(saved);
    if (flags & 0x200) __asm__ volatile("sti");
    if (rc == 1 && uip) *uip = ip;
    return rc;
}

extern int eth_receive(void);

// Send an ICMP echo request AND drain the NIC RX ring, all on the kernel address
// space. We cannot rely on the desktop loop's net_poll() to process the ARP/echo
// replies while a user ping process spins, so we both transmit and receive here:
//   - icmp_ping() fires the echo (or an ARP request if the next hop is unresolved)
//   - eth_receive() processes any pending RX frames (an ARP reply caches the MAC,
//     an ICMP echo reply sets the ping-reply flag)
// Both TX and RX touch the NIC MMIO/DMA in the kernel lower-half identity map,
// which is absent from the user process CR3, so the whole window runs on the
// kernel pml4 with interrupts disabled. Returns icmp_ping()'s result.
static int icmp_ping_kcr3(uint32_t dest_ip) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = icmp_ping(dest_ip);
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// Drain only the NIC RX ring on the kernel address space (used while waiting for
// the echo reply, so we do not keep retransmitting).
static void net_rx_drain_kcr3(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
}

// ---------------------------------------------------------------------------
// TCP syscall wrappers (used by userland network apps, e.g. /APPS/irc)
//
// Same root cause as ping: a TCP TX from a user-process syscall runs on the
// process CR3, which lacks the kernel lower-half identity map where the NIC
// MMIO/DMA live, so the SYN / data / FIN never reach the wire. We run every TCP
// operation on the kernel pml4 with interrupts disabled, and also drain the RX
// ring there so the handshake and incoming data make progress in-band rather
// than relying on the desktop poll loop being scheduled. tcp_send / tcp_recv
// take user pointers, which are NOT mapped under the kernel CR3, so we bounce
// the payload through a kernel-stack buffer while the user CR3 is still active.
extern void tcp_timer(void);

static inline void net_rx_drain_locked(void) {
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    tcp_timer();
}

static int tcp_connect_kcr3(int sock, uint32_t ip, uint16_t port) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_connect(sock, ip, port);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

static int tcp_send_kcr3(int sock, const void *ubuf, uint16_t len) {
    uint8_t kbuf[1600];
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    // Copy from user space while the process CR3 is still active.
    memcpy(kbuf, ubuf, len);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_send(sock, kbuf, len);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

static int tcp_recv_kcr3(int sock, void *ubuf, uint16_t len) {
    uint8_t kbuf[1600];
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = tcp_recv(sock, kbuf, len);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    // Copy to user space after the process CR3 is restored.
    if (rc > 0)
        memcpy(ubuf, kbuf, rc);
    return rc;
}

static int tcp_close_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_close(sock);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// tcp_get_state itself touches no NIC, but the userland app polls it in a loop
// waiting for the handshake to complete. We must run the full RX drain AND
// tcp_timer() here on the kernel CR3:
//   - eth_receive() processes the ARP reply (caches the next-hop MAC) and any
//     SYN-ACK, advancing the state and sending the final ACK.
//   - tcp_timer() retransmits the SYN. The very first SYN is dropped because the
//     next-hop MAC is not yet resolved (tcp_connect only fires the ARP request);
//     without a retransmit the handshake never starts. tcp_timer is time-based
//     (TCP_RETRANSMIT_TIMEOUT ticks), so calling it on every poll is safe and the
//     SYN goes back out once the ARP cache is populated.
// Both touch the NIC MMIO/DMA in the kernel lower-half identity map, absent from
// the user process CR3, so the whole window runs on the kernel pml4 with
// interrupts disabled.
static int tcp_state_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = (int)tcp_get_state(sock);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// #443: bind()+listen() combined into one syscall (this codebase never exposed
// a standalone SYS_BIND to userland). Neither tcp_bind() nor tcp_listen() sends
// anything on the wire -- they only update the local connection table entry
// (same as tcp_socket()/tcp_close(), which are also called directly below with
// no CR3 switch) -- so no kcr3 wrapping is needed here.
static int tcp_listen_bind(int sock, uint16_t port, int backlog) {
    int r = tcp_bind(sock, port);
    if (r < 0) return r;
    return tcp_listen(sock, backlog);
}

// #443: accept() must let a pending SYN's handshake actually complete (SYN ->
// SYN-ACK -> ACK -> ESTABLISHED) so tcp_accept() has something to return, which
// requires draining the NIC RX ring and running the retransmit timer -- the
// same reasoning as tcp_state_kcr3 above, which a polling tcp_connect() loop
// relies on for the same purpose. A userland server calling accept() in a loop
// must not depend on some other process's desktop loop happening to be
// scheduled to drive net_poll(); do it here so accept() makes progress on its
// own regardless of what else is running.
static int tcp_accept_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = tcp_accept(sock);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// ICMP ping: send one echo request to dest_ip (host byte order) and wait up to
// timeout_ms for a reply. Returns approximate round-trip ms (>=0) on reply,
// -1 on send failure or timeout.
//
// icmp_ping() -> ip_send() returns <0 when the next hop's MAC is not yet in the
// ARP cache; ip_send() fires an ARP request in that case and expects the caller
// to retry once the reply arrives. We retry the send (each retry also drains RX
// so the ARP reply is cached in the same syscall), then poll the reply flag.
static int64_t sys_ping(uint32_t dest_ip, int timeout_ms) {
    if (timeout_ms <= 0)
        timeout_ms = 1000;
    int waited = 0;

    // Phase 1: get the echo request onto the wire (resolving ARP if needed).
    int sent = 0;
    while (waited < timeout_ms) {
        if (icmp_ping_kcr3(dest_ip) >= 0) {
            sent = 1;
            break;
        }
        proc_sleep(20);
        waited += 20;
    }
    if (!sent)
        return -1;

    // Phase 2: wait for the matching echo reply, draining RX as we go.
    while (waited < timeout_ms) {
        net_rx_drain_kcr3();
        uint32_t src = 0;
        uint16_t seq = 0, rtt = 0;
        if (icmp_get_ping_reply(&src, &seq, &rtt))
            return (int)rtt;
        proc_sleep(5);
        waited += 5;
    }
    return -1;
}

// #430: signal engine (proc/signal.c), threads (proc/process.c) and futex
// (sync/futex.c). Declared here so the dispatcher can reach them.
extern int64_t sys_kill(int pid, int signo);
extern int64_t sys_sigaction(int signo, const void *new_act, void *old_act);
extern int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset);
extern int64_t sys_rt_sigreturn(void);
extern int64_t sys_alarm(uint32_t seconds);
extern int64_t sys_pause(void);
extern int proc_clone(uint32_t flags, void *user_stack, uint32_t *parent_tid,
                      uint32_t *child_tid, void *tls);
extern uint32_t proc_gettid(void);
extern uint32_t proc_set_tid_address(uint32_t *tidptr);
extern int64_t sys_futex(uint32_t *addr, int op, uint32_t val, uint64_t timeout,
                         uint32_t *addr2, uint32_t val3);

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6) {

    // #503 / MAYTERA-SEC-2026-0016: THE CHOKE POINT.
    //
    // Every user pointer this kernel is handed arrives here, once, before any
    // handler sees it. That placement is the entire design: #500 found 171
    // user-pointer arguments across 239 dispatcher cases and exactly five
    // validated call sites tree-wide, because a per-handler rule is a rule that
    // ~110 authors each had to remember and all but five forgot. Fixing the
    // handlers fixes tonight; the next syscall reopens the hole. Fixing the
    // dispatcher fixes the shape.
    //
    // Reachable ONLY from proc/syscall.asm, i.e. the SYSCALL instruction, i.e.
    // Ring 3. No kernel caller exists (kernel subsystems call the sys_* handlers
    // directly), so this cannot reject a legitimate kernel pointer. Verified by
    // grep across the tree before wiring: syscall.asm is the only caller.
    //
    // Deliberately NOT gated on proc_current()->privilege: gating would make a
    // syscall's validation depend on process state, which is the trust-a-flag
    // shape this task exists to remove. Ring 3 is Ring 3.
    //
    // A syscall with no descriptor is NOT validated and returns 0 here, so the
    // table rolls out incrementally. tools/syscall-ptr-lint holds the ledger of
    // what is still undeclared and fails the build if that set ever grows.
    {
        int64_t vrc = (int64_t)syscall_validate_args(num, arg1, arg2, arg3,
                                                     arg4, arg5, arg6);
        if (vrc != 0) {
            return vrc;   /* -EFAULT */
        }
    }

    switch (num) {
#ifdef SECTEST_SYSCALL
        // #500 / #503 MAYTERA-SEC-2026-0016 negative-control battery. DEBUG
        // BUILDS ONLY, never shipped: it deliberately hands the validator and
        // the dispatcher argtab kernel addresses in order to prove they are
        // rejected. Shipping it would hand Ring 3 a syscall whose whole job is
        // to dereference attacker-named addresses.
        //
        // NOTE this case sits ABOVE the argtab check?  No - deliberately NOT.
        // It is inside the switch, so syscall_validate_args() has already run
        // on it. SYS_SECTEST has no descriptor, so it passes through
        // unvalidated, which is exactly what this test needs: it must receive
        // its ubuf pointer untouched in order to build the boundary cases.
        case SYS_SECTEST:
            return validate_selftest((void *)arg1, arg2);
#endif
        // Networking (TCP + ICMP)
        case SYS_SOCKET:
            return tcp_socket();
        case SYS_CONNECT:
            return tcp_connect_kcr3((int)arg1, (uint32_t)arg2, (uint16_t)arg3);
        case SYS_SEND:
            return tcp_send_kcr3((int)arg1, (const void *)arg2, (uint16_t)arg3);
        case SYS_RECV:
            return tcp_recv_kcr3((int)arg1, (void *)arg2, (uint16_t)arg3);
        case SYS_TCP_CLOSE:
            return tcp_close_kcr3((int)arg1);
        case SYS_TCP_STATE:
            return (int64_t)tcp_state_kcr3((int)arg1);
        case SYS_LISTEN:
            return tcp_listen_bind((int)arg1, (uint16_t)arg2, (int)arg3);
        case SYS_ACCEPT:
            return tcp_accept_kcr3((int)arg1);
        case SYS_PING:
            return sys_ping((uint32_t)arg1, (int)arg2);

        // Process control
        case SYS_EXIT:
            return sys_exit((int)arg1);
        case SYS_FORK:
            return sys_fork();
        case SYS_EXEC:
            return sys_exec((const char *)arg1);
        case SYS_SPAWN:
            return sys_spawn_args((const char *)arg1, NULL, 0);
        case SYS_SPAWN_ARGS:
            return sys_spawn_args((const char *)arg1, (char **)arg2, (int)arg3);
        case SYS_SPAWN_REDIR:
            return sys_spawn_redir((const char *)arg1, (char **)arg2, (int)arg3,
                                   (const char *)arg4, (const char *)arg5, (int)arg6);
        case SYS_WIN16_RUN:
            // Non-blocking launch in a dedicated kernel process (#144).
            return win16_launch((const char *)arg1);
        case SYS_DOS_RUN:
            // Non-blocking MS-DOS launch in its own kernel proc + window (#208).
            return dos_launch((const char *)arg1);
        case SYS_WIN16_ACTIVE: {
            // (#200 SkiFree) Report whether a Win16 app currently owns the
            // keyboard/foreground. The compositor uses this to treat a running
            // Win16 game as continuous activity so its idle screensaver does not
            // black out the game while the Win16 message pump is the sole key
            // consumer (SYS_GET_KEYBOARD returns -1 to the compositor in that
            // state, so the compositor would otherwise never see input).
            extern volatile int g_win16_owns_screen;
            return g_win16_owns_screen ? 1 : 0;
        }
        case SYS_WAIT:
            return (int64_t)proc_wait(-1, (int *)arg1);
        // #503: this was `case 98: /* SYS_WAITPID */`, a BARE NUMERIC label, and
        // that is why it is worth a comment. syscall-ptr-lint keys on
        // `case SYS_<NAME>:`, so a numeric label makes the case INVISIBLE to the
        // ledger: this syscall hands Ring 3 an `int *` out-param and was not in
        // the 109 the inventory counted, purely because of how its label was
        // spelled. The lint's unit of measurement was a naming convention. A
        // sweep of the dispatcher found exactly one such label out of 239; using
        // the name (which proc/syscall.h has always defined) is the fix, and the
        // count is now 110. Do not reintroduce a numeric case label here.
        case SYS_WAITPID:
            return (int64_t)proc_wait((int)arg1, (int *)arg2);
        case SYS_GETPID:
            return sys_getpid();
        case SYS_GETPPID:
            return sys_getppid();
        case SYS_YIELD:
            return sys_yield();
        case SYS_SLEEP:
            return sys_sleep((uint32_t)arg1);

        // File I/O
        case SYS_OPEN:
            return sys_open((const char *)arg1, (int)arg2);
        case SYS_FCNTL:
            return sys_fcntl((int)arg1, (int)arg2, (long)arg3);
        case SYS_CLOSE:
            return sys_close((int)arg1);
        case SYS_READ:
            return sys_read((int)arg1, (void *)arg2, (size_t)arg3);
        case SYS_WRITE:
            return sys_write((int)arg1, (const void *)arg2, (size_t)arg3);
        case SYS_SEEK:
            return sys_seek((int)arg1, (int64_t)arg2, (int)arg3);
        case SYS_STAT:
            return sys_stat_path((const char *)arg1, (void *)arg2);
        case SYS_HTTP_FETCH:
            return sys_http_fetch((const char *)arg1, (char *)arg2, (uint32_t)arg3, (uint32_t *)arg4, (int *)arg5);
        case SYS_HTTP_FETCH_HDR:
            return sys_http_fetch_hdr((const char *)arg1, (const char *)arg2, (char *)arg3, (uint32_t)arg4, (uint32_t *)arg5, (int *)arg6);
        case SYS_HTTP_POST:
            return sys_http_post((const char *)arg1, (const char *)arg2, (const char *)arg3,
                                 (char *)arg4, (uint32_t)arg5, (int *)arg6);
        case SYS_HTTP_FETCH_START:
            return sys_http_fetch_start((const char *)arg1);
        case SYS_HTTP_FETCH_POLL:
            return sys_http_fetch_poll((int)arg1, (int *)arg2, (uint32_t *)arg3);
        case SYS_HTTP_FETCH_READ:
            return sys_http_fetch_read((int)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_HTTP_FETCH_CANCEL:
            return sys_http_fetch_cancel((int)arg1);
        case SYS_HTTP_POST_START:
            return sys_http_post_start((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_HTTP_POST_POLL:
            return sys_http_post_poll((int)arg1, (int *)arg2, (uint32_t *)arg3);
        case SYS_HTTP_POST_READ:
            return sys_http_post_read((int)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_HTTP_POST_CANCEL:
            return sys_http_post_cancel((int)arg1);

        // Directory / filesystem ops
        case SYS_MKDIR:
            return sys_mkdir((const char *)arg1, (int)arg2);
        case SYS_RMDIR:
            return sys_rmdir((const char *)arg1);
        case SYS_UNLINK:
            return sys_unlink((const char *)arg1);
        case SYS_READDIR:
            return sys_readdir((int)arg1, (void *)arg2);
        case SYS_RENAME:
            return sys_rename((const char *)arg1, (const char *)arg2);

        case SYS_GETCWD:
            return sys_getcwd((char *)arg1, (uint64_t)arg2);
        case SYS_CHDIR:
            return sys_chdir((const char *)arg1);

        // Memory
        case SYS_BRK:
            return sys_brk(arg1);
        case SYS_MMAP:
            return sys_mmap(arg1, arg2, (int)arg3, (int)arg4);
        case SYS_MUNMAP:
            return sys_munmap(arg1, arg2);

        // Console
        case SYS_PUTCHAR:
            return sys_putchar((int)arg1);
        case SYS_GETCHAR:
            return sys_getchar();

        // Time
        case SYS_TIME:
            return sys_time();
        case SYS_GET_TICKS:
            return (int64_t)timer_ticks;   // 250Hz monotonic ticks (4ms each)
        case SYS_UPTIME_MS: {
            // Monotonic milliseconds since boot. The kernel is the single
            // authority on the tick rate (g_timer_hz), so userland never has
            // to divide raw ticks by a guessed frequency.
            extern uint32_t g_timer_hz;
            uint32_t _hz = g_timer_hz ? g_timer_hz : 250;
            return (int64_t)((uint64_t)timer_ticks * 1000ULL / _hz);
        }
        case SYS_GET_VERSION: {
            // Copy "X.Y.Z (build N)" into the user buffer (arg1=buf, arg2=len).
            #define SYSV_S2(x) #x
            #define SYSV_S(x)  SYSV_S2(x)
            static const char *v = MAYTERA_VERSION_STRING " (build " SYSV_S(MAYTERA_BUILD_NUMBER) ")";
            char *ub = (char *)arg1; int ulen = (int)arg2;
            if (!ub || ulen <= 0) return -1;
            int i = 0; while (v[i] && i < ulen - 1) { ub[i] = v[i]; i++; }
            ub[i] = 0;
            return i;
        }
        case SYS_CLOCK:
            return sys_clock();

        // Window/Graphics syscalls
        case SYS_WIN_CREATE:
            return sys_win_create((const char *)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5);
        case SYS_WIN_DESTROY:
            return sys_win_destroy((int)arg1);
        case SYS_WIN_DRAW_RECT:
            return sys_win_draw_rect((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5, (uint32_t)arg6);
        case SYS_WIN_DRAW_TEXT:
            return sys_win_draw_text((int)arg1, (int)arg2, (int)arg3, (const char *)arg4, (uint32_t)arg5);
        case SYS_WIN_DRAW_TEXT_SMALL:
            return sys_win_draw_text_small((int)arg1, (int)arg2, (int)arg3, (const char *)arg4, (uint32_t)arg5);
        case SYS_WIN_DRAW_TTF:
            // size is packed into the top byte of the color argument (RGB is 24-bit).
            return sys_win_draw_text_ttf((int)arg1, (int)arg2, (int)arg3, (const char *)arg4,
                                         (uint32_t)arg5 & 0xFFFFFF,
                                         (int)(((uint32_t)arg5 >> 24) & 0xFF));
        case SYS_WIN_DRAW_PIXEL:
            return sys_win_draw_pixel((int)arg1, (int)arg2, (int)arg3, (uint32_t)arg4);
        case SYS_WIN_BLIT: {
            int src_w = (int)(arg4 & 0xFFFF);
            int src_h = (int)((arg4 >> 16) & 0xFFFF);
            return sys_win_blit((int)arg1, (int)arg2, (int)arg3, src_w, src_h, (uint32_t *)arg5);
        }
        case SYS_WIN_GET_EVENT:
            return sys_win_get_event((int)arg1, (void *)arg2, (int)arg3);
        case SYS_WIN_INVALIDATE:
            return sys_win_invalidate((int)arg1);
        case SYS_WIN_GET_SIZE:
            return sys_win_get_size((int)arg1, (int *)arg2, (int *)arg3);

        // User identity
        case SYS_GETUID:
            return sys_getuid();
        case SYS_SETUID:
            return sys_setuid((uint32_t)arg1);
        case SYS_GETGID:
            return sys_getgid();
        case SYS_SETGID:
            return sys_setgid((uint32_t)arg1);
        case SYS_GETEUID:
            return sys_geteuid();
        case SYS_GETEGID:
            return sys_getegid();
        case SYS_SETEUID:
            return sys_seteuid((uint32_t)arg1);
        case SYS_SETEGID:
            return sys_setegid((uint32_t)arg1);
        case SYS_CHMOD:
            return sys_chmod((const char *)arg1, (uint16_t)arg2);
        case SYS_CHOWN:
            return sys_chown((const char *)arg1, (uint32_t)arg2, (uint32_t)arg3);
        case SYS_PASSWD_CHANGE:
            return sys_passwd_change((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_SU:
            return sys_su((const char *)arg1, (const char *)arg2);
        case SYS_ADDUSER:
            return sys_adduser((const char *)arg1, (uint32_t)arg2, (uint32_t)arg3,
                              (const char *)arg4, (const char *)arg5);
        case SYS_SET_THEME:
            return sys_set_theme((int)arg1);
        case SYS_GET_THEME:
            return sys_get_theme();
        case SYS_THEME_COLOR: {
            extern uint32_t theme_get_color_by_id(int theme_id, int color_id);
            return (int64_t)(uint32_t)theme_get_color_by_id((int)arg1, (int)arg2);
        }
        case SYS_PRINT_LIST:
            return sys_print_list((void *)arg1, (int)arg2);
        case SYS_PRINT_JOB:
            return sys_print_job((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_PRINT_ADD:
            return sys_print_add((const char *)arg1, (const char *)arg2, (int)arg3,
                                 (const char *)arg4, (int)arg5);
        case SYS_PRINT_REMOVE:
            return sys_print_remove((const char *)arg1);
        case SYS_PRINT_IMAGE:
            return sys_print_image((const char *)arg1, (const char *)arg2);
        case SYS_PRINT_SCREEN:
            return sys_print_screen((const char *)arg1);
        case SYS_BOOTLOG_WRITE:
            return sys_bootlog_write((const char *)arg1);
        case SYS_SET_VOLUME:
            return sys_set_volume((int)arg1);
        case SYS_GET_VOLUME:
            return sys_get_volume();
        case SYS_SET_MUTE:
            return sys_set_mute((int)arg1);
        case SYS_GET_DISK_TOTAL:
            return sys_get_disk_total();
        case SYS_GET_DISK_FREE:
            return sys_get_disk_free();
        case SYS_SET_MOUSE_SPEED:
            return sys_set_mouse_speed((int)arg1);
        case SYS_GET_MOUSE_SPEED:
            return sys_get_mouse_speed();
        case SYS_GET_RTC_TIME:
            return sys_get_rtc_time();
        case SYS_GET_RTC_DATE:
            return sys_get_rtc_date();
        case SYS_SET_RTC_TIME: return sys_set_rtc_time(arg1);
        case SYS_SET_RTC_DATE: return sys_set_rtc_date(arg1);
        case SYS_GET_NET_INFO: return sys_get_net_info((void *)arg1, arg2);
        case SYS_NET_SET_STATIC: return sys_net_set_static((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_NET_DHCP: { extern int dhcp_discover_blocking(void); extern void net_clear_fault(void); net_clear_fault(); return (int64_t)dhcp_discover_blocking(); }  // #549: renew/reconnect clears fault
        case SYS_NET_IS_UP: { extern int net_is_up(void); return (int64_t)net_is_up(); }  // #374
        case SYS_DESKTOP_MENU_RELOAD: { extern void desktop_menu_reload(void); desktop_menu_reload(); return 0; }  // #402
        case SYS_KERNEL_SELFUPDATE: {  // #492 Stage 1b: authenticated brick-safe self-update
            extern int kernel_selfupdate_apply(const void *, uint32_t,
                                               const uint8_t *, uint32_t,
                                               const uint8_t *, uint32_t);
            // PRIVILEGE GATE: only a registered service holding SVC_PERM_SELFUPDATE
            // may install a kernel. Arbitrary Ring-3 apps (is_service==0) are
            // refused outright, so the primitive is unreachable from ordinary
            // user code. The mandatory RSA signature check inside the primitive is
            // the second, cryptographic gate.
            process_t *ku_cur = proc_current();
            if (!ku_cur || !ku_cur->is_service ||
                !(ku_cur->svc_perms & SVC_PERM_SELFUPDATE)) {
                return (int64_t)(-11) /* SELFUPD_ERR_PERM */;
            }
            const void *ku_img = (const void *)arg1;
            uint32_t ku_len = (uint32_t)arg2;
            const uint8_t *ku_sha = (const uint8_t *)arg3;
            uint32_t ku_build = (uint32_t)arg4;
            const uint8_t *ku_sig = (const uint8_t *)arg5;
            uint32_t ku_sig_len = (uint32_t)arg6;
            return (int64_t)kernel_selfupdate_apply(ku_img, ku_len, ku_sha, ku_build,
                                                    ku_sig, ku_sig_len);
        }
        case SYS_OTA_VERIFY_SIG: {  // #492 Stage 1b: authenticate a signed manifest
            // Read-only signature check against the baked-in OTA pubkey. Safe for
            // any caller (no privilege needed): it grants no capability, it only
            // tells the client whether a manifest is authentic before it acts.
            extern int kernel_ota_verify_sig(const uint8_t *, const uint8_t *, uint32_t);
            const uint8_t *ov_dig = (const uint8_t *)arg1;
            const uint8_t *ov_sig = (const uint8_t *)arg2;
            uint32_t ov_len = (uint32_t)arg3;
            return (int64_t)kernel_ota_verify_sig(ov_dig, ov_sig, ov_len);
        }
        case SYS_PKG_WRITE: {  // #402 package manager writes to the FAT ESP (/APPS ...)
            extern fat_fs_t g_fat_fs;
            extern int fat_write_file(fat_fs_t *, const char *, const void *, uint32_t);
            const char *pw_path = (const char *)arg1; const void *pw_data = (const void *)arg2;
            uint32_t pw_len = (uint32_t)arg3;
            if (!pw_path || (!pw_data && pw_len)) return -1;
            void *pw_kb = kmalloc(pw_len ? pw_len : 1);
            if (!pw_kb) return -1;
            if (pw_len) memcpy(pw_kb, pw_data, pw_len);
            int pw_rc = fat_write_file(&g_fat_fs, pw_path, pw_kb, pw_len);
            kfree(pw_kb);
            return pw_rc;
        }
        case SYS_NET_MOUNT:
            return sys_net_mount((const char *)arg1, (const char *)arg2,
                                 (const char *)arg3, (const char *)arg4);
        case SYS_NET_LIST_SHARES:
            return sys_net_list_shares((const char *)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_NET_UNMOUNT:
            return sys_net_unmount((const char *)arg1, (const char *)arg2);
        case SYS_GET_DISK_INFO: return sys_get_disk_info((int)arg1, (void *)arg2);
        case SYS_NTP_SYNC:     return sys_ntp_sync();
        case SYS_SET_CURSOR_THEME: {
            extern void cursor_set_theme(int theme);
            cursor_set_theme((int)arg1);
            return 0;
        }
        case SYS_GET_CURSOR_THEME: {
            extern int cursor_get_theme(void);
            return (int64_t)cursor_get_theme();
        }
        case SYS_SET_WALLPAPER:
            g_wallpaper_idx = (int)arg1;
            return 0;
        case SYS_GET_WALLPAPER:
            return (int64_t)g_wallpaper_idx;
        case SYS_SET_ICON_SIZE:
            g_icon_size = (int)arg1;
            return 0;
        case SYS_GET_ICON_SIZE:
            return (int64_t)g_icon_size;
        case SYS_SET_DISPLAY_FX:
            g_brightness = (int)arg1;
            g_nightlight = (int)arg2;
            if (g_brightness < 0) g_brightness = 0;
            if (g_brightness > 100) g_brightness = 100;
            if (g_nightlight < 0) g_nightlight = 0;
            if (g_nightlight > 100) g_nightlight = 100;
            return 0;
        case SYS_GET_DISPLAY_FX:
            return (int64_t)(g_brightness | (g_nightlight << 8));
        case SYS_DRAW_TTF: {
            extern void ttf_draw_string(int, int, const char *, int, unsigned int);
            ttf_draw_string((int)arg1, (int)arg2, (const char *)arg3, (int)arg4, (unsigned int)arg5);
            return 0;
        }
        case SYS_MEASURE_TTF: {
            extern int ttf_measure_string(const char *, int);
            return (int64_t)ttf_measure_string((const char *)arg1, (int)arg2);
        }
        case SYS_SET_FONT_SIZE:
            g_font_size = (int)arg1;
            if (g_font_size < 0) g_font_size = 0;
            if (g_font_size > 3) g_font_size = 3;
            return 0;
        case SYS_GET_FONT_SIZE:
            return (int64_t)g_font_size;

        // ---- OS-wide font registry (multi-face TrueType) ----
        case SYS_FONT_COUNT: {
            extern int ttf_face_count(void);
            return (int64_t)ttf_face_count();
        }
        case SYS_FONT_NAME: {
            extern int ttf_face_name(int, char *, int);
            char *ub = (char *)arg2; int cap = (int)arg3;
            if (!ub || cap <= 0) return -1;
            return (int64_t)ttf_face_name((int)arg1, ub, cap);
        }
        case SYS_FONT_SET_UI: {
            // System-wide UI font. Every legacy (non-_f) text path draws with
            // the active face, so this restyles the whole desktop live.
            extern int ttf_set_active_face(int);
            return (int64_t)ttf_set_active_face((int)arg1);
        }
        case SYS_FONT_GET_UI: {
            extern int ttf_get_active_face(void);
            return (int64_t)ttf_get_active_face();
        }
        case SYS_FONT_STYLE: {
            extern int ttf_face_style(int, char *, int);
            char *ub = (char *)arg2; int cap = (int)arg3;
            if (!ub || cap <= 0) return -1;
            return (int64_t)ttf_face_style((int)arg1, ub, cap);
        }
        case SYS_FONT_FIND: {
            extern int ttf_face_by_path(const char *);
            const char *up = (const char *)arg1;
            if (!up) return -1;
            return (int64_t)ttf_face_by_path(up);
        }
        // #542 OS-wide system clipboard. Kernel-held bounded store so ANY Ring-3
        // app can copy/paste across apps. Backing buffer + all length clamping
        // live in Rust (rustkern/clipboard.rs); the copy is bounded there.
        case SYS_CLIP_SET: {
            extern long clip_set_rs(const unsigned char *src, unsigned long len);
            return (int64_t)clip_set_rs((const unsigned char *)arg1,
                                        (unsigned long)arg2);
        }
        case SYS_CLIP_GET: {
            extern long clip_get_rs(unsigned char *dst, unsigned long cap);
            return (int64_t)clip_get_rs((unsigned char *)arg1,
                                        (unsigned long)arg2);
        }
        case SYS_CLIP_LEN: {
            extern long clip_len_rs(void);
            return (int64_t)clip_len_rs();
        }
        case SYS_FONT_RESCAN: {
            extern int ttf_rescan(void);
            return (int64_t)ttf_rescan();
        }
        case SYS_FONT_REMOVE: {
            extern int ttf_face_remove(int);
            return (int64_t)ttf_face_remove((int)arg1);
        }
        case SYS_FONT_GLYPH: {
            extern ttf_glyph_t *ttf_get_glyph_f(int, int, int, int);
            int face  = (int)(arg1 & 0xFF);
            int size  = (int)((arg1 >> 8) & 0xFFFF);
            int style = (int)((arg1 >> 24) & 0xFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            int *meta = (int *)arg3;
            uint8_t *ubmp = (uint8_t *)arg4;
            int cap = (int)arg5;
            ttf_glyph_t *g = ttf_get_glyph_f(face, (int)arg2, size, style);
            if (!g) return -1;
            if (meta) { meta[0]=g->width; meta[1]=g->height; meta[2]=g->xoff; meta[3]=g->yoff; meta[4]=g->advance; }
            if (g->bitmap && ubmp && cap >= g->width * g->height && g->width > 0 && g->height > 0)
                memcpy(ubmp, g->bitmap, (size_t)g->width * g->height);
            return (int64_t)g->advance;
        }
        case SYS_FONT_METRICS: {
            extern void ttf_get_metrics_f(int, int, int *, int *, int *);
            int face = (int)(arg1 & 0xFF);
            int size = (int)((arg1 >> 8) & 0xFFFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            int *out = (int *)arg2;
            if (!out) return -1;
            ttf_get_metrics_f(face, size, &out[0], &out[1], &out[2]);
            return 0;
        }
        case SYS_FONT_KERN: {
            extern int ttf_get_kerning_f(int, int, int, int);
            int face = (int)(arg1 & 0xFF);
            int size = (int)((arg1 >> 8) & 0xFFFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            return (int64_t)ttf_get_kerning_f(face, (int)arg2, (int)arg3, size);
        }
        case SYS_WIN_DRAW_TTF_EX: {
            int x = (int)(arg2 & 0xFFFF);
            int y = (int)((arg2 >> 16) & 0xFFFF);
            int face  = (int)(arg4 & 0xFF);
            int size  = (int)((arg4 >> 8) & 0xFFFF);
            int style = (int)((arg4 >> 24) & 0xFF);
            return sys_win_draw_text_ttf_ex((int)arg1, x, y, (const char *)arg3,
                                            (uint32_t)arg5, size, face, style);
        }
        case SYS_SET_WIN_BLANK:
            g_win_blit_suppressed = (int)arg1;
            return 0;
        case SYS_SET_WIN_OPACITY: {
            extern void wm_set_default_opacity(int opacity);
            wm_set_default_opacity((int)arg1);
            return 0;
        }
        case SYS_GET_WIN_OPACITY: {
            extern int wm_get_default_opacity(void);
            return (int64_t)wm_get_default_opacity();
        }
        case SYS_SET_CURSOR:   // (#116) arg1=style, arg2=size%
            g_cursor_style = (int)arg1;
            g_cursor_size  = (int)arg2;
            return 0;
        case SYS_GET_CURSOR:   // packed: style (low 8) | size<<8
            return (int64_t)((g_cursor_style & 0xFF) | ((g_cursor_size & 0xFFFF) << 8));
        case SYS_SET_SCREENSAVER:
            g_screensaver_type = (int)arg1;
            return 0;
        case SYS_GET_SCREENSAVER:
            return (int64_t)g_screensaver_type;
        case SYS_SET_SS_DELAY:
            g_screensaver_delay = (int)arg1;
            return 0;
        case SYS_GET_SS_DELAY:
            return (int64_t)g_screensaver_delay;
        case SYS_SET_SETTINGS_TAB:
            g_settings_tab = (int)arg1;
            return 0;
        case SYS_GET_SETTINGS_TAB:
            return (int64_t)g_settings_tab;
        case SYS_SCREENSAVER_TEST:
            g_ss_test = 1;
            return 0;
        case SYS_GET_SS_TEST: {
            int v = g_ss_test; g_ss_test = 0; return (int64_t)v;
        }
        case SYS_PLAY_WAV:
            return sys_play_wav((const char *)arg1);
        // Ring-3 PCM push (Phase 1, Ring-0 media exit). Additive: SYS_PLAY_WAV
        // above is untouched. All argument validation, the stream-ownership
        // check and the #426 wait-queue blocking live in drivers/audio_pcm.c.
        case SYS_AUDIO_PCM_OPEN:
            return sys_audio_pcm_open((uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3);
        case SYS_AUDIO_PCM_WRITE:
            return sys_audio_pcm_write((int)arg1, (const void *)arg2, (uint32_t)arg3);
        case SYS_AUDIO_PCM_CLOSE:
            return sys_audio_pcm_close((int)arg1);
        case SYS_AUDIO_POS_MS: {
            // #335: elapsed ms of the current USB-DAC stream = frames/rate.
            extern int uac_is_ready(void);
            extern uint64_t uac_frames_streamed(void);
            extern uint32_t uac_sample_rate(void);
            if (!uac_is_ready()) return -1;
            uint32_t r = uac_sample_rate(); if (!r) return -1;
            return (int64_t)(uac_frames_streamed() * 1000ULL / r);
        }
        case SYS_HDA_DBG: {
            // #71: userland HDA audio bring-up debug. arg1=op, arg2/3/4 = a/b/c.
            extern int64_t hda_debug_op(int op, uint64_t a, uint64_t b, uint64_t c);
            return hda_debug_op((int)arg1, (uint64_t)arg2, (uint64_t)arg3, (uint64_t)arg4);
        }
        case SYS_POWEROFF: {
            extern void gfx_boot_log_clear(void);
            extern void gfx_boot_splash(void);
            extern void gfx_boot_log(const char *);
            extern void acpi_shutdown(void);
            gfx_boot_log_clear();
            gfx_boot_splash();
            gfx_boot_log("Shutting down...");
            { extern volatile uint64_t timer_ticks; extern uint32_t g_timer_hz;
              uint32_t hz = g_timer_hz ? g_timer_hz : 250; uint64_t t0 = timer_ticks;
              volatile uint64_t cap = 0;
              while ((timer_ticks - t0) < (hz * 3 / 2) && cap < 4000000000ULL) cap++; }
            acpi_shutdown();
            return 0;
        }
        case SYS_REBOOT: {
            extern void gfx_boot_log_clear(void);
            extern void gfx_boot_splash(void);
            extern void gfx_boot_log(const char *);
            extern void acpi_reboot(void);
            gfx_boot_log_clear();
            gfx_boot_splash();
            gfx_boot_log("Restarting...");
            { extern volatile uint64_t timer_ticks; extern uint32_t g_timer_hz;
              uint32_t hz = g_timer_hz ? g_timer_hz : 250; uint64_t t0 = timer_ticks;
              volatile uint64_t cap = 0;
              while ((timer_ticks - t0) < (hz * 3 / 2) && cap < 4000000000ULL) cap++; }
            acpi_reboot();
            return 0;
        }
        case SYS_WIN_GET_POS:
            return sys_win_get_pos((int)arg1, (int *)arg2, (int *)arg3);
        case SYS_WIN_MOVE:
            return sys_win_move((int)arg1, (int)arg2, (int)arg3);
        case SYS_WIN_MOVE_BY:
            return sys_win_move_by((int)arg1, (int)arg2, (int)arg3);

        // Window manager query
        case SYS_WM_GET_WINDOWS:
            return sys_wm_get_windows((wm_window_info_t *)arg1, (int)arg2);
        case SYS_WM_FOCUS_WINDOW: {
            // Apps hold the user-window SLOT handle that win_create() returns, not
            // the internal window->id that sys_wm_focus_window() matches on. When a
            // process re-asserts focus on ITS OWN window by that slot handle (what
            // fullscreen games Arena/Squadron do every frame), resolve the slot
            // directly: the slot space (0..15) overlaps low window ids, so routing
            // it through the id path silently focused the WRONG window and the
            // game's WASD/keys went nowhere. Falls back to id matching (taskbar,
            // winswitch) when the handle is not a slot the caller owns.
            extern int64_t wm_focus_user_slot(int slot);
            if (wm_focus_user_slot((int)arg1) == 0) return 0;
            return sys_wm_focus_window((int)arg1);
        }
        case SYS_WM_MINIMIZE_WINDOW:
            return sys_wm_minimize_window((int)arg1);
        case SYS_WM_MAXIMIZE_WINDOW: {
            extern void wm_toggle_maximize_focused(void);
            wm_toggle_maximize_focused();
            return 0;
        }
        case SYS_RUN_NEXT_ON_AP: {
            // #279: mark the next user process this caller spawns as
            // migratable so the scheduler routes it to an application
            // processor (GUI `runap` / RC `launchap`).
            extern void proc_set_next_migratable(int v);
            proc_set_next_migratable(1);
            return 0;
        }
        case SYS_GET_MOUSE_SCROLL: {
            // OS-wide mouse wheel: return and clear the kernel scroll delta.
            extern int8_t mouse_get_scroll(void);
            return (int64_t)mouse_get_scroll();
        }
        case SYS_GET_GLOBAL_MOUSE:
            return sys_get_global_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3);
        case SYS_WIN_SET_NOCHROME:
            return sys_win_set_nochrome((int)arg1);
        case SYS_DNS_START:
            return sys_dns_start((const char *)arg1, (uint32_t *)arg2);
        case SYS_DNS_POLL:
            return sys_dns_poll((uint32_t *)arg1);
        case SYS_LIST_USERS:
            return sys_list_users((sc_user_info_t *)arg1, (int)arg2);
        case SYS_AUTHENTICATE:
            return sys_authenticate((const char *)arg1, (const char *)arg2);
        case SYS_DELETE_USER:
            return sys_delete_user((const char *)arg1);

        // IPC: Message Passing
        case SYS_MSG_CREATE_CHANNEL: return sys_msg_create_channel();
        case SYS_MSG_CONNECT:        return sys_msg_connect((int)arg1);
        case SYS_MSG_SEND:           return sys_msg_send((int)arg1, (const void *)arg2, (size_t)arg3);
        case SYS_MSG_RECV:           return sys_msg_recv((int)arg1, (void *)arg2, (size_t)arg3, (int)arg4);
        case SYS_MSG_ACCEPT:         return sys_msg_accept((int)arg1, (int)arg2);
        case SYS_MSG_CLOSE:          return sys_msg_close((int)arg1);
        case SYS_MSG_DESTROY:        return sys_msg_destroy_channel((int)arg1);

        // IPC: Shared Memory
        case SYS_SHM_CREATE:  return sys_shm_create((size_t)arg1, (int)arg2);
        case SYS_SHM_MAP:     return sys_shm_map((int)arg1, (void **)arg2);
        case SYS_SHM_UNMAP:   return sys_shm_unmap((int)arg1);
        case SYS_SHM_DESTROY: return sys_shm_destroy((int)arg1);
        case SYS_SHM_INFO:    return sys_shm_info((int)arg1, (size_t *)arg2, (uint32_t *)arg3);

        // IPC: Name Service
        case SYS_IPC_REGISTER_NAME: return sys_ipc_register_name((const char *)arg1, (int)arg2);
        case SYS_IPC_LOOKUP_NAME:   return sys_ipc_lookup_name((const char *)arg1);

        // Framebuffer / Compositor
        case SYS_FB_MAP:      return sys_fb_map();
        case SYS_FB_INFO:     return sys_fb_info((fb_info_user_t *)arg1);
        case SYS_FB_FLIP:     return sys_fb_flip();
        case SYS_FB_DAMAGE:   return sys_fb_damage((int)arg1, (int)arg2, (int)arg3, (int)arg4);
        case SYS_GET_MOUSE:   return sys_get_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3);
        case SYS_SET_MOUSE:   return sys_set_mouse((int)arg1, (int)arg2);
        case SYS_GET_KEY:     return sys_get_key((key_event_t *)arg1);
        case SYS_GRAB_INPUT:  return sys_grab_input((int)arg1);
        case SYS_INJECT_MOUSE: return sys_inject_mouse((int32_t)arg1, (int32_t)arg2, (int32_t)arg3, (int32_t)arg4);
        case SYS_SET_MOUSE_BUTTONS: return sys_set_mouse_buttons((uint32_t)arg1);
        case SYS_DECODE_IMAGE: return sys_decode_image((const void *)arg1, (uint32_t)arg2, (uint32_t)arg3, (void *)arg4, (uint32_t)arg5, (int *)arg6);
        case SYS_WIN_DRAW_IMAGE: return sys_win_draw_image((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5, (uint32_t *)arg6);

        case SYS_DUP: {
            int oldfd = (int)arg1;
            return fd_dup(oldfd, 0);
        }
        case SYS_DUP2: {
            int oldfd = (int)arg1;
            int newfd = (int)arg2;
            if (oldfd < 0 || newfd < 0 || oldfd >= MAX_FDS || newfd >= MAX_FDS) return -1;
            if (oldfd == newfd) return newfd;
            file_t *oldf = fd_get(oldfd);
            if (!oldf) return -1;
            // Close newfd if open
            if (fd_get(newfd)) fd_close(newfd);
            // Install old file at new fd
            file_get(oldf);
            fd_install(newfd, oldf);
            return newfd;
        }
        case SYS_PIPE: {
            int *user_pipefd = (int *)arg1;
            if (!user_pipefd) return -1;
            int pipefd[2];
            extern int pipe_create(int pipefd[2]);
            int ret = pipe_create(pipefd);
            if (ret == 0) {
                user_pipefd[0] = pipefd[0];
                user_pipefd[1] = pipefd[1];
            }
            return ret;
        }
        case SYS_IOCTL: {
            // Device control on an open fd (termios TCGETS/TCSETS, winsize, ...).
            // Without this, userland tcgetattr/tcsetattr always failed, so shells
            // (msh) and editors (mvi) could never enter raw mode over a pty/tty.
            file_t *f = fd_get((int)arg1);
            if (!f) return -1;
            extern int file_ioctl(file_t *f, unsigned cmd, void *arg);
            return file_ioctl(f, (unsigned)arg2, (void *)arg3);
        }
        case SYS_GET_NET_BYTES: {
            extern uint64_t net_total_bytes(void);
            return (int64_t)net_total_bytes();
        }
        case SYS_SSH_CLIENT: {
            // arg1=ipv4 (host byte order, a.b.c.d packed), arg2=user, arg3=pass,
            // arg4=(cols<<16)|rows, arg5=port. Bridges the kernel SSH-2 client to
            // this process's stdin (fd0) / stdout (fd1) and blocks until it ends.
            uint32_t ip = (uint32_t)arg1;
            const char *user = (const char *)arg2;
            const char *pass = (const char *)arg3;
            int cols = (int)((arg4 >> 16) & 0xffff);
            int rows = (int)(arg4 & 0xffff);
            uint16_t port = (uint16_t)arg5;
            if (!ip) return -1;
            file_t *fin = fd_get(0), *fout = fd_get(1);
            if (!fin || !fout) return -1;
            extern int ssh2_run_on_fds(uint32_t ip, uint16_t port, const char *user,
                                       const char *pass, int cols, int rows,
                                       void *fin, void *fout);
            return ssh2_run_on_fds(ip, port, user, pass, cols, rows, fin, fout);
        }
        case SYS_NET_INFO: {
            char *ubuf = (char *)arg1;
            unsigned long ulen = (unsigned long)arg2;
            if (!ubuf || ulen == 0) return -1;
            extern int net_format_info(char *buf, unsigned long len);
            return net_format_info(ubuf, ulen);
        }
        case SYS_SETPRIORITY: {
            // arg1 = pid (<=0 means the calling process), arg2 = level 0..4
            // (PRIO_IDLE..PRIO_REALTIME). Lets background apps/services demote
            // themselves so interactive work gets the CPU. Background kernel
            // threads (RemoteCtrl, sshd) are already PRIO_LOW.
            int pid = (int)arg1;
            int level = (int)arg2;
            if (level < PRIO_IDLE) level = PRIO_IDLE;
            if (level > PRIO_REALTIME) level = PRIO_REALTIME;
            process_t *tp = (pid <= 0) ? proc_current() : proc_get((uint32_t)pid);
            if (!tp) return -1;
            tp->priority = (process_priority_t)level;
            return 0;
        }
        case SYS_GET_CPU_USAGE: {
            extern int proc_get_cpu_usage(void);
            return proc_get_cpu_usage();
        }
        case SYS_GET_CPU_PER_CORE: {
            // Fill user buffer: [0]=core count, [1..count]=per-core CPU %% (0-100).
            extern int smp_get_core_count(void);
            extern int smp_get_core_pct(uint32_t);
            uint32_t *buf = (uint32_t *)arg1;
            if (!buf) return 0;
            int n = smp_get_core_count();
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            buf[0] = (uint32_t)n;
            for (int i = 0; i < n; i++) buf[1 + i] = (uint32_t)smp_get_core_pct((uint32_t)i);
            return n;
        }
        case SYS_GET_MEM_INFO: {
            extern uint64_t pmm_get_total_pages(void);
            extern uint64_t pmm_get_used_pages(void);
            unsigned long *tp = (unsigned long *)arg1;
            unsigned long *up = (unsigned long *)arg2;
            if (tp) *tp = (unsigned long)(pmm_get_total_pages() * 4096ULL);
            if (up) *up = (unsigned long)(pmm_get_used_pages() * 4096ULL);
            return 0;
        }

        case SYS_PROC_LIST:
            return proc_snapshot((proc_info_t *)arg1, (int)arg2);

        // #487/#349 Ring-3 process introspection. Unlike their neighbours here,
        // each of these VALIDATES its user pointer in the backend before any
        // write (see proc/procinfo.h), and every byte copied into the caller's
        // buffer is copied by a bounded Rust builder.
        case SYS_PROC_HANDLES:
            return sys_proc_handles((uint32_t)arg1, (void *)arg2, (int)arg3);

        case SYS_NET_CONNS:
            return sys_net_conns((uint32_t)arg1, (void *)arg2, (int)arg3);

        case SYS_SVC_LIST:
            return sys_svc_list((void *)arg1, (int)arg2);

        case SYS_SVC_CONTROL:
            return sys_svc_control((const char *)arg1, (int)arg2);

        case SYS_PROC_DETAIL:
            return sys_proc_detail((uint32_t)arg1, (void *)arg2);

        case SYS_COMPOSITOR_RENDER_WINDOWS:
            // Draw KWM window frames and app content onto the compositor framebuffer.
            // The compositor owns fb_back; it draws its desktop first, then we overlay
            // any open KWM windows (terminal, settings, etc.) on top.
            wm_draw_all();
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content
            return 0;

        case SYS_GET_KEYBOARD: {
            // While a Win16 app owns the screen, its own message pump
            // (win16_pump_input) must be the sole keyboard consumer; do not let
            // the compositor drain the key buffer or game keys are lost.
            extern volatile int g_win16_owns_screen;
            if (g_win16_owns_screen) return -1;
            return keyboard_has_char() ? keyboard_get_char() : -1;
        }

        case SYS_INJECT_KEY: {
            // Compositor forwards a raw keycode from the hardware queue to the
            // focused KWM window via wm_dispatch_event so user_window_event_handler
            // queues it into the per-window user event queue for win_get_event().
            int key = (int)arg1;
            gui_event_t ev;
            memset(&ev, 0, sizeof(ev));
            if (key >= 0x90 && key <= 0x98) {
                ev.type = EVENT_KEY_UP;
                ev.keycode = key - 0x10;
            } else if (key >= 0x80 && key < 0x90) {
                ev.type = EVENT_KEY_DOWN;
                ev.keycode = key;
            } else if (key > 0x98) {
                ev.type = EVENT_KEY_UP;
                ev.keycode = key & 0x7F;
            } else {
                ev.type = EVENT_KEY_DOWN;
                ev.keycode = key;
                ev.key_char = (char)key;
            }
            wm_dispatch_event(&ev);
            return 0;
        }

        case SYS_DEV_PCI_LIST: {
            extern int64_t devinfo_pci_list(devinfo_pci_t *, int);
            return devinfo_pci_list((devinfo_pci_t *)arg1, (int)arg2);
        }
        case SYS_DEV_USB_LIST: {
            extern int64_t devinfo_usb_list(devinfo_usb_t *, int);
            return devinfo_usb_list((devinfo_usb_t *)arg1, (int)arg2);
        }
        case SYS_DEV_IRQ_LIST: {
            extern int64_t devinfo_irq_list(devinfo_irq_t *, int);
            return devinfo_irq_list((devinfo_irq_t *)arg1, (int)arg2);
        }
        case SYS_SYSINFO: {
            extern int64_t devinfo_sysinfo(devinfo_sysinfo_t *);
            return devinfo_sysinfo((devinfo_sysinfo_t *)arg1);
        }

        // #265 cron-like timer/scheduler
        case SYS_CRON_ADD:
            return cron_add((const cron_job_t *)arg1);
        case SYS_CRON_LIST:
            return cron_list((cron_job_t *)arg1, (int)arg2);
        case SYS_CRON_REMOVE:
            return cron_remove((uint32_t)arg1);
        case SYS_CRON_ENABLE:
            return cron_enable((uint32_t)arg1, (int)arg2);

        // #430: Signals (implemented in proc/signal.c). The return-work hook in
        // syscall.asm already delivers pending signals on the way back to
        // userland; these cases simply install/raise/mask/return.
        case SYS_KILL:
            return sys_kill((int)arg1, (int)arg2);
        case SYS_TKILL:                 // tid == pid in our thread model
            return sys_kill((int)arg1, (int)arg2);
        case SYS_TGKILL:                // (tgid, tid, sig) -> deliver to tid
            return sys_kill((int)arg2, (int)arg3);
        case SYS_SIGACTION:
            return sys_sigaction((int)arg1, (const void *)arg2, (void *)arg3);
        case SYS_SIGPROCMASK:
            return sys_sigprocmask((int)arg1, (const uint64_t *)arg2,
                                   (uint64_t *)arg3);
        case SYS_SIGRETURN:
            return sys_rt_sigreturn();
        case SYS_ALARM:
            return sys_alarm((uint32_t)arg1);
        case SYS_PAUSE:
            return sys_pause();

        // #430: Threads + futex (proc/process.c clone, sync/futex.c futex).
        case SYS_CLONE:
            return proc_clone((uint32_t)arg1, (void *)arg2, (uint32_t *)arg3,
                              (uint32_t *)arg4, (void *)arg5);
        case SYS_GETTID:
            return (int64_t)proc_gettid();
        case SYS_SET_TID_ADDRESS:
            return (int64_t)proc_set_tid_address((uint32_t *)arg1);
        case SYS_FUTEX:
            return sys_futex((uint32_t *)arg1, (int)arg2, (uint32_t)arg3,
                             (uint64_t)arg4, (uint32_t *)arg5, (uint32_t)arg6);

                default:
            kprintf("[SYSCALL] Unknown syscall %lu\n", num);
            return -1;
    }
}

// ============================================================================
// Process syscalls
// ============================================================================

int64_t sys_exit(int exit_code) {
    kprintf("[SYSCALL] exit(%d)\n", exit_code);
    proc_exit(exit_code);
    return 0;  // Never reached
}

int64_t sys_fork(void) {
    return proc_fork();
}


// SYS_SPAWN_ARGS: spawn a new process with argv
// arg1 = path (user string), arg2 = argv[] (user pointer array), arg3 = argc
// Open a redirect target as a struct file_t, choosing the ext2 backing for the
// ext2 root volume (and the explicit /ext2 mount) and FAT otherwise. Returns
// NULL on error.
static file_t *open_redir_file(const char *path, int flags) {
    if (!path || !path[0]) return NULL;
    if (path_is_ext2(path)) {
        return ext2_vfs_open(ext2_relpath(path), flags);
    }
    if (path_root_ext2(path)) {
        // Use ext2 if the file already exists there or we are creating it;
        // otherwise fall back to FAT (ESP-only files).
        if (ext2_resolve_path(path) != 0 || (flags & O_CREAT)) {
            return ext2_vfs_open(path, flags);
        }
    }
    return fat_vfs_open(path, flags);
}

static int64_t spawn_impl(const char *path, char **argv, int argc,
                          const char *infile, const char *outfile, int append) {
    if (!path || argc < 0) return -1;
    if (argc > 64) argc = 64;

    // #95: a service may only spawn child processes if granted SVC_PERM_SPAWN.
    // No-op for normal processes (is_service == 0).
    {
        process_t *cur = proc_current();
        if (cur && cur->is_service && !(cur->svc_perms & SVC_PERM_SPAWN)) {
            return -1;  // EPERM: service lacks spawn capability
        }
    }

    extern fat_fs_t g_fat_fs;
    if (!g_fat_fs.mounted) return -1;

    // Read the ELF file from disk
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) return -1;

    // Validate ELF
    if (elf_validate(data, size) != 0) {
        kfree(data);
        return -1;
    }

    // Build kernel-side argv array by copying user strings into kernel buffers.
    // The string buffer (64 args x 256 bytes = 16KB) is allocated on the HEAP,
    // not on the kernel stack. A 16KB automatic array here previously risked
    // overflowing the 64KB kernel stack: this function keeps the buffer live
    // while calling proc_create_user() (FAT read, ELF load, address-space
    // setup, setup_user_argv), and any interrupt/scheduler nesting on top of
    // that could push the stack past its limit and corrupt adjacent kernel
    // heap memory. The corruption manifested as nondeterministic faults in the
    // *calling* process (e.g. a terminal spawning grep) after a variable number
    // of spawns.
    char (*kbuf)[256] = kmalloc(64 * 256);
    if (!kbuf) { kfree(data); return -1; }
    char *kargv[65];
    int kargc = 0;

    if (argv && argc > 0) {
        for (int i = 0; i < argc && i < 64; i++) {
            if (!argv[i]) break;
            // Copy from user pointer to kernel buffer
            const char *usrc = argv[i];
            int j = 0;
            while (j < 255 && usrc[j]) {
                kbuf[i][j] = usrc[j];
                j++;
            }
            kbuf[i][j] = '\0';
            kargv[i] = kbuf[i];
            kargc++;
        }
    }
    kargv[kargc] = NULL;

    // Extract program name from path
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }

    // Create the user process with argv. proc_create_user() (via
    // setup_user_argv) copies the argv strings onto the child's user stack, so
    // the kernel-side string buffer can be released as soon as it returns.
    int pid = proc_create_user(name, data, size, kargc > 0 ? kargv : NULL, NULL);
    kfree(data);
    kfree(kbuf);

    // Inherit stdio fds from the calling process so that the child writes
    // to the same PTY (or console) as the parent. Without this, children
    // spawned by msh in a terminal get /dev/console and their output goes
    // to serial instead of the terminal window.
    if (pid > 0 && argv) {   // only inherit stdio for argv spawns (msh/terminal); compositor/desktop apps keep their own console (#75)
        extern process_t *proc_get(uint32_t pid);
        process_t *parent = proc_current();
        process_t *child  = proc_get((uint32_t)pid);
        if (parent && child) {
            for (int fi = 0; fi < 3; fi++) {
                if (parent->fds[fi]) {
                    // Close the /dev/console fd that init_proc opened
                    if (child->fds[fi]) file_put(child->fds[fi]);
                    // Replace with parent's fd (same PTY slave)
                    child->fds[fi] = parent->fds[fi];
                    file_get(parent->fds[fi]);
                }
            }
        }
    }

    // Shell I/O redirection (#redirect): replace the child's stdin/stdout with
    // file-backed struct file_t objects, overriding the inherited PTY fds. The
    // child writes/reads through these; on child exit fd_close_all() releases
    // them, which commits a buffered write to disk (ext2_write_file / FAT).
    if (pid > 0 && (infile || outfile)) {
        extern process_t *proc_get(uint32_t pid);
        process_t *child = proc_get((uint32_t)pid);
        if (child) {
            if (outfile && outfile[0]) {
                file_t *of = open_redir_file(outfile,
                                 O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
                if (of) {
                    if (child->fds[1]) file_put(child->fds[1]);
                    child->fds[1] = of;
                }
            }
            if (infile && infile[0]) {
                file_t *inf = open_redir_file(infile, O_RDONLY);
                if (inf) {
                    if (child->fds[0]) file_put(child->fds[0]);
                    child->fds[0] = inf;
                }
            }
        }
    }

    return (int64_t)pid;
}

static int64_t sys_spawn_args(const char *path, char **argv, int argc) {
    return spawn_impl(path, argv, argc, NULL, NULL, 0);
}

static int64_t sys_spawn_redir(const char *path, char **argv, int argc,
                               const char *infile, const char *outfile, int append) {
    return spawn_impl(path, argv, argc, infile, outfile, append);
}

int64_t sys_exec(const char *path) {
    if (!path) return -1;
    extern int proc_execve_arm(const char *path, char **argv, char **envp);
    return proc_execve_arm(path, NULL, NULL);
}

int64_t sys_getpid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->pid : -1;
}

int64_t sys_getppid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->ppid : -1;
}

int64_t sys_yield(void) {
    proc_yield();
    return 0;
}

int64_t sys_sleep(uint32_t ms) {
    proc_sleep(ms);
    return 0;
}

// ============================================================================
// File I/O syscalls (using FAT filesystem)
// ============================================================================

// Simple file descriptor table (legacy kernel-wide table)
// NOTE: process.h defines MAX_FDS=64 for the VFS per-process fd table.
// This legacy table is used for basic FAT file access until full VFS migration.
#define LEGACY_MAX_FDS 16
static fat_file_t fd_table[LEGACY_MAX_FDS];
static int fd_used[LEGACY_MAX_FDS];

// #444: fd_used[]/e2fd[]/smbfd[] above are a SYSTEM-WIDE table (only 16 slots,
// 13 usable) shared by every process on the box, but sys_open()'s "find a free
// slot" scan used to be completely unlocked. Two processes calling sys_open()
// concurrently could both scan, both see the SAME slot as free, and both
// proceed to populate it (e2fd[fd]/smbfd[fd]) with their OWN file's data -
// whichever one finished last would silently win, so BOTH callers were handed
// back the same fd number and the loser's process would from then on read (and
// on close, free) the WINNER's file content instead of its own. This is a
// second, independent root cause of the #444 CPython "corrupt reads" symptom
// on top of the ATA/ext2-cache lazy-lock-init race fixed in drivers/ata.c and
// fs/ext2.c: proven live by a 4-independent-process concurrent-open hammer
// test where 3 of 4 processes opening 4 DIFFERENT files ended up reading the
// 4th process's exact file content byte-for-byte. Fixed by claiming the slot
// (fd_used[i] = 1) ATOMICALLY under this lock in the same scan that finds it,
// so no other caller can ever observe it as free once one caller has picked
// it; every failure path after the claim now releases the slot back
// (fd_used[fd] = 0) before returning. The lock is held only for the tiny
// scan+claim, never across the (possibly slow / possibly network-blocking for
// SMB/NFS) population work that follows, so this cannot turn into a
// hold-a-spinlock-across-a-blocking-call bug (#426).
static spinlock_t g_legacy_fd_lock = SPINLOCK_INIT;

// Release a legacy fd slot back to the free pool under g_legacy_fd_lock, so
// this write can never race with the locked scan-and-claim in sys_open().
static inline void legacy_fd_release(int fd) {
    uint64_t fl = spinlock_acquire_irqsave(&g_legacy_fd_lock);
    fd_used[fd] = 0;
    spinlock_release_irqrestore(&g_legacy_fd_lock, fl);
}

// ---- #99 Phase B: additive ext2 mount at the "/ext2" path prefix ------------
// ext2-backed fds run in parallel with the FAT fd_table (same fd numbers, tagged
// by e2fd[fd].used). FAT paths never touch this code, so existing behavior is
// byte-identical. Read opens cache the whole file; create opens buffer writes and
// flush on close (matches the FAT write-buffer model; no in-place overwrite yet).
typedef struct {
    int      used;
    int      is_dir;
    char     path[256];        // ext2-relative path (always starts with '/')
    uint8_t *rbuf; uint32_t rsize, rpos;   // file read cache
    uint8_t *wbuf; uint32_t wcap, wlen; int writing;  // create-on-close buffer
    uint32_t dir_ino, dir_pos; // directory iteration cursor
} ext2_fd_t;
static ext2_fd_t e2fd[LEGACY_MAX_FDS];

static int path_is_ext2(const char *p) {
    return p && p[0]=='/' && p[1]=='e' && p[2]=='x' && p[3]=='t' && p[4]=='2' &&
           (p[5]=='\0' || p[5]=='/');
}

// ---- #317 pass 2: SMB-backed fds, parallel to the ext2 e2fd table ----------
// Routes userland open/read/write/close/readdir/stat on "/SMB/<server>/<share>/
// <path>" through the SMB2 client (net/smb.c). Reads cache the whole file (like
// ext2); writes buffer and flush-on-close as an SMB upload (smb_vfs_write_whole);
// directory fds hold an smb dir-handle. FAT/ext2 paths never touch this code.
typedef struct {
    int      used;
    int      is_dir;
    int      is_nfs;           // #317 pass 4: 0 = SMB share, 1 = NFS export
    int      dirh;             // smb/nfs dir handle (is_dir)
    char     path[260];        // full /SMB/... or /NFS/... path
    uint8_t *rbuf; uint32_t rsize, rpos;            // file read cache
    uint8_t *wbuf; uint32_t wcap, wlen; int writing; // upload-on-close buffer
} smb_fd_t;
static smb_fd_t smbfd[LEGACY_MAX_FDS];

static int path_is_smb(const char *p) {
    return smb_vfs_is_smb_path(p);
}
// #317 pass 4: NFS exports use the same smbfd[] table (read/write/seek are
// fs-agnostic, operating on the cached rbuf/wbuf); only mount/stat/opendir/
// readdir/closedir/upload differ and branch on s->is_nfs.
static int path_is_nfs(const char *p) {
    return nfs_vfs_is_nfs_path(p) ? 1 : 0;
}
// "/ext2" -> "/", "/ext2/a/b" -> "/a/b"
static const char *ext2_relpath(const char *p) {
    const char *r = p + 5;
    return (*r == '\0') ? "/" : r;
}
// #99 cutover: true when ext2 is the root fs and `p` is a normal "/" path that
// should be served from ext2 (the UEFI ESP paths /boot, /EFI are never routed).
static int path_root_ext2(const char *p) {
    if (!g_root_ext2 || !p || p[0] != '/') return 0;
    if (path_is_ext2(p)) return 0;   // explicit /ext2 handled separately
    if (p[1]=='b'&&p[2]=='o'&&p[3]=='o'&&p[4]=='t'&&(p[5]=='/'||p[5]==0)) return 0;
    if (p[1]=='E'&&p[2]=='F'&&p[3]=='I'&&(p[4]=='/'||p[4]==0)) return 0;
    return 1;
}

/* #359 Phase 2: POSIX errno values so libc open() can set errno correctly
   (CPython's import machinery needs a missing file to raise FileNotFoundError,
   i.e. errno==ENOENT, not a bare OSError). sys_open now returns -errno. */
#ifndef MOS_EOK
#define MOS_EPERM   1
#define MOS_ENOENT  2
#define MOS_EBADF   9
#define MOS_ENOMEM  12
#define MOS_EACCES  13
#define MOS_EINVAL  22
#define MOS_EMFILE  24
#define MOS_EOK     0
#endif

/* #359: fcntl(fd, cmd, arg). CPython needs F_GETFL/F_SETFL/F_GETFD/F_SETFD/
   F_DUPFD to set up std fds and duplicate descriptors. We do not track a
   per-fd flags word, so F_GETFL reports O_RDWR and F_SETFL is a no-op; the
   duplicate commands reuse the existing per-process fd_dup(). */
int64_t sys_fcntl(int fd, int cmd, long arg) {
    switch (cmd) {
        case 0:    /* F_DUPFD          */
        case 1030: /* F_DUPFD_CLOEXEC  */ {
            int r = fd_dup(fd, (int)arg);
            return (r < 0) ? -MOS_EBADF : r;
        }
        case 1: return 0;        /* F_GETFD -> no FD_CLOEXEC tracked */
        case 2: return 0;        /* F_SETFD -> accept, ignore        */
        case 3: return 0x0002;   /* F_GETFL -> O_RDWR                */
        case 4: return 0;        /* F_SETFL -> accept, ignore        */
        default: return 0;       /* be permissive for other probes   */
    }
}

// #444: once a legacy fd slot has been eagerly claimed (fd_used[fd] = 1) by
// the scan in sys_open(), every failure path must release it again before
// returning, or a run of failed opens (e.g. probing for an optional config
// file that doesn't exist) permanently leaks slots out of the tiny 13-usable
// pool. FD_FAIL() is only valid inside sys_open(), after `fd` has been
// assigned and validated (fd >= 0).
#define FD_FAIL(x) do { legacy_fd_release(fd); return (x); } while (0)

int64_t sys_open(const char *path, int flags) {
    // #396: /dev/<name> device nodes (CDC-ACM serial, etc.) resolve through the
    // in-kernel dev namespace and install a file_t in the per-process fd table.
    if (path && path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/') {
        extern struct file *dev_open(const char *name, int flags);
        extern int fd_alloc_install(struct file *f);
        extern void file_put(struct file *f);
        struct file *df = dev_open(path + 5, flags);
        if (!df) return -1;
        int nfd = fd_alloc_install(df);
        if (nfd < 0) { file_put(df); return -1; }
        return nfd;
    }
    // Permission check
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        // #95: services are sandboxed to their declared capabilities. A
        // service without SVC_PERM_FSWRITE may not open files for writing
        // or create them. No-op for normal processes (is_service == 0).
        if (p->is_service && (flags & (1 | 2 | 0x40)) &&
            !(p->svc_perms & SVC_PERM_FSWRITE)) {
            return -MOS_EPERM;  // service lacks fs-write capability
        }
        int access = R_OK;
        if (flags & 1)  access = W_OK;          // O_WRONLY
        if (flags & 2)  access = R_OK | W_OK;   // O_RDWR
        // #317: SMB/NFS shares enforce access server-side (NTLM/RPC auth + share
        // ACLs); local POSIX perms (which default-deny W_OK to non-root) don't apply.
        if (!path_is_smb(path) && !path_is_nfs(path) &&
            perms_check(path, p->euid, p->egid, access) != 0) {
            return -MOS_EACCES;
        }
    }

    (void)flags;

    // Find a free fd and claim it ATOMICALLY (fd_used[i] = 1 while still
    // holding the lock) so no concurrent sys_open() can observe the same slot
    // as free (see the #444 comment on g_legacy_fd_lock above). Every failure
    // path below this point must reset fd_used[fd] = 0 before returning.
    int fd = -1;
    {
        uint64_t __fdfl = spinlock_acquire_irqsave(&g_legacy_fd_lock);
        for (int i = 3; i < LEGACY_MAX_FDS; i++) {  // 0, 1, 2 reserved for stdin/stdout/stderr
            if (!fd_used[i]) {
                fd = i;
                fd_used[i] = 1;
                break;
            }
        }
        spinlock_release_irqrestore(&g_legacy_fd_lock, __fdfl);
    }

    if (fd < 0) {
        return -MOS_EMFILE;  // no free fd
    }

    // #317 pass 2: SMB network share. Mount on demand, then open as a directory
    // (dir handle), a read cache (whole-file), or a write/upload buffer.
    if (path_is_smb(path)) {
        smb_fd_t *s = &smbfd[fd];
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        { int z = 0; while (path[z] && z < 259) { s->path[z] = path[z]; z++; } s->path[z] = 0; }
        if (smb_vfs_ensure_mount(path) != 0) FD_FAIL(-1);

        smb_dirent_t info;
        int have = (smb_stat(path, &info) == 0);
        int want_write = (flags & 0x1) || (flags & 0x2) || (flags & 0x40) || (flags & 0x200);

        if (have && info.is_directory) {
            int dh = smb_opendir(path);
            if (dh < 0) FD_FAIL(-1);
            s->is_dir = 1; s->dirh = dh;
        } else if (want_write) {
            // O_WRONLY/O_RDWR/O_CREAT/O_TRUNC: buffer writes, upload on close.
            s->writing = 1; s->wcap = 4096; s->wlen = 0;
            s->wbuf = (uint8_t *)kmalloc(s->wcap);
            if (!s->wbuf) FD_FAIL(-1);
        } else if (have) {
            // Read: cache the whole file.
            uint32_t sz = 0;
            s->rbuf = (uint8_t *)smb_vfs_read_whole(path, &sz);
            if (!s->rbuf) FD_FAIL(-1);
            s->rsize = sz; s->rpos = 0;
        } else {
            FD_FAIL(-MOS_ENOENT);  // not found and not creating
        }
        s->used = 1;
        fd_used[fd] = 1;
        return fd;
    }

    // #317 pass 4: NFS export. Same smbfd[] slot, is_nfs=1. Mount must exist
    // (NETMOUNTS.CFG at boot or an explicit SYS_NET_MOUNT). nfs_getattr decides
    // directory vs file; reads cache the whole file, writes upload on close.
    if (path_is_nfs(path)) {
        smb_fd_t *s = &smbfd[fd];
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        { int z = 0; while (path[z] && z < 259) { s->path[z] = path[z]; z++; } s->path[z] = 0; }
        s->is_nfs = 1;
        if (nfs_vfs_ensure_mount(path) != 0) FD_FAIL(-1);

        nfs_fattr3_t attrs;
        int have = (nfs_getattr(path, &attrs) == 0);
        int want_write = (flags & 0x1) || (flags & 0x2) || (flags & 0x40) || (flags & 0x200);

        if (have && attrs.type == NF3DIR) {
            int dh = nfs_opendir(path);
            if (dh < 0) FD_FAIL(-1);
            s->is_dir = 1; s->dirh = dh;
        } else if (want_write) {
            s->writing = 1; s->wcap = 4096; s->wlen = 0;
            s->wbuf = (uint8_t *)kmalloc(s->wcap);
            if (!s->wbuf) FD_FAIL(-1);
        } else if (have) {
            uint32_t sz = 0;
            s->rbuf = (uint8_t *)nfs_vfs_read_whole(path, &sz);
            if (!s->rbuf) FD_FAIL(-1);
            s->rsize = sz; s->rpos = 0;
        } else {
            FD_FAIL(-1);
        }
        s->used = 1;
        fd_used[fd] = 1;
        return fd;
    }

    // #99: serve from the ext2 volume for explicit "/ext2..." paths, and for all
    // "/" paths once ext2 is the root fs. For root-cutover paths we use ext2 when
    // the file already exists there or we are creating it; otherwise we fall
    // through to FAT so files that live only on the ESP still open.
    const char *rel = 0;
    // Normalize a bare (root-relative) filename to an absolute path so the
    // ext2-root redirect can resolve it. Userland apps open assets by bare name
    // (e.g. the compositor opens wallpapers as "MAYTERA.BMP"); both
    // path_root_ext2() and ext2_resolve_path() require a leading '/', so without
    // this a bare name never reaches ext2 and only resolves against the FAT ESP -
    // which on an ext2-root system holds boot files only, so the open fails and
    // e.g. the desktop wallpaper silently falls back to a gradient. Only active
    // when g_root_ext2 is set, so FAT-root behavior is byte-identical.
    char ext2_npath[260];
    const char *look = path;
    if (g_root_ext2 && path && path[0] != '/' && path[0] != '\0') {
        ext2_npath[0] = '/';
        int z = 0;
        while (path[z] && z < 258) { ext2_npath[z + 1] = path[z]; z++; }
        ext2_npath[z + 1] = '\0';
        look = ext2_npath;
    }
    if (path_is_ext2(path)) {
        rel = ext2_relpath(path);
    } else if (path_root_ext2(look)) {
        if (ext2_resolve_path(look) != 0 || (flags & 0x40)) rel = look;
    }
    if (rel) {
        ext2_fd_t *e = &e2fd[fd];
        for (uint64_t z = 0; z < sizeof(*e); z++) ((uint8_t *)e)[z] = 0;
        { int z = 0; while (rel[z] && z < 255) { e->path[z] = rel[z]; z++; } e->path[z] = 0; }
        uint32_t ino = ext2_resolve_path(rel);
        if (ino) {
            ext2_inode_t in;
            if (ext2_read_inode(ino, &in) != 0) FD_FAIL(-1);
            if ((in.i_mode & 0xF000) == 0x4000) {       // directory
                e->is_dir = 1; e->dir_ino = ino; e->dir_pos = 0;
            } else if ((flags & 0x3) || (flags & 0x200)) { // O_WRONLY/O_RDWR/O_TRUNC: overwrite
                // Existing regular file opened for writing: buffer the new
                // contents; ext2_write_file() truncates the inode in place on
                // close (#99 Phase C overwrite).
                e->writing = 1; e->wcap = 4096; e->wlen = 0;
                e->wbuf = (uint8_t *)kmalloc(e->wcap);
                if (!e->wbuf) FD_FAIL(-1);
            } else {
                uint32_t sz = in.i_size;
                e->rbuf = (uint8_t *)kmalloc(sz ? sz : 1);
                if (!e->rbuf) FD_FAIL(-1);
                int64_t n = sz ? ext2_read_file_ino(ino, e->rbuf, sz) : 0;
                e->rsize = (n > 0) ? (uint32_t)n : 0;
                e->rpos = 0;
            }
        } else if (flags & 0x40) {                       // O_CREAT
            e->writing = 1; e->wcap = 4096; e->wlen = 0;
            e->wbuf = (uint8_t *)kmalloc(e->wcap);
            if (!e->wbuf) FD_FAIL(-1);
        } else {
            FD_FAIL(-MOS_ENOENT);
        }
        e->used = 1;
        fd_used[fd] = 1;
        return fd;
    }

    // TODO: Validate user pointer
    // Open the file. If it does not exist and O_CREAT (0x40) is set, create it
    // first so userland tools (cp, mv, editors, curl -o, ...) can make new files.
    extern int fat_create(fat_fs_t *fs, const char *path);
    if (fat_open(&g_fat_fs, path, &fd_table[fd]) != 0) {
        if ((flags & 0x40) && fat_create(&g_fat_fs, path) == 0 &&
            fat_open(&g_fat_fs, path, &fd_table[fd]) == 0) {
            // created and opened successfully
        } else {
            FD_FAIL(-MOS_ENOENT);
        }
    }

    fd_used[fd] = 1;
    return fd;
}
#undef FD_FAIL

int64_t sys_close(int fd) {
    if (fd < 0) return -1;

    // First try per-process file descriptors (pipes, PTYs, etc.)
    process_t *proc = proc_current();
    if (proc && fd < MAX_FDS && proc->fds[fd]) {
        return fd_close(fd);
    }

    // #317: SMB-backed fd. Flush an upload (write-on-close), close dir handle.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        int rc = 0;
        if (s->is_nfs) {
            if (s->is_dir) {
                nfs_closedir(s->dirh);
            } else if (s->writing && s->wbuf) {
                rc = nfs_vfs_write_whole(s->path, s->wbuf, s->wlen);
            }
        } else if (s->is_dir) {
            smb_closedir(s->dirh);
        } else if (s->writing && s->wbuf) {
            rc = smb_vfs_write_whole(s->path, s->wbuf, s->wlen);
        }
        if (s->rbuf) kfree(s->rbuf);
        if (s->wbuf) kfree(s->wbuf);
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        legacy_fd_release(fd);
        return rc;
    }

    // ext2-backed fd: flush a buffered create to the ext2 volume, free buffers.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        int rc = 0;
        if (e->writing && e->wbuf) rc = ext2_write_file(e->path, e->wbuf, e->wlen);
        if (e->rbuf) kfree(e->rbuf);
        if (e->wbuf) kfree(e->wbuf);
        for (uint64_t z = 0; z < sizeof(*e); z++) ((uint8_t *)e)[z] = 0;
        legacy_fd_release(fd);
        return rc;
    }

    // Fallback: legacy FAT fd table
    if (fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }
    fat_close(&fd_table[fd]);
    legacy_fd_release(fd);
    return 0;
}

int64_t sys_read(int fd, void *buf, size_t count) {
    // Route through per-process file descriptors (PTY, pipes, etc.)
    process_t *proc = proc_current();
    if (proc && fd >= 0 && fd < 64 && proc->fds[fd]) {
        extern int64_t file_read(struct file *f, void *buf, size_t count);
        return file_read(proc->fds[fd], buf, count);
    }

    // #317: SMB-backed fd: serve from the cached file image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (s->is_dir || !s->rbuf) return -1;
        uint32_t avail = (s->rpos < s->rsize) ? (s->rsize - s->rpos) : 0;
        uint32_t n = (count < avail) ? (uint32_t)count : avail;
        if (n) { memcpy(buf, s->rbuf + s->rpos, n); s->rpos += n; }
        return (int64_t)n;
    }

    // ext2-backed fd: serve from the cached file image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (e->is_dir || !e->rbuf) return -1;
        uint32_t avail = (e->rpos < e->rsize) ? (e->rsize - e->rpos) : 0;
        uint32_t n = (count < avail) ? (uint32_t)count : avail;
        if (n) { memcpy(buf, e->rbuf + e->rpos, n); e->rpos += n; }
        return (int64_t)n;
    }

    // Fallback: legacy fd table for FAT files
    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    return fat_read(&fd_table[fd], buf, count);
}

int64_t sys_write(int fd, const void *buf, size_t count) {
    // Route through per-process file descriptors (PTY, pipes, etc.)
    process_t *proc = proc_current();
    if (proc && fd >= 0 && fd < 64 && proc->fds[fd]) {
        extern int64_t file_write(struct file *f, const void *buf, size_t count);
        return file_write(proc->fds[fd], buf, count);
    }

    // #317: SMB-backed fd: buffer writes; uploaded to the share on close.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (!s->writing || !s->wbuf) return -1;
        if (s->wlen + count > s->wcap) {
            uint32_t ncap = s->wcap ? s->wcap : 4096;
            while (ncap < s->wlen + count) ncap *= 2;
            uint8_t *nb = (uint8_t *)kmalloc(ncap);
            if (!nb) return -1;
            memcpy(nb, s->wbuf, s->wlen);
            kfree(s->wbuf); s->wbuf = nb; s->wcap = ncap;
        }
        memcpy(s->wbuf + s->wlen, buf, count);
        s->wlen += (uint32_t)count;
        return (int64_t)count;
    }

    // ext2-backed fd: buffer writes; flushed to the ext2 volume on close.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (!e->writing || !e->wbuf) return -1;
        if (e->wlen + count > e->wcap) {
            uint32_t ncap = e->wcap ? e->wcap : 4096;
            while (ncap < e->wlen + count) ncap *= 2;
            uint8_t *nb = (uint8_t *)kmalloc(ncap);
            if (!nb) return -1;
            memcpy(nb, e->wbuf, e->wlen);
            kfree(e->wbuf); e->wbuf = nb; e->wcap = ncap;
        }
        memcpy(e->wbuf + e->wlen, buf, count);
        e->wlen += (uint32_t)count;
        return (int64_t)count;
    }

    // Fallback: handle stdout/stderr via serial console
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;

        for (size_t i = 0; i < count; i++) {
            kputc(p[i]);
        }

        if (count > 0 && count < 256) {
            char msg_buf[256];
            size_t copy_len = count < 255 ? count : 255;
            memcpy(msg_buf, p, copy_len);
            msg_buf[copy_len] = '\0';

            if (copy_len > 0 && msg_buf[copy_len - 1] == '\n') {
                msg_buf[copy_len - 1] = '\0';
            }

            syslog_log(1, msg_buf);
        }

        return (int64_t)count;
    }

    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    return fat_write(&fd_table[fd], buf, count);
}

int64_t sys_seek(int fd, int64_t offset, int whence) {
    // #317: SMB-backed fd: seek within the cached read image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        int64_t np;
        if (whence == 0) np = offset;
        else if (whence == 1) np = (int64_t)s->rpos + offset;
        else if (whence == 2) np = (int64_t)s->rsize + offset;
        else return -1;
        if (np < 0) np = 0;
        if (np > (int64_t)s->rsize) np = s->rsize;
        s->rpos = (uint32_t)np;
        return np;
    }
    // ext2-backed fd: seek within the cached file image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        int64_t np;
        if (whence == 0) np = offset;
        else if (whence == 1) np = (int64_t)e->rpos + offset;
        else if (whence == 2) np = (int64_t)e->rsize + offset;
        else return -1;
        if (np < 0) np = 0;
        if (np > (int64_t)e->rsize) np = e->rsize;
        e->rpos = (uint32_t)np;
        return np;
    }
    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    // Convert whence for FAT driver
    uint32_t pos;
    switch (whence) {
        case 0:  // SEEK_SET
            pos = offset;
            break;
        case 1:  // SEEK_CUR
            pos = fd_table[fd].position + offset;
            break;
        case 2:  // SEEK_END
            pos = fd_table[fd].file_size + offset;
            break;
        default:
            return -1;
    }

    if (fat_seek(&fd_table[fd], pos) != 0) return -1;
    return (int64_t)fd_table[fd].position;  /* POSIX: lseek returns new offset */
}

// Kernel-side struct stat, byte-for-byte matching userland <sys/stat.h>.
typedef struct {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
} k_stat_t;
// #503 argtab sizeof-lock: SYS_STAT arg2 (SZ_K_STAT in rustkern.rs).
_Static_assert(sizeof(k_stat_t) == 88,
               "#503 argtab: SZ_K_STAT in rustkern.rs is stale");

// O(1) stat by path. Reads the file size and type directly from the FAT
// directory entry (via fat_open, which never walks the file's cluster chain),
// so stat'ing a directory of large files is cheap. Previously userland stat()
// sized files with SEEK_END, which made fat_seek walk every cluster: `ls -la /`
// over the multi-MB kernel.elf/wallpaper files effectively hung the machine.
int64_t sys_stat_path(const char *path, void *ubuf) {
    if (!path || !ubuf) return -1;

    // #317: SMB network share. Mount on demand and stat over SMB2.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        smb_dirent_t info;
        if (smb_stat(path, &info) != 0) return -1;
        k_stat_t *st = (k_stat_t *)ubuf;
        memset(st, 0, sizeof(*st));
        st->st_mode    = info.is_directory ? (0040000u | 0755u) : (0100000u | 0644u);
        st->st_nlink   = 1;
        st->st_size    = (long)info.size;
        st->st_blksize = 512;
        st->st_blocks  = ((long)info.size + 511) / 512;
        return 0;
    }

    // #317 pass 4: NFS export. Mount must exist; stat via NFSv3 GETATTR/LOOKUP.
    if (path_is_nfs(path)) {
        if (nfs_vfs_ensure_mount(path) != 0) return -1;
        nfs_fattr3_t attrs;
        if (nfs_getattr(path, &attrs) != 0) return -1;
        k_stat_t *st = (k_stat_t *)ubuf;
        memset(st, 0, sizeof(*st));
        int is_dir = (attrs.type == NF3DIR);
        st->st_mode    = is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
        st->st_nlink   = (attrs.nlink ? attrs.nlink : 1);
        st->st_size    = (long)attrs.size;
        st->st_blksize = 512;
        st->st_blocks  = ((long)attrs.size + 511) / 512;
        return 0;
    }

    // #359 Phase 2: ext2 root/volume paths. sys_stat previously had no ext2
    // branch, so ext2 paths fell through to FAT (which fails), and libc stat()
    // then fell back to open()+SEEK_END - which always reports S_IFREG. That made
    // every ext2 DIRECTORY look like a regular file, so CPython's PathFinder
    // (_path_isdir) never built a FileFinder and NO filesystem import worked.
    // Resolve the inode and report the real type (ext2 i_mode is POSIX-layout:
    // 0x4000=dir, 0x8000=reg, + perm bits) and size.
    {
        const char *rel = 0;
        if (path_is_ext2(path))        rel = ext2_relpath(path);
        else if (path_root_ext2(path)) rel = path;
        if (rel) {
            uint32_t ino = ext2_resolve_path(rel);
            if (ino) {
                ext2_inode_t in;
                if (ext2_read_inode(ino, &in) == 0) {
                    int is_dir = ((in.i_mode & 0xF000) == 0x4000);
                    k_stat_t *st = (k_stat_t *)ubuf;
                    memset(st, 0, sizeof(*st));
                    st->st_mode    = in.i_mode ? in.i_mode
                                     : (is_dir ? (0040000u|0755u) : (0100000u|0644u));
                    st->st_nlink   = 1;
                    st->st_size    = is_dir ? 0 : (long)in.i_size;
                    st->st_blksize = 1024;
                    st->st_blocks  = ((long)in.i_size + 511) / 512;
                    return 0;
                }
            }
            // Not present on ext2: fall through to FAT (file may be ESP-only).
        }
    }

    fat_file_t f;
    if (fat_open(&g_fat_fs, path, &f) != 0) return -1;

    int is_dir = f.is_dir || (f.attr & FAT_ATTR_DIRECTORY);
    uint32_t size = is_dir ? 0 : f.file_size;
    if (f.open) fat_close(&f);

    k_stat_t *st = (k_stat_t *)ubuf;   // identity-mapped: user ptr == phys
    memset(st, 0, sizeof(*st));
    st->st_mode    = is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
    st->st_nlink   = 1;
    st->st_size    = (long)size;
    st->st_blksize = 512;
    st->st_blocks  = ((long)size + 511) / 512;
    return 0;
}

// ===== #317 pass 2: SMB network mount control syscalls =====================
// sys_net_mount: establish (and cache) an authenticated connection to an SMB
// share so subsequent "/SMB/<server>/<share>/..." file access reuses it. The
// Files app calls this before navigating into a saved network location so that
// per-mount credentials (not just the guest default) are used.
int64_t sys_net_mount(const char *server, const char *share,
                      const char *user, const char *pass) {
    if (!server || !share) return -1;
    return smb_vfs_mount_creds(server, share, user, pass) == 0 ? 0 : -1;
}

// sys_net_list_shares: enumerate the shares a server exports (srvsvc/IPC$).
// Writes share names newline-separated into ubuf; returns the count (>=0) or -1.
int64_t sys_net_list_shares(const char *server, char *ubuf, uint32_t maxlen) {
    if (!server || !ubuf || maxlen == 0) return -1;
    extern uint32_t smb_resolve_ip(const char *host);
    uint32_t ip = smb_resolve_ip(server);
    if (!ip) return -1;
    int count = 0;
    char **shares = smb_list_shares(ip, &count);
    if (!shares) return -1;
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        const char *nm = shares[i] ? shares[i] : "";
        uint32_t nl = (uint32_t)strlen(nm);
        if (off + nl + 1 >= maxlen) break;
        memcpy(ubuf + off, nm, nl); off += nl;
        ubuf[off++] = '\n';
    }
    ubuf[off < maxlen ? off : maxlen - 1] = 0;
    smb_free_shares(shares, count);
    return count;
}

// sys_net_unmount: tear down an SMB share connection.
int64_t sys_net_unmount(const char *server, const char *share) {
    if (!server || !share) return -1;
    char mp[300];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);
    return smb_unmount(mp) == 0 ? 0 : -1;
}

// HTTP/HTTPS fetch (#http): wrap the kernel https_get() so userland (widgets)
// can pull JSON from web APIs. Blocking; https.c self-pumps net_poll().
// Decode an image (BMP/PNG/JPEG) from `data`[len] and point-sample it down to fit
// the target box packed in `target` (tw<<16 | th), writing BGRA pixels to out[out_cap].
// dims[0]/dims[1] receive the produced width/height. Returns bytes written, or -1.
// Point-sampling = the progressive/cheap path (#247): big images never allocate a
// huge userland buffer and downscale fast.
// #549: feed the net connectivity circuit-breaker from every fetch/POST outcome.
// A positive HTTP status means we reached a server (uplink works) - report OK even
// on 4xx/5xx. A transport failure (r<0 and no status: DNS/connect/recv timeout,
// nobody answered) reports a reach-failure toward the NET_FAULTY trip. Neutral
// otherwise. Cheap; safe from the fetch worker threads.
static void net_fetch_report(int r, int status) {
    extern void net_report_reach_ok(void);
    extern void net_report_reach_fail(void);
    if (status > 0)  net_report_reach_ok();
    else if (r < 0)  net_report_reach_fail();
}
// #549: once the interface is NET_FAULTY, no fetch touches the wire again (this is
// what makes the USB busy-poll storm stop mid-cycle and CPU fall to ~0). Recovery
// is explicit: Settings apply static / renew DHCP, a carrier replug, or a fresh
// DHCP bind all clear the fault (see net.c / sys_net_set_static / SYS_NET_DHCP).
static int net_fetch_blocked(int *ustatus) {
    extern int net_is_faulty(void);
    if (net_is_faulty()) { if (ustatus) *ustatus = 0; return 1; }
    return 0;
}

int64_t sys_http_fetch(const char *url, char *ubuf, uint32_t max_len, uint32_t *ubytes, int *ustatus) {
    if (!url || !ubuf || max_len == 0) return -1;
    if (net_fetch_blocked(ustatus)) return -1;
    extern int https_get(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern int wget_fetch(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    uint8_t *body = 0; uint32_t blen = 0; int status = 0;
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    int r = https ? https_get(url, &body, &blen, &status)
                  : wget_fetch(url, &body, &blen, &status);
    net_fetch_report(r, status);
    if (r < 0 || !body) { if (body) kfree(body); if (ustatus) *ustatus = status; return -1; }
    uint32_t n = (blen < max_len) ? blen : max_len;
    memcpy(ubuf, body, n);
    kfree(body);
    if (ubytes) *ubytes = n;
    if (ustatus) *ustatus = status;
    return (int64_t)n;
}

// #414 Home Assistant: blocking GET with an Authorization header. Runs in the
// background haservice process the same inline way sys_http_fetch does (which
// netinfo already uses safely from Ring 3); never on the compositor UI thread.
int64_t sys_http_fetch_hdr(const char *url, const char *headers, char *ubuf,
                           uint32_t max_len, uint32_t *ubytes, int *ustatus) {
    if (!url || !ubuf || max_len == 0) return -1;
    if (net_fetch_blocked(ustatus)) return -1;
    extern int wget_fetch_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    uint8_t *body = 0; uint32_t blen = 0; int status = 0;
    int r = wget_fetch_hdr(url, headers ? headers : "", &body, &blen, &status);
    net_fetch_report(r, status);
    if (ustatus) *ustatus = status;
    if (r < 0 || !body) { if (body) kfree(body); return -1; }
    uint32_t n = (blen < max_len) ? blen : max_len;
    memcpy(ubuf, body, n);
    kfree(body);
    if (ubytes) *ubytes = n;
    return (int64_t)n;
}

// ===== Async HTTP fetch: background worker threads so the browser UI never
// blocks during a download (#277). A small fixed job table; each START spawns a
// kernel thread running the (blocking) https_get/wget_fetch into a kernel
// buffer; the caller POLLs each frame and READs the body when done. =====
#define ASYNC_FETCH_MAX 6
typedef struct {
    volatile int in_use;
    volatile int state;     // 0=running, 1=done, 2=error
    int status;             // HTTP status
    uint8_t *body;
    uint32_t len;
    volatile int detached;  // caller canceled; worker frees on completion
    char url[1024];
} async_fetch_t;
static async_fetch_t g_async_fetch[ASYNC_FETCH_MAX];

extern void thread_exit(int) __attribute__((noreturn));
static void async_fetch_worker(void *arg) {
    async_fetch_t *j = (async_fetch_t *)arg;
    extern int https_get(const char *, uint8_t **, uint32_t *, int *);
    extern int wget_fetch(const char *, uint8_t **, uint32_t *, int *);
    uint8_t *body = 0; uint32_t len = 0; int status = 0;
    int https = (j->url[0]=='h' && j->url[1]=='t' && j->url[2]=='t' &&
                 j->url[3]=='p' && j->url[4]=='s');
    int r = https ? https_get(j->url, &body, &len, &status)
                  : wget_fetch(j->url, &body, &len, &status);
    net_fetch_report(r, status);   // #549 circuit-breaker
    j->status = status;
    if (r >= 0 && body) { j->body = body; j->len = len; j->state = 1; }
    else { if (body) kfree(body); j->len = 0; j->state = 2; }
    if (j->detached) { if (j->body) kfree(j->body); j->body = 0; j->in_use = 0; }
    { extern void proc_exit(int); proc_exit(0); }
}

int64_t sys_http_fetch_start(const char *url) {
    if (!url) return -1;
    if (net_fetch_blocked(0)) return -1;   // #549: no wire work while NET_FAULTY
    int slot = -1;
    for (int i = 0; i < ASYNC_FETCH_MAX; i++)
        if (!g_async_fetch[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;
    async_fetch_t *j = &g_async_fetch[slot];
    j->state = 0; j->status = 0; j->body = 0; j->len = 0; j->detached = 0;
    int k = 0; for (; url[k] && k < (int)sizeof(j->url) - 1; k++) j->url[k] = url[k];
    j->url[k] = 0;
    j->in_use = 1;
    extern int proc_create_ex(const char *, void (*)(void *), void *, process_priority_t, uint32_t);
    int tid = proc_create_ex("httpfetch", async_fetch_worker, j, PRIO_NORMAL, 128 * 1024);  // #264 big stack for TLS/HTTPS (#277 was thread_create_kernel)
    if (tid < 0) { j->in_use = 0; return -1; }
    return slot;
}

int64_t sys_http_fetch_poll(int id, int *ustatus, uint32_t *ulen) {
    if (id < 0 || id >= ASYNC_FETCH_MAX || !g_async_fetch[id].in_use) return -1;
    async_fetch_t *j = &g_async_fetch[id];
    if (ustatus) *ustatus = j->status;
    if (ulen) *ulen = j->len;
    return j->state;
}

int64_t sys_http_fetch_read(int id, char *ubuf, uint32_t max) {
    if (id < 0 || id >= ASYNC_FETCH_MAX || !g_async_fetch[id].in_use) return -1;
    async_fetch_t *j = &g_async_fetch[id];
    if (j->state == 0) return -2;   // still running
    uint32_t n = 0;
    if (j->state == 1 && j->body && ubuf) {
        n = (j->len < max) ? j->len : max;
        memcpy(ubuf, j->body, n);
    }
    if (j->body) kfree(j->body);
    j->body = 0; j->in_use = 0;
    return (int64_t)n;
}

int64_t sys_http_fetch_cancel(int id) {
    if (id < 0 || id >= ASYNC_FETCH_MAX || !g_async_fetch[id].in_use) return -1;
    async_fetch_t *j = &g_async_fetch[id];
    if (j->state == 0) { j->detached = 1; }       // worker frees on completion
    else { if (j->body) kfree(j->body); j->body = 0; j->in_use = 0; }
    return 0;
}

// HTTPS POST: url, extra headers (CRLF lines, e.g. Authorization + Content-Type),
// JSON body -> response body into ubuf (cap max_len). Returns bytes written, or
// -1. *ustatus gets the HTTP status. HTTP-only (http://) returns 501.
// ===== #264: Async HTTPS POST worker. The synchronous SYS_HTTP_POST path ran
// net code INLINE on the Ring-3 caller after juggling its CR3 to the kernel
// master; an IRQ/context-switch taken inside that borrowed-CR3 window left the
// caller running net code (and bkl_acquire) on a stale/wrong CR3, which hard-
// wedged the whole OS (CPU0 spinning in bkl_acquire, all APs HLT). The browser's
// async GET never had this because a genuine PRIV_KERNEL worker proc does ALL the
// net work on a real kernel CR3 while the user app only POLLS (non-blocking).
static char *kstrdup_opt(const char *src) {
    uint32_t n = src ? (uint32_t)strlen(src) : 0;
    char *d = kmalloc(n + 1);
    if (!d) return 0;
    if (n) memcpy(d, src, n);
    d[n] = 0;
    return d;
}
// We replicate that exactly for POST: START spawns a kernel worker that owns its
// kernel CR3 (proc->cr3==0 -> scheduler keeps it on the kernel master), runs the
// blocking https_post into kernel buffers, and the user app POLLs then READs.
// No CR3 juggling on the user side, ever.
#define ASYNC_POST_MAX 4
typedef struct {
    volatile int in_use;
    volatile int state;     // 0=running/queued, 1=done, 2=error
    int status;             // HTTP status
    uint8_t *body;          // response body (kfree'd by READ)
    uint32_t len;
    volatile int detached;  // caller canceled; worker frees on completion
    char *url;              // kernel copies of the request (kfree'd by worker)
    char *headers;
    char *reqbody;
} async_post_t;
static async_post_t g_async_post[ASYNC_POST_MAX];
static volatile int g_post_worker_started = 0;

static void async_post_free_req(async_post_t *j) {
    if (j->url)     { kfree(j->url);     j->url = 0; }
    if (j->headers) { kfree(j->headers); j->headers = 0; }
    if (j->reqbody) { kfree(j->reqbody); j->reqbody = 0; }
}

// #426: the worker's idle wake. File-static, so unlike a wait queue embedded in
// a kmalloc'd object it can never be freed under a waiter. Statically
// initialised (no init call, hence no init-ordering race).
//
// Exactly ONE producer feeds this worker: sys_http_post_start(), which wakes it
// after publishing j->url. That single source is sufficient (no redundancy
// needed) because it is unconditional and the check-then-park race is closed by
// wait_event()'s own re-check after __wait_prepare(). g_async_fetch is a
// SEPARATE table with a per-request worker and does not feed this queue.
static wait_queue_head_t g_post_job_wq = { .head = NULL, .lock = SPINLOCK_INIT };

// A job needs running iff in_use, still state 0, and fully populated (the START
// handler publishes url LAST, so a non-NULL url means the record is complete).
// Same predicate the worker's scan uses, so the park condition and the work
// condition can never disagree.
static int post_job_pending(void) {
    for (int i = 0; i < ASYNC_POST_MAX; i++) {
        async_post_t *j = &g_async_post[i];
        if (j->in_use && j->state == 0 && j->url) return 1;
    }
    return 0;
}

// ONE persistent worker proc drains the job table forever. No per-POST
// proc_create/proc_exit churn (that churn raced the user-process spawn path and
// faulted the kernel under load, #264/#317). The worker runs as a PRIV_KERNEL
// proc, so the scheduler keeps it on the kernel CR3; it does ALL net work while
// the user app only POLLs.
static void async_post_worker(void *arg) {
    (void)arg;
    extern int https_post(const char *, const char *, const char *,
                          uint8_t **, uint32_t *, int *);
    extern void proc_sleep(uint32_t ms);
    for (;;) {
        int did = 0;
        for (int i = 0; i < ASYNC_POST_MAX; i++) {
            async_post_t *j = &g_async_post[i];
            // A job needs running iff it is in_use, still state 0, and has a
            // request buffer (the START handler sets url last under in_use=1).
            if (!j->in_use || j->state != 0 || !j->url) continue;
            did = 1;
            uint8_t *body = 0; uint32_t len = 0; int status = 0;
            int r = https_post(j->url, j->headers ? j->headers : "",
                               j->reqbody ? j->reqbody : "",
                               &body, &len, &status);
            net_fetch_report(r, status);   // #549 circuit-breaker
            j->status = status;
            if (r >= 0 && body) { j->body = body; j->len = len; }
            else { if (body) kfree(body); j->len = 0; }
            async_post_free_req(j);
            // Publish terminal state LAST so a poller never reads done/error
            // before body/len/status are settled.
            j->state = (r >= 0 && body) ? 1 : 2;
            if (j->detached) { if (j->body) kfree(j->body); j->body = 0; j->in_use = 0; }
        }
        // #426: was `if (!did) proc_sleep(10);`, a 10ms idle poll that woke this
        // worker 100 times a second forever to almost always find nothing, and
        // that added up to 10ms of latency to every POST. Now it BLOCKS until a
        // job is actually submitted: no timeout, because the only thing that can
        // end this wait is an event we own and always signal, and a worker with
        // no work should wait indefinitely.
        if (!did) (void)wait_event(&g_post_job_wq, post_job_pending());
    }
}

static void ensure_post_worker(void) {
    if (g_post_worker_started) return;
    extern int proc_create_ex(const char *, void (*)(void *), void *, process_priority_t, uint32_t);
    int tid = proc_create_ex("httppost", async_post_worker, 0, PRIO_NORMAL, 128 * 1024); // #264 big stack for TLS/HTTPS
    if (tid >= 0) g_post_worker_started = 1;
}

// START: copy the request OUT of user memory (we are on the caller's CR3 now, so
// user pointers are valid) into kernel buffers, queue it, and make sure the
// persistent worker exists. Returns a job id, or -1. http:// is rejected.
int64_t sys_http_post_start(const char *url, const char *headers, const char *body) {
    if (!url) return -1;
    if (net_fetch_blocked(0)) return -1;   // #549: no wire work while NET_FAULTY
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    if (!https) return -1;
    int slot = -1;
    for (int i = 0; i < ASYNC_POST_MAX; i++)
        if (!g_async_post[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;
    async_post_t *j = &g_async_post[slot];
    j->state = 0; j->status = 0; j->body = 0; j->len = 0; j->detached = 0;
    j->url = 0; j->headers = 0; j->reqbody = 0;
    char *ku = kstrdup_opt(url);
    char *kh = kstrdup_opt(headers);
    char *kb = kstrdup_opt(body);
    if (!ku || !kh || !kb) {
        if (ku) kfree(ku);
        if (kh) kfree(kh);
        if (kb) kfree(kb);
        return -1;
    }
    j->headers = kh; j->reqbody = kb;
    j->in_use = 1;               // mark live BEFORE url so the worker only picks
    j->url = ku;                 // it up once fully populated (url published last)
    ensure_post_worker();
    // #426: wake the (now certainly existing) worker. Must come AFTER url is
    // published, or the worker could wake, run post_job_pending(), see an
    // incomplete record and park again with the job stranded. Harmless if the
    // worker was only just created and has not parked yet: no waiter means this
    // is a lock and a NULL check, and its first action is a full table scan.
    wake_up_all(&g_post_job_wq);
    return slot;
}

int64_t sys_http_post_poll(int id, int *ustatus, uint32_t *ulen) {
    if (id < 0 || id >= ASYNC_POST_MAX || !g_async_post[id].in_use) return -1;
    async_post_t *j = &g_async_post[id];
    if (ustatus) *ustatus = j->status;
    if (ulen) *ulen = j->len;
    return j->state;
}

int64_t sys_http_post_read(int id, char *ubuf, uint32_t max) {
    if (id < 0 || id >= ASYNC_POST_MAX || !g_async_post[id].in_use) return -1;
    async_post_t *j = &g_async_post[id];
    if (j->state == 0) return -2;   // still running
    uint32_t n = 0;
    if (j->state == 1 && j->body && ubuf) {
        n = (j->len < max) ? j->len : max;
        memcpy(ubuf, j->body, n);
    }
    if (j->body) kfree(j->body);
    j->body = 0; j->in_use = 0;
    return (int64_t)n;
}

int64_t sys_http_post_cancel(int id) {
    if (id < 0 || id >= ASYNC_POST_MAX || !g_async_post[id].in_use) return -1;
    async_post_t *j = &g_async_post[id];
    if (j->state == 0) { j->detached = 1; }    // worker frees on completion
    else { if (j->body) kfree(j->body); j->body = 0; j->in_use = 0; }
    return 0;
}

// HTTPS POST: url, extra headers (CRLF lines), JSON body -> response body into
// ubuf (cap max_len). Returns bytes written, or -1; *ustatus gets HTTP status.
int64_t sys_http_post(const char *url, const char *headers, const char *body,
                      char *ubuf, uint32_t max_len, int *ustatus) {
    if (!url || !ubuf || max_len == 0) return -1;
    if (net_fetch_blocked(ustatus)) return -1;   // #549
    extern int https_post(const char *url, const char *headers, const char *body,
                          uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern int wget_post_hdr(const char *, const char *, const char *,
                             uint8_t **, uint32_t *, int *);   // #414 plain-HTTP POST
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);

    // Kernel callers (RC, shell, in-kernel tools) already run on the kernel CR3:
    // run https_post inline (unchanged, proven path).
    if (!from_user) {
        uint8_t *rbody = 0; uint32_t blen = 0; int status = 0;
        int r = https ? https_post(url, headers, body, &rbody, &blen, &status)
                      : wget_post_hdr(url, headers ? headers : "", body ? body : "", &rbody, &blen, &status);
        net_fetch_report(r, status);   // #549
        if (r < 0) { if (rbody) kfree(rbody); if (ustatus) *ustatus = status; return -1; }
        uint32_t n = (blen < max_len) ? blen : max_len;
        if (rbody) memcpy(ubuf, rbody, n);
        if (rbody) kfree(rbody);
        if (ustatus) *ustatus = status;
        return (int64_t)n;
    }

    // User caller: https_post() must run on the KERNEL address space (net_poll /
    // eth_receive / e1000 DMA + rings + MMIO live in the identity-mapped LOWER
    // half, absent from a Ring-3 CR3). We run it INLINE on this process after
    // switching it to the kernel address space. This avoids spawning (and tearing
    // down) a worker process per POST: that per-POST proc_create/proc_exit churn
    // is itself a known intermittent kernel-corruption trigger (#264/#317) when a
    // user app drives several POSTs. Mechanics:
    //   - copy the request out of user memory into kernel buffers first
    //   - set cur->cr3 = 0 so the scheduler keeps us on the kernel CR3 across the
    //     proc_sleep() yields inside https_post (a bare `mov cr3` would be undone
    //     by the next context switch, which reloads cur->cr3)
    //   - load the kernel CR3 now, run https_post() inline (the proven path; the
    //     #297 net_lock serializes its NIC/TCP-table access against net_poll)
    //   - restore cur->cr3 + the user CR3 before copying the reply back.
    char *kurl = kstrdup_opt(url);
    char *khdr = kstrdup_opt(headers);
    char *kbody = kstrdup_opt(body);
    if (!kurl || !khdr || !kbody) {
        if (kurl) kfree(kurl);
        if (khdr) kfree(khdr);
        if (kbody) kfree(kbody);
        return -1;
    }

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;                                   // scheduler -> kernel CR3 on resume
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    uint8_t *rbody = 0; uint32_t blen = 0; int status = 0;
    int r = https ? https_post(kurl, khdr, kbody, &rbody, &blen, &status)
                  : wget_post_hdr(kurl, khdr, kbody, &rbody, &blen, &status);
    net_fetch_report(r, status);   // #549

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    kfree(kurl); kfree(khdr); kfree(kbody);

    if (r < 0) { if (rbody) kfree(rbody); if (ustatus) *ustatus = status; return -1; }
    uint32_t n = (blen < max_len) ? blen : max_len;
    if (rbody && n) memcpy(ubuf, rbody, n);    // back on user CR3: ubuf valid
    if (rbody) kfree(rbody);
    if (ustatus) *ustatus = status;
    return (int64_t)n;
}


// ============================================================================
// #318 network printing syscalls (IPP client). The actual IPP/HTTP exchange
// runs over the kernel TCP stack, so for a Ring-3 caller the network call must
// execute on the kernel address space (net_poll/e1000 DMA live in the
// identity-mapped lower half, absent from a user CR3). We mirror the proven
// sys_http_post() trick: copy args out of user memory, run the network call
// inline on the kernel CR3, then restore the user CR3.
// ============================================================================
int64_t sys_print_list(void *out, int max) {
    extern int print_list(void *out, int max);   // printer_cfg_t* erased to void*
    return print_list(out, max);
}

int64_t sys_print_add(const char *name, const char *host, int port,
                      const char *queue, int make_default) {
    extern int print_add(const char *name, const char *host, uint16_t port,
                         const char *queue, int make_default);
    if (!name || !host || !queue) return -1;
    return print_add(name, host, (uint16_t)port, queue, make_default);
}

int64_t sys_print_remove(const char *name) {
    extern int print_remove(const char *name);
    if (!name) return -1;
    return print_remove(name);
}

int64_t sys_print_job(const char *printer, const char *title, const char *text) {
    extern int print_job_text(const char *printer_name, const char *title, const char *text);
    if (!text) return -1;

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) {
        return print_job_text(printer, title, text);
    }

    char *kp = kstrdup_opt(printer);
    char *kt = kstrdup_opt(title);
    char *kx = kstrdup_opt(text);
    if (!kx) { if (kp) kfree(kp); if (kt) kfree(kt); if (kx) kfree(kx); return -1; }

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_text(kp, kt, kx);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    if (kt) kfree(kt);
    if (kx) kfree(kx);
    return r;
}

// Print an image FILE (path on the FAT disk) to a configured printer. Same
// CR3-switch discipline as sys_print_job: the IPP/HTTP exchange runs on the
// kernel address space.
int64_t sys_print_image(const char *printer, const char *path) {
    extern int print_job_image(const char *printer_name, const char *path);
    if (!path) return -1;

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) return print_job_image(printer, path);

    char *kp = kstrdup_opt(printer);
    char *kpath = kstrdup_opt(path);
    if (!kpath) { if (kp) kfree(kp); return -1; }

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_image(kp, kpath);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    kfree(kpath);
    return r;
}

// Print the current framebuffer (screen) to a configured printer.
int64_t sys_print_screen(const char *printer) {
    extern int print_job_screen(const char *printer_name);
    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) return print_job_screen(printer);

    char *kp = kstrdup_opt(printer);

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_screen(kp);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    return r;
}

// #307 real-hardware bring-up: let userland (the compositor's login screen,
// which is where the physical iMac14,4 "No user accounts found" bug is
// actually visible) append a line to the same persistent /BOOTLOG.TXT the
// kernel writes to. Bounded copy directly from the caller's own address
// space (same pattern as other simple string-arg syscalls; no CR3 switching
// needed since bootlog_write() never touches user memory itself, only the
// short kernel-side copy made here). Never fails loudly - logging must not
// be able to crash or block whatever is trying to log.
int64_t sys_bootlog_write(const char *msg) {
    if (!msg) return -1;
    char buf[200];
    int i = 0;
    while (msg[i] && i < (int)sizeof(buf) - 1) { buf[i] = msg[i]; i++; }
    buf[i] = '\0';
    bootlog_write("[USERSPACE] %s", buf);
    return 0;
}

// ============================================================================
// Memory syscalls
// ============================================================================

// Default heap start: just past the ELF load region.
// The actual per-process value is in process_t.brk.
// #487: the heap base now lives in process.h as PROC_DEFAULT_BRK_START so the
// per-process memory accounting (proc/procmem.c) can size the brk heap from the
// same constant. Aliased here to keep this file's existing call sites unchanged.
#define DEFAULT_BRK_START   PROC_DEFAULT_BRK_START

// mmap region lives between heap and stack (within PDPT[2], 2-3GB range).
// Start below the user stack to avoid colliding with kernel PDPT entries
// above 3GB that share read-only UEFI page directories.
#define DEFAULT_MMAP_START  0xA0000000ULL

int64_t sys_brk(uint64_t addr) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) {
        return -1;
    }

    if (p->brk == 0) {
        p->brk = DEFAULT_BRK_START;
    }

    if (addr == 0) {
        return (int64_t)p->brk;
    }

    // Expand/contract heap
    uint64_t old_brk = p->brk;
    uint64_t new_brk = (addr + 0xFFF) & ~0xFFF;  // Page align

    if (new_brk > old_brk) {
        // Expand: allocate new pages
        uint64_t pages = (new_brk - old_brk) / VMM_PAGE_SIZE_4K;
        if (vmm_alloc_user_pages(p->cr3, old_brk, pages, VMM_USER_RW) != 0) {
            return -1;
        }
    } else if (new_brk < old_brk) {
        // Contract: free pages
        uint64_t pages = (old_brk - new_brk) / VMM_PAGE_SIZE_4K;
        vmm_free_user_pages(p->cr3, new_brk, pages);
    }

    p->brk = new_brk;
    return (int64_t)new_brk;
}

// #429: demand-paged anonymous mmap. Instead of eagerly committing every page,
// register a lazy VMA in the process's mm and let the #PF handler fault pages
// in on first touch (real demand paging + W^X on writable data pages). Falls
// back to eager allocation if the VMA machinery is unavailable.
#ifndef VMA_READ
#define VMA_READ       (1 << 0)
#define VMA_WRITE      (1 << 1)
#define VMA_EXEC       (1 << 2)
#define VMA_PRIVATE    (1 << 4)
#define VMA_ANONYMOUS  (1 << 5)
#define VMA_LAZY       (1 << 10)
#endif
extern void *mm_create(void);
extern void *vma_create(uint64_t start, uint64_t end, uint32_t flags);
extern int   vma_add(void *mm, void *vma);
extern void *vma_find(void *mm, uint64_t addr);
extern int   do_munmap(void *mm, uint64_t addr, uint64_t length);

int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    (void)flags;

    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) {
        return -1;
    }
    if (len == 0) return -1;

    if (p->mmap_next == 0) {
        p->mmap_next = DEFAULT_MMAP_START;
    }
    uint64_t length = (len + 0xFFF) & ~0xFFFULL;
    if (addr == 0) {
        addr = p->mmap_next;
        p->mmap_next += length;
    }
    addr &= ~0xFFFULL;

    if (!p->mm) p->mm = mm_create();
    if (p->mm) {
        uint32_t vflags = VMA_READ | VMA_ANONYMOUS | VMA_PRIVATE | VMA_LAZY;
        if (prot & 0x2) vflags |= VMA_WRITE;   // PROT_WRITE
        if (prot & 0x4) vflags |= VMA_EXEC;    // PROT_EXEC
        if ((prot & 0x7) == 0) vflags |= VMA_WRITE;  // default anon = R/W
        void *vma = vma_create(addr, addr + length, vflags);
        if (vma) {
            if (vma_add(p->mm, vma) == 0) {
                return (int64_t)addr;   // pages fault in on demand
            }
            kfree(vma);
        }
    }

    // Fallback: eager commit (keeps mmap working even if VMA setup failed).
    uint64_t pages = length / VMM_PAGE_SIZE_4K;
    if (vmm_alloc_user_pages(p->cr3, addr, pages, VMM_USER_RW) != 0) {
        return -1;
    }
    return (int64_t)addr;
}

int64_t sys_munmap(uint64_t addr, uint64_t len) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) {
        return -1;
    }

    // #429: if this range is a demand-mapped VMA, drop the VMA + its pages so a
    // later access does not silently re-fault a fresh zero page.
    if (p->mm && vma_find(p->mm, addr)) {
        return do_munmap(p->mm, addr, len);
    }

    uint64_t pages = (len + VMM_PAGE_SIZE_4K - 1) / VMM_PAGE_SIZE_4K;
    vmm_free_user_pages(p->cr3, addr, pages);
    return 0;
}

// ============================================================================
// Console syscalls
// ============================================================================

int64_t sys_putchar(int c) {
    // Write to process's stdout fd if available (PTY slave)
    process_t *proc = proc_current();
    if (proc && proc->fds[1]) {
        extern int64_t file_write(struct file *f, const void *buf, size_t count);
        uint8_t ch = (uint8_t)c;
        file_write(proc->fds[1], &ch, 1);
        return c;
    }
    kputc((char)c);
    return c;
}

int64_t sys_getchar(void) {
    // Read one byte from the current process's stdin (fd 0)
    process_t *proc = proc_current();
    if (!proc || !proc->fds[0]) return -1;

    extern int64_t file_read(struct file *f, void *buf, size_t count);
    uint8_t c = 0;
    int64_t n = file_read(proc->fds[0], &c, 1);
    if (n <= 0) return -1;
    return (int64_t)c;
}

// ============================================================================
// Time syscalls
// ============================================================================

int64_t sys_time(void) {
    // Return seconds since boot (assuming 100Hz timer)
    return timer_ticks / 100;
}

int64_t sys_clock(void) {
    return timer_ticks;
}

// ============================================================================
// Window/Graphics syscalls
// ============================================================================

// Track windows per process (simple single-window model for now)
#define MAX_USER_WINDOWS 16
#define USER_EVENT_QUEUE_SIZE 128

// Per-window state for user processes
typedef struct {
    window_t *window;
    gui_event_t events[USER_EVENT_QUEUE_SIZE];
    uint32_t event_head;
    uint32_t event_tail;
    uint32_t event_count;
    uint32_t *content_buffer;  // Per-window pixel buffer (persistent across frames)
    int content_width;
    int content_height;
    int alloc_width;           // Allocated buffer width
    int alloc_height;          // Allocated buffer height
    uint32_t owner_pid;        // PID of the process that created this window
    // #453: win_get_event() used to busy-wait with proc_yield(), pegging a core
    // for every idle/docked window. Sleepers now block on this wait queue and
    // are woken when an event is queued. redraw_pending gates the per-composite
    // REDRAW so an idle window isn't fed a continuous REDRAW stream.
    wait_queue_head_t event_wq;
    int redraw_pending;
} user_window_t;

static user_window_t user_windows[MAX_USER_WINDOWS];

// Forward declaration for event handler
static void user_window_event_handler(void *app_data, gui_event_t *event);
static void user_window_draw_handler(void *app_data);

// Queue an event for a user-space window
static void user_window_queue_event(int handle, gui_event_t *event) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return;

    user_window_t *uw = &user_windows[handle];
    if (uw->event_count >= USER_EVENT_QUEUE_SIZE) {
        // Queue full, drop oldest event
        uw->event_head = (uw->event_head + 1) % USER_EVENT_QUEUE_SIZE;
        uw->event_count--;
    }

    uw->events[uw->event_tail] = *event;
    uw->event_tail = (uw->event_tail + 1) % USER_EVENT_QUEUE_SIZE;
    uw->event_count++;
    wake_up(&uw->event_wq);   // #453: wake a win_get_event() sleeper (IRQ-safe)
}

// Event handler for user-space windows - routes events to per-window queue
static void user_window_event_handler(void *app_data, gui_event_t *event) {
    int handle = (int)(uintptr_t)app_data;
    if (handle >= 0 && handle < MAX_USER_WINDOWS) {
        // Mouse events arrive from the compositor in ABSOLUTE screen
        // coordinates (sys_inject_mouse -> wm_inject_app_mouse passes them
        // through untranslated). Userland apps hit-test in their own content
        // space (origin 0,0 at the top-left of their drawable area), so we must
        // translate screen -> content-relative here using the same content
        // origin the draw handler blits to. Keyboard events carry no coords.
        if (event->type == EVENT_MOUSE_DOWN || event->type == EVENT_MOUSE_UP ||
            event->type == EVENT_MOUSE_MOVE || event->type == EVENT_MOUSE_SCROLL) {
            user_window_t *uw = &user_windows[handle];
            if (uw->window) {
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(uw->window, &wx, &wy, &ww, &wh);
                event->mouse_x -= wx;
                event->mouse_y -= wy;
            }
        }
        user_window_queue_event(handle, event);
    }
}

// Draw handler for user-space windows - blits content buffer (frame already drawn by wm_draw_all)
static void user_window_draw_handler(void *app_data) {
    if (g_win_blit_suppressed) return;   // screensaver owns the FB; don't punch through
    int handle = (int)(uintptr_t)app_data;
    if (handle >= 0 && handle < MAX_USER_WINDOWS && user_windows[handle].window) {
        user_window_t *uw = &user_windows[handle];
        window_t *win = uw->window;

        // NOTE: Do NOT call window_draw(win) here! It clears the content area.
        // Window frame is already drawn by wm_draw_all() before this is called.

        // Blit content buffer to window's content area
        if (uw->content_buffer) {
            int32_t wx, wy, ww, wh;
            window_get_content_bounds(win, &wx, &wy, &ww, &wh);

            extern void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *data);
            uint8_t op = win->opacity ? win->opacity : 255;
            // CLIP the content to the window's content rectangle so an app can
            // never draw outside its window box (right/bottom overflow).
            int bw = uw->content_width  < ww ? uw->content_width  : ww;
            int bh = uw->content_height < wh ? uw->content_height : wh;
            if (op >= 255) {
                if (uw->content_width == bw) {
                    fb_blit(wx, wy, bw, bh, uw->content_buffer);   // stride matches
                } else {
                    // buffer wider than the content area: blit row by row, clipped
                    for (int ry = 0; ry < bh; ry++)
                        fb_blit(wx, wy + ry, bw, 1, &uw->content_buffer[ry * uw->content_width]);
                }
            } else {
                // Per-window transparency: alpha-blend content over what's behind it.
                extern uint32_t fb_get_pixel(uint32_t x, uint32_t y);
                extern void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
                uint32_t inv = 255u - op;
                for (int ry = 0; ry < bh; ry++) {
                    for (int rx = 0; rx < bw; rx++) {
                        uint32_t sp = uw->content_buffer[ry * uw->content_width + rx];
                        uint32_t dp = fb_get_pixel((uint32_t)(wx + rx), (uint32_t)(wy + ry));
                        uint32_t r = (((sp >> 16) & 0xFF) * op + ((dp >> 16) & 0xFF) * inv) / 255;
                        uint32_t g = (((sp >> 8)  & 0xFF) * op + ((dp >> 8)  & 0xFF) * inv) / 255;
                        uint32_t b = (( sp        & 0xFF) * op + ( dp        & 0xFF) * inv) / 255;
                        fb_put_pixel((uint32_t)(wx + rx), (uint32_t)(wy + ry), (r << 16) | (g << 8) | b);
                    }
                }
            }

            // Log blit operation. Gated behind g_wm_blit_debug (default OFF):
            // an always-on per-frame [WM] blitted log floods the serial console
            // at ~30Hz (e.g. the aichat dock sliver) and starves the RC console.
            // Toggle on only when diagnosing blits.
            extern volatile int g_wm_blit_debug;
            static int blit_count = 0;
            if (g_wm_blit_debug && (++blit_count % 10 == 0)) {  // Log every 10th blit to avoid spam
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "[WM] user_window_draw_handler: handle=%d, blitted %dx%d buffer to (%d,%d) (#%d)",
                        handle, uw->content_width, uw->content_height, wx, wy, blit_count);
                syslog_log(1, log_msg);  // LOG_INFO
            }
        }

        // Send a redraw event to user-space ONLY when the app actually needs to
        // repaint: initial paint, resize, or an explicit win_invalidate() (all
        // of which set redraw_pending). The routine per-composite pass must NOT
        // re-queue a REDRAW, or the app's own blit -> recomposite -> REDRAW forms
        // a feedback loop that never lets win_get_event() sleep (#453). Still
        // coalesce so at most one REDRAW is queued at a time.
        if (uw->redraw_pending) {
            uw->redraw_pending = 0;
            int has_redraw = 0;
            uint32_t idx = uw->event_head;
            for (uint32_t i = 0; i < uw->event_count; i++) {
                if (uw->events[idx].type == EVENT_REDRAW) {
                    has_redraw = 1;
                    break;
                }
                idx = (idx + 1) % USER_EVENT_QUEUE_SIZE;
            }
            if (!has_redraw) {
                gui_event_t redraw_event;
                redraw_event.type = EVENT_REDRAW;
                redraw_event.target_id = handle;
                redraw_event.mouse_x = 0;
                redraw_event.mouse_y = 0;
                redraw_event.mouse_buttons = 0;
                redraw_event.scroll_delta = 0;
                redraw_event.keycode = 0;
                redraw_event.key_char = 0;
                user_window_queue_event(handle, &redraw_event);
            }
        }
    }
}

// Clean up all user windows owned by a process that is exiting.
// Called from proc_exit() in process.c.
void cleanup_user_windows_for_process(uint32_t pid) {
    // Destroy all windows owned by the exiting process.
    // This runs at proc_exit() time (interrupts may be disabled, so keep it simple).
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (user_windows[i].window && user_windows[i].owner_pid == pid) {
            // Free content buffer
            if (user_windows[i].content_buffer) {
                kfree(user_windows[i].content_buffer);
                user_windows[i].content_buffer = NULL;
            }
            // Unregister from window manager and destroy the window object
            window_t *win = user_windows[i].window;
            user_windows[i].window = NULL;
            user_windows[i].owner_pid = 0;
            user_windows[i].event_count = 0;
            // Find and unregister the app registration
            app_registration_t *reg = wm_get_app_by_window(win);
            if (reg) {
                // Compute array index via pointer difference from base
                extern app_registration_t *wm_get_app_by_id(int app_id);
                app_registration_t *base = wm_get_app_by_id(0);
                if (base) {
                    int idx = (int)(reg - base);
                    wm_unregister_app(idx);
                }
            }
            window_destroy(win);
        }
    }
    wm_invalidate_all();
}

// Called from window_resize() in window.c to reallocate the content buffer
// and notify the user-mode app via EVENT_RESIZE.
void user_window_handle_resize(window_t *win) {
    if (!win) return;

    // Find the user_window_t that owns this window
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (user_windows[i].window == win) {
            user_window_t *uw = &user_windows[i];
            int32_t cx, cy, cw, ch;
            window_get_content_bounds(win, &cx, &cy, &cw, &ch);
            if (cw <= 0 || ch <= 0) return;

            // Skip if dimensions haven't actually changed
            if (cw == uw->content_width && ch == uw->content_height) return;

            // Reallocate content buffer at the new size
            extern void *kmalloc(size_t size);
            extern void kfree(void *ptr);
            uint32_t *new_buf = kmalloc(cw * ch * sizeof(uint32_t));
            if (!new_buf) return;

            // Fill with window background color
            for (int p = 0; p < cw * ch; p++) {
                new_buf[p] = 0xFFF5F5F5;
            }

            // Copy old content (top-left aligned, clipped to new bounds)
            if (uw->content_buffer) {
                int copy_w = (cw < uw->content_width) ? cw : uw->content_width;
                int copy_h = (ch < uw->content_height) ? ch : uw->content_height;
                for (int y = 0; y < copy_h; y++) {
                    memcpy(&new_buf[y * cw],
                           &uw->content_buffer[y * uw->content_width],
                           copy_w * sizeof(uint32_t));
                }
                kfree(uw->content_buffer);
            }

            uw->content_buffer = new_buf;
            uw->content_width = cw;
            uw->content_height = ch;
            uw->alloc_width = cw;
            uw->alloc_height = ch;

            // (#200 resize-hang) If this is the Win16 host window, the Win16
            // interpreter (exec/win16api.c) still holds the OLD (now freed)
            // content buffer in g_win16_canvas with the OLD stride. Hand it the
            // new buffer + size so it does not write into freed memory (which
            // corrupted the heap and hung the OS on every Win16 window resize).
            extern void win16_host_rebind_canvas(int slot, uint32_t *new_buf,
                                                 int new_w, int new_h);
            win16_host_rebind_canvas(i, new_buf, cw, ch);

            // Queue EVENT_RESIZE so the app can redraw at the new size
            uw->redraw_pending = 1;   // #453: force a REDRAW after the resize
            gui_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_RESIZE;
            ev.mouse_x = cw;   // new width in mouse_x field
            ev.mouse_y = ch;   // new height in mouse_y field
            user_window_queue_event(i, &ev);

            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg),
                     "[WM] Resize window %d: new content %dx%d", i, cw, ch);
            syslog_log(1, log_msg);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Win16 host window (#144/#145). The Win16 interpreter runs in a kernel thread
// (see SYS_WIN16_RUN) and paints into a normal user-window content buffer so the
// compositor composites it like any app (live desktop + taskbar button) instead
// of taking over the framebuffer. Called from exec/win16api.c.
// ---------------------------------------------------------------------------
// (#215) Titlebar CLOSE (X) handler for the Win16 host window. The kernel WM
// invokes win->on_close when the X is clicked. We just latch the close request
// into the Win16 subsystem; the Win16 message pump (running in the win16 proc)
// converts it to a WM_QUIT and tears the app down cleanly (frees canvas, host
// window, DLL module images via registry_reset, and clears g_win16_busy so a
// DIFFERENT Win16 app can then launch). We must NOT free the window or run the
// interpreter from here: this runs in the desktop/WM context, and the host
// window is destroyed by win16_host_destroy() during that teardown.
static void win16_host_close_handler(window_t *win, gui_event_t *event) {
    (void)win; (void)event;
    extern void win16_request_close(void);
    win16_request_close();
}

int win16_host_create(const char *title, int x, int y, int w, int h,
                      uint32_t **out_buf, int *out_w, int *out_h,
                      window_t **out_win) {
    int slot = -1;
    for (int i = 0; i < MAX_USER_WINDOWS; i++)
        if (!user_windows[i].window) { slot = i; break; }
    if (slot < 0) return -1;

    window_t *win = window_create(title, x, y, w, h);
    if (!win) return -1;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ww <= 0) ww = 1;
    if (wh <= 0) wh = 1;

    extern void *kmalloc(size_t size);
    uint32_t *cb = kmalloc((size_t)ww * (size_t)wh * sizeof(uint32_t));
    if (!cb) { window_destroy(win); return -1; }
    for (int i = 0; i < ww * wh; i++) cb[i] = 0xFFC0C0C0;   // Win16 light gray

    user_windows[slot].window         = win;
    user_windows[slot].event_head     = 0;
    user_windows[slot].event_tail     = 0;
    user_windows[slot].event_count    = 0;
    user_windows[slot].content_buffer = cb;
    user_windows[slot].content_width  = ww;
    user_windows[slot].content_height = wh;
    user_windows[slot].alloc_width    = ww;
    user_windows[slot].alloc_height   = wh;
    user_windows[slot].owner_pid      = 0;   // kernel-owned (Win16 subsystem)
    wait_queue_head_init(&user_windows[slot].event_wq);   // #453
    user_windows[slot].redraw_pending = 1;                // #453: paint once on create

    int app_id = wm_register_app(win, (void *)(uintptr_t)slot,
                                 user_window_event_handler,
                                 user_window_draw_handler, NULL);
    if (app_id < 0) {
        extern void kfree(void *ptr);
        kfree(cb);
        window_destroy(win);
        user_windows[slot].window = NULL;
        return -1;
    }
    // (#215) Route the titlebar CLOSE (X) button to the Win16 teardown path.
    // Without this, on_close is NULL and the kernel WM just HIDES the window
    // (default behaviour), leaving the interpreter running and the single Win16
    // slot busy forever -> a second launch returns "busy".
    window_set_close_handler(win, win16_host_close_handler);

    window_show(win);
    wm_focus_window(win);
    wm_invalidate_all();

    if (out_buf) *out_buf = cb;
    if (out_w)   *out_w   = ww;
    if (out_h)   *out_h   = wh;
    if (out_win) *out_win = win;
    return slot;
}

// Report the on-screen content rect (client area) of a Win16 host window so the
// Win16 subsystem can translate the global kernel cursor (mouse_x/mouse_y, which
// are screen coords) into the window's canvas/client coords for mouse forwarding.
// Returns 0 on success.
int win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS) return -1;
    window_t *win = user_windows[slot].window;
    if (!win) return -1;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ox) *ox = wx;
    if (oy) *oy = wy;
    if (ow) *ow = ww;
    if (oh) *oh = wh;
    return 0;
}

void win16_host_destroy(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS) return;
    if (!user_windows[slot].window) return;
    user_window_t *uw = &user_windows[slot];

    app_registration_t *reg = wm_get_app_by_window(uw->window);
    if (reg) {
        extern app_registration_t *wm_get_app_by_id(int app_id);
        wm_unregister_app((int)(reg - wm_get_app_by_id(0)));
    }
    extern void kfree(void *ptr);
    if (uw->content_buffer) { kfree(uw->content_buffer); uw->content_buffer = NULL; }
    window_destroy(uw->window);
    uw->window = NULL;
    uw->event_count = 0;
    wm_invalidate_all();
}

// #185: mark an existing user window as borderless (no chrome). Sets the flag
// then reallocates the content buffer to the new (now full-window) content size
// so the app owns the entire window rectangle.
int64_t sys_win_set_nochrome(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    user_window_t *uw = &user_windows[handle];
    window_t *win = uw->window;

    win->flags |= WINDOW_FLAG_NOCHROME;
    // borderless panels are fixed (no titlebar to drag, no grips to resize)
    win->flags &= ~(WINDOW_FLAG_RESIZABLE | WINDOW_FLAG_MOVABLE);

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ww <= 0 || wh <= 0) return -1;

    extern void *kmalloc(size_t size);
    extern void kfree(void *p);
    uint32_t *nb = kmalloc((size_t)ww * wh * sizeof(uint32_t));
    if (!nb) return -1;
    for (int i = 0; i < ww * wh; i++) nb[i] = 0xFFF5F5F5;
    if (uw->content_buffer) kfree(uw->content_buffer);
    uw->content_buffer = nb;
    uw->content_width  = ww;
    uw->content_height = wh;
    uw->alloc_width    = ww;
    uw->alloc_height   = wh;

    // tell the app its new drawable size so it relays out
    gui_event_t ev; memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_RESIZE; ev.mouse_x = ww; ev.mouse_y = wh;
    user_window_queue_event(handle, &ev);

    // borderless panels still need keyboard focus to accept typing
    { extern void wm_focus_window(window_t *w); wm_focus_window(win); }

    wm_invalidate_all();
    return 0;
}

// Focus a user window addressed by the SLOT handle win_create() returns, but
// only when the CALLING process owns it. Returns 0 on success, -1 otherwise so
// the caller can fall back to window->id matching. This lets a fullscreen game
// (Arena/Squadron) re-assert keyboard focus on its own window every frame
// without the slot handle being misread as a window id (the two number spaces
// overlap for low values, which sent WASD to the wrong window). (#arena-wasd)
int64_t wm_focus_user_slot(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS || !user_windows[slot].window) return -1;
    process_t *cur = proc_current();
    if (!cur || user_windows[slot].owner_pid != (uint32_t)cur->pid) return -1;
    extern void wm_focus_window(window_t *win);
    wm_focus_window(user_windows[slot].window);
    return 0;
}

int64_t sys_win_create(const char *title, int x, int y, int width, int height) {
    // Find free window slot
    int slot = -1;
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (!user_windows[i].window) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    // Create the window
    window_t *win = window_create(title, x, y, width, height);
    if (!win) return -1;

    // Get content area dimensions
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);

    // Allocate content buffer for this window
    extern void *kmalloc(size_t size);
    uint32_t *content_buffer = kmalloc(ww * wh * sizeof(uint32_t));
    if (!content_buffer) {
        window_destroy(win);
        return -1;
    }

    // Clear buffer to light gray background (0xFFF5F5F5)
    for (int i = 0; i < ww * wh; i++) {
        content_buffer[i] = 0xFFF5F5F5;
    }

    // Initialize user window state
    user_windows[slot].window = win;
    user_windows[slot].event_head = 0;
    user_windows[slot].event_tail = 0;
    user_windows[slot].event_count = 0;
    user_windows[slot].content_buffer = content_buffer;
    user_windows[slot].content_width = ww;
    user_windows[slot].content_height = wh;
    user_windows[slot].alloc_width = ww;
    user_windows[slot].alloc_height = wh;
    user_windows[slot].owner_pid = proc_current() ? proc_current()->pid : 0;
    wait_queue_head_init(&user_windows[slot].event_wq);   // #453
    user_windows[slot].redraw_pending = 1;                // #453: paint once on create

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "[SYSCALL] Allocated content buffer for window %d: %dx%d (%zu bytes)",
            slot, ww, wh, (size_t)(ww * wh * sizeof(uint32_t)));
    syslog_log(1, log_msg);  // LOG_INFO

    // Register with window manager for event routing
    int app_id = wm_register_app(win, (void*)(uintptr_t)slot,
                                  user_window_event_handler,
                                  user_window_draw_handler,
                                  NULL);
    if (app_id < 0) {
        window_destroy(win);
        user_windows[slot].window = NULL;
        return -1;
    }

    window_show(win);
    wm_focus_window(win);  // New window gets keyboard focus immediately
    wm_invalidate_all();

    char log_msg2[128];
    snprintf(log_msg2, sizeof(log_msg2), "[SYSCALL] Created user window %d: \"%s\" at (%d,%d) %dx%d",
            slot, title, x, y, width, height);
    syslog_log(1, log_msg2);  // LOG_INFO
    return slot;
}

int64_t sys_win_destroy(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];

    // Unregister from window manager
    app_registration_t *reg = wm_get_app_by_window(uw->window);
    if (reg) {
        wm_unregister_app((int)(reg - wm_get_app_by_id(0)));  // Get index
    }

    // Free content buffer
    if (uw->content_buffer) {
        extern void kfree(void *ptr);
        kfree(uw->content_buffer);
        uw->content_buffer = NULL;

        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "[SYSCALL] Freed content buffer for window %d", handle);
        syslog_log(1, log_msg);  // LOG_INFO
    }

    window_destroy(uw->window);
    uw->window = NULL;
    uw->event_count = 0;
    wm_invalidate_all();
    return 0;
}

int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer) {
        return -1;
    }

    // Clip to buffer bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > uw->content_width) w = uw->content_width - x;
    if (y + h > uw->content_height) h = uw->content_height - y;
    if (w <= 0 || h <= 0) return 0;

    // Draw to content buffer
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < uw->content_width && py >= 0 && py < uw->content_height) {
                uw->content_buffer[py * uw->content_width + px] = color;
            }
        }
    }

    return 0;
}

int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer) {
        return -1;
    }

    // Clip to buffer bounds
    if (x < 0 || y < 0 || x >= uw->content_width || y >= uw->content_height) {
        return 0;
    }

    // Draw to content buffer
    uw->content_buffer[y * uw->content_width + x] = color;
    return 0;
}

int64_t sys_win_draw_text(int handle, int x, int y, const char *text, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !text) {
        return -1;
    }

    // Render each character into the content_buffer using the full 8x16 bitmap font.
    // font_get_glyph returns a 16-byte array where each byte is one row, MSB = leftmost pixel.
    int cx = x;
    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = font_get_glyph(*p);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    int px = cx + col;
                    int py = y + row;
                    if (px >= 0 && px < uw->content_width &&
                        py >= 0 && py < uw->content_height) {
                        uw->content_buffer[py * uw->content_width + px] = color;
                    }
                }
            }
        }
        cx += 8;  // Advance by one character width (8 pixels)
    }

    return 0;
}

// Antialiased TrueType text into a window's content buffer (window-relative,
// clipped to the content rect). y is the top of the text line. Used by apps
// that opt into TTF (e.g. Settings) via win_draw_text_ttf().
int64_t sys_win_draw_text_ttf(int handle, int x, int y, const char *text, uint32_t color, int size) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !text) return -1;
    if (size < 6) size = 6;
    if (size > 64) size = 64;

    extern ttf_glyph_t *ttf_get_glyph(int codepoint, int size, int style);
    extern void ttf_get_metrics(int size, int *ascent, int *descent, int *line_gap);
    extern int ttf_get_advance(int codepoint, int size);

    int ascent = size, descent = 0, line_gap = 0;
    ttf_get_metrics(size, &ascent, &descent, &line_gap);
    int baseline = y + ascent;
    int cw = uw->content_width, ch = uw->content_height;
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    int cx = x;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)*p;
        ttf_glyph_t *g = ttf_get_glyph(c, size, 0);
        if (!g) { cx += size / 2; continue; }
        if (g->bitmap) {
            int gx = cx + g->xoff, gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int py = gy + row; if (py < 0 || py >= ch) continue;
                for (int col = 0; col < g->width; col++) {
                    int px = gx + col; if (px < 0 || px >= cw) continue;
                    uint8_t a = g->bitmap[row * g->width + col]; if (!a) continue;
                    uint32_t *d = &uw->content_buffer[py * cw + px];
                    if (a >= 250) { *d = color; }
                    else {
                        uint32_t bg = *d; uint8_t br = (bg>>16)&0xFF, bgc = (bg>>8)&0xFF, bb = bg&0xFF;
                        uint8_t inv = 255 - a;
                        *d = (((cr*a+br*inv)/255)<<16) | (((cg*a+bgc*inv)/255)<<8) | ((cb*a+bb*inv)/255);
                    }
                }
            }
        }
        cx += g->advance ? g->advance : ttf_get_advance(c, size);
    }
    return 0;
}

// Face-aware antialiased TTF into a window (Font Browser previews, Studio text
// preview). Same as sys_win_draw_text_ttf but with an explicit face + style.
int64_t sys_win_draw_text_ttf_ex(int handle, int x, int y, const char *text, uint32_t color, int size, int face, int style) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !text) return -1;
    if (size < 6) size = 6;
    if (size > 96) size = 96;

    extern ttf_glyph_t *ttf_get_glyph_f(int, int, int, int);
    extern void ttf_get_metrics_f(int, int, int *, int *, int *);
    extern int ttf_get_advance_f(int, int, int);
    extern int ttf_get_kerning_f(int, int, int, int);

    int ascent = size, descent = 0, line_gap = 0;
    ttf_get_metrics_f(face, size, &ascent, &descent, &line_gap);
    int baseline = y + ascent;
    int cw = uw->content_width, ch = uw->content_height;
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    int cx = x;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '\n') { cx = x; baseline += ascent - descent + line_gap; continue; }
        ttf_glyph_t *g = ttf_get_glyph_f(face, c, size, style);
        if (!g) { cx += size / 2; continue; }
        if (g->bitmap) {
            int gx = cx + g->xoff, gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int py = gy + row; if (py < 0 || py >= ch) continue;
                for (int col = 0; col < g->width; col++) {
                    int px = gx + col; if (px < 0 || px >= cw) continue;
                    uint8_t a = g->bitmap[row * g->width + col]; if (!a) continue;
                    uint32_t *d = &uw->content_buffer[py * cw + px];
                    if (a >= 250) { *d = color; }
                    else {
                        uint32_t bg = *d; uint8_t br = (bg>>16)&0xFF, bgc = (bg>>8)&0xFF, bb = bg&0xFF;
                        uint8_t inv = 255 - a;
                        *d = (((cr*a+br*inv)/255)<<16) | (((cg*a+bgc*inv)/255)<<8) | ((cb*a+bb*inv)/255);
                    }
                }
            }
        }
        cx += g->advance ? g->advance : ttf_get_advance_f(face, c, size);
        if (p[1]) cx += ttf_get_kerning_f(face, c, (unsigned char)p[1], size);
    }
    return 0;
}

int64_t sys_win_draw_text_small(int handle, int x, int y, const char *text, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !text) return -1;
    // ~6x12 "medium" font: nearest-neighbor downscale of the 8x16 glyph, sized
    // between the 4x8 tooltip font and the 8x16 body font (for hints/captions).
    int cx = x;
    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = font_get_glyph(*p);
        for (int row = 0; row < 12; row++) {
            uint8_t bits = glyph[row * 16 / 12];
            for (int col = 0; col < 6; col++) {
                if (bits & (0x80 >> (col * 8 / 6))) {
                    int px = cx + col, py = y + row;
                    if (px >= 0 && px < uw->content_width && py >= 0 && py < uw->content_height)
                        uw->content_buffer[py * uw->content_width + px] = color;
                }
            }
        }
        cx += 7;  // 6px glyph + 1px spacing
    }
    return 0;
}

int64_t sys_win_get_event(int handle, void *event_buf, int timeout) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];

    // #453: this used to busy-wait with proc_yield() until an event arrived or
    // the timeout expired. That spun a whole CPU core for every idle/docked
    // window (aichat sat at ~99%), starved PRIO_LOW threads, and violated the
    // "no hand-rolled poll loops" rule (#426). Now we park on the window's wait
    // queue and genuinely SLEEP: a queued event fires wake_up(&event_wq) to wake
    // us early, and for a finite timeout we also arm a wake_time deadline so the
    // timer tick (wake_sleeping_procs) can wake us. No busy-wait, no core pegged.
    //
    // The lost-wakeup close (park under cli so no wake_up()/timer IRQ can slip
    // between the recheck and the state transition) now lives inside
    // __wait_event_wait_deadline() rather than being open-coded here. It also
    // takes the wait-queue lock, so unlike this copy it does not depend on the
    // BSP-only assumption that cli alone serializes every possible waker.
    if (uw->event_count == 0) {
        if (timeout == 0) {
            return 0;  // No event, non-blocking
        }

        // #426: this loop used to be an open-coded copy of "sleep until cond or
        // deadline". That mechanism now lives in exactly one place,
        // sync/waitq.h, so this is a call into the shared primitive instead of a
        // second implementation of it. Semantics are unchanged: the deadline is
        // still computed once with the same ms->ticks rounding, the condition is
        // still re-checked at the loop top, signals are still ignored (we
        // re-sleep rather than returning early), and a timeout still surfaces to
        // userland as the pre-existing "no event" return of 0.
        uint64_t deadline = (timeout < 0)
            ? WAIT_DEADLINE_NEVER
            : wq_deadline_in(wq_ms_to_ticks((uint64_t)timeout));

        (void)wait_event_deadline(&uw->event_wq, uw->event_count > 0, deadline);

        if (uw->event_count == 0) {
            return 0;  // Timed out with no event
        }
    }

    // Pop event from queue
    gui_event_t *out = (gui_event_t *)event_buf;
    *out = uw->events[uw->event_head];
    uw->event_head = (uw->event_head + 1) % USER_EVENT_QUEUE_SIZE;
    uw->event_count--;

    return out->type;  // Return event type (non-zero = event received)
}

int64_t sys_win_invalidate(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_windows[handle].redraw_pending = 1;   // #453: app explicitly asked to repaint
    window_invalidate(user_windows[handle].window);
    wm_invalidate_rect(&user_windows[handle].window->bounds);
    return 0;
}

int64_t sys_win_get_pos(int handle, int *x, int *y) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }
    window_t *win = user_windows[handle].window;
    if (x) *x = (int)win->bounds.x;
    if (y) *y = (int)win->bounds.y;
    return 0;
}

// #334: reposition a user window to an absolute screen position. Mirrors the
// SYS_WM_MAXIMIZE_WINDOW pattern (change kernel window bounds, force a full
// redraw); the compositor composites app windows at their kernel-reported
// bounds (see sys_wm_get_windows), so the moved window follows on the next
// frame. This makes ANY app window movable, including borderless (nochrome)
// panels like the Maytera HiFi that have no title bar for the WM to drag.
int64_t sys_win_move(int handle, int x, int y) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    window_t *win = user_windows[handle].window;
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);
    extern void wm_invalidate_all(void);
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int w  = (int)win->bounds.width;
    // Clamp so at least a 48px sliver stays reachable on screen.
    int min_x = -(w - 48);
    int max_x = sw - 48;
    if (x < min_x) x = min_x;
    if (x > max_x) x = max_x;
    if (y < 0) y = 0;
    if (y > sh - 24) y = sh - 24;
    win->bounds.x = x;
    win->bounds.y = y;
    wm_invalidate_all();
    return 0;
}

int64_t sys_win_move_by(int handle, int dx, int dy) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    window_t *win = user_windows[handle].window;
    return sys_win_move(handle, (int)win->bounds.x + dx, (int)win->bounds.y + dy);
}

int64_t sys_win_get_size(int handle, int *width, int *height) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (width) *width = uw->content_width;
    if (height) *height = uw->content_height;
    return 0;
}

// Fix sign-extended 32-bit user pointer (userland passes 32-bit addr, kernel zero-extends)
// #503: the prefix is USER_PTR_SX_PREFIX (proc/syscall_argtab.h) and is MIRRORED
// by sanitize_user_ptr() in rustkern.rs, because the argtab validator has to
// validate the address this macro produces rather than the raw arg Ring 3 passed.
// If you change the condition here, change it there; syscall_argtab_lock.c
// asserts the constant but cannot assert the shape of this expression.
#define SANITIZE_USER_PTR(ptr, type) do { \
    uint64_t __addr = (uint64_t)(ptr); \
    if ((__addr & USER_PTR_SX_PREFIX) == USER_PTR_SX_PREFIX) { \
        (ptr) = (type)((__addr) & 0xFFFFFFFFULL); \
    } \
} while(0)

int64_t sys_win_blit(int handle, int __attribute__((unused)) x, int __attribute__((unused)) y,
                     int src_w, int src_h, uint32_t *src_buffer) {
    SANITIZE_USER_PTR(src_buffer, uint32_t *);

    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !src_buffer) {
        return -1;
    }

    window_t *win = uw->window;
    // Size the content to the window's TRUE content rectangle (inside the
    // title bar + borders), so a full-window blit fills it exactly and never
    // overflows the frame. (Was bounds.width x bounds.height-30, which is too
    // wide and the wrong height -> right overflow + bottom gap.)
    int32_t cbx, cby, cbw, cbh;
    window_get_content_bounds(win, &cbx, &cby, &cbw, &cbh);
    int dst_w = cbw;
    int dst_h = cbh;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    // Grow content buffer if needed
    if (dst_w > uw->content_width || dst_h > uw->content_height) {
        int new_w = (dst_w > uw->content_width)  ? dst_w : uw->content_width;
        int new_h = (dst_h > uw->content_height) ? dst_h : uw->content_height;
        uint32_t *new_buf = (uint32_t *)kmalloc(new_w * new_h * 4);
        if (new_buf) {
            for (int i = 0; i < new_w * new_h; i++) new_buf[i] = 0xFF000000;
            if (uw->content_buffer) kfree(uw->content_buffer);
            uw->content_buffer = new_buf;
            uw->content_width  = new_w;
            uw->content_height = new_h;
        }
    }

    // Nearest-neighbor scale from src (src_w x src_h) to dst (dst_w x dst_h)
    int scale_x_fp = (src_w << 8) / dst_w;
    int scale_y_fp = (src_h << 8) / dst_h;
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (dy * scale_y_fp) >> 8;
        if (sy >= src_h) sy = src_h - 1;
        uint32_t *src_row = src_buffer + sy * src_w;
        uint32_t *dst_row = uw->content_buffer + dy * dst_w;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * scale_x_fp) >> 8;
            if (sx >= src_w) sx = src_w - 1;
            dst_row[dx] = src_row[sx];
        }
    }

    window_invalidate(win);
    return 0;
}

// Blit a w*h BGRA pixel buffer into a user window's content buffer at (x,y),
// clipped. Used by the browser to place decoded inline <img> images.
int64_t sys_win_draw_image(int handle, int x, int y, int w, int h, uint32_t *src) {
    SANITIZE_USER_PTR(src, uint32_t *);
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !src || w <= 0 || h <= 0) return -1;
    int cw = uw->content_width, ch = uw->content_height;
    for (int ry = 0; ry < h; ry++) {
        int dy = y + ry;
        if (dy < 0 || dy >= ch) continue;
        uint32_t *drow = &uw->content_buffer[(uint32_t)dy * (uint32_t)cw];
        uint32_t *srow = &src[(uint32_t)ry * (uint32_t)w];
        for (int rx = 0; rx < w; rx++) {
            int dx = x + rx;
            if (dx < 0 || dx >= cw) continue;
            drow[dx] = srow[rx];
        }
    }
    window_invalidate(uw->window);
    return 0;
}

// Parallel image downscale (#279 stage 3a). One output-row range per core; the
// range fn touches only kernel memory (src image + a kernel dst), so it is safe
// to run on APs (which lack the caller's user mappings).
struct img_scale_ctx { const uint32_t *src; uint32_t *dst; int sw, sh, dw, dh; };
static void img_scale_rows(int y0, int y1, void *vc) {
    struct img_scale_ctx *c = (struct img_scale_ctx *)vc;
    for (int y = y0; y < y1; y++) {
        int sy = y * c->sh / c->dh;
        const uint32_t *srow = &c->src[(uint32_t)sy * (uint32_t)c->sw];
        uint32_t *drow = &c->dst[(uint32_t)y * (uint32_t)c->dw];
        for (int x = 0; x < c->dw; x++) {
            uint32_t p = srow[x * c->sw / c->dw];
            uint32_t a = (p >> 24) & 0xFF;
            if (a < 255) {
                uint32_t ia = 255u - a;
                uint32_t r = (((p >> 16) & 0xFF) * a + 255u * ia) / 255u;
                uint32_t g = (((p >> 8)  & 0xFF) * a + 255u * ia) / 255u;
                uint32_t b = (( p        & 0xFF) * a + 255u * ia) / 255u;
                p = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            drow[x] = p;
        }
    }
}

int64_t sys_decode_image(const void *data, uint32_t len, uint32_t target,
                         void *out, uint32_t out_cap, int *dims) {
    SANITIZE_USER_PTR(data, const void *);
    SANITIZE_USER_PTR(out, void *);
    SANITIZE_USER_PTR(dims, int *);
    if (!data || !len || !out || !dims) return -1;
    int tw = (int)(target >> 16), th = (int)(target & 0xFFFF);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    extern int image_load(const void *data, uint32_t size, image_t *img);
    extern void image_free(image_t *img);
    image_t img; img.pixels = 0; img.width = 0; img.height = 0;
    if (image_load(data, len, &img) != 0 || !img.pixels || !img.width || !img.height) {
        if (img.pixels) image_free(&img);
        return -1;
    }
    int sw = (int)img.width, sh = (int)img.height;
    int dw = sw, dh = sh;
    if (dw > tw) { dh = (int)((long)dh * tw / dw); dw = tw; }
    if (dh > th) { dw = (int)((long)dw * th / dh); dh = th; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if ((uint32_t)(dw * dh * 4) > out_cap) {
        int maxrows = (int)(out_cap / (uint32_t)(dw * 4));
        if (maxrows < 1) { image_free(&img); return -1; }
        dh = maxrows;
    }
    uint32_t *dst = (uint32_t *)out;
    // #279 stage 3a: scale in parallel across cores. APs cannot write the user
    //  buffer (no user mappings), so on multi-core scale into a kernel temp
    // then copy to user here; single-core scales in place.
    extern int smp_get_core_count(void);
    extern void smp_parallel_for(int, int, void (*)(int, int, void *), void *);
    uint32_t outbytes = (uint32_t)dw * (uint32_t)dh * 4u;
    uint32_t *kdst = (smp_get_core_count() > 1) ? (uint32_t *)kmalloc(outbytes) : 0;
    if (kdst) {
        struct img_scale_ctx ctx = { img.pixels, kdst, sw, sh, dw, dh };
        smp_parallel_for(0, dh, img_scale_rows, &ctx);
        memcpy(dst, kdst, outbytes);
        kfree(kdst);
    } else {
        struct img_scale_ctx ctx = { img.pixels, dst, sw, sh, dw, dh };
        img_scale_rows(0, dh, &ctx);
    }
    image_free(&img);
    dims[0] = dw; dims[1] = dh;
    return (int64_t)(dw * dh * 4);
}


// ============================================================================
// Filesystem syscalls (mkdir, rmdir, unlink, rename, readdir)
// ============================================================================

int64_t sys_mkdir(const char *path, int mode) {
    (void)mode;
    if (!path) return -1;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_mkdir(path) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        strncpy(parent, path, sizeof(parent) - 1);
        parent[255] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
        } else {
            parent[0] = '/'; parent[1] = '\0';
        }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    int ret = fat_mkdir(&g_fat_fs, path);
    if (ret == 0 && p) {
        perms_set_default(path, p->euid, p->egid, 1);
    }
    return ret;
}

int64_t sys_rmdir(const char *path) {
    if (!path) return -1;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_rmdir(path) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        strncpy(parent, path, sizeof(parent) - 1);
        parent[255] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
        } else {
            parent[0] = '/'; parent[1] = '\0';
        }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    int ret = fat_delete(&g_fat_fs, path);
    if (ret == 0) perms_remove(path);
    return ret;
}

int64_t sys_unlink(const char *path) {
    if (!path) return -1;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_delete(path) == 0 ? 0 : -1;
    }

    // ext2 volume (#99): delete on the mounted ext2 filesystem.
    if (path_is_ext2(path)) {
        return ext2_unlink(ext2_relpath(path)) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        strncpy(parent, path, sizeof(parent) - 1);
        parent[255] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
        } else {
            parent[0] = '/'; parent[1] = '\0';
        }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    int ret = fat_delete(&g_fat_fs, path);
    if (ret == 0) perms_remove(path);
    return ret;
}

int64_t sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;

    // #317: SMB network share (both paths must be on the same share).
    if (path_is_smb(oldpath) || path_is_smb(newpath)) {
        if (!path_is_smb(oldpath) || !path_is_smb(newpath)) return -1;
        if (smb_vfs_ensure_mount(oldpath) != 0) return -1;
        return smb_rename(oldpath, newpath) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on both parent directories
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        strncpy(parent, oldpath, sizeof(parent) - 1);
        parent[255] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) *last_slash = '\0';
        else { parent[0] = '/'; parent[1] = '\0'; }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) return -1;

        strncpy(parent, newpath, sizeof(parent) - 1);
        parent[255] = '\0';
        last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) *last_slash = '\0';
        else { parent[0] = '/'; parent[1] = '\0'; }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) return -1;
    }

    int ret = fat_rename(&g_fat_fs, oldpath, newpath);
    if (ret == 0) {
        uint32_t uid, gid;
        uint16_t mode;
        if (perms_get(oldpath, &uid, &gid, &mode) == 0) {
            perms_remove(oldpath);
            perms_set(newpath, uid, gid, mode);
        }
    }
    return ret;
}

int64_t sys_readdir(int fd, void *entry_buf) {
    // Read next directory entry from an open directory fd.
    // entry_buf points to a dirent structure: { char name[256]; uint32_t type; uint32_t size; }
    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    if (!entry_buf) return -1;

    typedef struct {
        char name[256];
        uint32_t type;    // 0 = file, 1 = directory
        uint32_t size;
    } dirent_t;
    // #503 argtab sizeof-lock: SYS_READDIR arg2 (SZ_DIRENT in rustkern.rs).
    _Static_assert(sizeof(dirent_t) == 264,
                   "#503 argtab: SZ_DIRENT in rustkern.rs is stale");

    // #317 pass 4: NFS-backed directory fd. NFSv3 READDIR carries only names
    // (no type/size), so entries are reported as files with size 0; cat/ls work.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used && smbfd[fd].is_dir &&
        smbfd[fd].is_nfs) {
        smb_fd_t *s = &smbfd[fd];
        dirent_t *de = (dirent_t *)entry_buf;
        nfs_entry3_t *e;
        for (;;) {
            e = nfs_readdir(s->dirh);
            if (!e) return -1;   // end-of-dir or error
            if (e->name[0] == '.' &&
                (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0)))
                continue;
            break;
        }
        int i = 0; while (e->name[i] && i < 255) { de->name[i] = e->name[i]; i++; }
        de->name[i] = '\0';
        de->type = 0;
        de->size = 0;
        return 0;
    }

    // #317: SMB-backed directory fd.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used && smbfd[fd].is_dir) {
        smb_fd_t *s = &smbfd[fd];
        dirent_t *de = (dirent_t *)entry_buf;
        smb_dirent_t sde;
        // Skip the "." and ".." pseudo-entries the server returns; the Files app
        // synthesizes its own ".." row.
        for (;;) {
            int r = smb_readdir(s->dirh, &sde);
            if (r != 0) return -1;   // 1 = end-of-dir, -1 = error
            if (sde.name[0] == '.' &&
                (sde.name[1] == 0 || (sde.name[1] == '.' && sde.name[2] == 0)))
                continue;
            break;
        }
        int i = 0; while (sde.name[i] && i < 255) { de->name[i] = sde.name[i]; i++; }
        de->name[i] = '\0';
        de->type = sde.is_directory ? 1 : 0;
        de->size = (uint32_t)sde.size;
        return 0;
    }

    // ext2-backed directory fd.
    if (fd >= 3 && e2fd[fd].used && e2fd[fd].is_dir) {
        ext2_fd_t *e = &e2fd[fd];
        dirent_t *de = (dirent_t *)entry_buf;
        char nm[256]; uint32_t cino = 0; uint8_t ft = 0;
        if (ext2_readdir_ino(e->dir_ino, &e->dir_pos, nm, sizeof(nm), &cino, &ft) != 0) return -1;
        int i = 0; while (nm[i] && i < 255) { de->name[i] = nm[i]; i++; } de->name[i] = '\0';
        de->type = (ft == 2) ? 1 : 0;   // EXT2_FT_DIR == 2
        de->size = 0;
        if (ft != 2) { ext2_inode_t in; if (ext2_read_inode(cino, &in) == 0) de->size = in.i_size; }
        return 0;
    }

    dirent_t *de = (dirent_t *)entry_buf;

    // Use the FAT readdir function which takes a fat_dir_entry_t
    fat_dir_entry_t raw_entry;
    char name_buf[256];
    int ret = fat_readdir(&fd_table[fd], &raw_entry, name_buf);
    if (ret != 0) return -1;  // No more entries

    // Copy the name
    int i = 0;
    while (name_buf[i] && i < 255) { de->name[i] = name_buf[i]; i++; }
    de->name[i] = '\0';

    // Set type: bit 0x10 in attr means directory
    de->type = (raw_entry.attr & 0x10) ? 1 : 0;
    de->size = raw_entry.file_size;

    return 0;  // Success, got an entry
}

/* #317 pass 4: verify the terminal cat/ls path (sys_open/sys_read/sys_readdir)
   over the persisted network mounts loaded from /CONFIG/NETMOUNTS.CFG. This is
   the exact syscall path the shell `ls`/`cat` and the Files app use, exercised
   for both /SMB and /NFS. Runs from the deferred net worker; logs to serial. */
void netfs_fdpath_selftest(void) {
    extern int net_mounts_count(void);
    extern const char *net_mounts_path(int);
    typedef struct { char name[256]; uint32_t type; uint32_t size; } dirent_t;
    int n = net_mounts_count();
    if (n <= 0) return;
    kprintf("\n========== NETFS FD-PATH (task #317 pass 4) ==========\n");
    for (int m = 0; m < n; m++) {
        const char *mp = net_mounts_path(m);
        if (!mp) continue;
        kprintf("[NETFD] --- %s ---\n", mp);
        // ls: open the mount root as a directory, enumerate via sys_readdir.
        int dfd = (int)sys_open(mp, 0);   // O_RDONLY
        if (dfd < 0) { kprintf("[NETFD]   open dir FAILED\n"); continue; }
        int cnt = 0; dirent_t de;
        char first[256]; first[0] = 0;
        while (sys_readdir(dfd, &de) == 0 && cnt < 64) {
            if (cnt == 0) { int i=0; while(de.name[i]&&i<255){first[i]=de.name[i];i++;} first[i]=0; }
            kprintf("[NETFD]   %s\n", de.name);
            cnt++;
        }
        sys_close(dfd);
        kprintf("[NETFD]   ls: %d entries (first=%s)\n", cnt, first);
        // cat: read TEST.TXT from the mount root via the fd path.
        char fp[320]; snprintf(fp, sizeof(fp), "%s/TEST.TXT", mp);
        int ffd = (int)sys_open(fp, 0);
        if (ffd >= 0) {
            char buf[256]; int64_t r = sys_read(ffd, buf, sizeof(buf) - 1);
            if (r > 0) { buf[r] = 0; kprintf("[NETFD]   cat %s (%d bytes): %s\n", fp, (int)r, buf); }
            else kprintf("[NETFD]   cat %s read=%d\n", fp, (int)r);
            sys_close(ffd);
        } else {
            kprintf("[NETFD]   (no TEST.TXT to cat at this mount)\n");
        }
    }
    kprintf("======================================================\n\n");
}

/* #308 self-test: exercise the EXACT userland directory-listing path
   (sys_open(dir, O_RDONLY) followed by sys_readdir) on the ext2 root, the way
   the Files app does it. Logs results to serial at boot so we can confirm
   directory opens + readdir work from the syscall layer (not just kernel-side
   ext2_readdir_ino). Tests "/", "/APPS", "/CONFIG". */
void ext2_dir_open_selftest(void) {
    extern int g_root_ext2;
    if (!g_root_ext2) { kprintf("[#308] ext2 not root; skipping dir-open self-test\n"); return; }
    static const char *dirs[] = { "/", "/APPS", "/CONFIG" };
    typedef struct { char name[256]; uint32_t type; uint32_t size; } dirent_t;
    for (unsigned di = 0; di < sizeof(dirs)/sizeof(dirs[0]); di++) {
        const char *path = dirs[di];
        int64_t fd = sys_open(path, 0);
        if (fd < 0) {
            kprintf("[#308] FAIL: sys_open(%s) returned -1 (dir open broken)\n", path);
            continue;
        }
        int count = 0;
        dirent_t de;
        char first[64]; first[0] = 0;
        while (sys_readdir((int)fd, &de) == 0) {
            if (count == 0) { int i=0; while (de.name[i] && i<63){first[i]=de.name[i];i++;} first[i]=0; }
            count++;
            if (count > 5000) break;
        }
        sys_close((int)fd);
        kprintf("[#308] sys_open(%s)=%ld, sys_readdir listed %d entries (first=%s)\n",
                path, (long)fd, count, first);
        if (count == 0)
            kprintf("[#308] FAIL: dir %s opened but listed 0 entries (readdir broken)\n", path);
        else
            kprintf("[#308] PASS: dir %s listing works from userland syscall path\n", path);
    }
}


// ============================================================================
// Working directory syscalls
// ============================================================================

int64_t sys_getcwd(char *buf, uint64_t size) {
    process_t *p = proc_current();
    if (!p || !buf || size == 0) return -1;
    // Return "/" as default if cwd has not been set
    const char *src = (p->cwd[0]) ? p->cwd : "/";
    uint64_t i = 0;
    while (i < size - 1 && src[i]) { buf[i] = src[i]; i++; }
    buf[i] = '\0';
    return (int64_t)i;
}

int64_t sys_chdir(const char *path) {
    process_t *p = proc_current();
    if (!p || !path) return -1;

    // Build absolute resolved path
    char resolved[256];
    if (path[0] == '/') {
        int i = 0;
        while (i < 255 && path[i]) { resolved[i] = path[i]; i++; }
        resolved[i] = '\0';
    } else {
        // Relative path: prepend current cwd
        const char *base = (p->cwd[0]) ? p->cwd : "/";
        int ci = 0;
        while (ci < 254 && base[ci]) { resolved[ci] = base[ci]; ci++; }
        if (ci > 0 && resolved[ci - 1] != '/') { resolved[ci++] = '/'; }
        int pi = 0;
        while (ci < 255 && path[pi]) { resolved[ci++] = path[pi++]; }
        resolved[ci] = '\0';
    }

    // Validate: try to open the path to confirm it exists
    int fd = (int)sys_open(resolved, 0);
    if (fd < 0) return -1;  // path does not exist
    sys_close(fd);

    // Update process cwd
    int i = 0;
    while (i < 255 && resolved[i]) { p->cwd[i] = resolved[i]; i++; }
    p->cwd[i] = '\0';
    return 0;
}

// ============================================================================
// User identity syscalls
// ============================================================================

int64_t sys_getuid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->uid : -1;
}

int64_t sys_setuid(uint32_t uid) {
    process_t *p = proc_current();
    if (!p) return -1;

    // Root (euid 0) can set to any UID
    if (p->euid == 0) {
        p->uid = uid;
        p->euid = uid;
        return 0;
    }

    // Non-root can only set euid back to real uid
    if (uid == p->uid) {
        p->euid = uid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_getgid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->gid : -1;
}

int64_t sys_setgid(uint32_t gid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->gid = gid;
        p->egid = gid;
        return 0;
    }

    if (gid == p->gid) {
        p->egid = gid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_geteuid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->euid : -1;
}

int64_t sys_getegid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->egid : -1;
}

int64_t sys_seteuid(uint32_t euid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->euid = euid;
        return 0;
    }

    if (euid == p->uid) {
        p->euid = euid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_setegid(uint32_t egid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->egid = egid;
        return 0;
    }

    if (egid == p->gid) {
        p->egid = egid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_chmod(const char *path, uint16_t mode) {
    if (!path) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    return perms_chmod(path, p->euid, mode);
}

int64_t sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // Only root can change ownership
    if (p->euid != 0) return -1;  // EPERM

    // Get current mode, then update owner
    uint32_t cur_uid, cur_gid;
    uint16_t cur_mode;
    if (perms_get(path, &cur_uid, &cur_gid, &cur_mode) != 0) {
        cur_mode = 0755;  // Default
    }
    perms_set(path, uid, gid, cur_mode);
    return 0;
}

int64_t sys_passwd_change(const char *username, const char *old_pass, const char *new_pass) {
    if (!username || !new_pass) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // Look up the target user
    user_entry_t *target = user_lookup_name(username);
    if (!target) return -1;

    // Non-root users can only change their own password and must provide old password
    if (p->euid != 0) {
        if (target->uid != p->euid) return -1;  // EPERM
        if (!old_pass) return -1;
        if (user_verify_password(username, old_pass) != 0) return -1;
    }

    return user_set_password(username, new_pass);
}

int64_t sys_su(const char *username, const char *password) {
    if (!username || !password) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // Verify credentials
    if (user_verify_password(username, password) != 0) {
        return -1;
    }

    // Look up user to get UID/GID
    user_entry_t *target = user_lookup_name(username);
    if (!target) return -1;

    // Switch identity
    p->uid  = target->uid;
    p->gid  = target->gid;
    p->euid = target->uid;
    p->egid = target->gid;

    return 0;
}

int64_t sys_adduser(const char *username, uint32_t uid, uint32_t gid,
                    const char *home, const char *shell) {
    if (!username) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // Only root can create users
    if (p->euid != 0) return -1;  // EPERM

    int ret = user_create(username, uid, gid,
                          home ? home : "/HOME",
                          shell ? shell : "/APPS/MSH",
                          username);
    if (ret != 0) return -1;

    // Create home directory if specified
    if (home && home[0]) {
        fat_mkdir(&g_fat_fs, home);
        perms_set(home, uid, gid, 0750);
        users_make_home_skeleton(home, uid, gid);
    }

    // Persist to disk
    users_sync();

    return 0;
}

int64_t sys_set_theme(int theme_id) {
    extern void theme_set(int theme_id);
    extern void wm_invalidate_all(void);
    theme_set(theme_id);
    wm_invalidate_all();
    return 0;
}

int64_t sys_get_theme(void) {
    extern int theme_get_current_id(void);
    return (int64_t)theme_get_current_id();
}

int64_t sys_set_volume(int volume) {
    extern int audio_set_master_volume(int volume);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    audio_set_master_volume(volume);
    return 0;
}

int64_t sys_get_volume(void) {
    extern int audio_get_volume(void *vol);
    // audio_volume_t has master_left as first int field; read it directly
    int buf[16];
    if (audio_get_volume(buf) == 0) {
        int v = buf[0]; // master_left
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        return (int64_t)v;
    }
    return 0;
}

int64_t sys_set_mute(int mute_flag) {
    extern int audio_mute(int mute);
    audio_mute(mute_flag != 0);
    return 0;
}

int64_t sys_get_disk_total(void) {
    extern fat_fs_t g_fat_fs;
    uint64_t bytes = (uint64_t)g_fat_fs.cluster_count
                     * g_fat_fs.sectors_per_cluster
                     * g_fat_fs.bytes_per_sector;
    return (int64_t)(bytes / (1024 * 1024));
}

int64_t sys_get_disk_free(void) {
    extern fat_fs_t g_fat_fs;
    extern uint32_t fat_get_free_clusters(fat_fs_t *fs);
    uint32_t free_clusters = fat_get_free_clusters(&g_fat_fs);
    uint64_t bytes = (uint64_t)free_clusters
                     * g_fat_fs.sectors_per_cluster
                     * g_fat_fs.bytes_per_sector;
    return (int64_t)(bytes / (1024 * 1024));
}

int64_t sys_set_mouse_speed(int speed) {
    extern void mouse_set_sensitivity(int s);
    mouse_set_sensitivity(speed);
    return 0;
}

int64_t sys_get_mouse_speed(void) {
    extern int mouse_get_sensitivity(void);
    return (int64_t)mouse_get_sensitivity();
}

int64_t sys_get_rtc_time(void) {
    extern void rtc_read_time(int *hour, int *minute, int *second);
    int h = 0, m = 0, s = 0;
    rtc_read_time(&h, &m, &s);
    return (int64_t)((h << 16) | (m << 8) | s);
}

int64_t sys_get_rtc_date(void) {
    extern void rtc_read_date(int *day, int *month, int *year, int *weekday);
    int d = 1, mo = 1, y = 2026, wd = 0;
    rtc_read_date(&d, &mo, &y, &wd);
    // Pack: year in upper 16 bits, month in next 8, day in lowest 8
    return (int64_t)(((int64_t)y << 16) | ((mo & 0xFF) << 8) | (d & 0xFF));
}

// ============================================================================
// RTC write helpers (set time / set date)
// ============================================================================

static void rtc_write_reg(uint8_t reg, uint8_t val) {
    outb(0x70, reg);
    outb(0x71, val);
}

static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

int64_t sys_set_rtc_time(uint64_t packed) {
    int h = (int)((packed >> 16) & 0xFF);
    int m = (int)((packed >>  8) & 0xFF);
    int s = (int)(packed & 0xFF);
    // Poll until RTC not updating
    outb(0x70, 0x0A);
    while (inb(0x71) & 0x80) { outb(0x70, 0x0A); }
    rtc_write_reg(0x00, bin_to_bcd((uint8_t)s));
    rtc_write_reg(0x02, bin_to_bcd((uint8_t)m));
    rtc_write_reg(0x04, bin_to_bcd((uint8_t)h));
    return 0;
}

int64_t sys_set_rtc_date(uint64_t packed) {
    int y  = (int)((packed >> 16) & 0xFFFF);
    int mo = (int)((packed >>  8) & 0xFF);
    int d  = (int)(packed & 0xFF);
    outb(0x70, 0x0A);
    while (inb(0x71) & 0x80) { outb(0x70, 0x0A); }
    rtc_write_reg(0x07, bin_to_bcd((uint8_t)d));
    rtc_write_reg(0x08, bin_to_bcd((uint8_t)mo));
    rtc_write_reg(0x09, bin_to_bcd((uint8_t)(y % 100)));
    return 0;
}

// ============================================================================
// Network info syscall
// ============================================================================

static void ip_to_str(const uint8_t *ip, char *out) {
    // Format 4 bytes as A.B.C.D
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        if (v >= 100) { out[pos++] = '0' + v / 100; }
        if (v >= 10)  { out[pos++] = '0' + (v / 10) % 10; }
        out[pos++] = '0' + v % 10;
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

static void mac_to_str(const uint8_t *mac, char *out) {
    // Format 6 bytes as XX:XX:XX:XX:XX:XX
    const char *hex = "0123456789ABCDEF";
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        out[pos++] = hex[(mac[i] >> 4) & 0xF];
        out[pos++] = hex[mac[i] & 0xF];
        if (i < 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

int64_t sys_get_net_info(void *buf, uint64_t len) {
    if (!buf || len < sizeof(net_info_t)) return -1;
    net_info_t *ni = (net_info_t *)buf;
    // Report the IP layer's ACTUAL configured address, not the DHCP-offered one.
    // The kernel currently boots with a static IP (ip_set_address), so the DHCP
    // module's offered IP stays 0 and the panel wrongly showed "Disconnected"
    // even though networking is up. DHCP also routes through ip_set_address, so
    // this is correct for both static and DHCP configurations.
    extern uint32_t ip_get_address(void);
    extern uint32_t ip_get_gateway(void);
    extern uint32_t ip_get_netmask(void);
    // ip_to_str() reads the 4 address bytes in memory order, so the values
    // must be in network byte order; the IP layer stores them in host order.
    #define NETINFO_BSWAP(v) ((((v) & 0xFFu) << 24) | (((v) & 0xFF00u) << 8) | \
                              (((v) >> 8) & 0xFF00u) | (((v) >> 24) & 0xFFu))
    uint32_t ip  = NETINFO_BSWAP(ip_get_address());
    uint32_t gw  = NETINFO_BSWAP(ip_get_gateway());
    uint32_t nm  = NETINFO_BSWAP(ip_get_netmask());
    uint32_t dnsv = dhcp_get_dns();
    if (dnsv == 0) dnsv = ip_get_gateway();  // static config: show the gateway
    uint32_t dns = NETINFO_BSWAP(dnsv);
    #undef NETINFO_BSWAP
    uint8_t  mac[6];
    nic_get_mac(mac);
    ip_to_str((uint8_t *)&ip,  ni->ip);
    ip_to_str((uint8_t *)&gw,  ni->gateway);
    ip_to_str((uint8_t *)&nm,  ni->netmask);
    ip_to_str((uint8_t *)&dns, ni->dns);
    mac_to_str(mac, ni->mac);
    ni->connected = (ip != 0) ? 1 : 0;
    return 0;
}

// Parse a dotted-quad "a.b.c.d" into the IP layer's stored representation
// ((a<<24)|(b<<16)|(c<<8)|d), the inverse of what sys_get_net_info reports.
static uint32_t settings_parse_ip(const char *s) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int pi = 0, val = 0;
    const char *p = s;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        } else if (*p == '.' || *p == '\0') {
            if (pi < 4) parts[pi] = (uint32_t)(val & 0xFF);
            pi++; val = 0;
            if (*p == '\0') break;
        } else {
            break;
        }
    }
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

int64_t sys_net_set_static(const char *ip, const char *mask, const char *gw) {
    extern void ip_set_address(uint32_t);
    extern void ip_set_netmask(uint32_t);
    extern void ip_set_gateway(uint32_t);
    if (!ip || !mask || !gw) return -1;
    ip_set_address(settings_parse_ip(ip));
    ip_set_netmask(settings_parse_ip(mask));
    ip_set_gateway(settings_parse_ip(gw));
    { extern void net_clear_fault(void); net_clear_fault(); }   // #549: manual reconfigure = recovery
    kprintf("[NET] Static config applied: ip=%s mask=%s gw=%s\n", ip, mask, gw);
    return 0;
}

int64_t sys_get_disk_info(int idx, void *buf) {
    if (!buf || idx < 0 || idx > 3) return -1;
    extern int ata_drive_present(int);
    extern int ata_drive_type(int);
    extern int ata_drive_smart(int);
    extern unsigned long ata_drive_sectors(int);
    extern const char *ata_drive_model(int);
    extern const char *ata_drive_serial(int);
    sc_disk_info_t *d = (sc_disk_info_t *)buf;
    for (uint64_t i = 0; i < sizeof(*d); i++) ((uint8_t *)d)[i] = 0;
    if (!ata_drive_present(idx)) return -1;
    d->present = 1;
    d->type    = (uint8_t)ata_drive_type(idx);
    d->smart   = (int8_t)ata_drive_smart(idx);
    d->size_mb = (uint32_t)((ata_drive_sectors(idx) * 512UL) / (1024UL * 1024UL));
    const char *m = ata_drive_model(idx);
    int k; for (k = 0; k < 40 && m[k]; k++) d->model[k] = m[k];
    d->model[k] = 0;
    const char *sr = ata_drive_serial(idx);
    for (k = 0; k < 20 && sr[k]; k++) d->serial[k] = sr[k];
    d->serial[k] = 0;
    return 0;
}

// Play a sound file (MP3/WAV/...) by path, asynchronously (kernel thread).
int64_t sys_play_wav(const char *upath) {
    if (!upath) return -1;
    char path[128];
    int i = 0;
    for (; i < 127 && upath[i]; i++) path[i] = upath[i];
    path[i] = '\0';
    extern int audio_play_file_async(const char *path);
    return audio_play_file_async(path);
}

// ============================================================================
// Ring-3 PCM push (Phase 1 of the Ring-0 media-decode exit).
//
// These are deliberately thin: every check (format/rate/channel validation,
// user-pointer bounds, stream ownership by pid) and all #426 wait-queue
// blocking lives in drivers/audio_pcm.c next to the state it guards, rather
// than being smeared across the dispatcher. See drivers/audio_pcm.h.
// ============================================================================

int64_t sys_audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format) {
    return audio_pcm_open(rate, channels, format);
}

int64_t sys_audio_pcm_write(int handle, const void *pcm, uint32_t frames) {
    return audio_pcm_write(handle, pcm, frames);
}

int64_t sys_audio_pcm_close(int handle) {
    return audio_pcm_close(handle);
}

// ============================================================================
// NTP sync syscall
// ============================================================================

static volatile int g_ntp_done = 0;
static uint32_t     g_ntp_unix_ts = 0;

static void ntp_udp_cb(uint32_t src, uint16_t sp, const void *vdata, uint16_t len) {
    (void)src; (void)sp;
    const uint8_t *data = (const uint8_t *)vdata;
    if (len >= 48) {
        // Transmit timestamp starts at byte 40 (NTP epoch: Jan 1, 1900)
        uint32_t ntp_sec = ((uint32_t)data[40] << 24) | ((uint32_t)data[41] << 16)
                         | ((uint32_t)data[42] <<  8) |  (uint32_t)data[43];
        g_ntp_unix_ts = ntp_sec - 2208988800UL;
        g_ntp_done = 1;
    }
}

static void unix_days_to_ymd(int days, int *year_out, int *month_out, int *day_out) {
    int y = 1970;
    while (1) {
        int leap = ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0));
        int diy = leap ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        y++;
    }
    *year_out = y;
    int leap = ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0));
    int dpm[12] = {31, leap?29:28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int mo = 0;
    while (mo < 12 && days >= dpm[mo]) { days -= dpm[mo]; mo++; }
    *month_out = mo + 1;
    *day_out   = days + 1;
}

int64_t sys_ntp_sync(void) {
    uint8_t pkt[48];
    for (int i = 0; i < 48; i++) pkt[i] = 0;
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3 (client)

    g_ntp_done = 0;
    g_ntp_unix_ts = 0;

    uint32_t srv = (216u << 24) | (239u << 16) | (35u << 8) | 0u;
    udp_bind(12300, ntp_udp_cb);
    if (udp_send(srv, 12300, 123, pkt, 48) < 0) {
        udp_unbind(12300);
        return -1;
    }

    // #512: this loop had three defects, all fixed here.
    //
    // (a) UNITS. It was `timer_ticks + 2000`, which READS like 2 seconds and is
    //     2000 TICKS = 8 seconds at the actual g_timer_hz of 250. Same family as
    //     #420 (the 18.2Hz-vs-250Hz confusion in wget/https). Now the ms->ticks
    //     conversion is done by the one sanctioned helper, wq_ms_to_ticks(),
    //     against the LIVE tick rate, so the number below means what it says.
    //
    // (b) IT COULD NOT COMPLETE ON ITS OWN. g_ntp_done is set only by
    //     ntp_udp_cb(), which runs only from net_poll() -- and the old loop body
    //     never called net_poll(). The wait therefore depended entirely on some
    //     OTHER thread happening to pump the stack (the compositor's sys_fb_flip
    //     calls net_poll()). Where the compositor is not running or not flipping
    //     (serial/headless boot, compositor blocked), the sync could NEVER
    //     succeed and always failed after the full 8s. Correctness by
    //     coincidence. We now pump net_poll() ourselves every pass, exactly as
    //     net/dns.c's dns_wait() already does for the same UDP request/response
    //     shape. This is deliberately NOT converted to a blocking wait: per the
    //     wait-migration plan these net_poll()+sleep loops ARE the TCP/IP
    //     stack's execution engine (net_worker() only pumps while DHCP is
    //     unbound), so a bare block here would stop the stack and the wake would
    //     never come. This site collapses into sock_wait() in Phase 4, once an
    //     always-armed net service worker exists to drive the stack.
    //
    // (c) IT BURNED A FULL CORE. `pause` with no yield, for up to 8 seconds,
    //     reachable from Ring 3 via the Settings date/time panel. It is
    //     preemptible (IF=1, and the scheduler drops the BKL across a switch via
    //     bkl_release_all), so it did not hard-wedge the box, but it stayed
    //     runnable and competed for CPU the whole time while Settings froze.
    //     proc_sleep() parks us instead, which also releases the BKL so the
    //     compositor and the rest of the desktop keep running.
    //
    // The timeout STAYS: an NTP server is a remote peer that can go silent with
    // no detectable signal, and sys_ntp_sync owes its caller an answer. That is
    // a genuine class-B absent-event timeout, not a papered-over missing wake.
    #define NTP_TIMEOUT_MS   5000u   // typical NTP client budget; was an 8s accident
    #define NTP_POLL_MS      2u      // matches dns_wait()'s pump interval
    uint64_t deadline = wq_deadline_in(wq_ms_to_ticks(NTP_TIMEOUT_MS));
    while (!g_ntp_done && !wq_deadline_expired(deadline)) {
        net_poll();                  // (b): pump the stack we are waiting on
        if (g_ntp_done) break;       // reply landed in this very poll
        proc_sleep(NTP_POLL_MS);     // (c): yield + release the BKL, never spin
    }

    udp_unbind(12300);
    if (!g_ntp_done) return -1;

    uint32_t ts = g_ntp_unix_ts;
    int sec = (int)(ts % 60); ts /= 60;
    int min = (int)(ts % 60); ts /= 60;
    int hr  = (int)(ts % 24); ts /= 24;
    int y, mo, d;
    unix_days_to_ymd((int)ts, &y, &mo, &d);

    sys_set_rtc_time(((uint64_t)hr << 16) | ((uint64_t)min << 8) | (uint64_t)sec);
    sys_set_rtc_date(((uint64_t)y  << 16) | ((uint64_t)mo  << 8) | (uint64_t)d);
    return 0;
}
