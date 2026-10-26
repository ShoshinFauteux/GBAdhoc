/* fe_png.c -- see fe_png.h. */
#include <string.h>

#include "fe_png.h"
#include "fe_util.h"          /* fe_crc32: zlib-compatible, chainable */

/* ---- sizes ---------------------------------------------------------------
 * The match window is 16 KiB, not DEFLATE's 32: a 240-pixel RGB row is 721
 * bytes, so 16 KiB still reaches 22 rows back, and the chain links stay
 * 16-bit.  Tokens are buffered a block at a time (two passes: count, then
 * write with that block's own Huffman codes). */
#define WBITS   14
#define WSIZE   (1u << WBITS)
#define WMASK   (WSIZE - 1u)
#define HBITS   12
#define HSIZE   (1u << HBITS)
#define NTOK    8192u
#define CHAIN   48
#define MAXM    258

static size_t align8(size_t n) { return (n + 7u) & ~(size_t)7u; }

static size_t raw_bytes_max(unsigned w, unsigned h)
{
   return (size_t)(w * 3u + 1u) * h;          /* RGB: the larger layout */
}

size_t fe_png_work_bytes(unsigned w, unsigned h)
{
   return align8(raw_bytes_max(w, h)) +
          align8(HSIZE * sizeof(uint32_t)) +
          align8(WSIZE * sizeof(uint16_t)) +
          align8(NTOK * sizeof(uint32_t)) +
          align8(FE_PNG_IDAT) +
          align8(2u * (w * 3u)) +               /* two RGB rows (filters) */
          align8(1024u * sizeof(uint32_t));     /* the colour table */
}

/* ---- output: PNG chunks, the IDAT stream, bits --------------------------- */
typedef struct
{
   fe_png_write_fn write;
   void    *ctx;
   int      err;
   size_t   total;
   uint8_t *idat;            /* FE_PNG_IDAT bytes being gathered          */
   size_t   idat_n;
   uint32_t bitbuf;
   unsigned bitcnt;
   uint32_t adler_a, adler_b;
} out_t;

static void put_be32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void out_raw(out_t *o, const void *d, size_t n)
{
   if (o->err || !n)
      return;
   if (o->write(o->ctx, d, n) != 0)
      o->err = 1;
   o->total += n;
}

static void chunk(out_t *o, const char *type, const uint8_t *data, size_t n)
{
   uint8_t hd[8], crc[4];
   uint32_t c;
   put_be32(hd, (uint32_t)n);
   memcpy(hd + 4, type, 4);
   c = fe_crc32(0, hd + 4, 4);
   c = fe_crc32(c, data, n);
   put_be32(crc, c);
   out_raw(o, hd, 8);
   out_raw(o, data, n);
   out_raw(o, crc, 4);
}

static void idat_byte(out_t *o, uint8_t b)
{
   o->idat[o->idat_n++] = b;
   if (o->idat_n == FE_PNG_IDAT)
   {
      chunk(o, "IDAT", o->idat, o->idat_n);
      o->idat_n = 0;
   }
}

/* n <= 16 bits, LSB first (DEFLATE's bit order). */
static void bits(out_t *o, uint32_t v, unsigned n)
{
   o->bitbuf |= v << o->bitcnt;
   o->bitcnt += n;
   while (o->bitcnt >= 8)
   {
      idat_byte(o, (uint8_t)o->bitbuf);
      o->bitbuf >>= 8;
      o->bitcnt -= 8;
   }
}

static void bits_flush(out_t *o)
{
   if (o->bitcnt)
      idat_byte(o, (uint8_t)o->bitbuf);
   o->bitbuf = 0;
   o->bitcnt = 0;
}

static void adler(out_t *o, const uint8_t *p, size_t n)
{
   uint32_t a = o->adler_a, b = o->adler_b;
   while (n)
   {
      size_t k = n < 3800 ? n : 3800;     /* < 5552: no overflow before mod */
      n -= k;
      while (k--)
      {
         a += *p++;
         b += a;
      }
      a %= 65521u;
      b %= 65521u;
   }
   o->adler_a = a;
   o->adler_b = b;
}

/* ---- Huffman ------------------------------------------------------------- */

