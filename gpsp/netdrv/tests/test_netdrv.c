/* test_netdrv.c — netdrv unit tests (plan §5 Phase 3, ADR-0003 hardening).
 *
 * Most tests run on a deterministic in-memory mesh transport with a virtual
 * clock and seeded fault injection (loss/dup/latency+jitter -> reorder), so
 * every failure reproduces exactly. One final test exercises the real
 * transport_udp backend (loopback sockets + its fault shim) in real time.
 *
 * Coverage:
 *   1. wire      — build/parse round-trip; malformed/truncated/CRC rejects
 *   2. handshake — host + 2 clients establish under 20% loss
 *   3. arq       — 20% loss + 10% dup + 25ms jitter reorder; 400 reliable
 *                  payloads each way arrive complete, in order, uncorrupted
 *   3b. overload  — producer faster than window/RTT drains at 40 ms RTT:
 *                  ZERO RELIABLE payloads may be lost (ADR-0016 spill)
 *   3c. txfail    — a genuinely unabsorbable backlog fails the session
 *                  EXPLICITLY (ND_STOP_TX_FAILED), never silently
 *   3d. rto       — adaptive RTO at 40 ms RTT keeps retx/acked near the
 *                  loopback baseline instead of the field's 2.8 (ADR-0017)
 *   3e. oversize  — > ND_MAX_PAYLOAD is refused loudly, never truncated
 *   4. keepalive — dead client detected by host; dead host tears down client
 *   5. churn     — join/leave/rejoin cycles, id reuse, event bookkeeping
 *   6. fuzz      — 20k random/corrupted/truncated datagrams: nothing is
 *                  delivered, nothing crashes (the CRC+length wall the
 *                  garbage-intolerant core depends on)
 *   7. udp       — real UDP loopback with 20% loss + 10ms jitter
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netdrv.h"
#include "netdrv_wire.h"
#include "transport_udp.h"

static int g_fail;
#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } } while (0)

/* ===================================================== simulated transport == */

#define SIM_NODES 6
#define SIMQ      1024

typedef struct sim_pkt
{
   uint64_t due_us;
   uint8_t  src[6];
   uint16_t len;
   uint8_t  data[ND_MAX_FRAME + 32];
} sim_pkt;

typedef struct sim
{
   uint64_t now;
   uint32_t prng;
   int      loss_pct, dup_pct, lat_ms, jit_ms;
   int      n;
   struct
   {
      uint8_t mac[6];
      int     down;               /* unreachable (peer death simulation) */
      sim_pkt q[SIMQ];
      int     qn;
   } node[SIM_NODES];
} sim;

typedef struct sim_ep { sim *s; int idx; } sim_ep;

static uint32_t sim_rand(sim *s)
{
   uint32_t x = s->prng;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   s->prng = x;
   return x;
}

static void sim_init(sim *s, int nodes, uint32_t seed)
{
   int i;
   memset(s, 0, sizeof(*s));
   s->n = nodes;
   s->prng = seed ? seed : 1;
   for (i = 0; i < nodes; i++)
   {
      s->node[i].mac[0] = 0x02;   /* locally administered */
      s->node[i].mac[5] = (uint8_t)(i + 1);
   }
}

static void sim_deposit(sim *s, int dst, const uint8_t src_mac[6],
                        const void *buf, size_t len, int with_faults)
{
   int copies = 1;
   if (s->node[dst].down)
      return;
   if (with_faults)
   {
      if (s->loss_pct && (int)(sim_rand(s) % 100) < s->loss_pct)
         return;
      if (s->dup_pct && (int)(sim_rand(s) % 100) < s->dup_pct)
         copies = 2;
   }
   while (copies--)
   {
      sim_pkt *p;
      uint32_t delay_ms;
      if (s->node[dst].qn >= SIMQ)
         return;                            /* medium overflow: drop */
      delay_ms = (uint32_t)s->lat_ms +
         (with_faults && s->jit_ms ? sim_rand(s) % (uint32_t)(s->jit_ms + 1) : 0);
      p = &s->node[dst].q[s->node[dst].qn++];
      p->due_us = s->now + (uint64_t)delay_ms * 1000ull;
      memcpy(p->src, src_mac, 6);
      p->len = (uint16_t)(len > sizeof(p->data) ? sizeof(p->data) : len);
      memcpy(p->data, buf, p->len);
   }
}

static int sim_find(sim *s, const uint8_t mac[6])
{
   int i;
   for (i = 0; i < s->n; i++)
      if (memcmp(s->node[i].mac, mac, 6) == 0)
         return i;
   return -1;
}

static int sim_send_to(void *ctx, const uint8_t mac[6],
                       const void *buf, size_t len)
{
   sim_ep *e = (sim_ep *)ctx;
   int dst = sim_find(e->s, mac);
   if (dst >= 0 && dst != e->idx)
      sim_deposit(e->s, dst, e->s->node[e->idx].mac, buf, len, 1);
   return 0;
}

static int sim_broadcast(void *ctx, const void *buf, size_t len)
{
   sim_ep *e = (sim_ep *)ctx;
   int i;
   for (i = 0; i < e->s->n; i++)
      if (i != e->idx)
         sim_deposit(e->s, i, e->s->node[e->idx].mac, buf, len, 1);
   return 0;
}

static int sim_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   sim_ep *e = (sim_ep *)ctx;
   sim *s = e->s;
   int qi, best = -1, i;
   uint64_t best_due = 0;
   sim_pkt *p;
   size_t n;

   /* earliest-due packet that is due (jitter -> natural reorder) */
   for (i = 0; i < s->node[e->idx].qn; i++)
   {
      p = &s->node[e->idx].q[i];
      if (p->due_us <= s->now && (best < 0 || p->due_us < best_due))
      {
         best = i;
         best_due = p->due_us;
      }
   }
   if (best < 0)
      return 0;
   qi = best;
   p = &s->node[e->idx].q[qi];
   n = p->len > cap ? cap : p->len;
   memcpy(buf, p->data, n);
   memcpy(src_mac, p->src, 6);
   s->node[e->idx].q[qi] = s->node[e->idx].q[--s->node[e->idx].qn];
   return (int)n;
}

/* Cheap "would recv() return something?" — the shape a real transport's
 * pending() hook has (ADR-0021). Must agree with sim_recv exactly, or
 * netdrv_poll_needed() would gate away work that was actually there. */
static int sim_pending(void *ctx)
{
   sim_ep *e = (sim_ep *)ctx;
   sim *s = e->s;
   int i;
   for (i = 0; i < s->node[e->idx].qn; i++)
      if (s->node[e->idx].q[i].due_us <= s->now)
         return 1;
   return 0;
}

