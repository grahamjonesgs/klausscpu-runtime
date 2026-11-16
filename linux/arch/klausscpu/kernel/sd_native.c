// SPDX-License-Identifier: GPL-2.0
/*
 * The native 4-bit SD controller (klaussCPU SD_NATIVE_PLAN.md, register map
 * v1) as an mmc host: the mmc core runs the card protocol and mmc_block
 * makes it mmcblk0, the same name as the SPI driver's disk.
 *
 * Present when CPU_FEATURES[21] is set and SDM_CAPS reads non-zero; then
 * the SPI driver (sdcard.c) registers nothing.  klausscpu_sd.native=0 on
 * the kernel command line keeps the SPI driver instead.
 *
 * Requests: an interrupt-driven state machine run by a threaded handler
 * (the SD interrupt is the INT2 cascade's child 1).  CMD23 if the core
 * sends it, the command, its data, then CMD12 when the core asks for a
 * stop.  Data moves by PIO through the 4 KiB FIFO, a block at a time while
 * FIFO_THRESH says one can move; the controller stops the card's clock
 * between blocks when the FIFO is full or empty, so a slow handler only
 * slows the transfer.
 *
 * DMA (SDM_CAPS[10], the SD DMA spec v1.2): a data command written with
 * SDM_CFG DMA_EN moves its blocks between the card and one contiguous DDR
 * range at SDM_DMA_ADDR, and DATA_DONE comes when they are all moved.  So
 * with DMA the host takes one segment per request (max_segs = 1, up to
 * 1 MiB), maps it with the streaming DMA API (mm/dma-noncoherent.c cleans
 * the D-cache for it and waits for the walk before the command starts the
 * DMA, and invalidates it before the core sees the data), and gets one
 * interrupt at the end.  A request the engine can't take (several
 * segments, a block size not a multiple of 8, which it refuses, a buffer
 * not 8-byte aligned, or for a read not owning whole 32-byte cache lines)
 * goes by PIO, and so does one whose mapping or SDM_DMA_ADDR write did not
 * take.  Before the buffer is unmapped the engine is stopped (DMA_EN = 0
 * aborts one in flight) and DMA_BUSY waited out, escalating to a FIFO
 * reset and then to leaving SD mode, which resets the engine; one that
 * still won't stop panics the kernel rather than let it write freed
 * memory.  The card side is recovered as with PIO (FIFO reset, CMD12).  A
 * command the engine refuses up front (DMA_ERR, nothing sent to the card)
 * is sent again by PIO; after KSD_DMA_TROUBLE_MAX DMA errors or stop
 * failures DMA is turned off.  klausscpu_sd.dma=0 (at boot, or at run time
 * in /sys/module/klausscpu_sd/parameters/dma) keeps PIO, for A/B runs.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "klausscpu_sd."

static bool sd_native = true;
module_param_named(native, sd_native, bool, 0444);
MODULE_PARM_DESC(native, "Use the native SD controller when present (else SPI)");

static bool ksd_dma_param = true;
module_param_named(dma, ksd_dma_param, bool, 0644);
MODULE_PARM_DESC(dma, "Move data by DMA where the controller can (else PIO through the FIFO)");

/* Data requests whose data phase ran by DMA and by PIO since boot (a
 * command refused by the DMA engine and sent again counts as PIO). */
static unsigned long ksd_dma_requests, ksd_pio_requests;
module_param_named(native_dma_requests, ksd_dma_requests, ulong, 0444);
module_param_named(native_pio_requests, ksd_pio_requests, ulong, 0444);

/* Errors the mmc core recovered from (or not): it logs only failures. */
static unsigned long ksd_cmd_errors, ksd_data_errors, ksd_timeouts;
/* DMA_ERRs, and DMAs that needed more than DMA_EN = 0 to stop. */
static unsigned long ksd_dma_errors, ksd_dma_stop_failures;
module_param_named(native_dma_errors, ksd_dma_errors, ulong, 0444);
module_param_named(native_dma_stop_failures, ksd_dma_stop_failures, ulong, 0444);
module_param_named(native_cmd_errors, ksd_cmd_errors, ulong, 0444);
module_param_named(native_data_errors, ksd_data_errors, ulong, 0444);
module_param_named(native_timeouts, ksd_timeouts, ulong, 0444);

#define SD_BASE		(MMIO_BASE + 0x0)
#define SD_CTRL		0x000
#define  CTRL_DIV	GENMASK(15, 0)
#define  CTRL_CS_HI	BIT(16)
#define  CTRL_PWR_ON	BIT(17)
#define  CTRL_SD_MODE	BIT(18)
#define SDM_CMD		0x100
#define  CMD_INDEX	GENMASK(5, 0)
#define  CMD_RESP	GENMASK(10, 8)
#define   RESP_NONE	0
#define   RESP_48	1		/* R1, R5, R6, R7 */
#define   RESP_48B	2		/* R1b */
#define   RESP_136	3		/* R2 */
#define   RESP_48_NOCRC	4		/* R3 */
#define  CMD_DATA	BIT(11)
#define  CMD_WRITE	BIT(12)
#define  CMD_STOP	BIT(13)
#define SDM_ARG		0x108
#define SDM_RESP0	0x110
#define SDM_RESP1	0x118
#define SDM_BLK		0x120
#define  BLK_SIZE	GENMASK(11, 0)
#define  BLK_COUNT	GENMASK(31, 16)
#define SDM_STATUS	0x128
#define  ST_CMD_BUSY	BIT(0)
#define  ST_DATA_BUSY	BIT(1)
#define  ST_CARD	BIT(3)
#define  ST_THRESH	BIT(4)
#define  ST_DMA_BUSY	BIT(5)
#define  ST_CMD_DONE	BIT(8)
#define  ST_DATA_DONE	BIT(9)
#define  ST_CMD_TIMEOUT	BIT(16)
#define  ST_CMD_CRC	BIT(17)
#define  ST_CMD_END	BIT(18)
#define  ST_CMD_IDX	BIT(19)
#define  ST_DATA_TIMEOUT BIT(20)
#define  ST_DATA_CRC	BIT(21)
#define  ST_DATA_END	BIT(22)
#define  ST_WRITE_CRC	BIT(23)
#define  ST_WRITE_NOTOK	BIT(24)
#define  ST_BUSY_TIMEOUT BIT(25)
#define  ST_FIFO_ERR	BIT(26)
#define  ST_DMA_ERR	BIT(27)		/* the DMA address left DDR */
#define  ST_STICKY	(ST_CMD_DONE | ST_DATA_DONE | GENMASK(27, 16))
#define  ST_CMD_ERRS	(ST_CMD_TIMEOUT | ST_CMD_CRC | ST_CMD_END | ST_CMD_IDX)
#define  ST_DATA_ERRS	(GENMASK(24, 20) | ST_BUSY_TIMEOUT | ST_FIFO_ERR | ST_DMA_ERR)
#define SDM_IRQ_EN	0x130
#define SDM_TIMEOUT	0x138		/* [31:0] data, [63:32] busy, in clocks */
#define SDM_CFG		0x140
#define  CFG_4BIT	BIT(0)
#define  CFG_CLK_ON	BIT(2)		/* ([1] HS edge, [7:4] HS sample phase) */
#define  CFG_DMA_EN	BIT(16)		/* sampled when SDM_CMD is written */
#define SDM_FIFO	0x148
#define  FIFO_RESET	BIT(31)		/* also aborts a DMA */
#define SDM_DMA_ADDR	0x150		/* 64-bit PA, [2:0] read 0, advances;
					 * ignored while DMA_BUSY */
