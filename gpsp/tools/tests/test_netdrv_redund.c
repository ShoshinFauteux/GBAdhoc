/* test_netdrv_redund.c -- redundant DATA (ND_T_DATAR, nd_config.redund):
 * each first transmission carries the payloads sent just before it that are
 * still unacknowledged, so ONE lost datagram costs the receiver one frame
 * instead of a retransmission timeout.  The retransmission stays as the
 * fallback.
 *
 * Two netdrv instances over a scripted wire: one-way latency LAT frames, a
 * virtual clock of 16.7 ms frames, and each side sends one RFU-sized
 * (104 B) reliable payload a frame, as a linked adapter does.  Datagrams can
 * be dropped by index or at random, or swapped.  The receiver records the
 * frame each payload reached it in; lag = that - the frame it was sent in.
 *
 * Covered: redund 0 sends only DATA frames (the wire is unchanged) and a lost
 * datagram waits an RTO; redund 2: a single loss and two in a row are filled
 * from the next datagram (lag LAT + 1, no retransmission), three in a row
 * fall back to the retransmission, a swap delivers in order and counts no
 * duplicates, every datagram stays <= ND_MAX_DGRAM with the extra bytes
 * bounded, a DATAR whose inner lengths do not add up is dropped whole, and
 * 3 % random loss: how many payloads came late, with and without.  The
 * cable's cap (nd_config.red_body_max = ND_MAX_PAYLOAD): a DATAR never
 * outgrows a plain frame, small payloads still carry copies, big ones go
 * plain.  netdrv_rx_how names each delivery: in order, a redundant copy, or
 * a gap a retransmission filled.
 * Build it at the desktop sizes and the PSP's (psp/fe_features.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netdrv.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
  printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); \
  printf("\n"); } } while (0)

#define LAT      3            /* one-way latency, frames (~50 ms) */
#define FRAME_US 16743u
#define NPAY     4000
#define WQ       512

typedef struct { int due; uint16_t len; uint8_t d[ND_MAX_DGRAM]; } dgram;
static struct {
  dgram q[WQ];
  unsigned head, tail;
  unsigned n_data;            /* DATA/DATAR datagrams sent, for drop-by-index */
} wire[2];                    /* [0] = A -> B, [1] = B -> A */

static uint64_t now_us;
static int frame;
/* drop the FIRST A->B datagram of payloads [lo, hi] (retransmissions pass) */
static unsigned drop_lo = 1, drop_hi = 0;
static unsigned loss_pct;                     /* random loss, both ways */
static unsigned swap_at = 0;                  /* payload swap_at + 1 overtakes */
static uint8_t sent_once[2][NPAY];
static uint32_t prng = 12345;
static unsigned max_dgram, n_datar, n_data_plain, extra_max;
static unsigned plen = 104;                   /* payload bytes (>= 2) */
static unsigned red_cap;                      /* nd_config.red_body_max */

static unsigned rnd(void)
{
  prng ^= prng << 13; prng ^= prng >> 17; prng ^= prng << 5;
  return prng;
}

/* ---- the transport ---------------------------------------------------- */
typedef struct { int side; } ep;
static ep eps[2] = { { 0 }, { 1 } };
static const uint8_t mac_of[2][6] = { { 2, 0, 0, 0, 0, 1 }, { 2, 0, 0, 0, 0, 2 } };

