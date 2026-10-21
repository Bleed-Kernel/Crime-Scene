#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscalls.h>
#include <mouse.h>
#include <taskbar.h>
#include <window.h>
#include <server.h>
#include <term.h>
#include <font.h>
#include <theme.h>
#include <clock.h>
#include <battery.h>
#include <version.h>
#include <icon.h>
#include <apps.h>
#include <dialog.h>
#include <notify.h>

#define BUTTON_TOP      4
#define BUTTON_H        30
#define BUTTON_GAP      3
#define START_W         64
#define TASK_W_MAX      160     // a longer title is cut short with ..
#define TASK_W_MIN      34
#define TASK_PAD        6       // inside a task button, either end
#define TASK_ICON_GAP   4
#define DIVIDER_W       2
#define DIVIDER_GAP     5       // either side of a divider
#define CLOCK_PAD       8       // either side of the text in the tray

#define MEM_WARN_PERCENT 95
#define MEM_CHECK_FS    (1000 * FS_PER_MS)
#define WARN_W          16
#define WARN_H          14
#define WARN_GAP        6
#define BATT_LOW_PERCENT 20
#define BATT_CHECK_FS   (5000 * FS_PER_MS)
#define BATT_GAP        6       // either side of the divider between the battery and the time

#define TIP_PAD         8
#define TIP_GAP         4       // between the tip and the top of the bar

#define SLOT_NONE       (-1)
#define SLOT_START      (-2)    // the start button, as far as held is concerned
#define SLOT_PIN(i)     (-3 - (i))  // and the pinned ones after it


#define PIN_W           BUTTON_H
#define PINS_MAX        16

#define HOVER_TIP_FS    (500 * FS_PER_MS)   // how long the pointer rests on a button before its name comes up

#define MENU_W          190
#define MENU_BANNER_W   26
#define MENU_ITEM_H     28
#define MENU_SPARE      3
#define MENU_SEP_H      8
#define MENU_ICON_W     30
#define MENU_POWER_ITEMS 2      // restart and shutdown, under the programs
#define MENU_NAME_GAP   5       // between the name and the version
#define MENU_NONE       (-1)
#define MENU_NAME       "Bleed"

#define START_LABEL     "Start"

#define CTX_W           130     // what a right click on a button brings up, wider if the labels need it
#define CTX_ITEMS_MAX   3
#define CTX_PAD         10      // either side of a label

#define ABOUT_PATH      "/initrd/bin/about"

#define POWER_PATH      "/dev/power"
#define POWER_RESTART   0xEEEEEEEEUL
#define POWER_SHUTDOWN  0xFFFFFFFFUL

static const char *power_icons[MENU_POWER_ITEMS]  = { "O", "X" };
static const char *power_labels[MENU_POWER_ITEMS] = { "Restart", "Shutdown" };

// what was right clicked, which says what the menu has in it
enum { CTX_NONE, CTX_START, CTX_TASK, CTX_PIN, CTX_MENU };
enum { ACT_ABOUT, ACT_OPEN, ACT_PIN, ACT_UNPIN, ACT_CLOSE, ACT_KILL };
static const char *act_labels[] = { "About Bleed", "Open", "Pin to taskbar", "Unpin from taskbar", "Close", "Kill Task" };

static app_t pins[PINS_MAX];
static int   pins_count;

static int      held = SLOT_NONE;   // button the mouse went down on
static int      held_over;          // and whether the pointer is still on it
static char     clock_text[32];

static int      menu_open;
static int      menu_hover   = MENU_NONE;   // the item the pointer is on
static int      menu_pressed = MENU_NONE;   // the one the mouse went down on

static int      ctx_open;                   // one of CTX_, CTX_NONE when its not up
static int      ctx_target;                 // the window id, pin or menu item it was asked for on
static int      ctx_acts[CTX_ITEMS_MAX];
static int      ctx_count;
static int      ctx_x, ctx_y;               // where the pointer was when it was asked for
static int      ctx_hover    = MENU_NONE;
static int      ctx_pressed  = MENU_NONE;

static int      mem_percent;        // how much of the usable memory is taken
static int      mem_warn;           // its past the line, the tray has the warning in it
static uint64_t mem_checked;
static int      tip_hover;          // the pointer is on the warning
static int      tip_pinned;         // it was clicked, the tip stays up until its clicked again
static char     tip_text[2][64];

static int      batt_present;       // theres one, the tray has how full it is in it
static int      batt_percent;
static int      batt_charging;
static uint64_t batt_checked;
static char     batt_text[8];
static int      batt_hover;         // the pointer is on it
static char     batt_tip[32];

static int      hot_slot = SLOT_NONE;   // the button the pointer is on, lit up
static int      hover_slot = SLOT_NONE; // the pin or task button the pointer is resting on
static uint64_t hover_since;
static int      hover_shown;
static char     hover_text[CS_TITLE_MAX];
static rect_t   hover_rect;         // worked out when it comes up, the button can be gone by the time it goes

static uint32_t *seen_ids;
static int      *seen_slots;
static int       seen_count;
static int       seen_cap;
static uint32_t seen_top;

rect_t taskbar_rect(const display_t *d) {
    return (rect_t){0, d->height - TASKBAR_H, d->width, TASKBAR_H};
}

static rect_t start_button(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    return (rect_t){bar.x + BUTTON_GAP, bar.y + BUTTON_TOP, START_W, BUTTON_H};
}

static int menu_power(void) {
    return apps_count();
}

static int menu_items(void) {
    return menu_power() + MENU_POWER_ITEMS;
}

// straight up from the start button, standing on the bar
static rect_t menu_rect(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    h   = (menu_items() + MENU_SPARE) * MENU_ITEM_H + MENU_SEP_H + 6;
    return (rect_t){bar.x, bar.y - h, MENU_W, h};
}

