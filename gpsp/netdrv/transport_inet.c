/* transport_inet.c — PSP sceNetInet backend for netdrv.  See transport_inet.h
 * for why this exists next to transport_adhoc, and for the one behavioural
 * difference from the desktop transport_udp it is transcribed from.
 */

#include "transport_inet.h"

#include <pspkernel.h>
#include <pspnet_inet.h>

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#define INET_BOOK_CAP 8        /* peers; a Mystery Gift session has exactly 1 */

struct inet_transport
{
   int      fd;
   uint16_t port;             /* our bound port, host order */
   uint32_t bcast_be;         /* broadcast target, network order (0 = off) */
   uint16_t peer_port_be;     /* where broadcasts go, network order */

   struct sockaddr_in book[INET_BOOK_CAP];
   int      book_n;

   inet_transport_stats st;
};

/* One static instance: ADR-0007 memory posture -- the frontend has no heap to
 * spare once gpSP's ROM cache has eaten it (heap-exhausted-by-design), and a
 * PSP session is single-peer anyway. */
static inet_transport g_inet;
static int            g_inet_live;

/* netdrv addresses peers by MAC; both UDP backends fake one from ip:port
 * (netdrv.h:171).  Kept byte-identical to transport_udp.c so a roster entry
 * means the same thing on the desktop twin and on hardware. */
static void mac_from_addr(const struct sockaddr_in *a, uint8_t mac[6])
{
   memcpy(mac, &a->sin_addr.s_addr, 4);       /* network order */
   memcpy(mac + 4, &a->sin_port, 2);          /* network order */
}

static void addr_from_mac(const uint8_t mac[6], struct sockaddr_in *a)
{
   memset(a, 0, sizeof(*a));
   a->sin_family = AF_INET;
   memcpy(&a->sin_addr.s_addr, mac, 4);
   memcpy(&a->sin_port, mac + 4, 2);
}

static void book_add(inet_transport *t, const struct sockaddr_in *a)
{
   int i;
   for (i = 0; i < t->book_n; i++)
      if (t->book[i].sin_addr.s_addr == a->sin_addr.s_addr &&
          t->book[i].sin_port == a->sin_port)
         return;
   if (t->book_n < INET_BOOK_CAP)
      t->book[t->book_n++] = *a;
   t->st.book_n = (uint32_t)t->book_n;
}

static void raw_send(inet_transport *t, const struct sockaddr_in *to,
                     const void *buf, size_t len)
{
   /* sceNetInetSendto is declared returning size_t, so a failure arrives as
    * 0xFFFFFFFF rather than -1.  Cast before testing or every error reads as a
    * 4 GB success -- the same trap as recvfrom below. */
   if ((int)sceNetInetSendto(t->fd, buf, len, 0,
                             (const struct sockaddr *)to, sizeof(*to)) < 0)
      t->st.tx_fail++;
   else
      t->st.tx_pkts++;
}

static int inet_send_to(void *ctx, const uint8_t mac[6],
                        const void *buf, size_t len)
{
   inet_transport *t = (inet_transport *)ctx;
   struct sockaddr_in to;

   addr_from_mac(mac, &to);
   book_add(t, &to);          /* roster MACs decode to reachable addresses */
   raw_send(t, &to, buf, len);
   return 0;                  /* loss is fine -- ARQ owns reliability */
}

static int inet_broadcast(void *ctx, const void *buf, size_t len)
{
   inet_transport *t = (inet_transport *)ctx;
   int i;

   for (i = 0; i < t->book_n; i++)
      raw_send(t, &t->book[i], buf, len);

   /* AND to the broadcast address, unlike transport_udp.  The PSP is the
    * joiner: its first frame is ND_T_JOIN with an empty book, so a book-only
    * broadcast would be sent to nobody and the session could never start.
    * Harmless once the book is populated -- the phone sees the JOIN twice and
    * netdrv is duplicate-tolerant by construction. */
   if (t->bcast_be)
   {
      struct sockaddr_in to;
      memset(&to, 0, sizeof(to));
      to.sin_family      = AF_INET;
      to.sin_addr.s_addr = t->bcast_be;
      to.sin_port        = t->peer_port_be;
      raw_send(t, &to, buf, len);
      t->st.bcast_pkts++;
   }
   return 0;
}

