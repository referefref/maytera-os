// serial.c - Serial port driver implementation
#include "serial.h"
#include "sync/spinlock.h"   // #67 pass 7: console serialisation
#include "sync/waitq.h"      // #745 (task #70): the drain thread's one sanctioned wait
#include "string.h"
#include "cpu/mono.h"   // #745 (task #69): the shared monotonic clock, so the
                        // UART TX wait is bounded in TIME and not in bus cycles
#include <stdarg.h>

// ---------------------------------------------------------------------------
// #745 (task #69): IS THERE A UART AT ALL?
//
// serial_init() has ALWAYS ended with a real loopback presence test (write 0xAE
// through MCR loopback, read it back, return -1 if it does not match). Its one
// caller, main.c:639, has ALWAYS discarded that return value. So the kernel has
// been able to detect an absent or dead UART since the beginning and has never
// once acted on it - the project's recurring "the detection exists and nothing
// consumes it" pattern (#624 security_init, #433 validate_user_ptr, sse_save).
//
// The consequence is not cosmetic. serial_write() polls the LSR per character,
// and on a machine with no serial port at all that poll can never succeed, so
// every character pays the full budget before being dropped. The real iMac14,4
// target HAS NO SERIAL CONSOLE (main.c's #373 note says so in as many words),
// which is exactly the machine where the freeze is reported and the VM is not.
//
// Whether an absent 0x3F8 reads back 0x00 (THRE clear -> full spin per char) or
// floats to 0xFF (THRE set -> fast path, characters silently discarded) is
// CHIPSET-DEPENDENT and cannot be settled by reasoning. So both facts are now
// recorded and carried on /HEARTBEAT.TXT, which is the only telemetry that
// machine produces: g_serial_present is the loopback verdict, and
// g_serial_lsr_boot is the RAW LSR BYTE read at init. One boot of the real
// machine answers the question outright instead of leaving it a hypothesis.
//
// Default 1 ("assume present"), because serial_write() runs before serial_init()
// during very early boot and must behave exactly as it does today until the
// probe has actually run.
volatile int      g_serial_present  = 1;
volatile uint32_t g_serial_lsr_boot = 0xFFFFFFFFu;   // 0xFFFFFFFF = never probed
volatile uint32_t g_serial_lb_read  = 0xFFFFFFFFu;   // loopback read-back byte

// Initialize serial port
int serial_init(uint16_t port, uint32_t baud) {
    uint16_t divisor = 115200 / baud;
    // Raw LSR BEFORE we touch anything. This single byte distinguishes "no
    // decoder, floats high" (0xFF) from "decoded but dead" (0x00) from a real
    // UART (typically 0x60 = THRE|TEMT idle).
    g_serial_lsr_boot = (uint32_t)inb(port + SERIAL_LSR);

    // Disable interrupts
    outb(port + SERIAL_IER, 0x00);

    // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_LCR, 0x80);

    // Set divisor (low and high bytes)
    outb(port + SERIAL_DLL, divisor & 0xFF);
    outb(port + SERIAL_DLH, (divisor >> 8) & 0xFF);

    // 8 bits, no parity, one stop bit (8N1)
    outb(port + SERIAL_LCR, 0x03);

    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + SERIAL_FIFO, 0xC7);

    // IRQs enabled, RTS/DSR set
    outb(port + SERIAL_MCR, 0x0B);

    // Set in loopback mode to test serial chip
    outb(port + SERIAL_MCR, 0x1E);

    // Test serial chip by sending byte 0xAE
    outb(port + SERIAL_DATA, 0xAE);

    // Check if serial is faulty (i.e., not same byte as sent)
    uint8_t lb = inb(port + SERIAL_DATA);
    g_serial_lb_read = (uint32_t)lb;
    if (lb != 0xAE) {
        // #745 (task #69): LATCH IT, do not just report it to a caller that has
        // never looked. serial_write() checks this and becomes a no-op, so an
        // absent UART costs one compare per character forever instead of a
        // per-character poll that can never succeed. This cannot regress a VM:
        // there the loopback test passes and nothing changes.
        g_serial_present = 0;
        outb(port + SERIAL_MCR, 0x0F);   // leave the chip out of loopback mode
        return -1;  // Serial port is faulty or absent
    }
    g_serial_present = 1;

    // If serial is not faulty, set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(port + SERIAL_MCR, 0x0F);

    return 0;
}

// Check if data is available to read
int serial_received(uint16_t port) {
    return inb(port + SERIAL_LSR) & SERIAL_LSR_DR;
}

// Check if transmit buffer is empty
int serial_is_transmit_empty(uint16_t port) {
    return inb(port + SERIAL_LSR) & SERIAL_LSR_THRE;
}

// Read a character from serial port
char serial_read(uint16_t port) {
    while (!serial_received(port));
    return inb(port + SERIAL_DATA);
}

// #745 (task #69): how often the UART refused to drain, and the longest single
// character wait. Both are read (never reset) by the [NETSTARVE] serial line and
// the [HB] record. A non-zero drop count on a machine that looks frozen is the
// tell that this loop, not the network, owned the interrupts-off window.
volatile uint64_t g_serial_tx_drops   = 0;
volatile uint64_t g_serial_tx_max_us  = 0;

