/*
 * flash_klausscpu_qspi.c — Zephyr flash driver for the board's QSPI
 * configuration flash (Spansion S25FL128S, 16 MiB) through klaussCPU's
 * qspi_ctrl command engine (MMIO 0xF012_0000; klaussCPU MMIO_MAP.md).
 *
 * Offsets are flash byte addresses.  The FPGA bitstream lives below
 * 0xF0_0000; the hardware refuses program/erase there (QSPI_FLOOR), and this
 * driver refuses them first with -EACCES.  Reads anywhere are allowed.
 *
 * Registers:
 *   0x000 CMD     W  [7:0] opcode, [24:16] length (0..256); starts the command
 *   0x008 ADDR    RW [23:0]
 *   0x010 STATUS  R  [0] busy, [1] rejected, [2] ready
 *   0x020 FLOOR   R  lowest programmable address
 *   0x028 ID      R  0x5153_5049_0000_0001 (older bitstreams read 0)
 *   0x100 BUF     RW 256-byte data buffer (little-endian dwords)
 *
 * Every call takes a mutex: commands are multi-step (WREN, PP, poll RDSR).
 * Erase (~130-650 ms per 64 KiB sector) sleeps between status polls; page
 * program (~0.25 ms) busy-polls.
 *
 * S25FL-S error handling: after a failed program or erase, P_ERR / E_ERR is
 * set and WIP *stays* 1 until CLSR; the flash ignores everything else
 * meanwhile.  So every poll checks the error bits and clears them at once,
 * a timeout also sends CLSR, and each program/erase first waits for the
 * flash to be idle (an earlier timed-out operation may still be running).
 * Polling overwrites buffer bytes 0..0, so the buffer is filled afterwards.
 */

#define DT_DRV_COMPAT klausscpu_qspi_flash

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(flash_klausscpu_qspi, CONFIG_FLASH_LOG_LEVEL);

#define Q_CMD    0x000u
#define Q_ADDR   0x008u
#define Q_STATUS 0x010u
#define Q_FLOOR  0x020u
#define Q_ID     0x028u
#define Q_BUF    0x100u

#define Q_STATUS_BUSY     BIT(0)
#define Q_STATUS_REJECTED BIT(1)
#define Q_STATUS_READY    BIT(2)
#define Q_ID_V1           0x5153504900000001ull

#define OP_READ  0x03u
#define OP_PP    0x02u
#define OP_SE    0xD8u
#define OP_WREN  0x06u
#define OP_WRDI  0x04u
#define OP_RDSR1 0x05u
#define OP_CLSR  0x30u
#define OP_RDID  0x9Fu

#define SR1_WIP   BIT(0)
#define SR1_E_ERR BIT(5)
#define SR1_P_ERR BIT(6)

#define PAGE_SIZE 256u

#define FLASH_NODE  DT_INST_CHILD(0, flash_0)
#define FLASH_SIZE  DT_REG_SIZE(FLASH_NODE)
#define ERASE_SIZE  DT_PROP(FLASH_NODE, erase_block_size)
#define WRITE_SIZE  DT_PROP(FLASH_NODE, write_block_size)

/* Erase: 650 ms max for 64 KiB, 2.6 s for a 256 KiB sector; page program
 * 750 us max.  Generous timeouts (poll counts). */
#define ERASE_TIMEOUT_MS 5000
#define PP_TIMEOUT_POLLS 2000000

struct qspi_cfg {
	uintptr_t base;
};

struct qspi_data {
	struct k_mutex lock;
	uint32_t floor;
	bool present;
};

static inline uint64_t q_rd(const struct device *dev, uint32_t off)
{
	const struct qspi_cfg *cfg = dev->config;

	return *(volatile uint64_t *)(cfg->base + off);
}

static inline void q_wr(const struct device *dev, uint32_t off, uint64_t v)
{
	const struct qspi_cfg *cfg = dev->config;

	*(volatile uint64_t *)(cfg->base + off) = v;
}

/* Run one command to completion.  0, or -EACCES if the engine refused it. */
static int q_cmd(const struct device *dev, uint8_t op, uint32_t len, uint32_t addr)
{
	q_wr(dev, Q_ADDR, addr);
	q_wr(dev, Q_CMD, (uint64_t)op | ((uint64_t)len << 16));
	/* 256 bytes at 25 MHz is ~85 us: spin. */
	while (q_rd(dev, Q_STATUS) & Q_STATUS_BUSY) {
	}
	return (q_rd(dev, Q_STATUS) & Q_STATUS_REJECTED) ? -EACCES : 0;
}

