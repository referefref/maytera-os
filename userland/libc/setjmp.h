// setjmp.h - Non-local jumps for exception handling
#ifndef SETJMP_H
#define SETJMP_H

#include "types.h"

// jmp_buf structure for x86_64
// Must save: RBX, RBP, R12-R15 (callee-saved), RSP, RIP
typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
} jmp_buf[1];

// Save current execution context
// Returns 0 on first call, non-zero when returning via longjmp
int setjmp(jmp_buf env);

// Restore execution context
// Never returns - jumps to corresponding setjmp
// val becomes setjmp's return value (if val==0, setjmp returns 1)
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif // SETJMP_H
