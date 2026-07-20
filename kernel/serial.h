// serial.h - Serial port driver for debugging
#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

// Serial port base addresses
#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

// Serial port registers (offsets from base)
#define SERIAL_DATA         0   // Data register (RW)
#define SERIAL_IER          1   // Interrupt Enable Register
#define SERIAL_FIFO         2   // FIFO control register
#define SERIAL_LCR          3   // Line Control Register
#define SERIAL_MCR          4   // Modem Control Register
#define SERIAL_LSR          5   // Line Status Register
#define SERIAL_MSR          6   // Modem Status Register

// When DLAB (Divisor Latch Access Bit) is set in LCR:
#define SERIAL_DLL          0   // Divisor Latch Low byte
#define SERIAL_DLH          1   // Divisor Latch High byte

// Line Status Register bits
#define SERIAL_LSR_DR       0x01  // Data Ready
#define SERIAL_LSR_THRE     0x20  // Transmitter Holding Register Empty

// Initialize serial port
int serial_init(uint16_t port, uint32_t baud);

// Check if serial port is ready
int serial_received(uint16_t port);
int serial_is_transmit_empty(uint16_t port);

// Read/write single character
char serial_read(uint16_t port);
void serial_write(uint16_t port, char c);

// Write string to serial port
void serial_puts(uint16_t port, const char *str);

// Printf-like function for serial output
void serial_printf(uint16_t port, const char *fmt, ...);

// Default serial output (uses COM1)
void kprintf(const char *fmt, ...);
void kputs(const char *str);
void kputc(char c);

// Enable/disable dual output (serial + console)
extern int kprintf_dual_output;
void kprintf_set_dual_output(int enable);

#endif // SERIAL_H
