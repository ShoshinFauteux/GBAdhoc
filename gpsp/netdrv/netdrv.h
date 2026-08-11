/* netdrv.h — transport-agnostic netpacket session driver (plan §4.3, ADR-0003).
 *
 * Design truths this module is built around (docs/SERIAL-PROTO-NOTES.md):
 *   - FULL MESH: any peer unicasts to any client_id, any peer broadcasts.
 *     The RFU host role is independent of the libretro client_id.
 *   - The gpsp core sends 100% of traffic RELIABLE|FLUSH_HINT: the ARQ path
 *     carries everything; the unreliable path exists but is cold.
 *   - The core's receive path is NOT garbage-tolerant (rfu.c:735, rfu.c:845):
 *     CRC + exact-length framing here is load-bearing. Nothing that fails
 *     validation is ever delivered.
 *   - client_ids are 0..4 (host = 0, ≤4 clients) per ADR-0003.
 *   - send may be called re-entrantly from inside a deliver callback (the
 *     core sends ACKs from inside receive) — netdrv_send is safe in that
 *     context.
 *
 * Threading contract: single-threaded pump model. ALL calls into one netdrv
 * instance must come from one thread (the emu/main thread). The frontend
 * drives netdrv_pump(now_us) once per frame and from the core's
 * poll_receive callback; pump drains the transport, runs ARQ timers and
 * invokes the callbacks inline. On PSP the RX thread only feeds the
 * transport's ring; the transport recv() hook drains it on the main thread
 * (see docs/ARCHITECTURE.md, netdrv section).
 */
#ifndef NETDRV_H
#define NETDRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------- constants -- */

/* Payload budget (ADR-0016). ADR-0003 sized frames for 560 B to leave room
 * for the link-CABLE Advance Wars protocol (~520 B), which is explicitly
 * out of scope. The RFU protocol this product ships — the only one the
 * PSP/ad-hoc profile carries — maxes at 104 B (SERIAL-PROTO-NOTES §2.1),
 * and the largest control payload is the 138 B roster. The PSP build
 * therefore compiles with -DND_MAX_PAYLOAD=144, which buys a ~4x deeper
 * tx queue inside the SAME memory budget; desktop/UDP keeps 560 so the
 * cable protocols stay representable there. An oversize payload is NEVER
 * truncated: netdrv_send refuses it, counts tx_oversize and fails the
 * session loudly (ND_STOP_TX_FAILED). */
#ifndef ND_MAX_PAYLOAD
#define ND_MAX_PAYLOAD   560
#endif
#define ND_HDR_SIZE      16
#define ND_MAX_FRAME     (ND_HDR_SIZE + ND_MAX_PAYLOAD)
#define ND_MAX_CLIENTS   5     /* client_ids 0..4; host is 0 */
/* ARQ capacity (Gate-3 sizing, ADR-0010): an active RFU link runs a steady
 * ~2 payloads/frame (~120/s) with bursts.  window*RTT throughput must beat
 * that with margin even when injected jitter inflates the effective RTT, or
 * the tx ring overflows and DROPS RELIABLE PAYLOADS — observed as a silent
 * Union-Room wedge (one side's request/accept vanishes).  Window 32 at a
 * 30 ms RTT sustains ~1000/s. */
/* Both knobs are compile-time overridable (-DND_WINDOW=... -DND_TXQ_CAP=...).
 * The PSP/ad-hoc profile (psp/Makefile) pairs ND_MAX_PAYLOAD=144 with
 * window 32 / backlog 384 — a 4x deeper backlog than the Phase-4 halving
 * (96) in slightly LESS memory (ADR-0016). Desktop keeps the Gate-3
 * sizing at the 560 B payload budget. */
#ifndef ND_WINDOW
#define ND_WINDOW        32    /* ARQ sliding window (in-flight frames) */
#endif
#ifndef ND_TXQ_CAP
#define ND_TXQ_CAP       192   /* per-peer reliable backlog (incl. window) */
#endif
/* Spill: an unbounded-in-practice overflow list behind the fixed ring, so a
 * transient stall can NEVER discard a RELIABLE payload (ADR-0016). Nodes
 * are malloc'd per payload and freed on ack; this cap only exists to turn
 * a runaway into a loud, explicit session failure instead of unbounded
 * memory growth. */
