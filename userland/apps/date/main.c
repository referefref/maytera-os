// date - Display current date/time for MayteraOS
// Uses uptime since no RTC calendar is available yet

#include "../../libc/maytera.h"

// time() takes a (long*) and writes the result there if non-NULL; calling it
// argless passed a garbage pointer and faulted. Declare it and pass NULL.
extern long time(long *t);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    long uptime = time(0);
    long hours = uptime / 3600;
    long mins = (uptime % 3600) / 60;
    long secs = uptime % 60;

    printf("Uptime: %ld seconds (%ld:%02ld:%02ld)\n", uptime, hours, mins, secs);

    return 0;
}
