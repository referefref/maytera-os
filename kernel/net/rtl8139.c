// rtl8139.c - Realtek RTL8139 Fast Ethernet Driver Implementation
// Task #54: Additional NIC driver support
#include "rtl8139.h"
#include "../drivers/pci.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"

// External I/O functions
extern uint8_t inb(uint16_t port);
extern uint16_t inw(uint16_t port);
extern uint32_t inl(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void outw(uint16_t port, uint16_t val);
extern void outl(uint16_t port, uint32_t val);
extern void io_wait(void);

// External PMM
extern uint64_t pmm_alloc_page(void);

// Device state
static uint16_t rtl8139_io_base = 0;
static uint8_t rtl8139_mac[6];
static int rtl8139_initialized = 0;

// RX buffer (32K + 16 header + 2K wrap padding)
static uint8_t *rx_buffer = NULL;
static uint64_t rx_buffer_phys = 0;
static uint32_t rx_offset = 0;

// TX buffers (4 descriptors)
static uint8_t *tx_buffers[RTL8139_NUM_TX_DESC];
static uint64_t tx_buffers_phys[RTL8139_NUM_TX_DESC];
static int tx_cur = 0;

// Statistics
static uint64_t rx_packets = 0;
static uint64_t tx_packets = 0;
static uint64_t rx_errors = 0;
static uint64_t tx_errors = 0;

// ============================================================================
// I/O Helpers
// ============================================================================

static inline uint8_t rtl8139_read8(uint16_t reg) {
    return inb(rtl8139_io_base + reg);
}

static inline uint16_t rtl8139_read16(uint16_t reg) {
    return inw(rtl8139_io_base + reg);
}

static inline uint32_t rtl8139_read32(uint16_t reg) {
    return inl(rtl8139_io_base + reg);
}

static inline void rtl8139_write8(uint16_t reg, uint8_t val) {
    outb(rtl8139_io_base + reg, val);
}

static inline void rtl8139_write16(uint16_t reg, uint16_t val) {
    outw(rtl8139_io_base + reg, val);
}

static inline void rtl8139_write32(uint16_t reg, uint32_t val) {
    outl(rtl8139_io_base + reg, val);
}

// ============================================================================
// Initialization
// ============================================================================

int rtl8139_init(void) {
    kprintf("[RTL8139] Initializing RTL8139 driver...\n");
    
    // Find RTL8139 device
    pci_device_t *dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!dev) {
        dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID_ALT);
    }
    if (!dev) {
        kprintf("[RTL8139] No RTL8139 device found\n");
        return -1;
    }
    
    kprintf("[RTL8139] Found RTL8139 at %02x:%02x.%x\n",
            dev->bus, dev->slot, dev->func);
    
    // Enable bus mastering and I/O space
    pci_enable_bus_master(dev);
    
    // Get I/O base address (BAR0)
    uint64_t io_addr = pci_get_bar_address(dev, 0);
    if (io_addr == 0 || io_addr > 0xFFFF) {
        kprintf("[RTL8139] Invalid I/O base address\n");
        return -1;
    }
    rtl8139_io_base = (uint16_t)io_addr;
    kprintf("[RTL8139] I/O base: 0x%04x\n", rtl8139_io_base);
    
    // Power on the device (wake from sleep)
    rtl8139_write8(RTL8139_CONFIG1, 0x00);
    
    // Software reset
    rtl8139_write8(RTL8139_CR, RTL8139_CR_RST);
    
    // Wait for reset to complete
    int timeout = 1000;
    while ((rtl8139_read8(RTL8139_CR) & RTL8139_CR_RST) && timeout > 0) {
        timeout--;
        io_wait();
    }
    
    if (timeout == 0) {
        kprintf("[RTL8139] Reset timeout\n");
        return -1;
    }
    
    // Read MAC address from EEPROM (already loaded to IDR on power-up)
    uint32_t mac_low = rtl8139_read32(RTL8139_IDR0);
    uint16_t mac_high = rtl8139_read16(RTL8139_IDR0 + 4);
    rtl8139_mac[0] = mac_low & 0xFF;
    rtl8139_mac[1] = (mac_low >> 8) & 0xFF;
    rtl8139_mac[2] = (mac_low >> 16) & 0xFF;
    rtl8139_mac[3] = (mac_low >> 24) & 0xFF;
    rtl8139_mac[4] = mac_high & 0xFF;
    rtl8139_mac[5] = (mac_high >> 8) & 0xFF;
    
    kprintf("[RTL8139] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl8139_mac[0], rtl8139_mac[1], rtl8139_mac[2],
            rtl8139_mac[3], rtl8139_mac[4], rtl8139_mac[5]);
    
    // Allocate RX buffer (need ~35KB contiguous physical memory)
    // We'll use multiple pages
    int rx_pages = (RTL8139_RX_BUF_SIZE + 4095) / 4096;
    uint64_t first_page = pmm_alloc_page();
    if (first_page == 0) {
        kprintf("[RTL8139] Failed to allocate RX buffer\n");
        return -1;
    }
    rx_buffer_phys = first_page;
    rx_buffer = (uint8_t *)first_page;
    
    // Allocate remaining pages (assuming contiguous)
    for (int i = 1; i < rx_pages; i++) {
        uint64_t page = pmm_alloc_page();
        if (page == 0 || page != first_page + i * 4096) {
            kprintf("[RTL8139] Warning: RX buffer may not be contiguous\n");
            // Continue anyway - may work with smaller buffer
        }
    }
    memset(rx_buffer, 0, RTL8139_RX_BUF_SIZE);
    kprintf("[RTL8139] RX buffer at phys 0x%lx\n", rx_buffer_phys);
    
    // Allocate TX buffers (4 x 2KB)
    for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        tx_buffers_phys[i] = pmm_alloc_page();
        if (tx_buffers_phys[i] == 0) {
            kprintf("[RTL8139] Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        tx_buffers[i] = (uint8_t *)tx_buffers_phys[i];
        memset(tx_buffers[i], 0, RTL8139_TX_BUF_SIZE);
    }
    kprintf("[RTL8139] TX buffers allocated\n");
    
    // Set RX buffer address
    rtl8139_write32(RTL8139_RBSTART, (uint32_t)rx_buffer_phys);
    
    // Set TX buffer addresses
    for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        rtl8139_write32(RTL8139_TSAD0 + i * 4, (uint32_t)tx_buffers_phys[i]);
    }
    
    // Configure interrupt mask (enable all useful interrupts)
    rtl8139_write16(RTL8139_IMR, 
                    RTL8139_INT_ROK |      // RX OK
                    RTL8139_INT_RER |      // RX Error
                    RTL8139_INT_TOK |      // TX OK
                    RTL8139_INT_TER |      // TX Error
                    RTL8139_INT_RXOVW |    // RX Overflow
                    RTL8139_INT_PUNLC |    // Link Change
                    RTL8139_INT_FOVW);     // FIFO Overflow
    
    // Configure RX:
    // - Accept broadcast, multicast, physical match
    // - 32K buffer
    // - Max DMA burst 2048 bytes
    // - Wrap mode
    uint32_t rcr = RTL8139_RCR_AB |           // Accept Broadcast
                   RTL8139_RCR_AM |           // Accept Multicast
                   RTL8139_RCR_APM |          // Accept Physical Match
                   RTL8139_RBLEN_32K |        // 32K buffer
                   RTL8139_RCR_MXDMA_2048 |   // Max DMA 2048
                   RTL8139_RCR_WRAP;          // Wrap at end
    rtl8139_write32(RTL8139_RCR, rcr);
    
    // Configure TX:
    // - Interframe gap time
    // - Max DMA burst 2048 bytes
    // - Append CRC
    uint32_t tcr = RTL8139_TCR_MXDMA_2048 |   // Max DMA 2048
                   RTL8139_TCR_IFG |          // Default IFG
                   RTL8139_TCR_CRC;           // Append CRC
    rtl8139_write32(RTL8139_TCR, tcr);
    
    // Reset packet read pointer
    rx_offset = 0;
    rtl8139_write16(RTL8139_CAPR, 0);
    
    // Enable receiver and transmitter
    rtl8139_write8(RTL8139_CR, RTL8139_CR_RE | RTL8139_CR_TE);
    
    rtl8139_initialized = 1;
    
    kprintf("[RTL8139] Driver initialized, link %s\n",
            rtl8139_link_up() ? "up" : "down");
    
    return 0;
}

