#!/usr/bin/env python3
"""img2sprite.py - turn PNGs into 4bpp palettised C sprites for an MCU.

This is the general, reusable version of what MedSight's
tools/gen_ui_assets.py does for the Lumio mascot. That script is
project-specific: hardcoded crop boxes, a hardcoded output path, fonts, a
synthesised blink frame. This one takes any images on the command line and
emits a self-contained .h/.c pair you can drop into any project.

    python img2sprite.py mascot*.png --height 213 -o assets

Output format (matches MedSight's ui_sprite_t so the two are interchangeable):

    typedef struct {
        uint16_t w, h, stride;
        const uint16_t *palette;   /* palette[0] is the transparent index */
        const uint8_t  *pixels;    /* 4bpp, 2 pixels per byte, high nibble
                                      first, rows padded to `stride` bytes */
    } sprite_t;

15 colours plus transparent, RGB565. A 214x213 sprite costs about 22 KB.

WHY 4bpp AND NOT RGB565 PIXELS
    Full-colour costs w*h*2 bytes - three 214x213 mascot frames would be
    267 KB and would not fit. Cartoon line art uses very few distinct
    colours, so a 16-entry palette is visually lossless here and costs
    w*h/2. It also makes index 0 a free alpha channel.

REQUIREMENTS
    pip install pillow numpy

--------------------------------------------------------------------------
THREE THINGS THAT WILL BITE YOU (all learned the hard way on this project)
--------------------------------------------------------------------------

1. TRANSPARENCY BY FLOOD FILL, NOT BY THRESHOLD.
   The obvious "every near-white pixel is transparent" rule punches holes
   through white artwork - a white cat on a white page loses its body. This
   fills inward from the border instead, so only background connected to
   the edge becomes transparent. --bg none turns it off.

2. --shared-bbox FOR ANIMATION FRAMES.
   Trimming each frame to its own content makes the character jump between
   frames, because a frame where the ears droop has a different bounding
   box. Passing --shared-bbox computes ONE box over all inputs so the
   frames register. Use it for any set of images drawn on the same canvas.

3. THE NEAR-WHITE SNAP NEEDS A NEUTRALITY GUARD.
   Median-cut lands a large white area on a slightly tinted off-white,
   which reads as a colour cast next to a pure-white UI. Snapping
   near-whites to #FFFFFF fixes that - but "min(r,g,b) >= 220" alone also
   catches pale pink blush and pale skin tones and bleaches them out. The
   guard is that the colour must also be near-NEUTRAL:
   max(r,g,b) - min(r,g,b) <= 10. Disable the whole thing with --no-snap.
"""

import argparse
import os
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs pillow and numpy:  pip install pillow numpy")


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def c_ident(s):
    """A safe C identifier from a filename."""
    out = ''.join(ch if ch.isalnum() else '_' for ch in s)
    if not out or out[0].isdigit():
        out = 's_' + out
    while '__' in out:
        out = out.replace('__', '_')
    return out.strip('_').lower()


# ── background removal ─────────────────────────────────────────────────────

def alpha_from_outside(im, thresh, bright):
    """Alpha mask: opaque everywhere except background reachable from the
    border. See note 1 in the module docstring for why this is a flood fill
    and not a threshold."""
    w, h = im.size
    flood = im.copy()
    sentinel = (255, 0, 255)
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
             (w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2)]
    for s in seeds:
        if sum(flood.getpixel(s)) >= bright:
            ImageDraw.floodfill(flood, s, sentinel, thresh=thresh)
    a = np.asarray(flood).astype(int)
    outside = (a[:, :, 0] == 255) & (a[:, :, 1] == 0) & (a[:, :, 2] == 255)
    return (~outside).astype(np.uint8) * 255


def content_bbox(alpha, pad=0):
    ys, xs = np.nonzero(alpha >= 128)
    if len(xs) == 0:
        return None
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    h, w = alpha.shape
    return (max(0, x0 - pad), max(0, y0 - pad),
            min(w, x1 + pad), min(h, y1 + pad))


# ── quantise + pack ────────────────────────────────────────────────────────

