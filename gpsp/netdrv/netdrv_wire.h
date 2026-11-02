/* netdrv_wire.h — on-wire frame format for netdrv (internal + tests).
 *
 * Every datagram is exactly ND_HDR_SIZE + hdr.len bytes:
 *
 *   offset size field
 *   0      4    magic   'GPN1' (0x47504E31, serialized little-endian)
 *   4      1    ver     ND_WIRE_VER (2)
 *   5      1    type    ND_T_*
 *   6      1    src     sender client_id (0-4), ND_ID_JOINING while joining
 *   7      1    dst     dest client_id (0-4), ND_ID_BCAST for broadcast,
 *                       ND_ID_JOINING when addressed to an unassigned joiner
 *   8      2    seq     ARQ sequence (DATA) / unreliable seq (UDATA)
 *   10     2    ack     cumulative ack: next seq expected FROM the addressee
 *   12     2    len     payload length (<= ND_MAX_PAYLOAD)
 *   14     2    crc     CRC16-CCITT (poly 0x1021 init 0xFFFF) over the
 *                       24-byte header with crc=0, then the payload
 *   16     4    src_nonce   sender instance, little-endian
 *   20     4    dst_nonce   receiver instance (0 for discovery/ROSTER)
 *
 * All multi-byte fields little-endian on the wire (both targets are LE;
 * serialization is still explicit byte-at-a-time — no struct punning).
 * A receiver drops any frame failing: size >= 24, magic, ver, known type,
 * len <= ND_MAX_PAYLOAD, datagram size == 24 + len (EXACT), CRC. This wall
 * is what keeps garbage away from the non-tolerant core (ADR-0003).
 */
#ifndef NETDRV_WIRE_H
#define NETDRV_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ND_WIRE_MAGIC 0x47504E31u   /* 'GPN1' */
#define ND_WIRE_VER   2

/* Frame types */
enum
{
   ND_T_DATA     = 0,   /* reliable payload (ARQ) */
   ND_T_ACK      = 1,   /* bare cumulative ack */
   ND_T_UDATA    = 2,   /* unreliable sequenced payload (drop-stale) */
   ND_T_UDATA_US = 3,   /* unreliable unsequenced payload (always deliver) */
   ND_T_JOIN     = 4,   /* joiner -> broadcast {proto[24], nick[16], nonce u32} */
   ND_T_WELCOME  = 5,   /* host -> joiner: roster payload, your_id = assigned
                           (or ND_ID_REFUSED when the session is full) */
   ND_T_ROSTER   = 6,   /* host -> all: roster payload, your_id = 0xFF */
   ND_T_BYE      = 7,   /* sender is leaving */
   ND_T_PING     = 8,   /* keepalive; ack field still meaningful */
   ND_T_OFFER    = 9,   /* host -> joiner; seq|ack<<16 is a fresh challenge */
   ND_T_CONFIRM  = 10,  /* joiner -> host; echo challenge before admission */
   ND_T_CAPS     = 11,  /* version/feature exchange at admission (below) */
   ND_T__COUNT
};

/* Special ids in src/dst */
#define ND_ID_BCAST   0xFF
#define ND_ID_JOINING 0xFE
#define ND_ID_REFUSED 0xFD   /* WELCOME.your_id: join refused */

/* Roster payload (WELCOME / ROSTER):
 *   u8 your_id; u8 gen; u8 count; count * entry
 * entry (27 bytes): u8 id; u8 mac[6]; u32 nonce; char nick[16]
 * count <= ND_MAX_CLIENTS -> payload <= 3 + 5*27 = 138 bytes. */
#define ND_ROSTER_ENTRY_SIZE 27
#define ND_ROSTER_MAX_SIZE   (3 + 5 * ND_ROSTER_ENTRY_SIZE)

/* JOIN payload: char proto[24] (NUL-padded); char nick[16]; u32 nonce */
#define ND_JOIN_SIZE (24 + 16 + 4)

/* THE ADMISSION CHECK.  Two consoles that would misread each other must not
 * get a session, and must both be able to say why.
 *
 * ND_WIRE_VER stays 2 on purpose: every frame of another version is dropped
 * unread, so a bump would make an old peer silent instead of refusable.  What
 * 4.5.0 and earlier do with the two additions, both harmless:
 *   JOIN seq/ack   ignored (always 0 until now, and no host reads them), so
 *                  JOIN carries ND_LINK_VER in seq and the features in ack,
 *                  and seq == 0 means "predates this check";
 *   ND_T_CAPS      an unknown type, dropped and counted as malformed.
 *
 * host: a JOIN with another version or features, or seq == 0, gets no OFFER.
 *       Otherwise CAPS goes out just before the OFFER.
 * joiner: CONFIRMs only after a CAPS from that host.  It compares the build as
 *       well, and answers ND_CAPS_REFUSED so both consoles can tell the player.
 *       A host sending OFFER after OFFER and no CAPS is 4.5.0 or older:
 *       ND_OLD_HOST_OFFERS of them, not one, because a CAPS can be lost and an
 *       OFFER repeats with every JOIN retry (~500 ms each).
 *
 * Bump ND_LINK_VER when what a netdrv frame means changes for every session.
 * What one kind of session turns on is a feature bit instead (netdrv.h,
 * ND_FEAT_*). */
#ifndef ND_LINK_VER
#define ND_LINK_VER 1
#endif
/* CAPS payload: u8 status; u8 0; u16 ND_LINK_VER; u16 features;
 * char build[ND_BUILD_LEN] (NUL-padded, the core's library_version). */
#define ND_BUILD_LEN       16
#define ND_CAPS_SIZE       (6 + ND_BUILD_LEN)
#define ND_CAPS_OK         0
#define ND_CAPS_REFUSED    1
#define ND_OLD_HOST_OFFERS 6

typedef struct nd_hdr
{
   uint32_t magic;
   uint8_t  ver, type, src, dst;
   uint16_t seq, ack, len, crc;
   uint32_t src_nonce, dst_nonce;
} nd_hdr;

uint16_t nd_crc16(uint16_t crc, const void *data, size_t len);

/* Serialize hdr+payload into out (>= ND_MAX_FRAME). Computes CRC.
 * Returns total frame size. */
size_t nd_wire_build(uint8_t *out, const nd_hdr *h, const void *payload);

/* Parse+validate a datagram. Returns 0 and fills h / *payload (pointing
 * into buf) on success; -1 on any validation failure (reason: 1=short,
 * 2=magic/ver, 3=type, 4=len mismatch, 5=crc) reported via *reason if
 * non-NULL. */
int nd_wire_parse(const uint8_t *buf, size_t sz, nd_hdr *h,
                  const uint8_t **payload, int *reason);

#ifdef __cplusplus
}
#endif

#endif /* NETDRV_WIRE_H */
