/* Isolated tests for the protocol-neutral GB serial cable coordinator. */
#include <stdio.h>
#include <string.h>

#include "../netdrv/gb_link.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct endpoint endpoint;
typedef struct mesh
{
   gb_link_message_t queue[16];
   int count;
   endpoint *ep[2];
} mesh;

struct endpoint
{
   mesh *m;
   int index;
   int completions;
   uint16_t transfer_id;
   uint8_t received;
   enum gb_link_result result;
   gb_link_t link;
};

static int send_msg(void *ctx, const gb_link_message_t *msg)
{
   endpoint *ep = (endpoint *)ctx;
   if (ep->m->count >= 16)
      return -1;
   ep->m->queue[ep->m->count++] = *msg;
   return 0;
}

static void complete(void *ctx, uint16_t transfer_id, uint8_t byte,
                     enum gb_link_result result)
{
   endpoint *ep = (endpoint *)ctx;
   ep->completions++;
   ep->transfer_id = transfer_id;
   ep->received = byte;
   ep->result = result;
}

static void init_pair(mesh *m, endpoint *a, endpoint *b)
{
   memset(m, 0, sizeof(*m));
   memset(a, 0, sizeof(*a));
   memset(b, 0, sizeof(*b));
   a->m = b->m = m;
   a->index = 0; b->index = 1;
   m->ep[0] = a; m->ep[1] = b;
   gb_link_init(&a->link, 1, 2, 100, a, send_msg, complete);
   gb_link_init(&b->link, 2, 1, 100, b, send_msg, complete);
   gb_link_set_connected(&a->link, 1);
   gb_link_set_connected(&b->link, 1);
}

static void pump(mesh *m, uint32_t now_ms)
{
   while (m->count)
   {
      gb_link_message_t msg = m->queue[0];
      int i;
      for (i = 1; i < m->count; i++)
         m->queue[i - 1] = m->queue[i];
      m->count--;
      if (msg.sender_id == 1)
         gb_link_receive(&m->ep[1]->link, &msg, now_ms);
      else
         gb_link_receive(&m->ep[0]->link, &msg, now_ms);
   }
}

