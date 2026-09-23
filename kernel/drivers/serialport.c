// serialport.c - the mediated serial-port gateway (Stage 2, serial.port).
// See serialport.h for the design and for why this half is C, not Rust.
//
// THE SUBTRACTION THIS EXISTS FOR. Before Stage 2, a userland app reached a
// serial adapter by opening /dev/ttyACM0 directly, which Stage 0 left seeded
// 0666 so /APPS/PRINT3D would not break. That row is now tightened (fs/perms.c),
// so the raw path is refused to the desktop uid. The ONLY way to a serial port
// now is: SYS_SERIAL_LIST names the ports, SYS_SERIAL_OPEN(name) opens one, and
// the open is gated by a serial.port grant the compositor's consent produced.
// No app can name a base address, a channel, or a device path.
#include "serialport.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/vfs.h"
#include "../security/validate.h"   // copy_to_user / copy_from_user
#include "../fs/bootlog.h"

extern void kprintf(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Registry. Static, tiny, no allocation. A backend is EITHER a dev_open factory
// (USB CDC-ACM, base==0) OR a 16550 UART base (devopen==NULL).
// ---------------------------------------------------------------------------
typedef struct {
    char        name[SERIALPORT_NAME_MAX];
    uint32_t    cls;
    uint32_t    flags;
    uint16_t    base;        // 16550 backend base I/O port; 0 => dev factory
    dev_open_fn devopen;     // dev factory; NULL => 16550 backend
    uint8_t     used;
} sp_entry_t;

static sp_entry_t g_ports[SERIALPORT_MAX];
static uint32_t   g_port_count = 0;

static sp_entry_t *sp_find(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_port_count; i++) {
        if (g_ports[i].used && strcmp(g_ports[i].name, name) == 0)
            return &g_ports[i];
    }
    return NULL;
}

static sp_entry_t *sp_alloc(const char *name) {
    if (!name || !name[0]) return NULL;
    if (strlen(name) >= SERIALPORT_NAME_MAX) return NULL;
    if (sp_find(name)) return NULL;            // no duplicate names
    if (g_port_count >= SERIALPORT_MAX) return NULL;
    sp_entry_t *e = &g_ports[g_port_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, SERIALPORT_NAME_MAX - 1);
    e->used = 1;
    return e;
}

int serialport_register_dev(const char *name, uint32_t cls, dev_open_fn open) {
    if (!open) return -1;
    sp_entry_t *e = sp_alloc(name);
    if (!e) return -1;
    e->cls = cls;
    e->flags = SERIALPORT_F_NONE;
    e->base = 0;
    e->devopen = open;
    kprintf("[SERIALPORT] published '%s' (class %u, USB CDC-ACM)\n",
            name, (unsigned)cls);
    (void)bootlog_write("[SERIALPORT] published %s (usb-cdc)", name);
    return 0;
}

// ---------------------------------------------------------------------------
// 16550 backend. A self-contained loopback probe that does NOT touch serial.c's
// g_serial_present global (which is the COM1 console verdict): clobbering it
// would silence the console. Writes go through serial.c's bounded serial_write
// so a UART that never drains drops the character instead of freezing the
// kernel (the #426 discipline). Reads are non-blocking: a serial line is a
// remote peer, so a would-block read returns 0, and there is no wake to arm.
// ---------------------------------------------------------------------------
static int uart_probe_init(uint16_t base) {
    outb(base + SERIAL_IER,  0x00);   // no interrupts
    outb(base + SERIAL_LCR,  0x80);   // DLAB
    outb(base + SERIAL_DLL,  0x01);   // divisor 1 => 115200
    outb(base + SERIAL_DLH,  0x00);
    outb(base + SERIAL_LCR,  0x03);   // 8N1
    outb(base + SERIAL_FIFO, 0xC7);   // FIFO on, cleared, 14-byte
    outb(base + SERIAL_MCR,  0x1E);   // loopback + OUT1|OUT2|RTS
    outb(base + SERIAL_DATA, 0xAE);   // loopback test byte
    uint8_t lb = inb(base + SERIAL_DATA);
    if (lb != 0xAE) {
        outb(base + SERIAL_MCR, 0x0F);   // leave loopback anyway
        return 0;                        // absent / dead
    }
    outb(base + SERIAL_MCR, 0x0F);        // normal: OUT1|OUT2|RTS|DTR
    return 1;
}

static int64_t s16550_read(file_t *f, void *buf, size_t count) {
    uint16_t base = (uint16_t)(uintptr_t)f->priv;
    if (count == 0) return 0;
    uint8_t kb[128];
    uint32_t cap = (count < sizeof(kb)) ? (uint32_t)count : (uint32_t)sizeof(kb);
    uint32_t n = 0;
    // Bounded drain of what the FIFO holds now. A for-loop with a hard bound and
    // an early break on "no data ready": not a busy-wait, no condition to spin on.
    for (uint32_t i = 0; i < cap; i++) {
        if (!(inb(base + SERIAL_LSR) & SERIAL_LSR_DR)) break;
        kb[n++] = inb(base + SERIAL_DATA);
    }
    if (n == 0) return 0;                       // would-block
    if (copy_to_user(buf, kb, n) != 0) return -1;
    return (int64_t)n;
}

static int64_t s16550_write(file_t *f, const void *buf, size_t count) {
    uint16_t base = (uint16_t)(uintptr_t)f->priv;
    if (count == 0) return 0;
    uint8_t kb[128];
    uint32_t total = 0;
    while (total < count) {
        uint32_t chunk = (uint32_t)((count - total) < sizeof(kb) ?
                                    (count - total) : sizeof(kb));
        if (copy_from_user(kb, (const uint8_t *)buf + total, chunk) != 0)
            return total ? (int64_t)total : -1;
        for (uint32_t i = 0; i < chunk; i++) serial_write(base, (char)kb[i]);
        total += chunk;
    }
    return (int64_t)total;
}

