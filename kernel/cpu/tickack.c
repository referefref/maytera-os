// cpu/tickack.c - #tickdead: THE TICK IS ACKNOWLEDGED BEFORE THE LOCK.
//
// Read rustkern/tickack.rs first: it holds the defect, the scope decision and
// the blame rule, with self-tests. This file is the execution half: the 8259
// `outb`, the Local APIC store, one `rdtsc`, and the counters.
//
// THE ONE-LINE VERSION OF THE BUG. cpu/idt.c isr_handler() calls bkl_acquire()
// before it dispatches, so every EOI in this kernel is sent on the far side of
// an unbounded lock wait. For a device that is latency. For the TIMER it is
// fatal, because the EOI is the permission for the next tick: while the core
// waits for the BKL, its own 250 Hz clock is switched off, and the longer the
// lock is held the longer the clock stays off. A clock whose liveness depends
// on the lock being free is not redundant with anything.
//
// WHY THIS IS A NEW FILE RATHER THAN FOUR LINES IN cpu/isr.c. The hook has to
// run in cpu/idt.c (that is where the lock is taken) while the thing it
// acknowledges lives in cpu/isr.c and cpu/apic.c, and the arm switch, the
// counters and the heartbeat field are shared by all three. Spreading that
// across the two existing files is how the previous ordering became implicit in
// the first place.

#include "tickack.h"
#include "pic.h"
#include "apic.h"
#include "mono.h"
#include "smp.h"
#include "cpumax.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"

// Decisions, from rustkern/tickack.rs.
extern uint32_t tick_ack_kind_rs(uint64_t vec);
extern uint32_t tick_ack_is_late_rs(uint64_t delta_us, uint32_t hz);
extern int32_t  tick_blame_rs(uint64_t dnative, uint64_t dms, uint32_t hz,
                              uint32_t inflight, uint64_t ack_max_us);
extern uint32_t tick_ack_selftest_rs(void);

// Mirror of the Rust constants; cross-checked at boot by the Rust self-test,
// which asserts every one of these mappings directly.
#define TA_NONE  0u
#define TA_PIC0  1u
#define TA_LAPIC 2u

// The native-tick accounting that used to sit inside timer_handler(). It moved
// to a named function in cpu/isr.c so it can run HERE, before the lock,
// alongside the EOI. Advancing timer_ticks late would be almost as bad as
// acknowledging late: cpu/isr.c's redundant source decides the native tick is
// dead from g_tick_last_native_us, so a stamp deferred behind the BKL makes the
// failover synthesise ticks the PIT is about to deliver anyway, and the two
// clocks then double-count.
extern void timer_tick_account(void);
extern uint32_t g_timer_hz;

// The one mounted FAT filesystem. Declared per-file throughout this tree (see
// gui/presentscale.c:19); fs/fat.h describes it but does not declare it.
extern fat_fs_t g_fat_fs;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

volatile int g_tick_early_eoi = 1;   // shipping arm; see tickack.h

volatile uint64_t g_tick_ack_max_us    = 0;
volatile uint64_t g_tick_ack_worst_vec = 0;
volatile uint64_t g_tick_ack_late_n    = 0;
volatile uint64_t g_tick_ack_n         = 0;

// Read-and-reset window maximum for the heartbeat.
static volatile uint64_t s_ack_max_hb = 0;

// Per-core, per-kind. Two kinds can be in flight on one core at the same time
// in the CONTROL arm (an 8259 IRQ0 frame waiting for the lock, with a Local
// APIC tick nested on top of it), and they are acknowledged at different
// controllers, so one slot per core would have them overwrite each other's
// entry timestamp and understate the very delay being measured.
static volatile uint32_t s_inflight[MAYTERA_MAX_CPUS][3];
static volatile uint64_t s_entry_us[MAYTERA_MAX_CPUS][3];

static inline uint32_t ta_cpu(void) {
    uint32_t c = smp_get_cpu_id();
    return (c < MAYTERA_MAX_CPUS) ? c : 0;
}

// ---------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------

void tick_ack_pre_dispatch(uint64_t vec) {
    uint32_t kind = tick_ack_kind_rs(vec);
    if (kind == TA_NONE) return;              // every non-tick vector stops here

    uint32_t c = ta_cpu();
    s_inflight[c][kind]++;
    s_entry_us[c][kind] = mono_ready() ? mono_us() : 0;
    g_tick_ack_n++;

    if (!g_tick_early_eoi) return;            // CONTROL ARM: the handler will ack

    if (kind == TA_PIC0) {
        // Order matters and is not arbitrary. Count and stamp FIRST, then
        // release the line: the instant the EOI is sent the 8259 may deliver
        // IRQ0 again, and a second frame arriving before this one had stamped
        // g_tick_last_native_us would make the failover briefly believe the
        // native source had gone quiet.
        timer_tick_account();
        pic_send_eoi(0);
    } else {
        lapic_eoi();
    }
    tick_ack_note_eoi(vec);
}

