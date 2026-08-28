// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// fput110 - #110 artefact-level probe: does the child of fork()/clone() get the
// PARENT'S floating-point state, or a freshly initialized one?
//
// WHY IT IS NOT A KERNEL SELF-TEST. The kernel is built -mno-sse -mno-sse2 and
// cannot even express the state under test; the state being inherited belongs
// to Ring 3. A kernel test could at best inspect the bytes it just wrote into
// child->fpu_area, which is the thing under test asserting about itself. Only a
// Ring-3 process that puts a distinctive pattern in the real registers, forks,
// and reads the registers back in the CHILD proves the whole chain: the capture
// in proc_fork()/proc_clone(), the image in process_t::fpu_area, and the
// fxrstor64/xrstor64 the context switch does on the child's first run.
//
// WHY EVERYTHING HAPPENS INSIDE ONE INLINE-ASM BLOCK. Between "set the FPU
// state" and "read it back" there must be no compiler-generated code at all:
// userland is compiled -msse2 and a struct copy, a spill or a call could touch
// XMM. So the pattern load, the raw SYSCALL and the fxsave64 are one asm
// statement, and the parent/child split is a branch INSIDE it, so neither arm
// executes a single instruction this file did not write.
//
// WHY ONE STRUCT AND NOT SEVEN POINTER OPERANDS. The clone arm clobbers RAX,
// RCX, RDX, RSI, RDI, R8, R10 and R11 for the syscall ABI; asking gcc for a
// register per buffer on top of that is "impossible constraints". Everything
// the asm touches lives in ONE 64-byte-aligned struct addressed by a single
// register, with the field offsets passed as immediates (which cost no
// register at all).
//
// WHAT IT CHECKS. Not just the data registers: also MXCSR and the x87 control
// word. A copy that carried XMM0-15 but reset the rounding mode or unmasked an
// exception would still be wrong, and would still pass a register-only test.
// The patterns are distinctive PER REGISTER, so a partial copy shows up as a
// named register rather than as "all zero".
//
// Launched via /CONFIG/AUTORUN.CFG on a throwaway VM; stdout reaches serial
// through /dev/console. Verification aid, not a shipped app.
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

// ---------------------------------------------------------------------------
// The distinctive state. Every value is deliberately NOT the architectural
// default, so "the child got a fresh state" and "the child got the parent's
// state" are different bit patterns rather than zero vs nonzero.
//
//   x87 control word 0x0B7F : default is 0x037F. Same exception masks, but
//                             RC = 10 (round toward +inf) instead of 00.
//   MXCSR            0xFF80 : default is 0x1F80. Same exception masks, plus
//                             FTZ (bit 15) and RC = 11 (round toward zero).
//                             DAZ (bit 6) is deliberately NOT set: it is not
//                             guaranteed present in MXCSR_MASK and ldmxcsr
//                             #GPs on a bit the CPU does not implement.
//   st0                     : a finite double, loaded by bit pattern.
//   xmm(i)                  : a per-register pair, so a partial copy is named.
// ---------------------------------------------------------------------------
#define WANT_CW    0x0B7Fu
#define WANT_MXCSR 0x0000FF80u
#define WANT_DBL   0x40C34567ABCDEF01ULL

typedef struct {
    unsigned char      pbuf[512];    // fxsave64 image written by the PARENT
    unsigned char      cbuf[512];    // fxsave64 image written by the CHILD
    unsigned long long pat[32];      // 16 x 16 bytes of XMM patterns
    unsigned long long dbl;          // st0 source
    unsigned int       mx;           // ldmxcsr source
    unsigned short     cw;           // fldcw source
    unsigned short     _pad;
    volatile int       flag;         // clone child sets this when cbuf is written
    unsigned int       _pad2;
    unsigned long      flags;        // clone flags
    void              *sp;           // clone child stack top
    // #107 AVX arm. fxsave64 cannot see YMM_Hi128 at all, so the upper half of
    // each 256-bit register needs its own pattern and its own dump. Only
    // touched when the runtime check says every VEX instruction here is legal.
    unsigned long long ypat[64];     // 16 x 32 bytes of YMM patterns
    unsigned char      pyhi[256];    // parent's ymm0-15 upper halves
    unsigned char      cyhi[256];    // child's  ymm0-15 upper halves
} ctx_t;

static ctx_t g_ctx __attribute__((aligned(64)));

