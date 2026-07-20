// xhci.h - xHCI (USB 3.0) Host Controller Driver
// Full implementation with transfer rings, device contexts, and enumeration
#ifndef XHCI_H
#define XHCI_H

#include "../types.h"
#include "pci.h"

// =============================================================================
// xHCI Capability Registers (Section 5.3 of xHCI spec)
// =============================================================================

#define XHCI_CAP_CAPLENGTH      0x00    // Capability Register Length (1 byte)
#define XHCI_CAP_HCIVERSION     0x02    // Host Controller Interface Version (2 bytes)
#define XHCI_CAP_HCSPARAMS1     0x04    // Structural Parameters 1
#define XHCI_CAP_HCSPARAMS2     0x08    // Structural Parameters 2
#define XHCI_CAP_HCSPARAMS3     0x0C    // Structural Parameters 3
#define XHCI_CAP_HCCPARAMS1     0x10    // Capability Parameters 1
#define XHCI_CAP_DBOFF          0x14    // Doorbell Offset
#define XHCI_CAP_RTSOFF         0x18    // Runtime Register Space Offset
#define XHCI_CAP_HCCPARAMS2     0x1C    // Capability Parameters 2

// HCSPARAMS1 fields
#define XHCI_HCSPARAMS1_MAX_SLOTS(x)   ((x) & 0xFF)
#define XHCI_HCSPARAMS1_MAX_INTRS(x)   (((x) >> 8) & 0x7FF)
#define XHCI_HCSPARAMS1_MAX_PORTS(x)   (((x) >> 24) & 0xFF)

// HCSPARAMS2 fields
#define XHCI_HCSPARAMS2_IST(x)         ((x) & 0xF)
#define XHCI_HCSPARAMS2_ERST_MAX(x)    (((x) >> 4) & 0xF)
#define XHCI_HCSPARAMS2_SPR(x)         (((x) >> 26) & 1)
#define XHCI_HCSPARAMS2_MAX_SPB_HI(x)  (((x) >> 21) & 0x1F)
#define XHCI_HCSPARAMS2_MAX_SPB_LO(x)  (((x) >> 27) & 0x1F)

// HCCPARAMS1 fields
#define XHCI_HCCPARAMS1_AC64(x)        ((x) & 1)        // 64-bit capable
#define XHCI_HCCPARAMS1_CSZ(x)         (((x) >> 2) & 1) // Context size (0=32, 1=64)
#define XHCI_HCCPARAMS1_XECP(x)        (((x) >> 16) & 0xFFFF) // Extended caps pointer

// =============================================================================
// xHCI Operational Registers (Section 5.4)
// =============================================================================

#define XHCI_OP_USBCMD          0x00    // USB Command
#define XHCI_OP_USBSTS          0x04    // USB Status
#define XHCI_OP_PAGESIZE        0x08    // Page Size
#define XHCI_OP_DNCTRL          0x14    // Device Notification Control
#define XHCI_OP_CRCR            0x18    // Command Ring Control (64-bit)
#define XHCI_OP_DCBAAP          0x30    // Device Context Base Address Array Pointer (64-bit)
#define XHCI_OP_CONFIG          0x38    // Configure

// USBCMD bits
#define XHCI_CMD_RUN            (1 << 0)    // Run/Stop
#define XHCI_CMD_HCRST          (1 << 1)    // Host Controller Reset
#define XHCI_CMD_INTE           (1 << 2)    // Interrupter Enable
#define XHCI_CMD_HSEE           (1 << 3)    // Host System Error Enable
#define XHCI_CMD_LHCRST         (1 << 7)    // Light Host Controller Reset
#define XHCI_CMD_CSS            (1 << 8)    // Controller Save State
#define XHCI_CMD_CRS            (1 << 9)    // Controller Restore State
#define XHCI_CMD_EWE            (1 << 10)   // Enable Wrap Event
#define XHCI_CMD_EU3S           (1 << 11)   // Enable U3 MFINDEX Stop

