/* me_copy.cc -- the engine's input copy (me_render_glue.cc, phase 1), one
 * 64-byte cache line at a time.
 *
 * A plain memcpy of the dirty VRAM pages, OAM and palette is expanded by GCC
 * inline as an UNALIGNED loop (lwl/lwr + swl/swr, 16 bytes a pass): it cannot
 * see that the casted pointers are word aligned.  Here a pass moves a whole
 * line: 16 loads, then 16 stores.  Each 64-byte-aligned destination line is
 * first claimed with cache op 0x18 (Create Dirty Exclusive), so it is
 * allocated in the D-cache without being fetched from RAM only to be
 * overwritten.  Measured on both consoles (builds/rush-e2-out): 13 -> 8 us per
 * 1 KiB page, every copied word re-read from RAM and compared equal.
 *
 * Linked after video.o (Makefile), so the renderer moves as little as
 * possible on the engine's instruction cache.
 *
 * ME rules (main.c): no syscalls, cache ops by instruction only. */
extern "C" {
#include "common.h"
}

static inline void me_copy_line(unsigned int *d, const unsigned int *s)
{
   unsigned int a0 = s[0],  a1 = s[1],  a2 = s[2],  a3 = s[3];
   unsigned int a4 = s[4],  a5 = s[5],  a6 = s[6],  a7 = s[7];
   unsigned int a8 = s[8],  a9 = s[9],  aa = s[10], ab = s[11];
   unsigned int ac = s[12], ad = s[13], ae = s[14], af = s[15];
   d[0]  = a0; d[1]  = a1; d[2]  = a2; d[3]  = a3;
   d[4]  = a4; d[5]  = a5; d[6]  = a6; d[7]  = a7;
   d[8]  = a8; d[9]  = a9; d[10] = aa; d[11] = ab;
   d[12] = ac; d[13] = ad; d[14] = ae; d[15] = af;
}

/* Copy `nbytes` (a multiple of 64) from `src` to `dst`.  A pointer that is not
 * word aligned falls back to memcpy; the cache op is used only on a
 * 64-byte-aligned destination (the work area always is; OAM and the palette
 * are module .bss and get the line copy alone). */
extern "C" void me_copy_span(void *dst, const void *src, unsigned int nbytes)
{
   unsigned int *d = (unsigned int *)dst;
   const unsigned int *s = (const unsigned int *)src;
   unsigned int n = nbytes >> 6;
   if (((unsigned int)dst | (unsigned int)src) & 3u)
   {
      memcpy(dst, src, nbytes);
      return;
   }
   if (!((unsigned int)dst & 63u))
   {
      while (n--)
      {
#ifdef __mips__   /* the desktop module twin (tools/e2e) has no such cache */
         __asm__ volatile("cache 0x18, 0(%0)" : : "r"(d) : "memory");
#endif
         me_copy_line(d, s);
         d += 16;
         s += 16;
      }
   }
   else
   {
      while (n--)
      {
         me_copy_line(d, s);
         d += 16;
         s += 16;
      }
   }
}
