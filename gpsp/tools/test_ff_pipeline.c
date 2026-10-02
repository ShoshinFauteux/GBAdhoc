/* Tests the real me_rend_frame()/me_rend_present() functions extracted by
 * run_ff_tests.py, and the real FF draw gate (psp/ff_gate.h, included as is).
 * Only platform operations and surrounding state are stubbed.
 * The fake ME takes 12 ms, deliberately longer than the old 9 ms drop budget. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
#include "../psp/ff_gate.h"

#define MER_INPUT_US 50000u
#define MER_RETIRE_US 9000u
#define MER_MISS_LIMIT 120
#define VRAM_DIRTY_PAGES 96
#define ME_UNCACHED(p) 0u
#define fe_evt(...) ((void)0)
psp_config g_pcfg;
static int g_me_rend, g_ff_gate, g_ff_uncapped, g_ff_mult;
static int g_mer_pending, g_mer_ready, g_mer_cur, g_mer_last_presented, g_drew;
static int g_mer_sh_on, g_mer_dirty_cfg, g_mer_full_copy, g_mer_sameframe;
static unsigned g_mer_frames, g_mer_miss, g_mer_drops, g_fps_drawn;
static unsigned g_mer_wait_us, g_mer_wait_max, g_mer_post_us, g_mer_rend_seen;
static unsigned g_mer_census, g_mer_sh_us, g_mer_sh_max, g_mer_watch_us;
static unsigned g_mer_rend_n, g_mer_rend_us, g_mer_rend_max;
static unsigned g_vis_n, g_vis_us, g_vis_max, g_mer_wb_pages, g_mer_wb_n;
static unsigned g_mer_sf_hit, g_mer_sf_miss, g_mer_sf_noloop;
static unsigned g_mer_sf_wouldhit;
static int cap[2], stage[2];
static int *g_mer_cap[2] = {&cap[0], &cap[1]}, *me_capture_buf;
static int *g_mer_stage[2] = {&stage[0], &stage[1]};
static unsigned char vram_clean[96];
static unsigned now, done_at, latency, posts, watchdogs, blits;
static int teardown;

static unsigned sceKernelGetSystemTimeLow(void) { now += 100; return now; }
static int me_host_idle(void) { return now >= done_at; }
static int me_host_up(void) { return g_me_rend; }
static int me_host_input_done(void) { return 1; }
static void mer_note_sum(void) {}
static int me_host_watchdog_frame(void) { watchdogs++; return 0; }
static void me_rend_teardown(const char *why)
{
   assert(strcmp(why, "ff_render_wedge") == 0);
   teardown++;
   g_me_rend = 0;
}
static void me_rend_fill_desc(int out)
{
   assert(me_host_idle());
   assert(out != g_mer_ready);
}
static int me_host_post_render(unsigned desc)
{
   (void)desc;
   assert(me_host_idle());
   posts++;
   done_at = now + latency;
   return 0;
}
static void vid_draw_prestaged(const int *pix, unsigned w, unsigned h)
{
   assert(pix == g_mer_stage[g_mer_ready]);
   assert(w == 240 && h == 160);
   blits++;
}

/* SAME-FRAME RETIREMENT IS NOT EXERCISED BY THIS TEST, and this stub is what
 * says so out loud.
 *
 * run_ff_tests.py builds ff_pipeline.inc by slicing main_psp.c from
 * me_rend_present() up to the "Called from plat_video_frame" comment marker, so
 * me_rend_retire_if_idle() -- which sits between them -- is compiled in here
 * whether or not it is tested.  Returning 0 makes it inert, which is exactly
 * its state in every shipping profile: `me_sameframe` is a harness-only ini
 * key, and ADR-0067 points the harness ini at a path that cannot exist in a
 * playable build, so g_mer_sameframe is always 0 there.
 *
 * Every existing assertion below was written against that inert state and is
 * unchanged.  A future test that wants to cover retirement should return 1 here
 * and assert on g_mer_sf_hit / g_mer_ready itself. */
static int mer_sameframe_active(void)
{
   return 0;
}

/* main_psp.c's loop-phase breadcrumb (a store to g_loop_phase in instrumented
 * builds, nothing otherwise).  The sliced functions may use it; this test does
 * not observe it. */
#ifndef LOOP_PHASE
#define LOOP_PHASE(p) do { (void)(p); } while (0)
#endif
/* EVT ff_win's drawn counter: compiled out of a player build, as here. */
#ifndef FFW_DRAWN
#define FFW_DRAWN() ((void)0)
#endif

