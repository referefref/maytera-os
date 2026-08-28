// irqwin.h - #69: PER-SITE ACCOUNTING OF THE NETWORK STACK'S INTERRUPTS-OFF WINDOWS.
//
// WHY THIS EXISTS
//
// Task #69 is stated as "net_lock() disables interrupts before spinning, so any
// holder stops the whole machine". The lock does do that, and the previous pass
// on this ticket (ed26417, #745/#69) measured the resulting hold: 15-22us, and
// found the 144-second freeze it was blamed for was the serial console instead.
//
// That measurement is correct and it is not the whole interrupts-off window.
// `g_net_lock_hold_max_cyc` starts its clock INSIDE net_lock() and stops it
// inside net_unlock(). Seventeen call sites in this kernel execute their OWN
// `cli` FIRST, because they must switch CR3 to reach the NIC's lower-half
// identity-mapped MMIO/DMA rings, and then do the network work with net_lock
// nested somewhere inside that. On those paths net_lock's hold is a SUBSET of
// the time interrupts are actually off, and the instrument that exists cannot
// see the difference:
//
//     cli                          <-- interrupts off HERE
//     cr3 = kernel pml4
//         ... net_lock() ...       <-- what holdmax measures, the inner part
//         64 x eth_receive()
//         tcp_timer()
//     cr3 = process pml4
//     sti                          <-- interrupts on again HERE
//
// Worse, the socket.c family (sk_pump, sk_tcp_rx, sk_tcp_tx, sk_tcp_conn,
// sk_udp_tx) takes NO net_lock at all. It drains up to 64 frames and runs the
// TCP timer under nothing but its own `cli`, so those windows are INVISIBLE to
// holdmax entirely: they contribute exactly 0 to every number the heartbeat
// reports today. "holdmax is small" was therefore never evidence that the
// network's interrupts-off window is small.
//
// This header measures the OUTER window - the real one, from the `cli` to the
// matching `sti` - per call site, so the answer names the holder instead of
// being an anonymous duration. That naming requirement is #118's lesson, which
// #69 was explicitly asked to follow.
//
// COST, AND WHY IT IS NOT ITSELF THE MEASUREMENT (#67's lesson: a probe that
// lengthens the critical section it measures is not a probe). One rdtsc pair
// and one array update per WINDOW, never per loop iteration. The 64-frame drain
// inside the window is untouched. #67's contention probe was ~93% of its own
// reading because it did a shared-cacheline RMW on every spin iteration; this
// does its arithmetic once, on a line no other core is contending.
//
// KEPT IN C, deliberately, against the Rust-first rule. This is inline `cli`/
// `pushfq`/`rdtsc` that must land in the same basic block as the CR3 switch it
// brackets, on the kernel's hottest interrupts-off paths (every packet drain and
// every TCP syscall). An FFI crossing per window is a cost of the same order as
// the thing being measured, which would make the instrument wrong in the
// direction that matters. This is the identical justification net_lock(),
// net_trylock() and serial_write() already carry in this tree.
#ifndef NET_IRQWIN_H
#define NET_IRQWIN_H

#include "../types.h"

// One id per site that runs its own `cli`. Keep this in sync with
// g_irqwin_name[] in irqwin.c; the _Static_assert there enforces it.
enum {
    IRQWIN_SK_PUMP = 0,      // net/socket.c  sk_pump           (no net_lock)
    IRQWIN_SK_TCP_RX,        // net/socket.c  sk_tcp_rx         (no net_lock)
    IRQWIN_SK_TCP_TX,        // net/socket.c  sk_tcp_tx         (no net_lock)
    IRQWIN_SK_TCP_CONN,      // net/socket.c  sk_tcp_conn       (no net_lock)
    IRQWIN_SK_UDP_TX,        // net/socket.c  sk_udp_tx         (no net_lock)
    IRQWIN_DNS_START,        // proc/syscall.c sys_dns_start
    IRQWIN_DNS_POLL,         // proc/syscall.c sys_dns_poll
    IRQWIN_PING,             // proc/syscall.c icmp_ping_kcr3
    IRQWIN_RX_DRAIN,         // proc/syscall.c net_rx_drain_kcr3
    IRQWIN_DHCP_RESTART,     // proc/syscall.c net_dhcp_restart_c
    IRQWIN_TCP_CONNECT,      // proc/syscall.c tcp_connect_kcr3
    IRQWIN_TCP_SEND,         // proc/syscall.c tcp_send_kcr3
    IRQWIN_TCP_RECV,         // proc/syscall.c tcp_recv_kcr3
    IRQWIN_TCP_CLOSE,        // proc/syscall.c tcp_close_kcr3
    IRQWIN_TCP_STATE,        // proc/syscall.c tcp_state_kcr3
    IRQWIN_TCP_ACCEPT,       // proc/syscall.c tcp_accept_kcr3

