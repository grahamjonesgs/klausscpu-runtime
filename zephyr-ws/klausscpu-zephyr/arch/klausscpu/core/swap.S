/*
 * swap.S — KlaussCPU Zephyr context switch and timer ISR.
 *
 * arch_switch(void *switch_to, void **switched_from)
 * ─────────────────────────────────────────────────
 *   R0 = switch_to      : new thread's saved SP (void*)
 *   R1 = switched_from  : &current_thread->switch_handle (void**)
 *
 * Saves full R0-R15 frame + patches CALL return slot into an IRET frame
 * (same trick as FreeRTOS vPortYield), stores SP → *switched_from, loads
 * SP from switch_to, restores R0-R15, IRETs into the new thread.
 *
 * z_klausscpu_timer_isr, z_klausscpu_isr1..3
 * ──────────────────────────────────────────
 * INT_VEC0 (timer) and INT_VEC1..3 (blitter, Ethernet, VGA vsync) while the
 * source is enabled (irq.c).  Hardware clears the dispatched source's INT_MASK
 * bit on entry.  Each stub saves context, executes DI, switches to the ISR
 * stack (z_interrupt_stacks), calls sys_clock_isr() (timer) or
 * z_klausscpu_irq_dispatch(n) (the others), restores context (possibly a
 * different thread), IRETs.
 *
 * Per-task stack frame layout (low → high address after portSAVE_CONTEXT):
 *
 *   [SP+  0] R15
 *   [SP+  8] R14
 *   ...
 *   [SP+112] R1
 *   [SP+120] R0
 *   [SP+128] IRET frame (8 bytes): flags/INT_MASK/PC
 */

#include <zephyr/offsets.h>

#define INT_MASK_ADDR   0xF00F0000

/* The IRET frame's upper word holds the 4-bit INT_MASK at bits [10:7] (bits
   [42:39] of the 64-bit frame).  The port keeps INT_MASK at 0xF (irq.c), so
   every frame it builds carries 0xF. */
#define FRAME_MASK_ALL  0x780

    .text
    /* Call frame information goes to .debug_frame (not loaded), like the C
       code's, so debuggers can unwind through the entry points below. */
    .cfi_sections .debug_frame

/* ── Unwind annotations ─────────────────────────────────────────────────── */
/*
 * arch_switch and the interrupt stubs are entered with one 64-bit word
 * pushed (the CALL slot / the IRET frame), so the default rule CFA = SP+8,
 * PC = low 32 bits of [CFA-8] holds at their first instruction.  The 16
 * register pushes put Rn at CFA-16-8n.  The interrupt stubs copy the task's
 * saved SP into callee-saved R4 (its own value is already on the task
 * stack) before switching to the ISR stack, so CFA = R4+136 while
 * the handler runs.  Whenever SP is moved to a saved task frame the
 * whole frame is described again (cfi_task_frame).
 */
.macro push_cfi reg, n
    push    \reg
    .cfi_adjust_cfa_offset 8
    .cfi_offset \reg, -16-8*\n
.endm

.macro pop_cfi reg
    pop     \reg
    .cfi_adjust_cfa_offset -8
    .cfi_same_value \reg
.endm

/* SP points at a saved task frame: R15 at SP+0 .. R0 at SP+120, then the
   IRET frame. */
.macro cfi_task_frame
    .cfi_def_cfa sp, 136
    .cfi_offset r0, -16
    .cfi_offset r1, -24
    .cfi_offset r2, -32
    .cfi_offset r3, -40
    .cfi_offset r4, -48
    .cfi_offset r5, -56
    .cfi_offset r6, -64
    .cfi_offset r7, -72
    .cfi_offset r8, -80
    .cfi_offset r9, -88
    .cfi_offset r10, -96
    .cfi_offset r11, -104
    .cfi_offset r12, -112
    .cfi_offset r13, -120
    .cfi_offset r14, -128
    .cfi_offset r15, -136
.endm

/* ── portSAVE_CONTEXT ───────────────────────────────────────────────────── */
/* Saves R0-R15 onto the current stack.  Used by the timer ISR path.         */
.macro portSAVE_CONTEXT
    push_cfi r0, 0
    push_cfi r1, 1
    push_cfi r2, 2
    push_cfi r3, 3
    push_cfi r4, 4
    push_cfi r5, 5
    push_cfi r6, 6
    push_cfi r7, 7
    push_cfi r8, 8
    push_cfi r9, 9
    push_cfi r10, 10
    push_cfi r11, 11
    push_cfi r12, 12
    push_cfi r13, 13
    push_cfi r14, 14
    push_cfi r15, 15
