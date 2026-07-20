// ahci.h - AHCI (Advanced Host Controller Interface) driver for SATA
// Part of MayteraOS - Task #48
//
// AHCI is the standard interface for SATA host controllers.
// It provides a memory-mapped register interface for:
// - Port multiplier support
// - Native Command Queuing (NCQ)
// - Hot-plug support
// - Power management

#ifndef AHCI_H
#define AHCI_H

#include "../types.h"
#include "pci.h"

// AHCI PCI class codes
#define AHCI_CLASS_MASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA      0x06
#define AHCI_PROG_IF_AHCI       0x01

// HBA Memory Registers (Generic Host Control)
#define HBA_CAP         0x00    // Host Capabilities
#define HBA_GHC         0x04    // Global Host Control
#define HBA_IS          0x08    // Interrupt Status
#define HBA_PI          0x0C    // Ports Implemented
#define HBA_VS          0x10    // Version
#define HBA_CCC_CTL     0x14    // Command Completion Coalescing Control
#define HBA_CCC_PORTS   0x18    // Command Completion Coalescing Ports
#define HBA_EM_LOC      0x1C    // Enclosure Management Location
#define HBA_EM_CTL      0x20    // Enclosure Management Control
#define HBA_CAP2        0x24    // Host Capabilities Extended
#define HBA_BOHC        0x28    // BIOS/OS Handoff Control and Status

// Port registers (offset from port base = 0x100 + port * 0x80)
#define PORT_CLB        0x00    // Command List Base Address
#define PORT_CLBU       0x04    // Command List Base Address Upper 32-bits
#define PORT_FB         0x08    // FIS Base Address
#define PORT_FBU        0x0C    // FIS Base Address Upper 32-bits
#define PORT_IS         0x10    // Interrupt Status
#define PORT_IE         0x14    // Interrupt Enable
#define PORT_CMD        0x18    // Command and Status
#define PORT_TFD        0x20    // Task File Data
#define PORT_SIG        0x24    // Signature
#define PORT_SSTS       0x28    // SATA Status (SCR0: SStatus)
#define PORT_SCTL       0x2C    // SATA Control (SCR2: SControl)
#define PORT_SERR       0x30    // SATA Error (SCR1: SError)
#define PORT_SACT       0x34    // SATA Active (SCR3: SActive)
#define PORT_CI         0x38    // Command Issue
#define PORT_SNTF       0x3C    // SATA Notification (SCR4: SNotification)
#define PORT_FBS        0x40    // FIS-based Switching Control

// HBA Capabilities (CAP) bits
#define HBA_CAP_S64A    (1 << 31)   // 64-bit Addressing
#define HBA_CAP_SNCQ    (1 << 30)   // Native Command Queuing
#define HBA_CAP_SSNTF   (1 << 29)   // SNotification Register
#define HBA_CAP_SMPS    (1 << 28)   // Mechanical Presence Switch
#define HBA_CAP_SSS     (1 << 27)   // Staggered Spin-up
#define HBA_CAP_SALP    (1 << 26)   // Aggressive Link Power Management
#define HBA_CAP_SAL     (1 << 25)   // Activity LED
#define HBA_CAP_SCLO    (1 << 24)   // Command List Override
#define HBA_CAP_ISS_MASK (0xF << 20) // Interface Speed Support
#define HBA_CAP_SAM     (1 << 18)   // AHCI Only
#define HBA_CAP_SPM     (1 << 17)   // Port Multiplier
#define HBA_CAP_FBSS    (1 << 16)   // FIS-based Switching Support
#define HBA_CAP_PMD     (1 << 15)   // PIO Multiple DRQ Block
#define HBA_CAP_SSC     (1 << 14)   // Slumber State Capable
#define HBA_CAP_PSC     (1 << 13)   // Partial State Capable
#define HBA_CAP_NCS_MASK (0x1F << 8) // Number of Command Slots
#define HBA_CAP_CCCS    (1 << 7)    // Command Completion Coalescing
#define HBA_CAP_EMS     (1 << 6)    // Enclosure Management
#define HBA_CAP_SXS     (1 << 5)    // External SATA
#define HBA_CAP_NP_MASK 0x1F        // Number of Ports

// Global Host Control (GHC) bits
#define HBA_GHC_AE      (1 << 31)   // AHCI Enable
#define HBA_GHC_MRSM    (1 << 2)    // MSI Revert to Single Message
#define HBA_GHC_IE      (1 << 1)    // Interrupt Enable
#define HBA_GHC_HR      (1 << 0)    // HBA Reset