#define O(f) __builtin_offsetof(ctx_t, f)

static int g_fail;
static void check(const char *what, int ok, const char *detail);

// fxsave64 image accessors (Intel SDM Vol.1, "FXSAVE Area Layout").
static unsigned short     fx_fcw(const unsigned char *a)   { unsigned short v;     memcpy(&v, a + 0,  2); return v; }
static unsigned int       fx_mxcsr(const unsigned char *a) { unsigned int v;       memcpy(&v, a + 24, 4); return v; }
static unsigned long long fx_st0(const unsigned char *a)   { unsigned long long v; memcpy(&v, a + 32, 8); return v; }
static unsigned long long fx_xmm(const unsigned char *a, int i, int hi) {
    unsigned long long v; memcpy(&v, a + 160 + i * 16 + (hi ? 8 : 0), 8); return v;
}

static void fill_patterns(void) {
    for (int i = 0; i < 16; i++) {
        g_ctx.pat[i * 2 + 0] = 0x00C0DE0000000000ULL | (unsigned long long)(0x11 + i);
        g_ctx.pat[i * 2 + 1] = 0x110000000000BEEFULL ^ ((unsigned long long)(i + 1) << 32);
    }
    for (int i = 0; i < 16; i++) {
        // The upper half is distinct from the lower half AND from every other
        // register, so "inherited only what fxsave64 can see" (lower half
        // right, upper half zero) reads differently from "inherited nothing".
        g_ctx.ypat[i * 4 + 0] = g_ctx.pat[i * 2 + 0];
        g_ctx.ypat[i * 4 + 1] = g_ctx.pat[i * 2 + 1];
        g_ctx.ypat[i * 4 + 2] = 0x00A7E00000000000ULL | (unsigned long long)(0x51 + i);
        g_ctx.ypat[i * 4 + 3] = 0x77770000000FACEDULL ^ ((unsigned long long)(i + 1) << 40);
    }
    g_ctx.dbl = WANT_DBL;
    g_ctx.mx  = WANT_MXCSR;
    g_ctx.cw  = (unsigned short)WANT_CW;
}

#define SET_STATE                                   \
    "fninit\n\t"                                    \
    "fldcw    %c[cwo](%[c])\n\t"                    \
    "ldmxcsr  %c[mxo](%[c])\n\t"                    \
    "fldl     %c[dblo](%[c])\n\t"                   \
    "movdqu   %c[pto]+0(%[c]),   %%xmm0\n\t"        \
    "movdqu   %c[pto]+16(%[c]),  %%xmm1\n\t"        \
    "movdqu   %c[pto]+32(%[c]),  %%xmm2\n\t"        \
    "movdqu   %c[pto]+48(%[c]),  %%xmm3\n\t"        \
    "movdqu   %c[pto]+64(%[c]),  %%xmm4\n\t"        \
    "movdqu   %c[pto]+80(%[c]),  %%xmm5\n\t"        \
    "movdqu   %c[pto]+96(%[c]),  %%xmm6\n\t"        \
    "movdqu   %c[pto]+112(%[c]), %%xmm7\n\t"        \
    "movdqu   %c[pto]+128(%[c]), %%xmm8\n\t"        \
    "movdqu   %c[pto]+144(%[c]), %%xmm9\n\t"        \
    "movdqu   %c[pto]+160(%[c]), %%xmm10\n\t"       \
    "movdqu   %c[pto]+176(%[c]), %%xmm11\n\t"       \
    "movdqu   %c[pto]+192(%[c]), %%xmm12\n\t"       \
    "movdqu   %c[pto]+208(%[c]), %%xmm13\n\t"       \
    "movdqu   %c[pto]+224(%[c]), %%xmm14\n\t"       \
    "movdqu   %c[pto]+240(%[c]), %%xmm15\n\t"

#define OFFS                                        \
    [cwo]"i"(O(cw)), [mxo]"i"(O(mx)), [dblo]"i"(O(dbl)), [pto]"i"(O(pat)), \
    [pbo]"i"(O(pbuf)), [cbo]"i"(O(cbuf))

#define XMM_CLOBBER \
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7", \
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15"

