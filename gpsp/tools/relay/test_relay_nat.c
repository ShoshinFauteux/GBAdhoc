/* test_relay_nat.c -- the relay core, two transport_relay clients and two real
 * netdrv sessions in one process, over a simulated internet with NAT boxes
 * whose behaviour is a parameter.  Deterministic (virtual time, seeded).
 * docs/ONLINE-TUNNEL.md section 5 ("Testing it without the internet", 2).
 *
 * Per case: the host creates a room, the guest joins it by code, netdrv's
 * session forms over whatever path the rendezvous gives, and each side sends
 * N reliable payloads that the other must receive complete and in order.
 * The case states which path it must end on (direct or relay), and the
 * mid-session cases drop the direct path and require the relay to take over
 * with the netdrv session intact.
 *
 *   cc -I../../netdrv -o test_relay_nat test_relay_nat.c ../../netdrv/relay_core.c \
 *      ../../netdrv/transport_relay.c ../../netdrv/netdrv.c && ./test_relay_nat
 */
#include "relay_core.h"
#include "transport_relay.h"
#include "netdrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- network */

enum { NAT_NONE = 0, NAT_CONE = 1, NAT_SYM = 2 };   /* CONE = EIM mapping,
                                                       port-restricted filter */
#define NMAP 64
#define QCAP 4096
#define MS 1000ull

typedef struct { uint32_t ip; uint16_t port; } ep_t;

typedef struct
{
   int used;
   ep_t priv, dst;              /* dst only for symmetric */
   uint16_t pub_port;
   ep_t allowed[16];
   int nallowed;
   uint64_t last;
} nmap_t;

typedef struct
{
   int type;
   uint32_t pub_ip;
   uint32_t lan;                /* 10.0.N.0/24 */
   uint16_t next_port;
   unsigned timeout_ms;
   nmap_t m[NMAP];
} nat_t;

typedef struct
{
   uint64_t at;
   ep_t src, dst;
   uint16_t len;
   uint8_t data[RL_MAX_DGRAM];
} pkt_t;

static pkt_t q[QCAP];
static int qn;
static uint64_t g_now;          /* us */
static unsigned g_lat_ms = 20;  /* one way, per internet leg */
static uint32_t g_rng = 12345;

static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static uint64_t now_us(void) { return g_now; }

static uint32_t ip4(int a, int b, int c, int d)
{
   uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
   uint32_t v;
   memcpy(&v, x, 4);
   return v;
}

#define SERVER_IP ip4(203, 0, 113, 1)
#define SERVER_PORT 27420

/* hosts: index 0 = host console, 1 = guest console */
typedef struct
{
   ep_t priv;                   /* its own address (public if NAT_NONE) */
   nat_t *nat;
   pkt_t inbox[256];
   int in_head, in_tail;
} host_t;

static host_t H[2];
static nat_t N[2];
static int g_same_lan;          /* both consoles behind N[0] */
static uint64_t g_host_dark_from, g_host_dark_to;   /* host 0 off the air */
static int host_dark(int h) { return h == 0 && g_now >= g_host_dark_from && g_now < g_host_dark_to; }
static rl_core *g_core;

static void enqueue(ep_t src, ep_t dst, const void *b, size_t n, unsigned delay_ms)
{
   pkt_t *p;
   if (qn >= QCAP || n > RL_MAX_DGRAM)
      return;
   p = &q[qn++];
   p->at = g_now + delay_ms * MS;
   p->src = src;
   p->dst = dst;
   p->len = (uint16_t)n;
   memcpy(p->data, b, n);
}

static int lan_of(uint32_t ip) { return ((const uint8_t *)&ip)[0] == 10; }

