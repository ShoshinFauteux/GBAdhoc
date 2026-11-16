/* ---- LOW LATENCY: the main loop's half (switches and rationale at
 * g_flip_early; docs/LOW-LATENCY.md) ------------------------------------ */
static int mer_rewind_busy(void)
{
#ifdef REWIND
   return g_rw_showing || (g_rw_on && fe_host_rewind_busy()) ||
          fe_host_rewind_replaying();
#else
   return 0;
#endif
}

/* THE swap of a loop iteration, with its bookkeeping.  `early` = submitted
 * before the vblank the loop is waiting for. */
/* @extract:begin lowlat:lat_swap */
static int lat_swap(int early)
{
   unsigned vc, now;
   int r;

   UISCRIPT_DUMP(); /* @lab */ LAB_HOOK(lab_blink_draw();)
   if (g_wake_hold)
      return 0;
   vc  = sceDisplayGetVcount();
   now = (unsigned)sceKernelGetSystemTimeLow();
   if (g_lat_check && (int)(vc - g_lat_sub_vc) >= 1)
   {
      /* (the loop-top check never sees a post-vblank swap's latch) */
      if (vid_flip_latched())
         g_lat_l1++;
      else
         g_lat_l0++;
      g_lat_check = 0;
   }
   r   = vid_swap();
   if (!r)
   {
      g_lat_hold++;            /* nothing drawn: the screen holds */
      return 0;
   }
   g_lat_sub_vc = vc;
   g_lat_check  = 1;
   if (vid_flip_latched())
      g_lat_immed++;           /* nonzero would mean NEXTFRAME is not deferred */
   if (early)
   {
      int lead = us_to_vblank(now);
      g_lat_early++;
      if (lead >= 0 && lead < (1 << 20))
      {
         g_lat_lead_n++;
         g_lat_lead_us += (unsigned)lead;
         if ((unsigned)lead < g_lat_lead_min)
            g_lat_lead_min = (unsigned)lead;
      }
      g_lat_hc_n++;
      g_lat_hc_sum += (unsigned)sceDisplayGetCurrentHcount();
   }
   else
      g_lat_post++;
   if (g_bb_new)
   {
      unsigned d = vc + 1u - g_bb_pvc;   /* latch vblank - poll vblank */
      g_lat_hist[d < 7u ? d : 7u]++;
      g_bb_new = 0;
   }
   return r;
}

/* FF FLIP PACING (see g_ff_flip_pace).  1 = a flip now would latch less than
 * `n` vblanks after the last one (n = 1: the last one has not latched yet).
 * g_lat_sub_vc is read just before the submit: a vblank inside that window
 * (microseconds) makes the last flip latch one later than assumed here. */
/* @extract:begin ff:ffh_cadence */
static __attribute__((noinline)) int ffh_too_soon(unsigned n)
{
   return (int)(sceDisplayGetVcount() - (g_lat_sub_vc + n)) < 0;
}

/* The uncapped branch: hold this loop's picture instead of flipping it? */
static int ffh_hold(void)
{
   unsigned n;
   if (!(g_ff_flip_pace & 2) || !g_ff_gate)
      return 0;
   n = pcfg_ff_mode() == PCFG_FF_UNLIMITED ? 2u : 1u;
   if (!ffh_too_soon(n))
      return 0;
   if (!g_ffh_held)
      g_ffh_holds++;
   return 1;
}

/* FF FLIP PACING bit 2: the multiplier ceiling (ff_gate.h) for 1.5x, 2x and
 * 3x run through the gate.  At most ff_mult_x10 / 10 frames per vblank since
 * the anchor (the current interval's included); ahead of that, block on the
 * vblank, as the paced loop does.  Behind by more than 8 vblanks' worth:
 * re-anchor (forgive, never sprint).  For 3x this is the arithmetic of the
 * 3x-only ceiling it replaces. */
static unsigned ffh_cap_mult(void)
{
   unsigned m = (unsigned)g_pcfg.ff_mult_x10;
   return m ? m : 30u;   /* never 0 here: 0 would never stop waiting */
}

static __attribute__((noinline)) void ffh_cap(void)
{
   unsigned m = ffh_cap_mult();
   unsigned f = fe_host_frame_count(), vc = sceDisplayGetVcount();
   int blocked = 0;
   if (!g_ffc_on || ff_cap_lost(m, f - g_ffc_f, vc - g_ffc_vc))
   {
      g_ffc_on = 1;
      g_ffc_vc = vc;
      g_ffc_f  = f;
      g_ffc_bvc = vc - 2u;   /* not "just blocked": the gate draws first */
      return;
   }
   while (ff_cap_over(m, f - g_ffc_f, sceDisplayGetVcount() - g_ffc_vc) >= 0)
   {
      sceDisplayWaitVblankStart();
      g_ffc_waits++;
      blocked = 1;
   }
   if (blocked)
      g_ffc_bvc = sceDisplayGetVcount();
}

