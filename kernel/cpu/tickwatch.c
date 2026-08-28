// cpu/tickwatch.c - #745 (#62): measure periodic-tick DELIVERY and record it
// to the stick.
//
// This file is the INSTRUMENT. The structural fix - the redundant, always-
// armed Local APIC tick source - lives in cpu/isr.c next to the handler it
// belongs to; see tick_redundant_arm() and lapic_tick_handler() there.
//
// See cpu/tickwatch.h for what this is and why it exists. The short version:
// every timer in this OS hangs off one interrupt (the 8254 PIT on IRQ0, vector
// 32), nothing measured whether that interrupt actually arrives, and on the
// owner's iMac14,4 the evidence says it sometimes does not.
//
// WHY THIS FILE IS C AND NOT RUST. The all-new-kernel-code-in-Rust rule allows
// C where the code is genuinely entangled with asm/port-level hardware access.
// Everything here is exactly that: `in`/`out` on the 8259, memory-mapped Local
// APIC and I/O APIC register reads. The DECISIONS - what counts as a dead
// tick, and how many ticks the redundant source should synthesise - are all
// in rustkern/tickwatch.rs and the C only executes them. That is the
// established split in this tree (schedwatch.rs, sched_age.rs).

#include "tickwatch.h"
#include "pic.h"
#include "apic.h"
#include "mono.h"
#include "../serial.h"
#include "../string.h"       // snprintf for the heartbeat field
#include "../fs/bootlog.h"

// Decisions, from rustkern/tickwatch.rs.
extern int32_t  tick_health_verdict_rs(uint64_t dticks, uint64_t dms, uint32_t hz);
extern uint64_t tick_permille_rs(uint64_t dticks, uint64_t dms, uint32_t hz);
extern uint32_t tick_watch_selftest_rs(void);

// Mirror of the Rust constants. _Static_assert cannot reach across the FFI for
// values, so these are cross-checked at runtime by tickwatch_boot_report()
// calling the Rust self-test, which asserts the boundaries directly.
#define TW_UNKNOWN 0
#define TW_OK      1
#define TW_SLOW    2
#define TW_DEAD    3
#define TW_BURST   4

// ---------------------------------------------------------------------------
// State. Single-writer: tickwatch_poll() runs only on the heartbeat thread.
// The counters it READS are maintained in cpu/isr.c.
// ---------------------------------------------------------------------------
static uint64_t tw_prev_ticks   = 0;
static uint64_t tw_prev_ms      = 0;
static uint64_t tw_prev_lapic   = 0;
static int      tw_state        = TW_UNKNOWN;
static uint64_t tw_permille     = 0;
static uint32_t tw_dead_windows = 0;   // cumulative, for the heartbeat
static uint64_t tw_first_ok_ms  = 0;   // mono_ms of the FIRST healthy window
static int      tw_selftest     = -1;  // -1 = not run yet
static int      tw_consec_dead_reported = 0;   // one-shot loud line


// ---------------------------------------------------------------------------
// Register snapshot
// ---------------------------------------------------------------------------
void tickwatch_snapshot(tickwatch_regs_t *out) {
    if (!out) return;

    out->pic1_imr = inb(PIC1_DATA);
    out->pic2_imr = inb(PIC2_DATA);
    out->pic_irr  = pic_get_irr();
    out->pic_isr  = pic_get_isr();

    // The Local APIC may not be up yet (smp_init() runs late in boot). Reading
    // an unmapped LAPIC would fault, so gate on our own flag rather than on
    // hope.
    if (lapic_is_enabled()) {
        out->lapic_enabled   = 1;
        out->lapic_svr       = lapic_read(LAPIC_SVR);
        out->lapic_lint0     = lapic_read(LAPIC_LVT_LINT0);
        out->lapic_lvt_timer = lapic_read(LAPIC_LVT_TIMER);
        out->lapic_ticr      = lapic_read(LAPIC_TIMER_ICR);
        out->lapic_tccr      = lapic_read(LAPIC_TIMER_CCR);
    } else {
        out->lapic_enabled   = 0;
        out->lapic_svr       = 0;
        out->lapic_lint0     = 0;
        out->lapic_lvt_timer = 0;
        out->lapic_ticr      = 0;
        out->lapic_tccr      = 0;
    }

    // GSI0 and GSI2 are the only two entries the ISA timer can legally land on:
    // GSI0 directly, or GSI2 when the ACPI MADT carries an interrupt source
    // override for ISA IRQ0, which is the common arrangement on real firmware
    // and is exactly the kind of thing a VM does not exercise. ioapic_read()
    // returns 0 when no I/O APIC is mapped, so this is safe unconditionally.
    if (ioapic_available()) {
        out->ioapic_present  = 1;
        out->ioapic_redtbl0  = ioapic_read(0x10 + 0 * 2);
        out->ioapic_redtbl2  = ioapic_read(0x10 + 2 * 2);
    } else {
        out->ioapic_present  = 0;
        out->ioapic_redtbl0  = 0;
        out->ioapic_redtbl2  = 0;
    }
}

