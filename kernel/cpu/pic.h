// pic.h - 8259 Programmable Interrupt Controller driver
#ifndef PIC_H
#define PIC_H

#include "../types.h"

// PIC I/O ports
#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

// PIC commands
#define PIC_EOI         0x20    // End of Interrupt

// ICW1 (Initialization Command Word 1)
#define ICW1_ICW4       0x01    // ICW4 needed
#define ICW1_SINGLE     0x02    // Single mode (vs cascade)
#define ICW1_INTERVAL4  0x04    // Call address interval 4
#define ICW1_LEVEL      0x08    // Level triggered mode
#define ICW1_INIT       0x10    // Initialization

// ICW4 (Initialization Command Word 4)
#define ICW4_8086       0x01    // 8086/88 mode
#define ICW4_AUTO       0x02    // Auto EOI
#define ICW4_BUF_SLAVE  0x08    // Buffered mode (slave)
#define ICW4_BUF_MASTER 0x0C    // Buffered mode (master)
#define ICW4_SFNM       0x10    // Special fully nested mode

// Initialize the PIC and remap IRQs
void pic_init(void);

// Send End of Interrupt signal
void pic_send_eoi(uint8_t irq);

// Enable a specific IRQ
void pic_enable_irq(uint8_t irq);

// Disable a specific IRQ
void pic_disable_irq(uint8_t irq);

// Disable all IRQs (mask all)
void pic_disable(void);

// Get the combined ISR (In-Service Register)
uint16_t pic_get_isr(void);

// Get the combined IRR (Interrupt Request Register)
uint16_t pic_get_irr(void);

#endif // PIC_H

// PIT initialization
void pit_init(uint32_t frequency_hz);

// PIT frequency control
extern uint32_t g_timer_hz;
void pit_set_frequency(uint32_t frequency_hz);
uint32_t pit_get_frequency(void);
