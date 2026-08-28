// xhci.c - xHCI (USB 3.0) Host Controller Driver
// Full implementation with transfer rings, device contexts, and enumeration
#include "xhci.h"
#include "usb.h"
#include "usb_audio.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../fs/bootlog.h"
#include "../fs/fat.h"      // #156: fat_exists() for the /XHCIMSI.* ESP gate
#include "../proc/process.h"
#include "../cpu/mono.h"    // #507/#525: THE shared clock + busy-delay (no private PIT loop here)
#include "../cpu/idt.h"     // #139: idt_register_handler / interrupt_frame_t for the MSI handler
#include "../cpu/apic.h"    // #139: lapic_eoi / lapic_get_id
#include "../sync/waitq.h"
#include "usb_hid.h"            // #134: HID slot teardown on disconnect
#include "../sync/noblock.h"   // #614: g_xhci_evt_wq, the event-ring wait queue   // #433: proc_create/PRIO_* for the re-scan worker

// #62: budget for the endpoint-recovery COMMANDS issued after a failed
// transfer. These are host-controller commands (Stop/Reset Endpoint, Set TR
// Dequeue), not device transactions: on a working controller they retire in
// microseconds even when the DEVICE is silent, which is exactly the case they
// exist for. A short budget guarantees recovery can never cost more than the
// fault it repairs.
#define XHCI_RECOVER_CMD_MS 100

// Defined near the endpoint-recovery block at the bottom of this file; called
// from xhci_control_transfer_to() far above it.
int xhci_recover_control_endpoint(xhci_controller_t *xhc, int slot_id, int wait_rc);

// =============================================================================
// Global State
// =============================================================================

#define MAX_XHCI_CONTROLLERS 4
static xhci_controller_t xhci_controllers[MAX_XHCI_CONTROLLERS];
static int xhci_controller_count = 0;

// #323: continuous iso streaming flow-control + serial quieting.
volatile uint32_t xhci_iso_xfer_events = 0;
volatile int xhci_iso_quiet = 0;
// #102 idle-CPU: the per-transfer-event serial trace below is DEBUG-ONLY. It
// fired a slow UART kprintf for EVERY USB transfer completion; on a plain
// usb-storage boot (no HID/ECM/audio driver to set xhci_iso_quiet) the steady
// MSC bulk traffic produced ~1000 kprintfs/sec, and each one busy-waits the
// 16550 UART, pegging a full host core purely in the serial write path. Default
// OFF so it never spams in production; set to 1 only when debugging xHCI.
volatile int xhci_xfer_log = 0;   // gated by /CONFIG/USBDEBUG.CFG (see main.c)

// #307: per-endpoint transfer-event completion tracking for the non-blocking
// interrupt-IN model used by USB HID. xhci_process_event records the last
// transfer-event completion code and residual for each (slot, DCI) so a
// polling worker can tell whether an outstanding interrupt-IN TD has completed
// without blocking. Indexed [slot_id-1][dci]. cc==0 means "no completion yet".
// #134: ROOT port each enabled slot hangs off, stored ONE-BASED so that the
// plain zero initialisation of this array already means "unknown", with no
// init hook to forget to call.
static int16_t g_slot_root_port[XHCI_MAX_SLOTS];
static volatile uint8_t  g_xfer_cc[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
static volatile uint32_t g_xfer_residual[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];

// #139 MEASUREMENT. The microsecond (mono_us, truncated to 32 bits so this
// table costs 32 KB rather than 64 KB; unsigned subtraction still yields the
// correct delta across the ~71-minute wrap) at which a drainer OBSERVED the
// transfer event for this (slot, DCI). usb_hid.c differences it against its own
// submit and inject stamps, which is what separates the three delays that add
// up to mouse latency instead of guessing which one dominates:
//   submit  -> observe : the controller's own periodic schedule (the xHCI
//                        endpoint-context Interval field) plus event-ring
//                        drain delay;
//   observe -> inject  : the HID poll worker's wake cadence, i.e. precisely
//                        what #139 event-driven completion removes.
static volatile uint32_t g_xfer_done_us[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];

// #155: OPTIONAL per-transfer-event OBSERVER, for a driver that keeps MORE THAN
// ONE TD in flight on a single endpoint.
//
// g_xfer_cc[][] is ONE cell per (slot, DCI). That is correct and sufficient for
// every existing user (HID, MSC, CDC-ACM, Bluetooth), because each of them keeps
// exactly one TD outstanding and reaps it before submitting the next. It is
// structurally unusable for a QUEUED endpoint: the second completion overwrites
// the first, and with it the residual, so both the byte count and the fact that
// a transfer happened at all are lost. That single-TD-per-endpoint shape is the
// measured ~3 Mbit/s ceiling on the USB-Ethernet dongle (#155).
//
// Growing g_xfer_cc/g_xfer_residual by a queue depth would cost
// XHCI_MAX_SLOTS(256) * XHCI_MAX_ENDPOINTS(32) * depth for every slot on the
// machine to serve ONE singleton device. Instead the one driver that needs
// depth registers an observer and keeps its own completion ring, sized by that
// driver, freed by that driver, and invisible to every other endpoint.
//
// CONTRACT, and it is strict, because this is called from the event drainer:
//   - runs with the event-ring lock held, and MAY run with interrupts off and
//     inside an MSI handler's drain;
//   - MUST NOT block, sleep, log, allocate, or take any lock that another
//     context can hold across a sleep. A ring store and nothing else.
// NULL by default: for every build that has no USB NIC this is one
// branch-not-taken per transfer event.
void (*xhci_xfer_observer)(uint32_t slot, uint32_t dci,
                           uint8_t cc, uint32_t residual) = 0;

// #133: the completion code of the LAST FAILING transfer on each (slot, DCI).
// g_xfer_cc is cleared the instant a poller consumes it, so by the time a
// recovery path runs, the reason the transfer failed is already gone - and the
// reason is what decides the recovery. A Stall means the DEVICE has halted the
// endpoint and only a CLEAR_FEATURE(ENDPOINT_HALT) on the control endpoint
// un-halts it; a USB Transaction Error means the link dropped and a Reset
// Endpoint is the whole cure. Doing the second when the first was needed is a
// recovery that reports success and changes nothing, which is exactly how a
// wireless mouse ends up permanently dead beside a keyboard that is fine.
static volatile uint8_t g_xfer_last_err[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];

// #133: why did the last transfer on this endpoint fail? 0 = no failure seen.
uint8_t xhci_xfer_last_error(int slot_id, int dci) {
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS) return 0;
    if (dci < 0 || dci >= XHCI_MAX_ENDPOINTS) return 0;
    return g_xfer_last_err[slot_id - 1][dci];
}

// #139: read the observe stamp for one endpoint. 0 means "never observed".
uint32_t xhci_xfer_done_us(int slot_id, int dci) {
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS) return 0;
    if (dci < 0 || dci >= XHCI_MAX_ENDPOINTS) return 0;
    return g_xfer_done_us[slot_id - 1][dci];
}

// #307: the event ring is shared by control/bulk (blocking, via
// xhci_wait_for_event) and by the HID / UAC pollers (non-blocking, via
// xhci_poll_events). Since the scheduler is preemptive, a poller running on
// another thread could steal a completion event out from under a blocking
// transfer (observed: MSC "Data transfer failed" when the HID worker drained
// the ring first). This flag serializes event-ring consumption: blocking
// callers spin-acquire it; pollers try-acquire and simply skip if busy.
// #614: event-ring wait queue. See xhci_evt_worker() below for why blocking
// here is safe on hardware that never raises an interrupt.
static wait_queue_head_t g_xhci_evt_wq = { .head = NULL, .lock = SPINLOCK_INIT };

// =============================================================================
// #139: the xHCI interrupt this driver never had
// =============================================================================
// Until now the event ring was drained ONLY by timer-driven workers, so every
// completion waited for the next poll pass. USBCMD.INTE and Interrupter 0's
// IMAN.IE were both already set at init; the missing piece was the PCI-level
// delivery (MSI), so the controller had no way to tell anyone anything and the
// "hardware that never interrupts" in the #614 comment was a property of THIS
// DRIVER, not of the hardware.
//
// WHAT THE HANDLER DELIBERATELY DOES NOT DO: drain. xhci_process_event() calls
// kprintf() on the port-status-change and unknown-event paths, and kprintf
// takes g_console_lock. An ISR that took a lock the interrupted thread on the
// SAME cpu already holds deadlocks that cpu outright - which is exactly the
// four-cpu wedge blame.md records at #134, reached from the other direction.
// So the handler does the two register acknowledgements the spec requires and
// then WAKES, and the drain happens in the woken thread's context where
// kprintf and everything else is legal. The interrupt buys latency; it does
// not move any work into interrupt context.
static volatile uint64_t g_xhci_msi_seq = 0;    // bumped once per interrupt
static volatile uint64_t g_xhci_msi_hits = 0;   // total, for the boot log
static int g_xhci_msi_armed = 0;

// #156: THE MSI ARMING GATE. Default OFF.
//
// #139 armed an xHCI MSI for the first time in this kernel's life. It could
// never run on the test rig: QEMU's qemu-xhci exposes MSI-X (0x11) and not MSI
// (0x05), so pci_enable_msi() declined and xhci_msi_isr() shipped having
// EXECUTED ZERO TIMES on any machine anyone had watched. The one machine that
// can execute it is the owner's iMac14,4 (Intel xHCI at 00:14.0), and build
// 1921 is the first build in which it would have. That build hangs at boot on
// that machine and 1910 did not.
//
// So the arming is opt-in until it has been observed working on real hardware.
// OFF is EXACTLY the pre-#139 state: the event ring is drained by the two
// always-armed timer-driven workers (usb_hid_poll_worker and xhci_evt_worker),
// which is how every shipping build up to and including 1910 ran. #139's real
// win, the Endpoint Context Interval encoder fix that was making every
// full-speed interrupt endpoint 8x slower, is in a completely different
// function (xhci_configure_endpoint_ep) and is NOT affected by this gate.
//
// Resolved once, in xhci_setup_interrupt(), from:
//   compile-time default  XHCI_MSI_DEFAULT_ON (0; `make XHCIMSI=1` makes it 1)
//   /XHCIMSI.TXT on the FAT ESP  -> force ON
//   /XHCIMSI.OFF on the FAT ESP  -> force OFF (wins, so a kernel built ON can
//                                   still be recovered without a rebuild)
// Same shape as /SMPSCHED.TXT and /TORAMOFF.TXT, per the project rule about
// reusing the existing mechanism. READ ONLY: nothing in the kernel writes it,
// so blame.md's "a runtime-WRITABLE ESP marker needs /boot/" (fat_write_file's
// ext2 redirect) does not apply.
#ifndef XHCI_MSI_DEFAULT_ON
#define XHCI_MSI_DEFAULT_ON 0
#endif
static int g_xhci_msi_gate = 0;          // resolved in xhci_setup_interrupt()
int xhci_msi_gate(void) { return g_xhci_msi_gate; }

uint64_t xhci_msi_seq(void)   { return g_xhci_msi_seq; }
uint64_t xhci_msi_count(void) { return g_xhci_msi_hits; }
int      xhci_msi_armed(void) { return g_xhci_msi_armed; }

struct wait_queue_head *xhci_event_waitq(void) { return &g_xhci_evt_wq; }

static volatile int g_evt_busy = 0;
static inline int xhci_evt_trylock(void) { return __sync_lock_test_and_set(&g_evt_busy, 1) == 0; }
static inline void xhci_evt_unlock(void) { __sync_lock_release(&g_evt_busy); }

// #348: command-completion tracking, mirroring g_xfer_cc for transfer events.
// Whoever drains the event ring records the last command completion here so a
// blocking command issuer (xhci_send_command) reads it from this table instead
// of racing the ring / reading the ring directly. cc==0 means "no completion".
static volatile uint8_t g_cmd_cc = 0;
static volatile uint8_t g_cmd_slot = 0;   // slot id from the last completion

// #307 real-HW (b577): decisive on-screen diagnostics for the FIRST command
// (Enable Slot). xhci_send_command records here whether the last command timed
// out (no completion event ever arrived: command/event-ring DMA is not being
// serviced by the controller) versus returned a real completion code, so the
// per-port enumeration failure line can print "TIMEOUT" vs "cc=N". This makes
// the next iMac photo unambiguous about WHERE Enable Slot fails.
static volatile int g_xhci_last_cmd_cc = 0;       // completion code of last cmd
static volatile int g_xhci_last_cmd_timeout = 0;  // 1 = no completion event seen

// #307 real-HW (b576) reset-then-enumerate. On the physical iMac the USB-2 root
// ports come up connected-but-disabled (PED=0, PLS=7) during the initial scan,
// so device enumeration skipped them; the b575 reset pass enables them but did
// not then enumerate them, leaving "enumeration: 0 devices". These tables wire
// enumeration directly to the reset pass: a port enabled by the reset pass is
// enumerated immediately (with its freshly negotiated PORTSC speed), and
// g_port_enumerated[] stops the later xhci_enumerate_devices pass from
// enumerating the same port a second time. Keyed by controller index (matches
// the xhci_controllers[] array). g_enum_dev_found[] is the cumulative count of
// devices enumerated on a controller across BOTH passes, so the on-screen
// "enumeration: N device(s)" summary is accurate regardless of which pass ran.
static uint8_t g_port_enumerated[MAX_XHCI_CONTROLLERS][256];
static int     g_enum_dev_found[MAX_XHCI_CONTROLLERS];

// #135 (root port 4 flaps on the owner's iMac14,4, re-enumerating a keyboard
// that was never unplugged). State for the PORTSC transition log, the connect
// debounce, the flap governor and same-device recognition. Indexed exactly like
// g_port_enumerated[] above (controller index, then root-port number - 1).
static uint32_t g_portsc_prev[MAX_XHCI_CONTROLLERS][256];
static uint8_t  g_portsc_prev_valid[MAX_XHCI_CONTROLLERS][256];
static uint16_t g_port_flaps[MAX_XHCI_CONTROLLERS][256];      // confirmed disconnects
static uint16_t g_port_cooldown[MAX_XHCI_CONTROLLERS][256];   // re-scans to skip
static uint16_t g_port_stable[MAX_XHCI_CONTROLLERS][256];     // consecutive good scans
static uint16_t g_port_orphan[MAX_XHCI_CONTROLLERS][256];     // #134: consecutive scans
                                                              // flagged enumerated with no slot
static uint32_t g_port_last_devid[MAX_XHCI_CONTROLLERS][256]; // (vid<<16)|pid
static uint32_t g_legsup_off[MAX_XHCI_CONTROLLERS];           // MMIO byte offset, 0=none
static uint32_t g_portsc_log_lines = 0;

static inline int xhci_ctrl_index(xhci_controller_t *xhc) {
    int idx = (int)(xhc - xhci_controllers);
    if (idx < 0 || idx >= MAX_XHCI_CONTROLLERS) idx = 0;
    return idx;
}

// Enumerate a single already-reset+enabled root-hub port (defined below).
static int xhci_enumerate_port(xhci_controller_t *xhc, uint32_t port, int speed);
// #433: bounded-retry wrapper around xhci_enumerate_port; marks the port
// enumerated ONLY on success and leaves it eligible for re-scan on failure.
static int xhci_try_enumerate_port(xhci_controller_t *xhc, uint32_t port,
                                   int speed, int idx);

// #307/#375/#507: xhci_delay() is a REAL-TIME busy delay. It must be, because
// USB bring-up runs with interrupts still OFF (see main.c) and with no
// scheduler, and because USB enumeration is built out of spec-mandated MINIMUM
// waits: port-reset recovery (TDRSTR), the 2ms Set-Address recovery of USB 2.0
// 9.2.6.3, hub power-good, and the extra recovery gaps #307 added for slow real
// devices. A delay that is shorter than it claims does not fail loudly; it
// makes enumeration INTERMITTENT, which is exactly the shape of #433 (xHCI
// enumeration race), #373 (hub enumeration) and #366 (USB2/EHCI hand-off).
//
// HISTORY, because both wrong answers are instructive:
//   1. Originally a fixed-iteration PAUSE spin ("ms * 10000"). That is
//      calibrated to whatever per-instruction cost happens to apply wherever it
//      runs, and QEMU's TCG executes PAUSE far slower than real silicon, so the
//      loop that burned ~1ms under emulation burned a small FRACTION of a
//      millisecond on the real iMac. Fixed by #307 by measuring the PIT.
//   2. That PIT measurement was a PRIVATE copy of the latch-and-read loop, and
//      it treated one observed counter unit as one PIT input clock. cpu/pic.c
//      programs channel 0 with command byte 0x36, whose mode bits are 011 =
//      MODE 3 (square wave), in which the counter is decremented by TWO per
//      input clock. So every delay here ran for HALF its nominal duration
//      (#507). The factor of two was already correct forty lines into
//      rustkern/mono.rs, where the same constant is needed to calibrate the
//      monotonic clock: the defect was the DUPLICATION, not the arithmetic.
//
// So there is no private clock here any more. Timing comes from the one module
// that owns the PIT and the TSC (cpu/mono.h, #525), which is calibrated in
// main() immediately after pit_init() and therefore ready long before
// usb_init() enumerates the controller. See mono_busy_delay_us().
extern uint32_t g_timer_hz;

static void xhci_delay(uint32_t ms) {
    if (ms == 0) return;
    mono_busy_delay_us((uint64_t)ms * 1000ull);
}

// #362: exported wrapper so other USB drivers (usb_ecm.c / usb_asix.c) can
// use the same PIT-calibrated wall-clock delay for their transfer timeouts
// and vendor reset sequences (a PAUSE-burst loop is NOT wall-clock accurate;
// that is exactly the bug the calibrated xhci_delay() fixed for #307).
void xhci_delay_ms(uint32_t ms) { xhci_delay(ms); }

// =============================================================================
// Register Access
// =============================================================================