// USBSTS bits
#define XHCI_STS_HCH            (1 << 0)    // HC Halted
#define XHCI_STS_HSE            (1 << 2)    // Host System Error
#define XHCI_STS_EINT           (1 << 3)    // Event Interrupt
#define XHCI_STS_PCD            (1 << 4)    // Port Change Detect
#define XHCI_STS_SSS            (1 << 8)    // Save State Status
#define XHCI_STS_RSS            (1 << 9)    // Restore State Status
#define XHCI_STS_SRE            (1 << 10)   // Save/Restore Error
#define XHCI_STS_CNR            (1 << 11)   // Controller Not Ready
#define XHCI_STS_HCE            (1 << 12)   // Host Controller Error

// CRCR bits
#define XHCI_CRCR_RCS           (1 << 0)    // Ring Cycle State
#define XHCI_CRCR_CS            (1 << 1)    // Command Stop
#define XHCI_CRCR_CA            (1 << 2)    // Command Abort
#define XHCI_CRCR_CRR           (1 << 3)    // Command Ring Running

// =============================================================================
// Port Status and Control Register (Section 5.4.8)
// =============================================================================

#define XHCI_PORTSC_OFFSET      0x400   // Port registers start at op_base + 0x400

#define XHCI_PORTSC_CCS         (1 << 0)    // Current Connect Status
#define XHCI_PORTSC_PED         (1 << 1)    // Port Enabled/Disabled
#define XHCI_PORTSC_OCA         (1 << 3)    // Over-current Active
#define XHCI_PORTSC_PR          (1 << 4)    // Port Reset
#define XHCI_PORTSC_PLS_MASK    (0xF << 5)  // Port Link State
#define XHCI_PORTSC_PLS_U0      (0 << 5)    // U0 (enabled)
#define XHCI_PORTSC_PLS_U3      (3 << 5)    // U3 (suspended)
#define XHCI_PORTSC_PLS_DISABLED (4 << 5)   // Disabled
#define XHCI_PORTSC_PLS_RXDETECT (5 << 5)   // RxDetect
#define XHCI_PORTSC_PLS_POLLING (7 << 5)    // Polling
#define XHCI_PORTSC_PLS_RESUME  (15 << 5)   // Resume
#define XHCI_PORTSC_PP          (1 << 9)    // Port Power
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10) // Port Speed
#define XHCI_PORTSC_PIC_MASK    (3 << 14)   // Port Indicator Control
#define XHCI_PORTSC_LWS         (1 << 16)   // Port Link State Write Strobe
#define XHCI_PORTSC_CSC         (1 << 17)   // Connect Status Change
#define XHCI_PORTSC_PEC         (1 << 18)   // Port Enabled/Disabled Change
#define XHCI_PORTSC_WRC         (1 << 19)   // Warm Port Reset Change
#define XHCI_PORTSC_OCC         (1 << 20)   // Over-current Change
#define XHCI_PORTSC_PRC         (1 << 21)   // Port Reset Change
#define XHCI_PORTSC_PLC         (1 << 22)   // Port Link State Change
#define XHCI_PORTSC_CEC         (1 << 23)   // Port Config Error Change
#define XHCI_PORTSC_CAS         (1 << 24)   // Cold Attach Status
#define XHCI_PORTSC_WCE         (1 << 25)   // Wake on Connect Enable
#define XHCI_PORTSC_WDE         (1 << 26)   // Wake on Disconnect Enable
#define XHCI_PORTSC_WOE         (1 << 27)   // Wake on Over-current Enable
#define XHCI_PORTSC_DR          (1 << 30)   // Device Removable
#define XHCI_PORTSC_WPR         (1 << 31)   // Warm Port Reset

// Port speed values
#define XHCI_SPEED_FULL         1   // Full Speed (USB 1.1, 12 Mbps)
#define XHCI_SPEED_LOW          2   // Low Speed (USB 1.1, 1.5 Mbps)
#define XHCI_SPEED_HIGH         3   // High Speed (USB 2.0, 480 Mbps)
#define XHCI_SPEED_SUPER        4   // Super Speed (USB 3.0, 5 Gbps)
#define XHCI_SPEED_SUPER_PLUS   5   // Super Speed+ (USB 3.1, 10 Gbps)