/* Code lengths for freq[0..n) (n <= 288), none longer than `limit`.  Symbols
 * with no uses get 0.  Built with the two-queue method over the leaves
 * sorted by weight; too deep a tree halves the weights and builds again. */
static void huff_lengths(const uint32_t *freq_in, int n, int limit,
                         uint8_t *len)
{
   uint32_t f[288], w[2 * 288];
   uint16_t sym[288], par[2 * 288];
   uint8_t  dep[2 * 288];
   int m, i, j, k, a, b, maxd;

   for (i = 0; i < n; i++)
      f[i] = freq_in[i];
   for (;;)
   {
      memset(len, 0, (size_t)n);
      m = 0;
      for (i = 0; i < n; i++)
         if (f[i])
            sym[m++] = (uint16_t)i;
      if (m == 0)
         return;
      if (m == 1)
      {
         len[sym[0]] = 1;
         return;
      }
      /* insertion sort by weight (stable, n is small) */
      for (i = 1; i < m; i++)
      {
         uint16_t s = sym[i];
         for (j = i; j > 0 && f[sym[j - 1]] > f[s]; j--)
            sym[j] = sym[j - 1];
         sym[j] = s;
      }
      for (i = 0; i < m; i++)
         w[i] = f[sym[i]];
      i = 0;          /* next leaf     */
      j = m;          /* next internal */
      for (k = m; k < 2 * m - 1; k++)
      {
         if (j >= k || (i < m && w[i] <= w[j])) a = i++; else a = j++;
         if (j >= k || (i < m && w[i] <= w[j])) b = i++; else b = j++;
         w[k] = w[a] + w[b];
         par[a] = (uint16_t)k;
         par[b] = (uint16_t)k;
      }
      dep[2 * m - 2] = 0;
      maxd = 0;
      for (k = 2 * m - 3; k >= 0; k--)
      {
         dep[k] = (uint8_t)(dep[par[k]] + 1);
         if (k < m && dep[k] > maxd)
            maxd = dep[k];
      }
      if (maxd <= limit)
      {
         for (i = 0; i < m; i++)
            len[sym[i]] = dep[i];
         return;
      }
      for (i = 0; i < n; i++)
         if (f[i])
            f[i] = (f[i] >> 1) | 1u;
   }
}

/* Canonical codes, bit-reversed for LSB-first output. */
static void huff_codes(const uint8_t *len, int n, uint16_t *code)
{
   uint16_t count[16] = {0}, next[16];
   int i, b;
   uint16_t c = 0;
   for (i = 0; i < n; i++)
      count[len[i]]++;
   count[0] = 0;
   for (b = 1; b < 16; b++)
   {
      c = (uint16_t)((c + count[b - 1]) << 1);
      next[b] = c;
   }
   for (i = 0; i < n; i++)
   {
      unsigned l = len[i], v, r = 0, k;
      if (!l)
      {
         code[i] = 0;
         continue;
      }
      v = next[l]++;
      for (k = 0; k < l; k++)
         r |= ((v >> k) & 1u) << (l - 1 - k);
      code[i] = (uint16_t)r;
   }
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
   35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
   4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,
   193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
   9,9,10,10,11,11,12,12,13,13 };
static const uint8_t  CLORD[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,
   14,1,15 };

static int len_code(unsigned l)       /* 3..258 -> 0..28 */
{
   int c = 28;
   while (LBASE[c] > l)
      c--;
   return c;
}

static int dist_code(unsigned d)      /* 1..32768 -> 0..29 */
{
   int c = 29;
   while (DBASE[c] > d)
      c--;
   return c;
}

#define TOK_MATCH 0x80000000u

