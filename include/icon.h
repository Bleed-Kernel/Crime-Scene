#pragma once
#include <stdint.h>

typedef struct icon {
    uint32_t *pixels;
    int       w, h;
} icon_t;

// the 16px icon built into the program at path, NULL if none
const icon_t *icon_for_path(const char *path);

const icon_t *icon_for_path_res(const char *path, const char *res);

const icon_t *icon_for_task(uint64_t pid);
