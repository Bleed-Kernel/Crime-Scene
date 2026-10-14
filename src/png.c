#include <stdlib.h>
#include <string.h>
#include <png.h>

#define SIDE_MAX   8192     // nothing wider or taller than this gets decoded
#define CODE_BITS  15
#define LIT_CODES  288
#define DIST_CODES 30

// colour types
#define TYPE_GREY       0
#define TYPE_RGB        2
#define TYPE_PALETTE    3
#define TYPE_GREY_ALPHA 4
#define TYPE_RGBA       6

typedef struct {
    const uint8_t *in;
    size_t         in_size;
    size_t         in_at;
    uint8_t       *out;
    size_t         out_size;
    size_t         out_at;

    uint32_t       hold;    // bits taken from in that nothing has used yet
    int            held;
    int            bad;     // ran off the end of in, or hit a code that doesnt exist
} inflate_t;

// how many codes there are of each length, and the symbols in code order
typedef struct {
    uint16_t count[CODE_BITS + 1];
    uint16_t symbol[LIT_CODES];
} huffman_t;

static const uint16_t len_base[29]   = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t  len_extra[29]  = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                        4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30]  = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                        769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t  dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// low bits first, never more than 16 at a time
static uint32_t bits(inflate_t *s, int n) {
    while (s->held < n) {
        if (s->in_at >= s->in_size) {
            s->bad = 1;
            return 0;
        }
        s->hold |= (uint32_t)s->in[s->in_at++] << s->held;
        s->held += 8;
    }

    uint32_t v = s->hold & ((1u << n) - 1);
    s->hold >>= n;
    s->held  -= n;
    return v;
}

// -1 if the lengths ask for more codes than there are
static int huffman_build(huffman_t *h, const uint8_t *lengths, int n) {
    uint16_t next[CODE_BITS + 2];

    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;

    int left = 1;
    for (int len = 1; len <= CODE_BITS; len++) {
        left = left * 2 - h->count[len];
        if (left < 0)
            return -1;
    }

    next[1] = 0;
    for (int len = 1; len <= CODE_BITS; len++)
        next[len + 1] = (uint16_t)(next[len] + h->count[len]);
    for (int i = 0; i < n; i++) {
        if (lengths[i])
            h->symbol[next[lengths[i]]++] = (uint16_t)i;
    }
    return 0;
}

// a bit at a time. codes of one length are consecutive, so its a compare per length
static int decode(inflate_t *s, const huffman_t *h) {
    int code  = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= CODE_BITS; len++) {
        code |= (int)bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }
    s->bad = 1;
    return 0;
}

static int block_stored(inflate_t *s) {
    // starts on the next whole byte
    s->hold = 0;
    s->held = 0;
    if (s->in_size - s->in_at < 4)
        return -1;

    const uint8_t *p   = s->in + s->in_at;
    size_t         len = p[0] | (size_t)p[1] << 8;
    if (len != ((p[2] | (size_t)p[3] << 8) ^ 0xFFFF))
        return -1;
    s->in_at += 4;

    if (len > s->in_size - s->in_at || len > s->out_size - s->out_at)
        return -1;
    memcpy(s->out + s->out_at, s->in + s->in_at, len);
    s->in_at  += len;
    s->out_at += len;
    return 0;
}

static int block_codes(inflate_t *s, const huffman_t *lit, const huffman_t *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (s->bad)
            return -1;
        if (sym == 256)
            return 0;
        if (sym < 256) {
            if (s->out_at >= s->out_size)
                return -1;
            s->out[s->out_at++] = (uint8_t)sym;
            continue;
        }

        sym -= 257;
        if (sym >= 29)
            return -1;
        size_t len = len_base[sym] + bits(s, len_extra[sym]);

        sym = decode(s, dist);
        if (s->bad || sym >= DIST_CODES)
            return -1;
        size_t back = dist_base[sym] + bits(s, dist_extra[sym]);

        if (s->bad || back > s->out_at || len > s->out_size - s->out_at)
            return -1;
        // a byte at a time, the copy is allowed to run into what its writing
        for (; len; len--, s->out_at++)
            s->out[s->out_at] = s->out[s->out_at - back];
    }
}

