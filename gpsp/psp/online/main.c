/* gbadhoc_online.prx -- EXPERIMENTAL online play, loaded only for an online
 * session (online_link.h says why and how).  The session itself: one
 * sceNetInet UDP socket, the relay's name resolved with sceNetResolver, and
 * transport_relay.c (rendezvous, punch, relay fallback) under the EBOOT's
 * netdrv.  Wi-Fi association stays in the EBOOT (mgift_net.c, shared with
 * Mystery Gift) and is reached through the host table.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "online_link.h"
#include "../../netdrv/transport_relay.h"

PSP_MODULE_INFO("gbadhoc_online", 0, 1, 0);

const online_host *g_ol_host;

#define ONLINE_PORT 27500

static struct
{
   online_cfg cfg;
   int fd;
   uint16_t port;               /* the bound port (ONLINE_PORT unless taken) */
   int resolver_up;
   relay_transport *rt;
} O = { .fd = -1 };

static online_api g_api;

static void evt(const char *fmt, ...)
{
   char b[224];
   va_list ap;
   va_start(ap, fmt);
   g_ol_host->vsnprintf_(b, sizeof(b), fmt, ap);
   va_end(ap);
   g_ol_host->evt(b);
}

static void beat(void)
{
   if (g_ol_host->beat)
      g_ol_host->beat();
}

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* "a.b.c.d[:port]" -> ip (network order); 0 = not a dotted quad */
static uint32_t parse_ip(const char *s, uint16_t *port)
{
   unsigned v[5] = { 0, 0, 0, 0, RL_DEFAULT_PORT };
   int i = 0, digits = 0;
   uint8_t x[4];
   uint32_t ip;
   for (;; s++)
   {
      if (*s >= '0' && *s <= '9')
      {
         if (digits == 0 && i == 4)
            v[4] = 0;
         v[i] = v[i] * 10u + (unsigned)(*s - '0');
         if (++digits > 5)
            return 0;
         continue;
      }
      if (!digits)
         return 0;
      if (*s == '.' && i < 3) { i++; digits = 0; continue; }
      if (*s == ':' && i == 3) { i = 4; digits = 0; continue; }
      if (*s == 0 && (i == 3 || i == 4))
         break;
      return 0;
   }
   for (i = 0; i < 4; i++)
      if (v[i] > 255)
         return 0;
   if (v[4] == 0 || v[4] > 65534)
      return 0;
   x[0] = (uint8_t)v[0]; x[1] = (uint8_t)v[1]; x[2] = (uint8_t)v[2]; x[3] = (uint8_t)v[3];
   memcpy(&ip, x, 4);
   *port = (uint16_t)v[4];
   return ip;
}

static int sk_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   struct sockaddr_in a;
   (void)ctx;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   (void)sceNetInetSendto(O.fd, buf, len, 0, (struct sockaddr *)&a, sizeof(a));
   return 0;                            /* UDP: a failed send is a loss */
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
   g_ol_host->evt(line);
}

static int resolve(uint32_t *ip, uint16_t *port)
{
   char host[64];
   char *colon;
   int rid = -1, rc;
   struct in_addr a;
   void *buf;
   *port = RL_DEFAULT_PORT;
   if ((*ip = parse_ip(O.cfg.relay, port)) != 0)
      return 0;
   snprintf(host, sizeof(host), "%s", O.cfg.relay);
   colon = strchr(host, ':');
   if (colon)
   {
      uint16_t p2;
      char tmp[24];
      *colon = 0;
      snprintf(tmp, sizeof(tmp), "1.1.1.1:%s", colon + 1);
      if (parse_ip(tmp, &p2))
         *port = p2;
   }
   if (!O.resolver_up)
   {
      rc = sceNetResolverInit();
      if (rc < 0)
      {
         evt("online_error stage=dns_init rc=0x%08X", (unsigned)rc);
         return -1;
      }
      O.resolver_up = 1;
   }
   buf = calloc(1, 1024);
   if (!buf)
      return -1;
   memset(&a, 0, sizeof(a));
   rc = sceNetResolverCreate(&rid, buf, 1024);
   if (rc >= 0)
   {
      rc = sceNetResolverStartNtoA(rid, host, &a, 2 /* s */, 3);
      sceNetResolverDelete(rid);
   }
   free(buf);
   if (rc < 0 || !a.s_addr)
   {
      evt("online_error stage=dns host=%s rc=0x%08X", host, (unsigned)rc);
      return -1;
   }
   *ip = a.s_addr;
   {
      const uint8_t *p = (const uint8_t *)ip;
      evt("online_dns host=%s ip=%u.%u.%u.%u port=%u", host, p[0], p[1], p[2],
          p[3], *port);
   }
   return 0;
}