#define SDM_BLKDONE	0x158
#define SDM_DATA	0x180
#define SDM_CAPS	0x1f8
#define  CAPS_VERSION	GENMASK(7, 0)
#define  CAPS_4BIT	BIT(8)
#define  CAPS_HS	BIT(9)
#define  CAPS_DMA	BIT(10)
#define  CAPS_FIFO_BLKS	GENMASK(23, 16)

#define KSD_REF_HZ	100000000	/* CLK = 100 MHz / (2 (div + 1)) */
#define KSD_DIV_INIT	124		/* 400 kHz */
#define KSD_F_MAX	25000000	/* default speed; high speed is phase 3 */
/* Slot power off before a power-up: >= 10 ms, to be measured (plan 4.1). */
#define KSD_POWER_OFF_MS 20
#define KSD_READ_TMO_MS	100		/* the reset values' 100 ms / 500 ms */
#define KSD_BUSY_TMO_MS	500
/* The wait for each step of stopping the engine (DMA_EN, FIFO reset,
 * SD_MODE).  DMA_EN = 0 lets the engine finish the block it is moving: up
 * to a block's 16-byte DDR transactions (32 for 512 B), one bus tenure
 * each, so tens of microseconds typically.  The RTL session's estimate of
 * the absolute worst (every transaction waiting ~3,100 cycles behind VGA
 * scanout and a CPU miss) is 32 x 3,100 cycles, about 1 ms, plus ~45 us of
 * card time for a read; realistic under Linux, well under 0.2 ms.  So 5 ms
 * before escalating.  A FIFO reset stops it after the transaction in
 * flight (~31 us worst; SD_NATIVE_PLAN §8.4).  Only an aborted transfer
 * waits here: after a normal one DMA_BUSY is already 0.  To be replaced by
 * the RTL's measured worst case. */
#define KSD_DMA_STOP_US	5000
/* DMA errors and stop failures before DMA is turned off for good, and
 * good DMA requests that forgive one. */
#define KSD_DMA_TROUBLE_MAX 3
#define KSD_DMA_TROUBLE_DECAY 1024

enum ksd_state { KSD_IDLE, KSD_SBC, KSD_CMD, KSD_STOP };

struct ksd_host {
	struct mmc_host *mmc;
	void __iomem *base;
	struct mutex lock;		/* everything below, and the registers */
	struct mmc_request *mrq;	/* in flight */
	enum ksd_state state;
	u64 acc;			/* sticky status seen for the current command */
	u64 irq_en;
	bool cmd_done;			/* KSD_CMD: the command's response is in */
	unsigned int moved;		/* blocks moved through the FIFO */
	int pio_err;			/* the buffer ended before the data */
	bool dma_cap;			/* SDM_CAPS[10], and DMA not turned off */
	bool dma;			/* this request's data is mapped and moves by DMA */
	bool dma_reset;			/* stopping the DMA reset the engine */
	bool req_dma;			/* this request's data phase is by DMA */
	bool unsent;			/* nothing of this request reached the controller */
	unsigned int dma_trouble;	/* DMA errors and stop failures */
	unsigned int dma_good;		/* good DMA requests, for the decay */
	enum dma_data_direction dma_dir;
	dma_addr_t dma_start;		/* its SDM_DMA_ADDR */
	u32 data_tmo;			/* SDM_TIMEOUT[31:0] for this request */
	u32 busy_tmo;			/* SDM_TIMEOUT[63:32] */
	unsigned long deadline;		/* watchdog, jiffies */
	struct delayed_work watchdog;
	/* Bus state (set_ios). */
	u32 div;
	unsigned long clk_hz;
	u64 cfg;
	bool powered, init_clocks;
	unsigned long off_since;	/* jiffies when the slot was powered off */
};

static u64 rd(struct ksd_host *h, unsigned int off)
{
	return readq(h->base + off);
}

static void wr(struct ksd_host *h, unsigned int off, u64 v)
{
	writeq(v, h->base + off);
}

static void ksd_write_ctrl(struct ksd_host *h)
{
	wr(h, SD_CTRL, CTRL_SD_MODE | CTRL_CS_HI | (h->powered ? CTRL_PWR_ON : 0) |
	   FIELD_PREP(CTRL_DIV, h->div));
}

static u32 ksd_clocks(struct ksd_host *h, u64 ns)
{
	u64 c = div_u64(ns * h->clk_hz, NSEC_PER_SEC) + 1;

	return min_t(u64, c, U32_MAX);
}

static u32 ksd_ms_clocks(struct ksd_host *h, unsigned int ms)
{
	return ksd_clocks(h, (u64)ms * NSEC_PER_MSEC);
}

static void ksd_write_timeout(struct ksd_host *h)
{
	wr(h, SDM_TIMEOUT, (u64)h->busy_tmo << 32 | h->data_tmo);
}

/* ---- PIO ---- */

struct ksd_pio {
	struct sg_mapping_iter m;
	unsigned int off;		/* bytes used of m's current chunk */
};

