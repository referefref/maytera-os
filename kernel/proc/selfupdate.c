// selfupdate.c - MayteraOS kernel self-update (OTA) write path (#492 Stage 1a)
//
// The riskiest, most novel piece of the OTA security-update feature: the
// running kernel replaces its own boot kernel.elf on the FAT ESP, brick-safely,
// then (via a separate call) reboots to apply.
//
// -------------------------------------------------------------------------
// BRICK-SAFE CONTRACT (kernel_selfupdate_apply):
//   1. Verify sha256(new_kernel, len) == expected_sha256.        REFUSE if not.
//   2. Verify new_kernel is a valid x86-64 kernel ELF.           REFUSE if not.
//   3. Back up the CURRENT /boot/kernel.elf -> /boot/kernel.elf.bak, and keep
//      a copy of the old bytes in RAM for rollback.
//   4. Write the new image to ALL FOUR ESP paths, reading each back and
//      verifying sha256(readback) == expected after every write.
//   5. If ANY write/verify fails, RESTORE every path from the in-RAM backup and
//      return an error WITHOUT rebooting.
//   6. Only if all four verify, write /CONFIG/PENDING_UPDATE.TXT (build + sha)
//      and return SELFUPD_OK. Never reboots on its own.
//
// -------------------------------------------------------------------------
// SECURITY NOTE (#492 Stage 1b - authenticated):
//   CHECKED NOW (all before a single live byte is written):
//     1. mandatory sha256 integrity match (recomputed here, compared to the
//        caller-supplied expected digest),
//     2. real ELF shape/architecture validation,
//     3. MANDATORY RSA PKCS#1 v1.5 SIGNATURE over the sha256 digest, verified
//        against the baked-in OTA public key (proc/ota_pubkey.h). Only the
//        holder of the update server's private key can produce a signature that
//        verifies, so an unsigned / tampered / wrong-key image is REFUSED
//        (SELFUPD_ERR_SIGNATURE). The kernel no longer trusts the caller.
//   Additionally the SYS_KERNEL_SELFUPDATE syscall is privilege-gated in
//   proc/syscall.c: only a registered service holding SVC_PERM_SELFUPDATE may
//   invoke it, so arbitrary Ring-3 apps cannot reach this primitive at all.
// -------------------------------------------------------------------------

#include "selfupdate.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../crypto/crypto.h"
#include "../crypto/rsa.h"
#include "../exec/elf.h"
#include "../version.h"
#include "ota_pubkey.h"    // GENERATED baked-in RSA-2048 public key (OTA_PUBKEY_N/E)

extern fat_fs_t g_fat_fs;
extern void acpi_reboot(void);

// The four ESP kernel paths that MUST be kept in sync (the bootloader loads
// /boot/kernel.elf, but stale copies at the other three have historically
// caused the wrong kernel to load; see CLAUDE.md).
static const char *const g_kernel_paths[4] = {
    "/boot/kernel.elf",
    "/KERNEL.ELF",
    "/kernel.elf",
    "/EFI/BOOT/kernel.elf",
};
#define KPATH_PRIMARY  (g_kernel_paths[0])
#define KPATH_BACKUP   "/boot/kernel.elf.bak"
#define KPATH_MARKER   "/CONFIG/PENDING_UPDATE.TXT"

