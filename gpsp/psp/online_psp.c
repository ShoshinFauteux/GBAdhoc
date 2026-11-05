/* online_psp.c -- EXPERIMENTAL online play, the EBOOT's half.  See
 * online_psp.h and docs/ONLINE-TUNNEL.md section 8.
 *
 * The session code lives in gbadhoc_online.prx (psp/online/), loaded only
 * when a session goes online and unloaded when it ends; online_link.h says
 * why (boot heap) and how (two tables, a magic and a version).  What stays
 * here is small on purpose: the configuration, the loader, the host table
 * (memory, formatting, the event log, mgift_net.c's Wi-Fi) and forwarders.
 *
 * Failure is graceful: a module that is missing, does not fit in memory or
 * is another version is unloaded, the reason goes to online_psp_why() for
 * the Online page, and Wi-Fi has not been touched yet.
 */
#ifdef GBADHOC_ONLINE

#include "online_psp.h"
#include "online/online_link.h"
#include "mgift_net.h"
#include "../netdrv/relay_proto.h"
#include "../frontend-common/fe_evt.h"
#include "../frontend-common/fe_util.h"

#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Lab validates harness keys by grepping this exact call form
 * (backend/lab/inikeys.py).  online_psp_configure() reads each of them from
 * the harness ini first, then config.ini:
 *   fe_ini_get(HARNESS_INI, "online_relay")
 *   fe_ini_get(HARNESS_INI, "online_room")
 *   fe_ini_get_int(HARNESS_INI, "online_direct")
 *   fe_ini_get_int(HARNESS_INI, "online_wifi")
 *   fe_ini_get_int(HARNESS_INI, "online_drop_direct_ms")
 *   fe_ini_get_int(HARNESS_INI, "online_prx_cycles")
 */

#ifdef GPSP_PERF_RIG
void gpsp_net_wait_beat(void);
#define HOST_BEAT gpsp_net_wait_beat
#else
#define HOST_BEAT NULL
#endif

extern char g_dir_base[128];

static struct
{
   int enabled;
   online_cfg cfg;
   char why[64];
   SceUID mod;
   online_api api;
   int loaded;                  /* module started and api checked */
} O = { .mod = -1, .cfg = { .direct = 1, .wifi = 1 } };

static char g_cfg_relay[64];    /* config.ini online_relay, for menu sessions */

/* ---- the host table ------------------------------------------------------ */
static void h_evt(const char *line) { fe_evt("%s", line); FE_EVT_ONLY(line); }
static int h_vsnprintf(char *o, size_t c, const char *f, va_list ap) { return vsnprintf(o, c, f, ap); }
static void h_free(void *p) { free(p); }
/* Not const, and the texts below are char arrays, on purpose: .rodata is
 * the section whose growth moves the pinned .data a whole 8 KiB step
 * (docs/LAYOUT-PINNING.md), .data has room before its next boundary.
 * Online's few EBOOT strings live there so the image stays an ONLINE=0
 * build's (boot heap equal).  ui_psp.c and main_psp.c use them too. */
static online_host g_host = {
   OL_MAGIC_HOST, OL_VERSION, sizeof(online_host),
   calloc, h_free, h_vsnprintf, h_evt,
   mgnet_wifi_is_up, mgnet_local_ip,
   HOST_BEAT
};
static char s_prx_fmt[] = "%s/gbadhoc_online.prx";
static char s_missing[] = "online module missing";
static char s_nomem[] = "online module: no memory";
static char s_version[] = "online module: wrong version";
static char s_wifi_fmt[] = "Wi-Fi: %s";
static char s_relay_key[] = "online_relay";
static char s_relay_default[] = ONLINE_RELAY_DEFAULT;

