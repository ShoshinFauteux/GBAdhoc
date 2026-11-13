/* @extract:begin fn:power_cb */
static int power_cb(int unknown, int pwrflags, void *common)
{
   (void)unknown; (void)common;
   /* Log the raw flag word, before any delay or teardown, while the writer
    * can still get it to the stick.  A manual slide can arrive as
    * POWER_SWITCH without SUSPENDING (the later SUSPENDING must not tear
    * down the same renderer and volatile partition twice), and the PSP Go's
    * slider offers "Resume Game", which may deliver yet another subset.
    * The bits are the only way to tell. */
   fe_evt("power flags=0x%08X switch=%d susp=%d resuming=%d complete=%d slept=%d",
          (unsigned)pwrflags,
          !!(pwrflags & PSP_POWER_CB_POWER_SWITCH),
          !!(pwrflags & PSP_POWER_CB_SUSPENDING),
          !!(pwrflags & PSP_POWER_CB_RESUMING),
          !!(pwrflags & PSP_POWER_CB_RESUME_COMPLETE), g_pwr_slept);
   if (pwrflags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING))
      standby_note("flags=0x%08X switch=%d susp=%d slept=%d",
                   (unsigned)pwrflags,
                   !!(pwrflags & PSP_POWER_CB_POWER_SWITCH),
                   !!(pwrflags & PSP_POWER_CB_SUSPENDING), g_pwr_slept);
   if (pwrflags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING))
      g_pwr_sleep_pending = 1;
   /* Once the work is done for this sleep (the switch normally arrives first
    * and the SUSPENDING ~0.1 s later), a repeat returns at once: the firmware
    * is about to take the machine and nothing here would add anything.
    * Per sleep, not per teardown: a switch slid on the wake overlay (the
    * engine still down, g_pwr_slept 1) is a new sleep, and the loop must
    * park and quiesce for it like any other. */
   if ((pwrflags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING)) &&
       !(g_pcfg.standby && g_pwr_cb_done))
   {
      unsigned cb_t0 = (unsigned)sceKernelGetSystemTimeLow();
      int held = 0;
      /* Menu = HOME: HOME is the system's across a sleep.  The module turns
       * the popup back on within one poll (16 ms), well inside the 60 ms
       * delay below; the wake overlay keeps it there until Continue. */
      home_host_disarm();
      g_pwr_suspend++;
      g_pwr_gb_flush_req = 1;   /* serviced on the emulation thread */
      /* A GBA save made in the last seconds: ask the writer for its scan
       * NOW.  The delays below were always meant to let the .sav land, but
       * nothing asked for it -- the writer only scanned when its own 5 s
       * came round -- so a console put to sleep right after "saved the
       * game" slept with the save half on the stick or not at all. */
      fe_host_sram_request_scan();
      fe_evt("power suspending net=%d me=%d standby=%d module=%d retained=%d "
             "handed_back=%d", g_net_up, me_rend_active(), g_pcfg.standby,
             me_host_loaded(), me_host_retained(), me_host_handed_back());
      /* FLUSH BEFORE TOUCHING ANYTHING.  The first attempt did the teardown
       * first and the console hard-reset -- taking the queued line with it, so
       * the log said nothing at all about the failure it was there to record.
       * A diagnostic that cannot survive the crash it diagnoses is not a
       * diagnostic.  Every step below flushes as it goes, so the LAST line on
       * the stick names the step that killed the machine. */
      io_wake();
      sceKernelDelayThread(60000);
      if (g_pcfg.standby)
      {
         /* STOP THE FRAME BEFORE TAKING ITS MEMORY AWAY.
          *
          * The teardown clears the core's capture pointer and unlocks the
          * volatile partition the capture and stage buffers live in.  This
          * callback outranks the main thread and wakes from the delay above
          * wherever that thread happens to be -- including mid-frame, with
          * the core writing a scanline's registers through me_capture_buf or
          * the post filling g_mer_desc, and with the ME itself still drawing
          * into a stage.  Nothing stopped either, so a frame could land in
          * memory the OS was given back: a console that never woke (0 of 6 on
          * a PSP-3000), or one that woke and powered off a few steps after
          * Continue (PSP-1000, PSP Go).
          *
          * It hid in 2.0 and 2.0.1 because every step used to write a line to
          * the memory stick, and those writes blocked this callback long
          * enough for the main loop to notice the renderer was gone and finish
          * its frame first.  Removing the diagnostic file removed the accident.
          *
          * So: ask the loop to park between frames, wait for it, wait out the
          * ME's current job the way me_rend_suspend already does, and only
          * then tear down.  Both waits are bounded -- if the loop is inside a
          * menu it will not park, and nothing is being emulated there. */
         unsigned t0 = (unsigned)sceKernelGetSystemTimeLow(), park_us, idle_us;
         int idle = 1;
#ifdef REWIND
         g_pwr_park_t0 = t0;
#endif
         g_pwr_park_req = 1;
         /* Stage 1: has the loop seen it?  Stage 2, only if it has: wait
          * out its quiesce (see g_pwr_park_ack).  A loop with no park point
          * costs exactly what it always did. */
         while (!g_pwr_park_ack && !g_pwr_parked &&
                (unsigned)sceKernelGetSystemTimeLow() - t0 < PWR_PARK_WAIT_US)
            sceKernelDelayThread(2000);
         if (g_pwr_park_ack)
            while (!g_pwr_parked &&
                   (unsigned)sceKernelGetSystemTimeLow() - t0 <
                      PWR_PARK_WAIT_US + PWR_QUIESCE_WAIT_US)
               sceKernelDelayThread(2000);
         park_us = (unsigned)sceKernelGetSystemTimeLow() - t0;
         if (g_pwr_parked && g_pwr_slept)
            /* Slid again on the wake overlay: the engine is still down from
             * the last sleep (Continue rebuilds it), so the loop's quiesce
             * is all there is to do. */
            standby_note("suspend parked=1 park_us=%u already_down=1", park_us);
         else if (g_pwr_parked && !g_pwr_park_browser)
         {
            t0 = (unsigned)sceKernelGetSystemTimeLow();
            if (me_host_loaded())
               while (!(idle = me_host_idle()) &&
                      (unsigned)sceKernelGetSystemTimeLow() - t0 < 50000u)
                  sceKernelDelayThread(1000);
            idle_us = (unsigned)sceKernelGetSystemTimeLow() - t0;
            if (idle)
            {
               standby_note("suspend parked=1 park_us=%u me_idle=1 idle_us=%u",
                            park_us, idle_us);
               me_standby_down();
            }
            else
               standby_note("suspend teardown_skipped reason=me_busy park_us=%u idle_us=%u",
                            park_us, idle_us);
         }
#ifdef REWIND
         else if (!me_rend_active() && !psp_rewind_pool_tenant())
#else
         else if (!me_rend_active())
#endif
         {
            /* The loop did not park, or it is the game list's that did
             * (browser_park), and the renderer is off: the browser
             * (after an in-process return the volatile partition is KEPT
             * locked for the next game), the wake overlay, a GB game.
             * Nothing reads the partition, and it does not survive a sleep
             * (me_standby_down), so give it back now; the next me_rend_init
             * locks it again as a fresh boot does.  Every main-thread path
             * that can turn the renderer ON runs under pwr_hold, so this
             * cannot race me_rend_init. */
            int rel = me_rend_release_vmem();
            FE_EVT_ONLY(rel);
            standby_note("suspend unparked renderer=off vmem_released=%d "
                         "park_us=%u browser_parked=%d", rel, park_us,
                         g_pwr_parked);
            fe_evt("power unparked renderer=off vmem_released=%d", rel);
         }
         else
         {
            /* A timeout is not an acknowledgement. The frame may still be
             * reading capture/stage memory, so leave those allocations and
             * the ME alone; reclaiming them here races the main thread.
             * pwr_hold keeps the switch away from every main-thread window
             * that cannot park, so this should no longer be reachable. */
            standby_note("suspend teardown_skipped reason=park_timeout park_us=%u",
                         park_us);
            fe_evt("power teardown_skipped reason=park_timeout park_us=%u",
                   park_us);
         }
         if (g_pwr_parked)
            held = 1;      /* answered below, once this callback is done */
         else
            /* Release the loop in any case; a suspend that never completes
             * must not leave it waiting forever. */
            g_pwr_park_req = 0;
      }
      /* Nobody parked will drop the gameplay power lock (a menu, standby
       * = 0): drop it here, or the suspend would wait for it forever.  A
       * loop that acknowledged and is still quiescing drops it itself, once
       * parked and torn down (sleep_park_hold): dropped here, the suspend
       * would land with it inside a join, a .sav write or the radio's
       * teardown. */
      if (!g_pwr_parked && !g_pwr_park_ack)
         play_lock_drop("cb");
      /* Give the writer a real chance to land the .sav and this line before
       * the kernel takes the machine away.  Not a guarantee -- the window is
       * the kernel's to grant -- but it is the difference between usually
       * losing progress and usually not. */
      sceKernelDelayThread(80000);
      standby_note("cb out us=%u parked=%d hold=%d",
                   (unsigned)sceKernelGetSystemTimeLow() - cb_t0,
                   g_pwr_parked, held);
      /* Our part is done -- and only now, after the last line this callback
       * writes, so the loop's own notes never share the file with ours.  As
       * in TYL, the game stays paused until the wake: the loop stays in its
       * park, asleep, until RESUME_COMPLETE clears park_req; seeing
       * g_pwr_cb_done it drops the gameplay power lock and the suspend
       * proceeds. */
      if (held)
         g_pwr_cb_done = 1;
   }
   if (pwrflags & PSP_POWER_CB_RESUMING)
      fe_evt("power resuming");
   if (pwrflags & PSP_POWER_CB_RESUME_COMPLETE)
   {
      g_pwr_resume++;
      g_pwr_park_req = 0;      /* releases a loop held in its park */
      g_pwr_cb_done = 0;
      g_pwr_sleep_pending = 0;
      g_pwr_req_fires = 0;
      standby_note("resume_complete flags=0x%08X slept=%d",
                   (unsigned)pwrflags, g_pwr_slept);
      fe_evt("power resume_complete slept=%d", g_pwr_slept);
#ifdef REWIND
      /* Flag only (this is the callback's thread): see the main loop. */
      if (!g_pwr_slept && psp_rewind_pool_tenant())
         g_rw_pool_suspect = 1;
#endif
      /* Flag only -- the main loop rebuilds.  See the note above. */
      if (g_pcfg.standby && g_pwr_slept)
         g_pwr_rebuild = 1;
      io_wake();
   }
   return 0;
}
/* @extract:end fn:power_cb */

