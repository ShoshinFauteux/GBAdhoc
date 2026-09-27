/* White-box tests for the RFU link backlog: the ratchet that makes wireless
 * latency grow until "Communications failed", the measurement that shows it
 * (rfu_link_backlog), and the fix that bounds it (link shedding,
 * rfu_shed_keep).
 *
 * The "game" here is the part of librfu that matters: while the adapter says
 * data is available it keeps receiving (MscCallback_Child re-arms the wait
 * after every packet), and it drains its own recvQueue once per frame.  The
 * "host" puts one packet per host frame on the wire.  Everything else is the
 * production rfu.c, compiled in with --gc-sections like test_rfu_peer_binding.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../rfu.c"

u32 cpu_ticks;
u16 rand_gen(void) { return 0x4567; }
void netpacket_send(uint16_t id, const void *buf, size_t len)
{ (void)id; (void)buf; (void)len; }
void netpacket_send_unreliable(uint16_t id, const void *buf, size_t len)
{ (void)id; (void)buf; (void)len; }

static void put_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static unsigned host_seq;

/* A parent LL frame as librfu builds it: one UNI sub-frame, 3-byte header
 * (size | UNI<<14 | bmSlot<<18), then the command table.  `tag` 0 = an
 * all-zero table (content-free); otherwise byte 0 carries the tag. */
static unsigned parent_llf(uint8_t *d, unsigned tag)
{
  const unsigned size = 14;
  uint32_t hdr = size | (4u << 14) | (1u << 18);
  memset(d, 0, 3 + size);
  d[0] = (uint8_t)hdr; d[1] = (uint8_t)(hdr >> 8); d[2] = (uint8_t)(hdr >> 16);
  if (tag)
    d[3] = (uint8_t)tag;
  return 3 + size;
}

/* One host link frame arriving at the client (NET_RFU_HOST_SEND).  Default
 * traffic carries content (never sheddable), so the ratchet tests below are
 * about the queue, not the shedding. */
static void host_packet_tagged(unsigned tag)
{
  uint8_t p[12 + 32];
  unsigned n;
  memset(p, 0, sizeof(p));
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, NET_RFU_HOST_SEND);
  n = parent_llf(p + 12, tag);
  put_be32(p + 8, n);
  rfu_net_receive(p, 12 + n, 0);
}

static void host_packet(void)
{
  host_packet_tagged(1 + (host_seq++ % 250));
}

static unsigned delivered;
static unsigned seen_tags[4096], n_seen;

/* One emulated client frame: receive while the adapter says data is there
 * (bounded, the game re-arms at most a handful of times), then end the frame. */
static void client_frame(void)
{
  int polls;
  for (polls = 0; polls < 8; polls++) {
    if (!rfu_data_avail())
      break;
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 1);
    if (rfu_buf[0] != 0) {
      unsigned tag = (rfu_buf[1] >> 24) & 0xFF;   /* LE: byte 3 of word 1 */
      delivered++;
      if (tag && n_seen < 4096)
        seen_tags[n_seen++] = tag;
    }
  }
  rfu_frame_update();
}

static void client_reset(void)
{
  memset(&rfu_client, 0, sizeof(rfu_client));
  rfu_state = RFU_STATE_CLIENT;
  rfu_client.host_id = 0;
  rfu_client.devid = 0x77;
  rfu_gq_depth = 0;
  rfu_rx_delivered = 0;
  rfu_bl_win_n = 0;
  rfu_bl_win_min = RFU_BACKLOG_NONE;
  rfu_bl_standing = RFU_BACKLOG_NONE;
  delivered = 0;
  n_seen = 0;
  rfu_set_shed_keep(0);                /* each test opts in */
  rfu_set_frame_pace(1);               /* the shipping configuration */
  rfu_set_pace_max_hold(0);            /* observe the queue, never trim */
}

static void run(unsigned frames)
{
  unsigned i;
  for (i = 0; i < frames; i++) {
    host_packet();
    client_frame();
  }
}

/* Equal rates, no disturbance: the standing backlog stays at its floor. */
static void test_steady_link_has_no_backlog(void)
{
  client_reset();
  run(600);
  assert(rfu_link_backlog() <= 1);
  assert(delivered >= 598);
  printf("  steady: standing=%u delivered=%u\n", rfu_link_backlog(), delivered);
}

