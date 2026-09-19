/* mgift_net.c — infrastructure-Wi-Fi bring-up for Mystery Gift, AND the owner
 * of the UDP socket that talks to the phone (see mgift_net.h for why this is
 * separate from the trading link).
 *
 * This file's header used to say the socket lived in
 * netdrv/transport_inet.c.  It does not, and never does at run time: that file
 * is absent from psp/Makefile's OBJS and is never linked.  The socket is
 * created and bound here, and no netdrv session is involved in Mystery Gift.
 */

#include "mgift_net.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspwlan.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "fe_evt.h"

/* Same pool and priorities the adhoc driver uses.  128 KiB is the crib-sheet
 * value with a hardware observation behind it (ADHOC-NOTES §11.11), and the
 * priority must sit in 0x08-0x77 or PPSSPP rejects it. */
#define MGNET_POOL        (128 * 1024)
#define MGNET_PRIO        0x2A
#define MGNET_STACK       4096
#define MGNET_APCTL_STACK 0x1400
#define MGNET_APCTL_PRIO  0x30

/* Default association timeout.  Associating with a phone hotspot is DHCP plus
 * WPA, and a PSP-1000 radio is not quick about either. */
#define MGNET_CONNECT_US  (30u * 1000000u)

/* Datagrams drained per frame.  The station sends at most ~3/s, so this is
 * only a bound on how much work one tick can do if something floods the
 * broadcast port -- the poll must never be able to stall a frame. */
#define MGNET_DRAIN_MAX   16

/* Init progress ladder; down() unwinds exactly what up() reached. */
enum
{
   ST_DOWN = 0,
   ST_MOD_COMMON,
   ST_MOD_INET,
   ST_NET,           /* sceNetInit done */
   ST_INET,          /* sceNetInetInit done */
   ST_APCTL,         /* sceNetApctlInit done */
   ST_ASSOCIATED     /* apctl reached GOT_IP -- the top rung of BRING-UP.
                      * The socket is not a rung of this ladder: it is created
                      * and bound separately below, in this same file. */
};

static struct
{
   int      progress;
   char     ip[20];
   unsigned last_sce;
   const char *stage;
} N = { 0, { 0 }, 0, "down" };
static int Cprofile_slot;

static void stage(const char *s)
{
   N.stage = s;
}

const char *mgnet_stage(void)   { return N.stage; }
unsigned    mgnet_last_sce(void){ return N.last_sce; }
int         mgnet_wifi_is_up(void){ return N.progress >= ST_ASSOCIATED; }
const char *mgnet_local_ip(void){ return N.ip; }

int mgnet_config_count(void)
{
   netData d;
   int i, n = 0;

   /* The XMB stores network configurations 1..N with no count to ask for, so
    * probe until one is missing.  10 is well past what the XMB itself lists. */
   for (i = 1; i <= 10; i++)
   {
      memset(&d, 0, sizeof(d));
      if (sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &d) < 0)
         break;
      n = i;
   }
   return n;
}

int mgnet_config_name(int index, char *out, unsigned out_sz)
{
   netData d;

   if (!out || out_sz == 0)
      return -1;
   out[0] = '\0';
   memset(&d, 0, sizeof(d));
   if (sceUtilityGetNetParam(index, PSP_NETPARAM_NAME, &d) < 0)
      return -1;
   /* netData.asString is not promised to be NUL-terminated. */
   snprintf(out, out_sz, "%.*s", (int)sizeof(d.asString), d.asString);
   return 0;
}

/* ---- bring-up ----------------------------------------------------------- */

static int fail(int rc, const char *where, unsigned sce)
{
   N.last_sce = sce;
   stage(where);
   fe_evt("mg_net_error stage=%s rc=%d sce=0x%08X", where, rc, sce);
   mgnet_profile_release();
   mgnet_wifi_down();
   stage(where);      /* down() resets it; the failing rung is the useful one */
   return rc;
}

