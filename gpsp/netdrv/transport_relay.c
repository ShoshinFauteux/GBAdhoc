/* transport_relay.c -- netdrv over a GBR1 relay, with a direct hole-punched
 * path and relay fallback.  See transport_relay.h and docs/ONLINE-TUNNEL.md
 * section 5 ("Direct path") for the rules implemented here:
 *
 *   PROBING   PROBE the server's port and port + 1 from our one socket; the
 *             same public port twice = endpoint-independent mapping (EIM),
 *             two ports = symmetric.  1 s budget.
 *   ENTERING  CREATE or JOIN (resent every 500 ms; a JOIN to a room that does
 *             not exist yet keeps trying -- the host may still be coming up).
 *   IN_ROOM   KEEP to the server every 2 s.  Per peer, on CANDIDATES with
 *             punch = 1: from start, PUNCH every 20 ms for 2 s of PUMPED time
 *             (a stall of the caller extends the window) to each distinct
 *             candidate: private (same LAN), public, the router forward, and
 *             the address the peer's own punches came from (peer-
 *             reflexive).  Answer EVERY PUNCH with PUNCH_ACK, in any state;
 *             a punch arriving while we are on the relay starts our burst
 *             at once.  The path is DIRECT on the first PUNCH_ACK (the round
 *             trip), and moves to the private endpoint if that answers
 *             later.  On DIRECT: a PUNCH keep-alive whenever we sent nothing
 *             direct for 500 ms; FALL BACK to the relay when nothing direct
 *             arrived for 1.5 s while the server still answers, when no
 *             PUNCH_ACK came in the burst, or when a direct peer says it fell
 *             back (PEER RELAY).  The relay is the last resort, not the end:
 *             another burst follows after 3 s, then 6, 12, 24, 30, 30 ... s.
 */
#include "transport_relay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { P_NONE = 0, P_RELAY = 1, P_PUNCH = 2, P_DIRECT = 3, P_FALLBACK = 4 };

#define PROBE_EVERY_US   200000ull
#define PROBE_TRIES      5
#define ENTER_EVERY_US   500000ull
#define KEEP_EVERY_US    2000000ull
#define PUNCH_EVERY_US   20000ull
#define PUNCH_FOR_US     2000000ull  /* of PUMPED time: a stall extends it */
#define STALL_US         100000ull   /* a gap between timer runs this long
                                        is a stall (a save backup, a load) */
#define RETRY_FIRST_US   3000000ull  /* relay -> another punch burst after */
#define RETRY_MAX_US     30000000ull /*   3, 6, 12, 24, 30, 30 ... s */
#define DIRECT_IDLE_US   500000ull
#define DIRECT_DEAD_US   1500000ull
#define RELAY_ALIVE_US   6000000ull
#define LOBBY_LOST_US    8000000ull  /* 4 unanswered KEEPs: the room is gone */
#define CJ_PAD           64          /* CREATE/JOIN/PROBE padded: the server
                                        never answers larger than asked */
#define TS_RING          64

typedef struct
{
   int present, path;
   uint32_t pub_ip, priv_ip, map_ip, rfx_ip, dir_ip;
   uint16_t pub_port, priv_port, map_port, rfx_port, dir_port;
   uint64_t retry_us, backoff_us;   /* next punch burst while on the relay */
   uint64_t nonce;
   int punch;
   uint64_t start_us, end_us, next_punch_us;
   uint32_t counter;
   uint64_t sent_us[TS_RING];
   uint64_t last_rx_us, last_tx_us;
   uint32_t rtt_us;
} rl_peer;

struct relay_transport
{
   rl_sock s;
   rl_client_cfg cfg;
   int state, err;
   char errtext[32];
   uint64_t t0;
   uint64_t last_tick_us;        /* the last timers() run (stall detection) */
   /* probing */
   uint32_t probe_nonce;
   int probe_tries, probed[2];
   uint16_t pub_port[2];
   uint32_t pub_ip;
   uint8_t nat;
   uint64_t next_us;
   uint64_t cookie;              /* the server's COOKIE, echoed in CREATE/JOIN */
   int room_mode;                /* RL_MODE_* from CREATED/JOINED */
   /* room */
   int slot;
   uint64_t token;
   char code[RL_CODE_LEN + 1];
   uint64_t next_keep_us, last_server_rx_us;
   int reentering;               /* host: lost its lobby room, CREATE again */
   uint64_t reenter_us;
   uint32_t keep_ctr;
   uint64_t keep_sent_us;
   rl_peer p[RL_SLOTS];
   rl_client_stats st;
};

