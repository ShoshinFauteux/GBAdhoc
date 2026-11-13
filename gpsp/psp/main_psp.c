/* main_psp.c — native PSP frontend for the gpsp libretro core.
 *
 * This file is the shell of one translation unit: the module info, the
 * includes, the layout pads and main().  The rest of the frontend is in the
 * psp/fe/<module>.<n>.inc.c fragments it includes below, once each, in
 * source order; psp/fe/README.md maps each fragment to its module and gives
 * the conventions.  Edit the fragment that holds the code (this file for
 * main()); a patch made against the old single-file main_psp.c goes in
 * with tools/refactor/split.py replay, never by hand.
 */
#include <pspkernel.h>
#include <systemctrl.h>   /* ADR-0071: sctrlKernelLoadExecVSHMs2 (profile restart) */
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspsuspend.h>   /* sceKernelVolatileMemTryLock — engine buffers */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>   /* ADR-0080: memalign for the ME double/stage buffers */
#include <math.h>     /* cosf/sinf -- the rewind HUD */
#include <unistd.h>   /* sbrk -- heap_budget(), every build (heap_snap) */
#include <string.h>

#include "fe_host.h"
#include "../rfu_sched_api.h"   /* RFU_SCHED builds only */
#include "fe_linkprof.h"
#include "fe_evt.h"
#include <psprtc.h>
#include <time.h>
#include "fe_util.h"
#include "fe_autopilot.h"
#include "fe_gblink.h"
#ifdef GBA_LINK
#include "fe_gbadual.h"   /* fe_gbadual_probe: heartbeat / stall record */
#endif
#ifdef GPSP_CHEATS
#include "fe_cheats.h"
#endif
#include "netpacket_host.h"
#include "transport_adhoc.h"
#include "netdrv.h"        /* netdrv_sizeof / netdrv_set_arena -- the boot reserve */
#include "../gpsp_profile.h"   /* named build profiles + illegal-flag rejection */
#include "ff_psp.h"
#include "ff_gate.h"
#include "video_psp.h"
#include "osd_psp.h"
#include "ui_psp.h"
#include "share_psp.h"     /* harness keys only; the feature is ui_share.inc */
#include "room_label.h"
#include "link_games.h"
#include "rom_paths.h"
#include "state_slots.h"
#include "shot_name.h"      /* screenshot file names (issue #13) */
#include "fe_png.h"
#include "mgift_net.h"
#include "online_psp.h"      /* EXPERIMENTAL online play (ONLINE=1 builds) */
#ifdef GBADHOC_ONLINE
#include "libretro.h"        /* retro_get_system_info: the build the room matches */
#include "../netdrv/relay_proto.h"
/* 1 = the harness turned online on for every session (online_relay); else a
 * session is online only when Wireless > Online asked for it. */
static int g_online_boot;
/* writable on purpose: .rodata growth costs 8 KiB of boot heap (online_psp.c) */
static char s_online_fmt[] = "Online: %s";
static const char *online_build_id(void)
{
   struct retro_system_info si;
   memset(&si, 0, sizeof(si));
   retro_get_system_info(&si);
   return si.library_version;
}
#endif
#include "mgift_cart.h"
#include "me_host.h"
#include "home_host.h"
#include "sleep_q.h"
#include "me/me_mbox.h"   /* me_render_desc, ME_UNCACHED — for the render bench */
#include "config_psp.h"
#include "video_prof.h"   /* ADR-0034 renderer path profile (core-side) */
#include "video.h"        /* me_capture_frame + capture-mode globals (the
                           * u16/u32/s32 it needs come from psptypes.h) */

