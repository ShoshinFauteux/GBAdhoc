/* relay_client.c -- one console's netdrv session over transport_relay on real
 * POSIX UDP sockets: the desktop stand-in for a PSP in the NAT matrix
 * (tools/relay/natlab.py) and a smoke test against any gbadhoc-relay.
 *
 *   relay_client --server IP:PORT (--host|--join) --code CODE
 *                [--bind IP:PORT] [--relay-only] [--payloads N] [--secs S]
 *                [--drop-direct-after MS]
 *                [--lookup]          look the code up first (a desktop host's code)
 *   relay_client --invite XXXX-XXXX (--host|--join) ...   a desktop host, no server
 *
 * Sends N reliable 8-byte payloads (a counter) one per 16 ms frame once the
 * session is up, expects N from the peer in order, then exits 0 with one
 * RESULT line:  RESULT ok=1 got=N bad=0 path=direct|relay|fellback ...
 */
#include "transport_relay.h"
#include "netdrv.h"
#include "invite_code.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_fd = -1;

static uint64_t now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int sk_send(void *c, uint32_t ip, uint16_t port, const void *b, size_t n)
{
   struct sockaddr_in a;
   (void)c;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   return sendto(g_fd, b, n, 0, (struct sockaddr *)&a, sizeof(a)) < 0 ? 0 : 0;
}

static int sk_recv(void *c, uint32_t *ip, uint16_t *port, void *b, size_t cap)
{
   struct sockaddr_in a;
   socklen_t al = sizeof(a);
   ssize_t r = recvfrom(g_fd, b, cap, MSG_DONTWAIT, (struct sockaddr *)&a, &al);
   (void)c;
   if (r <= 0)
      return 0;
   *ip = a.sin_addr.s_addr;
   *port = ntohs(a.sin_port);
   return (int)r;
}

static void lg(void *u, const char *l)
{
   printf("%s %s\n", (const char *)u, l);
   fflush(stdout);
}

typedef struct { uint32_t next, got, bad; int up, lost, stopped; } app_t;
static void a_deliver(void *u, const void *b, size_t n, uint8_t s)
{
   app_t *a = (app_t *)u;
   uint32_t v;
   (void)s;
   if (n != 8)
      return;
   memcpy(&v, b, 4);
   v = ntohl(v);
   if (v != a->next)
      a->bad++;
   a->next = v + 1;
   a->got++;
}
static void a_started(void *u, uint8_t id) { (void)id; ((app_t *)u)->up = 1; }
static int a_conn(void *u, uint8_t id) { (void)u; (void)id; return 0; }
static void a_disc(void *u, uint8_t id) { (void)id; ((app_t *)u)->lost++; }
static void a_stop(void *u, int r) { (void)r; ((app_t *)u)->stopped++; }

static int parse_ep(const char *s, uint32_t *ip, uint16_t *port)
{
   char b[64];
   char *c;
   snprintf(b, sizeof(b), "%s", s);
   c = strchr(b, ':');
   if (!c)
      return -1;
   *c = 0;
   *ip = inet_addr(b);
   *port = (uint16_t)atoi(c + 1);
   return 0;
}

