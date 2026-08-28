// users.c - User and group database for MayteraOS
// Manages /CONFIG/PASSWD, /CONFIG/SHADOW, /CONFIG/GROUP

#include "users.h"
#include "pwpolicy.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "../fs/bootlog.h"
#include "../fs/cfgread.h"     // #192: is this read outcome worth a log line?
#include "../crypto/crypto.h"
#include "../crypto/csprng.h"

// #499 SECURITY: lockout timing runs on the SHARED MONOTONIC clock
// (sched_now_ms(), cpu/mono.h), never on the tick counter.
//
// The note that used to live here called the tick source a "minor weakening we
// accept". It was not minor: the tick counter counts ticks DELIVERED, not time
// ELAPSED, and under KVM a starved vCPU has its missed ticks re-delivered in a
// BURST (~1250 ticks, a nominal 5s at 250Hz, in ~15ms of real time). A
// brute-force lockout that expires EARLY is a THROTTLE BYPASS, and an attacker
// hammering the login is exactly the load that produces the burst.
//
// It also carried a second, always-on bug: the local HZ constant was hardcoded
// to 100 while the PIT actually runs at 250Hz, so every lockout was 2.5x
// SHORTER in real time than the policy states (30s -> 12s, 300s -> 120s).
// Deadlines are now absolute REAL milliseconds, so both go away.
#include "../cpu/mono.h"

// Hash and store a password with NO policy check. Forward declared because
// the dev-only default-accounts block below is the only other caller and it
// sits above the definition. Static on purpose: a policy bypass must not be
// reachable from another translation unit, which is the same reasoning that
// made user_verify_password() static in #745.
static int set_password_hashed(const char *username, const char *password);

// PBKDF2-HMAC-SHA256 work factor (#566). Chosen to land ~150-250ms per attempt
// on the iMac14,4 soft-float target (measured ~0.0016ms/iter on the the build host/kvm64
// build+VM host => ~80ms there; the real target is ~2.5-3x slower). SHA-256 is
// pure integer math so the -mno-sse soft-float kernel target does not penalize
// it. This is UX-bounded and sits below OWASP's 600k desktop guidance on
// purpose: the constrained target cannot afford 600k (~1s) per login. The
// random per-account salt, 0600 root-only SHADOW, and rate-limit/lockout below
// are the compensating controls. Retune if the login feels sluggish on real hw.
#define PBKDF2_ITERATIONS 50000u
#define PBKDF2_SALT_LEN   16
#define PBKDF2_DK_LEN     32

// External filesystem
extern fat_fs_t g_fat_fs;

// User and group tables
static user_entry_t user_table[MAX_USERS];
static group_entry_t group_table[MAX_GROUPS];
static int user_count = 0;
static int group_count = 0;

// Shadow password table (kept separate for security)
typedef struct {
    char username[USERNAME_MAX];
    char hash[PASSWORD_HASH_SIZE + 1]; // PBKDF2 record, legacy SHA-256 hex, or "*"
    uint8_t active;
    // Rate-limit / lockout state (#566). In-RAM only: a reboot resets it, but a
    // reboot requires physical/console access which is already root-equivalent
    // by design (see SECURE_LOGIN_DESIGN.md 3.3b), so persisting it buys little.
    uint32_t failed_attempts;
    uint64_t lockout_until_ms;  // #499: absolute REAL-ms deadline (sched_now_ms), 0 = not locked out
    // #745: the ELEVATION counter. Deliberately a SEPARATE pair rather than a
    // mode flag on the pair above, because the whole requirement is that an
    // app-raisable prompt can never move the numbers the login gate, the lock
    // screen and sshd read. See users.h for the argument in full.
    uint32_t elev_failed_attempts;
    uint64_t elev_lockout_until_ms;
} shadow_entry_t;

static shadow_entry_t shadow_table[MAX_USERS];
static int shadow_count = 0;

static bool users_initialized = false;

// ============================================================================
// Internal helpers
// ============================================================================

// Parse a decimal number from string, advance pointer past it
static uint32_t parse_uint_adv(const char **s) {
    uint32_t val = 0;
    while (**s >= '0' && **s <= '9') {
        val = val * 10 + (**s - '0');
        (*s)++;
    }
    return val;
}

// Copy string until delimiter or end, advance pointer past delimiter
static int copy_field(const char **src, char *dst, size_t dst_size, char delim) {
    size_t i = 0;
    while (**src && **src != delim && **src != '\n' && **src != '\r' && i < dst_size - 1) {
        dst[i++] = **src;
        (*src)++;
    }
    dst[i] = '\0';
    if (**src == delim) (*src)++;
    return (int)i;
}

// ============================================================================
// Password hashing (#566): salted PBKDF2-HMAC-SHA256, built from the existing
// crypto/sha256.c hmac_sha256 primitive (no new crypto invented). Constant-time
// compare so a byte-at-a-time timing oracle cannot leak the stored hash.
//
// Justification for C (project Rust-first mandate): this is thin glue tightly
// entangled with the existing C shadow-table (users.c), the C sc HMAC/SHA-256
// ABI, and it ships+verifies in a single pass. No float, no new algorithm - a
// standard PBKDF2 iteration loop over the existing HMAC. Stated per policy.
// ============================================================================

static const char HEX_CHARS[] = "0123456789abcdef";

