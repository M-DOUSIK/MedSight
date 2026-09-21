/* gui_draw.c - MedSight UI rendering and screens
 *
 * This is built around the project designer's reference artwork:
 * a white page inside a blush rounded card frame with heart/capsule corner
 * motifs, anti-aliased bold-italic type from a real font, soft rounded
 * buttons, and the Lumio mascot blitted from the designer's own drawing
 * rather than approximated with circles and triangles.
 *
 * The three things that actually changed how it looks:
 *   1. Real fonts (ui_assets.h). The old 8x8 bitmap font scaled by integer
 *      replication is why nothing could look finished no matter how the
 *      layout was arranged - 24px text made of 3x3 blocks reads as a
 *      prototype. gui_font_* draws 4bpp anti-aliased proportional glyphs.
 *   2. Real artwork. The mascot and corner motifs are cropped from the
 *      designer's PNGs at build time by tools/gen_ui_assets.py.
 *   3. One shape language. Everything is a rounded rectangle with a light
 *      fill, a saturated edge and ink text - see gui_draw.h's colour rules.
 */

#include "ui/gui_draw.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Generated sprite/font tables. Included here rather than compiled as their
 * own ui_assets.c: a new.c needs a matching <link> in the STM32CubeIDE
 *.project, and an IDE that already has the project open keeps a stale copy
 * of that file in memory, so the unit silently never compiles and the link
 * fails on every asset symbol. Including into a file already in the build
 * removes that failure mode. See tools/gen_ui_assets.py. */
#include "ui/ui_assets_data.inc"

static uint32_t s_gui_buffer = 0;
static uint16_t s_gui_width  = 0;
static uint16_t s_gui_height = 0;

