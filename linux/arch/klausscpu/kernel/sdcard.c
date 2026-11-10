// SPDX-License-Identifier: GPL-2.0
/*
 * The board's SD card (SPI-mode controller at MMIO 0xF000_0000, the runtime's
 * mmio.h; protocol as in the Zephyr port's disk_klausscpu_sd.c) as a block
 * device, mmcblk0 with partitions mmcblk0p1...  Polled: queue_rq runs the
 * transfer itself (BLK_MQ_F_BLOCKING), one 512-byte block per command for
 * now.  Without a card, or on an emulator without the controller (its
 * registers read 0), the probe fails and no disk is registered.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <asm/klausscpu.h>

#define SD_BASE		(MMIO_BASE + 0x0)
#define SD_CTRL		0x00
#define SD_DATA		0x08
#define SD_STATUS	0x10
#define SD_STATUS_BUSY	0x1
#define SD_CTRL_CS_HI	(1u << 16)
#define SD_CTRL_PWR_ON	(1u << 17)
#define SD_CLK_INIT	249		/* ~200 kHz */
#define SD_CLK_FAST	1		/* ~25 MHz */

#define SD_READY_MS	500
#define SD_TOKEN_MS	250
#define SD_WBUSY_MS	750
#define SD_RETRIES	3

static DEFINE_MUTEX(sd_lock);
static bool sd_blockaddr;		/* OCR CCS: SDHC/SDXC block addresses */
static int sd_major;
static struct gendisk *sd_disk;
static struct blk_mq_tag_set sd_tags;

static void __iomem *reg(unsigned int off)
{
	return (void __iomem *)(SD_BASE + off);
}

