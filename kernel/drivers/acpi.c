// acpi.c - ACPI (Advanced Configuration and Power Interface) driver implementation
#include "acpi.h"
#include "../boot_info.h"
#include "../serial.h"
#include "../string.h"

// Keyboard controller ports for fallback reset
#define KB_DATA_PORT    0x60
#define KB_CMD_PORT     0x64
#define KB_RESET_CMD    0xFE

// #298: cross-module hooks for the power-button SCI path.
#include "../cpu/idt.h"
extern void pic_enable_irq(uint8_t irq);
extern void pic_send_eoi(uint8_t irq);
extern int  fat_cache_flush(void *fs);   // flush dirty FAT readahead cache
extern void ata_flush_all(void);         // issue CACHE FLUSH to all ATA drives

// ACPI global state
static acpi_state_t acpi_state = {0};

// Forward declarations
static bool validate_rsdp(acpi_rsdp_t *rsdp);
static bool validate_table(acpi_sdt_header_t *header);
static int parse_rsdt(acpi_rsdt_t *rsdt);
static int parse_xsdt(acpi_xsdt_t *xsdt);
static int parse_fadt(acpi_fadt_t *fadt);
static void parse_dsdt_for_s5(uint8_t *dsdt, uint32_t length);

// Simple memory comparison (since we're freestanding)
static int acpi_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1;
    const uint8_t *p2 = s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

// Validate RSDP checksum
static bool validate_rsdp(acpi_rsdp_t *rsdp) {
    uint8_t sum = 0;
    uint8_t *ptr = (uint8_t *)rsdp;

    // First 20 bytes for ACPI 1.0
    for (int i = 0; i < 20; i++) {
        sum += ptr[i];
    }

    if (sum != 0) {
        return false;
    }

    // For ACPI 2.0+, also validate extended checksum
    if (rsdp->revision >= 2) {
        acpi_rsdp_ext_t *rsdp_ext = (acpi_rsdp_ext_t *)rsdp;
        sum = 0;
        for (uint32_t i = 0; i < rsdp_ext->length; i++) {
            sum += ptr[i];
        }
        if (sum != 0) {
            return false;
        }
    }

    return true;
}

// Validate ACPI table checksum
static bool validate_table(acpi_sdt_header_t *header) {
    uint8_t sum = 0;
    uint8_t *ptr = (uint8_t *)header;

    for (uint32_t i = 0; i < header->length; i++) {
        sum += ptr[i];
    }

    return sum == 0;
}