// Per-character TX budget. A 115200-baud UART shifts one character out in about
// 87us and 9600 baud in about 1ms, so 2ms cannot be reached by any healthy port
// at any plausible rate; it is only ever paid by a port that is not draining.
#define SERIAL_TX_BUDGET_US          2000ULL
// Fallback before the monotonic clock is calibrated (very early boot, where
// serial is the only output and the machine is single-threaded). An iteration
// cap is a poor bound, which is the whole point of this change, but it is the
// only one available before mono_init() and 20k is 10x smaller than the old
// 200k while still being far more than a working UART ever needs.
#define SERIAL_TX_UNCALIBRATED_SPINS 20000u

// #745 (task #69): THE PER-CHARACTER BUDGET IS NOT ENOUGH ON ITS OWN.
//
// A time budget per character still multiplies by the number of characters, and
// the kernel's diagnostic volume is not something the kernel controls: icmp.c
// prints unconditionally on EVERY received ICMP packet, and net_poll() drains up
// to 64 packets per call with interrupts off. A remote host choosing to ping us
// therefore chooses how long we spend in this loop. 64 packets x 4 lines x 30
// characters x 2ms is still 15 seconds of frozen machine.
//
// So the port that is not draining is LATCHED. After a drop, every subsequent
// character is discarded on a single compare until the backoff expires and we
// probe once more. A dead UART now costs the kernel one 2ms probe per 500ms no
// matter how much it prints, which is a bound on the SUBSYSTEM rather than on
// one call. This is the same async-fetch-then-cache shape CLAUDE.md already
// mandates for the compositor, applied to the UART: do not re-ask hardware that
// just told you it is not ready.
//
// A single successful write clears the latch instantly, so a port that recovers
// (a QEMU serial socket being read again) loses at most one backoff interval.
#define SERIAL_TX_STALL_BACKOFF_US   500000ULL
static volatile uint64_t g_serial_stall_until_us = 0;
static volatile uint16_t g_serial_stall_port     = 0;
volatile uint64_t g_serial_tx_suppressed = 0;   // chars dropped by the latch

// ---------------------------------------------------------------------------
// #745 (task #70): THE UART HAS EXACTLY ONE WRITER AT A TIME.
//
// Once the console is asynchronous, the drain thread writes the port from a
// preemptible thread context with no console lock held. That is the whole
// point, and it means any OTHER synchronous writer - a kprintf issued in the
// microseconds before the async flip, the fallback flush, kprintf_nolock, or
// dos/dosexec.c's direct INT 21h console echo - would race it a character at a
// time. On the first async boot that produced exactly the interleaving #67
// pass 7 had just fixed:
//     "[C[OCNOSNOSLOEL]E ]A ScYoNnCr icnogn ssoellef -ltievset:  PkApSrSi n(trfu set)"
//
// So the mutual exclusion moves to where the device actually is. It is taken
// PER CHARACTER, not per line: at ~87 us of polled wait per character the lock
// is a rounding error, but holding it across a whole 256-byte chunk would mean
// 22 ms with interrupts off, which is the exact latency this ticket exists to
// remove. Line atomicity is still provided one layer up by g_console_lock.
//
// g_serial_lock_bypass is set by console_panic_flush(): on the way down the
// holder may be the CPU that just died, and a panic must print regardless.
// ---------------------------------------------------------------------------
static spinlock_t g_uart_lock = SPINLOCK_INIT;
volatile int      g_serial_lock_bypass = 0;

static void serial_write_locked(uint16_t port, char c);

// Write a character to serial port
void serial_write(uint16_t port, char c) {
    if (g_serial_lock_bypass) { serial_write_locked(port, c); return; }
    uint64_t fl = spinlock_acquire_irqsave(&g_uart_lock);
    serial_write_locked(port, c);
    spinlock_release_irqrestore(&g_uart_lock, fl);
}