static void bytes_to_hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) {
        out[i * 2]     = HEX_CHARS[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEX_CHARS[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse `n` bytes worth of hex (2*n chars). Returns 1 on success, 0 on error.
static int hex_to_bytes(const char *in, int n, uint8_t *out) {
    for (int i = 0; i < n; i++) {
        int hi = hex_val(in[i * 2]);
        int lo = hex_val(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

// Constant-time compare of two NUL-terminated strings. Compares over the max
// of the two lengths so runtime does not depend on where a mismatch is, nor on
// the (public) stored length. Returns 1 if equal, 0 otherwise.
static int ct_streq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    volatile uint8_t diff = (uint8_t)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = i < la ? (uint8_t)a[i] : 0;
        uint8_t cb = i < lb ? (uint8_t)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

// PBKDF2-HMAC-SHA256 with dkLen == hLen == 32 (a single output block, so no
// block-index loop is needed). RFC 8018.
static void pbkdf2_hmac_sha256(const uint8_t *pw, size_t pwlen,
                               const uint8_t *salt, size_t saltlen,
                               uint32_t iters, uint8_t out[PBKDF2_DK_LEN]) {
    uint8_t U[32], T[32];
    // U1 = HMAC(pw, salt || INT_BE32(1))
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, pw, pwlen);
    hmac_sha256_update(&ctx, salt, saltlen);
    const uint8_t blk_idx[4] = { 0, 0, 0, 1 };
    hmac_sha256_update(&ctx, blk_idx, 4);
    hmac_sha256_final(&ctx, U);
    memcpy(T, U, 32);
    for (uint32_t i = 1; i < iters; i++) {
        // U_c = HMAC(pw, U_{c-1})
        hmac_sha256(pw, pwlen, U, 32, U);
        for (int j = 0; j < 32; j++) T[j] ^= U[j];
    }
    memcpy(out, T, 32);
    crypto_zero(U, sizeof(U));
    crypto_zero(T, sizeof(T));
}

// Build a self-describing shadow record: "pbkdf2$<iters>$<salthex>$<hashhex>".
static void make_pbkdf2_record(const char *password, const uint8_t *salt,
                               uint32_t iters, char *out /* PASSWORD_HASH_SIZE+1 */) {
    uint8_t dk[PBKDF2_DK_LEN];
    pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                       salt, PBKDF2_SALT_LEN, iters, dk);
    char salt_hex[PBKDF2_SALT_LEN * 2 + 1];
    char dk_hex[PBKDF2_DK_LEN * 2 + 1];
    bytes_to_hex(salt, PBKDF2_SALT_LEN, salt_hex);
    bytes_to_hex(dk, PBKDF2_DK_LEN, dk_hex);
    snprintf(out, PASSWORD_HASH_SIZE + 1, "pbkdf2$%u$%s$%s",
             (unsigned)iters, salt_hex, dk_hex);
    crypto_zero(dk, sizeof(dk));
}

// Verify `password` against a stored shadow hash field (new PBKDF2 record or a
// legacy bare 64-char SHA-256 hex). Returns 1 on match, 0 on mismatch/parse
// error. Constant-time on the final hash comparison.
static int verify_against_record(const char *password, const char *username,
                                 const char *stored) {
    if (strncmp(stored, "pbkdf2$", 7) == 0) {
        const char *p = stored + 7;
        // iters
        uint32_t iters = 0;
        while (*p >= '0' && *p <= '9') { iters = iters * 10 + (uint32_t)(*p - '0'); p++; }
        if (*p != '$' || iters == 0) return 0;
        p++;
        // salt hex (PBKDF2_SALT_LEN*2), terminated by '$'
        char salt_hex[PBKDF2_SALT_LEN * 2 + 1];
        int i = 0;
        while (*p && *p != '$' && i < PBKDF2_SALT_LEN * 2) salt_hex[i++] = *p++;
        salt_hex[i] = '\0';
        if (*p != '$' || i != PBKDF2_SALT_LEN * 2) return 0;
        p++;
        uint8_t salt[PBKDF2_SALT_LEN];
        if (!hex_to_bytes(salt_hex, PBKDF2_SALT_LEN, salt)) return 0;
        // recompute and compare the whole record string in constant time
        uint8_t dk[PBKDF2_DK_LEN];
        pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                           salt, PBKDF2_SALT_LEN, iters, dk);
        char dk_hex[PBKDF2_DK_LEN * 2 + 1];
        bytes_to_hex(dk, PBKDF2_DK_LEN, dk_hex);
        int ok = ct_streq(dk_hex, p);
        crypto_zero(dk, sizeof(dk));
        crypto_zero(dk_hex, sizeof(dk_hex));
        return ok;
    }
    // Legacy: bare SHA-256 hex of (password || username). Kept so pre-#566
    // images still authenticate; any password change upgrades to PBKDF2.
    {
        char combined[256];
        size_t plen = strlen(password), ulen = strlen(username);
        size_t total = plen + ulen;
        if (total > sizeof(combined) - 1) total = sizeof(combined) - 1;
        memcpy(combined, password, plen);
        memcpy(combined + plen, username, ulen);
        combined[total] = '\0';
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256(combined, total, digest);
        char hex[SHA256_DIGEST_SIZE * 2 + 1];
        bytes_to_hex(digest, SHA256_DIGEST_SIZE, hex);
        int ok = ct_streq(hex, stored);
        crypto_zero(combined, sizeof(combined));
        crypto_zero(digest, sizeof(digest));
        return ok;
    }
}

// #307 real-hardware robustness: a single failed/short USB-MSC read of a
// critical boot-path config file used to be indistinguishable from "the file
// doesn't exist" - load_passwd()/load_shadow()/load_groups() just returned
// on a NULL/zero-size fat_read_file() result, silently leaving 0 users
// loaded even though PASSWD/SHADOW really are present on disk (the physical
// iMac14,4 live-USB symptom: "No user accounts found" despite a verified-good
// image). Real USB-MSC devices can be slower/flakier than QEMU's virtual
// device under real timing (see xhci_delay() in drivers/xhci.c), so retry a
// bounded number of times with a short backoff, and log every attempt and
// outcome to the persistent boot log (bootlog_write) - this runs before
// sti() (main.c), so there is no scheduler yet and no proc_sleep(); the
// backoff is a plain io_wait() spin, matching the style already used
// elsewhere in this early-boot window (see main.c's spinner delays).
// #192: BUT ABSENCE IS NOT A FAILED READ, AND IT USED TO BE REPORTED AS ONE.
// A directory lookup that misses is DETERMINISTIC: asking again cannot change
// the answer, and asking again with a ~900,000-iteration io_wait() backoff
// between attempts burns real time for nothing. Worse, it said the wrong thing:
// "/CONFIG/TZ.CFG: read FAILED/empty ... giving up" for a timezone the user has
// simply not chosen yet, three lines at a time, restarted every 2 seconds by
// gui/clock.c's refresh, forever - 125 lines of one 3634-line capture, and a
// real boot failure was misdiagnosed because the evidence had scrolled away.
// The same shape hit /CONFIG/PASSWD, /CONFIG/SHADOW and /CONFIG/GROUP, whose
// absence is equally correct on a virgin image since #568 removed the default
// accounts.
//
// So CLASSIFY FIRST. fat_exists() is the right question and it is ROUTING-
// CORRECT: it consults the ext2 root first and then the FAT ESP, exactly as
// fat_read_file() does. (The comment in users_init() below that says fat_exists
// "only consults the FAT ESP" is STALE - that gap was fixed in the same #568/#99
// work; it is corrected there.) A miss means ABSENT: return at once, and let
// rustkern/cfgread.rs decide whether it is worth a line. It is: exactly one,
// per path, per boot. A hit means the file is THERE and would not read, which
// is the genuine #307 fault: retry as before, and stay LOUD.
//
// Non-static: shared with gui/login.c and proc/syscall.c via the prototype in
// fs/fat.h (#307).
void *fat_read_file_retry(fat_fs_t *fs, const char *path, uint32_t *size_out) {
    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        uint32_t size = 0;
        void *data = fat_read_file(fs, path, &size);
        if (data && size > 0) {
            // One line the FIRST time each file reads, or when it recovers.
            // Not one per read: this helper also serves per-syscall readers.
            if (cfgread_report_rs(path, -1, CFG_OUTCOME_OK) == CFG_LOG_NOTE) {
                bootlog_write("[CFG] %s: read OK (%u bytes%s)", path, size,
                              attempt > 1 ? ", after a retry" : "");
            }
            *size_out = size;
            return data;
        }
        if (data) kfree(data);  // zero-size result: free before retrying

        if (fat_exists(fs, path) != 1) {
            // ABSENT: normal for an unconfigured setting, and retrying a
            // deterministic lookup miss cannot help. NO retry, NO backoff.
            if (cfgread_report_rs(path, -1, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE) {
                // Wording matters here: this helper serves files whose
                // absence means DIFFERENT things (no accounts, no autologin, no
                // wallpaper, no timezone), and #568 deliberately ships NO
                // default credentials, so promising "built-in defaults" would be
                // a comforting lie for the very file that most needs the truth.
                bootlog_write("[CFG] %s: not present; continuing without it "
                              "(normal until this is configured)", path);
            }
            *size_out = 0;
            return NULL;
        }

        // PRESENT and unreadable: the #307 case. Loud, and worth another go.
        int act = cfgread_report_rs(path, -1, CFG_OUTCOME_IOERR);
        if (act == CFG_LOG_WARN) {
            bootlog_write("[CFG] %s: PRESENT but read FAILED on attempt %d/%d%s",
                          path, attempt, max_attempts,
                          attempt < max_attempts ? " - retrying" : " - giving up");
        } else if (act == CFG_LOG_SUPPRESSED) {
            bootlog_write("[CFG] %s: further read failures will not be logged "
                          "until it reads successfully again", path);
        }
        if (attempt < max_attempts) {
            for (volatile uint32_t d = 0; d < 300000u * (uint32_t)attempt; d++) { io_wait(); }
        }
    }
    *size_out = 0;
    return NULL;
}

// ============================================================================
// Load from disk
// ============================================================================

// Parse /CONFIG/PASSWD
// Format: username:uid:gid:home:display_name:shell
static void load_passwd(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/PASSWD", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && user_count < MAX_USERS) {
        // Skip empty lines
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        user_entry_t *u = &user_table[user_count];
        memset(u, 0, sizeof(user_entry_t));

        // username
        copy_field(&src, u->username, sizeof(u->username), ':');
        if (!u->username[0]) break;

        // uid
        u->uid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // gid
        u->gid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // home
        copy_field(&src, u->home, sizeof(u->home), ':');

        // display_name
        copy_field(&src, u->display_name, sizeof(u->display_name), ':');

        // shell
        copy_field(&src, u->shell, sizeof(u->shell), ':');

        u->active = 1;
        user_count++;

        // Skip to next line
        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d users from /CONFIG/PASSWD\n", user_count);
    bootlog_write("[USERS] Loaded %d user(s) from /CONFIG/PASSWD", user_count);
}

// Parse /CONFIG/SHADOW
// Format: username:sha256_hex (or "*" for no-login)
static void load_shadow(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/SHADOW", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && shadow_count < MAX_USERS) {
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        shadow_entry_t *s = &shadow_table[shadow_count];
        memset(s, 0, sizeof(shadow_entry_t));

        // username
        copy_field(&src, s->username, sizeof(s->username), ':');
        if (!s->username[0]) break;

        // hash
        copy_field(&src, s->hash, sizeof(s->hash), ':');

        s->active = 1;
        shadow_count++;

        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d shadow entries from /CONFIG/SHADOW\n", shadow_count);
    bootlog_write("[USERS] Loaded %d shadow entrie(s) from /CONFIG/SHADOW", shadow_count);
}

// Parse /CONFIG/GROUP
// Format: groupname:gid:member1,member2,...
static void load_groups(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/GROUP", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && group_count < MAX_GROUPS) {
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        group_entry_t *g = &group_table[group_count];
        memset(g, 0, sizeof(group_entry_t));

        // groupname
        copy_field(&src, g->groupname, sizeof(g->groupname), ':');
        if (!g->groupname[0]) break;

        // gid
        g->gid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // members (comma-separated usernames, resolve to UIDs)
        char members_str[256];
        copy_field(&src, members_str, sizeof(members_str), ':');

        // Parse member list
        if (members_str[0]) {
            const char *m = members_str;
            while (*m && g->member_count < MAX_USERS) {
                char member_name[USERNAME_MAX];
                int mi = 0;
                while (*m && *m != ',' && mi < USERNAME_MAX - 1) {
                    member_name[mi++] = *m++;
                }
                member_name[mi] = '\0';
                if (*m == ',') m++;

                // Resolve username to UID
                user_entry_t *u = user_lookup_name(member_name);
                if (u) {
                    g->members[g->member_count++] = u->uid;
                }
            }
        }

        g->active = 1;
        group_count++;

        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d groups from /CONFIG/GROUP\n", group_count);
    bootlog_write("[USERS] Loaded %d group(s) from /CONFIG/GROUP", group_count);
}

// ============================================================================
// Save to disk
// ============================================================================

// #693: returns 0 only if /CONFIG/PASSWD is on the medium. A discarded status
// here loses an account change silently: the in-memory table is right, the disk
// is stale, and the change vanishes at the next boot with nothing logged.
static int save_passwd(void) {
    char *buf = kmalloc(8192);
    if (!buf) return -1;
    int pos = 0;

    for (int i = 0; i < user_count; i++) {
        user_entry_t *u = &user_table[i];
        if (!u->active) continue;

        // username:uid:gid:home:display_name:shell
        int n = snprintf(buf + pos, 8192 - pos, "%s:%u:%u:%s:%s:%s\n",
                         u->username, u->uid, u->gid, u->home,
                         u->display_name, u->shell);
        if (n > 0) pos += n;
    }

    int rc = fat_write_file(&g_fat_fs, "/CONFIG/PASSWD", buf, pos);
    kfree(buf);
    if (rc != 0)
        kprintf("[USERS] FAILED to write /CONFIG/PASSWD (rc=%d): account changes "
                "are NOT on disk and will be lost at reboot\n", rc);
    return rc;
}

// #693: returns 0 only if /CONFIG/SHADOW is on the medium. This is the PASSWORD
// file: a silently-dropped write means a changed password still works with the
// OLD one after a reboot, which is a security failure, not just data loss.
static int save_shadow(void) {
    char *buf = kmalloc(8192);
    if (!buf) return -1;
    int pos = 0;

    for (int i = 0; i < shadow_count; i++) {
        shadow_entry_t *s = &shadow_table[i];
        if (!s->active) continue;

        int n = snprintf(buf + pos, 8192 - pos, "%s:%s\n",
                         s->username, s->hash);
        if (n > 0) pos += n;
    }

    int rc = fat_write_file(&g_fat_fs, "/CONFIG/SHADOW", buf, pos);
    if (rc != 0)
        kprintf("[USERS] FAILED to write /CONFIG/SHADOW (rc=%d): password changes "
                "are NOT on disk; the OLD password still applies after reboot\n", rc);
    // Ensure shadow file is root-only
    perms_set("/CONFIG/SHADOW", 0, 0, 0600);
    kfree(buf);
    return rc;
}

// #693: returns 0 only if /CONFIG/GROUP is on the medium.
static int save_groups(void) {
    char *buf = kmalloc(8192);
    if (!buf) return -1;
    int pos = 0;

    for (int i = 0; i < group_count; i++) {
        group_entry_t *g = &group_table[i];
        if (!g->active) continue;

        int n = snprintf(buf + pos, 8192 - pos, "%s:%u:",
                         g->groupname, g->gid);
        if (n > 0) pos += n;

        // Members
        for (int j = 0; j < g->member_count; j++) {
            user_entry_t *u = user_lookup_uid(g->members[j]);
            if (u) {
                if (j > 0) buf[pos++] = ',';
                int len = strlen(u->username);
                memcpy(buf + pos, u->username, len);
                pos += len;
            }
        }
        buf[pos++] = '\n';
    }

    int rc = fat_write_file(&g_fat_fs, "/CONFIG/GROUP", buf, pos);
    kfree(buf);
    if (rc != 0)
        kprintf("[USERS] FAILED to write /CONFIG/GROUP (rc=%d)\n", rc);
    return rc;
}

// ============================================================================
// Create defaults on first boot
// ============================================================================

// #568: default-credential creation is now OPT-IN behind MAYTERA_SHIP_DEFAULT_ACCOUNTS
// and is NOT defined in any golden/public build. A fresh install ships NO
// accounts; the login gate forces first-boot account creation instead. Kept
// only as a documented dev convenience.
#ifdef MAYTERA_SHIP_DEFAULT_ACCOUNTS
static void create_defaults(void) {
    kprintf("[USERS] Creating default user accounts...\n");

    // These two passwords fail the strength policy on every count, which is
    // the point of the policy. They go in through the unchecked hashing core
    // so this dev convenience keeps working AS a dev convenience, and the
    // bypass is visible in one place instead of being a hole in the rule.
    // MAYTERA_SHIP_DEFAULT_ACCOUNTS is not defined in any golden or public
    // build; this is one more reason not to define it.

    // Create root user
    user_create("root", 0, 0, "/", "/APPS/MSH", "Root");
    set_password_hashed("root", "root");

    // Create admin user
    user_create("admin", 1000, 1000, "/HOME/ADMIN", "/APPS/MSH", "Admin");
    set_password_hashed("admin", "admin");

    // Create default groups
    group_entry_t *g;

    g = &group_table[group_count];
    memset(g, 0, sizeof(group_entry_t));
    g->gid = 0;
    strncpy(g->groupname, "root", sizeof(g->groupname) - 1);
    g->members[0] = 0;
    g->member_count = 1;
    g->active = 1;
    group_count++;

    g = &group_table[group_count];
    memset(g, 0, sizeof(group_entry_t));
    g->gid = 1000;
    strncpy(g->groupname, "users", sizeof(g->groupname) - 1);
    g->members[0] = 1000;
    g->member_count = 1;
    g->active = 1;
    group_count++;

    // Create /HOME/ADMIN directory + standard home-folder skeleton
    fat_mkdir(&g_fat_fs, "/HOME");
    fat_mkdir(&g_fat_fs, "/HOME/ADMIN");
    perms_set("/HOME/ADMIN", 1000, 1000, 0750);
    users_make_home_skeleton("/HOME/ADMIN", 1000, 1000);

    // Save to disk
    if (users_sync() != 0)
        kprintf("[USERS] sync failed\n");
    kprintf("[USERS] Default accounts created (root/root, admin/admin)\n");
}
#endif // MAYTERA_SHIP_DEFAULT_ACCOUNTS

// ============================================================================
// Public API
// ============================================================================

void users_init(void) {
    kprintf("[USERS] Initializing user database...\n");

    memset(user_table, 0, sizeof(user_table));
    memset(group_table, 0, sizeof(group_table));
    memset(shadow_table, 0, sizeof(shadow_table));
    user_count = 0;
    group_count = 0;
    shadow_count = 0;

    if (!g_fat_fs.mounted) {
        kprintf("[USERS] No filesystem mounted, using defaults\n");
        bootlog_write("[USERS] No filesystem mounted; deferring to in-RAM defaults (no accounts persisted)");
        users_initialized = true;
        return;
    }

    // Ensure /CONFIG exists
    fat_mkdir(&g_fat_fs, "/CONFIG");

    // Load the on-disk user database. Detect "do accounts exist?" by actually
    // LOADING (load_passwd/load_shadow/load_groups go through fat_read_file_retry,
    // which routes to the ext2 root volume when ext2 is the root fs), NOT via
    // a bare existence test. Load-then-count is correct on BOTH a FAT-ESP root
    // and an ext2 root.
    //
    // CORRECTED 2026-08-22 (#192): the paragraph that used to be here said
    // "fat_exists() only consults the FAT ESP and never sees /CONFIG on an ext2
    // root, so it always reported absent there". That was true when it was
    // written and is NOT true now - fat_exists() (fs/fat.c) has mirrored
    // fat_read_file()'s ext2-first-then-ESP dispatch since the #568/#99 work,
    // and its own comment says so. The stale warning mattered: #192 needs
    // exactly that routing-correct existence test to tell "absent" from "would
    // not read", and a doc that says the primitive is broken is a doc that
    // makes the next person hand-roll a private copy of it.
    load_passwd();
    load_shadow();
    load_groups();
    bootlog_write("[USERS] loaded user DB from /CONFIG: %d user(s), %d shadow, %d group(s)",
                  user_count, shadow_count, group_count);

    if (user_count == 0) {
        // No real accounts on disk: genuine fresh install (or an unreadable DB
        // even after fat_read_file_retry()'s bounded retries). Either way, NEVER
        // ship a known-password account.
#ifdef MAYTERA_SHIP_DEFAULT_ACCOUNTS
        kprintf("[USERS] No accounts loaded; creating built-in defaults "
                "(MAYTERA_SHIP_DEFAULT_ACCOUNTS build flag)\n");
        memset(user_table, 0, sizeof(user_table));
        memset(group_table, 0, sizeof(group_table));
        memset(shadow_table, 0, sizeof(shadow_table));
        group_count = 0;
        shadow_count = 0;
        create_defaults();
#else
        kprintf("[USERS] No accounts on disk; login gate will force first-boot "
                "account creation (#568)\n");
        bootlog_write("[USERS] No accounts loaded from /CONFIG; login gate forces "
                      "first-boot account creation (no default creds shipped) (#568)");
#endif
    }

    // Ensure every existing user has the standard home-folder skeleton (covers
    // accounts created before this feature existed; fat_mkdir is idempotent).
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].home[0])
            users_make_home_skeleton(user_table[i].home, user_table[i].uid, user_table[i].gid);
    }

    users_initialized = true;
    kprintf("[USERS] User database ready (%d users, %d groups)\n",
            user_count, group_count);
    bootlog_write("[USERS] User database ready: %d user(s), %d group(s)", user_count, group_count);
}

