/* Host check of the filters-v2 Colour row (video.h PRESENTATION COLOUR,
 * video_color.h): the very inline model video.cc and the Media Engine run.
 *
 *   - NATIVE is convert_palette() exactly, for every one of the 32768 guest
 *     colours (and the bitmap path's direct colour is the same function);
 *   - GBA LCD colour is gpSP's gba_cc_lut, every entry, in this build's pixel
 *     order, whichever path a colour takes (raw, or via the converted
 *     palette value);
 *   - black & white is neutral (R = G/2 = B in every output), keeps black
 *     and white, and is monotonic in each channel;
 *   - a blended output pixel (sixth green bit set) maps like its 5-bit
 *     neighbour, so the menu preview can transform any picture pixel.
 *
 * Built twice by run_host_tests.py: with -DUSE_PSP_RGB565_FORMAT (the PSP)
 * and without (standard RGB565). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;
typedef uint32_t u32;
#include "../video_color.h"

extern const u16 gba_cc_lut[];

#if defined(USE_PSP_RGB565_FORMAT)
#define CONVERT(v) ((u16)(((v) & 0x1F) | (((v) & 0x7FE0) << 1)))
static void split(u16 c, u32 *r, u32 *g, u32 *b)
{ *r = c & 31; *g = (c >> 6) & 31; *b = (c >> 11) & 31; }
#else
#define CONVERT(v) ((u16)((((v) & 0x1F) << 11) | (((v) & 0x03E0) << 1) | \
                          (((v) >> 10) & 0x1F)))
static void split(u16 c, u32 *r, u32 *g, u32 *b)
{ *r = (c >> 11) & 31; *g = (c >> 6) & 31; *b = c & 31; }
#endif

int main(void)
{
   u32 v;
   for (v = 0; v < 32768; v++)
   {
      u32 r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
      u16 conv = CONVERT(v);
      u32 cr, cg, cb, lr, lg, lb;
      u16 c;

      /* NATIVE: identical to the shipped conversion, both entry points. */
      assert(vcol_raw(v, VIDEO_COLOR_NATIVE, gba_cc_lut) == conv);
      assert(vcol_conv(conv, VIDEO_COLOR_NATIVE, gba_cc_lut) == conv);

      /* GBA: gba_cc_lut[R<<10|G<<5|B] = r<<11|g<<6|b, reordered. */
      c = vcol_raw(v, VIDEO_COLOR_GBA, gba_cc_lut);
      assert(vcol_conv(conv, VIDEO_COLOR_GBA, gba_cc_lut) == c);
      split(c, &cr, &cg, &cb);
      {
         u16 l = gba_cc_lut[(r << 10) | (g << 5) | b];
         lr = (l >> 11) & 31; lg = (l >> 6) & 31; lb = l & 31;
      }
      assert(cr == lr && cg == lg && cb == lb);
      /* ...and without a table it is native, never garbage. */
      assert(vcol_raw(v, VIDEO_COLOR_GBA, NULL) == conv);

      /* B&W: neutral, the luma of the channels. */
      c = vcol_raw(v, VIDEO_COLOR_BW, gba_cc_lut);
      assert(vcol_conv(conv, VIDEO_COLOR_BW, gba_cc_lut) == c);
      split(c, &cr, &cg, &cb);
      assert(cr == cg && cg == cb);
      assert(cr == (r * 77 + g * 151 + b * 28 + 128) >> 8);
      /* Monotonic: one more step in any channel never darkens. */
      if (r < 31) { u32 r2, g2, b2;
         split(vcol_raw(v + 1, VIDEO_COLOR_BW, NULL), &r2, &g2, &b2);
         assert(r2 >= cr); }
      if (g < 31) { u32 r2, g2, b2;
         split(vcol_raw(v + 32, VIDEO_COLOR_BW, NULL), &r2, &g2, &b2);
         assert(r2 >= cr); }
      if (b < 31) { u32 r2, g2, b2;
         split(vcol_raw(v + 1024, VIDEO_COLOR_BW, NULL), &r2, &g2, &b2);
         assert(r2 >= cr); }

      /* A blended output pixel (green's low bit set) maps like its 5-bit
       * neighbour. */
      assert(vcol_conv(conv | 0x20, VIDEO_COLOR_BW, NULL) ==
             vcol_conv(conv, VIDEO_COLOR_BW, NULL));
      assert(vcol_conv(conv | 0x20, VIDEO_COLOR_GBA, gba_cc_lut) ==
             vcol_conv(conv, VIDEO_COLOR_GBA, gba_cc_lut));
   }
   assert(vcol_raw(0, VIDEO_COLOR_BW, NULL) == 0);
   assert(vcol_raw(0x7FFF, VIDEO_COLOR_BW, NULL) == CONVERT(0x7FFF));
   puts("PASS colour rows: native exact, GBA = gba_cc_lut for all 32768, "
        "B&W neutral/monotonic, blended pixels");
   return 0;
}
