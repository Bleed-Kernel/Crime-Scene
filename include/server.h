#pragma once
#include <display.h>
#include <window.h>
#include <crime/protocol.h>

int  server_init(void);
void server_shutdown(display_t *d);

void server_requests(display_t *d);

void server_sweep(display_t *d);

// if the client is gone its windows go with it
void server_send(display_t *d, window_t *win, cs_msg_t *msg);

// resize win and tell its client. 0 if the client has caught up with the last one, so its worth sending another
void server_resize(display_t *d, window_t *win, int x, int y, int w, int h);

// fill the screen, or go back to how it was before it did
void server_maximise(display_t *d, window_t *win);

// tell clients when the window with the focus changes
void server_focus(display_t *d);
