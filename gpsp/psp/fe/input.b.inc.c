#ifdef GPSP_PERF_RIG
static void jit_coh_frame(unsigned frames)
{
   static unsigned total, scans;
   u32 first = 0, n;
   if (!g_jit_coh_every || frames % g_jit_coh_every)
      return;
   n = jit_coherency_scan(&first);
   scans++;
   total += n;
   if (n || scans % 64 == 0)
      fe_evt("jit_coh f=%u n=%u first=%08x total=%u scans=%u",
             frames, (unsigned)n, (unsigned)first, total, scans);
}
#endif

/* ADR-0030: same boundary trick for the SMC write-ADDRESS profile.  The core
 * owns the buckets (main.c); this just forwards, and the take RESETS them, so
 * each `EVT smc_addr` describes exactly one heartbeat window.  A release core
 * has no profile (main.h SMC_PROFILE, code-health B-4): FE_SMC_PROFILE says the
 * same from the frontend's defines, and without it fe_host's smc_addr and
 * smc_block callbacks stay NULL (session.inc.c). */
#if !defined(GPSP_PROFILE_RELEASE) || defined(GPSP_SMC_PROFILE)
#define FE_SMC_PROFILE 1
void smc_prof_take(unsigned *hits, unsigned *pages, unsigned *iw, unsigned *ew,
                   unsigned *oth, unsigned *ovf,
                   unsigned *hot_addr, unsigned *hot_cnt,
                   unsigned *top_addr, unsigned *top_cnt, unsigned nhot);

static void plat_smc_addr(unsigned *hits, unsigned *pages, unsigned *iw,
                          unsigned *ew, unsigned *oth, unsigned *ovf,
                          unsigned *hot_addr, unsigned *hot_cnt,
                          unsigned *top_addr, unsigned *top_cnt, unsigned nhot)
{
   smc_prof_take(hits, pages, iw, ew, oth, ovf, hot_addr, hot_cnt,
                 top_addr, top_cnt, nhot);
}

/* Phase 5g: same boundary trick for the SMC BLOCK profile (main.c). */
void smc_blk_take(unsigned *watch, unsigned *xlat, unsigned *ovf,
                  unsigned *start, unsigned *end, unsigned *mode,
                  unsigned *reason, unsigned *cnt,
                  unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                  unsigned *wovf,
                  unsigned *head, unsigned *ctx, unsigned *snap,
                  unsigned *fus, unsigned *fbytes, unsigned *fwide,
                  unsigned *same, unsigned *diff, unsigned *unkn);

static void plat_smc_block(unsigned *watch, unsigned *xlat, unsigned *ovf,
                           unsigned *start, unsigned *end, unsigned *mode,
                           unsigned *reason, unsigned *cnt,
                           unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                           unsigned *wovf,
                           unsigned *head, unsigned *ctx, unsigned *snap,
                           unsigned *fus, unsigned *fbytes, unsigned *fwide,
                           unsigned *same, unsigned *diff, unsigned *unkn)
{
   smc_blk_take(watch, xlat, ovf, start, end, mode, reason, cnt,
                wpc, waddr, wcnt, wovf, head, ctx, snap,
                fus, fbytes, fwide, same, diff, unkn);
}
#endif

/* The core's flush timer wants a plain u32-microsecond clock (main.h).
 * Declared here rather than including the core's main.h, which pulls in the
 * whole emulator header set -- same pattern as smc_prof_take above. */
extern unsigned int (*smc_prof_clock)(void);

static unsigned int plat_smc_clock(void)
{
   return (unsigned int)sceKernelGetSystemTimeLow();
}

/* Phase 5h: same boundary trick again for the per-PHASE frame bracket.  The
 * core owns the accumulators and the probe level (main.c); this forwards the
 * take, which RESETS them, so each `EVT core_phase` describes exactly one
 * heartbeat window.  `core_phase_set_level` is called after the clock is
 * installed -- the core refuses any nonzero level without one. */
void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max,
                     unsigned *worst, unsigned nph,
                     unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman,
                     unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut);
void core_phase_set_level(unsigned lvl);

static void plat_core_phase(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                            unsigned *reads, unsigned *mean, unsigned *max,
                            unsigned *worst, unsigned nph,
                            unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                            unsigned *wbkw, unsigned *ugba, unsigned *dman,
                            unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut)
{
   core_phase_take(lvl, clk_ns, frames, reads, mean, max, worst, nph,
                   bkr, bkw, wbkr, wbkw, ugba, dman, stmr, negc, nego, rfux, rfut, wrfux, wrfut);
}

