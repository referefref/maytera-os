// libc_gap.cpp - POSIX gap-fillers SHARED by the MayteraOS C++ app ports.
//
// SHARED FILE (userland/libcompat). AssaultCube and OpenArena each carried a
// byte-identical private copy of this file until #745 promoted it here. Do
// NOT fork a private copy back into an app directory: extend THIS file, then
// go back and confirm every existing consumer still builds. Consumers today:
//   userland/apps/assaultcube, userland/apps/openarena
//
// NAMING NOTE: tmpfile() still writes /AC_TMP_*.TMP and syslog() still
// defaults its ident to assaultcube. Both are AssaultCube-flavoured but both
// are OBSERVABLE behaviour (a filename on disk, a log prefix), so #745 left
// them exactly as they were: the point of that change was to prove the move
// altered no behaviour at all. Renaming them is a separate change.
//
// A handful of POSIX functions AC's own upstream code calls directly (not
// through any MayteraOS or SDL abstraction) that neither the MayteraOS libc
// nor this port's other compat files provide: syslog family, setlocale,
// fstatvfs, tmpfile, backtrace/backtrace_symbols, system(). Unlike
// sdlshim.cpp's raw-syscall approach (compat/mos_syscalls.h), these need the
// REAL HOST struct layouts (struct statvfs, etc), so this file is built
// exactly like the vendor .cpp files themselves: host <syslog.h>/
// <sys/statvfs.h>/<execinfo.h>/<locale.h> headers, extern "C" linkage,
// linked against libc.a for anything real underneath (fopen, printf, vprintf).
//
// Scope call: these are AC/POSIX-emulation-specific (unlike the math.c/
// stdio.c/stdlib.c/scanf.c additions this pass also made to the SHARED
// libc, which are genuinely reusable primitives). Real where cheap
// (syslog actually prints, tmpfile actually creates a file, setlocale
// reports success), honest safe stub otherwise (fstatvfs reports a large
// static free-space figure so a low-disk-space check never spuriously
// trips; backtrace/system report "unavailable", matching how real libc
// behaves on a platform without process-exec or unwind support).
//
// No em-dashes per repo writing-style rule.
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/statvfs.h>
#include <execinfo.h>
#include <locale.h>
#include <unistd.h>   // getpid(), a real MayteraOS libc.a symbol

extern "C" {

// ---- syslog family: real, routed to stdout (this OS's serial console),
// not a no-op, so `-e5,serverlog...` style logging is actually visible. ----
static int g_syslog_facility = 0;
static char g_syslog_ident[128] = "";

void openlog(const char *ident, int option, int facility) {
    (void)option;
    g_syslog_facility = facility;
    if (ident) {
        int i = 0;
        while (ident[i] && i < (int)sizeof(g_syslog_ident) - 1) { g_syslog_ident[i] = ident[i]; i++; }
        g_syslog_ident[i] = 0;
    } else {
        g_syslog_ident[0] = 0;
    }
}
void syslog(int priority, const char *format, ...) {
    (void)priority;
    printf("[syslog:%s] ", g_syslog_ident[0] ? g_syslog_ident : "assaultcube");
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
    printf("\n");
}
void closelog(void) { g_syslog_ident[0] = 0; }

// ---- locale: MayteraOS has no locale tables at all; "POSIX"/"C" is
// already this platform's only behavior, so reporting success with
// whatever was asked for is accurate, not a lie. ----
char *setlocale(int category, const char *locale) {
    (void)category;
    static char buf[64] = "POSIX";
    if (locale) {
        int i = 0;
        while (locale[i] && i < (int)sizeof(buf) - 1) { buf[i] = locale[i]; i++; }
        buf[i] = 0;
    }
    return buf;
}

// ---- filesystem free-space query: stream.cpp only uses this for a
// low-disk-space warning before writing a log/demo file. MayteraOS has no
// syscall exposing real free-space (userland/libc/syscall.h has none), so
// this reports a large static figure (never spuriously blocks a write)
// rather than fabricating a specific real number. ----
int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd;
    if (!buf) return -1;
    for (size_t i = 0; i < sizeof(*buf); i++) ((char *)buf)[i] = 0;
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 1000000;
    buf->f_bfree = 500000;
    buf->f_bavail = 500000;
    return 0;
}

// ---- anonymous temp file: real, via the same fopen() the rest of this
// port already links against (MayteraOS libc.a). A predictable-but-unique
// name is good enough here (single-process game, not a multi-user shared
// /tmp), unlike glibc's O_TMPFILE/unlink trick this platform has no
// equivalent primitive for. ----
FILE *tmpfile(void) {
    static int counter = 0;
    char path[64];
    snprintf(path, sizeof(path), "/AC_TMP_%d_%d.TMP", (int)getpid(), counter++);
    return fopen(path, "w+b");
}

// ---- stack unwinding: no unwind tables/frame-pointer walk implemented for
// this port; tools.cpp's crash-dump path degrades to "no backtrace
// available" instead of crashing itself trying to produce one. ----
int backtrace(void **buffer, int size) { (void)buffer; (void)size; return 0; }
char **backtrace_symbols(void *const *buffer, int size) { (void)buffer; (void)size; return 0; }

// ---- process exec: no shell/exec facility wired up for this port yet
// (wizard.cpp's use is the interactive dedicated-server setup tool, not
// the graphical client's own game loop). Matches real system()'s own
// documented "command processor unavailable" return of nonzero via a
// NULL-argument probe; a real command always then also fails. ----
int system(const char *command) { (void)command; return -1; }

} // extern "C"
