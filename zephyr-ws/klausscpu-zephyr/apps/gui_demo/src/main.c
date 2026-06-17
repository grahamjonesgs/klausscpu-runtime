/*
 * main.c — minimal hand-rolled GUI over VNC for KlaussCPU.
 *
 * Draws a little control panel into the in-RAM framebuffer and serves it over
 * VNC.  Clicking the buttons (VNC PointerEvent) changes a counter and a colour
 * swatch; an uptime field ticks once a second.  Only the touched regions are
 * redrawn + marked dirty, so updates are tiny and snappy — the workload VNC is
 * actually good at (cf. vnc/PERFORMANCE.md).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <stdio.h>

#include "framebuffer.h"
#include "vnc_server.h"
#include "gui.h"

LOG_MODULE_REGISTER(gui, LOG_LEVEL_INF);

/* Compile-time RGB565 (fb_rgb is not a constant expression). */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG     RGB(18, 22, 38)
#define C_TITLE  RGB(40, 90, 170)
#define C_PANEL  RGB(28, 32, 52)
#define C_TEXT   RGB(230, 232, 245)
#define C_DIM    RGB(150, 155, 180)
#define C_ACCENT RGB(255, 200, 60)

/* ── widget state ───────────────────────────────────────────────────────── */

static int counter;
static int color_idx;
static const uint16_t swatches[] = {
	RGB(220, 60, 60), RGB(60, 200, 90), RGB(70, 120, 240),
	RGB(240, 200, 50), RGB(200, 90, 220), RGB(40, 210, 210),
};
#define NSWATCH ((int)(sizeof(swatches) / sizeof(swatches[0])))

/* Layout constants. */
#define COUNT_X   40
#define COUNT_Y   120
#define SWATCH_X  40
#define SWATCH_Y  330
#define SWATCH_W  220
#define SWATCH_H  90
#define UP_X      320
#define UP_Y      360

static void draw_counter(void)
{
	char buf[16];

	(void)snprintf(buf, sizeof(buf), "%d", counter);
	gui_fill(COUNT_X, COUNT_Y, 260, 8 * 7, C_PANEL);   /* erase old value */
	gui_text(COUNT_X, COUNT_Y, buf, C_ACCENT, 7);
}

static void draw_swatch(void)
{
	gui_fill(SWATCH_X, SWATCH_Y, SWATCH_W, SWATCH_H, swatches[color_idx]);
	gui_border(SWATCH_X, SWATCH_Y, SWATCH_W, SWATCH_H, 3, C_TEXT);
}

static void draw_uptime(void)
{
	char buf[24];
	uint32_t s = (uint32_t)(k_uptime_get() / 1000);

	(void)snprintf(buf, sizeof(buf), "UPTIME: %u S", s);
	gui_fill(UP_X, UP_Y, 280, 8 * 3, C_PANEL);
	gui_text(UP_X, UP_Y, buf, C_DIM, 3);
}

/* ── button actions ─────────────────────────────────────────────────────── */

static void act_inc(void)   { counter++;  draw_counter(); }
static void act_dec(void)   { counter--;  draw_counter(); }
static void act_reset(void) { counter = 0; draw_counter(); }
static void act_color(void) { color_idx = (color_idx + 1) % NSWATCH; draw_swatch(); }

static const struct gui_button buttons[] = {
	{ .x =  40, .y = 240, .w =  90, .h = 64, .label = "-",     .action = act_dec   },
	{ .x = 150, .y = 240, .w =  90, .h = 64, .label = "+",     .action = act_inc   },
	{ .x = 270, .y = 240, .w = 150, .h = 64, .label = "RESET", .action = act_reset },
	{ .x = 440, .y = 240, .w = 160, .h = 64, .label = "COLOR", .action = act_color },
};
#define NBTN ((int)(sizeof(buttons) / sizeof(buttons[0])))

static void build_gui(void)
{
	gui_fill(0, 0, FB_WIDTH, FB_HEIGHT, C_BG);
	gui_fill(0, 0, FB_WIDTH, 46, C_TITLE);
	gui_text(16, 11, "KLAUSSCPU VNC DEMO", C_TEXT, 3);

	gui_text(COUNT_X, 90, "COUNT", C_DIM, 2);
	draw_counter();

	for (int i = 0; i < NBTN; i++) {
		gui_button_draw(&buttons[i], false);
	}

	gui_text(SWATCH_X, SWATCH_Y - 24, "COLOR SWATCH", C_DIM, 2);
	draw_swatch();
	draw_uptime();

	gui_text(40, 450, "CLICK THE BUTTONS WITH YOUR MOUSE", C_DIM, 1);
}

/* ── VNC pointer input (runs on the VNC thread) ─────────────────────────── */

static int  pressed_idx = -1;
static bool prev_down;

static void on_pointer(uint16_t x, uint16_t y, uint8_t buttons_mask)
{
	bool down = buttons_mask & 1;   /* left button */

	if (down && !prev_down) {
		for (int i = 0; i < NBTN; i++) {
			if (gui_button_hit(&buttons[i], x, y)) {
				pressed_idx = i;
				gui_button_draw(&buttons[i], true);
				if (buttons[i].action) {
					buttons[i].action();
				}
				break;
			}
		}
	} else if (!down && prev_down && pressed_idx >= 0) {
		gui_button_draw(&buttons[pressed_idx], false);
		pressed_idx = -1;
	}
	prev_down = down;
}

/* ── DHCP ───────────────────────────────────────────────────────────────── */

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

static void wait_for_dhcp(void)
{
	k_sem_init(&dhcp_sem, 0, 1);
	net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);

	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("no network interface");
		return;
	}
	net_dhcpv4_start(iface);
	if (k_sem_take(&dhcp_sem, K_SECONDS(10)) != 0) {
		LOG_WRN("DHCP timeout — VNC will be unreachable");
	}
}

/* ── uptime ticker ──────────────────────────────────────────────────────── */

#define TICK_STACK 2048
static K_THREAD_STACK_DEFINE(tick_stack, TICK_STACK);
static struct k_thread tick_thread;

static void tick_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		draw_uptime();
		k_sleep(K_SECONDS(1));
	}
}

int main(void)
{
	printk("\nKlaussCPU VNC GUI demo\n");

	wait_for_dhcp();

	build_gui();
	vnc_register_input(NULL, on_pointer);
	vnc_server_start();
	LOG_INF("VNC GUI ready on port 5900");

	k_thread_create(&tick_thread, tick_stack, TICK_STACK,
			tick_entry, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
	k_thread_name_set(&tick_thread, "uptime");

	/* Don't return from main on this arch (jumps to a null return address). */
	k_sleep(K_FOREVER);
	return 0;
}
