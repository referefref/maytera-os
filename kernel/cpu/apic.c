// apic.c - Local APIC and I/O APIC driver implementation
// Part of Task #41 (SMP Support)

#include "apic.h"
#include "pic.h"
#include "mono.h"        // #62: the shared TSC-backed busy delay, replacing this
                         // file's private fake-microsecond spin loop
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../drivers/acpi_madt.h"
#include "../fs/bootlog.h"   // #426: a give-up must be durable, not just serial

// MSR numbers for APIC
#define MSR_APIC_BASE       0x1B
#define APIC_BASE_ENABLE    (1 << 11)
#define APIC_BASE_X2APIC    (1 << 10)
#define APIC_BASE_BSP       (1 << 8)
#define APIC_BASE_ADDR_MASK 0xFFFFFFFFF000ULL

// Spurious interrupt vector
#define SPURIOUS_VECTOR     0xFF

// Timer interrupt vector
#define TIMER_VECTOR        0x20    // IRQ0 vector

// Default APIC base address
#define DEFAULT_APIC_BASE   0xFEE00000

// ============================================================================
// Global State
// ============================================================================

// Local APIC virtual address (mapped in high memory)
static volatile uint32_t *lapic_base = NULL;

// I/O APIC virtual address
static volatile uint32_t *ioapic_base = NULL;

// APIC timer calibration (ticks per microsecond)
// #745 (#62): ticks per SECOND, in u64, not ticks per MICROSECOND in u32.
// Carrying the intermediate per-microsecond rate truncates it: on QEMU the true
// LAPIC timer rate is 1 GHz / 16 = 62,500,000 Hz = 62.5 ticks/us, and an
// integer per-us rate is 62, which is 0.8% low. MEASURED in our own boot log
// before this fix: the [TICKSRC] line reported icr=1240000 for the 50 Hz
// redundant tick, which is exactly 62 * 20000 - the truncation, visible in the
// artifact. The correct reload is 1,250,000. Keep the full-precision rate and
// divide ONCE, at the point of use.
static uint64_t lapic_ticks_per_sec = 0;

// Is APIC enabled?
static bool lapic_enabled = false;

// #426-class (unknown ASUS Intel i7 laptop): is the xAPIC MMIO window actually
// decoded on this machine? See the big block in lapic_init() below.
static bool lapic_x2apic_mode      = false;  // firmware left the CPU in x2APIC mode
static bool lapic_unusable         = false;  // the window is dead; lapic_* are no-ops
static int  lapic_unusable_logged  = 0;      // the "unusable" line is printed ONCE
static int  lapic_ipi_giveup_logged = 0;     // the IPI give-up line is printed ONCE

// Mark the Local APIC unusable and say so ONCE, loudly and greppably, on serial
// AND in the persistent boot log (serial is silent in GUI mode, and the machines
// where this fires may have no serial console at all).
//
// This is the SAFE degradation and it needs no new fallback code, because the
// fallback already exists: smp_init() reports the failure and the machine stays
// on one CPU, the PIT remains the tick source, and lapic_timer_init_vec()
// already refuses honestly when it cannot arm. Every lapic_* entry point in this
// file early-returns on lapic_base == NULL or on this flag, so nothing can touch
// a window that is not there.
static void lapic_mark_unusable(const char *why) {
    lapic_unusable = true;
    lapic_enabled  = false;
    if (lapic_unusable_logged) return;
    lapic_unusable_logged = 1;
    kprintf("[APIC] *** LOCAL APIC UNUSABLE (%s). Every lapic_* entry point is now "
            "a no-op: the PIT remains the tick source and the machine stays on a "
            "single CPU. ***\n", why);
    bootlog_write("[APIC] LAPIC UNUSABLE (%s): lapic_* disabled, PIT is the clock", why);
}

// ============================================================================
// Local APIC Access Functions
// ============================================================================

// Read from Local APIC register
uint32_t lapic_read(uint32_t offset) {
    // #426: lapic_unusable means the MMIO window is not decoded (x2APIC mode) or
    // the LAPIC is wedged. Reading it would return 0xFFFFFFFF at best, which is
    // the value that made lapic_wait_ipi_idle() unable to terminate. One
    // compare, then out, exactly as serial.c does for an absent UART.
    if (!lapic_base || lapic_unusable) return 0;
    return lapic_base[offset / 4];
}