static nmap_t *nat_out(nat_t *n, ep_t priv, ep_t dst)
{
   int i, fr = -1;
   for (i = 0; i < NMAP; i++)
   {
      nmap_t *m = &n->m[i];
      if (m->used && g_now - m->last > n->timeout_ms * MS)
         m->used = 0;
      if (!m->used)
      {
         if (fr < 0)
            fr = i;
         continue;
      }
      if (m->priv.ip == priv.ip && m->priv.port == priv.port &&
          (n->type != NAT_SYM || (m->dst.ip == dst.ip && m->dst.port == dst.port)))
         return m;
   }
   if (fr < 0)
      return NULL;
   memset(&n->m[fr], 0, sizeof(nmap_t));
   n->m[fr].used = 1;
   n->m[fr].priv = priv;
   n->m[fr].dst = dst;
   n->m[fr].pub_port = n->next_port++;
   return &n->m[fr];
}

static void route(pkt_t *p);

/* a console sends: through its NAT (if any) onto the internet */
static void host_send(int h, ep_t dst, const void *b, size_t n)
{
   host_t *x = &H[h];
   ep_t src = x->priv;
   nat_t *nt = x->nat;
   if (host_dark(h))
      return;
   if (nt && lan_of(dst.ip))
   {
      /* LAN delivery: only inside this NAT's own /24 -- another home's
       * private address is unreachable (and may be a stranger's device) */
      if (((const uint8_t *)&dst.ip)[2] == ((const uint8_t *)&src.ip)[2])
         enqueue(src, dst, b, n, 2);
      return;
   }
   if (nt)
   {
      nmap_t *m = nat_out(nt, src, dst);
      int i, seen = 0;
      if (!m)
         return;
      m->last = g_now;
      for (i = 0; i < m->nallowed; i++)
         if (m->allowed[i].ip == dst.ip && m->allowed[i].port == dst.port)
            seen = 1;
      if (!seen && m->nallowed < 16)
         m->allowed[m->nallowed++] = dst;
      src.ip = nt->pub_ip;
      src.port = m->pub_port;
   }
   enqueue(src, dst, b, n, g_lat_ms);
}

static void deliver_host(int h, pkt_t *p)
{
   host_t *x = &H[h];
   int nx = (x->in_head + 1) % 256;
   if (host_dark(h))
      return;
   if (nx == x->in_tail)
      return;
   x->inbox[x->in_head] = *p;
   x->in_head = nx;
}

static void server_send(void *user, int sock, const rl_addr *to, const void *buf, size_t len)
{
   ep_t src = { SERVER_IP, (uint16_t)(SERVER_PORT + sock) };
   ep_t dst = { to->ip_be, to->port };
   (void)user;
   enqueue(src, dst, buf, len, g_lat_ms);
}

static void route(pkt_t *p)
{
   int h, i;
   if (p->dst.ip == SERVER_IP)
   {
      rl_addr from = { p->src.ip, p->src.port };
      if (p->dst.port == SERVER_PORT || p->dst.port == SERVER_PORT + 1)
         relay_core_input(g_core, p->dst.port - SERVER_PORT, &from, p->data,
                          p->len, g_now / 1000);
      return;
   }
   for (h = 0; h < 2; h++)
   {
      host_t *x = &H[h];
      if (x->priv.ip == p->dst.ip && x->priv.port == p->dst.port)
      {
         /* a LAN packet (or a public, un-NATed console) */
         if (x->nat && !lan_of(p->src.ip))
            continue;          /* the internet cannot reach a private address */
         deliver_host(h, p);
         return;
      }
   }
   for (h = 0; h < 2; h++)
   {
      nat_t *nt = &N[h];
      if (!nt->pub_ip || nt->pub_ip != p->dst.ip)
         continue;
      for (i = 0; i < NMAP; i++)
      {
         nmap_t *m = &nt->m[i];
         int k, ok = 0;
         if (!m->used || m->pub_port != p->dst.port)
            continue;
         if (g_now - m->last > nt->timeout_ms * MS)
         {
            m->used = 0;
            return;
         }
         for (k = 0; k < m->nallowed; k++)
            if (m->allowed[k].ip == p->src.ip && m->allowed[k].port == p->src.port)
               ok = 1;
         if (!ok)
            return;            /* port-restricted filtering */
         m->last = g_now;
         {
            int t;
            for (t = 0; t < 2; t++)
               if (H[t].priv.ip == m->priv.ip && H[t].priv.port == m->priv.port)
               {
                  pkt_t c = *p;
                  c.dst = m->priv;
                  deliver_host(t, &c);
               }
         }
         return;
      }
      return;
   }
}

