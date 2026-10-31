/* Host model of the paced main loop around the REAL low-latency code of
 * psp/main_psp.c (tools/tests/test_lowlat_policy.py extracts it unchanged
 * into lowlat.inc): lat_swap, the vblank wait's begin/step/end, the tier-1
 * policy, me_rend_present, me_rend_retire_if_idle, us_to_vblank.
 *
 * Modelled here, and only here: the PSP clock and vblank (59.94 Hz), the
 * NEXTFRAME flip (latches at the next vblank; the last submit before a
 * vblank wins), the GE (a blit occupies it for a while), the ME (a render
 * posted at the core's vcount-160 point finishes some time later), the
 * core's frame time, and the audio-ring nudge (a two-vblank wait every
 * ~287 frames, as at 59.94 vs 59.73 Hz).  The loop's own code between those
 * calls mirrors the paced branch of main() step by step.
 *
 * Each scenario runs twice from the same random stream, tier 0 (me_lowlat
 * 0) and tier 1 (me_lowlat 1), and the test checks the policy's contract:
 *
 *   C1  the picture never goes backwards, and no emulated frame is ever
 *       skipped (shown on no vblank) -- in either mode;
 *   C2  every frame's input poll lands on the SAME vblank in both modes
 *       (emulation cadence, hence anything the guest sees, is untouched);
 *   C3  tier 1 repeats a frame no more often than tier 0, give or take the
 *       one vblank the picture may sit ahead at the end;
 *   C4  where there is room (light games) tier 1 engages and the mean
 *       poll->latch drops to ~1 vblank; where there is none it stays at
 *       tier 0 without cost.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the PSP, modelled ------------------------------------------------ */
#define VB_US_X3 50050ull                  /* 3 * 16683.33 us */
static uint64_t t_now;                     /* us */
static unsigned vb_count;                  /* vblanks passed */
static uint64_t vb_next = 16683;           /* time of the next vblank */
static uint64_t ge_done;                   /* GE busy until */
static uint64_t me_done;                   /* ME busy until */
static int      me_busy;

/* display: frame ids */
static int  disp_frame = -1, pend_frame = -2, bb_frame = -1, bb_filled;
static unsigned disp_buf, pend_buf, cur_buf = 1;   /* "addresses" */
#define MAXF 20000
static int  shown_on_vb[MAXF * 3];         /* frame shown on vblank i */
static unsigned poll_vb[MAXF];             /* vblank count at frame N's poll */
static unsigned first_vb[MAXF];            /* first vblank frame N was shown */
static int      ever_shown[MAXF];
static unsigned n_vb_rec;

/* audio ring, in input samples: the core produces one frame's worth per
 * emulated frame (32768 / 59.7275 Hz), the output thread takes chunks of
 * 761 every 23.22 ms (1024 at 44.1 kHz) -- so the ring climbs ~1.9 a frame
 * and the pacer's nudge (level > RING_HIGH_WATER) fires every ~288 frames */
#define AUDIO_CHUNK_US 23220u
#define AUDIO_CHUNK    761u
static unsigned in_rate = 32768;
static unsigned g_ring_high_water = 4096;
#define RING_HIGH_WATER g_ring_high_water
static volatile unsigned ring_w, ring_r;
static unsigned ring_frac, starved;
static unsigned g_fh_nudge, g_ar_nudge;
static int g_gbl_audio_on, g_gbl_follow;
static uint64_t audio_next = AUDIO_CHUNK_US;
static void audio_produce(void)
{
   ring_frac += 54862u;            /* 548.62 x 100 */
   ring_w += ring_frac / 100u;
   ring_frac %= 100u;
}

