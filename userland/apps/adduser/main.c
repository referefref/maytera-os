// adduser - Create a new user account for MayteraOS (root only)

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"

// SYS_ADDUSER: kernel-assisted user creation
#define SYS_ADDUSER 132

// adduser syscall: username, uid, gid, home, shell
static int sys_adduser(const char *username, unsigned int uid, unsigned int gid,
                       const char *home, const char *shell) {
    return (int)syscall5(SYS_ADDUSER, (long)username, uid, gid, (long)home, (long)shell);
}

static void read_input(const char *prompt, char *buf, int max) {
    printf("%s", prompt);
    int pos = 0;
    while (pos < max - 1) {
        int c = sys_getchar();
        if (c < 0) { yield(); continue; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b' && pos > 0) { pos--; printf("\b \b"); continue; }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
            sys_putchar(c);
        }
    }
    buf[pos] = '\0';
    printf("\n");
}

int main(int argc, char **argv) {
    // Only root can add users
    if (geteuid() != 0) {
        printf("adduser: operation not permitted (must be root)\n");
        return 1;
    }

    char username[32] = {0};
    char home[64] = {0};

    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
    } else {
        read_input("Username: ", username, sizeof(username));
    }

    if (!username[0]) {
        printf("adduser: username cannot be empty\n");
        return 1;
    }

    // Check if user already exists
    if (getpwnam(username)) {
        printf("adduser: user '%s' already exists\n", username);
        return 1;
    }

    // Auto-assign UID (find next available >= 1000)
    unsigned int uid = 1000;
    while (getpwuid(uid) != NULL && uid < 65534) {
        uid++;
    }

    // Default home directory
    snprintf(home, sizeof(home), "/HOME/%s", username);
    // Uppercase the username part for FAT
    for (int i = 6; home[i]; i++) {
        if (home[i] >= 'a' && home[i] <= 'z') home[i] -= 32;
    }

    printf("Creating user '%s' (uid=%u, gid=%u)\n", username, uid, uid);
    printf("Home directory: %s\n", home);

    int ret = sys_adduser(username, uid, uid, home, "/APPS/MSH");
    if (ret != 0) {
        printf("adduser: failed to create user\n");
        return 1;
    }

    // Set initial password
    char password[64] = {0};
    char confirm[64] = {0};
    read_input("Password: ", password, sizeof(password));
    read_input("Confirm password: ", confirm, sizeof(confirm));

    if (strcmp(password, confirm) != 0) {
        printf("adduser: passwords do not match\n");
        // User is created but has no password (login disabled)
        return 1;
    }

    // Set password via passwd_change syscall
    #define SYS_PASSWD_CHANGE 130
    syscall3(SYS_PASSWD_CHANGE, (long)username, (long)"", (long)password);

    printf("User '%s' created successfully\n", username);
    return 0;
}
