// crashapp - #279 SMP test: deliberately triggers a user-mode page fault (NULL
// deref) so we can confirm a faulting user process no longer wedges the SMP
// desktop (the crash modal used to block while holding the whole-kernel BKL).
#include "../../libc/maytera.h"
int main(void) {
    volatile int *p = (volatile int *)0;
    return *p;   // page fault -> user-mode crash path -> proc should be killed, system stays live
}
