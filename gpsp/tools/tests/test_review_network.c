/* Actual-source regressions for session admission. Link with netdrv.c. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "netdrv.h"
#include "netdrv_wire.h"

static const uint8_t local_mac[6] = {2,0,0,0,0,2};
static const uint8_t host_mac[6] = {2,0,0,0,0,1};
static const uint8_t other_mac[6] = {2,0,0,0,0,3};
static const uint32_t local_nonce = 0x12345678;
static uint8_t pending[ND_MAX_FRAME];
static int pending_len, starts, stops, connections, disconnections, reason, failures;
static uint64_t now;

#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; \
} } while (0)

static int tx(void *u, const uint8_t mac[6], const void *buf, size_t n)
{ (void)u; (void)mac; (void)buf; (void)n; return 0; }
static int broadcast(void *u, const void *buf, size_t n)
{ return tx(u, host_mac, buf, n); }
static int rx(void *u, uint8_t mac[6], void *buf, size_t cap)
{
   int n = pending_len; (void)u;
   if (!n || (size_t)n > cap) return 0;
   memcpy(mac, host_mac, 6); memcpy(buf, pending, n); pending_len = 0;
   return n;
}
static void addr(void *u, uint8_t mac[6])
{ (void)u; memcpy(mac, local_mac, 6); }
static void on_start(void *u, uint8_t id)
{ (void)u; CHECK(id == 1); starts++; }
static void on_stop(void *u, int why)
{ (void)u; stops++; reason = why; }
static int on_connect(void *u, uint8_t id)
{ (void)u; CHECK(id == 0 || id == 2); connections++; return 0; }
static void on_disconnect(void *u, uint8_t id)
{ (void)u; (void)id; disconnections++; }

static netdrv *joiner(void)
{
   nd_transport tp; nd_callbacks cb; nd_config cfg;
   netdrv *nd;
   memset(&tp, 0, sizeof(tp)); memset(&cb, 0, sizeof(cb));
   memset(&cfg, 0, sizeof(cfg));
   tp.send_to = tx; tp.broadcast = broadcast; tp.recv = rx; tp.local_addr = addr;
   cb.session_started = on_start; cb.session_stopped = on_stop;
   cb.peer_connected = on_connect;
   cb.peer_disconnected = on_disconnect;
   cfg.protocol = "admission-review"; cfg.nick = "joiner";
   cfg.join_nonce = local_nonce;
   pending_len = starts = stops = connections = disconnections = reason = 0;
   now = 1000000;
   nd = netdrv_create(&tp, &cb, &cfg);
   CHECK(nd != NULL);
   if (nd) { netdrv_join(nd); netdrv_pump(nd, now); }
   if (nd) {
      /* the host's version comes before its OFFER (netdrv_wire.h) */
      uint8_t caps[ND_CAPS_SIZE] = { ND_CAPS_OK, 0, ND_LINK_VER };
      nd_hdr h; memset(&h, 0, sizeof(h));
      h.type = ND_T_CAPS; h.src = 0; h.dst = ND_ID_JOINING;
      h.len = ND_CAPS_SIZE;
      h.src_nonce = 0x11223344; h.dst_nonce = local_nonce;
      pending_len = (int)nd_wire_build(pending, &h, caps);
      now += 10000; netdrv_pump(nd, now);
      memset(&h, 0, sizeof(h));
      h.type = ND_T_OFFER; h.src = 0; h.dst = ND_ID_JOINING;
      h.src_nonce = 0x11223344; h.dst_nonce = local_nonce; h.seq = 42;
      pending_len = (int)nd_wire_build(pending, &h, NULL);
      now += 10000; netdrv_pump(nd, now);
   }
   return nd;
}
static void entry(uint8_t *e, uint8_t id, const uint8_t mac[6], uint32_t nonce)
{
   memset(e, 0, ND_ROSTER_ENTRY_SIZE); e[0] = id; memcpy(e + 1, mac, 6);
   e[7] = (uint8_t)nonce; e[8] = (uint8_t)(nonce >> 8);
   e[9] = (uint8_t)(nonce >> 16); e[10] = (uint8_t)(nonce >> 24);
   memcpy(e + 11, "test", 5);
}
static unsigned welcome(uint8_t *pl)
{
   memset(pl, 0, ND_ROSTER_MAX_SIZE);
   pl[0] = 1; pl[1] = 1; pl[2] = 2;
   entry(pl + 3, 0, host_mac, 0x11223344);
   entry(pl + 3 + ND_ROSTER_ENTRY_SIZE, 1, local_mac, local_nonce);
   return 3 + 2 * ND_ROSTER_ENTRY_SIZE;
}
static void inject_type(netdrv *nd, uint8_t type, const uint8_t *pl,
                        unsigned len, uint8_t src, uint8_t dst)
{
   nd_hdr h; memset(&h, 0, sizeof(h));
   h.type = type; h.src = src; h.dst = dst; h.len = (uint16_t)len;
   h.src_nonce = 0x11223344;
   h.dst_nonce = type == ND_T_ROSTER ? 0 : local_nonce;
   if (type == ND_T_WELCOME) h.seq = 42;
   pending_len = (int)nd_wire_build(pending, &h, pl);
   now += 10000; netdrv_pump(nd, now);
}
static void inject(netdrv *nd, const uint8_t *pl, unsigned len,
                   uint8_t src, uint8_t dst)
{ inject_type(nd, ND_T_WELCOME, pl, len, src, dst); }
static void accepted(netdrv *nd)
{
   uint8_t mac[6];
   CHECK(netdrv_active(nd)); CHECK(netdrv_peer_count(nd) == 1);
   CHECK(starts == 1); CHECK(stops == 0); CHECK(connections == 1);
   CHECK(netdrv_peer_mac(nd, 0, mac) == 0);
   CHECK(memcmp(mac, host_mac, 6) == 0);
}

