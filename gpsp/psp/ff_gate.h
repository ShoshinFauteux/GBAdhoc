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
 * (tools/test_ff_pipeline.c). */
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

#endif /* FF_GATE_H */
