"""Compose a 480x272 'hero' backdrop for the GBAdhoc marquee shell.

WHY THIS EXISTS.  Box art is a ~1:1 portrait object; the PSP screen is 16:9.
Scaling a cover to fill 480x272 crops ~40% off the top and bottom, and what
survives is usually the middle of the artwork plus the silver GAME BOY ADVANCE
spine running down the left -- exactly where the marquee draws its title and
list.  The result reads as a mistake rather than a design.

So the hero is COMPOSED for the UI that sits on it, not merely resized:

  * the spine is detected and cropped away (it is a low-saturation vertical
    band on the left of every libretro GBA boxart, and it is branding, not art)
  * the artwork is blurred, zoomed and darkened to make a backdrop that has
    the cover's colour without competing with text
  * the cover itself sits SHARP on the right, where the marquee has no text
  * the left third is quietened further, because that is where the title,
    metadata and list rows land

Everything is derived from the cover, so it works for any game rather than
only the ones someone hand-made a wallpaper for.
"""
import os
from PIL import Image, ImageFilter, ImageEnhance, ImageStat

W, H = 480, 272          # PSP native
ART_H = 232              # sharp cover height in the composition
LEFT_QUIET = 0.58        # fraction of width kept calm for the UI


def _spine_width(im):
    """Width of the silver GBA spine on the left, in pixels.

    Two earlier attempts and why they failed, because both look reasonable:

      * plain per-column saturation -- fooled by Mario Kart, whose box has a
        small coloured corner at the very top; averaging the whole column
        dragged its mean above the threshold and the spine survived.
      * strongest vertical edge -- found the box's OUTER border (~36 px on a
        512 px scan) rather than the spine/front seam (~72 px), which made
        every cover worse.

    So: saturation, but sampled over the MIDDLE 60% of rows only (missing the
    decorative corners), smoothed over a small column window, and bounded so a
    dark or monochrome cover can never lose its artwork.
    """
    hsv = im.convert('HSV')
    w, h = im.size
    sat = hsv.split()[1]
    y0, y1 = int(h * 0.20), int(h * 0.80)
    band = sat.crop((0, y0, w, y1))
    px = band.load()
    bh = y1 - y0
    step = max(1, bh // 80)
    col = []
    for x in range(int(w * 0.34)):
        acc = n = 0
        for y in range(0, bh, step):
            acc += px[x, y]; n += 1
        col.append(acc / max(1, n))
    win = 5
    for x in range(len(col) - win):
        if sum(col[x:x + win]) / win > 40.0:      # colour genuinely starts
            return x
    return 0

# A GBA box spine is a consistent fraction of the front: ~14% of a square
# scan.  Detection is preferred, but a scan whose spine is dark or textured
# reports 0 and the branding then survives into the wallpaper (Minish Cap).
# So: trust detection when it lands in a plausible band, else use the nominal.
SPINE_NOMINAL = 0.145
SPINE_MIN, SPINE_MAX = 0.09, 0.26


def _cover_only(im):
    """Crop the GBA spine off the left of a boxart scan."""
    s = _spine_width(im)
    frac = s / float(im.width) if s else 0.0
    if not (SPINE_MIN <= frac <= SPINE_MAX):
        s = int(im.width * SPINE_NOMINAL)
    return im.crop((s, 0, im.width, im.height))


def _backdrop(cover):
    """Blurred, zoomed, darkened fill derived from the cover itself."""
    scale = max(W / cover.width, H / cover.height) * 1.35   # overscan
    bw, bh = int(cover.width * scale), int(cover.height * scale)
    bg = cover.resize((bw, bh), Image.LANCZOS)
    bg = bg.crop(((bw - W) // 2, (bh - H) // 2,
                  (bw - W) // 2 + W, (bh - H) // 2 + H))
    bg = bg.filter(ImageFilter.GaussianBlur(14))
    # NEARLY GREYSCALE ON PURPOSE.  The shell is monochrome -- greys, white,
    # and exactly one saturated object.  A full-colour blurred backdrop
    # competes with the cover for that role and makes the screen muddy;
    # draining it keeps the cover as the only colour on the display, which is
    # the same rule that makes the Shelf shell work.
    bg = ImageEnhance.Color(bg).enhance(0.18)
    bg = ImageEnhance.Brightness(bg).enhance(0.55)
    bg = ImageEnhance.Contrast(bg).enhance(1.15)
    return bg


def _quiet_left(img):
    """Darken the left side where the marquee draws its text.

    A horizontal ramp, not a hard edge: the seam is what made the in-emulator
    two-rect scrim look wrong.
    """
    ramp = Image.new('L', (W, 1))
    for x in range(W):
        t = x / (W * LEFT_QUIET)
        v = 0 if t >= 1 else int(150 * (1 - t) ** 1.6)
        ramp.putpixel((x, 0), v)
    ramp = ramp.resize((W, H))
    return Image.composite(Image.new('RGB', (W, H), (8, 8, 10)), img, ramp)


def compose(src_path, out_path):
    im = Image.open(src_path).convert('RGB')
    cover = _cover_only(im)

    canvas = _quiet_left(_backdrop(cover))

    # sharp cover, right-aligned, its own aspect preserved
    ch = ART_H
    cw = max(1, int(cover.width * ch / cover.height))
    sharp = cover.resize((cw, ch), Image.LANCZOS)
    x = W - cw - 26
    y = (H - ch) // 2

    shadow = Image.new('RGBA', (cw + 24, ch + 24), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 150), (12, 12, cw + 12, ch + 12))
    shadow = shadow.filter(ImageFilter.GaussianBlur(9))
    canvas.paste(Image.new('RGB', (1, 1)), (0, 0))          # no-op, keeps RGB
    canvas = Image.alpha_composite(canvas.convert('RGBA'),
                                   _place(shadow, x - 12, y - 12)).convert('RGB')
    canvas.paste(sharp, (x, y))

    canvas.save(out_path)
    return out_path


def _place(layer, x, y):
    full = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    full.paste(layer, (x, y), layer)
    return full
