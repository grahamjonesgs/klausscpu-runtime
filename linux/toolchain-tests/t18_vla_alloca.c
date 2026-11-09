int g(char *);
int f(int n) { char a[n]; return g(a) + g(__builtin_alloca(n)); }
