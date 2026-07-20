// serial.c - Serial port driver implementation
#include "serial.h"
#include "string.h"
#include <stdarg.h>

// Initialize serial port
int serial_init(uint16_t port, uint32_t baud) {
    uint16_t divisor = 115200 / baud;

    // Disable interrupts
    outb(port + SERIAL_IER, 0x00);

    // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_LCR, 0x80);

    // Set divisor (low and high bytes)
    outb(port + SERIAL_DLL, divisor & 0xFF);
    outb(port + SERIAL_DLH, (divisor >> 8) & 0xFF);

    // 8 bits, no parity, one stop bit (8N1)
    outb(port + SERIAL_LCR, 0x03);

    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + SERIAL_FIFO, 0xC7);

    // IRQs enabled, RTS/DSR set
    outb(port + SERIAL_MCR, 0x0B);

    // Set in loopback mode to test serial chip
    outb(port + SERIAL_MCR, 0x1E);

    // Test serial chip by sending byte 0xAE
    outb(port + SERIAL_DATA, 0xAE);

    // Check if serial is faulty (i.e., not same byte as sent)
    if (inb(port + SERIAL_DATA) != 0xAE) {
        return -1;  // Serial port is faulty
    }

    // If serial is not faulty, set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(port + SERIAL_MCR, 0x0F);

    return 0;
}

// Check if data is available to read
int serial_received(uint16_t port) {
    return inb(port + SERIAL_LSR) & SERIAL_LSR_DR;
}

// Check if transmit buffer is empty
int serial_is_transmit_empty(uint16_t port) {
    return inb(port + SERIAL_LSR) & SERIAL_LSR_THRE;
}

// Read a character from serial port
char serial_read(uint16_t port) {
    while (!serial_received(port));
    return inb(port + SERIAL_DATA);
}

// Write a character to serial port
void serial_write(uint16_t port, char c) {
    // BOUNDED wait for the UART transmit-holding register to empty. An unbounded
    // poll here hangs the ENTIRE kernel when TX can't drain: e.g. QEMU's serial
    // chardev buffer fills because nothing is reading the serial socket, so the
    // THRE bit never sets. That deterministically wedged VM2200's toram boot at
    // "Starting desktop services..." (trapped RIP inside this loop, RDX=0x3F8 /
    // RBP=0x3FD). #426 no-unbounded-busy-wait discipline: cap the spin and drop
    // the char rather than freezing the machine. When TX is healthy the very
    // first check passes, so this is behavior-identical on a draining port.
    for (int spin = 0; spin < 200000; spin++) {
        if (serial_is_transmit_empty(port)) {
            outb(port + SERIAL_DATA, c);
            return;
        }
    }
    // TX not draining; drop this char instead of hanging the kernel.
}

// Write a string to serial port
void serial_puts(uint16_t port, const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_write(port, '\r');
        }
        serial_write(port, *str++);
    }
}

// Helper function to print a number
static void print_number(uint16_t port, unsigned long value, int base, int width, char pad, int uppercase) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    char buffer[32];
    int i = 0;
    int len;

    if (value == 0) {
        buffer[i++] = '0';
    } else {
        while (value > 0) {
            buffer[i++] = digits[value % base];
            value /= base;
        }
    }

    len = i;

    // Padding
    while (len < width) {
        serial_write(port, pad);
        len++;
    }

    // Print in reverse order
    while (i > 0) {
        serial_write(port, buffer[--i]);
    }
}

