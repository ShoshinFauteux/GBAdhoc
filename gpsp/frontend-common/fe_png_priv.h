/* fe_png_priv.h -- what fe_png.c shares with fe_png_stream.c, and nothing
 * else includes.  The streamer is its own object only for where it links:
 * psp/Makefile puts it before layout_pad_fe, beside the other code of the
 * screenshot path, so its size is taken by the FE pad, not the GB pad
 * (docs/LAYOUT-PINNING.md, "Keeping the pads"). */
#ifndef FE_PNG_PRIV_H
#define FE_PNG_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include "fe_png.h"

/* noinline: one copy of each, called from both files. */
#define FE_PNG_SHARED __attribute__((noinline))

#define FE_PNG_TOK_MATCH 0x80000000u   /* | (len - 3) << 16 | dist */
#define FE_PNG_MAXM      258

/* The output: PNG chunks, the IDAT stream, the bit buffer, the Adler-32. */
typedef struct
{
   fe_png_write_fn write;
   void    *ctx;
   int      err;
   size_t   total;
   uint8_t *idat;            /* idat_cap bytes being gathered             */
   size_t   idat_n;
   size_t   idat_cap;        /* FE_PNG_IDAT, or the streamer's smaller one */
   uint32_t bitbuf;
   unsigned bitcnt;
   uint32_t adler_a, adler_b;
} fe_png_out;

/* A chunk: length, type, data, CRC. */
void fe_png__chunk(fe_png_out *o, const char *type, const uint8_t *data,
                   size_t n);
/* n <= 16 bits into the IDAT stream, LSB first. */
void fe_png__bits(fe_png_out *o, uint32_t v, unsigned n);
/* One dynamic-Huffman DEFLATE block of tokens (FE_PNG_TOK_MATCH or a
 * literal byte). */
void fe_png__block(fe_png_out *o, const uint32_t *tok, unsigned nt,
                   int final);

/* One RGB row filtered for PNG into d[0..stride] (d[0] = the filter). */
void fe_png__filter_row(const uint8_t *cur, const uint8_t *up, size_t stride,
                        uint8_t *d);

#endif
