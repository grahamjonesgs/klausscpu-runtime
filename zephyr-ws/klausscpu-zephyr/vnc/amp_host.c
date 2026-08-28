/*
 * amp_host.c — core 1's side of "VNC served by AMP core 2" (Zephyr).
 *
 * Same contract as baremetal/programs/core1_amp_vnc.c, as a Zephyr module:
 *   amp_host_init():  copy core2_image[] to the DDR text window, publish the
 *                     amp_fb_desc_t (framebuffer address/geometry), FLUSH,
 *                     C2_ETH_OWNER=1, START_PC, RUN=1; spawn the log drain.
 *   amp_post_frame(): FLUSH (pixels + descriptor in one walk) after seq++.
 * Cache rule: this core writes producer fields only (line 0 of the
 * descriptor); core 2's fields (line 1) are read after an INVALIDATE.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "framebuffer.h"
#include "amp_host.h"
#include "mmio.h"            /* REG_C2_*, REG_CACHE_*, C2_TEXT_ENTRY (repo root) */
#include "amp/amp_proto.h"
#include "core2_image.h"     /* baremetal/core2_image.h: core2_image[], _len */

LOG_MODULE_REGISTER(amp_host, LOG_LEVEL_INF);

static void cache_flush(void)
{
	REG_CACHE_CTRL = CACHE_CTRL_FLUSH;
	while (REG_CACHE_STATUS & 1u) {}
}

/* Core 2's console -> printk, at low priority. */
#define DRAIN_STACK 4096   /* printk/cbprintf on this 64-bit arch needs headroom */
static K_THREAD_STACK_DEFINE(drain_stack, DRAIN_STACK);
static struct k_thread drain_thread;

static void drain_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	char line[96];
	int n = 0;

	for (;;) {
		uint64_t v = REG_C2_LOG;

		if (v & 0x100u) {
			char ch = (char)(v & 0xFFu);

			REG_C2_LOG = 0;
			if (ch == '\n' || n == (int)sizeof(line) - 1) {
				line[n] = '\0';
				printk("[core2] %s\n", line);
				n = 0;
			} else if (ch != '\r') {
				line[n++] = ch;
			}
		} else {
			k_msleep(5);
		}
	}
}

int amp_host_init(void)
{
	amp_fb_desc_t *d = AMP_FB_DESC;
	uint32_t fb_base = (uint32_t)(uintptr_t)fb_pixels();

	if (fb_base < C2_LRAM_SIZE) {
		LOG_ERR("framebuffer at 0x%08x is inside core 2's BRAM shadow", fb_base);
		return -EINVAL;
	}
	if (core2_image_len > C2_TEXT_SIZE - 0x20u) {
		LOG_ERR("core-2 image too large (%u)", (unsigned)core2_image_len);
		return -ENOMEM;
	}

	REG_C2_CTRL = 0;
	/* Drain any stale core-2 console backlog from a previous program — the
	 * 512 B log FIFO survives core-1 reloads and would otherwise replay old
	 * lines (including an old "listening" marker) into this session. */
	for (int i = 0; i < 600; i++) {
		if (!(REG_C2_LOG & 0x100u)) break;
		REG_C2_LOG = 0;
	}
	memcpy((void *)(uintptr_t)C2_TEXT_ENTRY, core2_image, core2_image_len);

	memset(d, 0, sizeof(*d));
	d->fb_base = fb_base;
	d->stride  = FB_WIDTH * sizeof(uint16_t);
	d->width   = FB_WIDTH;
	d->height  = FB_HEIGHT;
	d->dx = 0; d->dy = 0; d->dw = FB_WIDTH; d->dh = FB_HEIGHT;
	d->seq   = 1;
	d->magic = AMP_FB_MAGIC;
	cache_flush();

	REG_C2_ETH_OWNER = 1;
	REG_C2_START_PC  = C2_TEXT_ENTRY;
	REG_C2_CTRL      = 1;
	LOG_INF("core 2 started: image %u B, fb %ux%u @0x%08x, LiteEth handed over",
		(unsigned)core2_image_len, FB_WIDTH, FB_HEIGHT, fb_base);

	/* Priority ABOVE the doom thread (12): doom's tick loop never blocks, so
	 * anything below it starves — the drain was lagging the console by tens
	 * of seconds.  The drain sleeps 5 ms per empty poll, so at prio 11 it
	 * costs doom nothing measurable. */
	(void)k_thread_create(&drain_thread, drain_stack, K_THREAD_STACK_SIZEOF(drain_stack),
			      drain_main, NULL, NULL, NULL, 11, 0, K_NO_WAIT);
	(void)k_thread_name_set(&drain_thread, "c2log");
	return 0;
}

void amp_post_frame(int x, int y, int w, int h)
{
	amp_fb_desc_t *d = AMP_FB_DESC;

	d->dx = (uint16_t)x; d->dy = (uint16_t)y;
	d->dw = (uint16_t)w; d->dh = (uint16_t)h;
	d->seq++;
	cache_flush();
}
