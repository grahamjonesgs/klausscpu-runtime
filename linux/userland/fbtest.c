/*
 * fbtest — /dev/fb0 checks and test patterns for the KlaussCPU VGA driver
 * (arch/klausscpu/kernel/klausscpu_fb.c).
 *
 *   fbtest info          the current mode (var and fix screeninfo)
 *   fbtest bars [WxHxB]  colour bars and a grey ramp, in the current mode or
 *                        WxHxB (e.g. 640x480x16, 320x240x16, 320x200x8);
 *                        8 bpp loads a palette.  The mode stays set.
 *   fbtest fill RRGGBB   the whole visible page in one colour (16 bpp)
 *   fbtest cmap          the palette as 256 lines "r g b" (0-255)
 *   fbtest shot          the visible page to stdout for linux/tools/fb2png.py:
 *                        a line "KLFB <xres> <yres> <bpp> <stride>", then for
 *                        8 bpp the palette (256 x r,g,b bytes), then yres
 *                        lines of <stride> bytes
 *   fbtest check         self-test: every mode, mmap and read() agree, pan
 *                        between pages, FBIO_WAITFORVSYNC, palette; prints
 *                        "fbtest: PASS" or FAIL lines.  Restores the mode.
 *   fbtest vsync [n]     time n (default 60) FBIO_WAITFORVSYNC calls
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fd;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static int fails;

#define FAIL(...) do { printf("fbtest: FAIL " __VA_ARGS__); putchar('\n'); fails++; } while (0)

static int get(void)
{
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("fbtest: FBIOGET_*SCREENINFO");
		return -1;
	}
	return 0;
}

static int set_mode(unsigned w, unsigned h, unsigned bpp, unsigned vh)
{
	struct fb_var_screeninfo v = var;

	v.xres = v.xres_virtual = w;
	v.yres = h;
	v.yres_virtual = vh ? vh : h;
	v.xoffset = v.yoffset = 0;
	v.bits_per_pixel = bpp;
	v.activate = FB_ACTIVATE_NOW;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &v))
		return -errno;
	return get();
}

static void info(void)
{
	printf("%s: %ux%u (virtual %ux%u, offset %u,%u) %u bpp, %s, stride %u\n",
	       fix.id, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
	       var.xoffset, var.yoffset, var.bits_per_pixel,
	       fix.visual == FB_VISUAL_PSEUDOCOLOR ? "palette" : "truecolor",
	       fix.line_length);
	printf("memory %08lx, %u KiB; red %u/%u green %u/%u blue %u/%u; ypanstep %u\n",
	       fix.smem_start, fix.smem_len / 1024, var.red.offset, var.red.length,
	       var.green.offset, var.green.length, var.blue.offset, var.blue.length,
	       fix.ypanstep);
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* 8 colour bars in the top 2/3, a grey ramp below; 8 bpp palette: 0-7 the
 * bars, 16-255 the ramp. */
static const unsigned bar_rgb[8] = {
	0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000,
};

static int load_palette(void)
{
	uint16_t r[256], g[256], b[256];
	struct fb_cmap cm = { 0, 256, r, g, b, NULL };

	for (int i = 0; i < 256; i++) {
		unsigned c = i < 8 ? bar_rgb[i] : i < 16 ? 0x808080 : (unsigned)((i - 16) * 255 / 239) * 0x010101;

		r[i] = (uint16_t)(((c >> 16) & 0xff) * 0x101);
		g[i] = (uint16_t)(((c >> 8) & 0xff) * 0x101);
		b[i] = (uint16_t)((c & 0xff) * 0x101);
	}
	if (ioctl(fd, FBIOPUTCMAP, &cm)) {
		perror("fbtest: FBIOPUTCMAP");
		return -1;
	}
	return 0;
}

static void draw_bars(uint8_t *page)
{
	unsigned w = var.xres, h = var.yres, split = h * 2 / 3;

	for (unsigned y = 0; y < h; y++) {
		uint8_t *row = page + (size_t)y * fix.line_length;

		for (unsigned x = 0; x < w; x++) {
			unsigned bar = x * 8 / w, grey = x * 255 / (w - 1);

			if (var.bits_per_pixel == 8) {
				row[x] = (uint8_t)(y < split ? bar : 16 + grey * 239 / 255);
			} else {
				unsigned c = y < split ? bar_rgb[bar] : grey * 0x010101u;

				((uint16_t *)row)[x] = rgb565(c >> 16, (c >> 8) & 0xff, c & 0xff);
			}
		}
	}
}