user_entry_t *user_lookup_uid(uint32_t uid) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            return &user_table[i];
        }
    }
    return NULL;
}

user_entry_t *user_lookup_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, name) == 0) {
            return &user_table[i];
        }
    }
    return NULL;
}

// #745: static, and deliberately not declared in users.h. See the block
// comment there. users_authenticate() below is the only way in.
static int user_verify_password(const char *username, const char *password) {
    if (!username || !password) return -1;

    // Find shadow entry
    shadow_entry_t *s = NULL;
    for (int i = 0; i < shadow_count; i++) {
        if (shadow_table[i].active &&
            strcmp(shadow_table[i].username, username) == 0) {
            s = &shadow_table[i];
            break;
        }
    }
    if (!s) return -1;

    // "*" means no-login
    if (strcmp(s->hash, "*") == 0) return -1;

    // Reject empty passwords outright: an empty password must never
    // authenticate (#566, closes the autologin/empty-password bypass class at
    // the lowest layer, defence in depth even though set_password also refuses
    // to store one).
    if (password[0] == '\0') return -1;

    return verify_against_record(password, username, s->hash) ? 0 : -1;
}

// Find (const) the shadow entry for a username, or NULL.
static shadow_entry_t *shadow_find(const char *username) {
    if (!username) return NULL;
    for (int i = 0; i < shadow_count; i++) {
        if (shadow_table[i].active &&
            strcmp(shadow_table[i].username, username) == 0)
            return &shadow_table[i];
    }
    return NULL;
}

