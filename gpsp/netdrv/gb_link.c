/* gb_link.c — asynchronous two-endpoint Game Boy byte-cable coordinator. */
#include "gb_link.h"

#include <string.h>

static int gb_link_expired(uint32_t now, uint32_t deadline)
{
   return (int32_t)(now - deadline) >= 0;
}

/* Transfer IDs are sender-local sequence numbers. Compare only IDs from the
 * same peer, using serial-number arithmetic so wraparound remains valid. */
static int gb_link_peer_id_is_newer(const gb_link_t *link, uint16_t id)
{
   return !link->peer_transfer_seen ||
          (int16_t)(id - link->last_peer_transfer_id) > 0;
}

static void gb_link_finish(gb_link_t *link, uint8_t received,
                           enum gb_link_result result)
{
   uint16_t transfer_id;
   if (!link->active)
      return;
   transfer_id = link->transfer_id;
   link->active = 0;
   link->peer_started = 0;
   if (link->complete)
      link->complete(link->ctx, transfer_id, received, result);
}

static int gb_link_valid_message(const gb_link_message_t *message)
{
   if (!message || message->version != GB_LINK_PROTOCOL_VERSION ||
       message->reserved != 0)
      return 0;
   if (message->type == GB_LINK_MSG_START)
      return message->clock_mode == GB_LINK_INTERNAL_CLOCK ||
             message->clock_mode == GB_LINK_EXTERNAL_CLOCK;
   if (message->type == GB_LINK_MSG_COMPLETE)
      return message->clock_mode == 0;
   return 0;
}

static int gb_link_send_start(gb_link_t *link)
{
   gb_link_message_t message;
   memset(&message, 0, sizeof(message));
   message.version = GB_LINK_PROTOCOL_VERSION;
   message.type = GB_LINK_MSG_START;
   message.sender_id = link->local_id;
   message.clock_mode = link->clock_mode;
   message.transfer_id = link->transfer_id;
   message.data = link->local_data;
   return link->send(link->ctx, &message);
}

static int gb_link_send_complete(gb_link_t *link)
{
   gb_link_message_t message;
   memset(&message, 0, sizeof(message));
   message.version = GB_LINK_PROTOCOL_VERSION;
   message.type = GB_LINK_MSG_COMPLETE;
   message.sender_id = link->local_id;
   message.transfer_id = link->transfer_id;
   message.data = link->local_data; /* clock master's byte */
   return link->send(link->ctx, &message);
}

static void gb_link_handle_peer_start(gb_link_t *link, uint8_t peer_clock,
                                      uint16_t peer_transfer_id,
                                      uint8_t peer_data, uint32_t now_ms);

void gb_link_init(gb_link_t *link, uint8_t local_id, uint8_t peer_id,
                  uint32_t timeout_ms, void *ctx, gb_link_send_fn send,
                  gb_link_complete_fn complete)
{
   if (!link)
      return;
   memset(link, 0, sizeof(*link));
   link->ctx = ctx;
   link->send = send;
   link->complete = complete;
   link->local_id = local_id;
   link->peer_id = peer_id;
   link->timeout_ms = timeout_ms ? timeout_ms : 1000;
   if (link->timeout_ms > 0x7FFFFFFFu)
      link->timeout_ms = 0x7FFFFFFFu;
}

void gb_link_set_connected(gb_link_t *link, int connected)
{
   if (!link)
      return;
   link->connected = connected ? 1 : 0;
   if (!link->connected)
   {
      link->pending_peer_start = 0;
      gb_link_finish(link, 0xFF, GB_LINK_RESULT_DISCONNECTED);
   }
}

int gb_link_start(gb_link_t *link, uint16_t transfer_id, uint8_t data,
                  enum gb_link_clock clock_mode, uint32_t now_ms)
{
   if (!link || !link->connected || !link->send || link->active ||
       link->local_id == link->peer_id ||
       (clock_mode != GB_LINK_INTERNAL_CLOCK &&
        clock_mode != GB_LINK_EXTERNAL_CLOCK))
      return -1;

   link->active = 1;
   link->clock_mode = (uint8_t)clock_mode;
   link->local_data = data;
   link->peer_data = 0xFF;
   link->peer_started = 0;
   link->transfer_id = transfer_id;
   link->deadline_ms = now_ms + link->timeout_ms;
   if (gb_link_send_start(link) < 0)
   {
      link->active = 0;
      return -1;
   }
   if (link->pending_peer_start)
   {
      uint8_t peer_clock = link->pending_peer_clock;
      uint8_t peer_data = link->pending_peer_data;
      uint16_t peer_transfer_id = link->pending_peer_transfer_id;
      uint32_t pending_deadline = link->pending_deadline_ms;
      link->pending_peer_start = 0;
      if (!gb_link_expired(now_ms, pending_deadline))
         gb_link_handle_peer_start(link, peer_clock, peer_transfer_id,
                                   peer_data, now_ms);
   }
   return 0;
}