// Parse the DSDT to find S5 sleep type values
static void parse_dsdt_for_s5(uint8_t *dsdt, uint32_t length) {
    // Look for "_S5_" or "\_S5_" in the DSDT
    // The S5 package contains SLP_TYPa and SLP_TYPb values
    // Format: _S5_ Package (N) { SLP_TYPa, SLP_TYPb, ... }

    for (uint32_t i = 0; i < length - 5; i++) {
        // Look for "_S5_" pattern
        if ((dsdt[i] == '_' && dsdt[i+1] == 'S' && dsdt[i+2] == '5' && dsdt[i+3] == '_') ||
            (dsdt[i] == '\\' && dsdt[i+1] == '_' && dsdt[i+2] == 'S' && dsdt[i+3] == '5' && dsdt[i+4] == '_')) {

            // Skip the name
            uint32_t j = (dsdt[i] == '\\') ? i + 5 : i + 4;

            // Skip any name path or method opcodes
            while (j < length && (dsdt[j] == 0x08 || dsdt[j] == 0x14)) {
                j++;
            }

            // Look for Package opcode (0x12)
            while (j < length - 4 && dsdt[j] != 0x12) {
                j++;
                if (j > i + 20) break;  // Don't search too far
            }

            if (j < length - 4 && dsdt[j] == 0x12) {
                j++;  // Skip Package opcode

                // Skip PkgLength (variable length encoding)
                if (dsdt[j] & 0xC0) {
                    // Multi-byte length
                    int extra = (dsdt[j] >> 6) & 0x03;
                    j += extra + 1;
                } else {
                    j++;  // Single byte length
                }

                // Skip NumElements
                j++;

                // Now we should have the SLP_TYP values
                // They can be ByteData (0x0A prefix) or just raw values
                uint16_t slp_typa = 0;
                uint16_t slp_typb = 0;

                // Read SLP_TYPa
                if (dsdt[j] == 0x0A) {  // BytePrefix
                    slp_typa = dsdt[j + 1];
                    j += 2;
                } else if (dsdt[j] == 0x0B) {  // WordPrefix
                    slp_typa = dsdt[j + 1] | (dsdt[j + 2] << 8);
                    j += 3;
                } else {
                    slp_typa = dsdt[j];
                    j++;
                }

                // Read SLP_TYPb
                if (dsdt[j] == 0x0A) {  // BytePrefix
                    slp_typb = dsdt[j + 1];
                } else if (dsdt[j] == 0x0B) {  // WordPrefix
                    slp_typb = dsdt[j + 1] | (dsdt[j + 2] << 8);
                } else {
                    slp_typb = dsdt[j];
                }

                acpi_state.slp_typa = slp_typa;
                acpi_state.slp_typb = slp_typb;

                kprintf("[ACPI] Found S5 sleep type: SLP_TYPa=%u, SLP_TYPb=%u\n",
                        slp_typa, slp_typb);
                return;
            }
        }
    }

    // If not found, use common default values
    // Many systems use 5 or 7 for S5 state
    kprintf("[ACPI] S5 sleep type not found in DSDT, using defaults\n");
    acpi_state.slp_typa = 5;
    acpi_state.slp_typb = 5;
}

// Parse FADT
static int parse_fadt(acpi_fadt_t *fadt) {
    if (!validate_table(&fadt->header)) {
        kprintf("[ACPI] FADT checksum invalid\n");
        return -1;
    }

    acpi_state.fadt = fadt;

    kprintf("[ACPI] FADT found:\n");
    kprintf("[ACPI]   PM1a Control Block: 0x%x\n", fadt->pm1a_cnt_blk);
    kprintf("[ACPI]   PM1b Control Block: 0x%x\n", fadt->pm1b_cnt_blk);
    kprintf("[ACPI]   SMI Command Port:   0x%x\n", fadt->smi_cmd);

    // #298: capture the PM1 event blocks + SCI line so we can arm the power
    // button. We have seen BOCHS/QEMU firmware ship a garbage X_pm1a_evt_blk
    // (0xafafafaf), so trust the legacy 16-bit I/O field first and only fall
    // back to the 64-bit X_ address when it is both present and a plausible
    // I/O port (< 0x10000). The legacy field is what QEMU i440fx fills in.
    {
        uint32_t legacy_a = fadt->pm1a_evt_blk;
        uint32_t legacy_b = fadt->pm1b_evt_blk;
        uint64_t xa = (acpi_state.revision >= 2) ? fadt->x_pm1a_evt_blk.address : 0;
        uint64_t xb = (acpi_state.revision >= 2) ? fadt->x_pm1b_evt_blk.address : 0;
        if (legacy_a != 0 && legacy_a < 0x10000) {
            acpi_state.pm1a_evt_blk = legacy_a;
            acpi_state.pm1b_evt_blk = (legacy_b < 0x10000) ? legacy_b : 0;
        } else if (xa != 0 && xa < 0x10000) {
            acpi_state.pm1a_evt_blk = (uint32_t)xa;
            acpi_state.pm1b_evt_blk = (xb != 0 && xb < 0x10000) ? (uint32_t)xb : 0;
        } else {
            acpi_state.pm1a_evt_blk = 0;
            acpi_state.pm1b_evt_blk = 0;
        }
    }
    acpi_state.pm1_evt_len   = fadt->pm1_evt_len;
    acpi_state.smi_cmd       = fadt->smi_cmd;
    acpi_state.acpi_enable_val = fadt->acpi_enable;
    acpi_state.sci_int       = (uint8_t)(fadt->sci_int & 0xFF);
    kprintf("[ACPI]   PM1a Event Block:   0x%x (len=%u)\n",
            acpi_state.pm1a_evt_blk, acpi_state.pm1_evt_len);
    kprintf("[ACPI]   SCI Interrupt:      %u\n", acpi_state.sci_int);

    // Check for reset register support
    if (fadt->header.length >= 129) {  // FADT revision 2+ has reset register
        if (fadt->flags & ACPI_FADT_RESET_REG_SUP) {
            acpi_state.reset_supported = true;
            kprintf("[ACPI]   Reset Register:     0x%lx (space=%u, width=%u)\n",
                    fadt->reset_reg.address,
                    fadt->reset_reg.address_space_id,
                    fadt->reset_reg.register_bit_width);
        }
    }

    // Get DSDT address
    uint64_t dsdt_addr = 0;
    if (acpi_state.revision >= 2 && fadt->x_dsdt != 0) {
        dsdt_addr = fadt->x_dsdt;
    } else if (fadt->dsdt != 0) {
        dsdt_addr = fadt->dsdt;
    }

    if (dsdt_addr != 0) {
        acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)(uintptr_t)dsdt_addr;
        if (validate_table(dsdt)) {
            kprintf("[ACPI] DSDT found at 0x%lx, length=%u\n",
                    dsdt_addr, dsdt->length);
            // Parse DSDT for S5 sleep state values
            parse_dsdt_for_s5((uint8_t *)dsdt + sizeof(acpi_sdt_header_t),
                             dsdt->length - sizeof(acpi_sdt_header_t));
        } else {
            kprintf("[ACPI] DSDT checksum invalid\n");
        }
    }

    return 0;
}

