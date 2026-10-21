#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <display.h>
#include <bleed/display.h>
#include <theme.h>

#define PAGE_SIZE 4096UL

typedef uint32_t px4_t __attribute__((vector_size(16), aligned(4), may_alias));

// memset can only do one byte repeated, a colour is four
static void pixels_fill(uint32_t *dst, uint32_t colour, size_t count) {
    px4_t four = {colour, colour, colour, colour};

    for (; count >= 16; count -= 16, dst += 16) {
        *(px4_t *)(dst)      = four;
        *(px4_t *)(dst + 4)  = four;
        *(px4_t *)(dst + 8)  = four;
        *(px4_t *)(dst + 12) = four;
    }
    for (; count >= 4; count -= 4, dst += 4)
        *(px4_t *)dst = four;
    while (count--)
        *dst++ = colour;
}

static void pixels_copy(uint32_t *dst, const uint32_t *src, size_t count) {
    for (; count >= 16; count -= 16, dst += 16, src += 16) {
        px4_t a = *(const px4_t *)(src);
        px4_t b = *(const px4_t *)(src + 4);
        px4_t c = *(const px4_t *)(src + 8);
        px4_t e = *(const px4_t *)(src + 12);
        *(px4_t *)(dst)      = a;
        *(px4_t *)(dst + 4)  = b;
        *(px4_t *)(dst + 8)  = c;
        *(px4_t *)(dst + 12) = e;
    }
    for (; count >= 4; count -= 4, dst += 4, src += 4)
        *(px4_t *)dst = *(const px4_t *)src;
    while (count--)
        *dst++ = *src++;
}

rect_t rect_clip(rect_t r, rect_t bounds) {
    int x0 = r.x > bounds.x ? r.x : bounds.x;
    int y0 = r.y > bounds.y ? r.y : bounds.y;
    int x1 = r.x + r.w < bounds.x + bounds.w ? r.x + r.w : bounds.x + bounds.w;
    int y1 = r.y + r.h < bounds.y + bounds.h ? r.y + r.h : bounds.y + bounds.h;

    if (x1 <= x0 || y1 <= y0)
        return (rect_t){0, 0, 0, 0};
    return (rect_t){x0, y0, x1 - x0, y1 - y0};
}

rect_t rect_union(rect_t a, rect_t b) {
    if (!a.w || !a.h) return b;
    if (!b.w || !b.h) return a;

    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (rect_t){x0, y0, x1 - x0, y1 - y0};
}

rect_t rect_shrink(rect_t r, int by) {
    return (rect_t){r.x + by, r.y + by, r.w - by * 2, r.h - by * 2};
}

