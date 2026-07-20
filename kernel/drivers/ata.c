// ata.c - ATA/IDE disk driver implementation (PIO and DMA modes)
#include "ata.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../sync/spinlock.h"
#include "ahci.h"

// Global I/O serialization lock. The DMA path uses a single shared bounce
// buffer (dma_buffer) and per-channel PRD tables, and every transfer drives
// the same Bus-Master IDE registers. Without serialization, two processes
// issuing disk I/O concurrently (e.g. the compositor reading a multi-megabyte
// wallpaper while a background service writes its log) overlap in the shared
// dma_buffer, so one transfer's data lands in the other's caller buffer --
// observed as a wallpaper made of fragments of several files with wrong
// colors, the corruption accumulating as more transfers collide. This lock
// makes each transfer atomic (irqsave covers SMP + local preemption). It is
// acquired only at the outermost public entry points so the internal DMA->PIO
// fallback does not deadlock.
//
// #444: this used to be a plain `static spinlock_t g_ata_io_lock;` paired
// with a lazily-run `ata_io_lock_init_once()` that did
// `if (!g_ata_io_lock_ready) { spinlock_init(&g_ata_io_lock); g_ata_io_lock_ready = 1; }`
// on every call. That check-then-act is NOT atomic: if two threads (e.g. two
// CPython-spawned readers hammering ext2 at boot) both reach it while
// g_ata_io_lock_ready is still 0, BOTH call spinlock_init() -- and if one of
// them does so *after* the other has already progressed to
// spinlock_acquire_irqsave() and started a real DMA transfer, the reinit
// stomps `locked` back to 0 out from under the lock holder, so the second
// thread's acquire succeeds immediately even though the first transfer is
// still in flight. Two DMA reads/writes then share the single global
// dma_buffer/PRD table/Bus-Master registers concurrently: one thread's
// half-finished sectors land as zero-filled holes in the other's buffer (the
// #444 CPython corruption), and a torn/overwritten PRD physical address can
// point the drive's DMA engine at the wrong page entirely, corrupting an
// unrelated kernel structure and producing the GPF/page-fault seen at
// different RIPs. Fixed the only correct way: statically initialize the lock
// (SPINLOCK_INIT, same pattern as net/net.c g_net_lock and sync/futex.c
// waiter_pool_lock) so it is valid from the first instruction that can ever
// run, with zero runtime init and therefore zero init race window.
static spinlock_t g_ata_io_lock = SPINLOCK_INIT;

// Drive information
static ata_drive_t drives[4];

// #307 AHCI/SATA dispatch. When an AHCI controller with attached SATA disks is
// present (e.g. on a q35 machine, or real hardware), the AHCI driver owns the
// disk. We register each AHCI SATA disk into a free drives[] slot so the rest
// of the kernel (fat_init, ext2, get_first_drive) keeps using the same
// (channel,drive) identifiers, and route block I/O for those slots to AHCI.
// g_ahci_slot_port[idx] == -1 means the slot is legacy IDE (or empty); >=0 is
// the AHCI physical port number that backs logical disk slot idx.
static int g_ahci_slot_port[4] = { -1, -1, -1, -1 };
static int g_ahci_active = 0;  // 1 once at least one AHCI disk is registered

// True if logical disk slot (channel,drive) is backed by AHCI.
static inline int disk_slot_is_ahci(uint8_t channel, uint8_t drive) {
    if (channel > 1 || drive > 1) return 0;
    int idx = channel * 2 + drive;
    return g_ahci_slot_port[idx] >= 0;
}  // [0]=Primary Master, [1]=Primary Slave,
                               // [2]=Secondary Master, [3]=Secondary Slave

// I/O port bases (can be overridden by PCI BAR)
static uint16_t io_base[2] = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
static uint16_t ctrl_base[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };

// Bus Master IDE base address (from BAR4)
static uint16_t bmide_base = 0;

// PRD tables for DMA (one per channel, aligned to 4-byte boundary)
// Each PRD table can hold up to 8 entries (64 bytes total)
#define PRD_MAX_ENTRIES     8
#define PRD_TABLE_SIZE      (PRD_MAX_ENTRIES * sizeof(prd_entry_t))
static prd_entry_t *prd_tables[2] = { NULL, NULL };
static uint64_t prd_tables_phys[2] = { 0, 0 };

// DMA transfer buffer (for non-contiguous physical memory)
// 64KB max per DMA transfer
#define DMA_BUFFER_SIZE     (64 * 1024)
static void *dma_buffer = NULL;
static uint64_t dma_buffer_phys = 0;

// DMA available flag
static int dma_enabled = 0;

// Check for PCI IDE controller and update port addresses
static void ata_detect_pci_ide(void) {
    // Look for PCI IDE controller (class 0x01, subclass 0x01)
    pci_device_t *ide = pci_find_class(0x01, 0x01);
    if (!ide) {
        // Also check for SATA controller (class 0x01, subclass 0x06)
        ide = pci_find_class(0x01, 0x06);
    }

    if (ide) {
        kprintf("[ATA] Found PCI storage controller: %04x:%04x at %02x:%02x.%x\n",
                ide->vendor_id, ide->device_id, ide->bus, ide->slot, ide->func);
        kprintf("[ATA]   Class: %02x.%02x, ProgIF: %02x\n",
                ide->class_code, ide->subclass, ide->prog_if);

        // Print all BARs
        for (int i = 0; i < 6; i++) {
            if (ide->bar[i] != 0) {
                kprintf("[ATA]   BAR%d: 0x%08x\n", i, ide->bar[i]);
            }
        }

        // Check programming interface for native mode vs compatibility mode
        // Bits 0-1: Primary channel mode (0=compatibility, 1=native)
        // Bits 2-3: Secondary channel mode
        if (ide->prog_if & 0x01) {
            // Primary in native mode - use BAR0/BAR1
            uint32_t bar0 = ide->bar[0] & ~0x3;  // I/O base
            uint32_t bar1 = ide->bar[1] & ~0x3;  // Control base
            if (bar0 && bar1) {
                io_base[0] = bar0;
                ctrl_base[0] = bar1 + 2;  // Control register is at offset 2
                kprintf("[ATA]   Primary native mode: I/O=0x%x, Ctrl=0x%x\n",
                        io_base[0], ctrl_base[0]);
            }
        } else {
            kprintf("[ATA]   Primary in compatibility mode (0x1F0/0x3F6)\n");
        }

        if (ide->prog_if & 0x04) {
            // Secondary in native mode - use BAR2/BAR3
            uint32_t bar2 = ide->bar[2] & ~0x3;
            uint32_t bar3 = ide->bar[3] & ~0x3;
            if (bar2 && bar3) {
                io_base[1] = bar2;
                ctrl_base[1] = bar3 + 2;
                kprintf("[ATA]   Secondary native mode: I/O=0x%x, Ctrl=0x%x\n",
                        io_base[1], ctrl_base[1]);
            }
        } else {
            kprintf("[ATA]   Secondary in compatibility mode (0x170/0x376)\n");
        }

        // Enable bus mastering and I/O space
        pci_enable_bus_master(ide);

        // Get Bus Master IDE base from BAR4
        if (ide->bar[4] != 0) {
            bmide_base = ide->bar[4] & ~0x3;  // I/O base (clear lower bits)
            kprintf("[ATA]   Bus Master IDE base: 0x%04x\n", bmide_base);

            // Read BMIDE status to check DMA capability
            uint8_t bmide_status_pri = inb(bmide_base + BMIDE_STATUS);
            uint8_t bmide_status_sec = inb(bmide_base + 8 + BMIDE_STATUS);
            kprintf("[ATA]   BMIDE status: Primary=0x%02x, Secondary=0x%02x\n",
                    bmide_status_pri, bmide_status_sec);

            // Allocate PRD tables (must be physically contiguous and aligned)
            // Using PMM to get physical pages - kernel uses identity mapping
            uint64_t prd_page = pmm_alloc_page();
            if (prd_page) {
                // Identity mapping: virtual = physical for kernel addresses
                prd_tables[0] = (prd_entry_t *)prd_page;
                prd_tables_phys[0] = prd_page;
                prd_tables[1] = (prd_entry_t *)(prd_page + PRD_TABLE_SIZE);
                prd_tables_phys[1] = prd_page + PRD_TABLE_SIZE;

                kprintf("[ATA]   PRD tables allocated at 0x%lx\n", prd_page);

                // Allocate DMA buffer (64KB = 16 pages)
                uint64_t dma_page = pmm_alloc_pages(DMA_BUFFER_SIZE / 4096);
                if (dma_page) {
                    dma_buffer = (void *)dma_page;  // Identity mapping
                    dma_buffer_phys = dma_page;
                    kprintf("[ATA]   DMA buffer allocated at 0x%lx (%d KB)\n",
                            dma_page, DMA_BUFFER_SIZE / 1024);
                    dma_enabled = 1;
                } else {
                    kprintf("[ATA]   WARNING: Failed to allocate DMA buffer\n");
                }
            } else {
                kprintf("[ATA]   WARNING: Failed to allocate PRD tables\n");
            }
        } else {
            kprintf("[ATA]   No Bus Master IDE (BAR4=0), DMA not available\n");
        }
    } else {
        kprintf("[ATA] No PCI IDE/SATA controller found, using legacy ports\n");
    }
}

