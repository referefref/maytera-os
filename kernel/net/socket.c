// socket.c - #524 real BSD sockets API over the existing TCP/UDP stack.
//
// See socket.h for the WHY-C rationale. Summary: this is fd-integrated glue
// entangled with the C wait_event macros, the C VFS file_ops vtable, the
// net_cr3 inline-asm CR3 window (NIC MMIO is absent from the process CR3), and
// copy_*_user. The mandated Rust part (syscall pointer descriptors) is in
// rustkern/argtab.rs. New kernel code in C here is justified by that
// entanglement, not convenience.
//
// Design notes:
//  - Descriptors are real fds (fs/vfs.h struct file). close() therefore works
//    through the normal SYS_CLOSE -> file_put -> ops->release path, and select()
//    works through file_poll(). The legacy raw SYS_SOCKET/... surface (TCB slot
//    indices) is untouched and still used by irc/nc/curl/vnc.
//  - RX in this stack is POLL-DRIVEN, not IRQ-driven (e1000.c: "RX interrupt
//    handled by polling in receive()"). A blocking recv therefore cannot simply
//    park on an async wake: nothing would fill the buffer while it slept. So the
//    blocking primitives DRIVE the NIC RX ring themselves in bounded slices via
//    wait_event_interruptible_timeout(). This is the sanctioned "timeout is the
//    correct semantics because the wake source (a remote peer) is outside our
//    control" case from CLAUDE.md, NOT a hand-rolled poll loop (which the
//    concurrency lint would reject). eth_receive() additionally fires
//    socket_net_wake() on every delivered frame, so a recv also wakes EARLY
//    whenever some other context (compositor/net_worker) drives the ring.

#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../sync/waitq.h"
#include "../cpu/mono.h"     // #499: sched_now_ms() - THE shared real-elapsed-ms clock
#include "irqwin.h"          // #69: per-site interrupts-off window accounting
#include "../security/validate.h"

// ---- externs from other TUs ----
extern int  eth_receive(void);
extern unsigned net_rx_drain_chunked(void);  // #69: THE shared chunked RX drain
extern void tcp_timer(void);
extern uint64_t vmm_get_pml4(void);
extern void net_lock(void);
extern void net_unlock(void);
extern int  tcp_rx_pending(int sock);   // #524 accessor added to tcp.c
extern void kprintf(const char *fmt, ...);

// POSIX-ish negative errnos returned to userland (libc maps to errno).
#define E_BADF        (-9)
#define E_INVAL       (-22)
#define E_FAULT       (-14)
#define E_AGAIN       (-11)
#define E_INTR        (-4)
#define E_TIMEDOUT    (-110)
#define E_CONNREFUSED (-111)
#define E_NOTCONN     (-107)
#define E_MFILE       (-24)
#define E_AFNOSUPPORT (-97)
#define E_PROTO       (-93)
#define E_INPROGRESS  (-115)
#define E_MSGSIZE     (-90)
#define E_ISCONN      (-106)
#define E_NOMEM       (-12)
#define E_PIPE        (-32)

// Layout locks: userland and the argtab descriptors assume these exact sizes.
_Static_assert(sizeof(k_sockaddr_in_t) == 16, "k_sockaddr_in_t must be 16 bytes");
_Static_assert(sizeof(k_timeval_t) == 16, "k_timeval_t must be 16 bytes");
_Static_assert(sizeof(k_fd_set_t) == 8, "k_fd_set_t must be 8 bytes");

// ---- byte order (host is little-endian x86-64) ----
static inline uint16_t sk_ntohs(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static inline uint16_t sk_htons(uint16_t h) { return sk_ntohs(h); }
static inline uint32_t sk_ntohl(uint32_t n) {
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) |
           ((n >> 8) & 0xFF00) | ((n >> 24) & 0xFF);
}
static inline uint32_t sk_htonl(uint32_t h) { return sk_ntohl(h); }

// ---- UDP receive ring ----
#define UDP_DGRAM_MAX   1472
#define UDP_RING_LEN    8
typedef struct udp_dgram {
    uint32_t src_ip;    // network byte order (as delivered)
    uint16_t src_port;  // host byte order
    uint16_t len;
    uint8_t  data[UDP_DGRAM_MAX];
} udp_dgram_t;

// ---- socket table ----
#define MAX_SOCKS 64
typedef struct sock {
    int      in_use;
    int      type;       // SOCK_STREAM / SOCK_DGRAM
    int      tcp_slot;   // STREAM: TCB index from tcp_socket(); -1 otherwise
    uint16_t local_port; // host order (UDP bound/ephemeral port); 0 = none
    int      bound;      // UDP: local_port is live for RX routing
    uint32_t rcv_timeo_ms;  // #499: ms of REAL time; 0 = block indefinitely
    uint32_t snd_timeo_ms;  // #499: ms of REAL time; 0 = block indefinitely
    int      so_error;
    // #524 shutdown() state. shut_rd => recv returns EOF(0); shut_wr => send
    // returns E_PIPE. fin_sent => a FIN has already gone out (via SHUT_WR/RDWR)
    // so sock_file_release must not double-close the TCB.
    int      shut_rd, shut_wr, fin_sent;
    // UDP RX ring (kmalloc'd for DGRAM, NULL for STREAM)
    udp_dgram_t *ring;
    int      r_head, r_tail, r_count;
    struct file *file;
} sock_t;

