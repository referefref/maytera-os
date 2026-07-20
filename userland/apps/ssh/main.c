// ssh - interactive SSH-2 client command for MayteraOS.
//
// Usage: ssh user@host [port]
//        ssh host            (user defaults to $USER or root)
//
// Parses the target, prompts for a password (no echo), puts the terminal into
// raw mode, then hands off to the kernel SSH-2 client (SYS_SSH_CLIENT), which
// shuttles bytes between the remote shell and this terminal until the session
// closes. host must be a dotted IPv4 address (no DNS resolution yet).
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/syscall.h"
#include "../../libc/termios.h"
#include "../../libc/unistd.h"

static unsigned int parse_ip(const char *s) {
    unsigned int parts[4] = {0, 0, 0, 0};
    int idx = 0;
    const char *p = s;
    while (*p && idx < 4) {
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') { parts[idx] = parts[idx] * 10 + (unsigned)(*p - '0'); p++; }
        idx++;
        if (*p == '.') p++;
        else if (*p) return 0;
    }
    if (idx != 4) return 0;
    for (int i = 0; i < 4; i++) if (parts[i] > 255) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

// Read a password with echo disabled (line-buffered is fine here).
static void read_password(char *buf, int max) {
    struct termios saved, t;
    int have = (tcgetattr(0, &saved) == 0);
    // TCSAFLUSH discards any input already queued (e.g. a stray newline left by
    // the shell that launched us when the terminal sends CR+LF for Enter), so the
    // prompt actually waits for the user instead of reading an empty password.
    if (have) { t = saved; t.c_lflag &= ~ECHO; tcsetattr(0, TCSAFLUSH, &t); }
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c < 0 || c == '\n' || c == '\r') break;
        if (c == 127 || c == '\b') { if (n > 0) n--; continue; }
        if (c >= ' ' && c < 127 && n < max - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    if (have) tcsetattr(0, TCSANOW, &saved);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ssh user@host [port]\n");
        return 1;
    }

    char user[64];
    char host[128];
    const char *target = argv[1];
    const char *at = strchr(target, '@');
    if (at) {
        int ul = (int)(at - target);
        if (ul > 63) ul = 63;
        for (int i = 0; i < ul; i++) user[i] = target[i];
        user[ul] = 0;
        int i = 0;
        for (const char *p = at + 1; *p && i < 127; p++) host[i++] = *p;
        host[i] = 0;
    } else {
        const char *u = getenv("USER");
        if (!u || !u[0]) u = "root";
        int i = 0; for (; u[i] && i < 63; i++) user[i] = u[i]; user[i] = 0;
        i = 0; for (const char *p = target; *p && i < 127; p++) host[i++] = *p; host[i] = 0;
    }

    int port = (argc >= 3) ? atoi(argv[2]) : 22;
    if (port <= 0) port = 22;

    unsigned int ip = parse_ip(host);
    if (!ip) {
        printf("ssh: cannot parse host '%s' (use a dotted IPv4 address)\n", host);
        return 1;
    }

    char pass[128];
    printf("%s@%s's password: ", user, host);
    read_password(pass, sizeof(pass));
    printf("\n");

    // Terminal size (default 80x24 if no tty / ioctl).
    int cols = 80, rows = 24;
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col) { cols = ws.ws_col; rows = ws.ws_row; }

    // Raw mode so keystrokes pass straight through to the remote shell.
    struct termios saved, raw;
    int have = (tcgetattr(0, &saved) == 0);
    if (have) { raw = saved; cfmakeraw(&raw); tcsetattr(0, TCSAFLUSH, &raw); }

    int csr = (cols << 16) | (rows & 0xffff);
    int rc = ssh_client(ip, user, pass, csr, port);

    if (have) tcsetattr(0, TCSANOW, &saved);
    printf("\r\nConnection to %s closed.\r\n", host);
    return rc < 0 ? 1 : 0;
}