static void malformed_welcomes(void)
{
   static const char *names[] = {
      "truncated roster", "hostless roster", "empty roster", "too many entries",
      "duplicate id", "duplicate MAC", "invalid peer id", "missing local entry",
      "wrong local MAC", "stale local nonce", "wrong host MAC", "host assigned to joiner",
      "invalid assigned id", "wrong header source", "wrong header destination",
      "truncated refusal", "hostless refusal", "trailing roster data", "short payload"
   };
   unsigned k;
   for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
   {
      uint8_t pl[ND_ROSTER_MAX_SIZE], good[ND_ROSTER_MAX_SIZE];
      unsigned len = welcome(pl), good_len = welcome(good);
      uint8_t src = 0, dst = ND_ID_JOINING;
      uint8_t *self = pl + 3 + ND_ROSTER_ENTRY_SIZE;
      nd_stats st;
      int before = failures;
      netdrv *nd = joiner();
      if (!nd) return;
      switch (k)
      {
         case 0: len = 3; break;
         case 1: memcpy(pl + 3, self, ND_ROSTER_ENTRY_SIZE); pl[2] = 1; len -= ND_ROSTER_ENTRY_SIZE; break;
         case 2: pl[2] = 0; len = 3; break;
         case 3: pl[2] = ND_MAX_CLIENTS + 1; break;
         case 4: pl[2] = 3; entry(self + ND_ROSTER_ENTRY_SIZE, 1, other_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 5: pl[2] = 3; entry(self + ND_ROSTER_ENTRY_SIZE, 2, local_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 6: pl[2] = 3; entry(self + ND_ROSTER_ENTRY_SIZE, ND_MAX_CLIENTS, other_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 7: pl[2] = 1; len -= ND_ROSTER_ENTRY_SIZE; break;
         case 8: memcpy(self + 1, other_mac, 6); break;
         case 9: self[7] ^= 1; break;
         case 10: memcpy(pl + 4, other_mac, 6); break;
         case 11: pl[0] = 0; break;
         case 12: pl[0] = ND_MAX_CLIENTS; break;
         case 13: src = 1; break;
         case 14: dst = 1; break;
         case 15: pl[0] = ND_ID_REFUSED; len = 3; break;
         case 16: pl[0] = ND_ID_REFUSED; memcpy(pl + 3, self, ND_ROSTER_ENTRY_SIZE); pl[2] = 1; len -= ND_ROSTER_ENTRY_SIZE; break;
         case 17: len++; break;
         case 18: len = 2; break;
      }
      inject(nd, pl, len, src, dst);
      CHECK(!netdrv_active(nd)); CHECK(netdrv_peer_count(nd) == 0);
      CHECK(starts == 0); CHECK(stops == 0); CHECK(connections == 0);
      netdrv_get_stats(nd, &st); CHECK(st.rx_drop_malformed == 1);
      /* Rejection must preserve JOINING, so the real host can still admit us. */
      inject(nd, good, good_len, 0, ND_ID_JOINING); accepted(nd);
      if (failures != before) fprintf(stderr, "case failed: %s\n", names[k]);
      netdrv_destroy(nd);
   }
}
static void valid_welcomes(void)
{
   uint8_t pl[ND_ROSTER_MAX_SIZE];
   unsigned len = welcome(pl);
   netdrv *nd = joiner();
   if (!nd) return;
   inject(nd, pl, len, 0, ND_ID_JOINING); accepted(nd);
   /* An identical retransmission must not restart an active session. */
   inject(nd, pl, len, 0, ND_ID_JOINING); accepted(nd);
   netdrv_destroy(nd);
   nd = joiner(); if (!nd) return;
   len = welcome(pl); pl[0] = ND_ID_REFUSED; pl[2] = 1;
   len -= ND_ROSTER_ENTRY_SIZE;
   inject(nd, pl, len, 0, ND_ID_JOINING);
   CHECK(!netdrv_active(nd)); CHECK(starts == 0); CHECK(connections == 0);
   CHECK(stops == 1); CHECK(reason == ND_STOP_REFUSED);
   /* A refused session is idle and cannot be admitted by a late WELCOME. */
   len = welcome(pl); inject(nd, pl, len, 0, ND_ID_JOINING);
   CHECK(!netdrv_active(nd)); CHECK(starts == 0); CHECK(stops == 1);
   netdrv_destroy(nd);
}
static void roster_updates(void)
{
   unsigned k;
   for (k = 0; k < 9; k++)
   {
      uint8_t pl[ND_ROSTER_MAX_SIZE];
      unsigned len = welcome(pl);
      uint8_t src = 0, dst = ND_ID_BCAST;
      nd_stats st;
      netdrv *nd = joiner(); if (!nd) return;
      inject(nd, pl, len, 0, ND_ID_JOINING); accepted(nd);
      pl[0] = ND_ID_BCAST; pl[1] = 2;
      switch (k)
      {
         case 0: len = 3; break;
         case 1: memcpy(pl + 3, pl + 3 + ND_ROSTER_ENTRY_SIZE, ND_ROSTER_ENTRY_SIZE); pl[2] = 1; len -= ND_ROSTER_ENTRY_SIZE; break;
         case 2: pl[2] = 3; entry(pl + len, 1, other_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 3: pl[2] = 3; entry(pl + len, 2, local_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 4: pl[2] = 3; entry(pl + len, ND_MAX_CLIENTS, other_mac, 7); len += ND_ROSTER_ENTRY_SIZE; break;
         case 5: memcpy(pl + 4, other_mac, 6); break;
         case 6: pl[0] = 1; break;
         case 7: src = 2; break;
         case 8: dst = 1; break;
      }
      inject_type(nd, ND_T_ROSTER, pl, len, src, dst); accepted(nd);
      CHECK(disconnections == 0);
      netdrv_get_stats(nd, &st); CHECK(st.rx_drop_malformed == 1);
      /* The rejected update must not consume generation 2 or affect peers. */
      len = welcome(pl); pl[0] = ND_ID_BCAST; pl[1] = 2; pl[2] = 3;
      entry(pl + len, 2, other_mac, 7); len += ND_ROSTER_ENTRY_SIZE;
      inject_type(nd, ND_T_ROSTER, pl, len, 0, ND_ID_BCAST);
      CHECK(netdrv_active(nd)); CHECK(netdrv_peer_count(nd) == 2);
      CHECK(connections == 2); CHECK(stops == 0);
      /* A subsequent genuine removal fires exactly one disconnect. */
      len -= ND_ROSTER_ENTRY_SIZE; pl[2] = 2; pl[1] = 3;
      inject_type(nd, ND_T_ROSTER, pl, len, 0, ND_ID_BCAST);
      CHECK(netdrv_peer_count(nd) == 1); CHECK(disconnections == 1);
      netdrv_destroy(nd);
   }
   /* Removed/replaced local membership must end the old instance. */
   for (k = 0; k < 3; k++)
   {
      uint8_t pl[ND_ROSTER_MAX_SIZE]; unsigned len = welcome(pl);
      netdrv *nd = joiner(); if (!nd) return;
      inject(nd, pl, len, 0, ND_ID_JOINING);
      pl[0] = ND_ID_BCAST; pl[1] = 2;
      if (k == 0) { pl[2] = 1; len -= ND_ROSTER_ENTRY_SIZE; }
      if (k == 1) pl[3 + ND_ROSTER_ENTRY_SIZE + 7] ^= 1;
      if (k == 2) memcpy(pl + 4 + ND_ROSTER_ENTRY_SIZE, other_mac, 6);
      inject_type(nd, ND_T_ROSTER, pl, len, 0, ND_ID_BCAST);
      CHECK(!netdrv_active(nd)); CHECK(stops == 1);
      CHECK(reason == ND_STOP_EVICTED); CHECK(disconnections == 1);
      netdrv_destroy(nd);
   }
   /* Same-generation periodic rosters are valid liveness; malformed ones
    * must not postpone host-loss detection. */
   for (k = 0; k < 2; k++)
   {
      uint8_t pl[ND_ROSTER_MAX_SIZE]; unsigned len = welcome(pl);
      netdrv *nd = joiner(); if (!nd) return;
      inject(nd, pl, len, 0, ND_ID_JOINING);
      pl[0] = ND_ID_BCAST; now += 7000000;
      inject_type(nd, ND_T_ROSTER, pl, k ? len : 3, 0, ND_ID_BCAST);
      now += 2000000; netdrv_pump(nd, now);
      CHECK(netdrv_active(nd) == (int)k);
      CHECK(stops == (k ? 0 : 1));
      if (!k) CHECK(reason == ND_STOP_HOST_LOST);
      netdrv_destroy(nd);
   }
}
int main(void)
{
   malformed_welcomes(); valid_welcomes(); roster_updates();
   printf("network admission regressions: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