// Write to Local APIC register
void lapic_write(uint32_t offset, uint32_t value) {
    if (!lapic_base || lapic_unusable) return;
    lapic_base[offset / 4] = value;
    // Read back to ensure write completes (memory fence)
    (void)lapic_base[LAPIC_ID / 4];
}

// ============================================================================
// Local APIC Initialization
// ============================================================================

// Check if APIC is available (via CPUID)
bool lapic_is_available(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;  // APIC flag in CPUID.1.EDX
}

// Check if x2APIC is supported
bool lapic_x2apic_supported(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 21)) != 0;  // x2APIC flag in CPUID.1.ECX
}

// Get Local APIC base address from MSR
uint64_t lapic_get_base(void) {
    return rdmsr(MSR_APIC_BASE) & APIC_BASE_ADDR_MASK;
}

// Set Local APIC base address
void lapic_set_base(uint64_t base) {
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    msr &= ~APIC_BASE_ADDR_MASK;
    msr |= (base & APIC_BASE_ADDR_MASK);
    msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, msr);
}

// Initialize the BSP's Local APIC
int lapic_init(void) {
    kprintf("[APIC] Initializing Local APIC...\n");
    
    // Check for APIC support
    if (!lapic_is_available()) {
        kprintf("[APIC] Error: No APIC available on this CPU\n");
        return -1;
    }
    
    // ------------------------------------------------------------------
    // IS THE xAPIC MMIO WINDOW ACTUALLY DECODED?  (#426-class)
    //
    // IA32_APIC_BASE (MSR 0x1B) bit 10 (EXTD) selects x2APIC mode, in which the
    // Local APIC is reached through MSRs 0x800-0x8FF and the 0xFEE00000 MMIO
    // window is NOT DECODED AT ALL. Some firmware hands the OS a CPU that is
    // already in x2APIC mode.
    //
    // THIS KERNEL NEVER LOOKED. APIC_BASE_X2APIC is defined at the top of this
    // file and was referenced NOWHERE in the tree; lapic_x2apic_supported()
    // had NO CALLERS; and the enable path below is a read-modify-write that ORs
    // in bit 11 and writes bit 10 back UNTOUCHED. So the kernel stayed in x2APIC
    // mode and then did MMIO at 0xFEE00000 anyway. Every read returns all-ones,
    // which is exactly what the banner at the end of this function was reporting
    // as "ID=255, Version=0xff, Max LVT=256" and leaving a human to decode; and
    // bit 12 of ICR_LOW is then permanently set, so lapic_wait_ipi_idle()'s
    // unbounded poll could never exit. That is the hang.
    //
    // DECISION: DETECT AND STAND DOWN. Do NOT try to transition back to xAPIC.
    // The SDM permits x2APIC -> xAPIC only through a disable step (EXTD=1,EN=1
    // -> EXTD=0,EN=0 -> EXTD=0,EN=1); a direct EXTD 1 -> 0 with EN still set is
    // architecturally a #GP, so the "obvious" clear-bit-10-keep-bit-11 fix would
    // fault on the very machine it is meant to rescue. Firmware that booted us
    // in x2APIC mode may also have programmed interrupt remapping that assumes
    // it. An MSR-based x2APIC driver is a new subsystem and out of scope.
    //
    // Standing down is cheap precisely because the degradation path already
    // exists and is already exercised: smp_init() falls back to a single CPU and
    // the PIT stays the tick source. So the cost of being wrong here is a
    // uniprocessor boot, and the cost of being wrong the other way is a hang.
    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    if (base_msr & APIC_BASE_X2APIC) {
        kprintf("[APIC] *** x2APIC MODE DETECTED: IA32_APIC_BASE(0x1B) = 0x%lx has "
                "bit 10 (EXTD) SET, so the 0xFEE00000 xAPIC MMIO window is NOT "
                "DECODED on this machine and every LAPIC register read would "
                "return 0xFFFFFFFF. (A banner reading ID=255, Version=0xff, "
                "Max LVT=256 means exactly this.) This kernel has no x2APIC MSR "
                "driver, so the Local APIC is being marked unusable rather than "
                "read through a dead window. ***\n", base_msr);
        bootlog_write("[APIC] x2APIC mode: IA32_APIC_BASE=0x%lx bit10 set, xAPIC MMIO not decoded",
                      base_msr);
        lapic_x2apic_mode = true;
        // lapic_base is deliberately left NULL: every lapic_read/lapic_write in
        // this file already early-returns on that, so no path can reach the
        // undecoded window even if a future caller forgets to check.
        lapic_mark_unusable("firmware left the CPU in x2APIC mode");
        return -1;
    }
    // First real caller lapic_x2apic_supported() has ever had. Reported even
    // when we are in xAPIC mode, because "the CPU supports x2APIC but firmware
    // left us in xAPIC" and "the CPU has no x2APIC at all" are different
    // machines, and next time this bites, the boot log should say which one.
    kprintf("[APIC] IA32_APIC_BASE = 0x%lx (xAPIC mode; CPU x2APIC capable: %s)\n",
            base_msr, lapic_x2apic_supported() ? "yes" : "no");

    // Get APIC base address (from MADT or MSR)
    uint64_t apic_phys;
    if (madt_is_initialized()) {
        apic_phys = madt_get_local_apic_address();
        kprintf("[APIC] Using MADT Local APIC address: 0x%lx\n", apic_phys);
    } else {
        apic_phys = lapic_get_base();
        kprintf("[APIC] Using MSR Local APIC address: 0x%lx\n", apic_phys);
    }
    
    if (apic_phys == 0) {
        apic_phys = DEFAULT_APIC_BASE;
        kprintf("[APIC] Using default APIC address: 0x%lx\n", apic_phys);
    }
    
    // Map APIC registers
    // The APIC is typically identity-mapped in kernel space
    // In a full implementation, we would use vmm_map_page() here
    // For now, assume identity mapping or direct physical access
    lapic_base = (volatile uint32_t *)apic_phys;
    
    // Enable the APIC via MSR
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, msr);

    // #426: the window must ANSWER before we start writing to it. An all-ones
    // version register is the undecoded/floating-bus signature, the same
    // reasoning drivers/mouse.c applies to a 0xFF status byte from port 0x64: no
    // real Local APIC reports version 0xFF with 256 LVT entries. Catching it
    // here means the LVT/SVR writes below never go into a dead window and the
    // ICR poll further down never gets a permanently-set delivery-status bit.
    {
        uint32_t probe = lapic_read(LAPIC_VERSION);
        if (probe == 0xFFFFFFFFu) {
            kprintf("[APIC] *** LAPIC_VERSION at 0x%lx reads 0xFFFFFFFF: the xAPIC "
                    "window is not decoded (unmapped, or a LAPIC that is not "
                    "responding). ***\n", apic_phys);
            bootlog_write("[APIC] LAPIC_VERSION=0xFFFFFFFF at 0x%lx: window not decoded",
                          apic_phys);
            lapic_mark_unusable("LAPIC_VERSION reads 0xFFFFFFFF, window not decoded");
            lapic_base = NULL;
            return -1;
        }
    }
    
    // Disable the legacy 8259 PIC
    // Send all interrupts to spurious vector
    // #279: KEEP the PIC enabled (virtual-wire mode); timer/keyboard route
    // through it. Disabling it here froze the BSP once SMP was wired.
    
    // Configure Spurious Interrupt Vector Register
    // Enable APIC + set spurious vector
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | SPURIOUS_VECTOR);
    
    // Set Task Priority to 0 (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);
    
    // Configure LVT entries (mask all initially)
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, 0x700);  // #279 ExtINT (virtual wire)
    lapic_write(LAPIC_LVT_LINT1, 0x400);  // #279 NMI
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    
    // Clear error status (write 0, then read)
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);
    
    // Send EOI to clear any pending interrupts
    lapic_eoi();
    
    lapic_enabled = true;
    
    uint32_t apic_id = lapic_get_id();
    uint32_t version = lapic_read(LAPIC_VERSION);
    kprintf("[APIC] Local APIC enabled: ID=%u, Version=0x%x, Max LVT=%u\n",
            apic_id, version & 0xFF, ((version >> 16) & 0xFF) + 1);
    
    return 0;
}

