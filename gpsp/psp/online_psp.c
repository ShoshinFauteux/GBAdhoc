/* online_psp.c -- EXPERIMENTAL online play on the PSP.  See online_psp.h and
 * docs/ONLINE-TUNNEL.md.  The pieces:
 *
 *   mgift_net.c        infrastructure Wi-Fi (apctl) up/down, already shipped
 *                      for Mystery Gift; here it associates with a SAVED
 *                      connection (online_wifi), never a scratch profile.
 *   one UDP socket     sceNetInet, non-blocking, bound to ONLINE_PORT.  It
 *                      carries the server, both PROBEs and every peer: the
 *                      rule that makes a hole punch work.
 *   transport_relay.c  the GBR1 client (rendezvous, punch, relay fallback);
 *                      netdrv runs on it exactly as on transport_adhoc.
 */
#ifdef GBADHOC_ONLINE

#include "online_psp.h"
#include "mgift_net.h"
#include "../netdrv/transport_relay.h"
#include "../frontend-common/fe_evt.h"
#include "../frontend-common/fe_util.h"

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define ONLINE_PORT 27500

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
   uint32_t server_ip;
   uint16_t server_port;
   char room[RL_CODE_LEN + 1];
   int direct, wifi;
   unsigned drop_direct_ms;
   int fd;
   int wifi_up;
   relay_transport *rt;
   char why[40];
} O = { .fd = -1 };

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

static uint32_t parse_ip(const char *s, uint16_t *port)
{
   unsigned a, b, c, d, p = RL_DEFAULT_PORT;
   uint8_t x[4];
   uint32_t v;
   int n = sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p);
   if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65534)
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

int online_psp_configure(const char *h, const char *c)
{
   char s[48] = "";
   int i;
   O.enabled = 0;
   if (!get_str(h, c, "online_relay", s, sizeof(s)) || !s[0])
      return 0;
   O.server_ip = parse_ip(s, &O.server_port);
   if (!O.server_ip)
   {
      fe_evt("online_config error=bad_relay value=\"%s\"", s);
      return 0;
   }
   O.room[0] = 0;
   if (get_str(h, c, "online_room", s, sizeof(s)))
   {
      for (i = 0; i < RL_CODE_LEN && s[i]; i++)
         O.room[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
      O.room[i] = 0;
   }
   O.direct = (int)get_int(h, c, "online_direct", 1) ? 1 : 0;
   O.wifi = (int)get_int(h, c, "online_wifi", 1);
   O.drop_direct_ms = (unsigned)get_int(h, c, "online_drop_direct_ms", 0);
   O.enabled = 1;
   {
      const uint8_t *p = (const uint8_t *)&O.server_ip;
      fe_evt("online_config relay=%u.%u.%u.%u:%u room=%s direct=%d wifi=%d "
             "drop_direct_ms=%u", p[0], p[1], p[2], p[3], O.server_port,
             O.room[0] ? O.room : "-", O.direct, O.wifi, O.drop_direct_ms);
   }
   return 1;
}

int online_psp_enabled(void) { return O.enabled; }
const char *online_psp_why(void) { return O.why; }

static int sk_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   struct sockaddr_in a;
   (void)ctx;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   return sceNetInetSendto(O.fd, buf, len, 0, (struct sockaddr *)&a, sizeof(a)) < 0
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

int online_psp_up(int is_host, const char *build, unsigned timeout_ms)
{
   rl_sock sk;
   rl_client_cfg cfg;
   struct sockaddr_in a;
   int rc, opt = 1;
   uint64_t t0;
   O.why[0] = 0;
   rc = mgnet_wifi_up(O.wifi, 30000000u);
   if (rc != MGNET_OK)
   {
      snprintf(O.why, sizeof(O.why), "wifi %s", mgnet_stage());
      fe_evt("online_error stage=wifi rc=%d at=%s sce=0x%08X", rc, mgnet_stage(),
             mgnet_last_sce());
      list_wifi();
      return -1;
   }
   O.wifi_up = 1;
   list_wifi();
   fe_evt("online_wifi_up ip=%s", mgnet_local_ip());
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
      snprintf(O.why, sizeof(O.why), "bind");
      fe_evt("online_error stage=bind port=%d", ONLINE_PORT);
      online_psp_down();
      return -1;
   }
   sk.ctx = NULL;
   sk.send = sk_send;
   sk.recv = sk_recv;
   memset(&cfg, 0, sizeof(cfg));
   cfg.server_ip_be = O.server_ip;
   cfg.server_port = O.server_port;
   cfg.is_host = is_host;
   memcpy(cfg.code, O.room, sizeof(cfg.code));
   if (build)
      strncpy((char *)cfg.build, build, RL_BUILD_LEN);
   {
      uint16_t dummy;
      char ipb[24];
      snprintf(ipb, sizeof(ipb), "%s:1", mgnet_local_ip());
      cfg.priv_ip_be = parse_ip(ipb, &dummy);
   }
   cfg.priv_port = ONLINE_PORT;
   cfg.direct = O.direct;
   cfg.now_us = now_us;
   cfg.log = rl_log;
   cfg.drop_direct_after_us = (uint64_t)O.drop_direct_ms * 1000u;
   O.rt = relay_transport_create(&sk, &cfg);
   if (!O.rt)
   {
      snprintf(O.why, sizeof(O.why), "memory");
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
         snprintf(O.why, sizeof(O.why), "%s", st == RL_ST_FAILED ? tx : "server timeout");
         fe_evt("online_error stage=room state=%d why=\"%s\"", st, O.why);
         online_psp_down();
         return -1;
      }
      sceKernelDelayThread(2000);
   }
   fe_evt("online_up room=%s slot=%d host=%d", relay_transport_code(O.rt),
          relay_transport_slot(O.rt), is_host);
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

void online_psp_stats_evt(const char *at)
{
   rl_client_stats s;
   int j;
   char paths[RL_SLOTS + 1];
   if (!O.rt)
      return;
   relay_transport_stats(O.rt, &s);
   for (j = 0; j < RL_SLOTS; j++)
      paths[j] = "-rpdf"[s.path[j] < 5 ? s.path[j] : 0];
   paths[RL_SLOTS] = 0;
   fe_evt("online_stats at=%s nat=%u paths=%s tx=%u rx=%u tx_direct=%u rx_direct=%u "
          "tx_relay=%u rx_relay=%u punches=%u/%u acks=%u fallbacks=%u "
          "rtt_relay_ms=%u rtt_direct_ms=%u/%u bad=%u", at, s.nat, paths, s.tx, s.rx,
          s.tx_direct, s.rx_direct, s.tx_relay, s.rx_relay, s.punches_tx,
          s.punches_rx, s.acks_rx, s.fallbacks, s.relay_rtt_us / 1000,
          s.direct_rtt_us[0] / 1000, s.direct_rtt_us[1] / 1000, s.rx_bad);
}

#endif /* GBADHOC_ONLINE */
