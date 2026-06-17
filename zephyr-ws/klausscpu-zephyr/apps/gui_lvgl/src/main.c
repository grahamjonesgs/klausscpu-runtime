/*
 * main.c — LVGL UI rendered over VNC on KlaussCPU.
 *
 * LVGL draws into the framebuffer via the "klausscpu,vnc-display" Zephyr
 * display driver (chosen zephyr,display); the VNC server streams it.  A custom
 * LVGL pointer input device is fed by VNC PointerEvents so the widgets are
 * clickable.  LVGL's Zephyr glue auto-inits (SYS_INIT) before main().
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>

#include <lvgl.h>

#include "vnc_server.h"

LOG_MODULE_REGISTER(gui_lvgl, LOG_LEVEL_INF);

/* ── VNC pointer state (written by the VNC thread, read by the LVGL indev) ── */

static volatile int32_t ptr_x;
static volatile int32_t ptr_y;
static volatile bool    ptr_pressed;

static void on_pointer(uint16_t x, uint16_t y, uint8_t buttons)
{
	ptr_x = x;
	ptr_y = y;
	ptr_pressed = (buttons & 1) != 0;   /* left button */
}

static void lvgl_ptr_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
	ARG_UNUSED(drv);
	data->point.x = ptr_x;
	data->point.y = ptr_y;
	data->state = ptr_pressed ? LV_INDEV_STATE_PRESSED
				  : LV_INDEV_STATE_RELEASED;
}

/* ── widgets ────────────────────────────────────────────────────────────── */

static lv_obj_t *count_label;
static lv_obj_t *slider_label;
static int counter;

static void update_count(void)
{
	lv_label_set_text_fmt(count_label, "COUNT: %d", counter);
}

static void btn_inc_cb(lv_event_t *e) { ARG_UNUSED(e); counter++; update_count(); }
static void btn_dec_cb(lv_event_t *e) { ARG_UNUSED(e); counter--; update_count(); }

static void slider_cb(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);

	lv_label_set_text_fmt(slider_label, "%d %%", (int)lv_slider_get_value(s));
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
			     lv_coord_t xofs, lv_event_cb_t cb)
{
	lv_obj_t *btn = lv_btn_create(parent);

	lv_obj_set_size(btn, 110, 56);
	lv_obj_align(btn, LV_ALIGN_CENTER, xofs, -40);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl = lv_label_create(btn);

	lv_label_set_text(lbl, text);
	lv_obj_center(lbl);
	return btn;
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_scr_act();

	lv_obj_t *title = lv_label_create(scr);

	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_label_set_text(title, "KlaussCPU  -  LVGL over VNC");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

	count_label = lv_label_create(scr);
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_28, 0);
	lv_obj_align(count_label, LV_ALIGN_TOP_MID, 0, 70);
	update_count();

	make_button(scr, LV_SYMBOL_MINUS, -90, btn_dec_cb);
	make_button(scr, LV_SYMBOL_PLUS, 90, btn_inc_cb);

	lv_obj_t *slider = lv_slider_create(scr);

	lv_obj_set_width(slider, 320);
	lv_obj_align(slider, LV_ALIGN_CENTER, 0, 60);
	lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	slider_label = lv_label_create(scr);
	lv_label_set_text(slider_label, "0 %");
	lv_obj_align(slider_label, LV_ALIGN_CENTER, 0, 90);

	lv_obj_t *sw = lv_switch_create(scr);

	lv_obj_align(sw, LV_ALIGN_CENTER, 0, 140);
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

int main(void)
{
	printk("\nKlaussCPU LVGL over VNC\n");

	wait_for_dhcp();
	vnc_server_start();              /* demo off; LVGL owns the framebuffer */
	vnc_register_input(NULL, on_pointer);

	static lv_indev_drv_t indev_drv;

	lv_indev_drv_init(&indev_drv);
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.read_cb = lvgl_ptr_read;
	lv_indev_drv_register(&indev_drv);

	build_ui();
	LOG_INF("LVGL UI ready on VNC :5900");

	/* LVGL is single-threaded: drive it (and the flush) from here only. */
	for (;;) {
		uint32_t idle = lv_timer_handler();

		if (idle > 50) {
			idle = 50;
		} else if (idle < 5) {
			idle = 5;
		}
		k_sleep(K_MSEC(idle));
	}
	return 0;
}
