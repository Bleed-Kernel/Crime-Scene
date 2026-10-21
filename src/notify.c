#include <string.h>
#include <mouse.h>
#include <notify.h>
#include <clock.h>
#include <anim.h>
#include <font.h>
#include <theme.h>

#define CARDS           5       // more than this and the oldest goes to make room
#define CARD_W          300
#define CARD_PAD        12
#define CARD_GAP        8
#define CARD_MARGIN     12      // from the edges of the screen
#define CARD_EDGE       3       // the raised edge, the title bar is inside it
#define CARD_CAPTION    20

#define STAY_MS         5000
#define SLIDE_MS        220
#define STAY_FS         (STAY_MS * FS_PER_MS)
#define SLIDE_FS        (SLIDE_MS * FS_PER_MS)

enum { CARD_NONE, CARD_IN, CARD_UP, CARD_OUT };

typedef struct {
    int      state;
    char     title[48];
    char     text[96];
    uint64_t since;         // when it went into the state its in
    int      slide;         // how far off to the right it is
    rect_t   shown;         // where it was last drawn
} card_t;

// oldest first, theyre stacked down from the top in this order
static card_t cards[CARDS];
static int    count;

static int card_h(void) {
    return CARD_EDGE * 2 + CARD_CAPTION + CARD_PAD * 2 + font_height(&font_ui);
}

static rect_t card_rect(const display_t *d, int i) {
    return (rect_t){d->width - CARD_MARGIN - CARD_W + cards[i].slide, CARD_MARGIN + i * (card_h() + CARD_GAP),
                    CARD_W, card_h()};
}

// whats there from i down, theyre about to move up
static void damage_from(display_t *d, int i) {
    for (; i < count; i++)
        display_damage(d, card_rect(d, i));
}

static void card_remove(display_t *d, int i) {
    damage_from(d, i);
    memmove(&cards[i], &cards[i + 1], (size_t)(count - i - 1) * sizeof(card_t));
    count--;
    memset(&cards[count], 0, sizeof(card_t));
    damage_from(d, i);
}

static void copy(char *dst, size_t size, const char *src) {
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

void notify_post(display_t *d, const char *title, const char *text) {
    if (count == CARDS)
        card_remove(d, 0);

    card_t *c = &cards[count++];
    memset(c, 0, sizeof(*c));
    copy(c->title, sizeof(c->title), title);
    copy(c->text, sizeof(c->text), text);
    c->state = CARD_IN;
    c->since = clock_now();
    c->slide = CARD_W + CARD_MARGIN;
    c->shown = card_rect(d, count - 1);
}

static void leave(card_t *c) {
    if (c->state == CARD_OUT)
        return;
    c->state = CARD_OUT;
    c->since = clock_now();
}

static int sliding(void) {
    for (int i = 0; i < count; i++) {
        if (cards[i].state == CARD_IN || cards[i].state == CARD_OUT)
            return 1;
    }
    return 0;
}

void notify_tick(display_t *d) {
    uint64_t now = clock_now();
    for (int i = 0; i < count; i++) {
        if (cards[i].state == CARD_UP && now - cards[i].since >= STAY_FS)
            leave(&cards[i]);
        if (cards[i].state == CARD_IN || cards[i].state == CARD_OUT)
            display_damage(d, cards[i].shown);
    }
}

int notify_wait_ms(void) {
    if (sliding())
        return 1;

    int      wait = -1;
    uint64_t now  = clock_now();
    for (int i = 0; i < count; i++) {
        uint64_t gone = now - cards[i].since;
        int      ms   = gone >= STAY_FS ? 0 : (int)((STAY_FS - gone) / FS_PER_MS) + 1;
        if (wait < 0 || ms < wait)
            wait = ms;
    }
    return wait;
}

void notify_frame(display_t *d) {
    uint64_t now = clock_now();
    int      off = CARD_W + CARD_MARGIN;

    for (int i = 0; i < count; i++) {
        card_t *c = &cards[i];
        if (c->state != CARD_IN && c->state != CARD_OUT)
            continue;

        uint64_t since = now - c->since;
        float    t     = since >= SLIDE_FS ? 1.0f : anim_ease((float)since / (float)SLIDE_FS);
        c->slide = c->state == CARD_IN ? (int)((float)off * (1.0f - t)) : (int)((float)off * t);

        rect_t r = card_rect(d, i);
        display_damage(d, rect_union(c->shown, r));
        c->shown = r;

        if (since >= SLIDE_FS && c->state == CARD_IN) {
            c->state = CARD_UP;
            c->since = now;
        } else if (since >= SLIDE_FS) {
            card_remove(d, i);
            i--;
        }
    }
}

void notify_draw(display_t *d, rect_t clip) {
    for (int i = 0; i < count; i++) {
        rect_t r = card_rect(d, i);
        if (!rect_overlaps(r, clip))
            continue;

        display_raised(d, clip, r);
        rect_t bar = {r.x + CARD_EDGE, r.y + CARD_EDGE, r.w - CARD_EDGE * 2, CARD_CAPTION};
        display_gradient(d, clip, bar, COLOUR_CAPTION_L, COLOUR_CAPTION_R, 0);
        font_draw(d, rect_clip(rect_shrink(bar, 2), clip), &font_caption, bar.x + 6,
                  bar.y + (bar.h - font_height(&font_caption)) / 2, cards[i].title, COLOUR_CAPTION_TEXT);

        rect_t inside = rect_clip(rect_shrink(r, CARD_EDGE), clip);
        font_draw(d, inside, &font_ui, bar.x + CARD_PAD - CARD_EDGE, bar.y + bar.h + CARD_PAD, cards[i].text,
                  COLOUR_TEXT);
    }
}

int notify_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons) {
    for (int i = 0; i < count; i++) {
        if (!rect_contains(card_rect(d, i), x, y))
            continue;
        if ((buttons & MOUSE_BTN_LEFT) && !(was_buttons & MOUSE_BTN_LEFT))
            leave(&cards[i]);
        return 1;
    }
    return 0;
}
