// envprobe - #112 measurement instrument for the cross-process environment.
//
// THIS EXISTS TO BE ABLE TO FAIL. It is run on a kernel WITHOUT the #112
// change first, where every check must report FAIL, and only then on a kernel
// with it, where they must report PASS. A test that has only ever been seen
// passing proves nothing about the thing it is pointed at.
//
// Modes. "all" is the default, so it can be driven from /CONFIG/AUTORUN.CFG,
// which launches one path and passes no arguments.
//
//   envprobe all       run every check below in order and summarise.
//   envprobe boot      KERNEL DEFAULT: a process with no Ring-3 parent to
//                      inherit from should still have PATH/SHELL/TERM.
//   envprobe child     print the inherited marker; exit 0 iff it is right.
//   envprobe show NAME print one variable; exit 0 iff it is set.
//   envprobe dump      print the whole environment.
//
// Deliberately verbose on stdout: the evidence for this ticket is a serial
// log, and a bare exit code in a log is not evidence anyone can check later.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "spawn.h"
#include "errno.h"
#include "sys/wait.h"
#include "fcntl.h"

#define MARK_NAME  "MAYTERA_T112"
#define MARK_VALUE "inherited-ok"
#define SELF       "/APPS/ENVPROBE"
#define TAG        "[T112]"

static int mode_child(void)
{
    const char *v = getenv(MARK_NAME);
    printf("%s child: %s=%s\n", TAG, MARK_NAME, v ? v : "(unset)");
    return (v && !strcmp(v, MARK_VALUE)) ? 0 : 1;
}

static int mode_show(const char *name)
{
    const char *v = getenv(name);
    printf("%s show: %s=%s\n", TAG, name, v ? v : "(unset)");
    return v ? 0 : 1;
}

static int mode_dump(void)
{
    int n = 0;
    if (environ) for (int i = 0; environ[i]; i++) { printf("%s   %s\n", TAG, environ[i]); n++; }
    printf("%s dump: %d entries\n", TAG, n);
    return 0;
}

static int mode_boot(void)
{
    const char *p = getenv("PATH");
    const char *sh = getenv("SHELL");
    const char *t = getenv("TERM");
    printf("%s boot: PATH=%s SHELL=%s TERM=%s\n", TAG,
           p ? p : "(unset)", sh ? sh : "(unset)", t ? t : "(unset)");
    return (p && p[0]) ? 0 : 1;
}