static void net_step(void)
{
   int i = 0;
   while (i < qn)
   {
      if (q[i].at <= g_now)
      {
         pkt_t p = q[i];
         q[i] = q[--qn];
         route(&p);
      }
      else
         i++;
   }
}

/* --------------------------------------------------------- socket shims */

static int sk_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   ep_t d = { ip, port };
   host_send((int)(intptr_t)ctx, d, buf, len);
   return 0;
}

static int sk_recv(void *ctx, uint32_t *ip, uint16_t *port, void *buf, size_t cap)
{
   host_t *x = &H[(int)(intptr_t)ctx];
   pkt_t *p;
   if (x->in_tail == x->in_head)
      return 0;
   p = &x->inbox[x->in_tail];
   x->in_tail = (x->in_tail + 1) % 256;
   if (p->len > cap)
      return 0;
   *ip = p->src.ip;
   *port = p->src.port;
   memcpy(buf, p->data, p->len);
   return p->len;
}

static uint64_t core_rand(void *u) { (void)u; return ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 16) ^ rnd(); }

static int g_verbose;
static void core_log(void *u, const char *l) { (void)u; if (g_verbose) printf("  [srv] %s\n", l); }
static void cli_log(void *u, const char *l) { if (g_verbose) printf("  [%s] %s\n", (const char *)u, l); }

/* --------------------------------------------------------- netdrv apps */

typedef struct
{
   const char *name;
   int started, connected, stopped, lost;
   uint32_t next_expect, got, bad;
} app_t;

static void app_deliver(void *u, const void *buf, size_t len, uint8_t src)
{
   app_t *a = (app_t *)u;
   uint32_t v;
   (void)src;
   if (len != 8)
      return;
   memcpy(&v, buf, 4);
   if (v != a->next_expect)
      a->bad++;
   a->next_expect = v + 1;
   a->got++;
}
static void app_started(void *u, uint8_t id) { (void)id; ((app_t *)u)->started = 1; }
static int app_connected(void *u, uint8_t id) { (void)id; ((app_t *)u)->connected++; return 0; }
static void app_disconnected(void *u, uint8_t id) { (void)id; ((app_t *)u)->lost++; }
static void app_stopped(void *u, int r) { (void)r; ((app_t *)u)->stopped++; }

/* ---------------------------------------------------------------- cases */

typedef struct
{
   const char *name;
   int nat0, nat1, same_lan;
   int expect_direct;           /* 1 = must end DIRECT, 0 = must end RELAY */
   uint64_t drop_direct_at_ms;  /* guest drops direct traffic from then on */
   unsigned nat_timeout_ms;
   int flush_nat_at_ms;         /* host's NAT forgets all mappings then */
} case_t;

