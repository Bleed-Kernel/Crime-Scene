#pragma once
#include <stdint.h>
#include <display.h>

// cards at the top right that go after a few seconds, or when clicked
void notify_post(display_t *d, const char *title, const char *text);

// how long until it wants another frame, -1 for never
void notify_tick(display_t *d);
int  notify_wait_ms(void);

// called once for each frame that goes out
void notify_frame(display_t *d);

void notify_draw(display_t *d, rect_t clip);

// 1 if the pointer was on a card
int  notify_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons);
