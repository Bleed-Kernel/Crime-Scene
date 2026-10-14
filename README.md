# Crime-Scene (For the Bleed Kernel)

Crime-Scene is the desktop for Bleed. it runs in usermode as one program, the kernel knows nothing about windows. as the good lord intended

Run `crime-scene` with no desktop up and it is the desktop, run it again while one is up and all it does is ask for another terminal.

Crime Scene will not start automatically, it is very important that Verdict Starts first as Bleed kinda depends on it, if crime scene starts first expect the unexpected.

## Features

- Start menu for launching programs
- Taskbar buttons for windows
- Clock, battery and a low memory warning in the tray
- Dragging, resizing, minimising and maximising windows
- Alt Tab to go round the windows
- Terminal Windows with Verdict
- Cursor Locking Semantics
- Window events
- TrueType fonts from `/initrd/fonts`
- PNG wallpaper, scaled to cover the screen
- Ctrl Alt Backspace to quit back to tty mode

## Using the API

Programs talk to the desktop with `libcrime.a`, the header is `crime/crime.h`. Add `-I<Crime-Scene>/include` and link `bin/libcrime.a` before blibc.

```c
#include <crime/crime.h>

cs_window_t *win = cs_window_open(320, 200, "Window Title");
if (!win)
    return 1;   // theres no desktop

for (int i = 0; i < win->pitch * win->height; i++)
    win->pixels[i] = 0x2B2B2F;
cs_window_commit(win, 0, 0, win->width, win->height);

cs_event_t ev;
while (cs_event_wait(&ev, -1)) {
    if (ev.type == CS_EV_CLOSE_REQUEST)
        break;
}
cs_window_close(win);
```

Nothing shows up until you commit, and closing is up to you. the close button only sends the event.

## Functions

| Function | Args | Return value | Description |
|---|---|---|---|
| cs_connect | N/A | 0, or -1 if there isnt a desktop | Find the running desktop. the others do this for you |
| cs_window_open | int width, int height, const char *title | cs_window_t*, NULL if it failed | A new window. `CS_SIZE_DEFAULT` (0) for either size leaves it up to the desktop |
| cs_window_close | cs_window_t *win | N/A | Close the window and unmap its pixels |
| cs_window_commit | cs_window_t *win, int x, int y, int w, int h | N/A | Tell the desktop that part of the window changed |
| cs_window_move | cs_window_t *win, int x, int y | N/A | Move the window on the screen |
| cs_window_resize | cs_window_t *win, int width, int height | N/A | Ask for a different size. it happens when the `CS_EV_RESIZE` turns up, and not at all while maximised |
| cs_pointer_lock | cs_window_t *win, int on | N/A | Hide the pointer and get raw movement instead of positions |
| cs_event_wait | cs_event_t *ev, int timeout_ms | 1 with an event, 0 if the timeout ran out | Wait for the next event. a negative timeout waits forever |
| cs_terminal_open | const char *cwd | 0 once its been asked | A new terminal window, the shell starts in cwd (or wherever the desktop is if its NULL) |

A program can have 16 windows open at once. maximum

The pointer lock only holds while the window has the focus, and the user can always take it back with Alt Tab. it stays free after that until they click in the window again.

## Types

### cs_window_t

| Type | Name | Description |
|---|---|---|
| uint32_t | id | The desktops name for the window |
| int | width | Width of the window in pixels |
| int | height | Height of the window in pixels |
| int | pitch | Pixels from one row to the next, can be more than width |
| uint32_t* | pixels | 0x00RRGGBB, draw here then commit |
| uint64_t | shm | The librarys own, leave it alone |
| uint32_t | serial | The librarys own, leave it alone |

The size is whats inside the frame, the title bar and border are the desktops and arent part of it.

Always step rows by `pitch` and not `width`, the buffer is made bigger than asked for so a small resize doesnt need a new one and allocating memory for a small resize is silly

### cs_event_t

Every event is the same struct (`cs_msg_t`), which fields mean anything depends on `type`.

| Type | Name | Description |
|---|---|---|
| uint32_t | type | One of the `CS_EV_*` below |
| uint32_t | window | The id of the window its for |
| int32_t | x, y | Pointer position or movement |
| int32_t | w, h | New size |
| uint32_t | buttons | `CS_BUTTON_LEFT`, `CS_BUTTON_RIGHT`, `CS_BUTTON_MIDDLE` |
| uint32_t | focused | 1 if the focus was just given, 0 if it was just lost |
| int32_t | pitch | New pitch |
| uint32_t | serial | Counts resizes |
| uint16_t | keycode | Keycode from the keyboard driver |
| uint8_t | key_action | `KEY_DOWN` or `KEY_RELEASE` |
| uint8_t | keymod | `KEYMOD_SHIFT`, `KEYMOD_CTRL`, `KEYMOD_ALT`, `KEYMOD_CAPS` |

## Events

### Close request

The close button was clicked. the window stays until you close it.

| Name | Description |
|---|---|
| type | Always `CS_EV_CLOSE_REQUEST` |

### Mouse

| Name | Description |
|---|---|
| type | Always `CS_EV_MOUSE` |
| x, y | Position inside the window |
| buttons | Which buttons are down right now |

There isnt a separate click event, compare `buttons` with the last one you got.

### Mouse relative

Only while the pointer is locked.

| Name | Description |
|---|---|
| type | Always `CS_EV_MOUSE_RELATIVE` |
| x, y | How far the mouse moved |
| buttons | Which buttons are down right now |

### Key

| Name | Description |
|---|---|
| type | Always `CS_EV_KEY` |
| keycode | The same keycode the keyboard device gives |
| key_action | Pressed or released |
| keymod | Which modifiers are held |

### Resize

By the time this comes out of `cs_event_wait` the window already has its new size and pixels, all thats left is to draw into them and commit.

| Name | Description |
|---|---|
| type | Always `CS_EV_RESIZE` |
| w, h | The new size |
| pitch | The new pitch |

### Focus

| Name | Description |
|---|---|
| type | Always `CS_EV_FOCUS` |
| focused | Whether it just got the focus or just lost it |