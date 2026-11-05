/* relay_core.h -- the GBR1 relay / rendezvous server, as a portable core:
 * no threads, no sockets, no allocation after relay_core_create.  A host
 * (tools/relay/relay_server.c on a VPS or this PC; later GBAdhoc XMB) owns
 * two UDP sockets (port and port + 1), feeds every datagram in and sends
 * what the core asks it to.  docs/ONLINE-TUNNEL.md sections 5-6.
 *
 * EXPERIMENTAL.  Nothing in a shipped build links it.
 */
#ifndef RELAY_CORE_H
#define RELAY_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "relay_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rl_addr
{
   uint32_t ip_be;      /* network order, as sin_addr.s_addr */
   uint16_t port;       /* HOST order */
} rl_addr;

typedef struct rl_core_cfg
{
   unsigned max_rooms;           /* 0 = 256 */
   unsigned room_idle_ms;        /* drop a room this long after its last packet; 0 = 60000 */
   unsigned lobby_ms;            /* a room nobody joined expires; 0 = 600000 */
   unsigned slot_idle_ms;        /* drop a silent slot; 0 = 20000 */
   unsigned pkt_per_s;           /* per-slot / per-source rate; 0 = 300 */
   unsigned joins_per_min;       /* CREATE+JOIN per source IP; 0 = 30 */
   unsigned rooms_per_ip;        /* live rooms one IP may host; 0 = 3 */
   unsigned bytes_per_s;         /* per-slot byte rate; 0 = 65536 */
   unsigned punch_start_ms;      /* CANDIDATES start offset; 0 = 150 */
   int      no_direct;           /* 1 = never offer a punch (relay only) */
   int      paused;              /* 1 = refuse CREATE/JOIN with RL_E_PAUSED */
   /* the directory (RL_T_REGISTER / RL_T_LOOKUP): the shared server keeps it,
    * a desktop host does not (it still answers LOOKUP for its own rooms) */
   int      directory;           /* 1 = accept REGISTER */
   unsigned max_dir;             /* entries; 0 = 256 */
   unsigned dir_per_ip;          /* live entries one IP may hold; 0 = 2 */
   unsigned lookups_per_min;     /* LOOKUPs per source IP, any code; 0 = 120 */
   /* send one datagram from socket `sock` (0 = main port, 1 = probe port) */
   void (*send)(void *user, int sock, const rl_addr *to, const void *buf, size_t len);
   /* 64 random bits (tokens, nonces, codes) */
   uint64_t (*random64)(void *user);
   void (*log)(void *user, const char *line);
   void *user;
} rl_core_cfg;

typedef struct rl_core_stats
{
   uint32_t rx, tx, rx_bad, rx_token, rx_rate, fwd, fwd_bytes;
   uint32_t rooms_now, rooms_made, joins, refused, punches_offered, paths_direct;
   uint32_t cookies_sent, cookie_bad, rx_bytes_rate;
   uint32_t dir_now, dir_made, dir_refreshed, lookups, located, dir_rx_bytes, dir_tx_bytes;
} rl_core_stats;

typedef struct rl_core rl_core;

rl_core *relay_core_create(const rl_core_cfg *cfg);
void relay_core_destroy(rl_core *c);
/* One datagram that arrived on socket `sock` from `from`. */
void relay_core_input(rl_core *c, int sock, const rl_addr *from,
                      const void *buf, size_t len, uint64_t now_ms);
/* Timers: expire rooms and slots.  Call every ~100 ms. */
void relay_core_tick(rl_core *c, uint64_t now_ms);
void relay_core_stats(const rl_core *c, rl_core_stats *out);

/* A desktop host's own room code (four letters; NULL clears it).  A CREATE
 * without a code gets it while no room holds it, and a JOIN without a code
 * (a PSP that came by an invite code) joins that room. */
void relay_core_set_home(rl_core *c, const char *code);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_CORE_H */