/* The next bytes of the request's buffer, *avail of them; NULL at its end. */
static u8 *ksd_pio_ptr(struct ksd_pio *p, unsigned int *avail)
{
	if (!p->m.addr || p->off == p->m.length) {
		if (p->m.addr)
			p->m.consumed = p->off;
		if (!sg_miter_next(&p->m))
			return NULL;
		p->off = 0;
	}
	*avail = p->m.length - p->off;
	return p->m.addr + p->off;
}

/* len bytes to or from the buffer, across a chunk boundary. */
static bool ksd_pio_bytes(struct ksd_pio *p, u8 *b, unsigned int len, bool to_sg)
{
	unsigned int got = 0, avail, n;
	u8 *q;

	while (got < len) {
		q = ksd_pio_ptr(p, &avail);
		if (!q)
			return false;
		n = min(avail, len - got);
		if (to_sg)
			memcpy(q, b + got, n);
		else
			memcpy(b + got, q, n);
		p->off += n;
		got += n;
	}
	return true;
}

/*
 * One block between the FIFO and the buffer.  Each FIFO word is the next 8
 * bytes, little-endian: byte 0 in bits [7:0].  A block takes ceil(size / 8)
 * words and starts a new word; with size mod 8 = 4 its last word carries
 * 4 bytes in [31:0] ([63:32] reads 0, and is ignored on a push).
 */
static bool ksd_pio_block(struct ksd_host *h, struct ksd_pio *p, unsigned int blksz, bool to_buf)
{
	void __iomem *fifo = h->base + SDM_DATA;
	unsigned int w, avail, n;
	u8 b[8], *q;

	/*
	 * The usual case: the whole block fits in the buffer's current chunk
	 * at an aligned address, so it moves as aligned doublewords.  (The
	 * unaligned accessors below are byte at a time on this CPU: 64-bit
	 * accesses must be aligned.)
	 */
	q = ksd_pio_ptr(p, &avail);
	if (q && avail >= blksz && !((unsigned long)q & 7) && !(blksz & 31)) {
		u64 *d = (u64 *)q;

		if (to_buf) {
			for (w = 0; w < blksz / 8; w += 4, d += 4) {
				d[0] = readq_relaxed(fifo);
				d[1] = readq_relaxed(fifo);
				d[2] = readq_relaxed(fifo);
				d[3] = readq_relaxed(fifo);
			}
		} else {
			for (w = 0; w < blksz / 8; w += 4, d += 4) {
				writeq_relaxed(d[0], fifo);
				writeq_relaxed(d[1], fifo);
				writeq_relaxed(d[2], fifo);
				writeq_relaxed(d[3], fifo);
			}
		}
		p->off += blksz;
		return true;
	}

	for (w = 0; w < DIV_ROUND_UP(blksz, 8); w++) {
		n = min(blksz - w * 8, 8u);
		if (n < 8) {
			/* The half word at the end of a 4-mod-8 block. */
			if (to_buf) {
				put_unaligned_le64(readq_relaxed(fifo), b);
				if (!ksd_pio_bytes(p, b, n, true))
					return false;
			} else {
				memset(b, 0, sizeof(b));
				if (!ksd_pio_bytes(p, b, n, false))
					return false;
				writeq_relaxed(get_unaligned_le64(b), fifo);
			}
			continue;
		}
		q = ksd_pio_ptr(p, &avail);
		if (!q)
			return false;
		if (avail >= 8) {
			if (to_buf)
				put_unaligned_le64(readq_relaxed(fifo), q);
			else
				writeq_relaxed(get_unaligned_le64(q), fifo);
			p->off += 8;
		} else if (to_buf) {
			put_unaligned_le64(readq_relaxed(fifo), b);
			if (!ksd_pio_bytes(p, b, 8, true))
				return false;
		} else {
			if (!ksd_pio_bytes(p, b, 8, false))
				return false;
			writeq_relaxed(get_unaligned_le64(b), fifo);
		}
	}
	return true;
}

/*
 * Move blocks while FIFO_THRESH says a whole one can move (read: one is in
 * the FIFO; write: there is room for one), up to limit blocks in all.
 */
static int ksd_pio(struct ksd_host *h, unsigned int limit)
{
	struct mmc_data *data = h->mrq->data;
	bool to_buf = data->flags & MMC_DATA_READ;
	struct ksd_pio p = { };
	int err = 0;

	if (h->moved >= limit)
		return 0;
	sg_miter_start(&p.m, data->sg, data->sg_len,
		       SG_MITER_ATOMIC | (to_buf ? SG_MITER_TO_SG : SG_MITER_FROM_SG));
	if (h->moved && !sg_miter_skip(&p.m, (off_t)h->moved * data->blksz))
		err = -EIO;
	while (!err && h->moved < limit && (rd(h, SDM_STATUS) & ST_THRESH)) {
		if (!ksd_pio_block(h, &p, data->blksz, to_buf))
			err = -EIO;	/* the buffer is shorter than the data */
		else
			h->moved++;
	}
	if (p.m.addr)
		p.m.consumed = p.off;
	sg_miter_stop(&p.m);
	if (h->moved == data->blocks && (h->irq_en & ST_THRESH)) {
		h->irq_en &= ~ST_THRESH;
		wr(h, SDM_IRQ_EN, h->irq_en);
	}
	return err;
}

/* ---- DMA ---- */

/*
 * The engine can take data: one segment, whole FIFO words per block (a
 * 4-mod-8 block's last word is half used), DMA_ADDR's 8-byte alignment,
 * and a read must own every cache line it writes (ARCH_DMA_MINALIGN), as
 * the invalidate after it drops them whole.
 */
static bool ksd_dma_usable(struct ksd_host *h, struct mmc_data *data)
{
	unsigned int len = data->blocks * data->blksz;
	unsigned int align = (data->flags & MMC_DATA_READ) ? ARCH_DMA_MINALIGN : 8;
	struct scatterlist *sg = data->sg;

	if (!h->dma_cap || !READ_ONCE(ksd_dma_param))
		return false;
	if (data->sg_len != 1 || sg->length < len || data->blksz % 8)
		return false;
	return !(sg->offset % align) && !(len % align);
}

