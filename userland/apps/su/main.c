// su - Switch user for MayteraOS
// Prompts for target user's password, then setuid and exec shell

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"

// SYS_SU: kernel-assisted switch user (verify password + setuid in one call)
#define SYS_SU 131

static int sys_su(const char *username, const char *password) {
    return (int)syscall2(SYS_SU, (long)username, (long)password);
}

static void read_password(const char *prompt, char *buf, int max) {
    printf("%s", prompt);
    int pos = 0;
    while (pos < max - 1) {
        int c = sys_getchar();
        if (c < 0) { yield(); continue; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b' && pos > 0) { pos--; continue; }
        if (c >= ' ' && c < 127) buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    printf("\n");
}

int main(int argc, char **argv) {
    const char *target = "root";

    if (argc > 1) {
        if (strcmp(argv[1], "-") == 0 && argc > 2) {
            target = argv[2];
        } else {
            target = argv[1];
        }
    }

    // Look up target user
    struct passwd *pw = getpwnam(target);
    if (!pw) {
        printf("su: unknown user '%s'\n", target);
        return 1;
    }

    // Root doesn't need a password
    uid_t euid = geteuid();
    if (euid != 0) {
        char password[64];
        printf("Password for %s: ", target);
        read_password("", password, sizeof(password));

        int ret = sys_su(target, password);
        // Clear password from memory
        memset(password, 0, sizeof(password));

        if (ret != 0) {
            printf("su: authentication failed\n");
            return 1;
        }
    } else {
        // Root can su without password, just setuid
        if (setuid(pw->pw_uid) != 0) {
            printf("su: setuid failed\n");
            return 1;
        }
        if (setgid(pw->pw_gid) != 0) {
            printf("su: setgid failed\n");
            return 1;
        }
    }

    printf("Switched to %s (uid=%u)\n", target, pw->pw_uid);

    // Exec the user's shell
    if (pw->pw_shell && pw->pw_shell[0]) {
        sys_exec(pw->pw_shell);
    }

    // If exec fails, just return
    return 0;
}
