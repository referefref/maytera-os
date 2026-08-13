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

// ---------------------------------------------------------------------------
// #745 (task #70): THE ASYNCHRONOUS CONSOLE. See the long block comment in
// serial.c for the measurement that motivates it and for the three risks it
// handles deliberately (panic ordering, overflow policy, the pre-thread boot
// path).
// ---------------------------------------------------------------------------

// UNLOCKED, SYNCHRONOUS emission. Panic path only: it must be able to print
// even if the console lock's holder died holding it, and it bypasses the ring
// because by then ordering matters more than throughput.
void kprintf_nolock(const char *fmt, ...);

// Drain whatever the ring still holds STRAIGHT to the UART, taking no locks,
// and latch the console back to synchronous. Called from kpanic_halt(), which
// is the one tail both kpanic() and cpu/idt.c's fault handler reach, so no
// panic can leave the buffer unflushed. Idempotent and safe with IF clear.
void console_panic_flush(void);

// The drain thread body. Started by main.c once the scheduler is live; it sets
// g_console_async itself, so async is never on without a consumer.
void console_drain_worker(void *arg);

// 1 = kprintf enqueues into the ring and console_drain_worker does the polled
// UART writes. Starts at 0 (early boot has no thread), and is cleared again by
// the stall fallback and by console_panic_flush().
extern volatile int      g_console_async;
// 0 = /CONSYNC.TXT was present on the ESP at boot: stay synchronous all boot.
// Exists so the SAME kernel binary can be measured both ways, exactly as
// /SMPSCHED.TXT gates #67's AP scheduling.
extern volatile int      g_console_async_wanted;
// Characters the ring threw away, and how many times the console gave up on an
// unresponsive drain and reverted to synchronous writes.
extern volatile uint64_t g_console_dropped;
extern volatile uint64_t g_console_fallbacks;

// Set by console_panic_flush(): make serial_write() skip its own UART lock,
// because on the way down the holder may be the CPU that just faulted.
extern volatile int      g_serial_lock_bypass;

// #745 (task #70) destructive proofs, armed by /CONPANIC.TXT and /CONBURST.TXT
// on the ESP. See the block comment above console_selftest_worker in serial.c.
extern volatile int g_console_test_panic;
extern volatile int g_console_test_burst;
void console_selftest_worker(void *arg);

// Enable/disable dual output (serial + console)
extern int kprintf_dual_output;
void kprintf_set_dual_output(int enable);

#endif // SERIAL_H