/* The draw decision of a frame under the multiplier ceiling, at its start
 * (ff_gate.h THE DRAW RULE BELOW 3x).  1.5x and 2x, on pace -- the anchor is
 * set, every interval since it had its quota, and the ceiling blocked into
 * this interval or the one before -- draw the frame the ceiling will stop
 * after, so each vblank gets the newest frame of its interval.  Otherwise
 * (3x always, so 3x is unchanged) the draw gate decides; leaving the
 * ceiling's rule re-arms the gate, so its first frame is drawn at once
 * instead of waiting out a boundary the ceiling's frames used. */
static __attribute__((noinline)) int ffh_cap_draw(void)
{
   unsigned m = ffh_cap_mult(), vc = sceDisplayGetVcount();
   unsigned df = fe_host_frame_count() - g_ffc_f, dv = vc - g_ffc_vc;
   if (m < 30u && g_ffc_on && ff_cap_on_pace(m, df, dv) &&
       vc - g_ffc_bvc <= 1u)
   {
      g_ffc_q = 1;
      return ff_cap_last(m, df, dv);
   }
   if (g_ffc_q)
   {
      g_ffc_q = 0;
      ff_gate_arm(&g_ff_gate_st, g_ff_gate_st.period_us);
   }
   return ff_gate_frame(&g_ff_gate_st, net_now_us());
}
/* @extract:end ff:ffh_cadence */

/* The paced branch's vblank wait, in three calls: begin, one step per pass
 * of the wait loop (1 = poll again shortly instead of blocking on the
 * vblank), end.  Single thread, single owner: the main loop. */
enum { LL_OFF, LL_WAIT_ME, LL_WAIT_GE, LL_DONE };
#define LAT_POLL_US 150u
static int      g_lw_swapped, g_lw_ll, g_lw_measure, g_lw_slot, g_lw_entering;
static unsigned g_lw_t;

/* Microseconds before the vblank by which a tier-1 present must START for
 * its flip to be submitted in time: the measured blit + OSD on the GE, four
 * mean deviations of it, one poll period (the GE is polled, never waited
 * on), and the configured margin.  See TIER 1 POLICY at g_flip_early. */
static int ll_need_us(void)
{
   unsigned n = g_ll_blit_ema + 4u * g_ll_blit_dev + LAT_POLL_US +
                g_ll_margin_us;
   if (n < 800u)
      n = 800u;
   if (n > 8000u)
      n = 8000u;
   return (int)n;
}

static void ll_blit_note(unsigned d)
{
   unsigned dev;
   if (d > 20000u)
      return;                   /* a sleep or a preemption, not a blit */
   dev = d > g_ll_blit_ema ? d - g_ll_blit_ema : g_ll_blit_ema - d;
   g_ll_blit_ema = (g_ll_blit_ema * 7u + d) / 8u;
   g_ll_blit_dev = (g_ll_blit_dev * 7u + dev) / 8u;
}

static void ll_win_reset(void)
{
   memset(g_ll_win, 0, sizeof(g_ll_win));
   g_ll_win_pos = g_ll_win_n = g_ll_win_miss = g_ll_clean = 0;
}

/* One measured frame into the engage window (miss = it would not have
 * made, or did not make, the vblank after its own input poll). */
static void ll_sample(int miss)
{
   unsigned w = g_ll_win_pos >> 5;
   uint32_t b = 1u << (g_ll_win_pos & 31u);
   if (g_ll_win_n == LL_WIN)
   {
      if (g_ll_win[w] & b)
         g_ll_win_miss--;
   }
   else
      g_ll_win_n++;
   if (miss)
   {
      g_ll_win[w] |= b;
      g_ll_win_miss++;
      g_ll_clean = 0;
   }
   else
   {
      g_ll_win[w] &= ~b;
      g_ll_clean++;
   }
   g_ll_win_pos = (g_ll_win_pos + 1u) % LL_WIN;
}

static int ll_eligible(void)
{
   return g_ll_win_n == LL_WIN && g_ll_win_miss <= LL_WIN_MISS &&
          g_ll_clean >= LL_CLEAN && g_ll_cool >= g_ll_recover;
}

