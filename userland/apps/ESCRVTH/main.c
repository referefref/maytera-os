// ESCRVTH - the HELPER half of the ESCRVT spawn-escape proof.
//
// ESCRVT is a marked, LOCKED escrow actor. It SYS_SPAWNs this program and has
// it do the deleting, which is the escape that was open until 2026-10-03:
// kernel/proc/process.c init_proc() memsets the whole new process_t, so a
// spawned child began life with escrow_active == 0. fork() and clone() copied
// the mark through their whole-struct memcpy, and execve() kept the SAME
// process_t so the mark survived it, but SPAWN built a FRESH one. Copying a
// process kept the mark; LAUNCHING A PROGRAM dropped it. A marked AI actor
// could therefore spawn this and the kernel would see an ordinary uid-0
// process with no contract at all.
//
// This program does not cooperate. It never calls escrow_enter(), never touches
// the userland AI tool layer, and issues raw syscalls as uid 0. What it checks
// is whether the KERNEL treats it as a marked actor anyway:
//
//   1. an OUT-OF-SCOPE write must be REFUSED. This is the cleanest single
//      signal of "am I marked": an unmarked uid-0 process can write anywhere.
//   2. an in-scope write must be ALLOWED, under a grant it never requested,
//      because it inherited its parent's contract rather than a bare mark.
//   3. its delete must be REDIRECTED into the Recycle Bin, not performed as a
//      hard unlink. It verifies the FILE IS GONE from its path; the PARENT
//      verifies the bytes are in the bin, because a child reporting on its own
//      containment is exactly the claim that needs independent checking.
//   4. it must not be able to self-exit, because the inheritance rule promotes
//      an inherited mark to LOCKED (rustkern/escrowmark.rs escmark_inherit_rs).
//
// Exit status 0 means every one of those held. ESCRVT waits for it and reports
// that status alongside its own measurement of the outcome.
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

#define SC      "/ESCRVTSC"
#define HELPERV SC "/HELPER.TXT"
#define INFILE  SC "/HELPER_IN.TXT"
#define OUTF    "/ESCRVTH_OUT.TMP"

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static void line(const char *label, int ok) {
    char l[224];
    snprintf(l, sizeof(l), "ESCRVTH: %s %s\n", label, ok ? "PASS" : "FAIL");
    say(l);
}

static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
static long r_open_w(const char *p) {
    return syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
}
static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}
static int put(const char *p, const char *d) {
    long fd = r_open_w(p);
    if (fd < 0) return -1;
    long n = (long)strlen(d);
    long w = syscall3(SYS_WRITE, fd, (long)d, n);
    syscall1(SYS_CLOSE, fd);
    return (w == n) ? 0 : -1;
}

int main(void) {
    say("ESCRVTH: spawned by a marked actor; checking whether the mark followed\n");

    // 1. Am I marked at all? An unmarked uid-0 process writes anywhere.
    int marked = (r_open_w(OUTF) < 0) && !exists(OUTF);
    line("inherited-the-mark(out-of-scope write refused)", marked);

    // 2. Did I inherit the CONTRACT, not just a bare mark? An in-scope write
    //    needs a live grant, and I never asked for one.
    int grant = (put(INFILE, "helper wrote this under an inherited grant\n") == 0);
    line("inherited-the-GRANT(in-scope write allowed)", grant);

    // 3. My delete must take effect and must not destroy. The parent checks the
    //    bin; I check only that the path is clear.
    long rc = r_unlink(HELPERV);
    int del_ok = (rc == 0) && !exists(HELPERV);
    line("delete-succeeded-and-path-is-clear", del_ok);

    // 4. An inherited mark is LOCKED, so I cannot take it off.
    int locked = (escrow_exit() != 0);
    int still  = (r_open_w(OUTF) < 0) && !exists(OUTF);
    line("inherited-mark-is-LOCKED(self-exit refused)", locked);
    line("still-enforced-after-the-attempt", still);

    (void)r_unlink(INFILE);

    int ok = marked && grant && del_ok && locked && still;
    say(ok ? "ESCRVTH: RESULT PASS\n" : "ESCRVTH: RESULT FAIL\n");
    return ok ? 0 : 1;
}
