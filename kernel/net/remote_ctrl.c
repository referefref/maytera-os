// remote_ctrl.c - Kernel-mode TCP remote control service for MayteraOS
//
// Listens on TCP port 2323. Each connection gets an interactive shell
// where commands mirror the kernel_shell() in main.c.
//
// ROADMAP: User-mode version planned. Will run as a ring-3 process using
// sys_tcp_* syscalls. The line protocol (newline-delimited text) stays the
// same so existing clients need no changes.

#include "remote_ctrl.h"
#include "../exec/ne.h"
extern int x86_16_selftest(void);
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../net/tcp.h"
#include "../net/net.h"
#include "../net/ip.h"
#include "ssh/ssh2.h"
#include "../mm/heap.h"
#include "../net/smb.h"   // task #317: SMB VFS routing + self-test
#include "../mm/pmm.h"
#include "../fs/fat.h"
#include "../drivers/ata.h"   // task #306: installer disk enumeration
#include "../proc/process.h"
#include "../proc/syscall.h"
#include "../proc/services.h"
#include "../boot_info.h"
#include "../security/nova.h"   // #449 Nova Guard: LLM prompt-injection screening

// ── forward declarations for kernel internals ──────────────────────────────
extern fat_fs_t  g_fat_fs;
extern boot_info_t *g_boot_info;
extern uint64_t  timer_ticks;
extern uint32_t  g_timer_hz;
extern void      proc_sleep(uint32_t ms);
extern void      acpi_shutdown(void);
extern void      acpi_reboot(void);
extern int       icmp_ping(uint32_t dest_ip);
extern int       icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms);
extern void      pmm_print_stats(void);
extern void      heap_print_stats(void);
extern void      proc_print_list(void);
extern void      nic_get_mac(uint8_t *mac);
extern int       nic_link_up(void);
extern uint32_t  ip_get_address(void);
extern uint32_t  ip_get_gateway(void);
extern uint32_t  ip_get_netmask(void);
extern int64_t   sys_exec(const char *path);

// ── per-session state ──────────────────────────────────────────────────────
#define RC_OUT_SIZE  8192
#define RC_RBUF_SIZE 512
#define RC_CMD_SIZE  256

typedef struct {
    int  sock;
    char outbuf[RC_OUT_SIZE];
    int  outpos;
    // per-session receive ring (so each session is independent)
    char rbuf[RC_RBUF_SIZE];
    int  rbuf_len;
} rc_session_t;

// ── output helpers ─────────────────────────────────────────────────────────

static void rc_flush(rc_session_t *s) {
    if (s->outpos <= 0 || s->sock < 0) { s->outpos = 0; return; }
    int sent = 0;
    while (sent < s->outpos) {
        int n = tcp_send(s->sock,
                         (s->outbuf + sent),
                         (uint16_t)(s->outpos - sent));
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

static void rc_printf(rc_session_t *s, const char *fmt, ...) {
    char tmp[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    rc_puts(s, tmp);
}

// Read a CR/LF-terminated line; returns chars in buf, 0=no line yet, -1=closed
static int rc_readline(rc_session_t *s, char *buf, int maxlen) {
    // Try to receive more bytes
    if (s->rbuf_len < RC_RBUF_SIZE - 1) {
        int n = tcp_recv(s->sock,
                         (s->rbuf + s->rbuf_len),
                         (uint16_t)(RC_RBUF_SIZE - 1 - s->rbuf_len));
        if (n > 0)       s->rbuf_len += n;
        else if (n < 0) { s->rbuf_len = 0; return -1; }
    }
    // Search for newline
    for (int i = 0; i < s->rbuf_len; i++) {
        if (s->rbuf[i] == '\n') {
            int len = i;
            if (len > 0 && s->rbuf[len-1] == '\r') len--;
            if (len >= maxlen) len = maxlen - 1;
            memcpy(buf, s->rbuf, (uint64_t)len);
            buf[len] = '\0';
            int consumed = i + 1;
            memmove(s->rbuf, s->rbuf + consumed, (uint64_t)(s->rbuf_len - consumed));
            s->rbuf_len -= consumed;
            return len;
        }
    }
    return 0;
}

// ── command implementations ────────────────────────────────────────────────

// Desktop-side hook for live start-menu reload after install/uninstall.
extern void desktop_menu_reload(void);

// Read `count` raw bytes from the socket into buf. First drains the
// session's line-read buffer (s->rbuf) then blocks on tcp_recv. Returns
// the number of bytes read (may be less than count on timeout/close).
static uint32_t rc_read_raw(rc_session_t *s, uint8_t *buf, uint32_t count) {
    uint32_t got = 0;

    // Drain any bytes already in s->rbuf first.
    if (s->rbuf_len > 0) {
        uint32_t take = (uint32_t)s->rbuf_len;
        if (take > count) take = count;
        memcpy(buf, s->rbuf, take);
        got += take;
        int remaining = s->rbuf_len - (int)take;
        if (remaining > 0) memmove(s->rbuf, s->rbuf + take, (uint64_t)remaining);
        s->rbuf_len = remaining;
    }

    // Then pull from the socket. Roughly 30 s total idle timeout.
    uint64_t deadline = timer_ticks + (uint64_t)(g_timer_hz * 30);
    while (got < count) {
        uint32_t want = count - got;
        uint16_t chunk = (want > 1400) ? 1400 : (uint16_t)want;
        int n = tcp_recv(s->sock, buf + got, chunk);
        if (n > 0) {
            got += (uint32_t)n;
            deadline = timer_ticks + (uint64_t)(g_timer_hz * 30);
        } else if (n < 0) {
            break;      // closed
        } else {
            if (timer_ticks > deadline) break;
            proc_sleep(5);
        }
    }
    return got;
}

// ── upload <path> <size> ──
// Next `<size>` raw bytes on the socket are written to the file.
// Capped at 2 MB as a safety net.
#define RC_UPLOAD_MAX (2u * 1024u * 1024u)

static void rc_cmd_upload(rc_session_t *s, const char *arg) {
    if (!arg || !*arg) { rc_puts(s, "Usage: upload <path> <size>\r\n"); return; }
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    // Parse <path> <size>
    char path[128];
    uint32_t size = 0;
    int i = 0;
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && i < (int)sizeof(path) - 1) path[i++] = *arg++;
    path[i] = '\0';
    while (*arg == ' ') arg++;
    while (*arg >= '0' && *arg <= '9') { size = size * 10u + (uint32_t)(*arg - '0'); arg++; }

    if (path[0] == '\0' || size == 0) {
        rc_puts(s, "Usage: upload <path> <size>\r\n");
        return;
    }
    if (size > RC_UPLOAD_MAX) {
        rc_printf(s, "Upload too large (%u > %u)\r\n", size, RC_UPLOAD_MAX);
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) { rc_puts(s, "OOM\r\n"); return; }

    rc_printf(s, "READY: send %u bytes\r\n", size);
    rc_flush(s);

    uint32_t got = rc_read_raw(s, buf, size);
    if (got != size) {
        rc_printf(s, "upload: short read (%u of %u)\r\n", got, size);
        kfree(buf);
        return;
    }

    int rc = fat_write_file(&g_fat_fs, path, buf, size);
    kfree(buf);
    if (rc == 0) rc_printf(s, "upload OK: %s (%u bytes)\r\n", path, size);
    else         rc_printf(s, "upload ERR: write %s failed\r\n", path);
}

// ── apps / install / uninstall ──
// Stores installed-app entries in /APPS/REGINI.CFG using the same
// CATEGORY=.../APP=... syntax desktop.c already parses. The first
// line is always `CATEGORY=Installed` so entries appear in their own
// start-menu section.

static void rc_cmd_apps(rc_session_t *s) {
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }
    uint32_t sz = 0;
    char *d = (char *)fat_read_file(&g_fat_fs, "/APPS/REGINI.CFG", &sz);
    if (!d || sz == 0) {
        rc_puts(s, "(no installed apps)\r\n");
        if (d) kfree(d);
        return;
    }
    rc_puts(s, "Installed apps (/APPS/REGINI.CFG):\r\n");
    for (uint32_t i = 0; i < sz; i++) {
        char c = d[i];
        if      (c == '\n') rc_puts(s, "\r\n");
        else if (c != '\r') { char t[2] = {c, 0}; rc_puts(s, t); }
    }
    rc_puts(s, "\r\n");
    kfree(d);
}

// Return 1 if `line` is `APP=<name>,...` (case-sensitive, rest ignored).
static int app_line_matches(const char *line, uint32_t llen, const char *name) {
    const char *prefix = "APP=";
    uint32_t plen = 4;
    if (llen < plen) return 0;
    for (uint32_t i = 0; i < plen; i++) if (line[i] != prefix[i]) return 0;

    uint32_t nlen = 0;
    while (name[nlen]) nlen++;
    if (llen < plen + nlen) return 0;
    for (uint32_t i = 0; i < nlen; i++) if (line[plen + i] != name[i]) return 0;
    // next char must be ',' or end-of-line
    if (llen == plen + nlen) return 1;
    return line[plen + nlen] == ',';
}

static void rc_cmd_install(rc_session_t *s, const char *arg) {
    if (!arg || !*arg) { rc_puts(s, "Usage: install <name> <elf-path>\r\n"); return; }
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    char name[64], path[128];
    int ni = 0, pi = 0;
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && ni < (int)sizeof(name) - 1) name[ni++] = *arg++;
    name[ni] = '\0';
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && *arg != '\r' && *arg != '\n' && pi < (int)sizeof(path) - 1)
        path[pi++] = *arg++;
    path[pi] = '\0';

    if (!name[0] || !path[0]) {
        rc_puts(s, "Usage: install <name> <elf-path>\r\n");
        return;
    }

    // Load current REGINI.CFG (if any) and strip any existing entry for
    // the same name so install is idempotent.
    uint32_t old_sz = 0;
    char *old = (char *)fat_read_file(&g_fat_fs, "/APPS/REGINI.CFG", &old_sz);

    // New content: header + filtered old + new APP= line
    uint32_t cap = (old_sz ? old_sz : 0) + 256;
    char *buf = (char *)kmalloc(cap);
    if (!buf) { rc_puts(s, "OOM\r\n"); if (old) kfree(old); return; }
    uint32_t pos = 0;
    const char *hdr = "CATEGORY=Installed\n";
    for (const char *p = hdr; *p && pos < cap - 1; p++) buf[pos++] = *p;

    if (old) {
        uint32_t start = 0;
        for (uint32_t i = 0; i <= old_sz; i++) {
            if (i == old_sz || old[i] == '\n') {
                uint32_t llen = i - start;
                if (llen > 0 && old[start + llen - 1] == '\r') llen--;
                // Skip the header line and any previous entry for this name
                int skip = 0;
                if (llen >= 9 && old[start] == 'C' && old[start+1] == 'A' &&
                    old[start+2] == 'T' && old[start+3] == 'E' &&
                    old[start+4] == 'G') skip = 1;
                if (app_line_matches(old + start, llen, name)) skip = 1;
                if (!skip && llen > 0 && pos + llen + 2 < cap) {
                    memcpy(buf + pos, old + start, llen);
                    pos += llen;
                    buf[pos++] = '\n';
                }
                start = i + 1;
            }
        }
        kfree(old);
    }

    // Append new entry
    char line[256];
    int n = 0;
    line[n++] = 'A'; line[n++] = 'P'; line[n++] = 'P'; line[n++] = '=';
    for (int j = 0; name[j] && n < (int)sizeof(line) - 2; j++) line[n++] = name[j];
    line[n++] = ',';
    const char *icon = "terminal";
    for (int j = 0; icon[j] && n < (int)sizeof(line) - 2; j++) line[n++] = icon[j];
    line[n++] = ',';
    for (int j = 0; path[j] && n < (int)sizeof(line) - 2; j++) line[n++] = path[j];
    line[n++] = '\n';
    if (pos + (uint32_t)n < cap) {
        memcpy(buf + pos, line, (uint32_t)n);
        pos += (uint32_t)n;
    }

    int rc = fat_write_file(&g_fat_fs, "/APPS/REGINI.CFG", buf, pos);
    kfree(buf);
    if (rc != 0) {
        rc_puts(s, "install: write REGINI.CFG failed\r\n");
        return;
    }

    desktop_menu_reload();
    rc_printf(s, "installed: %s -> %s\r\n", name, path);
}