void rtl8139_get_mac(uint8_t *mac) {
    if (mac) {
        memcpy(mac, rtl8139_mac, 6);
    }
}

// ============================================================================
// Transmit
// ============================================================================

int rtl8139_send(const void *data, uint16_t length) {
    if (!rtl8139_initialized || !data || length == 0) {
        return -1;
    }
    
    if (length > RTL8139_TX_BUF_SIZE) {
        kprintf("[RTL8139] Packet too large: %d\n", length);
        return -1;
    }
    
    // Wait for current TX descriptor to be available
    int timeout = 10000;
    while (!(rtl8139_read32(RTL8139_TSD0 + tx_cur * 4) & RTL8139_TSD_OWN) &&
           timeout > 0) {
        // Check for TX completion or timeout
        uint32_t status = rtl8139_read32(RTL8139_TSD0 + tx_cur * 4);
        if (status & (RTL8139_TSD_TOK | RTL8139_TSD_TUN | RTL8139_TSD_TABT)) {
            break;
        }
        timeout--;
        io_wait();
    }
    
    if (timeout == 0) {
        kprintf("[RTL8139] TX timeout\n");
        tx_errors++;
        return -1;
    }
    
    // Copy packet to TX buffer
    memcpy(tx_buffers[tx_cur], data, length);
    __asm__ volatile("mfence" ::: "memory");
    
    // Start transmission
    // Lower 13 bits = size, bit 13 = OWN (set to 0 to start TX)
    // Upper bits contain status after completion
    rtl8139_write32(RTL8139_TSD0 + tx_cur * 4, length & 0x1FFF);
    
    // Advance to next descriptor
    tx_cur = (tx_cur + 1) % RTL8139_NUM_TX_DESC;
    tx_packets++;
    
    return length;
}