// =============================================================================
// Runtime Registers (Section 5.5)
// =============================================================================

#define XHCI_RT_MFINDEX         0x00    // Microframe Index
#define XHCI_RT_IR0             0x20    // Interrupter Register Set 0

// Interrupter Register Set (32 bytes each)
#define XHCI_IR_IMAN            0x00    // Interrupter Management
#define XHCI_IR_IMOD            0x04    // Interrupter Moderation
#define XHCI_IR_ERSTSZ          0x08    // Event Ring Segment Table Size
#define XHCI_IR_ERSTBA          0x10    // Event Ring Segment Table Base Address (64-bit)
#define XHCI_IR_ERDP            0x18    // Event Ring Dequeue Pointer (64-bit)

// IMAN bits
#define XHCI_IMAN_IP            (1 << 0)    // Interrupt Pending
#define XHCI_IMAN_IE            (1 << 1)    // Interrupt Enable

// =============================================================================
// Doorbell Register (Section 5.6)
// =============================================================================

#define XHCI_DB_HOST            0       // Host controller doorbell (command ring)
#define XHCI_DB_DEVICE(slot)    ((slot) & 0xFF) // Device doorbell (slot 1-255)

// Doorbell target values
#define XHCI_DB_TARGET_CONTROL  1       // Control endpoint (EP 0)
#define XHCI_DB_TARGET_EP(ep)   ((ep) + 1) // Endpoint (1-30 for OUT, 2-31 for IN)

// =============================================================================
// Transfer Request Block (TRB) Types (Section 6.4)
// =============================================================================

// TRB Type field (bits 10-15)
#define XHCI_TRB_TYPE(x)        (((x) & 0x3F) << 10)
#define XHCI_TRB_TYPE_GET(x)    (((x) >> 10) & 0x3F)

// Transfer TRB Types
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_ISOCH               5
#define TRB_LINK                6
#define TRB_EVENT_DATA          7
#define TRB_NOOP                8

// Command TRB Types
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIG_EP           12
#define TRB_EVAL_CONTEXT        13
#define TRB_RESET_EP            14
#define TRB_STOP_EP             15
#define TRB_SET_TR_DEQUEUE      16
#define TRB_RESET_DEVICE        17
#define TRB_FORCE_EVENT         18
#define TRB_NEGOTIATE_BW        19
#define TRB_SET_LT              20
#define TRB_GET_BW              21
#define TRB_FORCE_HEADER        22
#define TRB_NOOP_CMD            23

// Event TRB Types
#define TRB_TRANSFER_EVENT      32
#define TRB_COMMAND_COMPLETION  33
#define TRB_PORT_STATUS_CHANGE  34
#define TRB_BANDWIDTH_REQUEST   35
#define TRB_DOORBELL_EVENT      36
#define TRB_HOST_CONTROLLER     37
#define TRB_DEVICE_NOTIFY       38
#define TRB_MFINDEX_WRAP        39

// TRB flags (control field)
#define TRB_CYCLE               (1 << 0)    // Cycle bit
#define TRB_ENT                 (1 << 1)    // Evaluate Next TRB
#define TRB_ISP                 (1 << 2)    // Interrupt on Short Packet
#define TRB_NS                  (1 << 3)    // No Snoop
#define TRB_CH                  (1 << 4)    // Chain bit
#define TRB_IOC                 (1 << 5)    // Interrupt on Completion
#define TRB_IDT                 (1 << 6)    // Immediate Data
#define TRB_BEI                 (1 << 9)    // Block Event Interrupt
#define TRB_DIR_IN              (1 << 16)   // Data direction IN (for Data TRB)

