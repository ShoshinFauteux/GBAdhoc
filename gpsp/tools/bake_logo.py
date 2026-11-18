#!/usr/bin/env python3
"""Bake the GBAdhoc wordmark: the in-app atlas, and (with --xmb) the XMB art.

    python tools/bake_logo.py                 -> psp/logo_ui.h
    python tools/bake_logo.py --xmb           -> also psp/assets/gbadhoc_icon0.png
                                                 and psp/assets/gbadhoc_pic1.png
    python tools/bake_logo.py --check         -> exit 1 if psp/logo_ui.h differs

SOURCE.  tools/logo/gbadhoc-logo-3-master@2x.png, the README's 2x master,
through tools/logo/logomark.py (the logo design study, 2026-10-02).  The old
baker cropped BAND = (130, 202, 349, 238) out of psp/assets/gbadhoc_pic1.png
and LANCZOS-resampled it 0.59x with autocontrast: a raster of a raster at a
non-integer scale (27 % of inked pixels mid-grey, cap line half a pixel off),
and once PIC1 was re-composed that band was the tagline, so re-running it
would have baked "PSP . GBA . GB . GBC EMULATOR".  Nothing here crops a
derived image any more.

THE ATLAS (psp/logo_ui.h).  The letters are a 1:6 design (stroke 18.5,
ascender 111 at 2x), so they snap: at a 4 px stroke the wordmark is 151x24.
logomark.render_letters(stroke_px=4, hint=True) measures the 13 vertical
stems and 5 horizontal bar levels on the mask and remaps each axis piecewise
linearly so every stem is exactly 4 px on whole pixels and ascender / cap /
x-height / baseline sit on whole rows, then box-filters.  Stored as 8-bit
COVERAGE in a 256x32 T8 texture (8 KiB of .rodata, unchanged): the GE draws
it through the same T8 + alpha-CLUT path as the text, so the theme tints it,
and vid_logo samples it NEAREST at 1:1.  Both texture dimensions must be
powers of two -- the GE silently reads the wrong texels otherwise.

THE XMB ART (--xmb).  ICON0 144x80: the 3 px hinted letters (114x18) with the
rainbow re-spaced for the icon (2 px arcs, 1 px gaps, outer radius 19) and a
2 px bar, on black (the dark theme).  PIC1 480x272: the 9 px letters (341x55),
the analytic rainbow and a 4 px bar, tagline in Inter SemiBold 15, on black.
The rainbow is analytic with butt caps on the ascender line: no "beads" at
the arc ends (the published master draws one at every end; that was the
dots).  Requires numpy and Pillow.
"""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'logo'))
import logomark as M  # noqa: E402

ATLAS_OUT = os.path.join(ROOT, 'psp', 'logo_ui.h')
ICON0_OUT = os.path.join(ROOT, 'psp', 'assets', 'gbadhoc_icon0.png')
PIC1_OUT = os.path.join(ROOT, 'psp', 'assets', 'gbadhoc_pic1.png')
FONT_SB = os.path.join(ROOT, 'assets', 'fonts', 'Inter-SemiBold.ttf')

STROKE_PX = 4                    # -> 151 x 24; must match VID_LOGO_W/H
TEX_W, TEX_H = 256, 32           # power of two, 8 KiB

DARK_BG = (0, 0, 0)              # the dark theme page (C_BG_TOP)
TAG_DARK = (139, 139, 139)       # C_ACCENT_DK on the dark theme


def atlas_cov():
    cov, _ = M.render_letters(stroke_px=STROKE_PX, hint=True)
    return cov


def write_atlas(path):
    cov = atlas_cov()
    h, w = cov.shape
    M.write_logo_header(path, cov, TEX_W, TEX_H,
                        note='Grid-snapped: every stem %d px, bars %d px, '
                             'cap/x-height/baseline on whole rows (%dx%d).'
                             % (STROKE_PX, STROKE_PX, w, h))
    return w, h


def icon0(bg=DARK_BG, ink=M.DARK_INK):
    """ICON0, 144x80: 3 px letters, the icon-spaced rainbow, a 2 px bar."""
    W, H = 144, 80
    L, _ = M.render_letters(stroke_px=3, hint=True)
    lh, lw = L.shape
    radii = [19 - 3 * i - 1 for i in range(5)]      # 18,15,12,9,6 (centre lines)
    rb_stroke, rb_h, bar_h = 2.0, 20, 2
    total = rb_h + lh + 3 + bar_h
    oy = (H - total) // 2 + rb_h
    ox = (W - lw) // 2
    cx = ox + M.RB_CX * (3 / M.STROKE)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    dist = np.sqrt((xx + 0.5 - cx) ** 2 + (yy + 0.5 - oy) ** 2)
    half = np.clip(oy - (yy + 0.5) + 0.5, 0, 1)
    layers = []
    for col, r in zip(M.RB_COL, radii):
        cov = np.clip(rb_stroke / 2 + 0.5 - np.abs(dist - r), 0, 1) * half
        layers.append((M.hexrgb(col), cov.astype(np.float32)))
    letters = np.zeros((H, W), np.float32)
    letters[oy:oy + lh, ox:ox + lw] = L
    layers.append((ink, letters))
    layers += M.bar_layers(3 / M.STROKE, ox, oy, (H, W), y=lh + 3, h=bar_h,
                           width=lw)
    bgarr = np.zeros((H, W, 3), np.float32) + np.array(bg, np.float32)
    return M.to_image(M.composite(bgarr, layers))


def pic1(bg=DARK_BG, ink=M.DARK_INK, tag_col=TAG_DARK):
    """PIC1, 480x272: 9 px letters centred at y=118, rainbow, bar, tagline."""
    from PIL import ImageDraw, ImageFont
    W, H = 480, 272
    L, _ = M.render_letters(stroke_px=9, hint=True)
    lh, lw = L.shape
    s = 9 / M.STROKE
    ox, oy = (W - lw) // 2, 118
    layers = M.rainbow_layers(s, ox, oy, (H, W))
    letters = np.zeros((H, W), np.float32)
    letters[oy:oy + lh, ox:ox + lw] = L
    layers.append((ink, letters))
    layers += M.bar_layers(s, ox, oy, (H, W), y=lh + 9, h=4, width=lw)
    bgarr = np.zeros((H, W, 3), np.float32) + np.array(bg, np.float32)
    im = M.to_image(M.composite(bgarr, layers))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT_SB, 15)
    t = 'PSP  ·  GBA  ·  GB  ·  GBC   EMULATOR'
    tw = d.textlength(t, font=f)
    d.text(((W - tw) / 2, oy + lh + 9 + 4 + 22), t, fill=tag_col, font=f)
    return im


def main(argv):
    if '--check' in argv:
        import tempfile
        tmp = os.path.join(tempfile.mkdtemp(), 'logo_ui.h')
        write_atlas(tmp)
        same = open(tmp, 'rb').read() == open(ATLAS_OUT, 'rb').read()
        print('psp/logo_ui.h %s' % ('reproduces' if same else 'DIFFERS'))
        return 0 if same else 1
    w, h = write_atlas(ATLAS_OUT)
    print('wrote %s  (%dx%d drawn, %dx%d texture, %d bytes)'
          % (os.path.relpath(ATLAS_OUT, ROOT), w, h, TEX_W, TEX_H,
             TEX_W * TEX_H))
    if '--xmb' in argv:
        icon0().save(ICON0_OUT, optimize=True)
        pic1().save(PIC1_OUT, optimize=True)
        print('wrote %s, %s' % (os.path.relpath(ICON0_OUT, ROOT),
                                os.path.relpath(PIC1_OUT, ROOT)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
