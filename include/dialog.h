#pragma once
#include <stdint.h>
#include <display.h>
#include <keyboard.h>

// a box in the middle of the screen. while its up it has the mouse and keyboard to itself

// yes or cancel, on_yes is called if its yes
void dialog_confirm(display_t *d, const char *title, const char *text, const char *yes,
                    void (*on_yes)(display_t *d));

// type a program in and run it
void dialog_run(display_t *d);

int  dialog_up(void);
void dialog_draw(display_t *d, rect_t clip);

// 1 if the dialog took it, always while its up
int  dialog_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons);
int  dialog_key(display_t *d, const keyboard_event_t *ev);