// rc_cmd_win16install: native MayteraOS "Win3.x installer".
// Registers a Win16 program into a Start-menu program group by appending to
// /WIN16GRP.CFG (read by the userland compositor's start menu). The group is
// created if it does not already exist. Items are inserted right after their
// GROUP header so the compositor's loader (which attributes ITEM lines to the
// most recent GROUP) groups them correctly.
//
//   Usage: win16install <name> <group> <path>
//     <name>  display name (use _ for spaces; converted to spaces)
//     <group> program-group name (use _ for spaces)
//     <path>  path to the NE/.COM executable on the FAT disk
static void rc_cmd_win16install(rc_session_t *s, const char *arg) {
    if (!arg || !*arg) {
        rc_puts(s, "Usage: win16install <name> <group> <path>\r\n");
        return;
    }
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    char name[64], group[64], path[128];
    int i;
    while (*arg == ' ') arg++;
    i = 0; while (*arg && *arg != ' ' && i < (int)sizeof(name)-1)  name[i++]  = *arg++;  name[i]  = 0;
    while (*arg == ' ') arg++;
    i = 0; while (*arg && *arg != ' ' && i < (int)sizeof(group)-1) group[i++] = *arg++;  group[i] = 0;
    while (*arg == ' ') arg++;
    i = 0; while (*arg && *arg != ' ' && *arg != '\r' && *arg != '\n' && i < (int)sizeof(path)-1)
        path[i++] = *arg++;
    path[i] = 0;

    if (!name[0] || !group[0] || !path[0]) {
        rc_puts(s, "Usage: win16install <name> <group> <path>\r\n");
        return;
    }
    // Underscores -> spaces for display fields.
    for (char *p = name;  *p; p++) if (*p == '_') *p = ' ';
    for (char *p = group; *p; p++) if (*p == '_') *p = ' ';

    uint32_t old_sz = 0;
    char *old = (char *)fat_read_file(&g_fat_fs, "/WIN16GRP.CFG", &old_sz);

    uint32_t cap = (old ? old_sz : 0) + 512;
    char *buf = (char *)kmalloc(cap);
    if (!buf) { rc_puts(s, "OOM\r\n"); if (old) kfree(old); return; }
    uint32_t pos = 0;

    // Build the new ITEM line once.
    char item[256];
    int n = 0;
    const char *ip = "ITEM|";
    for (const char *q = ip; *q; q++) item[n++] = *q;
    for (char *q = name; *q && n < (int)sizeof(item)-2; q++) item[n++] = *q;
    item[n++] = '|';
    for (char *q = path; *q && n < (int)sizeof(item)-2; q++) item[n++] = *q;
    item[n++] = '\n';

    int group_found = 0;
    int item_written = 0;

    if (old) {
        uint32_t start = 0;
        for (uint32_t k = 0; k <= old_sz; k++) {
            if (k == old_sz || old[k] == '\n') {
                uint32_t llen = k - start;
                if (k == old_sz && llen == 0) { start = k + 1; continue; }
                uint32_t cl = llen;
                if (cl > 0 && old[start + cl - 1] == '\r') cl--;

                // Copy this line through.
                if (pos + cl + 1 < cap) {
                    memcpy(buf + pos, old + start, cl);
                    pos += cl;
                    buf[pos++] = '\n';
                }
                // Is this our GROUP header? Compare "GROUP|<group>".
                if (cl >= 6 && memcmp(old + start, "GROUP|", 6) == 0) {
                    const char *gn = old + start + 6;
                    uint32_t gl = cl - 6;
                    uint32_t want = 0; while (group[want]) want++;
                    if (gl == want && memcmp(gn, group, want) == 0) {
                        group_found = 1;
                        // Insert the ITEM line right after this header.
                        if (pos + (uint32_t)n < cap) {
                            memcpy(buf + pos, item, (uint32_t)n);
                            pos += (uint32_t)n;
                            item_written = 1;
                        }
                    }
                }
                start = k + 1;
            }
        }
        kfree(old);
    }

    // Group did not exist: append a fresh GROUP header + the ITEM line.
    if (!group_found) {
        char gh[80];
        int m = 0;
        const char *gp = "GROUP|";
        for (const char *q = gp; *q; q++) gh[m++] = *q;
        for (char *q = group; *q && m < (int)sizeof(gh)-2; q++) gh[m++] = *q;
        gh[m++] = '\n';
        if (pos + (uint32_t)m < cap) { memcpy(buf + pos, gh, (uint32_t)m); pos += (uint32_t)m; }
        if (pos + (uint32_t)n < cap) { memcpy(buf + pos, item, (uint32_t)n); pos += (uint32_t)n; item_written = 1; }
    }

    if (!item_written) { rc_puts(s, "win16install: config full\r\n"); kfree(buf); return; }

    int rc = fat_write_file(&g_fat_fs, "/WIN16GRP.CFG", buf, pos);
    kfree(buf);
    if (rc != 0) { rc_puts(s, "win16install: write WIN16GRP.CFG failed\r\n"); return; }

    rc_printf(s, "win16install: %s -> group \"%s\" (%s)\r\n", name, group, path);
    rc_puts(s, "win16install: restart compositor (or reboot) to refresh Start menu\r\n");
}

static void rc_cmd_uninstall(rc_session_t *s, const char *arg) {
    if (!arg || !*arg) { rc_puts(s, "Usage: uninstall <name>\r\n"); return; }
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    char name[64];
    int ni = 0;
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && *arg != '\r' && *arg != '\n' && ni < (int)sizeof(name) - 1)
        name[ni++] = *arg++;
    name[ni] = '\0';
    if (!name[0]) { rc_puts(s, "Usage: uninstall <name>\r\n"); return; }

    uint32_t old_sz = 0;
    char *old = (char *)fat_read_file(&g_fat_fs, "/APPS/REGINI.CFG", &old_sz);
    if (!old || old_sz == 0) {
        rc_puts(s, "uninstall: REGINI.CFG empty or missing\r\n");
        if (old) kfree(old);
        return;
    }

    uint32_t cap = old_sz + 32;
    char *buf = (char *)kmalloc(cap);
    if (!buf) { rc_puts(s, "OOM\r\n"); kfree(old); return; }
    uint32_t pos = 0;
    int removed = 0;

    uint32_t start = 0;
    for (uint32_t i = 0; i <= old_sz; i++) {
        if (i == old_sz || old[i] == '\n') {
            uint32_t llen = i - start;
            if (llen > 0 && old[start + llen - 1] == '\r') llen--;
            if (app_line_matches(old + start, llen, name)) {
                removed++;
            } else if (llen > 0 && pos + llen + 2 < cap) {
                memcpy(buf + pos, old + start, llen);
                pos += llen;
                buf[pos++] = '\n';
            }
            start = i + 1;
        }
    }
    kfree(old);

    int rc = fat_write_file(&g_fat_fs, "/APPS/REGINI.CFG", buf, pos);
    kfree(buf);
    if (rc != 0) { rc_puts(s, "uninstall: write failed\r\n"); return; }

    if (removed) {
        desktop_menu_reload();
        rc_printf(s, "uninstalled: %s (%d entr%s removed)\r\n",
                  name, removed, removed == 1 ? "y" : "ies");
    } else {
        rc_printf(s, "uninstall: no entry named '%s'\r\n", name);
    }
}