static void serial_write_locked(uint16_t port, char c) {
    // BOUNDED wait for the UART transmit-holding register to empty. An unbounded
    // poll here hangs the ENTIRE kernel when TX can't drain: e.g. QEMU's serial
    // chardev buffer fills because nothing is reading the serial socket, so the
    // THRE bit never sets. That deterministically wedged VM <vmid>'s toram boot at
    // "Starting desktop services..." (trapped RIP inside this loop, RDX=0x3F8 /
    // RBP=0x3FD). #426 no-unbounded-busy-wait discipline: cap the spin and drop
    // the char rather than freezing the machine. When TX is healthy the very
    // first check passes, so this is behavior-identical on a draining port.
    //
    // #745 (task #69): THE CAP WAS ON ITERATIONS, WHICH IS NOT A TIME BOUND.
    //
    // The old bound was `spin < 200000`, one `inb` from the LSR per iteration.
    // An iteration count only bounds time if the iteration has a bounded cost,
    // and this one does not: a legacy port-I/O read is a bus cycle, roughly a
    // microsecond on real ISA/LPC silicon and a full VM exit under KVM. So the
    // "cap" was worth up to ~0.2 SECONDS PER CHARACTER, and kprintf emits one
    // byte at a time with no buffer. A single 60-character diagnostic line on a
    // machine whose serial TX never drains is therefore up to ~12 seconds, and
    // a handful of lines is the reported 30-second freeze.
    //
    // That matters far beyond serial: kprintf is called from inside net_lock()
    // (which holds interrupts off for its whole duration), from ISRs, and from
    // the panic path. Every one of those inherits this bound. It is the single
    // largest amplifier in the kernel's interrupts-off graph, and it is invisible
    // in a VM because QEMU's UART reports THRE on the first read, so the loop
    // exits at iteration 0 and nobody ever pays it. #426's rule is that a bound
    // must be a bound on the thing you actually care about; here that is TIME.
    //
    // The budget below is a real time budget, taken from the calibrated TSC.
    // 2 ms is over 20x the ~87us a 115200-baud UART needs to shift one
    // character out, and over 2x what 9600 baud needs, so no healthy port can
    // ever reach it; a dead port now costs 2 ms instead of 200,000 bus cycles.
    // Before the TSC is calibrated (mono_tsc_khz() == 0, very early boot) we
    // fall back to a small ITERATION cap, because at that point serial is the
    // only output we have and the machine is single-threaded anyway.
    // The clock is cpu/mono.h, the kernel's ONE monotonic clock (mono.h's own
    // header text: "DO NOT hand-roll another clock" - five subsystems already
    // grew private rdtsc helpers and that is how the tick-deadline bug family
    // spread). It is TSC-backed, calibrated before usb_init(), and documented
    // to work with interrupts off, which is exactly the context here.
    // #745 (task #69): no UART here at all (serial_init's loopback test failed).
    // One compare, then out. See the block comment on g_serial_present above.
    if (!g_serial_present) { g_serial_tx_suppressed++; return; }

    const int cal = mono_ready();
    const uint64_t t0 = cal ? mono_us() : 0;

    // Stall latch (see SERIAL_TX_STALL_BACKOFF_US above). On a healthy port
    // g_serial_stall_until_us is 0 and this is one compare against a static.
    if (g_serial_stall_until_us && g_serial_stall_port == port) {
        if (cal && t0 < g_serial_stall_until_us) { g_serial_tx_suppressed++; return; }
        g_serial_stall_until_us = 0;   // backoff expired: probe once more below
    }

    for (uint32_t spin = 0; ; spin++) {
        if (serial_is_transmit_empty(port)) {
            outb(port + SERIAL_DATA, c);
            g_serial_stall_until_us = 0;   // it drained: unlatch immediately
            // Record the wait only when we actually waited, so a healthy port
            // pays one compare and never touches the counters.
            if (spin && cal) {
                uint64_t d = mono_us() - t0;
                if (d > g_serial_tx_max_us) g_serial_tx_max_us = d;
            }
            return;
        }
        // Test the deadline every 64 polls, so reading the clock cannot
        // dominate the loop the clock is there to bound.
        if ((spin & 63u) == 63u) {
            if (cal) {
                if (mono_us() - t0 >= SERIAL_TX_BUDGET_US) break;
            } else if (spin >= SERIAL_TX_UNCALIBRATED_SPINS) {
                break;
            }
        }
    }
    // TX not draining; drop this char instead of freezing the machine, and
    // latch the port so the NEXT character costs a compare rather than another
    // full budget.
    g_serial_tx_drops++;
    if (cal) {
        uint64_t d = mono_us() - t0;
        if (d > g_serial_tx_max_us) g_serial_tx_max_us = d;
        g_serial_stall_port     = port;
        g_serial_stall_until_us = mono_us() + SERIAL_TX_STALL_BACKOFF_US;
    }
}

// Write a string to serial port
void serial_puts(uint16_t port, const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_write(port, '\r');
        }
        serial_write(port, *str++);
    }
}

// Helper function to print a number
// #672: print_number() lived here. It was the private integer formatter for
// serial.c's two hand-rolled parsers; both are gone, so it is too. The shared
// parser in string.c does this work now.

// Printf-like function for serial output
// #672: kprintf and serial_printf now share the ONE parser in string.c via a
// sink, instead of each carrying a private, weaker copy. kprintf's private copy
// parsed '0' and a numeric width but NOT the '-' flag, so every "%-13s" in the
// tree printed literally, consumed no argument, and shifted the rest of the
// line, including handing an integer to a "%s" that then dereferenced it.
//
// Emission is still one byte at a time, with NO intermediate buffer: kprintf is
// called from ISRs and from the panic path, so it must not put a large buffer on
// a possibly-nested kernel stack, and its long diagnostic lines must not be
// truncated to some buffer size. That is exactly why the shared primitive takes
// a sink rather than requiring every caller to format into a char[].
typedef struct { uint16_t port; } kfmt_serial_ctx_t;

static void kfmt_serial_sink(void *vctx, char c) {
    kfmt_serial_ctx_t *sc = (kfmt_serial_ctx_t *)vctx;
    // CR before LF, as the pre-#672 kprintf did for literal newlines. It now
    // also applies to a newline arriving via %c or %s, which is a fix: a bare
    // LF leaves a serial terminal's cursor mid-line.
    if (c == '\n') serial_write(sc->port, '\r');
    serial_write(sc->port, c);
}

void serial_printf(uint16_t port, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kfmt_serial_ctx_t ctx = { port };
    kvformat(kfmt_serial_sink, &ctx, fmt, args);
    va_end(args);
}

