#include <stdlib.h>
#include <string.h>
#include <ttf.h>

#define COMPOSITE_DEPTH 8       // glyphs made of glyphs made of glyphs, this is where it stops
#define GLYPH_PX_MAX    512     // nothing is drawn bigger than this either way

// composite glyph flags
#define ARGS_ARE_WORDS  0x0001
#define ARGS_ARE_XY     0x0002
#define HAVE_SCALE      0x0008
#define MORE_COMPONENTS 0x0020
#define HAVE_XY_SCALE   0x0040
#define HAVE_2X2        0x0080

// everything in the file is big endian, and none of it is trusted to stay inside the file
static uint32_t rd8(const ttf_t *t, uint32_t at) {
    return at < t->size ? t->data[at] : 0;
}

static uint32_t rd16(const ttf_t *t, uint32_t at) {
    return rd8(t, at) << 8 | rd8(t, at + 1);
}

static int rds16(const ttf_t *t, uint32_t at) {
    return (int16_t)rd16(t, at);
}

static uint32_t rd32(const ttf_t *t, uint32_t at) {
    return rd16(t, at) << 16 | rd16(t, at + 2);
}

static uint32_t table(const ttf_t *t, const char *tag) {
    uint32_t count = rd16(t, 4);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = 12 + i * 16;
        if (entry + 16 <= t->size && memcmp(t->data + entry, tag, 4) == 0)
            return rd32(t, entry + 8);
    }
    return 0;
}

// unicode with every character in it if the font has that, the 16 bit kind if not
static void cmap_pick(ttf_t *t, uint32_t cmap) {
    uint32_t count = rd16(t, cmap + 2);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t platform = rd16(t, cmap + 4 + i * 8);
        uint32_t encoding = rd16(t, cmap + 6 + i * 8);
        uint32_t sub      = cmap + rd32(t, cmap + 8 + i * 8);
        int      format   = (int)rd16(t, sub);

        int unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode || (format != 4 && format != 12))
            continue;
        if (format > t->cmap_format) {
            t->cmap        = sub;
            t->cmap_format = format;
        }
    }
}

int ttf_open(ttf_t *t, const uint8_t *data, size_t size) {
    memset(t, 0, sizeof(*t));
    t->data = data;
    t->size = size;

    // 0x00010000 or 'true', anything else isnt truetype outlines
    uint32_t kind = rd32(t, 0);
    if (kind != 0x00010000 && kind != 0x74727565)
        return -1;

    uint32_t head = table(t, "head");
    uint32_t hhea = table(t, "hhea");
    uint32_t maxp = table(t, "maxp");
    uint32_t cmap = table(t, "cmap");
    t->glyf = table(t, "glyf");
    t->loca = table(t, "loca");
    t->hmtx = table(t, "hmtx");
    if (!head || !hhea || !maxp || !cmap || !t->glyf || !t->loca || !t->hmtx)
        return -1;

    t->units     = (int)rd16(t, head + 18);
    t->loca_long = rds16(t, head + 50) != 0;
    t->glyphs    = (int)rd16(t, maxp + 4);
    t->ascent    = rds16(t, hhea + 4);
    t->descent   = -rds16(t, hhea + 6);
    t->hmetrics  = (int)rd16(t, hhea + 34);

    cmap_pick(t, cmap);
    if (!t->units || !t->glyphs || !t->hmetrics || !t->cmap_format)
        return -1;
    return 0;
}

static int glyph_format4(const ttf_t *t, uint32_t cp) {
    if (cp > 0xFFFF)
        return 0;

    uint32_t segs  = rd16(t, t->cmap + 6) / 2;
    uint32_t ends  = t->cmap + 14;
    uint32_t start = ends + segs * 2 + 2;
    uint32_t delta = start + segs * 2;
    uint32_t range = delta + segs * 2;

    for (uint32_t i = 0; i < segs; i++) {
        if (cp > rd16(t, ends + i * 2))
            continue;
        uint32_t first = rd16(t, start + i * 2);
        if (cp < first)
            return 0;

        uint32_t offset = rd16(t, range + i * 2);
        if (!offset)
            return (int)((cp + rd16(t, delta + i * 2)) & 0xFFFF);

        // the offset counts from where it was read, into the glyph ids after the segments
        uint32_t glyph = rd16(t, range + i * 2 + offset + (cp - first) * 2);
        return glyph ? (int)((glyph + rd16(t, delta + i * 2)) & 0xFFFF) : 0;
    }
    return 0;
}

static int glyph_format12(const ttf_t *t, uint32_t cp) {
    uint32_t groups = rd32(t, t->cmap + 12);

    for (uint32_t i = 0; i < groups; i++) {
        uint32_t group = t->cmap + 16 + i * 12;
        uint32_t first = rd32(t, group);
        if (cp >= first && cp <= rd32(t, group + 4))
            return (int)(rd32(t, group + 8) + (cp - first));
    }
    return 0;
}

