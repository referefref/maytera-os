// proc/secretuse.h - #secretuse: SYS_SECRET_USE, the SECRET-USE PROXY.
//
// ===========================================================================
// THE WHOLE IDEA IN ONE PARAGRAPH.
// ===========================================================================
// Ring 3 used to HOLD the user's TOTP shared secrets: /APPS/MFA read them from a
// file obfuscated with one fixed XOR byte, kept them in its own address space,
// and computed codes itself. Any read of that file or that process WAS the second
// factor. Now the secret lives only inside an authenticated, password-derived
// container that only Ring 0 can open, and Ring 3 asks for A CODE. The
// enumerated operations below are the ENTIRE surface, and none of them returns a
// secret. That is the property, and the file the app used to read is now
// root-only so that the absence of a `get` operation is UNREACHABILITY rather
// than mere omission.
//
// ===========================================================================
// WHAT IS DELIBERATELY NOT HERE. Read backlog A4 before proposing any of it.
// ===========================================================================
//   * There is NO operation that returns a shared secret, a base32 seed, or a
//     derived key. Not for "export", not for "backup", not for a debug build.
//     Adding one puts the file back in Ring 3 and undoes the change.
//   * There is NO caller-supplied counter or challenge. The counter comes from
//     realtime_sec_rs(). A responder over attacker-chosen input is an ORACLE: the
//     caller can then compute valid authenticators for a time or a challenge of
//     its choosing, which is the primitive for authenticating as the user
//     elsewhere. That reasoning is why secret_use(SMB, ntlmv2_response, challenge)
//     was refused (backlog A4), and a caller-supplied TOTP counter is the same
//     construction: it would let one call harvest a month of future codes.
//   * There is NO bearer-key proxy here. Backlog A5 measured that proxying an API
//     key buys ZERO without DESTINATION PINNING, because aiclient.c:3369 builds
//     the header in Ring 3 and the app picks the URL. Do not add one and describe
//     it as protected.
//
// ===========================================================================
// WHAT A CODE IS WORTH, stated so the protection is not oversold.
// ===========================================================================
// A TOTP code is a bearer token for one period. Any process running as the same
// uid can call SYS_SECRET_USE and read the current code, exactly as it could read
// it off the screen. What it CANNOT do is obtain the secret, and therefore it
// cannot generate a code at any other time, cannot take the secret to another
// machine, and cannot survive the session key being wiped. The right further
// control is a capability class for secret.use with a consent prompt; that is NOT
// built here, and it is recorded in docs/OPEN_BACKLOG.md A3 rather than claimed.
//
// ===========================================================================
// KEY LIFECYCLE. The one sentence that makes the encryption real.
// ===========================================================================
// The key is PBKDF2-HMAC-SHA256 over the user's login password (crypto/pbkdf2.c,
// the same KDF the SHADOW verifier uses, with an INDEPENDENT per-user salt so the
// two derivations are unrelated). It is derived at users_authenticate(), the one
// authenticator chokepoint, held ONLY in kernel memory, written to NO file, and
// wiped when the session locks. So even root reading the container off the disk
// gets ciphertext, and the confidentiality rests on the key rather than on a
// permission bit. That matters because backlog A2 measured that /CONFIG's inode
// modes disagree with PERMS.DB, so a permission bit is the weaker of the two
// controls and must not be the one carrying the weight.
#ifndef PROC_SECRETUSE_H
#define PROC_SECRETUSE_H

#include "../types.h"

// ---------------------------------------------------------------------------
// THE OPERATIONS. This list IS the surface. Grep it before believing any claim
// about what Ring 3 can ask for.
// ---------------------------------------------------------------------------
/// -> account count, or SECRET_E_LOCKED. Lets the app say "locked, log in again"
/// instead of showing an empty list, which is how the old app turned a failed
/// load into "you have no accounts".
#define SECRET_OP_STATUS   0
/// out = secret_totp_info_t[], outcap in BYTES -> count. Names and geometry only.
#define SECRET_OP_LIST     1
/// out = secret_totp_code_t[], outcap in BYTES -> count. The CURRENT code for
/// every account, computed against the KERNEL clock. One call per refresh.
#define SECRET_OP_CODES    2
/// in = const secret_totp_add_t* -> 0. The secret goes IN and never comes back.
#define SECRET_OP_ADD      3
/// idx -> 0. Removes an account and wipes its secret from kernel memory.
#define SECRET_OP_DEL      4

// Negative returns. Distinguished on purpose: "locked" and "corrupt" and "no
// clock" are different things the app must say differently to the user.
#define SECRET_E_ARG      (-1)
/// No session key for this uid. The user has not authenticated since boot, or the
/// session was locked and the key was wiped. NOT an error about the store.
#define SECRET_E_LOCKED   (-2)
#define SECRET_E_IO       (-3)
/// The container failed its Poly1305 tag: wrong key, or the bytes were altered.
/// Which one, deliberately cannot be told apart.
#define SECRET_E_AUTH     (-4)
#define SECRET_E_FORMAT   (-5)
#define SECRET_E_NOSPACE  (-6)
#define SECRET_E_FULL     (-7)
/// The kernel clock has never presented a plausible time, so a code would be a
/// wrong answer that looks right.
#define SECRET_E_NOCLOCK  (-8)
/// The secret class is declared against a key-source tier that is not built.
#define SECRET_E_NOTIMPL  (-9)

