// ahci.c - AHCI (Advanced Host Controller Interface) driver implementation
// Part of MayteraOS - Task #48
//
// This driver provides modern SATA disk I/O support via the AHCI interface.
// Features:
// - PCI enumeration to find AHCI controllers
// - HBA and port initialization
// - Command list and FIS setup
// - DMA read/write operations
// - Native Command Queuing (NCQ) support

#include "ahci.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

// Global AHCI state
static ahci_hba_t ahci_hba = {0};

// Memory allocation helpers (using identity-mapped kernel memory)
static void *ahci_alloc_aligned(size_t size, size_t alignment __attribute__((unused))) {
    // Allocate pages and ensure alignment
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) return NULL;
    
    // Clear the memory
    memset((void *)phys, 0, pages * PAGE_SIZE);
    return (void *)phys;
}

// Get physical address (identity mapping)
static uint64_t virt_to_phys(void *virt) {
    return (uint64_t)virt;
}

// Memory barrier
static inline void memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

// Small delay
static void ahci_delay(uint32_t ms) {
    // Simple busy-wait delay (not accurate, but functional)
    for (uint32_t i = 0; i < ms * 20000; i++) {
        io_wait();
    }
}

// Classify the signature's low 16 bits into a device type. The high 16 bits
// are device-specific (e.g. ATAPI's 0xEB14, PM's 0x9669); QEMU sometimes
// leaves them as 0xFFFF, so we match on the low half which is identical for the
// common case (0x0101).
static ahci_device_type_t ahci_sig_to_type(uint32_t sig) {
    uint32_t hi = sig >> 16;
    if (hi == 0xEB14) return AHCI_DEV_SATAPI;
    if (hi == 0xC33C) return AHCI_DEV_SEMB;
    if (hi == 0x9669) return AHCI_DEV_PM;
    if ((sig & 0xFFFF) == 0x0101) return AHCI_DEV_SATA;
    return AHCI_DEV_NULL;
}

// Decide whether a port has an attached device worth probing. We accept it if
// the PHY link is up (PxSSTS.DET == 3) OR if the signature register holds a
// valid SATA signature and the task file is not stuck BSY - the QEMU
// ich9-ahci can report DET=0 even with a working drive, but it does populate
// PxSIG with the device's signature once the initial D2H FIS arrives.
static ahci_device_type_t ahci_check_type(volatile uint32_t *port_regs) {
    uint32_t ssts = port_regs[PORT_SSTS / 4];
    uint8_t det = ssts & PORT_SSTS_DET_MASK;
    uint32_t sig = port_regs[PORT_SIG / 4];
    uint32_t tfd = port_regs[PORT_TFD / 4];

    ahci_device_type_t by_sig = ahci_sig_to_type(sig);

    if (det == PORT_SSTS_DET_PHY) {
        return by_sig;
    }

    // DET not 3: trust the signature only if the device looks ready.
    if (by_sig != AHCI_DEV_NULL && !(tfd & PORT_TFD_STS_BSY)) {
        return by_sig;
    }
    return AHCI_DEV_NULL;
}

// Stop command engine
static void port_stop_cmd(volatile uint32_t *port_regs) {
    // Clear ST (Start)
    port_regs[PORT_CMD / 4] &= ~PORT_CMD_ST;
    
    // Clear FRE (FIS Receive Enable)
    port_regs[PORT_CMD / 4] &= ~PORT_CMD_FRE;
    
    // Wait until FR (FIS Receive Running) and CR (Command Running) are cleared
    int timeout = 500;
    while (timeout > 0) {
        uint32_t cmd = port_regs[PORT_CMD / 4];
        if (!(cmd & PORT_CMD_FR) && !(cmd & PORT_CMD_CR)) {
            return;
        }
        ahci_delay(1);
        timeout--;
    }
    
    kprintf("[AHCI] Warning: Port stop command timeout\n");
}

// Start command engine
static void port_start_cmd(volatile uint32_t *port_regs) {
    // Wait until CR is cleared
    int timeout = 500;
    while ((port_regs[PORT_CMD / 4] & PORT_CMD_CR) && timeout > 0) {
        ahci_delay(1);
        timeout--;
    }
    
    // Set FRE and ST
    port_regs[PORT_CMD / 4] |= PORT_CMD_FRE;
    port_regs[PORT_CMD / 4] |= PORT_CMD_ST;
}

// Initialize a single port
static int port_init(int port_num) {
    ahci_port_t *port = &ahci_hba.ports[port_num];
    volatile uint32_t *port_regs = port->port_regs;

    // Stop command engine before touching CLB/FB.
    port_stop_cmd(port_regs);

    // Allocate command list (1024 bytes, 1KB aligned)
    port->cmd_list = ahci_alloc_aligned(1024, 1024);
    if (!port->cmd_list) {
        kprintf("[AHCI] Failed to allocate command list for port %d\n", port_num);
        return -1;
    }
    port->cmd_list_phys = virt_to_phys(port->cmd_list);

    // Allocate FIS receive area (256 bytes, 256B aligned)
    port->fis_area = ahci_alloc_aligned(256, 256);
    if (!port->fis_area) {
        kprintf("[AHCI] Failed to allocate FIS area for port %d\n", port_num);
        return -1;
    }
    port->fis_area_phys = virt_to_phys(port->fis_area);

    // Allocate command tables (256 bytes + PRDTs, 128B aligned).
    // Allocate space for 8 PRD entries per command slot.
    size_t cmd_table_size = sizeof(hba_cmd_table_t) + 8 * sizeof(hba_prdt_entry_t);

    for (uint32_t i = 0; i < ahci_hba.num_cmd_slots; i++) {
        port->cmd_tables[i] = ahci_alloc_aligned(cmd_table_size, 128);
        if (!port->cmd_tables[i]) {
            kprintf("[AHCI] Failed to allocate command table %d for port %d\n", i, port_num);
            return -1;
        }
        port->cmd_tables_phys[i] = virt_to_phys(port->cmd_tables[i]);

        // Initialize command header
        hba_cmd_header_t *cmd_header = &port->cmd_list[i];
        cmd_header->ctba = port->cmd_tables_phys[i];
        cmd_header->prdtl = 8;  // Max 8 PRD entries
    }

    // Program command list and FIS base addresses.
    port_regs[PORT_CLB / 4]  = (uint32_t)(port->cmd_list_phys & 0xFFFFFFFF);
    port_regs[PORT_CLBU / 4] = (uint32_t)(port->cmd_list_phys >> 32);
    port_regs[PORT_FB / 4]   = (uint32_t)(port->fis_area_phys & 0xFFFFFFFF);
    port_regs[PORT_FBU / 4]  = (uint32_t)(port->fis_area_phys >> 32);

    // Enable FIS receive so the HBA can post received FISes.
    port_regs[PORT_CMD / 4] |= PORT_CMD_FRE;
    port_regs[PORT_SERR / 4] = 0xFFFFFFFF;
    port_regs[PORT_IS / 4]   = 0xFFFFFFFF;

    // Clear latched SATA errors and interrupt status.
    port_regs[PORT_SERR / 4] = 0xFFFFFFFF;
    port_regs[PORT_IS / 4]   = 0xFFFFFFFF;

    // Wait until the device is no longer BSY/DRQ before issuing commands.
    int tfd_timeout = 500;
    while (tfd_timeout > 0) {
        uint32_t tfd = port_regs[PORT_TFD / 4];
        if (!(tfd & (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ))) break;
        ahci_delay(1);
        tfd_timeout--;
    }

    // Clear interrupt status, enable the interrupt sources we care about.
    port_regs[PORT_IS / 4] = 0xFFFFFFFF;
    port_regs[PORT_IE / 4] = PORT_INT_DHR | PORT_INT_PS | PORT_INT_DS |
                            PORT_INT_SDB | PORT_INT_TFE | PORT_INT_HBF |
                            PORT_INT_HBD | PORT_INT_IF;

    // Start command engine (sets FRE+ST).
    port_start_cmd(port_regs);

    return 0;
}

