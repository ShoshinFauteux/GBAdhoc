/* fe_png.h -- a small PNG writer for screenshots (GitHub issue #13).
 *
 * Self-contained: the PSP build links no zlib, and a screenshot does not
 * justify one.  The encoder is a dynamic-Huffman DEFLATE with greedy hash
 * chain matching -- what zlib does at a middling level -- plus the one
 * thing that matters most for a console frame: a picture with at most 256
 * colours (nearly every GBA and every GB frame) is written as an INDEXED
 * PNG, at 1, 2, 4 or 8 bits a pixel, so a four-shade Game Boy frame is a
 * few hundred bytes of data before compression even starts.  More colours
 * than that: 24-bit RGB with a per-row adaptive filter.
 *
 * NO HEAP.  The caller hands in all the memory it needs (fe_png_work_bytes:
 * ~100 KiB for an indexed 240x160 frame, ~200 KiB for RGB), because on a
 * PSP-1000 the heap is empty once a ROM is loaded.  Output goes through a
 * callback in IDAT chunks of at most FE_PNG_IDAT bytes (as libpng writes
 * them), so no output buffer is needed either.
 *
 * Pure C99; tools/tests/test_fe_png.c decodes the output with Python's zlib
 * and compares every pixel. */
#ifndef FE_PNG_H
#define FE_PNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_PNG_IDAT 4096

/* Write `n` bytes; 0 on success. */
typedef int (*fe_png_write_fn)(void *ctx, const void *data, size_t n);

/* The source frame's 16-bit layout. */
enum { FE_PNG_RGB565 = 0,    /* libretro: R bits 11-15, B bits 0-4      */
       FE_PNG_PSP5650 = 1 }; /* PSP GE:   R bits 0-4,   B bits 11-15    */

/* Bytes of work memory a w x h frame can need (the RGB worst case). */
size_t fe_png_work_bytes(unsigned w, unsigned h);

typedef struct
{
   unsigned colors;          /* distinct colours in the frame (257 = more) */
   unsigned bit_depth;       /* 1/2/4/8 indexed, 8 for RGB                 */
   int      indexed;
   size_t   bytes;           /* the whole file                            */
} fe_png_info;

/* Encode pix (w x h, pitch in BYTES) as a PNG through write().  `work` is
 * at least fe_png_work_bytes(w, h) bytes, 8-aligned.  0 on success; -1 on a
 * bad argument or too little work memory, -2 if write() failed. */
int fe_png_encode(const uint16_t *pix, unsigned w, unsigned h, size_t pitch,
                  int layout, void *work, size_t work_bytes,
                  fe_png_write_fn write, void *ctx, fe_png_info *info);

#ifdef __cplusplus
}
#endif

#endif