static uint64_t now(relay_transport *t) { return t->cfg.now_us(); }
static int relay_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap);

static void lg(relay_transport *t, const char *fmt, ...)
#if defined(__GNUC__)
   __attribute__((format(printf, 2, 3)))
#endif
   ;
static void lg(relay_transport *t, const char *fmt, ...)
{
   char b[192];
   va_list a;
   if (!t->cfg.log)
      return;
   va_start(a, fmt);
   vsnprintf(b, sizeof(b), fmt, a);
   va_end(a);
   t->cfg.log(t->cfg.log_user, b);
}

static void ipfmt(uint32_t ip_be, uint16_t port, char *o)
{
   const uint8_t *p = (const uint8_t *)&ip_be;
   sprintf(o, "%u.%u.%u.%u:%u", p[0], p[1], p[2], p[3], port);
}

static void raw(relay_transport *t, uint32_t ip, uint16_t port, const uint8_t *b, size_t n)
{
   t->st.tx++;
   t->s.send(t->s.ctx, ip, port, b, n);
}

static void to_server(relay_transport *t, int sock, uint8_t *b, size_t n)
{
   t->st.tx_relay++;
   raw(t, t->cfg.server_ip_be, (uint16_t)(t->cfg.server_port + sock), b, n);
}

static void send_probe(relay_transport *t)
{
   uint8_t b[CJ_PAD];
   int k;
   memset(b, 0, sizeof(b));
   rl_hdr(b, RL_T_PROBE, RL_SLOT_NONE, RL_SLOT_NONE, 0);
   rl_put32(b + RL_HDR, t->probe_nonce);
   for (k = 0; k < 2; k++)
      if (!t->probed[k])
         to_server(t, k, b, sizeof(b));
}

static void send_enter(relay_transport *t)
{
   uint8_t b[CJ_PAD];
   uint8_t *p = b + RL_HDR;
   memset(b, 0, sizeof(b));
   rl_hdr(b, t->cfg.is_host ? RL_T_CREATE : RL_T_JOIN, RL_SLOT_NONE,
          RL_SLOT_NONE, 0);
   memcpy(p, t->cfg.build, RL_BUILD_LEN);
   memcpy(p + RL_BUILD_LEN, t->cfg.code, strlen(t->cfg.code) >= RL_CODE_LEN
                                            ? RL_CODE_LEN : 0);
   rl_put_ip(p + RL_BUILD_LEN + RL_CODE_LEN, t->cfg.priv_ip_be);
   rl_put16(p + RL_BUILD_LEN + RL_CODE_LEN + 4, t->cfg.priv_port);
   p[RL_BUILD_LEN + RL_CODE_LEN + 6] = t->nat;
   rl_put64(p + RL_BUILD_LEN + RL_CODE_LEN + 7, t->cookie);
   p[RL_BUILD_LEN + RL_CODE_LEN + 15] = (uint8_t)t->cfg.mode;
   rl_put16(p + RL_BUILD_LEN + RL_CODE_LEN + 16, t->cfg.map_port);
   to_server(t, 0, b, sizeof(b));
}

static void send_keep(relay_transport *t)
{
   uint8_t b[RL_HDR + 4];
   rl_hdr(b, RL_T_KEEP, t->slot, RL_SLOT_NONE, t->token);
   rl_put32(b + RL_HDR, ++t->keep_ctr);
   t->keep_sent_us = now(t);
   t->st.keeps_tx++;
   to_server(t, 0, b, sizeof(b));
}

static void send_path(relay_transport *t, int peer, int path)
{
   uint8_t b[RL_HDR + 2];
   rl_hdr(b, RL_T_PATH, t->slot, RL_SLOT_NONE, t->token);
   b[RL_HDR] = (uint8_t)peer;
   b[RL_HDR + 1] = (uint8_t)path;
   to_server(t, 0, b, sizeof(b));
}

static void send_punch(relay_transport *t, int j, int type, uint32_t ctr,
                       uint32_t ip, uint16_t port)
{
   uint8_t b[RL_HDR + 4];
   rl_hdr(b, type, t->slot, j, t->p[j].nonce);
   rl_put32(b + RL_HDR, ctr);
   t->st.tx_direct++;
   raw(t, ip, port, b, sizeof(b));
}