int rect_contains(rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int rect_overlaps(rect_t a, rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

rect_t rect_lerp(rect_t a, rect_t b, float t) {
    return (rect_t){
        a.x + (int)((float)(b.x - a.x) * t), a.y + (int)((float)(b.y - a.y) * t),
        a.w + (int)((float)(b.w - a.w) * t), a.h + (int)((float)(b.h - a.h) * t),
    };
}


static rect_t display_bounds(const display_t *d) {
    return (rect_t){0, 0, d->width, d->height};
}

int display_init(display_t *d) {
    struct fb_info info;

    d->damage_count = 0;

    d->fd = _open("/dev/fb0", O_RDWR);
    if (d->fd < 0)
        return -1;
    if (_ioctl(d->fd, FB_IOC_GET_INFO, &info) < 0 || info.bpp != 32)
        return -1;

    d->width  = (int)info.width;
    d->height = (int)info.height;
    d->pitch  = (int)(info.pitch / 4);

    size_t fb_pages;
    d->front = mapfb(&fb_pages);
    d->back  = _mmap((info.pitch * info.height + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!d->front || !d->back)
        return -1;

    // last, after this the tty stops drawing and nobody would see an error
    if (_ioctl(d->fd, FB_IOC_ACQUIRE, NULL) < 0)
        return -1;
    return 0;
}

void display_shutdown(display_t *d) {
    _ioctl(d->fd, FB_IOC_RELEASE, NULL);
    _close(d->fd);
}

void display_fill(display_t *d, rect_t r, uint32_t colour) {
    r = rect_clip(r, display_bounds(d));

    for (int y = r.y; y < r.y + r.h; y++) {
        pixels_fill(d->back + (size_t)y * d->pitch + r.x, colour, (size_t)r.w);
    }
}

void display_dim(display_t *d, rect_t r, uint32_t keep) {
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = d->back + (size_t)y * d->pitch;
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t rb = ((row[x] & 0xFF00FF) * keep >> 8) & 0xFF00FF;
            uint32_t g  = ((row[x] & 0x00FF00) * keep >> 8) & 0x00FF00;
            row[x] = rb | g;
        }
    }
}

// the bottom right colour gets the two corners like the real thing
void display_edge(display_t *d, rect_t clip, rect_t r, uint32_t top_left, uint32_t bottom_right) {
    display_fill(d, rect_clip((rect_t){r.x, r.y, r.w - 1, 1}, clip), top_left);
    display_fill(d, rect_clip((rect_t){r.x, r.y, 1, r.h - 1}, clip), top_left);
    display_fill(d, rect_clip((rect_t){r.x, r.y + r.h - 1, r.w, 1}, clip), bottom_right);
    display_fill(d, rect_clip((rect_t){r.x + r.w - 1, r.y, 1, r.h}, clip), bottom_right);
}

// ordered dither, without it a slow gradient shows up as stripes
static const uint8_t bayer[4][4] = {
    {   0, 128,  32, 160 },
    { 192,  64, 224,  96 },
    {  48, 176,  16, 144 },
    { 240, 112, 208,  80 },
};

// where each channel starts and how far a step moves it, times 65536. the divide is done
// once here so a pixel only costs a multiply
typedef struct {
    int base[3];
    int step[3];
} gradient_t;

static uint32_t gradient_pixel(const gradient_t *g, int t, int dither) {
    uint32_t colour = 0;
    for (int i = 0; i < 3; i++) {
        int v = (((g->base[i] + g->step[i] * t) >> 8) + dither) >> 8;
        if (v > 255) v = 255;
        colour |= (uint32_t)v << (i * 8);
    }
    return colour;
}

// from at the left (or top) edge of r fading to the other colour, whatever part of it clip shows
void display_gradient(display_t *d, rect_t clip, rect_t r, uint32_t from, uint32_t to, int vertical) {
    clip = rect_clip(rect_clip(r, clip), display_bounds(d));
    int span = (vertical ? r.h : r.w) - 1;
    if (span < 1) span = 1;

    gradient_t g;
    for (int i = 0; i < 3; i++) {
        int a = (from >> (i * 8)) & 0xFF;
        int b = (to >> (i * 8)) & 0xFF;
        g.base[i] = a << 16;
        g.step[i] = ((b - a) << 16) / span;
    }

    for (int y = clip.y; y < clip.y + clip.h; y++) {
        uint32_t *row = d->back + (size_t)y * d->pitch;

        // going across, the dither is all that changes down the screen and it repeats every
        // 4 rows. so only the first 4 get worked out, the rest are copies of them
        if (!vertical && y - clip.y >= 4) {
            pixels_copy(row + clip.x, row + clip.x - 4 * (size_t)d->pitch, (size_t)clip.w);
            continue;
        }

        for (int x = clip.x; x < clip.x + clip.w; x++)
            row[x] = gradient_pixel(&g, vertical ? y - r.y : x - r.x, bayer[y & 3][x & 3]);
    }
}

void display_outline(display_t *d, rect_t clip, rect_t r, uint32_t colour) {
    display_edge(d, clip, r, colour, colour);
}

void display_button(display_t *d, rect_t clip, rect_t r, int down, int hot) {
    display_fill(d, rect_clip(r, clip), down ? COLOUR_LIGHT : hot ? COLOUR_BUTTON_LIT : COLOUR_FACE);
    if (down) {
        display_edge(d, clip, r, COLOUR_DKSHADOW, COLOUR_HILIGHT);
        display_edge(d, clip, rect_shrink(r, 1), COLOUR_SHADOW, COLOUR_LIGHT);
    } else {
        display_edge(d, clip, r, COLOUR_HILIGHT, COLOUR_DKSHADOW);
        display_edge(d, clip, rect_shrink(r, 1), COLOUR_LIGHT, COLOUR_SHADOW);
    }
}

void display_raised(display_t *d, rect_t clip, rect_t r) {
    display_fill(d, rect_clip(r, clip), COLOUR_FACE);
    display_edge(d, clip, r, COLOUR_LIGHT, COLOUR_DKSHADOW);
    display_edge(d, clip, rect_shrink(r, 1), COLOUR_HILIGHT, COLOUR_SHADOW);
}

void display_sunken(display_t *d, rect_t clip, rect_t r) {
    display_edge(d, clip, r, COLOUR_SHADOW, COLOUR_HILIGHT);
    display_edge(d, clip, rect_shrink(r, 1), COLOUR_DKSHADOW, COLOUR_LIGHT);
}

void display_blit_scaled(display_t *d, rect_t clip, rect_t to, const uint32_t *src, int w, int h, int alpha) {
    rect_t r = rect_clip(rect_clip(to, clip), display_bounds(d));
    if (!r.w || !r.h || !to.w || !to.h)
        return;

    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *in  = src + (size_t)((y - to.y) * h / to.h) * w;
        uint32_t       *out = d->back + (size_t)y * d->pitch;

        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t p = in[(x - to.x) * w / to.w];
            if (p == DISPLAY_CLEAR)
                continue;
            if (alpha >= 256) {
                out[x] = p & 0xFFFFFF;
                continue;
            }
            uint32_t a  = (uint32_t)alpha;
            uint32_t rb = ((out[x] & 0xFF00FF) * (256 - a) + (p & 0xFF00FF) * a) >> 8;
            uint32_t g  = ((out[x] & 0x00FF00) * (256 - a) + (p & 0x00FF00) * a) >> 8;
            out[x] = (rb & 0xFF00FF) | (g & 0x00FF00);
        }
    }
}

