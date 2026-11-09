# int setjmp(jmp_buf b): b = {r4, r5, r6, r7, r15, sp, return address}
.global __setjmp
.global _setjmp
.global setjmp
.type __setjmp, %function
.type _setjmp, %function
.type setjmp, %function
__setjmp:
_setjmp:
setjmp:
	pop r1				# the return address; SP is now the caller's
	stidx64 r4, r0, 0
	stidx64 r5, r0, 8
	stidx64 r6, r0, 16
	stidx64 r7, r0, 24
	stidx64 r15, r0, 32
	getsp r2
	stidx64 r2, r0, 40
	stidx64 r1, r0, 48
	setr r12, 0
	jmpr r1