#define SECRET_NAME_MAX   48
#define SECRET_B32_MAX    128
#define SECRET_CODE_MAX   12
#define SECRET_MAX_ACCOUNTS 32

// Field order gives these no padding at all; the _Static_asserts below lock the
// sizes, because a Ring-3 struct that disagrees with the kernel's by one byte is
// a silent misread of every field after the first.
typedef struct {
    char     issuer[SECRET_NAME_MAX];
    char     label[SECRET_NAME_MAX];
    uint16_t digits;
    uint16_t period;
    uint16_t alg;       // 1 = SHA1, 256 = SHA256
    uint16_t reserved;  // MUST be 0
} secret_totp_info_t;

typedef struct {
    char     code[SECRET_CODE_MAX];   // NUL-terminated decimal digits
    uint16_t remaining;               // seconds left in this window, 1..period
    uint16_t period;
} secret_totp_code_t;

typedef struct {
    char     issuer[SECRET_NAME_MAX];
    char     label[SECRET_NAME_MAX];
    char     b32[SECRET_B32_MAX];     // INPUT ONLY. Nothing ever reads this back out.
    uint16_t digits;
    uint16_t period;
    uint16_t alg;
    uint16_t reserved;
} secret_totp_add_t;

_Static_assert(sizeof(secret_totp_info_t) == 104, "secret_totp_info_t ABI");
_Static_assert(sizeof(secret_totp_code_t) == 16,  "secret_totp_code_t ABI");
_Static_assert(sizeof(secret_totp_add_t)  == 232, "secret_totp_add_t ABI");

// ---------------------------------------------------------------------------
// The syscall body. Args are USER pointers; the handler copies in and out.
// Process context only (it reads and writes files), which is why it is reached
// only from the syscall dispatcher and never from an ISR.
// ---------------------------------------------------------------------------
int64_t sys_secret_use(int op, uint32_t idx, void *out, uint32_t outcap,
                       const void *in, uint32_t inlen);

// ---------------------------------------------------------------------------
// Key lifecycle, called from proc/users.c and proc/syscall.c. Not a syscall.
// ---------------------------------------------------------------------------
// Derive and install this user's secret-store key. Called from
// users_authenticate() on SUCCESS ONLY, which is the single authenticator
// chokepoint: every path that proves a password goes through it, so there is
// exactly one place the key can come into existence.
//
// COST, stated because it is not free: this is a SECOND PBKDF2 over the same
// password (the store's salt is independent of SHADOW's by design, so the work
// cannot be shared), so a login that already spent ~80-250ms verifying spends
// that again. It is skipped entirely for a user with no secret store and no
// legacy MFA.DB to migrate, which is most users, so the common login is unchanged.
void seckey_on_authenticate(const char *username, const char *password);

// RE-KEY the store when the user's password changes.
//
// WHY THIS EXISTS, and it was a real hole in the first cut of this feature: the
// store key is derived from the login password, so changing the password makes
// every existing container UNOPENABLE. Without this the user's enrolled second
// factors would be silently destroyed by a routine `passwd`, which is exactly the
// "lose a secret without being told" failure secstore.rs's own header warns about.
//
// Called from sys_passwd() AFTER the new password is successfully set, and only on
// the path that PROVED the old one. Order matters: re-keying first would risk
// re-keying to a password that then fails the strength policy and never gets set,
// leaving no way in at all. Re-keying after can leave the store under the old key
// if the write fails, which is recoverable and is reported loudly.
//
// Returns 1 if it re-keyed, 0 if there was nothing to re-key, -1 on failure (and
// the caller must report that, because the user's accounts are then unreachable
// until someone restores the old password).
//
// AN ADMIN RESET CANNOT DO THIS, and that is correct rather than a limitation.
// When root changes another user's password without supplying the old one, there
// is no old key, so the store cannot be re-keyed and its contents are gone. A
// secret protected by a password is supposed to be unrecoverable to someone who
// does not know that password. sys_passwd() logs it plainly instead of letting the
// user discover it later.
int seckey_rekey(const char *username, const char *old_password,
                 const char *new_password);

// Wipe the key and every decrypted record for this uid, or for all uids.
// Called from sys_session_lock(): the key exists only while the session is
// unlocked, and unlocking runs users_authenticate() again, which re-derives it.
void seckey_wipe_uid(uint32_t uid);
void seckey_wipe_all(void);

// Boot self-test for rustkern/totp.rs plus the keyring's own invariants.
// Returns 0, or the failing case number. `out_checks` receives how many checks
// ran: a self-test that runs zero and returns 0 is vacuous.
int secretuse_selftest(uint32_t *out_checks);

#endif // PROC_SECRETUSE_H
