/* usb_handoff.c — ADR-0053: hand ms0 to the PC between runs, then relaunch.
 *
 * WHY THIS AND NOT PSPLINK: the log has to stay on the memory stick.  Shipping
 * it over a wire while the run is live perturbs exactly the I/O timing the run
 * exists to measure, and this project has already spent a day on stalls
 * measured in microseconds.
 *
 * WHY NO XMB: the emulator does not exit to the XMB and does not need anyone to
 * navigate to "USB Connection".  sceUsbActivate makes THIS process the mass
 * storage device, and sceKernelLoadExec relaunches THIS process.  The XMB only
 * ever sees us if the loop gives up and falls through to sceKernelExitGame().
 *
 * THE WINDOW IS A POLLING PERIOD, NOT A DEADLINE.  We serve the volume for
 * one window, release it, and only then look for the command file -- never
 * while it is exported.  There is deliberately NO attempt to cut the window
 * short by detecting the PC's eject; see the note in the wait loop for why no
 * such signal exists in the USB state.
 *
 * If no command has appeared when the window closes, we RE-ARM instead of
 * giving up, until the total budget is spent.  This matters because the PC
 * services both consoles together, so whichever finishes first has to outlast
 * its peer's remaining run -- and run lengths differ by minutes when one side
 * takes the graceful-disconnect path.  A single fixed window silently dropped
 * the early console out of the chain.
 *
 * SAFETY, in the order it matters:
 *   - handoff_run() is called after evt_shutdown(), i.e. after net_teardown,
 *     io_thread_stop and fe_host_shutdown.  Every thread that can touch ms0 is
 *     already stopped and the log is closed before USB is offered.
 *   - if the PC never writes a command within the total budget, we return and
 *     exit normally.  A silent PC must never strand a console in USB mode.
 *   - max_runs bounds the chain, so a stuck script cannot relaunch forever.
 *   - the whole feature is off unless `.gpsp-harness.ini` says otherwise.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspusbstor.h>
#include <psputility.h>
#include <psputility_usbmodules.h>
#include <pspmodulemgr.h>
#include <kubridge.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>
#include <pspctrl.h>
#include <psppower.h>

#include "usb_handoff.h"

extern char g_dir_base[128];

#define HANDOFF_PID          0x1c8         /* mass storage product id */
#define STORAGE_CAPACITY     (8 * 1024 * 1024)
#define POLL_MS              200

static int  h_enabled;
static int  h_window_s = 90;
static int  h_max_runs = 20;
static int  h_total_s   = 900;   /* whole-handoff budget across re-arms */
/* ADR-0056: the console PARKS; it does not leave.
 *
 * Originally an exhausted budget, or a STOP, dropped the console to the XMB --
 * where it is neither running nor exporting its card, so NOTHING the operator
 * does can reach it and a human has to press X to start the next batch.  That
 * defeated the whole loop.
 *
 * Now the end of a batch parks: USB stays offered on a cycle, the card stays
 * visible, and the console waits to be told to go again.  "Run 5 times and
 * stop" therefore ends with two consoles sitting reachable, and starting the
 * next batch is the operator writing CMD.TXT -- never touching the hardware.
 *
 * Only an explicit CMD=EXIT gives up the loop.  h_park_s bounds the parking
 * (0 = park indefinitely, which is the sane default for a console that lives
 * plugged into the rig). */
static int  h_park_s = 0;
/* Cap for the backed-off parked window.  A console that has been parked for
 * an hour has no reason to re-offer its card every 30 s: each cycle is a
 * mount + unmount on the host, which on Windows means an AutoPlay popup and a
 * pointless FAT remount.  The window therefore DOUBLES on each empty cycle up
 * to this, and snaps back to the configured value the moment a command lands,
 * so the loop stays responsive right after a run and goes quiet when idle. */
#define PARK_WINDOW_MAX_S  300

