/* relay_server.c -- gbadhoc-relay: the GBR1 relay / rendezvous server
 * (netdrv/relay_core.c) on POSIX UDP sockets.  docs/ONLINE-TUNNEL.md 6.
 *
 *   gbadhoc-relay [--bind IP] [--port P] [--no-direct] [--quiet]
 *       listens on P (default 27420) and P + 1 (the PROBE port)
 *   gbadhoc-relay --fwd PORT [--port P]
 *       FORWARDED mode, for a host the consoles cannot reach directly (WSL2
 *       behind Windows' NAT): one socket on PORT receives datagrams framed
 *       by tools/relay/winfwd.py as  ip[4] port[2] sock[1] payload  and the
 *       replies go back framed the same way, so the core still sees every
 *       console's real address.
 *
 * Prints one line per event and a counter line every 10 s.  Never binds a
 * public address unless --bind says so: the default is 127.0.0.1.
 */
#include "relay_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_fd[2] = { -1, -1 };
static int g_fwd = -1;
static struct sockaddr_in g_fwd_peer;
static int g_have_fwd_peer;
static int g_quiet;

static uint64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t rnd64(void *u)
{
   uint64_t v = 0;
   FILE *f = fopen("/dev/urandom", "rb");
   (void)u;
   if (!f || fread(&v, 1, sizeof(v), f) != sizeof(v))
      v = now_ms() * 6364136223846793005ull + 1442695040888963407ull;
   if (f)
      fclose(f);
   return v;
}

static void say(void *u, const char *l)
{
   (void)u;
   if (!g_quiet)
   {
      printf("%llu %s\n", (unsigned long long)now_ms(), l);
      fflush(stdout);
   }
}

static void do_send(void *u, int sock, const rl_addr *to, const void *buf, size_t len)
{
   (void)u;
   if (g_fwd >= 0)
   {
      uint8_t b[RL_MAX_DGRAM + 7];
      if (!g_have_fwd_peer || len > RL_MAX_DGRAM)
         return;
      memcpy(b, &to->ip_be, 4);
      b[4] = (uint8_t)(to->port >> 8);
      b[5] = (uint8_t)to->port;
      b[6] = (uint8_t)sock;
      memcpy(b + 7, buf, len);
      sendto(g_fwd, b, len + 7, 0, (struct sockaddr *)&g_fwd_peer, sizeof(g_fwd_peer));
      return;
   }
   {
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = to->ip_be;
      a.sin_port = htons(to->port);
      sendto(g_fd[sock], buf, len, 0, (struct sockaddr *)&a, sizeof(a));
   }
}

static int open_udp(const char *ip, int port)
{
   struct sockaddr_in a;
   int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
   if (fd < 0)
      return -1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_port = htons((uint16_t)port);
   a.sin_addr.s_addr = inet_addr(ip);
   if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
   {
      fprintf(stderr, "bind %s:%d: %s\n", ip, port, strerror(errno));
      close(fd);
      return -1;
   }
   fcntl(fd, F_SETFL, O_NONBLOCK);
   return fd;
}

int main(int argc, char **argv)
{
   const char *bind_ip = "127.0.0.1";
   int port = RL_DEFAULT_PORT, fwd_port = 0, i;
   rl_core_cfg cfg;
   rl_core *core;
   uint64_t last_stat = now_ms(), last_tick = 0;
   memset(&cfg, 0, sizeof(cfg));
   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_ip = argv[++i];
      else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--fwd") && i + 1 < argc) fwd_port = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--no-direct")) cfg.no_direct = 1;
      else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
      else
      {
         fprintf(stderr, "usage: gbadhoc-relay [--bind IP] [--port P] [--fwd PORT] [--no-direct] [--quiet]\n");
         return 2;
      }
   }
   if (fwd_port)
   {
      g_fwd = open_udp(bind_ip, fwd_port);
      if (g_fwd < 0)
         return 1;
   }
   else
   {
      g_fd[0] = open_udp(bind_ip, port);
      g_fd[1] = open_udp(bind_ip, port + 1);
      if (g_fd[0] < 0 || g_fd[1] < 0)
         return 1;
   }
   cfg.send = do_send;
   cfg.random64 = rnd64;
   cfg.log = say;
   core = relay_core_create(&cfg);
   if (!core)
      return 1;
   printf("gbadhoc-relay up %s port=%d%s%s\n", bind_ip, fwd_port ? fwd_port : port,
          fwd_port ? " (forwarded)" : "", cfg.no_direct ? " no-direct" : "");
   fflush(stdout);
   for (;;)
   {
      struct pollfd pf[2];
      int n = 0, k;
      if (g_fwd >= 0)
      {
         pf[n].fd = g_fwd; pf[n].events = POLLIN; n++;
      }
      else
         for (k = 0; k < 2; k++)
         {
            pf[n].fd = g_fd[k]; pf[n].events = POLLIN; n++;
         }
      poll(pf, (nfds_t)n, 50);
      for (k = 0; k < n; k++)
      {
         uint8_t b[RL_MAX_DGRAM + 8];
         for (;;)
         {
            struct sockaddr_in a;
            socklen_t al = sizeof(a);
            ssize_t r = recvfrom(pf[k].fd, b, sizeof(b), 0, (struct sockaddr *)&a, &al);
            rl_addr from;
            if (r <= 0)
               break;
            if (g_fwd >= 0)
            {
               if (r < 7)
                  continue;
               g_fwd_peer = a;
               g_have_fwd_peer = 1;
               memcpy(&from.ip_be, b, 4);
               from.port = (uint16_t)((b[4] << 8) | b[5]);
               relay_core_input(core, b[6] & 1, &from, b + 7, (size_t)r - 7, now_ms());
            }
            else
            {
               from.ip_be = a.sin_addr.s_addr;
               from.port = ntohs(a.sin_port);
               relay_core_input(core, k, &from, b, (size_t)r, now_ms());
            }
         }
      }
      if (now_ms() - last_tick >= 100)
      {
         last_tick = now_ms();
         relay_core_tick(core, last_tick);
      }
      if (now_ms() - last_stat >= 10000)
      {
         rl_core_stats s;
         char l[200];
         relay_core_stats(core, &s);
         snprintf(l, sizeof(l), "stats rx=%u tx=%u fwd=%u bytes=%u bad=%u token=%u rate=%u "
                  "rooms=%u made=%u joins=%u refused=%u punch=%u direct=%u",
                  s.rx, s.tx, s.fwd, s.fwd_bytes, s.rx_bad, s.rx_token, s.rx_rate,
                  s.rooms_now, s.rooms_made, s.joins, s.refused, s.punches_offered,
                  s.paths_direct);
         say(NULL, l);
         last_stat = now_ms();
      }
   }
}
