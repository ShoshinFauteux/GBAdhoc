/* relay_proto.h -- the GBR1 wire format: online play through a relay /
 * rendezvous server, with a direct (hole-punched) path between two consoles
 * when their NATs allow it (docs/ONLINE-TUNNEL.md sections 5-6).
 *
 * EXPERIMENTAL, off by default.  netdrv frames are carried UNCHANGED inside
 * DATA; nothing here parses them.
 *
 * Every datagram, server <-> console and console <-> console:
 *
 *    0  4  magic 'GBR1'
 *    4  1  version (RL_VER)
 *    5  1  type (RL_T_*)
 *    6  1  from slot (0 = the room's host, 1..RL_SLOTS-1; RL_SLOT_NONE before
 *          the server assigned one)
 *    7  1  to slot (RL_SLOT_ALL = every other slot)
 *    8  8  token: the sender's server-issued token on the relay path, the
 *          pair's punch nonce on the direct path, 0 before either exists
 *   16  .. body
 *
 * Multi-byte fields are big-endian (network order); addresses are IPv4 in
 * network order as 4 bytes, ports as 2.
 */
#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

#include <stdint.h>
#include <string.h>

#define RL_MAGIC0 'G'
#define RL_MAGIC1 'B'
#define RL_MAGIC2 'R'
#define RL_MAGIC3 '1'
#define RL_VER    1
#define RL_HDR    16
#define RL_SLOTS  5            /* netdrv's ND_MAX_CLIENTS */
#define RL_SLOT_NONE 0xFF
#define RL_SLOT_ALL  0xFE
#define RL_CODE_LEN  4            /* owner 2026-11-05: four letters (456,976 rooms) */
#define RL_BUILD_LEN 16
#define RL_MAX_DGRAM 600       /* RL_HDR + the largest netdrv frame, rounded */
#define RL_DEFAULT_PORT 27420  /* the server also listens on port + 1 (PROBE) */
#define RL_COOKIE_LEN 8

enum
{
   RL_T_CREATE = 1,      /* c->s  body: build[16] code[4] (0s = any) priv_ip[4] priv_port[2] nat[1]
                                     then cookie[8] mode[1] map_port[2]
                                     (JOIN the same; map_port: the external
                                     port of a UPnP / router forward to this
                                     socket, 0 = none or the same port) */
   RL_T_CREATED = 2,     /* s->c  body: code[4] slot[1]; header token = the new slot's token */
   RL_T_JOIN = 3,        /* c->s  body: build[16] code[4] priv_ip[4] priv_port[2] nat[1] */
   RL_T_JOINED = 4,      /* s->c  body: code[4] slot[1]; header token = this slot's token */
   RL_T_PEER = 5,        /* s->c  body: slot[1] state[1] (RL_PEER_*) */
   RL_T_DATA = 6,        /* any   body: one netdrv frame */
   RL_T_KEEP = 7,        /* c->s, echoed s->c: body counter[4] */
   RL_T_BYE = 8,         /* c->s  leave the room */
   RL_T_ERROR = 9,       /* s->c  body: code[1] text[...] */
   RL_T_PROBE = 10,      /* c->s  (to port and port + 1) body: nonce[4] */
   RL_T_PROBED = 11,     /* s->c  body: nonce[4] pub_ip[4] pub_port[2] sock[1] */
   RL_T_CANDIDATES = 12, /* s->c  body: slot[1] pub_ip[4] pub_port[2] priv_ip[4]
                                         priv_port[2] nonce[8] start_ms[2] punch[1]
                                         [map_ip[4] map_port[2]]: a third
                                         candidate, the peer's port forward
                                         (UPnP) on its public IP -- the port
                                         it asked for in CREATE/JOIN, else its
                                         own socket port (the usual "forward
                                         UDP 27500" setup); 0s = none */
   RL_T_PUNCH = 13,      /* c<->c body: counter[4] */
   RL_T_PUNCH_ACK = 14,  /* c<->c body: counter[4] (echoed) */
   RL_T_PATH = 15,       /* c->s  body: slot[1] path[1] (RL_PATH_*); the server
                                     tells that slot with PEER */
   RL_T_COOKIE = 16      /* s->c  body: cookie[8].  The answer to a CREATE/JOIN
                                     without a valid cookie: resend it with this
                                     one (after the nat byte).  Proves the source
                                     address is real before any room state is
                                     spent on it (spoofed-source floods). */
};

enum { RL_PEER_LEFT = 0, RL_PEER_IN = 1, RL_PEER_RELAY = 2, RL_PEER_DIRECT = 3 };
enum { RL_PATH_RELAY = 0, RL_PATH_DIRECT = 1 };
/* The room's link mode (CREATE body after the cookie; CREATED/JOINED after
 * the slot): the host picks it, a joiner follows. */
enum { RL_MODE_ANY = 0, RL_MODE_CABLE = 1, RL_MODE_ADAPTER = 2 };
/* nat byte in CREATE/JOIN, from the client's two PROBEs */
enum { RL_NAT_UNKNOWN = 0, RL_NAT_EIM = 1, RL_NAT_SYMMETRIC = 2 };
enum
{
   RL_E_NO_ROOM = 1, RL_E_FULL = 2, RL_E_VERSION = 3, RL_E_RATE = 4,
   RL_E_BAD = 5, RL_E_TOKEN = 6, RL_E_BUSY = 7, RL_E_PAUSED = 8
};

/* Room-code alphabet: letters only, A-Z (owner decision; the join screen warns about O/Q and I/J). */
#define RL_CODE_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define RL_CODE_SYMBOLS  26

static inline void rl_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void rl_put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void rl_put64(uint8_t *p, uint64_t v) { rl_put32(p, (uint32_t)(v >> 32)); rl_put32(p + 4, (uint32_t)v); }
static inline uint32_t rl_get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t rl_get32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t rl_get64(const uint8_t *p) { return ((uint64_t)rl_get32(p) << 32) | rl_get32(p + 4); }

static inline void rl_hdr(uint8_t *b, int type, int from, int to, uint64_t token)
{
   b[0] = RL_MAGIC0; b[1] = RL_MAGIC1; b[2] = RL_MAGIC2; b[3] = RL_MAGIC3;
   b[4] = RL_VER; b[5] = (uint8_t)type; b[6] = (uint8_t)from; b[7] = (uint8_t)to;
   rl_put64(b + 8, token);
}

/* 0 = a well-formed header (magic, version, length). */
static inline int rl_check(const uint8_t *b, int len)
{
   return (len >= RL_HDR && b[0] == RL_MAGIC0 && b[1] == RL_MAGIC1 &&
           b[2] == RL_MAGIC2 && b[3] == RL_MAGIC3 && b[4] == RL_VER) ? 0 : -1;
}

/* An IPv4 address in network order as 4 bytes <-> uint32 holding those
 * bytes in memory order (what sin_addr.s_addr holds). */
static inline void rl_put_ip(uint8_t *p, uint32_t ip_be) { memcpy(p, &ip_be, 4); }
static inline uint32_t rl_get_ip(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

#endif /* RELAY_PROTO_H */