// Wait for BSY to clear
static int ata_wait_bsy(uint16_t io) {
    int timeout = 100000;
    while ((inb(io + ATA_REG_STATUS) & ATA_SR_BSY) && timeout > 0) {
        timeout--;
    }
    return timeout > 0 ? 0 : -1;
}

// Wait for DRQ to set
static int ata_wait_drq(uint16_t io) {
    int timeout = 100000;
    while (!(inb(io + ATA_REG_STATUS) & ATA_SR_DRQ) && timeout > 0) {
        timeout--;
    }
    return timeout > 0 ? 0 : -1;
}

// Soft reset channel
static void ata_soft_reset(uint8_t channel) {
    uint16_t ctrl = ctrl_base[channel];

    // Set SRST bit
    outb(ctrl + ATA_REG_DEVCTRL, 0x04);
    io_wait();
    io_wait();
    io_wait();
    io_wait();

    // Clear SRST bit
    outb(ctrl + ATA_REG_DEVCTRL, 0x00);
    io_wait();
}

// Select drive
static void ata_select_drive(uint8_t channel, uint8_t drive) {
    uint16_t io = io_base[channel];
    outb(io + ATA_REG_DRIVE, drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER);

    // 400ns delay (read status 4 times)
    inb(io + ATA_REG_STATUS);
    inb(io + ATA_REG_STATUS);
    inb(io + ATA_REG_STATUS);
    inb(io + ATA_REG_STATUS);
}

// Identify drive
static int ata_identify(uint8_t channel, uint8_t drive, uint16_t *buffer) {
    uint16_t io = io_base[channel];

    // Select drive
    ata_select_drive(channel, drive);

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA]     IDENTIFY: BSY timeout before command\n");
        return -1;
    }

    // Send IDENTIFY command
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    // Small delay after command
    io_wait();

    // Check if drive exists
    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0) {
        kprintf("[ATA]     IDENTIFY: No drive (status=0)\n");
        return -1;  // No drive
    }

    kprintf("[ATA]     IDENTIFY: status after cmd=0x%02x\n", status);

    // Wait for BSY to clear
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA]     IDENTIFY: BSY timeout after command\n");
        return -1;
    }

    // Check for ATAPI signature
    uint8_t lba_mid = inb(io + ATA_REG_LBA_MID);
    uint8_t lba_hi = inb(io + ATA_REG_LBA_HI);
    kprintf("[ATA]     IDENTIFY: sig LBA_MID=0x%02x, LBA_HI=0x%02x\n", lba_mid, lba_hi);

    if (lba_mid == 0x14 && lba_hi == 0xEB) {
        // ATAPI device - send IDENTIFY PACKET command
        kprintf("[ATA]     IDENTIFY: ATAPI device, sending IDENTIFY PACKET\n");
        outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        if (ata_wait_bsy(io) != 0) {
            kprintf("[ATA]     IDENTIFY: BSY timeout after IDENTIFY PACKET\n");
            return -1;
        }
    } else if (lba_mid == 0x3C && lba_hi == 0xC3) {
        // SATA device signature
        kprintf("[ATA]     IDENTIFY: SATA device detected\n");
    } else if (lba_mid != 0 || lba_hi != 0) {
        kprintf("[ATA]     IDENTIFY: Unknown device type (LBA_MID=0x%02x, LBA_HI=0x%02x)\n",
                lba_mid, lba_hi);
        return -1;  // Not ATA
    }

    // Check for error
    status = inb(io + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) {
        uint8_t err = inb(io + ATA_REG_ERROR);
        kprintf("[ATA]     IDENTIFY: Error (status=0x%02x, error=0x%02x)\n", status, err);
        return -1;
    }

    // Wait for DRQ
    if (ata_wait_drq(io) != 0) {
        kprintf("[ATA]     IDENTIFY: DRQ timeout (status=0x%02x)\n", inb(io + ATA_REG_STATUS));
        return -1;
    }

    kprintf("[ATA]     IDENTIFY: Reading 256 words...\n");

    // Read identify data (256 words = 512 bytes)
    for (int i = 0; i < 256; i++) {
        buffer[i] = inw(io + ATA_REG_DATA);
    }

    kprintf("[ATA]     IDENTIFY: Success!\n");
    return 0;
}

// SMART RETURN STATUS: returns 1 (healthy), 0 (failing), -1 (unknown/unsupported).
// Run once at boot (single-threaded) so there is no runtime ATA register race.
int ata_smart_status(uint8_t channel, uint8_t drive) {
    if (channel > 1 || drive > 1) return -1;
    uint16_t io = io_base[channel];
    ata_select_drive(channel, drive);
    if (ata_wait_bsy(io) != 0) return -1;
    outb(io + ATA_REG_FEATURES, ATA_SMART_RETURN_STATUS);
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0x4F);
    outb(io + ATA_REG_LBA_HI, 0xC2);
    outb(io + ATA_REG_COMMAND, ATA_CMD_SMART);
    io_wait();
    if (ata_wait_bsy(io) != 0) return -1;
    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0 || (status & 0x01)) return -1;  // no drive / ERR -> unsupported
    uint8_t mid = inb(io + ATA_REG_LBA_MID);
    uint8_t hi  = inb(io + ATA_REG_LBA_HI);
    if (mid == 0x4F && hi == 0xC2) return 1;   // threshold not exceeded -> healthy
    if (mid == 0xF4 && hi == 0x2C) return 0;   // threshold exceeded -> failing
    return -1;
}

