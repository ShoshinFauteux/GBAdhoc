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

#else
/* ----------------------------------------------------------------- driver */
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
} adapter;

#define ADAPTER(p) { p##rs_fresh, p##rs_game_send, p##rs_game_recv, \
  p##rs_frame_end, p##rs_receive, p##rs_unlink, p##rs_frame, p##rs_off, \
  p##rs_cur_d, p##rs_on, p##rs_relocks, p##rs_config, p##rs_may_run, \
  p##rs_pause, p##rs_peer_paused, p##rs_give_up }
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

static void pump(void)
{
  int from;
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
 * force), later than D = 2, or out of stamp order; `shifted` = after the
 * stamp + off + D in force, which only a D that just dropped may cause;
 * `seen_d` = exactly at stamp + off + D, by D; `skipped` = a stamp missing
 * from the sequence (a loss, when the sender sends every frame). */
static unsigned got[2], bad[2], shifted[2], seen_d[2][3], skipped[2];
static int32_t last_stamp[2];

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
    if (a->on()) {
      int32_t d = a->cur_d();
      int32_t delay = (int32_t)(((uint32_t)(f - a->off()) - (uint32_t)t) & 0xFF);
      int32_t stamp = f - a->off() - delay;
      if (delay < d || delay > 2 || stamp < last_stamp[k])
        bad[k]++;
      else if (delay > d)
        shifted[k]++;
      else
        seen_d[k][d]++;
      if (last_stamp[k] >= 0 && stamp > last_stamp[k] + 1)
        skipped[k]++;
      last_stamp[k] = stamp;
    }
  }
  if (a->on())
    for (i = 0; i < per_frame[k]; i++) {
      a->game_send((unsigned)(f & 0xFF), send_ni[k]);
      send_ni[k] = 0;
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

/* Fresh adapters, D on both, linked: A at host frame 1000, B at 37. */
static void start(void)
{
  memset(wq_head, 0, sizeof(wq_head));
  memset(wq_tail, 0, sizeof(wq_tail));
  memset(n_log, 0, sizeof(n_log));
  memset(frozen, 0, sizeof(frozen));
  memset(send_ni, 0, sizeof(send_ni));
  per_frame[A] = per_frame[B] = 1;
  reset_counts();
  ad[A].fresh(1, 1, 1000);
  ad[B].fresh(0, 0, 37);
  ad[A].config(2);
  ad[B].config(!strcmp(mutant, "d0") ? 0 : 2);
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
  if (failures) {
    printf("rfu sched%s%s: %d check(s) FAILED\n", *mutant ? " mutant " : "", mutant,
           failures);
    return 1;
  }
  printf("rfu sched: all tests passed\n");
  return 0;
}
#endif
