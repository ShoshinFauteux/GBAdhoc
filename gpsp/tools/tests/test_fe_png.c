/* test_fe_png.c -- frontend-common/fe_png.c (screenshots, issue #13).
 *
 *   test_fe_png OUTDIR
 *
 * Encodes a set of frames (Game Boy shades, a GBA-like scene, >256 colours,
 * noise, one colour, odd sizes, a wide pitch, both pixel layouts) and writes
 * OUTDIR/<name>.png next to OUTDIR/<name>.rgb, the 8-bit RGB the PNG must
 * decode to.  tools/tests/check_png.py decodes every pair with Python's zlib
 * (an independent inflater) and compares each pixel; this program checks the
 * encoder's own contract: no write past the work block, refusals, sizes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fe_png.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int fwrite_cb(void *ctx, const void *d, size_t n)
{
   return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

static int fail_cb(void *ctx, const void *d, size_t n)
{
   (void)ctx; (void)d; (void)n;
   return -1;
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
   rng = rng * 1103515245u + 12345u;
   return rng >> 8;
}

static void expect_rgb(uint16_t c, int layout, unsigned char *p)
{
   unsigned r, g, b;
   if (layout == FE_PNG_PSP5650) { r = c & 31; g = (c >> 5) & 63; b = c >> 11; }
   else { r = c >> 11; g = (c >> 5) & 63; b = c & 31; }
   p[0] = (unsigned char)((r << 3) | (r >> 2));
   p[1] = (unsigned char)((g << 2) | (g >> 4));
   p[2] = (unsigned char)((b << 3) | (b >> 2));
}

static void one(const char *dir, const char *name, const uint16_t *pix,
                unsigned w, unsigned h, size_t pitch_px, int layout)
{
   char path[512];
   size_t need = fe_png_work_bytes(w, h);
   unsigned char *work = malloc(need + 64), *rgb;
   fe_png_info info;
   FILE *f;
   unsigned x, y;
   int rc;
   clock_t t0;

   memset(work, 0xA5, need + 64);
   snprintf(path, sizeof(path), "%s/%s.png", dir, name);
   f = fopen(path, "wb");
   CHECK(f != NULL);
   if (!f)
      return;
   t0 = clock();
   rc = fe_png_encode(pix, w, h, pitch_px * 2, layout, work, need, fwrite_cb,
                      f, &info);
   fclose(f);
   CHECK(rc == 0);
   for (x = 0; x < 64; x++)
      CHECK(work[need + x] == 0xA5);       /* nothing past the work block */
   printf("  %-14s %3ux%-3u %s depth %u colours %3u -> %6u bytes  %.2f ms\n",
          name, w, h, info.indexed ? "indexed" : "rgb    ", info.bit_depth,
          info.colors, (unsigned)info.bytes,
          (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
   rgb = malloc((size_t)w * h * 3);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         expect_rgb(pix[y * pitch_px + x], layout, rgb + (y * w + x) * 3);
   snprintf(path, sizeof(path), "%s/%s.rgb", dir, name);
   f = fopen(path, "wb");
   fprintf(f, "%u %u\n", w, h);
   fwrite(rgb, 1, (size_t)w * h * 3, f);
   fclose(f);
   free(rgb);
   free(work);
}

int main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   static uint16_t pix[512 * 256];
   static unsigned char work[300000] __attribute__((aligned(8)));
   unsigned x, y;

   /* refusals */
   CHECK(fe_png_encode(pix, 240, 160, 480, 0, work, 1000, fwrite_cb, stdout,
                       NULL) == -1);
   CHECK(fe_png_encode(pix, 240, 160, 480, 0, work + 1, sizeof(work) - 1,
                       fwrite_cb, stdout, NULL) == -1);
   CHECK(fe_png_encode(NULL, 240, 160, 480, 0, work, sizeof(work), fwrite_cb,
                       stdout, NULL) == -1);
   memset(pix, 0, sizeof(pix));
   CHECK(fe_png_encode(pix, 240, 160, 480, 0, work, sizeof(work), fail_cb,
                       NULL, NULL) == -2);
   CHECK(fe_png_work_bytes(240, 160) <= sizeof(work));

   /* Game Boy: four shades in 8x8 tiles, a few sprites */
   {
      static const uint16_t sh[4] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000 };
      for (y = 0; y < 144; y++)
         for (x = 0; x < 160; x++)
            pix[y * 160 + x] = sh[((x / 8 + y / 8) * 7 + ((x ^ y) & 3)) % 4 *
                                  ((x / 8 + y / 8) % 3 == 0)];
      one(dir, "gb_shades", pix, 160, 144, 160, FE_PNG_RGB565);
   }
   /* GBA-like: tiles from a 200-colour palette, a sky gradient */
   {
      uint16_t pal[200];
      for (x = 0; x < 200; x++)
         pal[x] = (uint16_t)(rnd() & 0xFFFF);
      for (y = 0; y < 160; y++)
         for (x = 0; x < 240; x++)
            pix[y * 240 + x] = y < 40 ? pal[y] :
               pal[40 + (((x / 8) * 31 + (y / 8) * 17 + (x & 7) * (y & 7)) % 160)];
      one(dir, "gba_scene", pix, 240, 160, 240, FE_PNG_PSP5650);
      one(dir, "gba_scene565", pix, 240, 160, 240, FE_PNG_RGB565);
   }
   /* more than 256 colours: a smooth gradient (RGB path, filters) */
   for (y = 0; y < 160; y++)
      for (x = 0; x < 240; x++)
         pix[y * 240 + x] = (uint16_t)(((x * 31 / 239) << 11) |
                                       (((y * 63) / 159) << 5) | ((x + y) & 31));
   one(dir, "gradient", pix, 240, 160, 240, FE_PNG_RGB565);
   /* noise: the worst case for the matcher */
   for (y = 0; y < 160; y++)
      for (x = 0; x < 240; x++)
         pix[y * 240 + x] = (uint16_t)rnd();
   one(dir, "noise", pix, 240, 160, 240, FE_PNG_PSP5650);
   /* one colour: long matches, 1-bit */
   for (x = 0; x < 240 * 160; x++)
      pix[x] = 0x1234;
   one(dir, "flat", pix, 240, 160, 240, FE_PNG_RGB565);
   /* odd sizes: the packed row's tail, three colours at 2 bits */
   for (y = 0; y < 5; y++)
      for (x = 0; x < 7; x++)
         pix[y * 7 + x] = (uint16_t)((x + y) % 3 * 0x2108);
   one(dir, "odd7x5", pix, 7, 5, 7, FE_PNG_RGB565);
   for (y = 0; y < 3; y++)
      for (x = 0; x < 13; x++)
         pix[y * 13 + x] = (uint16_t)((x * 5 + y * 3) % 11 * 0x1111);
   one(dir, "odd13x3_4bit", pix, 13, 3, 13, FE_PNG_RGB565);
   /* a 256-pixel pitch (the PSP staging buffers) with 240 used */
   for (y = 0; y < 160; y++)
      for (x = 0; x < 256; x++)
         pix[y * 256 + x] = x < 240 ? (uint16_t)((x / 16) * 0x0841 + y / 40)
                                    : 0xF800;   /* never in the picture */
   one(dir, "pitch256", pix, 240, 160, 256, FE_PNG_PSP5650);
   /* exactly 256 colours, and 257 */
   for (x = 0; x < 240 * 160; x++)
      pix[x] = (uint16_t)((x * 7) % 256 * 3);
   one(dir, "c256", pix, 240, 160, 240, FE_PNG_RGB565);
   pix[240 * 160 - 1] = 0xFFFF;
   one(dir, "c257", pix, 240, 160, 240, FE_PNG_RGB565);
   /* a long frame: several DEFLATE blocks and IDAT chunks of noise rows
    * between flat rows */
   for (y = 0; y < 256; y++)
      for (x = 0; x < 512; x++)
         pix[y * 512 + x] = (y % 7 == 0) ? (uint16_t)rnd() : (uint16_t)(y * 64);
   one(dir, "big512", pix, 400, 256, 512, FE_PNG_RGB565);

   if (fails)
   {
      fprintf(stderr, "fe_png: %d FAILED\n", fails);
      return 1;
   }
   printf("fe_png: encoder checks passed\n");
   return 0;
}
