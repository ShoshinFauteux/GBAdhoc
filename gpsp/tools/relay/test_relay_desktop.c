/* test_relay_desktop.c -- a desktop host (GBAdhoc XMB's relay: relay_core with
 * a home code, and relay_dirclient registering it) behind a home router with
 * UPnP port forwards, the shared server as a directory only, and two PSPs
 * (transport_relay + a real netdrv session each) behind NATs whose behaviour
 * is a parameter.  One process, virtual time, seeded: deterministic.
 * docs/ONLINE-TUNNEL.md, "Desktop hosts and discovery".
 *
 * Per case the PSPs find the desktop by its four-letter code (LOOKUP at the
 * directory) or by its eight-character invite code (no server at all), form
 * a room on the desktop, and send 600 reliable payloads each way.  Checked:
 * the payloads (complete, in order, no session loss), the path (direct where
 * both NATs allow it, through the desktop otherwise), that the directory
 * forwarded no game traffic, and the directory's bytes per session.
 *
 *   tools/relay/build.sh OUT && OUT/test_relay_desktop [-v]
 */
#include "relay_core.h"
#include "relay_dirclient.h"
#include "transport_relay.h"
#include "invite_code.h"
#include "netdrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NAT_NONE = 0, NAT_CONE = 1, NAT_SYM = 2 };
#define NMAP 64
#define QCAP 8192
#define MS 1000ull

typedef struct { uint32_t ip; uint16_t port; } ep_t;

typedef struct
{
   int used;
   ep_t priv, dst;
   uint16_t pub_port;
   ep_t allowed[16];
   int nallowed;
   uint64_t last;
} nmap_t;

typedef struct
{
   int type;
   uint32_t pub_ip;
   int lan;                     /* 10.0.<lan>.0/24 */
   uint16_t next_port;
   nmap_t m[NMAP];
   int nfwd;                    /* UPnP: pub_port -> priv, no filtering */
   uint16_t fwd_port[2];
   ep_t fwd_to[2];
} nat_t;

typedef struct
{
   uint64_t at;
   ep_t src, dst;
   int from_inside;             /* NAT index the packet left through, -1 */
   uint16_t len;
   uint8_t data[RL_MAX_DGRAM];
} pkt_t;

static pkt_t q[QCAP];
static int qn;
static uint64_t g_now;
static uint32_t g_rng = 4242;
static int g_verbose;

static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static uint64_t now_us(void) { return g_now; }

static uint32_t ip4(int a, int b, int c, int d)
{
   uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
   uint32_t v;
   memcpy(&v, x, 4);
   return v;
}
static int octet(uint32_t ip, int i) { return ((const uint8_t *)&ip)[i]; }

#define DIR_IP ip4(203, 0, 113, 1)
#define PORT 27420

/* entities: 0, 1 = PSPs; 2 = the desktop (sockets PORT and PORT + 1) */
typedef struct
{
   ep_t priv;
   int nat;                     /* -1 = a public address */
   pkt_t inbox[256];
   int in_head, in_tail;
} ent_t;

static ent_t E[3];
static nat_t N[3];
static int g_nnat;
static rl_core *g_dir, *g_desk;
static rl_dirc g_dc;
static int g_hairpin;           /* 1 = routers loop their public address back */
static uint32_t g_dir_fwd_seen;

static void enqueue(ep_t src, ep_t dst, const void *b, size_t n, unsigned delay_ms, int inside)
{
   pkt_t *p;
   if (qn >= QCAP || n > RL_MAX_DGRAM)
      return;
   p = &q[qn++];
   p->at = g_now + delay_ms * MS;
   p->src = src;
   p->dst = dst;
   p->from_inside = inside;
   p->len = (uint16_t)n;
   memcpy(p->data, b, n);
}

