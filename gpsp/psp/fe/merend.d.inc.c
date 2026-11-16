/* Heap fallback only: give the work area back to malloc.  0 = freed (or there
 * was none), -1 = it must be kept for now.
 *
 * Unlike every other engine buffer this one is WRITTEN by the ME through its
 * cache, so "the ME is idle" is not enough: up to 16 KiB of it is still in
 * dirty lines the ME would write back whenever it next needed them -- on top
 * of the list pointers free() is about to put in the block.  So the ME drops
 * its cache first, and a sync that does not come back means the block stays.
 * A module that is not loaded has no cache to drain: the core is in reset.
 *
 * (The volatile pool is not freed this way.  It goes back on the way into a
 * sleep, after the same sync has confirmed the flush -- or, when that sync
 * fails, regardless, with the session ending at the wake: me_standby_down --
 * and the ME is held in reset by the suspend that follows.  Whatever is
 * carved from it next is ours again.) */
/* @extract:begin fn:mer_work_heap_free */
static int mer_work_heap_free(void)
{
   if (!g_mer_work)
      return 0;
   if (me_host_loaded() && me_host_sync(50000u) != 0)
      return -1;
   free(g_mer_work);
   g_mer_work = NULL;
   return 0;
}
/* @extract:end fn:mer_work_heap_free */

/* @extract:begin fn:me_rend_suspend */
static void me_rend_suspend(void)
{
   int i, idle;
   /* Volatile-backed: the engine owns no main heap, np_start is unaffected —
    * keep rendering straight through the wireless bring-up. */
   if (g_mer_vmem)
      return;
   if (g_me_rend)
      me_rend_teardown("np_start");
   /* Wait out any in-flight ME job before freeing the buffers under it. */
   /* A watchdog timeout disables posting; it does not stop the second core. */
   idle = !me_host_loaded();
   if (!idle)
   {
      unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
      while (!(idle = me_host_idle()) &&
             ((unsigned)sceKernelGetSystemTimeLow() - t0) < 50000u)
         ;
   }
   if (!idle)
   {
      /* Keep the heap-backed buffers alive if the ME failed to finish.  The
       * renderer is already disabled, so they can be reclaimed on a later
       * successful suspend or process exit without racing an in-flight job. */
      fe_evt("me_rend suspend_timeout buffers_retained=1");
      g_mer_heap_pending_drain = 1;
      return;
   }
   if (mer_work_heap_free() != 0)
   {
      fe_evt("me_rend suspend_timeout buffers_retained=1 reason=sync");
      g_mer_heap_pending_drain = 1;
      return;
   }
   /* Heap-backed fallback: free EVERYTHING.  The 41KB-resident experiment
    * failed on hardware — the host's np_start needs the heap fully clean
    * (passed at 0 KB held, run 411; failed at 41 KB held).  Resume may then
    * fail on a fragmented post-session heap; that degrades the host to CPU
    * rendering, which is safe.  The volatile path above makes all of this
    * moot wherever the lock succeeds. */
   for (i = 0; i < 2; i++)
   {
      free(g_mer_cap[i]);   g_mer_cap[i]   = NULL;
      free(g_mer_stage[i]); g_mer_stage[i] = NULL;
   }
   free(g_mer_desc); g_mer_desc = NULL;
}
/* @extract:end fn:me_rend_suspend */

static void me_rend_resume(void)
{
   int i;
   if (g_mer_heap_pending_drain)
   {
      /* A timed-out command may still hold the descriptor and both buffer
       * pairs.  Never let init memset/reuse them until the ME is confirmed
       * idle.  If it has drained, release the retained heap before rebuilding
       * the renderer. */
      if (!me_host_up() || !me_host_idle())
      {
         fe_evt("me_rend resume_deferred reason=me_busy");
         return;
      }
      if (mer_work_heap_free() != 0)
      {
         fe_evt("me_rend resume_deferred reason=me_sync");
         return;
      }
      for (i = 0; i < 2; i++)
      {
         free(g_mer_cap[i]);   g_mer_cap[i]   = NULL;
         free(g_mer_stage[i]); g_mer_stage[i] = NULL;
      }
      free(g_mer_desc); g_mer_desc = NULL;
      g_mer_heap_pending_drain = 0;
   }
   if (g_me_rend_cfg && !g_me_rend && me_host_up())
      if (me_rend_init() != 0)
         fe_evt("me_rend resume_failed (CPU rendering)");
}

