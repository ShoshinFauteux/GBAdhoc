/* relay_dirclient.h -- a desktop host's half of the directory: register this
 * host's four-letter code with the shared server (RL_T_REGISTER), keep it
 * alive, and take it back on stop.  docs/ONLINE-TUNNEL.md, "Desktop hosts and
 * discovery".
 *
 * Portable and passive like relay_core: no sockets, no threads, no
 * allocation.  The host sends from the relay's own main socket (so the
 * directory sees the router's mapping of that socket), offers every datagram
 * that came from the directory to rl_dirc_input before the core sees it, and
 * calls rl_dirc_tick every ~100 ms.  The code the directory confirmed is the
 * one the host gives its relay core as its home code (relay_core_set_home).
 */
#ifndef RELAY_DIRCLIENT_H
#define RELAY_DIRCLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "relay_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { DC_OFF = 0, DC_REGISTERING, DC_REGISTERED, DC_FAILED };

typedef struct rl_dirc_cfg
{
   uint32_t dir_ip_be;          /* the shared server */
   uint16_t dir_port;           /* host order */
   uint16_t adv_port;           /* the port PSPs should use (the router's mapping); 0 = as seen */
   uint32_t priv_ip_be;         /* this PC's LAN address, for PSPs in the same home */
   uint16_t priv_port;
   char want[RL_CODE_LEN + 1];  /* the code to ask for ("" = the server picks) */
   void (*send)(void *user, uint32_t ip_be, uint16_t port, const void *buf, size_t len);
   void (*log)(void *user, const char *line);   /* optional */
   void *user;
} rl_dirc_cfg;

typedef struct rl_dirc
{
   rl_dirc_cfg cfg;
   int state;
   char code[RL_CODE_LEN + 1];  /* confirmed ("" until DC_REGISTERED once) */
   uint64_t token, cookie;
   uint64_t next_ms, last_ok_ms;
   unsigned tries;
   int err;                     /* RL_E_* of the last refusal */
   char errtext[32];
   uint32_t tx_bytes, rx_bytes, registers, refreshes;
   int code_changed;            /* set when code changes; the host clears it */
} rl_dirc;

void rl_dirc_start(rl_dirc *d, const rl_dirc_cfg *cfg, uint64_t now_ms);
/* 1 = the datagram was the directory's (consumed), 0 = not ours */
int  rl_dirc_input(rl_dirc *d, uint32_t ip_be, uint16_t port, const void *buf,
                   size_t len, uint64_t now_ms);
void rl_dirc_tick(rl_dirc *d, uint64_t now_ms);
/* UNREGISTER (once, best effort) and off */
void rl_dirc_stop(rl_dirc *d);

#ifdef __cplusplus
}
#endif

#endif