.endm

/* ── portRESTORE_CONTEXT ────────────────────────────────────────────────── */
/* Restores R15-R0 from the current stack (caller issues IRET after).  SP
   must already point at the saved task frame.                               */
.macro portRESTORE_CONTEXT
    cfi_task_frame
    pop_cfi r15
    pop_cfi r14
    pop_cfi r13
    pop_cfi r12
    pop_cfi r11
    pop_cfi r10
    pop_cfi r9
    pop_cfi r8
    pop_cfi r7
    pop_cfi r6
    pop_cfi r5
    pop_cfi r4
    pop_cfi r3
    pop_cfi r2
    pop_cfi r1
    pop_cfi r0
.endm


/* ── arch_switch ─────────────────────────────────────────────────────────── */
/*
 * Called as a normal C function (CALL pre-decrements SP by 8 and writes the
 * return address).  We convert that CALL slot into a valid IRET frame by
 * patching [SP+132] = FRAME_MASK_ALL (INT_MASK = 0xF in the upper 32-bit word).
 *
 * After pushing R0-R15 (128 bytes), stack looks like:
 *   [SP+  0] R15 slot     ← new SP stored to *switched_from
 *   ...
 *   [SP+120] R0  slot
 *   [SP+128] return PC  (lower 32 bits — already written by CALL)
 *   [SP+132] 0x780      ← we patch this: INT_MASK=0xF, flags=0, interrupts enabled
 */
/*
 * __start — Zephyr kernel entry point.
 *
 * On reset, SP is already valid (set by hardware or boot stub).
 * We clear BSS, copy .data from ROM, then call z_cstart().
 *
 * Placed in .text._start so the linker script puts it at address 0x20
 * (the first instruction the hardware executes after loading the ELF).
 */
    .section .text._start, "ax"
    .globl  __start
    .type   __start, @function
__start:
    /* Disable all interrupts immediately: Zephyr's boot code runs locked
       (arch_irq_lock() returns 0 until the first thread starts).  On a soft
       reset (FPGA reprogrammed without a full power cycle) INT_MASK may
       already be 1 from the previous run.  If a timer IRQ fires before
       the ISR stack is set up the ISR does setsp(0) → crash.
       DI blocks every source; INT_MASK = 0 as well drops the previous run's
       sources.  klausscpu_irq_init() (PRE_KERNEL_1) clears every INT_VEC and
       sets INT_MASK = 0xF; a source is enabled by writing its INT_VEC.  The
       first IRET (arch_switch into the main thread) enables interrupts again
       (frame bit 43 = 0) and restores INT_MASK from its frame (0xF). */
    di      r0
    setr    r0, INT_MASK_ADDR
    setr    r1, 0
    memset32 r1, r0

    /* Stack is set up by hardware reset (SP = end of RAM). */

    /* Startup delay (~150 ms at 100 MHz): the serial loader prints
       "Load Complete OK\r\n" on the same UART we are about to drive, and
       its TX FIFO may not have fully drained when control transfers to us.
       Spin so the loader's trailing bytes don't collide with Zephyr's
       first prints on the host terminal. */
    setr    r0, 0
    setr    r1, 5000000
.Lstartup_delay:
    addi    r0, r0, 1
    cmprr   r0, r1
    jmpne   .Lstartup_delay

    /* Clear BSS: from __bss_start to __bss_end. */
    setr    r0, __bss_start
    setr    r1, __bss_end
    setr    r2, 0
.Lbss_loop:
    cmprr   r0, r1
    jmpuge  .Lbss_done
    stidx64 r2, r0, 0
    addi    r0, r0, 8
    jmp     .Lbss_loop
.Lbss_done:

    /*
     * Copy .data from LMA (in ROM segment) to VMA (in RAM).
     *
     * With CONFIG_XIP=n (the default for this port) the linker places LMA == VMA,
     * so the src==dst check below short-circuits this loop to a no-op.  It only
     * does real work if a future build re-enables XIP — and in that case klausscc
     * must also be taught to handle PT_LOAD segments with VirtAddr != PhysAddr.
     */
    setr    r0, __data_start
    setr    r1, __data_end
    setr    r2, __data_load_start
    /* skip if src == dst (XIP or same-region build) */
    cmprr   r0, r2
    jmpe    .Ldata_done
    /* also skip if section is empty */
    cmprr   r0, r1
    jmpuge  .Ldata_done
