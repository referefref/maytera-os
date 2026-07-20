// pwd - print working directory
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[256];
    if (!getcwd(buf, sizeof(buf))) { perror("pwd"); return 1; }
    long n = (long)strlen(buf);
    write(1, buf, n);
    write(1, "\n", 1);
    return 0;
}
