#!/usr/bin/env python3
"""logomark -- the GBAdhoc mark, re-derived so it renders cleanly at any size.

The master is the README's 2x logo (published as assets/gbadhoc-logo-3-dark@2x.png,
880x320), kept beside this file as gbadhoc-logo-3-master@2x.png so every baked
form -- psp/logo_ui.h, ICON0, PIC1, ICON1 -- can be re-made from this tree
(tools/bake_logo.py, tools/logo/make_icon1.py).  Derived by the splash/logo design study
(docs/design/UI-MOCKUPS.md; its logomark.py), copied verbatim apart from MASTER and the
header writer's attribution.
Three things live in it:

  letters   "GBAdhoc", a raster mask 700 px wide x 111 px tall at that scale
            (stroke 18.5 px, ascender 111 px: a 1:6 design, which is what makes
            it snap to a 3 px / 18 px or 4 px / 24 px grid)
  rainbow   five concentric SEMICIRCLES, centre (354.4, 147.1) in the master,
            i.e. (264.5, 0) relative to the letters' top-left, radii 60/50/40/
            30/20, stroke 7, green / yellow / pink / purple / teal outside-in
            -- the GBC palette.  In the master every arc ends in a BEAD: a
            circle wider than the stroke.  That is the "small dots on either
            side".  Here the arcs are analytic, butt-capped on the ascender
            line, no beads.
  bar       the three console palettes: GBA (8F80FF) 233 px, GB (9BBC0F) 234
            px, then the five GBC stripes; 8 px tall, one stroke below the
            baseline.

Everything here is expressed in LETTER UNITS (the 2x master's pixels, origin
at the letters' top-left) and rendered at a scale `s` (dest px per unit).
The letters are resampled from the mask with an exact box filter; with
`hint=True` the vertical stems and horizontal bars are first SNAPPED to the
pixel grid (a piecewise-linear remap of each axis), so every stem is exactly
`stroke_px` wide and sits on whole pixels.  That is what a font rasteriser's
hinting does, applied to a wordmark.
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, 'gbadhoc-logo-3-master@2x.png')

# ---- geometry of the master (measured, see README "Measurements") -----------
L_X0, L_Y0, L_W, L_H = 90, 147, 700, 111     # letters box in the master
STROKE = 18.5                                 # letter stroke, master px
RB_CX, RB_CY = 264.5, 0.0                     # rainbow centre, letter units
RB_R = [60, 50, 40, 30, 20]                   # outer -> inner radius
RB_STROKE = 7.0
RB_COL = ['#7dcb2f', '#f5c518', '#e8386d', '#7b4fd8', '#1fb8b0']
BAR_Y, BAR_H = 130, 8                         # letter units (baseline 111 + 19)
BAR = [('#8f80ff', 233), ('#9bbc0f', 234), ('#e8386d', 47), ('#f5c518', 47),
       ('#7dcb2f', 46), ('#1fb8b0', 47), ('#7b4fd8', 46)]      # widths sum 700

# Horizontal edges (letter units, y) that carry a bar: (top, bottom) pairs.
# ascender 0, cap top 6, x-height top 32, baseline 111.
Y_EDGES = [0, 6, 25, 32, 50, 67, 68, 85, 94, 111]
Y_BARS = [(6, 25), (32, 50), (50, 68), (67, 85), (94, 111)]

DARK_INK = (236, 234, 228)       # letters on the dark theme (master)
LIGHT_INK = (22, 34, 51)         # letters on the light theme / PIC1 / ICON0


def hexrgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# ---- the letter mask ----------------------------------------------------------
_mask = None


def letters_mask():
    """Coverage 0..1 of the letters, L_H x L_W, from the master's alpha."""
    global _mask
    if _mask is None:
        im = np.asarray(Image.open(MASTER).convert('RGBA')).astype(np.float32)
        a = im[L_Y0:L_Y0 + L_H, L_X0:L_X0 + L_W, 3] / 255.0
        # The master's rainbow beads reach two rows into the letters box
        # (above the B and A).  Only the d and h ascenders (x >= 400) rise
        # above the cap line (row 6), so everything else up there is bead.
        a[0:6, :400] = 0
        _mask = a
    return _mask