static int run_case(const case_t *c)
{
   rl_core_cfg cc;
   rl_sock sk[2];
   rl_client_cfg cfg[2];
   relay_transport *rt[2];
   nd_transport tp[2];
   nd_callbacks cb[2];
   nd_config nc;
   netdrv *nd[2] = { NULL, NULL };
   app_t app[2];
   rl_client_stats st[2];
   int i, ok = 1, sent[2] = { 0, 0 };
   const int N_PAY = 600;
   uint64_t t_end;

   memset(q, 0, sizeof(q));
   qn = 0;
   g_now = 1000000;
   memset(H, 0, sizeof(H));
   memset(N, 0, sizeof(N));
   g_same_lan = c->same_lan;
   for (i = 0; i < 2; i++)
   {
      N[i].type = i ? c->nat1 : c->nat0;
      N[i].pub_ip = ip4(198, 51, 100, 10 + i);
      N[i].next_port = (uint16_t)(40000 + 1000 * i);
      N[i].timeout_ms = c->nat_timeout_ms ? c->nat_timeout_ms : 30000;
   }
   for (i = 0; i < 2; i++)
   {
      int behind = c->same_lan ? 0 : i;
      int t = c->same_lan ? c->nat0 : (i ? c->nat1 : c->nat0);
      if (t == NAT_NONE)
      {
         H[i].priv.ip = ip4(192, 0, 2, 10 + i);   /* a public address */
         H[i].nat = NULL;
      }
      else
      {
         H[i].priv.ip = ip4(10, 0, behind, 20 + i);
         H[i].nat = &N[behind];
      }
      H[i].priv.port = 27500;
   }

   memset(&cc, 0, sizeof(cc));
   cc.send = server_send;
   cc.random64 = core_rand;
   cc.log = core_log;
   g_core = relay_core_create(&cc);

   for (i = 0; i < 2; i++)
   {
      sk[i].ctx = (void *)(intptr_t)i;
      sk[i].send = sk_send;
      sk[i].recv = sk_recv;
      memset(&cfg[i], 0, sizeof(cfg[i]));
      cfg[i].server_ip_be = SERVER_IP;
      cfg[i].server_port = SERVER_PORT;
      cfg[i].is_host = i == 0;
      strcpy(cfg[i].code, "TEST");
      memcpy(cfg[i].build, "gbadhoc-test-b1", 15);
      cfg[i].priv_ip_be = H[i].priv.ip;
      cfg[i].priv_port = H[i].priv.port;
      cfg[i].direct = 1;
      cfg[i].now_us = now_us;
      cfg[i].log = cli_log;
      cfg[i].log_user = (void *)(i ? "guest" : "host");
      if (i == 1 && c->drop_direct_at_ms)
         cfg[i].drop_direct_after_us = c->drop_direct_at_ms * MS;
      rt[i] = relay_transport_create(&sk[i], &cfg[i]);
   }

   /* the handshake */
   for (t_end = g_now + 10000 * MS; g_now < t_end; g_now += MS)
   {
      net_step();
      relay_core_tick(g_core, g_now / 1000);
      for (i = 0; i < 2; i++)
         relay_transport_poll(rt[i]);
      if (relay_transport_poll(rt[0]) == RL_ST_IN_ROOM &&
          relay_transport_poll(rt[1]) == RL_ST_IN_ROOM)
         break;
   }
   if (relay_transport_poll(rt[0]) != RL_ST_IN_ROOM ||
       relay_transport_poll(rt[1]) != RL_ST_IN_ROOM)
   {
      printf("FAIL %s: room never formed (%d/%d)\n", c->name,
             relay_transport_poll(rt[0]), relay_transport_poll(rt[1]));
      return 0;
   }

   memset(&nc, 0, sizeof(nc));
   nc.protocol = "gpSP v1.0";
   for (i = 0; i < 2; i++)
   {
      memset(&app[i], 0, sizeof(app[i]));
      app[i].name = i ? "guest" : "host";
      memset(&cb[i], 0, sizeof(cb[i]));
      cb[i].user = &app[i];
      cb[i].deliver = app_deliver;
      cb[i].session_started = app_started;
      cb[i].peer_connected = app_connected;
      cb[i].peer_disconnected = app_disconnected;
      cb[i].session_stopped = app_stopped;
      relay_transport_iface(rt[i], &tp[i]);
      nc.join_nonce = i ? 0xBEEF : 0xF00D;
      nd[i] = netdrv_create(&tp[i], &cb[i], &nc);
   }
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);

   /* the session: one payload a side every 16 ms frame */
   for (t_end = g_now + 40000 * MS; g_now < t_end; g_now += MS)
   {
      net_step();
      relay_core_tick(g_core, g_now / 1000);
      if (c->flush_nat_at_ms && g_now == 1000000 + (uint64_t)c->flush_nat_at_ms * MS)
         memset(N[0].m, 0, sizeof(N[0].m));
      for (i = 0; i < 2; i++)
      {
         netdrv_pump(nd[i], g_now);
         if (netdrv_active(nd[i]) && netdrv_peer_count(nd[i]) > 0 &&
             sent[i] < N_PAY && (g_now / MS) % 16 == (unsigned)i * 8)
         {
            uint8_t p[8];
            uint32_t v = (uint32_t)sent[i];
            memcpy(p, &v, 4);
            memset(p + 4, 0xA5, 4);
            if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, p, sizeof(p),
                            i ? 0 : 1) == 0)
               sent[i]++;
         }
      }
      if (app[0].got >= (uint32_t)N_PAY && app[1].got >= (uint32_t)N_PAY)
         break;
   }

   for (i = 0; i < 2; i++)
      relay_transport_stats(rt[i], &st[i]);
   {
      int peer0 = 1, peer1 = 0;
      int path0 = st[0].path[peer0], path1 = st[1].path[peer1];
      int direct = path0 == 3 && path1 == 3;
      int relayed = (path0 == 1 || path0 == 4) && (path1 == 1 || path1 == 4);
      if (app[0].got != (uint32_t)N_PAY || app[1].got != (uint32_t)N_PAY ||
          app[0].bad || app[1].bad)
      {
         printf("FAIL %s: payloads host got %u bad %u, guest got %u bad %u\n",
                c->name, app[0].got, app[0].bad, app[1].got, app[1].bad);
         ok = 0;
      }
      if (app[0].lost || app[1].lost || app[0].stopped || app[1].stopped)
      {
         printf("FAIL %s: netdrv session lost (lost %d/%d stopped %d/%d)\n",
                c->name, app[0].lost, app[1].lost, app[0].stopped, app[1].stopped);
         ok = 0;
      }
      if (c->expect_direct ? !direct : !relayed)
      {
         printf("FAIL %s: path host=%d guest=%d, want %s\n", c->name, path0,
                path1, c->expect_direct ? "direct (3)" : "relay (1/4)");
         ok = 0;
      }
      if (ok)
         printf("ok   %-34s nat=%u/%u path=%s/%s direct_rx=%u/%u relay_rx=%u/%u "
                "fallbacks=%u/%u rtt_direct=%ums rtt_relay=%ums\n",
                c->name, st[0].nat, st[1].nat,
                path0 == 3 ? "direct" : path0 == 4 ? "fellback" : "relay",
                path1 == 3 ? "direct" : path1 == 4 ? "fellback" : "relay",
                st[0].rx_direct, st[1].rx_direct, st[0].rx_relay, st[1].rx_relay,
                st[0].fallbacks, st[1].fallbacks,
                st[0].direct_rtt_us[1] / 1000, st[0].relay_rtt_us / 1000);
   }
   for (i = 0; i < 2; i++)
   {
      netdrv_destroy(nd[i]);
      relay_transport_destroy(rt[i]);
   }
   relay_core_destroy(g_core);
   return ok;
}

