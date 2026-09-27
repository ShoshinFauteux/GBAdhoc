#!/usr/bin/env python3
"""Bake a TTF into an 8-bit coverage atlas + glyph table as a C header.

The PSP UI used a 1-bit 8x16 bitmap font, which has no anti-aliasing and a
fixed advance.  This produces what RetroShell PSP produces: an alpha atlas
uploaded as GU_PSM_T8 with an alpha CLUT, so the GE modulates vertex colour
by glyph coverage.  Baking on the host keeps the EBOOT self-contained -- no
runtime font asset to ship or lose.

Usage: bake_font.py <out.h> [<font dir>]        (default: assets/fonts)

    python tools/bake_font.py psp/font_ui.h

BUTTON GLYPHS.  The browser's hints name PSP face buttons, and "TRI" / "SQ"
in text both cost more pixels than the 480 px footer has and read worse than
the symbols the PSP's own dialogs use.  The EXTRAS table below appends six
glyphs after the printable ASCII range, at bytes 0x7F..0x84:

    0x7F  triangle    0x80  circle    0x81  cross
    0x82  square      0x83  star      0x84  star outline

Five come from Inter itself (U+25B3 U+25CB U+25A1 U+2605 U+2606), rasterised
SMALLER than the text so a 9-10 px shape sits on the cap midline of the line
instead of towering over the lowercase.  The cross is synthesised: Inter's
U+2715 is a full-height multiplication mark, and the PSP's cross is square.
Every extra is anti-aliased coverage like any other glyph, so vid_text needs
no special case -- video_psp.h names them (VID_GLYPH_*), and C code writes
`VID_GLYPH_X " play"`.  (Concatenation, not "\\x81 play": a hex escape
would swallow a following hex digit.)
"""
import sys, os
from PIL import Image, ImageFont, ImageDraw

FIRST, LAST = 0x20, 0x7E

# Atlas dimensions MUST BOTH be powers of two: the PSP's GE samples nothing
# else, and a non-power-of-two height does not fail loudly -- it silently
# reads the wrong texels, so every glyph draws as a smeared neighbour while
# the layout stays perfect.  A 256x96 `ui` atlas cost an hour of staring at
# text that was almost right.
FACES = [
    # name,  file,                px, atlas w/h
    ('ui',   'Inter-Regular.ttf',  15, 256, 128),
    ('hd',   'Inter-SemiBold.ttf', 19, 256, 128),
]

# byte, source ('cross' = synthesised), render px (ui, hd), advance (ui, hd)
EXTRAS = [
    (0x7F, '△', (10, 13), (12, 15)),   # triangle
    (0x80, '○', (10, 13), (12, 15)),   # circle
    (0x81, 'cross',  (9, 12),  (12, 15)),   # cross
    (0x82, '□', (10, 13), (12, 15)),   # square
    (0x83, '★', (11, 14), (12, 15)),   # star (a point larger: its
    (0x84, '☆', (11, 14), (12, 15)),   # outline is thin at 10 px)
]
# Where an extra's vertical centre goes, in line-box rows: the middle of the
# capitals.  Inter caps at 15 px span rows 4..15 of the ui line (asc 15),
# and at 19 px rows 5..19 of the hd line.
EXTRA_MID = {'ui': 9.5, 'hd': 12.0}


def render_cross(size):
    """Two anti-aliased diagonals, drawn 4x and averaged down."""
    s = 4
    big = Image.new('L', (size * s, size * s), 0)
    d = ImageDraw.Draw(big)
    stroke = max(1, int(round(size * s * 0.14)))
    pad = int(round(size * s * 0.06))
    d.line((pad, pad, size * s - 1 - pad, size * s - 1 - pad),
           fill=255, width=stroke)
    d.line((size * s - 1 - pad, pad, pad, size * s - 1 - pad),
           fill=255, width=stroke)
    return big.resize((size, size), Image.LANCZOS)


def render_extra(path, src, px):
    if src == 'cross':
        return render_cross(px)
    font = ImageFont.truetype(path, px)
    x0, y0, x1, y1 = font.getbbox(src)
    cell = Image.new('L', (max(1, x1 - x0), max(1, y1 - y0)), 0)
    ImageDraw.Draw(cell).text((-x0, -y0), src, font=font, fill=255)
    return cell


class Packer:
    def __init__(self, name, aw, ah):
        self.name, self.aw, self.ah = name, aw, ah
        self.atlas = Image.new('L', (aw, ah), 0)
        self.pen_x = self.pen_y = self.row_h = 0

    def put(self, cell):
        w, h = cell.size
        if self.pen_x + w + 1 > self.aw:
            self.pen_x = 0
            self.pen_y += self.row_h + 1
            self.row_h = 0
        if self.pen_y + h > self.ah:
            raise SystemExit('%s: atlas %dx%d too small'
                             % (self.name, self.aw, self.ah))
        self.atlas.paste(cell, (self.pen_x, self.pen_y))
        x, y = self.pen_x, self.pen_y
        self.pen_x += w + 1
        self.row_h = max(self.row_h, h)
        return x, y


