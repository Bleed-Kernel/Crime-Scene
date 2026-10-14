#include <stddef.h>
#include <wm.h>
#include <server.h>
#include <taskbar.h>
#include <clock.h>
#include <crime/crime.h>

static uint8_t   buttons_last;

static window_t *dragging;      // being moved by its title bar
static int       drag_dx;
static int       drag_dy;

static window_t *pressing;      // a title bar button went down on this one
static int       press_which;   // and which button it was
static window_t *grabbed;       // the press started in its content, it keeps the pointer until release

static window_t *hovered;       // the pointer is resting on one of its title bar buttons

#define DOUBLE_CLICK_FS (400 * FS_PER_MS)

static window_t *clicked;       // the title bar that was last pressed
static uint64_t  clicked_at;

#define EDGE_LEFT    (1 << 0)
#define EDGE_RIGHT   (1 << 1)
#define EDGE_TOP     (1 << 2)
#define EDGE_BOTTOM  (1 << 3)

#define CORNER_REACH 16         // this close to a corner along an edge and you have both edges

// a client that hasnt drawn for the last resize by now isnt going to hold up the next one
#define RESIZE_WAIT_FS (100 * FS_PER_MS)

// the window really changes size as the mouse moves. each step waits for the client to
// draw the one before, otherwise a slow program would have resizes piling up behind it
static window_t *resizing;
static int       resize_edges;
static int       resize_from_x;
static int       resize_from_y;
static rect_t    resize_start;  // the content rect when the drag began
static rect_t    resize_want;   // where the mouse says it should be now
static uint64_t  resize_sent;   // when the last step went out

static int       hover_edges;   // edges under the pointer with nothing held, for its shape

// the window that has the pointer locked. its hidden and stays put, the window gets how far
// the mouse moved instead of where it is
static window_t *locked;
static int       locked_dx;     // movement owed to it at the next frame
static int       locked_dy;
static int       locked_owed;

static window_t *motion_for;    // owed a motion event at the next frame
static int       motion_x;
static int       motion_y;

void wm_forget(window_t *win) {
    if (dragging == win)   dragging = NULL;
    if (pressing == win)   pressing = NULL;
    if (clicked == win)    clicked = NULL;
    if (grabbed == win)    grabbed = NULL;
    if (hovered == win)    hovered = NULL;
    if (resizing == win)   resizing = NULL;
    if (locked == win)     locked = NULL;
    if (motion_for == win) motion_for = NULL;
}

static void send_mouse(display_t *d, window_t *win, int x, int y, uint8_t buttons) {
    cs_msg_t msg = {
        .type    = CS_EV_MOUSE,
        .x       = x - win->x,
        .y       = y - win->y,
        .buttons = buttons,
    };
    server_send(d, win, &msg);
}

// which edges of win the pointer is on, 0 if its not on the border at all
static int edges_at(const window_t *win, int x, int y) {
    // a window filling the screen has its border off the edge, theres nothing to grab
    if (win->maximised)
        return 0;

    rect_t frame = window_frame(win);
    rect_t inner = {frame.x + WINDOW_FRAME, frame.y + WINDOW_FRAME,
                    frame.w - WINDOW_FRAME * 2, frame.h - WINDOW_FRAME * 2};
    if (!rect_contains(frame, x, y) || rect_contains(inner, x, y))
        return 0;

    int edges = 0;
    if (x < inner.x)             edges |= EDGE_LEFT;
    if (x >= inner.x + inner.w)  edges |= EDGE_RIGHT;
    if (y < inner.y)             edges |= EDGE_TOP;
    if (y >= inner.y + inner.h)  edges |= EDGE_BOTTOM;

    // the corner itself is a few pixels square, far too small a thing to aim for
    if (edges & (EDGE_TOP | EDGE_BOTTOM)) {
        if (x < frame.x + CORNER_REACH)             edges |= EDGE_LEFT;
        if (x >= frame.x + frame.w - CORNER_REACH)  edges |= EDGE_RIGHT;
    }
    if (edges & (EDGE_LEFT | EDGE_RIGHT)) {
        if (y < frame.y + CORNER_REACH)             edges |= EDGE_TOP;
        if (y >= frame.y + frame.h - CORNER_REACH)  edges |= EDGE_BOTTOM;
    }
    return edges;
}

static int resize_differs(const window_t *win, rect_t r) {
    return r.x != win->x || r.y != win->y || r.w != win->w || r.h != win->h;
}

// take the window to resize_want if its not there already. force skips waiting on the client
static void resize_step(display_t *d, int force) {
    if (!resizing || !resize_differs(resizing, resize_want))
        return;

    int caught_up = resizing->resize_acked == resizing->resize_serial;
    int waited    = clock_now() - resize_sent >= RESIZE_WAIT_FS;
    if (!force && !caught_up && !waited)
        return;

    // server_resize can find the client dead and take the window away, resizing goes NULL with it
    resize_sent = clock_now();
    server_resize(d, resizing, resize_want.x, resize_want.y, resize_want.w, resize_want.h);
}

