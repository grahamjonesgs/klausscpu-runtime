/* bench_cmds.c — `bench`: I/O and crypto throughput, and a transfer profile.
 *
 *   bench prof start | stop   split the time between start and stop into SD
 *                             reads/writes, GCM, CTR and the rest, plus the
 *                             CPU's busy/idle share (run a transfer between)
 *   bench sd [KiB]            raw SD reads (disk_access, read-only) at 1, 8
 *                             and 64 sectors per call
 *   bench fs [KiB]            write then read /SD:/bench.tmp in 32 KiB chunks
 *   bench gcm                 AES-128-GCM through wolfCrypt (the path SSH and
 *                             TLS take, under the glue's lock) at several
 *                             record sizes
 *
 * All numbers come from the 100 MHz PERF_CYCLES counter.  Prints use 64-bit
 * arguments only (see shell_cmds.c on cbprintf).
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/aes.h>

#include <klausscpu_prof.h>

#define BENCH_BUF (32 * 1024)
static uint8_t bench_buf[BENCH_BUF] __aligned(8);

typedef unsigned long long ull;

/* KiB/s for bytes moved in cyc cycles at 100 MHz. */
static ull kibps(uint64_t bytes, uint64_t cyc)
{
	return cyc ? (ull)(bytes * 100000000ull / cyc / 1024ull) : 0;
}

static ull ms(uint64_t cyc)
{
	return (ull)(cyc / 100000ull);
}

/* "NN.N%" of den, as two 64-bit numbers. */
static void pct(uint64_t num, uint64_t den, ull *ip, ull *fp)
{
	uint64_t p = den ? num * 1000ull / den : 0;

	*ip = p / 10;
	*fp = p % 10;
}

/* ── prof ───────────────────────────────────────────────────────────────── */

/* The CPU's idle share and the busiest threads come from Zephyr's runtime
 * accounting (CONFIG_SCHED_THREAD_USAGE_ALL): the PERF block counts cycles
 * spent in WAIT as executing, so it can't tell idle from busy. */
#define PROF_THREADS 48

struct prof_thr {
	k_tid_t tid;
	uint64_t cyc;
	char name[16];
};

static struct klausscpu_prof prof0;
static uint64_t prof_t0;
static k_thread_runtime_stats_t all0;
static struct prof_thr thr0[PROF_THREADS], thr1[PROF_THREADS];
static int nthr0, nthr1;

struct thr_snap {
	struct prof_thr *t;
	int n;
};

static void snap_one(const struct k_thread *cthread, void *user)
{
	struct thr_snap *s = user;
	struct k_thread *thread = (struct k_thread *)cthread;
	k_thread_runtime_stats_t st;

	if (s->n >= PROF_THREADS || k_thread_runtime_stats_get(thread, &st) != 0) {
		return;
	}
	struct prof_thr *e = &s->t[s->n++];

	e->tid = thread;
	e->cyc = st.execution_cycles;
	const char *nm = k_thread_name_get(thread);

	strncpy(e->name, nm ? nm : "?", sizeof(e->name) - 1);
	e->name[sizeof(e->name) - 1] = '\0';
}

static int snap_threads(struct prof_thr *t)
{
	struct thr_snap s = { t, 0 };

	k_thread_foreach_unlocked(snap_one, &s);
	return s.n;
}

void bench_prof_start(void)
{
	prof0 = klausscpu_prof;
	prof_t0 = kprof_now();
	(void)k_thread_runtime_stats_all_get(&all0);
	nthr0 = snap_threads(thr0);
}

/* Appends to out; returns the length written. */
#define APP(...) do { if (len < n) len += (size_t)snprintk(out + len, n - len, __VA_ARGS__); } while (0)

static void prof_line(char *out, size_t n, size_t *lenp, const char *what,
		      uint64_t cyc, uint64_t bytes, uint64_t calls, uint64_t wall)
{
	size_t len = *lenp;
	ull i, f;

	pct(cyc, wall, &i, &f);
	APP("  %-9s %6llu ms %3llu.%llu%%  %8llu KiB in %6llu calls"
	    "  %6llu KiB/s while in it\n",
	    what, ms(cyc), i, f, (ull)(bytes / 1024), (ull)calls,
	    kibps(bytes, cyc));
	*lenp = len;
}

