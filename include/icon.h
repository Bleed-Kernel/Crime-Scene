#pragma once
#include <stdint.h>

typedef struct icon {
    uint32_t *pixels;
    int       w, h;
} icon_t;

// the programs own icon at px (shrunk from a bigger one if it has to be), NULL if it hasnt got one
const icon_t *icon_find(const char *path, int px);

// the desktops standard icon for programs without one
const icon_t *icon_default(int px);

// icon_find, or the standard one if the program has none
const icon_t *icon_for_path(const char *path, int px);

const icon_t *icon_for_task(uint64_t pid, int px);