def _runs(line, thr=0.5):
    out, x = [], 0
    n = line.size
    while x < n:
        if line[x] >= thr:
            s = x
            while x < n and line[x] >= thr:
                x += 1
            out.append((s, x))
        else:
            x += 1
    return out


def _precise(line, s, e):
    """Sub-pixel edges of a run [s, e) from the partial pixels beside it."""
    l = s - (line[s - 1] if s > 0 else 0.0)
    r = e + (line[e] if e < line.size else 0.0)
    return float(l), float(r)


def stems():
    """Vertical stems: (left, right) in letter units, measured on rows that
    sit between the horizontal bars so only stems are hit; de-duplicated."""
    m = letters_mask()
    found = []
    for y in (29, 41, 58, 89):
        for s, e in _runs(m[y]):
            if 12 <= e - s <= 24:
                l, r = _precise(m[y], s, e)
                # A stem seen on an earlier row wins; an overlapping one on a
                # later row (the B's wider lower bowl) is left to the box
                # filter, because one axis map cannot snap both.
                if not any(l < fr + 2 and r > fl - 2 for fl, fr in found):
                    found.append((l, r))
    return sorted(found)


def _remap(anchors, s, stroke_px):
    """Piecewise-linear source->dest map.  anchors: list of (src, dst)."""
    xs = np.array([a[0] for a in anchors], np.float64)
    ys = np.array([a[1] for a in anchors], np.float64)
    return xs, ys


def axis_maps(s, stroke_px, hint, width_units=L_W, height_units=L_H):
    """Return (X, Y): arrays giving the SOURCE coordinate of every destination
    pixel boundary along x and y.  Hinted or plain."""
    if not hint:
        W = int(round(width_units * s))
        H = int(round(height_units * s))
        X = np.arange(W + 1) / s
        Y = np.arange(H + 1) / s
        return X, Y
    S = stroke_px
    # ---- y: snap bar edges greedily, every bar exactly S tall
    d = {0: 0}
    d[6] = int(round(6 * s)); d[25] = d[6] + S
    d[32] = max(d[25], int(round(32 * s))); d[50] = d[32] + S
    d[67] = max(d[50], int(round(67 * s))); d[68] = max(d[67], d[50] + S)
    d[85] = d[67] + S
    d[94] = max(d[85], int(round(94 * s))); d[111] = d[94] + S
    ya = sorted(d.items())
    H = d[111]
    # ---- x: snap every stem to [round(l*s), +S)
    xa = [(0.0, 0)]
    for l, r in stems():
        dl = int(round(l * s))
        if dl <= xa[-1][1]:
            dl = xa[-1][1] + 1
        xa.append((l, dl)); xa.append((r, dl + S))
    W = int(round(width_units * s))
    if W <= xa[-1][1]:
        W = xa[-1][1] + 1
    xa.append((float(width_units), W))
    # invert: dest boundary -> source coordinate
    def inv(anchors, n):
        src = np.array([a[0] for a in anchors]); dst = np.array([a[1] for a in anchors], np.float64)
        return np.interp(np.arange(n + 1, dtype=np.float64), dst, src)
    return inv(xa, W), inv(ya, H)


def _integral(m):
    I = np.zeros((m.shape[0] + 1, m.shape[1] + 1), np.float64)
    I[1:, 1:] = np.cumsum(np.cumsum(m, 0), 1)
    return I


def _integral_at(I, y, x):
    """Bilinear lookup of the integral image at fractional (y, x)."""
    y = np.clip(y, 0, I.shape[0] - 1); x = np.clip(x, 0, I.shape[1] - 1)
    y0 = np.floor(y).astype(int); x0 = np.floor(x).astype(int)
    y1 = np.minimum(y0 + 1, I.shape[0] - 1); x1 = np.minimum(x0 + 1, I.shape[1] - 1)
    fy = (y - y0)[:, None]; fx = (x - x0)[None, :]
    a = I[np.ix_(y0, x0)]; b = I[np.ix_(y0, x1)]; c = I[np.ix_(y1, x0)]; dd = I[np.ix_(y1, x1)]
    return (a * (1 - fy) * (1 - fx) + b * (1 - fy) * fx + c * fy * (1 - fx) + dd * fy * fx)


