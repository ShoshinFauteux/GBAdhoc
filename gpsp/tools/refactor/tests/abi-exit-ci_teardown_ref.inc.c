/* abi-exit-ci_teardown_ref.inc.c -- PLAN §3.2/§3.3 written as code: the reference
 * implementation test_exit_model.c plays in phase T.  It is NOT emulator code and is
 * never linked into the PSP build.  It exists to prove two things before C1:
 *   1. the §3.3 flag set (ORDER selector + PE_/TD_ flags) can express every A-C exit
 *      site, in every flavour, exactly as `psp-gcc -E` of 01067e74 orders it;
 *   2. the §3.2 session_end / inproc_try_return / process_end split reproduces the loop
 *      teardown under every runtime scenario exit_matrix.py interprets.
 * C1's psp/teardown.inc.c takes this file's place (TEARDOWN_INC) and must pass the same
 * expectations.
 * It follows the tree it is checked against: exit_expect.h says which of the reviewed
 * HW-1 / HW-2 changes that tree has (EXPECT_D4_FIRST: HW-1 F2; EXPECT_SETTINGS_FLUSH: HW-2
 * F3; EXPECT_F1: HW-2 F1; EXPECT_F5: HW-2 F5; the TD_ME site flag: HW-2 F9), so one
 * file is the reference at b7d0cafd and at the tree Epoch C starts from.  Compile-time mutants (-DMUT_*) are used by tests/abi-exit-ci_test_exit.sh
 * to prove the harness catches a deviation.
 */

/* ------------------------------------------------------- families A-C: exit_early */
static void model_exit_early(unsigned f)
{
   if (f & TD_PRE_EVT)
      fe_evt("diag");                       /* A1, C1: a log line before the teardown */
   if (f & TD_LOADFIN)
      ui_loading_finish(0);
   if (f & TD_NOTICE)
      ui_notice("t", "1", "2", "3", 15000);
   switch (f & ORDER_MASK) {
   case ORDER_CORE_FIRST:                   /* C3, C4 */
      if (f & TD_ME) {                      /* HW-2 F9: the ME first, as in session_end */
         me_rend_teardown("exit");
         me_video_teardown("exit", 1);
         me_host_shutdown();
      }
      io_thread_stop();
      fe_host_shutdown();
#ifdef REWIND
      PSP_REWIND_GAME_OVER();
#endif
      /* fall through: then the AV-first tail */
   case ORDER_AV_FIRST:                     /* A1-A5 */
      audio_stop();
#ifndef MUT_SITE_NO_VIDTERM_AV
      if (!(f & PE_NO_VIDTERM))
         vid_term();
#endif
      fe_evt("exit code=%d", 0);
      evt_shutdown();
      break;
   case ORDER_EVT_FIRST:                    /* B1-B10, C1, C2 */
#ifdef MUT_SITE_AV_BEFORE_EVT
      audio_stop();
      fe_evt("exit code=%d", 0);
      evt_shutdown();
#else
      fe_evt("exit code=%d", 0);
      evt_shutdown();
      audio_stop();
#endif
      if (!(f & PE_NO_VIDTERM))
         vid_term();
      break;
   }
   if (f & PE_HO_BRANCHES)                  /* HW-2 F7 (A5): a pass and a fail branch each hand off; */
      FE_HANDOFF(0, "x");                   /* the flat site vocabulary lists both */
   if (f & PE_RELAUNCH)
      relaunch_eboot(0);                    /* B8: no handoff (PE_NO_HANDOFF) */
   else if (!(f & PE_NO_HANDOFF))
      FE_HANDOFF(2, "x");
   FE_EXIT_GAME();
}

/* ------------------------------------------------------- family D */
static void model_session_end(void)
{
#ifdef EXPECT_F5                             /* HW-2 F5: a link that could not put the game back */
   if (g_link_fatal_why && !exit_reason) {
      exit_code = g_link_fatal_code;
      exit_reason = g_link_fatal_why;
   }
#endif
   /* D0 (g_perf_rig gate EVTs) is a runtime rig block: off in every scenario */
   ui_set_wake_yield(NULL);                 /* D1 */
   sleep_quiesce_up("loop_exit");           /* D2 */
#if defined(EXPECT_D4_FIRST) != defined(MUT_SWAP_D3_D4)   /* HW-1 F2: the hold first; the mutant flips it */
   pwr_hold("teardown");                    /* D4 */
   play_lock_drop("loop_exit");             /* D3 */
#else
   play_lock_drop("loop_exit");             /* D3 */
   pwr_hold("teardown");                    /* D4 */
#endif
   if (g_relaunch == RELAUNCH_GAMELIST) {   /* D5 TD_GAMELIST */
      ui_set_backdrop(g_wake_have ? g_wake_frame : NULL, g_wake_w, g_wake_h);
      ui_return_hold();
   }
   stall_watch_stop();                      /* D6 TD_RIG (macro no-op in release) */
   home_host_stop();                        /* D7 */
   drprof_stop(0);                          /* D8 */
   me_rend_teardown("exit");                /* D9 */
   me_video_teardown("exit", 1);
   if (g_relaunch != RELAUNCH_GAMELIST || inproc_refusal(have_variant) ||
       0 /* INPROC_ME_UNLOAD_MUTANT() */ || !me_host_retain(50000u))
      me_host_shutdown();                   /* D10 */
   if (!me_host_loaded() || me_host_retained()) {
      /* D11: free(g_me_stage[0..1]) -- untracked */
   }
   ui_return_tick();                        /* D12 */
   net_teardown();                          /* D13 */
#ifdef GBADHOC_MGIFT                         /* psp/Makefile MGIFT ?= 1; absent at 01067e74 */
   mgift_stop(1);
#endif
   emu_boost_stop();                        /* D14 */
   io_thread_stop();                        /* D15 */
   ui_return_tick();                        /* D16 */
#ifdef EXPECT_SETTINGS_FLUSH
   ui_settings_flush();                     /* D16b (HW-2 F3) */
#endif
   handoff_set_extra("x");                  /* D17 */
#ifdef MUT_EVT_TEXT                          /* a log line reworded: the Lab would not parse it */
   fe_evt("evt_drop count=%u", 0u);
#else
   fe_evt("evt_drop total=%u", 0u);
#endif
   fe_host_shutdown();                      /* D18 TD_CORE */
#ifdef REWIND
   PSP_REWIND_GAME_OVER();                  /* D19 */
#endif
   ui_return_tick();                        /* D20 */
}