// Completion codes
#define CC_SUCCESS              1
#define CC_DATA_BUFFER_ERROR    2
#define CC_BABBLE_DETECTED      3
#define CC_USB_TRANSACTION_ERROR 4
#define CC_TRB_ERROR            5
#define CC_STALL_ERROR          6
#define CC_RESOURCE_ERROR       7
#define CC_BANDWIDTH_ERROR      8
#define CC_NO_SLOTS_AVAILABLE   9
#define CC_INVALID_STREAM_TYPE  10
#define CC_SLOT_NOT_ENABLED     11
#define CC_EP_NOT_ENABLED       12
#define CC_SHORT_PACKET         13
#define CC_RING_UNDERRUN        14
#define CC_RING_OVERRUN         15
#define CC_VF_EVENT_RING_FULL   16
#define CC_PARAMETER_ERROR      17
#define CC_BANDWIDTH_OVERRUN    18
#define CC_CONTEXT_STATE_ERROR  19
#define CC_NO_PING_RESPONSE     20
#define CC_EVENT_RING_FULL      21
#define CC_INCOMPATIBLE_DEVICE  22
#define CC_MISSED_SERVICE       23
#define CC_COMMAND_RING_STOPPED 24
#define CC_COMMAND_ABORTED      25
#define CC_STOPPED              26
#define CC_STOPPED_LENGTH_INVALID 27
#define CC_MAX_EXIT_LATENCY_ERROR 29
#define CC_ISOCH_BUFFER_OVERRUN 31
#define CC_EVENT_LOST           32
#define CC_UNDEFINED            33
#define CC_INVALID_STREAM_ID    34
#define CC_SECONDARY_BANDWIDTH  35
#define CC_SPLIT_TRANSACTION    36

// =============================================================================
// Data Structures
// =============================================================================

// Transfer Request Block (TRB) - 16 bytes
typedef struct {
    uint64_t parameter;     // Parameter (address or immediate data)
    uint32_t status;        // Status field
    uint32_t control;       // Control field (type, flags)
} __attribute__((packed, aligned(16))) xhci_trb_t;

// Event Ring Segment Table Entry
typedef struct {
    uint64_t ring_base;     // Ring segment base address (64-byte aligned)
    uint32_t ring_size;     // Number of TRBs in segment
    uint32_t reserved;
} __attribute__((packed, aligned(16))) xhci_erst_entry_t;

// Slot Context (32 or 64 bytes depending on CSZ)
typedef struct {
    uint32_t route_string   : 20;   // Route String
    uint32_t speed          : 4;    // Speed
    uint32_t reserved1      : 1;
    uint32_t mtt            : 1;    // Multi-TT
    uint32_t hub            : 1;    // Hub
    uint32_t context_entries : 5;   // Context Entries

    uint32_t max_exit_latency : 16; // Max Exit Latency
    uint32_t root_hub_port  : 8;    // Root Hub Port Number
    uint32_t num_ports      : 8;    // Number of Ports (for hubs)

    uint32_t tt_hub_slot    : 8;    // TT Hub Slot ID
    uint32_t tt_port_num    : 8;    // TT Port Number
    uint32_t ttt            : 2;    // TT Think Time
    uint32_t reserved2      : 4;
    uint32_t interrupter    : 10;   // Interrupter Target

    uint32_t device_address : 8;    // USB Device Address
    uint32_t reserved3      : 19;
    uint32_t slot_state     : 5;    // Slot State

    uint32_t reserved4[4];          // Reserved
} __attribute__((packed)) xhci_slot_ctx_t;

// Endpoint Context (32 or 64 bytes)
typedef struct {
    uint32_t ep_state       : 3;    // Endpoint State
    uint32_t reserved1      : 5;
    uint32_t mult           : 2;    // Mult
    uint32_t max_pstreams   : 5;    // Max Primary Streams
    uint32_t lsa            : 1;    // Linear Stream Array
    uint32_t interval       : 8;    // Interval
    uint32_t max_esit_hi    : 8;    // Max ESIT Payload Hi

    uint32_t reserved2      : 1;
    uint32_t cerr           : 2;    // Error Count
    uint32_t ep_type        : 3;    // Endpoint Type
    uint32_t reserved3      : 1;
    uint32_t hid            : 1;    // Host Initiate Disable
    uint32_t max_burst      : 8;    // Max Burst Size
    uint32_t max_packet     : 16;   // Max Packet Size

    uint64_t tr_dequeue;            // TR Dequeue Pointer

    uint32_t avg_trb_len    : 16;   // Average TRB Length
    uint32_t max_esit_lo    : 16;   // Max ESIT Payload Lo

    uint32_t reserved4[3];
} __attribute__((packed)) xhci_ep_ctx_t;

