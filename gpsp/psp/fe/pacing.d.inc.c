/* @extract:begin lowlat:late_present */
/* LATE PRESENT (tier 0b).  me_early_retire shows the render of frame N at
 * the loop top of N+1 when the ME finished it during N's wait.  That loop
 * top only BLITS; the flip is the early flip in N+1's own wait, a whole
 * frame later.  When the render was still running at the loop top (the ME at
 * 222 MHz renders FireRed's overworld in ~11.5 ms from the vcount-160 post,
 * which ends within a millisecond either side of the vblank), the loop top
 * used to blit N-1 again and flip it -- a repeat -- and step 1 of
 * me_rend_frame then retired N at vcount 160.  N+1's render usually made its
 * own loop top, replaced N as the one to show, and N was never presented: a
 * repeat and a skip, the judder ingo3643 saw at 1x, ~250 times a minute.
 *
 * Instead the loop top presents nothing; the wait presents (blit + HUD +
 * OSD) whatever has retired by then -- N, retired by step 1 at the latest --
 * before the in-wait retire can look at N+1, and the early flip waits for
 * that blit as it waits for any GE work.  N latches on the very vblank it
 * would have on time: no repeat, no skip, no extra latency.  A render still
 * running at the wait (more than a frame late) shows the previous picture,
 * as before.  The blit moves from beside emulation into the wait, where the
 * loop was idle; a frame with no wait left flips after the vblank (the
 * post-wait swap flushes the GE first), which is what a late frame does
 * anyway.  Off under FF, the draw gate, a rewind and same-frame, as tier 0b
 * is; tier 1's own wait-present is untouched. */
/* BELOW FULL CLOCK ONLY.  At 333 MHz the ME renders a light game in ~7 ms
 * and tier 0b / tier 1 make their deadlines: beta.3's presentation is kept
 * there exactly (owner: 333 latency must not move; the hold below would
 * keep tier 1 out while the picture runs late).  The clock read is the one
 * running, whatever set it (the setting, the harness, a CFW holding its
 * own), re-read every 64 loops (~1 s) so a change in the menu applies. */
static unsigned g_dfr_clk_n __attribute__((section(".sbss")));
static int      g_dfr_slow __attribute__((section(".sbss")));
static int dfr_clock_low(void)
{
   if ((g_dfr_clk_n++ & 63u) == 0u)
      g_dfr_slow = scePowerGetCpuClockFrequency() < 300;
   return g_dfr_slow;
}

static int dfr_defer(void)
{
   g_dfr = 0;
   if (!g_dfr_cfg || !dfr_clock_low() || !g_me_rend || g_mer_pending < 0 ||
       g_mer_ready == -1 || g_mer_ready != g_mer_last_presented ||
       g_ff_mult || g_ff_uncapped || g_ff_gate ||
       !mer_retire_early_active() || mer_rewind_busy())
      return 0;
   g_dfr = 1;
   TEL(pacing_tel.dfr_n++);
   return 1;
}

static void dfr_present(void)
{
   if (!g_dfr)
      return;
   g_dfr = 0;
   if (g_mer_ready == g_mer_last_presented)
      TEL(pacing_tel.dfr_late++);        /* still rendering: the old picture again */
   me_rend_present();
#ifdef REWIND
   if (g_drew && (g_rw_on || g_rwh.denied))
      psp_rewind_hud();
#endif
   if (g_drew)
      osd_draw();
}

/* LATE FLIP HOLD (tier 0a at 1x, with the late present).  A frame that
 * overran its vblank is flipped after it (the post-wait swap) and latches on
 * the NEXT vblank.  The next loop's early flip, submitted before that same
 * vblank, used to replace it: the late frame was never shown, a repeat and a
 * skip.  Now a wait that finds the last flip still unlatched holds its early
 * flip, so every frame is shown, one vblank later than on time (the 4.1.0
 * cadence) -- until a nudge: in a two-vblank wait the hold ends at the first
 * vblank, and the early flip in the second interval brings the picture back
 * to the low-latency timing, consuming the repeat the nudge would show.  The
 * same rule FF flip pacing bit 0 uses at 3x (ffh_too_soon).  Off in tier 1
 * (its flips have their own deadline) and under any FF. */
static void lf_hold_begin(void)
{
   g_lf_hold = 0;
   if (!g_dfr_cfg || !g_dfr_slow || !g_flip_early || g_ff_mult || g_ff_uncapped || g_ff_gate ||
       g_ll_iter_h || !ffh_too_soon(1))
   {
      g_lf_behind = 0;
      return;
   }
   g_lf_behind  = 1;
   g_flip_early = 0;
   g_lw_slot    = 0;             /* no tier-1 entry in this wait either */
   g_lf_hold    = 1;
   TEL(pacing_tel.lf_n++);
}

/* After each vblank of the wait: the held flip has latched. */
static void lf_hold_vblank(int saved)
{
   if (g_lf_hold && !ffh_too_soon(1))
   {
      g_lf_hold    = 0;
      g_flip_early = saved;
      if ((int)(g_vc_target - sceDisplayGetVcount()) > 0)
      {
         TEL(pacing_tel.lf_caught++);     /* a vblank left: flipped early in it */
         g_lf_behind = 0;
      }
   }
}

/* CATCH UP SOON.  Running a vblank late (the last wait held and did not
 * catch up), take the audio-ring nudge now instead of in up to ~5 s.  Its
 * two-vblank wait is where the hold catches up: the held frame latches on
 * the first vblank, this one on the second, so the screen shows no repeat at
 * all -- the late frame's own repeat stands in for the clock-mismatch repeat
 * the nudge exists to make.  A nudge taken early moves, it is not added: the
 * next one comes a frame of ring later, so their long-run rate stays set by
 * 59.94 vs 59.73 Hz.  The ring sits up to a frame lower until then; it is
 * only taken while that leaves half the high-water mark (~3.7 frames of
 * audio) in the ring, so a run of late frames cannot starve the audio. */
static int lf_want_slot(unsigned level)
{
   return g_lf_behind && g_dfr_cfg && !link_session_any() &&
          level > RING_HIGH_WATER / 2u + in_rate / 60u;
}

/* @extract:end lowlat:late_present */