static inline uint32_t xhci_read32(xhci_controller_t *xhc __attribute__((unused)), volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static inline void xhci_write32(xhci_controller_t *xhc __attribute__((unused)), volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static inline uint64_t xhci_read64(xhci_controller_t *xhc __attribute__((unused)), volatile uint8_t *base, uint32_t offset) {
    uint64_t lo = *(volatile uint32_t *)(base + offset);
    uint64_t hi = *(volatile uint32_t *)(base + offset + 4);
    return lo | (hi << 32);
}

static inline void xhci_write64(xhci_controller_t *xhc __attribute__((unused)), volatile uint8_t *base, uint32_t offset, uint64_t value) {
    *(volatile uint32_t *)(base + offset) = (uint32_t)value;
    *(volatile uint32_t *)(base + offset + 4) = (uint32_t)(value >> 32);
}

// Capability register access
static inline uint32_t xhci_cap_read32(xhci_controller_t *xhc, uint32_t offset) {
    return xhci_read32(xhc, xhc->mmio_base, offset);
}

// Operational register access
static inline uint32_t xhci_op_read32(xhci_controller_t *xhc, uint32_t offset) {
    return xhci_read32(xhc, xhc->op_regs, offset);
}

static inline void xhci_op_write32(xhci_controller_t *xhc, uint32_t offset, uint32_t value) {
    xhci_write32(xhc, xhc->op_regs, offset, value);
}

static inline uint64_t xhci_op_read64(xhci_controller_t *xhc, uint32_t offset) {
    return xhci_read64(xhc, xhc->op_regs, offset);
}

static inline void xhci_op_write64(xhci_controller_t *xhc, uint32_t offset, uint64_t value) {
    xhci_write64(xhc, xhc->op_regs, offset, value);
}

// Runtime register access
static inline uint32_t xhci_rt_read32(xhci_controller_t *xhc, uint32_t offset) {
    return xhci_read32(xhc, xhc->rt_regs, offset);
}

static inline void xhci_rt_write32(xhci_controller_t *xhc, uint32_t offset, uint32_t value) {
    xhci_write32(xhc, xhc->rt_regs, offset, value);
}

static inline uint64_t xhci_rt_read64(xhci_controller_t *xhc, uint32_t offset) {
    return xhci_read64(xhc, xhc->rt_regs, offset);
}

static inline void xhci_rt_write64(xhci_controller_t *xhc, uint32_t offset, uint64_t value) {
    xhci_write64(xhc, xhc->rt_regs, offset, value);
}

// Port register access
static inline uint32_t xhci_portsc_read(xhci_controller_t *xhc, int port) {
    return xhci_op_read32(xhc, XHCI_PORTSC_OFFSET + (port * 0x10));
}

static inline void xhci_portsc_write(xhci_controller_t *xhc, int port, uint32_t value) {
    xhci_op_write32(xhc, XHCI_PORTSC_OFFSET + (port * 0x10), value);
}

// Doorbell access
static inline void xhci_doorbell_write(xhci_controller_t *xhc, uint32_t slot, uint32_t value) {
    xhc->doorbells[slot] = value;
}

// =============================================================================
// #135: root-port flap diagnosis (PORTSC decode, controller context, debounce)
// =============================================================================
//
// THE FAULT. On the owner's real iMac14,4 the 3-second re-scan alternates
// "port 4 disconnected" and "port 4 connected but not enumerated" forever,
// re-enumerating the still-physically-plugged Apple keyboard 05ac:024f into a
// fresh device slot and fresh hid_devices[] entries every cycle. The re-scan
// decided this on ONE bit: xhci_port_is_connected() is a bare PORTSC & CCS read.
// Nothing has ever dumped the rest of the register across the transition, so
// WHY CCS reads 0 on an attached device is, at the time of writing, unknown.
//
// These helpers exist to answer that from the owner's next boot, and they write
// to bootlog_write(), NEVER kprintf(): the iMac runs in GUI mode with no serial
// console, so a kprintf diagnostic on that machine does not exist. (That trap is
// why the USBLEGSUP hand-off below reported its result to nobody for months.)
//
// THE FIELD THAT DECIDES IT is CSC. The change bits are RW1C and the re-scan
// never clears them, so:
//   CCS=0 with CSC=1  -> the controller LATCHED a connect-status change; the
//                        device really did go electrically absent (cable, VBUS,
//                        or an agent that reset/depowered the port).
//   CCS=0 with CSC=0  -> the bit moved with no latched transition. That is not a
//                        cable event. Suspect the port being routed away from
//                        the xHCI, or firmware operating it behind us.
// PLS, PP, OCA and PR separate the sub-cases: PP=0 means something depowered the
// port, OCA=1 means over-current shut it down, PLS tells you whether the link is
// in Polling / RxDetect / Inactive / Compliance / Resume rather than U0.
static const char *xhci_pls_name(uint32_t pls) {
    switch (pls) {
        case 0:  return "U0";        case 1:  return "U1";
        case 2:  return "U2";        case 3:  return "U3-susp";
        case 4:  return "Disabled";  case 5:  return "RxDetect";
        case 6:  return "Inactive";  case 7:  return "Polling";
        case 8:  return "Recovery";  case 9:  return "HotReset";
        case 10: return "Compliance"; case 11: return "TestMode";
        case 15: return "Resume";    default: return "rsvd";
    }
}

// The persistent log is a 96 KB RAM buffer and everything past it is serial-only
// (i.e. invisible on this machine), so the transition log is HARD CAPPED. It
// only ever fires on a CHANGE, so a healthy machine spends one line per port.
#define XHCI_PORTSC_LOG_MAX 200

static void xhci_portsc_log(xhci_controller_t *xhc, uint32_t port, uint32_t v,
                            const char *why) {
    (void)xhc;
    if (g_portsc_log_lines > XHCI_PORTSC_LOG_MAX) return;
    g_portsc_log_lines++;
    if (g_portsc_log_lines > XHCI_PORTSC_LOG_MAX) {
        bootlog_write("[PORTSC] transition-log cap %d reached; further PORTSC "
                      "changes are serial-only", XHCI_PORTSC_LOG_MAX);
        return;
    }
    bootlog_write("[PORTSC] p%u %s raw=%08x CCS=%u PED=%u OCA=%u PR=%u "
                  "PLS=%u(%s) PP=%u spd=%u | CSC=%u PEC=%u WRC=%u OCC=%u "
                  "PRC=%u PLC=%u CEC=%u",
                  port + 1, why, v,
                  (v & XHCI_PORTSC_CCS) ? 1 : 0,
                  (v & XHCI_PORTSC_PED) ? 1 : 0,
                  (v & XHCI_PORTSC_OCA) ? 1 : 0,
                  (v & XHCI_PORTSC_PR)  ? 1 : 0,
                  (v >> 5) & 0xf, xhci_pls_name((v >> 5) & 0xf),
                  (v & XHCI_PORTSC_PP)  ? 1 : 0,
                  (v >> 10) & 0xf,
                  (v & XHCI_PORTSC_CSC) ? 1 : 0,
                  (v & XHCI_PORTSC_PEC) ? 1 : 0,
                  (v & XHCI_PORTSC_WRC) ? 1 : 0,
                  (v & XHCI_PORTSC_OCC) ? 1 : 0,
                  (v & XHCI_PORTSC_PRC) ? 1 : 0,
                  (v & XHCI_PORTSC_PLC) ? 1 : 0,
                  (v & XHCI_PORTSC_CEC) ? 1 : 0);
}

// USBLEGSUP / USBLEGCTLSTS bit definitions (xHCI 1.1 sections 7.1.1 / 7.1.2).
#define XHCI_LEGSUP_BIOS_OWNED  (1u << 16)
#define XHCI_LEGSUP_OS_OWNED    (1u << 24)
// SMI ENABLES live at bits 0 (USB SMI), 4 (Host System Error), 13 (OS
// Ownership), 14 (PCI Command), 15 (BAR). Bits 29-31 are RW1C SMI status.
#define XHCI_LEGCTL_SMI_ENABLES ((1u<<0)|(1u<<4)|(1u<<13)|(1u<<14)|(1u<<15))
#define XHCI_LEGCTL_SMI_STATUS  ((1u<<29)|(1u<<30)|(1u<<31))

// Everything OUTSIDE PORTSC that can make a still-plugged device read CCS=0.
// Two named suspects, both one register read and neither ever logged before:
//   - the Intel USB2/USB3 port-routing registers (#366). If firmware hands a
//     switchable USB2 port back to the EHCI companion, that port's xHCI PORTSC
//     reads CCS=0 with nothing having been unplugged. Root port 4 is the LAST
//     switchable USB2 port on this chipset (XUSB2PR read back 0x0000000f), which
//     makes this hypothesis specific and testable rather than decorative.
//   - USBLEGSUP. If HC BIOS Owned has gone high again, firmware has re-taken the
//     controller and is operating the ports behind us, which is exactly what
//     legacy USB keyboard emulation does, and it is the KEYBOARD port flapping.
static void xhci_portctx_log(xhci_controller_t *xhc, const char *why) {
    int idx = xhci_ctrl_index(xhc);
    uint32_t legsup = 0, legctl = 0;
    if (g_legsup_off[idx] && xhc->mmio_base) {
        volatile uint32_t *cap =
            (volatile uint32_t *)(xhc->mmio_base + g_legsup_off[idx]);
        legsup = cap[0];
        legctl = cap[1];
    }
    uint32_t pssen = 0, xusb2pr = 0;
    if (xhc->pci && xhc->pci->vendor_id == 0x8086) {
        pssen   = pci_read32(xhc->pci->bus, xhc->pci->slot, xhc->pci->func, 0xD0);
        xusb2pr = pci_read32(xhc->pci->bus, xhc->pci->slot, xhc->pci->func, 0xD8);
    }
    bootlog_write("[PORTCTX] %s USBSTS=%08x LEGSUP=%08x(BIOS_OWNED=%u) "
                  "LEGCTLSTS=%08x XUSB2PR=%08x PSSEN=%08x", why,
                  xhci_op_read32(xhc, XHCI_OP_USBSTS), legsup,
                  (legsup & XHCI_LEGSUP_BIOS_OWNED) ? 1 : 0, legctl,
                  xusb2pr, pssen);
}

// DEBOUNCE. USB 2.0 section 7.1.7.3 requires software to see a connect or
// disconnect stable for at least 100 ms before acting on it. The re-scan took
// exactly ONE CCS sample every 3 seconds and tore the port down on it, so a
// transient of any duration - including one caused by another agent touching the
// port between our samples - was indistinguishable from an unplug. Sample across
// >100 ms and believe a disconnect only when EVERY sample agrees. A disagreement
// is logged, because a "glitch" line is itself the evidence the single-sample
// code destroyed. Returns 1 when the disconnect is confirmed, 0 when it is not.
//
// Cost: this runs ONLY in the re-scan worker thread, and only on the scan where
// a port we believe is enumerated first reads CCS=0, so the steady state is
// unchanged. xhci_delay() is this file's calibrated real-time delay (see its
// definition above); it is a delay, not a condition poll.
#define XHCI_DEBOUNCE_SAMPLES 6
#define XHCI_DEBOUNCE_MS      20
static int xhci_port_disconnect_confirmed(xhci_controller_t *xhc, uint32_t port) {
    int ones = 0;
    uint32_t last = 0;
    for (int i = 0; i < XHCI_DEBOUNCE_SAMPLES; i++) {
        xhci_delay(XHCI_DEBOUNCE_MS);
        last = xhci_portsc_read(xhc, port);
        if (last & XHCI_PORTSC_CCS) ones++;
    }
    if (ones) {
        xhci_portsc_log(xhc, port, last, "DEBOUNCE-GLITCH");
        bootlog_write("[PORTSC] p%u: CCS=0 was a GLITCH - %d of %d debounce samples "
                      "over %dms read CCS=1; port NOT torn down",
                      port + 1, ones, XHCI_DEBOUNCE_SAMPLES,
                      XHCI_DEBOUNCE_SAMPLES * XHCI_DEBOUNCE_MS);
        return 0;
    }
    return 1;
}

// =============================================================================
// #135: xHCI USBLEGSUP BIOS->OS ownership hand-off
// =============================================================================
//
// THIS WAS BROKEN FOR AS LONG AS THE FILE HAS EXISTED IN GIT, and it failed
// silently. The hand-off block used to sit near the top of xhci_init(), THIRTY-SIX
// LINES BEFORE `xhc->mmio_base` is assigned from BAR0. xhci_cap_read32() and the
// extended-capability walk therefore dereferenced (NULL + offset): the code read
// HCCPARAMS1 from physical address 0x10, walked a fabricated capability list
// through low RAM (identity-mapped, so no fault), and - had a stray byte there
// read as capability id 1 - would have WRITTEN the OS-owned bit and the SMI mask
// into low memory. It never addressed the controller once.
//
// So on real hardware the controller may still be BIOS-OWNED, and firmware SMM
// legacy-USB code keeps operating the root ports behind us. That is a first-class
// suspect for #135: only the KEYBOARD port flaps, on a machine whose firmware
// emulates a legacy keyboard from exactly that port.
//
// Nothing pointed at any of this because the outcome was reported with kprintf()
// ONLY, and the iMac has no serial console.
//
// The SMI mask was wrong too. USBLEGCTLSTS bit 0 is USB SMI Enable, and the old
// `*ctlsts = *ctlsts & 0x7` KEPT it, so even a successful hand-off would have
// left the controller raising an SMI on every USB event. Clear every SMI enable
// and write 1 to the RW1C status bits to ack anything already pending, as Linux
// does in quirks.c:usb_disable_xhci_ports/xhci_bios_handoff.
static void xhci_bios_handoff(xhci_controller_t *xhc) {
    int idx = xhci_ctrl_index(xhc);
    g_legsup_off[idx] = 0;
    uint32_t hcc = xhci_cap_read32(xhc, XHCI_CAP_HCCPARAMS1);
    uint32_t xecp = (hcc >> 16) & 0xffff;   // offset in 32-bit dwords
    if (!xecp) {
        bootlog_write("[xHCI] BIOS hand-off: controller has NO extended "
                      "capabilities (HCCPARAMS1=0x%08x)", hcc);
        return;
    }
    uint32_t guard = 0;
    while (xecp && guard++ < 64) {
        volatile uint32_t *cap =
            (volatile uint32_t *)(xhc->mmio_base + (xecp * 4));
        uint32_t capval = *cap;
        uint8_t capid = capval & 0xff;
        if (capid == 1) {                       // USB Legacy Support Capability
            g_legsup_off[idx] = xecp * 4;
            uint32_t before = capval;
            int waited = 0;
            if (capval & XHCI_LEGSUP_BIOS_OWNED) {
                *cap = capval | XHCI_LEGSUP_OS_OWNED;   // request OS ownership
                for (int i = 0; i < 1000; i++) {        // spec allows up to 1s
                    if (!((*cap) & XHCI_LEGSUP_BIOS_OWNED)) break;
                    xhci_delay(1);
                    waited++;
                }
            }
            uint32_t after = *cap;
            volatile uint32_t *ctlsts = cap + 1;
            uint32_t ctl_before = *ctlsts;
            *ctlsts = (ctl_before & ~XHCI_LEGCTL_SMI_ENABLES) | XHCI_LEGCTL_SMI_STATUS;
            uint32_t ctl_after = *ctlsts;
            bootlog_write("[xHCI] BIOS->OS hand-off @+0x%x: LEGSUP %08x -> %08x "
                          "(BIOS_OWNED %u -> %u after %dms), LEGCTLSTS %08x -> %08x%s",
                          g_legsup_off[idx], before, after,
                          (before & XHCI_LEGSUP_BIOS_OWNED) ? 1 : 0,
                          (after  & XHCI_LEGSUP_BIOS_OWNED) ? 1 : 0, waited,
                          ctl_before, ctl_after,
                          (after & XHCI_LEGSUP_BIOS_OWNED)
                              ? "  *** FIRMWARE STILL OWNS THIS CONTROLLER ***" : "");
            return;
        }
        uint32_t next = (capval >> 8) & 0xff;   // next pointer, in dwords
        if (!next) break;
        xecp += next;
    }
    bootlog_write("[xHCI] BIOS hand-off: no USB Legacy Support capability in %u "
                  "extended-capability entries (xECP=0x%x)", guard, (hcc >> 16) & 0xffff);
}

// =============================================================================
// Speed Names
// =============================================================================

const char *xhci_speed_name(int speed) {
    switch (speed) {
        case XHCI_SPEED_FULL:       return "Full-Speed (12 Mbps)";
        case XHCI_SPEED_LOW:        return "Low-Speed (1.5 Mbps)";
        case XHCI_SPEED_HIGH:       return "High-Speed (480 Mbps)";
        case XHCI_SPEED_SUPER:      return "Super-Speed (5 Gbps)";
        case XHCI_SPEED_SUPER_PLUS: return "Super-Speed+ (10 Gbps)";
        default:                    return "Unknown";
    }
}

const char *xhci_completion_code_name(int code) {
    switch (code) {
        case CC_SUCCESS:            return "Success";
        case CC_DATA_BUFFER_ERROR:  return "Data Buffer Error";
        case CC_BABBLE_DETECTED:    return "Babble Detected";
        case CC_USB_TRANSACTION_ERROR: return "USB Transaction Error";
        case CC_TRB_ERROR:          return "TRB Error";
        case CC_STALL_ERROR:        return "Stall Error";
        case CC_RESOURCE_ERROR:     return "Resource Error";
        case CC_NO_SLOTS_AVAILABLE: return "No Slots Available";
        case CC_SLOT_NOT_ENABLED:   return "Slot Not Enabled";
        case CC_EP_NOT_ENABLED:     return "Endpoint Not Enabled";
        case CC_SHORT_PACKET:       return "Short Packet";
        case CC_RING_UNDERRUN:      return "Ring Underrun";
        case CC_RING_OVERRUN:       return "Ring Overrun";
        case CC_BANDWIDTH_ERROR:    return "Bandwidth Error";
        case CC_MISSED_SERVICE:     return "Missed Service";
        case CC_ISOCH_BUFFER_OVERRUN: return "Isoch Buffer Overrun";
        case CC_PARAMETER_ERROR:    return "Parameter Error";
        case CC_CONTEXT_STATE_ERROR: return "Context State Error";
        case CC_COMMAND_RING_STOPPED: return "Command Ring Stopped";
        case CC_COMMAND_ABORTED:    return "Command Aborted";
        case CC_STOPPED:            return "Stopped";
        default:                    return "Unknown";
    }
}

// =============================================================================
// Ring Buffer Operations
// =============================================================================

int xhci_ring_init(xhci_ring_t *ring, uint32_t size) {
    // Allocate physically contiguous memory for TRBs (16-byte aligned)
    size_t alloc_size = size * sizeof(xhci_trb_t);
    alloc_size = ALIGN_UP(alloc_size, PAGE_SIZE);

    // Allocate physical page(s)
    uint64_t phys = pmm_alloc_pages(alloc_size / PAGE_SIZE);
    if (phys == 0) {
        kprintf("[xHCI] Failed to allocate ring buffer\n");
        return -1;
    }

    // Map to kernel virtual address (identity map in early kernel)
    ring->trbs = (xhci_trb_t *)phys;
    ring->phys_addr = phys;
    ring->size = size;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_bit = 1;

    // Clear all TRBs
    memset(ring->trbs, 0, size * sizeof(xhci_trb_t));

    // Add link TRB at the end pointing back to start
    xhci_trb_t *link = &ring->trbs[size - 1];
    link->parameter = ring->phys_addr;
    link->status = 0;
    link->control = XHCI_TRB_TYPE(TRB_LINK) | TRB_CYCLE | (1 << 1); // Toggle cycle

    return 0;
}

void xhci_ring_free(xhci_ring_t *ring) {
    if (ring->trbs) {
        size_t alloc_size = ALIGN_UP(ring->size * sizeof(xhci_trb_t), PAGE_SIZE);
        pmm_free_pages(ring->phys_addr, alloc_size / PAGE_SIZE);
        ring->trbs = NULL;
        ring->phys_addr = 0;
    }
}

xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring) {
    // #307: Handle the wrap BEFORE handing out the TRB, not after.
    //
    // The last slot [size-1] is the LINK TRB. When the enqueue pointer reaches
    // it, arm the link with the CURRENT producer cycle bit, wrap to index 0, and
    // ONLY THEN toggle the cycle bit. The old code toggled the cycle bit right
    // after returning the boundary TRB, so the caller (which writes
    // trb->control |= ring->cycle_bit afterwards) stamped the already-toggled
    // (wrong) cycle bit onto the TRB at [size-2]. The controller then saw a
    // cycle mismatch at that TRB and silently stopped executing the ring, so
    // every transfer after the first wrap produced no completion event
    // (observed as "Event wait timeout" at ~command 127 on the 2-TRB-per-command
    // MSC IN ring). Checking at entry keeps the returned TRB's cycle bit correct.
    //
    // #375 robustness: under the TO-RAM full-image copy the transfer ring wraps
    // thousands of times back to back. A single garbage/out-of-range field here
    // would hand back a wild pointer that the caller then #GPs on (observed as a
    // non-canonical RAX in xhci_bulk_transfer). Harden every field so this ALWAYS
    // returns a valid in-ring TRB or NULL:
    //  - rings are identity-mapped, so trbs MUST equal phys_addr; if an adjacent
    //    overflow clobbered the cached pointer, restore it from phys_addr.
    //  - clamp a corrupted/out-of-range enqueue_idx back into the ring.
    if (!ring) return 0;
    if (ring->size == 0 || ring->size > (XHCI_RING_SIZE * 16u)) return 0;
    // Rings live in identity-mapped RAM below the 2 GB PMM cap, so trbs is always
    // exactly phys_addr. Trust phys_addr (validated in range) and rebuild trbs
    // from it every call: this self-heals a trbs pointer clobbered by an adjacent
    // overflow, and bails (NULL) if phys_addr itself is corrupt.
    if (ring->phys_addr < 0x1000ULL || ring->phys_addr >= 0x80000000ULL) return 0;
    ring->trbs = (xhci_trb_t *)ring->phys_addr;
    if (ring->enqueue_idx >= ring->size) ring->enqueue_idx = 0;

    if (ring->enqueue_idx == ring->size - 1) {
        xhci_trb_t *link = &ring->trbs[ring->size - 1];
        link->control = (link->control & ~TRB_CYCLE) | ring->cycle_bit;
        ring->enqueue_idx = 0;
        ring->cycle_bit ^= 1;  // Toggle cycle for the new pass
    }

    xhci_trb_t *trb = &ring->trbs[ring->enqueue_idx];
    ring->enqueue_idx++;
    return trb;
}

void xhci_ring_doorbell(xhci_controller_t *xhc, uint32_t slot, uint32_t target) {
    xhci_doorbell_write(xhc, slot, target);
}

// =============================================================================
// Event Ring Operations
// =============================================================================

static int xhci_event_ring_init(xhci_controller_t *xhc) {
    // Initialize event ring
    if (xhci_ring_init(&xhc->event_ring, XHCI_RING_SIZE) < 0) {
        return -1;
    }

    // Clear link TRB for event ring (events don't use link TRBs)
    memset(&xhc->event_ring.trbs[XHCI_RING_SIZE - 1], 0, sizeof(xhci_trb_t));

    // Allocate Event Ring Segment Table
    xhc->erst = (xhci_erst_entry_t *)pmm_alloc_pages(1);
    if (!xhc->erst) {
        xhci_ring_free(&xhc->event_ring);
        return -1;
    }
    xhc->erst_phys = (uint64_t)xhc->erst;
    memset(xhc->erst, 0, PAGE_SIZE);

    // Setup ERST entry
    xhc->erst[0].ring_base = xhc->event_ring.phys_addr;
    xhc->erst[0].ring_size = XHCI_RING_SIZE;
    xhc->erst[0].reserved = 0;

    // Program interrupter 0
    uint32_t ir_offset = XHCI_RT_IR0;

    // Set ERST size
    xhci_rt_write32(xhc, ir_offset + XHCI_IR_ERSTSZ, 1);

    // Set ERST base address
    xhci_rt_write64(xhc, ir_offset + XHCI_IR_ERSTBA, xhc->erst_phys);

    // Set event ring dequeue pointer
    xhci_rt_write64(xhc, ir_offset + XHCI_IR_ERDP, 
                    xhc->event_ring.phys_addr | (1 << 3)); // EHB bit

    // Enable interrupts for this interrupter
    xhci_rt_write32(xhc, ir_offset + XHCI_IR_IMAN, XHCI_IMAN_IE);

    return 0;
}

// =============================================================================
// Command Ring Operations
// =============================================================================

static int xhci_command_ring_init(xhci_controller_t *xhc) {
    if (xhci_ring_init(&xhc->cmd_ring, XHCI_RING_SIZE) < 0) {
        return -1;
    }

    // Program CRCR
    uint64_t crcr = xhc->cmd_ring.phys_addr | XHCI_CRCR_RCS;
    xhci_op_write64(xhc, XHCI_OP_CRCR, crcr);

    return 0;
}

// ===========================================================================
// #134: THE COMMAND RING IS A SINGLE SHARED RESOURCE AND IT HAD NO LOCK.
// ===========================================================================
// xhci_send_command_to() does four things that are only correct if exactly one
// thread is inside at a time, and nothing enforced that:
//
//   1. xhci_ring_enqueue(&xhc->cmd_ring) is a read-modify-write of
//      enqueue_idx plus, on the wrap, of cycle_bit. Two threads can be handed
//      the SAME TRB and each write a different command into it, or lose the
//      cycle toggle at the link TRB - the precise failure #307 already
//      documents at that function ("the controller saw a cycle mismatch and
//      silently stopped executing the ring").
//   2. g_cmd_cc is ONE byte for the WHOLE controller. Clearing it before
//      ringing the doorbell erases a completion another thread was waiting for.
//   3. g_cmd_slot is ONE byte too, and xhci_enable_slot() reads the slot id out
//      of it. A stolen completion therefore does not merely fail: it hands back
//      SOMEBODY ELSE'S SLOT NUMBER, and the caller then programs DCBAA and a
//      device context for a slot that is live - on this hardware, possibly the
//      USB mass-storage device the root filesystem is mounted from.
//   4. g_xhci_last_cmd_cc / _timeout are shared diagnostics.
//
// WHY THIS ONLY BECAME REACHABLE RECENTLY, and why the ticket is an unplug.
// Until #62/#133/#134 (aa68a1a, 40f51ce - both AFTER build 1902) the only
// commands issued once the scheduler was live came from the port re-scan
// worker, one thread, serialised by being one thread. Those commits added TWO
// more issuers on other threads, and both of them fire on a DISCONNECT:
//   - usb_hid_poll() -> xhci_recover_endpoint() -> Reset Endpoint + Set TR
//     Dequeue, from the HID poll worker, when the in-flight interrupt-IN TD of
//     the device being unplugged completes with a transaction error; and
//   - xhci_recover_control_endpoint() -> Stop/Reset Endpoint + Set TR Dequeue,
//     from whatever thread was doing a control transfer (on the owner's iMac
//     the ASIX USB-Ethernet link poller, which #62 measured burning 5 s
//     budgets on slot 1 DCI 1 thirteen times in one boot).
// while the re-scan worker is in xhci_teardown_port_slots() -> Disable Slot and
// then re-enumerating. Build 1902 lost input on an unplug and kept running;
// 1907+ freezes. This is the concurrency the difference introduced.
//
// THE LOCK IS THE MSC COMMAND LOCK'S SHAPE, DELIBERATELY. usb_msc.c already
// solved exactly this problem for the SCSI command lock (#617/#745): an
// uncontended atomic acquire, a wait-queue block with the re-check AFTER
// queueing (the lost-wake close), and a no-block fallback for the pre-scheduler
// phase where the lock is uncontended by construction. Copying that shape
// rather than inventing a third one is the project rule, and it means this
// lock behaves under IF=0 and pre-proc_init exactly like the one that has
// already been proven on the boot path.
//
// CONTENTION IS COUNTED, NOT ASSERTED. g_xhci_cmd_contended is the measurement
// that says whether two threads really do issue commands at once on a given
// machine; the first one also names the waiting caller's return address for
// addr2line. On a machine where it stays zero, this lock costs one atomic per
// command.
#define XHCI_CMD_BLOCK_MS 10
static wait_queue_head_t g_xhci_cmd_wq = { .head = NULL, .lock = SPINLOCK_INIT };
static volatile int      g_xhci_cmd_busy = 0;
uint64_t g_xhci_cmd_contended     = 0;   // times a caller had to wait
uint64_t g_xhci_cmd_noblock_refused = 0; // contention from a context that cannot park
uint64_t g_xhci_cmd_contend_ra    = 0;   // first waiter's return address

// A CONTENDED ACQUIRE FROM A CONTEXT THAT CANNOT PARK FAILS, IT DOES NOT SPIN.
// usb_msc.c's equivalent fallback is a `while (test_and_set)` justified by
// "unreachable by construction, because the pre-scheduler USB stack is
// single-threaded". That justification is sound for the uncontended case and
// worthless for this one: the whole point here is that OTHER threads issue
// commands, and #745 already documents what an unbounded wait entered with
// IF=0 costs (the machine stops, because the scheduler cannot run to let the
// holder finish). Returning -1 is honest and safe: every caller already treats
// a negative return as "the command failed", the recovery paths simply retry on
// their next pass, and the boot path never reaches it because it is
// single-threaded and therefore always takes the uncontended acquire above.
// Counted, so "this never happens" stays a measurement rather than a claim.
static int xhci_cmd_lock(void) {
    // #134 (2026-08-18): THE COMMAND CRITICAL SECTION MUST NOT ENTER THE
    // STORAGE STACK. This lock is NOT recursive and its contended path is an
    // unbounded for(;;), so anything that re-enters it from INSIDE the critical
    // section wedges the calling thread for ever. There is exactly such a path
    // today: xhci_wait_for_event() calls bootlog_write() on its TIMEOUT branch
    // while this lock is still held; bootlog_write() flushes /BOOTLOG.TXT; and
    // on a USB-rooted machine that flush goes fat/ext2 -> blk_write ->
    // usb_msc_transport, whose error recovery issues Reset Endpoint / Set TR
    // Dequeue COMMANDS. That is a self-deadlock in one thread, and an ABBA
    // against the MSC command lock in the other direction (a thread holding
    // msc_cmd_lock that needs a controller command, while we hold the command
    // lock and need msc_cmd_lock). Both loops are unbounded, so both are
    // permanent, and the thread that dies quietly is the port re-scan worker:
    // exactly the "no further re-scan output, machine otherwise usable" shape.
    //
    // Fix the mechanism, not the one call site: open the logger's OWN defer
    // window (fs/bootlog.c g_bl_defer, the same primitive usb_msc_transport()
    // already uses for this identical hazard) for the whole hold. Lines issued
    // inside still reach the RAM buffer and serial and are persisted by the
    // next safe context, so no diagnostic is lost; no present or future caller
    // can reach the medium from in here.
    if (__sync_lock_test_and_set(&g_xhci_cmd_busy, 1) == 0) { bootlog_defer_begin(); return 0; }

    if (g_xhci_cmd_contended++ == 0) {
        g_xhci_cmd_contend_ra = (uint64_t)__builtin_return_address(0);
        bootlog_write("[xHCI] #134: TWO THREADS issued controller commands at once "
                      "(waiter ra=%p). The command ring, g_cmd_cc and g_cmd_slot "
                      "are single-instance and were unserialised until now; this "
                      "is the window that could hand a caller another slot's "
                      "completion. Serialised from here.",
                      (void *)g_xhci_cmd_contend_ra);
    }

    for (;;) {
        if (!wq_may_block()) {
            g_xhci_cmd_noblock_refused++;
            return -1;
        }
        wait_queue_entry_t wqe;
        __wait_prepare(&g_xhci_cmd_wq, &wqe, 0);
        // Re-check AFTER queueing: the lost-wake close (usb_msc.c's shape).
        if (g_xhci_cmd_busy)
            (void)__wait_event_wait_deadline(&wqe, sched_now_ms() + XHCI_CMD_BLOCK_MS);
        __wait_finish(&g_xhci_cmd_wq, &wqe);
        if (__sync_lock_test_and_set(&g_xhci_cmd_busy, 1) == 0) { bootlog_defer_begin(); return 0; }
    }
}

static void xhci_cmd_unlock(void) {
    bootlog_defer_end();          // #134: see xhci_cmd_lock()
    __sync_lock_release(&g_xhci_cmd_busy);
    // Unconditional, like the event-drain wake above: wake_up_all() is safe from
    // IRQ context and a no-op on an empty queue.
    wake_up_all(&g_xhci_cmd_wq);
}

// Send a command and wait for completion, with an EXPLICIT budget.
//
// #62: the recovery path added below issues commands from inside an ALREADY
// FAILED transfer. Giving those commands the same 5s budget as enumeration
// would let a single quiet device turn one 5s stall into three, i.e. the
// recovery would be worse than the fault it repairs. Recovery therefore uses
// XHCI_RECOVER_CMD_MS. Every pre-existing caller keeps 5000ms via the
// xhci_send_command() wrapper below, so no existing behaviour changes.
static int xhci_send_command_to(xhci_controller_t *xhc, xhci_trb_t *cmd,
                                uint32_t timeout_ms) {
    // #134: everything from the ring enqueue to consuming g_cmd_cc is ONE
    // critical section. See the block comment above.
    if (xhci_cmd_lock() < 0) {
        kprintf("[xHCI] command declined: ring busy and this context cannot wait\n");
        return -1;
    }

    xhci_trb_t *trb = xhci_ring_enqueue(&xhc->cmd_ring);
    if (!trb) {
        kprintf("[xHCI] command ring enqueue failed\n");
        xhci_cmd_unlock();
        return -1;
    }

    // Copy command TRB
    trb->parameter = cmd->parameter;
    trb->status = cmd->status;
    trb->control = (cmd->control & ~TRB_CYCLE) | xhc->cmd_ring.cycle_bit;

    // #348: clear the shared command-completion slot BEFORE ringing so a
    // concurrent drainer records THIS completion for us (race-free).
    g_cmd_cc = 0;

    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");

    // Ring the host controller doorbell
    xhci_ring_doorbell(xhc, 0, 0);

    // Wait for command completion event. Record the outcome for the on-screen
    // diagnostics: xhci_wait_for_event returns -1 uniquely on TIMEOUT (no
    // completion event ever seen), -cc on a real command error, or CC_SUCCESS.
    int r = xhci_wait_for_event(xhc, TRB_COMMAND_COMPLETION, timeout_ms);
    if (r == -1) {
        g_xhci_last_cmd_timeout = 1;
        g_xhci_last_cmd_cc = 0;
    } else if (r < 0) {
        g_xhci_last_cmd_timeout = 0;
        g_xhci_last_cmd_cc = -r;
    } else {
        g_xhci_last_cmd_timeout = 0;
        g_xhci_last_cmd_cc = r;
    }
    xhci_cmd_unlock();
    return r;
}

static int xhci_send_command(xhci_controller_t *xhc, xhci_trb_t *cmd) {
    return xhci_send_command_to(xhc, cmd, 5000);
}

// =============================================================================
// DCBAA (Device Context Base Address Array)
// =============================================================================

static int xhci_dcbaa_init(xhci_controller_t *xhc) {
    // Allocate DCBAA (max_slots + 1 entries, each 8 bytes)
    size_t dcbaa_size = (xhc->max_slots + 1) * sizeof(uint64_t);
    dcbaa_size = ALIGN_UP(dcbaa_size, PAGE_SIZE);

    xhc->dcbaa = (uint64_t *)pmm_alloc_pages(dcbaa_size / PAGE_SIZE);
    if (!xhc->dcbaa) {
        return -1;
    }
    xhc->dcbaa_phys = (uint64_t)xhc->dcbaa;
    memset(xhc->dcbaa, 0, dcbaa_size);

    // Initialize device context pointers
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        xhc->dev_ctx[i] = NULL;
        xhc->dev_ctx_phys[i] = 0;
    }

    // Program DCBAAP
    xhci_op_write64(xhc, XHCI_OP_DCBAAP, xhc->dcbaa_phys);

    return 0;
}

// =============================================================================
// Scratchpad Buffers (#307 real-HW: required by Intel xHCI)
// =============================================================================
//
// xHCI spec 4.20: if HCSPARAMS2 Max Scratchpad Buffers > 0, software must
// allocate that many controller-page-sized buffers, build an array of their
// physical base addresses (the Scratchpad Buffer Array, 64-byte aligned), and
// store a pointer to that array in DCBAA entry 0 BEFORE setting Run. Without
// this the controller has no private DMA scratch space and the first command
// issued after Run (Enable Slot) never completes - the exact real-iMac failure.
//
// QEMU's emulated xHCI reports Max Scratchpad Buffers == 0, so this function is
// a strict no-op there (num == 0 returns immediately, DCBAA[0] stays 0): the
// emulated path is byte-for-byte unchanged and cannot regress. Every allocation
// comes from pmm_alloc_pages, which on this kernel is capped to the sub-2GB
// identity-mapped range, so the controller can always reach these buffers.
static int xhci_scratchpad_init(xhci_controller_t *xhc) {
    uint32_t hcs2 = xhci_cap_read32(xhc, XHCI_CAP_HCSPARAMS2);
    uint32_t hi = XHCI_HCSPARAMS2_MAX_SPB_HI(hcs2);
    uint32_t lo = XHCI_HCSPARAMS2_MAX_SPB_LO(hcs2);
    uint32_t num = (hi << 5) | lo;

    xhc->num_scratchpad_bufs = num;
    xhc->scratchpad_array = NULL;
    xhc->scratchpad_array_phys = 0;

    if (num == 0) {
        return 0;   // no scratchpad needed (QEMU path)
    }

    // Controller page size from the PAGESIZE operational register: bit n set
    // means the controller uses 2^(n+12)-byte pages. Real Intel HW uses 4KB
    // (bit 0), which matches our PMM allocation granularity so a page-aligned
    // pmm_alloc_pages result is correctly aligned for a scratchpad buffer.
    uint32_t ps = xhci_op_read32(xhc, XHCI_OP_PAGESIZE) & 0xFFFF;
    uint32_t page_bytes = PAGE_SIZE;
    for (int b = 0; b < 16; b++) {
        if (ps & (1u << b)) { page_bytes = 1u << (b + 12); break; }
    }
    if (page_bytes < PAGE_SIZE) page_bytes = PAGE_SIZE;
    uint32_t pages_each = page_bytes / PAGE_SIZE;
    if (pages_each == 0) pages_each = 1;

    // Scratchpad buffer array: num 64-bit physical pointers, page-aligned.
    size_t arr_bytes = ALIGN_UP((size_t)num * sizeof(uint64_t), PAGE_SIZE);
    uint64_t arr_phys = pmm_alloc_pages(arr_bytes / PAGE_SIZE);
    if (arr_phys == 0) {
        kprintf("[xHCI] Scratchpad: failed to allocate buffer array (%u bufs)\n", num);
        return -1;
    }
    uint64_t *arr = (uint64_t *)arr_phys;
    memset(arr, 0, arr_bytes);

    // Allocate each scratchpad buffer and record its base in the array.
    for (uint32_t i = 0; i < num; i++) {
        uint64_t bp = pmm_alloc_pages(pages_each);
        if (bp == 0) {
            kprintf("[xHCI] Scratchpad: failed to allocate buffer %u/%u\n", i + 1, num);
            return -1;
        }
        memset((void *)bp, 0, page_bytes);
        arr[i] = bp;
    }

    xhc->scratchpad_array = arr;
    xhc->scratchpad_array_phys = arr_phys;

    // DCBAA entry 0 points at the scratchpad buffer array.
    xhc->dcbaa[0] = arr_phys;
    __asm__ volatile("mfence" ::: "memory");

    kprintf("[xHCI] Scratchpad: %u buffer(s) of %u bytes, array phys 0x%016lx\n",
            num, page_bytes, arr_phys);
    bootlog_write("[xHCI] Scratchpad: %u buffer(s) %u bytes, array 0x%016lx",
                  num, page_bytes, arr_phys);
    return 0;
}

// =============================================================================
// Controller Reset
// =============================================================================

