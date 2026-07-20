// dev.c - In-kernel device namespace (Phase A2)

#include "dev.h"
#include "../fs/vfs.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../crypto/csprng.h"

extern void *kmalloc(size_t);
extern void kfree(void *);

// Static table; small enough that linear scan is fine. Grows when we add pts/N.
#define MAX_DEVICES 16

typedef struct {
    const char  *name;   // static string or kmalloc'd; either way, never freed
    dev_open_fn  open;
} dev_entry_t;

static dev_entry_t g_devs[MAX_DEVICES];
static int g_dev_count = 0;

int dev_register(const char *name, dev_open_fn open) {
    if (!name || !open) return -1;
    // Reject duplicates.
    for (int i = 0; i < g_dev_count; i++) {
        if (strcmp(g_devs[i].name, name) == 0) return -1;
    }
    if (g_dev_count >= MAX_DEVICES) return -1;
    g_devs[g_dev_count].name = name;
    g_devs[g_dev_count].open = open;
    g_dev_count++;
    return 0;
}

file_t *dev_open(const char *name, int flags) {
    if (!name) return NULL;
    for (int i = 0; i < g_dev_count; i++) {
        if (strcmp(g_devs[i].name, name) == 0) {
            file_t *f = g_devs[i].open(flags);
            // #487/#349: name the handle "/dev/<name>" here, at the single
            // dispatcher, so every registered device gets a path without each
            // driver's open() having to remember to record one.
            if (f) {
                char dp[VFS_FPATH_MAX];
                int n = 0;
                const char *pfx = "/dev/";
                while (pfx[n] && n < (int)sizeof(dp) - 1) { dp[n] = pfx[n]; n++; }
                int k = 0;
                while (name[k] && n < (int)sizeof(dp) - 1) { dp[n++] = name[k++]; }
                dp[n] = '\0';
                file_set_path(f, dp);
            }
            return f;
        }
    }
    return NULL;
}

// ============================================================================
// /dev/null - reads return EOF, writes are discarded
// ============================================================================

static int64_t devnull_read(file_t *f, void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return 0;  // EOF
}

static int64_t devnull_write(file_t *f, const void *buf, size_t count) {
    (void)f; (void)buf;
    return (int64_t)count;  // silently discard
}

static void devnull_release(file_t *f) {
    (void)f;
}

static const file_ops_t devnull_ops = {
    .read    = devnull_read,
    .write   = devnull_write,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = devnull_release,
    .poll    = NULL,
};

static file_t *devnull_open(int flags) {
    return file_alloc(&devnull_ops, NULL, flags);
}

// ============================================================================
// /dev/zero - reads return zeros, writes are discarded
// ============================================================================

static int64_t devzero_read(file_t *f, void *buf, size_t count) {
    (void)f;
    memset(buf, 0, count);
    return (int64_t)count;
}

static int64_t devzero_write(file_t *f, const void *buf, size_t count) {
    (void)f; (void)buf;
    return (int64_t)count;
}

static void devzero_release(file_t *f) {
    (void)f;
}

static const file_ops_t devzero_ops = {
    .read    = devzero_read,
    .write   = devzero_write,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = devzero_release,
    .poll    = NULL,
};

static file_t *devzero_open(int flags) {
    return file_alloc(&devzero_ops, NULL, flags);
}

// ============================================================================
// /dev/urandom, /dev/random - reads return CSPRNG output
//
// Previously a bare xorshift64 seeded ONCE from RDTSC on first read
// (deterministic across a run and guessable across boots, and write()s
// were silently discarded instead of stirring the pool). Now backed by
// crypto/csprng.c: an HMAC-DRBG seeded from RDSEED/RDRAND (when the CPU
// has them)+RDTSC jitter+PIT ticks+kernel addresses, auto-reseeded
// periodically. See crypto/csprng.h for the full design writeup.
//
// MayteraOS's CSPRNG never blocks once instantiated (instantiation happens
// lazily on first use if dev_init() hasn't already triggered it), so
// /dev/random and /dev/urandom are the same backing implementation, matching
// modern (post-5.6) Linux semantics: both are "give me CSPRNG output", not
// "block until an arbitrary entropy-count estimate is satisfied".
// ============================================================================

static int64_t devurandom_read(file_t *f, void *buf, size_t count) {
    (void)f;
    csprng_bytes(buf, count);
    return (int64_t)count;
}

static int64_t devurandom_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    // Stir caller-supplied bytes into the DRBG as additional entropy
    // instead of discarding them (previous behavior).
    csprng_add_entropy(buf, count);
    return (int64_t)count;
}

static void devurandom_release(file_t *f) {
    (void)f;
}

static const file_ops_t devurandom_ops = {
    .read    = devurandom_read,
    .write   = devurandom_write,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = devurandom_release,
    .poll    = NULL,
};

static file_t *devurandom_open(int flags) {
    return file_alloc(&devurandom_ops, NULL, flags);
}

// ============================================================================
// Device initialization
// ============================================================================

// Registered by drivers/console.c
extern void console_dev_init(void);

void dev_init(void) {
    console_dev_init();

    // #396: /dev/ttyACM0 - USB CDC-ACM serial (registered even if no device is
    // present yet; the factory refuses open() until one attaches).
    extern void usb_cdc_acm_dev_init(void);
    usb_cdc_acm_dev_init();

    // Real CSPRNG backing /dev/urandom + /dev/random (fix for the
    // fake-audit CRITICAL: this used to be a deterministic xorshift64).
    // Initialized explicitly here so boot log records RDSEED/RDRAND
    // availability; csprng_bytes()/csprng_add_entropy() would also
    // lazily self-init if this were skipped.
    csprng_init();

    // Self-test: pull two independent samples straight from the CSPRNG and
    // log them, so the boot log itself is evidence the old deterministic
    // xorshift64 sequence is gone (two reads differ; not all-zero/all-same).
    {
        uint8_t a[16], b[16];
        csprng_bytes(a, sizeof(a));
        csprng_bytes(b, sizeof(b));

        int same_bytes = 1;
        int all_zero_a = 1, all_zero_b = 1;
        for (size_t i = 0; i < sizeof(a); i++) {
            if (a[i] != b[i]) same_bytes = 0;
            if (a[i] != 0) all_zero_a = 0;
            if (b[i] != 0) all_zero_b = 0;
        }

        kprintf("[CSPRNG] selftest sample1: "
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
                a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]);
        kprintf("[CSPRNG] selftest sample2: "
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        kprintf("[CSPRNG] selftest: %s (samples_differ=%d all_zero_a=%d all_zero_b=%d)\n",
                (!same_bytes && !all_zero_a && !all_zero_b) ? "PASS" : "FAIL",
                !same_bytes, all_zero_a, all_zero_b);
    }

    // Register special device files
    dev_register("null", devnull_open);
    dev_register("zero", devzero_open);
    dev_register("urandom", devurandom_open);
    dev_register("random", devurandom_open);

    kprintf("[DEV] Registered /dev/null, /dev/zero, /dev/urandom, /dev/random\n");
}
