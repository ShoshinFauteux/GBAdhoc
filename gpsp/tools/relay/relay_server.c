/* relay_server.c -- gbadhoc-relay: the GBR1 relay / rendezvous server
 * (netdrv/relay_core.c) on POSIX UDP sockets.  docs/ONLINE-TUNNEL.md 6.
 *
 *   gbadhoc-relay [--bind IP] [--port P] [--no-direct] [--quiet]
 *       listens on P (default 27420) and P + 1 (the PROBE port)
 *   gbadhoc-relay ... [--status SPORT] [--pidfile FILE]
 *       --status: any datagram to 127.0.0.1:SPORT is answered with one line of
 *       counters (monitoring; loopback only, never the public interface).
 *       --pidfile: this process's PID, for a stop script.
 *   gbadhoc-relay --fwd PORT [--port P]
 *       FORWARDED mode, for a host the consoles cannot reach directly (WSL2
 *       behind Windows' NAT): one socket on PORT receives datagrams framed
 *       by tools/relay/winfwd.py as  ip[4] port[2] sock[1] payload  and the
 *       replies go back framed the same way, so the core still sees every
 *       console's real address.
 *
 *   gbadhoc-relay ... [--name DNS-NAME] [--public-port P] [--no-discover]
 *       HOME RELAY DISCOVERY: a console on this server's own LAN asks the
 *       LAN broadcast address first (RL_T_DISCOVER) and is told the name and
 *       public address this server serves, so it can use the LAN address
 *       when the router does no NAT hairpin.  On whenever the server knows
 *       who it is (--name and/or --public-ip); --name without --public-ip
 *       resolves the name for the public IP (again every 10 minutes: a home
 *       IP changes).  --public-port: the router's public port for the main
 *       port, if it is not the same.
 *
 * Prints one line per event and a counter line every 10 s.  Never binds a
 * public address unless --bind says so: the default is 127.0.0.1.
 */
#include "relay_core.h"
#include "relay_dirclient.h"
#include "invite_code.h"

#include <arpa/inet.h>
#include <netdb.h>
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
static int g_status = -1;
/* desktop-host mode (--home / --register): GBAdhoc XMB's relay, for natlab */
static rl_dirc g_dc;
static int g_dc_on;

/* --name's IPv4, 0 = none or private (a split-horizon DNS answer is no
 * public address).  Blocking, but only at start and every 10 minutes. */
static uint32_t resolve_public(const char *name)
{
   struct addrinfo hints, *res = NULL;
   uint32_t ip = 0;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_DGRAM;
   if (getaddrinfo(name, NULL, &hints, &res) == 0 && res)
   {
      ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
      if (rl_ip_private(ip))
         ip = 0;
   }
   if (res)
      freeaddrinfo(res);
   return ip;
}

static void dc_send(void *u, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   struct sockaddr_in a;
   (void)u;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip;
   a.sin_port = htons(port);
   sendto(g_fd[0], buf, len, 0, (struct sockaddr *)&a, sizeof(a));
}