int ttf_glyph(const ttf_t *t, uint32_t codepoint) {
    int glyph = t->cmap_format == 12 ? glyph_format12(t, codepoint) : glyph_format4(t, codepoint);
    return glyph < t->glyphs ? glyph : 0;
}

int ttf_advance(const ttf_t *t, int glyph) {
    // the last full entry stands for every glyph after it, thats how a monospace font is one entry
    if (glyph >= t->hmetrics)
        glyph = t->hmetrics - 1;
    return (int)rd16(t, t->hmtx + (uint32_t)glyph * 4);
}

// where a glyphs outline is in the file, 0 if it has none (a space)
static uint32_t glyph_at(const ttf_t *t, int glyph) {
    if (glyph < 0 || glyph >= t->glyphs)
        return 0;

    uint32_t from, to;
    if (t->loca_long) {
        from = rd32(t, t->loca + (uint32_t)glyph * 4);
        to   = rd32(t, t->loca + (uint32_t)glyph * 4 + 4);
    } else {
        from = rd16(t, t->loca + (uint32_t)glyph * 2) * 2;
        to   = rd16(t, t->loca + (uint32_t)glyph * 2 + 2) * 2;
    }
    return to > from ? t->glyf + from : 0;
}

typedef struct {
    float   x, y;
    uint8_t on;     // on the curve, or a control point pulling it
    uint8_t last;   // the contour ends here
} point_t;

typedef struct {
    point_t *points;
    int      count;
    int      cap;
} outline_t;

static point_t *outline_add(outline_t *o, int count) {
    if (o->count + count > o->cap) {
        int      cap    = (o->count + count) * 2;
        point_t *bigger = realloc(o->points, (size_t)cap * sizeof(point_t));
        if (!bigger)
            return NULL;
        o->points = bigger;
        o->cap    = cap;
    }

    point_t *at = o->points + o->count;
    o->count += count;
    return at;
}

// m is a 2x2 and a translation, how a composite places its parts
static int outline_simple(const ttf_t *t, uint32_t at, int contours, const float m[6], outline_t *o) {
    uint32_t ends   = at + 10;
    int      total  = (int)rd16(t, ends + (uint32_t)(contours - 1) * 2) + 1;
    uint32_t cursor = ends + (uint32_t)contours * 2;
    cursor += 2 + rd16(t, cursor);      // the hinting program, not run here

    if (total <= 0 || total > 0xFFFF || cursor >= t->size)
        return -1;

    uint8_t *flags = malloc((size_t)total);
    point_t *pts   = outline_add(o, total);
    if (!flags || !pts) {
        free(flags);
        return -1;
    }

    // a flag can say its repeated, the point of that being runs of the same flag
    for (int i = 0; i < total; ) {
        uint8_t flag   = (uint8_t)rd8(t, cursor++);
        int     repeat = (flag & 0x08) ? (int)rd8(t, cursor++) : 0;
        for (int n = 0; n <= repeat && i < total; n++)
            flags[i++] = flag;
    }

    // all the xs then all the ys, each one a step from the last
    int x = 0;
    for (int i = 0; i < total; i++) {
        if (flags[i] & 0x02) {
            int step = (int)rd8(t, cursor++);
            x += (flags[i] & 0x10) ? step : -step;
        } else if (!(flags[i] & 0x10)) {
            x += rds16(t, cursor);
            cursor += 2;
        }
        pts[i].x = (float)x;
    }

    int y = 0;
    for (int i = 0; i < total; i++) {
        if (flags[i] & 0x04) {
            int step = (int)rd8(t, cursor++);
            y += (flags[i] & 0x20) ? step : -step;
        } else if (!(flags[i] & 0x20)) {
            y += rds16(t, cursor);
            cursor += 2;
        }

        float px = pts[i].x;
        float py = (float)y;
        pts[i].x    = m[0] * px + m[2] * py + m[4];
        pts[i].y    = m[1] * px + m[3] * py + m[5];
        pts[i].on   = flags[i] & 0x01;
        pts[i].last = 0;
    }

    for (int c = 0; c < contours; c++) {
        int end = (int)rd16(t, ends + (uint32_t)c * 2);
        if (end < total)
            pts[end].last = 1;
    }
    pts[total - 1].last = 1;

    free(flags);
    return 0;
}

static float f2dot14(const ttf_t *t, uint32_t at) {
    return (float)rds16(t, at) / 16384.0f;
}

static int outline_glyph(const ttf_t *t, int glyph, const float m[6], outline_t *o, int depth);

