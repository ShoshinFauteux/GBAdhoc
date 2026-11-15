/* ------------------------------------------------------------------- main */

/* The PSP's local wall clock as seconds since 1970, with no timezone applied
 * (the core decodes it with gmtime).  Days from civil date: H. Hinnant's
 * algorithm, valid for any proleptic Gregorian date. */
static long long g_clock_fake;      /* harness psp_clock: this console's
                                     * clock reads this at boot (seconds
                                     * since 1970), then runs; 0 = real */
static long long g_clock_fake_tz;   /* harness psp_clock_tz: with psp_clock,
                                     * the seconds that clock is ahead of
                                     * UTC (the console's zone); default 0 */
static time_t psp_datetime_seconds(const ScePspDateTime *dt)
{
   long long y, m, era, yoe, doy, doe, days;
   y   = (long long)dt->year - (dt->month <= 2);
   m   = (long long)dt->month;
   era = (y >= 0 ? y : y - 399) / 400;
   yoe = y - era * 400;
   doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (long long)dt->day - 1;
   doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   days = era * 146097 + doe - 719468;
   return (time_t)(days * 86400 + (long long)dt->hour * 3600 +
                   (long long)dt->minute * 60 + (long long)dt->second);
}

static time_t psp_local_wallclock(void)
{
   ScePspDateTime dt;
   if (g_clock_fake)
      return (time_t)(g_clock_fake +
                      (long long)(sceKernelGetSystemTimeWide() / 1000000ull));
   if (sceRtcGetCurrentClockLocalTime(&dt) < 0)
      return (time_t)0;             /* no reading: the clocks hold */
   return psp_datetime_seconds(&dt);
}

/* The same clock as UTC: what the console's clock reads with its time zone
 * and daylight-saving setting taken off.  The GB/GBC cartridge clock runs
 * on this one, because its save stamps the time in UTC (fe_host.c); it
 * counts elapsed seconds only, so the game shows the same time either way. */
static time_t psp_utc_wallclock(void)
{
   ScePspDateTime dt;
   if (g_clock_fake)
      return (time_t)(g_clock_fake - g_clock_fake_tz +
                      (long long)(sceKernelGetSystemTimeWide() / 1000000ull));
   if (sceRtcGetCurrentClock(&dt, 0) < 0)
      return (time_t)0;
   return psp_datetime_seconds(&dt);
}

/* STATE LOADS WAIT FOR THE MEDIA ENGINE (FRONTEND lead #15).
 *
 * Proof first, from the code: the ME never WRITES guest memory -- it writes
 * only its PRX data, the stage buffer and the mailbox -- and it reads guest
 * VRAM/OAM/palette only in phase 1 of a render, which me_rend_frame waits out
 * (input_seq) before emulation resumes; shadows, when on, are frontend
 * copies.  So retro_unserialize cannot be corrupted by the ME, and a render
 * in flight only reads its own PRX copies and a capture buffer the load does
 * not touch.  The one exception is an `input_wedge` teardown, which gives up
 * while the ME may still be reading live VRAM; its output is discarded.
 *
 * The wait below therefore buys determinism, not safety: a load never
 * overlaps ME activity of any kind, whatever path got us here.  Bounded by
 * the same 50 ms ceiling as the input handshake, costs at most one render
 * per load, and a timeout only logs.  Saves need nothing: retro_serialize
 * only reads guest memory, and the thumbnail reads the READY stage, which the
 * ME is not writing. */
static void me_rend_quiesce(const char *why)
{
   unsigned t0;
   FE_EVT_ONLY(why);
   if (!me_host_up() || me_host_idle())
      return;
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle() &&
          ((unsigned)sceKernelGetSystemTimeLow() - t0) < MER_INPUT_US)
      ;
   if (!me_host_idle())
      fe_evt("me_rend quiesce_timeout why=%s", why);
}

/* The single state-load entry point for this frontend: quiesce, load, log. */
static int psp_state_load_io(const char *path, const char *why)
{
   int rc;
   me_rend_quiesce(why);
   /* A loaded state starts with no turbo button firing: a Toggle is off
    * until it is pressed again, a Hold still down starts a fresh train. */
   ctl_turbo_stop(&g_turbo);
   rc = fe_host_state_load(path);
   core_health_evt("load"); /* @lab */ LAB_HOOK(lab_event(LAB_EVT_STATE_LOAD, (unsigned)rc, why);)
   return rc;
}

/* SOFT RESET ("Reset game", the in-game menu).  Everything a state load
 * does around the core -- land a rewind, quiesce the Media Engine renderer,
 * stop the turbo trains, hold the power switch -- then fe_host_reset (the
 * battery to the stick, then the fresh-boot reset; fe_host.h).  Refused in
 * any link or wireless session.  Same rc as fe_host_reset. */
static int psp_game_reset(void)
{
   int rc;
   if (link_session_any())
   {
#ifdef GBA_LINK
      fe_evt("game_reset REFUSED reason=session net=%d mg=%d gblink=%d "
             "pair=%d", g_net_up, g_mg_on, fe_host_gblink_active(),
             fe_gbadual_active());
#else
      fe_evt("game_reset REFUSED reason=session net=%d mg=%d gblink=%d",
             g_net_up, g_mg_on, fe_host_gblink_active());
#endif
      return -2;
   }
   pwr_hold("reset");
#ifdef REWIND
   (void)psp_rewind_land("reset", 0);
#endif
   me_rend_quiesce("reset");
   ctl_turbo_stop(&g_turbo);
   rc = fe_host_reset();
   if (rc == 0 && g_pcfg.console != FE_CONSOLE_GBA)
      g_last_pix = NULL;    /* the GB machine (and its frame buffer) is new */
   core_health_evt("reset");
   pwr_release();
   return rc;
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static int psp_state_load(const char *path, const char *why)
{
   int rc;
   pwr_hold("state_load");
#ifdef REWIND
   (void)psp_rewind_land("state_load", 0);   /* (a failed load goes on here) */
#endif
   rc = psp_state_load_io(path, why);
   pwr_release();
   return rc;
}

