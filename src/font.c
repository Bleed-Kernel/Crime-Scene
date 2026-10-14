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

int font_load(font_t *out, const char *path, int px) {
    size_t   size;
    uint8_t *file = file_read(path, &size);
    if (!file)
        return -1;

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
        int glyph = ttf_glyph(&ttf, (uint32_t)(FONT_FIRST + i));
        int w, h, left, top;
        uint8_t *bits = ttf_render(&ttf, glyph, scale, &w, &h, &left, &top);

        int advance = round_px((float)ttf_advance(&ttf, glyph) * scale);
        glyphs[i].advance = (uint8_t)(advance > 255 ? 255 : advance);
        if (!bits)
            continue;

        // more than a glyph_t can say, the size asked for is silly
        if (w > 255 || h > 255 || left < -128 || left > 127 || ascent - top < -128 || ascent - top > 127) {
            free(bits);
            failed = 1;
            break;
        }

        size_t bytes = (size_t)w * h;
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

        glyphs[i].w      = (uint8_t)w;
        glyphs[i].h      = (uint8_t)h;
        glyphs[i].left   = (int8_t)left;
        glyphs[i].top    = (int8_t)(ascent - top);
        glyphs[i].offset = (uint32_t)used;
        used += bytes;
    }
    free(file);

    if (failed || !coverage) {
        free(glyphs);
        free(coverage);
        return -1;
    }

    out->line_height = ascent + descent;
    out->ascent      = ascent;
    out->glyphs      = glyphs;
    out->coverage    = coverage;
    return 0;
}

int font_load_role(font_t *out, const char *role, int px) {
    int dir = _open(FONT_DIR, O_RDONLY);
    if (dir < 0)
        return -1;

    dirent_t entry;
    char     path[sizeof(FONT_DIR) + sizeof(entry.name) + 1];
    size_t   role_len = strlen(role);
    int      found    = 0;

    for (size_t i = 0; !found && _readdir(dir, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        size_t len = strlen(entry.name);
        if (len < role_len || strcmp(entry.name + len - role_len, role) != 0)
            continue;

        strcpy(path, FONT_DIR "/");
        strcat(path, entry.name);
        found = 1;
    }
    _close(dir);

    return found ? font_load(out, path, px) : -1;
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

// y is the top of the line, not the baseline
void font_draw(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text, uint32_t colour) {
    clip = rect_clip(clip, (rect_t){0, 0, d->width, d->height});

    for (; *text; text++) {
        const font_glyph_t *g    = glyph_for(font, *text);
        const uint8_t      *bits = font->coverage + g->offset;
        int                 gx   = x + g->left;
        int                 gy   = y + g->top;

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
        x += g->advance;
    }
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
