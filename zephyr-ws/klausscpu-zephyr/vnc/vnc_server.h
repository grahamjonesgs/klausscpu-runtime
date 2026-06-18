/*
 * vnc_server.h — minimal RFB (VNC) server exposing the in-RAM framebuffer.
 */

#ifndef KLAUSSCPU_VNC_SERVER_H_
#define KLAUSSCPU_VNC_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

/* Draw the test pattern and start the VNC listener thread (RFB on :5900,
 * display :0).  Call once after DHCP. */
void vnc_server_start(void);

/* Input callbacks, invoked on the VNC connection thread when the client sends a
 * KeyEvent / PointerEvent.  keysym is an X11 keysym; buttons is the RFB pointer
 * button mask (bit0 = left).  Register NULL to ignore.  Handlers may draw into
 * the framebuffer (it's mutex-protected); keep them short. */
typedef void (*vnc_key_fn)(bool pressed, uint32_t keysym);
typedef void (*vnc_pointer_fn)(uint16_t x, uint16_t y, uint8_t buttons);
void vnc_register_input(vnc_key_fn key, vnc_pointer_fn pointer);

/* Accumulated CPU cycles spent in the display driver's flush copy (draw-buffer
 * -> framebuffer, via the blitter or a memcpy fallback) since the last call;
 * resets the counter.  100 MHz clock, so cycles/100 = microseconds.  Provided
 * by display_vnc.c (CONFIG_KLAUSSCPU_VNC_DISPLAY).  Splits render vs copy in
 * benchmarks. */
uint64_t vncd_copy_cyc_reset(void);

/* Of the flush-copy cycles above, the subset the blitter engine itself reported
 * busy (sum of BLIT_CYCLES); resets the counter.  copy_cyc - blit_cyc is the
 * whole-cache FLUSH/INVALIDATE overhead.  0 on the memcpy path. */
uint64_t vncd_blit_cyc_reset(void);

/* True when the flush copy is hardware-accelerated by the 2D DMA blitter (built
 * with CONFIG_KLAUSSCPU_VNC_BLITTER and probed present in the bitstream). */
bool vncd_blit_active(void);

#endif /* KLAUSSCPU_VNC_SERVER_H_ */
