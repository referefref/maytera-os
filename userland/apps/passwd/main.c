// passwd - Change user password for MayteraOS
// Root can change any user's password; non-root can only change their own.
// Passwords are stored as SHA-256 hashes in /CONFIG/SHADOW.

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"

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

static void read_password(const char *prompt, char *buf, int max) {
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
        if (c == '\b' && pos > 0) {
            pos--;
            continue;
        }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    printf("\n");
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
        read_password("Current password: ", old_pass, sizeof(old_pass));
    }

    read_password("New password: ", new_pass, sizeof(new_pass));
    read_password("Confirm new password: ", confirm, sizeof(confirm));

    if (strcmp(new_pass, confirm) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }

    if (strlen(new_pass) < 1) {
        printf("passwd: password cannot be empty\n");
        return 1;
    }

    int ret = sys_passwd_change(target_user, old_pass, new_pass);
    if (ret == 0) {
        printf("passwd: password updated successfully\n");
    } else {
        printf("passwd: failed to update password\n");
        return 1;
    }

    return 0;
}
