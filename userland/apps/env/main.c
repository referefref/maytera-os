// env - run a program in a modified environment, or print the environment.
//
// ============================================================================
// HISTORY, BECAUSE THIS FILE HAS BEEN WRONG IN TWO DIFFERENT WAYS
// ============================================================================
//
// ROUND 1 (before #745 local 108). /APPS/ENV did not run anything and did not
// set anything. With arguments it ECHOED each argument on its own line and
// exited 0, so `env FOO=bar prog` printed "FOO=bar" and "prog" and never
// started prog. With no arguments it printed a FABRICATED environment (PATH,
// SHELL, USER, HOME, TERM, PWD) that no process on this OS actually had.
//
// ROUND 2 (#745 local 108). It ran the command, and it REFUSED NAME=VALUE with
// exit 125 and a five-line explanation, because MayteraOS genuinely had no
// cross-process environment: kernel/proc/process.c's proc_create_user_as() met
// its envp parameter with "(void)envp;", setup_user_argv() wrote a NULL where
// envp belonged and labelled it "envp terminator (future)", and crt0 never
// looked at it. Refusing was the right answer to that OS.
//
// ROUND 3 (#112, this one). The OS changed underneath it. The kernel writes a
// real environment block onto the child's initial stack, crt0.S adopts it into
// `environ`, and SYS_SPAWN_ENV (394) carries the caller's environment across a
// spawn. Every refusal below is therefore gone, and this is a normal env(1):
//
//   env                       prints the process environment. It is inherited
//                             now, so on a normal system it is not empty.
//   env NAME=VALUE ... CMD    sets them and runs CMD with them.
//   env -i CMD                runs CMD with an EMPTY environment. That is a
//                             real request now, and it is really honoured: a
//                             spawn with envc == 0 is distinct from a spawn
//                             with no envp operand.
//   env -u NAME               removes NAME before running.
//   env NAME=VALUE            with no command: an error, exactly as POSIX
//                             says, because there is nothing to run it in.
//
// WHAT IS STILL NOT POSSIBLE, stated so nobody re-derives it: env cannot
// change its PARENT's environment, and no program can. That is not a MayteraOS
// limitation, it is what a process boundary means.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "spawn.h"
#include "errno.h"
#include "sys/wait.h"

static const char *PROG = "env";

// The kernel block's caps (rustkern/envblock.rs). Restated here only to give a
// better message than a bare spawn failure; the enforcement is over there.
#define ENV_MAX_ENTRIES 64
#define ENV_MAX_ENTRY   511

static void usage(void)
{
    fprintf(stderr, "Usage: %s [-i] [-u NAME] [NAME=VALUE]... [COMMAND [ARG]...]\n", PROG);
}

