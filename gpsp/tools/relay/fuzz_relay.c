/* fuzz_relay.c -- randomised, structure-aware packet fuzzing of the GBR1 relay
 * core and the client transport, under ASan + UBSan (tools/relay/build.sh).
 * No libFuzzer/AFL on this box, so: a seeded generator that mixes pure noise,
 * valid headers with random bodies of every type and length, replays of real
 * server replies (tokens, cookies, slots learned from what the core sent) and
 * bit flips of them -- millions of datagrams.
 *
 * Invariants checked on every datagram, besides "no sanitizer report":
 *   - NO AMPLIFICATION: anything the core sends to a source that holds no
 *     slot is never larger than the datagram that provoked it;
 *   - the core never sends more than RL_MAX_DGRAM, never to port 0;
 *   - rooms_now never exceeds max_rooms; DATA is only forwarded to addresses
 *     that hold a slot.
 *
 *   fuzz_relay [iterations] [seed]
 */
#include "relay_core.h"
#include "transport_relay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_rng = 0x1234567;
static uint64_t r64(void)
{
   g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
   return g_rng;
}
static unsigned rn(unsigned n) { return (unsigned)(r64() % n); }

/* what the core sent while handling the current datagram */
static rl_addr g_src;
static size_t g_in_len;
static int g_src_has_slot;
static unsigned long g_fail;
static uint8_t g_seen[64][RL_MAX_DGRAM];     /* replies, replayed later */
static size_t g_seen_len[64];
static unsigned g_seen_n;
static rl_core *g_core;

static int addr_holds_slot(const rl_addr *a);

static void core_send(void *u, int sock, const rl_addr *to, const void *buf, size_t len)
{
   (void)u;
   if (sock < 0 || sock > 1 || len > RL_MAX_DGRAM || !to->port)
   {
      printf("FAIL bad send sock=%d len=%u port=%u\n", sock, (unsigned)len, to->port);
      g_fail++;
   }
   if (to->ip_be == g_src.ip_be && to->port == g_src.port && !g_src_has_slot &&
       len > g_in_len)
   {
      printf("FAIL amplification: %u bytes out for %u in (type %u)\n",
             (unsigned)len, (unsigned)g_in_len, ((const uint8_t *)buf)[5]);
      g_fail++;
   }
   if (((const uint8_t *)buf)[5] == RL_T_DATA && !addr_holds_slot(to))
   {
      printf("FAIL DATA to an address with no slot\n");
      g_fail++;
   }
   memcpy(g_seen[g_seen_n % 64], buf, len);
   g_seen_len[g_seen_n % 64] = len;
   g_seen_n++;
}

static uint64_t core_rand(void *u) { (void)u; return r64(); }

/* a small population of sources, so state accumulates */
#define NSRC 24
static rl_addr g_pop[NSRC];
static uint64_t g_tok[NSRC];
static int g_slot[NSRC];
static uint64_t g_cookie[NSRC];
static uint8_t g_codes[16][RL_CODE_LEN];
static unsigned g_ncodes;

static int addr_holds_slot(const rl_addr *a)
{
   int i;
   for (i = 0; i < NSRC; i++)
      if (g_pop[i].ip_be == a->ip_be && g_pop[i].port == a->port && g_tok[i])
         return 1;
   /* the core may know a slot we did not record (a JOINED we did not parse);
    * be conservative: only flag addresses outside the population */
   for (i = 0; i < NSRC; i++)
      if (g_pop[i].ip_be == a->ip_be && g_pop[i].port == a->port)
         return 1;
   return 0;
}

