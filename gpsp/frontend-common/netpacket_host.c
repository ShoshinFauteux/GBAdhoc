/* netpacket_host.c — glue between netdrv and the core's netpacket interface.
 * See netpacket_host.h. Contract details: docs/SERIAL-PROTO-NOTES.md §1/§6,
 * plan §4.3 + Appendix A, ADR-0003.
 */
#include "netpacket_host.h"

#include <stdio.h>
#include <string.h>

#include "fe_evt.h"
#include "fe_host.h"
#include "libretro.h"

#define NP_STATS_INTERVAL_US 5000000ull   /* EVT net_stats every ~5 s */
#define NP_PROBE_INTERVAL_US 1000000ull

static struct
{
   int active;                      /* fe_np_start succeeded, not stopped */
   int core_started;                /* core start() called (session up) */
   const struct retro_netpacket_callback *rcb;
   netdrv *nd;
   uint64_t (*now_us)(void);
   int probe;
   uint64_t last_stats_us, last_probe_us;
   uint32_t probe_seq;
   uint32_t core_tx, core_rx;       /* payloads core<->driver */
   uint32_t core_tx_fail;
   int      failed;                 /* ND_STOP_TX_FAILED seen (see header) */
   fe_np_prof prof;                 /* ADR-0021 */
} np;

/* ------------------------------------------------------------ profiling -- */

static void prof_bump(uint32_t *sum, uint32_t *max, uint64_t d)
{
   if (d > 0xFFFFFFFFull)
      d = 0xFFFFFFFFull;
   *sum += (uint32_t)d;
   if ((uint32_t)d > *max)
      *max = (uint32_t)d;
}

/* Price one clock read, once, at session start. The instrumentation itself
 * has to be auditable: everything below is expressed in clock reads, so a
 * field log carries the constant that converts them to microseconds. */
static void prof_calibrate_clock(void)
{
   enum { N = 256 };
   uint64_t a, b;
   int i;
   np.prof.clk_ns = 0;
   if (!np.now_us)
      return;
   a = np.now_us();
   for (i = 0; i < N; i++)
      (void)np.now_us();
   b = np.now_us();
   np.prof.clk_ns = (uint32_t)((b - a) * 1000ull / (uint64_t)N);
}

void fe_np_prof_get(fe_np_prof *out)
{
   *out = np.prof;
}

void fe_np_prof_frame(uint32_t work_us)
{
   np.prof.frames++;
   prof_bump(&np.prof.frame_us, &np.prof.frame_max_us, work_us);
}

/* ------------------------------------------------ core -> driver (send) -- */

/* Debug fault (see netpacket_host.h).  Counts down; 0 = disarmed. */
static int np_drop_rfu_disc;

void fe_np_debug_drop_rfu_disconnect(int n)
{
   np_drop_rfu_disc = n;
   fe_evt("rfu_fault drop_disconnect=%d", n);
}

/* The RFU wire format is big-endian: word 0 is the "RFU1" magic and word 1
 * the message type (rfu.c rfu_net_send_cmd).  NET_RFU_DISCONNECT is 0x04. */
static int np_is_rfu_disconnect(const void *buf, size_t len)
{
   const unsigned char *b = (const unsigned char *)buf;
   return len >= 12 &&
          b[0] == 0x52 && b[1] == 0x46 && b[2] == 0x55 && b[3] == 0x31 &&
          b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0x04;
}

static void np_send_fn(int flags, const void *buf, size_t len,
                       uint16_t client_id)
{
   /* Re-entrant by contract: the core calls this from inside receive()
    * (rfu.c CONNECT_ACK / CLIENT_ACK paths) — netdrv_send is safe there. */
   if (!np.core_started || !np.nd)
      return;
   if (np_drop_rfu_disc > 0 && np_is_rfu_disconnect(buf, len))
   {
      np_drop_rfu_disc--;
      fe_evt("rfu_fault swallowed=disconnect to=%u left=%d",
             client_id, np_drop_rfu_disc);
      return;
   }
   np.core_tx++;
   if (netdrv_send(np.nd, flags, buf, len, client_id) != 0)
      np.core_tx_fail++;
}