size_t bench_prof_report(char *out, size_t n)
{
	struct klausscpu_prof p = klausscpu_prof;
	uint64_t wall = kprof_now() - prof_t0;
	k_thread_runtime_stats_t all1;
	size_t len = 0;
	ull i, f;

	(void)k_thread_runtime_stats_all_get(&all1);
	nthr1 = snap_threads(thr1);
	uint64_t tot = all1.execution_cycles - all0.execution_cycles;
	uint64_t idl = all1.idle_cycles - all0.idle_cycles;

	pct(tot - idl, tot, &i, &f);
	APP("window %llu ms, CPU busy %llu.%llu%% (idle %llu ms)\n", ms(wall), i, f,
	    tot ? (ull)(ms(wall) * idl / tot) : 0ull);
#define D(x) (p.x - prof0.x)
	prof_line(out, n, &len, "SD read", D(sd_rd_cyc), D(sd_rd_sec) * 512, D(sd_rd_calls), wall);
	prof_line(out, n, &len, "SD write", D(sd_wr_cyc), D(sd_wr_sec) * 512, D(sd_wr_calls), wall);
	prof_line(out, n, &len, "AES-GCM", D(gcm_cyc), D(gcm_bytes), D(gcm_calls), wall);
	prof_line(out, n, &len, "AES-CTR", D(ctr_cyc), D(ctr_bytes), D(ctr_calls), wall);
	if (D(sd_rd_late)) {
		APP("  SD pipelined-read retries: %llu\n", (ull)D(sd_rd_late));
	}
#undef D
	/* Threads by CPU time in the window (ISRs count to the thread they
	 * interrupted). */
	APP("  threads (share of window):\n");
	for (int k = 0; k < 8; k++) {
		int best = -1;
		uint64_t bc = 0;

		for (int a = 0; a < nthr1; a++) {
			uint64_t c0 = 0;

			for (int b = 0; b < nthr0; b++) {
				if (thr0[b].tid == thr1[a].tid) {
					c0 = thr0[b].cyc;
				}
			}
			/* A thread recreated in the window restarts at 0. */
			uint64_t d = thr1[a].cyc >= c0 ? thr1[a].cyc - c0 : thr1[a].cyc;

			if (thr1[a].tid != NULL && d > bc) {
				bc = d;
				best = a;
			}
		}
		if (best < 0 || tot == 0) {
			break;
		}
		pct(bc, tot, &i, &f);
		APP("    %-15s %3llu.%llu%%\n", thr1[best].name, i, f);
		thr1[best].tid = NULL;	/* don't pick it again */
	}
	return len;
}
#undef APP

static int cmd_bench_prof(const struct shell *sh, size_t argc, char **argv)
{
	static char rep[1536];

	if (argc < 2 || strcmp(argv[1], "start") == 0) {
		bench_prof_start();
		shell_print(sh, "bench prof: started");
		return 0;
	}
	if (strcmp(argv[1], "stop") != 0) {
		shell_error(sh, "usage: bench prof start|stop");
		return -EINVAL;
	}
	(void)bench_prof_report(rep, sizeof(rep));
	shell_fprintf(sh, SHELL_NORMAL, "%s", rep);
	return 0;
}

/* ── sd ─────────────────────────────────────────────────────────────────── */

static int cmd_bench_sd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t kib = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1024u;
	const char *disk = CONFIG_DISK_KLAUSSCPU_SD_VOLUME_NAME;
	static const uint32_t chunks[] = {1, 8, 64};
	uint32_t total = kib * 2u;	/* sectors */

	/* Read-only, from a fixed region well past the FAT boot area. */
	for (size_t c = 0; c < ARRAY_SIZE(chunks); c++) {
		uint32_t n = chunks[c], lba = 32768u;
		uint64_t t0 = kprof_now();

		for (uint32_t done = 0; done < total; done += n, lba += n) {
			if (disk_access_read(disk, bench_buf, lba, n) != 0) {
				shell_error(sh, "read failed at lba %llu", (ull)lba);
				return -EIO;
			}
		}
		uint64_t dt = kprof_now() - t0;

		shell_print(sh, "SD raw read %4llu KiB, %2llu sectors/call: %6llu KiB/s",
			    (ull)kib, (ull)n, kibps((uint64_t)total * 512u, dt));
	}
	shell_print(sh, "SD pipelined-read retries since boot: %llu",
		    (ull)klausscpu_prof.sd_rd_late);
	return 0;
}

/* ── fs ─────────────────────────────────────────────────────────────────── */

#define BENCH_FILE "/SD:/bench.tmp"

