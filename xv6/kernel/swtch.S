// klausscpu: context switch (KlaussCPU ABI: callee-saved R4-R7 and R15).
//
//   void swtch(struct context *old, struct context *new);
//
// Saves SP and the callee-saved registers in *old (r0) and loads them from
// *new (r1).  The return address is on the stack, so the RET returns into
// whatever called swtch on the new stack (or, for a new process, to the
// forkret address allocproc() put there).

    .text
    .globl  swtch
    .p2align 3
swtch:
    getsp   r2
    stidx64 r2, r0, 0
    stidx64 r4, r0, 8
    stidx64 r5, r0, 16
    stidx64 r6, r0, 24
    stidx64 r7, r0, 32
    stidx64 r15, r0, 40
    ldidx64 r2, r1, 0
    setsp   r2
    ldidx64 r4, r1, 8
    ldidx64 r5, r1, 16
    ldidx64 r6, r1, 24
    ldidx64 r7, r1, 32
    ldidx64 r15, r1, 40
    ret
