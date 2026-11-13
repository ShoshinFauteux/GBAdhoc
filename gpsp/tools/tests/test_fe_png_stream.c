/* test_fe_png_stream.c -- frontend-common/fe_png.c's fe_png_stream, the
 * zero-heap PNG writer of the "As displayed" screenshots.
 *
 *   test_fe_png_stream OUTDIR
 *
 * Writes OUTDIR/<name>.png next to OUTDIR/<name>.rgb (the 8-bit RGB the PNG
 * must decode to) for both sizes a screenshot has -- the 480x272 screen and
 * the 240x160 / 160x144 game picture -- and a few edge shapes; then
 * tools/tests/check_png.py decodes each with Python's zlib (every chunk CRC,
 * the Adler-32, every pixel in row order) and Pillow when present.
 *
 * This program checks the contract itself:
 *   - every source row is read in place: each frame is allocated exactly
 *     h * pitch bytes, so ASan stops a read past the last row or pixel;
 *   - ROW ORDER: every row of "rows" carries its own index, so a row out of
 *     order or repeated fails check_png's pixel compare;
 *   - refusals (too wide, a NULL frame, a short pitch) and a failing writer;
 *   - the PNG compresses a 2x-scaled frame (rows repeated) well;
 *   - NO HEAP: built with -DFE_PNG_STREAM_HEAPCHECK and
 *     -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc (no ASan), every
 *     allocation made during fe_png_stream is counted and must be zero. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fe_png.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#ifdef FE_PNG_STREAM_HEAPCHECK
static int g_in_stream, g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n)
{
   g_allocs += g_in_stream;
   return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b)
{
   g_allocs += g_in_stream;
   return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n)
{
   g_allocs += g_in_stream;
   return __real_realloc(p, n);
}
#define IN_STREAM(v) (g_in_stream = (v))
#else
#define IN_STREAM(v) ((void)0)
#endif

/* The output is gathered in a buffer the test owns, then written. */
typedef struct { unsigned char *p; size_t n, cap; } sink_t;
static int sink_cb(void *ctx, const void *d, size_t n)
{
   sink_t *s = (sink_t *)ctx;
   if (s->n + n > s->cap)
      return -1;
   memcpy(s->p + s->n, d, n);
   s->n += n;
   return 0;
}
static int fail_cb(void *ctx, const void *d, size_t n)
{
   (void)ctx; (void)d; (void)n;
   return -1;
}

static uint32_t rng = 777;
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

/* A frame of exactly h * pitch_px pixels on the heap (ASan's bounds). */
static uint16_t *frame(unsigned h, unsigned pitch_px)
{
   return (uint16_t *)malloc((size_t)h * pitch_px * sizeof(uint16_t));
}

