/*
 * vnc_server.c — minimal RFB (Remote Framebuffer / VNC) server for KlaussCPU.
 *
 * Serves the in-RAM RGB565 framebuffer (framebuffer.c) to standard VNC clients
 * (macOS Screen Sharing, TigerVNC, RealVNC) over a raw BSD socket on :5900.
 * Mirrors the listener style of ssh/httpd.c but is single-connection: a VNC
 * desktop is one screen and the board is a single-user dev target, so the
 * listener serves one client inline and the next waits in the accept backlog.
 *
 * Implemented subset of RFC 6143:
 *   - ProtocolVersion 3.8, security type None.
 *   - ServerInit advertising the native RGB565 format.
 *   - Client messages: SetPixelFormat, SetEncodings, FramebufferUpdateRequest;
 *     KeyEvent/PointerEvent/ClientCutText are parsed and discarded (no input
 *     handling yet — milestone 6).
 *   - Server messages: FramebufferUpdate, Raw encoding only.
 *
 * Pixel conversion: the framebuffer is RGB565, but clients commonly request a
 * 32-bpp true-colour format via SetPixelFormat.  convert_pixel() honours the
 * client's per-channel max/shift and byte order, so colours stay correct
 * regardless of what the client asks for.  (Raw + per-pixel conversion is
 * bandwidth-heavy and CPU-heavy; dirty-rect/encoding optimisation is
 * milestone 5.)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

#include "framebuffer.h"
#include "vnc_server.h"

LOG_MODULE_REGISTER(vncd, LOG_LEVEL_INF);

#define VNC_PORT        5900            /* RFB display :0 */
#define VNC_STACK_SIZE  8192
#define VNC_PRIO        7
#define DESKTOP_NAME    "KlaussCPU"

/* Refresh throttle: never send incremental updates faster than this, so a
 * fast-changing framebuffer can't flood the link or peg the soft core.  Dirt
 * accumulated between sends is coalesced into one update. */
#define VNC_MAX_FPS     10
#define VNC_MIN_FRAME_MS (1000 / VNC_MAX_FPS)
/* While an incremental update is pending, re-check the dirty box this often. */
#define VNC_POLL_MS     10

/* RFB pixel format (the 16-byte PIXEL_FORMAT structure), decoded. */
struct pixfmt {
	uint8_t  bpp;          /* bits per pixel: 8/16/32           */
	uint8_t  depth;
	uint8_t  big_endian;   /* multi-byte pixels sent big-endian */
	uint8_t  true_colour;
	uint16_t red_max, green_max, blue_max;
	uint8_t  red_shift, green_shift, blue_shift;
};

/* Native framebuffer format: RGB565, little-endian. */
static const struct pixfmt native_fmt = {
	.bpp = 16, .depth = 16, .big_endian = 0, .true_colour = 1,
	.red_max = 31, .green_max = 63, .blue_max = 31,
	.red_shift = 11, .green_shift = 5, .blue_shift = 0,
};

/* One converted scanline, max width at 4 bytes/pixel. */
/* Batch buffer: convert many scanlines into this, then send in big chunks
 * (one zsock_send per ~64 KB instead of one per row).  Per-row sends were the
 * dominant cost — 400 sends/frame of ~1.3 KB each, ~3.7 s/frame total. */
#define SENDBUF_SZ (64 * 1024)
static uint8_t sendbuf[SENDBUF_SZ];

static K_THREAD_STACK_DEFINE(vnc_stack, VNC_STACK_SIZE);
static struct k_thread vnc_thread;

#ifdef CONFIG_KLAUSSCPU_VNC_DEMO
#define DEMO_STACK_SIZE 2048
static K_THREAD_STACK_DEFINE(demo_stack, DEMO_STACK_SIZE);
static struct k_thread demo_thread;
#endif

/* ── byte helpers (RFB is big-endian on the wire) ───────────────────────── */

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* ── socket helpers ─────────────────────────────────────────────────────── */

static int recv_all(int s, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		int r = zsock_recv(s, p + got, n - got, 0);

		if (r <= 0) {
			return -1;
		}
		got += (size_t)r;
	}
	return 0;
}

static int send_all(int s, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t sent = 0;

	while (sent < n) {
		int r = zsock_send(s, p + sent, n - sent, 0);

		if (r <= 0) {
			return -1;
		}
		sent += (size_t)r;
	}
	return 0;
}