// Endpoint types
#define EP_TYPE_NOT_VALID       0
#define EP_TYPE_ISOCH_OUT       1
#define EP_TYPE_BULK_OUT        2
#define EP_TYPE_INTERRUPT_OUT   3
#define EP_TYPE_CONTROL         4
#define EP_TYPE_ISOCH_IN        5
#define EP_TYPE_BULK_IN         6
#define EP_TYPE_INTERRUPT_IN    7

// Input Control Context
typedef struct {
    uint32_t drop_flags;            // Drop Context Flags
    uint32_t add_flags;             // Add Context Flags
    uint32_t reserved[5];
    uint32_t config_value   : 8;    // Configuration Value
    uint32_t interface_num  : 8;    // Interface Number
    uint32_t alternate      : 8;    // Alternate Setting
    uint32_t reserved2      : 8;
} __attribute__((packed)) xhci_input_ctrl_ctx_t;

// Device Context (Slot + 31 Endpoint contexts)
// Size depends on CSZ: 32*32=1024 bytes or 32*64=2048 bytes
typedef struct {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t endpoints[31];
} __attribute__((packed)) xhci_device_ctx_t;

// Input Context (Input Control + Slot + 31 Endpoint contexts)
typedef struct {
    xhci_input_ctrl_ctx_t ctrl;
    uint32_t reserved[7];           // Padding to 32/64 bytes
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t endpoints[31];
} __attribute__((packed)) xhci_input_ctx_t;

// =============================================================================
// Ring Buffer
// =============================================================================

#define XHCI_RING_SIZE          256     // Number of TRBs per ring
#define XHCI_MAX_SLOTS          256     // Maximum device slots
#define XHCI_MAX_ENDPOINTS      32      // Maximum endpoints per device

typedef struct {
    xhci_trb_t *trbs;               // TRB array (physically contiguous)
    uint64_t phys_addr;             // Physical address of TRB array
    uint32_t enqueue_idx;           // Enqueue index
    uint32_t dequeue_idx;           // Dequeue index
    uint32_t cycle_bit;             // Current cycle state
    uint32_t size;                  // Number of TRBs
} xhci_ring_t;

// =============================================================================
// xHCI Controller State
// =============================================================================

typedef struct {
    // PCI device
    pci_device_t *pci;

    // Memory-mapped registers
    volatile uint8_t *mmio_base;    // Base address (capability regs)
    volatile uint8_t *op_regs;      // Operational registers
    volatile uint8_t *rt_regs;      // Runtime registers
    volatile uint32_t *doorbells;   // Doorbell registers

    // Controller capabilities
    uint32_t max_slots;             // Maximum device slots
    uint32_t max_ports;             // Maximum root hub ports
    uint32_t max_interrupters;      // Maximum interrupters
    uint32_t context_size;          // Context size (32 or 64 bytes)
    int has_64bit;                  // 64-bit address capability

    // Rings
    xhci_ring_t cmd_ring;           // Command ring
    xhci_ring_t event_ring;         // Event ring

    // Event Ring Segment Table
    xhci_erst_entry_t *erst;        // ERST
    uint64_t erst_phys;             // ERST physical address

    // Device Context Base Address Array
    uint64_t *dcbaa;                // DCBAA
    uint64_t dcbaa_phys;            // DCBAA physical address

    // #307 real-HW: scratchpad buffers. Intel xHCI (Lynx Point / iMac14,4)
    // reports HCSPARAMS2 Max Scratchpad Buffers > 0 and REQUIRES the driver to
    // allocate that many controller-page-sized buffers and place a pointer to
    // the buffer array in DCBAA[0] before Run; otherwise the controller has no
    // private DMA scratch and the very first command (Enable Slot) never
    // completes. QEMU reports 0 here so this stays empty on the emulated path.
    uint64_t *scratchpad_array;     // scratchpad buffer array (virt==phys)
    uint64_t scratchpad_array_phys; // physical address stored in DCBAA[0]
    uint32_t num_scratchpad_bufs;   // count from HCSPARAMS2 (0 = none needed)

    // Device contexts (output)
    xhci_device_ctx_t *dev_ctx[XHCI_MAX_SLOTS];
    uint64_t dev_ctx_phys[XHCI_MAX_SLOTS];

    // Transfer rings for each device endpoint
    xhci_ring_t *transfer_rings[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];

    // Status
    int initialized;
    uint32_t enabled_slots;         // Number of enabled slots
} xhci_controller_t;

