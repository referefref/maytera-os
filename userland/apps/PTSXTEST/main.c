// PTSXTEST - #fdguard exploit/verifier for HOLE #2: open("/dev/pts/N") attaches
// to another session's terminal.
//
// The PARENT creates a pty pair by opening /dev/ptmx (it becomes the pair's
// owner) and learns its index N via TIOCGPTN. An ATTACKER process (fork child:
// a different pid and tgid, and NOT the process the kernel wired that terminal
// to) then tries open("/dev/pts/N").
//
//   RED   (vulnerable build): the open succeeds, so an unrelated process has
//         attached to a terminal it neither created nor is attached to, and can
//         read its keystrokes / inject input (drivers/pty.c checked only in_use).
//   GREEN (fixed build): the attach is refused because the attacker is neither
//         the pair's owner (rustkern/ptsown.rs) nor a process whose controlling
//         terminal is N (process_t.ctty), and the refusal is recorded in
//         /CONFIG/SECURITY.LOG with the actor pid, target and reason.
//
// The child inherits the master fd via fork, but that is irrelevant to the
// gate, which is on the SLAVE-by-name open and keys on pair-owner tgid and
// ctty, both of which the child fails. Verdict to stdout + /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/termios.h"
#include "../../libc/sys/ioctl.h"
#include "../../libc/sys/wait.h"
#include "../../libc/signal.h"
#include "../../libc/syscall.h"

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }

int main(void) {
    int master = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
    if (master < 0) { say("PTSXTEST: SETUP-FAIL /dev/ptmx unavailable\n"); return 2; }
    int n = -1;
    if (ioctl(master, TIOCGPTN, &n) != 0 || n < 0) {
        say("PTSXTEST: SETUP-FAIL TIOCGPTN\n"); close(master); return 2;
    }

    int pfd[2];
    if (pipe(pfd) != 0) { say("PTSXTEST: SETUP-FAIL pipe\n"); close(master); return 2; }

    int pid = fork();
    if (pid < 0) { say("PTSXTEST: SETUP-FAIL fork\n"); close(master); return 2; }

    if (pid == 0) {
        // ATTACKER: a distinct process. Learn N from the parent, then try to
        // attach to /dev/pts/N which the parent owns.
        close(pfd[1]);
        char kb = 0; read(pfd[0], &kb, 1);
        int k = (int)(unsigned char)kb;
        char path[24];
        snprintf(path, sizeof(path), "/dev/pts/%d", k);
        int slave = open(path, O_RDWR);
        char line[160];
        if (slave >= 0) {
            snprintf(line, sizeof(line),
                "PTSXTEST: attached fd=%d to %s verdict=RED (attached to another session's terminal)\n",
                slave, path);
            say(line);
            close(slave);
            _exit(1);
        }
        snprintf(line, sizeof(line),
            "PTSXTEST: attach %s refused verdict=GREEN (cannot attach to a pts we do not own)\n",
            path);
        say(line);
        _exit(0);
    }

    // PARENT: hand the index to the attacker, keep the pair alive, wait.
    close(pfd[0]);
    { char kb = (char)n; write(pfd[1], &kb, 1); }
    { int st = 0; sys_waitpid(pid, &st, 0); }

    // POSITIVE CONTROL, and this is the one that matters most for regressions.
    // The LEGITIMATE terminal flow is exactly this: the process that opened
    // /dev/ptmx then opens /dev/pts/N BY NAME to wire a child's stdio
    // (userland/apps/terminal/term_pty.c does precisely this). If the guard
    // refused the pair's own creator, every Terminal foreground command would
    // break. So the owner's open MUST still succeed.
    {
        char path[24];
        snprintf(path, sizeof(path), "/dev/pts/%d", n);
        int mine = open(path, O_RDWR);
        char ol[176];
        snprintf(ol, sizeof(ol),
            "PTSXTEST: owner-open %s fd=%d %s (positive control: terminal flow must still work)\n",
            path, mine, mine >= 0 ? "OK" : "BROKEN");
        say(ol);
        if (mine >= 0) close(mine);
    }

    close(master);
    return 0;
}
