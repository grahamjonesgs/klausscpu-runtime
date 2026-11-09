# void __unmapself(void *base, size_t size): munmap our own stack, then exit.
.global __unmapself
.type __unmapself, %function
__unmapself:
	setr r12, 215		# SYS_munmap
	syscall
	setr r0, 0
	setr r12, 93		# SYS_exit
	syscall