/* One dynamic-Huffman block of tok[0..nt). */
static void block(out_t *o, const uint32_t *tok, unsigned nt, int final)
{
   uint32_t lf[286], df[30], cf[19];
   uint8_t  ll[286], dl[30], cl[19], all[286 + 30];
   uint16_t lc[286], dc[30], cc[19];
   uint8_t  rle[286 + 30], rle_x[286 + 30];
   unsigned i, nl, nd, nc, nr = 0, nall;

   memset(lf, 0, sizeof(lf));
   memset(df, 0, sizeof(df));
   memset(cf, 0, sizeof(cf));
   for (i = 0; i < nt; i++)
   {
      uint32_t t = tok[i];
      if (t & TOK_MATCH)
      {
         lf[257 + len_code(((t >> 16) & 0xFFu) + 3u)]++;
         df[dist_code(t & 0xFFFFu)]++;
      }
      else
         lf[t]++;
   }
   lf[256]++;
   /* Every tree gets at least two codes: a one-code tree is legal for
    * distances only, and some decoders dislike even that. */
   {
      unsigned used = 0;
      for (i = 0; i < 30; i++)
         used += df[i] != 0;
      if (used == 0)
         df[0] = df[1] = 1;
      else if (used == 1)
         df[df[0] ? 1 : 0] = 1;
      used = 0;
      for (i = 0; i < 286; i++)
         used += lf[i] != 0;
      if (used < 2)
         lf[0] = lf[0] ? lf[0] : 1;
   }
   huff_lengths(lf, 286, 15, ll);
   huff_lengths(df, 30, 15, dl);
   for (nl = 286; nl > 257 && !ll[nl - 1]; nl--)
      ;
   for (nd = 30; nd > 1 && !dl[nd - 1]; nd--)
      ;
   memcpy(all, ll, nl);
   memcpy(all + nl, dl, nd);
   nall = nl + nd;
   /* run-length the code lengths (16: repeat previous 3-6, 17: zeros 3-10,
    * 18: zeros 11-138) */
   for (i = 0; i < nall;)
   {
      unsigned v = all[i], run = 1;
      while (i + run < nall && all[i + run] == v)
         run++;
      if (v == 0 && run >= 3)
      {
         unsigned r = run;
         while (r >= 3)
         {
            unsigned k = r > 138 ? 138 : r;
            if (k >= 11) { rle[nr] = 18; rle_x[nr++] = (uint8_t)(k - 11); }
            else         { rle[nr] = 17; rle_x[nr++] = (uint8_t)(k - 3); }
            r -= k;
         }
         while (r--) { rle[nr] = 0; rle_x[nr++] = 0; }
      }
      else if (v != 0 && run >= 4)
      {
         unsigned r = run - 1;
         rle[nr] = (uint8_t)v; rle_x[nr++] = 0;
         while (r >= 3)
         {
            unsigned k = r > 6 ? 6 : r;
            rle[nr] = 16; rle_x[nr++] = (uint8_t)(k - 3);
            r -= k;
         }
         while (r--) { rle[nr] = (uint8_t)v; rle_x[nr++] = 0; }
      }
      else
      {
         unsigned r;
         for (r = 0; r < run; r++) { rle[nr] = (uint8_t)v; rle_x[nr++] = 0; }
      }
      i += run;
   }
   for (i = 0; i < nr; i++)
      cf[rle[i]]++;
   {
      unsigned used = 0;
      for (i = 0; i < 19; i++)
         used += cf[i] != 0;
      if (used < 2)
         cf[rle[0] == 0 ? 1 : 0]++;      /* a second, unused code */
   }
   huff_lengths(cf, 19, 7, cl);
   for (nc = 19; nc > 4 && !cl[CLORD[nc - 1]]; nc--)
      ;
   huff_codes(ll, 286, lc);
   huff_codes(dl, 30, dc);
   huff_codes(cl, 19, cc);

   bits(o, final ? 1u : 0u, 1);
   bits(o, 2u, 2);                           /* BTYPE 10: dynamic */
   bits(o, nl - 257u, 5);
   bits(o, nd - 1u, 5);
   bits(o, nc - 4u, 4);
   for (i = 0; i < nc; i++)
      bits(o, cl[CLORD[i]], 3);
   for (i = 0; i < nr; i++)
   {
      unsigned s = rle[i];
      bits(o, cc[s], cl[s]);
      if (s == 16) bits(o, rle_x[i], 2);
      else if (s == 17) bits(o, rle_x[i], 3);
      else if (s == 18) bits(o, rle_x[i], 7);
   }
   for (i = 0; i < nt; i++)
   {
      uint32_t t = tok[i];
      if (t & TOK_MATCH)
      {
         unsigned l = ((t >> 16) & 0xFFu) + 3u, d = t & 0xFFFFu;
         int c = len_code(l), e = dist_code(d);
         bits(o, lc[257 + c], ll[257 + c]);
         if (LEXT[c]) bits(o, l - LBASE[c], LEXT[c]);
         bits(o, dc[e], dl[e]);
         if (DEXT[e]) bits(o, d - DBASE[e], DEXT[e]);
      }
      else
         bits(o, lc[t], ll[t]);
   }
   bits(o, lc[256], ll[256]);
}

