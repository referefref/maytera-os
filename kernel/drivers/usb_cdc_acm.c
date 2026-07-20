// usb_cdc_acm.c - USB CDC-ACM serial class driver (#396). See usb_cdc_acm.h.
//
// Model (mirrors the USB HID / CDC-ECM non-blocking pattern from #307/#362):
//   - Enumerate: find the CDC communications interface (class 0x02 subclass 0x02
//     ACM) and the CDC data interface (class 0x0A) in the configuration
//     descriptor; claim the data interface's bulk-IN + bulk-OUT and the comm
//     interface's interrupt-IN notification endpoint.
//   - ACM init: SET_CONFIGURATION, then the class control requests
//     SET_LINE_CODING (115200 8N1) and SET_CONTROL_LINE_STATE (DTR|RTS). The
//     Atmel M3D bootloader ignores the baud but wants DTR asserted.
//   - TX: copy into a DMA bounce buffer, submit one bulk-OUT TD, wait (bounded).
//   - RX: submit one bulk-IN TD sized to the RX buffer; a short packet
//     terminates the transfer. Every wait is BOUNDED (xhci_delay_ms, 1ms/iter)
//     so a silent printer can never block on the full xHCI timeout.
//
// M3D Micro command reference (from the M33-Fio project, the build server:/root/m3d-ref):
//   Bootloader (single characters):  'S' = dump the 0x301-byte EEPROM (ends in
//     '\r'); firmwareVersion is the little-endian u32 at offset 0x00, the
//     serial number is ASCII near the end.  'Q' = leave the bootloader and run
//     the firmware (the device then RE-ENUMERATES on USB).
//   Firmware (iMe = ASCII RepRap gcode with a line number + XOR checksum):
//     "N0 M110*<chk>\n" resets the line number, "N1 G28*<chk>\n" homes, etc.
#include "usb_cdc_acm.h"
#include "dev.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../fs/bootlog.h"
#include "../fs/vfs.h"
#include "../fs/fat.h"

// -----------------------------------------------------------------------------
// Device state (single CDC-ACM serial port supported)
// -----------------------------------------------------------------------------
typedef struct {
    xhci_controller_t *xhc;
    int      slot_id;
    int      speed;
    int      comm_if;      // CDC communications interface number
    int      data_if;      // CDC data interface number
    int      in_dci;       // bulk-IN device context index
    int      out_dci;      // bulk-OUT device context index
    int      notify_dci;   // interrupt-IN notification DCI (-1 if none)
    int      in_mps;
    int      out_mps;
    uint8_t *rx_buf;
    uint32_t rx_buf_len;
    uint8_t *tx_buf;
    uint32_t tx_buf_len;
    int      in_armed;     // is a bulk-IN TD outstanding?
    uint32_t in_want;      // requested length of the outstanding IN
    uint16_t vid, pid;
    int      active;
} cdc_acm_t;

static cdc_acm_t g_cdc;

int usb_cdc_acm_present(void) { return g_cdc.active; }

// -----------------------------------------------------------------------------
// Bulk transfer helpers
// -----------------------------------------------------------------------------
int usb_cdc_acm_write(const void *data, uint32_t len, uint32_t timeout_ms) {
    cdc_acm_t *d = &g_cdc;
    if (!d->active || !data) return -1;
    if (len == 0) return 0;
    if (len > d->tx_buf_len) len = d->tx_buf_len;

    // Consume any stale completion left on the OUT DCI so it can't satisfy this
    // wait (e.g. a previous timed-out TX that completed late).
    uint32_t scrap = 0;
    (void)xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci, &scrap, 0);

    memcpy(d->tx_buf, data, len);
    if (xhci_int_in_submit(d->xhc, d->slot_id, d->out_dci,
                           (uint64_t)d->tx_buf, len) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < timeout_ms; i++) {
        uint32_t done = 0;
        int r = xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci, &done, len);
        if (r > 0) return (int)len;
        if (r < 0) return -1;
        xhci_delay_ms(1);
    }
    return -1;   // timeout
}