static int recv_discard(int s, size_t n)
{
	uint8_t tmp[64];

	while (n) {
		size_t c = n < sizeof(tmp) ? n : sizeof(tmp);

		if (recv_all(s, tmp, c)) {
			return -1;
		}
		n -= c;
	}
	return 0;
}

/* ── pixel conversion ───────────────────────────────────────────────────── */

static void parse_pixfmt(struct pixfmt *f, const uint8_t *p)
{
	f->bpp         = p[0];
	f->depth       = p[1];
	f->big_endian  = p[2];
	f->true_colour = p[3];
	f->red_max     = rd16(p + 4);
	f->green_max   = rd16(p + 6);
	f->blue_max    = rd16(p + 8);
	f->red_shift   = p[10];
	f->green_shift = p[11];
	f->blue_shift  = p[12];
}

/* RGB565 -> the client's requested format value (channel-scaled + shifted). */
/* Per-channel conversion LUTs (indexed by the RGB565 components), rebuilt
 * whenever the client pixel format changes.  This replaces 3 software divides
 * per pixel — ruinous on this core (no hardware divide), and the dominant cost
 * when streaming full frames — with table lookups + ORs. */
static uint32_t lut_r[32];
static uint32_t lut_g[64];
static uint32_t lut_b[32];

/* True when the client's format is byte-identical to our native RGB565 LE, so a
 * row can be memcpy'd out with no per-pixel conversion (the common case — it's
 * the standard VNC 16bpp format).  Set by build_luts(). */
static bool fmt_native;

static void build_luts(const struct pixfmt *f)
{
	fmt_native = (f->bpp == 16 && !f->big_endian &&
		      f->red_max == 31 && f->green_max == 63 && f->blue_max == 31 &&
		      f->red_shift == 11 && f->green_shift == 5 && f->blue_shift == 0);

	for (int i = 0; i < 32; i++) {
		uint8_t c8 = (uint8_t)((i << 3) | (i >> 2));   /* 5-bit -> 8-bit */

		lut_r[i] = (((uint32_t)c8 * f->red_max + 127) / 255) << f->red_shift;
		lut_b[i] = (((uint32_t)c8 * f->blue_max + 127) / 255) << f->blue_shift;
	}
	for (int i = 0; i < 64; i++) {
		uint8_t c8 = (uint8_t)((i << 2) | (i >> 4));   /* 6-bit -> 8-bit */

		lut_g[i] = (((uint32_t)c8 * f->green_max + 127) / 255) << f->green_shift;
	}
}

static inline uint32_t convert_pixel(uint16_t px)
{
	return lut_r[(px >> 11) & 0x1F] | lut_g[(px >> 5) & 0x3F] |
	       lut_b[px & 0x1F];
}

static void put_pixel_bytes(uint8_t *p, uint32_t v, int bytes, bool big_endian)
{
	if (big_endian) {
		for (int i = 0; i < bytes; i++) {
			p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
		}
	} else {
		for (int i = 0; i < bytes; i++) {
			p[i] = (uint8_t)(v >> (8 * i));
		}
	}
}

/* Intersection of two rectangles; false if they don't overlap. */
static bool rect_isect(int ax, int ay, int aw, int ah,
		       int bx, int by, int bw, int bh,
		       int *ox, int *oy, int *ow, int *oh)
{
	int x0 = MAX(ax, bx), y0 = MAX(ay, by);
	int x1 = MIN(ax + aw, bx + bw), y1 = MIN(ay + ah, by + bh);

	if (x0 >= x1 || y0 >= y1) {
		return false;
	}
	*ox = x0; *oy = y0; *ow = x1 - x0; *oh = y1 - y0;
	return true;
}

/* ── RFB messages ───────────────────────────────────────────────────────── */

static int send_server_init(int s)
{
	uint8_t buf[24];

	be16(buf + 0, FB_WIDTH);
	be16(buf + 2, FB_HEIGHT);
	/* PIXEL_FORMAT (16 bytes) advertising native RGB565 */
	buf[4]  = native_fmt.bpp;
	buf[5]  = native_fmt.depth;
	buf[6]  = native_fmt.big_endian;
	buf[7]  = native_fmt.true_colour;
	be16(buf + 8,  native_fmt.red_max);
	be16(buf + 10, native_fmt.green_max);
	be16(buf + 12, native_fmt.blue_max);
	buf[14] = native_fmt.red_shift;
	buf[15] = native_fmt.green_shift;
	buf[16] = native_fmt.blue_shift;
	buf[17] = buf[18] = buf[19] = 0;          /* padding */
	be32(buf + 20, (uint32_t)strlen(DESKTOP_NAME));

	if (send_all(s, buf, sizeof(buf))) {
		return -1;
	}
	return send_all(s, DESKTOP_NAME, strlen(DESKTOP_NAME));
}