/* ---- LZ77 over the filtered image ---------------------------------------- */
static uint32_t hash3(const uint8_t *p)
{
   uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
   return (v * 2654435761u) >> (32 - HBITS);
}

static void deflate_all(out_t *o, const uint8_t *raw, size_t n,
                        uint32_t *head, uint16_t *prev, uint32_t *tok)
{
   size_t pos = 0;
   unsigned nt = 0;
   int closed = 0;
#define INSERT(p) do {                                                     \
      uint32_t h_ = hash3(raw + (p));                                       \
      uint32_t old_ = head[h_];                                             \
      prev[(p) & WMASK] = (uint16_t)(old_ && (p) - (old_ - 1u) < WSIZE      \
                                     ? (p) - (old_ - 1u) : 0u);             \
      head[h_] = (uint32_t)(p) + 1u;                                        \
   } while (0)

   memset(head, 0, HSIZE * sizeof(uint32_t));
   o->bitbuf = 0;
   o->bitcnt = 0;
   while (pos < n)
   {
      unsigned best = 0, bdist = 0;
      if (pos + 3 <= n)
      {
         size_t maxl = n - pos < MAXM ? n - pos : MAXM;
         uint32_t cand = head[hash3(raw + pos)];
         int chain = CHAIN;
         while (cand && chain--)
         {
            size_t c = cand - 1u, d = pos - c, l;
            unsigned step;
            if (d >= WSIZE)
               break;
            if (raw[c + best] == raw[pos + best] && raw[c] == raw[pos])
            {
               for (l = 0; l < maxl && raw[c + l] == raw[pos + l]; l++)
                  ;
               if (l > best)
               {
                  best = (unsigned)l;
                  bdist = (unsigned)d;
                  if (l == maxl)
                     break;
               }
            }
            step = prev[c & WMASK];
            if (!step || step > c)
               break;
            cand = (uint32_t)(c - step) + 1u;
            if (pos - (cand - 1u) >= WSIZE)
               break;
         }
         INSERT(pos);
      }
      if (best >= 3)
      {
         size_t k;
         tok[nt++] = TOK_MATCH | ((uint32_t)(best - 3u) << 16) | bdist;
         for (k = 1; k < best; k++)
            if (pos + k + 3 <= n)
               INSERT(pos + k);
         pos += best;
      }
      else
      {
         tok[nt++] = raw[pos];
         pos++;
      }
      if (nt == NTOK)
      {
         closed = pos >= n;
         block(o, tok, nt, closed);
         nt = 0;
      }
   }
   if (closed)
      ;
   else if (nt || n == 0)
      block(o, tok, nt, 1);
   else
   {
      /* the last block was flushed full: close the stream with an empty
       * fixed block (BFINAL, BTYPE 01, EOB = 7 zero bits) */
      bits(o, 1u, 1);
      bits(o, 1u, 2);
      bits(o, 0u, 7);
   }
#undef INSERT
}

/* ---- the image ----------------------------------------------------------- */
static void to_rgb(uint16_t c, int layout, uint8_t *p)
{
   unsigned r, g, b;
   if (layout == FE_PNG_PSP5650)
   {
      r = c & 0x1Fu; g = (c >> 5) & 0x3Fu; b = (c >> 11) & 0x1Fu;
   }
   else
   {
      r = (c >> 11) & 0x1Fu; g = (c >> 5) & 0x3Fu; b = c & 0x1Fu;
   }
   p[0] = (uint8_t)((r << 3) | (r >> 2));
   p[1] = (uint8_t)((g << 2) | (g >> 4));
   p[2] = (uint8_t)((b << 3) | (b >> 2));
}