// #67 pass 7: SERIAL CONSOLE LOCK.
//
// There has never been one. It has never been needed, because until this ticket
// only one core ever ran kernel code, and once the #279 Big Kernel Lock existed
// it serialised every kprintf() BY ACCIDENT - not by design, and nowhere in the
// tree recorded as a reason the BKL had to be that wide.
//
// Narrowing the BKL (dropping it around the scheduler diagnostics, which were
// holding it ~10 ms at a time) removed that accidental serialisation, and two
// cores printing concurrently immediately produced interleaved output:
//     "hea[rtbDeaHtCP TwoRrC]k er 0t hevreenadt s r(unshnionwging"
// which is "[HB] heartbeat worker thread running" and a DHCP line woven together
// one character at a time. Harmless to kernel data; fatal to a serial log that
// is the only diagnostic this whole ticket runs on.
//
// A ticket spinlock taken irqsave, so the whole line is emitted as a unit. The
// panic path deliberately does NOT take it: a panic must print even if the
// holder died holding it.
//
// #745 (task #70): THE LOCK STAYS; WHAT IT COVERS GOT ~1000x SHORTER. It used
// to be held across the polled UART write, which is ~87 us PER CHARACTER, so a
// second core calling kprintf blocked for as long as this core's log line took
// to drain. It is now held only across the ring push below. Note that it is now
// a LINE-ATOMICITY lock, not the UART's mutual exclusion: that job belongs to
// g_uart_lock inside serial_write().
static spinlock_t g_console_lock = SPINLOCK_INIT;

// ===========================================================================
// #745 (task #70): THE ASYNCHRONOUS CONSOLE
// ===========================================================================
//
// THE DEFECT, as #67 measured it and #69 corroborated from the other side:
// kprintf() formats and writes SYNCHRONOUSLY to a polled 115200 UART, from
// inside interrupt handlers and while holding locks. serial_write() polls the
// THRE bit PER CHARACTER at roughly 87 us each, so a 120-character line is
// about 10 ms of busy-polling. On one core that is invisible because nobody
// waits. With a second core it serialises everything: first via the BKL, and
// after #67 pass 7 gave the console its own lock, via that lock.
//
//   build 253 (no console lock):  7 contended BKL acquires,       961 spins, maxhold     12 us
//   build 256 (console lock):    34 contended BKL acquires, 4,165,755 spins, maxhold 26,475 us
//
// The console lock was NECESSARY and it stays. The defect is not the lock, it
// is that the WRITE IS SYNCHRONOUS: a caller holding the BKL that calls kprintf
// blocks on the console lock for as long as somebody else's log line takes to
// drain.
//
// SO: kprintf() now formats into a RING BUFFER under the same short lock and
// returns. A dedicated kernel thread does the slow device writes holding no
// long-lived lock - it releases the BKL across each chunk exactly as
// sched_smp_report() and the scheduler's own context switch already do, and it
// never holds the console lock while touching the UART.
//
// The ring's index arithmetic and overflow policy live in Rust
// (rustkern/conring.rs) per the 2026-07-16 rule; the bytes live here so the
// panic path can reach them with no allocator and so that module needs no
// `static mut`. The struct layout is locked by the _Static_assert below.
//
// THE THREE NAMED RISKS AND WHERE EACH IS HANDLED
// -----------------------------------------------------------------------
// 1. PANIC-PATH ORDERING. console_panic_flush() latches the console back to
//    synchronous, sets g_serial_lock_bypass so a dead lock holder cannot stop
//    us, and drains the ring straight to the UART. It is called from
//    kpanic_halt(), which is the ONE tail every panic path reaches - both
//    kpanic() and cpu/idt.c's fault handler - so no panic can leave the buffer
//    unflushed, and kpanic() also calls it up front so its own banner is
//    synchronous.
// 2. OVERFLOW POLICY. Never block. On a full ring we enter dropping mode and
//    resume at the next line boundary once a quarter of the ring is free, and
//    the resume injects a "[CONDROP n chars lost]" marker, so a gap always
//    announces itself and carries its own size.
// 3. THE PRE-THREAD BOOT PATH. g_console_async starts at 0 and is set BY THE
//    DRAIN THREAD ITSELF, under the console lock, as its first real action.
//    Until a drain exists, kprintf is byte-for-byte the old synchronous
//    function. There is no window in which output is queued with nobody to
//    write it.
//
// AND A FOURTH, NOT IN THE BRIEF: A DRAIN THAT STOPS RUNNING WOULD BLACK-HOLE
// THE LOG. conring_stall_check_rs() asks, on a drop and with a real clock,
// whether the drain has moved a single byte in the last five seconds. If it has
// not, we flush and revert to the synchronous console for the rest of the boot.
// Slow and complete beats fast and empty.
//
// MEASURED CORRECTION, first async boot (build 1857): the first version of that
// fourth rule counted DROPPED CHARACTERS rather than time, and it fired on a
// perfectly healthy console - a producer can drop far more than 4096 characters
// in the 22 ms it takes the UART to shift one chunk, so "the kernel is
// out-running the UART right now" was being read as "nothing is draining". The
// rule is about the CONSUMER not moving, which is a statement about time.
// ===========================================================================

// Mirror of rustkern/conring.rs `ConRing`. The assert below is the lock.
typedef struct {
    uint64_t dropped;
    uint64_t drop_events;
    uint64_t pushed;
    uint64_t popped;
    uint64_t drain_seq;
    uint64_t stall_seq;
    uint64_t stall_since_us;
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    uint32_t dropping;
    uint32_t highwater;
    uint32_t stall_drops;
    uint32_t _pad;
    uint32_t _pad2;
} conring_t;
_Static_assert(sizeof(conring_t) == 88,
               "conring_t must match rustkern/conring.rs ConRing exactly");

