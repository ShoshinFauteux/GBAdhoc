"""Bake a hero image into the PSP's native texture format.

    python raw565.py <in_dir> <out_dir> [--no-dither]

WHY.  The GE samples RGB565.  A PNG hero therefore costs the console a zlib
inflate, a per-byte PNG unfilter pass, an RGB888->RGB565 conversion over
130,560 pixels, and then a copy -- to arrive at bytes we could simply have
stored in the first place.  A .565 file IS the texture: loading it is one
sceIoRead into the destination and nothing else.

FIDELITY.  Nothing is lost.  The texture is 16-bit either way; the only
question is where the 888->565 conversion happens.  Doing it here means we can
ORDERED-DITHER it, which the console's straight truncation cannot, so
gradients (skies, starfields) come out visibly cleaner than the PNG path
produces.

LAYOUT.  Rows are written at the TEXTURE stride, not the image width, so the
console can read the whole thing into the texture in one call with no
per-row seeking and no repacking.

  offset  size  field
  0       4     magic '565R'
  4       2     width      (texels of real image, e.g. 480)
  6       2     height     (e.g. 272)
  8       2     stride     (texels per row in the file, e.g. 512)
  10      2     flags      (bit0: dithered)
  12      4     reserved
  16      ...   stride*height u16, little-endian, PSP channel order

PSP CHANNEL ORDER IS NOT LIBRETRO'S.  The GE's 5650 texel is R in the low
bits: (r>>3) | ((g>>2)<<5) | ((b>>3)<<11).  Getting this backwards swaps red
and blue and looks like a corrupted palette -- the same trap ADR-0039
documents for the BMP dumper.
"""
import os
import struct
import sys

MAGIC = b'565R'
STRIDE = 512
HDR = 16

# 4x4 ordered dither, values -8..7 scaled to the quantisation step
_BAYER = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def to565(im, dither=True):
    """RGB888 image -> list of u16 rows in PSP channel order."""
    px = im.load()
    w, h = im.size
    rows = []
    for y in range(h):
        row = bytearray(STRIDE * 2)
        for x in range(w):
            r, g, b = px[x, y]
            if dither:
                # +/- half a quantisation step, so the error is spread rather
                # than always rounding the same way.
                t = _BAYER[y & 3][x & 3] - 8
                r = min(255, max(0, r + (t >> 1)))     # 8->5 bits: step 8
                g = min(255, max(0, g + (t >> 2)))     # 8->6 bits: step 4
                b = min(255, max(0, b + (t >> 1)))
            v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
            row[x * 2] = v & 0xFF
            row[x * 2 + 1] = (v >> 8) & 0xFF
        rows.append(bytes(row))
    return rows


def convert(src, dst, dither=True):
    from PIL import Image
    im = Image.open(src).convert('RGB')
    w, h = im.size
    if w > STRIDE:
        raise SystemExit('%s: %d px wide, will not fit the %d stride'
                         % (src, w, STRIDE))
    rows = to565(im, dither)
    with open(dst, 'wb') as fh:
        fh.write(struct.pack('<4sHHHHI', MAGIC, w, h, STRIDE,
                             1 if dither else 0, 0))
        for r in rows:
            fh.write(r)
    return os.path.getsize(dst)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    dither = '--no-dither' not in sys.argv
    os.makedirs(out_dir, exist_ok=True)
    tot_in = tot_out = 0
    for fn in sorted(os.listdir(src_dir)):
        if not fn.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp')):
            continue
        src = os.path.join(src_dir, fn)
        dst = os.path.join(out_dir, os.path.splitext(fn)[0] + '.565')
        n = convert(src, dst, dither)
        tot_in += os.path.getsize(src)
        tot_out += n
        print('  %-46s %6d KB -> %6d KB' %
              (fn[:46], os.path.getsize(src) // 1024, n // 1024))
    print()
    print('total %d KB -> %d KB   (dither %s)'
          % (tot_in // 1024, tot_out // 1024, 'on' if dither else 'off'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