static rect_t menu_item(const display_t *d, int item) {
    rect_t menu = menu_rect(d);
    // the programs at the top, then the empty rows and the groove, then the power ones at the bottom
    int    y    = menu.y + 3 + item * MENU_ITEM_H +
                  (item >= menu_power() ? MENU_SPARE * MENU_ITEM_H + MENU_SEP_H : 0);
    return (rect_t){menu.x + 3 + MENU_BANNER_W, y, menu.w - 6 - MENU_BANNER_W, MENU_ITEM_H};
}

static int menu_item_at(const display_t *d, int x, int y) {
    for (int i = 0; i < menu_items(); i++) {
        if (rect_contains(menu_item(d, i), x, y))
            return i;
    }
    return MENU_NONE;
}

// top left where the pointer was, moved back in if that would put it off the screen or over the bar
static rect_t ctx_rect(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    h   = ctx_count * MENU_ITEM_H + 6;
    int    w   = CTX_W;
    for (int i = 0; i < ctx_count; i++) {
        int need = font_text_width(&font_ui, act_labels[ctx_acts[i]]) + CTX_PAD * 2 + 6;
        if (need > w) w = need;
    }

    int x = ctx_x;
    int y = ctx_y;
    if (x + w > bar.x + bar.w) x = bar.x + bar.w - w;
    if (y + h > bar.y)         y = bar.y - h;
    return (rect_t){x, y, w, h};
}

static rect_t ctx_item(const display_t *d, int item) {
    rect_t ctx = ctx_rect(d);
    return (rect_t){ctx.x + 3, ctx.y + 3 + item * MENU_ITEM_H, ctx.w - 6, MENU_ITEM_H};
}

static int ctx_item_at(const display_t *d, int x, int y) {
    for (int i = 0; i < ctx_count; i++) {
        if (rect_contains(ctx_item(d, i), x, y))
            return i;
    }
    return MENU_NONE;
}

static int pin_find(const char *path) {
    for (int i = 0; path && i < pins_count; i++) {
        if (strcmp(pins[i].path, path) == 0)
            return i;
    }
    return -1;
}

static void ctx_add(int act) {
    if (ctx_count < CTX_ITEMS_MAX)
        ctx_acts[ctx_count++] = act;
}

static int window_pin(const window_t *win, app_t *out);

static void ctx_set(display_t *d, int open, int target, int x, int y) {
    if (ctx_open)
        display_damage(d, ctx_rect(d));

    ctx_open    = open;
    ctx_target  = target;
    ctx_count   = 0;
    ctx_x       = x;
    ctx_y       = y;
    ctx_hover   = MENU_NONE;
    ctx_pressed = MENU_NONE;

    app_t     pin;
    window_t *win = window_find((uint32_t)target);
    switch (open) {
        case CTX_START:
            ctx_add(ACT_ABOUT);
            break;
        case CTX_TASK:
            if (win && window_pin(win, &pin) == 0)
                ctx_add(pin_find(pin.path) < 0 ? ACT_PIN : ACT_UNPIN);
            ctx_add(ACT_CLOSE);
            ctx_add(ACT_KILL);
            break;
        case CTX_PIN:
            ctx_add(ACT_OPEN);
            ctx_add(ACT_UNPIN);
            break;
        case CTX_MENU:
            if (apps_index(target))
                ctx_add(pin_find(apps_index(target)->path) < 0 ? ACT_PIN : ACT_UNPIN);
            break;
    }

    if (ctx_open)
        display_damage(d, ctx_rect(d));
}

static void menu_set(display_t *d, int open) {
    if (open && ctx_open)
        ctx_set(d, CTX_NONE, 0, 0, 0);
    if (open == menu_open)
        return;

    if (menu_open)
        display_damage(d, menu_rect(d));
    menu_open    = open;
    menu_hover   = MENU_NONE;
    menu_pressed = MENU_NONE;
    if (open)
        apps_scan();
    display_damage(d, menu_rect(d));
    display_damage(d, taskbar_rect(d));
}

void taskbar_menu_toggle(display_t *d) {
    menu_set(d, !menu_open);
}

int taskbar_menus_up(void) {
    return menu_open || ctx_open;
}

void taskbar_menus_close(display_t *d) {
    menu_set(d, 0);
    if (ctx_open)
        ctx_set(d, CTX_NONE, 0, 0, 0);
}

// if the kernel says no theres nothing to be done about it, the desktop just carries on
static void power(unsigned long request) {
    int fd = _open(POWER_PATH, O_WRONLY);
    if (fd < 0)
        return;
    _ioctl(fd, request, NULL);
    _close(fd);
}

static void power_restart(display_t *d) {
    (void)d;
    power(POWER_RESTART);
}

static void power_shutdown(display_t *d) {
    (void)d;
    power(POWER_SHUTDOWN);
}

static uint32_t kill_target;       // the window kill task was asked for on, while its being asked about

static void kill_confirmed(display_t *d) {
    window_t *win = window_find(kill_target);
    if (win)
        server_kill(d, win);
}

static void menu_run(display_t *d, int item) {
    if (item < menu_power()) {
        app_start(d, apps_index(item));
        return;
    }
    // easy to hit by accident, theyre asked about first
    if (item == menu_power())
        dialog_confirm(d, "Restart", "Restart the computer?", "Restart", power_restart);
    if (item == menu_power() + 1)
        dialog_confirm(d, "Shut Down", "Shut down the computer?", "Shut Down", power_shutdown);
}

// the bar moves along to make room, so all of it gets drawn again
static void pin_add(display_t *d, const app_t *pin) {
    if (pins_count == PINS_MAX || pin_find(pin->path) >= 0)
        return;
    pins[pins_count++] = *pin;
    display_damage(d, taskbar_rect(d));
}

static void pin_remove(display_t *d, int index) {
    if (index < 0 || index >= pins_count)
        return;
    for (int i = index; i < pins_count - 1; i++)
        pins[i] = pins[i + 1];
    pins_count--;
    display_damage(d, taskbar_rect(d));
}

