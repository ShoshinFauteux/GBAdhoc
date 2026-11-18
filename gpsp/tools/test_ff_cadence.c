/* The fast-forward multiplier CADENCE: the real ceiling, draw rule and flip
 * hold (main_psp.c's ffh_too_soon .. ffh_cap_draw, extracted by
 * run_ff_tests.py as ff_cadence.inc through @extract ff:ffh_cadence), the real
 * draw gate and ceiling arithmetic (psp/ff_gate.h), the real paced loop's
 * frame count (@extract ff:mult_frames) and the real config_psp.c presets,
 * driven by a model of the uncapped branch of the main loop:
 *
 *     draw = ffh_cap_draw();  emulate (cost);  ffh_cap();
 *     if (drew) { if (ffh_hold()) hold; else flip (latches at the next vblank) }
 *
 * on a 59.94 Hz LCD (16683 us a vblank).  What the screen shows at each vblank
 * is the newest flip submitted before it.  Asserted: emulated rate, frames
 * per vblank interval, game frames the picture advances per vblank, drawn
 * pictures a second, and that 3x still draws exactly what the gate draws. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
#include "ff_gate.h"     /* run_ff_tests.py puts the real one (or a mutant) first */

void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "test"; }
const char *vid_filter_name(int mode) { (void)mode; return "test"; }

#define VB_US 16683.0
static double   now_us;
static unsigned frames;
static unsigned waits_this_loop;

static unsigned sceDisplayGetVcount(void) { return (unsigned)(now_us / VB_US); }
static int sceDisplayWaitVblankStart(void)
{
   now_us = (double)(sceDisplayGetVcount() + 1u) * VB_US + 1.0;
   waits_this_loop++;
   return 0;
}
static unsigned fe_host_frame_count(void) { return frames; }
static uint64_t net_now_us(void) { return (uint64_t)now_us; }

/* the state the extracted functions use (pacing.a.inc.c / power.a.inc.c) */
static int      g_ff_flip_pace = 7;
static int      g_ff_gate;
static ff_gate_t g_ff_gate_st;
static int      g_ffh_held;
static unsigned g_lat_sub_vc;
static int      g_ffc_on;
static unsigned g_ffc_vc, g_ffc_f, g_ffc_bvc;
/* telemetry statements (fe_priv.h TEL): this model counts none of them */
#define TEL(...) do { } while (0)
static int      g_ffc_q;
static unsigned g_ffm_acc;

#include "ff_cadence.inc"

/* The paced branch (FF FLIP PACING bit 2 off): frames this loop. */
static int mult_frames(void)
{
   int n;
#include "ff_mult_frames.inc"
   return n;
}

#define MAX_VB 8000
static unsigned shown_at[MAX_VB];   /* frame number on screen at vblank v */
static unsigned emu_in[MAX_VB];     /* frames finished in vblank interval v */
static unsigned flip_fr[MAX_VB];    /* newest flip submitted in interval v (0 none) */

typedef unsigned (*cost_fn)(unsigned frame);
static unsigned cost_us;
static unsigned c_const(unsigned f) { (void)f; return cost_us; }
static unsigned c_jitter(unsigned f)
{
   /* +-30 %, deterministic */
   unsigned h = f * 2654435761u;
   return cost_us * 7u / 10u + (h >> 8) % (cost_us * 6u / 10u + 1u);
}
static unsigned c_spike(unsigned f) { return cost_us + (f % 37u == 0 ? 20000u : 0u); }

typedef struct
{
   double   emu_fps, pics_fps;
   unsigned adv_hist[8];     /* game frames the picture advanced per vblank */
   unsigned emu_hist[8];     /* frames finished per vblank interval */
   unsigned alt_breaks;      /* 1.5x: two equal advances in a row */
   unsigned max_waits;       /* vblanks one loop's ceiling blocked */
   unsigned gate_diff;       /* draws that differ from a shadow gate (3x) */
   unsigned resyncs;         /* the gate re-anchored (lost boundaries) */
} run_t;

/* Run `secs` of uncapped multiplier FF at preset `mode`, cost model `cf`.
 * Measures from 1 s in.  `switch_from` >= 0: run that preset first for 2 s,
 * then switch the way main_psp.c does (re-anchor). */
