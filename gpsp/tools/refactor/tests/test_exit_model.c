/* test_exit_model.c -- T8's host half (PLAN §3.6, gate XS).
 *
 * Compiles an exit/teardown implementation on the host with RECORDING STUBS for every
 * primitive it may call, plays every exit site's flags and every runtime scenario of the
 * loop teardown, and requires the recorded call order to equal what exit_matrix.py
 * extracted from `psp-gcc -E` of the real main_psp.c for the same flavour.
 *
 *   cc -std=gnu99 -DFLAVOUR_<REL|LAB|HAR> <flavour defines> \
 *      -DEXPECT_H='"<out>/<flavour>/exit_expect.h"' \
 *      -DTEARDOWN_INC='"<teardown implementation>"' tests/test_exit_model.c
 *
 * Flavour defines mirror the PSP build: release -DGPSP_PLAYABLE -DGPSP_NO_TELEMETRY;
 * lab -DGPSP_PERF_RIG -DGPSP_KEEP_TELEMETRY -DLAB_HARNESS; harness -DGPSP_PERF_RIG
 * -DGPSP_KEEP_TELEMETRY.  All three have -DREWIND (tests/abi-exit-ci_test_exit.sh).
 *
 * In phase T TEARDOWN_INC is tests/abi-exit-ci_teardown_ref.inc.c, the PLAN §3.2/§3.3
 * design written as code.  After C1 it is psp/teardown.inc.c (with a thin adapter if
 * its ctx signatures differ).  The implementation must define:
 *    void model_exit_early(unsigned flags);      families A-C, one call per site
 *    void model_session_end(void);               D0-D20
 *    int  model_inproc_try_return(void);         1 = back to the list (goto session_top)
 *    void model_process_end(void);               P1-P13 (recorded, never exits)
 * and read the runtime state below (g_relaunch, exit_reason, ...).
 *
 * Besides the call order it checks (Epoch C C0a-5):
 *   - the fe_evt FORMAT STRINGS of every loop-teardown scenario, in order, against the
 *     ones `psp-gcc -E` shows (EXP_SCEN[].evt): the Lab's parsers read them.  The early
 *     sites' texts are per site and are compared by `exit_matrix.py check` on the tree;
 *   - the power-hold balance: pwr_hold / pwr_release must be back at 0 when a scenario
 *     returns to the game list, and nothing may be held at a handoff or an exit;
 *   - g_running as the in-process return leaves it (1; 0 when HOME came during the
 *     return, HW-2 F1) and the HOME_* scenarios when the golden has them.
 * Exit status: 0 all equal, 1 any difference (each one printed).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* site flags (exit_matrix.py flags_of / order_of) */
enum {
   ORDER_NONE = 0, ORDER_AV_FIRST = 1, ORDER_EVT_FIRST = 2, ORDER_CORE_FIRST = 3,
   ORDER_MASK = 3,
   TD_PRE_EVT = 1u << 2, TD_LOADFIN = 1u << 3, TD_NOTICE = 1u << 4, TD_IO = 1u << 5,
   TD_CORE = 1u << 6, TD_REWIND = 1u << 7, PE_NO_VIDTERM = 1u << 8, PE_RELAUNCH = 1u << 9,
   PE_NO_HANDOFF = 1u << 10, PE_NO_EVT = 1u << 11, TD_ME = 1u << 12,
   PE_HO_BRANCHES = 1u << 13
};

#include EXPECT_H

/* ------------------------------------------------------------ recorder */
static char g_rec[16384];
static void rec(const char *s)
{
   size_t n = strlen(g_rec);
   if (n + strlen(s) + 2 >= sizeof(g_rec))
      return;
   if (n)
      g_rec[n++] = ' ';
   strcpy(g_rec + n, s);
}

