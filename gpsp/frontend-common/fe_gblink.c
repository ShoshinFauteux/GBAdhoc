/* fe_gblink.c -- GB/GBC link session: two machines per console, buttons in
 * lockstep over the network.  Protocol and invariants: fe_gblink.h. */
#include "fe_gblink.h"
#include "fe_evt.h"
#include "fe_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ wire -- */

enum
{
   M_HELLO = 0x01, M_CONFIG = 0x02, M_NEED = 0x03, M_ROM = 0x04,
   M_ROM_RESULT = 0x05, M_SAVE = 0x06, M_READY = 0x07, M_BULK_ACK = 0x08,
   M_STATE = 0x09,
   M_INPUT = 0x10, M_HASH = 0x11, M_END_REQ = 0x12, M_END_AT = 0x13,
   M_FINAL = 0x14, M_ABORT = 0x1F
};

#define HELLO_LEN   (1 + 1 + 1 + 1 + 1 + 4 + 20 + 0x1C + 4 + 2 + 4)
_Static_assert(HELLO_LEN == FE_GBLINK_HELLO_LEN, "fe_gblink.h HELLO size");
#define STATE_MAX   (1024u * 1024u)
#define CONFIG_LEN  (1 + 1 + 8 + 2 + 2)
#define RING        512u           /* input frames kept per machine */
#define HASH_RING   64u
#define CHUNK_HDR   5u             /* type + u32 offset */
#define ROOM_KEEP   8              /* leave this much send room for inputs */
#define HASH_READ   (64u * 1024u)  /* SHA-1 streaming per step */
/* ...and at most this long reading the memory stick in one step, when the
 * platform gave us a clock.  hw2 measured a 64 KiB step at 33-50 ms on both
 * consoles: the X host's 2 MiB cartridge made exactly 32 such frames, the
 * join's 1 MiB 16, the Y host's 8 MiB 128 -- the whole connecting screen at
 * 25 fps, which with the audio is what "looked crashed".  Reading is the
 * stick's pace either way; this only spreads it over more frames. */
#define IO_STEP_US  6000u
/* Frames between agreeing an end and reaching it, beyond 2 x delay.  The
 * guest cannot pass the host's frame + delay without inputs the host sends
 * AFTER its END_AT on the same ordered channel, so END_AT always arrives in
 * time; the slack only covers the guest's own step.  It was 300 (5 s of
 * "Ending the link..." every session, hw3). */
#define END_MARGIN  8u
/* Setup steps (display frames) to wait for the other console's HELLO before
 * giving up: hw3 run 9 waited 20+ minutes on "Waiting for the other
 * player..." with the two consoles in different ad-hoc cells. */
#define NO_PEER_STEPS (30u * 60u)
#define SRAM_EVERY  30u            /* cartridge-RAM check cadence (slot 0) */
#define SRAM_QUIET  120u           /* ...an end waits this long after a write */
#define END_DEFER_MAX 8u           /* ...but at most this many times: some
                                    * games keep cartridge RAM as scratch */
/* bulk lane */
#define BK_ROM      0
#define BK_SAVE     1
#define BK_STATE    2
#define BK_KINDS    3
#define BK_WINDOW   128u           /* chunks in flight beyond the first gap */
#define BK_ACK_MAP  256u           /* bitmap reach of one acknowledgement */
#define BK_ACK_LEN  (1 + 1 + 4 + BK_ACK_MAP / 8)
#define BK_ACK_EVERY 2u            /* steps between acknowledgements */
#define BK_RATE_START 4u
#define BK_RATE_MAX 16u
#define BK_EPOCH    16u            /* steps between rate decisions */
#define BK_MIN_CHUNK 64u
#define BK_INPUT    0x10           /* bulk-lane kind: redundant inputs */
/* bulk-lane kind: an acknowledgement (the M_BULK_ACK body).  hw2 sent them
 * on the ordered channel, where one lost netdrv packet holds every later
 * one until its retransmission (>= 50 ms): the sender's timer fired first,
 * resent chunks that had arrived (X: 263 of 1389 received were duplicates)
 * and counted them as loss, and the rate fell to 1-4 a frame.  Here a lost
 * acknowledgement costs nothing -- the next one (2 steps later) is
 * cumulative.  The one that completes a transfer also goes ordered. */
#define BK_ACKLANE  0x11
#define INPUT_COPIES_MAX 16u
#define RTC_TRAILER 48u
#define RTC_SPAN    (512 * 86400)
#define RTC_EPOCH_MIN 852076800

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* ----------------------------------------------------------- state -- */

typedef struct side
{
   uint32_t rom_size;
   uint8_t sha1[20];
   uint8_t header[0x1C];            /* ROM 0x134..0x14F */
   uint8_t console, palette;
   uint32_t save_size;
   uint32_t state_size;             /* 0 = powers on from the battery image */
} side_t;

struct fe_gblink
{
   fe_gblink_platform p;
   fe_gblink_local l;
   char rom_path[512];
   uint64_t (*now_us)(void);

   int state, error;
   int peer_up;
   int local_slot;

   /* own cartridge: read into memory and hashed incrementally at the
    * start.  It is allocated first, into the space the single machine's
    * copy just freed, and becomes this console's machine's ROM at power-on: loading
    * it at power-on instead put the partner's cartridge (allocated during
    * the transfer) in the middle of that space, and an 8 MB cartridge then
    * found no 8 MB block (PPSSPP, 2026-09-28: free > largest block). */
   side_t own;
   FILE *hash_f;
   uint8_t *own_buf;                 /* gbcore_rom_alloc; the machine's later */
   uint32_t own_read;
   fe_sha1_ctx hash_ctx;
   int own_hashed, hello_sent;
   uint8_t *own_save;
   size_t own_save_size;
   uint8_t *own_state;
   uint32_t state_tx_off;

   /* partner */
   side_t peer;
   int have_hello;
   char partner_path[512];
   int partner_ready;                /* its cartridge can be loaded */
   int verifying;                    /* hashing a library candidate */
   FILE *verify_f;
   fe_sha1_ctx verify_ctx;
   uint32_t verify_left;
   uint8_t *partner_buf;             /* received cartridge (gbcore_rom_alloc) */
   size_t partner_alloc;
   uint32_t partner_rx;
   fe_sha1_ctx rx_ctx;
   int need_sent;
   int peer_need;                    /* -1 unknown */
   int peer_rom_ok;                  /* -1 unknown */
   int rom_tx_began;
   uint32_t rom_tx_off;
   uint8_t *peer_save;
   uint32_t peer_save_rx;
   uint8_t *peer_state;
   uint32_t peer_state_rx;

   /* configuration (host decides) */
   int have_config;
   unsigned delay, hash_interval, batch;
   int64_t seed;
   uint32_t save_tx_off;
   int save_ready;                   /* own save re-based, can stream */
   int ready_sent, peer_ready;

   /* running */
   gbdual_t *dual;
   uint16_t q[GBDUAL_SLOTS][RING];
   uint64_t have[GBDUAL_SLOTS];      /* inputs known for frames < have */
   uint16_t pending_buttons;
   struct { uint32_t frame; uint64_t hash; } own_h[HASH_RING], peer_h[HASH_RING];
   uint32_t sram_crc[GBDUAL_SLOTS];
   uint8_t *crc_buf;                 /* cartridge RAM image for sram_crc */
   size_t crc_cap;
   uint32_t sram_changed_at;         /* slot-0 frame of the last change */

   /* end */
   uint32_t end_frame;               /* 0 = none agreed */
   unsigned end_deferrals;
   int end_requested;
   int captured;
   uint64_t final_hash;
   uint32_t final_frame;
   uint8_t *final_save;
   size_t final_save_size;
   uint8_t *final_state;
   size_t final_state_size;
   int peer_final;
   uint64_t peer_final_hash;
   uint32_t peer_final_frame;

   /* bulk lane (0 chunk = the ordered channel carries transfers) */
   uint32_t bk_chunk;
   uint32_t tick;                    /* setup steps (one a display frame) */
   uint32_t no_hello_steps;          /* setup steps without the peer's HELLO */
   struct bk_tx
   {
      const uint8_t *data;
      uint32_t size, n, cum, next_new;
      uint8_t *acked;                /* bit per chunk */
      uint16_t *sent_at;             /* tick (low 16 bits) of the last send */
      uint8_t *tries;
      int active, done;
   } bt[BK_KINDS];
   struct bk_rx
   {
      uint8_t *buf;
      uint32_t size, n, cum, count, hashed, acked_at;
      uint8_t *have;
      int active, dirty, done;
   } br[BK_KINDS];
   unsigned bk_rate;
   int bk_slow_start;
   uint32_t bk_epoch_at, bk_epoch_sent, bk_epoch_resent;
   uint32_t bk_srtt8;                /* ticks x 8 */
   uint32_t bk_rttvar4;              /* ticks x 4 */

   fe_gblink_stats st;
   uint32_t streak;
};

/* ---------------------------------------------------------- helpers -- */

static int sendm(fe_gblink *s, const uint8_t *b, size_t n)
{
   return s->p.send ? s->p.send(s->p.user, b, n) : -1;
}

static void fail(fe_gblink *s, int error, int tell_peer)
{
   uint8_t m[2];
   if (s->state == FE_GBLINK_DONE || s->state == FE_GBLINK_FAILED)
      return;
   s->state = FE_GBLINK_FAILED;
   s->error = error;
   fe_evt("gblink_failed reason=%s slot=%d f0=%llu f1=%llu",
          fe_gblink_error_text(error), s->local_slot,
          (unsigned long long)s->st.frames[0],
          (unsigned long long)s->st.frames[1]);
   if (tell_peer && s->peer_up)
   {
      m[0] = M_ABORT;
      m[1] = (uint8_t)error;
      sendm(s, m, sizeof(m));
   }
}