static int t_send(void *ctx, const uint8_t mac[6], const void *buf, size_t len)
{
  int dir = ((ep *)ctx)->side;
  const uint8_t *b = (const uint8_t *)buf;
  dgram *g;
  (void)mac;
  if (len > max_dgram)
    max_dgram = (unsigned)len;
  if (len >= ND_HDR_SIZE + 4 && (b[5] == ND_T_DATA || b[5] == ND_T_DATAR)) {
    /* the payload index of the primary (the app's first two bytes), and
     * whether this is its first transmission */
    const uint8_t *pp = b + ND_HDR_SIZE + (b[5] == ND_T_DATAR ? 2 : 0);
    unsigned k = (unsigned)(pp[0] | pp[1] << 8), first = 0;
    wire[dir].n_data++;
    if (k < NPAY && !sent_once[dir][k]) {
      sent_once[dir][k] = 1;
      first = 1;
    }
    if (b[5] == ND_T_DATAR) {
      unsigned plen = (unsigned)(b[ND_HDR_SIZE] | b[ND_HDR_SIZE + 1] << 8);
      unsigned extra = (unsigned)len - (ND_HDR_SIZE + plen);
      n_datar++;
      if (extra > extra_max)
        extra_max = extra;
    } else
      n_data_plain++;
    if (dir == 0 && first && k >= drop_lo && k <= drop_hi)
      return 0;
    if (loss_pct && rnd() % 100 < loss_pct)
      return 0;
    if (dir == 0 && first && swap_at && k == swap_at + 1 &&
        wire[0].tail != wire[0].head) {
      /* k arrives first, what was queued before it after it */
      dgram tmp = wire[0].q[(wire[0].tail - 1) % WQ];
      g = &wire[0].q[(wire[0].tail - 1) % WQ];
      g->due = frame + LAT;
      g->len = (uint16_t)len;
      memcpy(g->d, buf, len);
      g = &wire[0].q[wire[0].tail++ % WQ];
      *g = tmp;
      g->due = frame + LAT;
      return 0;
    }
  }
  if (wire[dir].tail - wire[dir].head >= WQ) {
    printf("wire full\n");
    exit(1);
  }
  g = &wire[dir].q[wire[dir].tail++ % WQ];
  g->due = frame + LAT;
  g->len = (uint16_t)len;
  memcpy(g->d, buf, len);
  return 0;
}
static int t_bcast(void *ctx, const void *buf, size_t len)
{
  return t_send(ctx, NULL, buf, len);
}
static int t_recv(void *ctx, uint8_t src[6], void *buf, size_t cap)
{
  int dir = !((ep *)ctx)->side;       /* what the other side sent */
  dgram *g;
  if (wire[dir].head == wire[dir].tail)
    return 0;
  g = &wire[dir].q[wire[dir].head % WQ];
  if (g->due > frame)
    return 0;
  wire[dir].head++;
  if (g->len > cap)
    return 0;
  memcpy(buf, g->d, g->len);
  memcpy(src, mac_of[dir], 6);
  return g->len;
}
static void t_addr(void *ctx, uint8_t mac[6])
{
  memcpy(mac, mac_of[((ep *)ctx)->side], 6);
}

/* ---- the apps ------------------------------------------------------------ */
static int recv_frame[2][NPAY];       /* [receiver][payload] */
static int recv_how[2][NPAY];         /* netdrv_rx_how at delivery */
static unsigned next_k[2], order_bad[2], up[2];
static netdrv *nd[2];
static void deliver(void *user, const void *buf, size_t len, uint8_t src)
{
  int side = (int)(intptr_t)user;
  const uint8_t *b = (const uint8_t *)buf;
  unsigned k = (unsigned)(b[0] | b[1] << 8);
  size_t i;
  (void)src;
  if (len != plen || k != next_k[side])
    order_bad[side]++;
  for (i = 2; i < len; i++)
    if (b[i] != (uint8_t)(k * 7 + i))
      order_bad[side]++;
  next_k[side] = k + 1;
  if (k < NPAY) {
    recv_frame[side][k] = frame;
    recv_how[side][k] = netdrv_rx_how(nd[side]);
  }
}
static void started(void *user, uint8_t id) { (void)id; up[(int)(intptr_t)user] = 1; }

static int sent_frame[NPAY];