static void fall_back(relay_transport *t, int j, const char *why, int tell)
{
   rl_peer *q = &t->p[j];
   if (q->path == P_FALLBACK || q->path == P_RELAY)
      return;
   q->path = P_FALLBACK;
   t->st.fallbacks++;
   /* the relay is the last resort, not the end: punch again later (a peer
    * that was busy, a NAT that settled, a Wi-Fi gap), backing off */
   if (q->punch)
   {
      if (!q->backoff_us)
         q->backoff_us = RETRY_FIRST_US;
      q->retry_us = now(t) + q->backoff_us;
      q->backoff_us = q->backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US
                                                       : q->backoff_us * 2;
   }
   lg(t, "relay_path peer=%d path=relay why=%s retry_ms=%u", j, why,
      q->punch ? (unsigned)((q->retry_us - now(t)) / 1000u) : 0u);
   if (tell)
      send_path(t, j, RL_PATH_RELAY);
}

/* A punch burst to every candidate the peer has, from now. */
static void burst(relay_transport *t, rl_peer *q, uint64_t n)
{
   q->path = P_PUNCH;
   q->start_us = n;
   q->end_us = n + PUNCH_FOR_US;
   q->next_punch_us = n;
   t->st.bursts++;
}

static int ep_is(uint32_t ip, uint16_t port, uint32_t ip2, uint16_t port2)
{
   return ip && ip == ip2 && port == port2;
}

/* One PUNCH to each distinct candidate: private first (same LAN: the
 * shortest path, and the one a router without hairpin needs), then the
 * public endpoint the server saw, the router forward, and the address the
 * peer's own punches came from (peer-reflexive: a symmetric NAT's port for
 * US, which the server never sees). */
static void punch_all(relay_transport *t, int j, uint32_t c)
{
   rl_peer *q = &t->p[j];
   uint32_t ip[4];
   uint16_t pt[4];
   int k, m, n = 0;
   ip[0] = q->priv_ip; pt[0] = q->priv_port;
   ip[1] = q->pub_ip;  pt[1] = q->pub_port;
   ip[2] = q->map_ip;  pt[2] = q->map_port;
   ip[3] = q->rfx_ip;  pt[3] = q->rfx_port;
   for (k = 0; k < 4; k++)
   {
      if (!ip[k] || !pt[k])
         continue;
      for (m = 0; m < k; m++)
         if (ep_is(ip[m], pt[m], ip[k], pt[k]))
            break;
      if (m < k)
         continue;
      send_punch(t, j, RL_T_PUNCH, c, ip[k], pt[k]);
      n++;
   }
   (void)n;
}

/* A host waiting alone in its room (no peer yet) whose room the server no
 * longer has -- it went quiet (our KEEPs unanswered for LOBBY_LOST_US: a
 * Wi-Fi or carrier-NAT gap longer than the server's 20 s slot timeout, or the
 * 10-minute lobby limit) or it says our token is unknown (RL_E_TOKEN) --
 * enters again with the SAME code: the code read out to a friend stays good.
 * If the room is in fact still there, the server answers the CREATE with it
 * (same address) or "code in use" (a new NAT mapping): then we keep the old
 * slot, and the next KEEP re-pins it.  Never with a peer in the room: that
 * is the session's to handle (netdrv's peer timeout). */
static int lobby_alone(const relay_transport *t)
{
   int j;
   if (!t->cfg.is_host || t->state != RL_ST_IN_ROOM)
      return 0;
   for (j = 0; j < RL_SLOTS; j++)
      if (j != t->slot && t->p[j].present)
         return 0;
   return 1;
}

static void lobby_reenter(relay_transport *t, const char *why)
{
   memcpy(t->cfg.code, t->code, sizeof(t->cfg.code));
   t->reentering = 1;
   t->reenter_us = now(t);
   t->cookie = 0;
   t->state = RL_ST_ENTERING;
   t->next_us = 0;
   t->st.reenters++;
   lg(t, "relay_room_lost code=%s why=%s: entering again", t->code, why);
}