/* THE RATCHET.  One radio hiccup: 12 host packets held back (an ARQ
 * retransmit stalls the in-order stream) and then released together.  At
 * equal rates the client never gets them back: the backlog is permanent,
 * and the single catch-up delivery the pace gate grants is all the restoring
 * force there is. */
static void test_one_stall_is_permanent(void)
{
  unsigned i, before, after;
  client_reset();
  run(120);
  before = rfu_link_backlog();
  for (i = 0; i < 12; i++)
    client_frame();                    /* 12 frames, nothing arrives */
  for (i = 0; i < 12; i++)
    host_packet();                     /* ...then the burst lands */
  run(1200);                           /* 20 s of healthy, equal-rate link */
  after = rfu_link_backlog();
  printf("  one 12-frame stall: standing %u -> %u after 20 s at equal rates\n",
         before, after);
  assert(before <= 1);
  assert(after >= 9);                  /* ~11 frames of latency, forever */
}

/* Every lost client frame (a stall the pacer forgives, a page-in) is one
 * packet of permanent backlog: the lag grows with the count of losses. */
static void test_backlog_grows_with_client_losses(void)
{
  unsigned k, i, prev = 0;
  client_reset();
  for (k = 0; k < 5; k++) {
    run(300);
    for (i = 0; i < 4; i++)
      host_packet();                   /* host ran 4 frames the client lost */
    run(60);
    printf("  after %u loss episodes: standing=%u\n", k + 1, rfu_link_backlog());
    assert(rfu_link_backlog() >= prev + 3);
    prev = rfu_link_backlog();
  }
}

/* Without shedding, the only thing that drains the client's queue is the host
 * producing fewer packets than the client consumes (the game does this in some
 * phases, e.g. while it saves).  Note what this test does NOT show: the client
 * answers every packet it consumes, so in the real loop those answers pile up
 * in the HOST's queue instead -- pacing moves the latency, it cannot remove
 * it.  That is why the fix sheds content-free packets instead. */
static void test_host_slowdown_drains_it(void)
{
  unsigned i;
  client_reset();
  for (i = 0; i < 20; i++)
    host_packet();
  run(60);
  assert(rfu_link_backlog() >= 18);
  for (i = 0; i < 240; i++) {          /* host at ~5/6 of the client's rate */
    if (i % 6)
      host_packet();
    client_frame();
  }
  printf("  host 1/6 slower for 4 s: standing=%u\n", rfu_link_backlog());
  assert(rfu_link_backlog() <= 2);
}

static void test_not_a_client_reports_none(void)
{
  client_reset();
  run(60);
  assert(rfu_link_backlog() != RFU_BACKLOG_NONE);
  rfu_state = RFU_STATE_HOST;
  assert(rfu_link_backlog() == RFU_BACKLOG_NONE);
  rfu_frame_update();
  rfu_state = RFU_STATE_CLIENT;
  /* A fresh client session starts with no sample yet, not a stale number. */
  assert(rfu_link_backlog() == RFU_BACKLOG_NONE);
}

/* ---- link shedding --------------------------------------------------- */

static void test_llf_classifier(void)
{
  uint8_t d[64];
  unsigned n = parent_llf(d, 0);
  assert(rfu_llf_is_empty(d, n, true));
  n = parent_llf(d, 7);
  assert(!rfu_llf_is_empty(d, n, true));
  /* two zero UNI sub-frames back to back: still content-free */
  n = parent_llf(d, 0);
  n += parent_llf(d + n, 0);
  assert(rfu_llf_is_empty(d, n, true));
  /* an NI sub-frame (state 2) is never content-free: librfu's acks ride it */
  n = parent_llf(d, 0);
  {
    uint32_t hdr = 14 | (2u << 14) | (1u << 18);
    d[0] = (uint8_t)hdr; d[1] = (uint8_t)(hdr >> 8); d[2] = (uint8_t)(hdr >> 16);
  }
  assert(!rfu_llf_is_empty(d, n, true));
  /* the parent's 1-byte null frame, truncations, empty input: keep */
  d[0] = 0xFF;
  assert(!rfu_llf_is_empty(d, 1, true));
  n = parent_llf(d, 0);
  assert(!rfu_llf_is_empty(d, n - 1, true));
  assert(!rfu_llf_is_empty(d, 0, true));
  /* child format: 2-byte header, size in 0x1F, state at bit 10 */
  memset(d, 0, sizeof(d));
  d[0] = 14; d[1] = (uint8_t)(4u << 2);         /* 14 | UNI<<10 */
  assert(rfu_llf_is_empty(d, 16, false));
  d[5] = 1;
  assert(!rfu_llf_is_empty(d, 16, false));
  printf("  llf classifier: ok\n");
}

