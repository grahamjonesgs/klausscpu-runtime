/* "X" constraint */
void f(long x) { asm volatile("# %0" :: "X"(x)); }