static const uint16_t *row_of(const uint16_t *pix, size_t pitch, unsigned y)
{
   return (const uint16_t *)(const void *)((const uint8_t *)pix + pitch * y);
}

/* Colour table: open addressing, 1024 slots; slot = 0x80000000 | idx<<16 |
 * colour.  Returns the number of colours, or 257 for "more than 256". */
static unsigned count_colors(const uint16_t *pix, unsigned w, unsigned h,
                             size_t pitch, uint32_t *tab, uint16_t *pal)
{
   unsigned x, y, n = 0;
   memset(tab, 0, 1024 * sizeof(uint32_t));
   for (y = 0; y < h; y++)
   {
      const uint16_t *r = row_of(pix, pitch, y);
      uint16_t last = 0;
      int have_last = 0;
      for (x = 0; x < w; x++)
      {
         uint16_t c = r[x];
         uint32_t s;
         if (have_last && c == last)
            continue;
         last = c;
         have_last = 1;
         s = ((uint32_t)c * 40503u >> 6) & 1023u;
         while (tab[s] && (tab[s] & 0xFFFFu) != c)
            s = (s + 1) & 1023u;
         if (!tab[s])
         {
            if (n == 256)
               return 257;
            tab[s] = 0x80000000u | ((uint32_t)n << 16) | c;
            pal[n++] = c;
         }
      }
   }
   return n;
}

static unsigned color_index(const uint32_t *tab, uint16_t c)
{
   uint32_t s = ((uint32_t)c * 40503u >> 6) & 1023u;
   while ((tab[s] & 0xFFFFu) != c || !tab[s])
      s = (s + 1) & 1023u;
   return (tab[s] >> 16) & 0xFFu;
}

static uint8_t paeth(int a, int b, int c)
{
   int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p,
       pc = p > c ? p - c : c - p;
   if (pa <= pb && pa <= pc) return (uint8_t)a;
   if (pb <= pc) return (uint8_t)b;
   return (uint8_t)c;
}