int main(void)
{
   mesh m;
   endpoint a, b;
   uint8_t wire[GB_LINK_WIRE_SIZE];
   gb_link_message_t msg, decoded;

   /* One internal clock and one external clock: the internal side clocks
    * the byte, and both endpoints receive the opposite SB value. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 7, 0xA5, GB_LINK_INTERNAL_CLOCK, 10) == 0);
   CHECK(gb_link_start(&b.link, 77, 0x3C, GB_LINK_EXTERNAL_CLOCK, 10) == 0);
   pump(&m, 11);
   CHECK(a.completions == 1 && a.received == 0x3C);
   CHECK(b.completions == 1 && b.received == 0xA5);
   CHECK(a.result == GB_LINK_RESULT_OK && b.result == GB_LINK_RESULT_OK);
   CHECK(a.transfer_id == 7 && b.transfer_id == 77);

   /* Explicit wire encoding has fixed size/order and rejects malformed data. */
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_START;
   msg.sender_id = 4;
   msg.clock_mode = GB_LINK_INTERNAL_CLOCK;
   msg.transfer_id = 0x1234;
   msg.data = 0xA6;
   CHECK(gb_link_encode(&msg, wire) == GB_LINK_WIRE_SIZE);
   CHECK(!memcmp(wire, "GBL1", 4));
   CHECK(gb_link_decode(&decoded, wire, sizeof(wire)) == 0);
   CHECK(decoded.sender_id == 4 && decoded.transfer_id == 0x1234 &&
         decoded.data == 0xA6 && decoded.clock_mode == GB_LINK_INTERNAL_CLOCK);
   CHECK(gb_link_decode(&decoded, wire, sizeof(wire) - 1) < 0);
   wire[0] = 'X';
   CHECK(gb_link_decode(&decoded, wire, sizeof(wire)) < 0);
   wire[0] = 'G';
   wire[4] = 2;
   CHECK(gb_link_decode(&decoded, wire, sizeof(wire)) < 0);
   wire[4] = GB_LINK_PROTOCOL_VERSION;
   wire[7] = 2;
   CHECK(gb_link_decode(&decoded, wire, sizeof(wire)) < 0);

   /* A reliable START may arrive before the local frame poll sees external
    * SC. Buffer it, then pair it when the local endpoint starts. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&b.link, 500, 0xD2, GB_LINK_INTERNAL_CLOCK, 30) == 0);
   msg = m.queue[0];
   m.count = 0;
   gb_link_receive(&a.link, &msg, 31); /* a has not started its transfer */
   CHECK(a.link.pending_peer_start == 1);
   CHECK(gb_link_start(&a.link, 50, 0x21, GB_LINK_EXTERNAL_CLOCK, 32) == 0);
   CHECK(a.link.peer_started == 1 &&
         a.link.peer_clock_mode == GB_LINK_INTERNAL_CLOCK);
   pump(&m, 33); /* A START reaches B; B sends the completion back. */
   CHECK(a.completions == 1 && a.received == 0xD2);
   CHECK(b.completions == 1 && b.received == 0x21);

   /* Pending early starts expire rather than being paired with a later byte. */
   init_pair(&m, &a, &b);
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_START;
   msg.sender_id = 2;
   msg.clock_mode = GB_LINK_EXTERNAL_CLOCK;
   msg.data = 0x55;
   gb_link_receive(&a.link, &msg, 100);
   CHECK(a.link.pending_peer_start == 1);
   gb_link_tick(&a.link, 200);
   CHECK(a.link.pending_peer_start == 0);

   /* Simultaneous internal-clock requests elect the lower endpoint ID.
    * The lower ID here is endpoint 1 (a), independent of queue order. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 8, 0x12, GB_LINK_INTERNAL_CLOCK, 20) == 0);
   CHECK(gb_link_start(&b.link, 8, 0xE7, GB_LINK_INTERNAL_CLOCK, 20) == 0);
   pump(&m, 21);
   CHECK(a.completions == 1 && a.received == 0xE7);
   CHECK(b.completions == 1 && b.received == 0x12);

   /* Two external-clock sides cannot supply a clock; they time out cleanly. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 9, 0xFF, GB_LINK_EXTERNAL_CLOCK, 100) == 0);
   CHECK(gb_link_start(&b.link, 9, 0x00, GB_LINK_EXTERNAL_CLOCK, 100) == 0);
   pump(&m, 101);
   gb_link_tick(&a.link, 201);
   gb_link_tick(&b.link, 201);
   CHECK(a.completions == 1 && b.completions == 1);
   CHECK(a.result == GB_LINK_RESULT_TIMEOUT && b.result == GB_LINK_RESULT_TIMEOUT);
   CHECK(a.received == 0xFF && b.received == 0xFF);

   /* A retried START from a byte that already timed out cannot be paired
    * with the next local transfer, and a COMPLETE must echo that START's ID. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 40, 0x11, GB_LINK_EXTERNAL_CLOCK, 300) == 0);
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_START;
   msg.sender_id = 2;
   msg.clock_mode = GB_LINK_INTERNAL_CLOCK;
   msg.transfer_id = 700;
   msg.data = 0x22;
   gb_link_receive(&a.link, &msg, 301);
   CHECK(a.link.peer_started && a.link.peer_transfer_id == 700);
   gb_link_tick(&a.link, 401);
   CHECK(a.completions == 1 && a.result == GB_LINK_RESULT_TIMEOUT);
   CHECK(gb_link_start(&a.link, 41, 0x12, GB_LINK_EXTERNAL_CLOCK, 410) == 0);
   gb_link_receive(&a.link, &msg, 411); /* stale retransmission */
   CHECK(!a.link.peer_started);
   msg.transfer_id = 701;
   msg.data = 0x23;
   gb_link_receive(&a.link, &msg, 412);
   CHECK(a.link.peer_started && a.link.peer_transfer_id == 701);
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_COMPLETE;
   msg.sender_id = 2;
   msg.transfer_id = 700; /* stale completion from the previous byte */
   msg.data = 0x33;
   gb_link_receive(&a.link, &msg, 413);
   CHECK(a.link.active);
   msg.transfer_id = 701;
   gb_link_receive(&a.link, &msg, 414);
   CHECK(a.completions == 2 && a.result == GB_LINK_RESULT_OK &&
         a.received == 0x33);

   /* The clock master timed out on byte 5 (it never saw our START) and began
    * byte 6.  Its START(6) must supersede the START(5) this external side is
    * paired with; the master's COMPLETE(6) then finishes our byte.  Dropping
    * START(6) used to strand this side until its own timeout while the master
    * believed the byte was exchanged. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&b.link, 90, 0x44, GB_LINK_EXTERNAL_CLOCK, 500) == 0);
   m.count = 0; /* B's START is lost to A (A already timed out) */
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_START;
   msg.sender_id = 1;
   msg.clock_mode = GB_LINK_INTERNAL_CLOCK;
   msg.transfer_id = 5;
   msg.data = 0x05;
   gb_link_receive(&b.link, &msg, 501);
   CHECK(b.link.peer_started && b.link.peer_transfer_id == 5);
   msg.transfer_id = 6;
   msg.data = 0x06;
   gb_link_receive(&b.link, &msg, 502);
   CHECK(b.link.peer_started && b.link.peer_transfer_id == 6 &&
         b.link.peer_data == 0x06);
   msg.transfer_id = 5; /* a late duplicate of the abandoned byte */
   gb_link_receive(&b.link, &msg, 503);
   CHECK(b.link.peer_transfer_id == 6);
   msg.type = GB_LINK_MSG_COMPLETE;
   msg.clock_mode = 0;
   msg.transfer_id = 6;
   msg.data = 0x06;
   gb_link_receive(&b.link, &msg, 504);
   CHECK(b.completions == 1 && b.result == GB_LINK_RESULT_OK &&
         b.received == 0x06);

   /* While idle, the NEWEST early START is the one retained. */
   init_pair(&m, &a, &b);
   memset(&msg, 0, sizeof(msg));
   msg.version = GB_LINK_PROTOCOL_VERSION;
   msg.type = GB_LINK_MSG_START;
   msg.sender_id = 1;
   msg.clock_mode = GB_LINK_INTERNAL_CLOCK;
   msg.transfer_id = 20;
   msg.data = 0x20;
   gb_link_receive(&b.link, &msg, 600);
   msg.transfer_id = 21;
   msg.data = 0x21;
   gb_link_receive(&b.link, &msg, 601);
   CHECK(b.link.pending_peer_start &&
         b.link.pending_peer_transfer_id == 21 &&
         b.link.pending_peer_data == 0x21);
   msg.transfer_id = 20; /* stale retransmission cannot roll it back */
   gb_link_receive(&b.link, &msg, 602);
   CHECK(b.link.pending_peer_transfer_id == 21);

   /* A peer disconnect aborts an in-flight byte and cannot leave it wedged. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 10, 0x22, GB_LINK_EXTERNAL_CLOCK, 300) == 0);
   gb_link_set_connected(&a.link, 0);
   CHECK(a.completions == 1 && a.received == 0xFF);
   CHECK(a.result == GB_LINK_RESULT_DISCONNECTED);

   /* Busy, disconnected and invalid clock requests are refused without
    * modifying an active transaction. */
   init_pair(&m, &a, &b);
   CHECK(gb_link_start(&a.link, 11, 0x11, GB_LINK_EXTERNAL_CLOCK, 400) == 0);
   CHECK(gb_link_start(&a.link, 12, 0x12, GB_LINK_INTERNAL_CLOCK, 400) < 0);
   CHECK(gb_link_start(&b.link, 12, 0x12, (enum gb_link_clock)2, 400) < 0);
   gb_link_set_connected(&b.link, 0);
   CHECK(gb_link_start(&b.link, 13, 0x13, GB_LINK_INTERNAL_CLOCK, 400) < 0);

   puts("gb_link: 11 scenarios passed");
   return 0;
}