static int inet_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   inet_transport *t = (inet_transport *)ctx;
   struct sockaddr_in from;
   socklen_t flen = sizeof(from);
   int n;

   memset(&from, 0, sizeof(from));
   /* MSG_DONTWAIT as well as SO_NONBLOCK: belt and braces, because this is
    * called from the emulation thread and a blocking recv here would stall a
    * frame. */
   n = (int)sceNetInetRecvfrom(t->fd, buf, cap, MSG_DONTWAIT,
                               (struct sockaddr *)&from, &flen);
   if (n < 0)
      return 0;               /* would block / nothing pending */
   if (n == 0)
      return 0;
   if ((size_t)n > cap)
   {
      t->st.rx_oversize++;
      return 0;
   }

   /* Learn the sender so replies and broadcasts reach it.  netdrv's own
    * validation wall (magic/ver/type/len/CRC, netdrv_wire.h) is what keeps
    * garbage off the core -- this layer deliberately does not second-guess it. */
   book_add(t, &from);
   mac_from_addr(&from, src_mac);
   t->st.rx_pkts++;
   return n;
}

static void inet_local_addr(void *ctx, uint8_t mac[6])
{
   inet_transport *t = (inet_transport *)ctx;
   struct sockaddr_in a;
   socklen_t alen = sizeof(a);

   memset(&a, 0, sizeof(a));
   if (sceNetInetGetsockname(t->fd, (struct sockaddr *)&a, &alen) < 0 ||
       a.sin_addr.s_addr == htonl(INADDR_ANY))
   {
      /* Bound to 0.0.0.0, which is the normal case: netdrv only needs this to
       * be a stable, unique handle for US, and the phone learns our real
       * address from the datagram source anyway.  Advertise the port with a
       * zero host part rather than inventing a plausible-looking IP. */
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      a.sin_port        = htons(t->port);
   }
   mac_from_addr(&a, mac);
}

/* No `pending` hook on purpose.  It must be answerable without a syscall
 * (netdrv.h:188-199 -- the core polls it hundreds of times per emulated
 * frame), and sceNetInet gives us no way to peek that cheaply.  Leaving it
 * NULL selects netdrv's documented fallback: assume yes, pump every poll. */

inet_transport *inet_transport_create(uint16_t local_port, uint32_t bcast_be,
                                      uint16_t peer_port)
{
   inet_transport *t = &g_inet;
   struct sockaddr_in addr;
   int opt;

   if (g_inet_live)
      return NULL;

   memset(t, 0, sizeof(*t));
   t->fd           = -1;
   t->port         = local_port;
   t->bcast_be     = bcast_be;
   t->peer_port_be = htons(peer_port);

   t->fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
   if (t->fd < 0)
      return NULL;

   opt = 1;
   sceNetInetSetsockopt(t->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   opt = 1;
   sceNetInetSetsockopt(t->fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
   /* The PSP has no fcntl; SO_NONBLOCK is how a socket is made non-blocking. */
   opt = 1;
   sceNetInetSetsockopt(t->fd, SOL_SOCKET, SO_NONBLOCK, &opt, sizeof(opt));

   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_port        = htons(local_port);
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   if (sceNetInetBind(t->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
   {
      sceNetInetClose(t->fd);
      t->fd = -1;
      return NULL;
   }

   g_inet_live = 1;
   return t;
}

void inet_transport_destroy(inet_transport *t)
{
   if (!t || !g_inet_live)
      return;
   if (t->fd >= 0)
      sceNetInetClose(t->fd);
   t->fd       = -1;
   t->book_n   = 0;
   g_inet_live = 0;
}

void inet_transport_iface(inet_transport *t, nd_transport *out)
{
   /* Zero FIRST.  Callers legitimately pass uninitialised stack storage, and
    * the optional `pending` hook must read back NULL rather than whatever was
    * on the stack -- transport_udp.c carries the scar from exactly that
    * (a call through a stack value, segfaulting the desktop twin). */
   memset(out, 0, sizeof(*out));
   out->ctx        = t;
   out->send_to    = inet_send_to;
   out->broadcast  = inet_broadcast;
   out->recv       = inet_recv;
   out->local_addr = inet_local_addr;
   out->pending    = NULL;      /* see the note above inet_transport_create */
}

void inet_transport_get_stats(const inet_transport *t,
                              inet_transport_stats *out)
{
   if (!t || !out)
      return;
   *out = t->st;
}