int users_get_lockout(const char *username) {
    shadow_entry_t *s = shadow_find(username);
    if (!s || s->lockout_until_ms == 0) return 0;
    uint64_t now = sched_now_ms();
    if (now >= s->lockout_until_ms) return 0;
    return (int)((s->lockout_until_ms - now) / 1000ULL) + 1;
}

// Escalating lockout policy (#566). Applied AFTER a failed attempt, keyed off
// the running failed_attempts count. Reset to 0 on any success.
static void apply_lockout(shadow_entry_t *s) {
    uint32_t secs = 0;
    if (s->failed_attempts >= 10)      secs = 300; // 10+ -> 5 minutes
    else if (s->failed_attempts >= 5)  secs = 30;  // 5-9 -> 30 seconds
    if (secs)
        s->lockout_until_ms = sched_now_ms() + (uint64_t)secs * 1000ULL;
}

int users_authenticate(const char *username, const char *password) {
    if (!username || !password) return -1;
    if (password[0] == '\0') return -1;   // empty never authenticates

    shadow_entry_t *s = shadow_find(username);
    // Even with no shadow entry, do a dummy verify to keep timing uniform, then
    // fail. (No lockout state to touch for a non-existent account.)
    if (!s) {
        (void)verify_against_record(password, username, "*");
        return -1;
    }

    // Locked out right now?
    if (s->lockout_until_ms != 0) {
        if (sched_now_ms() < s->lockout_until_ms) {
            bootlog_write("[AUTH] '%s' rejected: locked out (%d s remaining)",
                          username, users_get_lockout(username));
            return -2;
        }
        // lockout expired
        s->lockout_until_ms = 0;
    }

    if (user_verify_password(username, password) == 0) {
        s->failed_attempts = 0;
        s->lockout_until_ms = 0;
        return 0;
    }

    s->failed_attempts++;
    apply_lockout(s);
    bootlog_write("[AUTH] '%s' failed auth (attempt %u%s)", username,
                  (unsigned)s->failed_attempts,
                  s->lockout_until_ms ? ", now locked out" : "");
    return (s->lockout_until_ms && sched_now_ms() < s->lockout_until_ms) ? -2 : -1;
}

