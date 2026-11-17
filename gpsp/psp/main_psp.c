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
#include "../rfu_sched_api.h"
#include "fe_linkprof.h"
#include "fe_evt.h"
#include <psprtc.h>
#include <time.h>
#include "fe_util.h"
#ifndef GPSP_PERF_RIG
#define FE_AUTOPILOT_STUBS   /* no autopilot in a player build (C2-5) */
#endif
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
#include "core_fe.h"      /* the core's exports to a frontend (tier C, C-6): no
                           * fragment copies a core declaration by hand */
#include "gbcore/gbcore.h"   /* gbcore_prepare */

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
#include "heap_trace.h"
#include "icprobe.h"
#include <kubridge.h>        /* kuKernelGetModel: the rig's psp_model line */
#include "perf_rig.h"
#include "sidecar_dirs.h"   /* saves/ and states/ (4.6.0-beta.4) */
#include "fe/fe_priv.h"      /* this file's macros + PLAY_* defaults (P5) */

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
#include "fe/loop.inc.c"
int main(int argc, char *argv[]) __attribute__((section(".text.fe_cold.main")));   /* C-1 */
/* @extract:begin fn:main */
int main(int argc, char *argv[])
{
   char rom_path[PSP_FILE_PATH_CAP];
   char save_path[PSP_FILE_PATH_CAP], state_path[PSP_FILE_PATH_CAP];
   int autoload_state_slot = 1;
   fe_host_config cfg;
   /* The in-process return to the game list (inproc_refusal): how many
    * times this process has done it, and the heap as it was when this
    * game's ROM was picked.  The per-game values a fresh boot starts every
    * game from are the boot's (bc), loop_reset puts them back. */
   int inproc_n = 0;
   heap_snap_t snap_pre;
   boot_ctx bc;   /* the boot's values (boot_run); frame_loop reads its own through S.bc */
   loop_t L = { .ff_last_mode = -1, .ff_last_me = -1 };   /* the loop's state (loop_t) */

   if (boot_run(argc, argv, &bc) >= 0)
      return 0;
   L.dump_at = bc.dump_at;
   L.gedump_at = bc.gedump_at;
   L.ui_demo = bc.ui_demo;
   L.ui_ctl_demo = bc.ui_ctl_demo;
   L.wake_shot = bc.wake_shot;
   L.player_shot_n = bc.player_shot_n;

   /* ONE PASS PER GAME: the selection, the boot of the game, the frame loop and
    * session_end; then the in-process return goes round again for the next
    * game (inproc_try_return), or the process ends (process_end, below).  The
    * pass keeps main()'s indentation. */
   for (;;)
   {
   if (session_select(rom_path, save_path, state_path, &autoload_state_slot, inproc_n,
                      bc.inproc_roms, bc.inproc_rom_n, bc.have_variant, bc.have_harness,
                      bc.folders) >= 0)
      return 0;

   session_configure(&cfg, rom_path, save_path, bc.have_harness, bc.net_host, bc.net_join);

   if (session_load(&cfg, &snap_pre, rom_path, inproc_n) >= 0)
      return 0;

   if (session_net_up(bc.have_script, bc.script_name, bc.net_host, bc.net_join, bc.net_probe,
                      bc.group, bc.nick) >= 0)
      return 0;

   session_pre_loop();
   const session_ctx S = { &bc, state_path, inproc_n, autoload_state_slot };
   frame_loop(&L, &S);
   /* @lab */ LAB_FFP(lab_ffpace_finish();)
   /* A link session that could not put the game back (F5): its code and
    * reason, which gblink_begin / link_end cannot reach (the loop's state). */
   if (g_link_fatal_why && !L.exit_reason)
   {
      L.exit_code = g_link_fatal_code;
      L.exit_reason = g_link_fatal_why;
   }

   if (g_perf_rig)
   {
      char gates[16 * 10];
      unsigned gn, gi;
      int gp = 0;

      rig_emit(fe_host_frame_count());
      gn = translation_gate_targets;
      if (gn > 16)
         gn = 16;
      for (gi = 0; gi < gn; gi++)
         gp += snprintf(gates + gp, sizeof(gates) - gp, "%s%08x",
                        gi ? "," : "", (unsigned)translation_gate_target_pc[gi]);
      if (!gp)
         snprintf(gates, sizeof(gates), "-");
      fe_evt("smc_gates n=%u pcs=%s", translation_gate_targets, gates);
      fe_evt("perf_done samples=%u expected=%u",rig_stats.total_n,rig_to-rig_from);
   }
   session_end(bc.have_variant);
   if (!inproc_try_return(bc.have_variant, &inproc_n, snap_pre))
      break;
   /* A HOME after the refusal check above (the unload, the census,
    * the clock) must still end the process: the list then returns at
    * once and leaves through its own HOME exit (F1). */
   loop_reset(&L, &bc, &autoload_state_slot);
   pwr_release();         /* the browser waits on the player: no hold */
   }   /* the next game */
   process_end(L.exit_code, L.exit_reason);
   return 0;
}
/* @extract:end fn:main */
