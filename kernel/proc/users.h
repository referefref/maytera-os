// users.h - User and group database for MayteraOS
// Provides user authentication, lookup, and management
#ifndef USERS_H
#define USERS_H

#include "../types.h"

#define MAX_USERS   32
#define MAX_GROUPS  32
#define USERNAME_MAX 32
#define DISPLAY_NAME_MAX 64
#define HOME_PATH_MAX 64
#define SHELL_PATH_MAX 64
// Secure-login (#566): the SHADOW hash field now holds a self-describing
// PBKDF2 record "pbkdf2$<iters>$<salthex>$<hashhex>" (7 + up to 10 + 1 + 32 +
// 1 + 64 = ~115 chars), not a bare 64-char SHA-256 hex. Sized with headroom.
// Legacy bare-64-hex SHA-256 entries are still parsed for backward compat.
#define PASSWORD_HASH_SIZE 160

// User entry
typedef struct {
    uint32_t uid;
    uint32_t gid;               // Primary group
    char username[USERNAME_MAX];
    char display_name[DISPLAY_NAME_MAX];
    char home[HOME_PATH_MAX];   // Home directory path
    char shell[SHELL_PATH_MAX]; // Login shell path
    uint8_t active;             // Slot in use?
} user_entry_t;

// Group entry
typedef struct {
    uint32_t gid;
    char groupname[USERNAME_MAX];
    uint32_t members[MAX_USERS]; // UIDs in this group
    int member_count;
    uint8_t active;
} group_entry_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize user database, load from /CONFIG/PASSWD, /CONFIG/SHADOW, /CONFIG/GROUP
void users_init(void);

// Look up user by UID
user_entry_t *user_lookup_uid(uint32_t uid);

// Look up user by username
user_entry_t *user_lookup_name(const char *name);
user_entry_t *users_all(int *count_out);
int user_delete_by_name(const char *username);

// user_verify_password() USED TO BE DECLARED HERE, and that is exactly how the
// rule below got broken (#745).
//
// It is the RAW credential check: constant-time compare against /CONFIG/SHADOW,
// PBKDF2 record or legacy bare-SHA-256, and NO rate limiting of any kind. The
// comment that used to sit here said, in as many words, that interactive auth
// paths MUST call users_authenticate() instead so failed attempts are counted
// and lockouts enforced (#566). Two callers did not: sys_su() and
// sys_passwd_change() both called the raw check, so SYS_SU was an unthrottled,
// unaudited password oracle against every account on the machine, root
// included, while the login gate and lock screen were properly rate limited.
//
// A comment is not a control. This project has now had that lesson from the
// release gate (#514), the concurrency lint (#514), the shared git index (#707)
// and the build slot (#699), and the answer each time was to make the wrong
// thing INEXPRESSIBLE rather than to write the rule down more firmly. So
// user_verify_password() is now `static` inside proc/users.c and is not
// declared anywhere. users_authenticate() is the ONLY authenticator the kernel
// exposes, a future caller cannot reach around it, and one that tries does not
// compile.

// Rate-limited authentication (#566). Wraps user_verify_password() with a
// per-account failed-attempt counter and escalating lockout, so EVERY caller
// (login gate, lock unlock, remote paths) shares one uniform policy that a
// direct syscall loop cannot bypass. An empty/NULL password is always rejected.
// Returns:
//    0  success (attempt counter reset)
//   -1  bad credentials
//   -2  account is locked out right now (see users_get_lockout for seconds)
int users_authenticate(const char *username, const char *password);

// Seconds remaining on an account's lockout (0 = not locked out). For UI
// messaging ("Too many attempts. Try again in Ns.").
int users_get_lockout(const char *username);

// Set/change a user's password. THIS IS THE POLICY CHOKEPOINT: every path
// that can put a credential on an account reaches it, so the strength rules
// and the breached-password check are applied HERE rather than in each
// caller. (They used to be applied in exactly one caller, the first-boot
// screen, which is why `passwd` could set a one-character password.)
//
// Returns 0 on success, -1 on a generic failure (bad arguments, shadow table
// full), or PW_RC(code) from proc/pwpolicy.h when the password was REFUSED by
// policy: -201..-208, distinguishable so the caller can say which rule broke.
// Additive: every pre-existing caller tests != 0.
//
// An EMPTY password is a policy failure here, not a way to mark an account
// no-login. Use user_set_nologin() for that; it is a different intent and it
// now has a different function, so "" can no longer mean two things.
int user_set_password(const char *username, const char *password);

