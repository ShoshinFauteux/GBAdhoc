/* gb_link.h — protocol-neutral coordinator for Game Boy serial byte transfers.
 *
 * This is deliberately separate from serial.c/serial_proto.c, which model
 * GBA SIOCNT and GBA RFU protocols. A GB serial core calls start() with its
 * SB byte and SC clock mode; a frontend carries gb_link_message_t over its
 * chosen reliable transport and passes received messages to receive().
 *
 * The coordinator does not own sockets, threads, emulated registers, or a
 * core. All entry points must be called on the emulation thread. A real core
 * integration still has to connect its serial callbacks to this API.
 */
#ifndef GB_LINK_H
#define GB_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_LINK_PROTOCOL_VERSION 1
#define GB_LINK_WIRE_SIZE 12

enum gb_link_clock
{
   GB_LINK_EXTERNAL_CLOCK = 0,
   GB_LINK_INTERNAL_CLOCK = 1
};

enum gb_link_message_type
{
   GB_LINK_MSG_START = 1,
   GB_LINK_MSG_COMPLETE = 2
};

enum gb_link_result
{
   GB_LINK_RESULT_OK = 0,
   GB_LINK_RESULT_TIMEOUT,
   GB_LINK_RESULT_DISCONNECTED,
   GB_LINK_RESULT_PROTOCOL
};

typedef struct gb_link_message
{
   uint8_t version;
   uint8_t type;
   uint8_t sender_id;
   uint8_t clock_mode;
   uint16_t transfer_id; /* sender-local; peer IDs need not be synchronized */
   uint8_t data;
   uint8_t reserved;
} gb_link_message_t;

typedef int (*gb_link_send_fn)(void *ctx, const gb_link_message_t *message);
typedef void (*gb_link_complete_fn)(void *ctx, uint16_t transfer_id,
                                    uint8_t received_byte,
                                    enum gb_link_result result);

typedef struct gb_link
{
   void *ctx;
   gb_link_send_fn send;
   gb_link_complete_fn complete;
   uint8_t local_id;
   uint8_t peer_id;
   uint32_t timeout_ms;
   uint8_t connected;
   uint8_t active;
   uint8_t clock_mode;
   uint8_t peer_clock_mode;
   uint8_t local_data;
   uint8_t peer_data;
   uint8_t peer_started;
   uint8_t pending_peer_start;
   uint8_t pending_peer_clock;
   uint8_t pending_peer_data;
   uint16_t transfer_id;
   uint16_t peer_transfer_id;
   uint16_t last_peer_transfer_id;
   uint16_t pending_peer_transfer_id;
   uint8_t peer_transfer_seen;
   uint32_t deadline_ms;
   uint32_t pending_deadline_ms;
} gb_link_t;

/* timeout_ms bounds the time waiting for a peer START/COMPLETE. Use a
 * monotonic millisecond clock for now_ms/tick(). */
void gb_link_init(gb_link_t *link, uint8_t local_id, uint8_t peer_id,
                  uint32_t timeout_ms, void *ctx, gb_link_send_fn send,
                  gb_link_complete_fn complete);
void gb_link_set_connected(gb_link_t *link, int connected);

/* Begin one 8-bit transfer. clock_mode is INTERNAL or EXTERNAL. The GB core
 * should keep the transfer pending until complete() is called. Returns 0 if
 * accepted, -1 if busy/disconnected/invalid. transfer_id is caller-generated
 * and returned unchanged to this endpoint's completion callback; each peer
 * may use an independent counter. The reliable ordered transport pairs the
 * one in-flight byte transaction. */
int gb_link_start(gb_link_t *link, uint16_t transfer_id, uint8_t data,
                  enum gb_link_clock clock_mode, uint32_t now_ms);

/* Deliver a validated message received from the configured peer. The caller
 * is responsible for reliable, ordered transport and sender validation. */
void gb_link_receive(gb_link_t *link, const gb_link_message_t *message,
                    uint32_t now_ms);

/* Advance timeouts. Safe to call every emulated frame. */
void gb_link_tick(gb_link_t *link, uint32_t now_ms);

/* Canonical 12-byte wire format: magic "GBL1", version, type, sender,
 * clock mode, transfer ID little-endian, byte value, reserved zero. No C
 * structs are sent raw, so ABI padding/endian differences cannot leak. */
int gb_link_encode(const gb_link_message_t *message,
                   uint8_t output[GB_LINK_WIRE_SIZE]);
int gb_link_decode(gb_link_message_t *message,
                   const uint8_t *input, unsigned length);

#ifdef __cplusplus
}
#endif
#endif