#ifndef ND_SPILL_MAX
#define ND_SPILL_MAX     512   /* payloads per peer beyond ND_TXQ_CAP */
#endif
#define ND_NICK_LEN      16
#define ND_PROTO_LEN     24    /* protocol_version match buffer ("gpSP v1.0") */

/* Adaptive RTO (ADR-0017). Per peer we run a TCP-style estimator —
 * SRTT/RTTVAR, RTO = SRTT + 4*RTTVAR, Karn's algorithm (never sample a
 * retransmitted packet), exponential backoff per retransmit — clamped
 * between a per-TRANSPORT floor and ceiling:
 *
 *   desktop/UDP : floor 30 ms  — ADR-0010's load-bearing value. Loopback
 *                 SRTT is ~1 ms, so the floor governs and behaviour is
 *                 bit-for-bit the Gate-3 timing.
 *   PSP/ad-hoc  : floor 100 ms — real radio RTT is 40-80 ms (field logs,
 *                 docs/HANDOFF.md issue #2). A 30 ms floor there fires
 *                 before an ACK can physically arrive: the measured storm
 *                 was retx/acked 2.8 with 68 % of RX being duplicates, on
 *                 a link that lost NOTHING (txfail=0).
 *
 * The floor only bounds the estimator from below; a link with an honest
 * 40 ms RTT settles at RTO ~= 60-100 ms on its own. */
#ifndef ND_RTO_MIN_US
#define ND_RTO_MIN_US     30000     /* RTO floor (transport constant) */
#endif
#ifndef ND_RTO_MAX_US
#define ND_RTO_MAX_US     240000    /* RTO ceiling, incl. backoff */
#endif
/* Cap on the FIRST retransmission of a slot — the loss-RECOVERY deadline,
 * as opposed to the RTO, which is the pacing estimate. They are different
 * problems and this core forces us to treat them separately:
 *
 *   The emulated RFU exchange dies if a lost frame takes too long to come
 *   back (ADR-0010 measured the cliff at ~60 ms on the desktop rig). An
 *   honest RTO on a link whose RTT is 30-80 ms is necessarily LARGER than
 *   that, so on a genuinely lossy link an honest RTO is too slow — the old
 *   fixed 30 ms timer worked precisely because it retransmitted blindly,
 *   before an ACK could arrive. That redundancy hid loss.
 *
 * So each transport declares how fast a lost frame must be recovered:
 *   desktop/UDP : 30 ms (sdl/Makefile) — restores the Gate-3 behaviour
 *                 exactly on a rig whose 5 % injected loss has no layer
 *                 underneath it to repair anything.
 *   PSP/ad-hoc  : unset — 802.11 already retransmits below us (the field
 *                 logs show txfail=0 / drop_crc=0: the radio lost nothing),
 *                 so the estimator governs and we stop generating the
 *                 duplicate storm that killed the field session.
 * Backoff after the first retransmit always uses the adaptive RTO. */
