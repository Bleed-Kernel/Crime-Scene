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

#define BUTTON_TOP      4       // from the top of the bar
#define BUTTON_H        22
#define BUTTON_GAP      3
#define START_W         54
#define TASK_W          34
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

#define MENU_W          190
#define MENU_BANNER_W   26
#define MENU_ITEM_H     28
#define MENU_SPARE      3
#define MENU_SEP_H      8
#define MENU_ICON_W     30
#define MENU_ITEMS      7
#define MENU_POWER      5       // the first of the two at the bottom
#define MENU_NAME_GAP   5       // between the name and the version
#define MENU_NONE       (-1)

#define CTX_W           130     // what a right click on the start button brings up
#define CTX_ITEMS       1

// these are just default programs im putting there for now
// some more dynamic logic later will be nice, maybe even context menus for folders
#define ABOUT_PATH      "/initrd/bin/about"
#define DOOM_PATH       "/initrd/bin/doom"
#define QUAKE_PATH      "/initrd/bin/quake2"
#define TETRIS_PATH     "/initrd/bin/tetris"
#define TASKV_PATH      "/initrd/bin/taskv"
#define FD_SCAN         256

#define POWER_PATH      "/dev/power"
#define POWER_RESTART   0xEEEEEEEEUL
#define POWER_SHUTDOWN  0xFFFFFFFFUL

static const char *menu_icons[MENU_ITEMS]  = { "$", "!", "Q", "#", "=", "O", "X" };
static const char *menu_labels[MENU_ITEMS] = { "Verdict Shell", "Doom", "Quake II", "Tetris", "Task Viewer",
                                               "Restart", "Shutdown" };
static const char *ctx_labels[CTX_ITEMS]   = { "About Bleed" };

static int      held = SLOT_NONE;   // button the mouse went down on
static int      held_over;          // and whether the pointer is still on it
static char     clock_text[32];

static int      menu_open;
static int      menu_hover   = MENU_NONE;   // the item the pointer is on
static int      menu_pressed = MENU_NONE;   // the one the mouse went down on

static int      ctx_open;
static int      ctx_x;                      // where the pointer was when it was asked for
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

static uint32_t *seen_ids;
static int      *seen_slots;
static int       seen_count;
static int       seen_cap;
static uint32_t seen_top;

rect_t taskbar_rect(const display_t *d) {
    return (rect_t){0, d->height - TASKBAR_H, d->width, TASKBAR_H};
}

// 1 to 9, then A to Z, then A1 A2 and so on for anything past that
static void slot_label(int slot, char out[8]) {
    if (slot < 9)
        snprintf(out, 8, "%d", slot + 1);
    else if (slot < 9 + 26)
        snprintf(out, 8, "%c", 'A' + (slot - 9));
    else
        snprintf(out, 8, "A%d", slot - (9 + 26) + 1);
}

static rect_t start_button(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    return (rect_t){bar.x + BUTTON_GAP, bar.y + BUTTON_TOP, START_W, BUTTON_H};
}

// straight up from the start button, standing on the bar
static rect_t menu_rect(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    h   = (MENU_ITEMS + MENU_SPARE) * MENU_ITEM_H + MENU_SEP_H + 6;
    return (rect_t){bar.x + BUTTON_GAP, bar.y - h, MENU_W, h};
}

static rect_t menu_item(const display_t *d, int item) {
    rect_t menu = menu_rect(d);
    // the programs at the top, then the empty rows and the groove, then the power ones at the bottom
    int    y    = menu.y + 3 + item * MENU_ITEM_H + (item >= MENU_POWER ? MENU_SPARE * MENU_ITEM_H + MENU_SEP_H : 0);
    return (rect_t){menu.x + 3 + MENU_BANNER_W, y, menu.w - 6 - MENU_BANNER_W, MENU_ITEM_H};
}

static int menu_item_at(const display_t *d, int x, int y) {
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (rect_contains(menu_item(d, i), x, y))
            return i;
    }
    return MENU_NONE;
}