// ============================================================================
// Receive
// ============================================================================

int rtl8139_receive(void *buffer, uint16_t buffer_size) {
    if (!rtl8139_initialized || !buffer) {
        return 0;
    }
    
    // Check if buffer is empty
    uint8_t cmd = rtl8139_read8(RTL8139_CR);
    if (cmd & RTL8139_CR_BUFE) {
        return 0;  // Buffer empty
    }
    
    // Read packet header at current offset
    rtl8139_rx_header_t *header = (rtl8139_rx_header_t *)(rx_buffer + rx_offset);
    
    uint16_t status = header->status;
    uint16_t length = header->length;
    
    // Check for valid packet
    if (!(status & RTL8139_RXS_ROK)) {
        // Error packet
        rx_errors++;
        
        // Skip this packet
        length = (length + 4 + 3) & ~3;  // Align to 4 bytes
        rx_offset = (rx_offset + length) % (32768 + 16);
        rtl8139_write16(RTL8139_CAPR, rx_offset - 16);
        
        return 0;
    }
    
    // Validate length (includes 4-byte CRC)
    if (length < 4 || length > 1518 + 4) {
        rx_errors++;
        
        // Skip packet
        length = (length + 4 + 3) & ~3;
        rx_offset = (rx_offset + length) % (32768 + 16);
        rtl8139_write16(RTL8139_CAPR, rx_offset - 16);
        
        return 0;
    }
    
    // Copy packet data (excluding 4-byte header, including CRC which we strip)
    uint16_t pkt_len = length - 4;  // Remove CRC
    if (pkt_len > buffer_size) {
        pkt_len = buffer_size;
    }
    
    // Handle buffer wrap-around
    uint8_t *pkt_data = rx_buffer + rx_offset + 4;  // Skip header
    uint32_t avail = RTL8139_RX_BUF_SIZE - (rx_offset + 4);
    
    if (pkt_len <= avail) {
        memcpy(buffer, pkt_data, pkt_len);
    } else {
        // Packet wraps around buffer
        memcpy(buffer, pkt_data, avail);
        memcpy((uint8_t *)buffer + avail, rx_buffer, pkt_len - avail);
    }
    
    // Update read pointer (align to 4 bytes, add 4 for header)
    uint32_t new_offset = rx_offset + length + 4;  // header + data
    new_offset = (new_offset + 3) & ~3;            // Align to 4
    rx_offset = new_offset % (32768 + 16);
    
    // Update CAPR (Current Address of Packet Read)
    // CAPR = rx_offset - 16 to leave room for wrap check
    rtl8139_write16(RTL8139_CAPR, (rx_offset - 16) & 0xFFFF);
    
    rx_packets++;
    return pkt_len;
}

