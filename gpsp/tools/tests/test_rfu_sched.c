/* Unit test of the adapter's one protocol, scheduled ticks (rfu_sched.h,
 * docs/RFU-COUPLING.md, code-health C-4).
 *
 * TWO production rfu.c instances talk over an in-memory netpacket wire: A is
 * the host (netplay peer 0), B its client (peer 1).  This file is compiled
 * twice.  With -DRFU_SCHED_INSTANCE it is one adapter: rfu.c, its stubs and
 * a few rs_* accessors.  That object is linked as A, and again as B with
 * every global it defines renamed rsb_* (objcopy, the gbab_ trick of the GBA
 * link pair).  Without the define it is the driver: the wire, the "games"
 * and the checks.  The build is in tools/run_host_tests.py (rfu_sched).
 *
 * Covered: the SYNC/ACK offset; delivery at F + off + D, exactly, never
 * early; the frame mark in byte 91 (host) / 87 (client) of a frame's last
 * data packet and the bare TICK of a frame with none; D = 1 while either side
 * is in a librfu NI exchange, for RFU_SCHED_NI_FRAMES; PAUSE; silence ->
 * give_up -> relock (a client's fresh SYNC, a host's AGAIN).  netpacket_host's
 * NP_RFU_FEATURES is checked in tools/test_gb_np_host.c, where its fakes live.
 * Two consoles only (DECISIONS.md 2): a third console is not tested.
 *
 * Mutants (argv[1]), each of which must make this test FAIL (the runner
 * checks): "d0" runs B with D = 0, "nomark" strips the frame mark off the
 * wire, as a sender that skipped it would. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RFU_SCHED_INSTANCE
/* ------------------------------------------------------------ one adapter */
#include "../../rfu.c"

u32 cpu_ticks;
u16 rand_gen(void) { return 0x4567; }
void rand_seed(u32 s) { (void)s; }
u16 io_registers[512];

int rs_self;   /* which wire end this instance is (set by the driver) */
void rs_wire_send(int from, uint16_t dst, const void *buf, size_t len);
void netpacket_send(uint16_t id, const void *buf, size_t len)
{ rs_wire_send(rs_self, id, buf, len); }
void netpacket_send_unreliable(uint16_t id, const void *buf, size_t len)
{ rs_wire_send(rs_self, id, buf, len); }
void netpacket_poll_receive(void) {}

/* A powered-on adapter at `frame`, linked the way a finished CONNECT leaves
 * it: a host with one client in slot 0, or a client of `peer`. */
void rs_fresh(int host, uint16_t peer, u32 frame)
{
  rfu_reset();
  memset(&rfu_client, 0, sizeof(rfu_client));
  rfu_sched_stop();
  rfu_sched_paused = 0;
  rfu_sched_ni_at = -100000;
  rfu_sched_relock_n = 0;
  rfu_sched_held_peak = 0;
  rfu_q_hi[0] = rfu_q_hi[1] = 0;
  rfu_frame_no = frame;
  if (host) {
    rfu_state = RFU_STATE_HOST;
    rfu_host.devid = 0x1111;
    rfu_host.accepting = 1;
    rfu_host.clients[0].devid = 0x2222;
    rfu_host.clients[0].client_id = peer;
  } else {
    rfu_state = RFU_STATE_CLIENT;
    rfu_client.host_id = peer;
    rfu_client.devid = 0x2222;
    rfu_client.clnum = 0;
  }
}

/* The game sends one link frame: a UNI sub-frame (LLSF state 4), or an NI
 * one (state 1), whose first payload byte is `tag`. */
void rs_game_send(unsigned tag, int ni)
{
  u8 d[20];
  u32 st = ni ? 1 : 4, blen, i;
  memset(d, 0, sizeof(d));
  if (rfu_state == RFU_STATE_HOST) {
    u32 hdr = 14 | (st << 14) | (1u << 18);      /* 3-byte parent header */
    d[0] = (u8)hdr; d[1] = (u8)(hdr >> 8); d[2] = (u8)(hdr >> 16);
    d[3] = (u8)tag;
    blen = 17;
    rfu_buf[0] = blen;
  } else {
    u32 hdr = 12 | (st << 10);                   /* 2-byte child header */
    d[0] = (u8)hdr; d[1] = (u8)(hdr >> 8);
    d[2] = (u8)tag;
    blen = 14;
    rfu_buf[0] = blen << (8 + rfu_client.clnum * 5);
  }
  for (i = 0; i < (blen + 3) / 4; i++)
    rfu_buf[1 + i] = leupack32(&d[i * 4]);
  rfu_cmd = RFU_CMD_SEND_DATA;
  rfu_plen = (u8)(1 + (blen + 3) / 4);
  rfu_process_command_inner();
}

/* The game reads one packet: its tag, or -1 when there is none. */
int rs_game_recv(void)
{
  rfu_cmd = RFU_CMD_RECV_DATA;
  rfu_plen = 0;
  rfu_process_command_inner();
  if (!rfu_buf[0])
    return -1;
  return rfu_state == RFU_STATE_HOST ? (int)((rfu_buf[1] >> 16) & 0xFF)
                                     : (int)(rfu_buf[1] >> 24);
}

void rs_frame_end(void) { rfu_frame_update(); }
void rs_receive(const void *buf, size_t len, uint16_t from)
{ rfu_net_receive(buf, len, from); }
void rs_unlink(void) { rfu_state = RFU_STATE_IDLE; }
s32 rs_frame(void) { return (s32)rfu_frame_no; }
s32 rs_off(void) { return rfu_sched_off; }
s32 rs_cur_d(void) { return rfu_sched_cur_d(); }
int rs_on(void) { return (int)rfu_sched_on; }
unsigned rs_relocks(void) { return rfu_sched_relock_n; }
void rs_config(unsigned d) { rfu_sched_config(d); }
int rs_may_run(void) { return rfu_sched_may_run(); }
void rs_pause(int on) { rfu_sched_pause(on); }
int rs_peer_paused(void) { return rfu_sched_peer_is_paused(); }
void rs_give_up(void) { rfu_sched_give_up(); }
unsigned rs_qhi(void) { return rfu_q_hi[0] > rfu_q_hi[1] ? rfu_q_hi[0] : rfu_q_hi[1]; }
int rs_partner(void) { return rfu_sched_partner(); }

