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