static void timers(relay_transport *t)
{
   uint64_t n = now(t);
   int j;
   /* A stall: nobody pumped us (the EBOOT backing a save up, a load, a
    * slow screen).  Nothing was read or punched meanwhile, and what the
    * peer sent waits in the socket: the windows do not count that time.
    * (Measured cause of a same-LAN pair never going direct: the joiner
    * stopped pumping right after JOINED, its 2 s punch window ran out
    * unseen and it never answered the host.) */
   if (t->last_tick_us && n - t->last_tick_us > STALL_US &&
       t->state == RL_ST_IN_ROOM)
   {
      uint64_t gap = n - t->last_tick_us;
      t->st.stalls++;
      for (j = 0; j < RL_SLOTS; j++)
      {
         rl_peer *q = &t->p[j];
         if (!q->present)
            continue;
         if (q->path == P_PUNCH)
         {
            if (q->start_us < t->last_tick_us)
               q->end_us += gap;
            else if (q->end_us - q->start_us < PUNCH_FOR_US)
               q->end_us = q->start_us + PUNCH_FOR_US;
         }
         else if (q->path == P_DIRECT)
            q->last_rx_us += gap;
      }
   }
   t->last_tick_us = n;
   if (lobby_alone(t) && n - t->last_server_rx_us > LOBBY_LOST_US &&
       n - t->reenter_us > LOBBY_LOST_US)
      lobby_reenter(t, "server_silent");
   else if (t->reentering && t->state == RL_ST_ENTERING &&
            n - t->reenter_us > LOBBY_LOST_US / 2)
   {
      /* nothing back at all (Wi-Fi gone): KEEP on the old slot for a while,
       * then CREATE again (every LOBBY_LOST_US), for as long as it lasts */
      t->state = RL_ST_IN_ROOM;
      t->reentering = 0;
   }
   switch (t->state)
   {
   case RL_ST_PROBING:
      if ((t->probed[0] && t->probed[1]) || t->probe_tries >= PROBE_TRIES)
      {
         if (t->probed[0] && t->probed[1])
            t->nat = t->pub_port[0] == t->pub_port[1] ? RL_NAT_EIM
                                                      : RL_NAT_SYMMETRIC;
         else
            t->nat = RL_NAT_UNKNOWN;
         {
            char a[24];
            ipfmt(t->pub_ip, t->pub_port[0], a);
            lg(t, "relay_nat nat=%s public=%s ports=%u/%u",
               t->nat == RL_NAT_EIM ? "eim" :
               t->nat == RL_NAT_SYMMETRIC ? "symmetric" : "unknown", a,
               t->pub_port[0], t->pub_port[1]);
         }
         t->st.nat = t->nat;
         t->state = RL_ST_ENTERING;
         t->next_us = 0;
      }
      else if (n >= t->next_us)
      {
         send_probe(t);
         t->probe_tries++;
         t->next_us = n + PROBE_EVERY_US;
      }
      break;
   case RL_ST_ENTERING:
      if (n >= t->next_us)
      {
         send_enter(t);
         t->next_us = n + ENTER_EVERY_US;
      }
      break;
   case RL_ST_IN_ROOM:
      if (n >= t->next_keep_us)
      {
         send_keep(t);
         t->next_keep_us = n + KEEP_EVERY_US;
      }
      for (j = 0; j < RL_SLOTS; j++)
      {
         rl_peer *q = &t->p[j];
         if (!q->present)
            continue;
         if (q->path == P_PUNCH && n >= q->start_us)
         {
            if (n >= q->end_us)
               fall_back(t, j, "no_ack", 1);
            else if (n >= q->next_punch_us)
            {
               uint32_t c = ++q->counter;
               q->sent_us[c % TS_RING] = n;
               punch_all(t, j, c);
               t->st.punches_tx++;
               q->next_punch_us = n + PUNCH_EVERY_US;
            }
         }
         else if ((q->path == P_FALLBACK || q->path == P_RELAY) && q->punch &&
                  q->retry_us && n >= q->retry_us)
         {
            q->retry_us = 0;
            lg(t, "relay_retry peer=%d", j);
            burst(t, q, n);
         }
         else if (q->path == P_DIRECT)
         {
            if (n - q->last_rx_us > DIRECT_DEAD_US &&
                n - t->last_server_rx_us < RELAY_ALIVE_US)
               fall_back(t, j, "direct_silent", 1);
            else if (n - q->last_tx_us > DIRECT_IDLE_US)
            {
               uint32_t c = ++q->counter;
               q->sent_us[c % TS_RING] = n;
               send_punch(t, j, RL_T_PUNCH, c, q->dir_ip, q->dir_port);
               q->last_tx_us = n;
            }
         }
      }
      break;
   default:
      break;
   }
}

