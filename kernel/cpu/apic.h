// apic.h - Local APIC and I/O APIC driver for MayteraOS
// Part of Task #41 (SMP Support)
//
// The Advanced Programmable Interrupt Controller (APIC) is the modern
// interrupt controller for x86. Each CPU has a Local APIC for inter-processor
// interrupts (IPIs), local interrupts (timer, thermal, etc.). The system has
// one or more I/O APICs for routing external device interrupts.

#ifndef APIC_H
#define APIC_H

#include "../types.h"

// ============================================================================
// Local APIC Register Offsets (memory-mapped)
// ============================================================================

#define LAPIC_ID            0x020   // Local APIC ID
#define LAPIC_VERSION       0x030   // Local APIC Version
#define LAPIC_TPR           0x080   // Task Priority Register
#define LAPIC_APR           0x090   // Arbitration Priority Register
#define LAPIC_PPR           0x0A0   // Processor Priority Register
#define LAPIC_EOI           0x0B0   // End Of Interrupt
#define LAPIC_RRD           0x0C0   // Remote Read Register
#define LAPIC_LDR           0x0D0   // Logical Destination Register
#define LAPIC_DFR           0x0E0   // Destination Format Register
#define LAPIC_SVR           0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_ISR           0x100   // In-Service Register (8 regs)
#define LAPIC_TMR           0x180   // Trigger Mode Register (8 regs)
#define LAPIC_IRR           0x200   // Interrupt Request Register (8 regs)
#define LAPIC_ESR           0x280   // Error Status Register
#define LAPIC_ICR_LOW       0x300   // Interrupt Command Register (low)
#define LAPIC_ICR_HIGH      0x310   // Interrupt Command Register (high)
#define LAPIC_LVT_TIMER     0x320   // LVT Timer Register
#define LAPIC_LVT_THERMAL   0x330   // LVT Thermal Sensor Register
#define LAPIC_LVT_PERF      0x340   // LVT Performance Counter Register
#define LAPIC_LVT_LINT0     0x350   // LVT LINT0 Register
#define LAPIC_LVT_LINT1     0x360   // LVT LINT1 Register
#define LAPIC_LVT_ERROR     0x370   // LVT Error Register
#define LAPIC_TIMER_ICR     0x380   // Timer Initial Count Register
#define LAPIC_TIMER_CCR     0x390   // Timer Current Count Register
#define LAPIC_TIMER_DCR     0x3E0   // Timer Divide Configuration Register

// ============================================================================
// Spurious Vector Register Bits
// ============================================================================

#define LAPIC_SVR_ENABLE    (1 << 8)    // APIC Software Enable
#define LAPIC_SVR_VECTOR    0xFF        // Spurious vector mask

// ============================================================================
// LVT Entry Bits
// ============================================================================

#define LAPIC_LVT_MASKED    (1 << 16)   // Interrupt masked
#define LAPIC_LVT_PENDING   (1 << 12)   // Delivery status (RO)

// Delivery modes
#define LAPIC_DM_FIXED      (0 << 8)    // Fixed
#define LAPIC_DM_SMI        (2 << 8)    // SMI
#define LAPIC_DM_NMI        (4 << 8)    // NMI
#define LAPIC_DM_INIT       (5 << 8)    // INIT
#define LAPIC_DM_EXTINT     (7 << 8)    // External interrupt

// Timer modes
#define LAPIC_TIMER_ONESHOT     (0 << 17)   // One-shot mode
#define LAPIC_TIMER_PERIODIC    (1 << 17)   // Periodic mode
#define LAPIC_TIMER_TSC         (2 << 17)   // TSC-Deadline mode

// Timer divider values
#define LAPIC_TIMER_DIV_1       0xB
#define LAPIC_TIMER_DIV_2       0x0
#define LAPIC_TIMER_DIV_4       0x1
#define LAPIC_TIMER_DIV_8       0x2
#define LAPIC_TIMER_DIV_16      0x3
#define LAPIC_TIMER_DIV_32      0x8
#define LAPIC_TIMER_DIV_64      0x9
#define LAPIC_TIMER_DIV_128     0xA

// ============================================================================
// Interrupt Command Register (ICR) Bits
// ============================================================================

// Delivery mode (bits 10:8)
#define ICR_DM_FIXED        (0 << 8)
#define ICR_DM_LOWEST       (1 << 8)
#define ICR_DM_SMI          (2 << 8)
#define ICR_DM_NMI          (4 << 8)
#define ICR_DM_INIT         (5 << 8)
#define ICR_DM_STARTUP      (6 << 8)

// Destination mode (bit 11)
#define ICR_DST_PHYSICAL    (0 << 11)
#define ICR_DST_LOGICAL     (1 << 11)

// Delivery status (bit 12) - read only
#define ICR_DS_PENDING      (1 << 12)

// Level (bit 14)
#define ICR_LEVEL_DEASSERT  (0 << 14)
#define ICR_LEVEL_ASSERT    (1 << 14)

// Trigger mode (bit 15)
#define ICR_TRIGGER_EDGE    (0 << 15)
#define ICR_TRIGGER_LEVEL   (1 << 15)

