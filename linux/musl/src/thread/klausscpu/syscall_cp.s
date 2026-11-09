# long __syscall_cp_asm(volatile int *cancel, long nr, a, b, c, d, e, f)
#                        r0                    r1  r2 r3 [sp+8..32]
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type __syscall_cp_asm, %function
__syscall_cp_asm:
__cp_begin:
	ldidx32 r8, r0, 0
	fbri r8, 0, 8, 2f		# NE: cancelled (fbri takes local labels only)
	getsp r8
	ldidx64 r9, r8, 24		# e
	ldidx64 r10, r8, 32		# f
	copy r12, r1
	copy r0, r2
	copy r1, r3
	ldidx64 r2, r8, 8		# c
	ldidx64 r3, r8, 16		# d
	push r4
	push r5
	copy r4, r9
	copy r5, r10
	syscall
__cp_end:
	pop r5
	pop r4
	ret
__cp_cancel:
2:
	jmp __cancel