int relay_transport_poll(relay_transport *t)
{
   uint8_t b[RL_MAX_DGRAM];
   uint8_t mac[6];
   /* Draining here would swallow DATA once netdrv runs (recv() does both
    * then).  Before the room exists nothing is DATA, so drain directly. */
   if (t->state != RL_ST_IN_ROOM)
      while (relay_recv(t, mac, b, sizeof(b)) > 0)
         ;
   timers(t);
   return t->state;
}

static void on_server(relay_transport *t, const uint8_t *b, int len, int sock)
{
   const uint8_t *body = b + RL_HDR;
   int n = len - RL_HDR;
   uint64_t tn = now(t);
   t->last_server_rx_us = tn;
   switch (b[5])
   {
   case RL_T_PROBED:
      if (n >= 11 && rl_get32(body) == t->probe_nonce && body[10] < 2 &&
          t->state == RL_ST_PROBING)
      {
         int k = body[10];
         (void)sock;
         t->probed[k] = 1;
         t->pub_ip = rl_get_ip(body + 4);
         t->pub_port[k] = (uint16_t)rl_get16(body + 8);
         t->st.pub_ip_be = t->pub_ip;
         t->st.pub_port[k] = t->pub_port[k];
      }
      break;
   case RL_T_CREATED:
   case RL_T_JOINED:
      if (n >= RL_CODE_LEN + 1 && t->state == RL_ST_ENTERING &&
          (b[5] == RL_T_CREATED) == (t->cfg.is_host != 0))
      {
         memcpy(t->code, body, RL_CODE_LEN);
         t->code[RL_CODE_LEN] = 0;
         t->slot = body[RL_CODE_LEN];
         t->room_mode = n >= RL_CODE_LEN + 2 ? body[RL_CODE_LEN + 1] : RL_MODE_ANY;
         t->token = rl_get64(b + 8);
         t->state = RL_ST_IN_ROOM;
         t->reentering = 0;
         t->next_keep_us = tn + KEEP_EVERY_US;
         lg(t, "relay_room code=%s slot=%d host=%d nat=%u", t->code, t->slot,
            t->cfg.is_host, t->nat);
         if (!t->cfg.is_host)
         {
            t->p[0].present = 1;       /* a room always has its host */
            if (!t->p[0].path)
               t->p[0].path = P_RELAY;
         }
      }
      break;
   case RL_T_ERROR:
      if (n >= 1)
      {
         int code = body[0];
         if (code == RL_E_NO_ROOM && t->state == RL_ST_ENTERING)
            break;                      /* the host is not up yet: retry */
         if (code == RL_E_TOKEN && lobby_alone(t))
         {
            lobby_reenter(t, "token_unknown");
            break;
         }
         if (t->reentering && t->state == RL_ST_ENTERING)
         {
            /* "code in use": our room is still there (a new NAT mapping);
             * keep the old slot -- the next KEEP re-pins it */
            t->state = RL_ST_IN_ROOM;
            t->reentering = 0;
            lg(t, "relay_room_kept code=%s error=%d", t->code, code);
            break;
         }
         if (t->state == RL_ST_ENTERING || t->state == RL_ST_PROBING)
         {
            int tl = n - 1 < (int)sizeof(t->errtext) - 1 ? n - 1
                                                         : (int)sizeof(t->errtext) - 1;
            t->err = code;
            memcpy(t->errtext, body + 1, (size_t)tl);
            t->errtext[tl] = 0;
            t->state = RL_ST_FAILED;
            lg(t, "relay_error code=%d text=\"%s\"", code, t->errtext);
         }
      }
      break;
   case RL_T_PEER:
      if (n >= 2 && body[0] < RL_SLOTS && body[0] != t->slot)
      {
         int j = body[0], stt = body[1];
         rl_peer *q = &t->p[j];
         if (stt == RL_PEER_IN)
         {
            if (!q->present)
            {
               memset(q, 0, sizeof(*q));
               q->present = 1;
               q->path = P_RELAY;
            }
         }
         else if (stt == RL_PEER_LEFT)
         {
            memset(q, 0, sizeof(*q));
            lg(t, "relay_peer slot=%d left", j);
         }
         else if (stt == RL_PEER_RELAY && q->path == P_DIRECT)
            fall_back(t, j, "peer_said", 0);
         /* while punching: our own burst decides (the peer may have
          * given up before we even started -- it answers our punches) */
      }
      break;
   case RL_T_CANDIDATES:
      if (n >= 24 && body[0] < RL_SLOTS && body[0] != t->slot)
      {
         int j = body[0];
         rl_peer *q = &t->p[j];
         char a1[24], a2[24], a3[24];
         if (!q->present)
         {
            memset(q, 0, sizeof(*q));
            q->present = 1;
         }
         q->pub_ip = rl_get_ip(body + 1);
         q->pub_port = (uint16_t)rl_get16(body + 5);
         q->priv_ip = rl_get_ip(body + 7);
         q->priv_port = (uint16_t)rl_get16(body + 11);
         q->nonce = rl_get64(body + 13);
         q->punch = body[23] && t->cfg.direct;
         q->start_us = tn + (uint64_t)rl_get16(body + 21) * 1000u;
         q->end_us = q->start_us + PUNCH_FOR_US;
         q->next_punch_us = q->start_us;
         q->path = q->punch ? P_PUNCH : P_RELAY;
         if (q->punch)
            t->st.bursts++;
         q->backoff_us = RETRY_FIRST_US;
         q->retry_us = 0;
         q->map_ip = 0;
         q->map_port = 0;
         q->rfx_ip = 0;
         q->rfx_port = 0;
         if (n >= 30)
         {
            q->map_ip = rl_get_ip(body + 24);
            q->map_port = (uint16_t)rl_get16(body + 28);
         }
         ipfmt(q->pub_ip, q->pub_port, a1);
         ipfmt(q->priv_ip, q->priv_port, a2);
         ipfmt(q->map_ip, q->map_port, a3);
         lg(t, "relay_candidates peer=%d public=%s private=%s forward=%s punch=%d",
            j, a1, a2, a3, q->punch);
      }
      break;
   case RL_T_COOKIE:
      if (n >= RL_COOKIE_LEN && t->state == RL_ST_ENTERING)
      {
         t->cookie = rl_get64(body);
         send_enter(t);                 /* answer the challenge at once */
         t->next_us = tn + ENTER_EVERY_US;
      }
      break;
   case RL_T_KEEP:
      t->st.keeps_rx++;
      if (n >= 4 && rl_get32(body) == t->keep_ctr)
         t->st.relay_rtt_us = (uint32_t)(tn - t->keep_sent_us);
      break;
   default:
      break;
   }
}

