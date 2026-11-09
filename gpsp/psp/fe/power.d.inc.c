/* STANDBY halves.  See the note on power_cb for why the teardown runs in the
 * callback and the rebuild runs on the main thread. */
/* BREADCRUMBS THAT SURVIVE A HARD RESET.
 *
 * The event log did not, and the reason is FAT, not our ring: fe_evt_service
 * does fwrite + fflush, so the BYTES reach the stick, but the directory entry
 * is only updated when the file is closed.  A kernel reset therefore leaves
 * frontend.log at its last-closed length -- which is why two standby attempts
 * of different lengths both produced a byte-identical 2464-byte log ending at
 * boot, and why every log that DID contain gameplay was one that exited
 * cleanly.
 *
 * So the standby trail gets its own file and pays for a full open/write/close
 * per line.  That is far too expensive for anything on the frame path and
 * exactly right for six lines across a suspend. */
static void standby_note(const char *fmt, ...)
{
#if defined(GPSP_NO_TELEMETRY) && !defined(GPSP_STANDBY_NOTES)
   /* A player build writes no diagnostic files -- ADR-0067, the same rule
    * that removes the event log.  This one was missed: every sleep and wake
    * appended to log/standby.log on the memory stick, from the power
    * callback, in 2.0 and 2.0.1. */
   (void)fmt;
#else
   char path[160], line[192];
   va_list ap;
   FILE *fh;
   int n;

   va_start(ap, fmt);
   n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   snprintf(path, sizeof(path), "%s/log/standby.log", g_dir_base);
   fh = fopen(path, "a");
   if (!fh)
      return;
   fprintf(fh, "%u %s\n", (unsigned)sceKernelGetSystemTimeLow(), line);
   fclose(fh);                        /* the close IS the commit */
#endif
}

/* A LADDER, NOT A SWITCH.  `standby` is 0-3 and each level does strictly more
 * than the one below, so the level that kills the console is the answer --
 * without a rebuild between attempts:
 *
 *   1  park the renderer, release volatile memory; module stays LOADED
 *   2  + unload the module (me_host_shutdown)
 *
 * Releasing volatile memory is NOT a rung -- see the note where it happens.
 *
 * Level 3 was the first attempt and the PSP-1000 hard-reset to the Sony logo,
 * which is the kernel giving up rather than anything being damaged.  The
 * suspect is unloading a kernel module from the power callback while the
 * kernel is already quiescing for suspend -- so the ladder exists to say
 * whether that is really it, or whether merely parking the renderer is
 * enough on its own.
 *
 * Each step logs and flushes BEFORE the next one runs. */
static void me_standby_step(const char *what)
{
   fe_evt("standby step=%s", what);
   standby_note("step %s", what);
}

/* @extract:begin fn:me_standby_down */
static void me_standby_down(void)
{
   me_standby_step("begin");
   /* Note what is running BEFORE tearing any of it down: the wake rebuilds
    * this, and only this. */
   g_pwr_had_host = me_host_up();
   /* SNAPSHOT FIRST.  me_rend_teardown() sets g_mer_ready = -1, so taking the
    * frame after it always found "nothing to copy" and the wake overlay came
    * up on a flat background instead of the game.  Order is the whole fix. */
   wake_snapshot();
   if (me_rend_active())
   {
      me_rend_teardown("suspend");
      me_standby_step("rend_down");
   }
   if (g_pcfg.standby >= 2)
   {
      me_host_shutdown();
      me_standby_step("module_unloaded");
   }
   /* ALWAYS release the volatile partition -- this is not an optional rung.
    *
    * The engine's buffers are carved out of the 4 MB OS pool at 0x08400000,
    * which the OS owns and is free to reclaim and re-hand to something else
    * while we are asleep.  The first ladder kept g_mer_vmem set at level 1, so
    * the resume path saw "volatile already locked", skipped re-locking, and
    * rebuilt the renderer onto POINTERS INTO A PARTITION WE NO LONGER HELD.
    * Writing there is a plausible way to reset the machine, and the machine
    * did reset on resume.
    *
    * So releasing is the safe half and re-locking on wake is mandatory.  What
    * remains genuinely questionable is unloading the module, which is what the
    * ladder is now for. */
   int release_result = me_rend_release_vmem();
   if (release_result < 0)
   {
      me_standby_step("vmem_retained");
      return;
   }
   if (release_result > 0)
      me_standby_step("vmem_released");
   /* The audio ring still holds sound from before the sleep, and the audio
    * thread plays it the moment the machine comes back -- ~20 ms of the game
    * you are no longer looking at, underneath a menu.  Drop it. */
   ring_r = ring_w;
   /* And leave the screen black rather than a stale frame of gameplay: the
    * LCD shows whatever is in VRAM the instant it powers on, well before we
    * are scheduled to draw anything. */
   vid_blank_all();
   g_pwr_slept = 1;
   me_standby_step("done");
}
/* @extract:end fn:me_standby_down */

