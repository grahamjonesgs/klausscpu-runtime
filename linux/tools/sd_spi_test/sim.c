// SPDX-License-Identifier: GPL-2.0
/*
 * Host test for linux/arch/klausscpu/kernel/sd_spi.c: the driver's protocol
 * code, unchanged, against a model of the board's SD controller and an
 * SPI-mode SD card.
 *
 * Controller: a DATA write starts a transfer that completes XFER_T register
 * accesses later; STATUS bit 0 is busy until then; DATA reads return the
 * last completed byte.  A "stall" (an instruction-fetch miss on the board)
 * lets time jump before a DATA or STATUS read, so the pipelined loop can
 * read the latch after the next byte has replaced it: it must notice (and
 * retry), never return the wrong byte.
 *
 * Card: CMD0/8/55/ACMD41/58/16/9, CMD17/18 (with CMD12), CMD24/25 (0xFC
 * blocks, 0xFD stop, which is ignored while busy as real cards do), ACMD23,
 * random access latency and busy periods, and injected faults: rejected
 * commands, data error tokens, rejected writes.
 *
 * Checks, per operation: the data read equals the reference, the card's
 * whole contents equal the reference (no write outside the request), the
 * card is idle with CS high afterwards, and no protocol violation (a byte
 * started while the bus was busy, a command while the card was busy, CS
 * raised mid-transfer, a stop token while busy).
 */
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "sd_spi.h"

#define NSECT		4096		/* a 2 MiB card */
#define XFER_T		4		/* register accesses per byte */
#define JIFFY_ACC	20000		/* register accesses per jiffy */

volatile unsigned long jiffies;
int model_verbose;
unsigned long model_log_lines;

static u8 store[NSECT][512], ref[NSECT][512];
static unsigned long violations;
static const char *last_violation;

static void violation(const char *what)
{
	violations++;
	last_violation = what;
	if (model_verbose)
		fprintf(stderr, "VIOLATION: %s\n", what);
}

static u32 rnd(u32 n)
{
	return n ? (u32)(random() % n) : 0;
}

/* Fault rates, 1 in N (0: never). */
static struct {
	u32 stall;		/* a DATA or STATUS read stalls */
	u32 cmd_reject;		/* CMD17/18/24/25 rejected */
	u32 token_err;		/* a read block's token is an error token */
	u32 wr_reject;		/* a written block's data response is an error */
	bool absent;		/* no card: MISO stays high */
	bool zero_regs;		/* no controller: registers read 0 */
} f;

static unsigned long stalls, injected;

/* ---- the card ---- */

enum cstate { C_IDLE, C_RD_MULTI, C_WR_WAIT, C_WR_DATA };

static struct {
	enum cstate st;
	bool sdsc, inited, app, multi_wr, wr_rejected;
	int acmd41;
	u8 cmd[6];
	int ncmd;
	u8 q[4096];
	unsigned int qh, qt;
	int busy;		/* bytes of 0x00 still to send */
	u32 rd_lba, wr_lba;
	u8 wbuf[514];
	int nw;
} c;

static void put(u8 b)
{
	c.q[c.qt++ % sizeof(c.q)] = b;
}

static void put_block(u32 lba)
{
	int i, lat = rnd(12);

	for (i = 0; i < lat; i++)
		put(0xff);
	if (f.token_err && !rnd(f.token_err)) {
		injected++;
		put(0x08);		/* out of range / error token */
		return;
	}
	put(0xfe);
	for (i = 0; i < 512; i++)
		put(store[lba][i]);
	put(rnd(256));
	put(rnd(256));
}

static u32 addr(u32 arg)
{
	return c.sdsc ? arg / 512 : arg;
}

static bool reject(void)
{
	if (f.cmd_reject && !rnd(f.cmd_reject)) {
		injected++;
		return true;
	}
	return false;
}