int mgnet_wifi_up(int conf, unsigned timeout_us)
{
   int rc, state;
   unsigned t0;
   union SceNetApctlInfo info;

   if (N.progress != ST_DOWN)
      return MGNET_ERR_ALREADY;
   Cprofile_slot = 0;
   N.last_sce = 0;
   if (!timeout_us)
      timeout_us = MGNET_CONNECT_US;

   /* The switch is the single most common reason wireless "does not work",
    * and it is free to check before touching a module. */
   if (sceWlanGetSwitchState() == 0)
   {
      stage("wlan_switch");
      return MGNET_ERR_WLAN_OFF;
   }

   stage("mod_common");
   rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
   if (rc < 0)
      return fail(MGNET_ERR_MODULES, "mod_common", (unsigned)rc);
   N.progress = ST_MOD_COMMON;

   stage("mod_inet");
   rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
   if (rc < 0)
      return fail(MGNET_ERR_MODULES, "mod_inet", (unsigned)rc);
   N.progress = ST_MOD_INET;

   stage("net_init");
   rc = sceNetInit(MGNET_POOL, MGNET_PRIO, MGNET_STACK, MGNET_PRIO,
                   MGNET_STACK);
   if (rc < 0)
      return fail(MGNET_ERR_NET_INIT, "net_init", (unsigned)rc);
   N.progress = ST_NET;

   stage("inet_init");
   rc = sceNetInetInit();
   if (rc < 0)
      return fail(MGNET_ERR_INET_INIT, "inet_init", (unsigned)rc);
   N.progress = ST_INET;

   stage("apctl_init");
   rc = sceNetApctlInit(MGNET_APCTL_STACK, MGNET_APCTL_PRIO);
   if (rc < 0)
      return fail(MGNET_ERR_APCTL_INIT, "apctl_init", (unsigned)rc);
   N.progress = ST_APCTL;

   /* Automatic means this exact open hotspot, not the first saved network.
    * Initialize the network stack before reading/writing its parameters. */
   if (conf <= 0)
   {
      conf = mgnet_profile_ensure(MGNET_AUTO_SSID, "");
      if (conf < 0)
         return fail(MGNET_ERR_NO_CONFIG, "auto_profile", N.last_sce);
   }

   stage("apctl_connect");
   rc = sceNetApctlConnect(conf);
   if (rc < 0)
      return fail(MGNET_ERR_CONNECT, "apctl_connect", (unsigned)rc);

   /* Poll rather than take a handler: the handler runs on apctl's own thread,
    * and the adhoc driver's rule is that library-thread callbacks may only
    * write flags.  There is nothing here a flag would buy us. */
   stage("apctl_wait");
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   for (;;)
   {
      state = 0;
      if (sceNetApctlGetState(&state) < 0)
         return fail(MGNET_ERR_CONNECT, "apctl_state", 0);
      if (state == PSP_NET_APCTL_STATE_GOT_IP)
         break;
      if ((unsigned)sceKernelGetSystemTimeLow() - t0 > timeout_us)
         return fail(MGNET_ERR_CONNECT_TIMEOUT, "apctl_wait",
                     (unsigned)state);
      sceKernelDelayThread(50000);
   }
   N.progress = ST_ASSOCIATED;

   memset(&info, 0, sizeof(info));
   if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) >= 0)
      snprintf(N.ip, sizeof(N.ip), "%.*s", (int)sizeof(info.ip), info.ip);
   else
      N.ip[0] = '\0';

   N.progress = ST_ASSOCIATED;
   stage("up");
   fe_evt("mg_net_up ip=%s port=%d conf=%d", N.ip, MGNET_LISTEN_PORT, conf);
   return MGNET_OK;
}

void mgnet_wifi_down(void)
{
   if (N.progress == ST_DOWN)
      return;

   if (N.progress >= ST_ASSOCIATED)
   {
      int state, i;
      sceNetApctlDisconnect();
      /* Wait for the radio to actually leave the AP before tearing the
       * library down.  The adhoc path learned this the hard way: terminating
       * under a live association is what wedges the next bring-up. */
      for (i = 0; i < 100; i++)
      {
         state = 0;
         if (sceNetApctlGetState(&state) < 0)
            break;
         if (state == PSP_NET_APCTL_STATE_DISCONNECTED)
            break;
         sceKernelDelayThread(50000);
      }
   }
   if (N.progress >= ST_APCTL)
      sceNetApctlTerm();
   if (N.progress >= ST_INET)
      sceNetInetTerm();
   if (N.progress >= ST_NET)
      sceNetTerm();
   if (N.progress >= ST_MOD_INET)
      sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
   if (N.progress >= ST_MOD_COMMON)
      sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);

   N.progress = ST_DOWN;
   N.ip[0]    = '\0';
   stage("down");
   fe_evt("mg_net_down");
}

