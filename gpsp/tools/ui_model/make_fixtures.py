#!/usr/bin/env python3
"""make_fixtures.py OUTDIR -- the inputs that let PPSSPP draw the overlay over
EXACTLY what the design mockups drew it over (docs/UI-OVERLAY.md):

  ui_backdrop.565          the mockups' 480x272 game frame, raw PSP-5650
                           (harness key `ui_backdrop`: drawn 1:1, nearest)
  slot{1,2,3,5}.thumb      the mockups' four 64x42 state previews, in the
                           STH1 format the save path writes (state_slots.h)

Needs builds/menu-mockups (UI_MOCKUPS=...) for pspdraw's assets."""
import os, sys, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.environ.get('UI_MOCKUPS',
                      os.path.normpath(os.path.join(HERE, '..', '..', '..', 'menu-mockups')))
sys.path.insert(0, MOCK)
import pspdraw as P   # noqa: E402


def psp5650(img):
    """PIL RGB -> little-endian PSP-5650 words (R in the low bits)."""
    a = np.asarray(img.convert('RGB'), dtype=np.uint16)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return (r | (g << 5) | (b << 11)).astype('<u2')


def main(out):
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'ui_backdrop.565'), 'wb').write(
        psp5650(P.ASSETS['frame']).tobytes())
    hdr = b'STH1' + struct.pack('<HHH', 64, 42, 64) + b'\0' * 6
    for i, exists in enumerate([1, 1, 1, 0, 1]):
        if exists:
            open(os.path.join(out, 'slot%d.thumb' % (i + 1)), 'wb').write(
                hdr + psp5650(P.ASSETS['thumbs'][i]).tobytes())
    print('fixtures in', out)


if __name__ == '__main__':
    main(sys.argv[1])
