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
   int by_join;              /* a desktop host's home room, opened by its first JOIN */
   rl_slot s[RL_SLOTS];
   uint64_t created_ms, last_ms;
   int joined_ever;
} rl_room;

#define IPT_N 128
/* per source IP: CREATE/JOIN attempts this minute, and the last code it
 * asked for that did not exist (a joiner waiting for its host retries that
 * same code twice a second; only a NEW code counts as a guess) */
typedef struct { uint32_t ip; uint32_t n, nl; uint64_t win_ms; char miss[RL_CODE_LEN]; } rl_ipent;

/* a directory entry: a desktop host's code -> its public endpoint.  The
 * address is the one the REGISTER came from (never one the host claims), the
 * port the one it advertises (its router's mapping). */
typedef struct
{
   int used;
   char code[RL_CODE_LEN];
   rl_addr src;              /* where its REGISTERs come from */
   uint16_t adv_port;
   rl_addr priv;             /* its LAN endpoint: told only to the same home */
   uint64_t token, last_ms;
} rl_dir;

struct rl_core
{
   rl_core_cfg cfg;
   rl_room *rooms;
   unsigned nrooms;
   rl_ipent ipt[IPT_N];
   uint64_t secret[2];       /* cookie key, random per process */
   rl_core_stats st;
   rl_dir *dir;              /* cfg.directory: max_dir entries */
   char home[RL_CODE_LEN];   /* a desktop host's own code (relay_core_set_home) */
   int has_home;
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
   if (!c->cfg.max_dir) c->cfg.max_dir = 256;
   if (!c->cfg.dir_per_ip) c->cfg.dir_per_ip = 2;
   if (!c->cfg.lookups_per_min) c->cfg.lookups_per_min = 120;
   c->secret[0] = c->cfg.random64(c->cfg.user);
   c->secret[1] = c->cfg.random64(c->cfg.user);
   c->nrooms = c->cfg.max_rooms;
   c->rooms = (rl_room *)calloc(c->nrooms, sizeof(rl_room));
   if (c->cfg.directory)
      c->dir = (rl_dir *)calloc(c->cfg.max_dir, sizeof(rl_dir));
   if (!c->rooms || (c->cfg.directory && !c->dir))
   {
      free(c->rooms);
      free(c->dir);
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
   free(c->dir);
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

/* the source IP's entry for this minute (made, or recycled) */
static rl_ipent *ip_entry(rl_core *c, uint32_t ip, uint64_t now)
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
            if (e->ip != ip)
               memset(e->miss, 0, sizeof(e->miss));
            e->ip = ip;
            e->n = e->nl = 0;
            e->win_ms = now;
         }
         return e;
      }
      if (!old || e->win_ms < old->win_ms)
         old = e;
   }
   /* full of live entries: recycle the stalest (sources are cookie-verified,
    * so filling the table takes real addresses) */
   memset(old, 0, sizeof(*old));
   old->ip = ip;
   old->win_ms = now;
   return old;
}

/* CREATE / JOIN / REGISTER / a LOOKUP guess: joins_per_min per source IP */
static int ip_allow(rl_core *c, uint32_t ip, uint64_t now)
{
   return ++ip_entry(c, ip, now)->n <= c->cfg.joins_per_min;
}

/* every LOOKUP, a guess or not: lookups_per_min per source IP */
static int ip_lookup_allow(rl_core *c, uint32_t ip, uint64_t now)
{
   return ++ip_entry(c, ip, now)->nl <= c->cfg.lookups_per_min;
}

/* A code that names nothing (JOIN NO_ROOM, LOOKUP NONE): waiting for a host
 * on the same code is not guessing, so only a code this IP has not just
 * missed counts against its limit. */