static void card_cmd(void)
{
	u8 cmd = c.cmd[0] & 0x3f;
	u32 arg = (u32)c.cmd[1] << 24 | c.cmd[2] << 16 | c.cmd[3] << 8 | c.cmd[4];
	bool app = c.app;
	int i;

	c.app = false;
	if (c.st == C_RD_MULTI) {
		if (cmd != 12) {
			violation("command other than CMD12 during a multi-block read");
			return;
		}
		c.qh = c.qt;		/* the stream stops */
		put(rnd(256));		/* stuff byte */
		put(0x00);		/* R1 */
		c.busy = rnd(20);	/* R1b */
		c.st = C_IDLE;
		return;
	}
	put(0xff);			/* NCR: one byte before the response */
	switch (cmd) {
	case 0:
		c.inited = false;
		c.acmd41 = 0;
		put(0x01);
		return;
	case 8:
		put(0x01); put(0); put(0); put(0x01); put(0xaa);
		return;
	case 55:
		c.app = true;
		put(c.inited ? 0x00 : 0x01);
		return;
	case 41:
		if (app && ++c.acmd41 >= 3)
			c.inited = true;
		put(c.inited ? 0x00 : 0x01);
		return;
	case 58:
		put(0x00); put(c.sdsc ? 0x80 : 0xc0); put(0xff); put(0x80); put(0x00);
		return;
	case 16:
		put(arg == 512 ? 0x00 : 0x40);
		return;
	case 9: {
		u8 csd[16] = { 0 };

		if (c.sdsc) {	/* v1: (7+1) << (7+2) << 9 >> 9 = 4096 sectors */
			csd[5] = 0x09; csd[7] = 0x01; csd[8] = 0xc0; csd[9] = 0x03; csd[10] = 0x80;
		} else {	/* v2: (3+1) * 1024 sectors */
			csd[0] = 0x40; csd[9] = 3;
		}
		put(0x00); put(0xff); put(0xff); put(0xfe);
		for (i = 0; i < 16; i++)
			put(csd[i]);
		put(0); put(0);
		return;
	}
	case 23:
		put(app ? 0x00 : 0x04);
		return;
	case 17:
	case 18:
	case 24:
	case 25:
		if (!c.inited) {
			put(0x05);
			return;
		}
		if ((c.sdsc && arg % 512) || addr(arg) >= NSECT) {
			put(0x40);	/* parameter error */
			return;
		}
		if (reject()) {
			put(0x04);
			return;
		}
		put(0x00);
		if (cmd == 17) {
			put_block(addr(arg));
		} else if (cmd == 18) {
			c.rd_lba = addr(arg);
			c.st = C_RD_MULTI;
		} else {
			c.wr_lba = addr(arg);
			c.multi_wr = cmd == 25;
			c.st = C_WR_WAIT;
		}
		return;
	default:
		put(0x04);		/* illegal command */
	}
}

/* One byte each way: the card's output for this transfer, given its input. */
static u8 card_xfer(u8 in)
{
	u8 out;

	if (c.qh != c.qt) {
		out = c.q[c.qh++ % sizeof(c.q)];
	} else if (c.busy > 0) {
		c.busy--;
		out = 0x00;
	} else if (c.st == C_RD_MULTI) {
		if (c.rd_lba < NSECT)
			put_block(c.rd_lba++);
		else
			put(0x08);
		out = c.q[c.qh++ % sizeof(c.q)];
	} else {
		out = 0xff;
	}

	switch (c.st) {
	case C_IDLE:
	case C_RD_MULTI:
		if (c.ncmd || (in & 0xc0) == 0x40) {
			if (!c.ncmd && c.busy > 0 && c.st == C_IDLE)
				violation("command sent while the card was busy");
			c.cmd[c.ncmd++] = in;
			if (c.ncmd == 6) {
				c.ncmd = 0;
				card_cmd();
			}
		}
		break;
	case C_WR_WAIT:
		if (in == 0xff)
			break;
		if (c.busy > 0 || c.qh != c.qt) {
			/* A real card ignores the stop token while busy. */
			violation("token sent while the card was busy");
			break;
		}
		if (in == (c.multi_wr ? 0xfc : 0xfe)) {
			c.st = C_WR_DATA;
			c.nw = 0;
		} else if (in == 0xfd && c.multi_wr) {
			put(0xff);
			c.busy = rnd(30);
			c.st = C_IDLE;
		} else {
			violation("unexpected byte while waiting for a data token");
		}
		break;
	case C_WR_DATA:
		c.wbuf[c.nw++] = in;
		if (c.nw < 514)
			break;
		if (f.wr_reject && !rnd(f.wr_reject)) {
			injected++;
			put(0x0b);	/* CRC error: block not written */
		} else {
			memcpy(store[c.wr_lba], c.wbuf, 512);
			c.wr_lba++;
			put(0xe5);	/* accepted */
		}
		c.busy = 1 + rnd(50);
		c.st = c.multi_wr ? C_WR_WAIT : C_IDLE;
		break;
	}
	return out;
}