static int outline_composite(const ttf_t *t, uint32_t at, const float m[6], outline_t *o, int depth) {
    uint32_t cursor = at + 10;
    uint32_t flags;

    do {
        flags = rd16(t, cursor);
        int part = (int)rd16(t, cursor + 2);
        cursor += 4;

        float dx = 0, dy = 0;
        if (flags & ARGS_ARE_WORDS) {
            if (flags & ARGS_ARE_XY) {
                dx = (float)rds16(t, cursor);
                dy = (float)rds16(t, cursor + 2);
            }
            cursor += 4;
        } else {
            if (flags & ARGS_ARE_XY) {
                dx = (float)(int8_t)rd8(t, cursor);
                dy = (float)(int8_t)rd8(t, cursor + 1);
            }
            cursor += 2;
        }

        float a = 1, b = 0, c = 0, d = 1;
        if (flags & HAVE_SCALE) {
            a = d = f2dot14(t, cursor);
            cursor += 2;
        } else if (flags & HAVE_XY_SCALE) {
            a = f2dot14(t, cursor);
            d = f2dot14(t, cursor + 2);
            cursor += 4;
        } else if (flags & HAVE_2X2) {
            a = f2dot14(t, cursor);
            b = f2dot14(t, cursor + 2);
            c = f2dot14(t, cursor + 4);
            d = f2dot14(t, cursor + 6);
            cursor += 8;
        }

        // the parts own placing first, then whatever placed us
        float sub[6] = {
            m[0] * a + m[2] * b,
            m[1] * a + m[3] * b,
            m[0] * c + m[2] * d,
            m[1] * c + m[3] * d,
            m[0] * dx + m[2] * dy + m[4],
            m[1] * dx + m[3] * dy + m[5],
        };
        if (outline_glyph(t, part, sub, o, depth + 1) < 0)
            return -1;
    } while ((flags & MORE_COMPONENTS) && cursor < t->size);

    return 0;
}

static int outline_glyph(const ttf_t *t, int glyph, const float m[6], outline_t *o, int depth) {
    uint32_t at = glyph_at(t, glyph);
    if (!at)
        return 0;
    if (depth > COMPOSITE_DEPTH)
        return -1;

    int contours = rds16(t, at);
    if (contours > 0)
        return outline_simple(t, at, contours, m, o);
    if (contours < 0)
        return outline_composite(t, at, m, o, depth);
    return 0;
}

// coverage is built up as how much each edge adds to the pixels right of it, then summed
// along the rows. its exact for straight edges, no supersampling
typedef struct {
    float *acc;
    int    w, h;
} raster_t;

static float fl(float v) {
    int i = (int)v;
    return (float)(v < (float)i ? i - 1 : i);
}

static float ce(float v) {
    int i = (int)v;
    return (float)(v > (float)i ? i + 1 : i);
}

static void line(raster_t *r, float x0, float y0, float x1, float y1) {
    if (y0 == y1)
        return;

    float dir = 1.0f;
    if (y0 > y1) {
        float tx = x0, ty = y0;
        x0 = x1; y0 = y1;
        x1 = tx; y1 = ty;
        dir = -1.0f;
    }

    float dxdy = (x1 - x0) / (y1 - y0);
    float x    = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }

    int bottom = (int)ce(y1);
    if (bottom > r->h)
        bottom = r->h;

    for (int y = (int)y0; y < bottom; y++) {
        float *row   = r->acc + y * r->w;
        float  top   = (float)y > y0 ? (float)y : y0;
        float  low   = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float  dy    = low - top;
        float  xnext = x + dxdy * dy;
        float  d     = dy * dir;

        float xa  = x < xnext ? x : xnext;
        float xb  = x < xnext ? xnext : x;
        float xaf = fl(xa);
        float xbc = ce(xb);
        int   ia  = (int)xaf;
        int   ib  = (int)xbc;

        if (ia < 0 || ib >= r->w) {
            x = xnext;
            continue;
        }

        if (ib <= ia + 1) {
            // the edge stays inside one pixel on this row
            float mid = 0.5f * (x + xnext) - xaf;
            row[ia]     += d - d * mid;
            row[ia + 1] += d * mid;
        } else {
            float s   = 1.0f / (xb - xa);
            float fa  = xa - xaf;
            float a0  = 0.5f * s * (1.0f - fa) * (1.0f - fa);
            float fb  = xb - xbc + 1.0f;
            float am  = 0.5f * s * fb * fb;

            row[ia] += d * a0;
            if (ib == ia + 2) {
                row[ia + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - fa);
                row[ia + 1] += d * (a1 - a0);
                for (int xi = ia + 2; xi < ib - 1; xi++)
                    row[xi] += d * s;
                float a2 = a1 + (float)(ib - ia - 3) * s;
                row[ib - 1] += d * (1.0f - a2 - am);
            }
            row[ib] += d * am;
        }
        x = xnext;
    }
}