// ---------------------------------------------------------------------------
// task #306: OS install-to-disk. Drives the kernel installer engine headlessly:
// partitions the target (GPT + protective MBR, single EF00 ESP at LBA 2048) and
// raw-clones the live/source ESP onto it so the target boots MayteraOS on its
// own. Usage: "osinstall" (auto-pick first non-boot ATA disk) or "osinstall N".
// ---------------------------------------------------------------------------
static void rc_osinstall_progress(void *ctx, int percent, const char *msg) {
    rc_session_t *s = (rc_session_t *)ctx;
    rc_printf(s, "  [%3d%%] %s\r\n", percent, msg);
    rc_flush(s);
}

static void rc_cmd_osinstall(rc_session_t *s, const char *arg) {
    extern int installer_do_install(int, void (*)(void *, int, const char *), void *);
    int target = -1;
    while (arg && *arg == ' ') arg++;
    if (arg && *arg >= '0' && *arg <= '9') {
        target = 0;
        while (*arg >= '0' && *arg <= '9') { target = target * 10 + (*arg - '0'); arg++; }
    } else {
        for (int d = 0; d < 4; d++) {
            if (d == g_fat_fs.drive) continue;
            uint8_t ch = (uint8_t)((d >> 1) & 1), u = (uint8_t)(d & 1);
            ata_drive_t *dr = ata_get_drive(ch, u);
            if (dr && dr->exists && dr->type == ATA_TYPE_ATA) { target = d; break; }
        }
    }
    if (target < 0) { rc_puts(s, "osinstall: no target disk found (need a second ATA disk)\r\n"); return; }
    rc_printf(s, "osinstall: installing MayteraOS to drive %d (boot/source drive=%d)\r\n",
              target, g_fat_fs.drive);
    rc_flush(s);
    int r = installer_do_install(target, rc_osinstall_progress, s);
    if (r == 0) rc_puts(s, "osinstall: SUCCESS - target disk is now bootable\r\n");
    else        rc_printf(s, "osinstall: FAILED (rc=%d)\r\n", r);
    rc_flush(s);
}

static void rc_cmd_help(rc_session_t *s) {
    rc_puts(s,
        "MayteraOS Remote Shell commands:\r\n"
        "  help          - this message\r\n"
        "  mem           - RAM summary\r\n"
        "  pmm           - physical memory stats (serial log)\r\n"
        "  heap          - heap stats (serial log)\r\n"
        "  ticks         - timer tick counter\r\n"
        "  ps            - process list (serial log)\r\n"
        "  net           - network status\r\n"
        "  ping [ip]     - ping gateway or a.b.c.d\r\n"
        "  ls [path]     - list FAT directory (serial log)\r\n"
        "  cat <path>    - print file contents (max 4 KB)\r\n"
        "  launch <path> - launch ELF app e.g. /APPS/TERMINAL.ELF\r\n"
        "  osinstall [N] - install MayteraOS to disk N (GPT+clone)\r\n"
        "  launchap <path> - launch app on an application processor (#279 SMP)\r\n"
        "  shell [path]  - spawn /APPS/MSH (or <path>) on a PTY (Phase J)\r\n"
        "  run <path> [args] - spawn ELF, stream stdout, print [exit N] (mdev)\r\n"
        "  nova <text>   - scan text for LLM prompt-injection (Nova Guard #449)\r\n"
        "  novatest      - run the Nova Guard self-test\r\n"
        "  upload <path> <size>       - receive raw bytes and write to file\r\n"
        "  apps                       - list installed apps\r\n"
        "  install <name> <elf-path>  - register app in start menu\r\n"
        "  uninstall <name>           - remove registered app\r\n"
        "  reload                     - reload start menu from config\r\n"
        "  svc [list|start|stop|enable|disable <name>] - background services\r\n"
        "  reboot        - reboot system\r\n"
        "  shutdown      - power off system\r\n"
        "  quit / exit   - close connection\r\n"
        "\r\n"
        "File/text/net tools (grep, head, tail, less, cp, mv, rm, sed,\r\n"
        "curl, nc, ...) now run in userland: type 'shell' for a msh prompt.\r\n"
        "\r\n");
}

static void rc_cmd_net(rc_session_t *s) {
    uint8_t mac[6];
    nic_get_mac(mac);
    rc_printf(s, "MAC:     %02x:%02x:%02x:%02x:%02x:%02x\r\n",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint32_t ip = ip_get_address();
    uint32_t gw = ip_get_gateway();
    uint32_t nm = ip_get_netmask();
    uint8_t *p;
    p = (uint8_t *)&ip; rc_printf(s, "IP:      %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    p = (uint8_t *)&nm; rc_printf(s, "Netmask: %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    p = (uint8_t *)&gw; rc_printf(s, "Gateway: %d.%d.%d.%d\r\n", p[3],p[2],p[1],p[0]);
    rc_printf(s, "Link:    %s\r\n", nic_link_up() ? "Up" : "Down");
}

static uint32_t rc_parse_ip(const char *str) {
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0;
    const char *p = str;
    while (*p && idx < 4) {
        while (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx]*10 + (uint32_t)(*p - '0');
            p++;
        }
        idx++;
        if (*p == '.') p++;
    }
    if (idx < 4) return 0;
    return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
}

static void rc_cmd_ping(rc_session_t *s, const char *arg) {
    uint32_t target;
    if (arg && *arg) {
        target = rc_parse_ip(arg);
        if (!target) { rc_puts(s, "Invalid IP. Usage: ping a.b.c.d\r\n"); return; }
    } else {
        target = ip_get_gateway();
        if (!target) { rc_puts(s, "No gateway. Usage: ping a.b.c.d\r\n"); return; }
    }
    uint8_t *tp = (uint8_t *)&target;
    rc_printf(s, "Pinging %d.%d.%d.%d...\r\n", tp[3],tp[2],tp[1],tp[0]);
    rc_flush(s);

    int sent = 0, received = 0;
    for (int i = 0; i < 4; i++) {
        icmp_ping(target); sent++;
        int got = 0;
        for (int w = 0; w < 300 && !got; w++) {
            uint32_t rip; uint16_t seq, ms;
            if (icmp_get_ping_reply(&rip, &seq, &ms)) {
                uint8_t *rp = (uint8_t *)&rip;
                rc_printf(s, "Reply from %d.%d.%d.%d: seq=%d time=%dms\r\n",
                          rp[3],rp[2],rp[1],rp[0], seq, ms);
                received++; got = 1;
            }
            proc_sleep(1);
        }
        if (!got) rc_puts(s, "Request timed out.\r\n");
        rc_flush(s);
    }
    rc_printf(s, "--- %d sent, %d received, %d%% loss ---\r\n",
              sent, received, sent > 0 ? ((sent-received)*100/sent) : 0);
}

static void rc_cmd_fetch(rc_session_t *s, const char *url) {
    extern int https_get(const char *url, uint8_t **b, uint32_t *l, int *st);
    extern int wget_fetch(const char *url, uint8_t **b, uint32_t *l, int *st);
    extern void kfree(void *);
    if (!url || !*url) { rc_puts(s, "usage: fetch <url>\r\n"); return; }
    uint8_t *body = 0; uint32_t len = 0; int status = 0;
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    int r = https ? https_get(url, &body, &len, &status)
                  : wget_fetch(url, &body, &len, &status);
    rc_printf(s, "fetch r=%d status=%d len=%u\r\n", r, status, len);
    if (body && len) {
        uint32_t n = len < 400 ? len : 400;
        char prev[420]; uint32_t j = 0;
        for (uint32_t i = 0; i < n; i++) { char c = (char)body[i]; prev[j++] = ((c>=32&&c<127)||c=='\n') ? c : '.'; }
        prev[j] = 0; rc_puts(s, prev); rc_puts(s, "\r\n");
    }
    if (body) kfree(body);
}

// ssh <ip> <user> <password> - SSH-2 client end-to-end test. Connects, opens an
// interactive shell, runs a probe command, streams output, then closes.
static rc_session_t *g_ssh_rc;
static void rc_ssh_log(void *ctx, const char *msg) {
    rc_session_t *s = (rc_session_t *)ctx;
    if (!s) return;
    rc_puts(s, "  [ssh] "); rc_puts(s, msg); rc_flush(s);
}
static void rc_ssh_on_data(void *ctx, const uint8_t *data, int len) {
    (void)ctx;
    if (!g_ssh_rc) return;
    char tmp[260]; int j = 0;
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) tmp[j++] = c;
        else if (c != '\r') tmp[j++] = '.';
        if (j >= (int)sizeof(tmp) - 2) { tmp[j] = 0; rc_puts(g_ssh_rc, tmp); j = 0; }
    }
    if (j) { tmp[j] = 0; rc_puts(g_ssh_rc, tmp); }
    rc_flush(g_ssh_rc);
}
static void rc_cmd_ssh(rc_session_t *s, const char *arg) {
    char ip_s[64] = {0}, user[64] = {0}, pass[160] = {0};
    // parse: <ip> <user> <password>
    int n = 0; const char *p = arg;
    char *dst[3] = { ip_s, user, pass }; int cap[3] = { 63, 63, 159 };
    while (p && *p && n < 3) {
        while (*p == ' ') p++;
        int k = 0; while (*p && *p != ' ' && k < cap[n]) dst[n][k++] = *p++;
        dst[n][k] = 0; if (k) n++;
        while (*p && *p != ' ') p++;   // skip overflow
    }
    if (n < 3) { rc_puts(s, "usage: ssh <ip> <user> <password>\r\n"); return; }
    uint32_t ip = rc_parse_ip(ip_s);
    if (!ip) { rc_puts(s, "ssh: bad ip\r\n"); return; }
    ssh2_client_t *c = (ssh2_client_t *)kmalloc(sizeof(ssh2_client_t));
    if (!c) { rc_puts(s, "ssh: out of memory\r\n"); return; }
    g_ssh_rc = s;
    extern void ssh2_set_log(void (*fn)(void *, const char *), void *ctx);
    ssh2_set_log(rc_ssh_log, s);   // route SSH stage logs to this session
    rc_printf(s, "ssh: connecting to %s:22 as %s ...\r\n", ip_s, user); rc_flush(s);
    int r = ssh2_connect(c, ip, 22, user, pass, 80, 24, rc_ssh_on_data, s);
    if (r != 0) { rc_printf(s, "ssh: FAILED: %s\r\n", c->err); kfree(c); g_ssh_rc = 0; ssh2_set_log(0, 0); return; }
    rc_puts(s, "ssh: CONNECTED + authenticated + shell ready\r\n"); rc_flush(s);
    ssh2_send_input(c, "uname -a; id; echo MAYTERA_SSH_OK\n", 34);
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start = timer_ticks;
    while (timer_ticks - start < hz * 6 && !c->closed) {
        ssh2_pump(c);
        proc_sleep(5);
    }
    ssh2_send_input(c, "exit\n", 5);
    for (int i = 0; i < 40 && !c->closed; i++) { ssh2_pump(c); proc_sleep(5); }
    ssh2_close(c);
    kfree(c);
    g_ssh_rc = 0;
    ssh2_set_log(0, 0);
    rc_puts(s, "\r\nssh: closed\r\n");
}

