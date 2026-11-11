# int sigsetjmp(sigjmp_buf b, int savemask)
# sigjmp_buf = {__jmp_buf (8 longs, 0..63), __fl (64), __ss (72..)}: the
# return address goes in __fl and r4 in __ss[1] while setjmp runs, as the
# other ports do.
.global sigsetjmp
.global __sigsetjmp
.type sigsetjmp, %function
.type __sigsetjmp, %function
.hidden __sigsetjmp_tail
sigsetjmp:
__sigsetjmp:
	fbri r1, 0, 8, 1f		# NE: save the mask too
	tail setjmp
1:	pop r2
	stidx64 r2, r0, 64
	stidx64 r4, r0, 80
	copy r4, r0
	call setjmp
	copy r1, r12
	copy r0, r4
	ldidx64 r4, r0, 80
	ldidx64 r2, r0, 64
	push r2
	tail __sigsetjmp_tail
