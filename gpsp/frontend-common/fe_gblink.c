/* fe_gblink.c -- GB/GBC link session: two machines per console, buttons in
 * lockstep over the network.  Protocol and invariants: fe_gblink.h. */
#include "fe_gblink.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_linkprof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ wire -- */

enum
{
   M_HELLO = 0x01, M_CONFIG = 0x02, M_NEED = 0x03, M_ROM = 0x04,
   M_ROM_RESULT = 0x05, M_SAVE = 0x06, M_READY = 0x07, M_BULK_ACK = 0x08,
   M_STATE = 0x09,
   M_CORECFG = 0x0A,          /* host -> guest: the mask in force (the answer
                                 to M_CAPS; never sent unasked) */
   M_CAPS = 0x16,             /* guest -> host: the negotiable bits it can do
                                 (only to a host that advertised) */
   M_INPUTS = 0x15,           /* buttons for n consecutive frames (mask bit 1) */
   M_INPUT = 0x10, M_HASH = 0x11, M_END_REQ = 0x12, M_END_AT = 0x13,
   M_FINAL = 0x14, M_ABORT = 0x1F
};

#define HELLO_LEN   (1 + 1 + 1 + 1 + 1 + 4 + 20 + 0x1C + 4 + 2 + 4)
_Static_assert(HELLO_LEN == FE_GBLINK_HELLO_LEN, "fe_gblink.h HELLO size");
#define STATE_MAX   (1024u * 1024u)
#define CONFIG_LEN  (1 + 1 + 8 + 2 + 2)
/* What a guest accepts in the host's CONFIG.  Every host since the first
 * link build sends values well inside these (delay 1-30 from the player's
 * setting, at most 60 from a rig; hash interval 60 or 240, never 0; batch
 * 1-154 scanlines for a GB pair, 114 for a GBA pair), so a 3.5.0 host is
 * unaffected; a damaged or hostile CONFIG is refused instead of run with.
 * The delay bounds how far the input ring (RING) is filled ahead; a zero
 * hash interval would switch the periodic comparison off for both consoles;
 * the batch is how far one machine may run ahead of the other. */
#define CONFIG_DELAY_MAX  60u
#define CONFIG_BATCH_MAX  1232u       /* scanlines: several frames */
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
/* fe_gblink_local.rom_sampled (a vtable pair only): the cartridge identity
 * is the SHA-1 of its size and SAMPLE_N slices of SAMPLE_LEN bytes spread
 * evenly over the file (the first at 0, so the header is in it, the last at
 * the end), instead of the whole file -- the owner's call (2026-10-03): a
 * 32 MiB cart streamed whole is ~8 s of "Checking the game" on the stick;
 * this is 256 KiB in 64 seeks.  Two dumps that differ only between slices
 * pass the check and are caught by the periodic HASH instead, as a desync.
 * Both consoles must use the same rule: it travels in HELLO's SHA-1 field,
 * and the protocol string of a cable room ("GBAdhoc cable v1") pins it. */
#define SAMPLE_N    64u
#define SAMPLE_LEN  4096u
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
/* A vtable pair (GBA) also ends, instead of waiting forever, when the
 * partner's inputs or its FINAL stop coming for this many steps (20 s):
 * a frozen partner console whose radio stays associated never makes netdrv
 * report the peer lost. */
#define WAIT_MAX_STEPS (20u * 60u)
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
/* bulk-lane kinds: a resend request (NACK) and its answer (docs/GB-LINK.md
 * 14.4).  A console stalled on the partner's buttons for frame f sends
 *   'GBLK' | BK_NACK | u32 f
 * and the partner answers with what it has from f on:
 *   'GBLK' | BK_RESEND | u32 first | u8 n | u32 end | n x u16 buttons
 * `end` is the end frame the host agreed (END_AT), 0 if none: a resend can
 * carry inputs the host made after its END_AT, so it carries the END too --
 * otherwise a guest could pass the end frame on resent inputs while END_AT
 * waits behind a lost packet on the ordered channel.  Without it a lost
 * INPUT waits for netdrv's retransmission timeout (>= 50 ms; hw5's radio
 * lost 1-3 %): a 3-4 frame stall each time. */
#define BK_NACK     0x12
#define BK_RESEND   0x13
#define NACK_RETRY_US 40000u           /* re-ask if still waiting after this */
#define RESEND_MAX  16u
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
   unsigned own_sample;              /* rom_sampled: samples hashed so far */
   int peer_reason;                  /* the peer's ABORT reason, -1 none */
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
   int config_defer;                 /* host: auto delay waits for RTT */
   unsigned config_defer_steps;
   unsigned delay, hash_interval, batch;
   int64_t seed;
   uint32_t save_tx_off;
   int save_ready;                   /* own save re-based, can stream */
   int ready_sent, peer_ready;
   uint32_t core_rx;             /* the host's core mask (M_CORECFG); 0 = none */
   uint32_t own_mask;            /* layout.core_mask as this console asked    */
   uint8_t  host_caps;           /* guest: the host's HELLO role byte         */
   uint8_t  caps_have;           /* host: M_CAPS arrived (a build that can)   */
   uint8_t  corecfg_wait;        /* guest: M_CAPS sent, M_CORECFG owed        */
   uint8_t  corecfg_have;        /* guest: M_CORECFG arrived                  */
   uint8_t  fl_logged;
   uint32_t ib_first;            /* input batching: first held frame, count, */
   unsigned ib_n;                /* and the held buttons                     */
   unsigned ib_age;              /* steps the first held frame has waited    */
   uint16_t ib_keys[2];

   /* running */
   gbdual_t *dual;
   const fe_gblink_machines *m;      /* NULL = the GB pair (gbdual) */
   int m_running;                    /* m->start succeeded, m->stop not yet */
   int final_hashed;                 /* m: slot 0 reached the end frame */
   uint32_t final_wait;              /* m: steps since our FINAL went out */
   int lookup_pending;               /* m: HELLO came before our own SHA-1 */
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
   uint32_t end_agreed;              /* the END_AT frame (before deferrals) */
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

   /* telemetry window (fe_gblink_take_window) and what a stall waits on */
   fe_gblink_window win;
   int need_slot;
   uint64_t need_frame;
   uint32_t hook_us;                 /* hash/crc time inside this advance */
   uint64_t nack_frame;              /* the partner frame last NACKed */
   uint64_t nack_us;                 /* ...and when */
   uint32_t nack_steps;              /* steps since (no clock) */
};

