/* Exercise the real profile selection/creation with SDK configuration stubs. */
#include <assert.h>
#include <stdarg.h>
#include "../psp/mgift_net.c"
#include "../psp/mgift_shortcut.h"

static struct { int exists; netData values[17]; } profiles[11];
static int created, deleted, fail_param = -1, fail_copy, corrupt_copy;
static int require_ready, apctl_ready, connected_slot;
int sceWlanGetSwitchState(void) { return 1; }
int sceUtilityLoadNetModule(int module) { (void)module; return 0; }
int sceUtilityUnloadNetModule(int module) { (void)module; return 0; }
int sceNetInit(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e;return 0; }
int sceNetTerm(void) { return 0; }
int sceNetInetInit(void) { return 0; }
int sceNetInetTerm(void) { return 0; }
int sceNetApctlInit(int a,int b) { (void)a;(void)b;apctl_ready=1;return 0; }
int sceNetApctlTerm(void) { apctl_ready=0;return 0; }
int sceNetApctlConnect(int slot) { assert(apctl_ready);connected_slot=slot;return 0; }
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
   puts("PASS: Automatic profile reuse/create/failure cleanup and Select+Down debounce/consumption");
   return 0;
}
