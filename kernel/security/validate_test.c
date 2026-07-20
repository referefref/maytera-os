// validate_test.c - #500 / MAYTERA-SEC-2026-0016 negative-control battery.
//
// WHY THIS EXISTS, AND WHY IT IS SHAPED LIKE THIS
//
// A pointer validator that accepts everything passes every positive test. The
// only tests with any information content are the ones that must be REJECTED.
// So this battery is deliberately mostly negative controls, and each one names
// the concrete attack it stands for.
//
// It runs IN THE CALLING RING-3 PROCESS'S ADDRESS SPACE, invoked from a syscall,
// because that is the only context where the thing under test (a walk of the
// caller's CR3) means anything. Nothing here is mocked: real page tables, real
// CR3, real user pages. blame.md records a negative-control rig in this very
// tree that was structurally vacuous (an include path that could never win), so
// the rig deliberately proves it can FAIL: case P1 is a positive control and
// case N9 is a self-check that the battery is actually reaching the validator.

#include "validate.h"
#include "../mm/vmm.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../proc/process.h"
#include "../proc/syscall_argtab.h"   // #503: the dispatcher choke point under test

// Linker-provided kernel image bounds (linker.ld).
extern char __kernel_end[];

#define KERNEL_TEXT_ADDR   0x400000ULL   // KERNEL_PHYS_BASE from linker.ld

typedef struct {
    const char *name;
    const char *attack;      // what a real attacker gets if this is accepted
    bool must_accept;        // true = positive control, false = must be rejected
    validate_error_t got;
} vt_case_t;

static void vt_report(vt_case_t *c, int *pass, int *fail) {
    bool accepted = (c->got == VALIDATE_OK);
    bool ok = (accepted == c->must_accept);
    if (ok) (*pass)++; else (*fail)++;
    // NOTE: kprintf() does NOT implement width/padding specifiers like %-4s;
    // it prints them literally and then mis-consumes the varargs. Plain %s only.
    kprintf("[SECTEST] %s %s want=%s got=%s %s\n",
            ok ? "PASS" : "FAIL",
            c->name,
            c->must_accept ? "ACCEPT" : "REJECT",
            validate_error_string(c->got),
            ok ? "" : " <<<< SECURITY HOLE");
    if (!ok && !c->must_accept) {
        kprintf("[SECTEST]      ^ ACCEPTED A POINTER IT MUST REJECT: %s\n", c->attack);
    }
}

// Dump the effective hardware rights of an address in the caller's CR3. This is
// the ground truth the validator is supposed to agree with.
static void vt_dump_eff(const char *what, uint64_t cr3, uint64_t addr) {
    uint64_t eff = vmm_get_effective_flags_in(cr3, addr);
    kprintf("[SECTEST]   %s va=0x%lx present=%d user=%d write=%d\n",
            what, addr,
            (eff & VMM_FLAG_PRESENT) ? 1 : 0,
            (eff & VMM_FLAG_USER) ? 1 : 0,
            (eff & VMM_FLAG_WRITABLE) ? 1 : 0);
}


// ===========================================================================
// #503 ARGTAB NEGATIVE CONTROLS.
//
// validate_selftest() above proves the VALIDATOR rejects what it must. That is
// necessary and not sufficient: #503 is the claim that the DISPATCHER now calls
// it, for the right arguments, with the right lengths. A perfect validator that
// the table points at the wrong argument, or hands the wrong length, protects
// nothing while looking exactly like protection.
//
// So these cases drive syscall_validate_args() itself, by real syscall number,
// exactly as the SYSCALL entry does. There is one negative control per
// DESCRIPTOR KIND, because a kind is the unit that can be wrong:
//
//   Kind::W + Len::Fixed   -> SYS_FB_INFO(201)    arg1 = kernel text
//   Kind::W + Len::Elems   -> SYS_PROC_LIST(238)  arg1 = ok, count overflows
//   Kind::W + Len::Arg     -> SYS_GETCWD(99)      arg1 = ok, len runs off
//   Kind::R + Len::Fixed   -> SYS_CRON_ADD(276)   arg1 = kernel text
//   Kind::R + Len::Arg     -> SYS_WRITE(13)       arg2 = kernel text (leak)
//   Kind::Str              -> SYS_OPEN(10)        arg1 = kernel text
//
// and matching positive controls, because a table that rejects everything would
// pass every negative case above and take the desktop with it.
// ===========================================================================