// Index-based accessors (idx = channel*2 + drive).
int ata_drive_present(int idx) { return (idx >= 0 && idx < 4) ? drives[idx].exists : 0; }
int ata_drive_type(int idx)    { return (idx >= 0 && idx < 4) ? drives[idx].type : 0; }
int ata_drive_smart(int idx)   { return (idx >= 0 && idx < 4) ? drives[idx].smart_status : -1; }
unsigned long ata_drive_sectors(int idx) { return (idx >= 0 && idx < 4) ? (unsigned long)drives[idx].sectors : 0; }
const char *ata_drive_model(int idx)  { return (idx >= 0 && idx < 4) ? drives[idx].model : ""; }
const char *ata_drive_serial(int idx) { return (idx >= 0 && idx < 4) ? drives[idx].serial : ""; }

// #298: issue FLUSH CACHE to every present ATA (non-ATAPI) drive so any data
// sitting in a drive write cache is committed before power-off. Best-effort and
// bounded; never blocks forever (ata_wait_bsy has its own timeout).
void ata_flush_all(void) {
    for (int idx = 0; idx < 4; idx++) {
        if (!drives[idx].exists || drives[idx].type == ATA_TYPE_ATAPI) continue;
        uint8_t channel = drives[idx].channel;
        uint8_t drive   = drives[idx].drive;
        if (channel > 1) continue;
        uint16_t io = io_base[channel];
        outb(io + ATA_REG_DRIVE, drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER);
        io_wait();
        if (ata_wait_bsy(io) != 0) continue;
        outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ata_wait_bsy(io);
    }
}

// Check if an IDE channel exists by reading the status register
static int __attribute__((unused)) ata_channel_exists(uint8_t channel) {
    uint16_t io = io_base[channel];
    uint8_t status = inb(io + ATA_REG_STATUS);

    // If we read 0xFF, the channel doesn't exist (floating bus)
    if (status == 0xFF) {
        return 0;
    }
    return 1;
}

