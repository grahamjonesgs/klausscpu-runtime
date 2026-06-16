/*
 * doomgeneric_zephyr.c — KlaussCPU/Zephyr platform backend for doomgeneric.
 *
 * Implements the six DG_* hooks doomgeneric needs and provides the Zephyr app
 * main(): mount the SD card (for the WAD), get an IP (DHCP), start the VNC
 * server (the display), then run the Doom game loop on a dedicated thread.
 *
 * Doom renders into DG_ScreenBuffer (640x400, XRGB8888); DG_DrawFrame converts
 * that to the VNC module's RGB565 framebuffer, centred vertically in 640x480.
 * Input (DG_GetKey) is stubbed for now — Doom's attract mode auto-plays demos
 * with no input, which is the first bring-up target.  VNC KeyEvent -> DG_GetKey
 * wiring is the next step.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>

#include "framebuffer.h"
#include "vnc_server.h"
#include "doomgeneric.h"

LOG_MODULE_REGISTER(doom, LOG_LEVEL_INF);

/* Centre Doom's 400-row image in the 480-row framebuffer. */
#define FB_YOFF  ((FB_HEIGHT - DOOMGENERIC_RESY) / 2)

BUILD_ASSERT(DOOMGENERIC_RESX == FB_WIDTH, "Doom width must match framebuffer");
BUILD_ASSERT(DOOMGENERIC_RESY <= FB_HEIGHT, "Doom height must fit framebuffer");

/* ── DG platform hooks ──────────────────────────────────────────────────── */

void DG_Init(void)
{
}

void DG_DrawFrame(void)
{
	const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;

	fb_lock();
	uint16_t *dst = fb_pixels();

	for (int y = 0; y < DOOMGENERIC_RESY; y++) {
		uint16_t *drow = dst + (size_t)(y + FB_YOFF) * FB_WIDTH;
		const uint32_t *srow = src + (size_t)y * DOOMGENERIC_RESX;

		for (int x = 0; x < DOOMGENERIC_RESX; x++) {
			uint32_t p = srow[x];   /* 0x00RRGGBB */

			drow[x] = fb_rgb((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
		}
	}
	fb_unlock();

	fb_mark_dirty(0, FB_YOFF, DOOMGENERIC_RESX, DOOMGENERIC_RESY);

	/* Heartbeat so we can confirm rendering is progressing (temporary). */
	static unsigned frames;

	if ((++frames % 70) == 0) {
		printk("doom: %u frames\n", frames);
	}
}

void DG_SleepMs(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

uint32_t DG_GetTicksMs(void)
{
	return (uint32_t)k_uptime_get();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	ARG_UNUSED(pressed);
	ARG_UNUSED(key);
	return 0;   /* no input yet — attract mode */
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

	for (;;) {
		doomgeneric_Tick();
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
	if (wait_for_dhcp() != 0) {
		LOG_WRN("no network — VNC will be unreachable");
	}

	vnc_server_start();
	LOG_INF("VNC server ready on port 5900 — connect to play");

	k_thread_create(&doom_thread, doom_stack, DOOM_STACK_SIZE,
			doom_entry, NULL, NULL, NULL, DOOM_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&doom_thread, "doom");

	/* Do NOT return from main on this arch: the KlaussCPU thread-exit path
	 * jumps to a null return address (PC=0 crash).  Idle here forever, like
	 * the ssh_shell app — the Doom thread does the work. */
	k_sleep(K_FOREVER);
	return 0;
}