typedef struct {
    const char *name;
    const char *attack;
    bool must_accept;
    int64_t rc;          // 0 = accepted, -14 = EFAULT
} at_case_t;

static void at_report(at_case_t *c, int *pass, int *fail) {
    bool accepted = (c->rc == 0);
    bool ok = (accepted == c->must_accept);
    if (ok) (*pass)++; else (*fail)++;
    kprintf("[ARGTAB] %s %s want=%s got=%s %s\n",
            c->name, ok ? "PASS" : "FAIL",
            c->must_accept ? "accept" : "REJECT",
            accepted ? "accept" : "REJECT",
            c->attack);
    if (!ok && accepted) {
        kprintf("[ARGTAB]      ^ DISPATCHER LET THIS THROUGH: %s\n", c->attack);
    }
    if (!ok && !accepted) {
        kprintf("[ARGTAB]      ^ REJECTED A LEGITIMATE CALL - this breaks apps: %s\n",
                c->attack);
    }
}

int64_t argtab_selftest(void *ubuf, uint64_t ubuf_len) {
    int pass = 0, fail = 0;
    at_case_t c[48];
    int n = 0;
    const uint64_t KT = KERNEL_TEXT_ADDR;   // present=1, user=0 in the caller's CR3
    const uint64_t U = (uint64_t)ubuf;

    kprintf("\n[ARGTAB] ===== #503 dispatcher argtab controls (%u syscalls declared) =====\n",
            syscall_desc_count());

    // ---- Kind::W + Len::Fixed -------------------------------------------
    // SYS_FB_INFO writes a 24-byte fb_info_user_t through arg1. Ring 3 naming
    // kernel text here is a 24-byte arbitrary kernel write.
    c[n++] = (at_case_t){ "A-N1", "SYS_FB_INFO arg1 -> kernel text (24B arbitrary write)",
        false, syscall_validate_args(201, KT, 0, 0, 0, 0, 0) };
    c[n++] = (at_case_t){ "A-P1", "SYS_FB_INFO arg1 -> real user buffer",
        true, syscall_validate_args(201, U, 0, 0, 0, 0, 0) };
    // Judgement (1) in rustkern.rs: NULL is SKIPPED, not rejected, because many
    // handlers take optional out-params and NULL-check them themselves. If this
    // ever flips to REJECT, live callers start getting -EFAULT.
    c[n++] = (at_case_t){ "A-P2", "NULL is skipped, not rejected (optional out-params)",
        true, syscall_validate_args(201, 0, 0, 0, 0, 0, 0) };

    // ---- Kind::W + Len::Elems -------------------------------------------
    // SYS_PROC_LIST writes arg2 * 64 bytes through arg1. THE overflow case: if
    // count*elem_size wrapped, an enormous request would become a tiny check
    // and pass. checked_mul must turn it into EFAULT instead.
    c[n++] = (at_case_t){ "A-N2", "SYS_PROC_LIST count*64 OVERFLOWS u64 (wrap -> tiny check)",
        false, syscall_validate_args(238, U, 0x2000000000000000ULL, 0, 0, 0, 0) };
    // Valid base, count chosen to run off the end of the real buffer. This is
    // the class the base-only check cannot see.
    c[n++] = (at_case_t){ "A-N3", "SYS_PROC_LIST valid base + count runs off into unmapped",
        false, syscall_validate_args(238, U, 1000000, 0, 0, 0, 0) };
    c[n++] = (at_case_t){ "A-P3", "SYS_PROC_LIST base + count that fits",
        true, syscall_validate_args(238, U, (ubuf_len / 64), 0, 0, 0, 0) };

    // ---- Kind::W + Len::Arg ---------------------------------------------
    c[n++] = (at_case_t){ "A-N4", "SYS_GETCWD valid base + attacker length (64MB)",
        false, syscall_validate_args(99, U, 64ULL * 1024 * 1024, 0, 0, 0, 0) };
    c[n++] = (at_case_t){ "A-P4", "SYS_GETCWD base + honest length",
        true, syscall_validate_args(99, U, ubuf_len, 0, 0, 0, 0) };

    // ---- Kind::R + Len::Fixed -------------------------------------------
    c[n++] = (at_case_t){ "A-N5", "SYS_CRON_ADD arg1 -> kernel text (128B kernel READ)",
        false, syscall_validate_args(276, KT, 0, 0, 0, 0, 0) };

    // ---- Kind::R + Len::Arg ---------------------------------------------
    // An unvalidated READ is "only" an info leak, but it is a leak of whatever
    // the attacker names: here, kernel text out through a file descriptor.
    c[n++] = (at_case_t){ "A-N6", "SYS_WRITE arg2 -> kernel text (leak kernel out to a fd)",
        false, syscall_validate_args(13, 1, KT, 64, 0, 0, 0) };
    c[n++] = (at_case_t){ "A-P5", "SYS_WRITE from a real user buffer",
        true, syscall_validate_args(13, 1, U, 64, 0, 0, 0) };

    // ---- Kind::Str -------------------------------------------------------
    c[n++] = (at_case_t){ "A-N7", "SYS_OPEN path -> kernel text (string overread)",
        false, syscall_validate_args(10, KT, 0, 0, 0, 0, 0) };
    ((char *)ubuf)[0] = '/'; ((char *)ubuf)[1] = 'A'; ((char *)ubuf)[2] = '\0';
    c[n++] = (at_case_t){ "A-P6", "SYS_OPEN with a real user path string",
        true, syscall_validate_args(10, U, 0, 0, 0, 0, 0) };

    // ---- sx: the SANITIZE_USER_PTR shim (SYS_DECODE_IMAGE 253) -----------
    // The kind that carries the most risk this week: arg1 is attacker-supplied
    // image bytes going into the Ring-0 decoders (already the source of
    // MAYTERA-SEC-2026-0013, a JPEG heap OOB WRITE), and the RSS reader is
    // making that input REMOTE.
    //
    // All three of this handler's pointers are SANITIZE_USER_PTR'd, so the
    // table declares them sx() and validates the MASKED address. These two cases
    // are the whole justification for that decision, in both directions.
    const uint64_t U_SX = 0xFFFFFFFF00000000ULL | (U & 0xFFFFFFFFULL);
    const uint64_t KT_SX = 0xFFFFFFFF00000000ULL | (KT & 0xFFFFFFFFULL);
    // Direction 1: a sign-extended USER pointer is exactly what the browser
    // sends (user.ld loads apps at 0x80000000 = negative as int32). If the table
    // validated the raw arg, this legal call would get EFAULT and every inline
    // <img> would silently stop decoding.
    c[n++] = (at_case_t){ "A-P9", "DECODE_IMAGE sign-extended USER ptr accepted (browser's real call shape)",
        true, syscall_validate_args(253, U_SX, 64, 0, U, ubuf_len, U) };
    // Direction 2: and mirroring the shim must NOT become the bypass. A
    // sign-extended KERNEL TEXT pointer masks down to 0x400000, which is kernel
    // text in the caller's own CR3. If the mask were applied without re-checking
    // U/S, this would be a write straight into the kernel image.
    c[n++] = (at_case_t){ "A-N9", "DECODE_IMAGE sign-extended KERNEL TEXT still rejected (shim is not a bypass)",
        false, syscall_validate_args(253, U, 64, 0, KT_SX, 4096, U) };
    c[n++] = (at_case_t){ "A-N10", "DECODE_IMAGE dims(arg6) -> kernel text (8B kernel write)",
        false, syscall_validate_args(253, U, 64, 0, U, ubuf_len, KT) };
    c[n++] = (at_case_t){ "A-N11", "DECODE_IMAGE out(arg4) valid base + 64MB out_cap runs off",
        false, syscall_validate_args(253, U, 64, 0, U, 64ULL * 1024 * 1024, U) };
    c[n++] = (at_case_t){ "A-N12", "DECODE_IMAGE data(arg1) -> kernel text (feed kernel image to decoder)",
        false, syscall_validate_args(253, KT, 64, 0, U, ubuf_len, U) };
    c[n++] = (at_case_t){ "A-P10", "DECODE_IMAGE fully legitimate call",
        true, syscall_validate_args(253, U, 64, 0, U, ubuf_len, U) };

    // ---- Kind::W + Len::ElemsClamped (SYS_SVC_LIST 320) ------------------
    // The handler clamps max to PI_MAX_SVCS(32) before writing, so the table
    // validates min(argN, 32) rows. P11 is the case that forced this kind to
    // exist: a caller is ENTITLED to pass a huge max precisely because the
    // handler clamps, and a plain Len::Elems would compute 1000*80 and reject a
    // legal call. 32*80 = 2560 <= the 4096-byte test buffer.
    c[n++] = (at_case_t){ "A-P11", "SVC_LIST huge max accepted: handler clamps, table clamps identically",
        true, syscall_validate_args(320, U, 1000000, 0, 0, 0, 0) };
    c[n++] = (at_case_t){ "A-N13", "SVC_LIST arg1 -> kernel text (service rows over kernel image)",
        false, syscall_validate_args(320, KT, 1, 0, 0, 0, 0) };
    // The specific way a clamp goes wrong: if min() collapsed to 0 (wrong
    // operand order, a cap of 0, a bad cast), len_bytes would return 0 and the
    // validator SKIPS zero-length buffers by design - so a broken clamp would
    // not fail loudly, it would silently accept a kernel-text base. This case is
    // deliberately mapping-independent: it does not depend on what happens to be
    // mapped after the test buffer, only on the clamp arithmetic being real.
    c[n++] = (at_case_t){ "A-N14", "SVC_LIST huge max + kernel-text base: clamp must not collapse to a zero-length skip",
        false, syscall_validate_args(320, KT, 1000000, 0, 0, 0, 0) };

    // ---- Kind::R + Len::Packed16 (SYS_WIN_BLIT 35) -----------------------
    // Length is src_w*src_h*4 with BOTH dimensions packed into arg4. The table
    // duplicates the dispatcher's unpack, so it is asserted here rather than
    // trusted: 0x10001000 = 4096x4096 = 64MB, which runs far off the end of ALL
    // mapped user memory and must be rejected; 0x00100010 = 16x16 = 1024 bytes
    // fits and must be accepted.
    //
    // WHAT THIS DELIBERATELY DOES NOT ASSERT, because the validator does not
    // promise it: an earlier version of this case used 64x64 = 16KB against the
    // 8KB probe buffer and expected a REJECT. It was ACCEPTED, and that is
    // CORRECT: .bss continues past probe_buf, so those pages really are present
    // and user-accessible. validate_user_ptr answers "may Ring 3 touch this?",
    // NOT "is this inside the object you meant". An app over-reading its own
    // memory is not a trust-boundary violation and this layer will never catch
    // it. Sizing a negative control below the end of the caller's own mappings
    // tests a promise nobody made.
    c[n++] = (at_case_t){ "A-N15", "WIN_BLIT 4096x4096 src (64MB) runs off into unmapped memory",
        false, syscall_validate_args(35, 0, 0, 0, 0x10001000, U, 0) };
    c[n++] = (at_case_t){ "A-P12", "WIN_BLIT 16x16 src (1KB) that fits is accepted",
        true, syscall_validate_args(35, 0, 0, 0, 0x00100010, U, 0) };
    // Both dimensions maxed: 65535*65535*4 = ~17GB. Nothing can satisfy it, and
    // the point is that it must REJECT rather than wrap to a small number that
    // passes. This is the Packed16 twin of A-N2.
    c[n++] = (at_case_t){ "A-N16", "WIN_BLIT 65535x65535 src (~17GB) must not wrap to a tiny check",
        false, syscall_validate_args(35, 0, 0, 0, 0xFFFFFFFF, U, 0) };
    c[n++] = (at_case_t){ "A-N17", "WIN_BLIT src buffer -> kernel text (blit kernel image into a window)",
        false, syscall_validate_args(35, 0, 0, 0, 0x00100010, KT, 0) };

    // ---- Kind::R + Len::Mul2 (SYS_WIN_DRAW_IMAGE 254) --------------------
    // Same idea, but w and h are separate args (arg4, arg5), so the multiply is
    // of two independent attacker-chosen values: the checked_mul is the control.
    c[n++] = (at_case_t){ "A-N18", "WIN_DRAW_IMAGE w*h*4 OVERFLOWS u64 (wrap -> tiny check)",
        false, syscall_validate_args(254, 0, 0, 0, 0x100000000ULL, 0x100000000ULL, U) };
    // As with A-N15: sized to run off ALL mapped user memory, not merely past
    // the probe buffer, because "past your own buffer but still your own memory"
    // is not something this layer claims to detect.
    c[n++] = (at_case_t){ "A-N19", "WIN_DRAW_IMAGE 4096x4096 (64MB) runs off into unmapped memory",
        false, syscall_validate_args(254, 0, 0, 0, 4096, 4096, U) };
    c[n++] = (at_case_t){ "A-P13", "WIN_DRAW_IMAGE 16x16 (1KB) that fits is accepted",
        true, syscall_validate_args(254, 0, 0, 0, 16, 16, U) };
    c[n++] = (at_case_t){ "A-N20", "WIN_DRAW_IMAGE src -> sign-extended kernel text (shim is not a bypass)",
        false, syscall_validate_args(254, 0, 0, 0, 16, 16, KT_SX) };

    // ---- The honesty case -------------------------------------------------
    // An UNDECLARED syscall is NOT validated and must return 0. This is not a
    // hole being tested for absence, it is the incremental rollout being tested
    // for honesty: if this ever returned EFAULT the table would be silently
    // covering syscalls it has no descriptor for. SYS_FORK(1) takes no pointers
    // and is deliberately not in the table.
    c[n++] = (at_case_t){ "A-P7", "undeclared syscall passes through unvalidated (ledger is honest)",
        true, syscall_validate_args(1, KT, KT, KT, 0, 0, 0) };
    // ...and the coverage claim itself must be checkable against the kernel.
    if (syscall_desc_covers(201) != 1 || syscall_desc_covers(1) != 0) {
        kprintf("[ARGTAB] A-N8 FAIL coverage introspection disagrees with the table\n");
        fail++;
    } else {
        kprintf("[ARGTAB] A-P8 PASS coverage introspection agrees (201 covered, 1 not)\n");
        pass++;
    }

    for (int i = 0; i < n; i++) at_report(&c[i], &pass, &fail);

    kprintf("[ARGTAB] ===== %d/%d passed, %d FAILED =====\n", pass, n + 1, fail);
    kprintf("[ARGTAB] RESULT: %s\n",
            fail == 0 ? "DISPATCHER CHOKE POINT HOLDS" : "CHOKE POINT LEAKS");
    return fail;
}

