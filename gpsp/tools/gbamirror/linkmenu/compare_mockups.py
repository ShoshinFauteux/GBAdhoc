#!/usr/bin/env python3
"""compare_mockups.py OUTDIR RUN[:theme] ... -- every PPSSPP GE dump of a link
screen against the design's PNG (docs/design/UI-MOCKUPS.md; PNGs <screen>-<theme>
.png), docs/GBA-LINK-UI.md.

A dump counts when its file name is a design screen (wireless-cable.bmp,
scan-mixed.bmp, ...).  RUN is a run_linkmenu_ppsspp.sh output dir; its
console dirs (c1, c2) are searched; the theme comes from the RUN name
(-t0 / -t1 / -dark / -light) or the :theme suffix.  For each pair:

  differ   pixels that differ at all
  >1step   pixels that differ by more than one RGB565 step in a channel
           (compared as 5/6/5-bit values): a coordinate, colour, alpha, glyph
           or order error lands here; PPSSPP's blend rounding cannot
  max      the largest channel difference

and a side-by-side OUTDIR/<screen>-<theme>.png: design | GE dump | the
>1-step pixels in red over the dump, dimmed.  OUTDIR/results.json and a
markdown table on stdout."""
import glob
import json
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.environ.get('LINKMODE_PNG', os.path.normpath(
    os.path.join(HERE, '..', '..', '..', '..', 'linkmode-mockups', 'png')))


def theme_of(run):
    m = re.search(r'-(t0|t1|dark|light)\b', run)
    if not m:
        return 'dark'
    return 'light' if m.group(1) in ('t1', 'light') else 'dark'


def compare(design, dump):
    a = np.asarray(Image.open(design).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(dump).convert('RGB'), dtype=np.int16)
    d = np.abs(a - b)
    differ = int((d.max(-1) > 0).sum())
    # in RGB565 steps: both images hold 565 values (the GE writes 565; the
    # design model writes 565 after every draw call)
    sa = np.stack([a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3], -1)
    sb = np.stack([b[..., 0] >> 3, b[..., 1] >> 2, b[..., 2] >> 3], -1)
    big = (np.abs(sa - sb) > 1).any(-1)
    return differ, int(big.sum()), int(d.max()), big, a, b


def sheet(a, b, big, out):
    h, w = a.shape[:2]
    c = (b * 0.35).astype(np.uint8)
    c[big] = (255, 40, 40)
    im = Image.new('RGB', (w * 3 + 8, h), (128, 128, 128))
    im.paste(Image.fromarray(a.astype(np.uint8)), (0, 0))
    im.paste(Image.fromarray(b.astype(np.uint8)), (w + 4, 0))
    im.paste(Image.fromarray(c), (2 * w + 8, 0))
    im.save(out)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    rows = []
    for arg in sys.argv[2:]:
        run, _, th = arg.partition(':')
        theme = th or theme_of(os.path.basename(run.rstrip('/')))
        for bmp in sorted(glob.glob(os.path.join(run, 'c*', '*.bmp'))):
            screen = os.path.splitext(os.path.basename(bmp))[0]
            design = os.path.join(PNG, '%s-%s.png' % (screen, theme))
            if not os.path.isfile(design):
                continue
            differ, big, mx, mask, a, b = compare(design, bmp)
            sheet(a, b, mask, os.path.join(out, '%s-%s.png' % (screen, theme)))
            rows.append(dict(screen=screen, theme=theme, run=os.path.basename(run),
                             differ=differ, big=big, max=mx))
    with open(os.path.join(out, 'results.json'), 'w') as f:
        json.dump(rows, f, indent=1)
    print('| screen | theme | differ | >1 step | max |')
    print('|---|---|---:|---:|---:|')
    for r in sorted(rows, key=lambda r: (r['screen'], r['theme'])):
        print('| %s | %s | %d | %d | %d |' % (r['screen'], r['theme'], r['differ'],
                                            r['big'], r['max']))


if __name__ == '__main__':
    main()