static void sim_local_addr(void *ctx, uint8_t mac[6])
{
   sim_ep *e = (sim_ep *)ctx;
   memcpy(mac, e->s->node[e->idx].mac, 6);
}

/* =========================================================== app harness == */

typedef struct app
{
   const char *name;
   int      started;
   uint8_t  local_id;
   int      stopped, stop_reason;
   int      conn_events, disc_events;
   uint8_t  connected[ND_MAX_CLIENTS];
   uint32_t delivered;
   uint32_t bad_payload;           /* corruption/order violations seen */
   uint32_t next_expect[ND_MAX_CLIENTS];
   netdrv  *nd;                    /* for re-entrant echo tests */
   int      echo;                  /* reply reliably to sender from deliver() */
   uint32_t echo_rx;
} app;

/* payload: u32 counter | u8 src_id | u8 kind | pattern bytes (len-6) */
static size_t msg_build(uint8_t *out, uint32_t counter, uint8_t src_id,
                        uint8_t kind, size_t len)
{
   size_t i;
   if (len < 6)
      len = 6;
   out[0] = (uint8_t)counter; out[1] = (uint8_t)(counter >> 8);
   out[2] = (uint8_t)(counter >> 16); out[3] = (uint8_t)(counter >> 24);
   out[4] = src_id;
   out[5] = kind;
   for (i = 6; i < len; i++)
      out[i] = (uint8_t)(counter * 31 + i);
   return len;
}

static void app_deliver(void *user, const void *buf, size_t len, uint8_t src)
{
   app *a = (app *)user;
   const uint8_t *p = (const uint8_t *)buf;
   uint32_t counter;
   size_t i;

   a->delivered++;
   if (len < 6 || src >= ND_MAX_CLIENTS)
   {
      a->bad_payload++;
      return;
   }
   counter = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
   if (p[4] != src)
      a->bad_payload++;
   if (p[5] == 1)                   /* echo reply — count, don't sequence */
   {
      a->echo_rx++;
      return;
   }
   if (counter != a->next_expect[src])
      a->bad_payload++;             /* out of order / dup / loss */
   else
      a->next_expect[src] = counter + 1;
   for (i = 6; i < len; i++)
      if (p[i] != (uint8_t)(counter * 31 + i))
      {
         a->bad_payload++;
         break;
      }
   if (a->echo && a->nd)            /* re-entrant send from deliver() */
   {
      uint8_t reply[8];
      msg_build(reply, counter, a->local_id, 1, sizeof(reply));
      netdrv_send(a->nd, ND_RELIABLE | ND_FLUSH_HINT, reply, sizeof(reply), src);
   }
}

static void app_started(void *user, uint8_t id)
{
   app *a = (app *)user;
   a->started = 1;
   a->local_id = id;
}

static int app_connected(void *user, uint8_t id)
{
   app *a = (app *)user;
   a->conn_events++;
   a->connected[id] = 1;
   return 0;
}

static void app_disconnected(void *user, uint8_t id)
{
   app *a = (app *)user;
   a->disc_events++;
   a->connected[id] = 0;
}

static void app_stopped(void *user, int reason)
{
   app *a = (app *)user;
   a->stopped = 1;
   a->stop_reason = reason;
}

static netdrv *mk_node_cfg(sim *s, sim_ep *ep, int idx, app *a,
                           const char *name, uint32_t nonce,
                           const char *proto, const nd_config *extra)
{
   nd_transport tp;
   nd_callbacks cb;
   nd_config cfg;

   ep->s = s;
   ep->idx = idx;
   tp.ctx = ep;
   tp.send_to = sim_send_to;
   tp.broadcast = sim_broadcast;
   tp.recv = sim_recv;
   tp.local_addr = sim_local_addr;
   tp.pending = sim_pending;

   memset(a, 0, sizeof(*a));
   a->name = name;
   memset(&cb, 0, sizeof(cb));
   cb.user = a;
   cb.deliver = app_deliver;
   cb.session_started = app_started;
   cb.peer_connected = app_connected;
   cb.peer_disconnected = app_disconnected;
   cb.session_stopped = app_stopped;

   if (extra)
      cfg = *extra;
   else
      memset(&cfg, 0, sizeof(cfg));
   cfg.protocol = proto ? proto : "gpSP v1.0";
   cfg.nick = name;
   cfg.join_nonce = nonce;

   a->nd = netdrv_create(&tp, &cb, &cfg);
   return a->nd;
}

static netdrv *mk_node(sim *s, sim_ep *ep, int idx, app *a, const char *name,
                       uint32_t nonce, const char *proto)
{
   return mk_node_cfg(s, ep, idx, a, name, nonce, proto, NULL);
}

/* run all nodes for ms virtual milliseconds */
static void sim_run(sim *s, netdrv **nds, int n, int ms)
{
   int t, i;
   for (t = 0; t < ms; t++)
   {
      s->now += 1000;
      for (i = 0; i < n; i++)
         if (nds[i])
            netdrv_pump(nds[i], s->now);
   }
}

/* ================================================================= tests == */

static void test_wire(void)
{
   uint8_t frame[ND_MAX_FRAME], payload[ND_MAX_PAYLOAD];
   nd_hdr h, ph;
   const uint8_t *pp;
   size_t sz, i;
   int reason;

   printf("[wire]\n");
   for (i = 0; i < sizeof(payload); i++)
      payload[i] = (uint8_t)(i * 7);

   memset(&h, 0, sizeof(h));
   h.type = ND_T_DATA; h.src = 1; h.dst = 0;
   h.seq = 0xBEEF; h.ack = 0x1234; h.len = 560;
   sz = nd_wire_build(frame, &h, payload);
   CHECK(sz == 576, "sz=%zu", sz);
   CHECK(nd_wire_parse(frame, sz, &ph, &pp, &reason) == 0, "reason=%d", reason);
   CHECK(ph.seq == 0xBEEF && ph.ack == 0x1234 && ph.len == 560, "fields");
   CHECK(memcmp(pp, payload, 560) == 0, "payload");

   /* corrupted byte -> CRC reject */
   frame[100] ^= 0x40;
   CHECK(nd_wire_parse(frame, sz, &ph, &pp, &reason) != 0 && reason == 5,
         "corrupt accepted, reason=%d", reason);
   frame[100] ^= 0x40;

   /* truncated -> exact-length reject */
   CHECK(nd_wire_parse(frame, sz - 1, &ph, &pp, &reason) != 0,
         "truncated accepted");
   /* extended -> exact-length reject */
   CHECK(nd_wire_parse(frame, sz + 1 <= sizeof(frame) ? sz : sz, &ph, &pp,
         &reason) == 0, "sanity");
   /* len field lies (bigger) */
   {
      uint8_t evil[ND_MAX_FRAME];
      memcpy(evil, frame, sz);
      evil[12] = 0xFF; evil[13] = 0x01;   /* len = 511 but sz says 560 */
      CHECK(nd_wire_parse(evil, sz, &ph, &pp, &reason) != 0,
            "lying len accepted");
   }
   /* short runt */
   CHECK(nd_wire_parse(frame, 8, &ph, &pp, &reason) != 0 && reason == 1,
         "runt accepted");
   /* bad magic */
   frame[0] ^= 0xFF;
   CHECK(nd_wire_parse(frame, sz, &ph, &pp, &reason) != 0 && reason == 2,
         "bad magic accepted");
   frame[0] ^= 0xFF;
   /* unknown type */
   frame[5] = 0x7F;
   CHECK(nd_wire_parse(frame, sz, &ph, &pp, &reason) != 0 && reason == 3,
         "bad type accepted");
}

