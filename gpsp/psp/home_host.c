/* home_host.c -- EBOOT side of Menu = HOME (see home_host.h,
 * docs/CONTROL-REMAP.md section 10). */
#include <pspkernel.h>
#include <kubridge.h>

#include <stdio.h>
#include <string.h>

#include "home_host.h"
#include "home/home_link.h"
#include "fe_evt.h"

/* Process-lifetime, so the module can never outlive what it points at. */
static home_link __attribute__((aligned(64))) g_link;
static SceUID g_mod = -1;
static int    g_tried;
static int    g_armed;
static unsigned g_seen_seq;
static unsigned g_last_kbeat, g_kbeat_us;
static int    g_said_fail;

/* A press older than this when the loop sees it is dropped (see header). */
#define HOME_FRESH_US 250000u
/* The module polls every 16 ms; this long without a poll means it is gone. */
#define HOME_DEAD_US  500000u

static unsigned now_us(void)
{
   return (unsigned)sceKernelGetSystemTimeLow();
}

int home_host_tried(void)
{
   return g_tried;
}

int home_host_start(const char *base_dir)
{
   char path[192];
   unsigned arg[1];
   SceUID mod;
   int st;

   if (g_tried)
      return g_mod >= 0 ? 0 : -1;
   g_tried = 1;

   memset(&g_link, 0, sizeof(g_link));
   g_link.magic = HOME_LINK_MAGIC;
   g_link.stall_ms = HOME_STALL_MS_DEFAULT;

   snprintf(path, sizeof(path), "%s/gbadhoc_home.prx", base_dir);
   mod = kuKernelLoadModule(path, 0, NULL);
   if (mod < 0)
   {
      fe_evt("home_service state=unavailable reason=load rc=0x%08X",
             (unsigned)mod);
      return -1;
   }
   arg[0] = (unsigned)&g_link;
   if (sceKernelStartModule(mod, sizeof(arg), arg, &st, NULL) < 0 || st < 0)
   {
      fe_evt("home_service state=unavailable reason=start rc=%d", st);
      sceKernelUnloadModule(mod);
      return -1;
   }
   g_mod = mod;
   g_seen_seq = g_link.home_seq;
   g_last_kbeat = g_link.kbeat;
   g_kbeat_us = now_us();
   fe_evt("home_service state=up popup_orig=%d", g_link.popup_orig);
   return 0;
}

void home_host_stop(void)
{
   g_link.armed = 0;
   g_armed = 0;
   if (g_mod >= 0)
   {
      int st;
      sceKernelStopModule(g_mod, 0, NULL, &st, NULL);
      sceKernelUnloadModule(g_mod);
      g_mod = -1;
      fe_evt("home_service state=stopped fallbacks=%u imp_rc=%d",
             g_link.fallbacks, g_link.imp_rc);
   }
}

void home_host_frame(int armed)
{
   g_link.beat = g_link.beat + 1;
   armed = armed && g_mod >= 0;
   if (armed != g_armed)
   {
      g_armed = armed;
      /* Disarming drops whatever the module counted meanwhile. */
      if (!armed)
         g_seen_seq = g_link.home_seq;
   }
   g_link.armed = armed ? HOME_ARMED : 0;
}

void home_host_disarm(void)
{
   g_link.armed = 0;
   g_armed = 0;
   g_seen_seq = g_link.home_seq;
}

int home_host_ok(void)
{
   unsigned kb, t;
   if (g_mod < 0 || g_link.version != HOME_LINK_VERSION)
      return 0;
   if (g_link.fail)
   {
      if (!g_said_fail)
      {
         g_said_fail = 1;
         fe_evt("home_service state=refused imp_rc=%d", g_link.imp_rc);
      }
      return 0;
   }
   kb = g_link.kbeat;
   t = now_us();
   if (kb != g_last_kbeat)
   {
      g_last_kbeat = kb;
      g_kbeat_us = t;
   }
   return (unsigned)(t - g_kbeat_us) < HOME_DEAD_US;
}

int home_host_take_press(void)
{
   unsigned seq = g_link.home_seq;
   if (seq == g_seen_seq)
      return 0;
   g_seen_seq = seq;
   if (!g_armed)
      return 0;
   return (unsigned)(now_us() - g_link.home_us) < HOME_FRESH_US;
}

unsigned home_host_fallbacks(void)
{
   return g_link.fallbacks;
}

int home_host_imp_rc(void)
{
   return g_link.imp_rc;
}