/* A PARKED CONSOLE MUST BE ESCAPABLE.
 *
 * handoff_run() executes after vid_term(), so a parked console is a black
 * screen that polls no input: indistinguishable from a hang, and the only way
 * out was holding POWER for ten seconds.  An unattended rig is still a thing a
 * person picks up.  Holding this combo leaves the loop and returns to the XMB
 * the ordinary way.  START+SELECT because no single button can be pressed by
 * accident in a bag, and both are far from POWER. */
#define ESCAPE_MASK  (PSP_CTRL_START | PSP_CTRL_SELECT)
#define ESCAPE_MS    1500

static int h_escape_held(int *held_ms)
{
   SceCtrlData pd;
   sceCtrlPeekBufferPositive(&pd, 1);
   if ((pd.Buttons & ESCAPE_MASK) == ESCAPE_MASK)
   {
      *held_ms += POLL_MS;
      return *held_ms >= ESCAPE_MS;
   }
   *held_ms = 0;
   return 0;
}

static char h_dir[160], h_result[192], h_cmd[192], h_state[192], h_runs[192];

void handoff_config(int enabled, int window_s, int max_runs, int total_s,
                    int park_s)
{
   h_enabled = enabled;
   h_park_s  = park_s;
   if (window_s > 0)
      h_window_s = window_s;
   if (max_runs > 0)
      h_max_runs = max_runs;
   if (total_s > 0)
      h_total_s = total_s;
   if (h_total_s < h_window_s)
      h_total_s = h_window_s;   /* a budget below one window is not a budget */
}

/* NEVER TOUCH ms0 WHILE IT IS EXPORTED OVER USB.
 *
 * This is the whole safety property of the handoff and I broke it with my own
 * diagnostics: h_note() wrote STATE.TXT with sceIoOpen/sceIoWrite from inside
 * the wait loop, i.e. while the memory stick was mounted on the PC.  Two
 * independent writers on one FAT volume corrupts the directory -- staged files
 * vanished, the ROM disappeared twice, and the XMB reported "Corrupted Data".
 *
 * Notes are therefore BUFFERED in RAM while the volume is exported and flushed
 * only once USB is down.  h_usb_active gates it; nothing else may open a file
 * while that flag is set. */
static void h_note(const char *fmt, ...);   /* used by the ms0 gate below */
static int  h_usb_active;
static char h_pending[2048];
static int  h_pending_len;

/* THE SINGLE GATE.  Every function here that opens, removes or creates
 * anything on ms0 asks this first.  Fixing only the one call site that bit us
 * would leave the others waiting; the property we need is "no path touches the
 * card while it is exported", and that has to be enforced in one place. */
static int h_ms0_forbidden(const char *what) __attribute__((unused));
static int h_ms0_forbidden(const char *what)
{
   if (!h_usb_active)
      return 0;
   h_note("BUG: %s attempted while ms0 was exported; refused", what);
   return 1;
}

static void h_flush_notes(void)
{
   int fd;
   if (h_pending_len <= 0 || h_usb_active)
      return;          /* never flush into an exported volume */
   fd = sceIoOpen(h_state, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, h_pending, h_pending_len);
      sceIoClose(fd);
   }
   h_pending_len = 0;
}

/* Raw, synchronous, self-contained writes.  The event log is closed by the
 * time any of this runs, so the handoff keeps its own breadcrumb trail -- and
 * it must, because the first hardware run of this file is the one most likely
 * to fail somewhere in the middle. */
static void h_note(const char *fmt, ...)
{
   char line[192];
   va_list ap;
   int fd, n;
   va_start(ap, fmt);
   n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   line[n] = '\n';
   if (h_usb_active)
   {
      /* Exported to the PC: buffer it, do NOT open a file on ms0. */
      if (h_pending_len + n + 1 < (int)sizeof(h_pending))
      {
         memcpy(h_pending + h_pending_len, line, n + 1);
         h_pending_len += n + 1;
      }
      return;
   }
   fd = sceIoOpen(h_state, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, line, n + 1);
      sceIoClose(fd);
   }
}

