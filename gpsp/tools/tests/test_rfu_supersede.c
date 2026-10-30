/* White-box tests for rfu_supersede: the wireless adapter's ONE receive
 * buffer, replayed on the sender's frame timeline (rfu.c, the block above
 * rfu_supersede_client).
 *
 * The real AGB-015 keeps one packet per direction; data the GBA has not read
 * when the next packet lands is overwritten, so a late reader gets the newest
 * data and never a backlog.  Our queues must still absorb transport clumps
 * and stalls (that data a real adapter WOULD have delivered), and must stop
 * holding what a real adapter would have replaced: a game that was not looking,
 * or a sender that produces frames faster than the receiver runs.
 *
 * The "game" is the part of librfu that matters here: while the adapter says
 * data is available it receives (one packet per frame, frame_pace 1, as
 * shipped), and asking counts as looking.  The "host" stamps every packet
 * with its own frame number, as rfu_net_send_data does.  Everything else is
 * the production rfu.c.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../rfu.c"

u32 cpu_ticks;
u16 rand_gen(void) { return 0x4567; }

/* Capture what the core puts on the wire (the stamp tests read it back). */
static uint8_t tx_last[256];
static size_t tx_last_len;
void netpacket_send(uint16_t id, const void *buf, size_t len)
{
  (void)id;
  if (len <= sizeof(tx_last)) {
    memcpy(tx_last, buf, len);
    tx_last_len = len;
  }
}
void netpacket_send_unreliable(uint16_t id, const void *buf, size_t len)
{ (void)id; (void)buf; (void)len; }

static void put_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

/* Host frame counter (the sender's timeline) and packet tags. */
static unsigned host_frame, host_tag;

/* One stamped host packet carrying `tag` (1..255) in its first payload byte
 * (a parent UNI sub-frame, so never content-free). stamped=0 = an older peer. */
static void host_packet_at(unsigned frame, unsigned tag, int stamped)
{
  uint8_t p[12 + 17];
  const unsigned size = 14;
  uint32_t hdr = size | (4u << 14) | (1u << 18), h;
  memset(p, 0, sizeof(p));
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, NET_RFU_HOST_SEND);
  h = 3 + size;
  if (stamped)
    h |= 0x80 | ((frame & 0xFFFFFF) << 8);
  put_be32(p + 8, h);
  p[12] = (uint8_t)hdr; p[13] = (uint8_t)(hdr >> 8); p[14] = (uint8_t)(hdr >> 16);
  p[15] = (uint8_t)tag;
  rfu_net_receive(p, sizeof(p), 0);
}

/* The host's next frame: one packet with the next tag. */
static void host_tick(void)
{
  host_tag = host_tag % 250 + 1;
  host_packet_at(host_frame++, host_tag, 1);
}

static unsigned seen[8192], n_seen;

/* One emulated client frame of a game that is looking: it asks, and receives
 * while the adapter says there is data (frame_pace lets one through). */
static void client_frame(void)
{
  int polls;
  for (polls = 0; polls < 8; polls++) {
    if (!rfu_data_avail())
      break;
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 1);
    if (rfu_buf[0] != 0 && n_seen < 8192)
      seen[n_seen++] = (rfu_buf[1] >> 24) & 0xFF;   /* LE byte 3 = our tag */
  }
  rfu_frame_update();
}

/* A frame in which the game never touches the adapter's receive path. */
static void client_frame_away(void)
{
  rfu_frame_update();
}

static void client_reset(int supersede)
{
  memset(&rfu_client, 0, sizeof(rfu_client));
  memset(rfu_touch_bits, 0, sizeof(rfu_touch_bits));
  rfu_state = RFU_STATE_CLIENT;
  rfu_client.host_id = 0;
  rfu_client.devid = 0x77;
  rfu_gq_depth = 0;
  rfu_rx_delivered = 0;
  rfu_bl_win_n = 0;
  rfu_bl_win_min = RFU_BACKLOG_NONE;
  rfu_bl_standing = RFU_BACKLOG_NONE;
  rfu_set_shed_keep(2);                 /* the shipping configuration */
  rfu_set_frame_pace(1);
  rfu_set_pace_max_hold(24);
  rfu_set_hold(1);
  rfu_set_supersede(supersede);
  host_frame = 1000;                    /* a timeline unlike ours */
  host_tag = 0;
  n_seen = 0;
}

/* Every delivered tag is newer than the one before it (none twice, none out
 * of order); returns how many were skipped. */
