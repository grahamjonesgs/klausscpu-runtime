/*
 * disk_klausscpu_sd.c — Zephyr disk driver for KlaussCPU SD card (SPI mode).
 *
 * Wraps the same SPI SD controller used by the FreeRTOS runtime (sd.c).
 * Provides the Zephyr disk_access API so FatFS can mount the SD card.
 * SD v2 cards: SDHC/SDXC (OCR CCS=1) block-addressed, SDSC (CCS=0)
 * byte-addressed with 512-byte blocks (CMD16).
 */
#define DT_DRV_COMPAT klausscpu_sd

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(disk_klausscpu_sd, CONFIG_DISK_KLAUSSCPU_SD_LOG_LEVEL);

/* ── MMIO register definitions (from runtime/mmio.h) ─────────────────────── */

#define SD_BASE           0xF0000000u
#define REG(a)            (*(volatile uint32_t *)(unsigned long)(a))

#define REG_SD_CTRL       REG(SD_BASE + 0x0000u)
#define REG_SD_DATA       REG(SD_BASE + 0x0008u)
#define REG_SD_STATUS     REG(SD_BASE + 0x0010u)

#define SD_STATUS_BUSY    (1u << 0)
#define SD_STATUS_CARD    (1u << 1)

#define SD_CTRL_CLKDIV(d) ((uint32_t)((d) & 0xFFFFu))
#define SD_CTRL_CS_HI     (1u << 16)
#define SD_CTRL_PWR_ON    (1u << 17)

#define SD_CLK_INIT       249u
#define SD_CLK_FAST       1u

/* Power-up: >= 1 ms after the supply is up before the first clocks (SD spec). */
#define SD_PWRUP_US       1000u

/* ── Low-level SPI ───────────────────────────────────────────────────────── */

static int s_ready;
static int s_blockaddr;	/* OCR CCS: 1 = SDHC/SDXC (block address), 0 = SDSC (byte address) */

static uint8_t spi_xfer(uint8_t tx)
{
	REG_SD_DATA = tx;
	while (REG_SD_STATUS & SD_STATUS_BUSY) {
	}
	return (uint8_t)REG_SD_DATA;
}

static void cs_assert(void)
{
	REG_SD_CTRL = (REG_SD_CTRL & 0xFFFFu) | SD_CTRL_PWR_ON;
}

static void cs_deassert(void)
{
	REG_SD_CTRL = (REG_SD_CTRL & 0xFFFFu) | SD_CTRL_CS_HI | SD_CTRL_PWR_ON;
}

static void set_clk(uint16_t div)
{
	REG_SD_CTRL = (REG_SD_CTRL & ~0xFFFFu) | SD_CTRL_CLKDIV(div);
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
	spi_xfer(0xFF);
	spi_xfer(0x40 | (cmd & 0x3F));
	spi_xfer((uint8_t)(arg >> 24));
	spi_xfer((uint8_t)(arg >> 16));
	spi_xfer((uint8_t)(arg >> 8));
	spi_xfer((uint8_t)(arg));
	spi_xfer(crc | 0x01);
	for (int i = 0; i < 8; i++) {
		uint8_t r = spi_xfer(0xFF);

		if ((r & 0x80) == 0) {
			return r;
		}
	}
	return 0xFF;
}

static uint8_t sd_acmd(uint8_t acmd, uint32_t arg)
{
	sd_cmd(55, 0, 0x65);
	return sd_cmd(acmd, arg, 0x77);
}

/* ── SD card init ────────────────────────────────────────────────────────── */