int xhci_reset(xhci_controller_t *xhc) {
    kprintf("[xHCI] Resetting controller...\n");

    // Wait for controller to be ready
    int timeout = 100;
    while (xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_CNR) {
        xhci_delay(1);
        if (--timeout == 0) {
            kprintf("[xHCI] ERROR: Controller not ready before reset\n");
            return -1;
        }
    }

    // Stop the controller if running
    uint32_t cmd = xhci_op_read32(xhc, XHCI_OP_USBCMD);
    if (!(xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        xhci_op_write32(xhc, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);

        // Wait for halt
        timeout = 100;
        while (!(xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
            xhci_delay(1);
            if (--timeout == 0) {
                kprintf("[xHCI] ERROR: Controller did not halt\n");
                return -1;
            }
        }
    }

    // Issue reset
    xhci_op_write32(xhc, XHCI_OP_USBCMD, XHCI_CMD_HCRST);

    // Wait for reset to complete
    timeout = 1000;
    while (xhci_op_read32(xhc, XHCI_OP_USBCMD) & XHCI_CMD_HCRST) {
        xhci_delay(1);
        if (--timeout == 0) {
            kprintf("[xHCI] ERROR: Reset did not complete\n");
            return -1;
        }
    }

    // Wait for CNR to clear
    timeout = 1000;
    while (xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_CNR) {
        xhci_delay(1);
        if (--timeout == 0) {
            kprintf("[xHCI] ERROR: Controller not ready after reset\n");
            return -1;
        }
    }

    kprintf("[xHCI] Reset complete\n");
    return 0;
}

// =============================================================================
// Controller Start
// =============================================================================

int xhci_start(xhci_controller_t *xhc) {
    // Set max device slots enabled
    uint32_t config = xhci_op_read32(xhc, XHCI_OP_CONFIG);
    config = (config & ~0xFF) | xhc->max_slots;
    xhci_op_write32(xhc, XHCI_OP_CONFIG, config);

    // Enable interrupts and run
    uint32_t cmd = xhci_op_read32(xhc, XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RUN | XHCI_CMD_INTE | XHCI_CMD_HSEE;
    xhci_op_write32(xhc, XHCI_OP_USBCMD, cmd);

    // Wait for controller to start
    int timeout = 100;
    while (xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        xhci_delay(1);
        if (--timeout == 0) {
            kprintf("[xHCI] ERROR: Controller did not start\n");
            return -1;
        }
    }

    kprintf("[xHCI] Controller started\n");
    return 0;
}

void xhci_stop(xhci_controller_t *xhc) {
    uint32_t cmd = xhci_op_read32(xhc, XHCI_OP_USBCMD);
    xhci_op_write32(xhc, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);

    // Wait for halt
    int timeout = 100;
    while (!(xhci_op_read32(xhc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        xhci_delay(1);
        if (--timeout == 0) break;
    }
}

// =============================================================================
// Controller Initialization
// =============================================================================

int xhci_init(pci_device_t *pci) {
    if (xhci_controller_count >= MAX_XHCI_CONTROLLERS) {
        kprintf("[xHCI] Maximum controllers reached\n");
        return -1;
    }

    xhci_controller_t *xhc = &xhci_controllers[xhci_controller_count];
    memset(xhc, 0, sizeof(xhci_controller_t));
    xhc->pci = pci;

    kprintf("[xHCI] Initializing controller at PCI %02x:%02x.%x\n",
            pci->bus, pci->slot, pci->func);

    // Enable bus mastering and memory space
    pci_enable_bus_master(pci);

    // #366: Intel xHCI USB2/USB3 port-routing hand-off (Lynx Point et al).
    // On Intel 8-series chipsets (iMac14,4) USB2 devices sit on the EHCI
    // companion controller after firmware hand-off, so the xHCI port scan
    // finds NOTHING (no boot stick, no keyboard, no #362 Ethernet dongle).
    // Mirror Linux usb_enable_intel_xhci_ports: enable SuperSpeed on the
    // switchable USB3 ports (USB3_PSSEN = USB3PRM), then route the
    // switchable USB2 ports from EHCI to xHCI (XUSB2PR = XUSB2PRM).
    // Strictly gated on PCI vendor 0x8086: QEMU (1b36:000d), NEC, Fresco
    // and every existing test path are untouched.
    int intel_routed = 0;
    uint32_t xusb2pr = 0;
    if (pci->vendor_id == 0x8086) {
        uint32_t usb3prm = pci_read32(pci->bus, pci->slot, pci->func, 0xD4);
        pci_write32(pci->bus, pci->slot, pci->func, 0xD0, usb3prm);
        uint32_t xusb2prm = pci_read32(pci->bus, pci->slot, pci->func, 0xDC);
        pci_write32(pci->bus, pci->slot, pci->func, 0xD8, xusb2prm);
        uint32_t pssen = pci_read32(pci->bus, pci->slot, pci->func, 0xD0);
        xusb2pr = pci_read32(pci->bus, pci->slot, pci->func, 0xD8);
        intel_routed = 1;
        kprintf("[xHCI] Intel port routing: USB3_PSSEN=0x%08x (PRM 0x%08x), XUSB2PR=0x%08x (PRM 0x%08x)\n",
                pssen, usb3prm, xusb2pr, xusb2prm);
        bootlog_write("[xHCI] Intel USB port routing applied: USB3_PSSEN=0x%08x XUSB2PR=0x%08x",
                      pssen, xusb2pr);
    }

    // #366: on-screen diagnostics. The boot splash's log window is the ONLY
    // debugging channel on real hardware when storage never mounts (the boot
    // log file can't be written); these lines stay on screen for a photo in
    // exactly that failure case (a successful boot clears them when the boot
    // image loads, which is fine - they're not needed then).
    {
        extern void gfx_boot_log(const char *message);
        char dl[96];
        snprintf(dl, sizeof(dl), "[USB] xHCI %04x:%04x at PCI %02x:%02x.%x%s",
                 pci->vendor_id, pci->device_id, pci->bus, pci->slot, pci->func,
                 intel_routed ? " (Intel: USB2 ports routed)" : "");
        gfx_boot_log(dl);
    }

    // #307/#240: enumerate EVERY USB host controller on the PCI bus and print
    // it on screen ONCE. If the iMac's keyboard/stick are behind a standalone
    // EHCI (Lynx Point 00:1A.0 / 00:1D.0) rather than this xHCI, that box will
    // show up here and prove #240 EHCI (not more xHCI port power) is required.
    // Brute-force config-space walk: does not depend on the PCI driver table.
    {
        extern void gfx_boot_log(const char *message);
        static int usb_ctrl_list_done = 0;
        if (!usb_ctrl_list_done) {
            usb_ctrl_list_done = 1;
            for (uint32_t b = 0; b < 256; b++) {
                for (uint32_t d = 0; d < 32; d++) {
                    for (uint32_t f = 0; f < 8; f++) {
                        uint32_t idv = pci_read32(b, d, f, 0x00);
                        if ((idv & 0xffff) == 0xffff) { if (f == 0) break; else continue; }
                        uint32_t cls = pci_read32(b, d, f, 0x08); // class/sub/progif in 31:8
                        uint8_t base = (cls >> 24) & 0xff;
                        uint8_t sub  = (cls >> 16) & 0xff;
                        uint8_t pif  = (cls >> 8)  & 0xff;
                        if (base == 0x0c && sub == 0x03) { // Serial bus / USB
                            const char *k = (pif == 0x30) ? "xHCI" :
                                            (pif == 0x20) ? "EHCI" :
                                            (pif == 0x10) ? "OHCI" :
                                            (pif == 0x00) ? "UHCI" : "USB?";
                            char cl[96];
                            snprintf(cl, sizeof(cl), "[USB] %s %04x:%04x @%02x:%02x.%x cls %02x/%02x/%02x",
                                     k, idv & 0xffff, (idv >> 16) & 0xffff,
                                     b, d, f, base, sub, pif);
                            gfx_boot_log(cl);
                            kprintf("%s\n", cl);
                        }
                    }
                }
            }
        }
    }

    // Get MMIO base address from BAR0
    uint64_t mmio_base = pci_get_bar_address(pci, 0);
    if (mmio_base == 0) {
        kprintf("[xHCI] ERROR: Failed to get BAR0 address\n");
        return -1;
    }

    xhc->mmio_base = (volatile uint8_t *)mmio_base;
    kprintf("[xHCI] MMIO base: 0x%016lx\n", mmio_base);
    bootlog_write("[xHCI] Controller at PCI %02x:%02x.%x, MMIO 0x%016lx",
                  pci->bus, pci->slot, pci->func, mmio_base);

    // Read capability registers
    uint32_t cap_length = xhci_cap_read32(xhc, XHCI_CAP_CAPLENGTH) & 0xFF;
    uint32_t hci_version = (xhci_cap_read32(xhc, XHCI_CAP_CAPLENGTH) >> 16) & 0xFFFF;

    kprintf("[xHCI] Capability length: 0x%02x, Version: %x.%x.%x\n",
            cap_length, (hci_version >> 8) & 0xF, (hci_version >> 4) & 0xF, hci_version & 0xF);

    // Calculate register offsets
    xhc->op_regs = xhc->mmio_base + cap_length;

    uint32_t rtsoff = xhci_cap_read32(xhc, XHCI_CAP_RTSOFF) & ~0x1F;
    xhc->rt_regs = xhc->mmio_base + rtsoff;

    uint32_t dboff = xhci_cap_read32(xhc, XHCI_CAP_DBOFF) & ~0x3;
    xhc->doorbells = (volatile uint32_t *)(xhc->mmio_base + dboff);

    // Read structural parameters
    uint32_t hcsparams1 = xhci_cap_read32(xhc, XHCI_CAP_HCSPARAMS1);
    xhc->max_slots = XHCI_HCSPARAMS1_MAX_SLOTS(hcsparams1);
    xhc->max_interrupters = XHCI_HCSPARAMS1_MAX_INTRS(hcsparams1);
    xhc->max_ports = XHCI_HCSPARAMS1_MAX_PORTS(hcsparams1);

    kprintf("[xHCI] Max slots: %u, Max ports: %u, Max interrupters: %u\n",
            xhc->max_slots, xhc->max_ports, xhc->max_interrupters);
    bootlog_write("[xHCI] Max slots %u, max ports %u, max interrupters %u",
                  xhc->max_slots, xhc->max_ports, xhc->max_interrupters);

    // Read capability parameters
    uint32_t hccparams1 = xhci_cap_read32(xhc, XHCI_CAP_HCCPARAMS1);
    xhc->has_64bit = XHCI_HCCPARAMS1_AC64(hccparams1);
    xhc->context_size = XHCI_HCCPARAMS1_CSZ(hccparams1) ? 64 : 32;

    kprintf("[xHCI] 64-bit: %s, Context size: %u bytes\n",
            xhc->has_64bit ? "yes" : "no", xhc->context_size);

    // #135: USBLEGSUP BIOS->OS hand-off. It MUST run here and not earlier: it
    // needs xhc->mmio_base, which is only assigned from BAR0 above. It must also
    // run BEFORE xhci_reset(), so the controller is ours before we reset it.
    xhci_bios_handoff(xhc);

    // Reset controller
    if (xhci_reset(xhc) < 0) {
        return -1;
    }

    // Initialize DCBAA
    if (xhci_dcbaa_init(xhc) < 0) {
        kprintf("[xHCI] ERROR: Failed to initialize DCBAA\n");
        return -1;
    }

    // #307 real-HW: allocate scratchpad buffers (DCBAA[0]) BEFORE Run. Must run
    // after dcbaa_init (needs xhc->dcbaa) and before xhci_start (the controller
    // reads DCBAA[0] once Run is set and the first command is issued). No-op on
    // QEMU (0 scratchpad buffers reported). A failure here is fatal because the
    // controller cannot function without the scratchpad space it asked for.
    if (xhci_scratchpad_init(xhc) < 0) {
        kprintf("[xHCI] ERROR: Failed to initialize scratchpad buffers\n");
        return -1;
    }

    // Initialize command ring
    if (xhci_command_ring_init(xhc) < 0) {
        kprintf("[xHCI] ERROR: Failed to initialize command ring\n");
        return -1;
    }

    // Initialize event ring
    if (xhci_event_ring_init(xhc) < 0) {
        kprintf("[xHCI] ERROR: Failed to initialize event ring\n");
        return -1;
    }

    // Start controller
    if (xhci_start(xhc) < 0) {
        return -1;
    }

    xhc->initialized = 1;
    xhci_controller_count++;

    // #307 real-HW (b577): decisive on-screen controller-state dump, printed
    // ONCE right after the controller is running and every DMA structure is
    // programmed, so the next iMac photo shows exactly what the FIRST command
    // (Enable Slot) will run against. It answers the two open real-HW questions:
    //   1. Are the command/event/DCBAA/scratchpad structures >4GB? The full
    //      64-bit physical addresses are printed; on this kernel the PMM caps
    //      them below 2GB, so any value > 0xFFFFFFFF would be a smoking gun.
    //   2. Did the controller get its required scratchpad? SPB shows the count
    //      and spArr shows the array we installed in DCBAA[0].
    // The register read-backs (CRCR/DCBAAP/ERSTBA/ERDP) show the FULL 64-bit
    // value the controller actually latched, confirming the high dwords.
    {
        extern void gfx_boot_log(const char *message);
        uint32_t hcc    = xhci_cap_read32(xhc, XHCI_CAP_HCCPARAMS1);
        uint32_t usbcmd = xhci_op_read32(xhc, XHCI_OP_USBCMD);
        uint32_t usbsts = xhci_op_read32(xhc, XHCI_OP_USBSTS);
        uint64_t crcr   = xhci_op_read64(xhc, XHCI_OP_CRCR);
        uint64_t dcbaap = xhci_op_read64(xhc, XHCI_OP_DCBAAP);
        uint64_t erstba = xhci_rt_read64(xhc, XHCI_RT_IR0 + XHCI_IR_ERSTBA);
        uint64_t erdp   = xhci_rt_read64(xhc, XHCI_RT_IR0 + XHCI_IR_ERDP);
        char d[96];
        snprintf(d, sizeof(d), "[USB] HCC=%08x AC64=%u CSZ=%u ctx=%u SPB=%u",
                 hcc, hcc & 1, (hcc >> 2) & 1, xhc->context_size,
                 xhc->num_scratchpad_bufs);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        snprintf(d, sizeof(d), "[USB] CMD=%08x STS=%08x run=%u hlt=%u hce=%u",
                 usbcmd, usbsts, usbcmd & 1, usbsts & 1, (usbsts >> 12) & 1);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        snprintf(d, sizeof(d), "[USB] cmdR=%016lx evtR=%016lx",
                 xhc->cmd_ring.phys_addr, xhc->event_ring.phys_addr);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        snprintf(d, sizeof(d), "[USB] DCBAA=%016lx spArr=%016lx",
                 xhc->dcbaa_phys, xhc->scratchpad_array_phys);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        snprintf(d, sizeof(d), "[USB] CRCR=%016lx DCBAAP=%016lx",
                 crcr, dcbaap);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        snprintf(d, sizeof(d), "[USB] ERSTBA=%016lx ERDP=%016lx",
                 erstba, erdp);
        gfx_boot_log(d); kprintf("[xHCI] %s\n", d);
        bootlog_write("[xHCI] state cmdR=%016lx evtR=%016lx DCBAA=%016lx spArr=%016lx SPB=%u",
                      xhc->cmd_ring.phys_addr, xhc->event_ring.phys_addr,
                      xhc->dcbaa_phys, xhc->scratchpad_array_phys,
                      xhc->num_scratchpad_bufs);
    }

    // #307: assert Port Power on every unpowered root-hub port, preserving the
    // RW1C change bits (write 0 to CSC/PEC/WRC/OCC/PRC/PLC/CEC so we don't
    // clear them) and never touching PED (write-1-disables) or PR (write-1
    // resets). QEMU already powers its ports so this is a no-op there; on real
    // Intel HW the ports come up unpowered after our reset and CCS reads 0
    // until PP is asserted and the connect debounce elapses.
    {
        uint32_t rw1c = XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC |
                        XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC |
                        XHCI_PORTSC_CEC;
        // #433 warm-reboot hardening. A warm reboot does NOT power-cycle USB, so
        // firmware or the previously running OS can leave root ports powered
        // (PP=1) with stale link state. The old code TRUSTED that pre-init state:
        // it asserted PP only on ports that read PP=0, so a warm-booted port that
        // was already PP=1 was never cleanly re-initialized, and on the real iMac
        // that left HID (interrupt-endpoint) ports enumerating unreliably from
        // one boot to the next. Do NOT trust firmware pre-init state: ALWAYS
        // power-cycle EVERY port (drive PP off, settle, then PP on) so every
        // device re-attaches from a clean cold state. This runs after the HCRST
        // controller reset in xhci_reset(). QEMU models PP and re-reports connect
        // across the off->on cycle, so the emulated path still ends PP=1 CCS=1.
        for (uint32_t port = 0; port < xhc->max_ports; port++) {
            uint32_t v = xhci_portsc_read(xhc, port);
            // RW1C-safe neutral with PP cleared: power the port OFF without
            // clobbering unconsumed change bits, disabling (PED), or resetting.
            v &= ~(rw1c | XHCI_PORTSC_PED | XHCI_PORTSC_PR | XHCI_PORTSC_PP);
            xhci_portsc_write(xhc, port, v);
        }
        // Let the ports fully power down before powering back on (~50ms margin).
        for (int i = 0; i < 50; i++) xhci_delay(1);
        int powered_now = 0;
        for (uint32_t port = 0; port < xhc->max_ports; port++) {
            uint32_t v = xhci_portsc_read(xhc, port);
            v &= ~(rw1c | XHCI_PORTSC_PED | XHCI_PORTSC_PR);
            v |= XHCI_PORTSC_PP;
            xhci_portsc_write(xhc, port, v);
            powered_now++;
        }
        // USB2 connect debounce is ~100ms; use ~250ms total for margin so the
        // freshly re-powered ports settle and CCS/CSC latch before we scan.
        for (int i = 0; i < 250; i++) xhci_delay(1);
        kprintf("[xHCI] Port power cycled off->on on %d port(s), debounced\n", powered_now);
        bootlog_write("[xHCI] Warm-reboot hardening: power-cycled %d port(s) off->on", powered_now);

        // #307: per-port PORTSC dump (raw hex + decoded PP/CCS/CSC/PED/PLS/spd)
        // AFTER power+debounce. One compact line per port so all 13 fit a photo.
        // This is THE definitive real-HW readout: PP=1 CCS=1 -> fixed; PP=1
        // CCS=0 on all ports (plus an EHCI in the controller list above) -> the
        // devices are on the EHCI, #240 needed.
        {
            extern void gfx_boot_log(const char *message);
            for (uint32_t port = 0; port < xhc->max_ports; port++) {
                uint32_t v = xhci_portsc_read(xhc, port);
                char pl[80];
                snprintf(pl, sizeof(pl),
                         "P%u PORTSC=%08x PP=%u CCS=%u CSC=%u PED=%u PLS=%u spd=%u",
                         port + 1, v,
                         (v & XHCI_PORTSC_PP)  ? 1 : 0,
                         (v & XHCI_PORTSC_CCS) ? 1 : 0,
                         (v & XHCI_PORTSC_CSC) ? 1 : 0,
                         (v & XHCI_PORTSC_PED) ? 1 : 0,
                         (v >> 5) & 0xf,        // PLS = bits 8:5
                         (v >> 10) & 0xf);      // Port Speed = bits 13:10
                gfx_boot_log(pl);
                kprintf("[xHCI] %s\n", pl);
            }
        }
    }

    // Scan ports
    kprintf("[xHCI] Scanning %u ports...\n", xhc->max_ports);
    bootlog_write("[xHCI] Port scan starting (%u ports)", xhc->max_ports);
    int connected_ports = 0;
    {
        extern void gfx_boot_log(const char *message);
        char pl[96];
        int pn = 0;
        pl[0] = 0;
        for (uint32_t port = 0; port < xhc->max_ports; port++) {
            xhci_dump_port_status(xhc, port);
            if (xhci_port_is_connected(xhc, port)) {
                uint32_t portsc = xhci_portsc_read(xhc, port);
                int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
                bootlog_write("[xHCI] Port %u: connected, speed %s", port + 1, xhci_speed_name(speed));
                // #366: compact on-screen summary, e.g. "p2:SS p6:HS p8:FS"
                if (pn < (int)sizeof(pl) - 12) {
                    const char *sn = (speed == 4 || speed == 5) ? "SS" :
                                     (speed == 3) ? "HS" :
                                     (speed == 2) ? "LS" : "FS";
                    pn += snprintf(pl + pn, sizeof(pl) - pn, "%sp%u:%s",
                                   pn ? " " : "", port + 1, sn);
                }
                connected_ports++;
            }
        }
        bootlog_write("[xHCI] Port scan complete: %d of %u ports connected",
                      connected_ports, xhc->max_ports);
        char sl[128];
        snprintf(sl, sizeof(sl), "[USB] ports connected: %d of %u  %s",
                 connected_ports, xhc->max_ports, connected_ports ? pl : "(NONE)");
        gfx_boot_log(sl);
    }

    // #307 (real-HW iMac): connected USB-2 root ports come up in Polling
    // (PLS=7, PED=0) and do NOT auto-enable the way USB-3 ports do. Software
    // must issue an explicit Port Reset to drive them Polling -> Enabled/U0
    // before Enable Slot / Address Device can enumerate the device. On the
    // physical iMac (v1.59/b573) the 4 connected ports read PED=0 PLS=7 and
    // enumeration was 0; this is the unblock. QEMU auto-enables its ports on
    // connect (PED=1 already), so this pass is a strict no-op there and cannot
    // regress the emulated path (the `!CCS || PED` guard skips every QEMU
    // port). The on-screen PR->PED progression makes the next iMac photo
    // decisive: PED flips to 1 -> fixed; PED stays 0 -> the reset itself failed.
    {
        extern void gfx_boot_log(const char *message);
        int idx = xhci_ctrl_index(xhc);
        int reset_ok = 0, reset_fail = 0;
        for (uint32_t port = 0; port < xhc->max_ports; port++) {
            uint32_t v = xhci_portsc_read(xhc, port);
            if (!(v & XHCI_PORTSC_CCS) || (v & XHCI_PORTSC_PED)) {
                continue;   // only connected-but-not-enabled ports need a reset
            }
            int r = xhci_port_reset(xhc, port);
            uint32_t nv = xhci_portsc_read(xhc, port);
            char rl[80];
            if (r == 0 && (nv & XHCI_PORTSC_PED)) {
                snprintf(rl, sizeof(rl), "[USB] P%u reset->PED=1 PLS=%u spd=%u OK",
                         port + 1, (nv >> 5) & 0xf, (nv >> 10) & 0xf);
                reset_ok++;
                gfx_boot_log(rl);
                kprintf("[xHCI] %s\n", rl);

                // #307 (b576): reset-then-enumerate. The port is now enabled
                // (PED=1, U0) with a freshly negotiated speed in its PORTSC
                // Port Speed field. Enumerate it RIGHT HERE, using that speed
                // for the slot-context Speed and EP0 max-packet guess. This is
                // the actual unblock on the real iMac: these USB-2 ports read
                // PED=0 during the initial scan (so plain enumeration skipped
                // them) and only become enabled by this reset pass. Mark the
                // port so the later xhci_enumerate_devices pass does not
                // enumerate it a second time. QEMU auto-enables its ports
                // (PED=1 already) so this branch never runs there - the QEMU
                // path stays entirely in xhci_enumerate_devices, unchanged.
                // #433: bounded-retry enumeration. The old code set
                // g_port_enumerated[idx][port]=1 BEFORE the attempt and
                // discarded the result, so a racy/failed enumeration
                // permanently flagged the port "done" and it was never retried
                // (the exact iMac symptom: keyboard enumerates on one boot,
                // dies the next). xhci_try_enumerate_port marks the port done
                // ONLY on success; on failure it retries with backoff and
                // leaves the port eligible for the periodic re-scan.
                int speed = (nv & XHCI_PORTSC_SPEED_MASK) >> 10;
                xhci_try_enumerate_port(xhc, port, speed, idx);
            } else {
                snprintf(rl, sizeof(rl), "[USB] P%u reset FAILED PED=%u PLS=%u",
                         port + 1, (nv & XHCI_PORTSC_PED) ? 1 : 0, (nv >> 5) & 0xf);
                reset_fail++;
                gfx_boot_log(rl);
                kprintf("[xHCI] %s\n", rl);
            }
        }
        if (reset_ok || reset_fail) {
            char sl[80];
            snprintf(sl, sizeof(sl), "[USB] port-reset: %d enabled, %d failed",
                     reset_ok, reset_fail);
            gfx_boot_log(sl);
            kprintf("[xHCI] %s\n", sl);
        }
    }

    return 0;
}

// =============================================================================
// Port Operations
// =============================================================================

int xhci_port_is_connected(xhci_controller_t *xhc, int port) {
    uint32_t portsc = xhci_portsc_read(xhc, port);
    return (portsc & XHCI_PORTSC_CCS) ? 1 : 0;
}

int xhci_port_get_speed(xhci_controller_t *xhc, int port) {
    uint32_t portsc = xhci_portsc_read(xhc, port);
    return (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
}

// All seven RW1C (write-1-to-clear) PORTSC change bits, in one mask.
#define XHCI_PORTSC_CHANGE_BITS (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                 XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                                 XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                 XHCI_PORTSC_CEC)

int xhci_port_reset(xhci_controller_t *xhc, int port) {
    kprintf("[xHCI] Resetting port %d...\n", port + 1);

    // #307 (real-HW iMac): drive a connected port from Polling (PLS=7) into
    // Enabled/U0 with an explicit Port Reset. Build an RW1C-safe "neutral"
    // PORTSC value first: preserve PP (and the RO/RWS bits), but write 0 to
    // every RW1C change bit so we do NOT clobber a change the driver has not
    // yet consumed, and write 0 to PED (write-1-disables), PR/WPR and LWS so
    // ONLY the reset we OR in below takes effect. This mirrors Linux
    // xhci_port_state_to_neutral() + hub_port_init().
    uint32_t clear_mask = XHCI_PORTSC_CHANGE_BITS | XHCI_PORTSC_PED |
                          XHCI_PORTSC_PR | XHCI_PORTSC_WPR | XHCI_PORTSC_LWS;
    uint32_t portsc = xhci_portsc_read(xhc, port);
    uint32_t neutral = portsc & ~clear_mask;

    // Assert Port Reset (PR, RW1S). PP is preserved by the neutral value.
    xhci_portsc_write(xhc, port, neutral | XHCI_PORTSC_PR);

    // Wait for reset completion, bounded by the PIT-calibrated delay. A
    // successful USB-2 root-port reset latches PRC, sets PED and parks the
    // link in U0 (PLS=0). Accept any of those signals; PR self-clears in HW.
    int timeout = 500;   // ~500 ms worst case
    while (timeout > 0) {
        xhci_delay(1);
        portsc = xhci_portsc_read(xhc, port);
        if ((portsc & XHCI_PORTSC_PRC) ||
            ((portsc & XHCI_PORTSC_PED) &&
             (portsc & XHCI_PORTSC_PLS_MASK) == XHCI_PORTSC_PLS_U0)) {
            break;
        }
        timeout--;
    }

    // Acknowledge the reset/link change bits (RW1C: write 1 to clear) while
    // still preserving PP and NOT disabling the port or re-asserting reset.
    portsc = xhci_portsc_read(xhc, port);
    xhci_portsc_write(xhc, port,
                      (portsc & ~clear_mask) | XHCI_PORTSC_CHANGE_BITS);

    // Re-read and judge the outcome purely on PED (the enable bit).
    portsc = xhci_portsc_read(xhc, port);
    int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
    if (!(portsc & XHCI_PORTSC_PED)) {
        kprintf("[xHCI] Port %d not enabled after reset (PORTSC=%08x PLS=%u)\n",
                port + 1, portsc, (portsc >> 5) & 0xf);
        return -1;
    }

    kprintf("[xHCI] Port %d reset complete, speed: %s\n", port + 1, xhci_speed_name(speed));
    return 0;
}

void xhci_dump_port_status(xhci_controller_t *xhc, int port) {
    uint32_t portsc = xhci_portsc_read(xhc, port);

    int connected = (portsc & XHCI_PORTSC_CCS) ? 1 : 0;
    int enabled = (portsc & XHCI_PORTSC_PED) ? 1 : 0;
    int powered = (portsc & XHCI_PORTSC_PP) ? 1 : 0;
    int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;

    if (connected) {
        kprintf("[xHCI]   Port %u: Connected, %s, %s, %s\n",
                port + 1,
                enabled ? "enabled" : "disabled",
                powered ? "powered" : "unpowered",
                xhci_speed_name(speed));
    }
}

// =============================================================================
// Event Handling
// =============================================================================

int xhci_process_event(xhci_controller_t *xhc, xhci_trb_t *event) {
    uint32_t type = XHCI_TRB_TYPE_GET(event->control);
    uint32_t cc = (event->status >> 24) & 0xFF;

    switch (type) {
        case TRB_TRANSFER_EVENT: {
            uint32_t slot = (event->control >> 24) & 0xFF;
            uint32_t ep = (event->control >> 16) & 0x1F;
            // #307/#348: record completion for BOTH the non-blocking
            // interrupt-IN (HID) path AND the blocking control/bulk (MSC) path.
            // The blocking waiter now reads this per-(slot,DCI) table instead of
            // consuming the event off the ring directly, so a poller that drains
            // the ring first can no longer steal a completion out from under it.
            if (slot >= 1 && slot <= XHCI_MAX_SLOTS && ep < XHCI_MAX_ENDPOINTS) {
                g_xfer_residual[slot - 1][ep] = event->status & 0xFFFFFF;
                // #139: stamp BEFORE publishing the completion code, so a HID
                // poller that observes cc != 0 always reads a stamp belonging
                // to THAT completion and never to the previous one.
                g_xfer_done_us[slot - 1][ep] = (uint32_t)mono_us();
                g_xfer_cc[slot - 1][ep] = cc ? cc : CC_SUCCESS;
            }
            // #155: hand the SAME completion to a queued-depth driver, which
            // cannot use the single cell above. See xhci_xfer_observer.
            if (xhci_xfer_observer)
                xhci_xfer_observer(slot, ep, (uint8_t)(cc ? cc : CC_SUCCESS),
                                   event->status & 0xFFFFFF);
            xhci_iso_xfer_events++;   // #323: flow-control counter
            if (xhci_xfer_log) {
                kprintf("[xHCI] Transfer event: slot %u, EP %u, CC=%s (%u)\n",
                        slot, ep, xhci_completion_code_name(cc), cc);
            }
            break;
        }

        case TRB_COMMAND_COMPLETION: {
            uint32_t slot = (event->control >> 24) & 0xFF;
            // #348: record for the drain-based command waiter (race-free, same
            // reasoning as transfer events above). xhci_enable_slot reads the
            // slot id from g_cmd_slot rather than off the ring.
            g_cmd_slot = (uint8_t)slot;
            g_cmd_cc = cc ? cc : CC_SUCCESS;
            kprintf("[xHCI] Command completion: slot %u, CC=%s\n",
                    slot, xhci_completion_code_name(cc));
            break;
        }

        case TRB_PORT_STATUS_CHANGE: {
            uint32_t port = ((event->parameter >> 24) & 0xFF) - 1;
            kprintf("[xHCI] Port %u status change\n", port + 1);
            xhci_dump_port_status(xhc, port);
            break;
        }

        default:
            kprintf("[xHCI] Unknown event type: %u\n", type);
            break;
    }

    return 0;
}

// #348: single shared event-ring drainer. BOTH the blocking waiters
// (xhci_wait_for_event / xhci_wait_transfer) and the non-blocking pollers
// (xhci_poll_events / xhci_int_in_poll) go through this. It TRY-acquires the
// event-ring lock: if another thread is already draining, it returns at once
// (that thread records every completion, including the one this caller waits
// on, into the shared g_xfer_cc / g_cmd_cc tables). This is the core of the
// HID+MSC coexistence fix: completions are recorded per (slot,DCI) and per
// command regardless of WHICH thread observes the event, so nothing is stolen.
// #426 (ASUS unknown-silicon bring-up): the drain loop below used to have NO
// iteration cap and NO deadline. Termination depended ENTIRELY on the
// controller's cycle-bit discipline being correct: a controller that leaves
// stale TRBs whose cycle bit matches across the whole segment, or a cycle_bit
// this driver mis-tracked after an error, spins here forever. The loop is
// reached from the MSI handler, from BOTH blocking waiters and from the
// periodic drain worker, so on unfamiliar xHCI silicon it is the single
// strongest hang candidate on the boot path, and this project's standing rule
// (#426) is that every device poll is bounded.
//
// The cap is expressed in terms of the ring itself, so it cannot silently
// become wrong if XHCI_RING_SIZE (or a future per-controller ring size)
// changes, with a generous floor for the degenerate size==0 case. It is
// UNREACHABLE in healthy operation: ERDP is advanced only AFTER the loop, so
// within ONE call the controller may legitimately produce at most one full
// segment of events (xhc->event_ring.size). Anything past 2x that means the
// controller is re-using entries this driver has not released yet, which is
// already an error, not a busy bus.
#define XHCI_DRAIN_CAP_MULT   2u
#define XHCI_DRAIN_CAP_FLOOR  512u
// How many cap trips get the full loud treatment. A wedged controller can trip
// on every single drain pass, and /USBLOG.TXT is written through the USB stack
// itself, so an unbounded report would be its own denial of service.
#define XHCI_DRAIN_CAP_REPORTS 8u

static uint32_t g_xhci_drain_cap_hits = 0;   // total trips, all controllers
static uint32_t g_xhci_drain_cap_reported = 0;
// Snapshot of the most recent trip, published for the deferred (blocking-safe)
// /USBLOG.TXT write below. Written under the event-ring lock, read after it.
static uint32_t g_xhci_drain_cap_pending = 0;
static int      g_xhci_drain_cap_ctrl = 0;
static uint32_t g_xhci_drain_cap_deq = 0;
static uint32_t g_xhci_drain_cap_cyc = 0;
static uint32_t g_xhci_drain_cap_cnt = 0;

static void xhci_drain_events(xhci_controller_t *xhc) {
    if (!xhci_evt_trylock()) return;

    uint32_t ir_offset = XHCI_RT_IR0;
    xhci_trb_t *event = &xhc->event_ring.trbs[xhc->event_ring.dequeue_idx];
    int processed = 0;
    uint32_t drained = 0;
    uint32_t drain_cap = xhc->event_ring.size * XHCI_DRAIN_CAP_MULT;
    if (drain_cap < XHCI_DRAIN_CAP_FLOOR) drain_cap = XHCI_DRAIN_CAP_FLOOR;

    while ((event->control & TRB_CYCLE) == xhc->event_ring.cycle_bit) {
        xhci_process_event(xhc, event);
        processed = 1;

        // Advance dequeue pointer
        xhc->event_ring.dequeue_idx++;
        if (xhc->event_ring.dequeue_idx >= xhc->event_ring.size) {
            xhc->event_ring.dequeue_idx = 0;
            xhc->event_ring.cycle_bit ^= 1;
        }
        event = &xhc->event_ring.trbs[xhc->event_ring.dequeue_idx];

        if (++drained >= drain_cap) {
            // Do NOT return silently: a silent bail here would turn a broken
            // controller into an unexplained "USB is slow / no storage found".
            // kprintf only at this point - we may be inside the MSI handler AND
            // we still hold the event-ring lock, and usblog_write() flushes
            // through the FAT/block layer, which on a USB-root box means
            // issuing xHCI transfers. Doing that from here would re-enter the
            // drainer with its lock held and hang. The on-disk line is written
            // after the unlock, and only from a context that may block.
            g_xhci_drain_cap_hits++;
            g_xhci_drain_cap_ctrl = (int)(xhc - xhci_controllers);
            g_xhci_drain_cap_deq  = xhc->event_ring.dequeue_idx;
            g_xhci_drain_cap_cyc  = xhc->event_ring.cycle_bit;
            g_xhci_drain_cap_cnt  = drained;
            g_xhci_drain_cap_pending = 1;
            kprintf("[xHCI] BUG: event drain hit cap %u (ctrl=%d deq=%u cycle=%u) "
                    "- possible stale cycle bit\n",
                    drained, g_xhci_drain_cap_ctrl,
                    xhc->event_ring.dequeue_idx, xhc->event_ring.cycle_bit);
            break;
        }
    }

    if (processed) {
        // Update ERDP once, after draining (EHB bit clears event-handler-busy).
        uint64_t erdp = xhc->event_ring.phys_addr +
                        xhc->event_ring.dequeue_idx * sizeof(xhci_trb_t);
        erdp |= (1 << 3);
        xhci_rt_write64(xhc, ir_offset + XHCI_IR_ERDP, erdp);
    }

    xhci_evt_unlock();

    // #426: the durable half of the cap report. The lock is released, so
    // usblog_write() may re-enter the drainer through the block layer, and
    // wq_may_block() (sync/noblock.h - the ONE canonical definition of "may
    // this context sleep", which xhci_may_block() below is an alias of) keeps
    // us out of ISR / pre-scheduler / interrupts-off contexts entirely. On a
    // stick-booted machine with no serial port, /USBLOG.TXT is the only place
    // this evidence can survive, which is exactly what made the iMac's USB
    // failure readable.
    if (g_xhci_drain_cap_pending && wq_may_block()) {
        g_xhci_drain_cap_pending = 0;
        if (g_xhci_drain_cap_reported < XHCI_DRAIN_CAP_REPORTS) {
            g_xhci_drain_cap_reported++;
            usblog_write("[xHCI] BUG: event drain hit cap %u (ctrl=%d deq=%u "
                         "cycle=%u) - possible stale cycle bit; trips=%u",
                         g_xhci_drain_cap_cnt, g_xhci_drain_cap_ctrl,
                         g_xhci_drain_cap_deq, g_xhci_drain_cap_cyc,
                         g_xhci_drain_cap_hits);
        }
    }

    // #614: wake anybody blocked waiting for a completion. Unconditional (not
    // only when `processed`), exactly as hda_service_stream() wakes
    // g_hda_space_wq: this function is also called from the periodic drain
    // workers, so an unconditional wake makes the wait SELF-HEALING - no lost
    // wakeup can outlive one drain pass - and costs a lock plus a NULL check
    // when nobody is waiting, which is the overwhelmingly common case. Every
    // waiter re-tests its own (slot,DCI) completion byte, so a spurious wake is
    // harmless by construction. wake_up_all() is documented safe from IRQ
    // context and is a no-op on an empty queue, so this is also safe on the
    // early-boot path where no process exists yet.
    wake_up_all(&g_xhci_evt_wq);
}

// #614: may THIS context sleep? Three conditions, all necessary:
//   - the scheduler is live (otherwise there is nothing to switch to);
//   - we are a process (pid 0 / pre-proc_init callers have no wait entry);
//   - interrupts are ON, i.e. we are not inside a cli+spinlock section or an
//     IRQ handler. #549 is the precedent: a TX path holding net_lock that
//     blocks on a wait queue DEADLOCKS, because the wake can never be
//     delivered. Callers of xhci_bulk_transfer()/xhci_control_transfer() are
//     not all under our control, so this is checked rather than assumed.
// #426 Phase 2: this WAS the private original of the rule. It has been
// generalised into sync/noblock.h (wq_may_block()) and asserted at the
// wait-queue and futex chokepoints, so this is now a one-line alias rather
// than a second, drifting copy of the same three conditions (CLAUDE.md: reuse
// the canonical primitive, never fork a private copy).
static inline int xhci_may_block(void) {
    return wq_may_block();
}

// #614: block until a drainer records a completion into *cc, or block_ms of the
// monotonic clock elapses. The prepare-then-RE-CHECK order is what closes the
// lost-wake window: we are already queued before we test *cc, so a drainer that
// records the completion between our test and our sleep still wakes us.
// The per-call cap is deliberate belt-and-braces: even if BOTH periodic drainers
// somehow stopped, this degrades to a bounded (zero-CPU) re-poll rather than
// stranding the caller until its overall transfer timeout.
#define XHCI_BLOCK_MS 10
static void xhci_block_for_cc(volatile uint8_t *cc) {
    wait_queue_entry_t wqe;
    __wait_prepare(&g_xhci_evt_wq, &wqe, 0);
    if (!*cc) {
        (void)__wait_event_wait_deadline(&wqe, sched_now_ms() + XHCI_BLOCK_MS);
    }
    __wait_finish(&g_xhci_evt_wq, &wqe);
}

// #348: block until the outstanding command completes. Drain-based and
// race-free: g_cmd_cc is set by whoever drains the ring. xhci_send_command
// clears g_cmd_cc before ringing the command doorbell. Retained under the old
// name/signature because the command ring is the only remaining "wait by type"
// user; transfers use xhci_wait_transfer (keyed by slot/DCI).
// #375 real-hardware CPU fix: the transfer/command wait used to poll the event
// ring and then BUSY-SPIN xhci_delay(1) (a PAUSE + PIT-latch loop) once per ms
// for the whole timeout budget. During early boot that is correct and necessary
// (interrupts are OFF, the scheduler is not running, and the PIT spin is the
// only wall-clock source). But once the scheduler is up, every runtime USB read
// (and there are many on a USB-root box) waits here too, and on a SLOW stick
// the transfer can take tens of ms - so a whole core gets pegged spinning in
// xhci_delay while the stick works. That is the "one core pegged" the iMac shows.
//
// Fix: keep the boot path byte-for-byte (busy-spin, iteration-count budget so
// the v1.65 bounded timeouts are preserved exactly), but at RUNTIME
// (sched_preemption_enabled()) switch to an adaptive poll-THEN-BLOCK: poll for
// a bounded ~1ms window at a FINE quantum (#619: XHCI_WAIT_POLL_US, a poll
// interval, NOT a spec delay) to catch the common quick completion with low
// latency, then block on g_xhci_evt_wq so the core idle-HLTs instead of burning
// while the slow stick transfers. The event ring is DMA'd by the controller regardless of
// interrupts, so draining after each sleep still observes the completion (no IRQ
// wiring needed). The runtime wait is bounded in real wall-clock via timer_ticks.
// #614: was 4. Each of these "spins" used to be an xhci_delay(1), a
// MILLISECOND-granularity busy delay, so the old value burned up to ~4ms of a
// core on EVERY transfer, and a USB-root box does tens of thousands of
// transfers per large file. #614 cut it to two spins so that everything slower
// BLOCKS on g_xhci_evt_wq at zero CPU instead of spinning.
//
// #619: that was still wrong, and #507 is what made the wrongness visible. A
// SPEC DELAY and a POLL INTERVAL are different things and must not share a
// primitive. xhci_delay() exists to satisfy USB's spec-mandated MINIMUM waits
// (port-reset recovery, the 2ms Set-Address recovery of USB 2.0 9.2.6.3, hub
// power-good); it is SUPPOSED to be at least as long as it claims, and #507
// fixed it to be exactly that (the old private PIT loop omitted the MODE 3
// factor of two, so every delay ran for HALF its nominal length). The fast path
// here is not waiting out a hardware-mandated gap: it is SAMPLING a DMA-written
// completion byte, and the only thing that makes a sampling interval correct is
// being as short as is cheap. Because this one call site borrowed the spec-delay
// primitive to do it, #507's correctness fix silently DOUBLED the completion
// polling quantum from ~0.5ms to a real 1ms. Enumeration was unaffected
// (+0.17s), but everything that reads through the USB block device roughly
// doubled: the boot fsck went 7.6s -> 14.2s on the same volume, and power-on to
// DESKTOP_READY went 56.0s -> 72.3s (measured, golden 979 vs golden 983).
//
// So the two uses are decoupled now. The spec delays KEEP #507's corrected
// duration, because that is the fix that matters on the real iMac. The fast path
// gets its OWN quantum, in microseconds, from the same shared monotonic clock
// (cpu/mono.h) that xhci_delay() itself uses, so there is still no private clock
// here. XHCI_WAIT_FAST_US is the total REAL-TIME budget of the fast path and is
// deliberately held at the pre-#507 value of ~1ms, so the CPU burned per
// transfer is no worse than the build this regressed from, while the SAMPLING is
// 40x finer: a completion that lands in microseconds (which is what a virtual
// bulk transfer does) is now observed within tens of microseconds instead of
// half a millisecond. Anything slower than the fast window still blocks on
// g_xhci_evt_wq at zero CPU, exactly as #614 intended.
//
// Why not delete the fast poll entirely and always block, now that #614/#616
// give this wait a real wait queue with two always-armed drainers? Because both
// drainers run at TICK granularity (this driver's own worker is a proc_sleep(1)
// loop, the HID worker is ~4ms), so a pure block would FLOOR per-transfer
// latency at about a tick and make the common case far worse than either 979 or
// 983. The short fine-grained poll is what keeps the common case fast; the wait
// queue is what keeps the slow case free. They are complementary, not
// alternatives.
#define XHCI_WAIT_POLL_US    25u
#define XHCI_WAIT_FAST_US    1000u
#define XHCI_WAIT_FAST_SPINS (XHCI_WAIT_FAST_US / XHCI_WAIT_POLL_US)

// sched_preemption_enabled() + proc_sleep() come from proc/process.h (included
// above for the #433 re-scan worker); do not re-declare sched_preemption_enabled
// here with a different bool width (it conflicts under -Werror).
extern volatile uint64_t timer_ticks;

// #525: the RUNTIME deadline below is measured with THE shared monotonic clock
// (cpu/mono.h, TSC-backed), NOT with timer_ticks. Ticks count DELIVERY, not
// TIME: under KVM a starved vCPU has its missed ticks REINJECTED in a burst, so
// timer_ticks leaps ~1250 (a nominal 5s at 250Hz) in ~15ms of real wall clock.
// The MSC bulk budget here is exactly 5000ms = 1250 ticks, i.e. precisely the
// size a single burst can erase, and this is the hottest path on a USB-root box
// (the live image reads its own root over USB-MSC). A premature return here is
// not a slow read: the transfer is still in flight, so the next CSW lands
// against the wrong command and desynchronises BOT ("Invalid CSW signature",
// usb_msc.c) into reset recovery. See #524/#499 and cpu/mono.h.
//
// NOTE the BOOT branch is deliberately left alone: it is already burst-immune,
// but NOT merely because it counts iterations. Each of its iterations is an
// xhci_delay(1) that measures real time off the PIT counter in hardware, so its
// "iteration budget" IS a wall clock. That is the property that matters, and it
// is why an iteration guard was the right shape there and the wrong shape here:
// this loop's iterations are paced by proc_sleep(1), i.e. by TICK DELIVERY, so
// a burst fabricates iterations and ticks together and a spin cap would count
// them just as fast. Only a clock outside the tick stream can fix this.
#include "../cpu/mono.h"

// #525 A/B HARNESS. Building with -DXHCI_LEGACY_TICK_DEADLINE restores the OLD
// tick-based deadline while KEEPING the monotonic clock for REPORTING only, so
// the two arms differ in exactly one variable: which clock the deadline is
// measured against. Both arms therefore print the same "%llums real, %llu
// ticks" diagnostic on timeout, which is what makes the comparison quantitative
// rather than a presence/absence argument. Not set in normal builds.
#ifdef XHCI_LEGACY_TICK_DEADLINE
static const int xhci_deadline_use_mono = 0;
#else
static const int xhci_deadline_use_mono = 1;
#endif

int xhci_wait_for_event(xhci_controller_t *xhc, uint32_t type, uint32_t timeout_ms) {
    int runtime = sched_preemption_enabled();
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t deadline = timer_ticks + ((uint64_t)timeout_ms * hz + 999) / 1000 + 1;
    // #525: real-time budget. mono_ok==0 (clock uncalibrated) keeps the old
    // tick deadline, so a calibration failure is never worse than before.
    int mono_ok = mono_ready();
    // mono_ok gates REPORTING; mono_dl gates the DEADLINE (see A/B harness).
    int mono_dl = mono_ok && xhci_deadline_use_mono;
    uint64_t mono_start = mono_ok ? mono_ms() : 0;
    uint64_t tick_start = timer_ticks;
    uint32_t spins = 0;
    for (;;) {
        xhci_drain_events(xhc);
        if (type == TRB_COMMAND_COMPLETION) {
            uint8_t cc = g_cmd_cc;
            if (cc) {
                g_cmd_cc = 0;
                if (cc == CC_SUCCESS) return cc;
                kprintf("[xHCI] Command error: %s (code %u)\n",
                        xhci_completion_code_name(cc), cc);
                return -cc;
            }
        }
        if (runtime) {
            // Expire on REAL elapsed time, not on ticks delivered (#525).
            if (mono_dl ? ((mono_ms() - mono_start) >= (uint64_t)timeout_ms)
                        : (timer_ticks >= deadline)) break;
            if (spins < XHCI_WAIT_FAST_SPINS) { mono_busy_delay_us(XHCI_WAIT_POLL_US); spins++; }
            else if (xhci_may_block()) xhci_block_for_cc(&g_cmd_cc);
            else proc_sleep(1);   // cannot sleep on a wq here; bounded fallback
        } else {
            // BOOT path (no scheduler, interrupts off): there is nothing to
            // block ON, so this is necessarily a busy poll. #619 applies the
            // same separation here. The QUANTUM is XHCI_WAIT_POLL_US, a poll
            // interval; the BUDGET is REAL ELAPSED TIME from the shared
            // monotonic clock, which is calibrated in main() before usb_init()
            // and is the very clock xhci_delay() itself reads, so the timeout
            // semantics are unchanged from the post-#507 build (there, one
            // iteration was one real millisecond, so the iteration budget WAS a
            // real-time budget; this states that directly instead of encoding
            // it in the quantum). The old iteration-count budget is kept
            // verbatim as the uncalibrated fallback, so a calibration failure
            // leaves this path exactly as it was and can never be worse.
            //
            // This branch is NOT a micro-optimisation: on a USB-root box the
            // pre-scheduler phase reads the whole FAT through it (the free-
            // cluster scan alone was 4.4s of a 57s boot at a 1ms quantum), and
            // that is why "USB init complete to ext2 mounted" doubled from
            // 5.97s to 11.07s between goldens 979 and 983.
            if (mono_ok) {
                if ((mono_ms() - mono_start) >= (uint64_t)timeout_ms) break;
                mono_busy_delay_us(XHCI_WAIT_POLL_US);
            } else {
                if (spins >= timeout_ms) break;   // pre-calibration fallback
                xhci_delay(1); spins++;
            }
        }
    }

    kprintf("[xHCI] Event wait timeout (%ums budget; %llums real, %llu ticks)\n",
            timeout_ms, mono_ok ? mono_ms() - mono_start : 0,
            timer_ticks - tick_start);
    bootlog_write("[xHCI] Command event wait TIMEOUT (%ums budget; %lums real, "
                  "%llu ticks elapsed)", timeout_ms,
                  mono_ok ? mono_ms() - mono_start : 0, timer_ticks - tick_start);
    return -1;
}

// #348: block until a transfer on (slot_id, DCI) completes. Race-free: the
// completion is recorded in g_xfer_cc[][] by whoever drains the ring (this
// waiter or a concurrent HID/UAC poller), so it can never be stolen. Caller
// MUST clear g_xfer_cc[slot-1][dci] before ringing the doorbell.
static int xhci_wait_transfer(xhci_controller_t *xhc, int slot_id, int dci,
                              uint32_t timeout_ms) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots ||
        dci < 1 || dci >= XHCI_MAX_ENDPOINTS) {
        return -1;
    }
    // #375: adaptive poll-then-block at runtime (see xhci_wait_for_event). This
    // is the HOTTEST path on a USB-root box - every MSC bulk read/write CSW waits
    // here - so it is what pegs a core on a slow stick. #619: the BOOT path below
    // is no longer unchanged either; it had the same borrowed-quantum defect and
    // it is the branch the whole pre-scheduler root read goes through.
    int runtime = sched_preemption_enabled();
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t deadline = timer_ticks + ((uint64_t)timeout_ms * hz + 999) / 1000 + 1;
    // #525: real-time budget. mono_ok==0 (clock uncalibrated) keeps the old
    // tick deadline, so a calibration failure is never worse than before.
    int mono_ok = mono_ready();
    // mono_ok gates REPORTING; mono_dl gates the DEADLINE (see A/B harness).
    int mono_dl = mono_ok && xhci_deadline_use_mono;
    uint64_t mono_start = mono_ok ? mono_ms() : 0;
    uint64_t tick_start = timer_ticks;
    uint32_t spins = 0;
    for (;;) {
        xhci_drain_events(xhc);
        uint8_t cc = g_xfer_cc[slot_id - 1][dci];
        if (cc) {
            g_xfer_cc[slot_id - 1][dci] = 0;
            if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) return cc;
            if (!xhci_iso_quiet) {
                kprintf("[xHCI] Transfer error slot %d DCI %d: %s (code %u)\n",
                        slot_id, dci, xhci_completion_code_name(cc), cc);
            }
            return -cc;
        }
        if (runtime) {
            // Expire on REAL elapsed time, not on ticks delivered (#525).
            if (mono_dl ? ((mono_ms() - mono_start) >= (uint64_t)timeout_ms)
                        : (timer_ticks >= deadline)) break;
            if (spins < XHCI_WAIT_FAST_SPINS) { mono_busy_delay_us(XHCI_WAIT_POLL_US); spins++; }
            else if (xhci_may_block()) xhci_block_for_cc(&g_xfer_cc[slot_id - 1][dci]);
            else proc_sleep(1);   // cannot sleep on a wq here; bounded fallback
        } else {
            // BOOT path (no scheduler, interrupts off): there is nothing to
            // block ON, so this is necessarily a busy poll. #619 applies the
            // same separation here. The QUANTUM is XHCI_WAIT_POLL_US, a poll
            // interval; the BUDGET is REAL ELAPSED TIME from the shared
            // monotonic clock, which is calibrated in main() before usb_init()
            // and is the very clock xhci_delay() itself reads, so the timeout
            // semantics are unchanged from the post-#507 build (there, one
            // iteration was one real millisecond, so the iteration budget WAS a
            // real-time budget; this states that directly instead of encoding
            // it in the quantum). The old iteration-count budget is kept
            // verbatim as the uncalibrated fallback, so a calibration failure
            // leaves this path exactly as it was and can never be worse.
            //
            // This branch is NOT a micro-optimisation: on a USB-root box the
            // pre-scheduler phase reads the whole FAT through it (the free-
            // cluster scan alone was 4.4s of a 57s boot at a 1ms quantum), and
            // that is why "USB init complete to ext2 mounted" doubled from
            // 5.97s to 11.07s between goldens 979 and 983.
            if (mono_ok) {
                if ((mono_ms() - mono_start) >= (uint64_t)timeout_ms) break;
                mono_busy_delay_us(XHCI_WAIT_POLL_US);
            } else {
                if (spins >= timeout_ms) break;   // pre-calibration fallback
                xhci_delay(1); spins++;
            }
        }
    }
    // The real-vs-ticks pair is the whole diagnostic: if a "5000ms" budget
    // expires after a handful of real ms while ticks show ~1250, the deadline
    // was erased by a tick burst, not by a slow device (#524/#499).
    kprintf("[xHCI] Transfer wait timeout (slot %d DCI %d; %ums budget, "
            "%llums real, %llu ticks)\n", slot_id, dci, timeout_ms,
            mono_ok ? mono_ms() - mono_start : 0, timer_ticks - tick_start);
    bootlog_write("[xHCI] Transfer wait TIMEOUT slot %d DCI %d (%ums budget; "
                  "%llums real, %llu ticks elapsed)", slot_id, dci, timeout_ms,
                  mono_ok ? mono_ms() - mono_start : 0, timer_ticks - tick_start);
    return -1;
}

void xhci_poll_events(xhci_controller_t *xhc) {
    // Non-blocking: drain whatever is available into the shared tables. If a
    // blocking waiter currently holds the drain lock, this is a no-op (that
    // waiter records our completions too), so no event is ever lost.
    xhci_drain_events(xhc);
}

// =============================================================================
// Slot and Device Operations
// =============================================================================

int xhci_enable_slot(xhci_controller_t *xhc) {
    kprintf("[xHCI] Enabling slot...\n");

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = XHCI_TRB_TYPE(TRB_ENABLE_SLOT);

    int result = xhci_send_command(xhc, &cmd);
    if (result < 0) {
        kprintf("[xHCI] Enable slot failed\n");
        return -1;
    }

    // #348: get the slot ID from the recorded command completion. Reading it
    // off the ring is unsafe now that any thread may drain (and thus advance the
    // dequeue pointer). xhci_send_command's drain stored it in g_cmd_slot.
    int slot_id = g_cmd_slot;

    kprintf("[xHCI] Enabled slot %d\n", slot_id);
    return slot_id;
}

int xhci_disable_slot(xhci_controller_t *xhc, int slot_id) {
    // #134: A DISABLED SLOT NUMBER IS IMMEDIATELY REUSABLE, AND THE HID TABLE
    // IS KEYED ON SLOT NUMBER. Before 40f51ce nothing in this kernel ever
    // disabled a slot at runtime, so slot ids only ever climbed (the 1902
    // hardware capture shows exactly that: 1,4,5,6,7,8,9 across replugs) and a
    // stale hid_devices[] entry could only ever be inert. Adding a teardown
    // created slot REUSE, and with it a new failure mode: an entry still
    // holding a recycled slot id makes the HID poll worker submit a Normal TRB
    // onto - and clear the completion byte of - whatever device now owns that
    // slot. A boot keyboard's interrupt-IN is DCI 3 and a USB mass-storage
    // device's bulk-IN is usually DCI 3 as well, and on this machine that MSC
    // device is the root filesystem.
    //
    // xhci_teardown_port_slots() does call usb_hid_detach_slot() first, but it
    // is keyed on g_slot_root_port[], which is written at exactly one site: any
    // HID slot that never passed through it is never released, and blame.md
    // records a real capture with no teardown line at all. A guard at one
    // caller is not an invariant. Releasing here, at the ONE place a slot is
    // freed, makes "no HID entry ever names a freed slot" true by construction
    // for every present and future caller. It is a no-op for a slot with no HID
    // entries, which is every non-HID caller of this function.
    usb_hid_detach_slot(xhc, slot_id);

    // #134 (2026-08-18): and the SAME argument applies to g_slot_root_port[].
    // It is the only record of which root port a slot hangs off, it is written
    // at exactly one site (xhci_address_device_ex) and it was cleared at
    // exactly one site (xhci_teardown_port_slots). Every OTHER disable - the
    // three enumeration-failure paths and the hub path - left the entry naming
    // a port for a slot that no longer exists. That stale entry is now
    // load-bearing: xhci_port_has_live_slot() uses this table to decide whether
    // a port marked "enumerated" really has a device, so a lie here would
    // become a port that can never be re-enumerated. Clear it at the ONE place
    // a slot is freed and it holds for every present and future caller.
    if (slot_id >= 1 && slot_id <= XHCI_MAX_SLOTS)
        g_slot_root_port[slot_id - 1] = 0;

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = XHCI_TRB_TYPE(TRB_DISABLE_SLOT) | ((slot_id & 0xFF) << 24);

    return xhci_send_command(xhc, &cmd);
}

// Allocate device context for a slot
static int xhci_alloc_device_context(xhci_controller_t *xhc, int slot_id) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) {
        return -1;
    }

    // Allocate device context (output context)
    size_t ctx_size = xhc->context_size * 32;  // Slot + 31 endpoints
    ctx_size = ALIGN_UP(ctx_size, PAGE_SIZE);

    uint64_t phys = pmm_alloc_pages(ctx_size / PAGE_SIZE);
    if (phys == 0) {
        return -1;
    }

    xhc->dev_ctx[slot_id - 1] = (xhci_device_ctx_t *)phys;
    xhc->dev_ctx_phys[slot_id - 1] = phys;
    memset(xhc->dev_ctx[slot_id - 1], 0, ctx_size);

    // Update DCBAA
    xhc->dcbaa[slot_id] = phys;

    return 0;
}

// #373 USB hub support: address a device with full slot-context control for
// devices attached BEHIND a hub. route_string encodes the hub port path (0 for
// a device on a root-hub port). root_hub_port is the 0-based ROOT port the whole
// tree hangs off. tt_hub_slot / tt_port_num are the Transaction Translator
// fields, required by the xHCI slot context for a Low/Full-Speed device that
// sits behind a High-Speed hub (0 = no TT, i.e. the device is High/Super-Speed
// or the entire path up to the root is Low/Full-Speed). Address Device for a
// device behind a hub FAILS unless the parent hub's slot context has already had
// its Hub bit + Number of Ports set (see xhci_configure_hub_slot) and the route
// string / TT fields here are correct.
static int xhci_address_device_ex(xhci_controller_t *xhc, int slot_id,
                                  int root_hub_port, uint32_t route_string,
                                  int speed, int tt_hub_slot, int tt_port_num) {
    kprintf("[xHCI] Addressing slot %d root-port %d route 0x%05x speed %s tt %d.%d\n",
            slot_id, root_hub_port + 1, route_string, xhci_speed_name(speed),
            tt_hub_slot, tt_port_num);

    // Allocate output device context
    if (xhci_alloc_device_context(xhc, slot_id) < 0) {
        kprintf("[xHCI] Failed to allocate device context\n");
        return -1;
    }

    // Allocate input context
    size_t input_size = xhc->context_size * 33;  // Input ctrl + slot + 31 endpoints
    input_size = ALIGN_UP(input_size, PAGE_SIZE);

    uint64_t input_phys = pmm_alloc_pages(input_size / PAGE_SIZE);
    if (input_phys == 0) {
        return -1;
    }

    xhci_input_ctx_t *input = (xhci_input_ctx_t *)input_phys;
    memset(input, 0, input_size);

    // Allocate transfer ring for endpoint 0 (control)
    xhci_ring_t *ep0_ring = (xhci_ring_t *)kmalloc(sizeof(xhci_ring_t));
    if (!ep0_ring) {
        pmm_free_pages(input_phys, input_size / PAGE_SIZE);
        return -1;
    }

    if (xhci_ring_init(ep0_ring, XHCI_RING_SIZE) < 0) {
        kfree(ep0_ring);
        pmm_free_pages(input_phys, input_size / PAGE_SIZE);
        return -1;
    }

    xhc->transfer_rings[slot_id - 1][0] = ep0_ring;

    // Setup input control context
    input->ctrl.add_flags = (1 << 0) | (1 << 1);  // Add slot and EP0 contexts

    // Setup slot context
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)((uint8_t *)input + xhc->context_size);
    slot_ctx->route_string = route_string & 0xFFFFF;
    slot_ctx->speed = speed;
    slot_ctx->context_entries = 1;  // Only EP0 for now
    slot_ctx->root_hub_port = root_hub_port + 1;
    // #134: remember which ROOT port this slot hangs off. The re-scan worker
    // needs it to know WHICH slots died when a port goes empty; without it the
    // driver had no way to associate a disconnect with the resources it had to
    // release, which is why nothing was ever released.
    if (slot_id >= 1 && slot_id <= XHCI_MAX_SLOTS)
        g_slot_root_port[slot_id - 1] = (int16_t)(root_hub_port + 1);
    // #373: TT fields for a Low/Full-Speed device behind a High-Speed hub.
    if (tt_hub_slot > 0) {
        slot_ctx->tt_hub_slot = tt_hub_slot & 0xFF;
        slot_ctx->tt_port_num = tt_port_num & 0xFF;
    }

    // Setup endpoint 0 context
    xhci_ep_ctx_t *ep0_ctx = (xhci_ep_ctx_t *)((uint8_t *)input + xhc->context_size * 2);

    // Max packet size depends on speed
    uint32_t max_packet;
    switch (speed) {
        case XHCI_SPEED_LOW:
            max_packet = 8;
            break;
        case XHCI_SPEED_FULL:
            // Full-speed control endpoints may be 8/16/32/64. Start at 8 (always
            // valid for the initial 8-byte descriptor read), then the enumerator
            // bumps it via Evaluate Context once bMaxPacketSize0 is known.
            max_packet = 8;
            break;
        case XHCI_SPEED_HIGH:
            max_packet = 64;
            break;
        case XHCI_SPEED_SUPER:
        case XHCI_SPEED_SUPER_PLUS:
            max_packet = 512;
            break;
        default:
            max_packet = 8;
            break;
    }

    ep0_ctx->ep_type = EP_TYPE_CONTROL;
    ep0_ctx->max_packet = max_packet;
    ep0_ctx->max_burst = 0;
    ep0_ctx->cerr = 3;  // 3 retries
    ep0_ctx->tr_dequeue = ep0_ring->phys_addr | ep0_ring->cycle_bit;
    ep0_ctx->avg_trb_len = 8;  // Average control transfer size

    // Send Address Device command
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = input_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_ADDRESS_DEVICE) | ((slot_id & 0xFF) << 24);

    int result = xhci_send_command(xhc, &cmd);

    // Free input context (no longer needed)
    pmm_free_pages(input_phys, input_size / PAGE_SIZE);

    if (result < 0) {
        kprintf("[xHCI] Address Device command failed\n");
        return -1;
    }

    kprintf("[xHCI] Device addressed successfully\n");
    // #433 (re-scoped): record TT (split-transaction) config for this slot to
    // /USBLOG.TXT. A Low/Full-Speed device BEHIND a High-Speed hub needs the slot
    // context's TT Hub Slot + TT Port fields set, or interrupt-IN TDs submit but
    // never complete (they need split transactions through the hub's TT). This
    // line makes it verifiable over SSH whether TT was configured for a Low-Speed
    // device (e.g. the iMac keyboard on slot 3 if it sits behind the 0a5c:4500
    // hub) - "no TT" on a hub-attached Low-Speed device would explain reports that
    // never complete. A Low-Speed device on a ROOT port needs no TT.
    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        if (tt_hub_slot > 0)
            usblog_write("  slot %d addressed: %s device BEHIND hub - TT configured "
                         "(TT hub slot %d, TT port %d, route 0x%05x, root port %d)",
                         slot_id, xhci_speed_name(speed), tt_hub_slot, tt_port_num,
                         route_string & 0xFFFFF, root_hub_port + 1);
        else
            usblog_write("  slot %d addressed: %s device on ROOT port %d - no TT "
                         "(route 0x%05x)", slot_id, xhci_speed_name(speed),
                         root_hub_port + 1, route_string & 0xFFFFF);
    } else {
        usblog_write("  slot %d addressed: %s device - no TT needed (route 0x%05x)",
                     slot_id, xhci_speed_name(speed), route_string & 0xFFFFF);
    }
    return 0;
}

