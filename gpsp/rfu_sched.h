/* rfu_sched.h -- scheduled ticks: couple two consoles over the emulated
 * adapter (docs/RFU-COUPLING.md).  Included once by rfu.c in -DRFU_SCHED builds
 * (tools/build.sh RFU_SCHED=1, off by default), off unless rfu_sched_d is set.
 * The frontend side is rfu_sched_api.h; netdrv refuses a partner whose build
 * differs (ND_FEAT_RFU_SCHED), since the two wire formats cannot mix.
 *
 * One tick is one emulated frame.  Whatever the game sends during frame F is
 * stamped F, and a TICK mark closes F.  The partner's game gets that data at
 * the start of its frame F + off + D, and that frame may not start until the
 * TICK has arrived: the console waits for a packet that is really missing,
 * never on an estimate.  netdrv's reliable lane delivers in order, so holding
 * TICK F means holding all of F's data.
 *
 * off = my frame - partner's frame, set once when the link forms: the client
 * sends SYNC with its frame, the host answers with off, and the client takes
 * -off.  Two consoles cannot wait on each other for D >= 1.
 *
 * A datagram costs the radio several ms on a PSP, so a tick must not cost one of
 * its own.  The last data packet of a frame is held to the end of the frame and
 * carries the mark inside it, in a byte the payload cannot reach (host packets
 * end at 90 bytes, client packets at 16).  Delivery is scheduled anyway, so the
 * short hold changes nothing the game can see.  A frame with no data still sends
 * a bare TICK.
 *
 * A receiver may change its own D at any time: a packet's due frame is worked
 * out when it is released, order is kept, and D >= 1 still cannot deadlock.  It
 * drops to 1 while either side is in a librfu NI exchange.  NI is the
 * acknowledged lane games use for handshakes, and those carry races as wide as
 * the round trip: in Mario Golf's lobby a client press that lands after the
 * parent has moved on to UNI repeats forever, because the game has stopped
 * clearing its NI receive slot.  Real adapters make that window 1-2 frames.
 *
 * A console that opens its menu says so (PAUSE), and its partner waits for as
 * long as that lasts, as a cable session does.  Silence with no PAUSE ends the
 * wait after the frontend's limit; ticks arriving later lock the pair again:
 * whichever side gave up asks, and the client sends a fresh SYNC.
 *
 * Two players.  A third console's traffic passes through unscheduled. */
#define NET_RFU_SCHED_SYNC  0x09    /* client -> host: my frame */
#define NET_RFU_SCHED_ACK   0x0A    /* host -> client: host frame - client frame */
#define NET_RFU_SCHED_TICK  0x0B    /* this frame's traffic is complete */
#define NET_RFU_SCHED_PAUSE 0x0C    /* 1 = my menu is open, 0 = closed */
#define NET_RFU_SCHED_AGAIN 0x0D    /* host -> client: send SYNC again */

#define RFU_SCHED_RING 128u
#define RFU_SCHED_NI_FRAMES 120     /* stay at D = 1 this long after NI traffic */

void rfu_net_receive(const void *buf, size_t len, uint16_t client_id);

unsigned rfu_sched_d CORE_SBSS;        /* D, 0 = off */
unsigned rfu_sched_held_peak CORE_SBSS;
static struct {
  s32 stamp;                        /* the partner's frame */
  u32 len, cid;
  u8  data[116];
} rfu_sched_ring[RFU_SCHED_RING] CORE_SBSS;
static u32 rfu_sched_head CORE_SBSS, rfu_sched_tail CORE_SBSS;
static u32 rfu_sched_replay CORE_SBSS;
static u32 rfu_sched_linked CORE_SBSS, rfu_sched_on CORE_SBSS,
           rfu_sched_gave_up CORE_SBSS;
static u32 rfu_sched_cid CORE_SBSS, rfu_sched_cfr CORE_SBSS;
static s32 rfu_sched_off CORE_SBSS, rfu_sched_peer_start CORE_SBSS,
           rfu_sched_peer_done CORE_SBSS;