static uint64_t clock_us(const fe_gblink *s)
{
   return s->now_us ? s->now_us() : 0;
}

static void win_time(uint32_t *sum, uint32_t *max, uint64_t t0, uint64_t t1)
{
   uint32_t d = t1 > t0 ? (uint32_t)(t1 - t0) : 0;
   *sum += d;
   if (d > *max)
      *max = d;
}

/* Cartridge-RAM change detector (frame_hook, every SRAM_EVERY frames): only
 * ever compared with its own previous value on this console, so any
 * deterministic function works.  It was CRC-32 by byte over a copy of the
 * battery image (2 x 32 KiB for Crystal): ~1.5 ms on a PSP every 30
 * frames, landing on the sync hash's frames too (GB-LINK.md section 14).
 * Words, two multiplies: ~8x fewer instructions. */
static uint32_t ram_hash(const uint8_t *p, size_t n)
{
   uint32_t a = 0x811c9dc5u, b = 0x9e3779b9u;
   size_t i = 0;
   for (; i + 4 <= n; i += 4)
   {
      uint32_t w;
      memcpy(&w, p + i, 4);
      a = (a ^ w) * 0x01000193u;
      b = (b + w) * 0x85ebca6bu;
   }
   for (; i < n; i++)
      a = (a ^ p[i]) * 0x01000193u;
   return a ^ (b >> 15) ^ (b << 17);
}

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
static void set_end(fe_gblink *s, uint32_t f);

/* A BK_NACK from the partner: send back this player's buttons from frame f
 * on (the ones made so far, at most RESEND_MAX), with the agreed end frame
 * when this console is the host (see BK_NACK).  Nothing to send when f is
 * not made yet -- the partner is simply ahead -- or long gone from the ring. */
static void resend_inputs(fe_gblink *s, uint32_t f)
{
   uint8_t d[4 + 1 + 4 + 1 + 4 + 2 * RESEND_MAX];
   int me = s->local_slot;
   uint64_t have = s->have[me];
   uint32_t n, i;
   if (!s->bk_chunk || !s->p.bulk_send || f < s->delay || f >= have ||
       have - f > RING / 2)
      return;
   n = have - f > RESEND_MAX ? RESEND_MAX : (uint32_t)(have - f);
   memcpy(d, FE_GBLINK_BULK_MAGIC, 4);
   d[4] = BK_RESEND;
   put32(d + 5, f);
   d[9] = (uint8_t)n;
   put32(d + 10, s->l.is_host ? s->end_agreed : 0u);
   for (i = 0; i < n; i++)
      put16(d + 14 + 2 * i, s->q[me][(f + i) % RING]);
   s->p.bulk_send(s->p.user, d, 14 + 2 * n);
   s->st.resends++;
   s->win.resends++;
}

/* A BK_RESEND: the partner's buttons from `first` on -- and, from the host,
 * the agreed end frame, applied BEFORE the buttons that could carry this
 * console past it (the guarantee END_AT's place in the ordered channel gives
 * on that channel). */
