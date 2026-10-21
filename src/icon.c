#include <stdlib.h>
#include <string.h>
#include <icon.h>
#include <png.h>
#include <crime/crime.h>

#define ICON_CACHE      64
#define ICON_PATH_MAX   256

typedef struct {
    char        path[ICON_PATH_MAX];
    const char *res;
    icon_t      icon;
    int         found;
} cached_t;

// a miss is kept too, so a program with no icon isnt read again for every window it opens
static cached_t cache[ICON_CACHE];
static int      cached;

const icon_t *icon_for_path(const char *path) {
    return icon_for_path_res(path, CS_RES_ICON16);
}

const icon_t *icon_for_path_res(const char *path, const char *res) {
    if (!path || !path[0] || strlen(path) >= ICON_PATH_MAX)
        return NULL;

    for (int i = 0; i < cached; i++) {
        if (cache[i].res == res && strcmp(cache[i].path, path) == 0)
            return cache[i].found ? &cache[i].icon : NULL;
    }

    // more programs than this is unlikely, any past it go without
    if (cached == ICON_CACHE)
        return NULL;

    cached_t *c = &cache[cached++];
    strcpy(c->path, path);
    c->res = res;

    size_t   size;
    uint8_t *png = cs_res_read(path, res, &size);
    if (png) {
        c->icon.pixels = png_decode(png, size, &c->icon.w, &c->icon.h);
        free(png);
    }
    c->found = c->icon.pixels != NULL;
    return c->found ? &c->icon : NULL;
}

const icon_t *icon_for_task(uint64_t pid) {
    char path[ICON_PATH_MAX];
    if (task_path(pid, path, sizeof(path)) != 0)
        return NULL;
    return icon_for_path(path);
}
