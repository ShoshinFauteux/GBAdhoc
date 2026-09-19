/* mgift_net.h — infrastructure-Wi-Fi bring-up for Mystery Gift.
 *
 * WHY THIS IS A SEPARATE STACK FROM THE TRADING LINK.  GBAdhoc's shipped
 * wireless feature is sceNetAdhoc PDP: IBSS (ad-hoc) mode, MAC-addressed,
 * port 0x4A4B, driven by netdrv/transport_adhoc.c.  The Mystery Gift Station is
 * an Android phone, and stock Android cannot create an IBSS -- so the phone is
 * the ACCESS POINT and the PSP associates with it as a station.  That is
 * infrastructure mode: a different radio mode, a different SDK (sceNetApctl +
 * sceNetInet rather than sceNetAdhocctl + PDP), and it cannot be up at the same
 * time as ad-hoc.
 *
 * Owns AP association, the temporary PSP network profile and the MGC2 UDP
 * parcel socket. The phone uploads card plus game-specific redemption scripts;
 * mgift_cart.c then speaks the game's wireless protocol locally.
 * Infrastructure Mystery Gift and ad-hoc trading remain mutually exclusive.
 */
#ifndef MGIFT_NET_H
#define MGIFT_NET_H

#include <stdint.h>

/* Ports.  The phone listens on 5394 and we listen on 21064 -- inherited from
 * the app's original design and kept so its existing socket setup still fits. */
#define MGNET_LISTEN_PORT   21064
#define MGNET_STATION_PORT  5394
#define MGNET_AUTO_SSID "Mystery Gift"

enum
{
   MGNET_OK                  = 0,
   MGNET_ERR_BUSY            = -1,  /* a trading session is up (interlock) */
   MGNET_ERR_WLAN_OFF        = -2,  /* hardware WLAN switch is off */
   MGNET_ERR_MODULES         = -3,  /* sceUtilityLoadNetModule failed */
   MGNET_ERR_NET_INIT        = -4,  /* sceNetInit failed */
   MGNET_ERR_INET_INIT       = -5,  /* sceNetInetInit failed */
   MGNET_ERR_APCTL_INIT      = -6,  /* sceNetApctlInit failed */
   MGNET_ERR_NO_CONFIG       = -7,  /* no stored network profile to connect */
   MGNET_ERR_CONNECT         = -8,  /* sceNetApctlConnect failed */
   MGNET_ERR_CONNECT_TIMEOUT = -9,  /* never reached GOT_IP */
   MGNET_ERR_ALREADY         = -12  /* already up */
};

/* Associate with a stored network profile. `conf` is 1-based;
 * 0 selects/creates the open MGNET_AUTO_SSID profile. `timeout_us`
 * 0 = default 30 s.  Blocks the caller while associating, exactly as
 * adhoc_transport_init does.  On any failure the partial init is unwound. */
int  mgnet_wifi_up(int conf, unsigned timeout_us);

/* Reverse teardown.  Idempotent, and correct after a partial/failed up(). */
void mgnet_wifi_down(void);

int  mgnet_wifi_is_up(void);

/* Diagnosis for a build with no log: which rung of the ladder we reached or
 * failed at, and the SCE error that did it. */
const char *mgnet_stage(void);
unsigned    mgnet_last_sce(void);

/* "192.168.43.17", or "" if apctl has not handed us one. */
const char *mgnet_local_ip(void);

/* ---- MGC2 parcel carrier ------------------------------------------------
 * One bounded datagram carries a 332-byte Wonder Card and relocatable scripts.
 * Layout and limits: mgift_payload.h and docs/MYSTERY-GIFT-DELIVERY.md.
 * MGA2 replies echo the parcel ID and say accepted, busy or invalid. Acceptance
 * means the emulator queued the parcel; the game still receives and saves it. */
#include "mgift_payload.h"

/* Open/close the listening socket.  The network must already be up. */
int  mgnet_card_open(void);
void mgnet_card_close(void);

/* Non-blocking.  Returns 1 and fills `out` when a valid parcel arrives; 0 otherwise.
 * Caller must reply after mgift_cart_set_parcel decides acceptance. Invalid
 * MGC2 parcels receive an invalid response directly. */
int  mgnet_card_poll(mgift_parcel *out);
void mgnet_card_reply(uint32_t id, unsigned result);

/* ---- the temporary connection profile -------------------------------------
 *
 * WHY A PROFILE HAS TO EXIST AT ALL.  sceNetApctlConnect() takes a stored
 * profile INDEX -- there is no call that takes an SSID and a key.  So something
 * must be in a slot before we can associate, even though we already know both.
 *
 * Automatic reuses any saved profile matching the open SSID, DHCP, automatic
 * DNS and no proxy. Otherwise it creates a free slot named GBAdhoc Mystery Gift.
 * A successful profile stays for the next session. Existing connections are
 * never overwritten or deleted; a newly created profile is removed on failure.
 *
 * Note the odd shape of that API: sceUtilitySetNetParam writes to configuration
 * ZERO, a scratch slot, and sceUtilityCreateNetParam CLEARS zero as a side
 * effect.  So the order has to be create the slot, then fill in zero, then copy
 * zero over the slot.  Doing it in the obvious order silently writes nothing.
 *
 * The display name and SSID are separate: matching is by credentials, not name. */
#define MGNET_PROFILE_NAME "GBAdhoc Mystery Gift"

/* Select/create an open profile after APCTL init. Key must be empty or NULL.
 * Return its 1-based slot, or <0 on error. */
int  mgnet_profile_ensure(const char *ssid, const char *key);

/* Failure cleanup: delete only a profile created by the current attempt. */
void mgnet_profile_release(void);

/* Stored network profiles, for the picker.  index is 1-based. */
int  mgnet_config_count(void);
int  mgnet_config_name(int index, char *out, unsigned out_sz);

#endif /* MGIFT_NET_H */
