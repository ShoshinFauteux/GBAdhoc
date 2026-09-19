/* mgift_cart.h — the emulated Pokémon distribution cart.
 *
 * WHAT THIS IS.  A Wonder Card distribution cart was a Game Boy Advance running
 * special software, talking to the player's cart over the AGB-015 Wireless
 * Adapter.  We cannot put that radio in a phone, so this plays the cart: it
 * speaks the game's own wireless protocol to the emulated FireRed, and the only
 * thing translated is the radio.  FireRed opens its own MYSTERY GIFT menu,
 * receives a real 332-byte Wonder Card, and saves it itself -- we never touch
 * the save file.
 *
 * The card itself arrives from the phone over Wi-Fi (mgift_net.c + the UDP
 * carrier), so the gift genuinely travels over the air.  What lives HERE is the
 * lock-step, frame-paced part that the Motorola radio chip and the cart's ROM
 * did between them, and it lives on this side for one reason: it is testable.
 * In C, next to the emulator, it runs against PPSSPP and a real ROM in minutes
 * per iteration; on the phone every attempt would need a human holding two
 * devices, and the PSP cannot report why the game rejected a frame.
 *
 * HOW IT REACHES THE GAME.  Through the seam the core already exposes,
 * fe_host_netpacket_cb() -- the same interface netdrv uses for a real second
 * console.  We call the core's start() as client_id 1 and inject the cart's
 * frames with receive(..., 0), so rfu.c sees an ordinary peer.  No netdrv, no
 * sockets, and NO NEW THREADS on this path.
 *
 * ISOLATION FROM THE TRADING LINK.  Nothing here includes, links against or
 * alters netdrv/, transport_adhoc.c or rfu.c.  It is a separate netpacket
 * driver that only exists while the player is inside Wireless -> Mystery Gift,
 * and the frontend interlocks the two modes as mutually exclusive.
 *
 * THE STACK, and where each layer is specified:
 *   L4  Mystery Gift server script     pokefirered/src/mystery_gift_server.c
 *   L3  MysteryGiftLink framing        pokefirered/src/mystery_gift_link.c
 *   L2.5 command / block protocol      pokefirered/src/link_rfu_2.c
 *   L2  RFU1 peer frames               gpsp/rfu.c:930
 *   L1  libretro netpacket             fe_host_netpacket_cb()
 */
#ifndef MGIFT_CART_H
#define MGIFT_CART_H

#include <stdint.h>
#include "mgift_payload.h"

/* Bring the cart up: attach to the core's netpacket interface. Advertising
 * starts after a parcel is loaded. Returns 0 on success, <0 if no netpacket
 * interface (which would mean the ROM is not using the serial/RFU path). */
int  mgift_cart_start(void);

/* Detach and stop advertising.  Idempotent. */
void mgift_cart_stop(void);

int  mgift_cart_active(void);

/* Once per frame, from the main loop.  Drives the advertise/handshake/transfer
 * state machine.  Cheap and non-blocking. */
void mgift_cart_frame(void);

/* Copy a parcel validated by mgift_parcel_decode: card plus FRLG/Emerald
 * redemption scripts. Return 0 accepted (including identical retries),
 * 1 busy with another gift, -1 invalid input. */
int  mgift_cart_set_parcel(const mgift_parcel *parcel);
int  mgift_cart_has_card(void);

/* One line of status for the Mystery Gift screen, e.g. "advertising" or
 * "sending card 12/28".  Never NULL. */
const char *mgift_cart_status(void);

/* Diagnostics for a build with no log. */
typedef struct
{
   uint32_t bcasts;        /* broadcasts sent */
   uint32_t connects;      /* CONNECT_REQ seen */
   uint32_t cmd_tx;        /* command slots sent to the game */
   uint32_t cmd_rx;        /* command slots received from the game */
   uint32_t blocks_tx;     /* link blocks fully sent */
   uint32_t blocks_rx;     /* link blocks fully received */
   uint32_t crc_fail;      /* received blocks whose CRC did not match */
   int      linked;        /* the game connected to us */
   int      svr_state;     /* server script position */
} mgift_cart_stats;

void mgift_cart_get_stats(mgift_cart_stats *out);

#endif /* MGIFT_CART_H */