static sock_t g_socks[MAX_SOCKS];
static wait_queue_head_t g_net_rx_wq;   // zero-init == wait_queue_head_init()
static uint16_t g_udp_ephem = 49152;
static int g_sock_inited = 0;

static const file_ops_t g_sock_fops;    // fwd

// slice used to bound the self-drive latency of the blocking loops.
static inline uint32_t sock_slice_ticks(void) {
    uint32_t s = g_timer_hz / 50;   // ~20ms
    return s ? s : 1;
}

// Absolute deadline from a timeout in MILLISECONDS of REAL time (0 => never).
//
// #499: this used to be `timer_ticks + timeo_ticks`. timer_ticks counts ticks
// DELIVERED, not time ELAPSED: under KVM a starved vCPU gets its missed ticks
// re-delivered in a BURST (~1250 ticks, a nominal 5s at 250Hz, in ~15ms of real
// time), so EVERY socket deadline here (recv/send/connect/accept/recvfrom/
// select, SO_RCVTIMEO/SO_SNDTIMEO) could collapse to an instant E_AGAIN/
// E_TIMEDOUT exactly when the machine was busiest. The inner
// wait_event_interruptible_timeout() slice was already mono-safe; the OVERALL
// deadline was not. sched_now_ms() (cpu/mono.h) is the ONE shared real-elapsed
// -ms clock, and falls back to tick-derived ms only before the monotonic clock
// is calibrated at boot, so this is never worse than the old behaviour.
static inline uint64_t sock_deadline(uint32_t timeo_ms) {
    if (timeo_ms == 0) return WAIT_DEADLINE_NEVER;
    return sched_now_ms() + (uint64_t)timeo_ms;
}
static inline int sock_past(uint64_t deadline) {
    return deadline != WAIT_DEADLINE_NEVER &&
           (int64_t)(sched_now_ms() - deadline) >= 0;
}

// ---- NIC CR3 window (mirror of proc/syscall.c net_cr3_enter/exit) ----
static inline uint64_t sk_cr3_enter(void) {
    uint64_t saved;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved));
    uint64_t kcr3 = vmm_get_pml4();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    return saved;
}
static inline void sk_cr3_exit(uint64_t saved) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(saved) : "memory");
}


// #69: drain (chunked, own windows) then run the TCP timer in one short window.
// tcp_timer() MUST still run on these paths: it is what retransmits the SYN
// whose first attempt was dropped for an unresolved next-hop MAC, which is the
// only reason a polling connect() ever completes. Dropping it while hoisting
// the drain would have broken connect in a way that looks like a network fault.
static void sk_drain_and_timer(void) {
    net_rx_drain_chunked();
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    tcp_timer();
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SUB_TCPTIMER);
}

// #69: THESE FIVE HOLD NO net_lock. They reach the NIC ring and the TCP
// connection table under nothing but their own `cli`, which on a single core is
// mutual exclusion by construction (nothing else can be running) but is
// INVISIBLE to every net_lock counter the heartbeat reports. holdmax= being
// small was therefore never evidence about these paths; they contributed
// exactly zero to it. IRQWIN_* measures the window that actually exists.
//
// Drive the NIC RX ring + TCP timer once, on the kernel CR3 with interrupts
// off. Bounded (<=64 frames). This is the RX engine of every blocking op.
static void sk_pump(void) {
    net_rx_drain_chunked();          // #69: chunked, outside any window
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    { IRQWIN_SUB_DECL; IRQWIN_SUB_BEGIN(); tcp_timer(); IRQWIN_SUB_END(IRQWIN_SUB_TCPTIMER); }
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SK_PUMP);
}

// STREAM ops that touch the NIC, each wrapped in one CR3 window.
static int sk_tcp_rx(int slot, void *kbuf, uint16_t chunk) {
    net_rx_drain_chunked();          // #69: chunked, outside the window
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    { IRQWIN_SUB_DECL; IRQWIN_SUB_BEGIN(); tcp_timer(); IRQWIN_SUB_END(IRQWIN_SUB_TCPTIMER); }
    int r; { IRQWIN_SUB_DECL; IRQWIN_SUB_BEGIN(); r = tcp_recv(slot, kbuf, chunk); IRQWIN_SUB_END(IRQWIN_SUB_TCPRECV); }
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SK_TCP_RX);
    return r;
}
static int sk_tcp_tx(int slot, const void *kbuf, uint16_t len) {
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    int r = tcp_send(slot, kbuf, len);
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SK_TCP_TX);
    sk_drain_and_timer();            // #69: chunked drain + timer, outside
    return r;
}
static int sk_tcp_conn(int slot, uint32_t hip, uint16_t hport) {
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    int r = tcp_connect(slot, hip, hport);
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SK_TCP_CONN);
    sk_drain_and_timer();            // #69: chunked drain + timer, outside
    return r;
}
static int sk_udp_tx(uint32_t hip, uint16_t sport, uint16_t dport,
                     const void *kbuf, uint16_t len) {
    IRQWIN_DECL;
    IRQWIN_ENTER();
    uint64_t saved = sk_cr3_enter();
    extern int udp_send(uint32_t, uint16_t, uint16_t, const void *, uint16_t);
    int r = udp_send(hip, sport, dport, kbuf, len);
    sk_cr3_exit(saved);
    IRQWIN_EXIT(IRQWIN_SK_UDP_TX);
    return r;
}

