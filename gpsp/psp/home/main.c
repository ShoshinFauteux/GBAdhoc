/* gbadhoc_home.prx -- HOME as the in-game menu button (Settings > Controls >
 * Menu = HOME).  A kernel module because only kernel code can see HOME in
 * the pad, and only sceImpose's kernel export can switch the system's HOME
 * popup off.  docs/CONTROL-REMAP.md section 10; the contract is home_link.h.
 *
 * WHAT IT DOES.  One kernel thread polls the pad every POLL_US.  While the
 * EBOOT says a game owns HOME (home_link.armed == HOME_ARMED) AND the EBOOT's
 * main loop is still beating, the system popup is off and every HOME press is
 * counted into home_link.home_seq for the EBOOT to act on.  The moment either
 * stops being true -- the browser, the wake overlay, a loading screen, a
 * frozen loop -- the popup is switched back on and HOME is the system's again.
 *
 * THE MECHANISM, AND WHY THIS ONE (6.60/6.61, PRO and ARK-4).
 *   - Reading HOME: sceCtrlPeekBufferPositive from a KERNEL thread (k1 = 0).
 *     The firmware strips HOME/NOTE/SCREEN/VOL from user-mode reads only; a
 *     kernel caller gets the raw bits.  Imported from sceCtrl_driver under its
 *     pre-6.xx NID; 6.xx randomised the kernel NIDs and PRO's/ARK's NID
 *     resolver maps it (PRO SystemControl nid_660_data.c: sceCtrl_driver
 *     0x3A622550 -> 0x2BA616AF).  The same resolver already carries this
 *     project's other kernel imports (sceSysreg_driver, SysEventForKernel)
 *     on every console the Media Engine runs on.
 *   - Suppressing the popup: sceImposeSetHomePopup(0) / (1), sceImpose_driver
 *     (resolver: 0x5595A71A -> 0xC08C41EF on 6.60).  This is the switch the
 *     impose module itself consults before drawing "Quit the game?"; it is
 *     what TempGBA4PSP (a GBA emulator, also on 6.xx CFW, also with a kernel
 *     helper PRX for the pad) uses for exactly this feature, and what
 *     TempAR and PSPdisp use to take HOME over.  The alternatives were
 *     worse: masking with sceCtrlSetButtonIntercept only hides buttons from
 *     USER readers -- the popup is driven from a kernel callback and still
 *     fires -- and hooking impose's ctrl callback means patching firmware
 *     code per version.
 *   - Not a sysevent handler, not an export, not a hook.  Nothing in the
 *     firmware is modified; the only state touched is the popup flag, and
 *     LoadExec (every exit path, crash included) re-initialises it.
 *
 * NOTHING HERE IS SHARED WITH THE MEDIA ENGINE MODULE.  gbadhoc_me.prx is
 * byte-for-byte what it was; this module has its own lifetime (loaded when a
 * game starts in HOME mode, unloaded on teardown), so GB/GBC and me_boot = 0
 * get HOME too, and the standby ladder's ME unload cannot take HOME with it. */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspimpose_driver.h>

#include "home_link.h"