#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
/* Split per-update cost into convert (CPU) vs send (network), + throughput. */
#define VNC_PROF_WINDOW 10
static void vnc_prof_account(uint32_t conv_ms, uint32_t send_ms, uint64_t bytes)
{
	static uint64_t sum_conv, sum_send, sum_bytes;
	static uint32_t upd;
	static int64_t t0_ms;

	if (upd == 0) {
		t0_ms = k_uptime_get();
	}
	sum_conv += conv_ms;
	sum_send += send_ms;
	sum_bytes += bytes;
	if (++upd >= VNC_PROF_WINDOW) {
		uint32_t ms = (uint32_t)(k_uptime_get() - t0_ms);

		printk("vnc: convert=%ums send=%ums %uKB/upd ~%u KB/s\n",
		       (uint32_t)(sum_conv / upd),
		       (uint32_t)(sum_send / upd),
		       (uint32_t)(sum_bytes / upd / 1024U),
		       ms ? (uint32_t)(sum_bytes / 1024U * 1000U / ms) : 0U);
		sum_conv = 0;
		sum_send = 0;
		sum_bytes = 0;
		upd = 0;
	}
}
#endif

/* Send one FramebufferUpdate carrying a single Raw rectangle [x,y,w,h].
 * Converts and sends one scanline at a time, copying out under fb_lock() and
 * sending unlocked so drawing is never stalled across a network write. */
static int send_rect(int s, int x, int y, int w, int h, const struct pixfmt *f)
{
	int bytes = f->bpp / 8;

	if (bytes < 1 || bytes > 4 || w <= 0 || h <= 0) {
		return -1;
	}
#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
	uint32_t conv_ms = 0, send_ms = 0;
#endif

	/* FramebufferUpdate header: type 0, pad, number-of-rectangles = 1. */
	uint8_t hdr[4] = { 0, 0, 0, 1 };
	/* Rectangle header: x, y, w, h, encoding (0 = Raw). */
	uint8_t rh[12];

	be16(rh + 0, (uint16_t)x);
	be16(rh + 2, (uint16_t)y);
	be16(rh + 4, (uint16_t)w);
	be16(rh + 6, (uint16_t)h);
	be32(rh + 8, 0);

	if (send_all(s, hdr, sizeof(hdr)) || send_all(s, rh, sizeof(rh))) {
		return -1;
	}

	size_t row_bytes = (size_t)w * bytes;
	int row = 0;

	while (row < h) {
		/* Convert as many whole scanlines as fit into the batch buffer
		 * (under one lock), then send the whole chunk unlocked. */
		size_t len = 0;
#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
		int64_t c0 = k_uptime_get();
#endif

		fb_lock();
		while (row < h && len + row_bytes <= SENDBUF_SZ) {
			const uint16_t *src =
				fb_pixels() + (size_t)(y + row) * FB_WIDTH + x;

			if (fmt_native) {
				/* RGB565 LE == our framebuffer: copy raw. */
				memcpy(sendbuf + len, src, row_bytes);
			} else {
				uint8_t *o = sendbuf + len;

				for (int col = 0; col < w; col++) {
					put_pixel_bytes(o, convert_pixel(src[col]),
							bytes, f->big_endian);
					o += bytes;
				}
			}
			len += row_bytes;
			row++;
		}
		fb_unlock();

#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
		conv_ms += (uint32_t)(k_uptime_get() - c0);
		int64_t s0 = k_uptime_get();
#endif
		if (send_all(s, sendbuf, len)) {
			return -1;
		}
#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
		send_ms += (uint32_t)(k_uptime_get() - s0);
#endif
	}
#ifdef CONFIG_KLAUSSCPU_VNC_PROFILE
	vnc_prof_account(conv_ms, send_ms, (uint64_t)w * h * bytes);
#endif
	return 0;
}

