#include <string.h>
#include <stdlib.h>
#include <syscalls.h>
#include <window.h>
#include <font.h>
#include <theme.h>
#include <taskbar.h>
#include <wallpaper.h>
#include <crime/sys.h>

#define SHADOW_REACH        11
#define SHADOW_DROP         3           // the shadow sits a little below the window
#define SHADOW_FOCUSED      150
#define SHADOW_BACK         80

#define WINDOW_SIZE_MAX 4096

#define STACK_GROW      32          // room for this many windows to start with, doubled when its full

// every window there is, bottom to top. the top one has the focus
static window_t **stack;
static int        stack_count;
static int        stack_cap;
static uint32_t  next_id = 1;
static int       cascade;               // how many steps from the corner the last new window went

rect_t window_content(const window_t *win) {
    return (rect_t){win->x, win->y, win->w, win->h};
}

rect_t window_frame(const window_t *win) {
    return (rect_t){
        win->x - WINDOW_INSET,
        win->y - WINDOW_INSET_TOP,
        win->w + WINDOW_INSET * 2,
        win->h + WINDOW_INSET_TOP + WINDOW_INSET
    };
}

rect_t window_bounds(const window_t *win) {
    rect_t frame = window_frame(win);
    return (rect_t){frame.x - WINDOW_SHADOW, frame.y - WINDOW_SHADOW,
                    frame.w + WINDOW_SHADOW * 2, frame.h + WINDOW_SHADOW * 2};
}

rect_t window_titlebar(const window_t *win) {
    rect_t frame = window_frame(win);
    return (rect_t){frame.x + WINDOW_FRAME, frame.y + WINDOW_FRAME, frame.w - WINDOW_FRAME * 2, WINDOW_CAPTION};
}

// close on the far right, then a gap, then maximise and minimise side by side
rect_t window_button(const window_t *win, int which) {
    rect_t bar = window_titlebar(win);
    int    x   = bar.x + bar.w - 3 - WINDOW_BUTTON_W;

    if (which != WINDOW_BUTTON_CLOSE) x -= WINDOW_BUTTON_W + 2;
    if (which == WINDOW_BUTTON_MIN)   x -= WINDOW_BUTTON_W;
    return (rect_t){x, bar.y + 3, WINDOW_BUTTON_W, WINDOW_BUTTON_H};
}

int window_button_at(const window_t *win, int x, int y) {
    for (int which = WINDOW_BUTTON_MIN; which <= WINDOW_BUTTON_CLOSE; which++) {
        if (rect_contains(window_button(win, which), x, y))
            return which;
    }
    return 0;
}

// the frame hangs off every edge by its own width, so only the title bar and content show
rect_t window_maximised_rect(const display_t *d) {
    return (rect_t){
        WINDOW_INSET - WINDOW_FRAME,
        WINDOW_INSET_TOP - WINDOW_FRAME,
        d->width + WINDOW_FRAME * 2 - WINDOW_INSET * 2,
        d->height - TASKBAR_H + WINDOW_FRAME * 2 - WINDOW_INSET_TOP - WINDOW_INSET
    };
}

// the highest window thats actually on screen, thats the one with the focus
window_t *window_top(void) {
    for (int i = stack_count - 1; i >= 0; i--) {
        if (!stack[i]->minimised)
            return stack[i];
    }
    return NULL;
}

window_t *window_index(int index) {
    return (index >= 0 && index < stack_count) ? stack[index] : NULL;
}

window_t *window_find(uint32_t id) {
    if (!id) return NULL;

    for (int i = 0; i < stack_count; i++) {
        if (stack[i]->id == id)
            return stack[i];
    }
    return NULL;
}

window_t *window_at(int x, int y) {
    for (int i = stack_count - 1; i >= 0; i--) {
        if (!stack[i]->minimised && rect_contains(window_frame(stack[i]), x, y))
            return stack[i];
    }
    return NULL;
}

