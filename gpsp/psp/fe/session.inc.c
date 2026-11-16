/* ---- THE SESSION (Epoch C C3, builds/epochc-out/PLAN-EPOCHC-beta5.md) -----------
 * The phases of one game before and after the frame loop.  What every game
 * starts from (Epoch C C3-1's session_reset) is loop_reset since tier C C-1
 * commit 5, beside the loop's state it sets (loop_t, fe/loop.inc.c). */

/* SESSION SELECT: which game, and where its save and states live -- the variant's
 * baked ROM, the harness's, the XMB launcher's, or the game list's (which parks);
 * the boot's power hold from here (r7-L5); the sidecars, the display profile,
 * the loading screen.  Its exits are B1-B10 (exit_early_end). */
static __attribute__((noinline, section(".text.fe_cold.session_select"))) int session_select(
   char *rom_path, char *save_path, char *state_path, int *autoload_state_slot, int inproc_n,
   char (*inproc_roms)[64], int inproc_rom_n, int have_variant, int have_harness, int folders)
{
   int launch_now = 0;   /* this session is an XMB launch (g_launch_rom) */
   /* ROM selection: variant builds auto-boot their baked ROM with a
    * separate save namespace (<base>/saves/, docs/VARIANTS.md); the
    * harness ini boots the first ROM in roms/ directly (the
    * historical Phase-1 behavior every e2e driver relies on); a human
    * without either gets the browser (plan §8, Gate 2). */
#ifndef GPSP_NO_TELEMETRY
   {
      extern void (*gpsp_heap_census)(const char *where);
      gpsp_heap_census = heap_census;
      heap_census("pre_select");
   }
#endif
   /* XMB LAUNCH (at relaunch_eboot): the first session boots the ROM the
    * launcher named.  A ROM that is not there falls through to the list,
    * where the player can see what is. */
   launch_now = 0;
   if (g_launch_rom[0] && !inproc_n && !have_variant)
   {
      int bad = strchr(g_launch_rom, ':')
         ? snprintf(rom_path, PSP_FILE_PATH_CAP, "%s", g_launch_rom) >=
              (int)PSP_FILE_PATH_CAP
         : psp_rom_path_join(rom_path, PSP_FILE_PATH_CAP, ROM_DIR,
                             g_launch_rom) != 0;
      if (!bad && file_exists(rom_path))
      {
         launch_now = 1;
         fe_evt("launch rom=%s", rom_path);
      }
      else
         fe_evt("launch_missing rom=%s", g_launch_rom);
      g_launch_rom[0] = '\0';
   }
   /* THE BOOT'S POWER HOLD (pwr_hold, see power_cb) starts with the game's
    * selection: right after the game list returns, or at the top of the
    * launches that have no list (PLAN r7-L5).  Between the two there is no
    * park point -- the list's ended with it, the loop's is far off -- while
    * the sidecars are settled (a save can be mid-move), the ROM's header
    * and the link mode are read and config.ini is read again.  The switch
    * now waits for the loop, as it did from here on already; every exit
    * below releases it through FE_HANDOFF / FE_EXIT_GAME / relaunch_eboot,
    * and a boot that stops on a notice lets it go while the notice waits
    * (boot_stop_notice). */
   if (launch_now)
      pwr_hold("boot");   /* rom_path is set; the sidecars below are the game list's */
   else if (have_variant)
   {
      char vrom[PSP_ROM_REL_PATH_CAP] = "", savedir[160];
      char stem[PSP_ROM_REL_PATH_CAP];
      const char *slash;
      pwr_hold("boot");
      fe_ini_get(VARIANT_INI, "rom", vrom, sizeof(vrom));
      if (!vrom[0])
      {
         fe_evt("exit code=2 reason=bad_variant");
         exit_early_end(XE_EVT_FIRST, 2, "bad_variant", 0);   /* ADR-0053 */
         return 0;
      }
      if (psp_rom_path_join(rom_path, PSP_FILE_PATH_CAP, g_dir_base, vrom) != 0)
      {
         fe_evt("exit code=2 reason=rom_path_too_long rom=%s", vrom);
         exit_early_end(XE_EVT_FIRST, 2, "rom_path_too_long", 0);
         return 0;
      }
      snprintf(savedir, sizeof(savedir), "%s/saves", g_dir_base);
      sceIoMkdir(savedir, 0777);
      slash = strrchr(vrom, '/');
      snprintf(stem, sizeof(stem), "%s", slash ? slash + 1 : vrom);
      snprintf(save_path, PSP_FILE_PATH_CAP, "%s/saves/%s", g_dir_base, stem);
      if (make_suffixed_path(save_path, ".sav", save_path,
                             PSP_FILE_PATH_CAP) != 0)
      {
         fe_evt("exit code=2 reason=sidecar_path_too_long rom=%s", vrom);
         exit_early_end(XE_EVT_FIRST, 2, "sidecar_path_too_long", 0);
         return 0;
      }
      snprintf(state_path, PSP_FILE_PATH_CAP, "%s/saves/%s", g_dir_base,
               stem);
      if (make_suffixed_path(state_path, ".st0", state_path,
                             PSP_FILE_PATH_CAP) != 0)
      {
         fe_evt("exit code=2 reason=sidecar_path_too_long rom=%s", vrom);
         exit_early_end(XE_EVT_FIRST, 2, "sidecar_path_too_long", 0);
         return 0;
      }
      fe_evt("variant rom=%s silent=%d group=%s", vrom, g_silent_mode,
             g_silent_group);
   }
   else if (have_harness && !HARNESS_INT("browser", 0))
   {
      /* `browser = 1` overrides the ADR-0067 suppression: the gallery runs
       * even with the harness channel present (asset capture / emulator
       * shoots — combine with ui_demo=1 for the unattended gallery dump). */
      /* `rom = <name.gba>` names the cart explicitly, relative to roms/.
       * Without it find_first_rom() takes whatever the FILESYSTEM returns
       * first, which is insertion order, not alphabetical -- a card holding
       * emerald.gba + firered.gba + leafgreen.gba silently picks by luck.
       * The key lets an arm switch games by staging the ini alone, which
       * matters because ROMs are far too big to stage. */
      char kr[PSP_ROM_REL_PATH_CAP] = "";
      pwr_hold("boot");
      HARNESS_STR("rom", kr, sizeof(kr));
      if (inproc_rom_n)
         snprintf(kr, sizeof(kr), "%s", inproc_roms[inproc_n % inproc_rom_n]);
      if (kr[0])
      {
         if (psp_rom_path_join(rom_path, PSP_FILE_PATH_CAP, ROM_DIR, kr) != 0)
         {
            fe_evt("exit code=2 reason=rom_path_too_long rom=%s", kr);
            exit_early_end(XE_EVT_FIRST, 2, "rom_path_too_long", 0);
            return 0;
         }
         if (!file_exists(rom_path))
         {
            /* Loud, not a fallback to some other game: silently running the
             * wrong cart would invalidate every number the run produces. */
            fe_evt("exit code=2 reason=rom_missing rom=%s", kr);
            exit_early_end(XE_EVT_FIRST, 2, "rom_missing", 0);
            return 0;
         }
         fe_evt("rom selected=%s (harness key)", kr);
      }
      else if (find_first_rom(rom_path, PSP_FILE_PATH_CAP) != 0)
      {
         fe_evt("exit code=2 reason=no_rom");
         exit_early_end(XE_EVT_FIRST, 2, "no_rom", 0);   /* ADR-0053 */
         return 0;
      }
   }
   else
   {
      int browser_state_slot = 0;
      fe_console_t selected_console = (fe_console_t)g_pcfg.console;
      int br;
      if (!inproc_n)
      {
         boot_mark("browser");
/* @extract:begin sb:list_call */
         boot_timeline_emit();
      }
      ui_set_browser_park(browser_park);   /* the list parks too */
      br = ui_browser(ROM_DIR, rom_path, PSP_FILE_PATH_CAP,
                          &browser_state_slot, &selected_console);
      ui_set_browser_park(NULL);
      pwr_hold("boot");   /* the list's park is over: see above (r7-L5) */
      if (br > 0)
/* @extract:end sb:list_call */
      {
         /* Settings changed Media Engine mode, which was latched above.
          * Nothing is emulating yet -- no SRAM to flush, no core thread to
          * stop.  Close the log and boot again. */
         fe_evt("exit code=0 reason=ui_relaunch");
         exit_early_end(XE_EVT_FIRST | XE_RELAUNCH, 0, NULL, 0);
         return 0;
      }
      if (br != 0)
      {
         /* HOME in the list is the player leaving, not a missing ROM (F12):
          * the Lab read it as `no_rom` code 2.  A ROM path too long (-1 with
          * the app still running) stays no_rom, code 2. */
         int home = !g_running;
         if (home)
            fe_evt("exit code=0 reason=home");
         else
            fe_evt("exit code=2 reason=no_rom");
         exit_early_end(XE_EVT_FIRST, home ? 0 : 2, home ? "home" : "no_rom", 0);   /* ADR-0053 */
         return 0;
      }
      g_pcfg.console = (int)selected_console;
      /* A slot picked on the browser's state shelf (any console). */
      if (browser_state_slot > 0)
      {
         *autoload_state_slot = browser_state_slot;
         g_autoload_state = 1;
         g_autoload_from_shelf = 1;
      }
   }
   /* Baked variant and harness launches have no interactive filter, so the
    * file type selects the matching GB hardware model. Browser launches use
    * the model explicitly chosen by the player. */
   if (launch_now || have_variant || (have_harness &&
       !HARNESS_INT("browser", 0)))
      g_pcfg.console = (int)console_from_rom_path(rom_path);
   /* PER-CONSOLE DISPLAY PROFILE (docs/DISPLAY-FEATURES.md): the running
    * ROM's console decides scale, filter, ambient bars and DMG palette.
    * Before ui_loading_begin (which bakes the ambient texture) and before
    * cfg.gb_palette is read below. */
   pcfg_display_select(g_pcfg.console);
   psp_display_apply("rom");
   vid_ambient_mode(g_pcfg.ambient);
   fe_evt("display_profile console=%s scale=%s filter=%s ambient=%d "
          "gb_palette=%d", pcfg_console_tag(g_pcfg.console),
          vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter),
          g_pcfg.ambient, g_pcfg.gb_palette);
   if (!have_variant)
   {
      const char *save_suffix =
         g_pcfg.console == FE_CONSOLE_GBA ? ".sav" :
         g_pcfg.console == FE_CONSOLE_GBC ? ".gbc.sav" : ".gb.sav";
      if (make_suffixed_path(rom_path, save_suffix, save_path,
                             PSP_FILE_PATH_CAP) != 0 ||
          make_suffixed_path(rom_path,
                             psp_state_slot1_suffix(
                                (fe_console_t)g_pcfg.console),
                             state_path, PSP_FILE_PATH_CAP) != 0)
      {
         fe_evt("exit code=2 reason=sidecar_path_too_long rom=%s", rom_path);
         exit_early_end(XE_EVT_FIRST, 2, "sidecar_path_too_long", 0);
         return 0;
      }
   }
   {
      /* FOLDERS: the legacy paths are in place; a Lab run's work folder
       * replaces them (lab_sidecars), otherwise the folders may. */
      char before_lab[PSP_FILE_PATH_CAP];
      snprintf(before_lab, sizeof(before_lab), "%s", save_path);
      /* @lab */ LAB_HOOK(if (!have_variant) lab_sidecars(save_path, PSP_FILE_PATH_CAP, state_path, PSP_FILE_PATH_CAP);)
      if (!have_variant)
         folders_settle(folders, rom_path,
                        g_pcfg.console == FE_CONSOLE_GBA ? ".sav" :
                        g_pcfg.console == FE_CONSOLE_GBC ? ".gbc.sav"
                                                         : ".gb.sav",
                        (fe_console_t)g_pcfg.console, save_path,
                        PSP_FILE_PATH_CAP, state_path, PSP_FILE_PATH_CAP,
                        before_lab);
   }
   ui_set_state_base(state_path);
   g_save_path_for_backup = save_path;
   ui_loading_begin(rom_path); /* also covers harness/variant auto-boot */
   /* Four bytes at 0xAC name the game for Mystery Gift: which title a card
    * claims to be for, and what to call it on screen.  Read here, while the
    * path is in scope, rather than exporting a core global. */
   if (g_pcfg.console == FE_CONSOLE_GBA)
      mgift_read_game_code(rom_path);
   link_read_identity(rom_path);      /* docs/GBA-LINK-UI.md */

   /* ME rendering, its capture format and FF synchronization consume GBA
    * 240x160 frames. GB-family video uses the normal dimension-aware GE path. */
   if (g_pcfg.console != FE_CONSOLE_GBA)
   {
      g_me_rend_cfg = 0;
      g_me_video_cfg = 0;
   }
   fe_log("rom_path=%s save_path=%s", rom_path, save_path);
   return -1;
}

