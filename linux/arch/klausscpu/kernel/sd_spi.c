// SPDX-License-Identifier: GPL-2.0
/*
 * The board's SD card controller (SPI mode, MMIO 0xF000_0000; the runtime's
 * mmio.h): the card protocol, ported from the Zephyr driver
 * (klausscpu-zephyr drivers/disk/disk_klausscpu_sd.c).  sdcard.c wraps it
 * as a block device.  linux/tools/sd_spi_test/ runs this file on the host
 * against a model of the controller and a card.
 *
 * Registers: CTRL [15:0] clock divider, [16] CS high, [17] power; DATA:
 * a write starts an 8-bit transfer, a read returns the last byte received
 * (latched when its transfer completes, separately from the shift
 * register); STATUS [0] busy.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <asm/klausscpu.h>
#include "sd_spi.h"

#define SD_BASE		(MMIO_BASE + 0x0)
#define SD_CTRL		0x00
#define SD_DATA		0x08
#define SD_STATUS	0x10
#define SD_STATUS_BUSY	0x1
#define SD_CTRL_CS_HI	(1u << 16)
#define SD_CTRL_PWR_ON	(1u << 17)
#define SD_CLK_INIT	249		/* ~200 kHz */
#define SD_CLK_FAST	1		/* ~25 MHz */

/* SD spec, SPI mode: read access <= 100 ms, write busy <= 250 ms (SDHC),
 * 500 ms (SDXC); with margin. */
#define SD_READY_MS	500
#define SD_TOKEN_MS	250
#define SD_WBUSY_MS	750
#define SD_RETRIES	3

/* Step codes, as Zephyr's driver logs them. */
#define SD_E_CMD	-1	/* command rejected; info = R1 */
#define SD_E_TOKEN	-2	/* no data token in time */
#define SD_E_DATA	-3	/* error token or data rejected; info = it */
#define SD_E_BUSY	-4	/* still busy after SD_WBUSY_MS */
#define SD_E_LATE	-5	/* a pipelined read may have lost a byte */

struct sd_spi_stats sd_spi_stats;
static bool sd_blockaddr;		/* OCR CCS: SDHC/SDXC block addresses */

static void __iomem *reg(unsigned int off)
{
	return (void __iomem *)(SD_BASE + off);
}

static bool spi_busy(void)
{
	return readl(reg(SD_STATUS)) & SD_STATUS_BUSY;
}

static u8 spi_xfer(u8 tx)
{
	writel(tx, reg(SD_DATA));
	while (spi_busy())
		cpu_relax();
	return readl(reg(SD_DATA));
}

/*
 * Data-block loops.  As DATA latches each received byte, the read loop can
 * start byte i+1 before reading byte i, so the CPU's per-byte work overlaps
 * the transfer instead of following it.  If byte i+1 completed before byte
 * i was read, it would replace it silently (the data CRC isn't checked).
 * Interrupts are off for each start+read pair, but an instruction-fetch
 * miss can still stall the read past the window, so STATUS is read after
 * DATA: if the bus is already idle, byte i+1 may have landed first, and the
 * block fails with SD_E_LATE, to be retried with the plain loop (see
 * sd_spi_read).  A wrong byte always implies that idle reading, so detection is
 * complete; a false alarm costs one retry.  Both loops end with the bus
 * idle.  Between pairs, interrupts are harmless: byte i+1 waits, latched.
 */
static int sd_rx_block(u8 *buf, bool pipe)
{
	unsigned long flags;
	bool late = false;
	int i;

	if (!pipe) {
		for (i = 0; i < 512; i++)
			buf[i] = spi_xfer(0xff);
		return 0;
	}
	writel(0xff, reg(SD_DATA));
	for (i = 0; i < 511; i++) {
		u32 st;
		u8 b;

		raw_local_irq_save(flags);
		while (spi_busy())
			;
		writel(0xff, reg(SD_DATA));	/* start byte i+1 */
		b = readl(reg(SD_DATA));	/* byte i (latched) */
		st = readl(reg(SD_STATUS));	/* i+1 still shifting? */
		raw_local_irq_restore(flags);
		buf[i] = b;
		late |= !(st & SD_STATUS_BUSY);
	}
	while (spi_busy())
		;
	buf[511] = readl(reg(SD_DATA));
	if (late) {
		sd_spi_stats.late++;
		return SD_E_LATE;
	}
	return 0;
}