static unsigned check_order(void)
{
  unsigned i, gaps = 0;
  for (i = 1; i < n_seen; i++) {
    unsigned d = (seen[i] + 250 - seen[i - 1]) % 250;
    assert(d >= 1 && d < 125);
    gaps += d - 1;
  }
  return gaps;
}

/* Equal rates with transport clumps (half the packets arrive a frame late,
 * two at once): a real adapter would have delivered every one, so nothing is
 * superseded. */
static void test_clumps_lose_nothing(void)
{
  unsigned f;
  client_reset(1);
  for (f = 0; f < 1200; f++) {
    if (f % 2 == 0) {
      host_frame++;                    /* this one is late ... */
      client_frame();
    } else {
      host_packet_at(host_frame - 1, host_tag = host_tag % 250 + 1, 1);
      host_tick();                     /* ... and lands with the next */
      client_frame();
    }
  }
  assert(check_order() == 0);
  assert(n_seen >= 1195);
  printf("  clumps at equal rates: %u delivered, 0 superseded, standing %u\n",
         n_seen, rfu_link_backlog());
}

/* A transport stall: 12 packets held back and released together while the
 * game was looking the whole time.  A real adapter would have delivered each
 * in its frame, so the rule never touches the burst.  If the sender then
 * pauses (Gen3 waiting on a reply) the queue drains and nothing is lost.  If
 * it does not, the lag the stall left STANDS -- no lossless force drains it
 * at equal rates -- and a lag that has stood RFU_SUPERSEDE_ARM frames is one
 * no one-packet adapter could hold: it is cut back to the allowance. */
static void test_stall(void)
{
  unsigned i, first, burst = 12, gaps;
  for (int pause = 1; pause >= 0; pause--) {
    client_reset(1);
    for (i = 0; i < 120; i++) {
      host_tick();
      client_frame();
    }
    first = host_frame;
    for (i = 0; i < burst; i++)
      client_frame();                  /* looking; nothing arrives */
    for (i = 0; i < burst; i++)
      host_packet_at(first + i, host_tag = host_tag % 250 + 1, 1);
    host_frame = first + burst;
    if (pause) {
      for (i = 0; i < burst + 2; i++)
        client_frame();                /* the sender waits; the burst drains */
      host_frame += burst + 2;
    }
    for (i = 0; i < 300; i++) {
      host_tick();
      client_frame();
    }
    gaps = check_order();
    if (pause) {
      assert(gaps == 0);
      assert(rfu_link_backlog() <= 1);
    } else {
      assert(gaps >= burst - RFU_SUPERSEDE_KEEP - 1 && gaps <= burst);
      assert(rfu_link_backlog() <= RFU_SUPERSEDE_KEEP);
    }
    printf("  12-frame stall, sender %s: %u superseded, standing %u\n",
           pause ? "pauses" : "keeps sending", gaps, rfu_link_backlog());
  }
}

/* The game stops looking (Golf loading a hole) while the host keeps sending.
 * On hardware it comes back to the newest packet; so does ours. */
static void test_not_looking_comes_back_to_newest(void)
{
  unsigned i, gaps, q;
  static const unsigned away[2] = { 20, 90 };
  for (int k = 0; k < 2; k++) {
    client_reset(1);
    for (i = 0; i < 120; i++) {
      host_tick();
      client_frame();
    }
    for (i = 0; i < away[k]; i++) {
      host_tick();
      client_frame_away();
    }
    q = rfu_client_queued();
    for (i = 0; i < 90; i++) {
      host_tick();
      client_frame();
    }
    gaps = check_order();
    printf("  %u frames not looking: queue %u at the return -> standing %u,"
           " %u superseded\n", away[k], q, rfu_link_backlog(), gaps);
    /* Shorter than the arming time: the queue holds it all until it has
     * stood that long.  Longer: trimmed while still away. */
    assert(away[k] < RFU_SUPERSEDE_ARM ? q >= away[k] - 1
                                       : q <= RFU_SUPERSEDE_KEEP + 1);
    assert(rfu_link_backlog() <= RFU_SUPERSEDE_KEEP);
    assert(gaps >= away[k] - RFU_SUPERSEDE_KEEP - 2);
  }
}

/* THE RATCHET THE FIXTURES FOUND.  The receiver runs 3 frames for the host's
 * 4 (a PSP that cannot hold the session rate).  The game looks every frame
 * and still can take only one packet a frame, the catch-up is shut, nothing
 * is content-free: without the rule the queue grows by a packet every three
 * frames until the 256-packet ceiling throws data away; with it the backlog holds at the jitter allowance and the
 * game is handed the newest data, as a real adapter would. */