static u32 rfu_sched_peer_ok CORE_SBSS;
static u32 rfu_sched_peer_paused CORE_SBSS, rfu_sched_paused CORE_SBSS;
unsigned rfu_sched_relock_n CORE_SBSS;
unsigned rfu_sched_no_poll_on CORE_SDATA = 1;
unsigned rfu_sched_ni_on CORE_SDATA = 1;
static s32 rfu_sched_ni_at CORE_SDATA = -100000;
static struct {
  u32 valid, ptype, h, plen, cid;
  u32 data[24];
} rfu_sched_pend CORE_SBSS;

/* Turn ticks on (d >= 1) or off (0).  Ticks replace the older delivery rules:
 * frame pace with its hold bound, shedding, the overflow hold and supersede.
 * Each console reads one tick of its partner's data a frame, so the backlog
 * those rules trim cannot form.  Hardware 2026-11-02 (build e9c45f3cfd76, Lab
 * notes "rfunew"): Tennis, Golf and the Emerald trade gave the same fps,
 * resends, drops and queue depth with all five off as with them on.  Called
 * after the frontend has set the rules, so it has the last word. */
void rfu_sched_config(unsigned d)
{
  rfu_sched_d = d;
  if (!d)
    return;
  rfu_set_frame_pace(0);
  rfu_set_pace_max_hold(0);
  rfu_set_shed_keep(0);
  rfu_set_hold(0);
  rfu_set_supersede(0);
}

/* The D this console is using now. */
static s32 rfu_sched_cur_d(void)
{
  if (rfu_sched_ni_on && (s32)rfu_frame_no - rfu_sched_ni_at < RFU_SCHED_NI_FRAMES)
    return 1;
  return (s32)rfu_sched_d;
}

/* Note a packet's librfu LLSF slot state: NI start, NI, NI end.  Parent frames
 * have a 3-byte header with the state at bit 14, child frames 2 bytes, bit 10. */
static void rfu_sched_see(const u8 *p, u32 blen, int from_host)
{
  u32 st;
  if (blen < 3)
    return;
  st = from_host ? (((u32)p[2] << 16 | (u32)p[1] << 8 | p[0]) >> 14) & 15
                 : (((u32)p[1] << 8 | p[0]) >> 10) & 15;
  if (st >= 1 && st <= 3)
    rfu_sched_ni_at = (s32)rfu_frame_no;
}

static void rfu_sched_flush(int mark)
{
  if (!rfu_sched_pend.valid)
    return;
  rfu_sched_pend.valid = 0;
  rfu_sched_mark = mark;
  rfu_sched_replay = 2;             /* straight onto the wire */
  rfu_net_send_data((int)rfu_sched_pend.cid, rfu_sched_pend.ptype,
                    rfu_sched_pend.h, rfu_sched_pend.data, rfu_sched_pend.plen);
  rfu_sched_replay = 0;
  rfu_sched_mark = 0;
}

/* rfu_net_send_data, on its way out.  1 = held. */
static int rfu_sched_hold_send(int client_id, u32 ptype, u32 h, const u32 *pload,
                               unsigned plen)
{
  if (!rfu_sched_d || rfu_sched_replay == 2 || !rfu_sched_linked ||
      (u32)client_id != rfu_sched_cid)
    return 0;
  rfu_sched_flush(0);
  {
    u32 w = pload[0];
    u8 b[3] = { (u8)w, (u8)(w >> 8), (u8)(w >> 16) };
    rfu_sched_see(b, plen, ptype == NET_RFU_HOST_SEND);
  }
  rfu_sched_pend.valid = 1;
  rfu_sched_pend.ptype = ptype;
  rfu_sched_pend.h = h;
  rfu_sched_pend.plen = plen;
  rfu_sched_pend.cid = (u32)client_id;
  memcpy(rfu_sched_pend.data, pload, (plen + 3) & ~3u);
  return 1;
}

static void rfu_sched_release(s32 frame, int all)
{
  rfu_sched_replay = 1;
  while (rfu_sched_head != rfu_sched_tail) {
    u32 i = rfu_sched_head % RFU_SCHED_RING;
    if (!all && rfu_sched_ring[i].stamp + rfu_sched_off + rfu_sched_cur_d() - frame > 0)
      break;
    rfu_sched_head++;
    rfu_net_receive(rfu_sched_ring[i].data, rfu_sched_ring[i].len,
                    (uint16_t)rfu_sched_ring[i].cid);
  }
  rfu_sched_replay = 0;
}

