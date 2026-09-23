// escrow_device.c - #246/#305 AI escrow Stage 4: CAP_SCOPE_DEVICE state + glue.
//
// See escrow_device.h for the model and the privilege boundary. This file is
// the C state half of Stage 4 (the pure identity policy is rustkern/capdev.rs):
//   - the KNOWN_HOSTS / TOFU store (kernel RAM + best-effort /CONFIG/AIDEVKH),
//   - the per-contract device binding table (keyed by AI_TASK node id),
//   - the path -> device-identity glue over the existing hotplug/usb_msc layer,
//   - the gated boot self-test.
//
// WHY C, NOT RUST. Every POLICY decision here is already in Rust and merely
// called: cap_device_fp_rs / cap_device_logical_rs / cap_device_confidence_rs /
// cap_tofu_verdict_rs (rustkern/capdev.rs). What remains is entanglement glue
// Rust cannot reach in this tree: the C hotplug device tables
// (hotplug_resolve_path / hotplug_get_device / usb_msc_get_device), the fat I/O
// (fat_read_file / fat_write_file over the ext2-backed /CONFIG), and a spinlock-
// protected fixed table. Same stated justification as fs/graphfs/fold.c.
//
// NO SPIN, NO BUSY-WAIT. Every table operation is a bounded linear scan under
// spinlock_acquire_irqsave held across pure memory only. The one blocking call
// (fat_write_file for persistence) runs with NO lock held, from escrow enter/
// spawn context (syscall context, may-block), exactly like the SHADOW write in
// proc/users.c.

#include "escrow_device.h"
#include "../sync/spinlock.h"
#include "../string.h"
#include "../serial.h"
#include "fat.h"
#include "../mm/heap.h"
#include "../drivers/hotplug.h"
#include "../drivers/usb_msc.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);

// --- rustkern/capdev.rs (the pure identity policy) -------------------------
extern uint64_t cap_device_fp_rs(uint32_t vid, uint32_t pid, uint64_t cap_bytes,
                                 uint32_t block_size,
                                 const uint8_t *vendor, uint32_t vendor_len,
                                 const uint8_t *product, uint32_t product_len,
                                 const uint8_t *revision, uint32_t revision_len);
extern uint64_t cap_device_logical_rs(uint32_t vid, uint32_t pid,
                                      const uint8_t *product, uint32_t product_len);
extern uint32_t cap_device_confidence_rs(uint32_t has_unique_serial);
extern int32_t  cap_tofu_verdict_rs(uint32_t known, uint64_t stored_fp, uint64_t cur_fp);
extern int32_t  cap_device_selftest_rs(void);

// ===========================================================================
// The KNOWN_HOSTS / TOFU store.
// ===========================================================================
#define ESCDEV_KH_MAX   64
#define ESCDEV_KH_PATH  "/CONFIG/AIDEVKH"
#define ESCDEV_KH_MAGIC "AIDEVKH1\n"

typedef struct {
    int      used;
    uint64_t logical;   // logical device key (VID/PID+product)
    uint64_t fp;        // trusted full fingerprint
} escdev_kh_t;

static escdev_kh_t g_kh[ESCDEV_KH_MAX];
static spinlock_t  g_kh_lock = SPINLOCK_INIT;
static int         g_kh_loaded = 0;

// ===========================================================================
// The per-contract device binding table (CAP_SCOPE_DEVICE).
// ===========================================================================
#define ESCDEV_BIND_MAX 64

typedef struct {
    int      used;
    uint32_t task_node;   // the contract's AI_TASK node id (unique per contract)
    uint64_t fp;          // the device fingerprint this contract is scoped to
} escdev_bind_t;

static escdev_bind_t g_bind[ESCDEV_BIND_MAX];
static spinlock_t    g_bind_lock = SPINLOCK_INIT;

// ===========================================================================
// Test override (BadUSB / different-device modelling). Body is real only under
// ESCROW_DEVICE_SELFTEST; a production kernel has neither the globals nor the
// branch in escdev_fp_for_path.
// ===========================================================================
#ifdef ESCROW_DEVICE_SELFTEST
static int      g_ovr_enable = 0;
static int      g_ovr_present = 1;
static uint64_t g_ovr_fp = 0;
void escdev_test_set_override(int enable, uint64_t fp, int present) {
    g_ovr_enable = enable; g_ovr_fp = fp; g_ovr_present = present;
}
#else
void escdev_test_set_override(int enable, uint64_t fp, int present) {
    (void)enable; (void)fp; (void)present;   // no-op in production
}
#endif

