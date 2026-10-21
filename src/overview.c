#include <stdlib.h>
#include <string.h>
#include <mouse.h>
#include <overview.h>
#include <window.h>
#include <taskbar.h>
#include <clock.h>
#include <anim.h>
#include <font.h>
#include <theme.h>

#define MOVE_MS         200
#define MOVE_FS         (MOVE_MS * FS_PER_MS)

#define PAD             40      // round the grid
#define GAP             28      // between the cells
#define LABEL_H         26      // under each one for its title
#define RING            3       // round the one thats picked
#define DIM             150     // out of 256, how much darker the wallpaper goes

#define KEY_ESCAPE      0x01
#define KEY_TAB         0x0F
#define KEY_ENTER       0x1C
#define KEY_ARROW_UP    0x48
#define KEY_ARROW_DOWN  0x50
#define KEY_ARROW_LEFT  0x4B
#define KEY_ARROW_RIGHT 0x4D

enum { OVERVIEW_GONE, OVERVIEW_IN, OVERVIEW_UP, OVERVIEW_OUT };

typedef struct {
    uint32_t  win;
    uint32_t *pixels;
    int       w, h;
    rect_t    from;         // where the window is
    rect_t    to;           // its place in the grid
    char      title[CS_TITLE_MAX];
} thumb_t;

static int       state;
static float     done;          // 0 where the windows are, 1 in the grid
static uint64_t  move_start;
static thumb_t  *thumbs;
static int       count;
static int       cols;
static int       picked;
static uint32_t  chosen;        // brought to the front once its gone

static rect_t work_rect(const display_t *d) {
    return (rect_t){0, 0, d->width, d->height - TASKBAR_H};
}

// as near square as it can be, the thumbnails as big as they can be and keep their shape
static void layout(const display_t *d) {
    cols = 1;
    while (cols * cols < count)
        cols++;
    int rows = (count + cols - 1) / cols;

    rect_t area   = rect_shrink(work_rect(d), PAD);
    int    cell_w = (area.w - (cols - 1) * GAP) / cols;
    int    cell_h = (area.h - (rows - 1) * GAP) / rows;

    for (int i = 0; i < count; i++) {
        int row = i / cols;
        int col = i % cols;

        // the last row is short, its in the middle
        int in_row = row == rows - 1 ? count - row * cols : cols;
        int left   = area.x + (area.w - (in_row * cell_w + (in_row - 1) * GAP)) / 2;

        thumb_t *t     = &thumbs[i];
        int      max_h = cell_h - LABEL_H;
        int      w     = t->w;
        int      h     = t->h;
        if (w > cell_w) { h = h * cell_w / w; w = cell_w; }
        if (h > max_h)  { w = w * max_h / h;  h = max_h; }

        int x = left + col * (cell_w + GAP) + (cell_w - w) / 2;
        int y = area.y + row * (cell_h + GAP) + (max_h - h) / 2;
        t->to = (rect_t){x, y, w, h};
    }
}

static void thumbs_free(void) {
    for (int i = 0; i < count; i++)
        free(thumbs[i].pixels);
    free(thumbs);
    thumbs = NULL;
    count  = 0;
}

void overview_open(display_t *d) {
    if (state == OVERVIEW_UP && count) {
        picked = (picked + 1) % count;
        display_damage(d, work_rect(d));
        return;
    }
    if (state != OVERVIEW_GONE)
        return;

    int total = 0;
    while (window_index(total))
        total++;
    thumbs = calloc((size_t)(total ? total : 1), sizeof(thumb_t));
    if (!thumbs)
        return;

    // bottom to top, so the one in front ends up last
    for (int i = 0; window_index(i); i++) {
        window_t *win = window_index(i);

        thumb_t *t = &thumbs[count];
        t->pixels  = window_snapshot(win, 1, &t->w, &t->h);
        if (!t->pixels)
            continue;
        t->win  = win->id;
        t->from = win->minimised ? taskbar_button_of(d, win) : window_frame(win);
        memcpy(t->title, win->title, CS_TITLE_MAX);
        count++;
    }
    layout(d);

    // the one behind the front one, so straight back out swaps the two like alt tab
    picked     = count >= 2 ? count - 2 : 0;
    chosen     = 0;
    state      = OVERVIEW_IN;
    done       = 0;
    move_start = clock_now();
    display_damage(d, (rect_t){0, 0, d->width, d->height});
}

int overview_up(void) {
    return state != OVERVIEW_GONE;
}

static void leave(display_t *d, uint32_t pick) {
    if (state != OVERVIEW_UP && state != OVERVIEW_IN)
        return;
    chosen     = pick;
    state      = OVERVIEW_OUT;
    move_start = clock_now();
    display_damage(d, (rect_t){0, 0, d->width, d->height});
}

