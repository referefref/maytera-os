// ping - ICMP echo utility for MayteraOS
// Usage: ping [host]
//   host: dotted-quad IPv4 address (e.g. 8.8.8.8). If omitted, pings the
//   configured default gateway.
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

int main(int argc, char **argv) {
    unsigned int ip = 0;
    const char *target;
    char gw[16];

    if (argc >= 2) {
        target = argv[1];
        if (parse_ip(target, &ip) != 0) {
            printf("ping: invalid address '%s' (expected a.b.c.d)\n", target);
            return 1;
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
    return recvd ? 0 : 1;
}