// ===========================================================================
// #745 ELEVATION AUTHENTICATION. See the block comment in users.h for why this
// exists as a second function with a second counter rather than a flag.
// ===========================================================================
static void apply_elev_lockout(shadow_entry_t *s) {
    uint32_t secs = 0;
    if (s->elev_failed_attempts >= 10)      secs = 300;
    else if (s->elev_failed_attempts >= 5)  secs = 30;
    if (secs)
        s->elev_lockout_until_ms = sched_now_ms() + (uint64_t)secs * 1000ULL;
}

int users_elev_lockout(const char *username) {
    shadow_entry_t *s = shadow_find(username);
    if (!s || s->elev_lockout_until_ms == 0) return 0;
    uint64_t now = sched_now_ms();
    if (now >= s->elev_lockout_until_ms) return 0;
    return (int)((s->elev_lockout_until_ms - now) / 1000ULL) + 1;
}

int users_authenticate_elev(const char *username, const char *password) {
    if (!username || !password) return -1;
    if (password[0] == '\0') return -1;

    shadow_entry_t *s = shadow_find(username);
    if (!s) {
        // Uniform timing, and nothing to lock: the caller cannot reach this
        // anyway, because the kernel derives the name from the requester's own
        // uid rather than accepting one.
        (void)verify_against_record(password, username, "*");
        return -1;
    }

    if (s->elev_lockout_until_ms != 0) {
        if (sched_now_ms() < s->elev_lockout_until_ms) {
            bootlog_write("[ELEV] '%s' rejected: elevation lockout (%d s remaining)",
                          username, users_elev_lockout(username));
            return -2;
        }
        s->elev_lockout_until_ms = 0;
    }

    if (user_verify_password(username, password) == 0) {
        // Resets the ELEVATION counter only. Deliberately does NOT touch
        // failed_attempts / lockout_until_ms: a correct password here must not
        // become a way to clear a running LOGIN lockout.
        s->elev_failed_attempts = 0;
        s->elev_lockout_until_ms = 0;
        return 0;
    }

    s->elev_failed_attempts++;
    apply_elev_lockout(s);
    bootlog_write("[ELEV] '%s' failed elevation auth (attempt %u%s); login counter untouched",
                  username, (unsigned)s->elev_failed_attempts,
                  s->elev_lockout_until_ms ? ", elevation locked out" : "");
    return (s->elev_lockout_until_ms && sched_now_ms() < s->elev_lockout_until_ms) ? -2 : -1;
}

static group_entry_t *admin_group(void) {
    for (int i = 0; i < group_count; i++)
        if (group_table[i].active && strcmp(group_table[i].groupname, "admin") == 0)
            return &group_table[i];
    return NULL;
}

int users_grant_admin(uint32_t uid) {
    group_entry_t *g = admin_group();
    if (!g) {
        if (group_count >= MAX_GROUPS) return -1;
        g = &group_table[group_count];
        memset(g, 0, sizeof(group_entry_t));
        // gid 27 is the conventional `wheel`/admin gid and is not one this tree
        // hands out to users (accounts get gid == uid from 1000 up), so it
        // cannot collide with a primary group.
        g->gid = 27;
        strncpy(g->groupname, "admin", sizeof(g->groupname) - 1);
        g->active = 1;
        group_count++;
    }
    for (int i = 0; i < g->member_count && i < MAX_USERS; i++)
        if (g->members[i] == uid) return 0;          // already in: idempotent
    if (g->member_count >= MAX_USERS) return -1;
    g->members[g->member_count++] = uid;
    bootlog_write("[USERS] uid %u added to the 'admin' group (may elevate, #745)",
                  (unsigned)uid);
    return 0;
}

// THE ADMIN SET. See users.h.
int users_may_elevate(uint32_t uid) {
    if (uid == 0) return 1;

    group_entry_t *admin = admin_group();
    if (admin) {
        for (int i = 0; i < admin->member_count && i < MAX_USERS; i++)
            if (admin->members[i] == uid) return 1;
        return 0;
    }

    // No admin group on this image. The first-boot administrator is the admin
    // set, and nobody else is.
    extern uint32_t first_admin_uid_rs(void);
    return (uid == first_admin_uid_rs()) ? 1 : 0;
}

// #745: see users.h. Deliberately reuses shadow_find() rather than repeating
// the table scan, and deliberately does NOT touch failed_attempts/lockout: this
// is a capability question, not an authentication attempt, and answering it must
// never consume an attempt or be rate-limited (the lock policy calls it).
int users_can_authenticate(const char *username) {
    if (!username || !username[0]) return 0;
    if (!user_lookup_name(username)) return 0;      // no such account
    shadow_entry_t *s = shadow_find(username);
    if (!s) return 0;                                // in PASSWD, absent from SHADOW
    if (strcmp(s->hash, "*") == 0) return 0;         // explicit no-login
    if (s->hash[0] == '\0') return 0;                // empty record, never valid
    return 1;
}

// ===========================================================================
// THE PASSWORD CHOKEPOINT.
//
// What was here before did exactly two things to a password: it refused an
// empty one, and it hashed it. Nothing else in the kernel had an opinion. The
// public hardening page described it accurately as "no password-strength policy
// at all", with a 6-character floor on one first-boot screen and nothing on
// `passwd`, on Settings' Add User, or on SYS_USER_CREATE_PW.
//
// The strength rules now live in ONE place (rustkern/pwpolicy.rs) and are
// applied at ONE call (user_set_password), because the alternative, checking in
// each caller, is the arrangement that produced this bug: the check was written
// once, in the wizard, and the three other paths were added later without it.
// A caller that wants to pre-validate calls users_password_check(), which is the
// SAME function, so a UI cannot pass a password its own check accepted and then
// be refused by the kernel.
//
// The empty-password case is deliberately NOT handled here any more. "" used to
// mean "mark this account no-login", which is a different intent wearing the
// same call; sys_adduser() relied on it. It is now user_set_nologin(), so an
// empty password is simply a rejected password (PW_ERR_EMPTY) and the no-login
// intent has to be stated.
// ===========================================================================

// Find the shadow row for `username`, creating it if absent. NULL when the
// table is full. Reuses shadow_find() rather than repeating the scan a third
// time.
static shadow_entry_t *shadow_find_or_create(const char *username) {
    shadow_entry_t *s = shadow_find(username);
    if (s) return s;
    if (shadow_count >= MAX_USERS) return NULL;
    s = &shadow_table[shadow_count++];
    memset(s, 0, sizeof(shadow_entry_t));
    strncpy(s->username, username, sizeof(s->username) - 1);
    s->active = 1;
    return s;
}

// Hash and store. NO policy check: the only callers are user_set_password(),
// which has just run the policy, and the dev-only default-accounts block.
static int set_password_hashed(const char *username, const char *password) {
    shadow_entry_t *s = shadow_find_or_create(username);
    if (!s) return -1;

    // Generate a fresh 16-byte random salt and store a PBKDF2 record.
    uint8_t salt[PBKDF2_SALT_LEN];
    csprng_bytes(salt, sizeof(salt));
    make_pbkdf2_record(password, salt, PBKDF2_ITERATIONS, s->hash);
    crypto_zero(salt, sizeof(salt));
    // A password change clears any lockout for that account.
    s->failed_attempts = 0;
    s->lockout_until_ms = 0;
    return 0;
}

