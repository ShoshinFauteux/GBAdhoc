/* VISIBLE-END POST: hand the ME its render at vcount 160 even when
 * presentation stays at the loop top.
 *
 * The ME reads vram/oam/palette when the render is POSTED, but the capture
 * holds the LCD registers of lines 0-159 only.  Posted at the end of
 * retro_run, the engine combined frame N's registers with graphics memory
 * that had already been through frame N's VBlank -- where games write the
 * NEXT frame.  Pokemon's battle animations ("monbg": Scratch, Tackle...) copy
 * a battler into BG1's tiles and map in the same VBlank that moves BG1's
 * scroll onto it; the engine drew the new map at the OLD scroll, and the
 * battler flashed at the top-left of the screen for one frame.  Measured with
 * the desktop model (ME_TIMING_SIM, docs/ME-RENDERER-DIVERGENCE.md): posting
 * at vcount 160 renders exactly the frame the CPU renderer draws, which is
 * where the capture is complete anyway.
 *
 * The work is the same work, moved 68 scanlines earlier; the previous render
 * was posted a full frame before, as before, so step 1 of me_rend_frame
 * waits no longer than it did.  Fast-forward WITHOUT the draw gate (the 1.5x,
 * 2x and 3x presets: g_ff_mult; an ungated uncapped run: bench mode, harness
 * `ff`) keeps the end-of-frame post it was validated with (FF-ARTIFACT-FIX.md);
 * the gated runs' DRAWN frames (Unlimited, Smooth, 3x under FF flip pacing bit
 * 2) post here like any 1x frame (docs/FF-PRESETS.md; me_vispost=0 is gone). */
/* MID-FRAME LOG (`me_midlog`, harness only, default 1 when the build has
 * ME_MIDFRAME_LOG; video.h, docs/ME-MIDFRAME.md).  The entry buffers and the
 * core's VRAM shadow are carved from the volatile partition after everything
 * else; without room, or on the heap fallback, the log stays off and the
 * engine behaves exactly as without the flag.  0 = the same off state, for a
 * one-binary A/B. */
#if ME_MIDFRAME_LOG
#define MER_LOG_ENTRIES 12288                         /* = 144 KiB a capture */
#define MER_LOG_BYTES   (MER_LOG_ENTRIES * (int)sizeof(me_log_entry))
static int g_mer_midlog = 1;
static me_log_entry *g_mer_log[2];
#endif
/* The render being posted right now is a vcount-160 post (the only kind the
 * mid-frame log is valid for). */
static int g_mer_in_vis;
static int mer_vispost_active(void)
{
   /* The draw gate's drawn frames post here too, exactly as at 1x: the gate
    * decides at frame START whether a frame is drawn, so a drawn frame is an
    * ordinary capture and gets the line-160 post and the mid-frame log.  An
    * ungated multiplier (g_ff_mult) or uncapped run keeps the frame-end post. */
   return (g_ff_gate || (!g_ff_mult && !g_ff_uncapped)) &&
          !skip_next_frame;
}
static int g_mer_posted;        /* posted from the vcount-160 hook this frame */
/* Frame start at file scope. frame_t0 in the main loop is a LOCAL and is only
 * taken while a session is up; this probe wants every frame. */
static uint64_t g_frame_start_us;

static int      g_mer_rend_seen;
/* Heap buffers retained after a failed ME drain must not be reused until the
 * in-flight command has completed. */
static int      g_mer_heap_pending_drain;

static void me_rend_teardown(const char *why)
{
   FE_EVT_ONLY(why);
   if (!g_me_rend)
      return;
   g_me_rend = 0;
#ifdef REWIND
   /* The stages are the engine's own again: whoever frees or re-uses them
    * next must not find a rewind slot there. */
   if (g_mer_stage_own[0])
   {
      g_mer_stage[0] = g_mer_stage_own[0];
      g_mer_stage[1] = g_mer_stage_own[1];
   }
   g_mer_priv[0] = g_mer_priv[1] = 0;
#endif
   g_mer_full_copy = 1;   /* eDRAM mirror is now stale; rebuild it in full */
   me_capture_mode = 0;                  /* core renders on the CPU again */
   me_capture_buf  = NULL;
   g_mer_pending = g_mer_ready = g_mer_last_presented = -1;
   fe_evt("me_rend off reason=%s frames=%u drops=%u", why,
          merend_tel.frames, merend_tel.drops);
}

static int me_rend_init(void);   /* defined below */

/* ---- RENDERER ENTRY POINTS FOR THE LIFECYCLE -----------------------------
 * The wireless session has talked to the engine through me_rend_suspend() /
 * me_rend_resume() since ADR-0080d.  The standby path did not: it read
 * g_me_rend, g_mer_vmem, g_mer_ready and g_mer_stage directly and hand-rolled
 * the volatile release.  Measured, comments stripped: ten renderer symbols
 * reached into the lifecycle code, eight of them raw variables, 24 references.
 * These four put standby on the same footing as the wireless path, so buffer
 * ownership lives in exactly one place -- it is now six symbols, all
 * functions, 11 references. */

static int me_rend_active(void)    { return g_me_rend; }
static int me_rend_vmem_held(void) { return g_mer_vmem; }