static nmap_t *nat_out(nat_t *n, ep_t priv, ep_t dst)
{
   int i, fr = -1;
   for (i = 0; i < NMAP; i++)
   {
      nmap_t *m = &n->m[i];
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

static int is_lan(uint32_t ip) { return octet(ip, 0) == 10; }

/* an entity sends from priv endpoint `src` */
static void ent_send(int e, ep_t src, ep_t dst, const void *b, size_t n)
{
   ent_t *x = &E[e];
   if (x->nat >= 0)
   {
      nat_t *nt = &N[x->nat];
      int i, fw = -1;
      if (is_lan(dst.ip))
      {
         if (octet(dst.ip, 2) == nt->lan)
            enqueue(src, dst, b, n, 2, -1);
         return;
      }
      if (dst.ip == nt->pub_ip && !g_hairpin)
         return;                         /* no NAT loopback */
      for (i = 0; i < nt->nfwd; i++)
         if (nt->fwd_to[i].ip == src.ip && nt->fwd_to[i].port == src.port)
            fw = i;
      if (fw >= 0)
         src.port = nt->fwd_port[fw];    /* a static forward maps outbound too */
      else
      {
         nmap_t *m = nat_out(nt, src, dst);
         int seen = 0;
         if (!m)
            return;
         m->last = g_now;
         for (i = 0; i < m->nallowed; i++)
            if (m->allowed[i].ip == dst.ip && m->allowed[i].port == dst.port)
               seen = 1;
         if (!seen && m->nallowed < 16)
            m->allowed[m->nallowed++] = dst;
         src.port = m->pub_port;
      }
      src.ip = nt->pub_ip;
      enqueue(src, dst, b, n, dst.ip == nt->pub_ip ? 2 : 20, x->nat);
      return;
   }
   enqueue(src, dst, b, n, 20, -1);
}

static void deliver(int e, pkt_t *p)
{
   ent_t *x = &E[e];
   if (e == 2)
   {
      rl_addr from = { p->src.ip, p->src.port };
      int sock = p->dst.port - PORT;
      if (sock < 0 || sock > 1)
         return;
      if (rl_dirc_input(&g_dc, p->src.ip, p->src.port, p->data, p->len, g_now / 1000))
         return;
      relay_core_input(g_desk, sock, &from, p->data, p->len, g_now / 1000);
      return;
   }
   {
      int nx = (x->in_head + 1) % 256;
      if (nx == x->in_tail)
         return;
      x->inbox[x->in_head] = *p;
      x->in_head = nx;
   }
}

static int ent_at(ep_t d)
{
   int e;
   for (e = 0; e < 3; e++)
      if (E[e].priv.ip == d.ip && (e != 2 || d.port == PORT || d.port == PORT + 1) &&
          (e == 2 || E[e].priv.port == d.port))
         return e;
   return -1;
}

static void route(pkt_t *p)
{
   int e, i, k;
   if (p->dst.ip == DIR_IP)
   {
      rl_addr from = { p->src.ip, p->src.port };
      if (p->dst.port == PORT || p->dst.port == PORT + 1)
         relay_core_input(g_dir, p->dst.port - PORT, &from, p->data, p->len, g_now / 1000);
      return;
   }
   e = ent_at(p->dst);
   if (e >= 0)
   {
      if (E[e].nat >= 0 && !is_lan(p->src.ip))
         return;                         /* the internet cannot reach a LAN address */
      deliver(e, p);
      return;
   }
   for (i = 0; i < g_nnat; i++)
   {
      nat_t *nt = &N[i];
      if (nt->pub_ip != p->dst.ip)
         continue;
      for (k = 0; k < nt->nfwd; k++)
         if (nt->fwd_port[k] == p->dst.port)
         {
            pkt_t c = *p;
            c.dst = nt->fwd_to[k];
            e = ent_at(c.dst);
            if (e >= 0)
               deliver(e, &c);
            return;
         }
      for (k = 0; k < NMAP; k++)
      {
         nmap_t *m = &nt->m[k];
         int j, ok = 0;
         if (!m->used || m->pub_port != p->dst.port)
            continue;
         for (j = 0; j < m->nallowed; j++)
            if (m->allowed[j].ip == p->src.ip && m->allowed[j].port == p->src.port)
               ok = 1;
         if (!ok)
            return;
         m->last = g_now;
         {
            pkt_t c = *p;
            c.dst = m->priv;
            e = ent_at(c.dst);
            if (e >= 0)
               deliver(e, &c);
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

/* ---- shims ---- */
static int sk_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   int e = (int)(intptr_t)ctx;
   ep_t d = { ip, port };
   ent_send(e, E[e].priv, d, buf, len);
   return 0;
}

static int sk_recv(void *ctx, uint32_t *ip, uint16_t *port, void *buf, size_t cap)
{
   ent_t *x = &E[(int)(intptr_t)ctx];
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

static void dir_send(void *u, int sock, const rl_addr *to, const void *buf, size_t len)
{
   ep_t s = { DIR_IP, (uint16_t)(PORT + sock) }, d = { to->ip_be, to->port };
   (void)u;
   if (len > RL_HDR && ((const uint8_t *)buf)[5] == RL_T_DATA)
      g_dir_fwd_seen++;
   enqueue(s, d, buf, len, 20, -1);
}

static void desk_send(void *u, int sock, const rl_addr *to, const void *buf, size_t len)
{
   ep_t s = { E[2].priv.ip, (uint16_t)(PORT + sock) }, d = { to->ip_be, to->port };
   (void)u;
   ent_send(2, s, d, buf, len);
}

static void dc_send(void *u, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   ep_t s = { E[2].priv.ip, PORT }, d = { ip, port };
   (void)u;
   ent_send(2, s, d, buf, len);
}

static uint64_t core_rand(void *u) { (void)u; return ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 16) ^ rnd(); }
static void lg(void *u, const char *l) { if (g_verbose) printf("  [%s] %s\n", (const char *)u, l); }

/* ---- netdrv apps ---- */
typedef struct { int started, connected, stopped, lost; uint32_t next_expect, got, bad; } app_t;
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

/* ---- cases ---- */
enum { BY_DIR = 0, BY_INVITE = 1 };
enum { W_OWN = 0, W_DESKHOME = 1 };   /* where a PSP lives */

typedef struct
{
   const char *name;
   int nat0, nat1;              /* each PSP's own router (W_OWN) */
   int where0, where1;          /* W_DESKHOME: behind the desktop's router */
   int by0, by1;
   int expect_direct;
   int desk_nat;                /* the desktop's router: NAT_CONE (+UPnP) or NAT_NONE */
   int hairpin;
   int both_join;               /* both PSPs "Join with code" (the mockup's flow): slot 0 hosts */
} case_t;

static void put_ent(int e, int where, int nattype, int own_nat)
{
   if (where == W_DESKHOME)
   {
      E[e].nat = 2;
      E[e].priv.ip = ip4(10, 0, 9, 30 + e);
   }
   else if (nattype == NAT_NONE)
   {
      E[e].nat = -1;
      E[e].priv.ip = ip4(192, 0, 2, 10 + e);
   }
   else
   {
      N[own_nat].type = nattype;
      N[own_nat].pub_ip = ip4(198, 51, 100, 10 + own_nat);
      N[own_nat].lan = own_nat;
      N[own_nat].next_port = (uint16_t)(40000 + 1000 * own_nat);
      E[e].nat = own_nat;
      E[e].priv.ip = ip4(10, 0, own_nat, 20 + e);
   }
   E[e].priv.port = 27500;
}

static int run_case(const case_t *c)
{
   rl_core_cfg cc;
   rl_dirc_cfg dcfg;
   rl_sock sk[2];
   rl_client_cfg cfg[2];
   relay_transport *rt[2];
   nd_transport tp[2];
   nd_callbacks cb[2];
   nd_config nc;
   netdrv *nd[2];
   app_t app[2];
   rl_client_stats st[2];
   rl_core_stats ds, ks;
   char invite[16];
   uint8_t pubip[4];
   uint32_t desk_pub;
   int i, hs, ok = 1, sent[2] = { 0, 0 };
   const int N_PAY = 600;
   uint64_t t_end, t_reg;

   memset(q, 0, sizeof(q));
   qn = 0;
   g_now = 1000000;
   memset(E, 0, sizeof(E));
   memset(N, 0, sizeof(N));
   g_nnat = 3;
   g_hairpin = c->hairpin;
   g_dir_fwd_seen = 0;
   /* the desktop's home: router 2, UPnP forwards PORT and PORT + 1 */
   E[2].priv.port = PORT;
   if (c->desk_nat == NAT_NONE)
   {
      E[2].nat = -1;
      E[2].priv.ip = ip4(192, 0, 2, 50);
      desk_pub = E[2].priv.ip;
   }
   else
   {
      N[2].type = c->desk_nat;
      N[2].pub_ip = desk_pub = ip4(198, 51, 100, 99);
      N[2].lan = 9;
      N[2].next_port = 45000;
      N[2].nfwd = 2;
      E[2].nat = 2;
      E[2].priv.ip = ip4(10, 0, 9, 2);
      for (i = 0; i < 2; i++)
      {
         N[2].fwd_port[i] = (uint16_t)(PORT + i);
         N[2].fwd_to[i].ip = E[2].priv.ip;
         N[2].fwd_to[i].port = (uint16_t)(PORT + i);
      }
   }
   put_ent(0, c->where0, c->nat0, 0);
   put_ent(1, c->where1, c->nat1, 1);

   memset(&cc, 0, sizeof(cc));
   cc.random64 = core_rand;
   cc.log = lg;
   cc.send = dir_send;
   cc.directory = 1;
   cc.user = (void *)"dir";
   g_dir = relay_core_create(&cc);
   cc.send = desk_send;
   cc.directory = 0;
   cc.max_rooms = 16;
   cc.user = (void *)"desk";
   g_desk = relay_core_create(&cc);
   relay_core_set_home(g_desk, "QWER");          /* a local pick, asked for */

   memset(&dcfg, 0, sizeof(dcfg));
   dcfg.dir_ip_be = DIR_IP;
   dcfg.dir_port = PORT;
   dcfg.adv_port = PORT;
   dcfg.priv_ip_be = E[2].priv.ip;
   dcfg.priv_port = PORT;
   strcpy(dcfg.want, "QWER");
   dcfg.send = dc_send;
   dcfg.log = lg;
   dcfg.user = (void *)"dirc";
   rl_dirc_start(&g_dc, &dcfg, g_now / 1000);
   for (t_end = g_now + 3000 * MS; g_now < t_end && g_dc.state != DC_REGISTERED; g_now += MS)
   {
      net_step();
      rl_dirc_tick(&g_dc, g_now / 1000);
   }
   if (g_dc.state != DC_REGISTERED || strcmp(g_dc.code, "QWER"))
   {
      printf("FAIL %s: the desktop never registered (state %d code %s)\n", c->name,
             g_dc.state, g_dc.code);
      return 0;
   }
   t_reg = g_now;
   memcpy(pubip, &desk_pub, 4);
   inv_encode(pubip, PORT, 1, invite);

   for (i = 0; i < 2; i++)
   {
      int by = i ? c->by1 : c->by0;
      sk[i].ctx = (void *)(intptr_t)i;
      sk[i].send = sk_send;
      sk[i].recv = sk_recv;
      memset(&cfg[i], 0, sizeof(cfg[i]));
      if (by == BY_DIR)
      {
         cfg[i].server_ip_be = DIR_IP;
         cfg[i].server_port = PORT;
         strcpy(cfg[i].code, g_dc.code);
         cfg[i].lookup = 1;
      }
      else
      {
         uint8_t ip[4];
         unsigned port;
         if (inv_decode(invite, ip, &port) != INV_OK)
         {
            printf("FAIL %s: invite %s does not decode\n", c->name, invite);
            return 0;
         }
         memcpy(&cfg[i].server_ip_be, ip, 4);
         cfg[i].server_port = (uint16_t)port;
         cfg[i].code[0] = 0;                     /* the host's home room */
      }
      cfg[i].is_host = i == 0 && !c->both_join;
      memcpy(cfg[i].build, "gbadhoc-test-b1", 15);
      cfg[i].priv_ip_be = E[i].priv.ip;
      cfg[i].priv_port = E[i].priv.port;
      cfg[i].direct = 1;
      cfg[i].now_us = now_us;
      cfg[i].log = lg;
      cfg[i].log_user = (void *)(i ? "guest" : "host");
      rt[i] = relay_transport_create(&sk[i], &cfg[i]);
   }
   for (t_end = g_now + 10000 * MS; g_now < t_end; g_now += MS)
   {
      net_step();
      relay_core_tick(g_dir, g_now / 1000);
      relay_core_tick(g_desk, g_now / 1000);
      rl_dirc_tick(&g_dc, g_now / 1000);
      if (relay_transport_poll(rt[0]) == RL_ST_IN_ROOM &&
          relay_transport_poll(rt[1]) == RL_ST_IN_ROOM)
         break;
   }
   if (relay_transport_poll(rt[0]) != RL_ST_IN_ROOM || relay_transport_poll(rt[1]) != RL_ST_IN_ROOM)
   {
      printf("FAIL %s: room never formed (%d/%d)\n", c->name, relay_transport_poll(rt[0]),
             relay_transport_poll(rt[1]));
      return 0;
   }
   if (strcmp(relay_transport_code(rt[0]), "QWER") || strcmp(relay_transport_code(rt[1]), "QWER"))
   {
      printf("FAIL %s: room codes %s/%s, want the home code QWER\n", c->name,
             relay_transport_code(rt[0]), relay_transport_code(rt[1]));
      ok = 0;
   }

   memset(&nc, 0, sizeof(nc));
   nc.protocol = "gpSP v1.0";
   for (i = 0; i < 2; i++)
   {
      memset(&app[i], 0, sizeof(app[i]));
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
   /* the netdrv host is the console the server gave slot 0 */
   hs = relay_transport_slot(rt[0]) == 0 ? 0 : 1;
   if (relay_transport_slot(rt[hs]) != 0 || relay_transport_slot(rt[1 - hs]) == 0)
   {
      printf("FAIL %s: slots %d/%d, want one 0\n", c->name, relay_transport_slot(rt[0]),
             relay_transport_slot(rt[1]));
      return 0;
   }
   netdrv_host(nd[hs]);
   netdrv_join(nd[1 - hs]);
   for (t_end = g_now + 40000 * MS; g_now < t_end; g_now += MS)
   {
      net_step();
      relay_core_tick(g_dir, g_now / 1000);
      relay_core_tick(g_desk, g_now / 1000);
      rl_dirc_tick(&g_dc, g_now / 1000);
      for (i = 0; i < 2; i++)
      {
         netdrv_pump(nd[i], g_now);
         if (netdrv_active(nd[i]) && netdrv_peer_count(nd[i]) > 0 && sent[i] < N_PAY &&
             (g_now / MS) % 16 == (unsigned)i * 8)
         {
            uint8_t p[8];
            uint32_t v = (uint32_t)sent[i];
            memcpy(p, &v, 4);
            memset(p + 4, 0xA5, 4);
            if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, p, sizeof(p),
                            i == hs ? relay_transport_slot(rt[1 - hs]) : 0) == 0)
               sent[i]++;
         }
      }
      if (app[0].got >= (uint32_t)N_PAY && app[1].got >= (uint32_t)N_PAY)
         break;
   }
   for (i = 0; i < 2; i++)
      relay_transport_stats(rt[i], &st[i]);
   relay_core_stats(g_dir, &ds);
   relay_core_stats(g_desk, &ks);
   {
      int p0 = st[0].path[relay_transport_slot(rt[1])], p1 = st[1].path[relay_transport_slot(rt[0])];
      int direct = p0 == 3 && p1 == 3;
      int relayed = (p0 == 1 || p0 == 4) && (p1 == 1 || p1 == 4);
      if (app[0].got != (uint32_t)N_PAY || app[1].got != (uint32_t)N_PAY || app[0].bad || app[1].bad)
      {
         printf("FAIL %s: payloads %u/%u bad %u/%u\n", c->name, app[0].got, app[1].got,
                app[0].bad, app[1].bad);
         ok = 0;
      }
      if (app[0].lost || app[1].lost || app[0].stopped || app[1].stopped)
      {
         printf("FAIL %s: netdrv session lost\n", c->name);
         ok = 0;
      }
      if (c->expect_direct ? !direct : !relayed)
      {
         printf("FAIL %s: path %d/%d, want %s\n", c->name, p0, p1,
                c->expect_direct ? "direct" : "relay");
         ok = 0;
      }
      for (i = 0; i < 2; i++)
      {
         int by = i ? c->by1 : c->by0;
         int where = i ? c->where1 : c->where0;
         uint32_t want_ip = where == W_DESKHOME && by == BY_DIR ? E[2].priv.ip : desk_pub;
         if (by == BY_DIR && st[i].located != RL_LOC_HOST)
         {
            printf("FAIL %s: psp %d lookup kind %u, want host\n", c->name, i, st[i].located);
            ok = 0;
         }
         if (st[i].host_ip_be != want_ip)
         {
            printf("FAIL %s: psp %d used server %08X, want %08X\n", c->name, i,
                   (unsigned)st[i].host_ip_be, (unsigned)want_ip);
            ok = 0;
         }
      }
      if (ds.fwd || g_dir_fwd_seen || ds.rooms_made)
      {
         printf("FAIL %s: the directory carried game traffic (fwd %u, rooms %u)\n", c->name,
                ds.fwd, ds.rooms_made);
         ok = 0;
      }
      if (ok)
         printf("ok   %-44s path=%s/%s via=%s/%s invite=%s dir_bytes=%u (lookups %u) "
                "desk_fwd=%u rtt_direct=%ums\n", c->name,
                p0 == 3 ? "direct" : "relay", p1 == 3 ? "direct" : "relay",
                c->by0 == BY_DIR ? "code" : "invite", c->by1 == BY_DIR ? "code" : "invite",
                invite, ds.dir_rx_bytes + ds.dir_tx_bytes, ds.lookups, ks.fwd,
                st[0].direct_rtt_us[1] / 1000);
   }
   (void)t_reg;
   for (i = 0; i < 2; i++)
   {
      netdrv_destroy(nd[i]);
      relay_transport_destroy(rt[i]);
   }
   rl_dirc_stop(&g_dc);
   relay_core_destroy(g_dir);
   relay_core_destroy(g_desk);
   return ok;
}

/* The directory alone: a restart loses the entry, the host gets its code
 * back; a taken code makes the client take the server's pick; one hour of
 * hosting costs this many bytes; an UNREGISTER frees the code at once. */
static int run_dir_lifecycle(void)
{
   rl_core_cfg cc;
   rl_dirc_cfg dcfg;
   rl_core_stats ds;
   int ok = 1;
   uint64_t t_end;
   uint32_t bytes0;
   memset(q, 0, sizeof(q));
   qn = 0;
   g_now = 1000000;
   memset(E, 0, sizeof(E));
   memset(N, 0, sizeof(N));
   g_nnat = 3;
   E[2].nat = -1;
   E[2].priv.ip = ip4(192, 0, 2, 50);
   E[2].priv.port = PORT;
   memset(&cc, 0, sizeof(cc));
   cc.random64 = core_rand;
   cc.send = dir_send;
   cc.directory = 1;
   g_dir = relay_core_create(&cc);
   cc.send = desk_send;
   cc.directory = 0;
   g_desk = relay_core_create(&cc);
   memset(&dcfg, 0, sizeof(dcfg));
   dcfg.dir_ip_be = DIR_IP;
   dcfg.dir_port = PORT;
   dcfg.send = dc_send;
   strcpy(dcfg.want, "ABCD");
   rl_dirc_start(&g_dc, &dcfg, g_now / 1000);
   for (t_end = g_now + 3600000 * MS; g_now < t_end; g_now += 10 * MS)
   {
      net_step();
      relay_core_tick(g_dir, g_now / 1000);
      rl_dirc_tick(&g_dc, g_now / 1000);
   }
   relay_core_stats(g_dir, &ds);
   bytes0 = ds.dir_rx_bytes + ds.dir_tx_bytes;
   if (g_dc.state != DC_REGISTERED || strcmp(g_dc.code, "ABCD") || ds.dir_now != 1)
   {
      printf("FAIL dir: an hour in, state %d code %s entries %u\n", g_dc.state, g_dc.code, ds.dir_now);
      ok = 0;
   }
   /* the server restarts: same code back within a refresh or two */
   relay_core_destroy(g_dir);
   cc.send = dir_send;
   cc.directory = 1;
   g_dir = relay_core_create(&cc);
   for (t_end = g_now + 70000 * MS; g_now < t_end; g_now += 10 * MS)
   {
      net_step();
      relay_core_tick(g_dir, g_now / 1000);
      rl_dirc_tick(&g_dc, g_now / 1000);
   }
   relay_core_stats(g_dir, &ds);
   if (g_dc.state != DC_REGISTERED || strcmp(g_dc.code, "ABCD") || ds.dir_now != 1)
   {
      printf("FAIL dir: after a restart, state %d code %s entries %u\n", g_dc.state, g_dc.code,
             ds.dir_now);
      ok = 0;
   }
   /* stop: the code is gone at once */
   rl_dirc_stop(&g_dc);
   for (t_end = g_now + 200 * MS; g_now < t_end; g_now += MS)
      net_step();
   relay_core_stats(g_dir, &ds);
   if (ds.dir_now != 0)
   {
      printf("FAIL dir: UNREGISTER left %u entries\n", ds.dir_now);
      ok = 0;
   }
   if (ok)
      printf("ok   %-44s 1 h of hosting = %u B at the directory (UDP payload), restart kept ABCD\n",
             "directory: refresh, restart, unregister", bytes0);
   relay_core_destroy(g_dir);
   relay_core_destroy(g_desk);
   return ok;
}

int main(int argc, char **argv)
{
   static const case_t cases[] = {
      /* name                                     nat0      nat1   where0      where1    by0        by1     direct desk      hairpin */
      { "code: cone x cone, direct",              NAT_CONE, NAT_CONE, W_OWN,    W_OWN,     BY_DIR,    BY_DIR,    1, NAT_CONE, 0, 0 },
      { "code: cone x symmetric, relayed by PC",  NAT_CONE, NAT_SYM,  W_OWN,    W_OWN,     BY_DIR,    BY_DIR,    0, NAT_CONE, 0, 0 },
      { "code: symmetric x symmetric, relayed",   NAT_SYM,  NAT_SYM,  W_OWN,    W_OWN,     BY_DIR,    BY_DIR,    0, NAT_CONE, 0, 0 },
      { "invite: cone x cone, direct",            NAT_CONE, NAT_CONE, W_OWN,    W_OWN,     BY_INVITE, BY_INVITE, 1, NAT_CONE, 0, 0 },
      { "invite: cone x symmetric, relayed",      NAT_CONE, NAT_SYM,  W_OWN,    W_OWN,     BY_INVITE, BY_INVITE, 0, NAT_CONE, 0, 0 },
      { "code + invite mixed, cone x cone",       NAT_CONE, NAT_CONE, W_OWN,    W_OWN,     BY_DIR,    BY_INVITE, 1, NAT_CONE, 0, 0 },
      { "code: host PSP in the PC's home, relayed on LAN", NAT_CONE, NAT_CONE, W_DESKHOME, W_OWN, BY_DIR, BY_DIR, 0, NAT_CONE, 0, 0 },
      { "code: public x cone, PC on a public IP", NAT_NONE, NAT_CONE, W_OWN,    W_OWN,     BY_DIR,    BY_DIR,    1, NAT_NONE, 0, 0 },
      { "both join by code: cone x cone, direct", NAT_CONE, NAT_CONE, W_OWN, W_OWN, BY_DIR, BY_DIR, 1, NAT_CONE, 0, 1 },
      { "both join by invite: cone x symmetric", NAT_CONE, NAT_SYM, W_OWN, W_OWN, BY_INVITE, BY_INVITE, 0, NAT_CONE, 0, 1 },
      { "both join, code + invite, symmetric x sym", NAT_SYM, NAT_SYM, W_OWN, W_OWN, BY_DIR, BY_INVITE, 0, NAT_CONE, 0, 1 },
   };
   unsigned i, fails = 0, n = sizeof(cases) / sizeof(cases[0]);
   g_verbose = argc > 1 && !strcmp(argv[1], "-v");
   for (i = 0; i < n; i++)
      if (!run_case(&cases[i]))
         fails++;
   if (!run_dir_lifecycle())
      fails++;
   printf("%s: %u/%u cases\n", fails ? "FAILED" : "PASS", n + 1 - fails, n + 1);
   return fails ? 1 : 0;
}
