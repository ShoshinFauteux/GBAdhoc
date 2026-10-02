/* Focused frontend test for the GB-only netpacket route and negotiation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../frontend-common/netpacket_host.h"
#include "../libretro/libretro-common/include/libretro.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

struct netdrv
{
   int active;
   int peers;
   int local_id;
};

static struct netdrv fake_nd;
static nd_callbacks fake_cb;
static char negotiated[ND_PROTO_LEN];
static int last_flags;
static uint16_t last_dst;
static size_t last_len;
static uint8_t last_payload[ND_MAX_PAYLOAD];
static int gb_rx_count, gb_peer_up, gb_peer_down, gba_rx_count, gba_start_count;
static uint8_t gb_rx_src;
static uint64_t fake_now(void) { return 1000000; }

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }

static void gba_start(uint16_t id, retro_netpacket_send_t send,
                      retro_netpacket_poll_receive_t poll)
{
   (void)id; (void)send; (void)poll; gba_start_count++;
}
static void gba_receive(const void *buf, size_t len, uint16_t src)
{
   (void)buf; (void)len; (void)src; gba_rx_count++;
}
static void gba_stop(void) { }
static struct retro_netpacket_callback gba_iface = {
   gba_start, gba_receive, gba_stop, NULL, NULL, NULL, "gpSP v1.0"
};
const void *fe_host_netpacket_cb(void) { return &gba_iface; }

netdrv *netdrv_create(const nd_transport *tp, const nd_callbacks *cb,
                      const nd_config *cfg)
{
   (void)tp;
   memset(&fake_nd, 0, sizeof(fake_nd));
   fake_cb = *cb;
   snprintf(negotiated, sizeof(negotiated), "%s", cfg->protocol);
   return &fake_nd;
}
void netdrv_destroy(netdrv *nd) { (void)nd; }
int netdrv_host(netdrv *nd)
{
   nd->active = 1; nd->local_id = 0;
   if (fake_cb.session_started) fake_cb.session_started(fake_cb.user, 0);
   return 0;
}
int netdrv_join(netdrv *nd)
{
   nd->active = 1; nd->local_id = 3;
   if (fake_cb.session_started) fake_cb.session_started(fake_cb.user, 3);
   return 0;
}
void netdrv_leave(netdrv *nd) { nd->active = 0; nd->peers = 0; }
void netdrv_pump(netdrv *nd, uint64_t now_us)
{
   static const char payload[] = "test";
   (void)now_us;
   if (fake_cb.deliver) fake_cb.deliver(fake_cb.user, payload,
                                        sizeof(payload) - 1, 1);
   (void)nd;
}
int netdrv_poll_needed(const netdrv *nd) { (void)nd; return 0; }
int netdrv_send(netdrv *nd, int flags, const void *buf, size_t len,
                uint16_t dst_id)
{
   (void)nd;
   last_flags = flags; last_dst = dst_id; last_len = len;
   if (len <= sizeof(last_payload)) memcpy(last_payload, buf, len);
   return 0;
}
int netdrv_send_capacity(const netdrv *nd, uint16_t dst_id)
{ (void)nd; (void)dst_id; return 32; }
int netdrv_unacked(const netdrv *nd, uint16_t dst_id)
{ (void)nd; (void)dst_id; return 0; }
int netdrv_peer_mac(const netdrv *nd, uint16_t id, uint8_t mac[6])
{ (void)nd; (void)id; memset(mac, 0, 6); return 0; }
int netdrv_active(const netdrv *nd) { return nd->active; }
int netdrv_local_id(const netdrv *nd) { return nd->active ? nd->local_id : -1; }
int netdrv_peer_count(const netdrv *nd) { return nd->peers; }
void netdrv_get_stats(const netdrv *nd, nd_stats *out)
{ (void)nd; memset(out, 0, sizeof(*out)); }
unsigned netdrv_peer_min_fps(const netdrv *nd) { (void)nd; return 0; }
void netdrv_set_local_fps(netdrv *nd, unsigned fps) { (void)nd; (void)fps; }

static void gb_receive(void *userdata, const void *payload, size_t len,
                       uint8_t src_id)
{
   (void)userdata;
   if (len == 4 && memcmp(payload, "test", 4) == 0)
   {
      gb_rx_count++;
      gb_rx_src = src_id;
   }
}
static void gb_peer(void *userdata, uint8_t peer_id, int connected)
{
   (void)userdata;
   if (peer_id == 1 && connected) gb_peer_up++;
   if (peer_id == 1 && !connected) gb_peer_down++;
}

int main(void)
{
   nd_transport transport;
   fe_np_config cfg;
   uint8_t local_id, peer_id;
   const char payload[] = "GBL1-byte";
   memset(&transport, 0, sizeof(transport));
   memset(&cfg, 0, sizeof(cfg));
   cfg.transport = &transport;
   cfg.is_host = 1;
   cfg.now_us = fake_now;

   fe_np_gb_set_receive(gb_receive, NULL);
   fe_np_gb_set_peer_callback(gb_peer, NULL);
   CHECK(fe_np_start_gb(&cfg) == 0);
   CHECK(strcmp(negotiated, "GBAdhoc GB link v2") == 0);
   CHECK(fe_np_gb_local_id(&local_id) && local_id == 0);
   CHECK(!fe_np_gb_peer_ready(&peer_id)); /* host has no peer yet */

   fake_nd.peers = 1;
   CHECK(fake_cb.peer_connected(fake_cb.user, 1) == 0);
   CHECK(gb_peer_up == 1);
   CHECK(fe_np_gb_peer_ready(&peer_id) && peer_id == 1);
   CHECK(fe_np_gb_send(peer_id, payload, sizeof(payload) - 1) == 0);
   CHECK(last_dst == 1 && last_len == sizeof(payload) - 1);
   CHECK(last_flags == (ND_RELIABLE | ND_FLUSH_HINT));
   CHECK(memcmp(last_payload, payload, last_len) == 0);
   fe_np_pump();
   CHECK(gb_rx_count == 1 && gb_rx_src == 1);
   CHECK(gba_rx_count == 0); /* GB payload never reaches GBA RFU */

   fake_nd.peers = 2;
   CHECK(fake_cb.peer_connected(fake_cb.user, 2) != 0);
   CHECK(gb_peer_up == 1); /* rejected client never enters peer state */
   fake_nd.peers = 1;
   CHECK(fe_np_gb_peer_ready(&peer_id) && peer_id == 1);
   fake_cb.peer_disconnected(fake_cb.user, 1);
   fake_nd.peers = 0;
   CHECK(gb_peer_down == 1 && !fe_np_gb_peer_ready(&peer_id));
   fe_np_stop();

   /* Local leave also notifies the GB link coordinator; netdrv_leave itself
    * deliberately emits no peer callbacks. */
   CHECK(fe_np_start_gb(&cfg) == 0);
   fake_nd.peers = 1;
   CHECK(fake_cb.peer_connected(fake_cb.user, 1) == 0);
   fe_np_stop();
   CHECK(gb_peer_down == 2);

   /* The original GBA start path retains its own negotiated core version and
    * routes received payloads to the libretro netpacket receive callback. */
   CHECK(fe_np_start(&cfg) == 0);
   CHECK(strcmp(negotiated, "gpSP v1.0") == 0);
   CHECK(gba_start_count == 1);
   fake_nd.peers = 1;
   fake_cb.deliver(fake_cb.user, payload, sizeof(payload) - 1, 1);
   CHECK(gba_rx_count == 1 && gb_rx_count == 1);
   fe_np_stop();

   puts("gb netpacket host: protocol routing/readiness passed");
   return 0;
}
