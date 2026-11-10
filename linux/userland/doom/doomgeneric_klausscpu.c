/*
 * doomgeneric_klausscpu.c — doomgeneric on Linux fbdev, for KlaussCPU's VGA
 * port (arch/klausscpu/kernel/klausscpu_fb.c).  Built by
 * linux/userland/doom.sh with CMAP256 at 320x200.
 *
 * Display: /dev/fb0 (DOOM_FBDEV) is switched to 320x200, 8 bpp, three
 * pages (the VGA doubles it to 640x400, centred).  Doom's 8-bit frames go
 * into the mapped pages directly: DG_ScreenBuffer points at a page that is
 * not on screen, I_FinishUpdate renders into it, and DG_DrawFrame flips to
 * it with FBIOPAN_DISPLAY (the driver flushes the cache; the VGA latches
 * the page at the next vblank) and loads the palette when it changes.  On a
 * frame buffer that refuses that mode, frames are converted to its 16 or
 * 32 bpp format, scaled by 1 or 2 and centred.  The mode is restored at
 * exit.
 *
 * Input: the terminal (UART console or an SSH session), in raw mode.  A
 * terminal reports presses only, so a key is released when it hasn't
 * repeated for a moment.  Arrows move and turn, z or , fire, space opens,
 * Enter/Esc work the menus, Tab is the map, 1-7 pick weapons, letters pass
 * through (y/n, cheats).  Ctrl-C quits.  Without a terminal, no input:
 * Doom plays its demos.
 *
 * The IWAD: -iwad FILE, else the first of /mnt/DOOM1.WAD (the SD card's
 * FAT root under the SD root file system), /mnt/sd/DOOM1.WAD, ./doom1.wad.
 * Config and saves go to the current directory, or $HOME if that is
 * read-only.  DOOM_FPS=1 prints the frame rate every 10 s.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"	/* colors[], palette_changed (CMAP256) */

#define W DOOMGENERIC_RESX
#define H DOOMGENERIC_RESY
#define PAGES 3

static int fb = -1;
static struct fb_var_screeninfo var, saved_var;
static struct fb_fix_screeninfo fix;
static int var_changed;
static uint8_t *fbmem;
static size_t fblen;
static int direct;		/* 8 bpp pages: zero-copy flips */
static int page;		/* the page DG_ScreenBuffer points at */
static int scale, xoff, yoff;	/* converted output */
static uint32_t lut[256];	/* palette in the frame buffer's format */

static int tty = -1;
static struct termios saved_tio;
static int tio_changed;

static int show_fps;
static unsigned frames;
static uint32_t fps_t0;

static uint32_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}

static void restore(void)
{
	if (tio_changed) {
		tcsetattr(tty, TCSANOW, &saved_tio);
		tio_changed = 0;
	}
	if (var_changed) {
		if (fbmem && fbmem != MAP_FAILED)
			memset(fbmem, 0, fblen);	/* no stale frame in the old mode */
		saved_var.activate = FB_ACTIVATE_NOW;
		saved_var.yoffset = 0;
		ioctl(fb, FBIOPUT_VSCREENINFO, &saved_var);
		var_changed = 0;
	}
}

