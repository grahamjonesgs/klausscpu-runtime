/* The kernel interfaces sd_spi.c uses, for the host test (sim.c models the
 * registers behind readl/writel). */
#ifndef SD_SPI_TEST_HOST_H
#define SD_SPI_TEST_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef u64 sector_t;

/* The kernel's values (not <errno.h>: it includes <linux/errno.h>, this shim). */
#define EIO		5
#define ENODEV		19
#define ETIMEDOUT	110

#define __iomem
#define MMIO_BASE	0xF0000000UL

u32 model_readl(unsigned long off);
void model_writel(u32 v, unsigned long off);
#define readl(a)	model_readl((unsigned long)(a) - MMIO_BASE)
#define writel(v, a)	model_writel((v), (unsigned long)(a) - MMIO_BASE)

extern volatile unsigned long jiffies;
#define msecs_to_jiffies(ms)	((unsigned long)(ms) / 10 + 1)
#define time_before(a, b)	((long)((a) - (b)) < 0)

#define raw_local_irq_save(f)		((f) = 1)
#define raw_local_irq_restore(f)	((void)(f))
#define cpu_relax()			((void)0)
#define udelay(us)			((void)0)
#define usleep_range(a, b)		((void)0)
#define msleep(ms)			((void)0)

extern int model_verbose;
extern unsigned long model_log_lines;
#define pr_log(fmt, ...) \
	do { model_log_lines++; if (model_verbose) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define pr_warn_ratelimited	pr_log
#define pr_err_ratelimited	pr_log

#endif
