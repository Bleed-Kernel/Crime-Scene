#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <syscalls.h>
#include <crime/crime.h>

#define WINDOW_MAX 16
#define QUEUE_MAX  32

#define OPEN_TIMEOUT_MS 3000

static uint64_t    server_pid;
static cs_window_t windows[WINDOW_MAX];

// events that turned up while we were waiting for the reply to an open
static cs_event_t  queue[QUEUE_MAX];
static int         queue_head;
static int         queue_count;

int cs_connect(void) {
    char buf[24] = {0};

    int fd = _open(CS_PID_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = _read(fd, buf, sizeof(buf) - 1);
    _close(fd);
    if (n <= 0)
        return -1;

    server_pid = (uint64_t)strtol(buf, NULL, 10);
    return server_pid ? 0 : -1;
}

int cs_terminal_open(const char *cwd) {
    if (!server_pid && cs_connect() < 0)
        return -1;

    cs_msg_t msg = { .type = CS_REQ_TERMINAL };
    strncpy(msg.title, cwd ? cwd : "", CS_TITLE_MAX - 1);
    strncpy(msg.text, cwd ? cwd : "", CS_TEXT_MAX - 1);
    return cs_msg_send(server_pid, &msg);
}

int cs_run(const char *const argv[]) {
    if (!server_pid && cs_connect() < 0)
        return -1;

    cs_msg_t msg = { .type = CS_REQ_RUN };
    size_t   at  = 0;
    for (int i = 0; argv[i]; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (at + len >= CS_TEXT_MAX)
            return -1;
        memcpy(msg.text + at, argv[i], len);
        at += len;
    }
    return at ? cs_msg_send(server_pid, &msg) : -1;
}

static void window_rebuffer(const cs_msg_t *msg) {
    for (int i = 0; i < WINDOW_MAX; i++) {
        cs_window_t *win = &windows[i];
        if (!win->id || win->id != msg->window)
            continue;

        // most resizes fit in the buffer we already have, only a bigger one needs mapping
        if (msg->shm != win->shm) {
            long pixels = shm_map(msg->shm, NULL);
            if (pixels <= 0)
                return;

            _munmap(win->pixels);
            win->pixels = (uint32_t *)pixels;
            win->shm    = msg->shm;
        }

        win->width  = msg->w;
        win->height = msg->h;
        win->pitch  = msg->pitch;
        win->serial = msg->serial;
        return;
    }
}

// next message from the desktop, anything from anyone else is dropped
static int client_recv(cs_msg_t *msg, int timeout_ms) {
    struct pollfd ipc = { POLL_FD_IPC, POLLIN, 0 };

    for (;;) {
        uint64_t sender;
        while (cs_msg_recv(msg, &sender)) {
            if (sender != server_pid)
                continue;
            if (msg->type == CS_EV_RESIZE)
                window_rebuffer(msg);
            return 1;
        }

        if (poll(&ipc, 1, timeout_ms) == 0)
            return 0;
    }
}

cs_window_t *cs_window_open(int width, int height, const char *title) {
    if (!server_pid && cs_connect() < 0)
        return NULL;

    cs_window_t *win = NULL;
    for (int i = 0; i < WINDOW_MAX; i++) {
        if (!windows[i].id) {
            win = &windows[i];
            break;
        }
    }
    if (!win)
        return NULL;

    cs_msg_t msg = { .type = CS_REQ_OPEN, .w = width, .h = height };
    strncpy(msg.title, title ? title : "", CS_TITLE_MAX - 1);
    if (cs_msg_send(server_pid, &msg) < 0)
        return NULL;

    for (;;) {
        if (!client_recv(&msg, OPEN_TIMEOUT_MS))
            return NULL;
        if (msg.type == CS_EV_OPENED)
            break;

        if (queue_count < QUEUE_MAX)
            queue[(queue_head + queue_count++) % QUEUE_MAX] = msg;
    }
    if (!msg.window)
        return NULL;

    long pixels = shm_map(msg.shm, NULL);
    if (pixels <= 0) {
        msg.type = CS_REQ_CLOSE;
        cs_msg_send(server_pid, &msg);
        return NULL;
    }

    win->id     = msg.window;
    win->width  = msg.w;
    win->height = msg.h;
    win->pitch  = msg.pitch;
    win->pixels = (uint32_t *)pixels;
    win->shm    = msg.shm;
    win->serial = 0;
    return win;
}

void cs_window_close(cs_window_t *win) {
    if (!win || !win->id)
        return;

    cs_msg_t msg = { .type = CS_REQ_CLOSE, .window = win->id };
    cs_msg_send(server_pid, &msg);

    _munmap(win->pixels);
    memset(win, 0, sizeof(*win));
}

void cs_window_commit(cs_window_t *win, int x, int y, int w, int h) {
    cs_msg_t msg = { .type = CS_REQ_COMMIT, .window = win->id, .x = x, .y = y, .w = w, .h = h,
                     .serial = win->serial };
    cs_msg_send(server_pid, &msg);
}

void cs_window_move(cs_window_t *win, int x, int y) {
    cs_msg_t msg = { .type = CS_REQ_MOVE, .window = win->id, .x = x, .y = y };
    cs_msg_send(server_pid, &msg);
}

void cs_window_title(cs_window_t *win, const char *title) {
    cs_msg_t msg = { .type = CS_REQ_TITLE, .window = win->id };
    strncpy(msg.title, title, CS_TITLE_MAX - 1);
    cs_msg_send(server_pid, &msg);
}

void cs_window_resize(cs_window_t *win, int width, int height) {
    cs_msg_t msg = { .type = CS_REQ_RESIZE, .window = win->id, .w = width, .h = height };
    cs_msg_send(server_pid, &msg);
}

void cs_pointer_lock(cs_window_t *win, int on) {
    cs_msg_t msg = { .type = CS_REQ_POINTER_LOCK, .window = win->id, .x = on ? 1 : 0 };
    cs_msg_send(server_pid, &msg);
}

int cs_event_wait(cs_event_t *ev, int timeout_ms) {
    if (queue_count) {
        *ev = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
        return 1;
    }
    return client_recv(ev, timeout_ms);
}
