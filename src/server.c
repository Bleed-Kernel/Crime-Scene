#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <syscalls.h>
#include <scheduler.h>
#include <server.h>
#include <wm.h>
#include <term.h>
#include <notify.h>
#include <taskbar.h>
#include <apps.h>
#include <crime/crime.h>

int server_init(void) {
    char buf[24];
    int  len = snprintf(buf, sizeof(buf), "%ld\n", _getpid());

    _mkdir("/tmp", 0755);
    int fd = _open(CS_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;

    long written = _write(fd, buf, len);
    _close(fd);
    return written == len ? 0 : -1;
}

static void window_drop(display_t *d, window_t *win) {
    wm_forget(win);
    window_close(d, win);
}

static void client_drop(display_t *d, uint64_t pid) {
    for (int i = 0; window_index(i); ) {
        window_t *win = window_index(i);
        if (win->owner == pid)
            window_drop(d, win);
        else
            i++;
    }
}

static int client_alive(uint64_t pid) {
    user_task_info_t info;
    if (_taskinfo(pid, &info) != 0)
        return 0;

    // our own children hang about as zombies until someone waits for them
    if (info.state == TASK_ZOMBIE && info.ppid == (uint64_t)_getpid())
        _waitpid((long)pid);

    return info.state != TASK_ZOMBIE && info.state != TASK_DEAD && info.state != TASK_FREE;
}

// a client that goes with its windows still open didnt close them itself, it crashed or was killed?
void server_sweep(display_t *d) {
    for (int i = 0; window_index(i); ) {
        uint64_t pid = window_index(i)->owner;
        if (client_alive(pid)) {
            i++;
            continue;
        }

        char text[96];
        snprintf(text, sizeof(text), "%.60s stopped without closing.", window_index(i)->title);
        client_drop(d, pid);
        notify_post(d, "Program ended", text);
    }
}

void server_send(display_t *d, window_t *win, cs_msg_t *msg) {
    msg->window = win->id;
    if (win->term) {
        term_event(d, win, msg);
        return;
    }
    if (cs_msg_send(win->owner, msg) < 0)
        client_drop(d, win->owner);
}

void server_resize(display_t *d, window_t *win, int x, int y, int w, int h) {
    if (window_resize(d, win, x, y, w, h) < 0)
        return;

    cs_msg_t msg = { .type = CS_EV_RESIZE, .w = win->w, .h = win->h, .pitch = win->cap_w,
                     .shm = win->shm, .serial = ++win->resize_serial };
    server_send(d, win, &msg);
}

void server_maximise(display_t *d, window_t *win) {
    rect_t to;

    if (win->maximised) {
        to = win->restore;
    } else {
        // a snapped window already has where it came from kept, thats where it goes back to
        if (!win->snapped)
            win->restore = window_content(win);
        to = window_maximised_rect(d);
    }
    win->snapped = 0;

    win->maximised = !win->maximised;
    server_resize(d, win, to.x, to.y, to.w, to.h);
}

void server_kill(display_t *d, window_t *win) {
    if (win->term) {
        term_kill(d, win);
        return;
    }

    // the desktop doesnt kill itself
    uint64_t pid = win->owner;
    if (pid == (uint64_t)_getpid())
        return;

    _kill((long)pid, SIGKILL);
    client_drop(d, pid);
    client_alive(pid);
}

static uint32_t focus_told;     // the window that was last told it has the focus

void server_focus(display_t *d) {
    window_t *top = window_focused();
    uint32_t  now = top ? top->id : 0;
    if (now == focus_told)
        return;

    // the old one might have closed, then theres nobody to tell
    window_t *old = window_find(focus_told);
    focus_told = now;

    if (old) {
        cs_msg_t msg = { .type = CS_EV_FOCUS, .focused = 0 };
        server_send(d, old, &msg);
    }

    // sending to old can take windows away with it if its client turned out to be dead
    top = window_find(now);
    if (top) {
        cs_msg_t msg = { .type = CS_EV_FOCUS, .focused = 1 };
        server_send(d, top, &msg);
    }
}

static void request_open(display_t *d, uint64_t sender, const cs_msg_t *req) {
    cs_msg_t reply = { .type = CS_EV_OPENED };

    char title[CS_TITLE_MAX];
    memcpy(title, req->title, CS_TITLE_MAX);
    title[CS_TITLE_MAX - 1] = '\0';

    window_t *win = window_open(d, sender, req->w, req->h, title);
    if (win) {
        reply.window = win->id;
        reply.shm    = win->shm;
        reply.w      = win->w;
        reply.h      = win->h;
        reply.pitch  = win->cap_w;
    }

    if (cs_msg_send(sender, &reply) < 0 && win)
        window_drop(d, win);
}

// whatever window is under where it was let go gets it
static void request_drop(display_t *d, window_t *from, cs_msg_t *req) {
    int       x  = from->x + req->x, y = from->y + req->y;
    window_t *to = window_at(x, y);
    if (!to || to == from || to->minimised || !rect_contains(window_content(to), x, y))
        return;

    req->type   = CS_EV_DROP;
    req->window = to->id;
    req->x      = x - to->x;
    req->y      = y - to->y;
    req->text[CS_TEXT_MAX - 1] = '\0';
    server_send(d, to, req);
}

void server_requests(display_t *d) {
    cs_msg_t req;
    uint64_t sender;

    while (cs_msg_recv(&req, &sender)) {
        if (req.type == CS_REQ_OPEN) {
            request_open(d, sender, &req);
            continue;
        }
        if (req.type == CS_REQ_TERMINAL) {
            // text has room for a long path
            req.title[CS_TITLE_MAX - 1] = '\0';
            req.text[CS_TEXT_MAX - 1]   = '\0';
            term_open(d, req.text[0] ? req.text : req.title);
            continue;
        }

        // the args are back to back, an empty one (two nuls) is the end
        if (req.type == CS_REQ_RUN) {
            char *argv[RUN_ARGS_MAX + 1];
            int   argc = 0;
            req.text[CS_TEXT_MAX - 1] = '\0';
            for (char *at = req.text; *at && argc < RUN_ARGS_MAX; at += strlen(at) + 1)
                argv[argc++] = at;
            argv[argc] = NULL;
            if (argc)
                app_run(d, argv);
            continue;
        }

        // everything else is about a window, and only its owner gets a say
        window_t *win = window_find(req.window);
        if (!win || win->owner != sender)
            continue;

        switch (req.type) {
            case CS_REQ_CLOSE:
                window_drop(d, win);
                break;
            case CS_REQ_COMMIT: {
                win->resize_acked = req.serial;

                rect_t r = { win->x + req.x, win->y + req.y, req.w, req.h };
                display_damage(d, rect_clip(r, window_content(win)));
                break;
            }
            case CS_REQ_MOVE:
                window_move(d, win, req.x, req.y);
                break;
            // a window filling the screen stays that way, what it wants only matters once its restored
            case CS_REQ_RESIZE:
                if (win->maximised) {
                    win->restore.w = req.w;
                    win->restore.h = req.h;
                } else {
                    server_resize(d, win, win->x, win->y, req.w, req.h);
                }
                break;
            case CS_REQ_TITLE:
                memcpy(win->title, req.title, CS_TITLE_MAX);
                win->title[CS_TITLE_MAX - 1] = '\0';
                if (!win->minimised)
                    display_damage(d, window_titlebar(win));
                display_damage(d, taskbar_rect(d));
                break;
            // asking again after giving it up counts as a fresh ask, wm_tick does the rest
            case CS_REQ_POINTER_LOCK:
                win->wants_lock  = req.x ? 1 : 0;
                win->lock_broken = 0;
                break;
            case CS_REQ_DROP:
                request_drop(d, win, &req);
                break;
        }
    }
}

void server_shutdown(display_t *d) {
    term_shutdown(d);

    // nothing left for the clients to talk to, take them down with us
    while (window_top()) {
        uint64_t pid = window_top()->owner;
        client_drop(d, pid);
        _kill((long)pid, SIGKILL);
        client_alive(pid);
    }
    _unlink(CS_PID_FILE);
}
