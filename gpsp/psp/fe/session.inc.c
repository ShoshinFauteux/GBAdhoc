/* ---- THE SESSION (Epoch C C3, builds/epochc-out/PLAN-EPOCHC-beta5.md) -----------
 * session_reset: what every game starts from, set again before the in-process
 * return goes back to the game list (docs/INPROC-RETURN.md) -- the statements
 * main() ran there, in their order.  main()'s locals are passed by address
 * (always_inline: they stay registers of main()), the values the boot latched
 * by value.  The tools read the call as this body (exit_matrix.EPOCHC_HELPERS;
 * tests/extract.epochc_expand). */
static inline __attribute__((always_inline)) void session_reset(
   int *exit_code, const char **exit_reason, int *ff_engaged, int *ff_toggled,
   int *ff_last_mode, int *ff_last_me, unsigned *prev_pad, int *chord_frames,
   int *inproc_menu_armed, int *autoload_state_slot, int me_rend_cfg_boot,
   int sram_thread_boot, int autoload_boot)
{
   g_running = !g_exit_req;
   g_relaunch = 0;
   *exit_code = 0;
   *exit_reason = NULL;
   *ff_engaged = 0;
   *ff_toggled = 0;
   *ff_last_mode = -1;
   *ff_last_me = -1;
   *prev_pad = 0;
   g_pad_swallow = 0;          /* nothing of the last game is held */
   *chord_frames = 0;
   g_dump_chord_pending = 0;   /* no screenshot of the next game */
   ctl_turbo_stop(&g_turbo);   /* the game was left: nothing auto-fires */
   ui_set_turbo_locked(0);     /* ...and no session outlives it */
   *inproc_menu_armed = 0;
   g_ff_uncapped = g_ff_mult = g_ff_user = g_ff_gate = 0;
   g_blit_suppress = 0;
   g_drew = 0;
   memset(&g_ff_gate_st, 0, sizeof(g_ff_gate_st));
   g_wake_have = 0;
   g_wake_menu = 0;
   g_last_pix = NULL;
   g_me_rend_cfg = me_rend_cfg_boot;
   g_me_video_cfg = 0;
   g_me_video = 0;
   g_pcfg.sram_thread = sram_thread_boot;
   g_autoload_state = autoload_boot;
   *autoload_state_slot = 1;
   g_autoload_from_shelf = 0;
}

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
   else if (have_harness && !fe_ini_get_int(HARNESS_INI, "browser", 0))
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
      fe_ini_get(HARNESS_INI, "rom", kr, sizeof(kr));
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
       !fe_ini_get_int(HARNESS_INI, "browser", 0)))
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