/* LAYOUT (code-health B-4): a player build inlines this everywhere, as
 * 5.0.0-beta.5 did.  B-3's frame-boundary code tipped GCC into splitting it
 * (ll_to_b.part.0, 36 B), a function the pinned warm tail had no room for in
 * the release: the 16 per-frame rows after it moved 36 B.  The rig builds
 * always had the split and keep it (their layout has its row).  (A separate
 * declaration, so the definition's text is the one the host test's mutants
 * patch: tools/tests/test_lowlat_policy.py.) */
#ifndef GPSP_PERF_RIG
static inline __attribute__((always_inline)) void ll_to_b(void);
#endif
static void ll_to_b(void)
{
   if (g_ll_mode_h)
   {
      g_ll_mode_h = 0;
      g_ll_to_b++;
      g_ll_cool = 0;
   }
}

/* THIS iteration's own render: posted after this iteration's input poll,
 * retired, and not yet on screen.  (A render from an earlier iteration is
 * never what tier 1 presents in the wait.) */
static int ll_own_ready(void)
{
   return g_mer_ready >= 0 && g_mer_ready != g_mer_last_presented &&
          g_mer_stage_seq[g_mer_ready] == g_lat_seq;
}

static int ll_own_posted(void)
{
   return ll_own_ready() ||
          (g_mer_pending >= 0 && g_mer_stage_seq[g_mer_pending] == g_lat_seq);
}

/* The blit + OSD of this iteration's render (tier 1, or the entry slot). */
static void ll_present(unsigned now)
{
   me_rend_present();
#ifdef REWIND
   if (g_drew && (g_rw_on || g_rwh.denied))
      psp_rewind_hud();
#endif
   if (g_drew)
      osd_draw();
   g_lw_t  = now;
   g_lw_ll = LL_WAIT_GE;
}

/* me_lowlat 2: take the audio-ring nudge now, as an entry slot (TIER 1
 * POLICY).  `level` = the ring level the pacer just read. */
static int ll_want_slot(unsigned level)
{
   return g_ll_cfg >= 2 && !g_ll_mode_h && level + in_rate / 60u > RING_HIGH_WATER &&
          mer_lowlat_active() && ll_own_posted() && ll_eligible();
}

/* `want`: vblanks this wait spans (2 on an audio-ring nudge). */
static __attribute__((noinline)) void lat_wait_begin(unsigned want)
{
   int active = mer_lowlat_active();

   g_lw_swapped  = 0;
   g_lw_entering = 0;
   g_lw_slot     = 0;
   g_lw_ll       = LL_OFF;
   if (!active)
   {
      ll_to_b();
      ll_win_reset();
   }
   if (g_ll_iter_h)
   {
      g_ll_eng++;
      if (active && ll_own_posted())
         g_lw_ll = LL_WAIT_ME;
      else
      {
         /* Nothing of this iteration's to present (a frame the core did
          * not draw, the ME went away): tier 0 shows whatever is drawn. */
         g_ll_iter_h = 0;
         ll_to_b();
      }
   }
   else if (g_ll_cool < (1u << 20))
      g_ll_cool++;
   /* Tier 0 with me_lowlat on: time this frame's render against the vblank
    * after its poll, to decide whether tier 1 would have made it. */
   g_lw_measure = !g_ll_iter_h && active && ll_own_posted();
   /* The entry slot (TIER 1 POLICY): a wait two vblanks long. */
   g_lw_slot = want >= 2u && !g_ll_iter_h && active && g_flip_early &&
               ll_own_posted() && ll_eligible();
   if (g_lw_slot)
      g_ll_slot++;
}