/* RFB client->server message types. */
enum {
	MSG_SET_PIXEL_FORMAT = 0,
	MSG_SET_ENCODINGS    = 2,
	MSG_FB_UPDATE_REQ    = 3,
	MSG_KEY_EVENT        = 4,
	MSG_POINTER_EVENT    = 5,
	MSG_CLIENT_CUT_TEXT  = 6,
};

static void serve_client(int s)
{
	struct pixfmt fmt = native_fmt;   /* until the client sets its own */
	uint8_t b[19];

	/* Handshake (RFB 3.8). */
	if (send_all(s, "RFB 003.008\n", 12) || recv_all(s, b, 12)) {
		return;
	}
	/* Offer exactly one security type: None (1). */
	uint8_t sec[2] = { 1, 1 };

	if (send_all(s, sec, sizeof(sec)) || recv_all(s, b, 1)) {
		return;
	}
	if (b[0] != 1) {
		LOG_WRN("client picked unsupported security type %u", b[0]);
		return;
	}
	/* SecurityResult: OK. */
	uint8_t ok[4] = { 0, 0, 0, 0 };

	if (send_all(s, ok, sizeof(ok))) {
		return;
	}
	/* ClientInit (shared-flag byte, ignored) then ServerInit. */
	if (recv_all(s, b, 1) || send_server_init(s)) {
		return;
	}

	LOG_INF("VNC client connected");
	build_luts(&fmt);   /* conversion tables for the initial (native) format */

	/* Incremental-update state: the client requests a region and expects an
	 * update only once something changes (RFB lets the server delay).  We
	 * remember the pending region and service it from the dirty box. */
	bool pending = false;
	int rq_x = 0, rq_y = 0, rq_w = 0, rq_h = 0;
	int64_t last_send = 0;

	for (;;) {
		/* 1. Service a pending incremental update once the framebuffer is
		 *    dirty and the throttle interval has elapsed; coalesced dirt
		 *    is intersected with the requested region. */
		if (pending && k_uptime_get() - last_send >= VNC_MIN_FRAME_MS) {
			int dx, dy, dw, dh, ix, iy, iw, ih;

			if (fb_take_dirty(&dx, &dy, &dw, &dh) &&
			    rect_isect(dx, dy, dw, dh, rq_x, rq_y, rq_w, rq_h,
				       &ix, &iy, &iw, &ih)) {
				if (send_rect(s, ix, iy, iw, ih, &fmt)) {
					return;
				}
				last_send = k_uptime_get();
				pending = false;
			}
		}

		/* 2. Wait for client data.  If an update is pending, wake
		 *    periodically to re-check the dirty box; otherwise block. */
		struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLIN };
		int pr = zsock_poll(&pfd, 1, pending ? VNC_POLL_MS : -1);

		if (pr < 0) {
			return;
		}
		if (pr == 0) {
			continue;       /* timeout — re-check pending */
		}

		/* 3. Read and handle one client message. */
		uint8_t type;

		if (recv_all(s, &type, 1)) {
			break;
		}
		switch (type) {
		case MSG_SET_PIXEL_FORMAT:        /* 3 pad + 16-byte format */
			if (recv_all(s, b, 19)) {
				return;
			}
			parse_pixfmt(&fmt, b + 3);
			build_luts(&fmt);
			LOG_INF("client pixel format: %u bpp, depth %u, %s-endian, "
				"truecolour=%u, max r%u/g%u/b%u, shift %u/%u/%u",
				fmt.bpp, fmt.depth,
				fmt.big_endian ? "big" : "little", fmt.true_colour,
				fmt.red_max, fmt.green_max, fmt.blue_max,
				fmt.red_shift, fmt.green_shift, fmt.blue_shift);
			if (!fmt.true_colour) {
				LOG_WRN("client requested palette mode (unsupported)");
			}
			break;

		case MSG_SET_ENCODINGS: {         /* 1 pad + count + count*s32 */
			if (recv_all(s, b, 3)) {
				return;
			}
			uint16_t n = rd16(b + 1);

			for (uint16_t i = 0; i < n; i++) {
				if (recv_all(s, b, 4)) {  /* ignore: Raw only */
					return;
				}
			}
			break;
		}

		case MSG_FB_UPDATE_REQ: {         /* incr + x + y + w + h */
			if (recv_all(s, b, 9)) {
				return;
			}
			int incr = b[0];
			int x = rd16(b + 1), y = rd16(b + 3);
			int w = rd16(b + 5), h = rd16(b + 7);

			/* Clamp to the framebuffer. */
			if (x > FB_WIDTH)  { x = FB_WIDTH;  }
			if (y > FB_HEIGHT) { y = FB_HEIGHT; }
			if (x + w > FB_WIDTH)  { w = FB_WIDTH - x;  }
			if (y + h > FB_HEIGHT) { h = FB_HEIGHT - y; }
			if (w <= 0 || h <= 0) {
				break;
			}
			if (!incr) {
				/* Non-incremental: full repaint now, and drop any
				 * dirt we just covered so it isn't re-sent. */
				int dx, dy, dw, dh;

				if (send_rect(s, x, y, w, h, &fmt)) {
					return;
				}
				last_send = k_uptime_get();
				(void)fb_take_dirty(&dx, &dy, &dw, &dh);
				pending = false;
			} else {
				/* Incremental: remember the region and answer from
				 * the dirty box (step 1) when something changes. */
				rq_x = x; rq_y = y; rq_w = w; rq_h = h;
				pending = true;
			}
			break;
		}

		case MSG_KEY_EVENT:               /* down + 2 pad + key  (7) */
			if (recv_all(s, b, 7)) {
				return;
			}
			break;

		case MSG_POINTER_EVENT:           /* mask + x + y        (5) */
			if (recv_all(s, b, 5)) {
				return;
			}
			break;

		case MSG_CLIENT_CUT_TEXT:         /* 3 pad + len + text  */
			if (recv_all(s, b, 7)) {
				return;
			}
			if (recv_discard(s, rd32(b + 3))) {
				return;
			}
			break;

		default:
			LOG_WRN("unknown client message type %u", type);
			return;
		}
	}
}

