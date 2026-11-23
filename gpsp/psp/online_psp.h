/* online_psp.h -- EXPERIMENTAL online play on the PSP (docs/ONLINE-TUNNEL.md):
 * the link session's netdrv runs over transport_relay (one sceNetInet UDP
 * socket, infrastructure Wi-Fi) instead of transport_adhoc.  Compiled with
 * psp/Makefile ONLINE=1 (-DGBADHOC_ONLINE).  OFF unless asked for, and
 * costs nothing then: every entry point is a session start, a teardown or a
 * once-a-heartbeat stats line; nothing runs per frame.
 *
 * Two ways in:
 *   - the player: Settings > Online play = on, then Wireless > Online >
 *     Create room / Join room (ui_psp.c).  main_psp.c calls online_psp_set()
 *     for that one session.
 *   - the harness / an expert config.ini: online_relay (host or IPv4[:port]),
 *     online_room (4 letters), online_direct, online_wifi,
 *     online_drop_direct_ms -- read once at boot by online_psp_configure().
 *
 * The relay defaults to ONLINE_RELAY_DEFAULT.
 */
#ifndef ONLINE_PSP_H
#define ONLINE_PSP_H

#include "../netdrv/netdrv.h"

#define ONLINE_RELAY_DEFAULT "relay.rayquaza.ca"
/* the side lane's largest datagram: netdrv/transport_relay.h RL_SIDE_MAX (a
 * relay forwards GBR1 datagrams of up to RL_MAX_DGRAM = 600 bytes) */
#define ONLINE_SIDE_MAX 584
/* ... and its pace cap in datagrams a display frame: a relay allows a
 * console 1000 datagrams and 256 KiB a second (relay_core.c), and 6 of up to
 * 584 bytes a frame is ~210 KB/s, room left for the inputs */
#define ONLINE_SIDE_RATE 6u
#define ONLINE_SIDE_SLOTS 32u     /* the side lane's ring, datagrams */

#ifdef GBADHOC_ONLINE
/* Boot: the harness / config.ini keys.  1 = online for every session. */
int  online_psp_configure(const char *harness_ini, const char *config_ini);
/* One session from the menu: relay (NULL = config.ini online_relay or the
 * default), room ("" = host: server picks), Wi-Fi profile (1-based), the
 * host's link mode (RL_MODE_*; the joiner learns it from the server). */
void online_psp_set(const char *relay, const char *room, int wifi, int mode);
/* Back to ad hoc for the next session (a menu session ended). */
void online_psp_clear(void);
int  online_psp_enabled(void);
/* Wi-Fi up, DNS, socket, room.  Blocks up to timeout_ms.  0 = in the room. */
/* < 0 failed (online_psp_why); 0 up; 1 up, and this joiner was told slot 0:
 * "Join with code" into a desktop host's room, the first PSP in hosts. */
int  online_psp_up(int is_host, const char *build, unsigned timeout_ms);
/* The player's cancel for the next online_psp_up (the LINKING screen's O,
 * HW-1 r4-L3): polled in the Wi-Fi association and in the module's server
 * lookup and room waits; nonzero gives up with why "cancelled".  NULL
 * clears it (every other bring-up has none). */
void online_psp_cancel_hook(int (*cancel)(void));
int  online_psp_is_up(void);
const char *online_psp_room(void);     /* the 4 letters, once up */
int  online_psp_room_mode(void);       /* RL_MODE_* the server reported */
void online_psp_iface(nd_transport *out);
void online_psp_down(void);
int  online_psp_connected(void);
/* EVT online_stats (heartbeat); nothing when no online session */
void online_psp_stats_evt(const char *at);
const char *online_psp_why(void);
/* The link session's bulk lane (fe_host.h gb_bulk_enable).  Online it is
 * the module's side lane (api.side_*): bulk_lane opens it for datagrams
 * that begin with `magic` (lane != NULL) or closes it (NULL), and cuts the
 * session's lane to what a relay carries: datagrams of at most
 * ONLINE_SIDE_MAX bytes, the adaptive pace capped at ONLINE_SIDE_RATE a
 * frame (harness gblink_bulk_rate_max overrides it).  bulk_send / bulk_recv
 * take the lane that is open: the side lane online, transport_adhoc.h's
 * bulk lane ad hoc -- one call site in the frontend for both. */
struct fe_bulk_lane;
int  online_psp_bulk_lane(struct fe_bulk_lane *lane, const char magic[4]);
int  online_psp_bulk_send(const uint8_t mac[6], const void *buf, size_t len);
int  online_psp_bulk_recv(void *buf, size_t cap);
#else
#define online_psp_configure(h, c) 0
#define online_psp_set(r, c, w, m) ((void)0)
#define online_psp_clear() ((void)0)
#define online_psp_enabled() 0
#define online_psp_up(h, b, t) (-1)
#define online_psp_cancel_hook(c) ((void)0)
#define online_psp_is_up() 0
#define online_psp_room() ""
#define online_psp_room_mode() 0
#define online_psp_iface(o) ((void)0)
#define online_psp_down() ((void)0)
#define online_psp_connected() 0
#define online_psp_stats_evt(a) ((void)0)
#define online_psp_why() ""
#define online_psp_bulk_lane(l, m) (-1)
#define online_psp_bulk_send adhoc_transport_bulk_send
#define online_psp_bulk_recv adhoc_transport_bulk_recv
#endif

#endif