// Printf-like function for serial output
void serial_printf(uint16_t port, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            // Parse width and padding
            char pad = ' ';
            int width = 0;
            int is_long = 0;

            if (*fmt == '0') {
                pad = '0';
                fmt++;
            }

            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            // Check for 'l' modifier
            if (*fmt == 'l') {
                is_long = 1;
                fmt++;
                if (*fmt == 'l') {
                    is_long = 2;
                    fmt++;
                }
            }

            switch (*fmt) {
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_write(port, c);
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (s == NULL) {
                        s = "(null)";
                    }
                    serial_puts(port, s);
                    break;
                }
                case 'd':
                case 'i': {
                    long value;
                    if (is_long) {
                        value = va_arg(args, long);
                    } else {
                        value = va_arg(args, int);
                    }
                    if (value < 0) {
                        serial_write(port, '-');
                        value = -value;
                    }
                    print_number(port, (unsigned long)value, 10, width, pad, 0);
                    break;
                }
                case 'u': {
                    unsigned long value;
                    if (is_long) {
                        value = va_arg(args, unsigned long);
                    } else {
                        value = va_arg(args, unsigned int);
                    }
                    print_number(port, value, 10, width, pad, 0);
                    break;
                }
                case 'x': {
                    unsigned long value;
                    if (is_long) {
                        value = va_arg(args, unsigned long);
                    } else {
                        value = va_arg(args, unsigned int);
                    }
                    print_number(port, value, 16, width, pad, 0);
                    break;
                }
                case 'X': {
                    unsigned long value;
                    if (is_long) {
                        value = va_arg(args, unsigned long);
                    } else {
                        value = va_arg(args, unsigned int);
                    }
                    print_number(port, value, 16, width, pad, 1);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    serial_puts(port, "0x");
                    print_number(port, (unsigned long)ptr, 16, sizeof(void*) * 2, '0', 0);
                    break;
                }
                case '%':
                    serial_write(port, '%');
                    break;
                default:
                    serial_write(port, '%');
                    serial_write(port, *fmt);
                    break;
            }
        } else {
            if (*fmt == '\n') {
                serial_write(port, '\r');
            }
            serial_write(port, *fmt);
        }
        fmt++;
    }

    va_end(args);
}

// Default kprintf functions using COM1
void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // We need to manually handle this since we can't easily forward va_list
    // So we'll reimplement the loop here
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            char pad = ' ';
            int width = 0;
            int is_long = 0;

            if (*fmt == '0') {
                pad = '0';
                fmt++;
            }

            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            if (*fmt == 'l') {
                is_long = 1;
                fmt++;
                if (*fmt == 'l') {
                    is_long = 2;
                    fmt++;
                }
            }

            switch (*fmt) {
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_write(COM1, c);
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (s == NULL) s = "(null)";
                    serial_puts(COM1, s);
                    break;
                }
                case 'd':
                case 'i': {
                    long value = is_long ? va_arg(args, long) : va_arg(args, int);
                    if (value < 0) {
                        serial_write(COM1, '-');
                        value = -value;
                    }
                    print_number(COM1, (unsigned long)value, 10, width, pad, 0);
                    break;
                }
                case 'u': {
                    unsigned long value = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                    print_number(COM1, value, 10, width, pad, 0);
                    break;
                }
                case 'x': {
                    unsigned long value = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                    print_number(COM1, value, 16, width, pad, 0);
                    break;
                }
                case 'X': {
                    unsigned long value = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                    print_number(COM1, value, 16, width, pad, 1);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    serial_puts(COM1, "0x");
                    print_number(COM1, (unsigned long)ptr, 16, 16, '0', 0);
                    break;
                }
                case '%':
                    serial_write(COM1, '%');
                    break;
                default:
                    serial_write(COM1, '%');
                    serial_write(COM1, *fmt);
                    break;
            }
        } else {
            if (*fmt == '\n') {
                serial_write(COM1, '\r');
            }
            serial_write(COM1, *fmt);
        }
        fmt++;
    }

    va_end(args);
}

// Global flag for dual output mode
int kprintf_dual_output = 0;

// External console function (from video/console.c)
extern void console_putc(char c);

void kprintf_set_dual_output(int enable) {
    kprintf_dual_output = enable;
}

void kputs(const char *str) {
    serial_puts(COM1, str);
    if (kprintf_dual_output) {
        while (*str) {
            console_putc(*str++);
        }
    }
}

void kputc(char c) {
    if (c == '\n') {
        serial_write(COM1, '\r');
    }
    serial_write(COM1, c);
    if (kprintf_dual_output) {
        console_putc(c);
    }
}
