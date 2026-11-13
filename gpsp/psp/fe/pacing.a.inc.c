/* ===========================================================================
 * LOW LATENCY (docs/LOW-LATENCY.md).  Three switches, one per mechanism, so
 * one binary carries every arm of the A/B.
 *
 * Where the shipping ME pipeline's latency came from, per emulated frame N
 * (input polled just after vblank V(k), frame N emulated in interval k):
 *
 *   vcount 160 of k     render of N posted to the ME, done ~3 ms later
 *   vcount 160 of k+1   ...but only RETIRED here, by the next post
 *   loop top of k+2     blitted (the loop-top present shows the ready stage)
 *   after V(k+3)        vid_swap(): SetFrameBuf NEXTFRAME
 *   V(k+4)              latched -- a NEXTFRAME flip issued just AFTER a
 *                       vblank waits for the following one
 *
 * i.e. latch - poll = 4 vblanks, where the CPU renderer (and the GB/GBC
 * cores) have 2 and the floor for "emulate the frame, then show it" is 1.
 *
 *   flip_early      (tier 0a, every path) submit the flip as soon as the
 *                   frame is complete and the GE idle, BEFORE the vblank
 *                   wait, so NEXTFRAME latches on the vblank we are waiting
 *                   for.  -1 vblank on every path; no CPU cost (the GE is
 *                   polled, never waited on, inside a wait that was idle).
 *   me_early_retire (tier 0b, ME) retire a finished render at the loop top
 *                   instead of at the next post.  -1 vblank; nothing waits.
 *   me_lowlat       (tier 1, ME) present the render in the wait of the
 *                   interval it was emulated in and flip before that
 *                   interval's vblank: latch - poll = 1.  Needs the render
 *                   and the blit to finish before the vblank; a frame that
 *                   cannot is shown one vblank later, as with tier 0, and
 *                   the mode drops back to tier 0 (see "TIER 1 POLICY").
 *
 * TIER 1 POLICY (claude/lowlat-t1).  Moving between tier 0 (2 vblanks) and
 * tier 1 (1 vblank) moves the picture by one vblank, and that is never free
 * on its own: leaving shows one frame twice (the miss that forced it), and
 * entering at an arbitrary frame would show one frame never (the prototype
 * superseded the frame the loop top had just retired).  So:
 *
 *   ENTER only in an iteration whose vblank wait is already two vblanks
 *   long -- the audio-ring nudge (`want` 2), which repeats a frame in tier 0
 *   anyway (~1 per 5 s at 1x: the PSP refreshes at 59.94, the GBA at 59.73).
 *   The frame the loop top retired is flipped early and latches on the first
 *   vblank as usual; this iteration's own render is then presented and
 *   flipped before the SECOND one.  Every frame is shown, none twice: the
 *   repeat the nudge would have made is what the latency step spends.  No
 *   frame is ever dropped to get into tier 1.
 *
 *   ...and only when the game has room for it: of the last LL_WIN measured
 *   frames at most LL_WIN_MISS would have missed (95 %), none of the last
 *   LL_CLEAN, and at least `me_lowlat_recover` frames since the last exit.
 *   A frame "would have made it" when its render retired before the vblank
 *   after its own input poll with at least `need` us to spare.
 *
 *   LEAVE on the first frame that misses (no choice: it is already late).
 *   That repeats one frame -- the repeat the entry saved -- so over any
 *   stretch tier 1 shows exactly the frames tier 0 shows, give or take one
 *   repeat moved in time.  Every frame is still emulated at the same vblank:
 *   only WHEN a finished picture is flipped changes, never the guest, so
 *   replays, vhash and link lockstep are untouched.
 *
 *   me_lowlat 2 also asks for that slot instead of waiting up to ~5 s for
 *   it (ll_want_slot): once eligible, the nudge is taken as soon as the ring
 *   is within one frame of audio of the high-water mark.  That moves a nudge
 *   earlier, it never adds one -- every nudge holds back one frame of
 *   production, so their long-run rate is the clock mismatch's alone -- and
 *   it can happen at most once per exit.  The ring sits at most one frame
 *   (~17 ms) lower until the next nudge is due.
 *
 *   `need` is measured per console: an EMA of the blit + OSD time on the GE
 *   (present to GE idle) plus four mean deviations, one poll period, and
 *   `me_lowlat_margin_us`.  A GE that finishes after the vblank widens it.
 *
 * Off under FF, rewind, the FF draw gate and any link or wireless session
 * (link_session_any): there the pacing is the link's and nothing is gained
 * by proving it safe.
 * ======================================================================== */