#include "ff_pipeline.inc"

static void reset(void)
{
   g_me_rend = 1;
   g_mer_pending = g_mer_ready = g_mer_last_presented = -1;
   g_mer_cur = 0;
   me_capture_buf = g_mer_cap[0];
   g_ff_gate = g_ff_uncapped = g_ff_mult = 0;
   g_mer_drops = g_fps_drawn = g_mer_miss = g_mer_census = 0;
   g_mer_watch_us = now = done_at = posts = watchdogs = blits = teardown = 0;
   latency = 12000;
}

/* Drive the gate with frames of a fixed cost for `secs` seconds; return the
 * number drawn and report the largest gap between two draws.  No two draws
 * may come closer than one frame. */
static unsigned gate_run(unsigned period, unsigned frame_us, unsigned secs,
                         unsigned *max_gap)
{
   ff_gate_t g;
   uint64_t t = 1000000, last = 0;
   unsigned drawn = 0;
   ff_gate_arm(&g, period);
   *max_gap = 0;
   while (t < 1000000ull + secs * 1000000ull)
   {
      if (ff_gate_frame(&g, t))
      {
         if (last && (unsigned)(t - last) > *max_gap)
            *max_gap = (unsigned)(t - last);
         if (last)
            assert(t - last >= frame_us);
         last = t;
         drawn++;
      }
      t += frame_us;
   }
   return drawn;
}

