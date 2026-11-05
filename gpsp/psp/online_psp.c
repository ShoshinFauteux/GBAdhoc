/* online_psp.c -- EXPERIMENTAL online play on the PSP.  See online_psp.h and
 * docs/ONLINE-TUNNEL.md.  The pieces:
 *
 *   mgift_net.c        infrastructure Wi-Fi (apctl) up/down, already shipped
 *                      for Mystery Gift; here it associates with one of the
 *                      PSP's SAVED connections (online_wifi), never a
 *                      scratch profile, and GBAdhoc never sees a password.
 *   sceNetResolver     the relay's host name (relay.rayquaza.ca) -> IPv4.
 *   one UDP socket     sceNetInet, non-blocking, bound to ONLINE_PORT.  It
 *                      carries the server, both PROBEs and every peer: the
 *                      rule that makes a hole punch work.
 *   transport_relay.c  the GBR1 client (rendezvous, punch, relay fallback);
 *                      netdrv runs on it exactly as on transport_adhoc.
 *
 * Memory: the state below is ~120 bytes of .bss; the transport (~2.5 KiB) and
 * the resolver's 1 KiB scratch are malloc'd only while a session is online.
 */
#ifdef GBADHOC_ONLINE

#include "online_psp.h"
#include "mgift_net.h"
#include "../netdrv/transport_relay.h"
#include "../frontend-common/fe_evt.h"
#include "../frontend-common/fe_util.h"

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define ONLINE_PORT 27500

#ifdef GPSP_PERF_RIG
/* main_psp.c (harness/lab builds only): a bounded network wait counts as the
 * main loop turning, for the harness stall exit.  Nothing in a release. */
void gpsp_net_wait_beat(void);
#define NET_WAIT_BEAT() gpsp_net_wait_beat()
#else
#define NET_WAIT_BEAT() ((void)0)
#endif
#define BEAT() NET_WAIT_BEAT()

/* The Lab validates harness keys by grepping this exact call form
 * (backend/lab/inikeys.py).  online_psp_configure() reads each of them from
 * the harness ini first, then config.ini:
 *   fe_ini_get(HARNESS_INI, "online_relay")
 *   fe_ini_get(HARNESS_INI, "online_room")
 *   fe_ini_get_int(HARNESS_INI, "online_direct")
 *   fe_ini_get_int(HARNESS_INI, "online_wifi")
 *   fe_ini_get_int(HARNESS_INI, "online_drop_direct_ms")
 */

static struct
{
   int enabled;
   char relay[64];              /* host name or IPv4[:port] */
   char room[RL_CODE_LEN + 1];
   int direct, wifi, mode;
   unsigned drop_direct_ms;
   int fd;
   int wifi_up, resolver_up;
   relay_transport *rt;
   char why[48];
} O = { .fd = -1, .direct = 1, .wifi = 1 };

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* "a.b.c.d[:port]" -> ip (network order); 0 = not a dotted quad */
static uint32_t parse_ip(const char *s, uint16_t *port)
{
   unsigned a, b, c, d, p = RL_DEFAULT_PORT;
   uint8_t x[4];
   uint32_t v;
   char tail;
   int n = sscanf(s, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &p, &tail);
   if (n < 4 || n > 5 || a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65534)
      return 0;
   x[0] = (uint8_t)a; x[1] = (uint8_t)b; x[2] = (uint8_t)c; x[3] = (uint8_t)d;
   memcpy(&v, x, 4);
   *port = (uint16_t)p;
   return v;
}

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

static void set_room(const char *s)
{
   int i, j = 0;
   for (i = 0; s && s[i] && j < RL_CODE_LEN; i++)
   {
      char ch = s[i];
      if (ch >= 'a' && ch <= 'z')
         ch = (char)(ch - 32);
      if (ch >= 'A' && ch <= 'Z')
         O.room[j++] = ch;
   }
   O.room[j] = 0;
}

static char g_cfg_relay[64];   /* config.ini online_relay, for menu sessions */