static u8 spi_xfer(u8 tx)
{
	writel(tx, reg(SD_DATA));
	while (readl(reg(SD_STATUS)) & SD_STATUS_BUSY)
		cpu_relax();
	return readl(reg(SD_DATA));
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

/* Wait for a data token: 0 for 0xFE, -ETIMEDOUT, or -EIO for an error token. */
static int sd_wait_token(void)
{
	unsigned long end = jiffies + msecs_to_jiffies(SD_TOKEN_MS) + 1;
	u8 t;

	while ((t = spi_xfer(0xff)) == 0xff)
		if (!time_before(jiffies, end))
			return -ETIMEDOUT;
	return t == 0xfe ? 0 : -EIO;
}

static int sd_hw_init(void)
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
static sector_t sd_capacity(void)
{
	u8 csd[16];
	int i;

	cs_assert();
	if (sd_cmd(9, 0, 0xaf) != 0 || sd_wait_token()) {
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

static int sd_read_block(sector_t lba, u8 *buf)
{
	int i, rc;

	cs_assert();
	if (sd_cmd(17, blk_arg(lba), 0xff) != 0) {
		cs_deassert();
		return -EIO;
	}
	rc = sd_wait_token();
	if (!rc) {
		for (i = 0; i < 512; i++)
			buf[i] = spi_xfer(0xff);
		spi_xfer(0xff);			/* CRC, not checked */
		spi_xfer(0xff);
	}
	cs_deassert();
	return rc;
}

static int sd_write_block(sector_t lba, const u8 *buf)
{
	unsigned long end;
	int i;
	u8 resp;

	cs_assert();
	if (sd_cmd(24, blk_arg(lba), 0xff) != 0) {
		cs_deassert();
		return -EIO;
	}
	spi_xfer(0xff);
	spi_xfer(0xfe);
	for (i = 0; i < 512; i++)
		spi_xfer(buf[i]);
	spi_xfer(0xff);
	spi_xfer(0xff);
	resp = spi_xfer(0xff) & 0x1f;
	if (resp != 0x05) {
		cs_deassert();
		return -EIO;
	}
	end = jiffies + msecs_to_jiffies(SD_WBUSY_MS) + 1;
	while (spi_xfer(0xff) == 0x00) {
		if (!time_before(jiffies, end)) {
			cs_deassert();
			return -ETIMEDOUT;
		}
	}
	cs_deassert();
	return 0;
}

static int sd_xfer(sector_t lba, u8 *buf, bool write)
{
	int tries, rc = -EIO;

	for (tries = 0; tries < SD_RETRIES && rc; tries++)
		rc = write ? sd_write_block(lba, buf) : sd_read_block(lba, buf);
	return rc;
}

static blk_status_t sd_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	sector_t lba = blk_rq_pos(rq);
	bool write = rq_data_dir(rq) == WRITE;
	blk_status_t st = BLK_STS_OK;
	struct req_iterator iter;
	struct bio_vec bv;

	blk_mq_start_request(rq);
	if (req_op(rq) != REQ_OP_READ && req_op(rq) != REQ_OP_WRITE) {
		st = req_op(rq) == REQ_OP_FLUSH ? BLK_STS_OK : BLK_STS_NOTSUPP;
		goto out;
	}
	mutex_lock(&sd_lock);
	rq_for_each_segment(bv, rq, iter) {
		u8 *buf = bvec_kmap_local(&bv);
		unsigned int off;

		for (off = 0; off < bv.bv_len; off += 512, lba++) {
			if (sd_xfer(lba, buf + off, write)) {
				pr_err_ratelimited("klausscpu-sd: %s error, sector %llu\n",
						   write ? "write" : "read", (u64)lba);
				st = BLK_STS_IOERR;
				break;
			}
		}
		kunmap_local(buf);
		if (st)
			break;
	}
	mutex_unlock(&sd_lock);
out:
	blk_mq_end_request(rq, st);
	return BLK_STS_OK;
}

static const struct blk_mq_ops sd_mq_ops = {
	.queue_rq = sd_queue_rq,
};

static const struct block_device_operations sd_fops = {
	.owner = THIS_MODULE,
};

static int __init klausscpu_sd_init(void)
{
	struct queue_limits lim = {
		.logical_block_size	= 512,
		.max_hw_sectors		= 128,
		.max_segments		= 32,
	};
	sector_t sectors;
	int err;

	if (sd_hw_init()) {
		pr_info("klausscpu-sd: no card\n");
		return 0;
	}
	sectors = sd_capacity();
	if (!sectors) {
		pr_warn("klausscpu-sd: card does not report its size\n");
		return 0;
	}

	sd_major = register_blkdev(0, "klausscpu-sd");
	if (sd_major < 0)
		return sd_major;

	sd_tags.ops = &sd_mq_ops;
	sd_tags.nr_hw_queues = 1;
	sd_tags.queue_depth = 4;
	sd_tags.numa_node = NUMA_NO_NODE;
	sd_tags.flags = BLK_MQ_F_BLOCKING;
	err = blk_mq_alloc_tag_set(&sd_tags);
	if (err)
		goto out_blkdev;

	sd_disk = blk_mq_alloc_disk(&sd_tags, &lim, NULL);
	if (IS_ERR(sd_disk)) {
		err = PTR_ERR(sd_disk);
		goto out_tags;
	}
	sd_disk->major = sd_major;
	sd_disk->first_minor = 0;
	sd_disk->minors = 16;
	sd_disk->fops = &sd_fops;
	strscpy(sd_disk->disk_name, "mmcblk0", DISK_NAME_LEN);
	set_capacity(sd_disk, sectors);
	err = add_disk(sd_disk);
	if (err)
		goto out_disk;
	pr_info("klausscpu-sd: mmcblk0, %llu MiB (%s)\n", (u64)sectors >> 11,
		sd_blockaddr ? "SDHC/SDXC" : "SDSC");
	return 0;

out_disk:
	put_disk(sd_disk);
out_tags:
	blk_mq_free_tag_set(&sd_tags);
out_blkdev:
	unregister_blkdev(sd_major, "klausscpu-sd");
	return err;
}
device_initcall(klausscpu_sd_init);