/* Main thread, once per wake -- AFTER the player answers the wake overlay
 * with Continue, not before it.
 *
 * It used to run first, so the overlay waited for its answer with the
 * renderer rebuilt, the Media Engine running and the volatile partition
 * locked again.  The overlay is a blocking loop that never reaches the
 * suspend handshake's park point, so a second press of the switch while it
 * was up took power_cb's park_timeout branch: nothing torn down, g_pwr_slept
 * left 0.  The console then slept holding the partition the OS reclaims
 * across a suspend, with an engine that does not survive one, and the wake
 * after that rebuilt nothing -- it restarted to the XMB.  (Measured with the
 * sleep simulator on a PSP-3000: `power teardown_skipped reason=park_timeout`,
 * then slept=0 rebuild=0 rend=1 vmem=1.)
 *
 * While the overlay is up the machine now stays exactly as the suspend left
 * it: renderer down, partition released, g_pwr_slept still 1.  A second
 * suspend parks the loop (the overlay yields to it) but finds nothing to tear
 * down (power_cb's already_down branch), and its resume asks for the same
 * single rebuild.  `Quit to game list` never
 * rebuilds at all: the teardown it goes through shuts the engine down anyway.
 *
 * Every step is logged: if resume ever fails on a model we do not own, this
 * line is the whole diagnosis. */
static void me_standby_up(void)
{
   int host_rc, rend_rc = -1;

   g_pwr_rebuild = 0;
   g_pwr_slept   = 0;
   /* Rebuild only what we tore down.  At level 1 the module is still loaded
    * and me_host_init would be a second load of a module that never left.
    *
    * And never build what boot chose not to.  GB-family games and me_boot = 0
    * run without the module, and g_me_rend_cfg = 0 means no renderer (GB-family,
    * ME mode off, or a renderer that failed at boot).  Waking used to start
    * both regardless: the renderer took over presentation (capture mode 1
    * stops the core drawing) but only understands GBA 240x160 frames, so a GB
    * game came back from sleep to a blank screen after Continue. */
   standby_note("wake begin host_up=%d had_host=%d me_rend=%d cfg=%d vmem=%d",
                me_host_up(), g_pwr_had_host, me_rend_active(), g_me_rend_cfg,
                me_rend_vmem_held());
   if (me_host_up())
      host_rc = 0;
   else if (g_pwr_had_host)
      host_rc = me_host_init(g_dir_base);
   else
      host_rc = 1;   /* never up: leave it down */
   standby_note("wake host=%d", host_rc);
   if (host_rc == 0 && g_me_rend_cfg)
      rend_rc = me_rend_active() ? 0 : me_rend_init();
   standby_note("wake rend=%d vmem=%d free=%d", rend_rc, me_rend_vmem_held(),
                sceKernelTotalFreeMemSize());
   fe_evt("standby up host=%d rend=%d cfg=%d vmem=%d free=%d frame=%d",
          host_rc, rend_rc, g_me_rend_cfg, me_rend_vmem_held(),
          sceKernelTotalFreeMemSize(), g_wake_have);
#ifdef REWIND
   psp_rewind_pool_back();      /* after the engine has taken its end */
#endif
   /* Whatever the audio thread queued while the machine was coming back is
    * also from before the sleep. */
   ring_r = ring_w;
   /* A failed rebuild is not fatal: g_me_rend stays 0 and the loop falls back
    * to CPU rendering exactly as it does when the ME never boots.  Slow, but
    * the game keeps running and the save is intact. */
}

