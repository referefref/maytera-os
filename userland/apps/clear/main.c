// clear - clear the terminal screen
#include "unistd.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // ESC [ 2 J : erase entire screen
    // ESC [ H  : move cursor to home (1,1)
    const char seq[] = "\x1b[2J\x1b[H";
    write(1, seq, sizeof(seq) - 1);
    return 0;
}