/* THE FIX.  Same one-stall ratchet as above, but the traffic is shaped like a
 * live trade -- mostly content-free -- and shedding is on: the standing
 * backlog returns to `keep`, and every packet that carried content still
 * reaches the game, once, in order. */
static void test_shedding_bounds_the_ratchet(void)
{
  unsigned i, tag = 1, burst_q;
  client_reset();
  rfu_set_shed_keep(2);
  for (i = 0; i < 120; i++) {           /* 1 in 5 packets carries content */
    host_packet_tagged((i % 5) ? 0 : tag++);
    client_frame();
  }
  for (i = 0; i < 12; i++)
    client_frame();                     /* stall: nothing arrives */
  for (i = 0; i < 12; i++)
    host_packet_tagged((i % 5) ? 0 : tag++);   /* the burst */
  burst_q = rfu_client_queued();
  for (i = 0; i < 600; i++) {
    host_packet_tagged((i % 5) ? 0 : tag++);
    client_frame();
  }
  printf("  shedding: queue %u after the burst, standing %u after 10 s\n",
         burst_q, rfu_link_backlog());
  assert(burst_q >= 12);
  assert(rfu_link_backlog() <= 2);
  assert(n_seen + 3 >= tag - 1);        /* at most `keep`+1 still queued */
  for (i = 0; i < n_seen; i++)
    assert(seen_tags[i] == i + 1);      /* none lost, none twice, in order */
}

/* Shedding can only remove what carries nothing.  An all-content backlog is
 * left alone -- the latency stays, but no command is ever lost. */
static void test_shedding_never_drops_content(void)
{
  unsigned i;
  client_reset();
  rfu_set_shed_keep(2);
  for (i = 0; i < 20; i++)
    host_packet();                      /* 20 packets, all with content */
  for (i = 0; i < 60; i++) {
    host_packet();
    client_frame();
  }
  assert(rfu_link_backlog() >= 18);
  for (i = 1; i < n_seen; i++)
    assert(seen_tags[i] == (seen_tags[i - 1] % 250) + 1);
  printf("  shedding: all-content backlog kept intact (standing %u)\n",
         rfu_link_backlog());
}

/* Off means off: keep = 0 is the historical queue. */
static void test_shedding_off_is_historical(void)
{
  unsigned i;
  client_reset();
  for (i = 0; i < 12; i++)
    host_packet_tagged(0);
  run(60);
  assert(rfu_link_backlog() >= 11);
}

/* Host side: the client->host queue sheds the same way. */
static void child_packet(unsigned clid_slot, unsigned tag)
{
  uint8_t p[12 + 16];
  memset(p, 0, sizeof(p));
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, NET_RFU_CLIENT_SEND);
  put_be32(p + 8, (16u << 24) | (clid_slot << 16) | 0x4321);
  p[12] = 14; p[13] = (uint8_t)(4u << 2);      /* child UNI header, size 14 */
  if (tag)
    p[14] = (uint8_t)tag;
  rfu_net_receive(p, sizeof(p), 2);
}

static unsigned host_queue_depth(void)
{
  unsigned i, n = 0;
  for (i = 0; i < RFU_PKT_QUEUE; i++)
    if (rfu_host.clients[0].pkts[i].datalen)
      n++;
  return n;
}

static void test_host_side_shedding(void)
{
  unsigned i, tags = 0, t;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_state = RFU_STATE_HOST;
  rfu_host.devid = 0x1111;
  rfu_host.clients[0].client_id = 2;
  rfu_host.clients[0].devid = 0x4321;
  rfu_set_shed_keep(2);
  for (i = 0; i < 12; i++)
    child_packet(0, i == 3 ? 0x33 : i == 9 ? 0x99 : 0);
  assert(host_queue_depth() == 12);
  rfu_frame_update();
  assert(host_queue_depth() == 2);          /* exactly the two with content */
  for (i = 0; i < 2; i++) {
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 2);
    t = (rfu_buf[1] >> 16) & 0xFF;          /* byte 2 of the child LL frame */
    tags = (tags << 8) | t;
  }
  assert(tags == 0x3399);                    /* in order */
  rfu_set_shed_keep(0);
  rfu_state = RFU_STATE_CLIENT;
  printf("  host-side shedding: 12 -> 2, both commands kept in order\n");
}

