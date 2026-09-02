// term_util.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_util.h"
#include "term_shell.h"   // [tabcomp]: input_line/input_pos/print_prompt for
                           // term_begin_output()/term_end_output() below.
#include "term_parse.h"   // term_putc()

// String helper functions

void str_copy(char *dest, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int str_starts(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}


void int_to_str(int num, char *buf) {
    char tmp[20];
    int i = 0;
    int neg = 0;
    
    if (num < 0) {
        neg = 1;
        num = -num;
    }
    
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// [tabcomp] Promoted verbatim from term_menu.c's static tm_begin_output()/
// tm_end_output() (see term_util.h). Behaviour unchanged: erase the
// half-typed input echo back to column 0 with the same backspace idiom every
// other editing path in this file uses, or reprint the prompt plus whatever
// the user had typed.
void term_begin_output(void) {
    for (int i = 0; i < input_pos; i++) { term_putc('\b'); term_putc(' '); term_putc('\b'); }
    term_puts("\r\n");
}
void term_end_output(void) {
    print_prompt();
    for (int i = 0; i < input_pos; i++) term_putc(input_line[i]);
}
