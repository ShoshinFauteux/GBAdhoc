/* ---- ADR-0064: EVT preempt — HOW OFTEN IS THE EMULATION THREAD ACTUALLY
 *      TAKEN OFF THE CPU, AND WHERE IN THE FRAME -------------------------
 *
 * Why this exists.  FULLSPEED §27.2 measured that `emu_prio = 0x2B` costs the
 * join 3.5-4.0 ms of frame time (`core` 11.9-13.5 -> 16.2-17.2 ms) and called
 * it "pure preemption overhead".  That is an INFERENCE from a difference of
 * two means; nobody has ever counted a single preemption on this rig.  Every
 * design that follows from it — the protected-window idea included — depends
 * on whether the emulation thread is interrupted twice per frame or fifty
 * times, and on whether those interruptions land inside `retro_run` or in the
 * frontend.  Two prior recommendations in this project were built on
 * unverified numbers and both were wrong, so this counts before anything is
 * designed.
 *
 * How.  `sceKernelReferThreadRunStatus()` returns the kernel's own
 * `threadPreemptCount` (times this thread was taken off the CPU by a
 * higher-priority THREAD becoming runnable) and `intrPreemptCount` (times an
 * INTERRUPT took it off).  Sampling those at four points in the loop splits
 * one frame into four accounted regions:
 *
 *   pre    frame start -> just before the core-run block  (input, autopilot,
 *          UI, fast-forward bookkeeping)
 *   core   the `fe_host_run_frame()` block itself, plus pace_burn
 *   post   after the core -> the point where `work_us` is taken (dump
 *          checks, blit_prof, autopilot status)
 *   wait   the point where `work_us` is taken -> the NEXT frame's start.
 *          This is the pacing/vblank wait, i.e. the time we WANT the radio to
 *          run in.  Preemptions here are free; preemptions in the other three
 *          are the 3.5-4.0 ms.
 *
 * `pre`+`core`+`post` is exactly the region `work_us` measures, so the counts
 * and the microseconds are commensurable.
 *
 * Cost and honesty about it.  Four kernel calls plus eight clock reads per
 * frame, all of which are self-timed into `self=` and reported, so the
 * instrument's own price is in its own output.  It is OFF unless
 * `preempt_prof = 1` is set in the harness ini: with the key absent not one
 * of these functions is entered and the loop is byte-for-byte the previous
 * behaviour.
 *
 *   EVT preempt n=<frames> t=<pre>/<core>/<post>/<wait>
 *               i=<pre>/<core>/<post>/<wait> wmax=<n> imax=<n>
 *               h=<h0>,..,<h6> rel=<n> self=<us> calls=<n> prio=0x<xx>
 *
 *   t/i    window TOTALS (not per-frame) of thread / interrupt preemptions in
 *          each region.  Divide by `n` for the per-frame figure.
 *   wmax   worst single frame's thread-preemption count over pre+core+post.
 *   imax   the same for interrupt preemptions.
 *   h[]    histogram of per-frame thread preemptions over pre+core+post,
 *          bins 0 / 1-2 / 3-4 / 5-8 / 9-16 / 17-32 / 33+.  A mean cannot tell
 *          "every frame loses 8 slices" from "one frame in ten loses 80", and
 *          those two want different fixes.
 *   rel    releaseCount delta — times the thread was released from a wait.
 *   self   microseconds this instrument itself spent inside the kernel calls.
 *   prio   the emulation thread's priority at emit time (so a build running
 *          the ADR-0065 boost window shows which side of the driver it is on).
 */
/* ADR-0066, see the main loop.  Frames between dump-marker polls; 0 = never.
 * ADR-0069 moved the default to 0 — L+R+SELECT replaces the marker file and
 * costs nothing.  Set `dump_marker_poll = 60` for the pre-0069 behaviour. */
static int      g_dump_marker_every = PLAY_DUMP_MARKER;

/* ADR-0069: the screenshot chord, and the yield the marker stat used to pay
 * for by accident.  `g_yield_us` microseconds of sceKernelDelayThread once per
 * `g_yield_every` frames; 0 us = off, which is the default in both builds. */
static int      g_dump_chord_pending;
static int      g_yield_us;
static int      g_yield_every = 60;