int usb_cdc_acm_read(void *buf, uint32_t maxlen, uint32_t timeout_ms) {
    cdc_acm_t *d = &g_cdc;
    if (!d->active || !buf || maxlen == 0) return -1;

    // Keep exactly one bulk-IN TD outstanding. Always request rx_buf_len so the
    // outstanding size is stable across calls; a short packet from the device
    // terminates the transfer early.
    if (!d->in_armed) {
        if (xhci_int_in_submit(d->xhc, d->slot_id, d->in_dci,
                               (uint64_t)d->rx_buf, d->rx_buf_len) != 0) {
            return -1;
        }
        d->in_armed = 1;
        d->in_want = d->rx_buf_len;
    }

    for (uint32_t i = 0; i < timeout_ms; i++) {
        uint32_t got = 0;
        int r = xhci_int_in_poll(d->xhc, d->slot_id, d->in_dci, &got, d->in_want);
        if (r > 0) {
            d->in_armed = 0;
            uint32_t n = (got < maxlen) ? got : maxlen;
            if (n) memcpy(buf, d->rx_buf, n);
            return (int)n;
        }
        if (r < 0) {
            d->in_armed = 0;
            return -1;
        }
        xhci_delay_ms(1);
    }
    return 0;   // bounded timeout; TD stays armed for the next call
}

// -----------------------------------------------------------------------------
// ACM class control requests
// -----------------------------------------------------------------------------
static int cdc_set_line_coding(cdc_acm_t *d, uint32_t baud, uint8_t stop,
                               uint8_t parity, uint8_t data_bits) {
    static uint8_t lc[7] __attribute__((aligned(64)));
    lc[0] = (uint8_t)(baud & 0xFF);
    lc[1] = (uint8_t)((baud >> 8) & 0xFF);
    lc[2] = (uint8_t)((baud >> 16) & 0xFF);
    lc[3] = (uint8_t)((baud >> 24) & 0xFF);
    lc[4] = stop;        // 0 = 1 stop bit
    lc[5] = parity;      // 0 = none
    lc[6] = data_bits;   // 8
    // SET_LINE_CODING: bmRequestType 0x21 (host->dev, class, interface),
    // bRequest 0x20, wValue 0, wIndex = comm interface.
    int cc = xhci_control_transfer(d->xhc, d->slot_id, 0x21, 0x20, 0,
                                   (uint16_t)d->comm_if, lc, 7);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -1;
}

static int cdc_set_control_line_state(cdc_acm_t *d, int dtr, int rts) {
    uint16_t v = (dtr ? 0x01 : 0) | (rts ? 0x02 : 0);
    // SET_CONTROL_LINE_STATE: bmRequestType 0x21, bRequest 0x22, no data.
    int cc = xhci_control_transfer(d->xhc, d->slot_id, 0x21, 0x22, v,
                                   (uint16_t)d->comm_if, NULL, 0);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -1;
}

// -----------------------------------------------------------------------------
// Configuration descriptor parse + attach
// -----------------------------------------------------------------------------
// Does this configuration contain a CDC-ACM (comm class 0x02 subclass 0x02)
// interface? Returns its bInterfaceNumber, or -1.
static int cfg_find_acm_iface(uint8_t *cfg, int total) {
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9 &&
            cfg[i + 5] == 0x02 && cfg[i + 6] == 0x02) {
            return cfg[i + 2];   // bInterfaceNumber
        }
        i += blen;
    }
    return -1;
}

