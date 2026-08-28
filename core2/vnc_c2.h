/* vnc_c2.h — RFB (VNC) server for AMP core 2 on lwIP's raw API. */
#ifndef VNC_C2_H
#define VNC_C2_H
void vnc_c2_init(void);     /* listen on :5900 (after lwip_init + netif up) */
void vnc_c2_poll(void);     /* call from the main loop: serves posted frames */
/* Heartbeat diagnostics: connection state (0=none, 1=handshaking, 2=up) and
 * whether an update is mid-pump. */
int  vnc_c2_status(void);
#endif
