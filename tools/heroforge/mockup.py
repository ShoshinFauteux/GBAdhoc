"""Render the Marquee shell over a hero, exactly as the PSP draws it.

Layout, colours and fonts are lifted from psp/ui_psp.c shell_marquee() and the
THM_DARK palette, so what this produces is what the console shows -- it is a
preview of the real thing, not an impression of it.

One deliberate difference, and it matters: the shell drops a uniform scrim at
alpha 205 over a hero it derived itself from box art.  A PRE-COMPOSED hero is
already darkened and already has its left feather, so a second 80% scrim would
crush it to black.  Pre-composed heroes therefore get a LIGHT scrim (55), and
the emulator has to make the same distinction -- see HERO_SCRIM below.
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 480, 272
HERO_SCRIM = 55            # for a pre-composed hero; 205 for a derived one

# THM_DARK, psp/ui_psp.c
C_ACCENT = (255, 255, 255)
C_SEL    = (255, 255, 255)
C_ITEM   = (201, 201, 201)
C_DIM    = (107, 107, 107)
C_VALUE  = (232, 232, 232)
C_BG     = (14, 14, 14)

_FONTS = os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), 'assets', 'fonts')


def _font(name, px):
    return ImageFont.truetype(os.path.join(_FONTS, name), px)


def render(hero, title, size_mb, has_save, above, below, scrim=HERO_SCRIM):
    """hero: 480x272 RGB.  above/below: the neighbouring titles in the list."""
    img = hero.convert('RGB').copy()
    img = Image.blend(img, Image.new('RGB', (W, H), C_BG), scrim / 255.0)
    d = ImageDraw.Draw(img)

    ui = _font('Inter-Regular.ttf', 15)
    hd = _font('Inter-SemiBold.ttf', 19)

    d.text((20, 10), 'GBAdhoc', font=hd, fill=C_ACCENT)      # vid_text_hd(20,10)
    d.text((20, 74), title, font=hd, fill=C_SEL)             # vid_text_hd(20,74)

    meta = '%d MB' % size_mb
    d.text((20, 102), meta, font=ui, fill=C_ITEM)            # vid_text(20,102)
    mw = d.textlength(meta, font=ui)
    d.text((20 + mw + 14, 102),
           'save present' if has_save else 'no save',
           font=ui, fill=C_VALUE if has_save else C_DIM)

    d.rectangle([20, 172, 22, 192], fill=C_ACCENT)           # vid_rect(20,172,3,21)
    for i, (txt, sel) in enumerate(((above, False), (title, True), (below, False))):
        y = 176 + (i - 1) * 25
        d.text((32, y), txt, font=ui, fill=C_SEL if sel else C_DIM)

    for x, t in ((20, 'X play'), (116, 'L/R page'), (244, 'START settings')):
        d.text((x, 252), t, font=ui, fill=C_DIM)
    return img
