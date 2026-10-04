/*
 * main.c — Mandelbrot zoom over VNC: a two-core showcase.
 *
 * Core 1 computes a zoom into Seahorse Valley, 320x200, as 8-bit palette
 * indices (64-bit fixed point, 28 fraction bits, cardioid/bulb skip).  Images
 * are double-buffered: while the next one computes, the finished one is
 * re-posted every POST_MS, scaled up toward the next image's zoom and with
 * the palette rotated one step, so the display zooms smoothly at ~25 fps
 * however long an image takes (see "continuous zoom" below).
 *
 *  - Two-core build (amp.conf): core 2 owns the network and serves VNC from
 *    the 8-bit indexed source (colour-map clients get the indices as-is; the
 *    RGB565 framebuffer is filled only while a true-colour client needs it).
 *    Core 1 does nothing but compute and post.
 *  - Single-core build (prj.conf only): core 1 also runs Zephyr's TCP/IP
 *    stack and the VNC server, and converts every post to RGB565 — the
 *    comparison.
 *
 * Console, every 5 s: images and posts completed, and compute throughput in
 * millions of iterations per second (the number to compare between builds).
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_NETWORKING
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#endif

#include "framebuffer.h"
#ifdef CONFIG_KLAUSSCPU_VNC_SERVER
#include "vnc_server.h"
#endif
#ifdef CONFIG_KLAUSSCPU_AMP_VNC
#include "amp_host.h"
#endif

LOG_MODULE_REGISTER(mandel, LOG_LEVEL_INF);

#define W        FB_WIDTH
#define H        FB_HEIGHT
#define FRAC     28
#define ONE      ((int64_t)1 << FRAC)
#define POST_MS  40              /* re-post (palette step) period: ~25 fps */
#define STATS_MS 5000

/* Zoom target (Seahorse Valley) and start/stop pixel steps.  Q28 resolves
 * ~3.7e-9; restart well before the step gets that coarse. */
#define CX       ((int64_t)(-0.743643887037151 * (double)ONE))
#define CY       ((int64_t)(0.131825904205330 * (double)ONE))
#define STEP0    ((int64_t)(3.2 / W * (double)ONE))
#define STEP_MIN 64

static inline int64_t fmul(int64_t a, int64_t b)
{
	return (a * b) >> FRAC;
}

static uint64_t g_iters;         /* iterations actually executed */

static inline unsigned int iterate(int64_t cr, int64_t ci, unsigned int maxit)
{
	/* Main cardioid and period-2 bulb are inside the set: skip them. */
	int64_t xq = cr - ONE / 4;
	int64_t ci2 = fmul(ci, ci);
	int64_t q = fmul(xq, xq) + ci2;

	if (fmul(q, q + xq) <= ci2 / 4) {
		return maxit;
	}
	int64_t xb = cr + ONE;

	if (fmul(xb, xb) + ci2 <= ONE / 16) {
		return maxit;
	}

	int64_t zr = 0, zi = 0, zr2 = 0, zi2 = 0;
	unsigned int n = 0;

	while (n < maxit && zr2 + zi2 <= 4 * ONE) {
		zi = ((zr * zi) >> (FRAC - 1)) + ci;
		zr = zr2 - zi2 + cr;
		zr2 = fmul(zr, zr);
		zi2 = fmul(zi, zi);
		n++;
	}
	g_iters += n;
	return n;
}

/* ── palette: 255-colour gradient, index 0 = inside (black) ───────────────── */
static uint32_t grad[255];
static uint32_t pal[256];        /* 0x00RRGGBB, rotated per post */
static uint16_t pal565[256];

static void build_gradient(void)
{
	/* Piecewise-linear through a classic deep-blue / white / orange set. */
	static const uint32_t keys[] = {
		0x000764, 0x206BCB, 0xEDFFFF, 0xFFAA00, 0x000200, 0x000764,
	};
	const int nseg = (int)ARRAY_SIZE(keys) - 1;

	for (int i = 0; i < 255; i++) {
		int seg = i * nseg / 255;
		int t = (i * nseg) % 255;          /* 0..254 within the segment */
		uint32_t a = keys[seg], b = keys[seg + 1];
		uint32_t c = 0;

		for (int s = 0; s < 24; s += 8) {
			int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;

			c |= (uint32_t)(ca + (cb - ca) * t / 255) << s;
		}
		grad[i] = c;
	}
}

static void build_palette(unsigned int offset)
{
	pal[0] = 0;
	pal565[0] = 0;
	for (int i = 1; i < 256; i++) {
		uint32_t c = grad[(i - 1 + offset) % 255];

		pal[i] = c;
		pal565[i] = fb_rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
	}
}