// Result bits from conring_push_rs(). Mirror of the R_* constants in conring.rs.
#define CONRING_R_ACCEPTED  0x1u
#define CONRING_R_WAKE      0x2u
#define CONRING_R_RESUME    0x4u
#define CONRING_R_DROPPED   0x8u

extern uint32_t conring_init_rs(conring_t *st, uint32_t size);
extern uint32_t conring_push_rs(conring_t *st, uint8_t *buf, uint8_t c);
extern uint32_t conring_pop_rs(conring_t *st, const uint8_t *buf, uint8_t *out, uint32_t cap);
extern uint32_t conring_pending_rs(const conring_t *st);
extern uint32_t conring_stall_check_rs(conring_t *st, uint64_t now_us, uint64_t limit_us);
extern uint32_t conring_selftest_rs(void);

// 1 MiB. Sized from evidence, not taste. A captured boot emits roughly 300 KB
// of serial log once the kernel is no longer throttled by the UART (measured:
// 297 KB in 110 s on the async arm against 118 KB on the synchronous one, which
// is the throughput this change buys), and the port carries 11.5 KB/s. A ring
// that can hold the ENTIRE boot log means the boot never drops a line even
// while the UART is minutes behind; `highwater` is reported so the next person
// can re-size it from a measurement rather than from this paragraph.
#define CONRING_BYTES        (1024u * 1024u)
// How long the drain may move NOTHING AT ALL before we call it dead. Five
// seconds is far outside any scheduling delay on this kernel and far inside any
// useful amount of lost log.
#define CONRING_STALL_US     5000000ULL
// Bytes handed to the UART per lock-acquire/release cycle. At ~87 us/char this
// is about 22 ms of polled writing per chunk, done with the console lock
// RELEASED, the BKL RELEASED and interrupts on, so it is fully preemptible -
// which is the whole point.
#define CONRING_CHUNK        256u

static uint8_t   g_conring_buf[CONRING_BYTES];
static conring_t g_conring;

volatile int      g_console_async        = 0;
volatile int      g_console_async_wanted = 1;
volatile uint64_t g_console_fallbacks    = 0;
volatile uint64_t g_console_dropped      = 0;

// Declared locally (matches cpu/smp.h) to avoid a serial -> cpu include edge,
// the same way fs/panic.c declares bkl_release_all().
extern uint32_t smp_this_cpu(void);

static wait_queue_head_t g_console_wq;
static volatile int      g_console_wq_ready = 0;
// Re-entrancy guard for the producer-side wake. wake_up() holds the queue
// spinlock across proc_wake() -> add_to_ready_queue(), which can kprintf on its
// error paths; without this, that kprintf would call wake_up() on the SAME
// queue on the SAME cpu and self-deadlock on a non-recursive spinlock.
// Correctness does NOT depend on this flag being precise across cores: a wake
// skipped by a racing core is picked up by the redundant timed arm below.
static volatile int      g_console_wake_guard = 0;

typedef struct { uint32_t bits; } kfmt_ring_ctx_t;

// Push one byte. Caller holds g_console_lock.
static uint32_t ring_put(kfmt_ring_ctx_t *rc, char c) {
    uint32_t r = conring_push_rs(&g_conring, g_conring_buf, (uint8_t)c);
    rc->bits |= r;
    return r;
}

// A gap in the log ALWAYS announces itself and carries its own size. Formatted
// by hand rather than through kvformat, because this runs from inside the
// formatting sink and must not recurse into it.
static void ring_emit_dropmark(kfmt_ring_ctx_t *rc) {
    char m[64];
    unsigned i = 0;
    const char *p = "[CONDROP ";
    while (*p) m[i++] = *p++;
    uint64_t v = g_conring.dropped;
    char d[24];
    int nd = 0;
    if (v == 0) d[nd++] = '0';
    while (v && nd < 20) { d[nd++] = (char)('0' + (v % 10)); v /= 10; }
    while (nd) m[i++] = d[--nd];
    p = " chars lost]\r\n";
    while (*p) m[i++] = *p++;
    for (unsigned k = 0; k < i; k++)
        rc->bits |= conring_push_rs(&g_conring, g_conring_buf, (uint8_t)m[k]);
}

static void kfmt_ring_sink(void *vctx, char c) {
    kfmt_ring_ctx_t *rc = (kfmt_ring_ctx_t *)vctx;
    // CR before LF, exactly as the synchronous sink does.
    if (c == '\n') {
        if (ring_put(rc, '\r') & CONRING_R_RESUME) ring_emit_dropmark(rc);
    }
    if (ring_put(rc, c) & CONRING_R_RESUME) ring_emit_dropmark(rc);
}

// Drain the ring to the UART. `take_lock` is 0 ONLY on the panic path, where
// the lock holder may be dead. Bounded: the ring cannot hold more than
// CONRING_BYTES, so the iteration cap is a backstop, not the mechanism.
static void console_drain_all(int take_lock) {
    uint8_t chunk[CONRING_CHUNK];
    for (unsigned guard = 0; guard < (CONRING_BYTES / CONRING_CHUNK) + 8u; guard++) {
        uint32_t n;
        if (take_lock) {
            uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
            n = conring_pop_rs(&g_conring, g_conring_buf, chunk, CONRING_CHUNK);
            spinlock_release_irqrestore(&g_console_lock, fl);
        } else {
            n = conring_pop_rs(&g_conring, g_conring_buf, chunk, CONRING_CHUNK);
        }
        if (!n) return;
        for (uint32_t i = 0; i < n; i++) serial_write(COM1, (char)chunk[i]);
    }
}

