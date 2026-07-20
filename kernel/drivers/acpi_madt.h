// acpi_madt.h - MADT (Multiple APIC Description Table) parsing for SMP support
// Part of MayteraOS ACPI subsystem - Task #49
#ifndef ACPI_MADT_H
#define ACPI_MADT_H

#include "../types.h"
#include "acpi.h"

// MADT signature
#define ACPI_SIG_MADT   "APIC"

// MADT entry types
#define MADT_TYPE_LOCAL_APIC            0
#define MADT_TYPE_IO_APIC               1
#define MADT_TYPE_INTERRUPT_OVERRIDE    2
#define MADT_TYPE_NMI_SOURCE            3
#define MADT_TYPE_LOCAL_APIC_NMI        4
#define MADT_TYPE_LOCAL_APIC_OVERRIDE   5
#define MADT_TYPE_IO_SAPIC              6
#define MADT_TYPE_LOCAL_SAPIC           7
#define MADT_TYPE_PLATFORM_INT_SOURCE   8
#define MADT_TYPE_LOCAL_X2APIC          9
#define MADT_TYPE_LOCAL_X2APIC_NMI      10
#define MADT_TYPE_GIC_CPU               11

// MADT flags
#define MADT_FLAG_PCAT_COMPAT           (1 << 0)

// Local APIC flags
#define LAPIC_FLAG_ENABLED              (1 << 0)
#define LAPIC_FLAG_ONLINE_CAPABLE       (1 << 1)

// Maximum supported CPUs
#define MAX_CPUS                        256

// Maximum I/O APICs
#define MAX_IO_APICS                    8

// MADT header structure
typedef struct {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    // Variable-length entries follow
} __attribute__((packed)) acpi_madt_t;

// Generic MADT entry header
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

// Type 0: Processor Local APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_local_apic_t;

// Type 1: I/O APIC
typedef struct {
    madt_entry_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) madt_io_apic_t;

// Type 2: Interrupt Source Override
typedef struct {
    madt_entry_header_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) madt_interrupt_override_t;

// Type 3: NMI Source
typedef struct {
    madt_entry_header_t header;
    uint16_t flags;
    uint32_t global_system_interrupt;
} __attribute__((packed)) madt_nmi_source_t;

// Type 4: Local APIC NMI
typedef struct {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint16_t flags;
    uint8_t lint_number;
} __attribute__((packed)) madt_local_apic_nmi_t;

// Type 5: Local APIC Address Override
typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t local_apic_address;
} __attribute__((packed)) madt_local_apic_override_t;

// Type 9: Processor Local x2APIC
typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_processor_uid;
} __attribute__((packed)) madt_local_x2apic_t;

// CPU information structure
typedef struct {
    uint32_t apic_id;           // APIC ID
    uint8_t acpi_id;            // ACPI Processor ID
    uint8_t is_bsp;             // Bootstrap processor flag
    uint8_t is_enabled;         // Processor enabled flag
    uint8_t is_x2apic;          // x2APIC mode flag
    uint8_t started;            // CPU has been started
    uint8_t reserved[3];
} cpu_info_t;

// I/O APIC information structure
typedef struct {
    uint8_t id;                 // I/O APIC ID
    uint32_t address;           // I/O APIC base address
    uint32_t gsi_base;          // Global System Interrupt base
} io_apic_info_t;

// Interrupt override information
typedef struct {
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} interrupt_override_t;

// MADT state structure
typedef struct {
    bool parsed;                            // MADT successfully parsed
    uint64_t local_apic_address;            // Local APIC address (may be overridden)
    uint32_t flags;                         // MADT flags
    
    // CPU information
    uint32_t cpu_count;                     // Total number of CPUs
    uint32_t enabled_cpu_count;             // Number of enabled CPUs
    cpu_info_t cpus[MAX_CPUS];              // CPU information array
    uint32_t bsp_apic_id;                   // BSP APIC ID
    
    // I/O APIC information
    uint32_t io_apic_count;                 // Number of I/O APICs
    io_apic_info_t io_apics[MAX_IO_APICS];  // I/O APIC information
    
    // Interrupt overrides
    uint32_t override_count;
    interrupt_override_t overrides[24];     // ISA IRQ overrides
    
    // NMI information
    uint8_t nmi_lint;                       // Local APIC NMI LINT# (0 or 1)
    uint16_t nmi_flags;                     // NMI flags
} madt_state_t;

// Initialize MADT parsing
// Returns 0 on success, -1 on failure
int madt_init(void);

// Get total number of CPUs (enabled + disabled)
uint32_t madt_get_cpu_count(void);

// Get number of enabled CPUs
uint32_t madt_get_enabled_cpu_count(void);

// Get CPU information by index
cpu_info_t *madt_get_cpu(uint32_t index);

// Get BSP APIC ID
uint32_t madt_get_bsp_apic_id(void);

// Get Local APIC address
uint64_t madt_get_local_apic_address(void);

// Get I/O APIC count
uint32_t madt_get_io_apic_count(void);

// Get I/O APIC information by index
io_apic_info_t *madt_get_io_apic(uint32_t index);

// Translate ISA IRQ to GSI (handles overrides)
uint32_t madt_irq_to_gsi(uint8_t irq);

// Get interrupt override flags for an IRQ
uint16_t madt_get_irq_flags(uint8_t irq);

// Check if MADT was parsed successfully
bool madt_is_initialized(void);

// Print MADT information (for debugging)
void madt_print_info(void);

#endif // ACPI_MADT_H
