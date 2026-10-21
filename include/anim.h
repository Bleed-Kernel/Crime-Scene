#pragma once
#include <display.h>
#include <window.h>

// while a window is animating its picture is drawn instead of it

void anim_open(display_t *d, window_t *win);

// the window is about to be freed, so its picture is taken now
void anim_close(display_t *d, window_t *win);

void anim_minimise(display_t *d, window_t *win, rect_t button);
void anim_restore(display_t *d, window_t *win, rect_t button);

// win is going, anything of it thats moving with it stops
void anim_forget(window_t *win);

// how long until it wants another frame, -1 for never
void anim_tick(display_t *d);
int  anim_wait_ms(void);

// quick to start, slow to finish, t from 0 to 1
float anim_ease(float t);

// called once for each frame that goes out
void anim_frame(display_t *d);

void anim_draw(display_t *d, rect_t clip);
