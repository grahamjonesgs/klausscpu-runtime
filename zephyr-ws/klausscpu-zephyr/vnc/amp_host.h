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
#endif
