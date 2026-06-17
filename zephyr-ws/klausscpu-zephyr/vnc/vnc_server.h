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

#endif /* KLAUSSCPU_VNC_SERVER_H_ */
