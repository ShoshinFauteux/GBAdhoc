/* gbadhoc_online.prx -- EXPERIMENTAL online play, loaded only for an online
 * session (online_link.h says why and how).  The session itself: one
 * sceNetInet UDP socket, the relay's name resolved with sceNetResolver, and
 * transport_relay.c (rendezvous, punch, relay fallback) under the EBOOT's
 * netdrv.  Before the server is contacted, home relay discovery (beta.5): one
 * DISCOVER to the LAN broadcast addresses, answers read for 400 ms -- a relay
 * on this LAN that is the configured one is used at its LAN address (routers
 * without NAT hairpin).  No answer: exactly as before.  Wi-Fi association stays in the EBOOT (mgift_net.c, shared with
 * Mystery Gift) and is reached through the host table.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspsysmem.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "online_link.h"
#include "../../netdrv/transport_relay.h"
#include "../../netdrv/invite_code.h"

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
   /* the path display (ol_recv): the transport's own recv, the next
    * refresh, the path last shown ('D'irect / 'R'elay, a toast on change) */
   int (*recv)(void *ctx, uint8_t mac[6], void *buf, size_t cap);
   uint64_t show_us;
   char shown;
   char chip[48];
   int invite;                  /* joined by an invite code: the server is a desktop host */
   /* home relay discovery: the configured server's name (none for a dotted
    * quad or an invite) and the broadcast addresses DISCOVER goes to */
   char name[64];
   uint32_t bcast[2];
} O = { .fd = -1 };

static online_api g_api;

/* WHAT THE SOCKET DID, for the failure text.  A release build has no log
 * (fe_evt is compiled out), and "the server did not answer" alone cannot
 * tell "nothing left the PSP" from "nothing came back": owner's Go,
 * 2026-11-05, the relay saw no datagram at all.  Counted from ol_up. */
static struct
{
   unsigned tx, tx_ok, rx;
   unsigned tx_fail;                    /* every failed sendto (ENOBUFS: the send buffer is full) */
   int tx_rc, tx_errno;      /* the first failed sendto */
   int rx_errno;             /* the last recvfrom error that is not EAGAIN */
} D;

/* "t<ok>/<sent> e<rc>/<errno> r<recv>/<errno> p<port> m<free KB>" */
static void diag(char *why, unsigned cap, const char *what)
{
   snprintf(why, cap, "%s t%u/%u e%X/%d r%u/%d p%u m%uK", what, D.tx_ok, D.tx,
            (unsigned)D.tx_rc, D.tx_errno, D.rx, D.rx_errno, (unsigned)O.port,
            (unsigned)(sceKernelTotalFreeMemSize() >> 10));
}