/*
 * A DMA error or a stop failure: after KSD_DMA_TROUBLE_MAX of them, DMA is
 * turned off (max_segs stays 1: the core can't take a new one after
 * mmc_add_host, and PIO handles one segment as well).  Every
 * KSD_DMA_TROUBLE_DECAY good DMA requests forgive one.
 */
static void ksd_dma_trouble(struct ksd_host *h)
{
	if (++h->dma_trouble == KSD_DMA_TROUBLE_MAX && h->dma_cap) {
		h->dma_cap = false;
		dev_err(mmc_dev(h->mmc), "%u DMA errors or stop failures: DMA off, PIO from now on\n",
			h->dma_trouble);
	}
}

/* Wait for DMA_BUSY to drop: 0, or -ETIMEDOUT; *st the last SDM_STATUS. */
static int ksd_dma_wait(struct ksd_host *h, u64 *st)
{
	return readq_poll_timeout_atomic(h->base + SDM_STATUS, *st, !(*st & ST_DMA_BUSY),
					 1, KSD_DMA_STOP_US);
}

/*
 * Stop the engine, whatever it is doing: DMA_EN = 0 (which aborts a DMA in
 * flight) and wait for DMA_BUSY to drop.  DMA_BUSY = 0 means every DDR
 * write the engine issued has been accepted at the coherent point, no
 * further access will be issued, and SDM_DMA_ADDR is final, whichever way
 * the DMA ended (spec v1.2 m1): after it a buffer may be unmapped (the
 * invalidate then shows the CPU the data) and SDM_DMA_ADDR read.  If
 * DMA_EN = 0 does not stop it, a FIFO reset (which also aborts a DMA) and
 * then leaving SD mode (which resets the native engine, spec v1.1 item 9)
 * are tried; an engine that still won't stop would go on writing memory
 * the kernel owns (a buffer about to be freed, or whatever a loader left
 * it pointed at): panic rather than corrupt it.  True if more than
 * DMA_EN = 0 was needed.  (SDM_DMA_ADDR is not read while DMA_BUSY: it
 * is not final then.)
 */
static bool ksd_engine_stop(struct ksd_host *h, const char *when)
{
	char buf[32];
	u64 st;

	h->cfg &= ~CFG_DMA_EN;
	wr(h, SDM_CFG, h->cfg);
	if (!ksd_dma_wait(h, &st))
		return false;
	dev_err(mmc_dev(h->mmc), "DMA did not stop %s (STATUS %llx): FIFO reset\n", when, st);
	wr(h, SDM_FIFO, FIFO_RESET);
	if (!ksd_dma_wait(h, &st))
		return true;
	dev_err(mmc_dev(h->mmc), "DMA still busy (STATUS %llx): resetting the native engine\n", st);
	wr(h, SD_CTRL, rd(h, SD_CTRL) & ~CTRL_SD_MODE);
	if (!ksd_dma_wait(h, &st)) {
		/* Back into SD mode. */
		ksd_write_ctrl(h);
		wr(h, SDM_CFG, h->cfg);
		wr(h, SDM_IRQ_EN, h->irq_en);
		return true;
	}
	h->dma_cap = false;
	/* dma_start is the address of the request now live, if there is one
	 * (h->dma); otherwise the engine's target is unknown. */
	if (h->dma)
		snprintf(buf, sizeof(buf), "buffer %llx", (u64)h->dma_start);
	else
		snprintf(buf, sizeof(buf), "buffer address unknown");
	panic("klausscpu-sd: the SD DMA engine does not stop %s (STATUS %llx, %s): it could write freed memory\n",
	      when, st, buf);
}

/*
 * Map the buffer and point the engine at it; the data command starts the
 * DMA.  The mapping skips dma-direct's cache sync and this cleans the
 * buffer itself (klausscpu_dma_clean), which says whether the clean
 * finished.  False, with nothing mapped, if the engine was busy (it is
 * stopped first), the mapping is not the buffer itself (a bounce buffer
 * would need the sync), the clean timed out, or SDM_DMA_ADDR did not
 * take: the request goes by PIO.
 */
static bool ksd_dma_start(struct ksd_host *h, struct mmc_data *data)
{
	struct scatterlist *sg = data->sg;
	unsigned int len = data->blocks * data->blksz;	/* what ksd_dma_usable() validated */

	if (rd(h, SDM_STATUS) & ST_DMA_BUSY) {
		/* Can't happen: ksd_dma_stop() leaves it idle or panics. */
		ksd_engine_stop(h, "at a new request");
		ksd_dma_stop_failures++;
		ksd_dma_trouble(h);
		return false;
	}
	h->dma_dir = mmc_get_dma_dir(data);
	if (dma_map_sg_attrs(mmc_dev(h->mmc), sg, data->sg_len, h->dma_dir, DMA_ATTR_SKIP_CPU_SYNC) != 1)
		return false;
	h->dma_start = sg_dma_address(sg);
	if (h->dma_start != sg_phys(sg) || !klausscpu_dma_clean(sg_phys(sg), len))
		goto unmap;
	/* The address register ignores writes while DMA_BUSY and reads back
	 * the live address: check that it took. */
	wr(h, SDM_DMA_ADDR, h->dma_start);
	if (rd(h, SDM_DMA_ADDR) != h->dma_start) {
		dev_err_ratelimited(mmc_dev(h->mmc), "SDM_DMA_ADDR did not take %llx: PIO\n",
				    (u64)h->dma_start);
		goto unmap;
	}
	h->cfg |= CFG_DMA_EN;
	wr(h, SDM_CFG, h->cfg);
	h->dma = true;
	return true;
unmap:
	dma_unmap_sg_attrs(mmc_dev(h->mmc), sg, data->sg_len, h->dma_dir, DMA_ATTR_SKIP_CPU_SYNC);
	return false;
}

/* The request's DMA must be over: stop the engine (dma_reset if it took a
 * reset: the request fails). */
static void ksd_dma_halt(struct ksd_host *h)
{
	if (!h->dma || !ksd_engine_stop(h, "at the end of a request"))
		return;
	ksd_dma_stop_failures++;
	ksd_dma_trouble(h);
	h->dma_reset = true;
}

/*
 * The request's DMA is over, or must be: halt the engine, invalidate the
 * buffer for the CPU after a device write (klausscpu_dma_inval, which also
 * sees to the I-cache), and unmap it.  A D-cache invalidate that timed out
 * may leave stale lines: the request fails (dma_reset).
 */
