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
#include <stdlib.h>
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
   if (!fb_staging)
      fb_staging = (uint16_t *)malloc(STAGE_BYTES);   /* vid_init is not run */
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


/* GRID FILTERS: the darkness the GE gives screen pixel X of one axis --
 * game_draw()'s strips, the mask's texel = GRID_N x the source coordinate,
 * sampled at the pixel centre (LINEAR: between the two nearest texel
 * centres; NEAREST: the texel under it) -- and in *pos that centre's source
 * coordinate.  n source px over d screen px. */
static float grid_sample(const float *prof, int filt, int n, int d, int X,
                         float *pos)
{
   int x0 = (X * n) / d / 60 * 60, sw, dx0, dx1;
   float u, f, w;
   int i0;
   for (;;)
   {
      sw  = n - x0 < 60 ? n - x0 : 60;
      dx0 = x0 * d / n;
      dx1 = (x0 + sw) * d / n;
      if (X < dx0)      x0 -= 60;
      else if (X >= dx1) x0 += 60;
      else break;
   }
   u = ((float)X + 0.5f - (float)dx0) * (float)(sw * GRID_N) /
       (float)(dx1 - dx0);
   *pos = (float)x0 + u / GRID_N;
   if (filt == GU_NEAREST)
      return prof[(int)u % GRID_N];
   f  = u - 0.5f;
   i0 = (int)(f + GRID_N) - GRID_N;            /* floor */
   w  = f - (float)i0;
   return prof[(i0 + GRID_N) % GRID_N] * (1.0f - w) +
          prof[(i0 + 1 + GRID_N) % GRID_N] * w;
}