/* ── 8×8 bitmap font (ASCII 32-127) - legacy, see gui_draw.h ─────────────── */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 '!'
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 35 '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 36 '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 37 '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 38 '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '\''
    {0x0E,0x1C,0x38,0x38,0x38,0x1C,0x0E,0x00}, // 40 '('
    {0x38,0x1C,0x0E,0x0E,0x0E,0x1C,0x38,0x00}, // 41 ')'
    {0x00,0x18,0x7E,0x3C,0x7E,0x18,0x00,0x00}, // 42 '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 '.'
    {0x00,0x03,0x06,0x0C,0x18,0x30,0x60,0x00}, // 47 '/'
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 48 '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49 '1'
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // 50 '2'
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 51 '3'
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, // 52 '4'
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 53 '5'
    {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 54 '6'
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 '7'
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 56 '8'
    {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, // 57 '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // 58 ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // 59 ';'
    {0x0E,0x1C,0x38,0x70,0x38,0x1C,0x0E,0x00}, // 60 '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 61 '='
    {0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0x00}, // 62 '>'
    {0x3C,0x66,0x06,0x1C,0x18,0x00,0x18,0x00}, // 63 '?'
    {0x3C,0x66,0x6E,0x6E,0x60,0x60,0x3C,0x00}, // 64 '@'
    {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 65 'A'
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 66 'B'
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 67 'C'
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 68 'D'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // 69 'E'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // 70 'F'
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, // 71 'G'
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 72 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 'I'
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 74 'J'
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 75 'K'
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 76 'L'
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 77 'M'
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 78 'N'
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 79 'O'
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 80 'P'
    {0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00}, // 81 'Q'
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, // 82 'R'
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 83 'S'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 84 'T'
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 85 'U'
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 86 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 87 'W'
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 88 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 89 'Y'
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 90 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 '['
    {0x00,0x60,0x30,0x18,0x0C,0x06,0x03,0x00}, // 92 backslash
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ']'
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, // 94 '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, // 95 '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 97 'a'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 98 'b'
    {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00}, // 99 'c'
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 100 'd'
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 101 'e'
    {0x1C,0x30,0x7E,0x30,0x30,0x30,0x30,0x00}, // 102 'f'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 103 'g'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 104 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 105 'i'
    {0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x3C}, // 106 'j'
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 107 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 'l'
    {0x00,0x00,0x7A,0x7F,0x6B,0x6B,0x6B,0x00}, // 109 'm'
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 110 'n'
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 111 'o'
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 112 'p'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, // 113 'q'
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 114 'r'
    {0x00,0x00,0x3C,0x60,0x3C,0x06,0x3C,0x00}, // 115 's'
    {0x30,0x30,0x7E,0x30,0x30,0x30,0x1C,0x00}, // 116 't'
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 117 'u'
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 118 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 119 'w'
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 120 'x'
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 121 'y'
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 122 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 123 '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, // 124 '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 125 '}'
    {0x3A,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, // 126 '~'
    {0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x00}  // 127 DEL
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Core primitives
 * ═══════════════════════════════════════════════════════════════════════════ */
void gui_draw_init(uint32_t buffer_address, uint16_t width, uint16_t height)
{
    s_gui_buffer = buffer_address;
    s_gui_width  = width;
    s_gui_height = height;
}

void gui_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!s_gui_buffer) return;
    if (x >= s_gui_width || y >= s_gui_height) return;
    if (x + w > s_gui_width)  w = s_gui_width  - x;
    if (y + h > s_gui_height) h = s_gui_height - y;

    volatile uint16_t *fb = (volatile uint16_t *)s_gui_buffer;
    for (uint16_t row = 0; row < h; row++)
    {
        uint32_t off = (uint32_t)(y + row) * s_gui_width + x;
        for (uint16_t col = 0; col < w; col++)
            fb[off + col] = color;
    }
}

/* Horizontal inset of a rounded corner of radius r, `dyc` rows from the
 * corner's centre line (1..r). */
static uint16_t rr_inset(uint16_t r, uint16_t dyc)
{
    float v = (float)r * (float)r - (float)dyc * (float)dyc;
    if (v < 0.0f) v = 0.0f;
    float ins = (float)r - sqrtf(v);
    return (uint16_t)(ins + 0.5f);
}

/* Inset for row `j` of an h-tall rounded rect (0 in the straight middle). */
static uint16_t rr_row_inset(uint16_t j, uint16_t h, uint16_t r)
{
    if (r == 0u) return 0u;
    if (j < r)            return rr_inset(r, (uint16_t)(r - j));
    if (j >= (uint16_t)(h - r)) return rr_inset(r, (uint16_t)(j - (h - r) + 1u));
    return 0u;
}

void gui_fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t r, uint16_t color)
{
    if (w == 0u || h == 0u) return;
    if (r > w / 2u) r = w / 2u;
    if (r > h / 2u) r = h / 2u;
    for (uint16_t j = 0; j < h; j++)
    {
        uint16_t ins = rr_row_inset(j, h, r);
        if (2u * ins >= w) continue;
        gui_draw_rect((uint16_t)(x + ins), (uint16_t)(y + j),
                      (uint16_t)(w - 2u * ins), 1u, color);
    }
}

void gui_stroke_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t r, uint16_t t, uint16_t color)
{
    if (w == 0u || h == 0u || t == 0u) return;
    if (r > w / 2u) r = w / 2u;
    if (r > h / 2u) r = h / 2u;
    if (2u * t >= w || 2u * t >= h) { gui_fill_round_rect(x, y, w, h, r, color); return; }

    uint16_t iw = (uint16_t)(w - 2u * t);
    uint16_t ih = (uint16_t)(h - 2u * t);
    uint16_t ir = (r > t) ? (uint16_t)(r - t) : 0u;
    if (ir > iw / 2u) ir = iw / 2u;
    if (ir > ih / 2u) ir = ih / 2u;

    for (uint16_t j = 0; j < h; j++)
    {
        uint16_t o = rr_row_inset(j, h, r);
        if (2u * o >= w) continue;

        if (j < t || j >= (uint16_t)(h - t))
        {
            gui_draw_rect((uint16_t)(x + o), (uint16_t)(y + j),
                          (uint16_t)(w - 2u * o), 1u, color);
            continue;
        }
        uint16_t i = rr_row_inset((uint16_t)(j - t), ih, ir);
        uint16_t left_end  = (uint16_t)(t + i);          /* relative to x */
        uint16_t right_beg = (uint16_t)(w - t - i);
        if (left_end > o)
            gui_draw_rect((uint16_t)(x + o), (uint16_t)(y + j),
                          (uint16_t)(left_end - o), 1u, color);
        if ((uint16_t)(w - o) > right_beg)
            gui_draw_rect((uint16_t)(x + right_beg), (uint16_t)(y + j),
                          (uint16_t)((w - o) - right_beg), 1u, color);
    }
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a8);

/* Scale every channel of an RGB565 colour by num/den. Used for a gem's rim
 * and its highlight, so one colour parameter produces the whole shaded
 * lentil and the palette stays a single number per hopper. */