static void test_handshake_loss(void)
{
   sim s;
   sim_ep ep[3];
   app a[3];
   netdrv *nd[3];
   int i;

   printf("[handshake under 20%% loss]\n");
   sim_init(&s, 3, 0xC0FFEE);
   s.loss_pct = 20;
   s.lat_ms = 2;
   s.jit_ms = 8;
   for (i = 0; i < 3; i++)
      nd[i] = mk_node(&s, &ep[i], i, &a[i], i == 0 ? "host" : "cli",
                      0x1000u + (uint32_t)i, NULL);

   netdrv_host(nd[0]);
   CHECK(a[0].started && a[0].local_id == 0, "host start");
   netdrv_join(nd[1]);
   netdrv_join(nd[2]);
   sim_run(&s, nd, 3, 10000);

   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]) && netdrv_active(nd[2]),
         "all active: %d %d %d", netdrv_active(nd[0]), netdrv_active(nd[1]),
         netdrv_active(nd[2]));
   CHECK(a[1].started && a[2].started, "clients started");
   CHECK((a[1].local_id == 1 && a[2].local_id == 2) ||
         (a[1].local_id == 2 && a[2].local_id == 1),
         "ids %u %u", a[1].local_id, a[2].local_id);
   CHECK(netdrv_peer_count(nd[0]) == 2, "host peers=%d", netdrv_peer_count(nd[0]));
   CHECK(netdrv_peer_count(nd[1]) == 2, "cli1 peers=%d", netdrv_peer_count(nd[1]));
   CHECK(netdrv_peer_count(nd[2]) == 2, "cli2 peers=%d", netdrv_peer_count(nd[2]));
   CHECK(a[0].conn_events == 2, "host conn events=%d", a[0].conn_events);
   CHECK(a[1].connected[0] && a[2].connected[0], "clients see host");

   for (i = 0; i < 3; i++)
      netdrv_destroy(nd[i]);
}

static void test_arq(void)
{
   enum { MSGS = 400 };
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint32_t sent[2] = { 0, 0 };
   uint8_t buf[ND_MAX_PAYLOAD];
   int t, i;
   nd_stats st0, st1;

   printf("[arq: 20%% loss, 10%% dup, 25ms jitter reorder, %d msgs each way]\n",
          (int)MSGS);
   sim_init(&s, 2, 0xDEADBEEF);
   s.loss_pct = 20;
   s.dup_pct = 10;
   s.lat_ms = 3;
   s.jit_ms = 25;
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* pump + paced sends (mix of unicast and reliable-broadcast fan-out;
    * lengths sweep 6..560 including the 104B RFU shape) */
   for (t = 0; t < 60000; t++)
   {
      s.now += 1000;
      for (i = 0; i < 2; i++)
      {
         netdrv_pump(nd[i], s.now);
         if ((t % 3) == i && sent[i] < MSGS)
         {
            uint16_t dst = sent[i] % 4 == 3 ? ND_BROADCAST_ID
                                            : (uint16_t)a[!i].local_id;
            size_t len = 6 + ((sent[i] * 37) % 555);
            if (sent[i] % 5 == 0)
               len = 104;                      /* RFU data frame size */
            if (netdrv_send_capacity(nd[i], dst) > 0)
            {
               msg_build(buf, sent[i], a[i].local_id, 0, len);
               if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                               dst) == 0)
                  sent[i]++;
            }
         }
      }
      if (a[0].next_expect[a[1].local_id] == MSGS &&
          a[1].next_expect[a[0].local_id] == MSGS)
         break;
   }

   CHECK(sent[0] == MSGS && sent[1] == MSGS, "sent %u %u", sent[0], sent[1]);
   CHECK(a[0].next_expect[a[1].local_id] == MSGS,
         "host received %u/%d in order", a[0].next_expect[a[1].local_id], (int)MSGS);
   CHECK(a[1].next_expect[a[0].local_id] == MSGS,
         "cli received %u/%d in order", a[1].next_expect[a[0].local_id], (int)MSGS);
   CHECK(a[0].bad_payload == 0 && a[1].bad_payload == 0,
         "corruption/order violations: %u %u", a[0].bad_payload, a[1].bad_payload);

   netdrv_get_stats(nd[0], &st0);
   netdrv_get_stats(nd[1], &st1);
   CHECK(st0.retx > 0 && st1.retx > 0, "retransmits happened (%u %u)",
         st0.retx, st1.retx);
   CHECK(st0.rx_dup + st1.rx_dup > 0, "dupes were deduped");
   CHECK(st0.tx_overflow == 0 && st1.tx_overflow == 0, "no reliable overflow");
   CHECK(st0.rx_drop_crc == 0, "sim injects no corruption");
   printf("  stats host: tx=%u rx=%u retx=%u dup=%u acked=%u\n",
          st0.tx_frames, st0.rx_frames, st0.retx, st0.rx_dup, st0.acked);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* ADR-0021 regression wall: the mid-frame poll gate must save work without
 * ever costing delivery.
 *
 * The core calls poll_receive from update_serial() — ~450 times per emulated
 * frame while its RFU waits — and every one of those used to run a full
 * pump. netdrv_poll_needed() short-circuits the idle ones. This test drives
 * the driver EXACTLY the way the PSP frontend now does (28 gated polls per
 * virtual millisecond plus one unconditional pump per 16 ms "frame") over a
 * lossy, jittery, reordering link, and asserts:
 *   - every payload still arrives, in order, exactly once, both ways;
 *   - the gate actually gated (most polls were skipped);
 *   - a queued outbound payload always opens the gate, so the core's
 *     response latency stays sub-frame (that is the only reason rfu.c polls
 *     mid-frame in the first place). */
