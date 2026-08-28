// BLITXTST - #blitguard: the Ring-3 proof that sys_win_blit() would read any
// address the caller named, and the proof that it no longer will.
//
// WHY THIS SHIPS AS A PROGRAM AND NOT AS PROSE
// -------------------------------------------
// The finding this closes had been WRITTEN DOWN TWICE by two different agents
// and fixed by nobody, and this project has repeatedly found that a "known bug"
// described in prose was actually stale. A claim about a trust boundary is only
// worth what can be re-run against it, so the attack lives here, as a program,
// and can be pointed at any future kernel.
//
// It is on build/unshipped-apps.list: it reaches an image only via
// --with-unshipped-apps. A program whose entire purpose is to hand the kernel
// attacker-chosen addresses has no business in a production golden, the same
// judgement CRASHAPP, netcrash and FDXTEST already carry.
//
// THE BUG, PRECISELY
// ------------------
// The dispatcher unpacks src_w = (arg4 & 0xFFFF) and src_h = ((arg4 >> 16) &
// 0xFFFF) and the handler scaled with
//
//     int sy = (dy * scale_y_fp) >> 8;
//     if (sy >= src_h) sy = src_h - 1;          // <-- src_h == 0 gives sy = -1
//     uint32_t *src_row = src_buffer + sy * src_w;
//     for (dx...) dst_row[dx] = src_row[sx];    // <-- raw Ring-0 read
//
// so src_h == 0 read from BELOW the pointer. And the #503 argtab descriptor for
// syscall 35 declares the length as src_w * src_h * 4, which is ZERO when
// src_h is zero, and syscall_validate_args() skips a zero-length range by
// design. So for exactly this input the pointer was never validated at all.
// The kernel is identity mapped (physical == virtual), so 0x400000 is kernel
// text and needs no clever address at all.
//
// WHAT EACH CASE PROVES, AND WHAT A PASS MEANS
// --------------------------------------------
//   C1  arg4 = 0, ptr = kernel text.       Must be REFUSED (rc != 0).
//                                          On an unfixed kernel this returns 0
//                                          and the window is painted with the
//                                          dword at 0x400000 - a solid colour
//                                          you can predict from kernel.elf and
//                                          then read off a screenshot.
//   C2  src_w = 1, src_h = 0, ptr = kernel text + 4. Same, via the other route:
//                                          reads src_buffer[-1].
//   C3  src_w = 0, src_h = 8, ptr = kernel text.     The other zero factor.
//   C4  arg4 = 0, ptr = unmapped canonical user address. On an unfixed kernel
//                                          this is a Ring-0 page fault on a
//                                          Ring-3-chosen address, i.e. the
//                                          machine dies. If this case returns
//                                          AT ALL, the read was bounded.
//   C5  a large src_w with src_h = 0, ptr just above a real user buffer. The
//                                          256 KiB under-read, aimed at the
//                                          app's own memory so that an unfixed
//                                          kernel survives it and can report.
//   C6  POSITIVE CONTROL: an ordinary, honest blit. Must SUCCEED. Without this
//                                          a kernel that refused everything
//                                          would "pass" every case above and
//                                          take the desktop with it.
//
// Results go to /BOOTLOG.TXT through sys_bootlog(), not only to stdout, because
// the machine that matters has no serial port.
#include <stdio.h>
#include "syscall.h"

#define SYS_WIN_BLIT_NUM 35

// Identity-mapped kernel text. KERNEL_PHYS_BASE from kernel/linker.ld, and the
// same constant kernel/security/validate_test.c builds its negative controls
// out of. Present in every process's CR3, supervisor-only: exactly the address
// a leak wants.
#define KERNEL_TEXT 0x400000UL
// Canonical, in the user half, and mapped by nothing.
#define UNMAPPED    0x0000700000000000UL

static int g_fail = 0;
static int g_pass = 0;

static void report(const char *name, const char *what, int want_refuse, long rc) {
    // want_refuse: 1 = the kernel must NOT accept this, 0 = it must accept it.
    int accepted = (rc == 0);
    int ok = want_refuse ? !accepted : accepted;
    if (ok) g_pass++; else g_fail++;
    char line[220];
    // No snprintf in this libc's freestanding subset that is worth trusting for
    // this; build the line by hand so the output shape is exact.
    int p = 0;
    const char *tag = ok ? "PASS " : "FAIL ";
    for (const char *s = "[BLITXTST] "; *s; s++) line[p++] = *s;
    for (const char *s = tag; *s; s++)          line[p++] = *s;
    for (const char *s = name; *s && p < 60; s++) line[p++] = *s;
    line[p++] = ' ';
    for (const char *s = "rc="; *s; s++) line[p++] = *s;
    long v = rc; if (v < 0) { line[p++] = '-'; v = -v; }
    char num[24]; int nn = 0;
    if (v == 0) num[nn++] = '0';
    while (v > 0) { num[nn++] = (char)('0' + (v % 10)); v /= 10; }
    while (nn > 0) line[p++] = num[--nn];
    line[p++] = ' ';
    for (const char *s = (want_refuse ? "want=REFUSE " : "want=ACCEPT "); *s; s++) line[p++] = *s;
    for (const char *s = what; *s && p < 200; s++) line[p++] = *s;
    if (!ok) for (const char *s = " <<<< TRUST BOUNDARY"; *s; s++) line[p++] = *s;
    line[p] = 0;
    printf("%s\n", line);
    sys_bootlog(line);       // durable: /BOOTLOG.TXT, not just serial
}

