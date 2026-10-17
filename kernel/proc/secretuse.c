// proc/secretuse.c - #secretuse: the SECRET-USE PROXY's kernel glue.
//
// Read proc/secretuse.h first: it holds the design, the refusals, and the honest
// limits. This file is the plumbing.
//
// WHY THIS FILE IS C, STATED PER THE RUST-FIRST MANDATE.
// It contains NO algorithm. Every byte of arithmetic and every parse of
// attacker-influenceable data is in rustkern/totp.rs (base32, RFC 4226
// truncation, the store record parser, the tier table) and rustkern/secstore.rs
// (the AEAD envelope). What is left here is uaccess with SMAP, a wait-queue
// mutex, fat_read_file/fat_write_file, perms_set, and a table lookup. That is
// entanglement with paging, uaccess and the existing C filesystem and permission
// APIs, which the mandate names as a legitimate reason, and moving it to Rust
// would add a large FFI surface around code that computes nothing. The split is
// the same one secstore.rs drew for itself: the envelope in Rust, the AEAD and
// the file I/O in C.
//
// WHERE THE FILES LIVE, and why not in the user's home.
//   /CONFIG/SECRETS/            root:root 0700
//   /CONFIG/SECRETS/U<uid>.SLT  16 bytes, the per-user KDF salt (PUBLIC by design)
//   /CONFIG/SECRETS/U<uid>.SEC  the secstore.rs container
// One root-only subtree, so "Ring 3 cannot read the store" is one directory fact
// rather than a per-file audit, and nothing root-owned appears inside a user's
// home. The salt is public on purpose (see secstore.rs's header); it is kept
// root-only anyway because there is no reason to hand it out.
//
// AND THE HONEST LAYERING: the permission is NOT what protects the secret.
// Backlog A2 measured that /CONFIG's inode modes say -rwxr-xr-x while PERMS.DB
// says 0600, so a Ring-3 read is refused by perms_check() and by nothing else.
// The confidentiality rests on the KEY, which is derived from the login password
// and written to no file at all. Someone who takes the disk gets ciphertext. The
// 0700 is defence in depth against a Ring-3 process copying the ciphertext away
// to attack the password offline, which is worth having and is not the main
// control.

#include "secretuse.h"
#include "users.h"
#include "process.h"
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "../fs/bootlog.h"
#include "../fs/userconf.h"
#include "../crypto/crypto.h"
#include "../crypto/csprng.h"
#include "../crypto/pbkdf2.h"
#include "../security/validate.h"
#include "../sync/spinlock.h"
#include "../sync/waitq.h"

extern fat_fs_t g_fat_fs;

// ---------------------------------------------------------------------------
// rustkern/secstore.rs - the container. rustkern/totp.rs - everything else.
// ---------------------------------------------------------------------------
#define SECSTORE_HDR_LEN 64
#define SECSTORE_TAG_LEN 16
#define SECSTORE_KEY_LEN 32
#define SECSTORE_SALT_LEN 16
#define SECSTORE_NONCE_LEN 12
#define SECSTORE_MAX_PLAINTEXT 8192

int secstore_seal_rs(const uint8_t *key, const uint8_t *salt, const uint8_t *nonce,
                     uint32_t iters, const uint8_t *plaintext, size_t pt_len,
                     uint8_t *out, size_t out_cap);
int secstore_open_rs(const uint8_t *key, const uint8_t *buf, size_t len,
                     uint8_t *out, size_t out_cap);

// Mirror of #[repr(C)] struct TotpRec in rustkern/totp.rs. The sizeof lock is the
// FFI contract: a one-byte disagreement here misreads every field after the first.
#define TOTP_NAME_MAX   48
#define TOTP_SECRET_MAX 64
#define TOTP_B32_MAX    128
#define TOTP_MAX_RECS   32

typedef struct {
    uint8_t  issuer[TOTP_NAME_MAX];
    uint8_t  label[TOTP_NAME_MAX];
    uint8_t  secret[TOTP_SECRET_MAX];
    uint16_t secret_len;
    uint16_t digits;
    uint16_t period;
    uint16_t alg;
} totp_rec_t;
_Static_assert(sizeof(totp_rec_t) == 168, "totp_rec_t must match rustkern/totp.rs TotpRec");

int  totp_parse_store_rs(const uint8_t *buf, size_t len, totp_rec_t *recs, size_t max_recs);
int  totp_format_store_rs(const totp_rec_t *recs, size_t count, uint8_t *out, size_t out_cap);
int  totp_make_rec_rs(totp_rec_t *rec, const uint8_t *issuer, const uint8_t *label,
                      const uint8_t *b32, uint32_t digits, uint32_t period, uint32_t alg);
int  totp_code_rs(const totp_rec_t *rec, int64_t unix_sec, uint8_t *out, size_t out_cap);
void totp_wipe_rec_rs(totp_rec_t *rec);
int  totp_selftest_rs(uint32_t *out_checks);
uint32_t secret_keysrc_for_class_rs(uint32_t klass);
int  secret_keysrc_is_implemented_rs(uint32_t src);

#define SECRET_CLASS_TOTP     1u
#define SECRET_KEYSRC_NONE    0u

// rustkern/ktime.rs. UTC seconds since the epoch, or 0 if the RTC has never
// presented a plausible date. 0 is a REFUSAL and must never be treated as 1970.
extern int64_t realtime_sec_rs(void);

// ---------------------------------------------------------------------------
// THE KEYRING.
//
// One slot per authenticated uid. Static, not kmalloc'd: an allocation failure
// path in a credential store is a path that is never tested, and a fixed table
// cannot be used after free. The cost is ~21 KiB of BSS, which is the price of
// not having those two failure modes.
//
// LOCKING. Two different locks, because two different things need protecting and
// one of them blocks.
//   * g_slot_lock (spinlock) guards the SLOT TABLE's shape: claiming, finding and
//     releasing a slot. Held for a handful of instructions, never across I/O.
//   * g_busy + g_busy_wq is a SLEEPABLE MUTEX built from the shared wait queue,
//     and it serialises the whole read-decrypt-modify-encrypt-write sequence.
//     It exists because that sequence does FILE I/O, and holding a spinlock
//     across a blocking read is exactly what wq_assert_may_block() catches. There
//     is no hand-rolled poll here: sec_enter() is a single wait_event() whose
//     condition is an atomic claim, which is the documented use of the primitive.
// ---------------------------------------------------------------------------
#define SECKEY_SLOTS 4

