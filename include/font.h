#pragma once
#include <stdint.h>
#include <display.h>
#include <ttf.h>

#define FONT_FIRST  32
#define FONT_GLYPHS 95

typedef struct {
    uint8_t  w, h;
    int8_t   left, top;     // where the bitmap sits from the pen and the top of the line
    uint8_t  advance;
    uint32_t offset;
} font_glyph_t;

struct font_extra;

typedef struct {
    int                 line_height;
    int                 ascent;
    const font_glyph_t *glyphs;
    const uint8_t      *coverage;

    uint8_t            *file;
    ttf_t               ttf;
    float               scale;
    struct font_extra  *extra;
    int                 extra_count;
    int                 extra_cap;
} font_t;

#define FONT_DIR            "/initrd/fonts"


// These are optional, if they dont exist then it just falls back to the standard UI font
#define FONT_UI_ROLE        "-ui.ttf"
#define FONT_CAPTION_ROLE   "-caption.ttf"
#define FONT_BOLD_ROLE      "-bold.ttf"
#define FONT_THIN_ROLE      "-thin.ttf"
#define FONT_MONO_ROLE      "-mono.ttf"

// how tall an em is in pixels
#define FONT_UI_PX          13
#define FONT_CAPTION_PX     13
#define FONT_BANNER_PX      20
#define FONT_VERSION_PX     14

extern font_t font_caption;     // window titles
extern font_t font_ui;          // everything else
extern font_t font_banner;      // the name up the side of the start menu
extern font_t font_version;     // and the version after it

// reads them all. -1 if theres no ui font to be had, theres nothing built in to fall back on
int  font_init(void);

// draw every glyph of the font at path into out. 0 if it worked, out is left alone if not
int  font_load(font_t *out, const char *path, int px);

// the same for whichever file in FONT_DIR has a name ending in role
int  font_load_role(font_t *out, const char *role, int px);

// the same as font_load_role but any character in the font can be drawn, with font_draw_cp
int  font_open_role(font_t *out, const char *role, int px);

// give back what font_load or font_open_role drew, for a font thats not one of the ones above
void font_free(font_t *font);

// y is the top of the line. a font from font_load only has ascii, anything else is a ?
void font_draw_cp(display_t *d, rect_t clip, font_t *font, int x, int y, uint32_t cp, uint32_t colour);

int  font_height(const font_t *font);
int  font_text_width(const font_t *font, const char *text);
void font_draw(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text, uint32_t colour);

// every digit the same width so a clock doesnt shuffle about as it goes
int  font_digits_width(const font_t *font, const char *text);
void font_draw_digits(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text,
                      uint32_t colour);

// the same text turned a quarter turn so it reads upwards. x is the left of the column the
// line stands in and y the bottom of it, where the text starts
void font_draw_up(display_t *d, rect_t clip, const font_t *font, int x, int y, const char *text, uint32_t colour);
