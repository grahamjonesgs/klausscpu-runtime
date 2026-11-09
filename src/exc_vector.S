// exc_vector.S — the exception entry stub, user-mode entry and return
// trampoline (MMU stage 1, see exc.h).
//
// The new instructions are .long words until klausscpu-llvm's MC layer has
// them (encodings in exc.h).

    .text

// exc_vector: EVEC points here.  The hardware has already switched to the
// kernel SP (if the exception came from user mode), pushed the 64-bit frame
// and set DI.  Save R15..R0 so that SP points at struct exc_regs
// { r[0..15], frame }, read ECAUSE/EADDR before anything can fault again,
// and call exc_dispatch(cause, eaddr, regs).  The handler may edit the saved
// registers and the frame; they are restored and IRET resumes.
    .p2align 3
    .globl  exc_vector
    .type   exc_vector, @function
exc_vector:
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
    .long   0x6C402000          // MFSR r0, ecause
    .long   0x6C403100          // MFSR r1, eaddr
    getsp   r2                  // r2 = &struct exc_regs
    call    exc_dispatch
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
    .size   exc_vector, . - exc_vector

// exc_enter_user(frame, arg, usp): r0 = a frame with PM = 1 and PC = the
// user entry point, r1 = the entry point's argument, r2 = the user SP.
// IRET pops the frame from this (kernel) stack, switches to user mode and
// to the user SP; the kernel SP left in SP_ALT is this SP, just above the
// popped frame, so the next user->kernel entry pushes below our caller.
    .p2align 3
    .globl  exc_enter_user
    .type   exc_enter_user, @function
exc_enter_user:
    .long   0x6C604020          // MTSR usp, r2
    push    r0
    copy    r0, r1
    iret
    .size   exc_enter_user, . - exc_enter_user

// The user entry point returns here (exc_run_user put this address on the
// user stack): exit with its return value.
    .p2align 3
    .globl  exc_user_return
    .type   exc_user_return, @function
exc_user_return:
    copy    r1, r12
    setr    r0, 0x7FFF0000      // SYS_EXIT_USER
    .long   0x6C080000          // SYSCALL
    jmp     exc_user_return     // not reached
    .size   exc_user_return, . - exc_user_return