static int sd_hw_init(void)
{
	s_ready = 0;
	REG_SD_CTRL = SD_CTRL_CLKDIV(SD_CLK_INIT) | SD_CTRL_CS_HI | SD_CTRL_PWR_ON;

	k_busy_wait(SD_PWRUP_US);

	for (int i = 0; i < 10; i++) {
		spi_xfer(0xFF);
	}

	cs_assert();

	if (sd_cmd(0, 0, 0x95) != 0x01) {
		cs_deassert();
		return -1;
	}
	if (sd_cmd(8, 0x000001AAu, 0x87) != 0x01) {
		cs_deassert();
		return -2;
	}
	spi_xfer(0xFF);
	spi_xfer(0xFF);
	spi_xfer(0xFF);
	spi_xfer(0xFF);

	for (int tries = 0; tries < 2000; tries++) {
		if (sd_acmd(41, 0x40000000u) == 0x00) {
			goto ready;
		}
		for (volatile int d = 0; d < 500; d++) {
		}
	}
	cs_deassert();
	return -3;

ready:
	/* OCR (CMD58, R3): CCS (bit 30) says how CMD17/CMD24 address the card. */
	if (sd_cmd(58, 0, 0xFD) != 0x00) {
		cs_deassert();
		return -4;
	}
	uint8_t ocr = spi_xfer(0xFF);	/* OCR[31:24] */

	spi_xfer(0xFF);
	spi_xfer(0xFF);
	spi_xfer(0xFF);
	s_blockaddr = (ocr & 0x40) != 0;

	/* SDSC: byte addresses, 512-byte blocks. */
	if (!s_blockaddr && sd_cmd(16, 512, 0xFF) != 0x00) {
		cs_deassert();
		return -5;
	}

	cs_deassert();
	set_clk(SD_CLK_FAST);
	s_ready = 1;
	return 0;
}

/* CMD17/CMD24 argument for block lba. */
static uint32_t blk_arg(uint32_t lba)
{
	return s_blockaddr ? lba : lba * 512u;
}

static int sd_read_block(uint32_t lba, uint8_t *buf)
{
	cs_assert();
	if (sd_cmd(17, blk_arg(lba), 0xFF) != 0x00) {
		cs_deassert();
		return -1;
	}

	for (int tries = 0; tries < 200000; tries++) {
		if (spi_xfer(0xFF) == 0xFE) {
			goto got_token;
		}
	}
	cs_deassert();
	return -2;

got_token:
	for (int i = 0; i < 512; i++) {
		buf[i] = spi_xfer(0xFF);
	}
	spi_xfer(0xFF);
	spi_xfer(0xFF);
	cs_deassert();
	return 0;
}

static int sd_write_block(uint32_t lba, const uint8_t *buf)
{
	cs_assert();
	if (sd_cmd(24, blk_arg(lba), 0xFF) != 0x00) {
		cs_deassert();
		return -1;
	}

	spi_xfer(0xFF);
	spi_xfer(0xFE);

	for (int i = 0; i < 512; i++) {
		spi_xfer(buf[i]);
	}
	spi_xfer(0xFF);
	spi_xfer(0xFF);

	uint8_t resp = spi_xfer(0xFF) & 0x1F;

	if (resp != 0x05) {
		cs_deassert();
		return -3;
	}

	for (int tries = 0; tries < 200000; tries++) {
		if (spi_xfer(0xFF) != 0x00) {
			break;
		}
	}
	cs_deassert();
	return 0;
}

static int sd_read_csd(uint8_t out[16])
{
	cs_assert();
	if (sd_cmd(9, 0, 0xAF) != 0x00) {
		cs_deassert();
		return -1;
	}

	for (int tries = 0; tries < 200000; tries++) {
		if (spi_xfer(0xFF) == 0xFE) {
			goto got_csd;
		}
	}
	cs_deassert();
	return -2;

got_csd:
	for (int i = 0; i < 16; i++) {
		out[i] = spi_xfer(0xFF);
	}
	spi_xfer(0xFF);
	spi_xfer(0xFF);
	cs_deassert();
	return 0;
}

/* ── Zephyr disk_access API ──────────────────────────────────────────────── */

static struct k_mutex sd_mutex;

static int disk_klausscpu_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_mutex_init(&sd_mutex);
	return 0;
}

static int disk_klausscpu_access_init(struct disk_info *disk)
{
	ARG_UNUSED(disk);
	k_mutex_lock(&sd_mutex, K_FOREVER);
	int rc = sd_hw_init();
	k_mutex_unlock(&sd_mutex);
	if (rc != 0) {
		LOG_ERR("SD card init failed: %d", rc);
		return rc;
	}
	LOG_INF("SD card initialized");
	return 0;
}