int online_psp_configure(const char *h, const char *c)
{
   char s[64] = "";
   O.enabled = 0;
   g_cfg_relay[0] = 0;
   if (c)
      fe_ini_get(c, "online_relay", g_cfg_relay, sizeof(g_cfg_relay));
   /* every-session online: only the harness ini turns it on (a player turns
    * it on per session from the menu) */
   if (!fe_ini_get(h, "online_relay", s, sizeof(s)) || !s[0])
      return 0;
   snprintf(O.relay, sizeof(O.relay), "%s", s);
   O.room[0] = 0;
   if (get_str(h, c, "online_room", s, sizeof(s)))
      set_room(s);
   O.direct = (int)get_int(h, c, "online_direct", 1) ? 1 : 0;
   O.wifi = (int)get_int(h, c, "online_wifi", 1);
   O.drop_direct_ms = (unsigned)get_int(h, c, "online_drop_direct_ms", 0);
   O.mode = RL_MODE_ANY;
   O.enabled = 1;
   fe_evt("online_config relay=%s room=%s direct=%d wifi=%d drop_direct_ms=%u",
          O.relay, O.room[0] ? O.room : "-", O.direct, O.wifi, O.drop_direct_ms);
   return 1;
}

void online_psp_set(const char *relay, const char *room, int wifi, int mode)
{
   snprintf(O.relay, sizeof(O.relay), "%s",
            relay && relay[0] ? relay : g_cfg_relay[0] ? g_cfg_relay
                                                      : ONLINE_RELAY_DEFAULT);
   set_room(room);
   O.wifi = wifi > 0 ? wifi : 1;
   O.mode = mode;
   O.direct = 1;
   O.drop_direct_ms = 0;
   O.enabled = 1;
   fe_evt("online_set relay=%s room=%s wifi=%d mode=%d", O.relay,
          O.room[0] ? O.room : "-", O.wifi, O.mode);
}

void online_psp_clear(void)
{
   if (!O.rt)
      O.enabled = 0;
}

int online_psp_enabled(void) { return O.enabled; }
int online_psp_is_up(void) { return O.rt != NULL; }
const char *online_psp_why(void) { return O.why; }
const char *online_psp_room(void) { return O.rt ? relay_transport_code(O.rt) : ""; }
int online_psp_room_mode(void) { return O.rt ? relay_transport_mode(O.rt) : 0; }
int online_psp_wifi_count(void) { return mgnet_config_count(); }
int online_psp_wifi_name(int i, char *o, unsigned c) { return mgnet_config_name(i, o, c); }

static int sk_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   struct sockaddr_in a;
   (void)ctx;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   return (int)sceNetInetSendto(O.fd, buf, len, 0, (struct sockaddr *)&a, sizeof(a)) < 0
          ? 0 : 0;                       /* UDP: a failed send is a loss */
}

static int sk_recv(void *ctx, uint32_t *ip, uint16_t *port, void *buf, size_t cap)
{
   struct sockaddr_in a;
   socklen_t al = sizeof(a);
   int n;
   (void)ctx;
   memset(&a, 0, sizeof(a));
   n = (int)sceNetInetRecvfrom(O.fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)&a, &al);
   if (n <= 0)
      return 0;
   *ip = a.sin_addr.s_addr;
   *port = ntohs(a.sin_port);
   return n;
}

static void rl_log(void *u, const char *line)
{
   (void)u;
   FE_EVT_ONLY(line);
   fe_evt("%s", line);
}

static void list_wifi(void)
{
   int n = mgnet_config_count(), i;
   char nm[64];
   fe_evt("online_wifi profiles=%d chosen=%d", n, O.wifi);
   for (i = 1; i <= n && i <= 10; i++)
      if (mgnet_config_name(i, nm, sizeof(nm)) == 0)
         fe_evt("online_wifi profile=%d name=\"%s\"", i, nm);
}