/* ---- HOLDING THE POWER SWITCH OVER WINDOWS THAT CANNOT PARK ---------------
 *
 * power_cb's handshake (park_req / parked, above) only works where the main
 * loop reaches its park point.  Several main-thread jobs run OUTSIDE it, for
 * up to seconds, with the ME renderer's buffers live in the volatile
 * partition (me_rend_suspend keeps a volatile-backed renderer running) and
 * files open: a game's boot and ROM load, the GBA link session's start/end
 * reloads (0.9-2 s on hardware), the quit/exit teardown and the in-process
 * return, state save/load with its .thumb/.shot, the Save States page's
 * reads, the wireless bring-up/teardown and Mystery Gift's.  A sleep landing
 * in one used to time the park out (250 ms) and skip the standby teardown,
 * so the renderer kept pointers into a partition the OS may hand to someone
 * else while we sleep -- the exact failure the handshake was built against.
 *
 * scePowerLock defers the power switch: a suspend requested while it is held
 * runs when the last scePowerUnlock does, i.e. at a point where the loop can
 * park.  (The switch's POWER_SWITCH flag is not deferred; only the suspend,
 * and with it SUSPENDING -- see power_request_poll.)  One nesting count, main thread only (every caller is on
 * the main thread); the kernel lock is taken on 0 -> 1 and dropped on 1 -> 0.
 * Every exit of the process (handoff_run, relaunch_eboot, sceKernelExitGame)
 * drops it first (pwr_release_all), so no path can leave the console unable
 * to sleep.  The longest hold is a big cart's boot (seconds); the player's
 * standby lands when the game is up, which is what any game that holds the
 * switch over a save does. */