/* ------------------------------------------------------------------ input */

#ifdef GBADHOC_MGIFT
#include "mgift_shortcut.h"
#endif
static unsigned g_pad;   /* raw SceCtrl buttons, sampled once per loop */
#ifdef GBADHOC_MGIFT
static unsigned g_mgift_consumed;
#else
/* MGIFT=0 (psp/Makefile): no chord, nothing consumed. */
#define g_mgift_consumed 0u
#endif

/* AN ANSWER IS NOT INPUT.  The button that closes the START+SELECT menu
 * (Resume, a state load, Host/Join) or answers the SELECT+TRIANGLE pause
 * screen / wake overlay is still down when the game gets the pad back, and
 * the next pad read handed it to the core (X on Resume reached Pokemon
 * Crystal as B).  pad_swallow_arm() where the UI lets go makes every
 * button held at its answer invisible until that button is released --
 * to the core (GBA and GB/GBC alike: ctl_game_mask reads g_pad) and to the
 * shortcuts.  A button first pressed after the answer (while Host/Join or a
 * state load works, say) is the game's at once. */
/* @extract:begin menu:pad_swallow */
static unsigned g_pad_swallow;
static void pad_swallow_arm(unsigned held)
{
   g_pad_swallow |= held;        /* and what is still swallowed stays so */
}
static unsigned pad_swallow(unsigned pad)
{
   g_pad_swallow &= pad;         /* a released button is free again */
   return pad & ~g_pad_swallow;
}
/* @extract:end menu:pad_swallow */

#ifdef GPSP_PERF_RIG
/* HARNESS ONLY: RAW PAD REPLAY (`pad_script = <file>`).
 *
 * The autopilot injects GBA buttons AFTER the frontend's button mapping, so a
 * script can never exercise what a player's thumb does: the PSP pad itself,
 * the mapping table and every frontend shortcut read from g_pad (Triangle,
 * Square, the SELECT chords).  PPSSPP cannot press Square either (see
 * `simff`).  This ORs scripted PSP buttons into g_pad at the pad read, keyed
 * to EMULATED frames so the game sees them at the same frame every run.
 *
 * One press per line: `<frame> <hold-frames> <BUTTONS>`, BUTTONS '+'-joined
 * from SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE, or a
 * 0x mask.  '#' comments.  Not compiled into a release. */
#define PAD_SCRIPT_MAX 256
static struct { unsigned f, n, mask; } g_pad_script[PAD_SCRIPT_MAX];
static int g_pad_script_n;

/* Menu = HOME harness keys (docs/CONTROL-REMAP.md section 10).
 *   home_sim = 1          the menu behaves as if gbadhoc_home.prx were up
 *                         (PPSSPP cannot run it), and a HOME bit in the pad --
 *                         a pad_script line `<frame> 2 0x10000` -- is a press.
 *   home_stall_frame = N  at emulated frame N the main loop blocks for
 *   home_stall_ms = M     M ms (default 8000) WITHOUT beating: on hardware
 *                         HOME must go back to the system after ~2 s (the
 *                         heartbeat fallback), and come back to the menu ~1 s
 *                         after the loop resumes. */
static int      g_home_sim;
static unsigned g_home_stall_frame, g_home_stall_ms;

static unsigned pad_script_buttons(const char *t)
{
   static const struct { const char *n; unsigned b; } names[] = {
      { "SELECT", PSP_CTRL_SELECT }, { "START", PSP_CTRL_START },
      { "UP", PSP_CTRL_UP }, { "RIGHT", PSP_CTRL_RIGHT },
      { "DOWN", PSP_CTRL_DOWN }, { "LEFT", PSP_CTRL_LEFT },
      { "L", PSP_CTRL_LTRIGGER }, { "R", PSP_CTRL_RTRIGGER },
      { "TRIANGLE", PSP_CTRL_TRIANGLE }, { "CIRCLE", PSP_CTRL_CIRCLE },
      { "CROSS", PSP_CTRL_CROSS }, { "SQUARE", PSP_CTRL_SQUARE },
   };
   char buf[96], *tok, *save = NULL;
   unsigned m = 0, i;
   if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
      return (unsigned)strtoul(t, NULL, 16);
   snprintf(buf, sizeof(buf), "%s", t);
   for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save))
   {
      for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
         if (strcmp(tok, names[i].n) == 0)
            break;
      if (i == sizeof(names) / sizeof(names[0]))
         return 0;                 /* unknown name: drop the whole line */
      m |= names[i].b;
   }
   return m;
}

