// acpi_madt.c - MADT (Multiple APIC Description Table) parsing for SMP support
// Part of MayteraOS ACPI subsystem - Task #49
// 
// The MADT provides information about:
// - Local APICs (one per CPU)
// - I/O APICs (interrupt routing)
// - Interrupt source overrides (ISA remapping)
// - NMI configuration
//
// This is critical for SMP support - we need to know how many CPUs exist
// and their APIC IDs before we can start them.

#include "acpi_madt.h"
#include "../serial.h"
#include "../string.h"

// MADT global state
static madt_state_t madt_state = {0};

// Get current APIC ID (to identify BSP)
static inline uint32_t get_current_apic_id(void) {
    // Read APIC ID from Local APIC register at offset 0x20
    // For x2APIC mode, use RDMSR instead
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x01, &eax, &ebx, &ecx, &edx);
    // APIC ID is in bits 24-31 of EBX
    return (ebx >> 24) & 0xFF;
}

// Parse Local APIC entry (Type 0)
static void parse_local_apic(madt_local_apic_t *entry) {
    if (madt_state.cpu_count >= MAX_CPUS) {
        kprintf("[MADT] Warning: Max CPU count exceeded\n");
        return;
    }
    
    cpu_info_t *cpu = &madt_state.cpus[madt_state.cpu_count];
    cpu->apic_id = entry->apic_id;
    cpu->acpi_id = entry->acpi_processor_id;
    cpu->is_enabled = (entry->flags & LAPIC_FLAG_ENABLED) ? 1 : 0;
    cpu->is_x2apic = 0;
    cpu->started = 0;
    
    // Check if this is the BSP
    if (entry->apic_id == get_current_apic_id()) {
        cpu->is_bsp = 1;
        madt_state.bsp_apic_id = entry->apic_id;
    } else {
        cpu->is_bsp = 0;
    }
    
    if (cpu->is_enabled) {
        madt_state.enabled_cpu_count++;
    }
    
    madt_state.cpu_count++;
    
    kprintf("[MADT]   Local APIC: ACPI ID=%u, APIC ID=%u, %s%s\n",
            entry->acpi_processor_id,
            entry->apic_id,
            cpu->is_enabled ? "enabled" : "disabled",
            cpu->is_bsp ? " (BSP)" : "");
}

// Parse I/O APIC entry (Type 1)
static void parse_io_apic(madt_io_apic_t *entry) {
    if (madt_state.io_apic_count >= MAX_IO_APICS) {
        kprintf("[MADT] Warning: Max I/O APIC count exceeded\n");
        return;
    }
    
    io_apic_info_t *io_apic = &madt_state.io_apics[madt_state.io_apic_count];
    io_apic->id = entry->io_apic_id;
    io_apic->address = entry->io_apic_address;
    io_apic->gsi_base = entry->global_system_interrupt_base;
    
    madt_state.io_apic_count++;
    
    kprintf("[MADT]   I/O APIC: ID=%u, Address=0x%08x, GSI Base=%u\n",
            entry->io_apic_id,
            entry->io_apic_address,
            entry->global_system_interrupt_base);
}

// Parse Interrupt Source Override entry (Type 2)
static void parse_interrupt_override(madt_interrupt_override_t *entry) {
    if (madt_state.override_count >= 24) {
        kprintf("[MADT] Warning: Max interrupt override count exceeded\n");
        return;
    }
    
    interrupt_override_t *override = &madt_state.overrides[madt_state.override_count];
    override->bus = entry->bus;
    override->source_irq = entry->source;
    override->gsi = entry->global_system_interrupt;
    override->flags = entry->flags;
    
    madt_state.override_count++;
    
    kprintf("[MADT]   IRQ Override: Bus=%u, Source IRQ=%u -> GSI=%u, Flags=0x%04x\n",
            entry->bus,
            entry->source,
            entry->global_system_interrupt,
            entry->flags);
}

// Parse Local APIC NMI entry (Type 4)
static void parse_local_apic_nmi(madt_local_apic_nmi_t *entry) {
    // ACPI processor ID 0xFF means all processors
    if (entry->acpi_processor_id == 0xFF || entry->acpi_processor_id == 0) {
        madt_state.nmi_lint = entry->lint_number;
        madt_state.nmi_flags = entry->flags;
        
        kprintf("[MADT]   Local APIC NMI: Processor=%s, LINT#=%u, Flags=0x%04x\n",
                entry->acpi_processor_id == 0xFF ? "ALL" : "BSP",
                entry->lint_number,
                entry->flags);
    }
}

// Parse Local APIC Address Override entry (Type 5)
static void parse_local_apic_override(madt_local_apic_override_t *entry) {
    madt_state.local_apic_address = entry->local_apic_address;
    
    kprintf("[MADT]   Local APIC Address Override: 0x%016lx\n",
            entry->local_apic_address);
}