// ---------------------------------------------------------------------------
// Small self-contained hex helpers (no libc strtoull in the kernel freestanding
// build; keep the persisted format text so it is inspectable).
// ---------------------------------------------------------------------------
static void escdev_u64_hex(uint64_t v, char out[17]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[15 - i] = hx[(v >> (i * 4)) & 0xF];
    out[16] = '\0';
}

static int escdev_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse up to 16 hex digits starting at *pp, advancing *pp. Returns the value;
// *ok is 1 if at least one hex digit was consumed.
static uint64_t escdev_parse_hex(const char **pp, const char *end, int *ok) {
    const char *p = *pp;
    uint64_t v = 0; int n = 0;
    while (p < end) {
        int d = escdev_hex_val(*p);
        if (d < 0) break;
        v = (v << 4) | (uint64_t)d;
        p++; n++;
        if (n >= 16) break;
    }
    *pp = p; *ok = (n > 0);
    return v;
}

// ---------------------------------------------------------------------------
// Persistence: serialise the store to /CONFIG/AIDEVKH (best-effort). Snapshots
// under the lock, then does the blocking write with NO lock held.
// ---------------------------------------------------------------------------
static void escdev_persist(void) {
    // Snapshot under the lock.
    escdev_kh_t snap[ESCDEV_KH_MAX];
    uint64_t flags = spinlock_acquire_irqsave(&g_kh_lock);
    memcpy(snap, g_kh, sizeof(snap));
    spinlock_release_irqrestore(&g_kh_lock, flags);

    // Build the text image. Each record is "<16hex> <16hex>\n" = 34 bytes; the
    // magic header plus 64 records fits comfortably.
    char buf[sizeof(ESCDEV_KH_MAGIC) + ESCDEV_KH_MAX * 34 + 8];
    unsigned pos = 0;
    const char *magic = ESCDEV_KH_MAGIC;
    while (*magic) buf[pos++] = *magic++;
    for (int i = 0; i < ESCDEV_KH_MAX; i++) {
        if (!snap[i].used) continue;
        char lh[17], fh[17];
        escdev_u64_hex(snap[i].logical, lh);
        escdev_u64_hex(snap[i].fp, fh);
        for (int j = 0; j < 16; j++) buf[pos++] = lh[j];
        buf[pos++] = ' ';
        for (int j = 0; j < 16; j++) buf[pos++] = fh[j];
        buf[pos++] = '\n';
    }
    int rc = fat_write_file(&g_fat_fs, ESCDEV_KH_PATH, buf, pos);
    if (rc != 0) {
        kprintf("[ESCDEV] TOFU store persist to %s failed (rc=%d); the in-RAM "
                "store stays authoritative this boot\n", ESCDEV_KH_PATH, rc);
    }
}

