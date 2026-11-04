/* klausscpu_prof.h — cumulative cycle counters for the I/O and crypto paths.
 *
 * The SD disk driver and the wolfSSL crypto glue add the cycles they spend
 * (from the free-running 100 MHz PERF_CYCLES counter) so that `bench prof`
 * can split a transfer's time into SD, GCM and everything else.  Times are
 * wall time inside the call, so they include any preemption by
 * higher-priority threads.  Updates are not atomic; that's fine for a
 * profile.
 */
#ifndef KLAUSSCPU_PROF_H
#define KLAUSSCPU_PROF_H

#include <stdint.h>

struct klausscpu_prof {
	uint64_t sd_rd_cyc, sd_rd_sec, sd_rd_calls;
	uint64_t sd_rd_late;	/* pipelined reads retried (sd_rx_block) */
	uint64_t sd_wr_cyc, sd_wr_sec, sd_wr_calls;
	uint64_t gcm_cyc, gcm_bytes, gcm_calls;
	uint64_t ctr_cyc, ctr_bytes, ctr_calls;
};

/* Defined in drivers/disk/disk_klausscpu_sd.c (every app that profiles
 * has the SD card). */
extern struct klausscpu_prof klausscpu_prof;

static inline uint64_t kprof_now(void)
{
	return *(volatile uint64_t *)0xF00D0008ul;	/* PERF_CYCLES */
}

#endif /* KLAUSSCPU_PROF_H */