/* ---- the controller ---- */

static u32 ctrl;
static unsigned long now, done_at, accesses;
static bool pending;
static u8 pending_rx, latch;

static void tick(void)
{
	now++;
	if (++accesses % JIFFY_ACC == 0)
		jiffies++;
	if (pending && now >= done_at) {
		pending = false;
		latch = pending_rx;
	}
}

static void stall(void)
{
	if (f.stall && !rnd(f.stall)) {
		stalls++;
		now += 3 * XFER_T;
		tick();
	}
}

u32 model_readl(unsigned long off)
{
	tick();
	if (f.zero_regs)
		return 0;
	switch (off) {
	case 0x00:
		return ctrl;
	case 0x08:
		stall();
		return latch;
	case 0x10:
		stall();
		return pending;
	}
	violation("read of an unknown register");
	return 0;
}

void model_writel(u32 v, unsigned long off)
{
	tick();
	if (f.zero_regs)
		return;
	switch (off) {
	case 0x00:
		if (pending)
			violation("CTRL written mid-transfer");
		if ((v & (1u << 16)) && !(ctrl & (1u << 16)) &&
		    (c.st != C_IDLE || c.ncmd || c.qh != c.qt))
			/* Busy is fine: the next command waits for ready. */
			violation("CS raised with the card mid-operation");
		ctrl = v;
		return;
	case 0x08:
		if (pending)
			violation("DATA written while the bus was busy");
		pending_rx = (f.absent || (ctrl & (1u << 16))) ? 0xff : card_xfer(v);
		pending = true;
		done_at = now + XFER_T;
		return;
	}
	violation("write to an unknown register");
}

/* ---- the test ---- */

static void reset(bool sdsc)
{
	memset(&c, 0, sizeof(c));
	c.sdsc = sdsc;
	ctrl = 0;
	pending = false;
	latch = 0;
}

static int fails;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fails++; \
		fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		return; \
	} \
} while (0)

static void probe_absent(void)
{
	memset(&f, 0, sizeof(f));
	reset(false);
	f.absent = true;
	CHECK(sd_spi_init() != 0, "no card: probe succeeded");
	f.absent = false;
	f.zero_regs = true;
	CHECK(sd_spi_init() != 0, "no controller: probe succeeded");
	f.zero_regs = false;
	printf("probe: no card and no controller both refused\n");
}

static u8 buf[128 * 512];

static void fill(bool sdsc)
{
	int i, j;

	reset(sdsc);
	for (i = 0; i < NSECT; i++)
		for (j = 0; j < 512; j++)
			store[i][j] = ref[i][j] = random();
}

/* Faults on every try: -EIO, the card unchanged, the protocol intact. */
static void persistent_faults(void)
{
	unsigned long v0 = violations;
	int rc, k;

	memset(&f, 0, sizeof(f));
	fill(false);
	CHECK(sd_spi_init() == 0, "init failed");
	for (k = 0; k < 3; k++) {
		f.cmd_reject = k == 0;
		f.token_err = k == 1;
		f.wr_reject = k == 2;
		rc = sd_spi_read(100, buf, 16, true, true);
		CHECK(k == 2 ? rc == 0 : rc == -EIO, "fault %d: read rc %d", k, rc);
		memset(buf, 0xa5, 16 * 512);
		rc = sd_spi_write(100, buf, 16, true, true);
		CHECK(k == 1 ? rc == 0 : rc == -EIO, "fault %d: write rc %d", k, rc);
		if (k == 1)
			memcpy(ref[100], buf, 16 * 512);
		CHECK(!memcmp(store, ref, sizeof(store)), "fault %d: card contents changed", k);
		CHECK(violations == v0, "fault %d: protocol violation (%s)", k, last_violation);
		CHECK(c.st == C_IDLE && (ctrl & (1u << 16)), "fault %d: card left mid-operation", k);
	}
	memset(&f, 0, sizeof(f));
	printf("persistent faults: -EIO, the card unchanged\n");
}

