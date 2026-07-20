#include <stdint.h>

extern void kprintf(const char *fmt, ...);

void print_rax(const char *msg, uint64_t rax_value) {
    kprintf("[ASM-DEBUG] %s: RAX=0x%lx (%ld)\n", msg, rax_value, (int64_t)rax_value);
}