static void on_signal(int sig)
{
	restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

/* ── display ─────────────────────────────────────────────────────────────── */

static int set_direct_mode(void)
{
	struct fb_var_screeninfo v = var;

	v.xres = v.xres_virtual = W;
	v.yres = H;
	v.yres_virtual = H * PAGES;
	v.xoffset = v.yoffset = 0;
	v.bits_per_pixel = 8;
	v.activate = FB_ACTIVATE_NOW;
	if (ioctl(fb, FBIOPUT_VSCREENINFO, &v) != 0)
		return -1;
	var_changed = 1;
	if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
		return -1;
	if (var.xres != W || var.yres != H || var.bits_per_pixel != 8 ||
	    var.yres_virtual < H * PAGES || fix.line_length != W ||
	    fix.visual != FB_VISUAL_PSEUDOCOLOR)
		return -1;
	return 0;
}

static void open_fb(void)
{
	const char *dev = getenv("DOOM_FBDEV") ? getenv("DOOM_FBDEV") : "/dev/fb0";

	fb = open(dev, O_RDWR);
	if (fb < 0) {
		fprintf(stderr, "doom: %s: %s (no frame buffer: needs a bitstream with the VGA "
			"port, or klausscpu_fb.force=1 to test without one)\n", dev, strerror(errno));
		exit(1);
	}
	if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "doom: %s: not a frame buffer: %s\n", dev, strerror(errno));
		exit(1);
	}
	saved_var = var;
	direct = set_direct_mode() == 0;
	if (!direct) {
		/* Use whatever mode it has. */
		if (var_changed) {
			restore();
			ioctl(fb, FBIOGET_VSCREENINFO, &var);
			ioctl(fb, FBIOGET_FSCREENINFO, &fix);
		}
		if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
			fprintf(stderr, "doom: %s: %u bpp, and 320x200x8 refused\n", dev,
				var.bits_per_pixel);
			exit(1);
		}
		scale = (var.xres >= 2 * W && var.yres >= 2 * H) ? 2 : 1;
		if (var.xres < W || var.yres < H) {
			fprintf(stderr, "doom: %s: %ux%u is smaller than %ux%u\n", dev, var.xres,
				var.yres, W, H);
			exit(1);
		}
		xoff = (var.xres - W * scale) / 2;
		yoff = (var.yres - H * scale) / 2;
	}
	fblen = fix.smem_len;
	fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fbmem == MAP_FAILED) {
		fprintf(stderr, "doom: mmap %s: %s\n", dev, strerror(errno));
		restore();
		exit(1);
	}
	if (direct) {
		memset(fbmem, 0, (size_t)W * H * PAGES);
		page = 1;
		DG_ScreenBuffer = fbmem + (size_t)page * W * H;
	} else {
		/* Black around the picture, on the visible page. */
		memset(fbmem + (size_t)var.yoffset * fix.line_length, 0,
		       (size_t)var.yres * fix.line_length);
	}
	printf("doom: %s %ux%u %u bpp, %s\n", fix.id, var.xres, var.yres, var.bits_per_pixel,
	       direct ? "8-bit pages, flipped" : scale == 2 ? "converted, doubled" : "converted");
}

static void load_palette(void)
{
	uint16_t r[256], g[256], b[256];
	struct fb_cmap cm = { 0, 256, r, g, b, NULL };

	for (int i = 0; i < 256; i++) {
		r[i] = (uint16_t)(colors[i].r * 0x101);
		g[i] = (uint16_t)(colors[i].g * 0x101);
		b[i] = (uint16_t)(colors[i].b * 0x101);
		if (var.bits_per_pixel == 16)
			lut[i] = (uint32_t)((colors[i].r >> 3) << 11 | (colors[i].g >> 2) << 5 |
					    colors[i].b >> 3);
		else
			lut[i] = (uint32_t)colors[i].r << var.red.offset |
				 (uint32_t)colors[i].g << var.green.offset |
				 (uint32_t)colors[i].b << var.blue.offset;
	}
	if (direct)
		ioctl(fb, FBIOPUTCMAP, &cm);
}

static void convert(void)
{
	const uint8_t *src = DG_ScreenBuffer;
	int bpp = var.bits_per_pixel / 8;

	for (int y = 0; y < H; y++) {
		const uint8_t *s = src + (size_t)y * W;

		for (int dy = 0; dy < scale; dy++) {
			uint8_t *row = fbmem + (size_t)(var.yoffset + yoff + y * scale + dy) *
						       fix.line_length + (size_t)xoff * bpp;

			if (bpp == 2) {
				uint16_t *d = (uint16_t *)row;

				for (int x = 0; x < W; x++)
					for (int k = 0; k < scale; k++)
						*d++ = (uint16_t)lut[s[x]];
			} else {
				uint32_t *d = (uint32_t *)row;

				for (int x = 0; x < W; x++)
					for (int k = 0; k < scale; k++)
						*d++ = lut[s[x]];
			}
		}
	}
}

void DG_Init(void)
{
	open_fb();
}

void DG_DrawFrame(void)
{
	if (palette_changed) {
		load_palette();
		palette_changed = false;
	}
	if (direct) {
		struct fb_var_screeninfo v = var;

		v.yoffset = (uint32_t)page * H;
		ioctl(fb, FBIOPAN_DISPLAY, &v);
		page = (page + 1) % PAGES;
		DG_ScreenBuffer = fbmem + (size_t)page * W * H;
	} else {
		convert();
	}
	if (show_fps) {
		uint32_t t = now_ms();

		frames++;
		if (t - fps_t0 >= 10000) {
			printf("doom: %u frames in %.1f s, %.1f fps\n", frames, (t - fps_t0) / 1000.0,
			       frames * 1000.0 / (t - fps_t0));
			fflush(stdout);
			frames = 0;
			fps_t0 = t;
		}
	}
}

