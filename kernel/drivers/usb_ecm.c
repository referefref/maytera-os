// usb_ecm.c - CDC-ECM USB Ethernet class driver + shared USB-NIC core (#362).
//
// Core model (mirrors the USB HID non-blocking pattern from #307):
//   RX: exactly one bulk-IN TD is kept outstanding. usb_eth_receive() polls
//       its completion via the shared per-(slot,DCI) table (xhci_int_in_poll),
//       parses the completed buffer into a small frame FIFO (one ECM transfer
//       is one Ethernet frame; ASIX transfers can carry several framed
//       packets), then resubmits. The net stack's nic_receive() pops frames
//       from the FIFO, exactly like e1000_receive() pops its RX ring.
//   TX: copy the frame into the next slot of a small DMA bounce-buffer RING,
//       submit one TD (plus a terminating ZLP TD when the frame is an exact
//       multiple of the endpoint max packet size) on the bulk OUT ring, and
//       RETURN immediately. TX is fire-and-forget: the completion is reaped
//       lazily off the shared event ring by the RX poll path / HID worker, and
//       the ring keeps a frame's buffer from being reused while its TD is still
//       in flight. Ethernet is lossy and TCP retransmits, so an un-acked frame
//       simply drops.
//
// #549: TX does NOT wait. nic_send() calls the driver under net_lock() (cli:
// interrupts OFF + a spinlock held), and TX also runs before the scheduler
// exists (DHCP during boot), so a blocking primitive (wait_event/proc_sleep)
// would deadlock here. The old code instead busy-polled xhci_delay_ms(1) up to
// ~40ms per send, pegging a core (and holding interrupts off for that whole
// window) on the iMac USB-Ethernet path. The CLAUDE.md no-block-context rule
// ("do async-fetch-then-cache, never block") applies to TX: submit and return.
// This also makes a dead/unplugged dongle an instant no-op (#381) rather than a
// 40ms spin.
#include "usb_net.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../fs/bootlog.h"
#include "../fs/fat.h"          // #155: /USBNETQ.OFF ESP lever
#include "../sync/spinlock.h"   // #155: RX queue + frame FIFO serialisation
#include "../sync/waitq.h"      // #155: the shared wait primitive, never a poll
#include "../proc/process.h"    // #155: proc_create for the bulk-IN reaper

usbnet_dev_t g_usbnet;

// =============================================================================
// Frame FIFO (RX side, between the bulk-IN parser and nic_receive())
// =============================================================================

// #362: 48 slots (was 16). One 16 KiB AX88772 bulk-IN transfer can carry
// ~10 full-size frames, and many more small ones; the FIFO is filled in one
// go by the de-framer before nic_receive() pops any of it. Overflow is still
// a safe drop (Ethernet is lossy), this just stops it happening in normal
// bulk transfer.
// #155: 128 slots (was 48), and the reason is now a rate, not a transfer size.
// The producer is the bulk-IN reaper thread, which can wake up to 250 times a
// second (one timer tick) and empties the whole in-flight queue on each pass;
// the consumer is net_poll()'s RX drain at ~85 Hz. Between two drains, 100
// Mbit delivers ~100 full-size frames. 48 slots overflowed that by 2x the
// moment the queue got deep. 128 * 1520 = 195 KiB of BSS.
#define USBNET_FIFO_SLOTS  128

static uint8_t  fifo_frame[USBNET_FIFO_SLOTS][USBNET_FRAME_MAX];
static uint16_t fifo_len[USBNET_FIFO_SLOTS];
static int fifo_head = 0;   // next slot to pop
static int fifo_tail = 0;   // next slot to fill
static int fifo_count = 0;

// #155: the FIFO now has TWO contexts on it - the reaper thread pushes with
// interrupts ON, net_poll() pops under net_lock() with interrupts OFF - so it
// needs a lock where before it had one caller and none. The critical section is
// ONE memcpy of at most 1520 bytes and nothing else: it is never held across a
// de-frame pass, so the worst case a net_poll() spinner can pay is sub-
// microsecond, which is what keeps [NETSTARVE] holdmax where it was.
// LOCK ORDER: net_lock -> fifo_lock. The reaper takes fifo_lock alone and never
// takes net_lock, so there is no cycle.
static spinlock_t fifo_lock = SPINLOCK_INIT;
uint64_t g_usbnet_fifo_drops = 0;   // frames dropped because the FIFO was full

void usbnet_fifo_push(const uint8_t *frame, uint32_t len) {
    if (len < 14 || len > USBNET_FRAME_MAX) return;   // runt / oversize
    uint64_t fl = spinlock_acquire_irqsave(&fifo_lock);
    if (fifo_count >= USBNET_FIFO_SLOTS) {            // overflow: drop
        g_usbnet_fifo_drops++;
        spinlock_release_irqrestore(&fifo_lock, fl);
        return;
    }
    memcpy(fifo_frame[fifo_tail], frame, len);
    fifo_len[fifo_tail] = (uint16_t)len;
    fifo_tail = (fifo_tail + 1) % USBNET_FIFO_SLOTS;
    fifo_count++;
    spinlock_release_irqrestore(&fifo_lock, fl);
}

static int usbnet_fifo_pop(uint8_t *out, uint32_t out_size) {
    uint64_t fl = spinlock_acquire_irqsave(&fifo_lock);
    if (fifo_count == 0) { spinlock_release_irqrestore(&fifo_lock, fl); return 0; }
    uint32_t len = fifo_len[fifo_head];
    if (len > out_size) len = out_size;
    memcpy(out, fifo_frame[fifo_head], len);
    fifo_head = (fifo_head + 1) % USBNET_FIFO_SLOTS;
    fifo_count--;
    spinlock_release_irqrestore(&fifo_lock, fl);
    return (int)len;
}

// =============================================================================
// Shared helpers
// =============================================================================

// #549: number of DMA slots in the TX ring. Fire-and-forget TX cycles through
// these so a frame's buffer is not overwritten while its bulk-OUT TD may still
// be in flight. 8 is far deeper than any realistic in-flight TX depth through
// this stack (a bulk-OUT completes in microseconds on a healthy link), so a
// reuse-while-in-flight collision is effectively impossible; even if one did
// occur the worst case is a single dropped frame, which Ethernet tolerates.
#define USBNET_TX_RING_SLOTS 8

