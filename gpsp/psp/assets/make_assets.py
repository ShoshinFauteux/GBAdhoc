#!/usr/bin/env python3
"""make_assets.py - derive the XMB assets for the GENERIC "PSP AGB" build
from psp/assets/xmb_source.png (purple-transparent PSP with the GBA
Wireless Adapter, "PSP AGB" on screen).

  ICON0.PNG  144x80   XMB game icon (fill-crop, slight top bias so the
                      adapter + GAME BOY label survive at icon size)
  PIC1.PNG   480x272  XMB background

Reproducible: run under any Python with Pillow, e.g.
  docker run --rm -v <repo>:/build -w /build/psp/assets python:3.12-slim \
      sh -c "pip install -q pillow && python make_assets.py"
Per-game invisible variants do NOT use these; their icon comes from
tools/make_variant.sh arguments (docs/VARIANTS.md).
"""
from PIL import Image

SRC = "xmb_source.png"


def fill_crop(im, tw, th, top_bias):
    """Scale-to-fill then crop tw x th; top_bias 0..1 picks the vertical
    window (0 = top, 0.5 = center)."""
    sw, sh = im.size
    scale = max(tw / sw, th / sh)
    rw, rh = round(sw * scale), round(sh * scale)
    r = im.resize((rw, rh), Image.LANCZOS)
    x = (rw - tw) // 2
    y = round((rh - th) * top_bias)
    return r.crop((x, y, x + tw, y + th))


def letterbox(im, tw, th, bg=(12, 12, 16)):
    """Scale-to-fit inside tw x th, centered on a dark background so the
    whole device + adapter silhouette reads at icon size."""
    sw, sh = im.size
    scale = min(tw / sw, th / sh)
    rw, rh = round(sw * scale), round(sh * scale)
    r = im.resize((rw, rh), Image.LANCZOS)
    out = Image.new("RGB", (tw, th), bg)
    out.paste(r, ((tw - rw) // 2, (th - rh) // 2))
    return out


def main():
    src = Image.open(SRC).convert("RGB")
    # Icon: the whole console + adapter, letterboxed — a crop that fills
    # 1.8:1 loses either the adapter or the screen logo at this size.
    letterbox(src, 144, 80).save("ICON0.PNG", optimize=True)
    # Background: fill-crop, slight top bias keeps the adapter visible.
    fill_crop(src, 480, 272, 0.35).save("PIC1.PNG", optimize=True)
    print("wrote ICON0.PNG (144x80) and PIC1.PNG (480x272)")


if __name__ == "__main__":
    main()