// Root-port (no hub, no TT) wrapper preserving the public signature.
int xhci_address_device(xhci_controller_t *xhc, int slot_id, int port, int speed) {
    return xhci_address_device_ex(xhc, slot_id, port, 0, speed, 0, 0);
}

// =============================================================================
// Transfer Operations
// =============================================================================

// #373 real-HW hub regression: internal control transfer with an EXPLICIT
// completion timeout. Standard device enumeration keeps the generous 5s default
// (via the xhci_control_transfer wrapper below), but hub class requests
// (GET_STATUS / SET_FEATURE / CLEAR_FEATURE / GET_DESCRIPTOR) use a SHORT
// bounded timeout (HUB_CTRL_TIMEOUT_MS). The hub connect-debounce and reset
// polling loops issue HUNDREDS of these per port; at the 5s default each, a real
// High-Speed hub that does not answer instantly (unlike QEMU's Full-Speed
// usb-hub) would burn many minutes = an effective boot hang before the USB-MSC
// root ever mounts. That is exactly the iMac14,4 #373 regression. Returns -1 on
// timeout (no completion event within timeout_ms), -cc on a command error, or
// the success completion code, identical to xhci_wait_transfer's contract.
int xhci_control_transfer_to(xhci_controller_t *xhc, int slot_id,
                          uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length, uint32_t timeout_ms) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) {
        return -1;
    }

    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][0];
    if (!ring) {
        return -1;
    }

    // Setup TRB
    xhci_trb_t *setup = xhci_ring_enqueue(ring);
    if (!setup) return -1;
    uint64_t setup_data = (uint64_t)request_type |
                          ((uint64_t)request << 8) |
                          ((uint64_t)value << 16) |
                          ((uint64_t)index << 32) |
                          ((uint64_t)length << 48);
    setup->parameter = setup_data;
    setup->status = 8;  // TRB transfer length = 8 for setup
    uint32_t trt = (length > 0) ? ((request_type & 0x80) ? 3 : 2) : 0;  // Transfer type
    setup->control = XHCI_TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16) | ring->cycle_bit;

    // Data TRB (if needed)
    if (length > 0 && data) {
        xhci_trb_t *data_trb = xhci_ring_enqueue(ring);
        if (!data_trb) return -1;
        data_trb->parameter = (uint64_t)data;  // Assumes physical address
        data_trb->status = length;
        uint32_t dir = (request_type & 0x80) ? TRB_DIR_IN : 0;
        data_trb->control = XHCI_TRB_TYPE(TRB_DATA) | dir | ring->cycle_bit;
    }

    // Status TRB
    xhci_trb_t *status = xhci_ring_enqueue(ring);
    if (!status) return -1;
    status->parameter = 0;
    status->status = 0;
    uint32_t status_dir = (length > 0 && (request_type & 0x80)) ? 0 : TRB_DIR_IN;
    status->control = XHCI_TRB_TYPE(TRB_STATUS) | TRB_IOC | status_dir | ring->cycle_bit;

    // #348: clear the EP0 (DCI 1) completion slot BEFORE ringing so a concurrent
    // poller draining the ring records THIS completion for us (race-free).
    g_xfer_cc[slot_id - 1][1] = 0;

    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");

    // Ring doorbell
    xhci_ring_doorbell(xhc, slot_id, 1);  // EP0 = doorbell target 1

    // Wait for completion (race-free, keyed by slot + DCI 1), bounded timeout.
    // #62: a FAILED control transfer must not be left behind. See
    // xhci_recover_control_endpoint() for why returning here without cleaning
    // up made one quiet device cost 5 seconds PER SUBSEQUENT REQUEST forever.
    int cc = xhci_wait_transfer(xhc, slot_id, 1, timeout_ms);
#ifndef XHCI_EPTEST_NORECOVER
    if (cc < 0) xhci_recover_control_endpoint(xhc, slot_id, cc);
#endif
    return cc;
}

// Public control transfer: standard device enumeration, generous 5s timeout.
int xhci_control_transfer(xhci_controller_t *xhc, int slot_id,
                          uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length) {
    return xhci_control_transfer_to(xhc, slot_id, request_type, request,
                                    value, index, data, length, 5000);
}

int xhci_bulk_transfer(xhci_controller_t *xhc, int slot_id, int endpoint,
                       void *data, uint32_t length, int direction) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) {
        return -1;
    }

    // Device Context Index (DCI): EP N, direction d (IN=1) -> DCI = 2*N + d.
    // The transfer ring is stored at [DCI] and the doorbell target IS the DCI.
    int dci = (endpoint * 2) + (direction ? 1 : 0);
    if (dci < 1 || dci >= XHCI_MAX_ENDPOINTS) {
        return -1;
    }

    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][dci];
    if (!ring) {
        return -1;
    }

    // Normal TRB for bulk transfer. TRB_ISP so a short packet still completes
    // (SCSI data-in phases are frequently shorter than the requested length).
    xhci_trb_t *trb = xhci_ring_enqueue(ring);
    if (!trb) return -1;
    trb->parameter = (uint64_t)data;
    trb->status = length;
    trb->control = XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | ring->cycle_bit;

    // #348: clear the per-(slot,DCI) completion BEFORE ringing so whoever drains
    // the event ring (this waiter OR a concurrent HID/UAC poller) records THIS
    // completion into g_xfer_cc for us. This is the fix for the HID+MSC coexist
    // stall: the MSC completion can no longer be stolen by the HID poller.
    g_xfer_cc[slot_id - 1][dci] = 0;

    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");

    // Ring doorbell (target == DCI)
    xhci_ring_doorbell(xhc, slot_id, dci);

    // Wait for completion (race-free, keyed by slot + DCI)
    return xhci_wait_transfer(xhc, slot_id, dci, 5000);
}

int xhci_interrupt_transfer(xhci_controller_t *xhc, int slot_id, int endpoint,
                            void *data, uint32_t length) {
    // Same as bulk transfer but for interrupt endpoints
    return xhci_bulk_transfer(xhc, slot_id, endpoint, data, length, 1);
}

// =============================================================================
// USB Standard Requests
// =============================================================================

int xhci_get_device_descriptor(xhci_controller_t *xhc, int slot_id, void *buf, int len) {
    return xhci_control_transfer(xhc, slot_id,
        0x80,       // bmRequestType: Device to Host, Standard, Device
        0x06,       // bRequest: GET_DESCRIPTOR
        0x0100,     // wValue: Device descriptor, index 0
        0x0000,     // wIndex: 0
        buf, len);
}

int xhci_set_address(xhci_controller_t *xhc, int slot_id, uint8_t address) {
    // xHCI handles addressing automatically via Address Device command
    // This is mainly for compatibility
    (void)address;
    return xhci_address_device(xhc, slot_id, 0, XHCI_SPEED_HIGH);
}

int xhci_set_configuration(xhci_controller_t *xhc, int slot_id, uint8_t config) {
    return xhci_control_transfer(xhc, slot_id,
        0x00,       // bmRequestType: Host to Device, Standard, Device
        0x09,       // bRequest: SET_CONFIGURATION
        config,     // wValue: Configuration value
        0x0000,     // wIndex: 0
        NULL, 0);
}

// =============================================================================
// Device Enumeration
// =============================================================================

// #325 Device Manager: enumerated-device record table (see xhci.h).
#define XHCI_ENUM_MAX 64
static xhci_enum_dev_t g_xhci_enum[XHCI_ENUM_MAX];
static int g_xhci_enum_count = 0;
int xhci_get_enum_count(void) { return g_xhci_enum_count; }
const xhci_enum_dev_t *xhci_get_enum_device(int index) {
    if (index < 0 || index >= g_xhci_enum_count) return 0;
    return &g_xhci_enum[index];
}

// #388 DEVLOG: hub inventory table (see xhci.h). Populated by xhci_enumerate_hub.
#define XHCI_HUB_MAX 8
static xhci_hub_rec_t g_xhci_hub[XHCI_HUB_MAX];
static int g_xhci_hub_count = 0;
int xhci_get_hub_count(void) { return g_xhci_hub_count; }
const xhci_hub_rec_t *xhci_get_hub_record(int index) {
    if (index < 0 || index >= g_xhci_hub_count) return 0;
    return &g_xhci_hub[index];
}

