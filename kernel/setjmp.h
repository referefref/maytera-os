// setjmp.h - Non-local jumps for MayteraOS kernel
#ifndef SETJMP_H
#define SETJMP_H

#include "types.h"

// jmp_buf stores callee-saved registers for x86_64
// rbx, rbp, r12, r13, r14, r15, rsp, rip (in this order)
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

// setjmp - Save the current execution context
// Returns 0 when called directly
// Returns val when returning via longjmp
int setjmp(jmp_buf env);

// longjmp - Restore a previously saved execution context
// val is the value that setjmp will appear to return (if 0, becomes 1)
__attribute__((noreturn))
void longjmp(jmp_buf env, int val);

#endif // SETJMP_H
