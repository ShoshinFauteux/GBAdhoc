/* relay_core.c -- GBR1 relay / rendezvous server core.  See relay_core.h and
 * docs/ONLINE-TUNNEL.md sections 5-6 for the rules this enforces:
 *
 *   - A reply to a source that holds no token is never larger than its
 *     request (no amplification): PROBE and CREATE/JOIN are padded by the
 *     client, CREATED/JOINED/PROBED/ERROR are short.
 *   - DATA is forwarded only between slots of one room, from the address
 *     pinned to the sender's token; the token is zeroed before forwarding so
 *     a peer never learns another slot's token.
 *   - Rate limits per slot (packets/s) and per source IP (rooms/joins per
 *     minute).  Rooms expire.
 */
#include "relay_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   int used;
   rl_addr addr, priv;
   uint16_t map_port;        /* a router forward to this socket, 0 = none */
   uint64_t token;
   uint64_t last_ms, repin_ms;
   uint8_t nat;
   uint32_t bucket;          /* packets this second */
   uint32_t bucket_bytes;    /* bytes this second */
   uint64_t bucket_s;
} rl_slot;

typedef struct
{
   int used;
   char code[RL_CODE_LEN];
   uint8_t build[RL_BUILD_LEN];
   uint8_t mode;             /* the host's link mode (RL_MODE_*), told to joiners */
   rl_slot s[RL_SLOTS];
   uint64_t created_ms, last_ms;
   int joined_ever;
} rl_room;

#define IPT_N 128
/* per source IP: CREATE/JOIN attempts this minute, and the last code it
 * asked for that did not exist (a joiner waiting for its host retries that
 * same code twice a second; only a NEW code counts as a guess) */
typedef struct { uint32_t ip; uint32_t n; uint64_t win_ms; char miss[RL_CODE_LEN]; } rl_ipent;

struct rl_core
{
   rl_core_cfg cfg;
   rl_room *rooms;
   unsigned nrooms;
   rl_ipent ipt[IPT_N];
   uint64_t secret[2];       /* cookie key, random per process */
   rl_core_stats st;
};

static void lg(rl_core *c, const char *fmt, ...)
#if defined(__GNUC__)
   __attribute__((format(printf, 2, 3)))
#endif
   ;
#include <stdarg.h>
static void lg(rl_core *c, const char *fmt, ...)
{
   char b[200];
   va_list a;
   if (!c->cfg.log)
      return;
   va_start(a, fmt);
   vsnprintf(b, sizeof(b), fmt, a);
   va_end(a);
   c->cfg.log(c->cfg.user, b);
}

static const char *ipstr(const rl_addr *a, char *b)
{
   const uint8_t *p = (const uint8_t *)&a->ip_be;
   sprintf(b, "%u.%u.%u.%u:%u", p[0], p[1], p[2], p[3], a->port);
   return b;
}

static int addr_eq(const rl_addr *a, const rl_addr *b)
{
   return a->ip_be == b->ip_be && a->port == b->port;
}

rl_core *relay_core_create(const rl_core_cfg *cfg)
{
   rl_core *c = (rl_core *)calloc(1, sizeof(*c));
   if (!c)
      return NULL;
   c->cfg = *cfg;
   if (!c->cfg.max_rooms) c->cfg.max_rooms = 256;
   if (!c->cfg.room_idle_ms) c->cfg.room_idle_ms = 60000;
   if (!c->cfg.lobby_ms) c->cfg.lobby_ms = 600000;
   if (!c->cfg.slot_idle_ms) c->cfg.slot_idle_ms = 20000;
   /* Per-slot caps, against a measured link session (PPSSPP pair through
    * this relay, 2026-11-05): the 1.1 MB "Sharing where you are" offers
    * ~390 datagrams/s and ~70 KB/s per slot, play ~65/s and ~3 KB/s.  The
    * old 300/s and 64 KB/s dropped 3,000-7,000 datagrams per share.  ~2.5x
    * headroom; the anti-abuse limits are the per-IP room and join counts. */
   if (!c->cfg.pkt_per_s) c->cfg.pkt_per_s = 1000;
   if (!c->cfg.joins_per_min) c->cfg.joins_per_min = 30;
   if (!c->cfg.punch_start_ms) c->cfg.punch_start_ms = 150;
   if (!c->cfg.rooms_per_ip) c->cfg.rooms_per_ip = 3;
   if (!c->cfg.bytes_per_s) c->cfg.bytes_per_s = 262144;
   c->secret[0] = c->cfg.random64(c->cfg.user);
   c->secret[1] = c->cfg.random64(c->cfg.user);
   c->nrooms = c->cfg.max_rooms;
   c->rooms = (rl_room *)calloc(c->nrooms, sizeof(rl_room));
   if (!c->rooms)
   {
      free(c);
      return NULL;
   }
   return c;
}