static int me_rend_init(void)
{
   int i;
   if (!me_host_up())
      return -1;
   /* First choice: carve everything from volatile memory (see g_mer_vmem). */
   if (g_mer_vmem_cfg && !g_mer_vmem && !g_mer_cap[0])
   {
      void *vbase = NULL;
      int vsize = 0;
      /* 98 KB of shadows on top of the renderer's own 206 KB; the partition
       * is 4 MB on every model (measured 4194304 on both consoles). */
      const int shbytes = 1024 * 96 + 512 * 2 + 512 * 2;
      /* The ME's work area is part of what MUST fit: without it the engine
       * refuses every render. */
      const int need    = 2 * MER_CAP_BYTES + 2 * (MER_STAGE_PITCH*161*2) + 128
                          + (int)ME_WORK_BYTES;
      if (MER_VM_TRYLOCK(&vbase, &vsize) == 0)
      {
         if (vbase && vsize >= need)
         {
            unsigned p = ((unsigned)vbase + 63u) & ~63u;
            g_mer_desc     = (me_render_desc *)p;   p += 128;
            g_mer_cap[0]   = (me_capture_frame *)p; p += MER_CAP_BYTES;
            g_mer_cap[1]   = (me_capture_frame *)p; p += MER_CAP_BYTES;
            g_mer_stage[0] = (uint16_t *)p;         p += MER_STAGE_PITCH*161*2;
            g_mer_stage[1] = (uint16_t *)p;         p += MER_STAGE_PITCH*161*2;
            /* Whole cache lines, start to end: p is 64-aligned here and
             * ME_WORK_BYTES is a multiple of 64, so no line of the ME's
             * area holds a byte of anything this CPU writes. */
            g_mer_work     = (void *)p;             p += ME_WORK_BYTES;
            g_mer_vmem = 1;
            if (g_mer_shadow && vsize >= need + shbytes + 64)
            {
               p = (p + 63u) & ~63u;
               g_mer_sh_vram = (unsigned char *)p;  p += 1024 * 96;
               g_mer_sh_oam  = (unsigned char *)p;  p += 512 * 2;
               g_mer_sh_pal  = (unsigned char *)p;  p += 512 * 2;
               g_mer_sh_on   = 1;
            }
#if ME_MIDFRAME_LOG
            p = (p + 63u) & ~63u;
            if (g_mer_midlog &&
                p + 2u * MER_LOG_BYTES + 1024u * 96u <= (unsigned)vbase + (unsigned)vsize)
            {
               g_mer_log[0]   = (me_log_entry *)p;  p += MER_LOG_BYTES;
               g_mer_log[1]   = (me_log_entry *)p;  p += MER_LOG_BYTES;
               me_vram_shadow = (u8 *)p;            p += 1024 * 96;
            }
#endif
            fe_evt("me_rend vmem base=%08x size=%d shadow=%d",
                   (unsigned)vbase, vsize, g_mer_sh_on);
#ifdef REWIND
            g_vm_eng_top = p;
            psp_rewind_pool_check();
#endif
         }
         else
         {
            /* TryLock succeeded but the region cannot hold our layout. */
            MER_VM_REJECT();
            fe_evt("me_rend vmem rejected size=%d", vsize);
         }
      }
   }
   for (i = 0; i < 2; i++)
   {
      if (!g_mer_cap[i])
         g_mer_cap[i] = (me_capture_frame *)memalign(64, sizeof(me_capture_frame));
      if (!g_mer_stage[i])
         g_mer_stage[i] = (uint16_t *)memalign(64, MER_STAGE_PITCH * 161 * 2);
   }
   if (!g_mer_desc)
      g_mer_desc = (me_render_desc *)memalign(64, sizeof(me_render_desc));
   if (!g_mer_work)
      g_mer_work = memalign(64, ME_WORK_BYTES);
   if (!g_mer_cap[0] || !g_mer_cap[1] || !g_mer_stage[0] || !g_mer_stage[1] ||
       !g_mer_desc || !g_mer_work)
   {
      fe_evt("me_rend off reason=alloc");
      /* KEEP NOTHING OF A FAILED BUILD (review M8).  This is the heap
       * fallback running out part-way (the pool carve cannot fail here: it
       * sets every pointer).  The buffers it did get used to stay, so the
       * next call found g_mer_cap[0] set, never tried the volatile pool
       * again, and failed the same way: one busy TryLock on a wake meant
       * CPU rendering until the game was quit.  Nothing has been posted on
       * these yet; an area left from an earlier build is drained first, as
       * me_rend_suspend does, and kept if the engine does not answer. */
      if (!g_mer_vmem && !g_mer_heap_pending_drain && me_host_idle() &&
          mer_work_heap_free() == 0)
      {
         for (i = 0; i < 2; i++)
         {
            free(g_mer_cap[i]);   g_mer_cap[i]   = NULL;
            free(g_mer_stage[i]); g_mer_stage[i] = NULL;
         }
         free(g_mer_desc); g_mer_desc = NULL;
      }
      return -1;
   }
   memset(g_mer_cap[0], 0, sizeof(me_capture_frame));
   memset(g_mer_cap[1], 0, sizeof(me_capture_frame));
#if ME_MIDFRAME_LOG
   /* NULL entries = logging off for that capture (heap fallback, no room,
    * or me_midlog 0). */
   me_capture_log_setup(g_mer_cap[0], g_mer_vmem ? g_mer_log[0] : NULL,
                        MER_LOG_ENTRIES);
   me_capture_log_setup(g_mer_cap[1], g_mer_vmem ? g_mer_log[1] : NULL,
                        MER_LOG_ENTRIES);
   fe_evt("me_rend midlog=%d", (g_mer_vmem && g_mer_log[0]) ? 1 : 0);
#endif
   memset(g_mer_stage[0], 0, MER_STAGE_PITCH * 161 * 2);
   memset(g_mer_stage[1], 0, MER_STAGE_PITCH * 161 * 2);
   sceKernelDcacheWritebackInvalidateRange(g_mer_stage[0], MER_STAGE_PITCH*161*2);
   sceKernelDcacheWritebackInvalidateRange(g_mer_stage[1], MER_STAGE_PITCH*161*2);
   /* The work area is the ME's from here on (see g_mer_work).  This CPU has
    * not written it, but its cache may still hold lines for the range from
    * whoever had the memory before -- heap blocks certainly, the pool
    * possibly -- and a dirty one written back later would land in the middle
    * of the ME's data.  Flush and drop them now, while nothing is there. */
   sceKernelDcacheWritebackInvalidateRange(g_mer_work, ME_WORK_BYTES);
   g_mer_work_gen++;      /* new build: the ME starts the area from nothing */
   fe_evt("me_rend work base=%08x bytes=%u src=%s gen=%u vram=+%u obj=+%u "
          "out=+%u", (unsigned)g_mer_work, (unsigned)ME_WORK_BYTES,
          g_mer_vmem ? "vmem" : "heap", g_mer_work_gen,
          (unsigned)ME_WORK_VRAM_OFF, (unsigned)ME_WORK_OBJ_OFF,
          (unsigned)ME_WORK_OUT_OFF);
   g_mer_cur = 0;
   g_mer_full_copy = 1;   /* fresh ME: nothing to patch against */
   g_mer_pending = g_mer_ready = g_mer_last_presented = -1;
   me_capture_buf  = g_mer_cap[0];
   me_capture_mode = g_mer_capmode;      /* 1: core stops rendering NOW */
   if (g_mer_capmode != 1)
      fe_evt("me_rend capmode=%u (CPU renders too)", g_mer_capmode);
#ifdef REWIND
   g_mer_stage_own[0] = g_mer_stage[0];
   g_mer_stage_own[1] = g_mer_stage[1];
   g_mer_priv[0] = g_mer_priv[1] = 0;
   g_rw_flush_posts = 32;       /* (see g_rw_flush_posts) */
#endif
   g_me_rend = 1;
   fe_evt("me_rend on stage_bytes=%u cap_bytes=%u",
          (unsigned)(MER_STAGE_PITCH * 161 * 2) * 2,
          (unsigned)sizeof(me_capture_frame) * 2);
   return 0;
}