void escdev_init(void) {
    memset(g_kh, 0, sizeof(g_kh));
    memset(g_bind, 0, sizeof(g_bind));
    g_kh_loaded = 0;

    if (!g_fat_fs.mounted) {
        kprintf("[ESCDEV] no root fs at init; TOFU store starts empty\n");
        return;
    }
    uint32_t sz = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, ESCDEV_KH_PATH, &sz);
    if (!data || sz < sizeof(ESCDEV_KH_MAGIC) - 1) {
        if (data) kfree(data);
        kprintf("[ESCDEV] TOFU KNOWN_HOSTS store empty (no %s yet)\n", ESCDEV_KH_PATH);
        g_kh_loaded = 1;
        return;
    }
    const char *p = data, *end = data + sz;
    // Header line.
    if (memcmp(p, ESCDEV_KH_MAGIC, sizeof(ESCDEV_KH_MAGIC) - 1) != 0) {
        kfree(data);
        kprintf("[ESCDEV] %s has an unrecognised header; ignoring (starts empty)\n",
                ESCDEV_KH_PATH);
        g_kh_loaded = 1;
        return;
    }
    p += sizeof(ESCDEV_KH_MAGIC) - 1;
    int n = 0;
    uint64_t flags = spinlock_acquire_irqsave(&g_kh_lock);
    while (p < end && n < ESCDEV_KH_MAX) {
        int ok1 = 0, ok2 = 0;
        uint64_t lg = escdev_parse_hex(&p, end, &ok1);
        if (!ok1) break;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        uint64_t fp = escdev_parse_hex(&p, end, &ok2);
        if (!ok2) break;
        while (p < end && *p != '\n') p++;      // to end of line
        if (p < end) p++;                        // past '\n'
        g_kh[n].used = 1; g_kh[n].logical = lg; g_kh[n].fp = fp;
        n++;
    }
    spinlock_release_irqrestore(&g_kh_lock, flags);
    kfree(data);
    g_kh_loaded = 1;
    kprintf("[ESCDEV] loaded %d device identit%s from %s (KNOWN_HOSTS/TOFU)\n",
            n, n == 1 ? "y" : "ies", ESCDEV_KH_PATH);
}

// ---------------------------------------------------------------------------
// escdev_fp_for_path: the path -> device-identity glue.
// ---------------------------------------------------------------------------
int escdev_fp_for_path(const char *path, uint64_t *fp_out, uint64_t *logical_out,
                       int *conf_out) {
#ifdef ESCROW_DEVICE_SELFTEST
    if (g_ovr_enable) {
        if (!g_ovr_present) return 0;                    // bound device "gone"
        if (fp_out) *fp_out = g_ovr_fp;
        if (logical_out) *logical_out = g_ovr_fp;        // logical follows fp under test
        if (conf_out) *conf_out = ESCDEV_CONF_MODEL;
        return 1;
    }
#endif
    if (!path) return 0;
    const char *rel = 0;
    int slot = hotplug_resolve_path(path, &rel);
    if (slot < 0) return 0;                               // not on a removable volume
    hotplug_device_t *dev = hotplug_get_device(slot);
    if (!dev) return 0;

    const uint8_t *rev = (const uint8_t *)"";
    uint32_t revlen = 0;
    if (dev->msc_device_index >= 0) {
        usb_msc_device_t *m = usb_msc_get_device(dev->msc_device_index);
        if (m) { rev = (const uint8_t *)m->luns[0].revision;
                 revlen = (uint32_t)sizeof(m->luns[0].revision); }
    }
    uint64_t fp = cap_device_fp_rs(dev->vendor_id, dev->product_id,
                                   dev->capacity_bytes, dev->block_size,
                                   (const uint8_t *)dev->vendor, (uint32_t)sizeof(dev->vendor),
                                   (const uint8_t *)dev->product, (uint32_t)sizeof(dev->product),
                                   rev, revlen);
    uint64_t lg = cap_device_logical_rs(dev->vendor_id, dev->product_id,
                                        (const uint8_t *)dev->product,
                                        (uint32_t)sizeof(dev->product));
    if (fp_out) *fp_out = fp;
    if (logical_out) *logical_out = lg;
    if (conf_out) *conf_out = (int)cap_device_confidence_rs(0);
    return 1;
}

// ---------------------------------------------------------------------------
// TOFU observe.
// ---------------------------------------------------------------------------
int escdev_tofu_observe(uint64_t logical_key, uint64_t fp) {
    int verdict;
    int added = 0;

    uint64_t flags = spinlock_acquire_irqsave(&g_kh_lock);
    int found = -1, free_slot = -1;
    for (int i = 0; i < ESCDEV_KH_MAX; i++) {
        if (g_kh[i].used && g_kh[i].logical == logical_key) { found = i; break; }
        if (!g_kh[i].used && free_slot < 0) free_slot = i;
    }
    if (found >= 0) {
        verdict = cap_tofu_verdict_rs(1, g_kh[found].fp, fp);
        // A MISMATCH must NOT overwrite the trusted fingerprint.
    } else {
        verdict = cap_tofu_verdict_rs(0, 0, fp);   // NEW
        if (free_slot >= 0) {
            g_kh[free_slot].used = 1;
            g_kh[free_slot].logical = logical_key;
            g_kh[free_slot].fp = fp;
            added = 1;
        }
        // If the table is full we still return NEW without recording. The
        // authority is the per-contract binding (re-checked every write), not
        // this cross-device memory, so a full KNOWN_HOSTS table must not deny.
    }
    spinlock_release_irqrestore(&g_kh_lock, flags);

    if (added) escdev_persist();   // blocking, NO lock held
    return verdict;
}

