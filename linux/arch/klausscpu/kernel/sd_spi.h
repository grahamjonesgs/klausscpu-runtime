/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The SD card's SPI-mode protocol (sd_spi.c), used by the block driver
 * (sdcard.c) and by the host test (linux/tools/sd_spi_test/).  Callers
 * serialise all calls.
 */
#ifndef _KLAUSSCPU_SD_SPI_H
#define _KLAUSSCPU_SD_SPI_H

#include <linux/types.h>

/* Counters, read-only module parameters. */
struct sd_spi_stats {
	unsigned long late;		/* pipelined blocks that read late, retried */
	unsigned long multi_fallbacks;	/* multi-block commands redone per block */
	unsigned long retries;		/* single-block commands retried */
};
extern struct sd_spi_stats sd_spi_stats;

bool sd_spi_blockaddr(void);
int sd_spi_init(void);
sector_t sd_spi_capacity(void);
/*
 * n sectors from lba to/from buf.  multi: one CMD18/CMD25 for the run,
 * else a command per sector.  pipe: the pipelined byte loops.  0 or -EIO.
 */
int sd_spi_read(sector_t lba, u8 *buf, unsigned int n, bool multi, bool pipe);
int sd_spi_write(sector_t lba, const u8 *buf, unsigned int n, bool multi, bool pipe);

#endif