/* Write back the live inputs and fill the render desc for stage `out`. */
static void me_rend_fill_desc(int out)
{
   /* Loop-top presentation queues a GE texture read from the ready stage.
    * Retiring the other ME stage can then make that old ready stage `out`.
    * A completed ME render does not mean the GE has finished reading its
    * predecessor: drain those reads before allowing the ME to overwrite it.
    * With no queued lists this is a no-op; it does not wait for vblank. */
   vid_gu_flush();

#ifdef REWIND
   /* Where this frame is drawn: its slot in the rewind's windows, or one of
    * the engine's own two stages.  `out` is never the ready stage, so the
    * picture on screen is not touched either way. */
   if (g_rw_slot)
   {
      g_mer_stage[out] = g_rw_slot;
      if (g_rw_flush_posts)
      {
         sceKernelDcacheWritebackInvalidateRange(g_rw_slot,
                                                 MER_STAGE_PITCH * 161 * 2);
         g_rw_flush_posts--;
      }
   }
   else
      g_mer_stage[out] = g_mer_stage_own[out];
   g_mer_priv[out] = g_rw_slot != NULL && fe_host_rewind_replaying();
#endif
   if (g_mer_sh_on)
   {
      /* Snapshot the live arrays so the ME has something stable to read and
       * emulation can resume without waiting for it.  Cached->cached in main
       * RAM; the writeback that follows is the whole coherency contract. */
      unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
      unsigned d;
      memcpy(g_mer_sh_vram, vram, 1024 * 96);
      memcpy(g_mer_sh_oam, oam_ram, 512 * 2);
      memcpy(g_mer_sh_pal, palette_ram_converted, 512 * 2);
      sceKernelDcacheWritebackRange(g_mer_sh_vram, 1024 * 96);
      sceKernelDcacheWritebackRange(g_mer_sh_oam, 512 * 2);
      sceKernelDcacheWritebackRange(g_mer_sh_pal, 512 * 2);
      d = (unsigned)sceKernelGetSystemTimeLow() - t0;
      g_mer_sh_us += d;
      if (d > g_mer_sh_max)
         g_mer_sh_max = d;
   }
   else if (g_mer_full_copy || !g_mer_dirty_cfg)
   {
      /* Mirror not trustworthy: copy everything and tell the ME to do the
       * same by passing no map. */
      sceKernelDcacheWritebackRange(vram, 1024 * 96);
      sceKernelDcacheWritebackRange(oam_ram, 512 * 2);
      sceKernelDcacheWritebackRange(palette_ram_converted, 512 * 2);
   }
   else
   {
      /* Write back ONLY the dirty pages -- the other half of the saving.
       * The old code pushed 96 KB through the cache every frame purely so
       * the ME could re-read data that had not changed. */
      unsigned i, n = 0;
      for (i = 0; i < VRAM_DIRTY_PAGES; i++)
         if (!vram_clean[i])
         {
            sceKernelDcacheWritebackRange(vram + (i << VRAM_DIRTY_SHIFT),
                                          1u << VRAM_DIRTY_SHIFT);
            n++;
         }
      sceKernelDcacheWritebackRange(vram_clean, VRAM_DIRTY_PAGES);
      sceKernelDcacheWritebackRange(oam_ram, 512 * 2);
      sceKernelDcacheWritebackRange(palette_ram_converted, 512 * 2);
      g_mer_wb_pages += n;
      g_mer_wb_n++;
   }
#if ME_MIDFRAME_LOG
   {
      /* The log describes lines 0-160 relative to the line-160 memory, so it
       * is only valid for a vcount-160 post.  Any other post (3x
       * fast-forward's end-of-frame one) renders from the snapshot alone, as
       * before the log existed. */
      me_capture_frame *c = g_mer_cap[g_mer_cur];
      if (!g_mer_in_vis)
         c->log_n = 0;
      if (c->log_n)
         sceKernelDcacheWritebackRange((void *)c->log_addr,
                                       c->log_n * sizeof(me_log_entry));
   }
#endif
   sceKernelDcacheWritebackRange(g_mer_cap[g_mer_cur],
                                 sizeof(me_capture_frame));
   g_mer_desc->vram      = g_mer_sh_on ? (unsigned)g_mer_sh_vram
                                       : (unsigned)vram;
   g_mer_desc->oam       = g_mer_sh_on ? (unsigned)g_mer_sh_oam
                                       : (unsigned)oam_ram;
   g_mer_desc->palette   = g_mer_sh_on ? (unsigned)g_mer_sh_pal
                                       : (unsigned)palette_ram_converted;
   /* 0 = no map = copy all of VRAM. See g_mer_full_copy. */
   g_mer_desc->vram_clean =
      (g_mer_sh_on || g_mer_full_copy || !g_mer_dirty_cfg)
      ? 0u : (unsigned)vram_clean;
   g_mer_desc->capture   = (unsigned)g_mer_cap[g_mer_cur];
   g_mer_desc->out       = (unsigned)g_mer_stage[out];
   g_mer_desc->out_pitch = MER_STAGE_PITCH;
   g_mer_desc->work       = (unsigned)g_mer_work;
   g_mer_desc->work_bytes = ME_WORK_BYTES;
   g_mer_desc->work_gen   = g_mer_work_gen;
   /* PRESENTATION COLOUR: the mode and the EBOOT's own LUT (me_mbox.h). */
   g_mer_desc->color_magic = ME_COLOR_MAGIC;
   g_mer_desc->color_mode  = video_color_mode;
   g_mer_desc->color_lut   = (unsigned)video_color_lut;
   g_mer_stage_mode[out] = video_color_mode;
   g_mer_stage_cap[out]  = (unsigned)g_mer_cur;
   sceKernelDcacheWritebackRange(g_mer_desc, sizeof(*g_mer_desc));
}

