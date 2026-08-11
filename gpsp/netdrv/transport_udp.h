/* transport_udp.h — desktop UDP backend for netdrv (plan §4.3).
 *
 * Addresses are faked as 6-byte MACs: mac[0..3] = IPv4 (network order),
 * mac[4..5] = UDP port (network order). "Broadcast" = unicast fan-out to an
 * address book seeded with the optional join target and grown from every
 * datagram source ever received (host learns joiners, clients learn the
 * mesh from roster MACs — which decode straight back to ip:port).
 *
 * Includes a togglable fault-injection shim (loss/dup/latency+jitter which
 * also yields reorder) applied on the send side; used by unit tests and the
 * netsmoke harness. POSIX sockets; desktop only (WSL/Linux).
 */
#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct udp_transport udp_transport;

/* Bind a UDP socket on local_port (0 = ephemeral). join_host/join_port
 * optionally seed the address book with the session host ("--join ip:port");
 * pass NULL/0 when hosting. Returns NULL on error. */
udp_transport *udp_transport_create(uint16_t local_port,
                                    const char *join_host, uint16_t join_port);
void udp_transport_destroy(udp_transport *t);

/* Fill the nd_transport vtable for netdrv_create(). */
void udp_transport_iface(udp_transport *t, nd_transport *out);

/* Fault-injection shim (all zero = transparent). loss/dup in percent
 * [0..100]; latency/jitter in milliseconds (each sent datagram is delayed
 * latency + uniform[0..jitter] ms — jitter also produces reordering).
 * seed selects the deterministic PRNG stream. */
void udp_transport_set_fault(udp_transport *t, int loss_pct, int dup_pct,
                             int latency_ms, int jitter_ms, uint32_t seed);

/* Bound local port (useful after ephemeral bind). */
uint16_t udp_transport_port(const udp_transport *t);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_UDP_H */
