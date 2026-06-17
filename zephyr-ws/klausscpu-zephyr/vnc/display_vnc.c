/*
 * display_vnc.c — exposes the in-RAM VNC framebuffer as a Zephyr display.
 *
 * A minimal display driver (compatible "klausscpu,vnc-display") whose write()
 * copies flushed RGB565 rectangles into the VNC framebuffer and marks them
 * dirty.  With it set as chosen zephyr,display, display-based stacks (LVGL)
 * render straight over VNC.  Pair with a matching devicetree node.
 */

#define DT_DRV_COMPAT klausscpu_vnc_display

#include <zephyr/drivers/display.h>
#include <zephyr/device.h>
#include <string.h>
#include <errno.h>

#include "framebuffer.h"

static int vncd_write(const struct device *dev, const uint16_t x,
		      const uint16_t y,
		      const struct display_buffer_descriptor *desc,
		      const void *buf)
{
	ARG_UNUSED(dev);
	const uint16_t *src = buf;

	if ((uint32_t)x + desc->width > FB_WIDTH ||
	    (uint32_t)y + desc->height > FB_HEIGHT) {
		return -EINVAL;
	}

	fb_lock();
	uint16_t *fb = fb_pixels();

	for (uint16_t row = 0; row < desc->height; row++) {
		memcpy(fb + (size_t)(y + row) * FB_WIDTH + x,
		       src + (size_t)row * desc->pitch,
		       (size_t)desc->width * sizeof(uint16_t));
	}
	fb_unlock();

	fb_mark_dirty(x, y, desc->width, desc->height);
	return 0;
}

static void vncd_get_capabilities(const struct device *dev,
				  struct display_capabilities *cap)
{
	ARG_UNUSED(dev);
	memset(cap, 0, sizeof(*cap));
	cap->x_resolution = FB_WIDTH;
	cap->y_resolution = FB_HEIGHT;
	cap->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	cap->current_pixel_format = PIXEL_FORMAT_RGB_565;
}

static int vncd_set_pixel_format(const struct device *dev,
				 const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return (pf == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static int vncd_blanking_off(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int vncd_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int vncd_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static const struct display_driver_api vncd_api = {
	.blanking_on = vncd_blanking_on,
	.blanking_off = vncd_blanking_off,
	.write = vncd_write,
	.get_capabilities = vncd_get_capabilities,
	.set_pixel_format = vncd_set_pixel_format,
};

#define VNCD_DEFINE(n)							\
	DEVICE_DT_INST_DEFINE(n, vncd_init, NULL, NULL, NULL,		\
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,\
			      &vncd_api);

DT_INST_FOREACH_STATUS_OKAY(VNCD_DEFINE)