.Ldata_loop:
    cmprr   r0, r1
    jmpuge  .Ldata_done
    ldidx64 r3, r2, 0
    stidx64 r3, r0, 0
    addi    r0, r0, 8
    addi    r2, r2, 8
    jmp     .Ldata_loop
.Ldata_done:

    /* Copy device_states, k_heap_area and related sections (second ROM→RAM segment).
     * LMA is contiguous with .datas in ROM; VMA is at __device_states_start. */
    setr    r0, __device_states_start
    setr    r1, __kernel_initialized_end
    setr    r2, __device_states_load_start
    cmprr   r0, r2
    jmpe    .Ldevdata_done          /* skip if LMA == VMA */
    cmprr   r0, r1
    jmpuge  .Ldevdata_done          /* skip if empty */
.Ldevdata_loop:
    cmprr   r0, r1
    jmpuge  .Ldevdata_done
    ldidx64 r3, r2, 0
    stidx64 r3, r0, 0
    addi    r0, r0, 8
    addi    r2, r2, 8
    jmp     .Ldevdata_loop
.Ldevdata_done:

    call    z_cstart
    /* z_cstart() should not return; halt if it does */
.Lhalt:
    jmp     .Lhalt
    .size   __start, . - __start

    .text   /* back to .text for arch_switch and timer ISR */

    .globl  arch_switch
    .type   arch_switch, @function
arch_switch:
    .cfi_startproc
    /* Save R0/R1 first so we can disable the timer safely. */
    push_cfi r0, 0
    push_cfi r1, 1
    /* Disable interrupts before completing the register save so a timer
       IRQ cannot corrupt the half-built frame.  DI, not INT_MASK = 0: the
       mask keeps selecting the sources, and the thread resumes (IRET) with
       the enable bit from its frame, which is 0 = enabled. */
    di      r0

    /*
     * ISR-context fast path.
     *
     * When called from within an ISR (on the ISR stack), the stub
     * (z_klausscpu_timer_isr, z_klausscpu_isrN) already saved the task stack SP to
     * current->switch_handle.  Saving the ISR-stack SP there would
     * corrupt that value and cause the next ISR to use SP≈0.
     *
     * Detect this case via _klausscpu_in_isr.  If set: clear the flag,
     * grab switch_to from the R0 save slot, and jump directly to the
     * new thread without touching switched_from.
     */
    setr    r0, _klausscpu_in_isr
    ldidx32 r0, r0, 0
    cmprv   r0, 0
    .cfi_remember_state             /* state at .Larch_switch_from_isr */
    jmpne   .Larch_switch_from_isr

    /* ── Normal thread-context switch ────────────────────────────── */
    /* Save remaining registers. */
    push_cfi r2, 2
    push_cfi r3, 3
    push_cfi r4, 4
    push_cfi r5, 5
    push_cfi r6, 6
    push_cfi r7, 7
    push_cfi r8, 8
    push_cfi r9, 9
    push_cfi r10, 10
    push_cfi r11, 11
    push_cfi r12, 12
    push_cfi r13, 13
    push_cfi r14, 14
    push_cfi r15, 15

    /* Patch the CALL return-address slot into an IRET frame.
       [SP+128..131] = return PC (already correct — do not touch).
       [SP+132..135] = FRAME_MASK_ALL  →  upper-32 bits [10:7] = INT_MASK = 0xF. */
    getsp   r2
    setr    r3, FRAME_MASK_ALL
    stidx32 r3, r2, 132

    /* Retrieve the arch_switch arguments from the original R0/R1 slots on
       the stack (they were pushed first, so they sit at [SP+120] and [SP+112]).
       R0_saved = switch_to      (new SP)
       R1_saved = switched_from  (&current switch_handle slot)           */
    ldidx64 r3, r2, 120   /* r3 = original R0 = switch_to */
    ldidx64 r4, r2, 112   /* r4 = original R1 = switched_from */

    /* Store current SP → *switched_from. */
    stidx64 r2, r4, 0

    /* Load new SP from switch_to (r3 holds the void* value). */
    setsp   r3

    portRESTORE_CONTEXT
    iret

    /* ── ISR-context switch ───────────────────────────────────────── */
    /*
     * The ISR stub already saved the preempted task's complete context
     * (R0-R15 + IRET frame) on the task stack and stored that task SP in
     * current->switch_handle.  We must NOT overwrite it with the ISR-stack SP.
     *
     * Stack layout here (only R0 and R1 pushed so far):
     *   [SP+0]  R1  (= switched_from, not needed — ISR save is authoritative)
     *   [SP+8]  R0  (= switch_to, the new thread's saved SP)
     *   [SP+16] return address (CALL slot)
     *
     * Clear _klausscpu_in_isr so the new thread runs with the flag off,
     * then switch directly to the new thread's task context.
     */