// #155: HOW MANY BULK-IN TDs TO KEEP IN FLIGHT, AND WHY THAT NUMBER.
//
// A bulk-IN TD retires on the FIRST SHORT PACKET, i.e. at the end of whatever
// burst the chip had ready - as little as ONE Ethernet frame (1514 + 4 header =
// 1518 bytes = two 512-byte packets plus a 494-byte short one). So the queue is
// consumed at up to the FRAME rate, not the transfer-size rate: 100 Mbit is
// ~8200 full-size frames/s, one every ~122 us.
//
// The reaper thread is woken by the xHCI event-ring wait queue. With MSI armed
// that is microseconds, but MSI is DEFAULT-OFF on this kernel (/XHCIMSI.OFF,
// #156), so the guaranteed floor is the periodic drainers: one timer tick at
// g_timer_hz = 250 Hz, i.e. 4 ms. In 4 ms at line rate ~33 TDs can retire.
//
// 32 is that number. It is a DEPTH chosen from the reaper's worst-case wake
// period times the frame rate, not a round number: below it the endpoint runs
// dry between wakes and the controller stops asking the device for data, which
// is the whole #155 defect in miniature. Above it costs memory for slack that
// the chip's own RX FIFO already provides.
//
// The per-slot SIZE is unchanged at whatever the vendor driver asked for
// (16 KiB for AX88772). Shrinking it to pay for depth would newly invoke the
// split-frame carry-over path, which has NEVER EXECUTED on hardware
// (usbsplit=0 over 2 MiB / ~6800 frames, #362) and is therefore the least
// trustworthy code in this driver. 32 * 16 KiB = 512 KiB of identity-mapped
// PMM, allocated once at attach.
#define USBNET_RX_SLOTS 32

int usbnet_alloc_buffers(usbnet_dev_t *d, uint32_t rx_len, uint32_t tx_len) {
    // Page allocations are identity-mapped (phys == virt). One page each is
    // enough for ECM/AX88772 (<= 2KB per transfer); AX88179 asks for more.
    // #155: rx_len is the PER-SLOT size; USBNET_RX_SLOTS slots are allocated.
    // #155: DEGRADE, NEVER FAIL. 32 x 16 KiB is 512 KiB of PHYSICALLY CONTIGUOUS
    // pages, 32x what this driver used to ask for. If the PMM cannot satisfy it
    // the old code's behaviour would be to fail usbnet_alloc_buffers(), fail the
    // attach, and leave the machine with NO NETWORK AT ALL - trading "the
    // network is slow" for "the network is gone", which is a far worse bug than
    // the one being fixed. So halve the slot count until it fits; depth 1 is the
    // pre-#155 behaviour and always allocatable.
    int slots = USBNET_RX_SLOTS;
    uint64_t rx_phys = 0;
    uint32_t rx_pages = 0;
    while (slots >= 1) {
        rx_pages = (rx_len * (uint32_t)slots + PAGE_SIZE - 1) / PAGE_SIZE;
        rx_phys = pmm_alloc_pages(rx_pages);
        if (rx_phys) break;
        if (slots == 1) break;
        slots /= 2;
    }
    if (slots != USBNET_RX_SLOTS)
        kprintf("[USB-NET] #155: only %d RX slot(s) allocatable (wanted %d)\n",
                slots, USBNET_RX_SLOTS);
    uint32_t tx_pages = (tx_len + PAGE_SIZE - 1) / PAGE_SIZE;
    // #549: the TX DMA ring is the source handed to the HC; tx_buf stays as the
    // staging buffer where the ASIX vendor header is built.
    uint32_t ring_bytes = tx_len * USBNET_TX_RING_SLOTS;
    uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t tx_phys = pmm_alloc_pages(tx_pages);
    uint64_t ring_phys = pmm_alloc_pages(ring_pages);
    if (!rx_phys || !tx_phys || !ring_phys) {
        kprintf("[USB-NET] DMA buffer allocation failed\n");
        return -1;
    }
    d->rx_ring = (uint8_t *)rx_phys;
    d->rx_buf = (uint8_t *)rx_phys;        // slot 0
    d->rx_buf_len = rx_len;
    d->rx_slots = slots;
    d->rx_qlen = 1;                        // depth 1 until the reaper ramps it
    d->rx_sub_seq = 0;
    d->rx_reap_seq = 0;
    d->rx_resync = 0;
    d->tx_buf = (uint8_t *)tx_phys;
    d->tx_buf_len = tx_len;
    d->tx_ring = (uint8_t *)ring_phys;
    d->tx_ring_slots = USBNET_TX_RING_SLOTS;
    d->tx_ring_next = 0;
    return 0;
}

int usbnet_config_bulk_eps(usbnet_dev_t *d, int ep_in, int in_mps,
                           int ep_out, int out_mps) {
    int in_dci = xhci_configure_endpoint_ep(d->xhc, d->slot_id, ep_in,
                                            EP_TYPE_BULK_IN, in_mps, 0, d->speed);
    if (in_dci < 0) return -1;
    int out_dci = xhci_configure_endpoint_ep(d->xhc, d->slot_id, ep_out,
                                             EP_TYPE_BULK_OUT, out_mps, 0, d->speed);
    if (out_dci < 0) return -1;
    d->in_dci = in_dci;
    d->out_dci = out_dci;
    d->in_mps = in_mps;
    d->out_mps = out_mps;
    return 0;
}

// Diagnostics: log the outcome of the first few TX/RX transfers to the boot
// log (real-hardware debugging aid; silent afterwards).
// #745 (task #69): g_tx_diag / g_rx_diag are gone with the bootlog_write()
// calls they throttled. Both of those lines were emitted from under net_lock()
// (usbnet_bulk_out on the TX side, usb_eth_receive on the RX drain), where a
// device write is a machine-wide stall.

// #745 (task #69): TX diagnostics as COUNTERS, not as log lines. usbnet_bulk_out
// runs under net_lock (interrupts off) and must not enter the storage stack; see
// the comment at the failure path below. Read off-path by [NETSTARVE] / [HB].
uint64_t g_usbnet_tx_submits      = 0;
uint64_t g_usbnet_tx_submit_fails = 0;
uint64_t g_usbnet_rx_completions  = 0;

// #549: ASYNCHRONOUS (fire-and-forget). Submit the frame's TD onto the bulk-OUT
// ring and RETURN, without waiting for the completion. See the file header for
// why waiting here is impossible (net_lock == interrupts off + pre-scheduler
// boot DHCP: a blocking wait would deadlock, a busy-poll pegs a core and holds
// interrupts off for its whole budget). timeout_ms is retained only for
// call-site/ABI compatibility; there is no wait.
int usbnet_bulk_out(usbnet_dev_t *d, const void *data, uint32_t len,
                    uint32_t timeout_ms, int send_zlp) {
    (void)timeout_ms;
    if (!d->active) return -1;
    if (len > d->tx_buf_len) return -1;
    if (!d->tx_ring || d->tx_ring_slots <= 0) return -1;

    // Non-blocking reap of any finished TX completion so neither the shared
    // event ring nor the OUT transfer ring backs up. This does NOT wait: it
    // drains whatever has already landed and returns at once.
    uint32_t scrap = 0;
    (void)xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci, &scrap, 0);

    // Rotate to the next DMA slot so this frame does not clobber a buffer whose
    // TD may still be in flight. usb_eth_send builds ASIX headers into d->tx_buf
    // (staging); ECM passes the raw frame. Either way we copy into the ring slot
    // that becomes the HC's DMA source, then advance.
    uint8_t *slot = d->tx_ring + (uint32_t)d->tx_ring_next * d->tx_buf_len;
    d->tx_ring_next = (d->tx_ring_next + 1) % d->tx_ring_slots;
    memcpy(slot, data, len);

    if (xhci_int_in_submit(d->xhc, d->slot_id, d->out_dci,
                           (uint64_t)slot, len) != 0) {
        // #745 (task #69): DO NOT LOG FROM HERE. This function runs under
        // net_lock() (interrupts off), and bootlog_write()'s flush goes
        // through fat/ext2 -> blk_write -> usb_msc_transport -> msc_cmd_lock,
        // whose no-block fallback is an UNBOUNDED wait that a cli'd context
        // cannot be preempted out of: one TX failure could stop the machine.
        // bootlog.c now refuses the device write from a no-block context
        // (the mechanism fix), and this call site records a COUNTER instead
        // of a line (the instance fix), so the net_lock hold stays a memcpy
        // and a TRB write. The count is the information; it is reported off
        // this path on the [NETSTARVE] serial line and the [HB] record.
        g_usbnet_tx_submit_fails++;
        return -1;
    }
    if (send_zlp) {
        // Terminating zero-length packet for frames that are an exact multiple
        // of the endpoint max packet size. Queue it right behind the data TD on
        // the same ring; the HC processes both in order. Fire-and-forget.
        (void)xhci_int_in_submit(d->xhc, d->slot_id, d->out_dci,
                                 (uint64_t)slot, 0);
    }
    // #745 (task #69): the old "TX #n submitted async" lines are removed for
    // the same reason as the failure line above, and they carried no
    // information that g_nic_tx_ok (net/net.c, already counted and already on
    // the heartbeat) does not carry more cheaply.
    g_usbnet_tx_submits++;
    return 0;   // submitted; completion is reaped lazily (Ethernet is lossy)
}

