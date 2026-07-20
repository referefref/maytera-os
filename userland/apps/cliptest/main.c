// cliptest - #542 system-wide clipboard round-trip test (CLI, two-process).
//
//   cliptest set <text...>   clipboard_set_text(joined args), prints "SET <n>"
//   cliptest get             clipboard_get_text, prints "GET <text>"
//   cliptest len             prints "LEN <n>"
//
// The clipboard lives in the KERNEL (SYS_CLIP_*), with no per-process state, so
// running `cliptest set FOO` in one process and then `cliptest get` in a SECOND,
// separate process must print FOO: proof that a copy in app A pastes in app B.
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "stdio.h"
#include "syscall.h"

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "set") == 0) {
        char buf[512];
        int n = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && n < 511) buf[n++] = ' ';
            for (int j = 0; argv[i][j] && n < 511; j++) buf[n++] = argv[i][j];
        }
        buf[n] = 0;
        int st = clipboard_set_text(buf);
        printf("SET %d\n", st);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        char buf[512];
        (void)clipboard_get_text(buf, (int)sizeof(buf));
        printf("GET %s\n", buf);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "len") == 0) {
        printf("LEN %d\n", clipboard_len());
        return 0;
    }
    printf("usage: cliptest set <text> | get | len\n");
    return 1;
}
