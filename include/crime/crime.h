#pragma once
#include <stdint.h>
#include <crime/protocol.h>

typedef struct cs_window {
    uint32_t  id;
    int       width;
    int       height;
    int       pitch;    // pixels from one row to the next, can be more than width
    uint32_t *pixels;   // 0x00RRGGBB, draw here then commit

    // these shouldnt be touched by anything else
    uint64_t  shm;
    uint32_t  serial;
} cs_window_t;

typedef cs_msg_t cs_event_t;

// find the running desktop, -1 if there isnt one
int cs_connect(void);

// ask the desktop for a new terminal window. the shell in it starts in cwd, or wherever
// the desktop is if thats NULL.
int cs_terminal_open(const char *cwd);

// the size the desktop gives a window that doesnt ask for one, a share of the screen
#define CS_SIZE_DEFAULT 0

cs_window_t *cs_window_open(int width, int height, const char *title);
void cs_window_close(cs_window_t *win);

// tell the desktop part of the window changed, nothing shows up until this
void cs_window_commit(cs_window_t *win, int x, int y, int w, int h);
void cs_window_move(cs_window_t *win, int x, int y);

// ask for a different size. it happens when the CS_EV_RESIZE for it turns up, and it
// doesnt happen at all while the window is maximised
void cs_window_resize(cs_window_t *win, int width, int height);

// ask for the pointer: its hidden, it stops moving, and the window gets CS_EV_MOUSE_RELATIVE
// with raw movement in place of positions. the desktop only honours it while the window has
// the focus
void cs_pointer_lock(cs_window_t *win, int on);

// 1 with an event in ev, 0 if timeout_ms ran out first. negative waits forever.
int cs_event_wait(cs_event_t *ev, int timeout_ms);