// =============================================================================
// #155: RX PIPELINING - keep several bulk-IN TDs queued on the endpoint
// =============================================================================
//
// THE DEFECT. Before this, exactly ONE bulk-IN TD existed and it was re-armed
// only inside usb_eth_receive(), which runs from net_poll() at ~85 Hz. Between
// the completion of one transfer and the next net_poll pass the endpoint had
// NOTHING queued, so the host controller did not ask the device for data at all.
// MEASURED on an AX88772B: 374 KB/s of a 100 Mbit link, from ~4.4 KB captured
// per transfer x 85 transfers/s. That is a ~3% duty cycle, and it is a
// PIPELINING limit: a bigger buffer cannot fix it, and did not (usbagg peaked at
// 13662 against a 16384 buffer, i.e. the buffer was never the thing that filled).
//
// THE FIX has two halves, and BOTH are needed:
//   (a) N TDs queued, so when one retires the next is already the controller's
//       current TD and the device is drained continuously;
//   (b) a dedicated reaper thread, so the queue is refilled on the xHCI event
//       ring's own cadence instead of the 85 Hz net_poll tick.
//
// WHY A COMPLETION RING AND NOT xhci_int_in_poll(). xhci.c records completions
// in g_xfer_cc[slot][dci], ONE CELL per endpoint. That is exactly right for
// every other driver in the tree, all of which keep one TD outstanding, and it
// is structurally unusable here: the second completion overwrites the first and
// its residual, so both the byte count and the fact that a transfer happened are
// lost. xhci.c grew ONE optional hook (xhci_xfer_observer) for this; the queue
// itself lives here, in the one driver that needs it.
//
// TDs on a transfer ring retire IN ORDER, so a single monotonic sequence number
// identifies both the buffer a completion belongs to (rx_reap_seq % rx_slots)
// and the buffer to re-arm (rx_sub_seq % rx_slots). Submission leads reaping by
// exactly rx_qlen, so a buffer is never handed back to the controller while its
// contents are still being de-framed.

// Completion ring, filled by the observer (which runs inside the event drainer)
// and drained by usbnet_rx_service(). At most rx_qlen <= USBNET_RX_SLOTS
// completions can be outstanding at once, because that is how many TDs exist,
// so at 2x USBNET_RX_SLOTS this ring CANNOT overflow. The overflow branch is
// kept anyway, counted, and treated as a resync rather than ignored: a lost
// completion would slide the buffer sequence by one and de-frame the wrong
// buffer forever after, which is silent RX corruption, not a dropped packet.
#define USBNET_RXC_SLOTS  64   // power of two, >= 2 * USBNET_RX_SLOTS
static volatile uint8_t  s_rxc_cc[USBNET_RXC_SLOTS];
static volatile uint32_t s_rxc_resid[USBNET_RXC_SLOTS];
static volatile uint32_t s_rxc_head = 0;   // consumer (usbnet_rx_service)
static volatile uint32_t s_rxc_tail = 0;   // producer (the observer)

// #155: RX SERVICE TOKEN, and why it is a token rather than a held lock.
//
// TWO contexts can service the queue: the reaper thread (interrupts on) and
// net_poll()'s inline fallback (net_lock held, interrupts OFF). They share more
// than the completion ring - they share the AX88772 de-framer's split-frame
// carry state (s_rx_have/s_rx_need/s_rx_part in usb_asix.c), which is file-
// static because there has only ever been one caller. Two concurrent de-frames
// would corrupt it.
//
// Holding a lock across the whole service pass would fix that and create a
// worse problem: the pass parses up to a full queue of 16 KiB buffers, and
// net_poll() would spin on it with interrupts off for the duration. That is the
// #381/#549 freeze class, i.e. the thing [NETSTARVE] holdmax exists to catch.
//
// So the pass is guarded by a TRY-token instead: whoever gets it services,
// whoever does not RETURNS IMMEDIATELY having done nothing. Missing a pass
// costs the inline fallback nothing (the reaper is already doing the work and
// the frames land in the FIFO it is about to pop), and it can never make a
// context with interrupts off wait for a context that has them on.
//
// rx_lock is held only across the token test-and-set and the resync flag: a
// handful of instructions, never across a de-frame, a submit or a log.
// LOCK ORDER: net_lock -> rx_lock -> fifo_lock; the reaper takes rx_lock and
// fifo_lock only, so there is no cycle.
//
// The token holder EXCLUSIVELY owns: s_rxc_head, rx_sub_seq, rx_reap_seq, the
// de-framer carry state, and bulk-IN TD submission. The observer is the only
// producer and touches only s_rxc_tail, so the ring is single-producer /
// single-consumer with no lock on the producer side at all.
static spinlock_t rx_lock = SPINLOCK_INIT;
static volatile int s_rx_busy = 0;

// Returns 1 if the caller now owns the service token.
static int usbnet_rx_try_claim(usbnet_dev_t *d, int allow_resync) {
    uint64_t fl = spinlock_acquire_irqsave(&rx_lock);
    if (s_rx_busy || (d->rx_resync && !allow_resync)) {
        spinlock_release_irqrestore(&rx_lock, fl);
        return 0;
    }
    s_rx_busy = 1;
    spinlock_release_irqrestore(&rx_lock, fl);
    return 1;
}

static void usbnet_rx_release(void) {
    uint64_t fl = spinlock_acquire_irqsave(&rx_lock);
    s_rx_busy = 0;
    spinlock_release_irqrestore(&rx_lock, fl);
}