/* SESSION CONFIGURE: the core's configuration for this game (fe_host_config: the
 * paths, the clocks, the GB link, the harness's machine pairs, the player's link
 * cable) and the switches the core reads before it loads (the link speed paths,
 * the blit mode, the console's clock for the RTC).  No exits. */
static __attribute__((noinline, section(".text.fe_cold.session_configure"))) void session_configure(
   fe_host_config *cfg, const char *rom_path, const char *save_path, int have_harness,
   int net_host, int net_join)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->console      = (fe_console_t)g_pcfg.console;
   cfg->rom_path      = rom_path;
   cfg->system_dir    = g_dir_base;
   cfg->save_path     = save_path;
   cfg->video_frame   = plat_video_frame;
   cfg->audio_frames  = plat_audio_frames;
   cfg->input_bitmask = plat_input_bitmask;
   cfg->time_us       = net_now_us;   /* heartbeat t_us (hw perf baseline) */
   cfg->yield_thread  = gb_serial_yield;
   /* GB/GBC cartridge RTCs (MBC3: Gold/Silver/Crystal).  time() on real PSP
    * firmware is roughly uptime, so without this the clock resets every boot
    * -- the same trap gpsp_wallclock fixes for the GBA RTC below. */
   cfg->wallclock     = psp_local_wallclock;
   /* A GB/GBC cartridge clock counts seconds; the only date in it is the
    * stamp in the save, which the format defines as UTC.  So that clock runs
    * on UTC, and the saves 3.5.0 and earlier stamped with local time are
    * recognised by when this install first ran a release that does
    * (fe_host.c gb_rtc_from_legacy).  The GBA keeps local time: its RTC is
    * a calendar. */
   if (g_pcfg.console != FE_CONSOLE_GBA)
   {
      cfg->wallclock = psp_utc_wallclock;
      cfg->wallclock_legacy = psp_local_wallclock;
      cfg->gb_rtc_legacy_before =
         (long long)pcfg_gb_rtc_utc_since((long)psp_local_wallclock());
      fe_evt("gb_rtc utc=%lld local=%lld legacy_before=%lld",
             (long long)psp_utc_wallclock(), (long long)psp_local_wallclock(),
             cfg->gb_rtc_legacy_before);
   }
   /* A link-session harness run with gblink_rtc_seed: the cartridge clock
    * is that seed, frozen, on BOTH consoles, from boot -- not the PSP's own
    * clock.  The session takes each console's save from its running game,
    * so the save's clock is whatever the console's clock made it: in
    * hardware run hw1 the PSP-1000 (clock 2011) loaded the 2026-stamped
    * Crystal save as "clock not real" and reset it, the session then
    * advanced that 2011 stamp to the 2026 seed (15.6 years), the day
    * counter overflowed, and Crystal stopped at "The clock's time may be
    * wrong." -- the join's script failed at frame 2178 in both Gen 2 runs.
    * A frozen seed gives the same save image on any console and in PPSSPP. */
   if (g_gblink_seed)
   {
      g_gblink_clock = (time_t)g_gblink_seed;
      cfg->wallclock = gblink_seed_wallclock;
      cfg->wallclock_legacy = NULL;   /* one frozen clock: nothing to move */
      fe_evt("gblink_wallclock frozen=%lld (harness gblink_rtc_seed)",
             g_gblink_seed);
   }
   cfg->gb_palette    = g_pcfg.gb_palette;   /* GB/GBC DMG palette */
   cfg->gbc_raw_colors = !g_pcfg.gbc_color_correction;
   cfg->gb_find_rom   = psp_gb_find_rom;     /* GB link: partner cartridge */
   cfg->gblink_batch  = g_gblink_batch;
   cfg->gblink_batch_armed = g_gblink_batch_armed;
   cfg->gblink_live   = g_gblink_live;
   cfg->gblink_resume_boot = g_gblink_resume_boot;
   cfg->heartbeat_frames = g_telemetry_period;
   cfg->gblink_rtc_keep_session_stamp = g_gblink_keep_stamp;
   cfg->gblink_wait   = plat_gblink_wait;    /* stall absorption */
   cfg->gblink_nack   = g_gbl_nack;          /* resend requests */
   cfg->gblink_delay_auto = g_gblink_delay_auto; /* 0 = off */
   cfg->gblink_hash_every  = g_gbl_hash_every;   /* harness A/B */
   if (g_gblink_bulk_bytes)
   {
      cfg->gb_bulk_enable = psp_gb_bulk_enable;
      cfg->gb_bulk_send   = psp_gb_bulk_send;
      cfg->gb_bulk_recv   = psp_gb_bulk_recv;
      cfg->gb_bulk_max    = g_gblink_bulk_bytes;
      cfg->gb_bulk_rate   = g_gblink_bulk_rate;
   }
   /* host=1 / join=1 on a GB/GBC cartridge is a link-session run. */
   if (g_pcfg.console != FE_CONSOLE_GBA && !g_gblink_role && (net_host || net_join))
      g_gblink_role = net_host ? 1 : 2;
   /* harness link_script_waits = 1: a menu-started session's rig holds the
    * autopilot until the link runs, as gblink_role does (docs/
    * GBA-LINK-UI.md); a release build reads no harness key. */
   cfg->gblink_script_waits = g_gblink_role != 0 ||
      HARNESS_INT("link_script_waits", 0) != 0;
   /* LINK SPEED defaults (PLAY_* above), every build: config.ini is the
    * kill switch; the harness block below overrides for an A/B arm.  The
    * two core switches are weak: a build without SCHED_FAST / IRQ_IO_FAST
    * has no such variable.  irq_io_fast is read when the emitter builds its
    * stubs, so it is set here, before any game loads. */
   {
#pragma weak sched_fast
#pragma weak irq_io_fast
      if (&sched_fast)
         sched_fast = fe_ini_get_int(p_config, "sched_fast",
                                     PLAY_SCHED_FAST) ? 1u : 0u;
      if (&irq_io_fast)
         irq_io_fast = fe_ini_get_int(p_config, "irq_io_fast",
                                      PLAY_IRQ_IO_FAST) ? 1u : 0u;
      {
#pragma weak msr_fast
         if (&msr_fast)
            /* default: on unless the core was built MSR_FAST_DEFAULT=0 */
            msr_fast = fe_ini_get_int(p_config, "msr_fast",
                                      PLAY_MSR_FAST && msr_fast) ? 1u : 0u;
      }
#ifdef GBA_LINK
      {
         long v = fe_ini_get_int(p_config, "gba_link_headless_audio",
                                 PLAY_GBA_LINK_HEADLESS_AUDIO);
         cfg->gba_dual_partner_audio = v == 2 ? -1 : (v ? 0 : 1);
         v = fe_ini_get_int(p_config, "gba_link_fast", PLAY_GBA_LINK_FAST);
         cfg->gba_dual_cable_fast = (v >= 0 && v <= 3) ? (int)v : 0;
      }
#endif
   }