/* host[:port] or a.b.c.d[:port] -> ip/port.  0 = resolved. */
static int resolve(uint32_t *ip, uint16_t *port)
{
   char host[64];
   char *colon;
   int rid = -1, rc;
   struct in_addr a;
   void *buf;
   *port = RL_DEFAULT_PORT;
   if ((*ip = parse_ip(O.relay, port)) != 0)
      return 0;
   snprintf(host, sizeof(host), "%s", O.relay);
   colon = strchr(host, ':');
   if (colon)
   {
      unsigned p = (unsigned)atoi(colon + 1);
      *colon = 0;
      if (p > 0 && p < 65535)
         *port = (uint16_t)p;
   }
   if (!O.resolver_up)
   {
      rc = sceNetResolverInit();
      if (rc < 0)
      {
         fe_evt("online_error stage=dns_init rc=0x%08X", rc);
         return -1;
      }
      O.resolver_up = 1;
   }
   buf = malloc(1024);
   if (!buf)
      return -1;
   rc = sceNetResolverCreate(&rid, buf, 1024);
   if (rc >= 0)
   {
      memset(&a, 0, sizeof(a));
      rc = sceNetResolverStartNtoA(rid, host, &a, 2 /* s */, 3);
      sceNetResolverDelete(rid);
   }
   free(buf);
   if (rc < 0 || !a.s_addr)
   {
      fe_evt("online_error stage=dns host=%s rc=0x%08X", host, rc);
      return -1;
   }
   *ip = a.s_addr;
   {
      const uint8_t *p = (const uint8_t *)ip;
      FE_EVT_ONLY(p);
      fe_evt("online_dns host=%s ip=%u.%u.%u.%u port=%u", host, p[0], p[1], p[2],
             p[3], *port);
   }
   return 0;
}

