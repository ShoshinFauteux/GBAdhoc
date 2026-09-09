"""Judge hero art under the real Marquee chrome.

    python preview.py <hero_dir> [--boxart <dir>] [--out <file.png>]

Every image in <hero_dir> is fitted to 480x272 and composited under the actual
shell: Inter at the real sizes, the THM_DARK palette, and the coordinates from
psp/ui_psp.c shell_marquee().  If a matching cover exists in --boxart, the
styles.panorama() version is rendered beside it, so generated art and derived
art are judged on the same screen instead of as loose pictures.

That distinction matters: every hero in this project looked acceptable as a
standalone image and only failed once the UI was drawn on top.  Legibility of
the left column is the thing to look at.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageDraw            # noqa: E402
import mockup                               # noqa: E402

W, H = mockup.W, mockup.H
EXT = ('.png', '.jpg', '.jpeg', '.webp', '.bmp')


def fit(im):
    """Cover 480x272 without distorting: scale to fill, crop the excess."""
    im = im.convert('RGB')
    s = max(W / im.width, H / im.height)
    r = im.resize((max(W, int(im.width * s)), max(H, int(im.height * s))),
                  Image.LANCZOS)
    return r.crop(((r.width - W) // 2, (r.height - H) // 2,
                   (r.width - W) // 2 + W, (r.height - H) // 2 + H))


def title_of(path):
    return os.path.splitext(os.path.basename(path))[0]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    hero_dir = sys.argv[1]
    box_dir = (sys.argv[sys.argv.index('--boxart') + 1]
               if '--boxart' in sys.argv else None)
    out = (sys.argv[sys.argv.index('--out') + 1]
           if '--out' in sys.argv else 'hero_preview.png')

    heroes = sorted(f for f in os.listdir(hero_dir) if f.lower().endswith(EXT))
    if not heroes:
        print('no images in %s' % hero_dir)
        return 1

    compare = None
    if box_dir and os.path.isdir(box_dir):
        import compose, styles
        compare = (compose, styles)

    cols = 2 if compare else 1
    sheet = Image.new('RGB', (W * cols, (H + 20) * len(heroes)), (8, 8, 10))
    d = ImageDraw.Draw(sheet)

    for i, fn in enumerate(heroes):
        name = title_of(fn)
        y = i * (H + 20)
        d.text((10, y + 5), name, fill=(190, 190, 190))
        if compare:
            d.text((W + 10, y + 5), 'panorama (derived)', fill=(120, 120, 120))

        hero = fit(Image.open(os.path.join(hero_dir, fn)))
        # A generated hero is already composed and already dark; the shell's
        # heavy 205 scrim is only for art it derived itself.
        sheet.paste(mockup.render(hero, name, 8, True,
                                  'Previous Game', 'Next Game',
                                  scrim=mockup.HERO_SCRIM), (0, y + 20))

        if compare:
            compose, styles = compare
            for cand in (name + '.png', name + '.jpg'):
                p = os.path.join(box_dir, cand)
                if os.path.exists(p):
                    cover = compose._cover_only(
                        Image.open(p).convert('RGB'))
                    sheet.paste(mockup.render(styles.panorama(cover), name, 8,
                                              True, 'Previous Game',
                                              'Next Game'), (W, y + 20))
                    break

    sheet.save(out)
    print('wrote %s  (%d heroes%s)'
          % (out, len(heroes), ', with derived comparison' if compare else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