// Initialize an AP's Local APIC
void lapic_init_ap(void) {
    // #426: the AP path had the IDENTICAL defect as the BSP path, a
    // read-modify-write that ORs in bit 11 (EN) and writes bit 10 (EXTD) back
    // untouched, and then does MMIO regardless. Stand down here exactly as the
    // BSP does, and check this AP's own IA32_APIC_BASE rather than assuming the
    // BSP's verdict covers it (EXTD is per-logical-processor).
    if (!lapic_base || lapic_unusable || lapic_x2apic_mode) return;

    // Enable APIC via MSR
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (msr & APIC_BASE_X2APIC) {
        lapic_mark_unusable("an AP came up in x2APIC mode");
        return;
    }
    msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, msr);
    
    // Configure Spurious Interrupt Vector Register
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | SPURIOUS_VECTOR);
    
    // Set Task Priority to 0
    lapic_write(LAPIC_TPR, 0);
    
    // Mask all LVT entries
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    
    // Clear errors and EOI
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);
    lapic_eoi();
}

// Get the APIC ID of the current CPU
uint32_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

// Send End-Of-Interrupt
void lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

// ============================================================================
// Inter-Processor Interrupts (IPI)
// ============================================================================

// Wait for IPI delivery to complete. Returns 0 when the ICR went idle, -1 on
// give-up (window dead, or the cap tripped).
//
// BOUNDED (#426). This site was carried in
// kernel/tools/concurrency-lint/allowlist.txt as [DEBT] with the words "the loop
// is UNBOUNDED: a wedged LAPIC hangs the kernel here forever. Needs an iteration
// cap and a loud give-up, not a wait queue." That debt is paid here, and the
// allowlist line is removed in the same change because the site no longer trips
// the lint at all (the body now contains a real register read, not just pause(),
// so it is no longer a DELAY_SPIN).
//
// WHY A SPIN IS STILL THE RIGHT SHAPE, AND NOT A WAIT QUEUE: ICR delivery status
// clears within bus cycles to a few microseconds and raises NO interrupt, so
// there is no wake source to arm and nothing for wait_event() to be woken by;
// and every caller (lapic_send_init / lapic_send_startup) runs pre-scheduler
// during AP bring-up, where proc_current() is NULL and wq_assert_may_block()
// would correctly refuse to let us sleep. What was wrong was never the spin. It
// was that the spin had no exit.
//
// WHY 1,000,000. It is the same number cpu/smp.c:403 already uses for the AP
// start handshake, so this file and its only consumer share one constant rather
// than two arbitrary ones. At roughly 140 cycles per PAUSE on a modern Intel
// core that is about 1.4e8 cycles, near 50 ms at 3 GHz; on an older core where
// PAUSE is ~10 cycles it is still milliseconds. Either way it is orders of
// magnitude beyond a real delivery (microseconds) and short enough that even the
// pathological case, five waits per AP across the 20-plus APs this laptop's MADT
// reports, gives up in seconds instead of never. In practice the all-ones check
// below fires on the FIRST iteration and latches, so the cap is the belt to that
// braces.
#define LAPIC_IPI_IDLE_SPINS 1000000u