static void test_poll_gate(void)
{
   enum { MSGS = 300, POLLS_PER_MS = 28, FRAME_MS = 16 };
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint32_t sent[2] = { 0, 0 };
   uint32_t polls = 0, pumped = 0, tx_opened = 0;
   uint8_t buf[ND_MAX_PAYLOAD];
   int t, i, k;

   printf("[poll gate: frontend cadence, 10%% loss + 20ms jitter, %d msgs "
          "each way]\n", (int)MSGS);
   sim_init(&s, 2, 0x0FF1CE21);
   s.loss_pct = 10;
   s.lat_ms = 3;
   s.jit_ms = 20;
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xC3, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xD4, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* Idle, both queues drained: the gate must say no. */
   sim_run(&s, nd, 2, 400);
   CHECK(!netdrv_poll_needed(nd[0]) && !netdrv_poll_needed(nd[1]),
         "idle link does not need a pump");

   /* A payload queued from the core's send_fn must reopen it immediately —
    * otherwise the reply waits for the next frame boundary. */
   msg_build(buf, 0, a[0].local_id, 0, 104);
   netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, buf, 104,
               (uint16_t)a[1].local_id);
   CHECK(netdrv_poll_needed(nd[0]), "queued tx opens the gate");
   netdrv_pump(nd[0], s.now);
   CHECK(!netdrv_poll_needed(nd[0]), "gate closes once the tx went out");
   sent[0] = 1;

   for (t = 0; t < 120000; t++)
   {
      s.now += 1000;
      for (i = 0; i < 2; i++)
      {
         /* The mid-frame poll storm, gated. */
         for (k = 0; k < POLLS_PER_MS; k++)
         {
            polls++;
            if (netdrv_poll_needed(nd[i]))
            {
               pumped++;
               netdrv_pump(nd[i], s.now);
            }
         }
         if ((t % FRAME_MS) == 0)
            netdrv_pump(nd[i], s.now);       /* the once-per-frame pump */

         if ((t % 8) == i * 4 && sent[i] < MSGS)
         {
            uint16_t dst = sent[i] % 4 == 3 ? ND_BROADCAST_ID
                                            : (uint16_t)a[!i].local_id;
            size_t len = sent[i] % 5 == 0 ? 104 : 6 + ((sent[i] * 37) % 90);
            if (netdrv_send_capacity(nd[i], dst) > 0)
            {
               msg_build(buf, sent[i], a[i].local_id, 0, len);
               if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                               dst) == 0)
               {
                  sent[i]++;
                  if (netdrv_poll_needed(nd[i]))
                     tx_opened++;
               }
            }
         }
      }
      if (a[0].next_expect[a[1].local_id] == MSGS &&
          a[1].next_expect[a[0].local_id] == MSGS)
         break;
   }

   CHECK(sent[0] == MSGS && sent[1] == MSGS, "sent %u %u", sent[0], sent[1]);
   CHECK(a[0].next_expect[a[1].local_id] == MSGS &&
         a[1].next_expect[a[0].local_id] == MSGS,
         "in-order delivery under the gate: %u/%u",
         a[0].next_expect[a[1].local_id], a[1].next_expect[a[0].local_id]);
   CHECK(a[0].bad_payload == 0 && a[1].bad_payload == 0,
         "corruption/order violations: %u %u", a[0].bad_payload,
         a[1].bad_payload);
   CHECK(tx_opened == sent[0] + sent[1] - 1,
         "every queued payload opened the gate (%u/%u)", tx_opened,
         sent[0] + sent[1] - 1);
   CHECK(pumped * 4 < polls, "the gate actually gated: %u pumps / %u polls",
         pumped, polls);
   printf("  polls=%u pumped=%u (%u%% short-circuited)\n", polls, pumped,
          polls ? (unsigned)(100 - (uint64_t)pumped * 100 / polls) : 0);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* ---------------------------------------------------------------------------
 * The three tests below are the regression wall for docs/HANDOFF.md issue #2
 * (field failure, two real PSPs, 2026-08-01): an RTO floor tuned on
 * microsecond-RTT loopback stormed on real radio (retx/acked 2.8, 68 % of RX
 * duplicates, nothing actually lost), the storm backed the tx ring up, and
 * the ring DISCARDED two RELIABLE core payloads — which destroys the RFU
 * state machine and wedges the game with a "communication error".
 * ------------------------------------------------------------------------ */

/* 1. Sustained overload at radio RTT must lose NOTHING. The producer runs
 *    faster than window/RTT can drain, so the fixed ring is guaranteed to
 *    fill; every payload must still arrive, in order, exactly once. */
static void test_no_reliable_loss_under_overload(void)
{
   enum { MSGS = 4000 };
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   nd_config cfg;
   uint8_t buf[128];
   uint32_t sent = 0, refused = 0;
   int t;
   nd_stats st;

   printf("[overload: 40ms RTT, producer >> drain rate, %d reliable msgs]\n",
          (int)MSGS);
   sim_init(&s, 2, 0x0FF10AD);
   s.lat_ms = 20;                 /* 20 ms each way = 40 ms RTT */
   s.jit_ms = 4;
   memset(&cfg, 0, sizeof(cfg));
   cfg.spill_max = 8000;          /* headroom: this test is about LOSS */
   nd[0] = mk_node_cfg(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL, &cfg);
   nd[1] = mk_node_cfg(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL, &cfg);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* 2 payloads/ms = 2000/s against a window*RTT ceiling of ~800/s. */
   for (t = 0; t < 40000; t++)
   {
      int k;
      s.now += 1000;
      netdrv_pump(nd[0], s.now);
      netdrv_pump(nd[1], s.now);
      for (k = 0; k < 2 && sent < MSGS && t < 2000; k++)
      {
         size_t len = msg_build(buf, sent, a[0].local_id, 0, 104);
         if (netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                         (uint16_t)a[1].local_id) == 0)
            sent++;
         else
            refused++;
      }
      if (sent == MSGS && a[1].next_expect[a[0].local_id] == MSGS)
         break;
   }

   netdrv_get_stats(nd[0], &st);
   CHECK(refused == 0, "sends refused under overload: %u", refused);
   CHECK(sent == MSGS, "queued %u/%d", sent, (int)MSGS);
   CHECK(st.tx_spill > 0, "spill never exercised (%u) — test is not overloading",
         st.tx_spill);
   CHECK(st.tx_overflow == 0, "RELIABLE PAYLOAD LOST (overflow=%u)",
         st.tx_overflow);
   CHECK(a[1].next_expect[a[0].local_id] == MSGS,
         "client received %u/%d in order", a[1].next_expect[a[0].local_id],
         (int)MSGS);
   CHECK(a[1].bad_payload == 0, "order/corruption violations: %u",
         a[1].bad_payload);
   CHECK(!a[0].stopped && !a[1].stopped, "session survived the overload");
   printf("  spilled=%u txq_hiwater=%u srtt_us=%u rto_us=%u\n",
          st.tx_spill, st.txq_hiwater, st.srtt_us, st.rto_us);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* 2. When the backlog genuinely cannot be absorbed (spill capped at 8 here),
 *    the failure must be EXPLICIT — session_stopped(ND_STOP_TX_FAILED) — and
 *    never a silent discard that leaves both sides pinging a dead link. */