.Larch_switch_from_isr:
    .cfi_restore_state
    setr    r0, _klausscpu_in_isr
    setr    r1, 0
    stidx32 r1, r0, 0          /* _klausscpu_in_isr = 0 */

    getsp   r14
    ldidx64 r3, r14, 8         /* r3 = switch_to (original R0) */

    setsp   r3
    portRESTORE_CONTEXT
    iret
    .cfi_endproc
    .size arch_switch, . - arch_switch


/* ── Interrupt stubs ───────────────────────────────────────────────────── */
/*
 * CONFIG_USE_SWITCH=y interrupt entry, one stub per source.
 *
 * Hardware has already: decremented SP by 8, written the 8-byte IRET frame
 * (PC + INT_MASK/flags), cleared the source's INT_MASK bit.
 *
 * The stub saves R0-R15 on the interrupted stack, then DI.  Between the
 * entry and the DI retiring another source may be taken: it nests as a
 * complete interrupt (its own frame and register save below ours, its handler
 * on the ISR stack, IRET back into this stub's first instructions), which is
 * safe because the stub has not touched any shared state yet.  After the DI
 * nothing can interrupt it, so the stubs share _klausscpu_in_isr and
 * _klausscpu_isr_handle without nesting.  It then records "in ISR", stores the
 * interrupted SP in current->switch_handle and _klausscpu_isr_handle, and
 * switches to the ISR stack, _kernel.cpus[0].irq_stack (Zephyr's
 * z_interrupt_stacks).
 *
 * The handler (sys_clock_isr() for the timer, z_klausscpu_irq_dispatch(n) for
 * the others) ends with z_klausscpu_isr_exit(): if a higher-priority thread
 * is ready it calls arch_switch(), whose ISR fast path restores the new thread
 * and IRETs directly, never returning here.  If the handler returns, the
 * interrupted thread is restored.  (The early-boot guard against a NULL ready
 * queue is in z_klausscpu_isr_exit().)
 */
.macro isr_enter
    portSAVE_CONTEXT
    /* Interrupts off for the rest of the handler.  R0 is saved, so DI can
       use it for the old enable bit. */
    di      r0

    /* Record "in ISR" so arch_is_in_isr() and arch_switch ISR fast path work. */
    setr    r0, _klausscpu_in_isr
    setr    r1, 1
    stidx32 r1, r0, 0

    /* Save interrupted task SP → current->switch_handle AND _klausscpu_isr_handle. */
    getsp   r0
    copy    r4, r0             /* task's saved SP, kept for the unwinder */
    .cfi_def_cfa r4, 136
    setr    r1, _kernel
    ldidx64 r2, r1, ___cpu_t_current_OFFSET
    stidx64 r0, r2, ___thread_t_switch_handle_OFFSET
    ldidx64 r3, r1, ___cpu_t_irq_stack_OFFSET
    setr    r1, _klausscpu_isr_handle
    stidx64 r0, r1, 0          /* _klausscpu_isr_handle = saved task SP */

    /* Switch to the ISR stack. */
    setsp   r3
.endm

.macro isr_leave
    /* The handler returned: no switch was triggered.
       Clear _klausscpu_in_isr and restore the interrupted thread. */
    setr    r0, _klausscpu_in_isr
    setr    r1, 0
    stidx32 r1, r0, 0

    setr    r12, _klausscpu_isr_handle
    ldidx64 r12, r12, 0        /* r12 = saved task SP */
    setsp   r12

    portRESTORE_CONTEXT
    iret
.endm

    .globl  z_klausscpu_timer_isr
    .type   z_klausscpu_timer_isr, @function
z_klausscpu_timer_isr:
    .cfi_startproc
    .cfi_signal_frame
    isr_enter
    /* Announce the tick (and reschedule: z_klausscpu_isr_exit()). */
    call    sys_clock_isr
    isr_leave
    .cfi_endproc
    .size z_klausscpu_timer_isr, . - z_klausscpu_timer_isr

.macro device_isr n
    .globl  z_klausscpu_isr\n
    .type   z_klausscpu_isr\n, @function
z_klausscpu_isr\n:
    .cfi_startproc
    .cfi_signal_frame
    isr_enter
    setr    r0, \n
    call    z_klausscpu_irq_dispatch
    isr_leave
    .cfi_endproc
    .size z_klausscpu_isr\n, . - z_klausscpu_isr\n
.endm

    device_isr 1
    device_isr 2
    device_isr 3