static int grow(int n) {
    return (n + WINDOW_GROW - 1) / WINDOW_GROW * WINDOW_GROW;
}

static uint32_t *buffer_new(int w, int h, uint64_t *shm_out) {
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    long   shm   = shm_create((bytes + CS_PAGE_SIZE - 1) / CS_PAGE_SIZE);
    if (shm <= 0)
        return NULL;

    long pixels = shm_map(shm, NULL);
    if (pixels <= 0) {
        shm_destroy(shm);
        return NULL;
    }

    *shm_out = (uint64_t)shm;
    return (uint32_t *)pixels;
}

static void buffer_fill(window_t *win, int x0, int y0, int w, int h, uint32_t colour) {
    for (int y = y0; y < y0 + h; y++) {
        uint32_t *row = win->pixels + (size_t)y * win->cap_w;
        for (int x = x0; x < x0 + w; x++)
            row[x] = colour;
    }
}

// where the next new window goes and, if it didnt ask for a size, how big it is. the same as
// windows does it but smaller: half the screen each way, each one a title bars height
// down and right of the last, starting over from the corner once one wouldnt fit
static void place(const display_t *d, int *x, int *y, int *w, int *h) {
    int work_w = d->width;
    int work_h = d->height - TASKBAR_H;

    if (*w <= 0 || *h <= 0) {
        *w = work_w / 2 - WINDOW_INSET * 2;
        *h = work_h / 2 - WINDOW_INSET_TOP - WINDOW_INSET;
    }

    int frame_w = *w + WINDOW_INSET * 2;
    int frame_h = *h + WINDOW_INSET_TOP + WINDOW_INSET;
    int step    = WINDOW_FRAME + WINDOW_CAPTION;

    cascade++;
    if (cascade * step + frame_w > work_w || cascade * step + frame_h > work_h)
        cascade = 1;

    *x = cascade * step + WINDOW_INSET;
    *y = cascade * step + WINDOW_INSET_TOP;
}

window_t *window_open(display_t *d, uint64_t owner, int w, int h, const char *title) {
    int x, y;
    place(d, &x, &y, &w, &h);

    if (w < 1 || h < 1 || w > WINDOW_SIZE_MAX || h > WINDOW_SIZE_MAX)
        return NULL;

    if (stack_count == stack_cap) {
        int        cap    = stack_cap ? stack_cap * 2 : STACK_GROW;
        window_t **bigger = realloc(stack, (size_t)cap * sizeof(window_t *));
        if (!bigger)
            return NULL;
        stack     = bigger;
        stack_cap = cap;
    }

    window_t *win = calloc(1, sizeof(window_t));
    if (!win)
        return NULL;

    // bigger than asked for, so growing the window a bit doesnt mean a whole new buffer
    int       cap_w = grow(w);
    int       cap_h = grow(h);
    uint64_t  shm;
    uint32_t *pixels = buffer_new(cap_w, cap_h, &shm);
    if (!pixels) {
        free(win);
        return NULL;
    }

    // white until the client draws something
    memset(pixels, 0xFF, (size_t)cap_w * cap_h * sizeof(uint32_t));

    // found before the id is set, so the window isnt counted against itself
    int slot = 0;
    for (int i = 0; i < stack_count; i++) {
        if (stack[i]->slot == slot) {
            slot++;
            i = -1;
        }
    }

    win->id     = next_id++;
    win->slot   = slot;
    win->owner  = owner;
    win->shm    = shm;
    win->pixels = pixels;
    win->w      = w;
    win->h      = h;
    win->cap_w  = cap_w;
    win->cap_h  = cap_h;
    win->x      = x;
    win->y      = y;

    strncpy(win->title, title, CS_TITLE_MAX - 1);
    win->title[CS_TITLE_MAX - 1] = '\0';

    // the old top one loses the focus, its title bar has to change
    if (window_top())
        display_damage(d, window_bounds(window_top()));

    stack[stack_count++] = win;
    display_damage(d, window_bounds(win));
    return win;
}

