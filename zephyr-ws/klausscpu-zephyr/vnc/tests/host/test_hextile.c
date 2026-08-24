/*
 * Host-side harness: compiles the REAL vnc_server.c (stub Zephyr headers) and
 * verifies the new paths:
 *   1. hextile_tile fuzz — 200k random tiles (solid / two-colour / noise /
 *      mixed, all client formats, all edge sizes) encoded then decoded by an
 *      independent RFC 6143 reference decoder; every pixel compared.
 *   2. send_rect_hextile — full-frame + unaligned sub-rect updates parsed off
 *      a capture socket (exercises the sendbuf flush path), plus the adaptive
 *      ht_skip fallback and the send_update dispatcher.
 *   3. send_rect zero-copy — full-width native updates byte-compared against
 *      the framebuffer; non-full-width staging path re-verified.
 * The capture socket also asserts fb_lock is NEVER held across a send.
 */
#define CONFIG_KLAUSSCPU_VNC_HEXTILE 1
#define CONFIG_KLAUSSCPU_VNC_ZERO_COPY 1

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "vnc_server.c"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ── framebuffer / kernel / socket stubs ─────────────────────────────────── */

static uint16_t FB[FB_WIDTH * FB_HEIGHT];
static int lock_depth;

uint16_t *fb_pixels(void) { return FB; }
void fb_lock(void)   { lock_depth++; }
void fb_unlock(void) { lock_depth--; assert(lock_depth >= 0); }
bool fb_take_dirty(int *x, int *y, int *w, int *h)
{
	(void)x; (void)y; (void)w; (void)h;
	return false;
}
void fb_mark_dirty(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void fb_test_pattern(void) {}
void fb_draw_pattern(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void fb_fill_rect(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void fb_clear(uint16_t c) { (void)c; }
void fb_put_pixel(int x, int y, uint16_t c) { (void)x; (void)y; (void)c; }

int64_t k_uptime_get(void) { static int64_t t; return t++; }
int printk(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}

static uint8_t cap[8 * 1024 * 1024];
static size_t cap_len;

ssize_t zsock_send(int s, const void *buf, size_t n, int flags)
{
	(void)s; (void)flags;
	assert(lock_depth == 0 && "fb_lock held across a send!");
	assert(cap_len + n <= sizeof(cap));
	memcpy(cap + cap_len, buf, n);
	cap_len += n;
	return (ssize_t)n;
}
ssize_t zsock_recv(int s, void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; abort(); }
int zsock_socket(int a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
int zsock_bind(int s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return -1; }
int zsock_listen(int s, int b) { (void)s; (void)b; return -1; }
int zsock_accept(int s, struct sockaddr *a, socklen_t *l) { (void)s; (void)a; (void)l; return -1; }
int zsock_close(int s) { (void)s; return 0; }
int zsock_poll(struct zsock_pollfd *p, int n, int t) { (void)p; (void)n; (void)t; return -1; }

/* ── reference decoder ───────────────────────────────────────────────────── */

static uint32_t rd_px(const uint8_t *p, int bytes, bool be)
{
	uint32_t v = 0;

	for (int i = 0; i < bytes; i++) {
		v |= (uint32_t)p[i] << (be ? 8 * (bytes - 1 - i) : 8 * i);
	}
	return v;
}

/* Decode one tile; returns bytes consumed, fills dec[] (client-space values). */
static size_t dec_tile(const uint8_t *in, size_t avail, uint32_t *dec,
		       int tw, int th, int bytes, bool be)
{
	assert(avail >= 1);
	uint8_t sub = in[0];
	const uint8_t *p = in + 1;

	if (sub & HT_RAW) {
		assert(sub == HT_RAW);
		for (int i = 0; i < tw * th; i++) {
			dec[i] = rd_px(p, bytes, be);
			p += bytes;
		}
		assert((size_t)(p - in) <= avail);
		return (size_t)(p - in);
	}
	assert(sub & HT_BG);
	uint32_t bg = rd_px(p, bytes, be);

	p += bytes;
	for (int i = 0; i < tw * th; i++) {
		dec[i] = bg;
	}
	if (sub & HT_FG) {
		assert(sub == (HT_BG | HT_FG | HT_SUBRECTS));
		uint32_t fg = rd_px(p, bytes, be);

		p += bytes;
		int n = *p++;

		for (int i = 0; i < n; i++) {
			int x = p[0] >> 4, y = p[0] & 15;
			int w = (p[1] >> 4) + 1, h = (p[1] & 15) + 1;

			p += 2;
			assert(x + w <= tw && y + h <= th);
			for (int r = 0; r < h; r++) {
				for (int c = 0; c < w; c++) {
					dec[(y + r) * tw + x + c] = fg;
				}
			}
		}
	} else {
		assert(sub == HT_BG);
	}
	assert((size_t)(p - in) <= avail);
	return (size_t)(p - in);
}

static uint32_t px_mask(int bytes)
{
	return bytes == 4 ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
}

/* ── client formats under test ───────────────────────────────────────────── */

static const struct pixfmt fmts[] = {
	{ 16, 16, 0, 1, 31, 63, 31, 11, 5, 0 },     /* native RGB565 LE  */
	{ 16, 16, 1, 1, 31, 63, 31, 11, 5, 0 },     /* RGB565 big-endian */
	{ 32, 24, 0, 1, 255, 255, 255, 16, 8, 0 },  /* xRGB8888 LE       */
	{ 32, 24, 1, 1, 255, 255, 255, 16, 8, 0 },  /* xRGB8888 BE       */
	{ 8, 6, 0, 1, 3, 3, 3, 4, 2, 0 },           /* RGB222            */
};

static uint32_t rnd_state = 0x4d7c1b74u;
static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1664525u + 1013904223u;
	return rnd_state >> 8;
}

/* ── test 1: tile fuzz ───────────────────────────────────────────────────── */

static void fuzz_tiles(void)
{
	for (long it = 0; it < 200000; it++) {
		const struct pixfmt *f = &fmts[rnd() % ARRAY_SIZE(fmts)];
		int bytes = f->bpp / 8;

		build_luts(f);

		int tw = 1 + (int)(rnd() % 16), th = 1 + (int)(rnd() % 16);
		int tx = (int)(rnd() % (FB_WIDTH - tw + 1));
		int ty = (int)(rnd() % (FB_HEIGHT - th + 1));
		int mode = (int)(rnd() % 4);
		uint16_t cA = (uint16_t)rnd();
		uint16_t cB = (uint16_t)rnd();

		if (cB == cA) {
			cB ^= 0x1F;
		}
		for (int r = 0; r < th; r++) {
			for (int c = 0; c < tw; c++) {
				uint16_t px;

				switch (mode) {
				case 0:  px = cA; break;
				case 1:  px = (rnd() & 1) ? cA : cB; break;
				case 2:  px = (uint16_t)rnd(); break;
				default: px = (c < tw / 2) ? cA : (uint16_t)rnd(); break;
				}
				FB[(size_t)(ty + r) * FB_WIDTH + tx + c] = px;
			}
		}

		uint8_t out[HT_TILE_MAX + 16];

		memset(out + HT_TILE_MAX, 0xA5, 16);
		size_t len = hextile_tile(out, tx, ty, tw, th, bytes, f);

		assert(len <= HT_TILE_MAX);
		for (int i = 0; i < 16; i++) {
			assert(out[HT_TILE_MAX + i] == 0xA5);   /* no overrun */
		}

		uint32_t dec[16 * 16];
		size_t used = dec_tile(out, len, dec, tw, th, bytes,
				       f->big_endian);

		assert(used == len);
		for (int r = 0; r < th; r++) {
			for (int c = 0; c < tw; c++) {
				uint16_t px = FB[(size_t)(ty + r) * FB_WIDTH + tx + c];
				uint32_t want = convert_pixel(px) & px_mask(bytes);

				if (dec[r * tw + c] != want) {
					fprintf(stderr,
						"tile mismatch it=%ld mode=%d fmt=%dbpp be=%d "
						"tile=%dx%d @(%d,%d) px(%d,%d): got %08x want %08x\n",
						it, mode, f->bpp, f->big_endian,
						tw, th, tx, ty, c, r,
						dec[r * tw + c], want);
					abort();
				}
				/* Native format must be a bit-exact identity
				 * (raw tiles memcpy; colours go via the LUTs —
				 * the two must agree). */
				if (f == &fmts[0]) {
					assert(convert_pixel(px) == px);
				}
			}
		}
	}
	printf("PASS: 200000-tile fuzz (all formats, all sizes)\n");
}

/* ── test 2: full send_rect_hextile + dispatcher ─────────────────────────── */

static void fill_fb_blocks(void)
{
	for (int by = 0; by < FB_HEIGHT; by += 16) {
		for (int bx = 0; bx < FB_WIDTH; bx += 16) {
			int mode = (int)(rnd() % 3);
			uint16_t cA = (uint16_t)rnd(), cB = (uint16_t)~cA;

			for (int r = by; r < MIN(by + 16, FB_HEIGHT); r++) {
				for (int c = bx; c < MIN(bx + 16, FB_WIDTH); c++) {
					FB[(size_t)r * FB_WIDTH + c] =
						mode == 0 ? cA :
						mode == 1 ? ((rnd() & 1) ? cA : cB) :
							    (uint16_t)rnd();
				}
			}
		}
	}
}

static void check_hextile_update(int x, int y, int w, int h,
				 const struct pixfmt *f)
{
	int bytes = f->bpp / 8;

	build_luts(f);
	cap_len = 0;
	assert(send_rect_hextile(0, x, y, w, h, f) == 0);

	/* FramebufferUpdate header + rectangle header. */
	assert(cap_len >= 16);
	assert(cap[0] == 0 && cap[2] == 0 && cap[3] == 1);
	assert(rd16(cap + 4) == x && rd16(cap + 6) == y);
	assert(rd16(cap + 8) == w && rd16(cap + 10) == h);
	assert(rd32(cap + 12) == 5);

	size_t off = 16;

	for (int ty = y; ty < y + h; ty += 16) {
		int th = MIN(16, y + h - ty);

		for (int tx = x; tx < x + w; tx += 16) {
			int tw = MIN(16, x + w - tx);
			uint32_t dec[16 * 16];

			off += dec_tile(cap + off, cap_len - off, dec, tw, th,
					bytes, f->big_endian);
			for (int r = 0; r < th; r++) {
				for (int c = 0; c < tw; c++) {
					uint16_t px = FB[(size_t)(ty + r) * FB_WIDTH + tx + c];

					assert(dec[r * tw + c] ==
					       (convert_pixel(px) & px_mask(bytes)));
				}
			}
		}
	}
	assert(off == cap_len);   /* stream fully consumed, nothing extra */
}

static void test_send_hextile(void)
{
	fill_fb_blocks();
	ht_skip = 0;
	check_hextile_update(0, 0, FB_WIDTH, FB_HEIGHT, &fmts[3]); /* 32bpp BE: forces flushes */
	ht_skip = 0;
	check_hextile_update(0, 0, FB_WIDTH, FB_HEIGHT, &fmts[0]);
	ht_skip = 0;
	check_hextile_update(7, 9, MIN(613, FB_WIDTH - 7), MIN(461, FB_HEIGHT - 9),
			     &fmts[0]);                            /* unaligned rect */
	ht_skip = 0;
	check_hextile_update(5, 6, 1, 1, &fmts[2]);                /* degenerate */
	printf("PASS: send_rect_hextile full/unaligned/degenerate updates\n");

	/* Adaptive fallback: noise must trip ht_skip; solid must not. */
	for (size_t i = 0; i < (size_t)FB_WIDTH * FB_HEIGHT; i++) {
		FB[i] = (uint16_t)rnd();
	}
	ht_skip = 0;
	check_hextile_update(0, 0, FB_WIDTH, FB_HEIGHT, &fmts[0]);
	assert(ht_skip == HT_RETRY);

	client_hextile = true;   /* dispatcher: skip counts down through Raw */
	cap_len = 0;
	assert(send_update(0, 0, 0, FB_WIDTH, FB_HEIGHT, &fmts[0]) == 0);
	assert(rd32(cap + 12) == 0 && ht_skip == HT_RETRY - 1);

	for (size_t i = 0; i < (size_t)FB_WIDTH * FB_HEIGHT; i++) {
		FB[i] = 0x1234;
	}
	ht_skip = 0;
	check_hextile_update(0, 0, FB_WIDTH, FB_HEIGHT, &fmts[0]);
	assert(ht_skip == 0);
	assert(cap_len < 16 + (size_t)((FB_WIDTH + 15) / 16) *
			      ((FB_HEIGHT + 15) / 16) * 4);   /* ~3 B/tile */
	printf("PASS: adaptive fallback (noise trips, solid doesn't; %zu B solid frame)\n",
	       cap_len);
}

/* ── test 3: zero-copy + staged raw paths ────────────────────────────────── */

static void test_send_raw(void)
{
	fill_fb_blocks();
	build_luts(&fmts[0]);
	assert(fmt_native);

	/* Full-width native: zero-copy — byte-identical to the framebuffer. */
	cap_len = 0;
	assert(send_rect(0, 0, 3, FB_WIDTH, 5, &fmts[0]) == 0);
	assert(rd32(cap + 12) == 0);
	assert(cap_len == 16 + (size_t)FB_WIDTH * 5 * 2);
	assert(memcmp(cap + 16, FB + (size_t)3 * FB_WIDTH,
		      (size_t)FB_WIDTH * 5 * 2) == 0);

	/* Non-full-width native: the staged path still works. */
	cap_len = 0;
	assert(send_rect(0, 1, 3, FB_WIDTH - 2, 5, &fmts[0]) == 0);
	assert(cap_len == 16 + (size_t)(FB_WIDTH - 2) * 5 * 2);
	for (int r = 0; r < 5; r++) {
		assert(memcmp(cap + 16 + (size_t)r * (FB_WIDTH - 2) * 2,
			      FB + (size_t)(3 + r) * FB_WIDTH + 1,
			      (size_t)(FB_WIDTH - 2) * 2) == 0);
	}

	/* Converted path (32bpp BE), spot-check decode. */
	build_luts(&fmts[3]);
	cap_len = 0;
	assert(send_rect(0, 2, 4, 10, 3, &fmts[3]) == 0);
	assert(cap_len == 16 + (size_t)10 * 3 * 4);
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 10; c++) {
			uint32_t got = rd_px(cap + 16 + ((size_t)r * 10 + c) * 4, 4, true);
			uint16_t px = FB[(size_t)(4 + r) * FB_WIDTH + 2 + c];

			assert(got == convert_pixel(px));
		}
	}
	printf("PASS: send_rect zero-copy / staged / converted paths\n");
}

int main(void)
{
	fuzz_tiles();
	test_send_hextile();
	test_send_raw();
	assert(lock_depth == 0);
	printf("ALL PASS\n");
	return 0;
}
