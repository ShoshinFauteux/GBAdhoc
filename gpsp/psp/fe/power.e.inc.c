/* THE PARK POINT (the suspend handshake's main-thread half), between two
 * frames: nothing is being emulated, captured or posted, so this is where
 * the power callback may take the renderer's memory.  Quiesce, report
 * `parked`, stay held until the wake (loop_unpark undoes the quiesce).
 *
 * Called at the top of the game loop and nowhere else.  That covers the
 * in-game menu (its pages are drawn by the loop) and, since the wake
 * overlay yields to it (wake_overlay_yield), the SELECT+TRIANGLE pause
 * screen too: every in-game sleep is entered from this one place, in the
 * same state. */
/* @extract:begin fn:loop_park */
static int loop_park(void)
{
   if (!g_pwr_park_req)
      return 0;
   {
#ifdef REWIND
      /* A rewind in progress LANDS before the history is taken away (the
       * callback suspends the rewind once this loop is parked, and play
       * would go on from wherever the machine stood: possibly the middle
       * of an old save sequence, replayed over a newer save).  It gets
       * what is left of the callback's wait, less a margin for the
       * engine to go idle; what cannot land in that is decided by
       * fe_host_rewind_suspend (vetted frame, or the game is reset). */
      if (g_rw_on && fe_host_rewind_busy())
      {
         unsigned used = (unsigned)sceKernelGetSystemTimeLow() - g_pwr_park_t0;
         unsigned room = used + 60000u < PWR_PARK_WAIT_US
                       ? PWR_PARK_WAIT_US - 60000u - used : 0u;
#ifdef GPSP_PERF_RIG
         if (g_ss_no_land)
            room = 0;
#endif
         if (room >= 10000u)
            (void)psp_rewind_land("sleep", room);
         else
            fe_evt("rewind_land_psp why=sleep ok=0 budget_us=0 (no room)");
      }
#endif
      /* Seen: the callback now waits for the quiesce, not the 250 ms. */
      g_pwr_park_ack = 1;
      sleep_quiesce_down();
      g_pwr_parked = 1;
      sleep_park_hold();
      g_pwr_parked = 0;
      g_pwr_park_ack = 0;
   }
   return 1;
}
/* @extract:end fn:loop_park */

/* The wake has come (or a hold ran out): undo the quiesce -- reopen what
 * reopens, restart the writer, then sound -- before anything else, the wake
 * overlay included.  A loop the callback let go before the wake runs on
 * without its sound until then, as TYL's does. */
/* @extract:begin fn:loop_unpark */
static void loop_unpark(const char *why)
{
   if (g_sq.down && !g_pwr_sleep_pending)
      sleep_quiesce_up(why);
}
/* @extract:end fn:loop_unpark */

/* THE GAME LIST'S PARK POINT (ui_set_browser_park), once a frame, before
 * the browser reads the pad or draws.
 *
 * The browser used to have none: the callback waited out its 250 ms, gave
 * the volatile partition back from its own thread and returned, and the
 * suspend then landed on a main thread still running the list -- a GE list
 * in flight, box art being read off the stick and decoded, and the audio
 * thread in sceAudioOutputBlocking with its channel reserved.  That is the
 * state the in-game sleep was taken out of (the TYL order above: nothing of
 * ours inside a driver when the deferred suspend runs), and the game list
 * after a game is where a PSP-3001 was seen to cold-boot on a sleep.
 *
 * So the list parks like the game loop: acknowledge, stop and join the
 * audio thread (the only thread of ours the list runs -- the writer, the
 * .sav, the ROM handle, HOME and the radio all went with the game's
 * teardown), report `parked`, and sleep in sleep_park_hold until the wake.
 * The callback sees g_pwr_park_browser and does only the list's half of
 * the teardown (the partition), so no wake overlay follows. */
/* @extract:begin fn:browser_park */
static int browser_park(void)
{
   int audio;
   if (!g_pwr_park_req)
      return 0;
   g_pwr_park_browser = 1;
   g_pwr_park_ack = 1;
   audio = sq_audio_stop(NULL);
   standby_note("browser park audio_stop rc=%d", audio);
   g_pwr_parked = 1;
   sleep_park_hold();
   g_pwr_parked = 0;
   g_pwr_park_ack = 0;
   g_pwr_park_browser = 0;
   if (audio == SQ_DONE || audio < 0)
      (void)sq_audio_start(NULL);
   standby_note("browser unpark audio_start=%d", audio_running);
   return 1;
}
/* @extract:end fn:browser_park */