#else
/* ----------------------------------------------------------------- driver */
#include "../../rfu_sched_api.h"     /* the inline rules: no symbols used */
#include "../../frontend-common/fe_linkwait.h"   /* the wait notice's rules */
#define ND_HELD_SILENT_T 1          /* netdrv.h ND_HELD_SILENT */
#define ND_HELD_GAP_T    2          /* netdrv.h ND_HELD_GAP */
#define NET_RFU_HEADER      0x52465531u
#define NET_RFU_HOST_SEND   0x05u
#define NET_RFU_CLIENT_SEND 0x06u
#define NET_RFU_SCHED_SYNC  0x09u
#define NET_RFU_SCHED_ACK   0x0Au
#define NET_RFU_SCHED_TICK  0x0Bu
#define NET_RFU_SCHED_PAUSE 0x0Cu
#define NET_RFU_SCHED_AGAIN 0x0Du
#define MARK                0xA5u
#define NI_FRAMES           120     /* RFU_SCHED_NI_FRAMES */

#define API(p) \
  void p##rs_fresh(int host, uint16_t peer, uint32_t frame); \
  void p##rs_game_send(unsigned tag, int ni); \
  int p##rs_game_recv(void); \
  void p##rs_frame_end(void); \
  void p##rs_receive(const void *buf, size_t len, uint16_t from); \
  void p##rs_unlink(void); \
  int32_t p##rs_frame(void); \
  int32_t p##rs_off(void); \
  int32_t p##rs_cur_d(void); \
  int p##rs_on(void); \
  unsigned p##rs_relocks(void); \
  void p##rs_config(unsigned d); \
  int p##rs_may_run(void); \
  void p##rs_pause(int on); \
  int p##rs_peer_paused(void); \
  void p##rs_give_up(void); \
  unsigned p##rs_qhi(void); \
  int p##rs_partner(void); \
  extern int p##rs_self;
API()
API(rsb_)

typedef struct {
  void (*fresh)(int, uint16_t, uint32_t);
  void (*game_send)(unsigned, int);
  int (*game_recv)(void);
  void (*frame_end)(void);
  void (*receive)(const void *, size_t, uint16_t);
  void (*unlink)(void);
  int32_t (*frame)(void), (*off)(void), (*cur_d)(void);
  int (*on)(void);
  unsigned (*relocks)(void);
  void (*config)(unsigned);
  int (*may_run)(void);
  void (*pause)(int);
  int (*peer_paused)(void);
  void (*give_up)(void);
  unsigned (*qhi)(void);
  int (*partner)(void);
} adapter;

#define ADAPTER(p) { p##rs_fresh, p##rs_game_send, p##rs_game_recv, \
  p##rs_frame_end, p##rs_receive, p##rs_unlink, p##rs_frame, p##rs_off, \
  p##rs_cur_d, p##rs_on, p##rs_relocks, p##rs_config, p##rs_may_run, \
  p##rs_pause, p##rs_peer_paused, p##rs_give_up, p##rs_qhi, p##rs_partner }
static const adapter ad[2] = { ADAPTER(), ADAPTER(rsb_) };
enum { A = 0, B = 1 };          /* A hosts (peer 0); B is its client (peer 1) */

static const char *mutant = "";
static int failures;
#define CHECK(c, ...) do { if (!(c)) { \
  printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); \
  printf("\n"); failures++; return; } } while (0)

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- the wire: one reliable, in-order queue per direction --------------- */
#define WQ 4096
typedef struct { uint16_t dst; uint16_t len; uint8_t d[128]; } pkt;
static pkt wq[2][WQ];
static unsigned wq_head[2], wq_tail[2];

/* What went out, per sender frame, for the mark checks. */
typedef struct { int32_t frame; uint32_t type, hdata; int mark; } sent;
static sent log_[2][8192];
static unsigned n_log[2];

void rs_wire_send(int from, uint16_t dst, const void *buf, size_t len)
{
  const uint8_t *b = (const uint8_t *)buf;
  pkt *p;
  if (len < 12 || len > sizeof(wq[0][0].d) || wq_tail[from] - wq_head[from] >= WQ) {
    printf("FAIL wire: %s sent %u bytes (queue %u)\n", from ? "B" : "A", (unsigned)len,
           wq_tail[from] - wq_head[from]);
    exit(1);
  }
  p = &wq[from][wq_tail[from]++ % WQ];
  p->dst = dst;
  p->len = (uint16_t)len;
  memcpy(p->d, buf, len);
  /* "nomark": a sender that never marks its frame's last packet. */
  if (!strcmp(mutant, "nomark") && len == 104)
    p->d[12 + (from == A ? 91 : 87)] = 0;
  if (n_log[from] < 8192) {
    sent *s = &log_[from][n_log[from]++];
    s->frame = ad[from].frame();
    s->type = be32(b + 4);
    s->hdata = be32(b + 8);
    s->mark = len == 104 && b[12 + (from == A ? 91 : 87)] == MARK;
  }
}

static int wire_down;           /* 1 = an outage: nothing is delivered */

static void pump(void)
{
  int from;
  if (wire_down)
    return;
  for (from = 0; from < 2; from++)
    while (wq_head[from] != wq_tail[from]) {
      pkt *p = &wq[from][wq_head[from]++ % WQ];
      if (p->dst == (from == A ? 1 : 0))
        ad[!from].receive(p->d, p->len, (uint16_t)from);
    }
}

/* ---- the games ---------------------------------------------------------- */
/* Each game sends `per_frame` packets a frame while its adapter is locked
 * (what a game sends before the lock is not scheduled), tagged with its own
 * frame number's low byte, so the receiver knows each packet's stamp and can
 * check the frame it arrives in. */