// The consumer stopped moving bytes for CONRING_STALL_US. Get everything out
// and never queue again this boot. Runs in whatever context noticed, which may
// be an ISR with interrupts off - that is exactly the old synchronous
// behaviour, and it is the correct last resort: an unflushed buffer is worse
// than a slow one. serial_write() serialises against the drain thread
// internally, so the worst this can do is split a line, not corrupt one.
static void console_async_fallback(void) {
    if (!g_console_async) return;
    uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
    if (!g_console_async) { spinlock_release_irqrestore(&g_console_lock, fl); return; }
    g_console_async = 0;
    spinlock_release_irqrestore(&g_console_lock, fl);
    g_console_fallbacks++;
    console_drain_all(1);
    kprintf_nolock("[CONDRAIN] the drain moved no bytes for %lu us; reverting to "
                   "SYNCHRONOUS console for the rest of this boot "
                   "(dropped=%lu events=%lu)\n",
                   (unsigned long)CONRING_STALL_US,
                   (unsigned long)g_conring.dropped,
                   (unsigned long)g_conring.drop_events);
}

// At most one producer-side wake per CONSOLE_WAKE_MIN_US. The drain also has a
// 50 ms always-armed timer, so this only ever shortens latency; it is never the
// thing that makes a wake happen at all, which is why throttling it is safe.
//
// It is throttled because the unthrottled version issued a wake from inside
// every ISR that logs, thousands of times a second. wake_up() reaches
// proc_wake() -> add_to_ready_queue(), and #610 records what happens when a
// thread is un-parked while it is still running: a second core can context
// switch into it on its live kernel stack. Nothing else in this kernel wakes
// anything at that rate, and the gate-ON arm was crashing.
#define CONSOLE_WAKE_MIN_US 50000ULL
static volatile uint64_t g_console_last_wake_us = 0;

static void console_wake_drain(void) {
    if (!g_console_wq_ready) return;
    if (g_console_wake_guard) return;
    if (mono_ready()) {
        uint64_t now = mono_us();
        if (now - g_console_last_wake_us < CONSOLE_WAKE_MIN_US) return;
        g_console_last_wake_us = now;
    }
    g_console_wake_guard = 1;
    wake_up(&g_console_wq);
    g_console_wake_guard = 0;
}

// Called after the console lock has been RELEASED. Nothing here may run under
// it: wake_up() reaches the scheduler and the stall check reads a clock.
static void console_after_push(uint32_t bits) {
    if (bits & CONRING_R_DROPPED) {
        g_console_dropped = g_conring.dropped;
        // Once per line at most, and only when something was actually lost.
        uint64_t now = mono_ready() ? mono_us() : 0;
        uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
        uint32_t dead = now ? conring_stall_check_rs(&g_conring, now, CONRING_STALL_US) : 0;
        spinlock_release_irqrestore(&g_console_lock, fl);
        if (dead) { console_async_fallback(); return; }
    }
    if (bits & CONRING_R_WAKE) console_wake_drain();
}

// PANIC PATH. Called from kpanic_halt(), the one tail every panic reaches, and
// again up front by kpanic(). Takes no locks, bypasses the UART lock (its
// holder may be the CPU that just died), drains what the ring holds, and
// latches the console back to synchronous so everything printed afterwards
// goes straight out. Idempotent and safe with IF clear.
void console_panic_flush(void) {
    g_console_async      = 0;
    g_serial_lock_bypass = 1;
    console_drain_all(0);
}

