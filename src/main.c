#include <stdio.h>
#include <display.h>
#include <cursor.h>
#include <input.h>
#include <clock.h>
#include <font.h>
#include <window.h>
#include <wm.h>
#include <server.h>
#include <term.h>
#include <taskbar.h>
#include <wallpaper.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crime/crime.h>
#include <fcntl.h>
#include <syscalls.h>
#include <scheduler.h>
#include <crime/sys.h>

// nothing reaches the screen more often than this
#define FRAME_FS (1000 * FS_PER_MS / 120)

#define FDS_FIXED 3

#define SWEEP_MS 1000
#define SWEEP_FS (SWEEP_MS * FS_PER_MS)

// shells that have exited get noticed this often
#define TERM_CHECK_MS 250
#define TERM_CHECK_FS (TERM_CHECK_MS * FS_PER_MS)

// backspace and tab, as the scancodes the keyboard driver hands us
#define KEY_BACKSPACE 0x0E
#define KEY_TAB       0x0F

// the windows keys, left and right. both keyboard drivers hand them over as these
#define KEY_META_L    0x5B
#define KEY_META_R    0x5C

static display_t display;
static cursor_t  cursor;
static rect_t         cursor_shown;
static cursor_shape_t cursor_shape_shown;
static int            cursor_hidden_shown;

static int            meta_down;
static int            ctrl_down;

static int cursor_changed(void) {
    rect_t now = cursor_rect(&cursor);
    return now.x != cursor_shown.x || now.y != cursor_shown.y || cursor.shape != cursor_shape_shown ||
           wm_locked() != cursor_hidden_shown;
}

// redraw everything thats damaged from the bottom up, then put it all on screen in one go
static void frame(void) {
    wm_frame(&display);

    if (cursor_changed()) {
        display_damage(&display, cursor_shown);
        display_damage(&display, cursor_rect(&cursor));
        cursor_shown        = cursor_rect(&cursor);
        cursor_shape_shown  = cursor.shape;
        cursor_hidden_shown = wm_locked();
    }

    // the taskbar is solid and on top, so windows only get drawn in whats above it

    rect_t bar  = taskbar_rect(&display);
    rect_t work = {0, 0, display.width, display.height - bar.h};

    for (int i = 0; i < display.damage_count; i++) {
        windows_compose(&display, rect_clip(display.damage[i], work));
        taskbar_draw(&display, display.damage[i]);
        if (!cursor_hidden_shown)
            cursor_draw(&cursor, &display, display.damage[i]);
    }
    display_present(&display);
}

// a mac trackpad has the one button, there ctrl and a click is the right button
static uint8_t ctrl_click(uint8_t buttons) {
    static uint8_t last;
    static int     as_right;

    if (!(buttons & MOUSE_BTN_LEFT))
        as_right = 0;
    else if (!(last & MOUSE_BTN_LEFT) && ctrl_down)
        as_right = 1;
    last = buttons;

    if (as_right)
        buttons = (uint8_t)((buttons & ~MOUSE_BTN_LEFT) | MOUSE_BTN_RIGHT);
    return buttons;
}

// returns 1 if its time to quit
static int input(int mouse) {
    mouse_event_t    mev;
    keyboard_event_t kev;

    while (mouse_next(mouse, &mev)) {
        if (wm_locked()) {
            wm_mouse_relative(&display, mev.dx, mev.dy, mev.buttons);
            continue;
        }
        cursor_move(&cursor, &display, mev.dx, mev.dy);
        wm_mouse(&display, cursor.x, cursor.y, ctrl_click(mev.buttons));
    }
    cursor.shape = wm_cursor();

    while (keyboard_next(0, &kev)) {
        ctrl_down = kev.keymod & KEYMOD_CTRL;

        // ctrl alt backspace is ours, everything else belongs to the window with the focus
        if (kev.action == KEY_DOWN && kev.keycode == KEY_BACKSPACE &&
            (kev.keymod & KEYMOD_CTRL) && (kev.keymod & KEYMOD_ALT))
            return 1;

        // alt tab is ours too. its the way out of a window thats holding the pointer
        if (kev.keycode == KEY_TAB && (kev.keymod & KEYMOD_ALT)) {
            if (kev.action == KEY_DOWN)
                wm_switch(&display);
            continue;
        }
        // the windows key is the start menu. once per press, a held key repeats
        if (kev.keycode == KEY_META_L || kev.keycode == KEY_META_R) {
            if (kev.action == KEY_DOWN && !meta_down)
                taskbar_menu_toggle(&display);
            meta_down = (kev.action == KEY_DOWN);
            continue;
        }
        wm_key(&display, &kev);
    }
    return 0;
}