static void gb_link_handle_peer_start(gb_link_t *link, uint8_t peer_clock,
                                      uint16_t peer_transfer_id,
                                      uint8_t peer_data, uint32_t now_ms)
{
   link->peer_started = 1;
   link->peer_clock_mode = peer_clock;
   link->peer_transfer_id = peer_transfer_id;
   link->peer_data = peer_data;

   /* A single internal-clock endpoint supplies the clock. If both request
    * it, stable endpoint ordering elects exactly one master. Two external
    * clock endpoints have no source and will time out. */
   if (link->clock_mode == GB_LINK_INTERNAL_CLOCK &&
       (peer_clock == GB_LINK_EXTERNAL_CLOCK || link->local_id < link->peer_id))
   {
      if (gb_link_send_complete(link) < 0)
      {
         gb_link_finish(link, 0xFF, GB_LINK_RESULT_DISCONNECTED);
         return;
      }
      gb_link_finish(link, link->peer_data, GB_LINK_RESULT_OK);
   }
   link->deadline_ms = now_ms + link->timeout_ms;
}

void gb_link_receive(gb_link_t *link, const gb_link_message_t *message,
                    uint32_t now_ms)
{
   if (!link || !gb_link_valid_message(message) || !link->connected ||
       message->sender_id != link->peer_id)
      return;

   if (message->type == GB_LINK_MSG_START)
   {
      /* Each endpoint has at most one byte in flight and sends a new START
       * only after its previous byte finished, so a NEWER START proves the
       * peer abandoned (timed out) whatever START we still hold. Older or
       * equal IDs are retransmissions/stale and are ignored. */
      if (!gb_link_peer_id_is_newer(link, message->transfer_id))
         return;
      if (!link->active)
      {
         /* The local SC write can come after the peer's reliable START;
          * retain the newest early START so the next local start can pair
          * with it. Keeping an older one instead pairs a byte the peer has
          * already given up on, shifting the stream by one byte. */
         link->pending_peer_start = 1;
         link->pending_peer_clock = message->clock_mode;
         link->pending_peer_data = message->data;
         link->pending_peer_transfer_id = message->transfer_id;
         link->pending_deadline_ms = now_ms + link->timeout_ms;
         link->last_peer_transfer_id = message->transfer_id;
         link->peer_transfer_seen = 1;
         return;
      }
      /* Active, and possibly already paired with an older peer START: that
       * byte will never be COMPLETEd (the peer moved on), so re-pair with
       * this one. Dropping it instead left this side waiting for a COMPLETE
       * carrying the old ID while the peer's COMPLETE named the new one --
       * a full timeout here and a byte the two sides disagree on. */
      link->last_peer_transfer_id = message->transfer_id;
      link->peer_transfer_seen = 1;
      gb_link_handle_peer_start(link, message->clock_mode,
                                message->transfer_id, message->data, now_ms);
      return;
   }

   if (message->type == GB_LINK_MSG_COMPLETE && link->active &&
       link->peer_started && message->transfer_id == link->peer_transfer_id &&
       link->peer_clock_mode == GB_LINK_INTERNAL_CLOCK &&
       (link->clock_mode == GB_LINK_EXTERNAL_CLOCK ||
        (link->clock_mode == GB_LINK_INTERNAL_CLOCK &&
         link->peer_id < link->local_id)))
      gb_link_finish(link, message->data, GB_LINK_RESULT_OK);
}

void gb_link_tick(gb_link_t *link, uint32_t now_ms)
{
   if (!link)
      return;
   if (link->pending_peer_start &&
       gb_link_expired(now_ms, link->pending_deadline_ms))
      link->pending_peer_start = 0;
   if (!link->active || !gb_link_expired(now_ms, link->deadline_ms))
      return;
   gb_link_finish(link, 0xFF, GB_LINK_RESULT_TIMEOUT);
}

int gb_link_encode(const gb_link_message_t *message,
                   uint8_t output[GB_LINK_WIRE_SIZE])
{
   if (!output || !gb_link_valid_message(message))
      return -1;
   output[0] = 'G'; output[1] = 'B'; output[2] = 'L'; output[3] = '1';
   output[4] = message->version;
   output[5] = message->type;
   output[6] = message->sender_id;
   output[7] = message->clock_mode;
   output[8] = (uint8_t)message->transfer_id;
   output[9] = (uint8_t)(message->transfer_id >> 8);
   output[10] = message->data;
   output[11] = 0;
   return GB_LINK_WIRE_SIZE;
}

int gb_link_decode(gb_link_message_t *message,
                   const uint8_t *input, unsigned length)
{
   gb_link_message_t decoded;
   if (!message || !input || length != GB_LINK_WIRE_SIZE ||
       input[0] != 'G' || input[1] != 'B' || input[2] != 'L' ||
       input[3] != '1' || input[11] != 0)
      return -1;
   memset(&decoded, 0, sizeof(decoded));
   decoded.version = input[4];
   decoded.type = input[5];
   decoded.sender_id = input[6];
   decoded.clock_mode = input[7];
   decoded.transfer_id = (uint16_t)(input[8] | ((uint16_t)input[9] << 8));
   decoded.data = input[10];
   if (!gb_link_valid_message(&decoded))
      return -1;
   *message = decoded;
   return 0;
}
