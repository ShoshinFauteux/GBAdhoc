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
/* VRAM dirty map (gba_memory.c) -- set by the C store macros and the DMA
 * path, NOT by translated code.  The probe below reports how much that
 * misses, which is what decides whether the dynarec store stub has to be
 * instrumented at all. */
#define VRAM_DIRTY_SHIFT 10
#define VRAM_DIRTY_PAGES ((1024 * 96) >> VRAM_DIRTY_SHIFT)
/* vram, vram_clean (1 = clean, 0 = dirty), vram_dirty_marks, io_registers,
 * oam_ram, palette_ram_converted and reg: core_fe.h.  The two #defines above
 * repeat core_fe.h's token for token (T14 asserts them). */

/* MUST equal video_psp.c's STAGE_STRIDE (256 px = 512-byte rows): the GE
 * texture reads rows at that spacing.  The first engine run shipped 512 px
 * here — a 2x stride mismatch, and the user's screen showed the textbook
 * signature: alternating black scanlines + the bottom half of the frame
 * falling off (each texture row pair = one real row + the empty half). */
#define MER_STAGE_PITCH   256            /* GE texture stride, pixels */
#define MER_RETIRE_US     9000           /* spin budget for a late ME frame */
#define MER_REWIND_RETIRE_US 2500        /* ...for a frame a rewind re-runs */
#define MER_INPUT_US      50000          /* input handshake hard ceiling */
#define MER_MISS_LIMIT    120            /* consecutive drops -> teardown */

static int g_me_rend_cfg;                /* wanted (pcfg/harness) */
static int g_me_rend;                    /* active */

/* STANDBY, STAGE 1: prove the resume path exists.
 *
 * The PSP cannot wake cleanly with the Media Engine running.  The kernel's
 * suspend does not cover the second core: its state lives in memory the power
 * manager does not preserve, and our engine buffers are carved out of the
 * VOLATILE partition (the 4 MB OS pool at 0x08400000), which is precisely the
 * memory the OS is free to reclaim while the machine is asleep.  So resume
 * cannot be a restore -- it has to be a REBUILD.
 *
 * What survives is the part that matters: SRAM, the dynarec caches and every
 * emulator structure live in the malloc heap, which the kernel does preserve.
 * So the GAME does not need restoring at all; only the presentation layer
 * does.  That is the claim stage 1 exists to test, and the failure mode if it
 * is wrong is a black screen needing a battery pull -- which is why this is
 * behind a config key rather than on by default.
 *
 * Split by thread, deliberately.  TEARDOWN runs here in the callback because
 * the machine is going away and there is no later; the callback already does
 * io_wake plus an 80 ms yield, so it is a context that tolerates real work.
 * REBUILD only sets a flag: module loading and GU work belong on the main
 * thread, and doing them from a callback is how you get a hang that looks
 * like the hardware's fault. */
static void me_standby_down(void);   /* defined with the ME globals below */
static void me_standby_up(void);
static int me_rend_active(void);
static int me_rend_vmem_held(void);
#ifdef REWIND
static int psp_rewind_pool_tenant(void);   /* the pool's second tenant */
#endif
static int me_rend_release_vmem(void);
static void standby_note(const char *fmt, ...);   /* defined below */
static volatile int g_pwr_slept;     /* we tore the engine down for a sleep */
static volatile int g_pwr_had_host;  /* the ME module was up when we slept */
static uint16_t *g_wake_frame;       /* the picture the overlay sits on */
static int       g_wake_have;        /* 1 = it holds a real frame       */
/* The snapshot's own size: 240x160 for a GBA, 160x144 for a GB/GBC.  It is
 * stored at the texture's top-left and drawn with these dimensions, so it
 * lands exactly where (and as large as) the live picture did. */
static unsigned  g_wake_w = 240, g_wake_h = 160;
/* The in-game menu's state-slot thumbnails (docs/UI-OVERLAY.md): five 64x43
 * PSP-5650 textures, also carved from the netdrv arena's tail -- no heap. */
_Static_assert(PSP_STATE_SLOT_COUNT == 5, "UI_THUMB_BYTES: five slots");
static uint16_t *g_ui_thumbs;
static int       g_wake_menu;        /* main loop: show the overlay     */
static const uint16_t *g_last_pix;   /* last frame the core handed us    */
/* 1 from the teardown until the overlay is dismissed: nothing else may present
 * or swap, so the first thing the player sees after the screen lights is the
 * overlay and not the frame we slept on.
 *
 * g_pwr_slept is what covers the START of that window.  The power callback
 * blanks VRAM, releases the parked loop and then waits 80 ms for the .sav
 * writer before the kernel takes the machine.  Holding only from
 * RESUME_COMPLETE (g_pwr_rebuild) let the loop swap a few more frames of
 * gameplay over the blank in those 80 ms, so the LCD lit with the old frame
 * and the overlay replaced it ~0.4 s later (PSP-1000, 3.0.0, on video). */
