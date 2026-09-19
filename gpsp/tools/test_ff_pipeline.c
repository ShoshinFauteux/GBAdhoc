/* Tests the real me_rend_frame()/me_rend_present() functions extracted by
 * run_ff_tests.py. Only platform operations and surrounding state are stubbed.
 * The fake ME takes 12 ms, deliberately longer than the old 9 ms drop budget. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"

#define MER_INPUT_US 50000u
#define MER_RETIRE_US 9000u
#define MER_MISS_LIMIT 120
#define VRAM_DIRTY_PAGES 96
#define ME_UNCACHED(p) 0u
#define fe_evt(...) ((void)0)
psp_config g_pcfg;
static int g_me_rend, g_ff_unlimited, g_ff_draw_all, g_ff_uncapped, g_ff_mult;
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

#include "ff_pipeline.inc"

static void reset(void)
{
   g_me_rend = 1;
   g_mer_pending = g_mer_ready = g_mer_last_presented = -1;
   g_mer_cur = 0;
   me_capture_buf = g_mer_cap[0];
   g_ff_unlimited = g_ff_draw_all = g_ff_uncapped = g_ff_mult = 0;
   g_mer_drops = g_fps_drawn = g_mer_miss = g_mer_census = 0;
   g_mer_watch_us = now = done_at = posts = watchdogs = blits = teardown = 0;
   latency = 12000;
}

int main(void)
{
   unsigned i, start;
   int cur;
   reset();
   g_ff_uncapped = g_ff_unlimited = 1;
   me_rend_frame(1);
   assert(posts == 1 && g_mer_pending == 0);
   cur = g_mer_cur;
   start = now;
   me_rend_frame(1);
   assert(now - start < 1000); /* Busy ME must not impose the old 9 ms wait. */
   assert(posts == 1 && g_mer_drops == 1 && g_mer_cur == cur);
   assert(me_capture_buf == g_mer_cap[cur]);
   assert(watchdogs == 0); /* Tiny guest frames are not heartbeat failures. */
   now = done_at;
   me_rend_frame(1);
   assert(posts == 2 && g_fps_drawn == 1);
   me_rend_present();
   me_rend_present();
   assert(blits == 1); /* Do not spend GPU work on duplicate frames. */

   /* Switch into Smooth while an Unlimited render is still in flight. */
   g_ff_unlimited = 0;
   g_ff_draw_all = 1;
   g_mer_drops = 0;
   for (i = 0; i < 100; i++)
      me_rend_frame(1);
   assert(posts == 102 && g_fps_drawn == 101 && !g_mer_drops && !teardown);
   assert(me_capture_buf == g_mer_cap[g_mer_cur]);

   reset();
   g_ff_mult = 1;
   me_rend_frame(1);
   me_rend_frame(1);
   assert(posts == 1 && g_mer_drops == 1); /* 3x retains its prior late policy. */

   reset();
   g_ff_draw_all = g_ff_uncapped = 1;
   latency = 1000000;
   me_rend_frame(1);
   start = now;
   me_rend_frame(1);
   assert(teardown == 1 && !g_me_rend && posts == 1);
   assert(now - start >= MER_INPUT_US && now - start < MER_INPUT_US + 1000);

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

   puts("PASS: busy-ME skipping, all-frame Smooth, mode switch, 3x policy, "
        "bounded fallback, retirement inert while same-frame is off");
   return 0;
}
