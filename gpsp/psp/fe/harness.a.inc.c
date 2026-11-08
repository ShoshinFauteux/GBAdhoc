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
#endif

