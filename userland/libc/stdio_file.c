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

static void stream_init(FILE *s, int fd, int mode_bits, int buf_mode,
                        char *buf, size_t bufsz) {
    s->fd = fd;
    s->flags = mode_bits | (buf_mode << 4);
    s->eof = 0;
    s->error = 0;
    s->ungot = -1;
    if (mode_bits & MODE_READ) {
        s->rd_buf = buf;
        s->rd_size = bufsz;
        s->rd_pos = 0;
        s->rd_len = 0;
    }
    if (mode_bits & MODE_WRITE) {
        s->wr_buf = buf;
        s->wr_size = bufsz;
        s->wr_pos = 0;
    }
    s->owns_buf = 0;
}

void __stdio_init(void) {
    stream_init(stdin, 0, MODE_READ, _IOLBF, 0, 0);
    stream_init(stdout, 1, MODE_WRITE, _IOLBF, g_stdout_buf, sizeof(g_stdout_buf));
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
    if (f->owns_buf && f->rd_buf) free(f->rd_buf);
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
    if (f->owns_buf && f->rd_buf) free(f->rd_buf);
    f->rd_buf = f->wr_buf = buf;
    f->rd_size = f->wr_size = sz;
    f->rd_pos = f->rd_len = 0;
    f->wr_pos = 0;
    f->owns_buf = 0;
    f->flags = (f->flags & 0xF) | (mode << 4);
    return 0;
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