static int per_frame[2] = { 1, 1 };
static int send_ni[2];          /* 1 = the next packet is an NI frame */
static int frozen[2];           /* 1 = this console runs no frames (a stall) */
/* Per receiver: deliveries; `bad` = early (before stamp + off + the D in
 * force), later than the configured D, or out of stamp order; `shifted` =
 * after the stamp + off + D in force, which only a D that just dropped may
 * cause; `seen_d` = exactly at stamp + off + D, by D; `skipped` = a stamp
 * missing from the sequence (a loss, when the sender sends every frame).
 * `win_*`: deliveries whose stamp (the SENDER's frame) lies in
 * [win_lo, win_hi], and the largest delay among them, in the receiver's
 * frames -- what the receiving game sees of a stretch of the sender's
 * traffic (the NI window checks). */
static unsigned got[2], bad[2], shifted[2], seen_d[2][8], skipped[2];
static int32_t last_stamp[2];
static int32_t win_lo[2] = { 1, 1 }, win_hi[2], win_max[2];
static unsigned win_n[2];
static unsigned cfg_d = 2;      /* the D start() configures */
/* Since start(): data packets each game sent, and each game received (in
 * any state); 1 = this game also sends while its ticks are off (a console
 * that gave up keeps playing). */
static unsigned sent_all[2], got_all[2];
static int send_free[2];

static void reset_counts(void)
{
  memset(got, 0, sizeof(got));
  memset(bad, 0, sizeof(bad));
  memset(shifted, 0, sizeof(shifted));
  memset(skipped, 0, sizeof(skipped));
  memset(seen_d, 0, sizeof(seen_d));
  last_stamp[0] = last_stamp[1] = -1;
}

/* One frontend pass for console k.  1 = a frame ran. */
static int pass(int k)
{
  const adapter *a = &ad[k];
  int32_t f;
  int t, i;
  if (frozen[k] || !a->may_run())
    return 0;
  f = a->frame();
  /* The game reads what was handed over for this frame. */
  while ((t = a->game_recv()) >= 0) {
    got[k]++;
    got_all[k]++;
    if (a->on()) {
      int32_t d = a->cur_d();
      int32_t delay = (int32_t)(((uint32_t)(f - a->off()) - (uint32_t)t) & 0xFF);
      int32_t stamp = f - a->off() - delay;
      if (delay < d || delay > (int32_t)cfg_d || stamp < last_stamp[k])
        bad[k]++;
      else if (delay > d)
        shifted[k]++;
      else
        seen_d[k][d]++;
      if (stamp >= win_lo[k] && stamp <= win_hi[k]) {
        win_n[k]++;
        if (delay > win_max[k])
          win_max[k] = delay;
      }
      if (last_stamp[k] >= 0 && stamp > last_stamp[k] + 1)
        skipped[k]++;
      last_stamp[k] = stamp;
    }
  }
  if (a->on() || send_free[k])
    for (i = 0; i < per_frame[k]; i++) {
      a->game_send((unsigned)(f & 0xFF), send_ni[k]);
      send_ni[k] = 0;
      sent_all[k]++;
    }
  a->frame_end();
  return 1;
}

/* Run both consoles until each has run `frames` more frames (a frozen one
 * is not waited for).  0 = they deadlocked. */
static int run(int frames)
{
  int done[2] = { 0, 0 }, idle = 0;
  while ((done[A] < frames && !frozen[A]) || (done[B] < frames && !frozen[B])) {
    int ran = 0, k;
    for (k = 0; k < 2; k++)
      if (done[k] < frames && pass(k)) {
        done[k]++;
        ran = 1;
      }
    pump();
    if (!ran && ++idle > 50)
      return 0;
    if (ran)
      idle = 0;
  }
  return 1;
}

/* Run both, each whenever its gate lets it, until console k has run `frames`
 * more frames (run() asks the same count of both, which a console that is
 * already ahead cannot meet).  0 = deadlock. */
static int run_k(int k, int frames)
{
  int done = 0, idle = 0;
  while (done < frames) {
    int ran = pass(!k);
    if (pass(k)) {
      done++;
      ran = 1;
    }
    pump();
    if (!ran && ++idle > 50)
      return 0;
    if (ran)
      idle = 0;
  }
  return 1;
}

/* Fresh adapters, D on both, linked: A at host frame 1000, B at 37. */
static void start(void)
{
  memset(wq_head, 0, sizeof(wq_head));
  memset(wq_tail, 0, sizeof(wq_tail));
  memset(n_log, 0, sizeof(n_log));
  memset(frozen, 0, sizeof(frozen));
  memset(send_ni, 0, sizeof(send_ni));
  memset(send_free, 0, sizeof(send_free));
  memset(sent_all, 0, sizeof(sent_all));
  memset(got_all, 0, sizeof(got_all));
  wire_down = 0;
  per_frame[A] = per_frame[B] = 1;
  reset_counts();
  ad[A].fresh(1, 1, 1000);
  ad[B].fresh(0, 0, 37);
  win_lo[A] = win_lo[B] = 1;
  win_hi[A] = win_hi[B] = 0;
  ad[A].config(cfg_d);
  ad[B].config(!strcmp(mutant, "d0") ? 0 : cfg_d);
}

/* Watch what console k receives of its partner's frames [lo, hi]. */
static void watch(int k, int32_t lo, int32_t hi)
{
  win_lo[k] = lo;
  win_hi[k] = hi;
  win_max[k] = 0;
  win_n[k] = 0;
}

/* start(), then a few frames to lock, then fresh counts.  The lock itself is
 * one frame late on the client: the ACK arrives after the client's frame
 * F + off + D for the host's first stamped frame F has already run unlocked,
 * so that one packet is handed over a frame late.  A property of the
 * handshake, not an error, so the counts begin after it. */
static int linked(void)
{
  start();
  if (!run(5) || !ad[A].on() || !ad[B].on())
    return 0;
  reset_counts();
  return 1;
}

/* 1. The client's first linked frame sends SYNC with its frame; the host
 *    answers off = its frame - that, and the client takes -off. */
