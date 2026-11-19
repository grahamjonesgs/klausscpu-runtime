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
 *
 * High speed (SDM_CAPS[9], SD_NATIVE_PLAN.md §9): the host offers
 * MMC_CAP_SD_HIGHSPEED and 50 MHz, the mmc core switches the card with
 * CMD6 at the identification clock (f_init, 400 kHz: mmc_sd_switch_hs
 * comes before mmc_set_clock), and set_ios at MMC_TIMING_SD_HS and 50 MHz
 * writes
 * clk_div 0 and SDM_CFG's HS_SAMPLE with a sample phase and IDELAY tap
 * (only while no command, data or DMA runs; every SDM_CFG write carries
 * them, as each reloads the tap).  The sample point is calibrated in the
 * core's SD high-speed tuning hooks (ksd_hs_calibrate): each phase at taps
 * 0/8/16/24 reads the card's CMD6 status (R1 on CMD, 64 bytes on DAT,
 * both CRC-checked) until a try fails, and the setting is the centre of
 * the widest passing window of the sample instant modulo the 20 ns clock
 * (klaussCPU tools/board/sd_hs_test.c's pick()), at phase p + 4 rather
 * than p <= 2 when both pass: they sample the same instant, but at p0/p1 a
 * slow card's eye can put the sample in the bit before, which loses the
 * next start bit when CLK parks.  The first sweep runs on the 1-bit bus
 * before ACMD6 goes out at 50 MHz, the second, which is logged, on the
 * 4-bit bus, and its setting must then read 16 blocks with CLK parked
 * before each, else the next candidates and the hs_phase/hs_tap default
 * are tried the same way.  If nothing works high speed is given up for
 * good (ksd_hs_give_up): the card is switched back to default speed (at
 * 12.5 MHz, confirmed by its status) and the host runs at 25 MHz (12.5
 * MHz if the switch can't be confirmed); on the card's first
 * initialisation the core
 * then initialises it again from scratch, on a re-initialisation it goes
 * on at 25 MHz.  klausscpu_sd.hs=0 keeps 25 MHz; hs_calibrate=0 uses
 * hs_phase/hs_tap without a sweep (still verified).  Without SDM_CAPS[9]
 * nothing here changes.
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
#include <linux/mmc/sd.h>
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

static bool ksd_hs_param = true;
module_param_named(hs, ksd_hs_param, bool, 0444);
MODULE_PARM_DESC(hs, "Use 50 MHz high speed where the controller has it (SDM_CAPS[9]; else 25 MHz)");

static bool ksd_hs_cal_param = true;
module_param_named(hs_calibrate, ksd_hs_cal_param, bool, 0444);
MODULE_PARM_DESC(hs_calibrate, "Calibrate the high-speed sample point (else use hs_phase and hs_tap)");

/* The board's centre (SD_NATIVE_PLAN.md §9.7, sdhs_5a6b2aa: phase 2, tap
 * 8) at the later of its two equivalent phases: SDM_CFG 0x863. */
static unsigned int ksd_hs_phase_param = 6;
module_param_named(hs_phase, ksd_hs_phase_param, uint, 0444);
MODULE_PARM_DESC(hs_phase, "High-speed sample phase before calibration, and its fallback (0-6)");

static unsigned int ksd_hs_tap_param = 8;
module_param_named(hs_tap, ksd_hs_tap_param, uint, 0444);
MODULE_PARM_DESC(hs_tap, "High-speed IDELAY tap before calibration, and its fallback (0-31)");

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
#define  ST_IDELAY_RDY	BIT(6)		/* the delay line is calibrated */
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
#define  CFG_HS_SAMPLE	BIT(1)		/* sample at HS_PHASE (plan §9.2) */
#define  CFG_CLK_ON	BIT(2)
#define  CFG_HS_PHASE	GENMASK(7, 4)	/* 0-6: 10 + 5p ns after CLK rises */
#define  CFG_IDELAY_TAP	GENMASK(12, 8)	/* 0-31: 78 ps earlier each */
#define  CFG_HS_BITS	(CFG_HS_SAMPLE | CFG_HS_PHASE | CFG_IDELAY_TAP)
#define  CFG_DMA_EN	BIT(16)		/* sampled when SDM_CMD is written */
#define SDM_FIFO	0x148
#define  FIFO_LEVEL	GENMASK(9, 0)	/* words */
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
#define  CAPS_IDELAY	BIT(11)		/* the input delay line */
#define  CAPS_FIFO_BLKS	GENMASK(23, 16)
#define  CAPS_HS_PHASES	GENMASK(27, 24)

#define KSD_REF_HZ	100000000	/* CLK = 100 MHz / (2 (div + 1)) */
#define KSD_DIV_INIT	124		/* 400 kHz */
#define KSD_F_MAX	25000000	/* default speed */
#define KSD_F_HS	50000000	/* high speed: clk_div 0 */
/* clk_div 3: the default sample point (HS_SAMPLE = 0) is F, the fabric
 * edge that drives CLK low (F - 20 + 5p with p = 4, plan §9.3): R + 20 ns at
 * 25 MHz, before a late high-speed eye opens (plan §9.4: the worst case
 * opens at 20.9 ns); at 12.5 MHz F is R + 40 ns, inside both a high-speed
 * card's eye and a default-speed one's. */
#define KSD_F_SAFE	12500000
/* Slot power off before a power-up: >= 10 ms, to be measured (plan 4.1). */
#define KSD_POWER_OFF_MS 20
#define KSD_READ_TMO_MS	100		/* the reset values' 100 ms / 500 ms */
#define KSD_BUSY_TMO_MS	500
/* The wait for each step of stopping the engine (DMA_EN, FIFO reset,
 * SD_MODE).  DMA_EN = 0 lets the engine finish the block it is moving: up
 * to a block's 16-byte DDR transactions (32 for 512 B), one bus tenure
 * each, so tens of microseconds typically.  Measured on the RTL (sddma
 * 3ae5ed6, socsdabort: VGA, blitter, a miss-heavy CPU loop and whole-cache
 * walks, 40 random aborts per case): at most 19,105 cycles (191 us); the
 * bound, every transaction behind a full VGA burst or a walk, is 32 x
 * ~2,800 cycles, about 0.9 ms.  So 2 ms before escalating.  A FIFO reset
 * stops it after the transaction in flight (measured at most 2,691 cycles,
 * 27 us).  Only an aborted transfer waits here: after a normal one
 * DMA_BUSY is already 0. */
#define KSD_DMA_STOP_US	2000
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
	/* High speed: SDM_CAPS[9] and hs=1; cleared if calibration fails. */
	bool hs_cap;
	bool hs_delay;			/* SDM_CAPS[11]: IDELAY taps */
	unsigned int hs_phases;		/* SDM_CAPS[27:24] */
	bool hs_on;			/* MMC_TIMING_SD_HS at clk_div 0 */
	unsigned int hs_phase, hs_tap;	/* the sample point in use */
	unsigned int fifo_blks;		/* SDM_CAPS[23:16], 512-byte blocks */
	bool hs_tuning;			/* the prepare hook ran, the execute hook not yet */
	unsigned int f_cap;		/* set_ios's highest clock */
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
				/* A response error aborts the data phase (plan 4.2).
				 * A DMA write's engine starts fetching at command
				 * accept, so the FIFO may hold its block in progress
				 * (plan 8.4): stopped, and the FIFO reset, before the
				 * core's retry. */
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

/* CLK = 100 MHz / (2 (div + 1)), the fastest not above hz. */
static void ksd_set_clock(struct ksd_host *h, unsigned int hz)
{
	h->div = clamp_t(u32, DIV_ROUND_UP(KSD_REF_HZ, 2 * hz), 1, 0x10000) - 1;
	h->clk_hz = KSD_REF_HZ / (2 * (h->div + 1));
	h->mmc->actual_clock = h->clk_hz;
	h->mmc->max_busy_timeout = div_u64((u64)U32_MAX * MSEC_PER_SEC, h->clk_hz);
}

/* SDM_CFG's sampling bits: HS_SAMPLE at the chosen phase and tap at 50 MHz
 * high speed; 0 (the default-speed sample point, tap 0) otherwise. */
static u64 ksd_hs_bits(struct ksd_host *h)
{
	if (!h->hs_on)
		return 0;
	return CFG_HS_SAMPLE | FIELD_PREP(CFG_HS_PHASE, h->hs_phase) |
	       FIELD_PREP(CFG_IDELAY_TAP, h->hs_tap);
}

/* No command, data phase or DMA running: the only time SDM_CFG's sampling
 * bits may change (plan §9.2: a change mid-frame can drop or repeat a
 * sample, a tap change can glitch the delayed lines).  True if idle. */
static bool ksd_wait_idle(struct ksd_host *h)
{
	u64 st;

	return !readq_poll_timeout_atomic(h->base + SDM_STATUS, st,
					  !(st & (ST_CMD_BUSY | ST_DATA_BUSY | ST_DMA_BUSY)),
					  1, 1000);
}

static void ksd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ksd_host *h = mmc_priv(mmc);
	long left;

	mutex_lock(&h->lock);
	/* At most f_cap: 50 MHz with high speed, else 25 MHz (12.5 MHz
	 * after a switch back to default speed that could not be
	 * confirmed); after ksd_hs_drop() the core may still ask for 50 MHz. */
	if (ios->clock)
		ksd_set_clock(h, min(ios->clock, h->f_cap));

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
			/* A power cycle returns the card to default speed, so a
			 * 12.5 MHz cap from an unconfirmed switch back (ksd_hs_drop)
			 * is no longer needed. */
			if (!h->hs_cap && h->f_cap == KSD_F_SAFE)
				h->f_cap = mmc->f_max = KSD_F_MAX;
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
	/* High speed samples at the calibrated phase and tap once the clock
	 * is 50 MHz (clk_div 0); legacy timing, or a slower clock, at the
	 * default-speed point with tap 0, as without high speed.  The data
	 * and busy timeouts are counted in clocks from clk_hz (ksd_request),
	 * so at 50 MHz they double in clocks and stay the same in time. */
	h->hs_on = h->hs_cap && ios->timing == MMC_TIMING_SD_HS && h->div == 0;
	if ((h->cfg ^ ksd_hs_bits(h)) & CFG_HS_BITS) {
		if (!ksd_wait_idle(h))
			dev_warn(mmc_dev(mmc), "sampling changed while busy (STATUS %llx)\n",
				 rd(h, SDM_STATUS));
		h->cfg = (h->cfg & ~CFG_HS_BITS) | ksd_hs_bits(h);
	}
	ksd_write_ctrl(h);
	wr(h, SDM_CFG, h->cfg);
	mutex_unlock(&h->lock);
}

/* ---- high-speed calibration ---- */

/* The IDELAY taps tried at each phase (the board test's, less 31). */
static const u8 ksd_cal_taps[] = { 0, 8, 16, 24 };
#define KSD_CAL_NTAPS	ARRAY_SIZE(ksd_cal_taps)
#define KSD_CAL_PHASES	16		/* SDM_CAPS[27:24] at most */
/* Good CMD6 reads that make a setting clean (a sweep stops at the first
 * failure, so a bad setting costs one try). */
#define KSD_CAL_TRIES	4
/* Polls: CMD_DONE (an R1b's busy included, up to SDM_TIMEOUT's busy
 * timeout), and DATA_DONE (up to its data timeout, the 100 ms read access
 * limit the core also uses); each a margin above the hardware's. */
#define KSD_CAL_CMD_US	((KSD_BUSY_TMO_MS + 100) * USEC_PER_MSEC)
#define KSD_CAL_DATA_US	((KSD_READ_TMO_MS + 50) * USEC_PER_MSEC)
/* A try's result besides SDM_STATUS's error bits. */
#define KSD_CAL_HUNG	BIT_ULL(62)	/* no CMD_DONE / DATA_DONE in time */
#define KSD_CAL_BAD	BIT_ULL(63)	/* a good CRC, the wrong content */
#define KSD_HS_PERIOD_PS 20000		/* 50 MHz */
/* Candidates a failed parked verify moves on to before the default. */
#define KSD_CAL_MORE	3

/* A command, polled (no interrupt during calibration): the error bits, 0
 * if it went well. */
static u64 ksd_cal_cmd(struct ksd_host *h, u32 opcode, u32 arg, u32 type, u64 flags)
{
	u64 st;

	wr(h, SDM_STATUS, ST_STICKY);
	wr(h, SDM_ARG, arg);
	wr(h, SDM_CMD, FIELD_PREP(CMD_INDEX, opcode) | FIELD_PREP(CMD_RESP, type) | flags);
	if (readq_poll_timeout(h->base + SDM_STATUS, st, st & ST_CMD_DONE, 2, KSD_CAL_CMD_US))
		return KSD_CAL_HUNG;
	return st & (ST_CMD_ERRS | ST_BUSY_TIMEOUT);
}

/* Wait for a data phase to end: 0 at DATA_DONE, else the error bits. */
static u64 ksd_cal_data_end(struct ksd_host *h)
{
	u64 st;

	if (readq_poll_timeout(h->base + SDM_STATUS, st, st & (ST_DATA_DONE | ST_DATA_ERRS),
			       2, KSD_CAL_DATA_US))
		return KSD_CAL_HUNG;
	return st & ST_DATA_ERRS;
}

/* CMD6 with arg and its 64-byte status into b, by PIO: 0 if both came in
 * good. */
static u64 ksd_cal_switch(struct ksd_host *h, u32 arg, u8 b[64])
{
	u64 e;
	int i;

	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_BLK, FIELD_PREP(BLK_SIZE, 64) | FIELD_PREP(BLK_COUNT, 1));
	e = ksd_cal_cmd(h, SD_SWITCH, arg, RESP_48, CMD_DATA);
	if (!e)
		e = ksd_cal_data_end(h);
	if (e)
		return e;
	if (FIELD_GET(FIFO_LEVEL, rd(h, SDM_FIFO)) < 64 / 8)
		return KSD_CAL_BAD;
	for (i = 0; i < 64 / 8; i++)
		put_unaligned_le64(readq_relaxed(h->base + SDM_DATA), b + 8 * i);
	return 0;
}