/* @extract:begin lowlat:defaults */
#ifndef PLAY_ME_LOWLAT
#define PLAY_ME_LOWLAT 2     /* tier 1 mode 2 on by default (ME_LOWLAT=0 builds it off) */
#endif
static int      g_flip_early       = 1;   /* `flip_early`      */
/* FF FLIP PACING (`ff_flip_pace`, docs/FF-PRESETS.md): never replace a flip
 * the LCD has not latched yet.  Bit 0 (3x only): the paced branch's early
 * flip (tier 0a) waits for the previous one to latch -- under 3x a loop is about one
 * vblank long, and an early flip landing in the interval the previous one is
 * still pending in replaced it (a dropped picture, the next one 6 frames on).
 * Bit 1: the draw gate's presets hold a finished picture in the back buffer
 * until the previous flip has been on screen for the preset's interval (1
 * vblank for Smooth, 2 for Unlimited), instead of flipping it the moment the
 * loop finishes.  The gate's wall-clock grid (16667 us) beats against the
 * LCD (16683 us); without the hold, the phase drifts through the vblank edge
 * every ~17 s and for seconds at a time two flips land in one interval (one
 * never shown) and the next has none.  Never blocks: the loop keeps
 * emulating, the held picture goes out on the first loop that may flip it,
 * and a newer render that is presented meanwhile replaces it.
 * Bit 2: 3x presents the way Smooth does -- the draw gate at 60 pictures a
 * second plus bit 1's hold -- under a ceiling of 3 frames per vblank
 * (ffh_cap3), instead of one picture per 3-frame loop.  A console that runs
 * 3x in full sees the same 180 fps and one picture per vblank as before.
 * One that cannot (FireRed at 222 MHz: ~125 fps = 42 loops a second) used to
 * flip each loop as it finished, 42 pictures a second on a 59.94 Hz LCD, so
 * every picture stayed up 1 or 2 vblanks in a near 50/50 mix; 4.1.0's slower
 * ~103 fps was 34 a second, mostly 2-vblank holds, a steadier rhythm.  Here
 * it is one picture per vblank, each ~2 frames on, at the same speed.
 * 1.5x and 2x take the same road under their own ceiling (1.5x: 3 frames per
 * 2 vblanks, ff_gate.h), and on pace draw by the ceiling instead of the gate:
 * the last frame of each vblank's quota, so the picture advances 2, 1, 2, 1
 * (1.5x) or 2, 2 (2x) game frames a vblank (ffh_cap_draw). */
#ifndef PLAY_FF_FLIP_PACE
#define PLAY_FF_FLIP_PACE 0
#endif
/* Small data, so the layout pin's .data/.bss sets do not move (LAYOUT-PINNING.md). */
static int      g_ff_flip_pace __attribute__((section(".sdata"))) = PLAY_FF_FLIP_PACE;
static int      g_ffh_held __attribute__((section(".sbss")));  /* back buffer holds an unflipped picture */
static unsigned g_ffh_holds __attribute__((section(".sbss")));
static unsigned g_ffh_waits __attribute__((section(".sbss")));
static unsigned g_ffh_vb __attribute__((section(".sbss")));
static int      g_ffh_3x __attribute__((section(".sbss")));    /* bit 2 in force this loop (any multiplier) */
static int      g_ffc_on __attribute__((section(".sbss")));    /* multiplier ceiling anchored */
static unsigned g_ffc_vc __attribute__((section(".sbss")));
static unsigned g_ffc_f __attribute__((section(".sbss")));
static unsigned g_ffc_waits __attribute__((section(".sbss")));
static unsigned g_ffc_bvc __attribute__((section(".sbss")));   /* vcount after the ceiling's last block */
static int      g_ffc_q __attribute__((section(".sbss")));     /* the ceiling chose the last draw */
static unsigned g_ffm_acc __attribute__((section(".sbss")));   /* paced multiplier: tenths of a frame owed */
static int      g_mer_early_retire = 1;   /* `me_early_retire` */
/* LATE PRESENT (tier 0b, `me_late_present`, default 1): see dfr_present(). */
#ifndef PLAY_ME_LATE_PRESENT
#define PLAY_ME_LATE_PRESENT 1
#endif
static int      g_dfr_cfg __attribute__((section(".sdata"))) = PLAY_ME_LATE_PRESENT;
static int      g_dfr __attribute__((section(".sbss")));      /* this loop's present waits for its render */
static unsigned g_dfr_n __attribute__((section(".sbss")));    /* loop tops that deferred */
static unsigned g_dfr_late __attribute__((section(".sbss")));  /* ...whose render was still not there */
static int      g_lf_hold __attribute__((section(".sbss")));  /* this wait's early flip is held */
static unsigned g_lf_n __attribute__((section(".sbss")));     /* waits that held it */
static unsigned g_lf_caught __attribute__((section(".sbss"))); /* ...and caught up in a nudge's 2nd interval */
static int      g_lf_behind __attribute__((section(".sbss")));  /* the picture runs a vblank late */
static unsigned g_lf_pull __attribute__((section(".sbss")));    /* nudges taken early to catch up */
static int      g_ll_cfg = PLAY_ME_LOWLAT; /* `me_lowlat`      */
static unsigned g_ll_recover   = 60;      /* `me_lowlat_recover` (frames) */
static unsigned g_ll_margin_us = 250;     /* `me_lowlat_margin_us` */
#define LL_WIN       128u   /* measured frames in the engage window */
#define LL_WIN_MISS  6u     /* at most this many would-miss in it (95.3 %) */
#define LL_CLEAN     32u    /* ...and none in the most recent this many */
static int      g_ll_mode_h;     /* persistent: same-interval mode engaged */
static int      g_ll_iter_h;     /* this loop iteration presents in its wait */
static uint32_t g_ll_win[LL_WIN / 32u];  /* 1 = that frame would have missed */
static unsigned g_ll_win_pos, g_ll_win_n, g_ll_win_miss, g_ll_clean;
static unsigned g_ll_cool;       /* frames since the last exit */
static unsigned g_ll_blit_ema = 2000;  /* us, blit + OSD on the GE (EMA)   */
static unsigned g_ll_blit_dev = 250;   /* us, its mean absolute deviation  */
static unsigned g_ll_hit, g_ll_miss_me, g_ll_miss_ge, g_ll_to_h, g_ll_to_b;
static unsigned g_ll_would, g_ll_wouldnt;
static unsigned g_ll_slot, g_ll_slot_fail, g_ll_eng, g_ll_lead_min = ~0u;
static unsigned g_ll_pull;       /* nudges taken early for an entry (mode 2) */

