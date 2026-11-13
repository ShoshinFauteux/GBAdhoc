/* ff_gate.h -- the fast-forward DRAW GATE (docs/FF-PRESETS.md).
 *
 * The LCD shows at most 60 frames a second, so drawing an emulated frame the
 * screen will never show is pure cost: on the CPU renderer it is ~6 ms of
 * rendering, and with the Media Engine it is the capture (per-line registers,
 * the mid-frame log, the palette/VRAM/OAM write-backs) on this CPU plus a
 * render on the other one.  The two unlimited presets therefore emulate flat
 * out and DRAW at most one frame per period:
 *
 *     Unlimited          33333 us   (30 drawn frames a second)
 *     Unlimited Smooth   16667 us   (60 drawn frames a second)
 *
 * The decision is made at the START of each emulated frame, so a frame that
 * will not be drawn is never captured or rendered at all (gpSP frameskip).
 *
 * THE RULE.  Boundaries sit on a fixed grid, one period apart.  A frame is
 * drawn when it will FINISH at or after the next boundary; its finishing time
 * is predicted as its start plus the previous frame's duration.  Drawing the
 * first frame that ends past the boundary -- rather than the first that STARTS
 * past it -- lands each draw at or just after its boundary, and advancing the
 * boundary by exactly one period (rather than "one period after the last
 * draw") keeps the long-run rate at exactly 30 or 60 instead of drifting low
 * by a fraction of a frame every period.
 *
 * Two clamps keep it honest when emulation is NOT fast:
 *  - a frame duration above one period (a menu, a state load, or emulation
 *    slower than the draw rate) is not a usable prediction, so it counts as
 *    zero: one stall cannot schedule a burst or delay the next draw;
 *  - a draw more than a period behind its boundary re-anchors the grid on
 *    itself, so a slow stretch is never "repaid" later as back-to-back draws.
 * When emulation runs slower than the draw rate, every frame is drawn.
 *
 * Header-only and platform-free so the host test exercises this exact code
 * (tools/test_ff_pipeline.c), the multiplier ceiling at the end included. */
#ifndef FF_GATE_H
#define FF_GATE_H

#include <stdint.h>

#define FF_GATE_PERIOD_UNLIMITED_US 33333u   /* 30 drawn frames/s */
#define FF_GATE_PERIOD_SMOOTH_US    16667u   /* 60 drawn frames/s */

typedef struct
{
   unsigned period_us;   /* 0 = off: every frame is drawn */
   uint64_t due_us;      /* next draw boundary; 0 = none yet */
   uint64_t prev_us;     /* previous frame's start; 0 = none yet */
   unsigned resyncs;     /* draws that re-anchored the grid (telemetry) */
} ff_gate_t;

/* (Re)start the gate with a new period (0 = off).  The first frame after
 * arming is always drawn. */
static inline void ff_gate_arm(ff_gate_t *g, unsigned period_us)
{
   /* resyncs is cumulative across arms on purpose (a telemetry counter). */
   g->period_us = period_us;
   g->due_us = 0;
   g->prev_us = 0;
}

/* Call once at the start of every emulated frame with the current time.
 * Returns 1 if this frame is to be drawn, 0 if it is to be skipped. */
static inline int ff_gate_frame(ff_gate_t *g, uint64_t now_us)
{
   unsigned dur;
   uint64_t end;

   if (!g->period_us)
      return 1;
   dur = g->prev_us ? (unsigned)(now_us - g->prev_us) : 0u;
   g->prev_us = now_us;
   if (dur > g->period_us)
      dur = 0;                     /* a stall is not a frame's cost */
   end = now_us + dur;             /* about when this frame will be done */
   if (g->due_us && end < g->due_us)
      return 0;
   if (g->due_us && end < g->due_us + g->period_us)
      g->due_us += g->period_us;   /* on the grid: the next boundary */
   else
   {
      if (g->due_us)
         g->resyncs++;                  /* a period behind: boundaries lost */
      g->due_us = end + g->period_us;   /* first draw, or a period behind */
   }
   return 1;
}

/* THE MULTIPLIER CEILING (FF FLIP PACING bit 2, main_psp.c g_ff_flip_pace).
 *
 * The multiplier presets (1.5x, 2x, 3x) run one emulated frame per loop under
 * a ceiling of mult_x10 / 10 frames per vblank, counted from an anchor:
 * `df` frames emulated and `dv` vblanks passed since it.  Ahead of the
 * ceiling the loop blocks on the vblank; 1.5x therefore runs 2, 1, 2, 1 ...
 * frames per vblank (3 per 2), 2x runs 2, and 3x runs 3, whatever the
 * console could do beyond that.
 *
 * Tenths of a frame, so 1.5x is exact: no float, no drift.  For 3x every
 * test below is the arithmetic ffh_cap3 had before the other two existed. */

/* >= 0: this vblank interval's quota is used up; wait for the next vblank. */
static inline int ff_cap_over(unsigned mult_x10, unsigned df, unsigned dv)
{
   return (int)(10u * df - mult_x10 * dv) - (int)mult_x10;
}

/* Behind by more than 8 vblanks' worth: re-anchor (forgive, never sprint). */
static inline int ff_cap_lost(unsigned mult_x10, unsigned df, unsigned dv)
{
   return (int)(mult_x10 * dv - 10u * df) > (int)(mult_x10 * 8u);
}

/* At the START of a frame: has every vblank interval since the anchor had its
 * quota?  Then the ceiling, not the console, sets the rate. */
static inline int ff_cap_on_pace(unsigned mult_x10, unsigned df, unsigned dv)
{
   return (int)(10u * df - mult_x10 * dv) >= 0;
}

/* At the START of a frame: will this frame be the last one of its vblank
 * interval (the ceiling blocks after it)?
 *
 * THE DRAW RULE BELOW 3x.  On pace, 1.5x and 2x draw exactly that frame: one
 * picture per vblank, each the newest frame the interval emulated, so the
 * picture advances 2, 1, 2, 1 ... game frames (1.5x) or 2, 2, ... (2x) with
 * no exception.  The draw gate's wall-clock grid (16667 us) beats against the
 * LCD (16683 us) and against the ceiling's bursts: it picks a different frame
 * of a burst as the phase drifts, and with one frame in every other interval
 * (1.5x) that reads as a picture held for two vblanks and then a jump of 3.
 * Off pace (the console cannot run the multiplier) the gate decides, as it
 * does for the unlimited presets: there is no burst to pick from. */
static inline int ff_cap_last(unsigned mult_x10, unsigned df, unsigned dv)
{
   return ff_cap_over(mult_x10, df + 1u, dv) >= 0;
}

#endif /* FF_GATE_H */
