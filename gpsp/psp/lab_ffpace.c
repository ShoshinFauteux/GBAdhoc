/* lab_ffpace.c -- fast-forward presentation cadence instrument (see lab_ffpace.h).
 * Lab builds only (LAB_HARNESS=1 adds this object). */
#ifdef LAB_HARNESS
#include <string.h>
#include <stdio.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include "fe_evt.h"
#include "fe_host.h"
#include "fe_util.h"
#include "lab_ffpace.h"

#define FFP_NTAG      4
#define FFP_TRACE     3600          /* per-vblank advance digits kept (60 s) */
#define FFP_SETTLE_US 2000000u      /* FF engaged this long before measuring */

/* Everything the observer counts, all unsigned so a window is a plain diff. */
typedef struct
{
   unsigned vb;        /* vblanks seen while measuring */
   unsigned miss;      /* vblanks the observer slept through (woke late) */
   unsigned shown;     /* displayed-buffer changes (a flip latched) */
   unsigned chg;       /* ...that showed a LATER game frame (new picture) */
   unsigned dup;       /* ...that showed the same game frame again */
   unsigned back;      /* ...that showed an EARLIER game frame */
   unsigned drop;      /* submitted flips never displayed (overwritten) */
   unsigned untag;     /* changes to a buffer with no tag (not ours) */
   unsigned raw[5];    /* vblanks between buffer changes 1,2,3,4,5+ */
   unsigned ivl[6];    /* vblanks between NEW pictures 1,2,3,4,5,6+ */
   unsigned gfc[10];   /* game frames per new picture [1..8], [9] = 9+ */
   unsigned lat[4];    /* latch vblank - submit vcount 0,1,2,3+ */
   unsigned m2[4][9];  /* new picture: interval 1,2,3,4+ x frames 1..8,9+ */
   unsigned adv_s;     /* sum over vblanks of frames advanced at that vblank */
   unsigned adv_s2;    /* ...of its square */
   unsigned adv_jerk;  /* sum |adv(v) - adv(v-1)| */
} ffp_obs;
#define FFP_NU (sizeof(ffp_obs) / sizeof(unsigned))

static volatile struct { unsigned addr, frame, vc, seq; } g_tag[FFP_NTAG];
static unsigned g_tag_rr, g_seq;
static volatile int g_on, g_run, g_meas;
static ffp_obs g_o;                       /* written by the observer only */
static ffp_obs g_prev, g_start;           /* main thread snapshots */
static char g_trace[FFP_TRACE];
static volatile unsigned g_trace_n;
static unsigned g_trace_out;
static int g_every = 300, g_clock;

/* main-thread side */
static const volatile int *g_me_on, *g_me_shown;
static unsigned g_stage_fr[4];
static unsigned g_ff_t, g_w_t, g_w_frame, g_w_loops, g_w_flips, g_win;
static unsigned g_w_mode[4];              /* loops in gate / mult / uncapped / other */
static unsigned g_w_me;
static unsigned g_mr_n, g_mr_us, g_mr_max, g_ms_us, g_ms_max, g_ms_n1;
static unsigned g_t_start, g_f_start, g_flips_tot;

static void snap(ffp_obs *d)
{
   memcpy(d, (const void *)&g_o, sizeof(*d));
}