static run_t run(int mode, cost_fn cf, unsigned cost, double secs, int switch_from)
{
   run_t r;
   ff_gate_t shadow;
   unsigned v, v0, v1, last_pic = 0, pic = 0, drew = 0, f0, i;
   double t_switch = switch_from >= 0 ? 2e6 : -1.0, t0;
   memset(&r, 0, sizeof(r));
   memset(shown_at, 0, sizeof(shown_at));
   memset(emu_in, 0, sizeof(emu_in));
   memset(flip_fr, 0, sizeof(flip_fr));
   now_us = 1.0;
   frames = 0;
   cost_us = cost;
   g_ffh_held = 0;
   g_lat_sub_vc = 0;
   g_ffc_on = g_ffc_q = 0;
   g_ffm_acc = 0;
   g_ff_gate = 1;
   pcfg_ff_set_mode(switch_from >= 0 ? switch_from : mode);
   ff_gate_arm(&g_ff_gate_st, FF_GATE_PERIOD_SMOOTH_US);
   ff_gate_arm(&shadow, FF_GATE_PERIOD_SMOOTH_US);
   g_ff_gate_st.resyncs = shadow.resyncs = 0;
   while (now_us < secs * 1e6)
   {
      int draw, shadow_draw;
      if (t_switch > 0 && now_us >= t_switch)
      {
         pcfg_ff_set_mode(mode);
         g_ffc_on = 0;        /* main_psp.c: a new multiplier re-anchors */
         t_switch = -1.0;
      }
      shadow_draw = ff_gate_frame(&shadow, net_now_us());
      draw = ffh_cap_draw();
      if (pcfg_ff_mode() == PCFG_FF_3X && draw != shadow_draw && now_us > 1e6)
         r.gate_diff++;
      now_us += cf(frames);
      frames++;
      v = sceDisplayGetVcount();
      if (v < MAX_VB)
         emu_in[v]++;
      if (draw)
      {
         drew = 1;
         pic = frames;
      }
      waits_this_loop = 0;
      ffh_cap();
      if (now_us > 1e6 && waits_this_loop > r.max_waits)
         r.max_waits = waits_this_loop;
      if (drew)
      {
         if (ffh_hold())
            g_ffh_held = 1;
         else
         {
            g_lat_sub_vc = sceDisplayGetVcount();
            if (g_lat_sub_vc < MAX_VB)
               flip_fr[g_lat_sub_vc] = pic;
            drew = 0;
            g_ffh_held = 0;
         }
      }
   }
   /* the screen: at vblank v, the newest flip submitted in an interval < v */
   v1 = sceDisplayGetVcount();
   assert(v1 < MAX_VB);
   for (v = 1; v <= v1; v++)
      shown_at[v] = flip_fr[v - 1] ? flip_fr[v - 1] : shown_at[v - 1];
   v0 = (unsigned)(1e6 / VB_US) + 1u;
   if (switch_from >= 0)
      v0 = (unsigned)(3e6 / VB_US);
   t0 = v0 * VB_US;
   f0 = 0;
   for (v = 0; v < v0; v++)
      f0 += emu_in[v];
   {
      unsigned fsum = 0, pics = 0, prev_adv = 0;
      for (v = v0; v < v1; v++)
      {
         unsigned adv = shown_at[v + 1] - shown_at[v];
         fsum += emu_in[v];
         r.emu_hist[emu_in[v] < 7 ? emu_in[v] : 7]++;
         r.adv_hist[adv < 7 ? adv : 7]++;
         if (adv)
            pics++;
         if (v > v0 && adv == prev_adv)
            r.alt_breaks++;
         prev_adv = adv;
         (void)last_pic;
      }
      r.emu_fps  = fsum / ((v1 - v0) * VB_US / 1e6);
      r.pics_fps = pics / ((v1 - v0) * VB_US / 1e6);
   }
   r.resyncs = g_ff_gate_st.resyncs;
   (void)t0; (void)f0; (void)i;
   return r;
}

static void show(const char *what, const run_t *r)
{
   printf("  %-28s emu %6.2f fps  pictures %5.2f/s  adv[0..4] %u %u %u %u %u  "
          "emu/vb[0..4] %u %u %u %u %u  max_wait %u  resync %u\n", what, r->emu_fps,
          r->pics_fps, r->adv_hist[0], r->adv_hist[1], r->adv_hist[2],
          r->adv_hist[3], r->adv_hist[4], r->emu_hist[0], r->emu_hist[1],
          r->emu_hist[2], r->emu_hist[3], r->emu_hist[4], r->max_waits,
          r->resyncs);
}

#define LCD_HZ (1e6 / VB_US)