// kimi <prompt> - test the HTTPS POST + Kimi (Moonshot) chat API end to end.
// Reads the key from /CONFIG/KIMI.KEY, posts a one-shot prompt, prints the reply.
static void rc_cmd_kimi(rc_session_t *s, const char *prompt) {
    extern int https_post(const char *url, const char *headers, const char *body,
                          uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    if (!prompt || !*prompt) { rc_puts(s, "usage: kimi <prompt>\r\n"); return; }

    // #449 Nova Guard: screen the prompt for injection/jailbreak patterns before
    // it reaches the LLM. Default policy is warn-and-continue (surfaces the
    // detection without silently blocking a possibly-legitimate prompt).
    {
        nova_hit_t nh[4];
        int nn = nova_scan(prompt, nh, 4);
        if (nn > 0) {
            rc_printf(s, "[NOVA] prompt-injection warning: %d rule(s) fired\r\n", nn);
            for (int i = 0; i < nn && i < 4; i++)
                rc_printf(s, "  - %s [%s] (matched: %s)\r\n",
                          nh[i].rule, nova_sev_name(nh[i].severity), nh[i].matched);
            rc_flush(s);
        }
    }

    // Load the API key.
    uint32_t ksz = 0;
    char *kf = (char *)fat_read_file(&g_fat_fs, "/CONFIG/KIMI.KEY", &ksz);
    if (!kf || ksz == 0) { rc_puts(s, "kimi: /CONFIG/KIMI.KEY missing\r\n"); if (kf) kfree(kf); return; }
    char key[128]; uint32_t ki = 0;
    for (uint32_t i = 0; i < ksz && ki < sizeof(key)-1; i++) {
        char c = kf[i]; if (c=='\r'||c=='\n') break; key[ki++] = c;
    }
    key[ki] = 0; kfree(kf);

    // Build headers + JSON body. Escape quotes/backslashes/newlines in the prompt.
    static char headers[256];
    int hl = 0;
    const char *h1 = "Authorization: Bearer ";
    for (const char *p = h1; *p; p++) headers[hl++] = *p;
    for (uint32_t i = 0; key[i]; i++) headers[hl++] = key[i];
    const char *h2 = "\r\nContent-Type: application/json\r\n";
    for (const char *p = h2; *p; p++) headers[hl++] = *p;
    headers[hl] = 0;

    static char body[2048];
    int bl = 0;
    const char *pre = "{\"model\":\"kimi-k2.6\",\"messages\":[{\"role\":\"user\",\"content\":\"";
    for (const char *p = pre; *p; p++) body[bl++] = *p;
    for (const char *p = prompt; *p && bl < (int)sizeof(body)-32; p++) {
        char c = *p;
        if (c=='"'||c=='\\') { body[bl++]='\\'; body[bl++]=c; }
        else if (c=='\n') { body[bl++]='\\'; body[bl++]='n'; }
        else body[bl++]=c;
    }
    const char *post = "\"}]}";
    for (const char *p = post; *p; p++) body[bl++] = *p;
    body[bl] = 0;

    rc_puts(s, "kimi: posting...\r\n"); rc_flush(s);
    uint8_t *resp = 0; uint32_t rlen = 0; int status = 0;
    int r = https_post("https://api.moonshot.ai/v1/chat/completions", headers, body, &resp, &rlen, &status);
    rc_printf(s, "kimi: r=%d status=%d len=%u\r\n", r, status, rlen);
    if (resp && rlen) {
        // Print the raw response (first 1200 bytes) so we can see content / errors.
        uint32_t n = rlen < 1200 ? rlen : 1200;
        for (uint32_t i = 0; i < n; i++) {
            char c = (char)resp[i];
            if (c=='\n') rc_puts(s, "\r\n");
            else if (c!='\r') { char t[2]={c,0}; rc_puts(s, t); }
        }
        rc_puts(s, "\r\n");
    }
    if (resp) kfree(resp);
}


// #297 diagnostics: dump the TCP connection table state distribution.
static void rc_cmd_tcpstat(rc_session_t *s) {
    extern int tcp_diag_snapshot(int *out_active, int *out_counts, int ncounts);
    extern const char* tcp_state_name(tcp_state_t state);
    int active = 0; int counts[11];
    int total = tcp_diag_snapshot(&active, counts, 11);
    rc_printf(s, "TCP slots: %d/%d active\r\n", active, total);
    for (int i = 0; i < 11; i++) {
        if (counts[i]) rc_printf(s, "  %-12s %d\r\n", tcp_state_name((tcp_state_t)i), counts[i]);
    }
}

// #297 stress: fire N sequential Kimi POSTs, printing status + TCP table after
// each so we can see exactly what exhausts/sticks on the Nth POST.
static void rc_cmd_kimix(rc_session_t *s, const char *arg) {
    while (*arg == ' ') arg++;
    int n = 0;
    while (*arg >= '0' && *arg <= '9') { n = n*10 + (*arg - '0'); arg++; }
    while (*arg == ' ') arg++;
    if (n <= 0) n = 10;
    const char *prompt = (*arg) ? arg : "Reply with the single word OK.";
    for (int i = 0; i < n; i++) {
        rc_printf(s, "--- kimix POST %d/%d ---\r\n", i+1, n); rc_flush(s);
        rc_cmd_tcpstat(s); rc_flush(s);
        rc_cmd_kimi(s, prompt);
        rc_flush(s);
    }
    rc_puts(s, "--- kimix done; final TCP table: ---\r\n");
    rc_cmd_tcpstat(s);
}


void rc_cmd_tcpstat_fwd(rc_session_t *s) { rc_cmd_tcpstat(s); }

// #297 stress: large-body POST loop. Builds a ~12KB user message (the AI ReAct
// loop's request body grows each turn with the full message history) and fires N
// of them, printing status + the TCP table after each so we can see whether the
// LARGE-body send path leaks sockets/PCBs or stalls on a full window.
static void rc_cmd_kimibig(rc_session_t *s, const char *arg) {
    extern int https_post(const char *url, const char *headers, const char *body,
                          uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern void rc_cmd_tcpstat_fwd(rc_session_t *s);
    while (*arg == ' ') arg++;
    int n = 0;
    while (*arg >= '0' && *arg <= '9') { n = n*10 + (*arg - '0'); arg++; }
    if (n <= 0) n = 10;

    uint32_t ksz = 0;
    char *kf = (char *)fat_read_file(&g_fat_fs, "/CONFIG/KIMI.KEY", &ksz);
    if (!kf || ksz == 0) { rc_puts(s, "kimibig: /CONFIG/KIMI.KEY missing\r\n"); if (kf) kfree(kf); return; }
    char key[128]; uint32_t ki = 0;
    for (uint32_t i = 0; i < ksz && ki < sizeof(key)-1; i++) { char c = kf[i]; if (c=='\r'||c=='\n') break; key[ki++] = c; }
    key[ki] = 0; kfree(kf);

    static char headers[256];
    int hl = 0;
    const char *h1 = "Authorization: Bearer ";
    for (const char *p = h1; *p; p++) headers[hl++] = *p;
    for (uint32_t i = 0; key[i]; i++) headers[hl++] = key[i];
    const char *h2 = "\r\nContent-Type: application/json\r\n";
    for (const char *p = h2; *p; p++) headers[hl++] = *p;
    headers[hl] = 0;

    // Build a ~12KB body: lots of padding context then a tiny instruction.
    static char body[16384];
    int bl = 0;
    const char *pre = "{\"model\":\"kimi-k2.6\",\"messages\":[{\"role\":\"system\",\"content\":\"";
    for (const char *p = pre; *p; p++) body[bl++] = *p;
    const char *pad = "You are a helpful assistant. Here is some background context that you may ignore. ";
    while (bl < 12000) { for (const char *p = pad; *p && bl < 12000; p++) body[bl++] = *p; }
    const char *mid = "\"},{\"role\":\"user\",\"content\":\"Reply with only the word OK.\"}],\"max_tokens\":8}";
    for (const char *p = mid; *p; p++) body[bl++] = *p;
    body[bl] = 0;

    rc_printf(s, "kimibig: body=%d bytes, %d POSTs\r\n", bl, n); rc_flush(s);
    for (int i = 0; i < n; i++) {
        rc_printf(s, "--- kimibig POST %d/%d ---\r\n", i+1, n); rc_flush(s);
        rc_cmd_tcpstat_fwd(s); rc_flush(s);
        uint8_t *resp = 0; uint32_t rlen = 0; int status = 0;
        int r = https_post("https://api.moonshot.ai/v1/chat/completions", headers, body, &resp, &rlen, &status);
        rc_printf(s, "kimibig: r=%d status=%d len=%u\r\n", r, status, rlen); rc_flush(s);
        if (resp) kfree(resp);
    }
    rc_puts(s, "--- kimibig done; final TCP table: ---\r\n");
    rc_cmd_tcpstat_fwd(s);
}

// task #317: run the SMB2/3 network-filesystem self-test (connect, negotiate,
// NTLM auth, tree connect, enumerate, read) against the server described in
// /CONFIG/SMBTEST.CFG. Full diagnostic goes to the serial log; a short summary
// goes back to the RC client. Runs in the fully-booted RC context where the
// net stack is known-good (HTTP fetch works here).
static void rc_cmd_smbtest(rc_session_t *s) {
    extern void kprintf_set_dual_output(int on);
    rc_puts(s, "Running SMB self-test (see serial log for full trace)...\r\n");
    rc_flush(s);
    kprintf_set_dual_output(1);
    smb_run_selftest();
    kprintf_set_dual_output(0);
    rc_puts(s, "SMB self-test complete (serial).\r\n");
}

static void rc_cmd_cat(rc_session_t *s, const char *path) {
    if (!path || !*path)    { rc_puts(s, "Usage: cat <path>\r\n");         return; }
    if (!g_fat_fs.mounted)  { rc_puts(s, "No filesystem mounted\r\n");     return; }
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data) { rc_printf(s, "Cannot read: %s\r\n", path); return; }
    rc_printf(s, "--- %s (%u bytes) ---\r\n", path, size);
    char *text = (char *)data;
    uint32_t limit = (size < 4096) ? size : 4096;
    for (uint32_t i = 0; i < limit; i++) {
        char c = text[i];
        if      (c == '\n') rc_puts(s, "\r\n");
        else if (c != '\r') { char t[2] = {c, 0}; rc_puts(s, t); }
    }
    if (size > 4096) rc_puts(s, "\r\n... (truncated)\r\n");
    rc_puts(s, "\r\n--- EOF ---\r\n");
    kfree(data);
}

static void rc_cmd_ls(rc_session_t *s, const char *path) {
    if (!path || !*path) path = "/";

    // task #317: list a network share via the SMB VFS routing.
    if (smb_vfs_is_smb_path(path)) {
        int dir = smb_vfs_opendir(path);
        if (dir < 0) { rc_printf(s, "SMB: cannot open %s\r\n", path); return; }
        smb_dirent_t de;
        int count = 0;
        while (smb_readdir(dir, &de) == 0 && count < 256) {
            if (de.name[0] == '.') continue;
            if (de.is_directory) rc_printf(s, "  [DIR]  %s\r\n", de.name);
            else                 rc_printf(s, "  %8u  %s\r\n", (unsigned)de.size, de.name);
            count++;
            if ((count & 7) == 0) rc_flush(s);
        }
        smb_closedir(dir);
        rc_printf(s, "Total: %d entries (SMB)\r\n", count);
        return;
    }

    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    fat_file_t dir;
    if (fat_open(&g_fat_fs, path, &dir) != 0) {
        rc_printf(s, "Cannot open: %s\r\n", path);
        return;
    }
    if (!dir.is_dir) {
        rc_printf(s, "Not a directory: %s\r\n", path);
        return;
    }

    fat_dir_entry_t entry;
    char name[256];
    int count = 0;
    while (fat_readdir(&dir, &entry, name) == 0) {
        if (!name[0] || name[0] == '.') continue;
        int is_dir2 = (entry.attr & 0x10) != 0;
        if (is_dir2) {
            rc_printf(s, "  [DIR]  %s\r\n", name);
        } else {
            rc_printf(s, "  %8u  %s\r\n", (unsigned)entry.file_size, name);
        }
        count++;
        if ((count & 7) == 0) rc_flush(s);
    }
    rc_printf(s, "Total: %d entries\r\n", count);
}

// (#196) disk-image mount/eject control + listing.
extern int  diskimg_mount(char letter, const char *imgpath);
extern void diskimg_eject(char letter);
extern int  diskimg_is_mounted(char letter);
extern int  diskimg_format(char letter);
extern const char *diskimg_mounted_name(char letter);
typedef void (*diskimg_dir_cb)(const char *name, int is_dir, unsigned int size, void *ud);
extern int  diskimg_listdir(char letter, const char *relpath, diskimg_dir_cb cb, void *ud);

static void rc_diskstatus(rc_session_t *s) {
    char drives[2] = { 'A', 'E' };
    for (int i = 0; i < 2; i++) {
        char d = drives[i];
        if (diskimg_is_mounted(d)) {
            int f = diskimg_format(d);
            rc_printf(s, "  %c: %s [%s]\r\n", d, diskimg_mounted_name(d),
                      f == 1 ? "ISO9660" : (f == 2 ? "FAT12" : "?"));
        } else {
            rc_printf(s, "  %c: (folder-backed, no image)\r\n", d);
        }
    }
}

static rc_session_t *g_dls_s;   // for the listdir callback
static void rc_dls_cb(const char *name, int is_dir, unsigned int size, void *ud) {
    (void)ud;
    if (is_dir) rc_printf(g_dls_s, "  [DIR]  %s\r\n", name);
    else        rc_printf(g_dls_s, "  %8u  %s\r\n", size, name);
}

static void rc_cmd_disk(rc_session_t *s, const char *arg) {
    // "disk"                       -> status
    // "disk mount <A|E> <imgpath>" -> mount
    // "disk eject <A|E>"           -> eject
    // "disk ls <A|E> [relpath]"    -> list the mounted image's directory
    if (!arg || !*arg) { rc_puts(s, "Mounted images:\r\n"); rc_diskstatus(s); return; }
    while (*arg == ' ') arg++;
    if (strncmp(arg, "mount ", 6) == 0) {
        const char *p = arg + 6; while (*p == ' ') p++;
        char drive = *p ? *p : 0; p++; while (*p == ' ') p++;
        if (!drive || !*p) { rc_puts(s, "Usage: disk mount <A|E> <imgpath>\r\n"); return; }
        int r = diskimg_mount(drive, p);
        if (r == 0) rc_printf(s, "Mounted %s on %c: [%s]\r\n", diskimg_mounted_name(drive),
                              drive, diskimg_format(drive) == 1 ? "ISO9660" : "FAT12");
        else rc_printf(s, "Mount failed (err %d): %s\r\n", r, p);
    } else if (strncmp(arg, "eject ", 6) == 0) {
        const char *p = arg + 6; while (*p == ' ') p++;
        if (!*p) { rc_puts(s, "Usage: disk eject <A|E>\r\n"); return; }
        diskimg_eject(*p);
        rc_printf(s, "Ejected %c:\r\n", *p);
    } else if (strncmp(arg, "ls ", 3) == 0) {
        const char *p = arg + 3; while (*p == ' ') p++;
        char drive = *p ? *p : 0; p++; while (*p == ' ') p++;
        if (!drive) { rc_puts(s, "Usage: disk ls <A|E> [relpath]\r\n"); return; }
        if (!diskimg_is_mounted(drive)) { rc_printf(s, "%c: has no image mounted\r\n", drive); return; }
        g_dls_s = s;
        int n = diskimg_listdir(drive, *p ? p : "", rc_dls_cb, 0);
        if (n < 0) rc_puts(s, "(cannot list - not a directory?)\r\n");
        else rc_printf(s, "Total: %d entries\r\n", n);
    } else {
        rc_puts(s, "Usage: disk [mount <A|E> <img> | eject <A|E> | ls <A|E> [path]]\r\n");
    }
}

static void rc_cmd_launch(rc_session_t *s, const char *path) {
    if (!path || !*path) { rc_puts(s, "Usage: launch <path>\r\n"); return; }
    while (*path == ' ') path++;
    rc_printf(s, "Launching %s...\r\n", path);
    rc_flush(s);

    // sys_exec() is a stub (returns -1). Spawn the ELF the same way the
    // desktop launches apps: read it, validate, and proc_create_user(). This
    // makes `launch` actually start GUI apps over the remote console.
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) { if (data) kfree(data); rc_printf(s, "launch: cannot read %s\r\n", path); return; }

    extern int elf_validate(const void *elf_data, uint32_t size);
    if (elf_validate(data, sz) != 0) { kfree(data); rc_printf(s, "launch: not a valid ELF: %s\r\n", path); return; }

    // Extract the basename for the process name.
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;

    int pid = proc_create_user(name, data, sz, 0, 0);
    kfree(data);
    if (pid > 0) rc_printf(s, "Launched %s (PID %d)\r\n", name, pid);
    else         rc_printf(s, "Launch failed (%d)\r\n", pid);
}