static void pin_run(display_t *d, int index) {
    if (index >= 0 && index < pins_count)
        app_start(d, &pins[index]);
}

// a terminal is started the way it was, a client is just run
static int window_pin(const window_t *win, app_t *out) {
    if (win->term) {
        if (app_info(term_path(win), out) != 0)
            return -1;
        out->terminal = 1;
        return 0;
    }

    char path[APP_PATH_MAX];
    if (task_path(win->owner, path, sizeof(path)) != 0 || app_info(path, out) != 0)
        return -1;
    out->terminal = 0;
    return 0;
}

static int menu_pin(int item, app_t *out) {
    const app_t *app = apps_index(item);
    if (!app)
        return -1;
    *out = *app;
    return 0;
}

static void ctx_pin(display_t *d, int act, int kind, int target) {
    app_t pin;
    int   found = -1;

    if (kind == CTX_PIN) {
        pin_remove(d, target);
        return;
    }
    if (kind == CTX_TASK) {
        window_t *win = window_find((uint32_t)target);
        found = win ? window_pin(win, &pin) : -1;
    }
    if (kind == CTX_MENU)
        found = menu_pin(target, &pin);
    if (found != 0)
        return;

    if (act == ACT_PIN)
        pin_add(d, &pin);
    else
        pin_remove(d, pin_find(pin.path));
}

static void ctx_run(display_t *d, int item) {
    int act    = ctx_acts[item];
    int kind   = ctx_open;
    int target = ctx_target;
    ctx_set(d, CTX_NONE, 0, 0, 0);

    // the window could have gone while the menu was up, its looked for again
    window_t *win = kind == CTX_TASK ? window_find((uint32_t)target) : NULL;

    if (act == ACT_ABOUT) program_run(ABOUT_PATH);
    if (act == ACT_OPEN)  pin_run(d, target);
    if (act == ACT_PIN || act == ACT_UNPIN)
        ctx_pin(d, act, kind, target);
    if (act == ACT_CLOSE && win) {
        cs_msg_t msg = { .type = CS_EV_CLOSE_REQUEST };
        server_send(d, win, &msg);
    }
    // it cant be taken back, so its asked about first
    if (act == ACT_KILL && win) {
        kill_target = win->id;
        dialog_confirm(d, "Kill Task", "End it now? Anything unsaved is lost.", "Kill", kill_confirmed);
    }

    if (kind == CTX_MENU)
        menu_set(d, 0);
}

// x of the divider between the start button and the pins
static int tasks_divider(const display_t *d) {
    rect_t shell = start_button(d);
    return shell.x + shell.w + DIVIDER_GAP;
}

static rect_t pin_button(const display_t *d, int index) {
    rect_t bar = taskbar_rect(d);
    int    x   = tasks_divider(d) + DIVIDER_W + DIVIDER_GAP + index * (PIN_W + BUTTON_GAP);
    return (rect_t){x, bar.y + BUTTON_TOP, PIN_W, BUTTON_H};
}

// x of the divider between the pins and the tasks, only there if anythings pinned
static int pins_divider(const display_t *d) {
    rect_t last = pin_button(d, pins_count - 1);
    return last.x + last.w + DIVIDER_GAP;
}

static int tasks_left(const display_t *d) {
    if (!pins_count)
        return tasks_divider(d) + DIVIDER_W + DIVIDER_GAP;
    return pins_divider(d) + DIVIDER_W + DIVIDER_GAP;
}

static rect_t clock_tray(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    w   = font_digits_width(&font_ui, clock_text) + CLOCK_PAD * 2;
    if (mem_warn)
        w += WARN_W + WARN_GAP;
    if (batt_present)
        w += font_text_width(&font_ui, batt_text) + BATT_GAP * 2 + DIVIDER_W;
    return (rect_t){bar.x + bar.w - BUTTON_GAP - w, bar.y + BUTTON_TOP, w, BUTTON_H};
}

// inside the tray, left of the time
static rect_t warn_icon(const display_t *d) {
    rect_t tray = clock_tray(d);
    return (rect_t){tray.x + CLOCK_PAD, tray.y + (tray.h - WARN_H) / 2, WARN_W, WARN_H};
}

static int tip_shown(void) {
    return mem_warn && (tip_hover || tip_pinned);
}

// inside the tray, left of the divider. the whole height of it so its not fiddly to point at
static rect_t batt_rect(const display_t *d) {
    rect_t tray = clock_tray(d);
    int    x    = tray.x + CLOCK_PAD + (mem_warn ? WARN_W + WARN_GAP : 0);
    return (rect_t){x, tray.y, font_text_width(&font_ui, batt_text), tray.h};
}

// the memory tip has that corner if its up
static int batt_tip_shown(void) {
    return batt_present && batt_hover && !tip_shown();
}

// above the percentage, moved back in if that would put it off the screen
static rect_t batt_tip_rect(const display_t *d) {
    int w = font_text_width(&font_ui, batt_tip) + TIP_PAD * 2;
    int h = font_height(&font_ui) + TIP_PAD * 2;

    rect_t bar  = taskbar_rect(d);
    rect_t batt = batt_rect(d);
    int    x    = batt.x + (batt.w - w) / 2;
    if (x + w > bar.x + bar.w - BUTTON_GAP) x = bar.x + bar.w - BUTTON_GAP - w;
    return (rect_t){x, bar.y - TIP_GAP - h, w, h};
}

static void batt_tip_damage(display_t *d) {
    if (batt_tip_shown())
        display_damage(d, batt_tip_rect(d));
}