uint64_t g_usbnet_rx_reaped   = 0;   // bulk-IN completions processed
uint64_t g_usbnet_rxc_drops   = 0;   // completions lost to a full ring (see above)
uint64_t g_usbnet_rx_resyncs  = 0;   // full pipe restarts (endpoint error/lost cc)
uint64_t g_usbnet_rx_dry      = 0;   // service passes that found the queue empty
uint64_t g_usbnet_rx_refills  = 0;   // passes that had to top the queue back up

// Submit ONE bulk-IN TD into the ring slot the submit sequence is pointing at.
// Caller holds rx_lock (or is the single-threaded start/restart path).
static int usbnet_rx_submit_one(usbnet_dev_t *d) {
    if (d->rx_slots <= 0) return -1;      // never modulo by zero
    uint32_t idx = d->rx_sub_seq % (uint32_t)d->rx_slots;
    uint8_t *buf = d->rx_ring + (uint64_t)idx * d->rx_buf_len;
    int r = xhci_int_in_submit(d->xhc, d->slot_id, d->in_dci,
                               (uint64_t)buf, d->rx_buf_len);
    if (r == 0) d->rx_sub_seq++;
    return r;
}

// Kept under its historical name so usb_eth_start() reads unchanged: arm the
// queue to its current depth.
static int usbnet_rx_submit(usbnet_dev_t *d) {
    int armed = 0;
    while ((int)(d->rx_sub_seq - d->rx_reap_seq) < d->rx_qlen) {
        if (usbnet_rx_submit_one(d) != 0) break;
        armed++;
    }
    return armed ? 0 : -1;
}

// #155: THE OBSERVER. Runs inside xhci_drain_events() with the event-ring lock
// held, possibly with interrupts off and possibly from an MSI handler. It does
// one bounds check and two stores. Nothing else is permitted here (see the
// contract on xhci_xfer_observer in xhci.h).
static void usbnet_xfer_observer(uint32_t slot, uint32_t dci,
                                 uint8_t cc, uint32_t residual) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || (int)slot != d->slot_id || (int)dci != d->in_dci) return;
    uint32_t t = s_rxc_tail;
    if ((uint32_t)(t - s_rxc_head) >= USBNET_RXC_SLOTS) {
        g_usbnet_rxc_drops++;
        d->rx_resync = 1;      // see the ring-size note above: never silent
        return;
    }
    s_rxc_cc[t & (USBNET_RXC_SLOTS - 1)] = cc;
    s_rxc_resid[t & (USBNET_RXC_SLOTS - 1)] = residual;
    __asm__ volatile("" ::: "memory");   // publish payload before the index
    s_rxc_tail = t + 1;
}

// Non-blocking, side-effect-free test: legal as a wait-queue condition.
static int usbnet_rxc_pending(void) { return s_rxc_tail != s_rxc_head; }

// Reap up to `budget` completed bulk-IN TDs: de-frame each into the frame FIFO
// and re-arm its buffer. Returns the number reaped.
//
// SAFE FROM BOTH CONTEXTS. The reaper thread calls it with interrupts on; the
// pre-scheduler boot path and net_poll()'s empty-FIFO fallback call it under
// net_lock with interrupts off. Nothing in here blocks: the de-frame is a pure
// parse, the submit is a TRB write plus a doorbell, and the only lock taken is
// rx_lock (held for a handful of instructions at a time, never across the
// de-frame). Endpoint RECOVERY is deliberately NOT done here - it issues
// command-ring operations that block - it is flagged and left to the reaper.
static int usbnet_rx_service(usbnet_dev_t *d, int budget) {
    if (!d->active || !d->started || d->rx_slots <= 0) return 0;
    if (!usbnet_rx_try_claim(d, 0)) return 0;      // someone else is servicing
    xhci_poll_events(d->xhc);          // drain -> the observer fills the ring
    int n = 0;
    while (n < budget && usbnet_rxc_pending()) {
        uint32_t h = s_rxc_head;
        uint8_t  cc = s_rxc_cc[h & (USBNET_RXC_SLOTS - 1)];
        uint32_t rs = s_rxc_resid[h & (USBNET_RXC_SLOTS - 1)];
        s_rxc_head = h + 1;
        uint32_t idx = d->rx_reap_seq % (uint32_t)d->rx_slots;
        d->rx_reap_seq++;
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            // xHCI 4.10.2.1: this endpoint is now HALTED and every TD still
            // queued behind it is dead. Only a full restart fixes it, and that
            // blocks, so it is flagged here and done on the reaper thread.
            d->rx_resync = 1;
            break;
        }
        uint32_t got = (rs <= d->rx_buf_len) ? (d->rx_buf_len - rs) : 0;
        uint8_t *buf = d->rx_ring + (uint64_t)idx * d->rx_buf_len;
        if (got > 0) {
            g_usbnet_rx_completions++;
            switch (d->type) {
                case USBNET_TYPE_ECM:
                    // One transfer == one Ethernet frame (short/ZLP terminated).
                    usbnet_fifo_push(buf, got);
                    break;
                case USBNET_TYPE_AX88772:
                case USBNET_TYPE_AX88179:
                    usb_asix_rx_fixup(d, buf, got);
                    break;
            }
        }
        usbnet_rx_submit_one(d);       // re-arm; keeps the depth constant
        g_usbnet_rx_reaped++;
        n++;
    }
    if (n == 0) g_usbnet_rx_dry++;
    // SELF-HEAL THE DEPTH. usbnet_rx_submit_one() advances the submit sequence
    // only when the TRB actually went onto the ring, so a transient enqueue
    // failure permanently shortens the queue by one - and repeated often enough
    // it drains to zero, at which point RX stops dead with no error anywhere.
    // Topping up here makes the depth a target the pipe converges back to
    // rather than a count that can only go down.
    if ((int)(d->rx_sub_seq - d->rx_reap_seq) < d->rx_qlen) {
        g_usbnet_rx_refills++;
        usbnet_rx_submit(d);
    }
    usbnet_rx_release();
    return n;
}

// Full pipe restart. BLOCKS (command ring), so this runs ONLY on the reaper
// thread. Recovers the halted endpoint, throws away every half-assembled frame
// and every stale completion, and re-arms the whole queue from a clean ring.
// xhci_set_tr_dequeue() rewinds the transfer ring to index 0 / cycle 1, exactly
// what a freshly initialised ring looks like, so restarting the sequence
// counters at 0 is not an approximation, it is the matching state.
#define USBNET_RX_MAX_RESYNCS 32
static void usbnet_rx_restart(usbnet_dev_t *d) {
    static uint32_t s_resyncs = 0;
    if (s_resyncs >= USBNET_RX_MAX_RESYNCS) { d->rx_resync = 0; return; }
    s_resyncs++;
    g_usbnet_rx_resyncs++;
    // Take the service token FIRST, and keep it across the (blocking) endpoint
    // recovery. That is safe precisely because it is a token and not a held
    // spinlock: an inline fallback that cannot claim it returns immediately
    // instead of spinning, so nothing waits on us while the command ring works.
    // Claiming it is also what proves no other context is mid-service, i.e. that
    // nobody is about to enqueue a TRB onto the ring we are resetting.
    // ONE attempt, no retry loop. The only other token holder is net_poll()'s
    // inline fallback, which holds it for a single bounded completion; if it has
    // it right now, this returns and the reaper's next pass (at most one timer
    // tick away) tries again. A `while (!claim) proc_sleep(1)` here would be a
    // hand-rolled poll, which #426 bans and the concurrency lint fails the build
    // over - and it would buy nothing, because the caller is already a loop with
    // a wait-queue sleep in it.
    if (!usbnet_rx_try_claim(d, 1)) { s_resyncs--; g_usbnet_rx_resyncs--; return; }
    (void)xhci_recover_endpoint(d->xhc, d->slot_id, d->in_dci);
    s_rxc_head = s_rxc_tail;           // discard every stale completion
    d->rx_sub_seq = 0;
    d->rx_reap_seq = 0;
    usb_asix_rx_reset();               // a gap makes a half-frame unusable
    usbnet_rx_submit(d);
    d->rx_resync = 0;
    usbnet_rx_release();
    bootlog_write("[USB-NET] #155: bulk-IN pipe restarted (resync %u/%u, "
                  "depth %d)", s_resyncs, (unsigned)USBNET_RX_MAX_RESYNCS,
                  d->rx_qlen);
}