static int block_fixed(inflate_t *s) {
    uint8_t   lengths[LIT_CODES];
    huffman_t lit, dist;

    memset(lengths, 8, LIT_CODES);
    memset(lengths + 144, 9, 112);
    memset(lengths + 256, 7, 24);
    huffman_build(&lit, lengths, LIT_CODES);

    memset(lengths, 5, DIST_CODES);
    huffman_build(&dist, lengths, DIST_CODES);
    return block_codes(s, &lit, &dist);
}

static int block_dynamic(inflate_t *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t   lengths[LIT_CODES + DIST_CODES] = {0};
    huffman_t meta, lit, dist;

    int lits  = (int)bits(s, 5) + 257;
    int dists = (int)bits(s, 5) + 1;
    int metas = (int)bits(s, 4) + 4;
    if (s->bad || lits > 286 || dists > DIST_CODES)
        return -1;

    // the code lengths are huffman coded themselves, this is the code for those
    for (int i = 0; i < metas; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (huffman_build(&meta, lengths, 19) < 0)
        return -1;

    for (int at = 0; at < lits + dists; ) {
        int sym = decode(s, &meta);
        if (s->bad)
            return -1;
        if (sym < 16) {
            lengths[at++] = (uint8_t)sym;
            continue;
        }

        // 16 is the last length again a few times, 17 and 18 are runs of zero
        int value = 0;
        int repeat;
        if (sym == 16) {
            if (!at)
                return -1;
            value  = lengths[at - 1];
            repeat = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + (int)bits(s, 3);
        } else {
            repeat = 11 + (int)bits(s, 7);
        }
        if (at + repeat > lits + dists)
            return -1;
        while (repeat--)
            lengths[at++] = (uint8_t)value;
    }

    // no end of block code means it could never end
    if (s->bad || !lengths[256])
        return -1;
    if (huffman_build(&lit, lengths, lits) < 0 || huffman_build(&dist, lengths + lits, dists) < 0)
        return -1;
    return block_codes(s, &lit, &dist);
}

static int inflate(inflate_t *s) {
    int last;

    do {
        last = (int)bits(s, 1);
        int type = (int)bits(s, 2);
        if (s->bad)
            return -1;

        int r = -1;
        if (type == 0) r = block_stored(s);
        if (type == 1) r = block_fixed(s);
        if (type == 2) r = block_dynamic(s);
        if (r < 0)
            return -1;
    } while (!last);
    return 0;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int format_ok(int depth, int type) {
    switch (type) {
    case TYPE_GREY:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case TYPE_PALETTE:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case TYPE_RGB:
    case TYPE_GREY_ALPHA:
    case TYPE_RGBA:
        return depth == 8 || depth == 16;
    }
    return 0;
}

// whichever of left, up and up left is nearest to left + up - up left
static int paeth(int a, int b, int c) {
    int pa = b > c ? b - c : c - b;
    int pb = a > c ? a - c : c - a;
    int pc = a + b - 2 * c;
    if (pc < 0) pc = -pc;

    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

// every row is a filter byte then the row, each byte stored as its difference from the ones
// before it. bpp is how many bytes back the same channel of the pixel to the left is
static int unfilter(uint8_t *raw, int h, size_t row, int bpp) {
    const uint8_t *prev = NULL;

    for (int y = 0; y < h; y++) {
        uint8_t  filter = raw[0];
        uint8_t *cur    = raw + 1;
        if (filter > 4)
            return -1;

        for (size_t i = 0; filter && i < row; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;

            if (filter == 1) cur[i] = (uint8_t)(cur[i] + a);
            if (filter == 2) cur[i] = (uint8_t)(cur[i] + b);
            if (filter == 3) cur[i] = (uint8_t)(cur[i] + (a + b) / 2);
            if (filter == 4) cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
        }
        prev = cur;
        raw += row + 1;
    }
    return 0;
}

// 16 bit samples are big endian, so the first byte of one is all that fits anyway
static uint32_t pixel(const uint8_t *row, int x, int depth, int type, int bpp, const uint32_t *palette) {
    if (depth < 8) {
        int      shift = 8 - depth - (x * depth & 7);
        uint32_t max   = (1u << depth) - 1;
        uint32_t v     = (row[(size_t)x * depth >> 3] >> shift) & max;
        if (type == TYPE_PALETTE)
            return palette[v];
        return 0xFF000000 | (v * 255 / max) * 0x010101;
    }

    const uint8_t *p    = row + (size_t)x * bpp;
    int            step = depth / 8;

    switch (type) {
    case TYPE_GREY:       return 0xFF000000 | (uint32_t)p[0] * 0x010101;
    case TYPE_RGB:        return 0xFF000000 | (uint32_t)p[0] << 16 | (uint32_t)p[step] << 8 | p[step * 2];
    case TYPE_PALETTE:    return palette[p[0]];
    case TYPE_GREY_ALPHA: return (uint32_t)p[step] << 24 | (uint32_t)p[0] * 0x010101;
    default:              return (uint32_t)p[step * 3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[step] << 8 | p[step * 2];
    }
}

uint32_t *png_decode(const uint8_t *data, size_t size, int *w_out, int *h_out) {
    static const uint8_t magic[8]    = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    static const uint8_t channels[7] = {1, 0, 3, 1, 2, 0, 4};

    if (size < 8 || memcmp(data, magic, 8) != 0)
        return NULL;

    uint32_t palette[256];
    for (int i = 0; i < 256; i++)
        palette[i] = 0xFF000000;

    // the image is one zlib stream cut up across the IDAT chunks, this is it back in one piece.
    // it cant come to more than the file
    uint8_t *packed      = malloc(size);
    size_t   packed_size = 0;
    if (!packed)
        return NULL;

    int w = 0, h = 0, depth = 0, type = 0;

    // a chunk is its length, four letters, that many bytes and a crc nobody checks
    for (size_t at = 8; size - at >= 12; ) {
        uint32_t       len  = be32(data + at);
        const uint8_t *name = data + at + 4;
        const uint8_t *body = data + at + 8;
        if (len > size - at - 12)
            break;

        if (!memcmp(name, "IHDR", 4) && len >= 13) {
            uint32_t wide = be32(body);
            uint32_t tall = be32(body + 4);
            // body[12] is interlacing
            if (!wide || !tall || wide > SIDE_MAX || tall > SIDE_MAX || !format_ok(body[8], body[9]) || body[12])
                break;
            w     = (int)wide;
            h     = (int)tall;
            depth = body[8];
            type  = body[9];
        } else if (!memcmp(name, "PLTE", 4)) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++)
                palette[i] = 0xFF000000 | (uint32_t)body[i * 3] << 16 | (uint32_t)body[i * 3 + 1] << 8 | body[i * 3 + 2];
        } else if (!memcmp(name, "tRNS", 4) && type == TYPE_PALETTE) {
            for (uint32_t i = 0; i < len && i < 256; i++)
                palette[i] = (palette[i] & 0xFFFFFF) | (uint32_t)body[i] << 24;
        } else if (!memcmp(name, "IDAT", 4)) {
            memcpy(packed + packed_size, body, len);
            packed_size += len;
        } else if (!memcmp(name, "IEND", 4)) {
            break;
        }
        at += 12 + (size_t)len;
    }

    // two bytes of zlib header first. 8 is deflate, 0x20 is a preset dictionary which png never has
    if (!w || packed_size < 2 || (packed[0] & 0x0F) != 8 || (packed[1] & 0x20)) {
        free(packed);
        return NULL;
    }

    int    bpp      = (channels[type] * depth + 7) / 8;
    size_t row      = ((size_t)w * channels[type] * depth + 7) / 8;
    size_t raw_size = (size_t)h * (row + 1);

    uint8_t  *raw    = malloc(raw_size);
    uint32_t *pixels = malloc((size_t)w * h * sizeof(uint32_t));

    inflate_t s = {
        .in       = packed + 2,
        .in_size  = packed_size - 2,
        .out      = raw,
        .out_size = raw_size,
    };

    if (!raw || !pixels || inflate(&s) < 0 || s.out_at != raw_size || unfilter(raw, h, row, bpp) < 0) {
        free(packed);
        free(raw);
        free(pixels);
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *line = raw + (size_t)y * (row + 1) + 1;
        for (int x = 0; x < w; x++)
            pixels[(size_t)y * w + x] = pixel(line, x, depth, type, bpp, palette);
    }

    free(packed);
    free(raw);
    *w_out = w;
    *h_out = h;
    return pixels;
}
