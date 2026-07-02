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

/* ── live hardware telemetry (perf counters: pipeline 0xF00D, cache 0xF005) ─ */

#define PREG64(a) (*(volatile uint64_t *)(unsigned long)(a))
struct perf_s { uint64_t cyc, instr, rh, rm, wh, wm, stall; };

static void perf_sample(struct perf_s *s)
{
	s->cyc   = PREG64(0xF00D0008u);
	s->instr = PREG64(0xF00D0010u);
	s->rh    = PREG64(0xF0050040u);
	s->rm    = PREG64(0xF0050048u);
	s->wh    = PREG64(0xF0050050u);
	s->wm    = PREG64(0xF0050058u);
	s->stall = PREG64(0xF0050068u);
}

/* ── widgets ────────────────────────────────────────────────────────────── */

static lv_obj_t *count_label;
static lv_obj_t *slider_label;
static lv_obj_t *ctl_arc;
static int counter;

static lv_obj_t *clock_label;          /* top-bar uptime */
static lv_obj_t *mon_label;            /* monitor: MIPS / stall */
static lv_obj_t *bar_rd, *bar_wr;      /* monitor: cache hit-rate bars */
static lv_obj_t *act_chart;            /* scrolling CPU-activity chart */
static lv_chart_series_t *act_ser;
static struct perf_s perf_prev;
static int64_t perf_t_prev;

static void update_count(void)
{
	lv_label_set_text_fmt(count_label, "COUNT: %d", counter);
}

static void btn_inc_cb(lv_event_t *e) { ARG_UNUSED(e); counter++; update_count(); }
static void btn_dec_cb(lv_event_t *e) { ARG_UNUSED(e); counter--; update_count(); }

static void slider_cb(lv_event_t *e)
{
	int v = (int)lv_slider_get_value(lv_event_get_target(e));

	lv_label_set_text_fmt(slider_label, "%d %%", v);
	if (ctl_arc) {
		lv_arc_set_value(ctl_arc, v);
	}
}

/* ── draggable windows ──────────────────────────────────────────────────── */

static void win_drag_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_get_act();
	lv_point_t v;

	if (!indev) {
		return;
	}
	lv_indev_get_vect(indev, &v);
	lv_obj_set_pos(win, lv_obj_get_x(win) + v.x, lv_obj_get_y(win) + v.y);
}

static void win_front_cb(lv_event_t *e)
{
	lv_obj_move_foreground(lv_event_get_user_data(e));
}

static lv_obj_t *make_window(const char *title, lv_coord_t x, lv_coord_t y,
			     lv_coord_t w, lv_coord_t h)
{
	lv_obj_t *win = lv_win_create(lv_scr_act(), 26);
	lv_obj_t *hdr;

	lv_obj_set_size(win, w, h);
	lv_obj_set_pos(win, x, y);
	lv_win_add_title(win, title);

	/* Drag the window by its title bar; raise it to the front on touch. */
	hdr = lv_win_get_header(win);
	lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hdr, win_drag_cb, LV_EVENT_PRESSING, win);
	lv_obj_add_event_cb(hdr, win_front_cb, LV_EVENT_PRESSED, win);
	return win;
}

/* ── window contents ────────────────────────────────────────────────────── */

static void build_monitor(lv_obj_t *win)
{
	lv_obj_t *c = lv_win_get_content(win);
	lv_obj_t *l;

	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_layout(c, 0);   /* LV_LAYOUT_NONE: absolute positioning */

	mon_label = lv_label_create(c);
	lv_obj_align(mon_label, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_label_set_text(mon_label, "MIPS  --\nstall --%");

	l = lv_label_create(c);
	lv_label_set_text(l, "rd hit");
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 60);
	bar_rd = lv_bar_create(c);
	lv_bar_set_range(bar_rd, 0, 100);
	lv_obj_set_size(bar_rd, 150, 12);
	lv_obj_align(bar_rd, LV_ALIGN_TOP_LEFT, 60, 62);

	l = lv_label_create(c);
	lv_label_set_text(l, "wr hit");
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 84);
	bar_wr = lv_bar_create(c);
	lv_bar_set_range(bar_wr, 0, 100);
	lv_obj_set_size(bar_wr, 150, 12);
	lv_obj_align(bar_wr, LV_ALIGN_TOP_LEFT, 60, 86);
}

static void build_chart(lv_obj_t *win)
{
	lv_obj_t *c = lv_win_get_content(win);

	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_layout(c, 0);   /* LV_LAYOUT_NONE: absolute positioning */

	act_chart = lv_chart_create(c);
	lv_obj_set_size(act_chart, lv_pct(100), lv_pct(100));
	lv_chart_set_type(act_chart, LV_CHART_TYPE_LINE);
	lv_chart_set_update_mode(act_chart, LV_CHART_UPDATE_MODE_SHIFT);
	lv_chart_set_point_count(act_chart, 50);
	lv_chart_set_range(act_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 25);
	act_ser = lv_chart_add_series(act_chart, lv_palette_main(LV_PALETTE_CYAN),
				      LV_CHART_AXIS_PRIMARY_Y);
}