def bake(name, path, px, aw, ah, face_idx):
    if aw & (aw - 1) or ah & (ah - 1):
        raise SystemExit('%s: atlas %dx%d is not power-of-two' % (name, aw, ah))
    font = ImageFont.truetype(path, px)
    asc, desc = font.getmetrics()
    pk = Packer(name, aw, ah)
    glyphs = []
    for cp in range(FIRST, LAST + 1):
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        bbox = font.getbbox(ch)            # (x0, y0, x1, y1) from the origin
        if bbox is None:
            bbox = (0, 0, 0, 0)
        x0, y0, x1, y1 = bbox
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if w and h:
            cell = Image.new('L', (w, h), 0)
            ImageDraw.Draw(cell).text((-x0, -y0), ch, font=font, fill=255)
            x, y = pk.put(cell)
            glyphs.append((x, y, w, h, x0, y0, adv))
        else:
            glyphs.append((0, 0, 0, 0, 0, 0, adv))
    # The button glyphs follow, contiguous with the ASCII range so the
    # existing `c - FU_FIRST` indexing reaches them.
    expect = LAST + 1
    for byte, src, pxs, advs in EXTRAS:
        if byte != expect:
            raise SystemExit('EXTRAS must be contiguous from 0x%02X' % (LAST + 1))
        expect += 1
        cell = render_extra(path, src, pxs[face_idx])
        w, h = cell.size
        adv = advs[face_idx]
        x, y = pk.put(cell)
        xoff = (adv - w) // 2
        yoff = int(round(EXTRA_MID[name] - h / 2.0))
        glyphs.append((x, y, w, h, xoff, yoff, adv))
    return asc, desc, pk.atlas, glyphs


def main():
    out = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'fonts')
    last = LAST + len(EXTRAS)
    parts = ['/* GENERATED by tools/bake_font.py -- do not edit by hand.\n'
             ' *\n'
             ' * Inter (SIL Open Font License 1.1) baked to 8-bit coverage\n'
             ' * atlases.  See THIRD_PARTY.md.  Bytes 0x7F..0x%02X are the PSP\n'
             ' * button glyphs (video_psp.h VID_GLYPH_*). */\n'
             '#ifndef FONT_UI_H\n#define FONT_UI_H\n\n'
             '#define FU_FIRST %d\n#define FU_LAST  %d\n'
             '#define FU_COUNT (FU_LAST - FU_FIRST + 1)\n\n'
             'typedef struct {\n'
             '   unsigned short x, y;\n'
             '   unsigned char  w, h;\n'
             '   signed char    xoff, yoff, xadv;\n'
             '} fu_glyph;\n\n' % (last, FIRST, last)]
    for idx, (name, fn, px, aw, ah) in enumerate(FACES):
        asc, desc, atlas, glyphs = bake(name, os.path.join(src, fn), px,
                                        aw, ah, idx)
        parts.append('/* %s: %s @%dpx, ascent %d, descent %d */\n'
                     % (name, fn, px, asc, desc))
        parts.append('#define FU_%s_W   %d\n#define FU_%s_H   %d\n'
                     '#define FU_%s_ASC %d\n#define FU_%s_LH  %d\n'
                     % (name.upper(), aw, name.upper(), ah,
                        name.upper(), asc, name.upper(), asc + desc))
        parts.append('static const fu_glyph fu_%s_g[FU_COUNT] = {\n' % name)
        for g in glyphs:
            parts.append('   {%d,%d,%d,%d,%d,%d,%d},\n' % g)
        parts.append('};\n')
        data = atlas.tobytes()
        # The GE reads texture data straight out of RAM and needs a 16-byte
        # aligned base.  Without this the sampler starts a few bytes into the
        # atlas and EVERY glyph draws as a smeared neighbour -- the layout
        # stays perfect, so it reads as a font bug rather than an alignment
        # one.  The two atlases differed only in where the linker put them.
        parts.append('static const unsigned char __attribute__((aligned(16)))'
                     ' fu_%s_a[%d] = {\n' % (name, len(data)))
        for i in range(0, len(data), 32):
            parts.append(''.join('%d,' % b for b in data[i:i + 32]) + '\n')
        parts.append('};\n\n')
    parts.append('#endif /* FONT_UI_H */\n')
    open(out, 'w', newline='\n').write(''.join(parts))
    print('wrote %s (%.0f KiB)' % (out, os.path.getsize(out) / 1024.0))


main()