    // ---- SUB-MEASUREMENTS. These do NOT open a window; they measure a PART of
    // one that is already open, so that "shrink the critical section" is aimed
    // by measurement instead of by assumption. Baseline said trecv/skrx were the
    // two worst windows; these say WHICH THIRD of them to attack. Without this
    // split the obvious move (shorten the 64-frame drain) is a guess, and a
    // guess that shortens the wrong thing still produces a plausible-looking
    // before/after because the max is noisy.
    IRQWIN_SUB_DRAIN,        // the `for (i < 64) eth_receive()` loop alone
    IRQWIN_SUB_TCPTIMER,     // tcp_timer() alone (walks conns, may RETRANSMIT)
    IRQWIN_SUB_TCPRECV,      // tcp_recv() alone (copy out of the socket buffer)
    // A chunk that consumed ZERO frames. Splits the drain's FIXED per-call cost
    // (cli + two CR3 writes + net_lock + one eth_receive that finds an empty
    // ring) away from its PER-FRAME cost. Without this split, "the drain costs
    // 31us on average" is ambiguous between "frames are expensive" and "asking
    // is expensive", and those two have opposite fixes.
    IRQWIN_SUB_DRAIN0,
    IRQWIN_NSITES
};

// Largest number of frames a single drain loop actually consumed. If this sits
// well under 64 the bound is not what makes the window long, and shortening it
// would be theatre.
extern volatile uint64_t g_irqwin_drain_max_frames;

// #549 REGIME COUNTER. Number of these windows entered by a caller that ALREADY
// had RFLAGS.IF clear, i.e. the no-block regime: pre-scheduler boot, or any
// context under an outer `cli`. For those callers IRQWIN_EXIT restores IF=0, so
// the chunked drain collapses to exactly one long window and behaves as it did
// before #69. This counter exists so that "the no-block path is unaffected" is
// an OBSERVED number rather than an argument from the source: if it reads 0 the
// claim is untested, not proven.
extern volatile uint64_t g_irqwin_noblock;

extern volatile uint64_t g_irqwin_max[IRQWIN_NSITES];   // longest window, cycles
extern volatile uint64_t g_irqwin_tot[IRQWIN_NSITES];   // total, cycles
extern volatile uint64_t g_irqwin_cnt[IRQWIN_NSITES];   // windows entered
extern const char *const g_irqwin_name[IRQWIN_NSITES];

static inline uint64_t irqwin_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void irqwin_record(int site, uint64_t cyc) {
    g_irqwin_cnt[site]++;
    g_irqwin_tot[site] += cyc;
    if (cyc > g_irqwin_max[site]) g_irqwin_max[site] = cyc;
}

// THE SHARED IDIOM. Before this, the sequence
//
//     uint64_t flags;
//     __asm__ volatile("pushfq; pop %0" : "=r"(flags));
//     __asm__ volatile("cli");
//     ...
//     if (flags & 0x200) __asm__ volatile("sti");
//
// was copy-pasted at all sixteen sites. Sixteen copies of a `cli` is exactly
// how a `cli` acquires code it was never scoped for (#632's lesson), and it is
// why no single edit could measure or shorten them. One definition now.
//
// NOTE ON `sti`: the restore is CONDITIONAL on the caller's saved IF, and must
// stay that way. Several of these run in pre-scheduler boot context where the
// caller already had interrupts off; unconditionally enabling them there is the
// #549 no-block regime violation in its most direct form.
#define IRQWIN_DECL          uint64_t _iw_flags = 0, _iw_t0 = 0
#define IRQWIN_ENTER()       do {                                             \
        __asm__ volatile("pushfq; pop %0" : "=r"(_iw_flags));                 \
        __asm__ volatile("cli");                                              \
        _iw_t0 = irqwin_tsc();                                                \
    } while (0)
#define IRQWIN_EXIT(site)    do {                                             \
        irqwin_record((site), irqwin_tsc() - _iw_t0);                         \
        if (_iw_flags & 0x200) __asm__ volatile("sti");                       \
        else g_irqwin_noblock++;   /* caller was already IF=0: #549 regime */  \
    } while (0)

// Measure a PART of an already-open window. No `cli`, no `sti`: the caller is
// already inside one. Declared separately from IRQWIN_DECL so a site can use
// both without the two clashing.
#define IRQWIN_SUB_DECL      uint64_t _iws_t0 = 0
#define IRQWIN_SUB_BEGIN()   do { _iws_t0 = irqwin_tsc(); } while (0)
#define IRQWIN_SUB_END(site) do { irqwin_record((site), irqwin_tsc() - _iws_t0); } while (0)

// THE shared bounded RX drain. Was copy-pasted as a bare `for (i < 64) { if
// (!eth_receive()) break; }` at nine sites; nine copies of a loop is how the
// bound in one of them silently stops matching the others.
#define IRQWIN_DRAIN64()     do {                                              \
        IRQWIN_SUB_DECL; IRQWIN_SUB_BEGIN();                                   \
        unsigned _iwd_n = 0;                                                   \
        for (int _iwd_i = 0; _iwd_i < 64; _iwd_i++) {                          \
            if (!eth_receive()) break;                                         \
            _iwd_n++;                                                          \
        }                                                                      \
        if (_iwd_n > g_irqwin_drain_max_frames)                                \
            g_irqwin_drain_max_frames = _iwd_n;                                \
        IRQWIN_SUB_END(IRQWIN_SUB_DRAIN);                                      \
    } while (0)

// Format one report line into buf. Returns the length written. Prints only
// sites that were actually entered, worst-max first, so an unexercised path
// costs no console width and cannot be mistaken for a measured zero.
int irqwin_report(char *buf, unsigned long len);

#endif // NET_IRQWIN_H
