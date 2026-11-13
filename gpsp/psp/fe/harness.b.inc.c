/* No process exit may leave the switch held: handoff_run (the USB handoff),
 * relaunch_eboot and sceKernelExitGame drop every hold first: every exit site
 * calls FE_HANDOFF / FE_EXIT_GAME, which release and then call the real one. */

#ifdef GPSP_PERF_RIG
/* ---- THE SLEEP SIMULATOR (harness only) -----------------------------------
 * PPSSPP never suspends, so power_cb's handshake, the standby teardown and
 * rebuild, the wake overlay and the in-process return's "standby" refusal
 * were reachable only on a console.  `sleep_sim_at = F` drives them: at game
 * frame F (of in-process game `sleep_sim_game`, default 0) a thread calls
 * power_cb exactly as the kernel's callback would -- SUSPENDING, then, after
 * `sleep_sim_ms`, RESUME_COMPLETE -- and keeps the main loop parked in
 * between, as a suspended CPU would be.  `sleep_sim_rom = 1` also closes the
 * paged ROM's file handle while the loop is parked (the PSP-1000's handle
 * does not survive a real sleep), so the next page read must take
 * load_gamepak_page's reopen.  `sleep_sim_browser = 1` instead fires 300 ms
 * after an in-process return, in the browser, where nothing parks.
 * `sleep_sim_choice` = 0 Continue / 1 Quit to game list answers the wake
 * overlay (ui_wake_inject; it reads the real pad, which PPSSPP has none of).
 * `sleep_sim_window = NAME` fires it the moment pwr_hold(NAME) is entered
 * (boot, teardown, session, state_save, state_load, ss_read, net, mgift,
 * wake, me_lost): the switch pressed inside that window.  Like the firmware it is
 * deferred while a power hold is up (scePowerLock; PPSSPP does not implement
 * it) -- `sleep_sim_nohold = 1` ignores the holds, i.e. the behaviour before
 * them, for the A/B.  `boot` opens with the game's selection (HW-1 r7-L5):
 * as the game list returns, or at the top of a harness, variant or XMB
 * launch -- before the sidecars, the ROM header and config.ini, which it
 * used to follow -- so a switch in the `boot` window (PLAN S-6) waits for
 * the loop's park from the list's return on, with no unheld stretch.
 * `sleep_sim_rewind_tick = N` (REWIND) fires it at the Nth loop on which a
 * rewind is in progress: the switch slid while the control is held
 * (docs/REWIND.md 22); N < 0: at the -Nth loop on which the machine stands
 * inside an old save sequence (fe_host_rewind_unvetted), the state the fix
 * is about.  `rewind_park_land = 0` is the MUTANT "the park point's budget is
 * spent": no landing, so the last resort has to act.
 * `sleep_sim_nopark = 1`: the loop does not answer the
 * park request and stands still until the resume, as a blocking loop that
 * has no park point would: power_cb then tears nothing
 * down.  `sleep_sim_trash_pool = 1` overwrites the rewind's part of the
 * volatile pool while "asleep", which is what the OS is entitled to do; and
 * `rewind_pool_restart = 0` is the MUTANT that leaves the history as it was
 * after such a sleep (docs/REWIND.md 23: the test must then fail).
 * `sleep_sim_again_ms = M` fires a SECOND suspend M ms after the first
 * resume -- with `sleep_sim_choice_frame` (ui_psp.c) late enough, that is
 * the switch pressed while the wake overlay is still waiting for an answer.
 * What it cannot simulate: the hardware actually losing power, the radio,
 * VRAM and the Memory Stick across a suspend. */
#include <streams/file_stream.h>
extern RFILE *gamepak_file_large;                 /* gba_memory.c */
static long g_ss_at, g_ss_ms = 1500, g_ss_again_ms;
static int g_ss_rom, g_ss_game, g_ss_browser, g_ss_fired, g_ss_nohold;
/* `sleep_sim_sav = 1`: the held-open .sav handle does not survive the sleep
 * either (fe_host_sram_test_kill_handle), so the first save write after the
 * wake fails as it does on a console. */