// #566: an account with no password is marked no-login ("*"), which
// user_verify_password already rejects. Explicit, so it cannot be confused with
// "the password happened to be the empty string".
int user_set_nologin(const char *username) {
    if (!username || !username[0]) return -1;
    shadow_entry_t *s = shadow_find_or_create(username);
    if (!s) return -1;
    strncpy(s->hash, "*", sizeof(s->hash) - 1);
    s->hash[sizeof(s->hash) - 1] = '\0';
    s->failed_attempts = 0;
    s->lockout_until_ms = 0;
    return 0;
}

int users_password_check(const char *username, const char *password) {
    if (!password) return PW_ERR_EMPTY;
    uint32_t plen = (uint32_t)strlen(password);

    // #218: the contains-username rule (PW_ERR_CONTAINS_USERNAME) exists to stop
    // a password being built out of the account's OWN per-machine identity:
    // "alice" must not be able to pick "alice1234". "root", though, is not a
    // per-machine secret identity; it is the fixed, universally-known reserved
    // name of uid 0. Applying the SUBSTRING form of the rule to it rejected any
    // strong passphrase that merely CONTAINED the token (e.g. a root password of
    // "MyTreeHasDeepRoots99"), which pushed the owner toward a WORSE password -
    // exactly the nonsense #218 reports. The only cases the rule was meant to
    // stop for root - "root", "toor", "rootroot" - are already refused at THIS
    // SAME chokepoint by the min-length (8), low-variety (>=4 distinct) and
    // breached-list rules, so nothing is lost by dropping the identity rule for
    // root. Every OTHER rule still applies to the root password. No non-root
    // account can be named "root" (user_create and the wizard both reserve it),
    // so this special case touches only uid 0. NOT applied to human accounts:
    // their contains-username rule is a legitimate per-machine check and stays.
    int reserved_root = (username && strcmp(username, "root") == 0);
    uint32_t ulen = (!reserved_root && username && username[0]) ? (uint32_t)strlen(username) : 0;
    return (int)pw_policy_check_rs((const uint8_t *)password, plen,
                                   ulen ? (const uint8_t *)username : (const uint8_t *)0,
                                   ulen);
}

int user_set_password(const char *username, const char *password) {
    if (!username || !username[0] || !password) return -1;

    int pc = users_password_check(username, password);
    if (pc != PW_OK) {
        // The REASON is logged, never the password. An operator reading the
        // trail can see that an account's password was refused and why, which
        // is the difference between "something failed" and a fact.
        bootlog_write("[AUTH] password REFUSED for '%s': %s (policy code %d)",
                      username, pw_policy_message(pc), pc);
        return PW_RC(pc);
    }
    return set_password_hashed(username, password);
}

// Byte equality with no early exit. Neither argument is a secret the caller
// does not already hold: both were typed by the same person into the same
// dialog moments ago, so there is no oracle here to defend. It is written this
// way so that nobody later copies an early-exit strcmp out of a password
// comparison into a place where the timing WOULD matter. The length comparison
// leaks the length; that is stated rather than hidden.
static int pw_bytes_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return diff == 0;
}

// #745. THE PAIR RULE, in one place. See users.h for the return codes.
int users_check_first_boot_pair(const char *username,
                                const char *user_password,
                                const char *root_password) {
    if (!username || !user_password || !root_password) return -1;

    // The contains-username rule is PER NAME. The single-password version of
    // this code checked one password against BOTH names, which was correct only
    // because there was one password; splitting it wrongly here would either
    // refuse a fine root password for containing the human's name, or accept a
    // root password of "root12345".
    int pc = users_password_check(username, user_password);
    if (pc != PW_OK) return PW_RC(pc);
    pc = users_password_check("root", root_password);
    if (pc != PW_OK) return PW_RC_ROOT(pc);

    // IDENTICAL PASSWORDS ARE REFUSED, not warned about.
    //
    // The entire point of asking for two passwords is that a compromise of the
    // desktop credential is no longer a compromise of uid 0. If the two are the
    // same string, that property is gone and nothing has been gained, while the
    // UI now CLAIMS two independent credentials. A control that misrepresents
    // what it protects is worse than the honest shared-credential arrangement
    // it replaced, because the next person reads the two fields and believes
    // them. So this is refused, with its own code so the screen can say why
    // instead of showing a generic failure.
    //
    // HONEST LIMIT: this is exact string equality. It refuses "hunter2" twice
    // and accepts "hunter2" / "hunter3", which is barely better. It is a floor
    // that removes the degenerate case, not a similarity test, and it is not
    // presented as one.
    if (pw_bytes_equal(user_password, root_password))
        return PW_RC_ROOT(PW_ERR_SAME_AS_OTHER);

    return 0;
}

// #745: staged root-password change. See users.h for the contract. The
// snapshot buffer holds a shadow RECORD (a PBKDF2 string), not a password, and
// it is zeroed on both commit and rollback so a stale record cannot be
// restored over a later, unrelated change.
static char root_pw_snapshot[PASSWORD_HASH_SIZE];
static int  root_pw_snapshot_state;   // 0 nothing staged, 1 root had a record, 2 root had none

void users_root_pw_commit(void) {
    crypto_zero(root_pw_snapshot, sizeof(root_pw_snapshot));
    root_pw_snapshot_state = 0;
}

void users_root_pw_rollback(void) {
    if (root_pw_snapshot_state == 1) {
        shadow_entry_t *s = shadow_find_or_create("root");
        if (s) {
            strncpy(s->hash, root_pw_snapshot, sizeof(s->hash) - 1);
            s->hash[sizeof(s->hash) - 1] = '\0';
            s->failed_attempts = 0;
            s->lockout_until_ms = 0;
        }
    } else if (root_pw_snapshot_state == 2) {
        // root had no shadow record at all before we staged one; putting an
        // empty record back would be a THIRD state. Deactivate the row.
        shadow_entry_t *s = shadow_find("root");
        if (s) s->active = 0;
    }
    users_root_pw_commit();
}

int users_root_pw_begin(const char *root_password) {
    if (!root_password) return -1;
    if (root_pw_snapshot_state != 0) {
        // A second begin() without a commit/rollback would overwrite the only
        // copy of the original record, so the first change could never be
        // undone. Refuse rather than silently lose it.
        kprintf("[USERS] root password change already staged; refusing to nest\n");
        return -1;
    }
    shadow_entry_t *s = shadow_find("root");
    if (s) {
        strncpy(root_pw_snapshot, s->hash, sizeof(root_pw_snapshot) - 1);
        root_pw_snapshot[sizeof(root_pw_snapshot) - 1] = '\0';
        root_pw_snapshot_state = 1;
    } else {
        root_pw_snapshot[0] = '\0';
        root_pw_snapshot_state = 2;
    }
    if (user_set_password("root", root_password) != 0) {
        users_root_pw_rollback();
        return -1;
    }
    return 0;
}

int user_create(const char *username, uint32_t uid, uint32_t gid,
                const char *home, const char *shell, const char *display_name) {
    if (!username) return -1;
    if (user_count >= MAX_USERS) return -1;

    // Check for duplicate username or UID
    if (user_lookup_name(username)) return -1;
    if (user_lookup_uid(uid)) return -1;

    user_entry_t *u = &user_table[user_count++];
    memset(u, 0, sizeof(user_entry_t));

    u->uid = uid;
    u->gid = gid;
    strncpy(u->username, username, sizeof(u->username) - 1);
    if (display_name) strncpy(u->display_name, display_name, sizeof(u->display_name) - 1);
    if (home) strncpy(u->home, home, sizeof(u->home) - 1);
    if (shell) strncpy(u->shell, shell, sizeof(u->shell) - 1);
    else strncpy(u->shell, "/APPS/MSH", sizeof(u->shell) - 1);
    u->active = 1;

    /* #711 slice 2: a user account IS an identity
     * (docs/CONTRACT_ARCHITECTURE.md section 3), so it gets a node in the
     * contract graph, recorded as a record in the tamper-evident journal. This
     * is the first producer of graph state outside GraphFS itself: creating an
     * account from Ring 3 makes a node appear in an append-only trail that
     * Ring 3 cannot write.
     *
     * A no-op before the journal is up (the accounts created during
     * users_init(), which runs before the filesystem-backed journal exists);
     * gfs_fold_init() seeds those from users_all() when it starts, so the graph
     * holds every account either way. The node id is (KIND_USER << 24) | uid,
     * which is the derivable-id scheme in fs/graphfs/fold.h; the literals are
     * spelled out because proc/ must not take a build dependency on fs/graphfs
     * headers for one call. */
    { extern int gfs_node_create(uint32_t node_id, uint16_t kind, uint32_t parent);
      (void)gfs_node_create((uint32_t)(((uint32_t)2 << 24) | (uid & 0x00FFFFFFu)),
                            2 /*GFS_KIND_USER*/, 0); }

    kprintf("[USERS] Created user '%s' (uid=%u, gid=%u)\n", username, uid, gid);
    return 0;
}