/* The desync-detector validation fault corrupts exactly one content packet
 * (the Nth), leaves content-free ones alone, and fires once. */
static void test_fault_corrupt_fires_once(void)
{
  unsigned i, flipped = 0, clean = 0;
  client_reset();
  rfu_set_fault_corrupt(2);
  for (i = 0; i < 8; i++) {
    host_packet_tagged((i % 2) ? 0 : 10 + i);   /* content, empty, ... */
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    rfu_rx_delivered = 0;
    assert(rfu_process_command_inner() >= 1);
    if (((rfu_buf[2] >> 8) & 0xFF) == 1)
      flipped++;
    else
      clean++;
    rfu_frame_update();
  }
  rfu_set_fault_corrupt(0);
  assert(flipped == 1 && clean == 7);
  /* The Nth content packet too short to corrupt: the NEXT one takes it. */
  client_reset();
  rfu_set_fault_corrupt(1);
  {
    uint8_t p[12 + 4];
    memset(p, 0, sizeof(p));
    put_be32(p, NET_RFU_HEADER);
    put_be32(p + 4, NET_RFU_HOST_SEND);
    put_be32(p + 8, 3);
    p[12] = 0; p[13] = 0x80; p[14] = 0x04;        /* NI-state header, size 0 */
    rfu_net_receive(p, 15, 0);
  }
  host_packet_tagged(9);
  rfu_cmd = RFU_CMD_RECV_DATA; rfu_plen = 0; rfu_rx_delivered = 0;
  assert(rfu_process_command_inner() >= 1);        /* the short one */
  rfu_frame_update();
  rfu_cmd = RFU_CMD_RECV_DATA; rfu_plen = 0; rfu_rx_delivered = 0;
  assert(rfu_process_command_inner() >= 1);
  assert(((rfu_buf[2] >> 8) & 0xFF) == 1);         /* flipped */
  rfu_set_fault_corrupt(0);
  /* A span of 3 from the 2nd content packet: content packets 2, 3, 4 are
   * flipped, at payload bytes 5, 6, 7 in turn; 1 and 5.. are untouched. */
  client_reset();
  rfu_set_fault_corrupt(2);
  rfu_set_fault_span(3);
  for (i = 0; i < 6; i++) {
    unsigned w;
    host_packet_tagged(20 + i);
    rfu_cmd = RFU_CMD_RECV_DATA; rfu_plen = 0; rfu_rx_delivered = 0;
    assert(rfu_process_command_inner() >= 1);
    w = rfu_buf[2];                          /* payload bytes 4..7 */
    if (i >= 1 && i <= 3)
      assert(w == (1u << (8 * i)));          /* byte 4 + i: 5, 6, 7 */
    else
      assert(w == 0);
    rfu_frame_update();
  }
  rfu_set_fault_corrupt(0);
  rfu_set_fault_span(1);
  /* Targeted mode: only the SETMONDATA chunk counts, and its value byte (16)
   * is the one flipped.  Frames laid out as the G2 dumps show them. */
  client_reset();
  rfu_set_fault_mode(1);
  rfu_set_fault_corrupt(1);
  for (i = 0; i < 3; i++) {
    uint8_t p[12 + 73];
    static const uint8_t text[14] =   /* chunk 0 of a PRINTSTRING record */
      { 0x00, 0x89, 0, 0, 0, 3, 0x48, 0, 0, 0, 0x10, 0, 0x30, 1 };
    static const uint8_t setmon[14] = /* chunk 0 of SETMONDATA PPMOVE1 = 23 */
      { 0x00, 0x89, 0, 1, 0, 1, 0x04, 0, 0, 0, 0x02, 0x09, 0x00, 23 };
    memset(p, 0, sizeof(p));
    put_be32(p, NET_RFU_HEADER);
    put_be32(p + 4, NET_RFU_HOST_SEND);
    put_be32(p + 8, 73);
    p[12] = 0x46; p[13] = 0x00; p[14] = 0x05;   /* the FRLG parent header */
    memcpy(p + 15, i == 1 ? setmon : text, 14);
    rfu_net_receive(p, 12 + 73, 0);
    rfu_cmd = RFU_CMD_RECV_DATA; rfu_plen = 0; rfu_rx_delivered = 0;
    assert(rfu_process_command_inner() >= 1);
    /* payload byte 16 is rfu_buf[1 + 16/4] byte 0 */
    if (i == 1)
      assert((rfu_buf[5] & 0xFF) == (23 ^ 1));   /* the value, flipped */
    else
      assert((rfu_buf[5] & 0xFF) == 1);          /* text untouched */
    assert(((rfu_buf[2] >> 8) & 0xFF) == 0);     /* byte 5 never flipped */
    rfu_frame_update();
  }
  assert(rfu_fault_fired == 1);
  rfu_set_fault_corrupt(0);
  rfu_set_fault_mode(0);
  printf("  fault_corrupt: exactly one content packet flipped; span walks bytes 5,6,7;"
         " targeted mode hits only the SETMONDATA value\n");
}

