/* rfu_sched_api.h -- what a frontend calls for scheduled ticks (rfu_sched.h,
 * docs/RFU-COUPLING.md).  Every build has them (C-4); rfu_sched_d = 0 is a
 * frontend with no tick gate (libretro, 3ds) or a harness A/B arm. */
#ifndef RFU_SCHED_API_H
#define RFU_SCHED_API_H

/* D, the partner's frames between publish and delivery.  2 for two consoles
 * in one room (ad hoc), 4 for an online room, unless a harness says
 * otherwise; 0 turns ticks off for this run. */
#define RFU_SCHED_D_DEFAULT 2u
/* A tick leaves its console at the end of its frame and is on time only while
 * one-way latency + the sender's frame work <= D frames.  The radio's ~5 ms
 * fits D = 2; the internet does not (PSP-3000 vs the Lab VPS, Emerald trade,
 * linked phase: 36.8 fps at D = 2, 41.5 at 3, 45.8 at 4; Lab ONLINE-SUITE.md
 * 10.3).  Only the partner's data waits D frames, never the player's own
 * controls, and librfu NI handshakes still run at D = 1 (rfu_sched.h). */
#define RFU_SCHED_D_ONLINE 4u

/* The D a session runs, from what BOTH consoles know before it starts: the
 * kind of session (an online room or ad hoc in one room -- the two cannot
 * meet: they are different transports) and the build, which an online room
 * and netdrv's admission both require to be the same.  Nothing is
 * negotiated, so the two can never disagree.  harness_d >= 0 (the harness
 * key rfu_sched, the Lab's D study arms) is used for every session as it is;
 * < 0 = not set. */
static inline unsigned rfu_sched_d_for(int online, long harness_d)
{
  if (harness_d >= 0)
    return (unsigned)harness_d;
  return online ? RFU_SCHED_D_ONLINE : RFU_SCHED_D_DEFAULT;
}

extern unsigned rfu_sched_d, rfu_sched_held_peak;
/* 1 = D drops to 1 through librfu NI handshakes and a link's start window
 * (rfu_sched.h); the harness key rfu_sched_ni = 0 is an A/B arm. */
extern unsigned rfu_sched_ni_on;
/* Sets D (one store: the older delivery rules are gone, C-4).  Call it
 * before any adapter session starts. */
void rfu_sched_config(unsigned d);
/* Once a loop: is this console's menu open?  The partner waits while it is. */
void rfu_sched_pause(int on);
int rfu_sched_peer_is_paused(void);
/* Before a frame: 1 = run it, 0 = the partner's tick is missing. */
int rfu_sched_may_run(void);
/* The partner went quiet: run free for the rest of this link. */
void rfu_sched_give_up(void);
int rfu_sched_active(void);
/* The partner's netplay id while locked, else -1. */
int rfu_sched_partner(void);

/* THE GATE'S WAIT (fe_host_rfu_gate), once per pass that found no tick.
 *
 * A partner that says its menu is open is waited for as long as that lasts.
 * One whose ticks are held up by the NETWORK -- netdrv_link_held: nothing
 * heard for RFU_GATE_QUIET_US (an idle peer PINGs every 250 ms), a receive
 * gap waiting for a retransmission, or our own data unacknowledged that long
 * -- is waited for too, as a cable session waits, until the tick lands or
 * netdrv declares the partner dead (8 s; the lost link then ends the wait in
 * rfu_sched_may_run).  Only a partner whose path is clear and that still
 * sends no tick for RFU_GATE_SILENT_US is given up on.
 *
 * Why the network must not count as silence.  Running free while the
 * partner is alive builds a lead: the free frames' data, and the partner's
 * for the frames it missed, are overdue once the pair locks again, and a
 * game drains its partner's data at about one packet a frame -- so the lead
 * arrives as one burst that no receive queue can hold.  Lab j7c1b2e (3000 vs
 * VPS, D = 4): one lost packet waited ~2.5 s for its retransmission (the RTO
 * had backed off in an outage) behind a gap of 139 frames, the VPS gave up at
 * 2 s three times, and both receive queues overflowed (rfu_qdrop 8 + 8); the
 * trade's save never finished.
 *
 * since: when the wait for this tick began (0 = not started; the gate
 * clears it after a frame runs).  held: fe_np_link_held for the partner.
 * 1 = give up now. */
#define RFU_GATE_SILENT_US 2000000u
#define RFU_GATE_QUIET_US  1000000u
static inline int rfu_gate_step(unsigned long long *since,
                                unsigned long long now, int peer_paused,
                                int held)
{
  if (!*since || peer_paused || held > 0) {
    *since = now;
    return 0;
  }
  return now - *since > RFU_GATE_SILENT_US;
}

#endif
