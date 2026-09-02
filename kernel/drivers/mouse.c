// mouse.c - PS/2 Mouse driver implementation
#include "mouse.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "../video/framebuffer.h"
// #426-class real-hardware bring-up: this file has to survive a machine with no
// 8042 at all. See the "IS THERE AN 8042 AT ALL?" block below.
#include "../cpu/isr.h"     // keyboard_process_scancode(): THE one scancode funnel
#include "../fs/bootlog.h"  // persistent diagnostics; never re-declare as a private extern
#include "acpi.h"           // FADT IAPC_BOOT_ARCH bit 1 = "8042 not present"

// PS/2 Controller ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

// PS/2 Commands
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_PORT2 0xA7
#define PS2_CMD_ENABLE_PORT2  0xA8
#define PS2_CMD_TEST_PORT2    0xA9
#define PS2_CMD_WRITE_PORT2   0xD4

// Mouse commands
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_RESEND      0xFE
#define MOUSE_CMD_SET_DEFAULT 0xF6
#define MOUSE_CMD_DISABLE     0xF5
#define MOUSE_CMD_ENABLE      0xF4
#define MOUSE_CMD_SET_RATE    0xF3
#define MOUSE_CMD_GET_ID      0xF2
#define MOUSE_CMD_SET_REMOTE  0xF0
#define MOUSE_CMD_SET_WRAP    0xEE
#define MOUSE_CMD_RESET_WRAP  0xEC
#define MOUSE_CMD_READ_DATA   0xEB
#define MOUSE_CMD_SET_STREAM  0xEA
#define MOUSE_CMD_STATUS_REQ  0xE9
#define MOUSE_CMD_SET_RES     0xE8
#define MOUSE_CMD_SET_SCALE21 0xE7
#define MOUSE_CMD_SET_SCALE11 0xE6

// Mouse responses
#define MOUSE_ACK  0xFA
#define MOUSE_NACK 0xFE

// Mouse sensitivity: 1=slow, 5=1:1, 10=fast. Default raised to 7 so the pointer
// feels quick and responsive out of the box (desktop UX pass); the compositor
// overrides this from the persisted UIPROFIL mouse_sens value on boot.
static int g_mouse_sensitivity = 7;

// #139/#133: THE sensitivity scale, with the remainder CARRIED instead of
// thrown away. `(delta * sensitivity) / 5` is integer division truncating
// toward zero, and at the shipped default of 7 that is a visibly uneven
// staircase applied to EVERY report:
//     raw dx  1  2  3  4  5  6  7      (7/5 = 1.4 px per raw count)
//     scaled  1  2  4  5  7  8  9      (steps of 1,2,1,2,1,1 - not 1.4)
// so a hand moving at a constant speed does not produce a constant cursor
// speed. Worse, the error is DISCARDED per report, so the size of the error
// depends on how many raw counts happen to land in one report - which means
// the pointer's effective gain changes with the report rate and therefore with
// system load. That is a jitter source in its own right, independent of the
// interval bug this ticket started from, and it applies equally to the PS/2
// and USB paths (both call this).
//
// Carrying the remainder makes the mapping exact in aggregate: N raw counts
// always become round(N * sensitivity / 5) pixels no matter how they are split
// across reports. The residual is per axis and per direction-agnostic (C
// truncation is toward zero and the remainder carries the same sign), and it
// is bounded by |sensitivity| so it can never accumulate into a jump.
//
// HONEST SCOPE: this is an arithmetic correctness fix with the numbers above as
// its argument. It has NOT been measured as a perceptual improvement, because
// nothing in this tree can measure pointer feel; do not report it as one.
//
// The residual is shared between the PS/2 IRQ path and the USB HID worker, as
// g_mouse.x/y already were. mouse_inject_hid() runs under cli() so the PS/2
// handler cannot interleave with it on the same cpu; on another cpu it could,
// and the cost of a torn update is bounded by the residual itself, under one
// pixel. A lock here would be worse than the fault: this is on the interrupt
// path of every mouse packet, and nobody has two mice.
static int g_sens_resid_x = 0;
static int g_sens_resid_y = 0;

static inline void mouse_scale_delta(int dx_in, int dy_in, int *dx_out, int *dy_out) {
    int nx = dx_in * g_mouse_sensitivity + g_sens_resid_x;
    int ny = dy_in * g_mouse_sensitivity + g_sens_resid_y;
    *dx_out = nx / 5;
    *dy_out = ny / 5;
    g_sens_resid_x = nx - (*dx_out) * 5;
    g_sens_resid_y = ny - (*dy_out) * 5;
}

void mouse_set_sensitivity(int s) {
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    g_mouse_sensitivity = s;
}

int mouse_get_sensitivity(void) {
    return g_mouse_sensitivity;
}

// Mouse state
static mouse_state_t g_mouse;
extern volatile uint64_t timer_ticks;
// While timer_ticks < this, ignore real PS/2 packets so a synthetic
// click injected via mouse_inject_button() is not overwritten mid-click.
volatile uint64_t g_mouse_synth_until = 0;
static int32_t mouse_min_x = 0;
static int32_t mouse_min_y = 0;
static int32_t mouse_max_x = 1920;
static int32_t mouse_max_y = 1080;

// Packet assembly
static uint8_t mouse_packet[4];
static int mouse_packet_idx = 0;
static int mouse_has_wheel = 0;

// Global variables for external access (fb_syscall.c)
int32_t mouse_x = 0;
int32_t mouse_y = 0;
uint8_t mouse_buttons = 0;

// Wait for PS/2 controller to be ready for input
static void ps2_wait_write(void) {
    int timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS_PORT) & 0x02)) {
        __asm__ volatile("pause");
    }
}

// Wait for PS/2 controller to have data
static int ps2_wait_read(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS_PORT) & 0x01)) {
        __asm__ volatile("pause");
    }
    return timeout > 0;
}

// ---------------------------------------------------------------------------
// IS THERE AN 8042 AT ALL?  (#426-class, unknown ASUS Intel i7 laptop)
//
// An unclaimed x86 I/O port floats to 0xFF, and every consumer of the status
// byte then reads a LIE that happens to be the most permissive answer possible:
//     0xFF & 0x01 (OBF)  -> "a byte is waiting", forever
//     0xFF & 0x20 (AUX)  -> "and it came from the mouse", for every byte
//     0xFF & 0x08 (A2)   -> the first-byte resync guard never fires
// so mouse_poll()'s drain loop had no exit at all: it spun reading 0xFF out of
// port 0x60 and fabricating mouse packets from it. That is a HARD HANG, and it
// is reachable exactly on the path that matters most on unfamiliar hardware:
// gui/desktop.c short-circuits its input block while the userland compositor is
// up, so the polled path runs when the compositor FAILED to launch.
//
// 0xFF is not a legal 8042 status byte under any circumstances. It asserts OBF,
// IBF, SYS, A2, INH, AUX, TIMEOUT and PARITY simultaneously, which no real
// controller ever does. Read it as "there is no controller here" and LATCH it,
// exactly as serial.c latches g_serial_present from its loopback probe (#745
// task #69) so an absent UART costs one compare per character instead of a poll
// that can never succeed. Same shape here: one compare per call, instead of
// re-probing 64 times per frame forever.
//
// FIRMWARE BEATS A FLOATING READ. The ACPI FADT's IAPC_BOOT_ARCH word (FADT
// offset 109, ACPI 2.0+) has bit 1 = "8042 not present". That is the platform
// TELLING us, rather than us inferring it from a bus that happens to float, so
// it is consulted first. It is read straight off the FADT through the already
// exported acpi_find_table("FACP"), so nothing in drivers/acpi.c has to change
// (parse_fadt() still never reads the field, which is why iapc_boot_arch had
// exactly one occurrence in the whole tree: its declaration). acpi_init() runs
// at main.c:1184, long before mouse_init() at main.c:2489, so the value is
// available in time.
static int g_ps2_absent        = 0;   // latched: there is no 8042 on this machine
static int g_ps2_absent_logged = 0;   // the "no 8042" line is printed ONCE
static int g_ps2_drain_logged  = 0;   // the drain-cap line is printed ONCE

// FADT layout lock: iapc_boot_arch MUST sit at FADT offset 109 or the bit test
// below is silently reading some other field. Same _Static_assert discipline the
// Rust FFI structs use (drivers/hotplug.c:31, drivers/usb_audio.c:101).
_Static_assert(__builtin_offsetof(acpi_fadt_t, iapc_boot_arch) == 109,
               "FADT IAPC_BOOT_ARCH must be at offset 109");

