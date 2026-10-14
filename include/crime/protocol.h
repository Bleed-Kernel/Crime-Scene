#pragma once
#include <stdint.h>

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