/* Encode, write NAME.png and NAME.rgb; returns the PNG's size. */
static size_t one(const char *dir, const char *name, const uint16_t *pix,
                  unsigned w, unsigned h, unsigned pitch_px, int layout)
{
   char path[512];
   sink_t s;
   fe_png_info info;
   unsigned char *rgb;
   unsigned x, y;
   FILE *f;
   int rc;
   clock_t t0;

   s.cap = (size_t)w * h * 4 + 4096;
   s.p = (unsigned char *)malloc(s.cap);
   s.n = 0;
   memset(&info, 0xA5, sizeof(info));
   t0 = clock();
   IN_STREAM(1);
   rc = fe_png_stream(pix, w, h, (size_t)pitch_px * 2, layout, sink_cb, &s,
                      &info);
   IN_STREAM(0);
   CHECK(rc == 0);
   CHECK(info.bytes == s.n);
   CHECK(info.bit_depth == 8 && !info.indexed && info.colors == 0);
   printf("  %-16s %3ux%-3u -> %7u bytes (raw %7u)  %.2f ms\n", name, w, h,
          (unsigned)s.n, w * h * 3 + h,
          (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
   snprintf(path, sizeof(path), "%s/%s.png", dir, name);
   f = fopen(path, "wb");
   CHECK(f != NULL);
   if (f)
   {
      fwrite(s.p, 1, s.n, f);
      fclose(f);
   }
   rgb = (unsigned char *)malloc((size_t)w * h * 3);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         expect_rgb(pix[(size_t)y * pitch_px + x], layout,
                    rgb + ((size_t)y * w + x) * 3);
   snprintf(path, sizeof(path), "%s/%s.rgb", dir, name);
   f = fopen(path, "wb");
   if (f)
   {
      fprintf(f, "%u %u\n", w, h);
      fwrite(rgb, 1, (size_t)w * h * 3, f);
      fclose(f);
   }
   free(rgb);
   free(s.p);
   return s.n;
}

/* A GBA-like 240x160 scene from a palette of `ncol` colours. */
static void scene(uint16_t *p, unsigned pitch_px, unsigned ncol)
{
   uint16_t pal[256];
   unsigned x, y;
   for (x = 0; x < 256; x++)
      pal[x] = (uint16_t)rnd();
   for (y = 0; y < 160; y++)
      for (x = 0; x < 240; x++)
         p[y * pitch_px + x] = y < 40 ? pal[y % ncol] :
            pal[(((x / 8) * 31 + (y / 8) * 17 + (x & 7) * (y & 7)) % ncol)];
}

int main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   static uint16_t small[240 * 160];
   uint16_t *f;
   unsigned x, y;
   size_t n;
   sink_t s;
   unsigned char buf[64];

   /* refusals: nothing written */
   s.p = buf; s.cap = sizeof(buf); s.n = 0;
   f = frame(272, 512);
   memset(f, 0, (size_t)272 * 512 * 2);
   CHECK(fe_png_stream(f, 481, 2, 1024, 0, sink_cb, &s, NULL) == -1);
   CHECK(fe_png_stream(NULL, 480, 272, 1024, 0, sink_cb, &s, NULL) == -1);
   CHECK(fe_png_stream(f, 480, 272, 958, 0, sink_cb, &s, NULL) == -1);
   CHECK(fe_png_stream(f, 0, 272, 1024, 0, sink_cb, &s, NULL) == -1);
   CHECK(fe_png_stream(f, 480, 272, 1024, 0, NULL, &s, NULL) == -1);
   CHECK(s.n == 0);
   CHECK(fe_png_stream(f, 480, 272, 1024, 0, fail_cb, NULL, NULL) == -2);
   free(f);

   /* THE SCREEN, as the GE leaves it: 480 wide in a 512-pixel VRAM row,
    * PSP 5650.  A 240x160 scene at 2x nearest (each source pixel 2x2) in
    * the middle of black-and-glow bars, so rows repeat. */
   scene(small, 240, 200);
   f = frame(272, 512);
   for (y = 0; y < 272; y++)
      for (x = 0; x < 512; x++)
      {
         uint16_t c;
         if (x >= 480)
            c = 0xDEAD;                    /* past the picture: never read */
         else if (y < 16 || y >= 256)
            c = 0;
         else
            c = small[((y - 16) / 2 % 160) * 240 + (x / 2) % 240];
         if (x < 480 && (y < 16 || y >= 256))
            c = (uint16_t)(((y * 31 / 271) & 31) | ((x / 60) << 5));
         f[y * 512 + x] = c;
      }
   n = one(dir, "screen_2x", f, 480, 272, 512, FE_PNG_PSP5650);
   CHECK(n < (size_t)480 * 272 * 3 / 4);  /* the repeats are found */
   free(f);

   /* ROW ORDER: each row is its own colour pattern, keyed on y */
   f = frame(272, 480);
   for (y = 0; y < 272; y++)
      for (x = 0; x < 480; x++)
         f[y * 480 + x] = (uint16_t)((y << 7) ^ (x * 13) ^ (y * 0x2F));
   one(dir, "rows", f, 480, 272, 480, FE_PNG_PSP5650);
   free(f);

   /* a bilinear-looking frame: smooth everywhere (the filters' case) */
   f = frame(272, 512);
   for (y = 0; y < 272; y++)
      for (x = 0; x < 480; x++)
         f[y * 512 + x] = (uint16_t)(((x * 31 / 479) & 31) |
                                     (((y * 63 / 271) & 63) << 5) |
                                     ((((x + y) / 24) & 31) << 11));
   one(dir, "screen_smooth", f, 480, 272, 512, FE_PNG_PSP5650);
   free(f);

   /* noise: nothing to find */
   f = frame(272, 480);
   for (y = 0; y < 272 * 480; y++)
      f[y] = (uint16_t)rnd();
   one(dir, "screen_noise", f, 480, 272, 480, FE_PNG_RGB565);
   free(f);

   /* one colour */
   f = frame(272, 480);
   for (y = 0; y < 272 * 480; y++)
      f[y] = 0x7BEF;
   n = one(dir, "screen_flat", f, 480, 272, 480, FE_PNG_PSP5650);
   CHECK(n < 4096);
   free(f);

   /* THE GAME PICTURE sizes, both layouts */
   f = frame(160, 240);
   scene(f, 240, 256);
   one(dir, "game_240x160", f, 240, 160, 240, FE_PNG_PSP5650);
   one(dir, "game_240x160_565", f, 240, 160, 240, FE_PNG_RGB565);
   free(f);
   f = frame(144, 256);
   for (y = 0; y < 144; y++)
      for (x = 0; x < 256; x++)
         f[y * 256 + x] = (uint16_t)((((x / 8) + (y / 8)) & 3) * 0x5294u);
   one(dir, "game_160x144", f, 160, 144, 256, FE_PNG_PSP5650);
   free(f);

   /* edges: one pixel; three rows of the widest; a tall thin strip; more
    * rows than one block of tokens holds */
   f = frame(1, 1);
   f[0] = 0xF81F;
   one(dir, "edge_1x1", f, 1, 1, 1, FE_PNG_RGB565);
   free(f);
   f = frame(3, 480);
   for (y = 0; y < 3 * 480; y++)
      f[y] = (uint16_t)(y * 7);
   one(dir, "edge_480x3", f, 480, 3, 480, FE_PNG_PSP5650);
   free(f);
   f = frame(600, 2);
   for (y = 0; y < 600 * 2; y++)
      f[y] = (uint16_t)(y / 6);
   one(dir, "edge_2x600", f, 2, 600, 2, FE_PNG_PSP5650);
   free(f);

#ifdef FE_PNG_STREAM_HEAPCHECK
   printf("heap: %d allocations inside fe_png_stream\n", g_allocs);
   CHECK(g_allocs == 0);
#endif
   printf("test_fe_png_stream: %s\n", fails ? "FAILED" : "ok");
   return fails ? 1 : 0;
}
