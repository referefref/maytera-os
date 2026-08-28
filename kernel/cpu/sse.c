// sse.c - SSE/FPU initialization for MayteraOS
#include "sse.h"
#include "../serial.h"

static int sse_is_available = 0;

// #745 local 107. See sse.h. Definitions live here because sse_init() is the
// ONE place that decides them, on the BSP and on every AP.
uint8_t  g_fpu_use_xsave = 0;
uint64_t g_fpu_xcr0      = 0;

static inline uint64_t xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv0(uint64_t v) {
    __asm__ volatile("xsetbv" :: "a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(0));
}

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

    // ---- #745 local 107: AVX state, or no AVX at all -------------------
    //
    // fxsave64/fxrstor64 (proc/context_switch.asm) save x87 and SSE, i.e.
    // XMM0-15. They DO NOT save the upper 128 bits of YMM0-15. A kernel that
    // lets AVX execute while saving state with fxsave64 loses those bits on
    // every context switch: silent, non-deterministic corruption of whatever
    // held a 256-bit value, with no fault and nothing to trace.
    //
    // There are therefore exactly two safe machine states, and this function
    // puts the core in one of them:
    //
    //   (a) XSAVE present -> set CR4.OSXSAVE, put x87|SSE|AVX in XCR0, and let
    //       context_switch use xsave64/xrstor64. AVX is legal AND preserved.
    //   (b) otherwise      -> CLEAR CR4.OSXSAVE. Every VEX-encoded instruction
    //       then raises #UD, in Ring 0 and Ring 3 alike, so no YMM state can
    //       exist to be lost.
    //
    // The clear in (b) is deliberate, not assumed. CR4 is INHERITED from the
    // firmware and this kernel had only ever OR-ed bits into it, so on any
    // machine whose UEFI left CR4.OSXSAVE set (we cannot inspect every one)
    // AVX was already legal underneath an fxsave64-only save path. That is the
    // one way the corruption could have been live rather than latent.
    //
    // Boot line printed unconditionally: on a machine we cannot log into, the
    // CR4/XCR0 pair is the only way to know which state it ended up in.
    {
        uint32_t xa, xb, xc, xd;
        cpuid(1, &xa, &xb, &xc, &xd);
        int has_xsave = (xc & CPUID_XSAVE) != 0;
        int has_avx   = (xc & CPUID_AVX) != 0;

        g_fpu_use_xsave = 0;
        g_fpu_xcr0      = 0;

        if (has_xsave) {
            write_cr4(read_cr4() | CR4_OSXSAVE);

            // CPUID.(0Dh,0): EDX:EAX = XCR0 bits this CPU supports,
            //                EBX     = save-area size for the CURRENT XCR0.
            cpuid(0x0D, &xa, &xb, &xc, &xd);
            uint64_t supported = ((uint64_t)xd << 32) | xa;
            uint64_t want = (FPU_XCR0_WANT & supported) | XCR0_X87;
            if (!(want & XCR0_SSE)) want = XCR0_X87;          // AVX requires SSE
            xsetbv0(want);

            cpuid(0x0D, &xa, &xb, &xc, &xd);                  // re-read for EBX
            if (xb <= FPU_AREA_SIZE) {
                g_fpu_xcr0      = want;
                g_fpu_use_xsave = 1;
            } else {
                // Refuse rather than overflow fpu_area. Back to state (b).
                xsetbv0(XCR0_X87 | (supported & XCR0_SSE));
                write_cr4(read_cr4() & ~(uint64_t)CR4_OSXSAVE);
                kprintf("[SSE] XSAVE area %u > %u bytes; XSAVE DISABLED\n",
                        xb, (unsigned)FPU_AREA_SIZE);
            }
        } else {
            write_cr4(read_cr4() & ~(uint64_t)CR4_OSXSAVE);
        }

#ifdef FPU_YMM_SELFTEST_RED
        // NEGATIVE CONTROL (make YMMTEST=red): leave AVX ENABLED in XCR0 but
        // force the fxsave64 save path, which is exactly the shape of the bug
        // (AVX legal on the CPU, only x87+SSE actually saved). This build MUST
        // FAIL the YMM test; if it does not, the test proves nothing.
        g_fpu_use_xsave = 0;
        kprintf("[SSE] *** YMMTEST RED: AVX enabled but forcing fxsave64 ***\n");
#endif

        kprintf("[SSE] cr4=0x%lx xcr0=0x%lx cpuid.xsave=%d cpuid.avx=%d "
                "save=%s\n",
                read_cr4(),
                (read_cr4() & CR4_OSXSAVE) ? xgetbv0() : 0ull,
                has_xsave, has_avx,
                g_fpu_use_xsave ? "xsave64(x87+SSE+AVX)" : "fxsave64(x87+SSE)");
    }

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