void relay_core_destroy(rl_core *c)
{
   if (!c)
      return;
   free(c->rooms);
   free(c);
}

void relay_core_stats(const rl_core *c, rl_core_stats *out)
{
   *out = c->st;
}

static void tx(rl_core *c, int sock, const rl_addr *to, const uint8_t *b, size_t n)
{
   c->st.tx++;
   c->cfg.send(c->cfg.user, sock, to, b, n);
}

static void send_error(rl_core *c, int sock, const rl_addr *to, int code, const char *text)
{
   uint8_t b[RL_HDR + 1 + 24];
   size_t tl = strlen(text);
   if (tl > 24)
      tl = 24;
   rl_hdr(b, RL_T_ERROR, RL_SLOT_NONE, RL_SLOT_NONE, 0);
   b[RL_HDR] = (uint8_t)code;
   memcpy(b + RL_HDR + 1, text, tl);
   tx(c, sock, to, b, RL_HDR + 1 + tl);
}

static void send_peer(rl_core *c, rl_room *r, int to, int slot, int state)
{
   uint8_t b[RL_HDR + 2];
   rl_hdr(b, RL_T_PEER, RL_SLOT_NONE, to, 0);
   b[RL_HDR] = (uint8_t)slot;
   b[RL_HDR + 1] = (uint8_t)state;
   tx(c, 0, &r->s[to].addr, b, sizeof(b));
}

static void send_candidates(rl_core *c, rl_room *r, int to, int about,
                            uint64_t nonce, int punch)
{
   uint8_t b[RL_HDR + 30];
   uint8_t *p = b + RL_HDR;
   const rl_slot *s = &r->s[about];
   uint16_t mp = s->map_port ? s->map_port : s->priv.port;
   rl_hdr(b, RL_T_CANDIDATES, RL_SLOT_NONE, to, 0);
   p[0] = (uint8_t)about;
   rl_put_ip(p + 1, r->s[about].addr.ip_be);
   rl_put16(p + 5, r->s[about].addr.port);
   rl_put_ip(p + 7, r->s[about].priv.ip_be);
   rl_put16(p + 11, r->s[about].priv.port);
   rl_put64(p + 13, nonce);
   rl_put16(p + 21, c->cfg.punch_start_ms);
   p[23] = (uint8_t)punch;
   /* the forward candidate: public IP, the forwarded port -- only when it
    * is a different endpoint from the one we saw */
   if (mp && mp != s->addr.port)
   {
      rl_put_ip(p + 24, s->addr.ip_be);
      rl_put16(p + 28, mp);
   }
   else
      memset(p + 24, 0, 6);
   tx(c, 0, &r->s[to].addr, b, sizeof(b));
}

static rl_ipent *ip_find(rl_core *c, uint32_t ip, uint64_t now)
{
   unsigned h = (ip ^ (ip >> 13)) % IPT_N, i;
   for (i = 0; i < IPT_N; i++)
   {
      rl_ipent *e = &c->ipt[(h + i) % IPT_N];
      if (e->ip == ip && now - e->win_ms <= 60000)
         return e;
      if (!e->ip)
         return NULL;
   }
   return NULL;
}

static int ip_allow(rl_core *c, uint32_t ip, uint64_t now)
{
   unsigned h = (ip ^ (ip >> 13)) % IPT_N, i;
   rl_ipent *old = NULL;
   for (i = 0; i < IPT_N; i++)
   {
      rl_ipent *e = &c->ipt[(h + i) % IPT_N];
      if (e->ip == ip || !e->ip || now - e->win_ms > 60000)
      {
         if (e->ip != ip || now - e->win_ms > 60000)
         {
            e->ip = ip;
            e->n = 0;
            e->win_ms = now;
         }
         return ++e->n <= c->cfg.joins_per_min;
      }
      if (!old || e->win_ms < old->win_ms)
         old = e;
   }
   /* full of live entries: recycle the stalest (sources are cookie-verified,
    * so filling the table takes real addresses) */
   old->ip = ip;
   old->n = 1;
   old->win_ms = now;
   return 1;
}