// sits just above the bar, up against the right of the screen
static rect_t tip_rect(const display_t *d) {
    int w0 = font_text_width(&font_ui, tip_text[0]);
    int w1 = font_text_width(&font_ui, tip_text[1]);
    int w  = (w0 > w1 ? w0 : w1) + TIP_PAD * 2;
    int h  = font_height(&font_ui) * 2 + TIP_PAD * 2;

    rect_t bar = taskbar_rect(d);
    return (rect_t){bar.x + bar.w - BUTTON_GAP - w, bar.y - TIP_GAP - h, w, h};
}

static void tip_set(display_t *d, int hover, int pinned) {
    batt_tip_damage(d);
    if (tip_shown())
        display_damage(d, tip_rect(d));
    tip_hover  = hover;
    tip_pinned = pinned;
    if (tip_shown())
        display_damage(d, tip_rect(d));
    batt_tip_damage(d);
}

static int mem_read(void) {
    system_memory_info_t mem;
    memset(&mem, 0, sizeof(mem));
    if (_meminfo(&mem) != 0 || !mem.MEMORY_USABLE || mem.MEMORY_FREE > mem.MEMORY_USABLE)
        return 0;
    return (int)((mem.MEMORY_USABLE - mem.MEMORY_FREE) * 100 / mem.MEMORY_USABLE);
}

static void mem_update(display_t *d) {
    if (mem_checked && clock_now() - mem_checked < MEM_CHECK_FS)
        return;
    mem_checked = clock_now();

    int percent = mem_read();
    if (percent == mem_percent)
        return;

    batt_tip_damage(d);
    if (tip_shown())
        display_damage(d, tip_rect(d));

    mem_percent = percent;
    mem_warn    = percent >= MEM_WARN_PERCENT;
    if (!mem_warn) {
        tip_hover  = 0;
        tip_pinned = 0;
    }
    snprintf(tip_text[0], sizeof(tip_text[0]), "Memory usage is at %d%%, consider closing programs.", percent);
    snprintf(tip_text[1], sizeof(tip_text[1]), "Your device may become unresponsive.");

    if (tip_shown())
        display_damage(d, tip_rect(d));
    batt_tip_damage(d);
    display_damage(d, taskbar_rect(d));
}

static void batt_update(display_t *d) {
    if (batt_checked && clock_now() - batt_checked < BATT_CHECK_FS)
        return;
    batt_checked = clock_now();

    battery_t batt;
    int       present = battery_read(&batt);
    if (present == batt_present && batt.percent == batt_percent && batt.charging == batt_charging)
        return;

    // the tray is a different width now, so where it was goes too
    int low_now = present && !batt.charging && batt.percent <= BATT_LOW_PERCENT;
    int low_was = batt_present && !batt_charging && batt_percent <= BATT_LOW_PERCENT;
    if (low_now && !low_was) {
        char text[64];
        snprintf(text, sizeof(text), "%d%% left, plug it in soon.", batt.percent);
        notify_post(d, "Battery low", text);
    }

    batt_tip_damage(d);
    display_damage(d, taskbar_rect(d));
    batt_present  = present;
    batt_percent  = batt.percent;
    batt_charging = batt.charging;
    snprintf(batt_text, sizeof(batt_text), "%d%%", batt_percent);
    snprintf(batt_tip, sizeof(batt_tip), "%d%% %s", batt_percent,
             batt_charging ? "Charging" : batt_percent <= BATT_LOW_PERCENT ? "Low Battery" : "remaining");
    batt_tip_damage(d);
}

static void batt_hover_set(display_t *d, int hover) {
    batt_tip_damage(d);
    batt_hover = hover;
    batt_tip_damage(d);
}

static int clock_divider(const display_t *d) {
    return clock_tray(d).x - DIVIDER_GAP - DIVIDER_W;
}

static int task_width(const window_t *win) {
    int w = TASK_PAD * 2 + font_text_width(&font_ui, win->title);
    if (win->icon)
        w += win->icon->w + TASK_ICON_GAP;

    if (w > TASK_W_MAX) w = TASK_W_MAX;
    if (w < TASK_W_MIN) w = TASK_W_MIN;
    return w;
}

// once they wont all fit none of them gets more than an even share of the room
static int task_share(const display_t *d) {
    int count = 0;
    int total = 0;
    for (; window_index(count); count++)
        total += task_width(window_index(count)) + (count ? BUTTON_GAP : 0);

    int room = clock_divider(d) - DIVIDER_GAP - tasks_left(d);
    if (!count || total <= room)
        return TASK_W_MAX;

    int share = (room - (count - 1) * BUTTON_GAP) / count;
    return share < TASK_W_MIN ? TASK_W_MIN : share;
}

static int task_width_in(const window_t *win, int share) {
    int w = task_width(win);
    return w > share ? share : w;
}

// buttons are laid out in slot order, each one after all the buttons with a lower slot
static rect_t task_button(const display_t *d, const window_t *win) {
    int share = task_share(d);
    int x     = tasks_left(d);
    for (int i = 0; window_index(i); i++) {
        if (window_index(i)->slot < win->slot)
            x += task_width_in(window_index(i), share) + BUTTON_GAP;
    }

    rect_t bar = taskbar_rect(d);
    return (rect_t){x, bar.y + BUTTON_TOP, task_width_in(win, share), BUTTON_H};
}

// a button that would run into the clock just isnt there
static int task_fits(const display_t *d, rect_t button) {
    return button.x + button.w <= clock_divider(d) - DIVIDER_GAP;
}

rect_t taskbar_button_of(const display_t *d, const window_t *win) {
    rect_t button = task_button(d, win);
    if (task_fits(d, button))
        return button;
    rect_t bar = taskbar_rect(d);
    return (rect_t){bar.x + bar.w / 2, bar.y, 1, 1};
}

static window_t *task_at(const display_t *d, int x, int y) {
    for (int i = 0; window_index(i); i++) {
        rect_t button = task_button(d, window_index(i));
        if (task_fits(d, button) && rect_contains(button, x, y))
            return window_index(i);
    }
    return NULL;
}