static void ksd_dma_stop(struct ksd_host *h)
{
	struct mmc_data *data = h->mrq->data;
	struct scatterlist *sg;
	unsigned int len;

	if (!h->dma)
		return;
	sg = data->sg;
	len = data->blocks * data->blksz;
	ksd_dma_halt(h);
	if (h->dma_dir != DMA_TO_DEVICE && !klausscpu_dma_inval(sg_phys(sg), len))
		h->dma_reset = true;
	dma_unmap_sg_attrs(mmc_dev(h->mmc), sg, data->sg_len, h->dma_dir, DMA_ATTR_SKIP_CPU_SYNC);
	h->dma = false;
}

/* Blocks the DMA got into DDR (a read) or fetched from it (a write). */
static unsigned int ksd_dma_blocks(struct ksd_host *h, struct mmc_data *data)
{
	u64 at = rd(h, SDM_DMA_ADDR);

	if (at < h->dma_start)
		return 0;
	return min_t(u64, div_u64(at - h->dma_start, data->blksz), data->blocks);
}

/* Abandon the data phase's data path: the DMA, then the FIFO. */
static void ksd_data_abort(struct ksd_host *h)
{
	ksd_dma_stop(h);
	wr(h, SDM_FIFO, FIFO_RESET);
}

/* ---- commands ---- */

/*
 * Start cmd.  The controller ignores SDM_CMD while CMD_BUSY is set, and a
 * data command while DATA_BUSY is (plan 4.8; other commands, CMD12
 * included, may run during a data phase): refuse (-EBUSY) rather than take
 * a later completion for this command's.  It also ignores a data command
 * while DMA_BUSY (plan §8.3: after a CMD12 or an abort the engine may still
 * be finishing its block with DATA_BUSY clear); that is never the case
 * here, as every DMA request ends in ksd_dma_stop(), which waits DMA_BUSY
 * out (or panics), and ksd_dma_start() checks it again.
 */
static int ksd_send(struct ksd_host *h, struct mmc_command *cmd)
{
	u32 flags = mmc_resp_type(cmd);
	u64 busy = ST_CMD_BUSY;
	unsigned int type;
	u64 v;

	if (cmd->data)
		busy |= ST_DATA_BUSY;
	if (rd(h, SDM_STATUS) & busy) {
		dev_err(mmc_dev(h->mmc), "CMD%u: controller still busy (STATUS %llx)\n",
			cmd->opcode, rd(h, SDM_STATUS));
		cmd->error = -EBUSY;
		return -EBUSY;
	}

	if (!(flags & MMC_RSP_PRESENT))
		type = RESP_NONE;
	else if (flags & MMC_RSP_136)
		type = RESP_136;
	else if (flags & MMC_RSP_BUSY)
		type = RESP_48B;
	else if (!(flags & MMC_RSP_CRC))
		type = RESP_48_NOCRC;
	else
		type = RESP_48;

	v = FIELD_PREP(CMD_INDEX, cmd->opcode);
	h->irq_en = ST_CMD_DONE | ST_CMD_ERRS;
	if (cmd->opcode == MMC_STOP_TRANSMISSION) {
		/* Ends an open-ended data phase cleanly; always R1b (plan 4.2). */
		type = RESP_48B;
		v |= CMD_STOP;
	}
	v |= FIELD_PREP(CMD_RESP, type);
	if (cmd->data) {
		v |= CMD_DATA;
		if (cmd->data->flags & MMC_DATA_WRITE)
			v |= CMD_WRITE;
		h->irq_en |= ST_DATA_DONE | ST_DATA_ERRS;
		if (!h->dma)
			h->irq_en |= ST_THRESH;
	} else if (type == RESP_48B) {
		h->irq_en |= ST_BUSY_TIMEOUT;
		h->busy_tmo = max(h->busy_tmo, ksd_ms_clocks(h, max(cmd->busy_timeout, KSD_BUSY_TMO_MS)));
		ksd_write_timeout(h);
	}

	h->acc = 0;
	wr(h, SDM_STATUS, ST_STICKY);
	wr(h, SDM_ARG, cmd->arg);
	wr(h, SDM_IRQ_EN, h->irq_en);
	wr(h, SDM_CMD, v);
	return 0;
}

/* The response and error of the command that just completed. */
static void ksd_cmd_result(struct ksd_host *h, struct mmc_command *cmd)
{
	u64 r0, r1;

	if (h->acc & ST_CMD_TIMEOUT)
		cmd->error = -ETIMEDOUT;
	else if (h->acc & (ST_CMD_CRC | ST_CMD_END | ST_CMD_IDX))
		cmd->error = -EILSEQ;
	else if (!cmd->data && (h->acc & ST_BUSY_TIMEOUT))
		cmd->error = -ETIMEDOUT;	/* R1b busy */
	if (cmd->error)
		ksd_cmd_errors++;
	if (cmd->error == -ETIMEDOUT && !(h->acc & ST_BUSY_TIMEOUT))
		return;
	if (cmd->flags & MMC_RSP_136) {
		r1 = rd(h, SDM_RESP1);
		r0 = rd(h, SDM_RESP0);
		cmd->resp[0] = r1 >> 32;
		cmd->resp[1] = r1;
		cmd->resp[2] = r0 >> 32;
		cmd->resp[3] = r0;
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = rd(h, SDM_RESP0);
	}
}

static int ksd_data_error(u64 st)
{
	if (st & (ST_DATA_TIMEOUT | ST_BUSY_TIMEOUT | ST_WRITE_NOTOK))
		return -ETIMEDOUT;
	if (st & (ST_DATA_CRC | ST_DATA_END | ST_WRITE_CRC))
		return -EILSEQ;
	return -EIO;			/* FIFO_ERR: a driver bug */
}

/* Ends the request: the caller completes the returned mrq unlocked. */
static struct mmc_request *ksd_finish(struct ksd_host *h)
{
	struct mmc_request *mrq = h->mrq;