// #388 DEVLOG: read-only root-hub port status snapshot for the inventory.
int xhci_root_port_info(xhci_controller_t *xhc, int port,
                        int *connected, int *enabled, int *speed) {
    if (!xhc || port < 0 || port >= (int)xhc->max_ports) return 0;
    uint32_t portsc = xhci_portsc_read(xhc, port);
    if (connected) *connected = (portsc & XHCI_PORTSC_CCS) ? 1 : 0;
    if (enabled)   *enabled   = (portsc & XHCI_PORTSC_PED) ? 1 : 0;
    if (speed)     *speed     = (int)((portsc >> 10) & 0xF);   // Port Speed field
    return 1;
}

// #307: class-driver attach. Parses the (already fetched) configuration
// descriptor, and for each HID (class 3) or Mass-Storage (class 8) interface
// finds the relevant endpoints, issues SET_CONFIGURATION, configures those
// endpoints on the controller, and hands off to the class driver. Audio is
// handled separately by uac_probe (it manages its own config + iso EP).
extern int usb_hid_attach(xhci_controller_t *xhc, int slot_id, int iface_num,
                          int ep_in, int ep_in_mps, int b_interval, int speed,
                          uint8_t subclass, uint8_t protocol, int rd_len);
extern int usb_msc_enumerate(xhci_controller_t *xhc, int slot_id, int interface_num,
                             int bulk_in_ep, int bulk_out_ep,
                             int max_packet_in, int max_packet_out);
// #390 COMPOSITE-HID re-arm: re-submit the interrupt-IN TD for every already-armed
// HID endpoint on a slot (except exclude_dci) after a Configure Endpoint command
// re-asserted them. Defined in usb_hid.c.
extern int usb_hid_rearm_slot(xhci_controller_t *xhc, int slot_id, int exclude_dci);
// #362: USB Ethernet probe (CDC-ECM class driver or ASIX vendor driver).
extern int usb_net_probe(xhci_controller_t *xhc, int slot_id, int speed,
                         uint16_t vid, uint16_t pid,
                         uint8_t *cfg, int cfg_total, uint8_t num_configs);

// #433 (re-scoped) /USBLOG.TXT descriptor dump. Emit the full descriptor picture
// for ONE enumerated device: VID:PID, device class, bNumConfigurations, and for
// the chosen config every interface (bInterfaceNumber/alt/class/subclass/proto)
// and every endpoint (bEndpointAddress, transfer type, wMaxPacketSize,
// bInterval). The runtime enumeration decisions (SET_PROTOCOL/SET_IDLE sent +
// result, Configure-Endpoint result, which interface bound) are appended by the
// class-driver path below and in usb_hid.c, so /USBLOG.TXT is a single readable
// account of what the kernel saw AND did for each device. This is the file that
// makes the iMac "keyboard works in no port" failure diagnosable over SSH.
static const char *usblog_ep_type(uint8_t attr) {
    switch (attr & 0x03) {
        case 0: return "control";
        case 1: return "isoch";
        case 2: return "bulk";
        case 3: return "interrupt";
    }
    return "?";
}

void xhci_usblog_device(int slot_id, int speed, uint16_t vid, uint16_t pid,
                        const uint8_t *dev_desc, const uint8_t *cfg, int total,
                        int cfg_ok) {
    usblog_write("==== USB device: %04x:%04x slot %d speed %s ====",
                 vid, pid, slot_id, xhci_speed_name(speed));
    usblog_write("  DEVICE: bDeviceClass=0x%02x bDeviceSubClass=0x%02x "
                 "bDeviceProtocol=0x%02x bNumConfigurations=%d bMaxPacketSize0=%d",
                 dev_desc[4], dev_desc[5], dev_desc[6], dev_desc[17], dev_desc[7]);
    if (!cfg_ok || total < 9) {
        usblog_write("  CONFIG: (no configuration descriptor captured)");
        return;
    }
    usblog_write("  CONFIG: bNumInterfaces=%d wTotalLength=%d bmAttributes=0x%02x "
                 "bMaxPower=%dmA", cfg[4], total, cfg[7], cfg[8] * 2);
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {          // INTERFACE
            int inum = cfg[i + 2], alt = cfg[i + 3], neps = cfg[i + 4];
            int icls = cfg[i + 5], isub = cfg[i + 6], iproto = cfg[i + 7];
            const char *tag = "";
            if (icls == 0x03) {
                tag = (iproto == 1) ? " (HID boot-keyboard)" :
                      (iproto == 2) ? " (HID boot-mouse)"    :
                      (isub == 1)   ? " (HID boot, other)"   : " (HID)";
            }
            usblog_write("  IFACE %d alt %d: bInterfaceClass=0x%02x "
                         "bInterfaceSubClass=0x%02x bInterfaceProtocol=0x%02x nEP=%d%s",
                         inum, alt, icls, isub, iproto, neps, tag);
        } else if (btype == 0x05 && blen >= 7) {   // ENDPOINT
            int eaddr = cfg[i + 2], eattr = cfg[i + 3];
            int emps = cfg[i + 4] | (cfg[i + 5] << 8);
            int eintv = cfg[i + 6];
            usblog_write("    EP 0x%02x %s %s wMaxPacketSize=%d bInterval=%d",
                         eaddr, (eaddr & 0x80) ? "IN " : "OUT",
                         usblog_ep_type(eattr), emps, eintv);
        } else if (btype == 0x21) {                // HID descriptor
            // #162: report the wDescriptorLength too. "HID class descriptor,
            // 9 bytes" said nothing useful; the interesting number is how big
            // the REPORT descriptor is, because that is the one we fetch.
            int rl = (blen >= 9 && cfg[i + 6] == 0x22)
                         ? (cfg[i + 7] | (cfg[i + 8] << 8)) : 0;
            usblog_write("    (HID class descriptor, %d bytes; report descriptor "
                         "%d bytes)", blen, rl);
        }
        i += blen;
    }
}

static void xhci_attach_class_drivers(xhci_controller_t *xhc, int slot_id,
                                      int speed, uint8_t *cfg, int total) {
    int cfg_value = (total >= 6) ? cfg[5] : 1;
    int did_set_config = 0;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;

        if (btype == 0x04) {   // INTERFACE descriptor
            int iface_num = cfg[i + 2];
            int iface_alt = cfg[i + 3];
            int iclass    = cfg[i + 5];
            int isub      = cfg[i + 6];
            int iproto    = cfg[i + 7];

            // #433 (re-scoped) Part B: only bind the DEFAULT alternate setting
            // (bAlternateSetting == 0). A composite HID device may advertise
            // extra alt settings for the same interface; acting on each would
            // configure the same endpoint twice / attach the same HID twice and
            // double-submit interrupt-IN TDs, which orphans/corrupts the
            // endpoint and is a plausible "keyboard enumerates but never types".
            // usb_hid_attach also dedupes by (slot,DCI) as a second guard.
            if (iface_alt != 0) {
                usblog_write("  slot %d iface %d alt %d: skipped (non-default alt)",
                             slot_id, iface_num, iface_alt);
                i += blen;
                continue;
            }

            // Walk this interface's endpoint descriptors (up to the next
            // interface descriptor).
            int ep_int_in = -1, ep_int_in_mps = 0, ep_int_interval = 0;
            int ep_bulk_in = -1, ep_bulk_in_mps = 0;
            int ep_bulk_out = -1, ep_bulk_out_mps = 0;
            // #162: wDescriptorLength of this interface's REPORT descriptor,
            // taken from its HID class descriptor (bDescriptorType 0x21). This
            // walk already SAW that descriptor and logged only its length in
            // bytes; the field inside it is what lets us ask the device for the
            // report descriptor, which is the only trustworthy statement of
            // what an interface actually sends. 0 = no HID class descriptor.
            int hid_rd_len = 0;
            int j = i + blen;
            while (j + 2 <= total) {
                int elen = cfg[j];
                int etype = cfg[j + 1];
                if (elen < 2 || j + elen > total) break;
                if (etype == 0x04) break;              // next interface
                if (etype == 0x21 && elen >= 9) {      // #162: HID class descriptor
                    // bLength, bDescriptorType(0x21), bcdHID(2), bCountryCode,
                    // bNumDescriptors, bDescriptorType, wDescriptorLength(2).
                    // Only the FIRST subordinate descriptor is read, and only
                    // when it is type 0x22 (Report); an optional Physical
                    // descriptor is not something we ask for.
                    if (cfg[j + 6] == 0x22)
                        hid_rd_len = cfg[j + 7] | (cfg[j + 8] << 8);
                }
                if (etype == 0x05 && elen >= 7) {      // ENDPOINT
                    int eaddr = cfg[j + 2];
                    int eattr = cfg[j + 3] & 0x03;
                    int emps  = cfg[j + 4] | (cfg[j + 5] << 8);
                    int eintv = cfg[j + 6];
                    if (eattr == 0x03) {               // interrupt
                        if (eaddr & 0x80) {
                            ep_int_in = eaddr; ep_int_in_mps = emps;
                            ep_int_interval = eintv;
                        }
                    } else if (eattr == 0x02) {        // bulk
                        if (eaddr & 0x80) { ep_bulk_in = eaddr; ep_bulk_in_mps = emps; }
                        else              { ep_bulk_out = eaddr; ep_bulk_out_mps = emps; }
                    }
                }
                j += elen;
            }

            if (iclass == 0x03) {   // HID
                // #433 (re-scoped) Part B: log which HID interface we are about
                // to bind and its role, so /USBLOG.TXT shows the boot-keyboard
                // interface being selected on its own merits (class 3, sub, proto)
                // rather than a hardcoded "interface 0". Every HID interface with
                // an interrupt-IN endpoint is bound independently, so a composite
                // keyboard whose boot-keyboard interface is NOT interface 0 still
                // binds.
                const char *role = (iproto == 1) ? "boot-keyboard" :
                                   (iproto == 2) ? "boot-mouse" : "generic-HID";
                usblog_write("  slot %d iface %d: HID sub 0x%02x proto 0x%02x (%s), "
                             "int-IN EP 0x%02x mps %d interval %d",
                             slot_id, iface_num, isub, iproto, role,
                             ep_int_in, ep_int_in_mps, ep_int_interval);
                if (ep_int_in < 0) {
                    kprintf("[xHCI]   HID interface %d has no interrupt-IN EP\n", iface_num);
                    usblog_write("  slot %d iface %d: NO interrupt-IN endpoint - "
                                 "cannot bind this HID interface", slot_id, iface_num);
                } else {
                    if (!did_set_config) {
                        xhci_set_configuration(xhc, slot_id, cfg_value);
                        did_set_config = 1;
                    }
                    // #433: the interrupt-IN endpoint config MUST succeed or the
                    // HID delivers no key/mouse events. The old code IGNORED the
                    // return of xhci_configure_endpoint_ep and attached anyway,
                    // producing the "keyboard enumerates but never types" device
                    // seen on the iMac when CONFIG_EP raced. Check the return and
                    // retry the config a few times; only attach on success, never
                    // attach a dead endpoint.
                    int cfg_dci = -1;
                    for (int attempt = 1; attempt <= 3; attempt++) {
                        cfg_dci = xhci_configure_endpoint_ep(xhc, slot_id, ep_int_in,
                                                   EP_TYPE_INTERRUPT_IN, ep_int_in_mps,
                                                   ep_int_interval, speed);
                        bootlog_write("[xHCI] slot %d HID iface %d CONFIG_EP ep 0x%02x "
                                      "attempt %d/3: %s", slot_id, iface_num, ep_int_in,
                                      attempt, cfg_dci >= 0 ? "OK" : "FAILED");
                        if (cfg_dci >= 0) break;
                        xhci_delay(10 * attempt);
                    }
                    usblog_write("  slot %d iface %d: CONFIG_EP(ep 0x%02x) -> %s (DCI %d)",
                                 slot_id, iface_num, ep_int_in,
                                 cfg_dci >= 0 ? "OK" : "FAILED", cfg_dci);
                    if (cfg_dci >= 0) {
                        // #135: CHECK THE RETURN. This call ignored it, so every
                        // reason usb_hid_attach() can refuse a bind was invisible
                        // at the call site; the owner's capture shows slot 9
                        // logging CONFIG_EP OK for both interfaces and then simply
                        // nothing, with input dead and no diagnostic. A refusal is
                        // now stated here as well, on the PERSISTENT log, whatever
                        // the reason inside the HID layer turns out to be.
                        int hid_rc = usb_hid_attach(xhc, slot_id, iface_num, ep_int_in,
                                       ep_int_in_mps, ep_int_interval, speed,
                                       (uint8_t)isub, (uint8_t)iproto, hid_rd_len);
                        if (hid_rc < 0) {
                            kprintf("[xHCI]   slot %d HID iface %d: usb_hid_attach "
                                    "REFUSED (rc=%d)\n", slot_id, iface_num, hid_rc);
                            bootlog_write("[xHCI] *** slot %d HID iface %d: "
                                          "usb_hid_attach REFUSED the bind (rc=%d) - "
                                          "THIS INPUT DEVICE WILL NOT WORK ***",
                                          slot_id, iface_num, hid_rc);
                        }
                    } else {
                        kprintf("[xHCI]   slot %d HID iface %d: interrupt-IN EP config "
                                "FAILED after retries; NOT attaching dead endpoint\n",
                                slot_id, iface_num);
                        bootlog_write("[xHCI] slot %d HID iface %d: interrupt-IN EP config "
                                      "FAILED after retries; HID not attached (re-scan "
                                      "will retry on replug)", slot_id, iface_num);
                    }
                }
            } else if (iclass == 0x08) {   // Mass Storage
                if (ep_bulk_in < 0 || ep_bulk_out < 0) {
                    kprintf("[xHCI]   MSC interface %d missing bulk EPs (in %d out %d)\n",
                            iface_num, ep_bulk_in, ep_bulk_out);
                } else {
                    if (!did_set_config) {
                        xhci_set_configuration(xhc, slot_id, cfg_value);
                        did_set_config = 1;
                    }
                    xhci_configure_endpoint_ep(xhc, slot_id, ep_bulk_in,
                                               EP_TYPE_BULK_IN, ep_bulk_in_mps, 0, speed);
                    xhci_configure_endpoint_ep(xhc, slot_id, ep_bulk_out,
                                               EP_TYPE_BULK_OUT, ep_bulk_out_mps, 0, speed);
                    usb_msc_enumerate(xhc, slot_id, iface_num,
                                      ep_bulk_in & 0x0F, ep_bulk_out & 0x0F,
                                      ep_bulk_in_mps, ep_bulk_out_mps);
                }
            }
        }
        i += blen;
    }
}

// =============================================================================
// #373 USB hub support
// =============================================================================
//
// The real iMac14,4 Apple keyboard is a USB HUB (an integrated keyboard plus
// downstream ports for the mouse), and our driver previously only enumerated
// devices on ROOT ports, so the keyboard/mouse never appeared (the USB stick
// worked because it sits on a root port). This block drives a device whose
// bDeviceClass is 0x09 (Hub) as a hub: read the hub descriptor for the port
// count, power each downstream port, poll for connect, reset connected ports,
// and enumerate the resulting devices via the normal path but with the correct
// slot-context Route String + Transaction Translator fields for a device behind
// a hub. Every step logs "[USB] <label> ..." to the boot splash + /BOOTLOG.TXT
// so a single iMac photo (or the log file) shows exactly what enumerated.

// USB hub class-specific requests / features (USB 2.0 spec 11.24).
#define USB_DT_HUB              0x29
#define HUB_FEAT_PORT_CONNECTION  0
#define HUB_FEAT_PORT_ENABLE      1
#define HUB_FEAT_PORT_RESET       4
#define HUB_FEAT_PORT_POWER       8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_ENABLE     17
#define HUB_FEAT_C_PORT_RESET      20
// wPortStatus bits (low 16 of GET_STATUS on a hub port).
#define HUB_PS_CONNECTION  (1 << 0)
#define HUB_PS_ENABLE      (1 << 1)
#define HUB_PS_RESET       (1 << 4)
#define HUB_PS_POWER       (1 << 8)
#define HUB_PS_LOW_SPEED   (1 << 9)
#define HUB_PS_HIGH_SPEED  (1 << 10)
// wPortChange bits (high 16 of GET_STATUS on a hub port).
#define HUB_PC_CONNECTION  (1 << 0)
#define HUB_PC_ENABLE      (1 << 1)
#define HUB_PC_RESET       (1 << 4)

// #373 real-HW: HARD bounded timeout for EVERY hub class control request. A
// healthy hub answers a control transfer in microseconds; 300ms is hugely
// generous (so it NEVER fires spuriously on QEMU's fast usb-hub) yet small
// enough that a non-responding real hub cannot stall boot: the enumerate_hub
// loops abort the whole hub after the FIRST such timeout, so total worst-case
// added boot time is a handful of these, ~1s, then boot continues to desktop.
#define HUB_CTRL_TIMEOUT_MS 300

// xhci_control_transfer_to (defined above) returns -1 uniquely on timeout (no
// completion event within the budget). enumerate_hub uses that to decide "abort
// this hub, keep booting".

// #373: single place to log a hub op that timed out / failed and that causes us
// to skip the remaining downstream work for this hub. Emits BOTH the on-screen
// splash line (so a real-iMac photo shows the exact culprit) and the disk
// /BOOTLOG.TXT line (now flushed because boot no longer hangs).
static void hub_bail_log(const char *hub_label, const char *op, int port) {
    extern void gfx_boot_log(const char *message);
    char el[96];
    if (port > 0)
        snprintf(el, sizeof(el), "[USB] %s.p%d %s TIMEOUT (skip hub)", hub_label, port, op);
    else
        snprintf(el, sizeof(el), "[USB] %s %s TIMEOUT (skip hub)", hub_label, op);
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
    bootlog_write("[xHCI] %s port %d: %s TIMEOUT - hub downstream skipped, boot continues",
                  hub_label, port, op);
}

static int hub_get_descriptor(xhci_controller_t *xhc, int slot, void *buf, int len) {
    return xhci_control_transfer_to(xhc, slot, 0xA0, 0x06 /*GET_DESCRIPTOR*/,
                                 (USB_DT_HUB << 8) | 0, 0, buf, len,
                                 HUB_CTRL_TIMEOUT_MS);
}
static int hub_set_port_feature(xhci_controller_t *xhc, int slot, int feat, int port) {
    return xhci_control_transfer_to(xhc, slot, 0x23, 0x03 /*SET_FEATURE*/,
                                 feat, port, NULL, 0, HUB_CTRL_TIMEOUT_MS);
}
static int hub_clear_port_feature(xhci_controller_t *xhc, int slot, int feat, int port) {
    return xhci_control_transfer_to(xhc, slot, 0x23, 0x01 /*CLEAR_FEATURE*/,
                                 feat, port, NULL, 0, HUB_CTRL_TIMEOUT_MS);
}
// #373 real-HW: returns the FULL 32-bit hub port status: wPortStatus in the low
// 16 bits and wPortChange in the high 16 bits. *ok is set to 0 if the GET_STATUS
// control transfer itself failed (so the caller can distinguish "port really
// reports 0x0000" from "the hub did not answer"). This is what the granular
// per-downstream-port diagnostics are built on.
static uint32_t hub_get_port_status_full(xhci_controller_t *xhc, int slot,
                                         int port, int *ok, int *timed_out) {
    static uint8_t sbuf[4] __attribute__((aligned(64)));
    memset(sbuf, 0, sizeof(sbuf));
    if (timed_out) *timed_out = 0;
    int cc = xhci_control_transfer_to(xhc, slot, 0xA3, 0x00 /*GET_STATUS*/,
                                   0, port, sbuf, 4, HUB_CTRL_TIMEOUT_MS);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        if (ok) *ok = 0;
        // cc == -1 uniquely means the transfer never completed (the real hub
        // did not answer). Surface that so the caller can abort the hub instead
        // of hammering the same non-responding request hundreds of times.
        if (timed_out) *timed_out = (cc == -1);
        return 0;
    }
    if (ok) *ok = 1;
    return (uint32_t)sbuf[0] | ((uint32_t)sbuf[1] << 8) |
           ((uint32_t)sbuf[2] << 16) | ((uint32_t)sbuf[3] << 24);
}

// #373: promote a slot to "hub" in its slot context (Hub bit, Number of Ports,
// TT Think Time, Multi-TT). Address Device for a device attached BEHIND this hub
// requires the Hub bit + Number of Ports to already be set here, so this runs
// right after the hub is enumerated and before its downstream ports are probed.
static int xhci_configure_hub_slot(xhci_controller_t *xhc, int slot_id,
                                   int num_ports, int ttt, int mtt) {
    size_t input_size = ALIGN_UP(xhc->context_size * 33, PAGE_SIZE);
    uint64_t input_phys = pmm_alloc_pages(input_size / PAGE_SIZE);
    if (input_phys == 0) return -1;
    xhci_input_ctx_t *input = (xhci_input_ctx_t *)input_phys;
    memset(input, 0, input_size);

    input->ctrl.add_flags = (1u << 0);   // update the slot context only
    input->ctrl.drop_flags = 0;

    xhci_slot_ctx_t *in_slot = (xhci_slot_ctx_t *)((uint8_t *)input + xhc->context_size);
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)((uint8_t *)xhc->dev_ctx[slot_id - 1]);
    memcpy(in_slot, out_slot, sizeof(xhci_slot_ctx_t));
    in_slot->hub = 1;
    in_slot->num_ports = num_ports & 0xFF;
    in_slot->ttt = ttt & 0x3;
    in_slot->mtt = mtt ? 1 : 0;

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = input_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_CONFIG_EP) | ((slot_id & 0xFF) << 24);
    int result = xhci_send_command(xhc, &cmd);
    pmm_free_pages(input_phys, input_size / PAGE_SIZE);
    // #373 real-HW diagnostic: surface the Configure-Endpoint completion code
    // either way. On the iMac, EP0 worked for the hub descriptor read but the
    // FIRST downstream control request right after this command timed out, so we
    // need to see whether promoting the slot to a hub actually succeeded (a bad
    // input context here could wedge EP0). g_xhci_last_cmd_timeout distinguishes
    // "no completion event" from a real non-success completion code.
    bootlog_write("[xHCI] Configure hub slot %d: Configure-Endpoint %s (r=%d timeout=%d cc=%d)",
                  slot_id, result >= 0 ? "OK" : "FAILED", result,
                  g_xhci_last_cmd_timeout, g_xhci_last_cmd_cc);
    if (result < 0) {
        kprintf("[xHCI] Configure hub slot %d failed cc=%d\n", slot_id, result);
        return -1;
    }
    return 0;
}

// Mutually recursive with xhci_enumerate_hub (a hub can hang off a hub).
static void xhci_probe_device(xhci_controller_t *xhc, int slot_id, int speed,
                              int root_port, uint32_t route_string, int depth,
                              int tt_slot, int tt_port, const char *label);
static void xhci_enumerate_hub(xhci_controller_t *xhc, int hub_slot, int hub_speed,
                               int root_port, uint32_t hub_route, int hub_depth,
                               int hub_tt_slot, int hub_tt_port, const char *hub_label);

// Read descriptors of an already-addressed slot, hand its interfaces to the
// class drivers, record it for the Device Manager, and - if it is a hub -
// enumerate everything behind it. Shared by root-port and behind-hub devices.
static void xhci_probe_device(xhci_controller_t *xhc, int slot_id, int speed,
                              int root_port, uint32_t route_string, int depth,
                              int tt_slot, int tt_port, const char *label) {
    extern void gfx_boot_log(const char *message);
    int idx = xhci_ctrl_index(xhc);
    char el[96];

    // Device descriptor: first 8 bytes (to learn bMaxPacketSize0), then full 18.
    static uint8_t desc[18] __attribute__((aligned(64)));
    memset(desc, 0, sizeof(desc));
    if (xhci_get_device_descriptor(xhc, slot_id, desc, 8) < 0) {
        snprintf(el, sizeof(el), "[USB] %s enum: slot=%d desc read FAILED",
                 label, slot_id);
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] %s slot %d: GET_DESCRIPTOR(8) FAILED", label, slot_id);
        xhci_disable_slot(xhc, slot_id);
        return;
    }

    uint8_t mps0 = desc[7];
    // Full-speed control endpoints may legally be 8/16/32/64 (low-speed is
    // always 8); only trust the probe if it reports one of those legal values.
    if (speed == XHCI_SPEED_FULL && mps0 != 8 &&
        (mps0 == 16 || mps0 == 32 || mps0 == 64)) {
        xhci_evaluate_ep0_mps(xhc, slot_id, mps0);
    }

    // Full 18-byte device descriptor, using the (possibly corrected) EP0 mps.
    int desc_valid = 0;
    int cc18 = -1;
    for (int attempt = 0; attempt < 3 && !desc_valid; attempt++) {
        if (attempt > 0) xhci_delay(20);
        memset(desc, 0, sizeof(desc));
        cc18 = xhci_get_device_descriptor(xhc, slot_id, desc, 18);
        if ((cc18 == CC_SUCCESS || cc18 == CC_SHORT_PACKET) &&
            desc[0] == 18 && desc[1] == 0x01) {
            desc_valid = 1;
        }
    }
    if (!desc_valid) {
        kprintf("[xHCI] GET_DESCRIPTOR (18) on %s malformed "
                "(cc=%d bLength=%u bType=%u); using it anyway\n",
                label, cc18, desc[0], desc[1]);
    }

    uint16_t vid = desc[8] | (desc[9] << 8);
    uint16_t pid = desc[10] | (desc[11] << 8);
    kprintf("[xHCI] Device %04x:%04x class 0x%02x sub 0x%02x proto 0x%02x mps0 %u\n",
            vid, pid, desc[4], desc[5], desc[6], mps0);
    bootlog_write("[xHCI] %s slot %d: %04x:%04x class 0x%02x sub 0x%02x proto 0x%02x speed %s mps0 %u",
                  label, slot_id, vid, pid, desc[4], desc[5], desc[6],
                  xhci_speed_name(speed), mps0);

    // #135: SAME-DEVICE RECOGNITION across a root-port flap. The captured iMac
    // log re-enumerated one keyboard into slots 3, 7, 8 and 9 and never once said
    // it was the same physical device; you had to notice the repeated 05ac:024f
    // by eye. Say it, and say what it cost.
    {
        int rp = (slot_id >= 1 && slot_id <= XHCI_MAX_SLOTS)
                     ? (int)g_slot_root_port[slot_id - 1] : 0;
        if (rp >= 1 && rp <= 256) {
            int ci = xhci_ctrl_index(xhc);
            uint32_t id = ((uint32_t)vid << 16) | (uint32_t)pid;
            if (g_port_flaps[ci][rp - 1] &&
                g_port_last_devid[ci][rp - 1] == id) {
                bootlog_write("[PORTFLAP] port %d: the SAME device %04x:%04x is back "
                              "after flap #%u - it was never unplugged; slot %d and a "
                              "fresh set of HID entries have been spent on it",
                              rp, vid, pid, g_port_flaps[ci][rp - 1], slot_id);
            }
            g_port_last_devid[ci][rp - 1] = id;
        }
    }

    // Read the configuration descriptor: 9 bytes for wTotalLength, then full.
    static uint8_t cfg[512] __attribute__((aligned(64)));
    char kinds[40];
    int  kn = 0;
    kinds[0] = 0;
    int cfg_ok = 0;
    int total = 0;
    int is_hub = (desc[4] == 0x09);   // #373: bDeviceClass == Hub
    memset(cfg, 0, sizeof(cfg));
    int cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                   (0x02 << 8) | 0, 0, cfg, 9);
    if ((cc == CC_SUCCESS || cc == CC_SHORT_PACKET) && cfg[1] != 0x02 &&
        (speed == XHCI_SPEED_FULL || speed == XHCI_SPEED_LOW)) {
        xhci_delay(20);
        memset(cfg, 0, sizeof(cfg));
        cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                   (0x02 << 8) | 0, 0, cfg, 9);
    }
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        total = cfg[2] | (cfg[3] << 8);
        if (total > (int)sizeof(cfg)) total = sizeof(cfg);
        if (total < 9) total = 9;
        memset(cfg, 0, sizeof(cfg));
        cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                   (0x02 << 8) | 0, 0, cfg, total);
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            cfg_ok = 1;
            int has_audio = 0;
            int i = 0;
            while (i + 2 <= total) {
                int blen = cfg[i];
                int btype = cfg[i + 1];
                if (blen < 2 || i + blen > total) break;
                if (btype == 0x04) {            // INTERFACE descriptor
                    int iclass = cfg[i + 5];
                    int iproto = cfg[i + 7];
                    kprintf("[xHCI]   interface %u alt %u class 0x%02x sub 0x%02x\n",
                            cfg[i + 2], cfg[i + 3], iclass, cfg[i + 6]);
                    const char *tag = 0;
                    if (iclass == 0x01)      { has_audio = 1; tag = "Audio"; }
                    else if (iclass == 0x03) { tag = (iproto == 1) ? "kbd" :
                                                     (iproto == 2) ? "mouse" : "HID"; }
                    else if (iclass == 0x08) { tag = "MSC"; }
                    else if (iclass == 0x09) { is_hub = 1; tag = "HUB"; }
                    else if (iclass == 0x02 || iclass == 0x0A ||
                             iclass == 0xE0) { tag = "Net"; }
                    if (tag && kn < (int)sizeof(kinds) - 8) {
                        kn += snprintf(kinds + kn, sizeof(kinds) - kn,
                                       "%s%s", kn ? "," : "", tag);
                    }
                }
                i += blen;
            }
            // #433 (re-scoped): dump this device's full descriptor tree to
            // /USBLOG.TXT BEFORE the class drivers act, so the keyboard's real
            // interfaces/endpoints are on disk (and read first) even if a later
            // CONFIG_EP/attach step fails or hangs. The class drivers append
            // their runtime SET_PROTOCOL/CONFIG_EP results right after.
            xhci_usblog_device(slot_id, speed, vid, pid, desc, cfg, total, cfg_ok);

            // Hubs are handled below (xhci_enumerate_hub); do not hand a hub's
            // interfaces to the HID/MSC/Audio/Net class drivers.
            if (!is_hub) {
                if (has_audio) {
                    kprintf("[xHCI] Audio class device detected, handing to UAC driver\n");
                    uac_probe(xhc, slot_id, vid, pid, cfg, total);
                }
                xhci_attach_class_drivers(xhc, slot_id, speed, cfg, total);
                usb_net_probe(xhc, slot_id, speed, vid, pid, cfg, total, desc[17]);
                // #396: USB CDC-ACM serial (M3D Micro 03eb:2404 and any ACM device).
                extern int usb_cdc_acm_probe(xhci_controller_t *xhc, int slot_id, int speed,
                                             uint16_t vid, uint16_t pid, uint8_t *cfg, int total);
                usb_cdc_acm_probe(xhc, slot_id, speed, vid, pid, cfg, total);
                // #383: Realtek RTL88x2BU (0bda:b812) WiFi probe.
                extern int rtl8812bu_probe(xhci_controller_t *xhc, int slot_id, int speed,
                                           uint16_t vid, uint16_t pid, uint8_t *cfg, int total);
                rtl8812bu_probe(xhc, slot_id, speed, vid, pid, cfg, total);
                // #372: Bluetooth USB HCI transport probe (gated by g_bt_enable).
                extern int bt_usb_probe(xhci_controller_t *xhc, int slot_id, int speed,
                                        uint16_t vid, uint16_t pid, uint8_t dev_class,
                                        uint8_t dev_subclass, uint8_t dev_proto,
                                        uint8_t *cfg, int total);
                bt_usb_probe(xhc, slot_id, speed, vid, pid, desc[4], desc[5], desc[6], cfg, total);
            }
        }
    }
    if (!cfg_ok) {
        kprintf("[xHCI] GET config descriptor failed (cc=%d)\n", cc);
        // No config descriptor to walk, but still record the device so a
        // /USBLOG.TXT reader sees it appeared and why nothing bound.
        xhci_usblog_device(slot_id, speed, vid, pid, desc, cfg, 0, 0);
    }

    // #366/#307/#373: on-screen per-device success line.
    snprintf(el, sizeof(el), "[USB] %s enum: slot=%d %04x:%04x cls=%02x %s",
             label, slot_id, vid, pid, desc[4],
             cfg_ok ? (kinds[0] ? kinds : "ok") : "cfg FAILED");
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);

    // #325: record this device for the Device Manager syscalls.
    if (g_xhci_enum_count < XHCI_ENUM_MAX) {
        xhci_enum_dev_t *er = &g_xhci_enum[g_xhci_enum_count++];
        er->slot_id = slot_id;
        er->port = root_port;
        er->speed = speed;
        er->address = slot_id;
        er->vendor_id = vid;
        er->product_id = pid;
        er->dev_class = desc[4];
        er->dev_subclass = desc[5];
        er->dev_protocol = desc[6];
        er->num_interfaces = cfg_ok ? cfg[4] : 0;
        // #388 DEVLOG: capture the raw descriptors + topology for /DEVLOG.TXT.
        memcpy(er->dev_desc, desc, sizeof(er->dev_desc));
        int clen = cfg_ok ? total : 0;
        if (clen > (int)sizeof(er->cfg)) clen = sizeof(er->cfg);
        if (clen < 0) clen = 0;
        if (clen) memcpy(er->cfg, cfg, clen);
        er->cfg_len = (uint16_t)clen;
        er->route = route_string;
        er->depth = (uint8_t)depth;
        er->is_hub = (uint8_t)(is_hub ? 1 : 0);
        {
            int li = 0;
            while (label && label[li] && li < (int)sizeof(er->label) - 1) {
                er->label[li] = label[li]; li++;
            }
            er->label[li] = 0;
        }
    }

    g_enum_dev_found[idx]++;
    xhc->enabled_slots++;

    // #373: if this device is a hub, drive every device behind it. Limit the
    // depth so a self-referential / malformed hub can never recurse forever
    // (the 20-bit route string holds at most 5 tiers anyway).
    if (is_hub && depth < 5) {
        xhci_enumerate_hub(xhc, slot_id, speed, root_port, route_string, depth,
                           tt_slot, tt_port, label);
    }
}

