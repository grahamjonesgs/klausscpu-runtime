# int __set_thread_area(void *p): TP (SR 7) is writable from user mode.
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area, %function
__set_thread_area:
	mtsr tp, r0
	setr r12, 0
	ret
