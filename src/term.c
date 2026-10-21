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
#include <font.h>
#include <taskbar.h>
#include <term.h>
#include <wm.h>

#define SHELL_PATH  "/initrd/bin/verdict"
#define SHELL_WINDOW_ARG "--window-shell"

// taskman stops looking here too
#define PID_SCAN    512

// as many as a task can have open
#define FD_SCAN     256

// how tall an em of the terminal font is, ctrl + and - move it a step at a time and ctrl 0 puts it back
#define TEXT_PX         12
#define TEXT_PX_MIN     8
#define TEXT_PX_MAX     40
#define TEXT_PX_STEP    2
#define TEXT_PAD        3

#define WHEEL_LINES     3

// how big a new terminal is in characters, unless thats more than half the screen
#define TEXT_COLS       80
#define TEXT_ROWS       20

// the sizes the kernel takes for a tty, in pixels either way
#define TTY_PX_MIN      64
#define TTY_PX_MAX      4096

// block elements are drawn by hand so they meet the next cell exactly
#define BLOCK_FIRST     0x2580
#define BLOCK_LAST      0x259F

typedef struct term {
    window_t *win;
    int       tty;
    int       keys;     // write end of the pipe thats the shells stdin
    long      shell;
    char     *path;

    
    int         text;
    int         px;
    font_t      font;
    int         cell_w, cell_h;
    uint32_t    tty_w, tty_h;
    tty_cell_t *cells;
    int         cells_cols, cells_rows;
    int         cols, rows;
    uint32_t    bg;
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

static long shell_start(const term_t *t, int key_read, char *const argv[], const char *cwd) {
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
    if (strcmp(argv[0], SHELL_PATH) == 0 && !argv[1]) {
        char *const shell[] = { argv[0], SHELL_WINDOW_ARG, NULL };
        execv(argv[0], shell);
        _exit(127);
    }
    execv(argv[0], argv);
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

static int text_font(term_t *t, int px) {
    font_t font;
    if (font_open_role(&font, FONT_MONO_ROLE, px) < 0)
        return -1;

    if (t->font.glyphs)
        font_free(&t->font);
    t->font   = font;
    t->px     = px;
    t->cell_w = font.glyphs['M' - FONT_FIRST].advance;
    t->cell_h = font_height(&font);
    return t->cell_w > 0 && t->cell_h > 0 ? 0 : -1;
}

static int text_room(term_t *t, int cols, int rows) {
    if (cols <= t->cells_cols && rows <= t->cells_rows)
        return 0;

    if (cols < t->cells_cols) cols = t->cells_cols;
    if (rows < t->cells_rows) rows = t->cells_rows;
    tty_cell_t *cells = calloc((size_t)cols * rows, sizeof(tty_cell_t));
    if (!cells)
        return -1;

    free(t->cells);
    t->cells      = cells;
    t->cells_cols = cols;
    t->cells_rows = rows;
    return 0;
}

// whatever rows of text changed since last time into cells, top to bottom says which
static int text_grab(term_t *t, int *top, int *bottom) {
    if (text_room(t, 1, 1) < 0)
        return -1;

    for (int tries = 0; tries < 2; tries++) {
        tty_text_grab_t grab = {
            .cells    = (uint64_t)(uintptr_t)t->cells,
            .stride   = (uint32_t)t->cells_cols,
            .max_rows = (uint32_t)t->cells_rows,
        };
        if (_ioctl(t->tty, TTY_IOCTL_GRAB_TEXT, &grab) < 0)
            return -1;

        t->tty_w = grab.cell_w;
        t->tty_h = grab.cell_h;
        t->bg    = grab.bg & 0xFFFFFF;

        if ((int)grab.cols > t->cells_cols || (int)grab.rows > t->cells_rows) {
            if (text_room(t, (int)grab.cols, (int)grab.rows) < 0)
                return -1;
            continue;
        }

        t->cols = (int)grab.cols;
        t->rows = (int)grab.rows;
        *top    = (int)grab.top;
        *bottom = (int)grab.bottom;
        return 0;
    }
    *top = *bottom = 0;
    return 0;
}

// as many of our characters as fit the window
static void text_fit(term_t *t) {
    if (!t->tty_w || !t->tty_h)
        return;

    int cols = (t->win->w - 2 * TEXT_PAD) / t->cell_w;
    int rows = (t->win->h - 2 * TEXT_PAD) / t->cell_h;
    int min_cols = (TTY_PX_MIN + (int)t->tty_w - 1) / (int)t->tty_w;
    int min_rows = (TTY_PX_MIN + (int)t->tty_h - 1) / (int)t->tty_h;
    if (cols < min_cols) cols = min_cols;
    if (rows < min_rows) rows = min_rows;
    if (cols > TTY_PX_MAX / (int)t->tty_w) cols = TTY_PX_MAX / (int)t->tty_w;
    if (rows > TTY_PX_MAX / (int)t->tty_h) rows = TTY_PX_MAX / (int)t->tty_h;

    tty_create_t size = { .width = (uint32_t)cols * t->tty_w, .height = (uint32_t)rows * t->tty_h };
    _ioctl(t->tty, TTY_IOCTL_RESIZE, &size);
}

// fg over bg by alpha out of 4, for the shades
static uint32_t shade(uint32_t fg, uint32_t bg, int alpha) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t f = (fg >> shift) & 0xFF;
        uint32_t b = (bg >> shift) & 0xFF;
        out |= ((f * alpha + b * (4 - alpha)) / 4) << shift;
    }
    return out;
}