// ---------------------------------------------------------------------------
// Per-contract binding table.
// ---------------------------------------------------------------------------
int escdev_bind_set(uint32_t task_node, uint64_t fp) {
    if (task_node == 0) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&g_bind_lock);
    int slot = -1;
    for (int i = 0; i < ESCDEV_BIND_MAX; i++) {
        if (g_bind[i].used && g_bind[i].task_node == task_node) { slot = i; break; }
        if (!g_bind[i].used && slot < 0) slot = i;
    }
    // Prefer an existing entry for this task_node; else the first free slot.
    for (int i = 0; i < ESCDEV_BIND_MAX && slot < 0; i++)
        if (!g_bind[i].used) slot = i;
    if (slot < 0) { spinlock_release_irqrestore(&g_bind_lock, flags); return -1; }
    g_bind[slot].used = 1;
    g_bind[slot].task_node = task_node;
    g_bind[slot].fp = fp;
    spinlock_release_irqrestore(&g_bind_lock, flags);
    return 0;
}

int escdev_bind_get(uint32_t task_node, uint64_t *fp_out) {
    if (task_node == 0) return 0;
    int r = 0;
    uint64_t flags = spinlock_acquire_irqsave(&g_bind_lock);
    for (int i = 0; i < ESCDEV_BIND_MAX; i++) {
        if (g_bind[i].used && g_bind[i].task_node == task_node) {
            if (fp_out) *fp_out = g_bind[i].fp;
            r = 1;
            break;
        }
    }
    spinlock_release_irqrestore(&g_bind_lock, flags);
    return r;
}

void escdev_bind_clear(uint32_t task_node) {
    if (task_node == 0) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_bind_lock);
    for (int i = 0; i < ESCDEV_BIND_MAX; i++) {
        if (g_bind[i].used && g_bind[i].task_node == task_node) {
            g_bind[i].used = 0; g_bind[i].task_node = 0; g_bind[i].fp = 0;
        }
    }
    spinlock_release_irqrestore(&g_bind_lock, flags);
}

// ---------------------------------------------------------------------------
// The device-scope enforcement decision (called from escrow_fs_guard()).
// ---------------------------------------------------------------------------
int escdev_check_write(uint32_t task_node, const char *path) {
    uint64_t bound = 0;
    if (!escdev_bind_get(task_node, &bound)) return 1;   // not device-scoped: allow
    uint64_t cur = 0;
    if (!escdev_fp_for_path(path, &cur, 0, 0)) {
        // The path is not on ANY removable device (the bound device is gone, or
        // the write targets some other fs). A device-scoped grant cannot be
        // satisfied off its device: fail closed.
        return 0;
    }
    if (cur != bound) return 0;   // different device, or identity changed (BadUSB). Fail closed.
    return 1;
}

// ===========================================================================
// Gated boot self-test.
// ===========================================================================
#ifdef ESCROW_DEVICE_SELFTEST
static int escdev_st_ok = 1;
static void escdev_st(const char *name, int ok) {
    if (!ok) escdev_st_ok = 0;
    kprintf("[ESCDEV-TEST] %-46s %s\n", name, ok ? "PASS" : "FAIL");
}