// Find a free command slot
static int find_cmd_slot(ahci_port_t *port) {
    volatile uint32_t *port_regs = port->port_regs;
    uint32_t slots = port_regs[PORT_SACT / 4] | port_regs[PORT_CI / 4];

    for (uint32_t i = 0; i < ahci_hba.num_cmd_slots; i++) {
        if (!(slots & (1 << i))) {
            return i;
        }
    }

    return -1;
}

// Read the CPU timestamp counter. Unlike timer_ticks (incremented by the PIT
// IRQ handler), RDTSC advances every cycle regardless of interrupt state, so
// it works as a wall-clock source even during ahci_init() (called from
// ata_init() before sti() enables interrupts in main.c) when timer_ticks is
// still frozen. Same primitive drivers/ata.c's read_tsc() uses for timing.
static inline uint64_t ahci_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Command timeout for wait_cmd_complete() below, expressed as a TSC cycle
// budget rather than a raw loop-iteration count. AHCI_CMD_TIMEOUT_MS (5s)
// matches typical ATA/AHCI command deadlines and is long enough for a slow
// real disk. AHCI_TSC_CYCLES_PER_MS_MAX is a deliberately generous ceiling
// (8 GHz; real x86-64 CPUs are always slower) used only to convert that
// millisecond budget into a cycle count without needing a calibrated TSC
// frequency (none is available this early - see ahci_rdtsc() above). Because
// the ceiling over-estimates the CPU's real clock, the deadline always takes
// AT LEAST AHCI_CMD_TIMEOUT_MS of real time to expire (possibly more on a
// slower CPU, never less) - the property that actually matters here: never
// fail a slow-but-working disk too early. This intentionally avoids
// ahci_delay(): that busy-wait's cost comes from thousands of `outb`
// I/O-port round trips (each a VM exit under virtualization), and while that
// was fine for this file's other, rare, low-iteration-count timeouts
// (port_stop_cmd/port_start_cmd/spin-up), a version of this fix that called
// ahci_delay() on every completion poll was caught (by booting it on a real
// q35+AHCI VM instead of trusting the change on paper) inflating a single
// command wait from microseconds to multiple real seconds on a busy/
// virtualized host, which, multiplied across the thousands of sector reads
// in a normal boot, turned into a multi-minute stall. Polling RDTSC plus a
// couple of MMIO register reads is cheap and needs no artificial delay.
#define AHCI_CMD_TIMEOUT_MS 5000
#define AHCI_TSC_CYCLES_PER_MS_MAX 8000000ULL
#define AHCI_CMD_TIMEOUT_CYCLES \
    ((uint64_t)AHCI_CMD_TIMEOUT_MS * AHCI_TSC_CYCLES_PER_MS_MAX)

// Presence-probe timeout for the init-time IDENTIFY (see ahci_identify). Once a
// port's PHY link is up (we spin it up before probing), a *present* SATA drive
// answers IDENTIFY within microseconds, so the full 5s AHCI_CMD_TIMEOUT_MS is
// only ever needed for real bulk data transfers, never to decide whether a port
// has a disk at all. The init loop issues IDENTIFY to EVERY implemented port and
// treats a timeout as "no device present". On a controller with implemented-but-
// empty ports (e.g. a q35 VM whose only disk is on USB, so its ich9-ahci ports
// are all empty), a full 5s per empty port, serialized across every port,
// stalled boot for ~100s and looked like a hang at the splash. A short probe
// budget cuts that to a couple of seconds while still giving a working, link-up
// drive a ~300x margin to answer. Real bulk-I/O waits keep the full 5s budget.
#define AHCI_PROBE_TIMEOUT_MS 300
#define AHCI_PROBE_TIMEOUT_CYCLES \
    ((uint64_t)AHCI_PROBE_TIMEOUT_MS * AHCI_TSC_CYCLES_PER_MS_MAX)

// Recover a port after a command error or timeout so a single bad/slow
// command does not permanently wedge it (previously: wait_cmd_complete just
// returned -1 on PxIS.TFE with no cleanup, PxCMD.ST stayed cleared from the
// TFE auto-clear, and the port never answered another command until reboot).
// Follows the AHCI 1.3.1 SS10.7 non-queued error recovery outline: capture
// PxTFD for diagnostics, RW1C-clear the latched PxIS/PxSERR status, drop any
// stuck PxCI/PxSACT bits, kick BSY/DRQ off with PxCMD.CLO if the HBA supports
// it, then stop and restart the command engine (PxCMD.ST) so the port accepts
// new commands again.
static void ahci_port_recover(ahci_port_t *port, int slot) {
    volatile uint32_t *port_regs = port->port_regs;

    uint32_t tfd  = port_regs[PORT_TFD / 4];
    uint32_t is   = port_regs[PORT_IS / 4];
    uint32_t serr = port_regs[PORT_SERR / 4];
    kprintf("[AHCI] Port recover: slot=%d TFD=0x%08x IS=0x%08x SERR=0x%08x\n",
            slot, tfd, is, serr);

    // Stop the command engine (also clears FRE); waits for CR/FR to drop.
    port_stop_cmd(port_regs);

    // RW1C: writing 1s clears the latched interrupt/error status bits.
    port_regs[PORT_IS / 4] = 0xFFFFFFFF;
    port_regs[PORT_SERR / 4] = 0xFFFFFFFF;

    // Drop any stuck issue/active bits for this slot so a stale bit can't
    // confuse find_cmd_slot() or a future completion check.
    port_regs[PORT_CI / 4] &= ~(1u << slot);
    port_regs[PORT_SACT / 4] &= ~(1u << slot);

    // If the device is still BSY/DRQ, PxCMD.ST cannot be set again until
    // that clears. Use Command List Override (CLO) if the HBA supports it.
    if ((tfd & (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)) &&
        (ahci_hba.cap & HBA_CAP_SCLO)) {
        port_regs[PORT_CMD / 4] |= PORT_CMD_CLO;
        int clo_timeout = 500;
        while ((port_regs[PORT_CMD / 4] & PORT_CMD_CLO) && clo_timeout > 0) {
            ahci_delay(1);
            clo_timeout--;
        }
    }

    // Restart the command engine (sets FRE+ST) so the port is usable again.
    port_start_cmd(port_regs);
}

