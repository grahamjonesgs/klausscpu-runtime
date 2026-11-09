/* __thread (kernel uses none, but percpu tricks sometimes) */
__thread int t;
int f(void) { return t; }
