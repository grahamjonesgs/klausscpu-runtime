# Signal handlers return here (sa_restorer): rt_sigreturn with SP at the frame.
.global __restore
.hidden __restore
.type __restore, %function
.global __restore_rt
.hidden __restore_rt
.type __restore_rt, %function
__restore:
__restore_rt:
	setr r12, 139			# SYS_rt_sigreturn
	syscall