void tick_ack_note_eoi(uint64_t vec) {
    uint32_t kind = tick_ack_kind_rs(vec);
    if (kind == TA_NONE) return;

    uint32_t c = ta_cpu();
    if (s_inflight[c][kind]) s_inflight[c][kind]--;

    uint64_t t0 = s_entry_us[c][kind];
    if (!t0 || !mono_ready()) return;
    uint64_t now = mono_us();
    if (now <= t0) return;
    uint64_t d = now - t0;

    if (d > s_ack_max_hb) s_ack_max_hb = d;
    if (d > g_tick_ack_max_us) {
        g_tick_ack_max_us    = d;
        g_tick_ack_worst_vec = vec;
    }
    if (tick_ack_is_late_rs(d, g_timer_hz ? g_timer_hz : 250)) g_tick_ack_late_n++;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

uint32_t tick_ack_inflight_pic(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAYTERA_MAX_CPUS; i++) n += s_inflight[i][TA_PIC0];
    return n;
}

uint64_t tick_ack_max_us_take(void) {
    uint64_t v = s_ack_max_hb;
    s_ack_max_hb = 0;
    return v;
}

int tick_ack_hb_field(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    // ack = worst entry-to-EOI microseconds in THIS window / lifetime worst /
    //       acknowledgements that took at least one tick period / frames
    //       currently delivered-but-unacknowledged on the 8259 IRQ0 vector.
    // The last field is the one that separates "the timer stopped" from "we
    // stopped acknowledging the timer", and nothing else on the heartbeat line
    // can imply it.
#ifdef TICKACK_FAULT_TEST
    if (g_tick_ack_fault_fired)
        return snprintf(buf, cap, "ack=%lluus/%lluus/%llu/%u%s FAULTINJECTED",
                        (unsigned long long)tick_ack_max_us_take(),
                        (unsigned long long)g_tick_ack_max_us,
                        (unsigned long long)g_tick_ack_late_n,
                        tick_ack_inflight_pic(),
                        g_tick_early_eoi ? "" : " PREFIX-ARM");
#endif
    return snprintf(buf, cap, "ack=%lluus/%lluus/%llu/%u%s",
                    (unsigned long long)tick_ack_max_us_take(),
                    (unsigned long long)g_tick_ack_max_us,
                    (unsigned long long)g_tick_ack_late_n,
                    tick_ack_inflight_pic(),
                    g_tick_early_eoi ? "" : " PREFIX-ARM");
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

void tick_ack_boot_report(void) {
    uint32_t st = tick_ack_selftest_rs();
    kprintf("[TICKACK] early EOI %s, selftest=%u (0=pass). The tick vectors "
            "(32, 0x41, 0x42) are acknowledged %s isr_handler() contends for "
            "the BKL.\n",
            g_tick_early_eoi ? "ON" : "OFF",
            st, g_tick_early_eoi ? "BEFORE" : "AFTER");
    bootlog_write("[TICKACK] earlyeoi=%d selftest=%u",
                  g_tick_early_eoi, st);
    if (st != 0) {
        kprintf("[TICKACK] *** WARNING: %u self-test case(s) FAILED - treat "
                "every ack= number as unverified ***\n", st);
        bootlog_write("[TICKACK] WARNING selftest failed: %u", st);
    }
}

#ifdef TICKACK_FAULT_TEST
volatile uint64_t g_tick_ack_fault_fired = 0;

void tick_ack_fault_stall(uint64_t vec) {
    static volatile int fired = 0;
    if (vec != 32 || fired) return;
    if (!mono_ready() || mono_ms() < TICKACK_FAULT_AT_MS) return;
    // Set BEFORE the sti. A frame that nests during the stall must see this
    // already taken, or every one of them stalls too. See tickack.h.
    fired = 1;
    g_tick_ack_fault_fired++;
    // No kprintf from here. This runs inside an IRQ0 frame with IF=0 and
    // BEFORE the BKL, so taking the console lock would wait on a lock another
    // core may hold while this core cannot be interrupted. The evidence is the
    // counter, the [TICKSRC] verdict this provokes, and the ack= field.
    __asm__ volatile("sti");
    mono_busy_delay_ms(TICKACK_FAULT_MS);
    __asm__ volatile("cli");   // restore the state the interrupt gate set
}
#endif

void tick_ack_read_gate(void) {
    // ONE BINARY, TWO ARMS. Absent (the golden) or anything but a leading '0'
    // is the fixed arm. Same shape as /CONFIG/DOSTICK.CFG.
    if (fat_exists(&g_fat_fs, "/CONFIG/TICKEOI.CFG") != 1) return;
    uint32_t sz = 0;
    char *buf = (char *)fat_read_file(&g_fat_fs, "/CONFIG/TICKEOI.CFG", &sz);
    if (!buf) return;
    char c0 = sz ? buf[0] : '1';
    kfree(buf);
    g_tick_early_eoi = (c0 != '0');
    kprintf("[TICKACK] arm from /CONFIG/TICKEOI.CFG: early EOI %s%s\n",
            g_tick_early_eoi ? "ON" : "OFF",
            g_tick_early_eoi ? "" : " - PRE-FIX ORDERING, this is a TEST arm");
    bootlog_write("[TICKACK] gate: earlyeoi=%d", g_tick_early_eoi);
}