// Spawn something and return its exit status, or -1 if it could not start.
static int run(char **args)
{
    pid_t pid = 0;
    int st = 0;
    int rc = posix_spawnp(&pid, args[0], NULL, NULL, args, environ);
    if (rc != 0) {
        printf("%s   spawn '%s' failed rc=%d\n", TAG, args[0], rc);
        return -1;
    }
    if (waitpid(pid, &st, 0) < 0) {
        printf("%s   waitpid failed\n", TAG);
        return -1;
    }
    return st;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "all";

    if (!strcmp(mode, "child")) return mode_child();
    if (!strcmp(mode, "dump"))  return mode_dump();
    if (!strcmp(mode, "boot"))  return mode_boot();
    if (!strcmp(mode, "show"))  return mode_show(argc > 2 ? argv[2] : "PATH");
    if (strcmp(mode, "all")) { printf("usage: envprobe [all|boot|child|show NAME|dump]\n"); return 2; }

    int fails = 0;
    printf("%s ==== #112 cross-process environment probe ====\n", TAG);

    // ---- 1. the kernel default block ---------------------------------------
    int r = mode_boot();
    printf("%s CHECK1 kernel-default-env: %s\n", TAG, r == 0 ? "PASS" : "FAIL");
    if (r) fails++;
    mode_dump();

    // ---- 2. the instrument itself ------------------------------------------
    // If setenv/getenv do not round-trip inside ONE process, everything below
    // is measuring the wrong thing, and saying so is the difference between a
    // result and a guess.
    if (setenv(MARK_NAME, MARK_VALUE, 1) != 0 ||
        !getenv(MARK_NAME) || strcmp(getenv(MARK_NAME), MARK_VALUE)) {
        printf("%s INSTRUMENT BROKEN: setenv/getenv do not round-trip\n", TAG);
        return 2;
    }
    printf("%s instrument: setenv/getenv round-trip OK\n", TAG);

    // ---- 3. THE PREMISE: does a setenv reach a child? ----------------------
    {
        char *a[3];
        a[0] = (char *)SELF; a[1] = (char *)"child"; a[2] = NULL;
        r = run(a);
        printf("%s CHECK2 setenv-crosses-spawn: %s (child exit=%d)\n",
               TAG, r == 0 ? "PASS" : "FAIL", r);
        if (r != 0) fails++;
    }

    // ---- 4. env(1) NAME=VALUE ----------------------------------------------
    {
        char *a[6];
        a[0] = (char *)"/APPS/ENV";
        a[1] = (char *)"T112_VIA_ENV=works";
        a[2] = (char *)SELF;
        a[3] = (char *)"show";
        a[4] = (char *)"T112_VIA_ENV";
        a[5] = NULL;
        r = run(a);
        printf("%s CHECK3 env(1)-NAME=VALUE: %s (exit=%d)\n",
               TAG, r == 0 ? "PASS" : "FAIL", r);
        if (r != 0) fails++;
    }

    // ---- 5. env -i really empties -----------------------------------------
    // The child should NOT see PATH. Inverted: exit 1 from `show` is the PASS.
    {
        char *a[6];
        a[0] = (char *)"/APPS/ENV";
        a[1] = (char *)"-i";
        a[2] = (char *)SELF;
        a[3] = (char *)"show";
        a[4] = (char *)"PATH";
        a[5] = NULL;
        r = run(a);
        printf("%s CHECK4 env-i-empties: %s (exit=%d, 1 is correct here)\n",
               TAG, r == 1 ? "PASS" : "FAIL", r);
        if (r != 1) fails++;
    }

    // ---- 6. msh `export` reaches a child ----------------------------------
    // The headline userland consumer. msh used to keep a PRIVATE 64-entry
    // table, so `export FOO=bar` set something only msh could see. It now goes
    // through setenv(), and msh's spawn wrappers carry `environ`.
    //
    // Driven by feeding msh a script on stdin (posix_spawn addopen(0,...)), and
    // the verdict is read back from a FILE the script writes with msh's own `>`
    // redirection, so this exercises the redirect path (sys_spawn_redir) as
    // well as the plain one. /T112.SH is staged next to this binary.
    {
        posix_spawn_file_actions_t fa;
        pid_t pid = 0;
        int st = 0;
        unlink("/T112.OUT");
        if (posix_spawn_file_actions_init(&fa) != 0 ||
            posix_spawn_file_actions_addopen(&fa, 0, "/T112.SH", O_RDONLY, 0) != 0) {
            printf("%s CHECK5 msh-export: FAIL (cannot build file actions)\n", TAG);
            fails++;
        } else {
            char *a[2]; a[0] = (char *)"/APPS/MSH"; a[1] = NULL;
            int rc = posix_spawn(&pid, a[0], &fa, NULL, a, environ);
            if (rc != 0) {
                printf("%s CHECK5 msh-export: FAIL (spawn rc=%d)\n", TAG, rc);
                fails++;
            } else {
                waitpid(pid, &st, 0);
                char buf[256];
                int n = 0;
                int fd = open("/T112.OUT", O_RDONLY);
                if (fd >= 0) { n = (int)read(fd, buf, sizeof(buf) - 1); close(fd); }
                if (n < 0) n = 0;
                buf[n] = '\0';
                printf("%s   msh captured: %s", TAG, n ? buf : "(nothing)\n");
                int ok = (strstr(buf, "T112_MSH=via-msh") != NULL);
                printf("%s CHECK5 msh-export: %s\n", TAG, ok ? "PASS" : "FAIL");
                if (!ok) fails++;
            }
        }
    }

    printf("%s ==== RESULT: %d check(s) FAILED ====\n", TAG, fails);
    return fails ? 1 : 0;
}