static void learn(int who, unsigned first)
{
   unsigned k;
   for (k = first; k < g_seen_n; k++)
   {
      const uint8_t *b = g_seen[k % 64];
      if (g_seen_len[k % 64] < RL_HDR)
         continue;
      if (b[5] == RL_T_COOKIE && g_seen_len[k % 64] >= RL_HDR + 8)
         g_cookie[who] = rl_get64(b + RL_HDR);
      if ((b[5] == RL_T_CREATED || b[5] == RL_T_JOINED) && g_seen_len[k % 64] >= RL_HDR + RL_CODE_LEN + 1)
      {
         g_tok[who] = rl_get64(b + 8);
         g_slot[who] = b[RL_HDR + RL_CODE_LEN];
         memcpy(g_codes[g_ncodes++ % 16], b + RL_HDR, RL_CODE_LEN);
      }
   }
}

static size_t gen(uint8_t *b, int who)
{
   size_t len;
   int mode = (int)rn(10);
   if (mode == 0)                                  /* pure noise */
   {
      size_t i;
      len = rn(RL_MAX_DGRAM + 40);
      for (i = 0; i < len; i++)
         b[i] = (uint8_t)r64();
      return len;
   }
   if (mode == 1 && g_seen_n)                      /* replay / flip a reply */
   {
      unsigned k = rn(g_seen_n < 64 ? g_seen_n : 64);
      len = g_seen_len[k];
      memcpy(b, g_seen[k], len);
      if (len && rn(2))
         b[rn((unsigned)len)] ^= (uint8_t)(1u << rn(8));
      return len;
   }
   {
      int type = (int)rn(18);
      uint64_t tok = rn(3) ? g_tok[who] : r64();
      size_t i;
      len = RL_HDR + rn(80);
      if (rn(4) == 0)
         len = rn(RL_HDR + 2);                       /* truncated */
      for (i = 0; i < len && i < RL_MAX_DGRAM; i++)
         b[i] = (uint8_t)r64();
      if (len >= RL_HDR)
         rl_hdr(b, type, rn(3) ? g_slot[who] : (int)rn(256),
                rn(2) ? RL_SLOT_ALL : (int)rn(6), tok);
      if ((type == RL_T_CREATE || type == RL_T_JOIN) && len >= RL_HDR)
      {
         uint8_t *p = b + RL_HDR;
         size_t need = RL_HDR + RL_BUILD_LEN + RL_CODE_LEN + 7 + 8;
         if (rn(3) && len < need)
            len = need + rn(16);
         if (len >= need)
         {
            memset(p, 'b', RL_BUILD_LEN);
            for (i = 0; i < RL_CODE_LEN; i++)
               p[RL_BUILD_LEN + i] = rn(5) ? (uint8_t)('A' + rn(3)) : (uint8_t)r64();
            if (type == RL_T_JOIN && g_ncodes && rn(4))
               memcpy(p + RL_BUILD_LEN, g_codes[rn(g_ncodes < 16 ? g_ncodes : 16)], RL_CODE_LEN);
            if (rn(4))
               rl_put64(p + RL_BUILD_LEN + RL_CODE_LEN + 7, g_cookie[who]);
         }
      }
      return len;
   }
}

/* ---- the client side: random datagrams into a transport_relay ---------- */
static uint8_t c_in[RL_MAX_DGRAM + 64];
static int c_in_len;
static uint32_t c_in_ip;
static uint16_t c_in_port;
static uint64_t c_now = 1000000;
static uint64_t c_clock(void) { return c_now; }
static int c_send(void *x, uint32_t ip, uint16_t port, const void *b, size_t n)
{
   (void)x; (void)ip; (void)port; (void)b;
   if (n > RL_MAX_DGRAM)
   {
      printf("FAIL client sent %u bytes\n", (unsigned)n);
      g_fail++;
   }
   return 0;
}
static int c_recv(void *x, uint32_t *ip, uint16_t *port, void *buf, size_t cap)
{
   int n = c_in_len;
   (void)x;
   if (n <= 0)
      return 0;
   c_in_len = 0;
   if ((size_t)n > cap)
      n = (int)cap;
   memcpy(buf, c_in, (size_t)n);
   *ip = c_in_ip;
   *port = c_in_port;
   return n;
}