// ---- small hex helpers (no dependency on libc formatting) ----------------
static void bytes_to_hex(const uint8_t *in, uint32_t n, char *out /* 2n+1 */) {
    static const char hx[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = hx[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

// Parse up to 32 bytes of hex from s (stops at first non-hex). Returns count of
// bytes decoded. Accepts lower/upper case. Only used by the (compile-gated)
// isolation harness, so mark unused so golden builds without it stay -Werror clean.
__attribute__((unused))
static uint32_t hex_to_bytes(const char *s, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max) {
        int hi = -1, lo = -1;
        char c = s[n * 2];
        char d = s[n * 2 + 1];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        if (d >= '0' && d <= '9') lo = d - '0';
        else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
        else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
        if (hi < 0 || lo < 0) break;
        out[n] = (uint8_t)((hi << 4) | lo);
        n++;
    }
    return n;
}

// Read `path`, compute its sha256, compare to `expected`. Returns 1 on match,
// 0 on any mismatch/read failure. Frees its own buffer.
static int verify_path_sha(fat_fs_t *fs, const char *path,
                           const uint8_t expected[32]) {
    uint32_t sz = 0;
    void *buf = fat_read_file(fs, path, &sz);
    if (!buf) return 0;
    uint8_t d[32];
    sha256(buf, sz, d);
    kfree(buf);
    return memcmp(d, expected, 32) == 0;
}

// Restore all four kernel paths from the in-RAM backup. Returns 0 if every
// path was rewritten successfully, -1 otherwise (the dangerous case).
static int restore_all(fat_fs_t *fs, const void *old_bytes, uint32_t old_len) {
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        if (fat_write_file(fs, g_kernel_paths[i], old_bytes, old_len) != 0) {
            kprintf("[SELFUPD] RESTORE FAILED for %s\n", g_kernel_paths[i]);
            bootlog_write("[SELFUPD] RESTORE FAILED for %s", g_kernel_paths[i]);
            ok = -1;
        }
    }
    return ok;
}

// ---- SIGNATURE: verify a detached RSA PKCS#1 v1.5 signature over the 32-byte
// image digest against the baked-in OTA public key. Returns 1 if the signature
// is valid, 0 otherwise. This is the authentication gate: only the holder of
// the update server's private key can produce a signature that verifies here.
static int selfupdate_verify_signature(const uint8_t digest[32],
                                       const uint8_t *sig, uint32_t sig_len) {
    if (!sig || sig_len == 0) return 0;
    rsa_public_key_t pub = {
        .n = (uint8_t *)OTA_PUBKEY_N, .n_len = sizeof(OTA_PUBKEY_N),
        .e = (uint8_t *)OTA_PUBKEY_E, .e_len = sizeof(OTA_PUBKEY_E),
    };
    // rsa_verify_pkcs1_sha256 rebuilds the standard SHA-256 DigestInfo and
    // compares, matching `openssl dgst -sha256 -sign` on the server.
    int rc = rsa_verify_pkcs1_sha256(&pub, digest, 32, sig, (size_t)sig_len);
    return rc == RSA_SUCCESS;
}

// Public wrapper so the OTA client (via SYS_OTA_VERIFY_SIG) can authenticate a
// signed manifest against the SAME baked-in public key the apply path uses,
// without duplicating RSA or the key in userland. Returns 0 if the signature
// over `digest` verifies, -1 otherwise.
int kernel_ota_verify_sig(const uint8_t digest[32],
                          const uint8_t *sig, uint32_t sig_len) {
    if (!digest) return -1;
    return selfupdate_verify_signature(digest, sig, sig_len) ? 0 : -1;
}

int kernel_selfupdate_apply(const void *new_kernel, uint32_t len,
                            const uint8_t expected_sha256[32],
                            uint32_t target_build,
                            const uint8_t *sig, uint32_t sig_len) {
    fat_fs_t *fs = &g_fat_fs;

    // ---- 0. argument + environment sanity --------------------------------
    if (!fs->mounted)                              return SELFUPD_ERR_NOFS;
    if (!new_kernel || !expected_sha256)           return SELFUPD_ERR_ARG;
    if (len < SELFUPD_MIN_IMAGE_LEN ||
        len > SELFUPD_MAX_IMAGE_LEN)               return SELFUPD_ERR_SIZE;

    // ---- 1. INTEGRITY: sha256(image) must equal expected -----------------
    uint8_t digest[32];
    sha256(new_kernel, len, digest);
    if (memcmp(digest, expected_sha256, 32) != 0) {
        kprintf("[SELFUPD] REFUSED: sha256 mismatch\n");
        bootlog_write("[SELFUPD] REFUSED: sha256 mismatch (len=%u)", len);
        return SELFUPD_ERR_SHA_MISMATCH;
    }

    // ---- 2. SHAPE: must be a valid x86-64 kernel ELF ---------------------
    int ev = elf_validate(new_kernel, len);
    if (ev != 0) {
        kprintf("[SELFUPD] REFUSED: not a valid x86-64 ELF (elf_validate=%d)\n", ev);
        bootlog_write("[SELFUPD] REFUSED: bad ELF (elf_validate=%d)", ev);
        return SELFUPD_ERR_NOT_ELF;
    }

    // ---- 2b. SIGNATURE (Stage 1b, MANDATORY): authenticate the image -----
    // Verify a detached RSA signature over expected_sha256 against the baked-in
    // public key. A missing/tampered/wrong-key signature is refused here, so the
    // kernel no longer trusts the caller: reaching this syscall is NOT enough to
    // install an image; you must also present a signature only the update
    // server's private key can produce.
    if (!selfupdate_verify_signature(expected_sha256, sig, sig_len)) {
        kprintf("[SELFUPD] REFUSED: RSA signature did not verify (sig_len=%u)\n", sig_len);
        bootlog_write("[SELFUPD] REFUSED: bad signature (sig_len=%u)", sig_len);
        return SELFUPD_ERR_SIGNATURE;
    }

    kprintf("[SELFUPD] verified image: len=%u build->%u, sha OK, ELF OK, SIG OK\n",
            len, target_build);
    bootlog_write("[SELFUPD] verified image len=%u target build %u sig OK", len, target_build);

    // ---- 3. BACK UP the current live kernel (RAM copy + on-disk .bak) -----
    uint32_t old_len = 0;
    void *old_bytes = fat_read_file(fs, KPATH_PRIMARY, &old_len);
    if (!old_bytes || old_len == 0) {
        if (old_bytes) kfree(old_bytes);
        kprintf("[SELFUPD] ERROR: cannot read current %s for backup\n", KPATH_PRIMARY);
        bootlog_write("[SELFUPD] ERROR: backup read of %s failed", KPATH_PRIMARY);
        return SELFUPD_ERR_BACKUP;
    }
    if (fat_write_file(fs, KPATH_BACKUP, old_bytes, old_len) != 0) {
        kfree(old_bytes);
        kprintf("[SELFUPD] ERROR: cannot write backup %s\n", KPATH_BACKUP);
        bootlog_write("[SELFUPD] ERROR: backup write of %s failed", KPATH_BACKUP);
        return SELFUPD_ERR_BACKUP;
    }
    kprintf("[SELFUPD] backed up %s (%u bytes) -> %s\n",
            KPATH_PRIMARY, old_len, KPATH_BACKUP);

    // Stage the new bytes into a private kernel buffer. The caller may hand us a
    // Ring-3 pointer; copy it so the FAT/DMA write path never dereferences user
    // memory (mirrors SYS_PKG_WRITE), and so sha/ELF checks and the writes all
    // operate on the exact same immutable bytes.
    void *img = kmalloc(len);
    if (!img) {
        kfree(old_bytes);
        return SELFUPD_ERR_NOMEM;
    }
    memcpy(img, new_kernel, len);

    // ---- 4. WRITE the new image to all four paths, verify each -----------
    int failed = 0;
    for (int i = 0; i < 4 && !failed; i++) {
        const char *p = g_kernel_paths[i];
        if (fat_write_file(fs, p, img, len) != 0) {
            kprintf("[SELFUPD] WRITE FAILED for %s\n", p);
            bootlog_write("[SELFUPD] WRITE FAILED for %s", p);
            failed = 1;
            break;
        }
        if (!verify_path_sha(fs, p, expected_sha256)) {
            kprintf("[SELFUPD] VERIFY FAILED for %s\n", p);
            bootlog_write("[SELFUPD] VERIFY FAILED for %s", p);
            failed = 1;
            break;
        }
        kprintf("[SELFUPD] wrote+verified %s\n", p);
    }

    if (failed) {
        // ---- ROLLBACK: restore every path from the in-RAM backup ----------
        kprintf("[SELFUPD] rolling back all paths from backup...\n");
        bootlog_write("[SELFUPD] rolling back from backup (%u bytes)", old_len);
        int rr = restore_all(fs, old_bytes, old_len);
        kfree(img);
        kfree(old_bytes);
        if (rr != 0) {
            // Write failed AND restore failed: the only genuinely dangerous
            // outcome. The .bak on disk is still the good kernel.
            return SELFUPD_ERR_RESTORE;
        }
        return SELFUPD_ERR_WRITE;
    }

    kfree(img);
    kfree(old_bytes);

    // ---- 5. SUCCESS: write the pending-update marker ---------------------
    // The critical brick-safe replacement has succeeded and every path is
    // verified; a marker-write hiccup must NOT revert a known-good kernel, so a
    // marker failure is logged but still returns success.
    fat_mkdir(fs, "/CONFIG");   // best-effort; ok if it already exists
    char shahex[65];
    bytes_to_hex(expected_sha256, 32, shahex);
    char marker[256];
    // Fixed, greppable format for the Stage-1b client + post-reboot checks.
    int mlen = 0;
    {
        // hand-rolled to avoid pulling snprintf semantics; small + bounded
        const char *l1 = "STATUS=APPLIED\nBUILD=";
        char nbuf[16];
        int ni = 0;
        uint32_t b = target_build;
        char rev[16]; int ri = 0;
        if (b == 0) rev[ri++] = '0';
        while (b > 0 && ri < 15) { rev[ri++] = (char)('0' + (b % 10)); b /= 10; }
        while (ri > 0) nbuf[ni++] = rev[--ri];
        nbuf[ni] = '\0';
        // assemble: l1 + build + "\nSHA256=" + shahex + "\n"
        int k = 0;
        for (const char *s = l1; *s; s++) marker[k++] = *s;
        for (int j = 0; j < ni; j++) marker[k++] = nbuf[j];
        const char *l2 = "\nSHA256=";
        for (const char *s = l2; *s; s++) marker[k++] = *s;
        for (int j = 0; shahex[j]; j++) marker[k++] = shahex[j];
        marker[k++] = '\n';
        marker[k] = '\0';
        mlen = k;
    }
    if (fat_write_file(fs, KPATH_MARKER, marker, (uint32_t)mlen) != 0) {
        kprintf("[SELFUPD] WARNING: kernel updated OK but marker write failed\n");
        bootlog_write("[SELFUPD] WARN: marker write failed (update still applied)");
    } else {
        kprintf("[SELFUPD] wrote marker %s\n", KPATH_MARKER);
    }

    kprintf("[SELFUPD] SUCCESS: all 4 paths -> build %u (sha %s)\n",
            target_build, shahex);
    bootlog_write("[SELFUPD] SUCCESS build %u sha %s", target_build, shahex);
    return SELFUPD_OK;
}

void kernel_selfupdate_reboot(void) {
    extern void acpi_shutdown_flush(void);
    kprintf("[SELFUPD] flushing + rebooting to apply update...\n");
    bootlog_write("[SELFUPD] rebooting to apply update");
    acpi_shutdown_flush();   // best-effort flush of buffered disk/log state
    acpi_reboot();           // does not return on success
    // If ACPI reboot somehow returns, spin so we do not fall through.
    for (;;) { __asm__ volatile ("hlt"); }
}

#ifdef SELFUPDATE_ISOLATION_HARNESS
// ===========================================================================
// Stage-1a ISOLATION TEST HARNESS  (NOT compiled into golden/shipping kernels)
// ---------------------------------------------------------------------------
// Superseded by the Stage-1b signed userland OTA daemon. Compiled only when the
// build explicitly defines SELFUPDATE_ISOLATION_HARNESS (the golden Makefile
// does not), so no boot-time disk-file scaffold exists in a shipping kernel.
// When enabled, the primitive can be driven from a request file dropped onto
// the boot disk. /SELFUPD.REQ (key=value lines):
//     TARGET=/STAGE819.ELF
//     SHA256=<64 hex chars>
//     BUILD=819
// On boot we parse it, load TARGET, call kernel_selfupdate_apply(), record the
// outcome in /SELFUPD.LOG, delete the request (so it never loops), and on
// success reboot into the new kernel. Absent request file => no-op. This scaffold
// is deliberately NOT the golden mechanism; the Stage-1b signed OTA client
// replaces it. Left compiled in for the isolation build only.
// ===========================================================================
static void find_kv(const char *txt, uint32_t txtlen, const char *key,
                    char *out, uint32_t outsz) {
    out[0] = '\0';
    uint32_t klen = (uint32_t)strlen(key);
    for (uint32_t i = 0; i + klen < txtlen; i++) {
        // key must be at start of a line
        if ((i == 0 || txt[i - 1] == '\n' || txt[i - 1] == '\r') &&
            memcmp(txt + i, key, klen) == 0) {
            uint32_t j = i + klen;
            uint32_t o = 0;
            while (j < txtlen && txt[j] != '\n' && txt[j] != '\r' && o + 1 < outsz) {
                out[o++] = txt[j++];
            }
            out[o] = '\0';
            return;
        }
    }
}

void selfupdate_boot_check(fat_fs_t *fs) {
    if (!fs || !fs->mounted) return;
    if (!fat_exists(fs, "/SELFUPD.REQ")) return;   // normal boot: no-op

    kprintf("[SELFUPD] /SELFUPD.REQ present: running Stage-1a self-update harness\n");
    bootlog_write("[SELFUPD] harness: /SELFUPD.REQ present");

    uint32_t rlen = 0;
    char *req = (char *)fat_read_file(fs, "/SELFUPD.REQ", &rlen);
    if (!req || rlen == 0) {
        if (req) kfree(req);
        return;
    }

    char target[128], shahex[80], buildstr[16];
    find_kv(req, rlen, "TARGET=", target, sizeof(target));
    find_kv(req, rlen, "SHA256=", shahex, sizeof(shahex));
    find_kv(req, rlen, "BUILD=", buildstr, sizeof(buildstr));
    kfree(req);

    uint32_t build = 0;
    for (int i = 0; buildstr[i] >= '0' && buildstr[i] <= '9'; i++)
        build = build * 10 + (uint32_t)(buildstr[i] - '0');

    uint8_t exp[32];
    uint32_t nb = hex_to_bytes(shahex, exp, 32);

    // one-shot: remove the request first so a mid-apply reset cannot loop
    fat_delete(fs, "/SELFUPD.REQ");

    if (target[0] != '/' || nb != 32) {
        fat_write_file(fs, "/SELFUPD.LOG",
                       "ERROR: bad request (need TARGET=/path, 64-hex SHA256)\n", 52);
        kprintf("[SELFUPD] harness: malformed request, skipping\n");
        return;
    }

    uint32_t ilen = 0;
    void *img = fat_read_file(fs, target, &ilen);
    if (!img) {
        fat_write_file(fs, "/SELFUPD.LOG", "ERROR: TARGET not readable\n", 27);
        kprintf("[SELFUPD] harness: cannot read TARGET %s\n", target);
        return;
    }

    // Optional detached signature file for the (deprecated) harness path. With
    // mandatory signatures, a valid /SELFUPD.SIG (256 raw bytes) is required for
    // the apply to pass; absent it, apply returns SELFUPD_ERR_SIGNATURE.
    uint8_t *sig = 0; uint32_t sig_len = 0;
    sig = (uint8_t *)fat_read_file(fs, "/SELFUPD.SIG", &sig_len);

    int rc = kernel_selfupdate_apply(img, ilen, exp, build, sig, sig_len);
    if (sig) kfree(sig);
    kfree(img);

    // record outcome
    char log[96];
    const char *pfx = "selfupdate rc=";
    int k = 0;
    for (const char *s = pfx; *s; s++) log[k++] = *s;
    // rc is small negative..0; print sign+digits
    int v = rc;
    if (v < 0) { log[k++] = '-'; v = -v; }
    char rev[8]; int ri = 0;
    if (v == 0) rev[ri++] = '0';
    while (v > 0 && ri < 7) { rev[ri++] = (char)('0' + (v % 10)); v /= 10; }
    while (ri > 0) log[k++] = rev[--ri];
    log[k++] = '\n';
    log[k] = '\0';
    fat_write_file(fs, "/SELFUPD.LOG", log, (uint32_t)k);
    kprintf("[SELFUPD] harness: %s", log);
    bootlog_write("[SELFUPD] harness result rc=%d", rc);

    if (rc == SELFUPD_OK) {
        kprintf("[SELFUPD] harness: apply OK, rebooting to new kernel\n");
        kernel_selfupdate_reboot();   // does not return
    }
    // failure path: fall through, boot continues with the ORIGINAL kernel.
    kprintf("[SELFUPD] harness: apply FAILED (rc=%d), keeping current kernel\n", rc);
}
#endif // SELFUPDATE_ISOLATION_HARNESS
