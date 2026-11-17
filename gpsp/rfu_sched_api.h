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

#endif