// =============================================================================
// USB Device Structure (xHCI-specific)
// =============================================================================

typedef struct {
    int slot_id;                    // xHCI slot ID (1-255)
    int port;                       // Root hub port
    int speed;                      // USB speed
    int address;                    // USB device address

    // Device descriptor
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    uint8_t num_configurations;

    // Configuration
    uint8_t current_config;

    // Controller reference
    xhci_controller_t *controller;
} xhci_device_t;

// #325 Device Manager: lightweight record of devices observed during
// xhci_enumerate_devices, so SYS_DEV_USB_LIST can report them (including
// the passed-through NuForce DAC). Read-only after enumeration.
typedef struct {
    int slot_id;
    int port;
    int speed;
    int address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t num_interfaces;
    // #388 DEVLOG: full descriptor capture for the /DEVLOG.TXT device inventory.
    // Filled at enumerate time from the descriptors actually read off the wire,
    // so the log reflects exactly what each device reported (no re-issued
    // transfers). cfg holds the raw configuration descriptor bytes (device +
    // interface + endpoint descriptors, concatenated) up to cfg_len.
    uint8_t  dev_desc[18];    // raw 18-byte DEVICE descriptor
    uint8_t  cfg[256];        // raw CONFIGURATION descriptor (capped)
    uint16_t cfg_len;         // valid bytes in cfg
    uint32_t route;           // xHCI route string (0 for a root-port device)
    uint8_t  depth;           // hub tier (0 = root port)
    uint8_t  is_hub;          // 1 if bDeviceClass/interface class == 0x09
    char     label[24];       // enumeration label, e.g. "P3" or "P3.2"
} xhci_enum_dev_t;
int xhci_get_enum_count(void);
const xhci_enum_dev_t *xhci_get_enum_device(int index);

// #388 DEVLOG: hub inventory captured during xhci_enumerate_hub. One record per
// hub, with the decoded hub descriptor and per-downstream-port status - so the
// log shows whether a device (e.g. the iMac keyboard) sits behind a hub and on
// which downstream port, even if it never bound.
typedef struct {
    int      hub_slot;
    int      root_port;       // 0-based root port the whole tree hangs off
    uint32_t route;           // route string of the hub itself
    int      depth;           // hub tier
    int      nports;          // bNbrPorts
    uint16_t hubchar;         // wHubCharacteristics
    struct {
        uint16_t status;      // wPortStatus
        uint16_t change;      // wPortChange
        uint8_t  connected;
        uint8_t  speed;       // XHCI_SPEED_* of the downstream device
        uint8_t  valid;       // 1 if this port entry was populated
    } ports[15];
} xhci_hub_rec_t;
int xhci_get_hub_count(void);
const xhci_hub_rec_t *xhci_get_hub_record(int index);

// #388 DEVLOG: read a root-hub port's live status for the inventory.
// Returns 1 if the port index is valid. *connected/*enabled/*speed set from
// PORTSC (speed is the XHCI_SPEED_* port-speed field).
int xhci_root_port_info(xhci_controller_t *xhc, int port,
                        int *connected, int *enabled, int *speed);

// =============================================================================
// Function Prototypes
// =============================================================================

// Controller initialization
int xhci_init(pci_device_t *pci);
int xhci_reset(xhci_controller_t *xhc);
int xhci_start(xhci_controller_t *xhc);
void xhci_stop(xhci_controller_t *xhc);

// Port operations
int xhci_port_reset(xhci_controller_t *xhc, int port);
int xhci_port_get_speed(xhci_controller_t *xhc, int port);
int xhci_port_is_connected(xhci_controller_t *xhc, int port);