PSP_MODULE_INFO("gpsp-adhoc", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
/* Large-memory mode is requested via MEMSIZE=1 in PARAM.SFO — a Makefile
 * concern (PSP_LARGE_MEMORY, playable only), not a module-info macro.  On a
 * 2000+ the heap below then spans the 64 MiB layout and the ROM cache holds
 * 32 MB carts fully resident. */
/* Leave 1 MiB of heap free for thread stacks etc.: the core greedily mallocs
 * ROM blocks at retro_init until malloc fails (FRONTEND-AUDIT §8 gotcha). */
PSP_HEAP_SIZE_KB(-1024);
#include "lab_harness.h"
#include "lab_ffpace.h"     /* Lab only: FF presentation cadence (LAB_FFP) */
/* All paths are derived from the EBOOT's own directory at runtime (argv[0])
 * so per-game variant installs (docs/VARIANTS.md) work from any folder;
 * falls back to the canonical install dir. */
#include "usb_handoff.h"                                  /* ADR-0053 */
#include "icprobe.h"
#include <kubridge.h>        /* kuKernelGetModel: the rig's psp_model line */
#include "perf_rig.h"
#include "sidecar_dirs.h"   /* saves/ and states/ (4.6.0-beta.4) */
#include "fe/fe_priv.h"      /* this file's macros + PLAY_* defaults (P5) */

/* LAYOUT_PAD_BYTES (psp/Makefile; docs/LAYOUT-PINNING.md): the layout
 * STABILITY PROBE, off unless a build asks for it.  An N-byte function that
 * nothing calls and that is never executed -- the stand-in for "an unrelated
 * change elsewhere in the code".  It sits in main_psp.o, the first object
 * linked, so without LAYOUT_PIN it moves every function after it (and, through
 * the heap, the translation cache); with LAYOUT_PIN the pinned hot path must
 * not move by a byte.  The hardware bench's "pad" arm is built with it. */
#define LAYOUT_PAD_STR2(x) #x   /* both pads use it: text 0 with .rodata > 0 must build */
#define LAYOUT_PAD_STR(x) LAYOUT_PAD_STR2(x)
#if defined(LAYOUT_PAD_BYTES) && LAYOUT_PAD_BYTES > 0
__asm__(".pushsection .text.layout_pad_cold,\"ax\",@progbits\n"
        ".globl layout_pad_cold\n"
        ".type layout_pad_cold, @function\n"
        "layout_pad_cold:\n"
        "  jr $ra\n"
        "  nop\n"
        "  .fill (" LAYOUT_PAD_STR(LAYOUT_PAD_BYTES) " - 8) / 4, 4, 0\n"
        ".size layout_pad_cold, . - layout_pad_cold\n"
        ".popsection\n");
#endif
/* LAYOUT_PAD_RO_BYTES: the same for .rodata, whose frontend part is linked
 * before instance A's: A's tables stay in their cache sets when the frontend's
 * strings change size (docs/LAYOUT-PINNING.md). */
#if defined(LAYOUT_PAD_RO_BYTES) && LAYOUT_PAD_RO_BYTES > 0
__asm__(".pushsection .rodata.layout_pad_ro,\"a\",@progbits\n"
        "  .fill " LAYOUT_PAD_STR(LAYOUT_PAD_RO_BYTES) ", 1, 0\n"
        ".popsection\n");
#endif

#include "fe/paths.inc.c"
#include "fe/harness.a.inc.c"
#include "fe/power.a.inc.c"
#include "fe/iothread.inc.c"
#include "fe/audio.a.inc.c"
#include "fe/input.a.inc.c"
#include "fe/inproc.a.inc.c"
#include "fe/input.b.inc.c"
#include "fe/merend.a.inc.c"
#include "fe/display.a.inc.c"
#include "fe/power.b.inc.c"
#include "fe/harness.b.inc.c"
#include "fe/merend.b.inc.c"
#include "fe/pacing.a.inc.c"
#include "fe/merend.c.inc.c"
#include "fe/display.b.inc.c"
#include "fe/power.c.inc.c"
#include "fe/display.c.inc.c"
#include "fe/power.d.inc.c"
#include "fe/merend.d.inc.c"
#include "fe/misc.a.inc.c"
#include "fe/harness.c.inc.c"
#include "fe/misc.b.inc.c"
#include "fe/link.a.inc.c"
#include "fe/pacing.b.inc.c"
#include "fe/link.b.inc.c"
#include "fe/boost.inc.c"
#include "fe/harness.d.inc.c"
#include "fe/link.c.inc.c"
#include "fe/inproc.b.inc.c"
#include "fe/link.d.inc.c"
#include "fe/audio.b.inc.c"
#include "fe/link.e.inc.c"
#include "fe/mgift.inc.c"
#include "fe/harness.e.inc.c"
#include "fe/misc.c.inc.c"
#include "fe/harness.f.inc.c"
#include "fe/rewind.inc.c"
#include "fe/pacing.c.inc.c"
#include "fe/power.e.inc.c"
#include "fe/pacing.d.inc.c"
#include "fe/teardown.inc.c"
#include "fe/boot.inc.c"
#include "fe/session.inc.c"
/* @extract:begin fn:main */
int main(int argc, char *argv[])
{
   char rom_path[PSP_FILE_PATH_CAP];
   char save_path[PSP_FILE_PATH_CAP], state_path[PSP_FILE_PATH_CAP];
   char script_name[64], script_path[192];
   char nick[24], group[16] = "";
   long autoexit, dump_at, dump_every, gedump_at, gedump_every, ff_ini, ui_demo, simff;
   long ui_ctl_demo = 0;        /* harness: Settings > Controls self-drive */
   long wake_shot = 0;          /* harness: open the wake overlay at frame N */
   /* harness `player_shot_at = N` (`player_shot_n = K`, default 1): take K
    * player screenshots, one a frame, from frame N -- the Screenshot
    * shortcut's own path, which a script cannot press (issue #13). */
   long player_shot_at = 0, player_shot_n = 1;
   long vhash_from, vhash_to;   /* ADR-0033(coreopt) video regression oracle */
   long vid_prof_win = 0;       /* ADR-0034(coreopt) renderer path profile */
   int have_script = 0, have_harness, have_variant;
   int autoload_state_slot = 1;
   int net_host = 0, net_join = 0, net_probe = 0;
   int exit_code = 0;
   const char *exit_reason = NULL;
   int ff_engaged = 0, ff_toggled = 0, ff_last_mode = -1, ff_last_me = -1;
   unsigned prev_pad = 0;
   int chord_frames = 0;
   fe_host_config cfg;
   /* The in-process return to the game list (inproc_refusal): how many
    * times this process has done it, the per-game values a fresh boot
    * starts every game from, and the heap as it was when this game's ROM
    * was picked. */
   int inproc_n = 0;
   int launch_now = 0;   /* this session is an XMB launch (g_launch_rom) */
   int folders = 0;      /* FOLDERS (folders_settle): saves/ and states/ */
   int me_rend_cfg_boot = 0, sram_thread_boot = 0, autoload_boot = 0;
   long inproc_cycles = 0, inproc_frames = 0, state_save_at = 0;
   int inproc_via_menu = 0, inproc_menu_armed = 0;
   char inproc_roms[6][64];
   int inproc_rom_n = 0;
   heap_snap_t snap_pre;

   {
      boot_ctx bc;
      if (boot_run(argc, argv, &bc) >= 0)
         return 0;
      memcpy(script_name, bc.script_name, sizeof(script_name));
      memcpy(nick, bc.nick, sizeof(nick));
      memcpy(group, bc.group, sizeof(group));
      autoexit = bc.autoexit;
      dump_at = bc.dump_at;
      dump_every = bc.dump_every;
      gedump_at = bc.gedump_at;
      gedump_every = bc.gedump_every;
      ff_ini = bc.ff_ini;
      ui_demo = bc.ui_demo;
      simff = bc.simff;
      ui_ctl_demo = bc.ui_ctl_demo;
      wake_shot = bc.wake_shot;
      player_shot_at = bc.player_shot_at;
      player_shot_n = bc.player_shot_n;
      vhash_from = bc.vhash_from;
      vhash_to = bc.vhash_to;
      vid_prof_win = bc.vid_prof_win;
      have_script = bc.have_script;
      have_harness = bc.have_harness;
      have_variant = bc.have_variant;
      net_host = bc.net_host;
      net_join = bc.net_join;
      net_probe = bc.net_probe;
      folders = bc.folders;
      me_rend_cfg_boot = bc.me_rend_cfg_boot;
      sram_thread_boot = bc.sram_thread_boot;
      autoload_boot = bc.autoload_boot;
      inproc_cycles = bc.inproc_cycles;
      inproc_frames = bc.inproc_frames;
      state_save_at = bc.state_save_at;
      inproc_via_menu = bc.inproc_via_menu;
      memcpy(inproc_roms, bc.inproc_roms, sizeof(inproc_roms));
      inproc_rom_n = bc.inproc_rom_n;
   }

session_top:
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
         ? snprintf(rom_path, sizeof(rom_path), "%s", g_launch_rom) >=
              (int)sizeof(rom_path)
         : psp_rom_path_join(rom_path, sizeof(rom_path), ROM_DIR,
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
      if (psp_rom_path_join(rom_path, sizeof(rom_path), g_dir_base, vrom) != 0)
      {
         fe_evt("exit code=2 reason=rom_path_too_long rom=%s", vrom);
         exit_early_end(XE_EVT_FIRST, 2, "rom_path_too_long", 0);
         return 0;
      }
      snprintf(savedir, sizeof(savedir), "%s/saves", g_dir_base);
      sceIoMkdir(savedir, 0777);
      slash = strrchr(vrom, '/');
      snprintf(stem, sizeof(stem), "%s", slash ? slash + 1 : vrom);
      snprintf(save_path, sizeof(save_path), "%s/saves/%s", g_dir_base, stem);
      if (make_suffixed_path(save_path, ".sav", save_path,
                             sizeof(save_path)) != 0)
      {
         fe_evt("exit code=2 reason=sidecar_path_too_long rom=%s", vrom);
         exit_early_end(XE_EVT_FIRST, 2, "sidecar_path_too_long", 0);
         return 0;
      }
      snprintf(state_path, sizeof(state_path), "%s/saves/%s", g_dir_base,
               stem);
      if (make_suffixed_path(state_path, ".st0", state_path,
                             sizeof(state_path)) != 0)
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
         if (psp_rom_path_join(rom_path, sizeof(rom_path), ROM_DIR, kr) != 0)
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
      else if (find_first_rom(rom_path, sizeof(rom_path)) != 0)
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
      br = ui_browser(ROM_DIR, rom_path, sizeof(rom_path),
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
         autoload_state_slot = browser_state_slot;
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
                             sizeof(save_path)) != 0 ||
          make_suffixed_path(rom_path,
                             psp_state_slot1_suffix(
                                (fe_console_t)g_pcfg.console),
                             state_path, sizeof(state_path)) != 0)
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
      /* @lab */ LAB_HOOK(if (!have_variant) lab_sidecars(save_path, sizeof(save_path), state_path, sizeof(state_path));)
      if (!have_variant)
         folders_settle(folders, rom_path,
                        g_pcfg.console == FE_CONSOLE_GBA ? ".sav" :
                        g_pcfg.console == FE_CONSOLE_GBC ? ".gbc.sav"
                                                         : ".gb.sav",
                        (fe_console_t)g_pcfg.console, save_path,
                        sizeof(save_path), state_path, sizeof(state_path),
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

   memset(&cfg, 0, sizeof(cfg));
   cfg.console      = (fe_console_t)g_pcfg.console;
   cfg.rom_path      = rom_path;
   cfg.system_dir    = g_dir_base;
   cfg.save_path     = save_path;
   cfg.video_frame   = plat_video_frame;
   cfg.audio_frames  = plat_audio_frames;
   cfg.input_bitmask = plat_input_bitmask;
   cfg.time_us       = net_now_us;   /* heartbeat t_us (hw perf baseline) */
   cfg.yield_thread  = gb_serial_yield;
   /* GB/GBC cartridge RTCs (MBC3: Gold/Silver/Crystal).  time() on real PSP
    * firmware is roughly uptime, so without this the clock resets every boot
    * -- the same trap gpsp_wallclock fixes for the GBA RTC below. */
   cfg.wallclock     = psp_local_wallclock;
   /* A GB/GBC cartridge clock counts seconds; the only date in it is the
    * stamp in the save, which the format defines as UTC.  So that clock runs
    * on UTC, and the saves 3.5.0 and earlier stamped with local time are
    * recognised by when this install first ran a release that does
    * (fe_host.c gb_rtc_from_legacy).  The GBA keeps local time: its RTC is
    * a calendar. */
   if (g_pcfg.console != FE_CONSOLE_GBA)
   {
      cfg.wallclock = psp_utc_wallclock;
      cfg.wallclock_legacy = psp_local_wallclock;
      cfg.gb_rtc_legacy_before =
         (long long)pcfg_gb_rtc_utc_since((long)psp_local_wallclock());
      fe_evt("gb_rtc utc=%lld local=%lld legacy_before=%lld",
             (long long)psp_utc_wallclock(), (long long)psp_local_wallclock(),
             cfg.gb_rtc_legacy_before);
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
      cfg.wallclock = gblink_seed_wallclock;
      cfg.wallclock_legacy = NULL;   /* one frozen clock: nothing to move */
      fe_evt("gblink_wallclock frozen=%lld (harness gblink_rtc_seed)",
             g_gblink_seed);
   }
   cfg.gb_palette    = g_pcfg.gb_palette;   /* GB/GBC DMG palette */
   cfg.gbc_raw_colors = !g_pcfg.gbc_color_correction;
   cfg.gb_find_rom   = psp_gb_find_rom;     /* GB link: partner cartridge */
   cfg.gblink_batch  = g_gblink_batch;
   cfg.gblink_batch_armed = g_gblink_batch_armed;
   cfg.gblink_live   = g_gblink_live;
   cfg.gblink_input_copies = g_gblink_copies;
   cfg.gblink_resume_boot = g_gblink_resume_boot;
   cfg.gblink_pace_slot0 = g_gblink_pace0;
   cfg.heartbeat_frames = g_telemetry_period;
   cfg.gblink_rtc_keep_session_stamp = g_gblink_keep_stamp;
   cfg.gblink_wait   = plat_gblink_wait;    /* stall absorption */
   cfg.gblink_nack   = g_gbl_nack;          /* resend requests */
   cfg.gblink_delay_auto = g_gblink_delay_auto; /* 0 = off */
   cfg.gblink_hash_every  = g_gbl_hash_every;   /* harness A/B */
   cfg.gblink_hash_legacy = g_gbl_hash_legacy;
   if (g_gblink_bulk_bytes)
   {
      cfg.gb_bulk_enable = psp_gb_bulk_enable;
      cfg.gb_bulk_send   = psp_gb_bulk_send;
      cfg.gb_bulk_recv   = psp_gb_bulk_recv;
      cfg.gb_bulk_max    = g_gblink_bulk_bytes;
      cfg.gb_bulk_rate   = g_gblink_bulk_rate;
   }
   /* host=1 / join=1 on a GB/GBC cartridge is a link-session run. */
   if (g_pcfg.console != FE_CONSOLE_GBA && !g_gblink_role && (net_host || net_join))
      g_gblink_role = net_host ? 1 : 2;
   /* harness link_script_waits = 1: a menu-started session's rig holds the
    * autopilot until the link runs, as gblink_role does (docs/
    * GBA-LINK-UI.md); a release build reads no harness key. */
   cfg.gblink_script_waits = g_gblink_role != 0 ||
      fe_ini_get_int(HARNESS_INI, "link_script_waits", 0) != 0;
   /* LINK SPEED defaults (PLAY_* above), every build: config.ini is the
    * kill switch; the harness block below overrides for an A/B arm.  The
    * two core switches are weak: a build without SCHED_FAST / IRQ_IO_FAST
    * has no such variable.  irq_io_fast is read when the emitter builds its
    * stubs, so it is set here, before any game loads. */
   {
      extern uint32_t sched_fast __attribute__((weak));
      extern uint32_t irq_io_fast __attribute__((weak));
      if (&sched_fast)
         sched_fast = fe_ini_get_int(p_config, "sched_fast",
                                     PLAY_SCHED_FAST) ? 1u : 0u;
      if (&irq_io_fast)
         irq_io_fast = fe_ini_get_int(p_config, "irq_io_fast",
                                      PLAY_IRQ_IO_FAST) ? 1u : 0u;
      {
         extern uint32_t msr_fast __attribute__((weak));
         if (&msr_fast)
            /* default: on unless the core was built MSR_FAST_DEFAULT=0 */
            msr_fast = fe_ini_get_int(p_config, "msr_fast",
                                      PLAY_MSR_FAST && msr_fast) ? 1u : 0u;
      }
#ifdef GBA_LINK
      {
         long v = fe_ini_get_int(p_config, "gba_link_headless_audio",
                                 PLAY_GBA_LINK_HEADLESS_AUDIO);
         cfg.gba_dual_partner_audio = v == 2 ? -1 : (v ? 0 : 1);
         v = fe_ini_get_int(p_config, "gba_link_fast", PLAY_GBA_LINK_FAST);
         cfg.gba_dual_cable_fast = (v >= 0 && v <= 3) ? (int)v : 0;
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
      cfg.gb_bench_headless = (int)fe_ini_get_int(HARNESS_INI, "gb_headless", 0);
      if (fe_ini_get(HARNESS_INI, "gb_dual_rom", v, sizeof(v)) && v[0])
      {
         snprintf(dual_rom, sizeof(dual_rom), "%s%s%s",
                  strchr(v, ':') ? "" : p_rom_dir, strchr(v, ':') ? "" : "/",
                  v);
         cfg.gb_dual_rom = dual_rom;
         cfg.gb_dual_mirror =
            (int)fe_ini_get_int(HARNESS_INI, "gb_dual_mirror", 0);
         cfg.gb_dual_batch =
            (unsigned)fe_ini_get_int(HARNESS_INI, "gb_dual_batch", 1);
         /* The one-console trade (docs/GB-CORE-PERF.md): gb_dual_inputs =
          * linkplay's input track for both machines, gb_dual_sav = slot 1's
          * battery image, gb_dual_rtc = the session clock seed. */
         cfg.gb_dual_armed =
            fe_ini_get_int(HARNESS_INI, "gb_dual_armed", 0) != 0;
         cfg.gb_dual_quiet =
            (unsigned)fe_ini_get_int(HARNESS_INI, "gb_dual_quiet", 0);
         if (fe_ini_get(HARNESS_INI, "gb_dual_rtc", v, sizeof(v)) && v[0])
            cfg.gb_dual_rtc = strtoll(v, NULL, 10);
         if (fe_ini_get(HARNESS_INI, "gb_dual_inputs", v, sizeof(v)) && v[0])
         {
            snprintf(dual_inputs, sizeof(dual_inputs), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg.gb_dual_inputs = dual_inputs;
         }
         if (fe_ini_get(HARNESS_INI, "gb_dual_sav", v, sizeof(v)) && v[0])
         {
            snprintf(dual_sav, sizeof(dual_sav), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg.gb_dual_sav = dual_sav;
         }
         if (fe_ini_get(HARNESS_INI, "gb_dual_state", v, sizeof(v)) && v[0])
         {
            snprintf(dual_state, sizeof(dual_state), "%s%s%s",
                     strchr(v, ':') ? "" : p_rom_dir,
                     strchr(v, ':') ? "" : "/", v);
            cfg.gb_dual_state = dual_state;
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
      cfg.gba_dual = (int)fe_ini_get_int(HARNESS_INI, "gba_dual", 0);
      cfg.gba_dual_mirror =
         (int)fe_ini_get_int(HARNESS_INI, "gba_dual_mirror", 0);
      cfg.gba_dual_lookahead =
         (unsigned)fe_ini_get_int(HARNESS_INI, "gba_dual_lookahead", 0);
      cfg.gba_dual_own_rom =
         (int)fe_ini_get_int(HARNESS_INI, "gba_dual_own_rom", 0);
      /* GBA-LINK-MIRROR.md 16: the headless partner mixes no sound
       * (config.ini kill switch; the harness key wins for an A/B arm) */
      {
         long v = fe_ini_get_int(p_config, "gba_link_headless_audio",
                                 PLAY_GBA_LINK_HEADLESS_AUDIO);
         v = fe_ini_get_int(HARNESS_INI, "gba_link_headless_audio", v);
         /* 2 (section 23): as 1, and the partner's ring is not read out */
         cfg.gba_dual_partner_audio = v == 2 ? -1 : (v ? 0 : 1);
         v = fe_ini_get_int(p_config, "gba_link_sram_crc", 0);
         cfg.gba_dual_sram_crc =
            fe_ini_get_int(HARNESS_INI, "gba_link_sram_crc", v) != 0;
         /* 17.5: instance B's translation cache offset mod 8 KiB (-1 = the
          * build's JIT_PIN_OFFSET, the same sets as instance A) */
         v = fe_ini_get_int(p_config, "gba_link_jit_pin_b", -1);
         v = fe_ini_get_int(HARNESS_INI, "gba_link_jit_pin_b", v);
         cfg.gba_dual_b_jit_pin_p1 = v >= 0 ? (unsigned)(v & 8191) + 1u : 0u;
         /* 21: the cable fast path in both cores (pure speed, same
          * machines; 0 = the path every build up to 3.5.0 used) */
         v = fe_ini_get_int(p_config, "gba_link_fast", PLAY_GBA_LINK_FAST);
         v = fe_ini_get_int(HARNESS_INI, "gba_link_fast", v);
         /* 24: scheduler fast paths (SCHED_FAST builds only; default 0).
          * Weak: a build without the switch has no such variable. */
         {
            extern uint32_t sched_fast __attribute__((weak));
            long sf = fe_ini_get_int(HARNESS_INI, "sched_fast",
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
            long tl = fe_ini_get_int(HARNESS_INI, "timer_lazy",
                                     &timer_lazy ? (long)timer_lazy : 0);
            if (&timer_lazy)
               timer_lazy = (uint32_t)tl;
            cfg.gba_core_mask = (&timer_lazy && (timer_lazy & 1u))
                                   ? FE_GBLINK_MASK_TIMER_LAZY : 0u;
            /* 28/29: "Fast link" is NOT set here: it is the player's
             * setting, read when a session starts (gblink_begin). */
            fe_evt("timer_lazy built=%d on=%d core_mask=%08x",
                   &timer_lazy != NULL, &timer_lazy ? (int)timer_lazy : 0,
                   cfg.gba_core_mask);
         }
         /* 25: IE/IF/IME stores inline in the I/O store stub (IRQ_IO_FAST
          * builds only; default 0).  Read by the emitter at game load. */
         {
            extern uint32_t irq_io_fast __attribute__((weak));
            long qf = fe_ini_get_int(HARNESS_INI, "irq_io_fast",
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
            long mf = fe_ini_get_int(HARNESS_INI, "msr_fast",
                                     &msr_fast ? (long)msr_fast : 0);
            if (&msr_fast)
               msr_fast = mf ? 1u : 0u;
            fe_evt("msr_fast built=%d on=%d", &msr_fast != NULL,
                   &msr_fast ? (int)msr_fast : 0);
         }
         /* 22: 2 = the checking mode (the full path decides, the fast
          * path's answers are compared with it and disagreements counted) */
         /* 23: 3 = the fast path plus the lean inline hooks */
         cfg.gba_dual_cable_fast = (v >= 0 && v <= 3) ? (int)v : 0;
         /* 22: the pair's wall-clock bound per step, ms (0 = 15000,
          * negative = none) */
         cfg.gba_dual_step_max_ms =
            (int)fe_ini_get_int(HARNESS_INI, "gba_link_step_max_ms", 0);
         /* 21: the one-console bench starts its pair at power-on (harness
          * only; without it the pair waits for a state load) */
         cfg.gba_dual_cold =
            fe_ini_get_int(HARNESS_INI, "gba_dual_cold", 0) != 0;
         /* 18: lend instance B's static block to the ROM cache while no
          * session runs (kill switch gba_link_lend = 0) */
         v = fe_ini_get_int(p_config, "gba_link_lend", 1);
         v = fe_ini_get_int(HARNESS_INI, "gba_link_lend", v);
         cfg.gba_link_nolend = v ? 0 : 1;
      }
      if (fe_ini_get(HARNESS_INI, "gba_dual_script", gd_script,
                     sizeof(gd_script)) && gd_script[0])
         cfg.gba_dual_script = gd_script;
      /* gba_dual_autopilot = file: the partner's autopilot script (RAM
       * predicates on the partner's machine); wins over gba_dual_script. */
      if (fe_ini_get(HARNESS_INI, "gba_dual_autopilot", gd_ap,
                     sizeof(gd_ap)) && gd_ap[0])
         cfg.gba_dual_autopilot = gd_ap;
      if (fe_ini_get_int(HARNESS_INI, "gba_dual_hash", 0))
      {
         snprintf(gd_hash, sizeof(gd_hash), "%s/gbadual_hash.txt", LOG_DIR);
         cfg.gba_dual_hash = gd_hash;
      }
      /* gblink_role = host|join with gba_dual = 1 on a GBA game: a
       * two-console link-cable session instead of the one-console bench.
       * gblink_hash_every: slot-0 frames between the session's HASH
       * messages (0 = 240, as a GB session). */
      g_gba_cable = cfg.gba_dual && g_gblink_role &&
                    g_pcfg.console == FE_CONSOLE_GBA;
      cfg.gba_dual_session = g_gba_cable;
      /* harness `link_avail = 1` (with gba_dual_late = 1): what a player's
       * build does at every GBA game load -- a cable room CAN be hosted, no
       * session is running.  For tests of everything that must keep
       * working in that state (the rewind did not: fe_host_rewind_blocked). */
      if (fe_ini_get_int(HARNESS_INI, "link_avail", 0) && !g_gba_cable &&
          g_pcfg.console == FE_CONSOLE_GBA)
      {
         cfg.gba_dual = 1;
         cfg.gba_dual_session = 1;
      }
      cfg.gblink_hash_every = (unsigned)fe_ini_get_int(HARNESS_INI,
                                                       "gblink_hash_every", 0);
      if (g_gba_cable)
         fe_evt("gba_cable role=%s hash_every=%u", g_gblink_role == 1 ?
                "host" : "join", cfg.gblink_hash_every);
      fe_evt("gba_dual_cfg on=%d mirror=%d own_rom=%d lookahead=%u "
             "script=%s hash=%d partner_audio=%d sram_crc=%d jit_pin_b=%d "
             "fast=%d cold=%d",
             cfg.gba_dual,
             cfg.gba_dual_mirror, cfg.gba_dual_own_rom,
             cfg.gba_dual_lookahead,
             cfg.gba_dual_script ? cfg.gba_dual_script : "-",
             cfg.gba_dual_hash != NULL, cfg.gba_dual_partner_audio,
             cfg.gba_dual_sram_crc, (int)cfg.gba_dual_b_jit_pin_p1 - 1,
             cfg.gba_dual_cable_fast, cfg.gba_dual_cold);
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
      int menu = (int)fe_ini_get_int(HARNESS_INI, "gba_link_menu", 0);
      g_link_exit_after = (unsigned)fe_ini_get_int(HARNESS_INI,
                                                   "link_exit_after", 0);
      fe_host_gba_cable_rom_sampled(
         !fe_ini_get_int(HARNESS_INI, "gba_link_rom_full", 0));
#else
      int menu = 1;
#endif
      if (menu && g_pcfg.console == FE_CONSOLE_GBA && !cfg.gba_dual)
      {
         cfg.gba_dual = 1;
         cfg.gba_dual_session = 1;
      }
      g_gba_cable_ok = cfg.gba_dual && cfg.gba_dual_session;
      /* Section 19: B loads at session start, its memory the session's.  A
       * player's build always; a harness run with gba_dual_late = 1 (the
       * rigs' role sessions keep B loaded at boot unless they ask). */
#ifdef GPSP_PERF_RIG
      g_gba_late = (int)fe_ini_get_int(HARNESS_INI, "gba_dual_late", 0) != 0;
      cfg.gba_session_reserve_kb = (unsigned)fe_ini_get_int(HARNESS_INI,
         "cable_reserve_kb", (long)CABLE_RESERVE_KB);
      cfg.gba_sessmem_fail = (int)fe_ini_get_int(HARNESS_INI,
                                                 "sessmem_fail", 0);
      if (cfg.gba_sessmem_fail)
         fe_evt("sessmem_fail mode=%d", cfg.gba_sessmem_fail);
#else
      g_gba_late = 1;
      cfg.gba_session_reserve_kb = CABLE_RESERVE_KB;
#endif
      cfg.gba_dual_late = g_gba_late;
      cfg.gba_reload_note = gba_reload_note;
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
            case 0: v = fe_ini_get_int(HARNESS_INI, "gba_link_pin_a_small", v); break;
            case 1: v = fe_ini_get_int(HARNESS_INI, "gba_link_pin_a_large", v); break;
            case 2: v = fe_ini_get_int(HARNESS_INI, "gba_link_pin_b_small", v); break;
            default: v = fe_ini_get_int(HARNESS_INI, "gba_link_pin_b_large", v); break;
            }
#endif
            p1 = v >= 0 ? (unsigned)(v & 8191) + 1u : 0u;
            if (k < 2)
               cfg.gba_sess_pin_a_p1[k] = p1;
            else
               cfg.gba_sess_pin_b_p1[k - 2] = p1;
         }
         fe_evt("gba_sess_pin a=%d/%d b=%d/%d (small/large, -1 keep)",
                (int)cfg.gba_sess_pin_a_p1[0] - 1,
                (int)cfg.gba_sess_pin_a_p1[1] - 1,
                (int)cfg.gba_sess_pin_b_p1[0] - 1,
                (int)cfg.gba_sess_pin_b_p1[1] - 1);
      }
      fe_evt("gba_link_menu on=%d cable_ok=%d late=%d", menu, g_gba_cable_ok,
             g_gba_late);
   }
#endif
   cfg.core_counters = plat_core_counters;                   /* ADR-0028 */
   cfg.boot_status   = rom_loading_stage;
   cfg.smc_addr      = plat_smc_addr;                        /* ADR-0030 */
   cfg.smc_block     = plat_smc_block;                       /* phase 5g */
   cfg.core_phase    = plat_core_phase;                      /* phase 5h */
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
      extern time_t (*gpsp_wallclock)(void);
      gpsp_wallclock = psp_local_wallclock;
      /* THE CARTRIDGE CLOCK IS THE CONSOLE'S (rtc_clock.h): published here
       * so the very first read has it, then kept current by the main loop.
       * A harness run keeps the repeatable frame clock unless it asks
       * (`rtc_wall = 1`): every rig compares runs made at different hours,
       * and a game that tints the screen by time of day would differ. */
      g_rtc_wall_on = have_harness
         ? (int)fe_ini_get_int(HARNESS_INI, "rtc_wall", 0) != 0
         : g_pcfg.rtc_realtime != 0;
      rtc_wall_publish(1);
      fe_evt("rtc_wall on=%d src=%s", g_rtc_wall_on,
             have_harness ? "harness" : "config");
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
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
   heap_snap(&snap_pre);
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
      long kb = fe_ini_get_int(HARNESS_INI, "cable_reserve_kb",
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
   extern void (*gpsp_rom_load_progress)(unsigned, unsigned);
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
   int boot_rc = fe_host_boot(&cfg);
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
         int meb = (int)fe_ini_get_int(HARNESS_INI, "me_boot",
                                       g_pcfg.me_boot);
         int mbn = (int)fe_ini_get_int(HARNESS_INI, "me_bench", 0);
         int mev = (int)fe_ini_get_int(HARNESS_INI, "me_video", PLAY_ME_VIDEO);
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
               snap_pre.kfree -= kf - (unsigned)sceKernelTotalFreeMemSize();
               snap_pre.kmax  -= km - (unsigned)sceKernelMaxFreeMemSize();
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

#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
   g_stall_s = (int)fe_ini_get_int(HARNESS_INI, "stall_watch_s", 0);
#ifdef GPSP_PERF_RIG
   g_stall_exit_s = (int)fe_ini_get_int(HARNESS_INI, "stall_exit_s", 0);
   g_stall_exit_hard =
      (int)fe_ini_get_int(HARNESS_INI, "stall_exit_hard", 0) != 0;
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

      /* ADR-0064: closes the PREVIOUS frame's `wait` region and opens this
       * frame's `pre`.  ADR-0065: open the protected window here, before any
       * of the frame's work — see emu_boost_open(). */
      preempt_mark(3);
      emu_boost_open();

      rtc_wall_publish(0);
      /* LOW LATENCY: did the previous iteration's flip latch on the vblank
       * after its submit?  (Checked before anything can submit again.) */
      if (g_lat_check &&
          (int)(sceDisplayGetVcount() - g_lat_sub_vc) >= 1)
      {
         if (vid_flip_latched())
            g_lat_l1++;
         else
            g_lat_l0++;
         g_lat_check = 0;
      }
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
      {
         unsigned age = (unsigned)sceKernelGetSystemTimeLow() - pd.TimeStamp;
         if (age < 100000u)
         {
            g_lat_age_n++;
            g_lat_age_us += age;
         }
      }
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
#ifdef RFU_SCHED
      if (rfu_sched_d)
         rfu_sched_pause(ui_active());   /* the partner waits while our menu is open */
#endif
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
               draw = ff_gate_frame(&g_ff_gate_st, net_now_us());
               if (!draw)
                  FFW_GATED();
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
      if (!mer_sameframe_active())
         me_rend_retire_if_idle();
      /* SAME-FRAME shows the frame at the END of its own frame, not at the
       * top of the next one -- so the top-of-loop present is skipped and the
       * GE draw happens in the vblank wait below, once the ME reports done. */
#ifdef REWIND
      if (psp_rewind_present())
         ;                    /* a rewind is on screen: its frame, not the ME's */
      else
#endif
      if (!mer_sameframe_active())
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
         g_ffh_vb++;
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
      preempt_mark(0);           /* ADR-0064: close `pre`, open `core` */
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
         int i;
         for (i = 0; i < 3; i++)
         {
            g_blit_suppress = (i != 2);
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
#ifdef RFU_SCHED
      else if (!fe_host_rfu_gate())
      {
         /* scheduled ticks: the partner's tick is still missing; no frame */
      }
#endif
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
      preempt_mark(1);           /* ADR-0064: close `core`, open `post` */
      frames = fe_host_frame_count();

      if (vid_prof_win > 0)
         vid_prof_frame((unsigned)vid_prof_win);
      vram_probe_frame();        /* no-op unless `vram_probe` is set */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
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
      /* ADR-0066 (SPIKE-FINDINGS §4/§8): a SYNCHRONOUS NEGATIVE
       * `sceIoGetstat()` on ms0:, on the emulation thread, INSIDE the region
       * `work_us` times, looking for a developer-convenience marker file that
       * no automated run ever creates (`tools/hw_loop.py` does not know the
       * path).  PRIOWINDOW §6 measured it on this device: **12.4 ms**, i.e.
       * 74 % of a 59.73 fps frame budget, once a second, in every run of four
       * sessions.
       *
       * BUT IT IS NOT FREE TO REMOVE, AND THAT IS THE POINT OF THE KNOB.
       * While the stat blocks, the emulation thread is NOT RUNNING — so this
       * was also an involuntary 12.4 ms/s of guaranteed CPU for the WLAN
       * driver and our own RX/TX threads.  Deleting it deletes that yield.
       * PRIOWINDOW §15 ran the 29.97 regression gate on a build that removed
       * it unconditionally and got **7/14 against 7/7 historical**
       * (Fisher p = 0.04).  That is not proof it was the cause — §16 is the
       * A/B that tests it — but it is more than enough to say a change like
       * this must not be a silent default.
       *
       * ADR-0069 SUPERSEDES THE DEFAULT, NOT THE REASONING.  The stat bought
       * two things and they are now separated:
       *
       *  * the FEATURE ("a human wants a screenshot right now") is served by
       *    L+R+SELECT above, off a pad we already read every frame, for free
       *    and with no filesystem involved.  `dump_marker_poll` therefore
       *    defaults to 0 and the stat is gone from the frame region;
       *  * the YIELD is now `sched_yield_us`, an explicit sceKernelDelayThread
       *    of a duration WE choose, rather than however long the Memory Stick
       *    felt like blocking for that second.  Default 0.
       *
       * §16's A/B on the old binary came back 4/6 with the same failure
       * signatures (Fisher p = 0.64), so the removal is no longer meaningfully
       * implicated in the 29.97 gate — but if it turns out the WLAN threads
       * genuinely miss the stall, `sched_yield_us = 12400` reconstructs it at
       * the measured size and that is the arm to run.
       *
       * `dump_marker_poll = 60` still restores the old mechanism exactly. */
      if (g_dump_marker_every &&
          (frames % (unsigned)g_dump_marker_every) == 0 &&
          file_exists(DUMP_MARKER))
      {
         dump_frame_bmp();
         sceIoRemove(DUMP_MARKER);
      }
      /* ADR-0069: the yield, made deliberate.  Same cadence the stat had (once
       * per `g_yield_every` frames) so an A/B against the historic build is a
       * like-for-like comparison of a 12.4 ms stall that reads the Memory
       * Stick against one that does not. */
      if (g_yield_us && g_yield_every &&
          (frames % (unsigned)g_yield_every) == 0)
         sceKernelDelayThread((SceUInt)g_yield_us);
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
      emu_boost_close();         /* ADR-0065: end the protected window */
      LOOP_PHASE(4);
      preempt_mark(2);           /* ADR-0064: close `post`, open `wait` */
      preempt_frame_end();

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
            g_ll_pull++;
         }
         else if (want == 1u && !g_ff_mult && !g_ff_uncapped && !g_gbl_audio_on &&
                  lf_want_slot(ring_w - ring_r))
         {
            want++;               /* CATCH UP SOON: the same nudge, early */
            g_fh_nudge++;
            g_ar_nudge++;
            g_lf_pull++;
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
            g_ffh_waits++;
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
               g_vb_vc = sceDisplayGetVcount();
               g_lat_vbhc_n++;
               g_lat_vbhc_sum += (unsigned)sceDisplayGetCurrentHcount();
            }
         }
         lat_wait_end();
         g_flip_early = ffh_fe;
         FE_LP_SET(FELP_L_RETIRE);

         /* Counted here, after the wait: still pending at the swap means the
          * ME did not make its own frame, so the previous one goes out and
          * this render lands next frame -- the shipping behaviour. */
         /* ONE LAST LOOK BEFORE GIVING UP.
          *
          * The loop above checks the ME and then BLOCKS in
          * sceDisplayWaitVblankStart().  The ME routinely finishes DURING
          * that block, and the loop then exits on its vcount condition
          * without ever re-checking -- so a frame that was rendered and
          * ready got thrown away and the previous one went out instead.
          * Measured before this existed: of 533 and 600 "misses" in two
          * windows, 529 and 502 had the ME already idle (99%% and 84%%).
          * Same-frame was not losing a race, it was not looking at the
          * finish line.  Hit rate 10.9%% -> 85.7%% on this one check; the
          * blit sits at the same vblank phase the in-loop present used, so
          * nothing about timing changes. */
         me_rend_retire_if_idle();
         if (mer_sameframe_active() && g_me_rend && g_mer_pending >= 0)
         {
            g_mer_sf_miss++;
            if (me_host_idle())
               g_mer_sf_wouldhit++;   /* must stay 0 */
         }

         /* THE PRESENT.  Unconditional, once, immediately before the swap.
          * Whatever is current goes into the back buffer -- a fresh render
          * if one retired this frame, otherwise the one already showing.
          * Re-drawing an unchanged frame costs a GE blit we would rather
          * not spend, and buys the guarantee that the back buffer is never
          * two frames stale.  That is a trade worth making every time. */
         FE_LP_SET(FELP_L_PRES);
#ifdef REWIND
         if (mer_sameframe_active() && !g_rw_showing)
#else
         if (mer_sameframe_active())
#endif
         {
            me_rend_present();
#ifdef REWIND
            if (g_drew && (g_rw_on || g_rwh.denied))
               psp_rewind_hud();
#endif
            if (g_drew)
               osd_draw();
         }

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
            ffh_cap3();           /* FF FLIP PACING bit 2: the 3x ceiling */
         if (g_drew)
         {
         /* UNCAPPED FF: swap only when a frame was actually blitted.
          *
          * No same-frame handling here.  mer_sameframe_active() is false
          * whenever fast-forward is engaged, so the loop-top present ran
          * as it always did and this branch is untouched shipping code. */
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
   /* @lab */ LAB_FFP(lab_ffpace_finish();)
   /* A link session that could not put the game back (F5): its code and
    * reason, which gblink_begin / link_end cannot reach (main's locals). */
   if (g_link_fatal_why && !exit_reason)
   {
      exit_code = g_link_fatal_code;
      exit_reason = g_link_fatal_why;
   }

   if (g_perf_rig)
   {
      extern unsigned int translation_gate_targets;
      extern unsigned int translation_gate_target_pc[];
      char gates[16 * 10];
      unsigned gn, gi;
      int gp = 0;

      rig_emit(fe_host_frame_count());
      gn = translation_gate_targets;
      if (gn > 16)
         gn = 16;
      for (gi = 0; gi < gn; gi++)
         gp += snprintf(gates + gp, sizeof(gates) - gp, "%s%08x",
                        gi ? "," : "", translation_gate_target_pc[gi]);
      if (!gp)
         snprintf(gates, sizeof(gates), "-");
      fe_evt("smc_gates n=%u pcs=%s", translation_gate_targets, gates);
      fe_evt("perf_done samples=%u expected=%u",rig_stats.total_n,rig_to-rig_from);
   }
   session_end(have_variant);
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
         dynarec_release_translation_caches();
         /* XS_HEAP's SMC tables (70,656 B): the first game's retro_init
          * takes them, after the boot snapshot, and a reload of the same
          * game reuses them -- but the next game's retro_init must find a
          * fresh boot's heap, not the last game's block, or the census
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
         heap_snap(&after);
         exit_sim("unload");        /* harness: HOME here (S-4 d) */
#ifndef GPSP_NO_TELEMETRY
         heap_census("post_unload");
#endif
         heap_trace_report("post_unload");
         if (!heap_snap_same(&after, &snap_pre))
         {
            /* Harness `inproc_force = 1` (diagnosis only): log, carry on. */
            if (!fe_ini_get_int(HARNESS_INI, "inproc_force", 0))
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
         fe_evt("inproc_return n=%d", inproc_n);
         inproc_n++;
         /* The list and the next game run at the setting's clock, whatever
          * the teardown met (a wake in the list itself is answered by the
          * next game's "power back": g_pwr_resume waits for it). */
         psp_clock_apply("inproc_return");
#ifdef GPSP_PERF_RIG
         if (g_ss_browser && inproc_n == g_ss_game + 1)
            sleep_sim_fire();        /* fires in the browser, 300 ms on */
#endif
         /* A HOME after the refusal check above (the unload, the census,
          * the clock) must still end the process: the list then returns at
          * once and leaves through its own HOME exit (F1). */
         session_reset(&exit_code, &exit_reason, &ff_engaged, &ff_toggled, &ff_last_mode,
                       &ff_last_me, &prev_pad, &chord_frames, &inproc_menu_armed,
                       &autoload_state_slot, me_rend_cfg_boot, sram_thread_boot,
                       autoload_boot);
         emu_boost_start();
         pwr_release();         /* the browser waits on the player: no hold */
         goto session_top;
      }
      fe_evt("inproc_refused why=%s", why);
      g_relaunch = RELAUNCH_GAMELIST;
      me_host_shutdown();       /* kept for a return that is not happening */
   }
   process_end(exit_code, exit_reason);
   return 0;
}
/* @extract:end fn:main */
