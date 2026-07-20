// gdt.h - Global Descriptor Table for x86_64
#ifndef GDT_H
#define GDT_H

#include "../types.h"

// GDT segment selectors (without RPL)
#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
// Build 103: Swapped for SYSRET compatibility
#define GDT_USER_DATA   0x18    // User data at entry 3 (SYSRET: SS = STAR+8)
#define GDT_USER_CODE   0x20    // User code at entry 4 (SYSRET: CS = STAR+16)
#define GDT_TSS         0x28

// User-mode selectors with RPL=3 (for IRET to Ring 3)
#define GDT_USER_CODE_RPL3  (GDT_USER_CODE | 3)  // 0x23
#define GDT_USER_DATA_RPL3  (GDT_USER_DATA | 3)  // 0x1B

// Access byte flags
#define GDT_PRESENT     (1 << 7)
#define GDT_DPL_RING0   (0 << 5)
#define GDT_DPL_RING3   (3 << 5)
#define GDT_SEGMENT     (1 << 4)
#define GDT_EXECUTABLE  (1 << 3)
#define GDT_DC          (1 << 2)  // Direction/Conforming
#define GDT_RW          (1 << 1)  // Read/Write
#define GDT_ACCESSED    (1 << 0)

// Flags nibble
#define GDT_GRANULARITY (1 << 3)  // 4KB granularity
#define GDT_LONG_MODE   (1 << 1)  // 64-bit code segment
#define GDT_SIZE_32     (1 << 2)  // 32-bit segment (not used in 64-bit mode)

// GDT entry structure (8 bytes for normal entries)
typedef struct {
    uint16_t limit_low;     // Limit bits 0-15
    uint16_t base_low;      // Base bits 0-15
    uint8_t  base_middle;   // Base bits 16-23
    uint8_t  access;        // Access byte
    uint8_t  flags_limit;   // Flags (4 bits) + Limit bits 16-19
    uint8_t  base_high;     // Base bits 24-31
} __attribute__((packed)) gdt_entry_t;

// TSS entry in GDT (16 bytes for 64-bit TSS)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle1;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_middle2;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) tss_entry_t;

// GDT pointer structure (for LGDT instruction)
typedef struct {
    uint16_t limit;         // Size of GDT - 1
    uint64_t base;          // Linear address of GDT
} __attribute__((packed)) gdt_ptr_t;

// Task State Segment (64-bit)
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          // Stack pointer for ring 0
    uint64_t rsp1;          // Stack pointer for ring 1
    uint64_t rsp2;          // Stack pointer for ring 2
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table entry 1
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   // I/O Permission Bitmap offset
} __attribute__((packed)) tss_t;

// Initialize GDT
void gdt_init(void);
void gdt_load_ap(void);
void gdt_init_ap(uint32_t cpu, uint64_t kstack_top);   // #279 per-CPU TSS+GDT
void gdt_set_kernel_stack_cpu(uint32_t cpu, uint64_t stack);
void cpu_set_kernel_stack(uint64_t top);   // #279 3b-1.5 per-CPU ring0 stack (syscall+IRQ)

// Set kernel stack pointer in TSS (for syscalls/interrupts)
void gdt_set_kernel_stack(uint64_t stack);

// External assembly function
extern void gdt_load(gdt_ptr_t *gdt_ptr, uint16_t code_seg, uint16_t data_seg);

#endif // GDT_H