// #279: like launch, but routes the new user process to an application
// processor (any app, not just spin*). Compute-bound apps get a free core.
static void rc_cmd_launchap(rc_session_t *s, const char *path) {
    if (!path || !*path) { rc_puts(s, "Usage: launchap <path>\r\n"); return; }
    while (*path == ' ') path++;
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) { if (data) kfree(data); rc_printf(s, "launchap: cannot read %s\r\n", path); return; }
    extern int elf_validate(const void *elf_data, uint32_t size);
    if (elf_validate(data, sz) != 0) { kfree(data); rc_printf(s, "launchap: not a valid ELF: %s\r\n", path); return; }
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;
    extern void proc_set_next_migratable(int v);
    proc_set_next_migratable(1);
    int pid = proc_create_user(name, data, sz, 0, 0);
    kfree(data);
    if (pid > 0) rc_printf(s, "Launched %s on an AP (PID %d)\r\n", name, pid);
    else         rc_printf(s, "launchap failed (%d)\r\n", pid);
}

// ── shell: spawn a user-mode shell on a fresh PTY and pump bytes ──
// Opens /dev/ptmx, spawns the ELF (default /APPS/MSH) with /dev/pts/N wired
// to fds 0/1/2, then sits in a pump loop:
//   master_ready  -> tcp_send(client, bytes)
//   tcp has data  -> file_write(master, bytes)     (keystrokes -> ldisc)
// Exits when the child dies, the master hangs up, or the socket closes.
static void rc_cmd_shell(rc_session_t *s, const char *arg) {
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }

    // Skip leading whitespace in optional elf path argument.
    while (arg && *arg == ' ') arg++;
    const char *elf_path = (arg && *arg) ? arg : "/APPS/MSH";

    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, elf_path, &sz);
    if (!data || sz == 0) {
        rc_printf(s, "shell: cannot load %s\r\n", elf_path);
        if (data) kfree(data);
        return;
    }

    extern struct file *dev_open(const char *name, int flags);
    extern void file_put(struct file *f);
    extern int64_t file_read(struct file *f, void *buf, uint64_t count);
    extern int64_t file_write(struct file *f, const void *buf, uint64_t count);
    extern int file_ioctl(struct file *f, unsigned cmd, void *arg2);
    extern int file_poll(struct file *f, int events);

    // O_RDWR|O_NONBLOCK on master; we use poll+read so we don't block the
    // session thread while waiting on tty output.
    struct file *master = dev_open("ptmx", 0x0002 | 0x0800);
    if (!master) {
        rc_puts(s, "shell: /dev/ptmx unavailable\r\n");
        kfree(data);
        return;
    }

    int pts_idx = -1;
    // TIOCGPTN = 0x80045430 (matches drivers/pty.c)
    if (file_ioctl(master, 0x80045430, &pts_idx) != 0 || pts_idx < 0) {
        rc_puts(s, "shell: TIOCGPTN failed\r\n");
        file_put(master);
        kfree(data);
        return;
    }

    // Derive proc name from last path component.
    const char *procname = elf_path;
    for (const char *p = elf_path; *p; p++) if (*p == '/') procname = p + 1;

    int pid = proc_create_user_tty(procname, data, sz, pts_idx);
    kfree(data);
    if (pid < 0) {
        rc_puts(s, "shell: spawn failed\r\n");
        file_put(master);
        return;
    }

    rc_printf(s, "[shell: spawned %s pid=%d on /dev/pts/%d]\r\n",
              procname, pid, pts_idx);
    rc_flush(s);

    process_t *child = proc_get((uint32_t)pid);
    uint8_t iobuf[512];
    int child_gone = 0;

    for (;;) {
        tcp_state_t st = tcp_get_state(s->sock);
        if (st == TCP_STATE_CLOSED     ||
            st == TCP_STATE_CLOSE_WAIT ||
            st == TCP_STATE_LAST_ACK) break;

        int did_work = 0;

        // Master -> TCP  (drain all available output)
        int mp = file_poll(master, 0x01 /*POLL_IN*/);
        if (mp & 0x01) {
            int64_t n = file_read(master, iobuf, sizeof(iobuf));
            if (n > 0) {
                int sent = 0;
                while (sent < (int)n) {
                    int r = tcp_send(s->sock,
                                     (const char *)(iobuf + sent),
                                     (uint16_t)(n - sent));
                    if (r <= 0) { sent = -1; break; }
                    sent += r;
                }
                if (sent < 0) break;
                did_work = 1;
            } else if (n == 0) {
                // slave closed -> EOF. Child will exit; wait one more tick.
            }
        } else if (mp & 0x10 /*POLL_HUP*/) {
            break;
        }

        // TCP -> Master  (best-effort non-blocking read)
        int rn = tcp_recv(s->sock, (char *)iobuf, sizeof(iobuf));
        if (rn > 0) {
            file_write(master, iobuf, (uint64_t)rn);
            did_work = 1;
        } else if (rn < 0) {
            break;
        }

        // #448/#449: don't stop the instant the child becomes a zombie - a fast
        // print-and-exit child leaves output buffered in the pty master. Finish
        // only once the child is gone AND a full pass produced no work.
        if (!did_work) {
            if (child_gone) break;
            child = proc_get((uint32_t)pid);
            if (!child || child->state == PROC_STATE_ZOMBIE ||
                child->state == PROC_STATE_UNUSED) { child_gone = 1; continue; }
            proc_sleep(5);
        }
    }

    // Dropping the master fires SIGHUP on the slave's fg_pgrp and wakes
    // any slave readers with EOF, letting the child tear down cleanly.
    file_put(master);
    // #264: reap the MSH child so it does not linger as a permanent zombie
    // (the leak that eventually filled the proc table and faulted spawns).
    // Give it a moment to finish exiting, then reclaim its slot.
    {
        extern int proc_reap(uint32_t pid);
        for (int w = 0; w < 40; w++) {
            child = proc_get((uint32_t)pid);
            if (!child || child->state == PROC_STATE_ZOMBIE ||
                child->state == PROC_STATE_UNUSED) break;
            proc_sleep(5);
        }
        proc_reap((uint32_t)pid);
    }
    rc_puts(s, "\r\n[shell exited]\r\n");
}