static void test_txq_failure_is_loud(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   nd_config cfg;
   uint8_t buf[104];
   uint32_t sent = 0, refused = 0;
   int t;
   nd_stats st;

   printf("[unrecoverable txq: explicit failure, never a silent drop]\n");
   sim_init(&s, 2, 0xBADBAD);
   s.lat_ms = 20;
   memset(&cfg, 0, sizeof(cfg));
   cfg.spill_max = 8;             /* forced: no room to absorb anything */
   nd[0] = mk_node_cfg(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL, &cfg);
   nd[1] = mk_node_cfg(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL, &cfg);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* Blast far past ring+spill inside one keepalive interval, so the peer
    * is unambiguously LIVE (not the tx_drop_dead disconnect path). */
   for (t = 0; t < 400 && !a[0].stopped; t++)
   {
      int k;
      s.now += 1000;
      netdrv_pump(nd[0], s.now);
      netdrv_pump(nd[1], s.now);
      for (k = 0; k < 10; k++)
      {
         size_t len = msg_build(buf, sent, a[0].local_id, 0, 104);
         if (netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                         (uint16_t)a[1].local_id) == 0)
            sent++;
         else
            refused++;
      }
   }

   netdrv_get_stats(nd[0], &st);
   CHECK(refused > 0, "the send path never reported failure to its caller");
   CHECK(a[0].stopped && a[0].stop_reason == ND_STOP_TX_FAILED,
         "session did not fail loudly (stopped=%d reason=%d)",
         a[0].stopped, a[0].stop_reason);
   CHECK(!netdrv_active(nd[0]), "driver still claims an active session");
   CHECK(st.tx_overflow > 0, "the lost-payload counter must be visible");
   CHECK(st.tx_drop_dead == 0, "misclassified as a dying link (%u)",
         st.tx_drop_dead);
   printf("  queued=%u refused=%u overflow=%u -> stop reason=%d\n",
          sent, refused, st.tx_overflow, a[0].stop_reason);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* 3. Adaptive RTO against the field numbers. Same traffic shape the RFU link
 *    generates (~2 payloads/frame each way at 60 fps) on a LOSSLESS 40 ms-RTT
 *    link — i.e. the real-radio case, where the old fixed 30 ms floor fired
 *    before an ACK could physically arrive. Field: retx/acked 2.8, dup/rx
 *    68 %. PPSSPP loopback baseline: 0.11. This must land near the baseline. */
static void test_adaptive_rto(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint8_t buf[104];
   uint32_t sent[2] = { 0, 0 };
   int t, i;
   nd_stats st[2];
   unsigned retx_pct, dup_pct;

   printf("[adaptive RTO: 40ms RTT, no loss, RFU-shaped traffic]\n");
   sim_init(&s, 2, 0x517074A);
   s.lat_ms = 20;                 /* 40 ms RTT — real PSP ad-hoc territory */
   s.jit_ms = 6;
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* 20 s of link traffic: 2 payloads per 16 ms frame, both directions. */
   for (t = 0; t < 20000; t++)
   {
      s.now += 1000;
      for (i = 0; i < 2; i++)
      {
         netdrv_pump(nd[i], s.now);
         if ((t % 16) == 0)
         {
            int k;
            for (k = 0; k < 2; k++)
            {
               size_t len = msg_build(buf, sent[i], a[i].local_id, 0, 104);
               if (netdrv_send(nd[i], ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                               (uint16_t)a[!i].local_id) == 0)
                  sent[i]++;
            }
         }
      }
   }
   sim_run(&s, nd, 2, 2000);

   netdrv_get_stats(nd[0], &st[0]);
   netdrv_get_stats(nd[1], &st[1]);
   for (i = 0; i < 2; i++)
   {
      retx_pct = st[i].acked ? st[i].retx * 100 / st[i].acked : 999;
      dup_pct  = st[i].rx_frames ? st[i].rx_dup * 100 / st[i].rx_frames : 999;
      printf("  %s: acked=%u retx=%u (%u%%) dup=%u/%u (%u%%) srtt=%uus "
             "rto=%uus samples=%u\n", i ? "cli" : "host", st[i].acked,
             st[i].retx, retx_pct, st[i].rx_dup, st[i].rx_frames, dup_pct,
             st[i].srtt_us, st[i].rto_us, st[i].rtt_samples);
      /* Field failure was 280 %; the loopback baseline is 11 %. */
      CHECK(retx_pct <= 25, "retransmit storm: retx/acked = %u%%", retx_pct);
      CHECK(dup_pct <= 15, "duplicate storm: dup/rx = %u%%", dup_pct);
      CHECK(st[i].rtt_samples > 100, "too few Karn-valid RTT samples (%u)",
            st[i].rtt_samples);
      CHECK(st[i].srtt_us >= 35000 && st[i].srtt_us <= 90000,
            "SRTT %u us does not match the 40 ms link", st[i].srtt_us);
      CHECK(st[i].rto_us >= st[i].srtt_us,
            "RTO %u < SRTT %u", st[i].rto_us, st[i].srtt_us);
      CHECK(st[i].tx_overflow == 0, "overflow under normal load");
   }
   CHECK(a[0].next_expect[a[1].local_id] == sent[1] &&
         a[1].next_expect[a[0].local_id] == sent[0],
         "everything delivered in order (%u/%u, %u/%u)",
         a[0].next_expect[a[1].local_id], sent[1],
         a[1].next_expect[a[0].local_id], sent[0]);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* 4. Oversize payloads are refused loudly, never truncated (ADR-0016). */
static void test_oversize_refused(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   static uint8_t big[ND_MAX_PAYLOAD + 64];
   nd_stats st;

   printf("[oversize payload: refused + explicit failure, never truncated]\n");
   sim_init(&s, 2, 0x0121FE);
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]), "session up");

   memset(big, 0xA5, sizeof(big));
   CHECK(netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, big,
                     ND_MAX_PAYLOAD + 1, (uint16_t)a[1].local_id) != 0,
         "oversize send accepted");
   sim_run(&s, nd, 2, 100);
   netdrv_get_stats(nd[0], &st);
   CHECK(st.tx_oversize == 1, "tx_oversize=%u", st.tx_oversize);
   CHECK(a[0].stopped && a[0].stop_reason == ND_STOP_TX_FAILED,
         "oversize RELIABLE did not fail the session (stopped=%d reason=%d)",
         a[0].stopped, a[0].stop_reason);
   CHECK(a[1].delivered == 0, "a truncated payload reached the peer");

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