static int g_ss_sav;
static long g_ss_rw_tick;
#ifdef REWIND
static long g_ss_rw_seen;
#endif
static int g_ss_nopark, g_ss_trash_pool, g_ss_no_restart, g_ss_no_land;
/* `sleep_sim_me_sync_fail = 1` is the MUTANT of PLAN S-9 (r2-B-1): the ME's
 * cache-flush sync fails while asleep (SLEEP_SIM_ME_SYNC_FAIL, fe_priv.h),
 * so me_standby_down takes its release path (the module left loaded) and
 * the wake ends the session (wake_service). */
static int g_ss_me_sync_fail;
/* `sleep_sim_request = 1`: the suspend is a software request (auto-sleep, the
 * Go's slider): no flag at all until the gameplay lock is dropped, and only
 * power_request_poll can get it dropped.  `sleep_sim_nopoll = 1` turns the
 * poll off, the 4.5.0 behaviour: the run reports request_stuck. */
static int g_ss_request, g_ss_nopoll;
static volatile int g_ss_req_up;
/* THE KERNEL'S LOCK COUNT, for a request.  The kernel's power thread waits
 * for the LAST scePowerUnlock: the gameplay lock and a pwr_hold window are
 * one count there, and the thread outranks every thread of ours, so the
 * suspend starts at the unlock that leaves the count at zero -- even when
 * the next lock is a statement later (the quit's play_lock_drop before its
 * pwr_hold("teardown"), PLAN F2).  The unlocking thread latches that moment
 * here (sleep_sim_lock_zero, with who unlocked); the simulator fires on the
 * latch, so it sees a zero a 2 ms poll would miss. */
static const char *volatile g_ss_lock_zero;
#ifdef REWIND
static void sleep_sim_trash_pool(void);
#endif
static char g_ss_window[16];
static volatile int g_ss_asleep, g_ss_trigger;
/* The thread exists from the harness config on (sleep_sim_start) and never
 * exits: creating or deleting it moves the kernel partition, which the
 * in-process return's census compares (kfree / kmax). */
static int sleep_sim_thread(SceSize args, void *argp)
{
   int closed = 0, cycle;
   unsigned t0, waited = 0;
   (void)args; (void)argp;
   while (!g_ss_trigger)
      sceKernelDelayThread(5000);
   for (cycle = 0; cycle < (g_ss_again_ms > 0 ? 2 : 1); cycle++)
   {
   if (cycle)
   {
      sceKernelDelayThread((SceUInt)g_ss_again_ms * 1000u);
      fe_evt("sleep_sim again after_ms=%ld wake_menu=%d rebuild=%d slept=%d "
             "rend=%d vmem=%d", g_ss_again_ms, g_wake_menu, g_pwr_rebuild,
             g_pwr_slept, me_rend_active(), me_rend_vmem_held());
      waited = 0;
   }
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   if (g_ss_browser)
      sceKernelDelayThread(300000);
   /* the firmware's part: a switch pressed under scePowerLock is delivered
    * at the unlock (30 s cap: a hold that never ends is a bug to report) */
   while (!g_ss_nohold && g_pwr_hold > 0 && waited < 30000000u)
   {
      sceKernelDelayThread(2000);
      waited = (unsigned)sceKernelGetSystemTimeLow() - t0;
   }
   if (g_ss_request)
   {
      int stuck;
      g_ss_req_up = 1;
      while (!g_ss_lock_zero &&
             (g_pwr_play_lock || (!g_ss_nohold && g_pwr_hold > 0)) &&
             waited < 10000000u)
      {
         sceKernelDelayThread(2000);
         waited = (unsigned)sceKernelGetSystemTimeLow() - t0;
      }
      stuck = !g_ss_lock_zero &&
              (g_pwr_play_lock || (!g_ss_nohold && g_pwr_hold > 0));
      fe_evt("sleep_sim request lock=%d hold=%d zero=%s waited_ms=%u%s",
             g_pwr_play_lock, g_pwr_hold,
             g_ss_lock_zero ? g_ss_lock_zero : "-", waited / 1000u,
             stuck ? " request_stuck" : "");
      if (stuck)
      {
         g_ss_req_up = 0;
         for (;;)
            sceKernelSleepThread();
      }
   }
   fe_evt("sleep_sim suspend ms=%ld rom=%d browser=%d window=%s "
          "deferred_ms=%u hold=%d nohold=%d", g_ss_ms, g_ss_rom, g_ss_browser,
          g_ss_window[0] ? g_ss_window : "-", waited / 1000u, g_pwr_hold,
          g_ss_nohold);
   /* (A request let through at a zero that a new hold has since closed is
    * no stuck hold: the kernel started the suspend at the zero.) */
   if (g_pwr_hold > 0 && !g_ss_nohold && !g_ss_lock_zero)
      fe_evt("sleep_sim hold_stuck why=%s", g_pwr_hold_why);
   g_ss_asleep = 1;
   power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
   /* the loop is still parked here when it parked at all (g_ss_asleep) */
   if (g_ss_rom && g_pwr_parked && gamepak_file_large)
   {
      filestream_close(gamepak_file_large);
      gamepak_file_large = NULL;
      closed = 1;
   }
   if (g_ss_sav)
   {
      /* Outside the EVT (T11 R4, with r6-B3): fe_evt's arguments are not
       * evaluated where telemetry compiles out. */
      int killed = fe_host_sram_test_kill_handle();
      FE_EVT_ONLY(killed);
      fe_evt("sleep_sim sav_handle_killed=%d", killed);
   }
#ifdef REWIND
   if (g_ss_trash_pool)
      sleep_sim_trash_pool();
#endif
   fe_evt("sleep_sim asleep parked=%d rom_closed=%d", g_pwr_parked, closed);
   sceKernelDelayThread((SceUInt)g_ss_ms * 1000u);
   g_ss_asleep = 0;
   g_ss_req_up = 0;
   power_cb(0, PSP_POWER_CB_RESUMING, NULL);
   power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
   fe_evt("sleep_sim resumed cycle=%d slept=%d rebuild=%d rend=%d vmem=%d",
          cycle, g_pwr_slept, g_pwr_rebuild, me_rend_active(),
          me_rend_vmem_held());
   }
   for (;;)
      sceKernelSleepThread();
   return 0;
}
static void sleep_sim_start(void)
{
   int th = sceKernelCreateThread("sleep_sim", sleep_sim_thread, 0x11, 0x2000,
                                  THREAD_ATTR_USER, NULL);
   if (th >= 0)
      sceKernelStartThread(th, 0, NULL);
   fe_evt("sleep_sim thread th=%d", th);
}
static void sleep_sim_fire(void)
{
   if (g_ss_fired)
      return;
   g_ss_fired = 1;
   g_ss_trigger = 1;
   fe_evt("sleep_sim fire");
}
static void sleep_sim_window_enter(const char *why)
{
   if (g_ss_window[0] && !g_ss_fired && !strcmp(why, g_ss_window))
      sleep_sim_fire();
}
/* Every scePowerUnlock of ours calls this after unlocking (pwr_release,
 * play_lock_drop): the count of the comment at g_ss_lock_zero. */