// Port Command and Status (PxCMD) bits
#define PORT_CMD_ICC_MASK   (0xF << 28) // Interface Communication Control
#define PORT_CMD_ASP    (1 << 27)   // Aggressive Slumber/Partial
#define PORT_CMD_ALPE   (1 << 26)   // Aggressive Link Power Management Enable
#define PORT_CMD_DLAE   (1 << 25)   // Drive LED on ATAPI Enable
#define PORT_CMD_ATAPI  (1 << 24)   // Device is ATAPI
#define PORT_CMD_APSTE  (1 << 23)   // Auto Partial to Slumber Transitions Enable
#define PORT_CMD_FBSCP  (1 << 22)   // FIS-based Switching Capable Port
#define PORT_CMD_ESP    (1 << 21)   // External SATA Port
#define PORT_CMD_CPD    (1 << 20)   // Cold Presence Detection
#define PORT_CMD_MPSP   (1 << 19)   // Mechanical Presence Switch State
#define PORT_CMD_HPCP   (1 << 18)   // Hot Plug Capable Port
#define PORT_CMD_PMA    (1 << 17)   // Port Multiplier Attached
#define PORT_CMD_CPS    (1 << 16)   // Cold Presence State
#define PORT_CMD_CR     (1 << 15)   // Command List Running
#define PORT_CMD_FR     (1 << 14)   // FIS Receive Running
#define PORT_CMD_MPSS   (1 << 13)   // Mechanical Presence Switch State
#define PORT_CMD_CCS_MASK (0x1F << 8) // Current Command Slot
#define PORT_CMD_FRE    (1 << 4)    // FIS Receive Enable
#define PORT_CMD_CLO    (1 << 3)    // Command List Override
#define PORT_CMD_POD    (1 << 2)    // Power On Device
#define PORT_CMD_SUD    (1 << 1)    // Spin-Up Device
#define PORT_CMD_ST     (1 << 0)    // Start

// Port Task File Data (PxTFD) bits
#define PORT_TFD_ERR_MASK   (0xFF << 8)
#define PORT_TFD_STS_MASK   0xFF
#define PORT_TFD_STS_ERR    (1 << 0)
#define PORT_TFD_STS_DRQ    (1 << 3)
#define PORT_TFD_STS_BSY    (1 << 7)

// Port SATA Status (PxSSTS) bits
#define PORT_SSTS_DET_MASK  0x0F    // Device Detection
#define PORT_SSTS_SPD_MASK  0xF0    // Current Interface Speed
#define PORT_SSTS_IPM_MASK  0xF00   // Interface Power Management

// Device detection values
#define PORT_SSTS_DET_NONE      0x0     // No device
#define PORT_SSTS_DET_PRESENT   0x1     // Device present, no PHY
#define PORT_SSTS_DET_PHY       0x3     // Device present, PHY established
#define PORT_SSTS_DET_OFFLINE   0x4     // PHY offline

// Device signatures
#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM     0x96690101  // Port multiplier

// Port Interrupt bits
#define PORT_INT_CPD    (1 << 31)   // Cold Presence Detect Status
#define PORT_INT_TFE    (1 << 30)   // Task File Error Status
#define PORT_INT_HBF    (1 << 29)   // Host Bus Fatal Error Status
#define PORT_INT_HBD    (1 << 28)   // Host Bus Data Error Status
#define PORT_INT_IF     (1 << 27)   // Interface Fatal Error Status
#define PORT_INT_INF    (1 << 26)   // Interface Non-fatal Error Status
#define PORT_INT_OF     (1 << 24)   // Overflow Status
#define PORT_INT_IPM    (1 << 23)   // Incorrect Port Multiplier Status
#define PORT_INT_PRC    (1 << 22)   // PhyRdy Change Status
#define PORT_INT_DMP    (1 << 7)    // Device Mechanical Presence Status
#define PORT_INT_PC     (1 << 6)    // Port Connect Change Status
#define PORT_INT_DP     (1 << 5)    // Descriptor Processed
#define PORT_INT_UF     (1 << 4)    // Unknown FIS Interrupt
#define PORT_INT_SDB    (1 << 3)    // Set Device Bits Interrupt
#define PORT_INT_DS     (1 << 2)    // DMA Setup FIS Interrupt
#define PORT_INT_PS     (1 << 1)    // PIO Setup FIS Interrupt
#define PORT_INT_DHR    (1 << 0)    // Device to Host Register FIS Interrupt

// FIS Types
#define FIS_TYPE_REG_H2D    0x27    // Register FIS - Host to Device
#define FIS_TYPE_REG_D2H    0x34    // Register FIS - Device to Host
#define FIS_TYPE_DMA_ACT    0x39    // DMA Activate FIS
#define FIS_TYPE_DMA_SETUP  0x41    // DMA Setup FIS
#define FIS_TYPE_DATA       0x46    // Data FIS
#define FIS_TYPE_BIST       0x58    // BIST Activate FIS
#define FIS_TYPE_PIO_SETUP  0x5F    // PIO Setup FIS
#define FIS_TYPE_DEV_BITS   0xA1    // Set Device Bits FIS

