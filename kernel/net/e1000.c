// e1000.c - Intel E1000 NIC driver implementation
#include "e1000.h"
#include "../drivers/pci.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../serial.h"
#include "../string.h"

// E1000 device state
static volatile uint8_t *e1000_mmio = NULL;
static uint64_t e1000_mmio_phys = 0;
static uint64_t e1000_mmio_size = 0;
static uint8_t e1000_mac[6];
static int e1000_initialized = 0;

// Flag to prevent MMIO access during crash handling
static volatile int e1000_in_crash_context = 0;

// Descriptor rings
static e1000_rx_desc_t *rx_descs __attribute__((aligned(16)));
static e1000_tx_desc_t *tx_descs __attribute__((aligned(16)));

// Buffers
static uint8_t *rx_buffers[E1000_NUM_RX_DESC];
static uint8_t *tx_buffers[E1000_NUM_TX_DESC];

// Current descriptor indices
static uint32_t rx_cur = 0;
static uint32_t tx_cur = 0;

// Safe check before MMIO access
static inline int e1000_can_access_mmio(void) {
    return e1000_initialized && e1000_mmio != NULL && !e1000_in_crash_context;
}

// Read E1000 register (with safety check)
static uint32_t e1000_read(uint32_t reg) {
    if (!e1000_can_access_mmio()) {
        return 0xFFFFFFFF;  // Return invalid value if not safe to access
    }
    return *(volatile uint32_t *)(e1000_mmio + reg);
}

// Write E1000 register (with safety check)
static void e1000_write(uint32_t reg, uint32_t value) {
    if (!e1000_can_access_mmio()) {
        return;  // Skip write if not safe to access
    }
    *(volatile uint32_t *)(e1000_mmio + reg) = value;
}

// Read MAC address from EEPROM
static void e1000_read_mac_eeprom(void) {
    uint32_t temp;

    // Try reading from RAL/RAH first (might be set by firmware)
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);

    if (ral != 0 && ral != 0xFFFFFFFF && (rah & 0xFFFF) != 0) {
        e1000_mac[0] = ral & 0xFF;
        e1000_mac[1] = (ral >> 8) & 0xFF;
        e1000_mac[2] = (ral >> 16) & 0xFF;
        e1000_mac[3] = (ral >> 24) & 0xFF;
        e1000_mac[4] = rah & 0xFF;
        e1000_mac[5] = (rah >> 8) & 0xFF;
        return;
    }

    // Try EEPROM read
    for (int i = 0; i < 3; i++) {
        e1000_write(E1000_EERD, (1) | (i << 8));  // Read address i

        // Wait for read to complete
        int timeout = 10000;
        while (timeout > 0) {
            temp = e1000_read(E1000_EERD);
            if (temp == 0xFFFFFFFF || (temp & (1 << 4))) break;  // Error or Done bit
            timeout--;
        }

        if (timeout == 0 || temp == 0xFFFFFFFF) {
            // EEPROM read failed, use default MAC
            e1000_mac[0] = 0x52;
            e1000_mac[1] = 0x54;
            e1000_mac[2] = 0x00;
            e1000_mac[3] = 0x12;
            e1000_mac[4] = 0x34;
            e1000_mac[5] = 0x56;
            return;
        }

        uint16_t data = (temp >> 16) & 0xFFFF;
        e1000_mac[i * 2] = data & 0xFF;
        e1000_mac[i * 2 + 1] = (data >> 8) & 0xFF;
    }
}

// Physical addresses for DMA (must use pmm_alloc to get identity-mapped memory)
static uint64_t rx_descs_phys = 0;
static uint64_t tx_descs_phys = 0;
static uint64_t rx_buffers_phys[E1000_NUM_RX_DESC];
static uint64_t tx_buffers_phys[E1000_NUM_TX_DESC];

