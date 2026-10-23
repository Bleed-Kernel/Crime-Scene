#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syscalls.h>
#include <icon.h>
#include <png.h>
#include <crime/crime.h>

#define ICON_CACHE      64
#define ICON_PATH_MAX   256
#define ICON_NAME_MAX   32

#define CABINET_PATH    "/initrd/bin/cabinet"
#define CABINET_DEVICE  ".bleed.cab.device"

typedef struct {
    char        path[ICON_PATH_MAX];
    const char *base;
    int         px;
    icon_t      icon;
    int         found;
} cached_t;

// the hand drawn sizes a program can carry next to its master, smallest first
static const int sizes[] = { 16, 32, 48 };
#define SIZES   (int)(sizeof(sizes) / sizeof(sizes[0]))

// a miss is kept too, so a program with no icon isnt read again for every window it opens
static cached_t cache[ICON_CACHE];
static int      cached;

static uint32_t *scale_down(const uint32_t *src, int sw, int sh, int dw, int dh) {
    uint32_t *out = malloc((size_t)dw * dh * sizeof(uint32_t));
    if (!out)
        return NULL;

    float fx = (float)sw / dw, fy = (float)sh / dh;
    for (int dy = 0; dy < dh; dy++) {
        float y0 = dy * fy, y1 = y0 + fy;
        for (int dx = 0; dx < dw; dx++) {
            float x0 = dx * fx, x1 = x0 + fx;
            float a = 0, r = 0, g = 0, b = 0;

            for (int sy = (int)y0; sy < sh && sy < y1; sy++) {
                float wy = (sy + 1 < y1 ? sy + 1 : y1) - (sy > y0 ? sy : y0);
                for (int sx = (int)x0; sx < sw && sx < x1; sx++) {
                    float    w  = wy * ((sx + 1 < x1 ? sx + 1 : x1) - (sx > x0 ? sx : x0));
                    uint32_t p  = src[sy * sw + sx];
                    float    pa = (p >> 24) * w;
                    a += pa;
                    r += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                }
            }

            uint32_t alpha = (uint32_t)(a / (fx * fy) + 0.5f);
            if (!alpha) {
                out[dy * dw + dx] = 0;
                continue;
            }
            out[dy * dw + dx] = (alpha > 255 ? 255 : alpha) << 24 | (uint32_t)(r / a + 0.5f) << 16 |
                                (uint32_t)(g / a + 0.5f) << 8 | (uint32_t)(b / a + 0.5f);
        }
    }
    return out;
}

static uint32_t *res_decode(const char *path, const char *base, int px, int *w, int *h) {
    char name[ICON_NAME_MAX];
    if (px)
        snprintf(name, sizeof(name), "%s%d", base, px);
    else
        snprintf(name, sizeof(name), "%s", base);

    size_t   size;
    uint8_t *png = cs_res_read(path, name, &size);
    if (!png)
        return NULL;
    uint32_t *pixels = png_decode(png, size, w, h);
    free(png);
    return pixels;
}

static int icon_load(const char *path, const char *base, int px, icon_t *out) {
    uint32_t *pixels = NULL;
    int       w, h;

    for (int i = 0; !pixels && i < SIZES; i++) {
        if (sizes[i] >= px)
            pixels = res_decode(path, base, sizes[i], &w, &h);
    }
    if (!pixels)
        pixels = res_decode(path, base, 0, &w, &h);
    for (int i = SIZES - 1; !pixels && i >= 0; i--)
        pixels = res_decode(path, base, sizes[i], &w, &h);
    if (!pixels)
        return 0;

    // the long side goes to px, a master that isnt square keeps its shape
    if (w > px || h > px) {
        int dw = w >= h ? px : (w * px + h / 2) / h;
        int dh = h >= w ? px : (h * px + w / 2) / w;
        dw = dw ? dw : 1;
        dh = dh ? dh : 1;

        uint32_t *small = scale_down(pixels, w, h, dw, dh);
        if (small) {
            free(pixels);
            pixels = small;
            w      = dw;
            h      = dh;
        }
    }

    out->pixels = pixels;
    out->w      = w;
    out->h      = h;
    return 1;
}

static const icon_t *icon_cached(const char *path, const char *base, int px) {
    if (!path || !path[0] || strlen(path) >= ICON_PATH_MAX)
        return NULL;

    for (int i = 0; i < cached; i++) {
        if (cache[i].px == px && strcmp(cache[i].base, base) == 0 && strcmp(cache[i].path, path) == 0)
            return cache[i].found ? &cache[i].icon : NULL;
    }

    // more programs than this is unlikely, any past it go without
    if (cached == ICON_CACHE)
        return NULL;

    cached_t *c = &cache[cached++];
    strcpy(c->path, path);
    c->base  = base;
    c->px    = px;
    c->found = icon_load(path, base, px, &c->icon);
    return c->found ? &c->icon : NULL;
}

const icon_t *icon_find(const char *path, int px) {
    return icon_cached(path, CS_RES_ICON, px);
}

// the default is built into the desktop itself
const icon_t *icon_default(int px) {
    static char self[ICON_PATH_MAX];
    if (!self[0] && task_path((uint64_t)_getpid(), self, sizeof(self)) != 0)
        return NULL;
    return icon_cached(self, CS_RES_DEFAULT, px);
}

const icon_t *icon_for_path(const char *path, int px) {
    const icon_t *icon = icon_find(path, px);
    return icon ? icon : icon_default(px);
}

const icon_t *icon_for_task(uint64_t pid, int px) {
    char path[ICON_PATH_MAX];
    if (task_path(pid, path, sizeof(path)) != 0)
        return icon_default(px);
    return icon_for_path(path, px);
}

const icon_t *icon_device(int px) {
    return icon_cached(CABINET_PATH, CABINET_DEVICE, px);
}