static int code_ok(const uint8_t *p)
{
   int i;
   for (i = 0; i < RL_CODE_LEN; i++)
      if (!p[i] || !strchr(RL_CODE_ALPHABET, p[i]))
         return 0;
   return 1;
}

static rl_room *room_by_code(rl_core *c, const uint8_t *code)
{
   unsigned i;
   for (i = 0; i < c->nrooms; i++)
      if (c->rooms[i].used && !memcmp(c->rooms[i].code, code, RL_CODE_LEN))
         return &c->rooms[i];
   return NULL;
}

static void room_free(rl_core *c, rl_room *r, const char *why)
{
   lg(c, "room_close code=%.4s why=%s", r->code, why);
   memset(r, 0, sizeof(*r));
   if (c->st.rooms_now)
      c->st.rooms_now--;
}

static void slot_leave(rl_core *c, rl_room *r, int k, const char *why)
{
   int j;
   r->s[k].used = 0;
   lg(c, "slot_leave code=%.4s slot=%d why=%s", r->code, k, why);
   if (k == 0)
   {
      for (j = 1; j < RL_SLOTS; j++)
         if (r->s[j].used)
            send_peer(c, r, j, 0, RL_PEER_LEFT);
      room_free(c, r, "host_left");
      return;
   }
   for (j = 0; j < RL_SLOTS; j++)
      if (r->s[j].used)
         send_peer(c, r, j, k, RL_PEER_LEFT);
}

/* CREATE / JOIN body: build[16] code[6] priv_ip[4] priv_port[2] nat[1] */
#define CJ_BODY (RL_BUILD_LEN + RL_CODE_LEN + 4 + 2 + 1)
#define CJ_COOKIE CJ_BODY            /* cookie[8] follows the nat byte */

static uint64_t mix64(uint64_t x)
{
   x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
   x ^= x >> 27; x *= 0x94D049BB133111EBull;
   return x ^ (x >> 31);
}

/* A stateless proof that the sender receives at its address: keyed hash of
 * ip:port and the 30 s epoch; the current and the previous epoch verify. */
static uint64_t cookie_of(const rl_core *c, const rl_addr *a, uint64_t epoch)
{
   uint64_t v = mix64(c->secret[0] ^ ((uint64_t)a->ip_be << 16) ^ a->port);
   return mix64(v ^ c->secret[1] ^ (epoch * 0x9E3779B97F4A7C15ull)) | 1u;
}

/* 1 = the body carries a valid cookie; 0 = it does not, and a COOKIE has
 * been sent (never larger than the request). */
static int cookie_check(rl_core *c, int sock, const rl_addr *from,
                        const uint8_t *body, size_t n, uint64_t now)
{
   uint64_t e = now / 30000, got;
   uint8_t b[RL_HDR + RL_COOKIE_LEN];
   if (n >= CJ_COOKIE + RL_COOKIE_LEN)
   {
      got = rl_get64(body + CJ_COOKIE);
      if (got && (got == cookie_of(c, from, e) || got == cookie_of(c, from, e - 1)))
         return 1;
      if (got)
         c->st.cookie_bad++;
   }
   rl_hdr(b, RL_T_COOKIE, RL_SLOT_NONE, RL_SLOT_NONE, 0);
   rl_put64(b + RL_HDR, cookie_of(c, from, e));
   c->st.cookies_sent++;
   tx(c, sock, from, b, sizeof(b));
   return 0;
}

static unsigned rooms_hosted_by(const rl_core *c, uint32_t ip)
{
   unsigned i, n = 0;
   for (i = 0; i < c->nrooms; i++)
      if (c->rooms[i].used && c->rooms[i].s[0].used && c->rooms[i].s[0].addr.ip_be == ip)
         n++;
   return n;
}

static void reply_slot(rl_core *c, int sock, const rl_addr *to, int type,
                       rl_room *r, int k)
{
   uint8_t b[RL_HDR + RL_CODE_LEN + 2];
   rl_hdr(b, type, RL_SLOT_NONE, k, r->s[k].token);
   memcpy(b + RL_HDR, r->code, RL_CODE_LEN);
   b[RL_HDR + RL_CODE_LEN] = (uint8_t)k;
   b[RL_HDR + RL_CODE_LEN + 1] = r->mode;
   tx(c, sock, to, b, sizeof(b));
}