static int guess_allow(rl_core *c, uint32_t ip, const uint8_t *code, uint64_t now)
{
   rl_ipent *e = ip_find(c, ip, now);
   if (!e || memcmp(e->miss, code, RL_CODE_LEN))
   {
      if (!ip_allow(c, ip, now))
         return 0;
      e = ip_find(c, ip, now);
      if (e)
         memcpy(e->miss, code, RL_CODE_LEN);
   }
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

static rl_dir *dir_by_code(rl_core *c, const uint8_t *code)
{
   unsigned i;
   if (!c->dir)
      return NULL;
   for (i = 0; i < c->cfg.max_dir; i++)
      if (c->dir[i].used && !memcmp(c->dir[i].code, code, RL_CODE_LEN))
         return &c->dir[i];
   return NULL;
}

/* One namespace: a room here, a directory entry and the home code never
 * share a code. */
static int code_taken(rl_core *c, const uint8_t *code)
{
   return room_by_code(c, code) || dir_by_code(c, code) ||
          (c->has_home && !memcmp(c->home, code, RL_CODE_LEN));
}

static void random_code(rl_core *c, char *code)
{
   do
   {
      uint64_t x = c->cfg.random64(c->cfg.user);
      int k;
      for (k = 0; k < RL_CODE_LEN; k++, x /= RL_CODE_SYMBOLS)
         code[k] = RL_CODE_ALPHABET[x % RL_CODE_SYMBOLS];
   } while (code_taken(c, (const uint8_t *)code));
}

void relay_core_set_home(rl_core *c, const char *code)
{
   c->has_home = code && code_ok((const uint8_t *)code);
   if (c->has_home)
      memcpy(c->home, code, RL_CODE_LEN);
   else
      memset(c->home, 0, sizeof(c->home));
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
static int cookie_check_at(rl_core *c, int sock, const rl_addr *from,
                           const uint8_t *body, size_t n, size_t at, uint64_t now)
{
   uint64_t e = now / 30000, got;
   uint8_t b[RL_HDR + RL_COOKIE_LEN];
   if (n >= at + RL_COOKIE_LEN)
   {
      got = rl_get64(body + at);
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

static int cookie_check(rl_core *c, int sock, const rl_addr *from,
                        const uint8_t *body, size_t n, uint64_t now)
{
   return cookie_check_at(c, sock, from, body, n, CJ_COOKIE, now);
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
      /* (the home code is this host's own: a PSP may open its room by name) */
      if (room_by_code(c, body + RL_BUILD_LEN) || dir_by_code(c, body + RL_BUILD_LEN))
      {
         send_error(c, sock, from, RL_E_BUSY, "code in use");
         return;
      }
      memcpy(r->code, body + RL_BUILD_LEN, RL_CODE_LEN);
   }
   else if (c->has_home && !room_by_code(c, (const uint8_t *)c->home))
      memcpy(r->code, c->home, RL_CODE_LEN);   /* a PSP that came by an invite */
   else
      random_code(c, r->code);
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

/* A desktop host has no PSP that "creates": both players "Join with code"
 * (the design mockup).  The first JOIN of the home code opens the home room
 * and takes slot 0 -- the room's host, whose link mode the room keeps (the
 * mode byte after the cookie, as in CREATE); the PSP that is told slot 0
 * runs the netdrv host.  Same limits as a CREATE. */
static void open_home(rl_core *c, int sock, const rl_addr *from,
                      const uint8_t *body, size_t n, uint64_t now)
{
   unsigned i;
   rl_room *r = NULL;
   char ab[24];
   if (!ip_allow(c, from->ip_be, now) ||
       rooms_hosted_by(c, from->ip_be) >= c->cfg.rooms_per_ip)
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many rooms");
      return;
   }
   for (i = 0; i < c->nrooms && !r; i++)
      if (!c->rooms[i].used)
         r = &c->rooms[i];
   if (!r)
   {
      send_error(c, sock, from, RL_E_BUSY, "server full");
      return;
   }
   r->used = 1;
   r->by_join = 1;
   memcpy(r->code, c->home, RL_CODE_LEN);
   memcpy(r->build, body, RL_BUILD_LEN);
   r->mode = n > CJ_COOKIE + RL_COOKIE_LEN ? body[CJ_COOKIE + RL_COOKIE_LEN] : 0;
   r->created_ms = r->last_ms = now;
   fill_slot(c, &r->s[0], from, body, n, now);
   c->st.rooms_now++;
   c->st.rooms_made++;
   lg(c, "room_create code=%.4s host=%s nat=%u by=join", r->code, ipstr(from, ab),
      r->s[0].nat);
   reply_slot(c, sock, from, RL_T_JOINED, r, 0);
}

static void on_join(rl_core *c, int sock, const rl_addr *from,
                    const uint8_t *body, size_t n, uint64_t now)
{
   rl_room *r;
   int k, j;
   char ab[24];
   const uint8_t *code;
   if (n < CJ_BODY)
   {
      c->st.rx_bad++;
      return;
   }
   /* no code: a PSP that came by an invite code joins this host's own room */
   code = code_ok(body + RL_BUILD_LEN) || !c->has_home ? body + RL_BUILD_LEN
                                                        : (const uint8_t *)c->home;
   r = room_by_code(c, code);
   if (r)
      for (k = r->by_join ? 0 : 1; k < RL_SLOTS; k++)
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
      if (c->has_home && !memcmp(code, c->home, RL_CODE_LEN))
      {
         open_home(c, sock, from, body, n, now);
         return;
      }
      if (!guess_allow(c, from->ip_be, code, now))
      {
         c->st.rx_rate++;
         send_error(c, sock, from, RL_E_RATE, "too many joins");
         return;
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

/* ---- the directory ---------------------------------------------------------
 * docs/ONLINE-TUNNEL.md, "Desktop hosts and discovery".  The same rules as
 * rooms: a cookie before any state is spent on a source, no reply larger
 * than its request (REGISTER and LOOKUP are padded to RL_DIR_PAD by the
 * client and must be at least as long as any reply), per-IP limits, a fixed
 * table, entries that expire.  The server stores a code, the source address
 * of its REGISTERs, the advertised port and a token -- nothing about who
 * looked it up. */

/* REGISTER body: code[4] adv_port[2] priv_ip[4] priv_port[2] flags[1] cookie[8] */
#define RG_BODY   13
#define RG_COOKIE RG_BODY
#define RG_MIN    48   /* body + cookie is 37 B; an ERROR may be 41: never amplify */

static void dir_tx(rl_core *c, int sock, const rl_addr *to, const uint8_t *b, size_t n)
{
   c->st.dir_tx_bytes += (uint32_t)n;
   tx(c, sock, to, b, n);
}

static void send_registered(rl_core *c, int sock, const rl_addr *to, const rl_dir *d)
{
   uint8_t b[RL_HDR + 8];
   rl_hdr(b, RL_T_REGISTERED, RL_SLOT_NONE, RL_SLOT_NONE, d->token);
   memcpy(b + RL_HDR, d->code, RL_CODE_LEN);
   rl_put16(b + RL_HDR + 4, RL_DIR_TTL_S);
   rl_put16(b + RL_HDR + 6, d->adv_port);
   dir_tx(c, sock, to, b, sizeof(b));
}

static void dir_fill(rl_dir *d, const rl_addr *from, const uint8_t *body, uint64_t now)
{
   uint16_t adv = (uint16_t)rl_get16(body + 4);
   d->src = *from;
   d->adv_port = adv ? adv : from->port;
   d->priv.ip_be = rl_get_ip(body + 6);
   d->priv.port = (uint16_t)rl_get16(body + 10);
   d->last_ms = now;
}

static void on_register(rl_core *c, int sock, const rl_addr *from, const uint8_t *b,
                        size_t len, uint64_t now)
{
   const uint8_t *body = b + RL_HDR;
   size_t n = len - RL_HDR;
   uint64_t tok = rl_get64(b + 8);
   unsigned i, mine = 0;
   rl_dir *d = NULL;
   char ab[24];
   /* a refresh (a token) may be short: its only answers are REGISTERED and
    * COOKIE, 24 B each; anything that can draw an ERROR is padded */
   if (len < (tok ? RL_REFRESH_LEN : RG_MIN))
   {
      c->st.rx_bad++;
      return;
   }
   c->st.dir_rx_bytes += (uint32_t)len;
   if (!c->dir)
   {
      if (len >= RG_MIN)                         /* never larger than the request */
         send_error(c, sock, from, RL_E_NO_DIR, "no directory here");
      return;
   }
   /* a refresh: the entry's token, from the address it was made from (a
    * new port is a NAT rebinding: followed, the token proves it) */
   if (tok)
      for (i = 0; i < c->cfg.max_dir; i++)
         if (c->dir[i].used && c->dir[i].token == tok && c->dir[i].src.ip_be == from->ip_be)
         {
            d = &c->dir[i];
            dir_fill(d, from, body, now);
            c->st.dir_refreshed++;
            send_registered(c, sock, from, d);
            return;
         }
   if (!cookie_check_at(c, sock, from, body, n, RG_COOKIE, now))
   {
      c->st.dir_tx_bytes += RL_HDR + RL_COOKIE_LEN;
      return;
   }
   if (c->cfg.paused)
   {
      send_error(c, sock, from, RL_E_PAUSED, "online play is paused");
      return;
   }
   if (code_ok(body))
   {
      d = dir_by_code(c, body);
      /* the same host again (it lost its token, or the server restarted):
       * its own code back */
      if (d && !addr_eq(&d->src, from))
         d = NULL, mine = 1;
      if (mine || room_by_code(c, body))
      {
         send_error(c, sock, from, RL_E_BUSY, "code in use");
         return;
      }
   }
   if (!ip_allow(c, from->ip_be, now))
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many codes");
      return;
   }
   if (!d)
   {
      for (i = 0, mine = 0; i < c->cfg.max_dir; i++)
         if (c->dir[i].used && c->dir[i].src.ip_be == from->ip_be)
            mine++;
      if (mine >= c->cfg.dir_per_ip)
      {
         c->st.rx_rate++;
         send_error(c, sock, from, RL_E_RATE, "too many codes");
         return;
      }
      for (i = 0; i < c->cfg.max_dir; i++)
         if (!c->dir[i].used)
         {
            d = &c->dir[i];
            break;
         }
      if (!d)
      {
         send_error(c, sock, from, RL_E_FULL, "directory full");
         return;
      }
      memset(d, 0, sizeof(*d));
      if (code_ok(body))
         memcpy(d->code, body, RL_CODE_LEN);
      else
         random_code(c, d->code);
      d->used = 1;
      c->st.dir_now++;
      c->st.dir_made++;
   }
   do
      d->token = c->cfg.random64(c->cfg.user);
   while (!d->token);
   dir_fill(d, from, body, now);
   lg(c, "dir_register code=%.4s host=%s adv_port=%u", d->code, ipstr(from, ab),
      d->adv_port);
   send_registered(c, sock, from, d);
}

static void on_unregister(rl_core *c, const rl_addr *from, const uint8_t *b, size_t len)
{
   uint64_t tok = rl_get64(b + 8);
   unsigned i;
   if (!c->dir || !tok || len < RL_HDR + RL_CODE_LEN)
   {
      c->st.rx_bad++;
      return;
   }
   c->st.dir_rx_bytes += (uint32_t)len;
   for (i = 0; i < c->cfg.max_dir; i++)
      if (c->dir[i].used && c->dir[i].token == tok && c->dir[i].src.ip_be == from->ip_be &&
          !memcmp(c->dir[i].code, b + RL_HDR, RL_CODE_LEN))
      {
         lg(c, "dir_close code=%.4s why=unregister", c->dir[i].code);
         memset(&c->dir[i], 0, sizeof(c->dir[i]));
         if (c->st.dir_now)
            c->st.dir_now--;
         return;
      }
}

static void on_lookup(rl_core *c, int sock, const rl_addr *from, const uint8_t *b,
                      size_t len, uint64_t now)
{
   const uint8_t *body = b + RL_HDR;
   uint8_t o[RL_LOCATED_LEN];
   uint8_t *p = o + RL_HDR;
   const rl_dir *d;
   int kind;
   if (len < RL_LOCATED_LEN || !code_ok(body))     /* padded: never amplify */
   {
      c->st.rx_bad++;
      return;
   }
   c->st.dir_rx_bytes += (uint32_t)len;
   c->st.lookups++;
   if (!ip_lookup_allow(c, from->ip_be, now))
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many lookups");
      return;
   }
   d = dir_by_code(c, body);
   kind = d ? RL_LOC_HOST
        : room_by_code(c, body) || (c->has_home && !memcmp(c->home, body, RL_CODE_LEN))
              ? RL_LOC_HERE : RL_LOC_NONE;
   if (kind == RL_LOC_NONE && !guess_allow(c, from->ip_be, body, now))
   {
      c->st.rx_rate++;
      send_error(c, sock, from, RL_E_RATE, "too many lookups");
      return;
   }
   memset(o, 0, sizeof(o));
   rl_hdr(o, RL_T_LOCATED, RL_SLOT_NONE, RL_SLOT_NONE, 0);
   memcpy(p, body, 8);                               /* code, nonce */
   p[8] = (uint8_t)kind;
   if (d)
   {
      c->st.located++;
      rl_put_ip(p + 9, d->src.ip_be);
      rl_put16(p + 13, d->adv_port);
      if (d->src.ip_be == from->ip_be)               /* the same home: its LAN address */
      {
         rl_put_ip(p + 15, d->priv.ip_be);
         rl_put16(p + 19, d->priv.port);
      }
   }
   rl_put_ip(p + 21, from->ip_be);
   dir_tx(c, sock, from, o, sizeof(o));
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
   case RL_T_REGISTER:
      on_register(c, sock, from, b, len, now_ms);
      return;
   case RL_T_UNREGISTER:
      on_unregister(c, from, b, len);
      return;
   case RL_T_LOOKUP:
      on_lookup(c, sock, from, b, len, now_ms);
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
   if (c->dir)
      for (i = 0; i < c->cfg.max_dir; i++)
         if (c->dir[i].used && now - c->dir[i].last_ms > RL_DIR_TTL_S * 1000ull)
         {
            lg(c, "dir_close code=%.4s why=expired", c->dir[i].code);
            memset(&c->dir[i], 0, sizeof(c->dir[i]));
            if (c->st.dir_now)
               c->st.dir_now--;
         }
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
