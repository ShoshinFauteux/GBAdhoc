"""Alternative hero compositions.

`compose.py` does the safe one: cover on a drained, blurred plate.  These are
the other two worth looking at, both built only from the cover's own pixels --
no generation, so no mangled logos and nothing new to redistribute.

  bleed     the cover's edges are MIRRORED outward and progressively blurred,
            so the artwork appears to continue past the box.  Reads as a
            wallpaper rather than a cover pasted on a background.

  panorama  the cover is enlarged past the frame and pushed right, so the
            frame is a CROP of the artwork.  The left is a long feather into
            the shell's own near-black.  No plate at all.
"""
from PIL import Image, ImageFilter, ImageEnhance

W, H = 480, 272
LEFT_QUIET = 0.58


def _quiet_left(img, strength=150, soften=0):
    """Darken -- and optionally BLUR -- the left, where the shell writes text.

    Darkening alone is not enough against a bright logo: contrast survives it,
    and "Pokemon Emerald" printed over the POKEMON wordmark was unreadable in
    the first mockup.  Blurring the same region destroys the competing edges,
    so the text has nothing to fight.
    """
    ramp = Image.new('L', (W, 1))
    for x in range(W):
        t = x / (W * LEFT_QUIET)
        ramp.putpixel((x, 0), 0 if t >= 1 else int(strength * (1 - t) ** 1.35))
    mask = ramp.resize((W, H))
    if soften:
        img = Image.composite(img.filter(ImageFilter.GaussianBlur(soften)),
                              img, mask)
    return Image.composite(Image.new('RGB', (W, H), (8, 8, 10)), img, mask)


def bleed(cover, art_h=232, margin=26):
    """Mirror the cover's edges outward, blurring with distance."""
    ch = art_h
    cw = max(1, int(cover.width * ch / cover.height))
    sharp = cover.resize((cw, ch), Image.LANCZOS)
    x = W - cw - margin
    y = (H - ch) // 2

    # Build an oversized mirrored tile: [flip | art | flip] both axes, so the
    # extension always continues the adjacent edge instead of inventing one.
    wide = Image.new('RGB', (cw * 3, ch * 3))
    for i, fx in enumerate((True, False, True)):
        for j, fy in enumerate((True, False, True)):
            t = sharp
            if fx:
                t = t.transpose(Image.FLIP_LEFT_RIGHT)
            if fy:
                t = t.transpose(Image.FLIP_TOP_BOTTOM)
            wide.paste(t, (i * cw, j * ch))

    # Crop the frame out of the mirrored field, aligned so the real art lands
    # exactly where the sharp copy will go.
    bg = wide.crop((cw - x, ch - y, cw - x + W, ch - y + H))
    bg = bg.filter(ImageFilter.GaussianBlur(11))
    bg = ImageEnhance.Color(bg).enhance(0.30)
    bg = ImageEnhance.Brightness(bg).enhance(0.50)

    canvas = _quiet_left(bg, 140)
    canvas.paste(sharp, (x, y))
    return canvas


def _quiet_bottom(img, band=46, strength=170):
    """Darken the last few rows, where the shell prints its control hints.

    Same reasoning as the left panel: the footer is fixed text over whatever
    the artwork happens to be, and on a bright cover it vanished.
    """
    ramp = Image.new('L', (1, H))
    for y in range(H):
        t = (H - y) / float(band)
        ramp.putpixel((0, y), 0 if t >= 1 else int(strength * (1 - t) ** 1.2))
    return Image.composite(Image.new('RGB', (W, H), (8, 8, 10)),
                           img, ramp.resize((W, H)))


def panorama(cover, zoom=1.06, push=0.34, rise=0.46):
    """The frame is a crop of the enlarged artwork; no plate, no card.

    Three corrections over the first pass, all visible in the samples:

      * the bottom ~9% of every GBA box is the ratings/publisher strip.  It is
        not artwork and it survived the crop, so it is trimmed FIRST.
      * zoom sits just above 1.0 -- the minimum that fills 480 px wide.  At
        1.42 the frame cropped INTO the logo ("ROID USION", "ELDA"), which
        reads as a broken image rather than a crop.  At 1.06 the full width
        of the cover survives and only the vertical band is chosen.
      * `rise` 0.46 deliberately crops BELOW the logo.  Keeping the logo put
        a bright wordmark in the upper left -- exactly where the shell writes
        the game's name -- and Metroid, Emerald and Advance Wars were all
        unreadable.  The logo is redundant anyway: the UI prints the title as
        text two lines down.  Key art only.
      * the left feather is longer and stronger, because unlike the plate
        styles there is no dark background here for the shell's text to sit on.
    """
    # Trim to the KEY-ART BAND before anything is scaled.  GBA boxes follow a
    # convention: logo in the top ~quarter, artwork through the middle,
    # ratings and publisher along the bottom.  Tuning `rise` alone could not
    # clear a tall wordmark -- Emerald's "EMERALD VERSION" still sat in the
    # text zone at 0.46 -- because the logo band is a different height on
    # every box.  Cutting it off is what generalises.
    cover = cover.crop((0, int(cover.height * 0.24),
                        cover.width, int(cover.height * 0.91)))

    # MUST cover both axes.  Scaling by height alone leaves a portrait cover
    # narrower than the 480 px frame, and the crop then returns a short image
    # that renders with black gutters down the sides.
    scale = max(W / cover.width, H / cover.height) * zoom
    cw, ch = int(cover.width * scale), int(cover.height * scale)
    big = cover.resize((cw, ch), Image.LANCZOS)
    left = max(0, min(cw - W, int((cw - W) * (0.5 + push))))
    top = max(0, min(ch - H, int((ch - H) * rise)))
    frame = big.crop((left, top, left + W, top + H))
    frame = ImageEnhance.Brightness(frame).enhance(0.74)
    frame = ImageEnhance.Color(frame).enhance(0.92)
    frame = _quiet_left(frame, 215, soften=6)
    return _quiet_bottom(frame)