int lapic_wait_ipi_idle(void) {
    if (!lapic_base || lapic_unusable) return -1;

    for (uint32_t i = 0; i < LAPIC_IPI_IDLE_SPINS; i++) {
        uint32_t icr = lapic_read(LAPIC_ICR_LOW);

        // All-ones is the undecoded-window signature, and it has ICR_DS_PENDING
        // permanently set, which is precisely what made the old loop unable to
        // terminate. Never spin on it: latch and get out.
        if (icr == 0xFFFFFFFFu) {
            lapic_mark_unusable("ICR_LOW reads 0xFFFFFFFF while waiting for IPI delivery");
            return -1;
        }
        if (!(icr & ICR_DS_PENDING)) return 0;
        pause();
    }

    if (!lapic_ipi_giveup_logged) {
        lapic_ipi_giveup_logged = 1;
        kprintf("[APIC] *** GAVE UP waiting for IPI delivery: ICR_LOW delivery-status "
                "still set after %u spins (ICR_LOW = 0x%x). The Local APIC is not "
                "completing IPIs; AP startup will fail and the machine stays on one "
                "CPU. ***\n", LAPIC_IPI_IDLE_SPINS, lapic_read(LAPIC_ICR_LOW));
        bootlog_write("[APIC] IPI delivery GIVE-UP after %u spins, ICR_LOW=0x%x",
                      LAPIC_IPI_IDLE_SPINS, lapic_read(LAPIC_ICR_LOW));
    }
    return -1;
}

