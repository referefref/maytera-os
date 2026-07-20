// curl - minimal HTTP/1.0 GET over TCP.
//   curl http://<host-ip>[:port][/path] [-o <save-path>]
// Host must be a numeric IPv4 literal (no DNS in userland yet).
// Prints the response body to stdout, or writes it to <save-path> with -o.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "fcntl.h"
#include "syscall.h"

extern int usleep(unsigned long us);

#define TCP_STATE_CLOSED       0
#define TCP_STATE_ESTABLISHED  4
#define TCP_ERR_IN_PROGRESS   (-8)

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

static char resp[262144];   // up to 256 KB of response

int main(int argc, char **argv) {
    const char *url = 0;
    const char *save = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { save = argv[++i]; }
        else if (!url) url = argv[i];
    }
    if (!url) {
        fputs("usage: curl http://<host-ip>[:port][/path] [-o <file>]\n", stderr);
        return 1;
    }

    // Strip scheme.
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    // host[:port][/path]
    char host[64]; int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < (int)sizeof(host) - 1) host[hi++] = *p++;
    host[hi] = 0;
    int port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
    }
    const char *path = (*p == '/') ? p : "/";

    unsigned int ip = parse_ip(host);
    if (ip == 0) {
        fprintf(stderr, "curl: cannot parse host '%s' (numeric IP only)\n", host);
        return 1;
    }
    if (port <= 0 || port > 65535) { fputs("curl: bad port\n", stderr); return 1; }

    int sock = tcp_socket();
    if (sock < 0) { fputs("curl: socket failed\n", stderr); return 1; }
    int rc = tcp_connect(sock, ip, port);
    if (rc < 0 && rc != TCP_ERR_IN_PROGRESS) {
        fprintf(stderr, "curl: connect to %s:%d failed\n", host, port);
        tcp_close(sock);
        return 1;
    }
    int established = 0;
    for (int i = 0; i < 100; i++) {
        int st = tcp_get_state(sock);
        if (st == TCP_STATE_ESTABLISHED) { established = 1; break; }
        if (st == TCP_STATE_CLOSED) break;
        usleep(50000);
    }
    if (!established) {
        fprintf(stderr, "curl: connect to %s:%d timed out\n", host, port);
        tcp_close(sock);
        return 1;
    }

    // Send the request.
    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: maytera-curl\r\nConnection: close\r\n\r\n",
        path, host);
    int sent = 0;
    while (sent < rl) {
        int n = tcp_send(sock, req + sent, rl - sent);
        if (n <= 0) break;
        sent += n;
    }

    // Read the whole response. tcp_recv must be called with len <= the kernel's
    // bounce buffer (1600 bytes), so read in <=1024-byte chunks.
    int total = 0, idle = 0;
    while (total < (int)sizeof(resp) && idle < 60) {
        int want = (int)sizeof(resp) - total;
        if (want > 1024) want = 1024;
        int n = tcp_recv(sock, resp + total, want);
        if (n > 0) { total += n; idle = 0; }
        else if (n < 0) break;
        else {
            int st = tcp_get_state(sock);
            if (st == TCP_STATE_CLOSED) break;
            usleep(50000);
            idle++;
        }
    }
    tcp_close(sock);

    // Split headers from body at the first blank line.
    int body = 0;
    for (int i = 0; i + 3 < total; i++) {
        if (resp[i]=='\r' && resp[i+1]=='\n' && resp[i+2]=='\r' && resp[i+3]=='\n') {
            body = i + 4;
            break;
        }
    }
    if (body == 0) body = 0;   // no header boundary found: emit everything
    int blen = total - body;

    if (save) {
        int fd = open(save, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) { fprintf(stderr, "curl: cannot open %s\n", save); return 1; }
        int off = 0;
        while (off < blen) {
            long w = write(fd, resp + body + off, blen - off);
            if (w <= 0) break;
            off += w;
        }
        close(fd);
        fprintf(stderr, "curl: wrote %d bytes to %s\n", blen, save);
    } else {
        int off = 0;
        while (off < blen) {
            long w = write(1, resp + body + off, blen - off);
            if (w <= 0) break;
            off += w;
        }
    }
    return 0;
}
