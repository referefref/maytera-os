// exec/x86_32_test.c - the #740 32-bit core's correctness oracle, and the
// end-to-end demonstration that a 32-bit guest instruction sequence actually
// retires inside a booted kernel.
//
// WHY THIS FILE IS THE POINT OF THE WHOLE CHANGE
//
// This repository's characteristic failure is code that compiles, links, and
// never executes: #710 async_io, #712 virtio, graphfs's 72 declarations with 0
// implementations, validate_user_ptr, sse_save. `nm` proving a symbol is linked
// proves nothing at all. So the 32-bit core does not get to claim it works
// because it builds. It has to say what it did, out loud, on a serial line,
// from a booted kernel, and be checkable against something that is not itself.
//
// THAT SOMETHING IS REAL SILICON. Every expected value in x86_32_vectors.h was
// read out of an actual 32-bit protected-mode execution of the exact same
// instruction bytes on the build host, in x86-64 COMPATIBILITY MODE, which is
// the same architectural mode a DOS/4GW guest runs in. Nothing here encodes
// what the author believed the answer should be. See the header comment of
// tools/x86-32-oracle/oracle-gen.py, and note that kernel/exec/x86_16.c:4557's
// hand-written 386 self-test has a case whose own comment says
// "(informational)" because its author was not sure what SHLD should produce.
// That is the failure mode this replaces.
//
// THE SAME FILE RUNS ON THE HOST. Built with -DX86_32_HOST_TEST it links into
// tools/x86-32-oracle/host-check with stubs, so an interpreter bug can be found
// in seconds instead of a kernel build plus a VM boot. The kernel and the host
// checker therefore share ONE comparison implementation rather than two that
// can drift, which is the shared-primitive rule applied to a test.

#ifdef X86_32_HOST_TEST
// NO libc headers. kernel/types.h typedefs size_t as `unsigned long long`,
// which collides with glibc's `unsigned long`, so including <stdlib.h> here
// would not compile. tools/x86-bench hit the same wall from the other side
// (blame.md #740 trap 4) and the answer is the same: declare the handful of
// host functions used, and keep the file under measurement unmodified.
extern int   printf(const char *fmt, ...);
extern void *malloc(unsigned long n);
extern void  free(void *p);
extern void *memset(void *d, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);
extern int   memcmp(const void *a, const void *b, unsigned long n);
#include "x86_32.h"
#include "x86_32_vectors.h"
#include "x86_32_hello.h"
#include "x86_32_miss.h"
#define KP(...)      printf(__VA_ARGS__)
#define BOOTLOG(...) ((void)0)
static void *x32_alloc(unsigned long n) { return malloc(n); }
static void  x32_free(void *p)          { free(p); }
static void  x32_zero(void *p, unsigned long n) { memset(p, 0, n); }
static void  x32_copy(void *d, const void *s, unsigned long n) { memcpy(d, s, n); }
static int   x32_diff(const void *a, const void *b, unsigned long n) { return memcmp(a, b, n); }
#else
#include "x86_32.h"
#include "x86_32_vectors.h"
#include "x86_32_hello.h"
#include "x86_32_miss.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/bootlog.h"
#define KP(...)      kprintf(__VA_ARGS__)
#define BOOTLOG(...) bootlog_write(__VA_ARGS__)
static void *x32_alloc(unsigned long n) { return kmalloc((size_t)n); }
static void  x32_free(void *p)          { kfree(p); }
static void  x32_zero(void *p, unsigned long n) { memset(p, 0, (size_t)n); }
static void  x32_copy(void *d, const void *s, unsigned long n) { memcpy(d, s, (size_t)n); }
static int   x32_diff(const void *a, const void *b, unsigned long n) { return memcmp(a, b, (size_t)n); }
#endif

static const char *x32_reg_name(int i) {
    static const char *n[8] = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };
    return n[i & 7];
}

static const char *x32_exit_name(uint32_t r) {
    switch (r) {
        case X32_EXIT_BUDGET:      return "BUDGET";
        case X32_EXIT_STOP_EIP:    return "STOP_EIP";
        case X32_EXIT_INT:         return "INT";
        case X32_EXIT_HLT:         return "HLT";
        case X32_EXIT_IO_IN:       return "IO_IN";
        case X32_EXIT_IO_OUT:      return "IO_OUT";
        case X32_EXIT_MISS:        return "MISS";
        case X32_EXIT_FAULT_UD:    return "FAULT_UD";
        case X32_EXIT_FAULT_MEM:   return "FAULT_MEM";
        case X32_EXIT_FAULT_DIV:   return "FAULT_DIV";
        case X32_EXIT_FAULT_LIMIT: return "FAULT_LIMIT";
        default:                   return "?";
    }
}