#define FADT_IAPC_8042_ABSENT  (1u << 1)   // ACPI: "8042 not present"

// -1 = firmware says there is NO 8042, +1 = firmware says there IS one,
//  0 = no opinion (no ACPI, no FADT, FADT too short, or an ACPI 1.0 FADT where
//      the field is reserved and must be ignored rather than believed).
static int ps2_fadt_verdict(void) {
    acpi_sdt_header_t *h = acpi_find_table(ACPI_SIG_FADT);
    if (!h) return 0;
    // Require BOTH the revision and the length. An ACPI 1.0 FADT is 116 bytes,
    // so a length check alone would happily read reserved bytes as a verdict.
    if (h->revision < 2 || h->length < 111) return 0;
    acpi_fadt_t *fadt = (acpi_fadt_t *)h;
    return (fadt->iapc_boot_arch & FADT_IAPC_8042_ABSENT) ? -1 : 1;
}

// True when port 0x64 reads back the undriven-bus signature.
static int ps2_status_is_floating(uint8_t status) {
    return status == 0xFF;
}

// Latch "no 8042" and say so ONCE, loudly and greppably, on serial AND in the
// persistent boot log (serial is silent in GUI mode and the target laptop has no
// serial console at all). ONCE, not per call: a line printed every frame is not
// a diagnostic, it is a denial of service on the log.
static void ps2_mark_absent(const char *why) {
    g_ps2_absent = 1;
    if (g_ps2_absent_logged) return;
    g_ps2_absent_logged = 1;
    kprintf("[PS2] *** NO 8042 CONTROLLER (%s). PS/2 keyboard/mouse polling is "
            "DISABLED for the rest of this boot; USB HID is the only input "
            "path. ***\n", why);
    bootlog_write("[PS2] no 8042 controller (%s): PS/2 polling disabled", why);
}

// Send command to PS/2 controller
static void ps2_send_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

// Send data to PS/2 data port
static void ps2_send_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

// Read from PS/2 data port
static uint8_t ps2_read_data(void) {
    if (ps2_wait_read()) {
        return inb(PS2_DATA_PORT);
    }
    return 0xFF;
}

// Send command to mouse (via port 2)
static uint8_t mouse_send_cmd(uint8_t cmd) {
    ps2_send_cmd(PS2_CMD_WRITE_PORT2);
    ps2_send_data(cmd);
    return ps2_read_data();
}

// Process mouse packet
static void mouse_process_packet(void) {
    if (g_mouse_synth_until && timer_ticks < g_mouse_synth_until) return;
    uint8_t flags = mouse_packet[0];

    // Store previous button state
    g_mouse.prev_buttons = g_mouse.buttons;

    // Extract button state
    g_mouse.buttons = flags & 0x07;

    // Extract movement
    int32_t dx = mouse_packet[1];
    int32_t dy = mouse_packet[2];

    // Handle sign extension
    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;

    // Y is inverted in PS/2
    dy = -dy;

    // Accumulate delta (cleared by mouse_get_state_and_clear)
    g_mouse.dx += dx;
    g_mouse.dy += dy;

    // Extract scroll wheel data (4th byte if wheel mouse)
    if (mouse_has_wheel) {
        // 4th byte is signed scroll value
        int8_t scroll = (int8_t)mouse_packet[3];
        g_mouse.scroll = scroll;
    } else {
        g_mouse.scroll = 0;
    }

    // Apply sensitivity scaling (5 = 1:1, values above/below speed up/slow down).
    // Remainder-carrying, see mouse_scale_delta().
    mouse_scale_delta(dx, dy, &dx, &dy);

    // Update position
    g_mouse.x += dx;
    g_mouse.y += dy;

    // Clamp to bounds
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables for external access
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
    mouse_buttons = g_mouse.buttons;
}

// Mouse interrupt handler
static void mouse_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t status = inb(PS2_STATUS_PORT);

    // Check if data is from mouse (bit 5 set)
    if (!(status & 0x20)) {
        pic_send_eoi(12);
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    // First byte should have bit 3 set (always 1)
    if (mouse_packet_idx == 0 && !(data & 0x08)) {
        // Resync - discard byte
        pic_send_eoi(12);
        return;
    }

    mouse_packet[mouse_packet_idx++] = data;

    int packet_size = mouse_has_wheel ? 4 : 3;

    if (mouse_packet_idx >= packet_size) {
        mouse_process_packet();
        mouse_packet_idx = 0;
    }

    pic_send_eoi(12);
}

// Try to enable scroll wheel (IntelliMouse)
static int mouse_enable_wheel(void) {
    // Magic sequence to enable wheel
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(200);
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(100);
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(80);

    // Get device ID
    mouse_send_cmd(MOUSE_CMD_GET_ID);
    uint8_t id = ps2_read_data();

    return (id == 3);  // ID 3 = wheel mouse
}

// Initialize PS/2 mouse
// ---------------------------------------------------------------------------
// #ASUSKBD: the i8042 bring-up, and why it had to be rewritten to REPORT
//
// SYMPTOM (ASUS i7-4720HQ laptop, Haswell/Lynx Point-LP, 2026-08-26 boot):
// USB keyboard and USB mouse both work; the INTERNAL keyboard is dead. The
// entire PS/2 record of that boot was two lines:
//
//     [PS2] FADT IAPC_BOOT_ARCH: 8042 not present (0x64=0x15)
//     [PS2] FADT/hardware disagree (0x64=0x15): probing anyway
//
// and then silence. Not a failure, not a success: SILENCE. Every line after the
// presence gate was a kprintf(), and kprintf() is serial-only. A laptop has no
// serial port. So the probe ran roughly fifteen controller commands and reported
// the outcome of exactly none of them to the only log that survives a reboot.
// A diagnostic that stops one line before the answer is the recurring defect
// shape in this tree (#71/#427 unbounded RIRB wait, #365 phantom AHCI); this is
// the same shape again, in the input stack.
//
// WHAT THE OLD SEQUENCE ACTUALLY DID, which is worse than "nothing":
//
//     ps2_send_cmd(PS2_CMD_ENABLE_PORT2);
//     ps2_send_cmd(PS2_CMD_READ_CONFIG);
//     uint8_t config = ps2_read_data();      // <-- returns the WRONG byte
//     config |= 0x02;
//     config &= ~0x20;
//     ps2_send_cmd(PS2_CMD_WRITE_CONFIG);
//     ps2_send_data(config);                 // <-- writes the WRONG byte back
//
// There is NO OUTPUT-BUFFER FLUSH anywhere in this driver, and the ASUS log
// proves the buffer was full: 0x64 = 0x15 has OBF (bit 0) SET before the first
// command is issued. ps2_read_data() waits for OBF and then reads 0x60, so with
// OBF already asserted by a STALE byte it returns immediately with that stale
// byte and never sees the config byte at all. Whatever was left in the buffer by
// firmware (a keyboard BAT 0xAA, a scancode, a mouse ACK) becomes `config`, and
// is then written back into the controller as configuration.
//
// That is not a cosmetic race. Two bits of the config byte decide whether the
// internal input devices exist at all:
//
//     bit 4 (0x10) = 1  ->  FIRST port clock DISABLED   -> internal KEYBOARD dead
//     bit 5 (0x20) = 1  ->  SECOND port clock DISABLED  -> internal TOUCHPAD dead
//     bit 6 (0x40) = 0  ->  translation OFF -> keyboard emits set 2 while
//                           cpu/isr.c's keyboard_process_scancode() decodes
//                           set 1, i.e. every key produces garbage
//
// The old code cleared bit 5 and set bit 1, and left bits 4 and 6 at whatever
// the stale byte happened to carry. A stale 0xAA yields config 0x8B: clock 1 on
// but translation OFF (garbled keyboard). A stale scancode such as 0x1C yields
// 0x1E: bit 4 SET, i.e. WE DISABLE THE INTERNAL KEYBOARD OURSELVES. Either way
// the internal keyboard is lost, and which one you get depends on a byte nobody
// chose. This driver was the prime suspect for its own bug report.
//
// It also never enabled the first port at all: no 0xAE, no config bit 0 (IRQ1),
// no 0xF4 (enable scanning) to the keyboard device. keyboard_init() is declared
// in drivers/keyboard.h and DEFINED NOWHERE, with zero callers in the tree
// (the zero-callers class again). cpu/isr.c registers the IRQ1 vector and
// unmasks PIC line 1, and that is the whole of our PS/2 keyboard "driver". The
// keyboard worked on every VM and on the iMac purely because their firmware
// happened to hand off an 8042 that was already enabled and already scanning.
// That is a assumption, and it is the first one an unfamiliar laptop broke.
//
// WHAT THIS REPLACEMENT DOES
//
// The canonical osdev i8042 bring-up, in the canonical order, with EVERY step
// reported to bootlog_write() (which survives a reboot) as well as kprintf():
// quiesce both ports, FLUSH, read config, controller self-test, per-port
// interface tests, enable, write a config byte whose every bit we chose, then
// reset+identify the device on EACH port. Both ports are reported SEPARATELY
// and by name, because on a Haswell laptop with no LPSS I2C controller (00:15.x
// is absent from this machine's PCI enumeration, so the internal keyboard is
// NOT I2C-HID) the touchpad is a PS/2 device on the SECOND port. One controller
// fault takes out both internal input devices, and one fix returns both.
//
// EVERY early return states its reason. That is the point of the exercise.
// ---------------------------------------------------------------------------