// Parse x2APIC entry (Type 9)
static void parse_local_x2apic(madt_local_x2apic_t *entry) {
    if (madt_state.cpu_count >= MAX_CPUS) {
        kprintf("[MADT] Warning: Max CPU count exceeded\n");
        return;
    }
    
    cpu_info_t *cpu = &madt_state.cpus[madt_state.cpu_count];
    cpu->apic_id = entry->x2apic_id;
    cpu->acpi_id = (uint8_t)(entry->acpi_processor_uid & 0xFF);
    cpu->is_enabled = (entry->flags & LAPIC_FLAG_ENABLED) ? 1 : 0;
    cpu->is_x2apic = 1;
    cpu->started = 0;
    
    // Check if this is the BSP
    uint32_t current_id = get_current_apic_id();
    if (entry->x2apic_id == current_id) {
        cpu->is_bsp = 1;
        madt_state.bsp_apic_id = entry->x2apic_id;
    } else {
        cpu->is_bsp = 0;
    }
    
    if (cpu->is_enabled) {
        madt_state.enabled_cpu_count++;
    }
    
    madt_state.cpu_count++;
    
    kprintf("[MADT]   x2APIC: UID=%u, x2APIC ID=%u, %s%s\n",
            entry->acpi_processor_uid,
            entry->x2apic_id,
            cpu->is_enabled ? "enabled" : "disabled",
            cpu->is_bsp ? " (BSP)" : "");
}

// Initialize MADT parsing
int madt_init(void) {
    kprintf("[MADT] Parsing Multiple APIC Description Table...\n");
    
    // Check if ACPI is initialized
    if (!acpi_is_initialized()) {
        kprintf("[MADT] Error: ACPI not initialized\n");
        return -1;
    }
    
    // Find MADT table
    acpi_sdt_header_t *header = acpi_find_table(ACPI_SIG_MADT);
    if (header == NULL) {
        kprintf("[MADT] Error: MADT not found\n");
        return -1;
    }
    
    acpi_madt_t *madt = (acpi_madt_t *)header;
    
    kprintf("[MADT] Found MADT at 0x%p, length=%u\n", (void *)madt, madt->header.length);
    
    // Initialize state
    memset(&madt_state, 0, sizeof(madt_state));
    
    // Get Local APIC address
    madt_state.local_apic_address = madt->local_apic_address;
    madt_state.flags = madt->flags;
    
    kprintf("[MADT] Local APIC Address: 0x%08x\n", madt->local_apic_address);
    kprintf("[MADT] Flags: 0x%08x%s\n", madt->flags,
            (madt->flags & MADT_FLAG_PCAT_COMPAT) ? " (PC/AT compatible)" : "");
    
    // Parse entries
    uint8_t *ptr = (uint8_t *)madt + sizeof(acpi_madt_t);
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    
    kprintf("[MADT] Parsing entries...\n");
    
    while (ptr < end) {
        madt_entry_header_t *entry = (madt_entry_header_t *)ptr;
        
        if (entry->length == 0) {
            kprintf("[MADT] Warning: Entry with zero length, stopping\n");
            break;
        }
        
        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC:
                parse_local_apic((madt_local_apic_t *)entry);
                break;
                
            case MADT_TYPE_IO_APIC:
                parse_io_apic((madt_io_apic_t *)entry);
                break;
                
            case MADT_TYPE_INTERRUPT_OVERRIDE:
                parse_interrupt_override((madt_interrupt_override_t *)entry);
                break;
                
            case MADT_TYPE_NMI_SOURCE:
                kprintf("[MADT]   NMI Source (type 3) - skipped\n");
                break;
                
            case MADT_TYPE_LOCAL_APIC_NMI:
                parse_local_apic_nmi((madt_local_apic_nmi_t *)entry);
                break;
                
            case MADT_TYPE_LOCAL_APIC_OVERRIDE:
                parse_local_apic_override((madt_local_apic_override_t *)entry);
                break;
                
            case MADT_TYPE_LOCAL_X2APIC:
                parse_local_x2apic((madt_local_x2apic_t *)entry);
                break;
                
            case MADT_TYPE_LOCAL_X2APIC_NMI:
                kprintf("[MADT]   x2APIC NMI (type 10) - skipped\n");
                break;
                
            default:
                kprintf("[MADT]   Unknown entry type %u, length %u\n",
                        entry->type, entry->length);
                break;
        }
        
        ptr += entry->length;
    }
    
    madt_state.parsed = true;
    
    kprintf("[MADT] Parsing complete:\n");
    kprintf("[MADT]   Total CPUs: %u\n", madt_state.cpu_count);
    kprintf("[MADT]   Enabled CPUs: %u\n", madt_state.enabled_cpu_count);
    kprintf("[MADT]   I/O APICs: %u\n", madt_state.io_apic_count);
    kprintf("[MADT]   Interrupt Overrides: %u\n", madt_state.override_count);
    kprintf("[MADT]   BSP APIC ID: %u\n", madt_state.bsp_apic_id);
    
    return 0;
}

