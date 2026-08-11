/* me_render_glue.cc — run the GBA scanline renderer (video.cc) ON the Media
 * Engine: the LIVE pipeline (mailbox v2).
 *
 * The input set and the 160-line replay loop are the ones proven bit-exact on
 * the desktop twice over: RENDER_REPLAY (~81k frames, full-register log) and
 * ME_CAP_VALIDATE (~61k frames, THIS slim capture struct).  What this file
 * adds is the two-phase hardware protocol:
 *
 *   phase 1 — SNAPSHOT: copy the live core vram/oam/palette into the PRX's
 *             own arrays (cached reads, invalidate first), then publish
 *             input_seq so the host resumes emulating the next frame.
 *   phase 2 — RENDER:   replay the me_capture_frame through update_scanline()
 *             into an ME-local buffer, then write pitched rows to the host's
 *             staging buffer (uncached, so the GE sees them with no ME
 *             writeback), and return a frame checksum.
 *
 * ME rules (psp/me/main.c header): no syscalls, cache ops via
 * __builtin_allegrex_cache only.  The 122 KB of video.o .text vs the ME's
 * 16 KB I-cache measured ~10 ms/frame median on hardware (bench, 198 samples)
 * — viable against the ~16.7 ms frame budget. */
extern "C" {
#include "common.h"          /* update_scanline, gba_screen_pixels,
                              * affine_reference_x/y, reg, OAM_UPDATED,
                              * me_capture_frame (video.h), GBA_SCREEN_PITCH */
}
#include "me_mbox.h"

/* ME-local frame buffer the renderer draws into (PRX BSS, cached-fast). */
static u16 me_out[GBA_SCREEN_PITCH * (160 + 1)];

/* Invalidate a main-RAM span in the ME D-cache before a cached read, so we see
 * the host's freshly written data and never a stale line. */
static void me_inv(unsigned int cached_addr, int nbytes)
{
   int i;
   for (i = 0; i < nbytes; i += 64)
      __builtin_allegrex_cache(0x19, (int)(cached_addr + i));
}

/* ME_CMD_RENDER entry (called from me_dispatch with the command's seq).
 * Returns an additive checksum of the output frame. */
extern "C" unsigned int me_render_run(volatile me_mbox *mb, unsigned int seq)
{
   me_render_desc *d = (me_render_desc *)(mb->arg0 & ~0x40000000u);
   const me_capture_frame *cap;
   unsigned int sum = 0;
   int ln;

   me_inv((unsigned int)d, sizeof(*d));
   if (!d->vram || !d->capture || !d->out || !d->out_pitch)
   {
      mb->input_seq = seq;           /* never leave the host waiting */
      return 0;
   }

   /* ---- phase 1: snapshot the live core arrays ------------------------- */
   me_inv(d->vram    & ~0x40000000u, 1024 * 96);
   memcpy(vram, (const void *)(d->vram & ~0x40000000u), 1024 * 96);
   me_inv(d->oam     & ~0x40000000u, 512 * 2);
   memcpy(oam_ram, (const void *)(d->oam & ~0x40000000u), 512 * 2);
   me_inv(d->palette & ~0x40000000u, 512 * 2);
   memcpy(palette_ram_converted, (const void *)(d->palette & ~0x40000000u),
          512 * 2);
   me_inv(d->capture & ~0x40000000u, sizeof(me_capture_frame));
   mb->input_seq = seq;              /* host may resume emulation NOW */

   /* ---- phase 2: render from the capture ------------------------------- */
   cap = (const me_capture_frame *)(d->capture & ~0x40000000u);
   sprite_limit      = 1;
   skip_next_frame   = 0;
   gba_screen_pixels = me_out;
   affine_reference_x[0] = cap->affine_seed[0];
   affine_reference_x[1] = cap->affine_seed[1];
   affine_reference_y[0] = cap->affine_seed[2];
   affine_reference_y[1] = cap->affine_seed[3];
   reg[OAM_UPDATED] = cap->oam_updated;

   for (ln = 0; ln < 160; ln++)
   {
      memcpy(io_registers, cap->ioregs[ln], ME_CAP_IOREGS * sizeof(u16));
      update_scanline();             /* VCOUNT rides in the captured regs */
   }

   /* Pitched, uncached write-out: 240-pixel rows at out_pitch stride. */
   {
      const unsigned int *src = (const unsigned int *)me_out;
      unsigned int dst   = d->out | 0x40000000u;
      unsigned int pitchb = d->out_pitch * 2;
      int row;
      for (row = 0; row < 160; row++)
      {
         unsigned int *o = (unsigned int *)(dst + (unsigned)row * pitchb);
         unsigned int w;
         for (w = 0; w < (GBA_SCREEN_PITCH * 2u) / 4u; w++)
         {
            o[w] = src[w];
            sum += src[w];
         }
         src += (GBA_SCREEN_PITCH * 2u) / 4u;
      }
   }
   return sum;
}
