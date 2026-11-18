#!/usr/bin/env python3
"""make_backdrops.py OUTDIR -- the three 480x272 game frames the link-mode
design (docs/design/UI-MOCKUPS.md) draws its screens over, as raw
PSP-5650 files for the harness key `ui_backdrop` (drawn 1:1 behind the menus
and the LINKING screen instead of the game), so a PPSSPP GE dump can be
compared with the design's PNG pixel for pixel (docs/GBA-LINK-UI.md):

  emerald.565   the Emerald capture of the menu mockups
  aw2.565       the Advance Wars 2 frame from the mirror rig, as FIT shows it
  gb.565        the GB capture
"""
import os
import sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LINKMODE = os.environ.get('LINKMODE_MOCKUPS', os.path.normpath(
    os.path.join(HERE, '..', '..', '..', '..', 'linkmode-mockups')))
sys.path.insert(0, LINKMODE)
import linkmode as L   # noqa: E402  (sets pspdraw up with the shipped atlases)


def psp5650(img):
    a = np.asarray(img.convert('RGB'), dtype=np.uint16)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return (r | (g << 5) | (b << 11)).astype('<u2')


def main(out):
    os.makedirs(out, exist_ok=True)
    for name, img in L.FRAMES.items():
        img = img.convert('RGB')
        assert img.size == (480, 272), (name, img.size)
        open(os.path.join(out, name + '.565'), 'wb').write(psp5650(img).tobytes())
    print('backdrops in', out, sorted(L.FRAMES))


if __name__ == '__main__':
    main(sys.argv[1])