static void on_vblank(void)
{
   vb_count++;
   if (pend_frame != -2)
   {
      disp_frame = pend_frame;
      disp_buf   = pend_buf;
      pend_frame = -2;
   }
   if (n_vb_rec < sizeof(shown_on_vb) / sizeof(shown_on_vb[0]))
      shown_on_vb[n_vb_rec++] = disp_frame;
   if (disp_frame >= 0 && disp_frame < MAXF && !ever_shown[disp_frame])
   {
      ever_shown[disp_frame] = 1;
      first_vb[disp_frame] = vb_count;
   }
   vb_next = (uint64_t)(vb_count + 1u) * VB_US_X3 / 3u;
}

static void advance_to(uint64_t t)
{
   for (;;)
   {
      if (audio_next <= t && audio_next <= vb_next)
      {
         t_now = audio_next;
         if (ring_w - ring_r < AUDIO_CHUNK)
         {
            starved++;
            ring_r = ring_w;
         }
         else
            ring_r += AUDIO_CHUNK;
         audio_next += AUDIO_CHUNK_US;
      }
      else if (vb_next <= t)
      {
         t_now = vb_next;
         on_vblank();
      }
      else
         break;
   }
   t_now = t;
}
static void advance(uint64_t d) { advance_to(t_now + d); }

static unsigned sceKernelGetSystemTimeLow(void) { return (unsigned)t_now; }
static unsigned sceDisplayGetVcount(void) { return vb_count; }
static int sceDisplayGetCurrentHcount(void) { return 0; }
static void sceKernelDelayThread(unsigned us) { advance(us + 20u); }
static void sceDisplayWaitVblankStart(void) { advance_to(vb_next + 30u); }

/* GE */
static unsigned blit_us_cur, osd_us = 120;
static int vid_gu_busy(void) { return t_now < ge_done; }
static void ge_add(unsigned us)
{
   ge_done = (ge_done > t_now ? ge_done : t_now) + us;
}

/* ME stages: the frame id each one holds */
static int g_me_rend = 1;
static int g_mer_stage_frame[2];
static void *g_mer_stage[2] = { &g_mer_stage_frame[0], &g_mer_stage_frame[1] };
static void vid_draw_prestaged(const void *staged, unsigned w, unsigned h)
{
   (void)w; (void)h;
   ge_add(blit_us_cur);
   bb_frame  = *(const int *)staged;
   bb_filled = 1;
}

static void vid_gu_flush(void)
{
   if (ge_done > t_now)
      advance_to(ge_done);          /* the CPU waits for the GE */
}
static unsigned g_flip_addr;
static int vid_swap(void)
{
   vid_gu_flush();
   if (!bb_filled)
      return 0;
   pend_frame = bb_frame;
   pend_buf   = cur_buf;
   g_flip_addr = cur_buf;
   cur_buf++;
   bb_filled = 0;
   return 1;
}
static int vid_flip_latched(void)
{
   return g_flip_addr && disp_buf == g_flip_addr;
}

static int me_host_idle(void) { return !me_busy || t_now >= me_done; }
static int me_host_render_refused(void) { return 0; }
static void me_rend_teardown(const char *w) { (void)w; assert(!"teardown"); }
static void mer_note_sum(void) { }

/* ---- what the extracted code refers to -------------------------------- */
static int g_drew, g_wake_hold, g_ff_gate, g_ff_mult, g_ff_uncapped;
static int g_mer_ready = -1, g_mer_pending = -1, g_mer_last_presented = -1;
static int g_mer_miss, g_mer_sameframe;
static unsigned g_fps_drawn, g_mer_sf_hit;
static unsigned g_vc_target;
#define FFW_DRAWN()     do { } while (0)
#define UISCRIPT_DUMP() do { } while (0)
#define LAB_HOOK(x)
static int osd_n;
static void osd_draw(void) { osd_n++; ge_add(osd_us); }
static int mer_rewind_busy(void) { return 0; }
static int link_session_any(void) { return 0; }
static int mer_sameframe_active(void) { return 0; }

