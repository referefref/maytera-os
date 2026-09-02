// dosmain.c - entry point for DOSUSER, the Ring-3 MayteraOS DOS host.
//
// Deliberately tiny. All the DOS behaviour is the KERNEL'S OWN SOURCE, compiled
// into this process unmodified (see mkgen.sh); this file only starts it and
// reports what happened. Anything that looks like DOS logic belonging here is a
// sign the port has started to fork, which is the failure this design exists to
// prevent.
//
// Compiled in the LIBC universe. dos_run_file() is declared locally rather than
// by including the kernel's dosexec.h, because that header would drag in the
// kernel type world; its signature is primitive-typed, so a local declaration
// is exact. See kbridge.h for the wall.
#include "../../../libc/stdlib.h"
#include "../../../libc/string.h"
#include "../../../libc/unistd.h"
#include "../../../libc/stdio.h"
#include "../../../libc/fcntl.h"
#include "../../../libc/dirent.h"
#include "../../../libc/syscall.h"   // #VOLAPI: sys_vol_info / dimg_vol_t

// kernel/dos/dosexec.c. Loads the program, sets up PSP + registers, runs it to
// completion, and returns the guest's exit code (<0 on load failure). It prints
// its own "%u instructions retired, %u frames presented" line, which is the
// differential oracle against the in-kernel path.
// kernel/dos/dosexec.c. Takes a whole LAUNCH LINE, "<path>[ <command tail>]",
// and splits it with the SAME rule dos_launch_common() uses (#172) instead of
// a second copy of it. This host is handed the raw /CONFIG/DOSRING3.CFG line,
// so calling dos_run_file() with it treated a command tail as part of the
// filename: Stunts LOAD.EXE without its /u MCGA argument exits 1 at 36,738
// instructions instead of 0 at 68,818. It prints its own
// "%u instructions retired, %u frames presented" line, which is the
// differential oracle against the in-kernel path.
extern int dos_run_line(const char *line);

// shim/kshim.c: names every facility that does not exist in Ring 3 and was
// nonetheless reached, with a call count.
extern void dosring3_unimpl_report(void);

// shim/kshim.c. Arms the #708 guest-filesystem slot with THIS process's real
// credentials, the way the in-kernel launcher arms it before proc_create().
// Returns 0 on success. See its definition for why skipping it produced a
// wrong guest PATH rather than a permission error.
extern int dosring3_arm_guest_identity(void);

// shim/volshim.c. #VOLAPI: one line per mounted virtual CD volume this process
// may see, printed BEFORE the guest starts. Without it, "the guest asked for a
// CD" is ambiguous between "no volume was offered" and "the wrong one was", and
// that ambiguity is exactly what cost a run: Red Alert's own "PLEASE INSERT A
// RED ALERT CD" screen was the only evidence there was, 16.7 billion
// instructions in. State the input, then read the outcome.
extern void volshim_report(void);

static void usage(void) {
    static const char msg[] =
        "DOSUSER - the MayteraOS DOS interpreter, hosted in Ring 3.\n"
        "usage: DOSUSER <dos-program-path>\n"
        "  e.g. DOSUSER /DOS/ROGUE/ROGUE.EXE\n";
    (void)write(2, msg, sizeof msg - 1);
}

// ---------------------------------------------------------------------------
// ADVERSARIAL SANDBOX PROBE (#708 follow-up)
//
// The security argument for moving the DOS interpreter to Ring 3 is that the
// guest becomes subject to the KERNEL's own credential checks at every open(),
// rather than to a Ring-0 thread policing itself with a synthesised uid. That
// is an argument. This is the measurement.
//
// It attempts, from inside the host process and through the EXACT path a
// guest's INT 21h 3Dh reaches (kb_open -> open()), to touch files a DOS guest
// must never reach: credential files on the ext2 root, another user's data, and
// escapes above the drive roots via "..". Each must be REFUSED.
//
// THE POSITIVE CONTROLS ARE THE POINT. A probe that reports DENIED for
// everything proves nothing - it is indistinguishable from a probe that is
// simply broken, pointed at a dead path, or running with no filesystem at all.
// So the list includes files that MUST be readable, and the run FAILS if any of
// those is denied. Same discipline as the rawsc self-test's negative arms:
// build the arm that can go red.
struct probe { const char *path; int must_open; const char *why; };

