// exc_vector.S — the exception entry stub, user-mode entry and return
// trampoline (MMU stage 1, see exc.h).
//
// The new instructions use klausscpu-llvm's `mmu` mnemonics (exc.h).

    .text

// exc_vector: EVEC points here.  The hardware has already switched to the
// kernel SP (if the exception came from user mode), pushed the 64-bit frame
// and set DI.
//
// MMU_PLAN §10.2: no memory access until SP is known safe.  An MPU GUARD
// denial (ECAUSE[8]) may have left SP, and the frame, inside a guard
// region, where a PUSH faults again (and each nested entry pushes another
// unchecked frame lower).  So the first instructions only use SCRATCH0/1
// and ECAUSE:
//   - no GUARD bit: restore r0 and take the normal path on this stack;
//   - GUARD: clear STATUS.MPU_EN (the guarded frame becomes readable), move
//     to exc_emergency_top, copy the frame there, and run the same body.
//     Afterwards the (possibly edited) frame is written back over the
//     original and SP returns to it for the IRET.  The MPU stays off; the
//     handler re-enables it (exc_mpu_enable) if it fixed the cause.
//
// The body saves R15..R0 so that SP points at struct exc_regs
// { r[0..15], frame }, reads ECAUSE/EADDR, and calls
// exc_dispatch(ecause, eaddr, regs).  The handler may edit the saved
// registers and the frame; they are restored and IRET resumes.

    .macro EXC_BODY
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
    mfsr r0, ecause
    mfsr r1, eaddr
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
    .endm

    .p2align 3
    .globl  exc_vector
    .type   exc_vector, @function
exc_vector:
    mtsr scratch0, r0
    mfsr r0, ecause
    andv    r0, r0, 256         // ECAUSE[8] GUARD (flags are in the frame)
    fbri    r0, 0, 0, .Lexc_normal
    // GUARD denial: SP may be inside a guard region.
    mtsr scratch1, r1
    mfsr r0, status
    bclr    r0, r0, 8           // MPU_EN = 0
    mtsr status, r0             // serializes
    getsp   r1                  // r1 = the SP holding the frame
    setr    r0, exc_emergency_top
    setsp   r0
    push    r1                  // [old SP]
    ldidx64 r0, r1, 0
    push    r0                  // [frame copy] = exc_regs.frame
    mfsr r1, scratch1
    mfsr r0, scratch0
    EXC_BODY
    mtsr scratch0, r0
    mtsr scratch1, r1
    pop     r0                  // the frame, as the handler left it
    pop     r1                  // old SP
    stidx64 r0, r1, 0           // write it back where IRET pops it
    setsp   r1
    mfsr r0, scratch0
    mfsr r1, scratch1
    iret
.Lexc_normal:
    mfsr r0, scratch0
    EXC_BODY
    iret
    .size   exc_vector, . - exc_vector

// The stack exc_vector moves to on a GUARD denial.
    .bss
    .p2align 4
    .globl  exc_emergency_stack
exc_emergency_stack:
    .space  4096
    .globl  exc_emergency_top
exc_emergency_top:
    .text

// exc_enter_user(frame, arg, usp): r0 = a frame with PM = 1 and PC = the
// user entry point, r1 = the entry point's argument, r2 = the user SP.
// IRET pops the frame from this (kernel) stack, switches to user mode and
// to the user SP; the kernel SP left in SP_ALT is this SP, just above the
// popped frame, so the next user->kernel entry pushes below our caller.
    .p2align 3
    .globl  exc_enter_user
    .type   exc_enter_user, @function
exc_enter_user:
    mtsr usp, r2
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
    syscall
    jmp     exc_user_return     // not reached
    .size   exc_user_return, . - exc_user_return