static const uint8_t quadrants[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };

// one of BLOCK_FIRST to BLOCK_LAST over cell, which already has the background in it
static void block_draw(display_t *view, rect_t cell, uint32_t cp, uint32_t fg, uint32_t bg) {
    int w = cell.w, h = cell.h;
    int x = cell.x, y = cell.y;

    if (cp == 0x2580) {                         // upper half
        display_fill(view, (rect_t){x, y, w, h / 2}, fg);
    } else if (cp <= 0x2588) {                  // lower eighths up to the full block
        int fill = h * (int)(cp - 0x2580) / 8;
        display_fill(view, (rect_t){x, y + h - fill, w, fill}, fg);
    } else if (cp <= 0x258F) {                  // left seven eighths down to one
        int fill = w * (int)(0x2590 - cp) / 8;
        display_fill(view, (rect_t){x, y, fill, h}, fg);
    } else if (cp == 0x2590) {                  // right half
        display_fill(view, (rect_t){x + w / 2, y, w - w / 2, h}, fg);
    } else if (cp <= 0x2593) {                  // light, medium and dark shade
        display_fill(view, cell, shade(fg, bg, (int)(cp - 0x2590)));
    } else if (cp == 0x2594) {                  // upper eighth
        display_fill(view, (rect_t){x, y, w, h / 8 ? h / 8 : 1}, fg);
    } else if (cp == 0x2595) {                  // right eighth
        int fill = w / 8 ? w / 8 : 1;
        display_fill(view, (rect_t){x + w - fill, y, fill, h}, fg);
    } else {
        uint8_t q  = quadrants[cp - 0x2596];
        int     hw = w / 2, hh = h / 2;
        if (q & 1) display_fill(view, (rect_t){x,      y,      hw,     hh},     fg);
        if (q & 2) display_fill(view, (rect_t){x + hw, y,      w - hw, hh},     fg);
        if (q & 4) display_fill(view, (rect_t){x,      y + hh, hw,     h - hh}, fg);
        if (q & 8) display_fill(view, (rect_t){x + hw, y + hh, w - hw, h - hh}, fg);
    }
}

static void text_paint(display_t *d, term_t *t, int top, int bottom, int whole) {
    window_t *win  = t->win;
    display_t view = { .back = win->pixels, .width = win->w, .height = win->h, .pitch = win->cap_w };

    if (whole) {
        display_fill(&view, (rect_t){0, 0, win->w, win->h}, t->bg);
        top    = 0;
        bottom = t->rows;
    }

    for (int row = top; row < bottom; row++) {
        rect_t band = { 0, TEXT_PAD + row * t->cell_h, win->w, t->cell_h };
        if (!whole)
            display_fill(&view, band, t->bg);

        const tty_cell_t *cells = t->cells + (size_t)row * t->cells_cols;
        for (int col = 0; col < t->cols; col++) {
            uint32_t cp = cells[col].cp;
            if (!cp)
                continue;

            rect_t   cell = { TEXT_PAD + col * t->cell_w, band.y, t->cell_w, t->cell_h };
            uint32_t fg   = cells[col].fg & 0xFFFFFF;
            uint32_t bg   = cells[col].bg & 0xFFFFFF;
            display_fill(&view, cell, bg);

            if (cp >= BLOCK_FIRST && cp <= BLOCK_LAST) {
                block_draw(&view, cell, cp, fg, bg);
                continue;
            }
            if (cp == ' ')
                continue;

            // the band and not the cell
            font_draw_cp(&view, band, &t->font, cell.x, cell.y, cp, fg);
        }
    }

    rect_t r = whole ? (rect_t){ win->x, win->y, win->w, win->h }
                     : (rect_t){ win->x, win->y + TEXT_PAD + top * t->cell_h, win->w, (bottom - top) * t->cell_h };
    display_damage(d, rect_clip(r, window_content(win)));
}

// the window or the font changed size, so the whole thing is laid out and drawn again
static void text_refit(display_t *d, term_t *t) {
    int top, bottom;
    text_fit(t);
    if (text_grab(t, &top, &bottom) == 0)
        text_paint(d, t, 0, 0, 1);
}

static void text_zoom(display_t *d, term_t *t, int px) {
    if (px < TEXT_PX_MIN) px = TEXT_PX_MIN;
    if (px > TEXT_PX_MAX) px = TEXT_PX_MAX;
    if (px == t->px || text_font(t, px) < 0)
        return;
    text_refit(d, t);
}

