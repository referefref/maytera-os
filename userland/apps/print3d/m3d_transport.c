// m3d_transport.c - see m3d_transport.h.
#include "m3d_transport.h"

#include <string.h>
#include "syscall.h"   // sys_open/sys_write/sys_close, O_* via fcntl semantics
#include "fcntl.h"

static void hexline(const uint8_t *b, int n, char *out) {
    static const char *hx = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < n; i++) { out[o++] = hx[(b[i] >> 4) & 0xF]; out[o++] = hx[b[i] & 0xF]; }
    out[o] = 0;
}

int m3d_transport_open(m3d_transport_t *t, m3d_backend_t backend, const char *target) {
    memset(t, 0, sizeof(*t));
    t->backend = backend;
    strncpy(t->target, target, sizeof(t->target) - 1);

    if (backend == M3D_BACKEND_VIRTUAL) {
        int fd = sys_open(target, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) return -1;
        t->fd = fd;
        t->open = 1;
        const char *hdr = "; MayteraOS 3D Print - virtual M3D printer log (#396)\n"
                          "; each entry: <ascii command> | <binary M3D packet hex>\n";
        sys_write(fd, hdr, (unsigned long)strlen(hdr));
        return 0;
    }

    // ----- real USB CDC-ACM backend (Micro: M3D_BACKEND_USB, Pro: M3D_BACKEND_USB_PRO) -----
    // Open the printer's USB serial device. The kernel CDC-ACM driver
    // (drivers/usb_cdc_acm.c, #396) enumerates the printer (Micro 03eb:2404;
    // Pro 0483:a21e), does the ACM line-coding + DTR|RTS setup at 115200 8N1,
    // and exposes it as /dev/ttyACM0. The Pro is silent until DTR|RTS is
    // asserted, which the driver already does on open (PROTOCOL.md #406 sec 5).
    // sys_open routes /dev/* to the in-kernel dev namespace and installs a
    // file_t, so sys_read/sys_write dispatch to the driver's bounded bulk-IN/OUT.
    // `target` is the device path ("/dev/ttyACM0"); default it if empty.
    {
        const char *dev = (target && target[0]) ? target : "/dev/ttyACM0";
        int fd = sys_open(dev, O_RDWR);
        if (fd < 0) return -1;   // no printer / driver not present
        t->fd = fd;
        t->open = 1;
        return 0;
    }
}

int m3d_transport_send(m3d_transport_t *t, const m3d_gcode_t *g) {
    if (!t->open) return -1;
    uint8_t packet[512];
    // The M3D Pro speaks Repetier "Binary V2"; the Micro (and the virtual log,
    // which mirrors the Micro stream) speak the M3D Micro binary framing.
    int n = (t->backend == M3D_BACKEND_USB_PRO)
                ? m3d_gcode_get_binary_v2(g, packet, sizeof(packet))
                : m3d_gcode_get_binary(g, packet, sizeof(packet));
    if (n < 0) return -1;

    if (t->backend == M3D_BACKEND_VIRTUAL) {
        char ascii[GC_LINE_MAX * 2];
        m3d_gcode_get_ascii(g, ascii);
        char hex[512 * 2 + 1];
        hexline(packet, n, hex);
        char line[GC_LINE_MAX * 2 + 1100];
        int o = 0;
        for (const char *p = ascii; *p; p++) line[o++] = *p;
        line[o++] = ' '; line[o++] = '|'; line[o++] = ' ';
        for (const char *p = hex; *p; p++) line[o++] = *p;
        line[o++] = '\n';
        sys_write(t->fd, line, (unsigned long)o);
    } else {
        // USB CDC-ACM: send the framed M3D binary packet over the serial link.
        long w = sys_write(t->fd, packet, (unsigned long)n);
        if (w < 0) return -1;
    }
    t->sent_commands++;
    t->sent_bytes += (unsigned long)n;
    return n;
}

int m3d_transport_send_line(m3d_transport_t *t, const char *ascii_line) {
    m3d_gcode_t g;
    if (!m3d_gcode_parse(&g, ascii_line)) return 0;   // blank/comment: nothing to send
    return m3d_transport_send(t, &g);
}

long m3d_transport_read(m3d_transport_t *t, char *buf, int maxlen) {
    if (!t->open) return -1;
    if (t->backend == M3D_BACKEND_VIRTUAL) { (void)buf; (void)maxlen; return 0; }
    // USB CDC-ACM: the kernel read is BOUNDED (short per-call timeout), so this
    // returns promptly with 0 when the printer has sent nothing (never blocks
    // on a full USB timeout). Caller polls/accumulates the printer's response.
    return sys_read(t->fd, buf, (unsigned long)maxlen);
}

// Scan an accumulated M115 reply. Sets *mode/*boot_version/*is_pro from what is
// present. Returns 1 once a definite mode (bootloader or firmware) is seen.
static int scan_handshake(const char *buf, int len, m3d_mode_t *mode,
                          int *boot_version, int *is_pro) {
    // Bootloader reply: 'B' immediately followed by one or more digits.
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == 'B' && buf[i + 1] >= '0' && buf[i + 1] <= '9') {
            int v = 0, j = i + 1;
            while (j < len && buf[j] >= '0' && buf[j] <= '9') { v = v * 10 + (buf[j] - '0'); j++; }
            *mode = M3D_MODE_BOOTLOADER;
            *boot_version = v;
            *is_pro = 1;   // the STM32 single-char bootloader is the Pro/Micro+ family
            return 1;
        }
    }
    // Firmware reply: contains "ok".
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == 'o' && buf[i + 1] == 'k') {
            *mode = M3D_MODE_FIRMWARE;
            return 1;
        }
    }
    return 0;
}

m3d_mode_t m3d_transport_connect(m3d_transport_t *t) {
    t->mode = M3D_MODE_UNKNOWN;
    t->boot_version = 0;

    if (!t->open) return M3D_MODE_UNKNOWN;
    if (t->backend == M3D_BACKEND_VIRTUAL) { t->mode = M3D_MODE_FIRMWARE; return t->mode; }

    // Detect the Pro from the banner: MACHINE_TYPE / "Pro" / Repetier PROTOCOL:2.
    // (The Pro is silent until DTR|RTS is asserted, which the kernel driver does
    // on open.) Send M115 up to a few times, accumulating the bounded reads.
    char buf[512];
    int len = 0;
    for (int attempt = 0; attempt < 8 && len < (int)sizeof(buf) - 1; attempt++) {
        sys_write(t->fd, "M115\r\n", 6);
        for (int poll = 0; poll < 4 && len < (int)sizeof(buf) - 1; poll++) {
            long r = m3d_transport_read(t, buf + len, (int)sizeof(buf) - 1 - len);
            if (r > 0) {
                len += (int)r;
                buf[len] = 0;
                if (scan_handshake(buf, len, &t->mode, &t->boot_version, &t->is_pro)) {
                    // If firmware, refine Pro detection from the banner text.
                    if (t->mode == M3D_MODE_FIRMWARE) {
                        for (int i = 0; i + 2 < len; i++) {
                            if ((buf[i] == 'P' && buf[i+1] == 'r' && buf[i+2] == 'o') ||
                                (i + 9 < len && !strncmp(buf + i, "PROTOCOL:2", 10)))
                                { t->is_pro = 1; break; }
                        }
                    }
                    return t->mode;
                }
            }
        }
    }
    return t->mode;
}

void m3d_transport_close(m3d_transport_t *t) {
    if (t->open) sys_close(t->fd);
    t->open = 0;
}