// Parse RSDT (32-bit table pointers)
static int parse_rsdt(acpi_rsdt_t *rsdt) {
    if (!validate_table(&rsdt->header)) {
        kprintf("[ACPI] RSDT checksum invalid\n");
        return -1;
    }

    acpi_state.rsdt = rsdt;

    uint32_t entries = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    kprintf("[ACPI] RSDT at 0x%p, %u entries\n", (void *)rsdt, entries);

    // Find FADT
    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)(uintptr_t)rsdt->entries[i];

        if (acpi_memcmp(header->signature, ACPI_SIG_FADT, 4) == 0) {
            return parse_fadt((acpi_fadt_t *)header);
        }
    }

    kprintf("[ACPI] FADT not found in RSDT\n");
    return -1;
}

// Parse XSDT (64-bit table pointers)
static int parse_xsdt(acpi_xsdt_t *xsdt) {
    if (!validate_table(&xsdt->header)) {
        kprintf("[ACPI] XSDT checksum invalid\n");
        return -1;
    }

    acpi_state.xsdt = xsdt;

    uint32_t entries = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
    kprintf("[ACPI] XSDT at 0x%p, %u entries\n", (void *)xsdt, entries);

    // Find FADT
    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)xsdt->entries[i];

        if (acpi_memcmp(header->signature, ACPI_SIG_FADT, 4) == 0) {
            return parse_fadt((acpi_fadt_t *)header);
        }
    }

    kprintf("[ACPI] FADT not found in XSDT\n");
    return -1;
}

