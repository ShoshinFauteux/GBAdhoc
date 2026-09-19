/* ff_psp.h -- fast-forward and session pacing: the whole interface.
 *
 * ff_psp.c was carved out of psp/main_psp.c.  This header is deliberately the
 * complete contract, in both directions, because the point of the split was to
 * make the coupling countable: 14 entry points out, 6 host symbols in.
 */
#ifndef FF_PSP_H
#define FF_PSP_H

#include <stdint.h>

/* Hoisted out of the pacing constants below (ADR-0037): the frameskip policy
 * sits between here and there and now needs it too. */
#define PACE_NOMINAL_X100      5973   /* GBA 59.7275 Hz: never target above */

/* ---- pacing modes and limits, shared because main() parses them ---------
 * `net_pace_mode` in config picks one; ff_psp.c holds the reasoning for each. */
#define PACE_MODE_OFF             0
#define PACE_MODE_FIXED           1   /* ADR-0033, default */
#define PACE_MODE_ADAPTIVE        2   /* ADR-0027/0028, kept for the A/B */
#define PACE_MODE_BACKPRESSURE    3   /* txq-driven; see ff_psp.c */

#define PACE_FLOOR_X100        4000   /* never pace BELOW 40.00 fps ourselves */
#define PACE_FIXED_RAMP_X100_PER_S 400 /* glide 4.00 fps/s: ~5 s in and out */

/* ---- WHAT THE FRONTEND CALLS ------------------------------------------- */
void skip_policy_set(const char *mode);
void skip_policy_session_begin(void);
void skip_policy_session_end(void);
void skip_policy_reapply(void);
void skip_policy_frame(void);
const char *ff_chip_text(void);
void pace_session_begin(void);
void pace_session_end(void);
void pace_frame(uint32_t work_us);
int pace_window(uint32_t work_us, unsigned *achieved_x100);
void pace_burn(void);
void pace_audio_step(unsigned target_x100);
unsigned pace_snap_x100(unsigned req_x100, unsigned *vb);
unsigned pace_extra_vblanks(void);

/* ---- WHAT THE FRONTEND MAY READ ----------------------------------------
 * Pacing state, for telemetry only.  Read-only on purpose: the engine decides
 * the rate, and nothing outside it may assign one. */
unsigned ff_pace_target_x100(void);
unsigned ff_pace_cap_x100(void);
unsigned ff_pace_peer_x100(void);
int      ff_pace_engaged(void);

/* The harness `pace_slow_us` knob: injected delay, to prove the pacing loop
 * reacts.  Clamped by the setter. */
int  ff_pace_slow_us(void);
void ff_pace_set_slow_us(int us);

/* ---- WHAT THE ENGINE READS FROM THE FRONTEND ---------------------------
 *
 * Six symbols, still defined in main_psp.c, listed here so the dependency is a
 * fact you can count rather than something you discover.  Each names its owner;
 * the engine only ever READS them.
 *
 * Narrowing these to a query interface is the next step and is deliberately
 * NOT part of this move: a pure relocation is reviewable, and mixing it with an
 * interface change would not be. */
extern int      g_ff_uncapped;   /* input/UI: uncapped fast-forward selected  */
extern int      g_ff_mult;       /* input/UI: multiplier FF active (paced)    */
extern int      g_net_up;        /* wireless: transport + netdrv live         */
extern int      g_net_is_host;   /* wireless: our role this session           */
extern unsigned in_rate;         /* audio: core sample rate                   */
/* The audio output rate, so the engine can compute the resample step it
 * writes to g_audio_step.  Defined here rather than in main_psp.c because
 * both need it and there is no audio module yet to own it -- when audio is
 * extracted this moves with it. */
#define OUT_RATE 44100
extern volatile unsigned g_audio_step;  /* audio: resample step, set by pacing */

/* Clock and telemetry the engine calls back into. */
uint64_t net_now_us(void);
void     sess_cost_evt(int force);

#endif /* FF_PSP_H */