static uint8_t *map(void)
{
	void *p = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (p == MAP_FAILED) {
		perror("fbtest: mmap");
		return NULL;
	}
	return p;
}

static int parse_mode(const char *s, unsigned *w, unsigned *h, unsigned *b)
{
	return sscanf(s, "%ux%ux%u", w, h, b) == 3 ? 0 : -1;
}

/* A pattern that differs per page, mode and offset. */
static uint8_t pat(unsigned page, size_t i)
{
	return (uint8_t)((i * 7 + (i >> 9) + page * 0x5b) ^ 0xa5);
}

static void check_mode(unsigned w, unsigned h, unsigned bpp, unsigned pages)
{
	size_t stride, len;
	uint8_t *m, *buf;

	if (set_mode(w, h, bpp, h * pages)) {
		FAIL("mode %ux%ux%u x%u pages refused", w, h, bpp, pages);
		return;
	}
	stride = (size_t)w * bpp / 8;
	if (var.xres != w || var.yres != h || var.bits_per_pixel != bpp ||
	    var.yres_virtual != h * pages || fix.line_length != stride)
		FAIL("mode %ux%ux%u: got %ux%ux%u virtual %u stride %u", w, h, bpp,
		     var.xres, var.yres, var.bits_per_pixel, var.yres_virtual, fix.line_length);
	if (fix.visual != (bpp == 8 ? FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR))
		FAIL("mode %ux%ux%u: visual %u", w, h, bpp, fix.visual);
	m = map();
	if (!m) {
		fails++;
		return;
	}
	len = stride * h;
	for (unsigned p = 0; p < pages; p++)
		for (size_t i = 0; i < len; i++)
			m[p * len + i] = pat(p + w + bpp, i);
	/* read() sees what mmap wrote */
	buf = malloc(len * pages);
	if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, buf, len * pages) != (ssize_t)(len * pages))
		FAIL("read() of %zu bytes", len * pages);
	else
		for (unsigned p = 0; p < pages; p++)
			for (size_t i = 0; i < len; i++)
				if (buf[p * len + i] != pat(p + w + bpp, i)) {
					FAIL("mode %ux%ux%u page %u byte %zu: read %02x", w, h, bpp, p, i,
					     buf[p * len + i]);
					p = pages;
					break;
				}
	/* write() shows through the mapping */
	memset(buf, 0x3c, 4096);
	if (lseek(fd, 100, SEEK_SET) != 100 || write(fd, buf, 4096) != 4096)
		FAIL("write() of 4096 bytes");
	else if (m[99] != pat(w + bpp, 99) || m[100] != 0x3c || m[4195] != 0x3c ||
		 m[4196] != pat(w + bpp, 4196))
		FAIL("write() not seen in the mapping");
	free(buf);
	/* flip to each page and back */
	for (unsigned p = 0; p < pages; p++) {
		struct fb_var_screeninfo v = var;

		v.yoffset = p * h;
		if (ioctl(fd, FBIOPAN_DISPLAY, &v))
			FAIL("pan to page %u: %s", p, strerror(errno));
	}
	{
		struct fb_var_screeninfo v = var;

		v.yoffset = pages * h;
		if (ioctl(fd, FBIOPAN_DISPLAY, &v) == 0)
			FAIL("pan past the last page accepted");
	}
	{
		__u32 crtc = 0;

		if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc))
			FAIL("FBIO_WAITFORVSYNC: %s", strerror(errno));
	}
	if (bpp == 8 && load_palette())
		fails++;
	munmap(m, fix.smem_len);
	printf("fbtest: %ux%ux%u, %u page%s: ok\n", w, h, bpp, pages, pages > 1 ? "s" : "");
}