// Get total number of CPUs
uint32_t madt_get_cpu_count(void) {
    return madt_state.cpu_count;
}

// Get number of enabled CPUs
uint32_t madt_get_enabled_cpu_count(void) {
    return madt_state.enabled_cpu_count;
}

// Get CPU information by index
cpu_info_t *madt_get_cpu(uint32_t index) {
    if (index >= madt_state.cpu_count) {
        return NULL;
    }
    return &madt_state.cpus[index];
}

// Get BSP APIC ID
uint32_t madt_get_bsp_apic_id(void) {
    return madt_state.bsp_apic_id;
}

// Get Local APIC address
uint64_t madt_get_local_apic_address(void) {
    return madt_state.local_apic_address;
}

// Get I/O APIC count
uint32_t madt_get_io_apic_count(void) {
    return madt_state.io_apic_count;
}

// Get I/O APIC information by index
io_apic_info_t *madt_get_io_apic(uint32_t index) {
    if (index >= madt_state.io_apic_count) {
        return NULL;
    }
    return &madt_state.io_apics[index];
}

// Translate ISA IRQ to GSI (handles overrides)
uint32_t madt_irq_to_gsi(uint8_t irq) {
    // Check for override
    for (uint32_t i = 0; i < madt_state.override_count; i++) {
        if (madt_state.overrides[i].source_irq == irq) {
            return madt_state.overrides[i].gsi;
        }
    }
    // No override - IRQ maps directly to GSI
    return irq;
}

// Get interrupt override flags for an IRQ
uint16_t madt_get_irq_flags(uint8_t irq) {
    for (uint32_t i = 0; i < madt_state.override_count; i++) {
        if (madt_state.overrides[i].source_irq == irq) {
            return madt_state.overrides[i].flags;
        }
    }
    // Default flags (active high, edge triggered)
    return 0;
}

// Check if MADT was parsed successfully
bool madt_is_initialized(void) {
    return madt_state.parsed;
}

// Print MADT information
void madt_print_info(void) {
    kprintf("\n[MADT] ====== CPU and APIC Information ======\n");
    
    if (!madt_state.parsed) {
        kprintf("[MADT] MADT not parsed\n");
        return;
    }
    
    kprintf("[MADT] Local APIC Address: 0x%016lx\n", madt_state.local_apic_address);
    
    kprintf("\n[MADT] CPUs (%u total, %u enabled):\n",
            madt_state.cpu_count, madt_state.enabled_cpu_count);
    kprintf("[MADT]   %-4s  %-8s  %-8s  %-7s  %-7s\n",
            "#", "ACPI ID", "APIC ID", "Enabled", "BSP");
    kprintf("[MADT]   ----  --------  --------  -------  -------\n");
    
    for (uint32_t i = 0; i < madt_state.cpu_count; i++) {
        cpu_info_t *cpu = &madt_state.cpus[i];
        kprintf("[MADT]   %-4u  %-8u  %-8u  %-7s  %-7s\n",
                i,
                cpu->acpi_id,
                cpu->apic_id,
                cpu->is_enabled ? "yes" : "no",
                cpu->is_bsp ? "yes" : "no");
    }
    
    kprintf("\n[MADT] I/O APICs (%u):\n", madt_state.io_apic_count);
    kprintf("[MADT]   %-4s  %-12s  %-10s\n", "ID", "Address", "GSI Base");
    kprintf("[MADT]   ----  ------------  ----------\n");
    
    for (uint32_t i = 0; i < madt_state.io_apic_count; i++) {
        io_apic_info_t *io_apic = &madt_state.io_apics[i];
        kprintf("[MADT]   %-4u  0x%08x    %-10u\n",
                io_apic->id,
                io_apic->address,
                io_apic->gsi_base);
    }
    
    if (madt_state.override_count > 0) {
        kprintf("\n[MADT] Interrupt Overrides (%u):\n", madt_state.override_count);
        kprintf("[MADT]   %-6s  %-5s  %-6s\n", "Source", "GSI", "Flags");
        kprintf("[MADT]   ------  -----  ------\n");
        
        for (uint32_t i = 0; i < madt_state.override_count; i++) {
            interrupt_override_t *override = &madt_state.overrides[i];
            kprintf("[MADT]   IRQ %-2u  %-5u  0x%04x\n",
                    override->source_irq,
                    override->gsi,
                    override->flags);
        }
    }
    
    kprintf("[MADT] ==========================================\n\n");
}