void socket_init(void) {
    wait_queue_head_init(&g_net_rx_wq);
    for (int i = 0; i < MAX_SOCKS; i++) g_socks[i].in_use = 0;
    g_sock_inited = 1;
}

// Safe from IRQ / net_lock context: wakes all blocked recv/accept/connect.
void socket_net_wake(void) {
    // wait_queue_head is zero-initialized safe (head==NULL, lock==0), so this
    // is a no-op even if called before socket_init().
    wake_up_all(&g_net_rx_wq);
}

// ---- fd <-> sock resolution ----
static sock_t *sock_from_fd(int fd) {
    file_t *f = fd_get(fd);
    if (!f || f->ops != &g_sock_fops) return 0;
    return (sock_t *)f->priv;
}
static sock_t *sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (!g_socks[i].in_use) {
            sock_t *s = &g_socks[i];
            s->in_use = 1; s->type = 0; s->tcp_slot = -1;
            s->local_port = 0; s->bound = 0;
            s->rcv_timeo_ms = 0; s->snd_timeo_ms = 0; s->so_error = 0;
            s->shut_rd = 0; s->shut_wr = 0; s->fin_sent = 0;
            s->ring = 0; s->r_head = s->r_tail = s->r_count = 0;
            s->file = 0;
            return s;
        }
    }
    return 0;
}

// ---- UDP RX delivery (called from udp_handle under net_lock, or from sk_pump
// under cli). dest_port is host order. Returns 1 if consumed. ----
int socket_udp_input(uint32_t src_ip, uint16_t src_port, uint16_t dest_port,
                     const void *data, uint16_t len) {
    if (len > UDP_DGRAM_MAX) len = UDP_DGRAM_MAX;
    int consumed = 0;
    net_lock();
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t *s = &g_socks[i];
        if (s->in_use && s->type == SOCK_DGRAM && s->bound &&
            s->local_port == dest_port && s->ring) {
            if (s->r_count < UDP_RING_LEN) {
                udp_dgram_t *d = &s->ring[s->r_tail];
                d->src_ip = src_ip;
                d->src_port = src_port;
                d->len = len;
                for (uint16_t k = 0; k < len; k++) d->data[k] = ((const uint8_t *)data)[k];
                s->r_tail = (s->r_tail + 1) % UDP_RING_LEN;
                s->r_count++;
            }
            // else: ring full, tail-drop (standard UDP behavior)
            consumed = 1;
            break;
        }
    }
    net_unlock();
    if (consumed) socket_net_wake();
    return consumed;
}

// Pop one datagram (under net_lock). Returns 1 if one was copied out.
static int udp_ring_pop(sock_t *s, udp_dgram_t *out) {
    int got = 0;
    net_lock();
    if (s->r_count > 0) {
        udp_dgram_t *d = &s->ring[s->r_head];
        out->src_ip = d->src_ip;
        out->src_port = d->src_port;
        out->len = d->len;
        for (uint16_t k = 0; k < d->len; k++) out->data[k] = d->data[k];
        s->r_head = (s->r_head + 1) % UDP_RING_LEN;
        s->r_count--;
        got = 1;
    }
    net_unlock();
    return got;
}

static uint16_t udp_alloc_ephem(void) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = g_udp_ephem++;
        if (g_udp_ephem == 0) g_udp_ephem = 49152;
        int taken = 0;
        for (int i = 0; i < MAX_SOCKS; i++) {
            if (g_socks[i].in_use && g_socks[i].type == SOCK_DGRAM &&
                g_socks[i].bound && g_socks[i].local_port == p) { taken = 1; break; }
        }
        if (!taken) return p;
    }
    return 0;
}

