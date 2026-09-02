// nslookup - name resolution diagnostic for MayteraOS
//
// WHY THIS EXISTS (browsenet, 2026-09-01). The owner reported "I can ping
// 1.1.1.1 but the browser cannot load example.com", and then: "there's no
// nslookup", "there's no dig". So the one question that would have split the
// search space in half - does this machine resolve names? - could not be asked
// on the machine itself. Three separate questions this week were unanswerable
// because the only instrument printed to a serial port the owner's laptop does
// not have. This one prints to the terminal.
//
// It deliberately reports MORE than the answer: which resolver was queried,
// where that resolver came from (DHCP option 6 or an explicit/static choice),
// the link and lease state, and whether the #549 connectivity breaker has
// marked the interface FAULTY. The FAULTY case is the one that looks exactly
// like a DNS fault from the outside: ICMP is not gated so ping works, while
// every HTTP fetch is refused, so the browser and the weather widget both fail
// while the network appears healthy.
//
// Usage: nslookup [name ...]
//        nslookup            (no args: report resolver + interface state only)
#include "stdio.h"
#include "string.h"
#include "syscall.h"

static void put_ip(const char *label, unsigned int ip) {
    if (ip == 0) { printf("%s: (not configured)\n", label); return; }
    printf("%s: %u.%u.%u.%u\n", label,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

static const char *dhcp_name(unsigned int st) {
    return st == NET_DHCP_BOUND       ? "BOUND"       :
           st == NET_DHCP_REQUESTING  ? "REQUESTING"  :
           st == NET_DHCP_DISCOVERING ? "DISCOVERING" : "IDLE";
}

static void report_state(const net_status_t *ns) {
    printf("Interface:\n");
    printf("  driver present : %s\n", ns->driver ? "yes" : "NO NIC AT ALL");
    printf("  carrier        : %s\n", ns->link_up ? "up" : "DOWN");
    put_ip("  address       ", ns->ip);
    put_ip("  gateway       ", ns->gateway);
    printf("  config source  : %s\n", ns->config_static ? "static (/CONFIG/NETIP.CFG)" : "DHCP");
    printf("  dhcp state     : %s\n", dhcp_name(ns->dhcp_state));
    put_ip("  resolver      ", ns->dns_active);
    put_ip("  dhcp offered  ", ns->dns_dhcp);
    if (ns->dns_active && ns->dns_dhcp && ns->dns_active != ns->dns_dhcp)
        printf("  NOTE: the active resolver is NOT the one DHCP offered "
               "(an explicit choice is pinned).\n");
    if (ns->faulty) {
        printf("\n  *** CONNECTIVITY BREAKER: this interface is marked FAULTY. ***\n");
        printf("  Six consecutive transport failures tripped it. While FAULTY,\n");
        printf("  HTTP/HTTPS fetches are REFUSED (NET_ERR_FAULTY) apart from one\n");
        printf("  re-probe every 30 seconds; ICMP and DNS are NOT gated, which is\n");
        printf("  why ping and this tool can still work while the browser and the\n");
        printf("  weather widget cannot. A completed transfer, a DHCP renew, a\n");
        printf("  carrier bounce, or Settings > Network applying a config clears it.\n");
    }
}

// See ping's note: the kernel exposes only the poll-split resolver, so the wait
// lives in Ring 3 by design. 6s budget at 50ms granularity.
static int resolve_host(const char *host, unsigned int *out) {
    unsigned int ip = 0;
    int rc = dns_start(host, &ip);
    if (rc == 1) { *out = ip; return 0; }
    if (rc < 0) return -1;
    for (int i = 0; i < 120; i++) {
        sys_sleep(50);
        rc = dns_poll(&ip);
        if (rc == 1) { *out = ip; return 0; }
        if (rc < 0) return -1;
    }
    return -2;
}

int main(int argc, char **argv) {
    net_status_t ns;
    int have_ns = (sys_net_status(&ns) == 0);

    if (argc < 2) {
        if (have_ns) report_state(&ns);
        else printf("nslookup: SYS_NET_STATUS failed\n");
        printf("\nusage: nslookup <name> [name ...]\n");
        return have_ns ? 0 : 1;
    }

    if (have_ns) {
        put_ip("Server", ns.dns_active);
        if (ns.dns_active == 0) {
            printf("nslookup: NO RESOLVER CONFIGURED. Nothing can be looked up.\n");
            report_state(&ns);
            return 1;
        }
        printf("\n");
    }

    int bad = 0;
    for (int a = 1; a < argc; a++) {
        unsigned int ip = 0;
        int rc = resolve_host(argv[a], &ip);
        if (rc == 0 && ip) {
            printf("Name:    %s\n", argv[a]);
            printf("Address: %u.%u.%u.%u\n\n",
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                   (ip >> 8) & 0xFF, ip & 0xFF);
        } else {
            bad = 1;
            printf("** server can't find %s: %s\n\n", argv[a],
                   rc == -2 ? "no answer within 6s"
                            : "lookup failed (NXDOMAIN, or negatively cached)");
        }
    }
    if (bad && have_ns) report_state(&ns);
    return bad;
}