// Report the first differing byte of a window rather than only its existence.
// A test that says "scratch differs" and stops has thrown away the one fact
// that would have told you which instruction was wrong.
static int x32_report_window(const char *what, uint32_t base,
                             const uint8_t *got, const uint8_t *want,
                             uint32_t len) {
    uint32_t i;
    int shown = 0;
    for (i = 0; i < len; i++) {
        if (got[i] == want[i]) continue;
        if (shown < 8) {
            KP("        %s+0x%02X  got %02X  silicon %02X  (linear 0x%08X)\n",
               what, i, got[i], want[i], base + i);
        }
        shown++;
    }
    if (shown > 8) KP("        ... and %d more %s bytes differ\n", shown - 8, what);
    return shown;
}

int x86_32_oracle_selftest(void) {
    uint32_t vi;
    int bad_vectors = 0;
    uint32_t rust_size = x86_32_cpu_size();

    // The layout lock that a header alone cannot provide: this number comes out
    // of rustc, the sizeof comes out of gcc, and a struct that drifted on one
    // side reaches here rather than reaching a guest.
    if (rust_size != (uint32_t)sizeof(x86_32_cpu_t)) {
        KP("[X8632] ABI MISMATCH: rustc sizeof(X8632Cpu)=%u, gcc sizeof(x86_32_cpu_t)=%u."
           " Refusing to run the oracle: every field offset below is suspect.\n",
           rust_size, (uint32_t)sizeof(x86_32_cpu_t));
        BOOTLOG("[X8632] ABI MISMATCH rust=%u c=%u", rust_size,
                (uint32_t)sizeof(x86_32_cpu_t));
        return -1;
    }

    for (vi = 0; vi < X86_32_VECTOR_COUNT; vi++) {
        const x86_32_vector_t *v = &g_x86_32_vectors[vi];
        x86_32_cpu_t cpu;
        uint8_t *arena;
        uint32_t r, i;
        int fails = 0;
        uint8_t got_scratch[X86_32_VEC_SCRATCH_LEN];
        uint8_t got_stack[X86_32_VEC_STACKWIN_LEN];

        arena = (uint8_t *)x32_alloc(v->arena_size);
        if (!arena) {
            KP("[X8632] %s: arena alloc of %u bytes FAILED\n", v->name, v->arena_size);
            bad_vectors++;
            continue;
        }
        x32_zero(arena, v->arena_size);
        x32_copy(arena + (v->code_base - v->arena_base), v->code, v->code_len);

        x86_32_init(&cpu, arena, v->arena_base, v->arena_size);
        cpu.eip = v->entry;
        cpu.stop_eip = v->stop;
        cpu.stop_eip_en = 1;
        // The native process starts with a flat DS/ES/SS at base 0. So does a
        // DOS/4GW guest, and so does this: seg_base is left all-zero by
        // x86_32_init and the selectors are cosmetic.
        //
        // The SELECTORS are not cosmetic once a vector reads one. `push %cs`
        // puts one in the compared stack window; an IRETD frame has to carry a
        // CS the SILICON will accept on the native arm; LSS/LDS/LES store a
        // selector and load it back. v->init_seg[] holds what the native
        // process actually ran under, read out of the run rather than assumed,
        // so both arms start from the same register file by construction
        // instead of from a hardcoded 0x23.
        for (i = 0; i < 6; i++) cpu.seg[i] = v->init_seg[i];

        // 2,000,000 is far more than any vector needs (the largest is a bubble
        // sort over 16 bytes) and far less than a hang. A vector that hits the
        // budget has looped, and that is reported as a distinct failure rather
        // than being silently compared against a half-finished state.
        r = x86_32_run(&cpu, 2000000ULL);

        if (r != X32_EXIT_STOP_EIP) {
            KP("[X8632] FAIL %-18s did not reach the end: exit=%s eip=0x%08X",
               v->name, x32_exit_name(r), cpu.eip);
            if (r == X32_EXIT_MISS) {
                KP(" MISS op=%02X op2=%03X modrm=%03X len=%u",
                   cpu.miss_op, cpu.miss_op2, cpu.miss_modrm, cpu.miss_len);
            } else if (r == X32_EXIT_FAULT_MEM) {
                KP(" addr=0x%08X (window 0x%08X..0x%08X)",
                   cpu.fault_addr, v->arena_base, v->arena_base + v->arena_size);
            }
            KP(" insns=%u\n", (uint32_t)cpu.insn_count);
            bad_vectors++;
            x32_free(arena);
            continue;
        }

        for (i = 0; i < 8; i++) {
            if (cpu.regs[i] != v->exp_regs[i]) {
                KP("[X8632] FAIL %-18s %s got 0x%08X  silicon 0x%08X\n",
                   v->name, x32_reg_name((int)i), cpu.regs[i], v->exp_regs[i]);
                fails++;
            }
        }
        {
            uint32_t gf = cpu.eflags & v->flag_mask;
            uint32_t wf = v->exp_eflags & v->flag_mask;
            if (gf != wf) {
                KP("[X8632] FAIL %-18s EFLAGS got 0x%04X silicon 0x%04X (mask 0x%04X, differ 0x%04X)\n",
                   v->name, gf, wf, v->flag_mask, gf ^ wf);
                if (v->mask_reason) KP("        mask reason: %s\n", v->mask_reason);
                fails++;
            }
        }
        if (x86_32_read_guest(&cpu, v->scratch_addr, got_scratch, X86_32_VEC_SCRATCH_LEN) != 0) {
            KP("[X8632] FAIL %-18s scratch window 0x%08X is outside the arena\n",
               v->name, v->scratch_addr);
            fails++;
        } else if (x32_diff(got_scratch, v->exp_scratch, X86_32_VEC_SCRATCH_LEN) != 0) {
            KP("[X8632] FAIL %-18s scratch window differs:\n", v->name);
            x32_report_window("scratch", v->scratch_addr, got_scratch,
                              v->exp_scratch, X86_32_VEC_SCRATCH_LEN);
            fails++;
        }
        if (x86_32_read_guest(&cpu, v->stackwin_addr, got_stack, X86_32_VEC_STACKWIN_LEN) != 0) {
            KP("[X8632] FAIL %-18s stack window 0x%08X is outside the arena\n",
               v->name, v->stackwin_addr);
            fails++;
        } else if (x32_diff(got_stack, v->exp_stackwin, X86_32_VEC_STACKWIN_LEN) != 0) {
            KP("[X8632] FAIL %-18s stack window differs:\n", v->name);
            x32_report_window("stack", v->stackwin_addr, got_stack,
                              v->exp_stackwin, X86_32_VEC_STACKWIN_LEN);
            fails++;
        }

        if (fails) bad_vectors++;
        x32_free(arena);
    }

    KP("[X8632] oracle: %u vectors vs native 32-bit execution, %d mismatched -> %s\n",
       (uint32_t)X86_32_VECTOR_COUNT, bad_vectors, bad_vectors ? "FAIL" : "PASS");
    BOOTLOG("[X8632] oracle %u vectors, %d mismatched -> %s",
            (uint32_t)X86_32_VECTOR_COUNT, bad_vectors, bad_vectors ? "FAIL" : "PASS");
    return bad_vectors;
}