static int g_net_up, g_mer_rend_seen;
static unsigned g_mer_post_us, g_mer_rend_n, g_mer_rend_us, g_mer_rend_max;
static unsigned g_mer_sf_miss, g_mer_sf_wouldhit;
static void fe_np_pump(void) { }
#define FE_LP_SET(x) do { } while (0)

#include "lowlat.inc"

/* ---- the scenario ----------------------------------------------------- */
typedef struct
{
   const char *name;
   unsigned work_us, work_jit;   /* core frame time: base + U(0, jit)     */
   unsigned heavy_pct, heavy_us; /* % of frames that take heavy_us extra  */
   unsigned phase;               /* >0: heavy only in alternate phases of this many frames */
   unsigned rend_us, rend_jit;   /* ME render after the post              */
   unsigned blit_us, blit_jit;
   unsigned vis_pct;             /* where in the frame the post happens   */
   int      expect_engage;       /* C4: 1 must engage (>= 90 %), 2 partly
                                    (misses force exits; mode 2 >= 35 %),
                                    0 must not, -1 any */
} scen;

static uint32_t rng;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

typedef struct
{
   unsigned frames, vb, hist[8], repeats, skipped, engaged, entries, exits;
   unsigned nudges, pulls, starved;
   double   mean;
   unsigned poll_vb[MAXF];
} result;

static void reset_world(int ll)
{
   t_now = 0; vb_count = 0; vb_next = 16683; ge_done = me_done = 0;
   ring_w = 3500; ring_r = 0; ring_frac = 0; starved = 0;
   g_ar_nudge = g_fh_nudge = 0;
   audio_next = AUDIO_CHUNK_US; g_ll_pull = 0;
   me_busy = 0;
   disp_frame = -1; pend_frame = -2; bb_frame = -1; bb_filled = 0;
   disp_buf = pend_buf = 0; cur_buf = 1; g_flip_addr = 0;
   memset(shown_on_vb, 0xff, sizeof(shown_on_vb));
   memset(ever_shown, 0, sizeof(ever_shown));
   n_vb_rec = 0;
   g_drew = 0; g_mer_ready = g_mer_pending = g_mer_last_presented = -1;
   g_vc_target = 0;
   memset(g_lat_hist, 0, sizeof(g_lat_hist));
   g_lat_over = 0;
   g_bb_new = 0; g_lat_seq = 0; g_lat_poll_vc = 0;
   g_vb_us = 0;
   g_ll_cfg = ll; g_ll_mode_h = 0; g_ll_iter_h = 0;
   ll_win_reset();
   g_ll_cool = 0; g_ll_blit_ema = 2000; g_ll_blit_dev = 250;
   g_ll_hit = g_ll_miss_me = g_ll_miss_ge = g_ll_to_h = g_ll_to_b = 0;
   g_ll_eng = g_ll_slot = g_ll_slot_fail = 0;
   g_flip_early = 1; g_mer_early_retire = 1;
}

/* the core's vcount-160 post (me_rend_frame steps 1-2, vispost) */
static void me_post(int frame, unsigned rend_us)
{
   if (g_mer_pending >= 0)
   {
      if (!me_host_idle())
         advance_to(me_done);       /* step 1 spins for a late render */
      g_mer_ready   = g_mer_pending;
      g_mer_pending = -1;
   }
   {
      int out = (g_mer_ready == 0) ? 1 : 0;
      g_mer_stage_frame[out] = frame;
      g_mer_pending = out;
      g_mer_stage_pvc[out] = g_lat_poll_vc;
      g_mer_stage_seq[out] = g_lat_seq;
      me_busy = 1;
      me_done = t_now + rend_us;
   }
}

