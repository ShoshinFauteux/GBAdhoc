/* White-box tests for the emulated wireless adapter's link LIFECYCLE: who may
 * end a link, what a slot means after it is freed, and what the adapter
 * reports when the radio session underneath it goes away.  Same build as
 * test_rfu_peer_binding.c (production rfu.c included, --gc-sections). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../rfu.c"

u32 cpu_ticks;
u16 rand_gen(void) { return 0x4567; }
void rand_seed(u32 s) { (void)s; }

/* read_ioreg/write_ioreg back onto this array (common.h macros index it). */
u16 io_registers[512];

static unsigned sends;
static uint16_t last_dst;
static uint32_t last_type, last_hdata;

static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

void netpacket_send(uint16_t id, const void *buf, size_t len)
{
  const uint8_t *b = (const uint8_t *)buf;
  assert(len >= 12);
  sends++;
  last_dst = id;
  last_type = be32(b + 4);
  last_hdata = be32(b + 8);
}

void netpacket_send_unreliable(uint16_t id, const void *buf, size_t len)
{
  netpacket_send(id, buf, len);
}

void netpacket_poll_receive(void) {}

static void put_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void packet(uint8_t p[16], uint32_t type, uint32_t hdata)
{
  memset(p, 0, 16);
  put_be32(p, NET_RFU_HEADER);
  put_be32(p + 4, type);
  put_be32(p + 8, hdata);
}

static void fresh(void)
{
  rfu_reset();
  rfu_disc_grace = 0;
  rfu_disc_defer = 0;
  memset(&rfu_client, 0, sizeof(rfu_client));
  sends = 0;
}

static s32 cmd(u32 c, u32 plen, u32 w0)
{
  rfu_cmd = (u8)c;
  rfu_plen = (u8)plen;
  rfu_buf[0] = w0;
  return rfu_process_command_inner();
}

static void make_client(uint16_t host_peer, uint16_t devid, uint16_t clnum)
{
  rfu_state = RFU_STATE_CLIENT;
  memset(&rfu_client, 0, sizeof(rfu_client));
  rfu_client.host_id = host_peer;
  rfu_client.devid = devid;
  rfu_client.clnum = clnum;
}

/* 1. A host's DISCONNECT names only slots that hold a client.  The game's
 *    mask can include a slot we already freed; that used to send a notice to
 *    netplay peer 0 (an empty slot's client_id), and the client there -- if
 *    this console was its host -- went idle on a healthy link. */
static void test_disconnect_skips_empty_slots(void)
{
  uint8_t p[16];
  fresh();
  rfu_state = RFU_STATE_HOST;
  rfu_host.devid = 0x1111;
  rfu_host.clients[1].devid = 0x2222;
  rfu_host.clients[1].client_id = 3;

  assert(cmd(RFU_CMD_DISCONNECT, 1, 0xF) == 0);
  assert(sends == 1);                       /* slot 1 only */
  assert(last_dst == 3 && last_type == NET_RFU_DISCONNECT);
  assert(last_hdata == (0x2222u | (1u << 16)));
  assert(rfu_host.clients[1].devid == 0);

  /* The receiving side: a client of peer 2 ignores a notice for a device ID
   * that is not its own (an older host's empty slot 2 -> devid 0, slot 2),
   * and still honours its own and the cart's bare notice. */
  make_client(2, 0x77, 0);
  packet(p, NET_RFU_DISCONNECT, 0x00020000);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CLIENT);
  packet(p, NET_RFU_DISCONNECT, 0x00000078);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CLIENT);
  packet(p, NET_RFU_DISCONNECT, 0x00000077);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_IDLE);

  make_client(0, 0x55, 1);
  packet(p, NET_RFU_DISCONNECT, 0);          /* Mystery Gift cart */
  rfu_net_receive(p, sizeof(p), 0);
  assert(rfu_state == RFU_STATE_IDLE);
  puts("  disconnect: empty slots skipped, foreign device IDs ignored");
}

/* 2. An ACK the client can no longer use hands its slot back.  The game
 *    gives up on a connection with CONCOMPL (state -> IDLE); an ACK landing
 *    after that used to be dropped silently while the host kept the slot --
 *    a phantom player in the host game's list for 4 s. */
static void test_late_connect_ack_releases_the_slot(void)
{
  uint8_t p[16];
  fresh();
  rfu_peer_bcst[2].valid = 1;
  rfu_peer_bcst[2].device_id = 0x2468;
  assert(cmd(RFU_CMD_CONNECT, 1, 0x2468) == 0);
  assert(rfu_state == RFU_STATE_CONNECTING);
  assert(cmd(RFU_CMD_CONCOMPL, 0, 0) == 1);       /* the game gives up */
  assert(rfu_buf[0] == RFU_CONN_COMP_FAIL && rfu_state == RFU_STATE_IDLE);

  sends = 0;
  packet(p, NET_RFU_CONNECT_ACK, 0x00010077);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_IDLE);
  assert(sends == 1 && last_dst == 2 && last_type == NET_RFU_DISCONNECT);
  assert(last_hdata == 0x00010077);

  /* ... and the host that receives it frees exactly that slot. */
  rfu_state = RFU_STATE_HOST;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_host.clients[1].devid = 0x77;
  rfu_host.clients[1].client_id = 4;
  packet(p, NET_RFU_DISCONNECT, last_hdata);
  rfu_net_receive(p, sizeof(p), 4);
  assert(rfu_host.clients[1].devid == 0);

  /* A duplicate ACK for the link we hold is not a stray: no notice. */
  make_client(2, 0x77, 1);
  sends = 0;
  packet(p, NET_RFU_CONNECT_ACK, 0x00010077);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CLIENT && sends == 0);
  /* An ACK from a host we never asked is handed back. */
  packet(p, NET_RFU_CONNECT_ACK, 0x00020099);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_state == RFU_STATE_CLIENT && rfu_client.devid == 0x77);
  assert(sends == 1 && last_dst == 3 && last_hdata == 0x00020099);
  /* Malformed ACKs (no device ID, slot >= 4) are not answered. */
  sends = 0;
  packet(p, NET_RFU_CONNECT_ACK, 0x00010000);
  rfu_net_receive(p, sizeof(p), 3);
  packet(p, NET_RFU_CONNECT_ACK, 0x00040077);
  rfu_net_receive(p, sizeof(p), 3);
  assert(sends == 0);
  puts("  connect: a late or foreign ACK hands its slot back");
}

int main(void)
{
  test_disconnect_skips_empty_slots();
  test_late_connect_ack_releases_the_slot();
  puts("RFU lifecycle tests passed");
  return 0;
}