/* Complete frames actually rendered in the current fps window (ME: a retired
 * render; CPU path: a frame the core really drew).  Reported beside the
 * emulated rate so a frameskipped or ME-outrun measurement can never read as
 * if every frame were being produced. */
static unsigned g_fps_drawn;

/* Loop-top presentation for Media Engine mode (ADR-0082). */
/* @extract:begin ff:me_rend_present */
/* @extract:begin lowlat:me_rend_present */
static __attribute__((noinline)) void me_rend_present(void)
{
   if (!g_me_rend || g_mer_ready < 0)
      return;
   if (g_ff_gate && g_mer_ready == g_mer_last_presented)
      return; /* No new render: keep the LCD frame and let emulation run. */
   if (g_mer_ready != g_mer_last_presented)
   {
      FFW_DRAWN();
      if (g_bb_new)
         g_lat_over++;          /* an unshown frame overwritten (dropped) */
      g_bb_new = 1;
      g_bb_pvc = g_mer_stage_pvc[g_mer_ready];
   }
   vid_draw_prestaged(g_mer_stage[g_mer_ready], 240, 160);
   g_mer_last_presented = g_mer_ready;
   g_drew = 1;
}

/* @extract:end lowlat:me_rend_present */
/* SAME-FRAME RETIREMENT.  Promote the stage the ME has just finished to
 * "ready" so this frame's own render is what gets presented this frame -- the
 * whole point of the feature.
 *
 * This is the stage-ownership handoff, and it was written out twice: once at
 * the ME's done-report inside the loop and once at the pre-swap blit.  Two
 * identical copies of an ownership protocol is one edit away from two
 * different protocols, so it lives here, in the renderer that owns the stages.
 *
 * It deliberately does NOT present.  The single present happens before the
 * swap, so a frame that never retires still leaves something in the back
 * buffer.  And there is exactly one producer: a second one from the
 * visible-end hook would corrupt stage/capture ownership. */