void escdev_selftest(void) {
    kprintf("\n========== ESCDEV STAGE 4 (CAP_SCOPE_DEVICE) SELF-TEST ==========\n");
    escdev_st_ok = 1;

    // 1. The pure Rust identity policy (determinism, BadUSB detection, TOFU
    //    verdicts, honest MODEL confidence).
    escdev_st("capdev.rs policy self-test (rs==expected)", cap_device_selftest_rs() == 0);

    // 2. The guard decision, exercised through the REAL escrow_fs_guard path via
    //    escdev_check_write, with fingerprints driven by the test override so a
    //    device swap / identity change is deterministic in a headless VM.
    const uint32_t T = 0x06AB0001u;                 // a synthetic AI_TASK node id
    const uint64_t FP_X = 0xA1A1A1A1A1A1A1A1ull;    // "device X" fingerprint
    const uint64_t FP_Y = 0xB2B2B2B2B2B2B2B2ull;    // a DIFFERENT device's fingerprint
    (void)escdev_bind_set(T, FP_X);                  // scope this contract to device X

    // 2a. ALLOW on the correct device: live fp == bound X.
    escdev_test_set_override(1, FP_X, 1);
    escdev_st("device-scoped write ALLOWED on the bound device", escdev_check_write(T, "/USBX/a") == 1);

    // 2b. REFUSE the SAME path on a DIFFERENT device: live fp == Y != bound X.
    escdev_test_set_override(1, FP_Y, 1);
    escdev_st("SAME path on a DIFFERENT device REFUSED (fail closed)", escdev_check_write(T, "/USBX/a") == 0);

    // 2c. FAIL CLOSED when the identity CHANGES under a live grant (BadUSB):
    //     same slot, live fp flipped from X to X-with-one-bit-changed.
    escdev_test_set_override(1, FP_X ^ 0x1ull, 1);
    escdev_st("identity change under live grant REFUSED (BadUSB)", escdev_check_write(T, "/USBX/a") == 0);

    // 2d. FAIL CLOSED when the bound device is GONE (re-enumeration / eject).
    escdev_test_set_override(1, 0, 0);
    escdev_st("bound device gone -> write REFUSED (fail closed)", escdev_check_write(T, "/USBX/a") == 0);

    // 2e. A NON-device-scoped contract is unaffected (Stage 1-3 behaviour): a
    //     task with no binding is allowed by the device check (path/grant scope
    //     already enforced upstream).
    escdev_test_set_override(1, FP_Y, 1);
    escdev_st("non-device-scoped contract unaffected by device check", escdev_check_write(0x06AB0002u, "/USBX/a") == 1);

    escdev_test_set_override(0, 0, 1);   // disable override
    escdev_bind_clear(T);

    // 3. TOFU store round-trip in RAM: first sight NEW, exact match OK, changed
    //    fingerprint for the same logical key MISMATCH (and the trusted record
    //    is NOT overwritten).
    uint64_t LG = 0xC3C3C3C3C3C3C3C3ull;
    escdev_st("TOFU first sight -> NEW", escdev_tofu_observe(LG, FP_X) == ESCDEV_TOFU_NEW);
    escdev_st("TOFU same fingerprint -> OK", escdev_tofu_observe(LG, FP_X) == ESCDEV_TOFU_OK);
    escdev_st("TOFU changed fingerprint -> MISMATCH", escdev_tofu_observe(LG, FP_Y) == ESCDEV_TOFU_MISMATCH);
    escdev_st("TOFU trusted record NOT overwritten", escdev_tofu_observe(LG, FP_X) == ESCDEV_TOFU_OK);

    // 4. The identity SOURCE on a REAL enumerated device, if one is mounted.
    //    Scan hotplug for a mounted, readable, removable volume and derive its
    //    real fingerprint via the same escdev_fp_for_path used in enforcement.
    int found_real = 0;
    for (int i = 0; i < 8; i++) {
        hotplug_device_t *d = hotplug_get_device(i);
        if (!d || d->status != HOTPLUG_STATUS_MOUNTED) continue;
        if (!d->mount_point[0]) continue;
        char probe[64];
        int k = 0;
        for (; d->mount_point[k] && k < 40; k++) probe[k] = d->mount_point[k];
        probe[k] = '\0';
        uint64_t fp = 0, lg = 0; int conf = 0;
        if (escdev_fp_for_path(probe, &fp, &lg, &conf) == 1 && fp != 0) {
            found_real = 1;
            kprintf("[ESCDEV-TEST] REAL device at %s: fp=%016lx logical=%016lx "
                    "confidence=%s (VID=%04x PID=%04x cap=%lu)\n",
                    probe, (unsigned long)fp, (unsigned long)lg,
                    conf == ESCDEV_CONF_SERIAL ? "SERIAL" :
                    conf == ESCDEV_CONF_MODEL ? "MODEL(not-unique)" : "NONE",
                    d->vendor_id, d->product_id, (unsigned long)d->capacity_bytes);

            // Build a sub-path on THIS real volume: "<mount>/e4.probe".
            char sub[80];
            int m = 0;
            for (; probe[m] && m < 60; m++) sub[m] = probe[m];
            if (m > 0 && sub[m - 1] == '/') m--;   // avoid a double slash
            const char *tail = "/e4.probe";
            for (int t = 0; tail[t]; t++) sub[m++] = tail[t];
            sub[m] = '\0';

            // 5. The REAL enforcement decision on a REAL device, override OFF, so
            //    the live fingerprint is derived from the actual enumerated
            //    descriptors and compared to the bound fingerprint. This is the
            //    exact decision escrow_fs_guard() runs for a device-scoped actor.
            const uint32_t RT = 0x06AB00FFu;
            (void)escdev_bind_set(RT, fp);                       // scope to the REAL device
            escdev_st("REAL device: in-scope write ALLOWED on bound device",
                      escdev_check_write(RT, sub) == 1);
            // A path NOT on the bound removable device (root fs) -> not that
            // device -> REFUSED (this is "same request, off the scoped device").
            escdev_st("REAL device: path off the scoped device REFUSED",
                      escdev_check_write(RT, "/e4.offdevice") == 0);
            // BadUSB: the device re-enumerates with a changed identity while the
            // grant is live -> REFUSED (fail closed). Modelled by flipping the
            // live fingerprint via the override (a real re-plug is not scriptable
            // headlessly); the DECISION exercised is the real one.
            escdev_test_set_override(1, fp ^ 0x1ull, 1);
            escdev_st("REAL device: identity change (BadUSB) REFUSED",
                      escdev_check_write(RT, sub) == 0);
            escdev_test_set_override(0, 0, 1);
            escdev_bind_clear(RT);
            break;
        }
    }
    // If no removable volume is MOUNTED (a second boot-present USB stick is
    // enumerated but not auto-mounted by the hotplug manager), still prove the
    // identity SOURCE works on a REAL enumerated device by deriving a real
    // fingerprint straight from the usb_msc descriptors escdev_fp_for_path reads.
    if (!found_real) {
        int nd = usb_msc_get_device_count();
        for (int i = 0; i < nd; i++) {
            usb_msc_device_t *m = usb_msc_get_device(i);
            if (!m || !m->present) continue;
            uint64_t cap = m->num_blocks * (uint64_t)m->block_size;
            uint64_t fp = cap_device_fp_rs(
                m->vendor_id, m->product_id, cap, m->block_size,
                (const uint8_t *)m->vendor, (uint32_t)sizeof(m->vendor),
                (const uint8_t *)m->product, (uint32_t)sizeof(m->product),
                (const uint8_t *)m->luns[0].revision, (uint32_t)sizeof(m->luns[0].revision));
            uint64_t lg = cap_device_logical_rs(m->vendor_id, m->product_id,
                (const uint8_t *)m->product, (uint32_t)sizeof(m->product));
            kprintf("[ESCDEV-TEST] REAL enumerated USB MSC dev %d: fp=%016lx "
                    "logical=%016lx VID=%04x PID=%04x '%s' '%s' cap=%luMB "
                    "confidence=MODEL(not-unique) (identity source proven on real "
                    "hardware descriptors)\n",
                    i, (unsigned long)fp, (unsigned long)lg,
                    m->vendor_id, m->product_id, m->vendor, m->product,
                    (unsigned long)(cap >> 20));
            found_real = 1;
        }
    }
    if (!found_real)
        kprintf("[ESCDEV-TEST] (no removable device enumerated; identity-source "
                "check on synthetic fields only)\n");

    kprintf("[ESCDEV-TEST] SUMMARY %s\n", escdev_st_ok ? "OVERALL PASS" : "OVERALL FAIL");
    kprintf("========== ESCDEV STAGE 4 SELF-TEST END ==========\n");
}
#else
void escdev_selftest(void) { /* not built in production */ }
#endif