/*
 * One try at the sample point in SDM_CFG: CMD6 in mode 0 with no change
 * (0x00FFFFFF), whose R1 tests the CMD line and whose 64-byte status the
 * DAT lines, both under CRC, read by PIO.  The status must say that high
 * speed is supported (byte 13 [1]) and selected (byte 16 [3:0] = 1).
 * CMD6 rather than CMD13 or a block read, so that the sweep leaves
 * klausscc --sd-fault's command and block counts alone.  0 if good.
 */
static u64 ksd_cal_try(struct ksd_host *h)
{
	u8 b[64];
	u64 e;

	e = ksd_cal_switch(h, 0x00ffffff, b);
	if (e)
		return e;
	if (!(b[13] & SD_MODE_HIGH_SPEED) || (b[16] & 0xf) != HIGH_SPEED_BUS_SPEED)
		return KSD_CAL_BAD;
	return 0;
}

/*
 * A setting's check, as klaussCPU sd_hs_test.c's rd_parked(): CMD18 of
 * twice the FIFO's blocks from block 0, each block drained only once the
 * FIFO is full (or holds every block still to come).  So CLK parks after
 * each of blocks fifo_blks .. 2 fifo_blks - 1 with the FIFO full, and each
 * of the last fifo_blks blocks follows a restart (a sample point a bit
 * early loses the next start bit then: DATA_CRC_ERR and DATA_END_ERR,
 * BLKDONE short, while commands and single blocks pass).  Then CMD12.  0
 * if good.
 */
