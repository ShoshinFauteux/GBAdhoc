/* EVT sess_cost — where a wireless session's per-frame time actually goes
 * (ADR-0021).
 *
 * The measurement that forced this line into existence: solo, no wireless,
 * a PSP-1000 runs Emerald at 57.6-58.7 fps with everything rendered.  Bring
 * a session up and it collapses to 34-47.  Skip 97 % of rendered frames and
 * it only reaches 50-57 — so it was never about drawing.  ~20 fps, a third
 * of the frame budget, went somewhere no log line could name.
 *
 * Emitted once per adhoc_stats window (600 frames, ~10 s).  Everything is
 * an average per frame unless noted, in microseconds, integer:
 *
 *   clk_ns   cost of ONE sceKernelGetSystemTimeWide call, measured on this
 *            console at session start.  Multiply by the call counts below
 *            to price the instrumentation itself — and to price what the
 *            pre-ADR-0021 poll path was paying (poll_n - work) times it.
 *   frame    one main-loop iteration, vblank wait EXCLUDED.  This is the
 *            number that has to stay under 16.7 ms; the loop is
 *            vblank-locked, so a frame 1 us over budget costs a whole
 *            16.7 ms.  That quantisation is why a few ms of session work
 *            shows up as 20 fps.
 *   pump     the once-per-frame fe_np_pump.
 *   poll     n = poll_receive calls the core made; work = how many of them
 *            found something to do; then avg/max of the ones that did.
 *   rx       transport drain + parse + deliver into the core (both pumps).
 *   arq      ARQ timers, retransmit scan, keepalive, roster (both pumps).
 *   pdp      sceNetAdhocPdpSend: calls in the window, then MEAN AND MAX PER
 *            CALL (not per frame).  This is the number PPSSPP cannot tell
 *            us — there it is a host UDP sendto.
 *   enq      what the EMULATION thread paid per send: the ring copy plus a
 *            semaphore signal with the TX thread on, the whole PdpSend
 *            without it.  Mean/max per call.
 *   txq      queued / sent-inline / ring-was-full counts.
 *   evt      EVT+LOG lines written in the window, then total and max us
 *            paid BY THE EMULATION THREAD.  Before ADR-0024 that was the
 *            whole fprintf+fflush to the memory stick (field max 12002 us
 *            on the 1000, 12525 us on the 3000).  With log_thread=1 it is
 *            only vsnprintf + a ring copy + a semaphore signal, so this
 *            number collapsing to tens of us IS the proof the move worked.
 *   evtio    the memory stick itself, now on the writer thread: total us in
 *            the window / worst single flush / lines dropped because the
 *            ring was full / ring high-water bytes.  `drop` must stay 0; if
 *            it does not, FE_EVT_RING is too small for this log volume.
 */
/* Windows shorter than this are not divided into: opening or closing the
 * in-game menu pauses the core but not the wireless tick, so the window can
 * span a single frame and the per-frame averages come out as nonsense
 * (`pump=14674 arq=8773` in the field log — microseconds of a whole menu
 * charged to one frame).  `force` is teardown, which must always report. */
#define SESS_COST_MIN_FRAMES 60

/* The vblank target stays HERE: the present loop owns it, and the pacing
 * engine in ff_psp.c does not touch it. */
/* ADR-0047: the absolute vblank we intend to present the next frame on.
 * Paced against sceDisplayGetVcount() so an overrunning frame consumes
 * its budget instead of adding a whole vblank on top of it. */
static unsigned g_vc_target;
#define PACE_VC_RESYNC_VB 8   /* arrears past this are forgiven, not repaid */