/* Indices -> RGB565 framebuffer, 8 pixels per step (as in apps/doom). */
static void convert_to_fb(const uint8_t *src)
{
	fb_lock();
	uint16_t *dst = fb_pixels();

	for (int y = 0; y < H; y++) {
		const uint64_t *s64 = (const uint64_t *)(const void *)(src + (size_t)y * W);
		uint64_t *d64 = (uint64_t *)(void *)(dst + (size_t)y * FB_WIDTH);

		for (int x = 0; x < W; x += 8) {
			uint64_t i = *s64++;

			*d64++ = (uint64_t)pal565[i & 0xFF] |
				 ((uint64_t)pal565[(i >> 8) & 0xFF] << 16) |
				 ((uint64_t)pal565[(i >> 16) & 0xFF] << 32) |
				 ((uint64_t)pal565[(i >> 24) & 0xFF] << 48);
			*d64++ = (uint64_t)pal565[(i >> 32) & 0xFF] |
				 ((uint64_t)pal565[(i >> 40) & 0xFF] << 16) |
				 ((uint64_t)pal565[(i >> 48) & 0xFF] << 32) |
				 ((uint64_t)pal565[i >> 56] << 48);
		}
	}
	fb_unlock();
}

/* ── display: continuous zoom between computed images ─────────────────────
 * Each computed image is ZOOM_NUM/ZOOM_DEN deeper than the last and takes
 * ~1 s, so showing them as-is jumps.  Instead every post shows the latest
 * image scaled up about the centre by a factor that grows from 1 to
 * ZOOM_DEN/ZOOM_NUM over the time the previous image took to compute: when
 * the next exact image is ready the display has already reached its zoom,
 * and swapping it in just sharpens the detail (nearest-neighbour, as XaoS
 * does).  Two display buffers so core 2 never streams one being written. */
#define ZOOM_NUM 15              /* each image: step *= 15/16 */
#define ZOOM_DEN 16

static uint8_t *front, *back;    /* latest computed image / being computed */
static uint8_t *disp[2];
static int disp_i;
static int64_t zoom_t0;          /* when front was shown */
static int64_t zoom_ms = 1000;   /* expected time to the next image */
static uint16_t xmap[W], ymap[H];
static unsigned int pal_offset;
static uint32_t n_posts;

static void post(void)
{
	build_palette(++pal_offset);

	/* Zoom factor f (Q16) from 1 to DEN/NUM; inverse inv = 1/f (Q16). */
	int64_t el = k_uptime_get() - zoom_t0;

	if (el > zoom_ms) {
		el = zoom_ms;
	}
	uint32_t f = 65536u + (uint32_t)((65536u * (ZOOM_DEN - ZOOM_NUM) / ZOOM_NUM) * el / zoom_ms);
	uint32_t inv = (uint32_t)(((uint64_t)65536u << 16) / f);

	for (int x = 0; x < W; x++) {
		xmap[x] = (uint16_t)(W / 2 + (((int64_t)(x - W / 2) * inv) >> 16));
	}
	for (int y = 0; y < H; y++) {
		ymap[y] = (uint16_t)(H / 2 + (((int64_t)(y - H / 2) * inv) >> 16));
	}

	uint8_t *d = disp[disp_i ^= 1];

	for (int y = 0; y < H; y++) {
		const uint8_t *srow = front + (size_t)ymap[y] * W;
		uint64_t *d64 = (uint64_t *)(void *)(d + (size_t)y * W);

		/* 8 pixels per u64 store (little-endian: pixel 0 in the low byte). */
		for (int x = 0; x < W; x += 8) {
			const uint16_t *m = &xmap[x];

			*d64++ = (uint64_t)srow[m[0]] |
				 ((uint64_t)srow[m[1]] << 8) |
				 ((uint64_t)srow[m[2]] << 16) |
				 ((uint64_t)srow[m[3]] << 24) |
				 ((uint64_t)srow[m[4]] << 32) |
				 ((uint64_t)srow[m[5]] << 40) |
				 ((uint64_t)srow[m[6]] << 48) |
				 ((uint64_t)srow[m[7]] << 56);
		}
	}

#ifdef CONFIG_KLAUSSCPU_AMP_VNC
	(void)amp_set_indexed(d, W);
	amp_set_palette(pal);
	if (amp_want_rgb565()) {
		convert_to_fb(d);
	}
	amp_post_frame(0, 0, W, H);
#else
	convert_to_fb(d);
	fb_mark_dirty(0, 0, W, H);
#endif
	n_posts++;
}

/* A new computed image: show it at zoom factor 1 and start zooming toward
 * the next, pacing the zoom by how long this one took to compute. */
static void show(uint8_t *img, int64_t compute_ms)
{
	front = img;
	zoom_t0 = k_uptime_get();
	if (compute_ms > 0) {
		zoom_ms = compute_ms < 100 ? 100 : compute_ms;
	}
	post();
}

