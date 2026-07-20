// nc - minimal netcat: open a TCP connection, optionally send a payload,
// print whatever comes back, then close.
//   nc <host-ip> <port> [payload words...]
// Host must be a numeric IPv4 literal (no DNS in userland yet).
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "syscall.h"

// usleep lives in unistd.h, which clashes with syscall.h (dup/dup2/pipe).
// Networked apps include syscall.h, so declare the one extra symbol we need.
extern int usleep(unsigned long us);

#define TCP_STATE_CLOSED       0
#define TCP_STATE_ESTABLISHED  4
#define TCP_ERR_IN_PROGRESS   (-8)

// Parse "a.b.c.d" into network-order int (first octet in MSB). 0 on error.
static unsigned int parse_ip(const char *s) {
    unsigned int parts[4] = {0,0,0,0};
    int idx = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (unsigned)(*p - '0');
            digits++;
            if (parts[idx] > 255) return 0;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0) return 0;
            digits = 0;
            if (*p == '\0') {
                if (idx != 3) return 0;
                return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
            }
            if (++idx > 3) return 0;
        } else {
            return 0;
        }
    }
}

static void out_all(const char *buf, int n) {
    int off = 0;
    while (off < n) {
        long w = write(1, buf + off, n - off);
        if (w <= 0) break;
        off += w;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: nc <host-ip> <port> [payload...]\n", stderr);
        return 1;
    }
    unsigned int ip = parse_ip(argv[1]);
    if (ip == 0) {
        // Not a numeric IP: resolve via the kernel DNS poll API.
        int dr = dns_start(argv[1], &ip);
        if (dr == 0) {
            for (int i = 0; i < 100 && dr == 0; i++) { usleep(50000); dr = dns_poll(&ip); }
        }
        if (dr != 1 || ip == 0) {
            fprintf(stderr, "nc: cannot resolve '%s'\n", argv[1]);
            return 1;
        }
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) { fputs("nc: bad port\n", stderr); return 1; }

    int sock = tcp_socket();
    if (sock < 0) { fputs("nc: socket failed\n", stderr); return 1; }

    int rc = tcp_connect(sock, ip, port);
    if (rc < 0 && rc != TCP_ERR_IN_PROGRESS) {
        fprintf(stderr, "nc: connect to %s:%d failed\n", argv[1], port);
        tcp_close(sock);
        return 1;
    }

    // Poll up to ~5s for the handshake.
    int established = 0;
    for (int i = 0; i < 100; i++) {
        int st = tcp_get_state(sock);
        if (st == TCP_STATE_ESTABLISHED) { established = 1; break; }
        if (st == TCP_STATE_CLOSED) break;
        usleep(50000);
    }
    if (!established) {
        fprintf(stderr, "nc: connect to %s:%d timed out\n", argv[1], port);
        tcp_close(sock);
        return 1;
    }

    // Send payload (argv[3..] joined by spaces, CRLF-terminated).
    if (argc > 3) {
        char payload[512];
        int pi = 0;
        for (int i = 3; i < argc && pi < (int)sizeof(payload) - 2; i++) {
            if (i > 3 && pi < (int)sizeof(payload) - 2) payload[pi++] = ' ';
            for (const char *s = argv[i]; *s && pi < (int)sizeof(payload) - 2; s++)
                payload[pi++] = *s;
        }
        payload[pi++] = '\r';
        payload[pi++] = '\n';
        int sent = 0;
        while (sent < pi) {
            int n = tcp_send(sock, payload + sent, pi - sent);
            if (n <= 0) break;
            sent += n;
        }
    }

    // Read response until ~3s idle or peer closes.
    char rbuf[1024];
    int idle = 0;
    while (idle < 60) {            // 60 * 50ms = 3s of idle
        int n = tcp_recv(sock, rbuf, sizeof(rbuf));
        if (n > 0) {
            out_all(rbuf, n);
            idle = 0;
        } else if (n < 0) {
            break;                 // closed/error
        } else {
            int st = tcp_get_state(sock);
            if (st == TCP_STATE_CLOSED) break;
            usleep(50000);
            idle++;
        }
    }
    tcp_close(sock);
    return 0;
}