/* vblank clock: the last vblank start the wait loop woke on */
static unsigned g_vb_us, g_vb_vc;

/* Pipeline telemetry: every number below is "vblank count at the moment the
 * NEXTFRAME flip latches" minus "vblank count at the input poll of the loop
 * iteration that emulated the frame".  1 is the floor. */
static unsigned g_lat_poll_vc;
static unsigned g_mer_stage_pvc[2];   /* poll vcount of each stage's frame */
static unsigned g_lat_seq;            /* loop iterations (input polls)     */
static unsigned g_mer_stage_seq[2];   /* ...the one each stage was posted in */
/* FILTERS V2 colour preview: per stage, the colour mode it was drawn in, the
 * capture it was drawn from, and the ME's checksum of it. */
static unsigned g_mer_stage_mode[2], g_mer_stage_cap[2], g_mer_stage_sum[2];
static unsigned g_bb_pvc;             /* ...of the back buffer's frame     */
static int      g_bb_new;             /* back buffer holds an unshown frame */
static unsigned g_lat_hist[8], g_lat_over, g_lat_early, g_lat_post, g_lat_hold;
static unsigned g_lat_sub_vc, g_lat_check, g_lat_l1, g_lat_l0, g_lat_immed;
static unsigned g_lat_lead_n, g_lat_lead_us, g_lat_lead_min = ~0u;
static unsigned g_lat_hc_n, g_lat_hc_sum, g_lat_vbhc_sum, g_lat_vbhc_n;
static unsigned g_lat_age_us, g_lat_age_n, g_lat_iter;
static unsigned g_lat_vc0;            /* vcount at the last census: `vb=` */
/* INPUT FRESHNESS (harness only, both default 0).  PPSSPP samples the pad
 * at vblank END (Core/HW/Display.cpp DisplayFireVblankEnd -> __CtrlVblank),
 * so a poll made the instant the vblank wait returns reads the sample taken
 * a whole frame earlier: `age_us` reads ~15.9 ms there.  Whether the real
 * firmware does the same is what these exist to find out.
 *   input_fresh = 1  if the sample predates this vblank by > 2 ms, re-peek
 *                    in 50 us steps (<= 1.5 ms) until a newer one lands.
 *   ctrl_cycle  = N  sceCtrlSetSamplingCycle(N) (5555..20000 us): timer
 *                    sampling instead of vblank sampling. */
static int      g_in_fresh, g_ctrl_cycle;
static unsigned g_in_fresh_n, g_in_fresh_us, g_in_fresh_got;

/* @extract:end lowlat:defaults */
static int mer_rewind_busy(void);     /* below, beside the rewind's state */
static int link_session_any(void);    /* below, beside the link UI */

/* @extract:begin ff:mer_retire_early_active */
/* @extract:begin lowlat:mer_retire_early_active */
static int mer_retire_early_active(void)
{
   if (g_ff_mult || g_ff_uncapped)
      return 0;
   return g_mer_sameframe || g_mer_early_retire || g_ll_cfg;
}

/* @extract:end ff:mer_retire_early_active */
static int mer_lowlat_active(void)
{
   return g_ll_cfg && g_me_rend && !g_ff_mult && !g_ff_uncapped &&
          !g_ff_gate && !mer_rewind_busy() && !link_session_any();
}

/* Microseconds to the next vblank start, extrapolated from the last one the
 * wait loop woke on (59.94 Hz = 16683.33 us).  Time only: it does not
 * depend on which edge sceDisplayGetVcount() counts.  Huge when not
 * anchored. */
#define VB_PERIOD_US(n) ((n) * 16683u + (n) / 3u)
static int us_to_vblank(unsigned now)
{
   unsigned el, n;
   if (!g_vb_us)
      return 1 << 30;
   el = now - g_vb_us;
   n  = el / 16683u + 1u;
   if (VB_PERIOD_US(n - 1u) > el)      /* the /3 term near a boundary */
      n--;
   return (int)(VB_PERIOD_US(n) - el);
}
/* @extract:end lowlat:mer_retire_early_active */