static int peer_of_nonce(relay_transport *t, const uint8_t *b)
{
   int j = b[6];
   uint64_t tok = rl_get64(b + 8);
   if (j >= RL_SLOTS || !t->p[j].present || !t->p[j].nonce || t->p[j].nonce != tok)
      return -1;
   return j;
}

static void slot_mac(int slot, uint8_t mac[6])
{
   mac[0] = 0x02; mac[1] = 0x47; mac[2] = 0x42; mac[3] = 0x52; mac[4] = 0x00;
   mac[5] = (uint8_t)slot;
}

static int relay_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   relay_transport *t = (relay_transport *)ctx;
   uint8_t b[RL_MAX_DGRAM];
   int guard;
   if (t->state == RL_ST_IN_ROOM || t->reentering)
      timers(t);
   for (guard = 0; guard < 64; guard++)
   {
      uint32_t ip = 0;
      uint16_t port = 0;
      int len = t->s.recv(t->s.ctx, &ip, &port, b, sizeof(b));
      int from_server;
      if (len <= 0)
         return len < 0 ? 0 : 0;
      t->st.rx++;
      if (rl_check(b, len) != 0)
      {
         t->st.rx_bad++;
         continue;
      }
      from_server = ip == t->cfg.server_ip_be &&
                    (port == t->cfg.server_port || port == t->cfg.server_port + 1);
      if (from_server)
      {
         if (b[5] == RL_T_DATA)
         {
            int n = len - RL_HDR;
            if (t->state != RL_ST_IN_ROOM || b[6] >= RL_SLOTS || n <= 0 ||
                (size_t)n > cap)
               continue;
            t->st.rx_relay++;
            slot_mac(b[6], src_mac);
            memcpy(buf, b + RL_HDR, (size_t)n);
            return n;
         }
         on_server(t, b, len, port == t->cfg.server_port ? 0 : 1);
         continue;
      }
      /* a peer, directly */
      if (t->cfg.drop_direct_after_us &&
          now(t) - t->t0 >= t->cfg.drop_direct_after_us)
         continue;
      {
         int j = peer_of_nonce(t, b);
         rl_peer *q;
         uint64_t tn = now(t);
         if (j < 0)
         {
            t->st.rx_bad++;
            continue;
         }
         q = &t->p[j];
         t->st.rx_direct++;
         if (b[5] == RL_T_PUNCH && len >= RL_HDR + 4)
         {
            t->st.punches_rx++;
            if (!t->cfg.direct)
               continue;
            /* Always answer, whatever our state: the peer's punch got
             * through, and the ACK proves the way back. */
            send_punch(t, j, RL_T_PUNCH_ACK, rl_get32(b + RL_HDR), ip, port);
            if (q->path == P_DIRECT)
            {
               q->last_rx_us = tn;
               continue;
            }
            /* peer-reflexive: an address the server never told us (a
             * symmetric NAT's port for us) -- punch it too */
            if (!ep_is(ip, port, q->priv_ip, q->priv_port) &&
                !ep_is(ip, port, q->pub_ip, q->pub_port) &&
                !ep_is(ip, port, q->map_ip, q->map_port) &&
                !ep_is(ip, port, q->rfx_ip, q->rfx_port))
            {
               char a[24];
               q->rfx_ip = ip;
               q->rfx_port = port;
               ipfmt(ip, port, a);
               lg(t, "relay_candidate peer=%d reflexive=%s", j, a);
            }
            /* the peer is punching and we had given up (it was late, or we
             * were): punch back now, the two bursts overlap */
            if (q->path == P_FALLBACK || q->path == P_RELAY)
            {
               q->retry_us = 0;
               lg(t, "relay_retry peer=%d why=peer_punch", j);
               burst(t, q, tn);
            }
            {
               uint32_t c = ++q->counter;
               q->sent_us[c % TS_RING] = tn;
               send_punch(t, j, RL_T_PUNCH, c, ip, port);
            }
            continue;
         }
         if (b[5] == RL_T_PUNCH_ACK && len >= RL_HDR + 4)
         {
            uint32_t c = rl_get32(b + RL_HDR);
            int priv = ep_is(ip, port, q->priv_ip, q->priv_port);
            t->st.acks_rx++;
            if (!t->cfg.direct)
               continue;
            /* an RTT sample only from the endpoint the path uses (the peer
             * may still be punching our other candidate) */
            if (c && c <= q->counter && q->counter - c < TS_RING &&
                (q->path != P_DIRECT || (ip == q->dir_ip && port == q->dir_port)))
               q->rtt_us = (uint32_t)(tn - q->sent_us[c % TS_RING]);
            t->st.direct_rtt_us[j] = q->rtt_us;
            if (q->path != P_DIRECT)
            {
               /* an ACK is the round trip: direct works both ways (also a
                * late one, after we had fallen back) */
               char a[24];
               q->path = P_DIRECT;
               q->dir_ip = ip;
               q->dir_port = port;
               q->retry_us = 0;
               q->backoff_us = RETRY_FIRST_US;
               q->last_rx_us = q->last_tx_us = tn;
               t->st.directs++;
               ipfmt(ip, port, a);
               lg(t, "relay_path peer=%d path=direct via=%s%s rtt_us=%u", j, a,
                  priv ? " (lan)" : "", (unsigned)q->rtt_us);
               send_path(t, j, RL_PATH_DIRECT);
            }
            else
            {
               q->last_rx_us = tn;
               /* the LAN beats a hairpin through the router: switch to the
                * private endpoint once it answers */
               if (priv && !ep_is(q->dir_ip, q->dir_port, ip, port))
               {
                  char a[24];
                  q->dir_ip = ip;
                  q->dir_port = port;
                  ipfmt(ip, port, a);
                  lg(t, "relay_path peer=%d path=direct via=%s (lan)", j, a);
               }
            }
            continue;
         }
         if (b[5] == RL_T_DATA)
         {
            int n = len - RL_HDR;
            if (n <= 0 || (size_t)n > cap)
               continue;
            if (q->path == P_DIRECT)
               q->last_rx_us = tn;
            slot_mac(j, src_mac);
            memcpy(buf, b + RL_HDR, (size_t)n);
            return n;
         }
      }
   }
   return 0;
}