static int text_key(display_t *d, term_t *t, const keyboard_event_t *ev) {
    if (!t->text || !(ev->keymod & KEYMOD_CTRL))
        return 0;

    char c = tty_key_to_ascii(ev);
    int  px;
    if (c == '=' || c == '+')
        px = t->px + TEXT_PX_STEP;
    else if (c == '-' || c == '_')
        px = t->px - TEXT_PX_STEP;
    else if (c == '0')
        px = TEXT_PX;
    else
        return 0;

    if (ev->action == KEY_DOWN)
        text_zoom(d, t, px);
    return 1;
}

static void text_free(term_t *t) {
    if (t->font.glyphs)
        font_free(&t->font);
    free(t->cells);
    t->cells = NULL;
    t->text  = 0;
}

// TEXT_COLS by TEXT_ROWS
static void text_size(const display_t *d, const term_t *t, int *w, int *h) {
    int max_w = d->width / 2 - WINDOW_INSET * 2;
    int max_h = (d->height - TASKBAR_H) / 2 - WINDOW_INSET_TOP - WINDOW_INSET;

    *w = TEXT_COLS * t->cell_w + 2 * TEXT_PAD;
    *h = TEXT_ROWS * t->cell_h + 2 * TEXT_PAD;
    if (*w > max_w) *w = max_w;
    if (*h > max_h) *h = max_h;
}

static void text_start(display_t *d, term_t *t) {
    int top, bottom;
    if (!t->font.glyphs || text_grab(t, &top, &bottom) < 0) {
        text_free(t);
        return;
    }
    t->text = 1;
    text_refit(d, t);
}

static void key(display_t *d, term_t *t, const cs_msg_t *ev) {
    keyboard_event_t out = {
        .keycode = ev->keycode,
        .action  = ev->key_action,
        .keymod  = ev->keymod,
    };

    if (text_key(d, t, &out))
        return;

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
static void redraw(display_t *d, term_t *t) {
    if (t->text) {
        int top, bottom;
        if (text_grab(t, &top, &bottom) == 0 && top < bottom)
            text_paint(d, t, top, bottom, 0);
        return;
    }

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
    text_free(t);
    free(t->path);

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

static int term_start(display_t *d, char *const argv[], const char *title, const char *cwd) {
    const char *path = argv[0];

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

    // with the mono font the window is sized in characters
    int w = CS_SIZE_DEFAULT, h = CS_SIZE_DEFAULT;
    if (text_font(t, TEXT_PX) == 0)
        text_size(d, t, &w, &h);

    t->win = window_open(d, (uint64_t)_getpid(), w, h, title);
    if (!t->win) {
        text_free(t);
        free(t);
        return -1;
    }
    // the desktop owns the window, its icon is the program running in it
    t->win->icon = icon_for_path(path);
    t->path      = strdup(path);

    int pipefd[2] = { -1, -1 };
    t->tty = tty_open(t->win->w, t->win->h);
    if (t->tty < 0 || _pipe(pipefd) < 0) {
        if (t->tty >= 0) {
            _ioctl(t->tty, TTY_IOCTL_DESTROY, NULL);
            _close(t->tty);
        }
        window_close(d, t->win);
        free(t->path);
        free(t);
        return -1;
    }
    t->keys = pipefd[1];

    text_start(d, t);

    t->shell = shell_start(t, pipefd[0], argv, cwd);
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
    char *const argv[] = { SHELL_PATH, NULL };
    return term_start(d, argv, "Verdict", cwd);
}

int term_run(display_t *d, char *const argv[], const char *title) {
    return term_start(d, argv, title, NULL);
}

void term_event(display_t *d, window_t *win, const cs_msg_t *msg) {
    term_t *t = win->term;

    switch (msg->type) {
        case CS_EV_KEY:
            key(d, t, msg);
            break;
        case CS_EV_CLOSE_REQUEST:
            term_close(d, t, 1);
            break;
        // the tty keeps whats on it and marks every row changed, the redraw fills the new buffer
        case CS_EV_RESIZE: {
            win->resize_acked = msg->serial;
            if (t->text) {
                text_refit(d, t);
                break;
            }
            tty_create_t size = { .width = (uint32_t)win->w, .height = (uint32_t)win->h };
            _ioctl(t->tty, TTY_IOCTL_RESIZE, &size);
            redraw(d, t);
            break;
        }
        case CS_EV_WHEEL: {
            int lines = -msg->wheel * WHEEL_LINES;
            _ioctl(t->tty, TTY_IOCTL_SCROLL, &lines);
            redraw(d, t);
            break;
        }
        // the shell only blinks its cursor on the tty thats being looked at
        case CS_EV_FOCUS:
            _ioctl(t->tty, TTY_IOCTL_SET_ACTIVE, (void *)(uintptr_t)msg->focused);
            break;
    }
}

void term_kill(display_t *d, window_t *win) {
    term_close(d, win->term, 1);
}

const char *term_path(const window_t *win) {
    return win->term->path;
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