/* ── compute thread ──────────────────────────────────────────────────────── */
#define MANDEL_STACK 16384
#define MANDEL_PRIO  12
static K_THREAD_STACK_DEFINE(mandel_stack, MANDEL_STACK);
static struct k_thread mandel_thread;

static void mandel_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	front = malloc((size_t)W * H);
	back = malloc((size_t)W * H);
	disp[0] = malloc((size_t)W * H);
	disp[1] = malloc((size_t)W * H);
	if (!front || !back || !disp[0] || !disp[1] ||
	    (((uintptr_t)front | (uintptr_t)back | (uintptr_t)disp[0] | (uintptr_t)disp[1]) & 7u)) {
		LOG_ERR("frame buffer allocation failed");
		return;
	}
	memset(front, 0, (size_t)W * H);
	build_gradient();
	show(front, 0);

	int64_t step = STEP0;
	uint32_t n_images = 0;
	int64_t next_post = k_uptime_get() + POST_MS;
	int64_t stats_t0 = k_uptime_get();
	uint64_t stats_iters = g_iters;
	unsigned int maxit = 64;

	for (;;) {
		/* Iteration budget grows with zoom depth (doublings of STEP0/step). */
		unsigned int depth = 0;

		while ((step << depth) < STEP0) {
			depth++;
		}
		maxit = 64 + 24 * depth;
		if (maxit > 640) {
			maxit = 640;
		}

		const int64_t x0 = CX - step * (W / 2);
		const int64_t y0 = CY - step * (H / 2);
		const int64_t img_t0 = k_uptime_get();

		for (int y = 0; y < H; y++) {
			uint8_t *row = back + (size_t)y * W;
			const int64_t ci = y0 + step * y;

			for (int x = 0; x < W; x++) {
				unsigned int n = iterate(x0 + step * x, ci, maxit);

				row[x] = (n >= maxit) ? 0 : (uint8_t)(1 + n % 255);
			}
			if (k_uptime_get() >= next_post) {
				post();
				next_post = k_uptime_get() + POST_MS;
			}
		}

		uint8_t *t = front;

		step -= step >> 4;               /* zoom in by 1/16 (ZOOM_NUM/DEN) per image */
		if (step < STEP_MIN) {
			step = STEP0;            /* restart: a jump back out, by design */
		}
		show(back, k_uptime_get() - img_t0);
		back = t;
		n_images++;

		int64_t now = k_uptime_get();

		if (now - stats_t0 >= STATS_MS) {
			uint32_t ms = (uint32_t)(now - stats_t0);
			uint64_t it = g_iters - stats_iters;

			/* <=2 args per line: multi-arg printk garbles on this arch. */
			printk("mandel: images=%u posts=%u\n", n_images, n_posts);
			printk("mandel: kiter/s=%u\n", (uint32_t)(it / ms));
			printk("mandel: depth=%u maxit=%u\n", depth, maxit);
			n_images = 0;
			n_posts = 0;
			stats_t0 = now;
			stats_iters = g_iters;
		}
	}
}

/* ── network (single-core build only) ────────────────────────────────────── */
#ifdef CONFIG_NETWORKING
static struct net_mgmt_event_callback dhcp_cb;
static struct k_sem dhcp_sem;

static void dhcp_handler(struct net_mgmt_event_callback *cb,
			 uint32_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		k_sem_give(&dhcp_sem);
	}
}

static int wait_for_dhcp(void)
{
	k_sem_init(&dhcp_sem, 0, 1);
	net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler, NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);

	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface");
		return -1;
	}
	net_dhcpv4_start(iface);
	if (k_sem_take(&dhcp_sem, K_SECONDS(10)) != 0) {
		LOG_WRN("DHCP timeout");
		return -1;
	}
	return 0;
}
#endif

int main(void)
{
	printk("\nKlaussCPU Mandelbrot (%s)\n",
	       IS_ENABLED(CONFIG_KLAUSSCPU_AMP_VNC) ? "two cores" : "single core");
#ifdef CONFIG_KLAUSSCPU_AMP_VNC
	if (amp_host_init() != 0) {
		LOG_ERR("AMP core 2 failed to start — no display");
	}
#else
	if (wait_for_dhcp() != 0) {
		LOG_WRN("no network — VNC will be unreachable");
	}
	vnc_server_start();
	LOG_INF("VNC server ready on port 5900");
#endif

	k_thread_create(&mandel_thread, mandel_stack, K_THREAD_STACK_SIZEOF(mandel_stack),
			mandel_entry, NULL, NULL, NULL, MANDEL_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&mandel_thread, "mandel");

	/* Never return from main on this arch (thread-exit jumps to PC 0). */
	k_sleep(K_FOREVER);
	return 0;
}
