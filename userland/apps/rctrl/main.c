// rctrl - MayteraOS usermode remote control shell
//
// Listens on TCP port 2324 (different from the kernel-mode shell on 2323).
// Runs in ring-3 using sys_tcp_* syscalls.
// Protocol: newline-delimited text commands, same auth as kernel shell.

#include "../../libc/maytera.h"

// ── config ─────────────────────────────────────────────────────────────────
#define RCTRL_PORT          2324
#define RCTRL_USER          "admin"
#define RCTRL_PASS          "maytera"
#define RCTRL_MAX_ATTEMPTS  3

// ── per-session state ───────────────────────────────────────────────────────
#define RC_OUT_SIZE  4096
#define RC_RBUF_SIZE 512
#define RC_CMD_SIZE  256

typedef struct {
    int  sock;
    char outbuf[RC_OUT_SIZE];
    int  outpos;
    char rbuf[RC_RBUF_SIZE];
    int  rbuf_len;
} rc_session_t;

// ── output helpers ──────────────────────────────────────────────────────────

static void rc_flush(rc_session_t *s) {
    if (s->outpos <= 0 || s->sock < 0) { s->outpos = 0; return; }
    int sent = 0;
    while (sent < s->outpos) {
        int n = tcp_send(s->sock, s->outbuf + sent, (uint16_t)(s->outpos - sent));
        if (n <= 0) break;
        sent += n;
    }
    s->outpos = 0;
}

static void rc_puts(rc_session_t *s, const char *str) {
    while (str && *str) {
        if (s->outpos >= RC_OUT_SIZE - 1) rc_flush(s);
        s->outbuf[s->outpos++] = *str++;
    }
}

