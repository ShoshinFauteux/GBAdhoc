/* fe_linkprof.h -- the frontend's phase word for the DRPROF_HW time-split
 * sampler in a link session (docs/GBA-LINK-MIRROR.md section 16).
 *
 * The core publishes WHAT it is doing in reg[40] (drprof.h); this says WHERE
 * the frontend is, so the sampler can split a session frame into: the
 * machines (then the running instance and ITS reg[40]), the sync hash, the
 * cartridge check, the network, the session's own bookkeeping, and the rest
 * of the loop.  Compiles to nothing unless DRPROF_HW. */
#ifndef FE_LINKPROF_H
#define FE_LINKPROF_H

enum
{
   FELP_LOOP = 0,     /* the main loop: present, OSD, audio produce, pacing */
   FELP_CORE = 1,     /* inside the machines (advance / retro_run)          */
   FELP_HASH = 2,     /* periodic sync hash                                 */
   FELP_CRC = 3,      /* cartridge-RAM check                                */
   FELP_NET = 4,      /* netdrv pump, bulk-lane drain, absorbed waits       */
   FELP_SESS = 5,     /* fe_gblink_step outside the above (protocol, sends) */
   FELP_VWAIT = 6,    /* the loop's vblank wait (pumps the radio; idle)     */
   FELP_N = 8,
   /* The main loop's own regions (section 17): counted as FELP_LOOP in
    * `fe=` and split again in `lp=`.  FELP_L0 + k, k < FELP_LN. */
   FELP_L0 = 8,
   FELP_L_TOP = FELP_L0 + 0,    /* loop top: power ticks, pad, input log     */
   FELP_L_NETF = FELP_L0 + 1,   /* net_frame()                               */
   FELP_L_GBLS = FELP_L0 + 2,   /* gblink_service(): session audio, OSD chip */
   FELP_L_MISC = FELP_L0 + 3,   /* mgift/rfu/audio status, UI chords, FF     */
   FELP_L_PRES0 = FELP_L0 + 4,  /* loop-top ME present + fps chip            */
   FELP_L_AP = FELP_L0 + 5,     /* autopilot script frame                    */
   FELP_L_RUN = FELP_L0 + 6,    /* fe_host_run_frame outside the session step*/
   FELP_L_POST = FELP_L0 + 7,   /* after the core: health, dumps, rig events */
   FELP_L_SHASH = FELP_L0 + 8,  /* harness shash.txt line (+ jit_coh_frame)  */
   FELP_L_ACCT = FELP_L0 + 9,   /* work accounting, pace_frame               */
   FELP_L_OSD = FELP_L0 + 10,   /* pre-wait osd_draw, vcount, win work       */
   FELP_L_RETIRE = FELP_L0 + 11,/* after the wait: the ME's last-look retire */
   FELP_L_PRES = FELP_L0 + 12,  /* (unset: the same-frame present, deleted)  */
   FELP_L_SWAP = FELP_L0 + 13,  /* vid_swap                                  */
   FELP_L_RIGN = FELP_L0 + 14,  /* rig_note (perf rig per-frame bookkeeping) */
   FELP_LN = 16,
   FELP_MASK = 31
};

#ifdef DRPROF_HW
extern volatile unsigned fe_lp_phase;
static inline unsigned fe_lp_swap(unsigned p)
{
   unsigned o = fe_lp_phase;
   fe_lp_phase = p;
   return o;
}
static inline void fe_lp_restore(unsigned *o) { fe_lp_phase = *o; }
#define FE_LP_SCOPE(p) \
   unsigned fe_lp_old_ __attribute__((cleanup(fe_lp_restore), unused)) = \
      fe_lp_swap(p)
#define FE_LP_SET(p) (fe_lp_phase = (p))
#else
#define FE_LP_SCOPE(p)    extern int fe_lp_unused_decl_ __attribute__((unused)) /* no code */
#define FE_LP_SET(p) do { } while (0)
#endif

#endif
