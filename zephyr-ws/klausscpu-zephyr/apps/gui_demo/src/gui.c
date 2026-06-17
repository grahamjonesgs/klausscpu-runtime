/*
 * gui.c — minimal widget drawing on top of the VNC framebuffer primitives.
 */
#include <string.h>

#include "framebuffer.h"
#include "gui.h"
#include "font8x8.h"

void gui_fill(int x, int y, int w, int h, uint16_t color)
{
	fb_fill_rect(x, y, w, h, color);
}

void gui_border(int x, int y, int w, int h, int thick, uint16_t color)
{
	fb_fill_rect(x, y, w, thick, color);             /* top    */
	fb_fill_rect(x, y + h - thick, w, thick, color); /* bottom */
	fb_fill_rect(x, y, thick, h, color);             /* left   */
	fb_fill_rect(x + w - thick, y, thick, h, color); /* right  */
}

void gui_text(int x, int y, const char *s, uint16_t fg, int scale)
{
	int x0 = x;

	fb_lock();
	uint16_t *fb = fb_pixels();

	for (; *s; s++) {
		const uint8_t *g = font_glyph(*s);

		for (int row = 0; row < 8; row++) {
			uint8_t bits = g[row];

			while (bits) {
				int col = __builtin_clz((unsigned)bits) - 24;
				int px0 = x + col * scale;
				int py0 = y + row * scale;

				for (int dy = 0; dy < scale; dy++) {
					for (int dx = 0; dx < scale; dx++) {
						int px = px0 + dx, py = py0 + dy;

						if ((unsigned)px < FB_WIDTH &&
						    (unsigned)py < FB_HEIGHT) {
							fb[(size_t)py * FB_WIDTH + px] = fg;
						}
					}
				}
				bits &= ~(0x80u >> col);
			}
		}
		x += 6 * scale;
	}
	fb_unlock();
	fb_mark_dirty(x0, y, x - x0, 8 * scale);
}

void gui_button_draw(const struct gui_button *b, bool pressed)
{
	uint16_t face = pressed ? fb_rgb(90, 90, 150) : fb_rgb(55, 60, 95);

	gui_fill(b->x, b->y, b->w, b->h, face);
	gui_border(b->x, b->y, b->w, b->h, 2, fb_rgb(170, 180, 230));

	int sc = 2;
	int tw = (int)strlen(b->label) * 6 * sc;
	int tx = b->x + (b->w - tw) / 2;
	int ty = b->y + (b->h - 8 * sc) / 2;

	gui_text(tx, ty, b->label, fb_rgb(255, 255, 255), sc);
}

bool gui_button_hit(const struct gui_button *b, int px, int py)
{
	return px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h;
}