const char *fe_gblink_error_text(int e)
{
   switch (e)
   {
   case FE_GBLINK_OK:            return "ok";
   case FE_GBLINK_E_PROTO:       return "protocol";
   case FE_GBLINK_E_ROM_MISSING: return "rom_missing";
   case FE_GBLINK_E_ROM_BAD:     return "rom_hash_mismatch";
   case FE_GBLINK_E_MEMORY:      return "out_of_memory";
   case FE_GBLINK_E_START:       return "start";
   case FE_GBLINK_E_DESYNC:      return "desync";
   case FE_GBLINK_E_PEER_LOST:   return "peer_lost";
   case FE_GBLINK_E_PEER_ABORT:  return "peer_aborted";
   case FE_GBLINK_E_COMMIT:      return "save_write";
   case FE_GBLINK_E_LOCAL:       return "cancelled";
   case FE_GBLINK_E_NO_PEER:     return "no_peer";
   }
   return "?";
}

static void hex20(const uint8_t *h, char *out)
{
   static const char d[] = "0123456789abcdef";
   int i;
   for (i = 0; i < 20; i++)
   {
      out[i * 2] = d[h[i] >> 4];
      out[i * 2 + 1] = d[h[i] & 15];
   }
   out[40] = '\0';
}

static void title_of(const uint8_t header[0x1C], char out[17])
{
   int i;
   for (i = 0; i < 16; i++)
   {
      uint8_t c = header[i];
      if (c < 0x20 || c > 0x7E)
         break;
      out[i] = (char)c;
   }
   out[i] = '\0';
   while (i > 0 && out[i - 1] == ' ')
      out[--i] = '\0';
}

/* The clock trailer (gbcore.h: 5 live registers, 5 latched, u64 UNIX
 * time) re-based so it reads "stamped at `seed`": the live clock is first
 * advanced by the time since this console stamped it (as the core does at
 * load), so both consoles load the same clock, and the partner's clock does
 * not depend on how far apart the two consoles' clocks are set. */
static void rtc_rebase(uint8_t *img, size_t size, size_t ram, int64_t own_now,
                       int64_t seed)
{
   uint8_t *t;
   int64_t stamp, c, day;
   uint32_t high;
   if (size != ram + RTC_TRAILER || ram > size)
      return;
   t = img + ram;
   stamp = (int64_t)get64(t + 40);
   high = get32(t + 16);
   if (stamp >= RTC_EPOCH_MIN && stamp <= own_now && !(high & 0x40))
   {
      day = (int64_t)(get32(t + 12) & 0xff) | ((int64_t)(high & 1) << 8);
      c = (int64_t)(get32(t) % 60) + (int64_t)(get32(t + 4) % 60) * 60 +
          (int64_t)(get32(t + 8) % 24) * 3600 + day * 86400 +
          (own_now - stamp);
      if (c >= RTC_SPAN)
      {
         high |= 0x80;
         c %= RTC_SPAN;
      }
      day = c / 86400;
      put32(t, (uint32_t)(c % 60));
      put32(t + 4, (uint32_t)((c / 60) % 60));
      put32(t + 8, (uint32_t)((c / 3600) % 24));
      put32(t + 12, (uint32_t)(day & 0xff));
      put32(t + 16, (high & ~1u) | (uint32_t)((day >> 8) & 1));
   }
   put64(t + 40, (uint64_t)seed);
}

/* ------------------------------------------------------ bulk lane -- */

