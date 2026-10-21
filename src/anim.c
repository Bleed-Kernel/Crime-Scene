#include <stdlib.h>
#include <string.h>
#include <anim.h>
#include <clock.h>

#define ANIMS           16      // more than this at once and the rest just happen

#define OPEN_MS         180
#define CLOSE_MS        160
#define MINIMISE_MS     220

#define OPEN_SCALE      235
#define CLOSE_SCALE     245
#define MINIMISED_ALPHA 64      // how much of it is left by the time it reaches the button

enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMISE, ANIM_RESTORE };

typedef struct {
    int       kind;
    uint32_t  win;          // the window, 0 once its gone
    uint32_t *pixels;       // its picture, the size of its frame
    int       w, h;
    rect_t    from, to;     // where the picture goes at the start and the end
    int       alpha_from, alpha_to;
    uint64_t  start, length;

    rect_t    now;          // where its going this frame
    int       alpha;
    rect_t    shown;        // where it went last frame, thats owed too
} anim_t;

static anim_t anims[ANIMS];

static rect_t scaled(rect_t r, int scale) {
    int w = r.w * scale / 256;
    int h = r.h * scale / 256;
    return (rect_t){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
}

float anim_ease(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static void anim_end(display_t *d, anim_t *a) {
    if (d)
        display_damage(d, a->shown);

    window_t *win = window_find(a->win);
    if (win && (a->kind == ANIM_OPEN || a->kind == ANIM_RESTORE)) {
        win->hidden = 0;
        if (d)
            display_damage(d, window_bounds(win));
    }
    free(a->pixels);
    memset(a, 0, sizeof(*a));
}

void anim_forget(window_t *win) {
    for (int i = 0; i < ANIMS; i++) {
        if (anims[i].kind && anims[i].win == win->id)
            anim_end(NULL, &anims[i]);
    }
    win->hidden = 0;
}

static anim_t *anim_new(display_t *d, window_t *win, int kind, uint64_t ms) {
    anim_forget(win);

    anim_t *a = NULL;
    for (int i = 0; i < ANIMS && !a; i++) {
        if (!anims[i].kind)
            a = &anims[i];
    }
    if (!a)
        return NULL;

    a->pixels = window_snapshot(win, win == window_focused() || kind == ANIM_OPEN || kind == ANIM_RESTORE,
                                &a->w, &a->h);
    if (!a->pixels)
        return NULL;

    a->kind   = kind;
    a->win    = win->id;
    a->start  = clock_now();
    a->length = ms * FS_PER_MS;
    display_damage(d, window_bounds(win));
    return a;
}

static void anim_begin(anim_t *a) {
    a->now   = a->from;
    a->alpha = a->alpha_from;
    a->shown = a->from;
}

void anim_open(display_t *d, window_t *win) {
    anim_t *a = anim_new(d, win, ANIM_OPEN, OPEN_MS);
    if (!a)
        return;
    rect_t frame  = window_frame(win);
    a->from       = scaled(frame, OPEN_SCALE);
    a->to         = frame;
    a->alpha_from = 0;
    a->alpha_to   = 256;
    win->hidden   = 1;
    anim_begin(a);
}

void anim_close(display_t *d, window_t *win) {
    anim_t *a = anim_new(d, win, ANIM_CLOSE, CLOSE_MS);
    if (!a)
        return;
    rect_t frame  = window_frame(win);
    a->from       = frame;
    a->to         = scaled(frame, CLOSE_SCALE);
    a->alpha_from = 256;
    a->alpha_to   = 0;
    a->win        = 0;
    anim_begin(a);
}

void anim_minimise(display_t *d, window_t *win, rect_t button) {
    anim_t *a = anim_new(d, win, ANIM_MINIMISE, MINIMISE_MS);
    if (!a)
        return;
    a->from       = window_frame(win);
    a->to         = button;
    a->alpha_from = 256;
    a->alpha_to   = MINIMISED_ALPHA;
    anim_begin(a);
}

void anim_restore(display_t *d, window_t *win, rect_t button) {
    anim_t *a = anim_new(d, win, ANIM_RESTORE, MINIMISE_MS);
    if (!a)
        return;
    a->from       = button;
    a->to         = window_frame(win);
    a->alpha_from = MINIMISED_ALPHA;
    a->alpha_to   = 256;
    win->hidden   = 1;
    anim_begin(a);
}

void anim_tick(display_t *d) {
    for (int i = 0; i < ANIMS; i++) {
        if (anims[i].kind)
            display_damage(d, anims[i].shown);
    }
}

int anim_wait_ms(void) {
    for (int i = 0; i < ANIMS; i++) {
        if (anims[i].kind)
            return 1;
    }
    return -1;
}

void anim_frame(display_t *d) {
    uint64_t now = clock_now();

    for (int i = 0; i < ANIMS; i++) {
        anim_t *a = &anims[i];
        if (!a->kind)
            continue;

        uint64_t since = now - a->start;
        if (since >= a->length) {
            anim_end(d, a);
            continue;
        }

        window_t *win = window_find(a->win);
        if (a->kind == ANIM_OPEN && win) {
            int       w, h;
            uint32_t *fresh = window_snapshot(win, 1, &w, &h);
            if (fresh) {
                free(a->pixels);
                a->pixels = fresh;
                a->w      = w;
                a->h      = h;
                a->to     = window_frame(win);
            }
        }

        float t  = anim_ease((float)since / (float)a->length);
        a->now   = rect_lerp(a->from, a->to, t);
        a->alpha = a->alpha_from + (int)((float)(a->alpha_to - a->alpha_from) * t);

        display_damage(d, rect_union(a->shown, a->now));
        a->shown = a->now;
    }
}

void anim_draw(display_t *d, rect_t clip) {
    for (int i = 0; i < ANIMS; i++) {
        anim_t *a = &anims[i];
        if (a->kind && rect_overlaps(a->now, clip))
            display_blit_scaled(d, clip, a->now, a->pixels, a->w, a->h, a->alpha);
    }
}
