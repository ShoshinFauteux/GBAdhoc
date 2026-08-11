#!/usr/bin/env python3
"""check_ge_colors.py - GU color-order assertions on GE drawbuffer dumps.

The PSP frontend's `gedump_at`/`testpat` paths read back the GE DRAWBUFFER
(raw VRAM bytes decoded with the real GE 5650 layout: R bits 0-4) and write
a 24bpp BMP.  This checker asserts those dumps show the colors the core
produced, which fails loudly if anyone reintroduces a channel-order bug in
the GU blit path (hw finding 2026-08-01: RGB565 uploaded as GU_PSM_5650
renders R/B-swapped on real GE; PPSSPP round-trips it invisibly on screen,
but the raw drawbuffer bytes still expose it -- which is what we decode).

Modes:
  bars <ge.bmp>
      Assert the 8 known test-pattern bars (testpat=1) at their screen
      positions (1x centered blit at 120,56).  Exact match required.
  compare <core.bmp> <ge.bmp> <ox> <oy>
      Assert the GE dump region (ox,oy)+(240x160) is pixel-identical to the
      core-buffer BMP (both are lossless 565->888 expansions).
Exit 0 = pass; prints expected-vs-seen on failure.
"""
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: expected 24bpp, got {bpp}")
    row = (w * 3 + 3) & ~3
    px = [[None] * w for _ in range(abs(h))]
    flip = h > 0  # positive height = bottom-up
    h = abs(h)
    for y in range(h):
        src_y = (h - 1 - y) if flip else y
        base = off + src_y * row
        line = px[y]
        for x in range(w):
            b, g, r = data[base + x * 3: base + x * 3 + 3]
            line[x] = (r, g, b)
    return w, h, px


def expand565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


# testpat bars: RGB565 values as fed to the blit path (main_psp.c run_testpat)
BARS = [0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0x8410]
BAR_W, BLIT_OX, BLIT_OY = 30, 120, 56


def mode_bars(ge_path):
    w, h, px = read_bmp(ge_path)
    if (w, h) != (480, 272):
        print(f"FAIL: GE dump is {w}x{h}, expected 480x272")
        return 1
    bad = 0
    for i, c in enumerate(BARS):
        x = BLIT_OX + i * BAR_W + BAR_W // 2
        y = BLIT_OY + 80
        want = expand565(c)
        got = px[y][x]
        tag = "OK  " if got == want else "FAIL"
        if got != want:
            bad += 1
        print(f"{tag} bar{i} 565=0x{c:04x} at ({x},{y}) want={want} got={got}")
    # borders (outside the 1x blit) must be cleared to black
    for (x, y) in ((10, 136), (470, 136), (240, 10), (240, 265)):
        got = px[y][x]
        if got != (0, 0, 0):
            bad += 1
            print(f"FAIL border ({x},{y}) want=(0,0,0) got={got}")
    return 1 if bad else 0


def mode_compare(core_path, ge_path, ox, oy):
    cw, ch, cpx = read_bmp(core_path)
    gw, gh, gpx = read_bmp(ge_path)
    if (cw, ch) != (240, 160) or (gw, gh) != (480, 272):
        print(f"FAIL: sizes core={cw}x{ch} ge={gw}x{gh}")
        return 1
    diff = 0
    first = None
    for y in range(ch):
        for x in range(cw):
            if cpx[y][x] != gpx[oy + y][ox + x]:
                diff += 1
                if first is None:
                    first = (x, y, cpx[y][x], gpx[oy + y][ox + x])
    total = cw * ch
    if diff:
        x, y, want, got = first
        print(f"FAIL: {diff}/{total} pixels differ; first at ({x},{y}) "
              f"core={want} ge={got}")
        return 1
    print(f"OK   GE region ({ox},{oy})+240x160 pixel-identical to core BMP "
          f"({total} px)")
    return 0


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "bars":
        sys.exit(mode_bars(sys.argv[2]))
    if len(sys.argv) >= 6 and sys.argv[1] == "compare":
        sys.exit(mode_compare(sys.argv[2], sys.argv[3],
                              int(sys.argv[4]), int(sys.argv[5])))
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
