#include <string.h>
#include <mouse.h>
#include <dialog.h>
#include <apps.h>
#include <font.h>
#include <theme.h>
#include <taskbar.h>
#include <window.h>

#define DIALOG_W        330
#define DIALOG_PAD      12
#define FIELD_H         24
#define FIELD_PAD       5       // between the edge of the box and the text in it
#define BUTTON_W        84
#define BUTTON_H        26
#define BUTTON_GAP      8
#define TEXT_MAX        128

#define KEY_ENTER       '\n'
#define KEY_ESCAPE      27
#define KEY_BACKSPACE   '\b'

#define KEY_HOME        0x47
#define KEY_LEFT        0x4B
#define KEY_RIGHT       0x4D
#define KEY_END         0x4F
#define KEY_DELETE      0x53

enum { DIALOG_NONE, DIALOG_CONFIRM, DIALOG_RUN };
enum { BUTTON_NONE = -1, BUTTON_YES, BUTTON_CANCEL };

static int   kind;
static char  title[32];
static char  text[64];
static char  yes_label[16];
static void (*yes_fn)(display_t *d);

static char  typed[TEXT_MAX];       // whats in the run box, kept for next time like 9x does
static int   failed;                // the last thing typed in couldnt be found
static int   selected;              // whats there from last time is picked out, typing replaces it
static int   at;                    // where the caret is in typed
static int   scroll;                // first character showing in the box

static int   pressed = BUTTON_NONE;
static int   pressed_over;

// centred above the taskbar
static rect_t content_rect(const display_t *d) {
    int line = font_height(&font_ui);
    int h    = DIALOG_PAD * 3 + line + BUTTON_H;
    // the run box has the field and a line under it for saying it couldnt find it
    if (kind == DIALOG_RUN)
        h += FIELD_H + line + DIALOG_PAD;

    int work_h  = d->height - TASKBAR_H;
    int frame_h = h + WINDOW_INSET_TOP + WINDOW_INSET;
    return (rect_t){(d->width - DIALOG_W) / 2, (work_h - frame_h) / 2 + WINDOW_INSET_TOP, DIALOG_W, h};
}

static rect_t dialog_rect(const display_t *d) {
    return rect_shrink(window_frame_around(content_rect(d)), -WINDOW_SHADOW);
}

static rect_t field_rect(const display_t *d) {
    rect_t r = content_rect(d);
    int    y = r.y + DIALOG_PAD + font_height(&font_ui) + DIALOG_PAD / 2;
    return (rect_t){r.x + DIALOG_PAD, y, r.w - DIALOG_PAD * 2, FIELD_H};
}

static rect_t button_rect(const display_t *d, int which) {
    rect_t r = content_rect(d);
    int    x = r.x + r.w - DIALOG_PAD - BUTTON_W;
    if (which == BUTTON_YES)
        x -= BUTTON_W + BUTTON_GAP;
    return (rect_t){x, r.y + r.h - DIALOG_PAD - BUTTON_H, BUTTON_W, BUTTON_H};
}

static int button_at(const display_t *d, int x, int y) {
    if (rect_contains(button_rect(d, BUTTON_YES), x, y))    return BUTTON_YES;
    if (rect_contains(button_rect(d, BUTTON_CANCEL), x, y)) return BUTTON_CANCEL;
    return BUTTON_NONE;
}

static void set(display_t *d, int to) {
    if (kind)
        display_damage(d, dialog_rect(d));
    kind    = to;
    pressed = BUTTON_NONE;
    if (kind)
        display_damage(d, dialog_rect(d));
}

