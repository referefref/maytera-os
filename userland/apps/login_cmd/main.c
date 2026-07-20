// login - TTY login program for MayteraOS
// Prompts for username and password, then sets UID and execs user shell

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"

// SYS_SU: kernel-assisted user switch (verify + setuid)
#define SYS_SU 131

static int sys_su(const char *username, const char *password) {
    return (int)syscall2(SYS_SU, (long)username, (long)password);
}

static void read_line(const char *prompt, char *buf, int max, int echo) {
    printf("%s", prompt);
    int pos = 0;
    while (pos < max - 1) {
        int c = sys_getchar();
        if (c < 0) { yield(); continue; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b' && pos > 0) {
            pos--;
            if (echo) printf("\b \b");
            continue;
        }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
            if (echo) sys_putchar(c);
        }
    }
    buf[pos] = '\0';
    printf("\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("\nMayteraOS login\n\n");

    int max_attempts = 3;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        char username[32] = {0};
        char password[64] = {0};

        read_line("login: ", username, sizeof(username), 1);
        if (!username[0]) continue;

        read_line("password: ", password, sizeof(password), 0);

        // Attempt authentication
        int ret = sys_su(username, password);
        // Clear password from memory
        memset(password, 0, sizeof(password));

        if (ret == 0) {
            // Login successful
            struct passwd *pw = getpwnam(username);
            if (pw) {
                printf("\nWelcome, %s\n\n", pw->pw_gecos[0] ? pw->pw_gecos : pw->pw_name);

                // Change to home directory
                if (pw->pw_dir && pw->pw_dir[0]) {
                    chdir(pw->pw_dir);
                }

                // Exec user's shell
                if (pw->pw_shell && pw->pw_shell[0]) {
                    sys_exec(pw->pw_shell);
                }

                // Fallback
                sys_exec("/APPS/MSH");
            }

            // If exec fails
            printf("login: cannot start shell\n");
            return 1;
        }

        printf("Login incorrect\n\n");

        // Brief delay after failed attempt
        sys_sleep(1000);
    }

    printf("Too many login attempts\n");
    return 1;
}
