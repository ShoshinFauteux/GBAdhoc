volatile int g_running = 1;
#ifdef GPSP_CATCH_SELFTEST
/* A variable so the compiler cannot prove the self-test store faults. */
volatile unsigned g_catch_selftest_addr = 0u;
#endif

/* STALL WATCH breadcrumbs (see stall_thread, harness `stall_watch_s`).  A
 * counter bumped at the top of every main-loop iteration and the region the
 * loop was last in; two plain stores per region, compiled out of a player
 * build.  1 top, 2 core (retro_run, incl. the video/audio callbacks), 3 after
 * the core, 4 pacing/swap, 5 menu, 7 me_rend_frame (inside 2). */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
static volatile unsigned g_loop_iter;
static volatile int      g_loop_phase;
#else
/* LOOP_TOP(), LOOP_PHASE(): this arm's definitions are in fe/fe_priv.h */
#endif
#ifdef GPSP_PERF_RIG
/* mgift_net.c / online_psp.c (NET_WAIT_BEAT, harness/lab builds only): a
 * bounded network wait counts as the main loop turning, so the harness stall
 * exit does not end a Wi-Fi association still inside its own timeout.  A
 * release has neither this function nor its call sites. */
void gpsp_net_wait_beat(void);
void gpsp_net_wait_beat(void)
{
   LOOP_TOP();
}

/* THE SYSTEM'S HOME, SIMULATED (harness `exit_sim_at`, PLAN HW-2 S-4): PPSSPP
 * has no HOME exit callback to press, so the named point of the quit's
 * teardown calls exit_cb itself, as cb_thread would with HOME pressed there.
 *   return  -- "Returning to game list", before the census (S-4 c)
 *   unload  -- after the in-process unload's heap_snap (S-4 d: F1 part 4)
 * Once per process.  A release has neither this nor its call sites. */
static int exit_cb(int arg1, int arg2, void *common);
static char g_exit_sim_at[12];
static void exit_sim(const char *where)
{
   if (!g_exit_sim_at[0] || strcmp(g_exit_sim_at, where))
      return;
   g_exit_sim_at[0] = '\0';
   fe_evt("exit_sim at=%s", where);
   exit_cb(0, 0, NULL);
}
#else
#define exit_sim(where) ((void)0)
#endif

