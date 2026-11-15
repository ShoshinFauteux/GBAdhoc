/* ---- THE ONE EXIT SEQUENCE (Epoch C, builds/epochc-out/PLAN-EPOCHC-beta5.md) --
 * Every way out of the process before the frame loop runs ONE sequence, in the
 * order its site always had, chosen by flags that are constants at each call:
 *
 *   exit_early_begin(f)   [ME] [CORE] [AUD VID]   what goes before the site's log line
 *   (the site's own `exit code=..` line, its text untouched: the Lab parses it)
 *   exit_early_end(f,..)  ES [AUD VID] HO|RL EXIT  the log closes, the handoff, the XMB
 *
 * The default order is AV-first (AUD VID EVT ES HO EXIT: family A, and the loop's
 * own end).  XE_EVT_FIRST keeps the order the selection's exits (B1-B10, C1, C2)
 * have: EVT ES AUD VID HO EXIT.  XE_ME and XE_CORE are the bad-script / net-failed
 * pair (C3, C4): the ME first (HW-2 F9), then the writer, the core and its save.
 * XE_RELAUNCH boots again instead of handing off (B8: not an end of run).
 * XE_HO_PASS hands off 0 when `pass`, else (code, why) (A5, HW-2 F7).
 * The exit map (tools/refactor/exit_matrix.py, T8) reads every call as this
 * body with its flags applied, so each site is checked as the copy it replaced.
 * Constant `if`s here have braced arms and no `else if`: the tools fold them. */
enum
{
   XE_EVT_FIRST = 1u << 0,
   XE_ME        = 1u << 1,
   XE_CORE      = 1u << 2,
   XE_RELAUNCH  = 1u << 3,
   XE_HO_PASS   = 1u << 4
};

static __attribute__((noinline, section(".text.fe_cold.exit_early_begin"))) void exit_early_begin(unsigned f)
{
   if (f & XE_ME)
   {
      /* The ME and its renderer stop before the core is freed, in the
       * loop teardown's order (F9). */
      me_rend_teardown("exit");
      me_video_teardown("exit", 1);
      me_host_shutdown();
   }
   if (f & XE_CORE)
   {
      io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
      fe_host_shutdown();
#ifdef REWIND
      PSP_REWIND_GAME_OVER();   /* fe_host let go of the pool */
#endif
   }
   if (!(f & XE_EVT_FIRST))
   {
      audio_stop();
      vid_term();
   }
}

static __attribute__((noinline, section(".text.fe_cold.exit_early_end"))) void exit_early_end(unsigned f,
   int code, const char *why, int pass)
{
   evt_shutdown();
   if (f & XE_EVT_FIRST)
   {
      audio_stop();
      vid_term();
   }
   if (f & XE_RELAUNCH)
   {
      relaunch_eboot(0);
   }
   if (f & XE_HO_PASS)
   {
      if (pass)
         FE_HANDOFF(0, NULL);
      else
         FE_HANDOFF(code, why);   /* ADR-0053 */
   }
   if (!(f & (XE_RELAUNCH | XE_HO_PASS)))
   {
      FE_HANDOFF(code, why);      /* ADR-0053 */
   }
   FE_EXIT_GAME();
}

/* SESSION END (family D, D1-D20): every way out of the frame loop -- HOME, the
 * menu's Exit, START+SELECT, a quit to the game list, a profile change, a
 * failed reset, the wake overlay's Quit, the harness and Lab ends -- runs this,
 * then main()'s in-process return (game list only), then process_end. */