/* A sign of life after a give-up: lock again. */
static void rfu_sched_relock(void)
{
  if (!rfu_sched_gave_up)
    return;
  rfu_sched_gave_up = 0;
  rfu_sched_relock_n++;
  if (rfu_state == RFU_STATE_CLIENT) {
    rfu_sched_cfr = rfu_frame_no;
    rfu_net_send_cmd(rfu_sched_cid, NET_RFU_SCHED_SYNC, rfu_frame_no);
  } else
    rfu_net_send_cmd(rfu_sched_cid, NET_RFU_SCHED_AGAIN, 0);
}

static void rfu_sched_stop(void)
{
  rfu_sched_flush(0);
  rfu_sched_peer_paused = 0;
  rfu_sched_on = rfu_sched_linked = rfu_sched_gave_up = 0;
  rfu_sched_peer_ok = 0;
  rfu_sched_release(0, 1);
}

/* The partner's data for one packet.  1 = queued for its frame. */
static int rfu_sched_take(const void *buf, size_t len, u32 cid, int stamped,
                          u32 s24)
{
  u32 i;
  const u8 *b = (const u8 *)buf;
  if (rfu_sched_replay || !stamped || cid != rfu_sched_cid)
    return 0;
  /* In order: this mark closes its frame once this packet is queued, below. */
  if (len >= 12 + 92 && b[12 + (rfu_state == RFU_STATE_CLIENT ? RFU_SCHED_MARK_H
                                                              : RFU_SCHED_MARK_C)] ==
      RFU_SCHED_MARK) {
    rfu_sched_peer_done = (s32)s24;
    rfu_sched_peer_ok = 1;
    rfu_sched_relock();
  }
  if (!rfu_sched_on || len > sizeof(rfu_sched_ring[0].data))
    return 0;
  if (len > 12)
    rfu_sched_see(b + 12, (u32)len - 12, rfu_state == RFU_STATE_CLIENT);
  if (rfu_sched_tail - rfu_sched_head >= RFU_SCHED_RING)
    rfu_sched_release(rfu_sched_ring[rfu_sched_head % RFU_SCHED_RING].stamp +
                      rfu_sched_off + rfu_sched_cur_d(), 0);
  i = rfu_sched_tail++ % RFU_SCHED_RING;
  rfu_sched_ring[i].stamp = (s32)s24;
  rfu_sched_ring[i].len = (u32)len;
  rfu_sched_ring[i].cid = cid;
  memcpy(rfu_sched_ring[i].data, buf, len);
  if (rfu_sched_tail - rfu_sched_head > rfu_sched_held_peak)
    rfu_sched_held_peak = rfu_sched_tail - rfu_sched_head;
  return 1;
}

/* A NET_RFU_SCHED_* packet.  The caller has checked the header. */
static void rfu_sched_net(u32 ptype, u32 hdata, u32 cid)
{
  if (!rfu_sched_d)
    return;
  if (ptype == NET_RFU_SCHED_SYNC && rfu_state == RFU_STATE_HOST) {
    rfu_sched_cid = cid;
    rfu_sched_linked = 1;
    rfu_sched_gave_up = 0;
    rfu_sched_peer_paused = 0;
    rfu_sched_off = (s32)rfu_frame_no - (s32)hdata;
    rfu_sched_peer_start = (s32)hdata;
    rfu_sched_peer_ok = 0;
    rfu_sched_on = 1;
    rfu_sched_ni_at = (s32)rfu_frame_no + 480;   /* a new link starts with handshakes */
    rfu_net_send_cmd(cid, NET_RFU_SCHED_ACK, (u32)rfu_sched_off);
  } else if (ptype == NET_RFU_SCHED_ACK && rfu_state == RFU_STATE_CLIENT &&
             rfu_sched_linked && cid == rfu_sched_cid) {
    rfu_sched_off = -(s32)hdata;
    rfu_sched_peer_start = (s32)rfu_sched_cfr + (s32)hdata;
    rfu_sched_on = 1;
    rfu_sched_ni_at = (s32)rfu_frame_no + 480;
  } else if (ptype == NET_RFU_SCHED_TICK && cid == rfu_sched_cid) {
    rfu_sched_peer_done = (s32)hdata;
    rfu_sched_peer_ok = 1;
    rfu_sched_relock();
  } else if (ptype == NET_RFU_SCHED_PAUSE && cid == rfu_sched_cid) {
    rfu_sched_peer_paused = hdata;
  } else if (ptype == NET_RFU_SCHED_AGAIN && cid == rfu_sched_cid &&
             rfu_state == RFU_STATE_CLIENT) {
    rfu_sched_gave_up = 0;
    rfu_sched_cfr = rfu_frame_no;
    rfu_net_send_cmd(cid, NET_RFU_SCHED_SYNC, rfu_frame_no);
  }
}

