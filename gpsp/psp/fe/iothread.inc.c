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
/* @extract:begin def:IO_JOIN_US */
#define IO_JOIN_US     5000000u   /* the last pass may be flushing 128 KiB */
/* @extract:end def:IO_JOIN_US */
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

/* @extract:begin fn:io_thread */
static int io_thread(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (g_io_run)
   {
      SceUInt tmo = IO_WAIT_US;
      sceKernelWaitSema(g_io_sema, 1, &tmo);
      if (g_pcfg.log_thread)
         fe_evt_service();
      if (g_pcfg.sram_thread)
         fe_host_sram_service_io();
   }
   /* The other service can still belong to the main thread. A logging-only
    * worker must never become a second consumer of the SRAM block queue. */
   if (g_pcfg.sram_thread)
      fe_host_sram_service_io();   /* last pass before we go away */
   if (g_pcfg.log_thread)
      fe_evt_service();
   return 0;
}
/* @extract:end fn:io_thread */

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
/* Returns 0 = the thread was joined, 1 = there was no thread, < 0 = the
 * join timed out and the thread was terminated (the kernel's rc). */
static int io_thread_stop(void)
{
   int had_thread = (g_io_thid >= 0);
   int rc = 1;

   if (had_thread)
   {
      SceUInt tmo = IO_JOIN_US;
      g_io_run = 0;
      sceKernelSignalSema(g_io_sema, 1);
      rc = sceKernelWaitThreadEnd(g_io_thid, &tmo);
      if (rc < 0)
         sceKernelTerminateDeleteThread(g_io_thid);
      else
      {
         sceKernelDeleteThread(g_io_thid);
         rc = 0;
      }
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
   return rc;
}

/* Every exit path funnels through here so the log is always closed with the
 * writer already stopped and the ring already drained. */
static void evt_shutdown(void)
{
   io_thread_stop();
   fe_evt_close();
}