int main(int argc, char **argv)
{
   rl_sock sk = { NULL, sk_send, sk_recv };
   rl_client_cfg cfg;
   relay_transport *rt;
   nd_transport tp;
   nd_callbacks cb;
   nd_config nc;
   netdrv *nd;
   app_t app;
   rl_client_stats st;
   uint32_t bind_ip = htonl(INADDR_ANY);
   uint16_t bind_port = 27500;
   int i, n_pay = 600, secs = 40, sent = 0, ok;
   uint64_t t0, next_frame;
   const char *tag;
   struct sockaddr_in a;

   memset(&cfg, 0, sizeof(cfg));
   cfg.direct = 1;
   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--server") && i + 1 < argc)
         parse_ep(argv[++i], &cfg.server_ip_be, &cfg.server_port);
      else if (!strcmp(argv[i], "--bind") && i + 1 < argc)
         parse_ep(argv[++i], &bind_ip, &bind_port);
      else if (!strcmp(argv[i], "--host")) cfg.is_host = 1;
      else if (!strcmp(argv[i], "--join")) cfg.is_host = 0;
      else if (!strcmp(argv[i], "--code") && i + 1 < argc)
         snprintf(cfg.code, sizeof(cfg.code), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--relay-only")) cfg.direct = 0;
      else if (!strcmp(argv[i], "--lookup")) cfg.lookup = 1;
      else if (!strcmp(argv[i], "--invite") && i + 1 < argc)
      {
         uint8_t ip[4];
         unsigned port;
         int r = inv_decode(argv[++i], ip, &port);
         if (r != INV_OK)
         {
            printf("RESULT ok=0 why=invite rc=%d\n", r);
            return 2;
         }
         memcpy(&cfg.server_ip_be, ip, 4);
         cfg.server_port = (uint16_t)port;
         cfg.code[0] = 0;
      }
      else if (!strcmp(argv[i], "--payloads") && i + 1 < argc) n_pay = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--drop-direct-after") && i + 1 < argc)
         cfg.drop_direct_after_us = (uint64_t)atoi(argv[++i]) * 1000u;
      else
      {
         fprintf(stderr, "usage: see relay_client.c\n");
         return 2;
      }
   }
   tag = cfg.is_host ? "host" : "join";
   g_fd = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = bind_ip;
   a.sin_port = htons(bind_port);
   if (g_fd < 0 || bind(g_fd, (struct sockaddr *)&a, sizeof(a)) < 0)
   {
      perror("bind");
      return 1;
   }
   fcntl(g_fd, F_SETFL, O_NONBLOCK);
   /* the private candidate: the address the OS picked for the route to the
    * server (connect-and-read trick on a second socket) */
   {
      int f2 = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in s, l;
      socklen_t ll = sizeof(l);
      memset(&s, 0, sizeof(s));
      s.sin_family = AF_INET;
      s.sin_addr.s_addr = cfg.server_ip_be;
      s.sin_port = htons(cfg.server_port);
      if (f2 >= 0 && connect(f2, (struct sockaddr *)&s, sizeof(s)) == 0 &&
          getsockname(f2, (struct sockaddr *)&l, &ll) == 0)
         cfg.priv_ip_be = l.sin_addr.s_addr;
      if (f2 >= 0)
         close(f2);
      cfg.priv_port = bind_port;
   }
   memcpy(cfg.build, "gbadhoc-relaytest", 16);
   cfg.now_us = now_us;
   cfg.log = lg;
   cfg.log_user = (void *)tag;
   rt = relay_transport_create(&sk, &cfg);
   t0 = now_us();
   while (relay_transport_poll(rt) != RL_ST_IN_ROOM)
   {
      if (relay_transport_poll(rt) == RL_ST_FAILED || now_us() - t0 > 30000000u)
      {
         printf("RESULT ok=0 why=no_room state=%d\n", relay_transport_poll(rt));
         return 1;
      }
      usleep(1000);
   }
   /* the role is the slot: a JOIN that opened a desktop host's home room is
    * told slot 0 and hosts the netdrv session */
   if (relay_transport_slot(rt) == 0 && !cfg.is_host)
   {
      cfg.is_host = 1;
      tag = "host";
   }
   memset(&app, 0, sizeof(app));
   memset(&cb, 0, sizeof(cb));
   cb.user = &app;
   cb.deliver = a_deliver;
   cb.session_started = a_started;
   cb.peer_connected = a_conn;
   cb.peer_disconnected = a_disc;
   cb.session_stopped = a_stop;
   memset(&nc, 0, sizeof(nc));
   nc.protocol = "gpSP v1.0";
   nc.join_nonce = (uint32_t)now_us() | 1u;
   relay_transport_iface(rt, &tp);
   nd = netdrv_create(&tp, &cb, &nc);
   if (cfg.is_host)
      netdrv_host(nd);
   else
      netdrv_join(nd);
   next_frame = now_us();
   while (now_us() - t0 < (uint64_t)secs * 1000000u)
   {
      netdrv_pump(nd, now_us());
      if (now_us() >= next_frame)
      {
         next_frame += 16742;
         if (netdrv_active(nd) && netdrv_peer_count(nd) > 0 && sent < n_pay)
         {
            uint8_t p[8];
            uint32_t v = htonl((uint32_t)sent);
            memcpy(p, &v, 4);
            memset(p + 4, 0x5A, 4);
            if (netdrv_send(nd, ND_RELIABLE | ND_FLUSH_HINT, p, 8,
                            cfg.is_host ? 1 : 0) == 0)
               sent++;
         }
      }
      if (app.got >= (uint32_t)n_pay && sent >= n_pay && netdrv_unacked(nd, cfg.is_host ? 1 : 0) == 0)
         break;
      usleep(1000);
   }
   relay_transport_stats(rt, &st);
   {
      int peer = cfg.is_host ? 1 : 0;
      int p = st.path[peer];
      nd_stats ns;
      netdrv_get_stats(nd, &ns);
      ok = app.got == (uint32_t)n_pay && !app.bad && !app.lost && !app.stopped;
      printf("RESULT ok=%d side=%s got=%u bad=%u sent=%d lost=%d path=%s nat=%u "
             "rx_direct=%u rx_relay=%u fallbacks=%u rtt_direct_ms=%u rtt_relay_ms=%u "
             "srtt_ms=%u retx=%u located=%u server=%s:%u secs=%.1f\n",
             ok, tag, app.got, app.bad, sent, app.lost,
             p == 3 ? "direct" : p == 4 ? "fellback" : p == 2 ? "punching" : "relay",
             st.nat, st.rx_direct, st.rx_relay, st.fallbacks, st.direct_rtt_us[peer] / 1000,
             st.relay_rtt_us / 1000, ns.srtt_us / 1000, ns.retx, st.located,
             inet_ntoa(*(struct in_addr *)&st.host_ip_be), st.host_port,
             (now_us() - t0) / 1e6);
   }
   /* linger: answer the peer's last acks */
   t0 = now_us();
   while (now_us() - t0 < 1500000u)
   {
      netdrv_pump(nd, now_us());
      usleep(2000);
   }
   netdrv_destroy(nd);
   relay_transport_destroy(rt);
   return ok ? 0 : 1;
}
