/* ---- THE FRAME LOOP (tier C, C-1: builds/health-out/design/C-1.md) ------------
 * One call per game: main() calls it after session_pre_loop and goes on to
 * session_end when g_running drops.  The loop below is main()'s text, moved
 * unchanged; the locals of main() it uses are copied in here and the ones it
 * writes copied back to *L when it ends (no loop local has its address taken,
 * C-1.md 2.3), so each name means what it meant in main().
 *   - loop_t: what the loop writes.  The first nine are main()'s per-game
 *     state, which loop_reset sets again; the six one-shot harness keys are
 *     sticky: the loop consumes each once per process, never again after an
 *     in-process return.  (rom_fail_paused, a sticky latch, went in C-0: the
 *     core's gamepak_read_failed says whether a failure was reported.)
 *   - session_ctx: what it only reads, for one game: the boot's values (bc),
 *     this game's state file, the in-process count and the autoload slot.
 *     `const` copies, so a write to one of them is a compile error.
 * Layout: frame_loop is main_psp.o's only .text.startup input, placed by
 * `pin frame_loop` (tools/layout/layout.lock; docs/LAYOUT-PINNING.md "Pins");
 * main() is cold glue in .text.fe_cold.main.  noinline: called once, it would
 * be inlined back into main(); noclone: an .isra/.constprop clone would rename
 * the symbol the lock, the hot order and the tools look up.
 * Inlining (C-1 T13): GCC's inline budget follows the caller's size, and
 * frame_loop is smaller than main() was, so it would inline other callees
 * than main()'s loop did.  Every function main()'s loop called out of line
 * and no caller inlined is `noinline` (lat_wait_step, net_frame, wake_service
 * ...: a hot-order row stays a function); gblink_service says what GCC chose. */
typedef struct loop_t
{
   int exit_code;
   const char *exit_reason;
   int ff_engaged, ff_toggled, ff_last_mode, ff_last_me;
   unsigned prev_pad;
   int chord_frames;
   int inproc_menu_armed;
   /* sticky: one-shot harness keys, consumed once per process */
   long dump_at, gedump_at, player_shot_n, ui_ctl_demo, ui_demo, wake_shot;
} loop_t;

typedef struct session_ctx
{
   const boot_ctx *bc;
   const char *state_path;
   int inproc_n;
   int autoload_state_slot;
} session_ctx;

