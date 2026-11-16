/* rfu_sched_api.h -- what a frontend calls for scheduled ticks (rfu_sched.h,
 * docs/RFU-COUPLING.md).  Every build has them (C-4); rfu_sched_d = 0 is a
 * frontend with no tick gate (libretro, 3ds) or a harness A/B arm. */
#ifndef RFU_SCHED_API_H
#define RFU_SCHED_API_H

/* D, the partner's frames between publish and delivery.  2 unless a harness
 * says otherwise; 0 turns ticks off for this run. */
#define RFU_SCHED_D_DEFAULT 2u

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
