# void longjmp(jmp_buf b, int val): back to setjmp's caller with val (1 if 0)
.global _longjmp
.global longjmp
.type _longjmp, %function
.type longjmp, %function
_longjmp:
longjmp:
	ldidx64 r4, r0, 0
	ldidx64 r5, r0, 8
	ldidx64 r6, r0, 16
	ldidx64 r7, r0, 24
	ldidx64 r15, r0, 32
	ldidx64 r2, r0, 40
	setsp r2
	copy r12, r1
	fbri r12, 0, 8, 1f		# NE: keep val
	setr r12, 1
1:	ldidx64 r2, r0, 48
	jmpr r2