static void run(const scen *s, int ll, unsigned frames, result *r)
{
   unsigned f, i;
   reset_world(ll);
   rng = 12345u;
   g_vb_us = 0;
   sceDisplayWaitVblankStart();
   g_vb_us = (unsigned)t_now;
   g_vc_target = vb_count;
   for (f = 0; f < frames; f++)
   {
      unsigned w   = s->work_us + (s->work_jit ? rnd() % s->work_jit : 0u);
      unsigned rd  = s->rend_us + (s->rend_jit ? rnd() % s->rend_jit : 0u);
      unsigned bl  = s->blit_us + (s->blit_jit ? rnd() % s->blit_jit : 0u);
      unsigned hv  = rnd() % 100u;
      unsigned vis;
      if (hv < s->heavy_pct && (!s->phase || (f / s->phase) % 2u))
         w += s->heavy_us;
      blit_us_cur = bl;

      /* loop top: main()'s own text (tier 0b retire, present / tier 1) */
#include "lowlat_top.inc"
      /* input poll */
      g_lat_poll_vc = sceDisplayGetVcount();
      g_lat_seq++;
      r->poll_vb[f] = g_lat_poll_vc;
      /* retro_run with the vcount-160 post */
      vis = w * s->vis_pct / 100u;
      advance(vis);
      me_post((int)f, rd);
      advance(w - vis);
      audio_produce();
      /* the paced branch: main()'s own text from the nudge to the swap */
      {
         unsigned want = 1u;
#include "lowlat_want.inc"
         if (g_drew)
            osd_draw();
         g_vc_target += want;
         if ((int)(sceDisplayGetVcount() - g_vc_target) > 4)
            g_vc_target = sceDisplayGetVcount();
#include "lowlat_wait.inc"
         g_drew = 0;
      }
      if (g_ll_iter_h)
         r->engaged++;
   }
   /* let the pipeline drain onto the screen */
   for (i = 0; i < 4; i++)
   {
      me_rend_retire_if_idle();
      me_rend_present();
      lat_swap(0);
      sceDisplayWaitVblankStart();
   }
   r->frames = frames;
   r->vb = vb_count;
   r->entries = g_ll_to_h;
   r->exits = g_ll_to_b;
   r->nudges = g_ar_nudge;
   r->pulls = g_ll_pull;
   r->starved = starved;
   memcpy(r->hist, g_lat_hist, sizeof(r->hist));
   {
      unsigned n = 0, sum = 0;
      for (i = 0; i < 8; i++)
      {
         n += g_lat_hist[i];
         sum += g_lat_hist[i] * i;
      }
      r->mean = n ? (double)sum / n : 0.0;
   }
   /* C1: monotonic, nothing skipped (after the first frame on screen) */
   {
      int last = -1;
      r->repeats = 0;
      for (i = 0; i < n_vb_rec; i++)
      {
         int fr = shown_on_vb[i];
         if (fr < 0)
            continue;
         if (fr < last)
         {
            printf("FAIL %s ll=%d: picture went back on vblank %u (%d after %d)\n",
                   s->name, ll, i, fr, last);
            exit(1);
         }
         if (fr == last)
            r->repeats++;
         last = fr;
      }
      r->skipped = 0;
      for (f = 1; f + 1 < frames && f < MAXF; f++)
         if (!ever_shown[f])
            r->skipped++;
   }
}

static unsigned failures;
static void check(int ok, const char *s, const char *what)
{
   if (!ok)
   {
      printf("FAIL %s: %s\n", s, what);
      failures++;
   }
}