static void fill_slot(rl_core *c, rl_slot *s, const rl_addr *from,
                      const uint8_t *body, size_t n, uint64_t now)
{
   memset(s, 0, sizeof(*s));
   s->used = 1;
   s->addr = *from;
   s->priv.ip_be = rl_get_ip(body + RL_BUILD_LEN + RL_CODE_LEN);
   s->priv.port = (uint16_t)rl_get16(body + RL_BUILD_LEN + RL_CODE_LEN + 4);
   s->nat = body[RL_BUILD_LEN + RL_CODE_LEN + 6];
   s->map_port = n >= CJ_COOKIE + RL_COOKIE_LEN + 3
                 ? (uint16_t)rl_get16(body + CJ_COOKIE + RL_COOKIE_LEN + 1) : 0;
   do
      s->token = c->cfg.random64(c->cfg.user);
   while (!s->token);
   s->last_ms = now;
}

static void on_create(rl_core *c, int sock, const rl_addr *from,
                      const uint8_t *body, size_t n, uint64_t now)
{
   unsigned i;
   rl_room *r = NULL;
   char ab[24];
   if (n < CJ_BODY)
   {
      c->st.rx_bad++;
      return;
   }
   /* a retry of a CREATE we already answered: answer again */
   for (i = 0; i < c->nrooms; i++)
      if (c->rooms[i].used && c->rooms[i].s[0].used &&
          addr_eq(&c->rooms[i].s[0].addr, from) &&
          (!code_ok(body + RL_BUILD_LEN) ||
           !memcmp(c->rooms[i].code, body + RL_BUILD_LEN, RL_CODE_LEN)))
      {
         reply_slot(c, sock, from, RL_T_CREATED, &c->rooms[i], 0);
         return;
      }
   if (!cookie_check(c, sock, from, body, n, now))
      return;
   if (c->cfg.paused)
   {
      send_error(c, sock, from, RL_E_PAUSED, "online play is paused");
      return;
   }
   if (!ip_allow(c, from->ip_be, now) ||
       rooms_hosted_by(c, from->ip_be) >= c->cfg.rooms_per_ip)
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many rooms");
      return;
   }
   for (i = 0; i < c->nrooms; i++)
      if (!c->rooms[i].used)
      {
         r = &c->rooms[i];
         break;
      }
   if (!r)
   {
      send_error(c, sock, from, RL_E_BUSY, "server full");
      return;
   }
   if (code_ok(body + RL_BUILD_LEN))
   {
      if (room_by_code(c, body + RL_BUILD_LEN))
      {
         send_error(c, sock, from, RL_E_BUSY, "code in use");
         return;
      }
      memcpy(r->code, body + RL_BUILD_LEN, RL_CODE_LEN);
   }
   else
   {
      do
      {
         uint64_t x = c->cfg.random64(c->cfg.user);
         int k;
         for (k = 0; k < RL_CODE_LEN; k++, x /= RL_CODE_SYMBOLS)
            r->code[k] = RL_CODE_ALPHABET[x % RL_CODE_SYMBOLS];
      } while (room_by_code(c, (const uint8_t *)r->code) != NULL);
   }
   r->used = 1;
   memcpy(r->build, body, RL_BUILD_LEN);
   r->mode = n > CJ_COOKIE + RL_COOKIE_LEN ? body[CJ_COOKIE + RL_COOKIE_LEN] : 0;
   r->created_ms = r->last_ms = now;
   fill_slot(c, &r->s[0], from, body, n, now);
   c->st.rooms_now++;
   c->st.rooms_made++;
   lg(c, "room_create code=%.4s host=%s nat=%u", r->code, ipstr(from, ab),
      r->s[0].nat);
   reply_slot(c, sock, from, RL_T_CREATED, r, 0);
}