/* ---- the temporary connection profile ----------------------------------- */

/* Only a profile newly created by this attempt may be removed on failure.
 * Existing profiles are borrowed and must never be deleted or overwritten. */

/* sceUtilityCheckNetParam returns 0 when the slot EXISTS.  The PSP's own
 * Network Settings list is 1-based and short; 10 is well past what it shows. */
#define MGNET_PROFILE_MAX 10

static int profile_name_is_ours(int slot)
{
   netData d;
   memset(&d, 0, sizeof(d));
   if (sceUtilityGetNetParam(slot, PSP_NETPARAM_NAME, &d) < 0)
      return 0;
   d.asString[sizeof(d.asString) - 1] = 0;
   return strcmp(d.asString, MGNET_PROFILE_NAME) == 0;
}

static int profile_matches_open(int slot, const char *ssid)
{
   netData d;
   static const int zero_params[] = { PSP_NETPARAM_SECURE,
      PSP_NETPARAM_IS_STATIC_IP, PSP_NETPARAM_MANUAL_DNS, PSP_NETPARAM_USE_PROXY };
   unsigned i;
   memset(&d, 0, sizeof(d));
   if (sceUtilityGetNetParam(slot, PSP_NETPARAM_SSID, &d) < 0)
      return 0;
   d.asString[sizeof(d.asString) - 1] = 0;
   if (strcmp(d.asString, ssid))
      return 0;
   for (i = 0; i < sizeof(zero_params) / sizeof(zero_params[0]); i++)
   {
      memset(&d, 0, sizeof(d));
      if (sceUtilityGetNetParam(slot, zero_params[i], &d) < 0 || d.asUint != 0)
         return 0;
   }
   return 1;
}

int mgnet_profile_ensure(const char *ssid, const char *key)
{
   int slot = 0, i, rc;
   unsigned v;

   Cprofile_slot = 0;
   if (!ssid || !ssid[0] || strlen(ssid) > 32 || (key && key[0]))
      return -1;

   /* A saved profile's display name is not its SSID. Reuse any exact open
    * hotspot match, including one left by an earlier successful session. */
   for (i = 1; i <= MGNET_PROFILE_MAX; i++)
      if (sceUtilityCheckNetParam(i) == 0 && profile_matches_open(i, ssid))
      {
         fe_evt("mg_profile reuse slot=%d ssid=%s", i, ssid);
         return i;
      }

   /* Create only in a free slot. CreateNetParam is not an update operation. */
   if (!slot)
      for (i = 1; i <= MGNET_PROFILE_MAX; i++)
         if (sceUtilityCheckNetParam(i) != 0)
         {
            slot = i;
            break;
         }

   if (!slot)
   {
      fe_evt("mg_profile err=no_free_slot");
      return -1;
   }

   /* ORDER MATTERS, and not in the obvious way.  CreateNetParam(slot) also
    * CLEARS configuration 0, and SetNetParam only ever writes configuration 0 --
    * so fill 0 AFTER creating, then copy 0 over the slot. */
   rc = sceUtilityCreateNetParam(slot);
   if (rc < 0)
   {
      N.last_sce = (unsigned)rc;
      fe_evt("mg_profile err=create slot=%d rc=0x%08X", slot, rc);
      return -1;
   }

   {
      const char *nm = MGNET_PROFILE_NAME;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_NAME, nm);
      if (rc < 0) goto failed;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_SSID, ssid);
      if (rc < 0) goto failed;
      /* Open security, DHCP, automatic DNS, no proxy. Create cleared the
       * scratch profile, so do not set an irrelevant WEP/WPA key parameter. */
      v = 0u;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_SECURE, &v);
      if (rc < 0) goto failed;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_IS_STATIC_IP, &v);
      if (rc < 0) goto failed;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_MANUAL_DNS, &v);
      if (rc < 0) goto failed;
      rc = sceUtilitySetNetParam(PSP_NETPARAM_USE_PROXY, &v);
      if (rc < 0) goto failed;
   }

   rc = sceUtilityCopyNetParam(0, slot);
   if (rc < 0)
      goto failed;
   if (!profile_matches_open(slot, ssid) || !profile_name_is_ours(slot))
   { rc = -1; goto failed; }

   Cprofile_slot = slot;
   fe_evt("mg_profile ready slot=%d ssid=%s secure=%d", slot, ssid,
          (key && key[0]) ? 2 : 0);
   return slot;