static void evt(const char *fmt, ...)
{
   char b[320];
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

/* v5: the player's O on the LINKING screen (host.cancel; NULL = none). */
static int cancelled(char *why, unsigned why_cap)
{
   if (!g_ol_host->cancel || !g_ol_host->cancel())
      return 0;
   snprintf(why, why_cap, "cancelled");
   evt("online_error stage=cancel");
   return 1;
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
   int n;
   (void)ctx;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   /* sceNetInetSendto is declared returning size_t: an error is negative */
   n = (int)sceNetInetSendto(O.fd, buf, len, 0, (struct sockaddr *)&a, sizeof(a));
   if (ip == O.bcast[0] || ip == O.bcast[1])
   {
      /* a DISCOVER: not in the counts the failure text reads (a network
       * that refuses broadcasts must not look like "nothing left the PSP") */
      if (n < 0)
         evt("online_discover stage=sendto rc=0x%08X errno=%d", (unsigned)n,
             sceNetInetGetErrno());
      return 0;
   }
   D.tx++;
   if (n >= 0)
      D.tx_ok++;
   else
      D.tx_fail++;
   if (n < 0 && !D.tx_rc)
   {
      D.tx_rc = n;
      D.tx_errno = sceNetInetGetErrno();
      evt("online_error stage=sendto rc=0x%08X errno=%d", (unsigned)n, D.tx_errno);
   }
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
   if (n < 0)
   {
      int e = sceNetInetGetErrno();
      if (e != 11 && e != 35)           /* EAGAIN / EWOULDBLOCK: nothing yet */
         D.rx_errno = e;
   }
   if (n <= 0)
      return 0;
   D.rx++;
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
      *colon = 0;
   snprintf(O.name, sizeof(O.name), "%s", host);   /* home relay discovery */
   if (colon)
      *colon = ':';
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
         return rc;
      }
      O.resolver_up = 1;
   }
   buf = calloc(1, 1024);
   if (!buf)
      return -2;
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
      return rc < 0 ? rc : -1;
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
      O.shown = 0;
      O.show_us = 0;
      if (g_ol_host->chip)
         g_ol_host->chip(NULL);
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
   O.invite = 0;
   O.name[0] = 0;
   O.bcast[0] = O.bcast[1] = 0;
   memset(&D, 0, sizeof(D));     /* an invite skips to have_server */
   O.port = 0;
   {
      int nsym = inv_count(cfg->room);
      if (nsym == INV_SHORT || nsym == INV_LONG)
      {
         /* an invite code: a desktop host's address, no server to ask.  The
          * Join screen passes the code as typed (it is too small to check
          * one), so this is THE check, before any socket exists */
         uint8_t ip[4];
         unsigned port;
         int r = inv_decode(cfg->room, ip, &port);
         if (r != INV_OK)
         {
            snprintf(why, why_cap, "invite code does not check out");
            evt("online_error stage=invite rc=%d", r);
            return -1;
         }
         memcpy(&sip, ip, 4);
         sport = (uint16_t)port;
         O.cfg.room[0] = 0;               /* the host's home room */
         O.invite = 1;
         evt("online_invite ip=%u.%u.%u.%u port=%u", ip[0], ip[1], ip[2], ip[3], port);
         goto have_server;
      }
      /* otherwise nothing (Create: the server picks) or a room code, exactly
       * four letters: a code one symbol short of an invite must not become a
       * LOOKUP of its first four letters */
      if (nsym != 0)
      {
         const char *p = cfg->room;
         int ok = nsym == RL_CODE_LEN;
         for (; ok && *p; p++)
            ok = *p == ' ' || *p == '-' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
         if (!ok)
         {
            snprintf(why, why_cap, "a room code is 4 letters, an invite 8 or 11");
            evt("online_error stage=code n=%d", nsym);
            return -1;
         }
      }
   }
   {
      /* a room code: its first four letters, upper-cased */
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
   memset(&D, 0, sizeof(D));
   O.port = 0;
   if (cancelled(why, why_cap))
   {
      ol_down();
      return -1;
   }
   rc = resolve(&sip, &sport);
   if (rc != 0)
   {
      snprintf(why, why_cap, "server name not found 0x%08X m%uK", (unsigned)rc,
               (unsigned)(sceKernelTotalFreeMemSize() >> 10));
      ol_down();
      return -1;
   }
have_server:
   {
      const uint8_t *p = (const uint8_t *)&sip;
      evt("online_server ip=%u.%u.%u.%u port=%u", p[0], p[1], p[2], p[3], sport);
   }
   beat();
   if (cancelled(why, why_cap))       /* the lookup can take ~6 s */
   {
      ol_down();
      return -1;
   }
   O.fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
   if (O.fd < 0)
   {
      snprintf(why, why_cap, "socket 0x%08X e%d m%uK", (unsigned)O.fd,
               sceNetInetGetErrno(), (unsigned)(sceKernelTotalFreeMemSize() >> 10));
      evt("online_error stage=socket rc=%d", O.fd);
      ol_down();
      return -1;
   }
   sceNetInetSetsockopt(O.fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   sceNetInetSetsockopt(O.fd, SOL_SOCKET, SO_NONBLOCK, &opt, sizeof(opt));
   /* home relay discovery sends to the LAN broadcast addresses */
   sceNetInetSetsockopt(O.fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_port = htons(ONLINE_PORT);
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   if ((rc = sceNetInetBind(O.fd, (struct sockaddr *)&a, sizeof(a))) < 0)
   {
      evt("online_error stage=bind port=%d rc=0x%08X errno=%d", ONLINE_PORT,
          (unsigned)rc, sceNetInetGetErrno());
      /* the port is taken (another program on this network stack, or two
       * emulated consoles on one PC): any port will do -- the server and the
       * peer learn it from the datagrams, and the private candidate below
       * reads it back */
      a.sin_port = 0;
      if ((rc = sceNetInetBind(O.fd, (struct sockaddr *)&a, sizeof(a))) < 0)
      {
         snprintf(why, why_cap, "bind 0x%08X e%d", (unsigned)rc,
                  sceNetInetGetErrno());
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
   /* HOME RELAY DISCOVERY: 255.255.255.255 and this LAN's /24 broadcast
    * (most homes; the netmask is the EBOOT's, and one more datagram costs
    * nothing).  transport_relay skips it when the server is a LAN address
    * already, and matches an answer by the name or the public IP. */
   O.bcast[0] = 0xFFFFFFFFu;
   if (rc_cfg.priv_ip_be && rl_ip_lan(rc_cfg.priv_ip_be))
   {
      uint8_t b[4];
      memcpy(b, &rc_cfg.priv_ip_be, 4);
      b[3] = 255;
      memcpy(&O.bcast[1], b, 4);
   }
   rc_cfg.discover_bcast_be[0] = O.bcast[0];
   rc_cfg.discover_bcast_be[1] = O.bcast[1];
   rc_cfg.discover_name = O.name[0] ? O.name : NULL;
   rc_cfg.priv_port = O.port;
   rc_cfg.direct = O.cfg.direct;
   /* a room code is looked up first: a desktop host's code redirects to it */
   rc_cfg.lookup = !O.invite && O.cfg.room[0] != 0;
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
         if (st == RL_ST_FAILED)
            snprintf(why, why_cap, "%s", tx);
         else
            /* the counts say which: t0/N = nothing left the PSP (the
             * first failed sendto's rc/errno follow), tN/N r0 = sent but
             * nothing came back */
            diag(why, why_cap, O.invite ? "the host did not answer"
                               : is_host ? "no answer" : "no room/answer");
         evt("online_error stage=room state=%d why=\"%s\"", st, why);
         ol_down();
         return -1;
      }
      if (cancelled(why, why_cap))
      {
         ol_down();
         return -1;
      }
      beat();
      sceKernelDelayThread(2000);
   }
   {
      rl_client_stats s;
      relay_transport_stats(O.rt, &s);
      evt("online_up room=%s slot=%d host=%d mode=%d lan_relay=%u answers=%u",
          relay_transport_code(O.rt), relay_transport_slot(O.rt), is_host,
          relay_transport_mode(O.rt), s.lan_relay, s.disc_answers);
   }
   return 0;
}

static int ol_is_up(void) { return O.rt != NULL; }
static const char *ol_room(void) { return O.rt ? relay_transport_code(O.rt) : ""; }
static int ol_room_mode(void) { return O.rt ? relay_transport_mode(O.rt) : 0; }
static char ol_text(char *out, unsigned cap);
static int ol_slot(void) { return O.rt ? relay_transport_slot(O.rt) : -1; }

/* netdrv's recv, once per pump: the transport's, plus once a second the
 * session chip ("Online ABCD  Direct 12 ms") and a toast when the path
 * changes -- the owner sees at a glance whether the relay is in the way. */
static int ol_recv(void *ctx, uint8_t mac[6], void *buf, size_t cap)
{
   int n = O.recv(ctx, mac, buf, cap);
   uint64_t t = now_us();
   if (t >= O.show_us && g_ol_host->chip)
   {
      char k;
      O.show_us = t + 1000000u;
      k = ol_text(O.chip, sizeof(O.chip));
      if (k && k != O.shown)
      {
         g_ol_host->toast("%s", O.chip);
         evt("online_path shown=\"%s\"", O.chip);
      }
      if (k)
         O.shown = k;
      g_ol_host->chip(O.chip);
   }
   return n;
}

static void ol_iface(nd_transport *out)
{
   relay_transport_iface(O.rt, out);
   O.recv = out->recv;
   out->recv = ol_recv;
}
/* Not connected once the server has been silent for 30 s: a lobby host's
 * room is presumed gone (transport_relay keeps entering it again with the
 * same code), and the EBOOT says "Wireless group lost" instead of showing a
 * dead room. */
static int ol_connected(void)
{
   return O.rt != NULL && g_ol_host->wifi_is_up() &&
          relay_transport_server_silent_us(O.rt) < 30000000ull;
}

/* the chip text; 'D' direct, 'R' relay, 0 no peer yet */
static char ol_text(char *out, unsigned cap)
{
   rl_client_stats s;
   int j, p = 0, peer = -1;
   if (!cap)
      return 0;
   out[0] = 0;
   if (!O.rt)
      return 0;
   relay_transport_stats(O.rt, &s);
   for (j = 0; j < RL_SLOTS; j++)
      if (j != relay_transport_slot(O.rt) && s.path[j])
      {
         peer = j;
         p = s.path[j];
         break;
      }
   /* the ping: the direct round trip (PUNCH keep-alive every 500 ms), or
    * on the relay ours to the server twice (the peer's leg is about ours) */
   if (peer < 0)
   {
      snprintf(out, cap, "Online %s  waiting", relay_transport_code(O.rt));
      return 0;
   }
   if (p == 3)
   {
      snprintf(out, cap, "Online %s  Direct %u ms", relay_transport_code(O.rt),
               (unsigned)(s.direct_rtt_us[peer] / 1000));
      return 'D';
   }
   if (p == 2 && !s.fallbacks)
   {
      /* the first punch burst (on the relay meanwhile): no verdict yet */
      snprintf(out, cap, "Online %s  connecting", relay_transport_code(O.rt));
      return 0;
   }
   /* through a desktop host: "Via host" (the design mockup's word), so a
    * player knows a PC is in the path; the shared server: "Via relay" */
   snprintf(out, cap, "Online %s  %s %u ms", relay_transport_code(O.rt),
            O.invite || s.located == RL_LOC_HOST ? "Via host" : "Via relay",
            (unsigned)(s.relay_rtt_us / 1000 * 2));
   return 'R';
}

static void ol_status(char *out, unsigned cap) { (void)ol_text(out, cap); }

/* v6: the side lane.  Sent with the transport's own send_to, so it takes the
 * session's path (direct, or DATA through the relay); received datagrams are
 * sorted into the transport's side ring by netdrv's pump (ol_recv). */
static nd_transport O_tp;               /* the iface, for side_send */
static int ol_side_open(unsigned slots, const char magic[4])
{
   if (!O.rt)
      return -1;
   relay_transport_iface(O.rt, &O_tp);
   return relay_transport_side_open(O.rt, slots, magic);
}
static void ol_side_close(void)
{
   if (O.rt)
      relay_transport_side_close(O.rt);
}
static int ol_side_send(const uint8_t mac[6], const void *buf, size_t len)
{
   unsigned f0 = D.tx_fail;
   if (!O.rt || !O_tp.send_to || len > RL_SIDE_MAX)
      return -1;
   O_tp.send_to(O_tp.ctx, mac, buf, len);
   /* a refused sendto (the PSP's send buffer is full) is reported, so the
    * bulk sender keeps the datagram for its next step and slows down */
   return D.tx_fail != f0 ? -1 : 0;
}
static int ol_side_recv(void *buf, size_t cap)
{
   return O.rt ? relay_transport_side_recv(O.rt, buf, cap) : 0;
}
static unsigned ol_side_depth(void)
{
   return O.rt ? relay_transport_side_depth(O.rt) : 0u;
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
       "rtt_relay_ms=%u rtt_direct_ms=%u/%u bad=%u bursts=%u directs=%u stalls=%u "
       "located=%u invite=%d lan_relay=%u side_rx=%u side_drop=%u tx_fail=%u", at, s.nat, paths,
       (unsigned)s.tx, (unsigned)s.rx, (unsigned)s.tx_direct, (unsigned)s.rx_direct,
       (unsigned)s.tx_relay, (unsigned)s.rx_relay, (unsigned)s.punches_tx,
       (unsigned)s.punches_rx, (unsigned)s.acks_rx, (unsigned)s.fallbacks,
       (unsigned)(s.relay_rtt_us / 1000), (unsigned)(s.direct_rtt_us[0] / 1000),
       (unsigned)(s.direct_rtt_us[1] / 1000), (unsigned)s.rx_bad,
       (unsigned)s.bursts, (unsigned)s.directs, (unsigned)s.stalls,
       (unsigned)s.located, O.invite, (unsigned)s.lan_relay, (unsigned)s.side_rx,
       (unsigned)s.side_drop, D.tx_fail);
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
   g_api.slot = ol_slot;
   g_api.side_open = ol_side_open;
   g_api.side_close = ol_side_close;
   g_api.side_send = ol_side_send;
   g_api.side_recv = ol_side_recv;
   g_api.side_depth = ol_side_depth;
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