// #373: enumerate every device behind an already-addressed hub. hub_depth is the
// hub's own tier (0 for a root-port hub); a downstream device on hub port p gets
// route string (hub_route | (p << (4*hub_depth))) and tier hub_depth+1.
static void xhci_enumerate_hub(xhci_controller_t *xhc, int hub_slot, int hub_speed,
                               int root_port, uint32_t hub_route, int hub_depth,
                               int hub_tt_slot, int hub_tt_port, const char *hub_label) {
    extern void gfx_boot_log(const char *message);
    char el[96];

    // Hub descriptor -> number of downstream ports + TT think time + power-good.
    static uint8_t hd[16] __attribute__((aligned(64)));
    memset(hd, 0, sizeof(hd));
    int cc = hub_get_descriptor(xhc, hub_slot, hd, sizeof(hd));
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        // cc == -1 is a bounded-timeout (real hub did not answer GET_DESCRIPTOR);
        // any other negative is a real error. Either way this hub is skipped and
        // boot continues (non-fatal), so a real Apple hub can no longer hang.
        if (cc == -1) {
            hub_bail_log(hub_label, "hub GET_DESCRIPTOR", 0);
        } else {
            snprintf(el, sizeof(el), "[USB] %s HUB desc FAILED cc=%d", hub_label, cc);
            gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
            bootlog_write("[xHCI] %s: hub descriptor FAILED cc=%d", hub_label, cc);
        }
        return;
    }
    int nports = hd[2];
    int wchar  = hd[3] | (hd[4] << 8);
    int ttt    = (wchar >> 5) & 0x3;
    int pgood  = hd[5];                 // bPwrOn2PwrGood, in 2ms units
    if (nports < 1) nports = 1;
    if (nports > 15) nports = 15;       // route-string nibble holds 1..15

    // #373 real-HW (the Apple-hub fix): decode wHubCharacteristics.
    //   bits 1:0 = Logical Power Switching Mode (LPSM):
    //     00 = ganged (one control powers all ports)
    //     01 = individual per-port power switching
    //     1x = no power switching (ports are ALWAYS powered)
    //   bit 2   = compound device (hub integrated into a larger device, e.g. the
    //             Apple keyboard+hub) - such devices almost never switch power.
    // The iMac's Apple hub does NOT answer SET_FEATURE(PORT_POWER) (times out),
    // which strongly implies "no / ganged power switching, ports pre-powered".
    // We now only issue per-port PORT_POWER for INDIVIDUAL mode, a single request
    // for GANGED, and NOTHING for no-switching - and the whole power step is
    // non-fatal (see below), so a hub whose ports are already powered still
    // enumerates its downstream devices.
    int psm       = wchar & 0x3;
    int compound  = (wchar >> 2) & 1;
    const char *psm_name = (psm == 0) ? "ganged" :
                           (psm == 1) ? "individual" : "none";

    // #388 DEVLOG: reserve a hub inventory record and fill its header now; the
    // per-downstream-port loop below records each port's status as it reads it.
    xhci_hub_rec_t *hrec = 0;
    if (g_xhci_hub_count < XHCI_HUB_MAX) {
        hrec = &g_xhci_hub[g_xhci_hub_count++];
        memset(hrec, 0, sizeof(*hrec));
        hrec->hub_slot  = hub_slot;
        hrec->root_port = root_port;
        hrec->route     = hub_route;
        hrec->depth     = hub_depth;
        hrec->nports    = nports;
        hrec->hubchar   = (uint16_t)wchar;
    }

    snprintf(el, sizeof(el), "[USB] %s enum: HUB %d ports", hub_label, nports);
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
    bootlog_write("[xHCI] %s: HUB %d ports ttt=%d pgood=%dms speed=%s",
                  hub_label, nports, ttt, pgood * 2, xhci_speed_name(hub_speed));
    snprintf(el, sizeof(el), "[USB] %s hubchar=0x%04x pwr-switch=%s%s",
             hub_label, (unsigned)wchar, psm_name, compound ? " compound" : "");
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
    bootlog_write("[xHCI] %s: wHubCharacteristics=0x%04x power-switching=%s compound=%d",
                  hub_label, (unsigned)wchar, psm_name, compound);

    // Promote the slot to a hub (Hub bit + Number of Ports) so the controller
    // will accept downstream Address Device commands routed through it. This is a
    // command-ring op (bounded at 5s by xhci_send_command); if it fails the
    // downstream Address Device commands would fail anyway, so skip the hub.
    if (xhci_configure_hub_slot(xhc, hub_slot, nports, ttt, 0) < 0) {
        hub_bail_log(hub_label, "configure hub slot", 0);
        return;
    }
    snprintf(el, sizeof(el), "[USB] %s configure-EP OK (hub bit set)", hub_label);
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);

    // Power downstream ports per the hub's power-switching mode, then honor
    // bPwrOn2PwrGood (2ms units) FULLY plus a margin before probing. This whole
    // step is NON-FATAL: on the iMac the Apple hub's ports are permanently
    // powered and it simply does not answer SET_FEATURE(PORT_POWER), so a
    // timeout here must NOT skip the hub - we log it and continue to read the
    // (already-powered) downstream port status. Only the status reads below are
    // treated as decisive.
    //   individual (01): power each port in turn.
    //   ganged     (00): a single request powers all ports.
    //   none       (1x): ports are always powered - issue nothing.
    int power_tmo = 0;
    if (psm == 1) {
        for (int p = 1; p <= nports; p++) {
            if (hub_set_port_feature(xhc, hub_slot, HUB_FEAT_PORT_POWER, p) == -1) {
                power_tmo = 1;
                bootlog_write("[xHCI] %s.p%d SET_FEATURE(PORT_POWER) TIMEOUT "
                              "(non-fatal, ports may be pre-powered)", hub_label, p);
                break;
            }
        }
    } else if (psm == 0) {
        if (hub_set_port_feature(xhc, hub_slot, HUB_FEAT_PORT_POWER, 1) == -1) {
            power_tmo = 1;
            bootlog_write("[xHCI] %s ganged SET_FEATURE(PORT_POWER) TIMEOUT "
                          "(non-fatal, ports may be pre-powered)", hub_label);
        }
    }
    snprintf(el, sizeof(el), "[USB] %s power(%s)%s", hub_label, psm_name,
             power_tmo ? " TIMEOUT-continue" :
             (psm >= 2 ? " skipped-alwayson" : " ok"));
    gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
    bootlog_write("[xHCI] %s: power step mode=%s result=%s", hub_label, psm_name,
                  power_tmo ? "TIMEOUT-continued" :
                  (psm >= 2 ? "skipped(always-on)" : "ok"));
    xhci_delay(pgood * 2 + 100);

    // #373 DECISIVE DIAGNOSTIC (the next-iMac deliverable): does EP0 still answer
    // AFTER Configure-Endpoint + the (mode-correct / skipped) power step? A
    // GET_STATUS on downstream port 1 is another EP0 control transfer. If it
    // SUCCEEDS, the earlier failure was PORT_POWER-only and the keyboard now
    // enumerates below. If it ALSO times out, EP0 is dead after Configure
    // Endpoint (deeper - chase configure_hub_slot / its input context next).
    // Either outcome is logged clearly; a timeout bails this hub non-fatally so
    // boot continues to the desktop.
    {
        int dok = 1, dtmo = 0;
        uint32_t dsc = hub_get_port_status_full(xhc, hub_slot, 1, &dok, &dtmo);
        if (dtmo) {
            snprintf(el, sizeof(el),
                     "[USB] %s EP0 DEAD after configure+power (GET_STATUS p1 TIMEOUT)",
                     hub_label);
            gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
            bootlog_write("[xHCI] %s: EP0 GET_STATUS(p1) TIMEOUT after Configure-Endpoint"
                          "+power step - PORT_POWER was not the (only) cause;"
                          " configure_hub_slot / input-context suspect", hub_label);
            hub_bail_log(hub_label, "get_port_status(post-power probe)", 1);
            return;
        }
        snprintf(el, sizeof(el),
                 "[USB] %s EP0 alive after power (GET_STATUS p1 st=%04x) -> enumerating",
                 hub_label, (unsigned)(dsc & 0xFFFF));
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] %s: EP0 GET_STATUS(p1)=0x%08x OK after power step -"
                      " downstream enumeration proceeds", hub_label, (unsigned)dsc);
    }

    for (int p = 1; p <= nports; p++) {
        // ---------------------------------------------------------------------
        // #307/#373 GRANULAR PER-DOWNSTREAM-PORT DIAGNOSTIC (the deliverable).
        // Read the raw wPortStatus + wPortChange and log them decoded for EVERY
        // port, connected or not, so a single iMac photo / boot log shows exactly
        // what each hub port reports. This is what distinguishes a
        // connect-detection/timing problem (conn=0 everywhere) from an
        // enumerate/TT problem (conn=1 but Address Device fails).
        // ---------------------------------------------------------------------
        int sok = 1, gs_tmo = 0;
        uint32_t sc = hub_get_port_status_full(xhc, hub_slot, p, &sok, &gs_tmo);
        if (gs_tmo) { hub_bail_log(hub_label, "get_port_status", p); return; }
        uint32_t status = sc & 0xFFFF;
        uint32_t change = (sc >> 16) & 0xFFFF;

        // CONNECT DEBOUNCE. QEMU's hub asserts PORT_CONNECTION instantly, but a
        // real High-Speed hub needs time after power-good before a downstream
        // device shows, and the connection must be stable (USB 2.0 spec: 100ms
        // debounce). Poll up to ~750ms for a connection that stays set for 100ms.
        // Each poll is a bounded GET_STATUS; a real hub that stops answering must
        // NOT be hammered 150 times at the transfer timeout each (that is the
        // #373 hang), so abort the hub the instant a poll times out.
        int connected = (status & HUB_PS_CONNECTION) ? 1 : 0;
        int stable = connected ? 20 : 0;   // steps of 5ms; 20*5 = 100ms stable
        for (int t = 0; t < 150 && stable < 20; t++) {   // 150*5 = 750ms cap
            xhci_delay(5);
            sc = hub_get_port_status_full(xhc, hub_slot, p, &sok, &gs_tmo);
            if (gs_tmo) { hub_bail_log(hub_label, "get_port_status(debounce)", p); return; }
            if (!sok) break;   // non-timeout transfer failure: stop polling this port
            status = sc & 0xFFFF;
            change = (sc >> 16) & 0xFFFF;
            if (status & HUB_PS_CONNECTION) { connected = 1; stable++; }
            else { connected = 0; stable = 0; }
        }

        const char *spn = (status & HUB_PS_LOW_SPEED)  ? "LS" :
                          (status & HUB_PS_HIGH_SPEED) ? "HS" : "FS";
        // #388 DEVLOG: record this downstream port's status (connected or not).
        if (hrec && p >= 1 && p <= 15) {
            hrec->ports[p - 1].status    = (uint16_t)status;
            hrec->ports[p - 1].change    = (uint16_t)change;
            hrec->ports[p - 1].connected = (uint8_t)connected;
            hrec->ports[p - 1].speed     = (uint8_t)((status & HUB_PS_LOW_SPEED)  ? XHCI_SPEED_LOW  :
                                                     (status & HUB_PS_HIGH_SPEED) ? XHCI_SPEED_HIGH :
                                                                                    XHCI_SPEED_FULL);
            hrec->ports[p - 1].valid     = 1;
        }
        snprintf(el, sizeof(el),
                 "[USB] %s.p%d st=%04x ch=%04x conn=%d en=%d pwr=%d spd=%s%s",
                 hub_label, p, status, change, connected,
                 (status & HUB_PS_ENABLE) ? 1 : 0,
                 (status & HUB_PS_POWER) ? 1 : 0, spn, sok ? "" : " (GS FAIL)");
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] %s.p%d status=%04x change=%04x conn=%d en=%d pwr=%d spd=%s%s",
                      hub_label, p, status, change, connected,
                      (status & HUB_PS_ENABLE) ? 1 : 0,
                      (status & HUB_PS_POWER) ? 1 : 0, spn, sok ? "" : " GS-FAIL");

        if (!connected) continue;   // empty port, already logged as conn=0

        // CLEAR CHANGE BITS. Per USB 2.0 spec the hub will not advance a port's
        // state machine while its change bits are set; ack connection (+ any
        // stale enable change) before driving the reset. Each is a bounded
        // control transfer; a real hub that stops answering aborts the hub.
        if (change & HUB_PC_CONNECTION)
            if (hub_clear_port_feature(xhc, hub_slot, HUB_FEAT_C_PORT_CONNECTION, p) == -1) {
                hub_bail_log(hub_label, "clear_feature C_CONNECTION", p); return; }
        if (change & HUB_PC_ENABLE)
            if (hub_clear_port_feature(xhc, hub_slot, HUB_FEAT_C_PORT_ENABLE, p) == -1) {
                hub_bail_log(hub_label, "clear_feature C_ENABLE", p); return; }
        // Connect debounce settle before reset (spec TATT_DB, 100ms).
        xhci_delay(100);

        // RESET SEQUENCING. Drive PORT_RESET, then poll until the hub clears
        // PORT_RESET and either sets C_PORT_RESET or PORT_ENABLE. On a real hub
        // reset completion is signalled by the C_PORT_RESET change bit; QEMU
        // asserts PORT_ENABLE. Accept either.
        if (hub_set_port_feature(xhc, hub_slot, HUB_FEAT_PORT_RESET, p) == -1) {
            hub_bail_log(hub_label, "set_feature PORT_RESET", p); return; }
        int reset_done = 0;
        int timeout = 200;                 // ~1s worst case (5ms steps)
        while (timeout-- > 0) {
            xhci_delay(5);
            sc = hub_get_port_status_full(xhc, hub_slot, p, &sok, &gs_tmo);
            if (gs_tmo) { hub_bail_log(hub_label, "get_port_status(reset)", p); return; }
            if (!sok) break;   // non-timeout transfer failure: give up this reset
            status = sc & 0xFFFF;
            change = (sc >> 16) & 0xFFFF;
            if (!(status & HUB_PS_RESET) &&
                ((change & HUB_PC_RESET) || (status & HUB_PS_ENABLE))) {
                reset_done = 1;
                break;
            }
        }
        // Ack reset (+ enable) change bits, then honor the 10ms reset recovery.
        if (hub_clear_port_feature(xhc, hub_slot, HUB_FEAT_C_PORT_RESET, p) == -1) {
            hub_bail_log(hub_label, "clear_feature C_RESET", p); return; }
        if (change & HUB_PC_ENABLE)
            if (hub_clear_port_feature(xhc, hub_slot, HUB_FEAT_C_PORT_ENABLE, p) == -1) {
                hub_bail_log(hub_label, "clear_feature C_ENABLE", p); return; }
        xhci_delay(10);

        // Re-read the settled post-reset status for enable + speed. The speed
        // bits are only valid after reset completes (LS/FS/HS of the DOWNSTREAM
        // device, which for the Apple keyboard is Low-Speed behind the HS hub).
        sc = hub_get_port_status_full(xhc, hub_slot, p, &sok, &gs_tmo);
        if (gs_tmo) { hub_bail_log(hub_label, "get_port_status(post-reset)", p); return; }
        status = sc & 0xFFFF;

        int dspeed = (status & HUB_PS_LOW_SPEED)  ? XHCI_SPEED_LOW  :
                     (status & HUB_PS_HIGH_SPEED) ? XHCI_SPEED_HIGH : XHCI_SPEED_FULL;

        snprintf(el, sizeof(el),
                 "[USB] %s.p%d post-reset st=%04x done=%d en=%d spd=%s",
                 hub_label, p, status, reset_done,
                 (status & HUB_PS_ENABLE) ? 1 : 0, xhci_speed_name(dspeed));
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] %s.p%d post-reset status=%04x reset_done=%d en=%d spd=%s",
                      hub_label, p, status, reset_done,
                      (status & HUB_PS_ENABLE) ? 1 : 0, xhci_speed_name(dspeed));

        if (!(status & HUB_PS_ENABLE)) {
            snprintf(el, sizeof(el), "[USB] %s.p%d reset FAILED st=%04x",
                     hub_label, p, status);
            gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
            bootlog_write("[xHCI] %s.p%d: hub port reset FAILED st=%04x",
                          hub_label, p, status);
            continue;
        }

        // Route string: append this hub port at this hub's tier.
        uint32_t droute = hub_route | ((uint32_t)(p & 0xF) << (4 * hub_depth));

        // Transaction Translator: a Low/Full-Speed device behind a High-Speed
        // hub needs the TT fields (this hub's slot + downstream port). Behind a
        // Full/Low-Speed hub the whole path is FS/LS so it inherits any upstream
        // TT (or none). High/Super-Speed devices need no TT.
        int d_tt_slot = 0, d_tt_port = 0;
        if (dspeed == XHCI_SPEED_LOW || dspeed == XHCI_SPEED_FULL) {
            if (hub_speed == XHCI_SPEED_HIGH) {
                d_tt_slot = hub_slot; d_tt_port = p;
            } else if (hub_tt_slot > 0) {
                d_tt_slot = hub_tt_slot; d_tt_port = hub_tt_port;
            }
        }

        int dslot = xhci_enable_slot(xhc);
        if (dslot < 1) {
            snprintf(el, sizeof(el), "[USB] %s.%d Enable Slot FAILED", hub_label, p);
            gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
            bootlog_write("[xHCI] %s.%d: enable slot FAILED", hub_label, p);
            continue;
        }
        if (xhci_address_device_ex(xhc, dslot, root_port, droute, dspeed,
                                   d_tt_slot, d_tt_port) < 0) {
            snprintf(el, sizeof(el), "[USB] %s.%d Address Device FAILED spd=%s",
                     hub_label, p, xhci_speed_name(dspeed));
            gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
            bootlog_write("[xHCI] %s.%d: address device FAILED spd=%s",
                          hub_label, p, xhci_speed_name(dspeed));
            xhci_disable_slot(xhc, dslot);
            continue;
        }
        if (dspeed == XHCI_SPEED_LOW || dspeed == XHCI_SPEED_FULL) xhci_delay(10);

        char dlabel[32];
        snprintf(dlabel, sizeof(dlabel), "%s.%d", hub_label, p);
        snprintf(el, sizeof(el), "[USB] hub %s: connect spd=%s -> slot=%d",
                 dlabel, xhci_speed_name(dspeed), dslot);
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);

        xhci_probe_device(xhc, dslot, dspeed, root_port, droute, hub_depth + 1,
                          d_tt_slot, d_tt_port, dlabel);
    }
}

// #307/#373 real-HW: enumerate a SINGLE root-hub port that has already been
// reset and enabled (PED=1, link U0). The caller passes the port's freshly
// negotiated PORTSC Port Speed value. Returns 1 if a device was enumerated.
static int xhci_enumerate_port(xhci_controller_t *xhc, uint32_t port, int speed) {
    extern void gfx_boot_log(const char *message);
    char el[96];

    // Enable Slot.
    int slot_id = xhci_enable_slot(xhc);
    if (slot_id < 1) {
        if (g_xhci_last_cmd_timeout) {
            snprintf(el, sizeof(el), "[USB] P%u enum: Enable Slot TIMEOUT (no event)",
                     port + 1);
        } else {
            snprintf(el, sizeof(el), "[USB] P%u enum: Enable Slot FAILED cc=%d",
                     port + 1, g_xhci_last_cmd_cc);
        }
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] Port %u: enable slot FAILED (timeout=%d cc=%d)",
                      port + 1, g_xhci_last_cmd_timeout, g_xhci_last_cmd_cc);
        return 0;
    }

    // Address Device on the root port (route 0, no TT).
    if (xhci_address_device_ex(xhc, slot_id, (int)port, 0, speed, 0, 0) < 0) {
        snprintf(el, sizeof(el), "[USB] P%u enum: slot=%d Address Device FAILED",
                 port + 1, slot_id);
        gfx_boot_log(el); kprintf("[xHCI] %s\n", el);
        bootlog_write("[xHCI] Port %u slot %d: address device FAILED", port + 1, slot_id);
        xhci_disable_slot(xhc, slot_id);
        return 0;
    }

    // #307: Full/Low-Speed real devices need a short recovery interval after
    // Address Device before they reliably answer control transfers.
    if (speed == XHCI_SPEED_FULL || speed == XHCI_SPEED_LOW) {
        xhci_delay(10);
    }

    char label[16];
    snprintf(label, sizeof(label), "P%u", port + 1);
    xhci_probe_device(xhc, slot_id, speed, (int)port, 0, 0, 0, 0, label);
    return 1;
}

// #433: bounded-retry enumeration of a single already-reset+enabled port.
// Real xHCI HID enumeration is racy on hardware: Enable Slot / Address Device /
// the descriptor fetch can transiently fail on one attempt and succeed moments
// later (this is why a plain keyboard works on one boot and dies the next while
// a bulk-only Ethernet dongle always works). Retry up to XHCI_ENUM_RETRIES
// times with an increasing settle delay, re-resetting the port between tries to
// recover its link state, and mark the port enumerated ONLY on success. On
// persistent failure the port is left UN-flagged so the periodic re-scan worker
// keeps retrying it (and a physically removed device stops the retry loop).
#define XHCI_ENUM_RETRIES 4
static int xhci_try_enumerate_port(xhci_controller_t *xhc, uint32_t port,
                                   int speed, int idx) {
    for (int attempt = 1; attempt <= XHCI_ENUM_RETRIES; attempt++) {
        int r = xhci_enumerate_port(xhc, port, speed);
        bootlog_write("[xHCI] Port %u enum attempt %d/%d: %s",
                      port + 1, attempt, XHCI_ENUM_RETRIES,
                      r ? "SUCCESS" : "failed");
        kprintf("[xHCI] Port %u enum attempt %d/%d: %s\n",
                port + 1, attempt, XHCI_ENUM_RETRIES, r ? "SUCCESS" : "failed");
        if (r) {
            g_port_enumerated[idx][port] = 1;
            return 1;
        }
        // Backoff, then re-reset the still-connected port and retry with its
        // freshly negotiated speed. If the device has physically gone, stop.
        xhci_delay(20 * attempt);
        uint32_t v = xhci_portsc_read(xhc, port);
        if (!(v & XHCI_PORTSC_CCS)) {
            bootlog_write("[xHCI] Port %u: device gone during retry; stopping", port + 1);
            break;
        }
        if (attempt < XHCI_ENUM_RETRIES) {
            xhci_port_reset(xhc, port);
            speed = (xhci_portsc_read(xhc, port) & XHCI_PORTSC_SPEED_MASK) >> 10;
        }
    }
    bootlog_write("[xHCI] Port %u: enumeration FAILED after retries; left eligible "
                  "for re-scan", port + 1);
    return 0;
}

int xhci_enumerate_devices(xhci_controller_t *xhc) {
    kprintf("[xHCI] Enumerating devices...\n");
    bootlog_write("[xHCI] Enumerating devices on %u port(s)", xhc->max_ports);
    extern void gfx_boot_log(const char *message);
    int idx = xhci_ctrl_index(xhc);

    for (uint32_t port = 0; port < xhc->max_ports; port++) {
        if (!xhci_port_is_connected(xhc, port)) {
            continue;
        }

        // #307 (b576): reset-then-enumerate guard. If the xhci_init reset pass
        // already reset AND enumerated this port (the real-HW USB-2 case), do
        // not enumerate it a second time - that would allocate a second slot for
        // the same device and could confuse the class driver.
        if (g_port_enumerated[idx][port]) {
            kprintf("[xHCI] Port %u already enumerated in reset pass; skipping\n",
                    port + 1);
            continue;
        }

        kprintf("[xHCI] Device detected on port %u\n", port + 1);
        bootlog_write("[xHCI] Port %u: device detected", port + 1);

        // Reset the port, then enumerate it. On QEMU the ports are already
        // enabled (PED=1) at this point and were NOT touched by the reset pass
        // above (its guard skips enabled ports), so this is the ONLY path that
        // enumerates QEMU devices - preserving the proven emulated behaviour.
        if (xhci_port_reset(xhc, port) < 0) {
            char rl[80];
            snprintf(rl, sizeof(rl), "[USB] P%u enum: port reset FAILED", port + 1);
            gfx_boot_log(rl); kprintf("[xHCI] %s\n", rl);
            bootlog_write("[xHCI] Port %u: reset FAILED, skipping device", port + 1);
            continue;
        }
        int speed = xhci_port_get_speed(xhc, port);
        // #433: mark enumerated only on success + bounded retry (see wrapper).
        xhci_try_enumerate_port(xhc, port, speed, idx);
    }

    int devices_found = g_enum_dev_found[idx];
    kprintf("[xHCI] Enumeration complete, %d device(s) found\n", devices_found);
    bootlog_write("[xHCI] Enumeration complete: %d device(s) found", devices_found);
    {   // #366: on-screen enumeration summary (cumulative across both passes)
        char dl[64];
        snprintf(dl, sizeof(dl), "[USB] enumeration: %d device(s)", devices_found);
        gfx_boot_log(dl);
    }
    return devices_found;
}

// =============================================================================
// Debug Output
// =============================================================================

void xhci_dump_controller_info(xhci_controller_t *xhc) {
    kprintf("\n[xHCI] Controller Information:\n");
    kprintf("  PCI: %02x:%02x.%x\n", xhc->pci->bus, xhc->pci->slot, xhc->pci->func);
    kprintf("  MMIO Base: 0x%016lx\n", (uint64_t)xhc->mmio_base);
    kprintf("  Max Slots: %u\n", xhc->max_slots);
    kprintf("  Max Ports: %u\n", xhc->max_ports);
    kprintf("  Max Interrupters: %u\n", xhc->max_interrupters);
    kprintf("  Context Size: %u bytes\n", xhc->context_size);
    kprintf("  64-bit Capable: %s\n", xhc->has_64bit ? "yes" : "no");
    kprintf("  Status: %s\n", xhc->initialized ? "initialized" : "not initialized");
    kprintf("  Enabled Slots: %u\n", xhc->enabled_slots);
}

// =============================================================================
// Evaluate Context: update EP0 max packet size (full-speed enumeration)
// =============================================================================

int xhci_evaluate_ep0_mps(xhci_controller_t *xhc, int slot_id, int max_packet) {
    size_t input_size = ALIGN_UP(xhc->context_size * 33, PAGE_SIZE);
    uint64_t input_phys = pmm_alloc_pages(input_size / PAGE_SIZE);
    if (input_phys == 0) return -1;
    xhci_input_ctx_t *input = (xhci_input_ctx_t *)input_phys;
    memset(input, 0, input_size);

    input->ctrl.add_flags = (1 << 1);   // only EP0 context (DCI 1)

    xhci_ep_ctx_t *ep0 = (xhci_ep_ctx_t *)((uint8_t *)input + xhc->context_size * 2);
    ep0->ep_type = EP_TYPE_CONTROL;
    ep0->max_packet = max_packet;
    ep0->cerr = 3;
    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][0];
    if (ring) ep0->tr_dequeue = ring->phys_addr | ring->cycle_bit;
    ep0->avg_trb_len = 8;

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = input_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_EVAL_CONTEXT) | ((slot_id & 0xFF) << 24);
    int result = xhci_send_command(xhc, &cmd);
    pmm_free_pages(input_phys, input_size / PAGE_SIZE);
    if (result < 0) {
        kprintf("[xHCI] Evaluate Context (EP0 mps %d) failed\n", max_packet);
        return -1;
    }
    kprintf("[xHCI] EP0 max packet updated to %d\n", max_packet);
    return 0;
}

// =============================================================================
// Isochronous OUT endpoint configuration and submission (USB Audio)
// =============================================================================

// Iso transfer rings are larger than the default so a batch can hold ~1 second
// of 1ms audio packets.
#define XHCI_ISO_RING_SIZE 1024

int xhci_configure_iso_out(xhci_controller_t *xhc, int slot_id,
                           int ep_addr, int max_packet, int interval) {
    int ep_num = ep_addr & 0x0F;
    int dci = ep_num * 2 + 0;   // OUT direction
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -1;

    // Allocate the iso transfer ring.
    xhci_ring_t *ring = (xhci_ring_t *)kmalloc(sizeof(xhci_ring_t));
    if (!ring) return -1;
    if (xhci_ring_init(ring, XHCI_ISO_RING_SIZE) < 0) {
        kfree(ring);
        return -1;
    }
    xhc->transfer_rings[slot_id - 1][dci] = ring;

    // Build the input context.
    size_t input_size = ALIGN_UP(xhc->context_size * 33, PAGE_SIZE);
    uint64_t input_phys = pmm_alloc_pages(input_size / PAGE_SIZE);
    if (input_phys == 0) return -1;
    xhci_input_ctx_t *input = (xhci_input_ctx_t *)input_phys;
    memset(input, 0, input_size);

    input->ctrl.add_flags = (1 << 0) | (1 << dci);   // slot ctx + iso EP
    input->ctrl.drop_flags = 0;

    // Copy the existing slot context and bump context_entries to cover the EP.
    xhci_slot_ctx_t *in_slot = (xhci_slot_ctx_t *)((uint8_t *)input + xhc->context_size);
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)((uint8_t *)xhc->dev_ctx[slot_id - 1]);
    memcpy(in_slot, out_slot, sizeof(xhci_slot_ctx_t));
    if (in_slot->context_entries < (uint32_t)dci) in_slot->context_entries = dci;

    // Endpoint context for the iso OUT endpoint.
    xhci_ep_ctx_t *ep = (xhci_ep_ctx_t *)((uint8_t *)input + xhc->context_size * (1 + dci));
    ep->ep_type = EP_TYPE_ISOCH_OUT;
    ep->max_packet = max_packet & 0x7FF;
    ep->max_burst = 0;
    ep->mult = 0;
    ep->cerr = 0;               // isochronous: no retries
    // Full-speed isochronous interval encoding: xHCI Interval = bInterval + 2.
    int xiv = interval + 2;
    if (xiv < 3) xiv = 3;
    if (xiv > 15) xiv = 15;
    ep->interval = xiv;
    ep->avg_trb_len = max_packet;
    ep->max_esit_lo = max_packet & 0xFFFF;
    ep->max_esit_hi = 0;
    ep->tr_dequeue = ring->phys_addr | ring->cycle_bit;

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = input_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_CONFIG_EP) | ((slot_id & 0xFF) << 24);
    int result = xhci_send_command(xhc, &cmd);
    pmm_free_pages(input_phys, input_size / PAGE_SIZE);
    if (result < 0) {
        kprintf("[xHCI] CONFIG_EP (iso) failed cc=%d\n", result);
        return -1;
    }
    return dci;
}

int xhci_iso_submit(xhci_controller_t *xhc, int slot_id, int dci,
                    uint64_t buf_phys, uint32_t total_bytes, uint32_t pkt_bytes) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    if (pkt_bytes == 0) return -1;

    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][dci];
    if (!ring) return -1;

    // Pre-compute how many TDs this batch will hold so we can flag the last one
    // with IOC (one completion event per batch: confirms the controller serviced
    // the iso ring, without flooding the event ring).
    int max_tds = (int)ring->size - 2;
    uint32_t tmp = 0;
    int total_tds = 0;
    while (tmp < total_bytes && total_tds < max_tds) {
        uint32_t l = pkt_bytes;
        if (tmp + l > total_bytes) l = total_bytes - tmp;
        tmp += l;
        total_tds++;
    }

    uint32_t off = 0;
    int count = 0;
    while (off < total_bytes && count < max_tds) {
        uint32_t len = pkt_bytes;
        if (off + len > total_bytes) len = total_bytes - off;

        xhci_trb_t *trb = xhci_ring_enqueue(ring);
        if (!trb) return -1;
        trb->parameter = buf_phys + off;
        trb->status = len & 0x1FFFF;            // TRB transfer length
        // Isoch TRB: Start Isoch ASAP (bit 31) so we need not compute frame IDs.
        uint32_t ctrl = XHCI_TRB_TYPE(TRB_ISOCH) | (1u << 31) | ring->cycle_bit;
        if (count == total_tds - 1) ctrl |= TRB_IOC;  // event on last TD only
        trb->control = ctrl;

        off += len;
        count++;
    }

    __asm__ volatile("mfence" ::: "memory");
    xhci_ring_doorbell(xhc, slot_id, dci);
    return count;
}

