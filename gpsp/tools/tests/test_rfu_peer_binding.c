/* White-box tests for binding RFU1 messages to the netplay peer selected by
 * the RFU handshake. Compile with function/data sections and --gc-sections so
 * the production rfu.c is tested without linking the emulator frontend. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../rfu.c"

u32 cpu_ticks;
u16 rand_gen(void) { return 0x4567; }

static unsigned sends;
static uint16_t last_dst;

void netpacket_send(uint16_t id, const void *buf, size_t len)
{
  (void)buf;
  assert(len == 16 || len == 104);
  sends++;
  last_dst = id;
}

void netpacket_send_unreliable(uint16_t id, const void *buf, size_t len)
{
  netpacket_send(id, buf, len);
}

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

static void connect_to_peer(unsigned peer, uint16_t devid)
{
  rfu_state = RFU_STATE_IDLE;
  rfu_peer_bcst[peer].valid = 1;
  rfu_peer_bcst[peer].device_id = devid;
  rfu_cmd = RFU_CMD_CONNECT;
  rfu_plen = 1;
  rfu_buf[0] = devid;
  assert(rfu_process_command_inner() == 0);
  assert(rfu_state == RFU_STATE_CONNECTING);
  assert(rfu_client.host_id == peer);
  assert(last_dst == peer);
}

static void test_handshake_binds_expected_host(void)
{
  uint8_t p[16];
  connect_to_peer(2, 0x2468);

  packet(p, NET_RFU_CONNECT_ACK, 0x00010077);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_state == RFU_STATE_CONNECTING);
  packet(p, NET_RFU_CONNECT_ACK, 0x00040077); /* invalid RFU slot */
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CONNECTING);
  packet(p, NET_RFU_CONNECT_ACK, 0x00010000); /* invalid device ID */
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CONNECTING);
  packet(p, NET_RFU_CONNECT_ACK, 0x00010077);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CLIENT);
  assert(rfu_client.host_id == 2 && rfu_client.devid == 0x77);

  connect_to_peer(1, 0x1357);
  packet(p, NET_RFU_CONNECT_NACK, 0);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_CONNECTING);
  rfu_net_receive(p, sizeof(p), 1);
  assert(rfu_state == RFU_STATE_IDLE);
}

static void test_host_only_admits_requests_for_its_device_id(void)
{
  uint8_t p[16];
  sends = 0;
  rfu_state = RFU_STATE_HOST;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_host.devid = 0x3344;

  packet(p, NET_RFU_CONNECT_REQ, 0x5566);
  rfu_net_receive(p, sizeof(p), 1);
  assert(rfu_host.clients[0].devid == 0);
  assert(sends == 0);

  packet(p, NET_RFU_CONNECT_REQ, 0x3344);
  rfu_net_receive(p, sizeof(p), 1);
  assert(rfu_host.clients[0].devid != 0);
  assert(rfu_host.clients[0].client_id == 1);
  assert(sends == 1 && last_dst == 1);
}

static void test_client_accepts_only_host_data_and_disconnect(void)
{
  uint8_t p[16];
  rfu_state = RFU_STATE_CLIENT;
  memset(&rfu_client, 0, sizeof(rfu_client));
  rfu_client.host_id = 2;
  rfu_client.devid = 0x77;
  rfu_client.clnum = 1;

  packet(p, NET_RFU_HOST_SEND, 4);
  put_be32(p + 12, 0x01020304);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_client.pkts[0].hblen == 0);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_client.pkts[0].hblen == 4);
  assert(rfu_client.pkts[0].hdata[0] == 1);

  packet(p, NET_RFU_DISCONNECT, 0x00010077);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_state == RFU_STATE_CLIENT);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_state == RFU_STATE_IDLE);
}

static void test_host_accepts_only_mapped_child(void)
{
  uint8_t p[16];
  const uint16_t devid = 0x4321;
  const uint32_t hdata = (4u << 24) | devid; /* slot 0, four payload bytes */
  rfu_state = RFU_STATE_HOST;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_host.clients[0].client_id = 2;
  rfu_host.clients[0].devid = devid;
  rfu_host.clients[0].clttl = 9;

  packet(p, NET_RFU_CLIENT_SEND, hdata);
  put_be32(p + 12, 0xA1A2A3A4);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[0].pkts[0].datalen == 0);
  assert(rfu_host.clients[0].clttl == 9);

  /* A malformed CLIENT_SEND must not fall through and masquerade as ACK. */
  packet(p, NET_RFU_CLIENT_SEND, (17u << 24) | devid);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[0].clttl == 9);

  packet(p, NET_RFU_CLIENT_SEND, hdata);
  put_be32(p + 12, 0xA1A2A3A4);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[0].pkts[0].datalen == 4);
  assert(rfu_host.clients[0].pkts[0].data[0] == 0xA1);

  rfu_host.clients[0].clttl = 9;
  packet(p, NET_RFU_CLIENT_ACK, devid);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[0].clttl == 9);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[0].clttl == 0);

  rfu_host.clients[0].clttl = 9;
  packet(p, NET_RFU_DISCONNECT, devid);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[0].devid == devid);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[0].devid == 0);
}

static void test_interleaved_host_slots_keep_peer_ownership(void)
{
  uint8_t p[16];
  const uint16_t devid0 = 0x4321;
  const uint16_t devid1 = 0x7654;
  const uint32_t slot1_send = (4u << 24) | (1u << 16) | devid1;

  rfu_state = RFU_STATE_HOST;
  memset(&rfu_host, 0, sizeof(rfu_host));
  rfu_host.clients[0].client_id = 2;
  rfu_host.clients[0].devid = devid0;
  rfu_host.clients[1].client_id = 3;
  rfu_host.clients[1].devid = devid1;
  rfu_host.clients[1].clttl = 9;

  /* Peer 2 cannot inject into peer 3's slot, even with its correct device ID. */
  packet(p, NET_RFU_CLIENT_SEND, slot1_send);
  put_be32(p + 12, 0xA1A2A3A4);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[1].pkts[0].datalen == 0);
  assert(rfu_host.clients[1].clttl == 9);

  /* The right peer can send to slot 1 while slot 0 remains independently live. */
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[1].pkts[0].datalen == 4);
  assert(rfu_host.clients[1].pkts[0].data[0] == 0xA1);
  assert(rfu_host.clients[0].pkts[0].datalen == 0);

  packet(p, NET_RFU_CLIENT_SEND, (4u << 24) | devid0);
  put_be32(p + 12, 0xB1B2B3B4);
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[0].pkts[0].datalen == 4);
  assert(rfu_host.clients[0].pkts[0].data[0] == 0xB1);
  assert(rfu_host.clients[1].pkts[0].data[0] == 0xA1);

  rfu_host.clients[1].clttl = 9;
  packet(p, NET_RFU_CLIENT_ACK, devid1 | (1u << 16));
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[1].clttl == 9);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[1].clttl == 0);

  packet(p, NET_RFU_DISCONNECT, devid1 | (1u << 16));
  rfu_net_receive(p, sizeof(p), 2);
  assert(rfu_host.clients[1].devid == devid1);
  rfu_net_receive(p, sizeof(p), 3);
  assert(rfu_host.clients[1].devid == 0);
  assert(rfu_host.clients[0].devid == devid0);
}

int main(void)
{
  test_handshake_binds_expected_host();
  test_host_only_admits_requests_for_its_device_id();
  test_client_accepts_only_host_data_and_disconnect();
  test_host_accepts_only_mapped_child();
  test_interleaved_host_slots_keep_peer_ownership();
  puts("RFU peer-binding tests passed");
  return 0;
}