static uint64_t g_t0;
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
   int port = RL_DEFAULT_PORT, fwd_port = 0, status_port = 0, i;
   const char *pidfile = NULL, *home = NULL, *reg = NULL, *pub = NULL, *lan = NULL;
   const char *pubip = NULL, *name = NULL;
   int adv_port = 0, no_discover = 0, pub_port = 0;
   uint64_t last_resolve = 0;
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
      else if (!strcmp(argv[i], "--no-directory")) cfg.directory = -1;
      else if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
      else if (!strcmp(argv[i], "--register") && i + 1 < argc) reg = argv[++i];
      else if (!strcmp(argv[i], "--advertise-port") && i + 1 < argc) adv_port = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--public") && i + 1 < argc) pub = argv[++i];
      else if (!strcmp(argv[i], "--public-ip") && i + 1 < argc) pubip = argv[++i];
      else if (!strcmp(argv[i], "--lan-ip") && i + 1 < argc) lan = argv[++i];
      else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
      else if (!strcmp(argv[i], "--public-port") && i + 1 < argc) pub_port = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--no-discover")) no_discover = 1;
      else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
      else if (!strcmp(argv[i], "--status") && i + 1 < argc) status_port = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidfile = argv[++i];
      else
      {
         fprintf(stderr, "usage: gbadhoc-relay [--bind IP] [--port P] [--fwd PORT] [--no-direct] [--quiet]\n"
                         "  [--no-directory]   do not keep the desktop-host directory (on by default)\n"
                         "  desktop-host mode (GBAdhoc XMB's relay): [--home CODE] [--register DIR_IP[:PORT]]\n"
                         "      [--advertise-port P] [--lan-ip IP] [--public IP (prints the invite code)]\n"
                         "  [--public-ip IP]   this server's public IPv4 (default: --public): consoles\n"
                         "                     on the server's own LAN are offered to remote peers there\n"
                         "  [--name DNS-NAME]  the name consoles are configured with (home relay\n"
                         "                     discovery; also the public IP when --public-ip is absent)\n"
                         "  [--public-port P]  the router's public port for --port, if different\n"
                         "  [--no-discover]    never answer a LAN console's DISCOVER\n");
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
   if (status_port)
   {
      g_status = open_udp("127.0.0.1", status_port);
      if (g_status < 0)
         return 1;
   }
   if (pidfile)
   {
      FILE *f = fopen(pidfile, "w");
      if (f)
      {
         fprintf(f, "%d\n", (int)getpid());
         fclose(f);
      }
   }
   if (getuid() == 0)
      fprintf(stderr, "warning: running as root; run the relay as an unprivileged user\n");
   cfg.send = do_send;
   cfg.random64 = rnd64;
   cfg.log = say;
   /* the shared server keeps the directory; a desktop host does not */
   cfg.directory = cfg.directory < 0 || home || reg ? 0 : 1;
   if (pubip || pub)
   {
      uint32_t ip = inet_addr(pubip ? pubip : pub);
      if (ip == INADDR_NONE || rl_ip_private(ip))
         fprintf(stderr, "warning: --public-ip %s is not a public IPv4; ignored\n",
                 pubip ? pubip : pub);
      else
         cfg.public_ip_be = ip;
   }
   if (name && !cfg.public_ip_be)
   {
      cfg.public_ip_be = resolve_public(name);
      last_resolve = now_ms();
   }
   cfg.served_name = name;
   cfg.public_port = (uint16_t)(pub_port > 0 && pub_port < 65535 ? pub_port : 0);
   cfg.discover = !no_discover && (name || cfg.public_ip_be);
   core = relay_core_create(&cfg);
   if (!core)
      return 1;
   if (name && strlen(name) > RL_NAME_MAX)
      fprintf(stderr, "warning: --name is longer than %d characters; not served\n", RL_NAME_MAX);
   if (cfg.discover)
   {
      const uint8_t *q = (const uint8_t *)&cfg.public_ip_be;
      printf("discover on: name=%s public=%u.%u.%u.%u:%d\n", name ? name : "-",
             q[0], q[1], q[2], q[3], cfg.public_port ? cfg.public_port : 0);
   }
   if (home)
      relay_core_set_home(core, home);
   if (reg && !fwd_port)
   {
      rl_dirc_cfg dc;
      char h[64], *c;
      memset(&dc, 0, sizeof(dc));
      snprintf(h, sizeof(h), "%s", reg);
      c = strchr(h, ':');
      dc.dir_port = RL_DEFAULT_PORT;
      if (c)
      {
         *c = 0;
         dc.dir_port = (uint16_t)atoi(c + 1);
      }
      dc.dir_ip_be = inet_addr(h);
      dc.adv_port = (uint16_t)(adv_port ? adv_port : port);
      dc.priv_ip_be = inet_addr(lan ? lan : bind_ip);
      dc.priv_port = (uint16_t)port;
      if (home)
         snprintf(dc.want, sizeof(dc.want), "%.4s", home);
      dc.send = dc_send;
      dc.log = say;
      rl_dirc_start(&g_dc, &dc, now_ms());
      g_dc_on = 1;
   }
   if (pub)
   {
      uint32_t ip = inet_addr(pub);
      char inv[16];
      if (inv_encode((const uint8_t *)&ip, (unsigned)(adv_port ? adv_port : port), 1, inv) == INV_OK)
         printf("invite %s\n", inv);
      else
         printf("invite none (port %d has no invite code)\n", adv_port ? adv_port : port);
   }
   g_t0 = now_ms();
   printf("gbadhoc-relay up %s port=%d%s%s\n", bind_ip, fwd_port ? fwd_port : port,
          fwd_port ? " (forwarded)" : "", cfg.no_direct ? " no-direct" : "");
   fflush(stdout);
   for (;;)
   {
      struct pollfd pf[3];
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
      if (g_status >= 0)
      {
         pf[n].fd = g_status; pf[n].events = POLLIN; n++;
      }
      poll(pf, (nfds_t)n, 50);
      if (g_status >= 0 && (pf[n - 1].revents & POLLIN))
      {
         uint8_t q[64];
         struct sockaddr_in a;
         socklen_t al = sizeof(a);
         while (recvfrom(g_status, q, sizeof(q), 0, (struct sockaddr *)&a, &al) >= 0)
         {
            rl_core_stats st;
            char l[400];
            int m;
            relay_core_stats(core, &st);
            m = snprintf(l, sizeof(l), "gbadhoc-relay ok rooms=%u made=%u joins=%u refused=%u "
                         "rx=%u tx=%u fwd=%u fwd_bytes=%u bad=%u token=%u rate=%u bytes_rate=%u "
                         "cookies=%u cookie_bad=%u punch=%u direct=%u uptime_s=%llu\n",
                         st.rooms_now, st.rooms_made, st.joins, st.refused, st.rx, st.tx, st.fwd,
                         st.fwd_bytes, st.rx_bad, st.rx_token, st.rx_rate, st.rx_bytes_rate,
                         st.cookies_sent, st.cookie_bad, st.punches_offered, st.paths_direct,
                         (unsigned long long)((now_ms() - g_t0) / 1000));
            sendto(g_status, l, (size_t)m, 0, (struct sockaddr *)&a, al);
            al = sizeof(a);
         }
         n--;
      }
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
               if (g_dc_on && k == 0 &&
                   rl_dirc_input(&g_dc, from.ip_be, from.port, b, (size_t)r, now_ms()))
               {
                  if (g_dc.code_changed)
                  {
                     g_dc.code_changed = 0;
                     relay_core_set_home(core, g_dc.code);
                     printf("home code=%s registered\n", g_dc.code);
                     fflush(stdout);
                  }
                  continue;
               }
               relay_core_input(core, k, &from, b, (size_t)r, now_ms());
            }
         }
      }
      if (now_ms() - last_tick >= 100)
      {
         last_tick = now_ms();
         relay_core_tick(core, last_tick);
         if (g_dc_on)
            rl_dirc_tick(&g_dc, last_tick);
      }
      if (name && !pubip && !pub && now_ms() - last_resolve >= 600000)
      {
         uint32_t ip = resolve_public(name);
         last_resolve = now_ms();
         if (ip && ip != cfg.public_ip_be)
         {
            const uint8_t *q = (const uint8_t *)&ip;
            cfg.public_ip_be = ip;
            relay_core_set_public_ip(core, ip);
            printf("public ip %u.%u.%u.%u (from %s)\n", q[0], q[1], q[2], q[3], name);
            fflush(stdout);
         }
      }
      if (now_ms() - last_stat >= 10000)
      {
         rl_core_stats s;
         char l[288];
         relay_core_stats(core, &s);
         snprintf(l, sizeof(l), "stats rx=%u tx=%u fwd=%u bytes=%u bad=%u token=%u rate=%u "
                  "rooms=%u made=%u joins=%u refused=%u punch=%u direct=%u dir=%u lookups=%u located=%u dir_bytes=%u "
                  "discovers=%u discovered=%u",
                  s.rx, s.tx, s.fwd, s.fwd_bytes, s.rx_bad, s.rx_token, s.rx_rate,
                  s.rooms_now, s.rooms_made, s.joins, s.refused, s.punches_offered,
                  s.paths_direct, s.dir_now, s.lookups, s.located, s.dir_rx_bytes + s.dir_tx_bytes,
                  s.discovers, s.discovered);
         say(NULL, l);
         last_stat = now_ms();
      }
   }
}
