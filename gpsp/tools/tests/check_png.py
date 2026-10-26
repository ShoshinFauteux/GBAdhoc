#!/usr/bin/env python3
"""check_png.py DIR -- decode every DIR/*.png written by test_fe_png with an
independent decoder (Python's zlib + this file's unfilter), check every chunk
CRC, and compare the pixels with DIR/<name>.rgb.  If Pillow is installed the
file is also opened with it, as a second opinion."""
import struct
import sys
import zlib
from pathlib import Path


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    pos, idat, plte, ihdr, seen = 8, b"", None, None, []
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        assert zlib.crc32(typ + body) & 0xFFFFFFFF == crc, "CRC %s" % typ
        seen.append(typ)
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    assert seen[0] == b"IHDR" and seen[-1] == b"IEND", seen
    w, h, depth, ctype, comp, filt, inter = ihdr
    assert comp == 0 and filt == 0 and inter == 0
    raw = zlib.decompress(idat)          # also verifies the Adler-32
    if ctype == 3:
        stride, bpp = (w * depth + 7) // 8, 1
        assert plte and len(plte) % 3 == 0
    else:
        assert ctype == 2 and depth == 8
        stride, bpp = w * 3, 3
    assert len(raw) == (stride + 1) * h, "raw size"
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]
            line[i] = (line[i] + p) & 255
        prev = line
        if ctype == 3:
            for x in range(w):
                bit = x * depth
                v = (line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
                assert v * 3 + 3 <= len(plte), "index past PLTE"
                out += plte[v * 3:v * 3 + 3]
        else:
            out += line
    return w, h, bytes(out), ctype, depth


def main():
    d = Path(sys.argv[1])
    pngs = sorted(d.glob("*.png"))
    assert pngs, "no PNGs in %s" % d
    try:
        from PIL import Image  # noqa
        have_pil = True
    except Exception:
        have_pil = False
    bad = 0
    for p in pngs:
        ref = (d / (p.stem + ".rgb")).read_bytes()
        hdr, pix = ref.split(b"\n", 1)
        rw, rh = map(int, hdr.split())
        try:
            w, h, rgb, ctype, depth = decode(p.read_bytes())
            assert (w, h) == (rw, rh), "size"
            assert rgb == pix, "pixels differ"
            if have_pil:
                im = Image.open(p).convert("RGB")
                assert im.size == (w, h) and im.tobytes() == pix, "Pillow disagrees"
            print("  ok  %-16s %4d bytes  type %d depth %d" %
                  (p.name, p.stat().st_size, ctype, depth))
        except AssertionError as e:
            bad += 1
            print("  FAIL %s: %s" % (p.name, e))
    print("check_png: %d/%d decoded identically%s" %
          (len(pngs) - bad, len(pngs), " (and by Pillow)" if have_pil else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