static uint8_t q_buf_byte(const struct device *dev, uint32_t i)
{
	return (uint8_t)(q_rd(dev, Q_BUF + (i & ~7u)) >> ((i & 7u) * 8u));
}

static uint8_t q_rdsr1(const struct device *dev)
{
	(void)q_cmd(dev, OP_RDSR1, 1, 0);
	return q_buf_byte(dev, 0);
}

/* Wait for WIP to clear.  sleep_ms > 0 sleeps between polls (erase).
 * A program/erase error (WIP stuck with P_ERR/E_ERR) is cleared with CLSR
 * and returns -EIO; a timeout also sends CLSR and returns -ETIMEDOUT. */
static int q_wait_ready(const struct device *dev, int sleep_ms, uint32_t limit)
{
	uint8_t sr;
	uint32_t n = 0;

	for (;;) {
		sr = q_rdsr1(dev);
		if (sr & (SR1_E_ERR | SR1_P_ERR)) {
			LOG_ERR("flash SR1 error 0x%02x", sr);
			(void)q_cmd(dev, OP_CLSR, 0, 0);
			(void)q_cmd(dev, OP_WRDI, 0, 0);
			return -EIO;
		}
		if (!(sr & SR1_WIP)) {
			return 0;
		}
		if (++n >= limit) {
			LOG_ERR("flash busy timeout (SR1 0x%02x)", sr);
			(void)q_cmd(dev, OP_CLSR, 0, 0);
			return -ETIMEDOUT;
		}
		if (sleep_ms > 0) {
			k_msleep(sleep_ms);
		}
	}
}

/* Before a program or erase: let any earlier operation finish. */
static int q_idle(const struct device *dev)
{
	return q_wait_ready(dev, 2, ERASE_TIMEOUT_MS / 2);
}

static bool range_ok(off_t offset, size_t len)
{
	return offset >= 0 && (size_t)offset <= FLASH_SIZE && len <= FLASH_SIZE - (size_t)offset;
}

static int qspi_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	struct qspi_data *d = dev->data;
	uint8_t *out = data;
	int rc = 0;

	if (!d->present) {
		return -ENODEV;
	}
	if (!range_ok(offset, len)) {
		return -EINVAL;
	}
	k_mutex_lock(&d->lock, K_FOREVER);
	while (len > 0) {
		uint32_t n = MIN(len, PAGE_SIZE);
		uint32_t i = 0;

		rc = q_cmd(dev, OP_READ, n, (uint32_t)offset);
		if (rc) {
			break;
		}
		for (; i + 8 <= n; i += 8) {
			uint64_t w = q_rd(dev, Q_BUF + i);

			memcpy(out + i, &w, 8);
		}
		for (; i < n; i++) {
			out[i] = q_buf_byte(dev, i);
		}
		out += n;
		offset += n;
		len -= n;
	}
	k_mutex_unlock(&d->lock);
	return rc;
}

static int qspi_write(const struct device *dev, off_t offset, const void *data, size_t len)
{
	struct qspi_data *d = dev->data;
	const uint8_t *in = data;
	int rc = 0;

	if (!d->present) {
		return -ENODEV;
	}
	if (!range_ok(offset, len)) {
		return -EINVAL;
	}
	if (len == 0) {
		return 0;
	}
	if ((uint32_t)offset < d->floor) {
		return -EACCES;
	}
	k_mutex_lock(&d->lock, K_FOREVER);
	while (len > 0) {
		/* A page program must not cross a 256-byte page boundary. */
		uint32_t n = MIN(len, PAGE_SIZE - ((uint32_t)offset & (PAGE_SIZE - 1)));
		uint32_t i;

		rc = q_idle(dev);         /* polls RDSR: before filling the buffer */
		if (rc) {
			break;
		}
		for (i = 0; i < n; i += 8) {
			uint64_t w = ~0ull;

			memcpy(&w, in + i, MIN(8u, n - i));
			q_wr(dev, Q_BUF + i, w);
		}
		rc = q_cmd(dev, OP_WREN, 0, 0);
		if (!rc) {
			rc = q_cmd(dev, OP_PP, n, (uint32_t)offset);
		}
		if (!rc) {
			rc = q_wait_ready(dev, 0, PP_TIMEOUT_POLLS);
		}
		if (rc) {
			(void)q_cmd(dev, OP_WRDI, 0, 0);
			break;
		}
		in += n;
		offset += n;
		len -= n;
	}
	k_mutex_unlock(&d->lock);
	return rc;
}