// ATA Commands
#define ATA_CMD_READ_DMA_EX     0x25    // READ DMA EXT
#define ATA_CMD_WRITE_DMA_EX    0x35    // WRITE DMA EXT
#define ATA_CMD_READ_FPDMA_Q    0x60    // READ FPDMA QUEUED (NCQ)
#define ATA_CMD_WRITE_FPDMA_Q   0x61    // WRITE FPDMA QUEUED (NCQ)
#define ATA_CMD_IDENTIFY        0xEC    // IDENTIFY DEVICE
#define ATA_CMD_FLUSH_CACHE_EX  0xEA    // FLUSH CACHE EXT

// Command slot count
#define AHCI_MAX_PORTS      32
#define AHCI_MAX_CMD_SLOTS  32
#define AHCI_SECTOR_SIZE    512
#define AHCI_PRD_MAX_SIZE   (4 * 1024 * 1024)  // 4MB per PRD entry

// FIS structures
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_REG_H2D
    uint8_t pmport_c;       // Port multiplier, C bit
    uint8_t command;        // Command register
    uint8_t feature_low;    // Feature register low byte
    
    uint8_t lba0;           // LBA bits 0-7
    uint8_t lba1;           // LBA bits 8-15
    uint8_t lba2;           // LBA bits 16-23
    uint8_t device;         // Device register
    
    uint8_t lba3;           // LBA bits 24-31
    uint8_t lba4;           // LBA bits 32-39
    uint8_t lba5;           // LBA bits 40-47
    uint8_t feature_high;   // Feature register high byte
    
    uint8_t count_low;      // Count register low byte
    uint8_t count_high;     // Count register high byte
    uint8_t icc;            // Isochronous command completion
    uint8_t control;        // Control register
    
    uint8_t reserved[4];    // Reserved
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint8_t fis_type;       // FIS_TYPE_REG_D2H
    uint8_t pmport_i;       // Port multiplier, Interrupt bit
    uint8_t status;         // Status register
    uint8_t error;          // Error register
    
    uint8_t lba0;           // LBA bits 0-7
    uint8_t lba1;           // LBA bits 8-15
    uint8_t lba2;           // LBA bits 16-23
    uint8_t device;         // Device register
    
    uint8_t lba3;           // LBA bits 24-31
    uint8_t lba4;           // LBA bits 32-39
    uint8_t lba5;           // LBA bits 40-47
    uint8_t reserved1;
    
    uint8_t count_low;      // Count register low byte
    uint8_t count_high;     // Count register high byte
    uint8_t reserved2[2];
    
    uint8_t reserved3[4];
} __attribute__((packed)) fis_reg_d2h_t;

typedef struct {
    uint8_t fis_type;       // FIS_TYPE_PIO_SETUP
    uint8_t pmport_di;      // Port multiplier, direction, interrupt
    uint8_t status;         // Status register
    uint8_t error;          // Error register
    
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved1;
    
    uint8_t count_low;
    uint8_t count_high;
    uint8_t reserved2;
    uint8_t e_status;       // New status
    
    uint16_t transfer_count;
    uint8_t reserved3[2];
} __attribute__((packed)) fis_pio_setup_t;

typedef struct {
    uint8_t fis_type;       // FIS_TYPE_DMA_SETUP
    uint8_t pmport_aid;     // Port multiplier, auto-activate, direction
    uint8_t reserved1[2];
    
    uint64_t dma_buffer_id;
    uint32_t reserved2;
    uint32_t dma_buffer_offset;
    uint32_t transfer_count;
    uint32_t reserved3;
} __attribute__((packed)) fis_dma_setup_t;

// Received FIS area (256 bytes minimum)
typedef struct {
    fis_dma_setup_t dsfis;      // DMA Setup FIS
    uint8_t pad0[4];
    
    fis_pio_setup_t psfis;      // PIO Setup FIS
    uint8_t pad1[12];
    
    fis_reg_d2h_t rfis;         // D2H Register FIS
    uint8_t pad2[4];
    
    uint8_t sdbfis[8];          // Set Device Bits FIS
    
    uint8_t ufis[64];           // Unknown FIS
    
    uint8_t reserved[0x60];     // Reserved
} __attribute__((packed)) hba_fis_t;

