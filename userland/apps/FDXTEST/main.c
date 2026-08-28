// FDXTEST - #fdguard exploit/verifier for HOLE #1: cross-process legacy fd.
//
// Two processes: a HOLDER (fork child) opens a secret marker file, keeping its
// SYSTEM-WIDE legacy fd (the 256..383 range #FDNS gave the FAT/ext2/SMB/NFS
// tables) live; a GUESSER (the parent) then scans that whole range calling
// read() on every number without having opened any of them.
//
//   RED   (vulnerable build): one of those reads returns the holder's secret,
//         because the legacy tables were a global namespace with no owner.
//   GREEN (fixed build): every read of a fd this process does not own is
//         refused, so nothing leaks, and the kernel records the refusal in
//         /CONFIG/SECURITY.LOG with the actor pid, the target slot and the
//         reason (rustkern/fdown.rs + proc/fdlayer.c legacy_owner_ok()).
//
// Verdict goes to stdout (serial, via /dev/console) AND to /BOOTLOG.TXT via
// sys_bootlog, so a headless VM can capture it either way.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/sys/wait.h"
#include "../../libc/signal.h"
#include "../../libc/syscall.h"

#define MARKER   "FDGUARD-SECRET-8bf3c2a1d0"
#define MARKPATH "/FDMARK.TXT"
#define LOW  256
#define HIGH 384

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }

int main(void) {
    // Create the secret, then CLOSE it, so the GUESSER (this process) holds no
    // legacy fd for it; only the holder child will.
    int wfd = open(MARKPATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (wfd < 0) { say("FDXTEST: SETUP-FAIL cannot create marker\n"); return 2; }
    const char *m = MARKER "\n";
    write(wfd, m, strlen(m));
    close(wfd);

    int pfd[2];
    if (pipe(pfd) != 0) { say("FDXTEST: SETUP-FAIL pipe\n"); return 2; }

    int pid = fork();
    if (pid < 0) { say("FDXTEST: SETUP-FAIL fork\n"); return 2; }

    if (pid == 0) {
        // HOLDER: a distinct process (own pid, own tgid). Open the marker via a
        // legacy fd and hold it, WITHOUT reading it (so the read offset stays 0
        // and the guesser's read sees the marker from byte 0). Signal ready.
        close(pfd[0]);
        int hfd = open(MARKPATH, O_RDONLY);
        if (hfd < 0) { write(pfd[1], "X", 1); for (;;) usleep(200000); }
        write(pfd[1], "R", 1);
        for (;;) usleep(200000);   // hold; the parent kills us when done
    }

    // GUESSER: wait until the holder has the file open.
    close(pfd[1]);
    { char b = 0; read(pfd[0], &b, 1); }

    int leaked = 0, firstfd = -1;
    char buf[64];
    for (int fd = LOW; fd < HIGH; fd++) {
        for (int i = 0; i < 64; i++) buf[i] = 0;
        long n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0 && strstr(buf, MARKER)) { leaked++; if (firstfd < 0) firstfd = fd; }
    }

    // POSITIVE CONTROL: the guard must refuse only OTHER processes. This
    // process opening and reading ITS OWN file through the same legacy fd
    // range must still work, or the "fix" is just a broken file layer. A
    // one-sided test that only shows refusals cannot tell a working guard from
    // a dead read path.
    int ownok = 0, ownfd = open(MARKPATH, O_RDONLY);
    if (ownfd >= 0) {
        for (int i = 0; i < 64; i++) buf[i] = 0;
        long n2 = read(ownfd, buf, sizeof(buf) - 1);
        if (n2 > 0 && strstr(buf, MARKER)) ownok = 1;
        close(ownfd);
    }
    { char ol[160];
      snprintf(ol, sizeof(ol),
        "FDXTEST: own-fd read fd=%d ok=%d (positive control: owner must still read)\n",
        ownfd, ownok);
      say(ol); }

    char line[160];
    if (leaked)
        snprintf(line, sizeof(line),
            "FDXTEST: leaked=%d firstfd=%d verdict=RED (read another process's open file)\n",
            leaked, firstfd);
    else
        snprintf(line, sizeof(line),
            "FDXTEST: leaked=0 verdict=GREEN (cross-process legacy fd read refused)\n");
    say(line);

    kill(pid, 9);
    { int st = 0; sys_waitpid(pid, &st, 0); }
    unlink(MARKPATH);
    return (leaked || !ownok) ? 1 : 0;
}