// stood on the bar where the pointer was, moved back in if that would put it off the screen
static rect_t ctx_rect(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    h   = CTX_ITEMS * MENU_ITEM_H + 6;
    int    x   = ctx_x;
    if (x + CTX_W > bar.x + bar.w) x = bar.x + bar.w - CTX_W;
    return (rect_t){x, bar.y - h, CTX_W, h};
}

static rect_t ctx_item(const display_t *d, int item) {
    rect_t ctx = ctx_rect(d);
    return (rect_t){ctx.x + 3, ctx.y + 3 + item * MENU_ITEM_H, ctx.w - 6, MENU_ITEM_H};
}

static int ctx_item_at(const display_t *d, int x, int y) {
    for (int i = 0; i < CTX_ITEMS; i++) {
        if (rect_contains(ctx_item(d, i), x, y))
            return i;
    }
    return MENU_NONE;
}

static void ctx_set(display_t *d, int open, int x) {
    if (ctx_open)
        display_damage(d, ctx_rect(d));

    ctx_open    = open;
    ctx_x       = x;
    ctx_hover   = MENU_NONE;
    ctx_pressed = MENU_NONE;
    if (ctx_open)
        display_damage(d, ctx_rect(d));
}

static void menu_set(display_t *d, int open) {
    if (open && ctx_open)
        ctx_set(d, 0, 0);
    if (open == menu_open)
        return;

    menu_open    = open;
    menu_hover   = MENU_NONE;
    menu_pressed = MENU_NONE;
    display_damage(d, menu_rect(d));
    display_damage(d, taskbar_rect(d));
}

void taskbar_menu_toggle(display_t *d) {
    menu_set(d, !menu_open);
}

// if the kernel says no theres nothing to be done about it, the desktop just carries on
static void power(unsigned long request) {
    int fd = _open(POWER_PATH, O_WRONLY);
    if (fd < 0)
        return;
    _ioctl(fd, request, NULL);
    _close(fd);
}

// forked twice so its nobodys to wait for, the one in the middle is gone as soon as its started
static void program_run(const char *path) {
    long pid = fork();
    if (pid < 0)
        return;
    if (pid > 0) {
        _waitpid(pid);
        return;
    }

    if (fork() == 0) {
        for (int fd = 3; fd < FD_SCAN; fd++)
            _close(fd);

        char *const argv[] = { (char *)path, NULL };
        execv(path, argv);
        _exit(127);
    }
    _exit(0);
}

static void menu_run(display_t *d, int item) {
    if (item == 0) term_open(d, NULL);
    if (item == 1) program_run(DOOM_PATH);
    if (item == 2) program_run(QUAKE_PATH);
    if (item == 3) program_run(TETRIS_PATH);
    // its a terminal program, so it gets a terminal to itself
    if (item == 4) term_run(d, TASKV_PATH, "Task Viewer");
    if (item == 5) power(POWER_RESTART);
    if (item == 6) power(POWER_SHUTDOWN);
}

static void ctx_run(int item) {
    if (item == 0) program_run(ABOUT_PATH);
}

// x of the divider between the start button and the tasks
static int tasks_divider(const display_t *d) {
    rect_t shell = start_button(d);
    return shell.x + shell.w + DIVIDER_GAP;
}