static uint16_t shade565(uint16_t c, uint32_t num, uint32_t den)
{
    uint32_t r = ((c >> 11) & 0x1Fu) * num / den;
    uint32_t g = ((c >>  5) & 0x3Fu) * num / den;
    uint32_t b = ( c        & 0x1Fu) * num / den;
    if (r > 31u) r = 31u;
    if (g > 63u) g = 63u;
    if (b > 31u) b = 31u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void gui_draw_gem(uint16_t cx, uint16_t cy, uint16_t r, uint16_t fill)
{
    if (!s_gui_buffer || r == 0u) return;

    const uint16_t rim  = shade565(fill, 62u, 100u);
    const uint16_t lite = shade565(fill, 145u, 100u);

    /* Highlight: a small disc up and to the left, the classic sugar-shell
     * glint. Kept as a fraction of r so it scales with the gem. */
    const float hx = (float)cx - (float)r * 0.34f;
    const float hy = (float)cy - (float)r * 0.38f;
    const float hr = (float)r * 0.36f;

    const float fr    = (float)r;
    const float r_rim = fr - 2.0f;          /* inside this it is body colour */

    volatile uint16_t *fb = (volatile uint16_t *)s_gui_buffer;

    int32_t y0 = (int32_t)cy - (int32_t)r - 1;
    int32_t y1 = (int32_t)cy + (int32_t)r + 1;
    int32_t x0 = (int32_t)cx - (int32_t)r - 1;
    int32_t x1 = (int32_t)cx + (int32_t)r + 1;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 >= (int32_t)s_gui_height) y1 = (int32_t)s_gui_height - 1;
    if (x1 >= (int32_t)s_gui_width)  x1 = (int32_t)s_gui_width  - 1;

    for (int32_t py = y0; py <= y1; py++)
    {
        uint32_t off = (uint32_t)py * s_gui_width;
        for (int32_t px = x0; px <= x1; px++)
        {
            float dx = (float)px - (float)cx;
            float dy = (float)py - (float)cy;
            float d  = sqrtf(dx * dx + dy * dy);
            if (d > fr + 1.0f) continue;

            /* Body colour first: rim near the edge, fill inside, with the
             * highlight blended over the fill. */
            uint16_t body;
            if (d > r_rim)
            {
                body = rim;
            }
            else
            {
                float hdx = (float)px - hx;
                float hdy = (float)py - hy;
                float hd  = sqrtf(hdx * hdx + hdy * hdy);
                if (hd <= hr)
                {
                    /* Fade the glint out towards its own edge. */
                    uint32_t a = (uint32_t)(200.0f * (1.0f - hd / hr));
                    body = blend565(fill, lite, a > 200u ? 200u : a);
                }
                else
                {
                    body = fill;
                }
            }

            /* Antialias the outer edge against whatever is already there. */
            if (d > fr - 1.0f)
            {
                float cov = (fr + 0.5f) - d;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                fb[off + (uint32_t)px] =
                    blend565(fb[off + (uint32_t)px], body, (uint32_t)(cov * 255.0f));
            }
            else
            {
                fb[off + (uint32_t)px] = body;
            }
        }
    }
}