// Initialize ACPI subsystem
int acpi_init(void) {
    kprintf("[ACPI] Initializing ACPI subsystem...\n");

    // Check if boot info is available
    if (g_boot_info == NULL) {
        kprintf("[ACPI] Error: Boot info not available\n");
        return -1;
    }

    // Check if RSDP address is available
    if (g_boot_info->acpi.rsdp_address == 0) {
        kprintf("[ACPI] Error: RSDP address not provided by bootloader\n");
        return -1;
    }

    // Get RSDP pointer
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)(uintptr_t)g_boot_info->acpi.rsdp_address;

    kprintf("[ACPI] RSDP at 0x%lx\n", g_boot_info->acpi.rsdp_address);

    // Validate RSDP signature
    if (acpi_memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) != 0) {
        kprintf("[ACPI] Error: Invalid RSDP signature\n");
        return -1;
    }

    // Validate RSDP checksum
    if (!validate_rsdp(rsdp)) {
        kprintf("[ACPI] Error: Invalid RSDP checksum\n");
        return -1;
    }

    acpi_state.revision = rsdp->revision;
    kprintf("[ACPI] ACPI revision: %u\n", rsdp->revision);

    // Print OEM ID
    char oem_id[7] = {0};
    for (int i = 0; i < 6; i++) {
        oem_id[i] = rsdp->oem_id[i];
    }
    kprintf("[ACPI] OEM ID: %s\n", oem_id);

    int result;

    // For ACPI 2.0+, prefer XSDT over RSDT
    if (rsdp->revision >= 2) {
        acpi_rsdp_ext_t *rsdp_ext = (acpi_rsdp_ext_t *)rsdp;
        if (rsdp_ext->xsdt_address != 0) {
            result = parse_xsdt((acpi_xsdt_t *)rsdp_ext->xsdt_address);
            if (result == 0) {
                acpi_state.initialized = true;
                kprintf("[ACPI] Initialization complete (using XSDT)\n");
                return 0;
            }
        }
    }

    // Fall back to RSDT
    if (rsdp->rsdt_address != 0) {
        result = parse_rsdt((acpi_rsdt_t *)(uintptr_t)rsdp->rsdt_address);
        if (result == 0) {
            acpi_state.initialized = true;
            kprintf("[ACPI] Initialization complete (using RSDT)\n");
            return 0;
        }
    }

    kprintf("[ACPI] Error: Failed to parse ACPI tables\n");
    return -1;
}

// #298: flush everything we can before powering off so in-RAM/driver caches
// reach the disk. The ext2 path is already write-through, but the FAT
// readahead cache and the ATA drive write caches still benefit from a flush.
void acpi_shutdown_flush(void) {
    fat_cache_flush(0);   // flush any dirty FAT readahead blocks
    ata_flush_all();      // tell each ATA drive to commit its write cache
}

// #298: SCI interrupt handler. QEMU/Proxmox "qm shutdown" presses the virtual
// ACPI power button, which raises the SCI (legacy IRQ). We acknowledge the
// power-button status, flush filesystems, then enter S5 (soft off).
static void acpi_sci_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t irq = acpi_state.sci_int ? acpi_state.sci_int : 9;

    uint16_t sts = 0;
    if (acpi_state.pm1a_evt_blk)
        sts |= inw((uint16_t)acpi_state.pm1a_evt_blk);
    if (acpi_state.pm1b_evt_blk)
        sts |= inw((uint16_t)acpi_state.pm1b_evt_blk);

    if (sts & ACPI_PWRBTN_STS) {
        // Acknowledge the status (write-1-to-clear) on both event blocks.
        if (acpi_state.pm1a_evt_blk)
            outw((uint16_t)acpi_state.pm1a_evt_blk, ACPI_PWRBTN_STS);
        if (acpi_state.pm1b_evt_blk)
            outw((uint16_t)acpi_state.pm1b_evt_blk, ACPI_PWRBTN_STS);

        pic_send_eoi(irq);
        kprintf("[ACPI] Power button pressed -> flushing + powering off\n");
        acpi_shutdown_flush();
        acpi_shutdown();   // does not return on success
        return;
    }

    // Some other SCI source (GPE etc.) - just clear what we can and EOI so the
    // line does not stay asserted and wedge the box.
    if (acpi_state.pm1a_evt_blk)
        outw((uint16_t)acpi_state.pm1a_evt_blk, sts);
    if (acpi_state.pm1b_evt_blk)
        outw((uint16_t)acpi_state.pm1b_evt_blk, sts);
    pic_send_eoi(irq);
}