/* ---- THE SLEEP QUIESCE (psp/sleep_q.h) ----------------------------------
 *
 * A PSP-3001 (ARK-4 6.61) powered OFF when the switch was slid in game: the
 * teardown completed, then the LED blinked, went solid and the next start was
 * a cold boot.  From the ROM list the same console sleeps and wakes, with the
 * ME never started (me_boot = 0) it fails identically, and Snes9xTYLme sleeps
 * on the same console and card.
 *
 * Until this, the in-game sleep was entered with everything in the game
 * still running: the teardown released the parked loop again, so the core
 * kept emulating (and reading ROM pages) into the suspend, the audio thread
 * kept a channel reserved and sat in sceAudioOutputBlocking with real sound,
 * the .sav writer thread and its held-open handle were live, and so was the
 * paged ROM's handle.  TYL (psp/psp.cpp:2545-2575, 3986-3993, 1841-1882) holds
 * scePowerLock through play; its callback only flags the pause; the MAIN loop
 * then stops and JOINS the sound thread (before_pause -> StopSoundThread),
 * kills the ME, and only then calls scePowerUnlock -- so the deferred suspend
 * runs while the main thread sleeps in sceKernelDelayThread and no other
 * thread of the game is inside a driver.  It keeps no file open across play.
 * The steps below reproduce that. */
#include <streams/file_stream.h>
extern RFILE *gamepak_file_large;                 /* gba_memory.c */
static void net_teardown_io(void);                /* defined below */
static sleep_q g_sq;
/* The gameplay scePowerLock (TYL's scheme).  Taken at the
 * top of the loop while a game is being played; dropped by whoever lets a
 * suspend proceed: the parked loop once the callback is done, the callback
 * itself when nothing is parked, and every exit (pwr_release_all).  Separate
 * from g_pwr_hold's nesting count: the kernel counts locks, so the two
 * cannot unbalance each other. */

/* @extract:begin fn:play_lock_take */
static void play_lock_take(void)
{
   /* Held over the wake overlay too: it yields to the park
    * (wake_overlay_yield), so a second sleep slid on it waits for the
    * park like one slid in play. */
   if (g_pwr_play_lock || g_pwr_sleep_pending || g_pwr_park_req)
      return;
   {
      int rc = scePowerLock(0);
      if (rc >= 0 || SLEEP_SIM_REQUEST())   /* PPSSPP has no scePowerLock */
         g_pwr_play_lock = 1;
      else
      {
         static int told;
         if (!told)
         {
            told = 1;
            standby_note("q play_lock rc=%d", rc);
         }
      }
   }
}
/* @extract:end fn:play_lock_take */

/* @extract:begin fn:play_lock_drop */
static void play_lock_drop(const char *who)
{
   int rc;
   if (!g_pwr_play_lock)
      return;
   g_pwr_play_lock = 0;
   rc = scePowerUnlock(0);
   sleep_sim_lock_zero(who);
   standby_note("q play_unlock by=%s rc=%d", who, rc);
   FE_EVT_ONLY(rc);
   FE_EVT_ONLY(who);
}
/* @extract:end fn:play_lock_drop */

