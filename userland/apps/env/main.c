// env - print the environment or run a command in a modified environment
// Usage: env [-i] [NAME=VALUE...] [COMMAND [ARG...]]
// With no arguments, prints default environment information.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "syscall.h"

int main(int argc, char **argv) {
    // If no arguments, print default environment variables
    if (argc <= 1) {
        // Since MayteraOS does not yet pass envp to processes,
        // print the known defaults.
        printf("PATH=/APPS\n");
        printf("SHELL=/APPS/MSH\n");

        // Get user info via syscall
        long uid = syscall0(120 /* SYS_GETUID */);
        if (uid == 0)
            printf("USER=root\n");
        else
            printf("USER=user\n");

        printf("HOME=/\n");
        printf("TERM=maytera\n");

        // Get current working directory
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)))
            printf("PWD=%s\n", cwd);
        else
            printf("PWD=/\n");

        return 0;
    }

    // With arguments: print NAME=VALUE pairs from argv
    // Future: support -i and running commands
    for (int i = 1; i < argc; i++) {
        // Check if it looks like NAME=VALUE
        int has_eq = 0;
        for (int j = 0; argv[i][j]; j++) {
            if (argv[i][j] == '=') { has_eq = 1; break; }
        }
        if (has_eq) {
            printf("%s\n", argv[i]);
        } else {
            // Not a VAR=VALUE: treat as a message or just print
            printf("%s\n", argv[i]);
        }
    }

    return 0;
}