int main(void)
{
   static const scen S[] = {
      /* name           work  jit  hv%  hv_us ph   rend  jit  blit jit vis eng */
      { "light",        5500, 800,   0,     0,  0, 3200, 400, 1500, 200, 70, 1 },
      { "light_slowblit",5500, 800,  0,     0,  0, 3200, 400, 2600, 900, 70, 1 },
      { "medium",       9500,1200,   2,  4000,  0, 3200, 600, 1500, 200, 70, 2 },
      { "hns_like",     9000,1500,   8,  4500,  0, 3200, 600, 1500, 300, 70, -1 },
      { "bursty",       8000,1000,  60,  6000,600, 3200, 600, 1500, 300, 70, 2 },
      { "heavy",       13200, 900,   0,     0,  0, 4000, 800, 1500, 200, 70, 0 },
      { "ub_like",     11500,1500,  10,  2500,  0, 7000,1000, 1500, 200, 60, -1 },
      { "sub60",       19000,2000,   0,     0,  0, 4500, 500, 1500, 200, 70, 0 },
   };
   static result r0, r1, r2;
   unsigned k, f;
   const unsigned frames = 12000;

   for (k = 0; k < sizeof(S) / sizeof(S[0]); k++)
   {
      const scen *s = &S[k];
      memset(&r0, 0, sizeof(r0));
      memset(&r1, 0, sizeof(r1));
      memset(&r2, 0, sizeof(r2));
      run(s, 0, frames, &r0);
      run(s, 1, frames, &r1);
      run(s, 2, frames, &r2);
      printf("%-15s t0 %.2f rep %u skip %u nudge %u | t1 %.2f rep %u skip %u "
             "eng %u%% in %u out %u | t2 %.2f rep %u skip %u eng %u%% "
             "in %u pull %u nudge %u starved %u/%u\n",
             s->name, r0.mean, r0.repeats, r0.skipped, r0.nudges,
             r1.mean, r1.repeats,
             r1.skipped, r1.engaged * 100u / frames, r1.entries, r1.exits,
             r2.mean, r2.repeats, r2.skipped, r2.engaged * 100u / frames,
             r2.entries, r2.pulls, r2.nudges, r0.starved, r2.starved);
      /* mode 2: the same contract, except that an early nudge moves the
       * poll vblanks after it (by design: a nudge re-timed, none added) */
      check(r2.skipped <= r0.skipped, s->name, "mode 2 skipped a frame tier 0 showed");
      check(r2.repeats <= r0.repeats + 1u, s->name, "mode 2 repeats more frames than tier 0");
      check(r2.nudges <= r0.nudges + 1u && r2.nudges + 1u >= r0.nudges, s->name,
            "mode 2 changed the number of nudges");
      check(r2.starved <= r0.starved, s->name, "mode 2 starved the audio");
      check(r2.mean <= r1.mean + 0.01, s->name, "mode 2 is slower than mode 1");
      if (s->expect_engage == 0)
         check(r2.entries == 0, s->name, "mode 2 engaged with no room");
      /* C1 */
      check(r1.skipped <= r0.skipped, s->name, "tier 1 skipped a frame tier 0 showed");
      /* C2 */
      for (f = 0; f < frames; f++)
         if (r0.poll_vb[f] != r1.poll_vb[f])
         {
            printf("  frame %u polled on vblank %u (t0) vs %u (t1)\n",
                   f, r0.poll_vb[f], r1.poll_vb[f]);
            check(0, s->name, "emulation cadence moved");
            break;
         }
      /* C3 */
      check(r1.repeats <= r0.repeats + 1u, s->name, "tier 1 repeats more frames than tier 0");
      /* C4 */
      if (s->expect_engage == 1)
      {
         check(r1.engaged * 100u / frames >= 90u, s->name, "tier 1 did not engage");
         check(r1.mean < r0.mean - 0.8, s->name, "tier 1 did not cut latency");
      }
      if (s->expect_engage == 2)
      {
         check(r1.mean < r0.mean - 0.1, s->name, "tier 1 did not cut latency");
         check(r2.engaged * 100u / frames >= 35u, s->name, "mode 2 did not engage");
         check(r2.mean < r0.mean - 0.3, s->name, "mode 2 did not cut latency");
      }
      if (s->expect_engage == 0)
         check(r1.entries == 0 && r1.mean <= r0.mean + 0.01, s->name,
               "tier 1 engaged with no room");
      check(r1.mean <= r0.mean + 0.01, s->name, "tier 1 raised latency");
   }
   if (failures)
   {
      printf("%u failure(s)\n", failures);
      return 1;
   }
   printf("lowlat policy: all scenarios pass\n");
   return 0;
}
