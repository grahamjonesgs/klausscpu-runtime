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
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
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
#define  ST_STICKY	(ST_CMD_DONE | ST_DATA_DONE | GENMASK(26, 16))
#define  ST_CMD_ERRS	(ST_CMD_TIMEOUT | ST_CMD_CRC | ST_CMD_END | ST_CMD_IDX)
#define  ST_DATA_ERRS	(GENMASK(24, 20) | ST_BUSY_TIMEOUT | ST_FIFO_ERR)
#define SDM_IRQ_EN	0x130
#define SDM_TIMEOUT	0x138		/* [31:0] data, [63:32] busy, in clocks */
#define SDM_CFG		0x140
#define  CFG_4BIT	BIT(0)
#define  CFG_CLK_ON	BIT(2)
#define SDM_FIFO	0x148
#define  FIFO_RESET	BIT(31)
#define SDM_BLKDONE	0x158
#define SDM_DATA	0x180
#define SDM_CAPS	0x1f8
#define  CAPS_VERSION	GENMASK(7, 0)
#define  CAPS_4BIT	BIT(8)
#define  CAPS_HS	BIT(9)
#define  CAPS_FIFO_BLKS	GENMASK(23, 16)

#define KSD_REF_HZ	100000000	/* CLK = 100 MHz / (2 (div + 1)) */
#define KSD_DIV_INIT	124		/* 400 kHz */
#define KSD_F_MAX	25000000	/* default speed; high speed is phase 3 */
/* Slot power off before a power-up: >= 10 ms, to be measured (plan 4.1). */
#define KSD_POWER_OFF_MS 20
#define KSD_READ_TMO_MS	100		/* the reset values' 100 ms / 500 ms */
#define KSD_BUSY_TMO_MS	500

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

/* ---- commands ---- */

/*
 * Start cmd.  The controller ignores SDM_CMD while CMD_BUSY is set, and a
 * data command while DATA_BUSY is (plan 4.8; other commands, CMD12
 * included, may run during a data phase): refuse (-EBUSY) rather than take
 * a later completion for this command's.
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
		h->irq_en |= ST_DATA_DONE | ST_DATA_ERRS | ST_THRESH;
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
		if (data && !h->pio_err)
			h->pio_err = ksd_pio(h, data->blocks);
		if (!h->cmd_done) {
			if (!(h->acc & ST_CMD_DONE))
				return NULL;
			h->cmd_done = true;
			ksd_cmd_result(h, mrq->cmd);
			if (mrq->cmd->error) {
				/* A response error aborts the data phase (plan 4.2). */
				if (data)
					wr(h, SDM_FIFO, FIFO_RESET);
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
			if (data->flags & MMC_DATA_READ)
				ksd_pio(h, good);
			data->bytes_xfered = min(good, h->moved) * data->blksz;
			wr(h, SDM_FIFO, FIFO_RESET);
			return ksd_end_data(h, true);
		}
		if (!(h->acc & ST_DATA_DONE))
			return NULL;
		if (h->moved < data->blocks) {
			/* Done, but blocks short of the count: can't happen. */
			data->error = -EIO;
			data->bytes_xfered = h->moved * data->blksz;
			wr(h, SDM_FIFO, FIFO_RESET);
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
		/* Clearing SD_MODE makes both engines abandon what they are
		 * doing (plan 4.8); the core's recovery starts from there. */
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
	if (WARN_ON(h->mrq) || !(rd(h, SDM_STATUS) & ST_CARD)) {
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
		wr(h, SDM_FIFO, FIFO_RESET);
		wr(h, SDM_BLK, FIELD_PREP(BLK_SIZE, data->blksz) |
			       FIELD_PREP(BLK_COUNT, data->blocks));
		ms += data->blocks * (KSD_BUSY_TMO_MS / 2);
	}
	ksd_write_timeout(h);
	ms += max(mrq->cmd->busy_timeout, mrq->stop ? mrq->stop->busy_timeout : 0);
	h->deadline = jiffies + msecs_to_jiffies(ms);
	mod_delayed_work(system_wq, &h->watchdog, msecs_to_jiffies(ms) + 1);

	h->state = mrq->sbc ? KSD_SBC : KSD_CMD;
	if (ksd_send(h, mrq->sbc ? mrq->sbc : mrq->cmd))
		done = ksd_finish(h);
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
	mutex_init(&h->lock);
	INIT_DELAYED_WORK(&h->watchdog, ksd_watchdog);
	caps = rd(h, SDM_CAPS);

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

	mmc->ops = &ksd_ops;
	mmc->f_min = DIV_ROUND_UP(KSD_REF_HZ, 2 * 0x10000);
	mmc->f_max = KSD_F_MAX;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_CMD23;
	if (caps & CAPS_4BIT)
		mmc->caps |= MMC_CAP_4_BIT_DATA;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	mmc->max_busy_timeout = div_u64((u64)U32_MAX * MSEC_PER_SEC, h->clk_hz);
	mmc->max_blk_size = 2048;
	mmc->max_blk_count = 65535;
	mmc->max_req_size = 1024 * 1024;
	mmc->max_seg_size = mmc->max_req_size;
	mmc->max_segs = 256;

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