static long blit_raw(int h, unsigned int src_w, unsigned int src_h, unsigned long ptr) {
    unsigned long packed = (src_w & 0xFFFFu) | ((src_h & 0xFFFFu) << 16);
    return syscall5(SYS_WIN_BLIT_NUM, (long)h, 0, 0, (long)packed, (long)ptr);
}

int main(void) {
    printf("BLITXTST: sys_win_blit() Ring-3 pointer trust-boundary harness\n");
    sys_bootlog("[BLITXTST] start: sys_win_blit trust-boundary harness");

    int h = win_create("blitguard probe", 80, 80, 320, 240);
    if (h < 0) {
        printf("BLITXTST: win_create failed, cannot test\n");
        sys_bootlog("[BLITXTST] ABORT win_create failed");
        return 1;
    }

    // A real, mapped, writable buffer of our own, for the positive control and
    // for the deliberately-survivable under-read in C5.
    static unsigned int own[256 * 64];
    for (unsigned int i = 0; i < sizeof(own) / sizeof(own[0]); i++) own[i] = 0xFF204060u;

    report("C1-kerneltext-arg4zero",
           "arg4=0 ptr=kernel text: zero declared length, so the argtab skipped it entirely",
           1, blit_raw(h, 0, 0, KERNEL_TEXT));

    report("C2-kerneltext-srch0",
           "src_w=1 src_h=0 ptr=kerneltext+4: reads src[-1], the dword at 0x400000",
           1, blit_raw(h, 1, 0, KERNEL_TEXT + 4));

    report("C3-kerneltext-srcw0",
           "src_w=0 src_h=8 ptr=kernel text: the other zero factor",
           1, blit_raw(h, 0, 8, KERNEL_TEXT));

    // Aimed at our OWN memory so an unfixed kernel survives it and can report.
    report("C5-underread-ownbuf",
           "src_w=4096 src_h=0 ptr=own+16KB: the 16 KiB under-read, aimed somewhere survivable",
           1, blit_raw(h, 4096, 0, (unsigned long)(&own[4096])));

    // POSITIVE CONTROL. Without it, a kernel that refused everything would
    // "pass" every case above and take the desktop with it.
    report("C6-positive-control",
           "an ordinary 64x64 blit from a real user buffer MUST still work",
           0, blit_raw(h, 64, 64, (unsigned long)own));

    // Latch ever_committed so the window actually publishes what it holds.
    // sys_win_blit only self-commits once a win_invalidate() has happened.
    win_invalidate(h);
    sys_sleep(400);

    // ---- THE VISUAL PROOF -------------------------------------------------
    // One more leak blit, then hold the window on screen long enough to
    // screenshot. The two outcomes are different, predicted colours:
    //
    //   UNFIXED kernel: the whole window becomes the dword at kernel vaddr
    //                   0x400000, which for this build is 0xE85250D6, i.e.
    //                   RGB(82, 80, 214) - a periwinkle blue.
    //   FIXED kernel:   the blit is refused and the window keeps the positive
    //                   control's 0xFF204060, i.e. RGB(32, 64, 96) - dark
    //                   slate blue.
    //
    // Predicting the colour from kernel.elf BEFORE running is what makes this
    // a disclosure proof rather than "the window looked odd".
    long vis = blit_raw(h, 1, 0, KERNEL_TEXT + 4);
    report("C7-visual-leak",
           "repeat of C2, held on screen: unfixed shows RGB(82,80,214), fixed keeps RGB(32,64,96)",
           1, vis);
    win_invalidate(h);
    sys_sleep(6000);

    // ---- LAST, BECAUSE ON AN UNFIXED KERNEL IT IS FATAL -------------------
    // A Ring-0 read of an unmapped address with no fault fixup. If this case
    // returns AT ALL, the read was bounded. It is last so that everything above
    // has already been reported and screenshotted before the machine may die.
    sys_bootlog("[BLITXTST] about to run C4 (unmapped ptr). An unfixed kernel dies HERE.");
    report("C4-unmapped-arg4zero",
           "arg4=0 ptr=unmapped: an unfixed kernel takes a Ring-0 #PF here and dies",
           1, blit_raw(h, 0, 0, UNMAPPED));

    win_invalidate(h);

    char sum[128];
    int p = 0;
    for (const char *s = "[BLITXTST] RESULT "; *s; s++) sum[p++] = *s;
    for (const char *s = (g_fail == 0 ? "ALL CASES HELD pass=" : "TRUST BOUNDARY LEAKS pass="); *s; s++) sum[p++] = *s;
    sum[p++] = (char)('0' + (g_pass / 10)); sum[p++] = (char)('0' + (g_pass % 10));
    for (const char *s = " fail="; *s; s++) sum[p++] = *s;
    sum[p++] = (char)('0' + (g_fail / 10)); sum[p++] = (char)('0' + (g_fail % 10));
    sum[p] = 0;
    printf("%s\n", sum);
    sys_bootlog(sum);

    win_destroy(h);
    return g_fail ? 1 : 0;
}
