/* ADR-0080d, applied to the engine: the host's np_start needs a clean heap.
 * The dual-engine gate run (host run 407) confirmed it — the engine's ~205 KB
 * boot allocation starved session creation and the host exited net_failed.
 * So the engine SUSPENDS around wireless bring-up: teardown + FREE everything,
 * let np_start claim its memory, then re-init from the post-session heap
 * (the exact profile me_video validated).  The CPU renders during the connect
 * screen; a failed re-alloc afterwards is the usual graceful CPU fallback. */
/* KEEP THE LAST FRAME for the wake overlay.
 *
 * It normally lives in the volatile partition we are about to hand back, so it
 * has to be copied into memory that survives: 82 KB (161 rows of 256) in
 * the tail of the netdrv arena, after netdrv's own part.  No heap cost,
 * which matters on the console with none to spare.
 *
 * Two sources, because the ME is not always the one drawing: its stage when
 * the renderer is up, otherwise the core's own frame buffer (the CPU path, and
 * what PPSSPP always uses -- which is what makes the overlay workshoppable off
 * hardware at all). */
static void wake_snapshot_from(const uint16_t *src, unsigned pitch,
                               unsigned src_w, unsigned src_h);
/* The colour mode the snapshot's frame was drawn in (filters v2). */
static unsigned g_wake_color;
static __attribute__((noinline)) void wake_snapshot(void)
{
   const uint16_t *src = NULL;
   unsigned pitch = 240;
   unsigned src_w = 240, src_h = 160;

   if (!g_wake_frame)
      return;
   src = me_rend_present_src(&pitch);
   g_wake_color = src ? g_mer_stage_mode[g_mer_ready] : video_color_mode;
   if (!src && g_last_pix)
      { src = g_last_pix; pitch = g_last_pitch; src_w = g_last_w; src_h = g_last_h; }
   if (!src)
      return;
   wake_snapshot_from(src, pitch, src_w, src_h);
}

static void wake_snapshot_from(const uint16_t *src, unsigned pitch,
                               unsigned src_w, unsigned src_h)
{
   unsigned y;
   memset(g_wake_frame, 0, WAKE_FRAME_BYTES);
   if (src_w > 240) src_w = 240;
   if (src_h > 160) src_h = 160;
   /* Top-left, at its own size: a GB picture centred inside 240x160 and
    * drawn AS 240x160 came out smaller than the live one (and at 2x was
    * cropped differently).  For a GBA frame this is the same copy as ever. */
   for (y = 0; y < src_h; y++)
   {
      memcpy(g_wake_frame + y * WAKE_TEX, src + y * pitch, src_w * 2);
      /* The staging buffer's edge texels (video_psp.c STAGE_STRIDE): a
       * bilinear tap past the right edge reads the edge, not black. */
      if (src_w < WAKE_TEX)
         g_wake_frame[y * WAKE_TEX + src_w] = src[y * pitch + src_w - 1];
   }
   if (src_h < WAKE_ROWS)
      memcpy(g_wake_frame + src_h * WAKE_TEX,
             g_wake_frame + (src_h - 1) * WAKE_TEX, (src_w + 1) * 2);
   g_wake_w = src_w;
   g_wake_h = src_h;
   /* The GE reads main RAM directly; these texels were written through the
    * CPU's cache, so they must be pushed out before it looks. */
   sceKernelDcacheWritebackRange(g_wake_frame, WAKE_FRAME_BYTES);
   g_wake_have = 1;
}