static void pad_script_load(const char *path)
{
   FILE *f = fopen(path, "r");
   char line[160];
   int bad = 0;
   if (!f)
   {
      fe_evt("pad_script err=open path=%s", path);
      return;
   }
   while (fgets(line, sizeof(line), f) && g_pad_script_n < PAD_SCRIPT_MAX)
   {
      unsigned fr, hold;
      char btn[96];
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
         continue;
      if (sscanf(line, "%u %u %95s", &fr, &hold, btn) != 3 ||
          !pad_script_buttons(btn) || !hold)
      {
         bad++;
         continue;
      }
      g_pad_script[g_pad_script_n].f = fr;
      g_pad_script[g_pad_script_n].n = hold;
      g_pad_script[g_pad_script_n].mask = pad_script_buttons(btn);
      g_pad_script_n++;
   }
   fclose(f);
   FE_EVT_ONLY(bad);
   fe_evt("pad_script n=%d bad=%d", g_pad_script_n, bad);
}

/* Scripted buttons held at emulated frame `f`; logs each press as it starts. */
static unsigned pad_script_mask(unsigned f)
{
   static unsigned last_f = ~0u;
   unsigned m = 0;
   int i;
   for (i = 0; i < g_pad_script_n; i++)
      if (f >= g_pad_script[i].f && f - g_pad_script[i].f < g_pad_script[i].n)
      {
         m |= g_pad_script[i].mask;
         if (f == g_pad_script[i].f && f != last_f)
            fe_evt("pad_script_press f=%u mask=0x%04x hold=%u", f,
                   g_pad_script[i].mask, g_pad_script[i].n);
      }
   last_f = f;
   return m;
}
#endif

/* ctl_map.h restates the SDK's button bits so it can be host-tested; hold it
 * to them. */
typedef char ctl_bits_match_sdk[
   (CTL_SELECT == PSP_CTRL_SELECT && CTL_START == PSP_CTRL_START &&
    CTL_UP == PSP_CTRL_UP && CTL_RIGHT == PSP_CTRL_RIGHT &&
    CTL_DOWN == PSP_CTRL_DOWN && CTL_LEFT == PSP_CTRL_LEFT &&
    CTL_L == PSP_CTRL_LTRIGGER && CTL_R == PSP_CTRL_RTRIGGER &&
    CTL_TRIANGLE == PSP_CTRL_TRIANGLE && CTL_CIRCLE == PSP_CTRL_CIRCLE &&
    CTL_CROSS == PSP_CTRL_CROSS && CTL_SQUARE == PSP_CTRL_SQUARE) ? 1 : -1];

/* Turbo buttons: which trains are running (ctl_map.h).  Stepped by
 * turbo_frame() once per loop iteration that runs the game; not part of the
 * emulated machine, never saved. */
static ctl_turbo g_turbo;

static uint32_t plat_input_bitmask(void)
{
   /* The player's mapping (Settings > Controls, psp/ctl_map.c): one table
    * lookup per game button -- the same ten tests the hard-coded chain did,
    * and with the default table the identical result (a 3.0 btn_swap
    * config arrives here as an explicit bind_a/bind_b pair).
    * Whatever the table says, this is the ONLY place a PSP button becomes a
    * GBA/GB button: the core, the GB/GBC core and the GB link lockstep all
    * read it through fe_host's input_bitmask, so a remap is local and what
    * crosses the network is still just the resulting button state.
    *
    * TURBO BUTTONS (docs/CONTROL-REMAP.md section 13) are added here and
    * nowhere else, so they too reach all three.  g_turbo.live is zero unless
    * a bound turbo button is firing right now: with none bound this is the
    * line above plus one flag test.  The pulse is a function of the emulated
    * frame number, so every read of one frame agrees and fast-forward's
    * three frames per pad sample each get their own phase. */
   uint32_t m = ctl_game_mask(&g_pcfg.controls, g_pad & ~g_mgift_consumed);
   if (g_turbo.live)
      m |= ctl_turbo_mask(&g_pcfg.controls, &g_turbo, fe_host_frame_count());
   return m;
}