/* IS THE ENGINE THE PRESENTER RIGHT NOW, and if so which buffer and what
 * stride.  The three callers that used to ask this inline -- the wake overlay,
 * the screenshot dumper and the vhash oracle -- each wrote their own copy of
 * `g_me_rend && g_mer_ready >= 0`, and two of them disagreed with the third
 * about whether a NULL stage counts.  So this is the loose form, matching what
 * the dumpers did: it answers "yes, the engine" and hands back the pointer even
 * when that pointer is NULL, because their own `if (!pix) return;` is what
 * decides in that case.  Conflating the two questions changed what a screenshot
 * does when they disagree.
 *
 * THE STRIDE IS IN PIXELS.  fe_host_last_frame() reports BYTES, so the two
 * dumpers double it at the call site, visibly: passing pixels where bytes were
 * wanted interleaved the left and right halves of every row in the first
 * ME-mode dumps, and the display looked fine the whole time. */
static int me_rend_presenting(const uint16_t **src, unsigned *pitch_px)
{
   if (!g_me_rend || g_mer_ready < 0)
      return 0;
   *src      = g_mer_stage[g_mer_ready];
   /* The ME writes the stage UNCACHED, but every caller here (vhash,
    * screenshot, state thumbnail, wake overlay) reads it through this CPU's
    * cache -- which may still hold lines from the last read of the same stage
    * two frames ago, i.e. a stale picture.  Drop them first.  Safe because
    * this CPU never writes a stage after me_rend_init's writeback-invalidate,
    * so there are no dirty lines to lose; base and size are 64-byte aligned. */
   if (g_mer_stage[g_mer_ready])
      sceKernelDcacheInvalidateRange(g_mer_stage[g_mer_ready],
                                     MER_STAGE_PITCH * 161 * 2);
   *pitch_px = MER_STAGE_PITCH;
   return 1;
}

/* The strict form: the loose one plus "and it actually has a buffer".  The wake
 * overlay wants this, because it copies unconditionally once it has a source
 * and has no later NULL check to fall back on. */
static const uint16_t *me_rend_present_src(unsigned *pitch)
{
   const uint16_t *src = NULL;
   unsigned px = 0;
   if (!me_rend_presenting(&src, &px) || !src)
      return NULL;
   *pitch = px;
   return src;
}

/* Hand the volatile partition back and drop every pointer into it.  Returns
 * whether there was anything to release, because the standby ladder logs the
 * rung only when it actually descends one.
 *
 * Safe after the ME has stopped or acknowledged its cache flush. The OS owns
 * the 4 MB pool
 * at 0x08400000 and may re-hand it while we sleep, so a resume that skipped
 * re-locking would rebuild onto a partition we no longer held -- which is a
 * plausible way to reset the machine, and the machine did reset.  The full
 * history is at the call site in me_standby_down(). */
/* The release itself, with no question asked of the engine: for a caller
 * that has already made sure the ME cannot touch the pool any more
 * (me_rend_release_vmem below), or that must give it back regardless
 * (me_standby_down's failed sync, which leaves the module loaded so the
 * kernel's suspend parks the ME through our sysevent slot). */
static int me_rend_drop_vmem(void);
/* @extract:begin fn:me_rend_release_vmem */
static int me_rend_release_vmem(void)
{
   /* Disabling rendering (including a watchdog fallback) does not stop an
    * in-flight job or drop dirty work-cache lines. Do not return that memory
    * to the OS until the engine has relinquished it. Normally standby >= 2
    * has already unloaded the module, so no extra command is needed; nor does
    * one kept across the in-process return (me_host_retain did this).
    * (SLEEP_SIM_ME_SYNC_FAIL: the harness's sync that fails while asleep.) */
   if (g_mer_vmem && me_host_loaded() && !me_host_retained() &&
       (SLEEP_SIM_ME_SYNC_FAIL() || me_host_sync(50000u) != 0))
   {
      fe_evt("me_rend release_deferred reason=me_sync");
      return -1;
   }
   return me_rend_drop_vmem();
}
/* @extract:end fn:me_rend_release_vmem */

/* @extract:begin fn:me_rend_drop_vmem */
static int me_rend_drop_vmem(void)
{
#ifdef REWIND
   /* The other tenant first: after this the rewind holds no pointer into
    * the pool (its history is gone; it starts again after the wake). */
   psp_rewind_pool_lost();
   if (!g_mer_vmem)
   {
      vm_unlock();              /* the rewind may have been the only tenant */
      return 0;
   }
#endif
   if (!g_mer_vmem)
      return 0;
   MER_VM_UNLOCK();
   g_mer_vmem   = 0;
   g_mer_desc   = NULL;
   g_mer_cap[0] = g_mer_cap[1] = NULL;
   g_mer_stage[0] = g_mer_stage[1] = NULL;
#ifdef REWIND
   g_mer_stage_own[0] = g_mer_stage_own[1] = NULL;
#endif
   /* The ME's work area goes with it.  The module keeps no pointer to it
    * between renders, and nothing can be posted until me_rend_init has
    * carved a new one. */
   g_mer_work = NULL;
#if ME_MIDFRAME_LOG
   g_mer_log[0] = g_mer_log[1] = NULL;
   me_vram_shadow = NULL;
#endif
   g_mer_ready = g_mer_pending = g_mer_last_presented = -1;
   return 1;
}
/* @extract:end fn:me_rend_drop_vmem */

