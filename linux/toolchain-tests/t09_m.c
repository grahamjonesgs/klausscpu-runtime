/* "m" memory constraint with a load/store */
long f(long *p) { long v; asm volatile("ldidx64 %0, %1" : "=r"(v) : "m"(*p)); return v; }
