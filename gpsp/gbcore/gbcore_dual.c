/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two Game Boys in one process (gbcore_dual.h).
 */
#include "gbcore_dual.h"

#include <stdlib.h>
#include <string.h>

typedef struct gbdual_port {
  struct gbdual *dual;
  int slot;
  /* External-clock arms: SC writes with bit 7 set and bit 0 clear. */
  uint32_t arms;
  uint8_t arm_sb;              /* SB when it last armed */
  uint32_t peer_arms_at_start; /* the peer's `arms` when our transfer began */
  uint64_t start_line;         /* scanline of our last internal start */
} gbdual_port_t;

/* Emulated wall clocks (gbdual_config_t.rtc_seed).  gbcore_set_wallclock
 * takes a plain function, one per instance, so the pair that owns them is
 * found through this pointer (one pair exists at a time: each instance
 * allows one machine). */
static struct gbdual *clock_owner;
static time_t emulated_clock(int slot);
static time_t emulated_clock0(void) { return emulated_clock(0); }
static time_t emulated_clock1(void) { return emulated_clock(1); }

struct gbdual {
  const gbcore_api_t *api[GBDUAL_SLOTS];
  gbcore_t *core[GBDUAL_SLOTS];
  gbcore_callbacks_t user[GBDUAL_SLOTS];
  gbdual_port_t port[GBDUAL_SLOTS];
  uint64_t frame[GBDUAL_SLOTS];
  uint64_t serial_bytes[GBDUAL_SLOTS];
  int need_begin[GBDUAL_SLOTS];
  int powered[GBDUAL_SLOTS];
  /* stepping: slot 0 leads by up to `batch` scanlines while the cable is
   * quiet; otherwise strict alternation (gbdual_config_t.batch_lines) */
  uint64_t lines[GBDUAL_SLOTS];    /* scanlines each machine has run */
  unsigned batch, lead_left;
  int in_batch;                    /* this turn started as a batch */
  uint64_t quiet_lines, last_serial;  /* in slot-0 scanlines */
  int touched;                     /* a serial event in the running line */
  uint64_t cur_line;               /* index of the scanline being run */
  /* A batch cut short: slot 0's serial event came at scanline cut_line,
   * and slot 1 is catching up through it.  For slot 1's scanlines before
   * cut_line, slot 0's port is shown as it was before that line (not
   * armed, arms = cut_arms): strict alternation would not have run slot
   * 0's line cut_line yet. */
  int cut;
  uint64_t cut_line;
  uint32_t cut_arms, line_arms;
  uint64_t serial_starts[GBDUAL_SLOTS];
  uint64_t batched_lines, cuts;
  int64_t rtc_seed;
  int pace_slot;
  uint64_t line_pairs;
  void (*serial_trace)(void *, int, uint8_t, uint8_t, uint8_t, uint64_t);
  void *serial_trace_userdata;
  void (*line_trace)(void *, int, uint64_t);
  void (*frame_hook)(void *, int, uint64_t);
  void *frame_hook_userdata;
};

/* Seconds: the seed plus the machine's completed frames at 59.7275 Hz
 * (70224 clocks of 4194304), which is the same on both consoles. */
static time_t emulated_clock(int slot)
{
  const struct gbdual *d = clock_owner;
  if (!d)
    return 0;
  return (time_t)(d->rtc_seed +
                  (int64_t)(d->frame[slot] * 70224ull / 4194304ull));
}

const gbcore_api_t *gbdual_api(int slot)
{
  if (slot == 0)
    return &gbcore_api;
  if (slot == 1)
    return &gbcoreb_api;
  return NULL;
}

/* Slot 1 is running a scanline that strict alternation would run before
 * slot 0's line cut_line. */
static int hidden(const struct gbdual *d)
{
  return d->cut && d->cur_line < d->cut_line;
}

/* The peer's arm count as `slot` may see it now. */
static uint32_t peer_arms_seen(const struct gbdual *d, int slot)
{
  if (slot == 1 && hidden(d))
    return d->cut_arms;
  return d->port[slot ^ 1].arms;
}