int window_resize(display_t *d, window_t *win, int x, int y, int w, int h) {
    if (w < WINDOW_MIN_W || h < WINDOW_MIN_H || w > WINDOW_SIZE_MAX || h > WINDOW_SIZE_MAX)
        return -1;

    // the client hasnt drawn at the new size yet, so any new area gets whatever was in
    // its corner. thats nearly always its background
    uint32_t fill  = win->pixels[(size_t)(win->h - 1) * win->cap_w + win->w - 1];
    int      old_w = win->w;
    int      old_h = win->h;

    display_damage(d, window_bounds(win));

    if (w > win->cap_w || h > win->cap_h) {
        int       cap_w = grow(w > win->cap_w ? w : win->cap_w);
        int       cap_h = grow(h > win->cap_h ? h : win->cap_h);
        uint64_t  shm;
        uint32_t *pixels = buffer_new(cap_w, cap_h, &shm);
        if (!pixels)
            return -1;

        for (int row = 0; row < old_h; row++)
            memcpy(pixels + (size_t)row * cap_w, win->pixels + (size_t)row * win->cap_w,
                   (size_t)old_w * sizeof(uint32_t));

        _munmap(win->pixels);
        shm_destroy(win->shm);
        win->pixels = pixels;
        win->shm    = shm;
        win->cap_w  = cap_w;
        win->cap_h  = cap_h;
    }

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;

    // the strip down the right and the one along the bottom, if it got bigger either way
    if (w > old_w)
        buffer_fill(win, old_w, 0, w - old_w, h < old_h ? h : old_h, fill);
    if (h > old_h)
        buffer_fill(win, 0, old_h, w, h - old_h, fill);

    display_damage(d, window_bounds(win));
    return 0;
}

static void stack_remove(window_t *win) {
    int i = 0;
    while (i < stack_count && stack[i] != win)
        i++;
    if (i == stack_count)
        return;

    for (; i < stack_count - 1; i++)
        stack[i] = stack[i + 1];
    stack_count--;
}

void window_close(display_t *d, window_t *win) {
    display_damage(d, window_bounds(win));
    stack_remove(win);

    _munmap(win->pixels);
    shm_destroy(win->shm);
    free(win);

    if (window_top())
        display_damage(d, window_bounds(window_top()));
}

void window_minimise(display_t *d, window_t *win) {
    if (win->minimised)
        return;

    display_damage(d, window_bounds(win));
    win->minimised = 1;

    // whatever is top now gets the focus, and has to look like it
    if (window_top())
        display_damage(d, window_bounds(window_top()));
}

void window_restore(display_t *d, window_t *win) {
    win->minimised = 0;
    window_raise(d, win);
    display_damage(d, window_bounds(win));
}

void window_raise(display_t *d, window_t *win) {
    if (window_top() == win)
        return;

    if (window_top())
        display_damage(d, window_bounds(window_top()));
    stack_remove(win);
    stack[stack_count++] = win;
    display_damage(d, window_bounds(win));
}

void window_move(display_t *d, window_t *win, int x, int y) {
    // enough of the title bar has to stay on screen to grab it again
    if (x > d->width - 48)  x = d->width - 48;
    if (x < 48 - win->w)    x = 48 - win->w;
    if (y > d->height - TASKBAR_H - 4) y = d->height - TASKBAR_H - 4;
    if (y < WINDOW_INSET_TOP) y = WINDOW_INSET_TOP;

    if (x == win->x && y == win->y)
        return;

    display_damage(d, window_bounds(win));
    win->x = x;
    win->y = y;
    display_damage(d, window_bounds(win));
}

static void fill(display_t *d, rect_t clip, rect_t r, uint32_t colour) {
    display_fill(d, rect_clip(r, clip), colour);
}