static int check(void)
{
	struct fb_var_screeninfo saved = var;

	check_mode(640, 480, 16, 2);
	check_mode(320, 240, 16, 2);
	check_mode(640, 480, 8, 3);
	check_mode(320, 200, 8, 3);
	check_mode(320, 240, 8, 1);
	/* refused: too wide, too tall, too large a virtual screen */
	if (set_mode(800, 600, 16, 0) == 0)
		FAIL("800x600 accepted");
	if (set_mode(640, 481, 16, 0) == 0)
		FAIL("640x481 accepted");
	if (set_mode(320, 241, 8, 0) == 0)
		FAIL("320x241 (doubled) accepted");
	if (set_mode(640, 480, 16, 480 * 3) == 0)
		FAIL("640x480x16 with 3 pages accepted");
	saved.activate = FB_ACTIVATE_NOW;
	saved.yoffset = 0;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &saved))
		FAIL("restoring the mode");
	get();
	puts(fails ? "fbtest: FAILED" : "fbtest: PASS");
	return fails ? 1 : 0;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "info";
	const char *dev = getenv("FBDEV") ? getenv("FBDEV") : "/dev/fb0";

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "fbtest: %s: %s\n", dev, strerror(errno));
		return 2;
	}
	if (get())
		return 2;
	if (!strcmp(cmd, "info")) {
		info();
	} else if (!strcmp(cmd, "bars")) {
		uint8_t *m;

		if (argc > 2) {
			unsigned w, h, b;
			int e;

			if (parse_mode(argv[2], &w, &h, &b)) {
				fprintf(stderr, "fbtest: mode is WxHxBPP\n");
				return 2;
			}
			e = set_mode(w, h, b, 0);
			if (e) {
				fprintf(stderr, "fbtest: %s refused: %s\n", argv[2], strerror(-e));
				return 1;
			}
		}
		if (var.bits_per_pixel == 8 && load_palette())
			return 1;
		m = map();
		if (!m)
			return 1;
		draw_bars(m + (size_t)var.yoffset * fix.line_length);
		munmap(m, fix.smem_len);
		info();
	} else if (!strcmp(cmd, "fill") && argc > 2) {
		unsigned c = (unsigned)strtoul(argv[2], NULL, 16);
		uint8_t *m;

		if (var.bits_per_pixel != 16) {
			fprintf(stderr, "fbtest: fill needs a 16 bpp mode\n");
			return 1;
		}
		m = map();
		if (!m)
			return 1;
		for (unsigned y = 0; y < var.yres; y++) {
			uint16_t *row = (uint16_t *)(m + (size_t)(y + var.yoffset) * fix.line_length);

			for (unsigned x = 0; x < var.xres; x++)
				row[x] = rgb565(c >> 16, (c >> 8) & 0xff, c & 0xff);
		}
		munmap(m, fix.smem_len);
	} else if (!strcmp(cmd, "cmap")) {
		uint16_t r[256], g[256], b[256];
		struct fb_cmap cm = { 0, 256, r, g, b, NULL };

		if (ioctl(fd, FBIOGETCMAP, &cm)) {
			perror("fbtest: FBIOGETCMAP");
			return 1;
		}
		for (int i = 0; i < 256; i++)
			printf("%u %u %u\n", r[i] >> 8, g[i] >> 8, b[i] >> 8);
	} else if (!strcmp(cmd, "shot")) {
		size_t len = (size_t)fix.line_length * var.yres;
		uint8_t *buf = malloc(len);

		printf("KLFB %u %u %u %u\n", var.xres, var.yres, var.bits_per_pixel, fix.line_length);
		if (var.bits_per_pixel == 8) {
			uint16_t r[256], g[256], b[256];
			struct fb_cmap cm = { 0, 256, r, g, b, NULL };

			if (ioctl(fd, FBIOGETCMAP, &cm)) {
				perror("fbtest: FBIOGETCMAP");
				return 1;
			}
			for (int i = 0; i < 256; i++)
				printf("%c%c%c", r[i] >> 8, g[i] >> 8, b[i] >> 8);
		}
		fflush(stdout);
		if (!buf || pread(fd, buf, len, (off_t)var.yoffset * fix.line_length) != (ssize_t)len ||
		    write(1, buf, len) != (ssize_t)len) {
			perror("fbtest: shot");
			return 1;
		}
	} else if (!strcmp(cmd, "check")) {
		return check();
	} else if (!strcmp(cmd, "vsync")) {
		int n = argc > 2 ? atoi(argv[2]) : 60;
		__u32 crtc = 0;
		double t0 = now();

		for (int i = 0; i < n; i++)
			if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc)) {
				perror("fbtest: FBIO_WAITFORVSYNC");
				return 1;
			}
		printf("%d vsyncs in %.3f s: %.2f Hz\n", n, now() - t0, n / (now() - t0));
	} else {
		fprintf(stderr, "usage: fbtest info|bars [WxHxBPP]|fill RRGGBB|cmap|shot|check|vsync [n]\n");
		return 2;
	}
	return 0;
}
