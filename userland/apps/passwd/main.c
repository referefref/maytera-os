// passwd - Change user password for MayteraOS
// Root can change any user's password; non-root can only change their own and
// must prove the current one.
//
// This tool stores NOTHING itself. It hands the three strings to
// SYS_PASSWD_CHANGE and the kernel does the work: sys_passwd_change()
// (kernel/proc/syscall.c) copies every argument out of Ring 3 exactly once
// (#745), authorizes the caller, verifies the old password through
// users_authenticate() (the ONE authenticator, rate limited with an escalating
// lockout, #566) and writes the new one through user_set_password(), which
// hashes with PBKDF2. The older comment here said "SHA-256 hashes in
// /CONFIG/SHADOW", which was wrong on both the algorithm and on who writes it.

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"
#include "../../libc/pwpolicy.h"

// SHA-256 implementation (minimal, for userland)
// We do the hash computation via a dedicated syscall or by reading/writing
// the shadow file directly. For simplicity, this tool writes a new shadow
// entry by delegating to a kernel syscall.

// SYS_PASSWD_CHANGE: custom syscall for changing passwords
// For now, we use a simpler approach: write directly to /CONFIG/SHADOW
// This requires root, or we add a setuid mechanism.

// Actually, the simplest approach for a hobby OS: use a dedicated syscall.
// We'll define SYS_PASSWD_CHANGE = 130

#define SYS_PASSWD_CHANGE 130

static int sys_passwd_change(const char *username, const char *old_pass, const char *new_pass) {
    return (int)syscall3(SYS_PASSWD_CHANGE, (long)username, (long)old_pass, (long)new_pass);
}

static int read_password(const char *prompt, char *buf, int max) {
    printf("%s", prompt);
    // Simple: read chars, don't echo (we just read from stdin)
    int pos = 0;
    while (pos < max - 1) {
        int c = sys_getchar();
        if (c < 0) {
            // No input available, yield
            yield();
            continue;
        }
        if (c == '\n' || c == '\r') break;
        if ((c == '\b' || c == 0x7f) && pos > 0) {
            // 0x7f (DEL) as well as 0x08: terminals disagree about which one
            // backspace sends, and because this prompt deliberately does not
            // echo, an unhandled DEL silently APPENDS a junk byte to the
            // password instead of deleting one. The user then sees a correct
            // password refused with no visible cause.
            pos--;
            continue;
        }
        if (c == 0x03) {          // Ctrl-C: abandon without changing anything
            buf[0] = '\0';
            printf("\n");
            return -1;
        }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    uid_t uid = getuid();
    uid_t euid = geteuid();
    const char *target_user = NULL;

    if (argc > 1) {
        target_user = argv[1];
        // Only root can change other users' passwords
        if (euid != 0) {
            struct passwd *pw = getpwuid(uid);
            if (!pw || strcmp(pw->pw_name, target_user) != 0) {
                printf("passwd: only root can change another user's password\n");
                return 1;
            }
        }
    } else {
        // Change own password
        struct passwd *pw = getpwuid(uid);
        if (!pw) {
            printf("passwd: cannot determine current user\n");
            return 1;
        }
        target_user = pw->pw_name;
    }

    printf("Changing password for %s\n", target_user);

    char old_pass[64] = {0};
    char new_pass[64] = {0};
    char confirm[64] = {0};

    // Non-root must enter old password
    if (euid != 0) {
        if (read_password("Current password: ", old_pass, sizeof(old_pass)) != 0) {
            printf("passwd: aborted, password unchanged\n");
            return 1;
        }
    }

    if (read_password("New password: ", new_pass, sizeof(new_pass)) != 0 ||
        read_password("Confirm new password: ", confirm, sizeof(confirm)) != 0) {
        printf("passwd: aborted, password unchanged\n");
        return 1;
    }

    if (strcmp(new_pass, confirm) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }

    if (strlen(new_pass) < 1) {
        printf("passwd: password cannot be empty\n");
        return 1;
    }

    // No local strength check on purpose: the kernel owns the policy and
    // enforces it whether this app asks or not. What this app does is DECODE
    // the answer, below.

    int ret = sys_passwd_change(target_user, old_pass, new_pass);
    if (ret == 0) {
        printf("passwd: password updated successfully\n");
    } else if (PW_RC_IS_POLICY(ret)) {
        // The kernel password policy refused it and said which rule. Printing
        // the rule is the whole reason the codes are distinguishable: a user
        // told only "failed" retypes the same password.
        printf("passwd: %s\n", pw_policy_message(PW_RC_CODE(ret)));
        printf("passwd: password unchanged\n");
        return 1;
    } else if (ret == -2) {
        // #566 escalating lockout. Reporting this as "incorrect" sends the
        // user off retyping a password that was already right.
        printf("passwd: account is locked out after repeated failures; wait and retry\n");
        return 1;
    } else {
        // Do not claim to know which of the two it was: the kernel returns -1
        // both for a wrong current password and for an unpermitted target, and
        // guessing here would be inventing detail we were not given.
        printf("passwd: password NOT changed (wrong current password, or not permitted)\n");
        return 1;
    }

    return 0;
}