static int ffp_thread(SceSize args, void *argp)
{
   unsigned last_addr = 0, last_vc = 0, last_seq = 0, last_frame = 0;
   unsigned last_chg_vc = 0, prev_vc, prev_adv = 0;
   int have = 0, have_chg = 0, was = 0;
   (void)args; (void)argp;
   prev_vc = sceDisplayGetVcount();
   while (g_run)
   {
      void *top = NULL;
      int w, fmt, m;
      unsigned vc, addr, adv = 0;
      sceDisplayWaitVblankStart();
      vc = sceDisplayGetVcount();
      if (sceDisplayGetFrameBuf(&top, &w, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE) != 0)
      {
         prev_vc = vc;
         continue;
      }
      addr = (unsigned)top & 0x1fffffffu;
      m = g_meas;
      if (m && !was)
      {
         have_chg = 0;               /* first new picture is the baseline */
         prev_adv = 0;
      }
      was = m;
      if (m)
      {
         g_o.vb++;
         if (vc - prev_vc > 1u && vc - prev_vc < 1000u)
            g_o.miss += vc - prev_vc - 1u;
      }
      prev_vc = vc;
      if (addr != last_addr)
      {
         int i, t = -1;
         for (i = 0; i < FFP_NTAG; i++)
            if (g_tag[i].addr == addr)
            {
               t = i;
               break;
            }
         if (t < 0)
         {
            if (m)
               g_o.untag++;
            have = 0;
         }
         else
         {
            unsigned seq = g_tag[t].seq, fr = g_tag[t].frame, svc = g_tag[t].vc;
            if (m && have)
            {
               unsigned d = vc - last_vc, l = vc - svc;
               if (d < 1u)
                  d = 1u;
               g_o.shown++;
               g_o.raw[d >= 5u ? 4 : d - 1u]++;
               if (seq - last_seq > 1u && seq - last_seq < 1000u)
                  g_o.drop += seq - last_seq - 1u;
               g_o.lat[l > 3u ? 3 : l]++;
               if ((int)(fr - last_frame) > 0)
               {
                  unsigned gf = fr - last_frame;
                  adv = gf;
                  g_o.chg++;
                  g_o.gfc[gf >= 9u ? 9 : gf]++;
                  if (have_chg)
                  {
                     unsigned iv = vc - last_chg_vc;
                     if (iv < 1u)
                        iv = 1u;
                     g_o.ivl[iv >= 6u ? 5 : iv - 1u]++;
                     g_o.m2[iv >= 4u ? 3 : iv - 1u][gf >= 9u ? 8 : gf - 1u]++;
                  }
                  last_chg_vc = vc;
                  have_chg = 1;
                  last_frame = fr;
               }
               else if (fr == last_frame)
                  g_o.dup++;
               else
               {
                  g_o.back++;
                  last_frame = fr;
               }
            }
            else
            {
               last_frame = fr;
               last_chg_vc = vc;
               have_chg = m;
            }
            last_seq = seq;
            have = 1;
         }
         last_addr = addr;
         last_vc = vc;
      }
      if (m)
      {
         unsigned n = g_trace_n;
         g_o.adv_s += adv;
         g_o.adv_s2 += adv * adv;
         g_o.adv_jerk += adv > prev_adv ? adv - prev_adv : prev_adv - adv;
         prev_adv = adv;
         if (n < FFP_TRACE)
         {
            g_trace[n] = (char)(adv >= 9u ? '9' : '0' + adv);
            g_trace_n = n + 1u;
         }
      }
   }
   return 0;
}