def box_resample(mask, X, Y):
    """Exact box filter: dest (j,i) = mean of mask over [X[i],X[i+1]) x [Y[j],Y[j+1])."""
    I = _integral(mask)
    G = _integral_at(I, Y, X)                     # (H+1, W+1)
    area = (Y[1:] - Y[:-1])[:, None] * (X[1:] - X[:-1])[None, :]
    cov = (G[1:, 1:] - G[:-1, 1:] - G[1:, :-1] + G[:-1, :-1]) / np.maximum(area, 1e-9)
    return np.clip(cov, 0, 1).astype(np.float32)


def render_letters(stroke_px=None, scale=None, hint=True):
    """Coverage array of the wordmark.  Give stroke_px (hinted grid) or scale."""
    if scale is None:
        scale = stroke_px / STROKE
    if stroke_px is None:
        stroke_px = int(round(STROKE * scale))
    X, Y = axis_maps(scale, stroke_px, hint)
    return box_resample(letters_mask(), X, Y), scale


# ---- analytic rainbow and bar ------------------------------------------------
def rainbow_layers(scale, ox, oy, size, radii=None, stroke=None, cap='butt', lift=0.0):
    """List of (rgb, coverage HxW) for the five arcs at `scale`, centre at
    letter-unit origin (ox, oy) in the destination.  Analytic anti-aliasing
    (coverage = clamp(halfwidth + 0.5 - |d - r|)), so there is nothing at the
    ends but the cut on the centre line: NO beads."""
    H, W = size
    radii = radii if radii is not None else [r * scale for r in RB_R]
    stroke = stroke if stroke is not None else RB_STROKE * scale
    cx = ox + RB_CX * scale; cy = oy + RB_CY * scale - lift
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    px = xx + 0.5 - cx; py = yy + 0.5 - cy
    dist = np.sqrt(px * px + py * py)
    layers = []
    for col, r in zip(RB_COL, radii):
        band = np.clip(stroke / 2 + 0.5 - np.abs(dist - r), 0, 1)
        if cap == 'butt':
            half = np.clip(cy - (yy + 0.5) + 0.5, 0, 1)   # upper half-plane
            cov = band * half
        else:                                          # round: a disc at each end
            half = np.clip(cy - (yy + 0.5) + 0.5, 0, 1)
            cov = band * half
            for ex in (cx - r, cx + r):
                dd = np.sqrt((xx + 0.5 - ex) ** 2 + (yy + 0.5 - cy) ** 2)
                cov = np.maximum(cov, np.clip(stroke / 2 + 0.5 - dd, 0, 1))
        layers.append((hexrgb(col), cov.astype(np.float32)))
    return layers


def bar_layers(scale, ox, oy, size, y=None, h=None, width=None):
    """The console-palette bar as (rgb, coverage) layers, pixel-snapped."""
    H, W = size
    y0 = oy + (BAR_Y * scale if y is None else y)
    hh = max(1, int(round(BAR_H * scale))) if h is None else h
    total = int(round(L_W * scale)) if width is None else width
    layers = []
    x = 0.0
    for col, w in BAR:
        w2 = w / L_W * total
        cov = np.zeros((H, W), np.float32)
        xa, xb = int(round(ox + x)), int(round(ox + x + w2))
        ya, yb = int(round(y0)), int(round(y0)) + hh
        cov[max(ya, 0):min(yb, H), max(xa, 0):min(xb, W)] = 1
        layers.append((hexrgb(col), cov))
        x += w2
    return layers