// fork(): both arms return to C. The child has its own COW copy of this
// address space, so it reports its own cbuf and the parent never sees it.
static long fork_probe(void) {
    long ret;
    __asm__ __volatile__(
        SET_STATE
        "mov      $1, %%eax\n\t"                /* SYS_FORK */
        "syscall\n\t"
        "test     %%rax, %%rax\n\t"
        "jz       1f\n\t"
        "fxsave64 %c[pbo](%[c])\n\t"
        "jmp      2f\n"
        "1:\n\t"
        "fxsave64 %c[cbo](%[c])\n"
        "2:\n\t"
        : "=a"(ret)
        : [c]"r"(&g_ctx), OFFS
        : "rcx", "r11", "memory", XMM_CLOBBER
    );
    return ret;
}

// clone(): the child runs on a brand-new stack the compiler knows nothing
// about, so it must not execute one instruction of C. It writes its image,
// raises the flag (the address space is shared, so the parent sees both), and
// exits, all inside the asm.
static long clone_probe(void) {
    long ret;
    __asm__ __volatile__(
        SET_STATE
        "mov      %c[flo](%[c]), %%rdi\n\t"     /* arg1: flags     */
        "mov      %c[spo](%[c]), %%rsi\n\t"     /* arg2: new stack */
        "xor      %%edx, %%edx\n\t"             /* arg3: ptid      */
        "xor      %%r10d, %%r10d\n\t"           /* arg4: ctid      */
        "xor      %%r8d, %%r8d\n\t"             /* arg5: tls       */
        "mov      $110, %%eax\n\t"              /* SYS_CLONE */
        "syscall\n\t"
        "test     %%rax, %%rax\n\t"
        "jz       1f\n\t"
        "fxsave64 %c[pbo](%[c])\n\t"
        "jmp      2f\n"
        "1:\n\t"
        "fxsave64 %c[cbo](%[c])\n\t"
        "movl     $1, %c[flgo](%[c])\n\t"
        "xor      %%edi, %%edi\n\t"
        "xor      %%eax, %%eax\n\t"             /* SYS_EXIT */
        "syscall\n\t"
        "hlt\n"                                 /* unreachable */
        "2:\n\t"
        : "=a"(ret)
        : [c]"r"(&g_ctx), OFFS, [flo]"i"(O(flags)), [spo]"i"(O(sp)),
          [flgo]"i"(O(flag))
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r10", "memory", XMM_CLOBBER
    );
    return ret;
}


// ===========================================================================
// #107 AVX ARM. Everything above uses fxsave64, which by construction cannot
// see YMM_Hi128 - the exact 128 bits #107 found the context switch used to
// drop. So on a machine where AVX is actually enabled, the SSE arm passing
// proves only that the LOWER half was inherited. This arm sets and reads the
// upper half directly with VEX instructions, so the answer is measured rather
// than inferred from the fact that fpu_save_live() shares a branch with the
// switch.
//
// Every instruction below #UDs unless CR4.OSXSAVE is set and XCR0[2:1]=11b, so
// it runs only behind avx_usable(). On a kvm64 VM (no XSAVE, no AVX, and
// cpu/sse.c then CLEARS CR4.OSXSAVE by construction) this arm is skipped and
// says so, rather than faulting or quietly reporting a pass it did not earn.
// ===========================================================================
static int avx_usable(void) {
    unsigned int a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1), "c"(0));
    (void)b; (void)d;
    if (!(c & (1u << 26))) return 0;   // XSAVE
    if (!(c & (1u << 27))) return 0;   // OSXSAVE (CR4.OSXSAVE set by the kernel)
    if (!(c & (1u << 28))) return 0;   // AVX
    unsigned int lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    (void)hi;
    return (lo & 0x6u) == 0x6u;        // XCR0 has SSE and AVX
}

#define YSET \
    "vmovdqu  %c[ypo]+0(%[c]), %%ymm0\n\t"        \
    "vmovdqu  %c[ypo]+32(%[c]), %%ymm1\n\t"        \
    "vmovdqu  %c[ypo]+64(%[c]), %%ymm2\n\t"        \
    "vmovdqu  %c[ypo]+96(%[c]), %%ymm3\n\t"        \
    "vmovdqu  %c[ypo]+128(%[c]), %%ymm4\n\t"        \
    "vmovdqu  %c[ypo]+160(%[c]), %%ymm5\n\t"        \
    "vmovdqu  %c[ypo]+192(%[c]), %%ymm6\n\t"        \
    "vmovdqu  %c[ypo]+224(%[c]), %%ymm7\n\t"        \
    "vmovdqu  %c[ypo]+256(%[c]), %%ymm8\n\t"        \
    "vmovdqu  %c[ypo]+288(%[c]), %%ymm9\n\t"        \
    "vmovdqu  %c[ypo]+320(%[c]), %%ymm10\n\t"        \
    "vmovdqu  %c[ypo]+352(%[c]), %%ymm11\n\t"        \
    "vmovdqu  %c[ypo]+384(%[c]), %%ymm12\n\t"        \
    "vmovdqu  %c[ypo]+416(%[c]), %%ymm13\n\t"        \
    "vmovdqu  %c[ypo]+448(%[c]), %%ymm14\n\t"        \
    "vmovdqu  %c[ypo]+480(%[c]), %%ymm15\n\t"