static int thumb_at(int x, int y) {
    for (int i = 0; i < count; i++) {
        rect_t r = thumbs[i].to;
        r.h += LABEL_H;
        if (rect_contains(r, x, y))
            return i;
    }
    return -1;
}

void overview_mouse(display_t *d, int x, int y, uint8_t buttons) {
    static uint8_t last;
    int pressed = (buttons & MOUSE_BTN_LEFT) && !(last & MOUSE_BTN_LEFT);
    last = buttons;
    if (state != OVERVIEW_UP)
        return;

    int i = thumb_at(x, y);
    if (i >= 0 && i != picked) {
        picked = i;
        display_damage(d, work_rect(d));
    }

    // a click on one picks it, anywhere else just goes back
    if (pressed)
        leave(d, i >= 0 ? thumbs[i].win : 0);
}

void overview_key(display_t *d, const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN || state != OVERVIEW_UP)
        return;

    int was = picked;
    switch (ev->keycode) {
        case KEY_ESCAPE:
            leave(d, 0);
            return;
        case KEY_ENTER:
            leave(d, count ? thumbs[picked].win : 0);
            return;
        case KEY_TAB:
            if (count)
                picked = (ev->keymod & KEYMOD_SHIFT) ? (picked + count - 1) % count : (picked + 1) % count;
            break;
        case KEY_ARROW_RIGHT: if (picked + 1 < count)     picked++;        break;
        case KEY_ARROW_LEFT:  if (picked > 0)             picked--;        break;
        case KEY_ARROW_DOWN:  if (picked + cols < count)  picked += cols;  break;
        case KEY_ARROW_UP:    if (picked - cols >= 0)     picked -= cols;  break;
    }
    if (picked != was)
        display_damage(d, work_rect(d));
}

void overview_tick(display_t *d) {
    if (state == OVERVIEW_IN || state == OVERVIEW_OUT)
        display_damage(d, (rect_t){0, 0, d->width, d->height});
}

int overview_wait_ms(void) {
    return state == OVERVIEW_IN || state == OVERVIEW_OUT ? 1 : -1;
}

static void gone(display_t *d) {
    window_t *win = window_find(chosen);
    if (win && win->minimised)
        window_restore(d, win);
    else if (win)
        window_raise(d, win);

    thumbs_free();
    state = OVERVIEW_GONE;
    display_damage(d, (rect_t){0, 0, d->width, d->height});
}

void overview_frame(display_t *d) {
    if (state != OVERVIEW_IN && state != OVERVIEW_OUT)
        return;

    uint64_t since = clock_now() - move_start;
    float    t     = since >= MOVE_FS ? 1.0f : anim_ease((float)since / (float)MOVE_FS);
    done = state == OVERVIEW_IN ? t : 1.0f - t;

    if (since < MOVE_FS)
        return;
    if (state == OVERVIEW_IN)
        state = OVERVIEW_UP;
    else
        gone(d);
}

void overview_draw(display_t *d, rect_t clip) {
    if (state == OVERVIEW_GONE || !clip.w || !clip.h)
        return;

    // the windows go but the taskbar stays
    windows_background(d, rect_clip(clip, work_rect(d)));
    display_dim(d, clip, 256 - (uint32_t)(DIM * done));

    for (int i = 0; i < count; i++) {
        thumb_t *t = &thumbs[i];
        rect_t   r = rect_lerp(t->from, t->to, done);
        if (state == OVERVIEW_UP && i == picked) {
            rect_t ring = rect_shrink(r, -RING - 1);
            for (int k = 0; k < RING; k++)
                display_outline(d, clip, rect_shrink(ring, k), COLOUR_CAPTION_R);
        }
        display_blit_scaled(d, clip, r, t->pixels, t->w, t->h, 256);

        if (state != OVERVIEW_UP)
            continue;
        rect_t   label = {t->to.x, t->to.y + t->to.h + RING + 4, t->to.w, LABEL_H - RING - 4};
        uint32_t ink   = i == picked ? COLOUR_TEXT : COLOUR_BACK_TEXT;
        int      tw    = font_text_width(&font_caption, t->title);
        font_draw(d, rect_clip(label, clip), &font_caption, label.x + (label.w - tw) / 2,
                  label.y + (label.h - font_height(&font_caption)) / 2, t->title, ink);
    }

    if (!count && state == OVERVIEW_UP) {
        const char *none = "No windows open";
        rect_t      work = work_rect(d);
        font_draw(d, clip, &font_caption, (work.w - font_text_width(&font_caption, none)) / 2, work.h / 2, none,
                  COLOUR_BACK_TEXT);
    }
}
