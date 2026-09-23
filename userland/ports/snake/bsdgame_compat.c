/* bsdgame_compat.c - OUR FILE, not upstream. Same one-file-of-missing-leaves
 * pattern as the tcc port's maytera_compat.c (strtold) and the sqlite port's
 * maytera_vfs.c: userland/libc is a real, mostly-POSIX freestanding libc but
 * carries none of the four BSD err(3)/warn(3) functions, not the BSD
 * random(3)/srandom(3) names (only ISO C rand()/srand() exist), and not
 * getlogin(3). All three gaps are leaves, not primitives this tree already
 * has under another name (owner rule 1 does not apply).
 *
 * PROGNAME comes from a -D on the compile line (see this port's build.sh),
 * one value per game, so err()/warn() print "snake: ..." etc. the way the
 * real BSD binaries do. Falls back to "game" if a build path forgets it.
 *
 * getlogin() HONESTY: snake.c's logit() calls getlogin() only to label a
 * best-effort play log line (see the PORT's SCOREFILE HONESTY note); it is
 * not used for any access-control decision. MayteraOS is single-session per
 * desktop login and getpwuid() (used elsewhere in snake.c) already resolves
 * the real account name, so this returns a fixed, honestly-labelled string
 * rather than inventing a fake account lookup.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef BSDGAME_PROGNAME
#define BSDGAME_PROGNAME "game"
#endif

static void bsdgame_vmsg(int eval, int with_errno, const char *fmt, va_list ap)
{
    int saved_errno = errno;
    fprintf(stderr, "%s: ", BSDGAME_PROGNAME);
    if (fmt != NULL)
        vfprintf(stderr, fmt, ap);
    if (with_errno)
        fprintf(stderr, "%s%s", (fmt != NULL) ? ": " : "", strerror(saved_errno));
    fprintf(stderr, "\n");
    if (eval >= 0)
        exit(eval);
}

void err(int eval, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bsdgame_vmsg(eval, 1, fmt, ap);
    va_end(ap);
    exit(eval); /* unreachable; silences a missing-noreturn warning */
}

void errx(int eval, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bsdgame_vmsg(eval, 0, fmt, ap);
    va_end(ap);
    exit(eval);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bsdgame_vmsg(-1, 1, fmt, ap);
    va_end(ap);
}

void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bsdgame_vmsg(-1, 0, fmt, ap);
    va_end(ap);
}

long random(void)
{
    return (long)rand();
}

void srandom(unsigned int seed)
{
    srand(seed);
}

char *getlogin(void)
{
    return "player";
}
