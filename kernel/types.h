// types.h - Basic type definitions for MayteraOS 64-bit kernel
#ifndef TYPES_H
#define TYPES_H

// ---------------------------------------------------------------------------
// #693: a persistence result that is thrown away makes a FAILED write
// indistinguishable from a successful one.
//
// MUST_CHECK marks a function whose return value states whether bytes reached
// persistent storage. Combined with the kernel's -Werror this is a
// COMPILER-ENFORCED control, not a lint. That choice is deliberate: #514 found
// the concurrency lint had silently not run for months because it was not a
// prerequisite of anything. An attribute cannot drift, cannot be forgotten in a
// Makefile, and cannot be left out of a source tarball.
//
// MEASURED on gcc 12.2.0, and the reason IGNORE_RESULT has to exist: a plain
// `(void)` cast does NOT silence warn_unused_result. gcc rejects `(void)f();`
// with exactly the same error as `f();`. Assigning to an unused variable is the
// only form gcc accepts. That is turned into a feature here: IGNORE_RESULT
// takes a REASON string, so a deliberately-dropped persistence result carries
// its justification at the call site, in the same file, where the next reader is
// already looking - not in a separate allowlist that rots (again, #514).
#define MUST_CHECK __attribute__((warn_unused_result))

#define IGNORE_RESULT(reason, expr)                                          \
    do {                                                                       \
        __attribute__((unused)) const char *_pi_why = (reason);                \
        __attribute__((unused)) long long   _pi_rc  = (long long)(expr);       \
    } while (0)


// Standard integer types
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

// Size types
typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;

// Boolean type
typedef int bool;
#define true  1
#define false 0

// NULL pointer
#define NULL ((void*)0)

// offsetof macro for structure field offset calculation
#ifndef offsetof
#define offsetof(type, member) ((size_t)(&((type *)0)->member))
#endif

// Useful macros
#define UNUSED(x) ((void)(x))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

// Memory sizes
#define KB (1024ULL)
#define MB (1024ULL * KB)
#define GB (1024ULL * MB)
#define TB (1024ULL * GB)

// Page size
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12

// Inline assembly helpers
#define cli() __asm__ volatile("cli")
#define sti() __asm__ volatile("sti")
#define hlt() __asm__ volatile("hlt")
#define nop() __asm__ volatile("nop")
#define pause() __asm__ volatile("pause")

// Port I/O
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// I/O wait (slight delay)
static inline void io_wait(void) {
    outb(0x80, 0);
}

// Read CR registers
static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

// Write CR registers
static inline void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(val));
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val));
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val));
}

// MSR access
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

// CPUID
static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

#endif // TYPES_H
