// mphalport.c - Hardware Abstraction Layer Implementation for MayteraOS

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mpconfig.h"
#include <stdint.h>

// Syscall definitions (must match kernel)
#define SYS_PUTCHAR     40
#define SYS_GETCHAR     41

// Raw syscall wrappers
static inline long syscall1(long num, long arg1) {
    long result;
    asm volatile(
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return result;
}

static inline long syscall0(long num) {
    long result;
    asm volatile(
        "syscall"
        : "=a"(result)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return result;
}

// Interrupt character for Ctrl+C
static int interrupt_char = -1;

void mp_hal_set_interrupt_char(int c) {
    interrupt_char = c;
}

// Read a character from stdin
int mp_hal_stdin_rx_chr(void) {
    int c;
    while (1) {
        c = (int)syscall0(SYS_GETCHAR);
        if (c >= 0) {
            // Check for interrupt character
            if (c == interrupt_char) {
                mp_sched_keyboard_interrupt();
            }
            return c;
        }
        // No char available, yield
        asm volatile(
            "mov $6, %%rax\n"  // SYS_YIELD
            "syscall\n"
            ::: "rax", "rcx", "r11", "memory"
        );
    }
}

// Write a string to stdout
void mp_hal_stdout_tx_str(const char *str) {
    while (*str) {
        syscall1(SYS_PUTCHAR, *str++);
    }
}

// Write a string with length to stdout
void mp_hal_stdout_tx_strn(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        syscall1(SYS_PUTCHAR, str[i]);
    }
}

// Write a string with newline conversion
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            syscall1(SYS_PUTCHAR, '\r');
        }
        syscall1(SYS_PUTCHAR, str[i]);
    }
}

// Poll for MayteraOS events (keyboard, etc.)
void mp_maytera_poll_events(void) {
    // Check for keyboard interrupt
    int c = (int)syscall0(SYS_GETCHAR);
    if (c == interrupt_char && interrupt_char >= 0) {
        mp_sched_keyboard_interrupt();
    }
}