/* @extract:begin lowlat:me_rend_retire_if_idle */
static __attribute__((noinline)) void me_rend_retire_if_idle(void)
{
   if (!g_me_rend || g_mer_pending < 0)
      return;
   /* me_sameframe keeps exactly its old reach; the low-latency retire
    * stays out of a rewind (which repoints the stages itself). */
   if (!mer_sameframe_active() &&
       (!mer_retire_early_active() || mer_rewind_busy()))
      return;
   if (!me_host_idle())
      return;
   if (me_host_render_refused())
   {
      me_rend_teardown("refused");     /* see step 1 of me_rend_frame */
      return;
   }
#ifdef REWIND
   /* (a stage drawn for a frame the rewind re-ran is not for the screen:
    * it retires without becoming the one to present) */
   if (g_mer_priv[g_mer_pending])
      g_mer_priv[g_mer_pending] = 0;
   else
#endif
   g_mer_ready   = g_mer_pending;
   mer_note_sum();
   g_mer_pending = -1;
   g_mer_miss    = 0;
   g_fps_drawn++;
   if (mer_sameframe_active())
      g_mer_sf_hit++;
}

/* @extract:end ff:me_rend_present */
/* @extract:end lowlat:me_rend_retire_if_idle */
/* Called from plat_video_frame at the end of every emulated frame.
 * `emulated` is false for duped/skipped frames (no capture happened). */
