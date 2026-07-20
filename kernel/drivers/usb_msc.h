// usb_msc.h - USB Mass Storage Class Driver (Enhanced)
// Supports USB flash drives, external hard drives with proper enumeration
#ifndef USB_MSC_H
#define USB_MSC_H

#include "../types.h"
#include "xhci.h"

// =============================================================================
// Mass Storage Class Constants
// =============================================================================

// USB Mass Storage Class Codes
#define USB_MSC_CLASS           0x08
#define USB_MSC_SUBCLASS_RBC    0x01    // Reduced Block Commands
#define USB_MSC_SUBCLASS_ATAPI  0x02    // SFF-8020i/MMC-2 (CD/DVD)
#define USB_MSC_SUBCLASS_QIC    0x03    // QIC-157 (tape)
#define USB_MSC_SUBCLASS_UFI    0x04    // UFI (floppy)
#define USB_MSC_SUBCLASS_SFF    0x05    // SFF-8070i
#define USB_MSC_SUBCLASS_SCSI   0x06    // SCSI transparent command set
#define USB_MSC_PROTOCOL_CBI_INT 0x00   // Control/Bulk/Interrupt with command completion interrupt
#define USB_MSC_PROTOCOL_CBI    0x01    // Control/Bulk/Interrupt without command completion interrupt
#define USB_MSC_PROTOCOL_BBB    0x50    // Bulk-Only (BBB) Transport

// Mass Storage Class Requests
#define USB_MSC_REQ_ADSC        0x00    // Accept Device-Specific Command
#define USB_MSC_REQ_GET_MAX_LUN 0xFE    // Get Max LUN
#define USB_MSC_REQ_RESET       0xFF    // Bulk-Only Mass Storage Reset

// CBW (Command Block Wrapper) - 31 bytes
#define USB_MSC_CBW_SIGNATURE   0x43425355  // "USBC"
#define USB_MSC_CBW_SIZE        31

// CSW (Command Status Wrapper) - 13 bytes
#define USB_MSC_CSW_SIGNATURE   0x53425355  // "USBS"
#define USB_MSC_CSW_SIZE        13

// CSW Status codes
#define USB_MSC_CSW_PASSED      0x00
#define USB_MSC_CSW_FAILED      0x01
#define USB_MSC_CSW_PHASE_ERROR 0x02

// Transfer timeout (milliseconds)
#define USB_MSC_TIMEOUT_MS      5000

// =============================================================================
// SCSI Commands
// =============================================================================

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_FORMAT_UNIT        0x04
#define SCSI_INQUIRY            0x12
#define SCSI_MODE_SELECT_6      0x15
#define SCSI_MODE_SENSE_6       0x1A
#define SCSI_START_STOP_UNIT    0x1B
#define SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_READ_FORMAT_CAPACITIES 0x23
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_SEEK_10            0x2B
#define SCSI_WRITE_VERIFY_10    0x2E
#define SCSI_VERIFY_10          0x2F
#define SCSI_SYNCHRONIZE_CACHE  0x35
#define SCSI_WRITE_BUFFER       0x3B
#define SCSI_READ_BUFFER        0x3C
#define SCSI_MODE_SELECT_10     0x55
#define SCSI_MODE_SENSE_10      0x5A
#define SCSI_READ_12            0xA8
#define SCSI_WRITE_12           0xAA
#define SCSI_READ_16            0x88
#define SCSI_WRITE_16           0x8A
#define SCSI_READ_CAPACITY_16   0x9E
#define SCSI_SERVICE_ACTION_IN  0x9E

// SCSI Sense Keys
#define SCSI_SENSE_NO_SENSE     0x00
#define SCSI_SENSE_RECOVERED    0x01
#define SCSI_SENSE_NOT_READY    0x02
#define SCSI_SENSE_MEDIUM_ERROR 0x03
#define SCSI_SENSE_HARDWARE_ERROR 0x04
#define SCSI_SENSE_ILLEGAL_REQUEST 0x05
#define SCSI_SENSE_UNIT_ATTENTION 0x06
#define SCSI_SENSE_DATA_PROTECT 0x07
#define SCSI_SENSE_BLANK_CHECK  0x08
#define SCSI_SENSE_ABORTED_COMMAND 0x0B

