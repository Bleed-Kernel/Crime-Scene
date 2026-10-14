#pragma once
#include <stdint.h>
#include <stddef.h>

// a truetype font file thats been read into memory. only the outlines kind.
typedef struct {
    const uint8_t *data;
    size_t         size;

    uint32_t glyf, loca, hmtx;
    uint32_t cmap;          // the one subtable thats used
    int      cmap_format;   // either 4 or 12

    int units;
    int loca_long;
    int glyphs;
    int hmetrics;

    int ascent;
    int descent;
} ttf_t;

// 0 if data is a font this can draw. data has to stay around as long as t does
int ttf_open(ttf_t *t, const uint8_t *data, size_t size);

// which glyph a character is, 0 is the fonts own missing glyph
int ttf_glyph(const ttf_t *t, uint32_t codepoint);

// how far the pen moves after a glyph, in font units
int ttf_advance(const ttf_t *t, int glyph);

// draw a glyph at scale pixels to the font unit. gives back w * h bytes of coverage for the
// caller to free, NULL for a glyph with nothing in it or if it went wrong.
uint8_t *ttf_render(const ttf_t *t, int glyph, float scale, int *w, int *h, int *left, int *top);