// Initialize receive descriptors using physical memory (identity-mapped via UEFI)
static void e1000_init_rx(void) {
    extern uint64_t pmm_alloc_page(void);

    // Allocate descriptor ring from physical memory (one page is enough for descriptors)
    rx_descs_phys = pmm_alloc_page();
    if (rx_descs_phys == 0) {
        kprintf("[E1000] Failed to allocate RX descriptors (physical)\n");
        return;
    }
    rx_descs = (e1000_rx_desc_t *)rx_descs_phys;  // Identity-mapped
    memset(rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    kprintf("[E1000] RX descriptors at phys 0x%lx\n", rx_descs_phys);

    // Allocate and set up buffers from physical memory
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_buffers_phys[i] = pmm_alloc_page();  // 4KB page for each buffer
        if (rx_buffers_phys[i] == 0) {
            kprintf("[E1000] Failed to allocate RX buffer %d (physical)\n", i);
            return;
        }
        rx_buffers[i] = (uint8_t *)rx_buffers_phys[i];  // Identity-mapped
        rx_descs[i].addr = rx_buffers_phys[i];  // Physical address for DMA
        rx_descs[i].status = 0;
    }

    // Set up descriptor ring with physical address
    e1000_write(E1000_RDBAL, rx_descs_phys & 0xFFFFFFFF);
    e1000_write(E1000_RDBAH, rx_descs_phys >> 32);
    e1000_write(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

    // Enable receiver
    uint32_t rctl = E1000_RCTL_EN |      // Enable
                    E1000_RCTL_BAM |      // Accept broadcast
                    E1000_RCTL_SECRC;     // Strip CRC
    e1000_write(E1000_RCTL, rctl);
}

// Initialize transmit descriptors using physical memory (identity-mapped via UEFI)
static void e1000_init_tx(void) {
    extern uint64_t pmm_alloc_page(void);

    // Allocate descriptor ring from physical memory (one page is enough)
    tx_descs_phys = pmm_alloc_page();
    if (tx_descs_phys == 0) {
        kprintf("[E1000] Failed to allocate TX descriptors (physical)\n");
        return;
    }
    tx_descs = (e1000_tx_desc_t *)tx_descs_phys;  // Identity-mapped
    memset(tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    kprintf("[E1000] TX descriptors at phys 0x%lx\n", tx_descs_phys);

    // Allocate buffers from physical memory
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_buffers_phys[i] = pmm_alloc_page();
        if (tx_buffers_phys[i] == 0) {
            kprintf("[E1000] Failed to allocate TX buffer %d (physical)\n", i);
            return;
        }
        tx_buffers[i] = (uint8_t *)tx_buffers_phys[i];  // Identity-mapped
        tx_descs[i].addr = tx_buffers_phys[i];  // Physical address for DMA
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TXD_STAT_DD;  // Mark as done initially
    }
    kprintf("[E1000] TX buffer 0 at phys 0x%lx\n", tx_buffers_phys[0]);

    // Set up descriptor ring with physical address
    e1000_write(E1000_TDBAL, tx_descs_phys & 0xFFFFFFFF);
    e1000_write(E1000_TDBAH, tx_descs_phys >> 32);
    e1000_write(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    // Enable transmitter
    uint32_t tctl = E1000_TCTL_EN |      // Enable
                    E1000_TCTL_PSP |      // Pad short packets
                    (15 << 4) |           // Collision threshold
                    (64 << 12);           // Collision distance
    e1000_write(E1000_TCTL, tctl);
}

// Map MMIO region into virtual address space
static int e1000_map_mmio(uint64_t phys_addr, uint64_t size) {
    // Calculate number of pages needed (E1000 MMIO is typically 128KB or 256KB)
    uint64_t num_pages = (size + VMM_PAGE_SIZE_4K - 1) / VMM_PAGE_SIZE_4K;
    
    kprintf("[E1000] Mapping MMIO: phys=0x%lx size=0x%lx pages=%lu\n", 
            phys_addr, size, num_pages);
    
    // Map each page with uncached attributes for MMIO
    // Use PCD (Page Cache Disable) flag for MMIO access
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_PCD;
    
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys_addr + (i * VMM_PAGE_SIZE_4K);
        uint64_t page_virt = page_phys;  // Use identity mapping for simplicity
        
        // Check if already mapped
        if (!vmm_is_mapped(page_virt)) {
            if (vmm_map_page(page_virt, page_phys, flags) != 0) {
                kprintf("[E1000] Failed to map MMIO page at 0x%lx\n", page_phys);
                return -1;
            }
        }
    }
    
    kprintf("[E1000] MMIO mapped successfully\n");
    return 0;
}

// Initialize E1000 driver
int e1000_init(void) {
    kprintf("[E1000] Initializing E1000 driver...\n");

    // Find E1000 device
    pci_device_t *dev = pci_find_device(PCI_VENDOR_INTEL, PCI_DEVICE_E1000);
    if (!dev) {
        dev = pci_find_device(PCI_VENDOR_INTEL, PCI_DEVICE_E1000_2);
    }
    if (!dev) {
        dev = pci_find_device(PCI_VENDOR_INTEL, PCI_DEVICE_E1000_3);
    }
    if (!dev) {
        // Try finding any Intel network controller
        dev = pci_find_class(0x02, 0x00);  // Network controller, Ethernet
        if (dev && dev->vendor_id != PCI_VENDOR_INTEL) {
            dev = NULL;
        }
    }

    if (!dev) {
        kprintf("[E1000] No E1000 device found\n");
        return -1;
    }

    kprintf("[E1000] Found E1000 at %02x:%02x.%x (Device ID: 0x%04x)\n",
            dev->bus, dev->slot, dev->func, dev->device_id);

    // Enable bus mastering
    pci_enable_bus_master(dev);

    // Get MMIO base address
    uint64_t mmio_addr = pci_get_bar_address(dev, 0);
    if (mmio_addr == 0) {
        kprintf("[E1000] Failed to get MMIO address\n");
        return -1;
    }

    // Get MMIO size (typically 128KB for E1000)
    uint32_t mmio_size = pci_get_bar_size(dev, 0);
    if (mmio_size == 0) {
        mmio_size = 0x20000;  // Default to 128KB if size detection fails
    }
    
    kprintf("[E1000] MMIO BAR: phys=0x%lx size=0x%x\n", mmio_addr, mmio_size);
    
    // Store physical address and size
    e1000_mmio_phys = mmio_addr;
    e1000_mmio_size = mmio_size;
    
    // Map MMIO region into virtual address space
    if (e1000_map_mmio(mmio_addr, mmio_size) != 0) {
        kprintf("[E1000] Failed to map MMIO region\n");
        return -1;
    }

    e1000_mmio = (volatile uint8_t *)mmio_addr;
    e1000_initialized = 1;  // Enable MMIO access for init register writes
    kprintf("[E1000] MMIO at 0x%lx (mapped)\n", mmio_addr);

    // Reset the device
    e1000_write(E1000_CTRL, E1000_CTRL_RST);
    for (int i = 0; i < 10000; i++) {
        io_wait();
    }

    // Wait for reset to complete
    int timeout = 1000;
    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) && timeout > 0) {
        timeout--;
        io_wait();
    }

    // Disable interrupts initially
    e1000_write(E1000_IMC, 0xFFFFFFFF);

    // Read MAC address
    e1000_read_mac_eeprom();
    kprintf("[E1000] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            e1000_mac[0], e1000_mac[1], e1000_mac[2],
            e1000_mac[3], e1000_mac[4], e1000_mac[5]);

    // Set the MAC address in RAL/RAH
    uint32_t ral = e1000_mac[0] | (e1000_mac[1] << 8) |
                   (e1000_mac[2] << 16) | (e1000_mac[3] << 24);
    uint32_t rah = e1000_mac[4] | (e1000_mac[5] << 8) | (1U << 31);  // AV bit
    e1000_write(E1000_RAL, ral);
    e1000_write(E1000_RAH, rah);

    // Clear multicast table
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + i * 4, 0);
    }

    // Initialize RX and TX
    e1000_init_rx();
    e1000_init_tx();

    // Set link up
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    e1000_write(E1000_CTRL, ctrl);

    // Enable interrupts
    e1000_write(E1000_IMS, E1000_INT_RXT | E1000_INT_LSC);

    kprintf("[E1000] Driver initialized (link %s)\n",
            e1000_link_up() ? "up" : "down");

    // Debug: print register values
    kprintf("[E1000] CTRL=0x%08x STATUS=0x%08x TCTL=0x%08x RCTL=0x%08x\n",
            e1000_read(E1000_CTRL), e1000_read(E1000_STATUS),
            e1000_read(E1000_TCTL), e1000_read(E1000_RCTL));
    kprintf("[E1000] TDH=%d TDT=%d tx_descs=%p tx_buf[0]=%p\n",
            e1000_read(E1000_TDH), e1000_read(E1000_TDT),
            (void*)tx_descs, (void*)tx_buffers[0]);

    // Send a test broadcast packet
    kprintf("[E1000] Sending test broadcast packet...\n");
    uint8_t test_pkt[64];
    memset(test_pkt, 0xFF, 6);  // Broadcast dest
    memcpy(test_pkt + 6, e1000_mac, 6);  // Our MAC as source
    test_pkt[12] = 0x08;  // EtherType 0x0806 (ARP)
    test_pkt[13] = 0x06;
    memset(test_pkt + 14, 0, 50);  // Padding
    int result = e1000_send(test_pkt, 64);
    kprintf("[E1000] Test packet send result: %d\n", result);

    pci_mark_claimed(dev, "e1000");  // #418: /DEVLOG.TXT PCI-claim tracking
    return 0;
}