static u64 ksd_cal_verify(struct ksd_host *h)
{
	unsigned int got, want, w, blks = 2 * h->fifo_blks;
	u64 st, n, e;

	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_BLK, FIELD_PREP(BLK_SIZE, 512) | FIELD_PREP(BLK_COUNT, blks));
	e = ksd_cal_cmd(h, MMC_READ_MULTIPLE_BLOCK, 0, RESP_48, CMD_DATA);
	if (e)
		return e;
	for (got = 0; got < blks; got++) {
		/* Let the FIFO fill (CLK parks), then take one block. */
		want = min(blks - got, h->fifo_blks) * (512 / 8);
		if (readq_poll_timeout(h->base + SDM_FIFO, n,
				       FIELD_GET(FIFO_LEVEL, n) >= want ||
				       (rd(h, SDM_STATUS) & ST_DATA_ERRS),
				       2, KSD_CAL_DATA_US))
			return KSD_CAL_HUNG;
		st = rd(h, SDM_STATUS);
		if (st & ST_DATA_ERRS)
			return st & ST_DATA_ERRS;
		for (w = 0; w < 512 / 8; w++)
			readq_relaxed(h->base + SDM_DATA);
	}
	e = ksd_cal_data_end(h);
	if (e)
		return e;
	if ((rd(h, SDM_BLKDONE) & 0xffff) != blks)
		return KSD_CAL_BAD;
	return ksd_cal_cmd(h, MMC_STOP_TRANSMISSION, 0, RESP_48B, CMD_STOP);
}