// ---------------------------------------------------------------------------
// The MISS policy, demonstrated rather than described.
//
// blame.md, 2026-08-07 (#740): "an emulated opcode with a stack side effect must
// be implemented or must MISS loudly. It must never be a silent no-op. A MISS
// is diagnosable; a skipped push is not." x86_16.c takes unimplemented D8..DF
// forms as no-ops "to keep IP aligned", and the result is not one wrong number:
// FLD's push and FCOMP's pop go missing, the emulated FP stack depth stops
// matching the guest's, and every later FP op touches the wrong register.
//
// This core reports the opcode and its TRUE LENGTH and leaves EIP on the
// instruction. That claim is only worth anything if the length is right, so the
// expected lengths in x86_32_miss.h are the assembler's own label offsets. A
// caller that skips by miss_len must land exactly on the next instruction, and
// this checks that it does, by skipping all six and requiring the walk to end
// on the byte after the last one.
// ---------------------------------------------------------------------------
int x86_32_miss_selftest(void) {
    x86_32_cpu_t cpu;
    uint8_t *arena;
    uint32_t i;
    int bad = 0;
    const uint32_t ARENA_BASE = X86_32_MISS_BASE & ~0xFFFu;
    const uint32_t ARENA_SIZE = 0x2000u;

    arena = (uint8_t *)x32_alloc(ARENA_SIZE);
    if (!arena) { KP("[X8632] miss: arena alloc FAILED\n"); return -1; }
    x32_zero(arena, ARENA_SIZE);
    x32_copy(arena + (X86_32_MISS_BASE - ARENA_BASE),
             g_x86_32_miss_code, X86_32_MISS_CODE_LEN);

    x86_32_init(&cpu, arena, ARENA_BASE, ARENA_SIZE);

    for (i = 0; i < X86_32_MISS_CASE_COUNT; i++) {
        const x86_32_miss_case_t *c = &g_x86_32_miss_cases[i];
        uint32_t r;
        cpu.eip = c->addr;
        cpu.miss_count = 0;
        r = x86_32_run(&cpu, 1);
        if (r != X32_EXIT_MISS) {
            KP("[X8632] FAIL miss %-10s at 0x%08X: exit=%s, expected MISS\n",
               c->text, c->addr, x32_exit_name(r));
            bad++;
            continue;
        }
        if (cpu.eip != c->addr) {
            KP("[X8632] FAIL miss %-10s left EIP at 0x%08X, not on the instruction (0x%08X)\n",
               c->text, cpu.eip, c->addr);
            bad++;
        }
        if (cpu.miss_len != c->len) {
            KP("[X8632] FAIL miss %-10s reported len=%u, assembler says %u."
               " A caller skipping by that lands mid-instruction.\n",
               c->text, cpu.miss_len, c->len);
            bad++;
        }
        if (cpu.miss_count != 1) {
            KP("[X8632] FAIL miss %-10s miss_count=%u, expected 1\n", c->text, cpu.miss_count);
            bad++;
        }
    }

    // Now the property that matters to a caller: skipping by miss_len walks the
    // whole block and lands exactly on the byte after it.
    {
        uint32_t eip = X86_32_MISS_BASE;
        uint32_t steps = 0;
        const uint32_t end = g_x86_32_miss_cases[X86_32_MISS_CASE_COUNT - 1].addr
                           + g_x86_32_miss_cases[X86_32_MISS_CASE_COUNT - 1].len;
        while (eip < end && steps < 32) {
            uint32_t r;
            cpu.eip = eip;
            r = x86_32_run(&cpu, 1);
            if (r != X32_EXIT_MISS) break;
            eip += cpu.miss_len;
            steps++;
        }
        if (eip != end || steps != X86_32_MISS_CASE_COUNT) {
            KP("[X8632] FAIL miss: skip-by-miss_len walk ended at 0x%08X after %u steps,"
               " expected 0x%08X after %u\n",
               eip, steps, end, (uint32_t)X86_32_MISS_CASE_COUNT);
            bad++;
        }
    }

    KP("[X8632] miss: %u unimplemented opcodes reported with the assembler's own"
       " lengths, EIP left on the instruction, skip-walk exact -> %s\n",
       (uint32_t)X86_32_MISS_CASE_COUNT, bad ? "FAIL" : "PASS");
    BOOTLOG("[X8632] miss %u cases -> %s", (uint32_t)X86_32_MISS_CASE_COUNT,
            bad ? "FAIL" : "PASS");
    x32_free(arena);
    return bad;
}