static void np_poll_receive_fn(void)
{
   /* Core polls mid-frame while its RFU sits in WAITEVENT — from
    * update_serial(), i.e. on every video event, ~450+ times per emulated
    * frame (rfu.c:936-950, main.c:143). Draining and delivering inline on
    * this (emu) thread is right; doing it unconditionally was not. Every
    * one of those calls used to cost a monotonic-clock syscall plus a full
    * pump (transport probe, per-peer ack scan, per-peer ARQ window scan)
    * to discover there was nothing to do — milliseconds per frame on a
    * PSP-1000, which a vblank-locked loop pays for in whole 16.7 ms frames.
    * netdrv_poll_needed() is the syscall-free gate in front of that; when
    * it says yes we do exactly what we always did. (ADR-0021) */
   uint64_t t0;
   if (!np.nd || !np.now_us)
      return;
   np.prof.poll_n++;
   if (!netdrv_poll_needed(np.nd))
      return;
   np.prof.poll_work_n++;
   t0 = np.now_us();
   netdrv_pump(np.nd, t0);          /* one clock read serves both uses */
   prof_bump(&np.prof.poll_us, &np.prof.poll_max_us, np.now_us() - t0);
}

/* ------------------------------------------------ driver -> core events -- */

static void np_deliver(void *user, const void *buf, size_t len, uint8_t src)
{
   (void)user;
   np.core_rx++;
   if (np.core_started && np.rcb->receive)
      np.rcb->receive(buf, len, src);
}

static void np_session_started(void *user, uint8_t local_id)
{
   (void)user;
   if (np.core_started)
      return;
   np.core_started = 1;
   np.rcb->start(local_id, np_send_fn, np_poll_receive_fn);
   fe_evt("session_start id=%u peers=%d", local_id,
          netdrv_peer_count(np.nd));
}

static int np_peer_connected(void *user, uint8_t id)
{
   (void)user;
   fe_evt("peer_connected id=%u", id);
   /* connected()/disconnected() are host-side-only calls per the API
    * (libretro.h:3155-3165, SERIAL-PROTO-NOTES §6); on the host they are
    * the admission gate. */
   if (netdrv_local_id(np.nd) == 0 && np.rcb->connected)
   {
      if (!np.rcb->connected(id))
      {
         fe_evt("peer_refused id=%u", id);
         return -1;                 /* netdrv removes the peer + refuses */
      }
   }
   return 0;
}

static void np_peer_disconnected(void *user, uint8_t id)
{
   (void)user;
   fe_evt("peer_disconnected id=%u", id);
   if (netdrv_local_id(np.nd) == 0 && np.rcb->disconnected)
      np.rcb->disconnected(id);
}

static void np_session_stopped(void *user, int reason)
{
   (void)user;
   if (reason == ND_STOP_TX_FAILED)
   {
      /* The driver could not honour the RELIABLE contract. Historically
       * this was a silent payload drop that corrupted the core's RFU state
       * and surfaced minutes later as the game's own "communication error"
       * with nothing in our logs (docs/HANDOFF.md issue #2). It is now an
       * explicit, user-visible failure. */
      nd_stats st;
      netdrv_get_stats(np.nd, &st);
      np.failed = 1;
      fe_evt("net_error reason=txq_overflow overflow=%u spill=%u txq_hi=%u "
             "srtt_us=%u rto_us=%u", st.tx_overflow, st.tx_spill,
             st.txq_hiwater, st.srtt_us, st.rto_us);
   }
   fe_evt("session_stop reason=%d", reason);
   if (np.core_started)
   {
      np.core_started = 0;
      if (np.rcb->stop)
         np.rcb->stop();
   }
}

static void np_log(void *user, const char *line)
{
   (void)user;
   fe_log("netdrv: %s", line);
}

/* ------------------------------------------------------------------- API -- */

/* ADR-0062: ARQ timers, harness-overridable so the loss-recovery deadline can
 * be swept on hardware without a rebuild.  0 = the build-time default.
 *
 *   rto_first_max_us -- cap on the FIRST retransmission of a slot, i.e. the
 *     loss-RECOVERY deadline as distinct from the RTO pacing estimate
 *     (netdrv.h:102-124).  The PSP build leaves it unset (= ND_RTO_MAX_US =
 *     2.5 s), so a slot that goes missing at full speed is not resent for
 *     seconds -- measured retx_age 1895 ms mean / 3515 ms max at 59.73 fps.
 *   rto_min_us -- the adaptive estimator's floor, 200 ms on this build. */
