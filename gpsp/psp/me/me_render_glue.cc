/* me_render_glue.cc — run the GBA scanline renderer (video.cc) ON the Media
 * Engine: the LIVE pipeline (mailbox v2).
 *
 * The input set and the 160-line replay loop are the ones proven bit-exact on
 * the desktop twice over: RENDER_REPLAY (~81k frames, full-register log) and
 * ME_CAP_VALIDATE (~61k frames, THIS slim capture struct).  What this file
 * adds is the two-phase hardware protocol:
 *
 *   phase 1 — SNAPSHOT: copy the live core vram/oam/palette into the
 *             engine's own copies (cached reads, invalidate first), then
 *             publish input_seq so the host resumes emulating the next frame.
 *   phase 2 — RENDER:   replay the me_capture_frame through update_scanline()
 *             into the engine's frame buffer, then write pitched rows to the host's
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

/* Must match gba_memory.h -- the ME does not include it. */
#define VRAM_DIRTY_SHIFT 10
#define VRAM_DIRTY_PAGES ((1024 * 96) >> VRAM_DIRTY_SHIFT)

/* THE WORK AREA (me_mbox.h): the VRAM mirror, the OBJ lists and the frame the
 * renderer draws into are the host's memory, named in every render desc.  The
 * first two are video.cc's (me_render_data.c holds the pointers it reads);
 * the frame is ours.  All three are set at the top of every render, from that
 * render's desc, and used by nothing outside it -- so there is no pointer here
 * that can outlive the memory it names. */
extern "C" {
extern u8 *me_work_vram, *me_work_vram_obj, *me_work_obj;
void me_obj_lists_reset(void);          /* video.cc */
}
static u16 *me_out;
/* The two sides agree on sizes through me_mbox.h alone; these are the shapes
 * the renderer actually uses (video.cc: obj_priority_list[5][160][128]). */
static_assert(GBA_SCREEN_PITCH * (160 + 1) * sizeof(u16) <= ME_WORK_OUT_BYTES,
              "the renderer's frame does not fit ME_WORK_OUT_BYTES");
static_assert(5 * 160 * 128 == ME_WORK_OBJ_BYTES && 1024 * 96 == ME_WORK_VRAM_BYTES,
              "ME_WORK_* no longer match the renderer's buffers");
static_assert((ME_WORK_BYTES & 63u) == 0 && (ME_WORK_OBJ_OFF & 63u) == 0 &&
              (ME_WORK_OUT_OFF & 63u) == 0,
              "the work area must be whole 64-byte cache lines");

/* The work area this module last brought to a known state, and the host's
 * generation for it.  Module .bss, so zero -- "none" -- after every load; the
 * host's generations start at 1. */
static unsigned int me_work_seen, me_work_gen_seen;
/* The colour LUT address this module has invalidated (0 = none yet). */
static unsigned int me_lut_seen;

/* Called by the dispatcher every time the ME comes out of reset.  The host
 * rebuilds the renderer after a sleep and says so with a new work_gen; this
 * is the same statement made from our side, for the sleep the host did not
 * manage to tear down for (main_psp.c, power_cb: teardown_skipped). */
extern "C" void me_render_boot(void)
{
   me_work_seen     = 0;
   me_work_gen_seen = 0;
   me_lut_seen      = 0;
}

/* Invalidate a main-RAM span in the ME D-cache before a cached read, so we see
 * the host's freshly written data and never a stale line. */
static void me_inv(unsigned int cached_addr, int nbytes)
{
   /* Line-aligned walk: stepping 64 from an UNALIGNED start misses the last
    * line whenever (addr % 64) + nbytes spills into one more line -- e.g. the
    * 96-byte vram_clean map at a line offset above 32 would leave the map's
    * tail (OBJ VRAM pages) served from a stale ME cache line. */
   unsigned int a   = cached_addr & ~63u;
   unsigned int end = cached_addr + (unsigned int)nbytes;
   for (; a < end; a += 64)
      __builtin_allegrex_cache(0x19, (int)a);
}

/* ME_CMD_RENDER entry (called from me_dispatch with the command's seq).
 * Returns an additive checksum of the output frame. */
