/* fe_linkwait.h -- "Waiting for the other player...": when a link session's
 * hold on the game is long enough to say so on screen.
 *
 * A link session holds the game, running no frame, while the partner's data
 * for the next one is missing: the adapter's tick gate (fe_host_rfu_gate,
 * rfu_sched_api.h rfu_gate_step), which waits out the network until netdrv
 * declares the partner dead (8 s), and the cable's lockstep
 * (fe_gblink_step's FE_GBLINK_STALL).  A short hold is a hitch nobody needs
 * told about; a frozen picture that lasts reads as a crash.  So once a hold
 * has lasted LINK_WAIT_NOTICE_US the frontend says what it is waiting for,
 * and clears it on the first frame that runs.  A partner that is declared
 * dead ends the hold the same way (the lost link lets the gate run), and the
 * session's own disconnect flow takes over from there.
 *
 * fe_host says whether a pass held (fe_host_link_held: either wait point);
 * the platform keeps `since` = when the hold began (0 = not holding) and
 * applies these two rules (psp/fe/link.d.inc.c link_wait_osd).  Header-only:
 * the rules are shared with the host test (tools/tests/test_rfu_sched.c). */
#ifndef FE_LINKWAIT_H
#define FE_LINKWAIT_H

#define LINK_WAIT_NOTICE_US 1000000u

/* Once a pass: held = this pass ran no frame for want of the partner's data.
 * A pass that ran one (or has no link) clears the hold. */
static inline void link_wait_note(unsigned long long *since,
                                  unsigned long long now, int held)
{
  if (!held)
    *since = 0;
  else if (!*since)
    *since = now ? now : 1;
}

/* 1 = this hold has lasted long enough to say so. */
static inline int link_wait_due(unsigned long long since,
                                unsigned long long now)
{
  return since && now - since >= LINK_WAIT_NOTICE_US;
}

#endif
