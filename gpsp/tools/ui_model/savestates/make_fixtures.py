#!/usr/bin/env python3
"""make_fixtures.py OUTDIR -- the card contents and backdrops the Save States
design (builds/savestate-mockups/savestate.py, direction A) draws, so a
PPSSPP GE dump of the real page can be held against Fable's PNGs
(docs/UI-OVERLAY.md section 13):

  OUTDIR/<con>/backdrop.565   the design's 480x272 menu backdrop, raw
                              PSP-5650 (harness key `ui_backdrop`)
  OUTDIR/<con>/slot<k>.thumb  64x42 STH1, as state_thumb_write makes it
  OUTDIR/<con>/slot<k>.shot   the native frame, STH1 stride 256, as
                              state_shot_write makes it (absent = a 3.1 state)
  OUTDIR/<con>/slot<k>.time   'YYYY-mm-dd HH:MM' for `touch -d` (the page's
                              timestamp is the file's mtime)

<con> is gba, gbc, gb.  Needs builds/savestate-mockups (SAVESTATE_MOCKUPS)
and builds/menu-mockups (UI_MOCKUPS) for the 3.1 thumbnail."""
import os, sys, struct
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
BUILDS = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
SS = os.environ.get('SAVESTATE_MOCKUPS', os.path.join(BUILDS, 'savestate-mockups'))
MOCK = os.environ.get('UI_MOCKUPS', os.path.join(BUILDS, 'menu-mockups'))
sys.path.insert(0, MOCK)
import pspdraw as P   # noqa: E402

MON = {m: i + 1 for i, m in enumerate('Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split())}

# savestate.py GAMES, verbatim: (native key or None, stamp, has .shot) --
# the design's five slots, then slots 6-10 (state_slots.h: ten a game, five
# rows on screen).  GBA slot 8 holds one more state so the scrolled window
# (ge_gba.ui `scrolled`, rows 4-8) has a picture in it; the rest are empty.
GAMES = {
    'gba': dict(back='battle', slots=[('mauville2', '26 Sep 2026, 19:10', 1),
                                      ('mauville', '1 Oct 2026, 08:44', 1),
                                      ('battle', '3 Oct 2026, 14:32', 1),
                                      None,
                                      ('OLD', '20 Sep 2026, 23:02', 0),
                                      None, None,
                                      ('mauville', '4 Oct 2026, 10:05', 1),
                                      None, None]),
    'gbc': dict(back='forest', slots=[('oak', '2 Oct 2026, 21:15', 1),
                                      ('forest', '3 Oct 2026, 09:58', 1),
                                      None, None, None] + [None] * 5),
    'gb': dict(back='kirby', slots=[('kirby', '3 Oct 2026, 12:20', 1),
                                    None, None, None, None] + [None] * 5),
}


def native(k):
    return Image.open(os.path.join(SS, 'assets', 'native-%s.png' % k)).convert('RGB')


def psp5650(img):
    a = np.asarray(img.convert('RGB'), dtype=np.uint16)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return (r | (g << 5) | (b << 11)).astype('<u2')


def lcd(im):
    """savestate.py lcd(): the picture nearest-scaled to 272 rows on black."""
    w, h = im.size
    dw = round(w * 272 / h)
    out = Image.new('RGB', (480, 272), (0, 0, 0))
    out.paste(im.resize((dw, 272), Image.NEAREST), ((480 - dw) // 2, 0))
    return out


def thumb64(im):
    """savestate.py thumb64() == main_psp.c state_thumb_write."""
    w, h = im.size
    dw = 64 if w * 42 >= 64 * h else int((w * 42 + h // 2) / h)
    t = Image.new('RGB', (64, 42), (0, 0, 0))
    t.paste(im.resize((dw, 42), Image.NEAREST), ((64 - dw) // 2, 0))
    return t


def shot(im):
    w, h = im.size
    body = np.zeros((h, 256), '<u2')
    body[:, :w] = psp5650(im)
    hdr = b'STH1' + struct.pack('<HHH', w, h, 256) + b'\0' * 6
    return hdr + body.tobytes()


def stamp_to_touch(s):
    d, rest = s.split(' ', 1)
    mon, rest = rest.split(' ', 1)
    year, hm = rest.split(', ')
    return '%s-%02d-%02d %s' % (year, MON[mon], int(d), hm)


def main(out):
    th_hdr = b'STH1' + struct.pack('<HHH', 64, 42, 64) + b'\0' * 6
    for con, g in GAMES.items():
        d = os.path.join(out, con)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, 'backdrop.565'), 'wb').write(
            psp5650(lcd(native(g['back']))).tobytes())
        for k, sl in enumerate(g['slots'], 1):
            for ext in ('thumb', 'shot', 'time'):
                p = os.path.join(d, 'slot%d.%s' % (k, ext))
                if os.path.exists(p):
                    os.remove(p)
            if not sl:
                continue
            key, ts, has_shot = sl
            if key == 'OLD':
                th = P.ASSETS['thumbs'][1]
            else:
                th = thumb64(native(key))
            open(os.path.join(d, 'slot%d.thumb' % k), 'wb').write(th_hdr + psp5650(th).tobytes())
            if has_shot:
                open(os.path.join(d, 'slot%d.shot' % k), 'wb').write(shot(native(key)))
            open(os.path.join(d, 'slot%d.time' % k), 'w').write(stamp_to_touch(ts) + '\n')
    print('fixtures in', out)


if __name__ == '__main__':
    main(sys.argv[1])