static void build_controls(lv_obj_t *win)
{
	lv_obj_t *c = lv_win_get_content(win);
	lv_obj_t *slider, *btn, *lbl;

	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_layout(c, 0);   /* LV_LAYOUT_NONE: absolute positioning */

	count_label = lv_label_create(c);
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_28, 0);
	lv_obj_align(count_label, LV_ALIGN_TOP_LEFT, 0, 0);
	update_count();

	btn = lv_btn_create(c);
	lv_obj_set_size(btn, 56, 44);
	lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 44);
	lv_obj_add_event_cb(btn, btn_dec_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn); lv_label_set_text(lbl, LV_SYMBOL_MINUS);
	lv_obj_center(lbl);

	btn = lv_btn_create(c);
	lv_obj_set_size(btn, 56, 44);
	lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 64, 44);
	lv_obj_add_event_cb(btn, btn_inc_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn); lv_label_set_text(lbl, LV_SYMBOL_PLUS);
	lv_obj_center(lbl);

	slider = lv_slider_create(c);
	lv_obj_set_width(slider, 180);
	lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 0, 112);
	lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	slider_label = lv_label_create(c);
	lv_label_set_text(slider_label, "0 %");
	lv_obj_align(slider_label, LV_ALIGN_TOP_LEFT, 0, 134);

	lv_obj_align(lv_switch_create(c), LV_ALIGN_TOP_LEFT, 200, 112);

	/* Arc gauge driven by the slider (display-only). */
	ctl_arc = lv_arc_create(c);
	lv_obj_set_size(ctl_arc, 96, 96);
	lv_arc_set_range(ctl_arc, 0, 100);
	lv_arc_set_value(ctl_arc, 0);
	lv_obj_align(ctl_arc, LV_ALIGN_TOP_RIGHT, 0, 12);
	lv_obj_clear_flag(ctl_arc, LV_OBJ_FLAG_CLICKABLE);
}

static void build_desktop(void)
{
	lv_obj_t *scr = lv_scr_act();
	lv_obj_t *bar, *t, *w;

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x12161f), 0);
	lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x243049), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

	/* top bar: title + live uptime clock */
	bar = lv_obj_create(scr);
	lv_obj_set_size(bar, lv_pct(100), 28);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 4, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	t = lv_label_create(bar);
	lv_label_set_text(t, LV_SYMBOL_HOME "  KlaussCPU Desktop");
	lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

	clock_label = lv_label_create(bar);
	lv_label_set_text(clock_label, "up 00:00:00");
	lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, 0, 0);

	w = make_window("System Monitor", 8, 36, 300, 196);
	build_monitor(w);

	w = make_window("CPU activity (MIPS)", 316, 36, 316, 196);
	build_chart(w);

	w = make_window("Controls", 80, 240, 440, 226);
	build_controls(w);

	perf_sample(&perf_prev);
	perf_t_prev = k_uptime_get();
}

/* Periodic UI refresh (clock + live perf monitor + activity chart). */
static void ui_tick(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	struct perf_s now;
	int64_t tn = k_uptime_get();
	uint32_t up = (uint32_t)(tn / 1000);
	uint32_t dt_ms;
	uint64_t dins, dcyc, drh, drm, dwh, dwm, dst;
	uint32_t mips, stallp, rdhit, wrhit;

	lv_label_set_text_fmt(clock_label, "up %02u:%02u:%02u",
			      up / 3600u, (up / 60u) % 60u, up % 60u);

	perf_sample(&now);
	dt_ms = (uint32_t)(tn - perf_t_prev);
	if (dt_ms == 0u) {
		dt_ms = 1u;
	}
	dins = now.instr - perf_prev.instr;
	dcyc = now.cyc - perf_prev.cyc;
	drh = now.rh - perf_prev.rh; drm = now.rm - perf_prev.rm;
	dwh = now.wh - perf_prev.wh; dwm = now.wm - perf_prev.wm;
	dst = now.stall - perf_prev.stall;

	mips   = (uint32_t)(dins / ((uint64_t)dt_ms * 1000u));   /* instr/us == MIPS */
	stallp = dcyc ? (uint32_t)(dst * 100u / dcyc) : 0u;
	rdhit  = (drh + drm) ? (uint32_t)(drh * 100u / (drh + drm)) : 0u;
	wrhit  = (dwh + dwm) ? (uint32_t)(dwh * 100u / (dwh + dwm)) : 0u;

	lv_label_set_text_fmt(mon_label, "MIPS  %u\nstall %u%%", mips, stallp);
	lv_bar_set_value(bar_rd, rdhit, LV_ANIM_OFF);
	lv_bar_set_value(bar_wr, wrhit, LV_ANIM_OFF);
	lv_chart_set_next_value(act_chart, act_ser, mips);

	perf_prev = now;
	perf_t_prev = tn;
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
	uint64_t cyc, instr, fetch, exec, mul, div, rh, rm, wh, wm, wb, stall;
};