/* Free-running clock for a few us (a card finishing a block it was
 * sending), then an engine still busy is reset by leaving SD mode, and the
 * FIFO and the status are cleared. */
static void ksd_cal_settle(struct ksd_host *h)
{
	wr(h, SDM_CFG, h->cfg | CFG_CLK_ON);
	udelay(20);
	wr(h, SDM_CFG, h->cfg);
	if (!ksd_wait_idle(h)) {
		wr(h, SD_CTRL, rd(h, SD_CTRL) & ~CTRL_SD_MODE);
		ksd_write_ctrl(h);
		wr(h, SDM_CFG, h->cfg);
	}
	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_STATUS, ST_STICKY);
}

/*
 * After a failed try: the card hears commands at any phase (the host's
 * outputs do not depend on it), so CMD12 stops a block it may still be
 * sending (a card already back in tran takes it as illegal, which the next
 * R1 reports: see ksd_hs_calibrate), then ksd_cal_settle().
 */
static void ksd_cal_recover(struct ksd_host *h)
{
	ksd_cal_cmd(h, MMC_STOP_TRANSMISSION, 0, RESP_48B, CMD_STOP);
	ksd_cal_settle(h);
}

/* Sample at phase p, tap t (idle). */
static void ksd_hs_set(struct ksd_host *h, unsigned int p, unsigned int t)
{
	h->hs_phase = p;
	h->hs_tap = t;
	h->cfg = (h->cfg & ~CFG_HS_BITS) | ksd_hs_bits(h);
	wr(h, SDM_CFG, h->cfg);
}