static void make(unsigned redund)
{
  int s;
  memset(wire, 0, sizeof(wire));
  memset(recv_frame, 0xFF, sizeof(recv_frame));
  memset(next_k, 0, sizeof(next_k));
  memset(order_bad, 0, sizeof(order_bad));
  memset(up, 0, sizeof(up));
  memset(sent_once, 0, sizeof(sent_once));
  max_dgram = n_datar = n_data_plain = extra_max = 0;
  drop_lo = 1; drop_hi = 0; loss_pct = 0; swap_at = 0;
  now_us = 1000000; frame = 0;
  for (s = 0; s < 2; s++) {
    nd_transport tp;
    nd_callbacks cb;
    nd_config cfg;
    memset(&tp, 0, sizeof(tp));
    tp.ctx = &eps[s];
    tp.send_to = t_send;
    tp.broadcast = t_bcast;
    tp.recv = t_recv;
    tp.local_addr = t_addr;
    memset(&cb, 0, sizeof(cb));
    cb.user = (void *)(intptr_t)s;
    cb.deliver = deliver;
    cb.session_started = started;
    memset(&cfg, 0, sizeof(cfg));
    cfg.protocol = "gpSP v1.0";
    cfg.join_nonce = 0x100u + (unsigned)s;
    cfg.redund = (uint8_t)redund;
    cfg.red_body_max = (uint16_t)red_cap;
    nd[s] = netdrv_create(&tp, &cb, &cfg);
  }
  netdrv_host(nd[0]);
  netdrv_join(nd[1]);
}

static void step(void)
{
  frame++;
  now_us += FRAME_US;
  netdrv_pump(nd[0], now_us);
  netdrv_pump(nd[1], now_us);
}

/* Link up, then `n` frames of one payload a frame each way. */
static void play(int n)
{
  int i, f;
  uint8_t p[560];
  for (i = 0; i < 400 && !(up[0] && up[1] && netdrv_peer_count(nd[0])); i++)
    step();
  CHECK(up[0] && up[1], "no session");
  for (f = 0; f < n && f < NPAY; f++) {
    int s;
    p[0] = (uint8_t)f;
    p[1] = (uint8_t)(f >> 8);
    for (i = 2; i < (int)plen; i++)
      p[i] = (uint8_t)(f * 7 + i);
    sent_frame[f] = frame;
    for (s = 0; s < 2; s++)
      netdrv_send(nd[s], ND_RELIABLE | ND_FLUSH_HINT, p, plen, (uint16_t)!s);
    step();
  }
  for (i = 0; i < 120; i++)
    step();
}

static int lag(int k) { return recv_frame[1][k] - sent_frame[k]; }
static void done(void) { netdrv_destroy(nd[0]); netdrv_destroy(nd[1]); }

/* The worst lag over payloads [lo, hi], and over all others. */
static void lags(int n, int lo, int hi, int *in, int *out)
{
  int k;
  *in = *out = 0;
  for (k = 0; k < n; k++) {
    int *w = k >= lo && k <= hi ? in : out;
    if (lag(k) > *w)
      *w = lag(k);
  }
}

static void test_off_unchanged(void)
{
  int in, out;
  make(0);
  drop_lo = drop_hi = 60;
  play(200);
  lags(200, 60, 60, &in, &out);
  CHECK(n_datar == 0, "redund 0 sent %u DATAR", n_datar);
  CHECK(max_dgram <= ND_MAX_FRAME, "redund 0 datagram %u > ND_MAX_FRAME", max_dgram);
  CHECK(next_k[1] == 200 && order_bad[1] == 0, "B got %u, %u bad", next_k[1], order_bad[1]);
  CHECK(in > LAT + 2, "a loss without redundancy cost only %d frames", in);
  /* the retransmission fills a gap the later payloads already crossed */
  CHECK(recv_how[1][60] == ND_RX_GAP && recv_how[1][61] == ND_RX_GAP,
        "redund 0: lost payload's how %d, its successor's %d", recv_how[1][60],
        recv_how[1][61]);
  CHECK(recv_how[1][59] == ND_RX_INORDER && recv_how[1][150] == ND_RX_INORDER,
        "redund 0: in-order hows %d %d", recv_how[1][59], recv_how[1][150]);
  printf("  redund 0: DATA only; a lost datagram waits for the retransmission "
         "(lag %d frames, %d without loss)\n", in, out);
  done();
}