// =============================================================================
// Data Structures
// =============================================================================

// Command Block Wrapper (CBW)
typedef struct {
    uint32_t signature;         // 0x43425355
    uint32_t tag;               // Command tag
    uint32_t data_transfer_len; // Expected data transfer length
    uint8_t  flags;             // Direction: 0x80 = IN, 0x00 = OUT
    uint8_t  lun;               // Logical Unit Number
    uint8_t  cb_length;         // Command block length (1-16)
    uint8_t  cb[16];            // Command block
} __attribute__((packed)) usb_msc_cbw_t;

// Command Status Wrapper (CSW)
typedef struct {
    uint32_t signature;         // 0x53425355
    uint32_t tag;               // Command tag (same as CBW)
    uint32_t data_residue;      // Difference between expected and actual data
    uint8_t  status;            // Command status
} __attribute__((packed)) usb_msc_csw_t;

// SCSI Inquiry Response (36 bytes standard)
typedef struct {
    uint8_t  peripheral;        // Peripheral qualifier and device type
    uint8_t  removable;         // Removable media
    uint8_t  version;           // SCSI version
    uint8_t  response_format;   // Response data format
    uint8_t  additional_len;    // Additional length
    uint8_t  flags[3];          // Various flags
    char     vendor[8];         // Vendor identification
    char     product[16];       // Product identification
    char     revision[4];       // Product revision
} __attribute__((packed)) scsi_inquiry_t;

// SCSI Read Capacity (10) Response
typedef struct {
    uint32_t last_lba;          // Last logical block address (big-endian)
    uint32_t block_size;        // Block size in bytes (big-endian)
} __attribute__((packed)) scsi_read_capacity_10_t;

// SCSI Read Capacity (16) Response
typedef struct {
    uint64_t last_lba;          // Last logical block address (big-endian)
    uint32_t block_size;        // Block size in bytes (big-endian)
    uint8_t  flags;
    uint8_t  reserved[19];
} __attribute__((packed)) scsi_read_capacity_16_t;

// SCSI Request Sense Response
typedef struct {
    uint8_t  response_code;     // 0x70 or 0x71
    uint8_t  obsolete;
    uint8_t  sense_key;         // Sense key (lower 4 bits)
    uint8_t  information[4];
    uint8_t  additional_len;
    uint8_t  cmd_specific[4];
    uint8_t  asc;               // Additional Sense Code
    uint8_t  ascq;              // Additional Sense Code Qualifier
    uint8_t  field_replaceable;
    uint8_t  sense_key_specific[3];
} __attribute__((packed)) scsi_sense_t;

// =============================================================================
// USB Mass Storage Device
// =============================================================================

#define USB_MSC_MAX_LUNS    8

// Per-LUN information
typedef struct {
    uint64_t num_blocks;        // Total number of blocks
    uint32_t block_size;        // Block size (typically 512)
    int ready;                  // Unit is ready
    int removable;              // Media is removable
    int write_protected;        // Media is write-protected
    char vendor[9];             // Vendor string (null-terminated)
    char product[17];           // Product string (null-terminated)
    char revision[5];           // Revision string (null-terminated)
} usb_msc_lun_t;

typedef struct {
    // USB device info
    xhci_controller_t *controller;
    int slot_id;
    int interface_num;
    int bulk_in_ep;             // Bulk IN endpoint number
    int bulk_out_ep;            // Bulk OUT endpoint number
    int max_packet_in;
    int max_packet_out;

    // Device info
    uint8_t max_lun;            // Maximum logical unit number
    uint32_t tag;               // CBW/CSW tag counter
    usb_msc_lun_t luns[USB_MSC_MAX_LUNS];

    // For compatibility (LUN 0 shortcuts)
    uint64_t num_blocks;
    uint32_t block_size;
    char vendor[9];
    char product[17];
    int ready;
    int removable;

    // Device identification
    uint16_t vendor_id;
    uint16_t product_id;

    // Mount state
    int mounted;                // Is filesystem mounted?
    char mount_point[32];       // Mount point (e.g., "/usb0")
} usb_msc_device_t;