/* The sample instant of phase p and tap t, in ps after CLK's rising edge,
 * modulo the 20 ns clock (plan §9.3): phase p and p + 4 are the same
 * instant one bit apart. */
static s32 ksd_hs_instant(unsigned int p, unsigned int t)
{
	s32 ps = 10000 + 5000 * (s32)min(p, 6U) - 78 * (s32)t;

	return ((ps % KSD_HS_PERIOD_PS) + KSD_HS_PERIOD_PS) % KSD_HS_PERIOD_PS;
}

/* The distance of instant t from centre, modulo the clock. */
static s32 ksd_hs_dist(s32 t, s32 centre)
{
	s32 d = (t - centre + KSD_HS_PERIOD_PS) % KSD_HS_PERIOD_PS;

	return d > KSD_HS_PERIOD_PS / 2 ? KSD_HS_PERIOD_PS - d : d;
}

/* The sweep's results: clean[ti][p], every try passed. */
struct ksd_cal {
	bool clean[KSD_CAL_NTAPS][KSD_CAL_PHASES];
	unsigned int ntap, nph;
};

/*
 * The window, as klaussCPU tools/board/sd_hs_test.c's pick(): the widest
 * gap between failing instants (modulo 20 ns) that holds passing ones.
 * Its centre in ps (-1 if no setting is clean), and *width_o its width.
 */
static s32 ksd_hs_centre(const struct ksd_cal *c, s32 *width_o)
{
	s32 tps[KSD_CAL_NTAPS * KSD_CAL_PHASES], centre = -1, width = 0, d, nxt;
	bool ok[KSD_CAL_NTAPS * KSD_CAL_PHASES], any, nok = false;
	unsigned int i, j, ti, p, np = 0, nfail = 0;

	for (ti = 0; ti < c->ntap; ti++) {
		for (p = 0; p < c->nph; p++) {
			tps[np] = ksd_hs_instant(p, ksd_cal_taps[ti]);
			ok[np] = c->clean[ti][p];
			nok |= ok[np];
			nfail += !ok[np];
			np++;
		}
	}
	if (nfail == 0) {
		*width_o = KSD_HS_PERIOD_PS;	/* everything passes */
		return 15000;
	}
	for (i = 0; i < np && nok; i++) {
		if (ok[i])
			continue;
		nxt = KSD_HS_PERIOD_PS;		/* to the next failing instant */
		for (j = 0; j < np; j++) {
			d = (tps[j] - tps[i] + KSD_HS_PERIOD_PS) % KSD_HS_PERIOD_PS;
			if (!ok[j] && j != i && d > 0 && d < nxt)
				nxt = d;
		}
		any = false;
		for (j = 0; j < np; j++) {
			d = (tps[j] - tps[i] + KSD_HS_PERIOD_PS) % KSD_HS_PERIOD_PS;
			if (ok[j] && d > 0 && d < nxt)
				any = true;
		}
		if (any && nxt > width) {
			width = nxt;
			centre = (tps[i] + nxt / 2) % KSD_HS_PERIOD_PS;
		}
	}
	*width_o = width;
	return centre;
}

/*
 * The setting pick() takes: the clean instant nearest the centre (the
 * smaller tap, then the lower phase, on a tie), moved from p <= 2 to p + 4
 * where that is clean too: they sample the same instant, but at p0/p1 a
 * slow card's eye can put the sample in the bit before, which loses the
 * next start bit when CLK parks (p3 has no p7).  False if none.
 */
static bool ksd_hs_pick(const struct ksd_cal *c, s32 centre, unsigned int *bp, unsigned int *bt)
{
	unsigned int ti, p, bti = 0;
	s32 d, bestd = S32_MAX;

	for (ti = 0; ti < c->ntap && centre >= 0; ti++) {
		for (p = 0; p < c->nph; p++) {
			if (!c->clean[ti][p])
				continue;
			d = ksd_hs_dist(ksd_hs_instant(p, ksd_cal_taps[ti]), centre);
			if (d < bestd) {
				bestd = d;
				*bp = p;
				bti = ti;
			}
		}
	}
	if (bestd == S32_MAX)
		return false;
	*bt = ksd_cal_taps[bti];
	if (*bp <= 2 && *bp + 4 < c->nph && c->clean[bti][*bp + 4])
		*bp += 4;
	return true;
}

