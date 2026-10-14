#include <stdlib.h>
#include <wallpaper.h>
#include <file.h>
#include <png.h>

#define BACKGROUND 0x000000

static uint32_t *wallpaper;     // the size of the screen, NULL if there isnt one

// part way from a to b, t is out of 256. red and blue go through together
static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t rb = ((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8;
    uint32_t g  = ((a & 0x00FF00) * (256 - t) + (b & 0x00FF00) * t) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

// theres nothing behind the wallpaper to show through it, so see through parts go to black
static void flatten(uint32_t *px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t alpha = px[i] >> 24;
        px[i] = mix(BACKGROUND, px[i], alpha + (alpha >> 7));
    }
}

// half the size each way, every pixel the average of the four it replaces
static void halve(uint32_t *px, int *w, int *h) {
    int half_w = *w / 2;
    int half_h = *h / 2;

    for (int y = 0; y < half_h; y++) {
        const uint32_t *top    = px + (size_t)y * 2 * *w;
        const uint32_t *bottom = top + *w;
        uint32_t       *out    = px + (size_t)y * half_w;

        for (int x = 0; x < half_w; x++)
            out[x] = mix(mix(top[x * 2], top[x * 2 + 1], 128), mix(bottom[x * 2], bottom[x * 2 + 1], 128), 128);
    }
    *w = half_w;
    *h = half_h;
}

// where along the image a pixel of the screen lands, 16.16 and kept inside it
static int64_t sample(int64_t start, int64_t step, int i, int size) {
    int64_t at = start + step * i + step / 2 - 0x8000;
    if (at < 0) at = 0;
    if (at > (int64_t)(size - 1) << 16) at = (int64_t)(size - 1) << 16;
    return at;
}

// fills the screen and keeps its shape, whatever hangs over the edges is cut off evenly
static uint32_t *cover(const uint32_t *src, int sw, int sh, int dw, int dh) {
    uint32_t *dst = malloc((size_t)dw * dh * sizeof(uint32_t));
    if (!dst)
        return NULL;

    // image pixels to a screen pixel, 16.16. the side that only just fits is the one that sets it
    int64_t step = (int64_t)sw * dh < (int64_t)sh * dw ? ((int64_t)sw << 16) / dw : ((int64_t)sh << 16) / dh;
    int64_t left = (((int64_t)sw << 16) - step * dw) / 2;
    int64_t top  = (((int64_t)sh << 16) - step * dh) / 2;

    for (int y = 0; y < dh; y++) {
        int64_t         sy = sample(top, step, y, sh);
        int             y0 = (int)(sy >> 16);
        const uint32_t *r0 = src + (size_t)y0 * sw;
        const uint32_t *r1 = y0 + 1 < sh ? r0 + sw : r0;
        uint32_t        fy = (uint32_t)(sy >> 8) & 0xFF;

        for (int x = 0; x < dw; x++) {
            int64_t  sx = sample(left, step, x, sw);
            int      x0 = (int)(sx >> 16);
            int      x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t fx = (uint32_t)(sx >> 8) & 0xFF;

            dst[(size_t)y * dw + x] = mix(mix(r0[x0], r0[x1], fx), mix(r1[x0], r1[x1], fx), fy);
        }
    }
    return dst;
}

void wallpaper_init(const display_t *d) {
    size_t   size;
    uint8_t *file = file_read(WALLPAPER_PATH, &size);
    if (!file)
        return;

    int       w, h;
    uint32_t *image = png_decode(file, size, &w, &h);
    free(file);
    if (!image)
        return;

    flatten(image, (size_t)w * h);

    // shrinking by more than half in one go skips pixels and it shows, so get close in halves first
    while (w >= d->width * 2 && h >= d->height * 2)
        halve(image, &w, &h);

    wallpaper = cover(image, w, h, d->width, d->height);
    free(image);
}

void wallpaper_draw(display_t *d, rect_t clip) {
    if (!wallpaper) {
        display_fill(d, clip, BACKGROUND);
        return;
    }
    display_blit(d, clip, 0, 0, wallpaper, d->width, d->height, d->width);
}