/* @extract:begin def:g_wake_hold */
#define g_wake_hold  (g_pwr_slept || g_pwr_rebuild || g_wake_menu)
/* @extract:end def:g_wake_hold */
static unsigned  g_last_pitch = 240;
static unsigned  g_last_w = 240, g_last_h = 160;
static volatile int g_pwr_rebuild;   /* main loop: rebuild it now */
/* SUSPEND HANDSHAKE.  The callback sets park_req; the main loop, at the top
 * of its next iteration, sets parked and waits until park_req clears.  See
 * power_cb for why the teardown must not run while a frame is in flight. */
static volatile int g_pwr_park_req;
static volatile int g_pwr_parked;
/* THE SLEEP QUIESCE (psp/sleep_q.h).  The park point does real work before
 * it reports `parked`: it stops the audio thread, the writer, the long-lived
 * files, the HOME module and the radio.  So the handshake has two answers:
 *   g_pwr_park_ack  the loop has SEEN the request (it is at its park point and
 *                   quiescing) -- awaited for PWR_PARK_WAIT_US, as before, so
 *                   a loop that has no park point (browser, menus) still costs
 *                   the callback 250 ms and no more;
 *   g_pwr_parked    the quiesce is finished -- awaited, once acknowledged,
 *                   up to PWR_QUIESCE_WAIT_US: the quiesce's own worst
 *                   case, so the callback never gives up on a loop that is
 *                   only slow.  A loop still quiescing past even that keeps
 *                   the gameplay lock, parked, until the SUSPENDING that
 *                   follows has torn down (sleep_park_hold), so the suspend
 *                   never lands mid-I/O or with the renderer up.
 * Everything blocking runs on the EMULATION thread, never here: it owns the
 * ROM handle and the core, and joining the audio and writer threads from it
 * cannot deadlock against this callback thread (0x11), which outranks them.
 * The callback only waits, bounded, which is what makes its return prompt.
 *
 * g_pwr_cb_done      the callback finished its part (the ME teardown); a
 *                    loop held in the park may now let the suspend proceed.
 * g_pwr_sleep_pending a switch/suspend was seen and RESUME_COMPLETE has not
 *                    come yet: the gameplay power lock stays down, and the
 *                    quiesce stays in force, until it does. */
static volatile int g_pwr_park_ack;
static volatile int g_pwr_cb_done;
static volatile int g_pwr_sleep_pending;
/* power_request_poll: sleeps it started that no wake has answered yet. */
static int g_pwr_req_fires;
/* The gameplay scePowerLock is held (play_lock_take). */
static volatile int g_pwr_play_lock;
/* The loop that answered `parked` is the game list's (browser_park), not the
 * game's: nothing to tear down but the volatile partition, no wake overlay
 * to ask for (g_pwr_slept stays 0). */
static volatile int g_pwr_park_browser;
/* Each step at its bound: the audio join, the writer's join, the .sav it
 * may leave to finish on the loop's thread (4 s: SRAM_SYNC_MAX_SPINS' "a
 * stick that has not taken 128 KiB in that is broken"), the radio
 * (adhoc_transport_term: two 1.5 s joins, a 0.2 s drain, a 2 s spin), and a
 * second for the rest (the HOME module's stop, the handles). */
/* @extract:begin def:sq_bounds */
#define SQ_SAV_LAND_US 4000000u
#define SQ_NET_TERM_US 5500000u
#define PWR_QUIESCE_WAIT_US (AUDIO_JOIN_US + IO_JOIN_US + SQ_SAV_LAND_US + \
                             SQ_NET_TERM_US + 1000000u)
/* @extract:end def:sq_bounds */
static void play_lock_drop(const char *who);
#ifdef REWIND
/* When the callback asked (it waits PWR_PARK_WAIT_US for the answer): what
 * the park point may spend landing a rewind first (psp_rewind_land). */
static volatile unsigned g_pwr_park_t0;
/* A resume found that the suspend before it tore nothing down while the
 * rewind lived in the volatile pool (psp_rewind_pool_restart). */
static volatile int g_rw_pool_suspect;
static int psp_rewind_land(const char *why, unsigned budget_us);
#endif
#define PWR_PARK_WAIT_US 250000u

/* ---- CPU CLOCK (Settings > "CPU clock"; config.ini / harness cpu_clock) ----
 *
 * Every release since 3.5.0 forced 333/333/166 at entry (ADR-0004).  That
 * stays the default; 222 and System are the player's choice (a CFW such as
 * ARK-4 can set a clock of its own, and System leaves it alone: this file
 * then never calls scePowerSetClockFrequency at all).
 *
 * Nothing else in the frontend assumes 333.  Every pacing, audio, FPS-chip,
 * rewind-budget, link and Media Engine timeout is wall-clock time from
 * sceKernelGetSystemTime*; the only iteration-counted waits are the ME's idle
 * polls (psp/me/main.c ME_IDLE_SPIN, ~50 us at 333), which at 222 poll every
 * ~75 us -- latency to start a posted job, not correctness.  What a lower
 * clock costs is speed: a game that needs most of a frame at 333 drops
 * frames at 222, and a link session runs at its slower console's pace.
 *
 * psp_clock_capture runs before anything can set a clock, so System can also
 * give back what was there after a change from the menu. */
