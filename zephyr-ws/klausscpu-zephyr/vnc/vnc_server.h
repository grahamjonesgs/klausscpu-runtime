/*
 * vnc_server.h — minimal RFB (VNC) server exposing the in-RAM framebuffer.
 */

#ifndef KLAUSSCPU_VNC_SERVER_H_
#define KLAUSSCPU_VNC_SERVER_H_

/* Draw the test pattern and start the VNC listener thread (RFB on :5900,
 * display :0).  Call once after DHCP. */
void vnc_server_start(void);

#endif /* KLAUSSCPU_VNC_SERVER_H_ */