/* MYSTERY GIFT goes through this queue too: psp/mgift_cart.c injects its
 * frames as NET_RFU_HOST_SEND into the game's CLIENT adapter.  Its join
 * handshake is NI sub-frames (state 1..3, mgift_cart_frame) and bare NI acks;
 * none of those may ever be shed, however deep the queue.  Frames built
 * exactly the way mgift_cart_frame() builds them. */
static void cart_frame(unsigned stage)
{
  uint8_t p[12 + 16];
  unsigned n = stage == 1 ? 7 : stage == 2 ? 1 : 0, size = 3 + n;
  uint32_t h = (1u << 18) | (stage << 14) | (stage == 3 ? 0u : 1u << 11) | n;
  memset(p, 0, sizeof(p));
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, NET_RFU_HOST_SEND);
  put_be32(p + 8, size);
  p[12] = (uint8_t)h; p[13] = (uint8_t)(h >> 8); p[14] = (uint8_t)(h >> 16);
  rfu_net_receive(p, 12 + size, 0);
}

static void test_mystery_gift_frames_never_shed(void)
{
  unsigned i;
  client_reset();
  rfu_set_shed_keep(2);
  for (i = 0; i < 12; i++)
    cart_frame(1 + (i % 3));
  rfu_frame_update();
  assert(rfu_client_queued() == 12);
  printf("  mystery gift: 12 NI handshake frames queued, none shed\n");
}


/* ---- HOLD, NEVER DISCARD (rfu_hold) ------------------------------------ */

/* A burst far deeper than the 64-slot queue (the H0b run 3 shape: the join's
 * transport stalled, then released ~60 at once): with the hold on, nothing is
 * discarded and every packet reaches the game, in order. */
static void test_hold_client_never_discards(void)
{
  unsigned i;
  client_reset();
  rfu_set_hold(1);
  for (i = 1; i <= 150; i++)
    host_packet_tagged(i);
  assert(rfu_client_queued() == 150);      /* 64 queued + 86 held */
  for (i = 0; i < 400 && delivered < 150; i++)
    client_frame();
  assert(delivered == 150 && n_seen == 150);
  for (i = 0; i < 150; i++)
    assert(seen_tags[i] == i + 1);         /* in order, none lost */
  assert(rfu_client_queued() == 0);
  rfu_set_hold(0);
  printf("  hold (client): 150-packet burst, 0 discarded, delivered in order\n");
}

/* Hold off is the historical queue: the 65th packet onwards is discarded. */
static void test_hold_off_is_historical(void)
{
  unsigned i;
  client_reset();
  rfu_set_hold(0);
  for (i = 1; i <= 150; i++)
    host_packet_tagged(i);
  assert(rfu_client_queued() == RFU_PKT_QUEUE);
}

/* The client's pace backstop (rfu_pace_max_hold) discarded the OLDEST
 * packets once the queue passed it -- H0b run 2's 55 pacedrops.  Under the
 * hold it does not; without the hold it still does (arm A is historical). */
static void test_hold_disarms_pace_backstop(void)
{
  unsigned i;
  client_reset();
  rfu_set_pace_max_hold(24);
  rfu_set_hold(1);
  for (i = 1; i <= 40; i++)
    host_packet_tagged(i);
  rfu_frame_update();
  assert(rfu_client_queued() == 40);
  client_reset();
  rfu_set_pace_max_hold(24);
  rfu_set_hold(0);
  for (i = 1; i <= 40; i++)
    host_packet_tagged(i);
  rfu_frame_update();
  assert(rfu_client_queued() == 24);
  rfu_set_pace_max_hold(0);
}

/* Host side, two clients interleaved: each client's packets stay in its own
 * order and none is lost, although both overflow into the one shared ring. */