#define YDUMP_P \
    "vextractf128 $1, %%ymm0, %c[pyo]+0(%[c])\n\t"  \
    "vextractf128 $1, %%ymm1, %c[pyo]+16(%[c])\n\t"  \
    "vextractf128 $1, %%ymm2, %c[pyo]+32(%[c])\n\t"  \
    "vextractf128 $1, %%ymm3, %c[pyo]+48(%[c])\n\t"  \
    "vextractf128 $1, %%ymm4, %c[pyo]+64(%[c])\n\t"  \
    "vextractf128 $1, %%ymm5, %c[pyo]+80(%[c])\n\t"  \
    "vextractf128 $1, %%ymm6, %c[pyo]+96(%[c])\n\t"  \
    "vextractf128 $1, %%ymm7, %c[pyo]+112(%[c])\n\t"  \
    "vextractf128 $1, %%ymm8, %c[pyo]+128(%[c])\n\t"  \
    "vextractf128 $1, %%ymm9, %c[pyo]+144(%[c])\n\t"  \
    "vextractf128 $1, %%ymm10, %c[pyo]+160(%[c])\n\t"  \
    "vextractf128 $1, %%ymm11, %c[pyo]+176(%[c])\n\t"  \
    "vextractf128 $1, %%ymm12, %c[pyo]+192(%[c])\n\t"  \
    "vextractf128 $1, %%ymm13, %c[pyo]+208(%[c])\n\t"  \
    "vextractf128 $1, %%ymm14, %c[pyo]+224(%[c])\n\t"  \
    "vextractf128 $1, %%ymm15, %c[pyo]+240(%[c])\n\t"
#define YDUMP_C \
    "vextractf128 $1, %%ymm0, %c[cyo]+0(%[c])\n\t"  \
    "vextractf128 $1, %%ymm1, %c[cyo]+16(%[c])\n\t"  \
    "vextractf128 $1, %%ymm2, %c[cyo]+32(%[c])\n\t"  \
    "vextractf128 $1, %%ymm3, %c[cyo]+48(%[c])\n\t"  \
    "vextractf128 $1, %%ymm4, %c[cyo]+64(%[c])\n\t"  \
    "vextractf128 $1, %%ymm5, %c[cyo]+80(%[c])\n\t"  \
    "vextractf128 $1, %%ymm6, %c[cyo]+96(%[c])\n\t"  \
    "vextractf128 $1, %%ymm7, %c[cyo]+112(%[c])\n\t"  \
    "vextractf128 $1, %%ymm8, %c[cyo]+128(%[c])\n\t"  \
    "vextractf128 $1, %%ymm9, %c[cyo]+144(%[c])\n\t"  \
    "vextractf128 $1, %%ymm10, %c[cyo]+160(%[c])\n\t"  \
    "vextractf128 $1, %%ymm11, %c[cyo]+176(%[c])\n\t"  \
    "vextractf128 $1, %%ymm12, %c[cyo]+192(%[c])\n\t"  \
    "vextractf128 $1, %%ymm13, %c[cyo]+208(%[c])\n\t"  \
    "vextractf128 $1, %%ymm14, %c[cyo]+224(%[c])\n\t"  \
    "vextractf128 $1, %%ymm15, %c[cyo]+240(%[c])\n\t"

#define YOFFS [ypo]"i"(O(ypat)), [pyo]"i"(O(pyhi)), [cyo]"i"(O(cyhi))