// Device operations
int xhci_enable_slot(xhci_controller_t *xhc);
int xhci_disable_slot(xhci_controller_t *xhc, int slot_id);
int xhci_address_device(xhci_controller_t *xhc, int slot_id, int port, int speed);
int xhci_configure_endpoint(xhci_controller_t *xhc, int slot_id);

// Transfer operations
int xhci_control_transfer(xhci_controller_t *xhc, int slot_id,
                          uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length);
int xhci_bulk_transfer(xhci_controller_t *xhc, int slot_id, int endpoint,
                       void *data, uint32_t length, int direction);
int xhci_interrupt_transfer(xhci_controller_t *xhc, int slot_id, int endpoint,
                            void *data, uint32_t length);

// Ring operations
int xhci_ring_init(xhci_ring_t *ring, uint32_t size);
void xhci_ring_free(xhci_ring_t *ring);
xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring);
void xhci_ring_doorbell(xhci_controller_t *xhc, uint32_t slot, uint32_t target);

// Event handling
int xhci_wait_for_event(xhci_controller_t *xhc, uint32_t type, uint32_t timeout_ms);
int xhci_process_event(xhci_controller_t *xhc, xhci_trb_t *event);
void xhci_poll_events(xhci_controller_t *xhc);

// #323: continuous iso streaming support. xhci_iso_xfer_events counts
// transfer-event TRBs (one per IOC-flagged iso TD) so a streaming worker can
// do drift-free flow control. Set xhci_iso_quiet to suppress per-event serial
// spam while a continuous stream is running.
extern volatile uint32_t xhci_iso_xfer_events;
extern volatile int xhci_iso_quiet;

// Enumeration
int xhci_enumerate_devices(xhci_controller_t *xhc);
// #433: re-scan connected-but-not-enumerated ports (hotplug + boot-race retry)
// and start the periodic re-scan worker thread.
int xhci_rescan_ports(xhci_controller_t *xhc);
void xhci_start_rescan_thread(void);
int xhci_get_device_descriptor(xhci_controller_t *xhc, int slot_id, void *buf, int len);
int xhci_set_address(xhci_controller_t *xhc, int slot_id, uint8_t address);
int xhci_set_configuration(xhci_controller_t *xhc, int slot_id, uint8_t config);

// Evaluate Context: correct EP0 max packet size after reading the descriptor.
int xhci_evaluate_ep0_mps(xhci_controller_t *xhc, int slot_id, int max_packet);

// #307: generic bulk/interrupt endpoint configuration (HID + Mass Storage).
// ep_type is one of EP_TYPE_BULK_IN/OUT or EP_TYPE_INTERRUPT_IN/OUT. Returns
// the DCI on success, -1 on failure.
int xhci_configure_endpoint_ep(xhci_controller_t *xhc, int slot_id,
                               int ep_addr, int ep_type, int max_packet,
                               int b_interval, int speed);

// #307: non-blocking interrupt-IN transfer model used by USB HID polling.
int xhci_int_in_submit(xhci_controller_t *xhc, int slot_id, int dci,
                       uint64_t buf_phys, uint32_t len);
int xhci_int_in_poll(xhci_controller_t *xhc, int slot_id, int dci,
                     uint32_t *out_len, uint32_t submitted_len);

// Isochronous OUT endpoint support (USB Audio). configure returns the DCI.
int xhci_configure_iso_out(xhci_controller_t *xhc, int slot_id,
                           int ep_addr, int max_packet, int interval);
int xhci_iso_submit(xhci_controller_t *xhc, int slot_id, int dci,
                    uint64_t buf_phys, uint32_t total_bytes, uint32_t pkt_bytes);

// Utility functions
const char *xhci_speed_name(int speed);
const char *xhci_completion_code_name(int code);
void xhci_dump_port_status(xhci_controller_t *xhc, int port);
void xhci_dump_controller_info(xhci_controller_t *xhc);

// #362: PIT-calibrated wall-clock delay (wrapper around the static
// xhci_delay) for use by USB class/vendor drivers.
void xhci_delay_ms(uint32_t ms);

// Global controller access
extern xhci_controller_t *xhci_get_controller(int index);
extern int xhci_get_controller_count(void);

#endif // XHCI_H