// #568: count active user accounts (login gate fresh-install detection).
int users_count_active(void) {
    int n = 0;
    for (int i = 0; i < user_count; i++)
        if (user_table[i].active) n++;
    return n;
}

// #568: create the initial administrator (uid 0) from an interactively chosen
// username + password. Replaces the old shipped root/root default with a
// user-chosen credential, hashed through the #566 PBKDF2 path. See users.h for
// the return codes.
int users_create_first_admin(const char *username, const char *user_password,
                             const char *root_password) {
    if (!username || !user_password || !root_password) return -1;

    // Validate username: non-empty, fits the field, printable ASCII, and no
    // ':' (the PASSWD field delimiter) / whitespace / control chars.
    size_t ulen = strlen(username);
    if (ulen == 0 || ulen >= USERNAME_MAX) return -2;
    for (size_t i = 0; i < ulen; i++) {
        unsigned char c = (unsigned char)username[i];
        if (c <= ' ' || c >= 127 || c == ':') return -2;
    }

    // #745: `root` is a reserved system account this function creates itself,
    // so it can never also be the interactive account's name. Rejected as an
    // invalid username (-2) rather than a generic failure, so the login screen
    // can say something useful. Checked BEFORE the passwords, because a
    // username of "root" would otherwise come back as a confusing
    // "password must not contain your username" against the human account.
    if (strcmp(username, "root") == 0) return -2;

    // Validate BOTH passwords against the FULL policy, and do it BEFORE
    // creating anything, so a refusal leaves nothing half-created and the
    // caller gets the specific reason AND which field it belongs to. The rule
    // itself lives in users_check_first_boot_pair() so the other first-boot
    // caller (sys_firstboot_admin) enforces exactly the same thing.
    {
        int pc = users_check_first_boot_pair(username, user_password, root_password);
        if (pc != 0) return pc;
    }

    if (user_lookup_name(username)) return -1;   // impossible on a fresh boot
    if (user_lookup_uid(0)) return -1;           // uid 0 already taken

    // #745 ROOT CAUSE FIX. This function used to mint the first interactive
    // account at uid 0, so a fresh install could not produce a non-root desktop
    // no matter what anything downstream did. It now creates TWO accounts:
    //
    //   root            uid 0     system account, owns the system files
    //   <username>      uid 1000  the human who is actually sitting there
    //
    // The uid is not written here; it comes from the ONE policy constant in
    // rustkern/sessionid.rs, whose boot self-test fails loudly if anyone sets it
    // back to 0.
    //
    // #745 FOLLOW-UP, NOW DONE: the two accounts used to be given the SAME
    // first-boot password, and this comment used to say so and call a separate
    // root password "the follow-up that removes the shared credential". It is
    // that follow-up. root's password is now its own argument, validated
    // independently, and refused if it equals the account password.
    //
    // WHAT IS STILL TRUE, and is a real trade rather than a detail: MayteraOS
    // has no sudo and no elevation mechanism. Root-only work means signing in
    // as root at the login gate. So a root password typed once and forgotten is
    // unrecoverable on this machine, which is why every screen that collects it
    // also confirms it. What is GAINED is that a compromise of the desktop
    // password is no longer a compromise of uid 0, and that the session was
    // already not uid 0: a compromised app, an AI tool call, or a stray write
    // cannot touch root-owned state without an explicit re-authentication.
    extern uint32_t first_admin_uid_rs(void);
    uint32_t admin_uid = first_admin_uid_rs();
    uint32_t admin_gid = admin_uid;
    if (user_lookup_uid(admin_uid)) return -1;

    // Build a sanitized home path /HOME/<NAME8>, uppercased alnum (FAT 8.3
    // friendly; ext2 stores it verbatim). Falls back to /HOME/ADMIN if the name
    // had no alnum characters.
    char home[HOME_PATH_MAX];
    int hp = 0;
    const char *pfx = "/HOME/";
    for (int i = 0; pfx[i] && hp < HOME_PATH_MAX - 1; i++) home[hp++] = pfx[i];
    int base = hp;
    for (size_t i = 0; i < ulen && (int)i < 8 && hp < HOME_PATH_MAX - 1; i++) {
        char c = username[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) home[hp++] = c;
    }
    home[hp] = '\0';
    if (hp <= base) { strncpy(home, "/HOME/ADMIN", sizeof(home) - 1); home[sizeof(home) - 1] = '\0'; }

    // The system account first. If anything below fails, both are rolled back,
    // so a half-created account database can never be persisted.
    if (user_create("root", 0, 0, "/", "/APPS/MSH", "Root") != 0) return -1;
    if (user_create(username, admin_uid, admin_gid, home, "/APPS/MSH", username) != 0) {
        user_delete(0);
        return -1;
    }
    // #745: the page that collected these passwords said this account can
    // administer the computer. Make that true.
    if (user_set_password(username, user_password) == 0)
        (void)users_grant_admin(admin_uid);
    if (user_set_password(username, user_password) != 0 ||
        user_set_password("root", root_password) != 0) {
        // Roll back BOTH so we never leave a passwordless account behind. The
        // shadow entries user_set_password may have created are removed by
        // user_delete (see users.c), and users_sync() is not called on this
        // path, so nothing reaches the disk.
        user_delete(admin_uid);
        user_delete(0);
        return -1;
    }

    // Groups: root (gid 0) holds root; users (gid 1000) holds the human. The
    // old code put the interactive account in the ROOT group, which was
    // harmless only while that account WAS root.
    group_entry_t *gr = group_lookup_gid(0);
    if (!gr && group_count < MAX_GROUPS) {
        gr = &group_table[group_count++];
        memset(gr, 0, sizeof(*gr));
        gr->gid = 0;
        strncpy(gr->groupname, "root", sizeof(gr->groupname) - 1);
        gr->active = 1;
    }
    if (gr) { gr->members[0] = 0; gr->member_count = 1; }

    group_entry_t *gu = group_lookup_gid(admin_gid);
    if (!gu && group_count < MAX_GROUPS) {
        gu = &group_table[group_count++];
        memset(gu, 0, sizeof(*gu));
        gu->gid = admin_gid;
        strncpy(gu->groupname, "users", sizeof(gu->groupname) - 1);
        gu->active = 1;
    }
    if (gu) { gu->members[0] = admin_uid; gu->member_count = 1; }

    // Home skeleton (Desktop/Documents/...), owned by the human, not by root.
    if (g_fat_fs.mounted) fat_mkdir(&g_fat_fs, "/HOME");
    users_make_home_skeleton(home, admin_uid, admin_gid);

    if (users_sync() != 0)
        kprintf("[USERS] sync failed\n");
    kprintf("[USERS] first-boot: '%s' (uid %u) + system root (uid 0) created; no default creds shipped\n",
            username, (unsigned)admin_uid);
    bootlog_write("[USERS] first-boot: interactive account '%s' uid=%u, home '%s' owned by %u:%u; "
                  "system root uid=0 created with its OWN password (#745)",
                  username, (unsigned)admin_uid, home, (unsigned)admin_uid, (unsigned)admin_gid);
    return 0;
}

