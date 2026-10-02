/* amp_host.h — core-1 (Zephyr) side of the AMP VNC path. */
#ifndef KLAUSSCPU_AMP_HOST_H_
#define KLAUSSCPU_AMP_HOST_H_
/* Boot core 2 with the embedded lwIP+VNC image, publish the framebuffer
 * descriptor, hand core 2 the LiteEth, start it, and start the log-drain
 * thread (core 2's printf -> this console).  Call after the framebuffer is
 * ready and BEFORE anything else touches ethernet (there is nothing else:
 * networking is off in AMP builds). */
int  amp_host_init(void);
/* Producer side of the contract: call after drawing [x,y,w,h] into the
 * framebuffer (outside fb_lock): whole-cache FLUSH + descriptor seq++. */
void amp_post_frame(int x, int y, int w, int h);

/* Optional 8-bit indexed source (same geometry as the framebuffer): lets core
 * 2 serve colour-map VNC clients straight from it.  amp_set_palette() copies
 * 256 x 0x00RRGGBB and bumps pal_seq; both are published by the next
 * amp_post_frame().  amp_want_rgb565() reports whether a connected client
 * still needs the RGB565 framebuffer filled. */
#include <stdbool.h>
#include <stdint.h>
int  amp_set_indexed(const void *idx, uint32_t stride);
void amp_set_palette(const uint32_t rgb[256]);
bool amp_want_rgb565(void);
#endif