static window_t *window_in_slot(int slot) {
    for (int i = 0; window_index(i); i++) {
        if (window_index(i)->slot == slot)
            return window_index(i);
    }
    return NULL;
}

// above the button, moved back in if that would put it off the screen
static rect_t hover_tip_rect(const display_t *d, rect_t button) {
    rect_t bar    = taskbar_rect(d);
    int    w      = font_text_width(&font_ui, hover_text) + TIP_PAD * 2;
    int    h      = font_height(&font_ui) + TIP_PAD;
    int    x      = button.x + (button.w - w) / 2;

    if (x + w > bar.x + bar.w - BUTTON_GAP) x = bar.x + bar.w - BUTTON_GAP - w;
    if (x < bar.x + BUTTON_GAP)             x = bar.x + BUTTON_GAP;
    return (rect_t){x, bar.y - TIP_GAP - h, w, h};
}

static void hover_hide(display_t *d) {
    if (hover_shown)
        display_damage(d, hover_rect);
    hover_shown = 0;
}

static void hover_show(display_t *d) {
    window_t *win = hover_slot >= 0 ? window_in_slot(hover_slot) : NULL;
    int       pin = SLOT_PIN(0) - hover_slot;
    rect_t    button;

    if (hover_slot <= SLOT_PIN(0) && pin < pins_count) {
        strcpy(hover_text, pins[pin].name);
        button = pin_button(d, pin);
    } else if (win) {
        strcpy(hover_text, win->title);
        button = task_button(d, win);
    } else {
        return;
    }

    hover_rect  = hover_tip_rect(d, button);
    hover_shown = 1;
    display_damage(d, hover_rect);
}

// going from one button to the next while a name is up brings the next one up straight away
static void hover_set(display_t *d, int slot) {
    if (slot == hover_slot)
        return;

    int was_shown = hover_shown;
    hover_hide(d);
    hover_slot  = slot;
    hover_since = clock_now();
    if (was_shown && slot != SLOT_NONE)
        hover_show(d);
}

void taskbar_tick(display_t *d) {
    if (hover_slot != SLOT_NONE && !hover_shown && clock_now() - hover_since >= HOVER_TIP_FS)
        hover_show(d);
}

void taskbar_frame(display_t *d) {
    static uint32_t shown;
    window_t *win = window_focused();
    uint32_t  id  = win && !win->minimised ? win->id : 0;
    if (id == shown)
        return;
    shown = id;
    display_damage(d, taskbar_rect(d));
}

int taskbar_wait_ms(void) {
    if (hover_slot == SLOT_NONE || hover_shown)
        return -1;

    uint64_t gone = clock_now() - hover_since;
    return gone >= HOVER_TIP_FS ? 0 : (int)((HOVER_TIP_FS - gone) / FS_PER_MS) + 1;
}

static void clock_read(char out[32]) {
    rtc_time_t now;
    memset(&now, 0, sizeof(now));
    _time(&now);
    snprintf(out, 32, "%02u:%02u  %02u/%02u/%u", now.hour, now.min, now.day, now.mon, now.year);
}

void taskbar_update(display_t *d) {
    int changed = 0;

    mem_update(d);
    batt_update(d);

    char now[32];
    clock_read(now);
    if (strcmp(now, clock_text) != 0) {
        batt_tip_damage(d);
        display_damage(d, taskbar_rect(d));
        memcpy(clock_text, now, sizeof(clock_text));
        batt_tip_damage(d);
    }

    int       count = 0;
    window_t *top   = window_focused();
    while (window_index(count))
        count++;

    // if theres no room to remember them the bar just gets drawn every time
    if (count > seen_cap) {
        uint32_t *ids   = realloc(seen_ids, (size_t)count * 2 * sizeof(uint32_t));
        int      *slots = realloc(seen_slots, (size_t)count * 2 * sizeof(int));
        if (ids)   seen_ids   = ids;
        if (slots) seen_slots = slots;
        if (ids && slots)
            seen_cap = count * 2;
        else
            changed = 1;
    }

    for (count = 0; count < seen_cap && window_index(count); count++) {
        window_t *win = window_index(count);
        if (count >= seen_count || seen_ids[count] != win->id || seen_slots[count] != win->slot)
            changed = 1;
        seen_ids[count]   = win->id;
        seen_slots[count] = win->slot;
    }
    if (count != seen_count || (top ? top->id : 0) != seen_top)
        changed = 1;

    seen_count = count;
    seen_top   = top ? top->id : 0;

    // a held task button whose window went away has nothing left to do on release
    if (held >= 0 && !window_in_slot(held))
        held = SLOT_NONE;

    // the buttons have moved, a name thats up could be over the wrong one
    if (changed) {
        hover_hide(d);
        hover_slot = SLOT_NONE;
        display_damage(d, taskbar_rect(d));
    }
}

// a dark line with a light one right beside it, the 9x etched groove
static void divider_draw(display_t *d, rect_t clip, int x) {
    rect_t bar = taskbar_rect(d);
    rect_t r   = {x, bar.y + BUTTON_TOP, 1, BUTTON_H};
    display_fill(d, rect_clip(r, clip), COLOUR_SHADOW);
    r.x++;
    display_fill(d, rect_clip(r, clip), COLOUR_HILIGHT);
}

static void title_fit(const char *title, int max_w, char out[CS_TITLE_MAX]) {
    strncpy(out, title, CS_TITLE_MAX - 1);
    out[CS_TITLE_MAX - 1] = '\0';
    if (font_text_width(&font_ui, out) <= max_w)
        return;

    int dots = font_text_width(&font_ui, "..");
    for (size_t len = strlen(out); len > 0; len--) {
        out[len] = '\0';
        if (font_text_width(&font_ui, out) + dots <= max_w) {
            strcat(out, "..");
            return;
        }
    }
    out[0] = '\0';
}

