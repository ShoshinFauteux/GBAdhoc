/* ADR-0071: set by UI_ACT_RELAUNCH; acted on after full teardown.
 * RELAUNCH_GAMELIST: "Quit to game list", which may return to the browser
 * IN THIS PROCESS instead (inproc_*, below). */
static int g_relaunch;
#define RELAUNCH_PROFILE   1
#define RELAUNCH_GAMELIST  2

/* ---- THE IN-PROCESS RETURN TO THE GAME LIST -----------------------------
 * (docs/INPROC-RETURN.md)  "Quit to game list" used to relaunch the EBOOT:
 * the screen went black for the whole firmware teardown and reload.  It now
 * unloads the game in place -- the same teardown the exit path has always
 * run (SRAM flushed, ME and HOME modules stopped and unloaded, threads
 * stopped, the core's retro_deinit), plus the one allocation that teardown
 * used to leave for the process (the LARGE translation-cache block) -- and
 * goes back to the browser in this process.
 *
 * TRUSTED ONLY WHEN PROVEN, EVERY TIME.  The heap is snapshotted when the ROM
 * is picked (heap_snap: newlib's own byte counts, the free-chunk count, the
 * largest block, the kernel partition) and again after the unload; any
 * difference in those counts (heap_snap_same) and the EBOOT is relaunched
 * exactly as before, from a state that has already been flushed and torn
 * down.  Also relaunched, without
 * trying: after any network module was loaded (ad-hoc, Mystery Gift), after a
 * guest fault was contained (BADJUMP recovery), after the Media Engine's
 * watchdog gave up, after a standby, on a variant install, and whenever
 * CONFIG.INI says `quick_return = 0` (the kill switch) or the build says
 * GPSP_INPROC_RETURN=0. */
#ifndef GPSP_INPROC_RETURN
#define GPSP_INPROC_RETURN 1
#endif
#ifdef GPSP_PERF_RIG
/* `inproc_me_unload = 1`, the MUTANT: the return unloads the module and stays
 * in-process, as 4.5.0 did.  A sleep from the list must then report
 * handed_back=1, or the test that looks for it proves nothing. */
static int g_inproc_me_unload;
#else
/* INPROC_ME_UNLOAD_MUTANT(): this arm's definition is in fe/fe_priv.h */
#endif
/* gbadhoc_net_touched: netdrv/transport_adhoc.h.  badjump_recoveries and
 * smc_fallback_stats: core_fe.h.  smc_fallback_stats (SMC_FALLBACK_STATS:
 * why SMC traps took a full RAM flush) is in no build.sh profile's core,
 * so it stays weak: the heartbeat tests its address. */
#pragma weak smc_fallback_stats

/* Why this quit may not return in-process, or NULL when it may.  Checked
 * after the teardown, so what it reads is final. */
static const char *inproc_refusal(int have_variant)
{
   /* HOME during the quit (F1): the player is leaving, not going back to
    * the list.  Monotone like every predicate below (it only goes 0 -> 1),
    * so the teardown's two calls can only disagree in the safe direction. */
   if (g_exit_req)
      return "home";
   if (!GPSP_INPROC_RETURN)
      return "build";
   if (!g_pcfg.quick_return)
      return "config";
   if (have_variant)
      return "variant";
   if (gbadhoc_net_touched)
      return "wireless";
   if (badjump_recoveries)
      return "guest_fault";
   if (me_host_was_dead())
      return "me_watchdog";
   if (g_pwr_suspend)
      return "standby";
   /* The kernel's sleep handler is in its slot again, and it spins for ever
    * on an ME with no firmware running (docs/INPROC-RETURN.md, section 8).
    * Only a relaunch makes that safe, and it is what happens here. */
   if (me_host_handed_back() && !INPROC_ME_UNLOAD_MUTANT())
      return "me_stopped";
   return NULL;
}