// Initialize ATA driver
void ata_init(void) {
    kprintf("[ATA] Initializing ATA driver...\n");

    // Explicitly initialize I/O port bases to default values
    // This is necessary because the bootloader may relocate .data section
    // to a different physical address than its virtual address, causing
    // static initializers to be inaccessible via their virtual addresses
    io_base[0] = ATA_PRIMARY_IO;
    io_base[1] = ATA_SECONDARY_IO;
    ctrl_base[0] = ATA_PRIMARY_CTRL;
    ctrl_base[1] = ATA_SECONDARY_CTRL;

    kprintf("[ATA] Default Primary channel I/O: 0x%x, Ctrl: 0x%x\n",
            ATA_PRIMARY_IO, ATA_PRIMARY_CTRL);
    kprintf("[ATA] Default Secondary channel I/O: 0x%x, Ctrl: 0x%x\n",
            ATA_SECONDARY_IO, ATA_SECONDARY_CTRL);

    // Check for PCI IDE controller and update ports if needed
    ata_detect_pci_ide();

    kprintf("[ATA] Using Primary I/O: 0x%x, Ctrl: 0x%x\n", io_base[0], ctrl_base[0]);
    kprintf("[ATA] Using Secondary I/O: 0x%x, Ctrl: 0x%x\n", io_base[1], ctrl_base[1]);

    uint16_t identify_buf[256];

    // Clear drive info
    memset(drives, 0, sizeof(drives));

    // Check all 4 possible drives
    for (int channel = 0; channel < 2; channel++) {
        uint16_t io = io_base[channel];

        // Check if channel exists
        kprintf("[ATA] Checking %s channel (I/O 0x%x)...\n",
                channel == 0 ? "Primary" : "Secondary", io);

        // Read status to check for floating bus
        uint8_t status = inb(io + ATA_REG_STATUS);
        kprintf("[ATA]   Initial status: 0x%02x\n", status);

        if (status == 0xFF) {
            kprintf("[ATA]   Channel not present (floating bus)\n");
            continue;
        }

        // Soft reset channel
        kprintf("[ATA]   Performing soft reset...\n");
        ata_soft_reset(channel);

        // Wait a bit after reset
        for (int i = 0; i < 10; i++) io_wait();

        status = inb(io + ATA_REG_STATUS);
        kprintf("[ATA]   Post-reset status: 0x%02x\n", status);

        for (int drv = 0; drv < 2; drv++) {
            int idx = channel * 2 + drv;
            drives[idx].channel = channel;
            drives[idx].drive = drv;

            kprintf("[ATA]   Probing %s...\n", drv == 0 ? "Master" : "Slave");

            // Select the drive
            ata_select_drive(channel, drv);

            // Read status after selection
            status = inb(io + ATA_REG_STATUS);
            kprintf("[ATA]     After select, status: 0x%02x\n", status);

            // Check signature registers for device type detection
            uint8_t lba_mid = inb(io + ATA_REG_LBA_MID);
            uint8_t lba_hi = inb(io + ATA_REG_LBA_HI);
            kprintf("[ATA]     LBA_MID=0x%02x, LBA_HI=0x%02x\n", lba_mid, lba_hi);

            if (ata_identify(channel, drv, identify_buf) == 0) {
                drives[idx].exists = 1;
                drives[idx].smart_status = -1;

                // Determine type (ATA vs ATAPI)
                lba_mid = inb(io + ATA_REG_LBA_MID);
                lba_hi = inb(io + ATA_REG_LBA_HI);

                if (lba_mid == 0x14 && lba_hi == 0xEB) {
                    drives[idx].type = ATA_TYPE_ATAPI;
                } else {
                    drives[idx].type = ATA_TYPE_ATA;
                }

                // Get drive info from identify data
                drives[idx].signature = identify_buf[0];
                drives[idx].capabilities = identify_buf[49];
                drives[idx].command_sets = (identify_buf[83] << 16) | identify_buf[82];

                // Get sector count
                if (drives[idx].command_sets & (1 << 26)) {
                    // LBA48 supported
                    drives[idx].sectors = ((uint64_t)identify_buf[103] << 48) |
                                         ((uint64_t)identify_buf[102] << 32) |
                                         ((uint64_t)identify_buf[101] << 16) |
                                         identify_buf[100];
                } else {
                    // LBA28 only
                    drives[idx].sectors = (identify_buf[61] << 16) | identify_buf[60];
                }

                // Get model string (words 27-46)
                for (int i = 0; i < 20; i++) {
                    drives[idx].model[i * 2] = identify_buf[27 + i] >> 8;
                    drives[idx].model[i * 2 + 1] = identify_buf[27 + i] & 0xFF;
                }
                drives[idx].model[40] = '\0';
                // Trim trailing spaces
                for (int i = 39; i >= 0 && drives[idx].model[i] == ' '; i--) {
                    drives[idx].model[i] = '\0';
                }

                // Get serial number (words 10-19)
                for (int i = 0; i < 10; i++) {
                    drives[idx].serial[i * 2] = identify_buf[10 + i] >> 8;
                    drives[idx].serial[i * 2 + 1] = identify_buf[10 + i] & 0xFF;
                }
                drives[idx].serial[20] = '\0';
                // Trim trailing spaces
                for (int i = 19; i >= 0 && drives[idx].serial[i] == ' '; i--) {
                    drives[idx].serial[i] = '\0';
                }

                kprintf("[ATA] FOUND: %s %s: %s\n",
                        channel == 0 ? "Primary" : "Secondary",
                        drv == 0 ? "Master" : "Slave",
                        drives[idx].type == ATA_TYPE_ATA ? "ATA" : "ATAPI");
                kprintf("      Model: %s\n", drives[idx].model);
                kprintf("      Serial: %s\n", drives[idx].serial);
                kprintf("      Signature: 0x%04x, Caps: 0x%04x\n",
                        drives[idx].signature, drives[idx].capabilities);

                if (drives[idx].type == ATA_TYPE_ATA) {
                    drives[idx].smart_status = (int8_t)ata_smart_status(channel, drv);
                    uint64_t size_mb = (drives[idx].sectors * ATA_SECTOR_SIZE) / (1024 * 1024);
                    kprintf("      Size: %lu MB (%lu sectors)\n", size_mb, drives[idx].sectors);
                    kprintf("      LBA48: %s\n",
                            (drives[idx].command_sets & (1 << 26)) ? "Yes" : "No");

                    // Check DMA capability (word 49, bit 8 = DMA supported)
                    if (drives[idx].capabilities & 0x0100) {
                        drives[idx].dma_capable = 1;
                        kprintf("      DMA: Supported\n");

                        // Check UDMA modes (word 88)
                        uint16_t udma_modes = identify_buf[88];
                        drives[idx].udma_mode = 0xFF;  // No UDMA by default
                        if (udma_modes & 0x40) {
                            drives[idx].udma_mode = 6;
                            kprintf("      UDMA: Mode 6 (133 MB/s)\n");
                        } else if (udma_modes & 0x20) {
                            drives[idx].udma_mode = 5;
                            kprintf("      UDMA: Mode 5 (100 MB/s)\n");
                        } else if (udma_modes & 0x10) {
                            drives[idx].udma_mode = 4;
                            kprintf("      UDMA: Mode 4 (66 MB/s)\n");
                        } else if (udma_modes & 0x08) {
                            drives[idx].udma_mode = 3;
                            kprintf("      UDMA: Mode 3 (44 MB/s)\n");
                        } else if (udma_modes & 0x04) {
                            drives[idx].udma_mode = 2;
                            kprintf("      UDMA: Mode 2 (33 MB/s)\n");
                        } else if (udma_modes & 0x02) {
                            drives[idx].udma_mode = 1;
                            kprintf("      UDMA: Mode 1 (25 MB/s)\n");
                        } else if (udma_modes & 0x01) {
                            drives[idx].udma_mode = 0;
                            kprintf("      UDMA: Mode 0 (16 MB/s)\n");
                        }
                    } else {
                        drives[idx].dma_capable = 0;
                        drives[idx].udma_mode = 0xFF;
                        kprintf("      DMA: Not supported\n");
                    }
                }
            } else {
                kprintf("[ATA]     No drive detected\n");
            }
        }
    }

    // #307: probe for an AHCI (SATA) controller and register its disks. This
    // runs after the legacy IDE probe so legacy drives keep their slots; AHCI
    // disks only take free slots. On q35/real hardware there is no legacy IDE,
    // so AHCI typically claims slot 0 (channel 0, drive 0) = the boot disk.
    if (ahci_init() == 0 && ahci_is_initialized()) {
        int n = 0;
        for (int idx = 0; idx < 4 && n < 4; idx++) {
            if (drives[idx].exists) continue;  // do not displace legacy IDE
            int port = ahci_get_nth_sata_port(n);
            if (port < 0) break;
            uint64_t secs = ahci_get_sector_count(port);
            drives[idx].exists      = 1;
            drives[idx].type        = ATA_TYPE_ATA;
            drives[idx].channel     = (uint8_t)(idx >> 1);
            drives[idx].drive       = (uint8_t)(idx & 1);
            drives[idx].dma_capable = 1;
            drives[idx].udma_mode   = 0xFF;
            drives[idx].sectors     = secs;
            drives[idx].smart_status = -1;
            const char *m = ahci_get_model(port);
            const char *sn = ahci_get_serial(port);
            if (m)  { strncpy(drives[idx].model, m, 40);  drives[idx].model[40] = 0; }
            if (sn) { strncpy(drives[idx].serial, sn, 20); drives[idx].serial[20] = 0; }
            g_ahci_slot_port[idx] = port;
            g_ahci_active = 1;
            kprintf("[DISK] slot %d (channel %d, drive %d) -> AHCI port %d, %lu sectors\n",
                    idx, drives[idx].channel, drives[idx].drive, port, (unsigned long)secs);
            n++;
        }
        if (g_ahci_active) {
            kprintf("[DISK] using AHCI backend for SATA disk(s)\n");
            // Prove WRITE DMA EXT + READ DMA EXT round-trip on real hardware.
            ahci_selftest();
            // Prove READ/WRITE FPDMA QUEUED (NCQ) round-trip through the
            // PxSACT-based completion fix (no-ops if NCQ unsupported).
            ahci_selftest_ncq();
        }
    } else {
        kprintf("[DISK] no AHCI controller; using legacy IDE backend\n");
    }

    kprintf("[ATA] ATA driver initialized\n");
}

// Get drive info
ata_drive_t *ata_get_drive(int channel, int drive) {
    int idx = channel * 2 + drive;
    if (idx < 0 || idx >= 4) return NULL;
    if (!drives[idx].exists) return NULL;
    return &drives[idx];
}