static __attribute__((noinline, noclone, section(".text.startup.fe_frame_loop")))
void frame_loop(loop_t *L, const session_ctx *S)
{
   const long autoexit = S->bc->autoexit;
   const long dump_every = S->bc->dump_every;
   const long gedump_every = S->bc->gedump_every;
   const long ff_ini = S->bc->ff_ini;
   const long simff = S->bc->simff;
   const long player_shot_at = S->bc->player_shot_at;
   const long vhash_from = S->bc->vhash_from;
   const long vhash_to = S->bc->vhash_to;
   const long vid_prof_win = S->bc->vid_prof_win;
   const int have_script = S->bc->have_script;
   const long inproc_cycles = S->bc->inproc_cycles;
   const long inproc_frames = S->bc->inproc_frames;
   const long state_save_at = S->bc->state_save_at;
   const int inproc_via_menu = S->bc->inproc_via_menu;
   const char *const state_path = S->state_path;
   const int inproc_n = S->inproc_n;
   const int autoload_state_slot = S->autoload_state_slot;
   int exit_code = L->exit_code;
   const char *exit_reason = L->exit_reason;
   int ff_engaged = L->ff_engaged, ff_toggled = L->ff_toggled;
   int ff_last_mode = L->ff_last_mode, ff_last_me = L->ff_last_me;
   unsigned prev_pad = L->prev_pad;
   int chord_frames = L->chord_frames;
   int inproc_menu_armed = L->inproc_menu_armed;
   long dump_at = L->dump_at, gedump_at = L->gedump_at;
   long player_shot_n = L->player_shot_n, ui_ctl_demo = L->ui_ctl_demo;
   long ui_demo = L->ui_demo, wake_shot = L->wake_shot;

   while (g_running)
   {
      unsigned frames;
      SceCtrlData pd;
      unsigned pad_new;
      unsigned ctl_fired;   /* CTL_FIRE_*: the player's shortcuts this frame */
      int home_mode;        /* Menu = HOME is in force this frame          */
      int home_pressed;     /* ...and HOME was pressed (consumed once)     */
      int ffh_loop;         /* FF flip pacing: entered with a held picture */
      int ffh_fe;           /* FF flip pacing: g_flip_early outside the wait */
      int session = fe_np_session_active();
      int harness_ff = ff_ini || fe_autopilot_ff();
      /* ADR-0021: the loop body without the vblank wait is the number that
       * has to fit 16.7 ms.  Two clock reads per frame, and only while a
       * session is up. */
      unsigned rig_work_us = 0;
      uint64_t frame_t0 = (g_net_up || g_perf_rig || LAB_ON) ? net_now_us() : 0;
      LOOP_TOP();
      FE_LP_SET(FELP_L_TOP);
      g_frame_start_us = frame_t0 ? frame_t0 : net_now_us();
      if (g_perf_rig && rig_expired(g_frame_start_us))
      {
         exit_code=6; exit_reason="perf_timeout"; g_running=0; break;
      }

      /* SUSPEND HANDSHAKE, main-thread half: between frames nothing is being
       * emulated, captured or posted, so this is where the power callback may
       * take the renderer's memory.  Wait here until it has. */
      if (g_pwr_gb_flush_req)
      {
         g_pwr_gb_flush_req = 0;
         /* GB mode goes straight to gb_save_flush: writes only if cart RAM
          * changed, never touches the io thread (FRONTEND round 2). */
         if (g_pcfg.console != FE_CONSOLE_GBA)
            (void)fe_host_sram_flush(0);
      }
      if (SLEEP_SIM_NOPARK() && (g_pwr_park_req || SLEEP_SIM_ASLEEP()))
      {
         /* harness: a blocking loop with no park point */
         while ((g_pwr_park_req || SLEEP_SIM_ASLEEP()) && g_running)
            sceKernelDelayThread(5000);
      }
      else
         (void)loop_park();
      loop_unpark("resume");
      /* TYL holds scePowerLock through play. */
      play_lock_take();
      power_request_poll();
#ifdef REWIND
      if (g_rw_pool_suspect)
      {
         g_rw_pool_suspect = 0;
         psp_rewind_pool_restart();
      }
      psp_rewind_pwr();
#endif
#if defined(GPSP_STANDBY_NOTES)
      {
         /* Diagnostic build: say when a ROM page read had to reopen the
          * file (gba_memory.c load_gamepak_page). */
         extern u32 gamepak_reopens;
         static u32 reopens_seen;
         if (gamepak_reopens != reopens_seen)
         {
            reopens_seen = gamepak_reopens;
            standby_note("rom reopened n=%u page_loads=%u",
                         (unsigned)gamepak_reopens,
                         (unsigned)gamepak_page_loads);
         }
      }
#endif

      rtc_wall_publish(0);
      /* LOW LATENCY: did the previous iteration's flip latch on the vblank
       * after its submit?  (Checked before anything can submit again.) */
#ifdef GPSP_KEEP_TELEMETRY
      if (pacing_tel.lat_check &&
          (int)(sceDisplayGetVcount() - g_lat_sub_vc) >= 1)
      {
         if (vid_flip_latched())
            pacing_tel.lat_l1++;
         else
            pacing_tel.lat_l0++;
         pacing_tel.lat_check = 0;
      }
#endif
      sceCtrlPeekBufferPositive(&pd, 1);
      if (g_in_fresh && g_vb_us &&
          (int)(g_vb_us - pd.TimeStamp) > 2000)
      {
         unsigned t0 = (unsigned)sceKernelGetSystemTimeLow(), tn = t0;
         g_in_fresh_n++;
         while ((int)(g_vb_us - pd.TimeStamp) > 2000 && tn - t0 < 1500u)
         {
            sceKernelDelayThread(50);
            sceCtrlPeekBufferPositive(&pd, 1);
            tn = (unsigned)sceKernelGetSystemTimeLow();
         }
         g_in_fresh_us += tn - t0;
         if ((int)(g_vb_us - pd.TimeStamp) <= 2000)
            g_in_fresh_got++;
      }
      g_lat_poll_vc = sceDisplayGetVcount();
      g_lat_seq++;
#ifdef GPSP_KEEP_TELEMETRY
      {
         unsigned age = (unsigned)sceKernelGetSystemTimeLow() - pd.TimeStamp;
         if (age < 100000u)
         {
            pacing_tel.lat_age_n++;
            pacing_tel.lat_age_us += age;
         }
      }
#endif
      /* THE THUMBSTICK (ctl_map.h).  Its four direction bits are bits
       * pad.Buttons does not use; clear them, then add what the stick says
       * in the chosen mode.  OFF adds nothing: the word is 3.5.0's. */
      g_pad = ctl_pad_word(pd.Buttons, g_pcfg.stick, pd.Lx, pd.Ly,
                           &g_stick_state);
      g_pad = pad_swallow(g_pad);   /* a UI's answer, until released */
#ifdef GPSP_PERF_RIG
      if (pd.Lx < g_stick_min[0]) g_stick_min[0] = pd.Lx;
      if (pd.Lx > g_stick_max[0]) g_stick_max[0] = pd.Lx;
      if (pd.Ly < g_stick_min[1]) g_stick_min[1] = pd.Ly;
      if (pd.Ly > g_stick_max[1]) g_stick_max[1] = pd.Ly;
      if (g_pad_script_n)
         g_pad |= pad_script_mask(fe_host_frame_count());
      if (g_uis_n)
      {
         g_pad |= uiscript_pad();
         if (g_uis_exit >= 0)
         {
            exit_code = g_uis_exit;
            exit_reason = exit_code == 8 ? "route_fail" : "ui_script";
            g_running = 0;
         }
      }
#endif
      pad_new = g_pad & ~prev_pad;

      /* ---- Menu = HOME (docs/CONTROL-REMAP.md section 10) -------------
       * The kernel module is loaded the first time the setting is on --
       * at the first frame of the game, or when the player flips it in the
       * in-game Controls page -- and from then on this is one beat and one
       * flag per frame.  HOME is OURS only in game: never on the wake
       * overlay (g_wake_hold), never before this loop (browser, loading),
       * never after it (teardown).  If the module is missing or refused,
       * the menu stays on START+SELECT, which always works, and the player
       * is told once. */
      home_pressed = 0;
      if (g_pcfg.controls.menu_home && !home_host_tried())
         home_host_start(g_dir_base);
      home_mode = g_pcfg.controls.menu_home && home_host_ok();
#ifdef GPSP_PERF_RIG
      if (g_home_sim && g_pcfg.controls.menu_home)
         home_mode = 1;
#endif
      home_host_frame(home_mode && !g_wake_hold);
      if (home_host_take_press())
         home_pressed = 1;
#ifdef GPSP_PERF_RIG
      if (g_home_sim && home_mode && !g_wake_hold &&
          (pad_new & PSP_CTRL_HOME))
         home_pressed = 1;
      if (g_home_stall_frame &&
          fe_host_frame_count() >= g_home_stall_frame)
      {
         unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
         g_home_stall_frame = 0;
         fe_evt("home_stall begin ms=%u", g_home_stall_ms);
         /* A SPIN, not a sleep: the frozen loop the fallback exists for is
          * one that never yields, and the module's thread has to win the
          * CPU from it by priority alone. */
         while ((unsigned)sceKernelGetSystemTimeLow() - t0 <
                g_home_stall_ms * 1000u)
            ;
         fe_evt("home_stall end fallbacks=%u", home_host_fallbacks());
      }
#endif
      if (home_pressed)
         fe_evt("home_press ui=%d", ui_active());
      if (g_pcfg.controls.menu_home && !home_mode)
      {
         static int home_told;
         static unsigned home_wait;
         if (!home_told && ++home_wait > 60)
         {
            home_told = 1;
            fe_evt("home_service fallback=start_select imp_rc=%d",
                   home_host_imp_rc());
            osd_toast("HOME menu unavailable: START+SELECT opens it");
         }
      }

      /* Every remappable shortcut, evaluated ONCE from this frame's pad --
       * exactly as the hard-coded tests below used to read g_pad/pad_new,
       * which nothing changes during the iteration.  The trigger shape of
       * each lives in ctl_shortcuts() (psp/ctl_map.c) so the host test runs
       * the same code; the context gates stay at each site. */
      ctl_fired = ctl_shortcuts(&g_pcfg.controls, g_pad, pad_new);
      prev_pad = g_pad;

      /* ADR-0069: SCREENSHOT ON DEMAND, WITHOUT TOUCHING THE FILESYSTEM.
       *
       * This replaces the dump-marker file.  That mechanism asked the Memory
       * Stick "does this file exist?" once a second, on this thread, inside
       * the timed region, and the answer was always no — a 12.4 ms synchronous
       * negative lookup (PRIOWINDOW §6) to service a request a human makes
       * perhaps twice a session.  The pad is already sampled two lines above,
       * so reading a chord out of it is free.
       *
       * L+R+SELECT, on the EDGE, so holding it dumps one frame and not sixty.
       * It cannot collide with START+SELECT (abort) or SELECT+START (parked
       * handoff), which are the only other reserved gestures.  Deferred to the
       * dump site below rather than fired here, so the captured buffer is the
       * one the other dump paths capture: drawn and synced, not mid-frame. */
      /* Remappable (Controls > Screenshot).  The START guard keeps it off the
       * fixed START+SELECT menu chord whatever it is bound to; with the
       * default L+R+SELECT this is the 3.0 test exactly.  Not while the
       * Controls page is capturing a binding: the player is pressing the
       * chord to TEACH it, not to use it.  Nor over the menu (r8/x3): the
       * pending shot is taken only by the game's own dump site, so one
       * fired with the menu up waited, kept, until after the menu closed,
       * and through the in-process return into the next game. */
      if ((ctl_fired & CTL_FIRE_SHOT) && !ui_capturing() && !ui_active())
         g_dump_chord_pending = 1;
#ifdef GPSP_CATCH_SELFTEST
      /* Bisect arm "catch self-test" only: L+R+UP+TRIANGLE stores to address
       * 0 on purpose, so the owner can see the ME_CATCH exception screen work
       * once before trusting its silence. */
      if ((ctl_fired & CTL_FIRE_CATCH) && !ui_capturing())
         *(volatile unsigned *)(uintptr_t)g_catch_selftest_addr = 0xDEADu;
#endif

      /* ADR-0057: ABORT A RUN WITHOUT A HARD RESET.
       *
       * HOME is registered (sceKernelRegisterExitCallback -> g_running = 0)
       * but its system dialog is not reliably reachable while we are driving
       * the display and an autopilot script owns the pad, so in practice the
       * only way to stop a run has been holding POWER.  Hard-resetting a
       * console mid-run is how a memory stick gets a corrupt FAT, which this
       * project has already paid for twice.
       *
       * START+SELECT held is the same combo that leaves the parked handoff
       * loop, so there is ONE gesture to remember for "stop, whatever you are
       * doing".  It sets g_running = 0, which exits through the normal
       * teardown -- SRAM flushed, log closed, handoff offered -- rather than
       * yanking power. */
      /* ADR-0057: keep the system idle timer alive for the WHOLE run.
       *
       * scePowerTick was only called while a netdrv session was up, so during
       * everything before the link forms -- walking, the attendant, the save,
       * the role menu -- nothing reset it.  The autopilot injects input into
       * the emulator, not into sceCtrl, so the SYSTEM sees a console that has
       * been untouched for minutes: it dims the screen and then suspends.  A
       * suspend mid-run ends an unattended run and leaves the loop waiting for
       * a handoff that will never come.  Tick unconditionally, once a second. */
      {  /* own counter: `frames` is not yet assigned this early in the loop */
         static unsigned pwr_tick;
         if ((pwr_tick++ % 30) == 0)
         {
            /* PSP_POWER_TICK_ALL (0) is documented as "everything" but did not
             * stop the backlight dimming on ARK 6.61 -- the display timer has
             * its own tick type and evidently is not covered.  Kick both
             * explicitly rather than trusting the combined constant. */
            scePowerTick(PSP_POWER_TICK_SUSPEND);
            scePowerTick(PSP_POWER_TICK_DISPLAY);
         }
      }

      {
         static unsigned abort_held;
         if ((g_pad & (PSP_CTRL_START | PSP_CTRL_SELECT))
               == (PSP_CTRL_START | PSP_CTRL_SELECT))
         {
            if (++abort_held == 90)      /* ~1.5 s at 60 Hz */
            {
               fe_evt("abort start+select held -- ending run cleanly");
               exit_reason = "user_abort";
               g_running = 0;
            }
         }
         else
            abort_held = 0;
      }

      /* ADR-0046: what the GAME was actually offered, logged on change only.
       *
       * The joining console drops most movement input in the Union Room --
       * persistent pressing eventually registers -- and it has done so in
       * every build.  Reading the code says our path is sound: the pad is
       * sampled once per loop iteration and the core reads it live from
       * input_state() every frame.  But "I read the code and it looks fine"
       * is exactly the reasoning that has been wrong four times today, and
       * the log has never carried a single byte about input.
       *
       * A held direction should appear here as ONE press line, then frames of
       * silence, then ONE release line.  If the log shows UP held across 60
       * frames while the character did not move, our path delivered it and
       * the game declined it -- the throttle is the game's link-wait, not our
       * bug.  If instead the mask flickers or the press never appears, it IS
       * ours.  Those are opposite fixes and this is the cheapest way to
       * choose between them.
       *
       * Cost: a compare per frame, and a line only when a button actually
       * changes -- a few dozen lines a minute of real play. */
      {
         static uint32_t prev_gba_mask;
         static int      gba_mask_primed;
         uint32_t gm = fe_host_input_mask();
         if (g_log_input && (!gba_mask_primed || gm != prev_gba_mask))
         {
            gba_mask_primed = 1;
            fe_evt("input mask=0x%03x held=%u f=%u net=%s",
                   (unsigned)gm, (unsigned)(gm & prev_gba_mask),
                   fe_host_frame_count(), g_net_up ? "up" : "down");
            prev_gba_mask = gm;
         }
      }

      FE_LP_SET(FELP_L_NETF);
      net_frame();
      if (rfu_sched_d)
         rfu_sched_pause(ui_active());   /* the partner waits while our menu is open */
      FE_LP_SET(FELP_L_GBLS);
      if (g_pcfg.console != FE_CONSOLE_GBA || g_gba_cable || g_gba_cable_ok)
      {
         int gcode = 0;
         const char *gwhy = NULL;
         if (gblink_service(have_script, &gcode, &gwhy))
         {
            exit_code = gcode;
            exit_reason = gwhy;
            g_running = 0;
         }
      }
      FE_LP_SET(FELP_L_MISC);
      if (g_pcfg.console == FE_CONSOLE_GBA)
      {
#ifdef GBADHOC_MGIFT
         mgift_frame();    /* Mystery Gift is GBA cartridge-specific. */
#endif /* GBADHOC_MGIFT */
         silent_frame();   /* variant silent-wireless policy (ADR-0013) */
         rfu_link_down_drain();
         rfu_trace_drain();
         exit_assist_frame();
      }
      audio_status_frame();   /* feeds core frameskip when engaged (ADR-0019) */
      audio_report_error();   /* keeps output failures from starving emulation */

#ifdef GBADHOC_MGIFT
      if (g_pcfg.console == FE_CONSOLE_GBA &&
          mgift_shortcut_update_chord(g_pad, CTL_BIND(CTL_SC_MGIFT),
                                      !ui_active() && !have_script &&
                                      !g_wake_menu, &g_mgift_consumed))
      {
#ifdef REWIND
         (void)psp_rewind_land("mgift", 0);
#endif
         if (g_mg_on) mgift_stop(0);
         else mgift_start();
      }
#endif /* GBADHOC_MGIFT */

      /* ---- SELECT+L save state / SELECT+R load state ------------------
       *
       * The menu already offers both, but a quick-save you have to open a
       * menu for is not a quick-save. These shortcuts intentionally stay on
       * slot 1 (.st0); the menu and browser can access all five slots.  The
       * save is immediate; the load asks first unless Settings > "Quick load
       * asks first" is off (below).
       *
       * Edge-triggered on the shoulder button while SELECT is held, so
       * holding the chord fires once rather than every frame.  SELECT+START
       * (the menu) and L+R+SELECT (the screenshot) both stay distinct
       * because each names a different third button.
       *
       * Deliberately NOT active while a script is driving: the harness holds
       * button combinations the fixture never intended as chords. */
      /* SELECT + TRIANGLE: open the wake overlay on demand.
       *
       * It exists to make the overlay workshoppable without a sleep cycle --
       * and, more to the point, without hardware at all: PPSSPP has no Media
       * Engine, but the CPU renderer still fills the frame the snapshot reads,
       * so the overlay composes there exactly as it does on a console.  Every
       * layout question about this screen was previously a full deploy, a
       * sleep and a photograph. */
      if (!ui_active() && !have_script &&
          (ctl_fired & CTL_FIRE_PAUSE))
      {
         wake_snapshot();
         g_wake_menu = 1;
         fe_evt("wake_menu via=chord frame=%d", g_wake_have);
      }

      if (!ui_active() && !have_script && state_path[0])
      {
         /* EXCLUSIVE SHOULDERS.  L+R+SELECT is already the screenshot chord,
          * and without this a screenshot would also fire a save on the way
          * through.  3.0 required the other shoulder to be UP; the general
          * form is the longer-chord rule (ctl_yields): a quick save/load
          * stands down while any longer shortcut containing its chord is
          * held, which for the defaults is exactly "the other shoulder". */
/* @extract:begin qs:dispatch */
         if (ctl_fired & CTL_FIRE_SAVE)
         {
            if (link_locks_states())
               osd_toast("Cannot save during a wireless session");
            /* THE QUICK SAVE CAN ASK (docs/UI-OVERLAY.md section 15.2,
             * issue #31): Settings > "Quick save asks first", off by
             * default (config.ini quick_save_ask).  On, a quick save over a
             * saved slot 1 opens the SAVE STATES page with its overwrite
             * question: X saves (UI_ACT_SAVESTATE below, and the menu
             * closes), any other key is Resume.  An empty slot 1 has
             * nothing to lose: it saves at once.  Off: not one call more
             * than before (the && stops at the setting). */
            else if (g_pcfg.quick_save_ask &&
                     fe_host_state_recover(state_path) >= 0 &&
                     file_exists(state_path))
            {
               fe_evt("state_save_ask via=chord");
               menu_open();
               if (!ui_state_save_ask(1))
               {
                  ui_close();
                  osd_toast(state_save_slot(state_path, 1) == 0
                               ? "State saved" : "Save failed");
               }
            }
            else if (state_save_slot(state_path, 1) == 0)
            {
               osd_toast("State saved");
            }
            else
               osd_toast("Save failed");
         }
         else if (ctl_fired & CTL_FIRE_LOAD)
         {
            /* NEVER LOAD DURING A WIRELESS SESSION. Restoring a state while
             * a trade or Mystery Gift transfer is active rewinds the game's
             * RFU state underneath a live transport. */
            if (link_locks_states())
               osd_toast("Cannot load during a wireless session");
            /* THE QUICK LOAD ASKS FIRST (docs/UI-OVERLAY.md section 14.3),
             * with the SAVE STATES page's own question, opened over the
             * game.  The default quick load is a shoulder away from the
             * quick save, and a load replaces everything since the last
             * save.  The d-pad picks another saved slot; X loads
             * (UI_ACT_LOADSTATE below, the page's load); O is Resume.  Nothing
             * in slot 1: said at once, no menu -- one stat, after the
             * interrupted-save recovery the page's own scan would run.
             * Keyed on the action, so any binding of bind_quickload asks.
             * Settings > "Quick load asks first" off (config.ini
             * quick_load_ask = 0): the load below, as before. */
            else if (g_pcfg.quick_load_ask)
            {
               if (fe_host_state_recover(state_path) < 0 ||
                   !file_exists(state_path))
                  osd_toast("No saved state");
               else
               {
                  fe_evt("state_load_ask via=chord");
                  menu_open();
                  if (!ui_state_load_ask(1))
                  {
                     ui_close();
                     osd_toast("No saved state");
                  }
               }
            }
            else if (psp_state_load(state_path, "chord") == 0)
            {
               fe_evt("state_load via=chord");
               osd_toast("State loaded");
            }
            else
               osd_toast("No saved state");
         }
/* @extract:end qs:dispatch */
      }

      /* Did we come back?  This line existing at all is the answer to
       * "does the app survive a standby" -- the question that decides
       * whether resuming is a re-init job or a much larger one. */
      if (g_pwr_resume)
      {
         g_pwr_resume = 0; /* @lab */ LAB_HOOK(lab_power_resumed();)
         fe_evt("power back frames=%u net=%d me=%d me_up=%d",
                fe_host_frame_count(), g_net_up, g_me_rend, me_host_up());
         /* The firmware may wake at its own clock: `was=` says. */
         psp_clock_apply("wake");
         osd_toast("Resumed");
      }

      /* ---- in-game menu (Select+Start held ~1/4 s, plan §8) ---------- */
      if (!ui_active())
      {
         /* A ROM PAGE COULD NOT BE READ (the card is out, the file gone):
          * the core mapped it as zeros and raised a sticky flag
          * (gba_memory.c load_gamepak_page).  The game pauses here, at the
          * frame boundary, in the menu, with the reason; when the menu
          * closes those pages are read again before the next frame.  A
          * failure again pauses again; Quit to game list is the way out.
          * The core's flag says whether this failure was reported
          * (fe_host_rom_read_poll; code-health C-0). */
         if (fe_host_rom_read_poll() == FE_ROM_READ_PAUSE)
         {
            menu_open();
            osd_toast("Cannot read the game from the Memory Stick.");
            osd_toast("Check it, then Resume to try again.");
         }
         if (home_mode)
         {
            /* Menu = HOME: a START+SELECT tap is the GAME's (the hold above
             * still ends the run), and HOME opens the menu. */
            chord_frames = 0;
            if (home_pressed && !have_script)
            {
               home_pressed = 0;
               fe_evt("menu_open via=home");
               menu_open();
            }
         }
         else if ((g_pad & (PSP_CTRL_SELECT | PSP_CTRL_START)) ==
             (PSP_CTRL_SELECT | PSP_CTRL_START) &&
             (!have_script || UISCRIPT_DRIVES))
         {
            if (++chord_frames >= 15)
            {
               chord_frames = 0;
               menu_open();
            }
         }
         else
            chord_frames = 0;

         if (ui_demo && fe_host_frame_count() >= 300)
         {
            ui_demo = 0;
            menu_open();
            ui_demo_start();
         }
         if (wake_shot && fe_host_frame_count() >= (unsigned)wake_shot)
         {
            wake_shot = 0;
            ui_wake_shot_arm();
            wake_snapshot();
            g_wake_menu = 1;
            fe_evt("wake_menu via=harness frame=%d", g_wake_have);
         }
         if (ui_ctl_demo && fe_host_frame_count() >= 300)
         {
            int which = (int)ui_ctl_demo;
            ui_ctl_demo = 0;
            menu_open();
            if (which >= 3)
               ui_home_demo_start(which);   /* 3 menu toggle, 4 delete */
            else
               ui_controls_demo_start();
         }
      }

      if (state_save_at && fe_host_frame_count() == (unsigned)state_save_at)
      {
         int ss_rc = state_save_slot(state_path, 1);
         FE_EVT_ONLY(ss_rc);
         fe_evt("state_save_at f=%ld rc=%d", state_save_at, ss_rc);
      }
#ifdef GPSP_PERF_RIG
      if (g_ss_at && !g_ss_browser && inproc_n == g_ss_game &&
          fe_host_frame_count() >= (unsigned)g_ss_at)
         sleep_sim_fire();
#ifdef REWIND
      if (g_ss_rw_tick && !g_ss_fired && g_rw_on && fe_host_rewind_busy() &&
          (g_ss_rw_tick > 0 ? ++g_ss_rw_seen == g_ss_rw_tick
                            : fe_host_rewind_unvetted() &&
                              ++g_ss_rw_seen == -g_ss_rw_tick))
      {
         fe_host_rewind_info ri;
         fe_host_rewind_info_get(&ri);
         fe_evt("sleep_sim rewind_tick=%ld mach=%u now=%u unvetted=%d",
                g_ss_rw_seen, (unsigned)ri.mach_frame, (unsigned)ri.now_frame,
                fe_host_rewind_unvetted());
         sleep_sim_fire();
      }
#endif
#endif
      /* Harness `inproc_cycles`: quit to the game list as the menu does. */
      if (inproc_n < inproc_cycles && g_running &&
          fe_host_frame_count() >= (unsigned)inproc_frames)
      {
         if (!inproc_via_menu)
         {
            fe_evt("inproc_harness quit n=%d frame=%u", inproc_n,
                   fe_host_frame_count());
            wake_snapshot();
            g_relaunch = RELAUNCH_GAMELIST;
            exit_reason = "game_list";
            g_running = 0;
         }
         else if (!inproc_menu_armed && !ui_active())
         {
            fe_evt("inproc_harness menu n=%d frame=%u", inproc_n,
                   fe_host_frame_count());
            inproc_menu_armed = 1;
            menu_open();
            ui_home_demo_start(5);
         }
      }

      /* Perf-rig autoload: one shot, ~half a second in, once video/audio/ME
       * are all up.  The scripted run then starts inside the saved scene. */
      if (g_autoload_state && (fe_host_frame_count() >= 30 ||
                               (g_gblink_role &&
                                g_pcfg.console != FE_CONSOLE_GBA)))
      {
         g_autoload_state = 0;
         char autoload_path[PSP_FILE_PATH_CAP];
         int state_rc = psp_state_path_for_slot(autoload_path,
                                                sizeof(autoload_path),
                                                state_path,
                                                (unsigned)autoload_state_slot)
            == 0 ? psp_state_load(autoload_path, "autoload") : -1;
         fe_evt("autoload_state slot=%d rc=%d shelf=%d", autoload_state_slot,
                state_rc, g_autoload_from_shelf);
         /* A GBA cable session links from exactly the loaded state. */
         if (state_rc == 0 && g_gba_cable)
            fe_host_gblink_hold(1);
         if (state_rc != 0 && g_autoload_from_shelf)
         {
            /* THE PLAYER ASKED FOR THIS STATE; THE RIG DID NOT.  For a rig
             * (`load_state = 1`) a missing state invalidates the run, so it
             * exits 6.  The state shelf used the same branch, so a state the
             * core refused -- a GB state from before a DMG-palette change, a
             * truncated file -- quit the app to the XMB, half a second after
             * the game had booted.  The game is running and the core is
             * untouched (every loader validates before it writes), so keep
             * playing from the boot and say why, as the in-game Load does. */
            osd_toast("Could not load state %d", autoload_state_slot);
         }
         else if (state_rc != 0)
         {
            exit_code=6; exit_reason="state_load_failed"; g_running=0; break;
         }
      }

#ifdef GPSP_CHEATS
      cheats_link_poll();
#endif

      /* Menu = HOME: HOME on the menu's top page closes it, like Resume, and
       * this iteration carries on into the game. */
      if (home_pressed && ui_active() && ui_home_press())
      {
         home_pressed = 0;
         pad_swallow_arm(g_pad);     /* whatever is held is not the game's */
         /* nor this iteration's pad, read before it, or its edges and
          * shortcuts (turbo, fast-forward, the video cycle, rewind) */
         g_pad = pad_new = ctl_fired = 0;
      }

#ifdef REWIND
      /* The menu opened, or the pause overlay is about to, in the middle of
       * a rewind: land first.  Everything they can do (a state save, a
       * session start, a cheat switched, Share, Mystery Gift, quit) then
       * finds the machine on the frame on screen, vetted. */
      if ((ui_active() || g_wake_menu) && g_rw_on && fe_host_rewind_busy())
         (void)psp_rewind_land(ui_active() ? "menu" : "pause", 0);
#endif

      if (ui_active())
      {
         /* Core paused; wireless keeps pumping; UI draws + acts. */
         ui_action act;
         LOOP_PHASE(5);
         /* A sleep taken with the menu open: the wake overlay, and on
          * Continue the rebuild, before the menu draws again (see
          * wake_service).  Then a fresh iteration, so the pad is read
          * after the answer -- and the answer's X, still down, is not read
          * by the menu as a press of its own. */
         if (g_pwr_rebuild || g_wake_menu)
         {
            int w = wake_service();
            if (w == WAKE_QUIT)
               exit_reason = "game_list";
            else if (!w)
               ui_swallow_pad();
            continue;
         }
         /* The menu is open: auto-fire stops, and no menu key (X is a
          * likely turbo button) can start or toggle one from in here. */
         if (g_turbo.live)
            ctl_turbo_stop(&g_turbo);
         ui_set_turbo_locked(link_session_any());
         link_ui_view();
         act = ui_frame(g_pad, link_locks_states(), g_session_info);
         switch (act)
         {
         case UI_ACT_SAVESTATE:
         {
            /* The Save states page stays open and reports on its own
             * plate (docs/UI-OVERLAY.md section 13); the toast is for any
             * other caller. */
            int rc = state_save_slot(state_path, (unsigned)ui_state_slot());
            if (!ui_state_save_done(rc))
               osd_toast(rc == 0 ? "State saved" : "State save FAILED");
            break;
         }
/* @extract:begin sl:case_loadstate */
         case UI_ACT_LOADSTATE:
         {
            char selected_state[PSP_FILE_PATH_CAP];
            int rc;
            rc = psp_state_path_for_slot(selected_state,
                                         sizeof(selected_state),
                                         state_path,
                                         (unsigned)ui_state_slot()) == 0
               ? psp_state_load(selected_state, "menu") : -1;
            /* Loaded: the menu closes, back into the game. */
            if (!ui_state_load_done(rc))
               osd_toast(rc == 0 ? "State loaded" : "No state to load");
            break;
         }
         case UI_ACT_NET_HOST:
/* @extract:end sl:case_loadstate */
            ui_net_action(1);
            break;
         case UI_ACT_NET_JOIN:
            ui_net_action(0);
            break;
         case UI_ACT_NET_DISCONNECT:
            if (fe_host_gblink_active())
            {
               /* A link session ends by agreement, so both saves match:
                * the teardown follows once it has (gblink_service). */
               fe_host_gblink_request_end();
               osd_toast("Ending the link...");
               break;
            }
            if (g_link_wait)
            {
               char rm[ROOM_LABEL_CAP];
               snprintf(rm, sizeof(rm), "%s", g_link_room);
               net_teardown();
               g_link_menu = 0;
               osd_toast("Stopped hosting %s", rm);
               break;
            }
            net_teardown();
            osd_toast("Disconnected");
            break;
#ifdef GBADHOC_MGIFT
         case UI_ACT_NET_MGIFT:
            if (g_pcfg.console == FE_CONSOLE_GBA)
               mgift_start();
            else
               osd_toast("Mystery Gift needs GBA");
            break;
         case UI_ACT_NET_MGIFT_STOP:
            if (g_pcfg.console == FE_CONSOLE_GBA)
               mgift_stop(0);
            break;
#endif /* GBADHOC_MGIFT */
         case UI_ACT_RESET:
         {
            /* "Reset game" (soft reset, fe_host_reset): the menu has
             * already closed.  A GB reboot that failed leaves no machine:
             * back to the game list by the normal clean exit. */
            int rc = psp_game_reset();
            if (rc == -1)
            {
               osd_toast("Reset failed: back to the game list");
               ui_settings_save();
               g_relaunch = RELAUNCH_GAMELIST;
               exit_reason = "reset_failed";
               g_running = 0;
            }
            else
               osd_toast(rc == 0 ? "Game reset" :
                         rc == -2 ? "Reset is off during link play"
                                  : "Reset refused: the save could not be written");
            break;
         }
         case UI_ACT_EXIT:
            g_running = 0;
            break;
         case UI_ACT_GAMELIST:
            /* Full clean shutdown (SRAM flushed, threads stopped), then
             * back to the ROM browser -- in this process when the unload is
             * proven (inproc_refusal), by the ADR-0071 relaunch otherwise.
             * The hold screen after the loop says what is happening.
             * One config.ini write: ui_close (the return) and the
             * teardown's flush find nothing left unsaved (F3). */
            ui_settings_save();
            g_relaunch = RELAUNCH_GAMELIST;
            exit_reason = "game_list";
            g_running = 0;
            break;
         case UI_ACT_RELAUNCH:
            /* ADR-0071: the trading profile changed.  Persist it, then leave
             * the loop by the NORMAL exit path — SRAM flushed, io thread
             * stopped, log closed — and relaunch at the very end, where the
             * USB handoff already proves it is safe to replace the process. */
            ui_settings_save();
            g_relaunch = RELAUNCH_PROFILE;
            exit_reason = "profile_change";
            g_running = 0;
            osd_toast("Restarting: Media Engine %s",
                      g_pcfg.me_mode ? "ON" : "OFF");
            break;
         default:
            break;
         }
         /* The menus carry the session themselves (the cluster says
          * HOSTING / LINKED, the Wireless row its mode): the chip would sit
          * on the footer's hints (docs/GBA-LINK-UI.md).  Toasts still show. */
         osd_chip_session(NULL);
         osd_draw();
         if (g_link_kind != LK_NONE)
            link_chip();              /* hidden again while LINKING */
         else if ((g_net_up || g_mg_on) && g_pcfg.osd_wireless)
            osd_chip_session(g_session_info);
         UISCRIPT_DUMP();
         sceDisplayWaitVblankStart();
         if (!g_wake_hold)
            vid_swap();
         /* Closed (Resume, a state load, Host/Join): the X or O that
          * closed it is still down, and is not the game's (pad_swallow).
          * The pad it answered, not one read now: a press made while the
          * action ran (Host/Join waits up to 15 s) is the game's. */
         if (!ui_active())
            pad_swallow_arm(g_pad);
         continue;
      }

      /* ---- Triangle: cycle video preset ------------------------------ */
      /* Remappable, and unbindable -- the owner's own complaint was an
       * accidental Triangle resizing the picture.  Deliberately NOT subject
       * to the longer-chord rule: 3.0 also cycled on SELECT+TRIANGLE, and
       * the defaults reproduce 3.0 exactly (docs/CONTROL-REMAP.md). */
#ifdef REWIND
      /* The rewind button: a level, read every loop.  Where the setting is
       * off the same button cycles the video preset while that is on AUTO
       * (unbound, and not set to none: ctl_video_auto), so Triangle is
       * never dead unless the player asked for it (GitHub #31).  With the
       * setting on in a GB / GBC game it says why nothing happens. */
      g_rw_held = (g_rw_on || g_rw_gb) &&
                  ((ctl_fired & CTL_FIRE_REWIND) || fe_autopilot_rewind());
      if (!g_rw_on && !g_rw_gb && (ctl_fired & CTL_FIRE_REWIND_PRESS) &&
          ctl_video_auto(&g_pcfg.controls))
         ctl_fired |= CTL_FIRE_VIDEO;
#endif
      if (ctl_fired & CTL_FIRE_VIDEO)
      {
         const char *name = vid_cycle_preset();
         g_pcfg.scale  = vid_scale_mode();
         g_pcfg.smooth = vid_smooth();     /* the effect row is untouched */
         g_pcfg.filter = vid_filter();
         /* This console's profile only: 2-4 INI rewrites, not ~50. */
         pcfg_display_save();
         fe_evt("video_mode scale=%s filter=%s",
                vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter));
         osd_toast("Video: %s", name);
      }

      /* ---- fast-forward (plan §4.4) ---------------------------------- */
      {
         int want_ff;
         int ff_press = (ctl_fired & CTL_FIRE_FF_PRESS) ? 1 : 0;
         if (g_pcfg.ff_hold)
            want_ff = (ctl_fired & CTL_FIRE_FF_HELD) ? 1 : 0;
         else
         {
            if (ff_press)
               ff_toggled = !ff_toggled;
            want_ff = ff_toggled;
         }
         if (simff && fe_host_frame_count() >= 300 &&
             fe_host_frame_count() < 300 + (unsigned)simff)
            want_ff = 1;   /* harness-simulated FF hold */
         if (session && want_ff)
         {
            /* Interlock: FF forced 1x during wireless sessions. */
            if (ff_press)
               osd_toast("Fast-forward locked: wireless session");
            want_ff = 0;
            ff_toggled = 0;
         }
         if (want_ff != ff_engaged ||
             (want_ff && (ff_last_mode != pcfg_ff_mode() || ff_last_me != g_me_rend)))
         {
            ff_engaged = want_ff;
            if (ff_engaged)
            {
               /* The core's own frameskip stays off throughout FF: 3x
                * renders every frame, and the two unlimited presets choose
                * their drawn frames with the draw gate below, which forces
                * each frame's skip directly (fe_host_skip_override). */
               fe_host_option_set_live("gpsp_frameskip", "disabled");
               ff_last_mode = pcfg_ff_mode();
               ff_last_me = g_me_rend;
               g_ffc_on = 0;     /* a new multiplier re-anchors its ceiling */
               g_ffm_acc = 0;
               osd_chip_ff(ff_chip_text());
               fe_evt("ff_user on mult_x10=%d", g_pcfg.ff_mult_x10);
            }
            else
            {
               /* Hand frameskip back to the session policy if a session
                * came up while FF was held (ADR-0019), else off.  The
                * policy defaults to leaving it disabled. */
               skip_policy_reapply();
               osd_chip_ff(NULL);
               fe_evt("ff_user off");
            }
         }
         g_ff_uncapped = harness_ff || (ff_engaged && g_pcfg.bench_mode) ||
                         (ff_engaged && g_pcfg.ff_mult_x10 == 0);
         g_ff_mult = ff_engaged && !g_ff_uncapped;
         g_ff_user = ff_engaged;
         g_ff_gate = ff_engaged && !g_pcfg.bench_mode &&
                     (pcfg_ff_mode() == PCFG_FF_SMOOTH ||
                      pcfg_ff_mode() == PCFG_FF_UNLIMITED);
         /* FF FLIP PACING bit 2: 3x through the gate under a 3x ceiling. */
         g_ffh_3x = (g_ff_flip_pace & 4) && g_ff_mult && !g_pcfg.bench_mode;
         if (g_ffh_3x)
         {
            g_ff_mult = 0;
            g_ff_uncapped = g_ff_gate = 1;
         }
         if (session)
            g_ff_uncapped = g_ff_mult = g_ff_user = g_ff_gate = 0;
#ifdef REWIND
         /* A rewind is always at display speed: while the button is down,
          * or its press is still fading, or the picture is on its way back
          * to play, this loop is an ordinary paced one.  Fast-forward
          * itself stays engaged and takes over again afterwards. */
         if (g_rw_on && (g_rw_held || g_rw_showing || g_rwh.intensity > 0.0f))
            g_ff_uncapped = g_ff_mult = g_ff_gate = 0;
#endif

         /* THE DRAW GATE (ff_gate.h): decide, before the frame is emulated,
          * whether it will be drawn -- at most 30 a second for Unlimited, 60
          * for Smooth.  A frame turned away is emulated in full but the core
          * neither renders nor captures it (no ME post, no mid-frame log).
          * Off, the override is -1 and the frameskip option rules as before. */
         {
            unsigned period = !g_ff_gate ? 0u :
               pcfg_ff_mode() == PCFG_FF_SMOOTH || g_ffh_3x ?
                  FF_GATE_PERIOD_SMOOTH_US : FF_GATE_PERIOD_UNLIMITED_US;
            if (!g_ffh_3x || !g_ff_uncapped)
               g_ffc_on = 0;       /* the 3x ceiling re-anchors next time */
            int draw = 1;
            if (period != g_ff_gate_st.period_us)
               ff_gate_arm(&g_ff_gate_st, period);
            if (period)
            {
               /* 1.5x / 2x on pace: the ceiling's last frame (ffh_cap_draw) */
               draw = g_ffh_3x ? ffh_cap_draw() :
                                 ff_gate_frame(&g_ff_gate_st, net_now_us());
               if (!draw)
                  TEL(g_ffw_gated++);
            }
            fe_host_skip_override(period ? !draw : -1);
         }
      }

      /* ADR-0082: present the ME-staged frame BEFORE running the core, so
       * the GE rasterises through the whole retro_run below and the
       * pre-swap sync is ~0.  No-op unless me_video is active. */
      /* One wake, one rebuild, on this thread.
       *
       * Ordering matters for how this LOOKS.  The LCD powers back on showing
       * whatever is still in VRAM -- the frame from before the sleep -- so the
       * overlay has to be the very next thing drawn.  The swap sites below are
       * suppressed while a wake is pending (see g_wake_hold), otherwise the
       * loop iteration that was in flight when the machine suspended finishes
       * and puts the stale frame up first, which reads as a flicker. */
      {
         int w = wake_service();
         if (w)
         {
            /* WAKE_YIELD: the switch, slid on the pause screen.  Round to
             * the top, which parks; nothing of this frame is presented or
             * run first. */
            if (w == WAKE_QUIT)
               exit_reason = "game_list";
            continue;
         }
      }
      if (g_wake_answered)
      {
         /* Continue on the wake overlay (PR #16): this iteration's edges
          * and shortcuts were read before it; turbo_frame below reads
          * pad_new.  (wake_service zeroed g_pad and armed the swallow.) */
         g_wake_answered = 0;
         pad_new = ctl_fired = 0;
      }
      FE_LP_SET(FELP_L_PRES0);
      /* FF FLIP PACING: a picture held by the last loop is still the back
       * buffer.  Whether this loop draws a NEW one is what g_drew says below
       * (the HUD is drawn on a new one only); if none, the held one stands. */
      ffh_loop = g_ffh_held && g_ff_gate;
      g_ffh_held = 0;
      if (ffh_loop)
         g_drew = 0;
      me_video_present();
/* @extract:begin lowlat:top */
      /* LOW LATENCY tier 0b: a render that finished during the previous
       * iteration is shown NOW, not after the next post retires it. */
      g_ll_iter_h = 0;
      me_rend_retire_if_idle();
#ifdef REWIND
      if (psp_rewind_present())
         ;                    /* a rewind is on screen: its frame, not the ME's */
      else
#endif
      {
         /* Tier 1: this iteration's own render is presented in its wait.
          * Steady in tier 1 nothing is waiting here (the last render was
          * shown in the last wait); if something is, it is shown and the
          * mode is left -- a frame is never skipped (TIER 1 POLICY). */
         if (g_ll_mode_h &&
             (!mer_lowlat_active() ||
              (g_mer_ready >= 0 && g_mer_ready != g_mer_last_presented)))
            ll_to_b();
         if (g_ll_mode_h)
            g_ll_iter_h = 1;
         else if (!dfr_defer())   /* LATE PRESENT: N's render not in yet */
            me_rend_present(); /* ME renderer: swap-phase present (FF triage) */
      }
#ifdef REWIND
      if (g_drew && (g_rw_on || g_rwh.denied))
/* @extract:end lowlat:top */
         psp_rewind_hud();
#endif
      if (ffh_loop && !g_drew)
      {
         g_drew = 1;              /* nothing new: the held picture stands */
         TEL(pacing_tel.ffh_vb++);
      }
      g_mer_posted = 0;

      /* FPS chip: emulated-frame rate over a ~1 s window, one decimal (a
       * healthy reading is the GBA's own 59.7).  A window with zero frames
       * (menu time) re-arms silently instead of flashing "0.0". */
      if (g_pcfg.show_fps)
      {
         unsigned fnow = (unsigned)sceKernelGetSystemTimeLow();
         if (!g_fps_win_us)
         {
            g_fps_win_us = fnow ? fnow : 1;
            g_fps_emu_frames = 0;
            g_fps_drawn = 0;
         }
         else if (fnow - g_fps_win_us >= 1000000u)
         {
            if (g_fps_emu_frames)
            {
               char fbuf[64];
               int  fn;
               unsigned smc_now = flush_ram_smc;
               unsigned dma_now = flush_ram_dma;
               unsigned el    = fnow - g_fps_win_us;
               unsigned fps10 = (unsigned)((unsigned long long)
                  g_fps_emu_frames * 10000000ull / el);
               unsigned drawn = (unsigned)((unsigned long long)
                  g_fps_drawn * 1000000ull / el);
               /* "emulated (rendered)" — the same pair FrogGBA's counter
                * reports, so the two can be photographed side by side. */
               fn = snprintf(fbuf, sizeof(fbuf), "%u.%u fps (%u)",
                             fps10 / 10, fps10 % 10, drawn);
               /* Cart bigger than the ROM cache: append the page-fault rate
                * (32 KiB stick reads/window) — the cause of any hitching. */
               if (gamepak_must_swap() && fn > 0)
                  fn += snprintf(fbuf + fn, sizeof(fbuf) - fn, "  pg %u",
                                 (unsigned)(gamepak_page_loads -
                                            g_fps_last_pg));
               /* Cache wipes per second, SPLIT BY SOURCE — they need
                * different fixes (ADR-0029).  `s` = a CPU store from
                * translated code hit a tagged halfword.  `d` = a DMA wrote
                * into tagged IWRAM/EWRAM; a DMA knows its whole destination
                * range up front, so that one is addressable by a range
                * invalidate.  Shown only when non-zero. */
               /* Dynarec diagnostics are for the lab, not the OSD: a player
                * running a ROM hack should see "59.7 fps (60)", not a row of
                * flush counters. */
               if (g_pcfg.bench_mode &&
                   (smc_now != g_fps_last_smc || dma_now != g_fps_last_dma) &&
                   fn > 0)
                  /* s = wipes/s, f = % of wall time INSIDE the flush (the
                   * tag memset), M = MiB/s memset, w = whole-region wipes/s
                   * (32K IWRAM / 256K EWRAM each). If f is large, the cost is
                   * the memset, not the re-translation — and the fix is to
                   * clear only the pages that carry tags, not the whole span. */
                  snprintf(fbuf + fn, sizeof(fbuf) - fn,
                           "  s%u x%u j%u%% f%u%%",
                           smc_now - g_fps_last_smc,
                           (unsigned)(smc_blk_xlat_total -
                                      g_fps_last_xlat),
                           /* j = % of wall time COMPILING code (needs
                            * core_phase = 2 in config.ini; reads 0 without
                            * it).  This is the cost the wipes force. */
                           (unsigned)((unsigned long long)
                              (cph_jit_total - g_fps_last_jit) * 100ull / el),
                           (unsigned)((unsigned long long)
                              (smc_flush_us - g_fps_last_fus) * 100ull / el));
               osd_chip_fps(fbuf);
            }
            g_fps_last_pg  = gamepak_page_loads;
            g_fps_last_smc  = flush_ram_smc;
            g_fps_last_dma  = flush_ram_dma;
            g_fps_last_fus  = smc_flush_us;
            g_fps_last_fkb  = smc_flush_bytes;
            g_fps_last_wide = smc_flush_wide;
            g_fps_last_xlat = smc_blk_xlat_total;
            g_fps_last_rom  = flush_rom_total;
            g_fps_last_jit  = cph_jit_total;
            g_fps_win_us = fnow ? fnow : 1;
            g_fps_emu_frames = 0;
            g_fps_drawn = 0;
         }
      }
      else if (g_fps_win_us)
      {
         g_fps_win_us = 0;
         osd_chip_fps(NULL);
      }

      /* ---- run the core ---------------------------------------------- */
      LOOP_PHASE(2);
#ifdef GPSP_CHEATS
      /* Again, immediately before the frame(s): whatever brought a session
       * up earlier in this iteration, no frame of it runs with codes on. */
      cheats_link_poll();
#endif
      /* Turbo buttons, from this iteration's pad -- the real pad (and the
       * harness's raw pad replay, which stands in for it), never autopilot
       * input, which is injected after the mapping.  Skipped outright while
       * nothing is bound and nothing is firing. */
      if (g_turbo.live || ctl_turbo_bound(&g_pcfg.controls))
         turbo_frame(g_pad & ~g_mgift_consumed, pad_new & ~g_mgift_consumed,
#ifdef REWIND
                     /* A rewind GESTURE stops every train, from the press
                      * of the control until a tick says PLAY -- so the
                      * landing after the control is let go is covered too
                      * (fe_host_rewind_busy: "no tick has said PLAY yet").
                      * Nothing fires while the game runs backwards or
                      * catches up (those frames take the LOGGED input
                      * anyway), and a Toggle is off when play resumes.
                      * g_rw_held alone would miss the landing; it is kept
                      * for the loop the press arrives on, before the first
                      * tick has made the gesture live. */
                     !(g_rw_on && (g_rw_held || fe_host_rewind_busy()))
#else
                     1
#endif
                     );
      if (g_gblink_exiting)
      {
         /* harness: the link session has ended; no more frames */
      }
      else if (g_ff_mult)
      {
         /* ff_mult_x10 / 10 frames a loop, the last one shown: 3x runs 3,
          * 2x 2, 1.5x 1, 2, 1, 2 ... (tenths owed carried in g_ffm_acc). */
         int i, n;
/* @extract:begin ff:mult_frames */
         g_ffm_acc += (unsigned)g_pcfg.ff_mult_x10;
         n = (int)(g_ffm_acc / 10u);
         g_ffm_acc %= 10u;
/* @extract:end ff:mult_frames */
         for (i = 0; i < n; i++)
         {
            g_blit_suppress = (i != n - 1);
            if (fe_host_autopilot_gate())
               fe_autopilot_frame();
            gblink_script_disconnect();
            DRPROF_PHASE(13);        /* DRPH_RETRO until the CPU loop runs */
#ifdef REWIND
            if (!psp_rewind_tick(1))
            {
               DRPROF_PHASE(15);
               break;                /* a rewind tick: one a loop, no frame */
            }
#endif
            fe_host_run_frame();
            DRPROF_PHASE(15);        /* DRPH_OUTSIDE */
         }
         g_blit_suppress = 0;
      }
      else if (!fe_host_rfu_gate())
      {
         /* scheduled ticks: the partner's tick is still missing; no frame */
      }
      else
      {
         FE_LP_SET(FELP_L_AP);
         if (fe_host_autopilot_gate())
            fe_autopilot_frame();
         gblink_script_disconnect();
         FE_LP_SET(FELP_L_RUN);
         DRPROF_PHASE(13);           /* DRPH_RETRO until the CPU loop runs */
#ifdef REWIND
         if (psp_rewind_tick(g_ff_uncapped))
#endif
         fe_host_run_frame();
         DRPROF_PHASE(15);           /* DRPH_OUTSIDE */
#ifdef GPSP_PERF_RIG
         gblink_cpu_scale();         /* harness gblink_cpu_x100, else no-op */
#endif
      }
      FE_LP_SET(FELP_L_POST);
      if (g_gblink_screen)
         gblink_draw_status();    /* link session: setting up or just ended */
#ifdef GPSP_PERF_RIG
      if (g_uis_linking[0])
         uiscript_linking_draw();
#endif
      pace_burn();               /* ADR-0027 harness knob, normally a no-op */
      /* A battery save made during fast-forward must not wait for FF to end
       * to reach the stick: the writer runs below us and FF never blocks.
       * No-op unless FF is on AND the game has an unpersisted save; then a
       * ~1 ms yield every couple of frames until the writer confirms. */
      (void)fe_host_sram_ff_nudge(g_ff_user || g_ff_uncapped);
      {  /* the core reset the game after a guest fault (see fe_host.c) */
         static unsigned faults_seen;
         unsigned faults = fe_host_guest_faults();
         if (faults != faults_seen)
         {
            faults_seen = faults;
            osd_toast("Emulation fault - game was reset");
         }
      }
      LOOP_PHASE(3);
      frames = fe_host_frame_count();

      if (vid_prof_win > 0)
         vid_prof_frame((unsigned)vid_prof_win);
#ifdef GPSP_KEEP_TELEMETRY
      {
         /* core_health on fe_host's 600-frame heartbeat cadence.  A
          * threshold, not `% 600`: 3x fast-forward advances 3 frames a loop. */
         static unsigned health_next = 600;
         if (frames >= health_next)
         {
            core_health_evt("hb");
            health_next = frames - frames % 600u + 600u;
         }
      }
#endif

      if (vhash_to && frames >= (unsigned)vhash_from &&
          frames <= (unsigned)vhash_to)
         vhash_frame();
#ifdef GPSP_PERF_RIG
      /* Every frame, independent of vhash: vhash also logs a line per frame
       * through the event ring, which is exactly the flood that starves it. */
      FE_LP_SET(FELP_L_SHASH);
      if (g_shash_on)
         shash_frame();
      jit_coh_frame(frames);
      FE_LP_SET(FELP_L_POST);
#endif
#ifdef DRPROF_HW
      if (frames % 300u == 0)
         drprof_window(frames);
#endif

      if (dump_at && frames >= (unsigned)dump_at)
      {
         dump_at = 0;
         dump_frame_bmp();
      }
      if (dump_every && (frames % (unsigned)dump_every) == 0)
         dump_frame_bmp();
      if (fe_autopilot_dump_pending())
         dump_frame_bmp();
#ifdef GPSP_PERF_RIG
      /* HARNESS ONLY.  A script asked to reload the savestate.  The R-trigger
       * chord that does this for a player is disabled while a script runs, so
       * this is the only route a script has.
       *
       * Deliberately NOT gated on g_net_up the way the chord is: the chord
       * refuses mid-session because rewinding one console desynchronises the
       * peer's trade protocol, and there is no peer here.  A stress script that
       * wanted to reload during a session would be testing something else.
       *
       * Reloading is the point: it restores guest RAM under a translation cache
       * that keeps its blocks, which is exactly where stale SMC evidence or a
       * stale block would show itself. */
      if (state_path[0] && fe_autopilot_state_pending())
      {
         int ap_rc = psp_state_load(state_path, "autopilot");
         fe_evt("ap_state_load rc=%d", ap_rc);
         FE_EVT_ONLY(ap_rc);
         smc_gates_reload_evt();
      }
#endif
      if (g_dump_chord_pending)      /* ADR-0069, set at the pad read above */
      {
         g_dump_chord_pending = 0;
         player_screenshot();        /* issue #13: named, PNG, a toast */
      }
      if (player_shot_at && player_shot_n > 0 &&
          frames >= (unsigned)player_shot_at)
      {
         player_shot_n--;
         player_screenshot();
      }
      if (gedump_at && frames >= (unsigned)gedump_at && g_drew)
      {
         gedump_at = 0;
         dump_ge_bmp();   /* drawbuffer is drawn + synced, not yet swapped */
      }
#ifdef GPSP_PERF_RIG
      /* Harness `gedump_loops = N`: the displayed screen every N loop
       * iterations, 64 at most -- counted in LOOPS, because a link session's
       * connecting/transfer screen runs no emulated frames (gedump_at can
       * never reach it). */
      if (g_gedump_loops && g_drew && g_gedump_left &&
          g_loop_iter % g_gedump_loops == 0)
      {
         char path[176];
         g_gedump_left--;
         snprintf(path, sizeof(path), "%s/gel_%06u.bmp", LOG_DIR, g_loop_iter);
         if (vid_dump_ge(path) == 0)
            fe_evt("ge_dump file=%s loop=%u frame=%u gblink_screen=%d", path,
                   g_loop_iter, frames, g_gblink_screen);
      }
#endif
      if (gedump_every && g_drew && (frames % (unsigned)gedump_every) == 0)
         dump_ge_bmp();
#ifdef REWIND
      {
         /* harness `rewind_gedump = N`: the screen every N loops while the
          * ring is up (the frame count stands still during a rewind, so
          * gedump_every cannot see one). */
         static int every = -1, n;
         if (every < 0)
            every = (int)fe_ini_get_int(HARNESS_INI, "rewind_gedump", 0);
         if (every > 0 && g_drew && g_rw_on && g_rwh.ui > 0.0f &&
             (n++ % every) == 0)
            dump_ge_bmp();
      }
#endif
      /* (ADR-0066/0069: the once-a-second screenshot marker-file stat, a
       * 12.4 ms blocking sceIoGetstat, and the deliberate yield that could
       * stand in for it, left the tree in code-health C2-4'.  L+R+SELECT is
       * the screenshot; the §16 A/B found the stall's removal not implicated
       * in the 29.97 gate.) */
#if defined(FX_SWEEP) && defined(GPSP_PERF_RIG)
      fx_sweep_step(frames);
#endif
      blit_prof_evt(frames);
      rig_romload_evt(frames);
      rig_watch_evt(frames);

      if (have_script)
      {
         int st = fe_autopilot_status();
         if (st == 1 && !g_gblink_role && !g_link_menu &&
             !g_gblink_after_left)
            g_running = 0;              /* script finished: clean exit
                                         * (a GB link run exits when its
                                         * session ends: gblink_service;
                                         * a menu session's, link_exit_after
                                         * frames after it ends) */
         else if (st == -1)
         {
            rig_fail_probe();
            exit_code = 3;
            exit_reason = "ap_fail";
            g_running = 0;
         }
      }
      if (autoexit && frames >= (unsigned)autoexit)
         g_running = 0;
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
      /* GBA-LINK-MIRROR.md 22: a bench pair that stopped on an error ends
       * the run, with the reason where a starved log cannot lose it. */
      if (!g_gba_cable_ok && fe_gbadual_fault() && g_running)
      {
         fe_evt("gbadual_failed err=%d frame=%u", fe_gbadual_fault(), frames);
         if (g_hb_s > 0)
            heartbeat_write(0);
         exit_code = 7;
         exit_reason = "gbadual_failed";
         g_running = 0;
      }
#endif
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
      /* loop iterations, not frames: a loaded state holds the game for
       * the link (fe_host_gblink_hold), so no frame runs before a session */
      if (g_sessmem_cycles && g_loop_iter >= g_sessmem_at)
      {
         int src = sessmem_cycle_test();
         g_sessmem_cycles = 0;
         exit_code = src ? 5 : 0;
         exit_reason = "sessmem_done";
         g_running = 0;
      }
#endif

      FE_LP_SET(FELP_L_ACCT);
      LOOP_PHASE(4);

      if (frame_t0)
      {
         /* One clock read, two consumers: ADR-0021's profile and ADR-0027's
          * capability estimate both want this frame's WORK time (every
          * vblank wait below is deliberately excluded). */
         uint32_t work_us = (uint32_t)(net_now_us() - frame_t0);
         rig_work_us = work_us;
         fe_np_prof_frame(work_us);
         frame_hist_note(work_us);        /* ADR-0063: no extra clock read */
         /* A link session's audio policy owns the step (GB-LINK.md 14). */
         if (g_pcfg.console == FE_CONSOLE_GBA && !g_gbl_audio_on)
            pace_frame(work_us);
      }
      else if (g_pcfg.console == FE_CONSOLE_GBA && !g_gbl_audio_on)
         pace_frame(0);   /* ADR-0033: the glide OUT of the clamp runs after
                           * teardown, when frame_t0 is no longer taken.  The
                           * work time is unused off-session (pace_window is
                           * only called while g_net_up), so 0 is honest. */

      if (!g_ff_uncapped)
      {
         /* Pacing: vblank-locked (59.94 Hz) with an audio-ring high-water
          * nudge toward the GBA's 59.7275 Hz (plan §4.4), plus ADR-0027's
          * peer match — extra vblanks that pull this console down to the
          * slowest peer's capability so the two games' frame-counted link
          * timeouts stay mutually consistent. */
         /* ADR-0047: pace against the ABSOLUTE vblank counter, not by counting
          * waits.
          *
          * The old code called sceDisplayWaitVblankStart() once, plus once per
          * `extra`.  That call blocks until the NEXT vblank edge -- so if the
          * frame's work had already crossed an edge, the first wait landed on
          * the edge AFTER the one we meant, and the period silently became
          *
          *     (ceil(work / 16.74ms) + extra)  instead of  (1 + extra)
          *
          * i.e. ANY frame costing more than one vblank bought a whole extra
          * vblank, whatever the clamp was set to.  That is why the join console
          * could sit on 10-15 ms of slack against a 33.4 ms budget and still
          * land on 19.98: the threshold that mattered was never the budget, it
          * was 16.74 ms.  It also explains why lowering the clamp never closed
          * the gap -- the trigger is independent of the target -- and why the
          * host (smaller frames, fewer crossings) always outran the client.
          *
          * Reproduced on the rig before this fix: --slow-join=12000 pushed the
          * join instance's frame to ~18-23 ms and its rate fell 29.96 -> 19.97
          * while the host held 29.97.  The control at --slow-join=0 held 29.96
          * on both.
          *
          * Tracking the counter instead means an overrunning frame CONSUMES
          * its budget rather than adding a fresh one on top. */
         unsigned extra = g_ff_mult || g_gbl_audio_on
                             ? 0u : pace_extra_vblanks();
         unsigned want  = 1u + extra;
         /* Not while a GB link session's rate follow holds the ring: there
          * the surplus is consumed by the resampler, and a whole extra
          * vblank here is a stall for the partner console. */
/* @extract:begin lowlat:want */
         if (!g_ff_mult && ring_w - ring_r > RING_HIGH_WATER &&
             !(g_gbl_audio_on && g_gbl_follow))
         {
            want++;               /* audio-ring nudge (plan §4.4), unchanged */
            g_fh_nudge++;         /* ADR-0063: previously unobservable */
            g_ar_nudge++;
         }
         else if (want == 1u && !g_ff_mult && ll_want_slot(ring_w - ring_r))
         {
            want++;               /* the same nudge, a little early (me_lowlat 2) */
            g_fh_nudge++;
            g_ar_nudge++;
            TEL(pacing_tel.ll_pull++);
         }
         else if (want == 1u && !g_ff_mult && !g_ff_uncapped && !g_gbl_audio_on &&
                  lf_want_slot(ring_w - ring_r))
         {
            want++;               /* CATCH UP SOON: the same nudge, early */
            g_fh_nudge++;
            g_ar_nudge++;
            TEL(pacing_tel.lf_pull++);
         }
         audio_ring_note();
/* @extract:end lowlat:want */
         FE_LP_SET(FELP_L_OSD);
         if (g_drew && !g_dfr)
            osd_draw();

         g_vc_target += want;
         /* If we have fallen further behind than we could ever repay, forgive
          * the debt rather than sprinting.  Without this a console that is
          * genuinely too slow would bank arrears and then run flat out the
          * moment it caught a break -- visible as a speed-up after a stall,
          * which is worse than being late. */
         /* ADR-0063 folds its two pacing counters into the vcount read that
          * was already here: `vc` replaces the first of the three calls this
          * block used to make, so the instrumented build makes FEWER syscalls
          * per frame than the uninstrumented one, not more. */
         {
            unsigned vc = sceDisplayGetVcount();
            if ((int)(vc - g_vc_target) > PACE_VC_RESYNC_VB)
            {
               g_vc_target = vc;
               g_fh_forgive++;
            }
            if ((int)(vc - g_vc_target) >= 0)
               g_fh_late++;       /* budget already spent: we will not wait */
            gblink_win_work(rig_work_us, (int)(vc - g_vc_target) >= 0);
         }
         /* PUMP THE TRANSPORT WHILE WE WAIT.
          *
          * netdrv is a single-threaded pump model (netdrv.h:16): the RX
          * thread only feeds the transport ring, and everything -- delivery
          * to the core AND the outbound ACK -- happens on the main thread
          * inside fe_np_pump.  That pump ran exactly once per frame, so an
          * arriving packet waited 0-16.7 ms (mean ~8) to be looked at, and
          * the ACK waited the same again on the far side.  Two quantisations
          * of ~8 ms is ~16 ms, which is most of the measured srtt of 17-23 ms
          * -- and why srtt sits suspiciously near one frame time while
          * rttvar stays tiny (quantisation is regular; radios are not).
          *
          * This wait is pure idle: the loop is parked on vblank with the
          * frame's work already done.  Pumping here costs no emulation time,
          * stays on the main thread so the threading contract holds, and
          * roughly halves the quantisation on both sides.
          *
          * Falsifier: if srtt stays ~20 ms while pumpgap drops, the latency
          * is genuinely in the radio and this whole line of reasoning is
          * wrong. */
         FE_LP_SET(FELP_VWAIT);
         /* FF FLIP PACING bit 0, 3x only: the last flip has not latched yet
          * (a loop that overran into the interval it is pending in): no early
          * flip in this wait -- it would replace that one.  The swap after the
          * wait flips this frame, as 4.1.0 always did.  Not at 1x: there a
          * post-vblank flip would keep every later frame post-vblank (one
          * frame of latency for good); low latency's recovery stands. */
         ffh_fe = g_flip_early;
         if ((g_ff_flip_pace & 1) && g_ff_mult && g_flip_early && ffh_too_soon(1))
         {
            g_flip_early = 0;
            TEL(pacing_tel.ffh_waits++);
         }
/* @extract:begin lowlat:wait */
         lat_wait_begin(want);
         dfr_present();           /* LATE PRESENT: before any retire/flip */
         lf_hold_begin();         /* LATE FLIP HOLD: never replace an unlatched flip */
         while ((int)(sceDisplayGetVcount() - g_vc_target) < 0)
         {
            if (g_net_up)
               fe_np_pump();
            /* SAME-FRAME, RETIRE ONLY.  Adopting the ME's output the moment
             * it finishes is the whole feature -- this frame's render shown
             * in this frame.  But it must NOT draw here: the single present
             * happens below, before the swap, so that a frame which never
             * retires still puts something in the back buffer. */
            me_rend_retire_if_idle();
            /* First moment the ME reports done: that IS the render duration
             * (measured from the post), and this wait is idle anyway. */
            if (g_me_rend && !g_mer_rend_seen && g_mer_post_us &&
                me_host_idle())
            {
               unsigned d = (unsigned)sceKernelGetSystemTimeLow() -
                            g_mer_post_us;
               g_mer_rend_seen = 1;
               g_mer_rend_n++;
               g_mer_rend_us += d;
#if defined(FX_SWEEP) && defined(GPSP_PERF_RIG)
               g_fxs_rn++;  g_fxs_ru += d;
#endif
               if (d > g_mer_rend_max)
                  g_mer_rend_max = d;
            }
            /* LOW LATENCY: flip as soon as the frame is complete (tier 0a)
             * or present this iteration's own render (tier 1).  While one
             * of those waits on the GE or the ME, poll in short sleeps --
             * still a wait, other threads still run -- instead of
             * sleeping through the moment the work became possible. */
            lf_hold_vblank(ffh_fe);   /* LATE FLIP HOLD: latched -> flip early again */
            if (lat_wait_step())
               sceKernelDelayThread(LAT_POLL_US);
            else
            {
               sceDisplayWaitVblankStart();
               g_vb_us = (unsigned)sceKernelGetSystemTimeLow();
#ifdef GPSP_KEEP_TELEMETRY
               pacing_tel.vb_vc = sceDisplayGetVcount();
               pacing_tel.lat_vbhc_n++;
               pacing_tel.lat_vbhc_sum += (unsigned)sceDisplayGetCurrentHcount();
#endif
            }
         }
         lat_wait_end();
         g_flip_early = ffh_fe;
         FE_LP_SET(FELP_L_RETIRE);

         /* ONE LAST LOOK BEFORE GIVING UP.
          *
          * The loop above checks the ME and then BLOCKS in
          * sceDisplayWaitVblankStart().  The ME routinely finishes DURING
          * that block, and the loop then exits on its vcount condition
          * without ever re-checking -- so a frame that was rendered and
          * ready would be thrown away.  (Measured on the old same-frame
          * design, deleted in code-health C2-4': of 533 and 600 "misses" in
          * two windows, 529 and 502 had the ME already idle.)  A render
          * still pending here lands next frame -- the shipping behaviour. */
         me_rend_retire_if_idle();

         FE_LP_SET(FELP_L_SWAP);
         if (!g_lw_swapped)
            lat_swap(0);
         g_lw_swapped = 0;
/* @extract:end lowlat:wait */
         g_drew = 0;
      }
      else
      {
         if (g_ffh_3x)
            ffh_cap();            /* FF FLIP PACING bit 2: the multiplier ceiling */
         if (g_drew)
         {
         /* UNCAPPED FF: swap only when a frame was actually blitted. */
         if (ffh_hold())
            g_ffh_held = 1;       /* FF FLIP PACING: a later loop flips it */
         else
         {
            osd_draw();
            lat_swap(0);
            g_drew = 0;
            g_ffh_held = 0;
         }
         }
      }
      FE_LP_SET(FELP_L_RIGN); /* @lab */ LAB_HOOK(if (lab_frame(fe_host_frame_count(), (unsigned)(net_now_us() - g_frame_start_us), rig_work_us, g_loop_iter, g_loop_phase) && g_running) { exit_code = 6; exit_reason = "lab_wall_limit"; g_running = 0; } lab_power_poll((unsigned)g_pwr_suspend, ui_active()); { int lab_sl = lab_state_due(); if (lab_sl == 1 && state_path[0]) lab_state_done(1, state_save_slot(state_path, 1)); else if (lab_sl == 2 && state_path[0]) lab_state_done(2, psp_state_load(state_path, "lab_timer")); })
      lat_census();
      if (g_perf_rig)
         rig_note(fe_host_frame_count(), (unsigned)(net_now_us()-g_frame_start_us), rig_work_us);
      ff_win_note(); /* @lab */ LAB_FFP(lab_ffpace_loop(g_ff_user, g_ff_gate, g_ff_mult, g_ff_uncapped, g_me_rend);)
      FE_LP_SET(FELP_LOOP);
   }
   L->exit_code = exit_code;
   L->exit_reason = exit_reason;
   L->ff_engaged = ff_engaged;
   L->ff_toggled = ff_toggled;
   L->ff_last_mode = ff_last_mode;
   L->ff_last_me = ff_last_me;
   L->prev_pad = prev_pad;
   L->chord_frames = chord_frames;
   L->inproc_menu_armed = inproc_menu_armed;
   L->dump_at = dump_at;
   L->gedump_at = gedump_at;
   L->player_shot_n = player_shot_n;
   L->ui_ctl_demo = ui_ctl_demo;
   L->ui_demo = ui_demo;
   L->wake_shot = wake_shot;
}

