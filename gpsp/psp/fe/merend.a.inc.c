/* The binding of a shortcut action (Mystery Gift's chord is read here; the
 * others arrive as ctl_shortcuts() bits). */
#define CTL_BIND(a)  ((unsigned)g_pcfg.controls.bind[(a)])

/* ------------------------------------------------------------------ video */

static const uint16_t *cur_frame;
static unsigned cur_w = FE_GBA_WIDTH, cur_h = FE_GBA_HEIGHT;
static size_t cur_pitch = FE_GBA_WIDTH * 2;

/* ---- ADR-0080 Stage 1: video staging on the Media Engine ---------------
 *
 * The pipeline the mission brief asked for, mapped onto the real seams:
 * the core's output buffer (`gba_screen_pixels`) is double-buffered by
 * flipping the core's own pointer between frames, so the main CPU never
 * copies a pixel.  Each completed frame is POSTED to the ME (non-blocking;
 * busy = drop, exactly the brief's collision policy), the ME does the
 * 480->512-stride staging copy into one of two main-RAM staging buffers,
 * and the GE draws the LAST COMPLETED staging buffer — one frame of
 * display latency, zero frames of main-CPU waiting.  The main-CPU path
 * (vid_draw_frame) is never removed: it serves until the first ME frame
 * completes and again forever if the watchdog trips.
 *
 * Coherency contract, one line each: SC writes core buffer cached ->
 * ranged writeback before post (in me_host_post_pitch_copy); ME reads and
 * writes uncached; GE reads physical RAM, which the ME's uncached stores
 * already reached. */
/* ADR-0080c: DEFER me_video ACTIVATION UNTIL A SESSION IS LIVE.
 *
 * me_video active on the HOST during np_start (ad-hoc group creation)
 * deterministically broke session bring-up (fs-me auto332/333, 2/2 on the
 * fixed EBOOT; the join was flawless).  The ME video path — buffer-pointer
 * flips and per-frame copy posts — must not run while the WLAN session is
 * being created.  It also has no reason to: single-player is unpaced with
 * ample headroom; the offload only matters once a paced session is up.
 *
 * So split "configured" from "active": `_cfg` is set once at boot (PRX up,
 * buffers allocated, key on); `g_me_video` (active offload) is switched on
 * only when g_net_up is true — which net_bringup sets AFTER np_start
 * succeeds — and switched back off (re-arming) when the session ends, so
 * the offload dodges EVERY session's np_start, not just the first.  A
 * watchdog/slow teardown is permanent for the boot (do not retry a dead
 * ME); a session-end teardown re-arms. */
int g_net_up;                         /* adhoc transport + netdrv live
                                              * (defined here, above
                                              * plat_video_frame, for the
                                              * ADR-0080c session gate)      */
static int g_me_video;                       /* actively offloading now      */
static int g_me_video_cfg;                   /* configured + ME ready        */
/* ADR-0080e: NO second core buffer.  We post the copy straight from the
 * core's own frame (`pix`); the ME reads it in ~3 ms (uncached 160 KB) while
 * the core does not re-render for ~16 ms, and the drop policy never lets two
 * copies overlap — so there is no overwrite race and the 77 KB second buffer
 * (which the host could not fit alongside np_start, auto346) is gone.  Two
 * 82 KB staging buffers remain (GE draws the last-completed while the ME
 * fills the next). */
static uint16_t *g_me_stage[2];              /* 256x161 RGB565 staging       */
static int g_me_stage_ready   = -1;          /* GE may draw this one         */
static int g_me_stage_pending = -1;          /* ME is filling this one       */
static unsigned g_me_vf, g_me_vdrops, g_me_census;
/* ADR-0080b: consecutive frames a posted copy failed to finish within its
 * frame.  This is the REAL "ME too slow for video" signal — distinct from
 * the host watchdog's heartbeat-freeze (ME wedged).  The first me_video
 * run (auto330) tore down at 8 because the OLD watchdog checked idle in the
 * same frame it posted the job — always busy, always a false miss. */
static unsigned g_me_vmiss;
#define ME_VMISS_LIMIT 8
static unsigned g_me_pw, g_me_ph;            /* frame dims for present (ADR-0082) */

/* `permanent` = 1 kills the offload for the rest of the boot (a dead/too-slow
 * ME); 0 just deactivates and RE-ARMS for the next session (ADR-0080c). */
static void me_video_teardown(const char *why, int permanent)
{
   FE_EVT_ONLY(why);   /* the reason is reported, not acted on */
   if (!g_me_video)
   {
      if (permanent)
         g_me_video_cfg = 0;
      return;
   }
   g_me_video = 0;
   if (permanent)
      g_me_video_cfg = 0;
   /* No gba_screen_pixels to restore — ADR-0080e never reassigns it. */
   g_me_stage_ready = g_me_stage_pending = -1;
   g_me_vmiss = 0;
   fe_evt("me_video off reason=%s frames=%u drops=%u rearm=%d", why, g_me_vf,
          g_me_vdrops, permanent ? 0 : 1);
}