/* Once a loop: is this console's menu open?  Sent on change. */
void rfu_sched_pause(int on)
{
  on = on ? 1 : 0;
  if (!rfu_sched_on || (u32)on == rfu_sched_paused)
    return;
  rfu_sched_paused = (u32)on;
  rfu_net_send_cmd(rfu_sched_cid, NET_RFU_SCHED_PAUSE, (u32)on);
}

int rfu_sched_peer_is_paused(void)
{
  return (int)rfu_sched_peer_paused;
}

/* Between emulated frames nothing can use a packet early, so the mid-frame
 * polls are wasted work on a linked, scheduled console. */
static int rfu_sched_no_poll(void)
{
  return rfu_sched_on && rfu_sched_no_poll_on;
}

/* End of frame, rfu_frame_no still naming it. */
static void rfu_sched_frame_end(void)
{
  u32 linked = 0, cid = 0, i;
  if (!rfu_sched_d)
    return;
  if (rfu_state == RFU_STATE_CLIENT) {
    linked = 1;
    cid = rfu_client.host_id;
  } else if (rfu_state == RFU_STATE_HOST) {
    for (i = 0; i < 4 && !linked; i++)
      if (rfu_host.clients[i].devid) {
        linked = 1;
        cid = rfu_host.clients[i].client_id;
      }
  }
  if (rfu_sched_linked && (!linked || cid != rfu_sched_cid))
    rfu_sched_stop();
  if (!linked)
    return;
  if (!rfu_sched_linked) {
    rfu_sched_linked = 1;
    rfu_sched_cid = cid;
    if (rfu_state == RFU_STATE_CLIENT) {
      rfu_sched_cfr = rfu_frame_no;
      rfu_net_send_cmd(cid, NET_RFU_SCHED_SYNC, rfu_frame_no);
    }
  }
  if (rfu_sched_pend.valid)
    rfu_sched_flush(1);
  else
    rfu_net_send_cmd(cid, NET_RFU_SCHED_TICK, rfu_frame_no);
}

/* Is the link the ticks were locked on still there?  A frontend that waits
 * runs no frames, so rfu_sched_frame_end cannot notice a lost partner. */
static int rfu_sched_still_linked(void)
{
  u32 i;
  if (rfu_state == RFU_STATE_CLIENT)
    return rfu_client.host_id == rfu_sched_cid;
  if (rfu_state == RFU_STATE_HOST)
    for (i = 0; i < 4; i++)
      if (rfu_host.clients[i].devid && rfu_host.clients[i].client_id == rfu_sched_cid)
        return 1;
  return 0;
}

/* Before a frame runs.  0 = the partner's tick has not come: pump and ask again.
 * 1 = run it; what is due in this frame has been handed to the receive path. */
int rfu_sched_may_run(void)
{
  s32 frame = (s32)rfu_frame_no, need;
  if (!rfu_sched_on)
    return 1;
  if (!rfu_sched_still_linked()) {
    rfu_sched_stop();
    return 1;
  }
  need = frame - rfu_sched_cur_d() - rfu_sched_off;
  if (need - rfu_sched_peer_start >= 0 &&
      (!rfu_sched_peer_ok || rfu_sched_peer_done - need < 0))
    return 0;
  rfu_sched_release(frame, 0);
  return 1;
}

/* The partner has gone quiet: hand everything over and run free for the rest of
 * this link, so the game's own timeouts decide what happens. */
void rfu_sched_give_up(void)
{
  rfu_sched_flush(0);
  rfu_sched_on = 0;
  rfu_sched_gave_up = 1;
  rfu_sched_release(0, 1);
}

int rfu_sched_active(void)
{
  return (int)rfu_sched_on;
}