// =============================================================================
// Hotplug Event Types
// =============================================================================

typedef enum {
    USB_MSC_EVENT_INSERTED,     // Device inserted and enumerated
    USB_MSC_EVENT_REMOVED,      // Device removed
    USB_MSC_EVENT_MOUNT_READY,  // Device ready to mount
    USB_MSC_EVENT_UNMOUNTED,    // Device unmounted (safe to remove)
    USB_MSC_EVENT_MEDIA_CHANGE  // Media changed (for card readers)
} usb_msc_event_type_t;

typedef struct {
    usb_msc_event_type_t type;
    int device_index;
    usb_msc_device_t *device;
} usb_msc_event_t;

// Hotplug callback function type
typedef void (*usb_msc_hotplug_callback_t)(usb_msc_event_t *event);

// =============================================================================
// Function Prototypes
// =============================================================================

// Initialize Mass Storage subsystem
void usb_msc_init(void);

// Probe and attach Mass Storage device
int usb_msc_probe(xhci_controller_t *xhc, int slot_id,
                  uint8_t interface_class, uint8_t interface_subclass,
                  uint8_t interface_protocol);

// Device enumeration with endpoint discovery
int usb_msc_enumerate(xhci_controller_t *xhc, int slot_id, int interface_num,
                      int bulk_in_ep, int bulk_out_ep,
                      int max_packet_in, int max_packet_out);

// Mass Storage class requests
int usb_msc_reset(usb_msc_device_t *dev);
int usb_msc_get_max_lun(usb_msc_device_t *dev);

// SCSI commands
int usb_msc_test_unit_ready(usb_msc_device_t *dev, uint8_t lun);
int usb_msc_inquiry(usb_msc_device_t *dev, uint8_t lun, scsi_inquiry_t *inquiry);
int usb_msc_read_capacity(usb_msc_device_t *dev, uint8_t lun);
int usb_msc_read_capacity_16(usb_msc_device_t *dev, uint8_t lun);
int usb_msc_request_sense(usb_msc_device_t *dev, uint8_t lun, scsi_sense_t *sense);
int usb_msc_start_stop_unit(usb_msc_device_t *dev, uint8_t lun, int start, int load_eject);
int usb_msc_prevent_allow_removal(usb_msc_device_t *dev, uint8_t lun, int prevent);

// Block I/O
int usb_msc_read(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                 void *buf, uint32_t num_blocks);
int usb_msc_write(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                  const void *buf, uint32_t num_blocks);
int usb_msc_sync(usb_msc_device_t *dev, uint8_t lun);

// Large block I/O (for >2TB drives)
int usb_msc_read_16(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                    void *buf, uint32_t num_blocks);
int usb_msc_write_16(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                     const void *buf, uint32_t num_blocks);

// Bulk-Only Transport
int usb_msc_transport(usb_msc_device_t *dev, usb_msc_cbw_t *cbw,
                      void *data, usb_msc_csw_t *csw);

// Error recovery
int usb_msc_clear_stall(usb_msc_device_t *dev, int endpoint);
int usb_msc_bulk_reset_recovery(usb_msc_device_t *dev);

// Device access
usb_msc_device_t *usb_msc_get_device(int index);
int usb_msc_get_device_count(void);
int usb_msc_find_device_by_slot(int slot_id);

// Device removal
void usb_msc_device_removed(int slot_id);

// Safe eject
int usb_msc_eject(usb_msc_device_t *dev);
int usb_msc_safe_remove(int device_index);

// Hotplug callback registration
void usb_msc_register_hotplug_callback(usb_msc_hotplug_callback_t callback);
void usb_msc_unregister_hotplug_callback(void);

// Polling for hotplug events (called from main loop)
void usb_msc_poll_hotplug(void);

// Debug
void usb_msc_print_devices(void);
const char *usb_msc_sense_key_name(uint8_t sense_key);

// #307: boot self-test (SCSI READ(10) + FAT root-dir listing) and its worker.
void usb_msc_selftest(void);
void usb_msc_start_selftest(void);

#endif // USB_MSC_H