int usb_cdc_acm_probe(xhci_controller_t *xhc, int slot_id, int speed,
                      uint16_t vid, uint16_t pid, uint8_t *cfg, int total) {
    if (g_cdc.active) return 0;   // one serial port supported

    int comm_if = cfg_find_acm_iface(cfg, total);
    if (comm_if < 0) return 0;    // not a CDC-ACM device

    int cfg_value = (total >= 6) ? cfg[5] : 1;

    // Union functional descriptor (0x24 subtype 0x06) names the data interface;
    // default to comm_if + 1 per the ACM spec.
    int data_if = comm_if + 1;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x24 && blen >= 5 && cfg[i + 2] == 0x06)
            data_if = cfg[i + 4];   // bSubordinateInterface0
        i += blen;
    }

    // Walk the endpoints: the comm interface carries an interrupt-IN
    // notification EP, the data interface carries the bulk pair.
    int ep_in = -1, ep_out = -1, ep_notify = -1;
    int in_mps = 0, out_mps = 0, notify_mps = 0;
    int cur_if = -1;
    i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {
            cur_if = cfg[i + 2];
        } else if (btype == 0x05 && blen >= 7) {
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            if (cur_if == data_if && eattr == 0x02) {          // bulk
                if (eaddr & 0x80) { ep_in = eaddr; in_mps = emps; }
                else              { ep_out = eaddr; out_mps = emps; }
            } else if (cur_if == comm_if && eattr == 0x03 &&
                       (eaddr & 0x80)) {                        // interrupt-IN
                ep_notify = eaddr; notify_mps = emps;
            }
        }
        i += blen;
    }

    if (ep_in < 0 || ep_out < 0) {
        kprintf("[CDC-ACM] no bulk endpoint pair on data interface %d\n", data_if);
        return 0;
    }

    kprintf("[CDC-ACM] %04x:%04x comm if %d data if %d bulk-in 0x%02x/%d "
            "bulk-out 0x%02x/%d notify 0x%02x cfg %d\n",
            vid, pid, comm_if, data_if, ep_in, in_mps, ep_out, out_mps,
            ep_notify < 0 ? 0 : ep_notify, cfg_value);

    cdc_acm_t *d = &g_cdc;
    memset(d, 0, sizeof(*d));
    d->xhc = xhc;
    d->slot_id = slot_id;
    d->speed = speed;
    d->comm_if = comm_if;
    d->data_if = data_if;
    d->notify_dci = -1;
    d->vid = vid;
    d->pid = pid;

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfg_value) < 0) {
        kprintf("[CDC-ACM] SET_CONFIGURATION %d failed\n", cfg_value);
        return 0;
    }

    // Bulk endpoints.
    int in_dci = xhci_configure_endpoint_ep(xhc, slot_id, ep_in,
                                            EP_TYPE_BULK_IN, in_mps, 0, speed);
    int out_dci = xhci_configure_endpoint_ep(xhc, slot_id, ep_out,
                                             EP_TYPE_BULK_OUT, out_mps, 0, speed);
    if (in_dci < 0 || out_dci < 0) {
        kprintf("[CDC-ACM] bulk endpoint configuration failed\n");
        return 0;
    }
    d->in_dci = in_dci;
    d->out_dci = out_dci;
    d->in_mps = in_mps;
    d->out_mps = out_mps;

    // Interrupt notification EP (best effort - data works without it).
    if (ep_notify >= 0) {
        int ndci = xhci_configure_endpoint_ep(xhc, slot_id, ep_notify,
                                              EP_TYPE_INTERRUPT_IN, notify_mps,
                                              8, speed);
        if (ndci >= 0) d->notify_dci = ndci;
    }

    // DMA bounce buffers (identity-mapped, phys == virt). 1 page each is plenty
    // for the M3D EEPROM dump (0x301 bytes) and any gcode line.
    uint64_t rx_phys = pmm_alloc_pages(1);
    uint64_t tx_phys = pmm_alloc_pages(1);
    if (!rx_phys || !tx_phys) {
        kprintf("[CDC-ACM] DMA buffer allocation failed\n");
        return 0;
    }
    d->rx_buf = (uint8_t *)rx_phys;
    d->rx_buf_len = PAGE_SIZE;
    d->tx_buf = (uint8_t *)tx_phys;
    d->tx_buf_len = PAGE_SIZE;

    // ACM line coding + control lines. Failures are non-fatal (the Atmel
    // bootloader accepts data regardless), but log them.
    if (cdc_set_line_coding(d, 115200, 0, 0, 8) != 0)
        kprintf("[CDC-ACM] SET_LINE_CODING failed (continuing)\n");
    if (cdc_set_control_line_state(d, 1, 1) != 0)
        kprintf("[CDC-ACM] SET_CONTROL_LINE_STATE failed (continuing)\n");

    d->active = 1;
    extern volatile int g_cdc_acm_generation;
    g_cdc_acm_generation++;
    kprintf("[CDC-ACM] serial port attached (slot %d) as /dev/ttyACM0\n", slot_id);
    bootlog_write("[CDC-ACM] %04x:%04x serial attached slot %d "
                  "bulk-in 0x%02x bulk-out 0x%02x (115200 8N1)",
                  vid, pid, slot_id, ep_in, ep_out);
    return 1;
}