static int g_clk_sys_cpu, g_clk_sys_bus;   /* the clock the EBOOT found */
static int g_clk_set;                      /* 1 = ours is in force        */
static int g_clk_harness = -1;             /* harness cpu_clock; -1 none  */

static void psp_clock_capture(void)
{
   g_clk_sys_cpu = scePowerGetCpuClockFrequency();
   g_clk_sys_bus = scePowerGetBusClockFrequency();
}

/* The mode in force: the harness key (Lab arms; never saved), else the
 * setting.  A menu change drops the harness value (ui_psp.c). */
int psp_clock_mode(void)
{
   return g_clk_harness >= 0 ? g_clk_harness : g_pcfg.cpu_clock;
}

void psp_clock_harness_drop(void)
{
   g_clk_harness = -1;
}

/* The clock actually in force, in MHz.  Not always the one asked for: a
 * CFW with an in-game clock of its own (the Lab's PSP-1000, 2026-11-02)
 * answers scePowerSetClockFrequency with 0 and keeps its clock, so the
 * Settings row says when that happens (ui_psp.c). */
int psp_clock_actual(void)
{
   return scePowerGetCpuClockFrequency();
}

/* FILTERS V2: apply the live display profile's Smoothing / Screen effect
 * rows (the GE) and its Colour row (the core, GBA only; video.h).  The CPU
 * renderer's two 1 KiB palette copies are this frontend array, so the core's
 * own .bss is unchanged (docs/LAYOUT-PINNING.md). */
static u16 g_vc_work[1024] __attribute__((aligned(64)));
#ifdef GPSP_PERF_RIG
static int g_h_smooth = -1, g_h_effect = -1, g_h_color = -1, g_h_scale = -1;
#endif
void psp_display_apply(const char *why)
{
   int scale = g_pcfg.scale, sm = g_pcfg.smooth, fx = g_pcfg.effect;
   int color = pcfg_display_console() == FE_CONSOLE_GBA ? g_pcfg.color : 0;
#ifdef GPSP_PERF_RIG
   /* Rig overrides reach the video layer only, never g_pcfg: nothing a rig
    * run forces can be saved into a console's CONFIG.INI. */
   if (g_h_scale >= 0 && g_h_scale < VID_SCALE_MODES)   scale = g_h_scale;
   if (g_h_smooth >= 0 && g_h_smooth < VID_SMOOTH_MODES) sm = g_h_smooth;
   if (g_h_effect >= 0 &&
       vid_fx_offered(g_h_effect, pcfg_display_console() != FE_CONSOLE_GBA))
      fx = g_h_effect;
   if (g_h_color >= 0 && pcfg_display_console() == FE_CONSOLE_GBA)
      color = g_h_color;
#endif
   vid_set_look(scale, sm, fx);
   video_color_set((u32)color, gba_cc_lut, g_vc_work);
   fe_evt("display_apply why=%s console=%s scale=%s smooth=%s effect=%s "
          "color=%d", why, pcfg_console_tag(pcfg_display_console()),
          vid_scale_name(scale), vid_smooth_name(sm), vid_fx_name(fx),
          (int)video_color_mode);
   FE_EVT_ONLY(why); FE_EVT_ONLY(scale);
}

void psp_clock_apply(const char *why)
{
   int mode = psp_clock_mode();
   int cpu0 = scePowerGetCpuClockFrequency();
   int bus0 = scePowerGetBusClockFrequency();
   int rc = 0, call = 0;

   if (mode != PCFG_CLOCK_SYSTEM)
   {
      /* pll = cpu = bus * 2, as 333/333/166 always was. */
      rc = scePowerSetClockFrequency(mode, mode, mode / 2);
      call = 1;
      g_clk_set = 1;
   }
   else if (g_clk_set && g_clk_sys_cpu > 0 && g_clk_sys_bus > 0)
   {
      /* System after one of ours: give back the clock found at entry.  The
       * PLL is not readable through the SDK; the lowest one both fit under
       * (cpu <= pll, bus * 2 <= pll) is what a CFW's own settings use. */
      int pll = g_clk_sys_cpu > g_clk_sys_bus * 2 ? g_clk_sys_cpu
                                                  : g_clk_sys_bus * 2;
      if (pll > 333)
         pll = 333;
      rc = scePowerSetClockFrequency(pll, g_clk_sys_cpu, g_clk_sys_bus);
      call = 1;
      g_clk_set = 0;
   }
   /* held=1: the call was made and the clock did not move (a CFW's). */
   fe_evt("clock why=%s mode=%s src=%s was=%d/%d now=%d/%d entry=%d/%d "
          "call=%d rc=%d held=%d", why, pcfg_clock_name(mode),
          g_clk_harness >= 0 ? "harness" : "config", cpu0, bus0,
          scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency(),
          g_clk_sys_cpu, g_clk_sys_bus, call, rc,
          call && mode != PCFG_CLOCK_SYSTEM &&
             scePowerGetCpuClockFrequency() != mode);
   FE_EVT_ONLY(cpu0); FE_EVT_ONLY(bus0); FE_EVT_ONLY(rc); FE_EVT_ONLY(why);
   FE_EVT_ONLY(call);
}

