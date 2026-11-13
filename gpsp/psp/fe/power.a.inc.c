/* --- fast-forward state --------------------------------------------------
 * 3x batches three emulated frames and presents the final capture.
 * Unlimited and Unlimited Smooth run without vblank pacing and DRAW at most
 * 30 / 60 frames a second (the draw gate, ff_gate.h, docs/FF-PRESETS.md): a
 * frame the gate turns away is emulated but never captured or rendered.
 * The 1-in-32 CPU presentation mask is retained only for the harness. */
#define FF_PRESENT_MASK 31
int g_ff_uncapped;
int g_ff_mult;         /* multiplier FF active (user, paced) */
static int g_ff_user;         /* distinguishes user profiles from the harness */
static int g_ff_gate;         /* Unlimited / Smooth: the draw gate is on */
static ff_gate_t g_ff_gate_st;
static int g_blit_suppress;   /* skip blits of intermediate FF frames */
static int g_drew;            /* a GU frame was drawn since the last swap */

/* EVT ff_win (docs/FF-PRESETS.md): NEW images put in the back buffer -- an ME
 * stage presented for the first time, or a CPU frame the core really drew.
 * Re-presents of an unchanged picture are not counted.  Telemetry builds
 * only; a player build compiles the counter out. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
static unsigned g_ffw_drawn, g_ffw_gated;
#else
/* FFW_DRAWN(), FFW_GATED(): this arm's definitions are in fe/fe_priv.h */
#endif

/* ------------------------------------------------------------------- exit */

/* HOME WAS PRESSED, FOR GOOD (PLAN HW-2 F1).  g_running is the loop's "keep
 * going", and the in-process return sets it to 1 again for the next game; a
 * HOME that lands in the quit's teardown was lost there and the list then
 * looped on it ("Please wait" for ever), or the refused tail relaunched the
 * app instead of leaving.  This one is sticky: set only here, never cleared,
 * read by inproc_refusal ("home"), the return's g_running and the exit's
 * relaunch. */
static volatile int g_exit_req;

static int exit_cb(int arg1, int arg2, void *common)
{
   (void)arg1; (void)arg2; (void)common;
   /* Main loop notices, flushes SRAM, logs EVT exit, then exits the game.
    * cb_thread: set two flags, never block, never tear anything down. */
   g_exit_req = 1;
   g_running = 0;
   return 0;
}

/* ---- standby / resume --------------------------------------------------
 *
 * The PSP KEEPS RAM across suspend, so nothing about the emulated machine
 * needs saving and restoring -- only peripherals die: the GU/display, the
 * audio channels, WLAN, and the Media Engine.  Waking is therefore a
 * re-initialisation problem, not a state problem.
 *
 * This first step does the half that is unambiguously safe: flush the .sav
 * on the way down, and record what the power system actually told us.  A
 * PSP Go's slider is a suspend request like any other, and today closing it
 * mid-game can cost unsaved progress.
 *
 * THE CALLBACK MUST NOT DRAIN.  fe_host.h's contract is that exactly one
 * thread calls fe_host_sram_service_io(), and that is the io thread.  So we
 * raise the flag, WAKE the writer, and yield long enough for it to be
 * scheduled before the machine goes down.  Draining from here would be a
 * second writer and could interleave mid-block. */
static volatile int g_pwr_suspend, g_pwr_resume;
/* GB/GBC battery RAM on suspend.  The io thread's flush is GBA-only and
 * the GB core's cartridge RAM may only be read on the emulation thread, so the
 * callback raises this for EVERY suspend (standby teardown or not) and the
 * main loop services it at the top of its next iteration, ahead of a park.
 * Known gap: a main thread inside a GB link byte wait (<= 3 s) services it
 * too late for that suspend. */
static volatile int g_pwr_gb_flush_req;

static int power_cb(int unknown, int pwrflags, void *common);
/* The GBA cartridge clock's wall time (gba_memory.c gpsp_rtc_wall,
 * rtc_clock.h): the console's local clock, refreshed at most five times a
 * second from the main loop.  A reading that fails (0) leaves the last one
 * in place; the core's guard holds the clock meanwhile. */
extern volatile long long gpsp_rtc_wall;
static int g_rtc_wall_on;
static unsigned g_rtc_wall_us;
static time_t psp_local_wallclock(void);
static void rtc_wall_publish(int force)
{
   unsigned now = (unsigned)sceKernelGetSystemTimeLow();
   time_t t;
   if (!g_rtc_wall_on)
   {
      gpsp_rtc_wall = 0;
      return;
   }
   if (!force && now - g_rtc_wall_us < 200000u)
      return;
   g_rtc_wall_us = now;
   t = psp_local_wallclock();
   if (t > 0)
      gpsp_rtc_wall = (long long)t;
}
static unsigned g_stick_state;      /* ctl_stick_bits: engaged directions */
#ifdef GPSP_PERF_RIG
/* Harness: the stick's raw range over the run (`EVT stick_range`), for
 * reading a real, worn stick's resting position. */
static unsigned char g_stick_min[2] = { 255, 255 }, g_stick_max[2] = { 0, 0 };
#endif

static SceUID g_cb_thid = -1;
static volatile int g_pwr_req_signal;   /* power_request_poll to cb_thread */
/* @extract:begin fn:cb_thread */
static int cb_thread(SceSize args, void *argp)
{
   (void)args; (void)argp;
   int cbid = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
   int pcb  = sceKernelCreateCallback("power_cb", power_cb, NULL);
   sceKernelRegisterExitCallback(cbid);
   if (pcb >= 0)
      scePowerRegisterCallback(0, pcb);
   g_cb_thid = sceKernelGetThreadId();
   /* Nothing wakes this thread but a callback, or power_request_poll. */
   for (;;)
   {
      sceKernelSleepThreadCB();
      if (g_pwr_req_signal)
      {
         g_pwr_req_signal = 0;
         power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
      }
   }
   return 0;
}
/* @extract:end fn:cb_thread */

/* r1-W8: the thread's id is known from its creation (g_cb_thid was only
 * set by the thread itself, a race with power_request_poll's wakeup), and a
 * failure is no longer silent: without this thread HOME and sleep do
 * nothing, so it is logged and the game says so (g_cb_failed). */
static int g_cb_failed;
static void setup_exit_callback(void)
{
   int rc;
   int thid = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0,
                                    THREAD_ATTR_USER, NULL);
   rc = thid;
   if (thid >= 0)
   {
      g_cb_thid = thid;
      rc = sceKernelStartThread(thid, 0, NULL);
      if (rc < 0)
      {
         sceKernelDeleteThread(thid);
         g_cb_thid = -1;
      }
   }
   if (rc < 0)
   {
      g_cb_failed = rc;
      fe_evt("cb_thread FAILED rc=%08x", (unsigned)rc);
   }
}