// Create the standard per-user home subfolders. FAT is 8.3-only (LFN pending)
// so the on-disk names are <=8 chars; the Files app maps them to friendly
// labels (Documents/Downloads/...). No-op for the root '/' home.
void users_make_home_skeleton(const char *home, uint32_t uid, uint32_t gid) {
    if (!home || !home[0] || !g_fat_fs.mounted) return;
    if (home[0] == '/' && home[1] == '\0') return;
    char path[160];
    // Create each parent component first (e.g. /HOME before /HOME/ADMIN); the
    // disk may not have /HOME yet, in which case mkdir(/HOME/ADMIN) would fail.
    int n = 0;
    for (int i = 0; home[i] && n < 158; i++) {
        path[n++] = home[i];
        if (home[i] == '/' && n > 1) { path[n - 1] = '\0'; fat_mkdir(&g_fat_fs, path); path[n - 1] = '/'; }
    }
    path[n] = '\0';
    fat_mkdir(&g_fat_fs, path);          // the home dir itself
    perms_set(path, uid, gid, 0750);
    // Standard subfolders.
    //
    // #745: APPS and CONFIG are new. Until now there was NO per-user
    // application location anywhere in the OS and nothing searched for one, so
    // "install this app for me" had nowhere to put anything and the App Store's
    // only possible destination was root-owned /APPS. That is what made a
    // non-root install fail, and it is a LAUNCHER problem, not a permission
    // problem: the fix is a directory the user already owns, NOT a relaxation
    // of /APPS. A desktop session that can rewrite /APPS/MSH owns the next root
    // login, so /APPS, /GAMES and /CONFIG keep exactly the modes they have.
    //
    // CONFIG holds this user's own preferences (#683 userconf.c already writes
    // there and mkdirs it on demand) and, since #745, their Start-menu
    // fragments and their per-user install registry.
    //
    // The loop below stamps every one of these uid:gid 0750, so the owner has
    // rwx and no OTHER non-root user can even list them.
    //
    // The count is now derived from the array. It was the literal 6, which is
    // the shape of bug where adding an entry compiles, runs, and silently does
    // nothing.
    //
    // #221b: GAMES and GAMES/NETHACK. A DOS game writes its save next to its
    // executable, and /DOS/<GAME> is root-owned 0755, so a non-root session
    // could not save at all (see the #221b note in fs/perms.c). The game's own
    // config can point elsewhere, but only at a directory that ALREADY EXISTS:
    // NetHack calls fopen(), never mkdir(), so a missing save directory is
    // indistinguishable to it from an unwritable one. This function runs on
    // every login as well as at account creation, so the directory appears for
    // accounts that predate this change too, not only for new ones.
    //
    // The nested entry works because "GAMES" is created first: fat_mkdir on
    // "<home>/GAMES/NETHACK" needs its parent to exist, and array order is what
    // guarantees it. Keep GAMES before GAMES/NETHACK.
    //
    // #rawrite: GAMES/RA. Red Alert differs from the two above in that nothing
    // in the GAME can be pointed at this directory: it has no config file
    // naming a save path and no typed-path requester, and it ignores the
    // current directory it was launched with (measured under a DOSBox-X
    // reference run, 2026-08-27: it wrote its swap file and rewrote
    // REDALERT.INI into the EXECUTABLE's directory and left the launch CWD
    // empty). So this directory is not somewhere the game is asked to write,
    // it is where dos/int21svc.c's overlay REDIRECTS the writes it makes to
    // /DOS/RA. Created here, with the other two, so it exists for accounts
    // that predate the change as well as for new ones; dos_overlay_prepare()
    // creates it on demand too, and both paths set the same 0750.
    static const char *subs[] = { "DESKTOP", "DOCUMENT", "DOWNLOAD", "PICTURES",
                                  "MUSIC", "VIDEOS", "APPS", "CONFIG",
                                  "GAMES", "GAMES/NETHACK", "GAMES/SIMCITY",
                                  "GAMES/RA" };
    for (unsigned i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        int m = 0;
        for (; home[m] && m < 120; m++) path[m] = home[m];
        if (m > 0 && path[m - 1] != '/') path[m++] = '/';
        for (int j = 0; subs[i][j] && m < 158; j++) path[m++] = subs[i][j];
        path[m] = '\0';
        fat_mkdir(&g_fat_fs, path);
        perms_set(path, uid, gid, 0750);
    }
}

int user_delete(uint32_t uid) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            // Also remove shadow entry
            for (int j = 0; j < shadow_count; j++) {
                if (shadow_table[j].active &&
                    strcmp(shadow_table[j].username, user_table[i].username) == 0) {
                    shadow_table[j].active = 0;
                    break;
                }
            }
            user_table[i].active = 0;
            return 0;
        }
    }
    return -1;
}

user_entry_t *users_get_table(int *count) {
    if (count) *count = user_count;
    return user_table;
}

group_entry_t *groups_get_table(int *count) {
    if (count) *count = group_count;
    return group_table;
}

group_entry_t *group_lookup_gid(uint32_t gid) {
    for (int i = 0; i < group_count; i++) {
        if (group_table[i].active && group_table[i].gid == gid) {
            return &group_table[i];
        }
    }
    return NULL;
}

group_entry_t *group_lookup_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < group_count; i++) {
        if (group_table[i].active && strcmp(group_table[i].groupname, name) == 0) {
            return &group_table[i];
        }
    }
    return NULL;
}

int user_in_group(uint32_t uid, uint32_t gid) {
    // Check primary group first
    user_entry_t *u = user_lookup_uid(uid);
    if (u && u->gid == gid) return 1;

    // Check group membership
    group_entry_t *g = group_lookup_gid(gid);
    if (!g) return 0;

    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) return 1;
    }
    return 0;
}

// #693: 0 only if EVERY file reached the medium. All three saves are attempted
// even after one fails, because a partial database on disk is still better than
// stopping at the first error, but the caller is told the truth.
int users_sync(void) {
    if (!g_fat_fs.mounted) return -1;
    int rc = 0;
    if (save_passwd() != 0) rc = -1;
    if (save_shadow() != 0) rc = -1;
    if (save_groups() != 0) rc = -1;
    if (perms_sync()  != 0) rc = -1;
    if (rc != 0) {
        kprintf("[USERS] user database sync FAILED; disk copy is STALE\n");
        bootlog_write("[USERS] sync FAILED: user database on disk is STALE");
        return rc;
    }
    kprintf("[USERS] User database synced to disk\n");
    return 0;
}

// Expose the user table for SYS_LIST_USERS (caller skips inactive slots).
user_entry_t *users_all(int *count_out) {
    if (count_out) *count_out = user_count;
    return user_table;
}

// Delete a user by name (mark inactive + persist). Never deletes root (uid 0).
int user_delete_by_name(const char *username) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            if (user_table[i].uid == 0) return -1;
            user_table[i].active = 0;
            // #693: the account is gone from memory, but if it is still on disk
            // it COMES BACK at the next boot. That is not a deletion, so the
            // caller is told it failed.
            int rc = 0;
            if (save_passwd() != 0) rc = -2;
            if (save_shadow() != 0) rc = -2;
            if (perms_sync()  != 0) rc = -2;
            if (rc != 0)
                kprintf("[USERS] delete of '%s' NOT persisted; it will reappear "
                        "after a reboot\n", username);
            return rc;
        }
    }
    return -1;
}

