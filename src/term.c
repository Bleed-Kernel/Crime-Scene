#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <syscalls.h>
#include <scheduler.h>
#include <keyboard.h>
#include <console.h>
#include <crime/crime.h>
#include <display.h>
#include <term.h>
#include <wm.h>

#define SHELL_PATH  "/initrd/bin/verdict"
#define SHELL_WINDOW_ARG "--window-shell"

// taskman stops looking here too
#define PID_SCAN    512

// as many as a task can have open
#define FD_SCAN     256

typedef struct term {
    window_t *win;
    int       tty;
    int       keys;     // write end of the pipe thats the shells stdin
    long      shell;
} term_t;

static term_t **terms;
static int      terms_count;
static int      terms_cap;

// a tty of our own that draws into memory instead of the screen
static int tty_open(int width, int height) {
    tty_create_t create = { .width = (uint32_t)width, .height = (uint32_t)height };

    int tty0 = _open("/dev/tty0", O_RDWR);
    long made = _ioctl(tty0, TTY_IOCTL_CREATE, &create);
    _close(tty0);
    if (made < 0)
        return -1;

    char path[24];
    snprintf(path, sizeof(path), "/dev/tty%u", create.index);
    return _open(path, O_RDWR);
}

static long shell_start(const term_t *t, int key_read, const char *path, const char *cwd) {
    long pid = fork();
    if (pid != 0)
        return pid;

    // key events in on stdin the way the keyboard device gives them, the tty for everything out
    dup2(key_read, 0);
    dup2(t->tty, 1);
    dup2(t->tty, 2);

    // the rest is the desktops, every other terminal included
    for (int fd = 3; fd < FD_SCAN; fd++)
        _close(fd);

    if (cwd && cwd[0])
        chdir(cwd);

    // the arg tells the shell this window is its own, without it it would go and ask for another
    int   shell = strcmp(path, SHELL_PATH) == 0;
    char *const argv[] = { (char *)path, shell ? SHELL_WINDOW_ARG : NULL, NULL };
    execv(path, argv);
    _exit(127);
    return -1;
}

static int shell_alive(const term_t *t) {
    user_task_info_t info;
    if (_taskinfo((uint64_t)t->shell, &info) != 0)
        return 0;
    if (info.state == TASK_ZOMBIE) {
        _waitpid(t->shell);
        return 0;
    }
    return info.state != TASK_DEAD && info.state != TASK_FREE;
}

// who started who, for everything running. 0 where theres no task
static uint64_t parents[PID_SCAN];
static uint8_t  running[PID_SCAN];

static void tasks_scan(void) {
    user_task_info_t info;

    for (uint64_t pid = 0; pid < PID_SCAN; pid++) {
        running[pid] = 0;
        if (_taskinfo(pid, &info) != 0)
            continue;
        if (info.state == TASK_ZOMBIE || info.state == TASK_DEAD || info.state == TASK_FREE)
            continue;
        running[pid] = 1;
        parents[pid] = info.ppid;
    }
}

static int under_shell(const term_t *t, uint64_t pid) {
    for (int hops = 0; hops < PID_SCAN && pid && pid < PID_SCAN && running[pid]; hops++) {
        pid = parents[pid];
        if (pid == (uint64_t)t->shell)
            return 1;
    }
    return 0;
}

static void interrupt(const term_t *t) {
    tasks_scan();
    for (uint64_t pid = PID_SCAN - 1; pid > 0; pid--) {
        if (running[pid] && under_shell(t, pid)) {
            _kill((long)pid, SIGINT);
            return;
        }
    }
}

static void shell_kill(const term_t *t) {
    tasks_scan();
    for (uint64_t pid = PID_SCAN - 1; pid > 0; pid--) {
        if (running[pid] && under_shell(t, pid))
            _kill((long)pid, SIGKILL);
    }
    _kill(t->shell, SIGKILL);
    _waitpid(t->shell);
}

static void key(const term_t *t, const cs_msg_t *ev) {
    keyboard_event_t out = {
        .keycode = ev->keycode,
        .action  = ev->key_action,
        .keymod  = ev->keymod,
    };

    if (out.action == KEY_DOWN && (out.keymod & KEYMOD_CTRL)) {
        char c = tty_key_to_ascii(&out);
        if (c == 'c' || c == 'C')
            interrupt(t);
    }

    // nothing is reading, drop it rather than hang the whole desktop until something does
    struct pollfd room = { t->keys, POLLOUT, 0 };
    if (poll(&room, 1, 0) != 1 || !(room.revents & POLLOUT))
        return;
    _write(t->keys, &out, sizeof(out));
}