/* loop_reset: what every game starts from, set again before the in-process
 * return goes back to the game list (docs/INPROC-RETURN.md) -- the statements
 * main() ran there, in their order (Epoch C C3-1's session_reset, which took
 * main()'s locals by address; tier C C-1 commit 5: the loop's state and the
 * boot's values).  Every loop_t field is set here or tagged `sticky` in loop_t
 * (tools/tests/test_session_reset.py).  always_inline: main()'s one call.  The
 * tools read the call as this body (exit_matrix.EPOCHC_HELPERS;
 * tests/extract.epochc_expand). */
static inline __attribute__((always_inline)) void loop_reset(loop_t *L, const boot_ctx *bc,
   int *autoload_state_slot)
{
   g_running = !g_exit_req;
   g_relaunch = 0;
   L->exit_code = 0;
   L->exit_reason = NULL;
   L->ff_engaged = 0;
   L->ff_toggled = 0;
   L->ff_last_mode = -1;
   L->ff_last_me = -1;
   L->prev_pad = 0;
   g_pad_swallow = 0;          /* nothing of the last game is held */
   L->chord_frames = 0;
   g_dump_chord_pending = 0;   /* no screenshot of the next game */
   ctl_turbo_stop(&g_turbo);   /* the game was left: nothing auto-fires */
   ui_set_turbo_locked(0);     /* ...and no session outlives it */
   L->inproc_menu_armed = 0;
   g_ff_uncapped = g_ff_mult = g_ff_user = g_ff_gate = 0;
   g_blit_suppress = 0;
   g_drew = 0;
   memset(&g_ff_gate_st, 0, sizeof(g_ff_gate_st));
   g_wake_have = 0;
   g_wake_menu = 0;
   g_last_pix = NULL;
   g_me_rend_cfg = bc->me_rend_cfg_boot;
   g_me_video_cfg = 0;
   g_me_video = 0;
   g_pcfg.sram_thread = bc->sram_thread_boot;
   g_autoload_state = bc->autoload_boot;
   *autoload_state_slot = 1;
   g_autoload_from_shelf = 0;
}
