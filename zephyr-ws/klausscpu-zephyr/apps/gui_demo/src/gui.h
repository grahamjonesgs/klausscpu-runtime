/*
 * gui.h — minimal immediate-mode-ish widgets drawn into the VNC framebuffer.
 */
#ifndef GUI_H_
#define GUI_H_

#include <stdint.h>
#include <stdbool.h>

/* Filled text at (x,y), `scale`x integer-scaled 8x8 glyphs (5px wide each). */
void gui_text(int x, int y, const char *s, uint16_t fg, int scale);

/* Solid rectangle. */
void gui_fill(int x, int y, int w, int h, uint16_t color);

/* `thick`-pixel rectangle outline. */
void gui_border(int x, int y, int w, int h, int thick, uint16_t color);

struct gui_button {
	int x, y, w, h;
	const char *label;
	void (*action)(void);
};

void gui_button_draw(const struct gui_button *b, bool pressed);
bool gui_button_hit(const struct gui_button *b, int px, int py);

#endif /* GUI_H_ */