// ── run <path> [args]: non-interactive spawn + capture stdout + exit code ──
// The dev-bridge sibling of `shell`: spawns an arbitrary ELF on a fresh PTY,
// streams its stdout to the socket, and on exit prints a machine-parseable
// "[exit N]" line so a client (tools/mdev/mdev.py) can read the process's exit
// code. Unlike `shell` it does NOT feed socket bytes back as keystrokes (the
// child is expected to be a non-interactive, print-and-exit test binary). This
// is the "push binary -> run -> collect output" loop used to iterate against a
// running VM or real hardware without reburning the boot medium.
static void rc_cmd_run(rc_session_t *s, const char *arg) {
    if (!g_fat_fs.mounted) { rc_puts(s, "No filesystem mounted\r\n"); return; }
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg) { rc_puts(s, "Usage: run <path> [args]\r\n"); return; }

    // Split the path from optional trailing args (args are accepted for a
    // future-proof syntax; the ELF loader does not consume them yet).
    char path[128]; int i = 0;
    while (*arg && *arg != ' ' && i < (int)sizeof(path) - 1) path[i++] = *arg++;
    path[i] = '\0';

    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        rc_printf(s, "run: cannot read %s\r\n", path);
        return;
    }
    extern int elf_validate(const void *elf_data, uint32_t size);
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        rc_printf(s, "run: not a valid ELF: %s\r\n", path);
        return;
    }

    extern struct file *dev_open(const char *name, int flags);
    extern void file_put(struct file *f);
    extern int64_t file_read(struct file *f, void *buf, uint64_t count);
    extern int file_ioctl(struct file *f, unsigned cmd, void *arg2);
    extern int file_poll(struct file *f, int events);
    extern int64_t file_write(struct file *f, const void *buf, uint64_t count);

    struct file *master = dev_open("ptmx", 0x0002 | 0x0800);
    if (!master) { rc_puts(s, "run: /dev/ptmx unavailable\r\n"); kfree(data); return; }
    int pts_idx = -1;
    if (file_ioctl(master, 0x80045430, &pts_idx) != 0 || pts_idx < 0) {
        rc_puts(s, "run: TIOCGPTN failed\r\n"); file_put(master); kfree(data); return;
    }

    const char *procname = path;
    for (const char *p = path; *p; p++) if (*p == '/') procname = p + 1;

    int pid = proc_create_user_tty(procname, data, sz, pts_idx);
    kfree(data);
    if (pid < 0) { rc_puts(s, "run: spawn failed\r\n"); file_put(master); return; }

    rc_printf(s, "[run: %s pid=%d pts=%d]\r\n", procname, pid, pts_idx);
    rc_flush(s);

    process_t *child = proc_get((uint32_t)pid);
    uint8_t iobuf[512];
    int child_gone = 0;
    for (;;) {
        tcp_state_t st = tcp_get_state(s->sock);
        if (st == TCP_STATE_CLOSED     ||
            st == TCP_STATE_CLOSE_WAIT ||
            st == TCP_STATE_LAST_ACK) break;

        int did_work = 0;

        // Master -> TCP (drain all available stdout).
        int mp = file_poll(master, 0x01 /*POLL_IN*/);
        if (mp & 0x01) {
            int64_t n = file_read(master, iobuf, sizeof(iobuf));
            if (n > 0) {
                int sent = 0;
                while (sent < (int)n) {
                    int r = tcp_send(s->sock, (const char *)(iobuf + sent),
                                     (uint16_t)(n - sent));
                    if (r <= 0) { sent = -1; break; }
                    sent += r;
                }
                if (sent < 0) break;
                did_work = 1;
            }
        } else if (mp & 0x10 /*POLL_HUP*/) {
            break;          // slave fully closed, nothing left to read
        }

        // #448 fix: TCP -> Master. Forward the client's input to the child's
        // stdin. Without this a stdin-reading child (e.g. bc's REPL) blocks
        // forever on the pty and wedges this serve loop. Mirrors rc_cmd_shell;
        // on client disconnect the loop breaks and file_put(master) below fires
        // SIGHUP -> EOF, unblocking the child cleanly.
        int rn = tcp_recv(s->sock, (char *)iobuf, sizeof(iobuf));
        if (rn > 0) {
            file_write(master, iobuf, (uint64_t)rn);
            did_work = 1;
        } else if (rn < 0) {
            break;
        }

        // Stop only once the child is gone AND a full pass produced no work
        // (a fast print-and-exit child leaves output buffered in the master).
        if (!did_work) {
            if (child_gone) break;
            child = proc_get((uint32_t)pid);
            if (!child || child->state == PROC_STATE_ZOMBIE ||
                child->state == PROC_STATE_UNUSED) { child_gone = 1; continue; }
            proc_sleep(5);
        }
    }

    // Read the exit code from the (now zombie) child before reaping it.
    int code = -1;
    child = proc_get((uint32_t)pid);
    if (child && child->state == PROC_STATE_ZOMBIE) code = child->exit_code;

    file_put(master);
    {
        extern int proc_reap(uint32_t pid);
        for (int w = 0; w < 40; w++) {
            child = proc_get((uint32_t)pid);
            if (!child || child->state == PROC_STATE_ZOMBIE ||
                child->state == PROC_STATE_UNUSED) break;
            proc_sleep(5);
        }
        child = proc_get((uint32_t)pid);
        if (code < 0 && child && child->state == PROC_STATE_ZOMBIE)
            code = child->exit_code;
        proc_reap((uint32_t)pid);
    }
    rc_printf(s, "\r\n[exit %d]\r\n", code);
}

