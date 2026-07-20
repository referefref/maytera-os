// sse.h - SSE/FPU initialization for MayteraOS
#ifndef SSE_H
#define SSE_H

#include "../types.h"

// CR0 bits
#define CR0_EM  (1 << 2)   // x87 FPU Emulation
#define CR0_TS  (1 << 3)   // Task Switched
#define CR0_ET  (1 << 4)   // Extension Type (always 1 on modern CPUs)
#define CR0_NE  (1 << 5)   // Numeric Error
#define CR0_MP  (1 << 1)   // Monitor Coprocessor

// CR4 bits
#define CR4_OSFXSR    (1 << 9)   // Enable FXSAVE/FXRSTOR
#define CR4_OSXMMEXCPT (1 << 10) // Enable SSE exceptions

// CPUID feature flags (EDX)
#define CPUID_FPU     (1 << 0)   // x87 FPU
#define CPUID_SSE     (1 << 25)  // SSE
#define CPUID_SSE2    (1 << 26)  // SSE2
#define CPUID_FXSR    (1 << 24)  // FXSAVE/FXRSTOR

// FPU/SSE state structure (512 bytes, 16-byte aligned)
typedef struct __attribute__((aligned(16))) {
    uint16_t fcw;           // FPU control word
    uint16_t fsw;           // FPU status word
    uint8_t  ftw;           // FPU tag word (abridged)
    uint8_t  reserved1;
    uint16_t fop;           // FPU opcode
    uint64_t fip;           // FPU instruction pointer
    uint64_t fdp;           // FPU data pointer
    uint32_t mxcsr;         // SSE control/status register
    uint32_t mxcsr_mask;    // MXCSR mask
    uint8_t  st_mm[8][16];  // x87/MMX registers (ST0-ST7/MM0-MM7)
    uint8_t  xmm[16][16];   // SSE registers (XMM0-XMM15)
    uint8_t  reserved2[96]; // Reserved
} fxsave_area_t;

/**
 * Initialize SSE/FPU support
 * Must be called early in kernel initialization
 * Returns 0 on success, -1 if SSE not supported
 */
int sse_init(void);

/**
 * Check if SSE is available
 */
int sse_available(void);

/**
 * Save FPU/SSE state
 * Buffer must be 16-byte aligned
 */
void sse_save(fxsave_area_t *state);

/**
 * Restore FPU/SSE state
 * Buffer must be 16-byte aligned
 */
void sse_restore(fxsave_area_t *state);

#endif // SSE_H