#ifdef CONFIG_KLAUSSCPU_VNC_DEMO
/* Placeholder content source: a two-tone box bouncing over the test pattern,
 * marking the framebuffer dirty so the dirty-rect/throttle path has something
 * to do.  Replace with the real (FPGA) framebuffer source in milestone 6+. */
#define DEMO_BOX 48
static void vnc_demo(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int x = 0, y = 0, dx = 4, dy = 3;

	for (;;) {
		fb_draw_pattern(x, y, DEMO_BOX, DEMO_BOX);   /* erase old box */

		x += dx;
		y += dy;
		if (x <= 0 || x >= FB_WIDTH - DEMO_BOX)  { dx = -dx; }
		if (y <= 0 || y >= FB_HEIGHT - DEMO_BOX) { dy = -dy; }

		/* White-bordered black box: visible over any background colour. */
		fb_fill_rect(x, y, DEMO_BOX, DEMO_BOX, fb_rgb(255, 255, 255));
		fb_fill_rect(x + 2, y + 2, DEMO_BOX - 4, DEMO_BOX - 4,
			     fb_rgb(0, 0, 0));

		k_sleep(K_MSEC(VNC_MIN_FRAME_MS));
	}
}
#endif /* CONFIG_KLAUSSCPU_VNC_DEMO */

static void vnc_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (srv < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(VNC_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind :%d failed: %d", VNC_PORT, -errno);
		(void)zsock_close(srv);
		return;
	}
	if (zsock_listen(srv, 1) < 0) {
		LOG_ERR("listen failed: %d", -errno);
		(void)zsock_close(srv);
		return;
	}

	LOG_INF("VNC server on :%d (%dx%d)", VNC_PORT, FB_WIDTH, FB_HEIGHT);

	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = zsock_accept(srv, (struct sockaddr *)&peer, &plen);

		if (cfd < 0) {
			continue;
		}
		serve_client(cfd);
		(void)zsock_close(cfd);
		LOG_INF("VNC client disconnected");
	}
}

void vnc_server_start(void)
{
	(void)k_thread_create(&vnc_thread, vnc_stack,
			      K_THREAD_STACK_SIZEOF(vnc_stack),
			      vnc_main, NULL, NULL, NULL,
			      VNC_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&vnc_thread, "vncd");

#ifdef CONFIG_KLAUSSCPU_VNC_DEMO
	fb_test_pattern();

	/* Placeholder animated content (see vnc_demo). */
	(void)k_thread_create(&demo_thread, demo_stack,
			      K_THREAD_STACK_SIZEOF(demo_stack),
			      vnc_demo, NULL, NULL, NULL,
			      VNC_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&demo_thread, "vncdemo");
#endif
}