#define PE_BINS 7
static SceUID   g_emu_thid = -1;
static int      g_pe_on;            /* harness key `preempt_prof`           */
static unsigned g_pe_n;
static unsigned g_pe_t[4], g_pe_i[4];   /* pre / core / post / wait        */
static unsigned g_pe_wmax, g_pe_imax;
static unsigned g_pe_hist[PE_BINS];
static unsigned g_pe_rel, g_pe_self_us, g_pe_calls;
static unsigned g_pe_last_t, g_pe_last_i, g_pe_last_r;
static int      g_pe_have;
static unsigned g_pe_fr_t, g_pe_fr_i;   /* this frame's work-region totals */

/* Sample once and attribute the delta since the previous sample to `slot`.
 * slot 0..3 = pre/core/post/wait; slot < 0 = prime the counters only. */
static void preempt_mark(int slot)
{
   SceKernelThreadRunStatus st;
   uint64_t t0;
   int rc;

   if (!g_pe_on || g_emu_thid < 0)
      return;
   memset(&st, 0, sizeof(st));
   st.size = sizeof(st);
   t0 = net_now_us();
   rc = sceKernelReferThreadRunStatus(g_emu_thid, &st);
   g_pe_self_us += (unsigned)(net_now_us() - t0);
   g_pe_calls++;
   if (rc < 0)
   {
      g_pe_on = 0;                 /* never let an instrument fail a run */
      fe_evt("preempt disabled rc=%d", rc);
      return;
   }
   if (g_pe_have && slot >= 0 && slot < 4)
   {
      unsigned dt = st.threadPreemptCount - g_pe_last_t;
      unsigned di = st.intrPreemptCount   - g_pe_last_i;
      g_pe_t[slot] += dt;
      g_pe_i[slot] += di;
      g_pe_rel     += st.releaseCount - g_pe_last_r;
      if (slot != 3)               /* 0..2 are the work region */
      {
         g_pe_fr_t += dt;
         g_pe_fr_i += di;
      }
   }
   g_pe_have   = 1;
   g_pe_last_t = st.threadPreemptCount;
   g_pe_last_i = st.intrPreemptCount;
   g_pe_last_r = st.releaseCount;
}

/* Called once per frame, after the `post` mark, to close out the histogram. */
static void preempt_frame_end(void)
{
   unsigned v;
   if (!g_pe_on)
      return;
   v = g_pe_fr_t;
   g_pe_n++;
   if (v > g_pe_wmax) g_pe_wmax = v;
   if (g_pe_fr_i > g_pe_imax) g_pe_imax = g_pe_fr_i;
   g_pe_hist[v == 0 ? 0 : v <= 2 ? 1 : v <= 4 ? 2 : v <= 8 ? 3 :
             v <= 16 ? 4 : v <= 32 ? 5 : 6]++;
   g_pe_fr_t = g_pe_fr_i = 0;
}

static void preempt_evt(void)
{
   SceKernelThreadRunStatus st;
   int prio = 0;
   FE_EVT_ONLY(prio);
   if (!g_pe_on)
      return;
   memset(&st, 0, sizeof(st));
   st.size = sizeof(st);
   if (sceKernelReferThreadRunStatus(g_emu_thid, &st) >= 0)
      prio = st.currentPriority;
   fe_evt("preempt n=%u t=%u/%u/%u/%u i=%u/%u/%u/%u wmax=%u imax=%u "
          "h=%u,%u,%u,%u,%u,%u,%u rel=%u self=%u calls=%u prio=0x%02X",
          g_pe_n,
          g_pe_t[0], g_pe_t[1], g_pe_t[2], g_pe_t[3],
          g_pe_i[0], g_pe_i[1], g_pe_i[2], g_pe_i[3],
          g_pe_wmax, g_pe_imax,
          g_pe_hist[0], g_pe_hist[1], g_pe_hist[2], g_pe_hist[3],
          g_pe_hist[4], g_pe_hist[5], g_pe_hist[6],
          g_pe_rel, g_pe_self_us, g_pe_calls, prio);
   memset(g_pe_t, 0, sizeof(g_pe_t));
   memset(g_pe_i, 0, sizeof(g_pe_i));
   memset(g_pe_hist, 0, sizeof(g_pe_hist));
   g_pe_n = g_pe_wmax = g_pe_imax = g_pe_rel = g_pe_self_us = g_pe_calls = 0;
}