static int qspi_erase(const struct device *dev, off_t offset, size_t size)
{
	struct qspi_data *d = dev->data;
	int rc = 0;

	if (!d->present) {
		return -ENODEV;
	}
	if (!range_ok(offset, size) || (offset % ERASE_SIZE) || (size % ERASE_SIZE)) {
		return -EINVAL;
	}
	if (size == 0) {
		return 0;
	}
	if ((uint32_t)offset < d->floor) {
		return -EACCES;
	}
	k_mutex_lock(&d->lock, K_FOREVER);
	for (size_t done = 0; done < size; done += ERASE_SIZE) {
		rc = q_idle(dev);
		if (!rc) {
			rc = q_cmd(dev, OP_WREN, 0, 0);
		}
		if (!rc) {
			rc = q_cmd(dev, OP_SE, 0, (uint32_t)(offset + done));
		}
		if (!rc) {
			rc = q_wait_ready(dev, 2, ERASE_TIMEOUT_MS / 2);
		}
		if (rc) {
			(void)q_cmd(dev, OP_WRDI, 0, 0);
			break;
		}
	}
	k_mutex_unlock(&d->lock);
	return rc;
}

static const struct flash_parameters qspi_params = {
	.write_block_size = WRITE_SIZE,
	.erase_value = 0xff,
};

static const struct flash_parameters *qspi_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &qspi_params;
}

static int qspi_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);
	*size = FLASH_SIZE;
	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static const struct flash_pages_layout qspi_layout = {
	.pages_count = FLASH_SIZE / ERASE_SIZE,
	.pages_size = ERASE_SIZE,
};

static void qspi_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
			     size_t *layout_size)
{
	ARG_UNUSED(dev);
	*layout = &qspi_layout;
	*layout_size = 1;
}
#endif

static DEVICE_API(flash, qspi_api) = {
	.read = qspi_read,
	.write = qspi_write,
	.erase = qspi_erase,
	.get_parameters = qspi_get_parameters,
	.get_size = qspi_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = qspi_page_layout,
#endif
};

static int qspi_init(const struct device *dev)
{
	struct qspi_data *d = dev->data;
	uint8_t id[6];

	k_mutex_init(&d->lock);
	d->present = false;
	if (q_rd(dev, Q_ID) != Q_ID_V1) {
		LOG_WRN("no QSPI controller in this bitstream");
		return -ENODEV;
	}
	for (int i = 0; i < 1000 && !(q_rd(dev, Q_STATUS) & Q_STATUS_READY); i++) {
		k_busy_wait(10);
	}
	if (q_cmd(dev, OP_RDID, sizeof(id), 0)) {
		return -EIO;
	}
	for (int i = 0; i < (int)sizeof(id); i++) {
		id[i] = q_buf_byte(dev, i);
	}
	if (id[0] != 0x01 || id[1] != 0x20 || id[2] != 0x18) {
		LOG_ERR("unexpected flash JEDEC id %02x %02x %02x", id[0], id[1], id[2]);
		return -ENODEV;
	}
	/* RDID byte 4: 0 = uniform 256 KiB sectors, 1 = 64 KiB + 4 KiB params. */
	if (id[4] == 0x00 && ERASE_SIZE < KB(256)) {
		LOG_ERR("flash has 256 KiB sectors but DT erase-block-size is %u", ERASE_SIZE);
		return -EINVAL;
	}
	/* Clear any error left by an interrupted program/erase (WIP stuck with
	 * P_ERR/E_ERR survives a CPU reset), and let a running one finish. */
	(void)q_wait_ready(dev, 0, 5000000);
	d->floor = (uint32_t)q_rd(dev, Q_FLOOR);
	d->present = true;
	LOG_INF("S25FL128S %u MiB, writable from 0x%06x", (unsigned)(FLASH_SIZE >> 20), d->floor);
	return 0;
}

static const struct qspi_cfg qspi_cfg0 = {
	.base = DT_INST_REG_ADDR(0),
};
static struct qspi_data qspi_data0;

DEVICE_DT_INST_DEFINE(0, qspi_init, NULL, &qspi_data0, &qspi_cfg0, POST_KERNEL,
		      CONFIG_FLASH_INIT_PRIORITY, &qspi_api);