// Mark an account as unable to authenticate at all (shadow hash "*"). This is
// what sys_adduser() does for an account created without a password: an
// EXPLICIT no-login record rather than an absent one (#745).
int user_set_nologin(const char *username);

// Would this password be accepted for this account? Returns PW_OK or a
// PW_ERR_* code from proc/pwpolicy.h. Pure: it changes nothing, so UI can
// call it before committing to anything, and it is the SAME call
// user_set_password() makes, so a UI that passes cannot then be refused.
// `username` may be NULL, which skips the contains-username rule.
int users_password_check(const char *username, const char *password);

// #745: can this account authenticate AT ALL? True only when a shadow record
// exists AND is not the "*" no-login marker. Distinct from "the password is
// correct": this asks whether ANY password could ever succeed.
//
// It exists because the session-lock policy must refuse to lock a session whose
// user could never unlock it. That case is not hypothetical: sys_adduser()
// created accounts with no shadow record at all, so the `ref` account shipped
// at uid 1002 in every golden is exactly such an account.
int users_can_authenticate(const char *username);

// ===========================================================================
// #745 ELEVATION. Authentication for a privilege ELEVATION prompt, and the
// question of who may raise one.
//
// WHY THIS IS A SECOND FUNCTION AND NOT A FLAG ON users_authenticate().
//
// apply_lockout() locks an account for 30s after 5 failures and 300s after 10,
// and shadow_entry_t.failed_attempts is READ AND ENFORCED by the kernel login
// gate (gui/login.c), the lock screen and sshd. An elevation prompt can be
// raised by an APP. If elevation failures fed that same counter, an app that
// repeatedly raises prompts would walk the user toward being locked out of
// their own LOGIN SCREEN: a no-privilege denial of service against the person
// sitting at the machine. The UI cap of three attempts per invocation does not
// fix that on its own, because nothing caps the number of INVOCATIONS.
//
// So elevation gets its OWN counter and its OWN deadline on the same shadow
// record. The two are fully independent in both directions:
//   * an elevation failure never touches failed_attempts / lockout_until_ms,
//     so it can never lock anyone out of login, unlock or ssh;
//   * a login failure never touches the elevation counter, and an elevation
//     SUCCESS never resets the login counter, so this is not a back door for
//     clearing a login lockout either.
//
// Elevation is still RATE LIMITED, with the same escalating policy, because the
// point was never "no limit" - it was "not the login limit". The password check
// itself is the same one: both functions reach the same static
// user_verify_password() inside users.c, which is still the only credential
// comparison in the kernel.
//
// Returns 0 success, -1 bad credentials, -2 elevation lockout running.
int users_authenticate_elev(const char *username, const char *password);

// Seconds left on an account's ELEVATION lockout (0 = none). Separate from
// users_get_lockout(), which answers the same question for login.
int users_elev_lockout(const char *username);

// THE ADMIN SET. May this uid be offered an elevation prompt at all?
//
// This is the authorisation decision, and it is made BEFORE anything is drawn:
// the password that follows proves PRESENCE, not authority. A uid that fails
// this never sees a password field (App Store Surface C).
//
//   uid 0                      yes, and is never prompted (it already IS the
//                              privilege; see sys_elev_request()).
//   an "admin" group exists    membership of it decides, full stop. This is the
//                              admin-settable mechanism: /CONFIG/GROUP is
//                              root-owned, so the set cannot be edited by the
//                              accounts it governs.
//   no "admin" group           the first-boot administrator (FIRST_ADMIN_UID)
//                              and nobody else. That is the honest reading of
//                              the admin set on every image built before the
//                              group existed, and it means this ships working
//                              rather than shipping disabled.
int users_may_elevate(uint32_t uid);

// Put `uid` in the admin set, creating the `admin` group if this image does not
// have one yet. Called by BOTH first-boot provisioning paths, because both of
// them show the user a page that says the account they are creating can
// administer the computer, and that sentence has to be backed by something.
//
// Does NOT persist on its own: the callers already call users_sync() (directly
// or via sys_user_create_pw), so the group reaches /CONFIG/GROUP in the same
// save as the account rather than in a second one that can fail separately.
// Idempotent. Returns 0 on success, -1 if the group table is full.
int users_grant_admin(uint32_t uid);

// Create a new user
// Returns 0 on success, -1 on failure
int user_create(const char *username, uint32_t uid, uint32_t gid,
                const char *home, const char *shell, const char *display_name);

// First-boot account creation (#568). Count of active user accounts; the login
// gate uses this to detect a fresh install (0 accounts) and force the
// create-account flow instead of showing an empty user picker.
int users_count_active(void);

