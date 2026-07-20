// acpi.h - ACPI (Advanced Configuration and Power Interface) driver
#ifndef ACPI_H
#define ACPI_H

#include "../types.h"

// ACPI table signatures
#define ACPI_SIG_RSDP   "RSD PTR "  // Root System Description Pointer
#define ACPI_SIG_RSDT   "RSDT"      // Root System Description Table
#define ACPI_SIG_XSDT   "XSDT"      // Extended System Description Table
#define ACPI_SIG_FADT   "FACP"      // Fixed ACPI Description Table
#define ACPI_SIG_DSDT   "DSDT"      // Differentiated System Description Table

// ACPI PM1 Control register bits
#define ACPI_SLP_EN     (1 << 13)   // Sleep enable bit

// ACPI PM1 Event register bits (#298 power button)
#define ACPI_PWRBTN_STS (1 << 8)    // Power button status (PM1 status block)
#define ACPI_PWRBTN_EN  (1 << 8)    // Power button enable  (PM1 enable block)
#define ACPI_SCI_EN     (1 << 0)    // SCI enable bit in PM1 control

// ACPI sleep states
#define ACPI_S0         0           // Working
#define ACPI_S1         1           // Sleeping with processor context maintained
#define ACPI_S2         2           // Sleeping, processor context lost
#define ACPI_S3         3           // Sleeping, Suspend to RAM
#define ACPI_S4         4           // Sleeping, Suspend to Disk
#define ACPI_S5         5           // Soft Off

// FADT flags
#define ACPI_FADT_RESET_REG_SUP (1 << 10)  // Reset register supported

// Generic Address Structure (GAS)
typedef struct {
    uint8_t  address_space_id;   // Address space (0=Memory, 1=I/O)
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;        // 0=undefined, 1=byte, 2=word, 3=dword, 4=qword
    uint64_t address;
} __attribute__((packed)) acpi_gas_t;

// RSDP (Root System Description Pointer) - ACPI 1.0
typedef struct {
    char     signature[8];       // "RSD PTR "
    uint8_t  checksum;           // Checksum of first 20 bytes
    char     oem_id[6];          // OEM identifier
    uint8_t  revision;           // 0 for ACPI 1.0, 2 for ACPI 2.0+
    uint32_t rsdt_address;       // Physical address of RSDT
} __attribute__((packed)) acpi_rsdp_t;

// RSDP Extended (ACPI 2.0+)
typedef struct {
    acpi_rsdp_t rsdp;            // First 20 bytes (ACPI 1.0 compatible)
    uint32_t length;             // Length of entire table
    uint64_t xsdt_address;       // Physical address of XSDT
    uint8_t  extended_checksum;  // Checksum of entire table
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_ext_t;

// Standard ACPI table header
typedef struct {
    char     signature[4];       // Table signature
    uint32_t length;             // Length of entire table
    uint8_t  revision;           // Table revision
    uint8_t  checksum;           // Checksum
    char     oem_id[6];          // OEM identifier
    char     oem_table_id[8];    // OEM table identifier
    uint32_t oem_revision;       // OEM revision
    uint32_t creator_id;         // Creator ID
    uint32_t creator_revision;   // Creator revision
} __attribute__((packed)) acpi_sdt_header_t;

// RSDT (Root System Description Table)
typedef struct {
    acpi_sdt_header_t header;
    uint32_t entries[];          // Array of 32-bit physical addresses
} __attribute__((packed)) acpi_rsdt_t;

// XSDT (Extended System Description Table)
typedef struct {
    acpi_sdt_header_t header;
    uint64_t entries[];          // Array of 64-bit physical addresses
} __attribute__((packed)) acpi_xsdt_t;

// FADT (Fixed ACPI Description Table)
typedef struct {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;      // Physical address of FACS
    uint32_t dsdt;               // Physical address of DSDT
    uint8_t  reserved1;          // Reserved (ACPI 1.0)
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;            // SCI interrupt
    uint32_t smi_cmd;            // SMI command port
    uint8_t  acpi_enable;        // Value to enable ACPI
    uint8_t  acpi_disable;       // Value to disable ACPI
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;       // PM1a Event Block
    uint32_t pm1b_evt_blk;       // PM1b Event Block
    uint32_t pm1a_cnt_blk;       // PM1a Control Block
    uint32_t pm1b_cnt_blk;       // PM1b Control Block
    uint32_t pm2_cnt_blk;        // PM2 Control Block
    uint32_t pm_tmr_blk;         // PM Timer Block
    uint32_t gpe0_blk;           // GPE0 Block
    uint32_t gpe1_blk;           // GPE1 Block
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    acpi_gas_t reset_reg;        // Reset register
    uint8_t  reset_value;        // Value to write for reset
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    // Extended fields (ACPI 2.0+)
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_evt_blk;
    acpi_gas_t x_pm1b_evt_blk;
    acpi_gas_t x_pm1a_cnt_blk;
    acpi_gas_t x_pm1b_cnt_blk;
    acpi_gas_t x_pm2_cnt_blk;
    acpi_gas_t x_pm_tmr_blk;
    acpi_gas_t x_gpe0_blk;
    acpi_gas_t x_gpe1_blk;
} __attribute__((packed)) acpi_fadt_t;

// ACPI state structure
typedef struct {
    bool initialized;            // ACPI successfully initialized
    uint8_t revision;            // ACPI revision
    acpi_rsdt_t *rsdt;           // Root System Description Table
    acpi_xsdt_t *xsdt;           // Extended System Description Table (ACPI 2.0+)
    acpi_fadt_t *fadt;           // Fixed ACPI Description Table
    uint16_t slp_typa;           // SLP_TYPa value for S5
    uint16_t slp_typb;           // SLP_TYPb value for S5
    bool reset_supported;        // Reset register is supported
    // #298: power-button SCI support so host "qm shutdown" (ACPI power button)
    // actually triggers an orderly power-off instead of timing out.
    uint32_t pm1a_evt_blk;       // PM1a Event Block (status + enable)
    uint32_t pm1b_evt_blk;       // PM1b Event Block (status + enable), may be 0
    uint8_t  pm1_evt_len;        // Length of a PM1 event block (status=enable=len/2)
    uint32_t smi_cmd;            // SMI command port (to enable ACPI mode)
    uint8_t  acpi_enable_val;    // Value to write to smi_cmd to enable ACPI mode
    uint8_t  sci_int;            // SCI interrupt (legacy IRQ line)
    bool     sci_armed;          // Power-button SCI handler installed + armed
} acpi_state_t;

// Initialize ACPI subsystem
// Returns 0 on success, -1 on failure
int acpi_init(void);

// Shutdown the system (power off)
// Returns only on failure
void acpi_shutdown(void);

// Reboot the system
// Returns only on failure
void acpi_reboot(void);

// #298: enable ACPI mode + arm the power-button SCI so host "qm shutdown"
// (an ACPI power-button press) is caught and turned into an orderly power-off.
// Safe no-op if ACPI/FADT was not parsed.
void acpi_enable_power_button(void);

// Flush filesystems then power off (used by the SCI handler / shutdown paths).
void acpi_shutdown_flush(void);

// Check if ACPI is initialized
bool acpi_is_initialized(void);

// Get ACPI revision
uint8_t acpi_get_revision(void);

// Find an ACPI table by signature
// Returns pointer to table header, or NULL if not found
acpi_sdt_header_t *acpi_find_table(const char *signature);

#endif // ACPI_H