static __attribute__((noinline)) int lat_wait_step(void)
{
   unsigned now = (unsigned)sceKernelGetSystemTimeLow();
   int need = ll_need_us();
   int poll = 0;

   if (g_lw_ll == LL_WAIT_ME)
   {
      if (!mer_lowlat_active())
      {
         g_lw_ll = LL_DONE;       /* the ME went away / FF / rewind / link */
         ll_to_b();
      }
      else if (ll_own_ready())
      {
         if (us_to_vblank(now) >= need)
            ll_present(now);
         else
         {
            g_ll_miss_me++;       /* rendered, but too late to blit */
            ll_sample(1);
            g_lw_ll = LL_DONE;
            ll_to_b();
         }
      }
      else if (us_to_vblank(now) < need)
      {
         g_ll_miss_me++;          /* the render will not make it */
         ll_sample(1);
         g_lw_ll = LL_DONE;
         ll_to_b();
      }
      else
         poll = 1;
   }
   /* ENTRY.  The frame the loop top retired was flipped early (below, on an
    * earlier pass) and has latched on the first vblank of the two; now, in
    * the second interval, present this iteration's own render and flip it
    * before the second vblank.  Too late for that = no entry, and nothing
    * lost: the loop top shows it next iteration, as tier 0 always does. */
   if (g_lw_slot && g_lw_ll == LL_OFF && g_lw_swapped)
   {
      if ((int)(g_vc_target - sceDisplayGetVcount()) > 1 ||
          !vid_flip_latched())
         ;                        /* still the first interval: block on it */
      else if (!mer_lowlat_active())
         g_lw_slot = 0;
      else if (ll_own_ready() && us_to_vblank(now) >= need)
      {
         g_lw_entering = 1;
         g_lw_slot     = 0;
         ll_present(now);
      }
      else if (ll_own_ready() || us_to_vblank(now) < need)
      {
         g_lw_slot = 0;
         g_ll_slot_fail++;
      }
      else
         poll = 1;
   }
   if (g_lw_ll == LL_WAIT_GE)
   {
      if (vid_gu_busy())
         poll = 1;
      else
      {
         unsigned vc = sceDisplayGetVcount();
         unsigned t  = (unsigned)sceKernelGetSystemTimeLow();
         int lead    = us_to_vblank(t);
         ll_blit_note(t - g_lw_t);
         lat_swap(1);
         g_lw_swapped = 1;
         g_lw_ll      = LL_DONE;
         if (sceDisplayGetVcount() != vc)
         {
            /* the vblank fell between the check and the submit: this flip
             * latches one later, as a tier-0 one would */
            g_ll_blit_dev += g_ll_blit_ema / 4u;
            if (g_lw_entering)
               g_ll_slot_fail++;
            else
            {
               g_ll_miss_ge++;
               ll_sample(1);
               ll_to_b();
            }
         }
         else
         {
            if (lead >= 0 && (unsigned)lead < g_ll_lead_min)
               g_ll_lead_min = (unsigned)lead;
            if (g_lw_entering)
            {
               g_ll_mode_h = 1;   /* from the next loop top */
               g_ll_to_h++;
            }
            else
            {
               g_ll_hit++;
               ll_sample(0);
            }
         }
         g_lw_entering = 0;
      }
   }
   if (g_lw_measure)
   {
      if (ll_own_ready())
      {
         int would = sceDisplayGetVcount() == g_lat_poll_vc &&
                     us_to_vblank(now) >= need;
         if (would)
            g_ll_would++;
         else
            g_ll_wouldnt++;
         ll_sample(!would);
         g_lw_measure = 0;
      }
      else if (sceDisplayGetVcount() != g_lat_poll_vc ||
               us_to_vblank(now) < need)
      {
         g_ll_wouldnt++;          /* not rendered in time to blit */
         ll_sample(1);
         g_lw_measure = 0;
      }
      else
         poll = 1;
   }
   /* Tier 0a: the frame is complete once the GE is idle; flip it now and
    * it latches on the vblank this loop is waiting for. */
   if (!g_lw_swapped && g_lw_ll == LL_OFF && g_flip_early)
   {
      if (vid_gu_busy())
         poll = 1;
      else
      {
         lat_swap(1);
         g_lw_swapped = 1;
      }
   }
   return poll;
}

static void lat_wait_end(void)
{
   if (g_lw_ll == LL_WAIT_ME)
   {
      g_ll_miss_me++;             /* the vblank came first */
      ll_sample(1);
      ll_to_b();
   }
   else if (g_lw_ll == LL_WAIT_GE)
   {
      /* The GE did not finish before the vblank: the swap below (tier 1)
       * or the next loop top (entry) flips it a vblank later, as tier 0
       * would have.  Widen the deadline: the blit took at least this long,
       * and the deviation grows. */
      ll_blit_note((unsigned)sceKernelGetSystemTimeLow() - g_lw_t);
      g_ll_blit_dev += g_ll_blit_ema / 4u;
      if (g_lw_entering)
         g_ll_slot_fail++;
      else
      {
         g_ll_miss_ge++;
         ll_sample(1);
         ll_to_b();
      }
   }
   if (g_lw_measure)
   {
      g_ll_wouldnt++;
      ll_sample(1);
   }
   if (g_lw_slot)
      g_ll_slot_fail++;           /* the wait ended before the slot was tried */
   g_lw_ll = LL_OFF;
   g_lw_measure = g_lw_slot = g_lw_entering = 0;
}