static int send_slot(relay_transport *t, int j, const void *buf, size_t len)
{
   uint8_t b[RL_MAX_DGRAM];
   rl_peer *q;
   if (len + RL_HDR > sizeof(b) || t->state != RL_ST_IN_ROOM)
      return -1;
   if (j < RL_SLOTS && j != t->slot && t->p[j].path == P_DIRECT)
   {
      q = &t->p[j];
      rl_hdr(b, RL_T_DATA, t->slot, j, q->nonce);
      memcpy(b + RL_HDR, buf, len);
      t->st.tx_direct++;
      q->last_tx_us = now(t);
      raw(t, q->dir_ip, q->dir_port, b, len + RL_HDR);
      return 0;
   }
   rl_hdr(b, RL_T_DATA, t->slot, j, t->token);
   memcpy(b + RL_HDR, buf, len);
   to_server(t, 0, b, len + RL_HDR);
   return 0;
}

static int tp_send_to(void *ctx, const uint8_t mac[6], const void *buf, size_t len)
{
   relay_transport *t = (relay_transport *)ctx;
   if (mac[0] != 0x02 || mac[1] != 0x47 || mac[2] != 0x42 || mac[3] != 0x52 ||
       mac[5] >= RL_SLOTS)
      return -1;
   return send_slot(t, mac[5], buf, len);
}

