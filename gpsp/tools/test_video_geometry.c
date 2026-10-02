/* Host check of the real scale geometry in psp/video_psp.c, including the
 * integer 2x mode (docs/DISPLAY-FEATURES.md): where each picture lands,
 * which source rows it shows, and the TRIANGLE preset cycle (2x is opt-in,
 * not in it).  Compiled like test_video_buffers.c: PSP SDK headers, and
 * --gc-sections drops every GU call this never reaches.
 *
 * Also the staging copy's edge texels (docs/GB-PALETTE-FIXES.md): the
 * column right of the picture and the row below it repeat its edge, for a
 * GB and a GBA frame, whatever the staging held before.  run_host_tests.py
 * builds this twice, with and without USE_PSP_RGB565_FORMAT (the PSP's
 * straight copy and the swapping one). */
#include <assert.h>
#include <stdio.h>
#define VID_TRIPLE
#include "../psp/video_psp.c"

void sceKernelDcacheWritebackRange(const void *p, unsigned int size)
{
   (void)p;
   (void)size;
}

static uint16_t ge_order(uint16_t v)
{
#ifdef USE_PSP_RGB565_FORMAT
   return v;
#else
   return (uint16_t)(((v & 0xF800u) >> 11) | (v & 0x07E0u) |
                     ((v & 0x001Fu) << 11));
#endif
}

static void staging_edges(unsigned w, unsigned h)
{
   static uint16_t src[256 * 256];
   unsigned x, y, pitch = 256 * 2;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         src[y * 256 + x] = (uint16_t)(y * 251 + x * 7 + 1);
   memset(fb_staging, 0xA5, STAGE_BYTES);        /* stale picture */
   stage_convert_rgb565(src, w, h, pitch);
   for (y = 0; y < h; y++)
   {
      for (x = 0; x < w; x++)
         assert(fb_staging[y * STAGE_STRIDE + x] == ge_order(src[y * 256 + x]));
      assert(fb_staging[y * STAGE_STRIDE + w] ==
             ge_order(src[y * 256 + w - 1]));
   }
   if (h < STAGE_ROWS)
      for (x = 0; x <= w; x++)
         assert(fb_staging[h * STAGE_STRIDE + x] ==
                fb_staging[(h - 1) * STAGE_STRIDE + x]);
}

static void expect(int scale, unsigned w, unsigned h,
                   int ox, int oy, int dw, int dh, int sy0, int sh)
{
   vid_geo g;
   g_scale = scale;
   dest_geo(w, h, &g);
   if (g.ox != ox || g.oy != oy || g.dw != dw || g.dh != dh ||
       g.sy0 != sy0 || g.sh != sh)
   {
      printf("FAIL scale=%d %ux%u: got %d,%d %dx%d rows %d+%d\n", scale, w, h,
             g.ox, g.oy, g.dw, g.dh, g.sy0, g.sh);
      assert(0);
   }
   /* Every mode keeps the drawn picture on screen or symmetric about it. */
   assert(g.ox + g.dw / 2 == VID_SCR_W / 2 || g.ox * 2 + g.dw == VID_SCR_W);
}