static void resize_drag(display_t *d, int x, int y) {
    rect_t r  = resize_start;
    int    dx = x - resize_from_x;
    int    dy = y - resize_from_y;

    if (resize_edges & EDGE_RIGHT)  r.w += dx;
    if (resize_edges & EDGE_BOTTOM) r.h += dy;
    if (resize_edges & EDGE_LEFT) {
        if (dx > r.w - WINDOW_MIN_W) dx = r.w - WINDOW_MIN_W;
        r.x += dx;
        r.w -= dx;
    }
    if (resize_edges & EDGE_TOP) {
        if (dy > r.h - WINDOW_MIN_H) dy = r.h - WINDOW_MIN_H;
        if (r.y + dy < WINDOW_INSET_TOP) dy = WINDOW_INSET_TOP - r.y;
        r.y += dy;
        r.h -= dy;
    }

    if (r.w < WINDOW_MIN_W) r.w = WINDOW_MIN_W;
    if (r.h < WINDOW_MIN_H) r.h = WINDOW_MIN_H;
    if (r.w > d->width)     r.w = d->width;
    if (r.h > d->height)    r.h = d->height;

    resize_want = r;
    resize_step(d, 0);
}

static void button_hover(display_t *d, int x, int y);

static void send_relative(display_t *d) {
    window_t *win = locked;
    cs_msg_t  msg = {
        .type    = CS_EV_MOUSE_RELATIVE,
        .x       = locked_dx,
        .y       = locked_dy,
        .buttons = buttons_last,
    };

    locked_dx   = 0;
    locked_dy   = 0;
    locked_owed = 0;
    server_send(d, win, &msg);
}

static void lock_release(int broken) {
    if (!locked)
        return;
    if (broken)
        locked->lock_broken = 1;
    locked      = NULL;
    locked_owed = 0;
}

int wm_locked(void) {
    return locked != NULL;
}

void wm_mouse_relative(display_t *d, int dx, int dy, uint8_t buttons) {
    if (!locked)
        return;

    uint8_t changed = buttons != buttons_last;
    buttons_last = buttons;

    locked_dx  += dx;
    locked_dy  += dy;
    locked_owed = 1;

    // a button going down or up cant wait for the frame, the movement so far goes with it
    if (changed)
        send_relative(d);
}

void wm_switch(display_t *d) {
    lock_release(1);

    // the one at the very bottom comes to the front, so repeating it goes round them all
    for (int i = 0; window_index(i); i++) {
        window_t *win = window_index(i);
        if (!win->minimised && win != window_top()) {
            window_raise(d, win);
            return;
        }
    }
}

void wm_tick(display_t *d) {
    resize_step(d, 0);

    // a lock only holds while its window wants it and has the focus
    if (locked && !locked->wants_lock)
        lock_release(0);
    if (locked && window_top() != locked)
        lock_release(1);

    window_t *top = window_top();
    if (!locked && top && top->wants_lock && !top->lock_broken && !dragging && !resizing && !pressing) {
        locked      = top;
        locked_dx   = 0;
        locked_dy   = 0;
        locked_owed = 0;

        // whatever the pointer was in the middle of is over
        grabbed     = NULL;
        motion_for  = NULL;
        hover_edges = 0;
        button_hover(d, -1, -1);
    }
}

int wm_busy(void) {
    return resizing && resize_differs(resizing, resize_want);
}

cursor_shape_t wm_cursor(void) {
    int edges = resizing ? resize_edges : hover_edges;

    int across = edges & (EDGE_LEFT | EDGE_RIGHT);
    int down   = edges & (EDGE_TOP | EDGE_BOTTOM);
    if (across && down) {
        int nwse = (edges & EDGE_LEFT ? 1 : 0) == (edges & EDGE_TOP ? 1 : 0);
        return nwse ? CURSOR_SIZE_NWSE : CURSOR_SIZE_NESW;
    }
    if (across) return CURSOR_SIZE_H;
    if (down)   return CURSOR_SIZE_V;
    return CURSOR_ARROW;
}

// the button only looks pushed in while the pointer is actually on it
static void button_hold(display_t *d, int x, int y) {
    if (!pressing)
        return;

    uint8_t down = rect_contains(window_button(pressing, press_which), x, y) ? press_which : 0;
    if (pressing->button_down == down)
        return;

    pressing->button_down = down;
    display_damage(d, window_button(pressing, press_which));
}