	ksd_dma_stop(h);
	/* Every request with a data phase is counted here, once, by how the
	 * data phase ran (a DMA refused and sent again counts as PIO); one
	 * that was never sent (-EBUSY) counts as neither. */
	if (mrq->data && !h->unsent) {
		if (h->req_dma)
			ksd_dma_requests++;
		else
			ksd_pio_requests++;
	}
	h->irq_en = 0;
	wr(h, SDM_IRQ_EN, 0);
	h->mrq = NULL;
	h->state = KSD_IDLE;
	cancel_delayed_work(&h->watchdog);
	return mrq;
}

/* After the data phase: CMD12 if the core asked for one, else done. */
static struct mmc_request *ksd_end_data(struct ksd_host *h, bool error)
{
	struct mmc_request *mrq = h->mrq;

	ksd_dma_stop(h);		/* the data is in DDR: unmap it */
	if (h->dma_reset) {
		/* The engine had to be reset, or the invalidate failed: trust
		 * none of the data. */
		if (!mrq->data->error)
			mrq->data->error = -EIO;
		mrq->data->bytes_xfered = 0;
		error = true;
	} else if (h->req_dma && !error && h->dma_trouble && !(++h->dma_good % KSD_DMA_TROUBLE_DECAY)) {
		h->dma_trouble--;
	}
	if (mrq->stop && (error || !mrq->sbc)) {
		h->state = KSD_STOP;
		if (!ksd_send(h, mrq->stop))
			return NULL;
	}
	return ksd_finish(h);
}

/* Advance the request on the status seen so far; the mrq when it is done. */
static struct mmc_request *ksd_advance(struct ksd_host *h)
{
	struct mmc_request *mrq = h->mrq;
	struct mmc_data *data = mrq->data;
	unsigned int good;

	switch (h->state) {
	case KSD_SBC:
		if (!(h->acc & ST_CMD_DONE))
			return NULL;
		ksd_cmd_result(h, mrq->sbc);
		if (mrq->sbc->error)
			return ksd_finish(h);
		h->state = KSD_CMD;
		if (ksd_send(h, mrq->cmd))
			return ksd_finish(h);
		return NULL;

	case KSD_CMD:
		if (data && h->dma && !h->cmd_done && (h->acc & ST_DMA_ERR) && !(h->acc & ST_CMD_DONE)) {
			/*
			 * Refused up front (spec v1.1 items 2 and 5: a block size
			 * not a multiple of 8, or a range leaving DDR): DMA_ERR,
			 * no command reached the card and nothing moved.  Send it
			 * again by PIO.
			 */
			ksd_dma_errors++;
			dev_warn_once(mmc_dev(h->mmc), "CMD%u: DMA refused (DMA_ERR, STATUS %llx, buffer %llx, %u x %u bytes): sent again by PIO\n",
				      mrq->cmd->opcode, h->acc, (u64)h->dma_start, data->blocks, data->blksz);
			ksd_dma_trouble(h);
			ksd_dma_stop(h);
			if (h->dma_reset) {
				data->error = -EIO;
				return ksd_finish(h);
			}
			h->req_dma = false;
			wr(h, SDM_FIFO, FIFO_RESET);
			if (ksd_send(h, mrq->cmd))
				return ksd_finish(h);
			return NULL;
		}
		if (data && !h->dma && !h->pio_err)
			h->pio_err = ksd_pio(h, data->blocks);
		if (!h->cmd_done) {
			if (!(h->acc & ST_CMD_DONE))
				return NULL;
			h->cmd_done = true;
			ksd_cmd_result(h, mrq->cmd);
			if (mrq->cmd->error) {
				/* A response error aborts the data phase (plan 4.2). */
				if (data)
					ksd_data_abort(h);
				return ksd_finish(h);
			}
			if (!data)
				return ksd_finish(h);
		}
		if (h->pio_err) {
			/* The buffer is shorter than the data: stop the transfer
			 * now that the command is done (CMD12 ends the data phase). */
			data->error = h->pio_err;
			data->bytes_xfered = 0;
			wr(h, SDM_FIFO, FIFO_RESET);
			return ksd_end_data(h, true);
		}
		if (h->acc & ST_DATA_ERRS) {
			/* Good blocks before the error count, and a read's are
			 * still in the FIFO: take them, drop the rest. */
			good = min_t(u32, rd(h, SDM_BLKDONE) & 0xffff, data->blocks);
			data->error = ksd_data_error(h->acc);
			if (h->acc & ST_DMA_ERR) {
				ksd_dma_errors++;
				dev_warn_once(mmc_dev(h->mmc), "CMD%u: DMA_ERR during the transfer (STATUS %llx)\n",
					      mrq->cmd->opcode, h->acc);
				ksd_dma_trouble(h);
			}
			ksd_dma_halt(h);	/* SDM_DMA_ADDR is final */
			if (h->dma)
				/* A read's good blocks count once they are in DDR. */
				h->moved = (data->flags & MMC_DATA_READ) ?
					   min(good, ksd_dma_blocks(h, data)) : good;
			else if (data->flags & MMC_DATA_READ)
				ksd_pio(h, good);
			data->bytes_xfered = min(good, h->moved) * data->blksz;
			ksd_data_errors++;
			dev_warn_ratelimited(mmc_dev(h->mmc),
					     "CMD%u arg %08x: data error %d (STATUS %llx) after %u of %u blocks%s\n",
					     mrq->cmd->opcode, mrq->cmd->arg, data->error, h->acc,
					     good, data->blocks, h->dma ? " (DMA)" : "");
			ksd_data_abort(h);
			return ksd_end_data(h, true);
		}
		if (!(h->acc & ST_DATA_DONE))
			return NULL;
		if (h->dma) {
			ksd_dma_halt(h);	/* SDM_DMA_ADDR is final */
			h->moved = ksd_dma_blocks(h, data);
		}
		if (h->moved < data->blocks) {
			/* Done, but blocks short of the count: can't happen. */
			dev_err(mmc_dev(h->mmc), "CMD%u: DATA_DONE after %u of %u blocks%s\n",
				mrq->cmd->opcode, h->moved, data->blocks, h->dma ? " (DMA)" : "");
			data->error = -EIO;
			data->bytes_xfered = 0;
			ksd_data_abort(h);
			return ksd_end_data(h, true);
		}
		data->bytes_xfered = data->blocks * data->blksz;
		return ksd_end_data(h, false);

	case KSD_STOP:
		if (!(h->acc & ST_CMD_DONE))
			return NULL;
		ksd_cmd_result(h, mrq->stop);
		return ksd_finish(h);

	case KSD_IDLE:
		break;
	}
	return NULL;
}

