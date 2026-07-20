// sse.c - SSE/FPU initialization for MayteraOS
#include "sse.h"
#include "../serial.h"

static int sse_is_available = 0;

// Note: read_cr0, write_cr0, read_cr4, write_cr4, cpuid are defined in types.h

// Initialize FPU
static inline void fpu_init(void) {
    __asm__ volatile("fninit");
}

// Initialize SSE/FPU support
int sse_init(void) {
    kprintf("[SSE] Initializing SSE/FPU support...\n");

    // Check CPUID for SSE support
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    int has_fpu = (edx & CPUID_FPU) != 0;
    int has_sse = (edx & CPUID_SSE) != 0;
    int has_sse2 = (edx & CPUID_SSE2) != 0;
    int has_fxsr = (edx & CPUID_FXSR) != 0;

    kprintf("[SSE] CPU features: FPU=%d SSE=%d SSE2=%d FXSR=%d\n",
            has_fpu, has_sse, has_sse2, has_fxsr);

    if (!has_fpu) {
        kprintf("[SSE] Error: No FPU support\n");
        return -1;
    }

    if (!has_sse || !has_fxsr) {
        kprintf("[SSE] Error: SSE or FXSAVE not supported\n");
        return -1;
    }

    // Configure CR0:
    // - Clear EM (bit 2): Disable FPU emulation
    // - Set MP (bit 1): Enable FPU monitoring
    // - Set NE (bit 5): Use native FPU error handling
    // - Clear TS (bit 3): Allow FPU/SSE usage
    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;  // Clear EM
    cr0 &= ~CR0_TS;  // Clear TS
    cr0 |= CR0_MP;   // Set MP
    cr0 |= CR0_NE;   // Set NE
    write_cr0(cr0);

    // Configure CR4:
    // - Set OSFXSR (bit 9): Enable FXSAVE/FXRSTOR for SSE
    // - Set OSXMMEXCPT (bit 10): Enable SSE exceptions
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR;
    cr4 |= CR4_OSXMMEXCPT;
    write_cr4(cr4);

    // Initialize the FPU
    fpu_init();

    // Initialize MXCSR to default value (mask all exceptions)
    uint32_t mxcsr = 0x1F80;  // Default: all exceptions masked, round to nearest
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));

    sse_is_available = 1;
    kprintf("[SSE] SSE/FPU initialized successfully\n");

    return 0;
}

// Check if SSE is available
int sse_available(void) {
    return sse_is_available;
}

// Save FPU/SSE state
void sse_save(fxsave_area_t *state) {
    if (!state) return;
    __asm__ volatile("fxsave %0" : "=m"(*state));
}

// Restore FPU/SSE state
void sse_restore(fxsave_area_t *state) {
    if (!state) return;
    __asm__ volatile("fxrstor %0" :: "m"(*state));
}
