/* TP (SR 7, user-writable) holds the pthread pointer: musl's default
 * layout (TLS below the thread structure).  No compiler TLS is used. */
static inline uintptr_t __get_tp()
{
	uintptr_t tp;
	__asm__ ("mfsr %0, tp" : "=r"(tp));
	return tp;
}

#define MC_PC __pc