// -----------------------------------------------------------------------------
// /dev/ttyACM0 node
// -----------------------------------------------------------------------------
static int64_t cdc_dev_read(file_t *f, void *buf, size_t count) {
    (void)f;
    // Bounded: at most ~60ms per userland read so a quiet printer never stalls
    // the caller. Returns 0 (would-block) if nothing arrived.
    int r = usb_cdc_acm_read(buf, (uint32_t)count, 60);
    return (r < 0) ? -1 : (int64_t)r;
}

static int64_t cdc_dev_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    int r = usb_cdc_acm_write(buf, (uint32_t)count, 300);
    return (r < 0) ? -1 : (int64_t)count;
}

static void cdc_dev_release(file_t *f) { (void)f; }

static const file_ops_t cdc_dev_ops = {
    .read    = cdc_dev_read,
    .write   = cdc_dev_write,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = cdc_dev_release,
    .poll    = NULL,
};

static file_t *cdc_dev_open(int flags) {
    if (!g_cdc.active) return NULL;   // no device present
    return file_alloc(&cdc_dev_ops, NULL, flags);
}

void usb_cdc_acm_dev_init(void) {
    dev_register("ttyACM0", cdc_dev_open);
}

// -----------------------------------------------------------------------------
// Serial-console test command: "m3d <sub>"
// -----------------------------------------------------------------------------
static void cdc_hexdump(const uint8_t *b, int n) {
    static const char *hx = "0123456789abcdef";
    char line[80];
    for (int i = 0; i < n; i++) {
        int o = 0;
        line[o++] = hx[(b[i] >> 4) & 0xF];
        line[o++] = hx[b[i] & 0xF];
        line[o] = 0;
        kprintf("%s", line);
        if ((i & 31) == 31) kprintf("\n");
    }
    kprintf("\n");
}

// Read up to want bytes (bounded overall), accumulating short bursts.
static int cdc_read_accumulate(uint8_t *out, int want, uint32_t overall_ms) {
    int off = 0;
    int idle = 0;
    uint32_t elapsed = 0;
    while (off < want && elapsed < overall_ms) {
        int r = usb_cdc_acm_read(out + off, (uint32_t)(want - off), 100);
        elapsed += 100;
        if (r < 0) { kprintf("[m3d] read error\n"); break; }
        if (r == 0) { if (++idle >= 6) break; continue; }
        idle = 0;
        off += r;
    }
    return off;
}

// Append a base-10 integer to out[o..], return new offset.
static int cdc_put_int(char *out, int cap, int o, int v) {
    char num[12]; int ni = 0;
    if (v < 0) { if (o < cap - 1) out[o++] = '-'; v = -v; }
    if (v == 0) num[ni++] = '0';
    while (v > 0 && ni < 11) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni > 0 && o < cap - 1) out[o++] = num[--ni];
    return o;
}

// XOR checksum over the first `len` bytes (Marlin/iMe style).
static int cdc_xor_checksum(const char *s, int len) {
    int c = 0;
    for (int i = 0; i < len; i++) c ^= (unsigned char)s[i];
    return c;
}

// Format one iMe wire line: "N<n> <cmd>*<chk>\n". Returns its length.
static int cdc_format_line(char *out, int cap, int n, const char *cmd) {
    int o = 0;
    if (o < cap - 1) out[o++] = 'N';
    o = cdc_put_int(out, cap, o, n);
    if (o < cap - 1) out[o++] = ' ';
    for (const char *p = cmd; *p && o < cap - 1; p++) out[o++] = *p;
    int chk = cdc_xor_checksum(out, o);   // checksum over "N<n> <cmd>"
    if (o < cap - 1) out[o++] = '*';
    o = cdc_put_int(out, cap, o, chk);
    if (o < cap - 1) out[o++] = '\n';
    out[o] = 0;
    return o;
}