int main(int argc, char **argv)
{
    int i = 1;
    int ignore_env = 0;

    // Options first. -u has to be COLLECTED, not just counted: it names a
    // variable to remove, and removing it is now something that can actually
    // happen. Counting it and moving on is what the previous version did, and
    // it was only correct because nothing could be removed.
    const char *unset[ENV_MAX_ENTRIES];
    int nunset = 0;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-") || !strcmp(a, "-i") || !strcmp(a, "--ignore-environment")) {
            ignore_env = 1;
            continue;
        }
        if (!strcmp(a, "-u") || !strcmp(a, "--unset")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option requires an argument -- 'u'\n", PROG);
                usage();
                return 125;
            }
            if (nunset >= ENV_MAX_ENTRIES) {
                fprintf(stderr, "%s: too many -u options\n", PROG);
                return 125;
            }
            unset[nunset++] = argv[++i];
            continue;
        }
        if (!strncmp(a, "--unset=", 8)) {
            if (nunset >= ENV_MAX_ENTRIES) {
                fprintf(stderr, "%s: too many --unset options\n", PROG);
                return 125;
            }
            unset[nunset++] = a + 8;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            // Silently treating an unknown option as the command name is how a
            // tool answers a different question with exit 0.
            fprintf(stderr, "%s: unrecognized option '%s'\n", PROG, a);
            usage();
            return 125;
        }
        break;
    }

    // ---- build the environment the child will get --------------------------
    //
    // Mutating OUR OWN environ is the whole mechanism: libc's spawn wrappers
    // hand `environ` to the kernel, so once this process's environment is the
    // one the caller asked for, the child gets it with no further plumbing.
    // env(1) exits immediately afterwards, so there is nothing left to
    // surprise.
    if (ignore_env) {
        // -i: start from nothing.
        //
        // The NAME IS COPIED OUT before unsetenv() is called, and that is not
        // tidiness. The obvious spelling is to punch a NUL over the '=' and
        // pass the entry itself, and it does not work: libc's env_find()
        // matches on "the bytes up to nlen, then an '='", so an entry whose
        // '=' has just been overwritten matches nothing and unsetenv() removes
        // nothing, and the loop below never terminates.
        while (environ && environ[0]) {
            int last = 0;
            while (environ[last + 1]) last++;
            const char *e = environ[last];
            char name[ENV_MAX_ENTRY + 1];
            size_t n = 0;
            while (e[n] && e[n] != '=' && n < sizeof(name) - 1) n++;
            if (e[n] != '=') break;         // cannot name it; stop rather than spin
            memcpy(name, e, n);
            name[n] = '\0';
            if (unsetenv(name) != 0) break;
        }
    }

    for (int u = 0; u < nunset; u++) {
        if (unsetenv(unset[u]) != 0) {
            fprintf(stderr, "%s: cannot unset '%s'\n", PROG, unset[u]);
            return 125;
        }
    }

    int assignments = 0;
    while (i < argc && strchr(argv[i], '=') != NULL) {
        char *a = argv[i];
        if (strlen(a) > ENV_MAX_ENTRY) {
            fprintf(stderr, "%s: '%s': entry longer than %d bytes\n",
                    PROG, a, ENV_MAX_ENTRY);
            return 125;
        }
        char *eq = strchr(a, '=');
        if (eq == a) {
            fprintf(stderr, "%s: '%s': empty variable name\n", PROG, a);
            return 125;
        }
        *eq = '\0';
        if (setenv(a, eq + 1, 1) != 0) {
            *eq = '=';
            fprintf(stderr, "%s: cannot set '%s'\n", PROG, a);
            return 125;
        }
        *eq = '=';
        assignments++;
        i++;
    }

    // ---- no command --------------------------------------------------------
    if (i >= argc) {
        if (assignments > 0) {
            // POSIX: assignments with no utility is an error. Printing the
            // environment instead would look like success for a request that
            // did nothing, which is this file's whole history.
            fprintf(stderr, "%s: assignment given with no command to run\n", PROG);
            usage();
            return 125;
        }
        if (environ)
            for (int k = 0; environ[k]; k++) printf("%s\n", environ[k]);
        return 0;
    }

    // ---- run the command ---------------------------------------------------
    // posix_spawnp is the shared spawner (userland/libc/spawn.c): it does the
    // $PATH search, the uppercase and ".ELF" name forms that FAT and /APPS
    // need, and it returns an ERROR NUMBER rather than setting errno. Reusing
    // it is why this file has no path-resolution code of its own.
    //
    // envp is passed EXPLICITLY as `environ` rather than as NULL. Both mean the
    // same thing to posix_spawn on this OS, but saying it is the point of the
    // program: `env` is the tool whose entire job is choosing the child's
    // environment, so it should not rely on a default to do it.
    {
        pid_t pid = 0;
        int st = 0;
        // An environ of NULL means "nothing was ever inherited or set", and
        // posix_spawn reads that as "inherit", which for -i would be the
        // opposite of what was asked. An explicit empty vector says empty.
        static char *empty_env[1] = { NULL };
        char **childenv = environ ? environ : (ignore_env ? empty_env : NULL);
        int rc = posix_spawnp(&pid, argv[i], NULL, NULL, &argv[i], childenv);
        if (rc != 0) {
            fprintf(stderr, "%s: %s: cannot run (error %d)\n", PROG, argv[i], rc);
            return (rc == ENOENT) ? 127 : 126;
        }
        if (waitpid(pid, &st, 0) < 0) {
            fprintf(stderr, "%s: %s: could not wait for the child\n", PROG, argv[i]);
            return 125;
        }
        return st;
    }
}