def make_sprite(im_rgb, alpha, name, ncolors=15, snap=True):
    w, h = im_rgb.size
    q = im_rgb.quantize(colors=ncolors, method=Image.MEDIANCUT,
                        dither=Image.NONE)
    pal = q.getpalette()[:ncolors * 3]
    idx = np.asarray(q).astype(np.uint8) + 1     # shift: index 0 reserved
    idx[alpha < 128] = 0                          # transparent

    if snap:
        for i in range(ncolors):
            r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
            # Near-white AND near-neutral - see note 3 in the docstring.
            if min(r, g, b) >= 220 and (max(r, g, b) - min(r, g, b)) <= 10:
                pal[i * 3:i * 3 + 3] = [255, 255, 255]
            elif max(r, g, b) <= 40:
                pal[i * 3:i * 3 + 3] = [20, 20, 20]

    palette = [0x0000] + [rgb565(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])
                          for i in range(ncolors)]

    stride = (w + 1) // 2
    packed = bytearray(stride * h)
    for y in range(h):
        row = idx[y]
        base = y * stride
        for x in range(w):
            if x & 1:
                packed[base + (x >> 1)] |= row[x] & 0x0F
            else:
                packed[base + (x >> 1)] |= (row[x] & 0x0F) << 4

    return dict(name=name, w=w, h=h, stride=stride,
                palette=palette, data=bytes(packed))


# ── emit ───────────────────────────────────────────────────────────────────

def carray(b, per=16, indent='    '):
    return '\n'.join(
        indent + ', '.join('0x%02X' % v for v in b[i:i + per]) + ','
        for i in range(0, len(b), per))


HEADER_TOP = """/* %(base)s.h - GENERATED by img2sprite.py. Do not edit by hand. */
#ifndef %(guard)s
#define %(guard)s

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 4bpp palettised sprite. palette[0] is the transparent index; `pixels` is
 * two pixels per byte, high nibble first, each row padded to `stride`. */
typedef struct {
    uint16_t        w, h;
    uint16_t        stride;
    const uint16_t *palette;
    const uint8_t  *pixels;
} sprite_t;

"""

BLIT_REFERENCE = """
/* Reference blitter - RGB565 framebuffer, index 0 skipped:
 *
 *   void blit(uint16_t *fb, int fb_w, int x, int y, const sprite_t *s)
 *   {
 *       for (int j = 0; j < s->h; j++)
 *           for (int i = 0; i < s->w; i++) {
 *               uint8_t byte = s->pixels[j * s->stride + (i >> 1)];
 *               uint8_t idx  = (i & 1) ? (byte & 0x0F) : (byte >> 4);
 *               if (idx) fb[(y + j) * fb_w + (x + i)] = s->palette[idx];
 *           }
 *   }
 */
"""


def emit(sprites, outdir, base, section):
    guard = c_ident(base).upper() + '_H'
    attr = ' __attribute__((section("%s")))' % section if section else ''

    h = [HEADER_TOP % dict(base=base, guard=guard)]
    for s in sprites:
        h.append('extern const sprite_t %s;   /* %ux%u, %u bytes */\n'
                 % (s['name'], s['w'], s['h'], len(s['data'])))
    h.append(BLIT_REFERENCE)
    h.append('\n#ifdef __cplusplus\n}\n#endif\n#endif /* %s */\n' % guard)

    c = ['/* %s.c - GENERATED by img2sprite.py. Do not edit by hand. */\n'
         '#include "%s.h"\n\n' % (base, base)]
    for s in sprites:
        c.append('static const uint16_t pal_%s[]%s = {\n    %s\n};\n'
                 % (s['name'], attr,
                    ', '.join('0x%04X' % v for v in s['palette'])))
        c.append('static const uint8_t px_%s[]%s = {\n%s\n};\n'
                 % (s['name'], attr, carray(s['data'])))
        c.append('const sprite_t %s = { %u, %u, %u, pal_%s, px_%s };\n\n'
                 % (s['name'], s['w'], s['h'], s['stride'],
                    s['name'], s['name']))

    os.makedirs(outdir, exist_ok=True)
    hp = os.path.join(outdir, base + '.h')
    cp = os.path.join(outdir, base + '.c')
    # ASCII on purpose: a stray non-UTF8 byte in a generated source file is
    # an annoying thing to debug through a cross-compiler.
    with open(hp, 'w', encoding='ascii', errors='replace', newline='\n') as f:
        f.write(''.join(h))
    with open(cp, 'w', encoding='ascii', errors='replace', newline='\n') as f:
        f.write(''.join(c))
    return hp, cp