/* A SUSPEND REQUEST THE GAMEPLAY LOCK HAS BEEN SITTING ON.
 *
 * Only the POWER switch tells us anything under scePowerLock: syscon's event
 * raises POWER_SWITCH at once, and that is what starts the park which drops
 * the lock.  A request made by software (scePowerRequestSuspend: the firmware's
 * own, for auto-sleep, a dying battery and, it appears, the PSP Go's slider)
 * raises no flag at all.  The kernel's power thread notes it and waits for the
 * last unlock, and SUSPENDING comes only after that.  So with the lock held
 * through play the request waited for a callback that was waiting for it, and
 * the slider did nothing (4.5.0; the lock came with the PSP-3001 fix, 2510780).
 * Decompiled power.prx: uOFW src/kd/power/powerSwitch.c, _scePowerOffThread.
 * That the slider takes this path is inferred: the switch works, the slide
 * does not, and it worked before the lock.
 *
 * scePowerIsRequest() reports the waiting request (PSPSDK's header says it
 * always returns 0; uOFW shows it reading the two fields the power thread sets
 * before it blocks).  Seen here, the request is answered as a switch: one sleep
 * path, the one every model already takes.  power_cb cannot run on this thread
 * (it waits for this thread to park), so cb_thread is woken to run it.  Windows
 * under pwr_hold wait for their release, as the suspend itself would.
 *
 * Every 16th pass, only with the lock held, so never outside play.  Called from
 * the loop's top, which play, the in-game menu and a link stall (a frame lost,
 * plat_gblink_wait bounded by it) all reach, and from the wake and pause
 * overlay's yield hook, which is the one place holding the lock that does not.
 *
 * A request that keeps reading true with no suspend following must not freeze
 * the game for a hold's 5 s over and over: a few tries, then leave it alone. */
/* @extract:begin def:PWR_REQ_MAX_FIRES */
#define PWR_REQ_MAX_FIRES 3
/* @extract:end def:PWR_REQ_MAX_FIRES */
int scePowerIsRequest(void);             /* in libpsppower, not in its header */
/* @extract:begin fn:power_request_poll */
static void power_request_poll(void)
{
   static unsigned pass;
   if (!g_pwr_play_lock || (++pass & 15u))
      return;
   if (g_pwr_sleep_pending || g_pwr_req_signal || g_pwr_hold > 0 ||
       g_pwr_req_fires >= PWR_REQ_MAX_FIRES || !POWER_IS_REQUEST())
      return;
   g_pwr_req_fires++;
   standby_note("deferred request fires=%d", g_pwr_req_fires);
   fe_evt("power deferred_request fires=%d", g_pwr_req_fires);
   g_pwr_req_signal = 1;
   sceKernelWakeupThread(g_cb_thid);
}
/* @extract:end fn:power_request_poll */

/* @extract:begin fn:sq_audio_stop */
static int sq_audio_stop(void *c)
{
   int rc;
   (void)c;
   if (audio_thid < 0)
      return SQ_NONE;
   if (!audio_running)
   {
      (void)audio_stop();       /* a worker that already died: just reap it */
      return SQ_NONE;
   }
   rc = audio_stop();
   ring_r = ring_w;             /* nothing from before the sleep is replayed */
   return rc;
}
/* @extract:end fn:sq_audio_stop */

/* @extract:begin fn:sq_audio_start */
static int sq_audio_start(void *c)
{
   (void)c;
   ring_r = ring_w;
   audio_start();
   if (audio_thid >= 0 && audio_running)
      return SQ_DONE;
   return audio_output_error < 0 ? audio_output_error : -1;
}
/* @extract:end fn:sq_audio_start */