// ---- file_ops ----
static int64_t sock_file_read(struct file *f, void *buf, size_t count) {
    sock_t *s = (sock_t *)f->priv;
    if (!s) return E_BADF;
    // Route through recv with flags derived from O_NONBLOCK. buf is the raw
    // user pointer (sys_read passes it through); recv core does copy_to_user.
    int nb = (f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0;
    // Find the fd of this file to reuse the syscall path is awkward; call the
    // internal core directly by fd is not available here, so replicate via a
    // helper that takes the sock.
    extern int64_t sock_recv_core(sock_t *, void *, uint64_t, int);
    return sock_recv_core(s, buf, count, nb);
}
static int64_t sock_file_write(struct file *f, const void *buf, size_t count) {
    sock_t *s = (sock_t *)f->priv;
    if (!s) return E_BADF;
    int nb = (f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0;
    extern int64_t sock_send_core(sock_t *, const void *, uint64_t, int);
    return sock_send_core(s, buf, count, nb);
}
static struct wait_queue_head *sock_file_poll_wq(struct file *f, int events) {
    (void)f; (void)events;
    return &g_net_rx_wq;
}

static int sock_file_poll(struct file *f, int events) {
    sock_t *s = (sock_t *)f->priv;
    if (!s) return POLL_ERR;
    int re = 0;
    if (s->type == SOCK_STREAM) {
        if (s->tcp_slot >= 0) {
            int p = tcp_rx_pending(s->tcp_slot);   // >0 data, -1 terminal (readable=EOF)
            if (p != 0) re |= POLL_IN;
            if (tcp_get_state(s->tcp_slot) == TCP_STATE_ESTABLISHED) re |= POLL_OUT;
        }
    } else { // DGRAM
        if (s->r_count > 0) re |= POLL_IN;
        re |= POLL_OUT;   // datagram sends are always ready
    }
    return re & (events ? events : (POLL_IN | POLL_OUT));
}
// #695 Phase 2: returns int like every other release, but NOTHING BLOCKING may
// be added here. This runs under cli() with a CR3 switch precisely because the
// release must not sleep, and it is reachable from proc_exit's fd_close_all.
// A socket has no medium to flush, so the answer is always 0.
static int sock_file_release(struct file *f) {
    sock_t *s = (sock_t *)f->priv;
    if (!s) return 0;
    if (s->type == SOCK_STREAM && s->tcp_slot >= 0 && !s->fin_sent) {
        // tcp_close sends a FIN (touches the NIC) - wrap in a CR3 window. It is
        // non-blocking, so this respects "release must never sleep". Skipped when
        // shutdown(SHUT_WR/RDWR) already emitted the FIN (#524 fin_sent guard).
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0" : "=r"(flags));
        __asm__ volatile("cli");
        uint64_t saved = sk_cr3_enter();
        tcp_close(s->tcp_slot);
        for (int i = 0; i < 64; i++) { if (!eth_receive()) break; }
        tcp_timer();
        sk_cr3_exit(saved);
        if (flags & 0x200) __asm__ volatile("sti");
    }
    if (s->ring) { kfree(s->ring); s->ring = 0; }
    s->in_use = 0;
    f->priv = 0;
    return 0;
}
static const file_ops_t g_sock_fops = {
    .read = sock_file_read,
    .write = sock_file_write,
    .seek = 0,
    .ioctl = 0,
    .release = sock_file_release,
    .poll = sock_file_poll,
    // #745 (local 82): poll(2)'s exact wake. g_net_rx_wq is woken by the
    // RX path, which is the same queue sys_sock_select() already sleeps on,
    // so a socket-only poll() gets a real wake rather than a re-scan.
    .poll_wq = sock_file_poll_wq,
};

// =====================================================================
// recv / send cores (shared by the syscall handlers and file_ops)
// =====================================================================
int64_t sock_recv_core(sock_t *s, void *ubuf, uint64_t len, int flags) {
    if (!s) return E_BADF;
    if (len == 0) return 0;
    if (s->type != SOCK_STREAM) return E_INVAL;  // DGRAM uses recvfrom
    if (s->tcp_slot < 0) return E_NOTCONN;
    if (s->shut_rd) return 0;   // #524 shutdown(SHUT_RD): read direction closed => EOF
    int nonblock = (flags & MSG_DONTWAIT) != 0;
    uint64_t deadline = sock_deadline(s->rcv_timeo_ms);
    uint8_t kbuf[2048];
    uint16_t chunk = (len > sizeof(kbuf)) ? (uint16_t)sizeof(kbuf) : (uint16_t)len;
    for (;;) {
        int n = sk_tcp_rx(s->tcp_slot, kbuf, chunk);
        if (n > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)n) != 0) return E_FAULT;
            return n;
        }
        if (n < 0) {
            // TCP_ERR_CLOSED etc: connection ended -> EOF (0) for CLOSED, error otherwise
            return 0; // remote closed: EOF
        }
        // n == 0: connected, no data yet
        if (nonblock) return E_AGAIN;
        if (sock_past(deadline)) return E_AGAIN;
        uint32_t slice = sock_slice_ticks();
        int rc = wait_event_interruptible_timeout(&g_net_rx_wq,
                    tcp_rx_pending(s->tcp_slot) != 0, slice);
        if (rc == WAIT_EINTR) return E_INTR;
    }
}