/* ---- ADR-0063: EVT frame_hist — the SHAPE of the frame-time distribution --
 *
 * Why this exists.  `EVT sess_cost frame=<mean>/<max>` reports a window mean
 * and an ALL-TIME max and nothing in between, and `EVT core_prof
 * core=<mean>/<winmax>/<max>` adds a per-window max.  Three sessions of work
 * (CLIFF, OVERNIGHT, FULLSPEED) have shown the failure at 45.00-59.73 is
 * probabilistic — 1.00 / 0.80 / 0.56 is a slope, not a step — so the quantity
 * that decides a run is how OFTEN a frame overruns its budget, and a mean
 * plus a max cannot see a change in that frequency.  A window whose one worst
 * frame is 42 ms is indistinguishable from a window with fifty of them.
 *
 * Cost: this is fed from the work_us the main loop ALREADY computes for
 * ADR-0021/ADR-0027 (`fe_np_prof_frame` / `pace_frame`), so it adds NO clock
 * read.  Per frame it is <= 11 compares and one increment.  At 59.73 fps that
 * is under 700 compares per wall second against a ~16.7 ms budget; the
 * profiler this replaces (`core_phase`) cost ~500 us PER FRAME (FULLSPEED
 * §20.4) and this is four orders of magnitude below that.
 *
 *   EVT frame_hist win=<frames> bud=<us> b=<b0>,..,<b10> ovb=<n> wmax=<us>
 *                  late=<n> forgive=<n> nudge=<n>
 *
 *   b[]      frame WORK time (vblank wait excluded — the same number
 *            `sess_cost frame=` averages), bucketed by the edges below.
 *            Deltas over the window, so the buckets sum to `win`.
 *   bud      the current pacing period in us (10^6 * 100 / pace target).
 *   ovb      frames whose work exceeded `bud`.  This is the headline: at
 *            59.73 the budget is 16740 us and the join's window MEAN is
 *            already 16911-17196, so a threshold fixed at 16.7 ms would be
 *            saturated — `ovb` follows the rate instead of assuming one.
 *   wmax     worst work time IN THIS WINDOW (`sess_cost frame=`'s max is
 *            all-time and latches on a boot frame; ADR-0028 was caught by
 *            exactly that and had to add a window max for the same reason).
 *   late     frames that arrived at the pacing wait ALREADY at or past
 *            `g_vc_target` — i.e. the frame consumed its whole vblank budget
 *            and we did not wait at all.  This is the over-budget count as
 *            the PACER sees it, in vblanks rather than in microseconds, and
 *            it needs no threshold at all.
 *   forgive  times the ADR-0047 arrears forgiveness fired
 *            (`vcount - target > PACE_VC_RESYNC_VB`): a stall so long the
 *            debt was written off.  Every one of these is a hard divergence
 *            from the peer's frame count.
 *   nudge    times the audio-ring high-water nudge (plan §4.4,
 *            `ring_w - ring_r > RING_HIGH_WATER`) added a whole extra vblank.
 *            At 59.73 that HALVES the frame's rate, and until this counter
 *            existed the nudge was completely unobservable — there is no
 *            other log line anywhere that reports the ring level.  It is
 *            listed as a suspect spike source and nobody has ever seen it
 *            fire or not fire.
 *
 * Bucket edges straddle BOTH budgets in play: 16740 us (59.73 fps) and
 * 33400 us (29.97 fps, the regression gate).  b5 is the "just over full
 * speed" bucket and b8 the "just over the gate" bucket, so one line reads
 * usefully at either rate.  The last bucket isolates the >100 ms save
 * frames, which are known and are not the target. */
#define FH_BINS 11
static const uint32_t g_fh_edge[FH_BINS - 1] = {
   10000, 12000, 14000, 15500, 16740, 20000, 25000, 33400, 50000, 100000
};
static unsigned g_fh_bin[FH_BINS];
static unsigned g_fh_n;           /* frames binned since the last emit      */
static unsigned g_fh_ovb;         /* frames over the CURRENT pacing budget */
static uint32_t g_fh_wmax;        /* worst work_us in the current window */
static unsigned g_fh_late;        /* pacing wait was already due          */
static unsigned g_fh_forgive;     /* ADR-0047 arrears written off         */
static unsigned g_fh_nudge;       /* audio-ring high-water extra vblank   */

/* Called from the main loop with the work time it already measured.  No
 * clock read, no division: the budget is recomputed only when the pace
 * target moves, which is at most once a second (ADR-0033's glide). */
static void frame_hist_note(uint32_t work_us)
{
   static unsigned cached_target;
   static uint32_t cached_bud = 16740;
   unsigned i;

   if (ff_pace_target_x100() != cached_target)
   {
      cached_target = ff_pace_target_x100();
      cached_bud    = cached_target
                    ? (uint32_t)(100000000ull / cached_target)
                    : 16740u;
   }
   for (i = 0; i < FH_BINS - 1; i++)
      if (work_us < g_fh_edge[i])
         break;
   g_fh_bin[i]++;
   g_fh_n++;
   if (work_us > cached_bud)
      g_fh_ovb++;
   if (work_us > g_fh_wmax)
      g_fh_wmax = work_us;
}

/* `win` is this line's OWN frame count, so b[] always sums to it.  It is not
 * necessarily `sess_cost win=`: sess_cost only exists while a session is up,
 * so the first frame_hist of a run also carries every frame since boot.
 * `swin` is sess_cost's count, printed so the two can be lined up. */
static void frame_hist_evt(unsigned swin)
{
   FE_EVT_ONLY(swin);
   unsigned bud = ff_pace_target_x100()
                ? (unsigned)(100000000ull / ff_pace_target_x100()) : 0u;
   FE_EVT_ONLY(bud);
   fe_evt("frame_hist win=%u swin=%u bud=%u "
          "b=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
          "ovb=%u wmax=%u late=%u forgive=%u nudge=%u",
          g_fh_n, swin, bud,
          g_fh_bin[0], g_fh_bin[1], g_fh_bin[2], g_fh_bin[3], g_fh_bin[4],
          g_fh_bin[5], g_fh_bin[6], g_fh_bin[7], g_fh_bin[8], g_fh_bin[9],
          g_fh_bin[10],
          g_fh_ovb, g_fh_wmax, g_fh_late, g_fh_forgive, g_fh_nudge);
   memset(g_fh_bin, 0, sizeof(g_fh_bin));
   g_fh_n = g_fh_ovb = g_fh_late = g_fh_forgive = g_fh_nudge = 0;
   g_fh_wmax = 0;
}