// Bounded per #426. Same budget the original ps2_wait_read() used.
#define PS2_IO_SPINS 100000

#define PS2_CMD_DISABLE_PORT1 0xAD
#define PS2_CMD_ENABLE_PORT1  0xAE
#define PS2_CMD_SELFTEST      0xAA
#define PS2_CMD_TEST_PORT1    0xAB

// Config byte bits BY NAME. Every one of these has cost someone a keyboard.
#define PS2_CFG_INT1     0x01   // IRQ1  (first port)  enabled
#define PS2_CFG_INT2     0x02   // IRQ12 (second port) enabled
#define PS2_CFG_SYSFLAG  0x04   // set once firmware POSTed the controller
#define PS2_CFG_CLK1_OFF 0x10   // 1 = FIRST  port clock DISABLED (kills keyboard)
#define PS2_CFG_CLK2_OFF 0x20   // 1 = SECOND port clock DISABLED (kills touchpad)
#define PS2_CFG_XLAT1    0x40   // 1 = translate first port into scancode set 1

// Log to BOTH channels. bootlog_write() reaches /BOOTLOG.TXT, which is the only
// channel that exists on a laptop; kprintf() is serial-only and was the sole
// destination of every line this function replaces.
#define PS2LOG(fmt, ...) do {                            \
        kprintf("[PS2] " fmt "\n", ##__VA_ARGS__);       \
        bootlog_write("[PS2] " fmt, ##__VA_ARGS__);      \
    } while (0)

// Read one byte from 0x60 with a bounded wait. Returns 0 and stores the byte, or
// -1 on TIMEOUT. ps2_read_data() returns 0xFF on timeout, which is exactly the
// byte an undriven bus returns, so its caller cannot distinguish "the controller
// answered 0xFF" from "the controller said nothing at all". Every verdict below
// depends on that distinction.
static int ps2_try_read(uint8_t *out) {
    int timeout = PS2_IO_SPINS;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & 0x01) { *out = inb(PS2_DATA_PORT); return 0; }
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_try_cmd(uint8_t cmd) {
    int timeout = PS2_IO_SPINS;
    while (timeout-- > 0) {
        if (!(inb(PS2_STATUS_PORT) & 0x02)) { outb(PS2_CMD_PORT, cmd); return 0; }
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_try_data(uint8_t d) {
    int timeout = PS2_IO_SPINS;
    while (timeout-- > 0) {
        if (!(inb(PS2_STATUS_PORT) & 0x02)) { outb(PS2_DATA_PORT, d); return 0; }
        __asm__ volatile("pause");
    }
    return -1;
}

// Empty the one-byte output buffer, reporting HOW MANY bytes were stale and what
// they were. This is the step whose absence caused the bug described above, so it
// reports even when it finds nothing: "0 stale bytes" is the evidence that the
// next config read is trustworthy.
static int ps2_flush(const char *when) {
    uint8_t seen[8];
    int n = 0, total = 0;
    for (int i = 0; i < 32; i++) {
        if (!(inb(PS2_STATUS_PORT) & 0x01)) break;
        uint8_t d = inb(PS2_DATA_PORT);
        if (n < 8) seen[n++] = d;
        total++;
    }
    if (total == 0) {
        PS2LOG("flush (%s): output buffer already empty, 0 stale byte(s)", when);
    } else {
        PS2LOG("flush (%s): discarded %d stale byte(s): "
               "%02x %02x %02x %02x %02x %02x %02x %02x (first %d shown)",
               when, total,
               n > 0 ? seen[0] : 0, n > 1 ? seen[1] : 0,
               n > 2 ? seen[2] : 0, n > 3 ? seen[3] : 0,
               n > 4 ? seen[4] : 0, n > 5 ? seen[5] : 0,
               n > 6 ? seen[6] : 0, n > 7 ? seen[7] : 0, n);
        PS2LOG("flush (%s): NOTE - a non-zero count here is exactly what made the "
               "old READ_CONFIG return a stale byte instead of the config byte",
               when);
    }
    return total;
}

static void ps2_log_status(const char *when, uint8_t st) {
    PS2LOG("status 0x64 = 0x%02x (%s): OBF=%d IBF=%d SYS=%d A2=%d INH=%d "
           "AUX=%d TIMEOUT=%d PARITY=%d",
           st, when,
           (st & 0x01) ? 1 : 0, (st & 0x02) ? 1 : 0,
           (st & 0x04) ? 1 : 0, (st & 0x08) ? 1 : 0,
           (st & 0x10) ? 1 : 0, (st & 0x20) ? 1 : 0,
           (st & 0x40) ? 1 : 0, (st & 0x80) ? 1 : 0);
}

static void ps2_log_config(const char *when, uint8_t cfg) {
    PS2LOG("config byte %s = 0x%02x: IRQ1=%d IRQ12=%d SYSFLAG=%d "
           "PORT1_CLOCK=%s PORT2_CLOCK=%s TRANSLATE=%s",
           when, cfg,
           (cfg & PS2_CFG_INT1) ? 1 : 0,
           (cfg & PS2_CFG_INT2) ? 1 : 0,
           (cfg & PS2_CFG_SYSFLAG) ? 1 : 0,
           (cfg & PS2_CFG_CLK1_OFF) ? "DISABLED(keyboard dead)" : "enabled",
           (cfg & PS2_CFG_CLK2_OFF) ? "DISABLED(touchpad dead)" : "enabled",
           (cfg & PS2_CFG_XLAT1) ? "on(set 1 to us)" : "OFF(raw set 2 to us)");
}

// Decode the 0xAB / 0xA9 interface-test result. These codes are the same for
// both ports, which is why one decoder serves both.
static const char *ps2_iface_result(uint8_t r) {
    switch (r) {
        case 0x00: return "PASS";
        case 0x01: return "FAIL: clock line stuck low";
        case 0x02: return "FAIL: clock line stuck high";
        case 0x03: return "FAIL: data line stuck low";
        case 0x04: return "FAIL: data line stuck high";
        case 0xFF: return "FAIL: general error / no such port";
        default:   return "FAIL: undocumented code";
    }
}

// Send a byte to the SECOND port's device (0xD4 prefix) and collect the ACK.
static int ps2_aux_cmd(uint8_t cmd, uint8_t *ack) {
    if (ps2_try_cmd(PS2_CMD_WRITE_PORT2) < 0) return -1;
    if (ps2_try_data(cmd) < 0) return -1;
    return ps2_try_read(ack);
}

// Send a byte to the FIRST port's device (no prefix: 0x60 goes straight there).
static int ps2_kbd_cmd(uint8_t cmd, uint8_t *ack) {
    if (ps2_try_data(cmd) < 0) return -1;
    return ps2_try_read(ack);
}

// What answered a PS/2 keyboard identify (0xF2)?
static const char *ps2_kbd_id_name(int len, uint8_t b0, uint8_t b1) {
    if (len == 0) return "ancient AT keyboard (no ID bytes: this is a valid answer)";
    if (len == 1 && b0 == 0x00) return "standard PS/2 mouse answering on port 1 (ports swapped?)";
    if (b0 != 0xAB) return "UNKNOWN (first ID byte is not 0xAB)";
    switch (b1) {
        case 0x83: return "MF2 keyboard";
        case 0x41: case 0xC1: return "MF2 keyboard with translation enabled";
        case 0x84: return "short/thinkpad-style keyboard";
        case 0x85: return "NCD N-97 / 122-key";
        case 0x86: return "122-key host-connect keyboard";
        case 0x90: case 0x91: case 0x92: return "Japanese keyboard";
        default:   return "keyboard, unrecognised subtype";
    }
}

static const char *ps2_aux_id_name(uint8_t id) {
    switch (id) {
        case 0x00: return "standard 3-button PS/2 mouse or a touchpad in default mode";
        case 0x03: return "IntelliMouse (scroll wheel)";
        case 0x04: return "IntelliMouse Explorer (wheel + 5 buttons)";
        default:   return "unrecognised pointing device";
    }
}

// ---------------------------------------------------------------------------
// #ASUSKBD: what does the FIRMWARE DECLARE, as opposed to what answered?
//
// The ASUS boot left us with firmware and hardware contradicting each other:
// the FADT says "no 8042" and port 0x64 answers 0x15. Neither side of that
// contradiction is evidence about what the INTERNAL keyboard actually is. The
// DSDT is, because a firmware that offers a legacy PS/2 keyboard declares a
// device with _HID = EISAID("PNP0303"), and one that offers HID-over-I2C
// declares PNP0C50 instead.
//
// Scanned, not interpreted. See rustkern/amlhid.rs for why a byte-pattern
// presence scan answers the question we actually have and an AML interpreter
// (weeks of work, a large Ring-0 attack surface parsing hostile firmware
// bytecode) is not needed to answer it, and for exactly how much weaker the
// resulting claim is.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t tables_scanned, bytes_scanned;
    uint32_t pnp0303, pnp030b, pnp0320;
    uint32_t pnp0f13, pnp0f03, pnp0f12, pnp0f0e;
    uint32_t pnp0c50, acpi0c50, pnp0a03;
    uint32_t name_ps2k, name_ps2m, name_kbd_, name_tpad, name_etpd;
    uint32_t vendor_elan, vendor_syna, vendor_asue, vendor_ftec;
} aml_hid_report_t;
// FFI layout lock, same discipline as drivers/hotplug.c:31 and usb_audio.c:101.
_Static_assert(sizeof(aml_hid_report_t) == 21 * 4,
               "aml_hid_report_t must match AmlHidReport in rustkern/amlhid.rs");

extern void     amlhid_reset_rs(aml_hid_report_t *out);
extern uint32_t amlhid_scan_rs(const uint8_t *base, uint32_t len, aml_hid_report_t *out);
extern uint32_t amlhid_selftest_rs(void);

static void ps2_report_acpi_declarations(void) {
    uint32_t st = amlhid_selftest_rs();
    if (st != 0) {
        PS2LOG("ACPI _HID scan SELF-TEST FAILED mask=0x%x. The scan below is NOT "
               "trustworthy and its result must be ignored.", st);
        return;
    }
    PS2LOG("ACPI _HID scan self-test PASS (finds a planted PNP0303/PNP0F13/"
           "PNP0A03/PS2K, and finds nothing in a blob that has none)");

    aml_hid_report_t r;
    amlhid_reset_rs(&r);

    uint64_t base = 0; uint32_t len = 0;
    if (acpi_get_dsdt(&base, &len)) {
        (void)amlhid_scan_rs((const uint8_t *)(uintptr_t)base, len, &r);
    } else {
        PS2LOG("ACPI _HID scan: NO VALIDATED DSDT available. Nothing can be said "
               "about what the firmware declares.");
        return;
    }
    // Real firmware splits device declarations across several SSDTs, and
    // acpi_find_table() can only ever return the first of them, so they are
    // enumerated explicitly. Bounded at 32: a machine with more than that is
    // one we are not going to diagnose from a boot log anyway.
    for (int i = 0; i < 32; i++) {
        uint64_t sb = 0; uint32_t sl = 0;
        if (!acpi_get_ssdt(i, &sb, &sl)) break;
        (void)amlhid_scan_rs((const uint8_t *)(uintptr_t)sb, sl, &r);
    }

    PS2LOG("ACPI _HID scan: %u table(s), %u bytes (DSDT + every SSDT)",
           r.tables_scanned, r.bytes_scanned);

    if (r.pnp0a03 == 0) {
        PS2LOG("ACPI _HID scan: POSITIVE CONTROL FAILED. Not one PNP0A03 (PCI "
               "root bus) in %u bytes of AML. Every x86 firmware declares one, "
               "so this is a BROKEN SCAN, not a strange machine. Ignore the "
               "counts below.", r.bytes_scanned);
        return;
    }
    PS2LOG("ACPI _HID scan: positive control OK (%u x PNP0A03 PCI root bus), so "
           "a zero below means ABSENT and not merely unfound", r.pnp0a03);

    PS2LOG("ACPI declares KEYBOARD: PNP0303=%u PNP030B=%u PNP0320=%u  "
           "names PS2K=%u KBD_=%u", r.pnp0303, r.pnp030b, r.pnp0320,
           r.name_ps2k, r.name_kbd_);
    PS2LOG("ACPI declares POINTER : PNP0F13=%u PNP0F03=%u PNP0F12=%u PNP0F0E=%u  "
           "names PS2M=%u TPAD=%u ETPD=%u", r.pnp0f13, r.pnp0f03, r.pnp0f12,
           r.pnp0f0e, r.name_ps2m, r.name_tpad, r.name_etpd);
    PS2LOG("ACPI declares I2C-HID : PNP0C50=%u ACPI0C50=%u  vendor strings "
           "ELAN=%u SYNA=%u ASUE=%u FTE=%u", r.pnp0c50, r.acpi0c50,
           r.vendor_elan, r.vendor_syna, r.vendor_asue, r.vendor_ftec);

    unsigned kbd = r.pnp0303 + r.pnp030b + r.pnp0320;
    unsigned ptr = r.pnp0f13 + r.pnp0f03 + r.pnp0f12 + r.pnp0f0e;
    unsigned i2c = r.pnp0c50 + r.acpi0c50;

    if (kbd > 0)
        PS2LOG("FIRMWARE VERDICT (keyboard): this firmware DECLARES a legacy PS/2 "
               "keyboard, so PS/2 is the right route for the internal keyboard "
               "and the FADT 'no 8042' bit contradicts the firmware's own DSDT. "
               "CAVEAT: a declaration is not an _STA evaluation; the device could "
               "still be declared and then reported absent.");
    else if (i2c > 0)
        PS2LOG("FIRMWARE VERDICT (keyboard): NO PS/2 keyboard declared, but an "
               "I2C-HID device IS. PS/2 is a DEAD END for the internal keyboard "
               "on this machine and an I2C-HID path would be needed. NOTE this "
               "machine has NO LPSS I2C controller on PCI (00:15.x absent), so "
               "if this fires, re-check that finding: the two disagree.");
    else
        PS2LOG("FIRMWARE VERDICT (keyboard): NO legacy PS/2 keyboard and NO "
               "I2C-HID device declared anywhere in the AML. The internal "
               "keyboard is reached by neither route we know how to drive.");

    if (ptr > 0)
        PS2LOG("FIRMWARE VERDICT (pointer): a legacy PS/2 pointing device IS "
               "declared, so the internal touchpad should be reachable on the "
               "SECOND i8042 port.");
    else if (i2c > 0)
        PS2LOG("FIRMWARE VERDICT (pointer): no PS/2 pointing device declared; an "
               "I2C-HID device is. The touchpad is most likely I2C-HID, i.e. a "
               "DIFFERENT route from the keyboard, and the two must be answered "
               "separately.");
    else
        PS2LOG("FIRMWARE VERDICT (pointer): no PS/2 and no I2C-HID pointing "
               "device declared in the AML.");
}

// Bring the i8042 up and SAY WHAT HAPPENED. Returns 1 if a controller answered,
// 0 if it did not. Fills the two device-present flags.
static int ps2_bringup(int *kbd_present, int *aux_present) {
    uint8_t b = 0, cfg = 0;
    int dual_channel = 0;

    *kbd_present = 0;
    *aux_present = 0;

    // FIRST, before touching a port: what does the firmware itself declare? This
    // is the half of the question that no amount of poking at 0x60/0x64 can
    // answer, and it is what tells us whether PS/2 is even the right route.
    ps2_report_acpi_declarations();

    // -- 1. Is anything driving the port? -----------------------------------
    // A single sample cannot tell a real controller from a floating bus, so take
    // four. An undriven bus is a CONSTANT (0xFF on nearly every chipset, 0x00 on
    // a few); a real controller's OBF/IBF bits move as we talk to it. The ASUS
    // sample of 0x15 was ONE read printed TWICE, which is not the same as a
    // stable reading and was never evidence of stability.
    uint8_t s[4];
    for (int i = 0; i < 4; i++) {
        s[i] = inb(PS2_STATUS_PORT);
        for (volatile int d = 0; d < 1000; d++) { __asm__ volatile("pause"); }
    }
    PS2LOG("status samples: %02x %02x %02x %02x (%s)",
           s[0], s[1], s[2], s[3],
           (s[0] == s[1] && s[1] == s[2] && s[2] == s[3]) ? "STABLE" : "CHANGING");
    ps2_log_status("first read", s[0]);

    int fadt = ps2_fadt_verdict();
    PS2LOG("ACPI FADT IAPC_BOOT_ARCH verdict: %s",
           fadt < 0 ? "8042 NOT PRESENT (bit 1 set)"
                    : (fadt > 0 ? "8042 present (bit 1 clear)"
                                : "no opinion (no FADT, too short, or ACPI 1.0)"));

    if (ps2_status_is_floating(s[0]) && ps2_status_is_floating(s[3])) {
        ps2_mark_absent("port 0x64 reads 0xFF on every sample, undriven bus");
        PS2LOG("VERDICT: NO CONTROLLER. Reason: 0x64 is a stable 0xFF, which no "
               "real 8042 ever returns (it would mean OBF+IBF+SYS+A2+INH+AUX+"
               "TIMEOUT+PARITY all asserted at once). Internal keyboard and "
               "internal touchpad are BOTH unreachable by this route.");
        return 0;
    }
    if (fadt < 0 && s[0] == 0x00 && s[3] == 0x00) {
        ps2_mark_absent("FADT says absent and port 0x64 reads a stable 0x00");
        PS2LOG("VERDICT: NO CONTROLLER. Reason: firmware says absent AND the bus "
               "reads a stable 0x00. Internal keyboard and touchpad are BOTH "
               "unreachable by this route.");
        return 0;
    }
    if (fadt < 0) {
        PS2LOG("firmware and hardware DISAGREE: FADT says no 8042 but 0x64 answers "
               "0x%02x. Probing anyway, as Linux does, because this bit is set "
               "wrongly by a great many UEFI firmwares. The self-test below is "
               "the decisive evidence, not this bit and not that byte.", s[0]);
    }

    // -- 2. Quiesce, then FLUSH ----------------------------------------------
    // Disable BOTH ports first so a device cannot inject a byte between our
    // flush and our config read. This is the step whose absence caused the bug.
    if (ps2_try_cmd(PS2_CMD_DISABLE_PORT1) < 0) {
        PS2LOG("VERDICT: NO USABLE CONTROLLER. Reason: input buffer never cleared "
               "(IBF stuck set) when sending 0xAD (disable port 1), so no command "
               "can be delivered at all. Internal keyboard and touchpad BOTH "
               "unreachable.");
        ps2_mark_absent("IBF permanently set: cannot deliver a command");
        return 0;
    }
    (void)ps2_try_cmd(PS2_CMD_DISABLE_PORT2);
    ps2_flush("after quiescing both ports");

    // -- 3. Read the config byte, now that it is trustworthy ------------------
    if (ps2_try_cmd(PS2_CMD_READ_CONFIG) < 0 || ps2_try_read(&cfg) < 0) {
        PS2LOG("VERDICT: NO USABLE CONTROLLER. Reason: command 0x20 (read config) "
               "produced no reply within the bounded wait. Internal keyboard and "
               "touchpad BOTH unreachable.");
        ps2_mark_absent("read-config (0x20) never answered");
        return 0;
    }
    ps2_log_config("as firmware left it", cfg);
    if (cfg & PS2_CFG_CLK1_OFF)
        PS2LOG("NOTE: firmware handed us the FIRST port with its clock DISABLED. "
               "Nothing in this kernel used to re-enable it, which is sufficient "
               "on its own to explain a dead internal keyboard.");
    if (cfg & PS2_CFG_CLK2_OFF)
        PS2LOG("NOTE: firmware handed us the SECOND port with its clock DISABLED "
               "(internal touchpad).");

    // -- 4. Controller self-test: the decisive presence evidence --------------
    // 0x55 is a value nothing but a real 8042 produces in response to 0xAA. This,
    // and not the status byte, settles whether the controller exists.
    if (ps2_try_cmd(PS2_CMD_SELFTEST) < 0 || ps2_try_read(&b) < 0) {
        PS2LOG("VERDICT: NO CONTROLLER. Reason: self-test 0xAA got no reply at all "
               "within the bounded wait. Something answers reads on 0x64 but does "
               "not implement the 8042 command set. Internal keyboard and touchpad "
               "BOTH unreachable by this route.");
        ps2_mark_absent("self-test 0xAA never answered");
        return 0;
    }
    if (b != 0x55) {
        PS2LOG("VERDICT: NO USABLE CONTROLLER. Reason: self-test 0xAA answered "
               "0x%02x, expected 0x55 (0xFC means the controller failed its own "
               "self-test). Internal keyboard and touchpad BOTH unreachable.", b);
        ps2_mark_absent("self-test 0xAA did not answer 0x55");
        return 0;
    }
    PS2LOG("controller self-test 0xAA -> 0x55 PASS. An 8042 IS PRESENT on this "
           "machine, whatever the FADT says.");

    // A passing self-test resets the controller on many chips, which silently
    // discards the config byte. Write it back before doing anything else.
    if (ps2_try_cmd(PS2_CMD_WRITE_CONFIG) >= 0) (void)ps2_try_data(cfg);
    ps2_flush("after self-test");

    // -- 5. Is there a SECOND port at all? ------------------------------------
    // Enable port 2 and re-read the config: on a dual-channel controller the
    // CLK2_OFF bit clears as a side effect of 0xA8. On a single-channel
    // controller it stays set, because there is no second port to enable.
    if (ps2_try_cmd(PS2_CMD_ENABLE_PORT2) >= 0 &&
        ps2_try_cmd(PS2_CMD_READ_CONFIG) >= 0 && ps2_try_read(&b) >= 0) {
        dual_channel = (b & PS2_CFG_CLK2_OFF) ? 0 : 1;
        PS2LOG("dual-channel test: config after 0xA8 = 0x%02x -> SECOND PORT %s",
               b, dual_channel ? "EXISTS (touchpad can live here)"
                               : "DOES NOT EXIST (single-channel controller)");
    } else {
        PS2LOG("dual-channel test: inconclusive (0xA8/0x20 did not answer); "
               "assuming single channel");
    }
    (void)ps2_try_cmd(PS2_CMD_DISABLE_PORT2);

    // -- 6. Per-port interface tests, reported SEPARATELY ---------------------
    int port1_ok = 0, port2_ok = 0;
    if (ps2_try_cmd(PS2_CMD_TEST_PORT1) >= 0 && ps2_try_read(&b) >= 0) {
        port1_ok = (b == 0x00);
        PS2LOG("FIRST port (keyboard) interface test 0xAB -> 0x%02x: %s",
               b, ps2_iface_result(b));
    } else {
        PS2LOG("FIRST port (keyboard) interface test 0xAB: NO REPLY (timeout)");
    }
    if (dual_channel) {
        if (ps2_try_cmd(PS2_CMD_TEST_PORT2) >= 0 && ps2_try_read(&b) >= 0) {
            port2_ok = (b == 0x00);
            PS2LOG("SECOND port (touchpad) interface test 0xA9 -> 0x%02x: %s",
                   b, ps2_iface_result(b));
        } else {
            PS2LOG("SECOND port (touchpad) interface test 0xA9: NO REPLY (timeout)");
        }
    } else {
        PS2LOG("SECOND port (touchpad) interface test SKIPPED: controller is "
               "single-channel, so there is no second port to test");
    }

    if (!port1_ok && !port2_ok) {
        PS2LOG("VERDICT: CONTROLLER PRESENT BUT NO USABLE PORT. Reason: the "
               "self-test passed but neither interface test did. Internal "
               "keyboard and touchpad are BOTH unreachable.");
        return 1;
    }

    // -- 7. Enable the ports that passed, and write a config byte we CHOSE -----
    if (port1_ok) (void)ps2_try_cmd(PS2_CMD_ENABLE_PORT1);
    if (port2_ok) (void)ps2_try_cmd(PS2_CMD_ENABLE_PORT2);

    uint8_t want = cfg;
    if (port1_ok) { want |= PS2_CFG_INT1; want &= (uint8_t)~PS2_CFG_CLK1_OFF; }
    if (port2_ok) { want |= PS2_CFG_INT2; want &= (uint8_t)~PS2_CFG_CLK2_OFF; }
    // Translation ON is not a preference: cpu/isr.c's keyboard_process_scancode()
    // decodes scancode set 1 only, and the canonical way to get set 1 is a
    // keyboard in set 2 with the controller translating. Step 8 sets the device
    // side to match, so the two halves cannot disagree.
    want |= PS2_CFG_XLAT1;
    want &= (uint8_t)~0x80;   // bit 7 is reserved and must be written zero

    if (ps2_try_cmd(PS2_CMD_WRITE_CONFIG) < 0 || ps2_try_data(want) < 0) {
        PS2LOG("VERDICT: DEGRADED. Reason: could not WRITE the config byte "
               "(0x60) we computed (0x%02x). Ports keep whatever firmware left.",
               want);
        return 1;
    }
    // Read it back. A write we did not verify is a claim, not a measurement.
    if (ps2_try_cmd(PS2_CMD_READ_CONFIG) >= 0 && ps2_try_read(&b) >= 0) {
        ps2_log_config("written back and re-read", b);
        if (b != want)
            PS2LOG("WARNING: config read-back 0x%02x != written 0x%02x. The "
                   "controller silently altered or ignored bits.", b, want);
    }

    // -- 8. FIRST port device: reset, identify, scancode set, enable ----------
    if (port1_ok) {
        if (ps2_kbd_cmd(0xFF, &b) == 0) {          // reset
            PS2LOG("FIRST port device reset 0xFF -> 0x%02x (%s)", b,
                   b == 0xFA ? "ACK" : (b == 0xFE ? "RESEND/NAK" : "unexpected"));
            if (b == 0xFA && ps2_try_read(&b) == 0) {
                PS2LOG("FIRST port device self-test result -> 0x%02x (%s)", b,
                       b == 0xAA ? "BAT PASS, a keyboard is physically attached"
                                 : "BAT FAIL or not a keyboard");
                // A device that passed its own self-test IS there, whatever it
                // does about identify below. Deciding presence on identify alone
                // would report a working-but-terse keyboard as absent.
                if (b == 0xAA) *kbd_present = 1;
            }
        } else {
            PS2LOG("FIRST port device reset 0xFF: NO REPLY. Reason: nothing is "
                   "attached to the first port, or it is not answering.");
        }

        (void)ps2_kbd_cmd(0xF5, &b);               // disable scanning while we ask
        uint8_t id0 = 0, id1 = 0; int idlen = 0;
        if (ps2_kbd_cmd(0xF2, &b) == 0 && b == 0xFA) {
            if (ps2_try_read(&id0) == 0) { idlen = 1;
                if (ps2_try_read(&id1) == 0) idlen = 2; }
            PS2LOG("FIRST port identify 0xF2 -> %d byte(s): %02x %02x = %s",
                   idlen, id0, id1, ps2_kbd_id_name(idlen, id0, id1));
            *kbd_present = 1;
        } else {
            PS2LOG("FIRST port identify 0xF2: NO ACK. Nothing usable on the "
                   "keyboard port.");
        }

        if (*kbd_present) {
            // Put the device in set 2 so the translation bit set in step 7
            // delivers set 1 to keyboard_process_scancode(). Query first, report
            // both, and only then change it, so the log records what firmware
            // had chosen as well as what we did.
            if (ps2_kbd_cmd(0xF0, &b) == 0 && b == 0xFA) {
                (void)ps2_try_data(0x00);
                if (ps2_try_read(&b) == 0 && b == 0xFA && ps2_try_read(&b) == 0)
                    PS2LOG("FIRST port scancode set as firmware left it: raw "
                           "byte 0x%02x (NOTE: with controller translation on, "
                           "many controllers translate this reply too, so 0x43 "
                           "here means set 2 and 0x41 means set 3; the raw byte "
                           "is printed rather than a guess)", b);
            }
            if (ps2_kbd_cmd(0xF0, &b) == 0 && b == 0xFA) {
                (void)ps2_try_data(0x02);
                (void)ps2_try_read(&b);
                PS2LOG("FIRST port scancode set -> 2 (with controller translation "
                       "on, that reaches us as set 1): %s",
                       b == 0xFA ? "ACK" : "NOT ACKED");
            }
        }

        // OUTSIDE the identify branch, deliberately. 0xF5 (disable scanning) was
        // sent above BEFORE identify; leaving the re-enable inside a branch that
        // identify can skip would mean a keyboard which merely declines to
        // identify is left with SCANNING DISABLED BY US. That is precisely the
        // class of bug this whole change exists to remove, so the last thing
        // done to the first port is always to turn it back on.
        if (ps2_kbd_cmd(0xF4, &b) == 0)
            PS2LOG("FIRST port enable scanning 0xF4 -> 0x%02x (%s)", b,
                   b == 0xFA ? "ACK - the internal keyboard should now send "
                               "scancodes on IRQ1" : "NOT ACKED");
        else
            PS2LOG("FIRST port enable scanning 0xF4: NO REPLY");
    }

    // -- 9. SECOND port device: reset, identify, and ask what KIND of pad -----
    if (port2_ok) {
        if (ps2_aux_cmd(0xFF, &b) == 0 && b == 0xFA) {
            uint8_t bat = 0, devid = 0;
            int got_bat = (ps2_try_read(&bat) == 0);
            int got_id  = (ps2_try_read(&devid) == 0);
            PS2LOG("SECOND port device reset 0xFF -> ACK, BAT=%s0x%02x, ID=%s0x%02x (%s)",
                   got_bat ? "" : "(none)", bat,
                   got_id ? "" : "(none)", devid,
                   got_id ? ps2_aux_id_name(devid) : "no ID byte returned");
            *aux_present = 1;
        } else {
            PS2LOG("SECOND port device reset 0xFF: NO ACK. Reason: nothing is "
                   "attached to the second port, so there is no PS/2 touchpad "
                   "here even though the port itself tested good.");
        }

        if (*aux_present) {
            // Synaptics/Elan identify knock: four SET-RESOLUTION commands each
            // carrying a 2-bit argument, then STATUS REQUEST. A Synaptics pad
            // answers with its identity (middle byte 0x47) instead of an ordinary
            // status packet; a plain mouse answers an ordinary status packet.
            // READ-ONLY: it selects no mode, so it is safe against a plain mouse.
            int knock_ok = 1;
            for (int i = 0; i < 4 && knock_ok; i++) {
                if (ps2_aux_cmd(0xE8, &b) != 0 || b != 0xFA) { knock_ok = 0; break; }
                if (ps2_aux_cmd(0x00, &b) != 0 || b != 0xFA) { knock_ok = 0; break; }
            }
            if (knock_ok && ps2_aux_cmd(0xE9, &b) == 0 && b == 0xFA) {
                uint8_t k0 = 0, k1 = 0, k2 = 0;
                int n = 0;
                if (ps2_try_read(&k0) == 0) { n = 1;
                    if (ps2_try_read(&k1) == 0) { n = 2;
                        if (ps2_try_read(&k2) == 0) n = 3; } }
                PS2LOG("SECOND port extended identify (E8 00 x4, E9) -> %d byte(s): "
                       "%02x %02x %02x = %s", n, k0, k1, k2,
                       (n == 3 && k1 == 0x47)
                           ? "SYNAPTICS touchpad (needs the Synaptics extended "
                             "init for absolute mode; it will still work as a "
                             "plain relative PS/2 mouse without it)"
                           : "not Synaptics; an ordinary PS/2 status packet, so "
                             "plain relative mouse mode is the right driver");
            } else {
                PS2LOG("SECOND port extended identify: knock not acknowledged; "
                       "treat as a plain relative PS/2 pointing device");
            }
        }
    }

    // -- 10. FINAL VERDICT, with a reason, naming BOTH devices ----------------
    PS2LOG("VERDICT: 8042 PRESENT (self-test 0x55). INTERNAL KEYBOARD: %s. "
           "INTERNAL POINTING DEVICE: %s. Controller is %s-channel.",
           *kbd_present ? "FOUND and enabled on the first port"
                        : (port1_ok ? "port tested good but NO DEVICE answered"
                                    : "first port FAILED its interface test"),
           *aux_present ? "FOUND on the second port"
                        : (dual_channel
                             ? (port2_ok ? "second port good but NO DEVICE answered"
                                         : "second port FAILED its interface test")
                             : "NO SECOND PORT on this controller"),
           dual_channel ? "dual" : "single");
    return 1;
}

int mouse_init(void) {
    kprintf("[MOUSE] Initializing PS/2 mouse...\n");

    // Initialize state
    memset(&g_mouse, 0, sizeof(g_mouse));
    g_mouse.x = mouse_max_x / 2;
    g_mouse.y = mouse_max_y / 2;

    // #745 (local 102): display rotation input note. This deliberately needed
    // NO functional change for rotation. Every input path in this file (the
    // PS/2 packet decode above and mouse_inject_hid() below, the only two
    // producers of g_mouse.dx/dy - USB HID and PS/2 share this one
    // accumulator, there is no second mouse state struct) is RELATIVE-delta:
    // a mouse's own dx/dy carries no reference to the panel's physical
    // mounting angle, only to the device's own sensor axes, so "move the
    // mouse right" already means "cursor moves right on whatever the user is
    // looking at" with no transform needed, once the screen itself is
    // presented correctly rotated (video/framebuffer.c). fb_get_width()/
    // fb_get_height() below are LOGICAL (post-rotation) as of fb_init(), so
    // the clamp bounds below are automatically correct for the active
    // rotation with zero changes here.
    //
    // The one input CLASS that WOULD need a transform is a PANEL-referenced
    // absolute coordinate source (a touch digitizer, or an absolute-HID
    // tablet report calibrated to the physical scanout) - this tree has
    // neither today (grepped: no ABS_X/tablet/digitizer HID parsing anywhere
    // under kernel/drivers). If one is ever added, its inverse-rotation
    // belongs in mouse_set_position() below (the one absolute-coordinate
    // entry point both sys_set_mouse() and sys_inject_mouse() already funnel
    // through), using fb_get_rotation() - NOT here, and NOT in every future
    // absolute-input driver individually.
    //
    // VNC (userland/apps/compositor/vnc.c) and the compositor's own cursor
    // warp were checked too: both read/write g_fb_width/g_fb_height (LOGICAL,
    // via SYS_FB_INFO), so they already operate in the correctly-rotated
    // space with no change needed either.

    // Set bounds from framebuffer
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    if (fb_w > 0 && fb_h > 0) {
        mouse_max_x = fb_w - 1;
        mouse_max_y = fb_h - 1;
        g_mouse.x = fb_w / 2;
        g_mouse.y = fb_h / 2;
    }

    // ------------------------------------------------------------------
    // Presence gate AND full controller bring-up. Deliberately placed AFTER the
    // framebuffer bounds block above and BEFORE the first device command: on a
    // machine with no 8042 the USB HID path (mouse_inject_hid) is the only
    // mouse, and it clamps against mouse_max_x/mouse_max_y, so those must be
    // set even when the whole PS/2 probe is then skipped.
    //
    // ps2_bringup() flushes the output buffer, self-tests the controller, tests
    // each port SEPARATELY, writes a config byte whose every bit we chose, and
    // brings up the internal KEYBOARD as well as the internal pointing device.
    // Every step reaches /BOOTLOG.TXT. See the long comment above it for the
    // stale-config-byte bug this replaces and why it killed both devices.
    //
    // ORDERING NOTE: the keyboard is enabled INSIDE ps2_bringup(), before any
    // mouse command runs, and deliberately so. The internal keyboard must not
    // depend on the mouse init sequence succeeding; every early return below
    // used to take the keyboard down with it. The cost is a few milliseconds in
    // which a keystroke could be misread as a mouse ACK and abort the MOUSE
    // init. That is the correct direction for the risk to point.
    {
        int kbd_present = 0, aux_present = 0;
        if (!ps2_bringup(&kbd_present, &aux_present)) {
            // ps2_bringup() has already logged a VERDICT line saying exactly why,
            // and called ps2_mark_absent() where that was the finding.
            return -1;
        }
        if (!aux_present) {
            kprintf("[MOUSE] no PS/2 pointing device on the second port; skipping "
                    "the mouse init sequence (USB HID is unaffected)\n");
            bootlog_write("[MOUSE] no PS/2 pointing device on the second port; "
                          "mouse init skipped (USB HID unaffected)");
            return -1;
        }
    }


    // Reset mouse
    uint8_t response = mouse_send_cmd(MOUSE_CMD_RESET);
    if (response != MOUSE_ACK) {
        kprintf("[MOUSE] Reset failed (response: 0x%02x)\n", response);
        return -1;
    }

    // Wait for self-test
    ps2_read_data();  // Should be 0xAA (test passed)
    ps2_read_data();  // Should be 0x00 (device ID)

    // Try to enable scroll wheel
    mouse_has_wheel = mouse_enable_wheel();
    if (mouse_has_wheel) {
        kprintf("[MOUSE] Scroll wheel enabled\n");
    }

    // Set sample rate to the PS/2 maximum (200 samples/sec) for smooth, low
    // latency cursor motion. This runs AFTER the IntelliMouse wheel-enable magic
    // sequence (which leaves the rate at 80), so it is the effective final rate.
    // (desktop UX pass - mouse feel)
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(200);  // 200 samples/sec (max)

    // Set resolution
    mouse_send_cmd(MOUSE_CMD_SET_RES);
    mouse_send_cmd(3);  // 8 counts/mm

    // Set scaling 1:1
    mouse_send_cmd(MOUSE_CMD_SET_SCALE11);

    // Enable data reporting
    response = mouse_send_cmd(MOUSE_CMD_ENABLE);
    if (response != MOUSE_ACK) {
        kprintf("[MOUSE] Enable failed\n");
        return -1;
    }

    // Register interrupt handler (IRQ 12 = INT 44)
    idt_register_handler(44, mouse_handler);

    // Enable IRQ 12 in PIC
    pic_enable_irq(12);

    kprintf("[MOUSE] Initialized at (%d, %d), bounds 0,0 to %d,%d\n",
            g_mouse.x, g_mouse.y, mouse_max_x, mouse_max_y);

    return 0;
}

// Get current mouse state
void mouse_get_state(mouse_state_t *state) {
    if (state) {
        *state = g_mouse;
    }
}

// Get current mouse state and clear deltas (for exclusive consumers like DOOM)
void mouse_get_state_and_clear(mouse_state_t *state) {
    __asm__ volatile("cli");
    if (state) {
        *state = g_mouse;
    }
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.scroll = 0;
    __asm__ volatile("sti");
}

// Test/automation hook: synthesize an absolute cursor position + button state.
// The compositor polls mouse_get_state() every frame and replays it through the
// real input path (sys_inject_mouse), so this produces a genuine click. Used by
// the RemoteCtrl `click x y` command for headless click testing.
// (#speedcap) THE ONE PLACE the synthetic button LEVEL is written. Split out of
// mouse_inject_button() below rather than copied, because the left-only version
// could not express a RIGHT click and every per-window context menu in this tree
// (the taskbar tile popup, contextmenu.c's CTX_MODE_DOCK, and therefore the #778
// DOS Speed dialog) opens on one. A second copy of this five-line critical
// section is exactly the fork CLAUDE.md's reuse rule exists to prevent, so
// mouse_inject_button() is now a wrapper and there is still one implementation.
//
// The mask argument is the PS/2 packet convention drivers/mouse.c already stores, and the
// compositor already tests: bit0 = left, bit1 = right, bit2 = middle. It is the
// WHOLE new level, not a set/clear, so releasing is mask 0 for either button.
void mouse_inject_button_mask(int32_t x, int32_t y, uint32_t mask) {
    __asm__ volatile("cli");
    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.x = x; g_mouse.y = y;
    g_mouse.buttons = mask & 7u;
    mouse_x = x; mouse_y = y; mouse_buttons = g_mouse.buttons;
    g_mouse_synth_until = timer_ticks + 60;   /* ~240ms at 250Hz */
    __asm__ volatile("sti");
}

void mouse_inject_button(int32_t x, int32_t y, int down) {
    // #197: the INJECTED leg of the click ledger (rustkern/clickacct.rs). One
    // relaxed atomic add; it is outside the cli/sti window below so the
    // critical section stays exactly as short as it was. It stays LEFT-only, as
    // its own doc comment in clickacct.rs says: the right button is not part of
    // the inject/routed/hit ledger, only of the SAMPLED leg that proves
    // delivery.
    extern void clickacct_note_inject_rs(int down);
    clickacct_note_inject_rs(down);
    mouse_inject_button_mask(x, y, down ? 1u : 0u);
}

// (Win16 SkiFree idle-freeze repro, #200-follow-on) Test/automation hook: move
// the synthetic cursor WITHOUT touching button state. mouse_inject_button()
// always forces g_mouse.buttons = down?1:0, so it cannot express "the mouse is
// hovering, no button involved" -- every call is a click edge. A real hardware
// mouse move never changes button state, and this repeatedly-called-with-
// unchanged-position path is exactly what a host cursor sitting over a window
// generates (continuous WM_MOUSEMOVE with no button transitions), which is the
// scenario needed to reproduce/verify the Win16 message-queue idle-starvation
// bug (an app's PeekMessage-driven idle/animate branch never seeing an empty
// queue while the cursor hovers). Touches the exact same globals
// mouse_inject_button does, so every consumer (win16_pump_mouse included) sees
// an identical move to real hardware; buttons are left exactly as they were.
void mouse_inject_move(int32_t x, int32_t y) {
    __asm__ volatile("cli");
    g_mouse.x = x; g_mouse.y = y;
    mouse_x = x; mouse_y = y;
    g_mouse_synth_until = timer_ticks + 60;   /* ~240ms at 250Hz */
    __asm__ volatile("sti");
}

// #307: inject a USB-HID boot mouse report (relative deltas + button bitmap +
// wheel). Feeds the SAME g_mouse state that the PS/2 packet path updates, so the
// compositor treats USB and PS/2 mice identically. HID Y is positive-down, the
// same as screen coordinates, so (unlike PS/2) it is NOT inverted here.
void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel) {
    __asm__ volatile("cli");
    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.buttons = buttons & 0x07;

    g_mouse.dx += dx;
    g_mouse.dy += dy;
    g_mouse.scroll = (int8_t)wheel;

    int sx = 0, sy = 0;
    mouse_scale_delta(dx, dy, &sx, &sy);   // remainder-carrying, see above
    g_mouse.x += sx;
    g_mouse.y += sy;

    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
    mouse_buttons = g_mouse.buttons;
    __asm__ volatile("sti");
}

// Get mouse position
void mouse_get_position(int32_t *x, int32_t *y) {
    if (x) *x = g_mouse.x;
    if (y) *y = g_mouse.y;
}

// Set mouse position. LOGICAL (post-rotation) coordinates in, always - both
// callers (sys_set_mouse: compositor cursor warp, sys_inject_mouse: VNC/
// automation click injection) already operate in logical space (#745 local
// 102 - see the note above mouse_init()'s fb_get_width() call). A future
// PANEL-referenced absolute input source (touch digitizer, absolute HID
// tablet) is the one thing that would need to inverse-rotate via
// fb_get_rotation() BEFORE calling this - not implemented, because no such
// driver exists in this tree yet.
void mouse_set_position(int32_t x, int32_t y) {
    g_mouse.x = x;
    g_mouse.y = y;

    // Clamp
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
}

// Set mouse bounds
void mouse_set_bounds(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y) {
    mouse_min_x = min_x;
    mouse_min_y = min_y;
    mouse_max_x = max_x;
    mouse_max_y = max_y;

    // Re-clamp current position
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
}

// Check if button is pressed
int mouse_button_pressed(uint8_t button) {
    return (g_mouse.buttons & button) != 0;
}

// Get current button state
uint8_t mouse_get_buttons(void) {
    return g_mouse.buttons;
}

// Check if button was just clicked
int mouse_button_clicked(uint8_t button) {
    return (g_mouse.buttons & button) && !(g_mouse.prev_buttons & button);
}

// Check if button was just released
int mouse_button_released(uint8_t button) {
    return !(g_mouse.buttons & button) && (g_mouse.prev_buttons & button);
}

// Get scroll wheel delta and clear it
int8_t mouse_get_scroll(void) {
    int8_t scroll = g_mouse.scroll;
    g_mouse.scroll = 0;
    return scroll;
}

// Check if scroll wheel is supported
int mouse_has_scroll_wheel(void) {
    return mouse_has_wheel;
}

// Poll mouse (for systems without interrupts working).
//
// BOUNDED (#426): at most MOUSE_POLL_DRAIN_MAX bytes per call, plus an explicit
// floating-bus check, so a machine with no 8042 degrades to a latched early
// return instead of hanging the fallback desktop forever. The old loop was
// `while (status & 0x01)` with no counter and no timeout; see the "IS THERE AN
// 8042 AT ALL?" block above for why 0xFF made that unable to terminate.
//
// Shape deliberately mirrors cpu/isr.c's keyboard_poll_i8042(), the sibling
// bounded drain over the same controller: a counted for-loop, re-read the status
// byte at the top of every iteration, and return the moment OBF is clear.
//
// WHY 64. The 8042 output buffer holds exactly ONE byte, so a healthy controller
// can never present more than one byte per read: the cap only has to cover the
// BACKLOG that accumulates between two calls. mouse_init() caps the device at
// 200 samples/sec at 3 or 4 bytes each, so 64 bytes is 16 to 21 whole packets,
// about 80 to 105 ms of stream drained in a single call. That is far more than
// any plausible fallback-desktop frame interval, and it is 2x the sibling
// keyboard cap (KBD_POLL_DRAIN_MAX = 32 in cpu/isr.c) for the obvious reason
// that a mouse packet is 3 to 4 bytes where a scancode is 1 to 2. Tripping it is
// never fatal: whatever is left is drained by the next call, so the worst case
// is cursor lag, not a hang, which is why the give-up line says so plainly.
#define MOUSE_POLL_DRAIN_MAX 64

void mouse_poll(void) {
    if (g_ps2_absent) return;   // latched: one compare, then out

    for (int i = 0; i < MOUSE_POLL_DRAIN_MAX; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (ps2_status_is_floating(status)) {
            ps2_mark_absent("port 0x64 reads 0xFF, undriven bus");
            return;
        }
        if (!(status & 0x01)) return;   // OBF clear: nothing left to drain

        uint8_t data = inb(PS2_DATA_PORT);

        if (!(status & 0x20)) {
            // Data from the KEYBOARD. ROUTE IT, do not throw it away.
            //
            // This byte used to be read and DISCARDED. Normally IRQ1 wins the
            // race and this branch almost never runs, which is why nobody
            // noticed; but on a board where IRQ1 delivery does not work while
            // the 8042 does, the fallback desktop silently destroyed EVERY
            // keystroke. keyboard_process_scancode() (cpu/isr.c:619) is THE
            // single funnel every scancode source in this kernel goes through
            // (PS/2 IRQ1, the polled i8042 drain, USB HID, Bluetooth HID and the
            // #334 test channel), so feeding it here gives the polled mouse path
            // the same cooked ring buffer, the same shift/ctrl/caps state and
            // the same DOS scancode tap as every other source, with no second
            // divergent keyboard path.
            //
            // SAFE FROM THIS CONTEXT: cpu/isr.c's keyboard_poll_i8042() already
            // does exactly this from early boot with IF=0, and documents that
            // the function takes no lock and touches only that file's statics.
            // mouse_poll() runs in thread context (gui/desktop.c:3569 and :3830,
            // gui/filebrowser.c:1486, gui/settings.c:896), which is strictly
            // less constrained, so it cannot block or deadlock its caller.
            //
            // HONEST RESIDUAL, UNCHANGED BY THIS EDIT: if IRQ1 was already
            // latched when we consumed the byte, keyboard_handler() will still
            // run and still read port 0x60 unconditionally with the buffer
            // empty. That race predates this change (the discard had it too);
            // what changes is only that the keystroke is no longer LOST.
            keyboard_process_scancode(data);
            continue;
        }

        // Data from the mouse.
        if (mouse_packet_idx == 0 && !(data & 0x08)) continue;   // resync

        mouse_packet[mouse_packet_idx++] = data;

        int packet_size = mouse_has_wheel ? 4 : 3;
        if (mouse_packet_idx >= packet_size) {
            mouse_process_packet();
            mouse_packet_idx = 0;
        }
    }

    // Every other exit above is a return, so reaching here means the cap tripped.
    if (!g_ps2_drain_logged) {
        g_ps2_drain_logged = 1;
        kprintf("[PS2] *** mouse_poll drain cap hit: %d bytes in one call and the "
                "8042 still reports data. Deferring the rest to the next call "
                "(cursor lag, not a hang). If this repeats, IRQ12 is not being "
                "delivered or the controller is stuck. ***\n",
                MOUSE_POLL_DRAIN_MAX);
        bootlog_write("[PS2] mouse_poll drain cap %d hit, deferring remainder",
                      MOUSE_POLL_DRAIN_MAX);
    }
}

// Check if mouse data available.
// Honours the same latch: on a floating bus (0x64 == 0xFF) the raw test would
// answer "yes" forever, so any caller that looped on it would hang exactly the
// way mouse_poll() used to. No caller does today, which is precisely why it
// needs the guard before one appears.
int mouse_has_data(void) {
    if (g_ps2_absent) return 0;
    uint8_t status = inb(PS2_STATUS_PORT);
    if (ps2_status_is_floating(status)) {
        ps2_mark_absent("port 0x64 reads 0xFF, undriven bus");
        return 0;
    }
    return (status & 0x21) == 0x21;  // Data available and from mouse
}
