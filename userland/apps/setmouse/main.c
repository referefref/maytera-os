// setmouse - tiny test helper: move the OS cursor to absolute (x,y).
// Used to verify the AI Chat dock peek/retract headlessly.
#include "syscall.h"
#include "stdlib.h"

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    set_mouse_pos(x, y);
    return 0;
}
