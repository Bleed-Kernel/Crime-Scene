#pragma once
#include <display.h>

#define WALLPAPER_PATH "/initrd/etc/wallpaper.png"

// reads the wallpaper and fits it to the screen. no file or a bad one and the desktop is just black
void wallpaper_init(const display_t *d);

// whats behind every window, the part of it inside clip
void wallpaper_draw(display_t *d, rect_t clip);