// =============================================================================
// #307: Generic BULK / INTERRUPT endpoint configuration
// =============================================================================
//
// Modeled on xhci_configure_iso_out but for bulk (MSC) and interrupt (HID)
// endpoints. Allocates a transfer ring, builds an input context that adds the
// slot context plus this one endpoint, and issues a CONFIG_EP command. Returns
// the DCI on success, -1 on failure. speed is one of XHCI_SPEED_*.
int xhci_configure_endpoint_ep(xhci_controller_t *xhc, int slot_id,
                               int ep_addr, int ep_type, int max_packet,
                               int b_interval, int speed) {
    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int dci = ep_num * 2 + is_in;
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -1;

    // If already configured (e.g. a device re-probed), reuse the ring.
    if (!xhc->transfer_rings[slot_id - 1][dci]) {
        xhci_ring_t *ring = (xhci_ring_t *)kmalloc(sizeof(xhci_ring_t));
        if (!ring) return -1;
        if (xhci_ring_init(ring, XHCI_RING_SIZE) < 0) {
            kfree(ring);
            return -1;
        }
        xhc->transfer_rings[slot_id - 1][dci] = ring;
    }
    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][dci];

    size_t input_size = ALIGN_UP(xhc->context_size * 33, PAGE_SIZE);
    uint64_t input_phys = pmm_alloc_pages(input_size / PAGE_SIZE);
    if (input_phys == 0) return -1;
    xhci_input_ctx_t *input = (xhci_input_ctx_t *)input_phys;
    memset(input, 0, input_size);

    input->ctrl.add_flags = (1u << 0) | (1u << dci);   // slot ctx + this EP
    input->ctrl.drop_flags = 0;

    // Copy existing slot context; bump context_entries to cover this DCI.
    xhci_slot_ctx_t *in_slot = (xhci_slot_ctx_t *)((uint8_t *)input + xhc->context_size);
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)((uint8_t *)xhc->dev_ctx[slot_id - 1]);
    memcpy(in_slot, out_slot, sizeof(xhci_slot_ctx_t));
    if (in_slot->context_entries < (uint32_t)dci) in_slot->context_entries = dci;

    xhci_ep_ctx_t *ep = (xhci_ep_ctx_t *)((uint8_t *)input + xhc->context_size * (1 + dci));
    ep->ep_type = ep_type;
    ep->max_packet = max_packet & 0x7FF;
    ep->max_burst = 0;
    ep->mult = 0;
    ep->cerr = 3;                       // 3 retries for bulk/interrupt
    ep->tr_dequeue = ring->phys_addr | ring->cycle_bit;
    ep->avg_trb_len = (ep_type == EP_TYPE_BULK_IN || ep_type == EP_TYPE_BULK_OUT)
                          ? 512 : max_packet;

    if (ep_type == EP_TYPE_INTERRUPT_IN || ep_type == EP_TYPE_INTERRUPT_OUT) {
        int xiv;
        if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER ||
            speed == XHCI_SPEED_SUPER_PLUS) {
            // High/Super speed: xHCI Interval = bInterval - 1 (1..16 -> 0..15).
            xiv = b_interval - 1;
        } else {
            // #139 ROOT CAUSE OF THE MOUSE LAG. Full/Low speed: bInterval is in
            // 1 ms frames, and the xHCI Endpoint Context Interval field is an
            // EXPONENT of 125 us microframes, so the encoding (xHCI 1.2 table
            // 6-12, and Linux xhci_parse_frame_interval) is
            //     Interval = floor(log2(bInterval * 8))
            // which for bInterval = 1 ms is 3, exactly as the comment here has
            // always said. The loop below already computes that value. The
            // `xiv += 3` that used to follow it applied the "+3" A SECOND TIME,
            // so every full-speed and low-speed interrupt endpoint in the
            // kernel was programmed with an interval EIGHT TIMES the period the
            // device asked for, and the controller polled it eight times too
            // slowly. Nothing failed: the endpoint worked perfectly, just at
            // 1/8 the rate, which is why this survived so long.
            //
            // MEASURED on a VM with TWO otherwise identical QEMU USB mice on
            // the SAME xHCI, one forced Full-Speed (bInterval 10, this branch)
            // and one High-Speed (bInterval 7, the branch above), driven with
            // continuous motion (see the [HIDLAT] lines in /BOOTLOG.TXT):
            //     full-speed, before this fix : reports every 63.9 ms  (15.6/s)
            //     high-speed, same boot       : reports every  8.2 ms  ( 122/s)
            //     full-speed, after this fix  : reports every  8.3 ms  ( 121/s)
            // The owner's iMac14,4 keyboard AND mouse both enumerate FULL-SPEED
            // (blame.md, #62/#135 captures: "P4 slot 7: 05ac:024f ...
            // Full-Speed (12 Mbps)"), and the Apple mouse reports bInterval=10,
            // so this branch is the one his hardware takes. It also explains the
            // #133 asymmetry: at ~15 Hz a KEYBOARD still feels perfect, because
            // keystrokes are discrete events tens of milliseconds apart, while
            // a pointer sampled 15 times a second is exactly "laggy and
            // jittery". Same controller, same driver, different perception of
            // the same defect.
            int frames = b_interval > 0 ? b_interval : 1;
            int microframes = frames * 8;
            xiv = 0;
            while ((1 << (xiv + 1)) <= microframes && xiv < 15) xiv++;
            // xHCI 1.2 6.2.3.6 restricts LS/FS interrupt endpoints to 3..10
            // (1 ms .. 128 ms). The formula above already lands in that range
            // for every legal bInterval (1..255 frames); clamp so a malformed
            // descriptor cannot program something the controller will reject.
            if (xiv < 3) xiv = 3;
            if (xiv > 10) xiv = 10;
        }
        if (xiv < 0) xiv = 0;
        if (xiv > 15) xiv = 15;
        ep->interval = xiv;
        // #139: record what was actually programmed. The interval is invisible
        // in every existing log, so an eight-fold error in it presented only as
        // "the mouse feels laggy" and could not be checked from a capture. The
        // owner's machine has no serial console, so this has to reach the
        // persistent log to be worth anything (blame.md #135).
        bootlog_write("[xHCI] slot %d DCI %d: interrupt EP 0x%02x bInterval %d "
                      "speed %d -> xHCI Interval %d (%d us period)",
                      slot_id, dci, ep_addr, b_interval, speed, xiv, 125 << xiv);
        ep->max_esit_lo = max_packet & 0xFFFF;
        ep->max_esit_hi = 0;
    }

    // #389 COMPOSITE-HID FIX: re-assert every endpoint already configured on this
    // slot in the SAME Configure Endpoint command.
    //
    // A single USB device that exposes TWO HID interfaces on ONE slot (the real
    // iMac keyboard 1a2c:95f6 = a KEYBOARD interface on EP 0x81/DCI 3 PLUS a MOUSE
    // interface on EP 0x82/DCI 5) has its endpoints configured by SEPARATE,
    // sequential calls to this function: first DCI 3, then DCI 5. The xHCI spec
    // says a Configure Endpoint command preserves endpoints whose Add flag is
    // clear, but real controllers are not uniformly strict, and configuring the
    // SECOND endpoint (mouse DCI 5) with an input context that only adds DCI 5 can
    // drop the FIRST endpoint (keyboard DCI 3) that was configured moments earlier.
    // That is exactly the reported failure: both interfaces enumerate and attach,
    // but the keyboard endpoint goes silent so no keystrokes reach the OS, while a
    // SEPARATE single-interface mouse (its own slot, only ever one CONFIG_EP) keeps
    // working. VMs never showed it because a qemu usb-kbd is single-interface, so
    // this second-endpoint-on-one-slot path was never exercised.
    //
    // To be correct on ANY controller we copy each already-configured endpoint's
    // LIVE output context back into the input context and set its Add flag, so the
    // command re-asserts (never drops) the keyboard endpoint while adding the
    // mouse endpoint. Copying the output context preserves each endpoint's current
    // TR dequeue pointer, which is the spec-supported way to keep an endpoint that
    // may already be running. This is a NO-OP for single-interface devices (they
    // have no other configured endpoint on the slot), so separate kbd + mouse and
    // MSC stay byte-identical.
    int reasserted = 0;
    for (int d = 2; d < XHCI_MAX_ENDPOINTS; d++) {
        if (d == dci) continue;                                // the new EP (set above)
        if (!xhc->transfer_rings[slot_id - 1][d]) continue;    // not configured
        xhci_ep_ctx_t *in_ep =
            (xhci_ep_ctx_t *)((uint8_t *)input + xhc->context_size * (1 + d));
        xhci_ep_ctx_t *out_ep =
            (xhci_ep_ctx_t *)((uint8_t *)xhc->dev_ctx[slot_id - 1] + xhc->context_size * d);
        memcpy(in_ep, out_ep, sizeof(xhci_ep_ctx_t));
        in_ep->ep_state = 0;               // ep_state is reserved on input; clear it
        input->ctrl.add_flags |= (1u << d);
        if ((uint32_t)d > in_slot->context_entries) in_slot->context_entries = d;
        reasserted++;
    }

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = input_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_CONFIG_EP) | ((slot_id & 0xFF) << 24);
    int result = xhci_send_command(xhc, &cmd);
    pmm_free_pages(input_phys, input_size / PAGE_SIZE);
    if (result < 0) {
        kprintf("[xHCI] CONFIG_EP (ep 0x%02x type %d) failed cc=%d\n",
                ep_addr, ep_type, result);
        return -1;
    }
    kprintf("[xHCI] Configured endpoint 0x%02x (DCI %d, type %d, mps %d)\n",
            ep_addr, dci, ep_type, max_packet);
    if (reasserted) {
        bootlog_write("[xHCI] slot %d: added DCI %d (ep 0x%02x) + re-asserted %d "
                      "existing endpoint(s) in one CONFIG_EP (composite HID safe)",
                      slot_id, dci, ep_addr, reasserted);
        // #390 COMPOSITE-HID FIX: the re-assert (above) keeps the previously
        // configured endpoint(s) from being DROPPED, but the controller
        // re-initializes each re-asserted endpoint context, which ORPHANS any
        // interrupt-IN TD that was already in flight on it. On the real iMac
        // keyboard 1a2c:95f6 the keyboard endpoint (slot 3 DCI 3) is armed FIRST
        // (usb_hid_attach submits its first TD) and THEN the mouse interface's
        // CONFIG_EP (DCI 5) re-asserts DCI 3, leaving the keyboard TD dead so no
        // keystrokes are ever delivered while the mouse (armed after its own
        // config) works. Re-submit a fresh interrupt-IN TD + ring the doorbell
        // for every already-armed HID endpoint on this slot except the one just
        // configured, so no in-flight TD is left orphaned. NO-OP for
        // single-interface devices and for non-HID slots (e.g. USB MSC bulk).
        int rearmed = usb_hid_rearm_slot(xhc, slot_id, dci);
        if (rearmed) {
            bootlog_write("[xHCI] slot %d: re-armed %d orphaned interrupt-IN TD(s) "
                          "after composite CONFIG_EP", slot_id, rearmed);
        }
    }
    return dci;
}

// #307: Non-blocking interrupt-IN model. Submit one Normal TRB on the interrupt
// IN ring and ring the doorbell. xhci_int_in_poll later drains the event ring
// and reports whether the TD completed, so a HID worker never blocks waiting on
// a keypress. buf_phys must be physically contiguous (identity-mapped).
int xhci_int_in_submit(xhci_controller_t *xhc, int slot_id, int dci,
                       uint64_t buf_phys, uint32_t len) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    xhci_ring_t *ring = xhc->transfer_rings[slot_id - 1][dci];
    if (!ring) return -1;

    g_xfer_cc[slot_id - 1][dci] = 0;   // clear previous completion
    xhci_trb_t *trb = xhci_ring_enqueue(ring);
    if (!trb) return -1;
    trb->parameter = buf_phys;
    trb->status = len & 0x1FFFF;
    trb->control = XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | ring->cycle_bit;
    __asm__ volatile("mfence" ::: "memory");
    xhci_ring_doorbell(xhc, slot_id, dci);
    return 0;
}

// =============================================================================
// #133/#134: xHCI endpoint HALT recovery
// =============================================================================
// Per xHCI 4.10.2.1 an endpoint whose transfer completes with Stall, USB
// Transaction Error, Babble Detected or Data Buffer Error is left in the HALTED
// state, where it executes nothing and ignores every doorbell. Recovering it is
// a two-command sequence. Until now this driver issued NEITHER: TRB_RESET_EP and
// TRB_SET_TR_DEQUEUE were #defined in xhci.h and had ZERO callers anywhere in
// the kernel, so the FIRST transfer error on any endpoint killed it until the
// next reboot. That is the mechanism behind a USB mouse that works and then
// stops while the keyboard (a different endpoint) carries on.

// EP0's transfer ring is stored at transfer_rings[slot-1][0] while its DCI is 1
// (xhci_control_transfer_to and xhci_address_device both index [0]). Every other
// endpoint is stored at its own DCI. Getting this wrong is silent: DCI 1 is
// never populated, so a recovery keyed on [1] would find NULL, do nothing, and
// report success-shaped failure. #62.
static xhci_ring_t *xhci_ring_for_dci(xhci_controller_t *xhc, int slot_id, int dci) {
    if (!xhc || slot_id < 1 || slot_id > (int)xhc->max_slots) return 0;
    if (dci < 1 || dci >= XHCI_MAX_ENDPOINTS) return 0;
    return xhc->transfer_rings[slot_id - 1][dci == 1 ? 0 : dci];
}