int64_t sock_send_core(sock_t *s, const void *ubuf, uint64_t len, int flags) {
    if (!s) return E_BADF;
    if (s->type != SOCK_STREAM) return E_INVAL;  // DGRAM uses sendto
    if (s->tcp_slot < 0) return E_NOTCONN;
    if (s->shut_wr) return E_PIPE;  // #524 shutdown(SHUT_WR): write direction closed
    if (len == 0) return 0;
    int nonblock = (flags & MSG_DONTWAIT) != 0;
    uint64_t deadline = sock_deadline(s->snd_timeo_ms);
    uint8_t kbuf[1400];
    uint64_t total = 0;
    while (total < len) {
        uint64_t remain = len - total;
        uint16_t chunk = (remain > sizeof(kbuf)) ? (uint16_t)sizeof(kbuf) : (uint16_t)remain;
        if (copy_from_user(kbuf, (const uint8_t *)ubuf + total, chunk) != 0)
            return total ? (int64_t)total : E_FAULT;
        // Push this chunk; tcp_send accepts up to what its send buffer holds.
        uint16_t pushed = 0;
        for (;;) {
            int r = sk_tcp_tx(s->tcp_slot, kbuf + pushed, (uint16_t)(chunk - pushed));
            if (r > 0) {
                pushed += (uint16_t)r;
                total += (uint16_t)r;
                if (pushed >= chunk) break;
                continue;
            }
            if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
                return total ? (int64_t)total : E_NOTCONN;
            }
            // WOULD_BLOCK / 0: send buffer full - drain acks and wait.
            if (nonblock) return total ? (int64_t)total : E_AGAIN;
            if (sock_past(deadline)) return total ? (int64_t)total : E_AGAIN;
            uint32_t slice = sock_slice_ticks();
            int rc = wait_event_interruptible_timeout(&g_net_rx_wq,
                        tcp_get_state(s->tcp_slot) != TCP_STATE_ESTABLISHED, slice);
            if (rc == WAIT_EINTR) return total ? (int64_t)total : E_INTR;
            // even on timeout, loop: sk_tcp_tx re-drains acks and retries.
        }
    }
    return (int64_t)total;
}

// =====================================================================
// syscall handlers
// =====================================================================
int64_t sys_sock_open(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return E_AFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return E_PROTO;
    sock_t *s = sock_alloc();
    if (!s) return E_MFILE;
    s->type = type;
    if (type == SOCK_STREAM) {
        int slot = tcp_socket();
        if (slot < 0) { s->in_use = 0; return E_MFILE; }
        s->tcp_slot = slot;
    } else {
        s->ring = (udp_dgram_t *)kmalloc(sizeof(udp_dgram_t) * UDP_RING_LEN);
        if (!s->ring) { s->in_use = 0; return E_NOMEM; }
    }
    file_t *f = file_alloc(&g_sock_fops, s, O_RDWR);
    // #120: name the description. Sockets were the ONE fd family that recorded
    // no path at all, so fstat() could not tell a socket from an anonymous
    // description and Task Manager (#487) showed a bare fd number for it.
    if (f) file_set_path(f, (s->type == SOCK_STREAM) ? "socket:[tcp]" : "socket:[udp]");
    if (!f) {
        if (s->ring) { kfree(s->ring); s->ring = 0; }
        if (s->tcp_slot >= 0) tcp_close(s->tcp_slot);
        s->in_use = 0;
        return E_MFILE;
    }
    s->file = f;
    int fd = fd_alloc_install(f);
    if (fd < 0) {
        IGNORE_RESULT("sock_file_release frees sock/ring/tcp and performs no persistent I/O; it cannot fail (#695)",
                      file_put(f));
        return E_MFILE;
    }
    return fd;
}

// Copy a user sockaddr_in into host-order (ip_host, port_host). Returns 0 or err.
static int copy_sockaddr_in(const void *uaddr, int addrlen,
                            uint32_t *ip_host, uint16_t *port_host) {
    if (addrlen < (int)sizeof(k_sockaddr_in_t)) return E_INVAL;
    k_sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return E_FAULT;
    if (sa.sin_family != AF_INET) return E_AFNOSUPPORT;
    *ip_host = sk_ntohl(sa.sin_addr);
    *port_host = sk_ntohs(sa.sin_port);
    return 0;
}

int64_t sys_sock_bind(int fd, const void *uaddr, int addrlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    uint32_t ip; uint16_t port;
    int e = copy_sockaddr_in(uaddr, addrlen, &ip, &port);
    if (e) return e;
    if (s->type == SOCK_STREAM) {
        if (tcp_bind(s->tcp_slot, port) < 0) return E_INVAL;
        s->local_port = port;
        return 0;
    } else {
        // DGRAM: register local_port for RX routing (socket_udp_input scans us).
        for (int i = 0; i < MAX_SOCKS; i++) {
            if (g_socks[i].in_use && &g_socks[i] != s &&
                g_socks[i].type == SOCK_DGRAM && g_socks[i].bound &&
                g_socks[i].local_port == port) return E_INVAL; // in use
        }
        s->local_port = port;
        s->bound = 1;
        return 0;
    }
}