static int disk_klausscpu_access_status(struct disk_info *disk)
{
	ARG_UNUSED(disk);
	if (!s_ready) {
		return DISK_STATUS_UNINIT;
	}
	return DISK_STATUS_OK;
}

static int disk_klausscpu_access_read(struct disk_info *disk, uint8_t *buf,
				      uint32_t sector, uint32_t count)
{
	ARG_UNUSED(disk);
	if (!s_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&sd_mutex, K_FOREVER);
	for (uint32_t i = 0; i < count; i++) {
		if (sd_read_block(sector + i, buf + i * 512u) != 0) {
			k_mutex_unlock(&sd_mutex);
			return -EIO;
		}
	}
	k_mutex_unlock(&sd_mutex);
	return 0;
}

static int disk_klausscpu_access_write(struct disk_info *disk,
				       const uint8_t *buf,
				       uint32_t sector, uint32_t count)
{
	ARG_UNUSED(disk);
	if (!s_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&sd_mutex, K_FOREVER);
	for (uint32_t i = 0; i < count; i++) {
		if (sd_write_block(sector + i, buf + i * 512u) != 0) {
			k_mutex_unlock(&sd_mutex);
			return -EIO;
		}
	}
	k_mutex_unlock(&sd_mutex);
	return 0;
}

static int disk_klausscpu_access_ioctl(struct disk_info *disk, uint8_t cmd,
				       void *buf)
{
	ARG_UNUSED(disk);
	if (!s_ready) {
		return -ENODEV;
	}

	switch (cmd) {
	case DISK_IOCTL_CTRL_SYNC:
		return 0;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		*(uint32_t *)buf = 512;
		return 0;
	case DISK_IOCTL_GET_SECTOR_COUNT: {
		uint8_t csd[16];

		k_mutex_lock(&sd_mutex, K_FOREVER);
		int rc = sd_read_csd(csd);
		k_mutex_unlock(&sd_mutex);
		if (rc != 0) {
			return -EIO;
		}
		switch ((csd[0] >> 6) & 0x3) {
		case 0: {
			/* CSD v1 (SDSC): (C_SIZE+1) << (C_SIZE_MULT+2) << READ_BL_LEN bytes */
			uint32_t read_bl_len = csd[5] & 0x0Fu;
			uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
					  ((uint32_t)csd[7] << 2) |
					  ((uint32_t)csd[8] >> 6);
			uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1) |
					       ((uint32_t)csd[10] >> 7);

			*(uint32_t *)buf = (uint32_t)((uint64_t)(c_size + 1u)
						      << (c_size_mult + 2u)
						      << read_bl_len >> 9);
			return 0;
		}
		case 1: {
			/* CSD v2 (SDHC/SDXC): (C_SIZE+1) * 512 KiB */
			uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
					  ((uint32_t)csd[8] << 8) |
					  (uint32_t)csd[9];

			*(uint32_t *)buf = (c_size + 1u) * 1024u;
			return 0;
		}
		default:
			return -ENOTSUP;
		}
	}
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
		*(uint32_t *)buf = 1;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static const struct disk_operations disk_klausscpu_ops = {
	.init = disk_klausscpu_access_init,
	.status = disk_klausscpu_access_status,
	.read = disk_klausscpu_access_read,
	.write = disk_klausscpu_access_write,
	.ioctl = disk_klausscpu_access_ioctl,
};

static struct disk_info disk_klausscpu_info = {
	.name = CONFIG_DISK_KLAUSSCPU_SD_VOLUME_NAME,
	.ops = &disk_klausscpu_ops,
};

static int disk_klausscpu_register(void)
{
	k_mutex_init(&sd_mutex);
	return disk_access_register(&disk_klausscpu_info);
}

SYS_INIT(disk_klausscpu_register, POST_KERNEL,
	 CONFIG_DISK_KLAUSSCPU_SD_INIT_PRIORITY);