/* ------------------------------------------------------- lobby cases */

/* A host waits ALONE in its room (as a PSP does while the code is read out to
 * a friend), behind a carrier NAT whose mappings expire after nat_timeout_ms
 * and are re-made on a new port every flush_every_ms, optionally off the air
 * (dark) for longer than the server's 20 s slot timeout, under the server's
 * lobby limit lobby_ms.  The guest joins by the SAME code at join_at_ms and
 * both sides must exchange N payloads. */
typedef struct
{
   const char *name;
   int nat0;
   unsigned nat_timeout_ms, flush_every_ms, lobby_ms;
   unsigned dark_at_ms, dark_ms, join_at_ms;
   int want_reenter;
} lobby_t;

static int run_lobby(const lobby_t *c)
{
   rl_core_cfg cc;
   rl_sock sk[2];
   rl_client_cfg cfg[2];
   relay_transport *rt[2] = { NULL, NULL };
   nd_transport tp[2];
   nd_callbacks cb[2];
   nd_config nc;
   netdrv *nd[2] = { NULL, NULL };
   app_t app[2];
   rl_client_stats st;
   int i, ok = 1, sent[2] = { 0, 0 };
   const int N_PAY = 300;
   uint64_t t0, t_end, last_flush;
   char code0[RL_CODE_LEN + 1] = "";

   memset(q, 0, sizeof(q));
   qn = 0;
   g_now = 1000000;
   t0 = g_now;
   memset(H, 0, sizeof(H));
   memset(N, 0, sizeof(N));
   g_same_lan = 0;
   for (i = 0; i < 2; i++)
   {
      N[i].type = i ? NAT_CONE : c->nat0;
      N[i].pub_ip = ip4(198, 51, 100, 10 + i);
      N[i].next_port = (uint16_t)(40000 + 1000 * i);
      N[i].timeout_ms = i ? 30000 : c->nat_timeout_ms;
      H[i].priv.ip = ip4(10, 0, i, 20 + i);
      H[i].priv.port = 27500;
      H[i].nat = &N[i];
   }
   g_host_dark_from = t0 + (uint64_t)c->dark_at_ms * MS;
   g_host_dark_to = g_host_dark_from + (uint64_t)c->dark_ms * MS;
   memset(&cc, 0, sizeof(cc));
   cc.send = server_send;
   cc.random64 = core_rand;
   cc.log = core_log;
   cc.lobby_ms = c->lobby_ms;
   g_core = relay_core_create(&cc);
   memset(&nc, 0, sizeof(nc));
   nc.protocol = "gpSP v1.0";
   for (i = 0; i < 2; i++)
   {
      sk[i].ctx = (void *)(intptr_t)i;
      sk[i].send = sk_send;
      sk[i].recv = sk_recv;
      memset(&cfg[i], 0, sizeof(cfg[i]));
      cfg[i].server_ip_be = SERVER_IP;
      cfg[i].server_port = SERVER_PORT;
      cfg[i].is_host = i == 0;
      strcpy(cfg[i].code, "ZAZA");
      memcpy(cfg[i].build, "gbadhoc-test-b1", 15);
      cfg[i].priv_ip_be = H[i].priv.ip;
      cfg[i].priv_port = H[i].priv.port;
      cfg[i].direct = 1;
      cfg[i].now_us = now_us;
      cfg[i].log = cli_log;
      cfg[i].log_user = (void *)(i ? "guest" : "host");
      memset(&app[i], 0, sizeof(app[i]));
      app[i].name = i ? "guest" : "host";
      memset(&cb[i], 0, sizeof(cb[i]));
      cb[i].user = &app[i];
      cb[i].deliver = app_deliver;
      cb[i].session_started = app_started;
      cb[i].peer_connected = app_connected;
      cb[i].peer_disconnected = app_disconnected;
      cb[i].session_stopped = app_stopped;
   }
   rt[0] = relay_transport_create(&sk[0], &cfg[0]);
   last_flush = g_now;
   t_end = t0 + ((uint64_t)c->join_at_ms + 40000) * MS;
   for (; g_now < t_end; g_now += MS)
   {
      net_step();
      relay_core_tick(g_core, g_now / 1000);
      if (c->flush_every_ms && g_now - last_flush >= (uint64_t)c->flush_every_ms * MS)
      {
         memset(N[0].m, 0, sizeof(N[0].m));   /* carrier NAT: a new mapping */
         last_flush = g_now;
      }
      if (!nd[0])
      {
         if (relay_transport_poll(rt[0]) == RL_ST_IN_ROOM)
         {
            snprintf(code0, sizeof(code0), "%s", relay_transport_code(rt[0]));
            relay_transport_iface(rt[0], &tp[0]);
            nc.join_nonce = 0xF00D;
            nd[0] = netdrv_create(&tp[0], &cb[0], &nc);
            netdrv_host(nd[0]);
         }
         continue;
      }
      if (!rt[1] && g_now >= t0 + (uint64_t)c->join_at_ms * MS)
         rt[1] = relay_transport_create(&sk[1], &cfg[1]);
      if (rt[1] && !nd[1] && relay_transport_poll(rt[1]) == RL_ST_IN_ROOM)
      {
         relay_transport_iface(rt[1], &tp[1]);
         nc.join_nonce = 0xBEEF;
         nd[1] = netdrv_create(&tp[1], &cb[1], &nc);
         netdrv_join(nd[1]);
      }
      for (i = 0; i < 2; i++)
      {
         if (!nd[i])
            continue;
         netdrv_pump(nd[i], g_now);
         if (netdrv_active(nd[i]) && netdrv_peer_count(nd[i]) > 0 &&
             sent[i] < N_PAY && (g_now / MS) % 16 == (unsigned)i * 8)
         {
            uint8_t p[8];
            uint32_t v = (uint32_t)sent[i];
            memcpy(p, &v, 4);
            memset(p + 4, 0xA5, 4);
            if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, p, sizeof(p),
                            i ? 0 : 1) == 0)
               sent[i]++;
         }
      }
      if (app[0].got >= (uint32_t)N_PAY && app[1].got >= (uint32_t)N_PAY)
         break;
   }
   relay_transport_stats(rt[0], &st);
   if (!nd[0] || strcmp(code0, "ZAZA") || strcmp(relay_transport_code(rt[0]), "ZAZA"))
   {
      printf("FAIL %s: host room code %s\n", c->name, relay_transport_code(rt[0]));
      ok = 0;
   }
   if (app[0].got != (uint32_t)N_PAY || app[1].got != (uint32_t)N_PAY ||
       app[0].bad || app[1].bad)
   {
      printf("FAIL %s: payloads host got %u, guest got %u (join at %u s)\n",
             c->name, app[0].got, app[1].got, c->join_at_ms / 1000);
      ok = 0;
   }
   if (c->want_reenter ? !st.reenters : st.reenters != 0)
   {
      printf("FAIL %s: reenters=%u\n", c->name, st.reenters);
      ok = 0;
   }
   if (ok)
      printf("ok   %-36s joined at %us, reenters=%u keeps=%u/%u\n", c->name,
             c->join_at_ms / 1000, st.reenters, st.keeps_tx, st.keeps_rx);
   for (i = 0; i < 2; i++)
   {
      if (nd[i])
         netdrv_destroy(nd[i]);
      if (rt[i])
         relay_transport_destroy(rt[i]);
   }
   relay_core_destroy(g_core);
   g_host_dark_from = g_host_dark_to = 0;
   return ok;
}