// The drain thread body. Started from main.c once the scheduler is live.
void console_drain_worker(void *arg) {
    (void)arg;
    // Declared locally (matches cpu/smp.h) to avoid a serial -> cpu include
    // edge, the same way fs/panic.c declares bkl_release_all().
    extern uint32_t bkl_release_all(void);
    extern void     bkl_acquire(void);
    extern void     bkl_set_reason(uint32_t r);
    extern int      g_smp_bkl_full;
    // 0x300 is the documented "kernel thread body / other" class in cpu/smp.c;
    // 0x70 is this ticket. A [SCHEDCORE] maxhold tagged @0x370 is THIS THREAD,
    // which is the difference between attributing a long hold and guessing.
    const uint32_t CONDRAIN_BKL_REASON = 0x0370u;

    // ONE INSTANCE, EVER. On the first async boot the two opening lines of this
    // function each came out TWICE, interleaved character by character, which
    // is only possible if this body ran concurrently with itself - and a second
    // entry would also re-init the ring under the first one's feet and hand the
    // same bytes out twice. main.c creates exactly one 'condrain' process, so
    // this guard should never fire; if it ever does, it says so on the console
    // rather than quietly corrupting the log.
    static volatile uint32_t g_condrain_entered = 0;
    if (atomic_cas32(&g_condrain_entered, 0, 1) != 0) {
        // OBSERVED on build 1860, gate ON: this fired, which means one process
        // was executing this body on two cores at once. Do NOT return: returning
        // from a kernel thread's entry point calls proc_exit(), which frees the
        // process while the other core is still running on its kernel stack, and
        // that turns a duplicated log line into a dead machine. Park instead, on
        // a queue nothing ever signals, and say so loudly first.
        kprintf_nolock("\n[CONDRAIN] SECOND ENTRY into console_drain_worker on "
                       "cpu %u: ONE process is running on TWO cores (#67 gate-ON "
                       "scheduler). Parking this instance instead of exiting.\n",
                       smp_this_cpu());
        for (;;) {
            (void)wait_event_timeout(&g_console_wq, 0, wq_ms_to_ticks(1000));
        }
    }

    if (!g_console_async_wanted) {
        kprintf("[CONDRAIN] /CONSYNC.TXT present: console stays SYNCHRONOUS "
                "(control arm); no drain\n");
        return;
    }

    wait_queue_head_init(&g_console_wq);
    g_console_wq_ready = 1;
    uint32_t st = conring_init_rs(&g_conring, CONRING_BYTES);
    if (st != 0) {
        kprintf("[CONDRAIN] conring_init_rs FAILED rc=%u; console stays "
                "SYNCHRONOUS\n", st);
        return;
    }
    // Prove the policy before trusting it with the only diagnostic this kernel
    // has. #67 lost three passes to instruments that were wrong, and a console
    // that silently eats lines is that failure in its worst form.
    uint32_t bad = conring_selftest_rs();
    if (bad != 0) {
        kprintf("[CONDRAIN] conring self-test FAILED at case %u; console stays "
                "SYNCHRONOUS\n", bad);
        return;
    }
    kprintf("[CONDRAIN] conring self-test PASS (rust); ring=%u bytes, "
            "stall=%lu us\n",
            (unsigned)CONRING_BYTES, (unsigned long)CONRING_STALL_US);

    // FLIP UNDER THE LOCK. A synchronous kprintf holds g_console_lock for its
    // whole line, so taking the lock here means no synchronous line can be in
    // flight when the switch happens.
    uint64_t fl0 = spinlock_acquire_irqsave(&g_console_lock);
    g_console_async = 1;
    spinlock_release_irqrestore(&g_console_lock, fl0);
    kprintf("[CONDRAIN] ASYNC console live: kprintf enqueues under a short "
            "lock, this thread does the polled UART writes with no long-held "
            "lock\n");

    uint64_t next_report = mono_ready() ? mono_us() + 10000000ULL : 0;

    for (;;) {
        // Drain everything currently queued. The console lock is held only
        // across the pop; the ~22 ms of polled UART writing happens with it
        // RELEASED, the BKL RELEASED and interrupts on, so this thread is fully
        // preemptible while it does it. That is the entire fix: the ten
        // milliseconds still exist, they are just no longer inside a lock that
        // another core needs.
        // DROP THE GIANT LOCK FOR THE WHOLE DRAIN PASS. This is the same
        // release/retake the scheduler performs around a context switch and
        // that sched_smp_report() performs around its own print, and it is the
        // whole reason the ten milliseconds stop mattering.
        //
        // ONCE per pass, not once per chunk. Re-acquiring between chunks meant
        // holding the giant lock across each ring pop and across any preemption
        // that landed in that window, and a hold measured across a deschedule
        // is exactly the artefact that sent #67 pass 8 after a phantom.
        //
        // Retaken with bkl_acquire(), NOT bkl_reacquire(). bkl_reacquire() in
        // cpu/smp.c has no wait on its contended path: after a failed CAS it
        // assigns bkl_owner/bkl_depth anyway, i.e. it STEALS the lock. That is
        // survivable at its one existing call site and fatal here; the first
        // build that used it panicked with "Invalid Opcode at RIP=0x45" and two
        // kfree()s on invalid pointers. bkl_acquire() is recursive and keyed on
        // the owning cpu, so calling it `depth` times restores the exact depth
        // we gave up.
        uint32_t depth = g_smp_bkl_full ? bkl_release_all() : 0;
        for (;;) {
            uint8_t chunk[CONRING_CHUNK];
            uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
            uint32_t n = conring_pop_rs(&g_conring, g_conring_buf, chunk, CONRING_CHUNK);
            spinlock_release_irqrestore(&g_console_lock, fl);
            if (!n) break;
            for (uint32_t i = 0; i < n; i++) serial_write(COM1, (char)chunk[i]);
        }
        for (uint32_t d = 0; d < depth; d++) bkl_acquire();
        if (depth) bkl_set_reason(CONDRAIN_BKL_REASON);

        // Periodic health line. This is the evidence for "no output was lost":
        // pushed and popped must track, dropped must be 0, and highwater says
        // how close the ring came to overflowing.
        if (next_report && mono_us() >= next_report) {
            next_report = mono_us() + 10000000ULL;
            kprintf("[CONDRAIN] pushed=%lu popped=%lu dropped=%lu events=%lu "
                    "highwater=%u/%u fallbacks=%lu\n",
                    (unsigned long)g_conring.pushed,
                    (unsigned long)g_conring.popped,
                    (unsigned long)g_conring.dropped,
                    (unsigned long)g_conring.drop_events,
                    (unsigned)g_conring.highwater, (unsigned)CONRING_BYTES,
                    (unsigned long)g_console_fallbacks);
            continue;   // that kprintf queued bytes; go drain them
        }

        if (!g_console_async) {
            // The stall fallback fired (or a panic flushed us). Anything still
            // queued has been flushed by whoever turned us off.
            kprintf_nolock("[CONDRAIN] async disabled; drain thread exiting\n");
            return;
        }

        // WAIT. Two wake sources, deliberately redundant, which is CLAUDE.md's
        // preferred shape rather than a timeout papering over a missing wake:
        //   * every push that takes the ring from empty to non-empty wakes us
        //     (console_after_push -> console_wake_drain), and
        //   * a 50 ms timed arm that is ALWAYS armed, so a wake skipped by the
        //     re-entrancy guard or the rate limiter above, or lost to any race,
        //     costs at most 50 ms of log latency and can never wedge the
        //     console. This is the hda_space_wq shape CLAUDE.md names as the
        //     BEST option: two independent sources, neither of which is
        //     load-bearing on its own.
        // The condition is re-tested by the macro before it sleeps, so a push
        // that lands between the check and the sleep cannot be missed.
        (void)wait_event_timeout(&g_console_wq,
                                 conring_pending_rs(&g_conring) != 0,
                                 wq_ms_to_ticks(50));
    }
}

