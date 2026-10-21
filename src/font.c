#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <font.h>
#include <file.h>
#include <ttf.h>

font_t font_caption;
font_t font_ui;
font_t font_banner;
font_t font_version;

static int round_px(float v) {
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

typedef struct font_extra {
    uint32_t      cp;
    font_glyph_t  glyph;
    uint8_t      *bits;     // glyph.w by glyph.h of coverage, NULL if theres nothing to it
} font_extra_t;

// bits is NULL for a space. -1 if its too big for a glyph_t
static int glyph_render(const ttf_t *ttf, float scale, int ascent, uint32_t cp, font_glyph_t *g, uint8_t **bits) {
    int glyph = ttf_glyph(ttf, cp);
    int w, h, left, top;

    *g    = (font_glyph_t){0};
    *bits = ttf_render(ttf, glyph, scale, &w, &h, &left, &top);

    int advance = round_px((float)ttf_advance(ttf, glyph) * scale);
    g->advance = (uint8_t)(advance > 255 ? 255 : advance);
    if (!*bits)
        return 0;

    if (w > 255 || h > 255 || left < -128 || left > 127 || ascent - top < -128 || ascent - top > 127) {
        free(*bits);
        *bits = NULL;
        return -1;
    }

    g->w    = (uint8_t)w;
    g->h    = (uint8_t)h;
    g->left = (int8_t)left;
    g->top  = (int8_t)(ascent - top);
    return 0;
}

static int font_build(font_t *out, uint8_t *file, size_t size, int px, int keep) {
    ttf_t ttf;
    if (ttf_open(&ttf, file, size) < 0) {
        free(file);
        return -1;
    }

    float scale   = (float)px / (float)ttf.units;
    int   ascent  = round_px((float)ttf.ascent * scale);
    int   descent = round_px((float)ttf.descent * scale);

    font_glyph_t *glyphs   = calloc(FONT_GLYPHS, sizeof(font_glyph_t));
    uint8_t      *coverage = NULL;
    size_t        used     = 0;
    size_t        cap      = 0;
    int           failed   = !glyphs;

    for (int i = 0; i < FONT_GLYPHS && !failed; i++) {
        uint8_t *bits;
        if (glyph_render(&ttf, scale, ascent, (uint32_t)(FONT_FIRST + i), &glyphs[i], &bits) < 0) {
            failed = 1;
            break;
        }
        if (!bits)
            continue;

        size_t bytes = (size_t)glyphs[i].w * glyphs[i].h;
        if (used + bytes > cap) {
            cap = (used + bytes) * 2;
            uint8_t *bigger = realloc(coverage, cap);
            if (!bigger) {
                free(bits);
                failed = 1;
                break;
            }
            coverage = bigger;
        }
        memcpy(coverage + used, bits, bytes);
        free(bits);

        glyphs[i].offset = (uint32_t)used;
        used += bytes;
    }

    if (failed || !coverage) {
        free(file);
        free(glyphs);
        free(coverage);
        return -1;
    }

    *out = (font_t){
        .line_height = ascent + descent,
        .ascent      = ascent,
        .glyphs      = glyphs,
        .coverage    = coverage,
    };
    if (keep) {
        out->file  = file;
        out->ttf   = ttf;
        out->scale = scale;
    } else {
        free(file);
    }
    return 0;
}

int font_load(font_t *out, const char *path, int px) {
    size_t   size;
    uint8_t *file = file_read(path, &size);
    if (!file)
        return -1;
    return font_build(out, file, size, px, 0);
}

// the file in FONT_DIR whose name ends in role into path, 0 if theres one
static int font_find(const char *role, char *path, size_t path_size) {
    int dir = _open(FONT_DIR, O_RDONLY);
    if (dir < 0)
        return -1;

    dirent_t entry;
    size_t   role_len = strlen(role);
    int      found    = 0;

    for (size_t i = 0; !found && _readdir(dir, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        size_t len = strlen(entry.name);
        if (len < role_len || strcmp(entry.name + len - role_len, role) != 0)
            continue;
        if (sizeof(FONT_DIR) + len + 1 > path_size)
            continue;

        strcpy(path, FONT_DIR "/");
        strcat(path, entry.name);
        found = 1;
    }
    _close(dir);
    return found ? 0 : -1;
}

int font_open_role(font_t *out, const char *role, int px) {
    char path[256];
    if (font_find(role, path, sizeof(path)) < 0)
        return -1;

    size_t   size;
    uint8_t *file = file_read(path, &size);
    if (!file)
        return -1;
    return font_build(out, file, size, px, 1);
}

int font_load_role(font_t *out, const char *role, int px) {
    char path[256];
    if (font_find(role, path, sizeof(path)) < 0)
        return -1;
    return font_load(out, path, px);
}

void font_free(font_t *font) {
    for (int i = 0; i < font->extra_cap; i++)
        free(font->extra[i].bits);
    free(font->extra);
    free(font->file);
    free((void *)font->glyphs);
    free((void *)font->coverage);
    *font = (font_t){0};
}

// the font for role, or the ui one at that size if theres no file for it
static int load_or_ui(font_t *out, const char *role, int px) {
    if (font_load_role(out, role, px) == 0)
        return 0;
    return font_load_role(out, FONT_UI_ROLE, px);
}

int font_init(void) {
    if (font_load_role(&font_ui, FONT_UI_ROLE, FONT_UI_PX) < 0)
        return -1;
    if (load_or_ui(&font_caption, FONT_CAPTION_ROLE, FONT_CAPTION_PX) < 0)
        return -1;
    if (load_or_ui(&font_banner, FONT_BOLD_ROLE, FONT_BANNER_PX) < 0)
        return -1;
    return load_or_ui(&font_version, FONT_THIN_ROLE, FONT_VERSION_PX);
}

static const font_glyph_t *glyph_for(const font_t *font, char c) {
    if (c < FONT_FIRST || c >= FONT_FIRST + FONT_GLYPHS)
        c = '?';
    return &font->glyphs[c - FONT_FIRST];
}

int font_height(const font_t *font) {
    return font->line_height;
}

int font_text_width(const font_t *font, const char *text) {
    int width = 0;
    for (; *text; text++)
        width += glyph_for(font, *text)->advance;
    return width;
}

// red and blue go through together, theres a spare byte between them for the multiply to
// spill into. adding 1 and the top byte before the shift is a divide by 255
static inline uint32_t blend(uint32_t under, uint32_t over, uint32_t alpha) {
    if (alpha == 255)
        return over;

    uint32_t rb = (over & 0xFF00FF) * alpha + (under & 0xFF00FF) * (255 - alpha);
    uint32_t g  = ((over >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * (255 - alpha);

    rb = ((rb + 0x010001 + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    g  = (g + 1 + (g >> 8)) >> 8;
    return rb | (g << 8);
}

// clip has to be on the display already
static void glyph_draw(display_t *d, rect_t clip, const font_glyph_t *g, const uint8_t *bits, int x, int y,
                       uint32_t colour) {
    int gx = x + g->left;
    int gy = y + g->top;

    // only the part of the glyph clip shows, worked out once instead of asked per pixel
    int col0 = clip.x > gx ? clip.x - gx : 0;
    int row0 = clip.y > gy ? clip.y - gy : 0;
    int col1 = clip.x + clip.w - gx < g->w ? clip.x + clip.w - gx : g->w;
    int row1 = clip.y + clip.h - gy < g->h ? clip.y + clip.h - gy : g->h;

    for (int row = row0; row < row1; row++) {
        const uint8_t *cover = bits + row * g->w;
        uint32_t      *dst   = d->back + (size_t)(gy + row) * d->pitch + gx;

        for (int col = col0; col < col1; col++) {
            if (cover[col])
                dst[col] = blend(dst[col], colour, cover[col]);
        }
    }
}

// y is the top of the line, not the baseline
void font_draw(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text, uint32_t colour) {
    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});

    for (; *text; text++) {
        const font_glyph_t *g = glyph_for(font, *text);
        glyph_draw(d, clip, g, font->coverage + g->offset, x, y, colour);
        x += g->advance;
    }
}

// every digit gets as much room as the widest one
static int digit_advance(const font_t *font) {
    int widest = 0;
    for (char c = '0'; c <= '9'; c++) {
        if (glyph_for(font, c)->advance > widest)
            widest = glyph_for(font, c)->advance;
    }
    return widest;
}

int font_digits_width(const font_t *font, const char *text) {
    int width = 0;
    int digit = digit_advance(font);
    for (; *text; text++)
        width += (*text >= '0' && *text <= '9') ? digit : glyph_for(font, *text)->advance;
    return width;
}

void font_draw_digits(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text,
                      uint32_t colour) {
    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});
    int digit = digit_advance(font);

    for (; *text; text++) {
        const font_glyph_t *g = glyph_for(font, *text);
        if (*text >= '0' && *text <= '9') {
            // in the middle of its room, a narrow 1 doesnt pull the rest along
            glyph_draw(d, clip, g, font->coverage + g->offset, x + (digit - g->advance) / 2, y, colour);
            x += digit;
        } else {
            glyph_draw(d, clip, g, font->coverage + g->offset, x, y, colour);
            x += g->advance;
        }
    }
}

static font_extra_t *extra_slot(font_extra_t *table, int cap, uint32_t cp) {
    uint32_t i = (cp * 2654435761u) & (uint32_t)(cap - 1);
    while (table[i].cp && table[i].cp != cp)
        i = (i + 1) & (uint32_t)(cap - 1);
    return &table[i];
}

// doubled once its three quarters full
static int extra_grow(font_t *font) {
    if ((font->extra_count + 1) * 4 <= font->extra_cap * 3)
        return 0;

    int           cap   = font->extra_cap ? font->extra_cap * 2 : 64;
    font_extra_t *table = calloc((size_t)cap, sizeof(font_extra_t));
    if (!table)
        return -1;

    for (int i = 0; i < font->extra_cap; i++) {
        if (font->extra[i].cp)
            *extra_slot(table, cap, font->extra[i].cp) = font->extra[i];
    }
    free(font->extra);
    font->extra     = table;
    font->extra_cap = cap;
    return 0;
}

// cp past ascii, drawn the first time and kept
static const font_extra_t *extra_for(font_t *font, uint32_t cp) {
    if (!font->file || !cp)
        return NULL;

    if (font->extra_cap) {
        font_extra_t *e = extra_slot(font->extra, font->extra_cap, cp);
        if (e->cp)
            return e;
    }
    if (extra_grow(font) < 0)
        return NULL;

    // one that wont draw is kept too, with nothing in it
    font_extra_t *e = extra_slot(font->extra, font->extra_cap, cp);
    e->cp = cp;
    glyph_render(&font->ttf, font->scale, font->ascent, cp, &e->glyph, &e->bits);
    font->extra_count++;
    return e;
}

void font_draw_cp(display_t *d, rect_t clip, font_t *font, int x, int y, uint32_t cp, uint32_t colour) {
    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});

    if (cp >= FONT_FIRST && cp < FONT_FIRST + FONT_GLYPHS) {
        const font_glyph_t *g = &font->glyphs[cp - FONT_FIRST];
        glyph_draw(d, clip, g, font->coverage + g->offset, x, y, colour);
        return;
    }

    const font_extra_t *e = extra_for(font, cp);
    if (!e) {
        const font_glyph_t *g = glyph_for(font, '?');
        glyph_draw(d, clip, g, font->coverage + g->offset, x, y, colour);
        return;
    }
    if (e->bits)
        glyph_draw(d, clip, &e->glyph, e->bits, x, y, colour);
}

// along the line becomes up the screen, down the line becomes across it
void font_draw_up(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text, uint32_t colour) {
    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});

    int pen = 0;
    for (; *text; text++) {
        const font_glyph_t *g    = glyph_for(font, *text);
        const uint8_t      *bits = font->coverage + g->offset;

        for (int row = 0; row < g->h; row++) {
            for (int col = 0; col < g->w; col++) {
                uint8_t alpha = bits[row * g->w + col];
                int     px    = x + g->top + row;
                int     py    = y - 1 - (pen + g->left + col);
                if (!alpha || !rect_contains(clip, px, py))
                    continue;

                uint32_t *dst = &d->back[py * d->pitch + px];
                *dst = blend(*dst, colour, alpha);
            }
        }
        pen += g->advance;
    }
}
