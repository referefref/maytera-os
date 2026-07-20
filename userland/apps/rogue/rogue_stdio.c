
/*
 * rogue_stdio.c - Minimal FILE implementation for Rogue on MayteraOS
 */

/* Include our compat headers directly (not maytera.h to avoid conflicts) */
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"
#include "compat/stdio.h"
#include "compat/curses.h"   /* for getch() used by stdin */

/* ── syscall wrappers ─────────────────────────────────────────────── */
static long _open(const char *p, int fl) { return syscall2(SYS_OPEN,  (long)p, fl);  }
static long _close(int fd)               { return syscall1(SYS_CLOSE, fd);            }
static long _read(int fd, void *b, long n){ return syscall3(SYS_READ, fd, (long)b, n);}
static long _write(int fd, const void *b, long n) { return syscall3(SYS_WRITE, fd, (long)b, n); }
static long _seek(int fd, long off, int w){ return syscall3(SYS_SEEK, fd, off, w);   }
static void _putch(char c)               { syscall1(40 /*SYS_PUTCHAR*/, c);           }

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* ── FILE pool ─────────────────────────────────────────────────────── */
#define MAX_FILES 8

static FILE _file_pool[MAX_FILES];
static int  _pool_init = 0;

/* stdin/stdout/stderr use special fds */
static FILE _stdin_f  = { 0, 0, 0, 0, 0 };
static FILE _stdout_f = { 1, 0, 0, 0, 0 };
static FILE _stderr_f = { 2, 0, 0, 0, 0 };
FILE *stdin  = &_stdin_f;
FILE *stdout = &_stdout_f;
FILE *stderr = &_stderr_f;

static void pool_init(void) {
    if (_pool_init) return;
    for (int i = 0; i < MAX_FILES; i++) {
        _file_pool[i].fd = -1;
    }
    _pool_init = 1;
}

static FILE *pool_alloc(int fd) {
    pool_init();
    for (int i = 0; i < MAX_FILES; i++) {
        if (_file_pool[i].fd < 0) {
            _file_pool[i].fd     = fd;
            _file_pool[i].eof    = 0;
            _file_pool[i].error  = 0;
            _file_pool[i].has_pb = 0;
            return &_file_pool[i];
        }
    }
    return (void*)0;
}

/* ── Core I/O ──────────────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (mode && (mode[0] == 'w' || mode[0] == 'W')) flags = O_WRONLY|O_CREAT|O_TRUNC;
    else if (mode && (mode[0] == 'a' || mode[0] == 'A')) flags = O_WRONLY|O_CREAT|O_APPEND;
    else if (mode && mode[1] == '+')                      flags = O_RDWR|O_CREAT;
    int fd = (int)_open(path, flags);
    if (fd < 0) return (void*)0;
    FILE *f = pool_alloc(fd);
    if (!f) { _close(fd); return (void*)0; }
    return f;
}

int fclose(FILE *fp) {
    if (!fp || fp->fd < 0) return EOF;
    if (fp == stdin || fp == stdout || fp == stderr) return 0;
    _close(fp->fd);
    fp->fd = -1;
    return 0;
}

int fflush(FILE *fp) { (void)fp; return 0; }
int feof(FILE *fp)   { return fp ? fp->eof   : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }

int fgetc(FILE *fp) {
    if (!fp) return EOF;
    if (fp->has_pb) { fp->has_pb = 0; return (unsigned char)fp->pbuf; }
    if (fp == stdin) {
        /* Read from curses (getch blocks until key) */
        int c = getch();
        if (c == '\r') c = '\n';
        return c;
    }
    if (fp->fd < 0 || fp->eof) return EOF;
    unsigned char c;
    long n = _read(fp->fd, &c, 1);
    if (n <= 0) { fp->eof = 1; return EOF; }
    return (int)c;
}

int fputc(int c, FILE *fp) {
    if (!fp) return EOF;
    if (fp == stdout || fp == stderr) {
        _putch((char)c);
        return c;
    }
    if (fp->fd < 0) return EOF;
    unsigned char cc = (unsigned char)c;
    _write(fp->fd, &cc, 1);
    return c;
}

int ungetc(int c, FILE *fp) {
    if (!fp || fp->has_pb) return EOF;
    fp->pbuf = (char)c;
    fp->has_pb = 1;
    return c;
}

char *fgets(char *s, int n, FILE *fp) {
    if (!s || n <= 0 || !fp) return (void*)0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return (void*)0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp) {
    if (!s || !fp) return EOF;
    while (*s) { if (fputc((unsigned char)*s++, fp) == EOF) return EOF; }
    return 0;
}

long fread(void *ptr, long size, long nmemb, FILE *fp) {
    if (!ptr || !fp || fp->fd < 0) return 0;
    long total = size * nmemb;
    long got = _read(fp->fd, ptr, total);
    if (got <= 0) { fp->eof = 1; return 0; }
    return got / size;
}

long fwrite(const void *ptr, long size, long nmemb, FILE *fp) {
    if (!ptr || !fp) return 0;
    long total = size * nmemb;
    if (fp == stdout || fp == stderr) {
        const char *p = (const char *)ptr;
        for (long i = 0; i < total; i++) _putch(p[i]);
        return nmemb;
    }
    if (fp->fd < 0) return 0;
    long n = _write(fp->fd, ptr, total);
    return n > 0 ? n / size : 0;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp || fp->fd < 0) return -1;
    fp->eof = 0;
    return (int)_seek(fp->fd, offset, whence);
}

long ftell(FILE *fp) {
    if (!fp || fp->fd < 0) return -1;
    return _seek(fp->fd, 0, 1 /*SEEK_CUR*/);
}

void rewind(FILE *fp) {
    if (fp && fp->fd >= 0) { fp->eof = 0; _seek(fp->fd, 0, 0); }
}

/* ── Formatted I/O ─────────────────────────────────────────────────── */

/* vsnprintf provided by libc - declare it */
extern int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) fputs(buf, fp);
    return n;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

/* sscanf / sprintf / snprintf are provided by libc */
/* We just need to declare them here for compat/stdio.h */

void perror(const char *msg) {
    if (msg && *msg) { fputs(msg, stderr); fputs(": error\n", stderr); }
    else fputs("error\n", stderr);
}