static void ol_down(void)
{
   if (O.rt)
   {
      g_api.stats_evt("down");
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
}

static int ol_up(const online_cfg *cfg, int is_host, const char *build,
                 unsigned timeout_ms, char *why, unsigned why_cap)
{
   rl_sock sk;
   rl_client_cfg rc_cfg;
   struct sockaddr_in a;
   int rc, opt = 1;
   uint64_t t0;
   uint32_t sip;
   uint16_t sport;
   why[0] = 0;
   if (O.rt)
      return 0;
   O.cfg = *cfg;
   {
      /* the room as typed: its first four letters, upper-cased */
      int i, j = 0;
      for (i = 0; cfg->room[i] && j < RL_CODE_LEN; i++)
      {
         char ch = cfg->room[i];
         if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
         if (ch >= 'A' && ch <= 'Z')
            O.cfg.room[j++] = ch;
      }
      O.cfg.room[j] = 0;
   }
   (void)rc;
   if (resolve(&sip, &sport) != 0)
   {
      snprintf(why, why_cap, "server name not found");
      ol_down();
      return -1;
   }
   beat();
   O.fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
   if (O.fd < 0)
   {
      snprintf(why, why_cap, "socket");
      evt("online_error stage=socket rc=%d", O.fd);
      ol_down();
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
      /* the port is taken (another program on this network stack, or two
       * emulated consoles on one PC): any port will do -- the server and the
       * peer learn it from the datagrams, and the private candidate below
       * reads it back */
      a.sin_port = 0;
      if (sceNetInetBind(O.fd, (struct sockaddr *)&a, sizeof(a)) < 0)
      {
         snprintf(why, why_cap, "socket bind");
         evt("online_error stage=bind port=%d", ONLINE_PORT);
         ol_down();
         return -1;
      }
   }
   {
      struct sockaddr_in me;
      socklen_t ml = sizeof(me);
      memset(&me, 0, sizeof(me));
      O.port = sceNetInetGetsockname(O.fd, (struct sockaddr *)&me, &ml) >= 0 && me.sin_port
               ? ntohs(me.sin_port) : ONLINE_PORT;
   }
   sk.ctx = NULL;
   sk.send = sk_send;
   sk.recv = sk_recv;
   memset(&rc_cfg, 0, sizeof(rc_cfg));
   rc_cfg.server_ip_be = sip;
   rc_cfg.server_port = sport;
   rc_cfg.is_host = is_host;
   memcpy(rc_cfg.code, O.cfg.room, RL_CODE_LEN);
   rc_cfg.code[RL_CODE_LEN] = 0;
   if (build)
      memcpy(rc_cfg.build, build, strlen(build) < RL_BUILD_LEN ? strlen(build) : RL_BUILD_LEN);
   {
      uint16_t dummy;
      char ipb[24];
      snprintf(ipb, sizeof(ipb), "%s:1", g_ol_host->local_ip());
      rc_cfg.priv_ip_be = parse_ip(ipb, &dummy);
   }
   rc_cfg.priv_port = O.port;
   rc_cfg.direct = O.cfg.direct;
   rc_cfg.mode = O.cfg.mode;
   rc_cfg.now_us = now_us;
   rc_cfg.log = rl_log;
   rc_cfg.drop_direct_after_us = (uint64_t)O.cfg.drop_direct_ms * 1000u;
   O.rt = relay_transport_create(&sk, &rc_cfg);
   if (!O.rt)
   {
      snprintf(why, why_cap, "out of memory");
      ol_down();
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
         snprintf(why, why_cap, "%s", st == RL_ST_FAILED ? tx
                  : is_host ? "the server did not answer"
                            : "no such room, or the server did not answer");
         evt("online_error stage=room state=%d why=\"%s\"", st, why);
         ol_down();
         return -1;
      }
      beat();
      sceKernelDelayThread(2000);
   }
   evt("online_up room=%s slot=%d host=%d mode=%d", relay_transport_code(O.rt),
       relay_transport_slot(O.rt), is_host, relay_transport_mode(O.rt));
   return 0;
}