static __attribute__((noinline, section(".text.fe_cold.session_end"))) void session_end(int have_variant)
{
   /* The quit / exit teardown runs outside the loop's park point (SRAM
    * flush, the ME and HOME modules, the radio, the core's unload, the
    * in-process return's census): the switch waits (pwr_hold, see power_cb).
    * Released before the browser (goto session_top) or by the exit itself. */
   /* The loop ended with a sleep's quiesce still in force (HOME or an exit
    * while held): put back what it stopped, so the teardown below finds the
    * state it was written for.  The gameplay power lock is play's only.
    * The hold BEFORE the lock is dropped (PLAN F2): the kernel counts both
    * as one, and a suspend request waiting on it runs at the unlock that
    * leaves the count at zero -- dropped first, that was this point, with
    * the teardown about to start and nothing parked.  The ME-lost wake's
    * hold (wake_service) is handed over the same way: dropped only now. */
   ui_set_wake_yield(NULL);   /* no game loop: nothing to park */
   sleep_quiesce_up("loop_exit");
   pwr_hold("teardown");
   if (g_wake_lost_hold)
   {
      g_wake_lost_hold = 0;
      pwr_release();
   }
   play_lock_drop("loop_exit");
   /* "Quit to game list": the menu goes, the frame it sat on stays with a
    * "Returning to game list..." chip, and the teardown below runs under it
    * (ui_return_hold; docs/INPROC-RETURN.md).  The wake overlay and the
    * harness quit come here with the snapshot taken but no menu backdrop. */
   if (g_relaunch == RELAUNCH_GAMELIST)
   {
      ui_set_backdrop(g_wake_have ? g_wake_frame : NULL, (int)g_wake_w,
                      (int)g_wake_h);
      ui_return_hold();
      exit_sim("return");        /* harness: HOME here (S-4 c) */
   }
   /* Clean exit (incl. HOME): stop the netdrv session + full adhoc
    * teardown first (BYE to peers while the radio is still up), then SRAM
    * is flushed inside fe_host_shutdown (dirty-check). */
   stall_watch_stop();   /* a clean exit is not a stall */
   /* Menu = HOME: HOME goes back to the system FIRST, before anything below
    * can block -- the handoff screen, a slow SRAM flush, a relaunch. */
   home_host_stop();
   drprof_stop(fe_host_frame_count());
   /* ADR-0080: stop the ME, then free our staging buffers (ADR-0080e: no
    * second core buffer to free; gba_screen_pixels was never reassigned). */
   me_rend_teardown("exit");
   me_video_teardown("exit", 1);
   /* THE IN-PROCESS RETURN KEEPS THE MODULE (docs/INPROC-RETURN.md, section
    * 8).  Stopping it would put the kernel's SceMeRpc handler back in charge
    * of an ME that has no firmware to answer it, and the next sleep would
    * hang in the kernel's suspend.  With the module kept the slot stays ours,
    * and a sleep from the list is the sleep every game has.  The core can
    * only be freed below because retaining waits out the ME's last job.
    * A quit that will not stay in this process stops it as ever, and so
    * does a return that goes wrong further down ("inproc_refused"). */
   if (g_relaunch != RELAUNCH_GAMELIST || inproc_refusal(have_variant) ||
       INPROC_ME_UNLOAD_MUTANT() || !me_host_retain(50000u))
      me_host_shutdown();
   if (!me_host_loaded() || me_host_retained())
   {
      free(g_me_stage[0]);   g_me_stage[0] = NULL;
      free(g_me_stage[1]);   g_me_stage[1] = NULL;
   }
   ui_return_tick();   /* the hold's ring keeps moving (no-op otherwise) */
   net_teardown();
#ifdef GBADHOC_MGIFT
   mgift_stop(1);      /* drops the AP association and unloads the net modules */
#endif /* GBADHOC_MGIFT */
   emu_boost_stop();   /* ADR-0065: before anything else changes priority */
   io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
   ui_return_tick();
   /* A Settings/Controls/Online page still open at a HOME, START+SELECT or
    * wake-quit kept its changes unsaved (F3).  Written here, synchronously,
    * on the same side of the writer's stop as ui_close's write always was
    * (PLAN §3.2 D16b), and before the SRAM flush that can be slow. */
   ui_settings_flush();
   /* The log's own loss, written SYNCHRONOUSLY now the writer is stopped, and
    * the liveness facts into RESULT.TXT, which is written with sceIo after the
    * log closes -- neither can be starved (RIG-DOUBLE-BATTLE §2.8 I6). */
   {
      char extra[128];
      snprintf(extra, sizeof(extra), "frames=%u\nevt_drop=%u\nt_ms=%u\n",
               fe_host_frame_count(), fe_evt_drops(),
               (unsigned)(net_now_us() / 1000ull));
      handoff_set_extra(extra);
      fe_evt("evt_drop total=%u", fe_evt_drops());
   }
   fe_host_shutdown();
#ifdef REWIND
   PSP_REWIND_GAME_OVER();   /* fe_host let go of the pool */
#endif
   ui_return_tick();
}