// the pid file can outlive a desktop that crashed, and its pid can be handed out again
static int desktop_running(void) {
    char buf[24] = {0};

    int fd = _open(CS_PID_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    long n = _read(fd, buf, sizeof(buf) - 1);
    _close(fd);
    if (n <= 0)
        return 0;

    long pid = strtol(buf, NULL, 10);
    if (pid <= 0 || pid == _getpid())
        return 0;

    user_task_info_t info;
    if (_taskinfo((uint64_t)pid, &info) != 0)
        return 0;
    if (info.state == TASK_ZOMBIE || info.state == TASK_DEAD || info.state == TASK_FREE)
        return 0;
    return strstr(info.name, "crime-scene") != NULL;
}

int main(void) {
    // theres only ever one desktop. run again while its up and all we do is ask it for another
    if (desktop_running()) {
        char cwd[CS_TITLE_MAX] = {0};
        if (!getcwd(cwd, sizeof(cwd)))
            cwd[0] = '\0';
        return cs_terminal_open(cwd) < 0 ? 1 : 0;
    }

    int mouse = mouse_open();
    if (mouse < 0) {
        printf("crime-scene: no mouse\n");
        return 1;
    }
    if (font_init() < 0) {
        printf("crime-scene: no font ending in " FONT_UI_ROLE " in " FONT_DIR "\n");
        return 1;
    }
    if (server_init() < 0) {
        printf("crime-scene: couldnt write " CS_PID_FILE "\n");
        return 1;
    }
    if (display_init(&display) < 0) {
        printf("crime-scene: couldnt get the screen\n");
        return 1;
    }

    wallpaper_init(&display);
    taskbar_update(&display);

    cursor.x = display.width / 2;
    cursor.y = display.height / 2;
    cursor_shown = cursor_rect(&cursor);

    display_damage(&display, (rect_t){0, 0, display.width, display.height});
    frame();

    // the mouse, the keyboard, the clients, then a tty for every terminal there is
    int            fds_cap = FDS_FIXED + 16;
    struct pollfd *fds     = malloc((size_t)fds_cap * sizeof(struct pollfd));
    if (!fds) {
        server_shutdown(&display);
        display_shutdown(&display);
        return 1;
    }

    uint64_t last_frame = clock_now();
    uint64_t last_sweep = clock_now();
    uint64_t last_terms = clock_now();

    for (;;) {
        int timeout = SWEEP_MS;

        if (display.damage_count || cursor_changed()) {
            uint64_t now   = clock_now();
            uint64_t since = now - last_frame;
            // half a millisecond early beats a whole one late, the wait below only comes in whole ones
            if (since + FS_PER_MS / 2 >= FRAME_FS) {
                frame();
                last_frame = since < FRAME_FS * 2 ? last_frame + FRAME_FS : now;
            } else {
                timeout = (int)((FRAME_FS - since + FS_PER_MS / 2) / FS_PER_MS);
                if (timeout < 1) timeout = 1;
            }
        }

        if (clock_now() - last_sweep >= SWEEP_FS) {
            server_sweep(&display);
            server_focus(&display);
            taskbar_update(&display);
            last_sweep = clock_now();
        }

        // a resize waiting on its client gets looked at again soon, with or without any input
        wm_tick(&display);
        if (wm_busy() && timeout > 10)
            timeout = 10;

        int terms = term_count();
        if (FDS_FIXED + terms > fds_cap) {
            int            cap    = (FDS_FIXED + terms) * 2;
            struct pollfd *bigger = realloc(fds, (size_t)cap * sizeof(struct pollfd));
            if (bigger) {
                fds     = bigger;
                fds_cap = cap;
            } else {
                terms = fds_cap - FDS_FIXED;
            }
        }

        fds[0] = (struct pollfd){ mouse,       POLLIN, 0 };
        fds[1] = (struct pollfd){ 0,           POLLIN, 0 };     // stdin is the keyboard
        fds[2] = (struct pollfd){ POLL_FD_IPC, POLLIN, 0 };
        term_pollfds(fds + FDS_FIXED, terms);
        if (terms && timeout > TERM_CHECK_MS)
            timeout = TERM_CHECK_MS;

        long ready = poll(fds, (unsigned long)(FDS_FIXED + terms), timeout);
        if (ready > 0)
            term_service(&display, fds + FDS_FIXED, terms);

        if (clock_now() - last_terms >= TERM_CHECK_FS) {
            term_sweep(&display);
            last_terms = clock_now();
        }
        if (ready <= 0)
            continue;

        if (input(mouse))
            break;
        server_requests(&display);
        server_focus(&display);
        taskbar_update(&display);
    }

    server_shutdown(&display);
    display_shutdown(&display);
    return 0;
}