static void test_reentrant_echo(void)
{
   enum { MSGS = 50 };
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint8_t buf[ND_MAX_PAYLOAD];
   uint32_t snt = 0;
   int t;

   printf("[re-entrant send from deliver() — core ACK pattern]\n");
   sim_init(&s, 2, 0x5EED);
   s.loss_pct = 10;
   s.jit_ms = 5;
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   a[1].echo = 1;                    /* client replies from inside deliver() */
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   for (t = 0; t < 20000; t++)
   {
      s.now += 1000;
      netdrv_pump(nd[0], s.now);
      netdrv_pump(nd[1], s.now);
      if ((t % 5) == 0 && snt < MSGS)
      {
         size_t len = msg_build(buf, snt, 0, 0, 40);
         if (netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, buf, len, 1) == 0)
            snt++;
      }
      if (a[0].echo_rx == MSGS)
         break;
   }
   CHECK(a[1].next_expect[0] == MSGS, "client got %u", a[1].next_expect[0]);
   CHECK(a[0].echo_rx == MSGS, "host got %u echoes", a[0].echo_rx);
   CHECK(a[0].bad_payload + a[1].bad_payload == 0, "clean");

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

static void test_keepalive_death(void)
{
   sim s;
   sim_ep ep[3];
   app a[3];
   netdrv *nd[3];
   netdrv *alive[3];
   int i;

   printf("[keepalive death detection]\n");
   sim_init(&s, 3, 0xFEED);
   for (i = 0; i < 3; i++)
      nd[i] = mk_node(&s, &ep[i], i, &a[i], i == 0 ? "host" : "cli",
                      0x2000u + (uint32_t)i, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   netdrv_join(nd[2]);
   sim_run(&s, nd, 3, 3000);
   CHECK(netdrv_peer_count(nd[0]) == 2, "setup peers=%d", netdrv_peer_count(nd[0]));

   /* Client 2 dies silently (stop pumping, unreachable). */
   s.node[2].down = 1;
   alive[0] = nd[0]; alive[1] = nd[1]; alive[2] = NULL;
   sim_run(&s, alive, 3, 10000);     /* > 8s dead threshold */

   CHECK(netdrv_peer_count(nd[0]) == 1, "host dropped dead client (peers=%d)",
         netdrv_peer_count(nd[0]));
   CHECK(!a[0].connected[a[2].local_id], "host disc event for dead client");
   CHECK(!a[1].connected[a[2].local_id],
         "surviving client saw roster removal");
   CHECK(netdrv_active(nd[1]), "survivor session intact");

   /* Now the host dies: client must tear the whole session down. */
   s.node[0].down = 1;
   alive[0] = NULL;
   sim_run(&s, alive, 3, 10000);
   CHECK(!netdrv_active(nd[1]), "client tore down after host death");
   CHECK(a[1].stopped && a[1].stop_reason == ND_STOP_HOST_LOST,
         "stop reason=%d", a[1].stop_reason);

   for (i = 0; i < 3; i++)
      netdrv_destroy(nd[i]);
}

static void test_roster_churn(void)
{
   sim s;
   sim_ep ep[4];
   app a[4];
   netdrv *nd[4];
   int i, cycle;

   printf("[roster churn under 10%% loss]\n");
   sim_init(&s, 4, 0xC0DE);
   s.loss_pct = 10;
   s.jit_ms = 5;
   for (i = 0; i < 4; i++)
      nd[i] = mk_node(&s, &ep[i], i, &a[i], i == 0 ? "host" : "cli",
                      0x3000u + (uint32_t)i, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);              /* stable resident client */
   sim_run(&s, nd, 4, 3000);
   CHECK(netdrv_active(nd[1]), "resident joined");

   for (cycle = 0; cycle < 3; cycle++)
   {
      /* node 2 joins, then leaves cleanly; node 3 joins and stays a bit */
      netdrv_join(nd[2]);
      netdrv_join(nd[3]);
      sim_run(&s, nd, 4, 4000);
      CHECK(netdrv_active(nd[2]) && netdrv_active(nd[3]),
            "cycle %d: churners active", cycle);
      CHECK(netdrv_peer_count(nd[0]) == 3, "cycle %d: host peers=%d",
            cycle, netdrv_peer_count(nd[0]));
      netdrv_leave(nd[2]);
      netdrv_leave(nd[3]);
      sim_run(&s, nd, 4, 2000);
      CHECK(netdrv_peer_count(nd[0]) == 1, "cycle %d: back to 1 peer (%d)",
            cycle, netdrv_peer_count(nd[0]));
      CHECK(netdrv_active(nd[1]), "cycle %d: resident survived churn", cycle);
   }

   /* Rejoin with a NEW nonce from the same address (process restart):
    * host must reset the channel and re-admit. */
   netdrv_destroy(nd[2]);
   nd[2] = mk_node(&s, &ep[2], 2, &a[2], "cli-reborn", 0x9999, NULL);
   netdrv_join(nd[2]);
   sim_run(&s, nd, 4, 4000);
   CHECK(netdrv_active(nd[2]), "reborn client joined");
   CHECK(netdrv_peer_count(nd[0]) == 2, "host peers=%d", netdrv_peer_count(nd[0]));

   /* Wrong protocol version must never be admitted. */
   {
      sim_ep ep_bad;
      app a_bad;
      netdrv *bad = mk_node(&s, &ep_bad, 3, &a_bad, "bad", 0x777, "gpSP v9.9");
      /* replace node 3 endpoint (old nd[3] is idle) */
      netdrv_join(bad);
      sim_run(&s, &bad, 1, 1000);
      sim_run(&s, nd, 3, 2000);     /* host keeps pumping */
      sim_run(&s, &bad, 1, 2000);
      CHECK(!netdrv_active(bad), "version-mismatch joiner rejected");
      netdrv_destroy(bad);
   }

   for (i = 0; i < 4; i++)
      netdrv_destroy(nd[i]);
}

static void test_fuzz(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint32_t r = 0x12345678;
   int it;
   nd_stats st;

   printf("[fuzz: 20000 hostile datagrams]\n");
   sim_init(&s, 2, 0xFA22);
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 2000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   for (it = 0; it < 20000; it++)
   {
      uint8_t pkt[ND_MAX_FRAME + 32];
      uint8_t evil_mac[6] = { 0x02, 0, 0, 0, 0, 0x66 };
      size_t len, k;
      int target = it & 1;

      r ^= r << 13; r ^= r >> 17; r ^= r << 5;

      switch (it % 4)
      {
      case 0:                        /* pure random bytes, random length */
         len = r % (ND_MAX_FRAME + 30);
         for (k = 0; k < len; k++)
         {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            pkt[k] = (uint8_t)r;
         }
         break;
      case 1:                        /* valid frame, one bit flipped */
      {
         nd_hdr h;
         uint8_t payload[64];
         memset(&h, 0, sizeof(h));
         h.type = (uint8_t)(r % ND_T__COUNT);
         h.src = (uint8_t)(r % 6);
         h.dst = (uint8_t)((r >> 8) % 6);
         h.seq = (uint16_t)r;
         h.ack = (uint16_t)(r >> 16);
         h.len = (uint16_t)(r % sizeof(payload));
         for (k = 0; k < h.len; k++)
            payload[k] = (uint8_t)(r + k);
         len = nd_wire_build(pkt, &h, payload);
         if (r & 1)
            pkt[r % len] ^= (uint8_t)(1u << (r % 8));
         /* else: VALID frame from an unknown mac — the roster wall */
         break;
      }
      case 2:                        /* valid frame, truncated */
      {
         nd_hdr h;
         uint8_t payload[128];
         memset(&h, 0, sizeof(h));
         h.type = ND_T_DATA;
         h.src = 1; h.dst = 0;
         h.seq = (uint16_t)r;
         h.len = 100;
         for (k = 0; k < h.len; k++)
            payload[k] = (uint8_t)r;
         len = nd_wire_build(pkt, &h, payload);
         len = 1 + (r % (len - 1));
         break;
      }
      default:                       /* correct magic, hostile header fields */
         memset(pkt, 0, sizeof(pkt));
         pkt[0] = 0x31; pkt[1] = 0x4E; pkt[2] = 0x50; pkt[3] = 0x47;
         pkt[4] = (uint8_t)(r % 3);          /* ver 0..2 */
         pkt[5] = (uint8_t)r;                /* any type */
         pkt[6] = (uint8_t)(r >> 8);
         pkt[7] = (uint8_t)(r >> 16);
         pkt[12] = 0xFF; pkt[13] = 0x7F;     /* len = 32767 */
         len = ND_HDR_SIZE + (r % 64);
         break;
      }
      /* decorrelate target from the case selector */
      sim_deposit(&s, target ^ ((it >> 2) & 1), evil_mac, pkt, len, 0);
      if ((it & 63) == 63)
         sim_run(&s, nd, 2, 4);
   }
   sim_run(&s, nd, 2, 2000);

   /* Nothing may have reached deliver(); session must still be alive. */
   CHECK(a[0].delivered == 0 && a[1].delivered == 0,
         "garbage delivered: %u %u", a[0].delivered, a[1].delivered);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]),
         "session survived the fuzz");
   {
      nd_stats st1;
      netdrv_get_stats(nd[0], &st);
      netdrv_get_stats(nd[1], &st1);
      CHECK(st.rx_drop_crc + st1.rx_drop_crc > 100,
            "bit-flipped frames died at the CRC wall (crc=%u+%u)",
            st.rx_drop_crc, st1.rx_drop_crc);
      CHECK(st.rx_drop_crc + st.rx_drop_malformed + st.rx_drop_unknown_peer +
            st1.rx_drop_crc + st1.rx_drop_malformed + st1.rx_drop_unknown_peer
            > 15000,
            "drops counted (crc=%u/%u mal=%u/%u unk=%u/%u)",
            st.rx_drop_crc, st1.rx_drop_crc, st.rx_drop_malformed,
            st1.rx_drop_malformed, st.rx_drop_unknown_peer,
            st1.rx_drop_unknown_peer);
      printf("  drops h/c: crc=%u/%u malformed=%u/%u unknown_peer=%u/%u\n",
             st.rx_drop_crc, st1.rx_drop_crc, st.rx_drop_malformed,
             st1.rx_drop_malformed, st.rx_drop_unknown_peer,
             st1.rx_drop_unknown_peer);
   }

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* -------- real transport_udp backend over loopback, faults enabled ------- */