// #298: enable ACPI mode (if the chipset needs it) and arm the power button.
void acpi_enable_power_button(void) {
    if (!acpi_state.initialized || acpi_state.fadt == NULL) {
        kprintf("[ACPI] power button: ACPI not initialized, skipping\n");
        return;
    }
    if (acpi_state.pm1a_evt_blk == 0) {
        kprintf("[ACPI] power button: no PM1 event block, skipping\n");
        return;
    }

    // Step 1: switch to ACPI mode if a SMI command + enable value are provided.
    // Skip if SCI is already enabled (QEMU often boots with ACPI mode on).
    uint16_t cnt = (uint16_t)acpi_state.fadt->pm1a_cnt_blk;
    if (acpi_state.revision >= 2 && acpi_state.fadt->x_pm1a_cnt_blk.address != 0)
        cnt = (uint16_t)acpi_state.fadt->x_pm1a_cnt_blk.address;
    if (acpi_state.smi_cmd != 0 && acpi_state.acpi_enable_val != 0) {
        if (cnt && (inw(cnt) & ACPI_SCI_EN)) {
            // already in ACPI mode
        } else {
            outb((uint16_t)acpi_state.smi_cmd, acpi_state.acpi_enable_val);
            // Wait (bounded) for SCI_EN to come up.
            for (int i = 0; i < 100000 && cnt; i++) {
                if (inw(cnt) & ACPI_SCI_EN) break;
                io_wait();
            }
        }
    }

    // Step 2: clear any stale power-button status (write-1-to-clear).
    outw((uint16_t)acpi_state.pm1a_evt_blk, ACPI_PWRBTN_STS);
    if (acpi_state.pm1b_evt_blk)
        outw((uint16_t)acpi_state.pm1b_evt_blk, ACPI_PWRBTN_STS);

    // Step 3: enable the power-button event in the PM1 enable register, which
    // sits at event_block + (event_len / 2).
    uint8_t half = acpi_state.pm1_evt_len ? (acpi_state.pm1_evt_len / 2) : 2;
    uint16_t en_a = (uint16_t)(acpi_state.pm1a_evt_blk + half);
    uint16_t cur = inw(en_a);
    outw(en_a, (uint16_t)(cur | ACPI_PWRBTN_EN));
    if (acpi_state.pm1b_evt_blk) {
        uint16_t en_b = (uint16_t)(acpi_state.pm1b_evt_blk + half);
        outw(en_b, (uint16_t)(inw(en_b) | ACPI_PWRBTN_EN));
    }

    // Step 4: install + unmask the SCI IRQ (legacy PIC, vector = IRQ + 32).
    uint8_t irq = acpi_state.sci_int ? acpi_state.sci_int : 9;
    idt_register_handler(32 + irq, acpi_sci_handler);
    pic_enable_irq(irq);
    acpi_state.sci_armed = true;
    kprintf("[ACPI] Power button armed (SCI IRQ %u, PM1 en 0x%x)\n", irq, en_a);
}

// Shutdown the system (power off)
void acpi_shutdown(void) {
    kprintf("[ACPI] Initiating system shutdown...\n");

    // #298: commit FS/drive caches before we cut power. Idempotent if the SCI
    // handler already flushed.
    acpi_shutdown_flush();

    cli();

    // Preferred path: real ACPI S5 (soft-off) via the PM1 control registers.
    // SLP_TYPx is bits 10-12, SLP_EN is bit 13.
    if (acpi_state.initialized && acpi_state.fadt != NULL) {
        uint32_t pm1a_cnt = 0, pm1b_cnt = 0;
        if (acpi_state.revision >= 2 && acpi_state.fadt->x_pm1a_cnt_blk.address != 0) {
            pm1a_cnt = (uint32_t)acpi_state.fadt->x_pm1a_cnt_blk.address;
            pm1b_cnt = (uint32_t)acpi_state.fadt->x_pm1b_cnt_blk.address;
        } else {
            pm1a_cnt = acpi_state.fadt->pm1a_cnt_blk;
            pm1b_cnt = acpi_state.fadt->pm1b_cnt_blk;
        }
        if (pm1a_cnt != 0) {
            uint16_t v = (acpi_state.slp_typa << 10) | ACPI_SLP_EN;
            kprintf("[ACPI] S5 -> PM1a 0x%x = 0x%x\n", pm1a_cnt, v);
            outw(pm1a_cnt, v);
            if (pm1b_cnt != 0)
                outw(pm1b_cnt, (acpi_state.slp_typb << 10) | ACPI_SLP_EN);
        }
    } else {
        kprintf("[ACPI] not initialized; trying emulator power-off ports\n");
    }

    // Fallbacks for emulators / chipsets where the parsed S5 path did not power
    // off (QEMU is what this VM uses). These are harmless no-ops on real HW.
    for (int i = 0; i < 100000; i++) io_wait();
    outw(0x604,  0x2000);   // QEMU >= 2.0 (PIIX4/ICH9 ACPI PM base 0x600)
    outw(0xB004, 0x2000);   // older QEMU / Bochs
    outw(0x4004, 0x3400);   // VirtualBox

    kprintf("[ACPI] Shutdown failed, system still running\n");
}

