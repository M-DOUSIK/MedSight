#!/usr/bin/env python3
"""
gen_ui_assets.py - MedSight UI asset generator.

Turns the reference artwork in tools/mascot_and_frames_design/ and a
permissively-licensed TTF into C arrays the firmware compiles into flash:

  * the mascot (Lumio) idle sprite, cropped from the designer's own PNG
  * the heart and pill-capsule corner motifs from the same sheet
  * two anti-aliased proportional fonts (DejaVu Sans Bold Oblique)

Output: firmware/FSBL/Src/ui/ui_assets.c
        firmware/FSBL/Inc/ui/ui_assets.h

Sprites are 4bpp palettised (15 colours + index 0 = transparent).
Fonts are 4bpp alpha coverage, proportional, with per-glyph metrics.
Everything lands in .text.msassets, i.e. the ROM region, so it does not
eat the RAM heap/stack margin.

Run from the repository root:  python tools/gen_ui_assets.py

Lives in tools/ with the artwork it reads; ROOT is resolved from this
file's location, so it still finds firmware/ and tools/ correctly.
"""

import os
import sys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, 'tools', 'mascot_and_frames_design')
SRC_SHEET = os.path.join(ART, 'State0-idle(1)', 'mascot - buddu_20260908135550.png')

OUT_C = os.path.join(ROOT, 'firmware', 'FSBL', 'Inc', 'ui', 'ui_assets_data.inc')
OUT_H = os.path.join(ROOT, 'firmware', 'FSBL', 'Inc', 'ui', 'ui_assets.h')

import matplotlib
FONT_TTF = os.path.join(os.path.dirname(matplotlib.__file__),
                        'mpl-data', 'fonts', 'ttf', 'DejaVuSans-BoldOblique.ttf')

SECTION = '__attribute__((section(".text.msassets")))'


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# ── sprite extraction ──────────────────────────────────────────────────────

def alpha_from_outside(im):
    """Flood-fill the near-white background inward from the border so the
    cat's own white fill stays opaque - only the surrounding page goes
    transparent."""
    w, h = im.size
    flood = im.copy()
    sentinel = (255, 0, 255)
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
             (w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2)]
    for s in seeds:
        if sum(flood.getpixel(s)) > 700:
            ImageDraw.floodfill(flood, s, sentinel, thresh=40)
    a = np.asarray(flood).astype(int)
    outside = (a[:, :, 0] == 255) & (a[:, :, 1] == 0) & (a[:, :, 2] == 255)
    return (~outside).astype(np.uint8) * 255


def make_sprite(im_rgb, alpha, target_w, target_h, name, ncolors=15):
    im_rgb = im_rgb.resize((target_w, target_h), Image.LANCZOS)
    al = Image.fromarray(alpha).resize((target_w, target_h), Image.LANCZOS)
    al = np.asarray(al)

    q = im_rgb.quantize(colors=ncolors, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = q.getpalette()[:ncolors * 3]
    idx = np.asarray(q).astype(np.uint8) + 1          # shift: 0 reserved
    idx[al < 128] = 0                                  # transparent

    # Median-cut tends to land the big white body on a slightly tinted
    # off-white, which reads as a colour cast against the white UI. Snap
    # near-whites back to pure white, and near-blacks to the ink colour, so
    # the line art stays crisp.
    for i in range(ncolors):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        # Only snap near-NEUTRAL near-whites. A pale pink blush is also very
        # light, and snapping it too would erase the cheeks and inner ears.
        if min(r, g, b) >= 220 and (max(r, g, b) - min(r, g, b)) <= 10:
            pal[i * 3:i * 3 + 3] = [255, 255, 255]
        elif max(r, g, b) <= 40:
            pal[i * 3:i * 3 + 3] = [20, 20, 20]

    palette = [0x0000] + [rgb565(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])
                          for i in range(ncolors)]

    stride = (target_w + 1) // 2
    packed = bytearray(stride * target_h)
    for y in range(target_h):
        row = idx[y]
        for x in range(target_w):
            if x & 1:
                packed[y * stride + (x >> 1)] |= row[x] & 0x0F
            else:
                packed[y * stride + (x >> 1)] |= (row[x] & 0x0F) << 4
    return dict(name=name, w=target_w, h=target_h, palette=palette,
                data=bytes(packed), stride=stride)