// Get MAC address
void e1000_get_mac(uint8_t *mac) {
    if (mac) {
        memcpy(mac, e1000_mac, 6);
    }
}

// Send a packet
int e1000_send(const void *data, uint16_t length) {
    if (!e1000_can_access_mmio() || !data || length == 0) {
        return -1;
    }

    if (length > E1000_TX_BUFFER_SIZE) {
        return -1;
    }

    // Wait for current descriptor to be available
    int timeout = 10000;
    while (!(tx_descs[tx_cur].status & E1000_TXD_STAT_DD) && timeout > 0) {
        timeout--;
    }

    if (timeout == 0) {
        kprintf("[E1000] TX timeout waiting for descriptor\n");
        return -1;
    }

    // Copy data to buffer (use physical address which is identity-mapped)
    uint8_t *buf = tx_buffers[tx_cur];

    // First verify the buffer is writable by writing a test pattern
    buf[0] = 0xDE;
    buf[1] = 0xAD;
    __asm__ volatile("mfence" ::: "memory");

    // Now copy the actual data
    memcpy(buf, data, length);
    __asm__ volatile("mfence" ::: "memory");

    // Set up descriptor with physical buffer address
    tx_descs[tx_cur].addr = tx_buffers_phys[tx_cur];
    tx_descs[tx_cur].length = length;
    tx_descs[tx_cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    tx_descs[tx_cur].status = 0;

    // Memory barrier to ensure all writes are flushed before updating tail
    __asm__ volatile("mfence" ::: "memory");

    // Update tail pointer
    uint32_t old_cur = tx_cur;
    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;

    e1000_write(E1000_TDT, tx_cur);

    // Small delay to allow hardware to process
    for (int i = 0; i < 1000; i++) {
        io_wait();
    }

    // Wait for transmission to complete
    timeout = 10000;
    while (!(tx_descs[old_cur].status & E1000_TXD_STAT_DD) && timeout > 0) {
        timeout--;
    }

    return (timeout > 0) ? length : -1;
}

// Receive a packet
static int e1000_rx_debug_counter __attribute__((unused)) = 0;
int e1000_receive(void *buffer, uint16_t buffer_size) {
    if (!e1000_can_access_mmio() || !buffer) {
        return 0;
    }

    // Check if there's a packet
    if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD)) {
        return 0;
    }

    // Get packet length
    uint16_t length = rx_descs[rx_cur].length;
    if (length > buffer_size) {
        length = buffer_size;
    }

    // Copy data
    memcpy(buffer, rx_buffers[rx_cur], length);

    // Reset descriptor
    rx_descs[rx_cur].status = 0;

    // Update tail pointer
    uint32_t old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write(E1000_RDT, old_cur);

    return length;
}

// Check link status
int e1000_link_up(void) {
    if (!e1000_can_access_mmio()) return 0;
    uint32_t status = e1000_read(E1000_STATUS);
    if (status == 0xFFFFFFFF) return 0;  // Invalid read
    return (status & 2) != 0;
}

// Handle interrupt
void e1000_irq_handler(void) {
    if (!e1000_can_access_mmio()) return;

    uint32_t icr = e1000_read(E1000_ICR);
    if (icr == 0xFFFFFFFF) return;  // Invalid read

    if (icr & E1000_INT_LSC) {
        kprintf("[E1000] Link status changed: %s\n",
                e1000_link_up() ? "up" : "down");
    }

    // RX interrupt handled by polling in receive()
}

// Enter crash context - disables all MMIO access
void e1000_enter_crash_context(void) {
    e1000_in_crash_context = 1;
    __asm__ volatile("mfence" ::: "memory");
}

// Exit crash context - re-enables MMIO access
void e1000_exit_crash_context(void) {
    e1000_in_crash_context = 0;
    __asm__ volatile("mfence" ::: "memory");
}

// Check if E1000 is initialized and safe to use
int e1000_is_safe(void) {
    return e1000_can_access_mmio();
}