failed:
   N.last_sce = (unsigned)rc;
   fe_evt("mg_profile err=configure slot=%d rc=0x%08X", slot, rc);
   sceUtilityDeleteNetParam(slot);
   return -1;
}

void mgnet_profile_release(void)
{
   if (!Cprofile_slot)
      return;
   /* Only ever delete the one we made, and only if it still looks like ours --
    * the player may have been editing their connections in the meantime. */
   if (profile_name_is_ours(Cprofile_slot))
   {
      sceUtilityDeleteNetParam(Cprofile_slot);
      fe_evt("mg_profile released slot=%d", Cprofile_slot);
   }
   Cprofile_slot = 0;
}

/* ---- the card carrier --------------------------------------------------- */

static int      Csock = -1;
static struct sockaddr_in Cfrom;
static int      Chave_from;

int mgnet_card_open(void)
{
   struct sockaddr_in addr;
   int opt;

   if (Csock >= 0)
      return 0;
   if (N.progress < ST_ASSOCIATED)
      return -1;

   Csock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
   if (Csock < 0)
      return -1;
   opt = 1;
   sceNetInetSetsockopt(Csock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   opt = 1;
   sceNetInetSetsockopt(Csock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
   /* No fcntl on the PSP; SO_NONBLOCK is how a socket is made non-blocking,
    * and it is what keeps the poll off the frame's critical path. */
   opt = 1;
   sceNetInetSetsockopt(Csock, SOL_SOCKET, SO_NONBLOCK, &opt, sizeof(opt));

   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_port        = htons(MGNET_LISTEN_PORT);
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   if (sceNetInetBind(Csock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
   {
      sceNetInetClose(Csock);
      Csock = -1;
      return -1;
   }
   Chave_from = 0;
   fe_evt("mg_card_listen port=%d", MGNET_LISTEN_PORT);
   return 0;
}

void mgnet_card_close(void)
{
   if (Csock >= 0)
      sceNetInetClose(Csock);
   Csock = -1;
   Chave_from = 0;
}

int mgnet_card_poll(mgift_parcel *out)
{
   uint8_t buf[MGIFT_PARCEL_MAX + 1];
   struct sockaddr_in from;
   socklen_t flen = sizeof(from);
   int n;

   if (Csock < 0 || !out)
      return 0;

   memset(&from, 0, sizeof(from));
   /* sceNetInetRecvfrom is declared returning size_t, so an error arrives as
    * 0xFFFFFFFF; cast before testing or "nothing waiting" reads as a 4 GB
    * datagram. */
   n = (int)sceNetInetRecvfrom(Csock, buf, sizeof(buf), MSG_DONTWAIT,
                               (struct sockaddr *)&from, &flen);
   if (n < 16)
      return 0;
   if (mgift_parcel_decode(out, buf, (unsigned)n) != 0)
   {
      if (!memcmp(buf, "MGC2", 4))
      {
         uint8_t ack[12];
         memcpy(ack, "MGA2", 4); memcpy(ack + 4, buf + 4, 4);
         memset(ack + 8, 0, 4); ack[8] = 2;
         sceNetInetSendto(Csock, ack, sizeof(ack), 0,
                         (struct sockaddr *)&from, sizeof(from));
      }
      return 0;
   }
   Cfrom      = from;
   Chave_from = 1;

   return 1;
}

void mgnet_card_reply(uint32_t id, unsigned result)
{
   uint8_t ack[12];
   if (Csock < 0 || !Chave_from) return;
   mgift_parcel_ack(ack, id, result);
   sceNetInetSendto(Csock, ack, sizeof(ack), 0,
                   (struct sockaddr *)&Cfrom, sizeof(Cfrom));
}