/* A transfer starts (the SC write).  Nothing crosses the cable yet; this
 * only notes when each side armed the external clock and, for an
 * internal-clock start, how often the peer had armed by then. */
static int on_serial_start(void *userdata, uint8_t outgoing,
                           int internal_clock, uint8_t *received)
{
  gbdual_port_t *p = (gbdual_port_t *)userdata;
  struct gbdual *d = p->dual;
  (void)received;
  d->touched = 1;
  d->serial_starts[p->slot]++;
  if (internal_clock)
  {
    p->peer_arms_at_start = peer_arms_seen(d, p->slot);
    p->start_line = d->cur_line;
  }
  else
  {
    p->arms++;
    p->arm_sb = outgoing;
    /* Slot 1 catching up arms BEFORE slot 0's transfer began (in the game's
     * time): it must not count as armed during that transfer. */
    if (p->slot == 1 && hidden(d) && d->port[0].start_line == d->cut_line)
      d->port[0].peer_arms_at_start++;
  }
  return 0;                    /* completion is decided by on_serial_clocked */
}

/* The cable.  The clocking machine's transfer has shifted its eight bits.
 *
 *  - The peer is waiting on the external clock now: it takes the byte and
 *    hands back what its SB held (TGB Dual's rule).
 *  - The peer armed the external clock at some point DURING this transfer
 *    but has since moved on -- the link handshakes of Pokemon Red/Blue and
 *    Gold/Silver/Crystal hold the external clock for only a few
 *    instructions each frame, then start an internal-clock attempt of their
 *    own.  On hardware the clocking side's pulses reach the peer while it
 *    is armed; here, stepping one scanline at a time, that moment is not
 *    visible, so the arm is remembered: the peer takes the byte (its own
 *    attempt is cancelled) and the clocking side gets the SB the peer armed
 *    with.  Two machines in exact lockstep then resolve deterministically,
 *    slot 0 clocking, where real consoles would resolve by their clock
 *    phase.
 *  - Otherwise the line is idle: 0xFF. */
static int on_serial_clocked(void *userdata, uint8_t outgoing,
                             uint8_t *received)
{
  gbdual_port_t *p = (gbdual_port_t *)userdata;
  struct gbdual *d = p->dual;
  int peer = p->slot ^ 1;
  gbdual_port_t *q = &d->port[peer];
  int hide = p->slot == 1 && hidden(d);
  uint8_t peer_sc = hide ? 0 : d->api[peer]->serial_control(d->core[peer]);
  uint32_t peer_arms = peer_arms_seen(d, p->slot);
  d->touched = 1;
  if ((peer_sc & 0x81) == 0x80)
    *received = d->api[peer]->serial_clock_in(d->core[peer], outgoing);
  else if (peer_arms != p->peer_arms_at_start)
  {
    *received = q->arm_sb;
    d->api[peer]->serial_deliver(d->core[peer], outgoing);
  }
  else
    *received = 0xFF;
  p->peer_arms_at_start = peer_arms;
  d->serial_bytes[p->slot]++;
  if (d->serial_trace)
    d->serial_trace(d->serial_trace_userdata, p->slot, outgoing, *received,
                    peer_sc, d->line_pairs);
  return 1;
}

static void on_video(void *userdata, const gbcore_video_frame_t *frame)
{
  gbdual_port_t *p = (gbdual_port_t *)userdata;
  const gbcore_callbacks_t *u = &p->dual->user[p->slot];
  if (u->video)
    u->video(u->userdata, frame);
}

static void on_audio(void *userdata, const int16_t *samples, size_t frames)
{
  gbdual_port_t *p = (gbdual_port_t *)userdata;
  const gbcore_callbacks_t *u = &p->dual->user[p->slot];
  if (u->audio_batch)
    u->audio_batch(u->userdata, samples, frames);
}