static int h_read_small(const char *path, char *buf, int cap)
{
   int fd = sceIoOpen(path, PSP_O_RDONLY, 0777), n = 0;
   if (fd < 0)
      return -1;
   n = sceIoRead(fd, buf, cap - 1);
   sceIoClose(fd);
   if (n < 0)
      n = 0;
   buf[n] = '\0';
   return n;
}

/* Which run of the chain is this?
 *
 * This lives in its OWN file, not in RESULT.TXT.  The counter was originally
 * read back out of RESULT.TXT -- which the PC deletes every cycle as its
 * "this console has been serviced" signal.  So it always read 0, `run` was
 * always 1, and handoff_max_runs never tripped: the chain had no runaway bound
 * at all.  RUNS.TXT is ours alone and the PC never touches it. */
static int h_run_index(void)
{
   char buf[64];
   if (h_read_small(h_runs, buf, sizeof(buf)) <= 0)
      return 0;
   return (int)strtol(buf, NULL, 10);
}

static void h_write_run_index(int run)
{
   char buf[32];
   int fd, n = snprintf(buf, sizeof(buf), "%d\n", run);
   fd = sceIoOpen(h_runs, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, buf, n);
      sceIoClose(fd);
   }
}

static void h_write_result(int run, int exit_code, const char *reason,
                           const char *status)
{
   char line[224];
   int fd, n;
   /* `status` is how the PC tells "just finished a run" (ready) from "sitting
    * here waiting for work" (parked).  Without it a parked console either
    * looks like a completed run and gets counted as one, or -- if it stops
    * publishing -- becomes invisible and the next batch finds nothing. */
   n = snprintf(line, sizeof(line),
                "run=%d\nexit=%d\nreason=%s\nstatus=%s\n",
                run, exit_code, reason ? reason : "ok",
                status ? status : "ready");
   fd = sceIoOpen(h_result, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, line, n);
      sceIoClose(fd);
   }
}

/* Load + start one kernel PRX from flash.  Returns the sce error, or 0.
 *
 * kuKernelLoadModule is the CFW-provided bridge that lets a USER-mode module
 * load a KERNEL one; plain sceKernelLoadModule cannot, which is the whole
 * reason this helper exists. */
static int h_load_start(const char *path)
{
   SceUID id = kuKernelLoadModule(path, 0, NULL);
   int status = 0, rc;
   if (id < 0)
      return (int)id;
   rc = sceKernelStartModule(id, 0, NULL, &status, NULL);
   return rc < 0 ? rc : 0;
}

/* USB mass storage needs the usbstor stack, and the FIRST attempt at this got
 * the module wrong: it called sceUtilityLoadUsbModule(PSP_USB_MODULE_PSPCM).
 * PSPCM is the USB *communication* module -- the utility loader has no
 * mass-storage entry at all (its whole list is PSPCM/ACC/MIC/CAM/GPS).  So
 * USBBusDriver started fine and USBStor_Driver could not, because its driver
 * was never in memory.  On hardware that surfaced as a bare `rc=-3`.
 *
 * These four PRXs are the mass-storage stack, and they are kernel modules, so
 * they go through kuKernelLoadModule.  Errors are RETURNED, not collapsed to a
 * step number: the first version reported only which call failed, which named
 * the symptom and not the cause. */
static const char *const h_usb_prx[] = {
   "flash0:/kd/semawm.prx",
   "flash0:/kd/usbstor.prx",
   "flash0:/kd/usbstormgr.prx",
   "flash0:/kd/usbstorms.prx",
   "flash0:/kd/usbstorboot.prx",
};

