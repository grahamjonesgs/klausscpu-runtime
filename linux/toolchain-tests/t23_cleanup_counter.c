/* __attribute__((cleanup)), __COUNTER__, _Static_assert, typeof, statement exprs */
void unlock(int *p);
int f(void) { int __attribute__((cleanup(unlock))) l = __COUNTER__; _Static_assert(sizeof(long) == 8, "LP64"); typeof(l) x = ({ int y = 3; y; }); return l + x; }