static int s16550_release(file_t *f) { (void)f; return 0; }   // nothing buffered

static const file_ops_t s16550_ops = {
    .read    = s16550_read,
    .write   = s16550_write,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = s16550_release,
    .poll    = NULL,
};

int serialport_register_uart(const char *name, uint16_t base) {
    if (base == COM1) return -1;              // COM1 is the console: never publish
    if (!uart_probe_init(base)) return -1;    // no UART actually there
    sp_entry_t *e = sp_alloc(name);
    if (!e) return -1;
    e->cls = SERIALPORT_CLS_UART16550;
    e->flags = SERIALPORT_F_NONE;
    e->base = base;
    e->devopen = NULL;
    kprintf("[SERIALPORT] published '%s' (class 1, 16550 @ 0x%x)\n",
            name, (unsigned)base);
    (void)bootlog_write("[SERIALPORT] published %s (16550 @ 0x%x)", name,
                        (unsigned)base);
    return 0;
}

// ---------------------------------------------------------------------------
// Public surface used by the syscall handlers (proc/syscall.c) and the
// capability request path (proc/caps.c).
// ---------------------------------------------------------------------------
int serialport_is_published(const char *name) {
    return sp_find(name) != NULL ? 1 : 0;
}

uint32_t serialport_list(serial_pub_t *out, uint32_t max) {
    if (!out) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_port_count && n < max; i++) {
        if (!g_ports[i].used) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, g_ports[i].name, SERIALPORT_NAME_MAX - 1);
        out[n].cls = g_ports[i].cls;
        out[n].flags = g_ports[i].flags;
        n++;
    }
    return n;
}

struct file *serialport_open(const char *name, int flags) {
    sp_entry_t *e = sp_find(name);
    if (!e) return NULL;
    file_t *f = NULL;
    if (e->devopen) {
        f = e->devopen(flags);               // USB CDC-ACM factory
    } else {
        f = file_alloc(&s16550_ops, (void *)(uintptr_t)e->base, flags);
    }
    if (f) {
        char p[VFS_FPATH_MAX];
        int k = 0;
        const char *pfx = "serial:";
        while (pfx[k] && k < (int)sizeof(p) - 1) { p[k] = pfx[k]; k++; }
        int j = 0;
        while (e->name[j] && k < (int)sizeof(p) - 1) { p[k++] = e->name[j++]; }
        p[k] = 0;
        file_set_path(f, p);
    }
    return f;
}

void serialport_init(void) {
    // COM2 (0x2F8) is the natural first target on a VM: a second, real 16550
    // that is not the console. On the real iMac 0x2F8 is typically absent, so
    // the loopback probe fails and nothing is published, which is correct: this
    // port is born gated, never ambient. USB CDC-ACM registers itself when a
    // device attaches (drivers/usb_cdc_acm.c).
    if (serialport_register_uart("ttyS1", COM2) != 0) {
        // Not an error: absent on most machines. State it once for the log.
        kprintf("[SERIALPORT] no COM2 UART present; ttyS1 not published\n");
    }
}

// ===========================================================================
// Syscall handlers (Ring 3 boundary). Dispatched from proc/syscall.c.
// ===========================================================================
extern int caps_current_covers_port(uint32_t cap, const char *port);

// SYS_SERIAL_LIST: name the published ports into a Ring-3 buffer. NOT gated: a
// port name and a class are not power, exactly as dimg volume enumeration is
// not (the noun to OPEN one still requires serial.port). Returns the count.
int64_t sys_serial_list(serial_pub_t *u_out, uint32_t max)
{
    if (!u_out) return -1;
    if (max > SERIALPORT_MAX) max = SERIALPORT_MAX;
    if (max == 0) return 0;
    serial_pub_t tmp[SERIALPORT_MAX];
    uint32_t n = serialport_list(tmp, max);
    if (n == 0) return 0;
    if (copy_to_user(u_out, tmp, (unsigned long)n * sizeof(serial_pub_t)) != 0)
        return -1;
    return (int64_t)n;
}

// SYS_SERIAL_OPEN: gated serial.port. The dispatcher chokepoint has already
// proven the caller holds a serial.port grant of SOME scope. Bind it to the
// EXACT requested port (caps_current_covers_port consumes one use), then open
// the backend and install an fd. Reads/writes then use the ordinary fd path, so
// the capability changes nothing about the call shape once the fd is held.
// Returns the fd, or CAP_EDENIED (-3) on a refused/uncovered request.
int64_t sys_serial_open(const char *u_name, int flags)
{
    char name[SERIALPORT_NAME_MAX];
    if (strncpy_from_user(name, u_name, sizeof(name)) < 0) return -1;
    name[sizeof(name) - 1] = 0;
    if (!serialport_is_published(name)) return -3;   // CAP_EDENIED: no such port
    // The grant must cover THIS exact port; consume one use.
    if (!caps_current_covers_port(6u /*CAP_SERIAL_PORT*/, name))
        return -3;                                   // CAP_EDENIED: not covered
    struct file *f = serialport_open(name, flags);
    if (!f) return -1;
    int fd = fd_alloc_install(f);
    if (fd < 0) {
        IGNORE_RESULT("fd table full: nothing else holds this description, so "
                      "this put is its final release", file_put(f));
        return -1;
    }
    return (int64_t)fd;
}