int64_t sys_sock_connect(int fd, const void *uaddr, int addrlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    uint32_t ip; uint16_t port;
    int e = copy_sockaddr_in(uaddr, addrlen, &ip, &port);
    if (e) return e;
    if (s->type == SOCK_DGRAM) {
        // Connected UDP: just record the peer as default dest via bind of ephemeral.
        if (!s->bound) { s->local_port = udp_alloc_ephem(); s->bound = (s->local_port != 0); }
        return 0;
    }
    if (s->tcp_slot < 0) return E_BADF;
    tcp_state_t st = tcp_get_state(s->tcp_slot);
    if (st == TCP_STATE_ESTABLISHED) return E_ISCONN;
    int nonblock = (s->file && (s->file->flags & O_NONBLOCK));
    int r = sk_tcp_conn(s->tcp_slot, ip, port);
    if (r < 0 && r != TCP_ERR_IN_PROGRESS) return E_CONNREFUSED;
    if (nonblock) return E_INPROGRESS;
    // Blocking: pump RX (retransmits SYN once ARP resolves) until ESTABLISHED.
    uint32_t timeo_ms = s->snd_timeo_ms ? s->snd_timeo_ms : 15000u;   // #499: REAL ms
    uint64_t deadline = sock_deadline(timeo_ms);
    for (;;) {
        st = tcp_get_state(s->tcp_slot);
        if (st == TCP_STATE_ESTABLISHED) return 0;
        if (st == TCP_STATE_CLOSED) return E_CONNREFUSED;
        if (sock_past(deadline)) return E_TIMEDOUT;
        sk_pump();
        st = tcp_get_state(s->tcp_slot);
        if (st == TCP_STATE_ESTABLISHED) return 0;
        if (st == TCP_STATE_CLOSED) return E_CONNREFUSED;
        uint32_t slice = sock_slice_ticks();
        int rc = wait_event_interruptible_timeout(&g_net_rx_wq,
                    tcp_get_state(s->tcp_slot) == TCP_STATE_ESTABLISHED ||
                    tcp_get_state(s->tcp_slot) == TCP_STATE_CLOSED, slice);
        if (rc == WAIT_EINTR) return E_INTR;
    }
}

int64_t sys_sock_listen(int fd, int backlog) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (s->type != SOCK_STREAM) return E_INVAL;
    if (backlog <= 0) backlog = 1;
    if (tcp_listen(s->tcp_slot, backlog) < 0) return E_INVAL;
    return 0;
}

int64_t sys_sock_accept(int fd, void *uaddr, void *uaddrlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (s->type != SOCK_STREAM) return E_INVAL;
    int nonblock = (s->file && (s->file->flags & O_NONBLOCK));
    uint64_t deadline = sock_deadline(s->rcv_timeo_ms);
    int newslot = -1;
    for (;;) {
        sk_pump();
        newslot = tcp_accept(s->tcp_slot);
        if (newslot >= 0) break;
        if (newslot != TCP_ERR_WOULD_BLOCK) return E_INVAL;
        if (nonblock) return E_AGAIN;
        if (sock_past(deadline)) return E_AGAIN;
        uint32_t slice = sock_slice_ticks();
        int rc = wait_event_interruptible_timeout(&g_net_rx_wq, 0, slice);
        if (rc == WAIT_EINTR) return E_INTR;
    }
    // Wrap the accepted TCB slot in a fresh socket fd.
    sock_t *ns = sock_alloc();
    if (!ns) { tcp_close(newslot); return E_MFILE; }
    ns->type = SOCK_STREAM;
    ns->tcp_slot = newslot;
    file_t *f = file_alloc(&g_sock_fops, ns, O_RDWR);
    if (f) file_set_path(f, "socket:[tcp]");   // #120: accepted connection
    if (!f) { ns->in_use = 0; tcp_close(newslot); return E_MFILE; }
    ns->file = f;
    int nfd = fd_alloc_install(f);
    if (nfd < 0) {
            IGNORE_RESULT("sock_file_release runs cli + CR3-switched and is "
                          "non-blocking by construction; it cannot fail (#695)",
                          file_put(f));
            return E_MFILE;
        }
    // Fill peer address if the caller wants it. #524: tcp_get_peer() reads the
    // accepted TCB's remote endpoint (HOST byte order); convert to network order
    // for the returned sockaddr_in so sin_addr/sin_port match the real peer.
    if (uaddr && uaddrlen) {
        int cap = 0;
        if (copy_from_user(&cap, uaddrlen, sizeof(int)) == 0 &&
            cap >= (int)sizeof(k_sockaddr_in_t)) {
            uint32_t pip = 0; uint16_t pport = 0;
            tcp_get_peer(newslot, &pip, &pport);   // host order
            k_sockaddr_in_t sa;
            sa.sin_family = AF_INET;
            sa.sin_port = sk_htons(pport);
            sa.sin_addr = sk_htonl(pip);
            for (int i = 0; i < 8; i++) sa.sin_zero[i] = 0;
            int wl = (int)sizeof(k_sockaddr_in_t);
            copy_to_user(uaddr, &sa, sizeof(sa));
            copy_to_user(uaddrlen, &wl, sizeof(int));
        }
    }
    return nfd;
}

int64_t sys_sock_send(int fd, const void *ubuf, uint64_t len, int flags) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    return sock_send_core(s, ubuf, len, flags);
}
int64_t sys_sock_recv(int fd, void *ubuf, uint64_t len, int flags) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    return sock_recv_core(s, ubuf, len, flags);
}