// FIRSTBOOT_MIN_PASSWORD USED TO LIVE HERE, and it was the whole password
// policy: a 6-character floor that ONE screen applied. gui/login.c checked
// it, users_create_first_admin() checked it, and no other path checked
// anything at all, so the rule was a property of a screen rather than of the
// system. It is deleted rather than raised, because leaving a second length
// constant next to the real one is how the two drift apart.
//
// The authoritative constants are PW_MIN_LEN / PW_MAX_LEN / PW_MIN_DISTINCT
// in rustkern/pwpolicy.rs. Ask for them with pw_policy_info_rs() instead of
// writing a number down twice; check a candidate with users_password_check().

// First-boot: create the initial accounts from an interactively chosen username
// and TWO passwords. Creates the human account (uid FIRST_ADMIN_UID) and the
// system `root` account (uid 0), replacing the old shipped root/root +
// admin/admin defaults (#568). Salts+hashes both via the #566 PBKDF2 path
// (user_set_password); creates the groups + home skeleton and persists
// PASSWD/SHADOW/GROUP.
//
// #745: root_password is a SEPARATE argument because this function used to set
// the SAME password on both accounts, so compromising the desktop password was
// also a compromise of uid 0. That was written down as a known limit here and
// in users.c, and this is the change that removes it. The two passwords must
// differ; see users_check_first_boot_pair().
//
// Returns:
//    0  success
//   -1  generic failure (table full / filesystem / uid 0 already taken)
//   -2  invalid username (empty, too long, contains ':' / whitespace / control,
//       or is the reserved name "root")
//   PW_RC(code) / PW_RC_ROOT(code)  a password was refused by policy;
//       PW_RC_CODE() / PW_RC_ROOT_CODE() give the PW_ERR_* reason
//       (proc/pwpolicy.h) and the band says WHICH password it was.
int users_create_first_admin(const char *username, const char *user_password,
                             const char *root_password);

// #745: validate the first-boot PAIR of passwords as ONE decision, before
// anything is created. Two callers set both passwords (this file's
// users_create_first_admin, for a virgin account database, and
// sys_firstboot_admin, for the shipped-image path the wizard actually takes),
// and if each applied the rules itself they would drift; the FIRST version of
// this code applied the single-password policy in one caller and nowhere else,
// which is how the whole password policy came to be a property of one screen.
//
// The contains-username rule is PER NAME, so this deliberately checks the
// account password against `username` and the root password against "root",
// rather than checking one password against both names as the single-password
// version did.
//
// Pure: it changes nothing, so a UI can call it to pre-validate.
// Returns:
//    0                                 both acceptable
//   -1                                 bad arguments
//   PW_RC(code)                        the ACCOUNT password broke rule `code`
//   PW_RC_ROOT(code)                   the ROOT password broke rule `code`
//   PW_RC_ROOT(PW_ERR_SAME_AS_OTHER)   the two are identical (see users.c for
//                                      why that is refused rather than warned)
int users_check_first_boot_pair(const char *username,
                                const char *user_password,
                                const char *root_password);

// #745: STAGED root-password change, so a provisioning step that sets root's
// password AND creates the human account can be undone as one unit rather than
// leaving root's password changed after a later failure.
//
//   begin()     snapshot root's current shadow record, then set the new
//               password. 0 on success, -1 on failure (nothing staged).
//   commit()    the operation succeeded; drop the snapshot.
//   rollback()  put root's previous record back exactly as it was (or remove
//               the record entirely if root had none).
//
// The snapshot is a shadow RECORD, never a password, and it never crosses this
// interface: callers get the three verbs, not the bytes.
int  users_root_pw_begin(const char *root_password);
void users_root_pw_commit(void);
void users_root_pw_rollback(void);

// Create the standard home-folder skeleton (Desktop/Documents/Downloads/...)
// for a user. Safe to call repeatedly; no-op for the root '/' home.
void users_make_home_skeleton(const char *home, uint32_t uid, uint32_t gid);

// Delete a user by UID
// Returns 0 on success, -1 on failure
int user_delete(uint32_t uid);

// Get the user table for enumeration
user_entry_t *users_get_table(int *count);

// Get the group table for enumeration
group_entry_t *groups_get_table(int *count);

// Look up group by GID
group_entry_t *group_lookup_gid(uint32_t gid);

// Look up group by name
group_entry_t *group_lookup_name(const char *name);

// Check if a user is in a group
int user_in_group(uint32_t uid, uint32_t gid);

// Save user database to disk
// #693: returns 0 only if the whole user database reached the medium.
MUST_CHECK int users_sync(void);

#endif // USERS_H
