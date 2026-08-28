// term_shell.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_SHELL_H
#define TERM_SHELL_H

#include "term_common.h"

// The built-in shell: command dispatch, builtins, $PATH resolution, execution
// (native ELF / DOS / Win16 / pty), pipelines, redirection and history.

// Input handling
#define TERM_SHELL_INPUT_MAX 256
#define TERM_SHELL_HISTORY_SIZE 32
extern char input_line[TERM_SHELL_INPUT_MAX];
extern int  input_pos;
extern char history[TERM_SHELL_HISTORY_SIZE][TERM_SHELL_INPUT_MAX];
extern int  history_count;
extern int  history_pos;

// Current working directory (the shell's own, kept in sync with SYS_CHDIR).
extern char cwd[256];

void print_prompt(void);
void execute_command(const char *cmd);
void add_to_history(const char *cmd);
void setenv_local(const char *name, const char *value);
const char *getenv_local(const char *name);

#endif // TERM_SHELL_H