// Running/Stopped -> Stopped. Per xHCI 4.6.9 this is the command that aborts a
// TD that is still pending, which is the state a TIMED-OUT transfer leaves the
// endpoint in. Reset Endpoint (below) is only legal from Halted and answers
// Context State Error otherwise, so the two are not interchangeable.
int xhci_stop_endpoint(xhci_controller_t *xhc, int slot_id, int dci) {
    if (!xhc || slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 1 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = XHCI_TRB_TYPE(TRB_STOP_EP) |
                  ((uint32_t)(slot_id & 0xFF) << 24) |
                  ((uint32_t)(dci & 0x1F) << 16);
    return xhci_send_command_to(xhc, &cmd, XHCI_RECOVER_CMD_MS);
}

// Halted -> Stopped.
int xhci_reset_endpoint(xhci_controller_t *xhc, int slot_id, int dci) {
    if (!xhc || slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 1 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = XHCI_TRB_TYPE(TRB_RESET_EP) |
                  ((uint32_t)(slot_id & 0xFF) << 24) |
                  ((uint32_t)(dci & 0x1F) << 16);
    return xhci_send_command_to(xhc, &cmd, XHCI_RECOVER_CMD_MS);
}

// Discard the aborted TD and restart the transfer ring from a known-clean state.
// The ring is rewound to index 0 with cycle 1 (exactly what xhci_ring_init would
// leave), and the controller's dequeue pointer is set to match, so producer and
// consumer agree again. Doing only the Reset Endpoint would leave the controller
// pointing mid-TD at the transfer that failed.
int xhci_set_tr_dequeue(xhci_controller_t *xhc, int slot_id, int dci) {
    if (!xhc || slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 1 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    xhci_ring_t *ring = xhci_ring_for_dci(xhc, slot_id, dci);
    if (!ring || ring->size == 0) return -1;
    if (ring->phys_addr < 0x1000ULL || ring->phys_addr >= 0x80000000ULL) return -1;

    ring->trbs = (xhci_trb_t *)ring->phys_addr;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_bit   = 1;
    memset(ring->trbs, 0, ring->size * sizeof(xhci_trb_t));
    xhci_trb_t *link = &ring->trbs[ring->size - 1];
    link->parameter = ring->phys_addr;
    link->status    = 0;
    link->control   = XHCI_TRB_TYPE(TRB_LINK) | TRB_CYCLE | (1 << 1);  // toggle
    __asm__ volatile("mfence" ::: "memory");

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    // Dequeue Cycle State must match the ring's producer cycle bit (1 above).
    cmd.parameter = (ring->phys_addr & ~0xFULL) | 1ULL;
    cmd.control = XHCI_TRB_TYPE(TRB_SET_TR_DEQUEUE) |
                  ((uint32_t)(slot_id & 0xFF) << 24) |
                  ((uint32_t)(dci & 0x1F) << 16);
    return xhci_send_command_to(xhc, &cmd, XHCI_RECOVER_CMD_MS);
}

// #133: clear a DEVICE-side endpoint halt (USB 2.0 9.4.1
// CLEAR_FEATURE(ENDPOINT_HALT), bmRequestType 0x02 to the endpoint). Identical
// in shape to usb_msc_clear_stall(), which has done exactly this for bulk
// endpoints since the MSC driver was written; the interrupt-IN path never had
// it, so a STALLED HID endpoint could be Reset Endpoint'ed on the HOST side for
// ever while the DEVICE kept its halt feature set and stalled the very next
// transaction. BLOCKS (a control transfer), so this belongs on the re-scan
// worker, never on the HID poll worker where it would freeze all input for the
// duration of the request.
int xhci_clear_endpoint_halt(xhci_controller_t *xhc, int slot_id, int ep_addr) {
    if (!xhc || slot_id < 1) return -1;
    return xhci_control_transfer_to(xhc, slot_id,
        0x02,           // bmRequestType: Host->Device, Standard, Endpoint
        0x01,           // bRequest: CLEAR_FEATURE
        0x00,           // wValue: ENDPOINT_HALT
        ep_addr,        // wIndex: endpoint address, direction bit included
        NULL, 0, 250);
}

// #62: recover the CONTROL endpoint (DCI 1) after a control transfer that
// failed, so the failure costs one budget instead of every budget from now on.
//
// MEASURED on the owner's iMac14,4 (build 1904, /BOOTLOG.TXT): 17 "Transfer
// wait TIMEOUT ... 5000ms budget; 5006ms real" lines in ONE boot, 13 of them on
// slot 1 DCI 1 - the AX88772 USB-Ethernet dongle's control endpoint - which is
// ~85 SECONDS of blocking. That is not 13 independent faults. xhci_wait_transfer
// returned -1 and NOTHING cleaned up: the aborted TD stayed on the EP0 ring, the
// endpoint was left Stopped or Halted (xHCI 4.10.2.1: an endpoint that halts
// executes nothing and ignores every doorbell), and every later request queued
// TRBs behind it that could never run. So request #2 onward were GUARANTEED to
// burn their full budget. One quiet device poisoned its own control endpoint for
// the rest of the boot, and the ASIX link poller re-triggered it every second.
//
// xhci_recover_endpoint() already existed for exactly this (#133/#134) but had
// ONE caller, the HID interrupt-IN poll path (usb_hid.c). The control path -
// every enumeration request, every hub class request, every vendor register
// access - had none.
//
// wait_rc is xhci_wait_transfer's return value, whose contract is: -1 uniquely
// means TIMEOUT (no completion event at all, TD still pending, endpoint Running
// or Stopped), any other negative value is -cc for a real error completion
// (Stall / Transaction Error / Babble / Data Buffer Error), which per spec
// leaves the endpoint HALTED. The two states need different commands and Reset
// Endpoint answers Context State Error from a non-halted endpoint, so we try the
// state-appropriate command first and fall back to the other rather than
// guessing. Either way the ring is then rewound with Set TR Dequeue.
int xhci_recover_control_endpoint(xhci_controller_t *xhc, int slot_id, int wait_rc) {
    if (!xhci_ring_for_dci(xhc, slot_id, 1)) return -1;
    int timed_out = (wait_rc == -1);
    int ok;
    if (timed_out) {
        ok = (xhci_stop_endpoint(xhc, slot_id, 1) >= 0) ||
             (xhci_reset_endpoint(xhc, slot_id, 1) >= 0);
    } else {
        ok = (xhci_reset_endpoint(xhc, slot_id, 1) >= 0) ||
             (xhci_stop_endpoint(xhc, slot_id, 1) >= 0);
    }
    int deq = xhci_set_tr_dequeue(xhc, slot_id, 1);
    g_xfer_cc[slot_id - 1][1] = 0;
    g_xfer_residual[slot_id - 1][1] = 0;
    // Rate-limited: the whole point is that this stops repeating, so a line per
    // event is affordable for the first few and pure noise after that.
    static uint32_t diag = 0;
    if (diag < 12) {
        diag++;
        bootlog_write("[xHCI] slot %d DCI 1 (control): %s recovery %s/%s (#62)",
                      slot_id, timed_out ? "timeout" : "error",
                      ok ? "ep-ok" : "ep-FAIL", (deq >= 0) ? "deq-ok" : "deq-FAIL");
    }
    return (ok && deq >= 0) ? 0 : -1;
}

// Full recovery: reset, restart the ring, and drop any stale completion so the
// caller's next poll cannot consume the error a second time. The caller re-arms.
int xhci_recover_endpoint(xhci_controller_t *xhc, int slot_id, int dci) {
    if (xhci_reset_endpoint(xhc, slot_id, dci) < 0) {
        bootlog_write("[xHCI] slot %d DCI %d: Reset Endpoint FAILED", slot_id, dci);
        return -1;
    }
    if (xhci_set_tr_dequeue(xhc, slot_id, dci) < 0) {
        bootlog_write("[xHCI] slot %d DCI %d: Set TR Dequeue FAILED", slot_id, dci);
        return -1;
    }
    g_xfer_cc[slot_id - 1][dci] = 0;
    g_xfer_residual[slot_id - 1][dci] = 0;
    bootlog_write("[xHCI] slot %d DCI %d: endpoint halt recovered "
                  "(Reset Endpoint + Set TR Dequeue)", slot_id, dci);
    return 0;
}

// #139: has a drainer already recorded an unconsumed completion for this
// endpoint? A pure read of the completion byte: no drain, no lock, no side
// effect, so it is legal as a wait-queue condition (which is evaluated with
// interrupts off inside the wait macro, where draining would not be).
int xhci_xfer_pending(int slot_id, int dci) {
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS) return 0;
    if (dci < 0 || dci >= XHCI_MAX_ENDPOINTS) return 0;
    return g_xfer_cc[slot_id - 1][dci] != 0;
}

// Returns 1 and sets *out_len (bytes actually transferred) if the outstanding
// interrupt-IN TD completed, 0 if still pending, -1 on error/stall.
int xhci_int_in_poll(xhci_controller_t *xhc, int slot_id, int dci, uint32_t *out_len,
                     uint32_t submitted_len) {
    if (slot_id < 1 || slot_id > (int)xhc->max_slots) return -1;
    if (dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -1;
    xhci_poll_events(xhc);
    uint8_t cc = g_xfer_cc[slot_id - 1][dci];
    if (cc == 0) return 0;                 // nothing yet
    g_xfer_cc[slot_id - 1][dci] = 0;       // consume
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        g_xfer_last_err[slot_id - 1][dci] = cc;   // #133: keep the REASON
        return -1;
    }
    g_xfer_last_err[slot_id - 1][dci] = 0;
    if (out_len) {
        uint32_t resid = g_xfer_residual[slot_id - 1][dci];
        *out_len = (resid <= submitted_len) ? (submitted_len - resid) : submitted_len;
    }
    return 1;
}

// =============================================================================
// Global Access Functions
// =============================================================================

xhci_controller_t *xhci_get_controller(int index) {
    if (index < 0 || index >= xhci_controller_count) {
        return NULL;
    }
    return &xhci_controllers[index];
}

int xhci_get_controller_count(void) {
    return xhci_controller_count;
}

// =============================================================================
// #433: periodic port re-scan / HID hotplug
// =============================================================================
//
// HID previously enumerated only ONCE at boot. If the boot enumeration lost the
// race (item 1) or a keyboard/mouse is hot-plugged after boot, it never came up.
// xhci_rescan_ports() walks every root port on a controller and:
//   - enumerates any port that is connected but not yet enumerated (bounded
//     retry via xhci_try_enumerate_port), which recovers a device that failed
//     every boot attempt AND enumerates hot-plugged devices; and
//   - clears the enumerated flag for a port that has since DISCONNECTED, so a
//     replug re-enumerates it.
// Returns the number of newly enumerated devices. This is called ONLY from the
// dedicated re-scan worker thread below, never from a hot path / IRQ / the
// compositor draw thread.
// #134: release the resources of every HID slot that hung off a now-empty root
// port. Before this, a disconnect cleared ONLY the "enumerated" flag: the xHCI
// slot stayed enabled, its transfer rings stayed allocated, and above all the
// hid_devices[] entries were never released. Since that table is capped and was
// append-only, two replug cycles of a composite keyboard exhausted it and every
// further HID attach was silently refused - a total, unrecoverable input lockout
// on a machine with no serial and sshd=0 (the reported #134).
//
// DELIBERATELY LIMITED TO HID SLOTS. A disconnected MSC or network slot keeps
// exactly its previous behaviour, so the USB-boot path cannot regress: the
// golden boots its root filesystem off a USB mass-storage device on this very
// controller, and that is not a thing to experiment with blind.
//
// The transfer-ring PAGES are intentionally not freed, only unlinked. The HID
// poll worker runs concurrently at ~250 Hz and takes no lock, so freeing memory
// it might be walking would trade a bounded, once-per-replug leak for a
// use-after-free. Unlinking is enough to make the scarce resource (the
// controller's device slots, 32 on this iMac) recyclable.
//
// #250 (2026-08-23): MASS STORAGE IS NOW TORN DOWN TOO, WITH THE BOOT DEVICE
// EXPLICITLY EXCLUDED.
//
// The paragraph above is still the rule for the ROOT device and is now
// enforced as a check rather than as a blanket exclusion. Leaving ALL MSC
// slots alone had a consequence that only became visible once removable
// volumes reached the UI: nothing ever called usb_msc_device_removed(), so
// pulling a stick fired no REMOVED event, the hotplug manager never tore the
// mount down, and the drive stayed in the Files sidebar and on the desktop
// forever, pointing at a filesystem that is no longer there.
//
// THE BOOT MEDIUM IS SKIPPED BY IDENTITY, not by hoping it never disconnects:
// blk_root_usb_index() names the exact USB MSC device the root filesystem is
// served from, and that device's slot is left completely untouched. A spurious
// disconnect on the boot port therefore behaves exactly as it did before this
// change. Every other mass-storage slot is a data volume the user plugged in,
// and the honest response to it going away is to say so.
static int xhci_teardown_port_slots(xhci_controller_t *xhc, uint32_t port) {
    int torn = 0;
    for (int slot = 1; slot <= (int)xhc->max_slots && slot <= XHCI_MAX_SLOTS; slot++) {
        if (g_slot_root_port[slot - 1] != (int16_t)(port + 1)) continue;

        // #250: mass storage. Checked BEFORE the HID test because a slot is
        // one or the other, and the MSC branch must not fall through to the
        // HID one.
        {
            extern int usb_msc_find_device_by_slot(int slot_id);
            extern void usb_msc_device_removed(int slot_id);
            extern int blk_root_is_usb(void);
            extern int blk_root_usb_index(void);
            int mi = usb_msc_find_device_by_slot(slot);
            if (mi >= 0) {
                if (blk_root_is_usb() && mi == blk_root_usb_index()) {
                    bootlog_write("[xHCI] re-scan: port %u disconnect: slot %d is the "
                                  "BOOT medium (usb msc %d); NOT torn down",
                                  port + 1, slot, mi);
                    continue;
                }
                // Fires USB_MSC_EVENT_REMOVED, which is what makes
                // drivers/hotplug.c unmount the volume, invalidate every open
                // handle on it (mount generation) and drop it from the UI.
                usb_msc_device_removed(slot);
                for (int dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++)
                    xhc->transfer_rings[slot - 1][dci] = 0;   // unlink, do not free
                xhci_disable_slot(xhc, slot);
                g_slot_root_port[slot - 1] = 0;
                torn++;
                bootlog_write("[xHCI] re-scan: port %u disconnect: slot %d torn down "
                              "(USB MSC device %d removed, slot disabled)",
                              port + 1, slot, mi);
                continue;
            }
        }

        if (!usb_hid_slot_has_device(xhc, slot)) continue;   // HID slots only
        usb_hid_detach_slot(xhc, slot);
        for (int dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++)
            xhc->transfer_rings[slot - 1][dci] = 0;          // unlink, do not free
        xhci_disable_slot(xhc, slot);
        g_slot_root_port[slot - 1] = 0;   // unknown again
        torn++;
        bootlog_write("[xHCI] re-scan: port %u disconnect: slot %d torn down "
                      "(HID entries released, slot disabled)", port + 1, slot);
    }
    return torn;
}

// #134 (2026-08-18): RE-ARM A ROOT PORT AFTER A CONFIRMED DISCONNECT.
//
// WHAT WAS ACTUALLY MISSING. The disconnect path cleared a SOFTWARE flag
// (g_port_enumerated) and told the log it had "cleared for re-enum on replug".
// It never touched the PORT. In particular it never acknowledged PORTSC's seven
// RW1C change bits, which this file's own #135 comment states plainly: "The
// change bits are RW1C and the re-scan never clears them". So after the first
// disconnect CSC is stuck at 1 for the life of the boot, and per xHCI 1.1
// 4.19.2 a change bit generates a Port Status Change Event only on a 0->1
// TRANSITION. A port whose CSC is already 1 therefore reports NOTHING to the
// event ring when a device is plugged back in. Today we poll CCS so we survive
// that; the moment anyone wires hotplug to the event ring - which is the
// obvious next step and how every other xHCI driver does it - the replug
// becomes invisible, silently. It also destroys the diagnostic: every [PORTSC]
// line after the first disconnect prints CSC=1 whether or not anything latched.
//
// THREE THINGS, in the order the spec requires:
//   1. ACK every RW1C change bit, RW1C-safely (never write 1 to PED, which
//      DISABLES the port, and never to PR/WPR/LWS). This is what makes the next
//      attach a real 0->1 transition again.
//   2. PORT POWER. A port with PP=0 cannot detect a connect at all, ever. The
//      owner's capture had PP=1, so this is not what bit him, but "power was
//      still on" was an OBSERVATION about one log, not an invariant, and the
//      check costs one register read.
//   3. LINK STATE. Inactive(6) and Compliance(10) are the two states a port can
//      be parked in where the link is dead and a fresh attach is NOT reported
//      until software recovers it. The recovery is a WARM port reset (WPR),
//      which this driver has never issued. RxDetect(5) - what the owner's port
//      was in - is the correct, connect-detecting idle state, so this branch
//      does NOT fire for his capture and is not claimed as the fix for it.
//
// NOT A RESET ON EVERY DISCONNECT. A hot (PR) reset is NOT needed to OBSERVE a
// connect on a USB2 root port: the USB2 root-hub port state machine (xHCI 1.1
// 4.19.1.1) moves Disconnected -> Disabled in hardware on attach, setting CCS
// and latching CSC. A reset is needed to get Disabled -> Enabled before any
// transfer can run, and xhci_rescan_ports() already issues one immediately
// before enumerating. That half was never missing.
static void xhci_port_rearm_for_connect(xhci_controller_t *xhc, uint32_t port) {
    const uint32_t clear_mask = XHCI_PORTSC_CHANGE_BITS | XHCI_PORTSC_PED |
                                XHCI_PORTSC_PR | XHCI_PORTSC_WPR |
                                XHCI_PORTSC_LWS;
    uint32_t before = xhci_portsc_read(xhc, port);

    // 1. Acknowledge every latched change bit.
    xhci_portsc_write(xhc, port, (before & ~clear_mask) | XHCI_PORTSC_CHANGE_BITS);

    uint32_t v = xhci_portsc_read(xhc, port);

    // 2. Port power.
    int repowered = 0;
    if (!(v & XHCI_PORTSC_PP)) {
        xhci_portsc_write(xhc, port, (v & ~clear_mask) | XHCI_PORTSC_PP);
        for (int i = 0; i < 100; i++) xhci_delay(1);   // power-good + USB2 debounce
        v = xhci_portsc_read(xhc, port);
        repowered = 1;
    }

    // 3. Dead link states: warm reset.
    int warm = 0;
    uint32_t pls = (v & XHCI_PORTSC_PLS_MASK) >> 5;
    if (pls == 6 || pls == 10) {
        xhci_portsc_write(xhc, port, (v & ~clear_mask) | XHCI_PORTSC_WPR);
        int t = 200;                                  // bounded, ~200 ms
        while (t-- > 0) {
            xhci_delay(1);
            v = xhci_portsc_read(xhc, port);
            if (v & XHCI_PORTSC_WRC) break;
        }
        v = xhci_portsc_read(xhc, port);
        xhci_portsc_write(xhc, port, (v & ~clear_mask) | XHCI_PORTSC_CHANGE_BITS);
        v = xhci_portsc_read(xhc, port);
        warm = 1;
    }

    // The witness. Uncapped (once per confirmed disconnect, not per transition)
    // and it prints the register BEFORE and AFTER, because "cleared for re-enum"
    // was a sentence about an intention and this is a measurement.
    bootlog_write("[xHCI] re-scan: port %u re-armed for connect: PORTSC %08x -> "
                  "%08x; change bits ACKed%s%s; PP=%u PLS=%u(%s) CSC=%u. A new "
                  "attach can now latch CSC 0->1.",
                  port + 1, before, v,
                  repowered ? "; port power RE-ASSERTED (was PP=0)" : "",
                  warm ? "; WARM RESET issued (link was Inactive/Compliance)" : "",
                  (v & XHCI_PORTSC_PP) ? 1 : 0,
                  (v & XHCI_PORTSC_PLS_MASK) >> 5,
                  xhci_pls_name((v & XHCI_PORTSC_PLS_MASK) >> 5),
                  (v & XHCI_PORTSC_CSC) ? 1 : 0);

    // Our own write is not a spontaneous transition: re-baseline the transition
    // log so the NEXT [PORTSC] line is genuinely about the hardware.
    {
        int idx = xhci_ctrl_index(xhc);
        g_portsc_prev[idx][port] = v;
        g_portsc_prev_valid[idx][port] = 1;
    }
}

// #134: does ANY device slot still claim this root port? This is the hardware-
// backed answer to "is this port really enumerated", as opposed to the software
// flag g_port_enumerated, which is only a belief. See the stale-flag self-heal
// in xhci_rescan_ports().
static int xhci_port_has_live_slot(xhci_controller_t *xhc, uint32_t port) {
    for (int s = 1; s <= (int)xhc->max_slots && s <= XHCI_MAX_SLOTS; s++)
        if (g_slot_root_port[s - 1] == (int16_t)(port + 1)) return 1;
    return 0;
}

// #135: a port that has been confirmed-disconnected this many times is not
// being replugged by a human; it is FLAPPING. Re-enumerating it every 3s burns a
// controller device slot plus (for a composite keyboard) two hid_devices[]
// entries per cycle and hammers the bus for a device that will drop again. After
// the threshold, back off for a cooling period and SAY SO, so the failure is
// diagnosable instead of merely expensive. A port that then stays up decays its
// flap count back to zero, so a genuinely flaky cable that settles is forgiven.
#define XHCI_FLAP_THRESHOLD       8
#define XHCI_FLAP_COOLDOWN_SCANS 10   // ~30s at the 3000ms re-scan interval
#define XHCI_FLAP_STABLE_SCANS   20   // ~60s connected+enumerated clears the count

// #134: consecutive scans a CONNECTED port may read "enumerated" with no device
// slot naming it before the flag is treated as stale. 3 scans ~ 9s.
#define XHCI_ORPHAN_SCANS         3

// =============================================================================
// #134 (2026-08-18): THE RE-SCAN WORKER'S SILENCE MUST NEVER BE AMBIGUOUS AGAIN
// =============================================================================
//
// THE DIAGNOSTIC GAP THAT COST THIS TICKET THREE SESSIONS. Every line this
// worker emits is conditional on something having CHANGED. On the owner's
// iMac14,4 capture (golden 1923) the last thing it ever said was
// "port 4 disconnect: slot 3 torn down", after which the file runs another 110
// lines from other subsystems and this worker says nothing at all - through a
// physical replug. Two completely different faults produce that byte-for-byte:
//
//   (a) the worker is ALIVE and the hardware genuinely never reported a
//       connect (a port/link/routing problem), or
//   (b) the worker is STOPPED - wedged on a lock, or its CPU halted by a kernel
//       fault, which on an SMP box leaves the machine perfectly usable (see
//       blame.md, "A kernel PANIC can present as a HANG").
//
// Those need opposite fixes and nothing in the artifact separates them. So:
// emit a line that is NOT conditional on anything. It carries a monotonically
// increasing scan counter (so a stopped worker is visible as a counter that
// stops), the monotonic clock, the RAW PORTSC of every root port (so the
// hardware's own answer to "is anything plugged in" is in the artifact even
// when nothing changed and even after the capped [PORTSC] transition log has
// given up), the driver's enumerated-flag for each port, and the command-lock
// contention counters.
//
// BUDGET, because #134 also had to fix a diagnostic that burned the log down:
// one line per controller per XHCI_HB_SCANS scans, plus one FORCED line after
// any teardown or stale-flag clear. At 20 scans that is one ~200-byte line per
// minute, i.e. ~12 KB/hour, against a RAM buffer that now compacts what it has
// already persisted. The per-event flap diagnostics stay capped as they are;
// this one is deliberately periodic rather than per-event, which is what makes
// it bounded no matter how badly a port misbehaves.
#define XHCI_HB_SCANS 20              // ~60s at the 3000ms re-scan interval
static uint64_t g_rescan_scans    = 0;
static int      g_rescan_hb_force = 0;

static void xhci_rescan_heartbeat(void) {
    for (int i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *xhc = &xhci_controllers[i];
        if (!xhc->initialized) continue;
        int idx = xhci_ctrl_index(xhc);
        uint32_t n = xhc->max_ports;
        if (n > 16) n = 16;            // keep the line bounded; count is printed
        char ports[288];
        int o = 0;
        ports[0] = 0;
        for (uint32_t p = 0; p < n; p++) {
            if (o > (int)sizeof(ports) - 20) break;
            uint32_t v = xhci_portsc_read(xhc, p);
            int w = snprintf(ports + o, sizeof(ports) - (size_t)o, "%sp%u=%08x%c",
                             o ? " " : "", p + 1, v,
                             g_port_enumerated[idx][p] ? 'E' : '-');
            if (w <= 0) break;
            o += w;
            if (o >= (int)sizeof(ports)) { o = (int)sizeof(ports) - 1; break; }
        }
        bootlog_write("[xHCI-HB] ctrl%d ALIVE scan#%lu t=%lums ports=%u "
                      "cmd(contend=%lu,noblk=%lu) %s",
                      i, (unsigned long)g_rescan_scans,
                      (unsigned long)(mono_ready() ? mono_ms() : 0),
                      xhc->max_ports,
                      (unsigned long)g_xhci_cmd_contended,
                      (unsigned long)g_xhci_cmd_noblock_refused,
                      ports);
    }
}

int xhci_rescan_ports(xhci_controller_t *xhc) {
    if (!xhc || !xhc->initialized) return 0;
    int idx = xhci_ctrl_index(xhc);
    int newly = 0;
    uint32_t nports = xhc->max_ports;
    if (nports > 256) nports = 256;
    for (uint32_t port = 0; port < nports; port++) {
        // #135: read PORTSC ONCE per port per scan and log it whenever it moves.
        // This is the readout that has never existed: the re-scan previously
        // consumed a single bit (CCS) of this register and threw the rest away,
        // so the reason a still-plugged keyboard reads disconnected was not
        // merely unknown, it was unrecorded.
        uint32_t v = xhci_portsc_read(xhc, port);
        int first = !g_portsc_prev_valid[idx][port];
        if (first || v != g_portsc_prev[idx][port]) {
            if (!first && ((v ^ g_portsc_prev[idx][port]) & XHCI_PORTSC_CCS)) {
                xhci_portsc_log(xhc, port, g_portsc_prev[idx][port], "was");
                xhci_portsc_log(xhc, port, v, "now CCS-CHANGED");
                xhci_portctx_log(xhc, "on-CCS-change");
            } else {
                xhci_portsc_log(xhc, port, v, first ? "first-scan" : "changed");
            }
            g_portsc_prev[idx][port] = v;
            g_portsc_prev_valid[idx][port] = 1;
        }

        int connected = (v & XHCI_PORTSC_CCS) ? 1 : 0;
        if (!connected) {
            if (g_port_enumerated[idx][port]) {
                // #135: DEBOUNCE before believing it. One sample every 3 seconds
                // is not a disconnect detector.
                if (!xhci_port_disconnect_confirmed(xhc, port)) continue;
                g_port_enumerated[idx][port] = 0;   // allow replug re-enumeration
                g_port_stable[idx][port] = 0;
                if (g_port_flaps[idx][port] < 0xffff) g_port_flaps[idx][port]++;
                bootlog_write("[xHCI] re-scan: port %u disconnected (debounced over "
                              "%dms, flap #%u); cleared for re-enum on replug",
                              port + 1, XHCI_DEBOUNCE_SAMPLES * XHCI_DEBOUNCE_MS,
                              g_port_flaps[idx][port]);
                xhci_teardown_port_slots(xhc, port);   // #134: and RELEASE it
                // #134: and RE-ARM THE PORT ITSELF, not just our idea of it.
                xhci_port_rearm_for_connect(xhc, port);
                // #134: prove the worker is still alive on the very next scan.
                // Three sessions of this ticket stalled on being unable to tell
                // "alive, nothing changed" from "stopped right here".
                g_rescan_hb_force = 1;
                if (g_port_flaps[idx][port] >= XHCI_FLAP_THRESHOLD) {
                    g_port_cooldown[idx][port] = XHCI_FLAP_COOLDOWN_SCANS;
                    bootlog_write("[xHCI] *** port %u is FLAPPING (%u confirmed "
                                  "disconnects of a device nobody unplugged). "
                                  "Backing off re-enumeration for %d re-scans "
                                  "(~%ds) so it stops burning a device slot and "
                                  "HID entries every cycle. See the [PORTSC] "
                                  "lines above for why CCS reads 0. ***",
                                  port + 1, g_port_flaps[idx][port],
                                  XHCI_FLAP_COOLDOWN_SCANS,
                                  (XHCI_FLAP_COOLDOWN_SCANS * 3));
                }
            }
            continue;
        }
        if (g_port_enumerated[idx][port]) {
            // #134 (2026-08-18): SELF-HEAL A STALE "ENUMERATED" FLAG.
            //
            // g_port_enumerated is the ONE thing standing between a connected
            // port and a re-enumeration, and it is a byte of driver belief. If
            // any path ever leaves it set on a port that has no device slot,
            // that port is dead until reboot and NOTHING SAYS SO: the re-scan
            // worker logs only on change, so it goes quiet and looks exactly
            // like a healthy idle port. That failure mode was reachable, because
            // until this commit xhci_disable_slot() did not clear
            // g_slot_root_port[] and the two records could disagree with nothing
            // ever comparing them.
            //
            // Cross-check the belief against the slot table every scan and clear
            // it if they disagree for XHCI_ORPHAN_SCANS in a row (hysteresis, so
            // a device mid-enumeration is never clobbered; enumeration is
            // synchronous on this thread, so the margin is generous). This turns
            // "the state we clear is the state we consult" into something the
            // running system checks, instead of something a comment asserts.
            if (!xhci_port_has_live_slot(xhc, port)) {
                if (++g_port_orphan[idx][port] >= XHCI_ORPHAN_SCANS) {
                    bootlog_write("[xHCI] re-scan: port %u reads CONNECTED and is "
                                  "flagged enumerated, but NO device slot names "
                                  "it after %d scans. The flag is STALE: clearing "
                                  "it so this port is enumerated again.",
                                  port + 1, XHCI_ORPHAN_SCANS);
                    g_port_enumerated[idx][port] = 0;
                    g_port_orphan[idx][port] = 0;
                    g_rescan_hb_force = 1;
                    continue;   // next scan resets + enumerates it
                }
            } else if (g_port_orphan[idx][port]) {
                g_port_orphan[idx][port] = 0;
            }
            // Connected and working. Decay the flap count so a port that settles
            // is not penalised forever by an old burst.
            if (g_port_flaps[idx][port]) {
                if (++g_port_stable[idx][port] >= XHCI_FLAP_STABLE_SCANS) {
                    bootlog_write("[xHCI] port %u stable for %d re-scans; clearing "
                                  "flap count (%u)", port + 1,
                                  XHCI_FLAP_STABLE_SCANS, g_port_flaps[idx][port]);
                    g_port_flaps[idx][port] = 0;
                    g_port_stable[idx][port] = 0;
                }
            }
            continue;   // already enumerated
        }
        if (g_port_cooldown[idx][port]) {
            // #134 (2026-08-18): SAY SO. This branch is the one place in the
            // driver where a port that is PHYSICALLY CONNECTED is deliberately
            // not enumerated, and it was completely silent for the whole 30s
            // cooldown. From the artifact that is indistinguishable from the
            // fault this ticket is about - a connected device that never comes
            // up and nothing explaining why - which is the exact ambiguity the
            // rest of this change exists to remove. Measured on VM <vmid>: an
            // unplug/replug arm at ~13s intervals trips XHCI_FLAP_THRESHOLD on
            // a port within 8 cycles and then produces three "connected but
            // never enumerated" cycles with no explanation anywhere.
            // Once per cooldown, not once per scan, so it stays bounded.
            if (g_port_cooldown[idx][port] == XHCI_FLAP_COOLDOWN_SCANS) {
                bootlog_write("[xHCI] re-scan: port %u IS CONNECTED but "
                              "re-enumeration is SUPPRESSED for the next %d "
                              "scans (~%ds) by the flap governor (%u confirmed "
                              "disconnects). This is deliberate back-off, not a "
                              "failure to detect the device. It clears itself; "
                              "%d consecutive good scans also clear the count.",
                              port + 1, XHCI_FLAP_COOLDOWN_SCANS,
                              XHCI_FLAP_COOLDOWN_SCANS * 3,
                              g_port_flaps[idx][port], XHCI_FLAP_STABLE_SCANS);
                g_rescan_hb_force = 1;
            }
            g_port_cooldown[idx][port]--;
            continue;   // flap governor: not this scan
        }
        // Connected but not yet enumerated: reset then bounded-retry enumerate.
        bootlog_write("[xHCI] re-scan: port %u connected but not enumerated; "
                      "resetting + enumerating", port + 1);
        kprintf("[xHCI] re-scan: enumerating port %u\n", port + 1);
        if (xhci_port_reset(xhc, port) < 0) continue;
        int speed = xhci_port_get_speed(xhc, port);
        if (xhci_try_enumerate_port(xhc, port, speed, idx)) newly++;
    }
    return newly;
}

#ifdef XHCI_EPTEST
// =============================================================================
// #62: DESTRUCTIVE self-test for control-endpoint recovery.
// =============================================================================
// Built only with `make XHCIEPTEST=1` (GREEN, recovery compiled in) or
// `make XHCIEPTEST=2` (RED, -DXHCI_EPTEST_NORECOVER removes the recovery call
// and restores the pre-fix behaviour). Never in a golden.
//
// It does NOT simulate the fault. It causes a REAL one: a GET_DESCRIPTOR for
// descriptor type 0xFF, which every USB device answers with a STALL, which per
// xHCI 4.10.2.1 leaves the control endpoint HALTED - the exact state the owner's
// iMac log shows slot 1 stuck in. It then issues a PERFECTLY VALID
// GET_DESCRIPTOR(DEVICE) on the same endpoint and reports whether it worked.
//
//   GREEN arm: stall observed, recovery runs, the follow-up request SUCCEEDS.
//   RED arm:   stall observed, no recovery, the follow-up request cannot be
//              executed by a halted endpoint and burns its whole budget.
//
// If both arms pass, the test is worthless and the result must be discarded.
static void xhci_ep0_recovery_selftest(void) {
    for (int i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *xhc = &xhci_controllers[i];
        if (!xhc->initialized) continue;
        // Highest slot with an EP0 ring: on the test VM the boot MSC device
        // enumerates first, so the highest slot is a throwaway peripheral and a
        // RED arm cannot take the root filesystem down with it.
        int slot = -1;
        for (int s = 1; s <= (int)xhc->max_slots && s <= XHCI_MAX_SLOTS; s++)
            if (xhc->transfer_rings[s - 1][0]) slot = s;
        if (slot < 0) continue;

        static uint8_t dbuf[64] __attribute__((aligned(64)));
        bootlog_write("[EPTEST] ctrl %d slot %d: arming (recovery %s)", i, slot,
#ifdef XHCI_EPTEST_NORECOVER
                      "DISABLED (RED arm)");
#else
                      "ENABLED (GREEN arm)");
#endif
        // 1. Provoke a genuine STALL: descriptor type 0xFF does not exist.
        memset(dbuf, 0, sizeof(dbuf));
        int r1 = xhci_control_transfer_to(xhc, slot, 0x80, 0x06, 0xFF00, 0,
                                          dbuf, 18, 1000);
        bootlog_write("[EPTEST] step1 provoke-stall rc=%d (want <0)", r1);

        // 2. A request that MUST work on a healthy endpoint.
        memset(dbuf, 0, sizeof(dbuf));
        int r2 = xhci_control_transfer_to(xhc, slot, 0x80, 0x06, 0x0100, 0,
                                          dbuf, 18, 1000);
        int ok = (r2 == CC_SUCCESS || r2 == CC_SHORT_PACKET) && dbuf[1] == 0x01;
        bootlog_write("[EPTEST] step2 recover-and-reuse rc=%d bDescType=0x%02x "
                      "=> %s", r2, dbuf[1], ok ? "PASS" : "FAIL");
        bootlog_write("[EPTEST] VERDICT slot %d: %s", slot, ok ? "PASS" : "FAIL");
    }
}
#endif  // XHCI_EPTEST

// The re-scan worker. It sleeps XHCI_RESCAN_INTERVAL_MS between scans on the
// scheduler's timer-sleep queue (proc_sleep, the SAME shared primitive the HID
// poll worker uses) - it deschedules the thread until the wake tick, so it never
// busy-waits/spin-polls and never runs on a hot path (CLAUDE.md concurrency
// rule). A generous multi-second interval keeps this pure background hotplug/
// retry maintenance, not a poll loop.
#define XHCI_RESCAN_INTERVAL_MS 3000
static void xhci_rescan_worker(void *arg) {
    (void)arg;
    bootlog_write("[xHCI] port re-scan worker started (~%dms interval)",
                  XHCI_RESCAN_INTERVAL_MS);
#ifdef XHCI_EPTEST
    proc_sleep(4000);              // let every device finish enumerating
    xhci_ep0_recovery_selftest();  // #62 destructive proof; test builds only
#endif
    g_rescan_hb_force = 1;             // #134: one heartbeat on the first scan,
                                       // so "the worker never started" and "the
                                       // worker started and stopped later" are
                                       // distinguishable from the artifact.
    for (;;) {
        proc_sleep(XHCI_RESCAN_INTERVAL_MS);
        g_rescan_scans++;
        // #133: give a parked HID device another chance. This thread, not the
        // HID poll worker, because the recovery may need a CLEAR_FEATURE
        // control transfer and blocking the poll worker freezes ALL input.
#ifndef HID_PARKTEST_NOREVIVE
        usb_hid_retry_failed();
#endif
        int total_new = 0;
        for (int i = 0; i < xhci_controller_count; i++) {
            total_new += xhci_rescan_ports(&xhci_controllers[i]);
        }
        // #134: unconditional liveness + raw port state. AFTER the scan, so the
        // PORTSC values printed are the post-scan truth, and so a scan that
        // wedges produces no heartbeat rather than a stale-but-reassuring one.
        if (g_rescan_hb_force || (g_rescan_scans % XHCI_HB_SCANS) == 0) {
            g_rescan_hb_force = 0;
            xhci_rescan_heartbeat();
        }
        if (total_new > 0) {
            bootlog_write("[xHCI] re-scan enumerated %d new device(s)", total_new);
            // A newly enumerated HID must be polled. usb_hid_start_poll_thread
            // is idempotent (#433): if the worker is already running it is a
            // no-op (the running worker polls every hid_devices[] entry, so a
            // new device is picked up automatically); if no HID existed at boot
            // and the poll thread was never started, this starts it now.
            extern void usb_hid_start_poll_thread(void);
            usb_hid_start_poll_thread();
        }
    }
}

// #614: THE REDUNDANT, ALWAYS-ARMED WAKE SOURCE.
//
// This kernel has NO xHCI interrupt handler at all: the event ring is DMA-polled
// (see xhci_drain_events). So a waiter that blocks on g_xhci_evt_wq needs a
// drainer that is GUARANTEED to run, whether or not the controller ever raises
// an interrupt and whether or not any other subsystem happens to be polling.
// Two INDEPENDENT periodic drainers now exist, both funnelling through
// xhci_drain_events(), which wakes the queue on every pass:
//   (a) usb_hid_poll_worker() at ~4ms - pre-existing, but it only runs if a USB
//       HID device enumerated. A PS/2-only or headless box has none, so it can
//       never be the sole wake source.
//   (b) this worker - unconditional whenever ANY xHCI controller initialised.
// A completion can therefore strand a waiter for at most one ~4ms pass even on
// hardware that never interrupts (the real iMac's xHCI, #433/#373/#366), and
// xhci_block_for_cc() additionally caps each block at XHCI_BLOCK_MS. This is
// strictly stronger than the ISR-plus-worker shape the HDA PCM pump uses: there
// the ISR is the fast path and the worker the safety net, whereas here BOTH legs
// are timer-driven and neither depends on the device behaving.
static void xhci_evt_worker(void *arg) {
    (void)arg;
    bootlog_write("[xHCI] event-ring drain worker started (%d controller(s))",
                  xhci_controller_count);
    // #139: PROVE the interrupt fires, on a machine with no serial console.
    // "MSI armed" is a statement about a register write; this is a statement
    // about the handler having RUN. blame.md's zero-callers family (the USBLEGSUP
    // hand-off, validate_user_ptr, sse_save) is entirely made of code that was
    // believed live because it existed. Reported a handful of times early and
    // then never again, so it cannot burn the log down.
    uint32_t reports = 0;
    uint64_t next_report_ms = 0;
    for (;;) {
        for (int i = 0; i < xhci_controller_count; i++) {
            if (xhci_controllers[i].initialized)
                xhci_drain_events(&xhci_controllers[i]);
        }
        if (reports < 6 && sched_now_ms() >= next_report_ms) {
            next_report_ms = sched_now_ms() + (reports ? 30000 : 5000);
            reports++;
            bootlog_write("[xHCI] #139: MSI armed=%d, handler has run %llu time(s) "
                          "at %llu ms uptime", xhci_msi_armed(),
                          (unsigned long long)xhci_msi_count(),
                          (unsigned long long)sched_now_ms());
        }
        proc_sleep(1);   // one tick; a real timer sleep, never a busy spin (#426)
    }
}

// =============================================================================
// #139: MSI handler and arming
// =============================================================================

// Acknowledge and wake. Nothing else. See the note at g_xhci_msi_seq above for
// why this must not drain the ring.
//
// The two acknowledgements are both REQUIRED and both RW1C ("write 1 to
// clear"), so each is a read followed by writing the SAME bit back, never a
// blanket write:
//   USBSTS.EINT  (xHCI 5.4.2) - set whenever an interrupter asserts; the
//                controller will not assert again until it is cleared.
//   IMAN.IP      (xHCI 5.5.2.1) - per-interrupter pending flag; with MSI this
//                is cleared by the handler, and leaving it set wedges that
//                interrupter permanently after exactly one interrupt.
// Getting either wrong produces a driver that interrupts ONCE and then looks
// exactly like hardware that does not interrupt at all, which is the failure
// this whole change exists to end, so both are written explicitly.
static void xhci_msi_isr(interrupt_frame_t *frame) {
    (void)frame;
    for (int i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *xhc = &xhci_controllers[i];
        if (!xhc->initialized) continue;
        uint32_t sts = xhci_op_read32(xhc, XHCI_OP_USBSTS);
        if (sts & XHCI_STS_EINT)
            xhci_op_write32(xhc, XHCI_OP_USBSTS, XHCI_STS_EINT);
        uint32_t iman = xhci_rt_read32(xhc, XHCI_RT_IR0 + XHCI_IR_IMAN);
        if (iman & XHCI_IMAN_IP)
            xhci_rt_write32(xhc, XHCI_RT_IR0 + XHCI_IR_IMAN, iman | XHCI_IMAN_IP);
    }
    g_xhci_msi_hits++;
    g_xhci_msi_seq++;
    // wake_up_all takes the queue lock with spinlock_acquire_irqsave, so any
    // thread holding it runs with IF=0 and this interrupt cannot preempt it on
    // the same cpu. Checked in sync/waitq.c, not assumed (the HDA MSI handler
    // relies on the identical property).
    wake_up_all(&g_xhci_evt_wq);
    lapic_eoi();
}

// RECORD THE CAPABILITY LIST, NOT JUST THE VERDICT. blame.md #135: a driver
// that decides a hardware state on one bit should log the whole register, or
// the reason it decided that way is not merely unknown, it is unrecorded.
// "No MSI capability" and "has MSI-X but no MSI" are completely different
// situations with completely different fixes, and the owner's iMac14,4 xHCI
// (PCI 00:14.0) has never had its capability list dumped by anything.
// Bounded walk: a corrupt list must not spin.
//
// #156: this now runs WHETHER OR NOT MSI is armed. It is the single piece of
// evidence that says whether that machine's controller has MSI at all, which
// is the question the #139 boot regression turns on, and it must not be
// gated behind the very thing it is meant to justify.
static void xhci_log_pci_caps(xhci_controller_t *xhc) {
    char caps[96];
    int n = 0;
    caps[0] = 0;
    uint8_t off = pci_read8(xhc->pci->bus, xhc->pci->slot, xhc->pci->func, 0x34);
    for (int guard = 0; guard < 48 && off >= 0x40; guard++) {
        uint8_t id   = pci_read8(xhc->pci->bus, xhc->pci->slot, xhc->pci->func, off);
        uint8_t next = pci_read8(xhc->pci->bus, xhc->pci->slot, xhc->pci->func, off + 1);
        if (n < (int)sizeof(caps) - 8) {
            const char *hex = "0123456789abcdef";
            if (n) caps[n++] = ',';
            caps[n++] = '0'; caps[n++] = 'x';
            caps[n++] = hex[(id >> 4) & 0xF];
            caps[n++] = hex[id & 0xF];
            caps[n] = 0;
        }
        if (next == off || next < 0x40) break;
        off = next;
    }
    bootlog_write("[xHCI] #139: %04x:%04x at %02x:%02x.%x PCI capabilities [%s]"
                  " (MSI 0x05 present=%d, MSI-X 0x11 present=%d)",
                  xhc->pci->vendor_id, xhc->pci->device_id,
                  xhc->pci->bus, xhc->pci->slot, xhc->pci->func, caps,
                  pci_find_capability(xhc->pci, PCI_CAP_ID_MSI) ? 1 : 0,
                  pci_find_capability(xhc->pci, PCI_CAP_ID_MSIX) ? 1 : 0);
}

// #156: resolve the gate ONCE. Runs from xhci_setup_interrupt(), which main.c
// calls long after the FAT ESP is mounted (the same ordering /SMPSCHED.TXT
// relies on), so fat_exists() is safe here.
//
// THE BUILD ARM MUST BE READABLE OUT OF THE ARTIFACT. blame.md's recurring
// defect is a build believed to have a feature compiled out on the strength of
// the filename or the make line (the COMPOSIT-*-testhook staging with no
// TESTHOOK symbol in it). #if, not a ternary, so exactly ONE of these strings
// is linked and `strings kernel.elf | grep '#156 BUILD-ARM'` answers which
// kernel you are holding without booting it.
#if XHCI_MSI_DEFAULT_ON
static const char XHCI_MSI_BUILD_ARM[] = "[xHCI] #156 BUILD-ARM: MSI DEFAULT ON";
#else
static const char XHCI_MSI_BUILD_ARM[] = "[xHCI] #156 BUILD-ARM: MSI DEFAULT OFF";
#endif
const char *xhci_msi_build_arm(void) { return XHCI_MSI_BUILD_ARM; }

static void xhci_msi_gate_resolve(void) {
#if XHCI_MSI_DEFAULT_ON
    const char *why = "compiled-in default ON (make XHCIMSI=1)";
#else
    const char *why = "compiled-in default OFF (#156)";
#endif
    g_xhci_msi_gate = XHCI_MSI_DEFAULT_ON;
    bootlog_write("%s", XHCI_MSI_BUILD_ARM);

    extern fat_fs_t g_fat_fs;
    if (g_fat_fs.mounted) {
        if (fat_exists(&g_fat_fs, "/XHCIMSI.TXT")) {
            g_xhci_msi_gate = 1;
            why = "/XHCIMSI.TXT present on the FAT ESP";
        }
        // OFF wins over ON: a kernel built or gated ON must always be
        // recoverable by dropping one file on the ESP, with no rebuild.
        if (fat_exists(&g_fat_fs, "/XHCIMSI.OFF")) {
            g_xhci_msi_gate = 0;
            why = "/XHCIMSI.OFF present on the FAT ESP (overrides)";
        }
    } else {
        why = "FAT ESP not mounted, no gate file read; compiled-in default kept";
    }

    bootlog_write("[xHCI] #156: MSI gate = %s (%s)",
                  g_xhci_msi_gate ? "ON" : "OFF", why);
    kprintf("[xHCI] #156: MSI gate = %s (%s)\n",
            g_xhci_msi_gate ? "ON" : "OFF", why);
}

void xhci_setup_interrupt(void) {
    if (xhci_controller_count <= 0) return;

    xhci_msi_gate_resolve();

    int armed = 0, no_cap = 0, gated = 0;
    for (int i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *xhc = &xhci_controllers[i];
        if (!xhc->initialized || !xhc->pci) continue;

        // Unconditional: this is the evidence, not the feature (see above).
        xhci_log_pci_caps(xhc);

        // #156: register the C handler EVEN WHEN THE GATE IS OFF. Nothing can
        // raise vector 0x51 with MSI disabled, so this changes no behaviour;
        // but cpu/idt.c installs the 0x51 IDT GATE unconditionally, and
        // isr_handler_impl() answers an unregistered vector with a kprintf and
        // NO EOI. That would leave the LAPIC's in-service bit for 0x51 set for
        // ever, permanently masking every lower-priority vector on that cpu -
        // including the timer at 32. A stray interrupt must be acknowledged.
        idt_register_handler(XHCI_MSI_VECTOR, xhci_msi_isr);

        if (!g_xhci_msi_gate) {
            gated++;
            bootlog_write("[xHCI] #156: MSI NOT armed on %04x:%04x at %02x:%02x.%x "
                          "(gate OFF). Completions come from the two always-armed "
                          "timer-driven drain workers, which is exactly how every "
                          "build up to 1910 ran. IMOD/IMAN untouched, INTx left "
                          "alone, pci_enable_msi() not called.",
                          xhc->pci->vendor_id, xhc->pci->device_id,
                          xhc->pci->bus, xhc->pci->slot, xhc->pci->func);
            continue;
        }

        // Interrupter moderation. IMODI is in 250 ns units and is the interval
        // the controller waits before asserting again, so it is the storm
        // guard: 4000 == 1 ms == at most 1000 interrupts/second per
        // controller, which is the value the spec leaves in place after a
        // reset. It is programmed EXPLICITLY rather than inherited, because
        // firmware may have left anything here and an inherited 0 means "no
        // moderation at all" - an unbounded interrupt rate on a bus that also
        // carries this machine's root filesystem. 1 ms of added worst-case
        // latency against a mouse whose fastest endpoint period is 1 ms is a
        // trade worth making in that direction.
        xhci_rt_write32(xhc, XHCI_RT_IR0 + XHCI_IR_IMOD, 4000);

        // Re-assert IMAN.IE: xhci_event_ring_init set it, but a controller
        // reset between then and now would have cleared it, and it costs one
        // MMIO write to not depend on that.
        uint32_t iman = xhci_rt_read32(xhc, XHCI_RT_IR0 + XHCI_IR_IMAN);
        xhci_rt_write32(xhc, XHCI_RT_IR0 + XHCI_IR_IMAN, iman | XHCI_IMAN_IE);

        if (pci_enable_msi(xhc->pci, XHCI_MSI_VECTOR, lapic_get_id())) {
            armed++;
            g_xhci_msi_armed = 1;
            bootlog_write("[xHCI] #139: MSI armed on vector 0x%02x -> LAPIC %u "
                          "(controller %04x:%04x at %02x:%02x.%x, IMOD=4000 "
                          "= 1ms max rate)", XHCI_MSI_VECTOR, lapic_get_id(),
                          xhc->pci->vendor_id, xhc->pci->device_id,
                          xhc->pci->bus, xhc->pci->slot, xhc->pci->func);
        } else {
            no_cap++;
            bootlog_write("[xHCI] #139: controller %04x:%04x at %02x:%02x.%x has "
                          "NO MSI capability (see the capability list above; if it "
                          "shows 0x11 the controller has MSI-X, which this kernel "
                          "cannot yet program); completions stay on the timer-driven "
                          "drain workers only - the pre-#139 behaviour, not a "
                          "regression", xhc->pci->vendor_id, xhc->pci->device_id,
                          xhc->pci->bus, xhc->pci->slot, xhc->pci->func);
        }
    }
    kprintf("[xHCI] #139/#156: MSI armed on %d controller(s), %d without "
            "capability, %d held off by the #156 gate\n", armed, no_cap, gated);
    bootlog_write("[xHCI] #139/#156: MSI armed on %d controller(s), %d without "
                  "capability, %d held off by the #156 gate", armed, no_cap, gated);
}

void xhci_start_rescan_thread(void) {
    if (xhci_controller_count <= 0) {
        bootlog_write("[xHCI] no controllers; port re-scan worker NOT started");
        return;
    }
    proc_create("xhci_rescan", xhci_rescan_worker, NULL, PRIO_NORMAL);
    // #614: the always-armed event-ring drainer that makes blocking waits safe.
    proc_create("xhci_evt", xhci_evt_worker, NULL, PRIO_NORMAL);
    kprintf("[xHCI] periodic port re-scan + event drain workers created\n");
}
