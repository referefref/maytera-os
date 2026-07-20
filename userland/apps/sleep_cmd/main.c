// sleep - Pause execution for a specified number of seconds

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sleep <seconds>\n");
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds <= 0) {
        printf("sleep: invalid time interval '%s'\n", argv[1]);
        return 1;
    }

    sleep((unsigned int)seconds * 1000);

    return 0;
}
