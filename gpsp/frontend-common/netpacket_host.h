/* netpacket_host.h — frontend side of the core's retro_netpacket_callback
 * (env call 78), wired to the netdrv session driver (plan §4.3, Phase 3).
 *
 * Transport-agnostic: the platform frontend supplies an nd_transport
 * (transport_udp on desktop, transport_adhoc on PSP later) and a clock.
 * All calls single-threaded from the emu/main thread; fe_np_pump() must run
 * once per frame (it also emits the periodic `EVT net_stats` line).
 * Requires fe_host_boot() to have completed (the core registers its
 * netpacket interface during retro_init).
 */
#ifndef NETPACKET_HOST_H
#define NETPACKET_HOST_H

#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_np_config
{
   const nd_transport *transport;   /* backend vtable (copied) */
   int is_host;                     /* 1 = host (client_id 0), 0 = join */
   const char *nick;                /* may be NULL */
   uint64_t (*now_us)(void);        /* monotonic clock */
   uint32_t nonce;                  /* random per process; 0 = from clock */
   /* Debug aid for the netsmoke harness: once per second, send a reliable
    * 104-byte probe broadcast through the full ARQ path. The remote core's
    * RFU receiver drops it on magic mismatch (rfu.c:865-867) — it exists so
    * the E2E harness can assert cross-process reliable delivery (acked>0)
    * before the autopilot can drive real Union-Room traffic. */
   int probe;
} fe_np_config;

/* Separate, experimental Game Boy serial-byte datagrams. Receive and peer
 * callbacks run synchronously from the netdrv pump on the emulation thread.
 * Do not call fe_np_pump() recursively from either callback. */
typedef void (*fe_np_gb_receive_fn)(void *userdata, const void *payload,
                                    size_t len, uint8_t src_id);
typedef void (*fe_np_gb_peer_fn)(void *userdata, uint8_t peer_id,
                                 int connected);

/* Bring the session up (host immediately; join keeps retrying until the
 * host answers). Calls the core's start() when the session establishes.
 * Returns 0 on success, -1 if the core registered no netpacket interface
 * or the protocol version is missing. */
int fe_np_start(const fe_np_config *cfg);

/* GB-only session: negotiates a wire protocol string distinct from the GBA
 * netpacket protocol. Callbacks must be installed first. GBA start semantics
 * are unchanged. */
int fe_np_start_gb(const fe_np_config *cfg);
void fe_np_gb_set_receive(fe_np_gb_receive_fn receive, void *userdata);
void fe_np_gb_set_peer_callback(fe_np_gb_peer_fn peer, void *userdata);
/* True only for an active GB session with exactly one connected peer; returns
 * that peer's netdrv client ID. Refuses zero-peer/multi-peer ambiguity. */
int fe_np_gb_peer_ready(uint8_t *peer_id);
/* Query this endpoint's netdrv client ID under the same active-session guard. */
int fe_np_gb_local_id(uint8_t *local_id);
/* Reliable ordered unicast payload; caller explicitly encodes GB protocol. */
int fe_np_gb_send(uint16_t peer_id, const void *payload, size_t len);
/* Free reliable-queue slots toward the one peer (0 without one): bulk
 * senders (a cartridge sent to the partner) pace themselves on it. */
int fe_np_gb_send_room(void);
/* Reliable payloads to the GB link partner not yet acknowledged. */
int fe_np_gb_unacked(void);
/* The GB link partner's radio address (for the bulk lane).  0 on success. */
int fe_np_gb_peer_mac(uint8_t mac[6]);

/* The GB-mode channel's netdrv protocol string (exact match at JOIN), set
 * before fe_np_start_gb(): NULL = "GBAdhoc GB link v2" (the GB/GBC link).
 * A GBA link-cable room uses "GBAdhoc cable v1" (docs/GBA-LINK-UI.md), so a
 * GB console and an older build are refused at JOIN instead of half-joining.
 * A setter, not a fe_np_config field (ADR-0035, as fe_np_set_arq_timers). */
void fe_np_set_gb_protocol(const char *proto);

/* The last netdrv session_stopped reason (ND_STOP_*, 0 = none since the last
 * fe_np_start*): a joiner told ND_STOP_REFUSED was turned away by a full
 * room, ND_STOP_VERSION by the admission check. */
int fe_np_stop_reason(void);

/* A peer was turned away for its version (netdrv_wire.h, THE ADMISSION CHECK)
 * since the last call: once, on either role, for the frontend to say so.  A
 * joiner also ends with fe_np_stop_reason() == ND_STOP_VERSION. */
int fe_np_take_refused(void);
/* The last such peer this session: 0 none, 1 another version, 2 one that
 * predates the check (4.5.0 or older).  Survives the stop. */
int fe_np_refused_kind(void);

/* Short human-readable reason for the last fe_np_start() failure, for the
 * frontend to show.  Never NULL. */
const char *fe_np_start_reason(void);

/* ADR-0062: override netdrv's ARQ timers before fe_np_start(). Microseconds;
 * 0 = leave the build-time ND_* default. See the comment at the definition. */
void fe_np_set_arq_timers(uint32_t rto_first_max_us, uint32_t rto_min_us);
/* Ack riding for the next session (netdrv.h nd_config.ack_ride_us; 0 = off). */
void fe_np_set_ack_ride(uint32_t us);
/* The product value a running GBA Link Cable session rides at (0 = never);
 * not a latch: fe_np_ack_ride_session applies and removes it. */
void fe_np_set_ack_ride_product(uint32_t us);
/* The ride in force in the open session right now (0 = none / no session). */
uint32_t fe_np_ack_ride_now(void);
/* A session that runs Fast link turns ack riding on for its length (and off
 * again at its end), unless fe_np_set_ack_ride fixed a value. */
