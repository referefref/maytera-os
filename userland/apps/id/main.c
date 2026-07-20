// id - Print user and group IDs for MayteraOS

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"
#include "../../libc/grp.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    uid_t euid = geteuid();
    gid_t gid = getgid();
    gid_t egid = getegid();

    struct passwd *pw = getpwuid(uid);
    struct group *gr = getgrgid(gid);

    // uid=1000(admin) gid=1000(users)
    printf("uid=%u", uid);
    if (pw) printf("(%s)", pw->pw_name);

    printf(" gid=%u", gid);
    if (gr) printf("(%s)", gr->gr_name);

    if (euid != uid) {
        struct passwd *epw = getpwuid(euid);
        printf(" euid=%u", euid);
        if (epw) printf("(%s)", epw->pw_name);
    }

    if (egid != gid) {
        struct group *egr = getgrgid(egid);
        printf(" egid=%u", egid);
        if (egr) printf("(%s)", egr->gr_name);
    }

    printf("\n");
    return 0;
}