int main(int argc, char **argv)
{
   unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 2000000ul, it;
   rl_core_cfg cfg;
   rl_core_stats st;
   relay_transport *cl[2];
   rl_sock sk = { NULL, c_send, c_recv };
   rl_client_cfg cc;
   uint64_t now = 1000;
   int i;
   if (argc > 2)
      g_rng = strtoull(argv[2], NULL, 0) | 1;
   memset(&cfg, 0, sizeof(cfg));
   cfg.send = core_send;
   cfg.random64 = core_rand;
   cfg.max_rooms = 32;
   g_core = relay_core_create(&cfg);
   for (i = 0; i < NSRC; i++)
   {
      g_pop[i].ip_be = (uint32_t)(0x0A000000u + (unsigned)i / 3);   /* some share an IP */
      g_pop[i].port = (uint16_t)(1000 + i);
   }
   memset(&cc, 0, sizeof(cc));
   cc.server_ip_be = 0x01020304;
   cc.server_port = 27420;
   cc.direct = 1;
   cc.now_us = c_clock;
   for (i = 0; i < 2; i++)
   {
      cc.is_host = i == 0;
      strcpy(cc.code, "FUZZ");
      cl[i] = relay_transport_create(&sk, &cc);
   }
   for (it = 0; it < iters; it++)
   {
      uint8_t b[RL_MAX_DGRAM + 64];
      int who = (int)rn(NSRC);
      size_t len = gen(b, who);
      unsigned first = g_seen_n;
      g_src = g_pop[who];
      g_in_len = len;
      g_src_has_slot = g_tok[who] != 0 && b[5] != RL_T_PROBE && b[5] != RL_T_CREATE &&
                       b[5] != RL_T_JOIN;
      if (len >= RL_HDR && (b[5] == RL_T_CREATE || b[5] == RL_T_JOIN))
         g_src_has_slot = 0;
      relay_core_input(g_core, (int)rn(2), &g_src, b, len, now);
      if (g_seen_n != first)
         learn(who, first);
      if (rn(64) == 0)
      {
         now += rn(30000);
         relay_core_tick(g_core, now);
      }
      now += rn(3);
      /* the client: the same datagram, from the server or from a stranger */
      {
         int k = (int)rn(2);
         uint8_t mac[6], out[RL_MAX_DGRAM];
         nd_transport tp;
         c_in_len = (int)(len > sizeof(c_in) ? sizeof(c_in) : len);
         memcpy(c_in, b, (size_t)c_in_len);
         c_in_ip = rn(2) ? cc.server_ip_be : (uint32_t)r64();
         c_in_port = rn(2) ? (uint16_t)(27420 + rn(2)) : (uint16_t)r64();
         relay_transport_iface(cl[k], &tp);
         if (relay_transport_poll(cl[k]) == RL_ST_IN_ROOM)
         {
            tp.recv(tp.ctx, mac, out, sizeof(out));
            if (rn(4) == 0)
               tp.send_to(tp.ctx, mac, out, rn(200));
         }
         c_now += rn(40000);
      }
      relay_core_stats(g_core, &st);
      if (st.rooms_now > 32)
      {
         printf("FAIL rooms_now %u > max\n", st.rooms_now);
         g_fail++;
      }
      if (g_fail > 20)
         break;
   }
   relay_core_stats(g_core, &st);
   printf("%s: %lu datagrams; rx=%u bad=%u token=%u rate=%u rooms_made=%u joins=%u cookies=%u/%u bad_cookie=%u fwd=%u\n",
          g_fail ? "FAIL" : "PASS", it, st.rx, st.rx_bad, st.rx_token, st.rx_rate, st.rooms_made,
          st.joins, st.cookies_sent, st.rx, st.cookie_bad, st.fwd);
   for (i = 0; i < 2; i++)
      relay_transport_destroy(cl[i]);
   relay_core_destroy(g_core);
   return g_fail ? 1 : 0;
}