extern "C" unsigned int me_render_run(volatile me_mbox *mb, unsigned int seq)
{
   me_render_desc *d = (me_render_desc *)(mb->arg0 & ~0x40000000u);
   const me_capture_frame *cap;
   unsigned int sum = 0;
   unsigned int work, fresh;

   me_inv((unsigned int)d, sizeof(*d));
   if (!d->vram || !d->capture || !d->out || !d->out_pitch)
   {
      mb->input_seq = seq;           /* never leave the host waiting */
      return 0;
   }
   /* NO WORK AREA, NO RENDER.  There is nothing in this module to fall back
    * on -- that is the point of the change -- so a desc without a usable area
    * is refused, out loud: the host reads refused_seq when it retires the
    * command and goes back to drawing on the main CPU.  Whole cache lines
    * only (see me_mbox.h), and at least the size this module was built for. */
   work = d->work & ~0x40000000u;
   if (!work || (work & 63u) || d->work_bytes < ME_WORK_BYTES)
   {
      mb->refused_seq = seq;
      mb->input_seq   = seq;
      return 0;
   }
   me_work_vram     = (u8 *)(work + ME_WORK_VRAM_OFF);
   me_work_vram_obj = me_work_vram + 0x10000;
   me_work_obj      = (u8 *)(work + ME_WORK_OBJ_OFF);
   me_out           = (u16 *)(work + ME_WORK_OUT_OFF);
   /* A work area we have not seen holds nothing of ours, whatever the host's
    * dirty map says about VRAM: see work_gen in me_mbox.h. */
   fresh = (work != me_work_seen || d->work_gen != me_work_gen_seen);

   /* ---- phase 1: snapshot the live core arrays ------------------------- */
   /* VRAM: copy only the pages the host marked dirty.
    *
    * A consecutive-frame compare measured 0.1-0.5 of 96 pages changing per
    * frame, so the flat 96 KB copy was re-fetching ~99.5% unchanged data
    * across the slow path to main RAM -- and the host SPUN for all of it
    * (wait_mean_us ~1450). The map is u8[96], 1 = clean, and is produced by
    * the dynarec's store stub plus the C write paths (coverage verified at
    * missed=0 over 255 marked pages).
    *
    * vram_clean == 0 means "no map": copy everything. The host sets that on
    * the first frame and after any teardown/resume, when this eDRAM mirror
    * cannot be trusted to be a correct base to patch. */
   if (d->vram_clean && !fresh)
   {
      const unsigned char *cl =
         (const unsigned char *)(d->vram_clean & ~0x40000000u);
      const unsigned char *src =
         (const unsigned char *)(d->vram & ~0x40000000u);
      unsigned int p;
      me_inv(d->vram_clean & ~0x40000000u, VRAM_DIRTY_PAGES);
      for (p = 0; p < VRAM_DIRTY_PAGES; p++)
      {
         if (cl[p])
            continue;                       /* clean: eDRAM copy still valid */
         me_inv((unsigned int)(src + (p << VRAM_DIRTY_SHIFT)),
                1u << VRAM_DIRTY_SHIFT);
         memcpy(me_work_vram + (p << VRAM_DIRTY_SHIFT),
                src + (p << VRAM_DIRTY_SHIFT), 1u << VRAM_DIRTY_SHIFT);
      }
   }
   else
   {
      me_inv(d->vram & ~0x40000000u, 1024 * 96);
      memcpy(me_work_vram, (const void *)(d->vram & ~0x40000000u), 1024 * 96);
   }
   me_inv(d->oam     & ~0x40000000u, 512 * 2);
   memcpy(oam_ram, (const void *)(d->oam & ~0x40000000u), 512 * 2);
   me_inv(d->palette & ~0x40000000u, 512 * 2);
   memcpy(palette_ram_converted, (const void *)(d->palette & ~0x40000000u),
          512 * 2);
   /* PRESENTATION COLOUR (me_mbox.h, video.h): this copy is the renderer's
    * own, so it is transformed in place; the log's palette values are
    * transformed as they are replayed (video_me_log.h ml_store). */
   {
      unsigned int mode = d->color_magic == ME_COLOR_MAGIC ? d->color_mode : 0;
      unsigned int lut  = d->color_lut & ~0x40000000u;
      if (mode >= VIDEO_COLOR_MODES || (mode == VIDEO_COLOR_GBA && !lut))
         mode = VIDEO_COLOR_NATIVE;
      if (mode == VIDEO_COLOR_GBA && lut != me_lut_seen)
      {
         me_inv(lut, ME_COLOR_LUT_BYTES);   /* read only: never dirty here */
         me_lut_seen = lut;
      }
      video_color_mode = mode;
      video_color_lut  = (const u16 *)lut;
      video_color_palette(palette_ram_converted);
   }
   me_inv(d->capture & ~0x40000000u, sizeof(me_capture_frame));
#if ME_MIDFRAME_LOG
   {
      /* The entries live in their own buffer; invalidate only what is used.
       * log_n is 0 unless this is a vcount-160 post (main_psp.c). */
      const me_capture_frame *c =
         (const me_capture_frame *)(d->capture & ~0x40000000u);
      if (c->log_n && c->log_addr)
         me_inv(c->log_addr & ~0x40000000u, c->log_n * sizeof(me_log_entry));
   }
#endif
   mb->input_seq = seq;              /* host may resume emulation NOW */

   if (fresh)
   {
      /* Bring the rest of a new area to what a freshly loaded module had in
       * its .bss: no OBJ lists, a black frame.  After input_seq, because the
       * host spins on that and this is 180 KB through a 16 KiB cache; it
       * happens once per renderer build, not per frame.  VRAM needs nothing
       * here -- the copy above just replaced all of it. */
      me_obj_lists_reset();
      memset(me_work_obj, 0, ME_WORK_OBJ_BYTES);
      memset(me_out, 0, ME_WORK_OUT_BYTES);
      me_work_seen     = work;
      me_work_gen_seen = d->work_gen;
   }

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

   /* The shared loop (video.cc): per-line registers and, with
    * ME_AFFINE_LINES, the per-line affine reference.  The desktop models
    * (ME_CAP_VALIDATE, ME_TIMING_SIM) run this same function. */
   me_replay_lines(cap, NULL, 1);

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