/* @extract:begin ff:me_rend_frame */
static void me_rend_frame(int emulated)
{
   unsigned t0, t1;

   if (!g_me_rend)
      return;
   LOOP_PHASE(7);
   g_mer_frames++;

   /* 1. Retire the previous post (render of frame N-1).  A full frame has
    *    elapsed since the post, so the ~10 ms render is normally long done;
    *    spin only for a genuinely late one.
    *
    *    Under the FF draw gate a frame the gate turned away (`emulated` 0)
    *    only LOOKS: it has nothing to post, so it never waits.  A drawn frame
    *    follows the previous post by a whole gate period (>= 16.7 ms), so its
    *    wait is normally nil; it may spin to the hard ceiling for a late one,
    *    because a drawn frame is a promise to the screen, not a best effort. */
   if (g_mer_pending >= 0)
   {
      unsigned budget = g_ff_gate ? (emulated ? MER_INPUT_US : 0u) :
                        MER_RETIRE_US;
#ifdef REWIND
      /* A frame re-run for a rewind does not stand and wait 9 ms for the
       * picture before it: the pace leaves a display period between two
       * drawn frames, and one that still finds the engine busy goes
       * without a picture. */
      if (fe_host_rewind_replaying())
         budget = MER_REWIND_RETIRE_US;
#endif
      t0 = (unsigned)sceKernelGetSystemTimeLow();
      while (!me_host_idle() &&
             ((unsigned)sceKernelGetSystemTimeLow() - t0) < budget)
         ;
#ifdef REWIND
      if (fe_host_rewind_replaying())
      {
         g_rwm.retire_us += (unsigned)sceKernelGetSystemTimeLow() - t0;
         if (!me_host_idle())
            g_rwm.busy++;
      }
#endif
      if (me_host_idle())
      {
         /* The ME completed the command without drawing: it found no usable
          * work area in the desc (me/me_mbox.h, refused_seq).  The stage
          * holds no frame, so it is never promoted; the core draws again
          * from the next frame, as after any other teardown. */
         if (me_host_render_refused())
         {
            me_rend_teardown("refused");
            goto present;
         }
#ifdef REWIND
         /* (see me_rend_retire_if_idle: a re-run frame's stage) */
         if (g_mer_priv[g_mer_pending])
            g_mer_priv[g_mer_pending] = 0;
         else
#endif
         g_mer_ready   = g_mer_pending;
         mer_note_sum(); /* @lab */ LAB_FFP(lab_ffpace_me_retire(g_mer_post_us, (unsigned)sceKernelGetSystemTimeLow() - t0);)
         g_mer_pending = -1;
         g_mer_miss    = 0;
         g_fps_drawn++;             /* a complete rendered frame landed */
      }
      else
      {
         if (g_ff_gate)
         {
            if (!emulated)
               goto present;   /* gated-out frame: look again next frame */
            /* Every drawn frame must be posted. A hung renderer must not
             * turn the gate into a silent dropping mode or stall the PSP
             * forever: use the existing CPU fallback on this hard timeout. */
            me_rend_teardown("ff_render_wedge");
            goto present;
         }
#ifdef REWIND
         /* A frame re-run by the rewind that found the engine busy simply
          * has no picture; it is not a late renderer and must never count
          * towards tearing the engine down. */
         if (fe_host_rewind_replaying())
            goto present;
#endif
         /* ME still busy: drop this frame (present the old one), do NOT
          * flip the capture buffer — the next emulated frame overwrites
          * the capture and we try again. */
         g_mer_drops++;
         /* Any fast-forward outruns the ME by design — drops there are the
          * expected outcome, not a sick renderer, and a teardown would
          * silently switch rendering paths mid-FF (or mid-measurement).
          * Normal-speed lateness still trips the watchdog. */
         if (++g_mer_miss >= MER_MISS_LIMIT &&
             !g_ff_uncapped && !g_ff_mult && !g_pcfg.bench_mode)
            me_rend_teardown("late");
         goto present;
      }
   }

   /* 2. Post this frame's render (only if the core actually captured it). */
   if (emulated && me_host_up())
   {
      int out = (g_mer_ready == 0) ? 1 : 0;   /* render into the other stage */
      me_rend_fill_desc(out);
      if (me_host_post_render((unsigned)ME_UNCACHED(g_mer_desc)) == 0)
      {
         /* 3. Hand the capture buffer over and resume.
          *
          * With shadows the ME is reading copies nothing will touch, so there
          * is NOTHING to wait for -- the ~1.45 ms spin below is skipped
          * entirely and the capture flip is immediately safe (the captures
          * were already double-buffered).  Without them we must still wait
          * for the ME to finish reading the live arrays before emulation is
          * allowed to write them again. */
         if (!g_mer_sh_on)
         {
            t0 = (unsigned)sceKernelGetSystemTimeLow();
            while (!me_host_input_done() &&
                   ((t1 = (unsigned)sceKernelGetSystemTimeLow()) - t0) < MER_INPUT_US)
               ;
            t1 = (unsigned)sceKernelGetSystemTimeLow() - t0;
            g_mer_wait_us += t1;
            if (t1 > g_mer_wait_max)
               g_mer_wait_max = t1;
            if (!me_host_input_done())
            {
               me_rend_teardown("input_wedge");   /* watchdog-class failure */
               goto present;
            }
            /* The ME has now copied. Only here is it safe to clear the map:
             * clearing it before the copy would lose pages the ME had not
             * read yet and leave its mirror stale. The core is blocked in the
             * spin above, so no VRAM write can be missed in this window. */
#if ME_MIDFRAME_LOG
            me_vram_map_consumed();   /* same clear, keeps the log's shadow honest */
#else
            memset(vram_clean, 1, VRAM_DIRTY_PAGES);
#endif
            if (g_mer_dirty_cfg)
               g_mer_full_copy = 0;   /* mirror is now valid to patch */
         }
         g_mer_pending = out; /* @lab */ LAB_FFP(lab_ffpace_me_post(out, fe_host_frame_count());)
         g_mer_stage_pvc[out] = g_lat_poll_vc;
         g_mer_stage_seq[out] = g_lat_seq;
#ifdef REWIND
         if (g_rw_slot)
            g_rw_slot_posted = 1;
#endif
         g_mer_post_us = (unsigned)sceKernelGetSystemTimeLow();
         g_mer_rend_seen = 0;
         g_mer_cur ^= 1;
         me_capture_buf = g_mer_cap[g_mer_cur];
      }
      else
         g_mer_drops++;
   }

present:
   /* 4. Presentation moved to loop-top (me_rend_present) — swap-phase
    * discipline; see the FF triage note above. */
   /* The gated presets may run many tiny guest frames during one valid ME
    * render. Do not count those as 30 separate heartbeat failures in a few ms. */
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   if (g_me_rend && (!g_ff_gate || t0 - g_mer_watch_us >= 16667u))
   {
      g_mer_watch_us = t0;
      if (me_host_watchdog_frame())
         me_rend_teardown("watchdog");
   }

   if (++g_mer_census >= 600)
   {
      fe_evt("me_rend frames=%u drops=%u wait_mean_us=%u wait_max_us=%u "
             "shadow=%d sh_mean_us=%u sh_max_us=%u "
             "dirty=%d wb_pages=%u sf=%d sf_hit=%u sf_miss=%u "
             "rend_n=%u rend_mean_us=%u rend_max_us=%u "
             "vis_n=%u vis_mean_us=%u vis_max_us=%u",
             g_mer_frames, g_mer_drops, g_mer_wait_us / g_mer_census,
             g_mer_wait_max, g_mer_sh_on,
             g_mer_sh_us / g_mer_census, g_mer_sh_max,
             g_mer_dirty_cfg,
             g_mer_wb_n ? g_mer_wb_pages / g_mer_wb_n : 0u,
             g_mer_sameframe, g_mer_sf_hit, g_mer_sf_miss,
             g_mer_rend_n, g_mer_rend_n ? g_mer_rend_us / g_mer_rend_n : 0,
             g_mer_rend_max,
             g_vis_n, g_vis_n ? g_vis_us / g_vis_n : 0, g_vis_max);
#if ME_MIDFRAME_LOG
      /* Cumulative recorder work (video.h me_log_stats_t): what the mid-frame
       * log costs this CPU, in 1 KiB passes and per-line checks. */
      fe_evt("me_midlog frames=%u tracked=%u pal=%u oam=%u scans=%u "
             "diffs=%u copies=%u base=%u entries=%u overflows=%u",
             me_log_stats.frames, me_log_stats.tracked_frames,
             me_log_stats.pal_checks, me_log_stats.oam_checks,
             me_log_stats.map_scans, me_log_stats.page_diffs,
             me_log_stats.page_copies, me_log_stats.base_copies,
             me_log_stats.entries, me_log_stats.overflows);
#endif
      g_mer_census = 0;
      g_mer_wait_us = g_mer_wait_max = 0;
      g_mer_sh_us = g_mer_sh_max = 0;
      g_mer_rend_n = g_mer_rend_us = g_mer_rend_max = 0;
      g_vis_n = g_vis_us = g_vis_max = 0;
      g_mer_wb_pages = g_mer_wb_n = 0;
      if (g_mer_sameframe)
         fe_evt("me_sf_gate noloop=%u wouldhit=%u miss=%u",
                g_mer_sf_noloop, g_mer_sf_wouldhit, g_mer_sf_miss);
      g_mer_sf_hit = g_mer_sf_miss = 0;
      g_mer_sf_noloop = g_mer_sf_wouldhit = 0;
   }
   LOOP_PHASE(2);   /* back in the core's frame (always called from it) */
}