/* Nothing to read back when writing: wait for the bus, start the byte. */
static void sd_tx_block(const u8 *buf, bool pipe)
{
	int i;

	if (!pipe) {
		for (i = 0; i < 512; i++)
			spi_xfer(buf[i]);
		return;
	}
	for (i = 0; i < 512; i++) {
		while (spi_busy())
			;
		writel(buf[i], reg(SD_DATA));
	}
	while (spi_busy())
		;
}

static void cs_assert(void)
{
	writel((readl(reg(SD_CTRL)) & 0xffff) | SD_CTRL_PWR_ON, reg(SD_CTRL));
}

static void cs_deassert(void)
{
	writel((readl(reg(SD_CTRL)) & 0xffff) | SD_CTRL_CS_HI | SD_CTRL_PWR_ON, reg(SD_CTRL));
}

static void set_clk(u16 div)
{
	writel((readl(reg(SD_CTRL)) & ~0xffffu) | div, reg(SD_CTRL));
}

/* Clock until the card releases MISO (0xFF): it has finished being busy. */
static int sd_wait_ready(unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms) + 1;

	do {
		if (spi_xfer(0xff) == 0xff)
			return 0;
	} while (time_before(jiffies, end));
	return -ETIMEDOUT;
}

static u8 sd_cmd(u8 cmd, u32 arg, u8 crc)
{
	int i;

	if (cmd == 0)
		spi_xfer(0xff);
	else if (sd_wait_ready(SD_READY_MS))
		return 0xff;
	spi_xfer(0x40 | (cmd & 0x3f));
	spi_xfer(arg >> 24);
	spi_xfer(arg >> 16);
	spi_xfer(arg >> 8);
	spi_xfer(arg);
	spi_xfer(crc | 1);
	for (i = 0; i < 8; i++) {
		u8 r = spi_xfer(0xff);

		if (!(r & 0x80))
			return r;
	}
	return 0xff;
}

static u8 sd_acmd(u8 acmd, u32 arg)
{
	sd_cmd(55, 0, 0x65);
	return sd_cmd(acmd, arg, 0x77);
}

/* Wait for a data token: 0 for 0xFE, SD_E_TOKEN, or SD_E_DATA (info). */
static int sd_wait_token(u8 *info)
{
	unsigned long end = jiffies + msecs_to_jiffies(SD_TOKEN_MS) + 1;
	u8 t;

	while ((t = spi_xfer(0xff)) == 0xff)
		if (!time_before(jiffies, end))
			return SD_E_TOKEN;
	if (t != 0xfe) {
		*info = t;
		return SD_E_DATA;
	}
	return 0;
}

/* Wait out a busy period (MISO held low): 0, or SD_E_BUSY. */
static int sd_wait_not_busy(unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms) + 1;

	while (spi_xfer(0xff) == 0x00)
		if (!time_before(jiffies, end))
			return SD_E_BUSY;
	return 0;
}

bool sd_spi_blockaddr(void)
{
	return sd_blockaddr;
}

int sd_spi_init(void)
{
	int i;
	u8 ocr;

	writel(SD_CLK_INIT | SD_CTRL_CS_HI | SD_CTRL_PWR_ON, reg(SD_CTRL));
	usleep_range(1000, 2000);
	for (i = 0; i < 10; i++)
		spi_xfer(0xff);
	cs_assert();
	if (sd_cmd(0, 0, 0x95) != 0x01)
		goto fail;
	if (sd_cmd(8, 0x1aa, 0x87) != 0x01)
		goto fail;
	for (i = 0; i < 4; i++)
		spi_xfer(0xff);
	for (i = 0; i < 2000; i++) {
		if (sd_acmd(41, 0x40000000) == 0)
			goto ready;
		udelay(500);
	}
	goto fail;
ready:
	if (sd_cmd(58, 0, 0xfd) != 0)
		goto fail;
	ocr = spi_xfer(0xff);
	spi_xfer(0xff);
	spi_xfer(0xff);
	spi_xfer(0xff);
	sd_blockaddr = ocr & 0x40;
	if (!sd_blockaddr && sd_cmd(16, 512, 0xff) != 0)
		goto fail;
	cs_deassert();
	set_clk(SD_CLK_FAST);
	return 0;
fail:
	cs_deassert();
	return -ENODEV;
}