int main(void)
{
   unsigned i, start, n, gap;
   int cur;

   /* ---- the draw gate itself ------------------------------------------ */
   /* Fast emulation: 30 / 60 draws a second whatever the frame cost -- the
    * grid does not drift low by a fraction of a frame per period. */
   n = gate_run(FF_GATE_PERIOD_UNLIMITED_US, 5000, 10, &gap);
   assert(n >= 299 && n <= 301 && gap <= 33333 + 5000);
   n = gate_run(FF_GATE_PERIOD_SMOOTH_US, 5000, 10, &gap);
   assert(n >= 599 && n <= 601 && gap <= 16667 + 5000);
   n = gate_run(FF_GATE_PERIOD_SMOOTH_US, 4100, 10, &gap);
   assert(n >= 599 && n <= 601);
   n = gate_run(FF_GATE_PERIOD_UNLIMITED_US, 1700, 10, &gap);
   assert(n >= 299 && n <= 301 && gap <= 33333 + 1700);
   /* Slower than the draw rate: every frame is drawn. */
   n = gate_run(FF_GATE_PERIOD_SMOOTH_US, 20000, 10, &gap);
   assert(n == 500);
   n = gate_run(FF_GATE_PERIOD_UNLIMITED_US, 40000, 10, &gap);
   assert(n == 250);
   /* Off: every frame. */
   n = gate_run(0, 5000, 1, &gap);
   assert(n == 200);
   {
      /* A draw lands at or just after its boundary: the frame drawn is the
       * first whose predicted END passes it, so with a steady cost the drawn
       * frame's end is in [boundary, boundary + one frame). */
      ff_gate_t g;
      uint64_t t = 5000000, boundary;
      ff_gate_arm(&g, FF_GATE_PERIOD_SMOOTH_US);
      assert(ff_gate_frame(&g, t));            /* first frame: drawn */
      boundary = t + FF_GATE_PERIOD_SMOOTH_US;
      for (i = 0; i < 2000; i++)
      {
         t += 3000;
         if (ff_gate_frame(&g, t))
         {
            assert(t + 3000 >= boundary && t + 3000 < boundary + 3000);
            boundary += FF_GATE_PERIOD_SMOOTH_US;
         }
      }
      /* A stall (menu, state load) neither bursts nor is repaid: after it,
       * draws resume one period apart. */
      assert(g.resyncs == 0);                  /* steady: never behind */
      t += 2000000;
      assert(ff_gate_frame(&g, t));
      assert(g.resyncs == 1);                  /* the stall: re-anchored */
      {
         uint64_t last = t;
         unsigned draws = 0;
         for (i = 0; i < 200; i++)
         {
            t += 3000;
            if (ff_gate_frame(&g, t))
            {
               assert(t - last >= FF_GATE_PERIOD_SMOOTH_US - 3000);
               last = t;
               draws++;
            }
         }
         assert(draws >= 34 && draws <= 37);   /* 600 ms at 60/s */
      }
      /* Re-arming (a preset change) draws the next frame at once. */
      ff_gate_arm(&g, FF_GATE_PERIOD_UNLIMITED_US);
      assert(ff_gate_frame(&g, t + 3000));
      assert(!ff_gate_frame(&g, t + 6000));
   }

   /* ---- the ME pipeline under the gate --------------------------------- */
   reset();
   g_ff_uncapped = g_ff_gate = 1;
   me_rend_frame(1);                      /* a drawn frame posts */
   assert(posts == 1 && g_mer_pending == 0);
   cur = g_mer_cur;
   start = now;
   me_rend_frame(0);                      /* a gated-out frame, ME busy */
   assert(now - start < 1000);            /* it never waits */
   assert(posts == 1 && g_mer_drops == 0 && g_mer_cur == cur && !teardown);
   assert(me_capture_buf == g_mer_cap[cur]);
   assert(watchdogs == 0); /* Tiny guest frames are not heartbeat failures. */
   now = done_at;
   me_rend_frame(0);                      /* gated-out frame, ME done: retire */
   assert(posts == 1 && g_fps_drawn == 1 && g_mer_pending < 0);
   me_rend_present();
   me_rend_present();
   assert(blits == 1); /* Do not spend GPU work on duplicate frames. */
   /* A drawn frame with the previous render still running waits for it
    * (bounded) and posts: drawn frames are never dropped. */
   me_rend_frame(1);
   assert(posts == 2);
   start = now;
   me_rend_frame(1);
   assert(posts == 3 && g_fps_drawn == 2 && !g_mer_drops && !teardown);
   assert(now - start >= latency - 200 && now - start < MER_INPUT_US);
   assert(me_capture_buf == g_mer_cap[g_mer_cur]);
   /* 100 drawn frames back to back (emulation slower than the gate period):
    * every one is posted. */
   for (i = 0; i < 100; i++)
      me_rend_frame(1);
   assert(posts == 103 && g_fps_drawn == 102 && !g_mer_drops && !teardown);

   reset();
   g_ff_mult = 1;
   me_rend_frame(1);
   me_rend_frame(1);
   assert(posts == 1 && g_mer_drops == 1); /* 3x retains its prior late policy. */
   me_rend_present();
   assert(blits == 0);   /* nothing retired yet */
   now = done_at;
   me_rend_frame(1);
   me_rend_present();
   me_rend_present();
   assert(blits == 2);   /* 3x re-presents every loop, as before */

   /* A stuck ME exits the gate through the bounded fallback -- on a drawn
    * frame only; a gated-out frame just keeps looking. */
   reset();
   g_ff_gate = g_ff_uncapped = 1;
   latency = 1000000;
   me_rend_frame(1);
   for (i = 0; i < 50; i++)
      me_rend_frame(0);
   assert(!teardown && g_me_rend);
   start = now;
   me_rend_frame(1);
   assert(teardown == 1 && !g_me_rend && posts == 1);
   assert(now - start >= MER_INPUT_US && now - start < MER_INPUT_US + 1000);

   /* Normal play is untouched: a late render is dropped after the 9 ms
    * budget and does not tear down below the miss limit. */
   reset();
   me_rend_frame(1);
   start = now;
   me_rend_frame(1);
   assert(posts == 1 && g_mer_drops == 1 && !teardown);
   assert(now - start >= MER_RETIRE_US && now - start < MER_RETIRE_US + 1000);

   /* Same-frame retirement is INERT unless a harness enables it.  This pins the
    * shipping state: with mer_sameframe_active() false, retirement must not
    * promote a stage, touch the counters, or present anything.  It is not
    * coverage of retirement working -- see the stub above for why that needs a
    * different test. */
   reset();
   g_me_rend = 1;
   g_mer_pending = 0;
   {
      unsigned hits = g_mer_sf_hit, drawn = g_fps_drawn;
      me_rend_retire_if_idle();
      assert(g_mer_pending == 0 && g_mer_ready == -1);
      assert(g_mer_sf_hit == hits && g_fps_drawn == drawn);
   }

   puts("PASS: draw gate 30/60 on a fixed grid, slow/stall/re-arm; gated "
        "frames never wait or drop; drawn frames always post; no duplicate "
        "blits; 3x and 1x policies unchanged; bounded fallback; retirement "
        "inert while same-frame is off");
   return 0;
}