// lights a title bar button up when the pointer is on it and nothing else is going on
static void button_hover(display_t *d, int x, int y) {
    window_t *win   = (dragging || grabbed || pressing || resizing) ? NULL : window_at(x, y);
    int       which = win ? window_button_at(win, x, y) : 0;
    if (!which)
        win = NULL;
    if (win == hovered && (!win || win->button_hover == which))
        return;

    if (hovered) {
        display_damage(d, window_button(hovered, hovered->button_hover));
        hovered->button_hover = 0;
    }
    if (win) {
        win->button_hover = (uint8_t)which;
        display_damage(d, window_button(win, which));
    }
    hovered = win;
}

static void press(display_t *d, int x, int y, uint8_t buttons) {
    window_t *win = window_at(x, y);
    if (!win)
        return;

    window_raise(d, win);

    int which = window_button_at(win, x, y);
    int edges = edges_at(win, x, y);

    if (which) {
        pressing    = win;
        press_which = which;
        button_hold(d, x, y);
    } else if (edges) {
        resizing      = win;
        resize_edges  = edges;
        resize_from_x = x;
        resize_from_y = y;
        resize_start  = window_content(win);
        resize_want   = resize_start;
        resize_sent   = 0;
    } else if (rect_contains(window_titlebar(win), x, y)) {
        uint64_t now = clock_now();
        if (clicked == win && now - clicked_at < DOUBLE_CLICK_FS) {
            clicked = NULL;
            server_maximise(d, win);
            return;
        }
        clicked    = win;
        clicked_at = now;

        // a window filling the screen has nowhere to be dragged to
        if (!win->maximised) {
            dragging = win;
            drag_dx  = x - win->x;
            drag_dy  = y - win->y;
        }
    } else if (rect_contains(window_content(win), x, y)) {
        // clicking in a window that wants the pointer is how it gets it back after alt tab
        win->lock_broken = 0;

        grabbed = win;
        send_mouse(d, win, x, y, buttons);
    }
}

static void release(display_t *d, int x, int y, uint8_t buttons) {
    if (grabbed)
        send_mouse(d, grabbed, x, y, buttons);

    // only counts if it comes back up on the button too, same as everywhere else
    if (pressing) {
        window_t *win  = pressing;
        int       over = rect_contains(window_button(win, press_which), x, y);

        win->button_down = 0;
        display_damage(d, window_button(win, press_which));
        pressing = NULL;

        if (over && press_which == WINDOW_BUTTON_MIN)
            window_minimise(d, win);
        if (over && press_which == WINDOW_BUTTON_MAX)
            server_maximise(d, win);
        if (over && press_which == WINDOW_BUTTON_CLOSE) {
            cs_msg_t msg = { .type = CS_EV_CLOSE_REQUEST };
            server_send(d, win, &msg);
        }
    }

    // the last step goes through whether the client is ready or not, its where the mouse let go
    if (resizing) {
        resize_step(d, 1);
        resizing = NULL;
    }

    dragging = NULL;
    grabbed  = NULL;
}

void wm_mouse(display_t *d, int x, int y, uint8_t buttons) {
    uint8_t left     = buttons & MOUSE_BTN_LEFT;
    uint8_t was      = buttons_last;
    uint8_t was_left = was & MOUSE_BTN_LEFT;
    buttons_last = buttons;

    if (!dragging && !resizing && !grabbed && !pressing && taskbar_mouse(d, x, y, buttons, was)) {
        hover_edges = 0;
        button_hover(d, -1, -1);
        return;
    }

    if (left && !was_left) {
        press(d, x, y, buttons);
        button_hover(d, x, y);
        return;
    }
    if (!left && was_left) {
        release(d, x, y, buttons);
        button_hover(d, x, y);
        return;
    }

    if (dragging) {
        window_move(d, dragging, x - drag_dx, y - drag_dy);
        return;
    }
    if (resizing) {
        resize_drag(d, x, y);
        return;
    }

    // only matters for what the pointer looks like, the press works it out again for itself
    window_t *under = left ? NULL : window_at(x, y);
    hover_edges = (under && !window_button_at(under, x, y)) ? edges_at(under, x, y) : 0;
    button_hold(d, x, y);
    button_hover(d, x, y);

    // plain motion, goes to whoever has the pointer
    window_t *win = grabbed;
    if (!win && !pressing && !left) {
        win = window_at(x, y);
        if (win && !rect_contains(window_content(win), x, y))
            win = NULL;
    }

    motion_for = win;
    motion_x   = x;
    motion_y   = y;
}

void wm_frame(display_t *d) {
    if (locked && locked_owed)
        send_relative(d);

    if (!motion_for)
        return;

    window_t *win = motion_for;
    motion_for = NULL;
    send_mouse(d, win, motion_x, motion_y, buttons_last);
}

void wm_key(display_t *d, const keyboard_event_t *ev) {
    window_t *win = window_top();
    if (!win)
        return;

    cs_msg_t msg = {
        .type       = CS_EV_KEY,
        .keycode    = ev->keycode,
        .key_action = ev->action,
        .keymod     = ev->keymod,
    };
    server_send(d, win, &msg);
}