static int h_usb_up(void)
{
   unsigned i;
   int rc;

   for (i = 0; i < sizeof(h_usb_prx) / sizeof(h_usb_prx[0]); i++)
   {
      rc = h_load_start(h_usb_prx[i]);
      /* 0x80020139 = already loaded.  The XMB may have brought parts of this
       * stack up already, and that is a success, not a failure. */
      if (rc < 0 && (unsigned)rc != 0x80020139u)
         h_note("  %s -> 0x%08X (continuing)", h_usb_prx[i], (unsigned)rc);
   }

   /* Only THREE calls are actually required: start the bus, start the storage
    * driver, activate.  Treating every step as fatal is what turned an
    * optional one into a hard stop -- sceUsbstorBootSetCapacity lives in the
    * sceUsbstorBoot library, which comes from usbstorboot.prx, and when that
    * PRX declines to load the call returns LIBRARY_IS_NOT_LINKED (0x8002013A)
    * and the whole handoff aborted with rc=-4.  The storage driver was already
    * up by then.  Capacity is a nicety: it sets the size reported to the host,
    * and the default is fine for a memory stick we only read a few files from. */
   rc = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
   if (rc < 0) { h_note("  sceUsbStart(bus) -> 0x%08X", (unsigned)rc); return -2; }
   rc = sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
   if (rc < 0) { h_note("  sceUsbStart(stor) -> 0x%08X", (unsigned)rc); return -3; }

   rc = sceUsbstorBootSetCapacity(STORAGE_CAPACITY);
   if (rc < 0)
      h_note("  setCapacity -> 0x%08X (NOT fatal, continuing)", (unsigned)rc);

   rc = sceUsbActivate(HANDOFF_PID);
   if (rc < 0) { h_note("  sceUsbActivate -> 0x%08X", (unsigned)rc); return -5; }
   h_note("  usb up OK");
   return 0;
}

static void h_usb_down(void)
{
   sceUsbDeactivate(HANDOFF_PID);
   sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
   sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
   /* The usbstor PRXs stay loaded on purpose: they are firmware modules the
    * XMB also uses, and repeatedly unloading/reloading a kernel driver stack
    * across dozens of chained runs is a far better way to wedge a console
    * than leaving four idle modules resident. */
}