/* The card's size in 512-byte sectors, from the CSD (v1 SDSC, v2 SDHC/XC). */
sector_t sd_spi_capacity(void)
{
	u8 csd[16], info;
	int i;

	cs_assert();
	if (sd_cmd(9, 0, 0xaf) != 0 || sd_wait_token(&info)) {
		cs_deassert();
		return 0;
	}
	for (i = 0; i < 16; i++)
		csd[i] = spi_xfer(0xff);
	spi_xfer(0xff);
	spi_xfer(0xff);
	cs_deassert();

	switch (csd[0] >> 6) {
	case 0: {
		u32 bl_len = csd[5] & 0xf;
		u32 c_size = ((csd[6] & 3) << 10) | (csd[7] << 2) | (csd[8] >> 6);
		u32 mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);

		return ((u64)(c_size + 1) << (mult + 2) << bl_len) >> 9;
	}
	case 1: {
		u32 c_size = ((csd[7] & 0x3f) << 16) | (csd[8] << 8) | csd[9];

		return (sector_t)(c_size + 1) * 1024;
	}
	}
	return 0;
}

static u32 blk_arg(sector_t lba)
{
	return sd_blockaddr ? lba : lba * 512;
}

/* Single-block read (CMD17). */
static int sd_read_block(sector_t lba, u8 *buf, u8 *info, bool pipe)
{
	u8 r1;
	int rc;

	cs_assert();
	r1 = sd_cmd(17, blk_arg(lba), 0xff);
	if (r1 != 0) {
		*info = r1;
		cs_deassert();
		return SD_E_CMD;
	}
	rc = sd_wait_token(info);
	if (!rc) {
		rc = sd_rx_block(buf, pipe);
		spi_xfer(0xff);			/* CRC, not checked */
		spi_xfer(0xff);
	}
	cs_deassert();
	return rc;
}

/* Single-block write (CMD24). */
static int sd_write_block(sector_t lba, const u8 *buf, u8 *info, bool pipe)
{
	u8 r1, resp;
	int rc;

	cs_assert();
	r1 = sd_cmd(24, blk_arg(lba), 0xff);
	if (r1 != 0) {
		*info = r1;
		cs_deassert();
		return SD_E_CMD;
	}
	spi_xfer(0xff);
	spi_xfer(0xfe);
	sd_tx_block(buf, pipe);
	spi_xfer(0xff);
	spi_xfer(0xff);
	resp = spi_xfer(0xff) & 0x1f;
	if (resp != 0x05) {
		*info = resp;
		cs_deassert();
		return SD_E_DATA;
	}
	/* The card holds MISO low while it programs the block. */
	rc = sd_wait_not_busy(SD_WBUSY_MS);
	cs_deassert();
	return rc;
}

/*
 * Multi-block transfers (CMD18/CMD25): one command for a run of sectors.
 * A read pays the card's access latency once, and a write lets the card
 * program the run as one sequential operation (ACMD23 pre-erases it).
 */

/*
 * CMD12 ends a multi-block read.  It goes out while the card is still
 * streaming data, so there is no ready wait first.  After the command come
 * a stuff byte, R1, then a busy period (R1b).
 */
static void sd_stop_read(void)
{
	int i;

	spi_xfer(0x40 | 12);
	spi_xfer(0);
	spi_xfer(0);
	spi_xfer(0);
	spi_xfer(0);
	spi_xfer(0x61);
	spi_xfer(0xff);
	for (i = 0; i < 8; i++)
		if (!(spi_xfer(0xff) & 0x80))
			break;
	sd_wait_not_busy(SD_WBUSY_MS);
}

static int sd_read_multi(sector_t lba, u8 *buf, unsigned int n, u8 *info, bool pipe)
{
	unsigned int b;
	int rc = 0;
	u8 r1;

	cs_assert();
	r1 = sd_cmd(18, blk_arg(lba), 0xff);
	if (r1 != 0) {
		*info = r1;
		cs_deassert();
		return SD_E_CMD;
	}
	for (b = 0; b < n; b++, buf += 512) {
		rc = sd_wait_token(info);
		if (rc)
			break;
		rc = sd_rx_block(buf, pipe);
		if (rc)
			break;
		spi_xfer(0xff);			/* CRC */
		spi_xfer(0xff);
	}
	sd_stop_read();
	cs_deassert();
	return rc;
}