static uint32_t np_rto_first_max_us, np_rto_min_us;

void fe_np_set_arq_timers(uint32_t rto_first_max_us, uint32_t rto_min_us)
{
   np_rto_first_max_us = rto_first_max_us;
   np_rto_min_us       = rto_min_us;
}

int fe_np_start(const fe_np_config *cfg)
{
   nd_callbacks cb;
   nd_config nc;
   const char *proto;

   if (np.active)
      return -1;

   np.rcb = (const struct retro_netpacket_callback *)fe_host_netpacket_cb();
   if (!np.rcb || !np.rcb->start)
   {
      fe_log("fe_np_start: core registered no netpacket interface");
      return -1;
   }
   proto = np.rcb->protocol_version;
   if (!proto || !proto[0])
   {
      fe_log("fe_np_start: core has no protocol_version string");
      return -1;
   }

   np.now_us = cfg->now_us;
   np.probe = cfg->probe;
   np.core_started = 0;
   np.failed = 0;
   np.core_tx = np.core_rx = np.core_tx_fail = 0;
   np.probe_seq = 0;
   np.last_stats_us = np.last_probe_us = 0;
   memset(&np.prof, 0, sizeof(np.prof));
   prof_calibrate_clock();

   memset(&cb, 0, sizeof(cb));
   cb.deliver           = np_deliver;
   cb.session_started   = np_session_started;
   cb.peer_connected    = np_peer_connected;
   cb.peer_disconnected = np_peer_disconnected;
   cb.session_stopped   = np_session_stopped;
   cb.log               = np_log;

   memset(&nc, 0, sizeof(nc));
   nc.protocol = proto;             /* "gpSP v1.0" — matched in handshake */
   nc.nick = cfg->nick;
   nc.join_nonce = cfg->nonce ? cfg->nonce
                              : (uint32_t)(cfg->now_us() | 1);
   nc.prof_us    = cfg->now_us;     /* ADR-0021 pump rx/arq split */
   /* ADR-0062 harness overrides; 0 = the build-time ND_* default. Routed
    * through a setter rather than a new fe_np_config field on purpose:
    * fe_np_config is included by several translation units and ADR-0035
    * records exactly what a silent struct-layout skew costs on this project. */
   nc.rto_first_max_us = np_rto_first_max_us;
   nc.retx_us          = np_rto_min_us;
   np.nd = netdrv_create(cfg->transport, &cb, &nc);
   if (!np.nd)
      return -1;

   if (cfg->is_host ? netdrv_host(np.nd) : netdrv_join(np.nd))
   {
      netdrv_destroy(np.nd);
      np.nd = NULL;
      return -1;
   }
   np.active = 1;
   fe_evt("net_up role=%s proto=\"%s\"", cfg->is_host ? "host" : "join", proto);
   return 0;
}

