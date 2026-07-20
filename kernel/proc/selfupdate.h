// selfupdate.h - MayteraOS kernel self-update (OTA) write path (#492 Stage 1a)
//
// This is the brick-safe mechanism by which the running kernel replaces its own
// boot kernel.elf on the FAT ESP and (via a separate call) reboots to apply.
// Stage 1a is the KERNEL MECHANISM ONLY: no network client, no UI, no update
// server (those are Stage 1b). See selfupdate.c for the full design + the
// honest security note (what is checked now vs deferred to 1b).
#ifndef SELFUPDATE_H
#define SELFUPDATE_H

#include "../types.h"
#include "../fs/fat.h"

// Result codes. 0 == success. All negative codes leave the LIVE kernel intact
// (either never touched, or fully restored from the in-RAM backup) EXCEPT
// SELFUPD_ERR_RESTORE, which means a write failed AND the rollback also failed
// (the only genuinely dangerous outcome; logged loudly, .bak still on disk).
#define SELFUPD_OK                0
#define SELFUPD_ERR_ARG          -1   // NULL args / bad parameters
#define SELFUPD_ERR_NOFS         -2   // boot FAT filesystem not mounted
#define SELFUPD_ERR_SIZE         -3   // image length outside sane bounds
#define SELFUPD_ERR_SHA_MISMATCH -4   // sha256(image) != expected_sha256 -> REFUSED
#define SELFUPD_ERR_NOT_ELF      -5   // not a valid x86-64 kernel ELF -> REFUSED
#define SELFUPD_ERR_NOMEM        -6   // kmalloc failed
#define SELFUPD_ERR_BACKUP       -7   // could not read+back-up the current kernel
#define SELFUPD_ERR_WRITE        -8   // a write/verify failed; rolled back OK, live kernel intact
#define SELFUPD_ERR_RESTORE      -9   // write failed AND rollback failed (DANGER)
#define SELFUPD_ERR_SIGNATURE   -10   // RSA signature over sha256 did NOT verify -> REFUSED
#define SELFUPD_ERR_PERM        -11   // caller lacks the selfupdate privilege -> REFUSED

// Sane image bounds. The current kernel is ~7 MB; refuse anything obviously
// wrong so a truncated/garbage image never reaches the write stage.
#define SELFUPD_MIN_IMAGE_LEN   (64u * 1024u)          // 64 KB floor
#define SELFUPD_MAX_IMAGE_LEN   (64u * 1024u * 1024u)  // 64 MB ceiling

// Apply a new kernel image, brick-safe. Verifies sha256(new_kernel,len) ==
// expected_sha256 and that it is a valid x86-64 kernel ELF; backs up the
// current /boot/kernel.elf to /boot/kernel.elf.bak; writes the new image to all
// four ESP kernel paths; reads each back and verifies its sha256 == expected;
// and only on full success writes /CONFIG/PENDING_UPDATE.TXT. If ANY step
// fails after the first write, every path is restored from the in-RAM backup
// and an error is returned WITHOUT rebooting. Does NOT reboot on its own.
//
// SIGNATURE (Stage 1b, MANDATORY): the kernel itself now authenticates the
// image. `sig`/`sig_len` is a detached RSA PKCS#1 v1.5 signature over
// SHA-256(new_kernel) produced with the update server's private key; it is
// verified against the baked-in public key (proc/ota_pubkey.h) BEFORE any byte
// is written. An unsigned, wrong-key, or tampered image is refused with
// SELFUPD_ERR_SIGNATURE. This makes the primitive stop trusting the caller: a
// Ring-3 process that reaches the syscall still cannot install an image it
// cannot sign. Pass sig=NULL/sig_len=0 to get an immediate SELFUPD_ERR_SIGNATURE.
int kernel_selfupdate_apply(const void *new_kernel, uint32_t len,
                            const uint8_t expected_sha256[32],
                            uint32_t target_build,
                            const uint8_t *sig, uint32_t sig_len);

// Verify a detached RSA signature over a 32-byte digest against the baked-in
// OTA public key (proc/ota_pubkey.h). Returns 0 if valid, -1 otherwise. Used by
// SYS_OTA_VERIFY_SIG so the userland OTA client can authenticate a signed
// manifest before acting on it, reusing the kernel's single trusted key.
int kernel_ota_verify_sig(const uint8_t digest[32],
                          const uint8_t *sig, uint32_t sig_len);

// Flush pending disk state, then ACPI-reboot to boot the freshly written
// kernel. Does not return on success. Call ONLY after a SELFUPD_OK apply.
void kernel_selfupdate_reboot(void);

// Stage-1a ISOLATION TEST HARNESS (superseded by the Stage-1b signed OTA
// client). Compiled ONLY when SELFUPDATE_ISOLATION_HARNESS is defined, which
// the golden Makefile does NOT define, so the boot-time /SELFUPD.REQ scaffold is
// absent from every golden/shipping kernel. The live trigger is the userland
// OTA daemon via SYS_KERNEL_SELFUPDATE, not a disk-file scaffold.
#ifdef SELFUPDATE_ISOLATION_HARNESS
void selfupdate_boot_check(fat_fs_t *fs);
#endif

#endif // SELFUPDATE_H
