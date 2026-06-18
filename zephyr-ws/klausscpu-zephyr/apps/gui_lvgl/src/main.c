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

/* ── render micro-benchmark (CONFIG_GUI_LVGL_BENCHMARK) ─────────────────── */
#ifdef CONFIG_GUI_LVGL_BENCHMARK

static void scene_fill(lv_obj_t *scr)
{
	for (int i = 0; i < 60; i++) {
		lv_obj_t *o = lv_obj_create(scr);

		lv_obj_remove_style_all(o);
		lv_obj_set_size(o, 90, 70);
		lv_obj_set_pos(o, (i * 53) % 550, (i * 37) % 410);
		lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(o, lv_color_hex(0x204080 + i * 0x070707), 0);
	}
}

static void scene_blend(lv_obj_t *scr)   /* 50% opacity -> read-modify-write blend */
{
	for (int i = 0; i < 60; i++) {
		lv_obj_t *o = lv_obj_create(scr);

		lv_obj_remove_style_all(o);
		lv_obj_set_size(o, 110, 90);
		lv_obj_set_pos(o, (i * 47) % 530, (i * 31) % 390);
		lv_obj_set_style_bg_opa(o, LV_OPA_50, 0);
		lv_obj_set_style_bg_color(o, lv_color_hex(0xe04020), 0);
	}
}

static void scene_rounded(lv_obj_t *scr) /* anti-aliased edges */
{
	for (int i = 0; i < 40; i++) {
		lv_obj_t *o = lv_obj_create(scr);

		lv_obj_remove_style_all(o);
		lv_obj_set_size(o, 90, 70);
		lv_obj_set_pos(o, (i * 61) % 550, (i * 41) % 410);
		lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(o, lv_color_hex(0x30a060), 0);
		lv_obj_set_style_radius(o, 18, 0);
	}
}

static void scene_shadow(lv_obj_t *scr)  /* blurred shadow = heavy blend */
{
	for (int i = 0; i < 16; i++) {
		lv_obj_t *o = lv_obj_create(scr);

		lv_obj_set_size(o, 100, 80);
		lv_obj_set_pos(o, (i * 97) % 520, (i * 83) % 380);
		lv_obj_set_style_shadow_width(o, 30, 0);
		lv_obj_set_style_shadow_color(o, lv_color_black(), 0);
	}
}

static void scene_text(lv_obj_t *scr)
{
	for (int i = 0; i < 24; i++) {
		lv_obj_t *l = lv_label_create(scr);

		lv_label_set_text(l, "KlaussCPU LVGL render benchmark 0123456789");
		lv_obj_set_pos(l, 8, i * 19);
	}
}

static void scene_gradient(lv_obj_t *scr)
{
	lv_obj_t *o = lv_obj_create(scr);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, 600, 440);
	lv_obj_set_pos(o, 20, 20);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(0x2040c0), 0);
	lv_obj_set_style_bg_grad_color(o, lv_color_hex(0xc04020), 0);
	lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
}

/* CPU/cache performance counters (board_io.h register map: cache block 0xF005,
 * pipeline block 0xF00D; cumulative 64-bit, non-destructive). */
#define REG64(a) (*(volatile uint64_t *)(unsigned long)(a))
struct cstat {
	uint64_t cyc, instr, rh, rm, wh, wm, wb, stall;
};

static void cstat_read(struct cstat *s)
{
	s->cyc   = REG64(0xF00D0008u);
	s->instr = REG64(0xF00D0010u);
	s->rh    = REG64(0xF0050040u);
	s->rm    = REG64(0xF0050048u);
	s->wh    = REG64(0xF0050050u);
	s->wm    = REG64(0xF0050058u);
	s->wb    = REG64(0xF0050060u);
	s->stall = REG64(0xF0050068u);
}

/* "<label> NN.NN%" — all-64-bit args are cbprintf-safe on this core. */
static void print_pct(const char *label, uint64_t num, uint64_t den)
{
	uint64_t p = den ? (num * 10000ULL) / den : 0;

	printk("  %s %llu.%02llu%%\n", label,
	       (unsigned long long)(p / 100), (unsigned long long)(p % 100));
}

static void bench_scene(const char *name, void (*build)(lv_obj_t *))
{
	lv_obj_t *scr = lv_scr_act();

	lv_obj_clean(scr);
	build(scr);
	lv_refr_now(NULL);                       /* warm draw, not timed */

	const int n = 20;
	struct cstat c0, c1;

	(void)vncd_copy_ms_reset();              /* clear flush-copy counter */
	cstat_read(&c0);
	int64_t t0 = k_uptime_get();

	for (int i = 0; i < n; i++) {
		lv_obj_invalidate(scr);          /* force full-screen redraw */
		lv_refr_now(NULL);               /* synchronous render + flush */
	}
	uint32_t total = (uint32_t)((k_uptime_get() - t0) / n);

	cstat_read(&c1);
	uint32_t copy = vncd_copy_ms_reset() / n;
	uint32_t render = (total > copy) ? total - copy : 0;

	uint64_t rh = c1.rh - c0.rh, rm = c1.rm - c0.rm;
	uint64_t wh = c1.wh - c0.wh, wm = c1.wm - c0.wm;
	uint64_t wb = c1.wb - c0.wb, st = c1.stall - c0.stall, cyc = c1.cyc - c0.cyc;
	uint64_t ins = c1.instr - c0.instr;
	uint64_t cpi_m = ins ? (cyc * 1000ULL) / ins : 0;   /* milli-CPI */

	printk("LVGL bench %-9s: total=%u render=%u copy=%u ms/frame\n",
	       name, total, render, copy);
	print_pct("rd hit:", rh, rh + rm);
	print_pct("wr hit:", wh, wh + wm);
	print_pct("stall%:", st, cyc);
	printk("  CPI: %llu.%03llu  instr/fr=%llu\n",
	       (unsigned long long)(cpi_m / 1000), (unsigned long long)(cpi_m % 1000),
	       (unsigned long long)(ins / n));
	printk("  rm/fr=%llu wm/fr=%llu wb/fr=%llu\n",
	       (unsigned long long)(rm / n), (unsigned long long)(wm / n),
	       (unsigned long long)(wb / n));
}

static void run_benchmark(void)
{
	printk("=== LVGL render benchmark (640x480, render + flush) ===\n");
	bench_scene("fill", scene_fill);
	bench_scene("blend50", scene_blend);
	bench_scene("rounded", scene_rounded);
	bench_scene("shadow", scene_shadow);
	bench_scene("text", scene_text);
	bench_scene("gradient", scene_gradient);
	printk("=== benchmark done ===\n");
	lv_obj_clean(lv_scr_act());
}
#endif /* CONFIG_GUI_LVGL_BENCHMARK */

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

#ifdef CONFIG_GUI_LVGL_BENCHMARK
	run_benchmark();   /* renders into the framebuffer; no network needed */
#endif

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
