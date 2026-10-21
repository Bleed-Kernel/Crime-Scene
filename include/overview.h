#pragma once
#include <stdint.h>
#include <display.h>
#include <keyboard.h>

// up, or on to the next one if its up already
void overview_open(display_t *d);

// the mouse and keyboard are its till its gone
int  overview_up(void);

void overview_mouse(display_t *d, int x, int y, uint8_t buttons);
void overview_key(display_t *d, const keyboard_event_t *ev);

void overview_tick(display_t *d);
int  overview_wait_ms(void);

// called once for each frame that goes out
void overview_frame(display_t *d);

void overview_draw(display_t *d, rect_t clip);