// Wait for command completion.
//
// is_ncq selects which register reflects real completion:
//  - Non-NCQ DMA (READ/WRITE DMA EXT, IDENTIFY, FLUSH): the HBA clears the
//    slot's bit in PxCI only once the command (including its data transfer)
//    is done, so polling PxCI is correct here - left unchanged.
//  - NCQ (READ/WRITE FPDMA QUEUED): the HBA clears PxCI as soon as the
//    command FIS is accepted by the device, LONG before the DMA data
//    transfer completes. Real completion is the device clearing the issued
//    tag's bit in PxSACT via a Set Device Bits (SDB) FIS. Polling PxCI for
//    NCQ commands returns "done" while the transfer is still in flight,
//    which is silent data corruption (reads return a stale/garbage buffer,
//    writes return before the data has landed). So for NCQ we poll PxSACT.
//
// The wait is bounded by real wall-clock time (an RDTSC cycle budget, see
// AHCI_CMD_TIMEOUT_CYCLES above), not by a raw CPU-speed-dependent iteration
// count. The poll itself is cheap (a couple of MMIO register reads plus an
// RDTSC), so the common case - a command that completes within microseconds,
// on real hardware or under QEMU's AHCI emulation alike - is caught almost
// immediately, exactly like this function did before this fix; only a
// genuinely slow or wedged command spins for anywhere near the full budget.
static int wait_cmd_complete(ahci_port_t *port, int slot, bool is_ncq,
                             uint64_t timeout_cycles) {
    volatile uint32_t *port_regs = port->port_regs;
    uint32_t slot_bit = (1u << slot);
    uint64_t start_tsc = ahci_rdtsc();

    for (;;) {
        bool done = is_ncq
            ? !(port_regs[PORT_SACT / 4] & slot_bit)   // NCQ: tag cleared via SDB FIS
            : !(port_regs[PORT_CI / 4] & slot_bit);    // Non-NCQ: PxCI cleared on completion

        if (done) {
            return 0;
        }

        // Check for errors (TFES/HBFS/HBDS/IFS all indicate the command
        // failed and the HBA has stopped the engine).
        uint32_t is = port_regs[PORT_IS / 4];
        if (is & (PORT_INT_TFE | PORT_INT_HBF | PORT_INT_HBD | PORT_INT_IF)) {
            kprintf("[AHCI] Command error: IS=0x%08x, TFD=0x%08x, slot=%d, ncq=%d\n",
                    is, port_regs[PORT_TFD / 4], slot, is_ncq ? 1 : 0);
            ahci_port_recover(port, slot);
            return -1;
        }

        if ((ahci_rdtsc() - start_tsc) >= timeout_cycles) {
            kprintf("[AHCI] Command timeout after ~%ums: slot=%d IS=0x%08x TFD=0x%08x ncq=%d\n",
                    (unsigned)(timeout_cycles / AHCI_TSC_CYCLES_PER_MS_MAX), slot,
                    port_regs[PORT_IS / 4], port_regs[PORT_TFD / 4], is_ncq ? 1 : 0);
            ahci_port_recover(port, slot);
            return -1;
        }
    }
}

// Issue IDENTIFY DEVICE command
static int ahci_identify(int port_num) {
    ahci_port_t *port = &ahci_hba.ports[port_num];
    volatile uint32_t *port_regs = port->port_regs;
    
    // Allocate buffer for identify data
    uint16_t *identify_data = ahci_alloc_aligned(512, 2);
    if (!identify_data) {
        return -1;
    }
    
    // Find free command slot
    int slot = find_cmd_slot(port);
    if (slot < 0) {
        kprintf("[AHCI] No free command slot\n");
        return -1;
    }
    
    // Set up command header
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;  // Command FIS length in DWORDs
    cmd_header->write = 0;  // Read
    cmd_header->prdtl = 1;  // One PRD entry
    cmd_header->prdbc = 0;
    
    // Set up command table
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t) + sizeof(hba_prdt_entry_t));
    
    // Set up PRD entry
    cmd_table->prdt_entry[0].dba = virt_to_phys(identify_data);
    cmd_table->prdt_entry[0].dbc = 511;  // 512 bytes - 1
    cmd_table->prdt_entry[0].i = 1;      // Interrupt on completion
    
    // Set up command FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  // Command
    fis->command = ATA_CMD_IDENTIFY;
    fis->device = 0;
    
    memory_barrier();
    
    // Issue command
    port_regs[PORT_CI / 4] = (1 << slot);

    // Wait for completion. This is a presence probe (IDENTIFY): a link-up drive
    // answers in microseconds, and the init loop treats a timeout as "no device",
    // so use the short probe budget, not the 5s bulk-I/O timeout, to avoid a
    // multi-second stall on every empty implemented port.
    if (wait_cmd_complete(port, slot, false, AHCI_PROBE_TIMEOUT_CYCLES) < 0) {
        return -1;
    }

    // Parse identify data
    // Model string (words 27-46)
    for (int i = 0; i < 20; i++) {
        port->model[i * 2] = identify_data[27 + i] >> 8;
        port->model[i * 2 + 1] = identify_data[27 + i] & 0xFF;
    }
    port->model[40] = '\0';
    // Trim trailing spaces
    for (int i = 39; i >= 0 && port->model[i] == ' '; i--) {
        port->model[i] = '\0';
    }
    
    // Serial number (words 10-19)
    for (int i = 0; i < 10; i++) {
        port->serial[i * 2] = identify_data[10 + i] >> 8;
        port->serial[i * 2 + 1] = identify_data[10 + i] & 0xFF;
    }
    port->serial[20] = '\0';
    // Trim trailing spaces
    for (int i = 19; i >= 0 && port->serial[i] == ' '; i--) {
        port->serial[i] = '\0';
    }
    
    // Check LBA48 support (word 83, bit 10)
    port->lba48_supported = (identify_data[83] & (1 << 10)) != 0;
    
    // Get sector count
    if (port->lba48_supported) {
        // LBA48 sector count (words 100-103)
        port->sector_count = ((uint64_t)identify_data[103] << 48) |
                            ((uint64_t)identify_data[102] << 32) |
                            ((uint64_t)identify_data[101] << 16) |
                            identify_data[100];
    } else {
        // LBA28 sector count (words 60-61)
        port->sector_count = ((uint32_t)identify_data[61] << 16) | identify_data[60];
    }
    
    // Check NCQ support (word 76, bit 8)
    port->ncq_supported = (identify_data[76] & (1 << 8)) != 0;
    if (port->ncq_supported) {
        // NCQ queue depth (word 75, bits 0-4, 0-based)
        port->ncq_depth = (identify_data[75] & 0x1F) + 1;
    }
    
    // Sector size (word 106)
    if (identify_data[106] & (1 << 12)) {
        // Logical sector size is specified in words 117-118 (a 32-bit dword
        // giving the size in words); words 118-119 is off by one and reads
        // the wrong half of the value (and one word past word 118's actual
        // partner), which corrupts the computed byte size on 4Kn drives.
        port->sector_size = (identify_data[117] | (identify_data[118] << 16)) * 2;
    } else {
        port->sector_size = 512;
    }
    
    return 0;
}