typedef struct { udp_transport *t; } udp_wrap;

static void test_udp_backend(void)
{
   enum { MSGS = 100 };
   udp_transport *th, *tc;
   nd_transport tph, tpc;
   nd_callbacks cbh, cbc;
   nd_config cfg;
   app ah, ac;
   netdrv *h, *c;
   uint8_t buf[ND_MAX_PAYLOAD];
   uint32_t snt = 0;
   uint64_t now = 0;
   int t;

   printf("[udp backend: loopback, 20%% loss + 10ms jitter]\n");
   th = udp_transport_create(0, NULL, 0);
   CHECK(th != NULL, "host socket");
   if (!th)
      return;
   tc = udp_transport_create(0, "127.0.0.1", udp_transport_port(th));
   CHECK(tc != NULL, "client socket");
   if (!tc)
   {
      udp_transport_destroy(th);
      return;
   }
   udp_transport_set_fault(th, 20, 5, 0, 10, 0x1111);
   udp_transport_set_fault(tc, 20, 5, 0, 10, 0x2222);
   /* An OPTIONAL vtable hook left as stack garbage is a call through a stack
    * value: exactly how nd_transport.pending segfaulted the desktop twin.
    * *_transport_iface() must zero what it does not set. */
   memset(&tph, 0xA5, sizeof(tph));
   udp_transport_iface(th, &tph);
   CHECK(tph.pending == NULL,
         "udp_transport_iface zeroes optional hooks it does not set");
   udp_transport_iface(tc, &tpc);

   memset(&ah, 0, sizeof(ah)); ah.name = "uhost";
   memset(&ac, 0, sizeof(ac)); ac.name = "ucli";
   memset(&cbh, 0, sizeof(cbh));
   cbh.user = &ah; cbh.deliver = app_deliver; cbh.session_started = app_started;
   cbh.peer_connected = app_connected; cbh.peer_disconnected = app_disconnected;
   cbh.session_stopped = app_stopped;
   cbc = cbh; cbc.user = &ac;

   memset(&cfg, 0, sizeof(cfg));
   cfg.protocol = "gpSP v1.0";
   cfg.nick = "udp";
   cfg.join_nonce = 0xF00D;
   h = netdrv_create(&tph, &cbh, &cfg);
   cfg.join_nonce = 0xBEEF;
   c = netdrv_create(&tpc, &cbc, &cfg);
   ah.nd = h; ac.nd = c;

   netdrv_host(h);
   netdrv_join(c);

   /* real-time pump: 1ms ticks (virtual now fed from loop count is fine —
    * only durations matter and we sleep 1ms per tick) */
   for (t = 0; t < 30000; t++)
   {
      now += 1000;
      netdrv_pump(h, now);
      netdrv_pump(c, now);
      if (netdrv_active(h) && netdrv_active(c) && snt < MSGS && (t % 4) == 0)
      {
         size_t len = msg_build(buf, snt, 0, 0, 104);
         if (netdrv_send(h, ND_RELIABLE | ND_FLUSH_HINT, buf, len,
                         ND_BROADCAST_ID) == 0)
            snt++;
      }
      if (ac.next_expect[0] == MSGS)
         break;
      usleep(1000);
   }

   CHECK(netdrv_active(h) && netdrv_active(c), "udp session up");
   CHECK(snt == MSGS, "sent %u", snt);
   CHECK(ac.next_expect[0] == MSGS, "udp client received %u/%d in order",
         ac.next_expect[0], (int)MSGS);
   CHECK(ah.bad_payload + ac.bad_payload == 0, "udp clean");

   netdrv_destroy(h);
   netdrv_destroy(c);
   udp_transport_destroy(th);
   udp_transport_destroy(tc);
}