// Destination shorthand (bits 19:18)
#define ICR_DST_NONE        (0 << 18)   // Use destination field
#define ICR_DST_SELF        (1 << 18)   // Self
#define ICR_DST_ALL         (2 << 18)   // All including self
#define ICR_DST_OTHERS      (3 << 18)   // All excluding self

// ============================================================================
// I/O APIC Registers
// ============================================================================

#define IOAPIC_IOREGSEL     0x00    // Register select (index)
#define IOAPIC_IOWIN        0x10    // Register data

// I/O APIC registers (via IOREGSEL/IOWIN)
#define IOAPIC_ID           0x00    // I/O APIC ID
#define IOAPIC_VER          0x01    // I/O APIC Version
#define IOAPIC_ARB          0x02    // Arbitration ID
#define IOAPIC_REDTBL_BASE  0x10    // Redirection Table base (24 entries, 2 regs each)

// Redirection table entry bits (64-bit)
#define IOAPIC_REDIR_MASKED     (1ULL << 16)
#define IOAPIC_REDIR_TRIGGER    (1ULL << 15)    // 1=level, 0=edge
#define IOAPIC_REDIR_INTPOL     (1ULL << 13)    // 1=low active, 0=high active
#define IOAPIC_REDIR_DESTMODE   (1ULL << 11)    // 1=logical, 0=physical
#define IOAPIC_REDIR_DM_FIXED   (0ULL << 8)
#define IOAPIC_REDIR_DM_LOWEST  (1ULL << 8)
#define IOAPIC_REDIR_DM_NMI     (4ULL << 8)

// ============================================================================
// IPI Types
// ============================================================================

// Inter-Processor Interrupt vectors
#define IPI_VECTOR_RESCHEDULE   0xF0    // Reschedule request
#define IPI_VECTOR_CALL         0xF1    // Function call
#define IPI_VECTOR_TLB          0xF2    // TLB shootdown
#define IPI_VECTOR_STOP         0xF3    // Stop CPU (for panic/debug)
#define IPI_VECTOR_WAKEUP       0xF4    // Wake from halt

// ============================================================================
// Local APIC Functions
// ============================================================================

// Initialize the BSP's Local APIC
// This enables the APIC and configures basic settings
int lapic_init(void);

// Initialize an AP's Local APIC (called by AP after startup)
void lapic_init_ap(void);

// Get the APIC ID of the current CPU
uint32_t lapic_get_id(void);

// Read from a Local APIC register
uint32_t lapic_read(uint32_t offset);

// Write to a Local APIC register
void lapic_write(uint32_t offset, uint32_t value);

// Send End-Of-Interrupt signal
void lapic_eoi(void);

// ============================================================================
// Inter-Processor Interrupts (IPI)
// ============================================================================

// Send IPI to a specific CPU (by APIC ID)
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);

// Send IPI to all CPUs except self
void lapic_send_ipi_all_excluding_self(uint32_t vector);

// Send IPI to all CPUs including self
void lapic_send_ipi_all(uint32_t vector);

// Send IPI to self
void lapic_send_ipi_self(uint32_t vector);

// Send INIT IPI to a CPU (for AP startup)
void lapic_send_init(uint32_t apic_id);

// Send STARTUP IPI to a CPU (for AP startup)
// vector is the page number of the startup code (e.g., 0x08 for 0x8000)
void lapic_send_startup(uint32_t apic_id, uint8_t vector);

// Wait for IPI delivery to complete
void lapic_wait_ipi_idle(void);

// ============================================================================
// Local APIC Timer
// ============================================================================

// Initialize the APIC timer with specified frequency (Hz)
void lapic_timer_init(uint32_t frequency_hz);

// One-shot timer for specified microseconds
void lapic_timer_oneshot(uint32_t microseconds);

// Stop the APIC timer
void lapic_timer_stop(void);

// Get current timer count
uint32_t lapic_timer_current(void);

// APIC timer calibration (uses PIT for reference)
void lapic_timer_calibrate(void);

// ============================================================================
// I/O APIC Functions
// ============================================================================

// Initialize the I/O APIC
int ioapic_init(void);

// Read from I/O APIC register
uint32_t ioapic_read(uint32_t index);

// Write to I/O APIC register
void ioapic_write(uint32_t index, uint32_t value);

// Route an IRQ to a CPU
// irq: Legacy IRQ number (0-23)
// vector: IDT vector to deliver
// apic_id: Target CPU APIC ID (or 0xFF for any CPU)
// flags: Polarity/trigger mode flags
void ioapic_route_irq(uint32_t irq, uint32_t vector, uint32_t apic_id, uint32_t flags);

// Mask (disable) an IRQ
void ioapic_mask_irq(uint32_t irq);

// Unmask (enable) an IRQ
void ioapic_unmask_irq(uint32_t irq);

// Get the I/O APIC ID
uint32_t ioapic_get_id(void);

// Get the number of supported IRQs
uint32_t ioapic_get_max_redirs(void);

// ============================================================================
// APIC State
// ============================================================================

// Check if Local APIC is available on this CPU
bool lapic_is_available(void);

// Check if x2APIC mode is supported
bool lapic_x2apic_supported(void);

// Enable x2APIC mode (if supported)
int lapic_enable_x2apic(void);

// Get the Local APIC base address
uint64_t lapic_get_base(void);

// Set the Local APIC base address (and enable)
void lapic_set_base(uint64_t base);

#endif // APIC_H