// ---------------------------------------------------------------------------
// The end-to-end demonstration.
//
// x86_32_hello.h holds a real assembled 32-bit protected-mode program, linked
// flat at a base above the low megabyte the way an LE object is. It prints a
// $-terminated string through INT 21h AH=09h, does a small loop so the retired
// count is not trivially 5, and exits through INT 21h AH=4Ch.
//
// THIS IS NOT AN INT 21h IMPLEMENTATION AND MUST NOT BECOME ONE.
// docs/DOS4GW_DESIGN.md section 6 states the constraint: there is exactly one
// INT 21h implementation, dos_svc_int21() in kernel/dos/int21svc.c, and the
// DPMI bridge attaches to it as a third caller. What follows recognises exactly
// two register states and prints from guest memory; any other AH value is a
// FAILURE OF THIS SELF-TEST, not a function to add here. If you find yourself
// wanting to add a case, you are writing the bridge, and the bridge belongs in
// the DPMI host next to a dos_svc_ctx_t, not in a test file. #713 spent real
// effort deleting the second INT 21h; this is where a third one would start.
// ---------------------------------------------------------------------------
int x86_32_hello_selftest(void) {
    x86_32_cpu_t cpu;
    uint8_t *arena;
    uint32_t r;
    int printed = 0, exited = 0, rc = 0;
    uint32_t guard = 0;

    arena = (uint8_t *)x32_alloc(X86_32_HELLO_ARENA_SIZE);
    if (!arena) {
        KP("[X8632] hello: arena alloc FAILED\n");
        return -1;
    }
    x32_zero(arena, X86_32_HELLO_ARENA_SIZE);
    x32_copy(arena + (X86_32_HELLO_BASE - X86_32_HELLO_ARENA_BASE),
             g_x86_32_hello_code, X86_32_HELLO_CODE_LEN);

    x86_32_init(&cpu, arena, X86_32_HELLO_ARENA_BASE, X86_32_HELLO_ARENA_SIZE);
    cpu.eip = X86_32_HELLO_ENTRY;
    // A real LE takes SS:ESP from its header; here the host places the stack at
    // the top of the arena, which is the same decision made in the same place.
    cpu.regs[X32_ESP] = X86_32_HELLO_ARENA_BASE + X86_32_HELLO_ARENA_SIZE - 16;

    for (;;) {
        if (++guard > 64) {   // bounded: no unbounded poll, #426
            KP("[X8632] hello: too many guest exits, giving up\n");
            rc = -1;
            break;
        }
        r = x86_32_run(&cpu, 1000000ULL);
        if (r == X32_EXIT_INT && cpu.exit_arg == 0x21) {
            uint32_t ah = (cpu.regs[X32_EAX] >> 8) & 0xFF;
            if (ah == 0x09) {
                // DS:EDX, flat, $-terminated. Read through the CHECKED accessor:
                // the string length is guest-controlled and so is the pointer.
                char buf[128];
                uint32_t n = 0;
                uint32_t la = cpu.seg_base[X32_DS] + cpu.regs[X32_EDX];
                while (n < sizeof(buf) - 1) {
                    uint8_t ch;
                    if (x86_32_read_guest(&cpu, la + n, &ch, 1) != 0) break;
                    if (ch == '$') break;
                    buf[n++] = (char)ch;
                }
                buf[n] = 0;
                KP("[X8632] guest INT 21h AH=09h at EIP=0x%08X EDX=0x%08X: \"%s\"\n",
                   cpu.eip, cpu.regs[X32_EDX], buf);
                printed = 1;
                continue;
            }
            if (ah == 0x4C) {
                exited = 1;
                KP("[X8632] guest INT 21h AH=4Ch exit code %u, EIP=0x%08X\n",
                   cpu.regs[X32_EAX] & 0xFF, cpu.eip);
                break;
            }
            KP("[X8632] hello: guest called INT 21h AH=%02Xh, which this SELF-TEST"
               " does not recognise. That is a test failure, not a missing service:"
               " services live in dos/int21svc.c.\n", ah);
            rc = -1;
            break;
        }
        KP("[X8632] hello: unexpected exit=%s arg=0x%X eip=0x%08X insns=%u\n",
           x32_exit_name(r), cpu.exit_arg, cpu.eip, (uint32_t)cpu.insn_count);
        if (r == X32_EXIT_MISS) {
            KP("        MISS op=%02X op2=%03X modrm=%03X len=%u\n",
               cpu.miss_op, cpu.miss_op2, cpu.miss_modrm, cpu.miss_len);
        }
        rc = -1;
        break;
    }

    // The program stores its loop result; reading it back proves the guest's
    // memory writes landed, not just that it reached the interrupt.
    {
        uint32_t sum = 0;
        uint8_t b[4];
        if (x86_32_read_guest(&cpu, X86_32_HELLO_SUM_ADDR, b, 4) == 0) {
            sum = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
        KP("[X8632] hello: retired %u guest instructions, guest-computed sum=%u"
           " (expected %u), printed=%d exited=%d -> %s\n",
           (uint32_t)cpu.insn_count, sum, (uint32_t)X86_32_HELLO_EXPECT_SUM,
           printed, exited,
           (printed && exited && sum == (uint32_t)X86_32_HELLO_EXPECT_SUM && !rc)
               ? "PASS" : "FAIL");
        BOOTLOG("[X8632] hello insns=%u sum=%u printed=%d exited=%d",
                (uint32_t)cpu.insn_count, sum, printed, exited);
        if (!(printed && exited && sum == (uint32_t)X86_32_HELLO_EXPECT_SUM)) rc = -1;
    }

    x32_free(arena);
    return rc;
}