/* @extract:end lowlat:lat_swap */
static void lat_census(void)
{
   unsigned i;
   if (++g_lat_iter < 600)
      return;
   fe_evt("lat iters=%u early=%u post=%u hold=%u over=%u "
          "hist=%u,%u,%u,%u,%u,%u,%u,%u latched_next=%u not=%u immed=%u "
          "lead_mean_us=%u lead_min_us=%u sub_hc=%u vb_hc=%u age_us=%u "
          "ll=%d h=%d hit=%u miss_me=%u miss_ge=%u would=%u wouldnt=%u "
          "to_h=%u to_b=%u blit_ema_us=%u age_n=%u fresh_n=%u fresh_got=%u "
          "fresh_us=%u eng=%u slot=%u slot_fail=%u win=%u/%u clean=%u "
          "need_us=%u blit_dev_us=%u lead1_min_us=%u vb=%u pull=%u",
          g_lat_iter, g_lat_early, g_lat_post, g_lat_hold, g_lat_over,
          g_lat_hist[0], g_lat_hist[1], g_lat_hist[2], g_lat_hist[3],
          g_lat_hist[4], g_lat_hist[5], g_lat_hist[6], g_lat_hist[7],
          g_lat_l1, g_lat_l0, g_lat_immed,
          g_lat_lead_n ? g_lat_lead_us / g_lat_lead_n : 0u,
          g_lat_lead_n ? g_lat_lead_min : 0u,
          g_lat_hc_n ? g_lat_hc_sum / g_lat_hc_n : 0u,
          g_lat_vbhc_n ? g_lat_vbhc_sum / g_lat_vbhc_n : 0u,
          g_lat_age_n ? g_lat_age_us / g_lat_age_n : 0u,
          g_ll_cfg, g_ll_mode_h, g_ll_hit, g_ll_miss_me, g_ll_miss_ge,
          g_ll_would, g_ll_wouldnt, g_ll_to_h, g_ll_to_b, g_ll_blit_ema,
          g_lat_age_n, g_in_fresh_n, g_in_fresh_got,
          g_in_fresh_n ? g_in_fresh_us / g_in_fresh_n : 0u,
          g_ll_eng, g_ll_slot, g_ll_slot_fail, g_ll_win_miss, g_ll_win_n,
          g_ll_clean, (unsigned)ll_need_us(), g_ll_blit_dev,
          g_ll_lead_min == ~0u ? 0u : g_ll_lead_min,
          sceDisplayGetVcount() - g_lat_vc0, g_ll_pull);
   if (g_ff_flip_pace && (g_ffh_holds || g_ffh_waits || g_ffc_waits))
      fe_evt("ffh pace=%d holds=%u held_loops=%u waits=%u cap3_waits=%u 3x=%d",
             g_ff_flip_pace, g_ffh_holds, g_ffh_vb, g_ffh_waits, g_ffc_waits,
             g_ffh_3x);
   g_ffh_holds = g_ffh_vb = g_ffh_waits = g_ffc_waits = 0;
   if (g_dfr_n)
      fe_evt("late_present n=%u late=%u", g_dfr_n, g_dfr_late);
   if (g_lf_n)
      fe_evt("late_flip held=%u caught=%u pull=%u", g_lf_n, g_lf_caught, g_lf_pull);
   g_dfr_n = g_dfr_late = g_lf_n = g_lf_caught = g_lf_pull = 0;
   g_lat_vc0 = sceDisplayGetVcount();
   for (i = 0; i < 8; i++)
      g_lat_hist[i] = 0;
   g_lat_iter = g_lat_early = g_lat_post = g_lat_hold = g_lat_over = 0;
   g_lat_l1 = g_lat_l0 = g_lat_immed = 0;
   g_lat_lead_n = g_lat_lead_us = 0;
   g_lat_lead_min = ~0u;
   g_lat_hc_n = g_lat_hc_sum = g_lat_vbhc_n = g_lat_vbhc_sum = 0;
   g_lat_age_n = g_lat_age_us = 0;
   g_ll_hit = g_ll_miss_me = g_ll_miss_ge = g_ll_would = g_ll_wouldnt = 0;
   g_ll_to_h = g_ll_to_b = 0;
   g_ll_eng = g_ll_slot = g_ll_slot_fail = g_ll_pull = 0;
   g_ll_lead_min = ~0u;
   g_in_fresh_n = g_in_fresh_us = g_in_fresh_got = 0;
}