// a curve is cut into as many lines as it takes to be within a fraction of a pixel of it
static void curve(raster_t *r, float x0, float y0, float x1, float y1, float x2, float y2) {
    float ddx = x0 - 2.0f * x1 + x2;
    float ddy = y0 - 2.0f * y1 + y2;
    float dev = 3.0f * (ddx * ddx + ddy * ddy);

    int n = 1;
    while (n < 32 && (float)(n * n * n * n) < dev)
        n++;

    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float u  = (float)i / (float)n;
        float v  = 1.0f - u;
        float qx = v * v * x0 + 2.0f * v * u * x1 + u * u * x2;
        float qy = v * v * y0 + 2.0f * v * u * y1 + u * u * y2;
        line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

// one closed contour. two control points in a row have a point on the curve halfway between
// them that the file leaves out, and a contour can start on a control point too
static void contour(raster_t *r, const point_t *p, int n) {
    if (n < 2)
        return;

    int   first = -1;
    for (int i = 0; i < n && first < 0; i++) {
        if (p[i].on)
            first = i;
    }

    float sx, sy;
    int   from, steps;
    if (first >= 0) {
        sx    = p[first].x;
        sy    = p[first].y;
        from  = first + 1;
        steps = n - 1;
    } else {
        sx    = (p[n - 1].x + p[0].x) * 0.5f;
        sy    = (p[n - 1].y + p[0].y) * 0.5f;
        from  = 0;
        steps = n;
    }

    float cx = sx, cy = sy;     // where the pen is
    float kx = 0, ky = 0;       // a control point waiting for the point after it
    int   held = 0;

    for (int i = 0; i < steps; i++) {
        const point_t *q = &p[(from + i) % n];

        if (q->on) {
            if (held)
                curve(r, cx, cy, kx, ky, q->x, q->y);
            else
                line(r, cx, cy, q->x, q->y);
            cx   = q->x;
            cy   = q->y;
            held = 0;
        } else {
            if (held) {
                float mx = (kx + q->x) * 0.5f;
                float my = (ky + q->y) * 0.5f;
                curve(r, cx, cy, kx, ky, mx, my);
                cx = mx;
                cy = my;
            }
            kx   = q->x;
            ky   = q->y;
            held = 1;
        }
    }

    if (held)
        curve(r, cx, cy, kx, ky, sx, sy);
    else
        line(r, cx, cy, sx, sy);
}

uint8_t *ttf_render(const ttf_t *t, int glyph, float scale, int *w, int *h, int *left, int *top) {
    outline_t o = {0};
    float     m[6] = { scale, 0, 0, scale, 0, 0 };

    *w = *h = *left = *top = 0;
    if (outline_glyph(t, glyph, m, &o, 0) < 0 || !o.count) {
        free(o.points);
        return NULL;
    }

    float xmin = o.points[0].x, xmax = xmin;
    float ymin = o.points[0].y, ymax = ymin;
    for (int i = 1; i < o.count; i++) {
        if (o.points[i].x < xmin) xmin = o.points[i].x;
        if (o.points[i].x > xmax) xmax = o.points[i].x;
        if (o.points[i].y < ymin) ymin = o.points[i].y;
        if (o.points[i].y > ymax) ymax = o.points[i].y;
    }

    int x0 = (int)fl(xmin);
    int y1 = (int)ce(ymax);
    int gw = (int)ce(xmax) - x0;
    int gh = y1 - (int)fl(ymin);
    if (gw <= 0 || gh <= 0 || gw > GLYPH_PX_MAX || gh > GLYPH_PX_MAX) {
        free(o.points);
        return NULL;
    }

    // a column spare on the right, an edge on the last pixel still adds to the one after it
    raster_t r = { .w = gw + 1, .h = gh };
    r.acc = calloc((size_t)r.w * r.h + 1, sizeof(float));
    uint8_t *out = malloc((size_t)gw * gh);
    if (!r.acc || !out) {
        free(r.acc);
        free(out);
        free(o.points);
        return NULL;
    }

    // the font has y going up and its origin on the baseline, the bitmap starts at its top left
    for (int i = 0; i < o.count; i++) {
        o.points[i].x -= (float)x0;
        o.points[i].y  = (float)y1 - o.points[i].y;
    }

    for (int i = 0, start = 0; i < o.count; i++) {
        if (!o.points[i].last)
            continue;
        contour(&r, o.points + start, i - start + 1);
        start = i + 1;
    }

    for (int y = 0; y < gh; y++) {
        float sum = 0;
        for (int x = 0; x < gw; x++) {
            sum += r.acc[y * r.w + x];
            float v = sum < 0 ? -sum : sum;
            out[y * gw + x] = (uint8_t)(v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
        }
    }

    free(r.acc);
    free(o.points);

    *w    = gw;
    *h    = gh;
    *left = x0;
    *top  = y1;
    return out;
}
