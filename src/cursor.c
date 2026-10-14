#include <cursor.h>

#define CURSOR_W 12
#define CURSOR_H 19

#define SIZE_LONG  19   // the resize arrows, along the way they point
#define SIZE_SHORT 7
#define SIZE_DIAG  15

#define CURSOR_FILL 0x000000
#define CURSOR_EDGE 0xFFFFFF

// # is the body, . is the outline, the tip is the hotspot
static const char *cursor_shape[CURSOR_H] = {
    ".           ",
    "..          ",
    ".#.         ",
    ".##.        ",
    ".###.       ",
    ".####.      ",
    ".#####.     ",
    ".######.    ",
    ".#######.   ",
    ".########.  ",
    ".#########. ",
    ".######.....",
    ".###.##.    ",
    ".##..##.    ",
    ".#.  .##.   ",
    "..   .##.   ",
    ".     .##.  ",
    "      .##.  ",
    "       ..   ",
};

// the resize arrows are only drawn as their black part with a clear pixel all the way round,
// the white outline is worked out from that. up and down is this one on its side
static const char *size_h_shape[SIZE_SHORT] = {
    "                   ",
    "    #         #    ",
    "   ##         ##   ",
    "  ###############  ",
    "   ##         ##   ",
    "    #         #    ",
    "                   ",
};

// top left to bottom right, the other diagonal is this one mirrored
static const char *size_diag_shape[SIZE_DIAG] = {
    "               ",
    " ######        ",
    " #####         ",
    " ####          ",
    " #####         ",
    " ## ###        ",
    " #   ###       ",
    "      ###      ",
    "       ###   # ",
    "        ### ## ",
    "         ##### ",
    "          #### ",
    "         ##### ",
    "        ###### ",
    "               ",
};

static void shape_size(cursor_shape_t shape, int *w, int *h) {
    switch (shape) {
        case CURSOR_SIZE_H:    *w = SIZE_LONG;  *h = SIZE_SHORT; break;
        case CURSOR_SIZE_V:    *w = SIZE_SHORT; *h = SIZE_LONG;  break;
        case CURSOR_SIZE_NWSE:
        case CURSOR_SIZE_NESW: *w = SIZE_DIAG;  *h = SIZE_DIAG;  break;
        default:               *w = CURSOR_W;   *h = CURSOR_H;   break;
    }
}

static int size_body(cursor_shape_t shape, int col, int row) {
    int w, h;
    shape_size(shape, &w, &h);
    if (col < 0 || row < 0 || col >= w || row >= h)
        return 0;

    switch (shape) {
        case CURSOR_SIZE_H:    return size_h_shape[row][col] == '#';
        case CURSOR_SIZE_V:    return size_h_shape[col][row] == '#';
        case CURSOR_SIZE_NWSE: return size_diag_shape[row][col] == '#';
        default:               return size_diag_shape[row][w - 1 - col] == '#';
    }
}

// '#' body, '.' outline, anything else nothing
static char shape_pixel(cursor_shape_t shape, int col, int row) {
    if (shape == CURSOR_ARROW)
        return cursor_shape[row][col];

    if (size_body(shape, col, row))
        return '#';
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (size_body(shape, col + dx, row + dy))
                return '.';
        }
    }
    return ' ';
}

rect_t cursor_rect(const cursor_t *c) {
    int w, h;
    shape_size(c->shape, &w, &h);

    // the arrow points with its tip, the rest are held by the middle
    if (c->shape == CURSOR_ARROW)
        return (rect_t){c->x, c->y, w, h};
    return (rect_t){c->x - w / 2, c->y - h / 2, w, h};
}

void cursor_move(cursor_t *c, const display_t *d, int dx, int dy) {
    c->x += dx;
    c->y += dy;

    // only the tip has to stay on screen
    if (c->x < 0) c->x = 0;
    if (c->y < 0) c->y = 0;
    if (c->x > d->width - 1)  c->x = d->width - 1;
    if (c->y > d->height - 1) c->y = d->height - 1;
}

void cursor_draw(const cursor_t *c, display_t *d, rect_t clip) {
    rect_t whole = cursor_rect(c);

    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});
    rect_t r = rect_clip(whole, clip);

    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = d->back + y * d->pitch;

        for (int x = r.x; x < r.x + r.w; x++) {
            char p = shape_pixel(c->shape, x - whole.x, y - whole.y);
            if (p == '#') row[x] = CURSOR_FILL;
            if (p == '.') row[x] = CURSOR_EDGE;
        }
    }
}
