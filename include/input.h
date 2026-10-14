#pragma once
#include <keyboard.h>
#include <mouse.h>

int mouse_open(void);

// one event at a time, 0 when theres nothing left
int mouse_next(int fd, mouse_event_t *ev);
int keyboard_next(int fd, keyboard_event_t *ev);
