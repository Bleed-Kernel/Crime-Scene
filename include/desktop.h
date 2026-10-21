#pragma once
#include <stdint.h>
#include <display.h>

// desktop icon logic
void desktop_init(display_t *d);

// over the wallpaper, under every window
void desktop_draw(display_t *d, rect_t clip);

int  desktop_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons);
