/*
 * doomgeneric_zephyr.c — KlaussCPU/Zephyr platform backend for doomgeneric.
 *
 * Implements the six DG_* hooks doomgeneric needs and provides the Zephyr app
 * main(): mount the SD card (for the WAD), get an IP (DHCP), start the VNC
 * server (the display), then run the Doom game loop on a dedicated thread.
 *
 * Doom renders into DG_ScreenBuffer (640x400, XRGB8888); DG_DrawFrame converts
 * that to the VNC module's RGB565 framebuffer, centred vertically in 640x480.
 * Keyboard input is wired: the VNC server delivers RFB KeyEvents (X11 keysyms)
 * to on_key(), which maps them to Doom key codes and queues them for DG_GetKey()
 * — so the game is playable over VNC, not just attract-mode demos.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#ifdef CONFIG_NETWORKING
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#endif

#include "framebuffer.h"
#ifdef CONFIG_KLAUSSCPU_VNC_SERVER
#include "vnc_server.h"
#endif
#ifdef CONFIG_KLAUSSCPU_AMP_VNC
#include "amp_host.h"        /* VNC served by AMP core 2 (P4b) */
#endif
#include "doomgeneric.h"
#include "doomkeys.h"

LOG_MODULE_REGISTER(doom, LOG_LEVEL_INF);

/* Centre Doom's image in the framebuffer (640x480).  At 320x200 it sits in the
 * middle; the VNC client scales it up for display. */
#define FB_XOFF  ((FB_WIDTH  - DOOMGENERIC_RESX) / 2)
#define FB_YOFF  ((FB_HEIGHT - DOOMGENERIC_RESY) / 2)

BUILD_ASSERT(DOOMGENERIC_RESX <= FB_WIDTH, "Doom width must fit framebuffer");
BUILD_ASSERT(DOOMGENERIC_RESY <= FB_HEIGHT, "Doom height must fit framebuffer");

#ifdef CONFIG_DOOM_PROFILE
/* Wall-clock ms timing (ms resolution is enough; k_cycle_get_32/64 now read
 * the free-running PERF_CYCLES counter and work too).  tick =
 * whole doomgeneric_Tick (includes any preemption by the VNC send thread);
 * convert = the ARGB->RGB565 loop; draws counts actual DG_DrawFrame calls so we
 * can see how many ticks actually paint. */
#define PROF_WINDOW   20
static uint64_t prof_tick_ms;
static uint64_t prof_conv_ms;
static uint32_t prof_ticks;
static uint32_t prof_draws;
static int64_t  prof_t0_ms;
#endif

/* ── DG platform hooks ──────────────────────────────────────────────────── */

void DG_Init(void)
{
}