// Read sectors (LBA28)
static int ata_read_sectors_impl(uint8_t channel, uint8_t drive, uint32_t lba,
                     uint8_t count, void *buffer) {
    if (channel > 1 || drive > 1) {
        kprintf("[ATA] READ: Invalid channel %d or drive %d\n", channel, drive);
        return -1;
    }
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!drives[idx].exists) {
        kprintf("[ATA] READ: Drive %d/%d does not exist\n", channel, drive);
        return -1;
    }
    if (drives[idx].type != ATA_TYPE_ATA) {
        kprintf("[ATA] READ: Drive %d/%d is not ATA type (type=%d)\n",
                channel, drive, drives[idx].type);
        return -1;
    }

    uint16_t io = io_base[channel];
    uint16_t *buf = (uint16_t *)buffer;

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA] READ: BSY timeout before read (LBA %u)\n", lba);
        return -1;
    }

    // Select drive and set LBA mode
    outb(io + ATA_REG_DRIVE, (drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));

    // Small delay after drive select
    io_wait();

    // Send parameters
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send read command
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    // Read sectors
    for (int s = 0; s < count; s++) {
        // Wait for DRQ
        if (ata_wait_drq(io) != 0) {
            kprintf("[ATA] READ: DRQ timeout at sector %d (LBA %u)\n", s, lba + s);
            return -1;
        }

        // Check for error
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            uint8_t err = inb(io + ATA_REG_ERROR);
            kprintf("[ATA] READ: Error at sector %d (status=0x%02x, error=0x%02x)\n",
                    s, status, err);
            return -1;
        }

        // Read sector data (256 words) in a single REP INSW burst instead of
        // 256 individual serializing inw() port reads. Big PIO speedup.
        uint16_t *dst = &buf[s * 256];
        uint32_t words = 256;
        __asm__ volatile("cld; rep insw"
                         : "+D"(dst), "+c"(words)
                         : "d"((uint16_t)(io + ATA_REG_DATA))
                         : "memory");
    }

    return count;
}

// Write sectors (LBA28)
static int ata_write_sectors_impl(uint8_t channel, uint8_t drive, uint32_t lba,
                      uint8_t count, const void *buffer) {
    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!drives[idx].exists || drives[idx].type != ATA_TYPE_ATA) {
        return -1;
    }

    uint16_t io = io_base[channel];
    const uint16_t *buf = (const uint16_t *)buffer;

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        return -1;
    }

    // Select drive and set LBA mode
    outb(io + ATA_REG_DRIVE, (drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));

    // Send parameters
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send write command
    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    // Write sectors
    for (int s = 0; s < count; s++) {
        // Wait for DRQ
        if (ata_wait_drq(io) != 0) {
            return -1;
        }

        // Write sector data (256 words)
        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, buf[s * 256 + i]);
        }
    }

    // Flush cache
    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy(io);

    return count;
}

// Read sectors (LBA48)
static int ata_read_sectors_ext_impl(uint8_t channel, uint8_t drive, uint64_t lba,
                         uint16_t count, void *buffer) {
    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!drives[idx].exists || drives[idx].type != ATA_TYPE_ATA) {
        return -1;
    }

    // Check if LBA48 is supported
    if (!(drives[idx].command_sets & (1 << 26))) {
        // Fall back to LBA28 if possible (already under the I/O lock).
        if (lba <= 0x0FFFFFFF && count <= 255) {
            return ata_read_sectors_impl(channel, drive, (uint32_t)lba, (uint8_t)count, buffer);
        }
        return -1;
    }

    uint16_t io = io_base[channel];
    uint16_t *buf = (uint16_t *)buffer;

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        return -1;
    }

    // Select drive
    outb(io + ATA_REG_DRIVE, drive ? 0x50 : 0x40);

    // Send high bytes first
    outb(io + ATA_REG_SECCOUNT, (count >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_LO, (lba >> 24) & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 32) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 40) & 0xFF);

    // Then low bytes
    outb(io + ATA_REG_SECCOUNT, count & 0xFF);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send read command
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);

    // Read sectors
    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0) {
            return -1;
        }

        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            return -1;
        }

        for (int i = 0; i < 256; i++) {
            buf[s * 256 + i] = inw(io + ATA_REG_DATA);
        }
    }

    return count;
}

// Write sectors (LBA48)
static int ata_write_sectors_ext_impl(uint8_t channel, uint8_t drive, uint64_t lba,
                          uint16_t count, const void *buffer) {
    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!drives[idx].exists || drives[idx].type != ATA_TYPE_ATA) {
        return -1;
    }

    // Check if LBA48 is supported
    if (!(drives[idx].command_sets & (1 << 26))) {
        // Already under the I/O lock; call the unlocked impl.
        if (lba <= 0x0FFFFFFF && count <= 255) {
            return ata_write_sectors_impl(channel, drive, (uint32_t)lba, (uint8_t)count, buffer);
        }
        return -1;
    }

    uint16_t io = io_base[channel];
    const uint16_t *buf = (const uint16_t *)buffer;

    if (ata_wait_bsy(io) != 0) {
        return -1;
    }

    outb(io + ATA_REG_DRIVE, drive ? 0x50 : 0x40);

    outb(io + ATA_REG_SECCOUNT, (count >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_LO, (lba >> 24) & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 32) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 40) & 0xFF);

    outb(io + ATA_REG_SECCOUNT, count & 0xFF);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);

    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0) {
            return -1;
        }

        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, buf[s * 256 + i]);
        }
    }

    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH_EXT);
    ata_wait_bsy(io);

    return count;
}

// Print drive info
void ata_print_info(void) {
    kprintf("\n[ATA] Drive Information:\n");

    for (int i = 0; i < 4; i++) {
        if (drives[i].exists) {
            kprintf("  %s %s:\n",
                    drives[i].channel == 0 ? "Primary" : "Secondary",
                    drives[i].drive == 0 ? "Master" : "Slave");
            kprintf("    Type:  %s\n", drives[i].type == ATA_TYPE_ATA ? "ATA" : "ATAPI");
            kprintf("    Model: %s\n", drives[i].model);
            kprintf("    Serial: %s\n", drives[i].serial);

            if (drives[i].type == ATA_TYPE_ATA) {
                uint64_t size_mb = (drives[i].sectors * ATA_SECTOR_SIZE) / (1024 * 1024);
                kprintf("    Size:  %lu MB\n", size_mb);
                kprintf("    LBA48: %s\n",
                        (drives[i].command_sets & (1 << 26)) ? "Yes" : "No");
            }
        }
    }
}

