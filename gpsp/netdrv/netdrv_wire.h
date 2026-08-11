/* netdrv_wire.h — on-wire frame format for netdrv (internal + tests).
 *
 * Every datagram is exactly ND_HDR_SIZE + hdr.len bytes:
 *
 *   offset size field
 *   0      4    magic   'GPN1' (0x47504E31, serialized little-endian)
 *   4      1    ver     ND_WIRE_VER (1)
 *   5      1    type    ND_T_*
 *   6      1    src     sender client_id (0-4), ND_ID_JOINING while joining
 *   7      1    dst     dest client_id (0-4), ND_ID_BCAST for broadcast,
 *                       ND_ID_JOINING when addressed to an unassigned joiner
 *   8      2    seq     ARQ sequence (DATA) / unreliable seq (UDATA)
 *   10     2    ack     cumulative ack: next seq expected FROM the addressee
 *   12     2    len     payload length (<= ND_MAX_PAYLOAD)
 *   14     2    crc     CRC16-CCITT (poly 0x1021 init 0xFFFF) over the
 *                       16-byte header with crc=0, then the payload
 *
 * All multi-byte fields little-endian on the wire (both targets are LE;
 * serialization is still explicit byte-at-a-time — no struct punning).
 * A receiver drops any frame failing: size >= 16, magic, ver, known type,
 * len <= ND_MAX_PAYLOAD, datagram size == 16 + len (EXACT), CRC. This wall
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
#define ND_WIRE_VER   1

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

typedef struct nd_hdr
{
   uint32_t magic;
   uint8_t  ver, type, src, dst;
   uint16_t seq, ack, len, crc;
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
