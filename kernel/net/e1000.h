// e1000.h - Intel E1000 NIC driver
#ifndef E1000_H
#define E1000_H

#include "../types.h"

// E1000 Register offsets
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EECD      0x0010  // EEPROM Control
#define E1000_EERD      0x0014  // EEPROM Read
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_RCTL      0x0100  // Receive Control
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_RDBAL     0x2800  // RX Descriptor Base Low
#define E1000_RDBAH     0x2804  // RX Descriptor Base High
#define E1000_RDLEN     0x2808  // RX Descriptor Length
#define E1000_RDH       0x2810  // RX Descriptor Head
#define E1000_RDT       0x2818  // RX Descriptor Tail
#define E1000_TDBAL     0x3800  // TX Descriptor Base Low
#define E1000_TDBAH     0x3804  // TX Descriptor Base High
#define E1000_TDLEN     0x3808  // TX Descriptor Length
#define E1000_TDH       0x3810  // TX Descriptor Head
#define E1000_TDT       0x3818  // TX Descriptor Tail
#define E1000_MTA       0x5200  // Multicast Table Array
#define E1000_RAL       0x5400  // Receive Address Low
#define E1000_RAH       0x5404  // Receive Address High

// CTRL register bits
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up
#define E1000_CTRL_RST      (1 << 26)  // Device Reset

// RCTL register bits
#define E1000_RCTL_EN       (1 << 1)   // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)   // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promiscuous Enable
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promiscuous Enable
#define E1000_RCTL_LBM      (3 << 6)   // Loopback Mode
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE    (3 << 16)  // Buffer Size
#define E1000_RCTL_SECRC    (1 << 26)  // Strip Ethernet CRC

// TCTL register bits
#define E1000_TCTL_EN       (1 << 1)   // Transmitter Enable
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets
#define E1000_TCTL_CT       (0xF << 4) // Collision Threshold
#define E1000_TCTL_COLD     (0x3F << 12) // Collision Distance

// ICR/IMS/IMC bits (Interrupt)
#define E1000_INT_TXDW      (1 << 0)   // TX Descriptor Written Back
#define E1000_INT_TXQE      (1 << 1)   // TX Queue Empty
#define E1000_INT_LSC       (1 << 2)   // Link Status Change
#define E1000_INT_RXO       (1 << 6)   // RX Overrun
#define E1000_INT_RXT       (1 << 7)   // RX Timer Interrupt

// TX Descriptor Command bits
#define E1000_TXD_CMD_EOP   (1 << 0)   // End of Packet
#define E1000_TXD_CMD_IFCS  (1 << 1)   // Insert FCS
#define E1000_TXD_CMD_RS    (1 << 3)   // Report Status

// TX Descriptor Status bits
#define E1000_TXD_STAT_DD   (1 << 0)   // Descriptor Done

// RX Descriptor Status bits
#ifndef E1000_RXD_STAT_DD
#define E1000_RXD_STAT_DD   (1 << 0)
#endif   // Descriptor Done
#ifndef E1000_RXD_STAT_EOP
#define E1000_RXD_STAT_EOP  (1 << 1)
#endif   // End of Packet

// Descriptor counts (must be multiple of 8)
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32

// Buffer size
#define E1000_RX_BUFFER_SIZE 2048
#define E1000_TX_BUFFER_SIZE 2048

// RX Descriptor (legacy format)
typedef struct {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Length of received data
    uint16_t checksum;  // Packet checksum
    uint8_t  status;    // Status
    uint8_t  errors;    // Errors
    uint16_t special;   // Special
} __attribute__((packed)) e1000_rx_desc_t;

// TX Descriptor (legacy format)
typedef struct {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Data length
    uint8_t  cso;       // Checksum offset
    uint8_t  cmd;       // Command
    uint8_t  status;    // Status
    uint8_t  css;       // Checksum start
    uint16_t special;   // Special
} __attribute__((packed)) e1000_tx_desc_t;

// Initialize E1000 driver
int e1000_init(void);

// Get MAC address
void e1000_get_mac(uint8_t *mac);

// Send a packet
int e1000_send(const void *data, uint16_t length);

// Receive a packet (returns length, 0 if none available)
int e1000_receive(void *buffer, uint16_t buffer_size);

// Check link status
int e1000_link_up(void);

// Handle interrupt
void e1000_irq_handler(void);

#endif // E1000_H

// Crash context management - call these to prevent MMIO access during crashes
void e1000_enter_crash_context(void);
void e1000_exit_crash_context(void);

// Check if E1000 is safe to use
int e1000_is_safe(void);