/* ------------------------------------------------------------ runtime state */
#define RELAUNCH_PROFILE  1
#define RELAUNCH_GAMELIST 2
static int g_relaunch, exit_code, have_variant, inproc_n;
static volatile int g_running, g_exit_req;      /* HW-2 F1: the sticky HOME latch */
static int g_link_fatal_code;                   /* HW-2 F5 */
static const char *g_link_fatal_why;
static int g_hold, g_hold_bad;                  /* the pwr_hold ledger of this run */
static char g_evt[16384];                       /* fe_evt format strings, \x1f after each */
static const char *exit_reason;
static int sc_refusal[2], sc_refusal_n, sc_census_ok, sc_force, sc_retained;
static void *g_cable_reserve;
static int g_gba_cable;
typedef struct { unsigned a; } heap_snap_t;
static heap_snap_t snap_pre;

/* ------------------------------------------------------------ recording stubs */
#define STUB0(n)      static void n(void) { rec(#n); }
#define STUB1(n, T)   static void n(T a) { (void)a; rec(#n); }
STUB1(ui_set_wake_yield, void *)
STUB1(sleep_quiesce_up, const char *)
STUB1(play_lock_drop, const char *)
static void pwr_hold(const char *w) { (void)w; g_hold++; rec("pwr_hold"); }
static void ui_set_backdrop(const void *p, int w, int h) { (void)p; (void)w; (void)h; rec("ui_set_backdrop"); }
STUB0(ui_return_hold)
STUB0(home_host_stop)
static void me_rend_teardown(const char *w) { (void)w; rec("me_rend_teardown"); }
static void me_video_teardown(const char *w, int f) { (void)w; (void)f; rec("me_video_teardown"); }
static const char *inproc_refusal(int v)
{
   (void)v;
   rec("inproc_refusal");
   return sc_refusal[sc_refusal_n < 2 ? sc_refusal_n++ : 1] ? "refused" : NULL;
}
static int me_host_retain(unsigned us) { (void)us; rec("me_host_retain"); return 1; }
STUB0(me_host_shutdown)
static int me_host_loaded(void) { rec("me_host_loaded"); return 1; }
static int me_host_retained(void) { rec("me_host_retained"); return sc_retained; }
STUB0(ui_return_tick)
STUB0(net_teardown)
STUB1(mgift_stop, int)
STUB0(online_psp_down)
STUB0(emu_boost_stop)
STUB0(io_thread_stop)
STUB0(ui_settings_flush)
STUB1(handoff_set_extra, const char *)
STUB0(fe_host_shutdown)
static void psp_rom_pool_over(void) { rec("REWIND_GAME_OVER"); }
static void psp_rewind_pwr(void) { }
STUB0(dynarec_release_translation_caches)
STUB0(dynarec_heap_tables_release)
STUB0(me_rend_suspend)
STUB0(sceKernelDcacheWritebackInvalidateAll)
STUB0(sceKernelIcacheInvalidateAll)
STUB0(home_host_forget)
STUB0(fe_host_session_reset)
STUB0(osd_reset)
STUB0(ui_close)
STUB0(vid_ambient_drop)
STUB0(audio_stop)
STUB0(audio_ring_reset)
STUB0(audio_start)
STUB1(heap_snap, heap_snap_t *)
STUB1(heap_census, const char *)
STUB1(heap_trace_report, const char *)
static int heap_snap_same(const heap_snap_t *a, const heap_snap_t *b) { (void)a; (void)b; rec("heap_snap_same"); return sc_census_ok; }
static int fe_ini_get_int(const char *f, const char *k, int d) { (void)f; (void)k; (void)d; rec("fe_ini_get_int"); return sc_force; }
STUB1(psp_clock_apply, const char *)
STUB0(sleep_sim_fire)
STUB1(ctl_turbo_stop, void *)
STUB1(ui_set_turbo_locked, int)
STUB0(emu_boost_start)
static void pwr_release(void) { if (--g_hold < 0) { g_hold = 0; g_hold_bad++; } rec("pwr_release"); }
STUB0(vid_term)
STUB0(shash_close)
static void gpsp_rtc_peek(long long *a, long long *b, int *c) { *a = *b = 0; *c = 0; rec("gpsp_rtc_peek"); }
static void core_health_evt_fn(const char *a) { (void)a; rec("core_health_evt"); }
STUB0(evt_shutdown)
static void pwr_release_all(void) { g_hold = 0; rec("pwr_release_all"); }
static void handoff_run(int c, const char *w) { (void)c; (void)w; g_hold_bad += g_hold != 0; rec("handoff_run"); }
static void lab_finish(int c, const char *w) { (void)c; (void)w; g_hold_bad += g_hold != 0; rec("handoff_run"); }
static int lab_active(void) { return 0; }
static void relaunch_eboot(int gl) { (void)gl; rec("relaunch_eboot"); }
static void sceKernelExitGame_real(void) { g_hold_bad += g_hold != 0; rec("sceKernelExitGame"); }
static void ui_notice(const char *a, const char *b, const char *c, const char *d, int ms)
{ (void)a; (void)b; (void)c; (void)d; (void)ms; rec("ui_notice"); }
STUB1(ui_loading_finish, int)
static void fe_evt_fn(const char *fmt, ...)
{
   char w[64] = "fe_evt:";
   size_t k = strlen(w), n = strlen(g_evt);
   if (n + strlen(fmt) + 2 < sizeof(g_evt)) {      /* the whole format string, for the text check */
      strcpy(g_evt + n, fmt);
      strcat(g_evt + n, "\x1f");
   }
   while (*fmt && ((*fmt >= 'a' && *fmt <= 'z') || (*fmt >= 'A' && *fmt <= 'Z') || *fmt == '_') && k < 60)
      w[k++] = *fmt++;
   w[k] = 0;
   rec(w);
}

