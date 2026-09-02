// ping - ICMP echo utility for MayteraOS
// Usage: ping [host]
//   host: dotted-quad IPv4 address (e.g. 8.8.8.8) OR a hostname
//   (e.g. example.com). If omitted, pings the configured default gateway.
#include "stdio.h"
#include "string.h"
#include "syscall.h"

// Parse "a.b.c.d" into a big-endian numeric IP: (a<<24)|(b<<16)|(c<<8)|d.
// Returns 0 on success, -1 on malformed input.
static int parse_ip(const char *s, unsigned int *out) {
    unsigned int octets[4];
    int oi = 0;
    int have_digit = 0;
    unsigned int val = 0;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (unsigned int)(c - '0');
            if (val > 255) return -1;
            have_digit = 1;
        } else if (c == '.' || c == '\0') {
            if (!have_digit || oi >= 4) return -1;
            octets[oi++] = val;
            val = 0;
            have_digit = 0;
            if (c == '\0') break;
        } else {
            return -1;
        }
    }
    if (oi != 4) return -1;
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}


// ---------------------------------------------------------------------------
// browsenet 2026-09-01: ping accepts a HOSTNAME.
//
// It never did, and that is how the owner found the fault: "ping can't do
// domains". The resolver existed in the kernel and was reachable from Ring 3
// the whole time (SYS_DNS_START 215 / SYS_DNS_POLL 216, wrapped by libc as
// dns_start()/dns_poll()); nothing in the tree had ever called it from a
// command-line tool, so there was no way to ask the machine a DNS question.
//
// The kernel exposes ONLY the poll-split resolver: there is no blocking
// resolve syscall to hand the wait to. A userland sleep-poll is therefore the
// interface, not a hand-rolled substitute for one; classicube's
// Socket_Maytera.c carries the same note. CLAUDE.md's ban on poll loops is a
// KERNEL rule (wait_event/futex), and this is Ring 3.
static int resolve_host(const char *host, unsigned int *out) {
    unsigned int ip = 0;
    int rc = dns_start(host, &ip);
    if (rc == 1) { *out = ip; return 0; }
    if (rc < 0) return -1;
    // Query is on the wire. DNS_TIMEOUT_MS * retries in the kernel is several
    // seconds; give it 6s at 50ms granularity, then give up rather than hang.
    for (int i = 0; i < 120; i++) {
        sys_sleep(50);
        rc = dns_poll(&ip);
        if (rc == 1) { *out = ip; return 0; }
        if (rc < 0) return -1;
    }
    return -2;   // still pending after 6s
}

// Print the connectivity breaker's state when a lookup or a ping fails. On the
// owner's machine every HTTP request was being refused by NET_FAULTY while the
// link, the lease, the gateway ARP and the resolver were all healthy, and no
// tool said so. A diagnostic that can only report success is not a diagnostic.
static void print_net_state(void) {
    net_status_t ns;
    if (sys_net_status(&ns) != 0) return;
    if (ns.faulty) {
        printf("note: the connectivity breaker has marked this interface FAULTY.\n");
        printf("      ICMP is not gated, but HTTP/HTTPS fetches are refused\n");
        printf("      (NET_ERR_FAULTY) except one re-probe every 30s.\n");
        printf("      Settings > Network > reconnect, or a DHCP renew, clears it.\n");
    }
    if (!ns.link_up) printf("note: no carrier on the interface.\n");
    if (ns.ip == 0)  printf("note: no IPv4 address configured.\n");
}

int main(int argc, char **argv) {
    unsigned int ip = 0;
    const char *target;
    char gw[16];

    if (argc >= 2) {
        target = argv[1];
        if (parse_ip(target, &ip) != 0) {
            // Not a dotted quad: treat it as a hostname and resolve it.
            int rc = resolve_host(target, &ip);
            if (rc != 0 || ip == 0) {
                printf("ping: cannot resolve '%s' (%s)\n", target,
                       rc == -2 ? "no answer from the resolver"
                                : "lookup failed");
                print_net_state();
                return 1;
            }
            printf("ping: %s resolved to %u.%u.%u.%u\n", target,
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                   (ip >> 8) & 0xFF, ip & 0xFF);
        }
    } else {
        net_info_t ni;
        if (get_net_info(&ni, sizeof(ni)) < 0 || ni.gateway[0] == '\0') {
            printf("ping: no host given and no default gateway\n");
            return 1;
        }
        strcpy(gw, ni.gateway);
        target = gw;
        if (parse_ip(target, &ip) != 0) {
            printf("ping: bad gateway address '%s'\n", target);
            return 1;
        }
    }

    printf("PING %s\n", target);
    int sent = 0, recvd = 0;
    for (int i = 0; i < 4; i++) {
        int rtt = sys_ping(ip, 1000);
        sent++;
        if (rtt >= 0) {
            recvd++;
            printf("reply from %s: seq=%d time=%dms\n", target, i, rtt);
        } else {
            printf("request timed out: seq=%d\n", i);
        }
    }
    printf("--- %s statistics ---\n", target);
    printf("%d sent, %d received, %d%% loss\n",
           sent, recvd, sent ? (sent - recvd) * 100 / sent : 0);
    if (!recvd) print_net_state();
    return recvd ? 0 : 1;
}