static int isqrt(int n) {
    int root = 0;
    while ((root + 1) * (root + 1) <= n)
        root++;
    return root;
}

#define SHADOW_REACH_MAX 16

// how dark the shadow is dx dy away from the caster, 255 touching it down to 0 at reach.
// worked out once, the square root is far too slow to do per pixel
static uint8_t shadow_fade[SHADOW_REACH_MAX][SHADOW_REACH_MAX];
static int     shadow_fade_reach;

static void shadow_fade_build(int reach) {
    for (int dy = 0; dy < reach; dy++) {
        for (int dx = 0; dx < reach; dx++) {
            int dist = isqrt(dx * dx + dy * dy);
            int left = dist < reach ? reach - dist : 0;
            shadow_fade[dy][dx] = (uint8_t)(255 * left * left / (reach * reach));
        }
    }
    shadow_fade_reach = reach;
}

static void shadow_row(display_t *d, rect_t caster, int strength, int y, int x0, int x1) {
    int dy = y < caster.y ? caster.y - y : y >= caster.y + caster.h ? y - (caster.y + caster.h - 1) : 0;
    uint32_t      *row  = d->back + (size_t)y * d->pitch;
    const uint8_t *fade = shadow_fade[dy];

    for (int x = x0; x < x1; x++) {
        int dx = x < caster.x ? caster.x - x : x >= caster.x + caster.w ? x - (caster.x + caster.w - 1) : 0;
        if (!fade[dx])
            continue;

        // the same as dividing by 255, without the divide
        uint32_t dark = (uint32_t)strength * fade[dx];
        uint32_t keep = 256 - ((dark + 1 + (dark >> 8)) >> 8);
        uint32_t rb   = ((row[x] & 0xFF00FF) * keep >> 8) & 0xFF00FF;
        uint32_t g    = ((row[x] & 0x00FF00) * keep >> 8) & 0x00FF00;
        row[x] = rb | g;
    }
}