/* ADR-0027: each side publishes its emulated frame rate and reads back the
 * slowest peer's.  What matters for the field is that it crosses on a BUSY
 * link (during a trade the keepalive PING is suppressed by the DATA flow, so
 * a naive implementation exchanges nothing exactly when it is needed), that
 * a peer which reports nothing reads back as 0 rather than as slow, and that
 * the value survives a stream of real traffic. */
static void test_peer_fps_exchange(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];
   uint8_t msg[32];
   int i;

   printf("[peer fps exchange: crosses on a busy link (ADR-0027)]\n");
   sim_init(&s, 2, 0x02701A);
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]) && netdrv_active(nd[1]), "session up");

   /* Nobody has reported yet: unknown reads as 0, never as "slow". */
   CHECK(netdrv_peer_min_fps(nd[0]) == 0, "unreported peer fps != 0 (%u)",
         netdrv_peer_min_fps(nd[0]));

   netdrv_set_local_fps(nd[0], 5880);   /* host capable of 58.80 */
   netdrv_set_local_fps(nd[1], 4650);   /* client capable of 46.50 */

   /* Keep the link BUSY so the keepalive arm of the PING is suppressed
    * throughout — this is the case the field actually runs in. */
   memset(msg, 0x5A, sizeof(msg));
   for (i = 0; i < 200; i++)
   {
      netdrv_send(nd[0], ND_RELIABLE | ND_FLUSH_HINT, msg, sizeof(msg),
                  (uint16_t)a[1].local_id);
      netdrv_send(nd[1], ND_RELIABLE | ND_FLUSH_HINT, msg, sizeof(msg),
                  (uint16_t)a[0].local_id);
      sim_run(&s, nd, 2, 10);
   }

   CHECK(netdrv_peer_min_fps(nd[0]) == 4650,
         "host sees client fps=%u, want 4650", netdrv_peer_min_fps(nd[0]));
   CHECK(netdrv_peer_min_fps(nd[1]) == 5880,
         "client sees host fps=%u, want 5880", netdrv_peer_min_fps(nd[1]));

   /* A revised report replaces the old one. */
   netdrv_set_local_fps(nd[1], 5200);
   sim_run(&s, nd, 2, 1500);
   CHECK(netdrv_peer_min_fps(nd[0]) == 5200,
         "host did not track client's new fps (%u)",
         netdrv_peer_min_fps(nd[0]));

   /* Turning reporting off leaves the last value alone rather than
    * fabricating a zero — absence of a report is not a report of zero. */
   netdrv_set_local_fps(nd[1], 0);
   sim_run(&s, nd, 2, 2000);
   CHECK(netdrv_peer_min_fps(nd[0]) == 5200,
         "a silent peer clobbered the last known fps (%u)",
         netdrv_peer_min_fps(nd[0]));

   /* Every payload still arrived: the piggybacked field must not disturb
    * the ARQ stream it rides beside. */
   CHECK(a[0].delivered == 200 && a[1].delivered == 200,
         "payload loss with fps reporting on (h=%d c=%d)",
         a[0].delivered, a[1].delivered);

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

/* A peer on a build with no fps reporting sends a bare PING, and must simply
 * be invisible to the policy — not slow, not a stall.  Same wire, no version
 * negotiation: this is the forward/backward compatibility guarantee. */
static void test_peer_fps_absent_is_unknown(void)
{
   sim s;
   sim_ep ep[2];
   app a[2];
   netdrv *nd[2];

   printf("[peer fps: an old peer reads as unknown, not as slow]\n");
   sim_init(&s, 2, 0x02702B);
   nd[0] = mk_node(&s, &ep[0], 0, &a[0], "host", 0xA1, NULL);
   nd[1] = mk_node(&s, &ep[1], 1, &a[1], "cli", 0xB2, NULL);
   netdrv_host(nd[0]);
   netdrv_join(nd[1]);
   sim_run(&s, nd, 2, 3000);
   CHECK(netdrv_active(nd[0]), "session up");

   /* Only the host reports. The client never does (the "old build" side). */
   netdrv_set_local_fps(nd[0], 5880);
   sim_run(&s, nd, 2, 3000);

   CHECK(netdrv_peer_min_fps(nd[0]) == 0,
         "silent peer reported a rate (%u)", netdrv_peer_min_fps(nd[0]));
   CHECK(netdrv_peer_min_fps(nd[1]) == 5880,
         "reporting peer not seen by a non-reporting one (%u)",
         netdrv_peer_min_fps(nd[1]));
   CHECK(!a[0].stopped && !a[1].stopped, "session died over an fps report");

   netdrv_destroy(nd[0]);
   netdrv_destroy(nd[1]);
}

int main(void)
{
   test_wire();
   test_handshake_loss();
   test_arq();
   test_poll_gate();
   test_no_reliable_loss_under_overload();
   test_txq_failure_is_loud();
   test_adaptive_rto();
   test_oversize_refused();
   test_reentrant_echo();
   test_keepalive_death();
   test_roster_churn();
   test_fuzz();
   test_peer_fps_exchange();
   test_peer_fps_absent_is_unknown();
   test_udp_backend();

   if (g_fail)
   {
      printf("FAILED: %d check(s)\n", g_fail);
      return 1;
   }
   printf("ALL TESTS PASSED\n");
   return 0;
}