static void inproc_unload(void)             /* the atomic unit, U1-U12 */
{
   dynarec_release_translation_caches();
   dynarec_heap_tables_release();
#ifdef GBA_LINK
   if (g_cable_reserve) { free(g_cable_reserve); g_cable_reserve = NULL; }
   g_gba_cable = 0;
#endif
   me_rend_suspend();
   sceKernelDcacheWritebackInvalidateAll();
   sceKernelIcacheInvalidateAll();
   home_host_forget();
   fe_host_session_reset();
   osd_reset();
   ui_close();
   vid_ambient_drop();
   audio_stop();
   audio_ring_reset();
   audio_start();
}

static int model_inproc_try_return(void)
{
   const char *why;
   heap_snap_t after;
   if (g_relaunch != RELAUNCH_GAMELIST)
      return 0;
#ifdef MUT_MERGE_REFUSAL                     /* judge 1's graft, refused by §0.2-2 */
   why = sc_refusal[0] ? "refused" : NULL;
#else
   why = inproc_refusal(have_variant);      /* I1: call #2, kept separate */
#endif
   fe_evt("audio_hash %08x samples=%u", 0u, 0u);   /* I1b */
   if (!why) {
      inproc_unload();
      heap_snap(&after);                    /* U12 */
#ifndef GPSP_NO_TELEMETRY
      heap_census("post_unload");           /* U12b */
#endif
      heap_trace_report("post_unload");     /* U13: release-live */
      if (!heap_snap_same(&after, &snap_pre)) {   /* I3 */
         if (!fe_ini_get_int("harness", "inproc_force", 0))  /* release-live */
            why = "census";
         fe_evt("inproc_census_mismatch arena=%u/%u used=%u/%u "
                "chunks=%u/%u fordblks=%u/%u free=%u/%u largest=%u/%u "
                "kfree=%u/%u kmax=%u/%u", 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                0u, 0u, 0u, 0u);
      }
   }
   if (!why) {                              /* I4 */
      fe_evt("inproc_return n=%d", inproc_n);
      inproc_n++;
      psp_clock_apply("inproc_return");
#ifdef GPSP_PERF_RIG
      if (g_ss_browser && inproc_n == g_ss_game + 1)
         sleep_sim_fire();
#endif
      /* session_reset(): only the calls are tracked */
      ctl_turbo_stop(&g_turbo);
      ui_set_turbo_locked(0);
#ifdef EXPECT_F1
      g_running = !g_exit_req;              /* HW-2 F1: HOME inside the unload */
#else
      g_running = 1;
#endif
      emu_boost_start();
#ifndef MUT_DROP_RELEASE                     /* back in the list with the switch still held */
      pwr_release();                        /* last */
#endif
      return 1;
   }
   fe_evt("inproc_refused why=%s", why);    /* I5 */
   g_relaunch = RELAUNCH_GAMELIST;
   me_host_shutdown();
   return 0;
}

static void model_process_end(void)
{
   long long shown, frames_t;
   int wall;
   audio_stop();                            /* P1 */
   vid_term();                              /* P2 PE_VID */
#ifndef MUT_UNGUARD_AUDIO_HASH
   if (g_relaunch != RELAUNCH_GAMELIST)     /* P3: pairs with I1b and I5's assignment */
#endif
      fe_evt("audio_hash %08x samples=%u", 0u, 0u);
#ifdef GPSP_PERF_RIG
   shash_close();                           /* P4 */
#endif
#ifndef MUT_RTC_PEEK_RIG_ONLY
   gpsp_rtc_peek(&shown, &frames_t, &wall); /* P5: release-live, never behind TD_RIG */
#endif
   fe_evt("rtc_clock mode=%s shown=%lld frames_clock=%lld console=%lld "
          "frames_ahead_s=%lld shown_ahead_s=%lld", "x", 0LL, 0LL, 0LL, 0LL, 0LL);
   (void)shown; (void)frames_t; (void)wall;
#ifdef GPSP_PERF_RIG
   fe_evt("stick_range lx=%u..%u ly=%u..%u mode=%s", 0u, 0u, 0u, 0u, "x");   /* P6 */
#endif
   core_health_evt("exit");                 /* P7 */
   if (exit_reason)                         /* P8 */
      fe_evt("exit code=%d reason=%s", exit_code, exit_reason);
   else
      fe_evt("exit code=%d", exit_code);
   evt_shutdown();                          /* P9 */
   FE_HANDOFF(exit_code, exit_reason);      /* P10 */
#if defined(EXPECT_F1) && !defined(MUT_F1_RELAUNCH)
   if (g_exit_req)                          /* P11 (HW-2 F1): HOME is an exit */
      g_relaunch = 0;
#endif
   if (g_relaunch)                          /* P12 */
      relaunch_eboot(g_relaunch == RELAUNCH_GAMELIST);
   FE_EXIT_GAME();                          /* P13 */
}