static void grid_axis_check(int n, int d)
{
   float prof[GRID_N], sum[256], b = (float)n / (float)d, pos, tot = 0.0f;
   /* Screen pixels of line per game pixel: 1 at an integer scale (the last
    * pixel), else a half-game-pixel line = d / 2n pixels (as dark as 2x). */
   float want = d % n == 0 ? 1.0f : (b < 0.5f ? 1.0f : 0.5f / b);
   int filt = grid_axis(d, n, prof), X, i;
   if (d <= n)
   {
      assert(filt == GRID_NONE);               /* 1x: nothing to draw */
      return;
   }
   assert(filt == (d % n == 0 ? GRID_HARD : GRID_SOFT));
   filt = filt == GRID_HARD ? GU_NEAREST : GU_LINEAR;
   for (i = 0; i < n; i++)
      sum[i] = 0.0f;
   for (X = 0; X < d; X++)
   {
      float a = grid_sample(prof, filt, n, d, X, &pos);
      /* The line belonging to game pixel i is centred about b/2 left of its
       * right edge i + 1: credit the pixel to the nearest such centre. */
      i = (int)(pos + b * 0.5f + 0.5f) - 1;
      if (i >= 0 && i < n)
         sum[i] += a;
      tot += a;
      if (d % n == 0)
      {
         /* Integer k: exactly the last screen pixel of each game pixel,
          * under LINEAR (the other axis fractional) as under NEAREST. */
         int last = (X + 1) % (d / n) == 0;
         assert(a == (last ? 1.0f : 0.0f));
         assert(grid_sample(prof, GU_LINEAR, n, d, X, &pos) == a);
      }
   }
   /* The same amount of line per game pixel, overall and line by line at
    * any phase (fractional: within the strips' rounding, the texel
    * steps, and the soft edge that spills past the credit window). */
   assert(tot > 0.97f * want * (float)n && tot < 1.03f * want * (float)n);
   for (i = 0; i < n - 1; i++)
      if (sum[i] < 0.8f * want || sum[i] > 1.2f * want)
      {
         fprintf(stderr, "FAIL grid n=%d d=%d: line %d darkness %.3f\n",
                 n, d, i, sum[i]);
         assert(0);
      }
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

   /* GRID FILTERS: one period per GAME pixel, per axis, at every scale. */
   {
      static const int ax[][2] = {
         { 240, 240 }, { 160, 160 }, { 144, 144 },      /* 1x: none        */
         { 240, 480 }, { 136, 272 }, { 160, 320 },      /* 2x, GBA 2x crop */
         { 240, 408 }, { 160, 272 },                    /* GBA fit         */
         { 160, 302 }, { 144, 272 },                    /* GB fit          */
         { 160, 480 },                                  /* GB stretch x 3  */
      };
      static const struct { int scale; unsigned w, h; int filt; } m[] = {
         { VID_SCALE_1X,      240, 160, -1 },
         { VID_SCALE_1X,      160, 144, -1 },
         { VID_SCALE_INT2,    240, 160, GU_NEAREST },
         { VID_SCALE_INT2,    160, 144, GU_NEAREST },
         { VID_SCALE_FIT,     240, 160, GU_LINEAR },
         { VID_SCALE_FIT,     160, 144, GU_LINEAR },
         { VID_SCALE_STRETCH, 240, 160, GU_LINEAR },     /* rows 1.7x */
         { VID_SCALE_STRETCH, 160, 144, GU_LINEAR },
      };
      unsigned i;
      int k, sm;
      for (i = 0; i < sizeof(ax) / sizeof(ax[0]); i++)
         grid_axis_check(ax[i][0], ax[i][1]);
      for (i = 0; i < sizeof(m) / sizeof(m[0]); i++)
         for (k = 0; k < 3; k++)
            for (sm = 0; sm < VID_SMOOTH_MODES; sm++)
         {
            vid_geo g;
            int filt = -1;
            const unsigned int *t;
            vid_set_look(m[i].scale, sm, VID_FX_LCD + k);
            dest_geo(m[i].w, m[i].h, &g);
            /* The effect never changes how the PICTURE is sampled. */
            if (sm == VID_SMOOTH_NEAREST)
            {
               assert(game_filter() == GU_NEAREST);
               assert(picture_filter(m[i].w, &g) == GU_NEAREST);
            }
            t = grid_tex(VID_FX_LCD + k, m[i].w, &g, &filt);
            assert((t == NULL) == (m[i].filt < 0));
            assert(vid_fx_state(VID_FX_LCD + k, m[i].scale, m[i].w, m[i].h)
                   == (t ? VID_FXS_ON : VID_FXS_OFF_1X));
            if (t)
               assert(filt == m[i].filt);
            if (t && m[i].scale == VID_SCALE_INT2)
            {
               /* 2x is the community patch's own pattern: texels 0-7 are
                * the first screen pixel, 8-15 the second. */
               int x, y;
               for (y = 0; y < GRID_N; y++)
                  for (x = 0; x < GRID_N; x++)
                  {
                     int ox = x >= GRID_N / 2, oy = y >= GRID_N / 2;
                     unsigned int want =
                        k == 0 ? ((ox || oy) ? 0x38000000u : 0)
                      : k == 1 ? ((ox && oy) ? 0x68000000u : 0)
                      :          (oy ? 0x58000000u : 0);
                     assert(t[y * GRID_N + x] == want);
                  }
            }
         }
      /* The ring: a mode seen before reuses its slot, no rebuild. */
      {
         vid_geo g;
         int filt;
         const unsigned int *a, *b;
         g_scale = VID_SCALE_FIT;
         dest_geo(240, 160, &g);
         a = grid_tex(VID_FX_LCD, 240, &g, &filt);
         b = grid_tex(VID_FX_LCD, 240, &g, &filt);
         assert(a == b);
      }
      /* Legacy values 3..5 are nearest + that grid; TRIANGLE keeps the
       * effect and cycles only the smoothing. */
      vid_set_mode(VID_SCALE_FIT, VID_FILTER_BILINEAR);   /* g_smooth */
      vid_set_mode(VID_SCALE_FIT, VID_FILTER_SCAN);
      assert(g_filter == VID_FILTER_NEAREST && g_fx == VID_FX_SCAN);
      assert(g_smooth == VID_FILTER_BILINEAR);   /* nearest is not a choice */
      assert(vid_filter() == VID_FILTER_SCAN);
      assert(strcmp(vid_filter_name(VID_FILTER_LCD), "LCD grid") == 0);
      assert(strcmp(vid_cycle_preset(), "fit bilinear, scanlines") == 0);
      assert(g_filter == VID_FILTER_BILINEAR && g_fx == VID_FX_SCAN);
      assert(strcmp(vid_cycle_preset(), "stretch, scanlines") == 0);
      assert(g_filter == VID_FILTER_NEAREST && g_fx == VID_FX_SCAN);
      vid_set_mode(VID_SCALE_FIT, 9);           /* out of range: bilinear */
      assert(g_filter == VID_FILTER_BILINEAR && g_fx == VID_FX_NONE);
   }
   puts("PASS grid filters: one period per game pixel (1x none, 2x the "
        "patch's pattern, fit/stretch even), any smoothing, presets");

   /* FILTERS V2: the legacy value and the two GE rows. */
   {
      int f, sm, fx;
      for (f = 0; f < VID_FILTER_MODES; f++)
      {
         vid_filter_split(f, &sm, &fx);
         assert(vid_filter_join(sm, fx) == f);
         vid_set_mode(VID_SCALE_FIT, f);
         assert(vid_filter() == f && vid_smooth() == sm && vid_fx() == fx);
      }
      for (sm = 0; sm < VID_SMOOTH_MODES; sm++)
         for (fx = 0; fx < VID_FX_MODES; fx++)
         {
            int j = vid_filter_join(sm, fx);
            assert(j >= 0 && j < VID_FILTER_MODES);
            /* An older build shows the grid when there is one, else the
             * smoothing. */
            assert(fx == VID_FX_NONE ? j == sm : j >= VID_FILTER_LCD);
         }
      assert(!vid_fx_offered(VID_FX_RGB, 0) && vid_fx_offered(VID_FX_RGB, 1));
      assert(!vid_fx_offered(VID_FX_RGB_VIVID, 0));
      assert(vid_fx_offered(VID_FX_VINTAGE, 0) && vid_fx_offered(VID_FX_LCD, 0));
      assert(!vid_fx_offered(VID_FX_MODES, 1) && !vid_fx_offered(-1, 1));
   }
   puts("PASS filters v2 rows: 0..5 split and join exactly");

   /* VINTAGE: the LCD grid's shape at 0x15, and its tone even at 1x. */
   {
      vid_geo g;
      int filt, x, y;
      const unsigned int *t, *l;
      vid_set_look(VID_SCALE_INT2, 0, VID_FX_VINTAGE);
      dest_geo(240, 160, &g);
      t = grid_tex(VID_FX_VINTAGE, 240, &g, &filt);
      l = grid_tex(VID_FX_LCD, 240, &g, &filt);
      assert(t && l && t != l);
      for (y = 0; y < GRID_N; y++)
         for (x = 0; x < GRID_N; x++)
            assert((t[y * GRID_N + x] != 0) == (l[y * GRID_N + x] != 0) &&
                   (t[y * GRID_N + x] == 0 ||
                    t[y * GRID_N + x] == 0x15000000u));
      g_scale = VID_SCALE_1X;
      dest_geo(240, 160, &g);
      assert(grid_tex(VID_FX_VINTAGE, 240, &g, &filt) == NULL);
      assert(vid_fx_state(VID_FX_VINTAGE, VID_SCALE_1X, 240, 160) ==
             VID_FXS_TONE_1X);
      assert(vid_fx_state(VID_FX_VINTAGE, VID_SCALE_FIT, 160, 144) ==
             VID_FXS_ON);
   }
   puts("PASS vintage: the LCD grid's shape, faint; tone only at 1x");

   /* RGB STRIPES: only where a game pixel is an exact 3+ screen pixels wide
    * (GB/GBC Stretch), and there every screen column is exactly one stripe,
    * R, G, B, R, G, B ... in step with the game pixels across the whole
    * width -- the GE sampling the texel at each column centre through
    * game_draw's strips (grid_sample's model). */
   {
      static const struct { int scale; unsigned w, h; int on; } c[] = {
         { VID_SCALE_STRETCH, 160, 144, 1 },
         { VID_SCALE_FIT,     160, 144, 0 },
         { VID_SCALE_INT2,    160, 144, 0 },
         { VID_SCALE_1X,      160, 144, 0 },
         { VID_SCALE_STRETCH, 240, 160, 0 },     /* GBA: 2x at most */
         { VID_SCALE_FIT,     240, 160, 0 },
         { VID_SCALE_INT2,    240, 160, 0 },
         { VID_SCALE_1X,      240, 160, 0 },
      };
      unsigned i;
      int v;
      for (v = VID_FX_RGB; v <= VID_FX_RGB_VIVID; v++)
         for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
         {
            vid_geo g;
            int filt = -1, X, y;
            const unsigned int *t;
            vid_set_look(c[i].scale, 0, v);
            dest_geo(c[i].w, c[i].h, &g);
            t = grid_tex(v, c[i].w, &g, &filt);
            assert((t != NULL) == c[i].on);
            assert(vid_fx_state(v, c[i].scale, c[i].w, c[i].h) ==
                   (c[i].on ? VID_FXS_ON : VID_FXS_NEED_3X));
            if (!t)
               continue;
            for (X = 0; X < g.dw; X++)
            {
               /* Column texel under screen column X (rows: texel 0, above
                * the row line). */
               float prof[GRID_N], pos;
               int s, want = X % 3, tx;
               for (tx = 0; tx < GRID_N; tx++)
                  prof[tx] = (float)tx;
               tx = (int)grid_sample(prof, GU_NEAREST, (int)c[i].w, g.dw, X,
                                     &pos);
               s = rgb_stripe(tx, g.dw / (int)c[i].w);
               assert(s == want);
               {
                  unsigned int px = t[tx];
                  unsigned int ch = (px >> (8 * s)) & 0xFFu;
                  assert(ch > 0);                       /* its own tint */
                  assert(((px >> (8 * ((s + 1) % 3))) & 0xFFu) == 0);
                  assert(((px >> (8 * ((s + 2) % 3))) & 0xFFu) == 0);
                  assert((px >> 24) >= (v == VID_FX_RGB ? 20u : 48u) &&
                         (px >> 24) <= (v == VID_FX_RGB ? 48u : 56u));
               }
            }
            /* The bottom texel row carries the row line: darker, more
             * opaque than the tint (the row profile is soft at 1.89x). */
            for (y = 0; y < GRID_N; y++)
               assert((t[y * GRID_N] >> 24) >= (t[(GRID_N / 2) * GRID_N] >> 24));
            assert((t[(GRID_N - 1) * GRID_N] >> 24) >
                   (t[(GRID_N / 2) * GRID_N] >> 24));
         }
   }
   puts("PASS RGB stripes: GB/GBC Stretch only, one stripe per screen "
        "column in step with every game pixel");
   return 0;
}
