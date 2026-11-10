// SPDX-License-Identifier: GPL-2.0
/*
 * The board's TRNG (MMIO 0xF00C_0000; the runtime's mmio.h) as a hw_random
 * source, which also seeds the kernel's random pool at boot.  Present when
 * CPU_FEATURES[20] says so (every bitstream with the crypto probe has it);
 * the emulator reports no TRNG.
 */
#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/io.h>
#include <asm/klausscpu.h>

#define TRNG_BASE	(MMIO_BASE + 0x000C0000UL)
#define TRNG_CTRL	0x00
#define TRNG_STATUS	0x08
#define TRNG_DATA	0x10
#define TRNG_ENABLE	0x1
#define TRNG_READY	0x1
#define TRNG_HEALTH_OK	0x2
#define CPU_FEAT_TRNG	(1UL << 20)

extern unsigned long klausscpu_cpu_features;

static void __iomem *trng(unsigned int off)
{
	return (void __iomem *)(TRNG_BASE + off);
}

static int kl_trng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	size_t got = 0;
	int spins = 0;

	while (got + 8 <= max) {
		u32 st = readl(trng(TRNG_STATUS));
		u64 w;

		if (!(st & TRNG_HEALTH_OK))
			return got ? got : -EIO;
		if (!(st & TRNG_READY)) {
			if (!wait || ++spins > 1000)
				break;
			udelay(10);
			continue;
		}
		w = readq(trng(TRNG_DATA));
		memcpy(data + got, &w, 8);
		got += 8;
	}
	return got;
}

static struct hwrng kl_trng = {
	.name		= "klausscpu-trng",
	.read		= kl_trng_read,
	.quality	= 1000,
};

static int __init kl_trng_init(void)
{
	if (!(klausscpu_cpu_features & CPU_FEAT_TRNG)) {
		pr_info("klausscpu-trng: not in this bitstream\n");
		return 0;
	}
	writel(TRNG_ENABLE, trng(TRNG_CTRL));
	return hwrng_register(&kl_trng);
}
device_initcall(kl_trng_init);
