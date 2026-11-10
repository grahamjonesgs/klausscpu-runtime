// SPDX-License-Identifier: GPL-2.0
/*
 * The board's QSPI configuration flash (S25FL128S, 16 MiB) through
 * klaussCPU's qspi_ctrl (MMIO 0xF012_0000), READ-ONLY, as MTD:
 *   bitstream  0x000000-0xEFFFFF  the FPGA's configuration
 *   storage    0xF00000-0xFBFFFF  Zephyr's keystore (NVS): the SSH host key
 *   scratch    0xFC0000-0xFFFFFF  board tests
 * (the Zephyr port's nexys_a7.dts).  The same chip configures the FPGA, so
 * this driver only ever issues RDID and READ: there is no write or erase
 * path at all, and the MTD device and its partitions are ROM.
 *
 * Registers (the Zephyr port's flash_klausscpu_qspi.c): CMD 0x000 ([7:0]
 * opcode, [24:16] length), ADDR 0x008, STATUS 0x010 ([0] busy, [1]
 * rejected, [2] ready), ID 0x028 (0x5153_5049_0000_0001; bitstreams without
 * qspi_ctrl, and the emulator, read 0), BUF 0x100 (256 bytes).
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <asm/klausscpu.h>

#define Q_BASE		(MMIO_BASE + 0x00120000UL)
#define Q_CMD		0x000
#define Q_ADDR		0x008
#define Q_STATUS	0x010
#define Q_ID		0x028
#define Q_BUF		0x100
#define Q_BUSY		0x1
#define Q_REJECTED	0x2
#define Q_READY		0x4
#define Q_ID_V1		0x5153504900000001ULL

#define OP_READ		0x03
#define OP_RDID		0x9f
#define Q_CHUNK		256
#define FLASH_SIZE	0x1000000

static DEFINE_MUTEX(q_lock);

static void __iomem *q(unsigned int off)
{
	return (void __iomem *)(Q_BASE + off);
}

/* Only RDID and READ are ever passed here. */
static int q_cmd(u8 op, u32 len, u32 addr)
{
	int spins = 0;

	if (op != OP_READ && op != OP_RDID)
		return -EPERM;
	writeq(addr, q(Q_ADDR));
	writeq(op | ((u64)len << 16), q(Q_CMD));
	while (readq(q(Q_STATUS)) & Q_BUSY) {
		if (++spins > 1000000)
			return -ETIMEDOUT;
		cpu_relax();
	}
	return (readq(q(Q_STATUS)) & Q_REJECTED) ? -EIO : 0;
}

static u8 q_buf_byte(u32 i)
{
	return readq(q(Q_BUF + (i & ~7u))) >> ((i & 7) * 8);
}

static int q_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
		      size_t *retlen, u_char *buf)
{
	int rc = 0;

	*retlen = 0;
	mutex_lock(&q_lock);
	while (len) {
		u32 n = min_t(size_t, len, Q_CHUNK), i = 0;

		rc = q_cmd(OP_READ, n, from);
		if (rc)
			break;
		for (; i + 8 <= n; i += 8) {
			u64 w = readq(q(Q_BUF + i));

			memcpy(buf + i, &w, 8);
		}
		for (; i < n; i++)
			buf[i] = q_buf_byte(i);
		buf += n;
		from += n;
		len -= n;
		*retlen += n;
	}
	mutex_unlock(&q_lock);
	return rc;
}

static const struct mtd_partition q_parts[] = {
	{ .name = "bitstream", .offset = 0x000000, .size = 0xf00000, .mask_flags = MTD_WRITEABLE },
	{ .name = "storage",   .offset = 0xf00000, .size = 0x0c0000, .mask_flags = MTD_WRITEABLE },
	{ .name = "scratch",   .offset = 0xfc0000, .size = 0x040000, .mask_flags = MTD_WRITEABLE },
};

static int __init kl_qspi_init(void)
{
	struct mtd_info *mtd;
	u8 id[3];
	int i;

	if (readq(q(Q_ID)) != Q_ID_V1) {
		pr_info("klausscpu-qspi: no qspi_ctrl in this bitstream\n");
		return 0;
	}
	for (i = 0; i < 1000 && !(readq(q(Q_STATUS)) & Q_READY); i++)
		udelay(10);
	if (q_cmd(OP_RDID, sizeof(id), 0))
		return -EIO;
	for (i = 0; i < sizeof(id); i++)
		id[i] = q_buf_byte(i);
	if (id[0] != 0x01 || id[1] != 0x20 || id[2] != 0x18) {
		pr_warn("klausscpu-qspi: unexpected JEDEC id %02x %02x %02x\n", id[0], id[1], id[2]);
		return -ENODEV;
	}

	mtd = kzalloc(sizeof(*mtd), GFP_KERNEL);
	if (!mtd)
		return -ENOMEM;
	mtd->name = "klausscpu-qspi";
	mtd->type = MTD_ROM;
	mtd->flags = MTD_CAP_ROM;		/* never writeable */
	mtd->size = FLASH_SIZE;
	mtd->erasesize = 0x10000;
	mtd->writesize = 1;
	mtd->writebufsize = 1;
	mtd->_read = q_mtd_read;
	mtd->owner = THIS_MODULE;
	pr_info("klausscpu-qspi: S25FL128S 16 MiB, read-only\n");
	return mtd_device_register(mtd, q_parts, ARRAY_SIZE(q_parts));
}
device_initcall(kl_qspi_init);