/* `n` first copies lost in a row: with redund 2, 1 and 2 cost LAT + n
 * frames (filled from the next datagram, no retransmission needed), 3 falls
 * back to the retransmission. */
static void test_losses(void)
{
  static const int ns[3] = { 1, 2, 3 };
  int r;
  for (r = 0; r < 3; r++) {
    nd_stats sa, sb;
    int in, out, n = ns[r];
    make(2);
    drop_lo = 60;
    drop_hi = (unsigned)(60 + n - 1);
    play(400);
    netdrv_get_stats(nd[0], &sa);
    netdrv_get_stats(nd[1], &sb);
    lags(400, 60, 60 + n - 1, &in, &out);
    CHECK(next_k[1] == 400 && order_bad[1] == 0, "%d lost: B got %u, %u bad", n,
          next_k[1], order_bad[1]);
    if (n <= ND_RED_MAX) {
      CHECK(in <= LAT + n, "%d lost: worst lag %d > %d", n, in, LAT + n);
      CHECK(sb.red_fill >= (unsigned)n, "%d lost: %u filled", n, sb.red_fill);
      CHECK(recv_how[1][60] == ND_RX_RED && recv_how[1][60 + n - 1] == ND_RX_RED,
            "%d lost: how %d / %d, a redundant copy expected", n, recv_how[1][60],
            recv_how[1][60 + n - 1]);
      CHECK(recv_how[1][59] == ND_RX_INORDER && recv_how[1][60 + n] == ND_RX_INORDER,
            "%d lost: neighbours' how %d / %d", n, recv_how[1][59],
            recv_how[1][60 + n]);
    } else
      CHECK(in > LAT + n, "%d lost: lag %d, the fallback should have been needed", n, in);
    /* a copy B already had is red_dup, never rx_dup: rx_dup only counts
     * what A really retransmitted */
    CHECK(sb.rx_dup <= sa.retx, "%d lost: rx_dup %u > A's retx %u", n, sb.rx_dup, sa.retx);
    CHECK(sb.red_dup > 0, "no copy was redundant");
    printf("  redund 2, %d in a row lost: worst lag %d frames (others %d), filled %u\n",
           n, in, out, sb.red_fill);
    done();
  }
}

static void test_swap_and_bound(void)
{
  int in, out;
  make(2);
  swap_at = 80;
  play(300);
  lags(300, 80, 81, &in, &out);
  CHECK(next_k[1] == 300 && order_bad[1] == 0, "swap: got %u, bad %u", next_k[1],
        order_bad[1]);
  CHECK(in <= LAT + 1, "swap: lag %d", in);
  CHECK(max_dgram <= ND_MAX_DGRAM, "datagram %u > ND_MAX_DGRAM %u", max_dgram,
        (unsigned)ND_MAX_DGRAM);
  CHECK(extra_max <= 2 + ND_RED_MAX * (4 + 104),
        "extra bytes %u > 2 + %u x (4 + 104)", extra_max, (unsigned)ND_RED_MAX);
  CHECK(n_datar > 0, "no DATAR sent");
  printf("  redund 2: a swap delivers in order; largest datagram %u B (ND_MAX_DGRAM %u), "
         "extra <= %u B per datagram\n", max_dgram, (unsigned)ND_MAX_DGRAM, extra_max);
  done();
}

static void test_malformed_bundle(void)
{
  uint8_t pl[64], buf[ND_MAX_DGRAM];
  nd_hdr h, hp;
  const uint8_t *p;
  int why;
  size_t n;
  memset(pl, 0, sizeof(pl));
  memset(&h, 0, sizeof(h));
  h.type = ND_T_DATAR;
  /* primary 10 B, then one entry: seq 5, 4 B */
  pl[0] = 10;
  pl[12] = 5;
  pl[14] = 4;
  h.len = 2 + 10 + 4 + 4;
  n = nd_wire_build(buf, &h, pl);
  CHECK(nd_wire_parse(buf, n, &hp, &p, &why) == 0, "a DATAR frame does not parse");
  CHECK(nd_red_check(p, hp.len) == 1, "a well-formed bundle reads %d entries",
        nd_red_check(p, hp.len));
  pl[14] = 200;                       /* the entry claims more than is there */
  CHECK(nd_red_check(pl, h.len) < 0, "an overrunning entry accepted");
  pl[14] = 3;                         /* ... or less: a stray byte at the end */
  CHECK(nd_red_check(pl, h.len) < 0, "a short entry accepted");
  pl[14] = 4;
  pl[0] = 30;                         /* the primary overruns */
  CHECK(nd_red_check(pl, h.len) < 0, "an overrunning primary accepted");
  pl[0] = 10;
  CHECK(nd_red_check(pl, 1) < 0, "a 1-byte body accepted");
  printf("  a DATAR whose lengths do not add up is refused whole\n");
}