// Send IPI to a specific CPU
void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    // #426: writing ICR_LOW while a previous IPI is still pending is
    // architecturally undefined, so a failed wait must abort the send rather
    // than push a second command into a controller that never finished the
    // first. On a dead window this is also what stops the send paths from
    // looping through five give-ups per AP.
    if (lapic_wait_ipi_idle() != 0) return;
    
    // Set destination APIC ID in high dword
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send IPI (fixed delivery, edge triggered, assert)
    lapic_write(LAPIC_ICR_LOW, 
                vector | ICR_DM_FIXED | ICR_DST_PHYSICAL | 
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to all CPUs excluding self
void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    if (lapic_wait_ipi_idle() != 0) return;
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_OTHERS |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to all CPUs including self
void lapic_send_ipi_all(uint32_t vector) {
    if (lapic_wait_ipi_idle() != 0) return;
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_ALL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to self
void lapic_send_ipi_self(uint32_t vector) {
    if (lapic_wait_ipi_idle() != 0) return;
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_SELF |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send INIT IPI to a CPU
void lapic_send_init(uint32_t apic_id) {
    if (lapic_wait_ipi_idle() != 0) return;
    
    // Set destination
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send INIT IPI (assert level)
    lapic_write(LAPIC_ICR_LOW,
                ICR_DM_INIT | ICR_DST_PHYSICAL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    
    if (lapic_wait_ipi_idle() != 0) return;
    
    // Deassert INIT IPI
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,
                ICR_DM_INIT | ICR_DST_PHYSICAL |
                ICR_LEVEL_DEASSERT | ICR_TRIGGER_LEVEL);
    
    (void)lapic_wait_ipi_idle();
}

// Send STARTUP IPI (SIPI) to a CPU
void lapic_send_startup(uint32_t apic_id, uint8_t vector) {
    if (lapic_wait_ipi_idle() != 0) return;
    
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_STARTUP | ICR_DST_PHYSICAL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
    
    (void)lapic_wait_ipi_idle();
}

// ============================================================================
// Local APIC Timer
// ============================================================================

// Calibrate APIC timer against the shared monotonic clock.
//
// #745 (#62): this used to call a private `lapic_delay_us()` that spun
// `us * 1000` `pause()` iterations and called the result microseconds. That is
// not a time base at all - it is a loop count whose duration depends entirely
// on the CPU, so every calibration derived from it was wrong by an unknown
// factor. It survived because the whole LAPIC timer path had ZERO CALLERS (the
// kernel has always run on the PIT), which the concurrency-lint allowlist
// recorded in as many words. #62 gives it its first real caller, so the fake
// delay had to go.
//
// The replacement is the SHARED primitive, not a new one: mono_busy_delay_us()
// (cpu/mono.h) is TSC-backed, calibrated once at boot against PIT channel 0,
// and works with interrupts off and before the scheduler exists - which is
// exactly this context. cpu/mono.h's own header comment lists the private
// re-derivations of this that have already cost the project bugs (xhci.c got
// the PIT mode-3 factor wrong, #507); this was one more of them.
void lapic_timer_calibrate(void) {
    kprintf("[APIC] Calibrating APIC timer against the monotonic clock...\n");

    // Configure timer divide register
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);

    // Start timer with maximum count
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    // Measure over 10 ms of REAL time, and use the time we ACTUALLY waited
    // rather than the time we asked for: mono_busy_delay_us() guarantees AT
    // LEAST its argument, so assuming exactly 10000 us builds a small bias into
    // every calibration.
    uint64_t t0 = mono_us();
    mono_busy_delay_us(10000);
    uint64_t real_us = mono_us() - t0;

    // Stop timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    // Calculate ticks elapsed
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    // Full-precision rate, one division, no intermediate per-microsecond value.
    // ~625,000 ticks * 1,000,000 is ~6.25e11, comfortably inside u64.
    lapic_ticks_per_sec = real_us ? ((uint64_t)elapsed * 1000000ull) / real_us : 0;

    // Print the rate so it can be anchored against a value we know from OUTSIDE
    // the kernel: on QEMU it must read close to 62,500,000 (1 GHz / 16). An
    // internal ratio between two of our own clocks is ambiguous in both
    // directions; an external anchor is not.
    kprintf("[APIC] Timer calibration: %llu ticks/sec (div 16, measured over %lluus)\n",
            lapic_ticks_per_sec, real_us);
    // A zero calibration would make the arming path program an initial count of
    // 0, which on x86 means "timer disabled" - a silently dead timer that looks
    // armed. Say so rather than shipping a clock that never fires.
    if (lapic_ticks_per_sec == 0) {
        kprintf("[APIC] *** WARNING: APIC timer calibration measured 0 ticks/sec; "
                "the LAPIC timer cannot be armed from this ***\n");
    }
}

// Initialize APIC timer for periodic interrupts on a CALLER-CHOSEN vector.
//
// #745 (#62): the vector became a parameter so the #62 tick failover can route
// the redundant tick to its own IDT entry (and therefore its own EOI: a
// LAPIC-delivered interrupt needs lapic_eoi(), a PIC-delivered one needs
// pic_send_eoi(0), and one handler cannot honestly do both). Rather than write
// a second copy of this function, the existing one grew an argument and
// lapic_timer_init() became a one-line wrapper preserving its old behaviour
// exactly - the project's standing "extend the shared primitive, never fork a
// private copy" rule. Existing callers see no change.
//
// Returns 1 if the timer was actually armed, 0 if it could not be (no usable
// calibration). A caller relying on this as a redundant clock must know which.
int lapic_timer_init_vec(uint32_t frequency_hz, uint32_t vector) {
    if (lapic_ticks_per_sec == 0) {
        lapic_timer_calibrate();
    }
    if (frequency_hz == 0) return 0;
    // Refuse rather than program ICR=0, which x86 reads as "timer disabled":
    // an unarmed timer that reports success is worse than an honest failure.
    if (lapic_ticks_per_sec == 0) {
        kprintf("[APIC] Timer NOT armed on vector 0x%02x: no calibration\n", vector);
        return 0;
    }

    // ONE division, from the full-precision rate.
    uint64_t reload = lapic_ticks_per_sec / (uint64_t)frequency_hz;

    // REFUSE rather than substitute a floor. `if (reload == 0) reload = 1;` was
    // here and it is an INTERRUPT STORM, not a fallback: at 62.5 MHz an initial
    // count of 1 is a 16 NANOSECOND period, so the line that was meant to keep
    // the timer alive would instead wedge the machine - and it would do so on
    // exactly the fast hardware where a truncated rate is most likely to reach
    // zero in the first place. A clock we cannot program correctly must fail
    // loudly and leave the caller on its previous behaviour, never fire flat
    // out. (Found by the #67 SMP author reviewing this function; the same
    // review found the truncation fixed above.)
    if (reload == 0 || reload > 0xFFFFFFFFull) {
        kprintf("[APIC] *** Timer NOT armed on vector 0x%02x: %u Hz needs reload "
                "%llu, which is out of range for a %llu ticks/sec timer. "
                "Refusing rather than programming a runaway. ***\n",
                vector, frequency_hz, reload, lapic_ticks_per_sec);
        return 0;
    }
    uint32_t initial_count = (uint32_t)reload;

    // Configure timer: periodic mode, not masked
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, initial_count);

    kprintf("[APIC] Timer configured: %u Hz, vector 0x%02x, ICR=%u\n",
            frequency_hz, vector, initial_count);
    return 1;
}

// Original entry point, unchanged in behaviour: periodic on TIMER_VECTOR.
void lapic_timer_init(uint32_t frequency_hz) {
    (void)lapic_timer_init_vec(frequency_hz, TIMER_VECTOR);
}

// #745 (#62): accessors so cpu/tickwatch.c can snapshot controller state
// without duplicating this file's private statics. Reading an unmapped LAPIC
// would fault, so callers must gate on lapic_is_enabled().
bool lapic_is_enabled(void) { return lapic_enabled; }

// #169: the LAPIC timer's CALIBRATED RATE, so a caller on an Application
// Processor can REFUSE rather than trigger a calibration.
//
// lapic_ticks_per_sec is a single global, calibrated once on the BSP against
// the monotonic clock. lapic_timer_init_vec() calls lapic_timer_calibrate() if
// it reads zero - which is correct on the BSP and WRONG on an AP: several APs
// arming their timers would each run a 10 ms calibration writing the same
// global, and a calibration is not a pure read (it programs LVT_TIMER, ICR and
// DCR and then MASKS the timer), so one AP's calibration can land in the middle
// of another's arming sequence and leave that core's timer masked.
//
// The AP path therefore checks this first and refuses loudly if it is zero, so
// lapic_timer_init_vec() can never take its calibrate branch on an AP.
//
// WHY REUSING THE BSP'S RATE IS SOUND: the LAPIC timer is driven from the
// processor's bus/core crystal clock, which is one clock shared by every core
// of an SMP package. A per-core recalibration would measure the same number.
// If that ever stops being true on some machine, the symptom is a wrong AP
// PREEMPTION PERIOD, not a wrong wall clock: #169 deliberately keeps timer_ticks
// single-writer, so an AP timer that is off by a factor cannot corrupt time.
uint64_t lapic_timer_rate(void) { return lapic_ticks_per_sec; }
int  ioapic_available(void) { return ioapic_base != NULL; }

// One-shot timer
void lapic_timer_oneshot(uint32_t microseconds) {
    if (lapic_ticks_per_sec == 0) {
        lapic_timer_calibrate();
    }
    // Same full-precision form, and the same refusal: a one-shot programmed
    // with a count of 0 never fires, and one clamped to 1 fires immediately and
    // forever.
    uint64_t count = (lapic_ticks_per_sec * (uint64_t)microseconds) / 1000000ull;
    if (count == 0 || count > 0xFFFFFFFFull) {
        kprintf("[APIC] one-shot NOT armed: %u us needs count %llu, out of range\n",
                microseconds, count);
        return;
    }

    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, TIMER_VECTOR | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_ICR, (uint32_t)count);
}

