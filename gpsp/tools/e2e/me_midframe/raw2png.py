#!/usr/bin/env python3
"""Put ME_TIMING_SIM dumps side by side: cpu | vis (| post) as one PNG.

    raw2png.py DUMP_DIR FRAME OUT.png

The dumps are 240x160 RGB565 (the core's desktop format, R in the high bits).
Pure standard library, so it runs anywhere the twin does."""
import os
import struct
import sys
import zlib


def load(path):
    data = open(path, 'rb').read()
    px = struct.unpack('<%dH' % (len(data) // 2), data)
    rows = []
    for y in range(160):
        row = bytearray()
        for v in px[y * 240:(y + 1) * 240]:
            row += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                          (v & 31) * 255 // 31))
        rows.append(row)
    return rows


def main():
    d, f, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    tags = [t for t in ('cpu', 'vis', 'post')
            if os.path.exists(os.path.join(d, 'f%06d_%s.raw' % (f, t)))]
    imgs = [load(os.path.join(d, 'f%06d_%s.raw' % (f, t))) for t in tags]
    gap = bytes((255, 255, 255)) * 4
    raw = b''.join(b'\0' + gap.join(bytes(im[y]) for im in imgs) for y in range(160))
    w = 240 * len(imgs) + 4 * (len(imgs) - 1)

    def chunk(k, v):
        return struct.pack('>I', len(v)) + k + v + struct.pack('>I', zlib.crc32(k + v))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, 160, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    open(out, 'wb').write(png)
    print(out, '=', ' | '.join(tags))


if __name__ == '__main__':
    main()
