"""gesurface.py -- pspdraw's Surface with the GE's arithmetic.

pspdraw (builds/menu-mockups) blends in floating point and quantises the
finished frame to RGB565 once.  The GE does not: every draw call blends in
8-bit integers against the 565 framebuffer and writes 565 back.  Over a
photograph, through a scrim, a ramp and a band, that difference is a 1-2 LSB
disagreement on a third of the pixels -- with every coordinate, colour and
alpha identical.  This surface does what the GE does, so a PPSSPP GE dump and
the model can be compared for exact identity (docs/UI-OVERLAY.md).

Measured against PPSSPP's software GE (see docs/UI-OVERLAY.md, "the model"):
  * blend    out = (src * a + dst * (255 - a)) // 255, per 8-bit channel
  * write    8-bit -> 565 by truncation; read back by bit replication
  * text     alpha = the atlas coverage byte (CLUT index == alpha)
  * ramp     per-row alpha interpolated across the strip (see gradient_a)
"""
import numpy as np
from PIL import Image
import pspdraw as P


def q565(rgb):
    """8-bit RGB (int array) -> the framebuffer's 565 -> 8-bit again."""
    r = rgb[..., 0] >> 3
    g = rgb[..., 1] >> 2
    b = rgb[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


class GESurface(P.Surface):
    # gradient row alpha: 'linspace' (pspdraw) or 'ge' (per-pixel-centre)
    ramp = 'ge'

    def __init__(self, theme='dark', console='GBA', backdrop=None):
        super().__init__(theme, console)
        self.ib = np.zeros((P.SCR_H, P.SCR_W, 3), np.int64)
        if backdrop is not None:
            self.set_fb(backdrop)

    def set_fb(self, pil):
        self.ib = q565(np.asarray(pil.convert('RGB'), dtype=np.int64))

    # a: (h, w) float alpha 0..1 (from pspdraw); col (3,) or (h, w, 3) floats
    def _blend(self, x, y, a, col):
        h, w = a.shape
        cx, cy, cw, ch = self.clip_rect
        x0, y0 = max(x, cx, 0), max(y, cy, 0)
        x1, y1 = min(x + w, cx + cw, P.SCR_W), min(y + h, cy + ch, P.SCR_H)
        if x1 <= x0 or y1 <= y0:
            return
        ai = np.rint(a[y0 - y:y1 - y, x0 - x:x1 - x] * 255).astype(np.int64)[..., None]
        sc = (col if col.ndim == 1 else col[y0 - y:y1 - y, x0 - x:x1 - x]).astype(np.int64)
        reg = self.ib[y0:y1, x0:x1]
        out = (sc * ai + reg * (255 - ai)) // 255
        self.ib[y0:y1, x0:x1] = q565(out)

    def gradient_a(self, x, y, w, h, c, a_top, a_bot):
        self.calls += 1
        if self.ramp == 'linspace':
            ramp = np.linspace(a_top, a_bot, h)
        else:
            # vertex alpha interpolated at each row's position from the top
            # vertex: a_top + (a_bot - a_top) * i / h, truncated
            i = np.arange(h)
            ramp = a_top + (a_bot - a_top) * i // h
        ramp = np.asarray(ramp, dtype=np.float64) / 255.0
        self._blend(x, y, np.repeat(ramp[:, None], w, axis=1), P.c565_to_rgb(c))

    def image_px(self, x, y, pil):
        """vid_image_px: 1:1, nearest, opaque."""
        self.calls += 1
        a = q565(np.asarray(pil.convert('RGB'), dtype=np.int64))
        h, w = a.shape[:2]
        self.ib[y:y + h, x:x + w] = a

    def finish(self):
        return Image.fromarray(self.ib.astype(np.uint8), 'RGB')