static int cmd_bench_fs(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t kib = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1024u;
	uint64_t bytes = (uint64_t)kib * 1024u;
	struct fs_file_t f;
	int rc;

	for (size_t i = 0; i < BENCH_BUF; i++) {
		bench_buf[i] = (uint8_t)(i * 7u + 3u);
	}
	(void)fs_unlink(BENCH_FILE);
	fs_file_t_init(&f);
	rc = fs_open(&f, BENCH_FILE, FS_O_CREATE | FS_O_WRITE);
	if (rc != 0) {
		shell_error(sh, "open: %d", rc);
		return rc;
	}
	uint64_t t0 = kprof_now();

	for (uint64_t done = 0; done < bytes; done += BENCH_BUF) {
		size_t n = bytes - done < BENCH_BUF ? (size_t)(bytes - done) : BENCH_BUF;

		if (fs_write(&f, bench_buf, n) != (ssize_t)n) {
			shell_error(sh, "write failed");
			(void)fs_close(&f);
			return -EIO;
		}
	}
	(void)fs_close(&f);
	uint64_t t1 = kprof_now();

	shell_print(sh, "FS write %llu KiB (32 KiB chunks, incl. close): %llu KiB/s",
		    (ull)kib, kibps(bytes, t1 - t0));

	fs_file_t_init(&f);
	rc = fs_open(&f, BENCH_FILE, FS_O_READ);
	if (rc != 0) {
		shell_error(sh, "open: %d", rc);
		return rc;
	}
	t0 = kprof_now();
	uint64_t got = 0;

	for (;;) {
		ssize_t n = fs_read(&f, bench_buf, BENCH_BUF);

		if (n <= 0) {
			break;
		}
		got += (uint64_t)n;
	}
	t1 = kprof_now();
	(void)fs_close(&f);
	(void)fs_unlink(BENCH_FILE);
	shell_print(sh, "FS read  %llu KiB (32 KiB chunks): %llu KiB/s",
		    (ull)(got / 1024), kibps(got, t1 - t0));
	return 0;
}

/* ── gcm ────────────────────────────────────────────────────────────────── */

static int cmd_bench_gcm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static const uint32_t sizes[] = {256, 1024, 2048, 8192, 16384};
	static Aes aes;
	uint8_t key[16], iv[12] = {0}, aad[13] = {0}, tag[16];
	const uint64_t per_size = 256u * 1024u;

	for (int i = 0; i < 16; i++) {
		key[i] = (uint8_t)(i * 17 + 1);
	}
	memset(bench_buf, 0x5A, BENCH_BUF);
	if (wc_AesInit(&aes, NULL, INVALID_DEVID) != 0 ||
	    wc_AesGcmSetKey(&aes, key, sizeof(key)) != 0) {
		shell_error(sh, "wc_AesGcmSetKey failed");
		return -EIO;
	}
	for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
		uint32_t sz = sizes[s];
		uint32_t reps = (uint32_t)(per_size / sz);
		uint64_t t0 = kprof_now();

		for (uint32_t r = 0; r < reps; r++) {
			iv[11] = (uint8_t)r;
			(void)wc_AesGcmEncrypt(&aes, bench_buf, bench_buf, sz, iv,
					       sizeof(iv), tag, sizeof(tag), aad,
					       sizeof(aad));
		}
		uint64_t dt = kprof_now() - t0;

		shell_print(sh, "GCM wolfCrypt %5llu B records: %6llu KiB/s  (%llu cyc/call)",
			    (ull)sz, kibps(per_size, dt), (ull)(dt / reps));
	}
	wc_AesFree(&aes);
	return 0;
}

/* ── opt: A/B switches ──────────────────────────────────────────────────── */

extern int klausscpu_sd_multiblock;
extern int klausscpu_sd_pipeline;
extern int aes_gcm_single_pass;
extern int klausscpu_tcp_nodelay;

static int cmd_bench_opt(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 3) {
		int v = (int)strtol(argv[2], NULL, 10);

		if (strcmp(argv[1], "sd_multi") == 0) {
			klausscpu_sd_multiblock = v;
		} else if (strcmp(argv[1], "sd_pipe") == 0) {
			klausscpu_sd_pipeline = v;
		} else if (strcmp(argv[1], "gcm_1pass") == 0) {
			aes_gcm_single_pass = v;
		} else if (strcmp(argv[1], "nodelay") == 0) {
			klausscpu_tcp_nodelay = v;
		} else {
			shell_error(sh, "unknown option %s", argv[1]);
			return -EINVAL;
		}
	}
	shell_print(sh, "sd_multi=%lld sd_pipe=%lld gcm_1pass=%lld nodelay=%lld",
		    (long long)klausscpu_sd_multiblock, (long long)klausscpu_sd_pipeline,
		    (long long)aes_gcm_single_pass, (long long)klausscpu_tcp_nodelay);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bench_subcmds,
	SHELL_CMD_ARG(opt, NULL, "[sd_multi|sd_pipe|gcm_1pass|nodelay 0|1]: A/B switches", cmd_bench_opt, 1, 2),
	SHELL_CMD_ARG(prof, NULL, "start|stop: profile a transfer", cmd_bench_prof, 1, 1),
	SHELL_CMD_ARG(sd, NULL, "[KiB]: raw SD read throughput", cmd_bench_sd, 1, 1),
	SHELL_CMD_ARG(fs, NULL, "[KiB]: file write/read throughput", cmd_bench_fs, 1, 1),
	SHELL_CMD_ARG(gcm, NULL, "AES-128-GCM throughput through wolfCrypt",
		      cmd_bench_gcm, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(bench, &bench_subcmds, "I/O and crypto benchmarks", NULL);
