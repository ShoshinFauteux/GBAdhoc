/* FILTERS V2: REDRAW THE MENU'S BACKDROP IN A NEW COLOUR MODE.
 *
 * The core is paused at the end of the frame (vcount 228), so its memory is
 * no longer the frame on screen: the VBlank has run since.  But that frame's
 * INPUTS still exist untouched -- its capture (per-line registers and the
 * mid-frame log), the line-160 palette and OAM the log was built against
 * (video_me_log.h, or the input shadows), and the engine's own VRAM mirror,
 * which no post has changed since.  So the engine can draw that frame again,
 * in any colour mode, without the core being touched at all.
 *
 * TRUSTED ONLY IF IT REPRODUCES.  The first redraw is in the mode the frame
 * was shown in, and its checksum must equal the one the engine reported for
 * the frame itself; anything that does not reproduce bit for bit (a dropped
 * frame after it, a log that overflowed, a state load) leaves the backdrop as
 * it was.  Then the second redraw, in the new mode, becomes the backdrop.
 * The frame on screen and the core's state are never touched: the redraws go
 * to the stage that is not being shown.
 *
 * Without the engine (the CPU renderer, PPSSPP), or when the check fails: a
 * snapshot taken in native colour is transformed on this CPU (exact for a
 * frame without blending); otherwise the new colour shows on resume. */
extern const u16 *me_log_line160_pal(void);
extern const u16 *me_log_line160_oam(void);
static const unsigned char g_all_clean[VRAM_DIRTY_PAGES + 64]
   __attribute__((aligned(64))) = { [0 ... VRAM_DIRTY_PAGES - 1] = 1 };

static int me_rend_redraw(unsigned mode, uint16_t *out, unsigned *sum)
{
   unsigned st = (unsigned)g_mer_ready, t0;
   me_capture_frame *cap;
   const u16 *pal, *oam;
   const void *vr;
   int all_clean;

   cap = g_mer_cap[g_mer_stage_cap[st]];
   if (!cap || g_mer_stage_cap[st] != (unsigned)(g_mer_cur ^ 1))
      return -1;                       /* a later post took the capture */
   if (g_mer_sh_on)
   {
      pal = (const u16 *)g_mer_sh_pal;  oam = (const u16 *)g_mer_sh_oam;
      vr  = g_mer_sh_vram;
      all_clean = 0;                   /* the shadow IS the frame's VRAM */
   }
#if ME_MIDFRAME_LOG
   else if (cap->log_n)
   {
      pal = me_log_line160_pal();
      oam = me_log_line160_oam();
      vr  = vram;
      all_clean = 1;                   /* keep the engine's own mirror */
   }
#endif
   else
   {
      pal = palette_ram_converted;     /* an end-of-frame post: now */
      oam = (const u16 *)oam_ram;
      vr  = vram;
      all_clean = 1;
   }
   if (!pal || !oam)
      return -1;
   sceKernelDcacheWritebackRange((void *)pal, 512 * 2);
   sceKernelDcacheWritebackRange((void *)oam, 512 * 2);
   g_mer_desc->vram       = (unsigned)vr;
   g_mer_desc->oam        = (unsigned)oam;
   g_mer_desc->palette    = (unsigned)pal;
   g_mer_desc->vram_clean = all_clean ? (unsigned)g_all_clean : 0u;
   g_mer_desc->capture    = (unsigned)cap;
   g_mer_desc->out        = (unsigned)out;
   g_mer_desc->out_pitch  = MER_STAGE_PITCH;
   g_mer_desc->work       = (unsigned)g_mer_work;
   g_mer_desc->work_bytes = ME_WORK_BYTES;
   g_mer_desc->work_gen   = g_mer_work_gen;
   g_mer_desc->color_magic = ME_COLOR_MAGIC;
   g_mer_desc->color_mode  = mode;
   g_mer_desc->color_lut   = (unsigned)gba_cc_lut;
   sceKernelDcacheWritebackRange(g_mer_desc, sizeof(*g_mer_desc));
   if (me_host_post_render((unsigned)ME_UNCACHED(g_mer_desc)) != 0)
      return -1;
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle() &&
          (unsigned)sceKernelGetSystemTimeLow() - t0 < 200000u)
      ;
   if (!me_host_idle() || me_host_render_refused())
      return -2;
   *sum = me_host_result();
   return 0;
}

void psp_color_preview(void)
{
   unsigned want = video_color_mode, sum = 0, t0 = 0;
   int rc = -9;
   uint16_t *out = NULL;

   if (!g_wake_have || !g_wake_frame || g_wake_color == want)
      return;
   if (g_me_rend && g_mer_desc && g_mer_ready >= 0 && g_mer_pending < 0 &&
       me_host_up() && me_host_idle())
   {
      unsigned st = (unsigned)g_mer_ready;
#ifdef REWIND
      out = g_mer_stage_own[st ^ 1];
#else
      out = g_mer_stage[st ^ 1];
#endif
      t0 = (unsigned)sceKernelGetSystemTimeLow();
      vid_gu_flush();                  /* the GE may still read that stage */
      rc = out ? me_rend_redraw(g_mer_stage_mode[st], out, &sum) : -1;
      if (rc == 0 && sum != g_mer_stage_sum[st])
         rc = -3;                      /* did not reproduce: not trusted */
      if (rc == 0)
         rc = me_rend_redraw(want, out, &sum);
      if (rc == 0)
      {
         sceKernelDcacheInvalidateRange(out, MER_STAGE_PITCH * 161 * 2);
         wake_snapshot_from(out, MER_STAGE_PITCH, 240, 160);
         g_wake_color = want;
      }
      t0 = (unsigned)sceKernelGetSystemTimeLow() - t0;
   }
   if (rc != 0 && g_wake_color == VIDEO_COLOR_NATIVE)
   {
      /* A native snapshot: transform it here.  Blended pixels are
       * transformed after their blend, the rest exactly. */
      unsigned y, x;
      /* Row g_wake_h and column g_wake_w are the snapshot's edge copies
       * (wake_snapshot_from): both are transformed (PLAN r2-B-5), or a
       * bilinear tap at the bottom edge would read native colour. */
      for (y = 0; y <= g_wake_h && y < WAKE_ROWS; y++)
         for (x = 0; x <= g_wake_w && x < WAKE_TEX; x++)
            g_wake_frame[y * WAKE_TEX + x] =
               video_color_pixel(g_wake_frame[y * WAKE_TEX + x]);
      sceKernelDcacheWritebackRange(g_wake_frame, WAKE_FRAME_BYTES);
      g_wake_color = want;
      rc = 1;
   }
   fe_evt("color_preview mode=%u rc=%d us=%u", want, rc, t0);
   FE_EVT_ONLY(t0);
}

