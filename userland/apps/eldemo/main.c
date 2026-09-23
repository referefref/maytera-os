// eldemo - a tiny libedit program that proves the MayteraOS libedit port (#745)
// gives real line editing and history through the Terminal's pty.
//
// It uses ONLY the native histedit API: el_init/el_gets/history. It sets a
// colour prompt via EL_PROMPT_ESC (so the ANSI codes do not corrupt libedit's
// cursor arithmetic), enables emacs-style keys (EL_EDITOR "emacs"), and wires a
// History ring so Up/Down recall previous lines. Type lines, press Up to recall
// them, use Left/Backspace/^A/^E/^W to edit, and "exit" or Ctrl-D to quit.
//
// It sets TERM=maytera itself: a pty-spawned child has no environ, and the
// ncurses termcap the library links against defaults an unset TERM to "maytera"
// anyway, but making it explicit keeps the demo self-contained.
#include <histedit.h>
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"

static const char *prompt_cb(EditLine *e) {
    (void)e;
    // \1 ... \1 brackets non-printing (colour) sequences for EL_PROMPT_ESC.
    return "\1\033[36m\1eldemo\1\033[0m\1> ";
}

int main(void) {
    setenv("TERM", "maytera-256color", 0);

    History *hist = history_init();
    HistEvent ev;
    history(hist, &ev, H_SETSIZE, 100);

    EditLine *el = el_init("eldemo", stdin, stdout, stderr);
    el_set(el, EL_PROMPT_ESC, prompt_cb, '\1');
    el_set(el, EL_EDITOR, "emacs");
    el_set(el, EL_HIST, history, hist);
    el_set(el, EL_SIGNAL, 1);

    printf("libedit %d.%d demo: type lines, Up/Down recall, Left/Backspace edit, 'exit' quits.\r\n",
           LIBEDIT_MAJOR, LIBEDIT_MINOR);

    int count;
    const char *line;
    while ((line = el_gets(el, &count)) != NULL && count > 0) {
        // Strip the trailing newline el_gets returns.
        char buf[1024];
        int n = 0;
        for (const char *p = line; *p && *p != '\n' && n < (int)sizeof(buf) - 1; p++)
            buf[n++] = *p;
        buf[n] = '\0';

        if (n == 0)
            continue;
        history(hist, &ev, H_ENTER, buf);

        if (strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0)
            break;

        printf("you typed: [%s] (%d chars)\r\n", buf, n);
    }

    printf("eldemo: bye\r\n");
    el_end(el);
    history_end(hist);
    return 0;
}
