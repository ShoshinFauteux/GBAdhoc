/* transport_relay.h -- netdrv over the internet: one UDP socket to a GBR1
 * relay / rendezvous server (netdrv/relay_core.c), and a direct hole-punched
 * path to each peer when both NATs allow it, the relay being the automatic
 * fallback.  docs/ONLINE-TUNNEL.md sections 5-6.
 *
 * EXPERIMENTAL, off by default: nothing creates one unless a harness key
 * (PSP: online_relay) or a command line asks.
 *
 * Portable: the socket is a two-hook shim, so the same code runs on the PSP
 * (sceNetInet, psp/online_psp.c), on the desktop (tools/relay/) and in the
 * in-process NAT-model test.  ONE socket carries everything -- the server,
 * both PROBEs and every peer -- because that is what makes a hole punch work:
 * the NAT reuses the mapping the server saw.
 *
 * netdrv addresses peers by MAC; a slot's MAC is 02:47:42:52:00:<slot>.
 * Single-threaded: every call from the thread that pumps netdrv.
 */
#ifndef TRANSPORT_RELAY_H
#define TRANSPORT_RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "netdrv.h"
#include "relay_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rl_sock
{
   void *ctx;
   /* send one datagram; ip in network order, port in host order.
    * 0 = sent (or dropped: UDP), <0 = hard error */
   int (*send)(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len);
   /* one pending datagram: its length (>0), 0 = none, <0 = error */
   int (*recv)(void *ctx, uint32_t *ip_be, uint16_t *port, void *buf, size_t cap);
} rl_sock;

typedef struct rl_client_cfg
{
   uint32_t server_ip_be;
   uint16_t server_port;           /* host order; the probe port is + 1 */
   int is_host;                    /* CREATE (1) or JOIN (0) */
   char code[RL_CODE_LEN + 1];     /* the room; "" = server picks (host only) */
   uint8_t build[RL_BUILD_LEN];    /* this binary's identity; rooms match it */
   uint32_t priv_ip_be;            /* this console's LAN address and port */
   uint16_t priv_port;             /*   (the same-LAN punch candidate) */
   int direct;                     /* 1 = try the direct path; 0 = relay only */
   int mode;                       /* host: the room's link mode (RL_MODE_*) */
   uint64_t (*now_us)(void);
   void (*log)(void *user, const char *line);   /* optional, EVT lines */
   void *log_user;
   /* harness only: drop every DIRECT datagram received from this moment on
    * (microseconds since create; 0 = never) -- forces the fallback */
   uint64_t drop_direct_after_us;
} rl_client_cfg;

enum
{
   RL_ST_PROBING = 1,    /* PROBEs out, classifying the NAT */
   RL_ST_ENTERING,       /* CREATE/JOIN out, waiting for the slot */
   RL_ST_IN_ROOM,        /* have a slot: netdrv can run */
   RL_ST_FAILED          /* refused (see relay_transport_error) */
};

typedef struct relay_transport relay_transport;

relay_transport *relay_transport_create(const rl_sock *s, const rl_client_cfg *cfg);
void relay_transport_destroy(relay_transport *t);   /* sends BYE */

/* Drive the handshake and the timers (also run from recv()).  Returns the
 * state.  Call until IN_ROOM before netdrv_host/netdrv_join. */
int relay_transport_poll(relay_transport *t);
int relay_transport_error(const relay_transport *t, const char **text);
const char *relay_transport_code(const relay_transport *t);
/* The room's link mode as the server tells it (RL_MODE_*; valid IN_ROOM). */
int relay_transport_mode(const relay_transport *t);
int relay_transport_slot(const relay_transport *t);
uint64_t relay_transport_server_silent_us(relay_transport *t);

void relay_transport_iface(relay_transport *t, nd_transport *out);

typedef struct rl_client_stats
{
   uint32_t tx, rx, tx_relay, tx_direct, rx_relay, rx_direct, rx_bad;
   uint32_t punches_tx, punches_rx, acks_rx, keeps_tx, keeps_rx;
   uint8_t nat;                    /* RL_NAT_* */
   uint32_t pub_ip_be;
   uint16_t pub_port[2];
   /* per peer slot: 0 none, 1 relay, 2 punching, 3 direct, 4 fell back */
   uint8_t path[RL_SLOTS];
   uint32_t direct_rtt_us[RL_SLOTS];
   uint32_t relay_rtt_us;
   uint32_t fallbacks;
   uint32_t reenters;              /* host: lobby room lost, entered again */
} rl_client_stats;
void relay_transport_stats(const relay_transport *t, rl_client_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_RELAY_H */
