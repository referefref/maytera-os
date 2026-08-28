// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stdio_file.c - FILE* stream implementation for MayteraOS userland
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "syscall.h"
#include <stdarg.h>

#define MODE_READ  0x1
#define MODE_WRITE 0x2
#define MODE_APPEND 0x4

struct FILE {
    int fd;
    int flags;         // MODE_* plus _IOFBF/_IOLBF/_IONBF << 4
    int eof;
    int error;
    int ungot;         // >= 0 when ungetc pending

    char *rd_buf;
    size_t rd_size;
    size_t rd_pos;
    size_t rd_len;

    char *wr_buf;
    size_t wr_size;
    size_t wr_pos;

    int owns_buf;
};

static FILE g_stdin_s;
static FILE g_stdout_s;
static FILE g_stderr_s;
FILE *stdin  = &g_stdin_s;
FILE *stdout = &g_stdout_s;
FILE *stderr = &g_stderr_s;

static char g_stdout_buf[BUFSIZ];
static char g_stderr_buf[64];

static int parse_mode(const char *mode, int *o_flags, int *mode_bits) {
    int f = 0;
    int m = 0;
    int plus = 0;
    char base = mode[0];
    for (int i = 1; mode[i]; i++) if (mode[i] == '+') plus = 1;

    if (base == 'r') {
        m = plus ? MODE_READ | MODE_WRITE : MODE_READ;
        f = plus ? O_RDWR : O_RDONLY;
    } else if (base == 'w') {
        m = plus ? MODE_READ | MODE_WRITE : MODE_WRITE;
        f = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    } else if (base == 'a') {
        m = plus ? MODE_READ | MODE_WRITE | MODE_APPEND : MODE_WRITE | MODE_APPEND;
        f = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    } else {
        return -1;
    }
    *o_flags = f;
    *mode_bits = m;
    return 0;
}

// task #582 (OpenArena port): rd_buf/wr_buf must both start out NULL, not
// whatever garbage bytes a non-zeroed malloc(sizeof(FILE)) (see fdopen())
// left at that offset. Previously this function only touched the buffer
// pointer belonging to the mode actually in use (rd_buf for MODE_READ,
// wr_buf for MODE_WRITE), so a write-only stream (fopen(path,"a"), no
// '+') left rd_buf uninitialized. fclose()/setvbuf() then unconditionally
// trusted rd_buf whenever owns_buf was set (fdopen() always sets it),
// so EVERY close of a write-only file called free() on that garbage
// pointer - a real, reproduced, measured heap-corruption bug: OpenArena's
// Sys_Print() does fopen("OALOG.TXT","a") + fputs + fclose() on every
// single console line, so this fired once per log line, live, right at
// FS_Startup's search-path printout ("[malloc] heap corruption: free()
// called with a pointer that does not resolve to a valid block", the
// freed "pointer" decoding to literal ASCII bytes of a nearby string like
// "q3config.cfg" - leftover heap content from a previous allocation of
// the same freed/reused block, not a real FILE-owned buffer at all).
// Zeroing both pointers here (and every size/pos field) up front, before
// the mode-specific branches below populate only what applies, makes an
// unused buffer pointer a defined NULL instead of undefined garbage.
static void stream_init(FILE *s, int fd, int mode_bits, int buf_mode,
                        char *buf, size_t bufsz) {
    s->fd = fd;
    s->flags = mode_bits | (buf_mode << 4);
    s->eof = 0;
    s->error = 0;
    s->ungot = -1;
    s->rd_buf = NULL;
    s->rd_size = 0;
    s->rd_pos = 0;
    s->rd_len = 0;
    s->wr_buf = NULL;
    s->wr_size = 0;
    s->wr_pos = 0;
    if (mode_bits & MODE_READ) {
        s->rd_buf = buf;
        s->rd_size = bufsz;
    }
    if (mode_bits & MODE_WRITE) {
        s->wr_buf = buf;
        s->wr_size = bufsz;
    }
    s->owns_buf = 0;
}

// task #582: fclose()/setvbuf() both need to free the ONE buffer fdopen()
// actually malloc'd, without (a) trusting rd_buf when the stream is
// write-only (see stream_init's comment above - that was the crash), and
// without (b) double-freeing when a mode has both bits set (e.g. "r+"),
// since stream_init() then points BOTH rd_buf and wr_buf at the SAME
// single malloc'd buffer. Free whichever of the two is non-NULL, and
// prefer rd_buf only because for the dual-mode case they are literally
// the same pointer, so it does not matter which one is chosen - it must
// just be freed exactly once.
static void free_owned_buf(FILE *f) {
    if (!f->owns_buf) return;
    if (f->rd_buf) {
        free(f->rd_buf);
    } else if (f->wr_buf) {
        free(f->wr_buf);
    }
    f->rd_buf = NULL;
    f->wr_buf = NULL;
    f->owns_buf = 0;
}