/* The wake overlay's yield hook (ui_set_wake_yield), once a frame: the
 * power callback wants the loop parked, so the overlay leaves and the loop
 * goes round to its top to park there (wake_service: WAKE_YIELD).
 *
 * The overlay does not park itself.  From in there the loop would sleep
 * nested in wake_service at the in-game present -- mid-iteration, after the
 * pad read and before the ME's staged frame is presented, and without the
 * loop top's own work (the GB cart-RAM flush the callback asks for, the
 * rewind's power hold).  With one park point there is nothing to differ. */
/* @extract:begin fn:wake_overlay_yield */
static int wake_overlay_yield(void)
{
   power_request_poll();
   return g_pwr_park_req;
}
/* @extract:end fn:wake_overlay_yield */

/* THE WAKE: one rebuild, after the overlay's answer, on this thread.
 * Returns WAKE_QUIT when the player chose `Quit to game list` (g_running is
 * 0 and the caller must `continue` to the loop's end), WAKE_YIELD when the
 * overlay left for a park (the caller `continue`s; the loop's top parks and
 * the overlay opens again after), else 0.
 *
 * Called where the game loop presents, and FIRST THING in the in-game
 * menu's branch.  That branch ends its iteration with `continue`, so it
 * never reached the call below it: a sleep taken with the menu open (it
 * parks, at the loop's top, like play) came back with g_pwr_rebuild set
 * forever -- g_wake_hold up, so the menu drew every frame and never swapped
 * onto the screen me_standby_down had blanked, and the renderer and the
 * volatile partition stayed down: a black screen with the menu live
 * underneath.  Now the overlay comes up over the menu, and Continue
 * rebuilds and returns to the menu where it was. */
/* @extract:begin def:wake_answers */
#define WAKE_QUIT  1
#define WAKE_YIELD 2
/* @extract:end def:wake_answers */
static int g_wake_answered;   /* Continue taken: the pad is the answer's */
/* THE WAKE THAT ENDS THE SESSION SAYS WHY (owner 2026-11-12).  A Media Engine
 * that stopped answering is not something a PSP should ever show, so when a
 * sleep found it so the wake goes to the game list (wake_service) -- and
 * without a word that reads as a crash.  Up for 3 s or until a button.
 *
 * NO HOLD, though this loop has no park point: it needs none.  The renderer
 * is off and the volatile pool already gone (me_standby_down), so a switch
 * slid here sleeps at once, catches nothing and wakes back into this loop,
 * as on a boot's notice (boot_stop_notice).  Under a hold that suspend
 * waited for the release -- the one moment it must not have: power_cb had
 * by then dropped the gameplay lock (nothing parked), so the release took
 * the kernel's count to zero with the loop's exit and the teardown's save
 * next, before the teardown's own hold (PLAN F2, at the loop's end). */