PSP_MODULE_INFO("gbadhoc_home", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

/* 16 ms: a HOME press lasts far longer than one poll, and the cost is one
 * pad peek (a buffer copy, no I/O) sixty times a second. */
#define POLL_US      16000
/* Above the EBOOT's emulation thread (0x20-0x2B) and its audio/callback
 * threads, so a main loop spinning at full speed cannot starve the fallback:
 * the stall check must run precisely when the EBOOT is NOT yielding. */
#define THREAD_PRIO  0x10
/* After a stall, the loop must beat steadily this long before HOME is taken
 * back from the system. */
#define REARM_US     1000000u
#define USER_LO      0x08800000u
#define USER_HI      0x0A000000u

static home_link *g_l;
static SceUID g_thid = -1;
static volatile int g_stop;
static int g_off;            /* we have switched the popup off */

static unsigned now_us(void)
{
   return (unsigned)sceKernelGetSystemTimeLow();
}

static int popup_set(int on)
{
   unsigned k1 = pspSdkSetK1(0);
   int rc = sceImposeSetHomePopup(on ? 1 : 0);
   pspSdkSetK1(k1);
   return rc;
}

static int home_thread(SceSize args, void *argp)
{
   home_link *l = g_l;
   SceCtrlData pad;
   unsigned prev = 0, last_beat, t_beat, t_resume;
   int refused = 0;          /* SetHomePopup(0) failed: stop asking */
   (void)args; (void)argp;

   last_beat = l->beat;
   t_beat = now_us();
   t_resume = t_beat - REARM_US;      /* settled from the start */
   l->version = HOME_LINK_VERSION;

   while (!g_stop)
   {
      unsigned t = now_us(), b = l->beat, stall_us, buttons = 0;
      int healthy, want;

      {
         unsigned k1 = pspSdkSetK1(0);
         if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
            buttons = pad.Buttons;
         pspSdkSetK1(k1);
      }
      stall_us = (l->stall_ms ? l->stall_ms : HOME_STALL_MS_DEFAULT) * 1000u;
      if (b != last_beat)
      {
         /* The loop came back from a stall: it has to beat steadily for
          * REARM_US before HOME is taken from the system again, so a loop
          * that limps (beats, stalls, beats) leaves HOME with the system
          * rather than flapping the popup under the player's thumb. */
         if ((unsigned)(t - t_beat) >= stall_us)
            t_resume = t;
         last_beat = b;
         t_beat = t;
      }
      healthy = (unsigned)(t - t_beat) < stall_us &&
                (unsigned)(t - t_resume) >= REARM_US;
      want = l->armed == HOME_ARMED && healthy && !refused;
      /* Never take HOME over in the middle of a press: that press began as
       * the system's, so it stays the system's. */
      if (want && !g_off && (buttons & PSP_CTRL_HOME))
         want = 0;

      if (want != g_off)
      {
         int rc = popup_set(!want);
         l->imp_rc = rc;
         if (!want)
         {
            /* Turning it back ON is never retried-and-held: whatever the
             * call said, we stop believing we own the popup. */
            if (l->armed == HOME_ARMED && (unsigned)(t - t_beat) >= stall_us)
               l->fallbacks = l->fallbacks + 1;
            g_off = 0;
         }
         else if (rc >= 0)
            g_off = 1;
         else
         {
            refused = 1;          /* the EBOOT reads `fail` and falls back */
            l->fail = 1;
         }
      }

      /* Count a press only while the popup is OURS: with it on, HOME is the
       * system's exit dialog and must not also open our menu behind it. */
      if (g_off && (buttons & PSP_CTRL_HOME) && !(prev & PSP_CTRL_HOME))
      {
         l->home_us = t;
         l->home_seq = l->home_seq + 1;
      }
      prev = buttons;
      l->popup_off = (unsigned)g_off;
      l->kbeat = l->kbeat + 1;
      sceKernelDelayThread(POLL_US);
   }

   if (g_off)
   {
      l->imp_rc = popup_set(1);
      g_off = 0;
      l->popup_off = 0;
   }
   return 0;
}

/* argp: u32[0] = the EBOOT's home_link (cached user address). */
int module_start(SceSize args, void *argp)
{
   unsigned p;
   home_link *l;
   if (args < 4 || !argp)
      return -1;
   p = *(unsigned *)argp;
   if ((p & 63u) || (p & 0x1FFFFFFFu) < USER_LO ||
       (p & 0x1FFFFFFFu) + sizeof(home_link) > USER_HI)
      return -2;
   l = (home_link *)p;
   if (l->magic != HOME_LINK_MAGIC)
      return -3;
   g_l = l;
   g_stop = 0;
   g_off = 0;
   {
      unsigned k1 = pspSdkSetK1(0);
      l->popup_orig = sceImposeGetHomePopup();
      pspSdkSetK1(k1);
   }
   g_thid = sceKernelCreateThread("gbadhoc_home", home_thread, THREAD_PRIO,
                                  0x1000, 0, NULL);
   if (g_thid < 0)
      return g_thid;
   if (sceKernelStartThread(g_thid, 0, NULL) < 0)
   {
      sceKernelDeleteThread(g_thid);
      g_thid = -1;
      return -4;
   }
   return 0;
}

int module_stop(SceSize args, void *argp)
{
   (void)args; (void)argp;
   g_stop = 1;
   if (g_thid >= 0)
   {
      SceUInt timeout = 500000;   /* the thread wakes every POLL_US */
      if (sceKernelWaitThreadEnd(g_thid, &timeout) < 0)
         sceKernelTerminateThread(g_thid);
      sceKernelDeleteThread(g_thid);
      g_thid = -1;
   }
   /* Whatever the thread managed, the popup leaves ON.  Unconditional: a
    * second "on" is harmless, a missing one is the bug this module must
    * never have. */
   {
      int rc = popup_set(1);
      if (g_l)
      {
         g_l->imp_rc = rc;
         g_l->popup_off = 0;
      }
   }
   g_off = 0;
   g_l = NULL;
   return 0;
}