static void test_random_loss(void)
{
  unsigned r, late[2], total = 2500;
  nd_stats st[2];
  for (r = 0; r < 2; r++) {
    int k;
    make(r ? 2 : 0);
    prng = 777;
    loss_pct = 3;
    play((int)total);
    netdrv_get_stats(nd[0], &st[r]);
    late[r] = 0;
    for (k = 0; k < (int)total; k++)
      if (lag(k) > LAT + 1)
        late[r]++;
    CHECK(next_k[1] == total && order_bad[1] == 0, "r%u got %u, bad %u", r, next_k[1],
          order_bad[1]);
    done();
  }
  CHECK(late[1] * 10 < late[0] + 1, "redundancy left %u of %u late (without: %u)",
        late[1], total, late[0]);
  printf("  3 %% loss, %u payloads: %u late without redundancy (retx %u), %u with "
         "(retx %u)\n", total, late[0], st[0].retx, late[1], st[1].retx);
}

/* The cable's cap: copies only while the datagram fits a plain frame (the
 * ad hoc transport drops anything bigger on arrival).  A button-sized
 * payload still carries two copies and a loss is filled one frame later; a
 * payload too big to carry any goes as plain DATA. */
static void test_cable_cap(void)
{
  int in, out;
  nd_stats sb;
  plen = 8;
  red_cap = ND_MAX_PAYLOAD;
  make(2);
  drop_lo = drop_hi = 60;
  play(300);
  netdrv_get_stats(nd[1], &sb);
  lags(300, 60, 60, &in, &out);
  CHECK(next_k[1] == 300 && order_bad[1] == 0, "cap: B got %u, %u bad", next_k[1],
        order_bad[1]);
  CHECK(max_dgram <= ND_MAX_FRAME, "cap: datagram %u > ND_MAX_FRAME %u", max_dgram,
        (unsigned)ND_MAX_FRAME);
  CHECK(n_datar > 0 && in <= LAT + 1 && sb.red_fill >= 1 &&
        recv_how[1][60] == ND_RX_RED,
        "cap, 8 B: %u DATAR, lag %d, filled %u, how %d", n_datar, in, sb.red_fill,
        recv_how[1][60]);
  printf("  cable cap, 8 B payloads: %u DATAR, largest datagram %u <= %u, a loss "
         "costs %d frames\n", n_datar, max_dgram, (unsigned)ND_MAX_FRAME, in);
  done();
  plen = ND_MAX_PAYLOAD;
  make(2);
  play(100);
  CHECK(max_dgram <= ND_MAX_FRAME && n_datar == 0,
        "cap, full payloads: datagram %u, %u DATAR", max_dgram, n_datar);
  done();
  plen = 104;
  red_cap = 0;
}

int main(void)
{
  printf("[netdrv redundant DATA, ND_MAX_PAYLOAD %u, ND_WINDOW %u]\n",
         (unsigned)ND_MAX_PAYLOAD, (unsigned)ND_WINDOW);
  test_off_unchanged();
  test_losses();
  test_swap_and_bound();
  test_malformed_bundle();
  test_random_loss();
  test_cable_cap();
  if (fails) {
    printf("netdrv redund: %d check(s) FAILED\n", fails);
    return 1;
  }
  printf("netdrv redund: all tests passed\n");
  return 0;
}
