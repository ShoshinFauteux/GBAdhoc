/* Exercise the real profile selection/creation with SDK configuration stubs. */
#include <assert.h>
#include <stdarg.h>
#include "../psp/mgift_net.c"
#include "../psp/mgift_shortcut.h"

/* The 3.0 shortcut, verbatim, for the remap equivalence check below. */
static int legacy_mgift(unsigned pad, int enabled, unsigned *consumed)
{
   const unsigned chord = PSP_CTRL_SELECT | PSP_CTRL_DOWN;
   const unsigned conflicts = PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_LEFT |
      PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE |
      PSP_CTRL_SQUARE | PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;
   if (!enabled) { *consumed = 0; return 0; }
   if (*consumed)
   {
      if (!(pad & chord)) *consumed = 0;
      return 0;
   }
   if ((pad & chord) == chord && !(pad & conflicts))
   {
      *consumed = chord;
      return 1;
   }
   return 0;
}

static struct { int exists; netData values[17]; } profiles[11];
static int created, deleted, fail_param = -1, fail_copy, corrupt_copy;
static int require_ready, apctl_ready, connected_slot;
int sceWlanGetSwitchState(void) { return 1; }
/* need_common: GetNetParam fails until PSP_NET_MODULE_COMMON is loaded (the
 * firmware behaviour the picker must survive if it is real).  load_rc forces
 * a load result, e.g. "already loaded". */
