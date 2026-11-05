/* online_psp.h -- EXPERIMENTAL online play on the PSP (docs/ONLINE-TUNNEL.md):
 * the link session's netdrv runs over transport_relay (one sceNetInet UDP
 * socket, infrastructure Wi-Fi) instead of transport_adhoc.  Compiled only
 * with psp/Makefile ONLINE=1 (-DGBADHOC_ONLINE); active only when a harness
 * or config.ini key names a relay:
 *
 *   online_relay = 10.0.0.40:27420   the GBR1 server (IPv4:port; required)
 *   online_room  = ABCD              the room: the host asks for it, the guest
 *                                    joins it ("" = host: server picks)
 *   online_direct = 1                try the hole-punched direct path (0 =
 *                                    relay only)
 *   online_wifi  = 1                 the PSP's saved network connection (1-based)
 *   online_drop_direct_ms = 0        harness: drop direct traffic after N ms
 *                                    (forces the relay fallback)
 *
 * The host/join role, the session and everything above netdrv are the ad hoc
 * path's, unchanged.
 */
#ifndef ONLINE_PSP_H
#define ONLINE_PSP_H

#include "../netdrv/netdrv.h"

#ifdef GBADHOC_ONLINE
/* Read the keys (harness ini first, then config.ini).  Returns 1 if online
 * play is configured for this boot. */
int  online_psp_configure(const char *harness_ini, const char *config_ini);
int  online_psp_enabled(void);
/* Wi-Fi up, socket, room.  Blocks up to timeout_ms.  0 = in the room. */
int  online_psp_up(int is_host, const char *build, unsigned timeout_ms);
void online_psp_iface(nd_transport *out);
void online_psp_down(void);
int  online_psp_connected(void);
/* EVT online_stats (heartbeat) */
void online_psp_stats_evt(const char *at);
const char *online_psp_why(void);
#else
#define online_psp_configure(h, c) 0
#define online_psp_enabled() 0
#define online_psp_up(h, b, t) (-1)
#define online_psp_iface(o) ((void)0)
#define online_psp_down() ((void)0)
#define online_psp_connected() 0
#define online_psp_stats_evt(a) ((void)0)
#define online_psp_why() ""
#endif

#endif