// Command Header (32 bytes)
typedef struct {
    // DW0
    uint8_t cfl:5;          // Command FIS Length in DWORDs (2-16)
    uint8_t atapi:1;        // ATAPI
    uint8_t write:1;        // Write (1=H2D, 0=D2H)
    uint8_t prefetch:1;     // Prefetchable
    
    uint8_t reset:1;        // Reset
    uint8_t bist:1;         // BIST
    uint8_t clear_busy:1;   // Clear Busy upon R_OK
    uint8_t reserved0:1;
    uint8_t pmp:4;          // Port Multiplier Port
    
    uint16_t prdtl;         // Physical Region Descriptor Table Length
    
    // DW1
    volatile uint32_t prdbc;    // PRD Byte Count (updated by HBA)
    
    // DW2-3
    uint64_t ctba;          // Command Table Base Address
    
    // DW4-7
    uint32_t reserved1[4];
} __attribute__((packed)) hba_cmd_header_t;

// Physical Region Descriptor Table Entry (16 bytes)
typedef struct {
    uint64_t dba;           // Data Base Address
    uint32_t reserved0;
    uint32_t dbc:22;        // Byte Count (0-based, 4MB max)
    uint32_t reserved1:9;
    uint32_t i:1;           // Interrupt on Completion
} __attribute__((packed)) hba_prdt_entry_t;

// Command Table (128-byte aligned)
typedef struct {
    uint8_t cfis[64];       // Command FIS
    uint8_t acmd[16];       // ATAPI Command
    uint8_t reserved[48];   // Reserved
    hba_prdt_entry_t prdt_entry[]; // PRD entries (variable)
} __attribute__((packed)) hba_cmd_table_t;

// AHCI device types
typedef enum {
    AHCI_DEV_NULL,
    AHCI_DEV_SATA,
    AHCI_DEV_SATAPI,
    AHCI_DEV_SEMB,
    AHCI_DEV_PM
} ahci_device_type_t;

// AHCI port state
typedef struct {
    bool present;
    ahci_device_type_t device_type;
    volatile uint32_t *port_regs;
    
    // Memory areas (must be physically contiguous)
    hba_cmd_header_t *cmd_list;     // Command list (1KB aligned)
    hba_fis_t *fis_area;            // Received FIS (256B aligned)
    hba_cmd_table_t *cmd_tables[AHCI_MAX_CMD_SLOTS];  // Command tables
    
    // Physical addresses
    uint64_t cmd_list_phys;
    uint64_t fis_area_phys;
    uint64_t cmd_tables_phys[AHCI_MAX_CMD_SLOTS];
    
    // Device information
    char model[41];
    char serial[21];
    uint64_t sector_count;
    uint32_t sector_size;
    bool ncq_supported;
    uint32_t ncq_depth;
    bool lba48_supported;
} ahci_port_t;

// AHCI HBA state
typedef struct {
    bool initialized;
    pci_device_t *pci_device;
    volatile uint32_t *abar;        // AHCI Base Address (memory-mapped)
    
    // Capabilities
    uint32_t cap;
    uint32_t cap2;
    uint32_t version;
    uint32_t ports_implemented;
    uint32_t num_ports;
    uint32_t num_cmd_slots;
    bool supports_64bit;
    bool supports_ncq;
    
    // Port information
    ahci_port_t ports[AHCI_MAX_PORTS];
} ahci_hba_t;

// Initialize AHCI driver
int ahci_init(void);

// Get the number of detected ports
int ahci_get_port_count(void);

// Get port information
ahci_port_t *ahci_get_port(int port_num);

// Read sectors from an AHCI device
// Returns number of sectors read, or -1 on error
int ahci_read(int port_num, uint64_t lba, uint32_t count, void *buffer);

// Write sectors to an AHCI device
// Returns number of sectors written, or -1 on error
int ahci_write(int port_num, uint64_t lba, uint32_t count, const void *buffer);

// Read using NCQ (if supported)
int ahci_read_ncq(int port_num, uint64_t lba, uint32_t count, void *buffer);

// Write using NCQ (if supported)
int ahci_write_ncq(int port_num, uint64_t lba, uint32_t count, const void *buffer);

// Flush cache
int ahci_flush(int port_num);

// Get device model string
const char *ahci_get_model(int port_num);

// Get device serial number
const char *ahci_get_serial(int port_num);

// Get device size in sectors
uint64_t ahci_get_sector_count(int port_num);

// Return the AHCI port number of the Nth attached SATA disk (0-based), or -1.
int ahci_get_nth_sata_port(int n);

// Print AHCI information
void ahci_print_info(void);

// Run a write+readback self-test on the first SATA disk.
void ahci_selftest(void);

// Run an NCQ (FPDMA QUEUED) write+readback self-test on the first SATA disk.
// No-op if the drive does not report NCQ support.
void ahci_selftest_ncq(void);

// Check if AHCI is initialized
bool ahci_is_initialized(void);

#endif // AHCI_H
