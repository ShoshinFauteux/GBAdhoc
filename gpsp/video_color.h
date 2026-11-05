/* video_color.h -- the PRESENTATION COLOUR model (video.h), as pure inline
 * functions of (colour, mode, LUT) so the host test (tools/test_color_rows.c)
 * checks the very code video.cc and the Media Engine run.
 *
 * Needs u16/u32 and this build's pixel-format define (USE_PSP_RGB565_FORMAT /
 * USE_XBGR1555_FORMAT / neither = standard RGB565), exactly as common.h's
 * convert_palette() does. */
#ifndef VIDEO_COLOR_H
#define VIDEO_COLOR_H

#define VIDEO_COLOR_NATIVE 0
#define VIDEO_COLOR_GBA    1
#define VIDEO_COLOR_BW     2
#define VIDEO_COLOR_MODES  3

/* One GBA colour (BGR555 as the guest stores it: R bits 0-4, G 5-9, B 10-14)
 * through the model, in this build's output format.  NATIVE is
 * convert_palette()'s result exactly. */
static inline u16 vcol_raw(u32 v, u32 mode, const u16 *lut)
{
  u32 r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
  if (mode == VIDEO_COLOR_GBA && lut)
  {
    /* gba_cc_lut: index R<<10 | G<<5 | B, value r<<11 | g<<6 | b. */
    u32 c = lut[(r << 10) | (g << 5) | b];
    r = (c >> 11) & 0x1F;  g = (c >> 6) & 0x1F;  b = c & 0x1F;
  }
  else if (mode == VIDEO_COLOR_BW)
  {
    /* BT.601 luma on the 5-bit channels (77 + 151 + 28 = 256): neutral,
     * white stays white, black stays black. */
    r = g = b = (r * 77 + g * 151 + b * 28 + 128) >> 8;
  }
#if defined(USE_XBGR1555_FORMAT)
  return (u16)(r | (g << 5) | (b << 10));
#elif defined(USE_PSP_RGB565_FORMAT)
  return (u16)(r | (g << 6) | (b << 11));
#else
  return (u16)((r << 11) | (g << 6) | b);
#endif
}

/* The same for a value convert_palette() produced (or any pixel of the
 * output format: a blended pixel's sixth green bit is dropped). */
static inline u16 vcol_conv(u32 c, u32 mode, const u16 *lut)
{
#if defined(USE_XBGR1555_FORMAT)
  return vcol_raw(c & 0x7FFF, mode, lut);
#elif defined(USE_PSP_RGB565_FORMAT)
  return vcol_raw((c & 0x1F) | ((c >> 1) & 0x7FE0), mode, lut);
#else
  return vcol_raw(((c >> 11) & 0x1F) | ((c >> 1) & 0x3E0) |
                  ((c & 0x1F) << 10), mode, lut);
#endif
}

#endif /* VIDEO_COLOR_H */
