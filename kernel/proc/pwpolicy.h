// pwpolicy.h - password policy result codes, shared by every kernel path that
// can set a password. The policy ITSELF lives in rustkern/pwpolicy.rs; this
// header is the C side of that FFI plus the return-code encoding.
//
// WHY THE CODES ARE DISTINGUISHABLE
// ---------------------------------
// Before this, every rejection came back as a bare -1 and the UI could only
// say "could not set password". A user who is told that, with a password they
// believe is fine, retypes the same one. The caller has to be able to tell
// "too short" from "on a breached list" from "contains your username" to say
// anything useful, so the codes below are carried all the way out to Ring 3.
//
// THE ENCODING. The kernel functions that set passwords already use -1 for a
// generic failure, -2 for "locked out" and -14 for a bad user pointer, and
// every existing caller tests `!= 0`. So a policy rejection is returned as
// PW_RC(code), which lands in -201..-208: additive (non-zero, so old callers
// still see a failure) and unambiguous (no collision with any existing code).
#ifndef PROC_PWPOLICY_H
#define PROC_PWPOLICY_H

#include "../types.h"

// MUST match the PW_* constants in rustkern/pwpolicy.rs and libc/pwpolicy.h.
// The order is the order the rules are checked in, so a caller receives the
// FIRST rule the password broke.
#define PW_OK                     0
#define PW_ERR_EMPTY              1
#define PW_ERR_BADCHAR            2
#define PW_ERR_TOO_SHORT          3
#define PW_ERR_TOO_LONG           4
#define PW_ERR_CONTAINS_USERNAME  5
#define PW_ERR_LOW_VARIETY        6
#define PW_ERR_SEQUENCE           7
#define PW_ERR_BREACHED           8
// #745: the first-boot flow now sets TWO passwords, the human account's and
// root's. A password that is perfectly acceptable on its own can still be
// refused for being the SAME on both accounts, because that reproduces exactly
// the weakness the two-password change removes: one compromised credential is
// then also uid 0. It needs its own code, not a generic failure, so the screen
// can say WHY.
//
// It is NOT produced by pw_policy_check_rs(). That function judges ONE password
// in isolation and knows nothing about any other one. This code is produced by
// users_check_first_boot_pair() in proc/users.c, which is the single place the
// pair rule lives.
#define PW_ERR_SAME_AS_OTHER      9
#define PW_ERR_LAST               9

// Return-code encoding, see the header comment.
#define PW_RC_BASE 200
#define PW_RC(code) (-(PW_RC_BASE + (int)(code)))
// True when rc is a policy rejection rather than a generic failure.
#define PW_RC_IS_POLICY(rc) \
    ((rc) <= -(PW_RC_BASE + PW_ERR_EMPTY) && (rc) >= -(PW_RC_BASE + PW_ERR_LAST))
// The PW_ERR_* code inside an rc, or PW_OK if it is not a policy rejection.
#define PW_RC_CODE(rc) (PW_RC_IS_POLICY(rc) ? (-(int)(rc) - PW_RC_BASE) : PW_OK)

// #745: WHICH of the two first-boot passwords was refused. One band could not
// say, and "your password broke rule 3" pointing at the wrong field is worse
// than no message: the user retypes the field that was already fine. So the
// ROOT password gets its own band with the same shape and a different base.
// -221..-229 collides with nothing: -1 generic, -2 locked out, -14 bad pointer,
// -201..-209 the account-password band.
#define PW_RC_ROOT_BASE 220
#define PW_RC_ROOT(code) (-(PW_RC_ROOT_BASE + (int)(code)))
#define PW_RC_ROOT_IS_POLICY(rc) \
    ((rc) <= -(PW_RC_ROOT_BASE + PW_ERR_EMPTY) && (rc) >= -(PW_RC_ROOT_BASE + PW_ERR_LAST))
#define PW_RC_ROOT_CODE(rc) \
    (PW_RC_ROOT_IS_POLICY(rc) ? (-(int)(rc) - PW_RC_ROOT_BASE) : PW_OK)

// The two bands must never overlap, or a root-password rejection would decode
// as an account-password one and point at the wrong field. Checked by the
// compiler rather than by reading, because PW_ERR_LAST is the kind of constant
// that grows.
_Static_assert(-(PW_RC_BASE + PW_ERR_LAST) > -(PW_RC_ROOT_BASE + PW_ERR_EMPTY),
               "PW_RC and PW_RC_ROOT bands overlap: raise PW_RC_ROOT_BASE");

// The policy constants and the facts about the breached-password table, read
// from the ONE place that owns them (rustkern/pwpolicy.rs) so no UI string has
// to hardcode a number that can drift.
typedef struct {
    uint32_t min_len;
    uint32_t max_len;
    uint32_t min_distinct;
    uint32_t table_entries;
    uint32_t table_min_len;
    uint32_t table_bytes;
} pw_policy_info_t;

// Size lock on the FFI struct: rustkern/pwpolicy.rs declares the Rust twin
// #[repr(C)] with the same six u32 fields. If either side grows a field the
// build stops here rather than silently reading past the end of the other.
_Static_assert(sizeof(pw_policy_info_t) == 24,
               "pw_policy_info_t must match #[repr(C)] PwPolicyInfo in rustkern/pwpolicy.rs");

// rustkern/pwpolicy.rs
uint32_t pw_policy_check_rs(const uint8_t *pw, uint32_t pw_len,
                            const uint8_t *user, uint32_t user_len);
void pw_policy_info_rs(pw_policy_info_t *out);
uint32_t pwpolicy_selftest_rs(void);

// Human-readable reason for a PW_ERR_* code. Used by the kernel first-boot
// account screen (gui/login.c); libc/pwpolicy.h carries the same strings for
// userland apps. Deliberately says WHAT to change, not just that it failed.
static inline const char *pw_policy_message(int code) {
    switch (code) {
    case PW_OK:                    return "Password accepted";
    case PW_ERR_EMPTY:             return "Password cannot be empty";
    case PW_ERR_BADCHAR:           return "Password contains a control character";
    case PW_ERR_TOO_SHORT:         return "Password must be at least 8 characters";
    case PW_ERR_TOO_LONG:          return "Password is too long (127 characters maximum)";
    case PW_ERR_CONTAINS_USERNAME: return "Password must not contain your username";
    case PW_ERR_LOW_VARIETY:       return "Password uses too few different characters";
    case PW_ERR_SEQUENCE:          return "Password is a keyboard or counting sequence";
    case PW_ERR_BREACHED:          return "Password appears in a known breached-password list";
    case PW_ERR_SAME_AS_OTHER:     return "The root password must be different from your account password";
    default:                       return "Password rejected";
    }
}

#endif // PROC_PWPOLICY_H