typedef struct {
    int      in_use;
    uint32_t uid;
    uint8_t  key[SECSTORE_KEY_LEN];
    uint8_t  salt[SECSTORE_SALT_LEN];
    uint32_t iters;
    int      loaded;          // recs[] reflect the container on disk
    int      count;
    totp_rec_t recs[TOTP_MAX_RECS];
} seckey_slot_t;

static seckey_slot_t g_slots[SECKEY_SLOTS];
static spinlock_t g_slot_lock;
static wait_queue_head_t g_busy_wq;
static int g_busy;
static int g_inited;

static void seckey_init_once(void) {
    if (g_inited) return;
    spinlock_init_named(&g_slot_lock, "seckey");
    wait_queue_head_init(&g_busy_wq);
    g_busy = 0;
    g_inited = 1;
}

// Atomic claim of the sleepable mutex. Returns 1 on success, 0 if held.
static int sec_try_claim(void) {
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    int got = 0;
    if (!g_busy) { g_busy = 1; got = 1; }
    spinlock_release_irqrestore(&g_slot_lock, f);
    return got;
}

static void sec_enter(void) {
    // wait_event() evaluates the condition zero or more times and returns when it
    // is true, so the claim happens inside the condition and there is no window
    // between "observed free" and "took it".
    (void)wait_event(&g_busy_wq, sec_try_claim());
}

static void sec_leave(void) {
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    g_busy = 0;
    spinlock_release_irqrestore(&g_slot_lock, f);
    wake_up(&g_busy_wq);
}

// Slot lookup. Caller holds the sleepable mutex, so the table cannot change
// under it; the spinlock is still taken because seckey_wipe_uid() can run from
// sys_session_lock() without the mutex (it must never block).
static seckey_slot_t *slot_find(uint32_t uid) {
    seckey_slot_t *r = NULL;
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    for (int i = 0; i < SECKEY_SLOTS; i++)
        if (g_slots[i].in_use && g_slots[i].uid == uid) { r = &g_slots[i]; break; }
    spinlock_release_irqrestore(&g_slot_lock, f);
    return r;
}

static void slot_clear(seckey_slot_t *s) {
    for (int i = 0; i < TOTP_MAX_RECS; i++) totp_wipe_rec_rs(&s->recs[i]);
    crypto_zero(s->key, sizeof(s->key));
    crypto_zero(s->salt, sizeof(s->salt));
    s->iters = 0;
    s->count = 0;
    s->loaded = 0;
    s->uid = 0;
    s->in_use = 0;
}

void seckey_wipe_uid(uint32_t uid) {
    seckey_init_once();
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    for (int i = 0; i < SECKEY_SLOTS; i++)
        if (g_slots[i].in_use && g_slots[i].uid == uid) slot_clear(&g_slots[i]);
    spinlock_release_irqrestore(&g_slot_lock, f);
}

void seckey_wipe_all(void) {
    seckey_init_once();
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    for (int i = 0; i < SECKEY_SLOTS; i++)
        if (g_slots[i].in_use) slot_clear(&g_slots[i]);
    spinlock_release_irqrestore(&g_slot_lock, f);
}

// ---------------------------------------------------------------------------
// Paths.
// ---------------------------------------------------------------------------
#define SECRETS_DIR "/CONFIG/SECRETS"

static void u32_to_dec(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 11);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

// Builds SECRETS_DIR "/U<uid>." <ext> into out (cap >= 48). `ext` is 3 chars.
static void secret_path(uint32_t uid, const char *ext, char *out, uint32_t cap) {
    char num[12];
    u32_to_dec(uid, num);
    // Bounded by construction: 15 + 1 + 1 + 10 + 1 + 3 + 1 = 32 < 48. The cap is
    // checked anyway so a future longer prefix cannot silently truncate into a
    // DIFFERENT path, which would be a different user's store.
    uint32_t need = (uint32_t)(strlen(SECRETS_DIR) + 2 + strlen(num) + 1 + strlen(ext) + 1);
    if (need > cap) { out[0] = '\0'; return; }
    strcpy(out, SECRETS_DIR);
    strcat(out, "/U");
    strcat(out, num);
    strcat(out, ".");
    strcat(out, ext);
}

// Create SECRETS_DIR root-owned 0700 if it is not there. Idempotent.
static void secrets_dir_ensure(void) {
    fat_mkdir(&g_fat_fs, SECRETS_DIR);
    perms_set(SECRETS_DIR, 0, 0, 0700);
}

