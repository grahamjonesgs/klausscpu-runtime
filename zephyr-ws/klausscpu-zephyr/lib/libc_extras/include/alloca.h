/* alloca.h — minimal libc has none; alloca is the compiler builtin. */
#ifndef KLAUSSCPU_ALLOCA_H
#define KLAUSSCPU_ALLOCA_H

#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#endif /* KLAUSSCPU_ALLOCA_H */