// ---------------------------------------------------------------------------
// Boot report
// ---------------------------------------------------------------------------
//
// This is the capture that matters most for a machine that is frozen FROM
// BOOT. The heartbeat thread needs the scheduler, and the scheduler needs the
// tick, so on a machine whose tick never started the heartbeat may never write
// a line at all. This runs on the boot path with no scheduler dependency, so
// it lands in /BOOTLOG.TXT regardless.
void tickwatch_boot_report(void) {
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;

    // Run the Rust self-test first. An instrument whose own boundaries are
    // wrong produces numbers that are not measurements, which is the documented
    // failure of this ticket's previous pass. 0 = all cases passed.
    tw_selftest = (int)tick_watch_selftest_rs();

    // Measure over a window that does not depend on the scheduler and does not
    // use the clock under test. mono_busy_delay_ms() is the shared TSC-backed
    // busy delay (cpu/mono.h); this is a measurement precondition, not a device
    // poll, and it is the same technique the existing [DELAY] check uses.
    // MEASURE THE NATIVE SOURCE, NOT THE TOTAL. By the time this runs the
    // redundant Local APIC source is already armed, and on a machine whose PIT
    // is dead it will be synthesising ticks into the SAME `timer_ticks`
    // counter. Judging the total would therefore report a healthy clock on
    // exactly the machine this diagnostic exists for - the instrument would be
    // blind to its own subject. Subtract the redundant source's contribution
    // and judge what is left, which is what the PIT actually delivered.
    uint64_t t0 = timer_ticks - g_tick_src_lapic, m0 = mono_ms();
    mono_busy_delay_ms(300);
    uint64_t dt = (timer_ticks - g_tick_src_lapic) - t0, dm = mono_ms() - m0;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;

    int      v  = (int)tick_health_verdict_rs(dt, dm, hz);
    uint64_t pm = tick_permille_rs(dt, dm, hz);

    tickwatch_regs_t r;
    tickwatch_snapshot(&r);

    const char *vs = (v == TW_OK)   ? "OK"
                   : (v == TW_SLOW) ? "SLOW"
                   : (v == TW_DEAD) ? "DEAD"
                   : (v == TW_BURST)? "BURST" : "UNKNOWN";

    // Serial AND bootlog. On the iMac only the bootlog copy survives.
    kprintf("[TICKSRC] boot verdict=%s NATIVEticks=%llu/%llums (%llu permille of %uHz) "
            "selftest=%d pic1=0x%02x pic2=0x%02x irr=0x%04x isr=0x%04x "
            "lapic=%u svr=0x%08x lint0=0x%08x lvtt=0x%08x icr=%u ccr=%u "
            "ioapic=%u red0=0x%08x red2=0x%08x\n",
            vs, dt, dm, pm, hz, tw_selftest,
            r.pic1_imr, r.pic2_imr, r.pic_irr, r.pic_isr,
            r.lapic_enabled, r.lapic_svr, r.lapic_lint0, r.lapic_lvt_timer,
            r.lapic_ticr, r.lapic_tccr,
            r.ioapic_present, r.ioapic_redtbl0, r.ioapic_redtbl2);

    bootlog_write("[TICKSRC] verdict=%s %llu NATIVEticks/%llums (%llu pm of %uHz) st=%d "
                  "pic1=0x%02x irr=0x%04x lapic=%u lint0=0x%08x lvtt=0x%08x "
                  "ioapic=%u red0=0x%08x red2=0x%08x",
                  vs, dt, dm, pm, hz, tw_selftest,
                  r.pic1_imr, r.pic_irr, r.lapic_enabled, r.lapic_lint0,
                  r.lapic_lvt_timer, r.ioapic_present, r.ioapic_redtbl0,
                  r.ioapic_redtbl2);

    // Say the consequence in words, not just numbers. A reader of a stick log
    // six weeks from now should not have to re-derive what "0 ticks" implies.
    if (v == TW_DEAD) {
        kprintf("[TICKSRC] *** THE PERIODIC TICK IS NOT BEING DELIVERED. Every "
                "sleep, timeout and scheduler preemption in this kernel is "
                "downstream of it; the desktop will only advance when another "
                "interrupt drags the scheduler forward. ***\n");
        bootlog_write("[TICKSRC] *** PERIODIC TICK NOT DELIVERED AT BOOT ***");
    } else if (v == TW_SLOW) {
        kprintf("[TICKSRC] *** Tick delivery is far below the programmed rate "
                "(%llu permille). Time advances, unevenly. ***\n", pm);
        bootlog_write("[TICKSRC] *** TICK SLOW: %llu permille ***", pm);
    }
    if (tw_selftest != 0) {
        kprintf("[TICKSRC] *** WARNING: %d self-test case(s) FAILED - treat "
                "every number on this line as unverified ***\n", tw_selftest);
        bootlog_write("[TICKSRC] WARNING selftest failed: %d", tw_selftest);
    }

    // Seed the window state so the first heartbeat poll measures a real
    // interval rather than everything since boot.
    tw_prev_ticks = timer_ticks;
    tw_prev_ms    = mono_ms();
    tw_prev_lapic = g_tick_src_lapic;   // poll() subtracts these the same way
    tw_state      = v;
    tw_permille   = pm;
    if (v == TW_OK && tw_first_ok_ms == 0) tw_first_ok_ms = tw_prev_ms;
}

