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
#include "../proc/process.h"   // #433: proc_create/PRIO_* for the re-scan worker

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
static volatile uint8_t  g_xfer_cc[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
static volatile uint32_t g_xfer_residual[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];

// #307: the event ring is shared by control/bulk (blocking, via
// xhci_wait_for_event) and by the HID / UAC pollers (non-blocking, via
// xhci_poll_events). Since the scheduler is preemptive, a poller running on
// another thread could steal a completion event out from under a blocking
// transfer (observed: MSC "Data transfer failed" when the HID worker drained
// the ring first). This flag serializes event-ring consumption: blocking
// callers spin-acquire it; pollers try-acquire and simply skip if busy.
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

// #307 real-hardware follow-up: xhci_delay() used to be a fixed-iteration
// PAUSE-instruction spin ("ms * 10000" iterations), which is calibrated to
// whatever per-instruction cost happens to apply wherever it runs. QEMU's TCG
// emulation executes PAUSE far slower than real silicon, so the exact same
// loop that reliably burns ~1ms under QEMU can burn only a small FRACTION of
// a millisecond of real wall-clock time on real hardware. Every xHCI control
// transfer (device enumeration for BOTH HID and MSC) and every MSC bulk
// transfer waits via xhci_wait_for_event()/xhci_wait_transfer(), which loop
// "timeout_ms" times calling this function once per iteration - so an
// inaccurate xhci_delay() silently shrinks every one of those timeouts on
// real hardware, including the real-hardware-specific post-Address-Device and
// malformed-descriptor recovery delays #307 itself added (see
// xhci_enumerate_devices), which were meant to give slow real devices extra
// time and are undermined if the "ms" they ask for isn't really milliseconds.
//
// Fix: measure real elapsed time using the legacy PIT channel 0 countdown
// directly (latch-and-read via ports 0x43/0x40), which keeps counting down in
// hardware regardless of whether interrupts are enabled. This matters because
// xHCI/USB bring-up runs with interrupts still OFF (see main.c), so the
// interrupt-driven timer_ticks/g_timer_hz counter used elsewhere in the
// kernel does NOT advance here - PIT channel 0 is the only wall-clock source
// available this early. pit_init() (cpu/pic.c) programs channel 0 and it is
// left running continuously; nothing else in the kernel reprograms channel 0
// (the PC speaker uses channel 2), so this is safe to read at any time after
// pit_init() runs in main(), which is long before xHCI init.
#define XHCI_PIT_CMD_PORT   0x43
#define XHCI_PIT_CH0_PORT   0x40
#define XHCI_PIT_INPUT_HZ   1193182u

extern uint32_t g_timer_hz;

static inline uint16_t xhci_pit_latch(void) {
    outb(XHCI_PIT_CMD_PORT, 0x00);  // latch command, channel 0 (counting unaffected)
    uint8_t lo = inb(XHCI_PIT_CH0_PORT);
    uint8_t hi = inb(XHCI_PIT_CH0_PORT);
    return (uint16_t)((hi << 8) | lo);
}

static void xhci_delay(uint32_t ms) {
    if (ms == 0) return;

    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint32_t reload = XHCI_PIT_INPUT_HZ / hz;
    if (reload == 0 || reload > 65535) reload = 65535;

    uint64_t target_counts = ((uint64_t)ms * XHCI_PIT_INPUT_HZ) / 1000;
    uint64_t counted = 0;
    uint16_t prev = xhci_pit_latch();

    // Safety cap: an extremely generous upper bound on loop iterations so a
    // wedged/absent PIT (should not happen - see comment above) degrades to
    // "delay ends a bit early" instead of an unbounded hang. This is far more
    // iterations than any real ms budget used in this file needs even at a
    // pessimistic per-iteration cost.
    uint32_t safety_iters = ms * 200000u + 1000000u;

    while (counted < target_counts && safety_iters--) {
        __asm__ volatile("pause");
        uint16_t cur = xhci_pit_latch();
        uint32_t delta = (cur <= prev) ? (uint32_t)(prev - cur)
                                        : (uint32_t)(prev + (reload - cur));
        counted += delta;
        prev = cur;
    }
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

// Send a command and wait for completion
static int xhci_send_command(xhci_controller_t *xhc, xhci_trb_t *cmd) {
    xhci_trb_t *trb = xhci_ring_enqueue(&xhc->cmd_ring);
    if (!trb) { kprintf("[xHCI] command ring enqueue failed\n"); return -1; }

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
    int r = xhci_wait_for_event(xhc, TRB_COMMAND_COMPLETION, 5000);
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
    return r;
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

    // #307: xHCI USBLEGSUP BIOS->OS ownership hand-off, BEFORE the reset.
    // Extended capability list starts at (HCCPARAMS1 xECP)*4 from MMIO base.
    // Cap id 1 = USB Legacy Support: set HC OS Owned (bit24), wait for HC BIOS
    // Owned (bit16) to clear, then quiesce SMIs in USBLEGCTLSTS (next dword).
    {
        uint32_t hcc = xhci_cap_read32(xhc, XHCI_CAP_HCCPARAMS1);
        uint32_t xecp = (hcc >> 16) & 0xffff;   // offset in 32-bit dwords
        uint32_t guard = 0;
        while (xecp && guard++ < 64) {
            volatile uint32_t *cap = (volatile uint32_t *)(xhc->mmio_base + (xecp * 4));
            uint32_t capval = *cap;
            uint8_t capid = capval & 0xff;
            if (capid == 1) { // USB Legacy Support Capability
                if (capval & (1u << 16)) { // HC BIOS Owned Semaphore set
                    *cap = capval | (1u << 24); // request HC OS Owned
                    for (int i = 0; i < 100; i++) { // up to ~100ms
                        if (!((*cap) & (1u << 16))) break;
                        xhci_delay(1);
                    }
                    kprintf("[xHCI] BIOS->OS hand-off: LEGSUP now 0x%08x\n", *cap);
                } else {
                    kprintf("[xHCI] BIOS->OS hand-off: not BIOS-owned (LEGSUP 0x%08x)\n", capval);
                }
                // Disable all legacy SMIs, ack RW1C bits in USBLEGCTLSTS.
                volatile uint32_t *ctlsts = cap + 1;
                *ctlsts = (*ctlsts) & 0x7;  // clear SMI enables; keep low bits
                break;
            }
            uint32_t next = (capval >> 8) & 0xff; // next ptr in dwords
            if (!next) break;
            xecp += next;
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
                g_xfer_cc[slot - 1][ep] = cc ? cc : CC_SUCCESS;
            }
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
static void xhci_drain_events(xhci_controller_t *xhc) {
    if (!xhci_evt_trylock()) return;

    uint32_t ir_offset = XHCI_RT_IR0;
    xhci_trb_t *event = &xhc->event_ring.trbs[xhc->event_ring.dequeue_idx];
    int processed = 0;

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
    }

    if (processed) {
        // Update ERDP once, after draining (EHB bit clears event-handler-busy).
        uint64_t erdp = xhc->event_ring.phys_addr +
                        xhc->event_ring.dequeue_idx * sizeof(xhci_trb_t);
        erdp |= (1 << 3);
        xhci_rt_write64(xhc, ir_offset + XHCI_IR_ERDP, erdp);
    }

    xhci_evt_unlock();
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
// (sched_preemption_enabled()) switch to an adaptive spin-THEN-SLEEP: poll fast
// for a few ms to catch the common quick completion with low latency, then
// proc_sleep() between polls so the core idle-HLTs instead of burning while the
// slow stick transfers. The event ring is DMA'd by the controller regardless of
// interrupts, so draining after each sleep still observes the completion (no IRQ
// wiring needed). The runtime wait is bounded in real wall-clock via timer_ticks.
#define XHCI_WAIT_FAST_SPINS 4   // ~4ms of quick polling before we start sleeping

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
            if (spins < XHCI_WAIT_FAST_SPINS) { xhci_delay(1); spins++; }
            else proc_sleep(1);   // sleeps >= 1 tick; core can HLT meanwhile
        } else {
            if (spins >= timeout_ms) break;   // preserve boot iteration-count budget
            xhci_delay(1); spins++;
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
    // #375: adaptive spin-then-sleep at runtime (see xhci_wait_for_event). This
    // is the HOTTEST path on a USB-root box - every MSC bulk read/write CSW waits
    // here - so it is what pegs a core on a slow stick. Boot path unchanged.
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
            if (spins < XHCI_WAIT_FAST_SPINS) { xhci_delay(1); spins++; }
            else proc_sleep(1);   // sleeps >= 1 tick; core idle-HLTs meanwhile
        } else {
            if (spins >= timeout_ms) break;   // preserve boot iteration-count budget
            xhci_delay(1); spins++;
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
    return xhci_wait_transfer(xhc, slot_id, 1, timeout_ms);
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
                          uint8_t subclass, uint8_t protocol);
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
            usblog_write("    (HID class descriptor, %d bytes)", blen);
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
            int j = i + blen;
            while (j + 2 <= total) {
                int elen = cfg[j];
                int etype = cfg[j + 1];
                if (elen < 2 || j + elen > total) break;
                if (etype == 0x04) break;              // next interface
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
                        usb_hid_attach(xhc, slot_id, iface_num, ep_int_in,
                                       ep_int_in_mps, ep_int_interval, speed,
                                       (uint8_t)isub, (uint8_t)iproto);
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
            // Full/Low speed: bInterval is in 1ms frames; convert to the
            // 125us-based log2 interval (1ms == 8 microframes -> Interval 3).
            int frames = b_interval > 0 ? b_interval : 1;
            int microframes = frames * 8;
            xiv = 0;
            while ((1 << (xiv + 1)) <= microframes && xiv < 15) xiv++;
            xiv += 3;
        }
        if (xiv < 0) xiv = 0;
        if (xiv > 15) xiv = 15;
        ep->interval = xiv;
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
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
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
int xhci_rescan_ports(xhci_controller_t *xhc) {
    if (!xhc || !xhc->initialized) return 0;
    int idx = xhci_ctrl_index(xhc);
    int newly = 0;
    for (uint32_t port = 0; port < xhc->max_ports; port++) {
        int connected = xhci_port_is_connected(xhc, port);
        if (!connected) {
            if (g_port_enumerated[idx][port]) {
                g_port_enumerated[idx][port] = 0;   // allow replug re-enumeration
                bootlog_write("[xHCI] re-scan: port %u disconnected; cleared for "
                              "re-enum on replug", port + 1);
            }
            continue;
        }
        if (g_port_enumerated[idx][port]) continue;   // already enumerated
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
    for (;;) {
        proc_sleep(XHCI_RESCAN_INTERVAL_MS);
        int total_new = 0;
        for (int i = 0; i < xhci_controller_count; i++) {
            total_new += xhci_rescan_ports(&xhci_controllers[i]);
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

void xhci_start_rescan_thread(void) {
    if (xhci_controller_count <= 0) {
        bootlog_write("[xHCI] no controllers; port re-scan worker NOT started");
        return;
    }
    proc_create("xhci_rescan", xhci_rescan_worker, NULL, PRIO_NORMAL);
    kprintf("[xHCI] periodic port re-scan worker created\n");
}
