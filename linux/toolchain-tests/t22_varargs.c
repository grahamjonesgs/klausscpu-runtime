#include <stdarg.h>
long f(int n, ...) { va_list ap; long s = 0; va_start(ap, n); while (n--) s += va_arg(ap, long); va_end(ap); return s; }