// Test read from a specific drive at LBA 0
void ata_test_read(uint8_t channel, uint8_t drive) {
    static uint8_t test_buf[512];

    kprintf("[ATA] Testing read from channel %d, drive %d, LBA 0...\n", channel, drive);

    int result = ata_read_sectors(channel, drive, 0, 1, test_buf);
    if (result <= 0) {
        kprintf("[ATA] Test read FAILED!\n");
        return;
    }

    kprintf("[ATA] Test read SUCCESS! First 32 bytes:\n");
    kprintf("[ATA] ");
    for (int i = 0; i < 32; i++) {
        kprintf("%02x ", test_buf[i]);
        if ((i + 1) % 16 == 0) kprintf("\n[ATA] ");
    }
    kprintf("\n");

    // Check for MBR signature
    if (test_buf[510] == 0x55 && test_buf[511] == 0xAA) {
        kprintf("[ATA] MBR signature found (0x55AA)\n");

        // Check partition type at offset 0x1BE (first partition)
        uint8_t ptype = test_buf[0x1C2];  // Partition type at offset 0x1BE + 4
        kprintf("[ATA] First partition type: 0x%02x\n", ptype);

        if (ptype == 0xEE) {
            kprintf("[ATA] GPT protective MBR detected\n");
        }
    }

    // Check for GPT signature at LBA 1
    result = ata_read_sectors(channel, drive, 1, 1, test_buf);
    if (result > 0) {
        // GPT signature is "EFI PART" at offset 0
        if (test_buf[0] == 'E' && test_buf[1] == 'F' && test_buf[2] == 'I' &&
            test_buf[3] == ' ' && test_buf[4] == 'P' && test_buf[5] == 'A' &&
            test_buf[6] == 'R' && test_buf[7] == 'T') {
            kprintf("[ATA] GPT header found at LBA 1\n");
        }
    }

    // Test reading LBA 2048 (common EFI partition start)
    kprintf("[ATA] Testing read from LBA 2048 (EFI partition)...\n");
    result = ata_read_sectors(channel, drive, 2048, 1, test_buf);
    if (result > 0) {
        kprintf("[ATA] LBA 2048 read SUCCESS! First 16 bytes:\n");
        kprintf("[ATA] ");
        for (int i = 0; i < 16; i++) {
            kprintf("%02x ", test_buf[i]);
        }
        kprintf("\n");

        // Check for FAT boot sector signature
        if (test_buf[0] == 0xEB || test_buf[0] == 0xE9) {
            kprintf("[ATA] FAT boot sector jump instruction found\n");
            // Print OEM name (bytes 3-10)
            kprintf("[ATA] OEM Name: '");
            for (int i = 3; i < 11; i++) {
                kprintf("%c", test_buf[i] >= 0x20 && test_buf[i] < 0x7F ? test_buf[i] : '.');
            }
            kprintf("'\n");
        }

        // Check signature at end
        if (test_buf[510] == 0x55 && test_buf[511] == 0xAA) {
            kprintf("[ATA] Boot sector signature (0x55AA) found at LBA 2048\n");
        }
    } else {
        kprintf("[ATA] LBA 2048 read FAILED!\n");
    }
}

// Get the first available ATA drive
int ata_get_first_drive(uint8_t *channel_out, uint8_t *drive_out) {
    for (int i = 0; i < 4; i++) {
        if (drives[i].exists && drives[i].type == ATA_TYPE_ATA) {
            *channel_out = drives[i].channel;
            *drive_out = drives[i].drive;
            kprintf("[ATA] First ATA drive: channel %d, drive %d\n",
                    *channel_out, *drive_out);
            return 0;
        }
    }
    kprintf("[ATA] No ATA drives found!\n");
    return -1;
}

// ============================================================================
// DMA Mode Functions
// ============================================================================

// Check if DMA is available for the given drive
int ata_dma_available(uint8_t channel, uint8_t drive) {
    if (!dma_enabled) return 0;
    if (channel > 1 || drive > 1) return 0;

    int idx = channel * 2 + drive;
    if (!drives[idx].exists || drives[idx].type != ATA_TYPE_ATA) return 0;

    return drives[idx].dma_capable;
}

// Wait for DMA transfer to complete
static int ata_dma_wait(uint8_t channel) {
    uint16_t bmide = bmide_base + (channel * 8);
    int timeout = 1000000;  // ~1 second timeout

    while (timeout > 0) {
        uint8_t status = inb(bmide + BMIDE_STATUS);

        // Check for error
        if (status & BMIDE_STATUS_ERR) {
            kprintf("[ATA DMA] Error occurred (status=0x%02x)\n", status);
            return -1;
        }

        // Check if DMA is no longer active (transfer complete)
        if (!(status & BMIDE_STATUS_ACTIVE)) {
            // Check if IRQ was raised
            if (status & BMIDE_STATUS_IRQ) {
                // Clear IRQ status by writing 1 to it
                outb(bmide + BMIDE_STATUS, BMIDE_STATUS_IRQ);
                return 0;
            }
        }

        timeout--;
    }

    kprintf("[ATA DMA] Timeout waiting for transfer\n");
    return -1;
}

// Prepare PRD table for a single contiguous transfer
static void ata_dma_setup_prd(uint8_t channel, uint64_t phys_addr, uint32_t byte_count) {
    prd_entry_t *prd = prd_tables[channel];

    // For transfers <= 64KB, we can use a single PRD entry
    // For larger transfers, we'd need multiple entries (not implemented here)
    if (byte_count > 65536) {
        kprintf("[ATA DMA] Warning: transfer size %u > 64KB, limiting\n", byte_count);
        byte_count = 65536;
    }

    // Set up PRD entry
    prd[0].phys_addr = (uint32_t)phys_addr;  // 32-bit physical address
    prd[0].byte_count = (byte_count == 65536) ? 0 : (uint16_t)byte_count;  // 0 = 64KB
    prd[0].flags = PRD_EOT;  // End of table

    // Set PRD table address in BMIDE
    uint16_t bmide = bmide_base + (channel * 8);
    outl(bmide + BMIDE_PRDT, (uint32_t)prd_tables_phys[channel]);
}

// Read sectors using DMA (LBA28)
static int ata_read_sectors_dma_impl(uint8_t channel, uint8_t drive, uint32_t lba,
                         uint8_t count, void *buffer) {
    if (!dma_enabled || !ata_dma_available(channel, drive)) {
        // Fall back to PIO mode
        return ata_read_sectors_impl(channel, drive, lba, count, buffer);
    }

    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    uint16_t io = io_base[channel];
    uint16_t bmide = bmide_base + (channel * 8);
    uint32_t byte_count = count * ATA_SECTOR_SIZE;

    // Limit to DMA buffer size
    if (byte_count > DMA_BUFFER_SIZE) {
        kprintf("[ATA DMA] Transfer too large (%u bytes), using PIO\n", byte_count);
        return ata_read_sectors_impl(channel, drive, lba, count, buffer);
    }

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA DMA] BSY timeout before read\n");
        return -1;
    }

    // Stop any previous DMA transfer
    outb(bmide + BMIDE_CMD, 0);

    // Clear error and IRQ status
    outb(bmide + BMIDE_STATUS, BMIDE_STATUS_ERR | BMIDE_STATUS_IRQ);

    // Set up PRD table pointing to our DMA buffer
    ata_dma_setup_prd(channel, dma_buffer_phys, byte_count);

    // Select drive and set LBA mode
    outb(io + ATA_REG_DRIVE, (drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    io_wait();

    // Send parameters
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send DMA read command
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ_DMA);

    // Start DMA transfer (read direction)
    outb(bmide + BMIDE_CMD, BMIDE_CMD_START | BMIDE_CMD_READ);

    // Wait for DMA to complete
    if (ata_dma_wait(channel) != 0) {
        // Stop DMA
        outb(bmide + BMIDE_CMD, 0);

        // Check ATA error
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            uint8_t err = inb(io + ATA_REG_ERROR);
            kprintf("[ATA DMA] Read error (status=0x%02x, error=0x%02x)\n", status, err);
        }

        return -1;
    }

    // Stop DMA
    outb(bmide + BMIDE_CMD, 0);

    // #444: ata_dma_wait() already confirmed !ACTIVE && IRQ (real completion,
    // never an early/ambiguous return), but force a full memory fence here too
    // before trusting dma_buffer's contents. Belt-and-suspenders against any
    // store-ordering surprise between the bus-master's write of the sector
    // data and this CPU's read of it.
    memory_barrier();

    // Copy data from DMA buffer to user buffer
    memcpy(buffer, dma_buffer, byte_count);

    return count;
}