static void test_sync_offset(void)
{
  start();
  CHECK(ad[A].may_run() && ad[B].may_run(), "unlinked consoles run free");
  ad[B].frame_end();                       /* B's frame 37 ends: SYNC(37) */
  CHECK(n_log[B] >= 1 && log_[B][0].type == NET_RFU_SCHED_SYNC &&
        log_[B][0].hdata == 37, "first client packet is SYNC(37)");
  pump();                                  /* A, still at frame 1000, answers */
  pump();
  CHECK(ad[A].on() && ad[B].on(), "both locked after SYNC/ACK (A %d B %d)",
        ad[A].on(), ad[B].on());
  CHECK(ad[A].off() == 1000 - 37, "host off %d", ad[A].off());
  CHECK(ad[B].off() == -(1000 - 37), "client off %d", ad[B].off());
  printf("  sync: host off %d, client off %d\n", ad[A].off(), ad[B].off());
}

/* 2. Every packet reaches the other game in frame F + off + D -- never early,
 *    never late, none lost -- in both directions, at D = 1 for the first
 *    480 + 120 frames (a new link starts with handshakes) and D = 2 after. */
static void test_delivery(void)
{
  int k;
  CHECK(linked(), "no lock");
  CHECK(run(900), "deadlock (A %d B %d)", ad[A].frame(), ad[B].frame());
  for (k = 0; k < 2; k++) {
    CHECK(bad[k] == 0 && shifted[k] == 0 && skipped[k] == 0,
          "%s: %u packets off schedule, %u late, %u missing", k ? "B" : "A", bad[k],
          shifted[k], skipped[k]);
    CHECK(seen_d[k][1] > 400 && seen_d[k][2] > 200,
          "%s: D=1 for %u, D=2 for %u deliveries", k ? "B" : "A", seen_d[k][1],
          seen_d[k][2]);
    /* All but the last D frames' worth has arrived. */
    CHECK(got[k] >= 900 - 2, "%s: got %u of ~900", k ? "B" : "A", got[k]);
  }
  CHECK(ad[A].off() + ad[B].off() == 0, "offsets %d / %d", ad[A].off(), ad[B].off());
  printf("  delivery: A got %u, B got %u, all at F + off + D (D=1 %u/%u, D=2 %u/%u)\n",
         got[A], got[B], seen_d[A][1], seen_d[B][1], seen_d[A][2], seen_d[B][2]);
}

/* 3. A frame's last data packet carries the mark (host byte 91, client 87)
 *    and earlier ones do not; a frame with no data sends a bare TICK naming
 *    it.  Frames alternate 0, 1 and 2 packets. */
static void test_mark_and_tick(void)
{
  int k, i, f, cycle;
  CHECK(linked(), "no lock");
  for (k = 0; k < 2; k++)
    n_log[k] = 0;
  for (cycle = 0; cycle < 60; cycle++) {
    per_frame[A] = per_frame[B] = cycle % 3;
    CHECK(run(1), "deadlock at cycle %d", cycle);
  }
  for (k = 0; k < 2; k++) {
    uint32_t dtype = k == A ? NET_RFU_HOST_SEND : NET_RFU_CLIENT_SEND;
    int32_t f0 = log_[k][0].frame;
    for (f = f0; f < f0 + 55; f++) {
      unsigned data = 0, marked = 0, ticks = 0, last_marked = 0;
      for (i = 0; i < (int)n_log[k]; i++) {
        const sent *s = &log_[k][i];
        if (s->frame != f)
          continue;
        if (s->type == dtype) {
          data++;
          marked += (unsigned)s->mark;
          last_marked = (unsigned)s->mark;
        } else if (s->type == NET_RFU_SCHED_TICK) {
          ticks++;
          CHECK(s->hdata == (uint32_t)f, "%s TICK names %u in frame %d",
                k ? "B" : "A", s->hdata, f);
        }
      }
      if (data) {
        CHECK(marked == 1 && last_marked && ticks == 0,
              "%s frame %d: %u data, %u marked, last marked %u, %u ticks",
              k ? "B" : "A", f, data, marked, last_marked, ticks);
      } else {
        CHECK(ticks == 1, "%s frame %d: no data and %u ticks", k ? "B" : "A", f, ticks);
      }
    }
  }
  printf("  mark: last data packet marked (host 91, client 87), bare TICK otherwise\n");
}

/* 4. An NI frame drops D to 1 on both consoles for RFU_SCHED_NI_FRAMES
 *    frames: at once on the sender, on arrival at the receiver. */
static void test_ni(void)
{
  int32_t at_a, at_b, f;
  CHECK(linked(), "no lock");
  CHECK(run(700), "deadlock before NI");
  CHECK(ad[A].cur_d() == 2 && ad[B].cur_d() == 2, "settled D %d/%d",
        ad[A].cur_d(), ad[B].cur_d());
  send_ni[A] = 1;
  at_a = ad[A].frame();
  CHECK(run(1), "deadlock");
  CHECK(ad[A].cur_d() == 1, "sender D %d after its NI frame", ad[A].cur_d());
  /* The receiver turns at the arrival, before that frame is delivered. */
  at_b = -1;
  for (f = 0; f < 10 && at_b < 0; f++) {
    CHECK(run(1), "deadlock");
    if (ad[B].cur_d() == 1)
      at_b = ad[B].frame();
  }
  CHECK(at_b >= 0, "receiver never dropped to D = 1");
  while (ad[A].frame() - at_a < NI_FRAMES - 2)
    CHECK(run(1) && ad[A].cur_d() == 1, "sender back at D %d too early (+%d)",
          ad[A].cur_d(), ad[A].frame() - at_a);
  CHECK(run(4) && ad[A].cur_d() == 2, "sender still at D %d", ad[A].cur_d());
  while (ad[B].frame() - at_b < NI_FRAMES - 3)
    CHECK(run(1) && ad[B].cur_d() == 1, "receiver back at D %d too early",
          ad[B].cur_d());
  CHECK(run(6) && ad[B].cur_d() == 2, "receiver still at D %d", ad[B].cur_d());
  /* When D drops, the packet that was due under D = 2 comes with the one now
   * due under D = 1: one late delivery at each console, no more. */
  CHECK(bad[A] == 0 && bad[B] == 0 && shifted[A] <= 1 && shifted[B] <= 1,
        "off schedule %u/%u, late %u/%u", bad[A], bad[B], shifted[A], shifted[B]);
  printf("  NI: D = 1 for %d frames on both consoles, then 2 again\n", NI_FRAMES);
}