def components(mask, minpix=200):
    """4-connected components, largest first, as (pixels, x0, y0, x1, y1)."""
    lab = np.zeros(mask.shape, np.int32)
    out, n = [], 0
    H, W = mask.shape
    for sy in range(H):
        for sx in range(W):
            if mask[sy, sx] and lab[sy, sx] == 0:
                n += 1
                stack = [(sy, sx)]
                lab[sy, sx] = n
                y0 = y1 = sy
                x0 = x1 = sx
                cnt = 0
                while stack:
                    y, x = stack.pop()
                    cnt += 1
                    y0 = min(y0, y); y1 = max(y1, y)
                    x0 = min(x0, x); x1 = max(x1, x)
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and lab[ny, nx] == 0:
                            lab[ny, nx] = n
                            stack.append((ny, nx))
                if cnt >= minpix:
                    out.append((cnt, x0, y0, x1, y1))
    out.sort(reverse=True)
    return out


def make_blink(crop):
    """Turn the awake face into a blinking one: find the two eyes and replace
    each with a closed-eye arc in the same ink colour. The designer drew no
    blink frame, so this is synthesised in the same style rather than
    inventing a new pose."""
    im = crop.copy()
    a = np.asarray(im).astype(int)
    h, w = a.shape[:2]
    dark = (a.sum(axis=2) < 260)
    band = np.zeros_like(dark)
    band[int(h * 0.20):int(h * 0.50), int(w * 0.20):int(w * 0.80)] = True
    cand = []
    for c in components(dark & band, 400):
        cw, ch = c[3] - c[1], c[4] - c[2]
        if 0.05 * w < cw < 0.20 * w and 0.6 < cw / max(ch, 1) < 1.7:
            cand.append(c)
    eyes = cand[:2]
    if len(eyes) == 2 and abs((eyes[0][2] + eyes[0][4]) - (eyes[1][2] + eyes[1][4])) > 0.15 * h:
        eyes = []
    if len(eyes) != 2:
        print(f'    blink: expected 2 eyes, found {len(eyes)} - leaving frame as-is')
        return im
    eyes.sort(key=lambda c: c[1])
    d = ImageDraw.Draw(im)
    for _, x0, y0, x1, y1 in eyes:
        pad = 3
        d.rectangle([x0 - pad, y0 - pad, x1 + pad, y1 + pad], fill=(255, 255, 255))
        cy = (y0 + y1) // 2
        d.arc([x0, cy - (y1 - y0) // 3, x1, cy + (y1 - y0) // 2],
              start=200, end=340, fill=(20, 20, 20), width=max(3, (x1 - x0) // 9))
    print(f'    blink: eyes at {[(c[1], c[2], c[3], c[4]) for c in eyes]}')
    return im


# ── font rasterisation ─────────────────────────────────────────────────────

FIRST, LAST = 32, 126


def make_font(px, name, first=FIRST, last=LAST):
    f = ImageFont.truetype(FONT_TTF, px)
    asc, desc = f.getmetrics()
    glyphs, blob = [], bytearray()

    for c in range(first, last + 1):
        ch = chr(c)
        box = f.getbbox(ch)
        adv = int(round(f.getlength(ch)))
        if box is None or box[2] <= box[0] or box[3] <= box[1]:
            glyphs.append((0, 0, 0, 0, adv, 0))
            continue
        x0, y0, x1, y1 = box
        gw, gh = x1 - x0, y1 - y0
        img = Image.new('L', (gw + 4, gh + 4), 0)
        ImageDraw.Draw(img).text((-x0 + 2, -y0 + 2), ch, font=f, fill=255)
        a = np.asarray(img)
        ys, xs = np.where(a > 8)
        if len(xs) == 0:
            glyphs.append((0, 0, 0, 0, adv, 0))
            continue
        cx0, cy0, cx1, cy1 = xs.min(), ys.min(), xs.max(), ys.max()
        a = a[cy0:cy1 + 1, cx0:cx1 + 1]
        gh, gw = a.shape
        xoff = x0 - 2 + int(cx0)
        yoff = y0 - 2 + int(cy0)          # from the text origin (top of ascent)

        stride = (gw + 1) // 2
        off = len(blob)
        rowbuf = bytearray(stride * gh)
        for y in range(gh):
            for x in range(gw):
                v = int(a[y, x]) >> 4                  # 8bpp -> 4bpp coverage
                if x & 1:
                    rowbuf[y * stride + (x >> 1)] |= v
                else:
                    rowbuf[y * stride + (x >> 1)] |= v << 4
        blob += rowbuf
        glyphs.append((gw, gh, xoff, yoff, adv, off))

    return dict(name=name, px=px, line=asc + desc, baseline=asc,
                glyphs=glyphs, blob=bytes(blob), first=first, last=last)


# ── emit ───────────────────────────────────────────────────────────────────

def carray(b, per=16, indent='    '):
    out = []
    for i in range(0, len(b), per):
        out.append(indent + ', '.join('0x%02X' % v for v in b[i:i + per]) + ',')
    return '\n'.join(out)


def main():
    sheet = Image.open(SRC_SHEET).convert('RGB')

    # theme colours, sampled from the designer's own artwork
    theme = {
        'FRAME': sheet.getpixel((95, 800)),      # blush frame band
        'BLUSH': sheet.getpixel((470, 875)),     # cheek blush
        'INK':   (20, 20, 20),
    }
    print('sampled theme:', theme)

    awake = Image.open(os.path.join(ART, '(9) pill taken gif',
                                    'mascot - buddu_20260908141459.png')).convert('RGB')
    wave  = Image.open(os.path.join(ART, '(9) pill taken gif',
                                    'mascot - buddu_20260908141510.png')).convert('RGB')

    def cat_box(im):
        a = np.asarray(im).astype(int)
        nw = (a.sum(axis=2) < 730)
        sub = nw[655:1320, 280:950]         # x-range excludes the corner motifs
        ys, xs = np.where(sub)
        return (280 + xs.min(), 655 + ys.min(), 280 + xs.max() + 1, 655 + ys.max() + 1)

    box_a, box_b = cat_box(awake), cat_box(wave)
    # One shared box so the two frames register against each other exactly -
    # otherwise the cat jitters between frames instead of waving.
    box = (min(box_a[0], box_b[0]), min(box_a[1], box_b[1]),
           max(box_a[2], box_b[2]), max(box_a[3], box_b[3]))
    print('  cat box:', box)

    MW = 214
    MH = int(round(MW * (box[3] - box[1]) / (box[2] - box[0])))
    print(f'  mascot frame: {MW}x{MH}')

    crop_a, crop_b = awake.crop(box), wave.crop(box)
    parts = []
    for nm, cr in (('mascot_a', crop_a), ('mascot_b', crop_b),
                   ('mascot_blink', make_blink(crop_a))):
        parts.append(make_sprite(cr, alpha_from_outside(cr), MW, MH, nm))
        print(f'  sprite {nm}: {MW}x{MH} = {len(parts[-1]["data"])} bytes')

    # Sad pose - all three frames of the designer's error GIF, so the
    # MASCOT_ERROR state animates rather than sitting on one still.
    SAD_FILES = ['mascot - buddu_20260908135418.png',
                 'mascot - buddu_20260908135433.png',
                 'mascot - buddu_20260908135451.png']
    sad_sheets = [Image.open(os.path.join(ART, '(10) error message gif', f)).convert('RGB')
                  for f in SAD_FILES]

    def sad_box_of(im):
        a = np.asarray(im).astype(int)
        m = (a.sum(axis=2) < 730)
        sub = m[600:1278, 200:1000]      # below the title, above the corner motifs
        ys, xs = np.where(sub)
        return (200 + xs.min(), 600 + ys.min(), 200 + xs.max() + 1, 600 + ys.max() + 1)

    boxes = [sad_box_of(im) for im in sad_sheets]
    # One shared box so the frames register against each other exactly.
    sad_box = (min(b[0] for b in boxes), min(b[1] for b in boxes),
               max(b[2] for b in boxes), max(b[3] for b in boxes))
    SW = 150
    SH = int(round(SW * (sad_box[3] - sad_box[1]) / (sad_box[2] - sad_box[0])))
    print(f'  sad box: {sad_box} -> {SW}x{SH}')
    for i, im in enumerate(sad_sheets):
        cr = im.crop(sad_box)
        parts.append(make_sprite(cr, alpha_from_outside(cr), SW, SH, f'mascot_sad{i}'))
        print(f'  sprite mascot_sad{i}: {SW}x{SH} = {len(parts[-1]["data"])} bytes')

    for nm, bx, tw, th in [
        ('sparkle_lg', (346, 435, 406, 521), 34, 48),
        ('sparkle_sm', (257, 592, 302, 639), 24, 25),
        ('heart',      (142, 307, 244, 391), 46, 38),
        ('capsule',    (969, 285, 1101, 418), 44, 44),
    ]:
        src = awake if nm.startswith('sparkle') else sheet
        cr = src.crop(bx)
        parts.append(make_sprite(cr, alpha_from_outside(cr), tw, th, nm))
        print(f'  sprite {nm}: {tw}x{th} = {len(parts[-1]["data"])} bytes')

    fonts = [make_font(34, 'lg'), make_font(23, 'md'), make_font(18, 'sm'),
             # digits only - the pill-count screen wants one very large numeral
             make_font(76, 'num', first=ord('0'), last=ord('9'))]
    for f in fonts:
        print(f'  font {f["name"]}: {f["px"]}px = {len(f["blob"])} bytes')

    total = sum(len(p['data']) for p in parts) + sum(len(f['blob']) for f in fonts)
    print(f'total asset bytes ~= {total} ({total/1024:.1f} KB)')

    # ---- header ----
    h = ['/* ui_assets.h - GENERATED by tools/gen_ui_assets.py. Do not edit. */',
         '#ifndef UI_ASSETS_H', '#define UI_ASSETS_H', '',
         '#include <stdint.h>', '',
         '#ifdef __cplusplus', 'extern "C" {', '#endif', '',
         '/* 4bpp palettised sprite; palette[0] is the transparent index. */',
         'typedef struct {', '    uint16_t        w, h;', '    uint16_t        stride;',
         '    const uint16_t *palette;', '    const uint8_t  *pixels;',
         '} ui_sprite_t;', '',
         '/* One glyph of a 4bpp anti-aliased proportional font. */',
         'typedef struct {', '    uint8_t  w, h;', '    int8_t   xoff, yoff;',
         '    uint8_t  advance;', '    uint16_t offset;', '} ui_glyph_t;', '',
         'typedef struct {', '    uint8_t           first, last;',
         '    uint8_t           line_height, baseline;',
         '    const ui_glyph_t *glyphs;', '    const uint8_t    *blob;',
         '} ui_font_t;', '']
    for p in parts:
        h.append(f'extern const ui_sprite_t ui_sprite_{p["name"]};')
    h.append('')
    for f in fonts:
        h.append(f'extern const ui_font_t ui_font_{f["name"]};')
    h += ['',
          '/* Theme colours sampled from the reference artwork (RGB565). */',
          f'#define THEME_FRAME_PINK  0x{rgb565(*theme["FRAME"]):04X}',
          f'#define THEME_BLUSH       0x{rgb565(*theme["BLUSH"]):04X}',
          '',
          '#ifdef __cplusplus', '}', '#endif', '', '#endif /* UI_ASSETS_H */', '']

    # ---- source ----
    c = ['/* ui_assets_data.inc - GENERATED by tools/gen_ui_assets.py. Do not edit.',
         ' *',
         ' * Mascot and corner motifs are cropped from the project designer\'s',
         ' * own original artwork (tools/mascot_and_frames_design/) - an',
         ' * original character, no third-party character IP.',
         ' * Fonts are rasterised from DejaVu Sans Bold Oblique, whose licence',
         ' * permits redistribution and embedding; see THIRD_PARTY_SOFTWARE.md.',
         ' *',
         ' * Included once, by gui_draw.c. It is an .inc rather than a .c on',
         ' * purpose: adding a new .c would need a matching <link> entry in',
         ' * STM32CubeIDE .project, and an already-open IDE keeps a stale copy',
         ' * of that file in memory, so the new unit silently never compiles',
         ' * and the link fails on the assets. Including it into a file that is',
         ' * already in the build removes that failure mode entirely.',
         ' */', '']

    for p in parts:
        n = p['name']
        c.append(f'static const uint16_t pal_{n}[16] {SECTION} = {{')
        c.append('    ' + ', '.join('0x%04X' % v for v in p['palette']))
        c.append('};')
        c.append(f'static const uint8_t px_{n}[] {SECTION} = {{')
        c.append(carray(p['data']))
        c.append('};')
        c.append(f'const ui_sprite_t ui_sprite_{n} = {{ {p["w"]}, {p["h"]}, '
                 f'{p["stride"]}, pal_{n}, px_{n} }};')
        c.append('')

    for f in fonts:
        n = f['name']
        c.append(f'static const ui_glyph_t gl_{n}[] {SECTION} = {{')
        for (gw, gh, xo, yo, adv, off) in f['glyphs']:
            c.append(f'    {{ {gw}, {gh}, {xo}, {yo}, {adv}, {off} }},')
        c.append('};')
        c.append(f'static const uint8_t blob_{n}[] {SECTION} = {{')
        c.append(carray(f['blob']))
        c.append('};')
        c.append(f'const ui_font_t ui_font_{n} = {{ {f["first"]}, {f["last"]}, '
                 f'{f["line"]}, {f["baseline"]}, gl_{n}, blob_{n} }};')
        c.append('')

    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    open(OUT_H, 'w', encoding='ascii', errors='replace', newline='\n').write('\n'.join(h))
    open(OUT_C, 'w', encoding='ascii', errors='replace', newline='\n').write('\n'.join(c))
    print('wrote', OUT_H)
    print('wrote', OUT_C)


if __name__ == '__main__':
    main()