int main(void)
{
   static const unsigned fast[] = { 2500, 4000, 6000, 8000, 10000 };
   unsigned i, total;
   run_t r;

   /* 1.5x on a console that can: exactly 3 frames per 2 vblanks, 2,1,2,1
    * emulated per interval, and every vblank a new picture that advances
    * 2,1,2,1 game frames -- never a repeat, never a jump. */
   for (i = 0; i < sizeof(fast) / sizeof(fast[0]); i++)
   {
      char what[48];
      r = run(PCFG_FF_1_5X, c_const, fast[i], 12.0, -1);
      snprintf(what, sizeof(what), "1.5x, %u us frames", fast[i]);
      show(what, &r);
      total = r.adv_hist[1] + r.adv_hist[2];
      assert(r.emu_fps > 1.5 * LCD_HZ - 0.2 && r.emu_fps < 1.5 * LCD_HZ + 0.2);
      assert(r.adv_hist[0] == 0 && total > 600);
      assert(r.adv_hist[1] + r.adv_hist[2] == total);
      assert(r.adv_hist[1] - r.adv_hist[2] + 1u <= 2u);   /* equal +-1 */
      assert(r.alt_breaks == 0);                         /* strictly 2,1,2,1 */
      assert(r.emu_hist[1] + r.emu_hist[2] == total);
      assert(r.pics_fps > LCD_HZ - 0.1);
      assert(r.max_waits <= 1);
   }
   /* the same with frame costs jittering +-30 % and a 20 ms spike every 37
    * frames (a ROM page load): still exactly 1.5x; no picture is repeated
    * and the advances stay 1-2 except around a spike. */
   r = run(PCFG_FF_1_5X, c_jitter, 6000, 12.0, -1);
   show("1.5x, 6 ms +-30 %", &r);
   assert(r.emu_fps > 1.5 * LCD_HZ - 0.2 && r.emu_fps < 1.5 * LCD_HZ + 0.2);
   assert(r.adv_hist[0] == 0 && r.adv_hist[3] == 0 && r.alt_breaks == 0);
   r = run(PCFG_FF_1_5X, c_spike, 4000, 12.0, -1);
   show("1.5x, 4 ms + spikes", &r);
   /* each spike leaves the ceiling's rule; the gate is re-armed then, so a
    * boundary it never drew is not a resync (EVT ff_win's resync stays a
    * real "lost boundaries" count) */
   assert(r.resyncs == 0);
   assert(r.emu_fps > 1.5 * LCD_HZ - 0.5);
   assert(r.adv_hist[0] + r.adv_hist[3] + r.adv_hist[4] + r.adv_hist[5] +
          r.adv_hist[6] + r.adv_hist[7] < r.adv_hist[1] / 20u);

   /* 2x: 2 frames every vblank, every picture 2 frames on */
   for (i = 0; i < 4; i++)
   {
      char what[48];
      r = run(PCFG_FF_2X, c_const, fast[i], 12.0, -1);
      snprintf(what, sizeof(what), "2x, %u us frames", fast[i]);
      show(what, &r);
      total = r.adv_hist[2];
      assert(r.emu_fps > 2.0 * LCD_HZ - 0.2 && r.emu_fps < 2.0 * LCD_HZ + 0.2);
      assert(total > 600 && r.emu_hist[2] == total);
      assert(r.adv_hist[0] + r.adv_hist[1] + r.adv_hist[3] + r.adv_hist[4] == 0);
   }

   /* A console too slow for the multiplier is not paced, and the gate draws
    * at most 60 pictures a second, never a gap of 3 vblanks. */
   r = run(PCFG_FF_1_5X, c_const, 14000, 12.0, -1);
   show("1.5x, 14 ms (cannot)", &r);
   assert(r.emu_fps > 1e6 / 14000 - 1.0 && r.max_waits == 0);
   assert(r.pics_fps <= 60.0 && r.adv_hist[0] < r.adv_hist[1] / 4u);
   assert(r.adv_hist[4] + r.adv_hist[5] + r.adv_hist[6] + r.adv_hist[7] == 0);

   /* 3x is the beta.4 behaviour: 3 frames a vblank, the gate's draws */
   for (i = 0; i < 3; i++)
   {
      char what[48];
      r = run(PCFG_FF_3X, c_const, fast[i], 12.0, -1);
      snprintf(what, sizeof(what), "3x, %u us frames", fast[i]);
      show(what, &r);
      assert(r.gate_diff == 0);
      if (fast[i] * 3u < 16000u)
         assert(r.emu_fps > 3.0 * LCD_HZ - 0.3 && r.emu_fps < 3.0 * LCD_HZ + 0.3);
   }

   /* Switching presets while held re-anchors: 3x -> 1.5x never blocks more
    * than one vblank, and settles into 1.5x's cadence. */
   r = run(PCFG_FF_1_5X, c_const, 4000, 10.0, PCFG_FF_3X);
   show("3x then 1.5x", &r);
   assert(r.max_waits <= 1 && r.alt_breaks == 0 && r.adv_hist[0] == 0);
   assert(r.emu_fps > 1.5 * LCD_HZ - 0.2 && r.emu_fps < 1.5 * LCD_HZ + 0.2);

   /* The paced branch (bit 2 off): frames per loop 1,2,1,2 / 2 / 3 */
   {
      static const struct { int mode; unsigned seq[4]; } p[] = {
         { PCFG_FF_1_5X, { 1, 2, 1, 2 } }, { PCFG_FF_2X, { 2, 2, 2, 2 } },
         { PCFG_FF_3X, { 3, 3, 3, 3 } },
      };
      unsigned k, j, sum;
      for (k = 0; k < 3; k++)
      {
         pcfg_ff_set_mode(p[k].mode);
         g_ffm_acc = 0;
         for (j = 0; j < 4; j++)
            assert((unsigned)mult_frames() == p[k].seq[j]);
         for (sum = 0, j = 0; j < 600; j++)
            sum += (unsigned)mult_frames();
         assert(sum * 10u == 600u * (unsigned)g_pcfg.ff_mult_x10);
      }
   }

   puts("PASS: 1.5x = 3 frames per 2 vblanks, pictures advance 2,1,2,1 every "
        "vblank; 2x = 2,2; jitter/spikes; too-slow falls to the gate; 3x "
        "draws exactly the gate's frames; preset switch re-anchors; paced "
        "loop 1,2 / 2 / 3");
   return 0;
}