// the caption buttons as 9x drew them
#define GLYPH_W 16
#define GLYPH_H 14

static const char *button_glyphs[][GLYPH_H] = {
    [WINDOW_BUTTON_MIN] = {
        "                ",
        "                ",
        "                ",
        "                ",
        "                ",
        "                ",
        "                ",
        "                ",
        "                ",
        "    ######      ",
        "    ######      ",
        "                ",
        "                ",
        "                ",
    },
    [WINDOW_BUTTON_MAX] = {
        "                ",
        "                ",
        "   #########    ",
        "   #########    ",
        "   #       #    ",
        "   #       #    ",
        "   #       #    ",
        "   #       #    ",
        "   #       #    ",
        "   #       #    ",
        "   #########    ",
        "                ",
        "                ",
        "                ",
    },
    [WINDOW_BUTTON_CLOSE] = {
        "                ",
        "                ",
        "                ",
        "    ##    ##    ",
        "     ##  ##     ",
        "      ####      ",
        "       ##       ",
        "      ####      ",
        "     ##  ##     ",
        "    ##    ##    ",
        "                ",
        "                ",
        "                ",
        "                ",
    },
};

static const char *restore_glyph[GLYPH_H] = {
    "                ",
    "                ",
    "     ######     ",
    "     ######     ",
    "     #    #     ",
    "   ###### #     ",
    "   ###### #     ",
    "   #    ###     ",
    "   #    #       ",
    "   #    #       ",
    "   ######       ",
    "                ",
    "                ",
    "                ",
};

static void button_draw(display_t *d, rect_t clip, const window_t *win, int which) {
    rect_t r    = window_button(win, which);
    int    down = win->button_down == which;
    int    hot  = down || win->button_hover == which;

    // only close goes red, the other two just lift a little
    uint32_t face = COLOUR_BUTTON, ink = COLOUR_BUTTON_X;
    uint32_t hi = COLOUR_BUTTON_HI, lo = COLOUR_BUTTON_LO;
    uint32_t in_hi = COLOUR_BUTTON_IN_HI, in_lo = COLOUR_BUTTON_IN_LO;
    if (hot && which == WINDOW_BUTTON_CLOSE) {
        face  = down ? COLOUR_HOT_DOWN : COLOUR_HOT;
        hi    = COLOUR_HOT_HI;
        lo    = COLOUR_HOT_LO;
        in_hi = COLOUR_HOT_IN_HI;
        in_lo = COLOUR_HOT_IN_LO;
        ink   = COLOUR_CAPTION_TEXT;
    } else if (hot) {
        face = down ? COLOUR_FACE : COLOUR_BUTTON_LIT;
        ink  = COLOUR_CAPTION_TEXT;
    }

    // shading on windows
    fill(d, clip, r, face);
    if (down) {
        display_edge(d, clip, r, lo, hi);
        display_edge(d, clip, rect_shrink(r, 1), in_lo, face);
    } else {
        display_edge(d, clip, r, hi, lo);
        display_edge(d, clip, rect_shrink(r, 1), in_hi, in_lo);
    }

    const char **glyph = button_glyphs[which];
    if (which == WINDOW_BUTTON_MAX && win->maximised)
        glyph = restore_glyph;

    // the glyph moves down and right a pixel when its pushed in
    int gx = r.x + (r.w - GLYPH_W) / 2 + down;
    int gy = r.y + (r.h - GLYPH_H) / 2 + down;
    for (int row = 0; row < GLYPH_H; row++) {
        for (int col = 0; col < GLYPH_W; col++) {
            if (glyph[row][col] == '#')
                fill(d, clip, (rect_t){gx + col, gy + row, 1, 1}, ink);
        }
    }
}