/*
 * After a failed parked verify: the next clean setting nearest the centre
 * that is not a p <= 2 alone (p >= 3, or p <= 2 taken as its clean p + 4),
 * other than the n already tried in (tp[], tt[]).  False if none.
 */
static bool ksd_hs_next(const struct ksd_cal *c, s32 centre, const unsigned int *tp,
			const unsigned int *tt, unsigned int n, unsigned int *bp, unsigned int *bt)
{
	unsigned int ti, p, q, k;
	s32 d, bestd = S32_MAX;

	for (ti = 0; ti < c->ntap && centre >= 0; ti++) {
		for (p = 0; p < c->nph; p++) {
			if (!c->clean[ti][p])
				continue;
			q = p;
			if (p <= 2) {
				if (p + 4 >= c->nph || !c->clean[ti][p + 4])
					continue;
				q = p + 4;
			}
			for (k = 0; k < n && (tp[k] != q || tt[k] != ksd_cal_taps[ti]); k++)
				;
			if (k < n)
				continue;
			d = ksd_hs_dist(ksd_hs_instant(p, ksd_cal_taps[ti]), centre);
			if (d < bestd) {
				bestd = d;
				*bp = q;
				*bt = ksd_cal_taps[ti];
			}
		}
	}
	return bestd != S32_MAX;
}

/* SDM_TIMEOUT's 100 ms / 500 ms at the current clock. */
static void ksd_cal_timeouts(struct ksd_host *h)
{
	wr(h, SDM_TIMEOUT, (u64)ksd_ms_clocks(h, KSD_BUSY_TMO_MS) << 32 |
			   ksd_ms_clocks(h, KSD_READ_TMO_MS));
}

/*
 * High speed does not work: default speed from now on (hs_cap and
 * MMC_CAP_SD_HIGHSPEED cleared, so no later initialisation tries again).
 * The card, which the core's CMD6 switched, goes back to default speed by
 * CMD6 mode 1 function 0 (0x80FFFFF0), sent at 12.5 MHz (clk_div 3) with
 * the default-speed sample point (SDM_CFG [1] and [12:4] = 0), which is
 * inside both a high-speed and a default-speed card's eye: so its R1 and
 * its 64-byte status are read, and the status must report function 0
 * (byte 16 [3:0]).  One more try after a failure.  Confirmed: 25 MHz from
 * then on; not (the card may still be in high speed): 12.5 MHz.  Then
 * CMD13 takes the ILLEGAL_COMMAND a recovery's CMD12 may leave.  The core
 * still has MMC_TIMING_SD_HS and 50 MHz in its ios: set_ios clamps the
 * clock to f_cap and samples at the default point without hs_cap.  True if
 * the card confirmed default speed.
 */
static bool ksd_hs_drop(struct ksd_host *h, struct mmc_card *card)
{
	struct mmc_host *mmc = h->mmc;
	bool ok = false;
	u8 b[64];
	int i;

	h->hs_cap = false;
	mmc->caps &= ~MMC_CAP_SD_HIGHSPEED;
	h->hs_on = false;
	if (!ksd_wait_idle(h))
		ksd_cal_recover(h);
	h->cfg &= ~CFG_HS_BITS;
	ksd_set_clock(h, KSD_F_SAFE);
	ksd_write_ctrl(h);
	wr(h, SDM_CFG, h->cfg);
	ksd_cal_timeouts(h);
	for (i = 0; i < 2 && !ok; i++) {
		ok = !ksd_cal_switch(h, 0x80fffff0, b) && (b[16] & 0xf) == 0;
		if (!ok)
			ksd_cal_recover(h);
	}
	h->f_cap = ok ? KSD_F_MAX : KSD_F_SAFE;
	mmc->f_max = h->f_cap;
	ksd_set_clock(h, h->f_cap);
	ksd_write_ctrl(h);
	ksd_cal_timeouts(h);
	ksd_cal_cmd(h, MMC_SEND_STATUS, card->rca << 16, RESP_48, 0);
	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_STATUS, ST_STICKY);
	return ok;
}

/*
 * Give up on high speed (ksd_hs_drop) and say so.  On the card's first
 * initialisation (no mmc->card yet) -EIO: the core powers the slot off
 * and initialises the card again from scratch, now without high speed, so
 * its log says "new SDHC card".  On a re-initialisation (mmc_sd_hw_reset
 * after a block error, or a resume), where nothing would retry, 0: this
 * initialisation goes on at 25 MHz with the card at default speed.
 */
