/* unistd.h — the types portable code expects from it (ssize_t, off_t,
 * SEEK_*).  Minimal libc has none and these apps don't use POSIX file
 * descriptors, so no functions are declared. */
#ifndef KLAUSSCPU_UNISTD_H
#define KLAUSSCPU_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#endif /* KLAUSSCPU_UNISTD_H */
