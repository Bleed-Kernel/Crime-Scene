#pragma once
#include <stdint.h>
#include <display.h>

#define TASKBAR_H 38

rect_t taskbar_rect(const display_t *d);

struct window;
rect_t taskbar_button_of(const display_t *d, const struct window *win);

// notice anything thats changed since it was last drawn and mark it for reddraw
void taskbar_update(display_t *d);

void taskbar_draw(display_t *d, rect_t clip);

// open the start menu, or put it away if its open
void taskbar_menu_toggle(display_t *d);

// the start menu and any right click menu go away
void taskbar_menus_close(display_t *d);
int  taskbar_menus_up(void);

// the pointer is at x y. buttons is whats down now, was_buttons what was an event ago.
// brings a buttons name up once the pointer has rested on it long enough
void taskbar_tick(display_t *d);

int  taskbar_wait_ms(void);

// before each frame, redraws the bar when the focused window changes only
void taskbar_frame(display_t *d);

// returns 1 if that was the taskbars to deal with and nothing under it should see it
int taskbar_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons);

int taskbar_wheel(display_t *d, int x, int y, int wheel);
