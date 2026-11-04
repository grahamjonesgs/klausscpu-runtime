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
#include <klausscpu_prof.h>

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

/* Data-block loops.  The controller latches each received byte into DATA
 * when the byte completes, separately from its shift register, so the read
 * loop can start byte i+1 before reading byte i: the CPU's per-byte work then
 * overlaps the 32-cycle transfer instead of following it.  If byte i+1
 * finished before byte i was read, it would silently replace it (the data
 * CRC isn't checked).  Interrupts are off for each start+read pair (DI/EI,
 * ~3 cycles), but an instruction-fetch miss (~54 cycles to DDR) can still
 * stall the read past the window.  So STATUS is read after DATA: if the bus
 * is already idle, byte i+1 may have landed first, and the block fails with
 * -5 to be retried; the last retry uses the plain loop.  A wrong byte always
 * implies that idle reading, so detection is complete; a false alarm costs
 * one retry.  Both loops end with the bus idle.  klausscpu_sd_pipeline = 0
 * selects the plain loops (A/B). */
int klausscpu_sd_pipeline = 1;

static int sd_rx_block(uint8_t *buf, int pipe)
{
	if (!pipe) {
		for (int i = 0; i < 512; i++) {
			buf[i] = spi_xfer(0xFF);
		}
		return 0;
	}
	int late = 0;

	REG_SD_DATA = 0xFF;
	for (int i = 0; i < 511; i++) {
		unsigned int key = irq_lock();

		while (REG_SD_STATUS & SD_STATUS_BUSY) {
		}
		REG_SD_DATA = 0xFF;			/* start byte i+1 */
		uint8_t b = (uint8_t)REG_SD_DATA;	/* byte i (latched) */
		uint32_t st = REG_SD_STATUS;		/* i+1 still shifting? */

		irq_unlock(key);
		buf[i] = b;
		late |= !(st & SD_STATUS_BUSY);
	}
	while (REG_SD_STATUS & SD_STATUS_BUSY) {
	}
	buf[511] = (uint8_t)REG_SD_DATA;
	if (late) {
		klausscpu_prof.sd_rd_late++;
		return -5;
	}
	return 0;
}