def main():
    ap = argparse.ArgumentParser(
        description='PNG -> 4bpp palettised C sprite.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='examples:\n'
               '  python img2sprite.py cat.png --height 213\n'
               '  python img2sprite.py frame*.png --shared-bbox --height 153\n'
               '  python img2sprite.py icon.png --crop 279,646,867,1247 --size 44x44\n')
    ap.add_argument('images', nargs='+', help='input image files')
    ap.add_argument('-o', '--outdir', default='.', help='output directory')
    ap.add_argument('--base', default='sprites',
                    help='output basename (default: sprites)')
    ap.add_argument('--prefix', default='sprite_',
                    help='C symbol prefix (default: sprite_)')
    ap.add_argument('--crop', metavar='X0,Y0,X1,Y1',
                    help='crop all inputs to this box before anything else')
    ap.add_argument('--size', metavar='WxH', help='resize to exactly WxH')
    ap.add_argument('--height', type=int,
                    help='resize to this height, keeping aspect')
    ap.add_argument('--width', type=int,
                    help='resize to this width, keeping aspect')
    ap.add_argument('--colors', type=int, default=15,
                    help='palette entries excluding transparent (max 15)')
    ap.add_argument('--bg', choices=('auto', 'none'), default='auto',
                    help='auto: flood-fill background from the border')
    ap.add_argument('--bg-thresh', type=int, default=40,
                    help='flood-fill colour tolerance (default 40)')
    ap.add_argument('--bg-bright', type=int, default=700,
                    help='min sum(r,g,b) for a corner to count as background')
    ap.add_argument('--trim', dest='trim', action='store_true', default=True,
                    help='crop to content bounds (default)')
    ap.add_argument('--no-trim', dest='trim', action='store_false')
    ap.add_argument('--shared-bbox', action='store_true',
                    help='ONE trim box across all inputs - use for animation '
                         'frames so they register')
    ap.add_argument('--pad', type=int, default=0,
                    help='pixels of margin to keep when trimming')
    ap.add_argument('--no-snap', dest='snap', action='store_false',
                    default=True,
                    help='do not snap near-white/near-black palette entries')
    ap.add_argument('--section',
                    help='__attribute__((section("..."))) for the tables, '
                         'e.g. .text.assets to force them into ROM')
    args = ap.parse_args()

    if not 1 <= args.colors <= 15:
        sys.exit('--colors must be 1..15 (index 0 is transparent)')

    crop = None
    if args.crop:
        crop = tuple(int(v) for v in args.crop.replace(' ', '').split(','))
        if len(crop) != 4:
            sys.exit('--crop wants X0,Y0,X1,Y1')

    # Pass 1: load, crop, build alpha.
    loaded = []
    for path in args.images:
        im = Image.open(path).convert('RGB')
        if crop:
            im = im.crop(crop)
        if args.bg == 'auto':
            alpha = alpha_from_outside(im, args.bg_thresh, args.bg_bright)
        else:
            alpha = np.full((im.size[1], im.size[0]), 255, np.uint8)
        loaded.append([path, im, alpha])

    # Pass 2: trim. One shared box for frame sets, per-image otherwise.
    if args.trim:
        if args.shared_bbox:
            boxes = [content_bbox(a, args.pad) for _, _, a in loaded]
            boxes = [b for b in boxes if b]
            if boxes:
                box = (min(b[0] for b in boxes), min(b[1] for b in boxes),
                       max(b[2] for b in boxes), max(b[3] for b in boxes))
                print('shared bbox: %s' % (box,))
                for e in loaded:
                    e[1] = e[1].crop(box)
                    e[2] = e[2][box[1]:box[3], box[0]:box[2]]
        else:
            for e in loaded:
                box = content_bbox(e[2], args.pad)
                if box:
                    e[1] = e[1].crop(box)
                    e[2] = e[2][box[1]:box[3], box[0]:box[2]]

    # Pass 3: resize, quantise, pack.
    sprites, total = [], 0
    for path, im, alpha in loaded:
        w, h = im.size
        if args.size:
            tw, th = (int(v) for v in args.size.lower().split('x'))
        elif args.height:
            th = args.height
            tw = max(1, round(w * th / h))
        elif args.width:
            tw = args.width
            th = max(1, round(h * tw / w))
        else:
            tw, th = w, h

        if (tw, th) != (w, h):
            im = im.resize((tw, th), Image.LANCZOS)
            alpha = np.asarray(
                Image.fromarray(alpha).resize((tw, th), Image.LANCZOS))

        name = args.prefix + c_ident(os.path.splitext(
            os.path.basename(path))[0])
        s = make_sprite(im, alpha, name, args.colors, args.snap)
        sprites.append(s)
        total += len(s['data']) + len(s['palette']) * 2
        print('%-28s %3ux%-3u  %6u bytes  %s'
              % (s['name'], s['w'], s['h'], len(s['data']),
                 os.path.basename(path)))

    hp, cp = emit(sprites, args.outdir, args.base, args.section)
    print('\n%d sprite(s), %.1f KB of ROM' % (len(sprites), total / 1024.0))
    print('wrote %s\n      %s' % (hp, cp))


if __name__ == '__main__':
    main()