// #155: the dedicated bulk-IN reaper.
//
// WAKE SOURCES, redundant by construction (CLAUDE.md preference 1):
//   (a) the xHCI MSI handler, which wakes g_xhci_evt_wq on every interrupt -
//       microsecond latency, but DEFAULT-OFF on this kernel (/XHCIMSI.OFF);
//   (b) xhci_evt_worker() and usb_hid_poll_worker(), which drain the event ring
//       periodically and wake the same queue unconditionally on every pass;
//   (c) a one-tick timeout on the wait itself.
// So the queue is refilled at worst once per timer tick even on hardware that
// never interrupts, and immediately when it does. There is no busy-poll here and
// no hand-rolled sleep: the timeout is the third leg of an already-armed wake,
// not a substitute for one.
//
// The MSI-sequence half of the condition is the #139 idiom: the ISR does not
// drain (it must not), so a completion recorded by nobody would leave the
// condition false; comparing xhci_msi_seq() against the value we last acted on
// turns "an interrupt arrived" into a wake reason without draining anything
// inside the wait macro.
#define USBNET_RX_BACKSTOP_MS 1        // rounds up to exactly one timer tick

static void usbnet_rx_worker(void *arg) {
    (void)arg;
    usbnet_dev_t *d = &g_usbnet;
    wait_queue_head_t *evtq = (wait_queue_head_t *)xhci_event_waitq();

    // #156-style ESP lever, resolved HERE and not at attach, because attach runs
    // during xHCI enumeration when the boot volume is not necessarily mounted.
    // By the time this thread runs, it is. /USBNETQ.OFF pins the queue at depth
    // 1, which is byte-for-byte the pre-#155 behaviour, so the owner can undo
    // this change on the real machine with no toolchain.
    int deep = 1;
    extern fat_fs_t g_fat_fs;
    if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/USBNETQ.OFF")) {
        deep = 0;
        bootlog_write("[USB-NET] #155: /USBNETQ.OFF present -> bulk-IN queue "
                      "pinned at depth 1 (pre-#155 behaviour)");
    }
    int announced = 0;

    for (;;) {
        // The device is gone (unplugged) or not yet (re)started. Do NOT service
        // its ring: the slot is disabled and the buffers are being reset by a
        // concurrent re-attach on the net worker. Fall through to the SAME
        // wait_event_timeout the loop already uses, so this is a skipped pass,
        // not a new wait and not a spin.
        if (!d->active || !d->started) {
            uint64_t idle_seen = xhci_msi_seq();
            (void)wait_event_timeout(evtq,
                                     (d->active && d->started) ||
                                     xhci_msi_seq() != idle_seen,
                                     wq_ms_to_ticks(USBNET_RX_BACKSTOP_MS));
            continue;
        }
        if (d->rx_resync) usbnet_rx_restart(d);
        // Ramp to full depth as a ONE-SHOT inside the normal loop, so a token
        // held by the inline fallback costs one more pass rather than a retry
        // loop. Until it succeeds the queue runs at depth 1, which is exactly
        // the pre-#155 behaviour, so a failed ramp is a slowdown and never a
        // broken NIC.
        if (deep && d->rx_qlen != d->rx_slots && usbnet_rx_try_claim(d, 0)) {
            d->rx_qlen = d->rx_slots;
            usbnet_rx_submit(d);       // arm the rest of the queue
            usbnet_rx_release();
        }
        if (!announced && (!deep || d->rx_qlen == d->rx_slots)) {
            announced = 1;
            bootlog_write("[USB-NET] #155: bulk-IN reaper running, %d TD(s) in "
                          "flight, %u bytes each, %ums backstop", d->rx_qlen,
                          d->rx_buf_len, (unsigned)USBNET_RX_BACKSTOP_MS);
            kprintf("[USB-NET] bulk-IN reaper: depth %d x %u bytes\n",
                    d->rx_qlen, d->rx_buf_len);
        }
        // Budget = one full queue plus one, so a single pass can empty the whole
        // in-flight set and still notice a completion that landed during it.
        usbnet_rx_service(d, d->rx_qlen + 1);
        uint64_t seen = xhci_msi_seq();
        (void)wait_event_timeout(evtq,
                                 usbnet_rxc_pending() || xhci_msi_seq() != seen,
                                 wq_ms_to_ticks(USBNET_RX_BACKSTOP_MS));
    }
}

static int g_usbnet_rx_worker_started = 0;

void usb_eth_start_rx_worker(void) {
    if (g_usbnet_rx_worker_started) return;
    if (!g_usbnet.active) return;
    g_usbnet_rx_worker_started = 1;
    proc_create("usbnet_rx", usbnet_rx_worker, NULL, PRIO_NORMAL);
}

// =============================================================================
// Backend API for net/net.c
// =============================================================================

int usb_eth_present(void) {
    return g_usbnet.active;
}

// Does the USB NIC live on this slot? Asked by xhci_teardown_port_slots(),
// which must know which of its slots belong to which class driver.
int usb_net_owns_slot(int slot_id) {
    return g_usbnet.active && g_usbnet.slot_id == slot_id;
}

// UNPLUG. Without this a replug is silently ignored: usb_net_probe() opens with
// `if (g_usbnet.active) return 0`, so the NEW device is claimed by nobody, and
// net_has_nic() still answers 1, so nothing would arm even if it were.
// g_usbnet also keeps pointing at a slot the controller has disabled.
//
// Runs on the xhci_rescan worker, from the ONE place a slot is freed, so it
// cannot be missed by a future caller (the same argument xhci.c makes for
// usb_hid_detach_slot). It clears the device flags and ARMS the unbind; the
// unbind itself happens on net_worker, like the attach, so nothing about the
// network stack is touched from inside USB teardown.
void usb_net_detach_slot(int slot_id) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || d->slot_id != slot_id) return;
    d->active  = 0;     // stops the RX reaper servicing a dead slot
    d->started = 0;     // a replug re-runs usb_eth_start()
    d->link    = 0;
    extern void netattach_on_detach_rs(void);
    netattach_on_detach_rs();
    kprintf("[USB-NET] NIC unplugged (slot %d)\n", slot_id);
    bootlog_write("[NETATTACH] USB NIC unplugged (slot %d); unbind armed", slot_id);
}