static void sleep_sim_lock_zero(const char *who)
{
   if (g_ss_req_up && !g_ss_lock_zero && !g_pwr_play_lock && g_pwr_hold <= 0)
      g_ss_lock_zero = who;
}
/* IS THE ENGINE QUIET ONCE THE POOL HAS GONE BACK?  (PLAN S-9.)  A failed
 * sleep sync releases the volatile pool with the module still loaded, and
 * nothing in user mode can halt the ME before the kernel's suspend does
 * (me_standby_down).  This is the measurement of that window: whether the
 * engine has a job in flight 1 ms and 10 ms after the release, on the
 * callback's thread, logged for the run to read.  `sim` = the failure was
 * the simulator's (sleep_sim_me_sync_fail), not the engine's.  Both logs,
 * as me_standby_step: the event log's writer is stopped for the sleep by
 * then, and standby.log is the one that is written through. */
/* @extract:begin fn:sleep_sim_me_quiet */
static void sleep_sim_me_quiet(void)
{
   int idle_1ms, idle_10ms, up, sim = g_ss_me_sync_fail;
   sceKernelDelayThread(1000);
   idle_1ms = me_host_idle();
   sceKernelDelayThread(9000);
   idle_10ms = me_host_idle();
   up = me_host_up();
   fe_evt("standby me_quiet idle_1ms=%d idle_10ms=%d up=%d sim=%d",
          idle_1ms, idle_10ms, up, sim);
   standby_note("me_quiet idle_1ms=%d idle_10ms=%d up=%d sim=%d",
                idle_1ms, idle_10ms, up, sim);
}
/* @extract:end fn:sleep_sim_me_quiet */
#else
static void sleep_sim_window_enter(const char *why) { (void)why; }
static void sleep_sim_lock_zero(const char *who) { (void)who; }
#endif