static int bit_get(const uint8_t *m, uint32_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t *m, uint32_t i) { m[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static uint32_t bk_len(uint32_t size, uint32_t chunk, uint32_t i)
{
   uint32_t off = i * chunk;
   return size - off < chunk ? size - off : chunk;
}

static void bk_free(fe_gblink *s)
{
   int k;
   for (k = 0; k < BK_KINDS; k++)
   {
      free(s->bt[k].acked);
      free(s->bt[k].sent_at);
      free(s->bt[k].tries);
      free(s->br[k].have);
      s->bt[k].acked = s->br[k].have = s->bt[k].tries = NULL;
      s->bt[k].sent_at = NULL;
   }
}

static int bk_tx_open(fe_gblink *s, int k, const uint8_t *data, uint32_t size)
{
   struct bk_tx *t = &s->bt[k];
   t->data = data;
   t->size = size;
   t->n = (size + s->bk_chunk - 1) / s->bk_chunk;
   t->acked = (uint8_t *)calloc(t->n / 8 + 1, 1);
   t->sent_at = (uint16_t *)calloc(t->n + 1, sizeof(uint16_t));
   t->tries = (uint8_t *)calloc(t->n + 1, 1);
   if (!t->acked || !t->sent_at || !t->tries)
   {
      fail(s, FE_GBLINK_E_MEMORY, 1);
      return -1;
   }
   t->active = 1;
   return 0;
}

static int bk_rx_open(fe_gblink *s, int k, uint8_t *buf, uint32_t size)
{
   struct bk_rx *r = &s->br[k];
   r->buf = buf;
   r->size = size;
   r->n = (size + s->bk_chunk - 1) / s->bk_chunk;
   r->have = (uint8_t *)calloc(r->n / 8 + 1, 1);
   if (!r->have)
   {
      fail(s, FE_GBLINK_E_MEMORY, 1);
      return -1;
   }
   r->active = 1;
   return 0;
}

static unsigned bk_rto(const fe_gblink *s)
{
   /* Jacobson/Karels: smoothed round trip + 4 x its deviation, 4..240
    * steps; 30 before the first sample (a PSP round trip is unknown). */
   uint32_t r = s->bk_srtt8 ? s->bk_srtt8 / 8 + s->bk_rttvar4 + 1 : 30;
   return r < 4 ? 4 : r > 240 ? 240 : r;
}

static void bk_send_chunk(fe_gblink *s, int k, uint32_t i)
{
   uint8_t d[FE_GBLINK_BULK_HDR + 1500];
   struct bk_tx *t = &s->bt[k];
   uint32_t n = bk_len(t->size, s->bk_chunk, i);
   memcpy(d, FE_GBLINK_BULK_MAGIC, 4);
   d[4] = (uint8_t)k;
   put32(d + 5, i);
   put32(d + 9, fe_crc32(0, t->data + (size_t)i * s->bk_chunk, n));
   memcpy(d + FE_GBLINK_BULK_HDR, t->data + (size_t)i * s->bk_chunk, n);
   s->p.bulk_send(s->p.user, d, FE_GBLINK_BULK_HDR + n);
   if (t->tries[i] < 255)
      t->tries[i]++;
   t->sent_at[i] = (uint16_t)s->tick;
   s->st.bulk_sent++;
}

/* One step of the sender, both kinds sharing the rate: resends of what has
 * been out longer than the retransmission time and still is not
 * acknowledged, then new chunks up to the window.  The rate adapts (AIMD)
 * unless the platform fixes it. */
/* A send step's time limit (with a clock): each PdpSend is paid by the
 * emulation thread, ~0.4-0.8 ms on hardware (hw2 sess_cost), so the full
 * 16-datagram rate could by itself take a frame's budget. */
#define BK_TX_STEP_US 8000u

static void bk_tx_step(fe_gblink *s)
{
   unsigned budget, rto = bk_rto(s);
   uint32_t sent0 = s->st.bulk_sent, resent0 = s->st.bulk_resent;
   uint64_t t0 = s->now_us ? s->now_us() : 0;
   int kk;
   if (s->p.bulk_rate)
      s->bk_rate = s->p.bulk_rate;
   budget = s->bk_rate;
   for (kk = 0; kk < BK_KINDS && budget; kk++)
   {
      /* the save and the state (small) ahead of the cartridge */
      static const int order[BK_KINDS] = { BK_SAVE, BK_STATE, BK_ROM };
      int k = order[kk];
      struct bk_tx *t = &s->bt[k];
      uint32_t i;
      if (!t->active || t->done)
         continue;
      for (i = t->cum; i < t->next_new && budget; i++)
         if (!bit_get(t->acked, i) &&
             (uint16_t)((uint16_t)s->tick - t->sent_at[i]) >= rto)
         {
            bk_send_chunk(s, k, i);
            s->st.bulk_resent++;
            budget--;
            if (s->now_us && s->now_us() - t0 >= BK_TX_STEP_US)
               budget = 0;
         }
      while (budget && t->next_new < t->n && t->next_new < t->cum + BK_WINDOW)
      {
         bk_send_chunk(s, k, t->next_new++);
         budget--;
         if (s->now_us && s->now_us() - t0 >= BK_TX_STEP_US)
            budget = 0;
      }
   }
   /* The rate follows the share of datagrams that had to be resent over
    * the last epoch -- not single losses: the PSP radio loses a few per
    * cent at random (hw1: ~3 %), which says nothing about congestion.
    * Doubling while under 6 %, then +1; over 12 % -25 %, over 25 % -50 %. */
   s->bk_epoch_sent += s->st.bulk_sent - sent0;
   s->bk_epoch_resent += s->st.bulk_resent - resent0;
   if (!s->p.bulk_rate && s->tick - s->bk_epoch_at >= BK_EPOCH &&
       s->bk_epoch_sent)
   {
      unsigned mx = s->p.bulk_rate_max ? s->p.bulk_rate_max : BK_RATE_MAX;
      uint32_t pct = s->bk_epoch_resent * 100u / s->bk_epoch_sent;
      if (pct > 25)
      {
         s->bk_rate = s->bk_rate > 1 ? s->bk_rate / 2 : 1;
         s->bk_slow_start = 0;
      }
      else if (pct > 12)
      {
         s->bk_rate -= s->bk_rate / 4;
         s->bk_slow_start = 0;
      }
      else if (pct < 6)
         s->bk_rate = s->bk_slow_start ? s->bk_rate * 2 : s->bk_rate + 1;
      if (s->bk_rate > mx)
         s->bk_rate = mx;
      if (!s->bk_rate)
         s->bk_rate = 1;
      s->bk_epoch_at = s->tick;
      s->bk_epoch_sent = s->bk_epoch_resent = 0;
   }
   s->st.bulk_rate = s->bk_rate;
}

static void bk_on_ack(fe_gblink *s, const uint8_t *b, size_t len)
{
   int k;
   struct bk_tx *t;
   uint32_t cum, i, sample = 0;
   int have_sample = 0;
   if (len != BK_ACK_LEN || b[1] >= BK_KINDS || !s->bt[b[1]].active)
   {
      fail(s, FE_GBLINK_E_PROTO, 1);
      return;
   }
   k = b[1];
   t = &s->bt[k];
   cum = get32(b + 2);
   if (cum > t->n)
   {
      fail(s, FE_GBLINK_E_PROTO, 1);
      return;
   }
   for (i = t->cum; i < cum; i++)
      if (!bit_get(t->acked, i))
      {
         bit_set(t->acked, i);
         if (t->tries[i] == 1)
         {
            sample = (uint16_t)((uint16_t)s->tick - t->sent_at[i]);
            have_sample = 1;
         }
      }
   for (i = 0; i < BK_ACK_MAP && cum + 1 + i < t->n; i++)
      if ((b[6 + i / 8] >> (i & 7)) & 1)
         bit_set(t->acked, cum + 1 + i);
   if (cum > t->cum)
      t->cum = cum;
   while (t->cum < t->n && bit_get(t->acked, t->cum))
      t->cum++;
   if (have_sample)
   {
      if (!s->bk_srtt8)
      {
         s->bk_srtt8 = sample * 8;
         s->bk_rttvar4 = sample * 2;        /* sample / 2, x 4 */
      }
      else
      {
         int32_t err = (int32_t)sample - (int32_t)(s->bk_srtt8 / 8);
         uint32_t aerr = (uint32_t)(err < 0 ? -err : err);
         s->bk_srtt8 = (uint32_t)((int32_t)s->bk_srtt8 + err);
         s->bk_rttvar4 = s->bk_rttvar4 - s->bk_rttvar4 / 4 + aerr;
      }
   }
   if (k == BK_ROM)
      s->rom_tx_off = t->cum >= t->n ? t->size : t->cum * s->bk_chunk;
   else if (k == BK_SAVE)
      s->save_tx_off = t->cum >= t->n ? t->size : t->cum * s->bk_chunk;
   else
      s->state_tx_off = t->cum >= t->n ? t->size : t->cum * s->bk_chunk;
   s->st.rom_tx_bytes = s->rom_tx_off;
   s->st.save_tx_bytes = s->save_tx_off;
   if (t->cum >= t->n && !t->done)
   {
      t->done = 1;
      if (k == BK_ROM)
         s->st.rom_sent = 1;
      fe_evt("gblink_bulk_sent kind=%s size=%u chunk=%u sent=%u resent=%u "
             "rate=%u srtt=%u",
             k == BK_ROM ? "rom" : k == BK_SAVE ? "save" : "state",
             (unsigned)t->size, (unsigned)s->bk_chunk,
             (unsigned)s->st.bulk_sent, (unsigned)s->st.bulk_resent,
             s->bk_rate, (unsigned)(s->bk_srtt8 / 8));
      if (k == BK_ROM)
         fe_evt("gblink_rom_send done size=%u", (unsigned)t->size);
   }
}

static void bk_send_ack(fe_gblink *s, int k)
{
   struct bk_rx *r = &s->br[k];
   uint8_t m[BK_ACK_LEN];
   uint32_t i;
   memset(m, 0, sizeof(m));
   m[0] = M_BULK_ACK;
   m[1] = (uint8_t)k;
   put32(m + 2, r->cum);
   for (i = 0; i < BK_ACK_MAP && r->cum + 1 + i < r->n; i++)
      if (bit_get(r->have, r->cum + 1 + i))
         m[6 + i / 8] |= (uint8_t)(1u << (i & 7));
   if (s->p.bulk_send)
   {
      uint8_t d[5 + BK_ACK_LEN];
      memcpy(d, FE_GBLINK_BULK_MAGIC, 4);
      d[4] = BK_ACKLANE;
      memcpy(d + 5, m, sizeof(m));
      s->p.bulk_send(s->p.user, d, sizeof(d));
   }
   if (!s->p.bulk_send || r->cum >= r->n)
      sendm(s, m, sizeof(m));
   r->dirty = 0;
   r->acked_at = s->tick;
}

static void rom_complete(fe_gblink *s);
static int take_input(fe_gblink *s, uint32_t f, uint16_t buttons, int fast);

/* The receiver's step: acknowledge, and hash the in-order prefix of a
 * cartridge as it grows (the SHA-1 is streamed, never a stall at the end). */
static void bk_rx_step(fe_gblink *s)
{
   int k;
   for (k = 0; k < BK_KINDS; k++)
   {
      struct bk_rx *r = &s->br[k];
      if (!r->active)
         continue;
      if (k == BK_ROM && !r->done)
      {
         uint32_t upto = r->cum >= r->n ? r->size : r->cum * s->bk_chunk;
         uint32_t n = upto - r->hashed;
         if (n > HASH_READ)
            n = HASH_READ;
         if (n)
         {
            fe_sha1_update(&s->rx_ctx, r->buf + r->hashed, n);
            r->hashed += n;
         }
         if (r->hashed >= r->size)
         {
            r->done = 1;
            rom_complete(s);
            if (s->state == FE_GBLINK_FAILED)
               return;
         }
      }
      if (r->dirty && s->tick - r->acked_at >= BK_ACK_EVERY)
         bk_send_ack(s, k);
   }
}

void fe_gblink_receive_bulk(fe_gblink *s, const void *buf, size_t len)
{
   const uint8_t *b = (const uint8_t *)buf;
   struct bk_rx *r;
   uint32_t i, n;
   int k;
   if (!s)
      return;
   if (len >= 10 && !memcmp(b, FE_GBLINK_BULK_MAGIC, 4) && b[4] == BK_INPUT)
   {
      uint32_t first = get32(b + 5), c = b[9], j;
      if (!s->have_config || c > INPUT_COPIES_MAX || len != 10 + 2 * c ||
          (s->state != FE_GBLINK_SETUP && s->state != FE_GBLINK_RUNNING &&
           s->state != FE_GBLINK_ENDING))
      {
         s->st.bulk_bad++;
         return;
      }
      for (j = 0; j < c; j++)
         if (take_input(s, first + j, (uint16_t)get16(b + 10 + 2 * j), 1) < 0)
            break;                    /* a gap: the ordered copy fills it */
      return;
   }
   if (s->state != FE_GBLINK_SETUP)
      return;
   if (len == 5 + BK_ACK_LEN && !memcmp(b, FE_GBLINK_BULK_MAGIC, 4) &&
       b[4] == BK_ACKLANE)
   {
      if (b[5] != M_BULK_ACK || b[6] >= BK_KINDS || !s->bt[b[6]].active)
      {
         s->st.bulk_bad++;
         return;
      }
      bk_on_ack(s, b + 5, BK_ACK_LEN);
      return;
   }
   if (len <= FE_GBLINK_BULK_HDR || memcmp(b, FE_GBLINK_BULK_MAGIC, 4) ||
       b[4] >= BK_KINDS || !s->br[b[4]].active)
   {
      s->st.bulk_bad++;
      return;
   }
   k = b[4];
   r = &s->br[k];
   i = get32(b + 5);
   n = (uint32_t)(len - FE_GBLINK_BULK_HDR);
   if (i >= r->n || n != bk_len(r->size, s->bk_chunk, i) ||
       fe_crc32(0, b + FE_GBLINK_BULK_HDR, n) != get32(b + 9))
   {
      s->st.bulk_bad++;
      return;
   }
   s->st.bulk_rx++;
   r->dirty = 1;
   if (bit_get(r->have, i))
   {
      s->st.bulk_dup++;
      return;
   }
   if (!r->count && k == BK_ROM && s->now_us)
      s->st.rom_rx_first_us = s->now_us();
   memcpy(r->buf + (size_t)i * s->bk_chunk, b + FE_GBLINK_BULK_HDR, n);
   bit_set(r->have, i);
   r->count++;
   while (r->cum < r->n && bit_get(r->have, r->cum))
      r->cum++;
   if (k == BK_ROM)
   {
      s->partner_rx = r->count >= r->n ? r->size : r->count * s->bk_chunk;
      s->st.rom_rx_bytes = s->partner_rx;
   }
   else if (r->count >= r->n && !r->done)
   {
      r->done = 1;
      if (k == BK_SAVE)
      {
         s->peer_save_rx = r->size;
         s->st.save_rx_bytes = r->size;
      }
      else
         s->peer_state_rx = r->size;
      bk_send_ack(s, k);
   }
}

/* The whole partner cartridge is here (either lane): check its SHA-1. */
static void rom_complete(fe_gblink *s)
{
   uint8_t d[20], m[2];
   if (s->now_us)
      s->st.rom_rx_last_us = s->now_us();
   s->partner_rx = s->peer.rom_size;
   s->st.rom_rx_bytes = s->partner_rx;
   fe_sha1_final(&s->rx_ctx, d);
   m[0] = M_ROM_RESULT;
   m[1] = memcmp(d, s->peer.sha1, 20) == 0;
   sendm(s, m, 2);
   fe_evt("gblink_rom_received size=%u ok=%u us=%llu chunk=%u rx=%u dup=%u "
          "bad=%u", s->partner_rx, m[1],
          (unsigned long long)(s->st.rom_rx_last_us - s->st.rom_rx_first_us),
          (unsigned)s->bk_chunk, (unsigned)s->st.bulk_rx,
          (unsigned)s->st.bulk_dup, (unsigned)s->st.bulk_bad);
   if (!m[1])
   {
      fail(s, FE_GBLINK_E_ROM_BAD, 0);
      return;
   }
   s->partner_ready = 1;
   s->st.rom_received = 1;
}

/* ------------------------------------------------------ lifecycle -- */

fe_gblink *fe_gblink_create(const fe_gblink_platform *p,
                            const fe_gblink_local *l)
{
   fe_gblink *s;
   long n;
   if (!p || !l || !l->rom_path || !p->send || p->max_payload < 32)
      return NULL;
   s = (fe_gblink *)calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->p = *p;
   s->l = *l;
   snprintf(s->rom_path, sizeof(s->rom_path), "%s", l->rom_path);
   s->l.rom_path = s->rom_path;
   s->local_slot = l->is_host ? 0 : 1;
   s->peer_need = -1;
   s->peer_rom_ok = -1;
   s->bk_rate = BK_RATE_START;
   s->bk_slow_start = 1;
   s->st.local_slot = s->local_slot;
   s->hash_f = fopen(s->rom_path, "rb");
   if (!s->hash_f || fseek(s->hash_f, 0, SEEK_END) != 0 ||
       (n = ftell(s->hash_f)) < 0x150 || n > 8 * 1024 * 1024 ||
       fseek(s->hash_f, 0x134, SEEK_SET) != 0 ||
       fread(s->own.header, 1, 0x1C, s->hash_f) != 0x1C ||
       fseek(s->hash_f, 0, SEEK_SET) != 0)
   {
      if (s->hash_f)
         fclose(s->hash_f);
      free(s);
      return NULL;
   }
   s->own.rom_size = (uint32_t)n;
   s->own_buf = gbcore_rom_alloc((size_t)n, NULL);
   if (!s->own_buf)
   {
      fe_evt("gblink_create_fail reason=memory rom_size=%u", (unsigned)n);
      fclose(s->hash_f);
      free(s);
      return NULL;
   }
   s->own.console = (uint8_t)l->console;
   s->own.palette = (uint8_t)l->palette;
   s->own.save_size = (uint32_t)l->save_size;
   fe_sha1_init(&s->hash_ctx);
   if (l->save_size)
   {
      s->own_save = (uint8_t *)malloc(l->save_size);
      if (!s->own_save)
      {
         fclose(s->hash_f);
         free(s->own_buf);
         free(s);
         return NULL;
      }
      memcpy(s->own_save, l->save, l->save_size);
   }
   s->own_save_size = l->save_size;
   if (l->state && l->state_size && l->state_size <= STATE_MAX)
   {
      s->own_state = (uint8_t *)malloc(l->state_size);
      if (!s->own_state)
      {
         fclose(s->hash_f);
         free(s->own_buf);
         free(s->own_save);
         free(s);
         return NULL;
      }
      memcpy(s->own_state, l->state, l->state_size);
      s->own.state_size = (uint32_t)l->state_size;
   }
   fe_evt("gblink_create role=%s rom_size=%u save_size=%u state_size=%u",
          l->is_host ? "host" : "guest", (unsigned)n,
          (unsigned)l->save_size, (unsigned)s->own.state_size);
   return s;
}

/* This console's clock in ms for the end-path events (t_ms=): the two
 * consoles' clocks differ, so only durations on one console are compared.
 * static inline: unused (no warning) when fe_evt compiles out. */
static inline unsigned long long evt_ms(const fe_gblink *s)
{
   return s->now_us ? (unsigned long long)(s->now_us() / 1000u) : 0ull;
}

void fe_gblink_destroy(fe_gblink *s)
{
   if (!s)
      return;
   if (s->hash_f) fclose(s->hash_f);
   if (s->verify_f) fclose(s->verify_f);
   gbdual_destroy(s->dual);
   bk_free(s);
   free(s->own_buf);
   free(s->crc_buf);
   free(s->partner_buf);
   free(s->own_save);
   free(s->peer_save);
   free(s->final_save);
   free(s->own_state);
   free(s->peer_state);
   free(s->final_state);
   free(s);
}

const void *fe_gblink_final_save(const fe_gblink *s, size_t *size)
{
   int ok = s && s->state == FE_GBLINK_DONE && s->final_save_size;
   if (size)
      *size = ok ? s->final_save_size : 0;
   return ok ? s->final_save : NULL;
}

const void *fe_gblink_final_state(const fe_gblink *s, size_t *size)
{
   int ok = s && s->state == FE_GBLINK_DONE && s->final_state_size;
   if (size)
      *size = ok ? s->final_state_size : 0;
   return ok ? s->final_state : NULL;
}

void fe_gblink_set_clock(fe_gblink *s, uint64_t (*now_us)(void))
{
   if (s)
      s->now_us = now_us;
}

/* -------------------------------------------------------- setup -- */

static void send_hello(fe_gblink *s)
{
   uint8_t m[HELLO_LEN];
   uint8_t *q = m;
   *q++ = M_HELLO;
   *q++ = FE_GBLINK_PROTO;
   *q++ = (uint8_t)(s->l.is_host ? 1 : 0);
   *q++ = s->own.console;
   *q++ = s->own.palette;
   put32(q, s->own.rom_size); q += 4;
   memcpy(q, s->own.sha1, 20); q += 20;
   memcpy(q, s->own.header, 0x1C); q += 0x1C;
   put32(q, s->own.save_size); q += 4;
   put16(q, s->p.bulk_send &&
               s->p.bulk_max > FE_GBLINK_BULK_HDR + BK_MIN_CHUNK &&
               s->p.bulk_max <= FE_GBLINK_BULK_HDR + 1500
            ? (uint32_t)s->p.bulk_max : 0u); q += 2;
   put32(q, s->own.state_size);
   sendm(s, m, sizeof(m));
   s->hello_sent = 1;
}

static void send_config(fe_gblink *s)
{
   uint8_t m[CONFIG_LEN];
   m[0] = M_CONFIG;
   m[1] = (uint8_t)s->delay;
   put64(m + 2, (uint64_t)s->seed);
   put16(m + 10, s->hash_interval);
   put16(m + 12, s->batch);
   sendm(s, m, sizeof(m));
}

static void config_known(fe_gblink *s)
{
   s->have_config = 1;
   /* Input queues exist from here: the partner may start (and send its
    * first buttons) a moment before we do. */
   s->have[0] = s->have[1] = s->delay;
   rtc_rebase(s->own_save, s->own_save_size,
              s->own_save_size >= RTC_TRAILER &&
              s->own_save_size % 0x100 == RTC_TRAILER % 0x100
                 ? s->own_save_size - RTC_TRAILER : s->own_save_size,
              s->l.wallclock_now, s->seed);
   s->save_ready = 1;
   s->st.input_delay = s->delay;
   fe_evt("gblink_config delay=%u seed=%lld hash_every=%u batch=%u",
          s->delay, (long long)s->seed, s->hash_interval, s->batch);
}

static void send_need(fe_gblink *s, int need)
{
   uint8_t m[2] = { M_NEED, (uint8_t)need };
   sendm(s, m, sizeof(m));
   s->need_sent = 1;
}

static void partner_lookup(fe_gblink *s)
{
   char h[41];
   if (!memcmp(s->peer.sha1, s->own.sha1, 20) &&
       s->peer.rom_size == s->own.rom_size)
   {
      snprintf(s->partner_path, sizeof(s->partner_path), "%s", s->rom_path);
      s->partner_ready = 1;
      send_need(s, 0);
      fe_evt("gblink_partner_rom source=same_as_own");
      return;
   }
   if (s->p.find_rom &&
       s->p.find_rom(s->p.user, s->peer.rom_size, s->peer.header,
                     s->partner_path, sizeof(s->partner_path)) &&
       (s->verify_f = fopen(s->partner_path, "rb")) != NULL)
   {
      fe_sha1_init(&s->verify_ctx);
      s->verify_left = s->peer.rom_size;
      s->verifying = 1;
      return;                        /* NEED follows the hash check */
   }
   hex20(s->peer.sha1, h);
   fe_evt("gblink_partner_rom source=transfer size=%u sha1=%s",
          (unsigned)s->peer.rom_size, h);
   s->partner_buf = gbcore_rom_alloc(s->peer.rom_size, &s->partner_alloc);
   if (!s->partner_buf)
   {
      fail(s, FE_GBLINK_E_MEMORY, 1);
      return;
   }
   fe_sha1_init(&s->rx_ctx);
   s->st.rom_total = s->peer.rom_size;
   send_need(s, 1);
}

/* A stick-reading step is over: its bytes, or (with a clock) its time. */
static int io_step_over(const fe_gblink *s, uint64_t t0, uint32_t budget)
{
   return !budget || (s->now_us && s->now_us() - t0 >= IO_STEP_US);
}

static void verify_step(fe_gblink *s)
{
   uint8_t buf[4096], d[20];
   uint32_t budget = HASH_READ;
   uint64_t t0 = s->now_us ? s->now_us() : 0;
   int bad = 0;
   while (s->verify_left && !io_step_over(s, t0, budget))
   {
      size_t want = s->verify_left < sizeof(buf) ? s->verify_left : sizeof(buf);
      if (fread(buf, 1, want, s->verify_f) != want)
      {
         bad = 1;                     /* shorter than it claimed */
         break;
      }
      fe_sha1_update(&s->verify_ctx, buf, want);
      s->verify_left -= (uint32_t)want;
      budget = budget > want ? budget - (uint32_t)want : 0;
   }
   if (!bad && s->verify_left)
      return;                         /* more next frame */
   fclose(s->verify_f);
   s->verify_f = NULL;
   s->verifying = 0;
   if (!bad)
   {
      fe_sha1_final(&s->verify_ctx, d);
      if (!memcmp(d, s->peer.sha1, 20))
      {
         s->partner_ready = 1;
         send_need(s, 0);
         fe_evt("gblink_partner_rom source=library path=%s", s->partner_path);
         return;
      }
   }
   fe_evt("gblink_partner_rom library_candidate_rejected path=%s",
          s->partner_path);
   s->partner_path[0] = '\0';
   s->partner_buf = gbcore_rom_alloc(s->peer.rom_size, &s->partner_alloc);
   if (!s->partner_buf)
   {
      fail(s, FE_GBLINK_E_MEMORY, 1);
      return;
   }
   fe_sha1_init(&s->rx_ctx);
   s->st.rom_total = s->peer.rom_size;
   send_need(s, 1);
}

static size_t chunk_data(const fe_gblink *s)
{
   return s->p.max_payload - CHUNK_HDR;
}

static int room(const fe_gblink *s)
{
   return s->p.send_room ? s->p.send_room(s->p.user) : 64;
}

static unsigned bulk_budget(const fe_gblink *s)
{
   return s->p.bulk_per_step ? s->p.bulk_per_step : 0xFFFFFFFFu;
}

static void stream_rom(fe_gblink *s, unsigned *budget)
{
   uint8_t m[1024 + CHUNK_HDR];
   size_t cd = chunk_data(s);
   if (cd > 1024)
      cd = 1024;
   if (!s->own_buf)
   {
      fail(s, FE_GBLINK_E_ROM_MISSING, 1);
      return;
   }
   if (!s->rom_tx_began)
   {
      s->rom_tx_began = 1;
      fe_evt("gblink_rom_send begin size=%u", (unsigned)s->own.rom_size);
   }
   while (s->rom_tx_off < s->own.rom_size && room(s) > ROOM_KEEP && *budget)
   {
      size_t n = s->own.rom_size - s->rom_tx_off;
      if (n > cd)
         n = cd;
      m[0] = M_ROM;
      put32(m + 1, s->rom_tx_off);
      memcpy(m + CHUNK_HDR, s->own_buf + s->rom_tx_off, n);
      if (sendm(s, m, n + CHUNK_HDR) != 0)
         break;
      s->rom_tx_off += (uint32_t)n;
      s->st.rom_tx_bytes = s->rom_tx_off;
      (*budget)--;
   }
   if (s->rom_tx_off >= s->own.rom_size)
   {
      s->st.rom_sent = 1;
      fe_evt("gblink_rom_send done size=%u", (unsigned)s->own.rom_size);
   }
}

static void stream_save(fe_gblink *s, unsigned *budget)
{
   uint8_t m[1024 + CHUNK_HDR];
   size_t cd = chunk_data(s);
   if (cd > 1024)
      cd = 1024;
   while (s->save_tx_off < s->own_save_size && room(s) > ROOM_KEEP &&
          *budget)
   {
      size_t n = s->own_save_size - s->save_tx_off;
      if (n > cd)
         n = cd;
      m[0] = M_SAVE;
      put32(m + 1, s->save_tx_off);
      memcpy(m + CHUNK_HDR, s->own_save + s->save_tx_off, n);
      if (sendm(s, m, n + CHUNK_HDR) != 0)
         break;
      s->save_tx_off += (uint32_t)n;
      s->st.save_tx_bytes = s->save_tx_off;
      (*budget)--;
   }
}

static void stream_state(fe_gblink *s, unsigned *budget)
{
   uint8_t m[1024 + CHUNK_HDR];
   size_t cd = chunk_data(s);
   if (cd > 1024)
      cd = 1024;
   while (s->state_tx_off < s->own.state_size && room(s) > ROOM_KEEP &&
          *budget)
   {
      size_t n = s->own.state_size - s->state_tx_off;
      if (n > cd)
         n = cd;
      m[0] = M_STATE;
      put32(m + 1, s->state_tx_off);
      memcpy(m + CHUNK_HDR, s->own_state + s->state_tx_off, n);
      if (sendm(s, m, n + CHUNK_HDR) != 0)
         break;
      s->state_tx_off += (uint32_t)n;
      (*budget)--;
   }
}

static void frame_hook(void *user, int slot, uint64_t frame);
static void on_video(void *user, const gbcore_video_frame_t *f)
{
   fe_gblink *s = (fe_gblink *)user;
   if (s->p.video)
      s->p.video(s->p.user, f);
}
static void on_audio(void *user, const int16_t *a, size_t n)
{
   fe_gblink *s = (fe_gblink *)user;
   if (s->p.audio)
      s->p.audio(s->p.user, a, n);
}

static void start_pair(fe_gblink *s)
{
   gbdual_config_t cfg;
   int slot;
   memset(&cfg, 0, sizeof(cfg));
   for (slot = 0; slot < GBDUAL_SLOTS; slot++)
   {
      gbdual_machine_t *m = &cfg.machine[slot];
      int mine = slot == s->local_slot;
      const side_t *sd = mine ? &s->own : &s->peer;
      if (mine)
      {
         m->rom_owned = s->own_buf;
         m->rom_size = s->own.rom_size;
         s->own_buf = NULL;            /* the machine owns it now */
      }
      else if (s->partner_buf)
      {
         m->rom_owned = s->partner_buf;
         m->rom_size = s->peer.rom_size;
         s->partner_buf = NULL;        /* the machine owns it now */
      }
      else
         m->rom_path = s->partner_path;
      m->console = (fe_console_t)sd->console;
      m->palette = sd->palette;
      m->headless = !mine;
      m->save = mine ? s->own_save : s->peer_save;
      m->save_size = mine ? s->own_save_size : sd->save_size;
      if (mine)
      {
         m->callbacks.userdata = s;
         m->callbacks.video = on_video;
         m->callbacks.audio_batch = on_audio;
      }
   }
   cfg.audio_rate = s->l.audio_rate ? s->l.audio_rate : 32768;
   cfg.link = 1;
   cfg.pace_slot = s->l.pace_slot0 ? 0 : s->local_slot;
   cfg.rtc_seed = s->seed;
   cfg.frame_hook = frame_hook;
   cfg.frame_hook_userdata = s;
   cfg.batch_lines = s->batch;
   s->dual = gbdual_create(&cfg);
   if (!s->dual)
   {
      fail(s, FE_GBLINK_E_START, 1);
      return;
   }
   for (slot = 0; slot < GBDUAL_SLOTS; slot++)
   {
      int mine = slot == s->local_slot;
      const uint8_t *st = mine ? s->own_state : s->peer_state;
      uint32_t n = mine ? s->own.state_size : s->peer.state_size;
      if (n && gbdual_api(slot)->state_load(gbdual_core(s->dual, slot), st,
                                            n) != 0)
      {
         fe_evt("gblink_state_load FAILED slot=%d size=%u", slot, (unsigned)n);
         fail(s, FE_GBLINK_E_START, 1);
         return;
      }
   }
   s->state = FE_GBLINK_RUNNING;
   fe_evt("gblink_start slot=%d delay=%u seed=%lld partner=%s batch=%u",
          s->local_slot, s->delay, (long long)s->seed,
          s->partner_path[0] ? "library" : "received", s->batch);
}

static int setup_complete(const fe_gblink *s)
{
   return s->have_config && s->partner_ready &&
          s->peer_save_rx >= s->peer.save_size &&
          s->save_tx_off >= s->own_save_size &&
          s->peer_state_rx >= s->peer.state_size &&
          s->state_tx_off >= s->own.state_size &&
          (s->peer_need == 0 ||
           (s->peer_need == 1 && s->rom_tx_off >= s->own.rom_size &&
            s->peer_rom_ok == 1));
}

static void setup_step(fe_gblink *s)
{
   if (!s->own_hashed)
   {
      uint32_t budget = HASH_READ;
      uint64_t t0 = s->now_us ? s->now_us() : 0;
      while (s->own_read < s->own.rom_size && !io_step_over(s, t0, budget))
      {
         size_t want = s->own.rom_size - s->own_read, got;
         if (want > 4096)
            want = 4096;
         got = fread(s->own_buf + s->own_read, 1, want, s->hash_f);
         if (got != want)
         {
            fail(s, FE_GBLINK_E_ROM_MISSING, 1);
            return;
         }
         fe_sha1_update(&s->hash_ctx, s->own_buf + s->own_read, got);
         s->own_read += (uint32_t)got;
         budget = budget > got ? budget - (uint32_t)got : 0;
      }
      if (s->own_read < s->own.rom_size)
         return;
      fe_sha1_final(&s->hash_ctx, s->own.sha1);
      fclose(s->hash_f);
      s->hash_f = NULL;
      s->own_hashed = 1;
      {
         char h[41];
         FE_EVT_ONLY(h);
         hex20(s->own.sha1, h);
         fe_evt("gblink_own_rom sha1=%s size=%u", h,
                (unsigned)s->own.rom_size);
      }
   }
   if (!s->peer_up)
      return;
   if (!s->hello_sent)
      send_hello(s);
   if (!s->have_hello)
      return;
   if (s->verifying)
      verify_step(s);
   s->tick++;
   if (s->bk_chunk)
   {
      /* Open each direction's transfers once what they carry exists. */
      if (!s->bt[BK_SAVE].active && s->save_ready && s->own_save_size &&
          bk_tx_open(s, BK_SAVE, s->own_save, (uint32_t)s->own_save_size))
         return;
      if (!s->bt[BK_STATE].active && s->have_config && s->own.state_size &&
          bk_tx_open(s, BK_STATE, s->own_state, s->own.state_size))
         return;
      if (!s->br[BK_STATE].active && s->peer.state_size && s->peer_state &&
          bk_rx_open(s, BK_STATE, s->peer_state, s->peer.state_size))
         return;
      if (!s->bt[BK_ROM].active && s->peer_need == 1 &&
          bk_tx_open(s, BK_ROM, s->own_buf, s->own.rom_size))
         return;
      if (!s->br[BK_SAVE].active && s->peer.save_size && s->peer_save &&
          bk_rx_open(s, BK_SAVE, s->peer_save, s->peer.save_size))
         return;
      if (!s->br[BK_ROM].active && s->partner_buf && !s->partner_ready &&
          !s->verifying &&
          bk_rx_open(s, BK_ROM, s->partner_buf, s->peer.rom_size))
         return;
      if (s->bt[BK_ROM].active && !s->rom_tx_began)
      {
         s->rom_tx_began = 1;
         fe_evt("gblink_rom_send begin size=%u chunk=%u",
                (unsigned)s->own.rom_size, (unsigned)s->bk_chunk);
      }
      bk_rx_step(s);
      if (s->state == FE_GBLINK_FAILED)
         return;
      bk_tx_step(s);
   }
   else
   {
      unsigned budget = bulk_budget(s);
      if (s->save_ready)
         stream_save(s, &budget);      /* small: first */
      if (s->have_config)
         stream_state(s, &budget);
      if (s->peer_need == 1 && s->rom_tx_off < s->own.rom_size)
         stream_rom(s, &budget);
   }
   if (!s->ready_sent && setup_complete(s))
   {
      uint8_t m[1] = { M_READY };
      sendm(s, m, 1);
      s->ready_sent = 1;
   }
   if (s->ready_sent && s->peer_ready && s->state == FE_GBLINK_SETUP)
      start_pair(s);
}

/* -------------------------------------------------------- running -- */

static void send_input(fe_gblink *s, uint32_t frame, uint16_t buttons)
{
   uint8_t m[7];
   m[0] = M_INPUT;
   put32(m + 1, frame);
   put16(m + 5, buttons);
   sendm(s, m, sizeof(m));
   /* The redundant copy: this frame and the ones before it, unreliable. */
   if (s->bk_chunk && s->p.bulk_send && s->p.input_copies)
   {
      uint8_t d[4 + 1 + 4 + 1 + 2 * INPUT_COPIES_MAX];
      unsigned n = s->p.input_copies > INPUT_COPIES_MAX ? INPUT_COPIES_MAX
                                                        : s->p.input_copies;
      uint32_t first, i;
      if (n > frame + 1)
         n = frame + 1;
      first = frame + 1 - n;
      memcpy(d, FE_GBLINK_BULK_MAGIC, 4);
      d[4] = BK_INPUT;
      put32(d + 5, first);
      d[9] = (uint8_t)n;
      for (i = 0; i < n; i++)
         put16(d + 10 + 2 * i, s->q[s->local_slot][(first + i) % RING]);
      s->p.bulk_send(s->p.user, d, 10 + 2 * n);
   }
}

/* The partner's buttons for frame f, from whichever copy arrives first.
 * Frames already held are duplicates; a frame past the next one needed is
 * a gap, which only the unreliable copy may leave (the ordered channel's
 * next message fills it). */
static int take_input(fe_gblink *s, uint32_t f, uint16_t buttons, int fast)
{
   int r = s->local_slot ^ 1;
   if (f < s->have[r])
      return 0;
   if (f > s->have[r])
      return -1;
   s->q[r][f % RING] = buttons;
   s->have[r]++;
   if (fast)
      s->st.inputs_fast++;
   else
      s->st.inputs_ordered++;
   return 1;
}

static int input_cb(void *user, int slot, uint64_t frame, uint16_t *buttons)
{
   fe_gblink *s = (fe_gblink *)user;
   if (s->captured)
      return 0;                      /* the session is over */
   if (slot == s->local_slot)
   {
      while (s->have[slot] <= frame + s->delay)
      {
         uint64_t f = s->have[slot];
         s->q[slot][f % RING] = s->pending_buttons;
         send_input(s, (uint32_t)f, s->pending_buttons);
         s->have[slot]++;
      }
   }
   if (frame < s->delay)
   {
      *buttons = 0;
      return 1;
   }
   if (frame >= s->have[slot])
      return 0;                      /* partner's buttons not here yet */
   *buttons = s->q[slot][frame % RING];
   return 1;
}

/* On the heap, for the session only: a static 128 KiB buffer here cost
 * every game, GBA included, that much heap (Emerald post-load 1.66 -> 1.50
 * MB in PPSSPP) for something only a link session uses. */
static uint32_t cart_crc(fe_gblink *s, int slot)
{
   const gbcore_api_t *api = gbdual_api(slot);
   gbcore_t *c = gbdual_core(s->dual, slot);
   size_t n = api->cart_ram_size(c);
   size_t need = api->save_ram_size(c) + 64;
   if (!n)
      return 0;
   if (s->crc_cap < need)
   {
      uint8_t *b = (uint8_t *)realloc(s->crc_buf, need);
      if (!b)
         return 0;
      s->crc_buf = b;
      s->crc_cap = need;
   }
   if (api->save_ram_read(c, s->crc_buf, s->crc_cap) != 0)
      return 0;
   return fe_crc32(0, s->crc_buf, n);
}

static void compare_hash(fe_gblink *s, uint32_t frame)
{
   unsigned i = (frame / (s->hash_interval ? s->hash_interval : 1)) % HASH_RING;
   if (s->own_h[i].frame != frame || s->peer_h[i].frame != frame)
      return;
   if (s->own_h[i].hash == s->peer_h[i].hash)
   {
      s->st.hashes_matched++;
      return;
   }
   fe_evt("gblink_desync frame=%u own=%016llx peer=%016llx", frame,
          (unsigned long long)s->own_h[i].hash,
          (unsigned long long)s->peer_h[i].hash);
   fail(s, FE_GBLINK_E_DESYNC, 1);
}

static void capture_end(fe_gblink *s, uint32_t frame)
{
   const gbcore_api_t *api = gbdual_api(s->local_slot);
   gbcore_t *c = gbdual_core(s->dual, s->local_slot);
   uint8_t m[13];
   size_t n = api->save_ram_size(c);
   s->final_hash = gbdual_sync_hash(s->dual);
   s->final_frame = frame;
   if (n)
   {
      s->final_save = (uint8_t *)malloc(n);
      if (!s->final_save || api->save_ram_read(c, s->final_save, n) != 0)
      {
         fail(s, FE_GBLINK_E_MEMORY, 1);
         return;
      }
   }
   s->final_save_size = n;
   n = api->state_size(c);
   if (n)
   {
      s->final_state = (uint8_t *)malloc(n);
      if (!s->final_state || api->state_save(c, s->final_state, n) != (long)n)
      {
         fail(s, FE_GBLINK_E_MEMORY, 1);
         return;
      }
      s->final_state_size = n;
   }
   s->captured = 1;
   m[0] = M_FINAL;
   put32(m + 1, frame);
   put64(m + 5, s->final_hash);
   sendm(s, m, sizeof(m));
   fe_evt("gblink_final frame=%u hash=%016llx save=%u t_ms=%llu", frame,
          (unsigned long long)s->final_hash, (unsigned)n, evt_ms(s));
}

static void frame_hook(void *user, int slot, uint64_t frame)
{
   fe_gblink *s = (fe_gblink *)user;
   uint32_t f = (uint32_t)frame;
   s->st.frames[slot] = frame;
   if (slot != 0 || s->state == FE_GBLINK_FAILED)
      return;
   if (f % SRAM_EVERY == 0)
   {
      int i;
      for (i = 0; i < GBDUAL_SLOTS; i++)
      {
         uint32_t c = cart_crc(s, i);
         if (c != s->sram_crc[i])
         {
            s->sram_crc[i] = c;
            s->sram_changed_at = f;
         }
      }
   }
   if (s->hash_interval && f % s->hash_interval == 0)
   {
      unsigned i = (f / s->hash_interval) % HASH_RING;
      uint8_t m[13];
      s->own_h[i].frame = f;
      s->own_h[i].hash = gbdual_sync_hash(s->dual);
      s->st.last_hash = s->own_h[i].hash;
      s->st.last_hash_frame = f;
      m[0] = M_HASH;
      put32(m + 1, f);
      put64(m + 5, s->own_h[i].hash);
      sendm(s, m, sizeof(m));
      s->st.hashes_sent++;
      compare_hash(s, f);
   }
   if (s->end_frame && f == s->end_frame && !s->captured)
   {
      /* Never cut a save in half: while either cartridge's RAM has changed
       * recently, move the end on (up to END_DEFER_MAX times, ~16 s: a game
       * that uses cartridge RAM as scratch never goes quiet).  Both consoles
       * see the same RAM at the same frames, so both move it identically. */
      if (f - s->sram_changed_at < SRAM_QUIET &&
          s->end_deferrals < END_DEFER_MAX)
      {
         s->end_deferrals++;
         s->end_frame += SRAM_QUIET;
         s->st.end_frame = s->end_frame;
         fe_evt("gblink_end_deferred frame=%u sram_changed_at=%u to=%u", f,
                s->sram_changed_at, s->end_frame);
         return;
      }
      if (f - s->sram_changed_at < SRAM_QUIET)
         fe_evt("gblink_end_forced frame=%u sram_changed_at=%u deferrals=%u "
                "t_ms=%llu", f, s->sram_changed_at, s->end_deferrals,
                evt_ms(s));
      capture_end(s, f);
   }
}

static void check_final(fe_gblink *s)
{
   if (!s->captured || !s->peer_final || s->state != FE_GBLINK_ENDING)
      return;
   if (s->peer_final_frame != s->final_frame ||
       s->peer_final_hash != s->final_hash)
   {
      fe_evt("gblink_final_mismatch own=%u/%016llx peer=%u/%016llx",
             s->final_frame, (unsigned long long)s->final_hash,
             s->peer_final_frame, (unsigned long long)s->peer_final_hash);
      fail(s, FE_GBLINK_E_DESYNC, 1);
      return;
   }
   if (s->final_save_size && s->p.commit_save &&
       s->p.commit_save(s->p.user, s->final_save, s->final_save_size) != 0)
   {
      fail(s, FE_GBLINK_E_COMMIT, 0);
      return;
   }
   s->state = FE_GBLINK_DONE;
   fe_evt("gblink_done frame=%u hash=%016llx save_committed=%u hashes=%u/%u "
          "stalls=%u t_ms=%llu", s->final_frame,
          (unsigned long long)s->final_hash,
          (unsigned)s->final_save_size, s->st.hashes_matched,
          s->st.hashes_sent, s->st.stalls, evt_ms(s));
}

static void set_end(fe_gblink *s, uint32_t f)
{
   s->end_frame = f;
   s->st.end_frame = f;
   if (s->state == FE_GBLINK_RUNNING)
      s->state = FE_GBLINK_ENDING;
   fe_evt("gblink_end_at frame=%u now=%llu t_ms=%llu", f,
          (unsigned long long)s->st.frames[0], evt_ms(s));
}

static void host_decide_end(fe_gblink *s)
{
   uint8_t m[5];
   uint32_t f = (uint32_t)s->st.frames[0] + END_MARGIN + 2 * s->delay;
   set_end(s, f);
   m[0] = M_END_AT;
   put32(m + 1, f);
   sendm(s, m, sizeof(m));
}

void fe_gblink_request_end(fe_gblink *s)
{
   uint8_t m[5];
   if (!s)
      return;
   if (s->state == FE_GBLINK_SETUP)
   {
      fail(s, FE_GBLINK_E_LOCAL, 1);
      return;
   }
   if (s->state != FE_GBLINK_RUNNING || s->end_requested || s->end_frame)
      return;
   s->end_requested = 1;
   fe_evt("gblink_end_request frame=%llu t_ms=%llu",
          (unsigned long long)s->st.frames[0], evt_ms(s));
   if (s->l.is_host)
      host_decide_end(s);
   else
   {
      m[0] = M_END_REQ;
      put32(m + 1, (uint32_t)s->st.frames[0]);
      sendm(s, m, sizeof(m));
   }
}

int fe_gblink_step(fe_gblink *s, uint16_t local_buttons)
{
   int r;
   if (!s)
      return FE_GBLINK_IDLE;
   if (s->state == FE_GBLINK_SETUP)
   {
      if (s->own_hashed && !s->have_hello &&
          ++s->no_hello_steps > NO_PEER_STEPS)
      {
         fail(s, FE_GBLINK_E_NO_PEER, 1);
         return FE_GBLINK_IDLE;
      }
      setup_step(s);
      return FE_GBLINK_IDLE;
   }
   if ((s->state != FE_GBLINK_RUNNING && s->state != FE_GBLINK_ENDING) ||
       s->captured)
   {
      check_final(s);
      return FE_GBLINK_IDLE;
   }
   s->pending_buttons = local_buttons;
   r = gbdual_advance(s->dual, input_cb, s);
   if (s->state == FE_GBLINK_FAILED)
      return FE_GBLINK_IDLE;
   if (s->captured)
   {
      check_final(s);
      return r == 1 ? FE_GBLINK_FRAME : FE_GBLINK_IDLE;
   }
   if (r == 1)
   {
      s->streak = 0;
      return FE_GBLINK_FRAME;
   }
   if (r < 0)
   {
      fail(s, FE_GBLINK_E_START, 1);
      return FE_GBLINK_IDLE;
   }
   s->st.stalls++;
   if (s->streak == 0)
      s->st.stall_episodes++;
   if (++s->streak > s->st.stall_streak_max)
      s->st.stall_streak_max = s->streak;
   return FE_GBLINK_STALL;
}

/* ------------------------------------------------------- receive -- */

void fe_gblink_peer(fe_gblink *s, int connected)
{
   if (!s)
      return;
   s->peer_up = connected ? 1 : 0;
   fe_evt("gblink_peer connected=%d state=%d", s->peer_up, s->state);
   if (!connected && (s->state == FE_GBLINK_SETUP ||
                      s->state == FE_GBLINK_RUNNING ||
                      s->state == FE_GBLINK_ENDING))
      fail(s, FE_GBLINK_E_PEER_LOST, 0);
}

void fe_gblink_receive(fe_gblink *s, const void *buf, size_t len)
{
   const uint8_t *b = (const uint8_t *)buf;
   if (!s || !len || s->state == FE_GBLINK_FAILED || s->state == FE_GBLINK_DONE)
      return;
   switch (b[0])
   {
   case M_HELLO:
   {
      const uint8_t *q = b + 1;
      if (len != HELLO_LEN || q[0] != FE_GBLINK_PROTO ||
          (q[1] != 0) == (s->l.is_host != 0) || s->have_hello)
      {
         fe_evt("gblink_hello_refused len=%u proto=%u role=%u",
                (unsigned)len, len > 1 ? b[1] : 0, len > 2 ? b[2] : 0);
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      q += 2;
      s->peer.console = *q++;
      s->peer.palette = *q++;
      s->peer.rom_size = get32(q); q += 4;
      memcpy(s->peer.sha1, q, 20); q += 20;
      memcpy(s->peer.header, q, 0x1C); q += 0x1C;
      s->peer.save_size = get32(q); q += 4;
      {
         uint32_t pm = get16(q);
         s->peer.state_size = get32(q + 2);
         uint32_t om = s->p.bulk_send &&
                       s->p.bulk_max > FE_GBLINK_BULK_HDR + BK_MIN_CHUNK &&
                       s->p.bulk_max <= FE_GBLINK_BULK_HDR + 1500
                          ? (uint32_t)s->p.bulk_max : 0u;
         uint32_t m = pm < om ? pm : om;
         s->bk_chunk = m > FE_GBLINK_BULK_HDR + BK_MIN_CHUNK
                          ? m - FE_GBLINK_BULK_HDR : 0;
         s->st.bulk_chunk = s->bk_chunk;
      }
      if (s->peer.rom_size < 0x150 || s->peer.rom_size > 8u * 1024 * 1024 ||
          s->peer.save_size > 0x20000 + RTC_TRAILER ||
          s->peer.state_size > STATE_MAX ||
          (s->peer.console != FE_CONSOLE_GB &&
           s->peer.console != FE_CONSOLE_GBC))
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      if (s->peer.save_size)
      {
         s->peer_save = (uint8_t *)malloc(s->peer.save_size);
         if (!s->peer_save)
         {
            fail(s, FE_GBLINK_E_MEMORY, 1);
            return;
         }
      }
      if (s->peer.state_size)
      {
         s->peer_state = (uint8_t *)malloc(s->peer.state_size);
         if (!s->peer_state)
         {
            fail(s, FE_GBLINK_E_MEMORY, 1);
            return;
         }
      }
      s->have_hello = 1;
      fe_evt("gblink_live own_state=%u peer_state=%u (0 = power on)",
             (unsigned)s->own.state_size, (unsigned)s->peer.state_size);
      fe_evt("gblink_bulk chunk=%u (0 = ordered channel)",
             (unsigned)s->bk_chunk);
      {
         char t[17];
         FE_EVT_ONLY(t);
         title_of(s->peer.header, t);
         fe_evt("gblink_hello peer_title=\"%s\" rom=%u save=%u console=%u",
                t, (unsigned)s->peer.rom_size, (unsigned)s->peer.save_size,
                s->peer.console);
      }
      if (s->l.is_host)
      {
         s->delay = s->l.input_delay;
         s->hash_interval = s->l.hash_interval ? s->l.hash_interval : 60;
         s->batch = s->l.batch_lines > 1 ? s->l.batch_lines : 1;
         s->seed = s->l.wallclock_now;
         send_config(s);
         config_known(s);
      }
      partner_lookup(s);
      break;
   }
   case M_CONFIG:
      if (len != CONFIG_LEN || s->l.is_host || s->have_config)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->delay = b[1];
      s->seed = (int64_t)get64(b + 2);
      s->hash_interval = get16(b + 10);
      s->batch = get16(b + 12);
      config_known(s);
      break;
   case M_NEED:
      if (len != 2)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->peer_need = b[1] ? 1 : 0;
      break;
   case M_ROM:
   {
      uint32_t off = len > CHUNK_HDR ? get32(b + 1) : 0;
      size_t n = len - CHUNK_HDR;
      if (len <= CHUNK_HDR || !s->partner_buf || off != s->partner_rx ||
          off + n > s->peer.rom_size)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      if (!s->partner_rx && s->now_us)
         s->st.rom_rx_first_us = s->now_us();
      memcpy(s->partner_buf + off, b + CHUNK_HDR, n);
      fe_sha1_update(&s->rx_ctx, b + CHUNK_HDR, n);
      s->partner_rx += (uint32_t)n;
      s->st.rom_rx_bytes = s->partner_rx;
      if (s->partner_rx == s->peer.rom_size)
         rom_complete(s);
      break;
   }
   case M_ROM_RESULT:
      if (len != 2)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->peer_rom_ok = b[1] ? 1 : 0;
      if (!b[1])
         fail(s, FE_GBLINK_E_ROM_BAD, 0);
      break;
   case M_SAVE:
   {
      uint32_t off = len > CHUNK_HDR ? get32(b + 1) : 0;
      size_t n = len - CHUNK_HDR;
      if (len <= CHUNK_HDR || off != s->peer_save_rx ||
          off + n > s->peer.save_size)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      memcpy(s->peer_save + off, b + CHUNK_HDR, n);
      s->peer_save_rx += (uint32_t)n;
      s->st.save_rx_bytes = s->peer_save_rx;
      break;
   }
   case M_STATE:
   {
      uint32_t off = len > CHUNK_HDR ? get32(b + 1) : 0;
      size_t n = len - CHUNK_HDR;
      if (len <= CHUNK_HDR || !s->peer_state || off != s->peer_state_rx ||
          off + n > s->peer.state_size)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      memcpy(s->peer_state + off, b + CHUNK_HDR, n);
      s->peer_state_rx += (uint32_t)n;
      break;
   }
   case M_READY:
      s->peer_ready = 1;
      break;
   case M_BULK_ACK:
      bk_on_ack(s, b, len);
      break;
   case M_INPUT:
   {
      int r = s->local_slot ^ 1;
      uint32_t f;
      FE_EVT_ONLY(r);
      if (len != 7 || !s->have_config)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      f = get32(b + 1);
      if (take_input(s, f, (uint16_t)get16(b + 5), 0) < 0)
      {
         fe_evt("gblink_input_gap got=%u want=%llu", f,
                (unsigned long long)s->have[r]);
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      break;
   }
   case M_HASH:
   {
      uint32_t f;
      unsigned i;
      if (len != 13 || !s->hash_interval)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      f = get32(b + 1);
      i = (f / s->hash_interval) % HASH_RING;
      s->peer_h[i].frame = f;
      s->peer_h[i].hash = get64(b + 5);
      compare_hash(s, f);
      break;
   }
   case M_END_REQ:
      if (s->l.is_host && !s->end_frame &&
          (s->state == FE_GBLINK_RUNNING))
         host_decide_end(s);
      break;
   case M_END_AT:
      if (len != 5 || s->l.is_host)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      if ((uint64_t)get32(b + 1) <= s->st.frames[0])
      {
         /* Already past it (a link stalled for longer than the margin): the
          * two consoles cannot end at the same frame.  Safe failure. */
         fe_evt("gblink_end_late at=%u now=%llu", get32(b + 1),
                (unsigned long long)s->st.frames[0]);
         fail(s, FE_GBLINK_E_DESYNC, 1);
         return;
      }
      set_end(s, get32(b + 1));
      break;
   case M_FINAL:
      if (len != 13)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->peer_final = 1;
      s->peer_final_frame = get32(b + 1);
      s->peer_final_hash = get64(b + 5);
      check_final(s);
      break;
   case M_ABORT:
      fe_evt("gblink_peer_abort reason=%s",
             fe_gblink_error_text(len > 1 ? b[1] : 0));
      fail(s, FE_GBLINK_E_PEER_ABORT, 0);
      break;
   default:
      fail(s, FE_GBLINK_E_PROTO, 1);
      break;
   }
}

/* ---------------------------------------------------- introspection -- */

int fe_gblink_state(const fe_gblink *s) { return s ? s->state : FE_GBLINK_FAILED; }
int fe_gblink_error(const fe_gblink *s) { return s ? s->error : FE_GBLINK_E_LOCAL; }
gbdual_t *fe_gblink_dual(const fe_gblink *s) { return s ? s->dual : NULL; }
int fe_gblink_local_slot(const fe_gblink *s) { return s ? s->local_slot : -1; }

void fe_gblink_get_stats(const fe_gblink *s, fe_gblink_stats *out)
{
   if (!s || !out)
      return;
   *out = s->st;
   out->local_slot = s->local_slot;
   out->input_delay = s->delay;
}

static void mb_line(char *out, size_t cap, const char *verb, const char *what,
                    uint32_t done, uint32_t total, int *pm)
{
   snprintf(out, cap, "%s %s  %u.%u / %u.%u MB", verb, what,
            (unsigned)(done / 1048576u),
            (unsigned)(done % 1048576u * 10u / 1048576u),
            (unsigned)(total / 1048576u),
            (unsigned)(total % 1048576u * 10u / 1048576u));
   if (pm)
      *pm = total ? (int)((uint64_t)done * 1000u / total) : 0;
}

/* The connecting screen: up to two lines, each with its own progress (per
 * mille, -1 = no bar).  Both directions of a cartridge transfer run at once,
 * so both are shown at once (hw2: the host said "Receiving" for 10 s while
 * it was also sending, then "Sending"). */
static void status_impl(const fe_gblink *s, char *l1, size_t c1, char *l2,
                        size_t c2, int *p1, int *p2)
{
   char t[17] = "", own[17] = "";
   int rx, tx;
   if (l1 && c1) l1[0] = '\0';
   if (l2 && c2) l2[0] = '\0';
   if (p1) *p1 = -1;
   if (p2) *p2 = -1;
   if (!s)
      return;
   if (s->have_hello)
      title_of(s->peer.header, t);
   title_of(s->own.header, own);
   switch (s->state)
   {
   case FE_GBLINK_SETUP:
      rx = s->partner_buf && s->partner_rx < s->peer.rom_size;
      tx = s->peer_need == 1 && s->rom_tx_off < s->own.rom_size;
      if (!s->own_hashed)
         mb_line(l1, c1, "Reading", own[0] ? own : "your game", s->own_read,
                 s->own.rom_size, p1);
      else if (!s->peer_up)
         snprintf(l1, c1, "Waiting for the other player...");
      else if (!s->have_hello)
         snprintf(l1, c1, "Connecting...");
      else if (rx || tx)
      {
         if (rx)
            mb_line(l1, c1, "Receiving", t[0] ? t : "the game", s->partner_rx,
                    s->peer.rom_size, p1);
         if (tx)
            mb_line(rx ? l2 : l1, rx ? c2 : c1, "Sending",
                    own[0] ? own : "your game", s->rom_tx_off,
                    s->own.rom_size, rx ? p2 : p1);
      }
      else if (s->verifying)
      {
         snprintf(l1, c1, "Checking %s", t[0] ? t : "the other game");
         if (p1 && s->peer.rom_size)
            *p1 = (int)((uint64_t)(s->peer.rom_size - s->verify_left) *
                        1000u / s->peer.rom_size);
      }
      else
         snprintf(l1, c1, "Exchanging saves...");
      if (l2 && !l2[0] && t[0])
         snprintf(l2, c2, "with %s", t);
      break;
   case FE_GBLINK_RUNNING:
      snprintf(l1, c1, "Linked: %s", t);
      snprintf(l2, c2, "delay %u", s->delay);
      break;
   case FE_GBLINK_ENDING:
      snprintf(l1, c1, "Ending the link...");
      break;
   case FE_GBLINK_DONE:
      snprintf(l1, c1, "Link ended. Game saved.");
      break;
   case FE_GBLINK_FAILED:
      if (s->error == FE_GBLINK_E_NO_PEER)
      {
         snprintf(l1, c1, "No other player found.");
         snprintf(l2, c2, "Check both PSPs' WLAN switch, then choose "
                  "Wireless again.");
         break;
      }
      snprintf(l1, c1, "Link ended: %s", fe_gblink_error_text(s->error));
      snprintf(l2, c2, "%s",
               s->error == FE_GBLINK_E_COMMIT ? "The save could not be written."
                                              : "Nothing was saved.");
      break;
   }
}

void fe_gblink_status(const fe_gblink *s, char *l1, size_t c1, char *l2,
                      size_t c2)
{
   status_impl(s, l1, c1, l2, c2, NULL, NULL);
}

void fe_gblink_status_progress(const fe_gblink *s, int *p1, int *p2)
{
   status_impl(s, NULL, 0, NULL, 0, p1, p2);
}