static irqreturn_t ksd_irq(int irq, void *dev_id)
{
	struct ksd_host *h = dev_id;
	struct mmc_request *done = NULL;
	u64 st;

	mutex_lock(&h->lock);
	st = rd(h, SDM_STATUS);
	if (st & ST_STICKY)
		wr(h, SDM_STATUS, st & ST_STICKY);
	if (h->mrq) {
		h->acc |= st & ST_STICKY;
		done = ksd_advance(h);
	} else {
		wr(h, SDM_IRQ_EN, 0);	/* nothing in flight */
	}
	mutex_unlock(&h->lock);
	if (done)
		mmc_request_done(h->mmc, done);
	return IRQ_HANDLED;
}

/* A controller that never finishes a request fails it instead of hanging. */
static void ksd_watchdog(struct work_struct *work)
{
	struct ksd_host *h = container_of(to_delayed_work(work), struct ksd_host, watchdog);
	struct mmc_request *done = NULL;
	struct mmc_request *mrq;

	mutex_lock(&h->lock);
	mrq = h->mrq;
	if (mrq && time_after_eq(jiffies, h->deadline)) {
		ksd_timeouts++;
		dev_err(mmc_dev(h->mmc), "CMD%u timed out (state %d, STATUS %llx, BLKDONE %llu)\n",
			mrq->cmd->opcode, h->state, rd(h, SDM_STATUS), rd(h, SDM_BLKDONE));
		if (h->state == KSD_SBC)
			mrq->sbc->error = -ETIMEDOUT;
		else if (h->state == KSD_STOP)
			mrq->stop->error = -ETIMEDOUT;
		else if (!h->cmd_done || !mrq->data)
			mrq->cmd->error = -ETIMEDOUT;
		else
			mrq->data->error = -ETIMEDOUT;
		/* Stop a DMA, then clearing SD_MODE makes both engines abandon
		 * what they are doing (plan 4.8); the core's recovery starts
		 * from there. */
		ksd_dma_stop(h);
		wr(h, SD_CTRL, rd(h, SD_CTRL) & ~CTRL_SD_MODE);
		ksd_write_ctrl(h);
		wr(h, SDM_FIFO, FIFO_RESET);
		wr(h, SDM_STATUS, ST_STICKY);
		done = ksd_finish(h);
	}
	mutex_unlock(&h->lock);
	if (done)
		mmc_request_done(h->mmc, done);
}

/* ---- mmc_host_ops ---- */

static void ksd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ksd_host *h = mmc_priv(mmc);
	struct mmc_data *data = mrq->data;
	struct mmc_request *done = NULL;
	unsigned int ms = 3000;

	mutex_lock(&h->lock);
	/* No card-present check here: the card is non-removable (see probe),
	 * and CARD_PRESENT read 0 with a card in on mmu-sd c1e540a. */
	if (WARN_ON(h->mrq)) {
		mutex_unlock(&h->lock);
		mrq->cmd->error = -ENOMEDIUM;
		mmc_request_done(mmc, mrq);
		return;
	}
	if (data && (data->blksz % 4 || data->blksz < 4 || data->blksz > 2048)) {
		/* Block sizes are multiples of 4 from 4 to 2048 (plan 4.3, 57a5a8c):
		 * 4 is ACMD22's, the count of blocks a failed write got onto the
		 * card, which the core's write recovery asks for. */
		mutex_unlock(&h->lock);
		data->error = -EINVAL;
		mmc_request_done(mmc, mrq);
		return;
	}
	h->mrq = mrq;
	h->cmd_done = false;
	h->moved = 0;
	h->pio_err = 0;
	if (h->cfg & CFG_CLK_ON) {	/* the power-up clocks are over */
		h->cfg &= ~CFG_CLK_ON;
		wr(h, SDM_CFG, h->cfg);
	}

	h->data_tmo = ksd_ms_clocks(h, KSD_READ_TMO_MS);
	h->busy_tmo = ksd_ms_clocks(h, KSD_BUSY_TMO_MS);
	if (data) {
		u64 ns = data->timeout_ns + div_u64((u64)data->timeout_clks * NSEC_PER_SEC,
						    h->clk_hz);

		h->data_tmo = max(h->data_tmo, ksd_clocks(h, ns));
		h->busy_tmo = max(h->busy_tmo, ksd_clocks(h, ns));
		/* Empty for the data: a DMA read moves whatever is at the
		 * FIFO's head to DDR (plan §8.2). */
		wr(h, SDM_FIFO, FIFO_RESET);
		wr(h, SDM_BLK, FIELD_PREP(BLK_SIZE, data->blksz) |
			       FIELD_PREP(BLK_COUNT, data->blocks));
		ms += data->blocks * (KSD_BUSY_TMO_MS / 2);
		h->dma = false;
		h->dma_reset = false;
		h->req_dma = ksd_dma_usable(h, data) && ksd_dma_start(h, data);
	}
	ksd_write_timeout(h);
	ms += max(mrq->cmd->busy_timeout, mrq->stop ? mrq->stop->busy_timeout : 0);
	h->deadline = jiffies + msecs_to_jiffies(ms);
	mod_delayed_work(system_wq, &h->watchdog, msecs_to_jiffies(ms) + 1);

	h->state = mrq->sbc ? KSD_SBC : KSD_CMD;
	h->unsent = false;
	if (ksd_send(h, mrq->sbc ? mrq->sbc : mrq->cmd)) {
		h->unsent = true;
		done = ksd_finish(h);
	}
	mutex_unlock(&h->lock);
	if (done)
		mmc_request_done(mmc, done);
}