// Stop the APIC timer
void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0);
}

// Get current timer count
uint32_t lapic_timer_current(void) {
    return lapic_read(LAPIC_TIMER_CCR);
}

// ============================================================================
// I/O APIC Functions
// ============================================================================

// Read from I/O APIC register
uint32_t ioapic_read(uint32_t index) {
    if (!ioapic_base) return 0;
    ioapic_base[IOAPIC_IOREGSEL / 4] = index;
    return ioapic_base[IOAPIC_IOWIN / 4];
}

// Write to I/O APIC register
void ioapic_write(uint32_t index, uint32_t value) {
    if (!ioapic_base) return;
    ioapic_base[IOAPIC_IOREGSEL / 4] = index;
    ioapic_base[IOAPIC_IOWIN / 4] = value;
}

// Initialize I/O APIC
int ioapic_init(void) {
    kprintf("[APIC] Initializing I/O APIC...\n");
    
    // Get I/O APIC address from MADT
    if (!madt_is_initialized() || madt_get_io_apic_count() == 0) {
        kprintf("[APIC] Error: No I/O APIC found in MADT\n");
        return -1;
    }
    
    io_apic_info_t *io_apic = madt_get_io_apic(0);
    if (!io_apic) {
        kprintf("[APIC] Error: Failed to get I/O APIC info\n");
        return -1;
    }
    
    // Map I/O APIC (assume identity mapping for now)
    ioapic_base = (volatile uint32_t *)(uint64_t)io_apic->address;
    
    uint32_t id = ioapic_get_id();
    uint32_t max_redir = ioapic_get_max_redirs();
    
    kprintf("[APIC] I/O APIC at 0x%x: ID=%u, Max Redirections=%u\n",
            io_apic->address, id, max_redir);
    
    // Mask all IRQ entries initially.
    //
    // KNOWN DEFECT, DELIBERATELY LEFT ALONE (#426 sweep, 2026-08-26). This loop
    // iterates over legacy IRQ NUMBERS and ioapic_mask_irq() then translates
    // each one through madt_irq_to_gsi(), so it does NOT in fact visit every
    // redirection entry. With the usual MADT interrupt-source override IRQ0 ->
    // GSI2 (present on essentially every modern board, this laptop included),
    // i = 0 masks entry 2 and i = 2 masks entry 2 AGAIN, so redirection entry 0
    // is never masked at all and is left exactly as firmware handed it over.
    //
    // The correct code would write the redirection registers by INDEX rather
    // than through the IRQ -> GSI translation. It is NOT being changed here, for
    // two measured reasons:
    //
    //   1. The bug is currently PROTECTIVE. If a board delivers the 8259 INTR
    //      through I/O APIC pin 0 in ExtINT mode (virtual-wire mode B) rather
    //      than through LINT0, then masking entry 0 KILLS THE CLOCK. Today's
    //      loop cannot do that; a "fixed" loop would, silently, on exactly the
    //      unfamiliar hardware this sweep is meant to make survivable. A wrong
    //      change here is strictly worse than the bug.
    //   2. Nothing downstream depends on the entries being masked. ioapic_route_irq(),
    //      ioapic_mask_irq() and ioapic_unmask_irq() have ZERO callers outside
    //      this file, so after ioapic_init() the I/O APIC is configured once and
    //      never touched again. lapic_init() keeps the 8259 alive and programs
    //      LINT0 as ExtINT (virtual wire), which is how interrupts actually
    //      reach the BSP on this kernel.
    //
    // So the entry-0 hole costs nothing today and insures against one real
    // hardware routing. Fix it only together with a real I/O APIC routing path,
    // and only with the ExtINT case handled explicitly.
    for (uint32_t i = 0; i <= max_redir; i++) {
        ioapic_mask_irq(i);
    }
    
    return 0;
}

