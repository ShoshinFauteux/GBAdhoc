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
 * wake): the switch pressed inside that window.  Like the firmware it is
 * deferred while a power hold is up (scePowerLock; PPSSPP does not implement
 * it) -- `sleep_sim_nohold = 1` ignores the holds, i.e. the behaviour before
 * them, for the A/B.
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
/* `sleep_sim_request = 1`: the suspend is a software request (auto-sleep, the
 * Go's slider): no flag at all until the gameplay lock is dropped, and only
 * power_request_poll can get it dropped.  `sleep_sim_nopoll = 1` turns the
 * poll off, the 4.5.0 behaviour: the run reports request_stuck. */
static int g_ss_request, g_ss_nopoll;
static volatile int g_ss_req_up;
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
      g_ss_req_up = 1;
      while (g_pwr_play_lock && waited < 10000000u)
      {
         sceKernelDelayThread(2000);
         waited = (unsigned)sceKernelGetSystemTimeLow() - t0;
      }
      fe_evt("sleep_sim request lock=%d waited_ms=%u%s", g_pwr_play_lock,
             waited / 1000u, g_pwr_play_lock ? " request_stuck" : "");
      if (g_pwr_play_lock)
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
   if (g_pwr_hold > 0 && !g_ss_nohold)
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
#else
static void sleep_sim_window_enter(const char *why) { (void)why; }
#endif



