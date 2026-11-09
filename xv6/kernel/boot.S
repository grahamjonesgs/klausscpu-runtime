// klausscpu: boot (replaces entry.S and start.c; XV6_PLAN.md).
//
// The loader puts the image at PA 0x20 and starts here with translation
// off.  The kernel is linked at kseg (KERNBASE + PA), so until it jumps
// there only position-independent code and constants may be used.
//
//  1. TLB entry 0 := a global identity mapping of page 0 (this code), so
//     the instruction after the MTSR that turns TE on can be fetched
//     (MMU_PLAN §11.2: it is fetched translated).
//  2. STATUS.TE = 1.
//  3. Jump (absolute) to the kseg alias, zero .bss, switch to stack0 and
//     call main().  main() drops entry 0 (tlbinit()).

    .section .text.boot, "ax"
    .globl  _start
_start:
    setr    r0, 0
    mtsr    tlb_index, r0
    setr    r0, 0x100               // TLB_HI: VPN 0, G = 1
    mtsr    tlb_hi, r0
    setr    r0, 0xB                 // TLB_LO: PFN 0, X | R | V
    mtsr    tlb_lo, r0
    tlbwi
    mfsr    r0, status
    bset    r0, r0, 9               // TE
    mtsr    status, r0
    setr    r0, kentry              // its kseg address: an absolute jump
    jmpr    r0                      // (jmp would be relaxed to PC-relative)

kentry:
    setr    r0, __bss_start
    setr    r1, __bss_end
    setr    r2, 0
.Lzero:
    cmprr   r0, r1
    jmpuge  .Lzeroed
    stidx64 r2, r0, 0
    addv    r0, r0, 8
    jmp     .Lzero
.Lzeroed:
    setr    r0, stack0_top
    setsp   r0
    call    main
.Lspin:
    jmp     .Lspin

    // The boot stack (main, then the scheduler).  In .bss, zeroed above
    // before it is used.
    .bss
    .p2align 4
stack0:
    .space  16384
    .globl  stack0_top
stack0_top:
