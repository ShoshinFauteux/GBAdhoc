/* main_psp.c — native PSP frontend for the gpsp libretro core.
 *
 * Hosts the statically-linked core via frontend-common/fe_host:
 *  - GU video layer in video_psp.c: RGB565→PSP-5650 staging conversion
 *    (the hw color-order fix), scaling 1x/fit/stretch + nearest/bilinear
 *    (Triangle cycles presets at runtime; persisted in config.ini)
 *  - audio via sceAudioOutputBlocking on its own thread, fed by an SPSC
 *    ring (core mixes at 65536 Hz; resampled to the PSP's 44100 Hz output)
 *  - sceCtrl input mapped per FRONTEND-AUDIT §4 (Triangle/Square are
 *    frontend keys: preset cycle + fast-forward; turbo A/B dropped)
 *  - UI v1 (ui_psp.c, plan §8): ROM browser (no harness ini => browser),
 *    in-game menu on Select+Start hold (Resume/Save state/Load state/
 *    Wireless/Settings/Exit), wireless panel with Host + Join(scan/code),
 *    OSD toasts + session/FF chips (osd_psp.c)
 *  - fast-forward as a product feature (plan §4.4): Square hold (or
 *    toggle), multiplier 1.5/2/3/uncapped from config, core frameskip
 *    engaged while active, OSD chip, forced 1x during wireless sessions
 *  - scePowerSetClockFrequency(333,333,166) at init (plan §9 / ADR-0004)
 *  - EVT log at <appdir>/log/frontend.log (fflush per line)
 *  - exit callback flushes SRAM — never lose a save
 *  - .gpsp-harness.ini (harness channel, ADR-0036; presence also implies
 *    "skip the ROM browser, boot the first ROM"): autoexit_frames /
 *    dump_at / dump_every; marker file log/dump_frame triggers an
 *    on-demand BMP framebuffer dump; gedump_at = N dumps the GE
 *    DRAWBUFFER (post-GU-blit, PSP-native 5650 decode) at frame N — the
 *    color-order regression check; testpat = 1 renders known color bars
 *    through the blit path instead of booting the core
 *    (run_gu_color_test.sh); script = <file> runs a scripted input
 *    script; ff = 1 forces uncapped fast-forward (harness mode);
 *    ui_demo = 1 opens the in-game menu at frame 300 and self-drives it
 *    (run_ui_smoke.sh)
 *  - wireless (harness keys host/join/nick/group/net_probe as in Phase
 *    4; nettest = 1 transport echo).  The UI wireless panel calls the
 *    same net_bringup/net_teardown.  FF interlocked off while a session
 *    is active; scePowerTick during sessions; .sav backed up to .bak
 *    before every session start (plan §4.5).
 */
#include <pspkernel.h>
#include <systemctrl.h>   /* ADR-0071: sctrlKernelLoadExecVSHMs2 (profile restart) */
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspsuspend.h>   /* sceKernelVolatileMemTryLock — engine buffers */

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>   /* ADR-0080: memalign for the ME double/stage buffers */
#include <string.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_autopilot.h"
#include "netpacket_host.h"
#include "transport_adhoc.h"
#include "video_psp.h"
#include "osd_psp.h"
#include "ui_psp.h"
#include "me_host.h"
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

/* All paths are derived from the EBOOT's own directory at runtime (argv[0])
 * so per-game variant installs (docs/VARIANTS.md) work from any folder;
 * falls back to the canonical install dir. */
#include "usb_handoff.h"                                  /* ADR-0053 */

#define BASE_DIR_DEFAULT "ms0:/PSP/GAME/gpsp-adhoc"
char g_dir_base[128] = BASE_DIR_DEFAULT;     /* also used by ui_psp */
static char p_rom_dir[144], p_log_dir[144], p_log_path[160];
static char p_harness[160], p_config[160], p_variant[160], p_marker[160];
static char p_legacy_ap[160];

/* THE HARNESS CONTROL CHANNEL — deliberately NOT a user-facing filename
 * (ADR-0036).  This file can auto-host, auto-join, skip the ROM browser,
 * force fast-forward and pin performance knobs; a stale copy on a memory
 * stick silently brought a wireless session up during a SOLO benchmark, and
 * had twice before skipped the browser or auto-hosted unexpectedly.
 *
 * It used to be called `autopilot.ini`, which sits in the same directory as
 * `config.ini` and `variant.ini` and looks exactly like something a user is
 * meant to edit.  That was the design error: an internal channel wearing a
 * user-config name.  The leading dot and the `gpsp-` prefix now say
 * "internal" to a human packaging a kit, and nobody creates this by accident.
 *
 * `variant.ini` is deliberately NOT renamed — that one is user-chosen. */
#define HARNESS_INI p_harness
/* Read-only: checked so a leftover can be REPORTED, never obeyed, never
 * deleted (it is the user's file, even when it is in the way). */
#define LEGACY_AUTOPILOT_INI p_legacy_ap
#define ROM_DIR     p_rom_dir
#define LOG_DIR     p_log_dir
#define LOG_PATH    p_log_path
#define CONFIG_INI  p_config
#define VARIANT_INI p_variant
#define DUMP_MARKER p_marker

/* ---- ADR-0067: THE PLAYABLE BUILD -------------------------------------
 *
 * `-DGPSP_PLAYABLE` produces a binary for someone to actually PLAY, from the
 * same tree as the harness build and with no runtime way to confuse the two:
 *
 *  * the harness ini path is pointed at a filename that cannot exist, so a
 *    leftover `.gpsp-harness.ini` on the user's card is inert.  No autopilot,
 *    no autoexit, no USB handoff, and the ROM browser comes up — none of which
 *    depends on the user having cleaned their card;
 *  * every harness-only key therefore returns its DEFAULT, so the shipping
 *    values below are supplied as those defaults rather than by a second code
 *    path.  There is exactly one place each knob is read, in both builds;
 *  * telemetry is compiled out (`GPSP_NO_TELEMETRY`), and no log directory or
 *    log file is created at all;
 *  * `PCFG_SESSION_FPS_DEF` is NOT touched.  The 29.97 default stands for the
 *    generic build; this build overrides its own copy of the setting.
 *
 * The four PLAY_* values are the FULLSPEED §28.2 ship stack.  In the harness
 * build every one of them is 0, which is that build's existing default, so
 * this block is provably behaviour-neutral there. */
/* NOTE: `GPSP_NO_TELEMETRY` is derived from `GPSP_PLAYABLE` in fe_evt.c, NOT
 * here.  It was here first, and it did nothing: `fe_evt.c` is a separate
 * translation unit and never saw this file's #define, so the playable build
 * linked with telemetry fully present while every token-presence check passed.
 * Caught by `psp-nm -u frontend-common/fe_evt.o` still listing `fopen` /
 * `fwrite` / `fflush` / `vsnprintf` — a BEHAVIOURAL check, which is the only
 * kind that would have caught it. */
#ifdef GPSP_PLAYABLE
#define PLAY_EMU_PRIO        0x2B
#define PLAY_IO_PRIO         0x2C
#define PLAY_POLL_MIN_CYCLES 34952
#define PLAY_BOOST_US        0
#define PLAY_FPS_X100        5973
/* ADR-0068 ON for a player.  Measured over 15 alternating runs at 59.73 with
 * COMPLETE SEPARATION on both consoles (p = 0.0002): host srtt 2.02 -> 0.91
 * emulated frames, join 1.45 -> 0.70.  Mechanism verified every run -- host
 * mid-frame polls 0 without it, 7-8 with it.
 *
 * IT IS NOT FREE: host frames-over-budget rise ~6.5 points (median 9.49 ->
 * 16.03 %, fixed 7000-frame prefix, distributions overlapping), which in
 * practice is ~59.1 fps median falling to ~57.4 on the HOST only; the join is
 * unchanged.  Included anyway because this build exists to complete trades by
 * hand and the link-robustness evidence is far stronger than the frame cost.
 * Set to 0 for the extra ~1.7 fps on the host. */
#define PLAY_IDLE_POLL       34952
#define PLAY_DUMP_MARKER     0          /* ADR-0066: OFF for a player */
/* ADR-0075: VALIDATED on hardware (fs-pace pace arm — Gate A closed, send
 * queue held <=1, gHeldKeyCodeToSend never nulled, trades clean).  ON for
 * players: this is the input-eating fix. */
#define PLAY_RFU_FRAME_PACE  1
/* Fixed-depth cushion (jitter buffer) PoC: hold each host packet N frames so a
 * stall must drain the whole reserve before the client underruns.  OFF by
 * default — UNVALIDATED on hardware; the desktop cannot exercise it (injected
 * jitter starves rather than backlogs).  Set rfu_cushion=N in the harness ini
 * to A/B it.  Keep N well under RFU_DEF_TIMEOUT (32). */
#define PLAY_RFU_CUSHION     0
/* ADR-0076: NOT superseded after all.  Live-hardware 2026-08-10 (fs stage-live)
 * proved exit_assist alone leaves the JOIN at rfu_discq mode=4 (grace expired)
 * with the queue wiped and 1 discans (press-A) still.  disc_grace=60 drained
 * the undelivered exit packets 64->3 but expired 3 short; 120 reached mode=5 —
 * the game issues its own clean disconnect INSIDE the window, rcl=0x0101, zero
 * discans on BOTH consoles.  Safe: exit_assist pins ka=0 so the 61-frame
 * keepalive never fires during the wider window.  ON for players. */
#define PLAY_RFU_DISC_GRACE  120
/* ADR-0079: VALIDATED (run 317, re-confirmed live 2026-08-10 — both sides run
 * the game's own READY_CLOSE_LINK handshake, rcl=0x0101, card stamped).  ON for
 * players: this is the "press A to return to lobby" fix. */
#define PLAY_EXIT_ASSIST     1
/* ADR-0080c: the Media Engine video-staging offload.  ON for players — the
 * ~750us/frame the ME frees is the performance headroom (join proven: blit
 * stage 751->0us, 37,200 frames 0 drops, srtt champion-band, trades clean;
 * host offload via the session-gated activation that dodges np_start).
 * SELF-DISABLING: if the ME does not answer its boot handshake (250 ms) or
 * the watchdog trips, the console silently runs single-core — so a model
 * where the ME behaves differently degrades, it does not break.  Validated
 * on a PSP-1000 pair; the fallback covers the rest until more models are
 * tested. */
#define PLAY_ME_BOOT         1
#define PLAY_ME_VIDEO        1
#else
#define PLAY_EMU_PRIO        0
#define PLAY_IO_PRIO         0
#define PLAY_POLL_MIN_CYCLES 0
#define PLAY_BOOST_US        0
#define PLAY_IDLE_POLL       0          /* ADR-0068 off by default (harness) */
/* ADR-0069: 0, not the historic 60.  The marker file's ONLY job was "let a
 * human ask for a screenshot mid-run", and L+R+SELECT now does that for free
 * off a pad we already read every frame — so the 12.4 ms stat is no longer
 * buying a feature, it is only buying the involuntary yield it always came
 * with.  `sched_yield_us` reconstructs that yield deliberately and at a size
 * we choose.  Set `dump_marker_poll = 60` to get the old mechanism back. */
#define PLAY_DUMP_MARKER     0
#define PLAY_RFU_FRAME_PACE  0          /* ADR-0075 off by default (harness) */
#define PLAY_RFU_CUSHION     0          /* fixed-depth cushion PoC off (harness) */
#define PLAY_RFU_DISC_GRACE  0          /* ADR-0076 off by default (harness) */
#define PLAY_EXIT_ASSIST     0          /* ADR-0079 off by default (harness) */
#define PLAY_ME_BOOT         0          /* ME keys off by default (harness)  */
#define PLAY_ME_VIDEO        0
#endif

static void init_paths(int argc, char *argv[])
{
   if (argc > 0 && argv && argv[0])
   {
      const char *slash = strrchr(argv[0], '/');
      if (slash && (size_t)(slash - argv[0]) < sizeof(g_dir_base))
      {
         memcpy(g_dir_base, argv[0], (size_t)(slash - argv[0]));
         g_dir_base[slash - argv[0]] = '\0';
      }
   }
   snprintf(p_rom_dir, sizeof(p_rom_dir), "%s/roms", g_dir_base);
   snprintf(p_log_dir, sizeof(p_log_dir), "%s/log", g_dir_base);
   snprintf(p_log_path, sizeof(p_log_path), "%s/log/frontend.log", g_dir_base);
#ifdef GPSP_PLAYABLE
   /* ADR-0067: a name no file can have, so every fe_ini_get*(HARNESS_INI,...)
    * in this binary returns its default and `have_harness` is 0.  Deliberately
    * not a compile-time removal of the call sites: one code path, two
    * behaviours, and no chance of the two builds drifting. */
   snprintf(p_harness, sizeof(p_harness), "%s/.playable-no-harness/x",
            g_dir_base);
#else
   snprintf(p_harness, sizeof(p_harness), "%s/.gpsp-harness.ini", g_dir_base);
#endif
   snprintf(p_legacy_ap, sizeof(p_legacy_ap), "%s/autopilot.ini", g_dir_base);
   snprintf(p_config, sizeof(p_config), "%s/config.ini", g_dir_base);
   snprintf(p_variant, sizeof(p_variant), "%s/variant.ini", g_dir_base);
   snprintf(p_marker, sizeof(p_marker), "%s/log/dump_frame", g_dir_base);
}

/* libretro joypad ids (kept local; matches libretro.h) */
#define JP_B      0
#define JP_Y      1
#define JP_SELECT 2
#define JP_START  3
#define JP_UP     4
#define JP_DOWN   5
#define JP_LEFT   6
#define JP_RIGHT  7
#define JP_A      8
#define JP_X      9
#define JP_L      10
#define JP_R      11

volatile int g_running = 1;

/* --- fast-forward state --------------------------------------------------
 * Uncapped FF (harness mode + "uncapped" user multiplier): no vblank
 * pacing, audio muted, frame blitted only every 32nd emulated frame.
 * Multiplier FF: N retro_runs per displayed frame (only the last one
 * blitted), audio muted, core frameskip engaged (plan §4.4). */
#define FF_PRESENT_MASK 31
static int g_ff_uncapped;
static int g_ff_mult;         /* multiplier FF active (user, paced) */
static int g_blit_suppress;   /* skip blits of intermediate FF frames */
static int g_drew;            /* a GU frame was drawn since the last swap */

/* ------------------------------------------------------------------- exit */

static int exit_cb(int arg1, int arg2, void *common)
{
   (void)arg1; (void)arg2; (void)common;
   /* Main loop notices, flushes SRAM, logs EVT exit, then exits the game. */
   g_running = 0;
   return 0;
}

static int cb_thread(SceSize args, void *argp)
{
   (void)args; (void)argp;
   int cbid = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
   sceKernelRegisterExitCallback(cbid);
   sceKernelSleepThreadCB();
   return 0;
}

static void setup_exit_callback(void)
{
   int thid = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0,
                                    THREAD_ATTR_USER, NULL);
   if (thid >= 0)
      sceKernelStartThread(thid, 0, NULL);
}

/* ------------------------------------------------- ms0 writer thread ----
 * ADR-0024.  Every EVT/LOG line used to be an fprintf + fflush to the
 * memory stick ON THE EMULATION THREAD.  The field priced it: `evt` maxima
 * of 12002 us (PSP-1000) and 12525 us (PSP-3000) — three quarters of a
 * 16.7 ms vblank period spent inside one flush, which in a vblank-locked
 * loop costs a whole extra period (and a peer four frames of silence).
 *
 * So the line now goes into fe_evt's ring and this thread does the I/O.
 * Priority 0x22 is BELOW main's 0x20 deliberately: signalling must not
 * preempt the emulator, and the writes land in the slack the emulation
 * thread already donates at sceDisplayWaitVblankStart — the same reasoning
 * as ADR-0021's `deferred` TX placement, but here there is no latency cost
 * to trade off, because nothing waits on a log line.
 *
 * Started only AFTER fe_host_boot(): boot diagnostics stay synchronous, so
 * a failure to load the BIOS/ROM is on the stick even if we never reach the
 * main loop.  config.ini `log_thread` (or `net_log_thread`) = 0 restores
 * the inline behaviour for a hardware A/B with no rebuild. */
#define IO_PRIO        0x22
#define IO_STACK       0x1000
#define IO_WAIT_US     250000     /* only bounds teardown latency */
#define IO_SEMA_MAX    64

static SceUID g_io_thid = -1, g_io_sema = -1;
static volatile int g_io_run;
/* ADR-0062c: harness-settable, because IO_PRIO's relationship to the
 * EMULATION thread is no longer fixed.  `emu_prio` (ADR-0062b) moves main
 * from 0x20 to 0x2B so the WLAN stack at 0x2A outranks it -- but on the PSP a
 * higher number is a LOWER priority, so anything numerically past 0x2A is
 * necessarily past 0x22 too.  Lowering main to reach the radio therefore
 * yields to OUR OWN housekeeping as well, which was never the intent: this
 * thread only drains the event log and the .sav and nothing waits on it.
 * Measured cost of that accident on the join at 59.73 (FULLSPEED-FINDINGS
 * §13.2): core 13871 -> 18819 us per frame.
 * Set io_prio numerically ABOVE emu_prio to put our housekeeping back below
 * the emulator while leaving the radio above it. */
static int g_io_prio = IO_PRIO;

static void io_wake(void)
{
   if (g_io_sema >= 0)
      sceKernelSignalSema(g_io_sema, 1);
}

/* ADR-0025: the same thread drains the .sav.  Both are memory-stick work
 * with nothing waiting on them, and one thread means one file-owning
 * context to reason about at shutdown. */
static void io_yield(void)
{
   sceKernelDelayThread(1000);
}

static const fe_host_io g_io_iface = { io_wake, io_yield };

static int io_thread(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (g_io_run)
   {
      SceUInt tmo = IO_WAIT_US;
      sceKernelWaitSema(g_io_sema, 1, &tmo);
      fe_evt_service();
      fe_host_sram_service_io();
   }
   fe_host_sram_service_io();   /* last pass before we go away */
   fe_evt_service();
   return 0;
}

static void io_thread_start(void)
{
   if (!g_pcfg.log_thread && !g_pcfg.sram_thread)
   {
      fe_evt("log_thread mode=0 sram_thread=0");
      return;
   }
   g_io_run  = 1;
   g_io_sema = sceKernelCreateSema("gpsp_io", 0, 0, IO_SEMA_MAX, NULL);
   if (g_io_sema >= 0)
   {
      g_io_thid = sceKernelCreateThread("gpsp_io", io_thread, g_io_prio,
                                        IO_STACK, THREAD_ATTR_USER, NULL);
      if (g_io_thid < 0 || sceKernelStartThread(g_io_thid, 0, NULL) < 0)
      {
         if (g_io_thid >= 0)
            sceKernelDeleteThread(g_io_thid);
         g_io_thid = -1;
         sceKernelDeleteSema(g_io_sema);
         g_io_sema = -1;
      }
   }
   if (g_io_thid < 0)
   {
      /* Never fail a run over an I/O optimisation: stay inline. */
      g_io_run = 0;
      fe_evt("log_thread mode=0 sram_thread=0 reason=thread_failed");
      return;
   }
   if (g_pcfg.log_thread)
      fe_evt_set_async(io_wake);
   if (g_pcfg.sram_thread)
      fe_host_set_io(&g_io_iface);
   fe_evt("log_thread mode=%d sram_thread=%d prio=0x%02x",
          g_pcfg.log_thread, g_pcfg.sram_thread, g_io_prio);
}

/* Stop the writer, then hand BOTH sinks back to the synchronous path and
 * drain whatever is still queued on THIS thread — fe_evt and the .sav each
 * tolerate exactly one consumer, so the order matters.  Idempotent.
 *
 * This is also the ADR-0025 ordering contract in code: after it returns, no
 * other thread can touch the .sav or the SRAM buffer, which is what makes
 * fe_host_shutdown()'s flush/sync/close and retro_unload_game() safe.  It
 * must therefore run BEFORE fe_host_shutdown(), not after. */
static void io_thread_stop(void)
{
   int had_thread = (g_io_thid >= 0);

   if (had_thread)
   {
      SceUInt tmo = 5000000;   /* the last pass may be flushing 128 KiB */
      g_io_run = 0;
      sceKernelSignalSema(g_io_sema, 1);
      if (sceKernelWaitThreadEnd(g_io_thid, &tmo) < 0)
         sceKernelTerminateDeleteThread(g_io_thid);
      else
         sceKernelDeleteThread(g_io_thid);
      g_io_thid = -1;
   }
   if (g_io_sema >= 0)
   {
      sceKernelDeleteSema(g_io_sema);
      g_io_sema = -1;
   }
   fe_host_set_io(NULL);
   fe_evt_set_async(NULL);
   if (had_thread)
   {
      /* Belt over braces: whatever the writer had not finished (including
       * the case where we had to terminate it) is completed here, on this
       * thread, before anything is closed or freed.  Save integrity is not
       * traded for smoothness — this is the line that guarantees it. */
      fe_host_sram_sync();
   }
}

/* Every exit path funnels through here so the log is always closed with the
 * writer already stopped and the ring already drained. */
static void evt_shutdown(void)
{
   io_thread_stop();
   fe_evt_close();
}

/* ------------------------------------------------------------------ audio */

#define OUT_RATE       44100
#define OUT_CHUNK      1024                 /* output frames per blocking write */
#define RING_FRAMES    32768                /* power of two, ~0.5 s at 65536 Hz */
#define RING_MASK      (RING_FRAMES - 1)
/* Audio-master pacing nudge (plan §4.4): skip an extra vblank when the ring
 * is more than 3/4 full — bleeds off the 59.94-vs-59.7275 Hz surplus. */
#define RING_HIGH_WATER (RING_FRAMES * 3 / 4)

static int16_t audio_ring[RING_FRAMES * 2];
static volatile unsigned ring_w;            /* producer: emu thread  */
static volatile unsigned ring_r;            /* consumer: audio thread */
static volatile int audio_running;
/* Resampler input rate.  Seeded with the core's historical default only so
 * the ring is sane before the core exists (audio_start runs before
 * fe_host_boot — see main()); the REAL value is taken from
 * fe_host_sample_rate() right after boot (ADR-0028).  Never hardcode this
 * ratio anywhere else. */
static unsigned in_rate = 32768;
static SceUID audio_thid = -1;

/* Resampler phase increment, 16.16, read fresh by the audio thread on every
 * output chunk.  Normally (in_rate << 16) / OUT_RATE.  ADR-0027 lowers it in
 * proportion to the pace target: when the emulator deliberately runs at, say,
 * 46.5 of the GBA's 59.7275 fps it also produces audio 46.5/59.7275 as fast,
 * so a consumer running at the nominal rate would drain the ring and emit
 * silence for ~22 % of every second.  Following the pace keeps the stream
 * CONTINUOUS at the cost of a proportional pitch drop — see ADR-0027 §audio
 * for why that beat the alternative. */
static volatile unsigned g_audio_step;

/* Audio correctness oracle.  The frame-dump oracle proves the VIDEO output of
 * a dynarec change is unaltered, and says nothing at all about sound — which
 * matters because GBA sound engines (M4A/Sappy) classically live in IWRAM and
 * SELF-MODIFY, patching per-channel volume/pitch immediates.  That is exactly
 * the code partial invalidation retires, so audio is the likeliest place for
 * a stale-jump bug to surface.  Rolling hash over every sample the core
 * produces, logged at exit; deterministic for a scripted run, so it compares
 * across builds the same way the frame hash does. */
static uint32_t g_audio_hash = 2166136261u;
static uint32_t g_audio_samples;

uint32_t plat_audio_hash(void)    { return g_audio_hash; }
uint32_t plat_audio_sample_count(void) { return g_audio_samples; }

static void plat_audio_frames(const int16_t *lr, size_t frames)
{
   size_t i;
   /* Hash BEFORE the FF/ring early-outs: this must describe what the core
    * generated, not what the output path happened to accept.  Bench-only —
    * it touches every sample the core produces, which is cheap but is pure
    * instrumentation and has no business running in a shipping build. */
   if (g_pcfg.bench_mode)
   {
      for (i = 0; i < frames * 2; i++)
         g_audio_hash = (g_audio_hash ^ (uint16_t)lr[i]) * 16777619u;
      g_audio_samples += (uint32_t)frames;
   }

   if ((g_ff_uncapped || g_ff_mult) && !g_pcfg.ff_audio)
      return;   /* FF mutes audio: ring drains to silence */
   for (i = 0; i < frames; i++)
   {
      if (ring_w - ring_r >= RING_FRAMES)
         break;                              /* full: drop the tail */
      {
         unsigned idx = ring_w & RING_MASK;
         audio_ring[idx * 2 + 0] = lr[i * 2 + 0];
         audio_ring[idx * 2 + 1] = lr[i * 2 + 1];
         ring_w++;
      }
   }
}

/* Feed the core's audio-buffer-status hook once per main-loop iteration
 * (ADR-0018, rescoped by ADR-0019).  It is a no-op unless the core is
 * actually running auto/auto_threshold frameskip — which, since ADR-0019,
 * a session no longer turns on by default: the field showed both consoles
 * holding real time (58.9 fps emulated) while `auto` quietly dropped
 * rendered frames on a transient buffer dip.  `active` is false while FF
 * mutes audio, so FF keeps its own policy. */
#define AUDIO_UNDERRUN_PCT 25

static void audio_status_frame(void)
{
   unsigned level = ring_w - ring_r;
   int pct = (int)((uint64_t)level * 100u / RING_FRAMES);
   int active = audio_running &&
                (g_pcfg.ff_audio || (!g_ff_uncapped && !g_ff_mult));
   fe_host_audio_buffer_status(active, pct, pct < AUDIO_UNDERRUN_PCT);
}

static int audio_thread(SceSize args, void *argp)
{
   static int16_t out[OUT_CHUNK * 2];
   unsigned step;
   unsigned frac = 0;
   int ch;

   (void)args; (void)argp;

   ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_CHUNK,
                          PSP_AUDIO_FORMAT_STEREO);
   if (ch < 0)
      return 0;

   while (audio_running)
   {
      int i;
      /* ADR-0027: re-read once per chunk (~23 ms), not per sample — the pace
       * target moves at most once a second and a chunk boundary is the
       * natural place to take the step change. */
      step = g_audio_step;
      for (i = 0; i < OUT_CHUNK; i++)
      {
         unsigned avail = ring_w - ring_r;
         if (avail < 2)
         {
            out[i * 2 + 0] = 0;              /* starved: silence */
            out[i * 2 + 1] = 0;
            continue;
         }
         {
            unsigned i0 = ring_r & RING_MASK;
            unsigned i1 = (ring_r + 1) & RING_MASK;
            int l0 = audio_ring[i0 * 2 + 0], l1 = audio_ring[i1 * 2 + 0];
            int r0 = audio_ring[i0 * 2 + 1], r1 = audio_ring[i1 * 2 + 1];
            int f = (int)(frac & 0xFFFF);
            out[i * 2 + 0] = (int16_t)(l0 + (((l1 - l0) * f) >> 16));
            out[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * f) >> 16));
         }
         frac += step;
         ring_r += frac >> 16;
         frac &= 0xFFFF;
      }
      sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, out);
   }

   sceAudioChRelease(ch);
   return 0;
}

static void audio_start(void)
{
   audio_running = 1;
   /* Nominal until the pace policy says otherwise (ADR-0027). */
   g_audio_step = (unsigned)(((uint64_t)in_rate << 16) / OUT_RATE);
   audio_thid = sceKernelCreateThread("gpsp_audio", audio_thread, 0x12,
                                      0x4000, THREAD_ATTR_USER, NULL);
   if (audio_thid >= 0)
      sceKernelStartThread(audio_thid, 0, NULL);
}

static void audio_stop(void)
{
   audio_running = 0;
   if (audio_thid >= 0)
   {
      sceKernelWaitThreadEnd(audio_thid, NULL);
      sceKernelDeleteThread(audio_thid);
      audio_thid = -1;
   }
}

/* ------------------------------------------------- core perf counters ---- */

/* ADR-0028.  gpsp's own monotonic counters, declared here rather than by
 * including the core's private headers (main.h / gba_memory.h are not on the
 * frontend include path, and frontend-common must stay core-agnostic — hence
 * the callback in fe_host_config).  Same boundary discipline as the
 * gpsp_rfu_link_down_hook weak symbol.
 *   flush_rom_total    ROM translation-cache flushes.  On this build
 *                      (SMALL_TRANSLATION_CACHE: 2 MiB ROM / 384 KiB RAM)
 *                      one of these discards up to 2 MiB of generated code,
 *                      memsets a 256 KiB branch-hash table, and forces every
 *                      subsequent block to be re-translated.
 *   flush_ram_full     RAM cache flushes caused by the RAM JIT cache running
 *                      out of room.  A bigger cache is the lever.
 *   flush_ram_smc      RAM cache flushes caused by a CPU store into RAM that
 *                      holds translated code.  The WHOLE RAM cache goes, plus
 *                      a memset of up to 288 KiB of SMC shadow, and a bigger
 *                      cache does nothing for these (ADR-0029).
 *   flush_ram_dma      the same, but triggered by a DMA transfer.
 *   gamepak_page_loads 32 KiB reads from the memory stick, mid-emulation,
 *                      when the ROM does not fit in RAM.  ADR-0026 priced
 *                      that stick at 34 ms for 4 KiB. */
extern u32 flush_rom_total;
extern u32 flush_ram_full;
extern u32 flush_ram_smc;
extern u32 flush_ram_dma;
extern u32 gamepak_page_loads;
/* What the flushes COST (main.h): time inside flush_translation_cache_ram(),
 * the tag-memset volume, and how often it took the whole-region else-branch
 * (32 KiB IWRAM / 256 KiB EWRAM).  smc_prof_clock is already installed below,
 * so these have been accumulating all along — they were just never shown. */
extern u32 smc_flush_us;
extern u32 smc_flush_bytes;
extern u32 smc_flush_wide;
extern u32 smc_blk_xlat_total;   /* RAM blocks re-translated (monotonic) */
extern u32 flush_rom_total;      /* ROM translation-cache wipes */
extern u32 cph_jit_total;        /* dynarec compilation us (monotonic) */
extern u32 smc_blk_watch;        /* 256-byte page the block/writer probe eyes */

static void plat_core_counters(unsigned *rom_flush, unsigned *ram_full,
                               unsigned *ram_smc, unsigned *ram_dma,
                               unsigned *page_load)
{
   *rom_flush = (unsigned)flush_rom_total;
   *ram_full  = (unsigned)flush_ram_full;
   *ram_smc   = (unsigned)flush_ram_smc;
   *ram_dma   = (unsigned)flush_ram_dma;
   *page_load = (unsigned)gamepak_page_loads;
}

/* ADR-0030: same boundary trick for the SMC write-ADDRESS profile.  The core
 * owns the buckets (main.c); this just forwards, and the take RESETS them, so
 * each `EVT smc_addr` describes exactly one heartbeat window. */
void smc_prof_take(unsigned *hits, unsigned *pages, unsigned *iw, unsigned *ew,
                   unsigned *oth, unsigned *ovf,
                   unsigned *hot_addr, unsigned *hot_cnt,
                   unsigned *top_addr, unsigned *top_cnt, unsigned nhot);

static void plat_smc_addr(unsigned *hits, unsigned *pages, unsigned *iw,
                          unsigned *ew, unsigned *oth, unsigned *ovf,
                          unsigned *hot_addr, unsigned *hot_cnt,
                          unsigned *top_addr, unsigned *top_cnt, unsigned nhot)
{
   smc_prof_take(hits, pages, iw, ew, oth, ovf, hot_addr, hot_cnt,
                 top_addr, top_cnt, nhot);
}

/* Phase 5g: same boundary trick for the SMC BLOCK profile (main.c). */
void smc_blk_take(unsigned *watch, unsigned *xlat, unsigned *ovf,
                  unsigned *start, unsigned *end, unsigned *mode,
                  unsigned *reason, unsigned *cnt,
                  unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                  unsigned *wovf,
                  unsigned *head, unsigned *ctx, unsigned *snap,
                  unsigned *fus, unsigned *fbytes, unsigned *fwide,
                  unsigned *same, unsigned *diff, unsigned *unkn);

static void plat_smc_block(unsigned *watch, unsigned *xlat, unsigned *ovf,
                           unsigned *start, unsigned *end, unsigned *mode,
                           unsigned *reason, unsigned *cnt,
                           unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                           unsigned *wovf,
                           unsigned *head, unsigned *ctx, unsigned *snap,
                           unsigned *fus, unsigned *fbytes, unsigned *fwide,
                           unsigned *same, unsigned *diff, unsigned *unkn)
{
   smc_blk_take(watch, xlat, ovf, start, end, mode, reason, cnt,
                wpc, waddr, wcnt, wovf, head, ctx, snap,
                fus, fbytes, fwide, same, diff, unkn);
}

/* The core's flush timer wants a plain u32-microsecond clock (main.h).
 * Declared here rather than including the core's main.h, which pulls in the
 * whole emulator header set -- same pattern as smc_prof_take above. */
extern unsigned int (*smc_prof_clock)(void);

static unsigned int plat_smc_clock(void)
{
   return (unsigned int)sceKernelGetSystemTimeLow();
}

/* Phase 5h: same boundary trick again for the per-PHASE frame bracket.  The
 * core owns the accumulators and the probe level (main.c); this forwards the
 * take, which RESETS them, so each `EVT core_phase` describes exactly one
 * heartbeat window.  `core_phase_set_level` is called after the clock is
 * installed -- the core refuses any nonzero level without one. */
void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max,
                     unsigned *worst, unsigned nph,
                     unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman,
                     unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut);
void core_phase_set_level(unsigned lvl);

static void plat_core_phase(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                            unsigned *reads, unsigned *mean, unsigned *max,
                            unsigned *worst, unsigned nph,
                            unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                            unsigned *wbkw, unsigned *ugba, unsigned *dman,
                            unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut)
{
   core_phase_take(lvl, clk_ns, frames, reads, mean, max, worst, nph,
                   bkr, bkw, wbkr, wbkw, ugba, dman, stmr, negc, nego, rfux, rfut, wrfux, wrfut);
}

/* ------------------------------------------------------------------ input */

static unsigned g_pad;   /* raw SceCtrl buttons, sampled once per loop */

static uint32_t plat_input_bitmask(void)
{
   /* Triangle (preset cycle) and Square (fast-forward) are frontend keys
    * now — they are NOT forwarded to the core (turbo A/B dropped; GBA has
    * no free buttons for them anyway, FRONTEND-AUDIT §4). */
   uint32_t m = 0;
   unsigned b = g_pad;
   if (b & PSP_CTRL_UP)       m |= 1u << JP_UP;
   if (b & PSP_CTRL_DOWN)     m |= 1u << JP_DOWN;
   if (b & PSP_CTRL_LEFT)     m |= 1u << JP_LEFT;
   if (b & PSP_CTRL_RIGHT)    m |= 1u << JP_RIGHT;
   /* A/B layout: default Circle=A/Cross=B (positional GBA match); the
    * settings "A/B buttons" toggle swaps them (btn_swap). */
   if (g_pcfg.btn_swap)
   {
      if (b & PSP_CTRL_CROSS)    m |= 1u << JP_A;   /* GBA A  */
      if (b & PSP_CTRL_CIRCLE)   m |= 1u << JP_B;   /* GBA B  */
   }
   else
   {
      if (b & PSP_CTRL_CIRCLE)   m |= 1u << JP_A;   /* GBA A  */
      if (b & PSP_CTRL_CROSS)    m |= 1u << JP_B;   /* GBA B  */
   }
   if (b & PSP_CTRL_LTRIGGER) m |= 1u << JP_L;
   if (b & PSP_CTRL_RTRIGGER) m |= 1u << JP_R;
   if (b & PSP_CTRL_START)    m |= 1u << JP_START;
   if (b & PSP_CTRL_SELECT)   m |= 1u << JP_SELECT;
   return m;
}

/* ------------------------------------------------------------------ video */

static const uint16_t *cur_frame;
static unsigned cur_w = FE_GBA_WIDTH, cur_h = FE_GBA_HEIGHT;
static size_t cur_pitch = FE_GBA_WIDTH * 2;

/* ---- ADR-0080 Stage 1: video staging on the Media Engine ---------------
 *
 * The pipeline the mission brief asked for, mapped onto the real seams:
 * the core's output buffer (`gba_screen_pixels`) is double-buffered by
 * flipping the core's own pointer between frames, so the main CPU never
 * copies a pixel.  Each completed frame is POSTED to the ME (non-blocking;
 * busy = drop, exactly the brief's collision policy), the ME does the
 * 480->512-stride staging copy into one of two main-RAM staging buffers,
 * and the GE draws the LAST COMPLETED staging buffer — one frame of
 * display latency, zero frames of main-CPU waiting.  The main-CPU path
 * (vid_draw_frame) is never removed: it serves until the first ME frame
 * completes and again forever if the watchdog trips.
 *
 * Coherency contract, one line each: SC writes core buffer cached ->
 * ranged writeback before post (in me_host_post_pitch_copy); ME reads and
 * writes uncached; GE reads physical RAM, which the ME's uncached stores
 * already reached. */
/* ADR-0080c: DEFER me_video ACTIVATION UNTIL A SESSION IS LIVE.
 *
 * me_video active on the HOST during np_start (ad-hoc group creation)
 * deterministically broke session bring-up (fs-me auto332/333, 2/2 on the
 * fixed EBOOT; the join was flawless).  The ME video path — buffer-pointer
 * flips and per-frame copy posts — must not run while the WLAN session is
 * being created.  It also has no reason to: single-player is unpaced with
 * ample headroom; the offload only matters once a paced session is up.
 *
 * So split "configured" from "active": `_cfg` is set once at boot (PRX up,
 * buffers allocated, key on); `g_me_video` (active offload) is switched on
 * only when g_net_up is true — which net_bringup sets AFTER np_start
 * succeeds — and switched back off (re-arming) when the session ends, so
 * the offload dodges EVERY session's np_start, not just the first.  A
 * watchdog/slow teardown is permanent for the boot (do not retry a dead
 * ME); a session-end teardown re-arms. */
static int g_net_up;                         /* adhoc transport + netdrv live
                                              * (defined here, above
                                              * plat_video_frame, for the
                                              * ADR-0080c session gate)      */
static int g_me_video;                       /* actively offloading now      */
static int g_me_video_cfg;                   /* configured + ME ready        */
/* ADR-0080e: NO second core buffer.  We post the copy straight from the
 * core's own frame (`pix`); the ME reads it in ~3 ms (uncached 160 KB) while
 * the core does not re-render for ~16 ms, and the drop policy never lets two
 * copies overlap — so there is no overwrite race and the 77 KB second buffer
 * (which the host could not fit alongside np_start, auto346) is gone.  Two
 * 82 KB staging buffers remain (GE draws the last-completed while the ME
 * fills the next). */
static uint16_t *g_me_stage[2];              /* 256x161 RGB565 staging       */
static int g_me_stage_ready   = -1;          /* GE may draw this one         */
static int g_me_stage_pending = -1;          /* ME is filling this one       */
static unsigned g_me_vf, g_me_vdrops, g_me_census;
/* ADR-0080b: consecutive frames a posted copy failed to finish within its
 * frame.  This is the REAL "ME too slow for video" signal — distinct from
 * the host watchdog's heartbeat-freeze (ME wedged).  The first me_video
 * run (auto330) tore down at 8 because the OLD watchdog checked idle in the
 * same frame it posted the job — always busy, always a false miss. */
static unsigned g_me_vmiss;
#define ME_VMISS_LIMIT 8
static unsigned g_me_pw, g_me_ph;            /* frame dims for present (ADR-0082) */
extern u16 *gba_screen_pixels;               /* core global (video.h)        */

/* `permanent` = 1 kills the offload for the rest of the boot (a dead/too-slow
 * ME); 0 just deactivates and RE-ARMS for the next session (ADR-0080c). */
static void me_video_teardown(const char *why, int permanent)
{
   if (!g_me_video)
   {
      if (permanent)
         g_me_video_cfg = 0;
      return;
   }
   g_me_video = 0;
   if (permanent)
      g_me_video_cfg = 0;
   /* No gba_screen_pixels to restore — ADR-0080e never reassigns it. */
   g_me_stage_ready = g_me_stage_pending = -1;
   g_me_vmiss = 0;
   fe_evt("me_video off reason=%s frames=%u drops=%u rearm=%d", why, g_me_vf,
          g_me_vdrops, permanent ? 0 : 1);
}

/* ADR-0080d: allocate the offload buffers LAZILY, at session-up — NOT at
 * boot.  The host's np_start (ad-hoc group creation) needs more heap than
 * the join's, and pre-allocating 242 KB at boot starved it: the host failed
 * np_start even with me_video merely ARMED (session-gated, never active) —
 * fs-me auto343 disproved the "activity collides" theory.  Allocating after
 * np_start has taken its memory reverts the host to the exact heap profile
 * of the working me_bench arm at the one moment that matters.  On failure
 * (memory genuinely too tight post-session) the console just runs plain —
 * graceful, never fatal. */
static int me_video_alloc(void)
{
   if (g_me_stage[0] && g_me_stage[1])
      return 0;   /* already allocated (a prior session this boot) */
   g_me_stage[0] = (uint16_t *)memalign(64, 512 * 161);
   g_me_stage[1] = (uint16_t *)memalign(64, 512 * 161);
   if (!g_me_stage[0] || !g_me_stage[1])
   {
      free(g_me_stage[0]); free(g_me_stage[1]);
      g_me_stage[0] = NULL; g_me_stage[1] = NULL;
      return -1;
   }
   memset(g_me_stage[0], 0, 512 * 161);
   memset(g_me_stage[1], 0, 512 * 161);
   sceKernelDcacheWritebackInvalidateRange(g_me_stage[0], 512 * 161);
   sceKernelDcacheWritebackInvalidateRange(g_me_stage[1], 512 * 161);
   return 0;
}

/* Turn the offload ON for a live session (ADR-0080c/d/e).  Allocate the two
 * staging buffers now (post-np_start); no second core buffer to flip. */
static void me_video_activate(void)
{
   if (me_video_alloc() != 0)
   {
      fe_evt("me_video off reason=alloc session=up");
      g_me_video_cfg = 0;                 /* cannot get buffers: give up */
      return;
   }
   g_me_stage_ready   = -1;
   g_me_stage_pending = -1;
   g_me_vmiss         = 0;
   g_me_video = 1;
   fe_evt("me_video on stage_bytes=%u session=up", 512u * 161u);
}

/* ADR-0082: the offload is now a two-stage pipeline split across the loop.
 *
 *   me_video_post()  — end of retro_run (from plat_video_frame): hand this
 *                      frame to the ME to stage.  NO GE work here.
 *   me_video_present()— TOP of the next loop iteration, BEFORE retro_run:
 *                      retire the just-staged buffer and issue its GE list.
 *
 * Because the list is issued at the top, the GE rasterises through the whole
 * ~12 ms retro_run, so the pre-swap sync is ~0 even on heavy animation
 * frames (the old end-of-run issue left the GE only the vblank slack, which
 * is zero when a frame is over budget — that was the reappearing ~1.3 ms
 * `wait`).  Still ONE frame of latency: a frame posted at the end of run N
 * is presented at the top of run N+1 and shown at swap N+1.  Safe only
 * because the ME copy is now the ~0.7 ms cached read (ADR-0082 PRX change),
 * which finishes inside the vblank gap between post and present. */
static void me_video_post(const uint16_t *pix, unsigned w, unsigned h,
                          size_t pitch)
{
   g_me_vf++;
   g_me_pw = w;
   g_me_ph = h;

   /* Post the new frame if the ME is free (no job in flight). */
   if (g_me_video && pix && g_me_stage_pending < 0)
   {
      int nxt = (g_me_stage_ready == 0) ? 1 : 0;
      if (me_host_post_pitch_copy(pix, g_me_stage[nxt], h, w * 2,
                                  (unsigned)pitch, 512) == 0)
         g_me_stage_pending = nxt;
      else
         g_me_vdrops++;
   }
   else if (g_me_video && pix)
      g_me_vdrops++;

   if (++g_me_census >= 600)
   {
      g_me_census = 0;
      fe_evt("me_video frames=%u drops=%u vmiss=%u beat_ok=%d", g_me_vf,
             g_me_vdrops, g_me_vmiss, me_host_up());
   }
}

/* Called at the TOP of the main loop, before fe_host_run_frame (ADR-0082). */
static void me_video_present(void)
{
   if (!g_me_video)
      return;

   /* Retire the copy posted last iteration.  The cached-read copy (~0.7 ms)
    * completed during the vblank gap; if it somehow did not (a huge stall),
    * keep the previous frame on screen (a repeat) and count a miss. */
   if (g_me_stage_pending >= 0)
   {
      if (me_host_idle())
      {
         g_me_stage_ready   = g_me_stage_pending;
         g_me_stage_pending = -1;
         g_me_vmiss = 0;
      }
      else if (++g_me_vmiss >= ME_VMISS_LIMIT)
      {
         me_video_teardown("slow", 1);
         return;
      }
   }

   /* Issue the GE list now — it rasterises through the whole retro_run. */
   if (g_me_stage_ready >= 0)
   {
      vid_draw_prestaged(g_me_stage[g_me_stage_ready], g_me_pw, g_me_ph);
      g_drew = 1;
   }

   if (g_me_video && me_host_watchdog_frame())
      me_video_teardown("watchdog", 1);   /* ME wedged (heartbeat frozen) */
}

/* ---- MEDIA ENGINE MODE: the live second-core renderer -------------------
 *
 * The user-facing engine (settings > "Media Engine mode", relaunch-to-apply;
 * harness override `me_mode`).  Pipeline, one frame of latency:
 *
 *   emulate N   : core runs in capture mode 1 — logs the per-line LCD regs
 *                 into g_cap[cur] and SKIPS the render (~6 ms stays here).
 *   frame end N : retire the ME's render of N-1 -> present it; write back
 *                 caches; post render(N) with capture=g_cap[cur]; wait for
 *                 input_seq (ME has copied vram/oam/palette, ~1 ms); flip
 *                 the capture buffer; emulation of N+1 proceeds while the
 *                 ME renders N in parallel (~10 ms vs the 16.7 ms budget).
 *
 * Correctness lineage: capture set proven bit-exact on the desktop
 * (RENDER_REPLAY ~81k + ME_CAP_VALIDATE ~61k frames, 0 mismatches); the
 * ~1.7% mid-frame-DMA frames render from end-of-frame state — a one-frame
 * transient during scene transitions, accepted for v1.
 *
 * Failure discipline (same as me_video): misses counted, never fatal —
 * teardown returns to main-CPU rendering mid-run, seamlessly. */
extern unsigned char  vram[];
extern unsigned short io_registers[];
extern unsigned short oam_ram[];
extern unsigned short palette_ram_converted[];
extern unsigned int   reg[];

/* MUST equal video_psp.c's STAGE_STRIDE (256 px = 512-byte rows): the GE
 * texture reads rows at that spacing.  The first engine run shipped 512 px
 * here — a 2x stride mismatch, and the user's screen showed the textbook
 * signature: alternating black scanlines + the bottom half of the frame
 * falling off (each texture row pair = one real row + the empty half). */
#define MER_STAGE_PITCH   256            /* GE texture stride, pixels */
#define MER_RETIRE_US     9000           /* spin budget for a late ME frame */
#define MER_INPUT_US      50000          /* input handshake hard ceiling */
#define MER_MISS_LIMIT    120            /* consecutive drops -> teardown */

static int g_me_rend_cfg;                /* wanted (pcfg/harness) */
static int g_me_rend;                    /* active */
static me_capture_frame *g_mer_cap[2];
static uint16_t *g_mer_stage[2];
static me_render_desc *g_mer_desc;
/* Engine buffers live in the PSP's VOLATILE MEMORY when available — the 4 MB
 * OS pool at 0x08400000 that games may lock and gpsp never touches.  Zero
 * main-heap footprint, so np_start never contends with the engine: the host
 * heap has NO margin (np_start passed with the engine fully freed, run 411,
 * and failed with just the 41 KB captures resident, run 415-era).  With
 * volatile backing the suspend/resume dance is unnecessary and skipped.
 * Fallback when the lock fails: main heap + the suspend/resume path. */
static int g_mer_vmem;
static int g_mer_cur;                    /* capture buffer the core fills */
static int g_mer_pending = -1;           /* stage being rendered by the ME */
static int g_mer_ready   = -1;           /* stage ready to present */
static unsigned g_mer_frames, g_mer_drops, g_mer_miss;
static unsigned g_mer_wait_us, g_mer_wait_max, g_mer_census;

static void me_rend_teardown(const char *why)
{
   if (!g_me_rend)
      return;
   g_me_rend = 0;
   me_capture_mode = 0;                  /* core renders on the CPU again */
   me_capture_buf  = NULL;
   g_mer_pending = g_mer_ready = -1;
   fe_evt("me_rend off reason=%s frames=%u drops=%u", why,
          g_mer_frames, g_mer_drops);
}

static int me_rend_init(void);   /* defined below */

/* ADR-0080d, applied to the engine: the host's np_start needs a clean heap.
 * The dual-engine gate run (host run 407) confirmed it — the engine's ~205 KB
 * boot allocation starved session creation and the host exited net_failed.
 * So the engine SUSPENDS around wireless bring-up: teardown + FREE everything,
 * let np_start claim its memory, then re-init from the post-session heap
 * (the exact profile me_video validated).  The CPU renders during the connect
 * screen; a failed re-alloc afterwards is the usual graceful CPU fallback. */
static void me_rend_suspend(void)
{
   int i;
   /* Volatile-backed: the engine owns no main heap, np_start is unaffected —
    * keep rendering straight through the wireless bring-up. */
   if (g_mer_vmem)
      return;
   if (g_me_rend)
      me_rend_teardown("np_start");
   /* Wait out any in-flight ME job before freeing the buffers under it. */
   if (me_host_up())
   {
      unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
      while (!me_host_idle() &&
             ((unsigned)sceKernelGetSystemTimeLow() - t0) < 50000u)
         ;
   }
   /* Heap-backed fallback: free EVERYTHING.  The 41KB-resident experiment
    * failed on hardware — the host's np_start needs the heap fully clean
    * (passed at 0 KB held, run 411; failed at 41 KB held).  Resume may then
    * fail on a fragmented post-session heap; that degrades the host to CPU
    * rendering, which is safe.  The volatile path above makes all of this
    * moot wherever the lock succeeds. */
   for (i = 0; i < 2; i++)
   {
      free(g_mer_cap[i]);   g_mer_cap[i]   = NULL;
      free(g_mer_stage[i]); g_mer_stage[i] = NULL;
   }
   free(g_mer_desc); g_mer_desc = NULL;
}

static void me_rend_resume(void)
{
   if (g_me_rend_cfg && !g_me_rend && me_host_up())
      if (me_rend_init() != 0)
         fe_evt("me_rend resume_failed (CPU rendering)");
}

static int me_rend_init(void)
{
   int i;
   if (!me_host_up())
      return -1;
   /* First choice: carve everything from volatile memory (see g_mer_vmem). */
   if (!g_mer_vmem && !g_mer_cap[0])
   {
      void *vbase = NULL;
      int vsize = 0;
      if (sceKernelVolatileMemTryLock(0, &vbase, &vsize) == 0 &&
          vbase && vsize >= (int)(2 * 20544 + 2 * (MER_STAGE_PITCH*161*2) + 128))
      {
         unsigned p = ((unsigned)vbase + 63u) & ~63u;
         g_mer_desc     = (me_render_desc *)p;   p += 128;
         g_mer_cap[0]   = (me_capture_frame *)p; p += 20544;
         g_mer_cap[1]   = (me_capture_frame *)p; p += 20544;
         g_mer_stage[0] = (uint16_t *)p;         p += MER_STAGE_PITCH*161*2;
         g_mer_stage[1] = (uint16_t *)p;
         g_mer_vmem = 1;
         fe_evt("me_rend vmem base=%08x size=%d", (unsigned)vbase, vsize);
      }
   }
   for (i = 0; i < 2; i++)
   {
      if (!g_mer_cap[i])
         g_mer_cap[i] = (me_capture_frame *)memalign(64, sizeof(me_capture_frame));
      if (!g_mer_stage[i])
         g_mer_stage[i] = (uint16_t *)memalign(64, MER_STAGE_PITCH * 161 * 2);
   }
   if (!g_mer_desc)
      g_mer_desc = (me_render_desc *)memalign(64, sizeof(me_render_desc));
   if (!g_mer_cap[0] || !g_mer_cap[1] || !g_mer_stage[0] || !g_mer_stage[1] ||
       !g_mer_desc)
   {
      fe_evt("me_rend off reason=alloc");
      return -1;
   }
   memset(g_mer_cap[0], 0, sizeof(me_capture_frame));
   memset(g_mer_cap[1], 0, sizeof(me_capture_frame));
   memset(g_mer_stage[0], 0, MER_STAGE_PITCH * 161 * 2);
   memset(g_mer_stage[1], 0, MER_STAGE_PITCH * 161 * 2);
   sceKernelDcacheWritebackInvalidateRange(g_mer_stage[0], MER_STAGE_PITCH*161*2);
   sceKernelDcacheWritebackInvalidateRange(g_mer_stage[1], MER_STAGE_PITCH*161*2);
   g_mer_cur = 0;
   g_mer_pending = g_mer_ready = -1;
   me_capture_buf  = g_mer_cap[0];
   me_capture_mode = 1;                  /* core stops rendering NOW */
   g_me_rend = 1;
   fe_evt("me_rend on stage_bytes=%u cap_bytes=%u",
          (unsigned)(MER_STAGE_PITCH * 161 * 2) * 2,
          (unsigned)sizeof(me_capture_frame) * 2);
   return 0;
}

/* Write back the live inputs and fill the render desc for stage `out`.
 * Shared by the async pipeline and the FF synchronous path. */
static void me_rend_fill_desc(int out)
{
   sceKernelDcacheWritebackRange(vram, 1024 * 96);
   sceKernelDcacheWritebackRange(oam_ram, 512 * 2);
   sceKernelDcacheWritebackRange(palette_ram_converted, 512 * 2);
   sceKernelDcacheWritebackRange(g_mer_cap[g_mer_cur],
                                 sizeof(me_capture_frame));
   g_mer_desc->vram      = (unsigned)vram;
   g_mer_desc->oam       = (unsigned)oam_ram;
   g_mer_desc->palette   = (unsigned)palette_ram_converted;
   g_mer_desc->capture   = (unsigned)g_mer_cap[g_mer_cur];
   g_mer_desc->out       = (unsigned)g_mer_stage[out];
   g_mer_desc->out_pitch = MER_STAGE_PITCH;
   sceKernelDcacheWritebackRange(g_mer_desc, sizeof(*g_mer_desc));
}

/* FAST-FORWARD presentation.  The async pipeline (post N, present N at the
 * end of N+1) starves during FF: the FF fast-paths stop pumping it, the GE
 * holds the last pre-FF frame, and the player "teleports" on release.  So
 * while FF is engaged we run a SYNCHRONOUS cycle on a wall-clock cadence —
 * every ~33 ms: post this frame's capture, wait out the ~10 ms render,
 * present immediately.  Display runs at ~30 fps while emulation sprints;
 * the sync waits cost FF roughly a quarter of its throughput, which beats
 * a frozen screen.  Leaves nothing in flight, so the async pipeline
 * resumes seamlessly the moment FF is released. */
#define MER_FF_CADENCE_US 33000u
static unsigned g_mer_ff_last_us;
/* FF-path probe (2x-freeze hunt): count every way this function can decline
 * to present, and log the census periodically.  Harness builds only pay the
 * evt; the counters are near-free. */
static unsigned g_ffp_calls, g_ffp_nemu, g_ffp_cad, g_ffp_stuck, g_ffp_postf,
                g_ffp_late, g_ffp_ok;

static void me_rend_ff_probe(void)
{
   if ((g_ffp_calls & 127u) == 1u)
      fe_evt("me_ffp calls=%u nemu=%u cad=%u stuck=%u postf=%u late=%u ok=%u"
             " mult=%d uncap=%d",
             g_ffp_calls, g_ffp_nemu, g_ffp_cad, g_ffp_stuck, g_ffp_postf,
             g_ffp_late, g_ffp_ok, g_ff_mult, g_ff_uncapped);
}

static void me_rend_ff_frame(int emulated)
{
   unsigned now = (unsigned)sceKernelGetSystemTimeLow();
   int out;
   unsigned t0;

   g_ffp_calls++;
   me_rend_ff_probe();
   if (!emulated || !me_host_up())
      { g_ffp_nemu++; return; }
   if (now - g_mer_ff_last_us < MER_FF_CADENCE_US)
      { g_ffp_cad++; return; }

   /* Drain any async post left over from before FF engaged. */
   if (g_mer_pending >= 0)
   {
      t0 = now;
      while (!me_host_idle() &&
             ((unsigned)sceKernelGetSystemTimeLow() - t0) < MER_RETIRE_US)
         ;
      if (!me_host_idle())
         { g_ffp_stuck++; return; }  /* still busy — try next cadence tick */
      g_mer_ready   = g_mer_pending;
      g_mer_pending = -1;
   }

   /* Synchronous render of THIS frame's capture.  The full-completion wait
    * subsumes the input_seq handshake: emulation cannot race the ME's reads
    * because we do not return until the render is done. */
   out = (g_mer_ready == 0) ? 1 : 0;
   me_rend_fill_desc(out);
   if (me_host_post_render((unsigned)ME_UNCACHED(g_mer_desc)) != 0)
      { g_ffp_postf++; return; }
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle() &&
          ((unsigned)sceKernelGetSystemTimeLow() - t0) < 30000u)
      ;
   if (!me_host_idle())
   {
      g_ffp_late++;
      g_mer_drops++;                 /* ME late; the watchdog covers wedges */
      return;
   }
   g_ffp_ok++;
   g_mer_ready      = out;
   g_mer_ff_last_us = (unsigned)sceKernelGetSystemTimeLow();
   /* NO draw here: presentation happens at loop-top (me_rend_present), in
    * phase with the swap.  Drawing from inside the emulation callback lands
    * on whichever framebuffer happens to be the target — frozen or shimmering
    * displays depending on FF mode (the 2x/3x/uncapped triage). */
}

/* Loop-top presentation for Media Engine mode — the ADR-0082 discipline:
 * draw the newest finished stage once per host frame, right before the swap,
 * so BOTH display buffers always carry the current frame.  Serves the async
 * pipeline and the FF synchronous path alike. */
static void me_rend_present(void)
{
   if (!g_me_rend || g_mer_ready < 0)
      return;
   vid_draw_prestaged(g_mer_stage[g_mer_ready], 240, 160);
   g_drew = 1;
}

/* Called from plat_video_frame at the end of every emulated frame.
 * `emulated` is false for duped/skipped frames (no capture happened). */
/* Complete frames actually rendered in the current fps window (ME: a retired
 * render; CPU path: a frame the core really drew).  Reported beside the
 * emulated rate so a frameskipped or ME-outrun measurement can never read as
 * if every frame were being produced. */
static unsigned g_fps_drawn;

static void me_rend_frame(int emulated)
{
   unsigned t0, t1;

   if (!g_me_rend)
      return;
   g_mer_frames++;

   /* 1. Retire the previous post (render of frame N-1).  A full frame has
    *    elapsed since the post, so the ~10 ms render is normally long done;
    *    spin only for a genuinely late one. */
   if (g_mer_pending >= 0)
   {
      t0 = (unsigned)sceKernelGetSystemTimeLow();
      while (!me_host_idle() &&
             ((unsigned)sceKernelGetSystemTimeLow() - t0) < MER_RETIRE_US)
         ;
      if (me_host_idle())
      {
         g_mer_ready   = g_mer_pending;
         g_mer_pending = -1;
         g_mer_miss    = 0;
         g_fps_drawn++;             /* a complete rendered frame landed */
      }
      else
      {
         /* ME still busy: drop this frame (present the old one), do NOT
          * flip the capture buffer — the next emulated frame overwrites
          * the capture and we try again. */
         g_mer_drops++;
         /* Any fast-forward outruns the ME by design — drops there are the
          * expected outcome, not a sick renderer, and a teardown would
          * silently switch rendering paths mid-FF (or mid-measurement).
          * Normal-speed lateness still trips the watchdog. */
         if (++g_mer_miss >= MER_MISS_LIMIT &&
             !g_ff_uncapped && !g_ff_mult && !g_pcfg.bench_mode)
            me_rend_teardown("late");
         goto present;
      }
   }

   /* 2. Post this frame's render (only if the core actually captured it). */
   if (emulated && me_host_up())
   {
      int out = (g_mer_ready == 0) ? 1 : 0;   /* render into the other stage */
      me_rend_fill_desc(out);
      if (me_host_post_render((unsigned)ME_UNCACHED(g_mer_desc)) == 0)
      {
         /* 3. Wait for the ME to consume the live arrays (~1 ms), then flip
          *    the capture buffer so emulation of the next frame is free. */
         t0 = (unsigned)sceKernelGetSystemTimeLow();
         while (!me_host_input_done() &&
                ((t1 = (unsigned)sceKernelGetSystemTimeLow()) - t0) < MER_INPUT_US)
            ;
         t1 = (unsigned)sceKernelGetSystemTimeLow() - t0;
         g_mer_wait_us += t1;
         if (t1 > g_mer_wait_max)
            g_mer_wait_max = t1;
         if (!me_host_input_done())
         {
            me_rend_teardown("input_wedge");   /* watchdog-class failure */
            goto present;
         }
         g_mer_pending = out;
         g_mer_cur ^= 1;
         me_capture_buf = g_mer_cap[g_mer_cur];
      }
      else
         g_mer_drops++;
   }

present:
   /* 4. Presentation moved to loop-top (me_rend_present) — swap-phase
    * discipline; see the FF triage note above. */
   if (g_me_rend && me_host_watchdog_frame())
      me_rend_teardown("watchdog");

   if (++g_mer_census >= 600)
   {
      fe_evt("me_rend frames=%u drops=%u wait_mean_us=%u wait_max_us=%u",
             g_mer_frames, g_mer_drops, g_mer_wait_us / g_mer_census,
             g_mer_wait_max);
      g_mer_census = 0;
      g_mer_wait_us = g_mer_wait_max = 0;
   }
}

/* FPS counter (Settings -> "FPS counter").  Counts EMULATED frames — the
 * increment sits above the blit-suppress and FF-cadence returns, so during
 * multiplier/uncapped FF the chip reads the true emulation speed (~179 at
 * 3x), not the ~30 Hz presentation cadence. */
static unsigned g_fps_emu_frames, g_fps_win_us, g_fps_last_pg, g_fps_last_smc,
                g_fps_last_dma, g_fps_last_fus, g_fps_last_fkb, g_fps_last_wide,
                g_fps_last_xlat, g_fps_last_rom, g_fps_last_jit;
static int g_autoload_state;   /* harness `load_state = 1`: boot into .st0 */

/* ROM-cache stats from gba_memory.c (declared here rather than pulling the
 * core's header chain into the frontend TU; gamepak_page_loads is already
 * declared with the perf stats above). */
extern u32 gamepak_size;
extern u32 gamepak_buffer_count;
int gamepak_must_swap(void);

static void plat_video_frame(const uint16_t *pix, unsigned w, unsigned h,
                             size_t pitch)
{
   static int rom_cache_logged;
   if (!rom_cache_logged)
   {
      /* One-shot: how much of the cart the ROM cache actually holds.  With
       * PSP_LARGE_MEMORY on a 2000+ this should read resident=1 even for
       * 32 MB carts; on a 1000 a 32 MB cart pages (resident=0). */
      rom_cache_logged = 1;
      fe_evt("rom_cache blocks=%u rom=%uKB resident=%d",
             gamepak_buffer_count, gamepak_size >> 10, !gamepak_must_swap());
   }
   g_fps_emu_frames++;
   if (pix)
   {
      cur_frame = pix;
      cur_w = w;
      cur_h = h;
      cur_pitch = pitch;
   }
   if (g_blit_suppress)
      return;   /* intermediate multiplier-FF frame */

   /* Media Engine mode: the second core renders; this frame's pixels came
    * from the capture pipeline, not `pix` (which the core never wrote).
    * FF gets its own synchronous cadence, and deliberately BYPASSES the
    * uncapped present mask below — ff_frame's wall-clock throttle is the
    * cadence authority (the mask capped the display at ~15 fps). */
   if (g_me_rend)
   {
      /* Smooth/bench FF stays on the ASYNC path even while uncapped: the
       * point is to keep the parallel pipeline running, and the synchronous
       * FF path both blocks on the ME and throttles rendering to a 33 ms
       * cadence (which is exactly what makes ordinary FF look choppy). */
      if ((g_ff_uncapped || g_ff_mult) &&
          !(g_pcfg.ff_smooth || g_pcfg.bench_mode))
         me_rend_ff_frame(pix != NULL);
      else
         me_rend_frame(pix != NULL);
      return;
   }

   /* Uncapped FF (CPU path): blit only the latest frame at a low cadence. */
   if (g_ff_uncapped && (fe_host_frame_count() & FF_PRESENT_MASK))
      return;

   /* ADR-0080c: session-gated activation.  Offload runs ONLY while a WLAN
    * session is live — which begins AFTER np_start, so the ME video path
    * never touches ad-hoc group creation, on host or join alike.  Re-arms
    * every session; a permanent teardown (dead/slow ME) leaves _cfg clear
    * so this cannot flip it back on. */
   if (g_me_video_cfg)
   {
      if (!g_me_video && g_net_up)
         me_video_activate();
      else if (g_me_video && !g_net_up)
         me_video_teardown("session_end", 0);   /* re-arm for next session */
   }

   if (g_me_video)
   {
      /* ADR-0082: post only — the GE list is issued at the TOP of the next
       * loop iteration (me_video_present), not here. */
      me_video_post(pix, w, h, pitch);
      return;
   }
   /* NULL = duped/skipped frame: re-draw the previous one (double-buffered
    * display needs every frame drawn). */
   if (cur_frame)
   {
      vid_draw_frame(cur_frame, cur_w, cur_h, cur_pitch);
      g_drew = 1;
      if (pix)
         g_fps_drawn++;      /* a new image, not a re-present of the old one */
   }
}

/* ------------------------------------------------------------------- misc */

static int find_first_rom(char *out, size_t out_sz)
{
   SceUID d = sceIoDopen(ROM_DIR);
   SceIoDirent ent;
   int found = 0;

   if (d < 0)
      return -1;
   memset(&ent, 0, sizeof(ent));
   while (sceIoDread(d, &ent) > 0)
   {
      size_t n = strlen(ent.d_name);
      if (n > 4 && strcasecmp(ent.d_name + n - 4, ".gba") == 0)
      {
         snprintf(out, out_sz, "%s/%s", ROM_DIR, ent.d_name);
         found = 1;
         break;
      }
      memset(&ent, 0, sizeof(ent));
   }
   sceIoDclose(d);
   return found ? 0 : -1;
}

static void make_suffixed_path(const char *rom, const char *suffix,
                               char *out, size_t out_sz)
{
   size_t n = strlen(rom);
   const char *dot = strrchr(rom, '.');
   if (dot)
      n = (size_t)(dot - rom);
   if (n > out_sz - strlen(suffix) - 1)
      n = out_sz - strlen(suffix) - 1;
   memcpy(out, rom, n);
   strcpy(out + n, suffix);
}

static void dump_frame_bmp(void)
{
   size_t pitch;
   const uint16_t *pix = fe_host_last_frame(&pitch);
   char path[176];
   /* Media Engine mode: the core buffer is never written (capture mode skips
    * the render) — dump the frame the ME actually presented instead, so
    * screenshots show what the screen shows. */
   if (g_me_rend && g_mer_ready >= 0)
   {
      pix   = g_mer_stage[g_mer_ready];
      pitch = MER_STAGE_PITCH * 2;   /* fe_host_last_frame pitch is BYTES —
                                      * passing pixels here interleaved the
                                      * left/right halves of every row in the
                                      * first ME-mode dumps (display was fine) */
   }
   if (!pix)
      return;
   snprintf(path, sizeof(path), "%s/frame_%06u.bmp", LOG_DIR,
            fe_host_frame_count());
   /* Decode the core buffer with the layout the core was BUILT to emit
    * (ADR-0039), or every dumped BMP — and the harness comparisons built on
    * them — comes out with R and B swapped. */
#ifdef USE_PSP_RGB565_FORMAT
   if (fe_bmp_write_psp565(path, pix, FE_GBA_WIDTH, FE_GBA_HEIGHT, pitch) == 0)
#else
   if (fe_bmp_write_rgb565(path, pix, FE_GBA_WIDTH, FE_GBA_HEIGHT, pitch) == 0)
#endif
      fe_evt("frame_dump file=%s", path);
}

/* ADR-0033: per-frame hash of the CORE's output buffer, taken upstream of the
 * GU blit (fe_host_last_frame is the pointer retro_video_refresh handed us).
 * This is the video regression oracle: a renderer change that alters ONE
 * pixel changes the hash.  FNV-1a over the 240x160 halfwords — position- and
 * bit-sensitive, ~38k iterations, and only ever run when a harness sets
 * autopilot.ini's vhash=1.  Zero cost otherwise. */
static void vhash_frame(void)
{
   size_t pitch;
   const uint16_t *pix = fe_host_last_frame(&pitch);
   uint32_t h = 2166136261u;
   unsigned y;
   if (!pix)
      return;
   for (y = 0; y < FE_GBA_HEIGHT; y++)
   {
      const uint16_t *row = (const uint16_t *)
                            ((const uint8_t *)pix + (size_t)y * pitch);
      unsigned x;
      for (x = 0; x < FE_GBA_WIDTH; x++)
      {
         h ^= row[x];
         h *= 16777619u;
      }
   }
   fe_evt("vhash f=%u h=%08x", fe_host_frame_count(), h);
}

/* ADR-0034: read the core renderer's per-path profile out once per window.
 * Emits the window MEAN of every bucket plus the breakdown of the single
 * worst scanline-render frame — the phase maxima must never be summed
 * (ADR-0032), so `vid_worst` is what a max is allowed to be read against.
 * All zero (and n==0) unless the core was built with -DVIDEO_PROF=1. */
static unsigned vp_sum[VP_COUNT], vp_worst[VP_COUNT], vp_max_tot, vp_win_n;
static unsigned vp_core_mean;
static unsigned vp_build_crc;   /* CRC32 of the EBOOT actually running */

/* ADR-0035: identify the BINARY in the profile line, not just the run.
 * A stale EBOOT in a sandbox produced two "different" builds with identical
 * timings and cost most of a session to spot.  A profile whose build cannot be
 * identified is not a measurement, so the CRC of the running EBOOT goes in
 * every `vid_prof` line and is emitted once at boot.  ~1.6 MiB of CRC, only
 * when profiling is enabled. */
static unsigned eboot_crc(void)
{
   char path[160];
   static unsigned char buf[4096];
   SceUID fd;
   unsigned crc = 0, total = 0;
   int n;

   snprintf(path, sizeof(path), "%s/EBOOT.PBP", g_dir_base);
   fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
   if (fd < 0)
      return 0;
   while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0)
   {
      crc = fe_crc32(crc, buf, (size_t)n);
      total += (unsigned)n;
   }
   sceIoClose(fd);
   fe_evt("build eboot_crc=%08x size=%u", crc, total);
   return crc;
}

static void vid_prof_frame(unsigned win)
{
   unsigned cur[VP_COUNT];
   char line[768];
   int n = video_prof_read_reset(cur, VP_COUNT);
   int i, off = 0;

   if (n <= 0)
      return;
   for (i = 0; i < n; i++)
      vp_sum[i] += cur[i];
   if (cur[VP_T_TOTAL] >= vp_max_tot)
   {
      vp_max_tot = cur[VP_T_TOTAL];
      for (i = 0; i < n; i++)
         vp_worst[i] = cur[i];
   }
   if (++vp_win_n < win)
      return;

   /* Cross-check against the whole retro_run call over the SAME window, so
    * the renderer's share is a ratio of two measurements rather than of one
    * measurement and a remembered number (ADR-0032). */
   {
      static unsigned prev_calls;
      static uint32_t prev_total;
      unsigned calls = 0; uint32_t total = 0, mx = 0;
      unsigned d;
      fe_host_core_prof(&calls, &total, &mx);
      d = calls - prev_calls;
      vp_core_mean = d ? (total - prev_total) / d : 0;
      prev_calls = calls; prev_total = total;
   }

   for (i = 0; i < n && off < (int)sizeof(line) - 24; i++)
      off += snprintf(line + off, sizeof(line) - off, "%s=%u ",
                      video_prof_name(i), vp_sum[i] / win);
   fe_evt("vid_prof f=%u bld=%08x core=%u tmax=%u %s", win, vp_build_crc,
          vp_core_mean, vp_max_tot, line);

   off = 0;
   for (i = 0; i < n && off < (int)sizeof(line) - 24; i++)
      off += snprintf(line + off, sizeof(line) - off, "%s=%u ",
                      video_prof_name(i), vp_worst[i]);
   fe_evt("vid_worst %s", line);

   memset(vp_sum, 0, sizeof(vp_sum));
   memset(vp_worst, 0, sizeof(vp_worst));
   vp_max_tot = 0;
   vp_win_n   = 0;
}

static void dump_ge_bmp(void)
{
   char path[176];
   snprintf(path, sizeof(path), "%s/ge_%06u.bmp", LOG_DIR,
            fe_host_frame_count());
   if (vid_dump_ge(path) == 0)
      fe_evt("ge_dump file=%s w=%d h=%d", path, VID_SCR_W, VID_SCR_H);
   else
      fe_evt("ge_dump file=%s FAILED", path);
}

static int file_exists(const char *path)
{
   SceIoStat st;
   return sceIoGetstat(path, &st) >= 0;
}

/* Pre-session save backup (plan §4.5): copy <save>.sav -> <save>.sav.bak.
 * Trading is the one operation users cannot afford to have corrupted. */
static void backup_save(const char *save_path)
{
   static uint8_t buf[FE_SRAM_SIZE];
   char bak[256];
   FILE *in, *out;
   size_t n;

   in = fopen(save_path, "rb");
   if (!in)
      return;
   n = fread(buf, 1, sizeof(buf), in);
   fclose(in);
   if (!n)
      return;
   snprintf(bak, sizeof(bak), "%s.bak", save_path);
   out = fopen(bak, "wb");
   if (!out)
      return;
   fwrite(buf, 1, n, out);
   fclose(out);
   fe_evt("sav_backup file=%s size=%u", bak, (unsigned)n);
}

/* ----------------------------------------------------------- wireless --- */

/* g_net_up moved up near the ME video globals (ADR-0080c session gate). */
static int g_net_is_host;
static int g_group_lost_logged;
static char g_session_info[48];
static const char *g_save_path_for_backup;

static uint64_t net_now_us(void)
{
   /* Microseconds, monotonic (ADHOC-NOTES §11.13) — netdrv timers must
    * never see wall-clock steps (ADR-0010). */
   return (uint64_t)sceKernelGetSystemTimeWide();
}

/* Same clock, video_prof's prototype (ADR-0034 renderer profile). */
static unsigned vprof_clock_us(void)
{
   return (unsigned)sceKernelGetSystemTimeLow();
}

/* Same clock, fe_evt's prototype (ADR-0021 log-I/O accounting).  The ADR-0067
 * playable build never installs it, because there is no log to account for. */
#ifndef GPSP_PLAYABLE
static unsigned long long evt_clock_us(void)
{
   return (unsigned long long)sceKernelGetSystemTimeWide();
}
#endif

static void net_error_evt(const char *what, int rc)
{
   fe_evt("net_error reason=%s stage=%s rc=%d sce=0x%08x",
          rc == ADHOC_ERR_WLAN_OFF ? "wlan_off" : what,
          adhoc_transport_stage(), rc,
          (unsigned)adhoc_transport_last_sce_error());
}

static void adhoc_stats_evt(void)
{
   adhoc_stats st;
   adhoc_transport_get_stats(&st);
   fe_evt("adhoc_stats tx=%u txwb=%u txfail=%u rx=%u ringdrop=%u "
          "oversize=%u rxerr=%u ctlevt=%u ctldisc=%u ctlerr=%u "
          "faultdel=%u faultdrop=%u",
          st.tx_pkts, st.tx_would_block, st.tx_fail, st.rx_pkts,
          st.rx_ring_drop, st.rx_oversize, st.rx_err,
          st.ctl_events, st.ctl_disconnects, st.ctl_errors,
          st.fault_delayed, st.fault_dropped);
}

/* EVT sess_cost — where a wireless session's per-frame time actually goes
 * (ADR-0021).
 *
 * The measurement that forced this line into existence: solo, no wireless,
 * a PSP-1000 runs Emerald at 57.6-58.7 fps with everything rendered.  Bring
 * a session up and it collapses to 34-47.  Skip 97 % of rendered frames and
 * it only reaches 50-57 — so it was never about drawing.  ~20 fps, a third
 * of the frame budget, went somewhere no log line could name.
 *
 * Emitted once per adhoc_stats window (600 frames, ~10 s).  Everything is
 * an average per frame unless noted, in microseconds, integer:
 *
 *   clk_ns   cost of ONE sceKernelGetSystemTimeWide call, measured on this
 *            console at session start.  Multiply by the call counts below
 *            to price the instrumentation itself — and to price what the
 *            pre-ADR-0021 poll path was paying (poll_n - work) times it.
 *   frame    one main-loop iteration, vblank wait EXCLUDED.  This is the
 *            number that has to stay under 16.7 ms; the loop is
 *            vblank-locked, so a frame 1 us over budget costs a whole
 *            16.7 ms.  That quantisation is why a few ms of session work
 *            shows up as 20 fps.
 *   pump     the once-per-frame fe_np_pump.
 *   poll     n = poll_receive calls the core made; work = how many of them
 *            found something to do; then avg/max of the ones that did.
 *   rx       transport drain + parse + deliver into the core (both pumps).
 *   arq      ARQ timers, retransmit scan, keepalive, roster (both pumps).
 *   pdp      sceNetAdhocPdpSend: calls in the window, then MEAN AND MAX PER
 *            CALL (not per frame).  This is the number PPSSPP cannot tell
 *            us — there it is a host UDP sendto.
 *   enq      what the EMULATION thread paid per send: the ring copy plus a
 *            semaphore signal with the TX thread on, the whole PdpSend
 *            without it.  Mean/max per call.
 *   txq      queued / sent-inline / ring-was-full counts.
 *   evt      EVT+LOG lines written in the window, then total and max us
 *            paid BY THE EMULATION THREAD.  Before ADR-0024 that was the
 *            whole fprintf+fflush to the memory stick (field max 12002 us
 *            on the 1000, 12525 us on the 3000).  With log_thread=1 it is
 *            only vsnprintf + a ring copy + a semaphore signal, so this
 *            number collapsing to tens of us IS the proof the move worked.
 *   evtio    the memory stick itself, now on the writer thread: total us in
 *            the window / worst single flush / lines dropped because the
 *            ring was full / ring high-water bytes.  `drop` must stay 0; if
 *            it does not, FE_EVT_RING is too small for this log volume.
 */
/* Windows shorter than this are not divided into: opening or closing the
 * in-game menu pauses the core but not the wireless tick, so the window can
 * span a single frame and the per-frame averages come out as nonsense
 * (`pump=14674 arq=8773` in the field log — microseconds of a whole menu
 * charged to one frame).  `force` is teardown, which must always report. */
#define SESS_COST_MIN_FRAMES 60

/* Session pacing state (ADR-0027/0028 adaptive, ADR-0033 fixed-rate).
 * Declared here rather than with the module below only because `sess_cost`
 * reports it — the module, and the reasoning, live under "session pacing"
 * further down. */
/* Hoisted out of the pacing constants below (ADR-0037): the frameskip policy
 * sits between here and there and now needs it too. */
#define PACE_NOMINAL_X100      5973   /* GBA 59.7275 Hz: never target above */

static int      g_pace_engaged;
static unsigned g_pace_cap_x100;      /* OUR capability (EMA), not achieved */
static unsigned g_pace_peer_x100;     /* last peer capability we acted on */
/* ADR-0047: the absolute vblank we intend to present the next frame on.
 * Paced against sceDisplayGetVcount() so an overrunning frame consumes
 * its budget instead of adding a whole vblank on top of it. */
static unsigned g_vc_target;
#define PACE_VC_RESYNC_VB 8   /* arrears past this are forgiven, not repaid */
/* Decaying low-water marks (ADR-0028): the sustained WORST of each side. */
static unsigned g_pace_self_lo, g_pace_peer_lo;
static uint64_t g_pace_self_lo_us, g_pace_peer_lo_us;
static unsigned g_pace_target_x100;   /* applied target, ramped */
static unsigned g_pace_goal_x100;     /* what the target is ramping toward */
static unsigned g_pace_acc;           /* fractional-vblank accumulator, /10000 */
static uint64_t g_pace_win_us;
static unsigned g_pace_win_frames;
static uint64_t g_pace_win_vb;        /* vblank periods this window's work needed */
static uint64_t g_pace_log_us;
static int      g_pace_floor_said;
static int      g_pace_slow_us;       /* harness only: see `pace_slow_us` */
/* ADR-0033 fixed-rate mode. */
static unsigned g_pace_fixed_x100;    /* the clamp: config net_session_fps */
static uint64_t g_pace_ramp_us;       /* last time the glide advanced */
static int      g_pace_miss_said;     /* one session_pace_miss per episode */

/* ---- ADR-0063: EVT frame_hist — the SHAPE of the frame-time distribution --
 *
 * Why this exists.  `EVT sess_cost frame=<mean>/<max>` reports a window mean
 * and an ALL-TIME max and nothing in between, and `EVT core_prof
 * core=<mean>/<winmax>/<max>` adds a per-window max.  Three sessions of work
 * (CLIFF, OVERNIGHT, FULLSPEED) have shown the failure at 45.00-59.73 is
 * probabilistic — 1.00 / 0.80 / 0.56 is a slope, not a step — so the quantity
 * that decides a run is how OFTEN a frame overruns its budget, and a mean
 * plus a max cannot see a change in that frequency.  A window whose one worst
 * frame is 42 ms is indistinguishable from a window with fifty of them.
 *
 * Cost: this is fed from the work_us the main loop ALREADY computes for
 * ADR-0021/ADR-0027 (`fe_np_prof_frame` / `pace_frame`), so it adds NO clock
 * read.  Per frame it is <= 11 compares and one increment.  At 59.73 fps that
 * is under 700 compares per wall second against a ~16.7 ms budget; the
 * profiler this replaces (`core_phase`) cost ~500 us PER FRAME (FULLSPEED
 * §20.4) and this is four orders of magnitude below that.
 *
 *   EVT frame_hist win=<frames> bud=<us> b=<b0>,..,<b10> ovb=<n> wmax=<us>
 *                  late=<n> forgive=<n> nudge=<n>
 *
 *   b[]      frame WORK time (vblank wait excluded — the same number
 *            `sess_cost frame=` averages), bucketed by the edges below.
 *            Deltas over the window, so the buckets sum to `win`.
 *   bud      the current pacing period in us (10^6 * 100 / pace target).
 *   ovb      frames whose work exceeded `bud`.  This is the headline: at
 *            59.73 the budget is 16740 us and the join's window MEAN is
 *            already 16911-17196, so a threshold fixed at 16.7 ms would be
 *            saturated — `ovb` follows the rate instead of assuming one.
 *   wmax     worst work time IN THIS WINDOW (`sess_cost frame=`'s max is
 *            all-time and latches on a boot frame; ADR-0028 was caught by
 *            exactly that and had to add a window max for the same reason).
 *   late     frames that arrived at the pacing wait ALREADY at or past
 *            `g_vc_target` — i.e. the frame consumed its whole vblank budget
 *            and we did not wait at all.  This is the over-budget count as
 *            the PACER sees it, in vblanks rather than in microseconds, and
 *            it needs no threshold at all.
 *   forgive  times the ADR-0047 arrears forgiveness fired
 *            (`vcount - target > PACE_VC_RESYNC_VB`): a stall so long the
 *            debt was written off.  Every one of these is a hard divergence
 *            from the peer's frame count.
 *   nudge    times the audio-ring high-water nudge (plan §4.4,
 *            `ring_w - ring_r > RING_HIGH_WATER`) added a whole extra vblank.
 *            At 59.73 that HALVES the frame's rate, and until this counter
 *            existed the nudge was completely unobservable — there is no
 *            other log line anywhere that reports the ring level.  It is
 *            listed as a suspect spike source and nobody has ever seen it
 *            fire or not fire.
 *
 * Bucket edges straddle BOTH budgets in play: 16740 us (59.73 fps) and
 * 33400 us (29.97 fps, the regression gate).  b5 is the "just over full
 * speed" bucket and b8 the "just over the gate" bucket, so one line reads
 * usefully at either rate.  The last bucket isolates the >100 ms save
 * frames, which are known and are not the target. */
#define FH_BINS 11
static const uint32_t g_fh_edge[FH_BINS - 1] = {
   10000, 12000, 14000, 15500, 16740, 20000, 25000, 33400, 50000, 100000
};
static unsigned g_fh_bin[FH_BINS];
static unsigned g_fh_n;           /* frames binned since the last emit      */
static unsigned g_fh_ovb;         /* frames over the CURRENT pacing budget */
static uint32_t g_fh_wmax;        /* worst work_us in the current window */
static unsigned g_fh_late;        /* pacing wait was already due          */
static unsigned g_fh_forgive;     /* ADR-0047 arrears written off         */
static unsigned g_fh_nudge;       /* audio-ring high-water extra vblank   */

/* Called from the main loop with the work time it already measured.  No
 * clock read, no division: the budget is recomputed only when the pace
 * target moves, which is at most once a second (ADR-0033's glide). */
static void frame_hist_note(uint32_t work_us)
{
   static unsigned cached_target;
   static uint32_t cached_bud = 16740;
   unsigned i;

   if (g_pace_target_x100 != cached_target)
   {
      cached_target = g_pace_target_x100;
      cached_bud    = cached_target
                    ? (uint32_t)(100000000ull / cached_target)
                    : 16740u;
   }
   for (i = 0; i < FH_BINS - 1; i++)
      if (work_us < g_fh_edge[i])
         break;
   g_fh_bin[i]++;
   g_fh_n++;
   if (work_us > cached_bud)
      g_fh_ovb++;
   if (work_us > g_fh_wmax)
      g_fh_wmax = work_us;
}

/* `win` is this line's OWN frame count, so b[] always sums to it.  It is not
 * necessarily `sess_cost win=`: sess_cost only exists while a session is up,
 * so the first frame_hist of a run also carries every frame since boot.
 * `swin` is sess_cost's count, printed so the two can be lined up. */
static void frame_hist_evt(unsigned swin)
{
   unsigned bud = g_pace_target_x100
                ? (unsigned)(100000000ull / g_pace_target_x100) : 0u;
   fe_evt("frame_hist win=%u swin=%u bud=%u "
          "b=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
          "ovb=%u wmax=%u late=%u forgive=%u nudge=%u",
          g_fh_n, swin, bud,
          g_fh_bin[0], g_fh_bin[1], g_fh_bin[2], g_fh_bin[3], g_fh_bin[4],
          g_fh_bin[5], g_fh_bin[6], g_fh_bin[7], g_fh_bin[8], g_fh_bin[9],
          g_fh_bin[10],
          g_fh_ovb, g_fh_wmax, g_fh_late, g_fh_forgive, g_fh_nudge);
   memset(g_fh_bin, 0, sizeof(g_fh_bin));
   g_fh_n = g_fh_ovb = g_fh_late = g_fh_forgive = g_fh_nudge = 0;
   g_fh_wmax = 0;
}

/* ---- ADR-0064: EVT preempt — HOW OFTEN IS THE EMULATION THREAD ACTUALLY
 *      TAKEN OFF THE CPU, AND WHERE IN THE FRAME -------------------------
 *
 * Why this exists.  FULLSPEED §27.2 measured that `emu_prio = 0x2B` costs the
 * join 3.5-4.0 ms of frame time (`core` 11.9-13.5 -> 16.2-17.2 ms) and called
 * it "pure preemption overhead".  That is an INFERENCE from a difference of
 * two means; nobody has ever counted a single preemption on this rig.  Every
 * design that follows from it — the protected-window idea included — depends
 * on whether the emulation thread is interrupted twice per frame or fifty
 * times, and on whether those interruptions land inside `retro_run` or in the
 * frontend.  Two prior recommendations in this project were built on
 * unverified numbers and both were wrong, so this counts before anything is
 * designed.
 *
 * How.  `sceKernelReferThreadRunStatus()` returns the kernel's own
 * `threadPreemptCount` (times this thread was taken off the CPU by a
 * higher-priority THREAD becoming runnable) and `intrPreemptCount` (times an
 * INTERRUPT took it off).  Sampling those at four points in the loop splits
 * one frame into four accounted regions:
 *
 *   pre    frame start -> just before the core-run block  (input, autopilot,
 *          UI, fast-forward bookkeeping)
 *   core   the `fe_host_run_frame()` block itself, plus pace_burn
 *   post   after the core -> the point where `work_us` is taken (dump
 *          checks, blit_prof, autopilot status)
 *   wait   the point where `work_us` is taken -> the NEXT frame's start.
 *          This is the pacing/vblank wait, i.e. the time we WANT the radio to
 *          run in.  Preemptions here are free; preemptions in the other three
 *          are the 3.5-4.0 ms.
 *
 * `pre`+`core`+`post` is exactly the region `work_us` measures, so the counts
 * and the microseconds are commensurable.
 *
 * Cost and honesty about it.  Four kernel calls plus eight clock reads per
 * frame, all of which are self-timed into `self=` and reported, so the
 * instrument's own price is in its own output.  It is OFF unless
 * `preempt_prof = 1` is set in the harness ini: with the key absent not one
 * of these functions is entered and the loop is byte-for-byte the previous
 * behaviour.
 *
 *   EVT preempt n=<frames> t=<pre>/<core>/<post>/<wait>
 *               i=<pre>/<core>/<post>/<wait> wmax=<n> imax=<n>
 *               h=<h0>,..,<h6> rel=<n> self=<us> calls=<n> prio=0x<xx>
 *
 *   t/i    window TOTALS (not per-frame) of thread / interrupt preemptions in
 *          each region.  Divide by `n` for the per-frame figure.
 *   wmax   worst single frame's thread-preemption count over pre+core+post.
 *   imax   the same for interrupt preemptions.
 *   h[]    histogram of per-frame thread preemptions over pre+core+post,
 *          bins 0 / 1-2 / 3-4 / 5-8 / 9-16 / 17-32 / 33+.  A mean cannot tell
 *          "every frame loses 8 slices" from "one frame in ten loses 80", and
 *          those two want different fixes.
 *   rel    releaseCount delta — times the thread was released from a wait.
 *   self   microseconds this instrument itself spent inside the kernel calls.
 *   prio   the emulation thread's priority at emit time (so a build running
 *          the ADR-0065 boost window shows which side of the driver it is on).
 */
/* ADR-0066, see the main loop.  Frames between dump-marker polls; 0 = never.
 * ADR-0069 moved the default to 0 — L+R+SELECT replaces the marker file and
 * costs nothing.  Set `dump_marker_poll = 60` for the pre-0069 behaviour. */
static int      g_dump_marker_every = PLAY_DUMP_MARKER;

/* ADR-0069: the screenshot chord, and the yield the marker stat used to pay
 * for by accident.  `g_yield_us` microseconds of sceKernelDelayThread once per
 * `g_yield_every` frames; 0 us = off, which is the default in both builds. */
static int      g_dump_chord_pending;
static int      g_yield_us;
static int      g_yield_every = 60;

#define PE_BINS 7
static SceUID   g_emu_thid = -1;
static int      g_pe_on;            /* harness key `preempt_prof`           */
static unsigned g_pe_n;
static unsigned g_pe_t[4], g_pe_i[4];   /* pre / core / post / wait        */
static unsigned g_pe_wmax, g_pe_imax;
static unsigned g_pe_hist[PE_BINS];
static unsigned g_pe_rel, g_pe_self_us, g_pe_calls;
static unsigned g_pe_last_t, g_pe_last_i, g_pe_last_r;
static int      g_pe_have;
static unsigned g_pe_fr_t, g_pe_fr_i;   /* this frame's work-region totals */

/* Sample once and attribute the delta since the previous sample to `slot`.
 * slot 0..3 = pre/core/post/wait; slot < 0 = prime the counters only. */
static void preempt_mark(int slot)
{
   SceKernelThreadRunStatus st;
   uint64_t t0;
   int rc;

   if (!g_pe_on || g_emu_thid < 0)
      return;
   memset(&st, 0, sizeof(st));
   st.size = sizeof(st);
   t0 = net_now_us();
   rc = sceKernelReferThreadRunStatus(g_emu_thid, &st);
   g_pe_self_us += (unsigned)(net_now_us() - t0);
   g_pe_calls++;
   if (rc < 0)
   {
      g_pe_on = 0;                 /* never let an instrument fail a run */
      fe_evt("preempt disabled rc=%d", rc);
      return;
   }
   if (g_pe_have && slot >= 0 && slot < 4)
   {
      unsigned dt = st.threadPreemptCount - g_pe_last_t;
      unsigned di = st.intrPreemptCount   - g_pe_last_i;
      g_pe_t[slot] += dt;
      g_pe_i[slot] += di;
      g_pe_rel     += st.releaseCount - g_pe_last_r;
      if (slot != 3)               /* 0..2 are the work region */
      {
         g_pe_fr_t += dt;
         g_pe_fr_i += di;
      }
   }
   g_pe_have   = 1;
   g_pe_last_t = st.threadPreemptCount;
   g_pe_last_i = st.intrPreemptCount;
   g_pe_last_r = st.releaseCount;
}

/* Called once per frame, after the `post` mark, to close out the histogram. */
static void preempt_frame_end(void)
{
   unsigned v;
   if (!g_pe_on)
      return;
   v = g_pe_fr_t;
   g_pe_n++;
   if (v > g_pe_wmax) g_pe_wmax = v;
   if (g_pe_fr_i > g_pe_imax) g_pe_imax = g_pe_fr_i;
   g_pe_hist[v == 0 ? 0 : v <= 2 ? 1 : v <= 4 ? 2 : v <= 8 ? 3 :
             v <= 16 ? 4 : v <= 32 ? 5 : 6]++;
   g_pe_fr_t = g_pe_fr_i = 0;
}

static void preempt_evt(void)
{
   SceKernelThreadRunStatus st;
   int prio = 0;
   if (!g_pe_on)
      return;
   memset(&st, 0, sizeof(st));
   st.size = sizeof(st);
   if (sceKernelReferThreadRunStatus(g_emu_thid, &st) >= 0)
      prio = st.currentPriority;
   fe_evt("preempt n=%u t=%u/%u/%u/%u i=%u/%u/%u/%u wmax=%u imax=%u "
          "h=%u,%u,%u,%u,%u,%u,%u rel=%u self=%u calls=%u prio=0x%02X",
          g_pe_n,
          g_pe_t[0], g_pe_t[1], g_pe_t[2], g_pe_t[3],
          g_pe_i[0], g_pe_i[1], g_pe_i[2], g_pe_i[3],
          g_pe_wmax, g_pe_imax,
          g_pe_hist[0], g_pe_hist[1], g_pe_hist[2], g_pe_hist[3],
          g_pe_hist[4], g_pe_hist[5], g_pe_hist[6],
          g_pe_rel, g_pe_self_us, g_pe_calls, prio);
   memset(g_pe_t, 0, sizeof(g_pe_t));
   memset(g_pe_i, 0, sizeof(g_pe_i));
   memset(g_pe_hist, 0, sizeof(g_pe_hist));
   g_pe_n = g_pe_wmax = g_pe_imax = g_pe_rel = g_pe_self_us = g_pe_calls = 0;
}

/* ---- ADR-0065: THE PROTECTED WINDOW ------------------------------------
 *
 * The problem, from FULLSPEED §27/§28.3.  `emu_prio = 0x2B` is what makes
 * full speed work at all (srtt 680 -> 20 ms, retx 27 % -> 0 %), because it
 * puts the emulation thread BELOW the WLAN driver's 0x2A so the radio no
 * longer has to live in the emulator's vanishing idle.  But it is a STANDING
 * demotion: the driver can take the CPU at any instant for the whole frame,
 * and that costs the join 3.5-4.0 ms of pure preemption overhead against a
 * 16.74 ms budget whose actual emulation work is only ~13.4 ms.  The network
 * fix created the frame overrun that is now the only remaining problem.
 *
 * The idea.  Keep the demotion as the DEFAULT state of the frame, but carve
 * out a bounded window at the top of each frame in which the emulator is
 * temporarily raised ABOVE the driver (`emu_prio_boost`, default 0x29), then
 * dropped back to `emu_prio`.  The radio keeps preemptive priority for the
 * rest of the frame.
 *
 * Why this direction and not the other.  If the window is mistuned it
 * degrades toward TODAY'S WORKING behaviour (a shorter or absent window = the
 * current standing demotion), not toward the broken pre-`emu_prio` state.  A
 * knob that raises the emulator permanently would fail the other way.
 *
 * How the window is bounded.  Not by the main loop — it cannot check a clock
 * from inside `retro_run`, which is ~13 ms of the frame in one call.  A
 * warden thread at 0x13 -- deliberately BELOW our callback thread (0x11) and
 * our audio thread (0x12) so it can never delay either, and above everything
 * else we own -- waits on a semaphore, sleeps `emu_prio_boost_us`, and lowers
 * the emulation thread from the outside.  The main loop ALSO closes the
 * window when its work region ends, so the window is
 *
 *      min(emu_prio_boost_us, this frame's actual work time)
 *
 * and can never extend into the vblank wait, which is the time the radio is
 * supposed to have.  Both closers are idempotent.
 *
 *   emu_prio_boost_us   0 = OFF, and with it off not one kernel call is made
 *                       and no thread is created — today's behaviour exactly.
 *   emu_prio_boost      priority during the window; default 0x29, i.e. one
 *                       step above the 0x2A WLAN stack.
 *
 *   EVT boost open=<n> close_self=<n> close_warden=<n> us=<n> prio=0x<xx>
 *                base=0x<xx> late=<n>
 *     close_self / close_warden say WHICH end shut the window, and their
 *     ratio is the result: all-self means the window is longer than the
 *     frame's work and the knob is just "emu_prio off"; all-warden means the
 *     window is genuinely bounded and the radio got the rest of the frame.
 *     `late` counts frames where the warden was still asleep from the
 *     PREVIOUS frame when a new one opened. */
static int      g_boost_us;          /* harness key, 0 = off               */
static int      g_boost_prio = 0x29;
static int      g_boost_base = 0x2B; /* what we drop back TO = emu_prio    */
static SceUID   g_boost_thid = -1, g_boost_sema = -1;
static volatile int g_boost_run;
static volatile int g_boost_open;    /* 1 while the window is raised       */
static unsigned g_boost_n_open, g_boost_n_self, g_boost_n_warden, g_boost_late;

/* Drop the emulation thread back to its standing priority.  Safe to call
 * from either thread and safe to call twice: the flag is the interlock. */
static void emu_boost_drop(void)
{
   if (!g_boost_open)
      return;
   g_boost_open = 0;
   sceKernelChangeThreadPriority(g_emu_thid, g_boost_base);
}

static int boost_warden(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (g_boost_run)
   {
      SceUInt tmo = 500000;
      if (sceKernelWaitSema(g_boost_sema, 1, &tmo) < 0)
         continue;                        /* timeout: just re-arm */
      if (!g_boost_run)
         break;
      sceKernelDelayThread((SceUInt)g_boost_us);
      if (g_boost_open)
      {
         g_boost_n_warden++;
         emu_boost_drop();
      }
   }
   return 0;
}

static void emu_boost_open(void)
{
   if (!g_boost_us || g_boost_thid < 0 || !g_net_up)
      return;
   if (g_boost_open)
      g_boost_late++;                     /* warden still asleep from last */
   g_boost_open = 1;
   sceKernelChangeThreadPriority(g_emu_thid, g_boost_prio);
   g_boost_n_open++;
   sceKernelSignalSema(g_boost_sema, 1);  /* max count 1: extras are lost */
}

static void emu_boost_close(void)
{
   if (!g_boost_us || !g_boost_open)
      return;
   g_boost_n_self++;
   emu_boost_drop();
}

static void emu_boost_start(void)
{
   if (!g_boost_us)
      return;
   g_boost_run  = 1;
   g_boost_sema = sceKernelCreateSema("gpsp_boost", 0, 0, 1, NULL);
   if (g_boost_sema >= 0)
   {
      g_boost_thid = sceKernelCreateThread("gpsp_boost", boost_warden,
                                           0x13, 0x800, THREAD_ATTR_USER,
                                           NULL);
      if (g_boost_thid < 0 || sceKernelStartThread(g_boost_thid, 0, NULL) < 0)
      {
         if (g_boost_thid >= 0)
            sceKernelDeleteThread(g_boost_thid);
         g_boost_thid = -1;
         sceKernelDeleteSema(g_boost_sema);
         g_boost_sema = -1;
      }
   }
   if (g_boost_thid < 0)
   {
      g_boost_us = 0;             /* never fail a run over an optimisation */
      fe_evt("boost disabled reason=thread_failed");
      return;
   }
   fe_evt("boost armed us=%d prio=0x%02X base=0x%02X warden=0x13",
          g_boost_us, g_boost_prio, g_boost_base);
}

static void emu_boost_stop(void)
{
   if (g_boost_thid < 0)
      return;
   emu_boost_drop();
   g_boost_run = 0;
   sceKernelSignalSema(g_boost_sema, 1);
   {
      SceUInt tmo = 1000000;
      sceKernelWaitThreadEnd(g_boost_thid, &tmo);
   }
   sceKernelDeleteThread(g_boost_thid);
   sceKernelDeleteSema(g_boost_sema);
   g_boost_thid = -1;
   g_boost_sema = -1;
}

static void boost_evt(void)
{
   if (!g_boost_us)
      return;
   fe_evt("boost open=%u close_self=%u close_warden=%u us=%d prio=0x%02X "
          "base=0x%02X late=%u",
          g_boost_n_open, g_boost_n_self, g_boost_n_warden,
          g_boost_us, g_boost_prio, g_boost_base, g_boost_late);
   g_boost_n_open = g_boost_n_self = g_boost_n_warden = g_boost_late = 0;
}

static void sess_cost_evt(int force)
{
   static fe_np_prof  prev;
   static adhoc_stats prev_a;
   static unsigned    prev_evt_lines, prev_evt_us, prev_evt_io_us;
   static unsigned    prev_core_calls;
   static uint32_t    prev_core_us;
   fe_np_prof  p;
   adhoc_stats a;
   unsigned    el, eu, emx;
   unsigned    eio, eiomx, edrop, ehi;
   unsigned    f, dpdp, denq;
   unsigned    ccalls, dcc;
   uint32_t    cus, cmax;
   unsigned    srf, sff, ssf, sdf, spl;  /* counters, SLOWEST retro_run */
   unsigned    crf, cff, csf, cdf, cpl;  /* cumulative, for window rates */
   static unsigned prev_crf, prev_cff, prev_csf, prev_cdf, prev_cpl;

   fe_np_prof_get(&p);
   adhoc_transport_get_stats(&a);
   fe_evt_prof(&el, &eu, &emx);
   fe_evt_prof_io(&eio, &eiomx, &edrop, &ehi);
   fe_host_core_prof(&ccalls, &cus, &cmax);        /* ADR-0027 §measurement */
   fe_host_core_spike(NULL, &srf, &sff, &ssf, &sdf, &spl);    /* ADR-0028 */
   plat_core_counters(&crf, &cff, &csf, &cdf, &cpl);

   f = p.frames - prev.frames;
   if (!force && f < SESS_COST_MIN_FRAMES)
      return;      /* deliberately do NOT advance prev: fold into the next */
   if (!f)
      f = 1;
   dpdp = a.tx_calls  - prev_a.tx_calls;
   denq = a.tx_queued + a.tx_inline - (prev_a.tx_queued + prev_a.tx_inline);
   dcc  = ccalls - prev_core_calls;

   fe_evt("sess_cost win=%u clk_ns=%u frame=%u/%u pump=%u/%u "
          "poll=%u/%u/%u/%u rx=%u/%u arq=%u/%u "
          "pdp=%u/%u/%u enq=%u/%u txq=%u/%u/%u evt=%u/%u/%u "
          "evtio=%u/%u/%u/%u txthr=%d core=%u/%u "
          "corewin=%u/%u/%u/%u/%u corespike=%u/%u/%u/%u/%u "
          "pace=%u.%02u/%u.%02u/%u.%02u/%d",
          f, p.clk_ns,
          (p.frame_us - prev.frame_us) / f, p.frame_max_us,
          (p.pump_us  - prev.pump_us)  / f, p.pump_max_us,
          (p.poll_n - prev.poll_n) / f,
          (p.poll_work_n - prev.poll_work_n) / f,
          (p.poll_us - prev.poll_us) / f, p.poll_max_us,
          (p.rx_us  - prev.rx_us)  / f, p.rx_max_us,
          (p.arq_us - prev.arq_us) / f, p.arq_max_us,
          dpdp, dpdp ? (a.tx_us - prev_a.tx_us) / dpdp : 0u, a.tx_max_us,
          denq ? (a.tx_enq_us - prev_a.tx_enq_us) / denq : 0u, a.tx_enq_max_us,
          a.tx_queued - prev_a.tx_queued, a.tx_inline - prev_a.tx_inline,
          a.tx_ring_full - prev_a.tx_ring_full,
          el - prev_evt_lines, eu - prev_evt_us, emx,
          eio - prev_evt_io_us, eiomx, edrop, ehi,
          adhoc_transport_tx_thread_active(),
          dcc ? (unsigned)((cus - prev_core_us) / dcc) : 0u, cmax,
          crf - prev_crf, cff - prev_cff, csf - prev_csf, cdf - prev_cdf,
          cpl - prev_cpl,
          srf, sff, ssf, sdf, spl,
          g_pace_target_x100 / 100, g_pace_target_x100 % 100,
          g_pace_cap_x100 / 100,    g_pace_cap_x100 % 100,
          g_pace_peer_x100 / 100,   g_pace_peer_x100 % 100,
          g_pace_engaged);

   frame_hist_evt(f);                                       /* ADR-0063 */
   preempt_evt();                                           /* ADR-0064 */
   boost_evt();                                             /* ADR-0065 */

   prev = p;
   prev_a = a;
   prev_evt_lines = el;
   prev_evt_us = eu;
   prev_evt_io_us = eio;
   prev_core_calls = ccalls;
   prev_core_us = cus;
   prev_crf = crf;
   prev_cff = cff;
   prev_csf = csf;
   prev_cdf = cdf;
   prev_cpl = cpl;
}

static void log_adhoc_up(const char *group)
{
   nd_transport tp;
   uint8_t mac[6];
   adhoc_transport_iface(&tp);
   tp.local_addr(tp.ctx, mac);
   fe_evt("adhoc_up group=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
          group && group[0] ? group : ADHOC_GROUP_DEFAULT,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Start the netdrv/netpacket layer over the (already-up) transport. */
static int net_start_np(int is_host, const char *group, const char *nick,
                        int probe)
{
   static nd_transport tp;               /* fe_np copies the vtable */
   fe_np_config npc;

   adhoc_transport_iface(&tp);
   memset(&npc, 0, sizeof(npc));
   npc.transport = &tp;
   npc.is_host   = is_host;
   npc.nick      = nick;
   npc.now_us    = net_now_us;
   npc.probe     = probe;
   if (fe_np_start(&npc) != 0)
   {
      fe_evt("net_error reason=np_start stage=up rc=-1 sce=0x0");
      return -1;
   }
   g_net_up = 1;
   g_net_is_host = is_host;
   g_group_lost_logged = 0;
   snprintf(g_session_info, sizeof(g_session_info), "%s  room %s",
            is_host ? "hosting" : "joined",
            group && group[0] ? group : ADHOC_GROUP_DEFAULT);
   /* Settings -> "Session overlay" hides the chip; g_session_info itself is
    * kept regardless — the wireless menu's status line reads it. */
   osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
   return 0;
}

/* Re-apply the session-chip visibility after a settings change, without
 * touching the session itself. */
void osd_session_chip_refresh(void)
{
   if (g_net_up)
      osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
}

/* ---- session frameskip policy (ADR-0019, supersedes ADR-0018) -----------
 *
 * ADR-0018 set gpsp_frameskip=auto for the whole session on the theory that
 * a console falling behind stretches the emulated link's timeouts.  The
 * first successful field session disproved the premise for both consoles:
 * the heartbeat ladder held ~58.9 fps emulated from start to finish, i.e.
 * identical to solo play, so `auto` was throwing away rendered frames while
 * the emulator was already keeping real time.  Worse, `auto` triggers on a
 * transient audio-buffer dip (fe_host_audio_buffer_status < 25 %) which our
 * vblank-locked main loop can produce with plenty of CPU headroom — and
 * because the loop then still waits for vblank, the skip buys back no
 * emulation throughput at all.  Pure user-visible stutter.
 *
 * Policy (config.ini `net_frameskip`):
 *   0 off       DEFAULT — never touch frameskip for a session.
 *   1 adaptive  start disabled; engage a BOUNDED skip only after
 *               SKIP_SLOW_WINDOWS consecutive 1 s windows below
 *               SKIP_SLOW_PCT_TGT of the applied pace target, and drop it
 *               again after SKIP_FAST_WINDOWS windows back above
 *               SKIP_FAST_PCT_TGT of it (ADR-0037 made both relative).
 *               Hysteresis, deliberately: `auto` reacted per frame.
 *   2 auto      ADR-0018 behaviour verbatim, kept so the two can be
 *               compared on real hardware in one sitting.
 * Every transition is logged, so a field log says which policy ran.
 *
 * ADR-0021 amendment — what mode 1 engages.  On hardware it skipped
 * 580 of 600 frames (`EVT fps ... rendered=1.9`) and STILL did not reach
 * 60 fps.  That is not a tuning miss, it is the mechanism: `auto_threshold`
 * skips whenever audio-buffer occupancy is below the threshold, and a
 * console that is genuinely behind never refills the ring, so the only
 * thing bounding it is the core's FRAMESKIP_MAX = 30 consecutive skips
 * (libretro/libretro.c:96,1361-1377) — 30 skipped, 1 rendered, forever.
 * A console short of ~1.4x needs half its renders back, not 97 % of them.
 * So the adaptive mode now engages `fixed_interval` with interval 1: skip
 * one, render one, hard bound, no audio-occupancy feedback loop at all.
 * The hysteresis around it is unchanged.
 *
 * ADR-0037 amendment — "behind" IS RELATIVE TO THE APPLIED PACE TARGET.
 * The two thresholds used to be absolute fps constants (5700/5850) derived
 * from the GBA's nominal 59.7275.  That was right while the only rate we
 * ever aimed at WAS the nominal one.  ADR-0033 then made a session clamp
 * itself to `net_session_fps`, and the constants did not follow: a console
 * pacing perfectly at its 40.00 (or 29.97) target measures 40.00 < 57.00,
 * concludes it is behind on three consecutive windows, and engages
 * interval-1 skip FOR THE WHOLE SESSION.  The field log is unambiguous —
 * `EVT fps emu=29.43 rendered=14.71 skipped=300` with `self_cap` reporting
 * 52-57 fps of capability sitting unused.  The emulator was hitting its
 * target exactly and throwing away half its frames for it.
 *
 * So the thresholds are now a FRACTION of `g_pace_target_x100`, the applied
 * (ramped) target.  Off-session that is PACE_NOMINAL_X100 and the numbers
 * come out at 5697/5853 — within 3/100 fps of the old constants, so this is
 * a no-op everywhere except the case it fixes.  Percentages rather than the
 * old literals because the target moves during the ~5 s glide and a fixed
 * offset would mean something different at each point on the ramp. */
#define SKIP_WIN_US        1000000ull
#define SKIP_SLOW_PCT_TGT     954    /* x10: below 95.4 % of target = behind */
#define SKIP_FAST_PCT_TGT     980    /* x10: above 98.0 % of target = fine   */
#define SKIP_SLOW_WINDOWS      3
#define SKIP_FAST_WINDOWS      5

enum { SKIP_POL_OFF = 0, SKIP_POL_ADAPTIVE, SKIP_POL_AUTO };

static int      g_skip_engaged;      /* auto_threshold currently set */
static int      g_skip_slow, g_skip_fast;
static uint64_t g_skip_win_us;
static unsigned g_skip_win_frames;

static void skip_policy_set(const char *mode)
{
   fe_host_option_set_live("gpsp_frameskip", mode);
}

static void skip_policy_session_begin(void)
{
   g_skip_engaged = 0;
   g_skip_slow = g_skip_fast = 0;
   g_skip_win_us = net_now_us();
   g_skip_win_frames = fe_host_frame_count();
   if (g_pcfg.net_frameskip == SKIP_POL_AUTO)
   {
      g_skip_engaged = 1;
      skip_policy_set("auto");
   }
   fe_evt("skip_policy mode=%s engaged=%d",
          g_pcfg.net_frameskip == SKIP_POL_AUTO     ? "auto" :
          g_pcfg.net_frameskip == SKIP_POL_ADAPTIVE ? "adaptive" : "off",
          g_skip_engaged);
}

/* Hand frameskip back to the session policy — used when fast-forward (which
 * owns gpsp_frameskip while it runs, plan §4.4) releases it. */
static void skip_policy_reapply(void)
{
   if (!g_net_up || !g_skip_engaged)
   {
      skip_policy_set("disabled");
      return;
   }
   if (g_pcfg.net_frameskip == SKIP_POL_AUTO)
      skip_policy_set("auto");
   else
   {
      fe_host_option_set_live("gpsp_frameskip_interval", "1");
      skip_policy_set("fixed_interval");
   }
}

static void skip_policy_session_end(void)
{
   if (g_skip_engaged)
   {
      skip_policy_set("disabled");
      g_skip_engaged = 0;
   }
}

/* Once per main-loop iteration while a session is up.  Never runs during
 * fast-forward: FF owns gpsp_frameskip itself (plan §4.4). */
/* The rate this console is CURRENTLY TRYING to hit, x100 (ADR-0037).  Before
 * pace_init runs, and whenever the pacing module is off, that is the GBA's
 * nominal rate — which is also what `g_pace_target_x100` is initialised to,
 * so the zero-check is belt-and-braces against call order, not a real case. */
static unsigned skip_pace_target_x100(void)
{
   return g_pace_target_x100 ? g_pace_target_x100 : PACE_NOMINAL_X100;
}

static void skip_policy_frame(void)
{
   uint64_t now, dt;
   unsigned frames, fps_x100, target, slow_x100, fast_x100;

   if (g_pcfg.net_frameskip != SKIP_POL_ADAPTIVE)
      return;
   if (g_ff_uncapped || g_ff_mult)
      return;
   if (ui_active())
   {
      /* The core is paused while the menu is open, so wall clock advances
       * and fe_host_frame_count() does not.  Left alone that divides real
       * seconds by zero frames and engages the skip on a fabricated
       * `fps=0.00` — which the field log duly showed.  Re-base the window
       * every menu frame so the first measurement after Resume is honest. */
      g_skip_win_us = net_now_us();
      g_skip_win_frames = fe_host_frame_count();
      return;
   }

   now = net_now_us();
   dt  = now - g_skip_win_us;
   if (dt < SKIP_WIN_US)
      return;

   frames = fe_host_frame_count() - g_skip_win_frames;
   fps_x100 = (unsigned)((uint64_t)frames * 100000000ull / dt);
   g_skip_win_us = now;
   g_skip_win_frames = fe_host_frame_count();

   /* ADR-0037: measure against what we are AIMING at, not against 59.7275. */
   target    = skip_pace_target_x100();
   slow_x100 = (unsigned)((uint64_t)target * SKIP_SLOW_PCT_TGT / 1000ull);
   fast_x100 = (unsigned)((uint64_t)target * SKIP_FAST_PCT_TGT / 1000ull);

   if (fps_x100 < slow_x100)
   {
      g_skip_fast = 0;
      if (!g_skip_engaged && ++g_skip_slow >= SKIP_SLOW_WINDOWS)
      {
         g_skip_engaged = 1;
         g_skip_slow = 0;
         fe_host_option_set_live("gpsp_frameskip_interval", "1");
         skip_policy_set("fixed_interval");
         /* `target=` is the whole point of ADR-0037: a field log must show
          * what the decision was measured AGAINST, or the next reader is
          * back to guessing which rate 29.43 was supposed to beat. */
         fe_evt("skip_engage fps=%u.%02u target=%u.%02u mode=fixed_interval "
                "interval=1", fps_x100 / 100, fps_x100 % 100,
                target / 100, target % 100);
      }
   }
   else if (fps_x100 >= fast_x100)
   {
      g_skip_slow = 0;
      if (g_skip_engaged && ++g_skip_fast >= SKIP_FAST_WINDOWS)
      {
         g_skip_engaged = 0;
         g_skip_fast = 0;
         skip_policy_set("disabled");
         fe_evt("skip_release fps=%u.%02u target=%u.%02u",
                fps_x100 / 100, fps_x100 % 100, target / 100, target % 100);
      }
   }
   else
      g_skip_slow = g_skip_fast = 0;   /* in the dead band: hold */
}

/* ---- session pacing (ADR-0033 fixed-rate; ADR-0027/0028 adaptive) --------
 *
 * TWO POLICIES LIVE HERE, selected by `config.ini net_pace_match`:
 *
 *   0  off       — both consoles free-run.  (Meaning unchanged.)
 *   1  FIXED     — DEFAULT (ADR-0033).  While a session is live both consoles
 *                  clamp to the SAME constant, `config.ini net_session_fps`
 *                  (default 40.00).  No negotiation, no control loop.
 *   2  adaptive  — ADR-0027/0028's peer-capability matcher, below, kept
 *                  verbatim for a hardware A/B.
 *
 * **THE MEANING OF `1` CHANGED.** It used to select the adaptive matcher.  An
 * existing config.ini carrying `net_pace_match=1` silently adopts fixed-rate
 * pacing, which is intended — 1 still means "pace during a session"; only the
 * how changed.  Anyone wanting the old behaviour must now write `2`.
 *
 * WHY THE CONTROL LOOP WENT AWAY.  The requirement changed: full-speed
 * emulation *during a wireless session* is now explicitly a nice-to-have, not
 * a requirement.  Trading is a temporary activity, and half speed for its
 * duration is acceptable so long as the session is not choppy, still accepts
 * input, and the emulator runs normally the moment the session ends.
 *
 * Against that requirement the matcher is not just unnecessary, it is
 * actively harmful.  Its target has to track a peer capability that genuinely
 * fluctuates with game workload (the field measured `peer_cap` walking
 * 49.9, 44.7, 41.6, 38.9, 41.1, 47.0, 51.0, 53.7, 55.4, 56.6, 52.6 ... within
 * seconds), so it spends the whole session hunting — and the applied rate
 * moving is itself a desync source, because Gen-3's RFU counts link timeouts
 * in FRAMES.  The hardware profile says why the hunt can never converge:
 * in-session steady state on a joining PSP-3000 is
 * `cpu 7754 vid 3069 blt 2973 audio 436` = 14253 us of a 16750 us budget, and
 * the host/join difference is entirely `cpu` (6071 -> 7754) — the GAME'S OWN
 * RFU driver code running on the emulated CPU.  That is not ours to optimise
 * and not ours to predict, so we stop predicting it.
 *
 * FIXED-RATE, then.  40.00 fps sits comfortably below the 45-58 fps both
 * consoles sustain in the join seat, and both sides compute it from the same
 * constant rather than from each other.  Deterministic, identical, and the
 * applied rate holds still for the whole session, which is the property the
 * link actually needs.  It costs speed on healthy sessions; per the changed
 * requirement, that is the trade we are choosing.
 *
 * RAMP, BOTH WAYS.  A step from 59.73 to 40.00 at session start is a stall by
 * another name — the frame it lands on is 8 ms longer, and the audio step
 * jumps with it.  So the applied rate GLIDES at PACE_FIXED_RAMP_X100_PER_S
 * (4.00 fps/s), ~5 s in and ~5 s out, on a per-frame time base rather than
 * the adaptive path's 1 s windows.  The glide out runs AFTER teardown, which
 * is why the throttle below gates on the applied target and never on
 * `g_net_up`: dropping the throttle instantly while the audio step is still
 * ramping would let the emulator outrun the consumer and overflow the ring.
 *
 * MISSING THE RATE IS REPORTED, NOT CHASED.  If a console cannot even sustain
 * the fixed rate we log `EVT session_pace_miss actual=` and carry on at the
 * configured clamp.  Chasing downward would reintroduce exactly the moving
 * target this ADR removed, and a known steady rate beats a correct-but-moving
 * one.  (The adaptive path's floor/low-water machinery is untouched and still
 * applies to mode 2.)
 *
 * AUDIO is unchanged in kind from ADR-0027: `g_audio_step` follows the
 * applied rate exactly, so production and consumption match and the stream
 * stays continuous, at the cost of a proportional pitch drop (~7 semitones at
 * 40.00 fps).  It glides because the pace glides.  The user approved this.
 *
 * ------------------------------------------------------------------------
 * What follows is mode 2's reasoning, unchanged (ADR-0027/0028):
 *
 * The field fact this exists for: the join/client role costs ~12 fps against
 * the host role on BOTH consoles (PSP-3000 58 host / 46 join; PSP-1000 56-58
 * host / 49-52 join), and the cost is inside the core's RFU client path, not
 * our transport (measured per-frame session cost totals ~500 us).  Two real
 * GBAs both run at 59.7275 Hz, so their link timing is mutually consistent.
 * Gen-3's RFU counts link timeouts in FRAMES, not wall-clock seconds — so
 * two emulators running ~20 % apart are permanently inconsistent in a way
 * two cartridges never are, and the slower side's game times out.  EQUALITY
 * MATTERS MORE THAN ABSOLUTE SPEED: if both run at the same rate, even a
 * slower one, the game-side link timing becomes mutually consistent again.
 *
 * THE CONTROL LOOP, and the trap in it.  Each side advertises a frame rate
 * and the faster side paces down to the slower.  If what a console
 * advertises is its *achieved* rate, the pair ratchets downward without
 * bound: A throttles to 46 to match B, A now measures and reports 46, B sees
 * "peer is at 46" and throttles itself, A re-measures lower, and both crawl
 * into the floor — a control-loop bug that looks exactly like a performance
 * regression.  So a console advertises its **capability**, never its
 * throttled rate:
 *
 *   capability = the rate this vblank-locked loop WOULD free-run at, derived
 *   from per-frame WORK time (the loop iteration with every vblank wait
 *   excluded — the same number ADR-0021 reports as `frame=`).  A frame
 *   costing w us occupies ceil(w / 16.683 ms) vblank periods, minimum one;
 *   sum that over a window and the rate is frames * 59.94 / periods.
 *
 * Our deliberate idle is not work, so throttling ourselves cannot move our
 * own advertised number.  `peer_cap` therefore always means "how fast my
 * partner CAN go", never "how fast my partner is currently choosing to go",
 * and the loop has exactly one mover: the genuinely faster console.  If
 * `self_cap` is ever seen sagging in the field log while `engaged=1`, the
 * ratchet is back and that line says so directly.
 *
 * WHAT THE FIELD CHANGED (ADR-0028).  The first hardware run proved the
 * mechanism and broke the control law.  Both consoles held `self_cap` flat
 * while throttling — the anti-ratchet is correct — and for the first time
 * both players entered the Union Room and could move.  But `peer_cap` swung
 * ~20 fps within seconds (49.9, 44.7, 41.6, 38.9, 41.1, 47.0, 51.0, 53.7,
 * 55.4, 56.6, 52.6, ...), and against a symmetric 2 fps/s ramp we were
 * permanently mid-chase, never at the right target, and even disengaged at
 * the floor mid-session.  The second log explained why: the console in the
 * JOIN seat becomes the slow one, so **the identity of the slow peer flips**
 * and a loop that tracks "whoever is behind right now" keeps reversing.
 *
 * So the target is no longer "the peer": it is the PAIR'S SUSTAINED WORST.
 *   - Each side keeps a decaying LOW-WATER MARK of both capabilities: a new
 *     low is taken immediately, and a low is only forgotten after 4 s of
 *     nothing worse, then at 0.50 fps per window.  This turns a 20 fps
 *     oscillation into a stable number.
 *   - The goal is `min(self_lo, peer_lo) + margin`.  **min() is symmetric**,
 *     so both consoles compute the SAME target from the same two numbers and
 *     a role flip does not move it — the identity of the mover changes, the
 *     target does not.  A steady 48 beats an accurate-but-moving 39-58.
 *   - ONE MOVER falls out for free instead of being enforced: whichever
 *     console is currently the binding constraint is already below the
 *     target and inserts no waits at all, because the throttle can only ever
 *     add delay.  No flag decides who moves.
 *
 * Guarantees, in the order they are enforced below:
 *   - ASYMMETRIC RAMP.  Fall 8.00 fps per window, rise 0.50.  Getting slow
 *     late is what desyncs a link; getting fast late costs nothing.
 *   - FLOOR.  We never pace ourselves below 40.00 fps — but we CLAMP there
 *     rather than releasing.  The field showed releasing is actively worse:
 *     against a peer at 38.9 it snaps us back to 59.73 and makes the gap
 *     21 fps instead of 1.1.  Absurd or zero reports are ignored outright.
 *   - CEILING.  The target never exceeds the GBA's own 59.7275 Hz.
 *   - STAY ENGAGED.  `engaged` is a wide-hysteresis *report* of "the target
 *     is meaningfully below nominal", not a gate on the throttle — the
 *     throttle follows the applied target alone.  Repeated engage/release
 *     cycling was itself a symptom; the 3000 released and never re-engaged
 *     even as it became the slower side.
 *   - RELEASE.  When the pair is genuinely fast again, or the peer goes
 *     away, the goal returns to nominal and the target ramps back off at the
 *     slow rate.  Nothing latches for the rest of the session.
 *
 * HOW THE TARGET IS APPLIED.  The loop is vblank-locked, so the only lever
 * is how many vblanks a frame waits for, and a whole extra vblank is a jump
 * from 59.94 to 29.97 fps — far too coarse.  Instead the fraction is
 * accumulated: a target needs 59.94/target vblanks per frame, so we carry
 * the fractional part in 1/10000ths and spend one extra vblank whenever it
 * crosses 1.  Target 46.50 -> 1.2890 vblanks/frame -> an extra vblank on
 * 28.9 % of frames, averaging exactly 46.50 fps.
 *
 * `config.ini net_pace_match` = 2 selects this policy (it was 1 before
 * ADR-0033), so the two policies A/B on hardware with no rebuild, exactly
 * like net_tx_thread/log_thread/sram_thread. */

#define PACE_MODE_OFF             0
#define PACE_MODE_FIXED           1   /* ADR-0033, default */
#define PACE_MODE_ADAPTIVE        2   /* ADR-0027/0028, kept for the A/B */
/* ADR-0078: FIXED's clamp, but the clamped rate FLOATS on backpressure.
 *
 * The fixed clamp (ADR-0033) bought rate equality with a constant, and the
 * constant has to be chosen for the worst case — 57.00 ships because the
 * join is ~2 fps short of nominal, so every pair pays 2.73 fps whether or
 * not their consoles could do better today.  The ADR-0027 matcher adapted,
 * but its signal was an EMA of the peer's ADVERTISED capability, and it
 * hunted (that is why ADR-0033 replaced it).
 *
 * This mode reuses ALL of FIXED's machinery — the glide, the audio step,
 * the snap, the miss reporting — and moves only WHERE the clamp value comes
 * from: the live reliable-TX backlog toward the peer (fe_np_txq_now).  A
 * receiver that stops absorbing shows up here within one RTO, as a direct
 * measurement rather than an estimate.  HOST-ONLY by design: the host is
 * the sender whose rate matters (child-side queues are child-only, HANDOVER
 * §2), and one adapting console cannot hunt against another — the join
 * simply runs its own fixed ceiling and the pair converges on what the join
 * actually absorbs.  On the join this mode degrades to plain FIXED.
 *
 * Control law, evaluated once per ~1 s pace window, asymmetric on purpose:
 *   backlog >= BP_HI            -> base falls  BP_STEP_DOWN, immediately
 *   backlog <= BP_LO for
 *     BP_RISE_WINS windows      -> base rises  BP_STEP_UP
 *   else                        -> hold, streak resets
 * Rise is slow (0.25 fps after 3 clean seconds), fall is fast (0.50 fps at
 * once), and every move is logged.  FALSIFIER (state it before the run):
 * if pace_bp shows the base oscillating — alternating up/down without a
 * change in conditions — this is ADR-0033's hunting again and the mode is
 * dead; if the base parks at the ini rate and never rises with txq at 0,
 * the signal is not informative and the mode is pointless. */
#define PACE_MODE_BACKPRESSURE    3
#define PACE_BP_HI                6    /* payloads: ~0.1 s of frames backed up */
#define PACE_BP_LO                1
#define PACE_BP_STEP_DOWN_X100    50
#define PACE_BP_STEP_UP_X100      25
#define PACE_BP_RISE_WINS         3
#define PACE_BP_FLOOR_X100        5500
static unsigned g_bp_low_streak;
static unsigned g_bp_hold_wins;       /* windows since the last pace_bp line */

#define PACE_VBLANK_HZ_X100    5994   /* PSP display 59.94 Hz */
#define PACE_VBLANK_US        16683   /* one vblank period */
#define PACE_FLOOR_X100        4000   /* never pace BELOW 40.00 fps ourselves */
#define PACE_MARGIN_X100         50   /* aim just above the worst, not at it */
#define PACE_ENGAGE_X100        150   /* "engaged" once 1.50 below nominal... */
#define PACE_RELEASE_X100        40   /* ...and only released within 0.40 */
#define PACE_RAMP_DOWN_X100     800   /* fall 8.00 fps per window: react fast */
#define PACE_RAMP_UP_X100        50   /* rise 0.50 fps per window: recover slow */
#define PACE_LOW_HOLD_US    4000000   /* remember a peer's worst for 4 s... */
#define PACE_LOW_RISE_X100       50   /* ...then let it forget 0.50 fps/window */
#define PACE_WIN_US         1000000   /* ~1 s measurement window */
#define PACE_EMA_DEN              3   /* new sample gets 1/3 weight */
#define PACE_PEER_MIN_X100     1000   /* sanity wall on a reported value: */
#define PACE_PEER_MAX_X100    20000   /*   10.00 .. 200.00 fps, else ignore */
#define PACE_LOG_US        10000000   /* heartbeat the steady state this often */
#define PACE_MAX_EXTRA_VB         2   /* bound on extra vblanks per frame */

/* ADR-0033 fixed-rate mode. */
#define PACE_FIXED_RAMP_X100_PER_S 400 /* glide 4.00 fps/s: ~5 s in and out */
#define PACE_MISS_MARGIN_X100      150 /* 1.50 fps of slack before "missed" */
#define PACE_MISS_LOG_US       10000000/* at most one miss line per 10 s */
/* ADR-0035: the snap only ever produces PACE_VBLANK_HZ_X100 / N.  N is capped
 * at 3 because N vblanks is (N-1) EXTRA vblanks and PACE_MAX_EXTRA_VB is 2 —
 * a snap the throttle could not actually deliver would be the same lie in a
 * different place. */
#define PACE_SNAP_MAX_VB            3

/* (state declared above sess_cost_evt, which reports it) */

/* Harness knob (.gpsp-harness.ini `pace_slow_us`).  The PPSSPP rig runs both
 * instances at full speed, so it cannot reproduce the field's host/join
 * asymmetry and pace matching would never engage there — nothing to observe.
 * Burning BUSY microseconds (not sleeping) on one instance manufactures a
 * genuinely slower peer whose cost lands in exactly the per-frame work time
 * the capability estimate reads, so the full loop is exercised. */
static void pace_burn(void)
{
   uint64_t t0;
   if (!g_pace_slow_us)
      return;
   t0 = net_now_us();
   while (net_now_us() - t0 < (uint64_t)g_pace_slow_us)
      ;
}

static void pace_audio_step(unsigned target_x100)
{
   /* One wall second at `target` fps contains target/59.7275 seconds of
    * emulated time, so the core produces that fraction of in_rate samples.
    * Consume at the same fraction and the ring neither drains nor floods. */
   if (target_x100 > PACE_NOMINAL_X100)
      target_x100 = PACE_NOMINAL_X100;
   g_audio_step = (unsigned)(((uint64_t)in_rate << 16) * target_x100
                             / ((uint64_t)OUT_RATE * PACE_NOMINAL_X100));
}

/* Decaying low-water mark (ADR-0028).  A new low is taken IMMEDIATELY — the
 * worst is what desyncs a link, so it must never be smoothed away.  A low is
 * only forgotten after PACE_LOW_HOLD_US of nothing worse, and then only at
 * PACE_LOW_RISE_X100 per window, which is what converts the field's 20 fps
 * swing into a target that holds still. */
static void pace_low_update(unsigned *lo, uint64_t *lo_us, unsigned v,
                            uint64_t now)
{
   if (!v)
      return;
   if (!*lo || v <= *lo)
   {
      *lo    = v;
      *lo_us = now;               /* restart the hold on every new low */
      return;
   }
   if (now - *lo_us >= PACE_LOW_HOLD_US)
   {
      /* Deliberately does NOT restart the hold: once the quiet period has
       * elapsed the mark keeps creeping up one step per window until it
       * meets the current value. */
      *lo += PACE_LOW_RISE_X100;
      if (*lo > v)
         *lo = v;
   }
}

/* Everything except the applied target and its audio step: those two are the
 * only state a glide is allowed to carry across a session boundary, and in
 * fixed-rate mode they must, or leaving a session would be a step change. */
static void pace_reset_state(void)
{
   g_pace_engaged     = 0;
   g_pace_cap_x100    = 0;
   g_pace_peer_x100   = 0;
   g_pace_self_lo     = 0;
   g_pace_peer_lo     = 0;
   g_pace_self_lo_us  = 0;
   g_pace_peer_lo_us  = 0;
   g_pace_acc         = 0;
   g_pace_win_vb      = 0;
   g_pace_floor_said  = 0;
   g_pace_miss_said   = 0;
   g_pace_win_us      = net_now_us();
   g_pace_win_frames  = fe_host_frame_count();
   g_pace_log_us      = g_pace_win_us;
   g_pace_ramp_us     = g_pace_win_us;
   fe_np_set_local_fps(0);            /* "unknown" until the first window */
}

/* Snap everything back to nominal.  Used by mode 0/2, where a session
 * boundary has always been a step (mode 2's ramp only ever moved the target
 * *within* a session). */
static void pace_reset(void)
{
   pace_reset_state();
   g_pace_target_x100 = PACE_NOMINAL_X100;
   g_pace_goal_x100   = PACE_NOMINAL_X100;
   pace_audio_step(PACE_NOMINAL_X100);
}

/* ADR-0035 — SNAP THE REQUESTED RATE TO ONE THE THROTTLE CAN ACTUALLY HOLD.
 *
 * The field ran a 40.00 target and achieved 35-38 fps with `self_cap=52-57`,
 * i.e. the console could have gone faster and still missed.  That is not a
 * control bug, it is QUANTIZATION.  We pace by waiting whole vblanks, so a
 * frame costs 1 or 2 of them; averaging 40 needs ~half the frames to finish
 * inside one 16.68 ms vblank, and when per-frame work sits just ABOVE that,
 * none do — every frame becomes 2 vblanks and the average collapses toward
 * 29.97 with jitter from the frames that occasionally take 3.
 *
 * The rates a console can hold with NO headroom assumption are exactly
 * `PACE_VBLANK_HZ / N`.  That set is sparse — 59.94, 29.97, 19.98 — and there
 * is deliberately nothing between nominal and 29.97: with whole-vblank pacing
 * there CANNOT be.  Anything else is only reachable when frames genuinely fit
 * in one vblank, which is exactly the assumption the field just falsified.
 *
 * NOTE FOR ANYONE READING THE FIELD CONFIG: 2 vblanks is **29.97**, not 29.86.
 * 29.86 is 59.7275/2 — the GBA's own frame rate halved — but the thing we
 * insert is a PSP DISPLAY vblank at 59.94 Hz, so the achievable rate is
 * 59.94/2.  A configured 29.86 snaps to 29.97 and the snap is logged; the
 * 0.11 fps is not worth a second mechanism, but the log must not be silent
 * about it or the next reader will think the clamp drifted.
 *
 * Returns the applied rate.  `*vb` receives the vblanks-per-frame it implies
 * (1 = nominal, i.e. not pacing at all). */
static unsigned pace_snap_x100(unsigned req_x100, unsigned *vb)
{
   unsigned n, best_n = 1, best_err = 0, first = 1;

   if (vb)
      *vb = 1;
   if (!g_pcfg.net_session_fps_snap)
      return req_x100;                 /* A/B: apply the request verbatim */

   /* A request within the engage band of nominal is not a request to pace,
    * and snapping it DOWN to 29.97 would be a wild overreaction. */
   if (req_x100 + PACE_ENGAGE_X100 > PACE_NOMINAL_X100)
      return PACE_NOMINAL_X100;

   /* Otherwise the user does want pacing, so never snap back UP to nominal:
    * search N >= 2 only.  Nearest by ERROR IN THE RATE, not by rounding the
    * divisor — rounding 59.94/40.00 = 1.4985 to N=1 would hand back nominal
    * and silently cancel the clamp. */
   for (n = 2; n <= PACE_SNAP_MAX_VB; n++)
   {
      unsigned rate = PACE_VBLANK_HZ_X100 / n;
      unsigned err  = (rate > req_x100) ? rate - req_x100 : req_x100 - rate;
      if (first || err < best_err)
      {
         first    = 0;
         best_err = err;
         best_n   = n;
      }
   }
   if (vb)
      *vb = best_n;
   return PACE_VBLANK_HZ_X100 / best_n;
}

/* `EVT session_pace fps=<applied goal> reason=<session_start|session_end>` —
 * ADR-0033's one line for "the clamp changed".  It reports the rate being
 * ramped TO, which is the number that matters; `EVT pace_match ... why=ramp`
 * is not emitted in fixed mode because there is nothing to negotiate. */
static void pace_session_evt(const char *reason)
{
   fe_evt("session_pace fps=%u.%02u reason=%s",
          g_pace_goal_x100 / 100, g_pace_goal_x100 % 100, reason);
}

static void pace_session_begin(void)
{
   /* ADR-0078: BACKPRESSURE is FIXED plus a floating clamp; it shares every
    * piece of session lifecycle with FIXED, including this one.  The ini
    * rate is the STARTING base; the control law moves it from there. */
   if (g_pcfg.net_pace_match != PACE_MODE_FIXED &&
       g_pcfg.net_pace_match != PACE_MODE_BACKPRESSURE)
   {
      pace_reset();
      return;
   }
   g_bp_low_streak = 0;
   g_bp_hold_wins  = 0;
   pace_reset_state();
   {
      unsigned req = (unsigned)g_pcfg.net_session_fps_x100, vb = 1;
      g_pace_fixed_x100 = pace_snap_x100(req, &vb);
      if (g_pace_fixed_x100 > PACE_NOMINAL_X100)
         g_pace_fixed_x100 = PACE_NOMINAL_X100;
      /* Log the snap whenever it moved the number.  A user who configured
       * 40.00 must be TOLD they are getting 29.97 — silently under-running a
       * requested rate is what produced the field's 35-38 fps mystery. */
      if (g_pace_fixed_x100 != req)
         fe_evt("session_pace_snap req=%u.%02u applied=%u.%02u vblanks=%u "
                "(achievable rates are %u.%02u/N)",
                req / 100, req % 100,
                g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100, vb,
                PACE_VBLANK_HZ_X100 / 100, PACE_VBLANK_HZ_X100 % 100);
   }
   g_pace_goal_x100 = g_pace_fixed_x100;
   /* The target is NOT snapped: it glides down from wherever it is (nominal,
    * or mid-glide if a previous session ended seconds ago). */
   pace_session_evt("session_start");
}

static void pace_session_end(void)
{
   if (g_pcfg.net_pace_match != PACE_MODE_FIXED &&
       g_pcfg.net_pace_match != PACE_MODE_BACKPRESSURE)
   {
      pace_reset();
      return;
   }
   pace_reset_state();
   g_pace_goal_x100 = PACE_NOMINAL_X100;
   pace_session_evt("session_end");
   /* The glide back up runs from the main loop after teardown — see
    * pace_fixed_ramp(), which is deliberately NOT gated on g_net_up. */
}

static void pace_log(const char *why)
{
   /* `*_lo` are the sustained worsts the target is actually derived from —
    * log them beside the instantaneous values so the field can see directly
    * whether the low-water marks are doing their job (ADR-0028). */
   fe_evt("pace_match target=%u.%02u self_cap=%u.%02u peer_cap=%u.%02u "
          "self_lo=%u.%02u peer_lo=%u.%02u engaged=%d why=%s",
          g_pace_target_x100 / 100, g_pace_target_x100 % 100,
          g_pace_cap_x100 / 100,    g_pace_cap_x100 % 100,
          g_pace_peer_x100 / 100,   g_pace_peer_x100 % 100,
          g_pace_self_lo / 100,     g_pace_self_lo % 100,
          g_pace_peer_lo / 100,     g_pace_peer_lo % 100,
          g_pace_engaged, why);
   g_pace_log_us = net_now_us();
}

/* Accumulates this frame's work cost and closes a ~1 s measurement window.
 * Returns 1 exactly when a window just closed, having refreshed
 * `g_pace_cap_x100` (CAPABILITY — see the header) and published it to the
 * peer; `*achieved_x100` is then the rate this console actually DELIVERED
 * over the window (wall clock, throttle waits included), which is the number
 * the fixed-rate miss check needs and the one capability deliberately is not.
 *
 * Both policies share this: mode 2 needs the capability, mode 1 needs the
 * achieved rate, and both want `self_cap` in `sess_cost` either way. */
static int pace_window(uint32_t work_us, unsigned *achieved_x100)
{
   uint64_t now, dt;
   unsigned frames, sample, vb;

   /* Cost this frame in whole vblank periods — what the loop actually pays,
    * and what a free-running loop would pay.  Never less than one. */
   vb = (unsigned)((work_us + PACE_VBLANK_US - 1) / PACE_VBLANK_US);
   if (vb < 1)
      vb = 1;
   g_pace_win_vb += vb;

   if (ui_active())
   {
      /* Core paused, wall clock still moving: re-base or the next window
       * divides real seconds by zero frames (the same trap skip_policy_frame
       * fell into in the field). */
      g_pace_win_us     = net_now_us();
      g_pace_win_frames = fe_host_frame_count();
      g_pace_win_vb     = 0;
      return 0;
   }

   now = net_now_us();
   dt  = now - g_pace_win_us;
   if (dt < PACE_WIN_US)
      return 0;

   frames = fe_host_frame_count() - g_pace_win_frames;
   if (!frames || !g_pace_win_vb)
   {
      g_pace_win_us     = now;
      g_pace_win_frames = fe_host_frame_count();
      g_pace_win_vb     = 0;
      return 0;
   }

   /* CAPABILITY, not achieved fps: our own throttle waits are not work and
    * so cannot appear here.  This is what stops the pair ratcheting down. */
   sample = (unsigned)((uint64_t)frames * PACE_VBLANK_HZ_X100 / g_pace_win_vb);
   if (sample > PACE_NOMINAL_X100)
      sample = PACE_NOMINAL_X100;      /* we never run the game faster */
   g_pace_cap_x100 = g_pace_cap_x100
      ? (g_pace_cap_x100 * (PACE_EMA_DEN - 1) + sample) / PACE_EMA_DEN
      : sample;

   /* ACHIEVED: frames per wall second, x100.  dt >= PACE_WIN_US so no /0. */
   *achieved_x100 = (unsigned)((uint64_t)frames * 100000000ull / dt);

   g_pace_win_us     = now;
   g_pace_win_frames = fe_host_frame_count();
   g_pace_win_vb     = 0;

   /* Publish OUR capability. The peer paces against this. */
   fe_np_set_local_fps(g_pace_cap_x100);
   return 1;
}

/* ADR-0033: glide the applied rate toward the goal on a per-frame time base.
 * Runs whether or not a session is up — the glide OUT happens after teardown
 * by construction, and gating this on `g_net_up` would turn leaving a session
 * back into the step change the ramp exists to avoid. */
static void pace_fixed_ramp(void)
{
   uint64_t now = net_now_us(), dt;
   unsigned step, was = g_pace_target_x100;

   /* Pre-boot both of these are still zero, and this runs from the FIRST
    * frame — before any session, because the glide out has to.  A zero goal
    * reads as "ramp to 0 fps", so the loop would throttle a solo game to the
    * PACE_MAX_EXTRA_VB floor with no session in sight.  Nominal is the only
    * safe reading of "not set yet"; the session sets a real goal. */
   if (!g_pace_target_x100)
      g_pace_target_x100 = PACE_NOMINAL_X100;
   if (!g_pace_goal_x100)
      g_pace_goal_x100 = PACE_NOMINAL_X100;
   if (!g_pace_ramp_us)
   {
      g_pace_ramp_us = now;
      return;
   }
   if (g_pace_target_x100 == g_pace_goal_x100)
   {
      g_pace_ramp_us = now;                       /* parked: no debt banked */
      return;
   }

   dt   = now - g_pace_ramp_us;
   step = (unsigned)((uint64_t)PACE_FIXED_RAMP_X100_PER_S * dt / 1000000ull);
   if (!step)
      return;   /* sub-step: leave g_pace_ramp_us alone so dt keeps growing,
                 * or a fast enough loop would never accumulate a whole step */
   g_pace_ramp_us = now;

   if (g_pace_target_x100 > g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_target_x100 - g_pace_goal_x100 > step)
            ? g_pace_target_x100 - step : g_pace_goal_x100;
   else
      g_pace_target_x100 =
         (g_pace_goal_x100 - g_pace_target_x100 > step)
            ? g_pace_target_x100 + step : g_pace_goal_x100;

   if (g_pace_target_x100 != was)
   {
      /* Audio follows the APPLIED rate exactly (ADR-0027 §audio): production
       * and consumption then match and the stream stays continuous. */
      pace_audio_step(g_pace_target_x100);
      /* `engaged` is a report only, same as mode 2. */
      g_pace_engaged =
         (g_pace_target_x100 + PACE_ENGAGE_X100 <= PACE_NOMINAL_X100);
      if (g_pace_target_x100 == g_pace_goal_x100)
         pace_session_evt(g_pace_goal_x100 >= PACE_NOMINAL_X100
                          ? "ramp_done_nominal" : "ramp_done");
   }
}

/* ADR-0033 sanity: report a console that cannot hold the fixed rate, and do
 * NOT chase it downward.  Chasing is exactly the moving target this policy
 * removed, and the user prefers a known steady rate to a correct-but-moving
 * one.  Only checked once the glide has settled — during the ramp the
 * achieved rate is legitimately not the clamp yet. */
static void pace_fixed_miss(unsigned achieved_x100, uint64_t now)
{
   if (g_pace_target_x100 != g_pace_goal_x100 || !achieved_x100)
      return;
   if (achieved_x100 + PACE_MISS_MARGIN_X100 >= g_pace_goal_x100)
   {
      g_pace_miss_said = 0;            /* recovered: re-arm the report */
      return;
   }
   if (g_pace_miss_said && now - g_pace_log_us < PACE_MISS_LOG_US)
      return;
   g_pace_miss_said = 1;
   g_pace_log_us    = now;
   fe_evt("session_pace_miss actual=%u.%02u fixed=%u.%02u self_cap=%u.%02u"
          " — not chasing",
          achieved_x100 / 100,     achieved_x100 % 100,
          g_pace_goal_x100 / 100,  g_pace_goal_x100 % 100,
          g_pace_cap_x100 / 100,   g_pace_cap_x100 % 100);
}

/* ADR-0078: one window's worth of backpressure decision.  Adjusts the
 * FLOATING base (g_pace_fixed_x100) that the fixed-mode glide then follows;
 * never touches g_pace_target_x100 directly, so audio and throttle stay in
 * the lockstep pace_extra_vblanks() documents. */
static void pace_bp_window(void)
{
   uint32_t q = fe_np_txq_now();
   unsigned was = g_pace_fixed_x100;
   const char *why = NULL;

   if (!g_net_is_host)
      return;                     /* join: plain FIXED at the ini ceiling */

   if (q >= PACE_BP_HI)
   {
      g_bp_low_streak = 0;
      if (g_pace_fixed_x100 > PACE_BP_FLOOR_X100)
      {
         g_pace_fixed_x100 =
            (g_pace_fixed_x100 - PACE_BP_FLOOR_X100 > PACE_BP_STEP_DOWN_X100)
               ? g_pace_fixed_x100 - PACE_BP_STEP_DOWN_X100
               : PACE_BP_FLOOR_X100;
         why = "down";
      }
   }
   else if (q <= PACE_BP_LO)
   {
      if (++g_bp_low_streak >= PACE_BP_RISE_WINS)
      {
         g_bp_low_streak = 0;
         if (g_pace_fixed_x100 < PACE_NOMINAL_X100)
         {
            g_pace_fixed_x100 =
               (PACE_NOMINAL_X100 - g_pace_fixed_x100 > PACE_BP_STEP_UP_X100)
                  ? g_pace_fixed_x100 + PACE_BP_STEP_UP_X100
                  : PACE_NOMINAL_X100;
            why = "up";
         }
      }
   }
   else
      g_bp_low_streak = 0;

   if (why)
   {
      g_pace_goal_x100 = g_pace_fixed_x100;
      g_bp_hold_wins   = 0;
      fe_evt("pace_bp txq=%u base=%u.%02u was=%u.%02u why=%s",
             (unsigned)q, g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100,
             was / 100, was % 100, why);
   }
   else if (++g_bp_hold_wins >= 10)
   {
      /* ADR-0058's rule: a probe whose silence looks like a dead instrument
       * is not evidence.  One hold line per ~10 s says the law is running. */
      g_bp_hold_wins = 0;
      fe_evt("pace_bp txq=%u base=%u.%02u why=hold streak=%u",
             (unsigned)q, g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100,
             g_bp_low_streak);
   }
}

/* Called once per main-loop iteration with the work time of the frame just
 * finished (vblank waits excluded). */
static void pace_frame(uint32_t work_us)
{
   uint64_t now;
   unsigned achieved = 0;
   unsigned peer, desired;
   int was_engaged;
   unsigned was_target;

   if (g_pcfg.net_pace_match == PACE_MODE_FIXED ||
       g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE)
   {
      pace_fixed_ramp();               /* every frame, session or not */
      if (g_net_up && pace_window(work_us, &achieved))
      {
         /* Nothing is negotiated in this mode, but keep reading the peer's
          * advertised capability: `sess_cost pace=` still reports it, and
          * "what could the pair actually have sustained" is the one question
          * a fixed clamp cannot answer for itself. */
         peer = fe_np_peer_min_fps();
         g_pace_peer_x100 =
            (peer < PACE_PEER_MIN_X100 || peer > PACE_PEER_MAX_X100) ? 0 : peer;
         if (g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE)
            pace_bp_window();          /* ADR-0078: move the base, maybe */
         pace_fixed_miss(achieved, net_now_us());
      }
      return;
   }

   if (!g_net_up)
      return;
   if (!pace_window(work_us, &achieved))
      return;

   was_engaged = g_pace_engaged;
   was_target  = g_pace_target_x100;
   now         = net_now_us();

   /* ---- decide the goal ------------------------------------------------ */
   peer = fe_np_peer_min_fps();
   if (peer < PACE_PEER_MIN_X100 || peer > PACE_PEER_MAX_X100)
      peer = 0;                        /* absurd, stale or silent: ignore */
   g_pace_peer_x100 = peer;

   /* Both sides' sustained worst.  Ours is tracked even with no peer, so a
    * target is available the instant one appears. */
   pace_low_update(&g_pace_self_lo, &g_pace_self_lo_us, g_pace_cap_x100, now);
   if (peer)
      pace_low_update(&g_pace_peer_lo, &g_pace_peer_lo_us, peer, now);

   if (!g_pcfg.net_pace_match || !peer || !g_pace_peer_lo)
   {
      g_pace_goal_x100 = PACE_NOMINAL_X100;
      g_pace_engaged   = 0;
   }
   else
   {
      /* THE PAIR'S sustained worst, not "the peer's".  min() is symmetric,
       * so both consoles land on the same number and a role flip does not
       * move it (ADR-0028). */
      unsigned pair = (g_pace_self_lo && g_pace_self_lo < g_pace_peer_lo)
                      ? g_pace_self_lo : g_pace_peer_lo;

      desired = pair + PACE_MARGIN_X100;
      if (desired < PACE_FLOOR_X100)
      {
         /* CLAMP, never release: snapping back to nominal against a peer
          * this slow widens the gap instead of closing it. */
         desired = PACE_FLOOR_X100;
         if (!g_pace_floor_said)
         {
            g_pace_floor_said = 1;
            fe_evt("pace_floor pair=%u.%02u floor=%u.%02u — clamped, still pacing",
                   pair / 100, pair % 100,
                   PACE_FLOOR_X100 / 100, PACE_FLOOR_X100 % 100);
         }
      }
      else
         g_pace_floor_said = 0;
      if (desired > PACE_NOMINAL_X100)
         desired = PACE_NOMINAL_X100;

      g_pace_goal_x100 = desired;

      /* `engaged` only REPORTS that the goal is meaningfully below nominal;
       * the throttle follows the applied target, so this cannot cause the
       * engage/release cycling the field showed. */
      if (!g_pace_engaged)
      {
         if (g_pace_goal_x100 + PACE_ENGAGE_X100 <= PACE_NOMINAL_X100)
            g_pace_engaged = 1;
      }
      else if (g_pace_goal_x100 + PACE_RELEASE_X100 > PACE_NOMINAL_X100)
         g_pace_engaged = 0;
   }

   /* ---- ramp the applied target: fall fast, rise slow ------------------- */
   if (g_pace_target_x100 > g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_target_x100 - g_pace_goal_x100 > PACE_RAMP_DOWN_X100)
            ? g_pace_target_x100 - PACE_RAMP_DOWN_X100 : g_pace_goal_x100;
   else if (g_pace_target_x100 < g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_goal_x100 - g_pace_target_x100 > PACE_RAMP_UP_X100)
            ? g_pace_target_x100 + PACE_RAMP_UP_X100 : g_pace_goal_x100;

   if (g_pace_target_x100 != was_target)
      pace_audio_step(g_pace_target_x100);

   if (g_pace_target_x100 != was_target)
      pace_log(g_pace_engaged != was_engaged
               ? (g_pace_engaged ? "engage" : "release") : "ramp");
   else if (g_pace_engaged != was_engaged)
      pace_log(g_pace_engaged ? "engage" : "release");
   else if (g_pace_engaged && now - g_pace_log_us >= PACE_LOG_US)
      pace_log("hold");
}

/* How many EXTRA vblanks this frame should wait for, beyond the one the loop
 * always takes.  Fractional accumulator: see the header comment. */
static unsigned pace_extra_vblanks(void)
{
   unsigned v_x10000, frac, n = 0;

   if (!g_pcfg.net_pace_match)
      return 0;
   /* Mode 2's target is snapped back to nominal at teardown, so this is
    * belt-and-braces there.  In FIXED mode there is deliberately no g_net_up
    * gate: the glide out of the clamp runs after teardown, and cutting the
    * throttle the instant the session ends — while the audio step is still
    * several seconds from nominal — is precisely the ring-overflow the
    * lockstep note below is about. */
   if (g_pcfg.net_pace_match == PACE_MODE_ADAPTIVE && !g_net_up)
      return 0;
   /* Gate on the APPLIED TARGET, not on `engaged`.  `engaged` moves the goal;
    * the target is what both this throttle and the audio step follow, and
    * they must stay in lockstep.  Releasing by clearing `engaged` alone would
    * drop the throttle instantly while the audio step was still ramping back
    * over several seconds — the emulator would then outrun the consumer and
    * the ring would overflow (dropped tail = clicks) for the whole ramp. */
   if (g_pace_target_x100 >= PACE_NOMINAL_X100 || g_pace_target_x100 == 0)
      return 0;                        /* not throttling: leave pacing alone */

   v_x10000 = (unsigned)((uint64_t)PACE_VBLANK_HZ_X100 * 10000u
                         / g_pace_target_x100);
   frac = (v_x10000 > 10000u) ? v_x10000 - 10000u : 0u;
   g_pace_acc += frac;
   while (g_pace_acc >= 10000u && n < PACE_MAX_EXTRA_VB)
   {
      g_pace_acc -= 10000u;
      n++;
   }
   if (g_pace_acc > 20000u)
      g_pace_acc = 0;                  /* pathological target: never bank debt */
   return n;
}

/* ADR-0034: `EVT blit_prof` on the same 600-frame cadence as `core_prof`, so
 * the two line up in the field log and the blit's share of the frame can be
 * read directly against `core_phase`'s `blt` phase.
 *
 * `stage` is the CPU-side conversion (plus, in mode 0 only, the 82 KiB cache
 * writeback) — the part `blit_mode` moves.  `gu` is the list build and the
 * sceGuSync that blocks until the GE has finished; no placement change can
 * shorten that, so if `gu` dominates then the staging A/B is the wrong hunt
 * and the answer is in the GE, not the copy.  Reporting them separately is
 * the point: one number could not tell those two stories apart. */
static void blit_prof_evt(unsigned frames)
{
   static unsigned last;
   unsigned n, st, stm, gu, gum, wt, wtm;

   if (!frames || frames == last || (frames % 600) != 0)
      return;
   last = frames;
   vid_blit_prof(&n, &st, &stm, &gu, &gum, &wt, &wtm);
   if (!n)
      return;
   /* `wait` is a SUBSET of `gu` (ADR-0038), not another term of `tot` — the
    * sum below is deliberately still stage+gu.  Read it as: of `gu`, `wait`
    * is stalled-on-the-GE and the remainder is list building. */
   fe_evt("blit_prof mode=%d name=%s n=%u stage=%u/%u gu=%u/%u wait=%u/%u "
          "tot=%u",
          vid_blit_mode(), vid_blit_mode_name(vid_blit_mode()), n,
          st / n, stm, gu / n, gum, wt / n, wtm, (st + gu) / n);
}

/* Full wireless bring-up around the booted core.  Returns 0 on success. */
static int net_bringup(int is_host, const char *group, const char *nick,
                       int probe)
{
   int rc;

   if (g_save_path_for_backup)
      backup_save(g_save_path_for_backup);

   /* ADR-0021: keep sceNetAdhocPdpSend off the emulation thread unless the
    * user turned it off for an A/B.  Must precede init (the thread is part
    * of the bring-up ladder). */
   adhoc_transport_set_tx_thread(g_pcfg.net_tx_thread);

   rc = adhoc_transport_init(group, 0);
   if (rc != ADHOC_OK)
   {
      net_error_evt("adhoc_init", rc);
      return rc;
   }
   log_adhoc_up(group);

   if (net_start_np(is_host, group, nick, probe) != 0)
   {
      adhoc_transport_term();
      return -1;
   }
   skip_policy_session_begin();          /* ADR-0019 (supersedes ADR-0018) */
   /* Order matters: the mode line first (it names the policy that is about to
    * act), then the policy's own `EVT session_pace`. */
   {
      /* Report the rate that will actually be APPLIED, with the request
       * beside it — ADR-0035: the two differ whenever the snap fires, and a
       * line that showed only the request would be the same silent lie the
       * field caught. */
      unsigned vb = 1;
      unsigned req  = (unsigned)g_pcfg.net_session_fps_x100;
      unsigned appl = pace_snap_x100(req, &vb);
      fe_evt("net_pace_match mode=%d policy=%s nominal=%u.%02u floor=%u.%02u "
             "fixed=%u.%02u req=%u.%02u snap=%d vblanks=%u ramp=%u.%02u",
             g_pcfg.net_pace_match,
             g_pcfg.net_pace_match == PACE_MODE_FIXED        ? "fixed"    :
             g_pcfg.net_pace_match == PACE_MODE_ADAPTIVE     ? "adaptive" :
             g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE ? "backpressure"
                                                             : "off",
             PACE_NOMINAL_X100 / 100, PACE_NOMINAL_X100 % 100,
             PACE_FLOOR_X100 / 100, PACE_FLOOR_X100 % 100,
             appl / 100, appl % 100, req / 100, req % 100,
             g_pcfg.net_session_fps_snap, vb,
             PACE_FIXED_RAMP_X100_PER_S / 100,
             PACE_FIXED_RAMP_X100_PER_S % 100);
   }
   pace_session_begin();                            /* ADR-0033 / ADR-0027 */
   fe_evt("net_tx_thread mode=%d", adhoc_transport_tx_thread_active());

   /* Netdrv (ADR-0016 profile: 144 B slots, 384-deep ring) + the 128 KiB
    * sceNet pool just landed: log what is left of the ADR-0007 headroom. */
   fe_evt("mem_free=%d max_block=%d net=up",
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
   return 0;
}

/* Disconnect-crash diagnosis: transport_adhoc.c calls this before each blocking
 * WLAN teardown step.  fe_evt flushes every line, so a mid-run Disconnect that
 * wedges (the force-quit black screen) leaves the offending step as the LAST
 * line in the log — turning an un-reproducible hang into a one-run pinpoint. */
void gpsp_adhoc_step(const char *step)
{
   fe_evt("adhoc_step name=%s", step);
}

static void net_teardown(void)
{
   if (!g_net_up)
      return;
   fe_evt("td_step name=begin");
   adhoc_stats_evt();
   sess_cost_evt(1);                                         /* ADR-0021 */
   fe_evt("td_step name=pre_np_stop");
   fe_np_stop();
   fe_evt("td_step name=pre_transport_term");
   adhoc_transport_term();
   fe_evt("td_step name=post_transport_term");
   g_net_up = 0;
   skip_policy_session_end();                               /* ADR-0019 */
   fe_evt("td_step name=post_skip_policy");
   /* ADR-0033: in fixed mode this only re-aims the goal at nominal — the
    * glide back up runs from the main loop over the next ~5 s.  In mode 0/2
    * it is the old hard reset. */
   pace_session_end();
   fe_evt("td_step name=post_pace");
   osd_chip_session(NULL);
   fe_evt("td_step name=done");
}

/* Per-frame wireless service, called from the main loop (its own counter:
 * core frames freeze while the menu is open, the pump must not). */
static void net_frame(void)
{
   static unsigned ticks;
   if (!g_net_up)
      return;
   ticks++;
   fe_np_pump();                          /* delivers + emits net_stats */

   /* The driver could not honour the RELIABLE contract: the core's RFU
    * state is unrecoverable from here (ADR-0016).  Say so and tear down
    * rather than keep a dead link nominally "up" — the silent version of
    * this is what produced the field's mystery comm-errors. */
   if (fe_np_failed())
   {
      osd_toast("Wireless error: link overloaded");
      net_teardown();
      return;
   }

   if ((ticks % 60) == 0)
      scePowerTick(PSP_POWER_TICK_ALL);                    /* no suspend
                                            mid-session (plan §9) */
   if ((ticks % 600) == 0)
   {
      adhoc_stats_evt();
      sess_cost_evt(0);                    /* ADR-0021 */
   }
   skip_policy_frame();                   /* ADR-0019 */
   if (!adhoc_transport_connected() && !g_group_lost_logged)
   {
      /* adhocctl DISCONNECT event (group dissolved / radio lost): netdrv
       * will also see peer death — log the root cause once. */
      fe_evt("adhoc_group_lost");
      osd_toast("Wireless group lost");
      g_group_lost_logged = 1;
   }
}

/* ----- silent wireless (per-game variant builds; ADR-0013) ---------------
 * When the game completes the RFU adapter login (weak core hook), the
 * frontend auto-negotiates a session on a fixed group with zero UI:
 * join first, and if no host answers within a jittered window, promote to
 * host (contention backoff: the jitter makes simultaneous activations
 * pick different promote times, so one side becomes host first and the
 * other's still-running JOIN retry loop latches onto it).  role=host/join
 * in variant.ini pins the role instead.  WLAN off => persistent overlay
 * warning, no retry storm. */

/* rfu.c, ADR-0042.  The core has no header; the frontend/core interface here
 * is a set of loose externs, so this follows the existing convention. */
extern void rfu_set_rx_cap(unsigned n);
/* ADR-0075.  At most N non-empty host-packet deliveries to the game per
 * emulated frame, gated consistently in RECV_DATA and rfu_data_avail() so the
 * WAITEVENT re-poll spin that sank the rfu_rx_cap arms cannot occur.  1 equals
 * both the game's drain rate and the real radio's cadence.  0 = off. */
extern void rfu_set_frame_pace(unsigned n);
extern void rfu_set_cushion(unsigned n);     /* fixed-depth jitter buffer PoC */
extern void adhoc_set_disc_await(int on);    /* candidate mid-run-Disconnect fix */
/* ADR-0059.  Percent, 100 = stock.  Stretches the emulated adapter's two
 * cycle-counted deadlines to compensate for a radio ~10x slower than the one
 * the game was written for.  See the long comment in rfu.c. */
extern void rfu_set_timeout_scale(unsigned to_x100, unsigned rtx_x100);
/* ADR-0062d.  Minimum emulated cycles between netpacket_poll_receive() calls
 * while the adapter waits.  0 = every call, the historical behaviour. */
extern void rfu_set_poll_min_cycles(unsigned n);
/* ADR-0068.  The mirror knob: minimum emulated cycles between polls while the
 * adapter is NOT waiting — the host's cadence, which is otherwise once per
 * frame.  0 = never = the historical behaviour.  See rfu.c. */
extern void rfu_set_idle_poll_cycles(unsigned n);
/* ADR-0074.  1 = hold a peer disconnect until the client's queued host packets
 * have been handed to the game, so the Union Room's exit negotiation is not
 * deleted in transit.  0 = the historical immediate teardown. */
extern void rfu_set_disc_defer(unsigned n);
/* ADR-0076.  Hold the adapter in CLIENT state for N frames after the peer's
 * disconnect so the game can finish its clock-master change and issue its OWN
 * disconnect -- the clean, error-free exit.  The lobby error is a race the
 * slow client loses (link_rfu_2.c:2492 fires unconditionally when our IDLE
 * answer wins); this gives the game the window the real exit sequence needs.
 * 0 = off = historical.  60 (~1 s) is the arm to try first. */
extern void rfu_set_disc_grace(unsigned frames);

/* ADR-0043 flight recorder.  Called from the EMULATION thread at the moment
 * the adapter is reset out of an active session — i.e. after the game has
 * already declared its fatal error.  Writing the log synchronously from here
 * violates the standing "no ms0: I/O on the emu thread" rule, and that is
 * deliberate and confined to this one call: the session is dead, there is no
 * frame timing left to protect, and the alternative is losing the only record
 * of what happened.  Emits a compact decoded transcript, oldest first. */
/* ADR-0045: the core reads this once per emulated frame, never per command. */
unsigned gpsp_rfu_now_us(void)
{
   return (unsigned)net_now_us();
}

void gpsp_rfu_flight_dump_hook(const unsigned *ring, unsigned cap,
                               unsigned start, unsigned n)
{
   static const char *stn[] = { "idle", "host", "conn", "clnt" };
   unsigned i;
   unsigned prev_frame = 0, prev_us = 0;
   unsigned worst_ms = 0, worst_frame = 0;
   int have_prev = 0;

   /* EIGHT entries per line, not one.
    *
    * One-per-line lost 228 of 1024 entries to the event queue on the very
    * first gated run -- `evtio=.../228/16383` -- and lost them SILENTLY: the
    * transcript simply had holes, in a diagnostic whose entire value is being
    * complete.  Packing cuts the line count 8x, which both fits the queue and
    * makes the dump readable.
    *
    * Field order per entry:  <cmd>:<state>:<gap-in-frames>
    * `cmd` >= 0xF0 is a state transition and its low nibble is the cause.
    *
    * Each LINE also carries `+<ms>`: wall-clock milliseconds since the
    * previous line.  ADR-0045 -- the frame gap alone is blind to the failure
    * the user could see happening.  During a save the core trips SMC detection
    * ~4096 times and a frame stretches to 38 ms against a 33.4 ms budget; in
    * frame-count that is identical to a healthy 16 ms frame, so a transcript
    * can read "perfectly regular" while the console falls behind a peer that
    * does not slow down with it.  Real time is the axis the peer lives on. */
   fe_evt("rfu_flight begin n=%u first=%u net=%s fmt=cmd:state:gap +ms=wallclock",
          n, start, g_net_up ? "up" : "down");

   for (i = 0; i < n; i += 8)
   {
      char line[224];
      int  off = 0;
      unsigned j;
      unsigned e0   = ((start + i) % cap) * 2;
      unsigned base = ring[e0] & 0x3FFFFF;
      unsigned bus  = ring[e0 + 1];
      unsigned dms  = have_prev ? (bus - prev_us) / 1000u : 0u;

      for (j = i; j < n && j < i + 8; j++)
      {
         unsigned e     = ((start + j) % cap) * 2;
         unsigned cmd   = (ring[e] >> 24) & 0xFF;
         unsigned st    = (ring[e] >> 22) & 0x3;
         unsigned frame =  ring[e] & 0x3FFFFF;
         unsigned gap   = have_prev ? frame - prev_frame : 0;
         int      w     = snprintf(line + off, sizeof(line) - (size_t)off,
                                   "%s%02x:%s:%u",
                                   j == i ? "" : " ", cmd, stn[st], gap);
         if (w < 0 || (size_t)w >= sizeof(line) - (size_t)off)
            break;
         off += w;
         prev_frame = frame;
         prev_us    = ring[e + 1];
         have_prev  = 1;
      }
      if (dms > worst_ms) { worst_ms = dms; worst_frame = base; }
      fe_evt("rfu_fl f=%u +%ums %s", base, dms, line);
   }

   /* The headline for a human: the longest real-time hole in the capture, and
    * where.  Eight commands span well under a frame in steady state, so a line
    * gap much above ~35 ms means the console stopped keeping up with its peer. */
   fe_evt("rfu_flight end worst_gap_ms=%u at_frame=%u", worst_ms, worst_frame);
}

/* ADR-0071: set by UI_ACT_RELAUNCH; acted on after full teardown. */
static int g_relaunch;

volatile int g_rfu_activated;
void gpsp_rfu_activated_hook(void)   /* overrides the weak rfu.c stub */
{
   g_rfu_activated = 1;
}

/* The emulated RFU link went down — i.e. the GAME ended the wireless
 * session (the player walked out of the Union Room), as distinct from us
 * tearing the transport down.  Both looked identical in the field logs.
 * Called from the emulation thread, so only latch here; the main loop
 * does the (fflushing) log write. */
static volatile unsigned g_rfu_down_pending;   /* reason<<8 | slot, +1 */

/* THE EXIT-STATE SNAPSHOT (fs-pace campaign, 2026-08-09).
 *
 * Run 305/309's flight recorder shows the CLIENT'S GAME issuing a deliberate
 * RFU_CMD_DISCONNECT on a healthy link during the Trade Center walk-out, and
 * the user sees the recoverable "press A" screen on both consoles.  The v9
 * fixture probes sample too late: link_rfu_2.c:961 consumes disconnectMode
 * and the post-warp game has re-cleaned status/errorState, so every
 * after-the-fact read is zeros.  The only instant the causal state exists is
 * the disconnect itself — which is EXACTLY when rfu.c calls this hook.
 *
 * So snapshot guest RAM here.  Emulation thread == the core's own thread, so
 * fe_host_mem_read is an in-thread memcpy; five reads, link-down only (a
 * handful per session).  ADDRESSES ARE EMERALD USA (pret symbols branch,
 * validated against production gMain):
 *   0x03000E19 sRfuKeepAliveTimer   61+ = overworld.c:2285's watchdog FIRED
 *   0x03005CE4 gRfu.disconnectMode  2 = RFU_DISCONNECT_ERROR = LinkRfu_
 *                                   FatalError (the watchdog path); 1 = NORMAL
 *   0x030050F1 gRfu.status          2 = CONNECTION_ERROR
 *   0x030050EE gRfu.errorState      1 = OCCURRED -> the press-A screen
 *   0x03000E14 sPlayerKeyInterceptCallback  WHICH wait episode was installed
 *   0x03000E10 sPlayerLinkStates[4] 2 = EXITING_ROOM, per byte
 * Junk values on any other ROM are harmless: this is a read-only harness
 * diagnostic keyed to a log line, not a behaviour change. */
static volatile unsigned g_rfu_down_ka, g_rfu_down_dm, g_rfu_down_st;
static volatile unsigned g_rfu_down_es, g_rfu_down_kcb, g_rfu_down_pls;
static volatile unsigned g_rfu_down_rcl;   /* gRfu.readyCloseLink[0..3] */

void gpsp_rfu_link_down_hook(unsigned reason, unsigned slot)
{
   unsigned char b = 0;
   unsigned w = 0;
   if (fe_host_mem_read(0x03000E19, &b, 1) == 0) g_rfu_down_ka = b;
   if (fe_host_mem_read(0x03005CE4, &b, 1) == 0) g_rfu_down_dm = b;
   if (fe_host_mem_read(0x030050F1, &b, 1) == 0) g_rfu_down_st = b;
   if (fe_host_mem_read(0x030050EE, &b, 1) == 0) g_rfu_down_es = b;
   if (fe_host_mem_read(0x03000E14, &w, 4) == 0) g_rfu_down_kcb = w;
   if (fe_host_mem_read(0x03000E10, &w, 4) == 0) g_rfu_down_pls = w;
   /* Did the peer's READY_CLOSE_LINK arrive before the death?  If rcl shows
    * the host ready while pls shows our own echo missing, the stall is
    * provably in the KEY channel alone, and a state-assist repair (write
    * the local link state the protocol already agreed on) is safe. */
   if (fe_host_mem_read(0x030050E4, &w, 4) == 0) g_rfu_down_rcl = w;
   g_rfu_down_pending = ((reason & 0xFF) << 8) | (slot & 0xFF) | 0x10000;
}

static void rfu_link_down_drain(void)
{
   unsigned v = g_rfu_down_pending;
   if (!v)
      return;
   g_rfu_down_pending = 0;
   fe_evt("rfu_link_down reason=%u slot=%u net=%s",
          (v >> 8) & 0xFF, v & 0xFF, g_net_up ? "up" : "down");
   fe_evt("rfu_exit_state ka=%u dm=%u st=%u es=%u kcb=0x%08x pls=0x%08x "
          "rcl=0x%08x",
          g_rfu_down_ka, g_rfu_down_dm, g_rfu_down_st, g_rfu_down_es,
          g_rfu_down_kcb, g_rfu_down_pls, g_rfu_down_rcl);
}

/* ---- ADR-0079: THE EXIT-ECHO STATE ASSIST -------------------------------
 *
 * The Trade Center exit dies the same way every run, and the death snapshot
 * proved it three times over (runs 311/313/315, byte-identical):
 * `rfu_exit_state ka=61 dm=1 kcb=0x080871c5 pls=0x81818183 rcl=0x0`.
 * Decoded: the join is parked in KeyInterCB_WaitForPlayersToExit; it has
 * ALREADY processed the host's EXIT_ROOM key (peer state 0x83 EXITING_ROOM);
 * only its OWN key's round-trip echo is missing (local 0x81 BUSY); and the
 * 61-frame keepalive watchdog (overworld.c:2285) then declares the link
 * fatally dead — on a link that is alive and idle.
 *
 * WHY THE ECHO CANNOT ARRIVE.  On real hardware the child's key reaches the
 * parent and the parent's SAME exchange returns the aggregated keys: the
 * echo completes inside one frame, before the parent's game can move on.
 * Over a transport with real latency the parent processes the key, sees all
 * players exiting, and advances into the exit script — after which it stops
 * relaying keys — before its last aggregated frame reaches the child.  The
 * echo is not late; it is structurally unsendable.  This is why every run
 * fails and why no watchdog-budget stretch can help.
 *
 * THE ASSIST.  When every condition of the lost-echo state is provably
 * present, write the ONE BYTE the echo would have written.  Conditions,
 * all read from guest RAM each frame (two loads in the common case):
 *   1. `rfu_exit_assist` key on, wireless session up, game running;
 *   2. sPlayerKeyInterceptCallback == KeyInterCB_WaitForPlayersToExit
 *      (0x080871C5 — a thumb pointer unique to this ROM, which also makes
 *      it the ROM-identity gate: no other game can present this value);
 *   3. the LOCAL player's link state is BUSY while the PEER's is
 *      EXITING_ROOM — i.e. the exit is mutually agreed and only the echo
 *      is missing;
 *   4. sRfuKeepAliveTimer >= 45 of 61 — the echo has had ~0.8 s to arrive;
 *      a merely-late echo lands long before this and the assist never fires.
 * Then sPlayerLinkStates[local] = EXITING_ROOM.  The game's own
 * WaitForPlayersToExit sees all-exiting on its next frame and runs the
 * REAL exit: EventScript_DoLinkRoomExit -> READY_CLOSE_LINK handshake with
 * a host that is still linked and waiting for exactly that -> clean
 * disconnect on both sides -> warp + trainer-card stamp.  Nothing is
 * skipped: the game executes its own negotiated exit, one lost byte later.
 *
 * The failure mode if the write is wrong is the status quo (the watchdog
 * fires at 61 as it does today).  Default OFF; Emerald USA only by the
 * pointer gate. */
static int g_exit_assist;              /* harness key rfu_exit_assist */
static unsigned g_exit_assist_n;       /* fired this session */

static void exit_assist_frame(void)
{
   unsigned kcb = 0;
   unsigned char ka = 0, ls[4];
   int local, peer_;

   if (!g_exit_assist || !g_net_up)
      return;
   if (fe_host_mem_read(0x03000E14, &kcb, 4) != 0 || kcb != 0x080871C5u)
      return;
   if (fe_host_mem_read(0x03000E19, &ka, 1) != 0 || ka < 45)
      return;
   if (fe_host_mem_read(0x03000E10, ls, 4) != 0)
      return;
   /* Two-player Trade Center: parent is link player 0, child is 1. */
   local = g_net_is_host ? 0 : 1;
   peer_ = 1 - local;
   if (ls[local] != 0x81 /* BUSY */ || ls[peer_] != 0x83 /* EXITING */)
      return;
   ls[local] = 0x83;
   if (fe_host_mem_write(0x03000E10 + (unsigned)local, &ls[local], 1) == 0)
   {
      g_exit_assist_n++;
      fe_evt("rfu_exit_assist local=%d ka=%u n=%u net=%s",
             local, ka, g_exit_assist_n, g_net_up ? "up" : "down");
   }
}

/* ----- emulated-adapter command/state trace (phase5j) --------------------
 * rfu_link_down only ever reported the four paths that end a *connection*.
 * The event that actually produces gen-3's UNRECOVERABLE link screen is the
 * adapter answering a command with an ERROR: librfu turns any non-zero REQ
 * result into LMAN_MSG_REQ_API_ERROR, pokeemerald maps that to
 * RFU_STATUS_FATAL_ERROR, and CB2_LinkError then sets gWirelessCommType=3 —
 * the variant of the error screen whose only exit is the power switch.  The
 * recoverable "press A" variant needs RFU_STATUS_CONNECTION_ERROR instead.
 * So the whole point of this trace is to make every ERROR answer, and every
 * adapter state transition that could cause one, visible in a field log.
 *
 * Called from the emulation thread; a lock-free SPSC ring keeps it to a
 * store and an index bump, and the main loop does the log write.  Entries
 * are dropped rather than overwritten if the ring fills (the count says so),
 * because the FIRST error is the interesting one. */
#define RFU_TRACE_RING 64
static volatile unsigned g_rfu_tr_buf[RFU_TRACE_RING];
static volatile unsigned g_rfu_tr_head;   /* producer: emulation thread   */
static volatile unsigned g_rfu_tr_tail;   /* consumer: main loop          */
static volatile unsigned g_rfu_tr_lost;

void gpsp_rfu_trace_hook(unsigned ev, unsigned a, unsigned b)
{
   unsigned head = g_rfu_tr_head;
   if (head - g_rfu_tr_tail >= RFU_TRACE_RING)
   {
      g_rfu_tr_lost++;
      return;
   }
   g_rfu_tr_buf[head % RFU_TRACE_RING] =
      ((ev & 0xFF) << 24) | ((a & 0xFFF) << 12) | (b & 0xFFF);
   g_rfu_tr_head = head + 1;
}

static const char *rfu_tr_state_name(unsigned s)
{
   static const char *n[] = { "idle", "host", "connecting", "client" };
   return s < 4 ? n[s] : "?";
}

static void rfu_trace_drain(void)
{
   static unsigned reported_lost;

   while (g_rfu_tr_tail != g_rfu_tr_head)
   {
      unsigned v  = g_rfu_tr_buf[g_rfu_tr_tail % RFU_TRACE_RING];
      unsigned ev = (v >> 24) & 0xFF, a = (v >> 12) & 0xFFF, b = v & 0xFFF;
      g_rfu_tr_tail++;

      switch (ev)
      {
      case 1: /* CMDERR — the one that becomes the fatal screen */
         fe_evt("rfu_cmderr cmd=0x%02x state=%s net=%s",
                a, rfu_tr_state_name(b), g_net_up ? "up" : "down");
         break;
      case 2: /* state transition */
         fe_evt("rfu_state new=%s cause=%u net=%s",
                rfu_tr_state_name(a), b, g_net_up ? "up" : "down");
         break;
      case 3:
         fe_evt("rfu_unkcmd cmd=0x%02x state=%s", a, rfu_tr_state_name(b));
         break;
      case 4:
         fe_evt("rfu_qdrop side=%s slot=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 5:
         fe_evt("rfu_cmd cmd=0x%02x state=%s", a, rfu_tr_state_name(b));
         break;
      case 6: /* adapter login (AgbRFU_SoftReset + AgbRFU_checkID) */
      {
         static const char *ph[] = { "reset", "handshake", "logged_in" };
         fe_evt("rfu_login phase=%s prev_words=%u",
                a < 3 ? ph[a] : "?", b);
         break;
      }
      case 7:
         /* New high-water of RECV_DATA commands served in ONE emulated
          * frame.  The game drains its 32-slot recvQueue once per frame and
          * `full` is a latch that turns into the game's UNRECOVERABLE error
          * screen, so this is the number to watch: steady state is 1, and
          * anything climbing toward 32 is the mechanism. */
         fe_evt("rfu_rxburst frame_max=%u prev=%u net=%s",
                a, b, g_net_up ? "up" : "down");
         break;
      case 8:
         /* The per-frame delivery cap held a packet back for one frame
          * (ADR-0042).  Seeing these is the cap working; seeing none while
          * rfu_rx_cap is set means it never bound and the run tells us
          * nothing about the hypothesis. */
         fe_evt("rfu_rxhold frame_max=%u delivered=%u", a, b);
         break;
      case 9:
         /* New deepest occupancy of an RFU receive queue (ADR-0044).  If this
          * approaches RFU_PKT_QUEUE the next burst will be dropped, and a
          * dropped RFU packet is payload the games cannot recover.  Read it
          * next to rfu_qdrop: qdrop = 0 with a high mark means the depth is
          * merely adequate, not generous. */
         fe_evt("rfu_qhi side=%s depth=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 10:
         /* ADR-0055: a host packet waited this long in our queue before the
          * game consumed it -- the CLIENT'S ANSWER LATENCY, reported as a new
          * high-water with the running mean beside it.
          *
          * This is the quantity the rfu_rx_cap experiment identified as the
          * one that matters.  Capping deliveries deliberately added a couple
          * of frames of answer latency and the transport collapsed: txq_hi
          * 465, spill 81, the project's first net_error.  The protocol
          * punishes a late reply far harder than it punishes volume, so a
          * multi-frame value here is the thing to chase -- not a busy frame. */
         fe_evt("rfu_answer max_us=%u mean_us=%u", a, b);
         break;
      case 11:
         /* ADR-0058: periodic census, emitted whether or not anything moved.
          * n=0 means the probe never measured anything and its placement is
          * wrong; n>0 with max_us=0 means answer latency is genuinely
          * sub-frame.  Case 10 above cannot tell those apart -- it only fires
          * on a new high-water, so an all-zero session is indistinguishable
          * from a dead instrument, which is how runs 6-9 read. */
         fe_evt("rfu_answer_census n=%u max_us=%u", a, b);
         break;
      case 12:
         /* ADR-0059: the CLIENT's emulated adapter told the game
          * RFU_CMD_RESP_TIMEO -- "your peer's data did not arrive in time".
          * This is the central event of CLIFF-FINDINGS' surviving mechanism
          * and it had no hook until now.  n is cumulative; t is the live
          * rfu_timeout in GBA frames, whose wall-clock worth is t/session_fps
          * seconds and therefore SHRINKS as the session rate rises. */
         fe_evt("rfu_timeo n=%u t_frames=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 13:
         /* ADR-0059: the game just configured the adapter's deadlines.  Once
          * per run.  Every timing table in CLIFF-FINDINGS is parameterised on
          * this number; this is the first time it is on the record. */
         fe_evt("rfu_syscfg timeout_frames=%u rtx_max=%u", a, b);
         break;
      case 14:
         /* ADR-0059: the HOST's rfu_resp_timeout (rtx_max/6 emulated frames)
          * expired and we handed the game a synthetic "clients did not
          * answer".  n is in units of 16.  Expected to be busy at every rate;
          * the number that matters is its rate per emulated frame. */
         fe_evt("rfu_noresp n16=%u rtx_max=%u", a, b);
         break;
      case 15:
         /* ADR-0072: HOW OFTEN, AND FOR HOW LONG, THE GAME THREW THE PLAYER'S
          * INPUT AWAY.
          *
          * pokeemerald's KeyInterCB_SelfIdle (src/overworld.c:2520) never
          * reaches KeyInterCB_ReadButtons while GetLinkRecvQueueLength() > 4,
          * and that queue drains exactly once per frame and exists only on the
          * CHILD.  So the client's controls are dead for as long as our
          * deliveries outrun the game's drain rate -- and the host cannot
          * suffer it at all.
          *
          * `n` is frames over the gate in a 600-frame (~10 s) window; `run` is
          * the longest unbroken stretch, which is what a human actually feels
          * as "it ate my input".  RFU_TR_RXBURST already showed peaks of 9 on
          * the join against 1-2 on the host, but a high-water cannot say
          * whether that was constant or a one-off.  This can. */
         /* ADR-0072 fix: peak moved to its own event (case 20) — the ring's
          * 12-bit fields masked the old (peak<<16) packing to zero here too. */
         fe_evt("rfu_rxgate n=%u/%u run_max=%u net=%s", a, 600u, b,
                g_net_up ? "up" : "down");
         break;
      case 16:
         /* ADR-0072: the recoverable "press A to return to lobby" error, the
          * first time it has ever been observable.  slots is the mask we told
          * the game had dropped; state is our adapter state at the moment we
          * said it. */
         fe_evt("rfu_discans slots=0x%x state=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 17:
         /* ADR-0072: CLUMPING, MEASURED AT THE WIRE.
          *
          * `clumped` is host packets that arrived in the SAME emulated frame
          * as the previous one, out of `n`.  The game drains its receive queue
          * once per frame, so a zero frame-delta is one unit of queue growth
          * and a steady one-per-frame stream — at any rate — cannot grow it at
          * all.  This is the number that decides whether OUR delivery pattern
          * can reach pokeemerald's threshold of 4.
          *
          * It exists because RFU_TR_RXBURST could not answer it: that counter
          * increments on every RECV_DATA command, so it measures how hard the
          * game is POLLING, and the client polls hardest when it is waiting
          * and receiving nothing.  A near-zero reading here retires the
          * clumping hypothesis outright. */
         fe_evt("rfu_arrival clumped=%u/%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 18:
         /* ADR-0074: `queued` is how many host packets were still undelivered
          * when the peer's disconnect arrived -- i.e. how much of the Union
          * Room exit negotiation the immediate teardown destroyed.  mode 0 =
          * wiped now (historical), 1 = deferral armed, 2 = deferral applied
          * after the drain.  ADR-0076 adds: 3 = grace window armed, 4 = grace
          * EXPIRED and the historical teardown ran, 5 = the game issued its
          * own disconnect inside the window -- THE RACE WAS WON and the exit
          * was clean.  A healthy grace arm shows 3 followed by 5. */
         fe_evt("rfu_discq queued=%u mode=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 19:
         /* ADR-0075: the pace census.  `n` = frames in a 600-frame window on
          * which frame-paced delivery deferred an eligible packet; `q_hi` =
          * the deepest OUR client queue got on those frames.  Emitted every
          * window while rfu_frame_pace is on, including n=0, so a gate that
          * never bound cannot be mistaken for a dead instrument.  Read next
          * to rfu_arrival: n should track the clumped fraction, and q_hi
          * bounds the extra latency (q_hi extra frames on the last packet of
          * the worst clump). */
         fe_evt("rfu_pace n=%u/%u q_hi=%u net=%s", a, 600u, b,
                g_net_up ? "up" : "down");
         break;
      case 20:
         /* ADR-0072 fix: the GAME's recvQueue-depth PEAK on its own event so
          * the 12-bit ring keeps it (peak>4 = Gate B input-eat, 32 = FATAL).
          * over_gate = frames past the gate this window. */
         fe_evt("rfu_gqpeak peak=%u over_gate=%u/%u net=%s", a, b, 600u,
                g_net_up ? "up" : "down");
         break;
      default:
         break;
      }
   }
   if (g_rfu_tr_lost != reported_lost)
   {
      reported_lost = g_rfu_tr_lost;
      fe_evt("rfu_trace_lost n=%u", reported_lost);
   }
}

enum { SIL_OFF = 0, SIL_AUTO, SIL_HOST, SIL_JOIN };
enum { SILST_IDLE = 0, SILST_JOINING, SILST_UP, SILST_FAILED };

static int g_silent_mode;   /* SIL_* */
static int g_silent_state;  /* SILST_* */
static unsigned g_silent_ticks, g_silent_deadline;
static char g_silent_group[16], g_silent_nick[24];

static void silent_frame(void)
{
   if (!g_silent_mode || g_silent_state == SILST_FAILED ||
       g_silent_state == SILST_UP)
      return;
   if (!g_rfu_activated)
      return;

   if (g_silent_state == SILST_IDLE)
   {
      int as_host = (g_silent_mode == SIL_HOST);
      int rc;
      if (g_net_up)                     /* user already connected via UI */
      {
         g_silent_state = SILST_UP;
         return;
      }
      fe_evt("silent_activate role=%s group=%s",
             as_host ? "host" : "join", g_silent_group);
      rc = net_bringup(as_host, g_silent_group, g_silent_nick, 0);
      if (rc == ADHOC_ERR_WLAN_OFF)
      {
         osd_chip_session("WLAN switch OFF");
         osd_toast("Turn the WLAN switch ON for wireless");
         fe_evt("silent_fail reason=wlan_off");
         g_silent_state = SILST_FAILED;
         return;
      }
      if (rc != 0)
      {
         osd_toast("Wireless start failed");
         fe_evt("silent_fail reason=bringup stage=%s",
                adhoc_transport_stage());
         g_silent_state = SILST_FAILED;
         return;
      }
      if (as_host)
      {
         fe_evt("silent_host promoted=0");
         osd_toast("Wireless ready (hosting %s)", g_silent_group);
         g_silent_state = SILST_UP;
         return;
      }
      /* join-first: wait a jittered window for a WELCOME */
      g_silent_ticks = 0;
      g_silent_deadline = 240 +
         (unsigned)((sceKernelGetSystemTimeWide() >> 5) % 120);
      g_silent_state = SILST_JOINING;
      return;
   }

   /* SILST_JOINING */
   g_silent_ticks++;
   if (fe_np_session_active())
   {
      fe_evt("silent_joined group=%s", g_silent_group);
      osd_toast("Wireless linked (%s)", g_silent_group);
      g_silent_state = SILST_UP;
      return;
   }
   if (g_silent_mode == SIL_AUTO && g_silent_ticks > g_silent_deadline)
   {
      /* Nobody hosting: promote.  Transport stays up; only the netdrv
       * role restarts (core stop()/start() is part of the netpacket
       * contract). */
      fe_np_stop();
      if (net_start_np(1, g_silent_group, g_silent_nick, 0) == 0)
      {
         fe_evt("silent_host promoted=1 waited=%u", g_silent_ticks);
         osd_toast("Wireless ready (hosting %s)", g_silent_group);
         g_silent_state = SILST_UP;
      }
      else
      {
         net_teardown();
         fe_evt("silent_fail reason=promote");
         g_silent_state = SILST_FAILED;
      }
   }
}

/* UI-driven wireless action (host=1 / host=0 join). */
static void ui_net_action(int is_host)
{
   int rc;
   if (g_net_up)
   {
      osd_toast("Already in a session");
      return;
   }
   /* One "connecting" frame so the user sees feedback during the blocking
    * adhocctl bring-up (<= 30 s). */
   vid_overlay_begin(1);
   vid_text_center(120, is_host ? "Starting host..." : "Joining room...",
                   0xFFFF);
   vid_text_center(150, ui_group(), 0x65BF);
   vid_overlay_end();
   vid_swap();

   me_rend_suspend();                  /* clean heap for np_start (run 407) */
   rc = net_bringup(is_host, ui_group(), g_pcfg.nick, 0);
   me_rend_resume();                   /* re-alloc from the post-session heap */
   if (rc == 0)
      osd_toast("%s %s", is_host ? "Hosting room" : "Joined room",
                ui_group());
   else if (rc == ADHOC_ERR_WLAN_OFF)
      osd_toast("WLAN switch is OFF");
   else
      osd_toast("Wireless start failed (%s)", adhoc_transport_stage());
}

/* ---- GU color-order test pattern (testpat = 1; no core, no ROM) --------
 * Renders 8 vertical 30px bars of known RGB565 values through the exact
 * production blit path, then GE-dumps the drawbuffer.  The harness asserts
 * the decoded bar colors — a channel-order regression fails loudly. */

/* The bars are the blit path's INPUT, so they must be written in whatever
 * layout the core emits (ADR-0039) — the harness then asserts the GE's
 * output is really red where we asked for red, which is what pins the
 * frontend's half of the format contract absolutely, with no core in the
 * loop.  Spelled as channel triples rather than hex so the two lists cannot
 * drift apart. */
#ifdef USE_PSP_RGB565_FORMAT
#define TESTPAT_PIX(r, g, b) \
   (uint16_t)(((b) << 11) | ((g) << 5) | (r))
#else
#define TESTPAT_PIX(r, g, b) \
   (uint16_t)(((r) << 11) | ((g) << 5) | (b))
#endif

static int run_testpat(void)
{
   static const uint16_t bar[8] = {
      TESTPAT_PIX(31, 63, 31),   /* white   */
      TESTPAT_PIX(31,  0,  0),   /* red     */
      TESTPAT_PIX( 0, 63,  0),   /* green   */
      TESTPAT_PIX( 0,  0, 31),   /* blue    */
      TESTPAT_PIX(31, 63,  0),   /* yellow  */
      TESTPAT_PIX( 0, 63, 31),   /* cyan    */
      TESTPAT_PIX(31,  0, 31),   /* magenta */
      TESTPAT_PIX(16, 32, 16)    /* gray    */
   };
   static uint16_t pat[FE_GBA_WIDTH * FE_GBA_HEIGHT];
   int f, x, y;

   for (y = 0; y < FE_GBA_HEIGHT; y++)
      for (x = 0; x < FE_GBA_WIDTH; x++)
         pat[y * FE_GBA_WIDTH + x] = bar[x / 30];

   fe_evt("testpat_start bars=8");
   for (f = 0; f < 120 && g_running; f++)
   {
      plat_video_frame(pat, FE_GBA_WIDTH, FE_GBA_HEIGHT, FE_GBA_WIDTH * 2);
      if (f == 60)
         dump_ge_bmp();
      sceDisplayWaitVblankStart();
      vid_swap();
      g_drew = 0;
   }
   return 0;
}

/* ---- Gate-4E transport echo (nettest = 1; no core, no ROM) -------------
 * Wire format: "GPNT" magic + type u8 (0 ping / 1 pong / 2 end) + seq u32
 * LE + 15 pad bytes = 24 B.  The join side broadcasts pings at 10 Hz and
 * asserts >= NETTEST_TARGET pongs; the host unicasts every ping back to
 * its source MAC and passes on the same threshold.  Exercises exactly the
 * surface netdrv needs: broadcast, unicast-to-src, RX thread + ring. */

#define NETTEST_TARGET   30
#define NETTEST_PKT_LEN  24

static int run_nettest(int is_host, const char *group, long secs)
{
   nd_transport tp;
   uint64_t start, last_ping = 0, last_tick = 0;
   uint32_t seq = 0, pings = 0, pongs = 0, ends_sent = 0;
   int done = 0, rc;

   fe_evt("nettest_start role=%s group=%s", is_host ? "host" : "join",
          group && group[0] ? group : ADHOC_GROUP_DEFAULT);
   rc = adhoc_transport_init(group, 0);
   if (rc != ADHOC_OK)
   {
      net_error_evt("adhoc_init", rc);
      return -1;
   }
   log_adhoc_up(group);
   adhoc_transport_iface(&tp);

   start = net_now_us();
   while (!done && g_running && net_now_us() - start < (uint64_t)secs * 1000000ull)
   {
      uint8_t src[6], buf[ND_MAX_FRAME];
      int n;
      uint64_t now = net_now_us();

      while ((n = tp.recv(tp.ctx, src, buf, sizeof(buf))) > 0)
      {
         uint32_t rseq;
         if (n < NETTEST_PKT_LEN || memcmp(buf, "GPNT", 4) != 0)
            continue;
         rseq = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);
         if (is_host && buf[4] == 0)      /* ping -> pong to source MAC */
         {
            buf[4] = 1;
            tp.send_to(tp.ctx, src, buf, NETTEST_PKT_LEN);
            pings++;
            if (pings == 1 || pings == NETTEST_TARGET)
               fe_evt("nettest_echo n=%u seq=%u", pings, rseq);
         }
         else if (is_host && buf[4] == 2) /* joiner done */
            done = 1;
         else if (!is_host && buf[4] == 1)
         {
            pongs++;
            if (pongs == 1 || pongs == NETTEST_TARGET)
               fe_evt("nettest_pong n=%u seq=%u", pongs, rseq);
         }
      }

      if (!is_host && now - last_ping >= 100000)
      {
         uint8_t pkt[NETTEST_PKT_LEN];
         memset(pkt, 0, sizeof(pkt));
         memcpy(pkt, "GPNT", 4);
         if (pongs < NETTEST_TARGET)
         {
            pkt[4] = 0;
            pkt[5] = (uint8_t)seq;        pkt[6] = (uint8_t)(seq >> 8);
            pkt[7] = (uint8_t)(seq >> 16); pkt[8] = (uint8_t)(seq >> 24);
            tp.broadcast(tp.ctx, pkt, sizeof(pkt));
            seq++;
         }
         else                             /* enough echoes: tell the host */
         {
            pkt[4] = 2;
            tp.broadcast(tp.ctx, pkt, sizeof(pkt));
            if (++ends_sent >= 5)
               done = 1;
         }
         last_ping = now;
      }

      if (now - last_tick >= 1000000)
      {
         scePowerTick(PSP_POWER_TICK_ALL);
         last_tick = now;
      }
      sceKernelDelayThread(10000);
   }

   fe_evt("nettest_done role=%s sent=%u echoed=%u pongs=%u",
          is_host ? "host" : "join", seq, pings, pongs);
   adhoc_stats_evt();
   adhoc_transport_term();
   return (is_host ? pings : pongs) >= NETTEST_TARGET ? 0 : -1;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char *argv[])
{
   char rom_path[256], save_path[256], state_path[256];
   char script_name[64], script_path[192];
   char nick[24], group[16] = "";
   long autoexit, dump_at, dump_every, gedump_at, ff_ini, ui_demo, simff;
   long vhash_from, vhash_to;   /* ADR-0033(coreopt) video regression oracle */
   long vid_prof_win;           /* ADR-0034(coreopt) renderer path profile */
   int have_script = 0, have_harness, have_variant;
   int net_host = 0, net_join = 0, net_probe = 0;
   int exit_code = 0;
   const char *exit_reason = NULL;
   int ff_engaged = 0, ff_toggled = 0, ff_acc = 0;
   unsigned prev_pad = 0;
   int chord_frames = 0;
   fe_host_config cfg;

   setup_exit_callback();

   scePowerSetClockFrequency(333, 333, 166);

   init_paths(argc, argv);
#ifndef GPSP_PLAYABLE
   sceIoMkdir(LOG_DIR, 0777);
   fe_evt_init(LOG_PATH, 0);
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
   fe_evt("clock=%d", scePowerGetCpuClockFrequency());
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

   /* Frontend resources are allocated BEFORE retro_init eats the heap for
    * ROM blocks (FRONTEND-AUDIT §8): GU uses VRAM + static list, audio ring
    * is static, audio thread stack is created here. */
   pcfg_load(CONFIG_INI);
   vid_init();
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   audio_start();

   have_harness = file_exists(HARNESS_INI);
   have_variant = file_exists(VARIANT_INI);

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
   autoexit   = fe_ini_get_int(HARNESS_INI, "autoexit_frames", 0);
   /* ADR-0053: harness-ini only and default OFF.  Putting this in CONFIG.INI
    * would let a stale file toggle USB on a console someone is playing --
    * the same failure ADR-0036 renamed autopilot.ini to prevent. */
   handoff_config(fe_ini_get_int(HARNESS_INI, "handoff", 0),
                  fe_ini_get_int(HARNESS_INI, "handoff_window_s", 90),
                  fe_ini_get_int(HARNESS_INI, "handoff_max_runs", 20),
                  fe_ini_get_int(HARNESS_INI, "handoff_total_s", 900),
                  fe_ini_get_int(HARNESS_INI, "handoff_park_s", 0));
   dump_at    = fe_ini_get_int(HARNESS_INI, "dump_at", 0);
   dump_every = fe_ini_get_int(HARNESS_INI, "dump_every", 0);
   gedump_at  = fe_ini_get_int(HARNESS_INI, "gedump_at", 0);
   ff_ini     = fe_ini_get_int(HARNESS_INI, "ff", 0);
   /* ADR-0033/0034 (coreopt): the video oracle and renderer path profile,
    * grafted onto the harness channel — the branch predates ADR-0036's
    * autopilot.ini -> .gpsp-harness.ini rename.  vhash=1 is shorthand for
    * "hash every frame"; vid_prof only produces a line if the core was
    * built with -DVIDEO_PROF=1. */
   vid_prof_win = fe_ini_get_int(HARNESS_INI, "vid_prof", 0);
   if (vid_prof_win > 0)
   {
      video_prof_set_clock(vprof_clock_us);
      vp_build_crc = eboot_crc();
   }
   vhash_from = fe_ini_get_int(HARNESS_INI, "vhash_from", 0);
   vhash_to   = fe_ini_get_int(HARNESS_INI, "vhash_to", 0);
   if (fe_ini_get_int(HARNESS_INI, "vhash", 0) && !vhash_to)
   {
      vhash_from = 1;
      vhash_to   = 0x7FFFFFFF;
   }
   ui_demo    = fe_ini_get_int(HARNESS_INI, "ui_demo", 0);
   if (ui_demo)
      ui_demo_shots();   /* browser dumps the gallery + auto-picks (README) */
   /* Asset shoot / theme override: -1 (absent) leaves the build default. */
   ui_set_theme_black((int)fe_ini_get_int(HARNESS_INI, "ui_theme_black", -1));
   simff      = fe_ini_get_int(HARNESS_INI, "simff", 0);   /* harness: hold a
                              virtual FF button for N frames from frame 300
                              (PPSSPP can't press Square; run_ui_smoke.sh) */
   fe_evt("ff mode=%s", ff_ini ? "uncapped" : "normal");
   if (fe_ini_get(HARNESS_INI, "script", script_name, sizeof(script_name)))
      have_script = 1;
   /* Perf-rig key: load the slot-0 savestate shortly after boot, so a run
    * can start INSIDE a scene (mid-battle benchmarks) instead of scripting
    * its way there through a title screen. */
   g_autoload_state = (int)fe_ini_get_int(HARNESS_INI, "load_state", 0);
   /* Point the SMC block/writer probe at a 256-byte page of interest.  The
    * default (0x03007D00) is nowhere near Unbound's storm, which `EVT
    * smc_addr` localised to 0x03006200/0x03006300 in IWRAM — four addresses
    * taking ~1450 writes/s between them.  Aiming the watch here fills in the
    * `blk=` and `wr=` fields: which translated blocks cover the page, and
    * which PCs are doing the writing. */
   {
      unsigned w = (unsigned)fe_ini_get_int(HARNESS_INI, "smc_watch", 0);
      if (w)
      {
         smc_blk_watch = w;
         fe_evt("smc_watch set=%08x", w);
      }
   }

   /* Wireless keys (harness channel; the UI panel drives the same calls). */
   net_host  = fe_ini_get_int(HARNESS_INI, "host", 0) != 0;
   net_join  = fe_ini_get_int(HARNESS_INI, "join", 0) != 0;
   net_probe = (int)fe_ini_get_int(HARNESS_INI, "net_probe", 0);
   /* ADR-0062e: `core_phase` HAS BEEN IN EVERY HARNESS INI IN THIS PROJECT
    * FOR THREE SESSIONS AND HAS NEVER BEEN READ.  It was only ever a
    * config.ini key (config_psp.c), so `core_phase = 2` in a .gpsp-harness.ini
    * was decorative and `core_phase = 0` did not turn the profiler off --
    * the console kept logging `EVT core_phase lvl=2` either way, which is how
    * this was caught (FULLSPEED-FINDINGS §18).  It is not free: the profiler
    * takes ~344 timer reads per frame ON THE EMULATION THREAD, and at full
    * speed the join's whole margin is about a millisecond. */
   g_pcfg.core_phase = (int)fe_ini_get_int(HARNESS_INI, "core_phase",
                                           g_pcfg.core_phase);
   /* ADR-0019: let the harness pin the session frameskip policy without
    * touching the user's config.ini (0 off / 1 adaptive / 2 auto). */
   g_pcfg.net_frameskip = (int)fe_ini_get_int(HARNESS_INI, "net_frameskip",
                                              g_pcfg.net_frameskip);
   /* ADR-0024: same, for the EVT writer thread (0 inline / 1 threaded). */
   g_pcfg.log_thread = (int)fe_ini_get_int(HARNESS_INI, "log_thread",
                                           g_pcfg.log_thread);
   /* ADR-0025: same, for the .sav block writer. */
   g_pcfg.sram_thread = (int)fe_ini_get_int(HARNESS_INI, "sram_thread",
                                            g_pcfg.sram_thread);
   /* Same, for session pacing.  ADR-0033 REMAPPED THE VALUES: 0 off,
    * 1 fixed-rate (default), 2 the ADR-0027/0028 adaptive matcher. */
   g_pcfg.net_pace_match = (int)fe_ini_get_int(HARNESS_INI, "net_pace_match",
                                               g_pcfg.net_pace_match);
   if (g_pcfg.net_pace_match < 0 || g_pcfg.net_pace_match > 2)
      g_pcfg.net_pace_match = PACE_MODE_FIXED;
   g_pcfg.net_session_fps_x100 =
      pcfg_fps_x100(HARNESS_INI, "net_session_fps",
                    g_pcfg.net_session_fps_x100);
   g_pcfg.net_session_fps_snap =
      (int)fe_ini_get_int(HARNESS_INI, "net_session_fps_snap",
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
    * and which persists in config.ini across relaunches.  PLAY_FPS_X100 is the
    * speed profile's value and stays the default; `profile = 1` selects 29.97.
    *
    * Latched HERE, once, before the pacer and the netdrv timeout scaling read
    * it — which is why switching profiles relaunches instead of taking effect
    * live.  Two consoles that disagree about how long a frame is are exactly
    * what gen-3's RFU cannot survive: it counts link timeouts in FRAMES. */
   g_pcfg.net_session_fps_x100 = PCFG_PROFILE_FPS_X100(g_pcfg.profile);
   g_pcfg.net_session_fps_snap = 0;
   g_pcfg.core_phase           = 0;
   /* `gu_defer = 1` was in the harness ini of ALL 17 full-speed runs behind
    * the 16/34 number, but it is 0 in a stock config.ini.  Forced here so the
    * playable build is the configuration that was actually measured, rather
    * than one knob off it. */
   g_pcfg.gu_defer             = 1;
#endif
   /* ADR-0034: same A/B channel for the blit staging placement. */
   g_pcfg.blit_mode = (int)fe_ini_get_int(HARNESS_INI, "blit_mode",
                                          g_pcfg.blit_mode);
   if (g_pcfg.blit_mode < 0 || g_pcfg.blit_mode >= VID_BLIT_MODES)
      g_pcfg.blit_mode = VID_BLIT_CACHED;
   /* ADR-0040: and for the deferred GE sync, so the colour test can run the
    * deferred path.  The rig cannot price it but it CAN prove that a list
    * still in flight never reaches the swap or the readback. */
   g_pcfg.gu_defer = (int)fe_ini_get_int(HARNESS_INI, "gu_defer",
                                         g_pcfg.gu_defer);
   g_pcfg.gu_defer = g_pcfg.gu_defer ? 1 : 0;
   /* ADR-0042: per-frame RFU delivery cap, harness-overridable so the trade
    * test can run both the capped and uncapped path. */
   g_pcfg.rfu_rx_cap = (int)fe_ini_get_int(HARNESS_INI, "rfu_rx_cap",
                                           g_pcfg.rfu_rx_cap);
   if (g_pcfg.rfu_rx_cap < 0)  g_pcfg.rfu_rx_cap = 0;
   if (g_pcfg.rfu_rx_cap > 16) g_pcfg.rfu_rx_cap = 16;
   rfu_set_rx_cap((unsigned)g_pcfg.rfu_rx_cap);
   fe_evt("rfu_rx_cap n=%d", g_pcfg.rfu_rx_cap);
   /* ADR-0075: frame-boundary delivery admission.  The successor to the
    * falsified rfu_rx_cap: same "at most N real deliveries per frame" idea,
    * but the eligibility gate is applied identically in rfu_data_avail(), so
    * the WAITEVENT spin that made every capped arm self-blinding cannot
    * occur.  1 = match the game's drain rate (the arm to run first). */
   {
      int fp = (int)fe_ini_get_int(HARNESS_INI, "rfu_frame_pace",
                                   PLAY_RFU_FRAME_PACE);
      if (fp < 0)  fp = 0;
      if (fp > 16) fp = 16;
      rfu_set_frame_pace((unsigned)fp);
      fe_evt("rfu_frame_pace n=%d", fp);
   }
   /* Fixed-depth cushion (jitter buffer) PoC — see PLAY_RFU_CUSHION. */
   {
      int cu = (int)fe_ini_get_int(HARNESS_INI, "rfu_cushion",
                                   PLAY_RFU_CUSHION);
      if (cu < 0)  cu = 0;
      if (cu > 24) cu = 24;
      rfu_set_cushion((unsigned)cu);
      fe_evt("rfu_cushion n=%d", cu);
   }
   /* Candidate mid-run-Disconnect fix (default OFF): await the DISCONNECTED
    * state before term/unload.  Run the breadcrumb-only build FIRST to confirm
    * the wedge step, then set disc_await=1 to A/B the fix. */
   {
      int da = (int)fe_ini_get_int(HARNESS_INI, "disc_await", 0);
      adhoc_set_disc_await(da);
      fe_evt("disc_await on=%d", da);
   }
   /* Media Engine mode: pcfg (the settings toggle, applied via relaunch) with
    * a harness override so the rig can A/B it per-arm. */
   {
      int mm = (int)fe_ini_get_int(HARNESS_INI, "me_mode", g_pcfg.me_mode);
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
      int ts  = (int)fe_ini_get_int(HARNESS_INI, "rfu_timeout_scale", 100);
      int rs  = (int)fe_ini_get_int(HARNESS_INI, "rfu_rtx_scale",     100);
      int aut = (g_pcfg.net_session_fps_x100 > 0)
                   ? (g_pcfg.net_session_fps_x100 * 100) / 2997 : 100;
      if (aut < 100) aut = 100;
      if (ts < 0) ts = aut;
      if (rs < 0) rs = aut;
      if (ts < 25) ts = 25;   if (ts > 1600) ts = 1600;
      if (rs < 25) rs = 25;   if (rs > 1600) rs = 1600;
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
      int np_ = (int)fe_ini_get_int(HARNESS_INI, "adhoc_net_prio", 0);
      int rf  = (int)fe_ini_get_int(HARNESS_INI, "nd_rto_first_max_us", 0);
      int rm  = (int)fe_ini_get_int(HARNESS_INI, "nd_rto_min_us", 0);
      if (rf < 0) rf = 0;
      if (rm < 0) rm = 0;
      adhoc_transport_set_net_prio(np_);
      fe_np_set_arq_timers((uint32_t)rf, (uint32_t)rm);
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
    * Note this also drops main below the IO thread (0x22): a log or SRAM write
    * will now preempt emulation.  That is visible in `EVT fps emu=` and is
    * part of what the arm is measuring, not a hidden cost.
    *
    * 0 = leave main at its default 0x20. */
   {
      int ep = (int)fe_ini_get_int(HARNESS_INI, "emu_prio", PLAY_EMU_PRIO);
      int ip = (int)fe_ini_get_int(HARNESS_INI, "io_prio",  PLAY_IO_PRIO);
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
      /* ADR-0066: frames between dump-marker polls.  ADR-0069 default = 0. */
      {
         int dm = (int)fe_ini_get_int(HARNESS_INI, "dump_marker_poll",
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
         int yu = (int)fe_ini_get_int(HARNESS_INI, "sched_yield_us", 0);
         int yf = (int)fe_ini_get_int(HARNESS_INI, "sched_yield_frames", 60);
         if (yu < 0) yu = 0;
         if (yf < 1) yf = 1;
         g_yield_us    = yu;
         g_yield_every = yf;
         if (yu)
            fe_evt("sched_yield us=%d every=%d", g_yield_us, g_yield_every);
      }
      /* ADR-0064: the preemption census.  Off unless asked for. */
      g_pe_on = (int)fe_ini_get_int(HARNESS_INI, "preempt_prof", 0) ? 1 : 0;
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
            fe_evt("msstat n=%d us=%u hit=%d",
                   k, (unsigned)(net_now_us() - t0), hit);
         }
      }
      /* ADR-0065: the protected window.  `base` is whatever the emulation
       * thread's STANDING priority ends up being, so the window degrades to
       * exactly today's behaviour when it is closed. */
      {
         int bu = (int)fe_ini_get_int(HARNESS_INI, "emu_prio_boost_us",
                                      PLAY_BOOST_US);
         int bp = (int)fe_ini_get_int(HARNESS_INI, "emu_prio_boost", 0x29);
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
      int pm = (int)fe_ini_get_int(HARNESS_INI, "rfu_poll_min_cycles",
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
      int dd = (int)fe_ini_get_int(HARNESS_INI, "rfu_disc_defer", 0);
      rfu_set_disc_defer(dd ? 1u : 0u);
      if (dd)
         fe_evt("rfu_disc_defer on");
      /* ADR-0079: the exit-echo state assist.  Default PLAY_EXIT_ASSIST — 1 in
       * the playable build (nonexistent HARNESS_INI -> default), 0 for harness.
       * Was hardcoded 0, which silently left the PLAYABLE build without the
       * exit fix despite PLAY_EXIT_ASSIST=1 (the message-17 exit crash). */
      g_exit_assist =
         (int)fe_ini_get_int(HARNESS_INI, "rfu_exit_assist", PLAY_EXIT_ASSIST) ? 1 : 0;
      fe_evt("rfu_exit_assist on=%d", g_exit_assist);
      /* ADR-0078: net_pace_match was a config.ini-only key, so ARM B's
       * harness ini silently ran mode 1 (run 315's lesson — the arm's own
       * log said `mode=1 policy=fixed` while the ini said 3).  Same
       * harness-overridable pattern as net_session_fps. */
      int pmode = (int)fe_ini_get_int(HARNESS_INI, "net_pace_match",
                                      g_pcfg.net_pace_match);
      if (pmode >= 0 && pmode <= 3 && pmode != g_pcfg.net_pace_match)
      {
         fe_evt("net_pace_match harness override %d -> %d",
                g_pcfg.net_pace_match, pmode);
         g_pcfg.net_pace_match = pmode;
      }
      /* ADR-0076: the exit-race grace window, in emulated frames. */
      int dg = (int)fe_ini_get_int(HARNESS_INI, "rfu_disc_grace",
                                   PLAY_RFU_DISC_GRACE);
      if (dg < 0)   dg = 0;
      if (dg > 600) dg = 600;
      rfu_set_disc_grace((unsigned)dg);
      fe_evt("rfu_disc_grace frames=%d", dg);
      int ip = (int)fe_ini_get_int(HARNESS_INI, "rfu_idle_poll_cycles",
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
   g_pace_slow_us = (int)fe_ini_get_int(HARNESS_INI, "pace_slow_us", 0);
   if (g_pace_slow_us < 0)      g_pace_slow_us = 0;
   if (g_pace_slow_us > 30000)  g_pace_slow_us = 30000;
   if (g_pace_slow_us)
      fe_evt("pace_slow_us=%d", g_pace_slow_us);
   if (!fe_ini_get(HARNESS_INI, "nick", nick, sizeof(nick)))
      snprintf(nick, sizeof(nick), "%s", g_pcfg.nick);
   if (!fe_ini_get(HARNESS_INI, "group", group, sizeof(group)))
      snprintf(group, sizeof(group), "%s", g_pcfg.group);

   /* Radio emulation for the PPSSPP rig (harness only — the desktop UDP
    * backend has the same knobs).  PPSSPP's AdhocServer loopback RTT is
    * microseconds; real PSP ad-hoc is tens of ms, which is the whole
    * reason the ARQ storm of docs/HANDOFF.md issue #2 escaped the harness.
    * Each side adds net_latency_ms to the one-way path -> RTT = 2x. */
   {
      int nl = (int)fe_ini_get_int(HARNESS_INI, "net_latency_ms", 0);
      int nj = (int)fe_ini_get_int(HARNESS_INI, "net_jitter_ms", 0);
      int np_ = (int)fe_ini_get_int(HARNESS_INI, "net_loss_pct", 0);
      if (nl || nj || np_)
      {
         adhoc_transport_set_fault(nl, nj, np_,
                                   0x243F6A88u ^ (uint32_t)(net_host ? 1 : 2));
         fe_evt("net_fault latency_ms=%d jitter_ms=%d loss_pct=%d", nl, nj, np_);
      }
   }

   /* Causation test for the exit-Union-Room fatal screen (phase5j).  See
    * netpacket_host.h: swallowing the parent's RFU DISCONNECT notice pins
    * the child's emulated adapter in RFU_STATE_CLIENT, which is the state
    * the field logs imply and which rfu.c has no other way out of. */
   {
      int dd = (int)fe_ini_get_int(HARNESS_INI, "rfu_drop_disconnect", 0);
      if (dd)
         fe_np_debug_drop_rfu_disconnect(dd);
   }

   /* testpat = 1: GU color-order test pattern — no core, no ROM. */
   if (fe_ini_get_int(HARNESS_INI, "testpat", 0))
   {
      run_testpat();
      audio_stop();
      vid_term();
      fe_evt("exit code=0");
      evt_shutdown();
      handoff_run(0, NULL);   /* ADR-0053 */
      sceKernelExitGame();
      return 0;
   }

   /* nettest = 1: Gate-4E transport echo — no core, no ROM. */
   if (fe_ini_get_int(HARNESS_INI, "nettest", 0))
   {
      long secs = fe_ini_get_int(HARNESS_INI, "nettest_secs", 90);
      int rc = run_nettest(net_host, group, secs);
      audio_stop();
      vid_term();
      if (rc == 0)
         fe_evt("exit code=0");
      else
         fe_evt("exit code=5 reason=nettest_fail");
      evt_shutdown();
      handoff_run(5, "nettest_fail");   /* ADR-0053 */
      sceKernelExitGame();
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

   /* ROM selection: variant builds auto-boot their baked ROM with a
    * separate save namespace (<base>/saves/, docs/VARIANTS.md); the
    * harness ini boots the first ROM in roms/ directly (the
    * historical Phase-1 behavior every e2e driver relies on); a human
    * without either gets the browser (plan §8, Gate 2). */
   if (have_variant)
   {
      char vrom[96] = "", savedir[160], stem[96];
      const char *slash;
      fe_ini_get(VARIANT_INI, "rom", vrom, sizeof(vrom));
      if (!vrom[0])
      {
         fe_evt("exit code=2 reason=bad_variant");
         evt_shutdown();
         audio_stop();
         vid_term();
         handoff_run(2, "bad_variant");   /* ADR-0053 */
         sceKernelExitGame();
         return 0;
      }
      snprintf(rom_path, sizeof(rom_path), "%s/%s", g_dir_base, vrom);
      snprintf(savedir, sizeof(savedir), "%s/saves", g_dir_base);
      sceIoMkdir(savedir, 0777);
      slash = strrchr(vrom, '/');
      snprintf(stem, sizeof(stem), "%s", slash ? slash + 1 : vrom);
      snprintf(save_path, sizeof(save_path), "%s/saves/%s", g_dir_base, stem);
      make_suffixed_path(save_path, ".sav", save_path, sizeof(save_path));
      snprintf(state_path, sizeof(state_path), "%s/saves/%s", g_dir_base,
               stem);
      make_suffixed_path(state_path, ".st0", state_path, sizeof(state_path));
      fe_evt("variant rom=%s silent=%d group=%s", vrom, g_silent_mode,
             g_silent_group);
   }
   else if (have_harness && !fe_ini_get_int(HARNESS_INI, "browser", 0))
   {
      /* `browser = 1` overrides the ADR-0067 suppression: the gallery runs
       * even with the harness channel present (asset capture / emulator
       * shoots — combine with ui_demo=1 for the unattended gallery dump). */
      if (find_first_rom(rom_path, sizeof(rom_path)) != 0)
      {
         fe_evt("exit code=2 reason=no_rom");
         evt_shutdown();
         audio_stop();
         handoff_run(2, "no_rom");   /* ADR-0053 */
         sceKernelExitGame();
         return 0;
      }
   }
   else if (ui_browser(ROM_DIR, rom_path, sizeof(rom_path)) != 0)
   {
      fe_evt("exit code=2 reason=no_rom");
      evt_shutdown();
      audio_stop();
      vid_term();
      handoff_run(2, "no_rom");   /* ADR-0053 */
      sceKernelExitGame();
      return 0;
   }
   if (!have_variant)
   {
      make_suffixed_path(rom_path, ".sav", save_path, sizeof(save_path));
      make_suffixed_path(rom_path, ".st0", state_path, sizeof(state_path));
   }
   g_save_path_for_backup = save_path;
   fe_log("rom_path=%s save_path=%s", rom_path, save_path);

   memset(&cfg, 0, sizeof(cfg));
   cfg.rom_path      = rom_path;
   cfg.system_dir    = g_dir_base;
   cfg.save_path     = save_path;
   cfg.video_frame   = plat_video_frame;
   cfg.audio_frames  = plat_audio_frames;
   cfg.input_bitmask = plat_input_bitmask;
   cfg.time_us       = net_now_us;   /* heartbeat t_us (hw perf baseline) */
   cfg.core_counters = plat_core_counters;                   /* ADR-0028 */
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

   if (fe_host_boot(&cfg) == 0)
   {
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
         int meb = (int)fe_ini_get_int(HARNESS_INI, "me_boot", PLAY_ME_BOOT);
         int mbn = (int)fe_ini_get_int(HARNESS_INI, "me_bench", 0);
         int mev = (int)fe_ini_get_int(HARNESS_INI, "me_video", PLAY_ME_VIDEO);
         if (g_me_rend_cfg)
         {
            meb = 1;         /* the renderer needs the ME booted */
            mev = 0;         /* and owns the ME: staging offload excluded */
         }
         if (meb || mbn || mev)
         {
            if (me_host_init(g_dir_base) == 0)
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
      fe_evt("exit code=1 reason=load_failed");
      evt_shutdown();
      audio_stop();
      vid_term();
      handoff_run(1, "load_failed");   /* ADR-0053 */
      sceKernelExitGame();
      return 0;
   }

   /* Heap headroom after the core's greedy ROM-buffer allocation (Gate-1
    * memory measurement; PPSSPP models more RAM than a PSP-1000 — the real
    * verdict lands at Gate 4-H). */
   fe_evt("mem_free=%d max_block=%d",
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());

   /* From here on the memory stick is somebody else's thread (ADR-0024).
    * Deliberately after boot: a BIOS/ROM failure must reach the stick even
    * if we never get to the main loop. */
   io_thread_start();

   if (have_script)
   {
      snprintf(script_path, sizeof(script_path), "%s/%s", g_dir_base, script_name);
      if (fe_autopilot_load(script_path) != 0)
      {
         io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
         fe_host_shutdown();
         audio_stop();
         vid_term();
         fe_evt("exit code=2 reason=bad_script");
         evt_shutdown();
         handoff_run(2, "bad_script");   /* ADR-0053 */
         sceKernelExitGame();
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
         io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
         fe_host_shutdown();
         audio_stop();
         vid_term();
         fe_evt("exit code=4 reason=net_failed");
         evt_shutdown();
         handoff_run(4, "net_failed");   /* ADR-0053 */
         sceKernelExitGame();
         return 0;
      }
      me_rend_resume();                /* re-alloc from the post-np_start heap */
   }

   while (g_running)
   {
      unsigned frames;
      SceCtrlData pd;
      unsigned pad_new;
      int session = fe_np_session_active();
      int harness_ff = ff_ini || fe_autopilot_ff();
      /* ADR-0021: the loop body without the vblank wait is the number that
       * has to fit 16.7 ms.  Two clock reads per frame, and only while a
       * session is up. */
      uint64_t frame_t0 = g_net_up ? net_now_us() : 0;

      /* ADR-0064: closes the PREVIOUS frame's `wait` region and opens this
       * frame's `pre`.  ADR-0065: open the protected window here, before any
       * of the frame's work — see emu_boost_open(). */
      preempt_mark(3);
      emu_boost_open();

      sceCtrlPeekBufferPositive(&pd, 1);
      g_pad = pd.Buttons;
      pad_new = g_pad & ~prev_pad;
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
      if ((pad_new & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
          && (g_pad & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
             == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)
          && !(g_pad & PSP_CTRL_START))
         g_dump_chord_pending = 1;

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
         if (!gba_mask_primed || gm != prev_gba_mask)
         {
            gba_mask_primed = 1;
            fe_evt("input mask=0x%03x held=%u f=%u net=%s",
                   (unsigned)gm, (unsigned)(gm & prev_gba_mask),
                   fe_host_frame_count(), g_net_up ? "up" : "down");
            prev_gba_mask = gm;
         }
      }

      net_frame();
      silent_frame();   /* variant silent-wireless policy (ADR-0013) */
      rfu_link_down_drain();  /* "the game ended it" breadcrumb (ADR-0019) */
      rfu_trace_drain();      /* adapter cmd/state trace (phase5j)         */
      exit_assist_frame();    /* ADR-0079: repair the lost exit-key echo   */
      audio_status_frame();   /* feeds core frameskip when engaged (ADR-0019) */

      /* ---- in-game menu (Select+Start held ~1/4 s, plan §8) ---------- */
      if (!ui_active())
      {
         if ((g_pad & (PSP_CTRL_SELECT | PSP_CTRL_START)) ==
             (PSP_CTRL_SELECT | PSP_CTRL_START) && !have_script)
         {
            if (++chord_frames >= 15)
            {
               chord_frames = 0;
               ui_open();
            }
         }
         else
            chord_frames = 0;

         if (ui_demo && fe_host_frame_count() >= 300)
         {
            ui_demo = 0;
            ui_open();
            ui_demo_start();
         }
      }

      /* Perf-rig autoload: one shot, ~half a second in, once video/audio/ME
       * are all up.  The scripted run then starts inside the saved scene. */
      if (g_autoload_state && fe_host_frame_count() >= 30)
      {
         g_autoload_state = 0;
         fe_evt("autoload_state rc=%d", fe_host_state_load(state_path));
      }

      if (ui_active())
      {
         /* Core paused; wireless keeps pumping; UI draws + acts. */
         ui_action act = ui_frame(g_pad, g_net_up, g_session_info);
         switch (act)
         {
         case UI_ACT_SAVESTATE:
            if (fe_host_state_save(state_path) == 0)
               osd_toast("State saved");
            else
               osd_toast("State save FAILED");
            break;
         case UI_ACT_LOADSTATE:
            if (fe_host_state_load(state_path) == 0)
               osd_toast("State loaded");
            else
               osd_toast("No state to load");
            break;
         case UI_ACT_NET_HOST:
            ui_net_action(1);
            break;
         case UI_ACT_NET_JOIN:
            ui_net_action(0);
            break;
         case UI_ACT_NET_DISCONNECT:
            net_teardown();
            osd_toast("Disconnected");
            break;
         case UI_ACT_EXIT:
            g_running = 0;
            break;
         case UI_ACT_GAMELIST:
            /* Same ADR-0071 relaunch flow as the Media Engine toggle — full
             * clean shutdown (SRAM flushed, threads stopped), then the app
             * replaces itself and boots into the ROM browser. */
            pcfg_save();
            g_relaunch = 1;
            exit_reason = "game_list";
            g_running = 0;
            osd_toast("Returning to game list");
            break;
         case UI_ACT_RELAUNCH:
            /* ADR-0071: the trading profile changed.  Persist it, then leave
             * the loop by the NORMAL exit path — SRAM flushed, io thread
             * stopped, log closed — and relaunch at the very end, where the
             * USB handoff already proves it is safe to replace the process. */
            pcfg_save();
            g_relaunch = 1;
            exit_reason = "profile_change";
            g_running = 0;
            osd_toast("Restarting: Media Engine %s",
                      g_pcfg.me_mode ? "ON" : "OFF");
            break;
         default:
            break;
         }
         osd_draw();
         sceDisplayWaitVblankStart();
         vid_swap();
         continue;
      }

      /* ---- Triangle: cycle video preset ------------------------------ */
      if (pad_new & PSP_CTRL_TRIANGLE)
      {
         const char *name = vid_cycle_preset();
         g_pcfg.scale  = vid_scale_mode();
         g_pcfg.filter = vid_filter();
         pcfg_save();
         fe_evt("video_mode scale=%s filter=%s",
                vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter));
         osd_toast("Video: %s", name);
      }

      /* ---- fast-forward (plan §4.4) ---------------------------------- */
      {
         int want_ff;
         if (g_pcfg.ff_hold)
            want_ff = (g_pad & PSP_CTRL_SQUARE) ? 1 : 0;
         else
         {
            if (pad_new & PSP_CTRL_SQUARE)
               ff_toggled = !ff_toggled;
            want_ff = ff_toggled;
         }
         if (simff && fe_host_frame_count() >= 300 &&
             fe_host_frame_count() < 300 + (unsigned)simff)
            want_ff = 1;   /* harness-simulated FF hold */
         if (session && want_ff)
         {
            /* Interlock: FF forced 1x during wireless sessions. */
            if (pad_new & PSP_CTRL_SQUARE)
               osd_toast("Fast-forward locked: wireless session");
            want_ff = 0;
            ff_toggled = 0;
         }
         if (want_ff != ff_engaged)
         {
            ff_engaged = want_ff;
            if (ff_engaged)
            {
               /* Engage core frameskip for FF headroom (hw ceiling is
                * ~1.1-2.3x uncapped without it).  auto frameskip needs the
                * audio-buffer-status env we decline; fixed_interval is the
                * deterministic equivalent for FF (muted audio anyway). */
               if (g_pcfg.ff_smooth || g_pcfg.bench_mode)
               {
                  /* Smooth FF: render EVERY frame.  Lower peak multiplier
                   * than the skipping path, but the motion reads as fast
                   * motion instead of a slideshow — and it is also the
                   * apples-to-apples configuration for benchmarking against
                   * an emulator running uncapped with frameskip off. */
                  fe_host_option_set_live("gpsp_frameskip", "disabled");
                  if (g_pcfg.bench_mode)
                     osd_chip_ff("\xAF BENCH");
                  else
                     osd_chip_ff(g_pcfg.ff_mult_x10 == 30 ? "\xAF 3.0x~" :
                                 g_pcfg.ff_mult_x10 == 0  ? "\xAF MAX~"  :
                                                            "\xAF 1.5x~");
               }
               else
               {
               fe_host_option_set_live("gpsp_frameskip", "fixed_interval");
               fe_host_option_set_live("gpsp_frameskip_interval", "1");
               if (g_pcfg.ff_mult_x10 == 0)
                  osd_chip_ff("\xAF MAX");
               else
                  osd_chip_ff(g_pcfg.ff_mult_x10 == 30 ? "\xAF 3.0x" :
                                                         "\xAF 1.5x");
               }
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
               ff_acc = 0;
            }
         }
         g_ff_uncapped = harness_ff || (ff_engaged && g_pcfg.bench_mode) ||
                         (ff_engaged && g_pcfg.ff_mult_x10 == 0);
         g_ff_mult = ff_engaged && !g_ff_uncapped;
         if (session)
            g_ff_uncapped = g_ff_mult = 0;   /* belt over the interlock */
      }

      /* ADR-0082: present the ME-staged frame BEFORE running the core, so
       * the GE rasterises through the whole retro_run below and the
       * pre-swap sync is ~0.  No-op unless me_video is active. */
      me_video_present();
      me_rend_present();   /* ME renderer: swap-phase present (FF triage) */

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
                                 gamepak_page_loads - g_fps_last_pg);
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
                           smc_blk_xlat_total - g_fps_last_xlat,
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
      if (g_ff_mult)
      {
         int runs, i;
         ff_acc += g_pcfg.ff_mult_x10;
         runs = ff_acc / 10;
         ff_acc %= 10;
         if (runs < 1)
            runs = 1;
         for (i = 0; i < runs; i++)
         {
            g_blit_suppress = (i != runs - 1);
            fe_autopilot_frame();
            fe_host_run_frame();
         }
         g_blit_suppress = 0;
      }
      else
      {
         fe_autopilot_frame();
         fe_host_run_frame();
      }
      pace_burn();               /* ADR-0027 harness knob, normally a no-op */
      preempt_mark(1);           /* ADR-0064: close `core`, open `post` */
      frames = fe_host_frame_count();

      if (vid_prof_win > 0)
         vid_prof_frame((unsigned)vid_prof_win);

      if (vhash_to && frames >= (unsigned)vhash_from &&
          frames <= (unsigned)vhash_to)
         vhash_frame();

      if (dump_at && frames >= (unsigned)dump_at)
      {
         dump_at = 0;
         dump_frame_bmp();
      }
      if (dump_every && (frames % (unsigned)dump_every) == 0)
         dump_frame_bmp();
      if (fe_autopilot_dump_pending())
         dump_frame_bmp();
      if (g_dump_chord_pending)      /* ADR-0069, set at the pad read above */
      {
         g_dump_chord_pending = 0;
         dump_frame_bmp();
      }
      if (gedump_at && frames >= (unsigned)gedump_at && g_drew)
      {
         gedump_at = 0;
         dump_ge_bmp();   /* drawbuffer is drawn + synced, not yet swapped */
      }
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
      blit_prof_evt(frames);

      if (have_script)
      {
         int st = fe_autopilot_status();
         if (st == 1)
            g_running = 0;              /* script finished: clean exit */
         else if (st == -1)
         {
            exit_code = 3;
            exit_reason = "ap_fail";
            g_running = 0;
         }
      }
      if (autoexit && frames >= (unsigned)autoexit)
         g_running = 0;

      emu_boost_close();         /* ADR-0065: end the protected window */
      preempt_mark(2);           /* ADR-0064: close `post`, open `wait` */
      preempt_frame_end();

      if (frame_t0)
      {
         /* One clock read, two consumers: ADR-0021's profile and ADR-0027's
          * capability estimate both want this frame's WORK time (every
          * vblank wait below is deliberately excluded). */
         uint32_t work_us = (uint32_t)(net_now_us() - frame_t0);
         fe_np_prof_frame(work_us);
         frame_hist_note(work_us);        /* ADR-0063: no extra clock read */
         pace_frame(work_us);
      }
      else
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
         unsigned extra = g_ff_mult ? 0u : pace_extra_vblanks();
         unsigned want  = 1u + extra;
         if (!g_ff_mult && ring_w - ring_r > RING_HIGH_WATER)
         {
            want++;               /* audio-ring nudge (plan §4.4), unchanged */
            g_fh_nudge++;         /* ADR-0063: previously unobservable */
         }
         if (g_drew)
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
         }
         while ((int)(sceDisplayGetVcount() - g_vc_target) < 0)
            sceDisplayWaitVblankStart();

         vid_swap();
         g_drew = 0;
      }
      else if (g_drew)
      {
         /* Uncapped FF: swap only when a frame was actually blitted. */
         osd_draw();
         vid_swap();
         g_drew = 0;
      }
   }

   /* Clean exit (incl. HOME): stop the netdrv session + full adhoc
    * teardown first (BYE to peers while the radio is still up), then SRAM
    * is flushed inside fe_host_shutdown (dirty-check). */
   /* ADR-0080: stop the ME, then free our staging buffers (ADR-0080e: no
    * second core buffer to free; gba_screen_pixels was never reassigned). */
   me_rend_teardown("exit");
   me_video_teardown("exit", 1);
   me_host_shutdown();
   free(g_me_stage[0]);   g_me_stage[0] = NULL;
   free(g_me_stage[1]);   g_me_stage[1] = NULL;
   net_teardown();
   emu_boost_stop();   /* ADR-0065: before anything else changes priority */
   io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
   fe_host_shutdown();
   audio_stop();
   vid_term();
   fe_evt("audio_hash %08x samples=%u", plat_audio_hash(),
          plat_audio_sample_count());
   if (exit_reason)
      fe_evt("exit code=%d reason=%s", exit_code, exit_reason);
   else
      fe_evt("exit code=%d", exit_code);
   evt_shutdown();
   /* ADR-0053: last thing before the XMB.  Every ms0 writer is stopped and the
    * log is closed, so it is safe to hand the stick to the PC here and nowhere
    * earlier.  Does not return if the PC asks for another run. */
   handoff_run(exit_code, exit_reason);
   /* ADR-0071: relaunch ourselves so the new trading profile is latched at
    * startup like any other boot.  Same call the USB handoff uses and for the
    * same reason: on CFW an EBOOT.PBP is launched the way the FIRMWARE
    * launches it, and plain sceKernelLoadExec returns instead of replacing the
    * process.  argp becomes the new argv[0], which is where every ms0 path is
    * derived from — passing NULL would silently fall back to the compiled-in
    * default directory and lose a variant install's saves. */
   if (g_relaunch)
   {
      char eboot[160];
      struct SceKernelLoadExecVSHParam param;
      snprintf(eboot, sizeof(eboot), "%s/EBOOT.PBP", g_dir_base);
      memset(&param, 0, sizeof(param));
      param.size = sizeof(param);
      param.args = (SceSize)(strlen(eboot) + 1);
      param.argp = eboot;
      param.key  = "game";
      sctrlKernelLoadExecVSHMs2(eboot, &param);
      /* Returned?  Then fall through to the XMB rather than hanging. */
   }
   sceKernelExitGame();
   return 0;
}
