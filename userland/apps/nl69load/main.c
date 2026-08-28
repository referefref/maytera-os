// nl69load - #69 load generator for the network stack's interrupts-off windows.
//
// WHY A DEDICATED APP. The sixteen sites that run their own `cli` to switch CR3
// for the NIC rings are reached ONLY from userland network syscalls. An idle
// desktop enters none of them: measured on golden 1963, [NETIRQ] reads
// "none-entered" indefinitely while [NETSTARVE] still reports net_lock holds,
// because net_poll() (the netpump thread) is the only network context running.
// So the interesting window cannot be observed without a userland driver, and
// the two existing candidates do not do the job: socktest handles exactly one
// connection and one line then exits, and curl goes through the kernel's own
// HTTP fetcher (net_lock, already measured by holdmax=), not through these.
//
// WHAT IT DRIVES. MayteraOS has TWO independent userland TCP surfaces and they
// land on DIFFERENT kernel code, so a driver that exercises one proves nothing
// about the other:
//
//   legacy  SYS_SOCKET/SYS_CONNECT/SYS_SEND/SYS_RECV/SYS_TCP_STATE
//           -> proc/syscall.c  tcp_*_kcr3()      (cli + CR3 + net_lock inside)
//   BSD     socket()/connect()/send()/recv()  (sys/socket.h)
//           -> net/socket.c    sk_tcp_*()       (cli + CR3, NO net_lock at all)
//
// This app runs both, in that order, in a loop, against an external echo/source
// server. Bulk receive is the interesting direction: it is what fills the RX
// ring and makes the `for (i < 64) eth_receive()` inside each window actually
// run 64 times instead of 1.
//
// KEPT SMALL PER TRANSFER ON PURPOSE. This kernel's TCP_SEND_BUFFER_SIZE is
// 4096 and sustained streams are known to tear down after ~8-14 MB, so the
// point here is MANY windows, not one huge stream: a max is what is being
// measured, and a torn-down connection would just stop generating windows.
#include "syscall.h"
#include "sys/socket.h"
#include "errno.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define SYS_BOOTLOG_WRITE 298

// Guard each octet SEPARATELY. A single `#ifndef PEER_A` around all four lets a
// command-line -DPEER_D=... be silently overridden by the in-source default
// (gcc warns "previous definition" and carries on), which cost one full
// build-deploy-boot cycle here: the app kept dialling the old peer and the
// rebuilt binary was byte-identical to the one it replaced.
#ifndef PEER_A
#define PEER_A 192
#endif
#ifndef PEER_B
#define PEER_B 168
#endif
#ifndef PEER_C
#define PEER_C 1
#endif
#ifndef PEER_D
#define PEER_D 112
#endif
#ifndef SRC_PORT
#define SRC_PORT 9701      // server streams bulk data at us, then closes
#endif

static char lbuf[512];
static void logline(const char *s) { syscall1(SYS_BOOTLOG_WRITE, (long)s); }
#define LOG(...) do { snprintf(lbuf, sizeof lbuf, __VA_ARGS__); logline(lbuf); } while (0)

static unsigned char buf[8192];

static unsigned int peer_ip(void) {
    return ((unsigned int)PEER_A << 24) | ((unsigned int)PEER_B << 16) |
           ((unsigned int)PEER_C << 8)  | (unsigned int)PEER_D;
}

// ---------------------------------------------------------------------------
// LEGACY family -> proc/syscall.c tcp_connect_kcr3 / tcp_send_kcr3 /
// tcp_recv_kcr3 / tcp_state_kcr3 / tcp_close_kcr3.
// ---------------------------------------------------------------------------
static long run_legacy(void) {
    int s = tcp_socket();
    if (s < 0) { LOG("NL69: legacy tcp_socket rc=%d", s); return -1; }
    int rc = tcp_connect(s, peer_ip(), SRC_PORT);
    // tcp_connect is non-blocking here: poll the state, which is itself one of
    // the measured windows (tcp_state_kcr3 runs a full 64-frame drain).
    for (int i = 0; i < 4000; i++) {
        int st = tcp_get_state(s);
        if (st == TCP_STATE_ESTABLISHED) break;
        if (st == TCP_STATE_CLOSED && i > 200) break;
        usleep(1000);
    }
    if (tcp_get_state(s) != TCP_STATE_ESTABLISHED) {
        LOG("NL69: legacy connect failed rc=%d state=%d", rc, tcp_get_state(s));
        tcp_close(s); return -1;
    }
    const char *req = "GO\n";
    tcp_send(s, req, 3);
    long total = 0;
    for (int i = 0; i < 20000; i++) {
        int n = tcp_recv(s, buf, (int)sizeof buf);
        if (n > 0) { total += n; continue; }
        if (tcp_get_state(s) != TCP_STATE_ESTABLISHED) break;
        usleep(1000);
    }
    tcp_close(s);
    return total;
}

// ---------------------------------------------------------------------------
// BSD family -> net/socket.c sk_tcp_conn / sk_tcp_tx / sk_tcp_rx / sk_pump.
// These are the five sites that hold NO net_lock.
// ---------------------------------------------------------------------------
static long run_bsd(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG("NL69: bsd socket rc=%d errno=%d", fd, errno); return -1; }
    struct timeval tv; tv.tv_sec = 15; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SRC_PORT);
    sa.sin_addr.s_addr = htonl(peer_ip());
    errno = 0;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        LOG("NL69: bsd connect failed errno=%d", errno);
        close(fd); return -1;
    }
    send(fd, "GO\n", 3, 0);
    long total = 0;
    for (;;) {
        long n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) { total += n; continue; }
        break;
    }
    close(fd);
    return total;
}

int main(void) {
    LOG("NL69: load generator start, peer=%u.%u.%u.%u:%u",
        PEER_A, PEER_B, PEER_C, PEER_D, SRC_PORT);
    // Let DHCP settle. net_is_up is not exposed here; the transfers themselves
    // report failure loudly, which is the signal we want on the serial log.
    for (int i = 0; i < 10; i++) usleep(500000);

    for (int round = 1; ; round++) {
        long a = run_legacy();
        long b = run_bsd();
        LOG("NL69: round=%d legacy=%ld bsd=%ld", round, a, b);
        usleep(200000);
    }
    return 0;
}