static int ksd_hs_give_up(struct ksd_host *h, struct mmc_card *card, const char *why)
{
	bool first = !h->mmc->card;

	if (ksd_hs_drop(h, card))
		dev_err(mmc_dev(h->mmc), "high speed: %s: 25 MHz default speed from now on%s\n",
			why, first ? " (the card is initialised again)" : "");
	else
		dev_err(mmc_dev(h->mmc), "high speed: %s; the card did not confirm the switch back to default speed: 12.5 MHz from now on%s\n",
			why, first ? " (the card is initialised again)" : "");
	h->hs_tuning = false;
	return first ? -EIO : 0;
}

/*
 * Calibrate the high-speed sample point (the host is claimed, no request
 * runs), in the core's prepare (final = false: 1-bit bus, before ACMD6 at
 * 50 MHz) and execute (final: 4-bit) hooks.  Every phase at every tap
 * (tap 0 only if the delay line is absent or not ready) until a try
 * fails; then pick()'s setting.  final: that setting must pass the parked
 * verify, else up to KSD_CAL_MORE more candidates (ksd_hs_next), else the
 * hs_phase / hs_tap default (also when no setting is clean), verified the
 * same way; if nothing passes, ksd_hs_give_up().  With hs_calibrate=0 no
 * sweep: the default, verified in final.  The prepare hook sets
 * hs_tuning and the execute hook clears it, so a prepare that finds it
 * set follows an initialisation that failed between them (ACMD6 at the
 * prepare's unverified setting): high speed is given up then.
 */
static int ksd_hs_calibrate(struct mmc_host *mmc, struct mmc_card *card, bool final)
{
	struct ksd_host *h = mmc_priv(mmc);
	struct ksd_cal c = { };
	unsigned int ti, p, r, bp = 0, bt = 0, def_p, def_t, n = 0;
	unsigned int tp[KSD_CAL_MORE + 2], tt[KSD_CAL_MORE + 2];
	char map[KSD_CAL_NTAPS * (KSD_CAL_PHASES + 1)] = "-", *m = map;
	const char *how = "";
	bool failed = false, passed = false, idelay;
	s32 centre = -1, width = 0;
	int ret = 0;
	u64 e;

	mutex_lock(&h->lock);
	if (!h->hs_on) {
		if (final)
			h->hs_tuning = false;
		goto out;
	}
	if (!final && h->hs_tuning) {
		ret = ksd_hs_give_up(h, card, "the last initialisation failed after calibration");
		goto out;
	}
	h->hs_tuning = !final;
	idelay = h->hs_delay && (rd(h, SDM_STATUS) & ST_IDELAY_RDY);
	if (h->hs_delay && !idelay)
		dev_warn(mmc_dev(mmc), "high speed: IDELAY_RDY is 0 (the delay line is not calibrated): tap 0 only\n");
	c.ntap = idelay ? KSD_CAL_NTAPS : 1;
	c.nph = h->hs_phases;
	def_p = min(ksd_hs_phase_param, c.nph - 1);
	def_t = idelay ? min(ksd_hs_tap_param, 31U) : 0;
	wr(h, SDM_IRQ_EN, 0);
	ksd_cal_timeouts(h);

	if (ksd_hs_cal_param) {
		for (ti = 0; ti < c.ntap; ti++) {
			for (p = 0; p < c.nph; p++) {
				ksd_hs_set(h, p, ksd_cal_taps[ti]);
				for (r = 0; r < KSD_CAL_TRIES; r++) {
					e = ksd_cal_try(h);
					if (e) {
						dev_dbg(mmc_dev(mmc), "phase %u tap %u: try %u failed (%llx)\n",
							p, ksd_cal_taps[ti], r, e);
						failed = true;
						ksd_cal_recover(h);
						break;
					}
				}
				c.clean[ti][p] = r == KSD_CAL_TRIES;
				passed |= r > 0;
				*m++ = c.clean[ti][p] ? 'P' : r ? '0' + r : '.';
			}
			*m++ = ti + 1 < c.ntap ? ' ' : '\0';
		}
		centre = ksd_hs_centre(&c, &width);
		if (!ksd_hs_pick(&c, centre, &bp, &bt)) {
			if (!passed) {
				ret = ksd_hs_give_up(h, card, "no sample point reads the card at 50 MHz");
				goto out;
			}
			dev_warn(mmc_dev(mmc), "high speed: no clean sample point (map %s, P = %u good reads): trying phase %u, tap %u\n",
				 map, KSD_CAL_TRIES, def_p, def_t);
			bp = def_p;
			bt = def_t;
			how = " (the default point)";
		}
	} else {
		bp = def_p;
		bt = def_t;
		how = " (hs_calibrate=0)";
	}
	ksd_hs_set(h, bp, bt);

	/* The 4-bit setting must also read across a CLK park; else the next
	 * candidates, then the default. */
	while (final) {
		e = ksd_cal_verify(h);
		if (!e)
			break;
		failed = true;
		ksd_cal_recover(h);
		tp[n] = bp;
		tt[n] = bt;
		n++;
		dev_warn(mmc_dev(mmc), "high speed: phase %u, tap %u fails a %u-block read across CLK parks (%llx)\n",
			 bp, bt, 2 * h->fifo_blks, e);
		for (r = 0; r < n && (tp[r] != def_p || tt[r] != def_t); r++)
			;
		if (n <= KSD_CAL_MORE && ksd_hs_next(&c, centre, tp, tt, n, &bp, &bt)) {
			how = " (a later candidate)";
		} else if (r == n) {
			bp = def_p;		/* the default, not tried yet */
			bt = def_t;
			how = " (the default point)";
		} else {
			ret = ksd_hs_give_up(h, card, "no sample point reads the card across a CLK park");
			goto out;
		}
		ksd_hs_set(h, bp, bt);
	}

	/* A CMD12 to a card already in tran latched ILLEGAL_COMMAND for the
	 * next R1: take it here, at the chosen setting. */
	if (failed)
		ksd_cal_cmd(h, MMC_SEND_STATUS, card->rca << 16, RESP_48, 0);
	wr(h, SDM_STATUS, ST_STICKY);
	if (final) {
		h->hs_tuning = false;
		dev_info(mmc_dev(mmc), "50 MHz high speed: phase %u, tap %u (SDM_CFG %#llx)%s, window %d.%d ns; map (phases 0-%u at taps %s): %s\n",
			 h->hs_phase, h->hs_tap, h->cfg & (CFG_4BIT | CFG_HS_BITS), how,
			 width / 1000, width % 1000 / 100, c.nph - 1,
			 idelay ? "0/8/16/24" : "0", map);
	} else {
		dev_dbg(mmc_dev(mmc), "1-bit calibration: phase %u, tap %u%s, map %s\n",
			h->hs_phase, h->hs_tap, how, map);
	}
out:
	mutex_unlock(&h->lock);
	return ret;
}

