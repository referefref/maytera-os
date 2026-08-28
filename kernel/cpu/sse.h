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
#define CR4_OSXSAVE   (1 << 18)  // Enable XSAVE/XRSTOR + XGETBV/XSETBV, and
                                 // gate every VEX-encoded (AVX) instruction

// CPUID feature flags (EDX)
#define CPUID_FPU     (1 << 0)   // x87 FPU
#define CPUID_SSE     (1 << 25)  // SSE
#define CPUID_SSE2    (1 << 26)  // SSE2
#define CPUID_FXSR    (1 << 24)  // FXSAVE/FXRSTOR

// CPUID.1 feature flags (ECX)
#define CPUID_XSAVE   (1u << 26) // XSAVE/XRSTOR/XSETBV/XGETBV implemented
#define CPUID_OSXSAVE (1u << 27) // reads back CR4.OSXSAVE
#define CPUID_AVX     (1u << 28) // AVX (VEX-encoded 256-bit YMM)

// XCR0 state-component bitmap (#745 local 107).
#define XCR0_X87      (1ull << 0)
#define XCR0_SSE      (1ull << 1)
#define XCR0_AVX      (1ull << 2)   // YMM_Hi128: the 128 bits fxsave64 loses
// The components this kernel is prepared to SAVE. Do not add a bit here
// without also proving FPU_AREA_SIZE still covers CPUID.(0Dh,0):EBX for the
// resulting XCR0 - sse_init() checks that at boot and refuses rather than
// overflowing process_t::fpu_area, but the check is a backstop, not a design.
#define FPU_XCR0_WANT (XCR0_X87 | XCR0_SSE | XCR0_AVX)

// THE ONE per-task FPU/SSE/AVX save area size and alignment. process_t,
// thread_t and g_dummy_fpu_area all use it; there is no second save path.
//   fxsave64 : 512 bytes, 16-byte aligned.
//   xsave64  : 512 legacy + 64 header + the enabled components, 64-byte
//              aligned. For XCR0 = x87|SSE|AVX that is 832 bytes on every
//              CPU that implements it (YMM_Hi128 at offset 576, size 256).
// 1024 leaves headroom without making process_t (MAX_PROCESSES <= 64, a
// static table) meaningfully larger: the whole table grows by 32 KB of .bss.
// AVX-512 would need 2688 and is deliberately NOT enabled: see FPU_XCR0_WANT.
#define FPU_AREA_SIZE  1024
#define FPU_AREA_ALIGN 64

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

// #745 local 107: set by sse_init() on EVERY core (BSP and each AP; CR4 and
// XCR0 are per-logical-processor). Read by proc/context_switch.asm to choose
// xsave64/xrstor64 over fxsave64/fxrstor64. Zero is the SAFE default: it means
// "AVX is not enabled on this machine", which is the state sse_init()
// guarantees by explicitly CLEARING CR4.OSXSAVE whenever it cannot save AVX.
extern uint8_t  g_fpu_use_xsave;
extern uint64_t g_fpu_xcr0;   // the RFBM passed to xsave64/xrstor64

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