int lab_ffpace_boot(const char *ini, int apply, const volatile int *me_rend,
                    const volatile int *me_shown)
{
   long pace = fe_ini_get_int(ini, "lab_pace", 0);
   long every = fe_ini_get_int(ini, "lab_pace_every", 300);
   int th;
   if (pace <= 0)
      return 0;
   g_every = every >= 30 && every <= 36000 ? (int)every : 300;
   g_clock = pace >= 100 && pace <= 333 ? (int)pace : 0;
   g_me_on = me_rend;
   g_me_shown = me_shown;
   g_on = g_run = 1;
   th = sceKernelCreateThread("lab_ffpace", ffp_thread, 0x11, 0x1000,
                              THREAD_ATTR_USER, NULL);
   if (th >= 0)
      sceKernelStartThread(th, 0, NULL);
   else
      g_on = g_run = 0;
   fe_evt("ffpace boot pace=%ld every_vb=%d clock_want=%d thread=%d apply=%d",
          pace, g_every, g_clock, th, apply);
   if (g_clock && apply)
   {
      int rc = scePowerSetClockFrequency(g_clock, g_clock, g_clock / 2);
      fe_evt("ffpace clock want=%d rc=%d now=%d/%d", g_clock, rc,
             scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
   }
   return g_clock;
}

void lab_ffpace_flip(unsigned phys_addr)
{
   unsigned fr;
   int i, t = -1;
   if (!g_on)
      return;
   /* ME renderer: the picture is the last-presented stage's render;
    * otherwise the CPU renderer drew the current frame. */
   if (g_me_on && *g_me_on && g_me_shown && *g_me_shown >= 0 && *g_me_shown < 4)
      fr = g_stage_fr[*g_me_shown];
   else
      fr = fe_host_frame_count();
   phys_addr &= 0x1fffffffu;
   for (i = 0; i < FFP_NTAG; i++)
      if (g_tag[i].addr == phys_addr)
      {
         t = i;
         break;
      }
   if (t < 0)
   {
      t = (int)g_tag_rr;
      g_tag_rr = (g_tag_rr + 1u) % FFP_NTAG;
   }
   /* Written before the flip is submitted, and never for the displayed
    * buffer (vid_swap never submits it), so the observer cannot read a
    * half-written tag for the picture on screen. */
   g_tag[t].addr  = 0;
   g_tag[t].frame = fr;
   g_tag[t].vc    = sceDisplayGetVcount();
   g_tag[t].seq   = ++g_seq;
   g_tag[t].addr  = phys_addr;
   if (g_meas)
   {
      g_w_flips++;
      g_flips_tot++;
   }
}

void lab_ffpace_me_post(int stage, unsigned frame)
{
   if (g_on && stage >= 0 && stage < 4)
      g_stage_fr[stage] = frame;
}

void lab_ffpace_me_retire(unsigned post_us, unsigned spin_us)
{
   unsigned d;
   if (!g_on || !g_meas || !post_us)
      return;
   d = (unsigned)sceKernelGetSystemTimeLow() - post_us;
   if (d > 1000000u)
      return;
   g_mr_n++;
   g_mr_us += d;
   if (d > g_mr_max)
      g_mr_max = d;
   g_ms_us += spin_us;
   if (spin_us > g_ms_max)
      g_ms_max = spin_us;
   if (spin_us >= 1000u)
      g_ms_n1++;
}

static void hist(char *b, size_t n, const unsigned *h, int k)
{
   int i, p = 0;
   b[0] = 0;
   for (i = 0; i < k && p < (int)n - 12; i++)
      p += snprintf(b + p, n - p, "%s%u", i ? "," : "", h[i]);
}

static void emit_obs(const char *tag, const ffp_obs *d, unsigned frames,
                     unsigned wall_us, unsigned loops, unsigned flips)
{
   char raw[64], ivl[64], gfc[96], lat[48];
   unsigned mean = d->vb ? d->adv_s * 100u / d->vb : 0;
   unsigned long long ex2 = d->vb ? (unsigned long long)d->adv_s2 * 10000ull / d->vb : 0;
   unsigned long long m2 = (unsigned long long)mean * mean;
   unsigned sd = 0, r;
   if (ex2 > m2)
   {
      /* integer sqrt of (E[x^2] - E[x]^2) in hundredths */
      unsigned long long v = ex2 - m2, x = 0, bit = 1ull << 40;
      while (bit > v)
         bit >>= 2;
      while (bit)
      {
         if (v >= x + bit)
         {
            v -= x + bit;
            x = (x >> 1) + bit;
         }
         else
            x >>= 1;
         bit >>= 2;
      }
      sd = (unsigned)x;
   }
   r = wall_us ? (unsigned)((unsigned long long)frames * 10000000ull / wall_us) : 0;
   hist(raw, sizeof(raw), d->raw, 5);
   hist(ivl, sizeof(ivl), d->ivl, 6);
   hist(gfc, sizeof(gfc), d->gfc + 1, 9);
   hist(lat, sizeof(lat), d->lat, 4);
   fe_evt("%s n=%u clk=%d/%d mode=%u/%u/%u/%u me=%u vb=%u fps10=%u loops=%u "
          "flips=%u shown=%u new=%u dup=%u back=%u drop=%u untag=%u miss=%u "
          "ivl=%s gfc=%s raw=%s lat=%s adv_x100=%u sd_x100=%u jerk_x100=%u "
          "mer_n=%u mer_mean_us=%u mer_max_us=%u spin_mean_us=%u spin_max_us=%u "
          "spin_ge1ms=%u",
          tag, g_win, scePowerGetCpuClockFrequency(),
          scePowerGetBusClockFrequency(), g_w_mode[0], g_w_mode[1],
          g_w_mode[2], g_w_mode[3], g_w_me, d->vb, r, loops, flips, d->shown,
          d->chg, d->dup, d->back, d->drop, d->untag, d->miss, ivl, gfc, raw,
          lat, mean, sd, d->vb ? d->adv_jerk * 100u / d->vb : 0,
          g_mr_n, g_mr_n ? g_mr_us / g_mr_n : 0, g_mr_max,
          g_mr_n ? g_ms_us / g_mr_n : 0, g_ms_max, g_ms_n1);
}

static void emit_trace(int all)
{
   unsigned n = g_trace_n;
   while (g_trace_out + 150u <= n || (g_trace_out < n && (all || n >= FFP_TRACE)))
   {
      char b[160];
      unsigned k = n - g_trace_out;
      if (k > 150u)
         k = 150u;
      memcpy(b, g_trace + g_trace_out, k);
      b[k] = 0;
      fe_evt("ffpace_trace at=%u adv=%s", g_trace_out, b);
      g_trace_out += k;
   }
}

void lab_ffpace_loop(int ff_user, int gate, int mult, int uncapped, int me_rend)
{
   unsigned now;
   if (!g_on)
      return;
   now = (unsigned)sceKernelGetSystemTimeLow();
   if (!ff_user)
   {
      if (g_meas)
         fe_evt("ffpace stop reason=ff_off");
      g_meas = 0;
      g_ff_t = 0;
      return;
   }
   if (!g_ff_t)
      g_ff_t = now ? now : 1u;
   if (!g_meas)
   {
      if (now - g_ff_t < FFP_SETTLE_US)
         return;
      snap(&g_prev);
      g_start = g_prev;
      g_w_t = g_t_start = now;
      g_w_frame = g_f_start = fe_host_frame_count();
      g_w_loops = g_w_flips = g_w_me = 0;
      memset(g_w_mode, 0, sizeof(g_w_mode));
      g_mr_n = g_mr_us = g_mr_max = g_ms_us = g_ms_max = g_ms_n1 = 0;
      g_meas = 1;
      fe_evt("ffpace start f=%u clk=%d/%d gate=%d mult=%d uncapped=%d me=%d",
             g_w_frame, scePowerGetCpuClockFrequency(),
             scePowerGetBusClockFrequency(), gate, mult, uncapped, me_rend);
      return;
   }
   g_w_loops++;
   g_w_mode[gate ? 0 : mult ? 1 : uncapped ? 2 : 3]++;
   if (me_rend)
      g_w_me++;
   if (g_o.vb - g_prev.vb >= (unsigned)g_every)
   {
      ffp_obs cur, d;
      unsigned i, f = fe_host_frame_count();
      snap(&cur);
      for (i = 0; i < FFP_NU; i++)
         ((unsigned *)&d)[i] = ((unsigned *)&cur)[i] - ((unsigned *)&g_prev)[i];
      g_win++;
      emit_obs("ffpace_win", &d, f - g_w_frame, now - g_w_t, g_w_loops, g_w_flips);
      emit_trace(0);
      g_prev = cur;
      g_w_t = now;
      g_w_frame = f;
      g_w_loops = g_w_flips = g_w_me = 0;
      memset(g_w_mode, 0, sizeof(g_w_mode));
      g_mr_n = g_mr_us = g_mr_max = g_ms_us = g_ms_max = g_ms_n1 = 0;
   }
}

void lab_ffpace_finish(void)
{
   ffp_obs cur, d;
   unsigned i, j;
   char m[400];
   int p = 0;
   if (!g_on)
      return;
   if (g_meas)
   {
      snap(&cur);
      for (i = 0; i < FFP_NU; i++)
         ((unsigned *)&d)[i] = ((unsigned *)&cur)[i] - ((unsigned *)&g_start)[i];
      emit_obs("ffpace_sum", &d, fe_host_frame_count() - g_f_start,
               (unsigned)sceKernelGetSystemTimeLow() - g_t_start, 0, g_flips_tot);
      for (i = 0; i < 4; i++)
      {
         p += snprintf(m + p, sizeof(m) - p, "%s", i ? " | " : "");
         for (j = 0; j < 9; j++)
            p += snprintf(m + p, sizeof(m) - p, "%s%u", j ? "," : "", d.m2[i][j]);
      }
      fe_evt("ffpace_m2 rows=ivl1,2,3,4+ cols=gf1..8,9+ m=%s", m);
      emit_trace(1);
   }
   g_meas = 0;
   g_run = 0;
   g_on = 0;
}
#endif