void handoff_run(int exit_code, const char *exit_reason)
{
   char cmd[64];
   char eboot[192];
   int  run, waited = 0, rc, total_ms = 0;
   int  last_state = -1;   /* usb state word, logged on change only */
   int  esc_ms = 0;

   if (!h_enabled)
      return;

   snprintf(h_dir,    sizeof(h_dir),    "%s/handoff", g_dir_base);
   snprintf(h_result, sizeof(h_result), "%s/RESULT.TXT", h_dir);
   snprintf(h_cmd,    sizeof(h_cmd),    "%s/CMD.TXT",    h_dir);
   snprintf(h_state,  sizeof(h_state),  "%s/STATE.TXT",  h_dir);
   snprintf(h_runs,   sizeof(h_runs),   "%s/RUNS.TXT",   h_dir);
   sceIoMkdir(h_dir, 0777);

   run = h_run_index() + 1;
   if (h_max_runs > 0 && run > h_max_runs)
   {
      h_note("run %d exceeds max_runs %d -- stopping the chain", run, h_max_runs);
      return;
   }

   /* The PC's cue that a run finished and the volume is about to appear. */
   sceIoRemove(h_cmd);
   h_write_run_index(run);
   h_write_result(run, exit_code, exit_reason, "ready");
   h_note("run=%d exit=%d reason=%s window=%ds", run, exit_code,
          exit_reason ? exit_reason : "ok", h_window_s);

   /* RE-ARM RATHER THAN GIVE UP.
    *
    * The PC deliberately waits for BOTH consoles before servicing either, so
    * whichever finishes first must survive its peer's remaining run time.  A
    * single fixed window meant any run-length asymmetry longer than the window
    * dropped the early console out of the chain: it read no command, fell
    * through to the XMB, and the loop was over.  Run lengths are not equal --
    * a client that takes the graceful-disconnect path finishes minutes apart
    * from a host that exits cleanly.
    *
    * So the window is a POLLING PERIOD, not a deadline.  We re-open it until
    * the total budget is spent.  The flap this causes is confined to the case
    * where the PC has not answered yet, which is exactly when it is harmless:
    * a waiting script simply sees the volume reappear. */
park:
   {
      int cur_window = h_window_s;   /* backs off while nothing arrives */
   for (;;)
   {
      /* ADVERTISE ON EVERY CYCLE.
       *
       * RESULT.TXT is how the PC knows a console is available, and the PC
       * DELETES it as its "serviced" acknowledgement.  A parked console that
       * does not rewrite it is invisible: the operator starts the next batch,
       * the PC waits for a result that will never come, and the console sits
       * there exported and ignored.  That is exactly how the first two-batch
       * test failed -- batch 1 ran, both consoles parked correctly, and batch
       * 2 found nothing to talk to.
       *
       * So re-publish whenever it is missing.  status=parked distinguishes
       * "waiting for work" from "just finished a run", which the PC needs in
       * order not to count a park as a completed run. */
      if (h_read_small(h_result, cmd, sizeof(cmd)) <= 0)
         h_write_result(run, exit_code, exit_reason, "parked");

      rc = h_usb_up();
      if (rc < 0)
      {
         h_usb_down();
         h_usb_active = 0;
         h_flush_notes();
         h_note("usb up FAILED rc=%d -- exiting normally", rc);
         return;
      }
      h_note("usb activated (window %ds, parked %ds)",
             cur_window, total_ms / 1000);
      h_usb_active = 1;   /* from here to h_usb_down(): ms0 is NOT ours */

      /* The USB state is LOGGED, never used as a control signal.
       *
       * It was originally the control signal: watch PSP_USB_CONNECTION_ESTABLISHED
       * (0x002) go high when the PC mounts us and low when it ejects, and cut
       * the window short.  That is not what the bit means.  PSPSDK's name is
       * misleading -- PPSSPP calls the same bit USB_STATUS_STARTED, and it is
       * set as soon as sceUsbStart succeeds, with no PC involved.  The loop
       * would have latched "mounted" on its first poll and then waited for a
       * bit that only clears on sceUsbStop.
       *
       * PSP_USB_CABLE_CONNECTED (0x020) does not rescue it either: that is the
       * physical cable, which never leaves the socket.  Ejecting a volume in
       * Windows unmounts it; it does not unplug anything.
       *
       * So there is no reliable "the PC has finished" signal available here,
       * and the honest design is the one that needs none: serve the window,
       * release the volume, look for the command file, re-arm if it is not
       * there.  Identical on hardware and in the rig, which is the other
       * reason to prefer it. */
      last_state = -1;
      waited     = 0;
      while (waited < cur_window * 1000)
      {
         int st = sceUsbGetState();
         if (st != last_state)
         {
            h_note("usb state 0x%03x at %dms%s%s%s", st, waited,
                   (st & PSP_USB_ACTIVATED)        ? " ACTIVATED"  : "",
                   (st & PSP_USB_CABLE_CONNECTED)  ? " CABLE"      : "",
                   (st & PSP_USB_CONNECTION_ESTABLISHED) ? " STARTED" : "");
            last_state = st;
         }
         if (h_escape_held(&esc_ms))
         {
            h_usb_down();
            h_usb_active = 0;
            h_flush_notes();
            h_note("START+SELECT held -- left the loop by request");
            return;
         }
         /* KEEP THE CONSOLE AWAKE WHILE PARKED.
          *
          * scePowerTick was added to the emulator's main loop (main_psp.c) so
          * a run would not dim or suspend mid-trade -- but it stops the instant
          * the run ends and we park HERE, which is where the console spends the
          * longest continuous stretches of the night (windows back off to 300s
          * and the parked total is longer still).  The backlight going out is
          * only cosmetic, but a real suspend would tear USB down and silently
          * end the unattended chain, and an unattended chain that dies at 2am
          * costs the whole night.  Ticking costs nothing and removes the whole
          * failure mode.  Reported by the user, who noticed the screens dimming
          * during handover and asked whether the fix covered it -- it did not. */
         scePowerTick(PSP_POWER_TICK_SUSPEND);
         scePowerTick(PSP_POWER_TICK_DISPLAY);

         sceKernelDelayThread(POLL_MS * 1000);
         waited   += POLL_MS;
         total_ms += POLL_MS;
      }
      h_note("window served (%ds)", cur_window);

      h_usb_down();
      h_usb_active = 0;
      h_flush_notes();    /* ours again -- commit the buffered breadcrumbs */
      h_note("usb released");

      /* Only now is the filesystem ours again. */
      if (h_read_small(h_cmd, cmd, sizeof(cmd)) > 0)
         break;

      /* No command yet.  Keep the card offered and keep waiting -- this is
       * the parked state, and it is where a console spends its time between
       * batches.  h_park_s == 0 means park indefinitely. */
      if (h_park_s > 0 && total_ms >= h_park_s * 1000)
      {
         h_note("parked %ds with no CMD.TXT -- giving up to the XMB", h_park_s);
         return;
      }
      /* Nothing there.  Back off before re-offering, so a long park does not
       * strobe the host's volume list. */
      if (cur_window < PARK_WINDOW_MAX_S)
      {
         cur_window *= 2;
         if (cur_window > PARK_WINDOW_MAX_S)
            cur_window = PARK_WINDOW_MAX_S;
         h_note("no CMD.TXT -- backing off to a %ds window", cur_window);
      }
   }

   {  /* The command file ends in a newline; trim it so the breadcrumb line
       * does not wrap mid-sentence and become hard to grep. */
      int i;
      for (i = 0; cmd[i]; i++)
         if (cmd[i] == 0x0D || cmd[i] == 0x0A)
         {
            cmd[i] = 0;
            break;
         }
   }
   }   /* end park scope */

   if (strncmp(cmd, "EXIT", 4) == 0)
   {
      h_note("CMD=EXIT -- leaving the loop to the XMB as instructed");
      return;
   }
   if (strncmp(cmd, "RUN", 3) != 0)
   {
      /* STOP ends the BATCH, not the console's availability.  Clear it and go
       * back to parking so the operator can start the next batch without
       * touching the hardware.  Exiting here is what used to strand the
       * console at the XMB, unreachable. */
      h_note("CMD=%.8s -- batch ended; parking for the next one", cmd);
      sceIoRemove(h_cmd);
      goto park;
   }

   snprintf(eboot, sizeof(eboot), "%s/EBOOT.PBP", g_dir_base);
   h_note("CMD=RUN -- relaunching %s (run %d)", eboot, run + 1);

   /* RELAUNCH VIA THE CFW, NOT VIA sceKernelLoadExec.
    *
    * Plain sceKernelLoadExec returned instead of replacing us -- every step of
    * the handoff worked on hardware and then died on this one call.  It is the
    * wrong API here: on CFW an EBOOT.PBP on the memory stick is launched the
    * way the FIRMWARE launches it, through LoadExecVSH with an apitype, and
    * sctrlKernelLoadExecVSHMs2 is documented as exactly "the function used by
    * the firmware to execute games (and homebrew) from a memory stick".  It is
    * exported to USER mode by pspsystemctrl_user, so no kernel build needed.
    *
    * param matters: `args`/`argp` become the new process's argv[0], which is
    * where g_dir_base comes from.  Passing NULL would relaunch us with no path
    * and every ms0 path would silently fall back to the compiled-in default. */
   {
      struct SceKernelLoadExecVSHParam param;
      memset(&param, 0, sizeof(param));
      param.size = sizeof(param);
      param.args = (SceSize)(strlen(eboot) + 1);
      param.argp = eboot;
      param.key  = "game";
      sctrlKernelLoadExecVSHMs2(eboot, &param);

      /* Still here?  Then that failed too.  Try the plain call as a fallback
       * so a console on a firmware where systemctrl is absent still has a
       * chance, rather than silently doing nothing. */
      h_note("sctrlKernelLoadExecVSHMs2 RETURNED -- trying sceKernelLoadExec");
      sceKernelLoadExec(eboot, NULL);
   }

   /* Only reached if BOTH failed; falling through exits to the XMB. */
   h_note("relaunch failed by both paths");
}