// Initialize AHCI driver
// Initialize a single AHCI controller (one PCI device). Returns the number of
// SATA disks found on it, or -1 on a fatal error. Called for every AHCI PCI
// device by ahci_init() so we pick the controller that actually has a disk
// (q35 has an empty built-in ich9-ahci at 1f.2 plus the populated one Proxmox
// adds for the sata0 disk).
static int ahci_init_one(pci_device_t *ahci_dev) {
    // Reset the global HBA state for this controller.
    memset(&ahci_hba, 0, sizeof(ahci_hba));
    ahci_hba.pci_device = ahci_dev;

    kprintf("[AHCI] Found AHCI controller: %04x:%04x at %02x:%02x.%x\n",
            ahci_dev->vendor_id, ahci_dev->device_id,
            ahci_dev->bus, ahci_dev->slot, ahci_dev->func);
    
    // Get ABAR (AHCI Base Memory Register) from BAR5
    uint64_t abar_phys = pci_get_bar_address(ahci_dev, 5);
    if (abar_phys == 0) {
        kprintf("[AHCI] Invalid ABAR\n");
        return -1;
    }
    
    kprintf("[AHCI] ABAR at 0x%016lx\n", abar_phys);
    
    // Map ABAR as UNCACHED MMIO. This is CRITICAL: the HBA registers (CI,
    // SSTS, IS, SCTL, ...) are memory-mapped device registers. If the page is
    // left cached (the default identity mapping), CI/SCTL writes can sit in the
    // CPU write buffer and SSTS reads return stale cache lines, so commands are
    // never seen by the controller and PxSSTS.DET appears stuck at 0. We remap
    // the ABAR region (HBA generic regs + 32 port regs = up to 0x1100 bytes)
    // with PCD set, exactly as the e1000 driver does for its BAR.
    {
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_PCD;
        uint64_t base = abar_phys & ~(VMM_PAGE_SIZE_4K - 1);
        // 0x2000 bytes covers all 32 ports (0x100 + 32*0x80 = 0x1100); round to 3 pages.
        for (uint64_t off = 0; off < 0x3000; off += VMM_PAGE_SIZE_4K) {
            uint64_t pg = base + off;
            // Remap even if already mapped, to force the uncached attributes.
            vmm_map_page(pg, pg, flags);
        }
    }
    ahci_hba.abar = (volatile uint32_t *)abar_phys;
    
    // Read HBA capabilities
    ahci_hba.cap = ahci_hba.abar[HBA_CAP / 4];
    ahci_hba.cap2 = ahci_hba.abar[HBA_CAP2 / 4];
    ahci_hba.version = ahci_hba.abar[HBA_VS / 4];
    ahci_hba.ports_implemented = ahci_hba.abar[HBA_PI / 4];
    
    // Parse capabilities
    ahci_hba.num_ports = (ahci_hba.cap & HBA_CAP_NP_MASK) + 1;
    ahci_hba.num_cmd_slots = ((ahci_hba.cap >> 8) & 0x1F) + 1;
    ahci_hba.supports_64bit = (ahci_hba.cap & HBA_CAP_S64A) != 0;
    ahci_hba.supports_ncq = (ahci_hba.cap & HBA_CAP_SNCQ) != 0;
    
    kprintf("[AHCI] Version: %x.%x%x\n",
            (ahci_hba.version >> 16) & 0xFFFF,
            (ahci_hba.version >> 8) & 0xFF,
            ahci_hba.version & 0xFF);
    kprintf("[AHCI] Ports implemented: 0x%08x\n", ahci_hba.ports_implemented);
    kprintf("[AHCI] Max ports: %u, Command slots: %u\n",
            ahci_hba.num_ports, ahci_hba.num_cmd_slots);
    kprintf("[AHCI] CAP=0x%08x (SSS=%d, CLO=%d)\n", ahci_hba.cap,
            (ahci_hba.cap & HBA_CAP_SSS) ? 1 : 0,
            (ahci_hba.cap & HBA_CAP_SCLO) ? 1 : 0);
    kprintf("[AHCI] 64-bit: %s, NCQ: %s\n",
            ahci_hba.supports_64bit ? "yes" : "no",
            ahci_hba.supports_ncq ? "yes" : "no");
    
    // Perform BIOS/OS handoff if needed (BOHC)
    if (ahci_hba.cap2 & (1 << 0)) {  // BOH capability
        kprintf("[AHCI] Performing BIOS/OS handoff...\n");
        ahci_hba.abar[HBA_BOHC / 4] |= (1 << 1);  // Set OS Owned Semaphore
        
        int timeout = 25;  // 25ms timeout
        while ((ahci_hba.abar[HBA_BOHC / 4] & (1 << 0)) && timeout > 0) {
            ahci_delay(1);
            timeout--;
        }
        
        if (timeout == 0) {
            kprintf("[AHCI] Warning: BIOS handoff timeout\n");
        }
    }
    
    // Enable AHCI mode. We deliberately do NOT issue an HBA reset (GHC.HR):
    // UEFI already enabled AHCI and established the SATA PHY links, and a full
    // HBA reset drops those links (QEMU then leaves PxSSTS.DET=0 until a
    // per-port COMRESET). Re-asserting AE and clearing interrupt status is
    // sufficient and far more robust on both QEMU q35 and real hardware.
    ahci_hba.abar[HBA_GHC / 4] |= HBA_GHC_AE;

    // Clear global interrupt status.
    ahci_hba.abar[HBA_IS / 4] = 0xFFFFFFFF;

    // Enable global interrupts (we still poll for completion).
    ahci_hba.abar[HBA_GHC / 4] |= HBA_GHC_IE;
    
    // Initialize implemented ports
    int ports_found = 0;
    for (int i = 0; i < 32; i++) {
        if (!(ahci_hba.ports_implemented & (1 << i))) {
            continue;
        }
        
        // Calculate port register base
        volatile uint32_t *port_regs = (volatile uint32_t *)
            ((uint8_t *)ahci_hba.abar + 0x100 + i * 0x80);
        
        ahci_hba.ports[i].port_regs = port_regs;

        // Spin the device up so its PHY comes online (PxSSTS.DET -> 3). On the
        // QEMU ich9-ahci (and real controllers with staggered spin-up) the
        // port reports DET=0 until software sets PxCMD.SUD. We set SUD + power
        // on + FRE so the link establishes, clear SERR, then poll DET. We do
        // NOT touch PxSCTL (a COMRESET there wedges the QEMU port) and we do
        // NOT reset the HBA.
        // Spin the device up (SUD/POD) and enable FIS receive so the HBA
        // populates PxSIG/PxTFD and answers commands. (QEMU only fills these in
        // after SUD; PxSSTS.DET may still read 0 on the QEMU ich9-ahci even
        // with a working drive, so we do not gate on it - see ahci_check_type.)
        port_regs[PORT_CMD / 4] |= (PORT_CMD_SUD | PORT_CMD_POD | PORT_CMD_FRE);
        port_regs[PORT_SERR / 4] = 0xFFFFFFFF;
        {
            int det_to = 30;
            while (det_to > 0) {
                uint32_t ssts = port_regs[PORT_SSTS / 4];
                uint32_t sig  = port_regs[PORT_SIG / 4];
                if ((ssts & PORT_SSTS_DET_MASK) == PORT_SSTS_DET_PHY) break;
                if ((sig & 0xFFFF) == 0x0101) break;
                ahci_delay(1);
                det_to--;
            }
        }
        port_regs[PORT_SERR / 4] = 0xFFFFFFFF;

        // Check device type
        ahci_device_type_t type = ahci_check_type(port_regs);
        ahci_hba.ports[i].device_type = type;
        
        if (type == AHCI_DEV_NULL) {
            continue;
        }

        const char *type_str;
        switch (type) {
            case AHCI_DEV_SATA:   type_str = "SATA"; break;
            case AHCI_DEV_SATAPI: type_str = "SATAPI"; break;
            case AHCI_DEV_SEMB:   type_str = "SEMB"; break;
            case AHCI_DEV_PM:     type_str = "PM"; break;
            default:             type_str = "Unknown"; break;
        }
        kprintf("[AHCI] Port %d: %s candidate\n", i, type_str);

        // Initialize the port (allocate command list/FIS/tables, start engine).
        if (port_init(i) < 0) {
            kprintf("[AHCI] Failed to initialize port %d\n", i);
            continue;
        }

        // For SATA devices, IDENTIFY is the authoritative presence test: on a
        // multi-port HBA the QEMU ich9-ahci mirrors the 0x0101 signature onto
        // every implemented port, but only the port with a real drive answers
        // IDENTIFY. We therefore mark the port present ONLY if IDENTIFY works.
        if (type == AHCI_DEV_SATA) {
            if (ahci_identify(i) == 0 && ahci_hba.ports[i].sector_count > 0) {
                ahci_hba.ports[i].present = true;
                ports_found++;
                uint64_t size_mb = (ahci_hba.ports[i].sector_count *
                                   ahci_hba.ports[i].sector_size) / (1024 * 1024);
                kprintf("[AHCI] port%d SATA disk, %lu sectors\n",
                        i, (unsigned long)ahci_hba.ports[i].sector_count);
                kprintf("[AHCI]   Model: %s\n", ahci_hba.ports[i].model);
                kprintf("[AHCI]   Serial: %s\n", ahci_hba.ports[i].serial);
                kprintf("[AHCI]   Size: %lu MB\n", size_mb);
                kprintf("[AHCI]   LBA48: %s, NCQ: %s (depth %u)\n",
                        ahci_hba.ports[i].lba48_supported ? "yes" : "no",
                        ahci_hba.ports[i].ncq_supported ? "yes" : "no",
                        ahci_hba.ports[i].ncq_depth);
            } else {
                // No real device on this port: stop its engine and skip it.
                kprintf("[AHCI] Port %d: IDENTIFY failed (no drive), skipping\n", i);
                port_stop_cmd(port_regs);
                ahci_hba.ports[i].present = false;
                ahci_hba.ports[i].device_type = AHCI_DEV_NULL;
            }
        } else {
            // Non-SATA (ATAPI/PM/SEMB): record as present but we do not drive it.
            ahci_hba.ports[i].present = true;
            ports_found++;
        }
    }
    
    if (ports_found > 0) {
        ahci_hba.initialized = true;
    }
    kprintf("[AHCI] Controller init complete: %d SATA disks active\n", ports_found);
    return ports_found;
}

