/* fe_png_stream.c -- fe_png_stream (fe_png.h): a 24-bit RGB PNG with no
 * work memory, for the "As displayed" screenshots.
 *
 * The same container, zlib stream and dynamic-Huffman blocks as
 * fe_png_encode (the blocks are fe_png.c's own, fe_png_priv.h), with every
 * buffer on the stack: two source rows as RGB, two filtered rows, one
 * block's tokens and a small IDAT buffer -- 15 KiB, plus up to 9 KiB in the
 * block coder while it runs (psp-gcc -fstack-usage) -- whatever the picture's
 * height.  Rows are read once each, top to bottom, straight from the
 * caller's pixels (VRAM on the PSP).
 *
 * What it gives up for that: the hash chains.  A match is looked for at
 * three distances only -- 1 (a run of one byte: a flat area after its
 * filter), 3 (the pixel to the left) and one filtered row back (a row the
 * scaler repeated) -- which is where a scaled console frame repeats itself.
 * Rows take libpng's filter heuristic, fe_png_encode's RGB filter.
 *
 * Size-optimised: it runs once per screenshot, and its bytes sit in front
 * of the FE pad of the pinned layout. */
#include <string.h>

#include "fe_png.h"
#include "fe_png_priv.h"

#define S_SMALL    __attribute__((optimize("Os")))
#define S_ROW_MAX  (FE_PNG_STREAM_MAX_W * 3u + 1u)   /* filter byte + RGB */
#define S_TOK      2048u                             /* >= S_ROW_MAX      */
#define S_IDAT     1024u

typedef char s_tok_fits[S_TOK >= S_ROW_MAX ? 1 : -1];

S_SMALL static void s_be32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Whole bytes into the IDAT stream (the bit buffer is empty). */
S_SMALL static void s_bytes(fe_png_out *o, const uint8_t *p, unsigned n)
{
   while (n--)
      fe_png__bits(o, *p++, 8);
}

S_SMALL static void s_adler(fe_png_out *o, const uint8_t *p, size_t n)
{
   uint32_t a = o->adler_a, b = o->adler_b;
   while (n--)
   {
      a += *p++;
      if (a >= 65521u)
         a -= 65521u;
      b += a;
      if (b >= 65521u)
         b -= 65521u;
   }
   o->adler_a = a;
   o->adler_b = b;
}

/* Length of the match at cur[p] against the bytes `d` back, at most maxl.
 * Bytes before the row are the previous filtered row's (prev, rowlen long),
 * which the stream sent just before this one. */
S_SMALL static unsigned s_match(const uint8_t *cur, const uint8_t *prev,
                                unsigned rowlen, unsigned p, unsigned d,
                                unsigned maxl)
{
   unsigned k;
   for (k = 0; k < maxl; k++)
   {
      unsigned q = p + k;
      uint8_t b = q >= d ? cur[q - d] : prev[rowlen + q - d];
      if (cur[q] != b)
         break;
   }
   return k;
}