// Write sectors using DMA (LBA28)
static int ata_write_sectors_dma_impl(uint8_t channel, uint8_t drive, uint32_t lba,
                          uint8_t count, const void *buffer) {
    if (!dma_enabled || !ata_dma_available(channel, drive)) {
        // Fall back to PIO mode
        return ata_write_sectors_impl(channel, drive, lba, count, buffer);
    }

    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    uint16_t io = io_base[channel];
    uint16_t bmide = bmide_base + (channel * 8);
    uint32_t byte_count = count * ATA_SECTOR_SIZE;

    // Limit to DMA buffer size
    if (byte_count > DMA_BUFFER_SIZE) {
        kprintf("[ATA DMA] Transfer too large (%u bytes), using PIO\n", byte_count);
        return ata_write_sectors_impl(channel, drive, lba, count, buffer);
    }

    // Copy data to DMA buffer
    memcpy(dma_buffer, buffer, byte_count);
    write_barrier();  // #444: ensure the copy is visible before the bus-master reads it

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA DMA] BSY timeout before write\n");
        return -1;
    }

    // Stop any previous DMA transfer
    outb(bmide + BMIDE_CMD, 0);

    // Clear error and IRQ status
    outb(bmide + BMIDE_STATUS, BMIDE_STATUS_ERR | BMIDE_STATUS_IRQ);

    // Set up PRD table
    ata_dma_setup_prd(channel, dma_buffer_phys, byte_count);

    // Select drive and set LBA mode
    outb(io + ATA_REG_DRIVE, (drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    io_wait();

    // Send parameters
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send DMA write command
    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA);

    // Start DMA transfer (write direction - no BMIDE_CMD_READ flag)
    outb(bmide + BMIDE_CMD, BMIDE_CMD_START);

    // Wait for DMA to complete
    if (ata_dma_wait(channel) != 0) {
        // Stop DMA
        outb(bmide + BMIDE_CMD, 0);

        // Check ATA error
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            uint8_t err = inb(io + ATA_REG_ERROR);
            kprintf("[ATA DMA] Write error (status=0x%02x, error=0x%02x)\n", status, err);
        }

        return -1;
    }

    // Stop DMA
    outb(bmide + BMIDE_CMD, 0);

    // Flush cache
    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy(io);

    return count;
}

// Read sectors using DMA (LBA48)
static int ata_read_sectors_dma_ext_impl(uint8_t channel, uint8_t drive, uint64_t lba,
                             uint16_t count, void *buffer) {
    if (!dma_enabled || !ata_dma_available(channel, drive)) {
        // Fall back to PIO mode
        return ata_read_sectors_ext_impl(channel, drive, lba, count, buffer);
    }

    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!(drives[idx].command_sets & (1 << 26))) {
        // LBA48 not supported, try LBA28
        if (lba <= 0x0FFFFFFF && count <= 255) {
            return ata_read_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buffer);
        }
        return -1;
    }

    uint16_t io = io_base[channel];
    uint16_t bmide = bmide_base + (channel * 8);
    uint32_t byte_count = count * ATA_SECTOR_SIZE;

    // Limit to DMA buffer size
    if (byte_count > DMA_BUFFER_SIZE) {
        kprintf("[ATA DMA] Transfer too large (%u bytes), using PIO\n", byte_count);
        return ata_read_sectors_ext_impl(channel, drive, lba, count, buffer);
    }

    // Wait for drive to be ready
    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA DMA] BSY timeout before read\n");
        return -1;
    }

    // Stop any previous DMA transfer
    outb(bmide + BMIDE_CMD, 0);
    outb(bmide + BMIDE_STATUS, BMIDE_STATUS_ERR | BMIDE_STATUS_IRQ);

    // Set up PRD table
    ata_dma_setup_prd(channel, dma_buffer_phys, byte_count);

    // Select drive
    outb(io + ATA_REG_DRIVE, drive ? 0x50 : 0x40);
    io_wait();

    // Send high bytes first (LBA48)
    outb(io + ATA_REG_SECCOUNT, (count >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_LO, (lba >> 24) & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 32) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 40) & 0xFF);

    // Then low bytes
    outb(io + ATA_REG_SECCOUNT, count & 0xFF);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    // Send DMA read ext command
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ_DMA_EXT);

    // Start DMA transfer
    outb(bmide + BMIDE_CMD, BMIDE_CMD_START | BMIDE_CMD_READ);

    // Wait for completion
    if (ata_dma_wait(channel) != 0) {
        outb(bmide + BMIDE_CMD, 0);
        return -1;
    }

    outb(bmide + BMIDE_CMD, 0);
    memory_barrier();  // #444: same completion-coherence fence as the LBA28 path
    memcpy(buffer, dma_buffer, byte_count);

    return count;
}

// Write sectors using DMA (LBA48)
static int ata_write_sectors_dma_ext_impl(uint8_t channel, uint8_t drive, uint64_t lba,
                              uint16_t count, const void *buffer) {
    if (!dma_enabled || !ata_dma_available(channel, drive)) {
        // Fall back to PIO mode
        return ata_write_sectors_ext_impl(channel, drive, lba, count, buffer);
    }

    if (channel > 1 || drive > 1) return -1;
    if (count == 0) return 0;

    int idx = channel * 2 + drive;
    if (!(drives[idx].command_sets & (1 << 26))) {
        // LBA48 not supported
        if (lba <= 0x0FFFFFFF && count <= 255) {
            return ata_write_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buffer);
        }
        return -1;
    }

    uint16_t io = io_base[channel];
    uint16_t bmide = bmide_base + (channel * 8);
    uint32_t byte_count = count * ATA_SECTOR_SIZE;

    // Limit to DMA buffer size
    if (byte_count > DMA_BUFFER_SIZE) {
        kprintf("[ATA DMA] Transfer too large (%u bytes), using PIO\n", byte_count);
        return ata_write_sectors_ext_impl(channel, drive, lba, count, buffer);
    }

    memcpy(dma_buffer, buffer, byte_count);

    if (ata_wait_bsy(io) != 0) {
        kprintf("[ATA DMA] BSY timeout before write\n");
        return -1;
    }

    outb(bmide + BMIDE_CMD, 0);
    outb(bmide + BMIDE_STATUS, BMIDE_STATUS_ERR | BMIDE_STATUS_IRQ);

    ata_dma_setup_prd(channel, dma_buffer_phys, byte_count);

    outb(io + ATA_REG_DRIVE, drive ? 0x50 : 0x40);
    io_wait();

    outb(io + ATA_REG_SECCOUNT, (count >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_LO, (lba >> 24) & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 32) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 40) & 0xFF);

    outb(io + ATA_REG_SECCOUNT, count & 0xFF);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);

    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA_EXT);
    outb(bmide + BMIDE_CMD, BMIDE_CMD_START);

    if (ata_dma_wait(channel) != 0) {
        outb(bmide + BMIDE_CMD, 0);
        return -1;
    }

    outb(bmide + BMIDE_CMD, 0);

    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH_EXT);
    ata_wait_bsy(io);

    return count;
}

// ============================================================================
// DMA Performance Test
// ============================================================================