int main(int argc, char **argv)
{
   static const case_t cases[] = {
      { "cone x cone: direct",              NAT_CONE, NAT_CONE, 0, 1, 0, 0, 0 },
      { "public x cone: direct",            NAT_NONE, NAT_CONE, 0, 1, 0, 0, 0 },
      { "cone x symmetric: relay",          NAT_CONE, NAT_SYM,  0, 0, 0, 0, 0 },
      { "symmetric x symmetric: relay",     NAT_SYM,  NAT_SYM,  0, 0, 0, 0, 0 },
      { "same LAN behind one NAT: direct",  NAT_CONE, NAT_CONE, 1, 1, 0, 0, 0 },
      { "direct dies mid-session: relay",   NAT_CONE, NAT_CONE, 0, 0, 5000, 0, 0 },
      { "host NAT forgets: relay",          NAT_CONE, NAT_CONE, 0, 0, 0, 0, 6000 },
      { "short NAT timeout (5 s): direct",  NAT_CONE, NAT_CONE, 0, 1, 0, 5000, 0 },
   };
   static const lobby_t lobbies[] = {
      /* name                                   nat      nat_to flush  lobby  dark@  dark   join@   reenter */
      { "lobby: carrier NAT 30 s, join 5 min",  NAT_SYM, 30000, 0,     0,     0,     0,     300000, 0 },
      { "lobby: NAT re-maps every 45 s",        NAT_SYM, 30000, 45000, 0,     0,     0,     200000, 0 },
      { "lobby: 25 s off the air at 40 s",      NAT_SYM, 30000, 0,     0,     40000, 25000, 120000, 1 },
      { "lobby: server limit 60 s, join 3 min", NAT_SYM, 30000, 0,     60000, 0,     0,     180000, 1 },
   };
   unsigned i, fails = 0, n = 0;
   g_verbose = argc > 1 && !strcmp(argv[1], "-v");
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++, n++)
      if (!run_case(&cases[i]))
         fails++;
   for (i = 0; i < sizeof(lobbies) / sizeof(lobbies[0]); i++, n++)
      if (!run_lobby(&lobbies[i]))
         fails++;
   printf("%s: %u/%u cases\n", fails ? "FAILED" : "PASS", n - fails, n);
   return fails ? 1 : 0;
}