void DG_SleepMs(uint32_t ms)
{
	usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
	return now_ms();
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

/* ── keyboard ────────────────────────────────────────────────────────────── */

#define QLEN 64
static uint16_t queue[QLEN];	/* pressed << 8 | key */
static unsigned qhead, qtail;

/* Keys held down, and when each was last seen. */
#define HELD 8
static struct {
	unsigned char key;
	uint32_t seen;
	int repeated;
} held[HELD];

#define RELEASE_MS 180		/* a tap */
#define REPEAT_RELEASE_MS 100	/* after autorepeat has started */

static void push(int pressed, unsigned char key)
{
	if (qtail - qhead < QLEN)
		queue[qtail++ % QLEN] = (uint16_t)(pressed << 8 | key);
}

static void press(unsigned char key, uint32_t t)
{
	int free = -1;

	for (int i = 0; i < HELD; i++) {
		if (held[i].key == key) {
			held[i].seen = t;
			held[i].repeated = 1;
			return;
		}
		if (!held[i].key && free < 0)
			free = i;
	}
	if (free < 0)
		return;
	held[free].key = key;
	held[free].seen = t;
	held[free].repeated = 0;
	push(1, key);
}

static unsigned char map_char(unsigned char c)
{
	switch (c) {
	case '\r':
	case '\n':
		return KEY_ENTER;
	case '\t':
		return KEY_TAB;
	case ' ':
		return KEY_USE;
	case ',':
	case 'z':
		return KEY_FIRE;
	case 0x7f:
	case 0x08:
		return KEY_BACKSPACE;
	}
	if (c >= 'A' && c <= 'Z')
		return (unsigned char)(c + 32);
	if (c >= 0x20 && c < 0x7f)
		return c;
	return 0;
}

static void poll_keys(void)
{
	unsigned char buf[64];
	uint32_t t = now_ms();
	ssize_t n;

	if (tty < 0)
		return;
	n = read(tty, buf, sizeof(buf));
	for (ssize_t i = 0; i < n; i++) {
		unsigned char k = 0;

		if (buf[i] == 0x1b) {
			if (i + 2 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
				switch (buf[i + 2]) {
				case 'A': k = KEY_UPARROW; break;
				case 'B': k = KEY_DOWNARROW; break;
				case 'C': k = KEY_RIGHTARROW; break;
				case 'D': k = KEY_LEFTARROW; break;
				}
				i += 2;
			} else {
				k = KEY_ESCAPE;
			}
		} else {
			k = map_char(buf[i]);
		}
		if (k)
			press(k, t);
	}
	for (int i = 0; i < HELD; i++)
		if (held[i].key &&
		    t - held[i].seen > (uint32_t)(held[i].repeated ? REPEAT_RELEASE_MS : RELEASE_MS)) {
			push(0, held[i].key);
			held[i].key = 0;
		}
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	if (qhead == qtail)
		poll_keys();
	if (qhead == qtail)
		return 0;
	*pressed = queue[qhead % QLEN] >> 8;
	*key = (unsigned char)queue[qhead % QLEN];
	qhead++;
	return 1;
}

static void open_tty(void)
{
	struct termios tio;

	if (!isatty(0) || tcgetattr(0, &saved_tio) != 0)
		return;
	tty = 0;
	tio = saved_tio;
	tio.c_lflag &= ~(ICANON | ECHO);	/* ISIG stays: Ctrl-C quits */
	tio.c_iflag &= ~(ICRNL | IXON);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &tio) == 0)
		tio_changed = 1;
}

/* ── main ────────────────────────────────────────────────────────────────── */

static const char *find_iwad(void)
{
	static const char *const paths[] = {
		"/mnt/DOOM1.WAD", "/mnt/doom1.wad", "/mnt/sd/DOOM1.WAD", "/mnt/sd/doom1.wad",
		"doom1.wad", "DOOM1.WAD",
	};

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
		if (access(paths[i], R_OK) == 0)
			return paths[i];
	return NULL;
}

int main(int argc, char **argv)
{
	char **args = calloc((size_t)argc + 3, sizeof(*args));
	int n = 0, have_iwad = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < argc; i++) {
		have_iwad |= i > 0 && !strcmp(argv[i], "-iwad");
		args[n++] = argv[i];
	}
	if (!have_iwad) {
		const char *wad = find_iwad();

		if (!wad) {
			fprintf(stderr, "doom: no IWAD: put DOOM1.WAD at the SD card's root "
				"(/mnt/DOOM1.WAD), or give -iwad FILE\n");
			return 1;
		}
		args[n++] = "-iwad";
		args[n++] = (char *)wad;
	}
	args[n] = NULL;
	if (access(".", W_OK) != 0 && getenv("HOME") && chdir(getenv("HOME")) != 0)
		perror("doom: chdir $HOME");
	show_fps = getenv("DOOM_FPS") != NULL;
	fps_t0 = now_ms();

	atexit(restore);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	open_tty();

	doomgeneric_Create(n, args);
	for (;;)
		doomgeneric_Tick();
}