gbdual_t *gbdual_create(const gbdual_config_t *config)
{
  gbdual_t *d = NULL;
  int owned_taken[GBDUAL_SLOTS] = { 0, 0 };
  int s;

  if (!config || config->pace_slot < 0 || config->pace_slot >= GBDUAL_SLOTS)
    goto fail;
  for (s = 0; s < GBDUAL_SLOTS; s++)
    if (config->machine[s].callbacks.serial_transfer ||
        config->machine[s].callbacks.serial_clocked)
      goto fail;
  if (config->rtc_seed && clock_owner)
    goto fail;
  d = (gbdual_t *)calloc(1, sizeof(*d));
  if (!d)
    goto fail;
  d->pace_slot = config->pace_slot;
  d->batch = config->batch_lines ? config->batch_lines : 1;
  d->quiet_lines = (uint64_t)(config->quiet_frames ? config->quiet_frames
                                                   : 60u) * 154u;
  d->rtc_seed = config->rtc_seed;
  d->serial_trace = config->serial_trace;
  d->serial_trace_userdata = config->serial_trace_userdata;
  d->line_trace = config->line_trace;
  d->frame_hook = config->frame_hook;
  d->frame_hook_userdata = config->frame_hook_userdata;
  if (d->rtc_seed)
    clock_owner = d;
  for (s = 0; s < GBDUAL_SLOTS; s++)
  {
    const gbdual_machine_t *m = &config->machine[s];
    gbcore_callbacks_t cb;
    d->api[s] = gbdual_api(s);
    d->user[s] = m->callbacks;
    d->port[s].dual = d;
    d->port[s].slot = s;
    d->need_begin[s] = 1;
    memset(&cb, 0, sizeof(cb));
    cb.userdata = &d->port[s];
    cb.video = on_video;
    cb.audio_batch = on_audio;
    if (config->link)
    {
      cb.serial_transfer = on_serial_start;
      cb.serial_clocked = on_serial_clocked;
    }
    /* Both consoles must start from the same machine, whatever either
     * has run before. */
    if (d->api[s]->power_on() != 0)
      goto fail;
    d->powered[s] = 1;
    d->api[s]->set_wallclock(d->rtc_seed ? (s ? emulated_clock1
                                                : emulated_clock0)
                                         : m->wallclock);
    d->api[s]->set_palette(m->palette);
    if (m->rom_owned)
    {
      owned_taken[s] = 1;          /* the adapter frees it on failure too */
      d->core[s] = d->api[s]->create_owned(m->rom_owned, m->rom_size,
                                           m->console, config->audio_rate,
                                           &cb);
    }
    else
      d->core[s] = d->api[s]->create(m->rom_path, m->rom_data, m->rom_size,
                                     m->console, config->audio_rate, &cb);
    if (!d->core[s])
      goto fail;
    if (m->headless)
      d->api[s]->set_headless(d->core[s], 1);
    if (m->save && m->save_size &&
        d->api[s]->save_ram_write(d->core[s], m->save, m->save_size) != 0)
      goto fail;
  }
  return d;

fail:
  if (config)
    for (s = 0; s < GBDUAL_SLOTS; s++)
      if (!owned_taken[s])
        free(config->machine[s].rom_owned);
  gbdual_destroy(d);
  return NULL;
}

void gbdual_destroy(gbdual_t *d)
{
  int s;
  if (!d)
    return;
  for (s = 0; s < GBDUAL_SLOTS; s++)
  {
    if (d->core[s])
      d->api[s]->shutdown(d->core[s]);
    /* Hand the instance back as a fresh program would have it. */
    if (d->powered[s])
      d->api[s]->power_on();
  }
  if (clock_owner == d)
    clock_owner = NULL;
  free(d);
}

/* May slot 0 run ahead?  Neither machine's serial port is busy (a transfer
 * requested, or armed on the external clock) and nothing serial happened
 * for the quiet period. */
static int cable_quiet(gbdual_t *d)
{
  int s;
  if (d->batch <= 1 || d->lines[0] - d->last_serial < d->quiet_lines)
    return 0;
  for (s = 0; s < GBDUAL_SLOTS; s++)
    if (d->api[s]->serial_control(d->core[s]) & 0x80)
      return 0;
  return 1;
}

