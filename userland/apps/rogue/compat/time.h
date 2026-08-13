
#ifndef COMPAT_TIME_H
#define COMPAT_TIME_H

/* MayteraOS port note: this file used to duplicate time_t/clock_t typedefs
 * and a static-inline time()/clock() here. The shared libc (libc/time.h)
 * now provides a complete, real time.h (time_t, struct tm, time(),
 * localtime(), gmtime(), etc, added under #422/#359) that rip.c's death()
 * screen needs (localtime() + struct tm). Duplicating the typedefs and
 * clock() here conflicted with the libc's own declarations ("static
 * declaration ... follows non-static declaration") and hid struct tm /
 * localtime() entirely, since -Icompat is searched before -I../../libc.
 * Forward to the real implementation instead of duplicating it. */
#include "../../../libc/time.h"

#endif
