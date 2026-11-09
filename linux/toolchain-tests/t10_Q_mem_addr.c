/* "r"(p) + "memory" clobber with a store */
void f(long *p, long v) { asm volatile("stidx64 %1, %0, 0" :: "r"(p), "r"(v) : "memory"); }