static void take_resend(fe_gblink *s, const uint8_t *b, size_t len)
{
   uint32_t first = get32(b + 5), c = b[9], end = get32(b + 10), j;
   if (!c || c > RESEND_MAX || len != 14 + 2 * c)
   {
      s->st.bulk_bad++;
      return;
   }
   if (end && !s->l.is_host)
   {
      if (!s->end_agreed)
      {
         if ((uint64_t)end <= s->st.frames[0])
         {
            fe_evt("gblink_end_late at=%u now=%llu via=resend", end,
                   (unsigned long long)s->st.frames[0]);
            fail(s, FE_GBLINK_E_DESYNC, 1);
            return;
         }
         fe_evt("gblink_end_via_resend frame=%u", end);
         set_end(s, end);
      }
      else if (end != s->end_agreed)
      {
         fe_evt("gblink_end_conflict at=%u agreed=%u via=resend", end,
                s->end_agreed);
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
   }
   for (j = 0; j < c; j++)
      if (take_input(s, first + j, (uint16_t)get16(b + 14 + 2 * j), 2) < 0)
         break;                       /* a gap the ordered channel fills */
}

/* This console just lost a display frame waiting for the partner's buttons
 * for frame need_frame: ask for them on the bulk lane (once per
 * NACK_RETRY_US for the same frame).  The partner answers only if it has
 * made them -- a partner that is merely behind costs one small datagram. */
static void nack_maybe(fe_gblink *s)
{
   uint8_t d[9];
   uint64_t now;
   if (!s->p.nack || !s->bk_chunk || !s->p.bulk_send ||
       s->need_slot == s->local_slot)
      return;
   now = clock_us(s);
   if (s->nack_frame == s->need_frame + 1 &&
       (now ? now - s->nack_us < NACK_RETRY_US : ++s->nack_steps < 3))
      return;
   memcpy(d, FE_GBLINK_BULK_MAGIC, 4);
   d[4] = BK_NACK;
   put32(d + 5, (uint32_t)s->need_frame);
   s->p.bulk_send(s->p.user, d, sizeof(d));
   s->nack_frame = s->need_frame + 1;
   s->nack_us = now;
   s->nack_steps = 0;
   s->st.nacks++;
   s->win.nacks++;
}

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
   if (len >= 9 && !memcmp(b, FE_GBLINK_BULK_MAGIC, 4) &&
       (b[4] == BK_NACK || b[4] == BK_RESEND))
   {
      /* the machines exist: the GB pair (dual) or a vtable pair (m) --
       * a vtable session has no `dual`, and testing only that dropped
       * every NACK of a GBA session unanswered (PPSSPP, 3 % loss: 1,474
       * NACKs, 0 resends) */
      if (!s->have_config || (!s->dual && !s->m) ||
          (s->state != FE_GBLINK_RUNNING && s->state != FE_GBLINK_ENDING))
         return;                      /* not running (yet / any more) */
      if (b[4] == BK_NACK)
      {
         if (len != 9)
            s->st.bulk_bad++;
         else
            resend_inputs(s, get32(b + 5));
      }
      else
         take_resend(s, b, len);
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
   s->own_mask = l->core_mask;
   snprintf(s->rom_path, sizeof(s->rom_path), "%s", l->rom_path);
   s->l.rom_path = s->rom_path;
   s->local_slot = l->is_host ? 0 : 1;
   s->peer_reason = -1;
   s->peer_need = -1;
   s->peer_rom_ok = -1;
   s->bk_rate = BK_RATE_START;
   s->bk_slow_start = 1;
   s->st.local_slot = s->local_slot;
   s->m = l->machines;
   s->hash_f = fopen(s->rom_path, "rb");
   if (!s->hash_f || fseek(s->hash_f, 0, SEEK_END) != 0 ||
       (n = ftell(s->hash_f)) < (long)(s->m ? s->m->rom_min : 0x150) ||
       n > (long)(s->m ? s->m->rom_max : 8u * 1024 * 1024) ||
       fseek(s->hash_f, (long)(s->m ? s->m->header_off : 0x134),
             SEEK_SET) != 0 ||
       fread(s->own.header, 1, 0x1C, s->hash_f) != 0x1C ||
       fseek(s->hash_f, 0, SEEK_SET) != 0)
   {
      if (s->hash_f)
         fclose(s->hash_f);
      free(s);
      return NULL;
   }
   s->own.rom_size = (uint32_t)n;
   /* A vtable pair's machine already has its cartridge: it is only hashed,
    * by streaming (setup_step), never held here. */
   s->own_buf = s->m ? NULL : gbcore_rom_alloc((size_t)n, NULL);
   if (!s->own_buf && !s->m)
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
   if (s->m && s->m_running)
      s->m->stop(s->m->user);
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
   /* role: non-zero = host (all an old guest tests); a host that can
    * negotiate says so in bit 1 (fe_gblink.h, core_mask) */
   *q++ = (uint8_t)(s->l.is_host ? (1u | FE_GBLINK_HOSTCAP_NEGOTIATE) : 0u);
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
   if (!s->m)
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

/* AUTO INPUT DELAY (host, fe_gblink_local.delay_auto; docs/ONLINE-TUNNEL.md
 * section 2).  A lockstep machine needs the partner's buttons for frame f
 * when it starts f; they left the partner D frames earlier, so a frame is
 * on time while
 *
 *     one-way latency + one pump period + one batching frame  <  D frames.
 *
 * One-way is half the smoothed RTT, plus two deviations of headroom for the
 * jitter; the pump and the batch each cost up to a frame.  Hence
 *
 *     D = ceil((srtt / 2 + 2 * rttvar + T) / T) + 1,   T = one GBA frame,
 *
 * clamped to [input_delay, delay_auto].  On the PSP-to-PSP radio (srtt
 * ~35 ms, rttvar ~4 ms) that is 4, today's default, so the radio is
 * unchanged.  CONFIG waits until the transport has two RTT samples (the
 * HELLOs' acknowledgements bring them) or DEFER_STEPS steps have passed. */
#define AUTO_FRAME_US     16743u
#define AUTO_DEFER_STEPS  90u
static void config_auto_step(fe_gblink *s)
{
   uint32_t var = 0, n = 0, rtt;
   unsigned d = s->l.input_delay;
   rtt = s->p.rtt_us(s->p.user, &var, &n);
   if (n < 2 && ++s->config_defer_steps < AUTO_DEFER_STEPS)
      return;
   if (n && rtt)
   {
      uint64_t need = (uint64_t)rtt / 2 + 2ull * var + AUTO_FRAME_US;
      unsigned a = (unsigned)((need + AUTO_FRAME_US - 1) / AUTO_FRAME_US) + 1;
      if (a > d)
         d = a;
   }
   if (d > s->l.delay_auto)
      d = s->l.delay_auto > s->l.input_delay ? s->l.delay_auto
                                             : s->l.input_delay;
   if (d > CONFIG_DELAY_MAX)
      d = CONFIG_DELAY_MAX;
   fe_evt("gblink_delay_auto srtt_us=%u rttvar_us=%u samples=%u steps=%u "
          "base=%u cap=%u delay=%u", (unsigned)rtt, (unsigned)var,
          (unsigned)n, s->config_defer_steps, s->l.input_delay,
          s->l.delay_auto, d);
   s->delay = d;
   s->config_defer = 0;
   send_config(s);
   config_known(s);
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
   if (s->m)
   {
      /* A vtable pair runs the same cartridge on both consoles (for now). */
      hex20(s->peer.sha1, h);
      fe_evt("gblink_partner_rom source=none sha1=%s (same cartridge needed)",
             h);
      fail(s, FE_GBLINK_E_ROM_MISSING, 1);
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

static void start_pair_m(fe_gblink *s)
{
   const uint8_t *st[2], *sv[2];
   size_t stn[2], svn[2];
   int slot;
   for (slot = 0; slot < 2; slot++)
   {
      int mine = slot == s->local_slot;
      st[slot] = mine ? s->own_state : s->peer_state;
      stn[slot] = mine ? s->own.state_size : s->peer.state_size;
      sv[slot] = mine ? s->own_save : s->peer_save;
      svn[slot] = mine ? s->own_save_size : s->peer.save_size;
   }
   if (s->m->start(s->m->user, s->local_slot, s->batch, s->seed, st, stn,
                   sv, svn,
                   frame_hook, s) != 0)
   {
      fail(s, FE_GBLINK_E_START, 1);
      return;
   }
   s->m_running = 1;
   s->state = FE_GBLINK_RUNNING;
   fe_evt("gblink_start slot=%d delay=%u seed=%lld partner=same batch=%u "
          "machines=vtable console=%u", s->local_slot, s->delay,
          (long long)s->seed, s->batch, (unsigned)s->m->console);
}

static void start_pair(fe_gblink *s)
{
   gbdual_config_t cfg;
   int slot;
   /* a guest that never sent M_CAPS is a build that cannot: plain session */
   if (s->l.is_host && !s->caps_have)
      s->l.core_mask &= ~FE_GBLINK_MASK_NEGOTIABLE;
   if (!s->fl_logged)
   {
      int src;
      int on = fe_gblink_fast_link(s, &src);
      FE_EVT_ONLY(on); FE_EVT_ONLY(src);
      s->fl_logged = 1;
      fe_evt("gblink_fast_link on=%d source=%s own=%d role=%s", on,
             src == FE_GBLINK_FL_HOST ? "host"
             : src == FE_GBLINK_FL_OLD_PEER ? "old_peer" : "own",
             (s->own_mask & FE_GBLINK_MASK_INPUT_BATCH) != 0,
             s->l.is_host ? "host" : "guest");
   }
   if (s->m)
   {
      start_pair_m(s);
      return;
   }
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
   cfg.batch_armed = (s->l.core_mask & FE_GBLINK_MASK_ARMED_BATCH) != 0;
   cfg.sync_hash_legacy = s->l.hash_legacy;
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
   fe_evt("gblink_stepping batch=%u armed=%d", s->batch, cfg.batch_armed);
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

/* The offset of slice k of a sampled identity (see SAMPLE_N). */
static uint32_t sample_off(const fe_gblink *s, unsigned k)
{
   uint32_t span = s->own.rom_size - SAMPLE_LEN;
   return (uint32_t)((uint64_t)span * k / (SAMPLE_N - 1u)) & ~3u;
}

/* 1 = this session identifies the own cartridge by slices. */
static int rom_sampled(const fe_gblink *s)
{
   return s->m && s->l.rom_sampled &&
          s->own.rom_size > SAMPLE_N * SAMPLE_LEN;
}

/* One step of the sampled identity: as many slices as the step's budget
 * allows.  Returns 0 while there is more to read, -1 on a read failure. */
static int sampled_step(fe_gblink *s)
{
   uint32_t budget = HASH_READ;
   uint64_t t0 = s->now_us ? s->now_us() : 0;
   uint8_t blk[SAMPLE_LEN];
   if (s->own_sample == 0 && s->own_read == 0)
   {
      uint8_t sz[4];
      put32(sz, s->own.rom_size);
      fe_sha1_update(&s->hash_ctx, sz, 4);
   }
   while (s->own_sample < SAMPLE_N && !io_step_over(s, t0, budget))
   {
      if (fseek(s->hash_f, (long)sample_off(s, s->own_sample), SEEK_SET) != 0 ||
          fread(blk, 1, SAMPLE_LEN, s->hash_f) != SAMPLE_LEN)
         return -1;
      fe_sha1_update(&s->hash_ctx, blk, SAMPLE_LEN);
      s->own_sample++;
      s->own_read += SAMPLE_LEN;
      budget = budget > SAMPLE_LEN ? budget - SAMPLE_LEN : 0;
   }
   return 0;
}

static void setup_step(fe_gblink *s)
{
   if (!s->own_hashed && rom_sampled(s))
   {
      if (sampled_step(s) != 0)
      {
         fail(s, FE_GBLINK_E_ROM_MISSING, 1);
         return;
      }
      if (s->own_sample < SAMPLE_N)
         return;
      fe_sha1_final(&s->hash_ctx, s->own.sha1);
      fclose(s->hash_f);
      s->hash_f = NULL;
      s->own_hashed = 1;
      {
         char h[41];
         FE_EVT_ONLY(h);
         hex20(s->own.sha1, h);
         fe_evt("gblink_own_rom sampled=%u sha1=%s size=%u", SAMPLE_N, h,
                (unsigned)s->own.rom_size);
      }
   }
   if (!s->own_hashed)
   {
      uint32_t budget = HASH_READ;
      uint64_t t0 = s->now_us ? s->now_us() : 0;
      while (s->own_read < s->own.rom_size && !io_step_over(s, t0, budget))
      {
         size_t want = s->own.rom_size - s->own_read, got;
         uint8_t blk[4096];
         uint8_t *dst = s->own_buf ? s->own_buf + s->own_read : blk;
         if (want > 4096)
            want = 4096;
         got = fread(dst, 1, want, s->hash_f);
         if (got != want)
         {
            fail(s, FE_GBLINK_E_ROM_MISSING, 1);
            return;
         }
         fe_sha1_update(&s->hash_ctx, dst, got);
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
   if (s->lookup_pending && s->own_hashed)
   {
      s->lookup_pending = 0;
      partner_lookup(s);
      if (s->state == FE_GBLINK_FAILED)
         return;
   }
   if (!s->peer_up)
      return;
   if (!s->hello_sent)
      send_hello(s);
   if (!s->have_hello)
      return;
   if (s->config_defer)
      config_auto_step(s);
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
   if (!s->ready_sent && setup_complete(s) && !s->l.is_host)
   {
      if (((s->core_rx ^ s->l.core_mask) & ~FE_GBLINK_MASK_NEGOTIABLE) &&
          (!s->corecfg_wait || s->corecfg_have))
      {
         /* the two consoles would compute different machines: refuse */
         fe_evt("gblink_core_mismatch host=%08x own=%08x",
                (unsigned)s->core_rx, (unsigned)s->l.core_mask);
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      /* the negotiable bits are the host's (0 from a host that sent none) */
      if (!s->corecfg_wait || s->corecfg_have)
         s->l.core_mask = (s->l.core_mask & ~FE_GBLINK_MASK_NEGOTIABLE) |
                          (s->core_rx & FE_GBLINK_MASK_NEGOTIABLE);
   }
   if (!s->ready_sent && setup_complete(s) &&
       (s->l.is_host || !s->corecfg_wait || s->corecfg_have))
   {
      uint8_t m[1] = { M_READY };
      sendm(s, m, 1);
      s->ready_sent = 1;
   }
   if (s->ready_sent && s->peer_ready && s->state == FE_GBLINK_SETUP)
      start_pair(s);
}

/* -------------------------------------------------------- running -- */

/* INPUT BATCHING (layout.core_mask bit 1, docs/GBA-LINK-MIRROR.md 28): the
 * buttons for two consecutive frames leave in one message, so the session
 * puts one packet on the air every second frame instead of every frame.
 * The same buttons reach the same frames; only how they travel changes.
 * Both consoles must agree (a build that does not know M_INPUTS would refuse
 * it), which the core-mask handshake checks before the session starts.
 * A held frame is flushed alone whenever the pair stalls, so batching can
 * delay a frame's buttons by one local frame at most. */
static void flush_input_batch(fe_gblink *s)
{
   uint8_t m[1 + 4 + 1 + 2 * 2];
   unsigned i;
   if (!s->ib_n)
      return;
   m[0] = M_INPUTS;
   put32(m + 1, s->ib_first);
   m[5] = (uint8_t)s->ib_n;
   for (i = 0; i < s->ib_n; i++)
      put16(m + 6 + 2 * i, s->ib_keys[i]);
   sendm(s, m, 6 + 2 * s->ib_n);
   s->ib_n = 0;
}

static void send_input(fe_gblink *s, uint32_t frame, uint16_t buttons)
{
   uint8_t m[7];
   if (s->l.core_mask & FE_GBLINK_MASK_INPUT_BATCH)
   {
      if (s->ib_n && s->ib_first + s->ib_n != frame)
         flush_input_batch(s);
      if (!s->ib_n)
      {
         s->ib_first = frame;
         s->ib_age = 0;
      }
      s->ib_keys[s->ib_n++] = buttons;
      /* STAGGER: the host's packet leaves on odd frames, the guest's on
       * even ones.  Both consoles are on the same frame at about the same
       * moment, so without this both would send together and each ack
       * would wait two whole frames for its ride (measured in PPSSPP:
       * the ride time ran out and bare ACKs came back).  Staggered, an
       * ack rides on the packet that leaves one frame later. */
      if (s->ib_n >= 2 || (frame & 1u) == (s->local_slot ? 0u : 1u))
         flush_input_batch(s);
      return;
   }
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
   if (fast == 2)
      s->st.inputs_resent++;
   else if (fast)
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
   {
      s->need_slot = slot;           /* what a wait waits for */
      s->need_frame = frame;
      return 0;                      /* partner's buttons not here yet */
   }
   if (slot != s->local_slot)
   {
      int32_t lead = (int32_t)(s->have[slot] - 1 - frame);
      if (!s->win.lead_n || lead < s->win.lead_min)
         s->win.lead_min = lead;
      s->win.lead_n++;
      s->win.lead_sum += (uint32_t)lead;
   }
   *buttons = s->q[slot][frame % RING];
   return 1;
}

/* The buttons a stalled pair waits for have arrived (or waiting is over). */
static int need_met(const fe_gblink *s)
{
   if (s->state != FE_GBLINK_RUNNING && s->state != FE_GBLINK_ENDING)
      return 1;
   return s->captured || s->have[s->need_slot] > s->need_frame;
}

/* On the heap, for the session only: a static 128 KiB buffer here cost
 * every game, GBA included, that much heap (Emerald post-load 1.66 -> 1.50
 * MB in PPSSPP) for something only a link session uses. */
static uint32_t cart_crc(fe_gblink *s, int slot)
{
   const gbcore_api_t *api;
   gbcore_t *c;
   size_t n, need;
   if (s->m)
      return s->m->cart_crc(s->m->user, slot);
   api = gbdual_api(slot);
   c = gbdual_core(s->dual, slot);
   n = api->cart_ram_size(c);
   need = api->save_ram_size(c) + 64;
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
   return s->l.hash_legacy ? fe_crc32(0, s->crc_buf, n)
                           : ram_hash(s->crc_buf, n);
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

/* A vtable pair: this console's machine has just completed the agreed frame
 * (its own frame boundary); the final hash was taken when slot 0 did. */
static void capture_end_m(fe_gblink *s)
{
   const fe_gblink_machines *m = s->m;
   uint8_t msg[13];
   size_t n = m->save_size(m->user, s->local_slot);
   if (n)
   {
      s->final_save = (uint8_t *)malloc(n);
      if (!s->final_save ||
          m->save_read(m->user, s->local_slot, s->final_save, n) != 0)
      {
         fail(s, FE_GBLINK_E_MEMORY, 1);
         return;
      }
   }
   s->final_save_size = n;
   n = m->state_size(m->user, s->local_slot);
   if (n)
   {
      s->final_state = (uint8_t *)malloc(n);
      if (!s->final_state ||
          m->state_save(m->user, s->local_slot, s->final_state, n) != 0)
      {
         fail(s, FE_GBLINK_E_MEMORY, 1);
         return;
      }
      s->final_state_size = n;
   }
   s->captured = 1;
   msg[0] = M_FINAL;
   put32(msg + 1, s->final_frame);
   put64(msg + 5, s->final_hash);
   sendm(s, msg, sizeof(msg));
   fe_evt("gblink_final frame=%u hash=%016llx save=%u state=%u t_ms=%llu",
          s->final_frame, (unsigned long long)s->final_hash,
          (unsigned)s->final_save_size, (unsigned)s->final_state_size,
          evt_ms(s));
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
   if (s->m && s->final_hashed && !s->captured && slot == s->local_slot &&
       f >= s->final_frame && s->state != FE_GBLINK_FAILED)
   {
      if (f != s->final_frame)
         fe_evt("gblink_final_capture_late frame=%u end=%u", f,
                s->final_frame);
      capture_end_m(s);           /* the local machine's own boundary */
   }
   if (slot != 0 || s->state == FE_GBLINK_FAILED)
      return;
   if (f % SRAM_EVERY == 0)
   {
      int i;
      uint64_t t0 = clock_us(s), t1;
      FE_LP_SCOPE(FELP_CRC);
      for (i = 0; i < GBDUAL_SLOTS; i++)
      {
         uint32_t c = cart_crc(s, i);
         if (c != s->sram_crc[i])
         {
            s->sram_crc[i] = c;
            s->sram_changed_at = f;
         }
      }
      t1 = clock_us(s);
      s->win.crc_n++;
      win_time(&s->win.crc_us, &s->win.crc_us_max, t0, t1);
      s->hook_us += t1 > t0 ? (uint32_t)(t1 - t0) : 0;
   }
   if (s->hash_interval && f % s->hash_interval == 0)
   {
      unsigned i = (f / s->hash_interval) % HASH_RING;
      uint8_t m[13];
      uint64_t t0 = clock_us(s);
      FE_LP_SCOPE(FELP_HASH);
      s->own_h[i].frame = f;
      s->own_h[i].hash = s->m ? s->m->sync_hash(s->m->user)
                              : gbdual_sync_hash(s->dual);
      {
         uint64_t t1 = clock_us(s);
         s->win.hash_n++;
         win_time(&s->win.hash_us, &s->win.hash_us_max, t0, t1);
         s->hook_us += t1 > t0 ? (uint32_t)(t1 - t0) : 0;
      }
      s->st.last_hash = s->own_h[i].hash;
      s->st.last_hash_frame = f;
      m[0] = M_HASH;
      put32(m + 1, f);
      put64(m + 5, s->own_h[i].hash);
      sendm(s, m, sizeof(m));
      s->st.hashes_sent++;
      compare_hash(s, f);
   }
   if (s->end_frame && f == s->end_frame && !s->captured &&
       !s->final_hashed)
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
      if (s->m)
      {
         /* Both consoles take the final hash here, at slot 0's boundary; the
          * local machine is captured at its own (above, or at once if it is
          * slot 0). */
         FE_LP_SET(FELP_HASH);
         s->final_hash = s->m->sync_hash(s->m->user);
         FE_LP_SET(FELP_CORE);
         s->final_frame = f;
         s->final_hashed = 1;
         if (s->local_slot == 0)
            capture_end_m(s);
         return;
      }
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
   s->end_agreed = f;
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
      if (s->m && s->captured && s->state == FE_GBLINK_ENDING &&
          ++s->final_wait > WAIT_MAX_STEPS)
      {
         fe_evt("gblink_final_timeout steps=%u", s->final_wait);
         fail(s, FE_GBLINK_E_PEER_LOST, 1);
      }
      return FE_GBLINK_IDLE;
   }
   /* input batching: a held frame never waits more than two steps for its
    * companion (the end of a session, a paused local machine) */
   if (s->ib_n && ++s->ib_age > 2)
      flush_input_batch(s);
   s->pending_buttons = local_buttons;
   {
      uint64_t t0 = clock_us(s), ta;
      uint32_t waited = 0;
      int waits = 0;
      s->hook_us = 0;
      s->win.steps++;
      FE_LP_SET(FELP_CORE);
      r = s->m ? s->m->advance(s->m->user, input_cb, s)
               : gbdual_advance(s->dual, input_cb, s);
      FE_LP_SET(FELP_SESS);
      ta = clock_us(s);
      /* Stall absorption: the partner's buttons are late, but maybe only by
       * a little.  Pump and retry while the platform says this display
       * frame still has time, instead of giving the whole frame up.  The
       * advance resumes exactly where it stopped, so the machines compute
       * the same thing either way (fe_gblink.h, wait). */
      while (r == 0 && s->p.wait && s->state != FE_GBLINK_FAILED &&
             !s->captured)
      {
         uint64_t w0 = clock_us(s), w1;
         while (!need_met(s) &&
                s->p.wait(s->p.user, waited + (uint32_t)(clock_us(s) - w0)))
            ;
         w1 = clock_us(s);
         waited += w1 > w0 ? (uint32_t)(w1 - w0) : 0;
         waits++;
         if (!need_met(s) || s->state == FE_GBLINK_FAILED || s->captured)
            break;
         FE_LP_SET(FELP_CORE);
         r = s->m ? s->m->advance(s->m->user, input_cb, s)
               : gbdual_advance(s->dual, input_cb, s);
         FE_LP_SET(FELP_SESS);
         ta = clock_us(s);
      }
      {
         /* machine time = the advances, less the waits and the hooks */
         uint32_t step = ta > t0 ? (uint32_t)(ta - t0) : 0;
         uint32_t adv = step > waited + s->hook_us
                           ? step - waited - s->hook_us : 0;
         s->win.adv_us += adv;
         if (adv > s->win.adv_us_max)
            s->win.adv_us_max = adv;
         s->win.step_us += step;
         if (step > s->win.step_us_max)
            s->win.step_us_max = step;
         s->win.wait_us += waited;
         if (waits && r == 1)
         {
            s->win.absorbed++;
            s->win.absorb_us += waited;
            if (waited > s->win.absorb_us_max)
               s->win.absorb_us_max = waited;
         }
      }
   }
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
      s->win.frames++;
      return FE_GBLINK_FRAME;
   }
   if (r < 0)
   {
      fail(s, FE_GBLINK_E_START, 1);
      return FE_GBLINK_IDLE;
   }
   s->st.stalls++;
   s->win.stalls++;
   flush_input_batch(s);        /* never hold buttons across a stall */
   nack_maybe(s);
   if (s->streak == 0)
   {
      s->st.stall_episodes++;
      s->win.stall_episodes++;
   }
   if (++s->streak > s->st.stall_streak_max)
      s->st.stall_streak_max = s->streak;
   if (s->streak > s->win.streak_max)
      s->win.streak_max = s->streak;
   if (s->m && s->streak > WAIT_MAX_STEPS)
   {
      fe_evt("gblink_stall_timeout steps=%u f0=%llu f1=%llu", s->streak,
             (unsigned long long)s->st.frames[0],
             (unsigned long long)s->st.frames[1]);
      fail(s, FE_GBLINK_E_PEER_LOST, 1);
      return FE_GBLINK_IDLE;
   }
   return FE_GBLINK_STALL;
}

void fe_gblink_take_window(fe_gblink *s, fe_gblink_window *out)
{
   if (!s || !out)
      return;
   *out = s->win;
   out->delay = s->delay;
   memset(&s->win, 0, sizeof(s->win));
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
      s->host_caps = s->l.is_host ? 0 : q[1];
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
      if (s->m ? (s->peer.rom_size < s->m->rom_min ||
                  s->peer.rom_size > s->m->rom_max ||
                  s->peer.save_size > s->m->save_max ||
                  s->peer.state_size > STATE_MAX ||
                  s->peer.console != s->m->console)
               : (s->peer.rom_size < 0x150 ||
                  s->peer.rom_size > 8u * 1024 * 1024 ||
                  s->peer.save_size > 0x20000 + RTC_TRAILER ||
                  s->peer.state_size > STATE_MAX ||
                  (s->peer.console != FE_CONSOLE_GB &&
                   s->peer.console != FE_CONSOLE_GBC)))
      {
         fe_evt("gblink_hello_refused console=%u rom=%u save=%u state=%u",
                (unsigned)s->peer.console, (unsigned)s->peer.rom_size,
                (unsigned)s->peer.save_size, (unsigned)s->peer.state_size);
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
         if (s->l.delay_auto && s->p.rtt_us)
            s->config_defer = 1;     /* setup_step: config_auto_step */
         else
         {
            send_config(s);
            config_known(s);
         }
      }
      else if (s->host_caps & FE_GBLINK_HOSTCAP_NEGOTIATE)
      {
         /* a host that negotiates: say what this build can do (whatever
          * this console's own setting is -- the host decides) and wait
          * for its answer before READY */
         uint8_t cm[5];
         cm[0] = M_CAPS;
         put32(cm + 1, FE_GBLINK_MASK_NEGOTIABLE);
         sendm(s, cm, sizeof(cm));
         s->corecfg_wait = 1;
         fe_evt("gblink_caps sent=%08x", (unsigned)FE_GBLINK_MASK_NEGOTIABLE);
      }
      /* A vtable pair compares SHA-1s only: wait for our own. */
      if (s->m && !s->own_hashed)
         s->lookup_pending = 1;
      else
         partner_lookup(s);
      break;
   }
   case M_CONFIG:
      if (len != CONFIG_LEN || s->l.is_host || s->have_config)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      if (b[1] > CONFIG_DELAY_MAX || get16(b + 10) == 0 ||
          get16(b + 12) == 0 || get16(b + 12) > CONFIG_BATCH_MAX)
      {
         fe_evt("gblink_config_refused delay=%u hash_every=%u batch=%u",
                (unsigned)b[1], (unsigned)get16(b + 10),
                (unsigned)get16(b + 12));
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->delay = b[1];
      s->seed = (int64_t)get64(b + 2);
      s->hash_interval = get16(b + 10);
      s->batch = get16(b + 12);
      config_known(s);
      break;
   case M_CORECFG:
      if (len != 5 || s->l.is_host || s->corecfg_have ||
          s->state != FE_GBLINK_SETUP)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      s->core_rx = get32(b + 1);
      s->corecfg_have = 1;
      break;
   case M_CAPS:
   {
      uint8_t cm[5];
      uint32_t can;
      if (len != 5 || !s->l.is_host || s->caps_have ||
          s->state != FE_GBLINK_SETUP)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      can = get32(b + 1);
      s->caps_have = 1;
      /* the host decides, within what the guest's build can do */
      s->l.core_mask = (s->l.core_mask & ~FE_GBLINK_MASK_NEGOTIABLE) |
                       (s->l.core_mask & FE_GBLINK_MASK_NEGOTIABLE & can);
      cm[0] = M_CORECFG;
      put32(cm + 1, s->l.core_mask);
      sendm(s, cm, sizeof(cm));
      fe_evt("gblink_core_mask sent=%08x guest_can=%08x",
             (unsigned)s->l.core_mask, (unsigned)can);
      break;
   }
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
      /* One byte, during setup, after the HELLO it answers -- and, for a
       * guest, after the CONFIG the host sends before it.  (Every build
       * sends it so; a repeat is harmless.) */
      if (len != 1 || s->state != FE_GBLINK_SETUP || !s->have_hello ||
          (!s->l.is_host && !s->have_config))
      {
         fe_evt("gblink_ready_refused len=%u state=%d hello=%d config=%d",
                (unsigned)len, (int)s->state, s->have_hello, s->have_config);
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
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
   case M_INPUTS:
   {
      int r = s->local_slot ^ 1;
      uint32_t f;
      unsigned i, n;
      FE_EVT_ONLY(r);
      if (len < 8 || !s->have_config ||
          !(s->l.core_mask & FE_GBLINK_MASK_INPUT_BATCH) ||
          (n = b[5]) == 0 || n > 2 || len != 6 + 2 * n)
      {
         fail(s, FE_GBLINK_E_PROTO, 1);
         return;
      }
      f = get32(b + 1);
      for (i = 0; i < n; i++)
         if (take_input(s, f + i, (uint16_t)get16(b + 6 + 2 * i), 0) < 0)
         {
            fe_evt("gblink_input_gap got=%u want=%llu", f + i,
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
      if (s->end_agreed)
      {
         /* a resend (BK_RESEND) carried it first: the same frame, or the
          * two consoles disagree */
         if (get32(b + 1) != s->end_agreed)
         {
            fe_evt("gblink_end_conflict at=%u agreed=%u", get32(b + 1),
                   s->end_agreed);
            fail(s, FE_GBLINK_E_PROTO, 1);
         }
         break;
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
      if (s->state != FE_GBLINK_DONE && s->state != FE_GBLINK_FAILED)
         s->peer_reason = len > 1 ? b[1] : 0;
      fail(s, FE_GBLINK_E_PEER_ABORT, 0);
      break;
   default:
      fail(s, FE_GBLINK_E_PROTO, 1);
      break;
   }
}

/* ---------------------------------------------------- introspection -- */

int fe_gblink_state(const fe_gblink *s) { return s ? s->state : FE_GBLINK_FAILED; }

int fe_gblink_fast_link(const fe_gblink *s, int *source)
{
   int on, src;
   if (!s)
      return 0;
   on = (s->l.core_mask & FE_GBLINK_MASK_INPUT_BATCH) != 0;
   if (s->l.is_host)
      src = (!on && (s->own_mask & FE_GBLINK_MASK_INPUT_BATCH))
               ? FE_GBLINK_FL_OLD_PEER : FE_GBLINK_FL_OWN;
   else
      src = (s->host_caps & FE_GBLINK_HOSTCAP_NEGOTIATE) ? FE_GBLINK_FL_HOST
                                                         : FE_GBLINK_FL_OLD_PEER;
   if (source)
      *source = src;
   return on;
}
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
      if (!s->own_hashed && rom_sampled(s))
      {
         snprintf(l1, c1, "Checking the game...");
         if (p1)
            *p1 = (int)(s->own_sample * 1000u / SAMPLE_N);
      }
      else if (!s->own_hashed)
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

void fe_gblink_get_progress(const fe_gblink *s, fe_gblink_progress *o)
{
   if (!o)
      return;
   memset(o, 0, sizeof(*o));
   o->peer_reason = -1;
   if (!s)
   {
      o->state = FE_GBLINK_FAILED;
      o->error = FE_GBLINK_E_LOCAL;
      o->phase = FE_GBLINK_PH_FAILED;
      return;
   }
   o->state = s->state;
   o->error = s->error;
   o->peer_reason = s->peer_reason;
   o->ran = s->m ? s->m_running : s->dual != NULL;
   o->peer_up = s->peer_up;
   if (s->have_hello)
   {
      title_of(s->peer.header, o->peer_title);
      memcpy(o->peer_header, s->peer.header, sizeof(o->peer_header));
      o->have_peer_header = 1;
   }
   switch (s->state)
   {
   case FE_GBLINK_SETUP:
      if (!s->own_hashed)
      {
         o->phase = FE_GBLINK_PH_CHECK;
         if (rom_sampled(s))
         {
            o->done = s->own_sample;
            o->total = SAMPLE_N;
         }
         else
         {
            o->done = s->own_read;
            o->total = s->own.rom_size;
         }
      }
      else if (!s->peer_up)
         o->phase = FE_GBLINK_PH_WAIT_PEER;
      else if (!s->have_hello)
         o->phase = FE_GBLINK_PH_HELLO;
      else if (s->partner_buf && s->partner_rx < s->peer.rom_size)
      {
         o->phase = FE_GBLINK_PH_ROM_RX;
         o->done = s->partner_rx;
         o->total = s->peer.rom_size;
      }
      else if (s->peer_need == 1 && s->rom_tx_off < s->own.rom_size)
      {
         o->phase = FE_GBLINK_PH_ROM_TX;
         o->done = s->rom_tx_off;
         o->total = s->own.rom_size;
      }
      else if (s->verifying)
      {
         o->phase = FE_GBLINK_PH_VERIFY;
         o->done = s->peer.rom_size - s->verify_left;
         o->total = s->peer.rom_size;
      }
      else if (!s->have_config || !s->partner_ready)
         o->phase = FE_GBLINK_PH_HELLO;
      else
      {
         uint32_t tot = (uint32_t)s->own_save_size + s->own.state_size +
                        s->peer.save_size + s->peer.state_size;
         uint32_t done = s->save_tx_off + s->state_tx_off +
                         s->peer_save_rx + s->peer_state_rx;
         o->phase = done >= tot ? FE_GBLINK_PH_START : FE_GBLINK_PH_SHARE;
         o->done = done > tot ? tot : done;
         o->total = tot;
      }
      break;
   case FE_GBLINK_RUNNING:
      o->phase = FE_GBLINK_PH_RUNNING;
      break;
   case FE_GBLINK_ENDING:
      o->phase = FE_GBLINK_PH_ENDING;
      break;
   case FE_GBLINK_DONE:
      o->phase = FE_GBLINK_PH_DONE;
      break;
   default:
      o->phase = FE_GBLINK_PH_FAILED;
      break;
   }
}