static void sd_tx_block(const uint8_t *buf)
{
	if (!klausscpu_sd_pipeline) {
		for (int i = 0; i < 512; i++) {
			spi_xfer(buf[i]);
		}
		return;
	}
	/* Nothing to read back: wait for the bus, start the byte, move on. */
	for (int i = 0; i < 512; i++) {
		while (REG_SD_STATUS & SD_STATUS_BUSY) {
		}
		REG_SD_DATA = buf[i];
	}
	while (REG_SD_STATUS & SD_STATUS_BUSY) {
	}
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

/* Timeouts (SD spec, SPI mode): read access <= 100 ms, write busy <= 250 ms
 * for SDHC (500 ms for SDXC).  Deadlines are in uptime ms, with margin. */
#define SD_READY_MS       500u
#define SD_TOKEN_MS       250u
#define SD_WBUSY_MS       750u
#define SD_RETRIES        3

/* Clock the bus until the card releases MISO (0xFF), i.e. it has finished any
 * previous busy period.  Commands sent while it is busy are not seen. */
static int sd_wait_ready(uint32_t ms)
{
	int64_t end = k_uptime_get() + ms;

	do {
		if (spi_xfer(0xFF) == 0xFF) {
			return 0;
		}
	} while (k_uptime_get() < end);
	return -1;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
	if (cmd == 0) {
		spi_xfer(0xFF);	/* card not initialised yet: no ready check */
	} else if (sd_wait_ready(SD_READY_MS) != 0) {
		return 0xFF;
	}
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

/* Single-block read (CMD17).  Returns 0, or a negative step code:
 * -1 command rejected (*info = R1), -2 no data token in time,
 * -3 data error token (*info = token). */
static int sd_read_block(uint32_t lba, uint8_t *buf, uint8_t *info, int pipe)
{
	cs_assert();
	uint8_t r1 = sd_cmd(17, blk_arg(lba), 0xFF);

	if (r1 != 0x00) {
		*info = r1;
		cs_deassert();
		return -1;
	}

	int64_t end = k_uptime_get() + SD_TOKEN_MS;
	uint8_t t;

	for (;;) {
		t = spi_xfer(0xFF);
		if (t != 0xFF) {
			break;
		}
		if (k_uptime_get() >= end) {
			cs_deassert();
			return -2;
		}
	}
	if (t != 0xFE) {
		*info = t;	/* data error token: 000x_xxxx */
		cs_deassert();
		return -3;
	}

	int rc = sd_rx_block(buf, pipe);

	spi_xfer(0xFF);
	spi_xfer(0xFF);
	cs_deassert();
	return rc;
}

/* Single-block write (CMD24).  Returns 0, or -1 command rejected (*info =
 * R1), -3 data rejected (*info = data response), -4 still busy after
 * SD_WBUSY_MS. */
static int sd_write_block(uint32_t lba, const uint8_t *buf, uint8_t *info)
{
	cs_assert();
	uint8_t r1 = sd_cmd(24, blk_arg(lba), 0xFF);

	if (r1 != 0x00) {
		*info = r1;
		cs_deassert();
		return -1;
	}

	spi_xfer(0xFF);
	spi_xfer(0xFE);

	sd_tx_block(buf);
	spi_xfer(0xFF);
	spi_xfer(0xFF);

	uint8_t resp = spi_xfer(0xFF) & 0x1F;

	if (resp != 0x05) {
		*info = resp;
		cs_deassert();
		return -3;
	}

	/* The card holds MISO low while it programs the block. */
	int64_t end = k_uptime_get() + SD_WBUSY_MS;

	while (spi_xfer(0xFF) == 0x00) {
		if (k_uptime_get() >= end) {
			cs_deassert();
			return -4;
		}
	}
	cs_deassert();
	return 0;
}

/* ── Multi-block transfers (CMD18 / CMD25) ──────────────────────────────── */
/*
 * One command for a run of sectors instead of one per sector: a read pays the
 * card's access latency once, and a write lets the card program the run as
 * one sequential operation (ACMD23 pre-erases it).  FatFs hands over runs
 * whenever a file's clusters are contiguous, e.g. SFTP's 32 KiB requests.
 */

/* Wait for a data token: 0 = 0xFE, -2 = none in time, -3 = error token. */
static int sd_wait_token(uint8_t *info)
{
	int64_t end = k_uptime_get() + SD_TOKEN_MS;
	uint8_t t;

	while ((t = spi_xfer(0xFF)) == 0xFF) {
		if (k_uptime_get() >= end) {
			return -2;
		}
	}
	if (t != 0xFE) {
		*info = t;
		return -3;
	}
	return 0;
}

/* Wait out a busy period (MISO held low): 0, or -4 if still busy. */
static int sd_wait_not_busy(uint32_t ms)
{
	int64_t end = k_uptime_get() + ms;

	while (spi_xfer(0xFF) == 0x00) {
		if (k_uptime_get() >= end) {
			return -4;
		}
	}
	return 0;
}

/* CMD12 ends a multi-block read.  It goes out while the card is still
 * streaming data, so no ready wait first; the byte after the command is a
 * stuff byte, then R1, then a busy period (R1b). */
static void sd_stop_read(void)
{
	spi_xfer(0x40 | 12);
	spi_xfer(0); spi_xfer(0); spi_xfer(0); spi_xfer(0);
	spi_xfer(0x61);
	spi_xfer(0xFF);
	for (int i = 0; i < 8; i++) {
		if ((spi_xfer(0xFF) & 0x80) == 0) {
			break;
		}
	}
	(void)sd_wait_not_busy(SD_WBUSY_MS);
}

static int sd_read_multi(uint32_t lba, uint8_t *buf, uint32_t n, uint8_t *info)
{
	cs_assert();
	uint8_t r1 = sd_cmd(18, blk_arg(lba), 0xFF);

	if (r1 != 0x00) {
		*info = r1;
		cs_deassert();
		return -1;
	}
	int rc = 0;

	for (uint32_t b = 0; b < n; b++, buf += 512) {
		rc = sd_wait_token(info);
		if (rc != 0) {
			break;
		}
		rc = sd_rx_block(buf, klausscpu_sd_pipeline);
		if (rc != 0) {
			break;
		}
		spi_xfer(0xFF);
		spi_xfer(0xFF);
	}
	sd_stop_read();
	cs_deassert();
	return rc;
}

static int sd_write_multi(uint32_t lba, const uint8_t *buf, uint32_t n,
			  uint8_t *info)
{
	cs_assert();
	/* Pre-erase hint; ignored if unsupported.  If the write then fails and
	 * the per-block fallback fails too, sectors past the failure may be
	 * left erased rather than holding their old data. */
	(void)sd_acmd(23, n);
	uint8_t r1 = sd_cmd(25, blk_arg(lba), 0xFF);

	if (r1 != 0x00) {
		*info = r1;
		cs_deassert();
		return -1;
	}
	int rc = 0;

	for (uint32_t b = 0; b < n; b++, buf += 512) {
		spi_xfer(0xFF);
		spi_xfer(0xFC);			/* multi-block data token */
		sd_tx_block(buf);
		spi_xfer(0xFF);
		spi_xfer(0xFF);
		uint8_t resp = spi_xfer(0xFF) & 0x1F;

		if (resp != 0x05) {
			*info = resp;
			rc = -3;
			break;
		}
		rc = sd_wait_not_busy(SD_WBUSY_MS);
		if (rc != 0) {
			break;
		}
	}
	/* The card ignores the stop token while busy (it would stay in
	 * receive-data state and refuse every later command), so wait first,
	 * as ChaN's reference driver does, also after an error response. */
	(void)sd_wait_not_busy(SD_WBUSY_MS);
	spi_xfer(0xFD);				/* stop transmission token */
	spi_xfer(0xFF);
	int brc = sd_wait_not_busy(SD_WBUSY_MS);

	cs_deassert();
	return rc != 0 ? rc : brc;
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

struct klausscpu_prof klausscpu_prof;

/* Multi-block CMD18/CMD25 for runs of sectors (1); 0 = one command per
 * sector, for A/B measurements (`bench opt sd_multi`). */
int klausscpu_sd_multiblock = 1;

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

	uint64_t t0 = kprof_now();

	k_mutex_lock(&sd_mutex, K_FOREVER);
	if (count > 1 && klausscpu_sd_multiblock) {
		int rc;
		uint8_t info = 0;

		for (int a = 0; a < SD_RETRIES; a++) {
			rc = sd_read_multi(sector, buf, count, &info);
			if (rc == 0) {
				goto read_done;
			}
		}
		LOG_WRN("multi-block read lba %u+%u failed (step %d info 0x%02x);"
			" retrying per block", sector, count, rc, info);
	}
	for (uint32_t i = 0; i < count; i++) {
		int rc;
		uint8_t info = 0;

		for (int a = 0; a < SD_RETRIES; a++) {
			/* The last try avoids the pipelined loop (see
			 * sd_rx_block) in case it keeps reading late. */
			rc = sd_read_block(sector + i, buf + i * 512u, &info,
					   klausscpu_sd_pipeline && a < SD_RETRIES - 1);
			if (rc == 0) {
				if (a > 0) {
					LOG_WRN("read lba %u ok on try %d",
						sector + i, a + 1);
				}
				break;
			}
		}
		if (rc != 0) {
			k_mutex_unlock(&sd_mutex);
			LOG_ERR("read lba %u failed: step %d info 0x%02x",
				sector + i, rc, info);
			return -EIO;
		}
	}
read_done:
	k_mutex_unlock(&sd_mutex);
	klausscpu_prof.sd_rd_cyc += kprof_now() - t0;
	klausscpu_prof.sd_rd_sec += count;
	klausscpu_prof.sd_rd_calls++;
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

	uint64_t t0 = kprof_now();

	k_mutex_lock(&sd_mutex, K_FOREVER);
	if (count > 1 && klausscpu_sd_multiblock) {
		int rc;
		uint8_t info = 0;

		for (int a = 0; a < SD_RETRIES; a++) {
			rc = sd_write_multi(sector, buf, count, &info);
			if (rc == 0) {
				goto write_done;
			}
		}
		LOG_WRN("multi-block write lba %u+%u failed (step %d info 0x%02x);"
			" retrying per block", sector, count, rc, info);
	}
	for (uint32_t i = 0; i < count; i++) {
		int rc;
		uint8_t info = 0;

		for (int a = 0; a < SD_RETRIES; a++) {
			rc = sd_write_block(sector + i, buf + i * 512u, &info);
			if (rc == 0) {
				if (a > 0) {
					LOG_WRN("write lba %u ok on try %d",
						sector + i, a + 1);
				}
				break;
			}
			if (rc == -4) {
				break;	/* card still busy: a retry can't help */
			}
		}
		if (rc != 0) {
			k_mutex_unlock(&sd_mutex);
			LOG_ERR("write lba %u failed: step %d info 0x%02x",
				sector + i, rc, info);
			return -EIO;
		}
	}
write_done:
	k_mutex_unlock(&sd_mutex);
	klausscpu_prof.sd_wr_cyc += kprof_now() - t0;
	klausscpu_prof.sd_wr_sec += count;
	klausscpu_prof.sd_wr_calls++;
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