void DG_DrawFrame(void)
{
	const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;

	fb_lock();
	uint16_t *dst = fb_pixels();
#ifdef CONFIG_DOOM_PROFILE
	int64_t c0 = k_uptime_get();
#endif

	for (int y = 0; y < DOOMGENERIC_RESY; y++) {
		uint16_t *drow = dst + (size_t)(y + FB_YOFF) * FB_WIDTH + FB_XOFF;
		const uint32_t *srow = src + (size_t)y * DOOMGENERIC_RESX;

		for (int x = 0; x < DOOMGENERIC_RESX; x++) {
			uint32_t p = srow[x];   /* 0x00RRGGBB */

			drow[x] = fb_rgb((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
		}
	}
#ifdef CONFIG_DOOM_PROFILE
	prof_conv_ms += k_uptime_get() - c0;
	prof_draws++;
#endif
	fb_unlock();

#ifdef CONFIG_KLAUSSCPU_AMP_VNC
	/* AMP: the frame is served by core 2 — publish it (cache FLUSH + seq++). */
	amp_post_frame(FB_XOFF, FB_YOFF, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
#else
	fb_mark_dirty(FB_XOFF, FB_YOFF, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
#endif
}

void DG_SleepMs(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

uint32_t DG_GetTicksMs(void)
{
	return (uint32_t)k_uptime_get();
}

/* ── VNC keyboard input → Doom ───────────────────────────────────────────────
 * The VNC server calls on_key() from its connection thread for every RFB
 * KeyEvent (an X11 keysym + press/release).  We map the keysym to a Doom key
 * code and push (pressed<<8 | key) into a small ring that the Doom thread drains
 * in DG_GetKey().  This mirrors doomgeneric's own X11 backend
 * (doomgeneric_xlib.c), which feeds the same shared i_input.c, so the key
 * handling is identical to a normal Doom build.
 *
 * The ring is single-producer (VNC thread) / single-consumer (Doom thread).
 * Both indices are volatile and word-sized, and the queue slot is written before
 * its index is advanced — volatile guarantees that ordering — so no lock is
 * needed on this single core (same approach as the LVGL app's volatile input
 * globals). */
#define KEYQUEUE_SIZE 16
static volatile unsigned short key_queue[KEYQUEUE_SIZE];
static volatile unsigned int   key_wr;   /* advanced by the VNC thread  */
static volatile unsigned int   key_rd;   /* advanced by the Doom thread */

#ifdef CONFIG_KLAUSSCPU_VNC_SERVER
/* X11 keysyms — Zephyr's minimal libc has no <X11/keysymdef.h>. */
#define XK_BackSpace  0xff08u
#define XK_Return     0xff0du
#define XK_Escape     0xff1bu
#define XK_Left       0xff51u
#define XK_Up         0xff52u
#define XK_Right      0xff53u
#define XK_Down       0xff54u
#define XK_Shift_L    0xffe1u
#define XK_Shift_R    0xffe2u
#define XK_Control_L  0xffe3u
#define XK_Control_R  0xffe4u
#define XK_space      0x0020u

/* keysym → Doom key code (doomkeys.h).  Same mapping as doomgeneric_xlib.c,
 * except unmapped keysyms >= 0x80 return 0 (ignored) rather than being passed
 * through tolower(): the Doom control codes live at 0xa0-0xaf, so a raw high
 * keysym could otherwise alias KEY_FIRE/arrows and inject phantom input. */
static unsigned char keysym_to_doom(uint32_t k)
{
	switch (k) {
	case XK_Return:    return KEY_ENTER;
	case XK_Escape:    return KEY_ESCAPE;
	case XK_Left:      return KEY_LEFTARROW;
	case XK_Right:     return KEY_RIGHTARROW;
	case XK_Up:        return KEY_UPARROW;
	case XK_Down:      return KEY_DOWNARROW;
	case XK_Control_L:
	case XK_Control_R: return KEY_FIRE;      /* Ctrl  = fire        */
	case XK_space:     return KEY_USE;       /* Space = use / open  */
	case XK_Shift_L:
	case XK_Shift_R:   return KEY_RSHIFT;    /* Shift = run         */
	case XK_BackSpace: return KEY_BACKSPACE;
	default:
		if (k >= 'A' && k <= 'Z') {
			return (unsigned char)(k + 32);  /* tolower: menu y/n etc. */
		}
		if (k < 0x80u) {
			return (unsigned char)k;         /* printable ASCII incl. 1-7 */
		}
		return 0;                                /* unmapped keysym → ignore */
	}
}

/* RFB KeyEvent handler — runs on the VNC connection thread; keep it short. */
static void on_key(bool pressed, uint32_t keysym)
{
	unsigned char k = keysym_to_doom(keysym);

	if (k == 0) {
		return;   /* a key Doom doesn't use */
	}

	unsigned int wr = key_wr;

	key_queue[wr] = (unsigned short)(((unsigned int)pressed << 8) | k);
	key_wr = (wr + 1u) % KEYQUEUE_SIZE;
}
#endif /* CONFIG_KLAUSSCPU_VNC_SERVER — AMP builds: no key input path yet */

int DG_GetKey(int *pressed, unsigned char *key)
{
	if (key_rd == key_wr) {
		return 0;   /* queue empty */
	}

	unsigned short kd = key_queue[key_rd];

	key_rd = (key_rd + 1u) % KEYQUEUE_SIZE;
	*pressed = kd >> 8;
	*key = (unsigned char)(kd & 0xFFu);
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	ARG_UNUSED(title);
}

/* ── SD mount + DHCP (mirrors ssh_shell) ────────────────────────────────── */

static FATFS fat_fs;
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

static int mount_sd(void)
{
	int rc = disk_access_init("SD");

	if (rc != 0) {
		LOG_ERR("SD init failed: %d", rc);
		return rc;
	}
	rc = fs_mount(&fatfs_mnt);
	if (rc != 0) {
		LOG_ERR("FatFS mount failed: %d", rc);
		return rc;
	}
	LOG_INF("SD card mounted at /SD:/");
	return 0;
}

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
	net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
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

#endif /* CONFIG_NETWORKING */

/* ── Doom game-loop thread ──────────────────────────────────────────────── */

#define DOOM_STACK_SIZE 131072
/* Lower priority than the VNC server (7) and net threads: Doom's tick loop
 * never blocks when it can't keep up 35 fps, so at equal priority it starves
 * the I/O threads (VNC connect hangs, no frames sent).  Run it in the
 * background so networking always preempts it. */
#define DOOM_PRIO       12
static K_THREAD_STACK_DEFINE(doom_stack, DOOM_STACK_SIZE);
static struct k_thread doom_thread;

static void doom_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	char *argv[] = { "doom", "-iwad", "/SD:/doom1.wad" };

	doomgeneric_Create(ARRAY_SIZE(argv), argv);

#ifdef CONFIG_DOOM_PROFILE
	prof_t0_ms = k_uptime_get();
#endif

	for (;;) {
#ifdef CONFIG_DOOM_PROFILE
		int64_t t0 = k_uptime_get();
#endif
		doomgeneric_Tick();
#ifdef CONFIG_DOOM_PROFILE
		prof_tick_ms += k_uptime_get() - t0;

		if (++prof_ticks >= PROF_WINDOW) {
			int64_t now = k_uptime_get();
			uint32_t wall = (uint32_t)(now - prof_t0_ms);

			printk("doom: drawfps=%u tick=%ums convert=%ums "
			       "(draws=%u/%u ticks in %ums)\n",
			       wall ? (prof_draws * 1000U / wall) : 0U,
			       (uint32_t)(prof_tick_ms / prof_ticks),
			       prof_draws ? (uint32_t)(prof_conv_ms / prof_draws) : 0U,
			       prof_draws, prof_ticks, wall);

			prof_tick_ms = 0;
			prof_conv_ms = 0;
			prof_ticks = 0;
			prof_draws = 0;
			prof_t0_ms = now;
		}
#endif
	}
}

/* Route libc stdout (Doom's printf/puts/putchar) to printk → UART.  The board
 * has no generic console driver (CONSOLE_HAS_DRIVER=n), so CONFIG_STDOUT_CONSOLE
 * can't be used; install the minimal-libc hook directly instead. */
extern void __stdout_hook_install(int (*hook)(int));
static int doom_stdout(int c)
{
	printk("%c", (char)c);
	return c;
}

int main(void)
{
	__stdout_hook_install(doom_stdout);
	printk("\nKlaussCPU DOOM\n");

	if (mount_sd() != 0) {
		LOG_ERR("no SD card — cannot load WAD");
	}
#ifdef CONFIG_KLAUSSCPU_AMP_VNC
	/* P4b: core 2 owns the network and serves VNC; this core only renders. */
	if (amp_host_init() != 0) {
		LOG_ERR("AMP core 2 failed to start — no display");
	}
#else
	if (wait_for_dhcp() != 0) {
		LOG_WRN("no network — VNC will be unreachable");
	}

	vnc_server_start();
	vnc_register_input(on_key, NULL);   /* keyboard -> Doom; no pointer */
	LOG_INF("VNC server ready on port 5900 — connect to play");
#endif

	k_thread_create(&doom_thread, doom_stack, DOOM_STACK_SIZE,
			doom_entry, NULL, NULL, NULL, DOOM_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&doom_thread, "doom");

	/* Do NOT return from main on this arch: the KlaussCPU thread-exit path
	 * jumps to a null return address (PC=0 crash).  Idle here forever, like
	 * the ssh_shell app — the Doom thread does the work. */
	k_sleep(K_FOREVER);
	return 0;
}