/* At 50 MHz on the 1-bit bus, before ACMD6 goes out at that speed. */
static int ksd_prepare_sd_hs_tuning(struct mmc_host *mmc, struct mmc_card *card)
{
	return ksd_hs_calibrate(mmc, card, false);
}

/* On the 4-bit bus: the setting kept, verified across a CLK park. */
static int ksd_execute_sd_hs_tuning(struct mmc_host *mmc, struct mmc_card *card)
{
	return ksd_hs_calibrate(mmc, card, true);
}

static int ksd_get_cd(struct mmc_host *mmc)
{
	struct ksd_host *h = mmc_priv(mmc);

	return !!(rd(h, SDM_STATUS) & ST_CARD);
}

static const struct mmc_host_ops ksd_ops = {
	.request		= ksd_request,
	.set_ios		= ksd_set_ios,
	.get_cd			= ksd_get_cd,
	.prepare_sd_hs_tuning	= ksd_prepare_sd_hs_tuning,
	.execute_sd_hs_tuning	= ksd_execute_sd_hs_tuning,
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
	h->cfg = 0;
	h->fifo_blks = max_t(unsigned int, FIELD_GET(CAPS_FIFO_BLKS, caps), 1);
	ksd_write_ctrl(h);
	wr(h, SDM_IRQ_EN, 0);
	wr(h, SDM_STATUS, ST_STICKY);
	wr(h, SDM_FIFO, FIFO_RESET);
	wr(h, SDM_CFG, h->cfg);
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
	h->hs_cap = (caps & CAPS_HS) && ksd_hs_param;
	h->f_cap = h->hs_cap ? KSD_F_HS : KSD_F_MAX;
	if (h->hs_cap) {
		h->hs_phases = clamp_t(unsigned int, FIELD_GET(CAPS_HS_PHASES, caps), 1, KSD_CAL_PHASES);
		h->hs_delay = caps & CAPS_IDELAY;
		h->hs_phase = min(ksd_hs_phase_param, h->hs_phases - 1);
		h->hs_tap = h->hs_delay ? min(ksd_hs_tap_param, 31U) : 0;
		mmc->caps |= MMC_CAP_SD_HIGHSPEED;
		mmc->f_max = KSD_F_HS;
	}
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
	if (caps & CAPS_HS)
		dev_info(&pdev->dev, "high speed %s: %u sample phases, %s, IDELAY_RDY %llu\n",
			 h->hs_cap ? "on" : "off (hs=0)", (unsigned int)FIELD_GET(CAPS_HS_PHASES, caps),
			 (caps & CAPS_IDELAY) ? "IDELAY taps" : "no delay line",
			 (rd(h, SDM_STATUS) & ST_IDELAY_RDY) >> 6);
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