/* 5. PAUSE: the menu's state reaches the partner, once per change; the
 *    partner waits (it runs no frame past the last tick) and does not lose
 *    its lock. */
static void test_pause(void)
{
  unsigned n, i, pauses = 0;
  CHECK(linked(), "no lock");
  CHECK(run(30), "deadlock");
  n = n_log[A];
  ad[A].pause(1);
  ad[A].pause(1);                           /* no change: nothing sent */
  for (i = n; i < n_log[A]; i++)
    pauses += log_[A][i].type == NET_RFU_SCHED_PAUSE;
  CHECK(pauses == 1, "%u PAUSE packets for one change", pauses);
  pump();
  CHECK(ad[B].peer_paused(), "client did not see the pause");
  frozen[A] = 1;                            /* the menu is open: no frames */
  run(50);
  CHECK(!ad[B].may_run() && ad[B].on(), "client runs on (%d) or lost its lock (%d)",
        ad[B].may_run(), ad[B].on());
  ad[A].pause(0);
  frozen[A] = 0;
  pump();
  CHECK(!ad[B].peer_paused(), "client did not see the resume");
  CHECK(run(30), "deadlock after resume");
  CHECK(bad[A] + bad[B] + shifted[A] + shifted[B] == 0, "off schedule");
  printf("  pause: partner sees it once, waits, resumes on schedule\n");
}

/* 6. Silence: the side left waiting gives up (the frontend's limit), runs
 *    free, and the first tick from the partner locks the pair again -- the
 *    client by a fresh SYNC, the host by asking for one (AGAIN). */
static void test_give_up_relock(void)
{
  int waiter;
  for (waiter = B; waiter >= A; waiter--) {
    int other = !waiter;
    unsigned again = 0, i, n;
    CHECK(linked(), "no lock");
    CHECK(run(700), "deadlock");
    frozen[other] = 1;
    run(10);
    CHECK(!ad[waiter].may_run(), "%s does not wait for a silent partner",
          waiter ? "B" : "A");
    ad[waiter].give_up();
    CHECK(!ad[waiter].on() && ad[waiter].may_run(), "%s did not run free",
          waiter ? "B" : "A");
    CHECK(run(40), "deadlock while free");   /* the waiter runs ahead */
    n = n_log[A];
    frozen[other] = 0;
    CHECK(run(40), "deadlock after the partner came back");
    CHECK(ad[waiter].relocks() == 1, "%s relocked %u times", waiter ? "B" : "A",
          ad[waiter].relocks());
    CHECK(ad[A].on() && ad[B].on(), "not locked again (A %d B %d)", ad[A].on(),
          ad[B].on());
    CHECK(ad[A].off() + ad[B].off() == 0, "offsets %d / %d", ad[A].off(), ad[B].off());
    for (i = n; i < n_log[A]; i++)
      again += log_[A][i].type == NET_RFU_SCHED_AGAIN;
    CHECK(again == (waiter == A ? 1u : 0u), "host sent AGAIN %u times", again);
    /* What arrived while the pair was unlocked was handed over raw; from
     * here on every delivery is scheduled again. */
    reset_counts();
    CHECK(run(300), "deadlock after relock");
    CHECK(bad[A] + bad[B] + shifted[A] + shifted[B] == 0 && got[A] > 250 && got[B] > 250,
          "off schedule %u/%u, late %u/%u, got %u/%u", bad[A], bad[B], shifted[A],
          shifted[B], got[A], got[B]);
    printf("  give up + relock: %s waited, gave up, locked again (off %d)\n",
           waiter ? "client" : "host", ad[A].off());
  }
}

/* 7. A lost link stops the ticks: everything held is handed over and both
 *    consoles run free (rfu_sched_may_run's still-linked check). */
static void test_link_lost(void)
{
  CHECK(linked(), "no lock");
  CHECK(run(100), "deadlock");
  frozen[A] = 1;
  run(5);
  CHECK(!ad[B].may_run(), "client does not wait");
  ad[B].unlink();
  CHECK(ad[B].may_run() && !ad[B].on(), "client still scheduled after its link went");
  printf("  link lost: the ticks stop, the console runs free\n");
}

/* 8. The D a session runs follows its kind, which both consoles know: 2 ad
 *    hoc, 7 online; a harness value (the Lab's D study arms, 0 included)
 *    wins for every kind. */
static void test_d_for_kind(void)
{
  CHECK(rfu_sched_d_for(0, -1) == 2, "ad hoc D %u", rfu_sched_d_for(0, -1));
  CHECK(rfu_sched_d_for(1, -1) == 7, "online D %u", rfu_sched_d_for(1, -1));
  CHECK(rfu_sched_d_for(1, 2) == 2 && rfu_sched_d_for(0, 4) == 4 &&
        rfu_sched_d_for(1, 3) == 3, "harness D not used as given");
  CHECK(rfu_sched_d_for(1, 0) == 0 && rfu_sched_d_for(0, 0) == 0,
        "harness D = 0 (raw queues) not kept");
  CHECK(RFU_SCHED_D_DEFAULT == 2 && RFU_SCHED_D_ONLINE == 7, "constants");
  printf("  D by kind: ad hoc %u, online %u, a harness D wins\n",
         rfu_sched_d_for(0, -1), rfu_sched_d_for(1, -1));
}

