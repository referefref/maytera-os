// apic.c - Local APIC and I/O APIC driver implementation
// Part of Task #41 (SMP Support)

#include "apic.h"
#include "pic.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../drivers/acpi_madt.h"

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
static uint32_t lapic_ticks_per_us = 0;

// Is APIC enabled?
static bool lapic_enabled = false;

// ============================================================================
// Local APIC Access Functions
// ============================================================================

// Read from Local APIC register
uint32_t lapic_read(uint32_t offset) {
    if (!lapic_base) return 0;
    return lapic_base[offset / 4];
}

// Write to Local APIC register
void lapic_write(uint32_t offset, uint32_t value) {
    if (!lapic_base) return;
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
    // Enable APIC via MSR
    uint64_t msr = rdmsr(MSR_APIC_BASE);
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

// Wait for IPI delivery to complete
void lapic_wait_ipi_idle(void) {
    // Poll ICR delivery status until cleared
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DS_PENDING) {
        pause();
    }
}

// Send IPI to a specific CPU
void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_wait_ipi_idle();
    
    // Set destination APIC ID in high dword
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send IPI (fixed delivery, edge triggered, assert)
    lapic_write(LAPIC_ICR_LOW, 
                vector | ICR_DM_FIXED | ICR_DST_PHYSICAL | 
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to all CPUs excluding self
void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    lapic_wait_ipi_idle();
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_OTHERS |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to all CPUs including self
void lapic_send_ipi_all(uint32_t vector) {
    lapic_wait_ipi_idle();
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_ALL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send IPI to self
void lapic_send_ipi_self(uint32_t vector) {
    lapic_wait_ipi_idle();
    
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_FIXED | ICR_DST_SELF |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

// Send INIT IPI to a CPU
void lapic_send_init(uint32_t apic_id) {
    lapic_wait_ipi_idle();
    
    // Set destination
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send INIT IPI (assert level)
    lapic_write(LAPIC_ICR_LOW,
                ICR_DM_INIT | ICR_DST_PHYSICAL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    
    lapic_wait_ipi_idle();
    
    // Deassert INIT IPI
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,
                ICR_DM_INIT | ICR_DST_PHYSICAL |
                ICR_LEVEL_DEASSERT | ICR_TRIGGER_LEVEL);
    
    lapic_wait_ipi_idle();
}

// Send STARTUP IPI (SIPI) to a CPU
void lapic_send_startup(uint32_t apic_id, uint8_t vector) {
    lapic_wait_ipi_idle();
    
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,
                vector | ICR_DM_STARTUP | ICR_DST_PHYSICAL |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
    
    lapic_wait_ipi_idle();
}

// ============================================================================
// Local APIC Timer
// ============================================================================

// Delay using busy-wait (rough approximation using CPU cycles)
static void lapic_delay_us(uint32_t us) {
    // Simple busy-wait delay
    // This is approximate and depends on CPU speed
    for (volatile uint32_t i = 0; i < us * 1000; i++) {
        pause();
    }
}

// Calibrate APIC timer using PIT
void lapic_timer_calibrate(void) {
    kprintf("[APIC] Calibrating APIC timer...\n");
    
    // Configure timer divide register
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    
    // Start timer with maximum count
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    
    // Wait for 10ms using a simple delay
    // In production, use PIT or HPET for accurate timing
    lapic_delay_us(10000);
    
    // Stop timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    
    // Calculate ticks elapsed
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    
    // Calculate ticks per microsecond (we measured 10ms = 10000us)
    lapic_ticks_per_us = elapsed / 10000;
    
    kprintf("[APIC] Timer calibration: %u ticks/us (with div 16)\n", lapic_ticks_per_us);
}

// Initialize APIC timer for periodic interrupts
void lapic_timer_init(uint32_t frequency_hz) {
    if (lapic_ticks_per_us == 0) {
        lapic_timer_calibrate();
    }
    
    // Calculate initial count for desired frequency
    // Period in microseconds = 1000000 / frequency_hz
    uint32_t period_us = 1000000 / frequency_hz;
    uint32_t initial_count = period_us * lapic_ticks_per_us;
    
    // Configure timer: periodic mode, not masked
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, initial_count);
    
    kprintf("[APIC] Timer configured: %u Hz, ICR=%u\n", frequency_hz, initial_count);
}

// One-shot timer
void lapic_timer_oneshot(uint32_t microseconds) {
    if (lapic_ticks_per_us == 0) {
        lapic_timer_calibrate();
    }
    
    uint32_t count = microseconds * lapic_ticks_per_us;
    
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, TIMER_VECTOR | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_ICR, count);
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
    
    // Mask all IRQ entries initially
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