// Reboot the system
void acpi_reboot(void) {
    kprintf("[ACPI] Initiating system reboot...\n");

    // Disable interrupts
    cli();

    // Try ACPI reset register first (if supported)
    if (acpi_state.initialized && acpi_state.reset_supported && acpi_state.fadt != NULL) {
        acpi_gas_t *reset_reg = &acpi_state.fadt->reset_reg;

        if (reset_reg->address != 0) {
            kprintf("[ACPI] Using ACPI reset register at 0x%lx\n", reset_reg->address);

            switch (reset_reg->address_space_id) {
                case 0:  // Memory space
                    *(volatile uint8_t *)(uintptr_t)reset_reg->address = acpi_state.fadt->reset_value;
                    break;
                case 1:  // I/O space
                    outb((uint16_t)reset_reg->address, acpi_state.fadt->reset_value);
                    break;
                default:
                    kprintf("[ACPI] Unsupported reset register address space: %u\n",
                            reset_reg->address_space_id);
                    break;
            }

            // Wait for reset
            for (int i = 0; i < 1000000; i++) {
                io_wait();
            }
        }
    }

    // Fallback: Use keyboard controller reset (8042)
    kprintf("[ACPI] Fallback: Using keyboard controller reset\n");

    // Wait for keyboard controller to be ready
    uint8_t status;
    int timeout = 100000;
    do {
        status = inb(KB_CMD_PORT);
        if (--timeout == 0) break;
    } while (status & 0x02);  // Wait for input buffer to be empty

    // Send reset command
    outb(KB_CMD_PORT, KB_RESET_CMD);

    // Wait for reset
    for (int i = 0; i < 1000000; i++) {
        io_wait();
    }

    // If we get here, keyboard controller reset didn't work
    // Try triple fault as last resort
    kprintf("[ACPI] Fallback: Using triple fault reset\n");

    // Load null IDT and trigger interrupt
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};

    __asm__ volatile(
        "lidt %0\n"
        "int $3"
        : : "m"(null_idt)
    );

    // Should never reach here
    for (;;) {
        hlt();
    }
}

// Check if ACPI is initialized
bool acpi_is_initialized(void) {
    return acpi_state.initialized;
}

// Get ACPI revision
uint8_t acpi_get_revision(void) {
    return acpi_state.revision;
}

// Find an ACPI table by signature
acpi_sdt_header_t *acpi_find_table(const char *signature) {
    if (!acpi_state.initialized) {
        return NULL;
    }

    // Search XSDT if available
    if (acpi_state.xsdt != NULL) {
        uint32_t entries = (acpi_state.xsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
        for (uint32_t i = 0; i < entries; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_state.xsdt->entries[i];
            if (acpi_memcmp(header->signature, signature, 4) == 0) {
                if (validate_table(header)) {
                    return header;
                }
            }
        }
    }

    // Fall back to RSDT
    if (acpi_state.rsdt != NULL) {
        uint32_t entries = (acpi_state.rsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
        for (uint32_t i = 0; i < entries; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t *)(uintptr_t)acpi_state.rsdt->entries[i];
            if (acpi_memcmp(header->signature, signature, 4) == 0) {
                if (validate_table(header)) {
                    return header;
                }
            }
        }
    }

    return NULL;
}