static int sandbox_probe(void) {
    static const struct probe probes[] = {
        // POSITIVE CONTROLS - these MUST succeed, or the probe is meaningless.
        { "/DOS/ROGUE/ROGUE.EXE", 1, "the guest's own program (ext2 root)" },
        { "/BUILDINFO.TXT",       1, "a world-readable file on the FAT ESP" },
        // NEGATIVE - credential material the guest must never reach.
        { "/CONFIG/SHADOW",       0, "password hashes (ext2 root)" },
        { "/CONFIG/KIMI.KEY",     0, "live LLM API key (ext2 root)" },
        { "/CONFIG/AUTHKEYS",     0, "SSH authorised keys (ext2 root)" },
        { "/CONFIG/SSH.CFG",      0, "SSH client config with credentials" },
        { "/CONFIG/EXTSVC.CFG",   0, "external service credentials" },
        // NEGATIVE - path escape above the guest's drive root.
        { "/DOS/ROGUE/../../CONFIG/SHADOW", 0, "..-escape to the shadow file" },
        { "/DOS/../CONFIG/KIMI.KEY",        0, "..-escape to the API key" },
    };
    const int n = (int)(sizeof probes / sizeof probes[0]);

    printf("[SANDBOX] uid=%u gid=%u  probing %d path(s)\n",
           (unsigned)getuid(), (unsigned)getgid(), n);

    int pos_fail = 0, neg_fail = 0;
    for (int i = 0; i < n; i++) {
        int fd = open(probes[i].path, O_RDONLY, 0);
        int opened = (fd >= 0);
        if (opened) close(fd);
        const char *verdict;
        if (probes[i].must_open) {
            verdict = opened ? "OPEN  (expected)" : "DENIED **CONTROL FAILED**";
            if (!opened) pos_fail++;
        } else {
            verdict = opened ? "OPEN  **LEAK**" : "DENIED (expected)";
            if (opened) neg_fail++;
        }
        printf("[SANDBOX]   %-34s %-26s %s\n",
               probes[i].path, verdict, probes[i].why);
    }

    printf("[SANDBOX] RESULT: %s  (positive-control failures=%d, leaks=%d)\n",
           (pos_fail == 0 && neg_fail == 0) ? "PASS" : "FAIL", pos_fail, neg_fail);
    if (pos_fail)
        printf("[SANDBOX] NOTE: a positive control failed, so the DENIED lines "
               "above prove NOTHING - the probe could not read anything at all.\n");
    return (pos_fail == 0 && neg_fail == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// #VOLAPI VOLUME-BOUNDARY PROBE
//
// The sandbox probe above proves the guest cannot reach credential files. This
// one proves the NEW surface did not open a new way out. Same discipline, and it
// is the discipline that matters: a probe reporting DENIED for everything is
// indistinguishable from a probe that is broken, pointed at nothing, or running
// with no volume mounted at all. So the POSITIVE CONTROLS come first and the run
// FAILS if any of them fails, which makes the refusals mean something.
//
// It probes the FIRST volume this process can actually see, discovered through
// the gateway rather than hardcoded, because a hardcoded drive letter is how a
// probe silently starts testing an empty drive.
// ---------------------------------------------------------------------------
static int volume_probe(void) {
    printf("[VOLPROBE] uid=%u gid=%u\n", (unsigned)getuid(), (unsigned)getgid());

    dimg_vol_t v;
    int found = -1;
    int seen = 0;
    for (int i = 0; i < 26; i++) {
        dimg_vol_t t;
        int rc = sys_vol_info(i, &t);
        if (rc != 0) continue;
        if (!(t.flags & DISKIMG_F_MOUNTED)) continue;
        seen++;
        printf("[VOLPROBE] visible %c: label='%s' cls=%u %s root=%s\n",
               'A' + i, t.label, (unsigned)t.cls,
               (t.flags & DISKIMG_F_READONLY) ? "ro" : "rw", t.root);
        if (found < 0) { v = t; found = i; }
    }
    if (found < 0) {
        printf("[VOLPROBE] RESULT: FAIL - no volume is visible, so every DENIED "
               "below would prove NOTHING. Mount media before running this.\n");
        return 1;
    }

    int pos_fail = 0, leak = 0;
    char path[512];

    // ---- POSITIVE CONTROLS. These MUST succeed. ----
    // 1. The volume root must be traversable, or nothing below is a real test.
    {
        DIR *d = opendir(v.root);
        int ok = (d != NULL);
        if (d) closedir(d);
        printf("[VOLPROBE]   opendir %-28s %s  (control: the volume root)\n",
               v.root, ok ? "OPEN  (expected)" : "DENIED **CONTROL FAILED**");
        if (!ok) pos_fail++;
    }
    // 2. The disc must have told us its own name. An empty label means the
    //    gateway answered but the label plumbing is dead, which would let a
    //    label-matching guest fail while every arm below still looked green.
    {
        int ok = (v.label[0] != 0);
        printf("[VOLPROBE]   label     %-28s %s  (control: label plumbing)\n",
               v.label, ok ? "PRESENT (expected)" : "EMPTY **CONTROL FAILED**");
        if (!ok) pos_fail++;
    }

    // ---- NEGATIVE. Escaping the volume. ----
    struct esc { const char *suffix; const char *why; };
    static const struct esc escapes[] = {
        { "/../../CONFIG/SHADOW",  ".. out of the volume to the shadow file" },
        { "/../../CONFIG/KIMI.KEY", ".. out of the volume to the API key" },
        { "/../DRIVE_C/AUTOEXEC.BAT", ".. sideways into another drive" },
    };
    for (int i = 0; i < (int)(sizeof escapes / sizeof escapes[0]); i++) {
        int n = 0;
        for (const char *q = v.root; *q && n < (int)sizeof path - 1; q++) path[n++] = *q;
        for (const char *q = escapes[i].suffix; *q && n < (int)sizeof path - 1; q++) path[n++] = *q;
        path[n] = 0;
        int fd = open(path, O_RDONLY, 0);
        int opened = (fd >= 0);
        if (opened) { close(fd); leak++; }
        printf("[VOLPROBE]   %-44s %s  %s\n", path,
               opened ? "OPEN  **LEAK**" : "DENIED (expected)", escapes[i].why);
    }

    // ---- NEGATIVE. Writing to read-only media. ----
    // This is the fs/fat.c FAT_EROFS guard. Before it, a write here did not
    // fail: it created a DIFFERENT file on the FAT ESP that no later read would
    // ever return, because fat_open()'s image branch answers first.
    {
        int n = 0;
        for (const char *q = v.root; *q && n < (int)sizeof path - 1; q++) path[n++] = *q;
        for (const char *q = "/VOLPROBE.TMP"; *q && n < (int)sizeof path - 1; q++) path[n++] = *q;
        path[n] = 0;
        int fd = open(path, O_WRONLY | O_CREAT, 0644);
        int opened = (fd >= 0);
        if (opened) { close(fd); leak++; }
        printf("[VOLPROBE]   %-44s %s  write to read-only media\n", path,
               opened ? "OPEN  **LEAK**" : "DENIED (expected)");
    }

    // ---- NEGATIVE. A letter with no disc must not be describable as one. ----
    {
        int bogus = 0;   // A: is a floppy class and holds nothing here.
        dimg_vol_t t;
        int rc = sys_vol_info(bogus, &t);
        int claims = (rc == 0 && (t.flags & DISKIMG_F_MOUNTED));
        if (claims) leak++;
        printf("[VOLPROBE]   A: (no disc) %-32s %s\n",
               claims ? "CLAIMS MOUNTED **LEAK**" : "reports empty (expected)", "");
    }

    printf("[VOLPROBE] RESULT: %s  (volumes visible=%d, positive-control "
           "failures=%d, leaks=%d)\n",
           (pos_fail == 0 && leak == 0) ? "PASS" : "FAIL", seen, pos_fail, leak);
    if (pos_fail)
        printf("[VOLPROBE] NOTE: a positive control failed, so the DENIED lines "
               "above prove NOTHING.\n");
    return (pos_fail == 0 && leak == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && !strcmp(argv[1], "--sandbox-probe"))
        return sandbox_probe();
    if (argc >= 2 && argv[1] && !strcmp(argv[1], "--volume-probe"))
        return volume_probe();
    if (argc < 2 || !argv[1] || !argv[1][0]) { usage(); return 2; }

    printf("[DOSRING3] host starting, guest='%s'\n", argv[1]);

    // ARM THE GUEST IDENTITY FIRST, and fail closed, exactly as the in-kernel
    // dos_launch_common() does. This host reaches dos_run_file() directly
    // rather than through that launcher, so the arm has to happen here or not
    // at all - and "not at all" is not a permission failure, it is a guest
    // whose "%HOME%" paths silently resolve somewhere else. MEASURED on
    // NetHack: unarmed, it looked for its level directory at
    // <install>/%HOME%/GAMES/NETHACK, failed its own writability probe and
    // exited 1 with "Some invalid directory locations were specified".
    {
        int arc = dosring3_arm_guest_identity();
        if (arc != 0) {
            printf("[DOSRING3] REFUSING to run: no usable identity for the "
                   "guest (arm rc=%d). A guest with no identity would start "
                   "and then fail every file operation.\n", arc);
            return 1;
        }
        printf("[DOSRING3] guest identity armed: uid=%u gid=%u\n",
               (unsigned)getuid(), (unsigned)getgid());
    }

    // AFTER the identity arm, and that ordering is load-bearing: the volume
    // gateway filters what it reports through perms_check() against THIS
    // process's credentials, so reporting before the arm would describe a
    // different process than the one that will do the opening.
    volshim_report();

    int rc = dos_run_line(argv[1]);

    printf("[DOSRING3] guest '%s' returned %d\n", argv[1], rc);
    dosring3_unimpl_report();
    return rc < 0 ? 1 : 0;
}