// Public entry point: probe EVERY AHCI controller on the PCI bus and keep the
// first one that has at least one attached SATA disk. This is required on q35,
// which exposes an empty built-in ich9-ahci (at 00:1f.2) in addition to the
// populated controller that backs the sata0 disk.
int ahci_init(void) {
    kprintf("[AHCI] Initializing AHCI driver...\n");
    int dev_count = pci_get_device_count();
    int any = 0;
    for (int i = 0; i < dev_count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->class_code != AHCI_CLASS_MASS_STORAGE ||
            dev->subclass != AHCI_SUBCLASS_SATA ||
            dev->prog_if != AHCI_PROG_IF_AHCI) {
            continue;
        }
        any = 1;
        pci_enable_bus_master(dev);
        int found = ahci_init_one(dev);
        if (found > 0) {
            return 0;
        }
        memset(&ahci_hba, 0, sizeof(ahci_hba));
    }
    if (!any) kprintf("[AHCI] No AHCI controller found\n");
    else      kprintf("[AHCI] No AHCI controller had an attached SATA disk\n");
    return -1;
}

// Read sectors
int ahci_read(int port_num, uint64_t lba, uint32_t count, void *buffer) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return -1;
    }
    
    ahci_port_t *port = &ahci_hba.ports[port_num];
    if (!port->present || port->device_type != AHCI_DEV_SATA) {
        return -1;
    }
    
    volatile uint32_t *port_regs = port->port_regs;
    
    // Find free command slot
    int slot = find_cmd_slot(port);
    if (slot < 0) {
        return -1;
    }
    
    // Set up command header
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->write = 0;  // Read
    cmd_header->prdbc = 0;
    
    // Calculate number of PRD entries needed
    uint32_t byte_count = count * port->sector_size;
    uint32_t prd_count = (byte_count + AHCI_PRD_MAX_SIZE - 1) / AHCI_PRD_MAX_SIZE;
    if (prd_count > 8) prd_count = 8;  // Limit to our allocated PRD count
    cmd_header->prdtl = prd_count;
    
    // Set up command table
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t) + prd_count * sizeof(hba_prdt_entry_t));
    
    // Set up PRD entries
    uint8_t *buf_ptr = (uint8_t *)buffer;
    uint32_t remaining = byte_count;
    for (uint32_t i = 0; i < prd_count; i++) {
        uint32_t this_size = (remaining > AHCI_PRD_MAX_SIZE) ? AHCI_PRD_MAX_SIZE : remaining;
        cmd_table->prdt_entry[i].dba = virt_to_phys(buf_ptr);
        cmd_table->prdt_entry[i].dbc = this_size - 1;
        cmd_table->prdt_entry[i].i = (i == prd_count - 1) ? 1 : 0;
        buf_ptr += this_size;
        remaining -= this_size;
    }
    
    // Set up command FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  // Command
    fis->command = ATA_CMD_READ_DMA_EX;
    
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    fis->device = 0x40;  // LBA mode
    
    fis->count_low = (uint8_t)(count);
    fis->count_high = (uint8_t)(count >> 8);
    
    memory_barrier();
    
    // Issue command
    port_regs[PORT_CI / 4] = (1 << slot);

    // Wait for completion (non-NCQ DMA: PxCI is the correct completion signal)
    if (wait_cmd_complete(port, slot, false, AHCI_CMD_TIMEOUT_CYCLES) < 0) {
        return -1;
    }

    return count;
}