// ============================================================================
// Link Status
// ============================================================================

int rtl8139_link_up(void) {
    if (!rtl8139_initialized) return 0;
    
    // MSR bit 2: Link Bad (0 = link good, 1 = link bad)
    uint8_t msr = rtl8139_read8(RTL8139_MSR);
    return !(msr & RTL8139_MSR_LINKB);
}

// ============================================================================
// IRQ Handler
// ============================================================================

void rtl8139_irq_handler(void) {
    if (!rtl8139_initialized) return;
    
    // Read and clear interrupt status
    uint16_t isr = rtl8139_read16(RTL8139_ISR);
    rtl8139_write16(RTL8139_ISR, isr);  // Write to clear
    
    if (isr & RTL8139_INT_ROK) {
        // Packet received - handled by polling
    }
    
    if (isr & RTL8139_INT_TOK) {
        // TX complete
    }
    
    if (isr & RTL8139_INT_RER) {
        kprintf("[RTL8139] RX error\n");
        rx_errors++;
    }
    
    if (isr & RTL8139_INT_TER) {
        kprintf("[RTL8139] TX error\n");
        tx_errors++;
    }
    
    if (isr & RTL8139_INT_RXOVW) {
        kprintf("[RTL8139] RX buffer overflow\n");
        rx_errors++;
        
        // Reset receiver
        rtl8139_write8(RTL8139_CR, rtl8139_read8(RTL8139_CR) & ~RTL8139_CR_RE);
        rtl8139_write8(RTL8139_CR, rtl8139_read8(RTL8139_CR) | RTL8139_CR_RE);
        rx_offset = 0;
        rtl8139_write16(RTL8139_CAPR, 0);
    }
    
    if (isr & RTL8139_INT_PUNLC) {
        kprintf("[RTL8139] Link %s\n", rtl8139_link_up() ? "up" : "down");
    }
    
    if (isr & RTL8139_INT_FOVW) {
        kprintf("[RTL8139] FIFO overflow\n");
        rx_errors++;
    }
}

// ============================================================================
// Statistics
// ============================================================================

void rtl8139_get_stats(uint64_t *rx_pkts, uint64_t *tx_pkts,
                       uint64_t *rx_errs, uint64_t *tx_errs) {
    if (rx_pkts) *rx_pkts = rx_packets;
    if (tx_pkts) *tx_pkts = tx_packets;
    if (rx_errs) *rx_errs = rx_errors;
    if (tx_errs) *tx_errs = tx_errors;
}