int64_t sys_sock_sendto(int fd, const void *ubuf, uint64_t len, int flags,
                        const void *uaddr, int addrlen) {
    (void)flags;
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (s->type != SOCK_DGRAM) return E_INVAL;
    if (len > UDP_DGRAM_MAX) return E_MSGSIZE;
    uint32_t ip; uint16_t port;
    int e = copy_sockaddr_in(uaddr, addrlen, &ip, &port);
    if (e) return e;
    if (!s->bound) {
        s->local_port = udp_alloc_ephem();
        s->bound = (s->local_port != 0);
        if (!s->bound) return E_AGAIN;
    }
    uint8_t kbuf[UDP_DGRAM_MAX];
    if (len > 0 && copy_from_user(kbuf, ubuf, (size_t)len) != 0) return E_FAULT;
    int r = sk_udp_tx(ip, s->local_port, port, kbuf, (uint16_t)len);
    if (r < 0) return E_INVAL;
    return (int64_t)len;
}

int64_t sys_sock_recvfrom(int fd, void *ubuf, uint64_t len, int flags,
                          void *uaddr, void *uaddrlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (s->type != SOCK_DGRAM) return E_INVAL;
    if (!s->bound) return E_INVAL;   // must bind() before recvfrom()
    int nonblock = (flags & MSG_DONTWAIT) ||
                   (s->file && (s->file->flags & O_NONBLOCK));
    uint64_t deadline = sock_deadline(s->rcv_timeo_ms);
    udp_dgram_t dg;   // per-call (on stack) so concurrent recvfrom()s do not race
    for (;;) {
        sk_pump();
        if (udp_ring_pop(s, &dg)) {
            uint16_t n = dg.len;
            if (n > len) n = (uint16_t)len;
            if (n > 0 && copy_to_user(ubuf, dg.data, n) != 0) return E_FAULT;
            if (uaddr && uaddrlen) {
                int cap = 0;
                if (copy_from_user(&cap, uaddrlen, sizeof(int)) == 0 &&
                    cap >= (int)sizeof(k_sockaddr_in_t)) {
                    k_sockaddr_in_t sa;
                    sa.sin_family = AF_INET;
                    sa.sin_port = sk_htons(dg.src_port);
                    sa.sin_addr = dg.src_ip;   // already network order
                    for (int i = 0; i < 8; i++) sa.sin_zero[i] = 0;
                    int wl = (int)sizeof(k_sockaddr_in_t);
                    copy_to_user(uaddr, &sa, sizeof(sa));
                    copy_to_user(uaddrlen, &wl, sizeof(int));
                }
            }
            return n;
        }
        if (nonblock) return E_AGAIN;
        if (sock_past(deadline)) return E_AGAIN;
        uint32_t slice = sock_slice_ticks();
        int rc = wait_event_interruptible_timeout(&g_net_rx_wq, s->r_count > 0, slice);
        if (rc == WAIT_EINTR) return E_INTR;
    }
}

int64_t sys_sock_setsockopt(int fd, int level, int optname,
                            const void *uoptval, int optlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (level != SOL_SOCKET) return 0;   // ignore unknown levels
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
        if (optlen < (int)sizeof(k_timeval_t)) return E_INVAL;
        k_timeval_t tv;
        if (copy_from_user(&tv, uoptval, sizeof(tv)) != 0) return E_FAULT;
        // #499: stored as REAL milliseconds, not ticks.
        uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
        // A nonzero sub-millisecond timeout must not round to 0, which means
        // "block forever" here; clamp it to 1ms.
        if (ms == 0 && (tv.tv_sec || tv.tv_usec)) ms = 1;
        uint32_t t = (ms > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)ms;
        if (optname == SO_RCVTIMEO) s->rcv_timeo_ms = t; else s->snd_timeo_ms = t;
        return 0;
    }
    return 0; // SO_REUSEADDR etc: accept as no-op
}

int64_t sys_sock_getsockopt(int fd, int level, int optname,
                            void *uoptval, void *uoptlen) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (level != SOL_SOCKET) return E_INVAL;
    int cap = 0;
    if (copy_from_user(&cap, uoptlen, sizeof(int)) != 0) return E_FAULT;
    if (optname == SO_ERROR) {
        int v = s->so_error; s->so_error = 0;
        if (cap < (int)sizeof(int)) return E_INVAL;
        if (copy_to_user(uoptval, &v, sizeof(int)) != 0) return E_FAULT;
        int wl = sizeof(int); copy_to_user(uoptlen, &wl, sizeof(int));
        return 0;
    }
    if (optname == SO_TYPE) {
        int v = s->type;
        if (cap < (int)sizeof(int)) return E_INVAL;
        if (copy_to_user(uoptval, &v, sizeof(int)) != 0) return E_FAULT;
        int wl = sizeof(int); copy_to_user(uoptlen, &wl, sizeof(int));
        return 0;
    }
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
        // #524: round-trip the timeout set by setsockopt back into a timeval.
        // #499: stored internally as REAL milliseconds; reconstruct sec/usec
        // (sub-millisecond precision is lost, which matches how it is applied
        // to the wait deadline).
        if (cap < (int)sizeof(k_timeval_t)) return E_INVAL;
        uint32_t t = (optname == SO_RCVTIMEO) ? s->rcv_timeo_ms : s->snd_timeo_ms;
        k_timeval_t tv;
        tv.tv_sec  = (int64_t)(t / 1000u);
        tv.tv_usec = (int64_t)((uint64_t)(t % 1000u) * 1000ULL);
        if (copy_to_user(uoptval, &tv, sizeof(tv)) != 0) return E_FAULT;
        int wl = (int)sizeof(k_timeval_t); copy_to_user(uoptlen, &wl, sizeof(int));
        return 0;
    }
    return E_INVAL;
}