static int ol_is_up(void) { return O.rt != NULL; }
static const char *ol_room(void) { return O.rt ? relay_transport_code(O.rt) : ""; }
static int ol_room_mode(void) { return O.rt ? relay_transport_mode(O.rt) : 0; }
static void ol_iface(nd_transport *out) { relay_transport_iface(O.rt, out); }
static int ol_connected(void) { return O.rt != NULL && g_ol_host->wifi_is_up(); }

static void ol_status(char *out, unsigned cap)
{
   rl_client_stats s;
   int j, p = 0, peer = -1;
   if (!cap)
      return;
   out[0] = 0;
   if (!O.rt)
      return;
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

static void ol_stats_evt(const char *at)
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
   evt("online_stats at=%s nat=%u paths=%s tx=%u rx=%u tx_direct=%u rx_direct=%u "
       "tx_relay=%u rx_relay=%u punches=%u/%u acks=%u fallbacks=%u "
       "rtt_relay_ms=%u rtt_direct_ms=%u/%u bad=%u", at, s.nat, paths,
       (unsigned)s.tx, (unsigned)s.rx, (unsigned)s.tx_direct, (unsigned)s.rx_direct,
       (unsigned)s.tx_relay, (unsigned)s.rx_relay, (unsigned)s.punches_tx,
       (unsigned)s.punches_rx, (unsigned)s.acks_rx, (unsigned)s.fallbacks,
       (unsigned)(s.relay_rtt_us / 1000), (unsigned)(s.direct_rtt_us[0] / 1000),
       (unsigned)(s.direct_rtt_us[1] / 1000), (unsigned)s.rx_bad);
}

int module_start(SceSize args, void *argp)
{
   online_link *l;
   if (args < sizeof(void *) || !argp)
      return OL_ERR_ABI;
   l = *(online_link **)argp;
   if (!l || !l->host || l->host->magic != OL_MAGIC_HOST ||
       l->host->version != OL_VERSION || l->host->size != sizeof(online_host) || !l->api)
      return OL_ERR_ABI;
   g_ol_host = l->host;
   memset(&g_api, 0, sizeof(g_api));
   g_api.magic = OL_MAGIC_API;
   g_api.version = OL_VERSION;
   g_api.size = sizeof(online_api);
   g_api.up = ol_up;
   g_api.down = ol_down;
   g_api.is_up = ol_is_up;
   g_api.room = ol_room;
   g_api.room_mode = ol_room_mode;
   g_api.iface = ol_iface;
   g_api.connected = ol_connected;
   g_api.stats_evt = ol_stats_evt;
   g_api.status = ol_status;
   *l->api = g_api;
   return 0;
}

int module_stop(SceSize args, void *argp)
{
   (void)args;
   (void)argp;
   ol_down();               /* never leave Wi-Fi or a socket behind */
   return 0;
}
