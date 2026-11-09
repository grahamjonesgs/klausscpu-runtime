// klausscpu: trap entry and exit (replaces kernelvec.S and trampoline.S).
//
// excvec (EVEC), timervec (INT_VEC[timer]) and tlbrefill (TLBVEC) are in
// kseg, as MMU_PLAN §11.3 requires.  The hardware has already pushed the
// 64-bit frame (on the process's kernel stack if the trap came from user
// mode) and, for exceptions, disabled interrupts.  The stubs push R15..R0
// and the user SP, which makes a struct trapframe (proc.h) at SP, and call
//   void trap(struct trapframe *tf, uint64 kind)   kind 0 exception, 1 timer
// trapret(tf) pops the trapframe and IRETs; usertrapret() calls it with
// the process's trapframe (a new process starts that way).

    .macro  SAVE
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    r7
    push    r6
    push    r5
    push    r4
    push    r3
    push    r2
    push    r1
    push    r0
    mfsr    r0, usp
    push    r0
    .endm

    .text
    .globl  excvec
    .p2align 3
excvec:
    SAVE
    getsp   r0
    setr    r1, 0
    call    trap
    getsp   r0
    jmp     trapret

    .globl  timervec
    .p2align 3
timervec:
    SAVE
    getsp   r0
    setr    r1, 1
    call    trap
    getsp   r0
    // fall through

    // void trapret(struct trapframe *tf): never returns.
    .globl  trapret
trapret:
    setsp   r0
    pop     r0
    mtsr    usp, r0
    pop     r0
    pop     r1
    pop     r2
    pop     r3
    pop     r4
    pop     r5
    pop     r6
    pop     r7
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15
    iret

// TLB miss: tlbrefill_c() (trap.c) loads the entry from the current page
// table and returns 1, or returns 0 if the page isn't mapped for the user;
// then the miss goes on to excvec as an exception (ECAUSE is still the
// TLB_MISS).  Saves only the caller-saved registers; the flags are restored
// by IRET from the frame.  POP leaves the flags alone, so the compare
// before the POPs decides.
    .globl  tlbrefill
    .p2align 3
tlbrefill:
    push    r0
    push    r1
    push    r2
    push    r3
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    call    tlbrefill_c
    cmprv   r12, 0
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     r3
    pop     r2
    pop     r1
    pop     r0
    jmpe    excvec
    iret