// soft shadow round caster, darkest where it touches and gone by reach pixels out.
// strength is out of 255. hole is the part something solid gets drawn over anyway, its skipped
void display_shadow(display_t *d, rect_t clip, rect_t caster, rect_t hole, int reach, int strength) {
    if (reach > SHADOW_REACH_MAX) reach = SHADOW_REACH_MAX;
    if (reach != shadow_fade_reach)
        shadow_fade_build(reach);

    // one short of reach each way, the fade has already hit nothing by reach itself
    rect_t area = {caster.x - reach + 1, caster.y - reach + 1, caster.w + reach * 2 - 2, caster.h + reach * 2 - 2};
    area = rect_clip(rect_clip(area, clip), display_bounds(d));

    for (int y = area.y; y < area.y + area.h; y++) {
        int x0 = area.x;
        int x1 = area.x + area.w;

        if (y < hole.y || y >= hole.y + hole.h) {
            shadow_row(d, caster, strength, y, x0, x1);
            continue;
        }

        // level with the hole, only the bits either side of it
        int left  = hole.x < x1 ? hole.x : x1;
        int right = hole.x + hole.w > x0 ? hole.x + hole.w : x0;
        shadow_row(d, caster, strength, y, x0, left);
        shadow_row(d, caster, strength, y, right, x1);
    }
}

// src is w by h with its rows pitch pixels apart, it lands with its top left at x y
void display_blit(display_t *d, rect_t clip, int x, int y, const uint32_t *src, int w, int h, int pitch) {
    rect_t r = rect_clip(rect_clip((rect_t){x, y, w, h}, clip), display_bounds(d));

    for (int row = r.y; row < r.y + r.h; row++) {
        pixels_copy(d->back + (size_t)row * d->pitch + r.x,
                    src + (size_t)(row - y) * pitch + (r.x - x), (size_t)r.w);
    }
}

void display_blend(display_t *d, rect_t clip, int x, int y, const uint32_t *src, int w, int h) {
    rect_t r = rect_clip(rect_clip((rect_t){x, y, w, h}, clip), display_bounds(d));

    for (int row = r.y; row < r.y + r.h; row++) {
        uint32_t       *dst = d->back + (size_t)row * d->pitch;
        const uint32_t *in  = src + (size_t)(row - y) * w - x;

        for (int col = r.x; col < r.x + r.w; col++) {
            uint32_t a = in[col] >> 24;
            if (!a)
                continue;
            if (a == 255) {
                dst[col] = in[col] & 0xFFFFFF;
                continue;
            }

            uint32_t out = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                uint32_t over  = (in[col] >> shift) & 0xFF;
                uint32_t under = (dst[col] >> shift) & 0xFF;
                out |= ((over * a + under * (255 - a) + 127) / 255) << shift;
            }
            dst[col] = out;
        }
    }
}

static void display_flush(display_t *d, rect_t r) {
    for (int y = r.y; y < r.y + r.h; y++) {
        size_t off = (size_t)y * d->pitch + r.x;
        pixels_copy(d->front + off, d->back + off, (size_t)r.w);
    }
}

static void list_remove(rect_t *list, int *count, int i) {
    for (; i < *count - 1; i++)
        list[i] = list[i + 1];
    (*count)--;
}

static void list_add(rect_t *list, int *count, rect_t r) {
    // two rects that overlap get drawn twice where they do. if one rect round both is no
    // bigger than the pair then use that instead, a window moving a few pixels is this every frame
    for (int i = 0; i < *count; i++) {
        rect_t other = list[i];
        rect_t both  = rect_union(other, r);
        if (!rect_overlaps(other, r) || both.w * both.h > other.w * other.h + r.w * r.h)
            continue;

        // the bigger rect might swallow more of them now, start over with it
        list_remove(list, count, i);
        r = both;
        i = -1;
    }

    // out of room, fold it into the last one. bigger than it needs to be but still right
    if (*count == DAMAGE_MAX) {
        list[DAMAGE_MAX - 1] = rect_union(list[DAMAGE_MAX - 1], r);
        return;
    }

    // kept sorted by y so present writes down the screen the same way its scanned out
    int i = (*count)++;
    while (i > 0 && list[i - 1].y > r.y) {
        list[i] = list[i - 1];
        i--;
    }
    list[i] = r;
}

void display_damage(display_t *d, rect_t r) {
    r = rect_clip(r, display_bounds(d));
    if (!r.w || !r.h)
        return;
    list_add(d->damage, &d->damage_count, r);
}

void display_present(display_t *d) {
    for (int i = 0; i < d->damage_count; i++)
        display_flush(d, d->damage[i]);
    d->damage_count = 0;

    // the framebuffer is write combined, a row shorter than a cache line can sit in the cpu
    // instead of on the screen until this pushes it out
    asm volatile("sfence" ::: "memory");
}
