"""Repair the hard bottom band in generated hero art.

    python deband.py <hero_dir> [--out <dir>] [--check]

The v1 prompt asked for "the bottom 45 pixels calm and dark" and the generator
obliged with a literal dark rectangle -- a flat step in luminance with a hard
horizontal edge, present in 10 of 12 images at 79-85% down.  It reads as a
letterbox bar rather than as artwork.

The DARKENING is wanted; the UI prints its control hints there.  Only the EDGE
is wrong.  So: find the step, measure how much darker the band is, divide that
factor back out to recover the artwork underneath, then re-apply the same
darkening as a smooth ramp.  Nothing is invented -- the pixels under the band
are still there, just scaled.

Where the band was painted opaque rather than multiplied, division cannot
recover what was covered; the result is then merely a soft gradient instead of
a hard one, which is still the improvement we want.
"""
import os
import sys

from PIL import Image, ImageStat


def _row_means(im, step=16):
    g = im.convert('L')
    w, h = g.size
    px = g.load()
    n = max(1, w // step)
    return [sum(px[x, y] for x in range(0, w, step)) / n for y in range(h)]


def find_seam(im, lo=0.60, min_step=12.0):
    """Threshold raised from 5 after the v2 prompt: a step of 5-10 in the
    bottom third is as likely to be real artwork -- a horizon, a shadow edge,
    the lip of a platform -- as a painted band, and "repairing" one of those
    brightens a region that was meant to be dark.  A deliberate scrim step is
    20+; below 12 it is not worth the risk of damaging a good image."""
    """Row index of the hard edge, or None."""
    rows = _row_means(im)
    h = len(rows)
    best, besty = 0.0, None
    for y in range(int(h * lo), h - 2):
        d = rows[y] - rows[y + 1]
        if d > best:
            best, besty = d, y
    return (besty, best) if best >= min_step else (None, best)


def deband(im, feather_frac=0.22):
    """Return the image with the bottom band's hard edge smoothed away."""
    im = im.convert('RGB')
    w, h = im.size
    y0, step = find_seam(im)
    if y0 is None:
        return im, 0.0

    rows = _row_means(im)
    above = sum(rows[max(0, y0 - 24):y0]) / max(1, len(rows[max(0, y0 - 24):y0]))
    below = sum(rows[y0 + 2:min(h, y0 + 26)]) / max(1, len(rows[y0 + 2:min(h, y0 + 26)]))
    if above <= 1 or below <= 1:
        return im, 0.0
    factor = below / above                      # how much darker the band is
    if factor >= 0.97:                          # nothing meaningful to undo
        return im, factor

    px = im.load()
    feather = max(1, int(h * feather_frac))
    start = max(0, y0 - feather)                # ramp begins above the seam
    inv = 1.0 / factor

    for y in range(start, h):
        # 1) undo the flat band below the seam
        undo = inv if y > y0 else 1.0
        # 2) re-apply the same darkening as a smooth ramp
        t = (y - start) / float(h - start)
        ramp = 1.0 - (1.0 - factor) * (t ** 1.4)
        k = undo * ramp
        if abs(k - 1.0) < 0.002:
            continue
        for x in range(w):
            r, g, b = px[x, y]
            px[x, y] = (min(255, int(r * k)),
                        min(255, int(g * k)),
                        min(255, int(b * k)))
    return im, factor


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = sys.argv[1]
    out = (sys.argv[sys.argv.index('--out') + 1]
           if '--out' in sys.argv else src)
    check = '--check' in sys.argv
    os.makedirs(out, exist_ok=True)

    for fn in sorted(os.listdir(src)):
        if not fn.lower().endswith(('.png', '.jpg', '.jpeg', '.webp')):
            continue
        im = Image.open(os.path.join(src, fn))
        y0, step = find_seam(im)
        if check:
            print("  %-44s seam=%s step=%.1f"
                  % (fn[:44], y0 if y0 is not None else '-', step))
            continue
        fixed, factor = deband(im)
        fixed.save(os.path.join(out, fn))
        print("  %-44s %s" % (fn[:44],
                              'debanded (x%.2f)' % factor if factor else 'no seam'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