static void copy(char *dst, size_t size, const char *src) {
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

void dialog_confirm(display_t *d, const char *t, const char *message, const char *yes, void (*on_yes)(display_t *d)) {
    set(d, DIALOG_NONE);
    copy(title, sizeof(title), t);
    copy(text, sizeof(text), message);
    copy(yes_label, sizeof(yes_label), yes);
    yes_fn = on_yes;
    set(d, DIALOG_CONFIRM);
}

void dialog_run(display_t *d) {
    set(d, DIALOG_NONE);
    copy(title, sizeof(title), "Run");
    copy(text, sizeof(text), "Type the name of a program, or its path.");
    copy(yes_label, sizeof(yes_label), "OK");
    failed   = 0;
    selected = typed[0] != '\0';
    at       = strlen(typed);
    scroll   = 0;
    set(d, DIALOG_RUN);
}

int dialog_up(void) {
    return kind != DIALOG_NONE;
}

static void answer(display_t *d, int which) {
    if (which == BUTTON_CANCEL) {
        set(d, DIALOG_NONE);
        return;
    }

    if (kind == DIALOG_CONFIRM) {
        void (*fn)(display_t *d) = yes_fn;
        set(d, DIALOG_NONE);
        if (fn)
            fn(d);
        return;
    }

    if (app_run_line(d, typed) == 0) {
        failed = 0;
        set(d, DIALOG_NONE);
    } else {
        failed = 1;
        display_damage(d, dialog_rect(d));
    }
}

static void button_draw(display_t *d, rect_t clip, rect_t r, const char *label, int down) {
    display_button(d, clip, r, down, 0);

    int tx = r.x + (r.w - font_text_width(&font_ui, label)) / 2 + down;
    int ty = r.y + (r.h - font_height(&font_ui)) / 2 + down;
    font_draw(d, rect_clip(rect_shrink(r, 2), clip), &font_ui, tx, ty, label, COLOUR_TEXT);
}

static int text_width(const char *s, int n) {
    char part[TEXT_MAX];
    memcpy(part, s, n);
    part[n] = '\0';
    return font_text_width(&font_ui, part);
}

void dialog_draw(display_t *d, rect_t clip) {
    if (!kind)
        return;
    if (!rect_overlaps(dialog_rect(d), clip))
        return;

    rect_t r = content_rect(d);
    window_chrome_draw(d, clip, r, title);
    display_fill(d, rect_clip(r, clip), COLOUR_FACE);

    int x = r.x + DIALOG_PAD;
    int y = r.y + DIALOG_PAD;
    font_draw(d, rect_clip(r, clip), &font_ui, x, y, text, COLOUR_TEXT);

    if (kind == DIALOG_RUN) {
        rect_t field = field_rect(d);
        display_fill(d, rect_clip(rect_shrink(field, 2), clip), COLOUR_SHADOW);
        display_sunken(d, clip, field);

        // scrolled just far enough to keep the caret in the box
        rect_t inside = rect_shrink(field, FIELD_PAD);
        int    room   = inside.w - 2;
        if (scroll > at)
            scroll = at;
        while (scroll < at && text_width(typed + scroll, at - scroll) > room)
            scroll++;
        while (scroll > 0 && font_text_width(&font_ui, typed + scroll - 1) <= room)
            scroll--;
        const char *shown = typed + scroll;

        int ty = field.y + (field.h - font_height(&font_ui)) / 2;
        if (selected) {
            rect_t highlight = {inside.x - 1, ty, font_text_width(&font_ui, shown) + 2, font_height(&font_ui)};
            display_fill(d, rect_clip(rect_clip(highlight, inside), clip), COLOUR_HOT_DOWN);
        }
        font_draw(d, rect_clip(inside, clip), &font_ui, inside.x, ty, shown, COLOUR_TEXT);

        rect_t caret = {inside.x + text_width(shown, at - scroll), ty, 1, font_height(&font_ui)};
        display_fill(d, rect_clip(rect_clip(caret, inside), clip), COLOUR_TEXT);

        if (failed)
            font_draw(d, rect_clip(r, clip), &font_ui, x, field.y + field.h + DIALOG_PAD / 2,
                      "Couldnt find that program.", COLOUR_LOW);
    }

    button_draw(d, clip, button_rect(d, BUTTON_YES), yes_label, pressed == BUTTON_YES && pressed_over);
    button_draw(d, clip, button_rect(d, BUTTON_CANCEL), "Cancel", pressed == BUTTON_CANCEL && pressed_over);
}

int dialog_mouse(display_t *d, int x, int y, uint8_t buttons, uint8_t was_buttons) {
    if (!kind)
        return 0;

    int left     = buttons & MOUSE_BTN_LEFT;
    int was_left = was_buttons & MOUSE_BTN_LEFT;

    if (left && !was_left) {
        pressed      = button_at(d, x, y);
        pressed_over = 1;
        if (pressed != BUTTON_NONE)
            display_damage(d, button_rect(d, pressed));
        return 1;
    }

    if (pressed == BUTTON_NONE)
        return 1;

    int over = button_at(d, x, y) == pressed;
    if (over != pressed_over) {
        pressed_over = over;
        display_damage(d, button_rect(d, pressed));
    }

    // only counts if it comes back up on the same button
    if (!left && was_left) {
        int which = pressed;
        display_damage(d, button_rect(d, pressed));
        pressed = BUTTON_NONE;
        if (over)
            answer(d, which);
    }
    return 1;
}

int dialog_key(display_t *d, const keyboard_event_t *ev) {
    if (!kind)
        return 0;
    if (ev->action != KEY_DOWN)
        return 1;

    char c = tty_key_to_ascii(ev);
    if (c == KEY_ENTER)  answer(d, BUTTON_YES);
    if (c == KEY_ESCAPE) answer(d, BUTTON_CANCEL);
    if (kind != DIALOG_RUN)
        return 1;

    int len = strlen(typed);
    int key = ev->keycode;
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END) {
        if (selected)
            at = (key == KEY_LEFT || key == KEY_HOME) ? 0 : len;
        else if (key == KEY_LEFT && at > 0)
            at--;
        else if (key == KEY_RIGHT && at < len)
            at++;
        else if (key == KEY_HOME)
            at = 0;
        else if (key == KEY_END)
            at = len;
        selected = 0;
        display_damage(d, dialog_rect(d));
        return 1;
    }

    // a key that changes it starts it over if the old text was picked out
    int printable = c >= ' ' && c < 127 && !(ev->keymod & (KEYMOD_CTRL | KEYMOD_ALT));
    if (selected && (printable || c == KEY_BACKSPACE || key == KEY_DELETE)) {
        typed[0] = '\0';
        at       = 0;
        len      = 0;
        selected = 0;
        if (!printable) {
            display_damage(d, dialog_rect(d));
            return 1;
        }
    }

    if (c == KEY_BACKSPACE && at > 0) {
        memmove(typed + at - 1, typed + at, len - at + 1);
        at--;
    } else if (key == KEY_DELETE && at < len) {
        memmove(typed + at, typed + at + 1, len - at);
    } else if (printable && len < TEXT_MAX - 1) {
        memmove(typed + at + 1, typed + at, len - at + 1);
        typed[at++] = c;
    } else {
        return 1;
    }

    failed = 0;
    display_damage(d, dialog_rect(d));
    return 1;
}
