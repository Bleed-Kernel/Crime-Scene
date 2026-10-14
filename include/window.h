#pragma once
#include <stdint.h>
#include <display.h>
#include <crime/protocol.h>

// how far the outside of the window is from the content
#define WINDOW_FRAME     4      // the raised edge and the grey inside it
#define WINDOW_CAPTION   22
#define WINDOW_EDGE      2      // the sunken edge round the content
#define WINDOW_INSET     (WINDOW_FRAME + WINDOW_EDGE)
#define WINDOW_INSET_TOP (WINDOW_FRAME + WINDOW_CAPTION + 1 + WINDOW_EDGE)

#define WINDOW_BUTTON_W  18
#define WINDOW_BUTTON_H  16

// the buttons on the title bar
#define WINDOW_BUTTON_MIN   1
#define WINDOW_BUTTON_MAX   2
#define WINDOW_BUTTON_CLOSE 3

#define WINDOW_SHADOW     14     // how far the shadow can reach past the frame

#define WINDOW_GROW       128    // buffers are sized up to a multiple of this

#define WINDOW_MIN_W      160
#define WINDOW_MIN_H      80

typedef struct window {
    uint32_t  id;
    uint64_t  owner;    // pid of the client
    uint64_t  shm;
    uint32_t *pixels;
    int       x, y;
    int       w, h;
    int       cap_w, cap_h;
    char      title[CS_TITLE_MAX];
    uint8_t   button_down;  // which title bar button is being held in
    uint8_t   button_hover; // which one the pointer is sitting on

    uint8_t   minimised;    // not on screen, only its taskbar button is
    uint8_t   maximised;    // filling the screen, restore is what it goes back to
    rect_t    restore;

    uint8_t   wants_lock;
    uint8_t   lock_broken;
    int       slot;

    uint32_t  resize_serial;
    uint32_t  resize_acked;

    struct term *term;
} window_t;

window_t *window_open(display_t *d, uint64_t owner, int w, int h, const char *title);
void      window_close(display_t *d, window_t *win);
void      window_move(display_t *d, window_t *win, int x, int y);
void      window_raise(display_t *d, window_t *win);

// off the screen and back. a restored window comes back on top
void      window_minimise(display_t *d, window_t *win);
void      window_restore(display_t *d, window_t *win);

// the content rect a window has when it fills the screen
rect_t    window_maximised_rect(const display_t *d);

// new place and size for the content. 0 if it worked
int       window_resize(display_t *d, window_t *win, int x, int y, int w, int h);

window_t *window_find(uint32_t id);
window_t *window_at(int x, int y);
window_t *window_top(void);

// bottom to top, NULL once index runs off the end
window_t *window_index(int index);

rect_t window_frame(const window_t *win);
rect_t window_bounds(const window_t *win);     // the frame plus its shadow, what to damage
rect_t window_titlebar(const window_t *win);
rect_t window_content(const window_t *win);
rect_t window_button(const window_t *win, int which);
int    window_button_at(const window_t *win, int x, int y);

// everything inside clip: the wallpaper, then the windows from the bottom up
void windows_compose(display_t *d, rect_t clip);