// ── nova <text> / novatest : Nova Guard prompt-injection scanner (#449) ──
static void rc_cmd_nova(rc_session_t *s, const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg) { rc_puts(s, "Usage: nova <text to scan>\r\n"); return; }
    nova_hit_t hits[NOVA_MAX_HITS];
    int n = nova_scan(arg, hits, NOVA_MAX_HITS);
    if (n == 0) { rc_puts(s, "NOVA: clean (no rules fired)\r\n"); return; }
    rc_printf(s, "NOVA: %d rule(s) fired (worst=%s)\r\n", n,
              nova_sev_name(nova_worst_severity(arg)));
    for (int i = 0; i < n && i < NOVA_MAX_HITS; i++)
        rc_printf(s, "  - %s [%s] %s (matched: %s)\r\n",
                  hits[i].rule, nova_sev_name(hits[i].severity),
                  hits[i].category, hits[i].matched);
}

static void rc_cmd_novatest(rc_session_t *s) {
    char rep[64];
    int fails = nova_selftest(rep, sizeof(rep));
    rc_printf(s, "%s (%d failure%s) - ruleset: %d rules\r\n",
              rep, fails, fails == 1 ? "" : "s", nova_rule_count());
    rc_printf(s, "attribution: %s\r\n", nova_attribution());
}

// Returns 1 if the session should close, 0 to keep alive
// #95: background services control.
//   svc            / svc list   - list services and their state
//   svc start <n>              - start a service
//   svc stop <n>               - stop a service
//   svc enable <n>             - mark a service enabled
//   svc disable <n>            - mark a service disabled (and stop it)
static void rc_cmd_svc(rc_session_t *s, const char *arg) {
    while (arg && *arg == ' ') arg++;

    if (!arg || !*arg || strcmp(arg, "list") == 0) {
        int n = svc_count();
        rc_printf(s, "Services (%d):\r\n", n);
        rc_puts(s, "  NAME             STATE    AUTO EN  ACCOUNT      PERMS\r\n");
        for (int i = 0; i < n; i++) {
            service_t *svc = svc_at(i);
            if (!svc) continue;
            char perms[48];
            svc_perms_str(svc->perms, perms, sizeof(perms));
            int running = svc_is_running(svc);
            rc_printf(s, "  %-16s %-8s %-4s %-3s %-12s %s\r\n",
                      svc->name,
                      running ? "running" : "stopped",
                      svc->autostart ? "yes" : "no",
                      svc->enabled ? "yes" : "no",
                      svc->account, perms);
        }
        return;
    }

    // Split verb and target name.
    char verb[16]; int v = 0;
    while (*arg && *arg != ' ' && v < (int)sizeof(verb) - 1) verb[v++] = *arg++;
    verb[v] = '\0';
    while (*arg == ' ') arg++;
    const char *name = arg;

    if (!*name) { rc_printf(s, "Usage: svc %s <name>\r\n", verb); return; }

    if (strcmp(verb, "start") == 0) {
        int r = svc_start(name);
        if (r > 0) rc_printf(s, "started '%s' (pid %d)\r\n", name, r);
        else       rc_printf(s, "start '%s' failed (%d)\r\n", name, r);
    } else if (strcmp(verb, "stop") == 0) {
        int r = svc_stop(name);
        rc_printf(s, r == 0 ? "stopped '%s'\r\n" : "stop '%s' failed\r\n", name);
    } else if (strcmp(verb, "enable") == 0) {
        int r = svc_enable(name, 1);
        rc_printf(s, r == 0 ? "enabled '%s'\r\n" : "no such service '%s'\r\n", name);
    } else if (strcmp(verb, "disable") == 0) {
        int r = svc_enable(name, 0);
        rc_printf(s, r == 0 ? "disabled '%s'\r\n" : "no such service '%s'\r\n", name);
    } else {
        rc_printf(s, "Unknown svc subcommand '%s'\r\n", verb);
    }
}