void fe_np_pump(void)
{
   uint64_t now;

   if (!np.active || !np.nd)
      return;
   now = np.now_us();
   netdrv_pump(np.nd, now);
   {
      nd_stats ps;
      uint64_t t1 = np.now_us();
      prof_bump(&np.prof.pump_us, &np.prof.pump_max_us, t1 - now);
      /* netdrv's own rx/arq split (nd_config.prof_us) is free-running; mirror
       * it here so one snapshot call gets the whole picture. */
      netdrv_get_stats(np.nd, &ps);
      np.prof.pumps      = ps.prof_pumps;
      np.prof.rx_us      = ps.prof_rx_us;
      np.prof.arq_us     = ps.prof_arq_us;
      np.prof.rx_max_us  = ps.prof_rx_max_us;
      np.prof.arq_max_us = ps.prof_arq_max_us;
   }

   if (np.probe && np.core_started && netdrv_peer_count(np.nd) > 0 &&
       now - np.last_probe_us >= NP_PROBE_INTERVAL_US)
   {
      /* 104-byte reliable broadcast; remote RFU receiver drops it on magic
       * mismatch. Exists purely so the smoke harness can assert acked>0. */
      uint8_t pkt[104];
      memset(pkt, 0, sizeof(pkt));
      memcpy(pkt, "PRB0", 4);
      pkt[4] = (uint8_t)np.probe_seq;
      pkt[5] = (uint8_t)(np.probe_seq >> 8);
      if (netdrv_send_capacity(np.nd, ND_BROADCAST_ID) > 2 &&
          netdrv_send(np.nd, ND_RELIABLE | ND_FLUSH_HINT, pkt, sizeof(pkt),
                      ND_BROADCAST_ID) == 0)
         np.probe_seq++;
      np.last_probe_us = now;
   }

   if (now - np.last_stats_us >= NP_STATS_INTERVAL_US)
   {
      nd_stats st;
      netdrv_get_stats(np.nd, &st);
      /* srtt/rto/retx_pct/txq_hi make this line self-diagnosing for the ARQ
       * storm class: the field failure would have read retx_pct=280 with an
       * rto_us stuck at the 30000 floor (docs/HANDOFF.md issue #2). */
      fe_evt("net_stats tx=%u rx=%u acked=%u retx=%u dup=%u "
             "drop_crc=%u drop_mal=%u drop_unk=%u overflow=%u "
             "drop_dead=%u core_tx=%u core_rx=%u peers=%d "
             "srtt_us=%u rto_us=%u retx_pct=%u txq_hi=%u spill=%u "
             "rttvar_us=%u beyondwin=%u retx_age=%u/%u reorder_hi=%u "
             "tx_ack=%u rx_ack=%u tx_data=%u pumpgap=%u/%u",
             st.tx_frames, st.rx_frames, st.acked, st.retx, st.rx_dup,
             st.rx_drop_crc, st.rx_drop_malformed, st.rx_drop_unknown_peer,
             st.tx_overflow, st.tx_drop_dead, np.core_tx, np.core_rx,
             netdrv_peer_count(np.nd),
             st.srtt_us, st.rto_us,
             st.acked ? (unsigned)(st.retx * 100u / st.acked) : 0u,
             st.txq_hiwater, st.tx_spill,
             /* ADR-0048: rttvar disambiguates a clamped rto from a computed
              * one; beyondwin unmixes rx_dup; retx_age is mean/max in ms of
              * how old a payload was when resent; reorder_hi is head-of-line
              * depth. See netdrv.h for why each exists. */
             st.rttvar_us, st.rx_beyond_win,
             st.retx_age_cnt ? st.retx_age_sum_ms / st.retx_age_cnt : 0u,
             st.retx_age_max_us / 1000u,
             st.reorder_hi,
             /* ADR-0059: separates 'ack sent late' from 'ack lost on air'.
              * tx_data alongside it makes the DATA/ACK split legible -- `tx`
              * is ALL frames and lumps DATA, ACK, PING and JOIN together. */
             st.tx_ack, st.rx_ack, st.tx_data,
             /* ADR-0061: mean/max ms between ARQ pumps. */
             st.pump_gap_n ? st.pump_gap_sum_ms / st.pump_gap_n : 0u,
             st.pump_gap_max_us / 1000u);
      np.last_stats_us = now;
   }
}

void fe_np_stop(void)
{
   if (!np.active)
      return;
   if (np.core_started)
   {
      np.core_started = 0;
      if (np.rcb->stop)
         np.rcb->stop();
   }
   if (np.nd)
   {
      netdrv_leave(np.nd);
      netdrv_destroy(np.nd);
      np.nd = NULL;
   }
   np.active = 0;
   np.failed = 0;
   fe_evt("net_down");
}

int fe_np_session_active(void)
{
   return np.active && np.core_started && netdrv_active(np.nd);
}

int fe_np_failed(void)
{
   return np.failed;
}

/* ---- peer frame-rate matching (ADR-0027) -------------------------------- */

void fe_np_set_local_fps(unsigned fps_x100)
{
   if (np.active)
      netdrv_set_local_fps(np.nd, fps_x100);
}

unsigned fe_np_peer_min_fps(void)
{
   return np.active ? netdrv_peer_min_fps(np.nd) : 0;
}

/* ADR-0078: the backpressure pacer's signal.  See netpacket_host.h. */
uint32_t fe_np_txq_now(void)
{
   nd_stats st;
   if (!np.active)
      return 0;
   netdrv_get_stats(np.nd, &st);
   return st.txq_now;
}
