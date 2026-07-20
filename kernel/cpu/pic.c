// pic.c - 8259 Programmable Interrupt Controller driver
#include "pic.h"
#include "../serial.h"

// IRQ offset for PIC1 (master) and PIC2 (slave)
#define PIC1_OFFSET     32
#define PIC2_OFFSET     40

// Initialize the PIC
void pic_init(void) {
    kprintf("[PIC] Initializing 8259 PIC...\n");

    // Start initialization sequence (cascade mode)
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    // ICW2: Set vector offsets
    // Map IRQ 0-7 to INT 32-39
    outb(PIC1_DATA, PIC1_OFFSET);
    io_wait();
    // Map IRQ 8-15 to INT 40-47
    outb(PIC2_DATA, PIC2_OFFSET);
    io_wait();

    // ICW3: Set cascade configuration
    // Tell Master PIC there is a slave at IRQ2 (bit 2)
    outb(PIC1_DATA, 0x04);
    io_wait();
    // Tell Slave PIC its cascade identity (IRQ2 = 2)
    outb(PIC2_DATA, 0x02);
    io_wait();

    // ICW4: Set 8086 mode
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    // Restore masks (or set to mask all initially)
    // Mask all IRQs except cascade (IRQ2)
    outb(PIC1_DATA, 0xFB);  // 11111011 - only IRQ2 (cascade) enabled
    outb(PIC2_DATA, 0xFF);  // All slave IRQs masked

    kprintf("[PIC] IRQs remapped to INT %d-%d\n", PIC1_OFFSET, PIC2_OFFSET + 7);
}

// Send End of Interrupt signal
void pic_send_eoi(uint8_t irq) {
    // If IRQ came from slave PIC (IRQ 8-15), send EOI to both
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    // Always send EOI to master
    outb(PIC1_COMMAND, PIC_EOI);
}

// Enable a specific IRQ
void pic_enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

// Disable a specific IRQ
void pic_disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port) | (1 << irq);
    outb(port, value);
}

// Disable all IRQs
void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// Helper to read OCW3 register
static uint16_t pic_get_reg(int ocw3) {
    outb(PIC1_COMMAND, ocw3);
    outb(PIC2_COMMAND, ocw3);
    return (inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

// Get In-Service Register
uint16_t pic_get_isr(void) {
    return pic_get_reg(0x0B);  // OCW3: read ISR
}

// Get Interrupt Request Register
uint16_t pic_get_irr(void) {
    return pic_get_reg(0x0A);  // OCW3: read IRR
}

// ============================================================================
// PIT (Programmable Interval Timer) Configuration
// ============================================================================

#define PIT_CHANNEL0_DATA   0x40
#define PIT_COMMAND         0x43
#define PIT_BASE_FREQ       1193182  // Base frequency in Hz

// Initialize PIT for specified frequency
void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;  // Default 100 Hz
    if (frequency_hz > 10000) frequency_hz = 10000;  // Max 10 kHz
    
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    // Command: Channel 0, lobyte/hibyte, rate generator mode, binary
    outb(PIT_COMMAND, 0x36);
    
    // Send divisor
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);         // Low byte
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);  // High byte
    
    g_timer_hz = frequency_hz;
    kprintf("[PIT] Timer configured: %u Hz (divisor %u)\n", frequency_hz, divisor);
}

// Current timer frequency in Hz
uint32_t g_timer_hz = 250;  // Default 250 Hz

// Change PIT frequency at runtime
void pit_set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 250;
    if (frequency_hz > 10000) frequency_hz = 10000;
    if (frequency_hz < 18) frequency_hz = 18;
    
    g_timer_hz = frequency_hz;
    
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);
    
    kprintf("[PIT] Frequency changed to %u Hz\n", frequency_hz);
}

// Get current tick rate
uint32_t pit_get_frequency(void) {
    return g_timer_hz;
}