// select() over up to MAX_FDS fds using file_poll(). fd_set is a single 64-bit
// mask (MayteraOS caps a process at 64 fds).
static int sock_scan_ready(uint64_t rin, uint64_t win, int nfds,
                           uint64_t *rout, uint64_t *wout) {
    uint64_t r = 0, w = 0; int cnt = 0;
    for (int fd = 0; fd < nfds && fd < 64; fd++) {
        uint64_t bit = 1ULL << fd;
        if (rin & bit) {
            file_t *f = fd_get(fd);
            if (f) { int pe = file_poll(f, POLL_IN); if (pe & POLL_IN) { r |= bit; cnt++; } }
        }
        if (win & bit) {
            file_t *f = fd_get(fd);
            if (f) { int pe = file_poll(f, POLL_OUT); if (pe & POLL_OUT) { w |= bit; cnt++; } }
        }
    }
    *rout = r; *wout = w; return cnt;
}

int64_t sys_sock_select(int nfds, void *ureadfds, void *uwritefds,
                        void *uexceptfds, void *utimeout) {
    (void)uexceptfds;
    if (nfds < 0) return E_INVAL;
    if (nfds > 64) nfds = 64;
    uint64_t rin = 0, win = 0;
    k_fd_set_t fs;
    if (ureadfds) { if (copy_from_user(&fs, ureadfds, sizeof(fs)) != 0) return E_FAULT; rin = fs.bits; }
    if (uwritefds){ if (copy_from_user(&fs, uwritefds, sizeof(fs)) != 0) return E_FAULT; win = fs.bits; }
    uint64_t deadline = WAIT_DEADLINE_NEVER;
    if (utimeout) {
        k_timeval_t tv;
        if (copy_from_user(&tv, utimeout, sizeof(tv)) != 0) return E_FAULT;
        // #499: REAL ms, not ticks (see sock_deadline).
        uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
        deadline = sched_now_ms() + ms;   // {0,0} => immediate poll (deadline==now)
    }
    for (;;) {
        sk_pump();   // drive RX so socket readiness reflects the wire
        uint64_t rout = 0, wout = 0;
        int cnt = sock_scan_ready(rin, win, nfds, &rout, &wout);
        if (cnt > 0 || sock_past(deadline) ||
            (utimeout && deadline != WAIT_DEADLINE_NEVER &&
             (int64_t)(sched_now_ms() - deadline) >= 0)) {
            if (ureadfds)  { fs.bits = rout; copy_to_user(ureadfds, &fs, sizeof(fs)); }
            if (uwritefds) { fs.bits = wout; copy_to_user(uwritefds, &fs, sizeof(fs)); }
            if (uexceptfds){ fs.bits = 0;    copy_to_user(uexceptfds, &fs, sizeof(fs)); }
            return cnt;
        }
        uint32_t slice = sock_slice_ticks();
        int rc = wait_event_interruptible_timeout(&g_net_rx_wq,
                    sock_scan_ready(rin, win, nfds, &rout, &wout) > 0, slice);
        if (rc == WAIT_EINTR) return E_INTR;
    }
}

int64_t sys_sock_shutdown(int fd, int how) {
    sock_t *s = sock_from_fd(fd);
    if (!s) return E_BADF;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return E_INVAL;
    // #524: honor the direction. SHUT_RD closes the read half (recv -> EOF),
    // SHUT_WR closes the write half (send -> E_PIPE) and emits a FIN so the peer
    // sees end-of-stream, SHUT_RDWR does both. The stack has no independent
    // half-close, so the FIN uses tcp_close(); fin_sent guards against a second
    // FIN from sock_file_release when the fd is later close()d.
    if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = 1;
    if (how == SHUT_WR || how == SHUT_RDWR) s->shut_wr = 1;
    if ((how == SHUT_WR || how == SHUT_RDWR) &&
        s->type == SOCK_STREAM && s->tcp_slot >= 0 && !s->fin_sent) {
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0" : "=r"(flags));
        __asm__ volatile("cli");
        uint64_t saved = sk_cr3_enter();
        tcp_close(s->tcp_slot);
        for (int i = 0; i < 64; i++) { if (!eth_receive()) break; }
        tcp_timer();
        sk_cr3_exit(saved);
        if (flags & 0x200) __asm__ volatile("sti");
        s->fin_sent = 1;
    }
    return 0;
}

