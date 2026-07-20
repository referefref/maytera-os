// rtl8139.h - Realtek RTL8139 Fast Ethernet Driver
// Task #54: Additional NIC driver support
#ifndef RTL8139_H
#define RTL8139_H

#include "../types.h"

// RTL8139 PCI IDs
#define RTL8139_VENDOR_ID       0x10EC
#define RTL8139_DEVICE_ID       0x8139
#define RTL8139_DEVICE_ID_ALT   0x8138  // Some variants

// I/O Register Offsets
#define RTL8139_IDR0            0x00    // MAC Address (6 bytes)
#define RTL8139_MAR0            0x08    // Multicast filter (8 bytes)
#define RTL8139_TSD0            0x10    // TX Status (4 descriptors, 4 bytes each)
#define RTL8139_TSAD0           0x20    // TX Start Address (4 descriptors, 4 bytes each)
#define RTL8139_RBSTART         0x30    // RX Buffer Start Address
#define RTL8139_ERBCR           0x34    // Early RX Byte Count
#define RTL8139_ERSR            0x36    // Early RX Status
#define RTL8139_CR              0x37    // Command Register
#define RTL8139_CAPR            0x38    // Current Address of Packet Read
#define RTL8139_CBR             0x3A    // Current Buffer Address
#define RTL8139_IMR             0x3C    // Interrupt Mask Register
#define RTL8139_ISR             0x3E    // Interrupt Status Register
#define RTL8139_TCR             0x40    // TX Configuration Register
#define RTL8139_RCR             0x44    // RX Configuration Register
#define RTL8139_TCTR            0x48    // Timer Count Register
#define RTL8139_MPC             0x4C    // Missed Packet Counter
#define RTL8139_9346CR          0x50    // 93C46 Command Register
#define RTL8139_CONFIG0         0x51    // Configuration Register 0
#define RTL8139_CONFIG1         0x52    // Configuration Register 1
#define RTL8139_TIMER_INT       0x54    // Timer Interrupt Register
#define RTL8139_MSR             0x58    // Media Status Register
#define RTL8139_CONFIG3         0x59    // Configuration Register 3
#define RTL8139_CONFIG4         0x5A    // Configuration Register 4
#define RTL8139_MULINT          0x5C    // Multiple Interrupt Select
#define RTL8139_RERID           0x5E    // Revision ID
#define RTL8139_TSAD            0x60    // TX Status of All Descriptors
#define RTL8139_BMCR            0x62    // Basic Mode Control Register
#define RTL8139_BMSR            0x64    // Basic Mode Status Register
#define RTL8139_ANAR            0x66    // Auto-Negotiation Advertisement Register
#define RTL8139_ANLPAR          0x68    // Auto-Negotiation Link Partner Register
#define RTL8139_ANER            0x6A    // Auto-Negotiation Expansion Register
#define RTL8139_DIS             0x6C    // Disconnect Counter
#define RTL8139_FCSC            0x6E    // False Carrier Sense Counter
#define RTL8139_NWAYTR          0x70    // N-Way Test Register
#define RTL8139_REC             0x72    // RX Error Counter
#define RTL8139_CSCR            0x74    // CS Configuration Register
#define RTL8139_PHY1_PARM       0x78    // PHY Parameter 1
#define RTL8139_TW_PARM         0x7C    // Twister Parameter
#define RTL8139_PHY2_PARM       0x80    // PHY Parameter 2

// Command Register (CR) bits
#define RTL8139_CR_RST          0x10    // Reset
#define RTL8139_CR_RE           0x08    // Receiver Enable
#define RTL8139_CR_TE           0x04    // Transmitter Enable
#define RTL8139_CR_BUFE         0x01    // Buffer Empty

// Interrupt Status/Mask bits
#define RTL8139_INT_SERR        0x8000  // System Error
#define RTL8139_INT_TIMEOUT     0x4000  // Timeout
#define RTL8139_INT_LENCHG      0x2000  // Cable Length Change
#define RTL8139_INT_FOVW        0x0040  // RX FIFO Overflow
#define RTL8139_INT_PUNLC       0x0020  // Packet Underrun/Link Change
#define RTL8139_INT_RXOVW       0x0010  // RX Buffer Overflow
#define RTL8139_INT_TER         0x0008  // TX Error
#define RTL8139_INT_TOK         0x0004  // TX OK
#define RTL8139_INT_RER         0x0002  // RX Error
#define RTL8139_INT_ROK         0x0001  // RX OK

// TX Configuration Register (TCR) bits
#define RTL8139_TCR_IFG         0x03000000  // Interframe Gap
#define RTL8139_TCR_HWVERID     0x7CC00000  // Hardware Version ID
#define RTL8139_TCR_MXDMA_2048  0x00000700  // Max DMA burst 2048 bytes
#define RTL8139_TCR_MXDMA_1024  0x00000600  // Max DMA burst 1024 bytes
#define RTL8139_TCR_MXDMA_512   0x00000500  // Max DMA burst 512 bytes
#define RTL8139_TCR_MXDMA_256   0x00000400  // Max DMA burst 256 bytes
#define RTL8139_TCR_LBK         0x00060000  // Loopback test
#define RTL8139_TCR_CRC         0x00010000  // Append CRC (normally set)

