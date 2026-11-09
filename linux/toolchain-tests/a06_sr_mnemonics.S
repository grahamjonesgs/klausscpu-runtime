/* the MMU mnemonics the kernel entry code uses */
	.text
	mfsr r0, ecause
	mfsr r1, eaddr
	mfsr r2, usp
	mtsr usp, r2
	mtsr scratch0, r0
	mfsr r3, cycle
	mfsr r4, tlb_hi
	mtsr tlb_lo, r4
	mtsr tlb_index, r5
	mtsr asid, r6
	mtsr tlbvec, r7
	tlbwi
	tlbr
	tlbp
	tlbinv
	syscall
	lr64 r1, r2
	sc64 r3, r4, r5
	getsp r1
	setsp r1
	iret
	ei r1
	di r1
	trap
	halt