static int g_pwr_hold;
static const char *g_pwr_hold_why = "";
static unsigned g_pwr_hold_t0;
static void sleep_sim_window_enter(const char *why);   /* harness, below */
static void sleep_sim_lock_zero(const char *who);      /* harness, below */
void pwr_hold(const char *why)
{
   if (g_pwr_hold++ == 0)
   {
      g_pwr_hold_why = why;
      g_pwr_hold_t0 = (unsigned)sceKernelGetSystemTimeLow();
      scePowerLock(0);
   }
   sleep_sim_window_enter(why);
}
void pwr_release(void)
{
   if (g_pwr_hold <= 0)
      return;
   if (--g_pwr_hold == 0)
   {
      unsigned ms = ((unsigned)sceKernelGetSystemTimeLow() - g_pwr_hold_t0) /
                    1000u;
      FE_EVT_ONLY(ms);
      scePowerUnlock(0);
      sleep_sim_lock_zero(g_pwr_hold_why);
      fe_evt("pwr_hold why=%s ms=%u", g_pwr_hold_why, ms);
   }
}
/* @extract:begin fn:pwr_release_all */
static void pwr_release_all(void)
{
   /* Every exit of the process comes through here, and a module the
    * in-process return kept is the one thing of a game that the exit's
    * teardown has not already stopped: the game list has no teardown. */
   if (me_host_retained())
      me_host_shutdown();
   if (g_pwr_hold > 0)
   {
      g_pwr_hold = 1;
      pwr_release();
   }
   play_lock_drop("exit");   /* the gameplay lock too */
}
/* @extract:end fn:pwr_release_all */

/* A BOOT THAT STOPS ON A NOTICE (a save that could not be read, no memory
 * for the game).  The notice waits on the player, for up to `hold_ms`, and
 * the boot's hold would make a POWER switch slid at that screen wait as
 * long.  It is dropped for the notice and taken again for the exit that
 * follows.  A sleep there catches nothing: no game is loaded, the renderer
 * is off, and power_cb's unparked branch is the one the game list had before
 * it parked.  Not inlined: main()'s code stays as it was. */
/* @extract:begin fn:boot_stop_notice */
static __attribute__((noinline)) void boot_stop_notice(const char *title,
      const char *l1, const char *l2, const char *l3, unsigned hold_ms)
{
   pwr_release();
   ui_notice(title, l1, l2, l3, hold_ms);
   pwr_hold("boot");
}
/* @extract:end fn:boot_stop_notice */