int main(void)
{
   /* GBA 240x160 */
   expect(VID_SCALE_1X,      240, 160, 120, 56, 240, 160, 0, 160);
   expect(VID_SCALE_FIT,     240, 160,  36,  0, 408, 272, 0, 160);
   expect(VID_SCALE_STRETCH, 240, 160,   0,  0, 480, 272, 0, 160);
   /* 2x: 480x320 cropped to 272 = source rows 12..147, two px each. */
   expect(VID_SCALE_INT2,    240, 160,   0,  0, 480, 272, 12, 136);
   /* GB/GBC 160x144 */
   expect(VID_SCALE_1X,      160, 144, 160, 64, 160, 144, 0, 144);
   expect(VID_SCALE_FIT,     160, 144,  89,  0, 302, 272, 0, 144);
   expect(VID_SCALE_STRETCH, 160, 144,   0,  0, 480, 272, 0, 144);
   /* 2x: 320x288 cropped to 272 = rows 4..139, 80 px bars each side. */
   expect(VID_SCALE_INT2,    160, 144,  80,  0, 320, 272, 4, 136);
   /* A source that cannot double stays 1x rather than crop sideways. */
   expect(VID_SCALE_INT2,    256, 120, 112, 76, 256, 120, 0, 120);
   puts("PASS geometry: 1x / fit / stretch / 2x for GBA and GB");

   /* 2x is nearest whatever the filter setting says. */
   g_scale = VID_SCALE_INT2;
   g_filter = VID_FILTER_BILINEAR;
   assert(game_filter() == GU_NEAREST);
   g_scale = VID_SCALE_FIT;
   assert(game_filter() == GU_LINEAR);

   /* TRIANGLE: the five presets, never 2x; from 2x it goes to fit. */
   g_scale = VID_SCALE_1X;
   g_filter = VID_FILTER_NEAREST;
   {
      int i;
      for (i = 0; i < 10; i++)
      {
         vid_cycle_preset();
         assert(g_scale != VID_SCALE_INT2);
      }
   }
   g_scale = VID_SCALE_INT2;
   g_filter = VID_FILTER_BILINEAR;
   assert(strcmp(vid_cycle_preset(), "fit") == 0);
   assert(g_scale == VID_SCALE_FIT && g_filter == VID_FILTER_NEAREST);
   assert(strcmp(vid_scale_name(VID_SCALE_INT2), "2x") == 0);
   puts("PASS presets: 2x opt-in, TRIANGLE from 2x lands on fit");

   staging_edges(160, 144);
   staging_edges(240, 160);
   puts("PASS staging edge texels: GB and GBA, right column and bottom row");

   /* SHARP BILINEAR: the prescale per mode (docs/SHARP-BILINEAR.md). */
   {
      static const struct { int scale; unsigned w, h; int rc, kx, ky; } c[] = {
         { VID_SCALE_1X,      240, 160, 0, 0, 0 },   /* integer: nearest */
         { VID_SCALE_FIT,     240, 160, 1, 2, 2 },   /* 480x320 -> 408x272 */
         { VID_SCALE_STRETCH, 240, 160, 1, 2, 2 },   /* 480x320 -> 480x272 */
         { VID_SCALE_INT2,    240, 160, 0, 0, 0 },
         { VID_SCALE_1X,      160, 144, 0, 0, 0 },
         { VID_SCALE_FIT,     160, 144, 1, 2, 2 },   /* 320x288 -> 302x272 */
         { VID_SCALE_STRETCH, 160, 144, 1, 3, 2 },   /* 480x288 -> 480x272 */
         { VID_SCALE_INT2,    160, 144, 0, 0, 0 },
      };
      unsigned i;
      for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
      {
         vid_geo g;
         int kx = -1, ky = -1, rc;
         g_scale = c[i].scale;
         dest_geo(c[i].w, c[i].h, &g);
         rc = sharp_factors(c[i].w, &g, &kx, &ky);
         if (rc != c[i].rc || (rc == 1 && (kx != c[i].kx || ky != c[i].ky)))
         {
            printf("FAIL sharp scale=%d %ux%u: rc=%d k=%dx%d\n", c[i].scale,
                   c[i].w, c[i].h, rc, kx, ky);
            assert(0);
         }
         if (rc == 1)
         {
            /* The target, plus its duplicated edge, fits the render target
             * and is never smaller than the picture it is resampled to. */
            assert(kx * (int)c[i].w + 1 <= SHARP_STRIDE);
            assert(ky * g.sh + 1 <= SHARP_ROWS);
            assert(kx * (int)c[i].w >= g.dw && ky * g.sh >= g.dh);
         }
         /* The one-pass filter: an integer scale is nearest, exactly. */
         g_filter = VID_FILTER_SHARP;
         assert(picture_filter(c[i].w, &g) ==
                (rc == 0 ? GU_NEAREST : GU_LINEAR));
         g_filter = VID_FILTER_NEAREST;
         assert(picture_filter(c[i].w, &g) == GU_NEAREST);
         g_filter = VID_FILTER_BILINEAR;
         assert(picture_filter(c[i].w, &g) ==
                (c[i].scale == VID_SCALE_INT2 ? GU_NEAREST : GU_LINEAR));
      }
      /* The VRAM layout the doc states: above the ambient texture, 8 KiB
       * aligned, inside the 2 MiB every model has. */
      {
         uintptr_t off = (vram_free_base() + AMB_TEX_BYTES + 0x1FFFu) &
                         ~(uintptr_t)0x1FFFu;
         assert(vram_free_base() == 1196544u);
         assert(off == 1236992u);
         assert(off + SHARP_BYTES == 1581056u);
         assert(off + SHARP_BYTES <= 2097152u);
      }
   }
   puts("PASS sharp: 2x2 prescale for GBA fit/stretch, 2x2/3x2 for GB, "
        "nearest at 1x and 2x, VRAM layout");

   /* vid_set_mode / names / TRIANGLE with sharp chosen. */
   vid_set_mode(VID_SCALE_FIT, 7);              /* odd value: bilinear */
   assert(g_filter == VID_FILTER_BILINEAR && g_smooth == VID_FILTER_BILINEAR);
   assert(strcmp(vid_filter_name(VID_FILTER_SHARP), "sharp bilinear") == 0);
   vid_set_mode(VID_SCALE_FIT, VID_FILTER_NEAREST);
   assert(strcmp(vid_cycle_preset(), "fit bilinear") == 0);   /* unchanged */
   vid_set_mode(VID_SCALE_FIT, VID_FILTER_SHARP);
   assert(g_filter == VID_FILTER_SHARP && g_smooth == VID_FILTER_SHARP);
   assert(strcmp(vid_cycle_preset(), "stretch") == 0);
   assert(g_filter == VID_FILTER_NEAREST);
   assert(strcmp(vid_cycle_preset(), "stretch sharp") == 0);
   assert(g_scale == VID_SCALE_STRETCH && g_filter == VID_FILTER_SHARP);
   assert(strcmp(vid_cycle_preset(), "1x") == 0);
   assert(strcmp(vid_cycle_preset(), "fit") == 0);
   assert(strcmp(vid_cycle_preset(), "fit sharp") == 0);
   assert(g_filter == VID_FILTER_SHARP);
   g_scale = VID_SCALE_INT2;                     /* from 2x: fit, as before */
   assert(strcmp(vid_cycle_preset(), "fit") == 0);
   puts("PASS sharp presets: TRIANGLE keeps the chosen smoothing");
   return 0;
}