// ubuf must be a genuine, writable, mapped Ring-3 buffer of at least 4096 bytes
// supplied by the calling app. We need a real user pointer to build the
// interesting boundary cases out of.
int64_t validate_selftest(void *ubuf, uint64_t ubuf_len) {
    int pass = 0, fail = 0;
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= VMM_ADDR_MASK;

    kprintf("\n[SECTEST] ===== #500 validate_user_ptr negative controls =====\n");
    kprintf("[SECTEST] caller cr3=0x%lx ubuf=0x%lx len=%lu\n", cr3, (uint64_t)ubuf, ubuf_len);

    // Ground truth first: what does the hardware actually allow here?
    vt_dump_eff("user buffer", cr3, (uint64_t)ubuf);
    vt_dump_eff("kernel text", cr3, KERNEL_TEXT_ADDR);
    vt_dump_eff("kernel end", cr3, (uint64_t)__kernel_end);
    void *kheap = kmalloc(64);
    if (kheap) vt_dump_eff("kernel heap", cr3, (uint64_t)kheap);

    vt_case_t cases[12];
    int n = 0;

    // ---- POSITIVE CONTROL -------------------------------------------------
    // If this fails the validator is useless and every other PASS below is
    // meaningless (a reject-everything validator "passes" all negative tests).
    cases[n++] = (vt_case_t){ "P1", "legit user buffer must work", true,
        validate_user_ptr(ubuf, 64, ACCESS_RW_USER) };

    // ---- THE #500 HOLE ----------------------------------------------------
    // Kernel text at a LOW identity-mapped address. Presence-only validation
    // accepts this: it is present in the caller's CR3 and < 2^47.
    cases[n++] = (vt_case_t){ "N1", "read kernel text = info leak of kernel code", false,
        validate_user_ptr((void *)KERNEL_TEXT_ADDR, 64, ACCESS_READ_USER) };

    cases[n++] = (vt_case_t){ "N2", "WRITE kernel text = arbitrary code overwrite = ring0", false,
        validate_user_ptr((void *)KERNEL_TEXT_ADDR, 64, ACCESS_WRITE_USER) };

    if (kheap) {
        cases[n++] = (vt_case_t){ "N3", "write kernel heap = corrupt kernel structs", false,
            validate_user_ptr(kheap, 64, ACCESS_WRITE_USER) };
    }

    // ---- CLASSIC POINTER CASES -------------------------------------------
    cases[n++] = (vt_case_t){ "N4", "NULL deref", false,
        validate_user_ptr(NULL, 8, ACCESS_READ_USER) };

    cases[n++] = (vt_case_t){ "N5", "upper-half kernel address", false,
        validate_user_ptr((void *)0xFFFF800000000000ULL, 8, ACCESS_READ_USER) };

    cases[n++] = (vt_case_t){ "N6", "non-canonical address", false,
        validate_user_ptr((void *)0x0000800000000000ULL, 8, ACCESS_READ_USER) };

    cases[n++] = (vt_case_t){ "N7", "unmapped user-half address", false,
        validate_user_ptr((void *)0x0000700000000000ULL, 8, ACCESS_READ_USER) };

    // ---- LENGTH, NOT JUST ADDRESS ----------------------------------------
    // THE classic bypass: a base the validator likes, plus a length the
    // attacker chose. If only the base were checked, both of these pass.
    cases[n++] = (vt_case_t){ "N8", "valid base + length wraps the address space", false,
        validate_user_ptr(ubuf, 0xFFFFFFFFFFFFFFFFULL, ACCESS_READ_USER) };

    cases[n++] = (vt_case_t){ "N9", "valid base + length runs off into unmapped memory", false,
        validate_user_ptr(ubuf, 64ULL * 1024 * 1024, ACCESS_READ_USER) };

    // Base valid, range walks DOWN? Not reachable (user is at 2GB, kernel at
    // 4MB, and len is unsigned) - so the interesting direction is upward, N9.

    // ---- STRING VALIDATOR -------------------------------------------------
    // The #487 fix did not touch validate_user_string(); it still used the dead
    // kernel_pml4 walk. P2 is the positive control that catches that.
    ((char *)ubuf)[0] = 'o'; ((char *)ubuf)[1] = 'k'; ((char *)ubuf)[2] = '\0';
    cases[n++] = (vt_case_t){ "P2", "legit user string must work", true,
        validate_user_string((const char *)ubuf, 64) };

    cases[n++] = (vt_case_t){ "N10", "string pointing at kernel text = leak", false,
        validate_user_string((const char *)KERNEL_TEXT_ADDR, 64) };

    for (int i = 0; i < n; i++) vt_report(&cases[i], &pass, &fail);

    if (kheap) kfree(kheap);

    kprintf("[SECTEST] ===== %d/%d passed, %d FAILED =====\n", pass, n, fail);
    kprintf("[SECTEST] RESULT: %s\n", fail == 0 ? "ALL CONTROLS HELD" : "HOLES PRESENT");

    // #503: the validator holding is necessary but not sufficient. Now prove the
    // DISPATCHER actually routes arguments into it, per descriptor kind.
    fail += (int)argtab_selftest(ubuf, ubuf_len);

    return fail;   // 0 = clean
}