/* the frontend's macro layer, as main_psp.c / fe_evt.h / lab_harness.h define it */
#ifdef GPSP_NO_TELEMETRY
#define fe_evt(...) ((void)0)
#else
#define fe_evt fe_evt_fn
#endif
#ifdef GPSP_PERF_RIG
#define core_health_evt core_health_evt_fn
#else
#define core_health_evt(at) do { } while (0)
#define stall_watch_stop() do { } while (0)
#endif
#define drprof_stop(f) do { (void)(f); } while (0)        /* no DRPROF flavour here */
#ifdef GPSP_PERF_RIG
STUB0(stall_watch_stop)
#endif
#ifdef LAB_HARNESS
#define LAB_HANDOFF(code, why, real) (lab_active() ? lab_finish((code), (why)) : real((code), (why)))
#else
#define LAB_HANDOFF(code, why, real) real((code), (why))
#endif
#define FE_HANDOFF(code, why) (pwr_release_all(), LAB_HANDOFF((code), (why), handoff_run))
#define FE_EXIT_GAME()        (pwr_release_all(), sceKernelExitGame_real())
#define PSP_REWIND_GAME_OVER() (psp_rom_pool_over(), psp_rewind_pwr())
static int g_wake_have, g_wake_w, g_wake_h, g_ss_browser, g_ss_game;
static void *g_wake_frame;
static int g_turbo;

#include TEARDOWN_INC

/* ------------------------------------------------------------ comparison */
static const char *voc(const char *n)
{
   static const char *map[][2] = {
      {"io_thread_stop", "IO"}, {"fe_host_shutdown", "HOST"}, {"REWIND_GAME_OVER", "RW"},
      {"audio_stop", "AUD"}, {"vid_term", "VID"}, {"evt_shutdown", "ES"},
      {"handoff_run", "HO"}, {"pwr_release_all", "PRA"}, {"relaunch_eboot", "RL"},
      {"sceKernelExitGame", "EXIT"}, {"ui_notice", "NOTICE"}, {"ui_loading_finish", "LOADFIN"},
      {"me_rend_teardown", "ME"}, {"me_video_teardown", "ME"}, {"me_host_shutdown", "ME"},
      {"net_teardown", "NET"}, {"online_psp_down", "NET"} };
   size_t i;
   if (!strncmp(n, "fe_evt:", 7))
      return "EVT";
   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (!strcmp(n, map[i][0]))
         return map[i][1];
   return NULL;
}

