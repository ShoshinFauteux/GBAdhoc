/* ---- BOOT (Epoch C C2, builds/epochc-out/PLAN-EPOCHC-beta5.md) ----------------
 * Everything main() did before its first game, in the order it always ran:
 * the exit callback, the paths (init_paths: the Lab build's own exits run in
 * there, so it stays ahead of every power hold), the log, CONFIG.INI, the
 * netdrv arena, display and audio, the harness keys, the boot probes (their
 * exits: family A), the GB core's allocation.  The text below is main()'s,
 * moved unchanged; its locals are boot_run's own, and the ones the session
 * reads later go back to main() through boot_ctx.  CONFIG.INI's document
 * (8 KiB) now lives in this frame and is gone when the boot is.
 *
 * Returns -1 when the boot is done, or main()'s return value after a boot
 * probe ended the process (exit_early_end: a failed sceKernelExitGame falls
 * through to main()'s `return 0` as it always did).  noinline, in its own
 * section (PLAN-EPOCHC-beta5 §4.2): never inlined back into main(), never in
 * front of it. */
typedef struct boot_ctx
{
   char script_name[64];
   char nick[24];
   char group[16];
   long autoexit;
   long dump_at;
   long dump_every;
   long gedump_at;
   long gedump_every;
   long ff_ini;
   long ui_demo;
   long simff;
   long ui_ctl_demo;
   long wake_shot;
   long player_shot_at;
   long player_shot_n;
   long vhash_from;
   long vhash_to;
   long vid_prof_win;
   int have_script;
   int have_harness;
   int have_variant;
   int net_host;
   int net_join;
   int net_probe;
   int folders;
   int me_rend_cfg_boot;
   int sram_thread_boot;
   int autoload_boot;
   long inproc_cycles;
   long inproc_frames;
   long state_save_at;
   int inproc_via_menu;
   char inproc_roms[6][64];
   int inproc_rom_n;
} boot_ctx;