static int tp_broadcast(void *ctx, const void *buf, size_t len)
{
   relay_transport *t = (relay_transport *)ctx;
   int j, any = 0;
   for (j = 0; j < RL_SLOTS; j++)
      if (j != t->slot && t->p[j].present)
      {
         send_slot(t, j, buf, len);
         any = 1;
      }
   if (!any)
      return send_slot(t, RL_SLOT_ALL, buf, len);
   return 0;
}

static void tp_local(void *ctx, uint8_t mac[6])
{
   relay_transport *t = (relay_transport *)ctx;
   slot_mac(t->state == RL_ST_IN_ROOM ? t->slot : 0xFF, mac);
}

relay_transport *relay_transport_create(const rl_sock *s, const rl_client_cfg *cfg)
{
   relay_transport *t = (relay_transport *)calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   t->s = *s;
   t->cfg = *cfg;
   t->t0 = now(t);
   t->slot = RL_SLOT_NONE;
   t->probe_nonce = (uint32_t)(t->t0 * 2654435761u) ^ 0x5A17u;
   if (!t->probe_nonce)
      t->probe_nonce = 1;
   t->state = cfg->direct ? RL_ST_PROBING : RL_ST_ENTERING;
   t->nat = RL_NAT_UNKNOWN;
   t->last_server_rx_us = t->t0;
   lg(t, "relay_start host=%d code=%s direct=%d", cfg->is_host,
      cfg->code[0] ? cfg->code : "-", cfg->direct);
   return t;
}

void relay_transport_destroy(relay_transport *t)
{
   if (!t)
      return;
   if (t->state == RL_ST_IN_ROOM)
   {
      uint8_t b[RL_HDR];
      rl_hdr(b, RL_T_BYE, t->slot, RL_SLOT_NONE, t->token);
      to_server(t, 0, b, sizeof(b));
   }
   free(t);
}

int relay_transport_error(const relay_transport *t, const char **text)
{
   if (text)
      *text = t->errtext;
   return t->err;
}

const char *relay_transport_code(const relay_transport *t) { return t->code; }
int relay_transport_mode(const relay_transport *t) { return t->room_mode; }
int relay_transport_slot(const relay_transport *t) { return t->slot; }
/* How long the server has not answered (us): the room is presumed lost
 * while a host keeps entering it again (lobby_reenter). */
uint64_t relay_transport_server_silent_us(relay_transport *t)
{
   return now(t) - t->last_server_rx_us;
}

void relay_transport_iface(relay_transport *t, nd_transport *out)
{
   memset(out, 0, sizeof(*out));
   out->ctx = t;
   out->send_to = tp_send_to;
   out->broadcast = tp_broadcast;
   out->recv = relay_recv;
   out->local_addr = tp_local;
   out->pending = NULL;        /* no cheap peek on sceNetInet (transport_inet.c) */
}

void relay_transport_stats(const relay_transport *t, rl_client_stats *out)
{
   int j;
   *out = t->st;
   for (j = 0; j < RL_SLOTS; j++)
      out->path[j] = (uint8_t)t->p[j].path;
}