/* recorded names -> site vocabulary, consecutive duplicates collapsed (as exit_matrix.py) */
static void to_voc(const char *in, char *out, size_t cap)
{
   char buf[16384], *save = NULL, *t;
   const char *last = "";
   out[0] = 0;
   snprintf(buf, sizeof(buf), "%s", in);
   for (t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
      const char *v = voc(t);
      if (!v || !strcmp(v, last))
         continue;
      if (out[0])
         strncat(out, " ", cap - strlen(out) - 1);
      strncat(out, v, cap - strlen(out) - 1);
      last = v;
   }
}

int main(void)
{
   int fails = 0, n = 0;
   size_t i;
   char got[16384];
   for (i = 0; i < sizeof(EXP_SITES) / sizeof(EXP_SITES[0]); i++) {
      g_rec[0] = 0;
      model_exit_early(EXP_SITES[i].flags);
      to_voc(g_rec, got, sizeof(got));
      n++;
      if (strcmp(got, EXP_SITES[i].seq)) {
         fails++;
         printf("  FAIL site %-4s\n     want: %s\n     got:  %s\n", EXP_SITES[i].id, EXP_SITES[i].seq, got);
      }
   }
   for (i = 0; i < sizeof(EXP_SCEN) / sizeof(EXP_SCEN[0]); i++) {
      const char *s = EXP_SCEN[i].name;
      g_rec[0] = 0;
      exit_code = 0;
      exit_reason = !strcmp(s, "XMB_REASON") ? "user_abort" : NULL;
      g_relaunch = !strncmp(s, "LIST", 4) ? RELAUNCH_GAMELIST : !strcmp(s, "PROFILE") ? RELAUNCH_PROFILE : 0;
      sc_refusal_n = 0;
      sc_refusal[0] = !strcmp(s, "LIST_REFUSED");
      sc_refusal[1] = !strcmp(s, "LIST_REFUSED") || !strcmp(s, "LIST_RETAIN_THEN_REFUSE");
      g_exit_req = !strncmp(s, "HOME_", 5);      /* HW-2 F1 (exit_matrix.py SCENARIOS_F1) */
      if (g_exit_req) {
         g_relaunch = !strcmp(s, "HOME_PROFILE") ? RELAUNCH_PROFILE : RELAUNCH_GAMELIST;
         sc_refusal[0] = !strcmp(s, "HOME_BEFORE_TEARDOWN");
         sc_refusal[1] = sc_refusal[0] || !strcmp(s, "HOME_BEFORE_REFUSAL2");
      }
      g_link_fatal_why = !strcmp(s, "LINK_FATAL") ? "gblink_reload" : NULL;   /* HW-2 F5 */
      g_link_fatal_code = g_link_fatal_why ? 4 : 0;
      g_running = 0;
      g_hold = g_hold_bad = 0;
      g_evt[0] = 0;
      sc_census_ok = strcmp(s, "LIST_CENSUS") && strcmp(s, "LIST_FORCE");
      sc_force = !strcmp(s, "LIST_FORCE");
      sc_retained = g_relaunch == RELAUNCH_GAMELIST && !sc_refusal[0];
      model_session_end();
      if (model_inproc_try_return()) {
         rec(g_running ? "g_running=1" : "g_running=0");
         rec("goto:session_top");
         g_hold_bad += g_hold != 0;     /* back in the list still holding the switch */
      } else
         model_process_end();
      n++;
      if (strcmp(g_rec, EXP_SCEN[i].seq)) {
         fails++;
         printf("  FAIL scenario %s\n     want: %s\n     got:  %s\n", s, EXP_SCEN[i].seq, g_rec);
      }
#if defined(EXPECT_HAS_EVT) && !defined(MODEL_NO_EVT_TEXT)
      else if (strcmp(g_evt, EXP_SCEN[i].evt)) {
         fails++;
         printf("  FAIL scenario %s: log text\n     want: %s\n     got:  %s\n", s, EXP_SCEN[i].evt, g_evt);
      }
#endif
      else if (g_hold_bad) {
         fails++;
         printf("  FAIL scenario %s: power hold not balanced (%d held at the end, %d bad points)\n",
                s, g_hold, g_hold_bad);
      }
   }
   printf("test_exit_model[%s]: %d/%d equal%s\n", EXPECT_FLAVOUR, n - fails, n, fails ? "  FAIL" : "");
   return fails ? 1 : 0;
}