// ===========================================================================
// #745 (task #70): THE TWO DESTRUCTIVE PROOFS.
//
// Both are no-ops unless the matching marker file is on the FAT ESP, so a
// shipping golden pays one already-loaded global compare at boot. They exist
// because the two properties the brief demands - a panic still prints, a burst
// does not silently drop - cannot be demonstrated by a healthy boot and cannot
// be demonstrated by reading the code.
// ===========================================================================
volatile int g_console_test_panic = 0;   // /CONPANIC.TXT
volatile int g_console_test_burst = 0;   // /CONBURST.TXT

void console_selftest_worker(void *arg) {
    (void)arg;
    extern void proc_sleep(uint32_t ms);
    extern void kpanic(const char *fmt, ...);

    proc_sleep(20000);   // let the desktop settle so the ring is in normal use

    if (g_console_test_burst) {
        kprintf("[CONTEST] BURST: about to emit far more than the %u-byte ring "
                "can hold, as fast as the CPU can format it. A gap MUST appear "
                "as a [CONDROP n chars lost] marker on a clean line boundary.\n",
                (unsigned)CONRING_BYTES);
        // ~100 bytes a line, 40000 lines = ~4 MB against a 1 MiB ring.
        for (uint32_t i = 0; i < 40000; i++)
            kprintf("[CONTEST] burst line %06u ..........................."
                    "...........................\n", i);
        kprintf("[CONTEST] BURST DONE. dropped=%lu events=%lu highwater=%u\n",
                (unsigned long)g_conring.dropped,
                (unsigned long)g_conring.drop_events,
                (unsigned)g_conring.highwater);
        proc_sleep(30000);   // let the ring drain so the markers reach the port
        kprintf("[CONTEST] BURST drained: pushed=%lu popped=%lu dropped=%lu\n",
                (unsigned long)g_conring.pushed,
                (unsigned long)g_conring.popped,
                (unsigned long)g_conring.dropped);
    }

    if (g_console_test_panic) {
        // Queue enough that the ring CANNOT be empty when the panic lands: 200
        // lines is ~13 KB, about 1.1 s of 115200-baud UART time, and the panic
        // is the very next statement.
        for (uint32_t i = 0; i < 200; i++)
            kprintf("[CONTEST] pre-panic marker %03u of 200 - this line was in "
                    "the ring when the panic fired\n", i);
        kpanic("#745 task #70 DELIBERATE panic: every one of the 200 markers "
               "above must appear, in order, BEFORE this banner");
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_console_async) {
        kfmt_ring_ctx_t rc = { 0 };
        uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
        kvformat(kfmt_ring_sink, &rc, fmt, args);
        spinlock_release_irqrestore(&g_console_lock, fl);
        console_after_push(rc.bits);
    } else {
        kfmt_serial_ctx_t ctx = { COM1 };
        uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
        kvformat(kfmt_serial_sink, &ctx, fmt, args);
        spinlock_release_irqrestore(&g_console_lock, fl);
    }
    va_end(args);
}

// Unlocked, SYNCHRONOUS emission, for the panic path only. See the note above.
// It deliberately bypasses the ring as well as the console lock: by the time
// anything calls this, ordering matters more than throughput and there may be
// no thread left to drain anything.
void kprintf_nolock(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kfmt_serial_ctx_t ctx = { COM1 };
    kvformat(kfmt_serial_sink, &ctx, fmt, args);
    va_end(args);
}

// Global flag for dual output mode
int kprintf_dual_output = 0;

// External console function (from video/console.c)
extern void console_putc(char c);

void kprintf_set_dual_output(int enable) {
    kprintf_dual_output = enable;
}

void kputs(const char *str) {
    if (g_console_async) {
        kfmt_ring_ctx_t rc = { 0 };
        uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
        for (const char *p = str; *p; p++) kfmt_ring_sink(&rc, *p);
        spinlock_release_irqrestore(&g_console_lock, fl);
        console_after_push(rc.bits);
    } else {
        serial_puts(COM1, str);
    }
    if (kprintf_dual_output) {
        while (*str) {
            console_putc(*str++);
        }
    }
}

void kputc(char c) {
    if (g_console_async) {
        kfmt_ring_ctx_t rc = { 0 };
        uint64_t fl = spinlock_acquire_irqsave(&g_console_lock);
        kfmt_ring_sink(&rc, c);
        spinlock_release_irqrestore(&g_console_lock, fl);
        console_after_push(rc.bits);
    } else {
        if (c == '\n') serial_write(COM1, '\r');
        serial_write(COM1, c);
    }
    if (kprintf_dual_output) {
        console_putc(c);
    }
}