static void ksd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ksd_host *h = mmc_priv(mmc);
	long left;

	mutex_lock(&h->lock);
	if (ios->clock) {
		h->div = clamp_t(u32, DIV_ROUND_UP(KSD_REF_HZ, 2 * ios->clock), 1, 0x10000) - 1;
		h->clk_hz = KSD_REF_HZ / (2 * (h->div + 1));
		mmc->actual_clock = h->clk_hz;
		mmc->max_busy_timeout = div_u64((u64)U32_MAX * MSEC_PER_SEC, h->clk_hz);
	}

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		if (h->powered) {
			h->powered = false;
			h->off_since = jiffies;
		}
		h->cfg &= ~CFG_CLK_ON;
		break;
	case MMC_POWER_UP:
		if (!h->powered) {
			/* Off long enough to reset a card left in SPI mode. */
			left = (long)(h->off_since + msecs_to_jiffies(KSD_POWER_OFF_MS) + 1 - jiffies);
			if (left > 0)
				msleep(jiffies_to_msecs(left));
			h->powered = true;
			h->init_clocks = true;
		}
		break;
	case MMC_POWER_ON:
		if (h->init_clocks) {
			/* >= 74 clocks with CMD high before CMD0: the core waits
			 * power_delay_ms with the clock running; the first
			 * request stops it. */
			h->init_clocks = false;
			h->cfg |= CFG_CLK_ON;
		}
		break;
	default:
		break;
	}

	if (ios->bus_width == MMC_BUS_WIDTH_4)
		h->cfg |= CFG_4BIT;
	else
		h->cfg &= ~CFG_4BIT;
	ksd_write_ctrl(h);
	wr(h, SDM_CFG, h->cfg);
	mutex_unlock(&h->lock);
}

static int ksd_get_cd(struct mmc_host *mmc)
{
	struct ksd_host *h = mmc_priv(mmc);

	return !!(rd(h, SDM_STATUS) & ST_CARD);
}

static const struct mmc_host_ops ksd_ops = {
	.request	= ksd_request,
	.set_ios	= ksd_set_ios,
	.get_cd		= ksd_get_cd,
};

/* ---- probe ---- */

bool klausscpu_sd_use_native(void)
{
	return sd_native && (klausscpu_cpu_features & CPU_FEAT_SDNATIVE) &&
	       readq((void __iomem *)(SD_BASE + SDM_CAPS)) != 0;
}

static int ksd_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct ksd_host *h;
	u64 caps;
	int irq, err;

	mmc = devm_mmc_alloc_host(&pdev->dev, sizeof(*h));
	if (!mmc)
		return -ENOMEM;
	h = mmc_priv(mmc);
	h->mmc = mmc;
	h->base = (void __iomem *)SD_BASE;
	h->dma = false;
	mutex_init(&h->lock);
	INIT_DELAYED_WORK(&h->watchdog, ksd_watchdog);
	caps = rd(h, SDM_CAPS);
	/* DDR is below 4 GiB; the engine takes any 8-byte aligned address. */
	h->dma_cap = (caps & CAPS_DMA) && !dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));

	/*
	 * Native mode with the slot off: the card may still be in SPI mode
	 * from the boot loader or a previous program, and only a power cycle
	 * leaves it.  set_ios(MMC_POWER_UP) waits out the rest of the time.
	 */
	h->div = KSD_DIV_INIT;
	h->clk_hz = KSD_REF_HZ / (2 * (h->div + 1));
	h->powered = false;
	h->off_since = jiffies;
	ksd_write_ctrl(h);
	wr(h, SDM_IRQ_EN, 0);
	wr(h, SDM_STATUS, ST_STICKY);
	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_CFG, 0);
	/* A DMA a loader left running may still be writing memory the kernel
	 * now owns: DMA_EN = 0 above aborts it; make sure it stopped. */
	if (rd(h, SDM_STATUS) & ST_DMA_BUSY) {
		ksd_engine_stop(h, "at probe");
		/* Whatever it wrote since the kernel started may be stale in
		 * the I-cache. */
		klausscpu_dma_stray_write();
	}

	mmc->ops = &ksd_ops;
	mmc->f_min = DIV_ROUND_UP(KSD_REF_HZ, 2 * 0x10000);
	mmc->f_max = KSD_F_MAX;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	/* Non-removable: the driver doesn't poll for card changes (insert the
	 * card before boot), and so the mmc core never consults get_cd, which
	 * also keeps it independent of SDM_STATUS CARD_PRESENT (0 with a card
	 * in on mmu-sd c1e540a). */
	mmc->caps = MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_CMD23 | MMC_CAP_NONREMOVABLE;
	if (caps & CAPS_4BIT)
		mmc->caps |= MMC_CAP_4_BIT_DATA;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	mmc->max_busy_timeout = div_u64((u64)U32_MAX * MSEC_PER_SEC, h->clk_hz);
	mmc->max_blk_size = 2048;
	mmc->max_blk_count = 65535;
	mmc->max_req_size = 1024 * 1024;
	mmc->max_seg_size = mmc->max_req_size;
	/* The DMA moves one contiguous range per command; with dma=0 at
	 * probe, PIO keeps its scatter-gather (a later dma=1 then takes the
	 * one-segment requests only). */
	mmc->max_segs = (h->dma_cap && ksd_dma_param) ? 1 : 256;

	irq = klausscpu_int2_irq(KL_INT2_SD);
	if (!irq)
		return -ENXIO;
	err = devm_request_threaded_irq(&pdev->dev, irq, NULL, ksd_irq, IRQF_ONESHOT,
					"klausscpu-sd", h);
	if (err)
		return err;
	err = mmc_add_host(mmc);
	if (err)
		return err;
	dev_info(&pdev->dev, "native SD controller v%u, %u-block FIFO, %s, IRQ %d\n",
		 (unsigned int)FIELD_GET(CAPS_VERSION, caps),
		 (unsigned int)FIELD_GET(CAPS_FIFO_BLKS, caps),
		 (caps & CAPS_4BIT) ? "4-bit" : "1-bit", irq);
	if (h->dma_cap)
		dev_info(&pdev->dev, "DMA %s, D-cache %s\n", ksd_dma_param ? "on" : "off (dma=0)",
			 klausscpu_dcache_range_ops() ? "range ops" : "whole-cache flushes");
	return 0;
}

static struct platform_driver ksd_driver = {
	.probe	= ksd_probe,
	.driver	= {
		.name			= "klausscpu-sdnative",
		.suppress_bind_attrs	= true,
	},
};

static int __init ksd_init(void)
{
	struct platform_device *pdev;
	int err;

	if (!klausscpu_sd_use_native())
		return 0;
	err = platform_driver_register(&ksd_driver);
	if (err)
		return err;
	pdev = platform_device_register_simple("klausscpu-sdnative", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&ksd_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}
device_initcall(ksd_init);