static void on_join(rl_core *c, int sock, const rl_addr *from,
                    const uint8_t *body, size_t n, uint64_t now)
{
   rl_room *r;
   int k, j;
   char ab[24];
   if (n < CJ_BODY)
   {
      c->st.rx_bad++;
      return;
   }
   r = room_by_code(c, body + RL_BUILD_LEN);
   if (r)
      for (k = 1; k < RL_SLOTS; k++)
         if (r->s[k].used && addr_eq(&r->s[k].addr, from))
         {
            reply_slot(c, sock, from, RL_T_JOINED, r, k);   /* a retry */
            return;
         }
   if (!cookie_check(c, sock, from, body, n, now))
      return;
   if (c->cfg.paused)
   {
      send_error(c, sock, from, RL_E_PAUSED, "online play is paused");
      return;
   }
   if (!r)
   {
      /* waiting for a host on the same code is not guessing: only a code
       * this IP has not just missed counts against its limit */
      rl_ipent *e = ip_find(c, from->ip_be, now);
      if (!e || memcmp(e->miss, body + RL_BUILD_LEN, RL_CODE_LEN))
      {
         if (!ip_allow(c, from->ip_be, now))
         {
            c->st.rx_rate++;
            send_error(c, sock, from, RL_E_RATE, "too many joins");
            return;
         }
         e = ip_find(c, from->ip_be, now);
         if (e)
            memcpy(e->miss, body + RL_BUILD_LEN, RL_CODE_LEN);
      }
      send_error(c, sock, from, RL_E_NO_ROOM, "no such room");
      return;
   }
   if (!ip_allow(c, from->ip_be, now))
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many joins");
      return;
   }
   if (memcmp(r->build, body, RL_BUILD_LEN))
   {
      c->st.refused++;
      send_error(c, sock, from, RL_E_VERSION, "different GBAdhoc build");
      return;
   }
   for (k = 1; k < RL_SLOTS; k++)
      if (!r->s[k].used)
         break;
   if (k == RL_SLOTS)
   {
      send_error(c, sock, from, RL_E_FULL, "room full");
      return;
   }
   fill_slot(c, &r->s[k], from, body, n, now);
   r->joined_ever = 1;
   r->last_ms = now;
   c->st.joins++;
   lg(c, "room_join code=%.4s slot=%d from=%s nat=%u", r->code, k,
      ipstr(from, ab), r->s[k].nat);
   reply_slot(c, sock, from, RL_T_JOINED, r, k);
   for (j = 0; j < RL_SLOTS; j++)
   {
      int punch;
      uint64_t nonce;
      if (j == k || !r->s[j].used)
         continue;
      send_peer(c, r, k, j, RL_PEER_IN);
      send_peer(c, r, j, k, RL_PEER_IN);
      /* Direct is worth a try unless BOTH sides are symmetric with no
       * forward: one reachable side is enough (the other punches it, and
       * it answers the address the punch came from -- peer-reflexive), so
       * a carrier-NAT phone reaches a home host directly. */
      punch = !c->cfg.no_direct &&
              (r->s[j].nat != RL_NAT_SYMMETRIC || r->s[k].nat != RL_NAT_SYMMETRIC ||
               r->s[j].map_port || r->s[k].map_port);
      do
         nonce = c->cfg.random64(c->cfg.user);
      while (!nonce);
      if (punch)
         c->st.punches_offered++;
      send_candidates(c, r, k, j, nonce, punch);
      send_candidates(c, r, j, k, nonce, punch);
   }
}

/* The slot a token-bearing datagram claims, or NULL. */
static rl_slot *auth(rl_core *c, const uint8_t *b, const rl_addr *from,
                     rl_room **room, int *slot, uint64_t now)
{
   unsigned i;
   int k = b[6];
   uint64_t tok = rl_get64(b + 8);
   if (k >= RL_SLOTS || !tok)
      return NULL;
   for (i = 0; i < c->nrooms; i++)
   {
      rl_room *r = &c->rooms[i];
      rl_slot *s = &r->s[k];
      if (!r->used || !s->used || s->token != tok)
         continue;
      if (!addr_eq(&s->addr, from))
      {
         /* NAT rebinding: the same token from a new port.  Re-pin, at most
          * once a second (a carrier NAT may move a mapping more than once;
          * only a holder of the 64-bit token can re-pin at all). */
         if (now - s->repin_ms < 1000)
            return NULL;
         s->repin_ms = now;
         s->addr = *from;
         lg(c, "slot_repin code=%.4s slot=%d", r->code, k);
      }
      *room = r;
      *slot = k;
      return s;
   }
   return NULL;
}

static int slot_rate(rl_core *c, rl_slot *s, size_t len, uint64_t now)
{
   uint64_t sec = now / 1000;
   if (s->bucket_s != sec)
   {
      s->bucket_s = sec;
      s->bucket = 0;
      s->bucket_bytes = 0;
   }
   s->bucket_bytes += (uint32_t)len;
   if (s->bucket_bytes > c->cfg.bytes_per_s)
   {
      c->st.rx_bytes_rate++;
      return 0;
   }
   return ++s->bucket <= c->cfg.pkt_per_s;
}

