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
int  online_psp_up(int is_host, const char *build, unsigned timeout_ms);
int  online_psp_is_up(void);
const char *online_psp_room(void);     /* the 4 letters, once up */
int  online_psp_room_mode(void);       /* RL_MODE_* the server reported */
void online_psp_iface(nd_transport *out);
void online_psp_down(void);
int  online_psp_connected(void);
/* EVT online_stats (heartbeat); nothing when no online session */
void online_psp_stats_evt(const char *at);
const char *online_psp_why(void);
/* The PSP's saved connections, for the Online screen's Network row. */
int  online_psp_wifi_count(void);
int  online_psp_wifi_name(int index, char *out, unsigned cap);
#else
#define online_psp_configure(h, c) 0
#define online_psp_set(r, c, w, m) ((void)0)
#define online_psp_clear() ((void)0)
#define online_psp_enabled() 0
#define online_psp_up(h, b, t) (-1)
#define online_psp_is_up() 0
#define online_psp_room() ""
#define online_psp_room_mode() 0
#define online_psp_iface(o) ((void)0)
#define online_psp_down() ((void)0)
#define online_psp_connected() 0
#define online_psp_stats_evt(a) ((void)0)
#define online_psp_why() ""
#define online_psp_wifi_count() 0
#define online_psp_wifi_name(i, o, c) (-1)
#endif

#endif
