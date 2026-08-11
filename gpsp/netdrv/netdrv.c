/* netdrv.c — session state machine + full-mesh roster + per-peer ARQ.
 * See netdrv.h for the contract and docs/ARCHITECTURE.md (netdrv section)
 * for the design rationale. Plain C, no OS dependencies: time comes in
 * through pump(now_us), I/O goes through the nd_transport vtable.
 */
#include "netdrv.h"
#include "netdrv_wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ wire utils -- */

/* CRC16-CCITT (poly 0x1021, init 0xFFFF), byte-at-a-time via a 512 B table
 * built on first use. Identical output to the previous bit-serial loop —
 * the table IS the unrolled loop — but ~8x cheaper, which matters on the
 * PSP where every frame is CRC'd twice (build + parse) inside the emu
 * thread's per-frame budget. Single-threaded by the netdrv pump contract,
 * so the lazy init needs no lock. */
static uint16_t nd_crc_tab[256];
static int      nd_crc_tab_ready;

static void nd_crc_init(void)
{
   int i, b;
   for (i = 0; i < 256; i++)
   {
      uint16_t c = (uint16_t)(i << 8);
      for (b = 0; b < 8; b++)
         c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
      nd_crc_tab[i] = c;
   }
   nd_crc_tab_ready = 1;
}