S_SMALL int fe_png_stream(const uint16_t *pix, unsigned w, unsigned h,
                          size_t pitch, int layout, fe_png_write_fn write,
                          void *ctx, fe_png_info *info)
{
   static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
   static const uint8_t zhdr[2] = { 0x78, 0x01 };
   static const unsigned char dsmall[2] = { 1, 3 };
   uint8_t rgb[2][S_ROW_MAX - 1u];   /* this row and the one above, RGB  */
   uint8_t flt[2][S_ROW_MAX];        /* the same, filtered               */
   uint32_t tok[S_TOK];
   uint8_t idat[S_IDAT], ihdr[13], ad[4];
   unsigned rowlen, nt = 0, x, y;
   size_t stride;
   fe_png_out o;

   if (!pix || !w || !h || w > FE_PNG_STREAM_MAX_W || h > 4096 || !write ||
       pitch < (size_t)w * 2u)
      return -1;
   stride = (size_t)w * 3u;
   rowlen = (unsigned)stride + 1u;
   memset(rgb[1], 0, stride);        /* the row above the first */

   memset(&o, 0, sizeof(o));
   o.write = write;
   o.ctx = ctx;
   o.idat = idat;
   o.idat_cap = S_IDAT;
   o.adler_a = 1;
   o.adler_b = 0;

   if (write(ctx, sig, 8) != 0)
      o.err = 1;
   o.total = 8;
   s_be32(ihdr, w);
   s_be32(ihdr + 4, h);
   ihdr[8]  = 8;
   ihdr[9]  = 2;                     /* truecolour */
   ihdr[10] = 0;
   ihdr[11] = 0;
   ihdr[12] = 0;
   fe_png__chunk(&o, "IHDR", ihdr, 13);
   s_bytes(&o, zhdr, 2);

   for (y = 0; y < h; y++)
   {
      const uint16_t *r =
         (const uint16_t *)(const void *)((const uint8_t *)pix + pitch * y);
      uint8_t *cur = rgb[y & 1u], *up = rgb[(y & 1u) ^ 1u];
      uint8_t *fc = flt[y & 1u], *fp = flt[(y & 1u) ^ 1u];
      unsigned p = 0;

      for (x = 0; x < w; x++)
      {
         unsigned c = r[x], cr, cg, cb;
         uint8_t *d = cur + x * 3u;
         if (layout == FE_PNG_PSP5650)
         {
            cr = c & 0x1Fu; cg = (c >> 5) & 0x3Fu; cb = (c >> 11) & 0x1Fu;
         }
         else
         {
            cr = (c >> 11) & 0x1Fu; cg = (c >> 5) & 0x3Fu; cb = c & 0x1Fu;
         }
         d[0] = (uint8_t)((cr << 3) | (cr >> 2));
         d[1] = (uint8_t)((cg << 2) | (cg >> 4));
         d[2] = (uint8_t)((cb << 3) | (cb >> 2));
      }
      fe_png__filter_row(cur, up, stride, fc);
      s_adler(&o, fc, rowlen);
      if (nt + rowlen > S_TOK)
      {
         fe_png__block(&o, tok, nt, 0);
         nt = 0;
      }
      while (p < rowlen)
      {
         unsigned maxl = rowlen - p < FE_PNG_MAXM ? rowlen - p : FE_PNG_MAXM;
         unsigned best = 0, bd = 0, l, i;
         if (y > 0)
         {
            best = s_match(fc, fp, rowlen, p, rowlen, maxl);
            bd = rowlen;
         }
         for (i = 0; i < 2; i++)
         {
            unsigned d = dsmall[i];
            if (y == 0 && d > p)
               continue;
            l = s_match(fc, fp, rowlen, p, d, maxl);
            if (l > best || (l == best && d < bd))   /* ties: the cheaper */
            {
               best = l;
               bd = d;
            }
         }
         if (best >= 3)
         {
            tok[nt++] = FE_PNG_TOK_MATCH | ((uint32_t)(best - 3u) << 16) | bd;
            p += best;
         }
         else
            tok[nt++] = fc[p++];
      }
   }
   if (nt)
      fe_png__block(&o, tok, nt, 1);
   else
   {
      fe_png__bits(&o, 1u, 1);       /* an empty final fixed block */
      fe_png__bits(&o, 1u, 2);
      fe_png__bits(&o, 0u, 7);
   }
   if (o.bitcnt)
      fe_png__bits(&o, 0u, 8u - o.bitcnt);
   s_be32(ad, (o.adler_b << 16) | o.adler_a);
   s_bytes(&o, ad, 4);
   if (o.idat_n)
      fe_png__chunk(&o, "IDAT", o.idat, o.idat_n);
   fe_png__chunk(&o, "IEND", NULL, 0);

   if (info)
   {
      info->colors = 0;
      info->bit_depth = 8;
      info->indexed = 0;
      info->bytes = o.total;
   }
   return o.err ? -2 : 0;
}