static long fork_probe_avx(void) {
    long ret;
    __asm__ __volatile__(
        "fninit\n\t"
        "fldcw    %c[cwo](%[c])\n\t"
        "ldmxcsr  %c[mxo](%[c])\n\t"
        "fldl     %c[dblo](%[c])\n\t"
        YSET
        "mov      $1, %%eax\n\t"                /* SYS_FORK */
        "syscall\n\t"
        "test     %%rax, %%rax\n\t"
        "jz       1f\n\t"
        "fxsave64 %c[pbo](%[c])\n\t"
        YDUMP_P
        "jmp      2f\n"
        "1:\n\t"
        "fxsave64 %c[cbo](%[c])\n\t"
        YDUMP_C
        "2:\n\t"
        : "=a"(ret)
        : [c]"r"(&g_ctx), OFFS, YOFFS
        : "rcx", "r11", "memory", XMM_CLOBBER
    );
    return ret;
}

static long clone_probe_avx(void) {
    long ret;
    __asm__ __volatile__(
        "fninit\n\t"
        "fldcw    %c[cwo](%[c])\n\t"
        "ldmxcsr  %c[mxo](%[c])\n\t"
        "fldl     %c[dblo](%[c])\n\t"
        YSET
        "mov      %c[flo](%[c]), %%rdi\n\t"
        "mov      %c[spo](%[c]), %%rsi\n\t"
        "xor      %%edx, %%edx\n\t"
        "xor      %%r10d, %%r10d\n\t"
        "xor      %%r8d, %%r8d\n\t"
        "mov      $110, %%eax\n\t"              /* SYS_CLONE */
        "syscall\n\t"
        "test     %%rax, %%rax\n\t"
        "jz       1f\n\t"
        "fxsave64 %c[pbo](%[c])\n\t"
        YDUMP_P
        "jmp      2f\n"
        "1:\n\t"
        "fxsave64 %c[cbo](%[c])\n\t"
        YDUMP_C
        "movl     $1, %c[flgo](%[c])\n\t"
        "xor      %%edi, %%edi\n\t"
        "xor      %%eax, %%eax\n\t"             /* SYS_EXIT */
        "syscall\n\t"
        "hlt\n"
        "2:\n\t"
        : "=a"(ret)
        : [c]"r"(&g_ctx), OFFS, YOFFS, [flo]"i"(O(flags)), [spo]"i"(O(sp)),
          [flgo]"i"(O(flag))
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r10", "memory", XMM_CLOBBER
    );
    return ret;
}

// Judge the upper halves. `hi` is the 256-byte vextractf128 dump.
static void judge_ymm(const char *arm, const unsigned char *hi) {
    char d[128];
    int bad = -1;
    for (int i = 0; i < 16; i++) {
        unsigned long long a0, a1;
        memcpy(&a0, hi + i * 16 + 0, 8);
        memcpy(&a1, hi + i * 16 + 8, 8);
        if (a0 != g_ctx.ypat[i * 4 + 2] || a1 != g_ctx.ypat[i * 4 + 3]) { bad = i; break; }
    }
    snprintf(d, sizeof(d), bad < 0 ? "%s all 16 upper halves match"
                                   : "%s first mismatch at ymm%d upper half",
             arm, bad);
    check("ymm0-15-hi128", bad < 0, d);
}

static void dump_ymm(const char *tag, const unsigned char *hi) {
    for (int i = 0; i < 16; i += 4) {
        unsigned long long v[8];
        for (int k = 0; k < 8; k++) memcpy(&v[k], hi + (i + k / 2) * 16 + (k % 2) * 8, 8);
        printf("[FPUT110] %s ymm%dhi=%016llx:%016llx ymm%dhi=%016llx:%016llx "
               "ymm%dhi=%016llx:%016llx ymm%dhi=%016llx:%016llx\n", tag,
               i + 0, v[1], v[0], i + 1, v[3], v[2],
               i + 2, v[5], v[4], i + 3, v[7], v[6]);
    }
}
// ---------------------------------------------------------------------------
static void dump(const char *tag, const unsigned char *a) {
    printf("[FPUT110] %s fcw=%04x mxcsr=%08x st0=%016llx\n",
           tag, (unsigned)fx_fcw(a), fx_mxcsr(a), (unsigned long long)fx_st0(a));
    for (int i = 0; i < 16; i += 4) {
        printf("[FPUT110] %s xmm%d=%016llx:%016llx xmm%d=%016llx:%016llx "
               "xmm%d=%016llx:%016llx xmm%d=%016llx:%016llx\n", tag,
               i + 0, (unsigned long long)fx_xmm(a, i + 0, 1), (unsigned long long)fx_xmm(a, i + 0, 0),
               i + 1, (unsigned long long)fx_xmm(a, i + 1, 1), (unsigned long long)fx_xmm(a, i + 1, 0),
               i + 2, (unsigned long long)fx_xmm(a, i + 2, 1), (unsigned long long)fx_xmm(a, i + 2, 0),
               i + 3, (unsigned long long)fx_xmm(a, i + 3, 1), (unsigned long long)fx_xmm(a, i + 3, 0));
    }
}

