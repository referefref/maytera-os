// ata.h - ATA/IDE disk driver
#ifndef ATA_H
#define ATA_H

#include "../types.h"

// ATA I/O ports (Primary channel)
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

// ATA I/O ports (Secondary channel)
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

// ATA register offsets (from base I/O port)
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LO      0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HI      0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07

// Control register offsets
#define ATA_REG_ALTSTATUS   0x00
#define ATA_REG_DEVCTRL     0x00

// ATA commands
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_READ_PIO_EXT    0x24
#define ATA_CMD_READ_DMA        0xC8
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_WRITE_PIO_EXT   0x34
#define ATA_CMD_WRITE_DMA       0xCA
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_SMART           0xB0
#define ATA_SMART_RETURN_STATUS 0xDA

// Bus Master IDE (BMIDE) register offsets from BAR4
#define BMIDE_CMD       0x00    // Command register
#define BMIDE_STATUS    0x02    // Status register
#define BMIDE_PRDT      0x04    // PRD Table address (32-bit)

// BMIDE Command register bits
#define BMIDE_CMD_START     0x01    // Start/Stop DMA
#define BMIDE_CMD_READ      0x08    // Read (1) or Write (0)

// BMIDE Status register bits
#define BMIDE_STATUS_ACTIVE 0x01    // DMA active
#define BMIDE_STATUS_ERR    0x02    // DMA error
#define BMIDE_STATUS_IRQ    0x04    // Interrupt raised
#define BMIDE_STATUS_DRV0   0x20    // Drive 0 can DMA
#define BMIDE_STATUS_DRV1   0x40    // Drive 1 can DMA
#define BMIDE_STATUS_SIMPLEX 0x80   // Simplex mode only

// Physical Region Descriptor (PRD) entry
typedef struct __attribute__((packed)) {
    uint32_t phys_addr;     // Physical address of data buffer (must be word-aligned)
    uint16_t byte_count;    // Byte count (0 = 64KB)
    uint16_t flags;         // Bit 15: End of Table (EOT)
} prd_entry_t;

#define PRD_EOT     0x8000  // End of PRD table

// ATA status register bits
#define ATA_SR_BSY      0x80    // Busy
#define ATA_SR_DRDY     0x40    // Drive ready
#define ATA_SR_DF       0x20    // Drive write fault
#define ATA_SR_DSC      0x10    // Drive seek complete
#define ATA_SR_DRQ      0x08    // Data request ready
#define ATA_SR_CORR     0x04    // Corrected data
#define ATA_SR_IDX      0x02    // Index
#define ATA_SR_ERR      0x01    // Error

// ATA error register bits
#define ATA_ER_BBK      0x80    // Bad block
#define ATA_ER_UNC      0x40    // Uncorrectable data
#define ATA_ER_MC       0x20    // Media changed
#define ATA_ER_IDNF     0x10    // ID not found
#define ATA_ER_MCR      0x08    // Media change request
#define ATA_ER_ABRT     0x04    // Command aborted
#define ATA_ER_TK0NF    0x02    // Track 0 not found
#define ATA_ER_AMNF     0x01    // Address mark not found

// Drive selection
#define ATA_DRIVE_MASTER    0xA0
#define ATA_DRIVE_SLAVE     0xB0

// Device types
#define ATA_TYPE_NONE       0
#define ATA_TYPE_ATA        1
#define ATA_TYPE_ATAPI      2

// Sector size
#define ATA_SECTOR_SIZE     512

// ATA drive info structure
typedef struct {
    uint8_t  exists;        // Drive exists
    uint8_t  type;          // ATA or ATAPI
    uint8_t  channel;       // 0 = primary, 1 = secondary
    uint8_t  drive;         // 0 = master, 1 = slave
    uint8_t  dma_capable;   // DMA mode supported
    uint8_t  udma_mode;     // Best UDMA mode supported (0-6, 0xFF = none)
    uint16_t signature;     // Drive signature
    uint16_t capabilities;  // Drive capabilities
    uint32_t command_sets;  // Supported command sets
    uint64_t sectors;       // Total sectors (LBA48 or LBA28)
    char     model[41];     // Model string (null-terminated)
    char     serial[21];    // Serial number (null-terminated)
    int8_t   smart_status;   // SMART health: 1=ok, 0=failing, -1=unknown
} ata_drive_t;

// Initialize ATA driver
void ata_init(void);

// Get drive info
ata_drive_t *ata_get_drive(int channel, int drive);

// SMART health check (cached at boot): 1=ok, 0=failing, -1=unknown
int ata_smart_status(uint8_t channel, uint8_t drive);

// Index-based accessors (idx = channel*2 + drive, 0..3) for syscalls.
int ata_drive_present(int idx);
int ata_drive_type(int idx);
int ata_drive_smart(int idx);
unsigned long ata_drive_sectors(int idx);
const char *ata_drive_model(int idx);
const char *ata_drive_serial(int idx);

// Read sectors (LBA28)
int ata_read_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                     uint8_t count, void *buffer);

// Write sectors (LBA28)
int ata_write_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                      uint8_t count, const void *buffer);

// Read sectors (LBA48)
int ata_read_sectors_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                         uint16_t count, void *buffer);

// Write sectors (LBA48)
int ata_write_sectors_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                          uint16_t count, const void *buffer);

// Print drive info
void ata_print_info(void);

// Test read from a drive (for debugging)
void ata_test_read(uint8_t channel, uint8_t drive);

// Get first available ATA drive
int ata_get_first_drive(uint8_t *channel_out, uint8_t *drive_out);

// DMA mode functions

// Check if DMA is available for the given drive
int ata_dma_available(uint8_t channel, uint8_t drive);

// Read sectors using DMA (LBA28)
int ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                         uint8_t count, void *buffer);

// Write sectors using DMA (LBA28)
int ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                          uint8_t count, const void *buffer);

// Read sectors using DMA (LBA48)
int ata_read_sectors_dma_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                             uint16_t count, void *buffer);

// Write sectors using DMA (LBA48)
int ata_write_sectors_dma_ext(uint8_t channel, uint8_t drive, uint64_t lba,
                              uint16_t count, const void *buffer);

// #298: flush all ATA drive write caches (used on shutdown).
void ata_flush_all(void);

#endif // ATA_H

// DMA performance benchmark
void ata_benchmark_dma(uint8_t channel, uint8_t drive);