/* @extract:begin fn:sq_writer_stop */
static int sq_writer_stop(void *c)
{
   int rc;
   (void)c;
   rc = io_thread_stop();       /* its last pass lands what it had */
   /* The callback asked for its scan 60-80 ms before this park, and the
    * game may have written SRAM since.  The writer gone, scan and drain once
    * more here, so the .sav on the stick is SRAM as it stands at the
    * suspend: also with no writer thread (sram_thread = 0: io_thread_stop
    * syncs nothing) or one terminated before its last pass. */
   (void)fe_host_sram_flush(0);
   fe_host_sram_sync();
   return rc;
}
/* @extract:end fn:sq_writer_stop */

/* @extract:begin fn:sq_writer_start */
static int sq_writer_start(void *c)
{
   (void)c;
   io_thread_start();
   return g_io_thid >= 0 ? SQ_DONE : -1;
}
/* @extract:end fn:sq_writer_start */

/* After sq_writer_stop: the .sav and the event log are the writer's while
 * it runs, and it is gone now. */
/* @extract:begin fn:sq_files_close */
static int sq_files_close(void *c, char *d, size_t n)
{
   int sav, rom = 0, evt, closed = 0;
   (void)c;
   sav = fe_host_sram_sleep_close();
   closed += sav;
   /* The paged ROM's handle is this thread's own (load_gamepak_page runs
    * here), and the loader reopens it by path on the next page miss -- the
    * PSP-1000 wake fix in gba_memory.c.  A whole-cart console never reads it
    * again at all. */
   if (gamepak_file_large)
   {
      filestream_close(gamepak_file_large);
      gamepak_file_large = NULL;
      rom = 1;
      closed++;
   }
   evt = fe_evt_is_open();
   if (evt)
      fe_evt_close();
   closed += evt;
   snprintf(d, n, "sav=%d rom=%d evt=%d", sav, rom, evt);
   return closed;
}
/* @extract:end fn:sq_files_close */

/* @extract:begin fn:sq_files_reopen */
static int sq_files_reopen(void *c, char *d, size_t n)
{
   int evt = 1;
   (void)c;
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
   evt = fe_evt_reopen_append(LOG_PATH);
#endif
   snprintf(d, n, "evt=%d sav=lazy rom=lazy", evt);
   return evt;
}
/* @extract:end fn:sq_files_reopen */

/* @extract:begin fn:sq_home_stop */
static int sq_home_stop(void *c)
{
   (void)c;
   if (!home_host_loaded())
      return SQ_NONE;
   home_host_stop();
   return home_host_loaded() ? -1 : SQ_DONE;
}
/* @extract:end fn:sq_home_stop */

/* @extract:begin fn:sq_home_rearm */
static int sq_home_rearm(void *c)
{
   (void)c;
   /* The loop's next frame loads it again, as at a game's first frame
    * (home_host_start under `menu_home && !home_host_tried()`). */
   home_host_forget();
   return SQ_DONE;
}
/* @extract:end fn:sq_home_rearm */

/* @extract:begin fn:sq_net_term */
static int sq_net_term(void *c)
{
   (void)c;
   if (!g_net_up)
      return SQ_NONE;
   net_teardown_io();           /* BYE, adhoc term, modules unloaded */
   return g_net_up ? -1 : SQ_DONE;
}
/* @extract:end fn:sq_net_term */

/* @extract:begin fn:sq_note */
static void sq_note(void *c, const char *line)
{
   (void)c;
   standby_note("%s", line);
   fe_evt("sleep %s", line);
}
/* @extract:end fn:sq_note */

/* @extract:begin decl:g_sq_ops */
static const sleep_q_ops g_sq_ops = {
   NULL,
   sq_audio_stop, sq_audio_start,
   sq_writer_stop, sq_writer_start,
   sq_files_close, sq_files_reopen,
   sq_home_stop, sq_home_rearm,
   sq_net_term,
   sq_note
};
/* @extract:end decl:g_sq_ops */