static int sd_write_multi(sector_t lba, const u8 *buf, unsigned int n, u8 *info, bool pipe)
{
	unsigned int b;
	int rc = 0, brc;
	u8 r1, resp;

	cs_assert();
	/*
	 * Pre-erase hint, ignored if unsupported.  If the write then fails and
	 * the per-block fallback fails too, sectors past the failure may be
	 * left erased rather than holding their old data.
	 */
	sd_acmd(23, n);
	r1 = sd_cmd(25, blk_arg(lba), 0xff);
	if (r1 != 0) {
		*info = r1;
		cs_deassert();
		return SD_E_CMD;
	}
	for (b = 0; b < n; b++, buf += 512) {
		spi_xfer(0xff);
		spi_xfer(0xfc);			/* multi-block data token */
		sd_tx_block(buf, pipe);
		spi_xfer(0xff);			/* CRC */
		spi_xfer(0xff);
		resp = spi_xfer(0xff) & 0x1f;
		if (resp != 0x05) {
			*info = resp;
			rc = SD_E_DATA;
			break;
		}
		rc = sd_wait_not_busy(SD_WBUSY_MS);
		if (rc)
			break;
	}
	/*
	 * The card ignores the stop token while busy (it would stay in
	 * receive-data state and refuse every later command), so wait first,
	 * as ChaN's reference driver does, also after an error response.
	 */
	sd_wait_not_busy(SD_WBUSY_MS);
	spi_xfer(0xfd);				/* stop transmission token */
	spi_xfer(0xff);
	brc = sd_wait_not_busy(SD_WBUSY_MS);
	cs_deassert();
	return rc ? rc : brc;
}

/*
 * Retries.  Each transfer gets SD_RETRIES tries at the card.  A late
 * pipelined read (SD_E_LATE) says nothing about the card: it switches the
 * rest of the request to the plain loop and doesn't count as a try, so a
 * stall can't use up the tries that card errors need.  (Zephyr's driver
 * uses the plain loop only for the last single-block try.)
 */
int sd_spi_read(sector_t lba, u8 *buf, unsigned int n, bool multi, bool pipe)
{
	unsigned int i;
	int a, rc;
	u8 info = 0;

	if (n > 1 && multi) {
		for (a = 0; a < SD_RETRIES; ) {
			rc = sd_read_multi(lba, buf, n, &info, pipe);
			if (!rc)
				return 0;
			if (rc == SD_E_LATE)
				pipe = false;
			else
				a++;
		}
		sd_spi_stats.multi_fallbacks++;
		pr_warn_ratelimited("klausscpu-sd: multi-block read %llu+%u failed (step %d info 0x%02x); retrying per block\n",
				    (u64)lba, n, rc, info);
	}
	for (i = 0; i < n; i++) {
		for (a = 0; a < SD_RETRIES; ) {
			rc = sd_read_block(lba + i, buf + i * 512, &info, pipe);
			if (!rc)
				break;
			sd_spi_stats.retries++;
			if (rc == SD_E_LATE)
				pipe = false;
			else
				a++;
		}
		if (rc) {
			pr_err_ratelimited("klausscpu-sd: read sector %llu failed (step %d info 0x%02x)\n",
					   (u64)(lba + i), rc, info);
			return -EIO;
		}
	}
	return 0;
}

int sd_spi_write(sector_t lba, const u8 *buf, unsigned int n, bool multi, bool pipe)
{
	unsigned int i;
	int a, rc;
	u8 info = 0;

	if (n > 1 && multi) {
		for (a = 0; a < SD_RETRIES; a++) {
			rc = sd_write_multi(lba, buf, n, &info, pipe);
			if (!rc)
				return 0;
		}
		sd_spi_stats.multi_fallbacks++;
		pr_warn_ratelimited("klausscpu-sd: multi-block write %llu+%u failed (step %d info 0x%02x); retrying per block\n",
				    (u64)lba, n, rc, info);
	}
	for (i = 0; i < n; i++) {
		for (a = 0; a < SD_RETRIES; a++) {
			rc = sd_write_block(lba + i, buf + i * 512, &info, pipe);
			if (!rc || rc == SD_E_BUSY)
				break;	/* still busy: a retry can't help */
			sd_spi_stats.retries++;
		}
		if (rc) {
			pr_err_ratelimited("klausscpu-sd: write sector %llu failed (step %d info 0x%02x)\n",
					   (u64)(lba + i), rc, info);
			return -EIO;
		}
	}
	return 0;
}