// Write sectors
int ahci_write(int port_num, uint64_t lba, uint32_t count, const void *buffer) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return -1;
    }
    
    ahci_port_t *port = &ahci_hba.ports[port_num];
    if (!port->present || port->device_type != AHCI_DEV_SATA) {
        return -1;
    }
    
    volatile uint32_t *port_regs = port->port_regs;
    
    // Find free command slot
    int slot = find_cmd_slot(port);
    if (slot < 0) {
        return -1;
    }
    
    // Set up command header
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->write = 1;  // Write
    cmd_header->prdbc = 0;
    
    // Calculate number of PRD entries needed
    uint32_t byte_count = count * port->sector_size;
    uint32_t prd_count = (byte_count + AHCI_PRD_MAX_SIZE - 1) / AHCI_PRD_MAX_SIZE;
    if (prd_count > 8) prd_count = 8;
    cmd_header->prdtl = prd_count;
    
    // Set up command table
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t) + prd_count * sizeof(hba_prdt_entry_t));
    
    // Set up PRD entries
    const uint8_t *buf_ptr = (const uint8_t *)buffer;
    uint32_t remaining = byte_count;
    for (uint32_t i = 0; i < prd_count; i++) {
        uint32_t this_size = (remaining > AHCI_PRD_MAX_SIZE) ? AHCI_PRD_MAX_SIZE : remaining;
        cmd_table->prdt_entry[i].dba = virt_to_phys((void *)buf_ptr);
        cmd_table->prdt_entry[i].dbc = this_size - 1;
        cmd_table->prdt_entry[i].i = (i == prd_count - 1) ? 1 : 0;
        buf_ptr += this_size;
        remaining -= this_size;
    }
    
    // Set up command FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  // Command
    fis->command = ATA_CMD_WRITE_DMA_EX;
    
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    fis->device = 0x40;  // LBA mode
    
    fis->count_low = (uint8_t)(count);
    fis->count_high = (uint8_t)(count >> 8);
    
    memory_barrier();
    
    // Issue command
    port_regs[PORT_CI / 4] = (1 << slot);

    // Wait for completion (non-NCQ DMA: PxCI is the correct completion signal)
    if (wait_cmd_complete(port, slot, false, AHCI_CMD_TIMEOUT_CYCLES) < 0) {
        return -1;
    }

    return count;
}

// Flush cache
int ahci_flush(int port_num) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return -1;
    }
    
    ahci_port_t *port = &ahci_hba.ports[port_num];
    if (!port->present || port->device_type != AHCI_DEV_SATA) {
        return -1;
    }
    
    volatile uint32_t *port_regs = port->port_regs;
    
    int slot = find_cmd_slot(port);
    if (slot < 0) return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->write = 0;
    cmd_header->prdtl = 0;
    cmd_header->prdbc = 0;
    
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command = ATA_CMD_FLUSH_CACHE_EX;
    
    memory_barrier();
    port_regs[PORT_CI / 4] = (1 << slot);

    return wait_cmd_complete(port, slot, false, AHCI_CMD_TIMEOUT_CYCLES);
}

