#pragma once
#include <display.h>

typedef enum {
    CURSOR_ARROW,
    CURSOR_SIZE_H,      // left and right edges
    CURSOR_SIZE_V,      // top and bottom
    CURSOR_SIZE_NWSE,   // top left and bottom right corners
    CURSOR_SIZE_NESW,   // the other two
} cursor_shape_t;

typedef struct {
    int            x, y;    // where the hotspot is
    cursor_shape_t shape;
} cursor_t;

rect_t cursor_rect(const cursor_t *c);
void   cursor_move(cursor_t *c, const display_t *d, int dx, int dy);
void   cursor_draw(const cursor_t *c, display_t *d, rect_t clip);
