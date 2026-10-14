#include <fcntl.h>
#include <syscalls.h>
#include <input.h>

#define BATCH 32

static mouse_event_t mouse_batch[BATCH];
static int           mouse_count;
static int           mouse_pos;

static keyboard_event_t keyboard_batch[BATCH];
static int              keyboard_count;
static int              keyboard_pos;

int mouse_open(void) {
    return _open("/dev/mouse", O_RDONLY);
}

int mouse_next(int fd, mouse_event_t *ev) {
    if (mouse_pos == mouse_count) {
        long n = _read(fd, mouse_batch, sizeof(mouse_batch));
        if (n <= 0)
            return 0;

        mouse_count = (int)(n / (long)sizeof(mouse_event_t));
        mouse_pos   = 0;
        if (!mouse_count)
            return 0;
    }

    *ev = mouse_batch[mouse_pos++];
    return 1;
}

int keyboard_next(int fd, keyboard_event_t *ev) {
    if (keyboard_pos == keyboard_count) {
        long n = _read(fd, keyboard_batch, sizeof(keyboard_batch));
        if (n <= 0)
            return 0;

        keyboard_count = (int)(n / (long)sizeof(keyboard_event_t));
        keyboard_pos   = 0;
        if (!keyboard_count)
            return 0;
    }

    *ev = keyboard_batch[keyboard_pos++];
    return 1;
}
