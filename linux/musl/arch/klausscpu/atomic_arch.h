/* KlaussCPU: LR/SC (lr32s/sc32, lr64/sc64; SC writes 0 on success). One
 * in-order core, so a barrier only has to stop the compiler. */

#define a_barrier a_barrier
static inline void a_barrier()
{
	__asm__ __volatile__ ("" : : : "memory");
}

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	long old, fail;
	__asm__ __volatile__ (
		"1:	lr32s %0, %2\n"
		"	fbr %0, %3, 8, 2f\n"	/* NE: no store */
		"	sc32 %1, %4, %2\n"
		"	fbri %1, 0, 8, 1b\n"	/* NE: lost the reservation */
		"2:"
		: "=&r"(old), "=&r"(fail)
		: "r"(p), "r"((long)t), "r"((long)s)
		: "memory");
	return old;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	void *old;
	long fail;
	__asm__ __volatile__ (
		"1:	lr64 %0, %2\n"
		"	fbr %0, %3, 8, 2f\n"
		"	sc64 %1, %4, %2\n"
		"	fbri %1, 0, 8, 1b\n"
		"2:"
		: "=&r"(old), "=&r"(fail)
		: "r"(p), "r"(t), "r"(s)
		: "memory");
	return old;
}
