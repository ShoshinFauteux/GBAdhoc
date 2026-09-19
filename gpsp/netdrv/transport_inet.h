/* transport_inet.h — PSP sceNetInet (infrastructure Wi-Fi) backend for netdrv.
 *
 * WHY THIS EXISTS ALONGSIDE transport_adhoc.
 *
 * transport_adhoc is sceNetAdhoc PDP: IBSS, MAC-addressed, the shipped path for
 * two PSPs trading.  This one carries the SAME netdrv frames over ordinary UDP
 * on an infrastructure network, because the peer is an Android phone playing a
 * Mystery Gift distribution cart and stock Android cannot create an IBSS.  The
 * phone is the access point; the PSP associates as a station (mgift_net.c brings
 * apctl up) and this owns the socket.
 *
 * It is a near-transcription of netdrv/transport_udp.c, the desktop backend,
 * down to the fake-MAC trick: netdrv addresses peers by a 6-byte MAC, and both
 * UDP backends synthesize one from 4 bytes of IPv4 plus 2 bytes of port
 * (netdrv.h:171).  A roster MAC therefore decodes straight back to a reachable
 * address with no lookup table.  Keeping the two files parallel is deliberate:
 * the desktop twin is where this protocol gets debugged, and a divergence there
 * is a bug that only reproduces on hardware.
 *
 * THE ONE REAL DIFFERENCE FROM transport_udp.  Its broadcast() sends only to
 * addresses already in the book, which is fine on a desktop mesh where every
 * peer is seeded from the command line.  Here the PSP is the JOINER and its
 * very first frame is a broadcast ND_T_JOIN sent before it knows the host's
 * address at all -- so book-only broadcast would mean the JOIN reaches nobody
 * and the session never starts.  broadcast() therefore also sends to a
 * configured broadcast address.  See inet_transport_create.
 *
 * NETDRV AND rfu.c ARE UNTOUCHED BY THIS FEATURE.  netdrv consumes an
 * nd_transport vtable and does not care what is under it -- that is the whole
 * point of the seam (netdrv.h:171-174 names transport_udp and transport_adhoc as
 * the two intended backends).  Handing it a third backend adds no code to the
 * shipped trading path, which is what keeps that path from regressing.
 */
#ifndef TRANSPORT_INET_H
#define TRANSPORT_INET_H

#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct inet_transport inet_transport;

/* Create the socket.  The network must ALREADY be up -- sceNetInit,
 * sceNetInetInit and an apctl association with GOT_IP (mgift_net.c owns that
 * ladder, and owns tearing it back down).  This deliberately does not touch
 * apctl: the association is a user-visible, seconds-long, failure-prone step
 * that belongs with the UI that narrates it, not buried in a transport.
 *
 *   local_port   port to bind (0 = ephemeral, but the phone needs a fixed one
 *                to answer before the roster exists, so pass a real port).
 *   bcast_be     broadcast address to use for peer-less broadcast, in NETWORK
 *                byte order (e.g. htonl(INADDR_BROADCAST)).  0 disables it and
 *                makes broadcast() book-only, matching transport_udp.
 *   peer_port    port to aim broadcasts at (the phone's listener).
 *
 * Returns NULL on error. */
inet_transport *inet_transport_create(uint16_t local_port, uint32_t bcast_be,
                                      uint16_t peer_port);
void inet_transport_destroy(inet_transport *t);

/* Fill the vtable for netdrv_create()/fe_np_start(). Valid while created. */
void inet_transport_iface(inet_transport *t, nd_transport *out);

/* Diagnostics, for the OSD in a build with no log. */
typedef struct inet_transport_stats
{
   uint32_t tx_pkts, tx_fail;
   uint32_t rx_pkts, rx_err, rx_oversize;
   uint32_t bcast_pkts;
   uint32_t book_n;
} inet_transport_stats;

void inet_transport_get_stats(const inet_transport *t,
                              inet_transport_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_INET_H */
