#pragma once
#include <stdint.h>

typedef struct {
    int x, y;
    int w, h;
} rect_t;

#define DAMAGE_MAX 16

typedef struct {
    int       fd;
    uint32_t *front;    // the real framebuffer
    uint32_t *back;     // everything gets drawn here first
    int       width;
    int       height;
    int       pitch;    // in pixels, not bytes

    rect_t    damage[DAMAGE_MAX];   // whats changed since the last present
    int       damage_count;
} display_t;

int  display_init(display_t *d);
void display_shutdown(display_t *d);

void display_fill(display_t *d, rect_t r, uint32_t colour);

// one pixel of 3d edge round r, the two colours are what make it look raised or sunken with some shading
void display_edge(display_t *d, rect_t clip, rect_t r, uint32_t top_left, uint32_t bottom_right);
void display_gradient(display_t *d, rect_t clip, rect_t r, uint32_t from, uint32_t to, int vertical);
void display_shadow(display_t *d, rect_t clip, rect_t caster, rect_t hole, int reach, int strength);
void display_blit(display_t *d, rect_t clip, int x, int y, const uint32_t *src, int w, int h, int pitch);
void display_damage(display_t *d, rect_t r);
void display_present(display_t *d);

rect_t rect_clip(rect_t r, rect_t bounds);
rect_t rect_shrink(rect_t r, int by);
int    rect_contains(rect_t r, int x, int y);
rect_t rect_union(rect_t a, rect_t b);
int    rect_overlaps(rect_t a, rect_t b);