static __attribute__((noinline, section(".text.fe_cold.boot_run"))) int boot_run(int argc,
   char *argv[], boot_ctx *bc)
{
   char script_name[64];
   char nick[24];
   char group[16] = "";
   long autoexit;
   long dump_at;
   long dump_every;
   long gedump_at;
   long gedump_every;
   long ff_ini;
   long ui_demo;
   long simff;
   long ui_ctl_demo = 0;
   long wake_shot = 0;
   long player_shot_at = 0;
   long player_shot_n = 1;
   long vhash_from;
   long vhash_to;
   long vid_prof_win = 0;
   int have_script = 0;
   int have_harness;
   int have_variant;
   int net_host = 0;
   int net_join = 0;
   int net_probe = 0;
   int folders = 0;
   int me_rend_cfg_boot = 0;
   int sram_thread_boot = 0;
   int autoload_boot = 0;
   long inproc_cycles = 0;
   long inproc_frames = 0;
   long state_save_at = 0;
   int inproc_via_menu = 0;
   char inproc_roms[6][64];
   int inproc_rom_n = 0;
   /* CONFIG.INI, read ONCE for the whole boot (fe_ini_doc): every
    * pcfg_load key.  8 KiB of this stack, no heap; main() never returns, so
    * it simply stays here. */
   fe_ini_doc cfg_doc;
   int cfg_ok;

   BOOT_T0_SET();
   /* stdout unbuffered: nothing reads it on a PSP, and a buffered stdout
    * takes a 1 KiB heap buffer at its first printf and keeps it for the
    * process -- possibly mid-game or mid-session (docs/INPROC-RETURN.md). */
   setvbuf(stdout, NULL, _IONBF, 0);
   setup_exit_callback();

   /* The clock the firmware or CFW left us; ours is set once config.ini is
    * read (psp_clock_apply "boot", below pcfg_load_doc). */
   psp_clock_capture();

   init_paths(argc, argv);
#ifdef GPSP_PERF_RIG
   boot_ballast_take();              /* first heap use of the process */
#endif
   /* Relaunched by "Quit to game list" (relaunch_eboot): the browser fades
    * in from the firmware's black instead of cutting in. */
   if (argc > 1 && argv[1] && strcmp(argv[1], RELAUNCH_ARGV1) == 0)
      ui_return_fade_in();
   /* Started from a game's XMB entry (XMB LAUNCH, at relaunch_eboot). */
   if (argc > 2 && argv[1] && argv[2] && strcmp(argv[1], LAUNCH_ARGV1) == 0)
      snprintf(g_launch_rom, sizeof(g_launch_rom), "%s", argv[2]);
#ifndef GPSP_PERF_RIG
   /* ADR-0067's harness path cannot exist in this build, and every one of
    * main()'s ~130 harness keys used to pay a Memory Stick negative lookup
    * to learn that, on a black screen, on every launch (boot timeline,
    * docs/UI-OVERLAY.md).  The call sites still run -- one code path --
    * they just no longer touch the stick.  file_exists() below still asks. */
   fe_ini_never(HARNESS_INI);
#endif
#ifdef GPSP_KEEP_TELEMETRY
   sceIoMkdir(LOG_DIR, 0777);
   rig_preserve_log();               /* before fe_evt_init truncates it */
   fe_evt_init(LOG_PATH, 0);
   /* Every lookup of these two files is recorded from here on, so the report
    * before the main loop can name a key nothing ever read. */
   fe_ini_audit(HARNESS_INI); /* @lab */ LAB_HOOK(lab_audit_keys();)
   fe_ini_audit(CONFIG_INI);
   fe_evt_set_clock(evt_clock_us);   /* ADR-0021: price the ms0 flushes */
#endif
   /* ADR-0054: stamp WHICH BINARY produced this log.  The config line already
    * records the parameters, so runs could be grouped into A/B arms by config
    * -- and that silently mixed three different builds that happened to share
    * a config into one "arm".  A run must carry its own build identity or the
    * arm is not an arm.  __DATE__/__TIME__ are the compiler's, so they change
    * on every rebuild and cost nothing at runtime. */
   fe_evt("build stamp=%s_%s", __DATE__, __TIME__);
   fe_evt("boot_ok");
#ifndef GPSP_NO_TELEMETRY
   boot_mem_census();                /* docs/MEMORY-BUDGET.md */
#endif
   /* ADR-0039: the core/frontend pixel-layout contract is a build flag on
    * both sides with nothing checking it at link time.  Say which one this
    * binary was built for, so a field log with wrong colors is one grep from
    * an answer instead of a rebuild. */
#ifdef USE_PSP_RGB565_FORMAT
   fe_evt("pixfmt=psp5650 stage=copy");
#else
   fe_evt("pixfmt=rgb565 stage=swap");
#endif
   fe_log("base_dir=%s", g_dir_base);

   sceCtrlSetSamplingCycle(0);
   sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
   boot_mark("entry");

   /* Frontend resources are allocated BEFORE retro_init eats the heap for
    * ROM blocks (FRONTEND-AUDIT §8): GU uses VRAM + static list, audio ring
    * is static, audio thread stack is created here.  CONFIG.INI is read
    * once, into cfg_doc, and every pcfg_load key is answered from it
    * (docs/UI-OVERLAY.md §12.8). */
   cfg_ok = fe_ini_doc_load(&cfg_doc, CONFIG_INI) == 0;
/* @extract:begin vol:boot_apply */
   pcfg_load_doc(CONFIG_INI, cfg_ok ? &cfg_doc : NULL);
   psp_audio_volume_apply();          /* before audio_start (Settings > Volume) */
   /* CPU clock: the setting, or the harness's `cpu_clock` for a Lab arm
    * (kept apart from g_pcfg so a run never writes it into config.ini).
    * Only ms of config reading ran at the entry clock before this. */
   {
      long hc = HARNESS_INT("cpu_clock", -1);
      g_clk_harness = hc < 0 ? -1 : pcfg_clock_snap(hc);
      /* @lab */ LAB_FFP({ int ffc = lab_ffpace_boot(HARNESS_INI, 0, &g_me_rend, &g_mer_last_presented); if (ffc > 0) g_clk_harness = pcfg_clock_snap(ffc); })
   }
   psp_clock_apply("boot"); /* @lab */ LAB_FFP(lab_ffpace_clock_enforce();)
   fe_evt("clock=%d", scePowerGetCpuClockFrequency());
   boot_mark("config");

   /* RESERVE THE NETDRV BLOCK WHILE THE HEAP IS STILL PRISTINE.
    *
    * netdrv is ~638 KiB in one piece (five peers x a 192-slot reliable
    * backlog).  It used to be calloc'd when the user starts a session --
    * after a 16 MiB ROM, the ME stages and the browser's 512 KiB art
    * textures have been through the heap.  On a PSP-1000, with half the user
    * partition of a 2000/3000/Go, the largest free hole at that moment can be
    * under 638 KiB while total free memory still looks healthy, and wireless
    * fails for a reason that has nothing to do with wireless.  Field report:
    * the 1000 could neither host nor join, and (because np_start returned -1,
    * which collides with ADHOC_ERR_WLAN_OFF) reported "WLAN switch is OFF"
    * with the switch on.
    *
    * Taking it here costs every player 638 KiB they may never use, and buys
    * that a session cannot fail on fragmentation.  If even this fails, leave
    * the arena unset: netdrv_create falls back to calloc and behaves exactly
    * as it did before. */
   {
      /* THE EXACT SIZE OF WHAT LIVES HERE.
       *
       * This block was exactly 1 MiB until 4.6.0, because init_gamepak_buffer
       * used to leave the frontend only the remainder R of "1 MiB blocks until
       * malloc fails", and reserving the struct's own 504 KiB once ate R on a
       * PSP-1000: "Failed to allocate frame buffer (77280 bytes)", Emerald back
       * at the XMB.  The loop's post-load reserve now holds that floor (>= 1
       * MiB, whatever the binary's size), so nothing here depends on this
       * block's size any more, and the ~150 KiB it left idle was ROM cache.
       *
       * In order: netdrv, the wake overlay's frozen frame, the menu's five
       * state thumbnails (the Save/Load state screens show them in game,
       * where the heap is gone, docs/UI-OVERLAY.md).  Both cost nothing
       * beyond this block, and each is laid out on a 64-byte line.
       *
       * The screenshot encoder's work memory (issue #13, 206,912 B for a
       * GBA frame) is NOT a fourth tenant any more: it borrows netdrv's own
       * part, which is idle whenever no session is up
       * (netdrv_arena_borrow; player_screenshot falls back to BMP while a
       * session holds it).  That was 6.3 ROM-cache pages held for every
       * player for a one-off encode. */
      size_t need = netdrv_sizeof();
      size_t take = need + 63u + WAKE_FRAME_BYTES + 63u + UI_THUMB_BYTES;
      void  *blk  = malloc(take);
      if (blk)
      {
         netdrv_set_arena(blk, need);
         g_wake_frame = (uint16_t *)(((unsigned)blk + need + 63u) & ~63u);
         g_ui_thumbs = (uint16_t *)(((unsigned)g_wake_frame +
                                     WAKE_FRAME_BYTES + 63u) & ~63u);
         ui_set_thumb_buffer(g_ui_thumbs);
         fe_evt("shot_work bytes=%u want=%u %s", (unsigned)need,
                (unsigned)fe_png_work_bytes(FE_GBA_WIDTH, FE_GBA_HEIGHT),
                need >= fe_png_work_bytes(FE_GBA_WIDTH, FE_GBA_HEIGHT)
                   ? "borrow" : "bmp-only");
      }
      fe_evt("net_arena need=%u took=%u %s free=%d max_block=%d",
             (unsigned)need, (unsigned)take, blk ? "reserved" : "FAILED",
             sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
   }

   boot_mark("arena");
   vid_init();
   psp_display_apply("boot");
   audio_start();
/* @extract:end vol:boot_apply */

   have_harness = HARNESS_PRESENT();
   have_variant = file_exists(VARIANT_INI);
   rig_boot_identity(have_harness);
#ifdef GPSP_KEEP_TELEMETRY
   /* The binary's identity on EVERY harness run, not only vid_prof ones: the
    * rig refuses to pair two consoles' logs unless their CRCs agree. */
   if (have_harness && !vp_build_crc)
      vp_build_crc = eboot_crc();
#endif

   /* ADR-0036.  A leftover `autopilot.ini` is IGNORED, never obeyed — and
    * never deleted, because it is the user's file even when it is in the way.
    * Say so on the very first line anyone will read: the failure this closes
    * was a solo benchmark that silently came up in a wireless session, and
    * the user had to infer it from the frame rate. */
   if (file_exists(LEGACY_AUTOPILOT_INI))
      fe_evt("legacy_autopilot_ignored file=%s — autopilot.ini is no longer "
             "read; the harness channel is .gpsp-harness.ini (ADR-0036). "
             "This file is being left alone, not deleted.",
             LEGACY_AUTOPILOT_INI);

   /* And the live channel is never silent either.  If a stale harness file
    * is what is driving this boot, the log says so before anything it
    * configures can surprise anyone. */
   if (have_harness)
      fe_evt("harness_ini active file=%s — automated control channel is "
             "present; ROM browser suppressed and harness keys apply",
             HARNESS_INI);
   /* FOLDERS (at make_suffixed_path): the player's layout, and the empty
    * roms/GBA, roms/GB, roms/GBC, saves/ and states/ on first run. */
   folders = (int)HARNESS_INT("folders",
      !have_variant && (!have_harness ||
                        HARNESS_INT("browser", 0) != 0));
   if (folders)
      sc_first_run(SC_FS_PSP, g_dir_base, ROM_DIR);
   autoexit   = HARNESS_INT("autoexit_frames", 0); /* @lab */ LAB_HOOK(lab_set_eboot_crc(vp_build_crc);)
   {
      /* GB link session harness keys (see gblink_service). */
      char gr[8] = "";
      if (HARNESS_STR("gblink_role", gr, sizeof(gr)))
         g_gblink_role = !strcmp(gr, "host") ? 1 : !strcmp(gr, "join") ? 2 : 0;
      /* config.ini gblink_delay: the player's own tuning (frames of input
       * delay, 1-30); the harness key, when present, wins. */
      g_gblink_delay = g_pcfg.gblink_delay_cfg >= 0
                     ? (unsigned)g_pcfg.gblink_delay_cfg : GBLINK_DELAY_DEFAULT;
      if (g_gblink_delay < 1 || g_gblink_delay > 30)
         g_gblink_delay = GBLINK_DELAY_DEFAULT;
      g_gblink_delay = (unsigned)HARNESS_INT("gblink_delay",
                                                g_gblink_delay);
      if (g_gblink_delay > 60)
         g_gblink_delay = 60;
      g_gblink_delay_auto = (unsigned)HARNESS_INT(
                                                     "gblink_delay_auto", 0);
      if (g_gblink_delay_auto > 30)
         g_gblink_delay_auto = 30;
      if (g_gblink_delay_auto)
         fe_evt("gblink_delay_auto cap=%u", g_gblink_delay_auto);
      g_gblink_seed = (long long)HARNESS_INT(
                                                "gblink_rtc_seed", 0);
      g_gblink_end_after = (unsigned)HARNESS_INT(
                                                    "gblink_end_after", 300);
      fe_host_gblink_tune((unsigned)HARNESS_INT(
                                                   "gblink_bulk", 0));
      /* 0: the partner's cartridge always crosses the radio, even when this
       * card holds a copy -- the transfer arm of a rig whose cards carry
       * every game for the other arms. */
      g_gblink_library = HARNESS_INT("gblink_library", 1) != 0;
      /* psp_clock: pretend the console's clock read this at boot -- e.g.
       * the PSP-1000's 2011 clock (1298863731) in PPSSPP, which is how the
       * hw1 Gen 2 failure is reproduced and its fix checked. */
      g_clock_fake = (long long)HARNESS_INT("psp_clock", 0);
      /* psp_clock_tz: that clock's zone, in seconds ahead of UTC (-18000 =
       * New York in winter), so the GB cartridge clock's UTC arithmetic can
       * be run at any offset in PPSSPP. */
      g_clock_fake_tz = (long long)HARNESS_INT("psp_clock_tz",
                                                  0);
      if (g_clock_fake)
         fe_evt("psp_clock fake=%lld tz=%lld (harness)", g_clock_fake,
                g_clock_fake_tz);
      /* The bulk lane's datagram (bytes incl. the 13-byte header; 0 = the
       * ordered channel carries transfers, 64+13..1472) and its rate
       * (datagrams a frame, 0 = adaptive): the hardware rig's arms. */
      {
         long bb = HARNESS_INT("gblink_bulk_bytes",
                                  (long)g_gblink_bulk_bytes);
         g_gblink_bulk_bytes = bb <= 0 ? 0u : bb < 77 ? 77u :
                               bb > 1472 ? 1472u : (unsigned)bb;
         g_gblink_bulk_rate = (unsigned)HARNESS_INT(
                                                       "gblink_bulk_rate", 0);
      }
      /* Scanline batching while the cable is quiet (host decides for
       * both): 1 = strict alternation (the control arm), default on. */
      g_gblink_batch = (unsigned)HARNESS_INT("gblink_batch",
                                                GBLINK_BATCH_DEFAULT);
      g_gblink_batch_armed = HARNESS_INT("gblink_batch_armed",
                                            GBLINK_ARMED_DEFAULT) != 0;
      /* gblink_live = 0: power on from the battery images (the 3.0-era
       * link); gblink_after = frames the game plays on after a session. */
      g_gblink_live = HARNESS_INT("gblink_live", 1) != 0;
      g_gblink_after = (unsigned)HARNESS_INT("gblink_after", 0);
      g_gblink_copies = (unsigned)HARNESS_INT(
                                                 "gblink_input_copies", 0);
      g_gblink_resume_boot = HARNESS_INT("gblink_resume",
                                            0) == 2;
      g_gblink_pace0 = HARNESS_INT("gblink_pace_slot0", 0) != 0;
      g_gblink_start_frames = (unsigned)HARNESS_INT(
                                                      "gblink_start_frames", 0);
#ifdef GPSP_PERF_RIG
      g_start_keepalive = HARNESS_INT("link_start_keepalive", 1) != 0;
      g_start_grace = HARNESS_INT("link_start_grace", 1) != 0;
      g_start_heard = HARNESS_INT("link_start_heard", 1) != 0;
      g_reload_slow_ms = (unsigned)HARNESS_INT("gblink_reload_slow_ms", 0);
      g_gblink_host_wait_peer = HARNESS_INT("gblink_host_wait_peer", 0) != 0;
      fe_evt("link_start_arms keepalive=%d grace=%d heard=%d slow_ms=%u host_wait_peer=%d",
             g_start_keepalive, g_start_grace, g_start_heard, g_reload_slow_ms,
             g_gblink_host_wait_peer);
#endif
      g_gblink_keep_stamp = HARNESS_INT(
                                           "gblink_rtc_keep_session", 0) != 0;
      /* Smoothness (gblink_service's block comment): config.ini is the
       * player's kill switch, the harness key wins for an A/B arm. */
      {
         long v;
         v = fe_ini_get_int(p_config, "gblink_absorb", 1);
         g_gbl_absorb = HARNESS_INT("gblink_absorb", v) != 0;
         v = fe_ini_get_int(p_config, "gblink_audio_cushion", 3072);
         v = HARNESS_INT("gblink_audio_cushion", v);
         g_gbl_cushion = v < 0 ? 0u : v > 16384 ? 16384u : (unsigned)v;
         v = fe_ini_get_int(p_config, "gblink_audio_follow", 60);
         v = HARNESS_INT("gblink_audio_follow", v);
         g_gbl_follow = v < 0 ? 0u : v > 100 ? 100u : (unsigned)v;
         v = fe_ini_get_int(p_config, "gblink_nack", 1);
         g_gbl_nack = HARNESS_INT("gblink_nack", v) != 0;
         v = fe_ini_get_int(p_config, "gblink_audio_soft", 1);
         g_gbl_soft =
            HARNESS_INT("gblink_audio_soft", v) != 0;
         /* harness only: the sync-hash A/B (both consoles alike) */
         v = HARNESS_INT("gblink_hash_every", 0);
         g_gbl_hash_every = v < 0 ? 0u : (unsigned)v;
         g_gbl_hash_legacy =
            HARNESS_INT("gblink_hash_legacy", 0) != 0;
         v = HARNESS_INT("gblink_win_ms", 10000);
         g_gbl_win_ms = v < 1000 ? 1000u : (unsigned)v;
         fe_evt("gblink_smooth absorb=%d audio_cushion=%u audio_follow=%u "
                "audio_soft=%d nack=%d win_ms=%u hash_every=%u "
                "hash_legacy=%d",
                g_gbl_absorb, g_gbl_cushion, g_gbl_follow, g_gbl_soft,
                g_gbl_nack,
                g_gbl_win_ms, g_gbl_hash_every, g_gbl_hash_legacy);
      }
      fe_evt("gblink_harness role=%s delay=%u seed=%lld end_after=%u "
             "library=%d bulk_bytes=%u bulk_rate=%u batch=%u armed=%d",
             g_gblink_role == 1 ? "host" :
             g_gblink_role == 2 ? "join" : "none", g_gblink_delay,
             g_gblink_seed, g_gblink_end_after, g_gblink_library,
             g_gblink_bulk_bytes, g_gblink_bulk_rate, g_gblink_batch,
             g_gblink_batch_armed);
   }
   /* ADR-0053: harness-ini only and default OFF.  Putting this in CONFIG.INI
    * would let a stale file toggle USB on a console someone is playing --
    * the same failure ADR-0036 renamed autopilot.ini to prevent. */
   handoff_config(HARNESS_INT("handoff", 0),
                  HARNESS_INT("handoff_window_s", 90),
                  HARNESS_INT("handoff_max_runs", 20),
                  HARNESS_INT("handoff_total_s", 900),
                  HARNESS_INT("handoff_park_s", 0));
   /* THE PSP GO'S INTERNAL STORAGE CANNOT BE HANDED OFF.  handoff_run exports
    * the volume with usbstorms.prx, the MEMORY STICK driver, so an app on ef0
    * finishes its run, writes RESULT.TXT to ef0 and then exports the M2 card
    * instead: the PC watches the internal volume, never sees the result, and
    * the Go parks forever on the run's last frame (2026-09-29, GBADHOC-DRPROF
    * installed on G: = ef0; docs/ROM-RESIDENCY.md "Which Go volume").  Refuse
    * it at boot, visibly, and run once to the XMB instead. */
   if (HARNESS_INT("handoff", 0) &&
       (strncmp(g_dir_base, "ef0", 3) == 0 || strncmp(g_dir_base, "EF0", 3) == 0))
   {
      handoff_config(0, 0, 0, 0, 0);
      fe_evt("handoff_refused base=%s reason=usb_export_serves_the_memory_stick "
             "-- install the rig on the Memory Stick (M2), not internal storage",
             g_dir_base);
      osd_toast("Rig is on internal storage: USB loop OFF (use the Memory Stick)");
   }
   dump_at    = HARNESS_INT("dump_at", 0);
   dump_every = HARNESS_INT("dump_every", 0);
   gedump_at  = HARNESS_INT("gedump_at", 0);
#ifdef GPSP_PERF_RIG
   g_gedump_loops = (unsigned)HARNESS_INT("gedump_loops", 0);
#endif
   /* README shoot: the displayed screen (scaled game + OSD) every N frames,
    * log/ge_%06u.bmp, so a clip can be cut from what the PSP actually shows. */
   gedump_every = HARNESS_INT("gedump_every", 0);
   ff_ini     = HARNESS_INT("ff", 0);
#ifdef REWIND
   psp_rewind_read_ini();
#endif
   /* Harness A/B for the startup-sized translation caches: `jit_small = 1`
    * keeps a 64 MiB console on the PSP-1000's small tier, so one EBOOT can
    * measure both on the same console and memory layout.  Must be set before
    * fe_host_boot (the core decides in retro_init); the `jit_cache` line
    * after boot echoes it as reason=forced, so a misspelt key shows up as
    * reason=heap_ok instead of passing silently. */
   jit_cache_force_small = HARNESS_INT("jit_small", 0) ? 1u : 0u;
   /* ADR-0033/0034 (coreopt): the video oracle and renderer path profile,
    * grafted onto the harness channel — the branch predates ADR-0036's
    * autopilot.ini -> .gpsp-harness.ini rename.  vhash=1 is shorthand for
    * "hash every frame"; vid_prof only produces a line if the core was
    * built with -DVIDEO_PROF=1. */
#ifdef GPSP_KEEP_TELEMETRY
   /* Every vid_prof output is an fe_evt line, so a player build leaves the
    * key alone: a stray harness file on a player's card must not cost a
    * boot-time EBOOT read (and its 4 KiB borrow) ahead of the ROM load. */
   vid_prof_win = HARNESS_INT("vid_prof", 0);
   if (vid_prof_win > 0)
   {
      video_prof_set_clock(vprof_clock_us);
      vp_build_crc = eboot_crc();
   }
#endif
   vhash_from = HARNESS_INT("vhash_from", 0);
   g_vhash_on = (int)HARNESS_INT("vhash", 0);
   g_vhash_legacy_rb = (int)HARNESS_INT("vhash_legacy_rb", 0);
   g_telemetry_period = (unsigned)HARNESS_INT(
                                                 "telemetry_period", 0);
#ifdef GPSP_PERF_RIG
   g_shash_on = (int)HARNESS_INT("shash", 0);
   /* Filters v2 rig keys: force the three display rows for every console
    * (the state-identity and cost runs).  -1 = the player's own. */
   g_h_smooth = (int)HARNESS_INT("fx_smooth", -1);
   g_h_effect = (int)HARNESS_INT("fx_effect", -1);
   g_h_color  = (int)HARNESS_INT("fx_color", -1);
   g_h_scale  = (int)HARNESS_INT("fx_scale", -1);
   /* the sleep simulator (sleep_sim_thread) */
   g_ss_at      = HARNESS_INT("sleep_sim_at", 0);
   g_ss_ms      = HARNESS_INT("sleep_sim_ms", 1500);
   g_ss_again_ms = HARNESS_INT("sleep_sim_again_ms", 0);
   ui_wake_inject_frame((int)HARNESS_INT(
                                            "sleep_sim_choice_frame", 30));
   g_ss_rom     = (int)HARNESS_INT("sleep_sim_rom", 0);
   g_ss_sav     = (int)HARNESS_INT("sleep_sim_sav", 0);
   fe_host_sram_test_exit_retry(
      (int)HARNESS_INT("sram_exit_retry", 1));
   g_ss_game    = (int)HARNESS_INT("sleep_sim_game", 0);
   g_ss_browser = (int)HARNESS_INT("sleep_sim_browser", 0);
   g_ss_nohold  = (int)HARNESS_INT("sleep_sim_nohold", 0);
   g_ss_rw_tick = HARNESS_INT("sleep_sim_rewind_tick", 0);
   g_ss_nopark  = (int)HARNESS_INT("sleep_sim_nopark", 0);
   g_ss_request = (int)HARNESS_INT("sleep_sim_request", 0);
   g_ss_nopoll  = (int)HARNESS_INT("sleep_sim_nopoll", 0);
   g_ss_trash_pool = (int)HARNESS_INT("sleep_sim_trash_pool", 0);
   g_ss_me_sync_fail = (int)HARNESS_INT("sleep_sim_me_sync_fail", 0);
   if (g_ss_me_sync_fail)
      fe_evt("sleep_sim mutant me_sync_fail=%d", g_ss_me_sync_fail);
   me_host_fake((int)HARNESS_INT("me_fake", 0));
   g_inproc_me_unload = (int)HARNESS_INT("inproc_me_unload", 0);
   g_ss_no_restart = !HARNESS_INT("rewind_pool_restart", 1);
   g_ss_no_land = !HARNESS_INT("rewind_park_land", 1);
   HARNESS_STR("sleep_sim_window", g_ss_window,
              sizeof(g_ss_window));
   ui_wake_inject((int)HARNESS_INT("sleep_sim_choice", -1));
   ui_batt_inject((int)HARNESS_INT("ui_batt", -1));
   ui_ctl_warn_inject((int)HARNESS_INT("ui_ctl_warn", 0));
   ui_turbo_lock_inject((int)HARNESS_INT("ui_turbo_lock", 0));
#ifdef GPSP_PERF_RIG
   g_turbo_lock_sim = HARNESS_INT("ui_turbo_lock", 0) != 0;
#endif
   if (g_ss_at || g_ss_browser || g_ss_window[0] || g_ss_rw_tick)
      sleep_sim_start();
   if (g_ss_at || g_ss_browser || g_ss_window[0])
      fe_evt("sleep_sim cfg at=%ld ms=%ld rom=%d game=%d browser=%d "
             "window=%s nohold=%d", g_ss_at, g_ss_ms, g_ss_rom, g_ss_game,
             g_ss_browser, g_ss_window[0] ? g_ss_window : "-", g_ss_nohold);
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
   g_sessmem_cycles = (unsigned)HARNESS_INT("sessmem_cycles", 0);
   g_gblink_repeat_left = (unsigned)HARNESS_INT("gblink_repeat", 0);
   g_sessmem_at = (unsigned)HARNESS_INT("sessmem_at", 600);
#endif
   g_shash_dump_from = HARNESS_INT("shash_dump_from", 0);
   g_shash_dump_to   = HARNESS_INT("shash_dump_to", 0);
   {
      char rid[48] = "", arm[16] = "";
      HARNESS_STR("run_id", rid, sizeof(rid));
      HARNESS_STR("arm", arm, sizeof(arm));
      snprintf(g_shash_id, sizeof(g_shash_id),
               "run_id=%s arm=%s eboot_crc=%08x model=%d",
               rid[0] ? rid : "none", arm[0] ? arm : "none",
               (unsigned)vp_build_crc, kuKernelGetModel());
      snprintf(g_hb_id, sizeof(g_hb_id), "%s", g_shash_id);
   }
   g_hb_s = (int)HARNESS_INT("heartbeat_s", 0);
   if (g_hb_s < 0 || g_hb_s > 60)
      g_hb_s = 0;
   fe_evt("shash on=%d heartbeat_s=%d", g_shash_on, g_hb_s);
   if (g_shash_on)
      shash_open();
#endif
   vhash_to   = HARNESS_INT("vhash_to", 0);
   if (HARNESS_INT("vhash", 0) && !vhash_to)
   {
      vhash_from = 1;
      vhash_to   = 0x7FFFFFFF;
   }
   ui_demo    = HARNESS_INT("ui_demo", 0);
   if (ui_demo)
      ui_demo_shots();   /* browser dumps the gallery + auto-picks (README) */
   /* docs/CONTROL-REMAP.md: the Controls page walked end to end, with GE
    * dumps.  Separate from ui_demo so run_ui_smoke.sh's ladder is untouched. */
   ui_ctl_demo = HARNESS_INT("ui_controls_demo", 0);
   /* docs/UI-OVERLAY.md: the wake overlay, opened as SELECT+TRIANGLE opens
    * it, photographed and dismissed -- a script cannot press the chord. */
   wake_shot = HARNESS_INT("wake_shot", 0);
   player_shot_at = HARNESS_INT("player_shot_at", 0);
   player_shot_n = HARNESS_INT("player_shot_n", 1);
   if (ui_ctl_demo == 2)
   {
      ui_controls_browser_demo();   /* from the browser instead */
      ui_ctl_demo = 0;
   }
   if (HARNESS_INT("ui_browser_demo", 0))
      ui_browser_demo_shots();   /* browser states shoot, then exit */
   {
      char bscript[64];
#ifdef GPSP_PERF_RIG
      char up_fake[96];
#endif
      if (HARNESS_STR("ui_browser_script", bscript, sizeof(bscript)))
         ui_browser_script_load(bscript);   /* README shoot: scripted browser */
      /* Share game data (docs/SAVE-SHARE.md), for the two-PPSSPP rig:
       * `share_burst` file datagrams a frame (1 = slow enough to interrupt),
       * `share_build` a pretended build string (the mismatch screens with
       * one EBOOT), `share_fail_write_after` a Memory Stick that fills up
       * after N bytes of a fetched file. */
      ui_share_burst((unsigned)HARNESS_INT("share_burst", 0));
      if (HARNESS_STR("share_build", bscript, sizeof(bscript)) &&
          bscript[0])
         share_build_override(bscript);
      share_fail_write_after((unsigned)HARNESS_INT(
         "share_fail_write_after", 0));
#ifdef GPSP_PERF_RIG
      if (HARNESS_STR("ui_script", bscript, sizeof(bscript)) &&
          bscript[0])
      {
         char up[256];
         snprintf(up, sizeof(up), "%s/%s", g_dir_base, bscript); /* @lab */ LAB_HOOK(lab_task_file(up, sizeof(up), bscript);)
         uiscript_load(up);               /* docs/GBA-LINK-UI.md */
      }
      if (HARNESS_STR("ui_scan_fake", up_fake, sizeof(up_fake)) &&
          up_fake[0])
         ui_scan_fake_set(up_fake);
#endif
   }
   /* Display-features shoot: GE-dump the first N loading-screen frames. */
   ui_loading_dump_shots((int)HARNESS_INT("loading_dump", 0));
   /* Asset shoot / theme override: -1 (absent) leaves the build default. */
   ui_set_theme_black((int)HARNESS_INT("ui_theme_black", -1));
   /* Menu-design check (docs/UI-OVERLAY.md): draw a fixed 480x272 picture
    * behind the in-game menu instead of the game, so a GE dump can be
    * compared pixel for pixel with the design model.  Read now, before the
    * core takes the heap; never set in an identity run. */
   {
      char bd[64];
      if (HARNESS_STR("ui_backdrop", bd, sizeof(bd)))
         ui_backdrop_inject(bd);
   }
   /* Price the Save states page's .shot read (docs/UI-OVERLAY.md 12). */
   ui_shot_probe_arm((int)HARNESS_INT("shot_probe", 0));
   simff      = HARNESS_INT("simff", 0);   /* harness: hold a
                              virtual FF button for N frames from frame 300
                              (PPSSPP can't press Square; run_ui_smoke.sh) */
   /* Harness `ff_preset = 1.5x | 2x | 3x | unlimited | smooth`: the preset a
    * `simff` hold engages, without staging a config.ini (docs/FF-PRESETS.md).
    * Echoed either way, so a misspelt value reads as bad= instead of
    * silently measuring the config's preset (harness-ini key trap). */
   {
      char ffp[16];
      if (HARNESS_STR("ff_preset", ffp, sizeof(ffp)))
      {
         int m = !strcmp(ffp, "1.5x") ? PCFG_FF_1_5X :
                 !strcmp(ffp, "2x") ? PCFG_FF_2X :
                 !strcmp(ffp, "3x") ? PCFG_FF_3X :
                 !strcmp(ffp, "unlimited") ? PCFG_FF_UNLIMITED :
                 !strcmp(ffp, "smooth") ? PCFG_FF_SMOOTH : -1;
         if (m >= 0)
            pcfg_ff_set_mode(m);
         fe_evt("ff_preset %s=%s now=%s", m >= 0 ? "set" : "bad", ffp,
                pcfg_ff_name());
      }
   }
   fe_evt("ff mode=%s", ff_ini ? "uncapped" : "normal");
   if (HARNESS_STR("script", script_name, sizeof(script_name)))
      have_script = 1;
#ifdef GPSP_PERF_RIG
   {
      /* Raw PSP pad replay.  NOT `have_script`: the point is to drive the
       * player's path, chords included, which a script run switches off. */
      char pad_name[64], pad_path[192];
      if (HARNESS_STR("pad_script", pad_name, sizeof(pad_name)))
      {
         snprintf(pad_path, sizeof(pad_path), "%s/%s", g_dir_base, pad_name); /* @lab */ LAB_HOOK(lab_task_file(pad_path, sizeof(pad_path), pad_name);)
         pad_script_load(pad_path);
      }
   }
   g_home_sim = (int)HARNESS_INT("home_sim", 0);
   HARNESS_STR("exit_sim_at", g_exit_sim_at,
              sizeof(g_exit_sim_at));
   g_home_stall_frame =
      (unsigned)HARNESS_INT("home_stall_frame", 0);
   g_home_stall_ms =
      (unsigned)HARNESS_INT("home_stall_ms", 8000);
   if (g_home_sim || g_home_stall_frame)
      fe_evt("home_harness sim=%d stall_frame=%u stall_ms=%u", g_home_sim,
             g_home_stall_frame, g_home_stall_ms);
#endif
   /* Perf-rig key: load the slot-0 savestate shortly after boot, so a run
    * can start INSIDE a scene (mid-battle benchmarks) instead of scripting
    * its way there through a title screen. */
   g_autoload_state = (int)HARNESS_INT("load_state", 0);
#ifdef GPSP_PERF_RIG
   g_perf_rig = have_harness && HARNESS_INT("perf_rig", 0);
#endif
   if (g_perf_rig)
   {
      unsigned from=(unsigned)HARNESS_INT("perf_from",300);
      unsigned to=(unsigned)HARNESS_INT("perf_to",3900);
      unsigned timeout=(unsigned)HARNESS_INT("perf_timeout_s",180);
      char job[64]={0}, fixture[32]={0};
      /* The ceiling used to be 36000 frames -- ten minutes of EMULATED time,
       * which is only about three wall-clock minutes under 3x fast-forward.  A
       * ten-minute soak asked for a window past it and got neither an error nor
       * its window: the clause below silently substitutes the defaults, so the
       * rig measured frames 300-3900 and stopped emitting perf_window lines for
       * the rest of the run.  For a crash hunt those lines ARE the evidence --
       * the last one written before the console stops is how far it got -- so
       * the ceiling now allows a long run rather than quietly truncating it.
       * The reset is kept for a genuinely nonsensical window. */
      if(to<=from || to>600000) { from=300; to=3900; }
      if(!timeout || timeout>3600) timeout=180;
      rig_config(from,to,timeout,net_now_us());
      HARNESS_STR("perf_job",job,sizeof(job));
      HARNESS_STR("perf_fixture",fixture,sizeof(fixture));
      fe_evt("perf_job id=%s fixture=%s from=%u to=%u timeout=%u",job,fixture,from,to,timeout);
      g_audio_oracle = HARNESS_INT("audio_oracle",0) ? 1 : 0;
   }
   /* Point the SMC block/writer probe at a 256-byte page of interest.  The
    * default (0x03007D00) is nowhere near Unbound's storm, which `EVT
    * smc_addr` localised to 0x03006200/0x03006300 in IWRAM — four addresses
    * taking ~1450 writes/s between them.  Aiming the watch here fills in the
    * `blk=` and `wr=` fields: which translated blocks cover the page, and
    * which PCs are doing the writing. */
   {
      unsigned w = (unsigned)HARNESS_INT("smc_watch", 0);
      if (w)
      {
         smc_blk_watch = w;
         fe_evt("smc_watch set=%08x", w);
      }
   }

   /* Wireless keys (harness channel; the UI panel drives the same calls). */
   net_host  = HARNESS_INT("host", 0) != 0;
   net_join  = HARNESS_INT("join", 0) != 0;
   net_probe = (int)HARNESS_INT("net_probe", 0);
   /* ADR-0062e: `core_phase` HAS BEEN IN EVERY HARNESS INI IN THIS PROJECT
    * FOR THREE SESSIONS AND HAS NEVER BEEN READ.  It was only ever a
    * config.ini key (config_psp.c), so `core_phase = 2` in a .gpsp-harness.ini
    * was decorative and `core_phase = 0` did not turn the profiler off --
    * the console kept logging `EVT core_phase lvl=2` either way, which is how
    * this was caught (FULLSPEED-FINDINGS §18).  It is not free: the profiler
    * takes ~344 timer reads per frame ON THE EMULATION THREAD, and at full
    * speed the join's whole margin is about a millisecond. */
   g_pcfg.core_phase = (int)HARNESS_INT("core_phase",
                                           g_pcfg.core_phase);
   /* The Lab turns the FPS chip on so the owner can read a console at a
    * glance; it draws after the frame is presented, so vhash never sees it. */
   g_pcfg.show_fps = (int)HARNESS_INT("show_fps",
                                         g_pcfg.show_fps) ? 1 : 0;
   /* ADR-0019: let the harness pin the session frameskip policy without
    * touching the user's config.ini (0 off / 1 adaptive / 2 auto). */
   g_pcfg.net_frameskip = (int)HARNESS_INT("net_frameskip",
                                              g_pcfg.net_frameskip);
   /* ADR-0024: same, for the EVT writer thread (0 inline / 1 threaded). */
   g_pcfg.log_thread = (int)HARNESS_INT("log_thread",
                                           g_pcfg.log_thread);
   /* ADR-0025: same, for the .sav block writer. */
   g_pcfg.sram_thread = (int)HARNESS_INT("sram_thread",
                                            g_pcfg.sram_thread);
   /* FF durability nudge A/B: 0 keeps detection and its EVT but never
    * yields, i.e. the pre-fix behaviour on the same binary. */
   {
      long nudge = HARNESS_INT("sram_ff_nudge", 1);
      fe_host_sram_ff_nudge_enable(nudge != 0);
      fe_evt("sram_ff_nudge on=%d", nudge != 0);
   }
   /* Same, for session pacing.  ADR-0033 REMAPPED THE VALUES: 0 off,
    * 1 fixed-rate (default), 2 the ADR-0027/0028 adaptive matcher,
    * 3 backpressure (ff_psp.h).  3 was clamped to 1 here and then
    * persisted by pcfg_save (r6-B1). */
   g_pcfg.net_pace_match = (int)HARNESS_INT("net_pace_match",
                                               g_pcfg.net_pace_match);
   if (g_pcfg.net_pace_match < PACE_MODE_OFF ||
       g_pcfg.net_pace_match > PACE_MODE_BACKPRESSURE)
      g_pcfg.net_pace_match = PACE_MODE_FIXED;
   g_pcfg.net_session_fps_x100 =
      pcfg_fps_x100(HARNESS_INI, "net_session_fps",
                    g_pcfg.net_session_fps_x100);
   g_pcfg.net_session_fps_snap =
      (int)HARNESS_INT("net_session_fps_snap",
                          g_pcfg.net_session_fps_snap);
   if (g_pcfg.net_session_fps_snap < 0 || g_pcfg.net_session_fps_snap > 1)
      g_pcfg.net_session_fps_snap = 1;
#ifdef GPSP_PLAYABLE
   /* ADR-0067: these four are config.ini keys, so unlike the harness-only
    * knobs above they have a value even with no harness ini.  Forced, not
    * defaulted, so a stale config.ini on the user's card cannot put a
    * playing console back on 29.97 or turn the profiler back on.
    *
    * `core_phase` is not cosmetic here: with telemetry compiled out its EVT
    * line disappears but its ~344 timer reads per frame do not, and that is
    * ~500 us/frame (FULLSPEED §20.4) of pure measurement on a build that has
    * nothing to measure. */
   /* ADR-0071: the rate comes from the TRADING PROFILE, which the player owns
    * and which persists in config.ini across relaunches.  The values are
    * PCFG_PROFILE_FPS_X100 (config_psp.h): 5700 for the speed profile, which is
    * the default, and 2997 for `profile = 1` (compatibility).
    *
    * Latched HERE, once, before the pacer and the netdrv timeout scaling read
    * it — which is why switching profiles relaunches instead of taking effect
    * live.  Two consoles that disagree about how long a frame is are exactly
    * what gen-3's RFU cannot survive: it counts link timeouts in FRAMES. */
   /* `net_session_fps_force = 1` in config.ini hands this back to the player;
    * see the field's note.  Both consoles must carry the same value. */
   if (!g_pcfg.net_session_fps_force)
      g_pcfg.net_session_fps_x100 = PCFG_PROFILE_FPS_X100(g_pcfg.profile);
   /* Scheduled ticks (rfu_sched.h) remove ADR-0073's reason for 57.00: a host
    * can no longer outrun its client and fill Emerald's receive queue, because
    * neither console starts a frame without the other's tick for it.  The pair
    * runs at the slower console's own rate, so the only clamp left is the real
    * GBA's 59.73.  The compatibility profile and net_session_fps_force keep
    * their meaning.  Same test as rfu_sched_config's, which runs later. */
   if (!g_pcfg.net_session_fps_force && g_pcfg.profile != PCFG_PROFILE_COMPAT &&
       HARNESS_INT("rfu_sched", RFU_SCHED_D_DEFAULT) > 0)
      g_pcfg.net_session_fps_x100 = PACE_NOMINAL_X100;
   g_pcfg.net_session_fps_snap = 0;
   if (!g_perf_rig) g_pcfg.core_phase = 0;
   if (g_pcfg.core_phase < 0 || g_pcfg.core_phase > 3) g_pcfg.core_phase = 0;
   /* `gu_defer = 1` was in the harness ini of ALL 17 full-speed runs behind
    * the 16/34 number, but it is 0 in a stock config.ini.  Forced here so the
    * playable build is the configuration that was actually measured, rather
    * than one knob off it. */
   g_pcfg.gu_defer             = 1;
#endif
   /* ADR-0034: same A/B channel for the blit staging placement. */
   g_pcfg.blit_mode = (int)HARNESS_INT("blit_mode",
                                          g_pcfg.blit_mode);
   if (g_pcfg.blit_mode < 0 || g_pcfg.blit_mode >= VID_BLIT_MODES)
      g_pcfg.blit_mode = VID_BLIT_CACHED;
   /* ADR-0040: and for the deferred GE sync, so the colour test can run the
    * deferred path.  The rig cannot price it but it CAN prove that a list
    * still in flight never reaches the swap or the readback. */
   g_pcfg.gu_defer = (int)HARNESS_INT("gu_defer",
                                         g_pcfg.gu_defer);
   g_pcfg.gu_defer = g_pcfg.gu_defer ? 1 : 0;
   /* Scheduled ticks (rfu_sched.h, docs/RFU-COUPLING.md), the adapter's one
    * protocol since C-4: D = 2 by default; a harness may pick another D, or 0
    * for a diagnostic run with raw queues (it refuses a ticks partner). */
   rfu_sched_config((unsigned)HARNESS_INT("rfu_sched", RFU_SCHED_D_DEFAULT));
   fe_evt("rfu_sched d=%u", rfu_sched_d);
   /* VALIDATION ONLY: corrupt the Nth content-bearing host packet this client
    * receives, to prove the rig's cross-console desync detector fires.  0 in
    * every real run; the value is echoed so a stray one cannot hide. */
   {
      int fc = (int)HARNESS_INT("rfu_fault_corrupt", 0);
      if (fc < 0)
         fc = 0;
      rfu_set_fault_corrupt((unsigned)fc);
      fe_evt("rfu_fault_corrupt n=%d", fc);
      fc = (int)HARNESS_INT("rfu_fault_span", 1);
      if (fc < 1)
         fc = 1;
      rfu_set_fault_span((unsigned)fc);
      fe_evt("rfu_fault_span n=%d", fc);
      fc = (int)HARNESS_INT("rfu_fault_mode", 0);
      rfu_set_fault_mode(fc > 0 ? (unsigned)fc : 0);
      fe_evt("rfu_fault_mode n=%d (1 = SETMONDATA value byte)", fc > 0 ? fc : 0);
   }
   /* Candidate mid-run-Disconnect fix (default OFF): await the DISCONNECTED
    * state before term/unload.  Run the breadcrumb-only build FIRST to confirm
    * the wedge step, then set disc_await=1 to A/B the fix. */
   {
      int da = (int)HARNESS_INT("disc_await", 0);
      adhoc_set_disc_await(da);
      fe_evt("disc_await on=%d", da);
   }
   /* Audio latency target (AUDIO_RING_TARGET): harness override for A/B. */
   {
      int rt = (int)HARNESS_INT("audio_ring_target",
                                   AUDIO_RING_TARGET);
      if (rt >= 1024 && rt < RING_FRAMES)
         g_ring_high_water = (unsigned)rt;
      fe_evt("audio_ring_target frames=%u", g_ring_high_water);
   }
   /* Media Engine mode: pcfg (the settings toggle, applied via relaunch) with
    * a harness override so the rig can A/B it per-arm. */
   {
      int mm = (int)HARNESS_INT("me_mode", g_pcfg.me_mode);
      g_me_rend_cfg = mm ? 1 : 0;
      fe_evt("me_mode on=%d src=%s", g_me_rend_cfg,
             mm == g_pcfg.me_mode ? "config" : "harness");
   }
   /* ADR-0059: stretch the emulated adapter's cycle-counted deadlines.
    *
    * Percent, 100 = stock = what every measurement to date was taken with, so
    * omitting these changes nothing.  -1 = AUTO = session_fps / 29.97, which
    * reproduces the known-good 29.97 wall-clock budget at any session rate
    * (100 at 29.97, 150 at 45.00, 200 at 59.73).
    *
    * `rfu_timeout_scale` moves the CLIENT's budget (rfu_timeout_cycles ->
    * RFU_CMD_RESP_TIMEO); `rfu_rtx_scale` moves the HOST's (rfu_resp_timeout
    * -> the synthetic "clients did not answer").  Separate knobs on purpose:
    * CLIFF-FINDINGS §14.3 records that host- and client-side timeouts were
    * never separated, and they have the same 1/fps shape. */
   {
      int ts  = (int)HARNESS_INT("rfu_timeout_scale", 100);
      int rs  = (int)HARNESS_INT("rfu_rtx_scale",     100);
      int aut = (g_pcfg.net_session_fps_x100 > 0)
                   ? (g_pcfg.net_session_fps_x100 * 100) / 2997 : 100;
      if (aut < 100) aut = 100;
      if (ts < 0) ts = aut;
      if (rs < 0) rs = aut;
      if (ts < 25)   ts = 25;
      if (ts > 1600) ts = 1600;
      if (rs < 25)   rs = 25;
      if (rs > 1600) rs = 1600;
      rfu_set_timeout_scale((unsigned)ts, (unsigned)rs);
      fe_evt("rfu_timeout_scale to=%d rtx=%d auto=%d", ts, rs, aut);
   }
   /* ADR-0062: WHO GETS THE CPU THE EMULATOR IS NOT USING, and how fast a
    * lost slot is recovered.  Both harness-only, both default to today's
    * behaviour, both must be set BEFORE net bring-up (which happens after
    * this function).  See FULLSPEED-FINDINGS §7 for the measurement that
    * motivates the first one: the emulated core costs ~14.5 ms per frame at
    * every session rate, so the idle time the WLAN driver lives in falls from
    * 17.6 ms/frame at 29.97 fps to 2.3 ms at 59.73, and the mean cost of one
    * sceNetAdhocPdpSend rises 60 us -> 1040 us across exactly that range. */
   {
      int np_ = (int)HARNESS_INT("adhoc_net_prio", 0);
      int rf  = (int)HARNESS_INT("nd_rto_first_max_us", 0);
      /* config.ini is the base, harness overrides -- and the base is no
       * longer 0/build-time: see g_pcfg.nd_rto_min_us for why 200 ms is
       * wrong at an 11 ms srtt. */
      int rm  = (int)HARNESS_INT("nd_rto_min_us",
                                    g_pcfg.nd_rto_min_us);
      if (rf < 0) rf = 0;
      if (rm < 0) rm = 0;
      adhoc_transport_set_net_prio(np_);
      fe_np_set_arq_timers((uint32_t)rf, (uint32_t)rm);
      /* 28: ack riding (0 = off): a bare ACK waits this long for a ride */
      {
         /* 4.0.0 beta: a GBA Link Cable session rides while it RUNS
          * (hardware: SF 45.2 -> 47.6 fps with riding alone), Fast link or
          * not: config.ini nd_ack_ride_us is that product value (0 = never).
          * A harness nd_ack_ride_us is a FIXED value instead: every session
          * of every kind, from its creation, 0 included.
          * Clamp: a riding ack leaves at most ride + one pump period after
          * the data it answers, and that must stay under the RTO floor or
          * the sender retransmits what was received (rm == 0 means the
          * transport's built-in floor). */
         int floor_us = rm > 0 ? rm : ND_RTO_FLOOR_DEFAULT_US;
         int ride_max = floor_us - ND_PUMP_PERIOD_US;
         int ar = (int)fe_ini_get_int(p_config, "nd_ack_ride_us",
                                      PLAY_ND_ACK_RIDE_US);
         int hr = (int)HARNESS_INT("nd_ack_ride_us", -1);
         if (ride_max < 0) ride_max = 0;
         if (ar < 0) ar = 0;
         if (ar > ride_max) ar = ride_max;
         fe_np_set_ack_ride_product((uint32_t)ar);
         if (hr >= 0)
         {
            if (hr > ride_max) hr = ride_max;
            fe_np_set_ack_ride((uint32_t)hr);
         }
         fe_evt("net_tuning ack_ride_cfg product_us=%d fixed_us=%d max_us=%d",
                ar, hr, ride_max);
      }
      if (np_ || rf || rm)
         fe_evt("net_tuning adhoc_net_prio=0x%02X rto_first_max_us=%d "
                "rto_min_us=%d", adhoc_transport_net_prio(), rf, rm);
   }
   /* ADR-0062b: THE SAME LEVER FROM THE OTHER END — lower the EMULATION
    * thread instead of raising the WLAN stack.
    *
    * `adhoc_net_prio = 0x1D` was tried first and it BREAKS THE HOST: with the
    * stack above the emulation thread, `sceNetAdhocctlConnect` never reaches
    * CONNECTED and bring-up dies at `stage=ctl_connect_wait rc=-9
    * sce=0x80410002` after the full 30 s wait (measured, run 086; the JOIN
    * came up fine at the same setting, so it is group CREATION that objects).
    *
    * Moving main from 0x20 to something numerically above ADHOC_NET_PRIO
    * (0x2A) produces the same ORDERING without touching sceNetInit at all, so
    * it cannot break bring-up the same way.  Our own RX (0x1E) and TX (0x1F)
    * threads stay above everything, which is what they were placed for.
    *
    * Note this also drops main below the IO thread's default 0x22, so a log or
    * SRAM write then preempts emulation; `io_prio` (ADR-0062c, below) puts
    * that thread back under the emulator.  The release sets both (main 0x2B,
    * io 0x2C: a lower number is a higher priority, so main outranks io).  With
    * emu_prio alone the preemption is visible in `EVT fps emu=` and is part of
    * what that arm measures, not a hidden cost.
    *
    * 0 = leave main at its default 0x20. */
   {
      int ep = (int)HARNESS_INT("emu_prio", PLAY_EMU_PRIO);
      int ip = (int)HARNESS_INT("io_prio",  PLAY_IO_PRIO);
      /* ADR-0064/0065 both need the emulation thread's UID, and both are
       * no-ops unless their key is set.  Taken here because this is the one
       * place that already knows it is running ON that thread. */
      g_emu_thid = sceKernelGetThreadId();
      if (ep)
      {
         if (ep < 0x08) ep = 0x08;
         if (ep > 0x77) ep = 0x77;
         sceKernelChangeThreadPriority(g_emu_thid, ep);
         fe_evt("emu_prio set=0x%02X (was 0x20; adhoc net stack is 0x%02X)",
                ep, adhoc_transport_net_prio());
      }
      /* ADR-0062c: keep OUR housekeeping below the emulator even when the
       * emulator has been put below the radio.  See g_io_prio. */
      if (ip)
      {
         if (ip < 0x08) ip = 0x08;
         if (ip > 0x77) ip = 0x77;
         g_io_prio = ip;
      }
      /* VRAM dirty-page probe: frames between samples, 0 = off (default).
       * See vram_probe_frame().  30 gives ~2 instrumented frames/second. */
      /* ME input shadows: retained only as a diagnostic A/B.  Hardware
       * rejected them: the 98 KiB host copy costs more than waiting for the
       * ME, and it also bypasses the later dirty-page transfer path.  Keep
       * the measured winner as the default; a harness may still set 1. */
      g_mer_shadow = (int)HARNESS_INT("me_shadow", 0);
      g_mer_vmem_cfg = (int)HARNESS_INT("me_vmem", 1) != 0;
      if (!g_mer_vmem_cfg)
         fe_evt("me_vmem 0 (engine buffers from the heap)");
      g_mer_dirty_cfg = (int)HARNESS_INT("me_dirty",
                                             g_pcfg.me_dirty);
      /* Same-frame is retained as a harness-only A/B.  Hardware showed that
       * issuing the GE list immediately before swap costs heavy games about
       * 1.5 ms per frame.  The loop-top pipeline overlaps that work with the
       * CPU, keeps exact output, and also holds full speed in light games. */
      g_mer_sameframe = (int)HARNESS_INT("me_sameframe", 0);
      /* LOW LATENCY (docs/LOW-LATENCY.md): one switch per mechanism. */
      g_flip_early = (int)HARNESS_INT("flip_early",
                                         g_flip_early) != 0;
      g_mer_early_retire = (int)HARNESS_INT("me_early_retire",
                                               g_mer_early_retire) != 0;
      g_dfr_cfg = (int)HARNESS_INT("me_late_present",
                                      g_dfr_cfg) != 0;
      g_ff_flip_pace = (int)HARNESS_INT("ff_flip_pace",
                                           g_ff_flip_pace) & 7;
      /* 0 off, 1 tier 1 (entries on the natural nudge), 2 = 1 + entries
       * on an early nudge (TIER 1 POLICY at g_flip_early) */
      g_ll_cfg = (int)HARNESS_INT("me_lowlat", PLAY_ME_LOWLAT);
      if (g_ll_cfg < 0 || g_ll_cfg > 2)
         g_ll_cfg = 1;
      g_ll_recover = (unsigned)HARNESS_INT("me_lowlat_recover",
                                              (int)g_ll_recover);
      g_ll_margin_us = (unsigned)HARNESS_INT(
                                                "me_lowlat_margin_us",
                                                (int)g_ll_margin_us);
      if (g_ll_cfg)
         g_mer_sameframe = 0;      /* two retire/present owners: never both */
      g_in_fresh = (int)HARNESS_INT("input_fresh",
                                       PLAY_INPUT_FRESH) != 0;
      g_ctrl_cycle = (int)HARNESS_INT("ctrl_cycle", 0);
      if (g_ctrl_cycle >= 5555 && g_ctrl_cycle <= 20000)
      {
         /* Called outside the EVT (r6-B3): fe_evt's arguments are not
          * evaluated where telemetry compiles out, so the call went too. */
         int cc_rc = sceCtrlSetSamplingCycle(g_ctrl_cycle);
         FE_EVT_ONLY(cc_rc);
         fe_evt("ctrl_cycle %d rc=%d", g_ctrl_cycle, cc_rc);
      }
      else
         g_ctrl_cycle = 0;
      fe_evt("input input_fresh=%d ctrl_cycle=%d", g_in_fresh, g_ctrl_cycle);
      fe_evt("latency flip_early=%d me_early_retire=%d me_lowlat=%d "
             "recover=%u margin_us=%u me_sameframe=%d ff_flip_pace=%d "
             "late_present=%d",
             g_flip_early, g_mer_early_retire, g_ll_cfg, g_ll_recover,
             g_ll_margin_us, g_mer_sameframe, g_ff_flip_pace, g_dfr_cfg);
      /* 0 restores the end-of-retro_run post for a one-binary A/B. */
      g_mer_vispost = (int)HARNESS_INT("me_vispost", 1) != 0;
      if (!g_mer_vispost)
         fe_evt("me_vispost 0 (render posted at frame end)");
      g_mer_capmode = HARNESS_INT("me_capmode", 1) == 2
                      ? 2u : 1u;
#if ME_MIDFRAME_LOG
      g_mer_midlog = (int)HARNESS_INT("me_midlog", 1) != 0;
      {
         me_log_vram_always =
            (int)HARNESS_INT("me_vram_always", 1) != 0;
         fe_evt("me_vram_always on=%d", me_log_vram_always);
      }
      if (!g_mer_midlog)
         fe_evt("me_midlog 0 (mid-frame log off)");
#endif
      g_vp_period = (int)HARNESS_INT("vram_probe", 0);
      if (g_vp_period)
         fe_evt("vram_probe period=%d", g_vp_period);

      /* ADR-0066: frames between dump-marker polls.  ADR-0069 default = 0. */
      {
         int dm = (int)HARNESS_INT("dump_marker_poll",
                                      PLAY_DUMP_MARKER);
         if (dm < 0) dm = 0;
         g_dump_marker_every = dm;
         fe_evt("dump_marker_poll n=%d", g_dump_marker_every);
      }
      /* ADR-0069: the deliberate yield.  `sched_yield_us` microseconds of
       * sceKernelDelayThread once per `sched_yield_frames` frames, so the CPU
       * the dump-marker stat used to hand the WLAN threads by accident can be
       * handed to them on purpose, at a size we picked.  Both default off. */
      {
         int yu = (int)HARNESS_INT("sched_yield_us", 0);
         int yf = (int)HARNESS_INT("sched_yield_frames", 60);
         if (yu < 0) yu = 0;
         if (yf < 1) yf = 1;
         g_yield_us    = yu;
         g_yield_every = yf;
         if (yu)
            fe_evt("sched_yield us=%d every=%d", g_yield_us, g_yield_every);
      }
      /* ADR-0064: the preemption census.  Off unless asked for. */
      g_pe_on = (int)HARNESS_INT("preempt_prof", 0) ? 1 : 0;
      if (g_pe_on)
         fe_evt("preempt_prof on thid=0x%08X", (unsigned)g_emu_thid);
      /* ADR-0066 measurement: PRICE the negative ms0: lookup that used to run
       * once a second inside the timed frame region.  SPIKE-FINDINGS §10.6
       * recorded "I did not price sceIoGetstat on this device" as an
       * unestablished claim; four samples at boot cost nothing and settle it.
       * A negative lookup is a full directory resolution, so this is an
       * upper-bound-ish figure for an idle stick — during a session the log
       * writer is also queued on the same device. */
      {
         int k;
         for (k = 0; k < 4; k++)
         {
            uint64_t t0 = net_now_us();
            int hit = file_exists(DUMP_MARKER);
   FE_EVT_ONLY(t0); FE_EVT_ONLY(hit);
            fe_evt("msstat n=%d us=%u hit=%d",
                   k, (unsigned)(net_now_us() - t0), hit);
         }
      }
      /* ADR-0065: the protected window.  `base` is whatever the emulation
       * thread's STANDING priority ends up being, so the window degrades to
       * exactly today's behaviour when it is closed. */
      {
         int bu = (int)HARNESS_INT("emu_prio_boost_us",
                                      PLAY_BOOST_US);
         int bp = (int)HARNESS_INT("emu_prio_boost", 0x29);
         g_boost_base = ep ? ep : 0x20;
         if (bp < 0x08) bp = 0x08;
         if (bp > 0x77) bp = 0x77;
         g_boost_prio = bp;
         if (bu < 0)     bu = 0;
         if (bu > 15000) bu = 15000;   /* must never span a whole frame */
         g_boost_us = bu;
         emu_boost_start();
      }
   }
   /* ADR-0062d: STOP THE CLIENT SPINNING ON THE RADIO.
    *
    * `rfu_update()` calls netpacket_poll_receive() on EVERY invocation while
    * the adapter is in WAITEVENT, and update_serial() invokes it many times
    * per emulated frame.  Measured (FULLSPEED-FINDINGS §14): the JOIN issues
    * **587 polls per frame** and the HOST issues **0**, because only the
    * client sits in WAITEVENT waiting on its peer.
    *
    * While the WLAN stack was starved those polls were nearly free (11-16 us
    * per frame in total).  Once the stack is scheduled (ADR-0062b) each poll
    * can actually do something and can be preempted inside the driver, and
    * the same 587 polls cost the join **855 us per frame** -- 5 % of a
    * 59.73 fps frame budget spent asking a radio 587 times whether one packet
    * has arrived.
    *
    * This sets a MINIMUM number of emulated cycles between polls.  The
    * original comment on that call ("otherwise we need to wait a full
    * frame!") is the thing to preserve: a limit of one poll per 1/8 frame
    * still checks the radio eight times per frame, which is eight times
    * better than the latency the comment was worried about, at 1/70th of the
    * calls.  0 = poll every time = the historical behaviour. */
   {
      int pm = (int)HARNESS_INT("rfu_poll_min_cycles",
                                   PLAY_POLL_MIN_CYCLES);
      if (pm < 0) pm = 0;
      rfu_set_poll_min_cycles((unsigned)pm);
      if (pm)
         fe_evt("rfu_poll_min_cycles n=%d (=1/%d emulated frame)",
                pm, pm ? (16777216 / 60) / pm : 0);
   }
   /* ADR-0068: the same limit applied to the OTHER side of the asymmetry.
    *
    * The knob above throttles a client that polls 587 times a frame.  This one
    * un-throttles a host that polls ZERO times a frame: the host is in
    * WAITEVENT 0.6 % of the time, so the branch above is never its branch, and
    * the single fe_np_pump() call site is the only place it reads the network
    * all frame.  The host TRANSMITS immediately (netdrv txq_push, nothing
    * Nagled) — it just does not look, so up to a whole host frame of "your
    * command is still unread" lands inside the client's WAITEVENT dwell.
    *
    * Default 0 = never = byte-for-byte the historical behaviour, so this is
    * inert until a run asks for it.  34952 (1/8 frame) matches the client
    * knob's cadence and is the arm to try first. */
   {
      int dd = (int)HARNESS_INT("rfu_disc_defer", 0);
      rfu_set_disc_defer(dd ? 1u : 0u);
      if (dd)
         fe_evt("rfu_disc_defer on");
      /* ADR-0079: the exit-echo state assist.  Default PLAY_EXIT_ASSIST — 1 in
       * the playable build (nonexistent HARNESS_INI -> default), 0 for harness.
       * Was hardcoded 0, which silently left the PLAYABLE build without the
       * exit fix despite PLAY_EXIT_ASSIST=1 (the message-17 exit crash). */
      g_exit_assist =
         (int)HARNESS_INT("rfu_exit_assist", PLAY_EXIT_ASSIST) ? 1 : 0;
      fe_evt("rfu_exit_assist on=%d", g_exit_assist);
      /* ADR-0078: net_pace_match was a config.ini-only key, so ARM B's
       * harness ini silently ran mode 1 (run 315's lesson — the arm's own
       * log said `mode=1 policy=fixed` while the ini said 3).  Same
       * harness-overridable pattern as net_session_fps. */
      int pmode = (int)HARNESS_INT("net_pace_match",
                                      g_pcfg.net_pace_match);
      if (pmode >= 0 && pmode <= 3 && pmode != g_pcfg.net_pace_match)
      {
         fe_evt("net_pace_match harness override %d -> %d",
                g_pcfg.net_pace_match, pmode);
         g_pcfg.net_pace_match = pmode;
      }
      /* ADR-0076: the exit-race grace window, in emulated frames. */
      int dg = (int)HARNESS_INT("rfu_disc_grace",
                                   PLAY_RFU_DISC_GRACE);
      if (dg < 0)   dg = 0;
      if (dg > 600) dg = 600;
      rfu_set_disc_grace((unsigned)dg);
      fe_evt("rfu_disc_grace frames=%d", dg);
      int ip = (int)HARNESS_INT("rfu_idle_poll_cycles",
                                   PLAY_IDLE_POLL);
      if (ip < 0) ip = 0;
      rfu_set_idle_poll_cycles((unsigned)ip);
      if (ip)
         fe_evt("rfu_idle_poll_cycles n=%d (=1/%d emulated frame)",
                ip, ip ? (16777216 / 60) / ip : 0);
   }
   /* ADR-0027 harness knob: burn N us of BUSY work per frame on this
    * instance only.  The rig runs both instances at full speed, so it cannot
    * reproduce the field's role asymmetry on its own and pace matching would
    * never engage there.  This manufactures a genuinely slower peer — busy,
    * not sleeping, so it lands in the same per-frame WORK time the
    * capability estimate reads and the whole loop is exercised end to end.
    * Harness only; never set in config.ini. */
   ff_pace_set_slow_us((int)HARNESS_INT("pace_slow_us", 0));
#ifdef GPSP_PERF_RIG
   g_gbl_cpu_x100 = (unsigned)HARNESS_INT("gblink_cpu_x100", 0);
   if (g_gbl_cpu_x100)
      fe_evt("gblink_cpu_x100=%u (harness: PPSSPP slow-CPU emulation)",
             g_gbl_cpu_x100);
#endif
   if (ff_pace_slow_us())
      fe_evt("pace_slow_us=%d", ff_pace_slow_us());
   if (!HARNESS_STR("nick", nick, sizeof(nick)))
      snprintf(nick, sizeof(nick), "%s", g_pcfg.nick);
   if (!HARNESS_STR("group", group, sizeof(group)))
      snprintf(group, sizeof(group), "%s", g_pcfg.group);

   /* Radio emulation for the PPSSPP rig (harness only — the desktop UDP
    * backend has the same knobs).  PPSSPP's AdhocServer loopback RTT is
    * microseconds; real PSP ad-hoc is tens of ms, which is the whole
    * reason the ARQ storm of docs/HANDOFF.md issue #2 escaped the harness.
    * Each side adds net_latency_ms to the one-way path -> RTT = 2x. */
#ifdef GBADHOC_ONLINE
   g_online_boot = online_psp_configure(HARNESS_INI, p_config);   /* EXPERIMENTAL */
#endif
   {
      int nl = (int)HARNESS_INT("net_latency_ms", 0);
      int nj = (int)HARNESS_INT("net_jitter_ms", 0);
      int np_ = (int)HARNESS_INT("net_loss_pct", 0);
      if (nl || nj || np_)
      {
         adhoc_transport_set_fault(nl, nj, np_,
                                   0x243F6A88u ^ (uint32_t)(net_host ? 1 : 2));
         fe_evt("net_fault latency_ms=%d jitter_ms=%d loss_pct=%d", nl, nj, np_);
      }
   }
   {
      int bo = (int)HARNESS_INT("net_blackout_ms", 0);
      int be = (int)HARNESS_INT("net_blackout_every_ms", 20000);
      if (bo > 0)
      {
         adhoc_transport_set_blackout(bo, be);
         fe_evt("net_blackout ms=%d every_ms=%d", bo, be);
      }
   }

   /* Causation test for the exit-Union-Room fatal screen (phase5j).  See
    * netpacket_host.h: swallowing the parent's RFU DISCONNECT notice pins
    * the child's emulated adapter in RFU_STATE_CLIENT, which is the state
    * the field logs imply and which rfu.c has no other way out of. */
   {
      int dd = (int)HARNESS_INT("rfu_drop_disconnect", 0);
      if (dd)
         fe_np_debug_drop_rfu_disconnect(dd);
   }

#if defined(GPSP_PERF_RIG) && \
    defined(GPSP_KEEP_TELEMETRY)
   /* FREEZE RIG RECOVERY BOOT (harness `freeze_rig_recover = 1`; from
    * claude/freeze-rig 2d5b84d).  The PC collects and removes
    * log/frontend.log after every run that reaches the handoff, so finding
    * one here means the previous run never got there: it hard-froze (or
    * lost power) and a human power-cycled and relaunched us.  Hand off at
    * once, without loading the ROM, so the PC collects frontend.prev.log,
    * shash.prev.txt, heartbeat.txt and stall.txt as the frozen run's
    * evidence before anything can overwrite them (freezerig.py: exit=7
    * reason=prev_run_uncollected is a FROZE verdict). */
   if (have_harness && g_rig_prev_uncollected &&
       HARNESS_INT("freeze_rig_recover", 0))
   {
      fe_evt("freeze_rig_recover prev_log=uncollected -- the previous run "
             "never reached its handoff; handing off without running");
      exit_early_begin(0);
      fe_evt("exit code=7 reason=prev_run_uncollected");
      exit_early_end(0, 7, "prev_run_uncollected", 0);
      return 0;
   }
#endif

   /* testpat = 1: GU color-order test pattern — no core, no ROM. */
   if (HARNESS_INT("testpat", 0))
   {
      run_testpat();
      exit_early_begin(0);
      fe_evt("exit code=0");
      exit_early_end(0, 0, NULL, 0);
      return 0;
   }

#ifdef GPSP_PERF_RIG
   /* icache_inval_probe = 1: does sceKernelIcacheInvalidateRange invalidate
    * at and above 16 KiB?  (psp/icprobe.c, docs/JIT-COHERENCY.md) -- no core,
    * no ROM. */
   if (HARNESS_INT("icache_inval_probe", 0))
   {
      int rc = icinv_probe_run();
      exit_early_begin(0);
      if (rc == 0)
         fe_evt("exit code=0 reason=icache_inval_probe");
      else
         fe_evt("exit code=5 reason=icinv_fail");
      exit_early_end(0, rc ? 5 : 0, rc ? "icinv_fail" : "icache_inval_probe", 0);
      return 0;
   }
   /* icache_probe = 1: the I-cache geometry, measured (psp/icprobe.c,
    * docs/CACHE-MAP.md) -- no core, no ROM. */
   if (HARNESS_INT("icache_probe", 0))
   {
      int rc = icprobe_run();
      exit_early_begin(0);
      if (rc == 0)
         fe_evt("exit code=0 reason=icache_probe");
      else
         fe_evt("exit code=5 reason=icprobe_fail");
      exit_early_end(0, rc ? 5 : 0, rc ? "icprobe_fail" : "icache_probe", 0);
      return 0;
   }
#endif

   /* nettest = 1: Gate-4E transport echo — no core, no ROM. */
   if (HARNESS_INT("nettest", 0))
   {
      long secs = HARNESS_INT("nettest_secs", 90);
      int rc = run_nettest(net_host, group, secs);
      exit_early_begin(0);
      if (rc == 0)
         fe_evt("exit code=0");
      else
         fe_evt("exit code=5 reason=nettest_fail");
      /* A pass hands off 0, as it logs (F7: it handed off 5 either way). */
      exit_early_end(XE_HO_PASS, 5, "nettest_fail", rc == 0);
      return 0;
   }

   /* Silent-wireless keys (variant.ini wins; harness silent=1 lets the
    * harness exercise the policy on the generic build). */
   {
      const char *src = have_variant ? VARIANT_INI : HARNESS_INI;
      long sil = fe_ini_get_int(src, have_variant ? "silent_wireless"
                                                  : "silent", 0);
      char role[8] = "";
      fe_ini_get(src, "role", role, sizeof(role));
      if (sil)
         g_silent_mode = (strcmp(role, "host") == 0) ? SIL_HOST :
                         (strcmp(role, "join") == 0) ? SIL_JOIN : SIL_AUTO;
      if (!fe_ini_get(src, "group", g_silent_group, sizeof(g_silent_group)))
         snprintf(g_silent_group, sizeof(g_silent_group), "%s",
                  group[0] ? group : g_pcfg.group);
      snprintf(g_silent_nick, sizeof(g_silent_nick), "%s", nick);
   }

   /* Harness: the in-process return driven without hands.  After
    * `inproc_frames` frames of each game the loop quits to the game list,
    * `inproc_cycles` times; game k is `inproc_roms` entry k (a|b|...|f,
    * relative to roms/, cycling), each booted as `rom =` would boot it.
    * docs/INPROC-RETURN.md has the census this exists for. */
   inproc_cycles = HARNESS_INT("inproc_cycles", 0);
   /* Harness `state_save_at = N`: save slot 1 at frame N of every game (the
    * in-process return's census covers a state save; fixtures for GB). */
   state_save_at = HARNESS_INT("state_save_at", 0);
   /* `inproc_via_menu = 1`: each quit is the menu's own Quit to game list,
    * driven by the UI demo engine, instead of a direct one. */
   inproc_via_menu = (int)HARNESS_INT("inproc_via_menu", 0);
   ui_return_dumps((int)HARNESS_INT("ret_dumps", 0));
   /* The fallbacks, without a radio or a fault: `ret_fade_in = 1` draws the
    * relaunched browser's fade from black; `inproc_fake_net = 1` marks this
    * process as having loaded a network module (inproc_refusal). */
   if (HARNESS_INT("ret_fade_in", 0))
      ui_return_fade_in();
   if (HARNESS_INT("inproc_fake_net", 0))
      gbadhoc_net_touched = 1;
   inproc_frames = HARNESS_INT("inproc_frames", 240);
   {
      char list[6 * 64 + 6];
      memset(inproc_roms, 0, sizeof(inproc_roms));
      if (HARNESS_STR("inproc_roms", list, sizeof(list)))
      {
         char *p = list;
         while (*p && inproc_rom_n < 6)
         {
            char *bar = strchr(p, '|');
            if (bar)
               *bar = '\0';
            snprintf(inproc_roms[inproc_rom_n++], sizeof(inproc_roms[0]),
                     "%.63s", p);
            if (!bar)
               break;
            p = bar + 1;
         }
      }
      if (inproc_cycles)
         fe_evt("inproc_harness cycles=%ld frames=%ld roms=%d",
                inproc_cycles, inproc_frames, inproc_rom_n);
   }
   /* What every game starts from: the values below are per-game state the
    * session edits (a GB game turns the renderer and the SRAM thread off,
    * the autoload fires once), restored before each in-process return. */
   me_rend_cfg_boot = g_me_rend_cfg;
   sram_thread_boot = g_pcfg.sram_thread;
   autoload_boot    = g_autoload_state;
   /* The GB core's one process-lifetime allocation, taken now rather than by
    * the first GB game, so no game's unload has anything to give back that
    * a fresh boot would not also hold (docs/INPROC-RETURN.md). */
   gbcore_prepare();

   /* What the session reads from here on. */
   memcpy(bc->script_name, script_name, sizeof(bc->script_name));
   memcpy(bc->nick, nick, sizeof(bc->nick));
   memcpy(bc->group, group, sizeof(bc->group));
   bc->autoexit = autoexit;
   bc->dump_at = dump_at;
   bc->dump_every = dump_every;
   bc->gedump_at = gedump_at;
   bc->gedump_every = gedump_every;
   bc->ff_ini = ff_ini;
   bc->ui_demo = ui_demo;
   bc->simff = simff;
   bc->ui_ctl_demo = ui_ctl_demo;
   bc->wake_shot = wake_shot;
   bc->player_shot_at = player_shot_at;
   bc->player_shot_n = player_shot_n;
   bc->vhash_from = vhash_from;
   bc->vhash_to = vhash_to;
   bc->vid_prof_win = vid_prof_win;
   bc->have_script = have_script;
   bc->have_harness = have_harness;
   bc->have_variant = have_variant;
   bc->net_host = net_host;
   bc->net_join = net_join;
   bc->net_probe = net_probe;
   bc->folders = folders;
   bc->me_rend_cfg_boot = me_rend_cfg_boot;
   bc->sram_thread_boot = sram_thread_boot;
   bc->autoload_boot = autoload_boot;
   bc->inproc_cycles = inproc_cycles;
   bc->inproc_frames = inproc_frames;
   bc->state_save_at = state_save_at;
   bc->inproc_via_menu = inproc_via_menu;
   memcpy(bc->inproc_roms, inproc_roms, sizeof(bc->inproc_roms));
   bc->inproc_rom_n = inproc_rom_n;
   return -1;
}