static void test_rate_mismatch_bounded(void)
{
  unsigned f, q_off, q_on, gaps;
  client_reset(0);
  for (f = 0; f < 1200; f++) {
    host_tick();
    if (f % 3 == 2)
      host_tick();
    client_frame();
  }
  q_off = rfu_client_queued();
  client_reset(1);
  for (f = 0; f < 1200; f++) {
    host_tick();
    if (f % 3 == 2)
      host_tick();
    client_frame();
  }
  q_on = rfu_client_queued();
  gaps = check_order();
  printf("  4:3 rate mismatch, 20 s: queue %u without the rule, %u with it "
         "(%u superseded)\n", q_off, q_on, gaps);
  assert(q_off >= RFU_PKT_QUEUE + RFU_HOLD_SLOTS - 2);   /* full: queue + hold */
  assert(q_on <= RFU_SUPERSEDE_KEEP + 1);
  assert(rfu_link_backlog() <= RFU_SUPERSEDE_KEEP);
  assert(gaps >= 350 && gaps <= 420);
}

/* An older peer (no stamp) keeps the historical queue: no timeline, no rule. */
static void test_unstamped_is_historical(void)
{
  unsigned f;
  client_reset(1);
  for (f = 0; f < 600; f++) {
    host_packet_at(0, host_tag = host_tag % 250 + 1, 0);
    if (f % 3 == 2)
      host_packet_at(0, host_tag = host_tag % 250 + 1, 0);
    client_frame();
  }
  assert(check_order() == 0);
  assert(rfu_client_queued() >= 150);
}

/* Off means off. */
static void test_off_is_historical(void)
{
  unsigned i;
  client_reset(0);
  for (i = 0; i < 90; i++) {
    host_tick();
    client_frame_away();
  }
  for (i = 0; i < 30; i++) {
    host_tick();
    client_frame();
  }
  assert(check_order() == 0);
  assert(rfu_client_queued() >= 85);
}

/* Gen3's traffic: bursts of several SEND_DATA per sender frame (the Union
 * Room handshake), then a wait for the reply, under transport jitter, with
 * the game reading from its reception callback -- here, skipping a frame now
 * and then.  On hardware nothing is lost and Gen3 needs every packet; the
 * queue swells (the e2e saw 9) and drains by itself.  Superseding inside
 * those transients desynced the impaired Emerald trade; the rule must stay
 * disarmed through them. */
static void test_gen3_bursts_lossless(void)
{
  unsigned f, k, tag = 0, sent = 0;
  for (int skip = 0; skip < 2; skip++) {
    client_reset(1);
    tag = 0;
    sent = 0;
    for (f = 0; f < 20; f++)
      client_frame();
    for (f = 0; f < 600; f++) {
      if (f % 10 < 3) {                        /* 3 frames of 3 sends */
        for (k = 0; k < 3; k++) {
          tag = tag % 250 + 1;
          host_packet_at(7000 + f, tag, 1);
          sent++;
        }
      }
      if (skip && f % 10 == 1)
        client_frame_away();                   /* reads a frame late */
      else
        client_frame();
    }
    for (f = 0; f < 60; f++)
      client_frame();
    assert(check_order() == 0);
    assert(n_seen == sent);
    printf("  Gen3-style bursts (3x3 per 10 frames%s): %u/%u delivered\n",
           skip ? ", a frame skipped in each" : "", n_seen, sent);
  }
}

/* A stamp that jumps by more than a minute (a new sender) restarts the
 * timeline instead of mapping across the gap. */
static void test_stamp_wrap_and_jump(void)
{
  unsigned f;
  client_reset(1);
  host_frame = 0xFFFFF0;               /* 24-bit wrap inside the run */
  for (f = 0; f < 200; f++) {
    host_tick();
    client_frame();
  }
  assert(check_order() == 0);
  host_frame = 0x123456;
  for (f = 0; f < 200; f++) {
    host_tick();
    client_frame();
  }
  assert(rfu_link_backlog() <= 1);
}

/* ---- host side: the client->host queue ------------------------------ */

static unsigned child_frame, child_tag;
static unsigned hseen[8192], n_hseen;