// RX Configuration Register (RCR) bits
#define RTL8139_RCR_ERTH        0x0F000000  // Early RX Threshold
#define RTL8139_RCR_MULERINT    0x00020000  // Multiple Early Interrupt
#define RTL8139_RCR_RER8        0x00010000  // RX Error 8
#define RTL8139_RCR_RXFTH       0x0000E000  // RX FIFO Threshold
#define RTL8139_RCR_RBLEN       0x00001800  // RX Buffer Length
#define RTL8139_RCR_MXDMA_2048  0x00000700  // Max DMA burst 2048 bytes
#define RTL8139_RCR_WRAP        0x00000080  // Wrap at end of buffer
#define RTL8139_RCR_EEPROMSEL   0x00000040  // EEPROM Type (0=9346, 1=9356)
#define RTL8139_RCR_AER         0x00000020  // Accept Error Packets
#define RTL8139_RCR_AR          0x00000010  // Accept Runt Packets
#define RTL8139_RCR_AB          0x00000008  // Accept Broadcast
#define RTL8139_RCR_AM          0x00000004  // Accept Multicast
#define RTL8139_RCR_APM         0x00000002  // Accept Physical Match
#define RTL8139_RCR_AAP         0x00000001  // Accept All Packets

// RX Buffer Length options (RBLEN field)
#define RTL8139_RBLEN_8K        (0 << 11)   // 8K + 16 bytes
#define RTL8139_RBLEN_16K       (1 << 11)   // 16K + 16 bytes
#define RTL8139_RBLEN_32K       (2 << 11)   // 32K + 16 bytes
#define RTL8139_RBLEN_64K       (3 << 11)   // 64K + 16 bytes

// TX Status register bits (TSD0-TSD3)
#define RTL8139_TSD_OWN         0x00002000  // Own (0=DMA complete)
#define RTL8139_TSD_TUN         0x00004000  // TX Underrun
#define RTL8139_TSD_TOK         0x00008000  // TX OK
#define RTL8139_TSD_OWC         0x20000000  // Out of Window Collision
#define RTL8139_TSD_TABT        0x40000000  // TX Abort
#define RTL8139_TSD_CRS         0x80000000  // Carrier Sense Lost

// Media Status Register (MSR) bits
#define RTL8139_MSR_RXPF        0x01    // RX Pause Flag
#define RTL8139_MSR_TXPF        0x02    // TX Pause Flag
#define RTL8139_MSR_LINKB       0x04    // Link Bad (0=good, 1=bad)
#define RTL8139_MSR_SPEED10     0x08    // Speed 10Mbps (0=100, 1=10)
#define RTL8139_MSR_RXFCE       0x40    // RX Flow Control Enable
#define RTL8139_MSR_TXFCE       0x80    // TX Flow Control Enable

// RX Packet header (4 bytes before each packet in buffer)
typedef struct {
    uint16_t status;
    uint16_t length;    // Including 4-byte CRC
} __attribute__((packed)) rtl8139_rx_header_t;

// RX header status bits
#define RTL8139_RXS_ROK         0x0001  // RX OK
#define RTL8139_RXS_FAE         0x0002  // Frame Alignment Error
#define RTL8139_RXS_CRC         0x0004  // CRC Error
#define RTL8139_RXS_LONG        0x0008  // Long Packet (> 4K)
#define RTL8139_RXS_RUNT        0x0010  // Runt Packet (< 64)
#define RTL8139_RXS_ISE         0x0020  // Invalid Symbol Error
#define RTL8139_RXS_BAR         0x2000  // Broadcast Address Received
#define RTL8139_RXS_PAM         0x4000  // Physical Address Matched
#define RTL8139_RXS_MAR         0x8000  // Multicast Address Received

// Buffer sizes
#define RTL8139_RX_BUF_SIZE     (32768 + 16 + 2048)  // 32K + header + wrap
#define RTL8139_TX_BUF_SIZE     2048
#define RTL8139_NUM_TX_DESC     4       // 4 TX descriptors

// ============================================================================
// Driver API
// ============================================================================

// Initialize RTL8139 driver
int rtl8139_init(void);

// Get MAC address
void rtl8139_get_mac(uint8_t *mac);

// Send a packet
int rtl8139_send(const void *data, uint16_t length);

// Receive a packet (returns length, 0 if none)
int rtl8139_receive(void *buffer, uint16_t buffer_size);

// Check link status
int rtl8139_link_up(void);

// IRQ handler
void rtl8139_irq_handler(void);

// Get device statistics
void rtl8139_get_stats(uint64_t *rx_packets, uint64_t *tx_packets,
                       uint64_t *rx_errors, uint64_t *tx_errors);

#endif // RTL8139_H
