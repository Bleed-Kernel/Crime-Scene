#pragma once
#include <stdint.h>
#include <display.h>

#define TASKBAR_H 30

rect_t taskbar_rect(const display_t *d);

// notice anything thats changed since it was last drawn and mark it for reddraw
void taskbar_update(display_t *d);

void taskbar_draw(display_t *d, rect_t clip);

// open the start menu, or put it away if its open
void taskbar_menu_toggle(display_t *d);

// the pointer is at x y. buttons is whats down now, was_buttons what was an event ago.
// returns 1 if that was the taskbars to deal with and nothing under it should see it
int taskbar_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons);
