#pragma once
#include <stdint.h>
#include <stddef.h>

// everything a program needs of libcrime: what goes over the wire, the windows, and the
// syscalls blibc doesnt have yet

// protocol

#define CS_PID_FILE  "/tmp/crime-scene.pid"
#define CS_TITLE_MAX 64

#define CS_BUTTON_LEFT   (1 << 0)
#define CS_BUTTON_RIGHT  (1 << 1)
#define CS_BUTTON_MIDDLE (1 << 2)

enum {
    // client to server
    CS_REQ_OPEN = 1,        // w, h, title. w and h of 0 leave the size up to the desktop
    CS_REQ_CLOSE,           // window
    CS_REQ_COMMIT,          // window, x y w h of what was drawn, serial
    CS_REQ_MOVE,            // window, x y on the screen
    CS_REQ_POINTER_LOCK,    // window, x is 1 to take the pointer or 0 to give it back
    CS_REQ_RESIZE,          // window, w h it would like to be. the answer is a CS_EV_RESIZE
    CS_REQ_TERMINAL,        // a new terminal with a shell in it, title is the directory it starts in

    // server to client
    CS_EV_OPENED = 100,     // window (0 if it failed), shm, w, h, pitch
    CS_EV_CLOSE_REQUEST,    // the close button was clicked, its up to the client to close
    CS_EV_MOUSE,            // x y inside the window, buttons
    CS_EV_KEY,              // keycode, key_action, keymod
    CS_EV_RESIZE,           // w, h, pitch, shm, serial. shm is often the same buffer as before
    CS_EV_FOCUS,            // focused says whether it just got the focus or just lost it
    CS_EV_MOUSE_RELATIVE,   // x y are how far the mouse moved, buttons. only while the pointer is locked
};

// every message is one of these in a page of its own
typedef struct {
    uint32_t type;
    uint32_t window;
    int32_t  x, y;
    int32_t  w, h;
    uint32_t buttons;
    uint32_t focused;
    int32_t  pitch;         // pixels from one row of the buffer to the next, at least w
    uint32_t serial;        // counts resizes, a commit says which one it was drawn for
    uint16_t keycode;
    uint8_t  key_action;
    uint8_t  keymod;
    uint64_t shm;
    char     title[CS_TITLE_MAX];
} cs_msg_t;

// windows

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

// syscalls

#define CS_PAGE_SIZE 4096UL

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

#define POLL_FD_IPC (-2)

struct pollfd {
    int   fd;
    short events;
    short revents;
};

// blibc doesnt have these yet
long poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);
long shm_create(unsigned long pages);
long shm_map(unsigned long id, size_t *pages_out);
long shm_destroy(unsigned long id);

int cs_msg_send(uint64_t pid, const cs_msg_t *msg);
int cs_msg_recv(cs_msg_t *msg, uint64_t *sender);
