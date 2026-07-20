// mphalport.h - Hardware Abstraction Layer for MayteraOS
#ifndef MPHALPORT_H
#define MPHALPORT_H

#include <stdint.h>
#include <stddef.h>

// Time functions
static inline uint64_t mp_hal_time_ticks(void) {
    uint64_t ticks;
    // Get system ticks via syscall
    asm volatile(
        "mov $51, %%rax\n"  // SYS_CLOCK
        "syscall\n"
        : "=a"(ticks)
        :
        : "rcx", "r11", "memory"
    );
    return ticks;
}

static inline uint64_t mp_hal_ticks_ms(void) {
    return mp_hal_time_ticks();  // Assuming ticks are in ms
}

static inline uint64_t mp_hal_ticks_us(void) {
    return mp_hal_time_ticks() * 1000;
}

static inline void mp_hal_delay_ms(uint64_t ms) {
    // Sleep via syscall
    asm volatile(
        "mov $7, %%rax\n"   // SYS_SLEEP
        "mov %0, %%rdi\n"
        "syscall\n"
        :
        : "r"(ms)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
}

static inline void mp_hal_delay_us(uint64_t us) {
    mp_hal_delay_ms((us + 999) / 1000);
}

// Console I/O
int mp_hal_stdin_rx_chr(void);
void mp_hal_stdout_tx_str(const char *str);
void mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len);

// Set interrupt character (Ctrl+C)
void mp_hal_set_interrupt_char(int c);

// GPIO (stub for now)
#define mp_hal_pin_obj_t void*

#endif // MPHALPORT_H