/* ADR-0080d: allocate the offload buffers LAZILY, at session-up — NOT at
 * boot.  The host's np_start (ad-hoc group creation) needs more heap than
 * the join's, and pre-allocating 242 KB at boot starved it: the host failed
 * np_start even with me_video merely ARMED (session-gated, never active) —
 * fs-me auto343 disproved the "activity collides" theory.  Allocating after
 * np_start has taken its memory reverts the host to the exact heap profile
 * of the working me_bench arm at the one moment that matters.  On failure
 * (memory genuinely too tight post-session) the console just runs plain —
 * graceful, never fatal. */
static int me_video_alloc(void)
{
   if (g_me_stage[0] && g_me_stage[1])
      return 0;   /* already allocated (a prior session this boot) */
   g_me_stage[0] = (uint16_t *)memalign(64, 512 * 161);
   g_me_stage[1] = (uint16_t *)memalign(64, 512 * 161);
   if (!g_me_stage[0] || !g_me_stage[1])
   {
      free(g_me_stage[0]); free(g_me_stage[1]);
      g_me_stage[0] = NULL; g_me_stage[1] = NULL;
      return -1;
   }
   memset(g_me_stage[0], 0, 512 * 161);
   memset(g_me_stage[1], 0, 512 * 161);
   sceKernelDcacheWritebackInvalidateRange(g_me_stage[0], 512 * 161);
   sceKernelDcacheWritebackInvalidateRange(g_me_stage[1], 512 * 161);
   return 0;
}

/* Turn the offload ON for a live session (ADR-0080c/d/e).  Allocate the two
 * staging buffers now (post-np_start); no second core buffer to flip. */
static void me_video_activate(void)
{
   if (me_video_alloc() != 0)
   {
      fe_evt("me_video off reason=alloc session=up");
      g_me_video_cfg = 0;                 /* cannot get buffers: give up */
      return;
   }
   g_me_stage_ready   = -1;
   g_me_stage_pending = -1;
   g_me_vmiss         = 0;
   g_me_video = 1;
   fe_evt("me_video on stage_bytes=%u session=up", 512u * 161u);
}

/* ADR-0082: the offload is now a two-stage pipeline split across the loop.
 *
 *   me_video_post()  — end of retro_run (from plat_video_frame): hand this
 *                      frame to the ME to stage.  NO GE work here.
 *   me_video_present()— TOP of the next loop iteration, BEFORE retro_run:
 *                      retire the just-staged buffer and issue its GE list.
 *
 * Because the list is issued at the top, the GE rasterises through the whole
 * ~12 ms retro_run, so the pre-swap sync is ~0 even on heavy animation
 * frames (the old end-of-run issue left the GE only the vblank slack, which
 * is zero when a frame is over budget — that was the reappearing ~1.3 ms
 * `wait`).  Still ONE frame of latency: a frame posted at the end of run N
 * is presented at the top of run N+1 and shown at swap N+1.  Safe only
 * because the ME copy is now the ~0.7 ms cached read (ADR-0082 PRX change),
 * which finishes inside the vblank gap between post and present. */
static void me_video_post(const uint16_t *pix, unsigned w, unsigned h,
                          size_t pitch)
{
   g_me_vf++;
   g_me_pw = w;
   g_me_ph = h;

   /* Post the new frame if the ME is free (no job in flight). */
   if (g_me_video && pix && g_me_stage_pending < 0)
   {
      int nxt = (g_me_stage_ready == 0) ? 1 : 0;
      if (me_host_post_pitch_copy(pix, g_me_stage[nxt], h, w * 2,
                                  (unsigned)pitch, 512) == 0)
         g_me_stage_pending = nxt;
      else
         g_me_vdrops++;
   }
   else if (g_me_video && pix)
      g_me_vdrops++;

   if (++g_me_census >= 600)
   {
      g_me_census = 0;
      fe_evt("me_video frames=%u drops=%u vmiss=%u beat_ok=%d", g_me_vf,
             g_me_vdrops, g_me_vmiss, me_host_up());
   }
}

/* Called at the TOP of the main loop, before fe_host_run_frame (ADR-0082). */
static void me_video_present(void)
{
   if (!g_me_video)
      return;

   /* Retire the copy posted last iteration.  The cached-read copy (~0.7 ms)
    * completed during the vblank gap; if it somehow did not (a huge stall),
    * keep the previous frame on screen (a repeat) and count a miss. */
   if (g_me_stage_pending >= 0)
   {
      if (me_host_idle())
      {
         g_me_stage_ready   = g_me_stage_pending;
         g_me_stage_pending = -1;
         g_me_vmiss = 0;
      }
      else if (++g_me_vmiss >= ME_VMISS_LIMIT)
      {
         me_video_teardown("slow", 1);
         return;
      }
   }

   /* Issue the GE list now — it rasterises through the whole retro_run. */
   if (g_me_stage_ready >= 0)
   {
      vid_draw_prestaged(g_me_stage[g_me_stage_ready], g_me_pw, g_me_ph);
      g_drew = 1;
   }

   if (g_me_video && me_host_watchdog_frame())
      me_video_teardown("watchdog", 1);   /* ME wedged (heartbeat frozen) */
}

