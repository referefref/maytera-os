// console.c - /dev/console backend (Phase A2)
//
// Writes: byte-by-byte to kputc (serial + VGA text console), and, for bursts
// <256 bytes, a single syslog_log(LOG_INFO, ...) of the same text (with any
// trailing '\n' trimmed). Reads: return 0 immediately (EOF). Phase C1 adds a
// real stdin via the controlling tty; until then, fds 0/1/2 just don't read.
//
// Every process gets this pre-opened on fds 0/1/2 by process.c.

#include "dev.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"
#include "../gui/syslog.h"

// Forward for the global syslog logger (defined in gui/syslog.c).
extern void syslog_log(log_level_t level, const char *message);

// ---- vtable ops ----

static int64_t console_read(file_t *f, void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
    // No stdin on /dev/console (yet). Returning 0 = EOF.
    return 0;
}

static int64_t console_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    if (!buf || count == 0) return 0;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < count; i++) kputc(p[i]);

    // Mirror to syslog for bursts under 256 bytes. Matches the behavior we
    // had in the fd 1/2 special case before Phase A2, so the SysLog app
    // continues to see the same traffic.
    if (count < 256) {
        char msg_buf[256];
        size_t copy_len = count < 255 ? count : 255;
        memcpy(msg_buf, p, copy_len);
        msg_buf[copy_len] = '\0';
        if (copy_len > 0 && msg_buf[copy_len - 1] == '\n') {
            msg_buf[copy_len - 1] = '\0';
        }
        if (msg_buf[0] != '\0') syslog_log(LOG_INFO, msg_buf);
    }
    return (int64_t)count;
}

static int64_t console_seek(file_t *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -1; // Character device: not seekable.
}

static int console_poll(file_t *f, int events) {
    (void)f;
    // Always writable, never readable (no stdin yet).
    return events & POLL_OUT;
}

static void console_release(file_t *f) {
    (void)f;
    // No per-file state to free; the file_t itself is kfree()'d by file_put.
}

static const file_ops_t console_fops = {
    .read    = console_read,
    .write   = console_write,
    .seek    = console_seek,
    .ioctl   = NULL,           // Phase C2 adds termios stubs
    .release = console_release,
    .poll    = console_poll,
};

// ---- factory ----

static file_t *console_open(int flags) {
    // Shared stateless device: priv=NULL is fine; every open allocates its
    // own file_t but all point at the same global fops.
    return file_alloc(&console_fops, NULL, flags);
}

void console_dev_init(void) {
    dev_register("console", console_open);
}