// Get I/O APIC ID
uint32_t ioapic_get_id(void) {
    return (ioapic_read(IOAPIC_ID) >> 24) & 0xF;
}

// Get maximum number of redirection entries
uint32_t ioapic_get_max_redirs(void) {
    return (ioapic_read(IOAPIC_VER) >> 16) & 0xFF;
}

// Route an IRQ to a CPU
void ioapic_route_irq(uint32_t irq, uint32_t vector, uint32_t apic_id, uint32_t flags) {
    // Check for ISA IRQ override
    uint32_t gsi = madt_irq_to_gsi(irq);
    uint16_t override_flags = madt_get_irq_flags(irq);
    
    // Build redirection entry
    uint64_t redir = vector;
    
    // Set delivery mode
    redir |= IOAPIC_REDIR_DM_FIXED;
    
    // Set destination (physical mode, specific APIC ID)
    redir |= ((uint64_t)apic_id << 56);
    
    // Apply polarity from override or flags
    if (override_flags & 0x02) {
        // Low active
        redir |= IOAPIC_REDIR_INTPOL;
    } else if (flags & IOAPIC_REDIR_INTPOL) {
        redir |= IOAPIC_REDIR_INTPOL;
    }
    
    // Apply trigger mode from override or flags
    if (override_flags & 0x08) {
        // Level triggered
        redir |= IOAPIC_REDIR_TRIGGER;
    } else if (flags & IOAPIC_REDIR_TRIGGER) {
        redir |= IOAPIC_REDIR_TRIGGER;
    }
    
    // Write redirection entry (low dword first)
    uint32_t reg_base = IOAPIC_REDTBL_BASE + (gsi * 2);
    ioapic_write(reg_base, (uint32_t)redir);
    ioapic_write(reg_base + 1, (uint32_t)(redir >> 32));
    
    kprintf("[APIC] Routed IRQ %u (GSI %u) to vector 0x%x, APIC ID %u\n",
            irq, gsi, vector, apic_id);
}

// Mask (disable) an IRQ
void ioapic_mask_irq(uint32_t irq) {
    uint32_t gsi = madt_irq_to_gsi(irq);
    uint32_t reg = IOAPIC_REDTBL_BASE + (gsi * 2);
    
    uint32_t low = ioapic_read(reg);
    low |= IOAPIC_REDIR_MASKED;
    ioapic_write(reg, low);
}

// Unmask (enable) an IRQ
void ioapic_unmask_irq(uint32_t irq) {
    uint32_t gsi = madt_irq_to_gsi(irq);
    uint32_t reg = IOAPIC_REDTBL_BASE + (gsi * 2);
    
    uint32_t low = ioapic_read(reg);
    low &= ~((uint32_t)IOAPIC_REDIR_MASKED);
    ioapic_write(reg, low);
}