static void m3d_cmd_info(void) {
    if (!g_cdc.active) { kprintf("[m3d] no CDC-ACM device attached\n"); return; }
    kprintf("[m3d] CDC-ACM %04x:%04x, bootloader EEPROM read ('S')\n",
            g_cdc.vid, g_cdc.pid);

    // Send the bootloader 'S' command.
    if (usb_cdc_acm_write("S", 1, 300) < 0) {
        kprintf("[m3d] failed to send 'S'\n");
        return;
    }
    static uint8_t eeprom[0x320] __attribute__((aligned(64)));
    memset(eeprom, 0, sizeof(eeprom));
    int got = cdc_read_accumulate(eeprom, 0x301, 3000);
    kprintf("[m3d] EEPROM response: %d bytes (expect 769)\n", got);
    if (got <= 0) {
        kprintf("[m3d] NO RESPONSE from printer (not in bootloader mode?)\n");
        return;
    }
    kprintf("[m3d] first 16 bytes: ");
    cdc_hexdump(eeprom, got < 16 ? got : 16);
    if (got >= 8) {
        uint32_t fwver = eeprom[0] | (eeprom[1] << 8) |
                         (eeprom[2] << 16) | ((uint32_t)eeprom[3] << 24);
        uint32_t fwcrc = eeprom[4] | (eeprom[5] << 8) |
                         (eeprom[6] << 16) | ((uint32_t)eeprom[7] << 24);
        kprintf("[m3d] firmwareVersion = %u  firmwareCrc = %u\n", fwver, fwcrc);
    }
    // Serial number: printable ASCII run near the tail (e.g. "BK...").
    kprintf("[m3d] serial/tail ASCII: ");
    int start = got - 24; if (start < 0) start = 0;
    for (int i = start; i < got; i++) {
        char c = (char)eeprom[i];
        kprintf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    kprintf("\n[m3d] last byte 0x%02x (expect 0x0d)\n", eeprom[got - 1]);
    if (got == 0x301 && eeprom[got - 1] == 0x0d)
        kprintf("[m3d] HANDSHAKE OK: valid 769-byte EEPROM received\n");
}

// Build the iMe unload/change-filament command sequence WITHOUT sending it.
// Faithful to M33-Fio's unloadFilament(): fan on, heat hotend and wait, zero the
// extruder axis, long reverse pull, then heater off + motors off. Prints the
// exact wire lines for verification. NEVER sends (heat/extrude is user-present).
static void m3d_cmd_unload_dryrun(int temp) {
    kprintf("[m3d] UNLOAD/change-filament sequence (DRY RUN - not sent)\n");
    kprintf("[m3d] target hotend temp = %d C (PLA)\n", temp);
    char line[128];
    char cmd[32];
    int n = 0, o;

    // 1) fan on, heat hotend and WAIT (M33-Fio unloadFilament: "M106","M109 S<t>").
    cdc_format_line(line, sizeof(line), n++, "M106");
    kprintf("  %s", line);
    o = 0;
    for (const char *p = "M109 S"; *p; p++) { cmd[o++] = *p; }
    o = cdc_put_int(cmd, sizeof(cmd), o, temp);
    cmd[o] = 0;
    cdc_format_line(line, sizeof(line), n++, cmd);
    kprintf("  %s", line);

    // 2) absolute mode, zero the extruder axis (M33-Fio: "G90","G92 E0").
    cdc_format_line(line, sizeof(line), n++, "G90");    kprintf("  %s", line);
    cdc_format_line(line, sizeof(line), n++, "G92 E0"); kprintf("  %s", line);

    // 3) long reverse pull ~100mm (M33-Fio steps E-2..E-N by 2 at F345).
    for (int e = 2; e <= 100; e += 2) {
        o = 0;
        for (const char *p = "G0 E-"; *p; p++) { cmd[o++] = *p; }
        o = cdc_put_int(cmd, sizeof(cmd), o, e);
        for (const char *p = " F345"; *p; p++) { cmd[o++] = *p; }
        cmd[o] = 0;
        cdc_format_line(line, sizeof(line), n++, cmd);
        if (e <= 6 || e >= 96) kprintf("  %s", line);   // show head + tail
        else if (e == 8) kprintf("  ... (E-8 .. E-94 F345) ...\n");
    }
    // 4) heater OFF, motors off (M33-Fio print-done: "M104 S0","M18").
    cdc_format_line(line, sizeof(line), n++, "M104 S0"); kprintf("  %s", line);
    cdc_format_line(line, sizeof(line), n++, "M18");     kprintf("  %s", line);
    kprintf("[m3d] unload path built: %d commands (iMe ASCII+XOR-checksum)\n", n);
    kprintf("[m3d] NOT SENT (heat/extrude is a user-present live step)\n");
}

// Encode a minimal M3D binary packet for a command using only integer
// N/M/G parameters (covers M110, G28), faithful to phase-1 m3d_gcode_get_binary:
// dataType bitmask (N=bit0, M=bit1, G=bit2) little-endian u32, then the present
// params in order N(i16), M(i16)|G(i16), then a Fletcher-16 checksum over the
// packet body. Returns the packet length.
static int m3d_bin_ng(uint8_t *out, int have_n, int nval, int is_m, int code) {
    // dataType base 0x1080 is the M3D protocol sentinel (bits 7 and 12);
    // omitting it makes the firmware answer e1 not-supported-protocol (#396 p2b).
    uint32_t dt = 0x1080u;
    if (have_n) dt |= (1u << 0);
    dt |= is_m ? (1u << 1) : (1u << 2);
    int o = 0;
    out[o++] = dt & 0xFF; out[o++] = (dt >> 8) & 0xFF;
    out[o++] = (dt >> 16) & 0xFF; out[o++] = (dt >> 24) & 0xFF;
    if (have_n) { out[o++] = nval & 0xFF; out[o++] = (nval >> 8) & 0xFF; }
    out[o++] = code & 0xFF; out[o++] = (code >> 8) & 0xFF;
    uint16_t s1 = 0, s2 = 0;
    for (int i = 0; i < o; i++) { s1 = (uint16_t)((s1 + out[i]) % 0xFF); s2 = (uint16_t)((s1 + s2) % 0xFF); }
    out[o++] = (uint8_t)s1; out[o++] = (uint8_t)s2;
    return o;
}

static void m3d_hexline(const uint8_t *b, int n) {
    static const char *hx = "0123456789abcdef";
    char s[3];
    for (int i = 0; i < n; i++) { s[0]=hx[(b[i]>>4)&0xF]; s[1]=hx[b[i]&0xF]; s[2]=0; kprintf("%s ", s); }
    kprintf("\n");
}

// Read + print any firmware response (bounded), returns bytes seen.
static int m3d_show_response(const char *tag, uint32_t ms) {
    static uint8_t rb[256];
    int total = 0, idle = 0;
    uint32_t el = 0;
    while (el < ms) {
        int r = usb_cdc_acm_read(rb, sizeof(rb) - 1, 100);
        el += 100;
        if (r < 0) { kprintf("[m3d] %s: read error\n", tag); break; }
        if (r == 0) { if (++idle >= 5) break; continue; }
        idle = 0; total += r; rb[r] = 0;
        kprintf("[m3d] %s recv: ", tag);
        for (int i = 0; i < r; i++) {
            char c = (char)rb[i];
            kprintf("%c", (c == '\n' || (c >= 32 && c < 127)) ? c : '.');
        }
        if (rb[r - 1] != '\n') kprintf("\n");
    }
    return total;
}

// Generation counter, bumped on every successful CDC-ACM attach so the home
// path can detect the firmware re-enumeration that follows the 'Q' command.
volatile int g_cdc_acm_generation = 0;

// AUTHORIZED HOME (bed clear, no heat, no extrude). Handles both printer modes:
//  - If the iMe FIRMWARE is already running (it emits "wait" when idle), send
//    "N0 M110" then "N1 G28" directly.
//  - If in the BOOTLOADER, send 'Q' to start the firmware. That RE-ENUMERATES
//    the USB device; the boot-time autotest runs before the hotplug port
//    rescan is active, so we cannot re-attach it here - report honestly and ask
//    to re-run once the printer is in firmware mode.
void m3d_cmd_home(void) {
    if (!g_cdc.active) { kprintf("[m3d] no CDC-ACM device attached\n"); return; }

    // Flush any pending firmware chatter (idle "wait" lines).
    static uint8_t junk[64];
    for (int i = 0; i < 6; i++) { if (usb_cdc_acm_read(junk, sizeof(junk), 100) <= 0) break; }

    // Probe: reset the line number. A live firmware answers "ok"/"wait"/"rs".
    char line[128];
    int n = cdc_format_line(line, sizeof(line), 0, "M110");
    kprintf("[m3d] TX: %s", line);
    usb_cdc_acm_write(line, n, 500);
    int got = m3d_show_response("M110", 2500);

    if (got <= 0) {
        kprintf("[m3d] no firmware reply - printer looks like it is in the bootloader.\n");
        kprintf("[m3d] sending 'Q' to start the firmware...\n");
        usb_cdc_acm_write("Q", 1, 300);
        kprintf("[m3d] 'Q' sent: the device now RE-ENUMERATES on USB. The boot autotest\n");
        kprintf("[m3d] runs before the hotplug rescan, so it cannot re-attach the new\n");
        kprintf("[m3d] port here. Re-run 'home' with the printer already in firmware mode.\n");
        return;
    }

    kprintf("[m3d] firmware is live (%d bytes).\n", got);

    // The M3D/iMe firmware parser answers "e1" (error 1 = not-supported-protocol)
    // to ASCII lines: this firmware speaks the BINARY M3D protocol. Send N0 M110
    // then N1 G28 as binary packets (phase-1 framing).
    uint8_t pkt[16];
    int pn;

    pn = m3d_bin_ng(pkt, 1, 0, 1, 110);   // N0 M110
    kprintf("[m3d] TX binary N0 M110: "); m3d_hexline(pkt, pn);
    usb_cdc_acm_write(pkt, pn, 500);
    m3d_show_response("M110(bin)", 2500);

    pn = m3d_bin_ng(pkt, 1, 1, 0, 28);    // N1 G28
    kprintf("[m3d] TX binary N1 G28: "); m3d_hexline(pkt, pn);
    usb_cdc_acm_write(pkt, pn, 500);
    m3d_show_response("G28(bin)", 10000);
    kprintf("[m3d] HOME (binary G28) sent. Expect physical homing motion.\n");
}

// Boot-time autotest. Always runs the SAFE read-only EEPROM handshake so the
// enumerate -> open -> query -> response pipe is proven headlessly on every
// boot. Then consults /M3D.CFG (first token) for an optional extra action:
//   "home"   -> the AUTHORIZED G28 home (leaves the bootloader with 'Q').
//   "unload" -> print the change-filament command sequence (dry run, not sent).
//   anything else / missing -> info only.
extern fat_fs_t g_fat_fs;
void m3d_boot_autotest(void) {
    if (!g_cdc.active) {
        kprintf("[m3d] boot autotest: no CDC-ACM serial device present\n");
        return;
    }
    kprintf("\n[m3d] ===== #396 M3D CDC-ACM boot autotest =====\n");
    m3d_cmd_info();   // always: read-only handshake

    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/M3D.CFG", &sz);
    if (!cfg || sz == 0) {
        kprintf("[m3d] no /M3D.CFG action file (info-only). Done.\n");
        if (cfg) kfree(cfg);
        return;
    }
    // First token.
    char act[16]; int a = 0;
    for (uint32_t i = 0; i < sz && a < 15; i++) {
        char c = cfg[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') break;
        act[a++] = c;
    }
    act[a] = 0;
    kfree(cfg);
    kprintf("[m3d] /M3D.CFG action = '%s'\n", act);
    if (strncmp(act, "home", 4) == 0) {
        m3d_cmd_home();
    } else if (strncmp(act, "unload", 6) == 0) {
        m3d_cmd_unload_dryrun(215);
    } else {
        kprintf("[m3d] action '%s' unrecognized; info-only.\n", act);
    }
    kprintf("[m3d] ===== autotest done =====\n");
}

void usb_cdc_acm_shell(const char *args) {
    while (*args == ' ') args++;
    if (strncmp(args, "info", 4) == 0 || *args == 0) {
        m3d_cmd_info();
    } else if (strncmp(args, "unload", 6) == 0) {
        m3d_cmd_unload_dryrun(215);   // PLA ~215C
    } else if (strncmp(args, "home", 4) == 0) {
        m3d_cmd_home();
    } else {
        kprintf("[m3d] usage: m3d [info|home|unload]\n");
        kprintf("[m3d]   info   - bootloader EEPROM read (safe, read-only)\n");
        kprintf("[m3d]   home   - start firmware (Q) then G28 home (authorized)\n");
        kprintf("[m3d]   unload - print the change-filament sequence (dry run)\n");
    }
}