static void test_hold_host_two_clients_in_order(void)
{
  unsigned i, got0 = 0, got1 = 0, polls;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_state = RFU_STATE_HOST;
  rfu_host.devid = 0x1111;
  rfu_host.clients[0].client_id = 2;
  rfu_host.clients[0].devid = 0x4321;
  rfu_host.clients[1].client_id = 3;
  rfu_host.clients[1].devid = 0x4321;
  rfu_set_shed_keep(0);
  rfu_set_hold(1);
  for (i = 1; i <= 90; i++) {
    child_packet(0, i);
    {
      uint8_t q[12 + 16];
      memset(q, 0, sizeof(q));
      put_be32(q, NET_RFU_HEADER);
      put_be32(q + 4, NET_RFU_CLIENT_SEND);
      put_be32(q + 8, (16u << 24) | (1u << 16) | 0x4321);
      q[12] = 14; q[13] = (uint8_t)(4u << 2);
      q[14] = (uint8_t)(100 + i);
      rfu_net_receive(q, sizeof(q), 3);
    }
  }
  for (polls = 0; polls < 200 && (got0 < 90 || got1 < 90); polls++) {
    u32 hdr, off = 1, len0, len1;
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 1);
    hdr = rfu_buf[0];
    len0 = (hdr >> 8) & 0x1F;
    len1 = (hdr >> 13) & 0x1F;
    if (len0) {
      assert(((rfu_buf[off] >> 16) & 0xFF) == got0 + 1);
      got0++;
    }
    if (len1) {
      u8 b[64];
      memcpy(b, &rfu_buf[off], sizeof(b));
      assert(b[len0 + 2] == 100 + got1 + 1);
      got1++;
    }
    rfu_frame_update();
  }
  assert(got0 == 90 && got1 == 90);
  rfu_set_hold(0);
  rfu_state = RFU_STATE_CLIENT;
  printf("  hold (host): 2 clients x 90, each in order, 0 discarded\n");
}

/* WHAT BOUNDS IT: holding turns a discard into latency, and shedding is what
 * drains it.  A 150-packet burst of mostly content-free packets (battle
 * traffic is 60-80 % empty) recovers to the keep within a few frames, and
 * every content packet survives in order. */
static void test_hold_backlog_recovers_by_shedding(void)
{
  unsigned i, f, content = 0;
  client_reset();
  rfu_set_hold(1);
  rfu_set_shed_keep(2);
  for (i = 1; i <= 150; i++) {
    unsigned tag = (i % 5 == 0) ? (++content) : 0;
    host_packet_tagged(tag);
  }
  assert(rfu_client_queued() == 150);
  for (f = 0; f < 60; f++) {
    host_packet_tagged(0);                 /* the link keeps running */
    client_frame();
  }
  assert(rfu_link_backlog() <= 2);
  assert(n_seen == content);
  for (i = 0; i < content; i++)
    assert(seen_tags[i] == i + 1);
  rfu_set_hold(0);
  rfu_set_shed_keep(0);
  printf("  hold + shedding: 150 backlog -> %u within 60 frames, %u content kept\n",
         rfu_link_backlog(), content);
}

/* A held packet whose link has gone is purged, never handed to a NEW link. */
static void test_hold_stale_purged(void)
{
  unsigned i;
  client_reset();
  rfu_set_hold(1);
  for (i = 1; i <= 80; i++)
    host_packet_tagged(i);
  assert(rfu_client_queued() == 80);
  memset(&rfu_client, 0, sizeof(rfu_client));
  rfu_client.devid = 0x99;                 /* a different link */
  rfu_hold_refill();
  assert(rfu_client_queued() == 0);
  rfu_set_hold(0);
}

int main(void)
{
  test_steady_link_has_no_backlog();
  test_one_stall_is_permanent();
  test_backlog_grows_with_client_losses();
  test_host_slowdown_drains_it();
  test_not_a_client_reports_none();
  test_llf_classifier();
  test_shedding_bounds_the_ratchet();
  test_shedding_never_drops_content();
  test_shedding_off_is_historical();
  test_host_side_shedding();
  test_fault_corrupt_fires_once();
  test_mystery_gift_frames_never_shed();
  test_hold_client_never_discards();
  test_hold_off_is_historical();
  test_hold_disarms_pace_backstop();
  test_hold_host_two_clients_in_order();
  test_hold_backlog_recovers_by_shedding();
  test_hold_stale_purged();
  printf("rfu link backlog: all tests passed\n");
  return 0;
}