/* @extract:begin fn:wake_me_lost_notice */
static __attribute__((noinline)) void wake_me_lost_notice(void)
{
   unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
   SceCtrlData pd;
   do
   {
      vid_overlay_begin(1);
      vid_text_center(110, "The Media Engine stopped during sleep.", 0xFFFF);
      vid_text_center(140, "Saving and returning to the game list.", 0x65BF);
      vid_overlay_end();
      vid_swap();
      sceDisplayWaitVblankStart();
      memset(&pd, 0, sizeof(pd));
      sceCtrlPeekBufferPositive(&pd, 1);
      if ((pd.Buttons & 0xF3F9u) &&
          (unsigned)sceKernelGetSystemTimeLow() - t0 > 500000u)
         break;                     /* a press, not a key held at the wake */
   } while ((unsigned)sceKernelGetSystemTimeLow() - t0 < 3000000u);
}
/* @extract:end fn:wake_me_lost_notice */
/* @extract:begin fn:wake_service */
static int wake_service(void)
{
   g_wake_answered = 0;   /* (also drops one the menu branch left) */
   if (g_pwr_rebuild)
   {
      /* The rebuild itself waits for the overlay's answer (me_standby_up):
       * g_pwr_slept stays 1 until then, which is also what keeps
       * g_wake_hold up and makes a second resume ask again. */
      g_pwr_rebuild = 0;
      ring_r = ring_w;              /* sound from before the sleep */
      g_wake_menu = 1;
      fe_evt("standby wake overlay_first slept=%d rend=%d vmem=%d",
             g_pwr_slept, me_rend_active(), me_rend_vmem_held());
   }
   /* THE ME DID NOT SURVIVE THE SLEEP (me_standby_down's failed sync, PLAN
    * r2-B-1).  No overlay and no rebuild: the session ends here, as `Quit to
    * game list` ends it, through the teardown (the save written) to the
    * list.  Continuing would be a game on one core, which this build never
    * runs (owner 2026-11-08; confirmed 2026-11-12 for an engine the watchdog
    * had already given up on).  Only an engine that had been up can be
    * lost: a console with no Media Engine at all (Adrenaline on a PS Vita)
    * never sets the flag and wakes onto the overlay below. */
   if (g_wake_menu && g_pwr_me_lost)
   {
      g_wake_menu = 0;
      g_pwr_me_lost = 0;
      home_host_disarm();
      fe_evt("ME_LOST *** the Media Engine did not survive the sleep: "
             "session ends *** loaded=%d up=%d", me_host_loaded(),
             me_host_up());
      fe_evt("wake_menu choice=game_list reason=me_lost slept=%d",
             g_pwr_slept);
      wake_me_lost_notice();
      g_pwr_slept = 0;
      g_pwr_rebuild = 0;
      g_relaunch = RELAUNCH_GAMELIST;
      g_running = 0;
      return WAKE_QUIT;
   }
   /* THE WAKE OVERLAY.  Modal, and deliberately so: nothing needs to be
    * emulated while it is up (the frame is a still and any link is long
    * gone), so it owns the pad and the swap chain until the player picks.
    * `Quit to game list` relaunches the EBOOT, which is how this build
    * returns to the browser everywhere else -- the core cannot unload a
    * ROM cleanly, and a relaunch flushes SRAM on the way out. */
   if (g_wake_menu)
   {
      g_wake_menu = 0;
      home_host_disarm();   /* the overlay is not the game: HOME is the system's */
      ctl_turbo_stop(&g_turbo);   /* the console slept: auto-fire is over */
      int answer = ui_wake_menu(g_wake_have ? g_wake_frame : NULL,
                                (int)g_wake_w, (int)g_wake_h,
                                g_pcfg.last_rom);
      if (answer == UI_WAKE_YIELD)
      {
         /* The switch: the loop parks at its top, and the overlay (the same
          * snapshot) is back after -- as the wake overlay if a sleep came. */
         g_wake_menu = 1;
         fe_evt("wake_menu yield park_req=%d", g_pwr_park_req);
         return WAKE_YIELD;
      }
      if (answer)
      {
         /* The same quit the menu's "Quit to game list" makes: through
          * the teardown (SRAM flushed) to the browser.  It used to
          * relaunch from here directly, skipping both. */
         fe_evt("wake_menu choice=game_list slept=%d", g_pwr_slept);
         /* Nothing is rebuilt for a game that is being left: the
          * teardown below stops the engine and the next game starts it
          * as a boot does.  Only the flags must not outlive the game
          * (g_wake_hold would keep its swaps suppressed). */
         g_pwr_slept = 0;
         g_pwr_rebuild = 0;
         g_relaunch = RELAUNCH_GAMELIST;
         g_running = 0;
         return WAKE_QUIT;
      }
      fe_evt("wake_menu choice=continue frame=%d slept=%d running=%d",
             g_wake_have, g_pwr_slept, g_running);
      g_wake_have = 0;
      {
         /* PR #16 (pad_swallow), here since #15 moved the overlay's answer
          * into wake_service.  The answer's X or O is still down: not the
          * game's.  Read now, as it answered: the rebuild below takes a
          * while.  Nor the pad this iteration read before the overlay
          * (g_pad, which the core and the menu read), nor its edges and
          * shortcuts (the caller's pad_new / ctl_fired: turbo_frame reads
          * pad_new after the present). */
         SceCtrlData pd;
         sceCtrlPeekBufferPositive(&pd, 1);
         pad_swallow_arm(pd.Buttons);
         g_pad = 0;
         g_wake_answered = 1;  /* the caller drops its pad_new and ctl_fired */
#ifdef REWIND
         g_rw_held = 0;
#endif
      }
      if (!g_running)
      {
         /* HOME / exit while the overlay was up: leaving, as above. */
         g_pwr_slept = 0;
         g_pwr_rebuild = 0;
      }
      else if (g_pwr_slept || g_pwr_rebuild)
      {
         unsigned up_t0 = (unsigned)sceKernelGetSystemTimeLow();
         pwr_hold("wake");          /* renderer rebuild: me_rend_init */
         me_standby_up();
         pwr_release();
         fe_evt("standby up_us=%u",
                (unsigned)sceKernelGetSystemTimeLow() - up_t0);
         FE_EVT_ONLY(up_t0);
      }
   }
   return 0;
}
/* @extract:end fn:wake_service */

