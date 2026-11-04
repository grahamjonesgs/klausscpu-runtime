/* alloca.h — minimal libc has none; alloca is the compiler builtin. */
#ifndef KLAUSSCPU_ALLOCA_H
#define KLAUSSCPU_ALLOCA_H

#include <stddef.h>

#ifndef alloca			/* zephyr/sys/rb.h defines it too */
#define alloca __builtin_alloca
#endif

#endif /* KLAUSSCPU_ALLOCA_H */