static void child_packet(unsigned frame, unsigned tag, int stamped)
{
  uint8_t p[12 + 92];
  memset(p, 0, sizeof(p));
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, NET_RFU_CLIENT_SEND);
  put_be32(p + 8, (16u << 24) | (0u << 16) | 0x4321);
  p[12] = 14; p[13] = (uint8_t)(4u << 2);      /* child UNI header, size 14 */
  p[14] = (uint8_t)tag;
  if (stamped) {
    p[12 + 88] = RFU_STAMP_TAG;
    p[12 + 89] = (uint8_t)(frame >> 16);
    p[12 + 90] = (uint8_t)(frame >> 8);
    p[12 + 91] = (uint8_t)frame;
  }
  rfu_net_receive(p, sizeof(p), 2);
}

static void host_game_frame(void)
{
  rfu_touch_now[1] = 1;                /* the host game asks every frame */
  if (rfu_data_avail()) {
    rfu_cmd = RFU_CMD_RECV_DATA;
    rfu_plen = 0;
    assert(rfu_process_command_inner() >= 1);
    if (rfu_buf[0] != 0 && n_hseen < 8192)
      hseen[n_hseen++] = (rfu_buf[1] >> 16) & 0xFF;  /* byte 2 = our tag */
  }
  rfu_frame_update();
}

static void host_reset(int supersede)
{
  memset(&rfu_host, 0, sizeof(rfu_host));
  memset(rfu_touch_bits, 0, sizeof(rfu_touch_bits));
  rfu_state = RFU_STATE_HOST;
  rfu_host.devid = 0x1111;
  rfu_host.clients[0].devid = 0x4321;
  rfu_host.clients[0].client_id = 2;
  rfu_set_hold(1);
  rfu_set_shed_keep(2);
  rfu_set_supersede(supersede);
  child_frame = 77;
  child_tag = 0;
  n_hseen = 0;
}

static unsigned host_q(void)
{
  unsigned i, n = 0;
  for (i = 0; i < RFU_PKT_QUEUE; i++)
    if (rfu_host.clients[0].pkts[i].datalen)
      n++;
  return n + rfu_hold_count(1, 0);
}

static void test_host_side_rate_mismatch(void)
{
  unsigned f, q_off, q_on;
  for (int on = 0; on < 2; on++) {
    host_reset(on);
    for (f = 0; f < 600; f++) {
      child_tag = child_tag % 250 + 1;
      child_packet(child_frame++, child_tag, 1);
      if (f % 4 == 3) {
        child_tag = child_tag % 250 + 1;
        child_packet(child_frame++, child_tag, 1);
      }
      host_game_frame();
    }
    if (on) q_on = host_q(); else q_off = host_q();
  }
  printf("  host side, 5:4 mismatch: queue %u without, %u with the rule\n",
         q_off, q_on);
  assert(q_off >= 140);
  assert(q_on <= RFU_SUPERSEDE_KEEP + 1);
}

/* ---- the wire ---------------------------------------------------------- */

static void test_stamps_on_the_wire(void)
{
  u32 pl[8] = { 0x11223344 };
  u32 h;
  rfu_state = RFU_STATE_HOST;
  rfu_frame_no = 0x1234567;
  rfu_net_send_data(0, NET_RFU_HOST_SEND, 17, pl, 17);
  assert(tx_last_len == 104);
  h = get_be32(tx_last + 8);
  assert((h & 0x7f) == 17);            /* what every receiver reads */
  assert(h & 0x80);
  assert((h >> 8) == 0x234567);
  rfu_state = RFU_STATE_CLIENT;
  rfu_net_send_data(0, NET_RFU_CLIENT_SEND, (16u << 24) | 0x4321, pl, 16);
  assert(get_be32(tx_last + 8) == ((16u << 24) | 0x4321));   /* unchanged */
  assert(tx_last[12] == 0x44);                                /* payload */
  assert(tx_last[12 + 88] == RFU_STAMP_TAG);
  assert(tx_last[12 + 89] == 0x23 && tx_last[12 + 90] == 0x45 &&
         tx_last[12 + 91] == 0x67);
  rfu_frame_no = 0;
}

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  test_stamps_on_the_wire();
  test_clumps_lose_nothing();
  test_stall();
  test_not_looking_comes_back_to_newest();
  test_rate_mismatch_bounded();
  test_unstamped_is_historical();
  test_off_is_historical();
  test_gen3_bursts_lossless();
  test_stamp_wrap_and_jump();
  test_host_side_rate_mismatch();
  printf("rfu supersede: all tests passed\n");
  return 0;
}
