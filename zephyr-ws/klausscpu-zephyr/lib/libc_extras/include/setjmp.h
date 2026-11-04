/* setjmp.h — setjmp/longjmp for KlaussCPU (implementation: src/setjmp.S).
 *
 * jmp_buf holds R4-R7 (callee-saved), R15 (frame pointer), the caller's SP
 * and the return address: 7 x 8 bytes.  That is the backend's whole
 * callee-saved set (CSR_KlaussCPU_Call), so a jmp_buf also captures every
 * register a garbage collector must scan (MicroPython's
 * MICROPY_GCREGS_SETJMP).
 */
#ifndef KLAUSSCPU_SETJMP_H
#define KLAUSSCPU_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long long jmp_buf[7];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#endif /* KLAUSSCPU_SETJMP_H */