def composite(bg, layers):
    """bg: HxWx3 float RGB 0..255; layers: [(rgb, cov)] painted in order."""
    out = bg.astype(np.float32).copy()
    for rgb, cov in layers:
        c = cov[..., None]
        out = out * (1 - c) + np.array(rgb, np.float32) * c
    return out


def to_image(arr):
    return Image.fromarray(np.clip(arr + 0.5, 0, 255).astype(np.uint8), 'RGB')


def mark(stroke_px, ink, bg_rgb, pad=(0, 0, 0, 0), hint=True, rainbow=True, bar=True,
         rb_scale=None, rb_stroke=None, rb_radii=None, bar_h=None, alpha=False):
    """A complete mark (letters + rainbow + bar) on a flat background.
    pad = (left, top, right, bottom) in dest px.  Returns (PIL, letters_cov,
    origin)."""
    L, s = render_letters(stroke_px=stroke_px, hint=hint)
    lh, lw = L.shape
    rb_scale = rb_scale if rb_scale is not None else s
    rb_h = int(round((RB_R[0] + RB_STROKE / 2) * rb_scale)) + 1
    top = pad[1] + (rb_h if rainbow else 0)
    bar_hh = bar_h if bar_h is not None else max(1, int(round(BAR_H * s)))
    bar_top = lh + stroke_px
    H = top + (bar_top + bar_hh if bar else lh) + pad[3]
    W = pad[0] + lw + pad[2]
    ox, oy = pad[0], top
    layers = []
    if rainbow:
        layers += rainbow_layers(rb_scale, ox + (RB_CX * s - RB_CX * rb_scale), oy, (H, W),
                                 radii=rb_radii, stroke=rb_stroke)
    letters = np.zeros((H, W), np.float32)
    letters[oy:oy + lh, ox:ox + lw] = L
    layers.append((ink, letters))
    if bar:
        layers += bar_layers(s, ox, oy, (H, W), y=bar_top, h=bar_hh, width=lw)
    bg = np.zeros((H, W, 3), np.float32) + np.array(bg_rgb, np.float32)
    rgb = composite(bg, layers)
    if alpha:
        a = np.zeros((H, W), np.float32)
        for _, cov in layers:
            a = a + cov * (1 - a)
        im = Image.fromarray(np.dstack([np.clip(rgb + 0.5, 0, 255).astype(np.uint8),
                                        (a * 255 + 0.5).astype(np.uint8)]), 'RGBA')
        return im, L, (ox, oy)
    return to_image(rgb), L, (ox, oy)


def write_logo_header(path, cov, tex_w=256, tex_h=32, note='',
                      by='tools/bake_logo.py'):
    """Write a logo_ui.h with the given coverage (H x W <= tex) -- the same
    layout tools/bake_logo.py produces, so vid_logo needs no change."""
    h, w = cov.shape
    assert w <= tex_w and h <= tex_h, (w, h)
    tex = np.zeros((tex_h, tex_w), np.uint8)
    tex[:h, :w] = (cov * 255 + 0.5).astype(np.uint8)
    data = tex.tobytes()
    with open(path, 'w', newline='\n') as fh:
        fh.write('/* GENERATED by %s -- do not edit.\n' % by +
                 ' *\n'
                 ' * The GBAdhoc wordmark as 8-bit coverage for the GE\'s T8 +\n'
                 ' * alpha-CLUT path, tinted by the theme accent at draw time.\n'
                 ' * %s */\n'
                 '#ifndef LOGO_UI_H\n#define LOGO_UI_H\n\n' % note)
        fh.write('#define LOGO_TEX_W %d\n#define LOGO_TEX_H %d\n'
                 '#define LOGO_W     %d\n#define LOGO_H     %d\n\n'
                 % (tex_w, tex_h, w, h))
        fh.write('static const unsigned char __attribute__((aligned(16)))\n'
                 '   logo_a[%d] = {\n' % len(data))
        for i in range(0, len(data), 32):
            fh.write(''.join('%d,' % b for b in data[i:i + 32]) + '\n')
        fh.write('};\n\n#endif /* LOGO_UI_H */\n')
    return len(data)
