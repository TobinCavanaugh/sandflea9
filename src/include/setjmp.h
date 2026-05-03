#ifndef SANDFLEA_SETJMP_H
#define SANDFLEA_SETJMP_H

typedef struct {
    unsigned long rbx;
    unsigned long rbp;
    unsigned long r12;
    unsigned long r13;
    unsigned long r14;
    unsigned long r15;
    unsigned long rsp;
    unsigned long rip;
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