static void check(const char *what, int ok, const char *detail) {
    if (!ok) g_fail++;
    printf("[FPUT110] %s %s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
}

// Judge one image against the state the parent set. Used for the clone arm
// (shared memory lets the parent see both) and, in the fork child, against
// itself; the fork arm's parent/child comparison is done by eye on serial
// because the two images live in two different address spaces.
static void judge(const char *arm, const unsigned char *a) {
    char d[128];
    snprintf(d, sizeof(d), "%s got=%04x want=%04x", arm, (unsigned)fx_fcw(a), WANT_CW);
    check("x87-control-word", fx_fcw(a) == (unsigned short)WANT_CW, d);

    snprintf(d, sizeof(d), "%s got=%08x want=%08x", arm, fx_mxcsr(a) & 0xFFFFu, WANT_MXCSR);
    check("mxcsr", (fx_mxcsr(a) & 0xFFFFu) == WANT_MXCSR, d);

    snprintf(d, sizeof(d), "%s st0=%016llx (0 = empty x87 stack = reset)",
             arm, (unsigned long long)fx_st0(a));
    check("x87-st0", fx_st0(a) != 0, d);

    int bad = -1;
    for (int i = 0; i < 16; i++) {
        if (fx_xmm(a, i, 0) != g_ctx.pat[i * 2 + 0] ||
            fx_xmm(a, i, 1) != g_ctx.pat[i * 2 + 1]) { bad = i; break; }
    }
    snprintf(d, sizeof(d), bad < 0 ? "%s all 16 match" : "%s first mismatch at xmm%d",
             arm, bad);
    check("xmm0-15", bad < 0, d);
}

int main(void) {
    fill_patterns();
    printf("[FPUT110] start pid=%d want fcw=%04x mxcsr=%08x st0=%016llx\n",
           (int)getpid(), (unsigned)WANT_CW, WANT_MXCSR,
           (unsigned long long)WANT_DBL);
    printf("[FPUT110] a RESET child would report fcw=037f mxcsr=00001f80 st0=0 xmm all 0\n");

    // ---- ARM 1: fork() -----------------------------------------------------
    memset(g_ctx.pbuf, 0, sizeof(g_ctx.pbuf));
    memset(g_ctx.cbuf, 0, sizeof(g_ctx.cbuf));
    long r = fork_probe();
    if (r == 0) {
        dump("fork-CHILD ", g_ctx.cbuf);
        judge("fork-CHILD", g_ctx.cbuf);
        printf("[FPUT110] ==== fork arm child-side fail=%d ====\n", g_fail);
        _exit(g_fail ? 1 : 0);
    }
    if (r < 0) {
        printf("[FPUT110] FAIL fork returned %ld\n", r);
        g_fail++;
    } else {
        dump("fork-PARENT", g_ctx.pbuf);
        printf("[FPUT110] fork parent: child pid=%ld (its own lines are above/below)\n", r);
    }
    usleep(1500000);   // let the child's lines land before the clone arm starts

    // ---- ARM 2: clone(), the pthread_create path ---------------------------
    memset(g_ctx.pbuf, 0, sizeof(g_ctx.pbuf));
    memset(g_ctx.cbuf, 0, sizeof(g_ctx.cbuf));
    g_ctx.flag = 0;
    g_ctx.flags = 0x00000100u    /* CLONE_VM      */
                | 0x00000200u    /* CLONE_FS      */
                | 0x00000400u    /* CLONE_FILES   */
                | 0x00000800u    /* CLONE_SIGHAND */
                | 0x00010000u;   /* CLONE_THREAD  */
    {
        unsigned long ssz = 64 * 1024;
        unsigned char *sb = (unsigned char *)malloc(ssz);
        if (!sb) {
            printf("[FPUT110] FAIL clone arm: no stack\n");
            g_fail++;
        } else {
            g_ctx.sp = (void *)(((unsigned long)(sb + ssz)) & ~0xFUL);
            long t = clone_probe();
            if (t <= 0) {
                printf("[FPUT110] FAIL clone returned %ld\n", t);
                g_fail++;
            } else {
                // Bounded wait for the child's image: a fixed number of fixed
                // sleeps, then give up and report what we have. No spin.
                for (int i = 0; i < 30 && !g_ctx.flag; i++) usleep(100000);
                if (!g_ctx.flag) {
                    printf("[FPUT110] FAIL clone child never wrote its image\n");
                    g_fail++;
                } else {
                    dump("clone-PARENT", g_ctx.pbuf);
                    dump("clone-CHILD ", g_ctx.cbuf);
                    judge("clone-CHILD", g_ctx.cbuf);
                }
            }
        }
    }

    // ---- ARM 3: the same two paths, but 256 bits wide (#107) ---------------
    if (!avx_usable()) {
        printf("[FPUT110] SKIP avx-arm: this CPU has no usable AVX "
               "(kvm64 masks it and cpu/sse.c then clears CR4.OSXSAVE), so "
               "YMM_Hi128 inheritance is NOT measured on this VM\n");
    } else {
        printf("[FPUT110] avx-arm: AVX usable, measuring YMM_Hi128 inheritance\n");
        memset(g_ctx.pbuf, 0, sizeof(g_ctx.pbuf));
        memset(g_ctx.cbuf, 0, sizeof(g_ctx.cbuf));
        memset(g_ctx.pyhi, 0, sizeof(g_ctx.pyhi));
        memset(g_ctx.cyhi, 0, sizeof(g_ctx.cyhi));
        long fr = fork_probe_avx();
        if (fr == 0) {
            dump("avxfork-CHILD ", g_ctx.cbuf);
            dump_ymm("avxfork-CHILD ", g_ctx.cyhi);
            judge("avxfork-CHILD", g_ctx.cbuf);
            judge_ymm("avxfork-CHILD", g_ctx.cyhi);
            printf("[FPUT110] ==== avx fork arm child-side fail=%d ====\n", g_fail);
            _exit(g_fail ? 1 : 0);
        }
        if (fr < 0) { printf("[FPUT110] FAIL avx fork returned %ld\n", fr); g_fail++; }
        else {
            dump("avxfork-PARENT", g_ctx.pbuf);
            dump_ymm("avxfork-PARENT", g_ctx.pyhi);
            judge_ymm("avxfork-PARENT", g_ctx.pyhi);   // the instrument itself
        }
        usleep(1500000);

        memset(g_ctx.pbuf, 0, sizeof(g_ctx.pbuf));
        memset(g_ctx.cbuf, 0, sizeof(g_ctx.cbuf));
        memset(g_ctx.pyhi, 0, sizeof(g_ctx.pyhi));
        memset(g_ctx.cyhi, 0, sizeof(g_ctx.cyhi));
        g_ctx.flag = 0;
        unsigned long ssz2 = 64 * 1024;
        unsigned char *sb2 = (unsigned char *)malloc(ssz2);
        if (!sb2) { printf("[FPUT110] FAIL avx clone arm: no stack\n"); g_fail++; }
        else {
            g_ctx.sp = (void *)(((unsigned long)(sb2 + ssz2)) & ~0xFUL);
            long t2 = clone_probe_avx();
            if (t2 <= 0) { printf("[FPUT110] FAIL avx clone returned %ld\n", t2); g_fail++; }
            else {
                for (int i = 0; i < 30 && !g_ctx.flag; i++) usleep(100000);
                if (!g_ctx.flag) {
                    printf("[FPUT110] FAIL avx clone child never wrote its image\n");
                    g_fail++;
                } else {
                    dump("avxclone-PARENT", g_ctx.pbuf);
                    dump_ymm("avxclone-PARENT", g_ctx.pyhi);
                    dump("avxclone-CHILD ", g_ctx.cbuf);
                    dump_ymm("avxclone-CHILD ", g_ctx.cyhi);
                    judge("avxclone-CHILD", g_ctx.cbuf);
                    judge_ymm("avxclone-CHILD", g_ctx.cyhi);
                }
            }
        }
    }

    printf("[FPUT110] ==== RESULT parent-side fail=%d ====\n", g_fail);
    return g_fail ? 1 : 0;
}