/* 9. D = 4 does not widen the NI window (Mario Golf's lobby race,
 *    rfu_sched.h): measured at D = 2 and D = 4 alike.
 *    a. An NI exchange (a lobby: both games in NI every frame): after its
 *       first round trip, every NI packet reaches the other game 1 frame
 *       after it was sent, on both consoles.
 *    b. The race itself: the parent moves on to UNI and the child's press
 *       (NI) was sent in that frame: the parent's game gets it 1 frame later
 *       -- the parent's own NI sends keep it at D = 1 for RFU_SCHED_NI_FRAMES.
 *    c. What D does change: the first NI packet after > RFU_SCHED_NI_FRAMES
 *       of UNI on both sides arrives at most D frames later (the receiver may
 *       already have run D frames ahead), then the exchange runs at 1. */
static void test_ni_window_d4(void)
{
  static const unsigned ds[3] = { 2, 4, 7 };   /* ad hoc, the old online D, online */
  int32_t lobby_max[3][2], press[3], cold[3];
  int i, r, k;
  for (r = 0; r < 3; r++) {
    int32_t a0, b0, pf;
    cfg_d = ds[r];
    CHECK(linked(), "no lock at D=%u", cfg_d);
    CHECK(run(700), "deadlock before the lobby");
    CHECK(ad[A].cur_d() == (int32_t)cfg_d && ad[B].cur_d() == (int32_t)cfg_d,
          "settled D %d/%d, want %u", ad[A].cur_d(), ad[B].cur_d(), cfg_d);
    /* a. the lobby: 200 frames of NI both ways; skip its first round trip */
    a0 = ad[A].frame();
    b0 = ad[B].frame();
    watch(A, b0 + 10, b0 + 190);
    watch(B, a0 + 10, a0 + 190);
    for (i = 0; i < 200; i++) {
      send_ni[A] = send_ni[B] = 1;
      CHECK(run(1), "deadlock in the lobby (D=%u)", cfg_d);
    }
    for (k = 0; k < 2; k++) {
      CHECK(win_n[k] > 150, "%s saw %u lobby packets", k ? "B" : "A", win_n[k]);
      lobby_max[r][k] = win_max[k];
    }
    /* b. the parent (A) moves on to UNI; the child (B) presses in that frame.
     *    Worst case: the parent has run as far ahead as its gate lets it
     *    (the child stalls a moment first), as it does on a slow wire. */
    frozen[B] = 1;
    run(10);                   /* A runs until its gate stops it */
    frozen[B] = 0;
    pf = ad[B].frame();
    watch(A, pf, pf);
    send_ni[B] = 1;
    CHECK(run_k(B, 11), "deadlock at the press");
    CHECK(win_n[A] == 1, "parent got the press %u times", win_n[A]);
    press[r] = win_max[A];
    /* c. 300 frames of UNI on both sides, then a cold NI from the child */
    CHECK(run(300), "deadlock in UNI");
    CHECK(ad[A].cur_d() == (int32_t)cfg_d, "parent D %d after UNI", ad[A].cur_d());
    frozen[B] = 1;
    run(10);
    frozen[B] = 0;
    pf = ad[B].frame();
    watch(A, pf, pf);
    send_ni[B] = 1;
    CHECK(run_k(B, 12), "deadlock at the cold NI");
    CHECK(win_n[A] == 1, "parent got the cold NI %u times", win_n[A]);
    cold[r] = win_max[A];
    CHECK(bad[A] + bad[B] == 0, "off schedule %u/%u (D=%u)", bad[A], bad[B], cfg_d);
  }
  cfg_d = 2;
  for (r = 0; r < 3; r++) {
    CHECK(lobby_max[r][0] == 1 && lobby_max[r][1] == 1,
          "lobby NI delay D=%u %d/%d (want 1)", ds[r], lobby_max[r][0], lobby_max[r][1]);
    CHECK(press[r] == 1, "press delay D=%u %d (want 1)", ds[r], press[r]);
    CHECK(cold[r] <= (int32_t)ds[r], "cold NI delay D=%u %d (want <= D)", ds[r], cold[r]);
  }
  printf("  NI window: lobby 1 frame, press 1 frame at D=2, 4 and 7; "
         "cold NI %d / %d / %d frames\n", cold[0], cold[1], cold[2]);
}

/* 10. The gate's wait (rfu_gate_step): a partner whose ticks the network
 *     holds (netdrv_link_held > 0) is never given up on, however long; a
 *     paused one neither; one on a clear path is, after RFU_GATE_SILENT_US
 *     of clear silence -- counted from when the path cleared. */
static void test_gate_step(void)
{
  unsigned long long since = 0, t;
  int gave = 0;
  /* j7c1b2e's VPS: a gap held its tick 2.5 s; old rule gave up at 2 s */
  for (t = 1000000; t < 1000000 + 7000000ull && !gave; t += 12000)
    gave = rfu_gate_step(&since, t, 0, ND_HELD_GAP_T);
  CHECK(!gave, "gave up on a partner the network was holding (at %llu us)", t);
  for (t = 0; t < 7000000ull && !gave; t += 12000)
    gave = rfu_gate_step(&since, 9000000ull + t, 1, 0);
  CHECK(!gave, "gave up on a paused partner");
  since = 0;
  /* gap for 3 s, then the path clears: give up only 2 s after that */
  for (t = 0; t <= 3000000ull; t += 12000)
    CHECK(!rfu_gate_step(&since, 20000000ull + t, 0, ND_HELD_SILENT_T), "held");
  for (t = 3012000ull; !gave && t < 9000000ull; t += 12000)
    gave = rfu_gate_step(&since, 20000000ull + t, 0, 0);
  CHECK(gave && t - 3012000ull >= RFU_GATE_SILENT_US &&
        t - 3012000ull <= RFU_GATE_SILENT_US + 24000u,
        "clear silence gave up after %llu us", t - 3012000ull);
  /* no session / no such peer (-1): today's rule, 2 s */
  since = 0;
  gave = 0;
  for (t = 0; !gave && t < 9000000ull; t += 12000)
    gave = rfu_gate_step(&since, 30000000ull + t, 0, -1);
  CHECK(gave && t <= RFU_GATE_SILENT_US + 24000u, "no-peer gave up after %llu", t);
  printf("  gate: waits out a held network and a pause, gives up after 2 s of "
         "clear silence\n");
}

