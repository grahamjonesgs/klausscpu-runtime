/*
 * entropy_klausscpu_trng.c — Zephyr entropy driver for the KlaussCPU TRNG.
 *
 * Registers (klaussCPU MMIO_MAP.md, base 0xF00C_0000):
 *   0x000 CTRL    W  [0] ENABLE, [1] RESEED (self-clearing: drop the FIFO
 *                     and refill it with fresh conditioned words)
 *   0x008 STATUS  R  [0] READY (a word is in the FIFO), [1] HEALTH_OK
 *   0x010 DATA    R  64-bit conditioned word; the read pops the FIFO
 *
 * HEALTH_OK is sticky: once a continuous health test trips, it reads 0 and no
 * new words enter the FIFO until a RESEED.  A request that finds it clear
 * reseeds once; if the health tests still fail, the request fails with -EIO
 * rather than hand out suspect entropy.
 */

#define DT_DRV_COMPAT klausscpu_trng

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(entropy_klausscpu_trng, CONFIG_ENTROPY_LOG_LEVEL);

#define TRNG_CTRL   0x000u
#define TRNG_STATUS 0x008u
#define TRNG_DATA   0x010u

#define TRNG_CTRL_ENABLE      BIT(0)
#define TRNG_CTRL_RESEED      BIT(1)
#define TRNG_STATUS_READY     BIT(0)
#define TRNG_STATUS_HEALTH_OK BIT(1)

/* The FIFO refills at about 400 k words/s: a reseed is ready well within
 * this many polls.  They run with the lock held, on the failure path only. */
#define TRNG_READY_POLLS 100000

struct trng_config {
	uintptr_t base;
};

static struct k_spinlock trng_lock;

static inline uint32_t trng_status(uintptr_t base)
{
	return *(volatile uint32_t *)(base + TRNG_STATUS);
}

static inline void trng_ctrl(uintptr_t base, uint32_t val)
{
	*(volatile uint32_t *)(base + TRNG_CTRL) = val;
}

static inline uint64_t trng_data(uintptr_t base)
{
	return *(volatile uint64_t *)(base + TRNG_DATA);
}

/* RESEED and wait for the first fresh word.  0 if the health tests pass. */
static int trng_reseed(uintptr_t base)
{
	trng_ctrl(base, TRNG_CTRL_ENABLE | TRNG_CTRL_RESEED);
	for (int i = 0; i < TRNG_READY_POLLS; i++) {
		uint32_t st = trng_status(base);

		if (st & TRNG_STATUS_READY) {
			return (st & TRNG_STATUS_HEALTH_OK) ? 0 : -EIO;
		}
	}
	return -ETIMEDOUT;
}

/* Copy up to len bytes.  With wait, block until len bytes are delivered;
 * otherwise stop at the first empty FIFO.  Returns the count or -EIO. */
static int trng_fill(uintptr_t base, uint8_t *buf, uint16_t len, bool wait)
{
	uint16_t got = 0;
	bool reseeded = false;

	while (got < len) {
		uint32_t st = trng_status(base);

		if (!(st & TRNG_STATUS_HEALTH_OK)) {
			if (reseeded || trng_reseed(base) != 0) {
				LOG_ERR("TRNG health tests failing (status %#x)", st);
				return -EIO;
			}
			reseeded = true;
			continue;
		}
		if (!(st & TRNG_STATUS_READY)) {
			if (!wait) {
				break;
			}
			continue;
		}
		uint64_t w = trng_data(base);
		uint16_t n = MIN((uint16_t)sizeof(w), (uint16_t)(len - got));

		memcpy(buf + got, &w, n);
		got += n;
	}
	return got;
}

static int trng_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
	const struct trng_config *cfg = dev->config;
	k_spinlock_key_t key = k_spin_lock(&trng_lock);
	int ret = trng_fill(cfg->base, buf, len, true);

	k_spin_unlock(&trng_lock, key);
	return ret < 0 ? ret : 0;
}

static int trng_get_entropy_isr(const struct device *dev, uint8_t *buf, uint16_t len,
				uint32_t flags)
{
	const struct trng_config *cfg = dev->config;
	k_spinlock_key_t key = k_spin_lock(&trng_lock);
	int ret = trng_fill(cfg->base, buf, len, (flags & ENTROPY_BUSYWAIT) != 0);

	k_spin_unlock(&trng_lock, key);
	if (ret == 0 && len > 0) {
		return -ENODATA;
	}
	return ret;
}

static int trng_init(const struct device *dev)
{
	const struct trng_config *cfg = dev->config;
	int ret;

	trng_ctrl(cfg->base, TRNG_CTRL_ENABLE);
	/* Drop words left from before this boot, then start from fresh ones. */
	for (int i = 0; i < 32 && (trng_status(cfg->base) & TRNG_STATUS_READY); i++) {
		(void)trng_data(cfg->base);
	}
	ret = trng_reseed(cfg->base);
	if (ret != 0) {
		LOG_ERR("TRNG not ready after reseed (%d)", ret);
	}
	return ret;
}

static DEVICE_API(entropy, trng_api) = {
	.get_entropy = trng_get_entropy,
	.get_entropy_isr = trng_get_entropy_isr,
};

#define TRNG_INIT(n)                                                                       \
	static const struct trng_config trng_config_##n = {                               \
		.base = DT_INST_REG_ADDR(n),                                               \
	};                                                                                 \
	DEVICE_DT_INST_DEFINE(n, trng_init, NULL, NULL, &trng_config_##n, PRE_KERNEL_1,    \
			      CONFIG_ENTROPY_INIT_PRIORITY, &trng_api);

DT_INST_FOREACH_STATUS_OKAY(TRNG_INIT)