const char *usb_eth_name(void) {
    switch (g_usbnet.type) {
        case USBNET_TYPE_ECM:     return "CDC-ECM";
        case USBNET_TYPE_AX88772: return "AX88772";
        case USBNET_TYPE_AX88179: return "AX88179";
        default:                  return "none";
    }
}

int usb_eth_start(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return -1;
    if (d->started) return 0;
    // Quiet the per-transfer-event serial spam: from here on RX/TX completions
    // arrive continuously (the HID poll worker sets this too, but the NIC may
    // start first when DHCP runs during boot).
    xhci_iso_quiet = 1;
    // #362: the ASIX de-framer carries split-frame state across transfers;
    // clear it whenever the pipe (re)starts so bytes from either side of a gap
    // can never be glued into one bogus frame.
    usb_asix_rx_reset();
    // #155: install the completion observer BEFORE the first submit, or the
    // very first transfer's completion is recorded nowhere and the queue is
    // stuck at zero in flight forever. Ordering, not decoration.
    xhci_xfer_observer = usbnet_xfer_observer;
    d->started = 1;
    if (usbnet_rx_submit(d) != 0) {
        kprintf("[USB-NET] initial bulk-IN submit failed\n");
        d->started = 0;
        xhci_xfer_observer = 0;
        return -1;
    }
    kprintf("[USB-NET] %s started: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            usb_eth_name(), d->mac[0], d->mac[1], d->mac[2],
            d->mac[3], d->mac[4], d->mac[5]);
    bootlog_write("[USB-NET] %s active as NIC, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                  usb_eth_name(), d->mac[0], d->mac[1], d->mac[2],
                  d->mac[3], d->mac[4], d->mac[5]);
    return 0;
}

void usb_eth_get_mac(uint8_t *mac) {
    if (mac) memcpy(mac, g_usbnet.mac, 6);
}

// #381: CHEAP, NON-BLOCKING carrier read. Returns the CACHED link state only,
// never issuing a USB control transfer on the caller's thread. nic_link_up() /
// net_is_up() are called from the compositor + net_poll hot path (the latter
// under net_lock == interrupts off); a cable-less ASIX PHY read there stalled
// the whole UI ~3s per cycle. The actual (possibly slow) PHY read now happens
// exclusively on the background net worker via usb_eth_poll_link().
int usb_eth_link_up(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return 0;
    if (d->type == USBNET_TYPE_ECM) return 1;   // no PHY to poll; bus-powered link
    return d->link;                             // cached; refreshed by net worker
}

// #381: active PHY link poll, called ONLY by the background net worker
// (net/net.c). Does the real double-read PHY query OFF the UI/net_poll path and
// updates the cached d->link that usb_eth_link_up() returns.
int usb_eth_poll_link(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return 0;
    if (d->type == USBNET_TYPE_ECM) { d->link = 1; return 1; }
    return usb_asix_poll_link(d);
}

int usb_eth_send(const void *data, uint16_t length) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || !data || length == 0) return -1;
    if (!d->started && usb_eth_start() != 0) return -1;

    switch (d->type) {
        case USBNET_TYPE_ECM: {
            int zlp = ((length % d->out_mps) == 0);
            return usbnet_bulk_out(d, data, length, 40, zlp);
        }
        case USBNET_TYPE_AX88772: {
            // 4-byte header: u16 length (LE), u16 ~length. If the padded
            // transfer would be an exact multiple of the max packet size,
            // append the magic pad marker instead of a ZLP (per the chip's
            // framing; matches the Linux asix driver's tx_fixup).
            uint32_t len = length;
            if (len + 4 > d->tx_buf_len) return -1;
            d->tx_buf[0] = (uint8_t)(len & 0xFF);
            d->tx_buf[1] = (uint8_t)((len >> 8) & 0xFF);
            d->tx_buf[2] = (uint8_t)(~len & 0xFF);
            d->tx_buf[3] = (uint8_t)((~len >> 8) & 0xFF);
            memcpy(d->tx_buf + 4, data, len);
            uint32_t total = len + 4;
            if ((total % d->out_mps) == 0 && total + 4 <= d->tx_buf_len) {
                d->tx_buf[total + 0] = 0x00;
                d->tx_buf[total + 1] = 0x00;
                d->tx_buf[total + 2] = 0xFF;
                d->tx_buf[total + 3] = 0xFF;
                total += 4;
            }
            return usbnet_bulk_out(d, d->tx_buf, total, 40, 0);
        }
        case USBNET_TYPE_AX88179: {
            // 8-byte header: u32 packet length (LE), u32 flags (0).
            uint32_t len = length;
            if (len + 8 > d->tx_buf_len) return -1;
            d->tx_buf[0] = (uint8_t)(len & 0xFF);
            d->tx_buf[1] = (uint8_t)((len >> 8) & 0xFF);
            d->tx_buf[2] = (uint8_t)((len >> 16) & 0xFF);
            d->tx_buf[3] = 0;
            memset(d->tx_buf + 4, 0, 4);
            memcpy(d->tx_buf + 8, data, len);
            uint32_t total = len + 8;
            int zlp = ((total % d->out_mps) == 0);
            return usbnet_bulk_out(d, d->tx_buf, total, 40, zlp);
        }
    }
    return -1;
}

int usb_eth_receive(void *buffer, uint16_t buffer_size) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || !buffer) return 0;
    if (!d->started && usb_eth_start() != 0) return 0;

    // #155: FAST PATH. Once the reaper thread is running it has already
    // de-framed everything into the FIFO off this path, so net_poll()'s RX
    // drain becomes a memcpy under net_lock instead of an event-ring drain plus
    // a 16 KiB parse. That is a strict REDUCTION in what net_lock covers with
    // interrupts off, which is why [NETSTARVE] holdmax must not regress.
    int n = usbnet_fifo_pop((uint8_t *)buffer, buffer_size);
    if (n > 0) return n;

    // FIFO empty. Service ONE completion inline. This is both the pre-scheduler
    // path (boot DHCP runs before the reaper thread exists) and the redundant
    // second service source afterwards, so a wedged reaper degrades to exactly
    // the pre-#155 behaviour rather than to a dead NIC. Bounded to one
    // completion so this can never become a long net_lock hold (#381/#549).
    // Endpoint recovery is NOT attempted here: it blocks, and this context
    // cannot. usbnet_rx_service() flags it and the reaper does it.
    usbnet_rx_service(d, 1);

    // Lazy PHY link refresh for ASIX parts (cheap counter-gated inside).
    if (d->type != USBNET_TYPE_ECM) usb_asix_refresh_link(d);

    return usbnet_fifo_pop((uint8_t *)buffer, buffer_size);
}

// =============================================================================
// CDC-ECM attach
// =============================================================================

