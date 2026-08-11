/* transport_udp.c — UDP backend + fault-injection shim. See transport_udp.h. */
#include "transport_udp.h"
#include "netdrv_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDP_BOOK_CAP   16     /* address book (max mesh is 5 anyway) */
#define UDP_DELAYQ_CAP 512    /* in-flight delayed datagrams (fault shim) */

typedef struct
{
   uint64_t due_us;
   struct sockaddr_in to;
   uint16_t len;
   uint8_t  data[ND_MAX_FRAME];
} udp_delayed;

struct udp_transport
{
   int fd;
   uint16_t port;                    /* host byte order */
   struct sockaddr_in book[UDP_BOOK_CAP];
   int book_n;

   /* fault shim */
   int loss_pct, dup_pct, latency_ms, jitter_ms;
   uint32_t prng;
   udp_delayed dq[UDP_DELAYQ_CAP];
   int dq_n;
};

/* ------------------------------------------------------------------ util -- */

static uint64_t now_us_(void)
{
   /* Monotonic: the fault shim's delay queue must not be affected by
    * wall-clock steps. */
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t prng_next(udp_transport *t)
{
   uint32_t x = t->prng ? t->prng : 0x9E3779B9u;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   t->prng = x;
   return x;
}

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

static void book_add(udp_transport *t, const struct sockaddr_in *a)
{
   int i;
   for (i = 0; i < t->book_n; i++)
      if (t->book[i].sin_addr.s_addr == a->sin_addr.s_addr &&
          t->book[i].sin_port == a->sin_port)
         return;
   if (t->book_n < UDP_BOOK_CAP)
      t->book[t->book_n++] = *a;
}

/* --------------------------------------------------------- faulty sending -- */

static void raw_send(udp_transport *t, const struct sockaddr_in *to,
                     const void *buf, size_t len)
{
   sendto(t->fd, buf, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void dq_flush(udp_transport *t, uint64_t now)
{
   int i = 0;
   while (i < t->dq_n)
   {
      if (t->dq[i].due_us <= now)
      {
         raw_send(t, &t->dq[i].to, t->dq[i].data, t->dq[i].len);
         t->dq[i] = t->dq[--t->dq_n];   /* order irrelevant: due-based */
      }
      else
         i++;
   }
}

static void shim_send(udp_transport *t, const struct sockaddr_in *to,
                      const void *buf, size_t len)
{
   int copies = 1;
   if (!t->loss_pct && !t->dup_pct && !t->latency_ms && !t->jitter_ms)
   {
      raw_send(t, to, buf, len);
      return;
   }
   if (t->loss_pct && (int)(prng_next(t) % 100) < t->loss_pct)
      return;                                        /* dropped */
   if (t->dup_pct && (int)(prng_next(t) % 100) < t->dup_pct)
      copies = 2;
   while (copies--)
   {
      uint32_t delay_ms = (uint32_t)t->latency_ms +
         (t->jitter_ms ? prng_next(t) % (uint32_t)(t->jitter_ms + 1) : 0);
      if (!delay_ms)
         raw_send(t, to, buf, len);
      else if (t->dq_n < UDP_DELAYQ_CAP && len <= ND_MAX_FRAME)
      {
         udp_delayed *d = &t->dq[t->dq_n++];
         d->due_us = now_us_() + (uint64_t)delay_ms * 1000ull;
         d->to = *to;
         d->len = (uint16_t)len;
         memcpy(d->data, buf, len);
      }
      else
         raw_send(t, to, buf, len);                  /* queue full: send now */
   }
}

/* -------------------------------------------------------- vtable functions -- */

static int udp_send_to(void *ctx, const uint8_t mac[6],
                       const void *buf, size_t len)
{
   udp_transport *t = (udp_transport *)ctx;
   struct sockaddr_in to;
   addr_from_mac(mac, &to);
   book_add(t, &to);          /* roster MACs decode to reachable addresses */
   shim_send(t, &to, buf, len);
   return 0;
}

static int udp_broadcast(void *ctx, const void *buf, size_t len)
{
   udp_transport *t = (udp_transport *)ctx;
   int i;
   for (i = 0; i < t->book_n; i++)
      shim_send(t, &t->book[i], buf, len);
   return 0;
}

static int udp_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   udp_transport *t = (udp_transport *)ctx;
   struct sockaddr_in from;
   socklen_t flen = sizeof(from);
   ssize_t n;

   dq_flush(t, now_us_());    /* release delayed sends */

   n = recvfrom(t->fd, buf, cap, 0, (struct sockaddr *)&from, &flen);
   if (n < 0)
      return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
   if (n == 0)
      return 0;
   book_add(t, &from);
   mac_from_addr(&from, src_mac);
   return (int)n;
}

static void udp_local_addr(void *ctx, uint8_t mac[6])
{
   udp_transport *t = (udp_transport *)ctx;
   struct sockaddr_in a;
   socklen_t alen = sizeof(a);
   memset(&a, 0, sizeof(a));
   if (getsockname(t->fd, (struct sockaddr *)&a, &alen) != 0 ||
       a.sin_addr.s_addr == INADDR_ANY)
   {
      /* Bound to 0.0.0.0: advertise loopback (desktop mesh runs on one
       * machine; multi-host desktop use would need an explicit bind ip). */
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      a.sin_port = htons(t->port);
   }
   mac_from_addr(&a, mac);
}

/* ------------------------------------------------------------------- API -- */

udp_transport *udp_transport_create(uint16_t local_port,
                                    const char *join_host, uint16_t join_port)
{
   udp_transport *t = (udp_transport *)calloc(1, sizeof(*t));
   struct sockaddr_in a;
   socklen_t alen = sizeof(a);
   int fl;

   if (!t)
      return NULL;
   t->fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (t->fd < 0)
   {
      free(t);
      return NULL;
   }
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   a.sin_port = htons(local_port);
   if (bind(t->fd, (struct sockaddr *)&a, sizeof(a)) != 0)
   {
      close(t->fd);
      free(t);
      return NULL;
   }
   if (getsockname(t->fd, (struct sockaddr *)&a, &alen) == 0)
      t->port = ntohs(a.sin_port);

   fl = fcntl(t->fd, F_GETFL, 0);
   fcntl(t->fd, F_SETFL, fl | O_NONBLOCK);

   if (join_host && join_port)
   {
      struct sockaddr_in h;
      memset(&h, 0, sizeof(h));
      h.sin_family = AF_INET;
      h.sin_port = htons(join_port);
      if (inet_pton(AF_INET, join_host, &h.sin_addr) != 1)
      {
         close(t->fd);
         free(t);
         return NULL;
      }
      book_add(t, &h);
   }
   t->prng = 0x243F6A88u;
   return t;
}

void udp_transport_destroy(udp_transport *t)
{
   if (!t)
      return;
   close(t->fd);
   free(t);
}

void udp_transport_iface(udp_transport *t, nd_transport *out)
{
   /* Zero first: callers legitimately pass an uninitialised stack struct
    * (sdl/main_sdl.c does), so every OPTIONAL hook must read back NULL, not
    * whatever was on the stack. Adding nd_transport.pending without this
    * segfaulted the desktop twin the moment the core first polled — caught
    * by run_trade_test.sh, invisible to the PSP rig, whose vtable happened
    * to live in .bss. Any future hook is covered by construction now. */
   memset(out, 0, sizeof(*out));
   out->ctx        = t;
   out->send_to    = udp_send_to;
   out->broadcast  = udp_broadcast;
   out->recv       = udp_recv;
   out->local_addr = udp_local_addr;
}

void udp_transport_set_fault(udp_transport *t, int loss_pct, int dup_pct,
                             int latency_ms, int jitter_ms, uint32_t seed)
{
   t->loss_pct   = loss_pct;
   t->dup_pct    = dup_pct;
   t->latency_ms = latency_ms;
   t->jitter_ms  = jitter_ms;
   if (seed)
      t->prng = seed;
}

uint16_t udp_transport_port(const udp_transport *t)
{
   return t->port;
}