/* ---- the module ---------------------------------------------------------- */
static void mem_evt(const char *at, unsigned ms)
{
   fe_evt("online_prx at=%s ms=%u free=%d max_block=%d", at, ms,
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
   FE_EVT_ONLY(at); FE_EVT_ONLY(ms);
}

/* module_start/module_stop thread stack.  The firmware default is 256 KiB, but
 * with Wi-Fi up on hardware only ~140 KiB is free (largest block ~80 KiB): a
 * NULL option failed with 0x80020190 NO_MEMORY on the 3000 (PPSSPP had room).
 * Both entry points only check the ABI / call down(), on a small stack. */
static SceKernelSMOption *sm_opt(void)
{
   static SceKernelSMOption so;
   memset(&so, 0, sizeof(so));
   so.size = sizeof(so);
   so.mpidstack = 2;                  /* user partition */
   so.stacksize = 16 * 1024;
   so.priority = 0x20;
   return &so;
}

static int prx_load(void)
{
   char path[192];
   online_link link;
   void *arg[1];
   int st = 0, rc;
   unsigned t0 = sceKernelGetSystemTimeLow();
   if (O.loaded)
      return 0;
   mem_evt("load_begin", 0);
   snprintf(path, sizeof(path), s_prx_fmt, g_dir_base);
   O.mod = sceKernelLoadModule(path, 0, NULL);
   if (O.mod < 0)
   {
      unsigned e = (unsigned)O.mod;
      snprintf(O.why, sizeof(O.why), "%s",
               e == 0x80010002u || e == 0x8002012Fu ? s_missing : s_nomem);
      fe_evt("online_prx at=load_failed rc=0x%08X", e);
      O.mod = -1;
      return -1;
   }
   memset(&O.api, 0, sizeof(O.api));
   link.host = &g_host;
   link.api = &O.api;
   arg[0] = &link;
   rc = sceKernelStartModule(O.mod, sizeof(arg), arg, &st, sm_opt());
   if (rc < 0 || st < 0 || O.api.magic != OL_MAGIC_API ||
       O.api.version != OL_VERSION || O.api.size != sizeof(online_api))
   {
      snprintf(O.why, sizeof(O.why), "%s", rc < 0 ? s_nomem : s_version);
      fe_evt("online_prx at=start_failed rc=0x%08X st=%d api=%08X/%u", (unsigned)rc, st,
             (unsigned)O.api.magic, (unsigned)O.api.version);
      if (rc >= 0 && st >= 0)
         sceKernelStopModule(O.mod, 0, NULL, &st, sm_opt());
      sceKernelUnloadModule(O.mod);
      O.mod = -1;
      return -1;
   }
   O.loaded = 1;
   mem_evt("loaded", ((unsigned)sceKernelGetSystemTimeLow() - t0) / 1000u);
   return 0;
}

static void prx_unload(void)
{
   int st = 0;
   unsigned t0 = sceKernelGetSystemTimeLow();
   if (O.mod < 0)
      return;
   if (O.loaded)
      sceKernelStopModule(O.mod, 0, NULL, &st, sm_opt());   /* module_stop: down() */
   sceKernelUnloadModule(O.mod);
   O.mod = -1;
   O.loaded = 0;
   memset(&O.api, 0, sizeof(O.api));
   mem_evt("unloaded", ((unsigned)sceKernelGetSystemTimeLow() - t0) / 1000u);
}

/* ---- configuration --------------------------------------------------------- */
#ifdef GPSP_PERF_RIG
static int get_str(const char *h, const char *c, const char *k, char *o, size_t n)
{
   return fe_ini_get(h, k, o, n) || (c && fe_ini_get(c, k, o, n));
}

static long get_int(const char *h, const char *c, const char *k, long def)
{
   char b[16];
   if (fe_ini_get(h, k, b, sizeof(b)))
      return fe_ini_get_int(h, k, def);
   return c ? fe_ini_get_int(c, k, def) : def;
}
#endif

/* the text as given; the module keeps only its first four letters, upper-cased */
static void set_room(const char *s)
{
   snprintf(O.cfg.room, sizeof(O.cfg.room), "%s", s ? s : "");
}

#ifdef GPSP_PERF_RIG
static void cycle_test(int n);
#endif

int online_psp_configure(const char *h, const char *c)
{
#ifdef GPSP_PERF_RIG
   char s[64] = "";
   int cycles;
#endif
   O.enabled = 0;
   g_cfg_relay[0] = 0;
   if (c)
      fe_ini_get(c, s_relay_key, g_cfg_relay, sizeof(g_cfg_relay));
#ifndef GPSP_PERF_RIG
   (void)h;
   return 0;      /* a player's build: online only per session, from the menu */
#else
   cycles = (int)fe_ini_get_int(h, "online_prx_cycles", 0);
   /* every-session online: only the harness ini turns it on (a player turns
    * it on per session from the menu) */
   if (!fe_ini_get(h, "online_relay", s, sizeof(s)) || !s[0])
   {
      if (cycles > 0)
         cycle_test(cycles);
      return 0;
   }
   snprintf(O.cfg.relay, sizeof(O.cfg.relay), "%s", s);
   O.cfg.room[0] = 0;
   if (get_str(h, c, "online_room", s, sizeof(s)))
      set_room(s);
   O.cfg.direct = (int)get_int(h, c, "online_direct", 1) ? 1 : 0;
   O.cfg.wifi = (int)get_int(h, c, "online_wifi", 1);
   O.cfg.drop_direct_ms = (unsigned)get_int(h, c, "online_drop_direct_ms", 0);
   O.cfg.mode = RL_MODE_ANY;
   O.enabled = 1;
   fe_evt("online_config relay=%s room=%s direct=%d wifi=%d drop_direct_ms=%u",
          O.cfg.relay, O.cfg.room[0] ? O.cfg.room : "-", O.cfg.direct, O.cfg.wifi,
          O.cfg.drop_direct_ms);
   if (cycles > 0)
      cycle_test(cycles);
   return 1;
#endif
}

void online_psp_set(const char *relay, const char *room, int wifi, int mode)
{
   snprintf(O.cfg.relay, sizeof(O.cfg.relay), "%s",
            relay && relay[0] ? relay : g_cfg_relay[0] ? g_cfg_relay
                                                      : s_relay_default);
   set_room(room);
   O.cfg.wifi = wifi > 0 ? wifi : 1;
   O.cfg.mode = mode;
   O.cfg.direct = 1;
   O.cfg.drop_direct_ms = 0;
   O.enabled = 1;
   fe_evt("online_set relay=%s room=%s wifi=%d mode=%d", O.cfg.relay,
          O.cfg.room[0] ? O.cfg.room : "-", O.cfg.wifi, O.cfg.mode);
}

void online_psp_clear(void)
{
   if (!online_psp_is_up())
      O.enabled = 0;
}

int online_psp_enabled(void) { return O.enabled; }
const char *online_psp_why(void) { return O.why; }
int online_psp_wifi_count(void) { return mgnet_config_count(); }
int online_psp_wifi_name(int i, char *o, unsigned c) { return mgnet_config_name(i, o, c); }

/* ---- the session ----------------------------------------------------------- */
#ifdef GPSP_PERF_RIG
static void list_wifi(void)
{
   int n = mgnet_config_count(), i;
   char nm[64];
   fe_evt("online_wifi profiles=%d chosen=%d", n, O.cfg.wifi);
   for (i = 1; i <= n && i <= 10; i++)
      if (mgnet_config_name(i, nm, sizeof(nm)) == 0)
         fe_evt("online_wifi profile=%d name=\"%s\"", i, nm);
}
#else
#define list_wifi() ((void)0)
#endif

/* Wi-Fi first (the net modules the module imports from), then the module,
 * then its session; every failure undoes exactly what was done. */
int online_psp_up(int is_host, const char *build, unsigned timeout_ms)
{
   int rc;
   O.why[0] = 0;
   if (online_psp_is_up())
      return 0;
   rc = mgnet_wifi_up(O.cfg.wifi, 30000000u);
   if (rc != MGNET_OK)
   {
      snprintf(O.why, sizeof(O.why), s_wifi_fmt, mgnet_stage());
      fe_evt("online_error stage=wifi rc=%d at=%s sce=0x%08X", rc, mgnet_stage(),
             mgnet_last_sce());
      list_wifi();
      return -1;
   }
   list_wifi();
   fe_evt("online_wifi_up ip=%s", mgnet_local_ip());
   if (prx_load() != 0)
   {
      mgnet_wifi_down();                  /* why[] says why */
      return -1;
   }
   if (O.api.up(&O.cfg, is_host, build, timeout_ms, O.why, sizeof(O.why)) != 0)
   {
      prx_unload();                       /* the module took its part down */
      mgnet_wifi_down();
      return -1;
   }
   /* 1: a joiner that was told slot 0 (the first PSP into a desktop host's
    * room) hosts the session.  A return value, not an accessor: ONLINE's
    * EBOOT text has almost no room left (ui_psp.c, screen_online). */
   return !is_host && O.api.slot() == 0;
}

int online_psp_is_up(void) { return O.loaded && O.api.is_up(); }
const char *online_psp_room(void) { return O.loaded ? O.api.room() : ""; }
int online_psp_room_mode(void) { return O.loaded ? O.api.room_mode() : 0; }
void online_psp_iface(nd_transport *out) { if (O.loaded) O.api.iface(out); }
int online_psp_connected(void) { return O.loaded && O.api.connected(); }

void online_psp_down(void)
{
   if (!O.loaded)
      return;
   O.api.down();
   prx_unload();
   mgnet_wifi_down();
}

void online_psp_stats_evt(const char *at)
{
   if (O.loaded)
      O.api.stats_evt(at);
}

#ifdef GPSP_PERF_RIG
/* Harness: online_prx_cycles = N loads, starts, stops and unloads the module
 * N times (with online_relay set, each cycle also creates a room and leaves
 * it, or tries a join), logging the partition's free memory at every step:
 * the leak test.  Never in a player's run (the key is the harness ini's). */
static void cycle_test(int n)
{
   int i, f0 = sceKernelTotalFreeMemSize(), m0 = sceKernelMaxFreeMemSize();
   fe_evt("online_prx_cycles n=%d free=%d max_block=%d", n, f0, m0);
   FE_EVT_ONLY(m0);
   FE_EVT_ONLY(f0);
   for (i = 0; i < n; i++)
   {
      int rc = 0;
      if (O.enabled)
      {
         char room[8];
         snprintf(room, sizeof(room), "CY%c%c", 'A' + (i / 26) % 26, 'A' + i % 26);
         if (i % 2 == 0)
            set_room(room);
         rc = online_psp_up(i % 2 == 0, "cycle-test", i % 2 == 0 ? 8000u : 3000u);
         online_psp_down();
      }
      else if ((rc = prx_load()) == 0)
         prx_unload();                    /* (no Wi-Fi: imports unresolved, never called) */
      fe_evt("online_prx_cycle i=%d rc=%d free=%d delta=%d max_block=%d", i, rc,
             sceKernelTotalFreeMemSize(), sceKernelTotalFreeMemSize() - f0,
             sceKernelMaxFreeMemSize());
      FE_EVT_ONLY(rc);
   }
   fe_evt("online_prx_cycles done n=%d free=%d delta=%d", n, sceKernelTotalFreeMemSize(),
          sceKernelTotalFreeMemSize() - f0);
   O.why[0] = 0;
}
#endif

#endif /* GBADHOC_ONLINE */