/* THE IN-PROCESS RETURN'S UNLOAD (U1-U12, docs/INPROC-RETURN.md): what the exit
 * path leaves to the process ending, given back before the census compares the
 * heap with the one this game's boot started from -- in its order.  Only the
 * return calls it, after the second inproc_refusal said yes; nothing in it can
 * refuse.  The tools read the call as this body (exit_matrix.EPOCHC_HELPERS;
 * tests/extract.epochc_expand); tests/test_inproc_unload.py pins the order. */
static __attribute__((noinline, section(".text.fe_cold.inproc_unload"))) void inproc_unload(void)
{
   dynarec_release_translation_caches();
   /* XS_HEAP's SMC tables (70,912 B: smc-cost added the 256 B of
    * retired-tag buckets): the first game's retro_init takes them,
    * after the boot snapshot, and a reload of the same game reuses
    * them -- but the next game's retro_init must find a fresh boot's
    * heap, not the last game's block, or the census
    * reads it as residue and refuses the return.  Given back here,
    * the next retro_init takes it again at the same place, zeroed
    * as on a fresh boot. */
   if (dynarec_heap_tables_release)
      dynarec_heap_tables_release();
#ifdef GBA_LINK
   /* The GBA link's cable reserve (harness builds outside late mode;
    * a player's build is late and never takes one) is per game: taken
    * after the snapshot, given back with the game.  A session would
    * have freed it, but a session also marks the radio used and so
    * never gets here (inproc_refusal "wireless"). */
   if (g_cable_reserve)
   {
      free(g_cable_reserve);
      g_cable_reserve = NULL;
   }
   g_gba_cable = 0;
#endif
   /* The renderer's buffers: kept when they are the volatile
    * partition's (the next game's me_rend_init reuses them, nothing
    * of the heap's), freed when they fell back to the heap -- the
    * exit path never had to. */
   me_rend_suspend();
   /* The next game's code is emitted into the same translation cache
    * the last one ran from.  Every publish syncs its own range, but a
    * relaunch also started the CPU caches from nothing, and the
    * in-process return must not be the first path to bet otherwise
    * (docs/RESIDENT-ICACHE-FIX.md: the resident-ROM crash was a stale
    * I-cache line outside a publish range). */
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

/* THE IN-PROCESS RETURN (I1-I5, docs/INPROC-RETURN.md), after session_end: only a
 * quit to the game list (g_relaunch == RELAUNCH_GAMELIST) asks for it.  The second
 * inproc_refusal (kept apart from session_end's first: PLAN §0.2-2), the unload,
 * the census against the heap this game's boot started from (snap_pre); then
 * either the return (its log line, the clock, the harness's browser sleep_sim;
 * returns 1: main() resets the session and goes round for the next game) or the
 * refusal (its log line, the relaunch into the list, the ME stopped; returns 0,
 * as for every other end: main() goes on to process_end).  The tools read
 * main()'s `if (!inproc_try_return(..)) break;` and the pass around it as the
 * `goto session_top` they replaced (aec_cparse.unloop_session;
 * tests/extract.epochc_unloop). */
static __attribute__((noinline, section(".text.fe_cold.inproc_try_return"))) int inproc_try_return(
   int have_variant, int *inproc_n, heap_snap_t snap_pre)
{
   if (g_relaunch == RELAUNCH_GAMELIST)
   {
      const char *why = inproc_refusal(have_variant);
      heap_snap_t after;
      fe_evt("audio_hash %08x samples=%u", plat_audio_hash(),
             plat_audio_sample_count());
      if (!why)
      {
         /* The rest of the unload: what the exit path leaves to the
          * process ending. */
         inproc_unload();
         heap_snap(&after);
         exit_sim("unload");        /* harness: HOME here (S-4 d) */
#ifndef GPSP_NO_TELEMETRY
         heap_census("post_unload");
#endif
         heap_trace_report("post_unload");
         if (!heap_snap_same(&after, &snap_pre))
         {
            /* Harness `inproc_force = 1` (diagnosis only): log, carry on. */
            if (!HARNESS_INT("inproc_force", 0))
               why = "census";
            fe_evt("inproc_census_mismatch arena=%u/%u used=%u/%u "
                   "chunks=%u/%u fordblks=%u/%u free=%u/%u largest=%u/%u "
                   "kfree=%u/%u kmax=%u/%u", snap_pre.arena, after.arena,
                   snap_pre.used, after.used, snap_pre.chunks, after.chunks,
                   snap_pre.fordblks, after.fordblks, snap_pre.free,
                   after.free, snap_pre.largest, after.largest,
                   snap_pre.kfree, after.kfree, snap_pre.kmax, after.kmax);
         }
      }
      if (!why)
      {
         /* A fresh boot's per-game state, then the browser again. */
         fe_evt("inproc_return n=%d", *inproc_n);
         (*inproc_n)++;
         /* The list and the next game run at the setting's clock, whatever
          * the teardown met (a wake in the list itself is answered by the
          * next game's "power back": g_pwr_resume waits for it). */
         psp_clock_apply("inproc_return");
#ifdef GPSP_PERF_RIG
         if (g_ss_browser && *inproc_n == g_ss_game + 1)
            sleep_sim_fire();        /* fires in the browser, 300 ms on */
#endif
         return 1;
      }
      fe_evt("inproc_refused why=%s", why);
      g_relaunch = RELAUNCH_GAMELIST;
      me_host_shutdown();       /* kept for a return that is not happening */
   }
   return 0;
}

/* PROCESS END (P1-P13): audio, display, the last log lines, the handoff, the
 * relaunch a profile change or a refused return asks for, the XMB. */
static __attribute__((noinline, section(".text.fe_cold.process_end"))) void process_end(int exit_code,
   const char *exit_reason)
{
   audio_stop();
   vid_term();
   if (g_relaunch != RELAUNCH_GAMELIST)
      fe_evt("audio_hash %08x samples=%u", plat_audio_hash(),
             plat_audio_sample_count());
#ifdef GPSP_PERF_RIG
   shash_close();
#endif
   {
      /* What the cartridge clock shows, what 3.5.0's frame clock would show
       * (boot time + emulated frames), and the console's clock. */
      extern void gpsp_rtc_peek(long long *, long long *, int *);
      long long shown = 0, frames_t = 0;
      int wall = 0;
      gpsp_rtc_peek(&shown, &frames_t, &wall);
      fe_evt("rtc_clock mode=%s shown=%lld frames_clock=%lld console=%lld "
             "frames_ahead_s=%lld shown_ahead_s=%lld",
             wall ? "wall" : "frames", shown, frames_t,
             (long long)psp_local_wallclock(),
             frames_t - (long long)psp_local_wallclock(),
             shown - (long long)psp_local_wallclock());
      FE_EVT_ONLY(shown); FE_EVT_ONLY(frames_t); FE_EVT_ONLY(wall);
   }
#ifdef GPSP_PERF_RIG
   fe_evt("stick_range lx=%u..%u ly=%u..%u mode=%s", g_stick_min[0],
          g_stick_max[0], g_stick_min[1], g_stick_max[1],
          ctl_stick_name(g_pcfg.stick));
#endif
   core_health_evt("exit");   /* final counters: heartbeats can be starved */
   if (exit_reason)
      fe_evt("exit code=%d reason=%s", exit_code, exit_reason);
   else
      fe_evt("exit code=%d", exit_code);
   evt_shutdown();
   /* ADR-0053: last thing before the XMB.  Every ms0 writer is stopped and the
    * log is closed, so it is safe to hand the stick to the PC here and nowhere
    * earlier.  Does not return if the PC asks for another run. */
   FE_HANDOFF(exit_code, exit_reason);
   /* ADR-0071: relaunch ourselves so the new trading profile is latched at
    * startup like any other boot.  Same call the USB handoff uses and for the
    * same reason: on CFW an EBOOT.PBP is launched the way the FIRMWARE
    * launches it, and plain sceKernelLoadExec returns instead of replacing the
    * process.  argp becomes the new argv[0], which is where every ms0 path is
    * derived from — passing NULL would silently fall back to the compiled-in
    * default directory and lose a variant install's saves. */
   /* Returns only if it failed: fall through to the XMB rather than hang.
    * HOME (F1) is an exit, whatever the quit asked for: after the audio_hash
    * guard above, which must still see the game-list value. */
   if (g_exit_req)
      g_relaunch = 0;
   if (g_relaunch)
      relaunch_eboot(g_relaunch == RELAUNCH_GAMELIST);
   FE_EXIT_GAME();
}

