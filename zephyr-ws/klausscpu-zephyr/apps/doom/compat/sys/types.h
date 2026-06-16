/* Minimal POSIX <sys/types.h> for the Doom port (minimal libc lacks it).
 * Types match Zephyr's exactly so duplicate typedefs are identical
 * (-Wno-typedef-redefinition is already in the engine compile flags). */
#ifndef DOOM_COMPAT_SYS_TYPES_H
#define DOOM_COMPAT_SYS_TYPES_H
#include <stddef.h>
typedef __SIZE_TYPE__   ssize_t;
typedef __INTPTR_TYPE__ off_t;
typedef unsigned int    mode_t;
#endif