int gbdual_advance(gbdual_t *d, gbdual_input_fn input, void *userdata)
{
  if (!d || !input)
    return -1;
  for (;;)
  {
    int s;
    int r;
    /* Slot 0 finishes its turn, then slot 1 catches up; aligned, slot 0
     * takes a new turn: one scanline, or `batch` while the cable is quiet.
     * With batch 1 this is exactly the old strict alternation.  Every
     * decision depends only on the machines' state, never on where a
     * stall or a frame return fell, so both consoles step identically. */
    if (d->lead_left)
      s = 0;
    else if (d->lines[1] < d->lines[0])
      s = 1;
    else
    {
      s = 0;
      d->in_batch = cable_quiet(d);
      d->lead_left = d->in_batch ? d->batch : 1;
    }
    if (d->need_begin[s])
    {
      uint16_t buttons = 0;
      if (!input(userdata, s, d->frame[s], &buttons))
        return 0;                              /* stall, resumable */
      if (d->api[s]->frame_begin(d->core[s], buttons) != 0)
        return -1;
      d->need_begin[s] = 0;
    }
    d->touched = 0;
    d->cur_line = d->lines[s];
    if (s == 0)
      d->line_arms = d->port[0].arms;
    r = d->api[s]->run_line(d->core[s]);
    if (r < 0)
      return -1;
    if (d->line_trace)
      d->line_trace(d->serial_trace_userdata, s, d->line_pairs);
    d->lines[s]++;
    if (d->in_batch)
      d->batched_lines++;
    if (s == 0)
      d->lead_left--;
    if (d->touched)
    {
      /* Serial activity: the rest of this turn is cancelled (the other
       * machine catches up next), and strict alternation holds for the
       * quiet period. */
      if (s == 0 && d->lines[1] + 1 < d->lines[0] && !d->cut)
      {
        d->cut = 1;
        d->cuts++;
        d->cut_line = d->cur_line;
        d->cut_arms = d->line_arms;
      }
      d->lead_left = 0;
      d->last_serial = d->lines[0];
    }
    if (d->cut && d->lines[1] >= d->lines[0])
      d->cut = 0;
    if (s == GBDUAL_SLOTS - 1)
      d->line_pairs++;
    if (r == 1)
    {
      d->need_begin[s] = 1;
      d->frame[s]++;
      if (d->frame_hook)
        d->frame_hook(d->frame_hook_userdata, s, d->frame[s]);
      if (s == d->pace_slot)
        return 1;
    }
  }
}

uint64_t gbdual_frame(const gbdual_t *d, int slot)
{
  return d && slot >= 0 && slot < GBDUAL_SLOTS ? d->frame[slot] : 0;
}

uint64_t gbdual_lines(const gbdual_t *d)
{
  return d ? d->line_pairs : 0;
}

uint64_t gbdual_serial_starts(const gbdual_t *d, int slot)
{
  return d && slot >= 0 && slot < GBDUAL_SLOTS ? d->serial_starts[slot] : 0;
}

uint64_t gbdual_batched_lines(const gbdual_t *d)
{
  return d ? d->batched_lines : 0;
}

uint64_t gbdual_batch_cuts(const gbdual_t *d)
{
  return d ? d->cuts : 0;
}

uint64_t gbdual_serial_bytes(const gbdual_t *d, int slot)
{
  return d && slot >= 0 && slot < GBDUAL_SLOTS ? d->serial_bytes[slot] : 0;
}

gbcore_t *gbdual_core(const gbdual_t *d, int slot)
{
  return d && slot >= 0 && slot < GBDUAL_SLOTS ? d->core[slot] : NULL;
}

uint64_t gbdual_sync_hash(gbdual_t *d)
{
  uint64_t a, b;
  if (!d)
    return 0;
  a = d->api[0]->sync_hash(d->core[0]);
  b = d->api[1]->sync_hash(d->core[1]);
  /* Order-sensitive: swapping the machines must change it. */
  return a ^ (b * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull);
}