// ---------------------------------------------------------------------------
// Per-window poll, from the heartbeat thread
// ---------------------------------------------------------------------------
int tickwatch_poll(uint64_t ticks, uint64_t now_ms, uint32_t hz) {
    if (tw_prev_ms == 0) {           // first call: open the window, judge nothing
        tw_prev_ticks = ticks;
        tw_prev_ms    = now_ms;
        tw_prev_lapic = g_tick_src_lapic;
        return TW_UNKNOWN;
    }

    uint64_t dticks = ticks   - tw_prev_ticks;
    uint64_t dms    = now_ms  - tw_prev_ms;
    uint64_t dlapic = g_tick_src_lapic - tw_prev_lapic;
    // Ticks the NATIVE source contributed. Every verdict below is computed on
    // THIS, never on the total: once the redundant source is carrying the
    // system the total reads perfectly healthy, so a verdict taken from it
    // would hide the very fault this instrument exists to find.
    uint64_t dnative = (dticks > dlapic) ? (dticks - dlapic) : 0;

    tw_prev_ticks = ticks;
    tw_prev_ms    = now_ms;
    tw_prev_lapic = g_tick_src_lapic;

    // Judge the NATIVE source: that is the thing suspected of being broken on
    // the iMac, and it is what a stick brought back from that machine needs to
    // be able to say.
    int v = (int)tick_health_verdict_rs(dnative, dms, hz);
    tw_permille = tick_permille_rs(dnative, dms, hz);
    tw_state    = v;

    if (v == TW_DEAD) {
        tw_dead_windows++;
    } else if (v != TW_UNKNOWN) {
        if (v == TW_OK && tw_first_ok_ms == 0) tw_first_ok_ms = now_ms;
    }

    // NOTE: nothing is ARMED here. The redundant tick source (cpu/isr.c) is
    // armed unconditionally at boot, because a watchdog that arms a backup
    // clock would itself need to be scheduled, and scheduling is downstream of
    // the clock it is watching. This function's job is to MEASURE and RECORD,
    // so that a stick brought back from the iMac can say what the tick was
    // doing, and the heartbeat's own liveness is then a second, independent
    // signal: if these records stop appearing at all, nothing was running.
    if (v == TW_DEAD && tw_consec_dead_reported == 0) {
        tw_consec_dead_reported = 1;
        tickwatch_regs_t dr;
        tickwatch_snapshot(&dr);
        kprintf("[TICKSRC] *** the NATIVE tick delivered 0 ticks in %llums of "
                "real time. The machine is now running on the redundant Local "
                "APIC source (%llu synthesised ticks so far). "
                "8259: mask=0x%02x isr=0x%04x irr=0x%04x ***\n",
                dms, (unsigned long long)g_tick_src_lapic,
                dr.pic1_imr, dr.pic_isr, dr.pic_irr);
        // Say WHICH of the two mechanisms the registers show, in words. A
        // reader of a stick log months from now should not have to remember
        // that a non-specific EOI clears the highest in-service bit.
        if (dr.pic1_imr & 0x01) {
            kprintf("[TICKSRC] ATTRIBUTED: IRQ0 is MASKED at the 8259 "
                    "(mask bit 0 set). Something disabled the timer interrupt.\n");
            bootlog_write("[TICKSRC] DEAD: IRQ0 MASKED (imr=0x%02x)", dr.pic1_imr);
        } else if (dr.pic_isr & 0x01) {
            kprintf("[TICKSRC] ATTRIBUTED: IRQ0 is UNMASKED but its IN-SERVICE "
                    "bit is STUCK SET. A timer ISR did not reach its EOI, so the "
                    "8259 will not deliver IRQ0 again until some other "
                    "non-specific EOI clears it. This is a LOCKING bug, not a "
                    "timer or routing bug.\n");
            bootlog_write("[TICKSRC] DEAD: IRQ0 ISR BIT STUCK (isr=0x%04x) - missed EOI",
                          dr.pic_isr);
        } else {
            kprintf("[TICKSRC] ATTRIBUTED: IRQ0 is unmasked and not in service, "
                    "so the 8259 believes it is idle. The interrupt is being "
                    "lost upstream of it - check LINT0/IOAPIC routing.\n");
            bootlog_write("[TICKSRC] DEAD: IRQ0 idle at the 8259 - upstream routing");
        }
        bootlog_write("[TICKSRC] NATIVE TICK DEAD at %llums (lapic ticks=%llu, "
                      "imr=0x%02x isr=0x%04x irr=0x%04x)",
                      now_ms, (unsigned long long)g_tick_src_lapic,
                      dr.pic1_imr, dr.pic_isr, dr.pic_irr);
        // This is the single most valuable record in the file for #62. Do not
        // let it sit in the RAM ring waiting for the 30-minute scheduled flush
        // on a machine the user is about to power-cycle.
        (void)bootlog_heartbeat_flush();
    }

    return v;
}