// pull whatever the tty drew since last time straight into the window
static void redraw(display_t *d, const term_t *t) {
    window_t  *win  = t->win;
    tty_grab_t grab = { .buffer = (uint64_t)(uintptr_t)win->pixels, .pitch = (uint32_t)win->cap_w };

    if (_ioctl(t->tty, TTY_IOCTL_GRAB, &grab) < 0 || grab.bottom <= grab.top)
        return;

    rect_t r = { win->x, win->y + (int)grab.top, win->w, (int)(grab.bottom - grab.top) };
    display_damage(d, rect_clip(r, window_content(win)));
}

static void term_close(display_t *d, term_t *t, int kill) {
    if (kill)
        shell_kill(t);

    _close(t->keys);
    _ioctl(t->tty, TTY_IOCTL_DESTROY, NULL);
    _close(t->tty);

    for (int i = 0; i < terms_count; i++) {
        if (terms[i] != t)
            continue;
        for (; i < terms_count - 1; i++)
            terms[i] = terms[i + 1];
        terms_count--;
    }

    wm_forget(t->win);
    window_close(d, t->win);
    free(t);
}

static int term_start(display_t *d, const char *path, const char *title, const char *cwd) {
    if (terms_count == terms_cap) {
        int      cap    = terms_cap ? terms_cap * 2 : 16;
        term_t **bigger = realloc(terms, (size_t)cap * sizeof(term_t *));
        if (!bigger)
            return -1;
        terms     = bigger;
        terms_cap = cap;
    }

    term_t *t = calloc(1, sizeof(term_t));
    if (!t)
        return -1;

    t->win = window_open(d, (uint64_t)_getpid(), CS_SIZE_DEFAULT, CS_SIZE_DEFAULT, title);
    if (!t->win) {
        free(t);
        return -1;
    }

    int pipefd[2] = { -1, -1 };
    t->tty = tty_open(t->win->w, t->win->h);
    if (t->tty < 0 || _pipe(pipefd) < 0) {
        if (t->tty >= 0) {
            _ioctl(t->tty, TTY_IOCTL_DESTROY, NULL);
            _close(t->tty);
        }
        window_close(d, t->win);
        free(t);
        return -1;
    }
    t->keys = pipefd[1];

    t->shell = shell_start(t, pipefd[0], path, cwd);
    _close(pipefd[0]);

    t->win->term = t;
    terms[terms_count++] = t;

    if (t->shell < 0) {
        term_close(d, t, 0);
        return -1;
    }
    return 0;
}

int term_open(display_t *d, const char *cwd) {
    return term_start(d, SHELL_PATH, "Verdict", cwd);
}

int term_run(display_t *d, const char *path, const char *title) {
    return term_start(d, path, title, NULL);
}

void term_event(display_t *d, window_t *win, const cs_msg_t *msg) {
    term_t *t = win->term;

    switch (msg->type) {
        case CS_EV_KEY:
            key(t, msg);
            break;
        case CS_EV_CLOSE_REQUEST:
            term_close(d, t, 1);
            break;
        // the tty keeps whats on it and marks every row changed, the redraw fills the new buffer
        case CS_EV_RESIZE: {
            tty_create_t size = { .width = (uint32_t)win->w, .height = (uint32_t)win->h };
            _ioctl(t->tty, TTY_IOCTL_RESIZE, &size);
            win->resize_acked = msg->serial;
            redraw(d, t);
            break;
        }
        // the shell only blinks its cursor on the tty thats being looked at
        case CS_EV_FOCUS:
            _ioctl(t->tty, TTY_IOCTL_SET_ACTIVE, (void *)(uintptr_t)msg->focused);
            break;
    }
}

int term_count(void) {
    return terms_count;
}

void term_pollfds(struct pollfd *fds, int count) {
    for (int i = 0; i < count; i++)
        fds[i] = (struct pollfd){ terms[i]->tty, POLLPRI, 0 };
}

void term_service(display_t *d, const struct pollfd *fds, int count) {
    for (int i = 0; i < count && i < terms_count; i++) {
        if (fds[i].revents & POLLPRI)
            redraw(d, terms[i]);
    }
}

void term_sweep(display_t *d) {
    for (int i = 0; i < terms_count; ) {
        if (shell_alive(terms[i]))
            i++;
        else
            term_close(d, terms[i], 0);
    }
}

void term_shutdown(display_t *d) {
    while (terms_count)
        term_close(d, terms[0], 1);
}
