/* Zephyr 4.x has no <zephyr/posix/time.h>, which wolfSSL 5.7.2's wc_port.h
 * still includes when _POSIX_C_SOURCE is not set.  Provide what it and
 * ssh/tlscert.c use from it: the C library's time.h plus clockid_t,
 * CLOCK_REALTIME and clock_gettime() (which tlscert.c implements on the
 * SNTP wall-clock).  The guards match <zephyr/posix/posix_time.h>.  Goes
 * away with wolfSSL (ZEPHYR_4_5_PLAN.md step 7). */
#ifndef KLAUSSCPU_COMPAT_ZEPHYR_POSIX_TIME_H_
#define KLAUSSCPU_COMPAT_ZEPHYR_POSIX_TIME_H_

#include <time.h>
#include <zephyr/sys/clock.h>

#if !defined(_CLOCKID_T_DECLARED) && !defined(__clockid_t_defined)
typedef unsigned long clockid_t;
#define _CLOCKID_T_DECLARED
#define __clockid_t_defined
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)SYS_CLOCK_REALTIME)
#endif

int clock_gettime(clockid_t clock_id, struct timespec *ts);

#endif /* KLAUSSCPU_COMPAT_ZEPHYR_POSIX_TIME_H_ */
