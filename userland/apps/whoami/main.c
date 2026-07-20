// whoami - Print current username for MayteraOS

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uid_t uid = geteuid();
    struct passwd *pw = getpwuid(uid);

    if (pw) {
        printf("%s\n", pw->pw_name);
    } else {
        printf("uid %u\n", uid);
    }

    return 0;
}