// GET_DESCRIPTOR(STRING) -> parse 12 UTF-16LE hex digits into a MAC address.
static int ecm_read_mac_string(xhci_controller_t *xhc, int slot_id,
                               int str_idx, uint8_t *mac) {
    static uint8_t sbuf[64] __attribute__((aligned(64)));
    if (str_idx <= 0) return -1;
    memset(sbuf, 0, sizeof(sbuf));
    int cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                   (uint16_t)((0x03 << 8) | str_idx), 0x0409,
                                   sbuf, sizeof(sbuf));
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    int blen = sbuf[0];
    if (blen > (int)sizeof(sbuf)) blen = sizeof(sbuf);
    int mi = 0, nhex = 0;
    uint8_t val = 0;
    for (int i = 2; i + 1 < blen && mi < 6; i += 2) {
        char c = (char)sbuf[i];
        int h;
        if (c >= '0' && c <= '9') h = c - '0';
        else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
        else if (c == ':' || c == '-') continue;   // tolerate separators
        else return -1;
        val = (uint8_t)((val << 4) | h);
        if (++nhex == 2) { mac[mi++] = val; val = 0; nhex = 0; }
    }
    return (mi == 6) ? 0 : -1;
}

// Does this configuration descriptor contain a CDC-ECM control interface?
static int cfg_find_ecm_iface(uint8_t *cfg, int total) {
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9 &&
            cfg[i + 5] == 0x02 && cfg[i + 6] == 0x06) {
            return cfg[i + 2];   // bInterfaceNumber
        }
        i += blen;
    }
    return -1;
}

// cfg_idx is the GET_DESCRIPTOR configuration INDEX this descriptor came from
// (0 = the one enumeration already fetched). It exists only so the durable log
// can say which configuration was bound; nothing branches on it.
int usb_ecm_attach_cfg_idx(xhci_controller_t *xhc, int slot_id, int speed,
                           uint8_t *cfg, int total, int cfg_idx) {
    int ctrl_if = cfg_find_ecm_iface(cfg, total);
    if (ctrl_if < 0) return -1;
    int cfg_value = (total >= 6) ? cfg[5] : 1;

    // Walk the CDC functional descriptors (type 0x24): union (subtype 0x06)
    // names the data interface; the Ethernet Networking functional descriptor
    // (subtype 0x0F) carries the MAC address string index.
    int data_if = ctrl_if + 1;   // spec default if no union descriptor
    int imac = -1;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x24 && blen >= 4) {
            int subtype = cfg[i + 2];
            if (subtype == 0x06 && blen >= 5) data_if = cfg[i + 4];  // union: bSubordinateInterface0
            if (subtype == 0x0F && blen >= 13) imac = cfg[i + 3];    // ether: iMACAddress
        }
        i += blen;
    }

    // Find the data interface alternate setting that carries the bulk pair
    // (alt 0 has zero endpoints per the ECM spec; QEMU follows this).
    int alt = -1, ep_in = -1, ep_out = -1, in_mps = 0, out_mps = 0;
    i = 0;
    int cur_if = -1, cur_alt = -1;
    int t_in = -1, t_out = -1, t_in_mps = 0, t_out_mps = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {
            // close out the previous alt setting
            if (cur_if == data_if && t_in >= 0 && t_out >= 0 && alt < 0) {
                alt = cur_alt; ep_in = t_in; ep_out = t_out;
                in_mps = t_in_mps; out_mps = t_out_mps;
            }
            cur_if = cfg[i + 2];
            cur_alt = cfg[i + 3];
            t_in = t_out = -1; t_in_mps = t_out_mps = 0;
        } else if (btype == 0x05 && blen >= 7 && cur_if == data_if) {
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            if (eattr == 0x02) {   // bulk
                if (eaddr & 0x80) { t_in = eaddr; t_in_mps = emps; }
                else              { t_out = eaddr; t_out_mps = emps; }
            }
        }
        i += blen;
    }
    if (cur_if == data_if && t_in >= 0 && t_out >= 0 && alt < 0) {
        alt = cur_alt; ep_in = t_in; ep_out = t_out;
        in_mps = t_in_mps; out_mps = t_out_mps;
    }
    if (ep_in < 0 || ep_out < 0) {
        kprintf("[USB-ECM] no bulk endpoint pair on data interface %d\n", data_if);
        return -1;
    }

    kprintf("[USB-ECM] ctrl if %d data if %d alt %d bulk-in 0x%02x/%d bulk-out 0x%02x/%d cfg %d\n",
            ctrl_if, data_if, alt, ep_in, in_mps, ep_out, out_mps, cfg_value);

    usbnet_dev_t *d = &g_usbnet;
    memset(d, 0, sizeof(*d));
    d->xhc = xhc;
    d->slot_id = slot_id;
    d->speed = speed;
    d->type = USBNET_TYPE_ECM;

    // MAC address from the Ethernet functional descriptor's string.
    int mac_from_descriptor = 1;
    if (ecm_read_mac_string(xhc, slot_id, imac, d->mac) != 0) {
        mac_from_descriptor = 0;
        // Fall back to a locally administered address rather than failing:
        // a NIC with a random MAC still gets a DHCP lease.
        kprintf("[USB-ECM] iMACAddress string unreadable, using fallback MAC\n");
        d->mac[0] = 0x02; d->mac[1] = 0x36; d->mac[2] = 0x32;
        d->mac[3] = 0x4D; d->mac[4] = 0x4F; d->mac[5] = 0x53;
    }

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfg_value) < 0) {
        kprintf("[USB-ECM] SET_CONFIGURATION %d failed\n", cfg_value);
        return -1;
    }
    if (alt > 0) {
        // SET_INTERFACE(data_if, alt) selects the endpoint-bearing setting.
        int cc = xhci_control_transfer(xhc, slot_id, 0x01, 0x0B,
                                       (uint16_t)alt, (uint16_t)data_if, NULL, 0);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            kprintf("[USB-ECM] SET_INTERFACE(%d, alt %d) failed cc=%d (continuing)\n",
                    data_if, alt, cc);
        }
    }
    if (usbnet_config_bulk_eps(d, ep_in, in_mps, ep_out, out_mps) != 0) {
        kprintf("[USB-ECM] endpoint configuration failed\n");
        return -1;
    }
    if (usbnet_alloc_buffers(d, 2048, 2048) != 0) return -1;

    // SET_ETHERNET_PACKET_FILTER: directed + broadcast + all-multicast.
    // Some real ECM devices keep RX gated until this arrives; QEMU accepts it.
    (void)xhci_control_transfer(xhc, slot_id, 0x21, 0x43, 0x000F,
                                (uint16_t)ctrl_if, NULL, 0);

    d->link = 1;
    d->active = 1;
    kprintf("[USB-ECM] attached: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    bootlog_write("[USB-ECM] CDC-ECM NIC attached (slot %d), MAC %02x:%02x:%02x:%02x:%02x:%02x",
                  slot_id, d->mac[0], d->mac[1], d->mac[2],
                  d->mac[3], d->mac[4], d->mac[5]);
    // BINDING PROVENANCE, durable (no ticket, 2026-08-28).
    //
    // A composite device can expose a vendor-specific configuration AND a CDC
    // one; the Realtek RTL8156 (0bda:8156) exposes three configurations and
    // the FIRST one, which is the only one /USBLOG.TXT records, is a single
    // class 0xff interface. From the old log line alone there was no way to
    // tell whether "CDC-ECM NIC attached" meant a real CDC binding or an ECM
    // framing bolted onto a vendor data path, and those two fail identically:
    // DHCP sends and nothing ever answers.
    //
    // Everything on this line is decisive. cfgidx/cfgval say WHICH of the
    // device's configurations we selected. ctrlif is the interface that
    // matched bInterfaceClass==0x02 && bInterfaceSubClass==0x06 (Communications
    // / Ethernet Networking Control Model), which is a strict class match, not
    // an endpoint-shape guess and not a VID/PID table. mac=descriptor means the
    // MAC came from the CDC Ethernet Networking functional descriptor's
    // iMACAddress string (0x24 subtype 0x0F), which does not exist outside a
    // real CDC function; mac=FALLBACK means we could not read it and installed
    // a locally administered address, which is worth knowing before blaming
    // the network.
    bootlog_write("[USB-ECM] binding: cfgidx=%d cfgval=%d ctrlif=%d(class 02/06) "
                  "dataif=%d alt=%d in=0x%02x/%d out=0x%02x/%d mac=%s",
                  cfg_idx, cfg_value, ctrl_if, data_if, alt,
                  ep_in, in_mps, ep_out, out_mps,
                  mac_from_descriptor ? "descriptor" : "FALLBACK");
    return 0;
}