int online_psp_up(int is_host, const char *build, unsigned timeout_ms)
{
   rl_sock sk;
   rl_client_cfg cfg;
   struct sockaddr_in a;
   int rc, opt = 1;
   uint64_t t0;
   uint32_t sip;
   uint16_t sport;
   O.why[0] = 0;
   if (O.rt)
      return 0;                          /* already up (menu join) */
   rc = mgnet_wifi_up(O.wifi, 30000000u);
   if (rc != MGNET_OK)
   {
      snprintf(O.why, sizeof(O.why), rc == MGNET_ERR_WLAN_OFF ? "WLAN switch off"
               : rc == MGNET_ERR_NO_CONFIG ? "no saved Wi-Fi network"
               : rc == MGNET_ERR_CONNECT_TIMEOUT ? "Wi-Fi: no address"
               : "Wi-Fi did not connect");
      fe_evt("online_error stage=wifi rc=%d at=%s sce=0x%08X", rc, mgnet_stage(),
             mgnet_last_sce());
      list_wifi();
      return -1;
   }
   O.wifi_up = 1;
   BEAT();
   list_wifi();
   fe_evt("online_wifi_up ip=%s", mgnet_local_ip());
   if (resolve(&sip, &sport) != 0)
   {
      snprintf(O.why, sizeof(O.why), "server name not found");
      online_psp_down();
      return -1;
   }
   BEAT();
   O.fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
   if (O.fd < 0)
   {
      snprintf(O.why, sizeof(O.why), "socket");
      fe_evt("online_error stage=socket rc=%d", O.fd);
      online_psp_down();
      return -1;
   }
   sceNetInetSetsockopt(O.fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   sceNetInetSetsockopt(O.fd, SOL_SOCKET, SO_NONBLOCK, &opt, sizeof(opt));
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_port = htons(ONLINE_PORT);
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   if (sceNetInetBind(O.fd, (struct sockaddr *)&a, sizeof(a)) < 0)
   {
      snprintf(O.why, sizeof(O.why), "socket bind");
      fe_evt("online_error stage=bind port=%d", ONLINE_PORT);
      online_psp_down();
      return -1;
   }
   sk.ctx = NULL;
   sk.send = sk_send;
   sk.recv = sk_recv;
   memset(&cfg, 0, sizeof(cfg));
   cfg.server_ip_be = sip;
   cfg.server_port = sport;
   cfg.is_host = is_host;
   memcpy(cfg.code, O.room, sizeof(cfg.code));
   if (build)
      memcpy(cfg.build, build, strlen(build) < RL_BUILD_LEN ? strlen(build) : RL_BUILD_LEN);
   {
      uint16_t dummy;
      char ipb[24];
      snprintf(ipb, sizeof(ipb), "%s:1", mgnet_local_ip());
      cfg.priv_ip_be = parse_ip(ipb, &dummy);
   }
   cfg.priv_port = ONLINE_PORT;
   cfg.direct = O.direct;
   cfg.mode = O.mode;
   cfg.now_us = now_us;
   cfg.log = rl_log;
   cfg.drop_direct_after_us = (uint64_t)O.drop_direct_ms * 1000u;
   O.rt = relay_transport_create(&sk, &cfg);
   if (!O.rt)
   {
      snprintf(O.why, sizeof(O.why), "out of memory");
      online_psp_down();
      return -1;
   }
   t0 = now_us();
   for (;;)
   {
      int st = relay_transport_poll(O.rt);
      if (st == RL_ST_IN_ROOM)
         break;
      if (st == RL_ST_FAILED || now_us() - t0 > (uint64_t)timeout_ms * 1000u)
      {
         const char *tx = "";
         relay_transport_error(O.rt, &tx);
         snprintf(O.why, sizeof(O.why), "%s", st == RL_ST_FAILED ? tx
                  : is_host ? "the server did not answer"
                            : "no such room, or the server did not answer");
         fe_evt("online_error stage=room state=%d why=\"%s\"", st, O.why);
         online_psp_down();
         return -1;
      }
      BEAT();
      sceKernelDelayThread(2000);
   }
   fe_evt("online_up room=%s slot=%d host=%d mode=%d", relay_transport_code(O.rt),
          relay_transport_slot(O.rt), is_host, relay_transport_mode(O.rt));
   return 0;
}

void online_psp_iface(nd_transport *out)
{
   relay_transport_iface(O.rt, out);
}

void online_psp_down(void)
{
   if (O.rt)
   {
      online_psp_stats_evt("down");
      relay_transport_destroy(O.rt);
      O.rt = NULL;
   }
   if (O.fd >= 0)
   {
      sceNetInetClose(O.fd);
      O.fd = -1;
   }
   if (O.resolver_up)
   {
      sceNetResolverTerm();
      O.resolver_up = 0;
   }
   if (O.wifi_up)
   {
      mgnet_wifi_down();
      O.wifi_up = 0;
   }
}

int online_psp_connected(void)
{
   return O.rt != NULL && mgnet_wifi_is_up();
}

void online_psp_status(char *out, unsigned cap)
{
   rl_client_stats s;
   int j, p = 0, peer = -1;
   if (!O.rt || !cap)
   {
      if (cap)
         out[0] = 0;
      return;
   }
   relay_transport_stats(O.rt, &s);
   for (j = 0; j < RL_SLOTS; j++)
      if (j != relay_transport_slot(O.rt) && s.path[j])
      {
         peer = j;
         p = s.path[j];
         break;
      }
   if (peer >= 0 && p == 3)
      snprintf(out, cap, "Online %s  direct %u ms", relay_transport_code(O.rt),
               (unsigned)(s.direct_rtt_us[peer] / 1000));
   else
      snprintf(out, cap, "Online %s  relay %u ms", relay_transport_code(O.rt),
               (unsigned)(s.relay_rtt_us / 1000 * 2));
}

void online_psp_stats_evt(const char *at)
{
   rl_client_stats s;
   int j;
   char paths[RL_SLOTS + 1];
   FE_EVT_ONLY(at);
   FE_EVT_ONLY(paths);
   if (!O.rt)
      return;
   relay_transport_stats(O.rt, &s);
   for (j = 0; j < RL_SLOTS; j++)
      paths[j] = "-rpdf"[s.path[j] < 5 ? s.path[j] : 0];
   paths[RL_SLOTS] = 0;
   fe_evt("online_stats at=%s nat=%u paths=%s tx=%u rx=%u tx_direct=%u rx_direct=%u "
          "tx_relay=%u rx_relay=%u punches=%u/%u acks=%u fallbacks=%u "
          "rtt_relay_ms=%u rtt_direct_ms=%u/%u bad=%u", at, s.nat, paths,
          (unsigned)s.tx, (unsigned)s.rx, (unsigned)s.tx_direct, (unsigned)s.rx_direct,
          (unsigned)s.tx_relay, (unsigned)s.rx_relay, (unsigned)s.punches_tx,
          (unsigned)s.punches_rx, (unsigned)s.acks_rx, (unsigned)s.fallbacks,
          (unsigned)(s.relay_rtt_us / 1000), (unsigned)(s.direct_rtt_us[0] / 1000),
          (unsigned)(s.direct_rtt_us[1] / 1000), (unsigned)s.rx_bad);
}

#endif /* GBADHOC_ONLINE */
