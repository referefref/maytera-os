// dos/dos4gw.c - #740: the C side of the DOS/4GW guest bridge.
//
// Deliberately almost empty, in the same shape as dos/dpmi.c. The bridge is
// Rust (rustkern/dos4gw.rs) and the guest's run loop lives next to the window,
// console and interrupt machinery it reuses (dos/dosexec.c). What is left for
// this file is the one thing neither of those can do: make the bridge RUN on
// every boot, whether or not anybody launches a DOS/4GW binary.
//
// WHY THAT MATTERS ENOUGH TO BE A FILE.
// This repository's characteristic failure is code that compiles, links and
// never executes: validate_user_ptr, sse_save, graphfs's 72 declarations with 0
// callers, #710 async_io, #712 virtio. The marshalling in rustkern/dos4gw.rs is
// exactly the kind of code that fault selects for, because it is only reached
// when a guest makes a service call, and until this change no guest could make
// one. So it is driven at boot with synthesised register files and the results
// are asserted, and the assertion COUNT is printed, because a self-test that
// ran zero checks and one that passed look identical otherwise (#514).
//
// The checks it runs are not ceremonial. Two of them are the faults that would
// otherwise be found months later in a game: a wrong exhi[] index (which puts
// the high half of EAX into ECX and is invisible to any 16-bit-only test,
// exec/x86_16.h:60), and a stub effect that is logged but not applied to the
// guest's own CF and AX (blame.md: a log-only MISS desynchronises the guest and
// surfaces the bug far from its cause).
#include "dos4gw.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/bootlog.h"
#include "../exec/go32.h"
#include "../exec/x86_32.h"
#include "../exec/softfpu.h"

// The self-test needs a 128 KiB arena to exercise the bounds chokepoint and the
// DPMI heap against real addresses rather than against a mock. It is allocated
// and freed here rather than being static, so the shipping golden carries no
// permanent cost for a test that runs once.
#define DOS4GW_ST_SCRATCH 0x20000u

// (#211) The go32 loader's fixture needs a synthetic file plus an arena to
// load it into: 0x1000 + 0x20000. It shares this file's scratch discipline
// (heap, freed, SKIPPED rather than PASS when it cannot be had) for the reason
// #212 wrote down the hard way: a buffer this size on the DOS task's 64 KiB
// kernel stack produced forty stack-overflow reports and an intermittent
// Invalid Opcode panic.
#define GO32_ST_SCRATCH   0x21000u

void dos4gw_selftest_report(void)
{
    uint32_t checks = 0;
    void *st = kmalloc(dos4gw_state_size_rs());
    uint8_t *scratch = (uint8_t *)kmalloc(DOS4GW_ST_SCRATCH);
    if (!st || !scratch) {
        if (st) kfree(st);
        if (scratch) kfree(scratch);
        // A skipped self-test is reported as a skip, never as a pass. The
        // difference is the whole point of printing the check count.
        kprintf("[4GW] bridge selftest SKIPPED: out of memory (%u + %u bytes)\n",
                dos4gw_state_size_rs(), DOS4GW_ST_SCRATCH);
        bootlog_write("[4GW] bridge selftest SKIPPED oom");
        return;
    }

    int rc = dos4gw_selftest_rs(st, scratch, DOS4GW_ST_SCRATCH, &checks);
    kprintf("[4GW] DOS/4GW bridge selftest: %s (%u checks%s)\n",
            rc == 0 ? "PASS" : "FAIL", checks,
            rc == 0 ? "" : ", see [4GW-ST] FAIL lines above");
    bootlog_write("[4GW] bridge selftest %s checks=%u firstfail=%d",
                  rc == 0 ? "PASS" : "FAIL", checks, rc);

    kfree(scratch);
    kfree(st);

    // ---- (#211) the three units the DJGPP guest depends on ---------------
    //
    // Driven here rather than from their own boot hooks because this function
    // already exists for exactly this reason and already owns the "allocate
    // scratch, report SKIPPED not PASS" pattern. Each prints its own count, so
    // a unit that silently stops running is visible as a count of zero rather
    // than as an unexplained regression in a game months later.

    // The shared IEEE-754 arithmetic (exec/softfpu.c), which both guest CPU
    // cores now use. It needs no scratch.
    {
        int c2 = 0;
        int r2 = sfp_selftest(&c2);
        kprintf("[softfpu] shared IEEE-754 arithmetic selftest: %s (%d checks)\n",
                r2 == 0 ? "PASS" : "FAIL", c2);
        bootlog_write("[softfpu] selftest %s checks=%d", r2 == 0 ? "PASS" : "FAIL", c2);
    }

    // The x87 unit (rustkern/x87.rs). The CPU object goes on the HEAP: see the
    // note in exec/x86_32.h, and the #212 scar above.
    {
        uint32_t c3 = 0;
        x86_32_cpu_t *cpu = (x86_32_cpu_t *)kmalloc(sizeof(x86_32_cpu_t));
        if (!cpu) {
            kprintf("[x87] selftest SKIPPED: out of memory (%u bytes)\n",
                    (unsigned)sizeof(x86_32_cpu_t));
            bootlog_write("[x87] selftest SKIPPED oom");
        } else {
            uint32_t r3 = x87_selftest_rs(cpu, &c3);
            kprintf("[x87] 32-bit guest x87 selftest: %s (%u checks%s)\n",
                    r3 == 0 ? "PASS" : "FAIL", c3,
                    r3 == 0 ? ", including djgpp's exact __detect_80387 sequence" : "");
            bootlog_write("[x87] selftest %s checks=%u fails=%u",
                          r3 == 0 ? "PASS" : "FAIL", c3, r3);
            kfree(cpu);
        }
    }

    // The go32/COFF loader (rustkern/go32.rs).
    {
        uint32_t c4 = 0;
        uint8_t *sc = (uint8_t *)kmalloc(GO32_ST_SCRATCH);
        if (!sc) {
            kprintf("[go32] loader selftest SKIPPED: out of memory (%u bytes)\n",
                    GO32_ST_SCRATCH);
            bootlog_write("[go32] selftest SKIPPED oom");
        } else {
            uint32_t r4 = go32_selftest_rs(sc, GO32_ST_SCRATCH, &c4);
            kprintf("[go32] DJGPP COFF loader selftest: %s (%u checks%s)\n",
                    c4 == 0 ? "SKIPPED" : (r4 == 0 ? "PASS" : "FAIL"), c4,
                    c4 == 0 ? " - scratch refused" : "");
            if (c4 != 0 && r4 != 0)
                kprintf("[go32] first FAILING check is number %u of %u\n", r4, c4);
            bootlog_write("[go32] selftest %s checks=%u fails=%u",
                          c4 == 0 ? "SKIPPED" : (r4 == 0 ? "PASS" : "FAIL"), c4, r4);
            kfree(sc);
        }
    }
}