static void cstat_read(struct cstat *s)
{
	s->cyc   = REG64(0xF00D0008u);
	s->instr = REG64(0xF00D0010u);
	s->fetch = REG64(0xF00D0018u);   /* fetch/decode cycles (overlapped by a pipeline) */
	s->exec  = REG64(0xF00D0020u);   /* execute/writeback cycles (incl. mem + blit spin) */
	s->mul   = REG64(0xF00D0028u);
	s->div   = REG64(0xF00D0030u);
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

	(void)vncd_copy_cyc_reset();             /* clear flush-copy counters */
	(void)vncd_blit_cyc_reset();
	cstat_read(&c0);
	int64_t t0 = k_uptime_get();

	for (int i = 0; i < n; i++) {
		lv_obj_invalidate(scr);          /* force full-screen redraw */
		lv_refr_now(NULL);               /* synchronous render + flush */
	}
	uint32_t total = (uint32_t)((k_uptime_get() - t0) / n);

	cstat_read(&c1);
	/* 100 MHz core: 100 cycles = 1 us.  Sub-ms once the blitter offloads it.
	 * Split the copy into the blit engine itself vs the whole-cache
	 * FLUSH/INVALIDATE bracket (the part region-scoped maintenance would cut). */
	uint64_t copy_cyc = vncd_copy_cyc_reset();   /* total flush cycles over n */
	uint64_t blit_cyc = vncd_blit_cyc_reset();
	uint32_t copy_us = (uint32_t)(copy_cyc / 100ULL / n);
	uint32_t blit_us = (uint32_t)(blit_cyc / 100ULL / n);
	uint32_t cache_us = (copy_us > blit_us) ? copy_us - blit_us : 0;
	uint32_t copy = copy_us / 1000u;
	uint32_t render = (total > copy) ? total - copy : 0;

	uint64_t rh = c1.rh - c0.rh, rm = c1.rm - c0.rm;
	uint64_t wh = c1.wh - c0.wh, wm = c1.wm - c0.wm;
	uint64_t wb = c1.wb - c0.wb, st = c1.stall - c0.stall, cyc = c1.cyc - c0.cyc;
	uint64_t ins = c1.instr - c0.instr;
	/* Render-only cycles: subtract the synchronous flush (blit + cache walk +
	 * blit-wait), so CPI / stall% / the cyc split reflect the CPU's rendering
	 * alone — meaningful on every scene, not just the blit-light ones. */
	uint64_t rcyc = (cyc > copy_cyc) ? cyc - copy_cyc : cyc;
	uint64_t cpi_m = ins ? (rcyc * 1000ULL) / ins : 0;   /* milli-CPI, render-only */

	printk("LVGL bench %-9s: total=%u render=%u copy=%u ms/frame "
	       "(copy %u us = blit %u + cache %u)\n",
	       name, total, render, copy, copy_us, blit_us, cache_us);
	print_pct("rd hit:", rh, rh + rm);
	print_pct("wr hit:", wh, wh + wm);
	print_pct("stall%:", st, rcyc);
	printk("  CPI(rndr): %llu.%03llu  instr/fr=%llu\n",
	       (unsigned long long)(cpi_m / 1000), (unsigned long long)(cpi_m % 1000),
	       (unsigned long long)(ins / n));
	/* Cycle split over render-only cycles (Tier-1 perf counters): a true
	 * fetch/execute pipeline overlaps fetch away, so fetch%% should fall as the
	 * core improves.  Excludes the flush, so it sums to ~100%% on every scene. */
	uint64_t fe = c1.fetch - c0.fetch, ex = c1.exec - c0.exec;
	uint64_t md = (c1.mul - c0.mul) + (c1.div - c0.div);

	printk("  cyc split: fetch %llu%% exec %llu%% muldiv %llu%%\n",
	       (unsigned long long)(rcyc ? fe * 100 / rcyc : 0),
	       (unsigned long long)(rcyc ? ex * 100 / rcyc : 0),
	       (unsigned long long)(rcyc ? md * 100 / rcyc : 0));
	printk("  rm/fr=%llu wm/fr=%llu wb/fr=%llu\n",
	       (unsigned long long)(rm / n), (unsigned long long)(wm / n),
	       (unsigned long long)(wb / n));
}

static void run_benchmark(void)
{
	printk("=== LVGL render benchmark (640x480, render + flush=%s) ===\n",
	       vncd_blit_active() ? "blitter" : "memcpy");
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

#ifdef CONFIG_GUI_LVGL_BLIT_SELFTEST
	vncd_blit_selftest();   /* blitter HW check; no LVGL/VNC involved */
#endif

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

	build_desktop();
	lv_timer_create(ui_tick, 500, NULL);   /* clock + live monitor + chart */
	LOG_INF("LVGL desktop ready on VNC :5900 (flush: %s)",
		vncd_blit_active() ? "blitter" : "memcpy");

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