// ---------------------------------------------------------------------------
// Heartbeat field
// ---------------------------------------------------------------------------
//
// Compact on purpose: this line already carries thirty fields and it is written
// to a bounded ring on a USB stick. Every token here is one that cannot be
// derived from the others.
int tickwatch_hb_field(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    tickwatch_regs_t r;
    tickwatch_snapshot(&r);

    const char *vs = (tw_state == TW_OK)   ? "ok"
                   : (tw_state == TW_SLOW) ? "slow"
                   : (tw_state == TW_DEAD) ? "DEAD"
                   : (tw_state == TW_BURST)? "burst" : "unk";

    // tsrc = native verdict / permille-of-nominal / cumulative dead windows
    // ok1  = mono ms at the FIRST window the native tick was judged healthy.
    //        On a machine that is dead from boot and later recovers, this is
    //        the timestamp of the recovery, which is the thing the owner's
    //        report ("it starts counting once I open an app") most needs.
    // lapt = ticks SYNTHESISED by the redundant source. Zero on a healthy
    //        machine; non-zero is proof the redundancy carried the system.
    // ic   = the raw interrupt-controller evidence, so a dead tick can be
    //        ATTRIBUTED rather than guessed at:
    //          8259 master MASK / 8259 ISR / 8259 IRR /
    //          LAPIC LINT0 / IOAPIC GSI0 / IOAPIC GSI2
    //
    //        THE ISR (in-service) REGISTER IS HERE BECAUSE OF A SECOND, LATER
    //        CANDIDATE and it was missing from the first version of this field.
    //        The 8259 stops delivering an IRQ while that IRQ's in-service bit
    //        is set, and our pic_send_eoi() sends a NON-SPECIFIC EOI (OCW2
    //        0x20), which clears the HIGHEST-priority in-service bit rather
    //        than the one named by its argument. So a timer ISR that fails to
    //        reach its EOI leaves bit 0 latched and IRQ0 never arrives again -
    //        a dead tick with NO timer bug, no mask change and no routing
    //        change anywhere. The MASK would look innocent; only the ISR shows
    //        it. Distinguishing that from a masked IRQ0 is the difference
    //        between a locking bug and an interrupt-routing bug, and the two
    //        have nothing in common except the symptom.
    return snprintf(buf, cap,
                    "tsrc=%s/%llu/%u ok1=%llums lapt=%llu "
                    "ic=%02x/%04x/%04x/%08x/%08x/%08x",
                    vs, (unsigned long long)tw_permille, tw_dead_windows,
                    (unsigned long long)tw_first_ok_ms,
                    (unsigned long long)g_tick_src_lapic,
                    r.pic1_imr, r.pic_isr, r.pic_irr, r.lapic_lint0,
                    r.ioapic_redtbl0, r.ioapic_redtbl2);
}

#ifdef TICKWATCH_FAULT_TEST
// #62 FAULT INJECTION. Masks IRQ0 at the 8259 master, which is precisely the
// shape of the iMac symptom: the PIT is programmed and counting, and its
// interrupt never reaches the CPU. QEMU will never produce that on its own,
// so without this the failover path could only ever be reasoned about, never
// watched working. Compiled out of the shipping build.
void tickwatch_fault_inject_kill_irq0(void) {
    uint8_t imr = inb(PIC1_DATA);
    outb(PIC1_DATA, (uint8_t)(imr | 0x01));   // bit 0 = IRQ0
    kprintf("[TICKSRC-FAULT] IRQ0 MASKED at the 8259 (IMR 0x%02x -> 0x%02x). "
            "The periodic tick is now dead exactly as it is on the iMac.\n",
            imr, inb(PIC1_DATA));
    bootlog_write("[TICKSRC-FAULT] IRQ0 masked, native tick killed");
}
#endif