// Back-compat wrapper: the config already fetched by enumeration is index 0.
int usb_ecm_attach_cfg(xhci_controller_t *xhc, int slot_id, int speed,
                       uint8_t *cfg, int total) {
    return usb_ecm_attach_cfg_idx(xhc, slot_id, speed, cfg, total, 0);
}

// =============================================================================
// Probe entry (called from xhci_enumerate_devices)
// =============================================================================

// ASIX VID/PID match table. is179 selects the AX88179 (USB3) register model.
static const struct { uint16_t vid, pid; int is179; } asix_ids[] = {
    { 0x0B95, 0x7720, 0 },   // AX88772
    { 0x0B95, 0x772A, 0 },   // AX88772A
    { 0x0B95, 0x772B, 0 },   // AX88772B
    { 0x0B95, 0x7E2B, 0 },   // AX88772B
    { 0x05AC, 0x1402, 0 },   // Apple USB Ethernet Adapter (AX88772/A)
    { 0x2001, 0x3C05, 0 },   // D-Link DUB-E100 rev B
    { 0x0DF6, 0x061C, 0 },   // Sitecom LN-028
    { 0x17EF, 0x7203, 0 },   // Lenovo U2L100P
    { 0x0B95, 0x1790, 1 },   // AX88179
    { 0x0B95, 0x178A, 1 },   // AX88178A
    { 0x04B4, 0x3610, 1 },   // Cypress GX3 (AX88179 core)
    { 0x2001, 0x4A00, 1 },   // D-Link DUB-1312
};

// Hot-plug attach hook (no ticket, 2026-08-28). Called on EVERY path that
// successfully claims a USB NIC (CDC-ECM in the first configuration, CDC-ECM in
// a later configuration, ASIX AX88772/AX88179, and anything added later), which
// is why it lives at the ONE exit of usb_net_probe() rather than in each driver.
//
// It arms a flag and returns. It does NOT start DHCP, bind the driver or touch
// the wire: this runs on the xhci_rescan worker inside device enumeration, one
// control transfer away from every other device on the bus, and #549 is the
// recorded cost of blocking somewhere that cannot block. net_worker does the
// work, within ~1s. See the long comment in net/net.c.
static int usbnet_probe_done(int claimed) {
    extern int  netattach_on_probe_rs(int nic_bound);
    extern int  net_has_nic(void);
    if (!claimed) return 0;
    if (netattach_on_probe_rs(net_has_nic())) {
        bootlog_write("[NETATTACH] hot-plug USB NIC (%s) armed for the net worker",
                      usb_eth_name());
    }
    return 1;
}

int usb_net_probe(xhci_controller_t *xhc, int slot_id, int speed,
                  uint16_t vid, uint16_t pid,
                  uint8_t *cfg, int cfg_total, uint8_t num_configs) {
    if (g_usbnet.active) return 0;   // one USB NIC supported

    // 1) ASIX vendor match (vendor-specific class; must key on VID/PID).
    for (unsigned k = 0; k < sizeof(asix_ids) / sizeof(asix_ids[0]); k++) {
        if (asix_ids[k].vid == vid && asix_ids[k].pid == pid) {
            kprintf("[USB-NET] ASIX %s dongle %04x:%04x detected\n",
                    asix_ids[k].is179 ? "AX88179" : "AX88772", vid, pid);
            return usbnet_probe_done(usb_asix_attach(xhc, slot_id, speed, vid, pid,
                                     asix_ids[k].is179, cfg, cfg_total) == 0);
        }
    }

    // 2) CDC-ECM in the already-fetched configuration (index 0).
    if (cfg_find_ecm_iface(cfg, cfg_total) >= 0) {
        return usbnet_probe_done(usb_ecm_attach_cfg(xhc, slot_id, speed,
                                                   cfg, cfg_total) == 0);
    }

    // 3) Search the device's OTHER configurations for an ECM function.
    // QEMU's usb-net exposes RNDIS as config descriptor index 0 and the
    // CDC-ECM function in the other configuration; real combo devices do
    // similar. Only bother for Communications-class-plausible devices
    // (multi-config), and cap the search.
    if (num_configs > 1) {
        static uint8_t cfg2[512] __attribute__((aligned(64)));
        for (int ci = 1; ci < (int)num_configs && ci < 4; ci++) {
            memset(cfg2, 0, sizeof(cfg2));
            int cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                           (uint16_t)((0x02 << 8) | ci), 0,
                                           cfg2, 9);
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) continue;
            int total = cfg2[2] | (cfg2[3] << 8);
            if (total > (int)sizeof(cfg2)) total = sizeof(cfg2);
            if (total < 9) continue;
            memset(cfg2, 0, sizeof(cfg2));
            cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                       (uint16_t)((0x02 << 8) | ci), 0,
                                       cfg2, (uint16_t)total);
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) continue;
            if (cfg_find_ecm_iface(cfg2, total) >= 0) {
                kprintf("[USB-NET] CDC-ECM function found in config index %d\n", ci);
                // DURABLE: on a composite device this is the step that decides
                // we are NOT binding the vendor-specific configuration that
                // enumeration fetched. It was kprintf-only, i.e. invisible on a
                // machine with no serial port, which is every real one.
                bootlog_write("[USB-NET] %04x:%04x: config index 0 has no CDC-ECM "
                              "function; found one in config index %d of %d, "
                              "binding that", vid, pid, ci, (int)num_configs);
                return usbnet_probe_done(usb_ecm_attach_cfg_idx(xhc, slot_id, speed,
                                                                cfg2, total, ci) == 0);
            }
        }
    }
    return 0;
}
