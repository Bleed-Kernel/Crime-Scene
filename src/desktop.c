#include <stdlib.h>
#include <string.h>
#include <mouse.h>
#include <desktop.h>
#include <apps.h>
#include <window.h>
#include <taskbar.h>
#include <clock.h>
#include <font.h>
#include <theme.h>
#include <crime/crime.h>

#define CELL_W          84
#define CELL_H          76
#define MARGIN          14      // from the edges of the screen
#define ICON_TOP        8       // the icon down from the top of its cell
#define LABEL_GAP       6       // between the icon and its name
#define LABEL_PAD       3       // round the name when its picked out
#define DRAG_START      4       // how far it has to move before its a drag and not a click

#define DOUBLE_CLICK_FS (400 * FS_PER_MS)

typedef struct {
    app_t         app;
    const icon_t *icon;     // the big one, or the small one if it hasnt got one
    int           x, y;     // top left of its cell
} item_t;

static item_t  *items;
static int      count;
static int      picked = -1;
static int      held   = -1;
static int      held_dx, held_dy;
static int      held_from_x, held_from_y;
static int      moved;          // its being dragged, not clicked
static int      clicked = -1;
static uint64_t clicked_at;

static rect_t cell(int i) {
    return (rect_t){items[i].x, items[i].y, CELL_W, CELL_H};
}

void desktop_init(display_t *d) {
    apps_scan();
    count = apps_count();
    items = calloc((size_t)(count ? count : 1), sizeof(item_t));
    if (!items) {
        count = 0;
        return;
    }

    int rows = (d->height - TASKBAR_H - MARGIN * 2) / CELL_H;
    if (rows < 1)
        rows = 1;

    for (int i = 0; i < count; i++) {
        items[i].app  = *apps_index(i);
        items[i].icon = icon_for_path_res(items[i].app.path, CS_RES_ICON32);
        if (!items[i].icon)
            items[i].icon = items[i].app.icon;
        items[i].x = MARGIN + (i / rows) * CELL_W;
        items[i].y = MARGIN + (i % rows) * CELL_H;
    }
}

// as much of name as fits in max_w, with .. on the end if it had to be cut
static void name_fit(const char *name, int max_w, char out[CS_TITLE_MAX]) {
    strncpy(out, name, CS_TITLE_MAX - 1);
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

void desktop_draw(display_t *d, rect_t clip) {
    for (int i = 0; i < count; i++) {
        rect_t r = cell(i);
        if (!rect_overlaps(r, clip))
            continue;

        const icon_t *icon = items[i].icon;
        int           ty   = r.y + ICON_TOP + (icon ? icon->h : 32) + LABEL_GAP;
        if (icon)
            display_blend(d, rect_clip(r, clip), r.x + (r.w - icon->w) / 2, r.y + ICON_TOP, icon->pixels, icon->w,
                          icon->h);

        char name[CS_TITLE_MAX];
        name_fit(items[i].app.name, r.w - LABEL_PAD * 2, name);
        int tw = font_text_width(&font_ui, name);
        int tx = r.x + (r.w - tw) / 2;

        // picked out its name goes on the title bar red, otherwise its got a shadow so it reads on any wallpaper
        if (i == picked) {
            rect_t label = {tx - LABEL_PAD, ty - 1, tw + LABEL_PAD * 2, font_height(&font_ui) + 2};
            display_fill(d, rect_clip(label, clip), COLOUR_CAPTION_R);
        } else {
            font_draw(d, rect_clip(r, clip), &font_ui, tx + 1, ty + 1, name, COLOUR_DKSHADOW);
        }
        font_draw(d, rect_clip(r, clip), &font_ui, tx, ty, name, COLOUR_TEXT);
    }
}

static int item_at(int x, int y) {
    for (int i = count - 1; i >= 0; i--) {
        if (rect_contains(cell(i), x, y))
            return i;
    }
    return -1;
}

static void pick(display_t *d, int i) {
    if (i == picked)
        return;
    if (picked >= 0)
        display_damage(d, cell(picked));
    picked = i;
    if (picked >= 0)
        display_damage(d, cell(picked));
}

// kept on the screen and off the taskbar
static void move_to(display_t *d, int i, int x, int y) {
    int bottom = d->height - TASKBAR_H - CELL_H;
    if (x < 0)                   x = 0;
    if (x > d->width - CELL_W)   x = d->width - CELL_W;
    if (y < 0)                   y = 0;
    if (y > bottom)              y = bottom;

    display_damage(d, cell(i));
    items[i].x = x;
    items[i].y = y;
    display_damage(d, cell(i));
}

int desktop_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons) {
    int left     = buttons & MOUSE_BTN_LEFT;
    int was_left = was_buttons & MOUSE_BTN_LEFT;

    if (held >= 0) {
        if (left) {
            int dx = x - held_from_x;
            int dy = y - held_from_y;
            if (!moved && dx * dx + dy * dy >= DRAG_START * DRAG_START)
                moved = 1;
            if (moved)
                move_to(d, held, x - held_dx, y - held_dy);
            return 1;
        }

        // let go without moving is a click, two of them close together starts it
        if (!moved) {
            uint64_t now = clock_now();
            if (clicked == held && now - clicked_at < DOUBLE_CLICK_FS) {
                clicked = -1;
                app_start(d, &items[held].app);
            } else {
                clicked    = held;
                clicked_at = now;
            }
        }
        held = -1;
        return 1;
    }

    // only a press on the bare desktop is ours
    if (!left || was_left || window_at(x, y) || rect_contains(taskbar_rect(d), x, y))
        return 0;

    int i = item_at(x, y);
    pick(d, i);
    if (i >= 0) {
        held        = i;
        held_dx     = x - items[i].x;
        held_dy     = y - items[i].y;
        held_from_x = x;
        held_from_y = y;
        moved       = 0;
    }
    return 1;
}
