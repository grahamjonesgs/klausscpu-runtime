/* -ffixed-r13 (global register for current / thread pointer) */
register unsigned long cur asm("r13");
unsigned long f(void) { return cur; }