static void window_draw(display_t *d, rect_t clip, const window_t *win, int focused) {
    rect_t frame = window_frame(win);
    clip = rect_clip(clip, window_bounds(win));
    if (!clip.w || !clip.h)
        return;

    rect_t caster = {frame.x, frame.y + SHADOW_DROP, frame.w, frame.h};
    display_shadow(d, clip, caster, frame, SHADOW_REACH, focused ? SHADOW_FOCUSED : SHADOW_BACK);

    // just the grey round the outside, the content covers the middle
    rect_t content = window_content(win);
    fill(d, clip, (rect_t){frame.x, frame.y, frame.w, content.y - frame.y}, COLOUR_FACE);
    fill(d, clip, (rect_t){frame.x, content.y + content.h, frame.w, WINDOW_INSET}, COLOUR_FACE);
    fill(d, clip, (rect_t){frame.x, content.y, WINDOW_INSET, content.h}, COLOUR_FACE);
    fill(d, clip, (rect_t){content.x + content.w, content.y, WINDOW_INSET, content.h}, COLOUR_FACE);
    display_edge(d, clip, frame, COLOUR_LIGHT, COLOUR_DKSHADOW);
    display_edge(d, clip, rect_shrink(frame, 1), COLOUR_HILIGHT, COLOUR_SHADOW);

    rect_t bar = window_titlebar(win);
    display_gradient(d, clip, bar,
                     focused ? COLOUR_CAPTION_L : COLOUR_BACK_L,
                     focused ? COLOUR_CAPTION_R : COLOUR_BACK_R, 0);

    uint32_t ink = focused ? COLOUR_CAPTION_TEXT : COLOUR_BACK_TEXT;
    // the title stops short of the buttons instead of running underneath them
    rect_t button = window_button(win, WINDOW_BUTTON_MIN);
    rect_t text   = {bar.x + 6, bar.y, 0, bar.h};
    text.w = button.x - 4 - text.x;
    font_draw(d, rect_clip(text, clip), &font_caption, text.x, bar.y + (bar.h - font_height(&font_caption)) / 2,
              win->title, ink);

    for (int which = WINDOW_BUTTON_MIN; which <= WINDOW_BUTTON_CLOSE; which++)
        button_draw(d, clip, win, which);

    rect_t sunken = rect_shrink(content, -WINDOW_EDGE);
    display_edge(d, clip, sunken, COLOUR_SHADOW, COLOUR_HILIGHT);
    display_edge(d, clip, rect_shrink(sunken, 1), COLOUR_DKSHADOW, COLOUR_LIGHT);
    display_blit(d, clip, win->x, win->y, win->pixels, win->w, win->h, win->cap_w);
}

// top is the highest window that could still show in clip. a frame is solid, so nothing below
// it gets drawn where it is, only in the strips of clip it leaves uncovered
static void compose(display_t *d, rect_t clip, int top) {
    if (!clip.w || !clip.h)
        return;

    while (top >= 0 && (stack[top]->minimised || !rect_overlaps(window_bounds(stack[top]), clip)))
        top--;
    if (top < 0) {
        wallpaper_draw(d, clip);
        return;
    }

    window_t *win  = stack[top];
    rect_t   solid = rect_clip(window_frame(win), clip);

    if (!solid.w || !solid.h) {
        // only its shadow reaches in here
        compose(d, clip, top - 1);
    } else {
        int right  = solid.x + solid.w;
        int bottom = solid.y + solid.h;

        compose(d, (rect_t){clip.x, clip.y, clip.w, solid.y - clip.y}, top - 1);
        compose(d, (rect_t){clip.x, bottom, clip.w, clip.y + clip.h - bottom}, top - 1);
        compose(d, (rect_t){clip.x, solid.y, solid.x - clip.x, solid.h}, top - 1);
        compose(d, (rect_t){right, solid.y, clip.x + clip.w - right, solid.h}, top - 1);
    }

    window_draw(d, clip, win, win == window_top());
}

void windows_compose(display_t *d, rect_t clip) {
    compose(d, clip, stack_count - 1);
}