/* An outage: nothing crosses the wire for `passes` frontend passes.  Each
 * console runs until its gate stops it (at most D frames past the last tick
 * it holds).  With give_up_k >= 0 that console gives up meanwhile (the old
 * frontend's 2 s rule) and runs `free` frames with its game still playing. */
static void outage(int passes, int give_up_k, int free)
{
  int i, k;
  wire_down = 1;
  for (i = 0; i < passes; i++)
    for (k = 0; k < 2; k++)
      pass(k);
  if (give_up_k >= 0) {
    CHECK(!ad[give_up_k].may_run(), "not stalled before the give-up");
    ad[give_up_k].give_up();
    send_free[give_up_k] = 1;
    for (i = 0; i < free; i++)
      pass(give_up_k);
  }
  wire_down = 0;
}

/* Settle: the games stop sending, everything in flight is handed over. */
static void settle(void)
{
  per_frame[A] = per_frame[B] = 0;
  run_k(A, 30);
  run_k(B, 30);
}

/* 11. An outage at D = 4 (online), the gate waiting it out: when the backlog
 *     lands in one piece, every packet still reaches the other game on
 *     schedule, none is dropped, and the receive queues stay shallow -- the
 *     lockstep bounds the backlog to D + 1 of the partner's frames. */
static void test_outage_no_burst(void)
{
  int k;
  cfg_d = 4;
  CHECK(linked(), "no lock");
  CHECK(run(700), "deadlock");
  per_frame[A] = per_frame[B] = 2;          /* a busy link: 2 packets a frame */
  CHECK(run(100), "deadlock");
  outage(600, -1, 0);                       /* ~10 s of passes: the gate waits */
  CHECK(run(300), "deadlock after the outage");
  for (k = 0; k < 2; k++)
    CHECK(bad[k] + shifted[k] + skipped[k] == 0,
          "%s: %u off schedule, %u late, %u missing", k ? "B" : "A", bad[k], shifted[k],
          skipped[k]);
  settle();
  for (k = 0; k < 2; k++) {
    CHECK(got_all[k] == sent_all[!k], "%s got %u of %u", k ? "B" : "A", got_all[k],
          sent_all[!k]);
    CHECK(ad[k].qhi() <= 2 * 2, "%s receive queue reached %u", k ? "B" : "A",
          ad[k].qhi());
  }
  printf("  outage at D=4, gate waits: %u/%u packets delivered, none lost, "
         "queues peaked %u/%u\n", got_all[A] + got_all[B], sent_all[A] + sent_all[B],
         ad[A].qhi(), ad[B].qhi());
  cfg_d = 2;
}

/* 12. The mechanism test 11 avoids (Lab j7c1b2e): the same outage, but one
 *     console gives up and runs free for ~5 s.  When the pair relocks, the
 *     free frames' data is overdue, the other game gets it in one frame, and
 *     its receive queue (RFU_PKT_QUEUE, 64) overflows: packets are lost. */
static void test_give_up_lead_overflows(void)
{
  cfg_d = 4;
  CHECK(linked(), "no lock");
  CHECK(run(700), "deadlock");
  outage(50, B, 300);
  run_k(A, 300);
  settle();
  CHECK(ad[A].qhi() >= 64 && got_all[A] < sent_all[B],
        "the lead did not overflow (queue %u, got %u of %u)", ad[A].qhi(), got_all[A],
        sent_all[B]);
  printf("  give-up during an outage (the old rule): host queue %u, %u of %u "
         "packets lost\n", ad[A].qhi(), sent_all[B] - got_all[A], sent_all[B]);
  cfg_d = 2;
}

/* 13. "Waiting for the other player..." (fe_linkwait.h) on the gate's real
 *     holds.  gate_pass is fe_host_rfu_gate's decision for console k at
 *     `now`: run when the tick is in; else the wait rule (rfu_gate_step,
 *     with netdrv's verdict `held`); a pass that runs no frame holds, one
 *     that runs ends the hold.  The partner B freezes (its process stopped:
 *     nothing it sends) and netdrv reads the path as held once nothing has
 *     been heard for RFU_GATE_QUIET_US, as netdrv_link_held does.
 *     a. a hitch shorter than the notice shows nothing;
 *     b. a 3 s freeze: shown from 1 s (within a pass), the whole hold, no
 *        give-up; hidden by the first frame after the partner is back;
 *     c. a partner declared dead at 8 s (netdrv: the link is lost): the
 *        notice goes on the very next pass and the console runs on its own
 *        (rfu_sched_may_run), never by a give-up -- what follows is the
 *        session's disconnect flow, untouched;
 *     d. a partner whose menu is open (PAUSE) is waited for: said too. */
#define PASS_US 16683ull
static int lw_gave;
static int gate_pass(int k, unsigned long long now, int held,
                     unsigned long long *gs, unsigned long long *lw)
{
  int ran = 0;
  if (ad[k].may_run()) {
    *gs = 0;
    ran = pass(k);
  } else if (rfu_gate_step(gs, now, ad[k].peer_paused(), held)) {
    ad[k].give_up();
    lw_gave++;
    *gs = 0;
    ran = pass(k);
  }
  link_wait_note(lw, now, !ran);
  return ran;
}

/* Run A (and B unless frozen) for `us` of passes from *t.  Returns the
 * notice's first time on (relative to *t at entry; -1 = never) and leaves
 * in *on_end whether it is on at the last pass. */
static long long wait_run(unsigned long long *t, unsigned long long us,
                          unsigned long long frozen_at, unsigned long long *gs,
                          unsigned long long *lw, int *on_end)
{
  unsigned long long t0 = *t, gsb = 0, lwb = 0;
  long long first = -1;
  int on = 0;
  for (; *t < t0 + us; *t += PASS_US) {
    /* frozen_at 0: netdrv never reads the path as held */
    int held = frozen[B] && frozen_at && *t >= frozen_at + RFU_GATE_QUIET_US
               ? ND_HELD_SILENT_T : 0;
    if (!frozen[B])
      gate_pass(B, *t, 0, &gsb, &lwb);
    gate_pass(A, *t, held, gs, lw);
    pump();
    on = link_wait_due(*lw, *t);
    if (on && first < 0)
      first = (long long)(*t - t0);
  }
  *on_end = on;
  return first;
}