// Write a file root:root 0600, and report whether the permission actually landed
// on the medium. A file written with the right bytes and the wrong mode is a file
// that leaks, so the perms_sync() result is not discarded.
static int write_root_0600(const char *path, const void *data, uint32_t len) {
    if (fat_write_file(&g_fat_fs, path, data, len) != 0) return -1;
    perms_set(path, 0, 0, 0600);
    if (perms_sync() != 0) {
        kprintf("[SECRET] WARNING: %s written but PERMS.DB is NOT on the medium; "
                "its 0600 will not survive a reboot\n", path);
        bootlog_write("[SECRET] perms_sync failed after writing %s", path);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The per-user salt. Read it, or create it from the CSPRNG on first use.
//
// The salt is PUBLIC (secstore.rs's header explains why that is safe and why it
// must NOT be the SHADOW salt: if it were, the derived key would equal the
// verifier output that /CONFIG/SHADOW already stores in cleartext hex, and reading
// SHADOW would hand over the store key).
// ---------------------------------------------------------------------------
static int salt_load_or_create(uint32_t uid, uint8_t salt[SECSTORE_SALT_LEN]) {
    char path[48];
    secret_path(uid, "SLT", path, sizeof(path));
    if (!path[0]) return -1;

    uint32_t sz = 0;
    void *buf = fat_read_file(&g_fat_fs, path, &sz);
    if (buf) {
        if (sz == SECSTORE_SALT_LEN) {
            memcpy(salt, buf, SECSTORE_SALT_LEN);
            crypto_zero(buf, sz);
            kfree(buf);
            return 0;
        }
        // A salt of the wrong length is REFUSED, not regenerated. Regenerating it
        // would make every existing container permanently unopenable while
        // looking like a successful login, which is how a user silently loses
        // their second factor. A human has to decide what happened here.
        kprintf("[SECRET] %s is %u bytes, expected %d; REFUSING to regenerate "
                "(that would orphan the existing store)\n",
                path, (unsigned)sz, SECSTORE_SALT_LEN);
        bootlog_write("[SECRET] bad salt length for uid=%u; store left alone", (unsigned)uid);
        crypto_zero(buf, sz);
        kfree(buf);
        return -1;
    }

    secrets_dir_ensure();
    csprng_bytes(salt, SECSTORE_SALT_LEN);
    if (write_root_0600(path, salt, SECSTORE_SALT_LEN) != 0) {
        crypto_zero(salt, SECSTORE_SALT_LEN);
        kprintf("[SECRET] could not write %s; no secret store for uid=%u\n",
                path, (unsigned)uid);
        return -1;
    }
    return 0;
}

// Does this user have anything to unlock? Used to skip the second PBKDF2 for the
// (common) user with no enrolled secrets, so an ordinary login costs nothing extra.
static int user_has_store(uint32_t uid, const char *home) {
    char path[48];
    uint32_t sz = 0;
    void *b;

    secret_path(uid, "SEC", path, sizeof(path));
    if (path[0]) {
        b = fat_read_file(&g_fat_fs, path, &sz);
        if (b) { kfree(b); return 1; }
    }
    // A legacy XOR MFA.DB is also a reason to unlock: it has to be migrated, and
    // migration needs the key.
    if (home && home[0]) {
        char legacy[256];
        if (userconf_kpath_home(home, "MFA.DB", legacy, sizeof(legacy)) == 0) {
            sz = 0;
            b = fat_read_file(&g_fat_fs, legacy, &sz);
            if (b) { crypto_zero(b, sz); kfree(b); return 1; }
        }
    }
    return 0;
}

// Find a user table entry by name, or NULL.
static user_entry_t *user_by_name(const char *username) {
    int n = 0;
    user_entry_t *tbl = users_all(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(tbl[i].username, username) == 0) return &tbl[i];
    return NULL;
}

// Install a key into this uid's keyring slot, discarding any decrypted table.
// CALLER MUST HOLD THE SLEEPABLE MUTEX: without it, installing a key while a
// sys_secret_use() call is mid-flight on the same slot lets that call finish a
// blocking file read and then decrypt with a key that changed underneath it. The
// result is only a failed tag (fail-closed, not a leak), but "your store will not
// open" arriving because you logged in on another terminal is a bug, not a policy.
static void slot_install_locked(uint32_t uid, const uint8_t key[SECSTORE_KEY_LEN],
                                const uint8_t salt[SECSTORE_SALT_LEN]) {
    uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
    seckey_slot_t *s = NULL;
    for (int i = 0; i < SECKEY_SLOTS; i++)
        if (g_slots[i].in_use && g_slots[i].uid == uid) { s = &g_slots[i]; break; }
    if (!s)
        for (int i = 0; i < SECKEY_SLOTS; i++)
            if (!g_slots[i].in_use) { s = &g_slots[i]; break; }
    // Evict slot 0 rather than refuse: a bounded table that turns away the user who
    // just logged in is worse than one that forgets an older session.
    if (!s) { slot_clear(&g_slots[0]); s = &g_slots[0]; }
    // A re-authentication must not keep a stale decrypted table around; the
    // records are reloaded under the new key on the next use.
    for (int i = 0; i < TOTP_MAX_RECS; i++) totp_wipe_rec_rs(&s->recs[i]);
    s->count = 0;
    s->loaded = 0;
    s->in_use = 1;
    s->uid = uid;
    s->iters = PBKDF2_ITERATIONS;
    memcpy(s->key, key, SECSTORE_KEY_LEN);
    memcpy(s->salt, salt, SECSTORE_SALT_LEN);
    spinlock_release_irqrestore(&g_slot_lock, f);
}

// ---------------------------------------------------------------------------
// seckey_on_authenticate() - THE ONE PLACE THE KEY IS BORN.
// ---------------------------------------------------------------------------
void seckey_on_authenticate(const char *username, const char *password) {
    if (!username || !password || !password[0]) return;
    seckey_init_once();
    if (!g_fat_fs.mounted) return;

    user_entry_t *u = user_by_name(username);
    if (!u) return;   // authenticated against a shadow row with no passwd row
    uint32_t uid = u->uid;
    const char *home = u->home;

    // Skip the whole derivation for a user with nothing to open. This is the
    // reason an ordinary login does not get slower.
    if (!user_has_store(uid, home)) return;

    uint8_t salt[SECSTORE_SALT_LEN];
    if (salt_load_or_create(uid, salt) != 0) return;

    uint8_t key[SECSTORE_KEY_LEN];
    uint64_t t0 = sched_now_ms();
    pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                       salt, SECSTORE_SALT_LEN, PBKDF2_ITERATIONS, key);
    uint64_t dt = sched_now_ms() - t0;

    sec_enter();
    slot_install_locked(uid, key, salt);
    sec_leave();

    crypto_zero(key, sizeof(key));
    crypto_zero(salt, sizeof(salt));
    kprintf("[SECRET] secret-store key derived for uid=%u (%u iters, %lu ms); "
            "kernel memory only, no file\n",
            (unsigned)uid, (unsigned)PBKDF2_ITERATIONS, (unsigned long)dt);
}

// ---------------------------------------------------------------------------
// Load and save the container.
// ---------------------------------------------------------------------------
#define LEGACY_OBF_KEY 0x5A

// One-time migration of the legacy /APPS/MFA store. The app wrote
// "MFADB1\n<rows>" XORed with a single fixed byte, and the sealed plaintext format
// is BYTE-IDENTICAL to those de-obfuscated bytes on purpose, so migration is a
// XOR and a seal with no reshaping step that could lose a field.
//
// The legacy file is deleted only AFTER the sealed container is on disk. If the
// order were reversed, a failed write would destroy the user's only copy.
//
// THREE-VALUED ON PURPOSE:
//    1  migrated; s->recs/s->count are loaded and the container is on disk
//    0  nothing to migrate. Either there is no legacy file (the normal case) or
//       there is one and it does not parse, which was LOGGED. Both mean "carry on
//       with an empty store".
//   <0  a real failure that must reach the caller: the sealed container could not
//       be written, so proceeding would look like success.
//
// The 0-for-a-corrupt-file case is deliberate and was a BUG FIX. It used to return
// SECRET_E_FORMAT, store_load() propagated that, and every operation then failed
// FOREVER: a user with a damaged legacy file could not read their store and could
// not enrol a new account either. The data in a corrupt file is unrecoverable
// whatever we return, so blocking the feature permanently buys nothing. The file is
// LEFT IN PLACE for a human, and the refusal is on serial and in /BOOTLOG.TXT,
// which is the only channel that reaches the iMac target at all.
static int migrate_legacy(seckey_slot_t *s, const char *home);
static int store_save(seckey_slot_t *s);

static int store_load(seckey_slot_t *s, const char *home) {
    if (s->loaded) return 0;

    char path[48];
    secret_path(s->uid, "SEC", path, sizeof(path));
    if (!path[0]) return SECRET_E_ARG;

    uint32_t sz = 0;
    uint8_t *buf = (uint8_t *)fat_read_file(&g_fat_fs, path, &sz);
    if (!buf) {
        // No container. Either a legacy file to migrate, or a genuinely empty
        // store. Both are normal; neither is an error.
        int m = migrate_legacy(s, home);
        if (m < 0) return m;        // could not WRITE the sealed store: propagate
        if (m == 1) return 0;       // migrated; the slot is loaded
        s->count = 0;               // nothing to migrate: a valid empty store
        s->loaded = 1;
        return 0;
    }

    uint8_t *pt = (uint8_t *)kmalloc(SECSTORE_MAX_PLAINTEXT);
    if (!pt) { crypto_zero(buf, sz); kfree(buf); return SECRET_E_IO; }

    int pn = secstore_open_rs(s->key, buf, sz, pt, SECSTORE_MAX_PLAINTEXT);
    crypto_zero(buf, sz);
    kfree(buf);
    if (pn < 0) {
        crypto_zero(pt, SECSTORE_MAX_PLAINTEXT);
        kfree(pt);
        // -5 is SECSTORE_E_AUTH: the tag did not verify. Wrong key (the user
        // changed their password without re-keying the store) or altered bytes,
        // and by design the two cannot be told apart. Say so rather than
        // reporting "no accounts", which is the failure this whole change is
        // about: the old app could not tell an empty store from a failed one.
        kprintf("[SECRET] uid=%u: store did not open (rc=%d). This is NOT "
                "'no accounts': the container is present and refused.\n",
                (unsigned)s->uid, pn);
        bootlog_write("[SECRET] uid=%u store open failed rc=%d", (unsigned)s->uid, pn);
        return (pn == -5) ? SECRET_E_AUTH : SECRET_E_FORMAT;
    }

    int rn = totp_parse_store_rs(pt, (size_t)pn, s->recs, TOTP_MAX_RECS);
    crypto_zero(pt, SECSTORE_MAX_PLAINTEXT);
    kfree(pt);
    if (rn < 0) {
        // The bytes passed the AEAD, so they were written by THIS kernel under
        // THIS user's key. A malformed row is therefore our own bug, not
        // tampering, and it is loud.
        kprintf("[SECRET] uid=%u: decrypted store failed to parse (rc=%d). The "
                "AEAD passed, so this is a KERNEL BUG, not tampering.\n",
                (unsigned)s->uid, rn);
        bootlog_write("[SECRET] uid=%u store parse failed rc=%d", (unsigned)s->uid, rn);
        s->count = 0;
        return SECRET_E_FORMAT;
    }
    s->count = rn;
    s->loaded = 1;
    return 0;
}

static int migrate_legacy(seckey_slot_t *s, const char *home) {
    if (!home || !home[0]) return 0;
    char legacy[256];
    if (userconf_kpath_home(home, "MFA.DB", legacy, sizeof(legacy)) != 0) return 0;

    uint32_t sz = 0;
    uint8_t *buf = (uint8_t *)fat_read_file(&g_fat_fs, legacy, &sz);
    if (!buf) return 0;             // nothing to migrate: the normal case
    if (sz == 0 || sz > SECSTORE_MAX_PLAINTEXT) {
        crypto_zero(buf, sz ? sz : 1);
        kfree(buf);
        kprintf("[SECRET] uid=%u: legacy MFA.DB is %u bytes, which cannot be a "
                "store; LEFT IN PLACE and nothing was migrated\n",
                (unsigned)s->uid, (unsigned)sz);
        bootlog_write("[SECRET] uid=%u legacy MFA.DB implausible size %u",
                      (unsigned)s->uid, (unsigned)sz);
        return 0;
    }
    for (uint32_t i = 0; i < sz; i++) buf[i] ^= LEGACY_OBF_KEY;

    int rn = totp_parse_store_rs(buf, sz, s->recs, TOTP_MAX_RECS);
    crypto_zero(buf, sz);
    kfree(buf);
    if (rn < 0) {
        kprintf("[SECRET] uid=%u: legacy MFA.DB did not parse (rc=%d); it is LEFT "
                "IN PLACE, nothing was migrated, and the store starts EMPTY so the "
                "user can still enrol\n", (unsigned)s->uid, rn);
        bootlog_write("[SECRET] uid=%u legacy MFA.DB parse failed rc=%d; left in place",
                      (unsigned)s->uid, rn);
        return 0;
    }
    s->count = rn;
    s->loaded = 1;
    int sv = store_save(s);
    if (sv != 0) {
        // The sealed copy is not on disk, so the legacy file MUST survive.
        kprintf("[SECRET] uid=%u: migration could not write the sealed store "
                "(rc=%d); legacy MFA.DB KEPT\n", (unsigned)s->uid, sv);
        s->loaded = 0;
        s->count = 0;
        return sv < 0 ? sv : SECRET_E_IO;
    }
    if (fat_delete(&g_fat_fs, legacy) != 0) {
        kprintf("[SECRET] uid=%u: migrated %d account(s), but could NOT delete %s. "
                "The obfuscated copy is still readable by Ring 3.\n",
                (unsigned)s->uid, s->count, legacy);
        bootlog_write("[SECRET] uid=%u legacy MFA.DB survived migration", (unsigned)s->uid);
    } else {
        kprintf("[SECRET] uid=%u: migrated %d TOTP account(s) from the XOR MFA.DB "
                "into the encrypted store; legacy file deleted\n",
                (unsigned)s->uid, s->count);
        bootlog_write("[SECRET] uid=%u migrated %d accounts to the encrypted store",
                      (unsigned)s->uid, s->count);
    }
    return 1;
}

static int store_save(seckey_slot_t *s) {
    char path[48];
    secret_path(s->uid, "SEC", path, sizeof(path));
    if (!path[0]) return SECRET_E_ARG;

    uint8_t *pt = (uint8_t *)kmalloc(SECSTORE_MAX_PLAINTEXT);
    if (!pt) return SECRET_E_IO;
    int pn = totp_format_store_rs(s->recs, (size_t)s->count, pt, SECSTORE_MAX_PLAINTEXT);
    if (pn < 0) {
        crypto_zero(pt, SECSTORE_MAX_PLAINTEXT);
        kfree(pt);
        return SECRET_E_FORMAT;
    }

    uint32_t cap = SECSTORE_HDR_LEN + (uint32_t)pn + SECSTORE_TAG_LEN;
    uint8_t *ct = (uint8_t *)kmalloc(cap);
    if (!ct) { crypto_zero(pt, SECSTORE_MAX_PLAINTEXT); kfree(pt); return SECRET_E_IO; }

    // A FRESH nonce for every write. Reusing one under the same key breaks
    // ChaCha20-Poly1305 completely (it is not a nonce-misuse-resistant mode), so
    // this is generated here rather than stored anywhere.
    uint8_t nonce[SECSTORE_NONCE_LEN];
    csprng_bytes(nonce, sizeof(nonce));

    int cn = secstore_seal_rs(s->key, s->salt, nonce, s->iters,
                             pt, (size_t)pn, ct, cap);
    crypto_zero(pt, SECSTORE_MAX_PLAINTEXT);
    kfree(pt);
    crypto_zero(nonce, sizeof(nonce));
    if (cn < 0) { crypto_zero(ct, cap); kfree(ct); return SECRET_E_FORMAT; }

    secrets_dir_ensure();
    int rc = write_root_0600(path, ct, (uint32_t)cn);
    crypto_zero(ct, cap);
    kfree(ct);
    return rc == 0 ? 0 : SECRET_E_IO;
}

// ---------------------------------------------------------------------------
// seckey_rekey() - the store survives a password change.
//
// See the block comment in secretuse.h for why this exists and why it is called
// AFTER the new password is set rather than before.
//
// The salt does NOT change. It is public, per-user, and its only job is to make
// this derivation unrelated to the SHADOW one; rotating it would buy nothing and
// would mean a failure mid-way left the salt and the container disagreeing.
//
// A BETTER SHAPE EXISTS AND IS NOT BUILT HERE. The standard design wraps a random
// per-user data key under the password-derived key, so a password change re-wraps
// 32 bytes instead of re-sealing the whole store, and so no container is ever
// rewritten for a reason unrelated to its contents. That is the right destination;
// this is the small change that stops the data loss today, using code that is
// already proven by crypto/secretuse_kat.sh. Recorded in docs/SECRET_STORE.md.
// ---------------------------------------------------------------------------
int seckey_rekey(const char *username, const char *old_password,
                 const char *new_password) {
    if (!username || !old_password || !new_password || !new_password[0]) return -1;
    seckey_init_once();
    if (!g_fat_fs.mounted) return 0;

    user_entry_t *u = user_by_name(username);
    if (!u) return 0;
    uint32_t uid = u->uid;

    char path[48];
    secret_path(uid, "SEC", path, sizeof(path));
    if (!path[0]) return -1;

    // Nothing sealed yet: no re-key needed, and the next authenticate will derive
    // under the new password. A legacy MFA.DB also needs nothing, because it is
    // not encrypted and will migrate under whatever key is current when it is read.
    uint32_t probe = 0;
    void *pb = fat_read_file(&g_fat_fs, path, &probe);
    if (!pb) return 0;
    kfree(pb);

    uint8_t salt[SECSTORE_SALT_LEN];
    if (salt_load_or_create(uid, salt) != 0) return -1;

    uint8_t oldk[SECSTORE_KEY_LEN], newk[SECSTORE_KEY_LEN];
    pbkdf2_hmac_sha256((const uint8_t *)old_password, strlen(old_password),
                       salt, SECSTORE_SALT_LEN, PBKDF2_ITERATIONS, oldk);
    pbkdf2_hmac_sha256((const uint8_t *)new_password, strlen(new_password),
                       salt, SECSTORE_SALT_LEN, PBKDF2_ITERATIONS, newk);

    int rc = -1;
    sec_enter();
    {
        // Install the OLD key, load the store under it, then install the NEW key
        // and save. Going through the normal load/save path rather than a private
        // copy means the re-key is covered by exactly the same parser, envelope
        // and error handling as every other access.
        slot_install_locked(uid, oldk, salt);
        seckey_slot_t *s = slot_find(uid);
        if (s) {
            int lr = store_load(s, u->home);
            if (lr == 0) {
                memcpy(s->key, newk, SECSTORE_KEY_LEN);
                int sv = store_save(s);
                if (sv == 0) {
                    rc = 1;
                } else {
                    // The container is still under the OLD key. Put the old key
                    // back so the session that is running now keeps working, and
                    // report: the user's next login will not be able to open it.
                    memcpy(s->key, oldk, SECSTORE_KEY_LEN);
                    kprintf("[SECRET] uid=%u: RE-KEY FAILED to write (rc=%d). The "
                            "store is still encrypted under the OLD password.\n",
                            (unsigned)uid, sv);
                    rc = -1;
                }
            } else {
                kprintf("[SECRET] uid=%u: RE-KEY could not open the store with the "
                        "old password (rc=%d); nothing was changed\n",
                        (unsigned)uid, lr);
                rc = -1;
            }
        }
    }
    sec_leave();

    crypto_zero(oldk, sizeof(oldk));
    crypto_zero(newk, sizeof(newk));
    crypto_zero(salt, sizeof(salt));
    if (rc == 1) {
        kprintf("[SECRET] uid=%u: secret store re-keyed to the new password\n",
                (unsigned)uid);
        bootlog_write("[SECRET] uid=%u store re-keyed on password change", (unsigned)uid);
    } else {
        bootlog_write("[SECRET] uid=%u store RE-KEY FAILED on password change",
                      (unsigned)uid);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// The syscall.
// ---------------------------------------------------------------------------
int64_t sys_secret_use(int op, uint32_t idx, void *out, uint32_t outcap,
                       const void *in, uint32_t inlen) {
    seckey_init_once();
    if (!g_fat_fs.mounted) return SECRET_E_IO;

    process_t *p = proc_current();
    if (!p) return SECRET_E_ARG;
    const uint32_t uid = p->uid;

    // Tier check FIRST, so an undeclared or unimplemented key source is refused
    // before anything touches a file. rustkern/totp.rs owns the mapping and its
    // self-test proves an undeclared class gets SECRET_KEYSRC_NONE.
    uint32_t src = secret_keysrc_for_class_rs(SECRET_CLASS_TOTP);
    if (src == SECRET_KEYSRC_NONE) return SECRET_E_ARG;
    if (!secret_keysrc_is_implemented_rs(src)) return SECRET_E_NOTIMPL;

    // NO "/" FALLBACK FOR AN UNKNOWN HOME, and this is a real fix rather than
    // tidying. `home` is used only to find a legacy MFA.DB to migrate, and
    // defaulting it to "/" means /CONFIG/MFA.DB, which is ROOT'S legacy TOTP
    // enrolment. Migrating that into another uid's store would copy root's second
    // factors to that user: precisely the cross-user read the deleted app comment
    // warned about ("a fallback read would let a non-root session pick up ROOT's
    // enrolment").
    //
    // It is currently UNREACHABLE: migration only runs when no container exists,
    // which means no key was derived, which means slot_find() already returned
    // LOCKED above. So this is not a live bug. It is removed anyway, because the
    // reachability argument depends on user_has_store()'s internals holding still,
    // and "safe because of how a different function happens to be written" is the
    // shape of assumption #698 deleted from the cron reader. An empty home now
    // means NO LEGACY PATH, not root's.
    user_entry_t *u = user_lookup_uid(uid);
    const char *home = (u && u->home[0]) ? u->home : NULL;

    sec_enter();
    int64_t rv;

    seckey_slot_t *s = slot_find(uid);
    if (!s) { rv = SECRET_E_LOCKED; goto done; }

    {
        int lr = store_load(s, home);
        if (lr != 0) { rv = lr; goto done; }
    }

    switch (op) {
    case SECRET_OP_STATUS:
        rv = s->count;
        break;

    case SECRET_OP_LIST: {
        // Names and geometry ONLY. There is no field in secret_totp_info_t that
        // could carry a secret, which is a property of the struct rather than of
        // this function remembering not to fill one in.
        // ONE copy_to_user FOR THE WHOLE ARRAY, staged through a kernel buffer.
        //
        // Two reasons, and the second one is why it is worth the kmalloc.
        //
        // 1. CORRECTNESS. A per-element copy that faults on element 7 leaves the
        //    caller holding six valid records and a return value it cannot trust,
        //    with no way to know where the array stopped being real. One copy
        //    either lands entirely or lands not at all.
        //
        // 2. It is the only form tools/smap-uaccess-lint can read correctly, and
        //    working that out cost three attempts worth recording. Rule B3 taints
        //    "the last identifier in the destination argument text" and then flags
        //    any memset/memcpy/deref naming it. `out + i * sizeof(info)` taints
        //    `info`, so the memset of a kernel stack local was flagged; hoisting a
        //    `stride` taints `stride`, so the memset was flagged again; naming a
        //    `uint8_t *udst` taints `udst`, and then its own DECLARATION matches
        //    the `\*\s*name` deref pattern and is flagged. Every one of those is a
        //    false positive, and the honest fix is not to silence the lint but to
        //    write the destination as the plain parameter `out`, which is exactly
        //    what a user pointer should look like at a uaccess call. The lint then
        //    taints the thing that really is user memory and finds nothing else.
        const uint32_t stride = (uint32_t)sizeof(secret_totp_info_t);
        uint32_t want = (uint32_t)s->count;
        if (outcap / stride < want) { rv = SECRET_E_NOSPACE; break; }
        if (!out) { rv = SECRET_E_ARG; break; }
        if (want == 0) { rv = 0; break; }
        secret_totp_info_t *tmp = (secret_totp_info_t *)kmalloc(want * stride);
        if (!tmp) { rv = SECRET_E_IO; break; }
        memset(tmp, 0, want * stride);
        for (uint32_t i = 0; i < want; i++) {
            memcpy(tmp[i].issuer, s->recs[i].issuer, SECRET_NAME_MAX);
            memcpy(tmp[i].label,  s->recs[i].label,  SECRET_NAME_MAX);
            tmp[i].issuer[SECRET_NAME_MAX - 1] = '\0';
            tmp[i].label[SECRET_NAME_MAX - 1] = '\0';
            tmp[i].digits = s->recs[i].digits;
            tmp[i].period = s->recs[i].period;
            tmp[i].alg    = s->recs[i].alg;
        }
        int crc = copy_to_user(out, tmp, want * stride);
        kfree(tmp);
        rv = (crc != 0) ? SECRET_E_ARG : (int64_t)want;
        break;
    }

    case SECRET_OP_CODES: {
        // THE COUNTER COMES FROM HERE AND NOWHERE ELSE. `idx`, `in` and `inlen`
        // are unused for this op precisely so there is no argument a caller could
        // use to choose a time. See secretuse.h and backlog A4.
        int64_t now = realtime_sec_rs();
        if (now <= 0) { rv = SECRET_E_NOCLOCK; break; }
        // Staged and copied once, same two reasons as SECRET_OP_LIST above. The
        // staging buffer is wiped before it is freed: a live TOTP code is a bearer
        // token for this period, and leaving one in a freed kernel allocation for
        // the next kmalloc to inherit is a small leak with a real consequence.
        const uint32_t stride = (uint32_t)sizeof(secret_totp_code_t);
        uint32_t want = (uint32_t)s->count;
        if (outcap / stride < want) { rv = SECRET_E_NOSPACE; break; }
        if (!out) { rv = SECRET_E_ARG; break; }
        if (want == 0) { rv = 0; break; }
        secret_totp_code_t *tmp = (secret_totp_code_t *)kmalloc(want * stride);
        if (!tmp) { rv = SECRET_E_IO; break; }
        memset(tmp, 0, want * stride);
        for (uint32_t i = 0; i < want; i++) {
            int rem = totp_code_rs(&s->recs[i], now, (uint8_t *)tmp[i].code,
                                   SECRET_CODE_MAX);
            tmp[i].period = s->recs[i].period;
            if (rem < 0) {
                // A single bad record does not fail the whole call; it reports as
                // an empty code so the app can show one broken row rather than
                // hiding every account behind it.
                tmp[i].code[0] = '\0';
                tmp[i].remaining = 0;
            } else {
                tmp[i].remaining = (uint16_t)rem;
            }
        }
        int crc2 = copy_to_user(out, tmp, want * stride);
        crypto_zero(tmp, want * stride);
        kfree(tmp);
        rv = (crc2 != 0) ? SECRET_E_ARG : (int64_t)want;
        break;
    }

    case SECRET_OP_ADD: {
        if (!in || inlen != sizeof(secret_totp_add_t)) { rv = SECRET_E_ARG; break; }
        if (s->count >= TOTP_MAX_RECS) { rv = SECRET_E_FULL; break; }
        secret_totp_add_t a;
        if (copy_from_user(&a, in, sizeof(a)) != 0) { rv = SECRET_E_ARG; break; }
        // Terminate every string the caller handed us BEFORE anything reads it.
        // The Rust side refuses an unterminated buffer, and this makes sure it
        // never has to: a Ring-3 struct arrives as bytes, not as promises.
        a.issuer[SECRET_NAME_MAX - 1] = '\0';
        a.label[SECRET_NAME_MAX - 1] = '\0';
        a.b32[SECRET_B32_MAX - 1] = '\0';
        totp_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        int mr = totp_make_rec_rs(&rec, (const uint8_t *)a.issuer, (const uint8_t *)a.label,
                                  (const uint8_t *)a.b32, a.digits, a.period, a.alg);
        crypto_zero(&a, sizeof(a));
        if (mr != 0) { crypto_zero(&rec, sizeof(rec)); rv = SECRET_E_FORMAT; break; }
        s->recs[s->count] = rec;
        s->count++;
        crypto_zero(&rec, sizeof(rec));
        int sv = store_save(s);
        if (sv != 0) {
            // Roll the in-memory table back so it cannot disagree with the disk.
            s->count--;
            totp_wipe_rec_rs(&s->recs[s->count]);
            rv = sv;
            break;
        }
        kprintf("[SECRET] uid=%u enrolled a TOTP account (%d total); the seed is "
                "sealed and will not be returned to Ring 3\n",
                (unsigned)uid, s->count);
        rv = 0;
        break;
    }

    case SECRET_OP_DEL: {
        if (idx >= (uint32_t)s->count) { rv = SECRET_E_ARG; break; }
        totp_rec_t saved = s->recs[idx];
        for (int i = (int)idx; i < s->count - 1; i++) s->recs[i] = s->recs[i + 1];
        totp_wipe_rec_rs(&s->recs[s->count - 1]);
        s->count--;
        int sv = store_save(s);
        if (sv != 0) {
            // Put it back: the table must match the disk, and a delete that did
            // not persist must not look like one that did.
            for (int i = s->count; i > (int)idx; i--) s->recs[i] = s->recs[i - 1];
            s->recs[idx] = saved;
            s->count++;
            crypto_zero(&saved, sizeof(saved));
            rv = sv;
            break;
        }
        crypto_zero(&saved, sizeof(saved));
        rv = 0;
        break;
    }

    default:
        rv = SECRET_E_ARG;
        break;
    }

done:
    sec_leave();
    return rv;
}

// ---------------------------------------------------------------------------
// Boot self-test. Runs rustkern/totp.rs's vectors and refusals, then checks the
// keyring's own invariants, which the Rust cannot see.
// ---------------------------------------------------------------------------
int secretuse_selftest(uint32_t *out_checks) {
    uint32_t rchecks = 0;
    int rc = totp_selftest_rs(&rchecks);
    if (rc != 0) { if (out_checks) *out_checks = rchecks; return rc; }
    // A self-test that ran nothing and returned 0 is vacuous. Refuse it.
    if (rchecks == 0) { if (out_checks) *out_checks = 0; return 1000; }

    uint32_t n = rchecks;
    seckey_init_once();

    // 1001: a uid with no derived key has NO slot, so SYS_SECRET_USE reports
    // LOCKED rather than opening anything. Uses a uid that cannot exist.
    n++;
    if (slot_find(0xFFFFFFFEu) != NULL) { if (out_checks) *out_checks = n; return 1001; }

    // 1002: the sleepable mutex is exclusive. A second claim while held must
    // FAIL, or two callers could be inside the read-modify-write at once.
    n++;
    if (!sec_try_claim()) { if (out_checks) *out_checks = n; return 1002; }
    n++;
    if (sec_try_claim()) { sec_leave(); if (out_checks) *out_checks = n; return 1002; }
    sec_leave();
    // 1003: and it is released, so the next caller can take it. A mutex that
    // leaks its held state would wedge every later call, which is worse than one
    // that never locked.
    n++;
    if (!sec_try_claim()) { if (out_checks) *out_checks = n; return 1003; }
    sec_leave();

    // 1004: slot_clear() really clears. A slot recycled between users must not
    // carry the previous user's key.
    n++;
    {
        seckey_slot_t *t = &g_slots[SECKEY_SLOTS - 1];
        uint64_t f = spinlock_acquire_irqsave(&g_slot_lock);
        int was_in_use = t->in_use;
        if (!was_in_use) {
            memset(t->key, 0xA5, sizeof(t->key));
            t->count = 7;
            t->loaded = 1;
            t->in_use = 1;
            t->uid = 0xFFFFFFFDu;
            slot_clear(t);
        }
        uint8_t any = 0;
        for (unsigned i = 0; i < sizeof(t->key); i++) any |= t->key[i];
        int bad = !was_in_use && (any != 0 || t->count != 0 || t->loaded != 0 || t->in_use != 0);
        spinlock_release_irqrestore(&g_slot_lock, f);
        if (bad) { if (out_checks) *out_checks = n; return 1004; }
    }

    // ======================================================================
    // 1100+: THE COMPOSITION TEST. Everything above tests a PART.
    //
    // WHY THIS IS SEPARATE AND WHY IT MATTERS MOST. crypto/secretuse_kat.sh proves
    // the arithmetic and crypto/secstore_kat.sh proves the envelope, and BOTH pass
    // with a store whose "key" is a constant, because neither knows a password
    // exists. This case wires the REAL crypto/pbkdf2.c to the REAL secstore.rs
    // envelope to the REAL rustkern/totp.rs parser and asks the three questions the
    // unit tests cannot:
    //
    //   does the RIGHT password open it, does the WRONG password FAIL,
    //   and is a TAMPERED container REFUSED
    //
    // The first without the second two is not evidence of encryption.
    //
    // ENTIRELY IN MEMORY, and at 1000 KDF iterations rather than the shipping
    // 50000. This runs on EVERY boot of EVERY machine, so it must not write files
    // and must not cost a tenth of a second. The iteration count is not what is
    // under test here (the KAT's RFC vectors cover the KDF's arithmetic at its real
    // cost); what is under test is that the three layers COMPOSE. Using the
    // shipping 50000 would add ~250ms to every boot on the iMac to re-prove
    // something already proven elsewhere.
    // ======================================================================
    {
        static const char PW_RIGHT[] = "correct-horse-battery-staple";
        static const char PW_WRONG[] = "correct-horse-battery-stapld";  // one byte
        const uint32_t E2E_ITERS = 1000;
        uint8_t salt[SECSTORE_SALT_LEN], nonce[SECSTORE_NONCE_LEN];
        for (int i = 0; i < SECSTORE_SALT_LEN; i++) salt[i] = (uint8_t)(0x40 + i);
        for (int i = 0; i < SECSTORE_NONCE_LEN; i++) nonce[i] = (uint8_t)(0x90 + i);

        uint8_t kr[SECSTORE_KEY_LEN], kw[SECSTORE_KEY_LEN];
        pbkdf2_hmac_sha256((const uint8_t *)PW_RIGHT, sizeof(PW_RIGHT) - 1,
                           salt, SECSTORE_SALT_LEN, E2E_ITERS, kr);
        pbkdf2_hmac_sha256((const uint8_t *)PW_WRONG, sizeof(PW_WRONG) - 1,
                           salt, SECSTORE_SALT_LEN, E2E_ITERS, kw);

        // 1100: two passwords differing in ONE BYTE give unrelated keys. If this
        // failed, every case below would pass for the wrong reason.
        n++;
        if (crypto_memcmp(kr, kw, SECSTORE_KEY_LEN) == 0) {
            if (out_checks) *out_checks = n;
            return 1100;
        }

        totp_rec_t *rec = (totp_rec_t *)kmalloc(sizeof(totp_rec_t));
        uint8_t *pt = (uint8_t *)kmalloc(512);
        uint8_t *ct = (uint8_t *)kmalloc(1024);
        uint8_t *back = (uint8_t *)kmalloc(512);
        if (!rec || !pt || !ct || !back) {
            if (rec) kfree(rec);
            if (pt) kfree(pt);
            if (ct) kfree(ct);
            if (back) kfree(back);
            // Out of memory is NOT a pass. A self-test that silently skips itself
            // when a kmalloc fails is the "skipped test reported as green" shape.
            n++;
            if (out_checks) *out_checks = n;
            return 1101;
        }
        int e2e = 0;   // the failing case number, or 0

        memset(rec, 0, sizeof(*rec));
        if (totp_make_rec_rs(rec, (const uint8_t *)"E2E",
                             (const uint8_t *)"compose",
                             (const uint8_t *)"GEZDGNBVGY3TQOJQ", 6, 30, 1) != 0)
            e2e = 1102;

        int pn = 0;
        if (!e2e) {
            pn = totp_format_store_rs(rec, 1, pt, 512);
            if (pn <= 0) e2e = 1103;
        }
        int cn = 0;
        if (!e2e) {
            cn = secstore_seal_rs(kr, salt, nonce, E2E_ITERS, pt, (size_t)pn, ct, 1024);
            if (cn != SECSTORE_HDR_LEN + pn + SECSTORE_TAG_LEN) e2e = 1104;
        }

        // 1105: the RIGHT key opens it and the plaintext is byte-identical.
        if (!e2e) {
            int rn = secstore_open_rs(kr, ct, (size_t)cn, back, 512);
            if (rn != pn || memcmp(back, pt, (uint32_t)pn) != 0) e2e = 1105;
        }
        // 1106: and the recovered plaintext parses back to the SAME record, which
        // is what makes 1105 a statement about the data and not just the length.
        if (!e2e) {
            totp_rec_t *rt = (totp_rec_t *)kmalloc(sizeof(totp_rec_t));
            if (!rt) {
                e2e = 1106;
            } else {
                memset(rt, 0, sizeof(*rt));
                int k = totp_parse_store_rs(back, (size_t)pn, rt, 1);
                if (k != 1 || rt->secret_len != rec->secret_len ||
                    memcmp(rt->secret, rec->secret, rec->secret_len) != 0 ||
                    rt->digits != rec->digits || rt->period != rec->period ||
                    rt->alg != rec->alg)
                    e2e = 1106;
                totp_wipe_rec_rs(rt);
                kfree(rt);
            }
        }
        // 1107: THE WRONG PASSWORD FAILS. One byte different in the password, and
        // the container refuses. This is the case that makes the word "encrypted"
        // mean something here.
        //
        // `make SECRETE2ETEST=1` hands case 1107 the RIGHT key where the wrong one
        // belongs, so the boot line can be SEEN to say FAIL on an otherwise healthy
        // machine. Same argument as NOBLOCKTEST and RTCLKTESTFAIL: a self-test
        // nobody has watched go red is indistinguishable from one that is not wired
        // up (#514/#665). The kernel build NEVER defines this by default; grep the
        // Makefile.
        if (!e2e) {
#ifdef SECRET_E2E_TESTFAIL
            if (secstore_open_rs(kr, ct, (size_t)cn, back, 512) != -5) e2e = 1107;
#else
            if (secstore_open_rs(kw, ct, (size_t)cn, back, 512) != -5) e2e = 1107;
#endif
        }
        // 1108: and it hands back NO plaintext when it refuses. A caller that
        // ignores the return value must not be able to read unverified bytes.
        if (!e2e) {
            uint8_t any = 0;
            for (int i = 0; i < pn; i++) any |= back[i];
            if (any != 0) e2e = 1108;
        }
        // 1109: A TAMPERED CIPHERTEXT IS REFUSED. One flipped bit in the body.
        if (!e2e) {
            ct[SECSTORE_HDR_LEN] ^= 0x01;
            if (secstore_open_rs(kr, ct, (size_t)cn, back, 512) != -5) e2e = 1109;
            ct[SECSTORE_HDR_LEN] ^= 0x01;
        }
        // 1110: A TAMPERED TAG IS REFUSED.
        if (!e2e) {
            ct[cn - 1] ^= 0x80;
            if (secstore_open_rs(kr, ct, (size_t)cn, back, 512) != -5) e2e = 1110;
            ct[cn - 1] ^= 0x80;
        }
        // 1111: A REWRITTEN KDF ITERATION COUNT IS REFUSED. This is the whole
        // reason the header is the AAD: without it an attacker rewrites iters to 1
        // and the file still opens, so its KDF cost is theirs to choose. Offset 12
        // is kdf_iters (secstore.rs's layout table).
        if (!e2e) {
            uint8_t save = ct[12];
            ct[12] = 1;
            if (secstore_open_rs(kr, ct, (size_t)cn, back, 512) != -5) e2e = 1111;
            ct[12] = save;
        }
        // 1112: and after all that tampering, the UNMODIFIED container still opens.
        // Without this the four cases above would also pass if open_rs simply
        // always failed, which is the "a verifier that refuses everything answers
        // no" twin of the vacuous verifier.
        if (!e2e) {
            if (secstore_open_rs(kr, ct, (size_t)cn, back, 512) != pn) e2e = 1112;
        }

        n += 13;   // 1100 was counted above; 1102..1112 plus the restore check
        crypto_zero(kr, sizeof(kr));
        crypto_zero(kw, sizeof(kw));
        crypto_zero(pt, 512);
        crypto_zero(back, 512);
        crypto_zero(ct, 1024);
        totp_wipe_rec_rs(rec);
        kfree(rec); kfree(pt); kfree(ct); kfree(back);
        if (e2e) { if (out_checks) *out_checks = n; return e2e; }
    }

    if (out_checks) *out_checks = n;
    return 0;
}