void fe_np_ack_ride_session(int on);

/* Per-frame service: pumps netdrv (delivering into the core), emits
 * net_stats. Safe to call when not started (no-op). */
void fe_np_pump(void);

/* Tear down: core stop(), BYE to peers, driver freed. */
void fe_np_stop(void);

/* A session that is starting (netdrv.h, netdrv_keepalive): keep the link
 * alive from inside a blocking stretch (a session start's game reload); a
 * live peer's silence limit, 0 = netdrv's default (8 s); frames received
 * so far (any, from anyone: "is the other side there at all").  No-ops / 0
 * with no session up. */
void fe_np_keepalive(void);
void fe_np_set_peer_dead_us(uint32_t us);
uint32_t fe_np_rx_frames(void);

/* 1 while a session is established (start() called, not stopped). */
int fe_np_session_active(void);

/* 1 once the driver has reported ND_STOP_TX_FAILED — the RELIABLE contract
 * could not be honoured, so the link is unrecoverable (ADR-0016). The
 * frontend must surface this to the user and tear the session down; the
 * silent alternative is what wedged two field consoles. Cleared by
 * fe_np_stop(). */
int fe_np_failed(void);

/* ---- peer frame-rate matching (ADR-0027) --------------------------------
 * Publish this console's recent emulated frame rate to the session, and read
 * back the slowest rate any peer has reported. Both in hundredths of a
 * frame/s (5973 = 59.73); 0 = unknown. No-ops / 0 with no session up. */
void     fe_np_set_local_fps(unsigned fps_x100);
unsigned fe_np_peer_min_fps(void);

/* ADR-0078: live reliable-TX backlog toward the peer(s) — ring + spill,
 * summed over active peers, 0 with no session.  The backpressure pacer's
 * signal; sampled once per ~1 s pace window, so the stats-struct copy is
 * noise.  See nd_stats.txq_now. */
uint32_t fe_np_txq_now(void);

/* Smoothed round trip to the peer (worst active peer) in microseconds, its
 * variance, and the RTT samples behind it; all 0 with no session or before
 * the first sample.  The auto input delay reads it once per session. */
uint32_t fe_np_rtt(uint32_t *rttvar_us, uint32_t *samples);

/* ---- per-frame session cost accounting (ADR-0021) -----------------------
 *
 * A live session cost the PSP-1000 ~20 fps and no log line could say where
 * it went; skipping 97 % of rendered frames barely helped, so it was never
 * about drawing. These are free-running totals — the caller snapshots them
 * once per heartbeat window and diffs, so nothing here divides on the hot
 * path. Times are microseconds.
 *
 * Nesting is deliberate and documented rather than subtracted out:
 *   frame_us  ⊇ everything below (one main-loop iteration, vblank wait
 *               excluded — it is the number the 16.7 ms budget is against)
 *   pump_us   = the once-per-frame fe_np_pump()
 *   poll_us   = the core's mid-frame poll_receive pumps that did work
 *   rx_us/arq_us split every pump (both kinds) into "drain + parse +
 *               deliver into the core" vs "ARQ timers, retx, keepalive,
 *               roster"; they sum to roughly pump_us + poll_us.
 * poll_n counts every poll_receive call, poll_work_n the ones that were not
 * short-circuited by netdrv_poll_needed() — their difference times clk_ns
 * is what the pre-ADR-0021 code was paying for nothing. */
typedef struct fe_np_prof
{
   uint32_t clk_ns;          /* measured cost of ONE now_us() call, in ns */
   uint32_t frames;
   uint32_t frame_us, frame_max_us;
   uint32_t pumps;           /* netdrv_pump calls that actually ran */
   uint32_t pump_us, pump_max_us;
   uint32_t poll_n, poll_work_n;
   uint32_t poll_us, poll_max_us;
   uint32_t rx_us, rx_max_us;
   uint32_t arq_us, arq_max_us;
} fe_np_prof;

void fe_np_prof_get(fe_np_prof *out);

/* Report one main-loop iteration's work time (excluding the vblank wait).
 * Costs the caller two clock reads per frame. */
void fe_np_prof_frame(uint32_t work_us);

/* ---- debug fault: swallow the core's RFU DISCONNECT notices -------------
 * A causation test, not a feature.  Same class as adhoc_transport_set_fault
 * (ADR-0017): the harness could not previously produce the adapter state the
 * field reports, so it could not tell a hypothesis from a fact.
 *
 * Gen-3's Union Room child NEVER disconnects itself — it posts a
 * LEAVE_GROUP_NOTICE and the PARENT calls rfu_REQ_disconnect for it
 * (pokeemerald src/link_rfu_2.c:1644-1670).  That parent-side DISCONNECT is
 * the ONLY thing that returns the child's emulated adapter to
 * RFU_STATE_IDLE, and rfu.c has no other route back (HOST_STOP refuses to
 * clear a populated host, HOST_START only clears slots when already IDLE,
 * and ID_RESET_REQ 0x10 / ID_STOP_MODE_REQ 0x3d are no-op ACKs).  Dropping
 * the notice therefore pins the peer's adapter in RFU_STATE_CLIENT — which
 * is exactly the state the field logs imply (host `rfu_link_down reason=4`
 * TTL expiry, client no breadcrumb at all).
 *
 * `n` = how many outgoing NET_RFU_DISCONNECT packets to swallow (0 = off).
 * Off by default and only ever armed from autopilot.ini. */
void fe_np_debug_drop_rfu_disconnect(int n);

/* Core hook: a radio peer left the session (any role).  Defined by the core
 * (libretro.c); netpacket_host.c carries a weak no-op. */
void gpsp_netpacket_peer_lost(uint16_t client_id);

#ifdef __cplusplus
}
#endif

#endif /* NETPACKET_HOST_H */