#ifdef GPSP_PERF_RIG
   /* Harness: the GB link session's machine pair without the network --
    * gb_dual_rom = <full path> runs that cart headless beside the player's
    * (fe_host.h), gb_dual_mirror = 1 hands it the player's buttons, and
    * gb_dual_state = <full path of a .stN> starts it from a save state.  For
    * measuring what the second machine costs. */
   {
      /* A value without a device (no ':') names a file in this app's roms
       * folder, so one ini works on ms0: and on the Go's ef0:. */
      static char dual_rom[352], dual_state[352], dual_inputs[352],
                  dual_sav[352];
      char v[200];
      /* gb_headless = 1: the single machine without picture or synthesis
       * (what a link session's partner machine costs on its own). */
      cfg->gb_bench_headless = (int)HARNESS_INT("gb_headless", 0);
      if (HARNESS_STR("gb_dual_rom", v, sizeof(v)) && v[0])
      {
         snprintf(dual_rom, sizeof(dual_rom), "%s%s%s",
                  strchr(v, ':') ? "" : p_rom_dir, strchr(v, ':') ? "" : "/",
                  v);
         cfg->gb_dual_rom = dual_rom;
         cfg->gb_dual_mirror =
            (int)HARNESS_INT("gb_dual_mirror", 0);
         cfg->gb_dual_batch =
            (unsigned)HARNESS_INT("gb_dual_batch", 1);
         /* The one-console trade (docs/GB-CORE-PERF.md): gb_dual_inputs =
          * linkplay's input track for both machines, gb_dual_sav = slot 1's
          * battery image, gb_dual_rtc = the session clock seed. */
         cfg->gb_dual_armed =
            HARNESS_INT("gb_dual_armed", 0) != 0;
         cfg->gb_dual_quiet =
            (unsigned)HARNESS_INT("gb_dual_quiet", 0);
         if (HARNESS_STR("gb_dual_rtc", v, sizeof(v)) && v[0])
            cfg->gb_dual_rtc = strtoll(v, NULL, 10);
         if (HARNESS_STR("gb_dual_inputs", v, sizeof(v)) && v[0])
         {
            snprintf(dual_inputs, sizeof(dual_inputs), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg->gb_dual_inputs = dual_inputs;
         }
         if (HARNESS_STR("gb_dual_sav", v, sizeof(v)) && v[0])
         {
            snprintf(dual_sav, sizeof(dual_sav), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg->gb_dual_sav = dual_sav;
         }
         if (HARNESS_STR("gb_dual_state", v, sizeof(v)) && v[0])
         {
            snprintf(dual_state, sizeof(dual_state), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg->gb_dual_state = dual_state;
         }
      }
   }
#ifdef GBA_LINK
   /* Harness: the GBA link mirror's machine pair without the network
    * (docs/GBA-LINK-MIRROR.md).  gba_dual = 1 runs a second GBA core on this
    * cartridge, cabled to the player's and started from every state it
    * loads; its buttons are the player's (gba_dual_mirror = 1) or the
    * frame-stamped gba_dual_script; gba_dual_lookahead in cycles;
    * gba_dual_hash = 1 writes both machines' per-frame hashes to
    * log/gbadual_hash.txt. */
   {
      static char gd_script[200], gd_hash[200], gd_ap[200];
      cfg->gba_dual = (int)HARNESS_INT("gba_dual", 0);
      cfg->gba_dual_mirror =
         (int)HARNESS_INT("gba_dual_mirror", 0);
      cfg->gba_dual_lookahead =
         (unsigned)HARNESS_INT("gba_dual_lookahead", 0);
      cfg->gba_dual_own_rom =
         (int)HARNESS_INT("gba_dual_own_rom", 0);
      /* GBA-LINK-MIRROR.md 16: the headless partner mixes no sound
       * (config.ini kill switch; the harness key wins for an A/B arm) */
      {
         long v = fe_ini_get_int(p_config, "gba_link_headless_audio",
                                 PLAY_GBA_LINK_HEADLESS_AUDIO);
         v = HARNESS_INT("gba_link_headless_audio", v);
         /* 2 (section 23): as 1, and the partner's ring is not read out */
         cfg->gba_dual_partner_audio = v == 2 ? -1 : (v ? 0 : 1);
         v = fe_ini_get_int(p_config, "gba_link_sram_crc", 0);
         cfg->gba_dual_sram_crc =
            HARNESS_INT("gba_link_sram_crc", v) != 0;
         /* 17.5: instance B's translation cache offset mod 8 KiB (-1 = the
          * build's JIT_PIN_OFFSET, the same sets as instance A) */
         v = fe_ini_get_int(p_config, "gba_link_jit_pin_b", -1);
         v = HARNESS_INT("gba_link_jit_pin_b", v);
         cfg->gba_dual_b_jit_pin_p1 = v >= 0 ? (unsigned)(v & 8191) + 1u : 0u;
         /* 21: the cable fast path in both cores (pure speed, same
          * machines; 0 = the path every build up to 3.5.0 used) */
         v = fe_ini_get_int(p_config, "gba_link_fast", PLAY_GBA_LINK_FAST);
         v = HARNESS_INT("gba_link_fast", v);
         /* 24: scheduler fast paths (SCHED_FAST builds only; default 0).
          * Weak: a build without the switch has no such variable. */
         {
            extern uint32_t sched_fast __attribute__((weak));
            long sf = HARNESS_INT("sched_fast",
                                     &sched_fast ? (long)sched_fast : 0);
            if (&sched_fast)
               sched_fast = sf ? 1u : 0u;
            fe_evt("sched_fast built=%d on=%d", &sched_fast != NULL,
                   &sched_fast ? (int)sched_fast : 0);
         }
         /* 26: TIMER_LAZY (prototype, guest-visible, a 0/1 switch) or HALT_HOP
          * (exact; a bit mask, bit 1 HALT_HOP, bit 2 HOP_DRAWN; the build's
          * default when the key is absent).  Only TIMER_LAZY's bit 0 enters
          * the session's core mask (FE_GBLINK_MASK_TIMER_LAZY, strict: both
          * consoles must agree).  HALT_HOP's bits stay here: it is exact and
          * each machine is hashed at its own boundary, so the consoles need
          * not agree, and the mask's bits 1-2 are the negotiable batching
          * bits (fe_gblink.h). */
         {
            extern uint32_t timer_lazy __attribute__((weak));
            long tl = HARNESS_INT("timer_lazy",
                                     &timer_lazy ? (long)timer_lazy : 0);
            if (&timer_lazy)
               timer_lazy = (uint32_t)tl;
            cfg->gba_core_mask = (&timer_lazy && (timer_lazy & 1u))
                                   ? FE_GBLINK_MASK_TIMER_LAZY : 0u;
            /* 28/29: "Fast link" is NOT set here: it is the player's
             * setting, read when a session starts (gblink_begin). */
            fe_evt("timer_lazy built=%d on=%d core_mask=%08x",
                   &timer_lazy != NULL, &timer_lazy ? (int)timer_lazy : 0,
                   cfg->gba_core_mask);
         }
         /* 25: IE/IF/IME stores inline in the I/O store stub (IRQ_IO_FAST
          * builds only; default 0).  Read by the emitter at game load. */
         {
            extern uint32_t irq_io_fast __attribute__((weak));
            long qf = HARNESS_INT("irq_io_fast",
                                     &irq_io_fast ? (long)irq_io_fast : 0);
            if (&irq_io_fast)
               irq_io_fast = qf ? 1u : 0u;
            fe_evt("irq_io_fast built=%d on=%d", &irq_io_fast != NULL,
                   &irq_io_fast ? (int)irq_io_fast : 0);
         }
         /* MSR_FAST builds: mode switches by MSR / SPSR restore banked in
          * asm (mips_stub.S msr_fast_store); read on every switch. */
         {
            extern uint32_t msr_fast __attribute__((weak));
            long mf = HARNESS_INT("msr_fast",
                                     &msr_fast ? (long)msr_fast : 0);
            if (&msr_fast)
               msr_fast = mf ? 1u : 0u;
            fe_evt("msr_fast built=%d on=%d", &msr_fast != NULL,
                   &msr_fast ? (int)msr_fast : 0);
         }
         /* 22: 2 = the checking mode (the full path decides, the fast
          * path's answers are compared with it and disagreements counted) */
         /* 23: 3 = the fast path plus the lean inline hooks */
         cfg->gba_dual_cable_fast = (v >= 0 && v <= 3) ? (int)v : 0;
         /* 22: the pair's wall-clock bound per step, ms (0 = 15000,
          * negative = none) */
         cfg->gba_dual_step_max_ms =
            (int)HARNESS_INT("gba_link_step_max_ms", 0);
         /* 21: the one-console bench starts its pair at power-on (harness
          * only; without it the pair waits for a state load) */
         cfg->gba_dual_cold =
            HARNESS_INT("gba_dual_cold", 0) != 0;
         /* 18: lend instance B's static block to the ROM cache while no
          * session runs (kill switch gba_link_lend = 0) */
         v = fe_ini_get_int(p_config, "gba_link_lend", 1);
         v = HARNESS_INT("gba_link_lend", v);
         cfg->gba_link_nolend = v ? 0 : 1;
      }
      if (HARNESS_STR("gba_dual_script", gd_script,
                     sizeof(gd_script)) && gd_script[0])
         cfg->gba_dual_script = gd_script;
      /* gba_dual_autopilot = file: the partner's autopilot script (RAM
       * predicates on the partner's machine); wins over gba_dual_script. */
      if (HARNESS_STR("gba_dual_autopilot", gd_ap,
                     sizeof(gd_ap)) && gd_ap[0])
         cfg->gba_dual_autopilot = gd_ap;
      if (HARNESS_INT("gba_dual_hash", 0))
      {
         snprintf(gd_hash, sizeof(gd_hash), "%s/gbadual_hash.txt", LOG_DIR);
         cfg->gba_dual_hash = gd_hash;
      }
      /* gblink_role = host|join with gba_dual = 1 on a GBA game: a
       * two-console link-cable session instead of the one-console bench.
       * gblink_hash_every: slot-0 frames between the session's HASH
       * messages (0 = 240, as a GB session). */
      g_gba_cable = cfg->gba_dual && g_gblink_role &&
                    g_pcfg.console == FE_CONSOLE_GBA;
      cfg->gba_dual_session = g_gba_cable;
      /* harness `link_avail = 1` (with gba_dual_late = 1): what a player's
       * build does at every GBA game load -- a cable room CAN be hosted, no
       * session is running.  For tests of everything that must keep
       * working in that state (the rewind did not: fe_host_rewind_blocked). */
      if (HARNESS_INT("link_avail", 0) && !g_gba_cable &&
          g_pcfg.console == FE_CONSOLE_GBA)
      {
         cfg->gba_dual = 1;
         cfg->gba_dual_session = 1;
      }
      cfg->gblink_hash_every = (unsigned)HARNESS_INT(
                                                       "gblink_hash_every", 0);
      if (g_gba_cable)
         fe_evt("gba_cable role=%s hash_every=%u", g_gblink_role == 1 ?
                "host" : "join", cfg->gblink_hash_every);
      fe_evt("gba_dual_cfg on=%d mirror=%d own_rom=%d lookahead=%u "
             "script=%s hash=%d partner_audio=%d sram_crc=%d jit_pin_b=%d "
             "fast=%d cold=%d",
             cfg->gba_dual,
             cfg->gba_dual_mirror, cfg->gba_dual_own_rom,
             cfg->gba_dual_lookahead,
             cfg->gba_dual_script ? cfg->gba_dual_script : "-",
             cfg->gba_dual_hash != NULL, cfg->gba_dual_partner_audio,
             cfg->gba_dual_sram_crc, (int)cfg->gba_dual_b_jit_pin_p1 - 1,
             cfg->gba_dual_cable_fast, cfg->gba_dual_cold);
   }
#endif
#endif
   /* THE PLAYER'S LINK CABLE (docs/GBA-LINK-UI.md).  The Wireless menu can
    * host or join a Link Cable room on any GBA game, so instance B is loaded
    * for sessions at boot (idle until one starts: fe_gbadual's session mode
    * never runs it alone; its JIT is the static small tier), and the
    * session's memory is reserved below when the game can link by cable at
    * all.  A release build does this for every GBA game; a harness build
    * only with gba_link_menu = 1, so the rigs' single-player and role runs
    * are what they were. */
   ui_linkmode_load(p_rom_dir);
#ifdef GBA_LINK
   {
#ifdef GPSP_PERF_RIG
      int menu = (int)HARNESS_INT("gba_link_menu", 0);
      g_link_exit_after = (unsigned)HARNESS_INT(
                                                   "link_exit_after", 0);
      fe_host_gba_cable_rom_sampled(
         !HARNESS_INT("gba_link_rom_full", 0));
#else
      int menu = 1;
#endif
      if (menu && g_pcfg.console == FE_CONSOLE_GBA && !cfg->gba_dual)
      {
         cfg->gba_dual = 1;
         cfg->gba_dual_session = 1;
      }
      g_gba_cable_ok = cfg->gba_dual && cfg->gba_dual_session;
      /* Section 19: B loads at session start, its memory the session's.  A
       * player's build always; a harness run with gba_dual_late = 1 (the
       * rigs' role sessions keep B loaded at boot unless they ask). */
#ifdef GPSP_PERF_RIG
      g_gba_late = (int)HARNESS_INT("gba_dual_late", 0) != 0;
      cfg->gba_session_reserve_kb = (unsigned)HARNESS_INT(
         "cable_reserve_kb", (long)CABLE_RESERVE_KB);
      cfg->gba_sessmem_fail = (int)HARNESS_INT(
                                                 "sessmem_fail", 0);
      if (cfg->gba_sessmem_fail)
         fe_evt("sessmem_fail mode=%d", cfg->gba_sessmem_fail);
#else
      g_gba_late = 1;
      cfg->gba_session_reserve_kb = CABLE_RESERVE_KB;
#endif
      cfg->gba_dual_late = g_gba_late;
      cfg->gba_reload_note = gba_reload_note;
      /* Section 20: a cable session's translation-cache offsets mod 8 KiB per
       * tier (A = this game's core, B = the partner's; -1 = A keeps the
       * single-player pin, B the build's).  config.ini or harness keys
       * gba_link_pin_{a,b}_{small,large}; the session's start applies them
       * by reloading A, its end puts the single-player pin back. */
      {
         static const char *const kn[4] = {
            "gba_link_pin_a_small", "gba_link_pin_a_large",
            "gba_link_pin_b_small", "gba_link_pin_b_large" };
         static const long def[4] = { GBA_SESS_PIN_A_SMALL, GBA_SESS_PIN_A_LARGE,
                                      GBA_SESS_PIN_B_SMALL, GBA_SESS_PIN_B_LARGE };
         int k;
         for (k = 0; k < 4; k++)
         {
            long v = fe_ini_get_int(p_config, kn[k], def[k]);
            unsigned p1;
#ifdef GPSP_PERF_RIG
            /* spelled out, not kn[k]: rig key audits (link_hw.py) look for
             * the literal read of every harness key they emit */
            switch (k)
            {
            case 0: v = HARNESS_INT("gba_link_pin_a_small", v); break;
            case 1: v = HARNESS_INT("gba_link_pin_a_large", v); break;
            case 2: v = HARNESS_INT("gba_link_pin_b_small", v); break;
            default: v = HARNESS_INT("gba_link_pin_b_large", v); break;
            }
#endif
            p1 = v >= 0 ? (unsigned)(v & 8191) + 1u : 0u;
            if (k < 2)
               cfg->gba_sess_pin_a_p1[k] = p1;
            else
               cfg->gba_sess_pin_b_p1[k - 2] = p1;
         }
         fe_evt("gba_sess_pin a=%d/%d b=%d/%d (small/large, -1 keep)",
                (int)cfg->gba_sess_pin_a_p1[0] - 1,
                (int)cfg->gba_sess_pin_a_p1[1] - 1,
                (int)cfg->gba_sess_pin_b_p1[0] - 1,
                (int)cfg->gba_sess_pin_b_p1[1] - 1);
      }
      fe_evt("gba_link_menu on=%d cable_ok=%d late=%d", menu, g_gba_cable_ok,
             g_gba_late);
   }
#endif
   cfg->core_counters = plat_core_counters;                   /* ADR-0028 */
   cfg->boot_status   = rom_loading_stage;
#ifdef FE_SMC_PROFILE
   cfg->smc_addr      = plat_smc_addr;                        /* ADR-0030 */
   cfg->smc_block     = plat_smc_block;                       /* phase 5g */
#endif
   cfg->core_phase    = plat_core_phase;                      /* phase 5h */
   smc_prof_clock    = plat_smc_clock;                       /* phase 5g */
   /* Order matters: the core clamps the level to 0 without a clock. */
   core_phase_set_level((unsigned)g_pcfg.core_phase);         /* phase 5h */
   /* ADR-0034: staging placement.  Logged with the mode ACTUALLY IN FORCE,
    * not the one requested — VRAM silently falls back if it will not fit,
    * and a field log that reported the request would be a lie. */
   {
      int bm = vid_set_blit_mode(g_pcfg.blit_mode);
      fe_evt("blit_mode req=%d mode=%d name=%s", g_pcfg.blit_mode, bm,
             vid_blit_mode_name(bm));
      g_pcfg.blit_mode = bm;
   }
   vid_set_gu_defer(g_pcfg.gu_defer);
   fe_evt("gu_defer=%d", vid_gu_defer());

   /* THE CONSOLE'S CLOCK FOR THE GBA RTC -- see gpsp_wallclock in
    * gba_memory.c.  Must be set before the cart loads, which is when the
    * RTC takes its base. */
   {
      gpsp_wallclock = psp_local_wallclock;
      /* THE CARTRIDGE CLOCK IS THE CONSOLE'S (rtc_clock.h): published here
       * so the very first read has it, then kept current by the main loop.
       * A harness run keeps the repeatable frame clock unless it asks
       * (`rtc_wall = 1`): every rig compares runs made at different hours,
       * and a game that tints the screen by time of day would differ. */
      g_rtc_wall_on = have_harness
         ? (int)HARNESS_INT("rtc_wall", 0) != 0
         : g_pcfg.rtc_realtime != 0;
      rtc_wall_publish(1);
      fe_evt("rtc_wall on=%d src=%s", g_rtc_wall_on,
             have_harness ? "harness" : "config");
#ifdef GPSP_KEEP_TELEMETRY
      {
         ScePspDateTime dt;
         time_t libc_now = time(NULL);
         memset(&dt, 0, sizeof(dt));
         sceRtcGetCurrentClockLocalTime(&dt);
         fe_evt("rtc_seed local=%04u-%02u-%02u %02u:%02u:%02u "
                "wallclock=%lld libc_time=%lld",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                (long long)psp_local_wallclock(), (long long)libc_now);
      }
#endif
   }
}

/* SESSION LOAD: the heap this game's unload must give back (snap_pre), the GBA
 * link's cable reserve, the boot gate, the ROM cache, the rewind pool, the core's
 * boot, the cheats, the ME and its renderer, the writer thread.  Its exits are C1
 * (mem_budget) and C2 (load_failed).  inproc_n is read by a log line only, which a
 * release build compiles out (hence `unused`). */
static __attribute__((noinline, section(".text.fe_cold.session_load"))) int session_load(
   const fe_host_config *cfg, heap_snap_t *snap_pre, const char *rom_path,
   __attribute__((unused)) int inproc_n)
{
   /* The boot runs outside the loop's park point for up to seconds: the ROM
    * load (a resident 32 MiB cart), the battery, the ME module and renderer
    * (me_rend_init), the wireless bring-up of a harness role.  The power
    * switch waits for the loop under pwr_hold("boot"), taken with the game's
    * selection (at session_top, r7-L5); every failure exit below releases
    * it through FE_HANDOFF / FE_EXIT_GAME. */
#ifndef GPSP_NO_TELEMETRY
   heap_census("post_select");
#endif
   /* The heap this game's unload must give back, byte for byte -- taken
    * before the GBA link's cable reserve, which the in-process return
    * frees with the game (docs/INPROC-RETURN.md). */
   heap_trace_mark();
   heap_snap(snap_pre);
   fe_evt("inproc_session n=%d rom=%s", inproc_n, rom_path);
#ifdef GBA_LINK
   /* A menu cable session needs it too, unless this game can only host
    * and join Wireless Adapter rooms (link_games.h + the override).  Not in
    * late mode (GBA-LINK-MIRROR.md 19): a session's memory then comes from
    * instance B's block, withdrawn from the ROM cache at session start. */
   if (!g_gba_late &&
       (g_gba_cable ||
        (g_gba_cable_ok &&
         ui_link_mode_for(g_game_code, g_game_key) != LINK_ADAPTER)))
   {
      /* Taken before the boot gate so the gate sees what the core will:
       * a reserve that would leave the core below its minimum (a hang, see
       * MEM_GBA_CORE_MIN) is handed back, and the session then fails to
       * start with an EVT instead. */
      long kb = HARNESS_INT("cable_reserve_kb",
                               (long)CABLE_RESERVE_KB);
      unsigned left = 0;
      if (kb > 0 && kb <= 16384)
         g_cable_reserve = malloc((size_t)kb << 10);
      if (g_cable_reserve && !mem_budget_gba_ok(&left))
      {
         free(g_cable_reserve);
         g_cable_reserve = NULL;
         fe_evt("cable_reserve released reason=core_min largest=%u", left);
      }
      fe_evt("cable_reserve kb=%ld got=%d", kb, g_cable_reserve != NULL);
   }
#endif
#if defined(GBA_LINK) || defined(GPSP_MEM_BUDGET)
   /* THE BOOT GATE (docs/MEMORY-BUDGET.md): a console that cannot give the
    * core its minimum gets a screen that says so, not the NULL-page hang. */
   if (g_pcfg.console == FE_CONSOLE_GBA)
   {
      unsigned have = 0;
      if (!mem_budget_gba_ok(&have))
      {
         char l2[80], l3[80];
         snprintf(l2, sizeof(l2), "A GBA game needs %u KiB of free memory",
                  (unsigned)(MEM_GBA_CORE_MIN >> 10));
         snprintf(l3, sizeof(l3), "to start; this console has %u KiB left.",
                  have >> 10);
         fe_evt("mem_budget FAIL need=%u largest=%u", (unsigned)MEM_GBA_CORE_MIN,
                have);
         ui_loading_finish(0);
         boot_stop_notice("NOT ENOUGH MEMORY", "This build cannot start a game here.",
                          l2, l3, 15000);
         fe_evt("exit code=8 reason=mem_budget");
         /* 8, as logged and as docs/MEMORY-BUDGET.md says (F7): the handoff
          * said 1, which is load_failed's code. */
         exit_early_end(XE_EVT_FIRST, 8, "mem_budget", 0);   /* ADR-0053 */
         return 0;
      }
   }
#endif
   /* THE ROM PAGE CACHE'S SIZE, chosen before retro_init takes it. */
   rom_cache_select(rom_path);

   /* Boot observer only: page faults and normal emulation never draw UI. */
   gpsp_rom_load_progress = rom_loading_progress;
#ifdef REWIND
   /* Before the cartridge: the core fills the heap with it on a PSP-1000. */
   g_rw_on = 0;
   g_rw_asleep = 0;
   g_rw_pool = 0;
   g_rw_showing = 0;
   g_rw_show = NULL;
   g_rw_slot = NULL;
   memset(&g_rwh, 0, sizeof(g_rwh));
   g_rwh.head = -1.5707963f;            /* the ring's head starts at the top */
   g_rwh.sp = 1.0f;
   psp_rewind_read_ini();               /* the Settings toggle, as it is NOW */
   if (g_rw_want && g_pcfg.console == FE_CONSOLE_GBA)
   {
      size_t pool_bytes;
      void *pool = psp_rewind_pool_offer(&pool_bytes);
      fe_host_rewind_set_pool(pool, pool_bytes);
      (void)fe_host_rewind_reserve(&g_rw_opts);
   }
   else
      fe_host_rewind_prepare(NULL);     /* off: the store paths carry no marks */
   psp_rom_pool_offer();
#endif
   int boot_rc = fe_host_boot(cfg);
   gpsp_rom_load_progress = NULL;
#ifdef REWIND
   if (boot_rc == 0 && g_rw_want && g_pcfg.console == FE_CONSOLE_GBA)
      g_rw_on = fe_host_rewind_enable(&g_rw_opts) == 0;
   g_rw_gb = boot_rc == 0 && g_rw_want && g_pcfg.console != FE_CONSOLE_GBA;
   if (!g_rw_on)
      g_rw_pool = 0;
#endif
   /* The Wireless menus' view of this game (docs/GBA-LINK-UI.md): can it
    * host a Link Cable room in this boot (instance B loaded)? */
   ui_set_game(g_pcfg.console == FE_CONSOLE_GBA ? g_game_code : NULL,
               g_pcfg.console == FE_CONSOLE_GBA ? NULL : g_gb_title,
               g_game_key, fe_host_gba_cable_ready());
   snprintf(g_shot_rom, sizeof(g_shot_rom), "%s", rom_path);   /* shot names */
   if (g_cb_failed && boot_rc == 0)
      osd_toast("HOME and sleep are unavailable (error %08X)",
                (unsigned)g_cb_failed);
#ifdef GPSP_CHEATS
   /* This game's cheat list and the player's choices (docs/CHEATS.md).  After
    * the boot: the list is static, the file is read with sceIo, so the core's
    * take-everything ROM allocation is undisturbed.  Codes first run at the
    * top of the first emulated frame. */
   if (boot_rc == 0)
   {
      int on = ui_cheats_load(rom_path, g_pcfg.console, 1);
      /* A session that is already up (a harness boot that hosts) locks them
       * before the first frame, and says so instead. */
      cheats_link_poll();
      if (on == 1 && !fe_cheats_locked())
         osd_toast("1 cheat on");
      else if (on > 1 && !fe_cheats_locked())
         osd_toast("%d cheats on", on);
   }
#endif
   if (boot_rc == 0)
   {
      rom_loading_stage("Starting renderer");
      /* ADR-0028: adopt the core's real audio rate now that it exists. The
       * audio thread re-reads g_audio_step every output chunk, so this is
       * picked up without restarting it. */
      unsigned r = fe_host_sample_rate();
      if (r)
      {
         in_rate = r;
         pace_audio_step(PACE_NOMINAL_X100);
         fe_evt("audio_rate in=%u out=%u step=%u", in_rate, OUT_RATE,
                g_audio_step);
      }

      /* ---- ADR-0080: Media Engine bring-up ---------------------------
       * All three keys are harness-only and default OFF: the shipped
       * binary is single-core byte-for-byte until an ini says otherwise.
       * me_boot loads the kernel PRX and handshakes; me_bench runs the
       * Stage-0 microbenchmark (boot-time only — the WLAN-contention arm
       * runs it while a session is live, which is the go/no-go gate the
       * MEDIAENGINE findings demand); me_video enables the Stage-1
       * staging pipeline, which additionally needs the boot to have
       * produced a core framebuffer to double-buffer. */
      {
         /* Defaults were hardcoded 0, which silently left the PLAYABLE build
          * single-core despite PLAY_ME_BOOT/PLAY_ME_VIDEO=1 (the same latent
          * bug class as the exit_assist default).  PLAY_* now reaches here. */
         /* config.ini is the base, harness overrides.  It became a player
          * key for the standby investigation: `me_mode = 0` turns the
          * RENDERER off but the module still loads, so the first attempt to
          * isolate "did we ever load a kernel module" was void -- the log said
          * `me_init state=up` with me_mode=0.  This key is the real switch. */
         int meb = (int)HARNESS_INT("me_boot",
                                       g_pcfg.me_boot);
         int mbn = (int)HARNESS_INT("me_bench", 0);
         int mev = (int)HARNESS_INT("me_video", PLAY_ME_VIDEO);
         if (g_pcfg.console != FE_CONSOLE_GBA)
            meb = mbn = mev = 0;
         if (g_me_rend_cfg)
         {
            meb = 1;         /* the renderer needs the ME booted */
            mev = 0;         /* and owns the ME: staging offload excluded */
         }
         /* me_boot = 0 IS AUTHORITATIVE, and it has to be, because the load is
          * `meb || mbn || mev` and me_video defaults to 1 in a playable build.
          * Two standby runs were spent on a switch that did not switch: the
          * log said `me_init state=up` with me_boot = 0 because me_video had
          * already voted yes.  A key that means "never touch the second core"
          * must outrank every other vote, including the renderer's. */
         if (!g_pcfg.me_boot)
         {
            meb = mbn = mev = 0;
            g_me_rend_cfg = 0;
         }
         fe_evt("me_boot cfg=%d boot=%d bench=%d video=%d rend=%d standby=%d",
                g_pcfg.me_boot, meb, mbn, mev, g_me_rend_cfg, g_pcfg.standby);
         /* With standby armed the ME may only come up if it can take the
          * kernel's SceMeRpc sysevent slot -- that handler is what puts the
          * second core away on suspend, and a console that cannot do that
          * hard-resets on wake.  Single-core is the safe degradation. */
         me_host_require_sysevent(g_pcfg.standby ? 1 : 0);
         if (meb || mbn || mev)
         {
            /* The module stays resident across an in-process return, so this
             * game's unload will not give back what its load takes: move
             * the census's reference by exactly that (none at all when a
             * retained module is taken up again). */
            unsigned kf = (unsigned)sceKernelTotalFreeMemSize();
            unsigned km = (unsigned)sceKernelMaxFreeMemSize();
            int me_rc = me_host_init(g_dir_base);
            if (me_host_loaded())
            {
               snap_pre->kfree -= kf - (unsigned)sceKernelTotalFreeMemSize();
               snap_pre->kmax  -= km - (unsigned)sceKernelMaxFreeMemSize();
            }
            if (me_rc < 0 && g_pcfg.standby)
            {
               /* The one failure a player must be TOLD about.  With standby on
                * the engine refuses to start unless it can take the kernel's
                * sleep handler, so a console in this state is running
                * single-core -- correct and safe, but visibly slower with no
                * explanation, because a release build has no log to read.
                * Silence here would read as "this emulator is slow". */
               osd_toast("Sleep unsupported here: running single-core");
               osd_toast("Set standby = 0 in CONFIG.INI for full speed");
            }
            if (me_rc == 0)
            {
               if (mbn)
                  me_host_bench(76800, 32);
               if (g_me_rend_cfg)
               {
                  /* MEDIA ENGINE MODE: allocate + go, right now.  Unlike
                   * me_video this is a whole-app mode, not session-gated —
                   * the np_start heap interaction is a known open question
                   * (ADR-0080d); alloc failure or a mid-run wedge falls
                   * back to CPU rendering, never breaks the app. */
                  if (me_rend_init() != 0)
                     g_me_rend_cfg = 0;
               }
               else if (mev && gba_screen_pixels)
               {
                  /* ADR-0080c/d: CONFIGURE only.  No allocation here — the
                   * buffers (242 KB) are allocated at session-up in
                   * me_video_activate, AFTER np_start has claimed its heap.
                   * Pre-allocating at boot starved the host's np_start
                   * (auto343).  Activation + alloc both wait for g_net_up,
                   * so the ME video path never touches session creation on
                   * host or join. */
                  g_me_video_cfg = 1;
                  fe_evt("me_video armed (alloc + activate when session up)");
               }
            }
         }
      }
   }
   else
   {
      int refusal = fe_host_battery_refusal();
      ui_loading_finish(0);
      /* A save that exists but could not be read stops the boot, so the
       * game cannot overwrite it.  Say so: a bare return to the XMB left
       * the player with a game that would not start and no reason. */
      if (refusal == 1)
         boot_stop_notice("SAVE FILE COULD NOT BE READ",
                          "The Memory Stick could not read its .sav file.",
                          "The file has not been changed. Back it up,",
                          "then rename it to start without the save.",
                          30000);
      else if (refusal == 2)
         boot_stop_notice("NOT ENOUGH MEMORY",
                          "No memory is left to load this game's save.",
                          "The save has not been changed.", NULL, 30000);
      fe_evt("exit code=1 reason=load_failed battery=%d", refusal);
      exit_early_end(XE_EVT_FIRST, 1, "load_failed", 0);   /* ADR-0053 */
      return 0;
   }

   ui_loading_finish(1);
   rig_rom_identity(rom_path);

   /* Heap headroom after the core's greedy ROM-buffer allocation (Gate-1
    * memory measurement; PPSSPP models more RAM than a PSP-1000 — the real
    * verdict lands at Gate 4-H). */
   fe_evt("mem_free=%d max_block=%d",
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
#ifndef GPSP_NO_TELEMETRY
   {
      /* The same moment, inside the heap: this is the post-load budget the
       * PSP-1000 lives on (see heap_budget).  Then the translation-cache
       * tier the core chose in retro_init -- once per boot, since the core
       * decides once per process.  tier=none: a GB/GBC game, no dynarec. */
      unsigned hb_total = 0, hb_top = 0;
      heap_budget(&hb_total, &hb_top);
      fe_evt("heap_budget free=%u top=%u", hb_total, hb_top);
      heap_census("post_load");
      {
         extern u32 rom_cache_watermark;
         fe_evt("jit_cache tier=%s rom=%u ram=%u base=%p reason=%s forced=%u "
                "mod8k=0x%04x stubs=%u",
                jit_cache_tier == 2 ? "large" :
                jit_cache_tier == 1 ? "small" : "none",
                (unsigned)rom_translation_cache_size,
                (unsigned)ram_translation_cache_size,
                (void *)rom_translation_cache, jit_cache_reason,
                (unsigned)jit_cache_force_small,
                (unsigned)((uintptr_t)rom_translation_cache & 8191u),
                (unsigned)rom_cache_watermark);
      }
   }
#endif

   /* From here on the memory stick is somebody else's thread (ADR-0024).
    * Deliberately after boot: a BIOS/ROM failure must reach the stick even
    * if we never get to the main loop. */
   /* GB/GBC: the battery snapshot stays on the emulation thread (the core
    * owns that RAM); the writer thread only writes the copy it is handed
    * (fe_host.c, gb_save_flush_periodic). */
   io_thread_start();
   return -1;
}

/* SESSION NET UP: the harness's autopilot script and its wireless role (host or
 * join), around the booted core.  Its exits are C3 (bad_script) and C4
 * (net_failed). */
static __attribute__((noinline, section(".text.fe_cold.session_net_up"))) int session_net_up(
   int have_script, const char *script_name, int net_host, int net_join, int net_probe,
   const char *group, const char *nick)
{
   char script_path[192];
   if (have_script)
   {
      snprintf(script_path, sizeof(script_path), "%s/%s", g_dir_base, script_name); /* @lab */ LAB_HOOK(lab_task_file(script_path, sizeof(script_path), script_name);)
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
      snprintf(g_script_path, sizeof(g_script_path), "%s", script_path);
#endif
      if (fe_autopilot_load(script_path) != 0)
      {
         /* The ME and its renderer stop before the core is freed, in the
          * loop teardown's order (F9: they were left running). */
         exit_early_begin(XE_ME | XE_CORE);
         fe_evt("exit code=2 reason=bad_script");
         exit_early_end(0, 2, "bad_script", 0);   /* ADR-0053 */
         return 0;
      }
   }

   if (net_host || net_join)
   {
      /* Bring the wireless session up around the booted core.  Blocks in
       * adhocctl group join (<= 30 s); the game starts running while the
       * netdrv JOIN/WELCOME handshake completes via per-frame pumps. */
      me_rend_suspend();               /* clean heap for np_start (run 407) */
      if (net_bringup(net_host, group, nick, net_probe) != 0)
      {
         exit_early_begin(XE_ME | XE_CORE);          /* F9, as bad_script above */
         fe_evt("exit code=4 reason=net_failed");
         exit_early_end(0, 4, "net_failed", 0);   /* ADR-0053 */
         return 0;
      }
      me_rend_resume();                /* re-alloc from the post-np_start heap */
      /* GB/GBC: the link session -- after the harness's save state is in
       * (gblink_service starts it then). */
      if (g_pcfg.console != FE_CONSOLE_GBA && !g_autoload_state &&
          !g_gblink_start_frames)
      {
         g_gblink_auto_started = 1;
         gblink_begin(net_host);
      }
   }
   return -1;
}

/* SESSION PRE-LOOP: the stall watch and the profilers, the harness link's hold,
 * then the hand-over to the frame loop: the boot's power hold ends here (the
 * loop can park from now on) and the pause screen parks too.  No exits. */
static __attribute__((noinline, section(".text.fe_cold.session_pre_loop"))) void session_pre_loop(
   void)
{
#if defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
   g_stall_s = (int)HARNESS_INT("stall_watch_s", 0);
#ifdef GPSP_PERF_RIG
   g_stall_exit_s = (int)HARNESS_INT("stall_exit_s", 0);
   g_stall_exit_hard =
      (int)HARNESS_INT("stall_exit_hard", 0) != 0;
   if (g_stall_exit_s > 0)
      fe_evt("stall_exit on s=%d hard=%d", g_stall_exit_s, g_stall_exit_hard);
#endif
#ifdef GPSP_STALL_RECORDER
   /* The release-layout stall recorder (a hard-freeze bisect arm): always on,
    * 6 s, and nothing is written unless the main loop really stops. */
   if (g_stall_s <= 0)
      g_stall_s = 6;
   sceIoMkdir(LOG_DIR, 0777);
#endif
#endif
   /* A harness link run holds its game (as the pause menu would) from here
    * until the session starts, so the state it links from is the loaded
    * .st0 exactly, however long the radio takes. */
   if (g_gblink_role && g_pcfg.console != FE_CONSOLE_GBA && !g_gblink_start_frames)
      fe_host_gblink_hold(1);
   stall_watch_start();
   drprof_start();
   rig_cfg_audit_report();   /* every harness key has been read by now */

   pwr_release();            /* the boot's hold: the loop can park from here */
   ui_set_wake_yield(wake_overlay_yield);   /* the pause screen parks too */
}