uint16_t nd_crc16(uint16_t crc, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   size_t i;
   if (!nd_crc_tab_ready)
      nd_crc_init();
   for (i = 0; i < len; i++)
      crc = (uint16_t)((crc << 8) ^ nd_crc_tab[(crc >> 8) ^ p[i]]);
   return crc;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t nd_wire_build(uint8_t *out, const nd_hdr *h, const void *payload)
{
   uint16_t crc;
   put32(out, ND_WIRE_MAGIC);
   out[4] = ND_WIRE_VER;
   out[5] = h->type;
   out[6] = h->src;
   out[7] = h->dst;
   put16(out + 8,  h->seq);
   put16(out + 10, h->ack);
   put16(out + 12, h->len);
   put16(out + 14, 0);
   if (h->len)
      memcpy(out + ND_HDR_SIZE, payload, h->len);
   crc = nd_crc16(0xFFFF, out, ND_HDR_SIZE + h->len);
   put16(out + 14, crc);
   return ND_HDR_SIZE + h->len;
}

int nd_wire_parse(const uint8_t *buf, size_t sz, nd_hdr *h,
                  const uint8_t **payload, int *reason)
{
   uint8_t tmp[ND_HDR_SIZE];
   uint16_t crc;
   int why = 0;

   if (sz < ND_HDR_SIZE || sz > ND_MAX_FRAME)
      why = 1;
   else if (get32(buf) != ND_WIRE_MAGIC || buf[4] != ND_WIRE_VER)
      why = 2;
   else if (buf[5] >= ND_T__COUNT)
      why = 3;
   else
   {
      h->magic = get32(buf);
      h->ver   = buf[4];
      h->type  = buf[5];
      h->src   = buf[6];
      h->dst   = buf[7];
      h->seq   = get16(buf + 8);
      h->ack   = get16(buf + 10);
      h->len   = get16(buf + 12);
      h->crc   = get16(buf + 14);
      if (h->len > ND_MAX_PAYLOAD || sz != (size_t)(ND_HDR_SIZE + h->len))
         why = 4;    /* EXACT length or nothing */
      else
      {
         memcpy(tmp, buf, ND_HDR_SIZE);
         put16(tmp + 14, 0);
         crc = nd_crc16(0xFFFF, tmp, ND_HDR_SIZE);
         crc = nd_crc16(crc, buf + ND_HDR_SIZE, h->len);
         if (crc != h->crc)
            why = 5;
      }
   }

   if (reason)
      *reason = why;
   if (why)
      return -1;
   *payload = buf + ND_HDR_SIZE;
   return 0;
}

/* ----------------------------------------------------------------- state -- */

/* The wire format and the roster payload both have to fit the configured
 * payload budget — a build that gets this wrong must not link (ADR-0016). */
typedef char nd_payload_budget_assert[
   (ND_MAX_PAYLOAD >= ND_ROSTER_MAX_SIZE && ND_MAX_PAYLOAD >= ND_JOIN_SIZE &&
    ND_MAX_PAYLOAD >= 104) ? 1 : -1];

/* ADR-0050: the transmit queue is stored as TWO parallel arrays, hot metadata
 * and cold payload, rather than one array of fat slots.
 *
 * Both arq_tx_due() and arq_pump_tx() walk the whole in-flight window every
 * pump to read one 8-byte timer per slot. Interleaved, that walk strides
 * the old fat slot's 168 B across 21 KB to read 1 KB of timers, touching 128
 * of the PSP's 256 data-cache lines -- and dragging 144-byte payloads it never
 * looks at into cache alongside them. The whole per-peer ARQ state is 83712 B
 * against a 16384 B D-cache, so that scan evicts the dynarec's working set
 * twice a frame.
 *
 * Split, the same walk is 24-byte stride over 3072 B contiguous: 48 lines
 * instead of 128, and sequential so the prefetcher can work. The payloads are
 * touched only when a packet is actually (re)transmitted, which is where they
 * belong.
 *
 * The metadata array for a full 384-slot queue is 9216 B, which is what makes
 * it a candidate for the 16 KB on-chip scratchpad; the fat layout never was. */
typedef struct nd_txmeta
{
   uint64_t next_tx_us;          /* 0 = not yet sent */
   uint64_t first_tx_us;         /* for the RTT sample (Karn: rtx must be 0) */
   uint16_t seq;
   uint16_t len;
   uint8_t  rtx;                 /* retransmit count (backoff exponent) */
} nd_txmeta;

/* The whole point of the split is that the scanned window stays small and
 * contiguous. Adding a field here is not free: at ND_WINDOW=128 every 8 bytes
 * of growth is another 16 cache lines dragged through the hot pump. If this
 * assert fires, the honest fix is almost always to put the new field in the
 * payload array or in nd_peer, not to widen the metadata. */
typedef char nd_txmeta_is_lean_assert[sizeof(nd_txmeta) <= 24 ? 1 : -1];

/* Spill node: one RELIABLE payload that did not fit the fixed ring. Sized
 * to the payload, not to ND_MAX_PAYLOAD — the RFU shapes are 16/36/104 B. */
typedef struct nd_spill
{
   struct nd_spill *next;
   uint16_t len;
   uint8_t  payload[1];          /* [len] */
} nd_spill;

typedef struct nd_rxslot
{
   uint8_t  used;
   uint16_t seq;
   uint16_t len;
   uint8_t  payload[ND_MAX_PAYLOAD];
} nd_rxslot;

typedef struct nd_peer
{
   uint8_t  active;
   uint8_t  id;
   uint8_t  mac[6];
   uint32_t nonce;
   char     nick[ND_NICK_LEN];

   /* ARQ tx: ring of queued reliable payloads; [head..head+count) live,
    * first min(count, ND_WINDOW) are the in-flight window. Payloads that
    * do not fit go to the spill FIFO behind it (order preserved) and are
    * pulled back into the ring as it drains — RELIABLE is never dropped. */
   nd_txmeta txq[ND_TXQ_CAP];                    /* hot: scanned every pump */
   uint8_t   txpay[ND_TXQ_CAP][ND_MAX_PAYLOAD];  /* cold: only on (re)transmit */
   uint16_t  txq_head, txq_count;
   uint16_t  tx_next_seq;
   nd_spill *spill_head, *spill_tail;
   uint32_t  spill_count;

   /* Adaptive RTO state (RFC 6298 shape, ADR-0017). */
   uint32_t  srtt_us, rttvar_us, rto_us;
   uint32_t  rtt_samples;
   uint64_t  last_backoff_us;    /* rate-limits the backoff to 1 per RTO */

   /* ARQ rx */
   uint16_t  rx_expect;
   nd_rxslot reorder[ND_WINDOW];
   uint8_t   need_ack;

   /* unreliable drop-stale */
   uint16_t  urx_last;
   uint8_t   urx_any;

   /* ADR-0027: last emulated frame rate this peer reported, in hundredths
    * of a frame/s. 0 = never reported (old build, or not yet heard). */
   uint16_t  fps_x100;

   uint64_t  last_rx_us, last_tx_us;
   uint64_t  last_fps_tx_us;     /* our own fps-report cadence to this peer */
} nd_peer;

enum { ND_S_IDLE, ND_S_JOINING, ND_S_ACTIVE };

struct netdrv
{
   nd_transport tp;
   nd_callbacks cb;
   nd_config    cfg;
   char         proto[ND_PROTO_LEN];
   char         nick[ND_NICK_LEN];

   int      state;
   int      is_host;
   uint64_t last_pump_us;                /* ADR-0061 */
   uint8_t  local_id;
   uint8_t  local_mac[6];
   uint8_t  host_mac[6];         /* client: learned from WELCOME */
   uint8_t  roster_gen;
   uint8_t  have_gen;

   nd_peer  peers[ND_MAX_CLIENTS];   /* indexed by client_id; self unused */

   uint16_t utx_seq;             /* unreliable tx counter (global per sender) */
   uint16_t local_fps_x100;      /* ADR-0027: what we advertise; 0 = off */
   uint64_t now;
   uint64_t last_join_tx, last_roster_tx;
   int      roster_dirty;
   int      in_pump;
   int      tx_due;              /* ADR-0021: a RELIABLE payload was queued
                                    (or re-queued from spill) and has not
                                    been handed to the transport yet, so the
                                    next poll_receive must pump even with an
                                    empty rx ring. Set by the enqueue paths,
                                    cleared once arq_pump_tx has run over
                                    every peer. Keeps the core's outbound
                                    RFU latency sub-frame, which is the whole
                                    reason rfu.c polls mid-frame at all. */
   int      fatal;               /* ND_STOP_TX_FAILED pending: the RELIABLE
                                    contract broke. Raised from send paths
                                    (possibly re-entrant, inside the core's
                                    send_fn) and actioned at the top of the
                                    next pump, never under the caller's
                                    stack. */

   nd_stats st;

   uint8_t  scratch[ND_MAX_FRAME];   /* tx build buffer */
   uint8_t  rxbuf[ND_MAX_FRAME + 64];/* rx buffer (+ slack to detect oversize) */
};

/* ------------------------------------------------------------------ misc -- */

static void nd_logf(netdrv *nd, const char *fmt, ...)
{
   char line[192];
   va_list ap;
   if (!nd->cb.log)
      return;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   nd->cb.log(nd->cb.user, line);
}

static int seq_lt(uint16_t a, uint16_t b)  { return (int16_t)(a - b) < 0; }

static uint32_t tmo(uint32_t override_us, uint32_t def_us)
{
   return override_us ? override_us : def_us;
}

/* -------------------------------------------------------------- tx paths -- */

static void nd_xmit(netdrv *nd, const uint8_t mac[6], uint8_t type,
                    uint8_t dst, uint16_t seq, uint16_t ack,
                    const void *payload, uint16_t len)
{
   nd_hdr h;
   size_t sz;
   h.type = type;
   h.src  = (nd->state == ND_S_ACTIVE) ? nd->local_id : ND_ID_JOINING;
   h.dst  = dst;
   h.seq  = seq;
   h.ack  = ack;
   h.len  = len;
   sz = nd_wire_build(nd->scratch, &h, payload);
   nd->st.tx_frames++;
   nd->st.tx_bytes += (uint32_t)sz;
   if (mac)
      nd->tp.send_to(nd->tp.ctx, mac, nd->scratch, sz);
   else
      nd->tp.broadcast(nd->tp.ctx, nd->scratch, sz);
}

/* Unicast to an established peer, stamping the piggyback ack. */
static void nd_xmit_peer(netdrv *nd, nd_peer *p, uint8_t type,
                         uint16_t seq, const void *payload, uint16_t len)
{
   nd_xmit(nd, p->mac, type, p->id, seq, p->rx_expect, payload, len);
   p->need_ack = 0;
   p->last_tx_us = nd->now;
}

/* ADR-0027: a PING, carrying our emulated frame rate as a 2-byte LE payload
 * when we have one to report. A peer on an older build sees a PING with a
 * payload it ignores (the type's handler never read one), and a peer that
 * reports nothing sends the bare PING it always did — so this is wire
 * compatible in both directions and needs no version negotiation. */
static void nd_xmit_ping(netdrv *nd, nd_peer *p)
{
   uint8_t pl[2];
   if (!nd->local_fps_x100)
   {
      nd_xmit_peer(nd, p, ND_T_PING, 0, NULL, 0);
   }
   else
   {
      pl[0] = (uint8_t)(nd->local_fps_x100 & 0xFF);
      pl[1] = (uint8_t)(nd->local_fps_x100 >> 8);
      nd_xmit_peer(nd, p, ND_T_PING, 0, pl, 2);
   }
   p->last_fps_tx_us = nd->now;
}

/* ------------------------------------------------------------ ARQ engine -- */

/* Ring index of the i'th live slot. Metadata and payload share it, so callers
 * that need both take the index once rather than dereferencing twice. */
static uint16_t txq_idx(const nd_peer *p, uint16_t i)
{
   return (uint16_t)((p->txq_head + i) % ND_TXQ_CAP);
}

static nd_txmeta *txq_at(nd_peer *p, uint16_t i)
{
   return &p->txq[txq_idx(p, i)];
}

static uint32_t rto_floor(const netdrv *nd) { return tmo(nd->cfg.retx_us, ND_RTO_MIN_US); }
static uint32_t rto_ceil(const netdrv *nd)  { return tmo(nd->cfg.retx_max_us, ND_RTO_MAX_US); }

/* Deadline for a slot's FIRST retransmission: the adaptive RTO, but never
 * later than the transport's loss-recovery cap (see ND_RTO_FIRST_MAX_US —
 * pacing and loss recovery are different problems for this core). */
static uint32_t rto_first(const netdrv *nd, const nd_peer *p)
{
   uint32_t cap = tmo(nd->cfg.rto_first_max_us, ND_RTO_FIRST_MAX_US);
   return p->rto_us < cap ? p->rto_us : cap;
}

/* Does this transport pin loss recovery instead of letting the estimator
 * drive retransmission? True when it declared a recovery cap below the RTO
 * ceiling (desktop/UDP: 30 ms vs 240 ms). See ND_RTO_FIRST_MAX_US. */
static int rto_recovery_pinned(const netdrv *nd)
{
   return tmo(nd->cfg.rto_first_max_us, ND_RTO_FIRST_MAX_US) < rto_ceil(nd);
}

static void spill_free(nd_peer *p)
{
   nd_spill *s = p->spill_head;
   while (s)
   {
      nd_spill *n = s->next;
      free(s);
      s = n;
   }
   p->spill_head = p->spill_tail = NULL;
   p->spill_count = 0;
}

static void arq_reset(nd_peer *p)
{
   p->txq_head = p->txq_count = 0;
   p->tx_next_seq = 0;
   p->rx_expect = 0;
   memset(p->reorder, 0, sizeof(p->reorder));
   p->need_ack = 0;
   p->urx_any = 0;
   p->urx_last = 0;
   spill_free(p);
}

/* ---- adaptive RTO (RFC 6298 shape, Karn's algorithm — ADR-0017) -------- */

static void rto_recompute(netdrv *nd, nd_peer *p)
{
   uint32_t lo = rto_floor(nd), hi = rto_ceil(nd);
   uint32_t r;
   if (!p->rtt_samples)
      r = lo;
   else
   {
      uint32_t v = p->rttvar_us;
      if (v > (0xFFFFFFFFu - p->srtt_us) / 4)
         v = (0xFFFFFFFFu - p->srtt_us) / 4;      /* no overflow, ever */
      r = p->srtt_us + 4 * v;
   }
   if (r < lo) r = lo;
   if (r > hi) r = hi;
   p->rto_us = r;
}

/* One RTT sample, in us. Only ever called for a slot that was transmitted
 * exactly once (Karn: an ack for a retransmitted slot is ambiguous — it
 * may be acking the original — so sampling it would drag SRTT down and
 * re-arm the very storm we are killing). */
static void rtt_sample(netdrv *nd, nd_peer *p, uint32_t r)
{
   if (!p->rtt_samples)
   {
      p->srtt_us   = r;
      p->rttvar_us = r / 2;
   }
   else
   {
      uint32_t d = p->srtt_us > r ? p->srtt_us - r : r - p->srtt_us;
      p->rttvar_us = (p->rttvar_us * 3 + d) / 4;    /* 3/4 var + 1/4 |err| */
      p->srtt_us   = (p->srtt_us * 7 + r) / 8;      /* 7/8 srtt + 1/8 R */
   }
   if (p->rtt_samples < 0xFFFFFFFFu)
      p->rtt_samples++;
   rto_recompute(nd, p);
}

/* ---- tx queue: ring + spill (no RELIABLE payload is ever discarded) ---- */

static void txq_hiwater(netdrv *nd, nd_peer *p)
{
   uint32_t q = (uint32_t)p->txq_count + p->spill_count;
   if (q > nd->st.txq_hiwater)
      nd->st.txq_hiwater = q;
}

/* Push one payload into the ring's tail; sends it immediately if it lands
 * inside the window (FLUSH_HINT: nothing is ever Nagled). Ring must have
 * room. */
static void txq_push(netdrv *nd, nd_peer *p, const void *buf, uint16_t len)
{
   uint16_t   ix = txq_idx(p, p->txq_count);
   nd_txmeta *s  = &p->txq[ix];
   s->seq = p->tx_next_seq++;
   s->len = len;
   s->rtx = 0;
   s->next_tx_us = 0;
   s->first_tx_us = 0;
   memcpy(p->txpay[ix], buf, len);
   p->txq_count++;
   if (p->txq_count <= ND_WINDOW)
   {
      nd_xmit_peer(nd, p, ND_T_DATA, s->seq, p->txpay[ix], s->len);
      nd->st.tx_data++;
      s->first_tx_us = nd->now;
      s->next_tx_us  = nd->now + rto_first(nd, p);
   }
}

/* Pull spilled payloads back into the ring as it drains. Order is FIFO
 * throughout, so sequence numbers stay monotonic. */
static void spill_refill(netdrv *nd, nd_peer *p)
{
   while (p->spill_head && p->txq_count < ND_TXQ_CAP)
   {
      nd_spill *s = p->spill_head;
      p->spill_head = s->next;
      if (!p->spill_head)
         p->spill_tail = NULL;
      p->spill_count--;
      txq_push(nd, p, s->payload, s->len);
      free(s);
   }
}

/* The RELIABLE contract lives here (ADR-0016).  Full ring toward a LIVE
 * peer is BACKPRESSURE, not loss: park the payload in the spill FIFO. The
 * old code discarded it and counted tx_overflow — which is exactly how two
 * field consoles lost RFU commands and wedged ("arq overflow peer=0
 * (payload lost)", docs/HANDOFF.md issue #2 kill shot).
 *
 * A full ring toward a peer that has been silent > 2x keepalive is a
 * different animal: the window head can never be acked, so ANY finite
 * buffer fills until keepalive death discards the queue anyway (Gate-4E
 * --disconnect: the game keeps sending to a SIGKILLed peer for seconds).
 * Those drops stay classified as tx_drop_dead and do not spill. */
static int arq_enqueue(netdrv *nd, nd_peer *p, const void *buf, uint16_t len)
{
   if (p->txq_count >= ND_TXQ_CAP)
   {
      uint64_t quiet = nd->now - p->last_rx_us;
      nd_spill *sp;

      if (quiet > 2ull * tmo(nd->cfg.keepalive_us, ND_KEEPALIVE_US))
      {
         nd->st.tx_drop_dead++;
         nd_logf(nd, "arq drop to silent peer=%u quiet_ms=%u (dying link)",
                 p->id, (unsigned)(quiet / 1000));
         return -1;
      }

      if (p->spill_count >= (nd->cfg.spill_max ? nd->cfg.spill_max
                                               : (uint32_t)ND_SPILL_MAX) ||
          (sp = (nd_spill *)malloc(sizeof(nd_spill) + len)) == NULL)
      {
         /* Genuinely unrecoverable. Do NOT drop quietly: the core's RFU
          * state machine would lose a command it never retransmits and the
          * game would wedge minutes later with no diagnosis. */
         nd->st.tx_overflow++;
         nd->fatal = ND_STOP_TX_FAILED;
         nd_logf(nd, "arq UNRECOVERABLE peer=%u txq=%u spill=%u — failing "
                     "session (RELIABLE contract cannot be honoured)",
                 p->id, (unsigned)p->txq_count, (unsigned)p->spill_count);
         return -1;
      }

      sp->next = NULL;
      sp->len  = len;
      memcpy(sp->payload, buf, len);
      if (p->spill_tail)
         p->spill_tail->next = sp;
      else
         p->spill_head = sp;
      p->spill_tail = sp;
      p->spill_count++;
      nd->st.tx_spill++;
      txq_hiwater(nd, p);
      nd->tx_due = 1;
      return 0;                  /* accepted — backpressure, not loss */
   }

   txq_push(nd, p, buf, len);
   txq_hiwater(nd, p);
   nd->tx_due = 1;
   return 0;
}

/* Cumulative ack: peer expects `ack` next, so everything before it is done. */
static void arq_on_ack(netdrv *nd, nd_peer *p, uint16_t ack)
{
   uint32_t sample = 0;
   int have_sample = 0;

   while (p->txq_count && seq_lt(txq_at(p, 0)->seq, ack))
   {
      nd_txmeta *done = txq_at(p, 0);
      /* Karn: only a slot transmitted exactly once yields an unambiguous
       * RTT. Take the newest such sample from this ack burst (one sample
       * per ack keeps the EWMA weighting honest). */
      if (done->rtx == 0 && done->first_tx_us && nd->now >= done->first_tx_us)
      {
         sample = (uint32_t)(nd->now - done->first_tx_us);
         have_sample = 1;
      }
      p->txq_head = (uint16_t)((p->txq_head + 1) % ND_TXQ_CAP);
      p->txq_count--;
      nd->st.acked++;
      /* A slot may have slid into the window without ever being sent. */
      if (p->txq_count >= ND_WINDOW)
      {
         uint16_t   ix = txq_idx(p, ND_WINDOW - 1);
         nd_txmeta *s  = &p->txq[ix];
         if (s->next_tx_us == 0)
         {
            nd_xmit_peer(nd, p, ND_T_DATA, s->seq, p->txpay[ix], s->len);
            nd->st.tx_data++;
            s->first_tx_us = nd->now;
            s->next_tx_us  = nd->now + rto_first(nd, p);
         }
      }
   }
   if (have_sample)
      rtt_sample(nd, p, sample);
   if (p->spill_head)
      spill_refill(nd, p);
}

/* Is anything of ours due to go out to this peer on this tick? Used to
 * decide whether a pending ack can piggyback (free) or must go as a bare
 * ACK straight away (ADR-0017: honest RTT samples need prompt acks). */
static int arq_tx_due(const nd_peer *p, uint64_t now)
{
   uint16_t i, win = p->txq_count < ND_WINDOW ? p->txq_count : ND_WINDOW;
   for (i = 0; i < win; i++)
   {
      const nd_txmeta *s = &p->txq[(uint16_t)((p->txq_head + i) % ND_TXQ_CAP)];
      if (s->next_tx_us == 0 || now >= s->next_tx_us)
         return 1;
   }
   return 0;
}

static void arq_pump_tx(netdrv *nd, nd_peer *p)
{
   uint16_t i, win;
   uint32_t cap = rto_ceil(nd);

   if (p->spill_head)
      spill_refill(nd, p);
   win = p->txq_count < ND_WINDOW ? p->txq_count : ND_WINDOW;

   for (i = 0; i < win; i++)
   {
      uint16_t   ix = txq_idx(p, i);
      nd_txmeta *s  = &p->txq[ix];
      if (s->next_tx_us == 0)
      {  /* never sent (was beyond window at enqueue time) */
         nd_xmit_peer(nd, p, ND_T_DATA, s->seq, p->txpay[ix], s->len);
         nd->st.tx_data++;
         s->first_tx_us = nd->now;
         s->next_tx_us  = nd->now + rto_first(nd, p);
      }
      else if (nd->now >= s->next_tx_us)
      {
         uint32_t next;
         if (rto_recovery_pinned(nd))
         {
            /* This transport pins loss recovery (ND_RTO_FIRST_MAX_US), so it
             * owns the whole ladder: per-slot base<<rtx, capped — the exact
             * ADR-0010 behaviour the desktop rig is validated against. The
             * estimator keeps running and reporting; it just does not drive
             * retransmission here. Anything else changes recovery latency on
             * a link that has nothing underneath it to repair loss. */
            uint32_t base = rto_first(nd, p);
            next = base << (s->rtx < 4 ? s->rtx : 4);
            if (next > cap || next < base)     /* cap + shift overflow */
               next = cap;
         }
         else
         {
            /* Exponential backoff, RETAINED at peer level (Karn's algorithm,
             * second half). Per-slot backoff is not enough: if the floor sits
             * below the true RTT, every slot's FIRST transmission times out,
             * so every ack is ambiguous, so no sample is ever Karn-valid and
             * the estimator never starts — precisely the deadlock the field
             * logs show (retx/acked 2.8 with SRTT unknowable). Doubling the
             * peer's RTO and keeping it means later slots start from the
             * backed-off value, one of them survives un-retransmitted, and
             * that clean sample re-derives RTO from measurement.
             * Rate-limited to one doubling per RTO interval so a window full
             * of simultaneous timeouts counts as one loss event. */
            if (nd->now - p->last_backoff_us >= p->rto_us)
            {
               uint32_t r = p->rto_us * 2;
               if (r > cap || r < p->rto_us)   /* cap + overflow guard */
                  r = cap;
               p->rto_us = r;
               p->last_backoff_us = nd->now;
            }
            next = p->rto_us;
         }
         if (s->rtx < 255)
            s->rtx++;
         {
            /* Price this resend against the packet's own age, not against the
             * RTO we intended -- those differ whenever the scan is late. */
            uint32_t age = (uint32_t)(nd->now - s->first_tx_us);
            if (age > nd->st.retx_age_max_us)
               nd->st.retx_age_max_us = age;
            nd->st.retx_age_sum_ms += age / 1000u;
            nd->st.retx_age_cnt++;
         }
         nd_xmit_peer(nd, p, ND_T_DATA, s->seq, p->txpay[ix], s->len);
         nd->st.tx_data++;
         nd->st.retx++;
         s->next_tx_us = nd->now + next;
      }
   }
}

static void nd_deliver(netdrv *nd, nd_peer *p, const uint8_t *buf,
                       uint16_t len, int reliable)
{
   if (reliable)
      nd->st.delivered_rel++;
   else
      nd->st.delivered_unrel++;
   if (nd->cb.deliver)
      nd->cb.deliver(nd->cb.user, buf, len, p->id);
}

static void arq_rx_data(netdrv *nd, nd_peer *p, uint16_t seq,
                        const uint8_t *payload, uint16_t len)
{
   int16_t d = (int16_t)(seq - p->rx_expect);
   p->need_ack = 1;
   if (d < 0)
   {
      nd->st.rx_dup++;         /* old or duplicate — re-ack, don't deliver */
      return;
   }
   if (d == 0)
   {
      uint8_t local[ND_MAX_PAYLOAD];
      uint16_t llen = len;
      int i, progressed = 1;
      /* Copy out: deliver() may re-enter send paths that reuse buffers. */
      memcpy(local, payload, len);
      p->rx_expect++;
      nd_deliver(nd, p, local, llen, 1);
      /* Drain any buffered successors now unblocked. */
      while (progressed)
      {
         progressed = 0;
         for (i = 0; i < ND_WINDOW; i++)
         {
            nd_rxslot *r = &p->reorder[i];
            if (r->used && r->seq == p->rx_expect)
            {
               r->used = 0;
               p->rx_expect++;
               nd_deliver(nd, p, r->payload, r->len, 1);
               progressed = 1;
            }
            else if (r->used && seq_lt(r->seq, p->rx_expect))
               r->used = 0;   /* stale stash */
         }
      }
      return;
   }
   if (d < ND_WINDOW)
   {
      /* Out of order but within window: stash (dup-safe). */
      int i, free_i = -1, used = 0;
      for (i = 0; i < ND_WINDOW; i++)
      {
         if (p->reorder[i].used && p->reorder[i].seq == seq)
            return;                       /* already stashed */
         if (p->reorder[i].used)
            used++;
         else if (free_i < 0)
            free_i = i;
      }
      if ((uint32_t)used + 1u > nd->st.reorder_hi)
         nd->st.reorder_hi = (uint32_t)used + 1u;
      if (free_i >= 0)
      {
         p->reorder[free_i].used = 1;
         p->reorder[free_i].seq  = seq;
         p->reorder[free_i].len  = len;
         memcpy(p->reorder[free_i].payload, payload, len);
      }
      return;
   }
   /* Beyond window: sender can't legally be here; drop. Counted separately
    * from rx_dup -- this is a broken window contract, not a duplicate, and
    * summing the two produces a "spurious retransmit rate" that silently
    * mixes in packets the peer never retransmitted. */
   nd->st.rx_beyond_win++;
}

/* -------------------------------------------------------- roster payloads -- */

static int nd_peer_count_(const netdrv *nd)
{
   int i, n = 0;
   for (i = 0; i < ND_MAX_CLIENTS; i++)
      if (nd->peers[i].active)
         n++;
   return n;
}

static size_t roster_build(netdrv *nd, uint8_t *out, uint8_t your_id)
{
   size_t off = 3;
   int i;
   uint8_t count = 0;

   /* entry 0: ourselves (the host) */
   out[off] = nd->local_id;
   memcpy(out + off + 1, nd->local_mac, 6);
   put32(out + off + 7, nd->cfg.join_nonce);
   memset(out + off + 11, 0, ND_NICK_LEN);
   memcpy(out + off + 11, nd->nick, strlen(nd->nick));
   off += ND_ROSTER_ENTRY_SIZE;
   count++;

   for (i = 0; i < ND_MAX_CLIENTS; i++)
   {
      nd_peer *p = &nd->peers[i];
      if (!p->active)
         continue;
      out[off] = p->id;
      memcpy(out + off + 1, p->mac, 6);
      put32(out + off + 7, p->nonce);
      memset(out + off + 11, 0, ND_NICK_LEN);
      memcpy(out + off + 11, p->nick, strnlen(p->nick, ND_NICK_LEN - 1));
      off += ND_ROSTER_ENTRY_SIZE;
      count++;
   }

   out[0] = your_id;
   out[1] = nd->roster_gen;
   out[2] = count;
   return off;
}

static void host_broadcast_roster(netdrv *nd)
{
   uint8_t pl[ND_ROSTER_MAX_SIZE];
   size_t n = roster_build(nd, pl, ND_ID_BCAST);
   nd_xmit(nd, NULL, ND_T_ROSTER, ND_ID_BCAST, 0, 0, pl, (uint16_t)n);
   nd->last_roster_tx = nd->now;
   nd->roster_dirty = 0;
}

static void peer_remove(netdrv *nd, uint8_t id, int fire_cb)
{
   nd_peer *p = &nd->peers[id];
   if (!p->active)
      return;
   p->active = 0;
   arq_reset(p);
   if (fire_cb && nd->cb.peer_disconnected)
      nd->cb.peer_disconnected(nd->cb.user, id);
}

static void peer_add(netdrv *nd, uint8_t id, const uint8_t mac[6],
                     uint32_t nonce, const char *nick)
{
   nd_peer *p = &nd->peers[id];
   spill_free(p);                /* never leak a previous tenant's backlog */
   memset(p, 0, sizeof(*p));
   p->active = 1;
   p->id = id;
   memcpy(p->mac, mac, 6);
   p->nonce = nonce;
   memcpy(p->nick, nick, ND_NICK_LEN - 1);
   p->nick[ND_NICK_LEN - 1] = '\0';
   p->last_rx_us = nd->now;
   p->last_tx_us = nd->now;
   /* No RTT samples yet: start at the transport floor (ADR-0017). */
   p->rto_us = rto_floor(nd);
}

static void session_teardown(netdrv *nd, int reason)
{
   int i;
   for (i = 0; i < ND_MAX_CLIENTS; i++)
   {
      if (nd->peers[i].active && nd->cb.peer_disconnected)
         nd->cb.peer_disconnected(nd->cb.user, (uint8_t)i);
      nd->peers[i].active = 0;
      arq_reset(&nd->peers[i]);
   }
   nd->state = ND_S_IDLE;
   nd->is_host = 0;
   nd->have_gen = 0;
   if (reason != ND_STOP_LOCAL && nd->cb.session_stopped)
      nd->cb.session_stopped(nd->cb.user, reason);
}

/* Client: apply a roster payload from the host. */
static void roster_apply(netdrv *nd, const uint8_t *pl, uint16_t len)
{
   uint8_t gen, count, present[ND_MAX_CLIENTS] = { 0 };
   const uint8_t *e;
   int i;

   if (len < 3)
      return;
   gen = pl[1];
   count = pl[2];
   if (count > ND_MAX_CLIENTS || len != (uint16_t)(3 + count * ND_ROSTER_ENTRY_SIZE))
      return;
   if (nd->have_gen && (int8_t)(gen - nd->roster_gen) <= 0)
      return;                     /* stale or same generation */
   nd->roster_gen = gen;
   nd->have_gen = 1;

   e = pl + 3;
   for (i = 0; i < count; i++, e += ND_ROSTER_ENTRY_SIZE)
   {
      uint8_t id = e[0];
      uint32_t nonce = get32(e + 7);
      char nick[ND_NICK_LEN];
      if (id >= ND_MAX_CLIENTS)
         continue;
      memcpy(nick, e + 11, ND_NICK_LEN);
      nick[ND_NICK_LEN - 1] = '\0';
      if (id == nd->local_id)
      {
         present[id] = 1;         /* ourselves */
         continue;
      }
      present[id] = 1;
      if (nd->peers[id].active)
      {
         if (nd->peers[id].nonce != nonce ||
             memcmp(nd->peers[id].mac, e + 1, 6) != 0)
         {
            /* Different instance now owns this id: reset the channel. */
            nd_logf(nd, "roster gen=%u: id=%u nonce/mac changed — ARQ reset",
                    gen, id);
            peer_remove(nd, id, 1);
            peer_add(nd, id, e + 1, nonce, nick);
            if (nd->cb.peer_connected)
               nd->cb.peer_connected(nd->cb.user, id);
         }
      }
      else
      {
         peer_add(nd, id, e + 1, nonce, nick);
         if (nd->cb.peer_connected)
            nd->cb.peer_connected(nd->cb.user, id);
      }
   }

   for (i = 0; i < ND_MAX_CLIENTS; i++)
      if (nd->peers[i].active && !present[i])
         peer_remove(nd, (uint8_t)i, 1);

   /* We fell out of the host's roster (evicted after a long stall):
    * the session is over for us. */
   if (nd->state == ND_S_ACTIVE && !nd->is_host && !present[nd->local_id])
      session_teardown(nd, ND_STOP_EVICTED);
}

/* ------------------------------------------------------------ handshakes -- */

static void client_send_join(netdrv *nd)
{
   uint8_t pl[ND_JOIN_SIZE];
   memset(pl, 0, sizeof(pl));
   memcpy(pl, nd->proto, strnlen(nd->proto, ND_PROTO_LEN - 1));
   memcpy(pl + 24, nd->nick, strnlen(nd->nick, ND_NICK_LEN - 1));
   put32(pl + 40, nd->cfg.join_nonce);
   nd_xmit(nd, NULL, ND_T_JOIN, ND_ID_BCAST, 0, 0, pl, ND_JOIN_SIZE);
   nd->last_join_tx = nd->now;
}

static void host_send_welcome(netdrv *nd, const uint8_t mac[6], uint8_t your_id)
{
   uint8_t pl[ND_ROSTER_MAX_SIZE];
   size_t n = roster_build(nd, pl, your_id);
   nd_xmit(nd, mac, ND_T_WELCOME, ND_ID_JOINING, 0, 0, pl, (uint16_t)n);
}

static void host_on_join(netdrv *nd, const uint8_t mac[6],
                         const uint8_t *pl, uint16_t len)
{
   char proto[ND_PROTO_LEN], nick[ND_NICK_LEN];
   uint32_t nonce;
   int i, free_id = -1;

   if (len != ND_JOIN_SIZE)
      return;
   memcpy(proto, pl, ND_PROTO_LEN);
   proto[ND_PROTO_LEN - 1] = '\0';
   memcpy(nick, pl + 24, ND_NICK_LEN);
   nick[ND_NICK_LEN - 1] = '\0';
   nonce = get32(pl + 40);

   if (strncmp(proto, nd->proto, ND_PROTO_LEN) != 0)
   {
      nd_logf(nd, "join refused: proto '%s' != '%s'", proto, nd->proto);
      return;                     /* silently ignore; wrong app/version */
   }

   /* Known instance re-JOINing (WELCOME lost, or same-addr restart)? */
   for (i = 1; i < ND_MAX_CLIENTS; i++)
   {
      nd_peer *p = &nd->peers[i];
      if (p->active && memcmp(p->mac, mac, 6) == 0)
      {
         if (p->nonce != nonce)
         {
            /* Same address, new process: reset the channel, keep the id. */
            uint8_t id = p->id;
            peer_remove(nd, id, 1);
            peer_add(nd, id, mac, nonce, nick);
            nd->roster_gen++;
            nd->roster_dirty = 1;
            if (nd->cb.peer_connected &&
                nd->cb.peer_connected(nd->cb.user, id) != 0)
            {
               peer_remove(nd, id, 0);
               host_send_welcome(nd, mac, ND_ID_REFUSED);
               return;
            }
         }
         host_send_welcome(nd, mac, p->id);
         return;
      }
   }

   for (i = 1; i < ND_MAX_CLIENTS; i++)
      if (!nd->peers[i].active) { free_id = i; break; }

   if (free_id < 0)
   {
      host_send_welcome(nd, mac, ND_ID_REFUSED);   /* session full */
      return;
   }

   peer_add(nd, (uint8_t)free_id, mac, nonce, nick);
   nd->roster_gen++;
   nd->roster_dirty = 1;
   if (nd->cb.peer_connected &&
       nd->cb.peer_connected(nd->cb.user, (uint8_t)free_id) != 0)
   {
      /* Admission refused by the frontend/core. */
      peer_remove(nd, (uint8_t)free_id, 0);
      host_send_welcome(nd, mac, ND_ID_REFUSED);
      return;
   }
   host_send_welcome(nd, mac, (uint8_t)free_id);
   host_broadcast_roster(nd);
   nd_logf(nd, "admitted peer id=%d nick=%s", free_id, nick);
}

static void client_on_welcome(netdrv *nd, const uint8_t mac[6],
                              const uint8_t *pl, uint16_t len)
{
   uint8_t your_id;
   if (nd->state != ND_S_JOINING || len < 3)
      return;
   your_id = pl[0];
   if (your_id == ND_ID_REFUSED)
   {
      nd->state = ND_S_IDLE;
      if (nd->cb.session_stopped)
         nd->cb.session_stopped(nd->cb.user, ND_STOP_REFUSED);
      return;
   }
   if (your_id == 0 || your_id >= ND_MAX_CLIENTS)
      return;
   nd->local_id = your_id;
   nd->state = ND_S_ACTIVE;
   nd->is_host = 0;
   memcpy(nd->host_mac, mac, 6);
   if (nd->cb.session_started)
      nd->cb.session_started(nd->cb.user, nd->local_id);
   roster_apply(nd, pl, len);    /* fires peer_connected incl. host (id 0) */
}

/* -------------------------------------------------------------- rx frame -- */

static nd_peer *lookup_peer(netdrv *nd, uint8_t id, const uint8_t mac[6])
{
   nd_peer *p;
   if (id >= ND_MAX_CLIENTS)
      return NULL;
   p = &nd->peers[id];
   if (!p->active || memcmp(p->mac, mac, 6) != 0)
      return NULL;   /* id/mac mismatch: stale or spoofed — drop upstream */
   return p;
}

static void handle_frame(netdrv *nd, const uint8_t mac[6],
                         const nd_hdr *h, const uint8_t *pl)
{
   nd_peer *p;

   /* Control plane first (roster-independent types). */
   switch (h->type)
   {
   case ND_T_JOIN:
      if (nd->state == ND_S_ACTIVE && nd->is_host)
         host_on_join(nd, mac, pl, h->len);
      return;
   case ND_T_WELCOME:
      client_on_welcome(nd, mac, pl, h->len);
      return;
   case ND_T_ROSTER:
      if (nd->state == ND_S_ACTIVE && !nd->is_host)
      {
         if (memcmp(mac, nd->host_mac, 6) != 0)
         {
            nd->st.rx_drop_unknown_peer++;   /* roster from a non-host source */
            return;
         }
         roster_apply(nd, pl, h->len);
         if (nd->peers[0].active)
            nd->peers[0].last_rx_us = nd->now;   /* roster counts as liveness */
      }
      return;
   default:
      break;
   }

   if (nd->state != ND_S_ACTIVE)
      return;

   /* Data plane: must come from an established roster peer. */
   p = lookup_peer(nd, h->src, mac);
   if (!p)
   {
      nd->st.rx_drop_unknown_peer++;
      return;
   }
   /* Unicast frames must actually be for us. */
   if (h->dst != nd->local_id && h->dst != ND_ID_BCAST)
   {
      nd->st.rx_drop_malformed++;
      return;
   }
   p->last_rx_us = nd->now;

   switch (h->type)
   {
   case ND_T_DATA:
      arq_on_ack(nd, p, h->ack);
      nd->st.rx_data++;
      arq_rx_data(nd, p, h->seq, pl, h->len);
      break;
   case ND_T_ACK:
      nd->st.rx_ack++;                    /* ADR-0059 */
      arq_on_ack(nd, p, h->ack);
      break;
   case ND_T_PING:
      arq_on_ack(nd, p, h->ack);
      /* ADR-0027: optional 2-byte LE emulated frame rate, hundredths. A
       * bare PING (old build) leaves the last known value alone rather
       * than zeroing it — absence of a report is not a report of zero. */
      if (h->len >= 2)
         p->fps_x100 = (uint16_t)(pl[0] | ((uint16_t)pl[1] << 8));
      break;
   case ND_T_UDATA:
   {
      int16_t d;
      arq_on_ack(nd, p, h->ack);
      d = (int16_t)(h->seq - p->urx_last);
      if (p->urx_any && d <= 0)
      {
         nd->st.rx_drop_stale++;
         break;
      }
      p->urx_last = h->seq;
      p->urx_any = 1;
      nd_deliver(nd, p, pl, h->len, 0);
      break;
   }
   case ND_T_UDATA_US:
      arq_on_ack(nd, p, h->ack);
      nd_deliver(nd, p, pl, h->len, 0);
      break;
   case ND_T_BYE:
      if (!nd->is_host && p->id == 0)
      {
         session_teardown(nd, ND_STOP_HOST_BYE);
         return;
      }
      peer_remove(nd, p->id, 1);
      if (nd->is_host)
      {
         nd->roster_gen++;
         nd->roster_dirty = 1;
         host_broadcast_roster(nd);
      }
      break;
   default:
      break;
   }
}

/* ------------------------------------------------------------------ pump -- */

/* Profiling helpers (ADR-0021): no-ops unless cfg.prof_us is wired. */
static uint64_t prof_t0(const netdrv *nd)
{
   return nd->cfg.prof_us ? nd->cfg.prof_us() : 0;
}

/* Close one section and open the next off a SINGLE clock read: a pump costs
 * three reads total, not four, and the sections cannot drift apart. */
static uint64_t prof_mark(netdrv *nd, uint64_t t0, uint32_t *sum, uint32_t *max)
{
   uint64_t now, d;
   if (!nd->cfg.prof_us)
      return 0;
   now = nd->cfg.prof_us();
   d = now - t0;
   if (d > 0xFFFFFFFFull)
      d = 0xFFFFFFFFull;
   *sum += (uint32_t)d;
   if ((uint32_t)d > *max)
      *max = (uint32_t)d;
   return now;
}

void netdrv_pump(netdrv *nd, uint64_t now_us)
{
   /* ADR-0061: interval since the previous pump. */
   if (nd->last_pump_us) {
      uint32_t gap = (uint32_t)(now_us - nd->last_pump_us);
      if (gap > nd->st.pump_gap_max_us)
         nd->st.pump_gap_max_us = gap;
      nd->st.pump_gap_sum_ms += gap / 1000u;
      nd->st.pump_gap_n++;
   }
   nd->last_pump_us = now_us;
   int i, budget;
   uint64_t pt = 0;

   if (!nd || nd->in_pump)
      return;                     /* re-entry guard (nested poll_receive) */
   nd->in_pump = 1;
   nd->now = now_us;
   nd->st.prof_pumps++;

   /* 0. A RELIABLE payload could not be queued since the last pump. The
    *    core's link is unrecoverable from that instant (it never resends),
    *    so fail loudly here — outside the caller's send_fn stack — instead
    *    of pinging a corpse for minutes (docs/HANDOFF.md issue #2). */
   if (nd->fatal)
   {
      int reason = nd->fatal;
      nd->fatal = 0;
      if (nd->state == ND_S_ACTIVE)   /* a teardown may have raced us */
      {
         nd_logf(nd, "session failed: txq_overflow (RELIABLE payload could "
                     "not be queued) — tearing down");
         session_teardown(nd, reason);
         nd->in_pump = 0;
         return;
      }
   }

   /* 1. Drain the transport. */
   pt = prof_t0(nd);
   for (budget = 0; budget < 256; budget++)
   {
      uint8_t mac[6];
      nd_hdr h;
      const uint8_t *pl;
      int why;
      int n = nd->tp.recv(nd->tp.ctx, mac, nd->rxbuf, sizeof(nd->rxbuf));
      if (n <= 0)
         break;
      nd->st.rx_frames++;
      nd->st.rx_bytes += (uint32_t)n;
      if (nd_wire_parse(nd->rxbuf, (size_t)n, &h, &pl, &why) != 0)
      {
         if (why == 5)
            nd->st.rx_drop_crc++;
         else
            nd->st.rx_drop_malformed++;
         continue;
      }
      handle_frame(nd, mac, &h, pl);
      if (nd->state == ND_S_IDLE && !nd->is_host)
         break;                   /* torn down mid-drain */
   }

   /* 1b. Prompt explicit ACKs (ADR-0017). A DATA frame received in this
    *     tick is acked in this tick. Where we have traffic of our own due,
    *     the ack rides along for free (arq_pump_tx below stamps rx_expect
    *     into every unicast); where we do not, the bare ACK goes out NOW
    *     rather than after the retx/keepalive scan — and, importantly,
    *     even if that scan returns early (host-death teardown). Ack
    *     latency is part of the peer's RTT sample, so this keeps SRTT
    *     honest and stops us provoking our own retransmits. */
   if (nd->state == ND_S_ACTIVE)
   {
      for (i = 0; i < ND_MAX_CLIENTS; i++)
      {
         nd_peer *p = &nd->peers[i];
         if (p->active && p->need_ack && !arq_tx_due(p, nd->now))
            { nd->st.tx_ack++; nd_xmit_peer(nd, p, ND_T_ACK, 0, NULL, 0); }  /* ADR-0059 */
      }
   }
   pt = prof_mark(nd, pt, &nd->st.prof_rx_us, &nd->st.prof_rx_max_us);

   /* 2. Timers. */
   if (nd->state == ND_S_JOINING)
   {
      if (nd->now - nd->last_join_tx >= tmo(nd->cfg.join_retry_us, ND_JOIN_RETRY_US)
          || nd->last_join_tx == 0)
         client_send_join(nd);
   }
   else if (nd->state == ND_S_ACTIVE)
   {
      uint32_t ka   = tmo(nd->cfg.keepalive_us, ND_KEEPALIVE_US);
      uint32_t dead = tmo(nd->cfg.peer_dead_us, ND_PEER_DEAD_US);

      for (i = 0; i < ND_MAX_CLIENTS; i++)
      {
         nd_peer *p = &nd->peers[i];
         if (!p->active)
            continue;

         arq_pump_tx(nd, p);

         if (p->need_ack)
            { nd->st.tx_ack++; nd_xmit_peer(nd, p, ND_T_ACK, 0, NULL, 0); }  /* ADR-0059 */

         /* Death authority (split-brain guard): the host judges clients;
          * clients judge only the host link. Client<->client liveness is
          * host-relayed via roster generations. */
         if (nd->now - p->last_rx_us >= dead)
         {
            if (nd->is_host)
            {
               nd_logf(nd, "peer id=%u dead (keepalive timeout, "
                           "silent=%ums rx_frames=%u drop_unk=%u)", p->id,
                       (unsigned)((nd->now - p->last_rx_us) / 1000),
                       nd->st.rx_frames, nd->st.rx_drop_unknown_peer);
               peer_remove(nd, p->id, 1);
               nd->roster_gen++;
               nd->roster_dirty = 1;
               continue;
            }
            else if (p->id == 0)
            {
               nd_logf(nd, "host dead (keepalive timeout, silent=%ums "
                           "rx_frames=%u drop_unk=%u) — teardown",
                       (unsigned)((nd->now - p->last_rx_us) / 1000),
                       nd->st.rx_frames, nd->st.rx_drop_unknown_peer);
               session_teardown(nd, ND_STOP_HOST_LOST);
               nd->in_pump = 0;
               return;
            }
         }

         /* Keepalive PING (idle link), OR the ADR-0027 frame-rate report,
          * which must run even on a busy link: during a trade we send DATA
          * every frame, so the keepalive arm alone would never fire and the
          * peer's rate would never reach the other side. */
         if (nd->now - p->last_tx_us >= ka ||
             (nd->local_fps_x100 &&
              nd->now - p->last_fps_tx_us >= ND_FPS_REPORT_US))
            nd_xmit_ping(nd, p);
      }

      /* arq_pump_tx has now run over every live peer, so anything still
       * queued is window-blocked rather than waiting on us (ADR-0021). */
      nd->tx_due = 0;

      if (nd->is_host &&
          (nd->roster_dirty ||
           nd->now - nd->last_roster_tx >= tmo(nd->cfg.roster_us, ND_ROSTER_US)))
         host_broadcast_roster(nd);
   }
   prof_mark(nd, pt, &nd->st.prof_arq_us, &nd->st.prof_arq_max_us);

   nd->in_pump = 0;
}

int netdrv_poll_needed(const netdrv *nd)
{
   if (!nd || nd->in_pump)
      return 0;
   if (nd->fatal || nd->tx_due)
      return 1;
   if (!nd->tp.pending)
      return 1;                   /* transport cannot tell: assume yes */
   return nd->tp.pending(nd->tp.ctx) != 0;
}

/* ------------------------------------------------------------------- API -- */

netdrv *netdrv_create(const nd_transport *tp, const nd_callbacks *cb,
                      const nd_config *cfg)
{
   netdrv *nd = (netdrv *)calloc(1, sizeof(*nd));
   if (!nd)
      return NULL;
   nd->tp = *tp;
   nd->cb = *cb;
   nd->cfg = *cfg;
   if (cfg->protocol)
      memcpy(nd->proto, cfg->protocol, strnlen(cfg->protocol, ND_PROTO_LEN - 1));
   if (cfg->nick)
      memcpy(nd->nick, cfg->nick, strnlen(cfg->nick, ND_NICK_LEN - 1));
   if (!nd->cfg.join_nonce)
      nd->cfg.join_nonce = 1;     /* must be nonzero; caller should randomize */
   nd->tp.local_addr(nd->tp.ctx, nd->local_mac);
   nd->state = ND_S_IDLE;
   return nd;
}

void netdrv_destroy(netdrv *nd)
{
   int i;
   if (!nd)
      return;
   for (i = 0; i < ND_MAX_CLIENTS; i++)
      spill_free(&nd->peers[i]);
   free(nd);
}

int netdrv_host(netdrv *nd)
{
   if (nd->state != ND_S_IDLE)
      return -1;
   nd->state = ND_S_ACTIVE;
   nd->is_host = 1;
   nd->local_id = 0;
   nd->fatal = 0;
   nd->roster_gen = 1;
   nd->roster_dirty = 1;
   if (nd->cb.session_started)
      nd->cb.session_started(nd->cb.user, 0);
   return 0;
}

int netdrv_join(netdrv *nd)
{
   if (nd->state != ND_S_IDLE)
      return -1;
   nd->state = ND_S_JOINING;
   nd->is_host = 0;
   nd->fatal = 0;
   nd->last_join_tx = 0;          /* pump sends the first JOIN immediately */
   return 0;
}

void netdrv_leave(netdrv *nd)
{
   int i;
   if (nd->state == ND_S_ACTIVE)
   {
      for (i = 0; i < ND_MAX_CLIENTS; i++)
         if (nd->peers[i].active)
            nd_xmit(nd, nd->peers[i].mac, ND_T_BYE, nd->peers[i].id,
                    0, 0, NULL, 0);
   }
   for (i = 0; i < ND_MAX_CLIENTS; i++)
   {
      nd->peers[i].active = 0;
      arq_reset(&nd->peers[i]);
   }
   nd->state = ND_S_IDLE;
   nd->is_host = 0;
   nd->have_gen = 0;
   nd->fatal = 0;
}

int netdrv_send(netdrv *nd, int flags, const void *buf, size_t len,
                uint16_t dst_id)
{
   int i, rc = 0;

   if (!nd || nd->state != ND_S_ACTIVE || len == 0)
      return -1;

   if (len > ND_MAX_PAYLOAD)
   {
      /* NEVER truncate: a half payload is worse than none to a receiver
       * that validates exact lengths. The build's payload budget is wrong
       * for this workload — say so loudly and fail the session (ADR-0016). */
      nd->st.tx_oversize++;
      if (flags & ND_RELIABLE)
         nd->fatal = ND_STOP_TX_FAILED;
      nd_logf(nd, "send REFUSED: payload %u > ND_MAX_PAYLOAD %u (build "
                  "profile too small for this core)",
              (unsigned)len, (unsigned)ND_MAX_PAYLOAD);
      return -1;
   }

   if (flags & ND_RELIABLE)
   {
      if (dst_id == ND_BROADCAST_ID)
      {
         /* Reliable broadcast = per-peer ARQ fan-out (ADR-0003). */
         for (i = 0; i < ND_MAX_CLIENTS; i++)
            if (nd->peers[i].active)
               if (arq_enqueue(nd, &nd->peers[i], buf, (uint16_t)len) != 0)
                  rc = -1;
         return rc;
      }
      if (dst_id >= ND_MAX_CLIENTS || !nd->peers[dst_id].active)
         return -1;
      return arq_enqueue(nd, &nd->peers[dst_id], buf, (uint16_t)len);
   }

   /* Unreliable (cold path: the gpsp core never uses it). */
   {
      uint8_t type = (flags & ND_UNSEQUENCED) ? ND_T_UDATA_US : ND_T_UDATA;
      uint16_t seq = nd->utx_seq++;
      if (dst_id == ND_BROADCAST_ID)
      {
         nd_hdr h;
         size_t sz;
         h.type = type; h.src = nd->local_id; h.dst = ND_ID_BCAST;
         h.seq = seq; h.ack = 0; h.len = (uint16_t)len;
         sz = nd_wire_build(nd->scratch, &h, buf);
         nd->st.tx_frames++;
         nd->st.tx_bytes += (uint32_t)sz;
         return nd->tp.broadcast(nd->tp.ctx, nd->scratch, sz) < 0 ? -1 : 0;
      }
      if (dst_id >= ND_MAX_CLIENTS || !nd->peers[dst_id].active)
         return -1;
      nd_xmit(nd, nd->peers[dst_id].mac, type, (uint8_t)dst_id, seq,
              nd->peers[dst_id].rx_expect, buf, (uint16_t)len);
      nd->peers[dst_id].need_ack = 0;
      nd->peers[dst_id].last_tx_us = nd->now;
      return 0;
   }
}

/* Free slots before backpressure kicks in (ring only — the spill behind it
 * absorbs anything beyond, so this is a pacing hint, never a loss cliff). */
static int peer_capacity(const netdrv *nd, const nd_peer *p)
{
   uint32_t spill_cap = nd->cfg.spill_max ? nd->cfg.spill_max
                                          : (uint32_t)ND_SPILL_MAX;
   int32_t free_ring = (int32_t)ND_TXQ_CAP - (int32_t)p->txq_count;
   int32_t free_spill = (int32_t)spill_cap - (int32_t)p->spill_count;
   if (free_spill < 0)
      free_spill = 0;
   return (int)(free_ring + free_spill);
}

int netdrv_send_capacity(const netdrv *nd, uint16_t dst_id)
{
   int i, cap = ND_TXQ_CAP + (int)ND_SPILL_MAX;
   if (!nd || nd->state != ND_S_ACTIVE)
      return 0;
   if (dst_id == ND_BROADCAST_ID)
   {
      for (i = 0; i < ND_MAX_CLIENTS; i++)
         if (nd->peers[i].active && peer_capacity(nd, &nd->peers[i]) < cap)
            cap = peer_capacity(nd, &nd->peers[i]);
      return cap;
   }
   if (dst_id >= ND_MAX_CLIENTS || !nd->peers[dst_id].active)
      return 0;
   return peer_capacity(nd, &nd->peers[dst_id]);
}

int netdrv_active(const netdrv *nd)   { return nd && nd->state == ND_S_ACTIVE; }
int netdrv_local_id(const netdrv *nd)
{
   return (nd && nd->state == ND_S_ACTIVE) ? (int)nd->local_id : -1;
}
int netdrv_peer_count(const netdrv *nd)
{
   return nd ? nd_peer_count_(nd) : 0;
}

/* ---- peer frame-rate matching (ADR-0027) -------------------------------- */

void netdrv_set_local_fps(netdrv *nd, unsigned fps_x100)
{
   if (!nd)
      return;
   if (fps_x100 > 0xFFFFu)
      fps_x100 = 0xFFFFu;        /* 655.35 fps: clamp, never wrap */
   nd->local_fps_x100 = (uint16_t)fps_x100;
}

unsigned netdrv_peer_min_fps(const netdrv *nd)
{
   unsigned lo = 0;
   int i;
   if (!nd || nd->state != ND_S_ACTIVE)
      return 0;
   for (i = 0; i < ND_MAX_CLIENTS; i++)
   {
      const nd_peer *p = &nd->peers[i];
      if (!p->active || !p->fps_x100)
         continue;
      if (!lo || p->fps_x100 < lo)
         lo = p->fps_x100;
   }
   return lo;
}
void netdrv_get_stats(const netdrv *nd, nd_stats *out)
{
   int i;
   *out = nd->st;
   /* Live estimator readout: report the WORST (largest) values across
    * active peers — that is the peer whose link is closest to trouble. */
   out->srtt_us = out->rto_us = out->rtt_samples = out->rttvar_us = 0;
   out->txq_now = 0;
   for (i = 0; i < ND_MAX_CLIENTS; i++)
   {
      const nd_peer *p = &nd->peers[i];
      if (!p->active)
         continue;
      if (p->srtt_us > out->srtt_us)     out->srtt_us = p->srtt_us;
      if (p->rto_us  > out->rto_us)      out->rto_us  = p->rto_us;
      if (p->rttvar_us > out->rttvar_us) out->rttvar_us = p->rttvar_us;
      if (p->rtt_samples > out->rtt_samples) out->rtt_samples = p->rtt_samples;
      out->txq_now += (uint32_t)p->txq_count + p->spill_count;   /* ADR-0078 */
   }
}