// NCQ Read (if supported)
int ahci_read_ncq(int port_num, uint64_t lba, uint32_t count, void *buffer) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return -1;
    }
    
    ahci_port_t *port = &ahci_hba.ports[port_num];
    if (!port->present || port->device_type != AHCI_DEV_SATA) {
        return -1;
    }
    
    // Fall back to regular DMA if NCQ not supported
    if (!port->ncq_supported) {
        return ahci_read(port_num, lba, count, buffer);
    }
    
    volatile uint32_t *port_regs = port->port_regs;
    
    int slot = find_cmd_slot(port);
    if (slot < 0) return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->write = 0;
    cmd_header->prdbc = 0;
    
    uint32_t byte_count = count * port->sector_size;
    uint32_t prd_count = (byte_count + AHCI_PRD_MAX_SIZE - 1) / AHCI_PRD_MAX_SIZE;
    if (prd_count > 8) prd_count = 8;
    cmd_header->prdtl = prd_count;
    
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t) + prd_count * sizeof(hba_prdt_entry_t));
    
    uint8_t *buf_ptr = (uint8_t *)buffer;
    uint32_t remaining = byte_count;
    for (uint32_t i = 0; i < prd_count; i++) {
        uint32_t this_size = (remaining > AHCI_PRD_MAX_SIZE) ? AHCI_PRD_MAX_SIZE : remaining;
        cmd_table->prdt_entry[i].dba = virt_to_phys(buf_ptr);
        cmd_table->prdt_entry[i].dbc = this_size - 1;
        cmd_table->prdt_entry[i].i = (i == prd_count - 1) ? 1 : 0;
        buf_ptr += this_size;
        remaining -= this_size;
    }
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command = ATA_CMD_READ_FPDMA_Q;
    
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    // NCQ uses feature register for count
    fis->feature_low = (uint8_t)(count);
    fis->feature_high = (uint8_t)(count >> 8);
    
    fis->device = 0x40;  // LBA mode
    fis->count_low = slot << 3;  // Tag in sector count
    
    memory_barrier();
    
    // Set SACT first for NCQ
    port_regs[PORT_SACT / 4] = (1 << slot);
    port_regs[PORT_CI / 4] = (1 << slot);

    // NCQ completion is signaled by the device clearing this tag's bit in
    // PxSACT (via a Set Device Bits FIS), not by PxCI clearing - the HBA
    // clears PxCI as soon as it accepts the command FIS, well before the DMA
    // transfer into `buffer` is complete. Polling PxCI here would return
    // "done" while the buffer is still being filled (stale/garbage data).
    return wait_cmd_complete(port, slot, true, AHCI_CMD_TIMEOUT_CYCLES) == 0 ? (int)count : -1;
}

// NCQ Write (if supported)
int ahci_write_ncq(int port_num, uint64_t lba, uint32_t count, const void *buffer) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return -1;
    }
    
    ahci_port_t *port = &ahci_hba.ports[port_num];
    if (!port->present || port->device_type != AHCI_DEV_SATA) {
        return -1;
    }
    
    if (!port->ncq_supported) {
        return ahci_write(port_num, lba, count, buffer);
    }
    
    volatile uint32_t *port_regs = port->port_regs;
    
    int slot = find_cmd_slot(port);
    if (slot < 0) return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->write = 1;
    cmd_header->prdbc = 0;
    
    uint32_t byte_count = count * port->sector_size;
    uint32_t prd_count = (byte_count + AHCI_PRD_MAX_SIZE - 1) / AHCI_PRD_MAX_SIZE;
    if (prd_count > 8) prd_count = 8;
    cmd_header->prdtl = prd_count;
    
    hba_cmd_table_t *cmd_table = port->cmd_tables[slot];
    memset(cmd_table, 0, sizeof(hba_cmd_table_t) + prd_count * sizeof(hba_prdt_entry_t));
    
    const uint8_t *buf_ptr = (const uint8_t *)buffer;
    uint32_t remaining = byte_count;
    for (uint32_t i = 0; i < prd_count; i++) {
        uint32_t this_size = (remaining > AHCI_PRD_MAX_SIZE) ? AHCI_PRD_MAX_SIZE : remaining;
        cmd_table->prdt_entry[i].dba = virt_to_phys((void *)buf_ptr);
        cmd_table->prdt_entry[i].dbc = this_size - 1;
        cmd_table->prdt_entry[i].i = (i == prd_count - 1) ? 1 : 0;
        buf_ptr += this_size;
        remaining -= this_size;
    }
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command = ATA_CMD_WRITE_FPDMA_Q;
    
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    fis->feature_low = (uint8_t)(count);
    fis->feature_high = (uint8_t)(count >> 8);
    
    fis->device = 0x40;
    fis->count_low = slot << 3;
    
    memory_barrier();
    
    port_regs[PORT_SACT / 4] = (1 << slot);
    port_regs[PORT_CI / 4] = (1 << slot);

    // As with ahci_read_ncq: real completion (data actually landed on the
    // media/write cache) is the device clearing PxSACT's tag bit via an SDB
    // FIS, not PxCI clearing early.
    return wait_cmd_complete(port, slot, true, AHCI_CMD_TIMEOUT_CYCLES) == 0 ? (int)count : -1;
}

// Return the AHCI port number of the Nth attached SATA disk (0-based),
// or -1 if there is no such disk. Used by the block dispatch layer to map a
// logical disk index to a physical AHCI port.
int ahci_get_nth_sata_port(int n) {
    if (!ahci_hba.initialized || n < 0) return -1;
    int seen = 0;
    for (int i = 0; i < 32; i++) {
        if (ahci_hba.ports[i].present &&
            ahci_hba.ports[i].device_type == AHCI_DEV_SATA) {
            if (seen == n) return i;
            seen++;
        }
    }
    return -1;
}

// Get port count
int ahci_get_port_count(void) {
    if (!ahci_hba.initialized) return 0;
    
    int count = 0;
    for (int i = 0; i < 32; i++) {
        if (ahci_hba.ports[i].present) {
            count++;
        }
    }
    return count;
}

// Get port information
ahci_port_t *ahci_get_port(int port_num) {
    if (!ahci_hba.initialized || port_num >= 32) {
        return NULL;
    }
    if (!ahci_hba.ports[port_num].present) {
        return NULL;
    }
    return &ahci_hba.ports[port_num];
}

// Get model string
const char *ahci_get_model(int port_num) {
    ahci_port_t *port = ahci_get_port(port_num);
    return port ? port->model : NULL;
}

// Get serial number
const char *ahci_get_serial(int port_num) {
    ahci_port_t *port = ahci_get_port(port_num);
    return port ? port->serial : NULL;
}

// Get sector count
uint64_t ahci_get_sector_count(int port_num) {
    ahci_port_t *port = ahci_get_port(port_num);
    return port ? port->sector_count : 0;
}

// Check if initialized
bool ahci_is_initialized(void) {
    return ahci_hba.initialized;
}