static void test_wait_notice(void)
{
  unsigned long long t = 50000000ull, gs = 0, lw = 0, fz;
  long long first, shown_b;
  int on;
  CHECK(LINK_WAIT_NOTICE_US == 1000000u, "the notice is owed after 1 s (owner), "
        "not %u us", LINK_WAIT_NOTICE_US);
  CHECK(!link_wait_due(0, 99000000ull) &&
        link_wait_due(1, LINK_WAIT_NOTICE_US + 1ull) &&
        !link_wait_due(1, LINK_WAIT_NOTICE_US), "link_wait_due edges");
  lw = 0;
  link_wait_note(&lw, 0, 1);
  CHECK(lw == 1, "a hold starting at time 0 not recorded");
  lw = 0;
  lw_gave = 0;
  CHECK(linked(), "no lock");
  CHECK(run(100), "deadlock");
  /* a. a 0.5 s hitch */
  CHECK(wait_run(&t, 200000, 0, &gs, &lw, &on) < 0 && !on, "notice on a clean link");
  frozen[B] = 1;
  fz = t;
  first = wait_run(&t, 500000, fz, &gs, &lw, &on);
  CHECK(first < 0, "a 0.5 s hitch showed the notice (at %lld us)", first);
  CHECK(lw != 0, "the 0.5 s hitch was not a hold");
  frozen[B] = 0;
  first = wait_run(&t, 300000, 0, &gs, &lw, &on);
  CHECK(first < 0 && !on && lw == 0, "after the hitch: notice %lld, hold %llu", first, lw);
  /* b. a 3 s freeze */
  frozen[B] = 1;
  fz = t;
  first = wait_run(&t, 3000000, fz, &gs, &lw, &on);
  shown_b = first;
  CHECK(first >= (long long)LINK_WAIT_NOTICE_US &&
        first <= (long long)(LINK_WAIT_NOTICE_US + 4 * PASS_US),
        "a 3 s freeze showed the notice at %lld us (want ~1 s)", first);
  CHECK(on && !lw_gave, "3 s freeze: on at the end %d, give-ups %d", on, lw_gave);
  frozen[B] = 0;
  {
    /* hidden by the first frame A runs once B is back, and shown until then */
    unsigned long long gsb = 0, lwb = 0;
    int i, ran = 0;
    for (i = 0; i < 20 && !ran; i++, t += PASS_US) {
      gate_pass(B, t, 0, &gsb, &lwb);
      pump();
      ran = gate_pass(A, t, 0, &gs, &lw);
      CHECK(ran ? !link_wait_due(lw, t) : link_wait_due(lw, t),
            "resume pass %d: ran %d, notice %d", i, ran, link_wait_due(lw, t));
    }
    CHECK(ran && i <= 3, "A ran again only after %d passes", i);
  }
  first = wait_run(&t, 200000, 0, &gs, &lw, &on);
  CHECK(!on && lw == 0 && first < 0, "after the freeze: on %d, hold %llu", on, lw);
  CHECK(!lw_gave, "gave up on a held partner");
  /* d. the partner's menu: PAUSE, then no frames */
  ad[B].pause(1);                       /* sent at once (rfu_sched_pause) */
  frozen[B] = 1;
  /* netdrv's verdict stays clear: PAUSE alone keeps the gate waiting */
  first = wait_run(&t, 2500000, 0, &gs, &lw, &on);   /* past the 2 s rule too */
  CHECK(first >= (long long)LINK_WAIT_NOTICE_US &&
        first <= (long long)(LINK_WAIT_NOTICE_US + 4 * PASS_US) && on && !lw_gave,
        "partner menu: notice %lld, on %d, give-ups %d", first, on, lw_gave);
  frozen[B] = 0;
  ad[B].pause(0);
  first = wait_run(&t, 300000, 0, &gs, &lw, &on);
  CHECK(!on && lw == 0, "after the partner's menu: on %d", on);
  /* c. dead at 8 s */
  frozen[B] = 1;
  fz = t;
  first = wait_run(&t, 8000000, fz, &gs, &lw, &on);
  CHECK(on && !lw_gave, "before the death: on %d, give-ups %d", on, lw_gave);
  ad[A].unlink();                       /* netdrv: peer dead -> link lost */
  /* the gate's next pass (the frame itself is the unlinked game's, which
   * this wire's games do not model: test 7 stops here too) */
  on = ad[A].may_run();
  link_wait_note(&lw, t, !on);
  CHECK(on && lw == 0 && !link_wait_due(lw, t) && !lw_gave && !ad[A].on(),
        "after the death: ran %d, hold %llu, give-ups %d, on %d", on, lw, lw_gave,
        ad[A].on());
  printf("  wait notice: none for a 0.5 s hitch; a 3 s hold shows it from %lld ms "
         "of freeze to the first frame back; a partner menu shows it; a dead "
         "partner clears it on the next pass, no give-up\n", shown_b / 1000);
}

int main(int argc, char **argv)
{
  if (argc > 1)
    mutant = argv[1];
  rs_self = A;
  rsb_rs_self = B;
  test_sync_offset();
  test_delivery();
  test_mark_and_tick();
  test_ni();
  test_pause();
  test_give_up_relock();
  test_link_lost();
  test_d_for_kind();
  test_ni_window_d4();
  test_gate_step();
  test_outage_no_burst();
  test_give_up_lead_overflows();
  test_wait_notice();
  if (failures) {
    printf("rfu sched%s%s: %d check(s) FAILED\n", *mutant ? " mutant " : "", mutant,
           failures);
    return 1;
  }
  printf("rfu sched: all tests passed\n");
  return 0;
}
#endif
