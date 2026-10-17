// PKGRDRH - the child half of PKGRDR's shell-redirection arms.
//
// All it does is print a fixed 16-byte tag to stdout and exit 0. PKGRDR spawns
// it through SYS_SPAWN_REDIR with stdout redirected to a file, which is the
// Ring-3 route into open_redir_file(), the gate under test.
//
// WHY A DEDICATED HELPER AND NOT /APPS/HELLO. The ALLOW arm has to prove the
// redirect really WROTE, and the only honest way to do that is a byte
// comparison against output this prover controls. Borrowing a shipping app
// would make the arm depend on that app's output text, which nothing stops
// from changing, and a changed banner would read as a kernel regression.
//
// NO TRAILING NEWLINE and no printf: write(1, ...) once, so the expected bytes
// are exactly the 16 below and the arm cannot be confused by stdio buffering
// or by a line ending.
#include "../../libc/syscall.h"

// Must match REDIR_TAG / REDIR_TAGLEN in userland/apps/PKGRDR/main.c.
static const char TAG[16] = { 'P','K','G','R','D','R','H','-',
                              'R','E','D','I','R','-','O','K' };

int main(void) {
    long n = syscall3(SYS_WRITE, 1, (long)TAG, (long)sizeof(TAG));
    return (n == (long)sizeof(TAG)) ? 0 : 1;
}
