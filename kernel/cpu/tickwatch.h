// cpu/tickwatch.h - #745 (#62): is the periodic tick actually being delivered?
//
// THE QUESTION THIS ANSWERS. On the real iMac14,4 the owner reports that the
// desktop only advances while something is running: stop moving the mouse and
// uptime stops counting, open an app and a real one-per-second tick appears.
// Every timer in this OS is downstream of one interrupt - the 8254 PIT on IRQ0,
// vector 32 - so "the clock stops when the machine is idle" is a claim about
// interrupt DELIVERY, and until now nothing in the tree measured that. `uptime`
// is ticks/hz, which is circular: it is derived from the very counter under
// suspicion and can never reveal a delivery pathology.
//
// WHAT IT DOES.
//   * Measures tick delivery against cpu/mono.h (TSC-backed, independent of
//     timer_ticks) and classifies each window OK / SLOW / DEAD / BURST.
//   * Snapshots the interrupt-controller state that would EXPLAIN a dead tick:
//     the 8259 mask and request registers, the Local APIC's SVR and LINT0
//     (virtual-wire routing), and the I/O APIC redirection entries for the two
//     GSIs the ISA timer can legally land on (0 and, under an ACPI interrupt
//     source override, 2).
//   * Records all of it to /HEARTBEAT.TXT and /BOOTLOG.TXT, because the iMac
//     has no serial console and the USB stick is the only telemetry channel.
//   * Arms a REDUNDANT tick source (the Local APIC timer) if, and only if, the
//     native tick is measured dead. See tickwatch_poll().
//
// THE DECISIONS LIVE IN RUST (rustkern/tickwatch.rs), per the
// all-new-kernel-code-in-Rust rule. What is here is the part that cannot be:
// `in`/`out` port access, memory-mapped APIC reads, IDT registration and the
// mutable state they act on. Same split, same reason, as schedwatch.rs.
#ifndef CPU_TICKWATCH_H
#define CPU_TICKWATCH_H

#include "../types.h"

// Interrupt-controller state that could explain a missing IRQ0. Every field is
// a RAW register read, not a derived opinion: on hardware we do not control,
// the raw value is the evidence and any interpretation we add is a guess.
typedef struct {
    uint8_t  pic1_imr;        // 8259 master mask. Bit 0 set = IRQ0 MASKED.
    uint8_t  pic2_imr;        // 8259 slave mask.
    uint16_t pic_irr;         // pending-but-undelivered requests.
    uint16_t pic_isr;         // in-service.
    uint32_t lapic_svr;       // bit 8 = APIC software-enabled.
    uint32_t lapic_lint0;     // 0x700 = ExtINT (virtual wire): the path an
                              // 8259-delivered IRQ0 must take once the LAPIC
                              // is enabled. Bit 16 set = MASKED = no PIC
                              // interrupt reaches this CPU at all.
    uint32_t lapic_lvt_timer; // failover source state.
    uint32_t lapic_ticr;      // failover initial count (0 = not running).
    uint32_t lapic_tccr;      // failover current count.
    uint32_t ioapic_present;  // 1 if an I/O APIC was found and mapped.
    uint32_t ioapic_redtbl0;  // GSI0 low dword. Bit 16 set = masked.
    uint32_t ioapic_redtbl2;  // GSI2 low dword (ISA IRQ0 override target).
    uint32_t lapic_enabled;   // our own bookkeeping, for cross-checking.
} tickwatch_regs_t;

// Read the current interrupt-controller state. Safe from any context: port and
// MMIO reads only, no locks, no allocation, no waiting.
void tickwatch_snapshot(tickwatch_regs_t *out);

// One-shot boot report. Call once after interrupts are enabled and after the
// existing [DELAY] tick-vs-mono check, so the two corroborate each other.
// Writes a [TICKSRC] line to the serial console AND to /BOOTLOG.TXT, which is
// the copy that survives to be read off the stick.
void tickwatch_boot_report(void);

// Judge one window and act on it. Called from the heartbeat thread.
//   `ticks`  the current value of timer_ticks (total, any source).
//   `now_ms` REAL milliseconds from mono_ms(). MUST NOT be tick-derived.
//   `hz`     the programmed tick rate.
// Returns the verdict (TICK_OK / TICK_SLOW / TICK_DEAD / TICK_BURST /
// TICK_UNKNOWN, see rustkern/tickwatch.rs).
//
// This MEASURES and RECORDS only; it arms nothing. The redundant tick source
// is armed unconditionally at boot (tick_redundant_arm), because a watchdog
// that armed a backup clock on demand would itself have to be scheduled, and
// scheduling is downstream of the clock it would be watching.
int tickwatch_poll(uint64_t ticks, uint64_t now_ms, uint32_t hz);

// Format the compact heartbeat field (`tsrc=...`). Returns bytes written.
// Bounded; never allocates.
int tickwatch_hb_field(char *buf, uint32_t cap);

// Ticks delivered by the FAILOVER source only. cpu/isr.c maintains it. The
// native count is (timer_ticks - this), which is what the failover policy
// needs: judging "did the native source come back" from the TOTAL would be
// satisfied by the failover's own ticks and it would never release.
extern volatile uint64_t g_tick_src_lapic;

// #745 (#62): arm the redundant tick source. Implemented in cpu/isr.c next to
// the handler it arms. Call once, after smp_init() has brought the Local APIC
// up. Loud on failure: a redundancy that silently did not arm looks exactly
// like one that is working.
void tick_redundant_arm(void);

// #62 FAULT INJECTION, for proving the failover in a VM. Masks IRQ0 at the
// 8259, which reproduces exactly the symptom the iMac shows (a periodic timer
// that is programmed but whose interrupt never arrives) on hardware that would
// otherwise never fail. Compiled in only under -DTICKWATCH_FAULT_TEST, so it
// cannot ship: see kernel/Makefile.
#ifdef TICKWATCH_FAULT_TEST
void tickwatch_fault_inject_kill_irq0(void);
#endif

#endif // CPU_TICKWATCH_H