int fe_png_encode(const uint16_t *pix, unsigned w, unsigned h, size_t pitch,
                  int layout, void *work, size_t work_bytes,
                  fe_png_write_fn write, void *ctx, fe_png_info *info)
{
   static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
   uint8_t *wp = (uint8_t *)work, *raw, *idat, *rowa, *rowb, ihdr[13];
   uint32_t *head, *tok, *tab;
   uint16_t *prev, pal[256];
   unsigned colors, depth = 8, x, y;
   size_t rawn = 0, stride;
   int indexed;
   out_t o;

   if (!pix || !w || !h || w > 4096 || h > 4096 || !write || !work ||
       ((uintptr_t)work & 7u) || work_bytes < fe_png_work_bytes(w, h))
      return -1;
   raw  = wp;                         wp += align8(raw_bytes_max(w, h));
   head = (uint32_t *)(void *)wp;     wp += align8(HSIZE * sizeof(uint32_t));
   prev = (uint16_t *)(void *)wp;     wp += align8(WSIZE * sizeof(uint16_t));
   tok  = (uint32_t *)(void *)wp;     wp += align8(NTOK * sizeof(uint32_t));
   idat = wp;                         wp += align8(FE_PNG_IDAT);
   rowa = wp;                         rowb = wp + w * 3u;
   wp  += align8(2u * (w * 3u));
   tab  = (uint32_t *)(void *)wp;

   colors = count_colors(pix, w, h, pitch, tab, pal);
   indexed = colors <= 256;
#ifdef FE_PNG_LOW_DEPTH
   /* 1/2/4-bit indices: legal PNG, ~40% smaller for a Game Boy frame, but
    * off by default -- the XMB's Photo viewer has not been seen showing a
    * sub-byte palette image, and 8-bit indexed is the form every decoder
    * meets.  The host test runs both. */
   if (indexed)
      depth = colors <= 2 ? 1 : colors <= 4 ? 2 : colors <= 16 ? 4 : 8;
#endif

   /* the filtered scanlines, whole */
   if (indexed)
   {
      stride = (w * depth + 7u) / 8u;
      for (y = 0; y < h; y++)
      {
         const uint16_t *r = row_of(pix, pitch, y);
         uint8_t *d = raw + rawn;
         unsigned acc = 0, nb = 0;
         size_t k = 1;
         d[0] = 0;                    /* filter None: best for indices */
         for (x = 0; x < w; x++)
         {
            acc = (acc << depth) | color_index(tab, r[x]);
            nb += depth;
            if (nb == 8)
            {
               d[k++] = (uint8_t)acc;
               acc = 0;
               nb = 0;
            }
         }
         if (nb)
            d[k++] = (uint8_t)(acc << (8 - nb));
         rawn += stride + 1u;
      }
   }
   else
   {
      uint8_t *cur = rowa, *up = rowb;
      stride = (size_t)w * 3u;
      memset(up, 0, stride);          /* the row above the first */
      for (y = 0; y < h; y++)
      {
         const uint16_t *r = row_of(pix, pitch, y);
         uint8_t *d = raw + rawn, *sw;
         unsigned long sum[5] = { 0, 0, 0, 0, 0 };
         unsigned best = 0, f;
         size_t i;
         for (x = 0; x < w; x++)
            to_rgb(r[x], layout, cur + x * 3u);
         /* libpng's heuristic: the filter with the smallest sum of
          * absolute (signed) residuals */
         for (i = 0; i < stride; i++)
         {
            int a = i >= 3 ? cur[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
            uint8_t v[5];
            v[0] = cur[i];
            v[1] = (uint8_t)(cur[i] - a);
            v[2] = (uint8_t)(cur[i] - b);
            v[3] = (uint8_t)(cur[i] - ((a + b) >> 1));
            v[4] = (uint8_t)(cur[i] - paeth(a, b, c));
            for (f = 0; f < 5; f++)
               sum[f] += v[f] < 128 ? v[f] : 256u - v[f];
         }
         for (f = 1; f < 5; f++)
            if (sum[f] < sum[best])
               best = f;
         d[0] = (uint8_t)best;
         for (i = 0; i < stride; i++)
         {
            int a = i >= 3 ? cur[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
            uint8_t p = best == 0 ? 0 : best == 1 ? (uint8_t)a :
                        best == 2 ? (uint8_t)b : best == 3 ? (uint8_t)((a + b) >> 1)
                                  : paeth(a, b, c);
            d[1 + i] = (uint8_t)(cur[i] - p);
         }
         rawn += stride + 1u;
         sw = up;
         up = cur;
         cur = sw;
      }
   }

   memset(&o, 0, sizeof(o));
   o.write = write;
   o.ctx = ctx;
   o.idat = idat;
   o.adler_a = 1;
   o.adler_b = 0;

   out_raw(&o, sig, 8);
   put_be32(ihdr, w);
   put_be32(ihdr + 4, h);
   ihdr[8]  = (uint8_t)depth;
   ihdr[9]  = indexed ? 3 : 2;        /* palette / truecolour */
   ihdr[10] = 0;
   ihdr[11] = 0;
   ihdr[12] = 0;
   chunk(&o, "IHDR", ihdr, 13);
   if (indexed)
   {
      uint8_t plte[256 * 3];
      unsigned i;
      for (i = 0; i < colors; i++)
         to_rgb(pal[i], layout, plte + i * 3u);
      chunk(&o, "PLTE", plte, colors * 3u);
   }
   /* zlib stream: header, DEFLATE, Adler-32 of the filtered data */
   idat_byte(&o, 0x78);
   idat_byte(&o, 0xDA);
   adler(&o, raw, rawn);
   deflate_all(&o, raw, rawn, head, prev, tok);
   bits_flush(&o);
   {
      uint8_t ad[4];
      unsigned i;
      put_be32(ad, (o.adler_b << 16) | o.adler_a);
      for (i = 0; i < 4; i++)
         idat_byte(&o, ad[i]);
   }
   if (o.idat_n)
      chunk(&o, "IDAT", o.idat, o.idat_n);
   chunk(&o, "IEND", NULL, 0);

   if (info)
   {
      info->colors = colors;
      info->bit_depth = depth;
      info->indexed = indexed;
      info->bytes = o.total;
   }
   return o.err ? -2 : 0;
}