// the label goes down and right a pixel with the button
static void button_draw(display_t *d, rect_t clip, rect_t r, const char *label, int down, int hot) {
    display_button(d, clip, r, down, hot);

    int tx = r.x + (r.w - font_text_width(&font_ui, label)) / 2 + down;
    int ty = r.y + (r.h - font_height(&font_ui)) / 2 + down;
    font_draw(d, rect_clip(rect_shrink(r, 2), clip), &font_ui, tx, ty, label, COLOUR_TEXT);
}

// a triangle with a ! in it
static void warn_draw(display_t *d, rect_t clip, rect_t r) {
    for (int row = 0; row < WARN_H; row++) {
        int half = row * (WARN_W / 2 - 1) / (WARN_H - 1);
        display_fill(d, rect_clip((rect_t){r.x + WARN_W / 2 - 1 - half, r.y + row, half * 2 + 2, 1}, clip),
                     COLOUR_WARN);
    }
    display_fill(d, rect_clip((rect_t){r.x + WARN_W / 2 - 1, r.y + 5, 2, 5}, clip), COLOUR_SHADOW);
    display_fill(d, rect_clip((rect_t){r.x + WARN_W / 2 - 1, r.y + 11, 2, 2}, clip), COLOUR_SHADOW);
}

static void tip_box_draw(display_t *d, rect_t clip, rect_t r) {
    display_fill(d, rect_clip(r, clip), COLOUR_FACE);
    display_edge(d, clip, r, COLOUR_HILIGHT, COLOUR_DKSHADOW);
    display_edge(d, clip, rect_shrink(r, 1), COLOUR_LIGHT, COLOUR_SHADOW);
}

static void tip_draw(display_t *d, rect_t clip) {
    rect_t r = tip_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    tip_box_draw(d, clip, r);
    for (int i = 0; i < 2; i++)
        font_draw(d, rect_clip(rect_shrink(r, 2), clip), &font_ui, r.x + TIP_PAD,
                  r.y + TIP_PAD + i * font_height(&font_ui), tip_text[i], COLOUR_TEXT);
}

static void batt_tip_draw(display_t *d, rect_t clip) {
    rect_t r = batt_tip_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    tip_box_draw(d, clip, r);
    font_draw(d, rect_clip(rect_shrink(r, 2), clip), &font_ui, r.x + TIP_PAD, r.y + TIP_PAD, batt_tip,
              COLOUR_TEXT);
}

// the same etched groove as the dividers on the bar, lying down
static void groove_draw(display_t *d, rect_t clip, rect_t below) {
    rect_t sep = {below.x + 2, below.y - MENU_SEP_H / 2 - 1, below.w - 4, 1};
    display_fill(d, rect_clip(sep, clip), COLOUR_SHADOW);
    sep.y++;
    display_fill(d, rect_clip(sep, clip), COLOUR_HILIGHT);
}

// the 9x start menu: a raised box with the name up a strip on the left
static void menu_draw(display_t *d, rect_t clip) {
    rect_t r = menu_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    display_raised(d, clip, r);

    // a title bar stood on its end, the dark end is where the text starts
    rect_t banner = {r.x + 3, r.y + 3, MENU_BANNER_W, r.h - 6};
    display_gradient(d, clip, banner, COLOUR_CAPTION_R, COLOUR_CAPTION_L, 1);

    // the version is smaller, its moved across so both sit on the one baseline
    int name_x = banner.x + (banner.w - font_height(&font_banner)) / 2;
    int name_y = banner.y + banner.h - 6;
    font_draw_up(d, rect_clip(banner, clip), &font_banner, name_x, name_y, MENU_NAME, COLOUR_CAPTION_TEXT);
    font_draw_up(d, rect_clip(banner, clip), &font_version, name_x + font_banner.ascent - font_version.ascent,
                 name_y - font_text_width(&font_banner, MENU_NAME) - MENU_NAME_GAP, BLEED_VERSION,
                 COLOUR_CAPTION_TEXT);

    for (int i = 0; i < menu_items(); i++) {
        rect_t       item = menu_item(d, i);
        uint32_t     text = COLOUR_TEXT;
        int          ty   = item.y + (item.h - font_height(&font_ui)) / 2;
        const app_t *app  = apps_index(i);

        if (i == menu_hover)
            display_fill(d, rect_clip(item, clip), COLOUR_BUTTON_LIT);

        // a program without an icon gets the first letter of its name which looks ass but whatever
        char        letter[2] = { app ? app->name[0] : '\0', '\0' };
        const char *mark      = app ? letter : power_icons[i - menu_power()];
        const char *label     = app ? app->name : power_labels[i - menu_power()];

        if (app && app->icon) {
            display_blend(d, rect_clip(item, clip), item.x + (MENU_ICON_W - app->icon->w) / 2,
                          item.y + (item.h - app->icon->h) / 2, app->icon->pixels, app->icon->w, app->icon->h);
        } else {
            int iw = font_text_width(&font_caption, mark);
            font_draw(d, rect_clip(item, clip), &font_caption, item.x + (MENU_ICON_W - iw) / 2, ty, mark, text);
        }
        font_draw(d, rect_clip(item, clip), &font_ui, item.x + MENU_ICON_W, ty, label, text);
    }

    groove_draw(d, clip, menu_item(d, menu_power()));
}

static void ctx_draw(display_t *d, rect_t clip) {
    rect_t r = ctx_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    display_raised(d, clip, r);

    for (int i = 0; i < ctx_count; i++) {
        rect_t item = ctx_item(d, i);
        if (i == ctx_hover)
            display_fill(d, rect_clip(item, clip), COLOUR_BUTTON_LIT);

        font_draw(d, rect_clip(item, clip), &font_ui, item.x + CTX_PAD,
                  item.y + (item.h - font_height(&font_ui)) / 2, act_labels[ctx_acts[i]], COLOUR_TEXT);
    }
}