/* Emulation thread, at the park point, before it reports `parked`. */
/* @extract:begin fn:sleep_quiesce_down */
static void sleep_quiesce_down(void)
{
   unsigned t0;
   if (g_sq.down || !g_pcfg.standby)
      return;
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   (void)sleep_q_down(&g_sq, &g_sq_ops);
   standby_note("q down us=%u stopped=0x%X",
                (unsigned)sceKernelGetSystemTimeLow() - t0, g_sq.stopped);
}
/* @extract:end fn:sleep_quiesce_down */

/* Emulation thread, once the wake has come (RESUME_COMPLETE), or at a hold's
 * timeout, or when the loop ends with the quiesce still in force. */
/* @extract:begin fn:sleep_quiesce_up */
static void sleep_quiesce_up(const char *why)
{
   if (!g_sq.down)
      return;
   standby_note("q up why=%s stopped=0x%X", why, g_sq.stopped);
   (void)sleep_q_up(&g_sq, &g_sq_ops);
}
/* @extract:end fn:sleep_quiesce_up */

/* The loop's park, held the way TYL's main loop sits in
 * sceKernelDelayThread while the console sleeps: no frame, no ROM read, no
 * GE list, nothing of ours inside a driver.  When the callback has done its
 * part, the gameplay power lock is dropped HERE -- the suspend then proceeds
 * with this thread already asleep.  RESUME_COMPLETE clears park_req and ends
 * the hold.  Bounded by a count of 5 ms naps rather than the clock: the naps
 * stop while the CPU is suspended, so only a suspend that never came can run
 * the count out. */
/* @extract:begin def:SLEEP_HOLD_NAPS */
#define SLEEP_HOLD_NAPS 1000u            /* 5 s awake without a suspend */
/* @extract:end def:SLEEP_HOLD_NAPS */
#ifdef REWIND
static void psp_rewind_pwr(void);         /* defined with the rewind */
#endif
/* @extract:begin fn:sleep_park_hold */
static void sleep_park_hold(void)
{
   unsigned naps = 0;
   /* Held until the wake, also when the callback let go before we parked
    * (a quiesce that overran its budget): the suspend still comes, and it
    * should find this thread asleep here, not running frames half
    * quiesced. */
   while ((g_pwr_park_req || g_pwr_sleep_pending || SLEEP_SIM_ASLEEP()) &&
          g_running)
   {
      /* The callback is done with us: it held us (cb_done), or gave up and
       * left the lock to us (park_req cleared). */
      int cb_over = g_pwr_cb_done || !g_pwr_park_req;
      /* Dropped only once a teardown has run for this sleep.  A callback
       * that gave up tore nothing down: the lock is kept until the hold
       * runs out and play resumes, and power_request_poll then asks again
       * for the suspend still waiting on it.  Dropped at once, the suspend
       * would land with the renderer in the volatile partition. */
      if (g_pwr_cb_done)
      {
         play_lock_drop("park");
#ifdef REWIND
         /* A rewind inside an old save holds the switch too; the teardown
          * has suspended it, so that hold is over -- but only this thread
          * looks (every loop), and it is here. */
         psp_rewind_pwr();
#endif
      }
      sceKernelDelayThread(5000);
      if (cb_over && ++naps >= SLEEP_HOLD_NAPS && !SLEEP_SIM_ASLEEP())
      {
         /* The switch was seen and nothing followed it.  Do not hold the
          * game forever: resume, and if the engine was torn down let the
          * wake overlay rebuild it as after a real sleep. */
         standby_note("q hold_timeout naps=%u slept=%d", naps, g_pwr_slept);
         g_pwr_sleep_pending = 0;
         g_pwr_cb_done = 0;
         if (g_pwr_slept)
            g_pwr_rebuild = 1;
         g_pwr_park_req = 0;
         break;
      }
   }
   /* Left with a sleep still pending (the game is exiting): it is this
    * thread that must now let the suspend through. */
   if (g_pwr_sleep_pending)
      play_lock_drop("park_exit");
}
/* @extract:end fn:sleep_park_hold */