// Print AHCI information
void ahci_print_info(void) {
    kprintf("\n[AHCI] ====== AHCI Controller Information ======\n");
    
    if (!ahci_hba.initialized) {
        kprintf("[AHCI] Not initialized\n");
        return;
    }
    
    kprintf("[AHCI] Version: %x.%x%x\n",
            (ahci_hba.version >> 16) & 0xFFFF,
            (ahci_hba.version >> 8) & 0xFF,
            ahci_hba.version & 0xFF);
    kprintf("[AHCI] Capabilities: 0x%08x\n", ahci_hba.cap);
    kprintf("[AHCI] Command Slots: %u, Ports: %u\n",
            ahci_hba.num_cmd_slots, ahci_hba.num_ports);
    kprintf("[AHCI] 64-bit: %s, NCQ: %s\n",
            ahci_hba.supports_64bit ? "yes" : "no",
            ahci_hba.supports_ncq ? "yes" : "no");
    
    kprintf("\n[AHCI] Connected Devices:\n");
    
    for (int i = 0; i < 32; i++) {
        if (!ahci_hba.ports[i].present) continue;
        
        ahci_port_t *port = &ahci_hba.ports[i];
        
        const char *type_str;
        switch (port->device_type) {
            case AHCI_DEV_SATA:   type_str = "SATA"; break;
            case AHCI_DEV_SATAPI: type_str = "SATAPI"; break;
            case AHCI_DEV_SEMB:   type_str = "SEMB"; break;
            case AHCI_DEV_PM:     type_str = "PM"; break;
            default:             type_str = "Unknown"; break;
        }
        
        kprintf("[AHCI]   Port %d: %s\n", i, type_str);
        
        if (port->device_type == AHCI_DEV_SATA) {
            uint64_t size_mb = (port->sector_count * port->sector_size) / (1024 * 1024);
            kprintf("[AHCI]     Model:  %s\n", port->model);
            kprintf("[AHCI]     Serial: %s\n", port->serial);
            kprintf("[AHCI]     Size:   %lu MB\n", size_mb);
            kprintf("[AHCI]     LBA48:  %s\n", port->lba48_supported ? "yes" : "no");
            kprintf("[AHCI]     NCQ:    %s (depth %u)\n",
                    port->ncq_supported ? "yes" : "no",
                    port->ncq_depth);
        }
    }
    
    kprintf("[AHCI] =============================================\n\n");
}

// #307 AHCI write+readback self-test. Writes a known pattern to a scratch LBA
// near the end of the disk (well past any filesystem data), reads it back, and
// reports PASS/FAIL. Proves WRITE DMA EXT + READ DMA EXT round-trip through the
// real controller. Restores the original sector contents afterwards.
void ahci_selftest(void) {
    if (!ahci_hba.initialized) {
        kprintf("[AHCI] selftest: AHCI not initialized, skipping\n");
        return;
    }
    int port = ahci_get_nth_sata_port(0);
    if (port < 0) { kprintf("[AHCI] selftest: no SATA disk\n"); return; }

    uint64_t total = ahci_get_sector_count(port);
    if (total < 16) { kprintf("[AHCI] selftest: disk too small\n"); return; }
    uint64_t lba = total - 8;  // scratch sector near the end

    static uint8_t save_buf[512];
    static uint8_t test_buf[512];
    static uint8_t read_buf[512];

    // Save the original sector so we do not corrupt the disk.
    if (ahci_read(port, lba, 1, save_buf) != 1) {
        kprintf("[AHCI] selftest: initial read FAILED\n");
        return;
    }
    // Build a recognizable pattern.
    for (int i = 0; i < 512; i++) test_buf[i] = (uint8_t)(i ^ 0xA5);
    test_buf[0]='A'; test_buf[1]='H'; test_buf[2]='C'; test_buf[3]='I';

    if (ahci_write(port, lba, 1, test_buf) != 1) {
        kprintf("[AHCI] selftest: WRITE FAILED\n");
        return;
    }
    memset(read_buf, 0, 512);
    if (ahci_read(port, lba, 1, read_buf) != 1) {
        kprintf("[AHCI] selftest: read-back FAILED\n");
        return;
    }
    int ok = (memcmp(read_buf, test_buf, 512) == 0);
    kprintf("[AHCI] selftest: write+readback of LBA %lu -> %s\n",
            (unsigned long)lba, ok ? "PASS" : "FAIL");

    // Restore the original contents.
    ahci_write(port, lba, 1, save_buf);
}

// NCQ write+readback self-test. Same shape as ahci_selftest() but drives the
// FPDMA QUEUED (NCQ) entry points directly, at a different scratch LBA so the
// two tests never touch the same sector. This is the load-bearing proof for
// the PxSACT-vs-PxCI completion fix: before the fix, wait_cmd_complete polled
// PxCI, which the HBA clears as soon as it accepts the command FIS - long
// before the DMA transfer finishes - so a fast readback right after "success"
// could race the write (or return a stale/garbage buffer on read) and this
// test would intermittently or reliably FAIL/mismatch. After the fix,
// completion is gated on PxSACT's tag bit clearing (set by the device's SDB
// FIS once the transfer really is done), so the round-trip is deterministic.
// No-ops (with a log line) if the drive/controller does not report NCQ
// support, since ahci_read_ncq/ahci_write_ncq transparently fall back to the
// plain DMA path in that case and would not be exercising PxSACT at all.
void ahci_selftest_ncq(void) {
    if (!ahci_hba.initialized) {
        kprintf("[AHCI] selftest_ncq: AHCI not initialized, skipping\n");
        return;
    }
    int port_num = ahci_get_nth_sata_port(0);
    if (port_num < 0) { kprintf("[AHCI] selftest_ncq: no SATA disk\n"); return; }

    ahci_port_t *port = ahci_get_port(port_num);
    if (!port || !port->ncq_supported) {
        kprintf("[AHCI] selftest_ncq: drive does not report NCQ, skipping\n");
        return;
    }

    uint64_t total = ahci_get_sector_count(port_num);
    if (total < 24) { kprintf("[AHCI] selftest_ncq: disk too small\n"); return; }
    uint64_t lba = total - 16;  // distinct scratch sector from ahci_selftest()

    static uint8_t save_buf[512];
    static uint8_t test_buf[512];
    static uint8_t read_buf[512];

    if (ahci_read_ncq(port_num, lba, 1, save_buf) != 1) {
        kprintf("[AHCI] selftest_ncq: initial read FAILED\n");
        return;
    }
    for (int i = 0; i < 512; i++) test_buf[i] = (uint8_t)(i ^ 0x5A);
    test_buf[0]='N'; test_buf[1]='C'; test_buf[2]='Q'; test_buf[3]='!';

    if (ahci_write_ncq(port_num, lba, 1, test_buf) != 1) {
        kprintf("[AHCI] selftest_ncq: WRITE (FPDMA QUEUED) FAILED\n");
        return;
    }
    memset(read_buf, 0, 512);
    if (ahci_read_ncq(port_num, lba, 1, read_buf) != 1) {
        kprintf("[AHCI] selftest_ncq: read-back (FPDMA QUEUED) FAILED\n");
        return;
    }
    int ok = (memcmp(read_buf, test_buf, 512) == 0);
    kprintf("[AHCI] selftest_ncq: NCQ write+readback of LBA %lu -> %s\n",
            (unsigned long)lba, ok ? "PASS" : "FAIL");

    // Restore the original contents (plain write is fine for restore).
    ahci_write_ncq(port_num, lba, 1, save_buf);
}