static rect_t clock_tray(const display_t *d) {
    rect_t bar = taskbar_rect(d);
    int    w   = font_text_width(&font_ui, clock_text) + CLOCK_PAD * 2;
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

// buttons are laid out in slot order, so position is how many windows have a lower slot
static rect_t task_button(const display_t *d, const window_t *win) {
    int before = 0;
    for (int i = 0; window_index(i); i++) {
        if (window_index(i)->slot < win->slot)
            before++;
    }

    rect_t bar = taskbar_rect(d);
    int    x   = tasks_divider(d) + DIVIDER_W + DIVIDER_GAP + before * (TASK_W + BUTTON_GAP);
    return (rect_t){x, bar.y + BUTTON_TOP, TASK_W, BUTTON_H};
}

// a button that would run into the clock just isnt there
static int task_fits(const display_t *d, rect_t button) {
    return button.x + button.w <= clock_divider(d) - DIVIDER_GAP;
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
    window_t *top   = window_top();
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

    if (changed)
        display_damage(d, taskbar_rect(d));
}

static void divider_draw(display_t *d, rect_t clip, int x) {
    rect_t bar = taskbar_rect(d);
    rect_t r   = {x, bar.y + BUTTON_TOP, 1, BUTTON_H};

    // a dark line with a light one right beside it, the 9x etched groove
    display_fill(d, rect_clip(r, clip), COLOUR_SHADOW);
    r.x++;
    display_fill(d, rect_clip(r, clip), COLOUR_HILIGHT);
}

static void button_draw(display_t *d, rect_t clip, rect_t r, const char *label, int down) {
    display_fill(d, rect_clip(r, clip), down ? COLOUR_LIGHT : COLOUR_FACE);
    if (down) {
        display_edge(d, clip, r, COLOUR_DKSHADOW, COLOUR_HILIGHT);
        display_edge(d, clip, rect_shrink(r, 1), COLOUR_SHADOW, COLOUR_LIGHT);
    } else {
        display_edge(d, clip, r, COLOUR_HILIGHT, COLOUR_DKSHADOW);
        display_edge(d, clip, rect_shrink(r, 1), COLOUR_LIGHT, COLOUR_SHADOW);
    }

    // the label goes down and right a pixel with the button
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

// the 9x start menu: a raised box with the name up a strip on the left
static void menu_draw(display_t *d, rect_t clip) {
    rect_t r = menu_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    display_fill(d, rect_clip(r, clip), COLOUR_FACE);
    display_edge(d, clip, r, COLOUR_LIGHT, COLOUR_DKSHADOW);
    display_edge(d, clip, rect_shrink(r, 1), COLOUR_HILIGHT, COLOUR_SHADOW);

    rect_t banner = {r.x + 3, r.y + 3, MENU_BANNER_W, r.h - 6};
    // a title bar stood on its end, the dark end is where the text starts
    display_gradient(d, clip, banner, COLOUR_CAPTION_R, COLOUR_CAPTION_L, 1);

    // the version is smaller, its moved across so both sit on the one baseline
    int name_x = banner.x + (banner.w - font_height(&font_banner)) / 2;
    int name_y = banner.y + banner.h - 6;
    font_draw_up(d, rect_clip(banner, clip), &font_banner, name_x, name_y, "Bleed", COLOUR_CAPTION_TEXT);
    font_draw_up(d, rect_clip(banner, clip), &font_version, name_x + font_banner.ascent - font_version.ascent,
                 name_y - font_text_width(&font_banner, "Bleed") - MENU_NAME_GAP, BLEED_VERSION,
                 COLOUR_CAPTION_TEXT);

    for (int i = 0; i < MENU_ITEMS; i++) {
        rect_t   item = menu_item(d, i);
        uint32_t text = COLOUR_TEXT;
        int      ty   = item.y + (item.h - font_height(&font_ui)) / 2;

        if (i == menu_hover)
            display_fill(d, rect_clip(item, clip), COLOUR_BUTTON_LIT);

        int iw = font_text_width(&font_caption, menu_icons[i]);
        font_draw(d, rect_clip(item, clip), &font_caption, item.x + (MENU_ICON_W - iw) / 2, ty, menu_icons[i], text);
        font_draw(d, rect_clip(item, clip), &font_ui, item.x + MENU_ICON_W, ty, menu_labels[i], text);
    }

    // the same etched groove as the dividers on the bar, lying down
    rect_t below = menu_item(d, MENU_POWER);
    rect_t sep   = {below.x + 2, below.y - MENU_SEP_H / 2 - 1, below.w - 4, 1};
    display_fill(d, rect_clip(sep, clip), COLOUR_SHADOW);
    sep.y++;
    display_fill(d, rect_clip(sep, clip), COLOUR_HILIGHT);
}

static void ctx_draw(display_t *d, rect_t clip) {
    rect_t r = ctx_rect(d);
    if (!rect_overlaps(r, clip))
        return;

    display_fill(d, rect_clip(r, clip), COLOUR_FACE);
    display_edge(d, clip, r, COLOUR_LIGHT, COLOUR_DKSHADOW);
    display_edge(d, clip, rect_shrink(r, 1), COLOUR_HILIGHT, COLOUR_SHADOW);

    for (int i = 0; i < CTX_ITEMS; i++) {
        rect_t item = ctx_item(d, i);
        if (i == ctx_hover)
            display_fill(d, rect_clip(item, clip), COLOUR_BUTTON_LIT);

        font_draw(d, rect_clip(item, clip), &font_ui, item.x + 10, item.y + (item.h - font_height(&font_ui)) / 2,
                  ctx_labels[i], COLOUR_TEXT);
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

    rect_t bar = taskbar_rect(d);
    clip = rect_clip(clip, bar);
    if (!clip.w || !clip.h)
        return;

    // raised along the top edge only, the other three sides are off the screen
    display_fill(d, clip, COLOUR_FACE);
    display_fill(d, rect_clip((rect_t){bar.x, bar.y, bar.w, 1}, clip), COLOUR_LIGHT);
    display_fill(d, rect_clip((rect_t){bar.x, bar.y + 1, bar.w, 1}, clip), COLOUR_HILIGHT);

    button_draw(d, clip, start_button(d), "Start", menu_open || (held == SLOT_START && held_over));
    divider_draw(d, clip, tasks_divider(d));

    // the window with the focus has its button held in, like 9x
    window_t *top = window_top();
    for (int i = 0; window_index(i); i++) {
        window_t *win    = window_index(i);
        rect_t    button = task_button(d, win);
        if (!task_fits(d, button))
            continue;

        char label[8];
        slot_label(win->slot, label);
        button_draw(d, clip, button, label, win == top || (held == win->slot && held_over));
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
    font_draw(d, rect_clip(tray, clip), &font_ui, text_x, tray.y + (tray.h - font_height(&font_ui)) / 2,
              clock_text, COLOUR_TEXT);
}

// which button is at x y, as the value held would have for it
static int button_at(const display_t *d, int x, int y) {
    if (rect_contains(start_button(d), x, y))
        return SLOT_START;

    window_t *win = task_at(d, x, y);
    return win ? win->slot : SLOT_NONE;
}

int taskbar_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons) {
    int left      = buttons & MOUSE_BTN_LEFT;
    int was_left  = was_buttons & MOUSE_BTN_LEFT;
    int right     = buttons & MOUSE_BTN_RIGHT;
    int was_right = was_buttons & MOUSE_BTN_RIGHT;

    int on_bar  = rect_contains(taskbar_rect(d), x, y);
    int on_warn = mem_warn && rect_contains(warn_icon(d), x, y);

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
            int run = (item == ctx_pressed) ? item : MENU_NONE;
            ctx_set(d, 0, 0);
            if (run != MENU_NONE)
                ctx_run(run);
            return 1;
        }

        // any other press puts it away and still counts as a press on whats there
        if ((left && !was_left) || (right && !was_right))
            ctx_set(d, 0, 0);
        else if (in_ctx || ctx_pressed != MENU_NONE)
            return 1;
    }

    if (right && !was_right && held == SLOT_NONE && rect_contains(start_button(d), x, y)) {
        menu_set(d, 0);
        ctx_set(d, 1, x);
        return 1;
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
        held      = button_at(d, x, y);
        held_over = 1;
        display_damage(d, taskbar_rect(d));
        return 1;
    }

    if (held != SLOT_NONE) {
        int over = (button_at(d, x, y) == held);
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