#ifndef ND_RTO_FIRST_MAX_US
#define ND_RTO_FIRST_MAX_US ND_RTO_MAX_US   /* default: no extra constraint */
#endif
#define ND_RETX_US        ND_RTO_MIN_US   /* legacy names (nd_config maps) */
#define ND_RETX_MAX_US    ND_RTO_MAX_US
#define ND_KEEPALIVE_US   250000    /* ping an idle peer every ~250ms */
#define ND_PEER_DEAD_US   8000000   /* declare dead after ~8s of silence
                                       (generous: loaded CI hosts stall whole
                                       processes for seconds; real radio has
                                       dropouts — plan risk #5) */
#define ND_JOIN_RETRY_US  500000    /* JOIN broadcast retry */
#define ND_ROSTER_US      1000000   /* host roster refresh broadcast */
/* ADR-0027: how often each side reports its emulated frame rate to a peer.
 * PING is the carrier and the keepalive PING is suppressed whenever we sent
 * anything else, so during an active trade (DATA every frame) it would never
 * fire — this is a SEPARATE cadence that runs regardless of other traffic.
 * One 18-byte datagram per peer per 500 ms is ~36 B/s against a session that
 * already moves ~65 packets/s each way. */
#define ND_FPS_REPORT_US  500000

/* Send flags — mirror RETRO_NETPACKET_* numerically (libretro.h:3082-3086)
 * so the frontend can pass the core's flags through unchanged. */
#define ND_UNRELIABLE   0
#define ND_RELIABLE     (1 << 0)
#define ND_UNSEQUENCED  (1 << 1)
#define ND_FLUSH_HINT   (1 << 2)

#define ND_BROADCAST_ID 0xFFFF   /* == RETRO_NETPACKET_BROADCAST */

/* session_stopped reasons */
enum
{
   ND_STOP_HOST_LOST = 1,   /* keepalive timeout on the host link */
   ND_STOP_HOST_BYE  = 2,   /* host sent BYE */
   ND_STOP_REFUSED   = 3,   /* host refused our JOIN (session full / admission) */
   ND_STOP_LOCAL     = 4,   /* local netdrv_leave() */
   ND_STOP_EVICTED   = 5,   /* host dropped us from the roster */
   ND_STOP_TX_FAILED = 6    /* LOCAL, unrecoverable: a RELIABLE payload could
                               not be queued (spill exhausted / OOM) or was
                               oversize. The RELIABLE contract cannot be
                               honoured, so the session fails LOUDLY instead
                               of silently corrupting the core's RFU state
                               (ADR-0016). Frontends surface this to the
                               user; it is never a normal disconnect. */
};

/* ---------------------------------------------------- transport interface -- */

/* ~6-function transport API (plan §4.3). Addresses are 6-byte MACs; the UDP
 * backend fakes them from ip:port (4B IPv4 + 2B port). init/term live in
 * each backend's own header (transport_udp.h / transport_adhoc.h) since
 * their parameters differ; this vtable is what netdrv consumes.
 * All hooks are non-blocking and called from the pump thread only. */
typedef struct nd_transport
{
   void *ctx;
   /* Send one frame to a specific peer. Returns 0 on success (queued/sent),
    * <0 on hard error. Loss is fine — ARQ owns reliability. */
   int  (*send_to)(void *ctx, const uint8_t mac[6], const void *buf, size_t len);
   /* Send one frame to every reachable peer. */
   int  (*broadcast)(void *ctx, const void *buf, size_t len);
   /* Fetch one pending frame. Returns payload length >0, 0 if none pending,
    * <0 on hard error. cap is the buffer capacity (>= ND_MAX_FRAME). */
   int  (*recv)(void *ctx, uint8_t src_mac[6], void *buf, size_t cap);
   /* Local address as a fake/real MAC. */
   void (*local_addr)(void *ctx, uint8_t mac[6]);
   /* OPTIONAL (may be NULL) — and because it is optional, every backend's
    * *_transport_iface() MUST zero this struct before filling it: callers
    * pass uninitialised stack storage, and a garbage "optional" hook is a
    * call through a stack value.
    *
    * Nonzero if recv() would return a frame right
    * now. Must be CHEAP — netdrv_poll_needed() calls it on the core's
    * mid-frame poll_receive path, which runs hundreds of times per emulated
    * frame (ADR-0021). A transport that cannot answer without syscalls
    * leaves it NULL, and netdrv then assumes "yes" (the pre-ADR-0021
    * behaviour: every poll pumps). */
   int  (*pending)(void *ctx);
} nd_transport;

/* ------------------------------------------------------------ callbacks -- */

typedef struct nd_callbacks
{
   void *user;
   /* Validated in-order payload from peer src_id -> core receive(). */
   void (*deliver)(void *user, const void *buf, size_t len, uint8_t src_id);
   /* Session is up; local_id is our client_id (0 = we are host). */
   void (*session_started)(void *user, uint8_t local_id);
   /* Peer appeared in the session. Return 0 to accept; nonzero to refuse
    * (host side admission -> peer is removed and told BYE). */
   int  (*peer_connected)(void *user, uint8_t id);
   void (*peer_disconnected)(void *user, uint8_t id);
   /* Session torn down for a remote reason (ND_STOP_*). Not fired for a
    * local netdrv_leave(). */
   void (*session_stopped)(void *user, int reason);
   /* Optional debug log line (already formatted, no newline). May be NULL. */
   void (*log)(void *user, const char *line);
} nd_callbacks;

/* ---------------------------------------------------------------- stats -- */

typedef struct nd_stats
{
   uint32_t tx_frames, rx_frames;        /* all frames on the wire */
   uint32_t tx_bytes,  rx_bytes;
   uint32_t tx_data,   rx_data;          /* reliable DATA frames */
   uint32_t retx;                        /* retransmissions */
   uint32_t delivered_rel;               /* payloads handed to deliver() */
   uint32_t delivered_unrel;
   uint32_t acked;                       /* our reliable payloads acked by peers */
   /* ADR-0059: standalone ACK frames out and in.
    *
    * The client retransmits ~31x more per payload than the host (122 vs 3.9 per
    * 1000), and ~59% of those arrive as rx_dup at a host that ALREADY HAD the
    * payload -- so the data got through and the acknowledgement did not come
    * back inside the 200ms RTO.  Nothing in nd_stats could say whether those
    * acks were sent late or sent and lost: tx_frames lumps DATA, ACK, PING and
    * JOIN together, and tx_data counts only DATA.
    *
    * Counting ACK frames on both ends separates the two: if a console's tx_ack
    * roughly matches its peer's rx_ack, acks are being SENT and LOST on air; if
    * tx_ack itself is low, they are not being sent often enough.  One counter
    * each, incremented where the frames are already being emitted and parsed. */
   uint32_t tx_ack, rx_ack;              /* standalone ND_T_ACK frames */
   /* ADR-0061: how long BETWEEN pumps, not how long a pump takes.
    *
    * At 59.73 fps the host measured srtt=36ms and rto=200ms, yet retx_age was
    * 3063ms mean / 10443ms max -- packets resent 15x later than the RTO. Either
    * the ARQ scan is not running (starvation: netdrv_pump is driven once per
    * emulated frame, so a long frame IS a long gap), or the scan runs fine and
    * the window is head-of-line blocked. Both predict a huge retx_age and the
    * existing counters cannot tell them apart. This one can: gaps of seconds
    * mean starvation, gaps of ~16ms with packets still aging mean the window. */
   uint32_t pump_gap_max_us, pump_gap_sum_ms, pump_gap_n;
   uint32_t rx_dup;                      /* duplicate/stale DATA (ARQ dedupe):
                                            seq BEHIND rx_expect. This is the
                                            counter that prices the peer's
                                            retransmissions as spurious --
                                            pair it against the PEER's retx. */
   uint32_t rx_beyond_win;               /* DATA >= ND_WINDOW ahead of
                                            rx_expect. A sender that respects
                                            the window cannot produce this, so
                                            nonzero means the window contract
                                            is broken, NOT that we saw a
                                            duplicate. Split out of rx_dup
                                            because the two were conflated and
                                            a spurious-retx rate computed from
                                            the sum is not a rate at all. */
   uint32_t rx_drop_malformed;           /* bad magic/ver/len/type/ids */
   uint32_t rx_drop_crc;                 /* CRC16 mismatch */
   uint32_t rx_drop_unknown_peer;        /* data from mac not in roster */
   uint32_t rx_drop_stale;               /* unreliable drop-stale path */
   uint32_t tx_overflow;                 /* RELIABLE payload lost toward a LIVE
                                            peer. Since ADR-0016 this can only
                                            be reached after the spill list is
                                            also exhausted, and it is always
                                            accompanied by ND_STOP_TX_FAILED —
                                            never a silent drop. Harnesses
                                            assert it stays 0. */
   uint32_t tx_drop_dead;                /* reliable queue full toward a peer
                                            silent > 2x keepalive — a
                                            disconnection in progress; drops
                                            are inevitable at any queue depth
                                            and void once the peer dies */
   uint32_t tx_spill;                    /* payloads parked in the spill list
                                            because the fixed ring was full
                                            (backpressure absorbed, no loss) */
   uint32_t tx_oversize;                 /* send refused: len > ND_MAX_PAYLOAD
                                            (fatal — never truncated) */
   uint32_t txq_hiwater;                 /* max queued payloads seen on any one
                                            peer (ring + spill) */
   /* ADR-0078: the LIVE depth, not the high-water — summed ring + spill
    * across active peers at the moment of the netdrv_get_stats call.  This
    * is the backpressure signal: unacked reliable payloads accumulate here
    * exactly when the peer stops absorbing them, which makes it a direct
    * measurement of the receiver's sustainable rate where the ADR-0027 EMA
    * was an estimate of it. */
   uint32_t txq_now;
   /* Live ARQ timing, worst (largest) across active peers — 0 with no peers.
    * srtt_us is 0 until the first Karn-valid RTT sample lands. */
   uint32_t srtt_us, rto_us;
   /* rttvar_us is the other half of RFC 6298: rto = srtt + 4*rttvar, then
    * clamped to [floor, ceiling]. Without it a report of rto_us == the
    * ceiling is ambiguous -- it cannot be told from a computed value that
    * happens to land there, so it cannot be told whether the clamp is
    * truncating a legitimately larger RTO and firing retransmits early. */
   uint32_t rttvar_us;
   /* Age of a payload (first transmission -> now) at the moment we resend it.
    * If retransmits fire at ~rto the timer is behaving and the ack was late or
    * lost; if they fire far later the scan itself is being starved. */
   uint32_t retx_age_max_us, retx_age_sum_ms, retx_age_cnt;
   /* Deepest the receive reorder buffer got. A large value means a gap is
    * holding delivery while successors pile up behind it -- head-of-line
    * blocking, which the emulated RFU sees as a stall, not as loss. */
   uint32_t reorder_hi;
   uint32_t rtt_samples;
   /* Profiling (ADR-0021), free-running totals; only advance when
    * nd_config.prof_us is set. The frontend diffs them per heartbeat window
    * to price the pump. rx = transport drain + parse + deliver-into-core
    * (steps 1 and 1b of netdrv_pump); arq = the timer/retx/keepalive/roster
    * section (step 2). pumps counts netdrv_pump calls that ran. */
   uint32_t prof_pumps;
   uint32_t prof_rx_us, prof_arq_us;
   uint32_t prof_rx_max_us, prof_arq_max_us;
} nd_stats;

/* --------------------------------------------------------------- config -- */

typedef struct nd_config
{
   /* Exact string matched during session handshake; use the core's
    * protocol_version ("gpSP v1.0"). Truncated at ND_PROTO_LEN-1. */
   const char *protocol;
   const char *nick;              /* truncated at ND_NICK_LEN-1; may be NULL */
   uint32_t    join_nonce;        /* nonzero random per process instance;
                                     lets peers detect a rejoin and reset ARQ */
   /* Timing overrides in us; 0 = default (ND_*_US above).
    * retx_us / retx_max_us are the adaptive-RTO floor / ceiling;
    * rto_first_max_us is the first-retransmission (recovery) cap. */
   uint32_t retx_us, retx_max_us, keepalive_us, peer_dead_us,
            join_retry_us, roster_us, rto_first_max_us;
   /* Spill cap override (payloads per peer); 0 = ND_SPILL_MAX. Tests drive
    * this to force the unrecoverable path deterministically. */
   uint32_t spill_max;
   /* OPTIONAL profiling clock (ADR-0021). When non-NULL netdrv_pump splits
    * its own cost into nd_stats.prof_rx_us / prof_arq_us. Costs 4 extra
    * clock reads per pump, so it is only sane once the pump is called ~once
    * per frame rather than once per scanline — which is the point of
    * netdrv_poll_needed(). NULL = no profiling, zero overhead. */
   uint64_t (*prof_us)(void);
} nd_config;

/* ------------------------------------------------------------------ API -- */

typedef struct netdrv netdrv;

/* Allocate a driver bound to a transport. Copies cfg/cb/tp by value. */
netdrv *netdrv_create(const nd_transport *tp, const nd_callbacks *cb,
                      const nd_config *cfg);
void netdrv_destroy(netdrv *nd);

/* Become session host (local client_id 0). session_started fires from
 * inside this call. Returns 0 on success. */
int netdrv_host(netdrv *nd);

/* Start joining: broadcasts JOIN until WELCOME (session_started fires from
 * a later pump) or until the caller gives up and calls netdrv_leave. */
int netdrv_join(netdrv *nd);

/* Leave the session: BYE to all peers, state reset. No callbacks fired. */
void netdrv_leave(netdrv *nd);

/* Drive the driver: drains transport, delivers payloads, runs ARQ retx,
 * keepalive, death detection, roster/JOIN timers. Call once per frame and
 * from the core's poll_receive. now_us must be monotonic. Nested calls
 * (re-entry from a callback) are detected and return immediately. */
void netdrv_pump(netdrv *nd, uint64_t now_us);

/* Would a pump right now do anything at all? (ADR-0021)
 *
 * The core calls poll_receive from update_serial(), i.e. on every video
 * event while its RFU sits in WAITEVENT — ~450+ times per emulated frame,
 * ~27,000 times a second. Pumping unconditionally there means 27,000
 * monotonic-clock syscalls plus 27,000 window scans a second on the
 * emulation thread; on a PSP-1000 that is milliseconds per frame, and a
 * vblank-locked loop pays for a millisecond over budget with a whole
 * 16.7 ms frame. This predicate is the cheap gate in front of that: it
 * takes no clock and no syscall, and answers yes only when a datagram is
 * waiting or we have ARQ payload queued that has not gone out yet.
 *
 * Answering yes when idle is always CORRECT, only wasteful — a transport
 * with no pending() hook keeps the old behaviour exactly. */
int netdrv_poll_needed(const netdrv *nd);

/* Send a payload (from the core's send_fn — re-entrant safe). flags are
 * ND_ / RETRO_NETPACKET_ values; dst_id is a client_id or ND_BROADCAST_ID.
 * Reliable broadcast fans out per-peer over ARQ (ADR-0003).
 * Returns 0 on success/queued, -1 on error (oversize, no session, bad dst).
 *
 * RELIABLE payloads are never discarded: if the fixed ring is full the
 * payload goes to the peer's spill list (backpressure, still ordered). The
 * only -1 cases toward a live peer are oversize and a genuinely exhausted
 * spill — both raise ND_STOP_TX_FAILED on the next pump so the caller can
 * fail the session visibly rather than run on a corrupted link. */
int netdrv_send(netdrv *nd, int flags, const void *buf, size_t len,
                uint16_t dst_id);

/* Free reliable-queue slots toward dst_id (min across peers for
 * ND_BROADCAST_ID). Lets a frontend-level sender pace itself instead of
 * hitting the tx_overflow drop; the core itself is naturally paced by the
 * RFU request/response rhythm. */
int netdrv_send_capacity(const netdrv *nd, uint16_t dst_id);

/* Introspection */
int  netdrv_active(const netdrv *nd);        /* session up? */
int  netdrv_local_id(const netdrv *nd);      /* -1 if no session */
int  netdrv_peer_count(const netdrv *nd);    /* connected remote peers */
void netdrv_get_stats(const netdrv *nd, nd_stats *out);

/* ---- peer frame-rate matching (ADR-0027) --------------------------------
 *
 * Two real GBAs both run at 59.7275 Hz, so their link timing is mutually
 * consistent; gen-3's RFU counts link timeouts in FRAMES, not seconds. Two
 * emulators running 20 % apart are not consistent, and the slower side's
 * game times out. So each side publishes its own recent emulated frame rate
 * and the faster side paces itself down to match. netdrv only carries the
 * number — the pacing policy lives in the frontend.
 *
 * Units are hundredths of a frame per second (5973 = 59.73 fps), which fits
 * a uint16 with room to spare and needs no float on the wire. 0 means
 * "unknown / not reporting" in both directions, so a peer running an older
 * build (whose PING has no payload) is simply invisible to the policy and
 * everything degrades to pre-ADR-0027 behaviour. */

/* Publish this console's rate. Cheap; call once per measurement window.
 * 0 disables reporting (PING goes back to a bare keepalive). */
void netdrv_set_local_fps(netdrv *nd, unsigned fps_x100);

/* Slowest ACTIVE peer that has reported a rate, in hundredths.
 * 0 = no peer has reported one (no session, or all peers are silent/old). */
unsigned netdrv_peer_min_fps(const netdrv *nd);

#ifdef __cplusplus
}
#endif

#endif /* NETDRV_H */