void taskbar_draw(display_t *d, rect_t clip) {
    // the parts of the bar that arent on the bar, they go over whatever windows are there
    if (menu_open)
        menu_draw(d, clip);
    if (ctx_open)
        ctx_draw(d, clip);
    if (tip_shown())
        tip_draw(d, clip);
    if (batt_tip_shown())
        batt_tip_draw(d, clip);
    if (hover_shown && rect_overlaps(hover_rect, clip)) {
        rect_t r = hover_rect;
        tip_box_draw(d, clip, r);
        font_draw(d, rect_clip(rect_shrink(r, 2), clip), &font_ui, r.x + TIP_PAD, r.y + TIP_PAD / 2, hover_text,
                  COLOUR_TEXT);
    }

    rect_t bar = taskbar_rect(d);
    clip = rect_clip(clip, bar);
    if (!clip.w || !clip.h)
        return;

    // raised along the top edge only, the other three sides are off the screen
    display_fill(d, clip, COLOUR_FACE);
    display_fill(d, rect_clip((rect_t){bar.x, bar.y, bar.w, 1}, clip), COLOUR_LIGHT);
    display_fill(d, rect_clip((rect_t){bar.x, bar.y + 1, bar.w, 1}, clip), COLOUR_HILIGHT);

    rect_t start = start_button(d);
    int    open  = menu_open || (held == SLOT_START && held_over);
    display_button(d, clip, start, open, hot_slot == SLOT_START);
    font_draw(d, rect_clip(rect_shrink(start, 2), clip), &font_caption,
              start.x + (start.w - font_text_width(&font_caption, START_LABEL)) / 2 + open,
              start.y + (start.h - font_height(&font_caption)) / 2 + open, START_LABEL, COLOUR_TEXT);
    divider_draw(d, clip, tasks_divider(d));

    // just the icon, or the first letter of the name if theres no icon
    for (int i = 0; i < pins_count; i++) {
        rect_t        button = pin_button(d, i);
        int           down   = held == SLOT_PIN(i) && held_over;
        int           hot    = hot_slot == SLOT_PIN(i);
        const icon_t *icon   = pins[i].icon;

        if (icon) {
            button_draw(d, clip, button, "", down, hot);
            display_blend(d, rect_clip(rect_shrink(button, 2), clip), button.x + (button.w - icon->w) / 2 + down,
                          button.y + (button.h - icon->h) / 2 + down, icon->pixels, icon->w, icon->h);
        } else {
            char letter[2] = { pins[i].name[0], '\0' };
            button_draw(d, clip, button, letter, down, hot);
        }
    }
    if (pins_count)
        divider_draw(d, clip, pins_divider(d));

    // the focused windows button is held in, like 9x
    for (int i = 0; window_index(i); i++) {
        window_t *win    = window_index(i);
        rect_t    button = task_button(d, win);
        if (!task_fits(d, button))
            continue;

        int down = (held == win->slot && held_over) || (win == window_focused() && !win->minimised);
        button_draw(d, clip, button, "", down, hot_slot == win->slot);

        rect_t inside = rect_clip(rect_shrink(button, 2), clip);
        int    x      = button.x + TASK_PAD + down;
        if (win->icon) {
            display_blend(d, inside, x, button.y + (button.h - win->icon->h) / 2 + down, win->icon->pixels,
                          win->icon->w, win->icon->h);
            x += win->icon->w + TASK_ICON_GAP;
        }

        int  room = button.x + button.w - TASK_PAD - x + down;
        char title[CS_TITLE_MAX];
        title_fit(win->title, room, title);
        font_draw(d, rect_clip((rect_t){x, button.y, room, button.h}, clip), &font_ui,
                  x, button.y + (button.h - font_height(&font_ui)) / 2 + down, title,
                  win->minimised ? COLOUR_BACK_TEXT : COLOUR_TEXT);
    }

    divider_draw(d, clip, clock_divider(d));

    rect_t tray = clock_tray(d);
    display_edge(d, clip, tray, COLOUR_SHADOW, COLOUR_HILIGHT);

    int text_x = tray.x + CLOCK_PAD;
    if (mem_warn) {
        warn_draw(d, clip, warn_icon(d));
        text_x += WARN_W + WARN_GAP;
    }
    if (batt_present) {
        font_draw(d, rect_clip(tray, clip), &font_ui, text_x, tray.y + (tray.h - font_height(&font_ui)) / 2,
                  batt_text, batt_percent <= BATT_LOW_PERCENT ? COLOUR_LOW : COLOUR_TEXT);
        text_x += font_text_width(&font_ui, batt_text) + BATT_GAP;

        // the same groove as the ones on the bar, kept off the edges of the tray
        rect_t groove = {text_x, tray.y + 4, 1, tray.h - 8};
        display_fill(d, rect_clip(groove, clip), COLOUR_SHADOW);
        groove.x++;
        display_fill(d, rect_clip(groove, clip), COLOUR_HILIGHT);
        text_x += DIVIDER_W + BATT_GAP;
    }
    font_draw_digits(d, rect_clip(tray, clip), &font_ui, text_x, tray.y + (tray.h - font_height(&font_ui)) / 2,
                     clock_text, COLOUR_TEXT);
}

// which button is at x y, as the value held would have for it
static int button_at(const display_t *d, int x, int y) {
    if (rect_contains(start_button(d), x, y))
        return SLOT_START;
    for (int i = 0; i < pins_count; i++) {
        if (rect_contains(pin_button(d, i), x, y))
            return SLOT_PIN(i);
    }

    window_t *win = task_at(d, x, y);
    return win ? win->slot : SLOT_NONE;
}

static rect_t slot_rect(const display_t *d, int slot) {
    if (slot == SLOT_START)
        return start_button(d);
    if (slot <= SLOT_PIN(0))
        return pin_button(d, SLOT_PIN(0) - slot);
    window_t *win = window_in_slot(slot);
    return win ? task_button(d, win) : (rect_t){0, 0, 0, 0};
}

