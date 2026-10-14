#pragma once
#include <display.h>
#include <window.h>
#include <input.h>
#include <cursor.h>

// 1 while a window has the pointer locked: its hidden and movement goes to wm_mouse_relative
int wm_locked(void);
void wm_mouse_relative(display_t *d, int dx, int dy, uint8_t buttons);

// hand the pointer back if a window has it, and bring the next window forward
void wm_switch(display_t *d);

// the pointer is at x y after this event
void wm_mouse(display_t *d, int x, int y, uint8_t buttons);
void wm_key(display_t *d, const keyboard_event_t *ev);

// once a frame, sends on the pointer motion that built up since the last one
void wm_frame(display_t *d);

// call often. a resize thats in progress catches up with the mouse here, once the client is ready
void wm_tick(display_t *d);

// 1 while a resize is waiting on a client, the caller shouldnt sleep long
int wm_busy(void);

// what the pointer should look like where it is now
cursor_shape_t wm_cursor(void);

// win is about to go away
void wm_forget(window_t *win);
