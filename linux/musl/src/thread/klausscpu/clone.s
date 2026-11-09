# int __clone(func, stack, flags, arg, ptid, tls, ctid)
#              r0     r1     r2    r3  [sp+8] [sp+16] [sp+24]
# syscall(SYS_clone, flags, stack, ptid, ctid, tls)   (no CLONE_BACKWARDS)
#              r12    r0     r1     r2    r3    r4
.global __clone
.hidden __clone
.type __clone, %function
__clone:
	andi r1, r1, -16
	addi r1, r1, -16
	stidx64 r0, r1, 0		# func and arg for the child, on its stack
	stidx64 r3, r1, 8
	getsp r8
	ldidx64 r9, r8, 8		# ptid
	ldidx64 r10, r8, 16		# tls
	ldidx64 r11, r8, 24		# ctid
	push r4				# r4 is callee-saved
	copy r0, r2
	copy r2, r9
	copy r3, r11
	copy r4, r10
	setr r12, 220			# SYS_clone
	syscall
	fbri r12, 0, 0, 1f		# EQ: the child
	pop r4
	ret
1:	getsp r8			# the child's SP is the new stack
	ldidx64 r9, r8, 0
	ldidx64 r0, r8, 8
	setr r15, 0
	callr r9
	copy r0, r12
	setr r12, 93			# SYS_exit
	syscall