static int rc_dispatch(rc_session_t *s, char *line) {
    while (*line == ' ') line++;   // trim leading whitespace
    if (!*line) return 0;          // blank line

    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) return 1;

    if (strcmp(line, "help") == 0) {
        rc_cmd_help(s);
    } else if (strcmp(line, "mem") == 0) {
        uint64_t total_mb = g_boot_info ? g_boot_info->total_memory / (1024*1024) : 0;
        rc_printf(s, "Total RAM: %lu MB\r\n", total_mb);
    } else if (strcmp(line, "pmm") == 0) {
        rc_puts(s, "[pmm] -> serial log\r\n"); rc_flush(s);
        pmm_print_stats();
    } else if (strcmp(line, "heap") == 0) {
        rc_puts(s, "[heap] -> serial log\r\n"); rc_flush(s);
        heap_print_stats();
    } else if (strcmp(line, "ticks") == 0) {
        rc_printf(s, "Ticks: %lu  (timer: %u Hz)\r\n",
                  (unsigned long)timer_ticks, (unsigned)g_timer_hz);
    } else if (strcmp(line, "ps") == 0) {
        rc_puts(s, "[ps] -> serial log\r\n"); rc_flush(s);
        proc_print_list();
    } else if (strcmp(line, "net") == 0) {
        rc_cmd_net(s);
    } else if (strncmp(line, "fetch ", 6) == 0) {
        rc_cmd_fetch(s, line + 6);
    } else if (strncmp(line, "kimi ", 5) == 0) {
        rc_cmd_kimi(s, line + 5);
    } else if (strcmp(line, "tcpstat") == 0) {
        rc_cmd_tcpstat(s);
    } else if (strncmp(line, "kimix ", 6) == 0) {
        rc_cmd_kimix(s, line + 6);
    } else if (strcmp(line, "kimix") == 0) {
        rc_cmd_kimix(s, "10 Reply with the single word OK.");
    } else if (strncmp(line, "kimibig ", 8) == 0) {
        rc_cmd_kimibig(s, line + 8);
    } else if (strcmp(line, "kimibig") == 0) {
        rc_cmd_kimibig(s, "10");
    } else if (strncmp(line, "ssh ", 4) == 0) {
        rc_cmd_ssh(s, line + 4);
    } else if (strcmp(line, "ping") == 0) {
        rc_cmd_ping(s, NULL);
    } else if (strncmp(line, "ping ", 5) == 0) {
        rc_cmd_ping(s, line + 5);
    } else if (strncmp(line, "ls", 2) == 0 &&
               (line[2] == '\0' || line[2] == ' ')) {
        rc_cmd_ls(s, (line[2] == ' ') ? line+3 : "/");
    } else if (strcmp(line, "smbtest") == 0) {
        rc_cmd_smbtest(s);
    } else if (strncmp(line, "cat ", 4) == 0) {
        rc_cmd_cat(s, line + 4);
    } else if (strcmp(line, "disk") == 0) {
        rc_puts(s, "Mounted images:\r\n"); rc_diskstatus(s);
    } else if (strncmp(line, "disk ", 5) == 0) {
        rc_cmd_disk(s, line + 5);
    } else if (strncmp(line, "osinstall", 9) == 0 &&
               (line[9] == '\0' || line[9] == ' ')) {
        rc_cmd_osinstall(s, line[9] == ' ' ? line + 10 : "");
    } else if (strncmp(line, "launch ", 7) == 0) {
        rc_cmd_launch(s, line + 7);
    } else if (strncmp(line, "launchap ", 9) == 0) {
        rc_cmd_launchap(s, line + 9);
    } else if (strncmp(line, "click ", 6) == 0) {
        /* Test helper: inject a left mouse click at absolute screen x y into the
         * window manager (same path the compositor uses), so click-dependent
         * features can be exercised headlessly over RC. */
        const char *p = line + 6;
        int x = 0, y = 0, sx = 0;
        while (*p == ' ') p++;
        if (*p == '-') { sx = 1; p++; }
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); p++; }
        if (sx) x = -x;
        while (*p == ' ') p++;
        sx = 0;
        if (*p == '-') { sx = 1; p++; }
        while (*p >= '0' && *p <= '9') { y = y * 10 + (*p - '0'); p++; }
        if (sx) y = -y;
        extern void mouse_inject_button(int32_t, int32_t, int);
        extern void proc_sleep(uint32_t);
        mouse_inject_button(x, y, 0);   /* position cursor, button released */
        proc_sleep(60);
        mouse_inject_button(x, y, 1);   /* press */
        proc_sleep(160);                /* hold across several compositor frames */
        mouse_inject_button(x, y, 0);   /* release */
        proc_sleep(60);
        rc_printf(s, "click %d %d\r\n", x, y);
    } else if (strncmp(line, "scroll ", 7) == 0) {
        /* Test helper: inject a mouse-wheel scroll at absolute x y with a signed
         * delta into the WM (same EVENT_MOUSE_SCROLL path the compositor uses),
         * so wheel scrolling can be exercised headlessly over RC. */
        const char *p = line + 7;
        int x = 0, y = 0, d = 0, sx = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { x = x*10 + (*p - '0'); p++; }
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { y = y*10 + (*p - '0'); p++; }
        while (*p == ' ') p++;
        if (*p == '-') { sx = 1; p++; }
        while (*p >= '0' && *p <= '9') { d = d*10 + (*p - '0'); p++; }
        if (sx) d = -d;
        extern void wm_inject_app_scroll(int32_t, int32_t, int32_t);
        wm_inject_app_scroll(x, y, d);
        rc_printf(s, "scroll %d %d %d\r\n", x, y, d);
    } else if (strncmp(line, "win16 ", 6) == 0) {
        // Non-blocking launch in its own kernel process (#144): the desktop and
        // this RC session stay responsive while the Win16 app runs in a window.
        extern int win16_launch(const char *path);
        int wr = win16_launch(line + 6);
        rc_printf(s, "win16: %s %s\r\n",
                  (wr == 0) ? "launched" : "failed (busy or bad path)", line + 6);
    } else if (strncmp(line, "dos ", 4) == 0) {
        extern int dos_launch(const char *path);
        int dr = dos_launch(line + 4);
        rc_printf(s, "dos: %s %s\r\n",
                  (dr == 0) ? "launched" : "failed (busy or bad path)", line + 4);
    } else if (strncmp(line, "win16install ", 13) == 0) {
        rc_cmd_win16install(s, line + 13);
    } else if (strcmp(line, "sshd") == 0) {
        extern void ssh_server_start(void);
        ssh_server_start();
        rc_puts(s, "sshd: started (listening on port 22; needs /CONFIG/SSHHOST.KEY)\r\n");
    } else if (strcmp(line, "sshgui") == 0) {
        extern void sshterm_launch(void);
        sshterm_launch();
        rc_puts(s, "sshgui: launched (reads /CONFIG/SSH.CFG)\r\n");
    } else if (strcmp(line, "win16test") == 0) {
        rc_puts(s, "win16: interpreter selftest -> console\r\n"); rc_flush(s);
        int t = x86_16_selftest();
        rc_printf(s, "win16: selftest %s\r\n", t == 0 ? "PASS" : "FAIL");
    } else if (strcmp(line, "shell") == 0) {
        rc_cmd_shell(s, NULL);
    } else if (strncmp(line, "shell ", 6) == 0) {
        rc_cmd_shell(s, line + 6);
    } else if (strncmp(line, "run ", 4) == 0) {
        rc_cmd_run(s, line + 4);
    } else if (strcmp(line, "run") == 0) {
        rc_puts(s, "Usage: run <path> [args]\r\n");
    } else if (strncmp(line, "nova ", 5) == 0) {
        rc_cmd_nova(s, line + 5);
    } else if (strcmp(line, "novatest") == 0) {
        rc_cmd_novatest(s);
    } else if (strncmp(line, "upload ", 7) == 0) {
        rc_cmd_upload(s, line + 7);
    } else if (strncmp(line, "install ", 8) == 0) {
        rc_cmd_install(s, line + 8);
    } else if (strncmp(line, "uninstall ", 10) == 0) {
        rc_cmd_uninstall(s, line + 10);
    } else if (strcmp(line, "apps") == 0) {
        rc_cmd_apps(s);
    } else if (strcmp(line, "svc") == 0) {
        rc_cmd_svc(s, NULL);
    } else if (strncmp(line, "svc ", 4) == 0) {
        rc_cmd_svc(s, line + 4);
    } else if (strcmp(line, "reload") == 0) {
        desktop_menu_reload();
        rc_puts(s, "menu reloaded\r\n");
    } else if (strcmp(line, "reboot") == 0) {
        rc_puts(s, "Rebooting...\r\n"); rc_flush(s);
        acpi_reboot();
    } else if (strcmp(line, "shutdown") == 0) {
        rc_puts(s, "Shutting down...\r\n"); rc_flush(s);
        acpi_shutdown();
    } else {
        rc_printf(s, "Unknown: '%s'  (type 'help', or 'shell' for userland tools)\r\n", line);
    }
    return 0;
}

// ── session loop ───────────────────────────────────────────────────────────

// Constant-time string comparison (prevents timing side-channels)
static int rc_streq(const char *a, const char *b) {
    int diff = 0;
    while (*a || *b) {
        diff |= (unsigned char)*a ^ (unsigned char)*b;
        if (*a) a++;
        if (*b) b++;
    }
    return diff == 0;
}

// Prompt for username/password. Returns 1 on success, 0 on failure/timeout.
static int rc_authenticate(rc_session_t *s) {
    char user[64], pass[64];

    for (int attempt = 0; attempt < REMOTE_CTRL_MAX_ATTEMPTS; attempt++) {
        // Username
        rc_puts(s, "Username: ");
        rc_flush(s);
        uint64_t deadline = timer_ticks + (uint64_t)(g_timer_hz * 30);
        int r;
        do {
            r = rc_readline(s, user, (int)sizeof(user));
            if (r < 0) return 0;
            if (timer_ticks > deadline) {
                rc_puts(s, "\r\nTimeout.\r\n"); rc_flush(s); return 0;
            }
            if (r == 0) proc_sleep(5);
        } while (r == 0);

        // Password
        rc_puts(s, "Password: ");
        rc_flush(s);
        deadline = timer_ticks + (uint64_t)(g_timer_hz * 30);
        do {
            r = rc_readline(s, pass, (int)sizeof(pass));
            if (r < 0) return 0;
            if (timer_ticks > deadline) {
                rc_puts(s, "\r\nTimeout.\r\n"); rc_flush(s); return 0;
            }
            if (r == 0) proc_sleep(5);
        } while (r == 0);

        if (rc_streq(user, REMOTE_CTRL_USER) && rc_streq(pass, REMOTE_CTRL_PASS)) {
            rc_puts(s, "\r\nWelcome.\r\n");
            rc_flush(s);
            return 1;
        }

        rc_puts(s, "\r\nInvalid credentials.\r\n");
        rc_flush(s);
        proc_sleep(2000);  // slow down brute force
    }

    rc_puts(s, "Too many failures.\r\n");
    rc_flush(s);
    return 0;
}

static void rc_handle_session(int client) {
    rc_session_t s;
    s.sock     = client;
    s.outpos   = 0;
    s.rbuf_len = 0;

    rc_puts(&s, "\r\nMayteraOS Remote Shell v1.0\r\n");
    rc_flush(&s);

    if (!rc_authenticate(&s)) {
        tcp_close(client);
        kprintf("[RCTRL] Authentication failed - connection closed\n");
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
        if (r == 0) { proc_sleep(5); continue; }

        int close = rc_dispatch(&s, line);
        if (close) {
            rc_puts(&s, "Bye!\r\n");
            rc_flush(&s);
            break;
        }
        rc_puts(&s, "maytera# ");
        rc_flush(&s);
    }

    tcp_close(client);
    kprintf("[RCTRL] Session closed\n");
}

// ── kernel thread ──────────────────────────────────────────────────────────

static void remote_ctrl_thread(void *arg) {
    (void)arg;
    kprintf("[RCTRL] Remote control starting on TCP port %d\n", REMOTE_CTRL_PORT);

    // Wait for network to settle after boot (3 s)
    uint64_t ready = timer_ticks + (uint64_t)(g_timer_hz * 3);
    while (timer_ticks < ready) {
        proc_sleep(5);
    }

    int srv = tcp_socket();
    if (srv < 0) { kprintf("[RCTRL] tcp_socket failed (%d)\n", srv); return; }

    if (tcp_bind(srv, REMOTE_CTRL_PORT) < 0) {
        kprintf("[RCTRL] tcp_bind(%d) failed\n", REMOTE_CTRL_PORT);
        tcp_close(srv); return;
    }
    if (tcp_listen(srv, 4) < 0) {
        kprintf("[RCTRL] tcp_listen failed\n");
        tcp_close(srv); return;
    }

    kprintf("[RCTRL] Listening on 0.0.0.0:%d\n", REMOTE_CTRL_PORT);

    for (;;) {
        int client = tcp_accept(srv);
        if (client >= 0) {
            kprintf("[RCTRL] Connection accepted (sock=%d)\n", client);
            rc_handle_session(client);
        }
        net_poll(); tcp_timer();   // pump the stack between accepts
        proc_sleep(50);            // idle accept poll backed off 10ms -> 50ms
    }
}

// ── public API ────────────────────────────────────────────────────────────

void remote_ctrl_init(void) {
    int pid = proc_create("RemoteCtrl", remote_ctrl_thread, NULL, PRIO_LOW);
    if (pid < 0) kprintf("[RCTRL] Failed to create thread (%d)\n", pid);
    else         kprintf("[RCTRL] Thread created (PID %d)\n", pid);
}