// #745 printf-shredding fix. Before this, printf()/putchar()/puts() never
// touched this FILE layer at all: they called syscall1(SYS_PUTCHAR, c)
// directly, one syscall per CHARACTER (see stdio.c). The kernel's /dev/console
// backend (drivers/console.c) mirrors every write() under 256 bytes to the
// syslog ring as ONE record, so a one-byte write() became a one-BYTE syslog
// record: a diagnostic line assembled from N putchar() calls came out as N
// shredded, unreadably interleaved entries instead of one line. The fix is
// this buffering layer, already fully built and already correct for fopen()/
// fwrite()/fprintf() - printf()/putchar()/puts() just never used it. See the
// stdio.c end of this fix for the call-site side.
//
// stdout's buffering mode is chosen HERE, once, based on whether fd 1 is a
// real interactive terminal:
//   - isatty(1) FALSE (the common case: GUI apps like the compositor, and
//     autorun/redirected tools) -> _IOLBF. A whole printf() line is now one
//     write(2, buf, n) syscall, which is one syslog record and, incidentally,
//     matches real libc's own "line-buffered unless truly non-interactive"
//     default. Flushed on '\n', on a full 4096-byte buffer, or via fflush()/
//     process exit (see exit() in stdlib.c and __libc_fini() in libc_init.c).
//   - isatty(1) TRUE (an app launched from a real PTY-backed terminal, e.g.
//     msh/vi/rogue/less run interactively) -> _IONBF, preserving EXACTLY
//     today's per-character-immediate behavior. Those apps echo keystrokes
//     and draw partial lines (shell prompts, cursor movement) with no
//     trailing newline and no fflush() of their own; buffering that would
//     make the terminal look frozen until Enter. This is the "something
//     depends on the old behavior" case the fix must not silently regress,
//     and it is why the split is by isatty(), not a blanket buffer-everything
//     change. console_fops has no .ioctl (see drivers/console.c), so
//     isatty(1) reliably reads 0 there and 1 on a real pts/N slave
//     (drivers/pty.c / drivers/tty.c implement TIOCGPGRP) - verified by
//     reading both .ioctl paths, not assumed.
//
// stderr stays _IONBF unconditionally, matching the codebase's own existing
// crash-diagnostic convention: assert.c's __assert_fail() and stack_guard.c's
// __stack_chk_fail() already write to fd 2 (raw or via fprintf(stderr,...))
// expecting it to survive an abort()/_exit() with no flush. Nothing about
// this fix touches that path.
void __stdio_init(void) {
    stream_init(stdin, 0, MODE_READ, _IOLBF, 0, 0);
    int stdout_buf_mode = isatty(1) ? _IONBF : _IOLBF;
    stream_init(stdout, 1, MODE_WRITE, stdout_buf_mode, g_stdout_buf, sizeof(g_stdout_buf));
    stream_init(stderr, 2, MODE_WRITE, _IONBF, g_stderr_buf, sizeof(g_stderr_buf));
}

FILE *fdopen(int fd, const char *mode) {
    int of, mb;
    if (parse_mode(mode, &of, &mb) < 0) { errno = EINVAL; return 0; }
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return 0; }
    char *buf = (char *)malloc(BUFSIZ);
    if (!buf) { free(f); errno = ENOMEM; return 0; }
    stream_init(f, fd, mb, _IOFBF, buf, BUFSIZ);
    f->owns_buf = 1;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int of, mb;
    if (parse_mode(mode, &of, &mb) < 0) { errno = EINVAL; return 0; }
    int fd = open(path, of);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, mode);
    if (!f) { close(fd); return 0; }
    return f;
}

static int flush_writes(FILE *f) {
    if (!(f->flags & MODE_WRITE)) return 0;
    if (f->wr_pos == 0) return 0;
    size_t off = 0;
    while (off < f->wr_pos) {
        long w = write(f->fd, f->wr_buf + off, f->wr_pos - off);
        if (w < 0) { f->error = 1; return -1; }
        off += w;
    }
    f->wr_pos = 0;
    return 0;
}