// Minimal vsnprintf-less printf via libc snprintf
static void rc_printf(rc_session_t *s, const char *fmt, ...) {
    char tmp[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    rc_puts(s, tmp);
}

// Read CR/LF-terminated line. Returns length, 0=no line yet, -1=closed.
static int rc_readline(rc_session_t *s, char *buf, int maxlen) {
    if (s->rbuf_len < RC_RBUF_SIZE - 1) {
        int n = tcp_recv(s->sock, s->rbuf + s->rbuf_len,
                         (uint16_t)(RC_RBUF_SIZE - 1 - s->rbuf_len));
        if (n > 0)      s->rbuf_len += n;
        else if (n < 0) { s->rbuf_len = 0; return -1; }
    }
    for (int i = 0; i < s->rbuf_len; i++) {
        if (s->rbuf[i] == '\n') {
            int len = i;
            if (len > 0 && s->rbuf[len-1] == '\r') len--;
            if (len >= maxlen) len = maxlen - 1;
            for (int j = 0; j < len; j++) buf[j] = s->rbuf[j];
            buf[len] = '\0';
            int consumed = i + 1;
            for (int j = 0; j < s->rbuf_len - consumed; j++)
                s->rbuf[j] = s->rbuf[j + consumed];
            s->rbuf_len -= consumed;
            return len;
        }
    }
    return 0;
}

// ── auth ────────────────────────────────────────────────────────────────────

static int rc_streq(const char *a, const char *b) {
    int diff = 0;
    while (*a || *b) {
        diff |= (unsigned char)*a ^ (unsigned char)*b;
        if (*a) a++;
        if (*b) b++;
    }
    return diff == 0;
}

static int rc_authenticate(rc_session_t *s) {
    char user[64], pass[64];

    for (int attempt = 0; attempt < RCTRL_MAX_ATTEMPTS; attempt++) {
        rc_puts(s, "Username: ");
        rc_flush(s);
        int r;
        do {
            r = rc_readline(s, user, (int)sizeof(user));
            if (r < 0) return 0;
            sys_sleep(5);
        } while (r == 0);

        rc_puts(s, "Password: ");
        rc_flush(s);
        do {
            r = rc_readline(s, pass, (int)sizeof(pass));
            if (r < 0) return 0;
            sys_sleep(5);
        } while (r == 0);

        if (rc_streq(user, RCTRL_USER) && rc_streq(pass, RCTRL_PASS)) {
            rc_puts(s, "\r\nWelcome.\r\n");
            rc_flush(s);
            return 1;
        }

        rc_puts(s, "\r\nInvalid credentials.\r\n");
        rc_flush(s);
        sys_sleep(2000);
    }

    rc_puts(s, "Too many failures.\r\n");
    rc_flush(s);
    return 0;
}

// ── helpers ─────────────────────────────────────────────────────────────────

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static uint32_t parse_ip(const char *str) {
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0;
    const char *p = str;
    while (*p && idx < 4) {
        while (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (uint32_t)(*p - '0');
            p++;
        }
        idx++;
        if (*p == '.') p++;
    }
    if (idx < 4) return 0;
    return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
}

// ── commands ────────────────────────────────────────────────────────────────

static void cmd_help(rc_session_t *s) {
    rc_puts(s,
        "MayteraOS Remote Shell (usermode) commands:\r\n"
        "  help          - this message\r\n"
        "  net           - network status\r\n"
        "  ping [ip]     - ping gateway or a.b.c.d\r\n"
        "  ls [path]     - list directory\r\n"
        "  cat <path>    - print file (max 4 KB)\r\n"
        "  launch <path> - launch ELF app\r\n"
        "  quit / exit   - close connection\r\n"
        "\r\n");
}

static void cmd_net(rc_session_t *s) {
    net_info_t info;
    if (net_get_info(&info) < 0) {
        rc_puts(s, "net_get_info failed\r\n");
        return;
    }
    rc_printf(s, "MAC:     %02x:%02x:%02x:%02x:%02x:%02x\r\n",
              info.mac[0], info.mac[1], info.mac[2],
              info.mac[3], info.mac[4], info.mac[5]);
    uint8_t *p;
    p = (uint8_t *)&info.ip;
    rc_printf(s, "IP:      %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    p = (uint8_t *)&info.netmask;
    rc_printf(s, "Netmask: %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    p = (uint8_t *)&info.gateway;
    rc_printf(s, "Gateway: %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    rc_printf(s, "Link:    %s\r\n", info.link_up ? "Up" : "Down");
}

static void cmd_ping(rc_session_t *s, const char *arg) {
    uint32_t target;
    if (arg && *arg) {
        target = parse_ip(arg);
        if (!target) { rc_puts(s, "Invalid IP. Usage: ping a.b.c.d\r\n"); return; }
    } else {
        net_info_t info;
        if (net_get_info(&info) < 0 || !info.gateway) {
            rc_puts(s, "No gateway. Usage: ping a.b.c.d\r\n"); return;
        }
        target = info.gateway;
    }
    uint8_t *tp = (uint8_t *)&target;
    rc_printf(s, "Pinging %d.%d.%d.%d...\r\n", tp[3],tp[2],tp[1],tp[0]);
    rc_flush(s);

    int sent = 0, received = 0;
    for (int i = 0; i < 4; i++) {
        icmp_ping(target); sent++;
        int got = 0;
        for (int w = 0; w < 300 && !got; w++) {
            uint32_t rip = 0; uint16_t seq = 0, ms = 0;
            if (icmp_ping_recv(&rip, &seq, &ms)) {
                uint8_t *rp = (uint8_t *)&rip;
                rc_printf(s, "Reply from %d.%d.%d.%d: seq=%d time=%dms\r\n",
                          rp[3],rp[2],rp[1],rp[0], seq, ms);
                received++; got = 1;
            }
            sys_sleep(1);
        }
        if (!got) rc_puts(s, "Request timed out.\r\n");
        rc_flush(s);
    }
    rc_printf(s, "--- %d sent, %d received, %d%% loss ---\r\n",
              sent, received, sent > 0 ? ((sent-received)*100/sent) : 0);
}

static void cmd_ls(rc_session_t *s, const char *path) {
    if (!path || !*path) path = "/";
    dirent_t entry;
    int count = 0;
    for (int idx = 0; ; idx++) {
        int r = sys_readdir(path, idx, &entry);
        if (r != 0) break;
        if (!entry.name[0] || entry.name[0] == '.') continue;
        if (entry.is_directory)
            rc_printf(s, "  [DIR]  %s\r\n", entry.name);
        else
            rc_printf(s, "  %8u  %s\r\n", entry.size, entry.name);
        count++;
        if ((count & 7) == 0) rc_flush(s);
    }
    rc_printf(s, "Total: %d entries\r\n", count);
}

static void cmd_cat(rc_session_t *s, const char *path) {
    if (!path || !*path) { rc_puts(s, "Usage: cat <path>\r\n"); return; }
    int fd = sys_open(path, 0);
    if (fd < 0) { rc_printf(s, "Cannot open: %s\r\n", path); return; }
    rc_printf(s, "--- %s ---\r\n", path);
    char buf[256];
    int total = 0;
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0 && total < 4096) {
        for (long i = 0; i < n && total < 4096; i++, total++) {
            char c = buf[i];
            if (c == '\n') rc_puts(s, "\r\n");
            else if (c != '\r') { char t[2] = {c, 0}; rc_puts(s, t); }
        }
    }
    if (total >= 4096) rc_puts(s, "\r\n... (truncated)\r\n");
    rc_puts(s, "\r\n--- EOF ---\r\n");
    sys_close(fd);
}

static void cmd_launch(rc_session_t *s, const char *path) {
    if (!path || !*path) { rc_puts(s, "Usage: launch <path>\r\n"); return; }
    rc_printf(s, "Launching %s...\r\n", path);
    rc_flush(s);
    int pid = sys_exec(path);
    if (pid < 0) rc_printf(s, "Launch failed (%d)\r\n", pid);
    else         rc_printf(s, "Launched PID %d\r\n", pid);
}

// Returns 1 to close, 0 to keep alive
static int dispatch(rc_session_t *s, char *line) {
    // trim leading spaces
    while (*line == ' ') line++;
    if (!*line) return 0;

    if (str_eq(line, "quit") || str_eq(line, "exit")) return 1;

    if (str_eq(line, "help"))              cmd_help(s);
    else if (str_eq(line, "net"))          cmd_net(s);
    else if (str_eq(line, "ping"))         cmd_ping(s, "");
    else if (str_starts(line, "ping "))    cmd_ping(s, line + 5);
    else if (str_eq(line, "ls"))           cmd_ls(s, "/");
    else if (str_starts(line, "ls "))      cmd_ls(s, line + 3);
    else if (str_starts(line, "cat "))     cmd_cat(s, line + 4);
    else if (str_starts(line, "launch ")) cmd_launch(s, line + 7);
    else rc_printf(s, "Unknown: '%s'  (type 'help')\r\n", line);

    return 0;
}

// ── session loop ────────────────────────────────────────────────────────────

static void handle_session(int client) {
    rc_session_t s;
    s.sock     = client;
    s.outpos   = 0;
    s.rbuf_len = 0;

    rc_puts(&s, "\r\nMayteraOS Remote Shell v1.0 (usermode)\r\n");
    rc_flush(&s);

    if (!rc_authenticate(&s)) {
        tcp_close(client);
        return;
    }

    rc_puts(&s, "Type 'help' for commands.\r\nmaytera# ");
    rc_flush(&s);

    char line[RC_CMD_SIZE];
    for (;;) {
        tcp_state_t st = tcp_get_state(client);
        if (st == TCP_STATE_CLOSED     ||
            st == TCP_STATE_CLOSE_WAIT ||
            st == TCP_STATE_LAST_ACK) break;

        int r = rc_readline(&s, line, RC_CMD_SIZE);
        if (r < 0) break;
        if (r == 0) { sys_sleep(1); continue; }

        int close = dispatch(&s, line);
        if (close) {
            rc_puts(&s, "Bye!\r\n");
            rc_flush(&s);
            break;
        }
        rc_puts(&s, "maytera# ");
        rc_flush(&s);
    }

    tcp_close(client);
}

// ── entry point ─────────────────────────────────────────────────────────────

int main(void) {
    // Wait 3s for network to settle after boot
    sys_sleep(3000);

    int srv = tcp_socket();
    if (srv < 0) sys_exit(1);

    if (tcp_bind(srv, RCTRL_PORT) < 0) { tcp_close(srv); sys_exit(1); }
    if (tcp_listen(srv, 4) < 0)        { tcp_close(srv); sys_exit(1); }

    for (;;) {
        int client = tcp_accept(srv);
        if (client >= 0) {
            handle_session(client);
        }
        sys_sleep(10);
    }
}