static int need_common, common_loaded, loads, unloads, load_rc;
int sceUtilityLoadNetModule(int module)
{
   if (module == PSP_NET_MODULE_COMMON) { loads++; if (load_rc) return load_rc; common_loaded = 1; }
   return 0;
}
int sceUtilityUnloadNetModule(int module)
{
   if (module == PSP_NET_MODULE_COMMON) { unloads++; assert(common_loaded); common_loaded = 0; }
   return 0;
}
int sceNetInit(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e;return 0; }
int sceNetTerm(void) { return 0; }
int sceNetInetInit(void) { return 0; }
int sceNetInetTerm(void) { return 0; }
int sceNetApctlInit(int a,int b) { (void)a;(void)b;apctl_ready=1;return 0; }
int sceNetApctlTerm(void) { apctl_ready=0;return 0; }
int sceNetApctlConnect(int slot) { assert(apctl_ready);if(!profiles[slot].exists)return (int)0x80410A0B;connected_slot=slot;return 0; }
int sceNetApctlDisconnect(void) { connected_slot=0;return 0; }
int sceNetApctlGetState(int *s) { *s=connected_slot?PSP_NET_APCTL_STATE_GOT_IP:PSP_NET_APCTL_STATE_DISCONNECTED;return 0; }
int sceNetApctlGetInfo(int code,union SceNetApctlInfo *info) { (void)code;strcpy(info->ip,"192.168.1.20");return 0; }
unsigned sceKernelGetSystemTimeLow(void) { return 100; }
int sceKernelDelayThread(SceUInt delay) { (void)delay;return 0; }
void fe_evt(const char *fmt, ...) { (void)fmt; }
int sceUtilityCheckNetParam(int id) { return profiles[id].exists ? 0 : -1; }
int sceUtilityGetNetParam(int id, int param, netData *out)
{
   assert(!require_ready || apctl_ready);
   if (need_common && !common_loaded) return (int)0x80110601;
   if (!profiles[id].exists) return -1;
   *out = profiles[id].values[param];
   return 0;
}
int sceUtilityCreateNetParam(int id)
{
   assert(id > 0 && id <= 10);
   assert(!profiles[id].exists); /* regression: old Automatic recreates it */
   memset(&profiles[0], 0, sizeof(profiles[0]));
   profiles[0].exists = profiles[id].exists = 1;
   created++;
   return 0;
}
int sceUtilitySetNetParam(int param, const void *value)
{
   if (param == fail_param) return -77;
   if (param == PSP_NETPARAM_NAME || param == PSP_NETPARAM_SSID)
      strcpy(profiles[0].values[param].asString, value);
   else profiles[0].values[param].asUint = *(const unsigned *)value;
   return 0;
}
int sceUtilityCopyNetParam(int src, int dst)
{
   assert(src == 0 && dst > 0);
   if (fail_copy) return -78;
   profiles[dst] = profiles[src];
   if (corrupt_copy) profiles[dst].values[PSP_NETPARAM_SECURE].asUint = 1;
   return 0;
}
int sceUtilityDeleteNetParam(int id)
{
   assert(id > 0); memset(&profiles[id], 0, sizeof(profiles[id])); deleted++; return 0;
}
static void add(int id, const char *name, const char *ssid, unsigned secure)
{
   profiles[id].exists = 1;
   strcpy(profiles[id].values[PSP_NETPARAM_NAME].asString, name);
   strcpy(profiles[id].values[PSP_NETPARAM_SSID].asString, ssid);
   profiles[id].values[PSP_NETPARAM_SECURE].asUint = secure;
}
static void reset(void)
{
   memset(profiles, 0, sizeof(profiles));
   Cprofile_slot = created = deleted = fail_copy = corrupt_copy = 0;
   need_common = common_loaded = loads = unloads = load_rc = 0;
   fail_param = -1;
}
int main(void)
{
   int i;
   unsigned consumed = 0, chord = PSP_CTRL_SELECT | PSP_CTRL_DOWN;
   reset(); require_ready=1;add(1,"Home","Home",2);
   assert(mgnet_wifi_up(0,0)==MGNET_OK && connected_slot==2);
   mgnet_wifi_down();assert(!apctl_ready && !connected_slot);
   assert(mgnet_wifi_up(0,0)==MGNET_OK && connected_slot==2 && created==1);
   mgnet_wifi_down();
   /* Manual override preserves its chosen index rather than invoking Auto. */
   assert(mgnet_wifi_up(1,0)==MGNET_OK && connected_slot==1 && created==1);
   mgnet_wifi_down();require_ready=0;
   reset(); add(1, "Home", "Home", 2);
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") == 2);
   assert(created == 1 && profile_matches_open(2, MGNET_AUTO_SSID));
   assert(!strcmp(profiles[1].values[PSP_NETPARAM_SSID].asString, "Home"));
   /* Repeat start, including after relaunch: no writes to an existing match. */
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") == 2);
   mgnet_profile_release(); assert(created == 1 && deleted == 0 && profiles[2].exists);
   reset(); add(4, "My phone", MGNET_AUTO_SSID, 0);
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") == 4 && !created);
   mgnet_profile_release(); assert(profiles[4].exists && !deleted);
   reset(); add(1, MGNET_PROFILE_NAME, "Old phone", 0);
   add(2, "Secured", MGNET_AUTO_SSID, 2);
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") == 3);
   assert(!strcmp(profiles[1].values[PSP_NETPARAM_SSID].asString, "Old phone"));
   assert(profiles[2].values[PSP_NETPARAM_SECURE].asUint == 2);
   mgnet_profile_release(); assert(!profiles[3].exists && deleted == 1);
   for (i = 0; i < 17; i++) {
      if (i != 0 && i != 1 && i != 2 && i != 4 && i != 8 && i != 13) continue;
      reset(); add(1, "Home", "Home", 2); fail_param = i;
      assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") < 0);
      assert(profiles[1].exists && !profiles[2].exists && deleted == 1);
   }
   reset(); fail_copy = 1;
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") < 0 && !profiles[1].exists);
   reset(); corrupt_copy = 1;
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") < 0 && !profiles[1].exists);
   reset(); for (i = 1; i <= 10; i++) add(i, "Home", "Home", 2);
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") < 0 && !created && !deleted);
   add(10, "Phone", MGNET_AUTO_SSID, 0);
   assert(mgnet_profile_ensure(MGNET_AUTO_SSID, "") == 10 && !created);

   /* The picker's slot list.  The XMB never compacts slots: the owner's Go
    * had its only connection away from slot 1, and the old probe stopped at
    * the first gap, so it listed nothing ("?") and joined empty slot 1. */
   {
      unsigned char sl[10];
      memset(sl, 0, sizeof(sl));
      reset(); add(2, "GBAdhoc Relay", MGNET_AUTO_SSID, 0);
      assert(mgnet_config_list(sl, 10) == 1 && sl[0] == 2 && mgnet_config_count() == 1);
      assert(!loads && !unloads);                 /* found: no module load */
      add(5, "Home", "Home", 2); add(10, "Cafe", "Cafe", 0);
      assert(mgnet_config_list(sl, 10) == 3 && sl[0] == 2 && sl[1] == 5 && sl[2] == 10);
      assert(mgnet_config_list(sl, 2) == 3 && sl[1] == 5);   /* max respected */
      assert(mgnet_config_list(NULL, 0) == 3);
      /* Manual choice of a non-first slot connects THAT slot. */
      assert(mgnet_wifi_up(5, 0) == MGNET_OK && connected_slot == 5 && !created);
      mgnet_wifi_down();
      /* Firmware that wants the common module first: one load, one unload,
       * the same answer, nothing left loaded. */
      reset(); need_common = 1; add(3, "GBAdhoc Relay", MGNET_AUTO_SSID, 0);
      assert(mgnet_config_list(sl, 10) == 1 && sl[0] == 3);
      assert(loads == 1 && unloads == 1 && !common_loaded);
      /* No connections at all: still balanced, and Automatic remains. */
      reset(); need_common = 1;
      assert(mgnet_config_list(sl, 10) == 0 && loads == 1 && unloads == 1);
      reset();
      assert(mgnet_config_list(sl, 10) == 0 && loads == 1 && unloads == 1);
      /* A load that did not succeed here ("already loaded") is not undone. */
      reset(); load_rc = (int)0x80110F02;
      assert(mgnet_config_list(sl, 10) == 0 && loads == 1 && !unloads);
      /* With the radio up the probe never touches the modules. */
      reset(); add(1, "Home", "Home", 0);
      assert(mgnet_wifi_up(1, 0) == MGNET_OK);
      loads = unloads = 0; profiles[1].exists = 0;
      assert(mgnet_config_list(sl, 10) == 0 && !loads && !unloads);
      mgnet_wifi_down();
      /* A failed connect keeps its SCE code for the on-screen message. */
      reset(); add(2, "GBAdhoc Relay", MGNET_AUTO_SSID, 0);
      assert(mgnet_wifi_up(1, 0) == MGNET_ERR_CONNECT);
      assert(mgnet_last_sce() == 0x80410A0Bu && !strcmp(mgnet_stage(), "apctl_connect"));
      assert(!common_loaded && !apctl_ready);
   }

   assert(!mgift_shortcut_update(PSP_CTRL_SELECT, 1, &consumed));
   assert(mgift_shortcut_update(chord, 1, &consumed) && consumed == chord);
   for (i = 0; i < 120; i++) assert(!mgift_shortcut_update(chord, 1, &consumed));
   assert(!mgift_shortcut_update(PSP_CTRL_DOWN, 1, &consumed) && consumed == chord);
   assert(!mgift_shortcut_update(chord, 1, &consumed));
   assert(!mgift_shortcut_update(0, 1, &consumed) && !consumed);
   assert(!mgift_shortcut_update(PSP_CTRL_DOWN, 1, &consumed));
   assert(mgift_shortcut_update(chord, 1, &consumed));
   assert(!mgift_shortcut_update(0, 1, &consumed));
   assert(!mgift_shortcut_update(chord | PSP_CTRL_START, 1, &consumed));
   assert(!mgift_shortcut_update(chord | PSP_CTRL_LTRIGGER, 1, &consumed));
   assert(!mgift_shortcut_update(chord, 0, &consumed) && !consumed);

   /* Control remap: the chord is now the player's binding.  The default
    * (SELECT+DOWN) must behave exactly as the hand-written 3.0 version over
    * long random pad sequences; an unbound chord never fires and consumes
    * nothing; a rebound chord fires only on its own buttons. */
   {
      unsigned c_new = 0, c_old = 0, seed = 7, pad, k;
      for (k = 0; k < 2000000; k++)
      {
         int en, f_new, f_old;
         seed = seed * 1103515245u + 12345u;
         /* bias toward the chord so it is actually exercised */
         pad = (seed >> 16) & MGIFT_SHORTCUT_ALL;
         if ((seed >> 8) & 1) pad &= chord | PSP_CTRL_START;
         en = ((seed >> 4) & 15) != 0;
         f_new = mgift_shortcut_update(pad, en, &c_new);
         f_old = legacy_mgift(pad, en, &c_old);
         assert(f_new == f_old && c_new == c_old);
      }
      consumed = 0;
      for (k = 0; k < 4096; k++)
         assert(!mgift_shortcut_update_chord(k, 0, 1, &consumed) && !consumed);
      consumed = 0;
      assert(!mgift_shortcut_update_chord(chord, PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER,
                                          1, &consumed));
      assert(mgift_shortcut_update_chord(PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER,
                                         PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER,
                                         1, &consumed) &&
             consumed == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
   }
   puts("PASS: Automatic profile reuse/create/failure cleanup, gapped slot list, and Select+Down debounce/consumption");
   return 0;
}