// #695: fflush() IS NOT DURABILITY, and this is the layer where that gets
// confused. flush_writes() above only drains the userland stdio buffer into
// write(2); after it returns 0 the bytes are in the KERNEL, not on the medium.
// For MayteraOS specifically, a write() to an ext2 or SMB/NFS fd only appends to
// a kernel buffer that is committed at flush time, so fflush() can succeed on a
// file that will never exist. To learn that the bytes are on the disk, call
// fsync(fileno(f)) after fflush(f) and check IT.
int fflush(FILE *f) {
    if (!f) {
        int rc = 0;
        if (stdout) rc |= flush_writes(stdout);
        if (stderr) rc |= flush_writes(stderr);
        return rc;
    }
    return flush_writes(f);
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int rc = flush_writes(f);
    int cr = close(f->fd);
    if (cr < 0) rc = EOF;
    free_owned_buf(f);
    if (f != &g_stdin_s && f != &g_stdout_s && f != &g_stderr_s) free(f);
    return rc;
}

int fileno(FILE *f) { return f ? f->fd : -1; }
int feof(FILE *f)    { return f ? f->eof : 0; }
int ferror(FILE *f)  { return f ? f->error : 0; }

static int buf_mode(FILE *f) { return (f->flags >> 4) & 0xF; }

int fputc(int c, FILE *f) {
    if (!f || !(f->flags & MODE_WRITE)) { errno = EBADF; return EOF; }
    unsigned char ch = (unsigned char)c;
    if (buf_mode(f) == _IONBF || !f->wr_buf) {
        if (write(f->fd, &ch, 1) != 1) { f->error = 1; return EOF; }
        return ch;
    }
    f->wr_buf[f->wr_pos++] = (char)ch;
    if (f->wr_pos == f->wr_size || (buf_mode(f) == _IOLBF && ch == '\n')) {
        if (flush_writes(f) < 0) return EOF;
    }
    return ch;
}

int fputs(const char *s, FILE *f) {
    while (*s) {
        if (fputc(*s++, f) == EOF) return EOF;
    }
    return 0;
}

size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f) {
    size_t total = sz * n;
    const unsigned char *p = (const unsigned char *)buf;
    size_t i;
    for (i = 0; i < total; i++) {
        if (fputc(p[i], f) == EOF) break;
    }
    return i / (sz ? sz : 1);
}

static int refill(FILE *f) {
    if (!(f->flags & MODE_READ)) return -1;
    f->rd_pos = 0;
    if (!f->rd_buf || f->rd_size == 0) {
        // unbuffered: read one byte direct
        return -2;
    }
    long r = read(f->fd, f->rd_buf, f->rd_size);
    if (r < 0) { f->error = 1; f->rd_len = 0; return -1; }
    if (r == 0) { f->eof = 1; f->rd_len = 0; return -1; }
    f->rd_len = r;
    return 0;
}

int fgetc(FILE *f) {
    if (!f || !(f->flags & MODE_READ)) { errno = EBADF; return EOF; }
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    // If stream has a write buffer with pending data, flush first
    if (f->flags & MODE_WRITE) flush_writes(f);
    if (!f->rd_buf || f->rd_size == 0) {
        unsigned char ch;
        long r = read(f->fd, &ch, 1);
        if (r < 0) { f->error = 1; return EOF; }
        if (r == 0) { f->eof = 1; return EOF; }
        return ch;
    }
    if (f->rd_pos >= f->rd_len) {
        if (refill(f) < 0) return EOF;
    }
    return (unsigned char)f->rd_buf[f->rd_pos++];
}

char *fgets(char *s, int n, FILE *f) {
    if (n <= 0 || !s) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return 0;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    f->ungot = c & 0xFF;
    f->eof = 0;
    return c;
}

size_t fread(void *buf, size_t sz, size_t n, FILE *f) {
    size_t total = sz * n;
    unsigned char *p = (unsigned char *)buf;
    size_t i;
    for (i = 0; i < total; i++) {
        int c = fgetc(f);
        if (c == EOF) break;
        p[i] = (unsigned char)c;
    }
    return i / (sz ? sz : 1);
}

int fseek(FILE *f, long off, int whence) {
    if (!f) { errno = EBADF; return -1; }
    flush_writes(f);
    f->rd_pos = f->rd_len = 0;
    f->ungot = -1;
    f->eof = 0;
    long r = lseek(f->fd, off, whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f) { errno = EBADF; return -1; }
    long r = lseek(f->fd, 0, 1);
    if (r < 0) return -1;
    // account for unread bytes in buffer
    if (f->flags & MODE_READ) r -= (long)(f->rd_len - f->rd_pos);
    if (f->flags & MODE_WRITE) r += (long)f->wr_pos;
    return r;
}