static void hot_set(display_t *d, int slot) {
    if (slot == hot_slot)
        return;
    if (hot_slot != SLOT_NONE)
        display_damage(d, slot_rect(d, hot_slot));
    hot_slot = slot;
    if (hot_slot != SLOT_NONE)
        display_damage(d, slot_rect(d, hot_slot));
}

int taskbar_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons) {
    int left      = buttons & MOUSE_BTN_LEFT;
    int was_left  = was_buttons & MOUSE_BTN_LEFT;
    int right     = buttons & MOUSE_BTN_RIGHT;
    int was_right = was_buttons & MOUSE_BTN_RIGHT;

    int on_bar  = rect_contains(taskbar_rect(d), x, y);

    int by = on_bar ? taskbar_rect(d).y + TASKBAR_H / 2 : y;
    int on_warn = mem_warn && rect_contains(warn_icon(d), x, y);

    // only resting on a pin or a task with nothing else going on
    int rest = SLOT_NONE;
    if (on_bar && !buttons && !menu_open && !ctx_open && held == SLOT_NONE)
        rest = button_at(d, x, by);
    hover_set(d, rest == SLOT_START ? SLOT_NONE : rest);
    int hot = on_bar && !buttons ? button_at(d, x, by) : SLOT_NONE;
    hot_set(d, hot);

    // done before anything can return, the pointer leaving the bar has to take the tip down
    if (on_warn != tip_hover)
        tip_set(d, on_warn, tip_pinned);
    int on_batt = batt_present && rect_contains(batt_rect(d), x, y);
    if (on_batt != batt_hover)
        batt_hover_set(d, on_batt);

    if (on_warn && left && !was_left && held == SLOT_NONE) {
        tip_set(d, tip_hover, !tip_pinned);
        return 1;
    }

    if (ctx_open) {
        int in_ctx = rect_contains(ctx_rect(d), x, y);
        int item   = ctx_item_at(d, x, y);

        if (item != ctx_hover) {
            ctx_hover = item;
            display_damage(d, ctx_rect(d));
        }

        if (left && !was_left && in_ctx) {
            ctx_pressed = item;
            return 1;
        }
        if (!left && was_left && ctx_pressed != MENU_NONE) {
            if (item == ctx_pressed)
                ctx_run(d, item);
            else
                ctx_set(d, CTX_NONE, 0, 0, 0);
            return 1;
        }

        // any other press puts it away and still counts as a press on whats there
        if ((left && !was_left) || (right && !was_right))
            ctx_set(d, CTX_NONE, 0, 0, 0);
        else if (in_ctx || ctx_pressed != MENU_NONE)
            return 1;
    }

    if (right && !was_right && held == SLOT_NONE && on_bar) {
        int       slot = button_at(d, x, by);
        window_t *win  = slot >= 0 ? window_in_slot(slot) : NULL;

        if (slot == SLOT_START || win || slot <= SLOT_PIN(0)) {
            menu_set(d, 0);
            if (slot == SLOT_START) ctx_set(d, CTX_START, 0, x, d->height);
            if (win)                ctx_set(d, CTX_TASK, (int)win->id, x, d->height);
            if (slot <= SLOT_PIN(0)) ctx_set(d, CTX_PIN, SLOT_PIN(0) - slot, x, d->height);
            return 1;
        }
    }

    if (menu_open) {
        int in_menu = rect_contains(menu_rect(d), x, y);
        int item    = menu_item_at(d, x, y);

        if (item != menu_hover) {
            menu_hover = item;
            display_damage(d, menu_rect(d));
        }

        if (left && !was_left && in_menu) {
            menu_pressed = item;
            return 1;
        }

        if (right && !was_right && in_menu) {
            if (item != MENU_NONE && item < menu_power())
                ctx_set(d, CTX_MENU, item, x, y);
            return 1;
        }
        // has to come back up on the one it went down on, same as the buttons
        if (!left && was_left && menu_pressed != MENU_NONE) {
            int run = (item == menu_pressed) ? item : MENU_NONE;
            menu_set(d, 0);
            if (run != MENU_NONE)
                menu_run(d, run);
            return 1;
        }

        // a press anywhere else puts the menu away, and still counts as a press on whats there.
        // on the start button its left to the button, that closes it on the way back up
        if (left && !was_left && !rect_contains(start_button(d), x, y))
            menu_set(d, 0);
        else if (in_menu || menu_pressed != MENU_NONE)
            return 1;
    }

    // once a button is held the pointer is ours wherever it goes, until the mouse button comes up
    if (held == SLOT_NONE && !on_bar)
        return 0;

    if (left && !was_left) {
        held      = button_at(d, x, by);
        held_over = 1;
        display_damage(d, taskbar_rect(d));
        return 1;
    }

    if (held != SLOT_NONE) {
        int over = (button_at(d, x, by) == held);
        if (over != held_over) {
            held_over = over;
            display_damage(d, taskbar_rect(d));
        }
    }

    if (!left && was_left && held != SLOT_NONE) {
        int pressed = held;
        int over    = held_over;
        held = SLOT_NONE;
        display_damage(d, taskbar_rect(d));

        // only counts if it comes back up on the same button
        if (over && pressed == SLOT_START)
            menu_set(d, !menu_open);
        if (over && pressed <= SLOT_PIN(0))
            pin_run(d, SLOT_PIN(0) - pressed);
        if (over && pressed >= 0) {
            // the same as 9x: brings it up, or puts it away if its the one already in front
            window_t *win = window_in_slot(pressed);
            if (win && win->minimised)
                window_restore(d, win);
            else if (win && win == window_top())
                window_minimise(d, win);
            else if (win)
                window_raise(d, win);
        }
    }
    return 1;
}