// Read TSC (Time Stamp Counter) for performance measurement
static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Test and compare PIO vs DMA performance
void ata_benchmark_dma(uint8_t channel, uint8_t drive) {
    static uint8_t bench_buf[32768];  // 32KB test buffer (64 sectors)
    uint64_t start, end;
    uint64_t pio_cycles, dma_cycles;
    int result;
    int iterations = 5;

    kprintf("\n[ATA] ====== DMA Benchmark ======\n");
    kprintf("[ATA] Testing channel %d, drive %d\n", channel, drive);
    kprintf("[ATA] Buffer size: 32KB (64 sectors), %d iterations\n", iterations);

    if (!ata_dma_available(channel, drive)) {
        kprintf("[ATA] DMA not available on this drive, skipping benchmark\n");
        return;
    }

    int idx = channel * 2 + drive;
    if (!drives[idx].exists || drives[idx].type != ATA_TYPE_ATA) {
        kprintf("[ATA] Drive not found or not ATA type\n");
        return;
    }

    // Warm-up read (to ensure drive is spinning and cache is primed)
    ata_read_sectors(channel, drive, 2048, 64, bench_buf);

    // Test PIO mode (using the direct function, not the wrapper)
    kprintf("[ATA] Testing PIO mode...\n");
    pio_cycles = 0;
    for (int i = 0; i < iterations; i++) {
        start = read_tsc();
        result = ata_read_sectors(channel, drive, 2048, 64, bench_buf);
        end = read_tsc();
        if (result <= 0) {
            kprintf("[ATA]   PIO read failed at iteration %d\n", i);
            return;
        }
        pio_cycles += (end - start);
    }
    pio_cycles /= iterations;
    kprintf("[ATA]   PIO average: %lu cycles\n", pio_cycles);

    // Test DMA mode directly (bypass fallback by checking dma_enabled manually)
    // We need to ensure we're actually using DMA
    kprintf("[ATA] Testing DMA mode...\n");
    dma_cycles = 0;
    for (int i = 0; i < iterations; i++) {
        // Call DMA function directly - it should use DMA since we already checked availability
        start = read_tsc();
        result = ata_read_sectors_dma(channel, drive, 2048, 64, bench_buf);
        end = read_tsc();
        if (result <= 0) {
            kprintf("[ATA]   DMA read failed at iteration %d\n", i);
            return;
        }
        dma_cycles += (end - start);
    }
    dma_cycles /= iterations;
    kprintf("[ATA]   DMA average: %lu cycles\n", dma_cycles);

    // Calculate speedup
    if (dma_cycles > 0) {
        uint64_t speedup_x10 = (pio_cycles * 10) / dma_cycles;
        kprintf("[ATA] DMA speedup: %lu.%lux faster than PIO\n",
                speedup_x10 / 10, speedup_x10 % 10);
    }

    // Verify data integrity
    kprintf("[ATA] Verifying data integrity...\n");
    uint8_t pio_buf[512], dma_buf[512];
    ata_read_sectors(channel, drive, 2048, 1, pio_buf);
    ata_read_sectors_dma(channel, drive, 2048, 1, dma_buf);

    int mismatch = 0;
    for (int i = 0; i < 512; i++) {
        if (pio_buf[i] != dma_buf[i]) {
            mismatch++;
        }
    }

    if (mismatch == 0) {
        kprintf("[ATA] Data integrity: OK (PIO and DMA match)\n");
    } else {
        kprintf("[ATA] Data integrity: FAIL (%d byte mismatches!)\n", mismatch);
    }

    kprintf("[ATA] ============================\n\n");
}

// ============================================================================
// Public DMA wrappers: serialize every transfer on the global I/O lock so the
// shared dma_buffer / PRD tables / Bus-Master registers cannot be corrupted by
// a concurrent transfer from another process. The bodies above are the static
// *_impl functions; these thin wrappers keep the public symbol names. The lock
// is taken only here (outermost) so the impl's DMA->PIO fallback never
// recurses into it.
// ============================================================================
// Public PIO wrappers: serialize every PIO transfer on the same global I/O lock
// the DMA path uses. Without this, two threads issuing PIO reads/writes to the
// same IDE channel interleave their port I/O (drive-select, LBA, command, the
// REP INSW data burst) and corrupt or hang each other. This mattered once ext2
// became the root FS: the compositor process and the kernel desktop-draw loop
// read the disk concurrently, where under FAT the recursive FAT lock had
// implicitly serialized all disk access. The lock is taken only in these
// outermost wrappers; the *_impl bodies (and the DMA->PIO fallback) stay
// unlocked so they never recurse into it.
int ata_read_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                     uint8_t count, void *buffer) {
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_read_sectors_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_write_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                      uint8_t count, const void *buffer) {
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_write_sectors_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_read_sectors_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                         uint16_t count, void *buffer) {
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_read_sectors_ext_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_write_sectors_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                          uint16_t count, const void *buffer) {
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_write_sectors_ext_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                         uint8_t count, void *buffer) {

    if (disk_slot_is_ahci(channel, drive)) {
        int __port = g_ahci_slot_port[channel * 2 + drive];
        uint64_t __fl = spinlock_acquire_irqsave(&g_ata_io_lock);
        int __r = ahci_read(__port, (uint64_t)lba, (uint32_t)count, buffer);
        spinlock_release_irqrestore(&g_ata_io_lock, __fl);
        return __r;
    }
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_read_sectors_dma_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                          uint8_t count, const void *buffer) {

    if (disk_slot_is_ahci(channel, drive)) {
        int __port = g_ahci_slot_port[channel * 2 + drive];
        uint64_t __fl = spinlock_acquire_irqsave(&g_ata_io_lock);
        int __r = ahci_write(__port, (uint64_t)lba, (uint32_t)count, buffer);
        spinlock_release_irqrestore(&g_ata_io_lock, __fl);
        return __r;
    }
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_write_sectors_dma_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_read_sectors_dma_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                             uint16_t count, void *buffer) {

    if (disk_slot_is_ahci(channel, drive)) {
        int __port = g_ahci_slot_port[channel * 2 + drive];
        uint64_t __fl = spinlock_acquire_irqsave(&g_ata_io_lock);
        int __r = ahci_read(__port, lba, (uint32_t)count, buffer);
        spinlock_release_irqrestore(&g_ata_io_lock, __fl);
        return __r;
    }
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_read_sectors_dma_ext_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}

int ata_write_sectors_dma_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                              uint16_t count, const void *buffer) {

    if (disk_slot_is_ahci(channel, drive)) {
        int __port = g_ahci_slot_port[channel * 2 + drive];
        uint64_t __fl = spinlock_acquire_irqsave(&g_ata_io_lock);
        int __r = ahci_write(__port, lba, (uint32_t)count, buffer);
        spinlock_release_irqrestore(&g_ata_io_lock, __fl);
        return __r;
    }
    uint64_t fl = spinlock_acquire_irqsave(&g_ata_io_lock);
    int r = ata_write_sectors_dma_ext_impl(channel, drive, lba, count, buffer);
    spinlock_release_irqrestore(&g_ata_io_lock, fl);
    return r;
}