void rewind(FILE *f) { fseek(f, 0, 0); f->error = 0; f->eof = 0; }

int setvbuf(FILE *f, char *buf, int mode, size_t sz) {
    flush_writes(f);
    free_owned_buf(f);
    f->rd_buf = f->wr_buf = buf;
    f->rd_size = f->wr_size = sz;
    f->rd_pos = f->rd_len = 0;
    f->wr_pos = 0;
    f->owns_buf = 0;
    f->flags = (f->flags & 0xF) | (mode << 4);
    return 0;
}

// setbuf: the traditional (pre-setvbuf) buffering call, added for the
// Rogue port (userland/apps/rogue, task: verify+fix rogue). NULL means
// "unbuffered"; a non-NULL buffer means "fully buffered, BUFSIZ bytes"
// (glibc/BSD semantics). This is a real shared-libc primitive, not a
// rogue-specific shim, so any future port that calls setbuf() gets it
// for free instead of re-adding a private copy.
void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

// fprintf: reuse vsnprintf into a small chunked buffer then fwrite
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    return (int)fwrite(buf, 1, n, f);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

// ---------------------------------------------------------------------------
// freopen()/tmpfile()/tmpnam(), added for the Lua 5.4 port (local queue 91).
// They live down here because all three need the FILE internals above.
// ---------------------------------------------------------------------------

// freopen(): close the stream's current file and re-point the SAME FILE object
// at 'path'. The stream object survives, which is the whole reason callers use
// it: Lua's luaL_loadfilex() has already handed its FILE to a reader and needs
// the same one to continue in binary mode once it discovers the script is a
// precompiled chunk.
//
// The mode-only form freopen(NULL, mode, f) is REFUSED with EINVAL rather than
// silently ignored. It means "change this stream's mode in place", which needs
// an fd whose access mode can be changed after the fact; this kernel has no
// such call, and a caller that thinks it switched a read stream to write would
// be badly wrong.
FILE *freopen(const char *path, const char *mode, FILE *f) {
    int of, mb;
    if (!f) { errno = EBADF; return 0; }
    if (!path) { errno = EINVAL; return 0; }
    if (parse_mode(mode, &of, &mb) < 0) { errno = EINVAL; return 0; }

    flush_writes(f);
    close(f->fd);

    // Keep whatever buffer the stream already owns; only the fd and the
    // read/write mode bits change.
    char  *buf   = f->rd_buf ? f->rd_buf : f->wr_buf;
    size_t bufsz = f->rd_buf ? f->rd_size : f->wr_size;
    int    owns  = f->owns_buf;

    int fd = open(path, of);
    if (fd < 0) {
        // C says the stream is closed on failure. Leave it in a state where
        // every further operation fails rather than reading a stale fd.
        stream_init(f, -1, mb, _IOFBF, buf, bufsz);
        f->owns_buf = owns;
        f->error = 1;
        return 0;
    }
    stream_init(f, fd, mb, _IOFBF, buf, bufsz);
    f->owns_buf = owns;
    return f;
}

// tmpnam(): MayteraOS HAS NO TEMPORARY FILE FACILITY, and this says so in the
// way ISO C provides for. (tmpfile() says the same thing and lives in its own
// translation unit, tmpfile.c - see the comment there for why.)
//
// This is deliberate, and it is not laziness. tmpfile() is specified to create
// a file that is REMOVED WHEN IT IS CLOSED, which needs a filesystem that can
// unlink an open file; neither the FAT nor the ext2 path in this kernel can.
// tmpnam() is specified to return a name that IS NOT the name of an existing
// file, and there is no temporary directory anywhere in the image to put one
// in - inventing "/TMPnnnn.TMP" at the root of the system volume would be a
// filesystem-layout policy decision smuggled in through libc, and it would
// leave litter that nothing ever collects.
//
// Returning NULL is a conforming outcome for both (C99 7.21.4.3 and 7.21.4.4),
// and it is loud: every caller has to handle it, and Lua's io.tmpfile() and
// os.tmpname() turn it into a script-visible error. The alternative - handing
// back a file that is not temporary - would be the dishonest answer, and the
// caller would never find out.
//
// If MayteraOS ever grows a real temporary directory with a collector, THIS is
// the place to implement them, for every app at once.
char *tmpnam(char *s) {
    (void)s;
    errno = ENOSYS;
    return 0;
}
