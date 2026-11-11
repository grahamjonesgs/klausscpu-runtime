# void *dlsym(void *restrict handle, const char *restrict name): __dlsym
# with our caller's return address (at [sp]), which RTLD_NEXT searches from.
.global dlsym
.hidden __dlsym
.type dlsym, %function
dlsym:
	getsp r2
	ldidx64 r2, r2, 0
	tail __dlsym
