/* ---- THE SESSION (Epoch C C3, builds/epochc-out/PLAN-EPOCHC-beta5.md) -----------
 * session_reset: what every game starts from, set again before the in-process
 * return goes back to the game list (docs/INPROC-RETURN.md) -- the statements
 * main() ran there, in their order.  main()'s locals are passed by address
 * (always_inline: they stay registers of main()), the values the boot latched
 * by value.  The tools read the call as this body (exit_matrix.EPOCHC_HELPERS;
 * tests/extract.epochc_expand). */
static inline __attribute__((always_inline)) void session_reset(
   int *exit_code, const char **exit_reason, int *ff_engaged, int *ff_toggled,
   int *ff_last_mode, int *ff_last_me, unsigned *prev_pad, int *chord_frames,
   int *inproc_menu_armed, int *autoload_state_slot, int me_rend_cfg_boot,
   int sram_thread_boot, int autoload_boot)
{
   g_running = !g_exit_req;
   g_relaunch = 0;
   *exit_code = 0;
   *exit_reason = NULL;
   *ff_engaged = 0;
   *ff_toggled = 0;
   *ff_last_mode = -1;
   *ff_last_me = -1;
   *prev_pad = 0;
   g_pad_swallow = 0;          /* nothing of the last game is held */
   *chord_frames = 0;
   g_dump_chord_pending = 0;   /* no screenshot of the next game */
   ctl_turbo_stop(&g_turbo);   /* the game was left: nothing auto-fires */
   ui_set_turbo_locked(0);     /* ...and no session outlives it */
   *inproc_menu_armed = 0;
   g_ff_uncapped = g_ff_mult = g_ff_user = g_ff_gate = 0;
   g_blit_suppress = 0;
   g_drew = 0;
   memset(&g_ff_gate_st, 0, sizeof(g_ff_gate_st));
   g_wake_have = 0;
   g_wake_menu = 0;
   g_last_pix = NULL;
   g_me_rend_cfg = me_rend_cfg_boot;
   g_me_video_cfg = 0;
   g_me_video = 0;
   g_pcfg.sram_thread = sram_thread_boot;
   g_autoload_state = autoload_boot;
   *autoload_state_slot = 1;
   g_autoload_from_shelf = 0;
}