/* @extract:end ff:me_rend_frame */
/* FPS counter (Settings -> "FPS counter").  Counts EMULATED frames — the
 * increment sits above the blit-suppress and FF-cadence returns, so during
 * multiplier/uncapped FF the chip reads the true emulation speed (~179 at
 * 3x), not the ~30 Hz presentation cadence. */
static unsigned g_fps_emu_frames, g_fps_win_us, g_fps_last_pg, g_fps_last_smc,
                g_fps_last_dma, g_fps_last_fus, g_fps_last_fkb, g_fps_last_wide,
                g_fps_last_xlat, g_fps_last_rom, g_fps_last_jit;
static int g_autoload_state;   /* harness `load_state = 1`: boot into .st0 */
/* 1 when the autoload was asked for by the player (the browser's state
 * shelf), not by a rig.  Decides what a failed load means -- see the
 * autoload in the main loop. */
static int g_autoload_from_shelf;

/* The ROM-cache stats are gba_memory.c's (core_fe.h).  gamepak_must_swap
 * returns bool there; this int copy goes in C6-4 (health-out/design/C-6.md). */
int gamepak_must_swap(void);

static void plat_video_frame(const uint16_t *pix, unsigned w, unsigned h,
                             size_t pitch)
{
   static int rom_cache_logged;
   if (!rom_cache_logged && g_pcfg.console == FE_CONSOLE_GBA)
   {
      /* One-shot: how much of the cart the ROM cache actually holds.  With
       * PSP_LARGE_MEMORY on a 2000+ this should read resident=1 even for
       * 32 MB carts; on a 1000 a 32 MB cart pages (resident=0). */
      rom_cache_logged = 1;
#ifndef GPSP_NO_TELEMETRY
      {
         /* Where the blocks landed: on a 64 MiB console the heap runs past
          * 0x0A000000, so this says whether any of the cart lives up there. */
         unsigned bi, blo = ~0u, bhi = 0;
         for (bi = 0; bi < gamepak_buffer_count; bi++)
         {
            unsigned a = (unsigned)(uintptr_t)gamepak_buffers[bi];
            if (a < blo) blo = a;
            if (a > bhi) bhi = a;
         }
         fe_evt("rom_cache blocks=%u rom=%uKB resident=%d lo=%08x hi=%08x "
                "plan=%s want=%u got=%u spare=%u pages=%u",
                gamepak_buffer_count, gamepak_size >> 10,
                !gamepak_must_swap(), blo, bhi + (1u << 20),
                gamepak_cap_reason, (unsigned)gamepak_resident_wanted,
                (unsigned)gamepak_resident_got,
                (unsigned)gamepak_static_blocks,
                (unsigned)gamepak_buffer_pages);
      }
#endif
   }
   /* A link session's stall re-presents the last picture (NULL) without
    * emulating anything: counting it made the WAITING console's chip read
    * ~60 while both ran at 45 (hw3 Y: the join showed 58-61, its log 45). */
   if (!fe_host_gblink_stalled())
      g_fps_emu_frames++;
   if (pix)
   {
      /* Kept for the wake overlay's snapshot when the ME is not the one
       * drawing -- the CPU path, and everything under PPSSPP. */
      g_last_pix   = pix;
      g_last_pitch = (unsigned)(pitch / 2);
      g_last_w = w;
      g_last_h = h;
      cur_frame = pix;
      cur_w = w;
      cur_h = h;
      cur_pitch = pitch;
   }
   if (g_blit_suppress)
      return;   /* intermediate multiplier-FF frame */

   /* All ME profiles share the capture/retire pipeline.  Under the draw gate
    * a gated-out frame arrives here with pix == NULL (no capture): it only
    * retires a finished render. */
   if (g_me_rend)
   {
      if (!g_mer_posted)
         me_rend_frame(pix != NULL);
      return;
   }

   /* Preserve the harness's historical CPU presentation sampling. The user
    * presets blit every frame the core drew (the gate already chose them). */
   if (g_ff_uncapped && !g_ff_user &&
       (fe_host_frame_count() & FF_PRESENT_MASK))
      return;
   if (g_ff_gate && !pix)
      return; /* Gated-out frame: nothing new, so no duplicate GE blit. */

   /* ADR-0080c: session-gated activation.  Offload runs ONLY while a WLAN
    * session is live — which begins AFTER np_start, so the ME video path
    * never touches ad-hoc group creation, on host or join alike.  Re-arms
    * every session; a permanent teardown (dead/slow ME) leaves _cfg clear
    * so this cannot flip it back on. */
   if (g_me_video_cfg)
   {
      if (!g_me_video && g_net_up)
         me_video_activate();
      else if (g_me_video && !g_net_up)
         me_video_teardown("session_end", 0);   /* re-arm for next session */
   }

   if (g_me_video)
   {
      /* ADR-0082: post only — the GE list is issued at the TOP of the next
       * loop iteration (me_video_present), not here. */
      me_video_post(pix, w, h, pitch);
      return;
   }
   /* NULL = duped/skipped frame: re-draw the previous one (double-buffered
    * display needs every frame drawn). */
   if (cur_frame)
   {
      vid_draw_frame(cur_frame, cur_w, cur_h, cur_pitch);
      g_drew = 1;
      if (pix)
      {
         g_fps_drawn++;      /* a new image, not a re-present of the old one */
         FFW_DRAWN();
         if (g_bb_new)
            g_lat_over++;
         g_bb_new = 1;
         g_bb_pvc = g_lat_poll_vc;
      }
   }
}