void relay_core_input(rl_core *c, int sock, const rl_addr *from,
                      const void *buf, size_t len, uint64_t now_ms)
{
   const uint8_t *b = (const uint8_t *)buf;
   const uint8_t *body = b + RL_HDR;
   size_t n;
   rl_room *r = NULL;
   rl_slot *s;
   int k = 0;
   c->st.rx++;
   if (len > RL_MAX_DGRAM || rl_check(b, (int)len) != 0)
   {
      c->st.rx_bad++;
      return;
   }
   n = len - RL_HDR;
   switch (b[5])
   {
   case RL_T_PROBE:
   {
      uint8_t o[RL_HDR + 11];
      if (n < 4 || len < sizeof(o))      /* padded: never amplify */
      {
         c->st.rx_bad++;
         return;
      }
      rl_hdr(o, RL_T_PROBED, RL_SLOT_NONE, RL_SLOT_NONE, 0);
      memcpy(o + RL_HDR, body, 4);
      rl_put_ip(o + RL_HDR + 4, from->ip_be);
      rl_put16(o + RL_HDR + 8, from->port);
      o[RL_HDR + 10] = (uint8_t)sock;
      tx(c, sock, from, o, sizeof(o));
      return;
   }
   case RL_T_CREATE:
      on_create(c, sock, from, body, n, now_ms);
      return;
   case RL_T_JOIN:
      on_join(c, sock, from, body, n, now_ms);
      return;
   default:
      break;
   }
   s = auth(c, b, from, &r, &k, now_ms);
   if (!s)
   {
      c->st.rx_token++;
      /* a KEEP for a slot we no longer have (its room expired or went
       * silent): say so, so a waiting host enters again at once.  17 B
       * back for a 20 B KEEP: never an amplifier. */
      if (b[5] == RL_T_KEEP && len >= RL_HDR + 4)
      {
         uint8_t o[RL_HDR + 1];
         rl_hdr(o, RL_T_ERROR, RL_SLOT_NONE, RL_SLOT_NONE, 0);
         o[RL_HDR] = RL_E_TOKEN;
         tx(c, sock, from, o, sizeof(o));
      }
      return;
   }
   if (!slot_rate(c, s, len, now_ms))
   {
      c->st.rx_rate++;
      return;
   }
   s->last_ms = now_ms;
   r->last_ms = now_ms;
   switch (b[5])
   {
   case RL_T_DATA:
   {
      uint8_t o[RL_MAX_DGRAM];
      int to = b[7], j;
      memcpy(o, b, len);
      rl_put64(o + 8, 0);               /* never leak the sender's token */
      for (j = 0; j < RL_SLOTS; j++)
      {
         if (j == k || !r->s[j].used || (to != RL_SLOT_ALL && to != j))
            continue;
         c->st.fwd++;
         c->st.fwd_bytes += (uint32_t)len;
         tx(c, 0, &r->s[j].addr, o, len);
      }
      return;
   }
   case RL_T_KEEP:
   {
      uint8_t o[RL_HDR + 4];
      rl_hdr(o, RL_T_KEEP, RL_SLOT_NONE, k, 0);
      memcpy(o + RL_HDR, body, n >= 4 ? 4 : 0);
      tx(c, sock, from, o, RL_HDR + (n >= 4 ? 4 : 0));
      return;
   }
   case RL_T_BYE:
      slot_leave(c, r, k, "bye");
      return;
   case RL_T_PATH:
      if (n >= 2 && body[0] < RL_SLOTS && r->s[body[0]].used && body[0] != k)
      {
         if (body[1] == RL_PATH_DIRECT)
            c->st.paths_direct++;
         lg(c, "path code=%.4s slot=%d peer=%d path=%s", r->code, k, body[0],
            body[1] == RL_PATH_DIRECT ? "direct" : "relay");
         send_peer(c, r, body[0], k,
                   body[1] == RL_PATH_DIRECT ? RL_PEER_DIRECT : RL_PEER_RELAY);
      }
      return;
   default:
      c->st.rx_bad++;
      return;
   }
}

void relay_core_tick(rl_core *c, uint64_t now)
{
   unsigned i;
   int k;
   for (i = 0; i < c->nrooms; i++)
   {
      rl_room *r = &c->rooms[i];
      if (!r->used)
         continue;
      if (!r->joined_ever && now - r->created_ms > c->cfg.lobby_ms)
      {
         room_free(c, r, "lobby_expired");
         continue;
      }
      if (now - r->last_ms > c->cfg.room_idle_ms)
      {
         room_free(c, r, "idle");
         continue;
      }
      for (k = RL_SLOTS - 1; k >= 0 && r->used; k--)
         if (r->s[k].used && now - r->s[k].last_ms > c->cfg.slot_idle_ms)
            slot_leave(c, r, k, "silent");
   }
}
