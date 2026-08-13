// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// libc/pwpolicy.h - USERLAND mirror of the kernel password policy codes.
//
// The policy itself runs in the kernel (rustkern/pwpolicy.rs, called from the
// one chokepoint in proc/users.c), so an app cannot bypass it by not consulting
// this header, and an app that DOES consult it can tell the user which rule
// they broke instead of "could not set password".
//
// Any syscall that sets a password (SYS_PASSWD_CHANGE, SYS_USER_CREATE_PW)
// returns PW_RC(code) for a policy rejection, which lands in -201..-208 and is
// distinct from the pre-existing -1 (generic/permission), -2 (locked out) and
// -14 (bad pointer). Decode it with PW_RC_CODE() and print pw_policy_message().
//
// KEEP IN SYNC with kernel/proc/pwpolicy.h and kernel/rustkern/pwpolicy.rs. The
// codes are a wire format between Ring 0 and Ring 3; they are not free to
// renumber.
#ifndef LIBC_PWPOLICY_H
#define LIBC_PWPOLICY_H

#define PW_OK                     0
#define PW_ERR_EMPTY              1
#define PW_ERR_BADCHAR            2
#define PW_ERR_TOO_SHORT          3
#define PW_ERR_TOO_LONG           4
#define PW_ERR_CONTAINS_USERNAME  5
#define PW_ERR_LOW_VARIETY        6
#define PW_ERR_SEQUENCE           7
#define PW_ERR_BREACHED           8
// #745: produced by the first-boot pair check (proc/users.c), never by the
// single-password policy. See kernel/proc/pwpolicy.h for why it exists.
#define PW_ERR_SAME_AS_OTHER      9
#define PW_ERR_LAST               9

#define PW_RC_BASE 200
#define PW_RC(code) (-(PW_RC_BASE + (int)(code)))
#define PW_RC_IS_POLICY(rc) \
    ((rc) <= -(PW_RC_BASE + PW_ERR_EMPTY) && (rc) >= -(PW_RC_BASE + PW_ERR_LAST))
#define PW_RC_CODE(rc) (PW_RC_IS_POLICY(rc) ? (-(int)(rc) - PW_RC_BASE) : PW_OK)

// #745: SYS_FIRSTBOOT_ADMIN validates TWO passwords and reports which one was
// refused: the account password in the -201..-209 band above, root's in this
// one. Decode with PW_RC_ROOT_CODE() and print pw_policy_message().
#define PW_RC_ROOT_BASE 220
#define PW_RC_ROOT(code) (-(PW_RC_ROOT_BASE + (int)(code)))
#define PW_RC_ROOT_IS_POLICY(rc) \
    ((rc) <= -(PW_RC_ROOT_BASE + PW_ERR_EMPTY) && (rc) >= -(PW_RC_ROOT_BASE + PW_ERR_LAST))
#define PW_RC_ROOT_CODE(rc) \
    (PW_RC_ROOT_IS_POLICY(rc) ? (-(int)(rc) - PW_RC_ROOT_BASE) : PW_OK)

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

#endif // LIBC_PWPOLICY_H
