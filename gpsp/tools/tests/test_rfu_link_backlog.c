/* White-box tests for the RFU link backlog census (rfu_link_backlog, the
 * Lab's rfu_backlog), the content-free classifier (rfu_llf_is_empty, the
 * rfu_empty census), the host queue and the desync-detector fault.
 *
 * The "game" here reads one host packet per frame, librfu's own cadence, so a
 * burst stands in the queue and the census must see it.  (The delivery rules
 * that used to shape this queue -- frame pace, shedding, the overflow hold --
 * went with code-health C-4: scheduled ticks keep the backlog from forming,
 * and their tests went with them.)  The "host" puts one packet per host frame
 * on the wire.  Everything else is the production rfu.c, compiled in with
 * --gc-sections like test_rfu_peer_binding.
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
 * traffic carries content. */
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

/* One emulated client frame: receive once if the adapter says data is there,
 * then end the frame. */
#define READS_PER_FRAME 1
static void client_frame(void)
{
  int polls;
  for (polls = 0; polls < READS_PER_FRAME; polls++) {
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

/* One radio hiccup: 12 host packets held back (an ARQ retransmit stalls the
 * in-order stream) and then released together.  At equal rates the client
 * never gets them back, and the census reports it as standing latency. */
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

/* The host producing fewer packets than the client consumes (the game does
 * this in some phases, e.g. while it saves) drains it, and the census follows. */
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

/* ---- the content-free classifier (rfu_empty census) -------------------- */

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

/* Host side: a client packet as the wire carries it (NET_RFU_CLIENT_SEND,
 * 12 + 16 bytes: shorter than the 104 a stamped one has, so this also guards
 * the stamp read in rfu_net_receive). */
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

/* The host keeps every client packet, in order, content-free or not. */
static void test_host_queue_keeps_order(void)
{
  unsigned i, t;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_state = RFU_STATE_HOST;
  rfu_host.devid = 0x1111;
  rfu_host.clients[0].client_id = 2;
  rfu_host.clients[0].devid = 0x4321;
  for (i = 0; i < 12; i++)
    child_packet(0, i == 3 ? 0x33 : i == 9 ? 0x99 : 0);
  assert(host_queue_depth() == 12);
  rfu_frame_update();
  assert(host_queue_depth() == 12);
  for (i = 0; i < 12; i++) {
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 2);
    t = (rfu_buf[1] >> 16) & 0xFF;          /* byte 2 of the child LL frame */
    assert(t == (i == 3 ? 0x33u : i == 9 ? 0x99u : 0u));
  }
  assert(host_queue_depth() == 0);
  rfu_state = RFU_STATE_CLIENT;
  printf("  host queue: 12 client packets kept, read back in order\n");
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

int main(void)
{
  test_steady_link_has_no_backlog();
  test_one_stall_is_permanent();
  test_backlog_grows_with_client_losses();
  test_host_slowdown_drains_it();
  test_not_a_client_reports_none();
  test_llf_classifier();
  test_host_queue_keeps_order();
  test_fault_corrupt_fires_once();
  printf("rfu link backlog: all tests passed\n");
  return 0;
}