static void workload(const char *name, bool sdsc, int iters)
{
	unsigned long ops[2][2][2] = { 0 }, before = sd_spi_stats.late;
	unsigned long fb0 = sd_spi_stats.multi_fallbacks, rt0 = sd_spi_stats.retries;
	unsigned long inj0 = injected, st0 = stalls, v0 = violations;
	int i, j;

	fill(sdsc);
	CHECK(sd_spi_init() == 0, "%s: init failed", name);
	CHECK(sd_spi_capacity() == NSECT, "%s: capacity %llu", name,
	      (unsigned long long)sd_spi_capacity());
	CHECK(sd_spi_blockaddr() == !sdsc, "%s: addressing mode", name);

	for (i = 0; i < iters; i++) {
		bool wr = rnd(2), multi = rnd(4) != 0, pipe = rnd(4) != 0;
		u32 n = rnd(3) ? 1 + rnd(128) : 1 + rnd(4);
		u32 lba = rnd(NSECT - n + 1);
		int rc;

		ops[wr][multi][pipe]++;
		if (wr) {
			for (j = 0; j < (int)(n * 512); j++)
				buf[j] = random();
			rc = sd_spi_write(lba, buf, n, multi, pipe);
			CHECK(rc == 0, "%s: write %u+%u rc %d", name, lba, n, rc);
			memcpy(ref[lba], buf, n * 512);
		} else {
			memset(buf, 0x5a, sizeof(buf));
			rc = sd_spi_read(lba, buf, n, multi, pipe);
			CHECK(rc == 0, "%s: read %u+%u rc %d", name, lba, n, rc);
			CHECK(!memcmp(buf, ref[lba], n * 512), "%s: read %u+%u (multi %d pipe %d) returned wrong data",
			      name, lba, n, multi, pipe);
		}
		CHECK(!memcmp(store, ref, sizeof(store)), "%s: card contents differ after %s %u+%u",
		      name, wr ? "write" : "read", lba, n);
		CHECK(violations == v0, "%s: protocol violation (%s) in %s %u+%u", name,
		      last_violation, wr ? "write" : "read", lba, n);
		CHECK(c.st == C_IDLE && (ctrl & (1u << 16)), "%s: card left mid-operation", name);
	}
	printf("%-22s %5d ops OK; faults %5lu, stalls %5lu; late reads %4lu, multi fallbacks %3lu, retries %4lu\n",
	       name, iters, injected - inj0, stalls - st0, sd_spi_stats.late - before,
	       sd_spi_stats.multi_fallbacks - fb0, sd_spi_stats.retries - rt0);
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 600;

	model_verbose = getenv("SD_SPI_TEST_VERBOSE") != NULL;
	srandom(argc > 2 ? atoi(argv[2]) : 1);

	probe_absent();
	persistent_faults();

	memset(&f, 0, sizeof(f));
	workload("SDHC clean", false, iters);
	workload("SDSC clean", true, iters / 2);

	f.stall = 300;
	workload("SDHC stalls", false, iters);
	{
		unsigned long late = sd_spi_stats.late;

		/* A stall of every few reads: pipelined tries keep failing, so
		 * the plain last try (and the fallback) must carry the data. */
		f.stall = 4;
		workload("SDHC heavy stalls", false, iters / 4);
		if (sd_spi_stats.late == late) {
			fails++;
			fprintf(stderr, "FAIL: heavy stalls never detected as late\n");
		}
	}

	memset(&f, 0, sizeof(f));
	/* Rates where SD_RETRIES faults in a row on one block stay unlikely
	 * (persistent_faults covers that case). */
	f.cmd_reject = 400;
	f.token_err = 300;
	f.wr_reject = 300;
	workload("SDHC card errors", false, iters);

	f.stall = 300;
	workload("SDSC errors + stalls", true, iters);

	printf("%s: %lu log lines from the driver\n", fails ? "FAIL" : "PASS", model_log_lines);
	return fails != 0;
}