void gui_blit_sprite(uint16_t x, uint16_t y, const ui_sprite_t *s)
{
    if (!s_gui_buffer || !s) return;
    volatile uint16_t *fb = (volatile uint16_t *)s_gui_buffer;

    for (uint16_t j = 0; j < s->h; j++)
    {
        uint16_t py = (uint16_t)(y + j);
        if (py >= s_gui_height) break;
        const uint8_t *row = s->pixels + (uint32_t)j * s->stride;
        uint32_t off = (uint32_t)py * s_gui_width;

        for (uint16_t i = 0; i < s->w; i++)
        {
            uint16_t px = (uint16_t)(x + i);
            if (px >= s_gui_width) break;
            uint8_t b   = row[i >> 1];
            uint8_t idx = (i & 1u) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
            if (idx == 0u) continue;               /* transparent */
            fb[off + px] = s->palette[idx];
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Anti-aliased proportional text
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a8)
{
    uint32_t sr = (src >> 11) & 0x1Fu, sg = (src >> 5) & 0x3Fu, sb = src & 0x1Fu;
    uint32_t dr = (dst >> 11) & 0x1Fu, dg = (dst >> 5) & 0x3Fu, db = dst & 0x1Fu;
    uint32_t ia = 255u - a8;
    uint32_t r = (sr * a8 + dr * ia + 127u) / 255u;
    uint32_t g = (sg * a8 + dg * ia + 127u) / 255u;
    uint32_t b = (sb * a8 + db * ia + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static const ui_glyph_t *glyph_of(const ui_font_t *f, char c)
{
    uint8_t u = (uint8_t)c;
    if (u < f->first || u > f->last) u = (uint8_t)'?';
    return &f->glyphs[u - f->first];
}

/* Width of one line (stops at '\n' or NUL). */
static uint16_t line_width(const ui_font_t *f, const char *s)
{
    uint32_t w = 0;
    for (; *s && *s != '\n'; s++) w += glyph_of(f, *s)->advance;
    return (uint16_t)w;
}

uint16_t gui_font_width(const ui_font_t *f, const char *str)
{
    if (!f || !str) return 0u;
    return line_width(f, str);
}

static void draw_glyph(const ui_font_t *f, const ui_glyph_t *g,
                       int32_t pen_x, int32_t line_y, uint16_t color)
{
    if (!g->w || !g->h) return;
    volatile uint16_t *fb = (volatile uint16_t *)s_gui_buffer;
    const uint8_t *bm = f->blob + g->offset;
    uint16_t stride = (uint16_t)((g->w + 1) / 2);

    for (uint16_t j = 0; j < g->h; j++)
    {
        int32_t py = line_y + g->yoff + (int32_t)j;
        if (py < 0 || py >= (int32_t)s_gui_height) continue;
        uint32_t off = (uint32_t)py * s_gui_width;

        for (uint16_t i = 0; i < g->w; i++)
        {
            int32_t px = pen_x + g->xoff + (int32_t)i;
            if (px < 0 || px >= (int32_t)s_gui_width) continue;
            uint8_t b = bm[(uint32_t)j * stride + (i >> 1)];
            uint8_t a = (i & 1u) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
            if (a == 0u) continue;
            fb[off + px] = (a >= 15u) ? color
                                      : blend565(fb[off + px], color, (uint32_t)a * 17u);
        }
    }
}

void gui_font_text(uint16_t x, uint16_t y, const char *str,
                   uint16_t color, const ui_font_t *f)
{
    if (!s_gui_buffer || !f || !str) return;
    int32_t pen = x, line_y = y;
    for (; *str; str++)
    {
        if (*str == '\n') { line_y += f->line_height; pen = x; continue; }
        const ui_glyph_t *g = glyph_of(f, *str);
        draw_glyph(f, g, pen, line_y, color);
        pen += g->advance;
    }
}

/* As gui_font_text_centered(), plus `track` extra pixels between every pair
 * of glyphs. The generated faces are set solid for body copy, which is right
 * for sentences and too tight for a short run of large digits read at
 * arm's length - "100%" in particular closes up into one shape. Tracking is
 * a typographic fix, not a font change: the glyphs are untouched, only the
 * advances grow, and the string stays centred because the extra width is
 * folded into the measurement below. */
void gui_font_text_centered_tracked(uint16_t cx, uint16_t y, const char *str,
                                    uint16_t color, const ui_font_t *f,
                                    uint8_t track)
{
    if (!s_gui_buffer || !f || !str) return;
    int32_t line_y = y;
    while (*str)
    {
        uint16_t n = 0u;
        for (const char *p = str; *p && *p != '\n'; p++) n++;
        int32_t w = (int32_t)line_width(f, str) + (n > 1u ? (int32_t)track * (n - 1u) : 0);
        int32_t x = (int32_t)cx - w / 2;
        if (x < 0) x = 0;
        int32_t pen = x;
        for (; *str && *str != '\n'; str++)
        {
            const ui_glyph_t *g = glyph_of(f, *str);
            draw_glyph(f, g, pen, line_y, color);
            pen += g->advance + (int32_t)track;
        }
        if (*str == '\n') str++;
        line_y += f->line_height;
    }
}

void gui_font_text_centered(uint16_t cx, uint16_t y, const char *str,
                            uint16_t color, const ui_font_t *f)
{
    if (!s_gui_buffer || !f || !str) return;
    int32_t line_y = y;
    while (*str)
    {
        uint16_t w = line_width(f, str);
        int32_t x = (int32_t)cx - (int32_t)w / 2;
        if (x < 0) x = 0;
        int32_t pen = x;
        for (; *str && *str != '\n'; str++)
        {
            const ui_glyph_t *g = glyph_of(f, *str);
            draw_glyph(f, g, pen, line_y, color);
            pen += g->advance;
        }
        if (*str == '\n') str++;
        line_y += f->line_height;
    }
}

/* ── Legacy 8×8 text ────────────────────────────────────────────────────── */
void gui_draw_text(uint16_t x, uint16_t y,
                   const char *str, uint16_t color, uint8_t scale)
{
    if (!scale) scale = 1;
    uint16_t sx = x;
    while (*str)
    {
        if (*str == '\n') { y = (uint16_t)(y + 10u * scale); x = sx; str++; continue; }
        uint8_t ch = (uint8_t)*str;
        if (ch < 32 || ch > 127) ch = '?';
        const uint8_t *g = font8x8[ch - 32];
        for (uint8_t row = 0; row < 8; row++)
        {
            uint8_t rd = g[row];
            for (uint8_t col = 0; col < 8; col++)
                if (rd & (1u << (7 - col)))
                    gui_draw_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
        x = (uint16_t)(x + 9u * scale);
        str++;
    }
}

void gui_draw_text_centered(uint16_t x_center, uint16_t y,
                            const char *str, uint16_t color, uint8_t scale)
{
    if (!scale) scale = 1;
    uint16_t len = 0;
    for (const char *cp = str; *cp && *cp != '\n'; cp++) len++;
    uint16_t half = (uint16_t)((len * 9u * scale) / 2u);
    uint16_t x    = (half > x_center) ? 0u : (uint16_t)(x_center - half);
    gui_draw_text(x, y, str, color, scale);
}

/* ── Flush helpers ──────────────────────────────────────────────────────── */
static void flush_all(void)
{
    SCB_CleanDCache_by_Addr((uint32_t *)s_gui_buffer,
                             (int32_t)(s_gui_width * s_gui_height * 2));
}
static void flush_rows(uint16_t y_start, uint16_t y_end)
{
    if (y_end >= s_gui_height) y_end = (uint16_t)(s_gui_height - 1u);
    uint32_t start = s_gui_buffer + (uint32_t)(y_start * s_gui_width * 2);
    uint32_t size  = (uint32_t)((y_end - y_start + 1) * s_gui_width * 2);
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)size);
}
void gui_draw_flush(void) { flush_all(); }
void gui_draw_flush_rows(uint16_t y_start, uint16_t y_end) { flush_rows(y_start, y_end); }

void gui_draw_mascot_bg(void) { /* anime_ui handles its own clear */ }

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared chrome
 * ═══════════════════════════════════════════════════════════════════════════ */
void gui_draw_frame(void)
{
    gui_draw_rect(0, 0, s_gui_width, s_gui_height, THEME_BG);
    gui_stroke_round_rect(FRAME_INSET, FRAME_INSET,
                          (uint16_t)(s_gui_width  - 2u * FRAME_INSET),
                          (uint16_t)(s_gui_height - 2u * FRAME_INSET),
                          FRAME_RADIUS, FRAME_THICK, THEME_FRAME);

    /* Heart top-left / bottom-right, capsule top-right / bottom-left -
     * the arrangement the designer used on every reference sheet. */
    const ui_sprite_t *hr = &ui_sprite_heart;
    const ui_sprite_t *cp = &ui_sprite_capsule;
    gui_blit_sprite(MOTIF_MARGIN, MOTIF_MARGIN, hr);
    gui_blit_sprite((uint16_t)(s_gui_width - MOTIF_MARGIN - cp->w), MOTIF_MARGIN, cp);
    gui_blit_sprite(MOTIF_MARGIN, (uint16_t)(s_gui_height - MOTIF_MARGIN - cp->h), cp);
    gui_blit_sprite((uint16_t)(s_gui_width  - MOTIF_MARGIN - hr->w),
                    (uint16_t)(s_gui_height - MOTIF_MARGIN - hr->h), hr);
}

void gui_draw_title_bar(const char *title, uint16_t accent)
{
    if (!title || !*title) return;
    gui_font_text_centered((uint16_t)(s_gui_width / 2u), TITLE_Y, title,
                           accent, &ui_font_lg);
    /* A short accent rule under the title: enough to signal state
     * (neutral / dispense / warning / alert) without a heavy colour bar. */
    gui_fill_round_rect((uint16_t)((s_gui_width - 140u) / 2u),
                        (uint16_t)(TITLE_Y + TITLE_H + 4u), 140u, 5u, 2u, accent);
}

/* Light fill that pairs with each saturated edge colour. */
static uint16_t fill_for_edge(uint16_t edge)
{
    if (edge == THEME_GREEN_EDGE) return THEME_GREEN_FILL;
    if (edge == THEME_AMBER_EDGE) return THEME_AMBER_FILL;
    if (edge == THEME_RED_EDGE)   return THEME_RED_FILL;
    if (edge == THEME_INK_SOFT)   return THEME_BG;
    return THEME_ROSE_FILL;
}

/* The three faces in descending order, so "one step down" and "shrink until
 * it fits" are both a single index move. */
static const ui_font_t *const FACE_LADDER[3] = {
    &ui_font_lg, &ui_font_md, &ui_font_sm
};

/* Step down the ladder until `str` fits in `avail` pixels, starting at
 * `from`. Returns the smallest index tried, never past the small face - a
 * string that does not fit even in sm is a caller problem, and a clipped
 * label is still better than a silently overflowing one. */
static uint8_t face_that_fits(const char *str, uint16_t avail, uint8_t from)
{
    for (uint8_t i = from; i < 3u; i++) {
        if (gui_font_width(FACE_LADDER[i], str) <= avail) return i;
    }
    return 2u;
}

/* Draw `str` centred on cx, truncated with an ellipsis if it will not fit in
 * `avail` pixels. For text that comes from a PATIENT (names are up to 31
 * characters and this UI cannot control them), clipping to the box is the
 * only correct behaviour - the alternative is a name running off the panel
 * or over the next column. */
void gui_font_text_ellipsis(uint16_t cx, uint16_t y, const char *str,
                            uint16_t avail, uint16_t color, const ui_font_t *f)
{
    if (!str || !*str) return;

    if (gui_font_width(f, str) <= avail) {
        gui_font_text_centered(cx, y, str, color, f);
        return;
    }

    /* Longest prefix that fits with ".." appended. Two dots rather than a
     * single ellipsis glyph because the font is ASCII-only. */
    char buf[64];
    uint16_t dots = gui_font_width(f, "..");
    size_t   n    = 0u;
    char     probe[64];

    while (n + 1u < sizeof(probe)) {
        if (str[n] == '\0') break;
        probe[n]     = str[n];
        probe[n + 1] = '\0';
        if ((uint16_t)(gui_font_width(f, probe) + dots) > avail) break;
        n++;
    }
    if (n == 0u) return;              /* not even one glyph fits */

    if (n >= sizeof(buf) - 3u) n = sizeof(buf) - 4u;
    memcpy(buf, str, n);
    buf[n]     = '.';
    buf[n + 1] = '.';
    buf[n + 2] = '\0';
    gui_font_text_centered(cx, y, buf, color, f);
}

void gui_draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t edge, const char *line1, const char *line2)
{
    uint16_t r = (h < 56u) ? (uint16_t)(h / 3u) : 18u;
    gui_fill_round_rect(x, y, w, h, r, fill_for_edge(edge));
    gui_stroke_round_rect(x, y, w, h, r, (h < 56u) ? 3u : 5u, edge);

    uint16_t cx = (uint16_t)(x + w / 2u);
    /* Usable width inside the 5 px stroke and its corner radius. */
    uint16_t avail = (w > 32u) ? (uint16_t)(w - 32u) : w;
    bool two = (line2 && *line2);

    /* The face is chosen by what FITS, not by the button's
     * height alone. See the note above this function for the defect this
     * replaces. */
    uint8_t start = (h >= 78u) ? 0u : 1u;

    if (two)
    {
        uint8_t i1 = face_that_fits(line1, avail, start);
        /* A subtitle is subordinate type and is never the same size as the
         * label above it - one step down, then shrink further if needed. */
        uint8_t i2 = face_that_fits(line2, avail, (uint8_t)(i1 + 1u < 3u ? i1 + 1u : 2u));

        const ui_font_t *f1 = FACE_LADDER[i1];
        const ui_font_t *f2 = FACE_LADDER[i2];

        uint16_t total = (uint16_t)(f1->line_height + f2->line_height);
        uint16_t top   = (h > total) ? (uint16_t)(y + (h - total) / 2u) : y;
        gui_font_text_ellipsis(cx, top, line1, avail, THEME_INK, f1);
        gui_font_text_ellipsis(cx, (uint16_t)(top + f1->line_height), line2,
                               avail, THEME_INK_SOFT, f2);
    }
    else if (line1 && *line1)
    {
        const ui_font_t *f = FACE_LADDER[face_that_fits(line1, avail, start)];
        uint16_t top = (h > f->line_height)
                     ? (uint16_t)(y + (h - f->line_height) / 2u) : y;
        gui_font_text_ellipsis(cx, top, line1, avail, THEME_INK, f);
    }
}

/* Bottom message panel: Lumio's greeting, the SD warning, transient notes. */
static void draw_dialog_panel(const char *text)
{
    gui_fill_round_rect(DLG_BOX_X, DLG_BOX_Y, DLG_BOX_W, DLG_BOX_H, 16u, THEME_ROSE_FILL);
    gui_stroke_round_rect(DLG_BOX_X, DLG_BOX_Y, DLG_BOX_W, DLG_BOX_H, 16u, 3u, THEME_FRAME);
    if (text && *text)
    {
        uint16_t lines = 1u;
        for (const char *p = text; *p; p++) if (*p == '\n') lines++;

        /* Step down to the small face rather than run out of the panel.
         * An earlier build had instruction messages that were
         * four lines against a two-line panel, so the tail was drawn below
         * the box and looked cut off. Choosing the face by measured height
         * means a long transient message degrades instead of overflowing. */
        const ui_font_t *f = &ui_font_md;
        if ((uint16_t)(lines * f->line_height) > (uint16_t)(DLG_BOX_H - 6u))
            f = &ui_font_sm;

        uint16_t total = (uint16_t)(lines * f->line_height);
        uint16_t top   = (DLG_BOX_H > total)
                       ? (uint16_t)(DLG_BOX_Y + (DLG_BOX_H - total) / 2u)
                       : (uint16_t)(DLG_BOX_Y + 3u);
        gui_font_text_centered((uint16_t)(s_gui_width / 2u), top, text,
                               THEME_INK, f);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Screens
 * ═══════════════════════════════════════════════════════════════════════════ */
void gui_draw_home_screen(void)
{
    gui_draw_frame();
    gui_draw_title_bar("SMART PILL DISPENSER", ACCENT_NEUTRAL);

    gui_draw_button(REG_BTN_X, REG_BTN_Y, REG_BTN_W, REG_BTN_H,
                    THEME_ROSE_EDGE, "REGISTER PATIENT", "");
    gui_draw_button(DISP_BTN_X, DISP_BTN_Y, DISP_BTN_W, DISP_BTN_H,
                    THEME_GREEN_EDGE, "DISPENSE PILLS", "");

    gui_blit_sprite(MASCOT_BG_X, MASCOT_BG_Y, &ui_sprite_mascot_a);

    draw_dialog_panel("Hello! I am Lumio - tap a button to begin.");

    flush_all();
}

void gui_draw_dialog_text(const char *text)
{
    draw_dialog_panel(text);
    flush_rows(DLG_BOX_Y, (uint16_t)(DLG_BOX_Y + DLG_BOX_H));
}

void gui_draw_ready_screen(const char *title, uint16_t accent, const char *dialog_msg)
{
    gui_draw_frame();
    gui_draw_title_bar(title, accent);

    gui_draw_button(READY_BTN_X, READY_BTN_Y, READY_BTN_W, READY_BTN_H,
                    accent, "I AM READY", "");

    gui_blit_sprite(MASCOT_BG_X, MASCOT_BG_Y, &ui_sprite_mascot_a);
    draw_dialog_panel(dialog_msg);

    flush_all();
}

/* ── Dispense flow ──────────────────────────────────────────────────────── */
/* The dose being dispensed, remembered so the progress callback can fill the
 * gems in as the bar advances without the caller passing it every step. */
static uint8_t s_dispense_gems = 0u;

/* One row of the dose, centred: `lit` of them in hopper colour, the rest as
 * empty outlines, so the row doubles as a count and as a progress readout. */
static void draw_dose_gems(uint8_t total, uint8_t lit)
{
    if (total == 0u) return;
    const uint16_t r    = 17u;
    const uint16_t step = 46u;
    const uint16_t row_w = (uint16_t)(step * (total - 1u) + 2u * r);
    uint16_t cx = (uint16_t)(s_gui_width / 2u - row_w / 2u + r);

    gui_draw_rect((uint16_t)(DOSE_GEM_CX - DOSE_GEM_MAX_W / 2u), DOSE_GEM_Y,
                  DOSE_GEM_MAX_W, DOSE_GEM_H, THEME_BG);

    for (uint8_t i = 0; i < total; i++)
    {
        gui_draw_gem(cx, (uint16_t)(DOSE_GEM_Y + DOSE_GEM_H / 2u), r,
                     (i < lit) ? GEM_RED : GEM_OFF);
        cx = (uint16_t)(cx + step);
    }
}

void gui_draw_dispensing_screen(const char *patient_name, uint8_t pill_count)
{
    gui_draw_frame();
    gui_draw_title_bar("DISPENSING", ACCENT_DISPENSE);

    char line[80];
    snprintf(line, sizeof(line), "For %s", patient_name ? patient_name : "");
    gui_font_text_centered((uint16_t)(s_gui_width / 2u), 138u, line,
                           THEME_INK, &ui_font_lg);
    snprintf(line, sizeof(line), "%u pill%s", (unsigned)pill_count,
             pill_count == 1u ? "" : "s");
    gui_font_text_centered((uint16_t)(s_gui_width / 2u), 190u, line,
                           THEME_INK_SOFT, &ui_font_md);

    s_dispense_gems = pill_count;
    draw_dose_gems(pill_count, 0u);

    /* Track; gui_draw_dispensing_progress() fills it. */
    gui_fill_round_rect(DISPENSE_BAR_X, DISPENSE_BAR_Y,
                        DISPENSE_BAR_W, DISPENSE_BAR_H, 16u, THEME_GREEN_FILL);
    gui_stroke_round_rect(DISPENSE_BAR_X, DISPENSE_BAR_Y,
                          DISPENSE_BAR_W, DISPENSE_BAR_H, 16u, 4u, THEME_GREEN_EDGE);

    flush_all();
}

void gui_draw_dispensing_progress(uint16_t percent, uint8_t pills_done)
{
    if (percent > 100u) percent = 100u;
    if (pills_done > s_dispense_gems) pills_done = s_dispense_gems;

    uint16_t pad     = 7u;
    uint16_t inner_x = (uint16_t)(DISPENSE_BAR_X + pad);
    uint16_t inner_y = (uint16_t)(DISPENSE_BAR_Y + pad);
    uint16_t inner_w = (uint16_t)(DISPENSE_BAR_W - 2u * pad);
    uint16_t inner_h = (uint16_t)(DISPENSE_BAR_H - 2u * pad);
    uint16_t fill_w  = (uint16_t)((uint32_t)inner_w * percent / 100u);

    gui_fill_round_rect(inner_x, inner_y, inner_w, inner_h, 12u, THEME_GREEN_FILL);
    if (fill_w > 12u)
        gui_fill_round_rect(inner_x, inner_y, fill_w, inner_h, 12u, THEME_GREEN_EDGE);

    /* Fill the dose row in step with the bar. pills_done is passed in rather
     * than derived from the percentage: the caller steps the bar per pill, so
     * it already knows the exact count, and rounding a percentage back into a
     * pill index made the gems light on the wrong steps. */
    draw_dose_gems(s_dispense_gems, pills_done);
    flush_rows(DOSE_GEM_Y, (uint16_t)(DOSE_GEM_Y + DOSE_GEM_H));

    /* The readout, one size up from the old md caption and tracked apart so
     * the digits stay distinct at arm's length. */
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)percent);
    gui_draw_rect(0u, PCT_TEXT_Y, s_gui_width, ui_font_lg.line_height, THEME_BG);
    gui_font_text_centered_tracked((uint16_t)(s_gui_width / 2u), PCT_TEXT_Y,
                                   pct, THEME_INK_SOFT, &ui_font_lg, 4u);

    flush_rows(DISPENSE_BAR_Y,
               (uint16_t)(PCT_TEXT_Y + ui_font_lg.line_height));
}

void gui_draw_confirm_taken_screen(void)
{
    gui_draw_frame();
    gui_draw_title_bar("TAKE YOUR PILLS NOW", ACCENT_DISPENSE);

    gui_draw_button(TAKEN_BTN_X, TAKEN_BTN_Y, TAKEN_BTN_W, TAKEN_BTN_H,
                    THEME_GREEN_EDGE, "I TOOK IT!", "");
    gui_draw_button(SKIP_BTN_X, SKIP_BTN_Y, SKIP_BTN_W, SKIP_BTN_H,
                    THEME_INK_SOFT, "SKIP", "");

    /* The camera pane. Drawn as a recessed placeholder here so the
     * layout is complete even when the overlay is compiled out or the watch
     * never starts - an empty hole would read as a fault. ai_overlay.c paints
     * live frames into exactly this rectangle. */
    /* Rounded, like the panel the overlay is about to draw over it, so the
     * pane does not visibly change SHAPE the moment the first camera frame
     * lands. ai_overlay_tick() strokes the anti-aliased border on its first
     * draw; this is what the user sees in the moment before that. */
    gui_fill_round_rect(CONFIRM_PV_X - 4u, CONFIRM_PV_Y - 4u,
                        CONFIRM_PV_W + 8u, CONFIRM_PV_H + 8u, 26u,
                        THEME_GREEN_EDGE);
    gui_fill_round_rect(CONFIRM_PV_X, CONFIRM_PV_Y,
                        CONFIRM_PV_W, CONFIRM_PV_H, 22u, THEME_INK_SOFT);
    gui_draw_text_centered(CONFIRM_PV_X + (CONFIRM_PV_W / 2u),
                           CONFIRM_PV_Y + (CONFIRM_PV_H / 2u) - 8u,
                           "WATCHING...", THEME_BG, 2);

    flush_all();
}

void gui_draw_taken_thankyou_screen(void)
{
    gui_draw_frame();
    gui_draw_title_bar("THANK YOU!", ACCENT_SUCCESS);
    gui_blit_sprite(MASCOT_BG_X, MASCOT_BG_Y, &ui_sprite_mascot_a);
    gui_font_text_centered(300u, 200u, "Your dose has\nbeen recorded.",
                           THEME_INK, &ui_font_lg);
    flush_all();
}

/* ── Alert / choice ─────────────────────────────────────────────────────── */
static void draw_alert_frame(const char *title, const char *message,
                             uint16_t accent, uint16_t msg_cx)
{
    gui_draw_frame();
    gui_draw_title_bar(title, accent);
    if (message && *message)
    {
        gui_font_text_centered(msg_cx, 150u, message, THEME_INK, &ui_font_md);
    }
}

void gui_draw_two_choice_screen(const char *title, const char *message,
                                uint16_t accent,
                                const char *left_label, const char *right_label)
{
    draw_alert_frame(title, message, accent, CHOICE_MSG_CX);

    /* First frame of the sad pose. anime_ui animates the remaining two over
     * this same box while the screen is up (MASCOT_ERROR). */
    gui_blit_sprite(MASCOT_SAD_X, MASCOT_SAD_Y, &ui_sprite_mascot_sad0);

    gui_draw_button(CHOICE_LEFT_X,  CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H,
                    THEME_GREEN_EDGE, left_label, "");
    gui_draw_button(CHOICE_RIGHT_X, CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H,
                    THEME_INK_SOFT, right_label, "");
    flush_all();
}

void gui_draw_alert_screen(const char *title, const char *message,
                           uint16_t accent, const char *button_label)
{
    draw_alert_frame(title, message, accent, (uint16_t)(s_gui_width / 2u));
    gui_draw_button(ALERT_BTN_X, ALERT_BTN_Y, ALERT_BTN_W, ALERT_BTN_H,
                    accent, button_label, "");
    flush_all();
}
